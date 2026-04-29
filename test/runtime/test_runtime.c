/* Phase 1A runtime regression suite. Each test exercises a single subsystem
 * (refcount, string, array, exception). Tests deliberately avoid relying on
 * malloc-tracking instrumentation — they assert observable behaviour and
 * cross-check finalizer counts via test-local descriptors. */
#include "runtime/feng_runtime.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(expr)                                                                  \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

/* --- Test object with finalizer counter -------------------------------- */

typedef struct TestObject {
    FengManagedHeader header;
    int payload;
} TestObject;

static int g_finalize_count = 0;

static void test_object_finalize(void *self) {
    (void)self;
    ++g_finalize_count;
}

static const FengTypeDescriptor test_object_descriptor = {
    .name = "test.TestObject",
    .size = sizeof(TestObject),
    .finalizer = test_object_finalize,
};

/* --- Tests ------------------------------------------------------------- */

static void test_object_retain_release(void) {
    TestObject *obj;

    g_finalize_count = 0;
    obj = (TestObject *)feng_object_new(&test_object_descriptor);
    ASSERT(obj != NULL);
    ASSERT(obj->header.refcount == 1U);
    ASSERT(obj->header.tag == FENG_TYPE_TAG_OBJECT);
    obj->payload = 42;

    feng_retain(obj);
    ASSERT(obj->header.refcount == 2U);

    feng_release(obj);
    ASSERT(obj->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    feng_release(obj);
    ASSERT(g_finalize_count == 1);
}

static void test_retain_release_nullsafe(void) {
    ASSERT(feng_retain(NULL) == NULL);
    feng_release(NULL); /* must not crash */
}

static void test_assign_barrier(void) {
    TestObject *a;
    TestObject *b;
    void *slot = NULL;

    g_finalize_count = 0;
    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_assign(&slot, a);
    ASSERT(slot == a);
    ASSERT(a->header.refcount == 2U);

    feng_assign(&slot, b);
    ASSERT(slot == b);
    ASSERT(a->header.refcount == 1U);
    ASSERT(b->header.refcount == 2U);

    feng_assign(&slot, NULL);
    ASSERT(slot == NULL);
    ASSERT(b->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    feng_release(a);
    feng_release(b);
    ASSERT(g_finalize_count == 2);
}

static void test_take(void) {
    TestObject *a;
    void *slot;

    g_finalize_count = 0;
    a = (TestObject *)feng_object_new(&test_object_descriptor);
    slot = NULL;
    feng_assign(&slot, a);
    ASSERT(a->header.refcount == 2U);

    {
        void *taken = feng_take(&slot);

        ASSERT(taken == a);
        ASSERT(slot == NULL);
        /* refcount unchanged: take transfers the +1 to the caller. */
        ASSERT(a->header.refcount == 2U);

        feng_release(taken);
    }
    ASSERT(a->header.refcount == 1U);
    feng_release(a);
    ASSERT(g_finalize_count == 1);
}

static void test_string_literal_immortal(void) {
    FengString *a = feng_string_literal("hello", 5);
    FengString *b = feng_string_literal("", 0);

    ASSERT(feng_string_length(a) == 5U);
    ASSERT(memcmp(feng_string_data(a), "hello", 5) == 0);
    ASSERT(feng_string_data(a)[5] == '\0');

    ASSERT(feng_string_length(b) == 0U);
    ASSERT(feng_string_data(b)[0] == '\0');

    /* Literal refcount must remain immortal across retain/release cycles. */
    feng_retain(a);
    feng_release(a);
    feng_release(a);
    ASSERT(feng_string_length(a) == 5U);
}

static void test_string_concat(void) {
    FengString *hello = feng_string_literal("hello, ", 7);
    FengString *world = feng_string_literal("world", 5);
    FengString *combined = feng_string_concat(hello, world);

    ASSERT(feng_string_length(combined) == 12U);
    ASSERT(memcmp(feng_string_data(combined), "hello, world", 12) == 0);
    ASSERT(feng_string_data(combined)[12] == '\0');
    feng_release(combined);

    /* NULL operands behave as the empty string. */
    combined = feng_string_concat(NULL, world);
    ASSERT(feng_string_length(combined) == 5U);
    feng_release(combined);

    combined = feng_string_concat(hello, NULL);
    ASSERT(feng_string_length(combined) == 7U);
    feng_release(combined);

    combined = feng_string_concat(NULL, NULL);
    ASSERT(feng_string_length(combined) == 0U);
    feng_release(combined);
}

static const FengTypeDescriptor i32_element_descriptor = {
    .name = "i32",
    .size = sizeof(int32_t),
    .finalizer = NULL,
};

static void test_array_primitive(void) {
    FengArray *array = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 4U);
    int32_t *items;
    size_t i;

    ASSERT(feng_array_length(array) == 4U);
    items = (int32_t *)feng_array_data(array);
    ASSERT(items != NULL);
    for (i = 0U; i < 4U; ++i) {
        ASSERT(items[i] == 0);
    }
    items[0] = 10;
    items[3] = 30;
    ASSERT(items[0] == 10);
    ASSERT(items[3] == 30);

    feng_array_check_index(array, 0U);
    feng_array_check_index(array, 3U);

    feng_release(array);
}

static void test_array_managed_releases_elements(void) {
    FengArray *array;
    void **slots;
    TestObject *a;
    TestObject *b;

    g_finalize_count = 0;
    array = feng_array_new(&test_object_descriptor, sizeof(void *), true, 2U);
    slots = (void **)feng_array_data(array);

    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);
    feng_assign(&slots[0], a);
    feng_assign(&slots[1], b);
    feng_release(a);
    feng_release(b);
    /* Now only the array holds a reference; finalize_count is still 0. */
    ASSERT(g_finalize_count == 0);

    feng_release(array);
    ASSERT(g_finalize_count == 2);
}

