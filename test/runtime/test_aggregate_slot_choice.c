/* Descriptor-driven selection, independent of the compiler's union layout. */
#include "runtime/feng_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

/* A leaf records destruction order and can inspect or resurrect its owner. */
typedef struct ChoiceLeaf {
    FengManagedHeader header;
    unsigned id;
} ChoiceLeaf;

static unsigned leaf_events[32];
static size_t leaf_event_count;
static void **watched_slot;
static unsigned watched_id;
static unsigned resurrect_id;
static void *resurrected_leaf;

/* Observe clearing before finalization, and optionally publish a new owner. */
static void choice_leaf_finalize(void *value) {
    ChoiceLeaf *leaf = value;
    CHECK(leaf_event_count < sizeof leaf_events / sizeof leaf_events[0]);
    leaf_events[leaf_event_count++] = leaf->id;
    if (leaf->id == watched_id) CHECK(*watched_slot == NULL);
    if (leaf->id == resurrect_id) resurrected_leaf = feng_retain(leaf);
}

static const FengTypeDescriptor choice_leaf_descriptor = {
    .name = "test.ChoiceLeaf",
    .size = sizeof(ChoiceLeaf),
    .finalizer = choice_leaf_finalize,
};

/* Allocate one independent +1 leaf reference. */
static ChoiceLeaf *choice_leaf(unsigned id) {
    ChoiceLeaf *leaf = feng_object_new(&choice_leaf_descriptor);
    leaf->id = id;
    return leaf;
}

/* Sequential nested payload: both fields are managed, in declaration order. */
typedef struct ChoicePair {
    void *first;
    void *second;
} ChoicePair;

static const FengManagedSlotDescriptor choice_pair_slots[] = {
    {offsetof(ChoicePair, first), FENG_SLOT_POINTER, NULL},
    {offsetof(ChoicePair, second), FENG_SLOT_POINTER, NULL},
};
static const FengAggregateDescriptor choice_pair_descriptor = {
    .name = "test.ChoicePair",
    .size = sizeof(ChoicePair),
    .managed_slot_count = 2U,
    .managed_slots = choice_pair_slots,
};

/* Generic selected aggregate, deliberately not a C or Feng union. */
typedef struct ChoiceValue {
    uint32_t choice;
    uint32_t marker;
    ChoicePair payload;
    FengManagedSlotDescriptor forward;
} ChoiceValue;

static const FengManagedSlotDescriptor choice_slots[] = {
    {offsetof(ChoiceValue, payload), FENG_SLOT_NONE, NULL},
    {offsetof(ChoiceValue, payload.first), FENG_SLOT_POINTER, NULL},
    {offsetof(ChoiceValue, payload), FENG_SLOT_NESTED_AGGREGATE,
     &choice_pair_descriptor},
    {offsetof(ChoiceValue, payload), FENG_SLOT_NONE, NULL},
    {offsetof(ChoiceValue, forward), FENG_SLOT_FORWARD, NULL},
    {offsetof(ChoiceValue, payload.second), FENG_SLOT_POINTER, NULL},
    {offsetof(ChoiceValue, payload), FENG_SLOT_NONE, NULL},
};
static const FengDefaultZeroInitDescriptor choice_zero = {
    .kind = FENG_DEFAULT_ZERO_BYTES,
};
static const FengAggregateDescriptor choice_descriptor = {
    .name = "test.ChoiceValue",
    .size = sizeof(ChoiceValue),
    .default_zero_init = &choice_zero,
    .managed_slot_count = sizeof choice_slots / sizeof choice_slots[0],
    .managed_slots = choice_slots,
    .managed_slot_choice_offset = offsetof(ChoiceValue, choice) + 1U,
};
static const FengGenericParamDescriptor choice_generic = {
    .kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
    .descriptor = &choice_descriptor,
};

