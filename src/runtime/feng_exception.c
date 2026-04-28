/* Exception frame stack. Each try block in generated code allocates a stack
 * FengExceptionFrame, pushes it before setjmp, and pops it on every exit path
 * (normal completion, caught throw, or rethrow propagation). */
#include "runtime/feng_runtime.h"

#include <stdlib.h>

/* Single-threaded for Phase 1A; thread-local storage keeps this future-proof
 * without changing the public API. */
static _Thread_local FengExceptionFrame *g_top_frame = NULL;

void feng_exception_push(FengExceptionFrame *frame) {
    if (frame == NULL) {
        feng_panic("feng_exception_push: NULL frame");
    }

    frame->prev = g_top_frame;
    frame->value = NULL;
    frame->is_managed = 0;
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
        if (is_managed) {
            feng_release(value);
        }
        feng_panic("uncaught exception");
    }

    frame->value = value;
    frame->is_managed = is_managed;
    longjmp(frame->jb, 1);
}
