/* Array runtime with tail-inline payload and immutable per-instance capacity.
 * Ordinary arrays are created with length == capacity. Internal container
 * storage may reserve uninitialized slots in [length, capacity); payload
 * addressing is shared, while lifecycle traversal is always length-bounded. */
#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
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

/* Validate the array and erased element descriptor shared by all storage
 * contracts. Per-instance metadata remains authoritative for lifecycle work;
 * the generic descriptor is checked only for ABI consistency. */
static const struct FengArray *feng_array_storage_require_compatible(
        const char *operation,
        const FengGenericParamDescriptor *type,
        const FengArray *array) {
    const struct FengArray *storage = (const struct FengArray *)array;
    size_t generic_size;

    if (storage == NULL) {
        feng_panic("%s: array must not be NULL", operation);
    }
    if (type == NULL) {
        feng_panic("%s: type must not be NULL", operation);
    }
    if (type->kind != storage->element_kind) {
        feng_panic("%s: element kind mismatch (array=%d, type=%d)",
                   operation,
                   (int)storage->element_kind,
                   (int)type->kind);
    }
    if (type->descriptor == NULL) {
        feng_panic("%s: type descriptor must not be NULL", operation);
    }

    switch (type->kind) {
        case FENG_VALUE_TRIVIAL:
            generic_size = feng_generic_trivial_descriptor(type)->size;
            break;
        case FENG_VALUE_MANAGED_POINTER:
            generic_size = sizeof(void *);
            break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            if (feng_generic_aggregate_descriptor(type) !=
                storage->element_aggregate) {
                feng_panic("%s: aggregate descriptor mismatch", operation);
            }
            generic_size = feng_generic_aggregate_descriptor(type)->size;
            break;
        default:
            feng_panic("%s: unknown value kind=%d",
                       operation,
                       (int)type->kind);
    }

    if (generic_size != storage->element_size) {
        feng_panic("%s: element size mismatch (array=%zu, type=%zu)",
                   operation,
                   storage->element_size,
                   generic_size);
    }
    return storage;
}

/* Convert a non-negative Feng int contract argument to the runtime size
 * domain, trapping before any storage mutation on invalid input. */
static size_t feng_array_storage_require_size(const char *operation,
                                              const char *argument,
                                              intptr_t value) {
    if (value < 0) {
        feng_panic("%s: %s must be non-negative, got %" PRIdPTR,
                   operation,
                   argument,
                   value);
    }
    if ((uintmax_t)value > (uintmax_t)SIZE_MAX) {
        feng_panic("%s: %s exceeds runtime size range",
                   operation,
                   argument);
    }
    return (size_t)value;
}

/* Drop one initialized slot without leaving a stale managed pointer visible
 * to a synchronous cycle-collector traversal. */
static void feng_array_storage_release_slot(struct FengArray *storage,
                                            unsigned char *slot) {
    switch (storage->element_kind) {
        case FENG_VALUE_TRIVIAL:
            return;
        case FENG_VALUE_MANAGED_POINTER: {
            void *managed = *(void **)slot;

            *(void **)slot = NULL;
            feng_release(managed);
            return;
        }
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            feng_aggregate_release_and_clear_internal(
                slot,
                storage->element_aggregate);
            return;
        default:
            feng_panic("feng_array_storage: corrupted element_kind=%d",
                       (int)storage->element_kind);
    }
}

/* Return the fixed physical capacity of a valid array storage allocation. */
intptr_t feng_array_storage_get_capacity(
        const FengGenericParamDescriptor *type,
        const FengArray *array) {
    const struct FengArray *storage = feng_array_storage_require_compatible(
        "feng_array_storage_get_capacity",
        type,
        array);

    if (storage->capacity > (size_t)INTPTR_MAX) {
        feng_panic("feng_array_storage_get_capacity: capacity overflow");
    }
    return (intptr_t)storage->capacity;
}

/* Insert one copy-initialized element and move the existing suffix right
 * without changing ownership of the moved elements. */
void feng_array_storage_insert(const FengGenericParamDescriptor *type,
                               FengArray *array,
                               intptr_t index,
                               const void *value) {
    struct FengArray *storage = (struct FengArray *)
        feng_array_storage_require_compatible("feng_array_storage_insert",
                                              type,
                                              array);
    size_t insertion_index = feng_array_storage_require_size(
        "feng_array_storage_insert",
        "index",
        index);
    unsigned char *payload;
    unsigned char *slot;
    size_t suffix_size;
    void *managed = NULL;

    if (value == NULL) {
        feng_panic("feng_array_storage_insert: value must not be NULL");
    }
    if (insertion_index > storage->length) {
        feng_panic("feng_array_storage_insert: index %zu out of range (length=%zu)",
                   insertion_index,
                   storage->length);
    }
    if (storage->length >= storage->capacity) {
        feng_panic("feng_array_storage_insert: array storage is full (length=%zu, capacity=%zu)",
                   storage->length,
                   storage->capacity);
    }

    payload = (unsigned char *)feng_array_payload_inline(storage);
    slot = payload + insertion_index * storage->element_size;
    suffix_size = (storage->length - insertion_index) * storage->element_size;

    switch (storage->element_kind) {
        case FENG_VALUE_TRIVIAL:
            break;
        case FENG_VALUE_MANAGED_POINTER:
            managed = *(void *const *)value;
            feng_retain(managed);
            break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            feng_aggregate_retain((void *)value,
                                  storage->element_aggregate);
            break;
        default:
            feng_panic("feng_array_storage_insert: corrupted element_kind=%d",
                       (int)storage->element_kind);
    }

    if (suffix_size > 0U) {
        memmove(slot + storage->element_size, slot, suffix_size);
    }
    if (storage->element_kind == FENG_VALUE_MANAGED_POINTER) {
        *(void **)slot = managed;
    } else {
        memcpy(slot, value, storage->element_size);
    }
    storage->length += 1U;
}