/* Form borrowed bytes; callers explicitly acquire each active reference. */
static void choice_value_init(ChoiceValue *value, uint32_t choice,
                              void *first, void *second) {
    memset(value, 0, sizeof *value);
    value->choice = choice;
    value->marker = UINT32_C(0x12345678);
    value->payload.first = first;
    value->payload.second = second;
    value->forward = (FengManagedSlotDescriptor){
        offsetof(ChoiceValue, payload.second), FENG_SLOT_POINTER, NULL};
}

/* Independent fixture expectations for first/second field ownership. */
static unsigned choice_owns(uint32_t choice, unsigned field) {
    static const unsigned owners[][2] = {
        {0, 0}, {1, 0}, {1, 1}, {0, 0}, {0, 1}, {0, 1}, {0, 0},
    };
    return owners[choice][field];
}

/* All kinds and first/middle/last NONE entries preserve unrelated fields. */
static void test_choice_kinds(void) {
    for (uint32_t choice = 0; choice < choice_descriptor.managed_slot_count;
         ++choice) {
        ChoiceLeaf *first = choice_leaf(1), *second = choice_leaf(2);
        ChoiceValue value, before;
        choice_value_init(&value, choice, first, second);
        memcpy(&before, &value, sizeof before);
        feng_aggregate_retain(&value, &choice_descriptor);
        CHECK(first->header.refcount == 1U + choice_owns(choice, 0));
        CHECK(second->header.refcount == 1U + choice_owns(choice, 1));
        feng_aggregate_release(&value, &choice_descriptor);
        CHECK(first->header.refcount == 1U && second->header.refcount == 1U);
        CHECK(memcmp(&value, &before, sizeof value) == 0);
        leaf_event_count = 0;
        feng_release(first);
        feng_release(second);
        CHECK(leaf_event_count == 2U);

        /* Inactive storage may contain bytes that are not valid pointers. */
        if (choice_owns(choice, 0) == 0 && choice_owns(choice, 1) == 0) {
            value.payload.first = value.payload.second = (void *)(uintptr_t)1;
            feng_aggregate_retain(&value, &choice_descriptor);
            feng_aggregate_release_and_clear_internal(&value, &choice_descriptor);
            CHECK(value.payload.first == (void *)(uintptr_t)1);
        }
    }
    ChoiceValue empty;
    memset(&empty, 0xA5, sizeof empty);
    feng_aggregate_default_zero_init(&empty, &choice_descriptor);
    CHECK(empty.choice == 0U && empty.payload.first == NULL);
    feng_aggregate_release(&empty, &choice_descriptor);
}

/* A byte buffer permits valid selector reads at aligned and unaligned offsets. */
typedef struct ChoiceOffsetValue {
    void *pointer;
    unsigned char selectors[12];
} ChoiceOffsetValue;

/* Exercise unaligned and last-valid offsets, and an index larger than 255. */
static void test_choice_offsets(void) {
    ChoiceOffsetValue value = {0};
    FengManagedSlotDescriptor slots[257] = {0};
    slots[256] = (FengManagedSlotDescriptor){0U, FENG_SLOT_POINTER, NULL};
    FengAggregateDescriptor desc = {
        .size = sizeof value,
        .managed_slot_count = 257U,
        .managed_slots = slots,
    };
    ChoiceLeaf *leaf = choice_leaf(1);
    value.pointer = leaf;
    for (size_t offset = offsetof(ChoiceOffsetValue, selectors);
         offset + sizeof(uint32_t) <= sizeof value; ++offset) {
        uint32_t index = 256U;
        desc.managed_slot_choice_offset = offset + 1U;
        memcpy((unsigned char *)&value + offset, &index, sizeof index);
        feng_aggregate_retain(&value, &desc);
        CHECK(leaf->header.refcount == 2U);
        feng_aggregate_release(&value, &desc);
        CHECK(leaf->header.refcount == 1U);
    }
    /* Unset selection ignores value bytes, including an empty table. */
    desc.managed_slot_choice_offset = 0U;
    feng_aggregate_retain(&value, &desc);
    CHECK(leaf->header.refcount == 2U);
    feng_aggregate_release(&value, &desc);
    desc.managed_slot_count = 0U;
    desc.managed_slots = NULL;
    feng_aggregate_retain(&value, &desc);
    feng_aggregate_release(&value, &desc);
    leaf_event_count = 0;
    feng_release(leaf);
    CHECK(leaf_event_count == 1U);
}