static void test_array_zero_length(void) {
    FengArray *array = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 0U);

    ASSERT(feng_array_length(array) == 0U);
    ASSERT(feng_array_data(array) == NULL);
    feng_release(array);
}

static void test_exception_caught(void) {
    FengExceptionFrame frame;
    int caught = 0;
    void *value = NULL;

    feng_exception_push(&frame);
    if (setjmp(frame.jb) == 0) {
        feng_exception_throw((void *)(uintptr_t)0x1234, 0);
        ASSERT(0);
    } else {
        caught = 1;
        value = frame.value;
        feng_exception_pop();
    }
    ASSERT(caught == 1);
    ASSERT(value == (void *)(uintptr_t)0x1234);
    ASSERT(feng_exception_current() == NULL);
}

static void test_exception_managed_value_caught(void) {
    FengExceptionFrame frame;
    TestObject *thrown;

    g_finalize_count = 0;
    thrown = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_exception_push(&frame);
    if (setjmp(frame.jb) == 0) {
        feng_exception_throw(thrown, 1);
        ASSERT(0);
    } else {
        ASSERT(frame.value == thrown);
        ASSERT(frame.is_managed == 1);
        feng_exception_pop();
        feng_release(thrown);
    }
    ASSERT(g_finalize_count == 1);
}

static void test_exception_nested_propagation(void) {
    FengExceptionFrame outer;
    FengExceptionFrame inner;
    int outer_caught = 0;

    feng_exception_push(&outer);
    if (setjmp(outer.jb) == 0) {
        feng_exception_push(&inner);
        if (setjmp(inner.jb) == 0) {
            feng_exception_throw((void *)(uintptr_t)0xABCD, 0);
            ASSERT(0);
        } else {
            void *value = inner.value;

            /* Inner caught — pop and rethrow up to outer. */
            feng_exception_pop();
            feng_exception_throw(value, 0);
            ASSERT(0);
        }
    } else {
        outer_caught = 1;
        ASSERT(outer.value == (void *)(uintptr_t)0xABCD);
        feng_exception_pop();
    }
    ASSERT(outer_caught == 1);
    ASSERT(feng_exception_current() == NULL);
}

/* --- Finalizer resurrection (Phase 1B-1) ------------------------------- */

/* Test fixture: a descriptor whose finalizer republishes `self` into a global
 * slot, simulating user code that accidentally (or intentionally) revives the
 * object from within its own finalizer. */
static void *g_resurrect_slot = NULL;
static int   g_resurrect_calls = 0;
static int   g_resurrect_remaining = 0; /* how many more times to resurrect */

static void resurrect_finalizer(void *self) {
    ++g_resurrect_calls;
    if (g_resurrect_remaining > 0) {
        --g_resurrect_remaining;
        /* Republish self by retaining; the slot is a long-lived global. */
        g_resurrect_slot = feng_retain(self);
    }
}

static const FengTypeDescriptor resurrect_descriptor = {
    .name = "test.Resurrect",
    .size = sizeof(TestObject),
    .finalizer = resurrect_finalizer,
};

static void test_finalizer_resurrection_then_release(void) {
    TestObject *obj;

    g_resurrect_slot = NULL;
    g_resurrect_calls = 0;
    g_resurrect_remaining = 1;

    obj = (TestObject *)feng_object_new(&resurrect_descriptor);
    ASSERT(obj->header.refcount == 1U);

    /* Drop last reference; finalizer fires, republishes self -> resurrected. */
    feng_release(obj);
    ASSERT(g_resurrect_calls == 1);
    ASSERT(g_resurrect_slot == obj);
    ASSERT(obj->header.refcount == 1U);

    /* Drop the resurrected reference; this time finalizer does NOT resurrect,
     * so the object must actually be freed and the finalizer must run again. */
    g_resurrect_slot = NULL;
    feng_release(obj);
    ASSERT(g_resurrect_calls == 2);
}

static void test_finalizer_resurrection_reruns_on_next_release(void) {
    TestObject *obj;
    int i;

    g_resurrect_slot = NULL;
    g_resurrect_calls = 0;
    g_resurrect_remaining = 3; /* resurrect three times */

    obj = (TestObject *)feng_object_new(&resurrect_descriptor);

    /* Each release should trigger another finalizer run while resurrections
     * remain; once they run out the object is finally freed. */
    for (i = 0; i < 3; ++i) {
        feng_release(obj);
        ASSERT(g_resurrect_calls == i + 1);
        ASSERT(g_resurrect_slot == obj);
        ASSERT(obj->header.refcount == 1U);
        g_resurrect_slot = NULL;
    }

    /* 4th release: g_resurrect_remaining == 0, no resurrection -> free. */
    feng_release(obj);
    ASSERT(g_resurrect_calls == 4);
}

static void test_finalizer_no_resurrection_releases(void) {
    TestObject *obj;

    /* Reuses test_object_descriptor which has a plain non-resurrecting
     * finalizer; verifies the new re-check path still frees in the common
     * case. */
    g_finalize_count = 0;
    obj = (TestObject *)feng_object_new(&test_object_descriptor);
    feng_release(obj);
    ASSERT(g_finalize_count == 1);
}

/* --- Phase 1B-3 cycle collector tests ----------------------------------
 *
 * The collector internal API lives in src/runtime/feng_runtime_internal.h
 * (not part of the public ABI), but the test binary links the same runtime
 * objects so we may include the internal header directly. */
#include "runtime/feng_runtime_internal.h"

