/* Phase 1A runtime regression suite. Each test exercises a single subsystem
 * (refcount, string, array, exception). Tests deliberately avoid relying on
 * malloc-tracking instrumentation — they assert observable behaviour and
 * cross-check finalizer counts via test-local descriptors. */
#include "runtime/feng_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

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

#define ASSERT_SCALAR_VALUE_BOX(box_type, descriptor, initial_value, other_value) \
    do {                                                                            \
        box_type *left = (box_type *)feng_object_new(&(descriptor));                \
        box_type *right = (box_type *)feng_object_new(&(descriptor));               \
                                                                                    \
        ASSERT(left != NULL);                                                       \
        ASSERT(right != NULL);                                                      \
        ASSERT(left->header.desc == &(descriptor));                                 \
        ASSERT(right->header.desc == &(descriptor));                                \
        ASSERT(left->header.tag == FENG_TYPE_TAG_OBJECT);                           \
        ASSERT(right->header.tag == FENG_TYPE_TAG_OBJECT);                          \
        ASSERT(left->header.refcount == 1U);                                        \
        ASSERT(right->header.refcount == 1U);                                       \
        ASSERT((descriptor).size == sizeof(box_type));                              \
        ASSERT((descriptor).is_potentially_cyclic == false);                        \
        ASSERT((descriptor).managed_field_count == 0U);                             \
        ASSERT((descriptor).managed_fields == NULL);                                \
        ASSERT((descriptor).equal_fn != NULL);                                      \
        ASSERT(sizeof(box_type) <= 40U);                                            \
                                                                                    \
        left->value = (initial_value);                                              \
        right->value = (initial_value);                                             \
        ASSERT(feng_spec_subject_equal(left, right));                               \
        right->value = (other_value);                                               \
        ASSERT(!feng_spec_subject_equal(left, right));                              \
                                                                                    \
        feng_release(left);                                                         \
        feng_release(right);                                                        \
    } while (0)

static void test_scalar_box_runtime_contract(void) {
    const FengTypeDescriptor *const descriptors[] = {
        &feng_value_box_bool_descriptor,
        &feng_value_box_i8_descriptor,
        &feng_value_box_i16_descriptor,
        &feng_value_box_i32_descriptor,
        &feng_value_box_i64_descriptor,
        &feng_value_box_u8_descriptor,
        &feng_value_box_u16_descriptor,
        &feng_value_box_u32_descriptor,
        &feng_value_box_u64_descriptor,
        &feng_value_box_f32_descriptor,
        &feng_value_box_f64_descriptor,
    };
    FengValueBox__i32 *i32_box;
    FengValueBox__f32 *f32_box;
    FengValueBox__f64 *positive_zero_box;
    FengValueBox__f64 *negative_zero_box;
    FengValueBox__f64 *nan_left_box;
    FengValueBox__f64 *nan_right_box;
    uint64_t nan_bits = UINT64_C(0x7ff8000000000042);
    double nan_value;

    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__bool, feng_value_box_bool_descriptor, true, false);
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__i8, feng_value_box_i8_descriptor, INT8_C(-7), INT8_C(8));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__i16, feng_value_box_i16_descriptor, INT16_C(-70), INT16_C(80));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__i32, feng_value_box_i32_descriptor, INT32_C(-700), INT32_C(800));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__i64, feng_value_box_i64_descriptor, INT64_C(-7000), INT64_C(8000));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__u8, feng_value_box_u8_descriptor, UINT8_C(7), UINT8_C(8));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__u16, feng_value_box_u16_descriptor, UINT16_C(70), UINT16_C(80));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__u32, feng_value_box_u32_descriptor, UINT32_C(700), UINT32_C(800));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__u64, feng_value_box_u64_descriptor, UINT64_C(7000), UINT64_C(8000));
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__f32, feng_value_box_f32_descriptor, 1.5F, 2.5F);
    ASSERT_SCALAR_VALUE_BOX(
        FengValueBox__f64, feng_value_box_f64_descriptor, 1.5, 2.5);

    for (size_t i = 0U; i < sizeof(descriptors) / sizeof(descriptors[0]); ++i) {
        for (size_t j = i + 1U; j < sizeof(descriptors) / sizeof(descriptors[0]); ++j) {
            ASSERT(descriptors[i] != descriptors[j]);
        }
    }

    i32_box = (FengValueBox__i32 *)feng_object_new(&feng_value_box_i32_descriptor);
    f32_box = (FengValueBox__f32 *)feng_object_new(&feng_value_box_f32_descriptor);
    ASSERT(i32_box != NULL);
    ASSERT(f32_box != NULL);
    i32_box->value = 1;
    f32_box->value = 1.0F;
    ASSERT(!feng_spec_subject_equal(i32_box, f32_box));
    feng_release(i32_box);
    feng_release(f32_box);

    positive_zero_box =
        (FengValueBox__f64 *)feng_object_new(&feng_value_box_f64_descriptor);
    negative_zero_box =
        (FengValueBox__f64 *)feng_object_new(&feng_value_box_f64_descriptor);
    ASSERT(positive_zero_box != NULL);
    ASSERT(negative_zero_box != NULL);
    positive_zero_box->value = 0.0;
    negative_zero_box->value = -0.0;
    ASSERT(!feng_spec_subject_equal(positive_zero_box, negative_zero_box));
    feng_release(positive_zero_box);
    feng_release(negative_zero_box);

    memcpy(&nan_value, &nan_bits, sizeof(nan_value));
    nan_left_box =
        (FengValueBox__f64 *)feng_object_new(&feng_value_box_f64_descriptor);
    nan_right_box =
        (FengValueBox__f64 *)feng_object_new(&feng_value_box_f64_descriptor);
    ASSERT(nan_left_box != NULL);
    ASSERT(nan_right_box != NULL);
    nan_left_box->value = nan_value;
    nan_right_box->value = nan_value;
    ASSERT(feng_spec_subject_equal(nan_left_box, nan_right_box));
    feng_release(nan_left_box);
    feng_release(nan_right_box);
}

#undef ASSERT_SCALAR_VALUE_BOX

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

static void test_string_utf8_length_contract(void) {
    FengString *ascii = feng_string_literal("hello", 5U);
    FengString *utf8 = feng_string_literal("你好", 6U);

    ASSERT(feng_string_utf8_length(ascii) == 5);
    ASSERT(feng_string_utf8_length(utf8) == 6);
    ASSERT(feng_string_utf8_length(NULL) == 0);
}

static const FengTypeDescriptor i32_element_descriptor = {
    .name = "i32",
    .size = sizeof(int32_t),
    .finalizer = NULL,
};
static const FengGenericParamDescriptor i32_runtime_generic_descriptor = {
    .kind = FENG_VALUE_TRIVIAL,
    .descriptor = &feng_i32_descriptor,
    .witness = NULL,
};
static const FengGenericParamDescriptor f64_runtime_generic_descriptor = {
    .kind = FENG_VALUE_TRIVIAL,
    .descriptor = &feng_f64_descriptor,
    .witness = NULL,
};
static const FengGenericParamDescriptor object_runtime_generic_descriptor = {
    .kind = FENG_VALUE_MANAGED_POINTER,
    .descriptor = &test_object_descriptor,
    .witness = NULL,
};
static const FengGenericParamDescriptor string_runtime_generic_descriptor = {
    .kind = FENG_VALUE_MANAGED_POINTER,
    .descriptor = &feng_string_descriptor,
    .witness = NULL,
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

static void test_array_payload_alignment(void) {
    FengArray *trivial = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 3U);
    FengArray *managed = feng_array_new(&test_object_descriptor, sizeof(void *), true, 2U);
    void *trivial_data = feng_array_data(trivial);
    void *managed_data = feng_array_data(managed);
    uintptr_t align_mask = (uintptr_t)_Alignof(max_align_t) - (uintptr_t)1U;

    ASSERT(trivial_data != NULL);
    ASSERT(managed_data != NULL);
    ASSERT((((uintptr_t)trivial_data) & align_mask) == 0U);
    ASSERT((((uintptr_t)managed_data) & align_mask) == 0U);

    feng_release(managed);
    feng_release(trivial);
}