/* Source and target select independently, including shared managed objects. */
static void test_choice_assignment_matrix(void) {
    for (uint32_t source = 0; source < 7U; ++source) {
        for (uint32_t target = 0; target < 7U; ++target) {
            for (unsigned move = 0; move < 2U; ++move) {
                ChoiceLeaf *first = choice_leaf(1), *second = choice_leaf(2);
                ChoiceValue src, dst, original, remaining;
                choice_value_init(&src, source, first, second);
                choice_value_init(&dst, target, first, second);
                feng_aggregate_retain(&src, &choice_descriptor);
                feng_aggregate_retain(&dst, &choice_descriptor);
                memcpy(&original, &src, sizeof original);
                memcpy(&remaining, &src, sizeof remaining);
                if (move) {
                    feng_aggregate_take(&dst, &src, &choice_descriptor);
                    if (choice_owns(source, 0)) remaining.payload.first = NULL;
                    if (choice_owns(source, 1)) remaining.payload.second = NULL;
                } else {
                    feng_aggregate_assign(&dst, &src, &choice_descriptor);
                }
                CHECK(memcmp(&src, &remaining, sizeof src) == 0);
                CHECK(memcmp(&dst, &original, sizeof dst) == 0);
                CHECK(first->header.refcount == 1U + (2U - move) * choice_owns(source, 0));
                CHECK(second->header.refcount == 1U + (2U - move) * choice_owns(source, 1));
                feng_aggregate_assign(&dst, &dst, &choice_descriptor);
                feng_aggregate_take(&dst, &dst, &choice_descriptor);
                CHECK(memcmp(&dst, &original, sizeof dst) == 0);
                feng_aggregate_release(&src, &choice_descriptor);
                feng_aggregate_release(&dst, &choice_descriptor);
                CHECK(first->header.refcount == 1U && second->header.refcount == 1U);
                leaf_event_count = 0;
                feng_release(first);
                feng_release(second);
                CHECK(leaf_event_count == 2U);
            }
        }
    }
}

/* Clear-before-release, field order and resurrection use the original visitors. */
static void test_choice_clear_and_resurrection(void) {
    ChoiceValue value;
    choice_value_init(&value, 2U, choice_leaf(1), choice_leaf(2));
    watched_slot = &value.payload.first;
    watched_id = 1U;
    leaf_event_count = 0;
    feng_aggregate_release_and_clear_internal(&value, &choice_descriptor);
    CHECK(leaf_event_count == 2U && leaf_events[0] == 1U && leaf_events[1] == 2U);
    CHECK(value.payload.first == NULL && value.payload.second == NULL);
    CHECK(value.choice == 2U && value.marker == UINT32_C(0x12345678));
    choice_value_init(&value, 1U, choice_leaf(1), (void *)(uintptr_t)1);
    resurrect_id = 1U;
    leaf_event_count = 0;
    feng_aggregate_release_and_clear_internal(&value, &choice_descriptor);
    CHECK(leaf_event_count == 1U && resurrected_leaf != NULL);
    CHECK(((ChoiceLeaf *)resurrected_leaf)->header.refcount == 1U);
    CHECK(value.payload.first == NULL && value.payload.second == (void *)(uintptr_t)1);
    resurrect_id = watched_id = 0U;
    feng_release(resurrected_leaf);
    resurrected_leaf = NULL;
    watched_slot = NULL;
    CHECK(leaf_event_count == 2U);
}

