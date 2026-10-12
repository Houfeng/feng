/* Exercise shared bodies through optimized and ordinary external call paths. */
#include "consumer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Internal allocator lets this test distinguish initialized length from capacity. */
FengArray *feng_array_new_storage_kinded(FengValueKind kind,
    const FengAggregateDescriptor *aggregate, const FengTypeDescriptor *element,
    size_t size, size_t length, size_t capacity);

/* A managed reference whose finalizer records destruction order. */
typedef struct Object { FengManagedHeader header; int id; } Object;
/* A value containing a managed slot and a scalar payload. */
typedef struct Value { Object *object; int tag; } Value;
static int destroyed[16], destroyed_count, initialized;
/* Record exactly one destruction for each owned object. */
static void finalize(void *pointer) {
    assert(destroyed_count < 16);
    destroyed[destroyed_count++] = ((Object *)pointer)->id;
}
static const FengTypeDescriptor object_desc = {
    .name = "inline.Object", .size = sizeof(Object), .finalizer = finalize
};
/* A non-byte-zero default exercises aggregate initialization and release. */
static void default_value(void *pointer) {
    Value *value = pointer;
    value->object = feng_object_new(&object_desc);
    value->object->id = ++initialized;
    value->tag = 42;
}
static const FengDefaultZeroInitDescriptor value_zero = {
    .kind = FENG_DEFAULT_INIT_FN, .init_fn = default_value
};
static const FengManagedSlotDescriptor value_slots[] = {
    { .offset = offsetof(Value, object), .kind = FENG_SLOT_POINTER }
};
static const FengAggregateDescriptor value_desc = {
    .name = "inline.Value", .size = sizeof(Value), .default_zero_init = &value_zero,
    .managed_slot_count = 1, .managed_slots = value_slots
};

/* Empty representations, array storage prefixes, and canonical function identity. */
static void accessors(void) {
    assert(feng_array_length(NULL) == 0 && feng_array_data(NULL) == NULL);
    assert(feng_array_payload_inline(NULL) == NULL);
    assert(consumer_a_length() == feng_array_length && consumer_b_length() == feng_array_length);
    assert(consumer_a_data() == feng_array_data && consumer_b_data() == feng_array_data);
    assert(consumer_a_check() == feng_array_check_index && consumer_b_check() == feng_array_check_index);
    for (size_t capacity = 0; capacity < 12; ++capacity) {
        for (size_t length = 0; length <= capacity; ++length) {
            FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                NULL, NULL, 1, length, capacity);
            unsigned char *data = feng_array_data(array);
            assert(feng_array_length(array) == length);
            assert(consumer_a_length()(array) == length);
            assert(consumer_b_data()(array) == data);
            assert((data == NULL) == (length == 0));
            assert((feng_array_payload_inline(array) == NULL) == (capacity == 0));
            if (length != 0) {
                assert((uintptr_t)data % _Alignof(max_align_t) == 0);
                assert(data == (unsigned char *)array + FENG_ARRAY_DATA_OFFSET);
            }
            for (size_t i = 0; i < length; ++i) {
                assert(data[i] == 0); data[i] = (unsigned char)(i + 17);
                assert(consumer_a_read(array, i) == i + 17 + length);
                assert(consumer_b_read(array, i) == i + 17 + length);
                consumer_a_check()(array, i);
            }
            feng_release(array);
        }
    }
}

/* Inline data access must not alter initialization, ownership, or finalizer order. */
static void lifecycle(void) {
    FengArray *managed = feng_array_new_kinded(FENG_VALUE_MANAGED_POINTER, NULL,
        &object_desc, sizeof(void *), 2);
    Object **objects = feng_array_data(managed);
    assert(objects[0] == NULL && objects[1] == NULL);
    for (int i = 0; i < 2; ++i) {
        objects[i] = feng_object_new(&object_desc); objects[i]->id = i + 7;
    }
    feng_retain(managed); feng_release(managed);
    assert(destroyed_count == 0);
    feng_release(managed);
    assert(destroyed_count == 2 && destroyed[0] == 7 && destroyed[1] == 8);
    FengArray *aggregate = feng_array_new_storage_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
        &value_desc, NULL, sizeof(Value), 2, 9);
    Value *values = feng_array_data(aggregate);
    assert(initialized == 2 && values[0].tag == 42 && values[1].tag == 42);
    assert(values[0].object->header.refcount == 1 && values[1].object->header.refcount == 1);
    FengArray *outer = feng_array_new_kinded(FENG_VALUE_MANAGED_POINTER, NULL, NULL, sizeof(void *), 1);
    ((FengArray **)feng_array_data(outer))[0] = aggregate;
    feng_release(outer);
    assert(destroyed_count == 4 && destroyed[2] == 1 && destroyed[3] == 2);
}

/* Isolated invalid accesses let the shell check the exact existing panic text. */
int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "null") == 0) consumer_a_read(NULL, 0);
        FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL, NULL, NULL, 1, 2, 9);
        consumer_b_read(array, strcmp(argv[1], "length") == 0 ? 2 : SIZE_MAX);
        return 1;
    }
    accessors(); lifecycle(); feng_runtime_shutdown();
    puts("PASS: inline identity, storage boundaries, values and ownership");
    return 0;
}
