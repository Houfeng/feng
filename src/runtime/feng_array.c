/* Fixed-length array runtime. Element storage is heap-allocated separately
 * from the header so that arbitrary alignment (up to malloc's guarantee) is
 * trivially satisfied for any 1A element type. */
#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <stdlib.h>
#include <string.h>

const FengTypeDescriptor feng_array_descriptor = {
    .name = "feng.builtin.array",
    .size = 0U, /* variable layout per instance */
    .finalizer = NULL,
};

void feng_array_finalize_internal(struct FengArray *a) {
    if (a == NULL) {
        return;
    }

    if (a->items != NULL) {
        size_t i;

        switch (a->element_kind) {
            case FENG_VALUE_TRIVIAL:
                /* No per-element work; raw bytes only. */
                break;
            case FENG_VALUE_MANAGED_POINTER: {
                void **slots = (void **)a->items;
                for (i = 0U; i < a->length; ++i) {
                    feng_release(slots[i]);
                }
                break;
            }
            case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS: {
                unsigned char *base = (unsigned char *)a->items;
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

    free(a->items);
    a->items = NULL;
}

FengArray *feng_array_new_kinded(FengValueKind element_kind,
                                 const FengAggregateValueDescriptor *element_aggregate,
                                 const FengTypeDescriptor *element_desc,
                                 size_t element_size,
                                 size_t length) {
    struct FengArray *a;

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

    a = (struct FengArray *)calloc(1, sizeof(*a));
    if (a == NULL) {
        feng_panic("feng_array_new_kinded: out of memory for header");
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
        a->items = calloc(length, element_size);
        if (a->items == NULL) {
            free(a);
            feng_panic("feng_array_new_kinded: out of memory for %zu elements",
                       length);
        }
        /* For AGGREGATE elements, the descriptor decides whether all-zero
         * bytes are a legal default. ZERO_BYTES is already satisfied by
         * calloc; INIT_FN aggregates require a per-element initialiser
         * call so the managed slots reach a properly-retained state. */
        if (element_kind == FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS &&
            element_aggregate->default_init->kind == FENG_DEFAULT_INIT_FN) {
            unsigned char *base = (unsigned char *)a->items;
            size_t i;
            for (i = 0U; i < length; ++i) {
                element_aggregate->default_init->init_fn(base + i * element_size);
            }
        }
    } else {
        a->items = NULL;
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
    return array != NULL ? ((struct FengArray *)array)->items : NULL;
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