/* Legacy forward points to a nested selected aggregate at a nonzero offset. */
typedef struct ChoiceForward {
    FengManagedSlotDescriptor forward;
    ChoiceValue value;
} ChoiceForward;

static const FengManagedSlotDescriptor choice_forward_slot = {
    offsetof(ChoiceForward, forward), FENG_SLOT_FORWARD, NULL,
};
static const FengAggregateDescriptor choice_forward_descriptor = {
    .size = sizeof(ChoiceForward),
    .managed_slot_count = 1U,
    .managed_slots = &choice_forward_slot,
};

/* New selection and existing forward can nest in either direction. */
static void test_choice_forward_nesting(void) {
    ChoiceLeaf *first = choice_leaf(1), *second = choice_leaf(2);
    ChoiceForward outer = {0};
    outer.forward = (FengManagedSlotDescriptor){
        offsetof(ChoiceForward, value), FENG_SLOT_NESTED_AGGREGATE,
        &choice_descriptor};
    choice_value_init(&outer.value, 4U, first, second);
    outer.value.forward = (FengManagedSlotDescriptor){
        offsetof(ChoiceValue, payload), FENG_SLOT_NESTED_AGGREGATE,
        &choice_pair_descriptor};
    feng_aggregate_retain(&outer, &choice_forward_descriptor);
    CHECK(first->header.refcount == 2U && second->header.refcount == 2U);
    feng_aggregate_release(&outer, &choice_forward_descriptor);
    CHECK(first->header.refcount == 1U && second->header.refcount == 1U);
    outer.value.forward.kind = FENG_SLOT_NONE;
    outer.value.forward.nested = NULL;
    feng_aggregate_retain(&outer, &choice_forward_descriptor);
    CHECK(first->header.refcount == 1U && second->header.refcount == 1U);
    feng_aggregate_release(&outer, &choice_forward_descriptor);
    leaf_event_count = 0;
    feng_release(first);
    feng_release(second);
    CHECK(leaf_event_count == 2U);
}

/* Arrays and logical exception-frame cleanup retain descriptor-driven ownership. */
static void test_choice_array_and_cleanup(void) {
    ChoiceLeaf *first = choice_leaf(1), *second = choice_leaf(2);
    ChoiceValue value;
    choice_value_init(&value, 2U, first, second);
    FengArray *array = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
        &choice_descriptor, NULL, sizeof(ChoiceValue), 2U);
    ChoiceValue *items = feng_array_data(array);
    CHECK(items[0].choice == 0U && items[1].choice == 0U);
    feng_aggregate_assign(&items[0], &value, &choice_descriptor);
    value.choice = 4U;
    feng_aggregate_assign(&items[1], &value, &choice_descriptor);
    FengArray *slice = feng_array_slice(&choice_generic, array, 0, 2);
    CHECK(first->header.refcount == 3U && second->header.refcount == 5U);
    feng_array_storage_remove(&choice_generic, array, 0, 1);
    CHECK(first->header.refcount == 2U && second->header.refcount == 4U);
    feng_release(array);
    feng_release(slice);
    CHECK(first->header.refcount == 1U && second->header.refcount == 1U);

    FengFrameMarker function_frame, frame;
    FengCleanupNode cleanup;
    feng_frame_push(&function_frame);
    feng_try_frame_push(&frame);
    value.choice = 2U;
    feng_aggregate_retain(&value, &choice_descriptor);
    feng_cleanup_push_aggregate(&cleanup, &value, &choice_descriptor);
    feng_frame_release_to(&frame);
    feng_frame_pop();
    CHECK(first->header.refcount == 1U && second->header.refcount == 1U);
    leaf_event_count = 0;
    feng_release(first);
    feng_release(second);
    CHECK(leaf_event_count == 2U);
}

/* A managed object exposes the selected aggregate through the existing field API. */
typedef struct ChoiceNode {
    FengManagedHeader header;
    ChoiceForward edge;
} ChoiceNode;

