#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

static size_t feng_array_slice_index_from_i64(int64_t value,
                                              const char *name) {
    if (value < 0) {
        feng_panic("feng_array_slice: %s must be non-negative, got %" PRId64,
                   name,
                   value);
    }
    if ((uint64_t)value > (uint64_t)SIZE_MAX) {
        feng_panic("feng_array_slice: %s exceeds runtime size range", name);
    }

    return (size_t)value;
}

static void feng_array_slice_copy_into(struct FengArray *dst,
                                       const struct FengArray *src,
                                       size_t start,
                                       size_t length) {
    const unsigned char *src_payload;
    unsigned char *dst_payload;

    if (length == 0U) {
        return;
    }

    src_payload = (const unsigned char *)feng_array_payload_inline_const(src);
    dst_payload = (unsigned char *)feng_array_payload_inline(dst);

    switch (src->element_kind) {
        case FENG_VALUE_TRIVIAL:
            memcpy(dst_payload,
                   src_payload + start * src->element_size,
                   length * src->element_size);
            return;
        case FENG_VALUE_MANAGED_POINTER: {
            void **dst_slots = (void **)dst_payload;
            void *const *src_slots = (void *const *)src_payload;
            size_t i;

            for (i = 0U; i < length; ++i) {
                dst_slots[i] = feng_retain(src_slots[start + i]);
            }
            return;
        }
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS: {
            const unsigned char *src_base = src_payload + start * src->element_size;
            size_t i;

            for (i = 0U; i < length; ++i) {
                feng_aggregate_assign(dst_payload + i * src->element_size,
                                      src_base + i * src->element_size,
                                      src->element_aggregate);
            }
            return;
        }
        default:
            feng_panic("feng_array_slice: corrupted element_kind=%d",
                       (int)src->element_kind);
    }
}

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

FengArray *feng_array_slice(const FengArray *value, int64_t start, int64_t end) {
    const struct FengArray *src = (const struct FengArray *)value;
    size_t slice_start;
    size_t slice_end;
    size_t slice_length;
    FengArray *result;

    if (src == NULL) {
        feng_panic("feng_array_slice: array must not be NULL");
    }

    slice_start = feng_array_slice_index_from_i64(start, "start");
    slice_end = feng_array_slice_index_from_i64(end, "end");

    if (slice_end < slice_start) {
        feng_panic("feng_array_slice: end (%" PRId64 ") must be >= start (%" PRId64 ")",
                   end,
                   start);
    }
    if (slice_end > src->length) {
        feng_panic("feng_array_slice: range [%" PRId64 ", %" PRId64 ") out of range (length=%zu)",
                   start,
                   end,
                   src->length);
    }

    slice_length = slice_end - slice_start;
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
