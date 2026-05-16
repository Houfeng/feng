/* Fixed-length array runtime.
 *
 * Current implementation uses split layout: array header and element payload
 * are allocated separately (`items`).
 *
 * Phase-1 optimization target is true tail-inline payload with no placeholder
 * member in FengArray. After that migration, payload address must be computed
 * by one shared helper using aligned offset (max_align_t baseline), and all
 * array access/finalize/collector paths must reuse that helper. */
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

    if (a == NULL || a->length == 0U) {
        return NULL;
    }
    base = (unsigned char *)a;
    return (void *)(base + feng_array_data_offset());
}

const void *feng_array_payload_inline_const(const struct FengArray *a) {
    const unsigned char *base;

    if (a == NULL || a->length == 0U) {
        return NULL;
    }
    base = (const unsigned char *)a;
    return (const void *)(base + feng_array_data_offset());
}

const FengTypeDescriptor feng_array_descriptor = {
    .name = "feng.builtin.array",
    .size = 0U, /* variable layout per instance */
    .finalizer = NULL,
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

FengArray *feng_array_new_kinded(FengValueKind element_kind,
                                 const FengAggregateValueDescriptor *element_aggregate,
                                 const FengTypeDescriptor *element_desc,
                                 size_t element_size,
                                 size_t length) {
    struct FengArray *a;
    size_t payload_size;
    size_t data_offset;
    size_t total_size;

    /* Per-kind precondition checks: surface descriptor / codegen mistakes
     * eagerly rather than miscount or corrupt during finalize. */
    switch (element_kind) {
        case FENG_VALUE_TRIVIAL:
            if (element_aggregate != NULL) {
                feng_panic("feng_array_new_kinded: TRIVIAL kind must have NULL element_aggregate");
            }
            break;
        case FENG_VALUE_MANAGED_POINTER:
            if (element_aggregate != NULL) {
                feng_panic("feng_array_new_kinded: MANAGED_POINTER kind must have NULL element_aggregate");
            }
            if (element_size != sizeof(void *)) {
                feng_panic("feng_array_new_kinded: MANAGED_POINTER element_size must be sizeof(void*) (%zu), got %zu",
                           sizeof(void *),
                           element_size);
            }
            break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            if (element_aggregate == NULL) {
                feng_panic("feng_array_new_kinded: AGGREGATE kind requires non-NULL element_aggregate");
            }
            if (element_size != element_aggregate->size) {
                feng_panic("feng_array_new_kinded: AGGREGATE element_size %zu disagrees with descriptor->size %zu",
                           element_size,
                           element_aggregate->size);
            }
            if (element_aggregate->default_init == NULL) {
                feng_panic("feng_array_new_kinded: aggregate descriptor missing default_init policy");
            }
            break;
        default:
            feng_panic("feng_array_new_kinded: unknown element_kind=%d",
                       (int)element_kind);
    }

    if (element_size == 0U) {
        feng_panic("feng_array_new_kinded: element_size must be non-zero");
    }
    if (length != 0U && element_size > SIZE_MAX / length) {
        feng_panic("feng_array_new_kinded: length %zu * element_size %zu overflows",
                   length,
                   element_size);
    }
    payload_size = length * element_size;
    data_offset = feng_array_data_offset();
    if (payload_size > SIZE_MAX - data_offset) {
        feng_panic("feng_array_new_kinded: data_offset %zu + payload_size %zu overflows",
                   data_offset,
                   payload_size);
    }
    total_size = data_offset + payload_size;

    a = (struct FengArray *)calloc(1, total_size);
    if (a == NULL) {
        feng_panic("feng_array_new_kinded: out of memory for %zu bytes", total_size);
    }

    a->header.desc = &feng_array_descriptor;
    a->header.tag = FENG_TYPE_TAG_ARRAY;
    a->header.refcount = 1U;
    a->length = length;
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
    return feng_array_payload_inline((struct FengArray *)array);
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

const FengAggregateValueDescriptor *feng_array_element_aggregate(const FengArray *array) {
    if (array == NULL) {
        feng_panic("feng_array_element_aggregate: array must not be NULL");
    }
    return ((const struct FengArray *)array)->element_aggregate;
}
