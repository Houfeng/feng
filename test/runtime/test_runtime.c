/* Phase 1A runtime regression suite. Each test exercises a single subsystem
 * (refcount, string, array, exception). Tests deliberately avoid relying on
 * malloc-tracking instrumentation — they assert observable behaviour and
 * cross-check finalizer counts via test-local descriptors. */
#include "runtime/feng_runtime.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(expr)                                                                  \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

/* --- Test object with finalizer counter -------------------------------- */

typedef struct TestObject {
    FengManagedHeader header;
    int payload;
} TestObject;

static int g_finalize_count = 0;

static void test_object_finalize(void *self) {
    (void)self;
    ++g_finalize_count;
}

static const FengTypeDescriptor test_object_descriptor = {
    .name = "test.TestObject",
    .size = sizeof(TestObject),
    .finalizer = test_object_finalize,
};

/* --- Tests ------------------------------------------------------------- */

static void test_object_retain_release(void) {
    TestObject *obj;

    g_finalize_count = 0;
    obj = (TestObject *)feng_object_new(&test_object_descriptor);
    ASSERT(obj != NULL);
    ASSERT(obj->header.refcount == 1U);
    ASSERT(obj->header.tag == FENG_TYPE_TAG_OBJECT);
    obj->payload = 42;

    feng_retain(obj);
    ASSERT(obj->header.refcount == 2U);

    feng_release(obj);
    ASSERT(obj->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    feng_release(obj);
    ASSERT(g_finalize_count == 1);
}

static void test_retain_release_nullsafe(void) {
    ASSERT(feng_retain(NULL) == NULL);
    feng_release(NULL); /* must not crash */
}

static void test_assign_barrier(void) {
    TestObject *a;
    TestObject *b;
    void *slot = NULL;

    g_finalize_count = 0;
    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_assign(&slot, a);
    ASSERT(slot == a);
    ASSERT(a->header.refcount == 2U);

    feng_assign(&slot, b);
    ASSERT(slot == b);
    ASSERT(a->header.refcount == 1U);
    ASSERT(b->header.refcount == 2U);

    feng_assign(&slot, NULL);
    ASSERT(slot == NULL);
    ASSERT(b->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    feng_release(a);
    feng_release(b);
    ASSERT(g_finalize_count == 2);
}

static void test_take(void) {
    TestObject *a;
    void *slot;

    g_finalize_count = 0;
    a = (TestObject *)feng_object_new(&test_object_descriptor);
    slot = NULL;
    feng_assign(&slot, a);
    ASSERT(a->header.refcount == 2U);

    {
        void *taken = feng_take(&slot);

        ASSERT(taken == a);
        ASSERT(slot == NULL);
        /* refcount unchanged: take transfers the +1 to the caller. */
        ASSERT(a->header.refcount == 2U);

        feng_release(taken);
    }
    ASSERT(a->header.refcount == 1U);
    feng_release(a);
    ASSERT(g_finalize_count == 1);
}

static void test_string_literal_immortal(void) {
    FengString *a = feng_string_literal("hello", 5);
    FengString *b = feng_string_literal("", 0);

    ASSERT(feng_string_length(a) == 5U);
    ASSERT(memcmp(feng_string_data(a), "hello", 5) == 0);
    ASSERT(feng_string_data(a)[5] == '\0');

    ASSERT(feng_string_length(b) == 0U);
    ASSERT(feng_string_data(b)[0] == '\0');

    /* Literal refcount must remain immortal across retain/release cycles. */
    feng_retain(a);
    feng_release(a);
    feng_release(a);
    ASSERT(feng_string_length(a) == 5U);
}

static void test_string_concat(void) {
    FengString *hello = feng_string_literal("hello, ", 7);
    FengString *world = feng_string_literal("world", 5);
    FengString *combined = feng_string_concat(hello, world);

    ASSERT(feng_string_length(combined) == 12U);
    ASSERT(memcmp(feng_string_data(combined), "hello, world", 12) == 0);
    ASSERT(feng_string_data(combined)[12] == '\0');
    feng_release(combined);

    /* NULL operands behave as the empty string. */
    combined = feng_string_concat(NULL, world);
    ASSERT(feng_string_length(combined) == 5U);
    feng_release(combined);

    combined = feng_string_concat(hello, NULL);
    ASSERT(feng_string_length(combined) == 7U);
    feng_release(combined);

    combined = feng_string_concat(NULL, NULL);
    ASSERT(feng_string_length(combined) == 0U);
    feng_release(combined);
}

static const FengTypeDescriptor i32_element_descriptor = {
    .name = "i32",
    .size = sizeof(int32_t),
    .finalizer = NULL,
};

static void test_array_primitive(void) {
    FengArray *array = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 4U);
    int32_t *items;
    size_t i;

    ASSERT(feng_array_length(array) == 4U);
    items = (int32_t *)feng_array_data(array);
    ASSERT(items != NULL);
    for (i = 0U; i < 4U; ++i) {
        ASSERT(items[i] == 0);
    }
    items[0] = 10;
    items[3] = 30;
    ASSERT(items[0] == 10);
    ASSERT(items[3] == 30);

    feng_array_check_index(array, 0U);
    feng_array_check_index(array, 3U);

    feng_release(array);
}

static void test_array_managed_releases_elements(void) {
    FengArray *array;
    void **slots;
    TestObject *a;
    TestObject *b;

    g_finalize_count = 0;
    array = feng_array_new(&test_object_descriptor, sizeof(void *), true, 2U);
    slots = (void **)feng_array_data(array);

    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);
    feng_assign(&slots[0], a);
    feng_assign(&slots[1], b);
    feng_release(a);
    feng_release(b);
    /* Now only the array holds a reference; finalize_count is still 0. */
    ASSERT(g_finalize_count == 0);

    feng_release(array);
    ASSERT(g_finalize_count == 2);
}

