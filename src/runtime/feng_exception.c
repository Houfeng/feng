/* Exception frame stack. Each try block in generated code allocates a stack
 * FengExceptionFrame, pushes it before setjmp, and pops it on every exit path
 * (normal completion, caught throw, or rethrow propagation). */
#include "runtime/feng_runtime.h"

#include <stdlib.h>
#include <stddef.h>

/* Single-threaded for Phase 1A; thread-local storage keeps this future-proof
 * without changing the public API. */
static _Thread_local FengExceptionFrame *g_top_frame = NULL;
static _Thread_local FengCleanupNode    *g_cleanup_top = NULL;
static _Thread_local FengUnwindException *g_current_unwind = NULL;

#if !defined(_WIN32)
static void feng_unwind_exception_cleanup(_Unwind_Reason_Code reason,
                                          struct _Unwind_Exception *unwind) {
    FengUnwindException *exception;

    (void)reason;
    if (unwind == NULL) {
        return;
    }
    exception = (FengUnwindException *)((char *)unwind - offsetof(FengUnwindException, unwind));
    if (g_current_unwind == exception) {
        g_current_unwind = NULL;
    }
    free(exception);
}
#endif

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

static void feng_release_current_unwind_exception(bool release_value) {
    FengUnwindException *exception = g_current_unwind;

    if (exception == NULL) {
        return;
    }
    g_current_unwind = NULL;
    if (release_value && exception->value != NULL) {
        feng_release(exception->value);
        exception->value = NULL;
    }
    free(exception);
}

void feng_exception_push(FengExceptionFrame *frame) {
    if (frame == NULL) {
        feng_panic("feng_exception_push: NULL frame");
    }

    frame->prev = g_top_frame;
    frame->value = NULL;
    frame->is_managed = 0;
    frame->cleanup_top = g_cleanup_top;
    g_top_frame = frame;
}

void feng_exception_pop(void) {
    if (g_top_frame == NULL) {
        feng_panic("feng_exception_pop: stack underflow");
    }
    g_top_frame = g_top_frame->prev;
}

FengExceptionFrame *feng_exception_current(void) {
    return g_top_frame;
}

void feng_exception_throw(void *value, int is_managed) {
    FengExceptionFrame *frame = g_top_frame;

    if (frame == NULL) {
        /* Walk and release every still-live managed local, then panic. We do
         * NOT pop the cleanup nodes here: g_cleanup_top will be pointing at
         * stack memory that abort() will discard anyway, and the slots have
         * been NULLed so any stale read post-panic is harmless. */
        while (g_cleanup_top != NULL) {
            void **slot = g_cleanup_top->slot;
            if (slot != NULL && *slot != NULL) {
                feng_release(*slot);
                *slot = NULL;
            }
            g_cleanup_top = g_cleanup_top->prev;
        }
        if (is_managed) {
            feng_release(value);
        }
        feng_panic("uncaught exception");
    }

    /* Release every managed local that is in scope between the throw site
     * and the catching frame. The cleanup_top snapshot recorded at
     * feng_exception_push time marks the boundary. */
    while (g_cleanup_top != frame->cleanup_top) {
        FengCleanupNode *node = g_cleanup_top;
        if (node == NULL) {
            /* Defensive: the chain became shorter than the snapshot, which
             * means LIFO discipline was violated by the generator. */
            feng_panic("feng_exception_throw: cleanup chain underflow");
        }
        void **slot = node->slot;
        if (slot != NULL && *slot != NULL) {
            feng_release(*slot);
            *slot = NULL;
        }
        g_cleanup_top = node->prev;
    }

    frame->value = value;
    frame->is_managed = is_managed;
    longjmp(frame->jb, 1);
}

void feng_cleanup_push(FengCleanupNode *node, void **slot) {
    if (node == NULL) {
        feng_panic("feng_cleanup_push: NULL node");
    }
    if (slot == NULL) {
        feng_panic("feng_cleanup_push: NULL slot");
    }
    node->slot = slot;
    node->prev = g_cleanup_top;
    g_cleanup_top = node;
}

void feng_cleanup_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_cleanup_pop: chain underflow");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

void feng_frame_push(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_frame_push: NULL marker");
    }
    marker->node.slot = NULL;
    marker->node.prev = g_cleanup_top;
    g_cleanup_top = &marker->node;
}

void feng_frame_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_frame_pop: chain underflow");
    }
    if (g_cleanup_top->slot != NULL) {
        feng_panic("feng_frame_pop: top cleanup node is not a frame marker");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

void feng_throw(void *value, const FengTypeDescriptor *desc) {
    FengUnwindException *exception =
        (FengUnwindException *)calloc(1U, sizeof(*exception));

    if (exception == NULL) {
        feng_release(value);
        feng_panic("feng_throw: out of memory");
    }

    feng_release_current_unwind_exception(true);
    feng_unwind_exception_init(exception, value, desc);
    g_current_unwind = exception;

    if (g_top_frame == NULL) {
        feng_release_current_unwind_exception(true);
        feng_panic("uncaught exception");
    }

    feng_exception_throw(exception, 0);
}

void *feng_caught_value(void) {
    return g_current_unwind != NULL ? g_current_unwind->value : NULL;
}

int feng_caught_clause(void) {
    return g_current_unwind != NULL ? g_current_unwind->matched_clause : -1;
}

void feng_rethrow(void) {
    FengUnwindException *exception = g_current_unwind;

    if (exception == NULL) {
        feng_panic("feng_rethrow: no current exception");
    }
    if (g_top_frame == NULL) {
        feng_panic("uncaught exception");
    }
    feng_exception_throw(exception, 0);
}

void feng_release_unwind_exception(void) {
    feng_release_current_unwind_exception(true);
}
