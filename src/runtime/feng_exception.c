#include "runtime/feng_runtime.h"
#include "runtime/feng_exception_lsda.h"

#include <stddef.h>
#include <stdlib.h>

static _Thread_local FengCleanupNode *g_cleanup_top = NULL;
static _Thread_local FengCatchContext *g_catch_top = NULL;
static _Thread_local size_t g_finalizer_depth = 0U;

#if !defined(_WIN32)
const FengLSDA feng_empty_function_lsda[1] = {{0}};
#endif

/* Execute an already detached cleanup node exactly once. */
static void feng_cleanup_release_node(FengCleanupNode *node) {
    void **slot;

    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case FENG_NODE_DEFER:
        if (node->defer_fn != NULL) {
            node->defer_fn(node->defer_closure);
        }
        return;
    case FENG_NODE_RELEASE:
        if (node->aggregate_desc != NULL) {
            if (node->aggregate_value != NULL) {
                feng_aggregate_release(node->aggregate_value, node->aggregate_desc);
                node->aggregate_value = NULL;
            }
            return;
        }
        slot = node->slot;
        if (slot != NULL && *slot != NULL) {
            feng_release(*slot);
            *slot = NULL;
        }
        return;
    case FENG_NODE_MARKER:
        /* Generated cleanup entries own the logical frame boundary. */
        return;
    }
}

/* Drain live resources before terminating for an uncaught Feng exception. */
static void feng_cleanup_release_all(void) {
    while (g_cleanup_top != NULL) {
        FengCleanupNode *node = g_cleanup_top;
        g_cleanup_top = node->prev;
        feng_cleanup_release_node(node);
    }
}

/* Destroy one exception owner after it has been detached from its context. */
static void feng_unwind_exception_destroy(FengUnwindException *exception) {
    if (exception == NULL) {
        return;
    }
    if (exception->value != NULL) {
        feng_release(exception->value);
        exception->value = NULL;
    }
    free(exception);
}

#if !defined(_WIN32)
/* Platform cleanup releases the same record and payload as catch exit. */
static void feng_unwind_exception_cleanup(_Unwind_Reason_Code reason,
                                          struct _Unwind_Exception *unwind) {
    FengUnwindException *exception;

    (void)reason;
    if (unwind == NULL) {
        return;
    }
    exception = (FengUnwindException *)((char *)unwind - offsetof(FengUnwindException, unwind));
    feng_unwind_exception_destroy(exception);
}
#endif

/* Initialize the platform record without changing any active catch owner. */
static void feng_unwind_exception_init(FengUnwindException *exception,
                                       void *value,
                                       const FengTypeDescriptor *desc) {
#if defined(_WIN32)
    exception->exception_class = FENG_EXCEPTION_CLASS;
#else
    exception->unwind.exception_class = FENG_EXCEPTION_CLASS;
    exception->unwind.exception_cleanup = feng_unwind_exception_cleanup;
#endif
    exception->value = value;
    exception->desc = desc;
    exception->matched_clause = -1;
}

/* Finish a detached catch node. During landing-pad cleanup the node already
 * owns its exception but has not yet joined the active catch stack. */
static void feng_exception_catch_cleanup(void *closure) {
    FengCatchContext *context = (FengCatchContext *)closure;
    FengUnwindException *exception = context->exception;

    if (g_catch_top == context) {
        g_catch_top = context->previous;
    }
    context->exception = NULL;
    context->previous = NULL;
    feng_unwind_exception_destroy(exception);
}

/* Preserve the legacy symbol, but never silently mix incompatible old objects
 * with the native-EH runtime. New generated code has no registration calls. */
void feng_register_lsda(const FengLSDA *regions, int region_count) {
    if (regions == NULL || region_count <= 0) {
        return;
    }
    feng_panic("legacy exception metadata: rebuild Feng runtime and all packages");
}

/* Track a managed-pointer local on the current thread's cleanup chain. */
void feng_cleanup_push(FengCleanupNode *node, void **slot) {
    if (node == NULL) {
        feng_panic("feng_cleanup_push: NULL node");
    }
    if (slot == NULL) {
        feng_panic("feng_cleanup_push: NULL slot");
    }
    node->kind = FENG_NODE_RELEASE;
    node->slot = slot;
    node->aggregate_value = NULL;
    node->aggregate_desc = NULL;
    node->defer_fn = NULL;
    node->defer_closure = NULL;
    node->prev = g_cleanup_top;
    g_cleanup_top = node;
}