static void test_array_zero_length(void) {
    FengArray *array = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 0U);

    ASSERT(feng_array_length(array) == 0U);
    ASSERT(feng_array_data(array) == NULL);
    feng_release(array);
}

static void test_exception_caught(void) {
    FengExceptionFrame frame;
    int caught = 0;
    void *value = NULL;

    feng_exception_push(&frame);
    if (setjmp(frame.jb) == 0) {
        feng_exception_throw((void *)(uintptr_t)0x1234, 0);
        ASSERT(0);
    } else {
        caught = 1;
        value = frame.value;
        feng_exception_pop();
    }
    ASSERT(caught == 1);
    ASSERT(value == (void *)(uintptr_t)0x1234);
    ASSERT(feng_exception_current() == NULL);
}

static void test_exception_managed_value_caught(void) {
    FengExceptionFrame frame;
    TestObject *thrown;

    g_finalize_count = 0;
    thrown = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_exception_push(&frame);
    if (setjmp(frame.jb) == 0) {
        feng_exception_throw(thrown, 1);
        ASSERT(0);
    } else {
        ASSERT(frame.value == thrown);
        ASSERT(frame.is_managed == 1);
        feng_exception_pop();
        feng_release(thrown);
    }
    ASSERT(g_finalize_count == 1);
}

static void test_exception_nested_propagation(void) {
    FengExceptionFrame outer;
    FengExceptionFrame inner;
    int outer_caught = 0;

    feng_exception_push(&outer);
    if (setjmp(outer.jb) == 0) {
        feng_exception_push(&inner);
        if (setjmp(inner.jb) == 0) {
            feng_exception_throw((void *)(uintptr_t)0xABCD, 0);
            ASSERT(0);
        } else {
            void *value = inner.value;

            /* Inner caught — pop and rethrow up to outer. */
            feng_exception_pop();
            feng_exception_throw(value, 0);
            ASSERT(0);
        }
    } else {
        outer_caught = 1;
        ASSERT(outer.value == (void *)(uintptr_t)0xABCD);
        feng_exception_pop();
    }
    ASSERT(outer_caught == 1);
    ASSERT(feng_exception_current() == NULL);
}