static size_t node_finalizations;
static ChoiceNode *resurrect_node;
static ChoiceNode *resurrected_node;

/* Count collection and optionally resurrect one node to exercise survivor scans. */
static void choice_node_finalize(void *value) {
    ++node_finalizations;
    if (value == resurrect_node) resurrected_node = feng_retain(value);
}

/* Ordinary ARC destruction releases only the current aggregate edges. */
static void choice_node_release_children(void *value) {
    ChoiceNode *node = value;
    feng_aggregate_release_and_clear_internal(&node->edge,
                                               &choice_forward_descriptor);
}

static const FengManagedFieldDescriptor choice_node_fields[] = {
    {offsetof(ChoiceNode, edge), NULL, &choice_forward_descriptor},
};
static const FengTypeDescriptor choice_node_descriptor = {
    .name = "test.ChoiceNode",
    .size = sizeof(ChoiceNode),
    .finalizer = choice_node_finalize,
    .release_children = choice_node_release_children,
    .is_potentially_cyclic = true,
    .managed_field_count = 1U,
    .managed_fields = choice_node_fields,
};

/* Build legacy forward -> selected aggregate -> pointer/aggregate/forward. */
static ChoiceNode *choice_node(void) {
    ChoiceNode *node = feng_object_new(&choice_node_descriptor);
    node->edge.forward = (FengManagedSlotDescriptor){
        offsetof(ChoiceForward, value), FENG_SLOT_NESTED_AGGREGATE,
        &choice_descriptor};
    return node;
}

/* Establish one cyclic edge and, for the pair case, one external child. */
static void choice_node_link(ChoiceNode *node, uint32_t choice,
                              ChoiceNode *child, ChoiceLeaf *external) {
    ChoiceValue *value = &node->edge.value;
    choice_value_init(value, choice, NULL, NULL);
    if (choice_owns(choice, 0)) value->payload.first = feng_retain(child);
    else value->payload.first = (void *)(uintptr_t)1;
    if (choice_owns(choice, 1)) {
        value->payload.second = feng_retain(choice == 2U ? (void *)external : (void *)child);
    } else value->payload.second = (void *)(uintptr_t)1;
}

/* Run collection under the runtime's existing lock contract. */
static void choice_collect(void) {
    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();
}

/* Read-only scans, edge rewriting, external children and resurrection agree. */
static void test_choice_cycles(void) {
    static const uint32_t choices[] = {1U, 2U, 4U, 5U};
    for (size_t i = 0; i < sizeof choices / sizeof choices[0]; ++i) {
        for (unsigned resurrect = 0; resurrect < 2U; ++resurrect) {
            feng_cycle_runtime_shutdown();
            ChoiceNode *a = choice_node(), *b = choice_node();
            ChoiceLeaf *external = choice_leaf(1);
            choice_node_link(a, choices[i], b, external);
            choice_node_link(b, choices[i], a, external);
            node_finalizations = leaf_event_count = 0;
            resurrect_node = resurrect ? a : NULL;
            feng_release(a);
            feng_release(b);
            choice_collect();
            CHECK(node_finalizations == 2U && leaf_event_count == 0U);
            if (resurrect) {
                CHECK(resurrected_node == a);
                CHECK(a->header.refcount == 2U && b->header.refcount == 1U);
                CHECK(external->header.refcount == (choices[i] == 2U ? 3U : 1U));
                /* Drop the resurrection root, then collect the intact cycle. */
                resurrect_node = NULL;
                resurrected_node = NULL;
                feng_release(a);
                choice_collect();
                CHECK(node_finalizations == 4U);
            }
            CHECK(external->header.refcount == 1U);
            feng_release(external);
            CHECK(leaf_event_count == 1U);
            feng_cycle_runtime_shutdown();
        }
    }
    /* NONE must not make inactive pointer-shaped bytes visible to the collector. */
    ChoiceNode *node = choice_node();
    choice_value_init(&node->edge.value, 3U, (void *)(uintptr_t)1, (void *)(uintptr_t)1);
    feng_retain(node);
    feng_release(node);
    choice_collect();
    CHECK(node->header.refcount == 1U);
    feng_release(node);
    feng_cycle_runtime_shutdown();
}