/* A two-field finalizer-less node type: each instance can hold up to two
 * managed children. We use this to construct arbitrary cyclic graphs.
 * `is_potentially_cyclic = true` so feng_release routes instances through
 * the candidate buffer. */
typedef struct CycNode {
    FengManagedHeader header;
    void *child_a;
    void *child_b;
} CycNode;

static const FengManagedFieldDescriptor cyc_node_fields[] = {
    { offsetof(CycNode, child_a), NULL },
    { offsetof(CycNode, child_b), NULL },
};

static const FengTypeDescriptor cyc_node_descriptor = {
    .name = "test.CycNode",
    .size = sizeof(CycNode),
    .finalizer = NULL,
    .is_potentially_cyclic = true,
    .managed_field_count = sizeof(cyc_node_fields) / sizeof(cyc_node_fields[0]),
    .managed_fields = cyc_node_fields,
};

/* Same shape but advertises a user finalizer so we can verify that 1B-3
 * abandons cycles containing finalizers (they leak rather than crash; 1B-4
 * will replace this branch with two-phase finalizer collection). */
static int g_cyc_fin_count = 0;
static void cyc_node_fin_finalize(void *self) {
    (void)self;
    ++g_cyc_fin_count;
}

static const FengTypeDescriptor cyc_node_fin_descriptor = {
    .name = "test.CycNodeFin",
    .size = sizeof(CycNode),
    .finalizer = cyc_node_fin_finalize,
    .is_potentially_cyclic = true,
    .managed_field_count = sizeof(cyc_node_fields) / sizeof(cyc_node_fields[0]),
    .managed_fields = cyc_node_fields,
};

/* Build a fresh CycNode with refcount = 1. */
static CycNode *cyc_new(const FengTypeDescriptor *desc) {
    CycNode *n = (CycNode *)feng_object_new(desc);
    return n;
}

/* Manually link `parent->child_X = child` taking a +1 reference on child. */
static void cyc_link(void **slot, void *child) {
    *slot = feng_retain(child);
}

static void test_cycle_collector_reclaims_two_node_cycle(void) {
    /* Build A <-> B, then drop external references so only the internal
     * cycle remains. The collector must free both. */
    CycNode *a = cyc_new(&cyc_node_descriptor);
    CycNode *b = cyc_new(&cyc_node_descriptor);

    cyc_link(&a->child_a, b); /* a holds +1 on b -> b.rc = 2 */
    cyc_link(&b->child_a, a); /* b holds +1 on a -> a.rc = 2 */

    /* Drop external refs. Each release brings rc back to 1 (cycle internal
     * refs remain) and enqueues the candidate. */
    feng_release(a);
    feng_release(b);

    /* Force collection irrespective of threshold. */
    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();

    /* If we got here without a use-after-free / leak crash, the cycle was
     * reclaimed. We can't assert "memory was freed" without a tracker; the
     * shutdown call below would catch double-frees on the candidate buffer. */
    feng_cycle_runtime_shutdown();
}

static void test_cycle_collector_does_not_collect_externally_referenced(void) {
    /* Same A <-> B cycle, but keep an external reference on A. The
     * collector must NOT free either. */
    CycNode *a = cyc_new(&cyc_node_descriptor);
    CycNode *b = cyc_new(&cyc_node_descriptor);

    cyc_link(&a->child_a, b); /* a.rc=1, b.rc=2 */
    cyc_link(&b->child_a, a); /* a.rc=2, b.rc=2 */

    /* Drop b's external ref. b's true rc becomes 1 (only a holds it).
     * a's true rc remains 2 (test local + b's link). */
    feng_release(b);

    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();

    /* After collection, scan must have classified a and b as BLACK
     * (externally reachable through the test-local ref on a) and restored
     * both refcounts. */
    ASSERT(a->header.refcount == 2U);
    ASSERT(b->header.refcount == 1U);

    /* Tear down without leaking: cyc_node has no user finalizer, so we
     * manually clear every internal link before dropping references. */
    void *b_via_a = a->child_a; a->child_a = NULL;
    void *a_via_b = b->child_a; b->child_a = NULL;
    feng_release(a_via_b); /* a.rc 2 -> 1 (external still held) */
    feng_release(b_via_a); /* b.rc 1 -> 0 -> freed */
    feng_release(a);       /* a.rc 1 -> 0 -> freed */

    feng_cycle_runtime_shutdown();
}

static void test_cycle_collector_reclaims_cycle_with_finalizer(void) {
    /* Build A <-> B with finalizer-bearing descriptor. 1B-4 must invoke
     * each finalizer exactly once and free both nodes (no resurrection). */
    g_cyc_fin_count = 0;
    CycNode *a = cyc_new(&cyc_node_fin_descriptor);
    CycNode *b = cyc_new(&cyc_node_fin_descriptor);

    cyc_link(&a->child_a, b);
    cyc_link(&b->child_a, a);

    feng_release(a);
    feng_release(b);

    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();

    /* Both finalizers ran exactly once; memory has been freed. (We cannot
     * dereference a or b — they are gone. A leak / UAF would be caught by
     * the shutdown drain below or by a subsequent test's allocator state.) */
    ASSERT(g_cyc_fin_count == 2);

    feng_cycle_runtime_shutdown();
}

/* --- Phase 1B-4 resurrection tests ------------------------------------- */

/* Slots and counters used by the resurrecting finalizers below. The
 * finalizers consult `g_cyc_res_*_enabled` so we can disable resurrection
 * during teardown to avoid an infinite resurrection loop. */
static CycNode *g_cyc_res_slot_self = NULL;
static int g_cyc_res_self_fin_count = 0;
static bool g_cyc_res_self_enabled = false;