/* --- Finalizer resurrection (Phase 1B-1) ------------------------------- */

/* Test fixture: a descriptor whose finalizer republishes `self` into a global
 * slot, simulating user code that accidentally (or intentionally) revives the
 * object from within its own finalizer. */
static void *g_resurrect_slot = NULL;
static int   g_resurrect_calls = 0;
static int   g_resurrect_remaining = 0; /* how many more times to resurrect */

static void resurrect_finalizer(void *self) {
    ++g_resurrect_calls;
    if (g_resurrect_remaining > 0) {
        --g_resurrect_remaining;
        /* Republish self by retaining; the slot is a long-lived global. */
        g_resurrect_slot = feng_retain(self);
    }
}

static const FengTypeDescriptor resurrect_descriptor = {
    .name = "test.Resurrect",
    .size = sizeof(TestObject),
    .finalizer = resurrect_finalizer,
};

static void test_finalizer_resurrection_then_release(void) {
    TestObject *obj;

    g_resurrect_slot = NULL;
    g_resurrect_calls = 0;
    g_resurrect_remaining = 1;

    obj = (TestObject *)feng_object_new(&resurrect_descriptor);
    ASSERT(obj->header.refcount == 1U);

    /* Drop last reference; finalizer fires, republishes self -> resurrected. */
    feng_release(obj);
    ASSERT(g_resurrect_calls == 1);
    ASSERT(g_resurrect_slot == obj);
    ASSERT(obj->header.refcount == 1U);

    /* Drop the resurrected reference; this time finalizer does NOT resurrect,
     * so the object must actually be freed and the finalizer must run again. */
    g_resurrect_slot = NULL;
    feng_release(obj);
    ASSERT(g_resurrect_calls == 2);
}

static void test_finalizer_resurrection_reruns_on_next_release(void) {
    TestObject *obj;
    int i;

    g_resurrect_slot = NULL;
    g_resurrect_calls = 0;
    g_resurrect_remaining = 3; /* resurrect three times */

    obj = (TestObject *)feng_object_new(&resurrect_descriptor);

    /* Each release should trigger another finalizer run while resurrections
     * remain; once they run out the object is finally freed. */
    for (i = 0; i < 3; ++i) {
        feng_release(obj);
        ASSERT(g_resurrect_calls == i + 1);
        ASSERT(g_resurrect_slot == obj);
        ASSERT(obj->header.refcount == 1U);
        g_resurrect_slot = NULL;
    }

    /* 4th release: g_resurrect_remaining == 0, no resurrection -> free. */
    feng_release(obj);
    ASSERT(g_resurrect_calls == 4);
}

static void test_finalizer_no_resurrection_releases(void) {
    TestObject *obj;

    /* Reuses test_object_descriptor which has a plain non-resurrecting
     * finalizer; verifies the new re-check path still frees in the common
     * case. */
    g_finalize_count = 0;
    obj = (TestObject *)feng_object_new(&test_object_descriptor);
    feng_release(obj);
    ASSERT(g_finalize_count == 1);
}

int main(void) {
    test_object_retain_release();
    test_retain_release_nullsafe();
    test_assign_barrier();
    test_take();
    test_string_literal_immortal();
    test_string_concat();
    test_array_primitive();
    test_array_managed_releases_elements();
    test_array_zero_length();
    test_exception_caught();
    test_exception_managed_value_caught();
    test_exception_nested_propagation();
    test_finalizer_resurrection_then_release();
    test_finalizer_resurrection_reruns_on_next_release();
    test_finalizer_no_resurrection_releases();

    fputs("test_runtime: ok\n", stdout);
    return 0;
}
