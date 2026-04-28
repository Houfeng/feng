/* Exception frame stack. Each try block in generated code allocates a stack
 * FengExceptionFrame, pushes it before setjmp, and pops it on every exit path
 * (normal completion, caught throw, or rethrow propagation). */
#include "runtime/feng_runtime.h"

#include <stdlib.h>

/* Single-threaded for Phase 1A; thread-local storage keeps this future-proof
 * without changing the public API. */
static _Thread_local FengExceptionFrame *g_top_frame = NULL;
static _Thread_local FengCleanupNode    *g_cleanup_top = NULL;

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