static void cyc_node_res_self_finalize(void *self) {
    ++g_cyc_res_self_fin_count;
    if (!g_cyc_res_self_enabled) {
        return;
    }
    /* Resurrect self by publishing into a global and bumping the rc. */
    g_cyc_res_slot_self = (CycNode *)self;
    feng_retain(self);
}

static const FengTypeDescriptor cyc_node_res_self_descriptor = {
    .name = "test.CycNodeResSelf",
    .size = sizeof(CycNode),
    .finalizer = cyc_node_res_self_finalize,
    .is_potentially_cyclic = true,
    .managed_field_count = sizeof(cyc_node_fields) / sizeof(cyc_node_fields[0]),
    .managed_fields = cyc_node_fields,
};

static void test_cycle_collector_finalizer_resurrects_self(void) {
    /* A <-> B cycle. A's finalizer resurrects A by publishing to a global.
     * Per §13.2 BFS propagation, B (held by A) also survives.
     * After collection: both A and B are BLACK with restored refcounts. */
    g_cyc_res_self_fin_count = 0;
    g_cyc_fin_count = 0;
    g_cyc_res_slot_self = NULL;
    g_cyc_res_self_enabled = true;

    CycNode *a = cyc_new(&cyc_node_res_self_descriptor);
    CycNode *b = cyc_new(&cyc_node_fin_descriptor);

    cyc_link(&a->child_a, b);
    cyc_link(&b->child_a, a);

    feng_release(a); /* drop external refs; both become candidates */
    feng_release(b);

    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();

    /* Both finalizers ran exactly once. */
    ASSERT(g_cyc_res_self_fin_count == 1);
    ASSERT(g_cyc_fin_count == 1);
    ASSERT(g_cyc_res_slot_self == a);
    /* A and B both survive (BFS from A reached B via a->child_a). Cycle
     * links are preserved because both endpoints survive. */
    ASSERT(a->child_a == b);
    ASSERT(b->child_a == a);
    /* a.rc = global(1) + b->child_a(1) = 2; b.rc = a->child_a(1) = 1. */
    ASSERT(a->header.refcount == 2U);
    ASSERT(b->header.refcount == 1U);

    /* Teardown: disable resurrection, then break the cycle and drop refs. */
    g_cyc_res_self_enabled = false;
    g_cyc_res_slot_self = NULL;
    void *b_via_a = a->child_a; a->child_a = NULL;
    void *a_via_b = b->child_a; b->child_a = NULL;
    feng_release(a_via_b); /* a.rc 2 -> 1 */
    feng_release(b_via_a); /* b.rc 1 -> 0 -> finalizer + free */
    feng_release(a);       /* a.rc 1 -> 0 -> finalizer (no-op) + free */
    ASSERT(g_cyc_res_self_fin_count == 2); /* +1 from final release */
    ASSERT(g_cyc_fin_count == 2);          /* +1 from b's free */

    feng_cycle_runtime_shutdown();
}

/* Resurrect-partner finalizer: when armed, publishes a designated target
 * into a global. This is mounted on a dedicated descriptor so we can build
 * a topology where only the target is reachable as a survivor seed. */
static CycNode *g_cyc_res_partner_target = NULL;
static CycNode *g_cyc_res_slot_partner = NULL;
static int g_cyc_res_partner_fin_count = 0;
static bool g_cyc_res_partner_enabled = false;

static void cyc_node_res_partner_finalize(void *self) {
    (void)self;
    ++g_cyc_res_partner_fin_count;
    if (!g_cyc_res_partner_enabled || g_cyc_res_partner_target == NULL) {
        return;
    }
    g_cyc_res_slot_partner = g_cyc_res_partner_target;
    feng_retain(g_cyc_res_partner_target);
}

static const FengTypeDescriptor cyc_node_res_partner_descriptor = {
    .name = "test.CycNodeResPartner",
    .size = sizeof(CycNode),
    .finalizer = cyc_node_res_partner_finalize,
    .is_potentially_cyclic = true,
    .managed_field_count = sizeof(cyc_node_fields) / sizeof(cyc_node_fields[0]),
    .managed_fields = cyc_node_fields,
};