/* Remove an initialized range, release only the removed elements, and move
 * the surviving suffix left without retain/release churn. */
void feng_array_storage_remove(const FengGenericParamDescriptor *type,
                               FengArray *array,
                               intptr_t index,
                               intptr_t count) {
    struct FengArray *storage = (struct FengArray *)
        feng_array_storage_require_compatible("feng_array_storage_remove",
                                              type,
                                              array);
    size_t removal_index = feng_array_storage_require_size(
        "feng_array_storage_remove",
        "index",
        index);
    size_t removal_count = feng_array_storage_require_size(
        "feng_array_storage_remove",
        "count",
        count);
    size_t tail_start;
    size_t tail_count;
    size_t new_length;
    unsigned char *payload;
    size_t i;

    if (removal_index > storage->length ||
        removal_count > storage->length - removal_index) {
        feng_panic("feng_array_storage_remove: range [index=%zu, count=%zu] out of range (length=%zu)",
                   removal_index,
                   removal_count,
                   storage->length);
    }
    if (removal_count == 0U) {
        return;
    }

    tail_start = removal_index + removal_count;
    tail_count = storage->length - tail_start;
    new_length = storage->length - removal_count;
    payload = (unsigned char *)feng_array_payload_inline(storage);

    if (storage->element_kind != FENG_VALUE_TRIVIAL) {
        for (i = removal_index; i < tail_start; ++i) {
            feng_array_storage_release_slot(
                storage,
                payload + i * storage->element_size);
        }
    }
    if (tail_count > 0U) {
        memmove(payload + removal_index * storage->element_size,
                payload + tail_start * storage->element_size,
                tail_count * storage->element_size);
    }
    storage->length = new_length;
}

/* Allocate fixed-capacity replacement storage and consume the old storage's
 * initialized range without consuming the caller's array reference.
 *
 * Old array: keep its allocation, capacity, header, and reference count, but
 * set its length to zero after a successful transfer. Any aliases still point
 * to this now-empty array; stale payload bytes no longer represent Feng values.
 *
 * Moved prefix: copy [0, move_length) byte-for-byte to the same indices in the
 * replacement and transfer slot ownership without default initialization,
 * retain, or release. The replacement becomes the sole semantic owner.
 *
 * Discarded suffix: release [move_length, old_length) exactly once according
 * to element kind. The slot-release helper clears managed references before a
 * release that may synchronously expose the array to the cycle collector. */
FengArray *feng_array_storage_migrate(
        const FengGenericParamDescriptor *type,
        FengArray *array,
        intptr_t new_capacity) {
    struct FengArray *storage = (struct FengArray *)
        feng_array_storage_require_compatible("feng_array_storage_migrate",
                                              type,
                                              array);
    size_t capacity = feng_array_storage_require_size(
        "feng_array_storage_migrate",
        "new_capacity",
        new_capacity);
    size_t move_length = storage->length < capacity
                             ? storage->length
                             : capacity;
    FengArray *result = feng_array_new_storage_kinded(
        storage->element_kind,
        storage->element_aggregate,
        storage->element_desc,
        storage->element_size,
        0U,
        capacity);
    struct FengArray *new_storage = (struct FengArray *)result;
    unsigned char *old_payload =
        (unsigned char *)feng_array_payload_inline(storage);
    size_t i;

    /* Elements truncated by the requested capacity do not move. Release each
     * non-trivial slot once while the old initialized range is still visible. */
    if (storage->element_kind != FENG_VALUE_TRIVIAL) {
        for (i = move_length; i < storage->length; ++i) {
            feng_array_storage_release_slot(
                storage,
                old_payload + i * storage->element_size);
        }
    }
    /* This raw copy transfers the retained prefix; it is intentionally not a
     * Feng copy-initialization and therefore performs no element RC changes. */
    if (move_length > 0U) {
        memcpy(feng_array_payload_inline(new_storage),
               old_payload,
               move_length * storage->element_size);
    }

    /* No lifecycle callback occurs between these stores. Emptying the source
     * prevents its finalizer or collector traversal from releasing the moved
     * prefix; publishing the destination length establishes its sole ownership. */
    storage->length = 0U;
    new_storage->length = move_length;
    return result;
}
