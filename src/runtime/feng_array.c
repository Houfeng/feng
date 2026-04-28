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

    if (a->element_is_managed && a->items != NULL) {
        size_t i;
        void **slots = (void **)a->items;

        for (i = 0U; i < a->length; ++i) {
            feng_release(slots[i]);
        }
    }

    free(a->items);
    a->items = NULL;
}

FengArray *feng_array_new(const FengTypeDescriptor *element_desc,
                          size_t element_size,
                          bool element_is_managed,
                          size_t length) {
    struct FengArray *a;

    if (element_size == 0U) {
        feng_panic("feng_array_new: element_size must be non-zero");
    }
    if (length != 0U && element_size > SIZE_MAX / length) {
        feng_panic("feng_array_new: length %zu * element_size %zu overflows",
                   length,
                   element_size);
    }

    a = (struct FengArray *)calloc(1, sizeof(*a));
    if (a == NULL) {
        feng_panic("feng_array_new: out of memory for header");
    }

    a->header.desc = &feng_array_descriptor;
    a->header.tag = FENG_TYPE_TAG_ARRAY;
    a->header.refcount = 1U;
    a->length = length;
    a->element_size = element_size;
    a->element_desc = element_desc;
    a->element_is_managed = element_is_managed;

    if (length > 0U) {
        a->items = calloc(length, element_size);
        if (a->items == NULL) {
            free(a);
            feng_panic("feng_array_new: out of memory for %zu elements", length);
        }
    } else {
        a->items = NULL;
    }

    return (FengArray *)a;
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