/* Track descriptor-driven aggregate cleanup on the same LIFO chain. */
void feng_cleanup_push_aggregate(FengCleanupNode *node,
                                 void *value,
                                 const FengAggregateDescriptor *desc) {
    if (node == NULL) {
        feng_panic("feng_cleanup_push_aggregate: NULL node");
    }
    if (value == NULL) {
        feng_panic("feng_cleanup_push_aggregate: NULL value");
    }
    if (desc == NULL) {
        feng_panic("feng_cleanup_push_aggregate: NULL descriptor");
    }
    node->kind = FENG_NODE_RELEASE;
    node->slot = NULL;
    node->aggregate_value = value;
    node->aggregate_desc = desc;
    node->defer_fn = NULL;
    node->defer_closure = NULL;
    node->prev = g_cleanup_top;
    g_cleanup_top = node;
}

/* Register a stack-backed defer callback without taking closure ownership. */
void feng_defer_push(FengCleanupNode *node,
                     void (*fn)(void *),
                     void *closure) {
    if (node == NULL) {
        feng_panic("feng_defer_push: NULL node");
    }
    if (fn == NULL) {
        feng_panic("feng_defer_push: NULL fn");
    }
    node->kind = FENG_NODE_DEFER;
    node->slot = NULL;
    node->aggregate_value = NULL;
    node->aggregate_desc = NULL;
    node->defer_fn = fn;
    node->defer_closure = closure;
    node->prev = g_cleanup_top;
    g_cleanup_top = node;
}

/* Detach a node whose cleanup is emitted explicitly by generated code. */
void feng_cleanup_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_cleanup_pop: chain underflow");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

/* Mark the boundary of one generated function's resources. */
void feng_frame_push(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_frame_push: NULL marker");
    }
    marker->node.kind = FENG_NODE_MARKER;
    marker->node.slot = NULL;
    marker->node.aggregate_value = NULL;
    marker->node.aggregate_desc = NULL;
    marker->node.defer_fn = NULL;
    marker->node.defer_closure = NULL;
    marker->node.prev = g_cleanup_top;
    marker->is_function_boundary = true;
    g_cleanup_top = &marker->node;
}

/* Mark a try expression without initializing any later catch state. */
void feng_try_frame_push(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_try_frame_push: NULL marker");
    }
    marker->node.kind = FENG_NODE_MARKER;
    marker->node.slot = NULL;
    marker->node.aggregate_value = NULL;
    marker->node.aggregate_desc = NULL;
    marker->node.defer_fn = NULL;
    marker->node.defer_closure = NULL;
    marker->node.prev = g_cleanup_top;
    marker->is_function_boundary = false;
    g_cleanup_top = &marker->node;
}

/* Remove a normally exited function/try marker after its resource cleanup. */
void feng_frame_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_frame_pop: chain underflow");
    }
    if (g_cleanup_top->kind != FENG_NODE_MARKER) {
        feng_panic("feng_frame_pop: top cleanup node is not a frame marker");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

/* Release newer nodes while leaving the requested boundary on the chain. */
static void feng_cleanup_release_above(FengCleanupNode *boundary) {
    while (g_cleanup_top != NULL && g_cleanup_top != boundary) {
        FengCleanupNode *node = g_cleanup_top;
        g_cleanup_top = node->prev;
        feng_cleanup_release_node(node);
    }
    if (g_cleanup_top != boundary) {
        feng_panic("feng cleanup: boundary is not on cleanup chain");
    }
}

/* Release a protected region and remove its now-empty marker. */
void feng_frame_release_to(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_frame_release_to: NULL marker");
    }
    feng_cleanup_release_above(&marker->node);
    g_cleanup_top = g_cleanup_top->prev;
}

/* Protect the delivered record before running any potentially reentrant
 * try cleanup. Reusing the marker as its owner also releases that record if
 * a cleanup callback escapes instead of returning to this landing pad. */
void feng_exception_catch_begin(FengCatchContext *context) {
    FengCleanupNode *node;

    if (context == NULL || context->exception == NULL) {
        feng_panic("feng_exception_catch_begin: missing context or exception");
    }
    node = &context->frame.node;
    if (node->kind != FENG_NODE_MARKER || context->frame.is_function_boundary) {
        feng_panic("feng_exception_catch_begin: context is not a try marker");
    }
    context->previous = NULL;
    node->kind = FENG_NODE_DEFER;
    node->defer_fn = feng_exception_catch_cleanup;
    node->defer_closure = context;
    feng_cleanup_release_above(node);
    context->previous = g_catch_top;
    g_catch_top = context;
}

/* End one active catch after its younger cleanup nodes have been removed. */
void feng_exception_catch_end(void) {
    FengCatchContext *context = g_catch_top;

    if (context == NULL || g_cleanup_top != &context->frame.node) {
        feng_panic("feng_exception_catch_end: catch is not the cleanup top");
    }
    g_cleanup_top = context->frame.node.prev;
    feng_exception_catch_cleanup(context);
}

/* Enter the runtime's existing finalizer exception barrier. */
void feng_exception_enter_finalizer(void) {
    g_finalizer_depth++;
}

