/* Actual array headers must retain a final opportunity to nominate a cycle. */
#include "runtime/feng_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); \
} } while (0)

/* One object owns an array whose active element points back at the object. */
typedef struct ArrayCycleNode {
    FengManagedHeader header;
    FengArray *array;
} ArrayCycleNode;

static unsigned array_cycle_drops;
static bool array_cycle_revive;
static ArrayCycleNode *array_cycle_saved;

/* Observe collection and optionally preserve the entire mixed component. */
static void array_cycle_finalize(void *value) {
    ++array_cycle_drops;
    if (array_cycle_revive) {
        array_cycle_revive = false;
        array_cycle_saved = feng_retain(value);
    }
}

/* Normal ARC destruction uses the same existing release entry point. */
static void array_cycle_release_children(void *value) {
    ArrayCycleNode *node = value;
    FengArray *array = node->array;
    node->array = NULL;
    feng_release(array);
}

static const FengManagedFieldDescriptor array_cycle_fields[] = {
    {offsetof(ArrayCycleNode, array), NULL, NULL},
};
static const FengTypeDescriptor array_cycle_descriptor = {
    .name = "test.ArrayCycleNode", .size = sizeof(ArrayCycleNode),
    .finalizer = array_cycle_finalize, .release_children = array_cycle_release_children,
    .is_potentially_cyclic = true, .managed_field_count = 1,
    .managed_fields = array_cycle_fields,
};

/* A selected aggregate element also tests tag-aware array traversal. */
typedef struct ArrayCycleValue {
    uint32_t tag;
    void *payload;
} ArrayCycleValue;

static const FengDefaultZeroInitDescriptor array_cycle_zero = {.kind = FENG_DEFAULT_ZERO_BYTES};
static const FengManagedSlotDescriptor array_cycle_slots[] = {
    {offsetof(ArrayCycleValue, payload), FENG_SLOT_NONE, NULL},
    {offsetof(ArrayCycleValue, payload), FENG_SLOT_POINTER, NULL},
};
static const FengAggregateDescriptor array_cycle_aggregate = {
    .name = "test.ArrayCycleValue", .size = sizeof(ArrayCycleValue),
    .default_zero_init = &array_cycle_zero,
    .managed_slot_count = 2U, .managed_slots = array_cycle_slots,
    .managed_slot_choice_offset = offsetof(ArrayCycleValue, tag) + 1U,
};
static const FengAggregateDescriptor array_cycle_empty = {
    .name = "test.NoManagedSlots", .size = sizeof(int),
    .default_zero_init = &array_cycle_zero,
};

/* The first scan sees an external array owner; only the later array release
 * can nominate the now-unreachable component. Repeated live scans must not
 * permanently suppress nomination, including after finalizer resurrection. */
static void array_cycle_last_owner(bool aggregate, bool resurrect) {
    feng_cycle_runtime_shutdown();
    array_cycle_drops = 0;
    array_cycle_revive = resurrect;
    array_cycle_saved = NULL;
    ArrayCycleNode *node = feng_object_new(&array_cycle_descriptor);
    FengArray *array = feng_array_new_storage_kinded(
        aggregate ? FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS : FENG_VALUE_MANAGED_POINTER,
        aggregate ? &array_cycle_aggregate : NULL, NULL,
        aggregate ? sizeof(ArrayCycleValue) : sizeof(void *), 1U, 2U);
    CHECK(array->header.desc->is_potentially_cyclic);
    node->array = feng_retain(array);
    if (aggregate) {
        ArrayCycleValue *items = feng_array_data(array);
        items[0] = (ArrayCycleValue){.tag = 1U, .payload = feng_retain(node)};
        items[1] = (ArrayCycleValue){.tag = 1U, .payload = (void *)(uintptr_t)1};
    } else {
        void **items = feng_array_data(array);
        items[0] = feng_retain(node);
        items[1] = (void *)(uintptr_t)1;
    }
    feng_release(node);
    CHECK(array_cycle_drops == 0U);
    feng_retain(array);
    feng_release(array);
    CHECK(array_cycle_drops == 0U);
    feng_release(array);
    CHECK(array_cycle_drops == 1U);
    if (resurrect) {
        CHECK(array_cycle_saved == node && node->array == array);
        CHECK(node->header.refcount == 2U && array->header.refcount == 1U);
        array_cycle_saved = NULL;
        feng_release(node);
        CHECK(array_cycle_drops == 2U);
    }
    feng_cycle_runtime_shutdown();
}

/* Trivial and empty-aggregate arrays retain their existing acyclic path. */
void test_array_cycle_candidates(void) {
    FengArray *trivial = feng_array_new_kinded(FENG_VALUE_TRIVIAL, NULL, NULL, sizeof(int), 1U);
    FengArray *empty = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
                                            &array_cycle_empty, NULL, sizeof(int), 1U);
    CHECK(trivial->header.desc == &feng_array_descriptor);
    CHECK(empty->header.desc == &feng_array_descriptor);
    CHECK(!trivial->header.desc->is_potentially_cyclic);
    feng_release(trivial);
    feng_release(empty);
    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(1U);
    feng_cycle_unlock();
    for (unsigned aggregate = 0; aggregate < 2U; ++aggregate)
        for (unsigned resurrect = 0; resurrect < 2U; ++resurrect)
            array_cycle_last_owner(aggregate != 0U, resurrect != 0U);
    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(256U);
    feng_cycle_unlock();
    puts("array cycle candidates: last array owner, live scans, selected slots and resurrection passed");
}
