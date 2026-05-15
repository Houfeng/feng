#include "intrinsic/feng_intrinsic.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void feng_intrinsic_abort(const char *message) {
    fputs(message, stderr);
    fputc('\n', stderr);
    abort();
}

int64_t feng_string_utf8_length(uint8_t *value) {
    size_t length = value != NULL ? strlen((const char *)value) : 0U;

    if (length > (size_t)INT64_MAX) {
        feng_intrinsic_abort("feng_string_utf8_length: length overflow");
    }
    return (int64_t)length;
}
