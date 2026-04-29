/* By-value aggregate value lifecycle (dev/feng-value-model-pending.md §5).
 *
 * The five public APIs (retain / release / assign / take / default_init) are
 * all implemented on top of a single internal walker that recursively
 * descends FengManagedSlotDescriptor tables, dispatching only on
 * FengManagedSlotKind. Adding a new by-value aggregate type therefore
 * requires zero changes to this file: codegen emits a new
 * FengAggregateValueDescriptor and the walker handles it transparently
 * (open/closed principle, see §5.4 / §12 of the design doc).
 *
 * The walker calls back into the existing single-pointer primitives
 * (feng_retain / feng_release) on every FENG_SLOT_POINTER slot, so the
 * Phase 1A managed-pointer infrastructure remains the sole authority over
 * refcounting; aggregates never touch FengManagedHeader directly. */
#include "runtime/feng_runtime.h"

#include <string.h>

/* Visitor invoked once per FENG_SLOT_POINTER slot. `pslot` points at the
 * pointer-sized storage of the slot (i.e. (void**)((char*)value + offset)).
 * Nested aggregate slots are recursed through inside the walker itself and
 * never surface to the visitor. */
typedef void (*FengManagedSlotVisitor)(void **pslot, void *ctx);

static void feng_visit_aggregate_managed_slots(
        void *value,
        const FengAggregateValueDescriptor *desc,
        FengManagedSlotVisitor visitor,
        void *ctx) {
    const FengManagedSlotDescriptor *slots = desc->managed_slots;
    size_t count = desc->managed_slot_count;
    char *base = (char *)value;
    for (size_t i = 0; i < count; i++) {
        const FengManagedSlotDescriptor *slot = &slots[i];
        void *slot_addr = base + slot->offset;
        switch (slot->kind) {
            case FENG_SLOT_POINTER:
                visitor((void **)slot_addr, ctx);
                break;
            case FENG_SLOT_NESTED_AGGREGATE:
                /* Nested aggregates are flattened by recursion: the visitor
                 * still only ever sees raw pointer slots. */
                if (slot->nested == NULL) {
                    feng_panic("feng_aggregate: nested slot in '%s' has no descriptor",
                               desc->name != NULL ? desc->name : "<unknown>");
                }
                feng_visit_aggregate_managed_slots(slot_addr, slot->nested,
                                                   visitor, ctx);
                break;
            default:
                feng_panic("feng_aggregate: unknown slot kind %d in '%s'",
                           (int)slot->kind,
                           desc->name != NULL ? desc->name : "<unknown>");
        }
    }
}

static void feng_aggregate_assert_desc(const FengAggregateValueDescriptor *desc,
                                        const char *fn) {
    if (desc == NULL) {
        feng_panic("%s: NULL descriptor", fn);
    }
}

static void feng_aggregate_assert_value(const void *value, const char *fn) {
    if (value == NULL) {
        feng_panic("%s: NULL value", fn);
    }
}

/* --- visitors ---------------------------------------------------------- */

static void visit_retain(void **pslot, void *ctx) {
    (void)ctx;
    /* feng_retain is NULL-safe and immortal-safe. */
    (void)feng_retain(*pslot);
}

static void visit_release(void **pslot, void *ctx) {
    (void)ctx;
    /* feng_release is NULL-safe. */
    feng_release(*pslot);
}

static void visit_null_out(void **pslot, void *ctx) {
    (void)ctx;
    *pslot = NULL;
}

/* --- public API -------------------------------------------------------- */

void feng_aggregate_retain(void *value,
                           const FengAggregateValueDescriptor *desc) {
    feng_aggregate_assert_desc(desc, "feng_aggregate_retain");
    feng_aggregate_assert_value(value, "feng_aggregate_retain");
    feng_visit_aggregate_managed_slots(value, desc, visit_retain, NULL);
}

void feng_aggregate_release(void *value,
                            const FengAggregateValueDescriptor *desc) {
    feng_aggregate_assert_desc(desc, "feng_aggregate_release");
    feng_aggregate_assert_value(value, "feng_aggregate_release");
    feng_visit_aggregate_managed_slots(value, desc, visit_release, NULL);
}

void feng_aggregate_assign(void *dst, const void *src,
                           const FengAggregateValueDescriptor *desc) {
    feng_aggregate_assert_desc(desc, "feng_aggregate_assign");
    feng_aggregate_assert_value(dst, "feng_aggregate_assign");
    feng_aggregate_assert_value(src, "feng_aggregate_assign");

    /* Pointer-identity self-assignment is a no-op: every managed slot
     * already holds the desired reference, and copying the bytes onto
     * themselves would be a memcpy on overlapping regions (UB for
     * memcpy). */
    if (dst == src) {
        return;
    }

    /* Retain-before-release ordering keeps shared subobjects safe: if dst
     * and src happen to share a managed pointer, retaining first ensures
     * the refcount never drops to zero between the release and the final
     * memcpy. */
    feng_visit_aggregate_managed_slots((void *)src, desc, visit_retain, NULL);
    feng_visit_aggregate_managed_slots(dst, desc, visit_release, NULL);
    memcpy(dst, src, desc->size);
}

void feng_aggregate_take(void *dst, void *src,
                         const FengAggregateValueDescriptor *desc) {
    feng_aggregate_assert_desc(desc, "feng_aggregate_take");
    feng_aggregate_assert_value(dst, "feng_aggregate_take");
    feng_aggregate_assert_value(src, "feng_aggregate_take");

    if (dst == src) {
        return;
    }

    /* Drop the destination's managed references, transfer the bytes, then
     * blank out the source's managed slots so that a subsequent
     * feng_aggregate_release on the source is a well-defined no-op
     * (NULL pointers are skipped by visit_release). */
    feng_visit_aggregate_managed_slots(dst, desc, visit_release, NULL);
    memcpy(dst, src, desc->size);
    feng_visit_aggregate_managed_slots(src, desc, visit_null_out, NULL);
}

void feng_aggregate_default_init(void *value_out,
                                 const FengAggregateValueDescriptor *desc) {
    feng_aggregate_assert_desc(desc, "feng_aggregate_default_init");
    feng_aggregate_assert_value(value_out, "feng_aggregate_default_init");
    if (desc->default_init == NULL) {
        feng_panic("feng_aggregate_default_init: '%s' has no default policy",
                   desc->name != NULL ? desc->name : "<unknown>");
    }

    switch (desc->default_init->kind) {
        case FENG_DEFAULT_ZERO_BYTES:
            memset(value_out, 0, desc->size);
            return;
        case FENG_DEFAULT_INIT_FN:
            if (desc->default_init->init_fn == NULL) {
                feng_panic("feng_aggregate_default_init: '%s' missing init_fn",
                           desc->name != NULL ? desc->name : "<unknown>");
            }
            desc->default_init->init_fn(value_out);
            return;
        default:
            feng_panic("feng_aggregate_default_init: unknown default kind %d in '%s'",
                       (int)desc->default_init->kind,
                       desc->name != NULL ? desc->name : "<unknown>");
    }
}
