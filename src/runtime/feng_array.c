/* Array runtime with tail-inline payload and immutable per-instance capacity.
 * Ordinary arrays are created with length == capacity. Internal container
 * storage may reserve uninitialized slots in [length, capacity); payload
 * addressing is shared, while lifecycle traversal is always length-bounded. */
#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <stdlib.h>
#include <string.h>

static size_t feng_array_align_up(size_t value, size_t align) {
    return (value + (align - 1U)) & ~(align - 1U);
}

static size_t feng_array_data_offset(void) {
    return feng_array_align_up(sizeof(struct FengArray), _Alignof(max_align_t));
}

void *feng_array_payload_inline(struct FengArray *a) {
    unsigned char *base;

    if (a == NULL || a->capacity == 0U) {
        return NULL;
    }
    base = (unsigned char *)a;
    return (void *)(base + feng_array_data_offset());
}

const void *feng_array_payload_inline_const(const struct FengArray *a) {
    const unsigned char *base;

    if (a == NULL || a->capacity == 0U) {
        return NULL;
    }
    base = (const unsigned char *)a;
    return (const void *)(base + feng_array_data_offset());
}

void feng_array_slice_copy_into(struct FengArray *dst,
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

const FengTypeDescriptor feng_array_descriptor = {
    .name = "feng.builtin.array",
    .size = 0U, /* variable layout per instance */
    .finalizer = NULL,
    .reified_generic_params_count = 0, .reified_generic_params = NULL,
    .reified_field_offset_count = 0,   .reified_field_offsets = NULL,
    .reified_agg_deps_count = 0,       .reified_agg_deps = NULL,
    .reified_type_deps_count = 0,      .reified_type_deps = NULL,
};

void feng_array_finalize_internal(struct FengArray *a) {
    void *payload;

    if (a == NULL) {
        return;
    }

    payload = feng_array_payload_inline(a);

    if (payload != NULL) {
        size_t i;

        switch (a->element_kind) {
            case FENG_VALUE_TRIVIAL:
                /* No per-element work; raw bytes only. */
                break;
            case FENG_VALUE_MANAGED_POINTER: {
                void **slots = (void **)payload;
                for (i = 0U; i < a->length; ++i) {
                    feng_release(slots[i]);
                }
                break;
            }
            case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS: {
                unsigned char *base = (unsigned char *)payload;
                for (i = 0U; i < a->length; ++i) {
                    feng_aggregate_release(base + i * a->element_size,
                                           a->element_aggregate);
                }
                break;
            }
            default:
                feng_panic("feng_array_finalize_internal: corrupted element_kind=%d",
                           (int)a->element_kind);
        }
    }
}

FengArray *feng_array_new_storage_kinded(
    FengValueKind element_kind,
    const FengAggregateDescriptor *element_aggregate,
    const FengTypeDescriptor *element_desc,
    size_t element_size,
    size_t length,
    size_t capacity) {
    struct FengArray *a;
    size_t payload_size;
    size_t data_offset;
    size_t total_size;

    /* Per-kind precondition checks: surface descriptor / codegen mistakes
     * eagerly rather than miscount or corrupt during finalize. */
    switch (element_kind) {
        case FENG_VALUE_TRIVIAL:
            if (element_aggregate != NULL) {
                feng_panic("feng_array_new_storage_kinded: TRIVIAL kind must have NULL element_aggregate");
            }
            break;
        case FENG_VALUE_MANAGED_POINTER:
            if (element_aggregate != NULL) {
                feng_panic("feng_array_new_storage_kinded: MANAGED_POINTER kind must have NULL element_aggregate");
            }
            if (element_size != sizeof(void *)) {
                feng_panic("feng_array_new_storage_kinded: MANAGED_POINTER element_size must be sizeof(void*) (%zu), got %zu",
                           sizeof(void *),
                           element_size);
            }
            break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            if (element_aggregate == NULL) {
                feng_panic("feng_array_new_storage_kinded: AGGREGATE kind requires non-NULL element_aggregate");
            }
            if (element_size != element_aggregate->size) {
                feng_panic("feng_array_new_storage_kinded: AGGREGATE element_size %zu disagrees with descriptor->size %zu",
                           element_size,
                           element_aggregate->size);
            }
            if (element_aggregate->default_init == NULL) {
                feng_panic("feng_array_new_storage_kinded: aggregate descriptor missing default_init policy");
            }
            break;
        default:
            feng_panic("feng_array_new_storage_kinded: unknown element_kind=%d",
                       (int)element_kind);
    }

    if (element_size == 0U) {
        feng_panic("feng_array_new_storage_kinded: element_size must be non-zero");
    }
    if (length > capacity) {
        feng_panic("feng_array_new_storage_kinded: length %zu exceeds capacity %zu",
                   length,
                   capacity);
    }
    if (capacity != 0U && element_size > SIZE_MAX / capacity) {
        feng_panic("feng_array_new_storage_kinded: capacity %zu * element_size %zu overflows",
                   capacity,
                   element_size);
    }
    payload_size = capacity * element_size;
    data_offset = feng_array_data_offset();
    if (payload_size > SIZE_MAX - data_offset) {
        feng_panic("feng_array_new_storage_kinded: data_offset %zu + payload_size %zu overflows",
                   data_offset,
                   payload_size);
    }
    total_size = data_offset + payload_size;

    a = (struct FengArray *)calloc(1, total_size);
    if (a == NULL) {
        feng_panic("feng_array_new_storage_kinded: out of memory for %zu bytes", total_size);
    }

    a->header.desc = &feng_array_descriptor;
    a->header.tag = FENG_TYPE_TAG_ARRAY;
    a->header.refcount = 1U;
    a->length = length;
    a->capacity = capacity;
    a->element_size = element_size;
    a->element_desc = element_desc;
    a->element_kind = element_kind;
    a->element_aggregate = element_aggregate;

    if (length > 0U) {
        unsigned char *base = (unsigned char *)feng_array_payload_inline(a);

        /* For AGGREGATE elements, the descriptor decides whether all-zero
         * bytes are a legal default. ZERO_BYTES is already satisfied by
         * calloc; INIT_FN aggregates require a per-element initialiser
         * call so the managed slots reach a properly-retained state. */
        if (element_kind == FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS &&
            element_aggregate->default_init->kind == FENG_DEFAULT_INIT_FN) {
            size_t i;
            for (i = 0U; i < length; ++i) {
                element_aggregate->default_init->init_fn(base + i * element_size);
            }
        }
    }

    return (FengArray *)a;
}

FengArray *feng_array_new_kinded(FengValueKind element_kind,
                                 const FengAggregateDescriptor *element_aggregate,
                                 const FengTypeDescriptor *element_desc,
                                 size_t element_size,
                                 size_t length) {
    return feng_array_new_storage_kinded(element_kind,
                                         element_aggregate,
                                         element_desc,
                                         element_size,
                                         length,
                                         length);
}

FengArray *feng_array_new(const FengTypeDescriptor *element_desc,
                          size_t element_size,
                          bool element_is_managed,
                          size_t length) {
    return feng_array_new_kinded(element_is_managed ? FENG_VALUE_MANAGED_POINTER
                                                     : FENG_VALUE_TRIVIAL,
                                 NULL,
                                 element_desc,
                                 element_size,
                                 length);
}

size_t feng_array_length(const FengArray *array) {
    return array != NULL ? ((const struct FengArray *)array)->length : 0U;
}

void *feng_array_data(FengArray *array) {
    struct FengArray *storage = (struct FengArray *)array;

    if (storage == NULL || storage->length == 0U) {
        return NULL;
    }
    return feng_array_payload_inline(storage);
}

void feng_array_check_index(const FengArray *array, size_t index) {
    size_t length = feng_array_length(array);

    if (index >= length) {
        feng_panic("array index %zu out of range (length=%zu)", index, length);
    }
}

FengValueKind feng_array_element_kind(const FengArray *array) {
    if (array == NULL) {
        feng_panic("feng_array_element_kind: array must not be NULL");
    }
    return ((const struct FengArray *)array)->element_kind;
}

const FengAggregateDescriptor *feng_array_element_aggregate(const FengArray *array) {
    if (array == NULL) {
        feng_panic("feng_array_element_aggregate: array must not be NULL");
    }
    return ((const struct FengArray *)array)->element_aggregate;
}