static void test_array_length_contract_uses_descriptor(void) {
    FengArray *array = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 4U);

    ASSERT(feng_array_get_length(&i32_runtime_generic_descriptor, array) == 4);
    ASSERT(feng_array_get_length(&i32_runtime_generic_descriptor, NULL) == 0);

    feng_release(array);
}

static void test_array_slice_trivial_copies_subrange(void) {
    FengArray *source = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 4U);
    int32_t *source_items = (int32_t *)feng_array_data(source);
    FengArray *slice;
    FengArray *empty;
    int32_t *slice_items;

    source_items[0] = 10;
    source_items[1] = 20;
    source_items[2] = 30;
    source_items[3] = 40;

    slice = feng_array_slice(&i32_runtime_generic_descriptor, source, 1, 2);
    slice_items = (int32_t *)feng_array_data(slice);

    ASSERT(feng_array_length(slice) == 2U);
    ASSERT(feng_array_element_kind(slice) == FENG_VALUE_TRIVIAL);
    ASSERT(slice_items != NULL);
    ASSERT(slice_items[0] == 20);
    ASSERT(slice_items[1] == 30);

    slice_items[0] = 99;
    ASSERT(source_items[1] == 20);

    empty = feng_array_slice(&i32_runtime_generic_descriptor, source, 2, 0);
    ASSERT(feng_array_length(empty) == 0U);
    ASSERT(feng_array_data(empty) == NULL);

    feng_release(empty);
    feng_release(slice);
    feng_release(source);
}

static void test_array_slice_managed_pointer_retains_elements(void) {
    FengArray *source;
    FengArray *slice;
    void **source_slots;
    void **slice_slots;
    TestObject *a;
    TestObject *b;
    TestObject *c;
    TestObject *sliced_b;
    TestObject *sliced_c;

    g_finalize_count = 0;
    source = feng_array_new(&test_object_descriptor, sizeof(void *), true, 3U);
    source_slots = (void **)feng_array_data(source);

    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);
    c = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_assign(&source_slots[0], a);
    feng_assign(&source_slots[1], b);
    feng_assign(&source_slots[2], c);
    feng_release(a);
    feng_release(b);
    feng_release(c);

    slice = feng_array_slice(&object_runtime_generic_descriptor, source, 1, 2);
    slice_slots = (void **)feng_array_data(slice);
    sliced_b = (TestObject *)slice_slots[0];
    sliced_c = (TestObject *)slice_slots[1];

    ASSERT(feng_array_length(slice) == 2U);
    ASSERT(feng_array_element_kind(slice) == FENG_VALUE_MANAGED_POINTER);
    ASSERT(sliced_b == source_slots[1]);
    ASSERT(sliced_c == source_slots[2]);
    ASSERT(sliced_b->header.refcount == 2U);
    ASSERT(sliced_c->header.refcount == 2U);

    feng_release(source);
    ASSERT(g_finalize_count == 1);
    ASSERT(sliced_b->header.refcount == 1U);
    ASSERT(sliced_c->header.refcount == 1U);

    feng_release(slice);
    ASSERT(g_finalize_count == 3);
}

static void test_expression_equal_contract_uses_descriptor(void) {
    int32_t left_i32 = 10;
    int32_t right_i32 = 10;
    int32_t other_i32 = 30;
    double positive_zero = 0.0;
    double negative_zero = -0.0;
    double nan_value = NAN;
    FengString *left_string = feng_string_literal("hello", 5U);
    FengString *right_string = feng_string_concat(feng_string_literal("he", 2U),
                                                  feng_string_literal("llo", 3U));
    FengString *other_string = feng_string_literal("world", 5U);
    TestObject *left_object = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *same_object = left_object;
    TestObject *other_object = (TestObject *)feng_object_new(&test_object_descriptor);

    ASSERT(feng_expression_equal(&i32_runtime_generic_descriptor, &left_i32, &right_i32));
    ASSERT(!feng_expression_equal(&i32_runtime_generic_descriptor, &left_i32, &other_i32));
    ASSERT(feng_expression_equal(&f64_runtime_generic_descriptor, &positive_zero, &negative_zero));
    ASSERT(!feng_expression_equal(&f64_runtime_generic_descriptor, &nan_value, &nan_value));

    ASSERT(feng_expression_equal(&string_runtime_generic_descriptor,
                                 &left_string,
                                 &right_string));
    ASSERT(!feng_expression_equal(&string_runtime_generic_descriptor,
                                  &left_string,
                                  &other_string));

    ASSERT(feng_expression_equal(&object_runtime_generic_descriptor,
                                 &left_object,
                                 &same_object));
    ASSERT(!feng_expression_equal(&object_runtime_generic_descriptor,
                                  &left_object,
                                  &other_object));

    feng_release(other_object);
    feng_release(left_object);
    feng_release(right_string);
}

static void test_array_slice_aggregate_assigns_elements(void);

static void test_frame_marker_release_to_try_marker(void) {
    FengFrameMarker function_marker;
    FengFrameMarker try_marker;
    FengCleanupNode node;
    TestObject *local;

    g_finalize_count = 0;
    local = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_frame_push(&function_marker);
    feng_try_frame_push(&try_marker);
    feng_cleanup_push(&node, (void **)&local);
    feng_frame_release_to(&try_marker);

    ASSERT(local == NULL);
    ASSERT(g_finalize_count == 1);
    feng_frame_pop();
}

static void test_frame_marker_release_to_try_preserves_outer_cleanup(void) {
    FengFrameMarker function_marker;
    FengFrameMarker try_marker;
    FengCleanupNode outer_node;
    FengCleanupNode inner_node;
    TestObject *outer;
    TestObject *inner;

    g_finalize_count = 0;
    outer = (TestObject *)feng_object_new(&test_object_descriptor);
    inner = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_frame_push(&function_marker);
    feng_cleanup_push(&outer_node, (void **)&outer);
    feng_try_frame_push(&try_marker);
    feng_cleanup_push(&inner_node, (void **)&inner);

    feng_frame_release_to(&try_marker);
    ASSERT(inner == NULL);
    ASSERT(outer != NULL);
    ASSERT(g_finalize_count == 1);

    feng_cleanup_pop();
    feng_frame_pop();
    feng_release(outer);
    ASSERT(g_finalize_count == 2);
}

static void test_frame_marker_lifo_pop(void) {
    FengFrameMarker marker;
    FengCleanupNode node;
    TestObject *local;

    g_finalize_count = 0;
    local = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_frame_push(&marker);
    feng_cleanup_push(&node, (void **)&local);
    feng_cleanup_pop();
    feng_frame_pop();

    feng_release(local);
    ASSERT(g_finalize_count == 1);
}

/* --- Finalizer resurrection (Phase 1B-1) ------------------------------- */

/* Model a non-escaping self-capturing closure: the capture retains `self`
 * while the finalizer body runs, then releases it before returning. */
static int g_temporary_self_finalizer_calls = 0;

static void temporary_self_finalizer(void *self) {
    void *temporary;

    ++g_temporary_self_finalizer_calls;
    ASSERT(((FengManagedHeader *)self)->refcount >= 1U);
    temporary = feng_retain(self);
    feng_release(temporary);
}

static const FengTypeDescriptor temporary_self_descriptor = {
    .name = "test.TemporarySelf",
    .size = sizeof(TestObject),
    .finalizer = temporary_self_finalizer,
};

/* A balanced temporary self reference must neither resurrect the object nor
 * re-enter its finalizer before the original invocation returns. */
static void test_finalizer_temporary_self_reference_does_not_reenter(void) {
    TestObject *obj;

    g_temporary_self_finalizer_calls = 0;
    obj = (TestObject *)feng_object_new(&temporary_self_descriptor);
    feng_release(obj);
    ASSERT(g_temporary_self_finalizer_calls == 1);
}

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

/* Verify that ordinary arrays keep length == capacity, while internal storage
 * can reserve a payload without exposing it through the public data accessor. */
