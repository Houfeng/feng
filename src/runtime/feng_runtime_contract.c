#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
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
            memcpy(out, value, feng_generic_value_size(type));
            return;
        case FENG_VALUE_MANAGED_POINTER: {
            void *managed = *(void *const *)value;

            feng_retain(managed);
            *(void **)out = managed;
            return;
        }
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            if (type->descriptor == NULL) {
                feng_panic("%s: aggregate descriptor must not be NULL", name);
            }
            feng_aggregate_retain((void *)value,
                                  feng_generic_aggregate_descriptor(type));
            memcpy(out, value, feng_generic_value_size(type));
            return;
    }

    feng_panic("%s: unknown value kind=%d", name, (int)type->kind);
}

/* Returns the UTF-8 byte length of a Feng string as a stable intptr_t contract
 * result, trapping if the runtime size exceeds the contract range. */
intptr_t feng_string_utf8_length(FengString *value) {
    size_t length = feng_string_length(value);

    if (length > (size_t)INTPTR_MAX) {
        feng_panic("feng_string_utf8_length: length overflow");
    }

    return (intptr_t)length;
}

/* Returns the logical element count of an array as a stable intptr_t contract
 * result. The generic descriptor is accepted for ABI uniformity but is not
 * consulted by this helper. */
intptr_t feng_array_get_length(const FengGenericParamDescriptor *type,
                              const FengArray *value) {
    size_t length = feng_array_length(value);

    (void)type;

    if (length > (size_t)INTPTR_MAX) {
        feng_panic("feng_array_get_length: length overflow");
    }

    return (intptr_t)length;
}

/* Copies the right-open range [start, start + length) into a fresh array while
 * preserving per-element ownership semantics recorded on the source array. The
 * generic descriptor is currently carried only to match the runtime generic ABI. */
FengArray *feng_array_slice(const FengGenericParamDescriptor *type,
                            const FengArray *value,
                            intptr_t start,
                            intptr_t length) {
    const struct FengArray *src = (const struct FengArray *)value;
    size_t slice_start;
    size_t slice_length;
    FengArray *result;

    (void)type;

    if (src == NULL) {
        feng_panic("feng_array_slice: array must not be NULL");
    }

    if (start < 0) {
        feng_panic("feng_array_slice: start must be non-negative, got %" PRIdPTR,
                   start);
    }
    if ((uintptr_t)start > (uintptr_t)SIZE_MAX) {
        feng_panic("feng_array_slice: start exceeds runtime size range");
    }
    if (length < 0) {
        feng_panic("feng_array_slice: length must be non-negative, got %" PRIdPTR,
                   length);
    }
    if ((uintptr_t)length > (uintptr_t)SIZE_MAX) {
        feng_panic("feng_array_slice: length exceeds runtime size range");
    }

    slice_start = (size_t)start;
    slice_length = (size_t)length;

    if (slice_start > src->length ||
        slice_length > src->length - slice_start) {
        feng_panic("feng_array_slice: range [start=%" PRIdPTR ", length=%" PRIdPTR "] out of range (length=%zu)",
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

/* Thin wrappers around libc malloc/free that bridge Feng's intptr_t
 * pointer representation to the system-header pointer/size_t types. */
intptr_t feng_alloc(intptr_t size) {
    return (intptr_t)malloc((size_t)size);
}

void feng_free(intptr_t ptr) {
    free((void *)(intptr_t)ptr);
}

/* Returns true when the given C-side pointer is NULL. */
bool feng_pointer_is_null(void *ptr) {
    return ptr == NULL;
}

/* Returns true when two C-side pointers have the same address. */
bool feng_pointer_equal(void *left, void *right) {
    return left == right;
}

/* Returns a pointer advanced by `offset` bytes from `ptr`. */
void *feng_pointer_move(void *ptr, intptr_t offset) {
    return (char *)ptr + offset;
}

/* Returns the signed byte distance between two pointers (a - b). */
intptr_t feng_pointer_diff(void *a, void *b) {
    return (intptr_t)((char *)a - (char *)b);
}

/* Reads a scalar value of size determined by the generic type descriptor from
 * the memory at `ptr` into the caller-provided `result` slot.  Only trivial
 * (scalar) types are allowed; managed pointers and aggregates cause a panic. */
void feng_pointer_get_scalar(const FengGenericParamDescriptor *type,
                             void *ptr, void *result) {
    if (type == NULL) {
        feng_panic("feng_pointer_get_scalar: type descriptor must not be NULL");
    }
    if (type->kind != FENG_VALUE_TRIVIAL) {
        feng_panic("feng_pointer_get_scalar: only scalar types allowed, got kind=%d",
                   (int)type->kind);
    }
    if (ptr == NULL) {
        feng_panic("feng_pointer_get_scalar: null pointer");
    }
    if (result == NULL) {
        feng_panic("feng_pointer_get_scalar: result carrier must not be NULL");
    }
    memcpy(result, ptr, feng_generic_trivial_descriptor(type)->size);
}

/* Reads a pointer value stored at the memory location pointed to by `ptr`.
 * This is a generic "follow one level of indirection" operation for C interop
 * with functions that use output pointer parameters (T**). */
void *feng_pointer_get_pointer(void *ptr) {
    if (ptr == NULL) {
        feng_panic("feng_pointer_get_pointer: null pointer");
    }
    void *result;
    memcpy(&result, ptr, sizeof(void *));
    return result;
}

/* Compares the byte sub-ranges [a_start, a_end) and [b_start, b_end) of two
 * Feng strings for equality without any intermediate allocation. */
bool feng_string_range_equal(FengString *a, intptr_t a_start, intptr_t a_end,
                             FengString *b, intptr_t b_start, intptr_t b_end) {
    size_t a_total = feng_string_length(a);
    size_t b_total = feng_string_length(b);
    size_t a_len, b_len, range_len;

    if (a_start < 0 || a_end < a_start || (size_t)a_end > a_total) {
        feng_panic("feng_string_range_equal: a range [%" PRIdPTR ", %" PRIdPTR ") "
                   "out of bounds (length=%zu)", a_start, a_end, a_total);
    }
    if (b_start < 0 || b_end < b_start || (size_t)b_end > b_total) {
        feng_panic("feng_string_range_equal: b range [%" PRIdPTR ", %" PRIdPTR ") "
                   "out of bounds (length=%zu)", b_start, b_end, b_total);
    }

    a_len = (size_t)(a_end - a_start);
    b_len = (size_t)(b_end - b_start);
    if (a_len != b_len) {
        return false;
    }

    range_len = a_len;
    if (range_len == 0U) {
        return true;
    }

    return memcmp(feng_string_data(a) + a_start,
                  feng_string_data(b) + b_start,
                  range_len) == 0;
}

/* Test-only runtime contract used to exercise bare-T return lowering with a
 * real descriptor-aware out carrier. */
void __test_value_identity(const FengGenericParamDescriptor *type,
                           const void *value,
                           void *out) {
    runtime_contract_copy_value_to_out("__test_value_identity", type, value, out);
}
