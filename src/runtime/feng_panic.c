/* Fatal-error reporter. Phase 1A simply prints to stderr and aborts; future
 * phases will introduce structured panics with backtraces. */
#include "runtime/feng_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void feng_panic(const char *fmt, ...) {
    va_list args;

    fputs("feng: panic: ", stderr);
    if (fmt != NULL) {
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