static void test_array_storage_capacity_and_public_data(void) {
    FengArray *ordinary = feng_array_new(&i32_element_descriptor,
                                         sizeof(int32_t),
                                         false,
                                         3U);
    FengArray *storage_value = feng_array_new_storage_kinded(
        FENG_VALUE_TRIVIAL,
        NULL,
        &i32_element_descriptor,
        sizeof(int32_t),
        0U,
        4U);
    struct FengArray *ordinary_storage = (struct FengArray *)ordinary;
    struct FengArray *storage = (struct FengArray *)storage_value;
    int32_t *payload = (int32_t *)feng_array_payload_inline(storage);

    ASSERT(ordinary_storage->length == 3U);
    ASSERT(ordinary_storage->capacity == 3U);
    ASSERT(storage->length == 0U);
    ASSERT(storage->capacity == 4U);
    ASSERT(feng_array_length(storage_value) == 0U);
    ASSERT(feng_array_data(storage_value) == NULL);
    ASSERT(payload != NULL);
    ASSERT(feng_array_payload_inline_const(storage) == payload);
    payload[0] = 42;
    ASSERT(payload[0] == 42);

    feng_release(storage_value);
    feng_release(ordinary);
}

/* Verify that zero logical length hides the reserved payload from ordinary
 * array access while storage insertion can initialize and expose that slot. */
static void test_array_storage_zero_length_reuses_capacity(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_TRIVIAL,
        NULL,
        &i32_element_descriptor,
        sizeof(int32_t),
        0U,
        1U);
    int32_t value = 42;

    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_data(storage) == NULL);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              0,
                              &value);
    ASSERT(feng_array_length(storage) == 1U);
    ASSERT(*(int32_t *)feng_array_data(storage) == 42);

    feng_release(storage);
}

/* Verify that finalization ignores stale bytes in uninitialized capacity. */
static void test_array_storage_finalize_uses_length(void) {
    FengArray *storage_value;
    struct FengArray *storage;
    void **slots;
    TestObject *active;
    TestObject *inactive;

    g_finalize_count = 0;
    storage_value = feng_array_new_storage_kinded(
        FENG_VALUE_MANAGED_POINTER,
        NULL,
        &test_object_descriptor,
        sizeof(void *),
        1U,
        3U);
    storage = (struct FengArray *)storage_value;
    slots = (void **)feng_array_payload_inline(storage);
    active = (TestObject *)feng_object_new(&test_object_descriptor);
    inactive = (TestObject *)feng_object_new(&test_object_descriptor);
    slots[0] = active;
    slots[1] = inactive;

    feng_release(storage_value);
    ASSERT(g_finalize_count == 1);

    feng_release(inactive);
    ASSERT(g_finalize_count == 2);
}

/* Exercise all four storage contracts with trivial elements, including
 * middle insertion/removal and replacement allocations in both directions. */
static void test_array_storage_contracts_trivial(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_TRIVIAL,
        NULL,
        &i32_element_descriptor,
        sizeof(int32_t),
        0U,
        5U);
    FengArray *grown;
    FengArray *shrunk;
    FengArray *empty;
    int32_t ten = 10;
    int32_t twenty = 20;
    int32_t thirty = 30;
    int32_t *items;

    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           storage) == 5);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              0,
                              &ten);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              1,
                              &thirty);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              1,
                              &twenty);
    items = (int32_t *)feng_array_data(storage);
    ASSERT(feng_array_length(storage) == 3U);
    ASSERT(items[0] == 10);
    ASSERT(items[1] == 20);
    ASSERT(items[2] == 30);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              1,
                              1);
    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              2,
                              0);
    items = (int32_t *)feng_array_data(storage);
    ASSERT(feng_array_length(storage) == 2U);
    ASSERT(items[0] == 10);
    ASSERT(items[1] == 30);

    grown = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                       storage,
                                       7);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           storage) == 5);
    ASSERT(feng_array_length(grown) == 2U);
    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           grown) == 7);
    items = (int32_t *)feng_array_data(grown);
    ASSERT(items[0] == 10);
    ASSERT(items[1] == 30);
    feng_release(storage);

    shrunk = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                        grown,
                                        1);
    ASSERT(feng_array_length(grown) == 0U);
    ASSERT(feng_array_length(shrunk) == 1U);
    ASSERT(*(int32_t *)feng_array_data(shrunk) == 10);
    feng_release(grown);

    empty = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                       shrunk,
                                       0);
    ASSERT(feng_array_length(shrunk) == 0U);
    ASSERT(feng_array_length(empty) == 0U);
    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           empty) == 0);
    ASSERT(feng_array_data(empty) == NULL);
    feng_release(shrunk);
    feng_release(empty);
}

/* Verify that managed insertion establishes exactly one new hold, movement
 * preserves existing holds, and removal/migration release only discarded slots. */
static void test_array_storage_contracts_managed_pointer(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_MANAGED_POINTER,
        NULL,
        &test_object_descriptor,
        sizeof(void *),
        0U,
        4U);
    FengArray *shrunk;
    TestObject *a;
    TestObject *b;
    TestObject *c;
    void **items;

    g_finalize_count = 0;
    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);
    c = (TestObject *)feng_object_new(&test_object_descriptor);

    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              0,
                              &a);
    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              1,
                              &c);
    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              1,
                              &b);
    ASSERT(a->header.refcount == 2U);
    ASSERT(b->header.refcount == 2U);
    ASSERT(c->header.refcount == 2U);
    items = (void **)feng_array_data(storage);
    ASSERT(items[0] == a);
    ASSERT(items[1] == b);
    ASSERT(items[2] == c);

    feng_release(a);
    feng_release(b);
    feng_release(c);
    feng_array_storage_remove(&object_runtime_generic_descriptor,
                              storage,
                              1,
                              1);
    ASSERT(g_finalize_count == 1);
    items = (void **)feng_array_data(storage);
    ASSERT(items[0] == a);
    ASSERT(items[1] == c);
    ASSERT(a->header.refcount == 1U);
    ASSERT(c->header.refcount == 1U);

    shrunk = feng_array_storage_migrate(&object_runtime_generic_descriptor,
                                        storage,
                                        1);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_length(shrunk) == 1U);
    ASSERT(*(void **)feng_array_data(shrunk) == a);
    ASSERT(a->header.refcount == 1U);
    ASSERT(g_finalize_count == 2);

    feng_release(storage);
    ASSERT(g_finalize_count == 2);
    feng_release(shrunk);
    ASSERT(g_finalize_count == 3);
}

/* Cover head, middle, and tail insertion for trivial storage. */
static void test_array_storage_insert_positions_trivial(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_TRIVIAL,
        NULL,
        &i32_element_descriptor,
        sizeof(int32_t),
        0U,
        4U);
    int32_t values[] = {10, 20, 30, 40};
    int32_t *items;

    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              0,
                              &values[1]);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              0,
                              &values[0]);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              2,
                              &values[3]);
    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              storage,
                              2,
                              &values[2]);

    items = (int32_t *)feng_array_data(storage);
    ASSERT(feng_array_length(storage) == 4U);
    ASSERT(items[0] == 10);
    ASSERT(items[1] == 20);
    ASSERT(items[2] == 30);
    ASSERT(items[3] == 40);

    feng_release(storage);
}

/* Cover head, middle, and tail insertion for managed pointers, proving that
 * each new slot retains once and moving existing slots causes no RC churn. */
static void test_array_storage_insert_positions_managed_pointer(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_MANAGED_POINTER,
        NULL,
        &test_object_descriptor,
        sizeof(void *),
        0U,
        4U);
    TestObject *values[4];
    void **items;
    size_t i;

    g_finalize_count = 0;
    for (i = 0U; i < 4U; ++i) {
        values[i] = (TestObject *)feng_object_new(&test_object_descriptor);
    }

    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              0,
                              &values[1]);
    ASSERT(values[1]->header.refcount == 2U);
    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              0,
                              &values[0]);
    ASSERT(values[0]->header.refcount == 2U);
    ASSERT(values[1]->header.refcount == 2U);
    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              2,
                              &values[3]);
    ASSERT(values[0]->header.refcount == 2U);
    ASSERT(values[1]->header.refcount == 2U);
    ASSERT(values[3]->header.refcount == 2U);
    feng_array_storage_insert(&object_runtime_generic_descriptor,
                              storage,
                              2,
                              &values[2]);

    items = (void **)feng_array_data(storage);
    for (i = 0U; i < 4U; ++i) {
        ASSERT(items[i] == values[i]);
        ASSERT(values[i]->header.refcount == 2U);
        feng_release(values[i]);
    }
    ASSERT(g_finalize_count == 0);

    feng_release(storage);
    ASSERT(g_finalize_count == 4);
}

