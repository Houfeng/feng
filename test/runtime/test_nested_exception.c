/* Exercise the compiler/runtime catch protocol through native unwinding.
 * These tests inspect record identity and reference counts, which are not
 * language-level observations available to the complementary FCTS suite. */
#include "runtime/feng_runtime.h"

#if !defined(_WIN32)
#include <llvm_c_eh.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

/* Exception subject with a per-thread observable finalization event. */
typedef struct NestedPayload {
    FengManagedHeader header;
    unsigned char code;
} NestedPayload;

static _Thread_local int nested_finalizations;
static _Thread_local int nested_event_fd = -1;

/* Count finalization and optionally report it from a terminating subprocess. */
static void nested_finalize(void *value) {
    NestedPayload *payload = (NestedPayload *)value;
    ++nested_finalizations;
    if (nested_event_fd >= 0) {
        CHECK(write(nested_event_fd, &payload->code, 1U) == 1);
    }
}

static const FengTypeDescriptor nested_descriptor = {
    .name = "test.NestedExceptionPayload",
    .size = sizeof(NestedPayload),
    .finalizer = nested_finalize
};

/* Allocate the exact +1 reference transferred to feng_throw. */
static NestedPayload *nested_payload(unsigned char code) {
    NestedPayload *payload = feng_object_new(&nested_descriptor);
    payload->code = code;
    return payload;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"

/* Use the same native EH protocol as generated code, including the logical
 * function cleanup when a handler rethrows or raises a replacement. */
static void nested_native_try(void (*body)(void *), void *argument,
                              void (*handler)(FengCatchContext *, void *),
                              void *state) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, __feng_personality_v0);
    FengFrameMarker function_frame;
    FengCatchContext context;
    if (__llvm_c_eh_region(1u, 0u, &&cleanup, 0u)) goto cleanup;
    if (__llvm_c_eh_region(2u, 1u, &&landing, 1u, (const void *)0)) goto landing;
    feng_frame_push(&function_frame);
    __llvm_c_eh_activate(1u);
    feng_try_frame_push(&context.frame);
    __llvm_c_eh_activate(2u);
    body(argument);
    feng_frame_pop();
    __llvm_c_eh_activate(1u);
    goto done;
landing:
    __llvm_c_eh_activate(1u);
    if (__llvm_c_eh_selector(2u) != __llvm_c_eh_typeid(NULL)) {
        feng_frame_release_to(&context.frame);
        __llvm_c_eh_propagate(2u);
    }
    context.exception = __llvm_c_eh_exception(2u);
    context.exception->matched_clause = 0;
    feng_exception_catch_begin(&context);
    CHECK(feng_caught_clause() == 0);
    handler(&context, state);
    feng_exception_catch_end();
done:
    feng_frame_pop();
    __llvm_c_eh_activate(0u);
    return;
cleanup:
    __llvm_c_eh_activate(0u);
    feng_frame_release_to(&function_frame);
    __llvm_c_eh_propagate(1u);
}

#pragma clang diagnostic pop

/* Transfer an already-owned subject without an additional retain. */
static void nested_raise(void *argument) {
    feng_throw(argument, &nested_descriptor);
}

/* Keep observations outside the lexical catch that owns each record. */
typedef struct NestedRecordState {
    FengCatchContext *outer;
    FengUnwindException *original;
    NestedPayload *subject;
    bool rethrow;
    bool received;
} NestedRecordState;

/* Check independent inner ownership and the link to the still-live caller. */
static void nested_inner_handler(FengCatchContext *context, void *argument) {
    NestedRecordState *state = argument;
    NestedPayload *inner = feng_caught_value();
    CHECK(context->previous == state->outer);
    CHECK(context->exception != state->original);
    CHECK(inner->code == 'B' && inner->header.refcount == 1U);
    CHECK(state->subject->header.refcount == 1U);
    CHECK(nested_finalizations == 0);
}

