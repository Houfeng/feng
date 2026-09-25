#ifndef FENG_EXCEPTION_LSDA_H
#define FENG_EXCEPTION_LSDA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private result of decoding one native call-site entry; not a runtime ABI. */
typedef struct FengExceptionLanding {
    uintptr_t address;
    intptr_t selector;
    bool cleanup;
} FengExceptionLanding;

/* Decode compiler-owned LSDA. A non-NULL limit additionally bounds the entire
 * input (used by decoder tests); the unwind ABI supplies no overall length.
 * All declared table bounds, offsets and integer encodings are validated. */
bool feng_exception_lsda_find(const unsigned char *data,
                              const unsigned char *limit,
                              uintptr_t region_start, uintptr_t ip,
                              const void *thrown_type, bool allow_catch,
                              FengExceptionLanding *out);

#endif