/* Cover zero-count, head, middle, tail, and whole-range removal while
 * checking the exact surviving order after every movement. */
static void test_array_storage_remove_ranges_trivial(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_TRIVIAL,
        NULL,
        &i32_element_descriptor,
        sizeof(int32_t),
        7U,
        7U);
    int32_t *items = (int32_t *)feng_array_data(storage);
    size_t i;

    for (i = 0U; i < 7U; ++i) {
        items[i] = (int32_t)(i + 1U);
    }

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              7,
                              0);
    ASSERT(feng_array_length(storage) == 7U);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              0,
                              1);
    items = (int32_t *)feng_array_data(storage);
    ASSERT(feng_array_length(storage) == 6U);
    ASSERT(items[0] == 2 && items[5] == 7);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              2,
                              2);
    items = (int32_t *)feng_array_data(storage);
    ASSERT(feng_array_length(storage) == 4U);
    ASSERT(items[0] == 2 && items[1] == 3);
    ASSERT(items[2] == 6 && items[3] == 7);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              3,
                              1);
    items = (int32_t *)feng_array_data(storage);
    ASSERT(feng_array_length(storage) == 3U);
    ASSERT(items[0] == 2 && items[1] == 3 && items[2] == 6);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              storage,
                              0,
                              3);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_data(storage) == NULL);

    feng_release(storage);
}

/* Verify removed managed slots release exactly once and surviving slots keep
 * their original RC while moving through head, middle, and tail removals. */
static void test_array_storage_remove_ranges_managed_pointer(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_MANAGED_POINTER,
        NULL,
        &test_object_descriptor,
        sizeof(void *),
        7U,
        7U);
    TestObject *values[7];
    void **items = (void **)feng_array_data(storage);
    size_t i;

    g_finalize_count = 0;
    for (i = 0U; i < 7U; ++i) {
        values[i] = (TestObject *)feng_object_new(&test_object_descriptor);
        items[i] = feng_retain(values[i]);
        ASSERT(values[i]->header.refcount == 2U);
    }

    feng_array_storage_remove(&object_runtime_generic_descriptor,
                              storage,
                              7,
                              0);
    for (i = 0U; i < 7U; ++i) {
        ASSERT(values[i]->header.refcount == 2U);
    }

    feng_array_storage_remove(&object_runtime_generic_descriptor,
                              storage,
                              0,
                              1);
    ASSERT(values[0]->header.refcount == 1U);
    items = (void **)feng_array_data(storage);
    ASSERT(items[0] == values[1] && items[5] == values[6]);
    for (i = 1U; i < 7U; ++i) {
        ASSERT(values[i]->header.refcount == 2U);
    }

    feng_array_storage_remove(&object_runtime_generic_descriptor,
                              storage,
                              2,
                              2);
    ASSERT(values[3]->header.refcount == 1U);
    ASSERT(values[4]->header.refcount == 1U);
    items = (void **)feng_array_data(storage);
    ASSERT(items[0] == values[1] && items[1] == values[2]);
    ASSERT(items[2] == values[5] && items[3] == values[6]);
    ASSERT(values[1]->header.refcount == 2U);
    ASSERT(values[2]->header.refcount == 2U);
    ASSERT(values[5]->header.refcount == 2U);
    ASSERT(values[6]->header.refcount == 2U);

    feng_array_storage_remove(&object_runtime_generic_descriptor,
                              storage,
                              3,
                              1);
    ASSERT(values[6]->header.refcount == 1U);
    items = (void **)feng_array_data(storage);
    ASSERT(items[0] == values[1]);
    ASSERT(items[1] == values[2]);
    ASSERT(items[2] == values[5]);

    feng_array_storage_remove(&object_runtime_generic_descriptor,
                              storage,
                              0,
                              3);
    ASSERT(values[1]->header.refcount == 1U);
    ASSERT(values[2]->header.refcount == 1U);
    ASSERT(values[5]->header.refcount == 1U);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(g_finalize_count == 0);

    feng_release(storage);
    ASSERT(g_finalize_count == 0);
    for (i = 0U; i < 7U; ++i) {
        feng_release(values[i]);
    }
    ASSERT(g_finalize_count == 7);
}

/* Verify migration always creates a replacement instance for equal, larger,
 * smaller, and zero capacities while preserving the retained prefix order. */
static void test_array_storage_migrate_capacity_shapes(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_TRIVIAL,
        NULL,
        &i32_element_descriptor,
        sizeof(int32_t),
        3U,
        4U);
    FengArray *replacement;
    int32_t *items = (int32_t *)feng_array_data(storage);

    items[0] = 10;
    items[1] = 20;
    items[2] = 30;

    replacement = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                              storage,
                                              4);
    ASSERT(replacement != storage);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           replacement) == 4);
    items = (int32_t *)feng_array_data(replacement);
    ASSERT(items[0] == 10 && items[1] == 20 && items[2] == 30);
    feng_release(storage);
    storage = replacement;

    replacement = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                              storage,
                                              6);
    ASSERT(replacement != storage);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           replacement) == 6);
    items = (int32_t *)feng_array_data(replacement);
    ASSERT(items[0] == 10 && items[1] == 20 && items[2] == 30);
    feng_release(storage);
    storage = replacement;

    replacement = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                              storage,
                                              2);
    ASSERT(replacement != storage);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_length(replacement) == 2U);
    items = (int32_t *)feng_array_data(replacement);
    ASSERT(items[0] == 10 && items[1] == 20);
    feng_release(storage);
    storage = replacement;

    replacement = feng_array_storage_migrate(&i32_runtime_generic_descriptor,
                                              storage,
                                              0);
    ASSERT(replacement != storage);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_length(replacement) == 0U);
    ASSERT(feng_array_storage_get_capacity(&i32_runtime_generic_descriptor,
                                           replacement) == 0);
    feng_release(storage);
    feng_release(replacement);
}

/* Verify equal and growth migration preserve every element hold, while
 * shrink and zero-capacity migration release each truncated hold once. */
static void test_array_storage_migrate_managed_pointer_refcounts(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_MANAGED_POINTER,
        NULL,
        &test_object_descriptor,
        sizeof(void *),
        3U,
        4U);
    FengArray *replacement;
    TestObject *values[3];
    void **items = (void **)feng_array_data(storage);
    size_t i;

    g_finalize_count = 0;
    for (i = 0U; i < 3U; ++i) {
        values[i] = (TestObject *)feng_object_new(&test_object_descriptor);
        items[i] = feng_retain(values[i]);
        ASSERT(values[i]->header.refcount == 2U);
    }

    replacement = feng_array_storage_migrate(&object_runtime_generic_descriptor,
                                              storage,
                                              4);
    ASSERT(replacement != storage);
    for (i = 0U; i < 3U; ++i) {
        ASSERT(values[i]->header.refcount == 2U);
    }
    feng_release(storage);
    storage = replacement;

    replacement = feng_array_storage_migrate(&object_runtime_generic_descriptor,
                                              storage,
                                              6);
    ASSERT(replacement != storage);
    for (i = 0U; i < 3U; ++i) {
        ASSERT(values[i]->header.refcount == 2U);
    }
    feng_release(storage);
    storage = replacement;

    replacement = feng_array_storage_migrate(&object_runtime_generic_descriptor,
                                              storage,
                                              2);
    ASSERT(replacement != storage);
    ASSERT(values[0]->header.refcount == 2U);
    ASSERT(values[1]->header.refcount == 2U);
    ASSERT(values[2]->header.refcount == 1U);
    items = (void **)feng_array_data(replacement);
    ASSERT(items[0] == values[0] && items[1] == values[1]);
    feng_release(storage);
    storage = replacement;

    replacement = feng_array_storage_migrate(&object_runtime_generic_descriptor,
                                              storage,
                                              0);
    ASSERT(replacement != storage);
    ASSERT(values[0]->header.refcount == 1U);
    ASSERT(values[1]->header.refcount == 1U);
    ASSERT(values[2]->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);
    feng_release(storage);
    feng_release(replacement);

    for (i = 0U; i < 3U; ++i) {
        feng_release(values[i]);
    }
    ASSERT(g_finalize_count == 3);
}

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
    { offsetof(CycNode, child_a), NULL, NULL },
    { offsetof(CycNode, child_b), NULL, NULL },
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