static void test_cycle_collector_partial_resurrection_frees_unsurvived(void) {
    /* Topology: cycle A <-> B (both finalizer-bearing), plus a leaf D
     * reachable only from A via a->child_b. D has no outgoing managed
     * refs. A's finalizer (res-partner) resurrects D when armed. B has a
     * plain finalizer.
     *
     * Pre-collection refs:
     *   A.rc = 2 (test-local + B->child_a)
     *   B.rc = 2 (test-local + A->child_a)
     *   D.rc = 1 (A->child_b)
     *
     * After dropping external refs on A and B, the cycle becomes garbage
     * and D is reachable only from white A → D joins the white set too.
     * A's finalizer publishes D externally; A and B do NOT resurrect
     * themselves.
     *
     * Phase 1.5: D.rc(1, from global) > intra_in(1 from A) ? 1 > 1 = false.
     *
     * Wait — both A→D and the global ref give D rc=2 post Phase 1, with
     * intra_in[D]=1 (A still points to D). 2 > 1, so D is a survivor seed.
     * D has no children, so BFS stops. A and B are not survived → freed.
     * Sanitise: A's child_b slot points to D (survivor). When freeing A,
     * the free→survivor pass dec's D.rc by 1. After collection: D.rc = 1
     * (just the global). */
    g_cyc_res_partner_fin_count = 0;
    g_cyc_fin_count = 0;
    g_cyc_res_slot_partner = NULL;
    g_cyc_res_partner_target = NULL;
    g_cyc_res_partner_enabled = false;

    CycNode *a = cyc_new(&cyc_node_res_partner_descriptor);
    CycNode *b = cyc_new(&cyc_node_fin_descriptor);
    CycNode *d = cyc_new(&cyc_node_fin_descriptor);

    cyc_link(&a->child_a, b); /* A -> B */
    cyc_link(&b->child_a, a); /* B -> A (closes cycle) */
    cyc_link(&a->child_b, d); /* A -> D (leaf) */

    /* We hold one external on D to simulate "D is referenced only via A"?
     * No — we want D to be unreachable except via the cycle. Drop our
     * external on D so its rc reflects only A's link. */
    feng_release(d); /* d.rc 2 -> 1 (only A->child_b remains) */

    g_cyc_res_partner_target = d;
    g_cyc_res_partner_enabled = true;

    feng_release(a); /* a.rc 2 -> 1, enqueued */
    feng_release(b); /* b.rc 2 -> 1, enqueued */

    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();

    /* A's finalizer ran (and resurrected D). B's finalizer ran. D may or
     * may not have had its finalizer run before resurrection-classification
     * — current implementation runs ALL whites' finalizers in Phase 1
     * regardless of whether they will survive. So D.fin also ran. */
    ASSERT(g_cyc_res_partner_fin_count == 1);
    /* B is fin-desc (g_cyc_fin_count++); D is also fin-desc and is a white
     * member, so its finalizer also ran. Total: B + D = 2. */
    ASSERT(g_cyc_fin_count == 2);
    ASSERT(g_cyc_res_slot_partner == d);
    /* D survived; its rc reflects only the global hold (A->D edge died
     * with A). */
    ASSERT(d->header.refcount == 1U);

    /* Teardown: disable, drop the resurrection ref. */
    g_cyc_res_partner_enabled = false;
    g_cyc_res_partner_target = NULL;
    g_cyc_res_slot_partner = NULL;
    feng_release(d); /* d.rc 1 -> 0 -> finalizer + free. */
    ASSERT(g_cyc_fin_count == 3);

    feng_cycle_runtime_shutdown();
}

static void test_cycle_collector_acyclic_object_never_enqueued(void) {
    /* Acyclic descriptor: feng_release must take the ARC fast path and NEVER
     * acquire the cycle mutex. We can verify the latter indirectly by
     * confirming feng_cycle_collect_locked has nothing to do after a normal
     * retain/release cycle on test_object_descriptor (which has
     * is_potentially_cyclic == false). */
    g_finalize_count = 0;
    TestObject *o = (TestObject *)feng_object_new(&test_object_descriptor);
    feng_retain(o);
    feng_release(o);
    feng_release(o);
    ASSERT(g_finalize_count == 1);

    feng_cycle_lock();
    feng_cycle_collect_locked(); /* must be a no-op (empty buffer) */
    feng_cycle_unlock();
}

/* --- Finalizer exception escape (docs/feng-lifetime.md §13.2) ---------- */

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* A finalizer that throws an unmanaged exception value and never catches it.
 * The runtime barrier in feng_finalizer_invoke must intercept the throw and
 * panic; the process must exit via abort() (SIGABRT). */
static void throwing_finalizer(void *self) {
    (void)self;
    feng_exception_throw((void *)"finalizer-throw", 0);
}

static const FengTypeDescriptor throwing_descriptor = {
    .name = "test.Throwing",
    .size = sizeof(TestObject),
    .finalizer = throwing_finalizer,
    /* Acyclic: forces the ARC release path. */
    .is_potentially_cyclic = false,
};

static const FengTypeDescriptor throwing_cyclic_descriptor = {
    .name = "test.ThrowingCyc",
    .size = sizeof(CycNode),
    .finalizer = throwing_finalizer,
    .is_potentially_cyclic = true,
    .managed_field_count = sizeof(cyc_node_fields) / sizeof(cyc_node_fields[0]),
    .managed_fields = cyc_node_fields,
};

/* Run `body` in a forked child and assert it terminated via SIGABRT. The
 * parent process is unaffected so subsequent tests still execute normally.
 * stderr from the child is silenced so the test log only shows pass/fail. */
static void assert_child_aborts(void (*body)(void)) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        /* Child: silence the panic message so test output stays clean. */
        FILE *null_err = freopen("/dev/null", "w", stderr);
        (void)null_err;
        body();
        /* Body returned without aborting — that itself is a failure. */
        _exit(99);
    }
    int status = 0;
    pid_t got = waitpid(pid, &status, 0);
    ASSERT(got == pid);
    ASSERT(WIFSIGNALED(status));
    ASSERT(WTERMSIG(status) == SIGABRT);
}

static void arc_throw_body(void) {
    TestObject *o = (TestObject *)feng_object_new(&throwing_descriptor);
    feng_release(o); /* triggers throwing_finalizer */
}

static void cycle_throw_body(void) {
    /* Build a 2-node cycle whose nodes carry the throwing finalizer; force
     * collection so Phase 1 invokes the finalizer through the cycle path. */
    CycNode *a = (CycNode *)feng_object_new(&throwing_cyclic_descriptor);
    CycNode *b = (CycNode *)feng_object_new(&throwing_cyclic_descriptor);
    a->child_a = feng_retain(b);
    b->child_a = feng_retain(a);
    feng_release(a);
    feng_release(b);
    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();
}

static void test_finalizer_throw_on_arc_path_aborts(void) {
    assert_child_aborts(arc_throw_body);
}

static void test_finalizer_throw_on_cycle_path_aborts(void) {
    assert_child_aborts(cycle_throw_body);
}

/* --- Threshold-triggered collection ------------------------------------ */

/* Verify that feng_release-induced threshold trigger actually collects the
 * cycle without requiring an explicit feng_cycle_collect_locked() call.
 * Uses the test-only threshold setter to force collection on the very next
 * candidate enqueue. */
