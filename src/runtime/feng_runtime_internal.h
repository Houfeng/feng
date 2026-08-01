/* Internal layouts shared between runtime translation units. NOT part of the
 * codegen ABI: generated C code must use the accessor functions in
 * feng_runtime.h instead of poking these fields directly. */
#ifndef FENG_RUNTIME_INTERNAL_H
#define FENG_RUNTIME_INTERNAL_H

#include "runtime/feng_runtime.h"

struct FengString {
    FengManagedHeader header;
    size_t length;
    /* Null-terminated UTF-8 bytes; allocation extends to length + 1. */
    char data[];
};

struct FengArray {
    FengManagedHeader header;
    /* Number of initialized elements, not bytes. */
    size_t length;
    /* Number of tail-inline element slots allocated for this instance. */
    size_t capacity;
    /* Per-element storage size in bytes (fixed per array instance). */
    size_t element_size;
    const FengTypeDescriptor *element_desc;
    /* Three-way classification per docs/engineering/feng-value-model-delivered.md §7.3.
     * TRIVIAL    — bytes only.
     * MANAGED_POINTER — payload is interpreted as `void *` slots.
     * AGGREGATE  — payload is interpreted as by-value aggregates of size
     *              `element_aggregate->size`; `element_aggregate` MUST be
     *              non-NULL when element_kind == AGGREGATE. */
    FengValueKind element_kind;
    const FengAggregateDescriptor *element_aggregate;
    /* No payload pointer/placeholder member exists in this struct.
     * Element bytes live in the same allocation, tail-inline after an
     * alignment-adjusted offset computed by feng_array_payload_inline(). */
};

/* Tag-specific child cleanup invoked by feng_release after refcount drops to 0,
 * before the user-defined finalizer (when present) and the final free. */
void feng_string_finalize_internal(struct FengString *s);
void feng_array_finalize_internal(struct FengArray *a);

/* Internal array payload accessors.
 *
 * Both helpers return NULL when `a == NULL` or `a->capacity == 0`.
 * Otherwise they return the aligned tail-inline payload address inside the
 * same allocation as `struct FengArray`, including when `a->length == 0`.
 *
 * All runtime paths (array accessors/finalize/collector) must reuse these
 * helpers instead of open-coding offset math. */
void *feng_array_payload_inline(struct FengArray *a);
const void *feng_array_payload_inline_const(const struct FengArray *a);

/* Creates a fresh +1 internal array storage allocation. `length` describes
 * the initialized prefix and must not exceed the immutable `capacity`.
 * Allocation size is based on `capacity`, while default initialization and
 * all subsequent lifecycle traversal are restricted to `[0, length)`. */
FengArray *feng_array_new_storage_kinded(
    FengValueKind element_kind,
    const FengAggregateDescriptor *element_aggregate,
    const FengTypeDescriptor *element_desc,
    size_t element_size,
    size_t length,
    size_t capacity);

/* Copy `length` elements from `src[start, start+length)` into `dst[0, length)`.
 * Retain/release semantics are handled per element kind. Caller must ensure
 * both arrays are kind-compatible and ranges are in-bounds. */
void feng_array_slice_copy_into(struct FengArray *dst,
                                const struct FengArray *src,
                                size_t start,
                                size_t length);

/* Release every managed slot in an aggregate after first clearing that slot.
 * This ordering keeps collector traversal from observing a pointer whose
 * ownership has already been dropped. Non-managed bytes are left unchanged. */
void feng_aggregate_release_and_clear_internal(
    void *value,
    const FengAggregateDescriptor *desc);

/* Invoke a user-declared finalizer behind a sentinel exception barrier. Per
 * docs/specifications/feng-lifetime.md §13.2 / docs/specifications/feng-type.md, a finalizer must not let
 * an exception propagate past its body; if one does, the runtime panics
 * immediately rather than unwinding across ARC/collector C frames (which
 * would skip all subsequent ARC bookkeeping and corrupt the candidate
 * buffer). Both the ARC release path and the cycle collector's Phase 1
 * route every user-finalizer call through this helper. Caller is responsible
 * for the NULL-check on `desc->finalizer` only as an optimisation; the
 * helper itself is a no-op when either `desc` or `desc->finalizer` is NULL. */
void feng_finalizer_invoke(const FengTypeDescriptor *desc, void *self);
void feng_exception_enter_finalizer(void);
void feng_exception_leave_finalizer(void);

/* Test-only override for the cycle collector candidate-buffer threshold.
 * Replaces whatever value cycle_init_once read from FENG_GC_THRESHOLD (or
 * the default). Caller MUST hold feng_cycle_lock; intended for regression
 * tests that need to exercise the threshold-triggered collection path
 * deterministically. Production code MUST NOT call this. */
void feng_cycle_set_threshold_for_test(size_t threshold);

/* --- Phase 1B cycle collector internal API ------------------------------
 *
 * The collector lives entirely inside the runtime TU set; codegen never sees
 * any of these symbols. The single integration point with the ARC fast path
 * is `feng_cycle_enqueue_candidate`, called by feng_release whenever an
 * object whose descriptor is marked `is_potentially_cyclic` has its refcount
 * decremented to a positive value (i.e. the object survived the release but
 * may now be part of an unreachable cycle). The collector serialises all of
 * its state behind a single recursive mutex so that retain/release on
 * potentially-cyclic objects across threads remain race-free. */

/* Acquire the global collector mutex (recursive). All retain/release on
 * potentially-cyclic objects pass through this lock so the candidate buffer
 * and per-object cycle_state bits are only ever mutated under STW. */
void feng_cycle_lock(void);
void feng_cycle_unlock(void);

/* Add `header` to the candidate buffer if it is not already buffered, then
 * trigger a collection cycle when the buffer reaches the configured
 * threshold. Caller MUST hold feng_cycle_lock. */
void feng_cycle_enqueue_candidate(struct FengManagedHeader *header);

/* Remove `header` from the candidate buffer if present and clear its
 * BUFFERED flag. Used by feng_release when an object's refcount reaches
 * zero before the next collection cycle: the entry would otherwise be a
 * dangling pointer to freed memory at collect time. Caller MUST hold
 * feng_cycle_lock. */
void feng_cycle_remove_candidate(struct FengManagedHeader *header);

/* Force-process the candidate buffer immediately (used by tests and by the
 * runtime shutdown path). Caller MUST hold feng_cycle_lock. */
void feng_cycle_collect_locked(void);

/* Release any heap retained by the collector itself. Safe to call multiple
 * times; intended for test-suite tear-down so leak checkers stay quiet. */
void feng_cycle_runtime_shutdown(void);

#endif /* FENG_RUNTIME_INTERNAL_H */