/* Cycle-path analogue of a temporary self capture. With threshold one, its
 * nested release enqueues a candidate while Phase 1 is active; the current
 * white set must remain owned by the outer collection round. */
static int g_cyc_temporary_self_fin_count = 0;

static void cyc_node_temporary_self_finalize(void *self) {
    void *temporary;

    ++g_cyc_temporary_self_fin_count;
    temporary = feng_retain(self);
    feng_release(temporary);
}

static const FengTypeDescriptor cyc_node_temporary_self_descriptor = {
    .name = "test.CycNodeTemporarySelf",
    .size = sizeof(CycNode),
    .finalizer = cyc_node_temporary_self_finalize,
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

/* Phase 1 temporary self references may enqueue deferred candidates, but
 * cannot recursively collect the current white set or leave stale entries
 * after the free-set is reclaimed. */
static void test_cycle_finalizer_temporary_self_reference_defers_collection(void) {
    CycNode *a;
    CycNode *b;

    g_cyc_temporary_self_fin_count = 0;
    a = cyc_new(&cyc_node_temporary_self_descriptor);
    b = cyc_new(&cyc_node_temporary_self_descriptor);
    cyc_link(&a->child_a, b);
    cyc_link(&b->child_a, a);
    feng_release(a);
    feng_release(b);

    feng_cycle_lock();
    feng_cycle_set_threshold_for_test(1U);
    feng_cycle_collect_locked();
    feng_cycle_set_threshold_for_test(256U);
    feng_cycle_unlock();

    ASSERT(g_cyc_temporary_self_fin_count == 2);
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

/* Exercise all collector array walkers through finalization and survivor BFS,
 * proving that stale bytes in [length, capacity) are never treated as edges. */
static void test_array_storage_cycle_collector_uses_length(void) {
    FengArray *storage_value;
    struct FengArray *storage;
    void **slots;
    CycNode *node;
    TestObject *inactive;
    void *node_via_array;
    void *array_via_node;

    g_finalize_count = 0;
    g_cyc_res_self_fin_count = 0;
    g_cyc_res_slot_self = NULL;
    g_cyc_res_self_enabled = true;

    storage_value = feng_array_new_storage_kinded(
        FENG_VALUE_MANAGED_POINTER,
        NULL,
        &cyc_node_res_self_descriptor,
        sizeof(void *),
        1U,
        2U);
    storage = (struct FengArray *)storage_value;
    slots = (void **)feng_array_payload_inline(storage);
    node = cyc_new(&cyc_node_res_self_descriptor);
    inactive = (TestObject *)feng_object_new(&test_object_descriptor);

    slots[0] = feng_retain(node);
    slots[1] = inactive;
    node->child_a = feng_retain(storage_value);

    feng_release(storage_value);
    feng_release(node);

    feng_cycle_lock();
    feng_cycle_collect_locked();
    feng_cycle_unlock();

    ASSERT(g_cyc_res_self_fin_count == 1);
    ASSERT(g_cyc_res_slot_self == node);
    ASSERT(g_finalize_count == 0);
    ASSERT(node->header.refcount == 2U);
    ASSERT(storage->header.refcount == 1U);

    g_cyc_res_self_enabled = false;
    node_via_array = slots[0];
    slots[0] = NULL;
    array_via_node = node->child_a;
    node->child_a = NULL;
    g_cyc_res_slot_self = NULL;

    feng_release(node_via_array);
    feng_release(array_via_node);
    feng_release(node);
    ASSERT(g_cyc_res_self_fin_count == 2);

    feng_release(inactive);
    ASSERT(g_finalize_count == 1);
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

/* --- Finalizer exception escape (docs/specifications/feng-lifetime.md §13.2) ---------- */

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* A finalizer that throws an exception value and never catches it.
 * The runtime barrier in feng_finalizer_invoke must intercept the throw and
 * panic; the process must exit via abort() (SIGABRT). */
static void throwing_finalizer(void *self) {
    TestObject *payload;

    (void)self;
    payload = (TestObject *)feng_object_new(&test_object_descriptor);
    feng_throw(payload, &test_object_descriptor);
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

static void array_slice_out_of_range_body(void) {
    FengArray *source = feng_array_new(&i32_element_descriptor, sizeof(int32_t), false, 2U);
    FengArray *slice = feng_array_slice(&i32_runtime_generic_descriptor, source, 1, 3);
    feng_release(slice);
    feng_release(source);
}

/* Child-process body proving ordinary index checks use logical length rather
 * than reserved storage capacity. */
static void array_storage_zero_length_index_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     0U,
                                                     1U);
    feng_array_check_index(array, 0U);
    feng_release(array);
}

/* Child-process body for a negative insertion index. */
static void array_storage_insert_negative_index_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     0U,
                                                     1U);
    int32_t value = 1;

    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              array,
                              -1,
                              &value);
    feng_release(array);
}

/* Child-process body for an insertion index beyond logical length. */
static void array_storage_insert_out_of_range_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     0U,
                                                     1U);
    int32_t value = 1;

    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              array,
                              1,
                              &value);
    feng_release(array);
}

/* Child-process body for insertion into full storage. */
static void array_storage_insert_full_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     1U,
                                                     1U);
    int32_t value = 1;

    feng_array_storage_insert(&i32_runtime_generic_descriptor,
                              array,
                              1,
                              &value);
    feng_release(array);
}

/* Child-process body for a negative removal index. */
static void array_storage_remove_negative_index_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     1U,
                                                     1U);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              array,
                              -1,
                              0);
    feng_release(array);
}

/* Child-process body for a negative removal count. */
static void array_storage_remove_negative_count_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     1U,
                                                     1U);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              array,
                              0,
                              -1);
    feng_release(array);
}

/* Child-process body for a removal range beyond logical length. */
static void array_storage_remove_out_of_range_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     1U,
                                                     1U);

    feng_array_storage_remove(&i32_runtime_generic_descriptor,
                              array,
                              1,
                              1);
    feng_release(array);
}

/* Child-process body for a negative migration capacity. */
static void array_storage_migrate_negative_capacity_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     0U,
                                                     1U);
    FengArray *replacement = feng_array_storage_migrate(
        &i32_runtime_generic_descriptor,
        array,
        -1);

    feng_release(replacement);
    feng_release(array);
}

/* Child-process body for the invalid length/capacity invariant. */
static void array_storage_length_exceeds_capacity_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     2U,
                                                     1U);
    feng_release(array);
}