static void test_cycle_collector_threshold_triggers_collection(void) {
    g_cyc_fin_count = 0;

    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(1U);
    feng_cycle_unlock();

    /* Build a 2-node cycle of finalizer-bearing nodes. */
    CycNode *a = (CycNode *)feng_object_new(&cyc_node_fin_descriptor);
    CycNode *b = (CycNode *)feng_object_new(&cyc_node_fin_descriptor);
    a->child_a = feng_retain(b); /* a -> b */
    b->child_a = feng_retain(a); /* b -> a */

    /* Drop our external refs. The first feng_release puts a node into the
     * candidate buffer (a still has b's link, so it survives the dec) and
     * threshold=1 triggers collection inline. The cycle is not yet closed
     * from the collector's POV (b still externally held), so this first
     * collection must NOT free anything. */
    feng_release(a);
    ASSERT(g_cyc_fin_count == 0);

    /* Drop the second external. Now release dec's b's rc to 1, enqueues b,
     * threshold=1 triggers collection — and this time the cycle is fully
     * closed so both finalizers must run and both nodes must be freed. */
    feng_release(b);
    ASSERT(g_cyc_fin_count == 2);

    /* Restore the default for subsequent tests. */
    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(256U);
    feng_cycle_unlock();
    feng_cycle_runtime_shutdown();
}

/* --- Multi-threaded retain/release stress ------------------------------ */

#include <pthread.h>

typedef struct {
    CycNode *shared;       /* externally pinned for the duration of the run */
    int      iterations;
} StressArgs;

static void *stress_worker(void *arg) {
    StressArgs *a = (StressArgs *)arg;
    for (int i = 0; i < a->iterations; ++i) {
        /* retain/release on a potentially-cyclic object exercises the
         * STW lock acquisition path. We also build and tear down a
         * short-lived 2-node cycle each iteration so the candidate
         * buffer grows and the threshold-trigger path runs concurrently
         * with other threads. */
        feng_retain(a->shared);
        feng_release(a->shared);

        CycNode *x = (CycNode *)feng_object_new(&cyc_node_descriptor);
        CycNode *y = (CycNode *)feng_object_new(&cyc_node_descriptor);
        x->child_a = feng_retain(y);
        y->child_a = feng_retain(x);
        feng_release(x);
        feng_release(y);
    }
    return NULL;
}

static void test_cycle_collector_multithreaded_stress(void) {
    /* The collector serialises on a single recursive mutex (§13.1 STW
     * model). This test spawns several writers to assert that
     * concurrent retain/release on potentially-cyclic objects plus
     * concurrent cycle creation never crashes, never double-frees, and
     * leaves the candidate buffer drainable at shutdown. */
    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(1U);
    feng_cycle_unlock();

    CycNode *shared = (CycNode *)feng_object_new(&cyc_node_descriptor);
    enum { N_THREADS = 4, ITERATIONS = 2000 };
    pthread_t tids[N_THREADS];
    StressArgs args = { .shared = shared, .iterations = ITERATIONS };

    for (int i = 0; i < N_THREADS; ++i) {
        int rc = pthread_create(&tids[i], NULL, stress_worker, &args);
        ASSERT(rc == 0);
    }
    for (int i = 0; i < N_THREADS; ++i) {
        ASSERT(pthread_join(tids[i], NULL) == 0);
    }

    /* shared survived because every retain was paired with a release and
     * we still hold the original +1. Drop it and let shutdown verify the
     * collector reaches a clean state. */
    feng_release(shared);

    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(256U);
    feng_cycle_unlock();
    feng_cycle_runtime_shutdown();
}

/* --- By-value aggregate (FengAggregateValueDescriptor) ----------------
 *
 * These tests exercise the five public APIs declared in feng_runtime.h
 * (retain / release / assign / take / default_init) against the existing
 * single-pointer primitives. The fixtures below model two shapes:
 *
 *   FatPair  — one managed pointer + one trivial int. Models a fat
 *              object-form spec (subject + witness footprint).
 *   Outer    — two pointer slots + one nested FatPair, used to verify
 *              that the walker recurses through FENG_SLOT_NESTED_AGGREGATE
 *              without exposing nesting to any caller-visible API.
 */

typedef struct FatPair {
    void *subject;       /* managed pointer slot */
    int   tag;           /* trivial */
} FatPair;

static void fat_pair_default_init(void *out) {
    FatPair *p = (FatPair *)out;
    /* Default subject is a fresh +1 object so callers observe a
     * fully-constructed value (mirrors fat spec witness/subject contract).
     * The init contract requires every managed slot to hold either NULL
     * or a +1 retained reference — we use the latter. */
    p->subject = feng_object_new(&test_object_descriptor);
    p->tag = -1;
}

static const FengAggregateDefaultInitDescriptor fat_pair_default = {
    .kind = FENG_DEFAULT_INIT_FN,
    .init_fn = fat_pair_default_init,
};

static const FengManagedSlotDescriptor fat_pair_slots[] = {
    { offsetof(FatPair, subject), FENG_SLOT_POINTER, NULL },
};

static const FengAggregateValueDescriptor fat_pair_desc = {
    .name = "test.FatPair",
    .size = sizeof(FatPair),
    .default_init = &fat_pair_default,
    .managed_slot_count = sizeof(fat_pair_slots) / sizeof(fat_pair_slots[0]),
    .managed_slots = fat_pair_slots,
};

typedef struct OuterAgg {
    void   *head;            /* managed pointer */
    FatPair inner;           /* nested aggregate */
    void   *tail;            /* managed pointer */
} OuterAgg;

static const FengManagedSlotDescriptor outer_slots[] = {
    { offsetof(OuterAgg, head),  FENG_SLOT_POINTER,         NULL },
    { offsetof(OuterAgg, inner), FENG_SLOT_NESTED_AGGREGATE, &fat_pair_desc },
    { offsetof(OuterAgg, tail),  FENG_SLOT_POINTER,         NULL },
};