/* The old forward layout must work without enabling the new selector field. */
static void test_legacy_forward_cycles(void) {
    for (unsigned closure = 0; closure < 2U; ++closure) {
        ChoiceNode *a = choice_node(), *b = choice_node();
        ChoiceLeaf *external = choice_leaf(1);
        a->header.tag = b->header.tag = closure ? FENG_TYPE_TAG_CLOSURE
                                               : FENG_TYPE_TAG_OBJECT;
        a->edge.forward = b->edge.forward = (FengManagedSlotDescriptor){
            offsetof(ChoiceForward, value.payload), FENG_SLOT_NESTED_AGGREGATE,
            &choice_pair_descriptor};
        a->edge.value.payload = (ChoicePair){feng_retain(b), feng_retain(external)};
        b->edge.value.payload = (ChoicePair){feng_retain(a), feng_retain(external)};
        node_finalizations = leaf_event_count = 0;
        resurrect_node = a;
        feng_release(a);
        feng_release(b);
        choice_collect();
        CHECK(node_finalizations == 2U && resurrected_node == a);
        CHECK(a->header.refcount == 2U && b->header.refcount == 1U);
        CHECK(external->header.refcount == 3U);
        resurrect_node = resurrected_node = NULL;
        feng_release(a);
        choice_collect();
        CHECK(node_finalizations == 4U && external->header.refcount == 1U);
        feng_release(external);
        CHECK(leaf_event_count == 1U);
        feng_cycle_runtime_shutdown();
    }
}

/* Selected array slots participate in all scans; spare capacity is inactive. */
static void test_choice_array_cycles(void) {
    ChoiceNode *node = choice_node();
    ChoiceLeaf *external = choice_leaf(1);
    FengArray *array = feng_array_new_storage_kinded(
        FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &choice_descriptor, NULL,
        sizeof(ChoiceValue), 1U, 2U);
    ChoiceValue *items = feng_array_data(array);
    choice_value_init(&items[0], 2U, feng_retain(node), feng_retain(external));
    choice_value_init(&items[1], 2U, (void *)(uintptr_t)1, (void *)(uintptr_t)1);
    choice_value_init(&node->edge.value, 1U, feng_retain(array), (void *)(uintptr_t)1);
    node_finalizations = leaf_event_count = 0;
    resurrect_node = node;
    feng_release(array);
    feng_release(node);
    choice_collect();
    CHECK(node_finalizations == 1U && resurrected_node == node);
    CHECK(node->header.refcount == 2U);
    CHECK(((struct FengArray *)array)->header.refcount == 1U);
    CHECK(external->header.refcount == 2U);
    CHECK(items[0].payload.first == node);
    resurrect_node = resurrected_node = NULL;
    feng_release(node);
    choice_collect();
    CHECK(node_finalizations == 2U && external->header.refcount == 1U);
    feng_release(external);
    CHECK(leaf_event_count == 1U);
    feng_cycle_runtime_shutdown();
}

/* A free selected aggregate drops its reference to the resurrected child. */
static void test_choice_partial_resurrection(void) {
    ChoiceNode *parent = choice_node(), *child = choice_node();
    choice_value_init(&parent->edge.value, 2U, feng_retain(parent), feng_retain(child));
    choice_value_init(&child->edge.value, 0U, (void *)(uintptr_t)1, (void *)(uintptr_t)1);
    node_finalizations = 0;
    resurrect_node = child;
    feng_release(parent);
    feng_release(child);
    choice_collect();
    CHECK(node_finalizations == 2U && resurrected_node == child);
    CHECK(child->header.refcount == 1U);
    CHECK(child->edge.value.payload.first == (void *)(uintptr_t)1);
    resurrect_node = resurrected_node = NULL;
    feng_release(child);
    CHECK(node_finalizations == 3U);
    feng_cycle_runtime_shutdown();
}