/* Child-process body for capacity byte-size overflow. */
static void array_storage_capacity_overflow_body(void) {
    FengArray *array = feng_array_new_storage_kinded(FENG_VALUE_TRIVIAL,
                                                     NULL,
                                                     &i32_element_descriptor,
                                                     sizeof(int32_t),
                                                     0U,
                                                     SIZE_MAX);
    feng_release(array);
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

static void test_array_slice_out_of_range_aborts(void) {
    assert_child_aborts(array_slice_out_of_range_body);
}

/* Verify internal storage construction rejects invalid shapes before allocation. */
static void test_array_storage_invalid_shape_aborts(void) {
    assert_child_aborts(array_storage_length_exceeds_capacity_body);
    assert_child_aborts(array_storage_capacity_overflow_body);
}

/* Verify storage operations reject negative values, invalid ranges, and full
 * insertion, and that ordinary indexing still rejects reserved-only slots. */
static void test_array_storage_invalid_operations_abort(void) {
    assert_child_aborts(array_storage_zero_length_index_body);
    assert_child_aborts(array_storage_insert_negative_index_body);
    assert_child_aborts(array_storage_insert_out_of_range_body);
    assert_child_aborts(array_storage_insert_full_body);
    assert_child_aborts(array_storage_remove_negative_index_body);
    assert_child_aborts(array_storage_remove_negative_count_body);
    assert_child_aborts(array_storage_remove_out_of_range_body);
    assert_child_aborts(array_storage_migrate_negative_capacity_body);
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

/* --- By-value aggregate (FengAggregateDescriptor) ----------------
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

static const FengAggregateDescriptor fat_pair_desc = {
    .name = "test.FatPair",
    .size = sizeof(FatPair),
    .default_init = &fat_pair_default,
    .managed_slot_count = sizeof(fat_pair_slots) / sizeof(fat_pair_slots[0]),
    .managed_slots = fat_pair_slots,
};

static const FengGenericParamDescriptor fat_pair_runtime_generic_descriptor = {
    .kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
    .descriptor = &fat_pair_desc,
    .witness = NULL,
};

static void test_test_value_identity_contract_copies_trivial_value(void) {
    int32_t value = 42;
    int32_t out = 0;

    __test_value_identity(&i32_runtime_generic_descriptor, &value, &out);
    ASSERT(out == 42);
}

static void test_test_value_identity_contract_retains_managed_pointer(void) {
    TestObject *value;
    TestObject *out = NULL;

    g_finalize_count = 0;
    value = (TestObject *)feng_object_new(&test_object_descriptor);

    __test_value_identity(&object_runtime_generic_descriptor, &value, &out);

    ASSERT(out == value);
    ASSERT(value->header.refcount == 2U);

    feng_release(value);
    ASSERT(g_finalize_count == 0);
    ASSERT(out->header.refcount == 1U);

    feng_release(out);
    ASSERT(g_finalize_count == 1);
}

static void test_test_value_identity_contract_retains_aggregate(void) {
    FatPair value = {0};
    FatPair out = {0};
    TestObject *subject;

    g_finalize_count = 0;
    subject = (TestObject *)feng_object_new(&test_object_descriptor);
    value.subject = subject;
    value.tag = 7;

    __test_value_identity(&fat_pair_runtime_generic_descriptor, &value, &out);

    ASSERT(out.subject == subject);
    ASSERT(out.tag == 7);
    ASSERT(subject->header.refcount == 2U);

    feng_aggregate_release(&value, &fat_pair_desc);
    ASSERT(g_finalize_count == 0);
    ASSERT(subject->header.refcount == 1U);

    feng_aggregate_release(&out, &fat_pair_desc);
    ASSERT(g_finalize_count == 1);
}

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

static const FengAggregateDescriptor outer_desc = {
    .name = "test.OuterAgg",
    .size = sizeof(OuterAgg),
    .default_init = &outer_default_zero,
    .managed_slot_count = sizeof(outer_slots) / sizeof(outer_slots[0]),
    .managed_slots = outer_slots,
};

/* Runtime generic carrier for the nested three-managed-slot aggregate. */
static const FengGenericParamDescriptor outer_runtime_generic_descriptor = {
    .kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
    .descriptor = &outer_desc,
    .witness = NULL,
};

typedef struct ForwardAgg {
    FengManagedSlotDescriptor active;
    union {
        int   trivial;
        void *ptr;
        FatPair nested;
    } payload;
} ForwardAgg;

static const FengManagedSlotDescriptor forward_slots[] = {
    { offsetof(ForwardAgg, active), FENG_SLOT_FORWARD, NULL },
};

static const FengAggregateDescriptor forward_desc = {
    .name = "test.ForwardAgg",
    .size = sizeof(ForwardAgg),
    .default_init = &outer_default_zero,
    .managed_slot_count = sizeof(forward_slots) / sizeof(forward_slots[0]),
    .managed_slots = forward_slots,
};

static void test_array_slice_aggregate_assigns_elements(void) {
    FengArray *source;
    FengArray *slice;
    FatPair *source_items;
    FatPair *slice_items;
    TestObject *subject1;
    TestObject *subject2;

    g_finalize_count = 0;
    source = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
                                   &fat_pair_desc,
                                   NULL,
                                   fat_pair_desc.size,
                                   3U);
    source_items = (FatPair *)feng_array_data(source);
    source_items[0].tag = 10;
    source_items[1].tag = 20;
    source_items[2].tag = 30;
    subject1 = (TestObject *)source_items[1].subject;
    subject2 = (TestObject *)source_items[2].subject;

    slice = feng_array_slice(&fat_pair_runtime_generic_descriptor, source, 1, 2);
    slice_items = (FatPair *)feng_array_data(slice);

    ASSERT(feng_array_length(slice) == 2U);
    ASSERT(feng_array_element_kind(slice) == FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS);
    ASSERT(feng_array_element_aggregate(slice) == &fat_pair_desc);
    ASSERT(slice_items[0].subject == subject1);
    ASSERT(slice_items[1].subject == subject2);
    ASSERT(slice_items[0].tag == 20);
    ASSERT(slice_items[1].tag == 30);
    ASSERT(subject1->header.refcount == 2U);
    ASSERT(subject2->header.refcount == 2U);
    ASSERT(g_finalize_count == 2);

    slice_items[0].tag = 99;
    ASSERT(source_items[1].tag == 20);

    feng_release(source);
    ASSERT(g_finalize_count == 3);
    ASSERT(subject1->header.refcount == 1U);
    ASSERT(subject2->header.refcount == 1U);

    feng_release(slice);
    ASSERT(g_finalize_count == 5);
}

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
    /* take preserves non-managed bytes in src (per docs/engineering/feng-value-model
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

static void test_aggregate_forward_none_is_noop(void) {
    ForwardAgg value = {0};
    value.active = (FengManagedSlotDescriptor){ 0U, FENG_SLOT_NONE, NULL };
    value.payload.trivial = 123;

    feng_aggregate_retain(&value, &forward_desc);
    feng_aggregate_release(&value, &forward_desc);

    ASSERT(value.payload.trivial == 123);
}

static void test_aggregate_forward_pointer_retain_release(void) {
    g_finalize_count = 0;
    TestObject *subject = (TestObject *)feng_object_new(&test_object_descriptor);
    ForwardAgg value = {0};
    value.active = (FengManagedSlotDescriptor){
        offsetof(ForwardAgg, payload), FENG_SLOT_POINTER, NULL
    };
    value.payload.ptr = subject;

    feng_aggregate_retain(&value, &forward_desc);
    ASSERT(subject->header.refcount == 2U);

    feng_aggregate_release(&value, &forward_desc);
    ASSERT(subject->header.refcount == 1U);
    ASSERT(g_finalize_count == 0);

    feng_aggregate_release(&value, &forward_desc);
    ASSERT(g_finalize_count == 1);
}

static void test_aggregate_forward_nested_assign(void) {
    g_finalize_count = 0;
    TestObject *src_subject = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *dst_subject = (TestObject *)feng_object_new(&test_object_descriptor);
    FengManagedSlotDescriptor nested_slot = {
        offsetof(ForwardAgg, payload), FENG_SLOT_NESTED_AGGREGATE, &fat_pair_desc
    };
    ForwardAgg src = {0};
    ForwardAgg dst = {0};
    src.active = nested_slot;
    src.payload.nested.subject = src_subject;
    src.payload.nested.tag = 17;
    dst.active = nested_slot;
    dst.payload.nested.subject = dst_subject;
    dst.payload.nested.tag = 23;

    feng_aggregate_assign(&dst, &src, &forward_desc);

    ASSERT(dst.active.kind == FENG_SLOT_NESTED_AGGREGATE);
    ASSERT(dst.payload.nested.subject == src_subject);
    ASSERT(dst.payload.nested.tag == 17);
    ASSERT(src_subject->header.refcount == 2U);
    ASSERT(g_finalize_count == 1);

    feng_aggregate_release(&dst, &forward_desc);
    feng_aggregate_release(&src, &forward_desc);
    ASSERT(g_finalize_count == 2);
}

static void test_aggregate_forward_take_nulls_source_pointer(void) {
    g_finalize_count = 0;
    TestObject *dst_subject = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *src_subject = (TestObject *)feng_object_new(&test_object_descriptor);
    FengManagedSlotDescriptor pointer_slot = {
        offsetof(ForwardAgg, payload), FENG_SLOT_POINTER, NULL
    };
    ForwardAgg dst = {0};
    ForwardAgg src = {0};
    dst.active = pointer_slot;
    dst.payload.ptr = dst_subject;
    src.active = pointer_slot;
    src.payload.ptr = src_subject;

    feng_aggregate_take(&dst, &src, &forward_desc);

    ASSERT(dst.payload.ptr == src_subject);
    ASSERT(src.payload.ptr == NULL);
    ASSERT(src.active.kind == FENG_SLOT_POINTER);
    ASSERT(src_subject->header.refcount == 1U);
    ASSERT(g_finalize_count == 1);

    feng_aggregate_release(&src, &forward_desc);
    ASSERT(g_finalize_count == 1);

    feng_aggregate_release(&dst, &forward_desc);
    ASSERT(g_finalize_count == 2);
}

/* ---- §7.3 Array element three-classification ----------------------- */

static void test_array_kinded_trivial_matches_legacy(void) {
    /* TRIVIAL kind via the kinded API must behave identically to the legacy
     * (element_is_managed = false) constructor. */
    FengArray *a = feng_array_new_kinded(FENG_VALUE_TRIVIAL,
                                         NULL,
                                         &i32_element_descriptor,
                                         sizeof(int32_t),
                                         3U);
    ASSERT(feng_array_length(a) == 3U);
    ASSERT(feng_array_element_kind(a) == FENG_VALUE_TRIVIAL);
    ASSERT(feng_array_element_aggregate(a) == NULL);
    int32_t *items = (int32_t *)feng_array_data(a);
    ASSERT(items != NULL);
    /* calloc-zeroed. */
    for (size_t i = 0U; i < 3U; ++i) {
        ASSERT(items[i] == 0);
    }
    feng_release(a);
}

static void test_array_kinded_managed_pointer_matches_legacy(void) {
    g_finalize_count = 0;
    FengArray *a = feng_array_new_kinded(FENG_VALUE_MANAGED_POINTER,
                                         NULL,
                                         &test_object_descriptor,
                                         sizeof(void *),
                                         2U);
    ASSERT(feng_array_element_kind(a) == FENG_VALUE_MANAGED_POINTER);
    ASSERT(feng_array_element_aggregate(a) == NULL);
    void **slots = (void **)feng_array_data(a);
    TestObject *o0 = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *o1 = (TestObject *)feng_object_new(&test_object_descriptor);
    /* Move ownership in (legacy callers do `slots[i] = obj` directly). */
    slots[0] = o0;
    slots[1] = o1;
    ASSERT(g_finalize_count == 0);
    feng_release(a);
    /* Array finalize releases both slots. */
    ASSERT(g_finalize_count == 2);
}

static void test_array_aggregate_zero_bytes_default(void) {
    /* OuterAgg uses FENG_DEFAULT_ZERO_BYTES — calloc alone suffices. */
    FengArray *a = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
                                         &outer_desc,
                                         NULL,
                                         outer_desc.size,
                                         3U);
    ASSERT(feng_array_length(a) == 3U);
    ASSERT(feng_array_element_kind(a) == FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS);
    ASSERT(feng_array_element_aggregate(a) == &outer_desc);

    OuterAgg *items = (OuterAgg *)feng_array_data(a);
    for (size_t i = 0U; i < 3U; ++i) {
        ASSERT(items[i].head == NULL);
        ASSERT(items[i].tail == NULL);
        ASSERT(items[i].inner.subject == NULL);
        ASSERT(items[i].inner.tag == 0);
    }
    /* Empty release path must be a clean no-op even with NULL slots. */
    feng_release(a);
}