/* Leave the finalizer barrier after normal completion. */
void feng_exception_leave_finalizer(void) {
    if (g_finalizer_depth == 0U) {
        feng_panic("feng_exception_leave_finalizer: depth underflow");
    }
    g_finalizer_depth--;
}

#if !defined(_WIN32)
/* Native tables describe physical frames after inlining; their landing pads
 * perform Feng's logical-scope cleanup. Search is free of ownership effects. */
_Unwind_Reason_Code __feng_personality_v0(int version,
                                          _Unwind_Action actions,
                                          uint64_t exception_class,
                                          struct _Unwind_Exception *unwind,
                                          struct _Unwind_Context *context) {
    if (version != 1 || unwind == NULL || context == NULL ||
        exception_class != FENG_EXCEPTION_CLASS) {
        return _URC_CONTINUE_UNWIND;
    }
    const FengUnwindException *exception = (const FengUnwindException *)(
        (const char *)unwind - offsetof(FengUnwindException, unwind));
    int ip_before_instruction = 0;
    uintptr_t ip = (uintptr_t)_Unwind_GetIPInfo(context, &ip_before_instruction);
    if (!ip_before_instruction && ip != 0U) --ip;
    FengExceptionLanding landing;
    if (!feng_exception_lsda_find((const unsigned char *)_Unwind_GetLanguageSpecificData(context), NULL,
                                  (uintptr_t)_Unwind_GetRegionStart(context), ip,
                                  exception->desc, !(actions & _UA_FORCE_UNWIND),
                                  &landing)) {
        return (actions & _UA_SEARCH_PHASE) ? _URC_FATAL_PHASE1_ERROR : _URC_FATAL_PHASE2_ERROR;
    }
    if (actions & _UA_SEARCH_PHASE)
        return landing.selector != 0 ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
    if ((actions & _UA_CLEANUP_PHASE) && landing.address != 0U &&
        (landing.cleanup || landing.selector != 0)) {
        intptr_t selector = (actions & _UA_HANDLER_FRAME) ? landing.selector : 0;
        _Unwind_SetGR(context, __builtin_eh_return_data_regno(0), (uintptr_t)unwind);
        _Unwind_SetGR(context, __builtin_eh_return_data_regno(1), (uintptr_t)selector);
        _Unwind_SetIP(context, landing.address);
        return _URC_INSTALL_CONTEXT;
    }
    return _URC_CONTINUE_UNWIND;
}

#endif

/* Transfer an owned payload into a new record, leaving active catches intact. */
void feng_throw(void *value, const FengTypeDescriptor *desc) {
#if defined(_WIN32)
    (void)desc;
    feng_release(value);
    feng_panic("feng_throw: native Windows exception backend is not implemented");
#else
    FengUnwindException *exception;
    _Unwind_Reason_Code reason;

    if (g_finalizer_depth > 0U) {
        feng_release(value);
        feng_panic("exception escaped finalizer");
    }

    exception = (FengUnwindException *)calloc(1U, sizeof(*exception));
    if (exception == NULL) {
        feng_release(value);
        feng_panic("feng_throw: out of memory");
    }

    feng_unwind_exception_init(exception, value, desc);

    reason = _Unwind_RaiseException(&exception->unwind);
    feng_cleanup_release_all();
    feng_unwind_exception_destroy(exception);
    feng_panic("uncaught exception (unwind reason=%d)", (int)reason);
#endif
}

/* Borrow the payload only from the innermost active catch. */
void *feng_caught_value(void) {
    return g_catch_top != NULL && g_catch_top->exception != NULL
               ? g_catch_top->exception->value : NULL;
}

/* Read the clause selected for the innermost active catch. */
int feng_caught_clause(void) {
    return g_catch_top != NULL && g_catch_top->exception != NULL
               ? g_catch_top->exception->matched_clause : -1;
}

/* Transfer the active catch's original record back to the platform unwinder. */
void feng_rethrow(void) {
#if defined(_WIN32)
    feng_panic("feng_rethrow: native Windows exception backend is not implemented");
#else
    FengUnwindException *exception = g_catch_top != NULL ? g_catch_top->exception : NULL;
    _Unwind_Reason_Code reason;

    if (exception == NULL) {
        feng_panic("feng_rethrow: no current exception");
    }
    g_catch_top->exception = NULL;
    reason = _Unwind_Resume_or_Rethrow(&exception->unwind);

    /* A return means the restarted search found no outer handler. Match the
     * uncaught feng_throw path by draining live frames and releasing the same
     * exception object before terminating. */
    feng_cleanup_release_all();
    feng_unwind_exception_cleanup(reason, &exception->unwind);
    feng_panic("uncaught rethrown exception (unwind reason=%d)", (int)reason);
#endif
}