/* A nonzero selector offset also works on the no-finalizer collection path. */
static void test_choice_cycle_offset_without_finalizers(void) {
    FengAggregateDescriptor aggregate = choice_descriptor;
    FengTypeDescriptor descriptor = choice_node_descriptor;
    aggregate.managed_slot_choice_offset = offsetof(ChoiceValue, marker) + 1U;
    descriptor.finalizer = NULL;
    ChoiceNode *a = choice_node(), *b = choice_node();
    ChoiceLeaf *external = choice_leaf(1U);
    a->header.desc = b->header.desc = &descriptor;
    a->edge.forward.nested = b->edge.forward.nested = &aggregate;
    choice_node_link(a, 2U, b, external);
    choice_node_link(b, 2U, a, external);
    a->edge.value.marker = b->edge.value.marker = 2U;
    a->edge.value.choice = b->edge.value.choice = UINT32_MAX;
    leaf_event_count = 0U;
    feng_release(a);
    feng_release(b);
    choice_collect();
    CHECK(external->header.refcount == 1U && leaf_event_count == 0U);
    feng_release(external);
    CHECK(leaf_event_count == 1U);
    feng_cycle_runtime_shutdown();
}

/* Ordinary pointer fields isolate restoration from both aggregate mechanisms. */
typedef struct RecoveryNode {
    FengManagedHeader header;
    void *next;
    void *external;
    unsigned id;
} RecoveryNode;

static unsigned recovery_events[3];
static bool recovery_publish;
static bool recovery_replace;
static bool recovery_create;
static bool recovery_bridge;
static uint32_t recovery_expected_rc;
static void *recovery_replacement;
static RecoveryNode *recovery_root;

/* Ordinary ARC cleanup, also used by a child created during finalization. */
static void recovery_release_children(void *value) {
    RecoveryNode *node = value;
    feng_release(feng_take(&node->next));
    feng_release(feng_take(&node->external));
}

/* Count the temporary external owner's destruction. */
static void recovery_transient_finalize(void *value) {
    RecoveryNode *node = value;
    ++recovery_events[node->id];
}

static const FengManagedFieldDescriptor recovery_fields[] = {
    {offsetof(RecoveryNode, next), NULL, NULL},
    {offsetof(RecoveryNode, external), NULL, NULL},
};
static const FengTypeDescriptor recovery_transient_descriptor = {
    .name = "test.RecoveryTransient",
    .size = sizeof(RecoveryNode),
    .finalizer = recovery_transient_finalize,
    .release_children = recovery_release_children,
    .managed_field_count = 2U,
    .managed_fields = recovery_fields,
};

/* Check real counts inside user code, then optionally change outgoing edges. */
static void recovery_finalize(void *value) {
    RecoveryNode *node = value;
    ++recovery_events[node->id];
    if (node->id != 0U) return;
    if (recovery_expected_rc != 0U) {
        CHECK(((FengManagedHeader *)node->external)->refcount == recovery_expected_rc);
    }
    if (recovery_bridge) {
        /* Move a white child behind a newly allocated external owner. Its last
         * release in Phase 2 will run normal ARC on the surviving child. */
        RecoveryNode *bridge = feng_object_new(&recovery_transient_descriptor);
        bridge->id = 2U;
        bridge->next = feng_retain(node->external);
        feng_assign(&node->external, bridge);
        feng_release(bridge);
    } else if (recovery_create) {
        ChoiceLeaf *fresh = choice_leaf(2U);
        feng_assign(&node->external, fresh);
        feng_release(fresh);
    } else if (recovery_replace) {
        feng_assign(&node->external, recovery_replacement);
    }
    if (recovery_publish) recovery_root = feng_retain(node);
}