static void test_array_aggregate_init_fn_runs_per_element(void) {
    /* FatPair uses FENG_DEFAULT_INIT_FN — every element must reach a
     * properly-initialised state with subject != NULL and refcount 1. */
    g_finalize_count = 0;
    FengArray *a = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
                                         &fat_pair_desc,
                                         NULL,
                                         fat_pair_desc.size,
                                         4U);
    FatPair *items = (FatPair *)feng_array_data(a);
    for (size_t i = 0U; i < 4U; ++i) {
        ASSERT(items[i].subject != NULL);
        ASSERT(items[i].tag == -1);
        ASSERT(((FengManagedHeader *)items[i].subject)->refcount == 1U);
    }
    /* Releasing the array calls feng_aggregate_release on each element,
     * which in turn finalizes each TestObject. */
    feng_release(a);
    ASSERT(g_finalize_count == 4);
}

/* Aggregate default initialization and release cover only the initialized
 * prefix, even when the allocation contains additional storage slots. */
static void test_array_storage_aggregate_lifecycle_uses_length(void) {
    FengArray *storage_value;
    struct FengArray *storage;
    FatPair *items;

    g_finalize_count = 0;
    storage_value = feng_array_new_storage_kinded(
        FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
        &fat_pair_desc,
        NULL,
        fat_pair_desc.size,
        2U,
        4U);
    storage = (struct FengArray *)storage_value;
    items = (FatPair *)feng_array_payload_inline(storage);

    ASSERT(items[0].subject != NULL);
    ASSERT(items[1].subject != NULL);
    ASSERT(items[2].subject == NULL);
    ASSERT(items[3].subject == NULL);

    feng_release(storage_value);
    ASSERT(g_finalize_count == 2);
}

/* Verify aggregate storage operations retain only inserted values, move
 * surviving aggregates without RC churn, and release removed/truncated slots. */
static void test_array_storage_contracts_aggregate(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
        &fat_pair_desc,
        NULL,
        fat_pair_desc.size,
        0U,
        3U);
    FengArray *shrunk;
    TestObject *a;
    TestObject *b;
    FatPair first;
    FatPair second;
    FatPair *items;

    g_finalize_count = 0;
    a = (TestObject *)feng_object_new(&test_object_descriptor);
    b = (TestObject *)feng_object_new(&test_object_descriptor);
    first.subject = a;
    first.tag = 10;
    second.subject = b;
    second.tag = 20;

    feng_array_storage_insert(&fat_pair_runtime_generic_descriptor,
                              storage,
                              0,
                              &first);
    feng_array_storage_insert(&fat_pair_runtime_generic_descriptor,
                              storage,
                              1,
                              &second);
    feng_array_storage_insert(&fat_pair_runtime_generic_descriptor,
                              storage,
                              1,
                              &first);
    ASSERT(a->header.refcount == 3U);
    ASSERT(b->header.refcount == 2U);
    items = (FatPair *)feng_array_data(storage);
    ASSERT(items[0].subject == a && items[0].tag == 10);
    ASSERT(items[1].subject == a && items[1].tag == 10);
    ASSERT(items[2].subject == b && items[2].tag == 20);

    feng_aggregate_release(&first, &fat_pair_desc);
    feng_aggregate_release(&second, &fat_pair_desc);
    ASSERT(a->header.refcount == 2U);
    ASSERT(b->header.refcount == 1U);

    feng_array_storage_remove(&fat_pair_runtime_generic_descriptor,
                              storage,
                              1,
                              1);
    ASSERT(a->header.refcount == 1U);
    ASSERT(b->header.refcount == 1U);
    items = (FatPair *)feng_array_data(storage);
    ASSERT(items[0].subject == a && items[0].tag == 10);
    ASSERT(items[1].subject == b && items[1].tag == 20);

    shrunk = feng_array_storage_migrate(&fat_pair_runtime_generic_descriptor,
                                        storage,
                                        1);
    ASSERT(feng_array_length(storage) == 0U);
    ASSERT(feng_array_length(shrunk) == 1U);
    items = (FatPair *)feng_array_data(shrunk);
    ASSERT(items[0].subject == a && items[0].tag == 10);
    ASSERT(a->header.refcount == 1U);
    ASSERT(g_finalize_count == 1);

    feng_release(storage);
    ASSERT(g_finalize_count == 1);
    feng_release(shrunk);
    ASSERT(g_finalize_count == 2);
}

