#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
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

FengArray *feng_array_slice(const FengArray *value, int64_t start, int64_t length) {
    const struct FengArray *src = (const struct FengArray *)value;
    size_t slice_start;
    size_t slice_length;
    FengArray *result;

    if (src == NULL) {
        feng_panic("feng_array_slice: array must not be NULL");
    }

    if (start < 0) {
        feng_panic("feng_array_slice: start must be non-negative, got %" PRId64,
                   start);
    }
    if ((uint64_t)start > (uint64_t)SIZE_MAX) {
        feng_panic("feng_array_slice: start exceeds runtime size range");
    }
    if (length < 0) {
        feng_panic("feng_array_slice: length must be non-negative, got %" PRId64,
                   length);
    }
    if ((uint64_t)length > (uint64_t)SIZE_MAX) {
        feng_panic("feng_array_slice: length exceeds runtime size range");
    }

    slice_start = (size_t)start;
    slice_length = (size_t)length;

    if (slice_start > src->length ||
        slice_length > src->length - slice_start) {
        feng_panic("feng_array_slice: range [start=%" PRId64 ", length=%" PRId64 "] out of range (length=%zu)",
                   start,
                   length,
                   src->length);
    }

    result = feng_array_new_kinded(src->element_kind,
                                   src->element_aggregate,
                                   src->element_desc,
                                   src->element_size,
                                   slice_length);
    feng_array_slice_copy_into((struct FengArray *)result,
                               src,
                               slice_start,
                               slice_length);
    return result;
}