/* Handle a nested record, restore this original, and optionally transfer it. */
static void nested_outer_handler(FengCatchContext *context, void *argument) {
    NestedRecordState *state = argument;
    state->outer = context;
    state->original = context->exception;
    state->subject = feng_caught_value();
    CHECK(state->subject->code == 'A' && state->subject->header.refcount == 1U);
    nested_native_try(nested_raise, nested_payload('B'), nested_inner_handler, state);
    CHECK(nested_finalizations == 1);
    CHECK(context->exception == state->original);
    CHECK(feng_caught_value() == state->subject);
    CHECK(state->subject->header.refcount == 1U);
    CHECK(feng_caught_clause() == 0);
    if (state->rethrow) feng_rethrow();
}

/* Run one original catch that may rethrow into its caller. */
static void nested_original_body(void *argument) {
    nested_native_try(nested_raise, nested_payload('A'), nested_outer_handler, argument);
}

/* Verify rethrow kept the original record, subject, descriptor and +1 hold. */
static void nested_rethrow_handler(FengCatchContext *context, void *argument) {
    NestedRecordState *state = argument;
    CHECK(context->exception == state->original);
    CHECK(context->exception->desc == &nested_descriptor);
    CHECK(feng_caught_value() == state->subject);
    CHECK(state->subject->header.refcount == 1U);
    CHECK(context->previous == NULL);
    CHECK(nested_finalizations == 1);
    state->received = true;
}

/* Verify both normal catch completion and original-record rethrow. */
static void nested_record_identity(bool rethrow) {
    NestedRecordState state = {0};
    state.rethrow = rethrow;
    nested_finalizations = 0;
    CHECK(feng_caught_value() == NULL && feng_caught_clause() == -1);
    if (rethrow) {
        nested_native_try(nested_original_body, &state, nested_rethrow_handler, &state);
        CHECK(state.received);
    } else {
        nested_original_body(&state);
    }
    CHECK(nested_finalizations == 2);
    CHECK(feng_caught_value() == NULL && feng_caught_clause() == -1);
}

/* Each thread repeatedly exercises its own active catch and cleanup chain. */
static void *nested_thread_worker(void *argument) {
    (void)argument;
    for (unsigned int i = 0U; i < 64U; ++i) {
        nested_record_identity((i & 1U) != 0U);
    }
    return NULL;
}

/* Raise a replacement without an outer handler, testing terminal cleanup. */
static void nested_uncaught_handler(FengCatchContext *context, void *argument) {
    (void)context;
    if (argument != NULL) feng_rethrow();
    feng_throw(nested_payload('B'), &nested_descriptor);
}

/* Uncaught throw/rethrow must clean all live catches before aborting. */
static void nested_uncaught_cleanup(bool rethrow) {
    int events[2];
    int status;
    unsigned char observed[4] = {0};
    size_t count = 0U;
    CHECK(pipe(events) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(events[0]);
        nested_event_fd = events[1];
        CHECK(freopen("/dev/null", "w", stderr) != NULL);
        nested_native_try(nested_raise, nested_payload('A'), nested_uncaught_handler,
                          rethrow ? &status : NULL);
        _exit(91);
    }
    close(events[1]);
    for (;;) {
        ssize_t got = read(events[0], observed + count, sizeof(observed) - count);
        CHECK(got >= 0);
        if (got == 0) break;
        count += (size_t)got;
        CHECK(count < sizeof(observed));
    }
    close(events[0]);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CHECK(count == (rethrow ? 1U : 2U));
    CHECK(observed[0] == 'A');
    if (!rethrow) CHECK(observed[1] == 'B');
}
#endif

/* Register native runtime checks alongside the existing runtime suite. */
void test_nested_exception_runtime(void) {
#if !defined(_WIN32)
    pthread_t threads[4];
    nested_record_identity(false);
    nested_record_identity(true);
    nested_uncaught_cleanup(false);
    nested_uncaught_cleanup(true);
    for (size_t i = 0U; i < sizeof(threads) / sizeof(threads[0]); ++i) {
        CHECK(pthread_create(&threads[i], NULL, nested_thread_worker, NULL) == 0);
    }
    for (size_t i = 0U; i < sizeof(threads) / sizeof(threads[0]); ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    puts("nested exception runtime ownership, identity and TLS tests passed");
#endif
}