static const FengTypeDescriptor recovery_descriptor = {
    .name = "test.RecoveryNode",
    .size = sizeof(RecoveryNode),
    .finalizer = recovery_finalize,
    .release_children = recovery_release_children,
    .is_potentially_cyclic = true,
    .managed_field_count = 2U,
    .managed_fields = recovery_fields,
};

/* Unchanged, cleared, replaced and freshly allocated external fields, with
 * and without resurrection, must each keep exactly their true owners. */
static void test_cycle_external_ownership(void) {
    for (unsigned action = 0; action < 4U; ++action) {
        for (unsigned resurrect = 0; resurrect < 2U; ++resurrect) {
            ChoiceLeaf *old = choice_leaf(1U), *replacement = choice_leaf(3U);
            RecoveryNode *node = feng_object_new(&recovery_descriptor);
            node->next = feng_retain(node);
            node->external = feng_retain(old);
            memset(recovery_events, 0, sizeof recovery_events);
            leaf_event_count = 0U;
            recovery_publish = resurrect != 0;
            recovery_replace = action == 1U || action == 2U;
            recovery_create = action == 3U;
            recovery_replacement = action == 2U ? replacement : NULL;
            recovery_expected_rc = 2U;
            feng_release(node);
            choice_collect();
            CHECK(recovery_events[0] == 1U);
            CHECK(old->header.refcount == 1U + (resurrect && action == 0U));
            CHECK(replacement->header.refcount == 1U + (resurrect && action == 2U));
            CHECK(leaf_event_count == (action == 3U && !resurrect ? 1U : 0U));
            recovery_publish = recovery_replace = recovery_create = false;
            recovery_expected_rc = 0U;
            if (resurrect) {
                CHECK(recovery_root == node && node->header.refcount == 2U);
                if (action == 3U) {
                    CHECK(((ChoiceLeaf *)node->external)->header.refcount == 1U);
                }
                recovery_root = NULL;
                feng_release(node);
                choice_collect();
                CHECK(recovery_events[0] == 2U);
            }
            CHECK(old->header.refcount == 1U && replacement->header.refcount == 1U);
            feng_release(old);
            feng_release(replacement);
            CHECK(leaf_event_count == (action == 3U ? 3U : 2U));
            feng_cycle_runtime_shutdown();
        }
    }
}

/* A Phase 2 external release may destroy a survivor already removed from wl. */
static void test_cycle_external_callback_releases_survivor(void) {
    RecoveryNode *parent = feng_object_new(&recovery_descriptor);
    RecoveryNode *child = feng_object_new(&recovery_descriptor);
    child->id = 1U;
    parent->next = feng_retain(parent);
    parent->external = feng_retain(child);
    memset(recovery_events, 0, sizeof recovery_events);
    recovery_bridge = true;
    feng_release(parent);
    feng_release(child);
    choice_collect();
    recovery_bridge = false;
    CHECK(recovery_events[0] == 1U);
    CHECK(recovery_events[1] == 2U);
    CHECK(recovery_events[2] == 1U);
    feng_cycle_runtime_shutdown();
}

/* Registered by the existing runtime test executable; no separate test runner. */
void test_aggregate_slot_choice(void) {
    test_choice_kinds();
    test_choice_offsets();
    test_choice_assignment_matrix();
    test_choice_clear_and_resurrection();
    test_choice_forward_nesting();
    test_choice_array_and_cleanup();
    test_choice_cycles();
    test_legacy_forward_cycles();
    test_choice_array_cycles();
    test_choice_partial_resurrection();
    test_choice_cycle_offset_without_finalizers();
    test_cycle_external_ownership();
    test_cycle_external_callback_releases_survivor();
    puts("aggregate slot choice: lifecycle, offsets, forwarding, arrays and cycles passed");
}