/* Cover head, middle, and tail insertion for nested aggregates, proving that
 * every managed slot is retained once and moved aggregates keep stable RC. */
static void test_array_storage_insert_positions_aggregate(void) {
    FengArray *storage = feng_array_new_storage_kinded(
        FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
        &outer_desc,
        NULL,
        outer_desc.size,
        0U,
        4U);
    TestObject *objects[4][3];
    OuterAgg values[4];
    OuterAgg *items;
    size_t i;
    size_t j;

    g_finalize_count = 0;
    memset(values, 0, sizeof(values));
    for (i = 0U; i < 4U; ++i) {
        for (j = 0U; j < 3U; ++j) {
            objects[i][j] = (TestObject *)feng_object_new(&test_object_descriptor);
        }
        values[i].head = objects[i][0];
        values[i].inner.subject = objects[i][1];
        values[i].inner.tag = (int)i;
        values[i].tail = objects[i][2];
    }

    feng_array_storage_insert(&outer_runtime_generic_descriptor,
                              storage,
                              0,
                              &values[1]);
    for (j = 0U; j < 3U; ++j) {
        ASSERT(objects[1][j]->header.refcount == 2U);
    }
    feng_array_storage_insert(&outer_runtime_generic_descriptor,
                              storage,
                              0,
                              &values[0]);
    for (i = 0U; i < 2U; ++i) {
        for (j = 0U; j < 3U; ++j) {
            ASSERT(objects[i][j]->header.refcount == 2U);
        }
    }
    feng_array_storage_insert(&outer_runtime_generic_descriptor,
                              storage,
                              2,
                              &values[3]);
    for (j = 0U; j < 3U; ++j) {
        ASSERT(objects[3][j]->header.refcount == 2U);
    }
    feng_array_storage_insert(&outer_runtime_generic_descriptor,
                              storage,
                              2,
                              &values[2]);

    items = (OuterAgg *)feng_array_data(storage);
    for (i = 0U; i < 4U; ++i) {
        ASSERT(items[i].head == objects[i][0]);
        ASSERT(items[i].inner.subject == objects[i][1]);
        ASSERT(items[i].inner.tag == (int)i);
        ASSERT(items[i].tail == objects[i][2]);
        for (j = 0U; j < 3U; ++j) {
            ASSERT(objects[i][j]->header.refcount == 2U);
        }
        feng_aggregate_release(&values[i], &outer_desc);
    }
    ASSERT(g_finalize_count == 0);
    for (i = 0U; i < 4U; ++i) {
        for (j = 0U; j < 3U; ++j) {
            ASSERT(objects[i][j]->header.refcount == 1U);
        }
    }

    feng_release(storage);
    ASSERT(g_finalize_count == 12);
}

static void test_array_aggregate_assign_per_element_tracks_refcount(void) {
    /* User-driven: zero-initialise the array, then move owning references
     * into each element via feng_aggregate_assign, and verify the array
     * finalizer drops them when released. */
    g_finalize_count = 0;
    FengArray *a = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
                                         &outer_desc,
                                         NULL,
                                         outer_desc.size,
                                         2U);
    OuterAgg *items = (OuterAgg *)feng_array_data(a);

    TestObject *h0 = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *t0 = (TestObject *)feng_object_new(&test_object_descriptor);
    TestObject *h1 = (TestObject *)feng_object_new(&test_object_descriptor);

    OuterAgg src0 = { .head = h0,
                      .inner = { .subject = NULL, .tag = 0 },
                      .tail = t0 };
    OuterAgg src1 = { .head = h1,
                      .inner = { .subject = NULL, .tag = 0 },
                      .tail = NULL };

    feng_aggregate_assign(&items[0], &src0, &outer_desc);
    feng_aggregate_assign(&items[1], &src1, &outer_desc);
    /* Each holder now sits at rc=2 (src + array slot). */
    ASSERT(h0->header.refcount == 2U);
    ASSERT(t0->header.refcount == 2U);
    ASSERT(h1->header.refcount == 2U);

    /* Drop the source aggregates first. */
    feng_aggregate_release(&src0, &outer_desc);
    feng_aggregate_release(&src1, &outer_desc);
    ASSERT(g_finalize_count == 0);
    ASSERT(h0->header.refcount == 1U);
    ASSERT(t0->header.refcount == 1U);
    ASSERT(h1->header.refcount == 1U);

    /* Releasing the array must release each element's slots. */
    feng_release(a);
    ASSERT(g_finalize_count == 3);
}

static void test_array_aggregate_zero_length(void) {
    FengArray *a = feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
                                         &fat_pair_desc,
                                         NULL,
                                         fat_pair_desc.size,
                                         0U);
    ASSERT(feng_array_length(a) == 0U);
    ASSERT(feng_array_data(a) == NULL);
    ASSERT(feng_array_element_kind(a) == FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS);
    ASSERT(feng_array_element_aggregate(a) == &fat_pair_desc);
    feng_release(a);
}

int main(void) {
    test_object_retain_release();
    test_retain_release_nullsafe();
    test_assign_barrier();
    test_take();
    test_scalar_box_runtime_contract();
    test_string_literal_immortal();
    test_string_concat();
    test_string_utf8_length_contract();
    test_array_primitive();
    test_array_managed_releases_elements();
    test_array_zero_length();
    test_array_payload_alignment();
    test_array_length_contract_uses_descriptor();
    test_array_slice_trivial_copies_subrange();
    test_array_slice_managed_pointer_retains_elements();
    test_expression_equal_contract_uses_descriptor();
    test_test_value_identity_contract_copies_trivial_value();
    test_test_value_identity_contract_retains_managed_pointer();
    test_test_value_identity_contract_retains_aggregate();
    test_array_slice_aggregate_assigns_elements();
    test_frame_marker_release_to_try_marker();
    test_frame_marker_release_to_try_preserves_outer_cleanup();
    test_frame_marker_lifo_pop();
    test_finalizer_temporary_self_reference_does_not_reenter();
    test_finalizer_resurrection_then_release();
    test_finalizer_resurrection_reruns_on_next_release();
    test_finalizer_no_resurrection_releases();
    test_array_storage_capacity_and_public_data();
    test_array_storage_zero_length_reuses_capacity();
    test_array_storage_finalize_uses_length();
    test_array_storage_contracts_trivial();
    test_array_storage_contracts_managed_pointer();
    test_array_storage_insert_positions_trivial();
    test_array_storage_insert_positions_managed_pointer();
    test_array_storage_remove_ranges_trivial();
    test_array_storage_remove_ranges_managed_pointer();
    test_array_storage_migrate_capacity_shapes();
    test_array_storage_migrate_managed_pointer_refcounts();
    test_cycle_collector_reclaims_two_node_cycle();
    test_cycle_collector_does_not_collect_externally_referenced();
    test_cycle_collector_reclaims_cycle_with_finalizer();
    test_cycle_finalizer_temporary_self_reference_defers_collection();
    test_cycle_collector_finalizer_resurrects_self();
    test_array_storage_cycle_collector_uses_length();
    test_cycle_collector_partial_resurrection_frees_unsurvived();
    test_cycle_collector_acyclic_object_never_enqueued();
    test_finalizer_throw_on_arc_path_aborts();
    test_finalizer_throw_on_cycle_path_aborts();
    test_array_slice_out_of_range_aborts();
    test_array_storage_invalid_shape_aborts();
    test_array_storage_invalid_operations_abort();
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
    test_aggregate_forward_none_is_noop();
    test_aggregate_forward_pointer_retain_release();
    test_aggregate_forward_nested_assign();
    test_aggregate_forward_take_nulls_source_pointer();

    test_array_kinded_trivial_matches_legacy();
    test_array_kinded_managed_pointer_matches_legacy();
    test_array_aggregate_zero_bytes_default();
    test_array_aggregate_init_fn_runs_per_element();
    test_array_storage_aggregate_lifecycle_uses_length();
    test_array_storage_contracts_aggregate();
    test_array_storage_insert_positions_aggregate();
    test_array_aggregate_assign_per_element_tracks_refcount();
    test_array_aggregate_zero_length();

    fputs("test_runtime: ok\n", stdout);
    return 0;
}
