#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

/* Shared helper for bare-T return contracts. Copies the source value into the
 * caller-provided output slot while preserving Feng ownership semantics for
 * managed pointers and by-value aggregates. */
static void runtime_contract_copy_value_to_out(const char *name,
                                               const FengGenericParamDescriptor *type,
                                               const void *value,
                                               void *out) {
    if (type == NULL) {
        feng_panic("%s: type must not be NULL", name);
    }
    if (value == NULL) {
        feng_panic("%s: value must not be NULL", name);
    }
    if (out == NULL) {
        feng_panic("%s: out must not be NULL", name);
    }

    switch (type->kind) {
        case FENG_VALUE_TRIVIAL:
            memcpy(out, value, type->size);
            return;
        case FENG_VALUE_MANAGED_POINTER: {
            void *managed = *(void *const *)value;

            feng_retain(managed);
            *(void **)out = managed;
            return;
        }
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            if (type->aggregate == NULL) {
                feng_panic("%s: aggregate descriptor must not be NULL", name);
            }
            feng_aggregate_retain((void *)value, type->aggregate);
            memcpy(out, value, type->size);
            return;
    }

    feng_panic("%s: unknown value kind=%d", name, (int)type->kind);
}

/* Returns the UTF-8 byte length of a Feng string as a stable i64 contract
 * result, trapping if the runtime size exceeds the contract range. */
int64_t feng_string_utf8_length(FengString *value) {
    size_t length = feng_string_length(value);

    if (length > (size_t)INT64_MAX) {
        feng_panic("feng_string_utf8_length: length overflow");
    }

    return (int64_t)length;
}

/* Returns the logical element count of an array as a stable i64 contract
 * result. The generic descriptor is accepted for ABI uniformity but is not
 * consulted by this helper. */
int64_t feng_array_length_i64(const FengGenericParamDescriptor *type,
                              const FengArray *value) {
    size_t length = feng_array_length(value);

    (void)type;

    if (length > (size_t)INT64_MAX) {
        feng_panic("feng_array_length_i64: length overflow");
    }

    return (int64_t)length;
}

/* Copies the right-open range [start, start + length) into a fresh array while
 * preserving per-element ownership semantics recorded on the source array. The
 * generic descriptor is currently carried only to match the runtime generic ABI. */
FengArray *feng_array_slice(const FengGenericParamDescriptor *type,
                            const FengArray *value,
                            int64_t start,
                            int64_t length) {
    const struct FengArray *src = (const struct FengArray *)value;
    size_t slice_start;
    size_t slice_length;
    FengArray *result;

    (void)type;

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

/* Test-only runtime contract used to exercise bare-T return lowering with a
 * real descriptor-aware out carrier. */
void __test_value_identity(const FengGenericParamDescriptor *type,
                           const void *value,
                           void *out) {
    runtime_contract_copy_value_to_out("__test_value_identity", type, value, out);
}