static const FengAggregateDefaultInitDescriptor outer_default_zero = {
    .kind = FENG_DEFAULT_ZERO_BYTES,
    .init_fn = NULL,
};

static const FengAggregateValueDescriptor outer_desc = {
    .name = "test.OuterAgg",
    .size = sizeof(OuterAgg),
    .default_init = &outer_default_zero,
    .managed_slot_count = sizeof(outer_slots) / sizeof(outer_slots[0]),
    .managed_slots = outer_slots,
};

static void test_aggregate_retain_release_paired(void) {
    g_finalize_count = 0;
    TestObject *o = (TestObject *)feng_object_new(&test_object_descriptor);
    /* p takes ownership of the +1 from object_new. */
    FatPair p;
    p.subject = o;
    p.tag = 7;

    feng_aggregate_retain(&p, &fat_pair_desc);
    ASSERT(o->header.refcount == 2U);

    feng_aggregate_release(&p, &fat_pair_desc);
    ASSERT(o->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    /* Drop p's ownership. */
    feng_aggregate_release(&p, &fat_pair_desc);
    ASSERT(g_finalize_count == 1);
}

static void test_aggregate_retain_release_null_slot(void) {
    /* NULL pointer slots must be skipped silently. */
    FatPair p = { .subject = NULL, .tag = 0 };
    feng_aggregate_retain(&p, &fat_pair_desc);   /* no-op on NULL */
    feng_aggregate_release(&p, &fat_pair_desc);  /* no-op on NULL */
    ASSERT(p.subject == NULL);
    ASSERT(p.tag == 0);
}

static void test_aggregate_assign_disjoint(void) {
    g_finalize_count = 0;
    TestObject *a = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *b = (TestObject *)feng_object_new(&test_object_descriptor);

    /* Each FatPair takes ownership of one fresh +1 reference. */
    FatPair dst = { .subject = a, .tag = 1 };
    FatPair src = { .subject = b, .tag = 2 };

    feng_aggregate_assign(&dst, &src, &fat_pair_desc);
    /* dst now holds b (+1 freshly retained); the previous slot's +1 on a
     * was released, so a finalized. */
    ASSERT(dst.subject == b);
    ASSERT(dst.tag == 2);
    ASSERT(g_finalize_count == 1);
    ASSERT(b->header.refcount == 2U); /* src's +1 + dst's +1 */

    feng_aggregate_release(&dst, &fat_pair_desc);
    ASSERT(b->header.refcount == 1U);
    feng_aggregate_release(&src, &fat_pair_desc);
    ASSERT(g_finalize_count == 2);
}

static void test_aggregate_assign_shared_subobject(void) {
    /* Both dst and src reference the same managed object — retain-before-
     * release ordering must keep the refcount > 0 throughout. */
    g_finalize_count = 0;
    TestObject *o = (TestObject *)feng_object_new(&test_object_descriptor);
    /* Each owner needs its own +1; the original retain is dst's. */
    feng_retain(o);
    FatPair dst = { .subject = o, .tag = 11 };
    FatPair src = { .subject = o, .tag = 22 };
    ASSERT(o->header.refcount == 2U);

    feng_aggregate_assign(&dst, &src, &fat_pair_desc);
    ASSERT(dst.subject == o);
    ASSERT(dst.tag == 22);
    /* dst released its old +1 and acquired a new +1 — net change zero. */
    ASSERT(o->header.refcount == 2U);
    ASSERT(g_finalize_count == 0);

    feng_aggregate_release(&dst, &fat_pair_desc);
    feng_aggregate_release(&src, &fat_pair_desc);
    ASSERT(g_finalize_count == 1);
}

static void test_aggregate_assign_self(void) {
    /* Pointer-identity self-assign must be a no-op. */
    g_finalize_count = 0;
    TestObject *o = (TestObject *)feng_object_new(&test_object_descriptor);
    FatPair p = { .subject = o, .tag = 99 };

    feng_aggregate_assign(&p, &p, &fat_pair_desc);
    ASSERT(p.subject == o);
    ASSERT(p.tag == 99);
    ASSERT(o->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    feng_aggregate_release(&p, &fat_pair_desc);
    ASSERT(g_finalize_count == 1);
}

static void test_aggregate_take_transfers_ownership(void) {
    g_finalize_count = 0;
    TestObject *a = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *b = (TestObject *)feng_object_new(&test_object_descriptor);

    FatPair dst = { .subject = a, .tag = 1 };
    FatPair src = { .subject = b, .tag = 2 };

    feng_aggregate_take(&dst, &src, &fat_pair_desc);
    /* a was released (and finalized: it had only dst's +1); b moved into
     * dst with no refcount change; src's managed slot was nulled so a
     * subsequent release on it is a no-op. */
    ASSERT(dst.subject == b);
    ASSERT(dst.tag == 2);
    ASSERT(src.subject == NULL);
    /* take preserves non-managed bytes in src (per dev/feng-value-model
     * §5.2). */
    ASSERT(src.tag == 2);
    ASSERT(b->header.refcount == 1U);
    ASSERT(g_finalize_count == 1); /* a finalized */

    /* Releasing src now must not double-free. */
    feng_aggregate_release(&src, &fat_pair_desc);
    ASSERT(b->header.refcount == 1U);

    feng_aggregate_release(&dst, &fat_pair_desc);
    ASSERT(g_finalize_count == 2);
}

static void test_aggregate_take_self(void) {
    g_finalize_count = 0;
    TestObject *o = (TestObject *)feng_object_new(&test_object_descriptor);
    FatPair p = { .subject = o, .tag = 5 };
    feng_aggregate_take(&p, &p, &fat_pair_desc);
    ASSERT(p.subject == o);
    ASSERT(o->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);
    feng_aggregate_release(&p, &fat_pair_desc);
    ASSERT(g_finalize_count == 1);
}

static void test_aggregate_default_init_zero_bytes(void) {
    OuterAgg v;
    /* Stuff the struct so we can confirm memset clears it. */
    memset(&v, 0xA5, sizeof(v));
    feng_aggregate_default_init(&v, &outer_desc);
    ASSERT(v.head == NULL);
    ASSERT(v.tail == NULL);
    ASSERT(v.inner.subject == NULL);
    ASSERT(v.inner.tag == 0);
    /* All-NULL slots: release must remain a clean no-op. */
    feng_aggregate_release(&v, &outer_desc);
}

static void test_aggregate_default_init_fn(void) {
    g_finalize_count = 0;
    FatPair v;
    memset(&v, 0xCC, sizeof(v));
    feng_aggregate_default_init(&v, &fat_pair_desc);
    ASSERT(v.subject != NULL);
    ASSERT(v.tag == -1);
    ASSERT(((FengManagedHeader *)v.subject)->refcount == 1U);

    feng_aggregate_release(&v, &fat_pair_desc);
    ASSERT(g_finalize_count == 1);
}

static void test_aggregate_nested_walker(void) {
    /* Verify the walker recurses through FENG_SLOT_NESTED_AGGREGATE: the
     * inner FatPair's managed pointer must be retained/released alongside
     * the outer struct's own pointer slots. */
    g_finalize_count = 0;
    TestObject *h = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *m = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *t = (TestObject *)feng_object_new(&test_object_descriptor);

    OuterAgg v;
    feng_aggregate_default_init(&v, &outer_desc);
    /* Manually populate as if codegen had moved each owning +1 into the
     * slots. */
    v.head = h;
    v.inner.subject = m;
    v.inner.tag = 1;
    v.tail = t;

    feng_aggregate_retain(&v, &outer_desc);
    ASSERT(h->header.refcount == 2U);
    ASSERT(m->header.refcount == 2U);
    ASSERT(t->header.refcount == 2U);

    feng_aggregate_release(&v, &outer_desc);
    ASSERT(h->header.refcount == 1U);
    ASSERT(m->header.refcount == 1U);
    ASSERT(t->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    /* Final release drops v's ownership and frees all three. */
    feng_aggregate_release(&v, &outer_desc);
    ASSERT(g_finalize_count == 3);
}

static void test_aggregate_nested_assign(void) {
    /* assign must copy nested aggregates correctly: both inner pointer
     * and outer pointers participate. */
    g_finalize_count = 0;
    TestObject *src_head  = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *src_inner = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *src_tail  = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *dst_head  = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *dst_inner = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *dst_tail  = (TestObject *)feng_object_new(&test_object_descriptor);

    OuterAgg src = {
        .head = src_head,
        .inner = { .subject = src_inner, .tag = 11 },
        .tail = src_tail,
    };
    OuterAgg dst = {
        .head = dst_head,
        .inner = { .subject = dst_inner, .tag = 22 },
        .tail = dst_tail,
    };

    feng_aggregate_assign(&dst, &src, &outer_desc);
    ASSERT(dst.head == src_head);
    ASSERT(dst.inner.subject == src_inner);
    ASSERT(dst.inner.tag == 11);
    ASSERT(dst.tail == src_tail);
    /* The three dst originals each lost dst's +1 (their only +1 in this
     * test) and finalized; the three src originals gained dst's +1, so
     * each now sits at refcount 2. */
    ASSERT(g_finalize_count == 3);
    ASSERT(src_head->header.refcount == 2U);
    ASSERT(src_inner->header.refcount == 2U);
    ASSERT(src_tail->header.refcount == 2U);

    feng_aggregate_release(&dst, &outer_desc);
    feng_aggregate_release(&src, &outer_desc);
    ASSERT(g_finalize_count == 6);
}

int main(void) {
    test_object_retain_release();
    test_retain_release_nullsafe();
    test_assign_barrier();
    test_take();
    test_string_literal_immortal();
    test_string_concat();
    test_array_primitive();
    test_array_managed_releases_elements();
    test_array_zero_length();
    test_exception_caught();
    test_exception_managed_value_caught();
    test_exception_nested_propagation();
    test_finalizer_resurrection_then_release();
    test_finalizer_resurrection_reruns_on_next_release();
    test_finalizer_no_resurrection_releases();
    test_cycle_collector_reclaims_two_node_cycle();
    test_cycle_collector_does_not_collect_externally_referenced();
    test_cycle_collector_reclaims_cycle_with_finalizer();
    test_cycle_collector_finalizer_resurrects_self();
    test_cycle_collector_partial_resurrection_frees_unsurvived();
    test_cycle_collector_acyclic_object_never_enqueued();
    test_finalizer_throw_on_arc_path_aborts();
    test_finalizer_throw_on_cycle_path_aborts();
    test_cycle_collector_threshold_triggers_collection();
    test_cycle_collector_multithreaded_stress();

    test_aggregate_retain_release_paired();
    test_aggregate_retain_release_null_slot();
    test_aggregate_assign_disjoint();
    test_aggregate_assign_shared_subobject();
    test_aggregate_assign_self();
    test_aggregate_take_transfers_ownership();
    test_aggregate_take_self();
    test_aggregate_default_init_zero_bytes();
    test_aggregate_default_init_fn();
    test_aggregate_nested_walker();
    test_aggregate_nested_assign();

    fputs("test_runtime: ok\n", stdout);
    return 0;
}
