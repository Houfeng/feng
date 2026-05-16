#include "runtime/feng_runtime.h"

#include <limits.h>

int64_t feng_string_utf8_length(FengString *value) {
    size_t length = feng_string_length(value);

    if (length > (size_t)INT64_MAX) {
        feng_panic("feng_string_utf8_length: length overflow");
    }

    return (int64_t)length;
}

int64_t feng_array_length_i64(const FengArray *value) {
    size_t length = feng_array_length(value);

    if (length > (size_t)INT64_MAX) {
        feng_panic("feng_array_length_i64: length overflow");
    }

    return (int64_t)length;
}
