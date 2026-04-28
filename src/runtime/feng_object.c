/* Reference-counting core: retain/release, barrier helpers, descriptor-driven
 * destruction. All managed objects (FengString, FengArray, user objects) carry
 * FengManagedHeader as their first member, so a single set of helpers handles
 * every kind. */
#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static void feng_finalize_managed(FengManagedHeader *header) {
    void *self = (void *)header;
    bool has_user_finalizer = (header->desc != NULL && header->desc->finalizer != NULL);

    /* User-defined finalizer runs first so that its body still observes a fully
     * constructed object (including managed children). Per
     * docs/feng-lifetime.md §13.2 ARC path, the runtime must re-check the
     * refcount after the finalizer returns: if user code published `self` (or
     * indirectly increased the refcount) during finalization, the object has
     * been resurrected and MUST NOT be freed in this pass; the next time the
     * refcount falls to zero, the finalizer will run again. */
    if (has_user_finalizer) {
        header->desc->finalizer(self);

        /* Acquire fences pair with the release-CAS in feng_retain so that any
         * stores another thread published before its retain become visible
         * here when we observe a resurrected refcount. */
        uint32_t current = atomic_load_explicit((_Atomic uint32_t *)&header->refcount,
                                                memory_order_acquire);
        if (current > 0U) {
            /* Resurrected. Per §13.2, the per-tag child cleanup AND the
             * codegen-emitted release_children are both postponed: only the
             * user finalizer runs in this pass. The next release will
             * re-enter this routine and re-run the finalizer. */
            return;
        }
    }

    /* Codegen-emitted release_children: drops each managed field's reference.
     * Runs unconditionally for any descriptor that provides it; for built-in
     * tags (string/array) this slot is NULL and child cleanup is performed
     * by the tag-specific hook below. */
    if (header->desc != NULL && header->desc->release_children != NULL) {
        header->desc->release_children(self);
    }

    /* Tag-specific child release. Built-in string/array carry no user
     * finalizer and no release_children; their internal cleanup runs here.
     * Object/closure tags have already had their managed children dropped
     * by release_children above. */
    switch (header->tag) {
        case FENG_TYPE_TAG_STRING:
            feng_string_finalize_internal((struct FengString *)self);
            break;
        case FENG_TYPE_TAG_ARRAY:
            feng_array_finalize_internal((struct FengArray *)self);
            break;
        case FENG_TYPE_TAG_OBJECT:
        case FENG_TYPE_TAG_CLOSURE:
            break;
    }

    free(header);
}

void *feng_retain(void *obj) {
    FengManagedHeader *header;

    if (obj == NULL) {
        return NULL;
    }

    header = (FengManagedHeader *)obj;
    if (header->refcount == FENG_REFCOUNT_IMMORTAL) {
        return obj;
    }

    (void)atomic_fetch_add_explicit((_Atomic uint32_t *)&header->refcount,
                                    (uint32_t)1,
                                    memory_order_relaxed);
    return obj;
}

void feng_release(void *obj) {
    FengManagedHeader *header;
    uint32_t previous;
    bool potentially_cyclic;

    if (obj == NULL) {
        return;
    }

    header = (FengManagedHeader *)obj;
    if (header->refcount == FENG_REFCOUNT_IMMORTAL) {
        return;
    }

    /* For potentially-cyclic objects we serialise the entire release path
     * behind the cycle collector mutex. The collector relies on having a
     * stable view of every retain/release affecting candidate-eligible
     * objects (Phase 1B-3 STW model). Acyclic objects bypass the lock
     * entirely so the ARC fast path stays contention-free. */
    potentially_cyclic = (header->desc != NULL && header->desc->is_potentially_cyclic);
    if (potentially_cyclic) {
        feng_cycle_lock();
    }

    previous = atomic_fetch_sub_explicit((_Atomic uint32_t *)&header->refcount,
                                         (uint32_t)1,
                                         memory_order_acq_rel);
    if (previous == 1U) {
        /* Refcount reached zero. If the object is currently buffered as a
         * cycle candidate, scrub the entry first so the next collection
         * cycle does not dereference freed memory. */
        if (potentially_cyclic) {
            feng_cycle_remove_candidate(header);
        }
        feng_finalize_managed(header);
    } else if (previous == 0U) {
        /* Refcount underflow indicates a codegen bug; abort early so the
         * regression is impossible to miss. */
        feng_panic("feng_release: refcount underflow on '%s'",
                   header->desc != NULL && header->desc->name != NULL ? header->desc->name
                                                                      : "<unknown>");
    } else if (potentially_cyclic) {
        /* Survived the release but might have just become the root of an
         * unreachable cycle. Enqueue for the next collection sweep. */
        feng_cycle_enqueue_candidate(header);
    }

    if (potentially_cyclic) {
        feng_cycle_unlock();
    }
}

void feng_assign(void **slot, void *new_value) {
    void *old;

    if (slot == NULL) {
        return;
    }

    old = *slot;
    *slot = feng_retain(new_value);
    feng_release(old);
}

void *feng_take(void **slot) {
    void *value;

    if (slot == NULL) {
        return NULL;
    }

    value = *slot;
    *slot = NULL;
    return value;
}

void *feng_object_new(const FengTypeDescriptor *desc) {
    FengManagedHeader *header;

    if (desc == NULL || desc->size < sizeof(FengManagedHeader)) {
        feng_panic("feng_object_new: invalid descriptor");
    }

    header = (FengManagedHeader *)calloc(1, desc->size);
    if (header == NULL) {
        feng_panic("feng_object_new: out of memory for '%s'",
                   desc->name != NULL ? desc->name : "<unknown>");
    }

    header->desc = desc;
    header->tag = FENG_TYPE_TAG_OBJECT;
    header->refcount = 1U;
    return header;
}
