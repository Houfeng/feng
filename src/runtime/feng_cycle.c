/* Phase 1B cycle collector — Bacon-Rajan trial-deletion.
 *
 * Spec: docs/feng-lifetime.md §13.1 (cycle collector contract) and §11
 * (potentially-cyclic SCC metadata).
 *
 * Concurrency model (1B-3 baseline):
 *   - A single global recursive mutex serialises every mutation of the
 *     candidate buffer and every traversal of the per-object cycle_state.
 *     The lock is recursive so that the ARC fast path can re-enter through
 *     itself (feng_release on a potentially-cyclic object during normal
 *     execution acquires the lock, enqueues, and may transitively release
 *     other objects via the user finalizer; those calls must not deadlock).
 *   - The collector is stop-the-world: while feng_cycle_collect_locked runs,
 *     every retain/release on potentially-cyclic objects is blocked at
 *     feng_cycle_lock. Acyclic objects bypass the lock entirely so the
 *     ARC fast path remains contention-free.
 *
 * Triggering:
 *   - The collector runs when the candidate buffer reaches a size threshold.
 *     The default threshold is FENG_CYCLE_DEFAULT_THRESHOLD; users may
 *     override it via the FENG_GC_THRESHOLD environment variable (≥1).
 *
 * Algorithm (Bacon & Rajan, 2001 — synchronous trial-deletion variant):
 *   1. Mark phase: from each candidate, recursively decrement the refcount
 *      of every reachable managed child once and colour newly-visited
 *      objects GREY.
 *   2. Scan phase: from each candidate, partition GREY into BLACK (refcount
 *      survived a non-zero residual → externally referenced) and WHITE
 *      (refcount hit zero → only internal cycle references remain).
 *      scan_black restores refcounts on the BLACK closure.
 *   3. Collect phase: every WHITE object is freed.
 *
 * Cycle path with finalizers (Phase 1B-4 — docs/feng-lifetime.md §13.2):
 *   Once the WHITE set is determined, if any white-coloured object has a
 *   user finalizer (FengTypeDescriptor.finalizer != NULL), the collector
 *   runs the §13.2 two-phase protocol:
 *     - Phase 1   : invoke every white object's user finalizer once. Order
 *                   is unspecified. All white memory remains valid for the
 *                   duration so cross-object field reads in finalizers do
 *                   not produce use-after-free.
 *     - Phase 1.5 : recompute the per-white internal refcount (the inbound
 *                   intra-component edge count) and compare against the
 *                   live refcount. Any white whose live refcount exceeds its
 *                   internal count was resurrected externally; from each
 *                   such root run a BFS along intra-white outgoing edges,
 *                   marking the closure SURVIVED. The remainder of the
 *                   white set forms free_set.
 *     - Phase 2   : free every object in free_set. The codegen-emitted
 *                   release_children callback is NOT invoked on cycle-path
 *                   frees: components are reclaimed as a unit, and BLACK
 *                   external children referenced by white objects are
 *                   handled separately (see free_external_refs_of_whites). */

#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Configuration ------------------------------------------------------ */

#define FENG_CYCLE_DEFAULT_THRESHOLD ((size_t)256)
#define FENG_CYCLE_INITIAL_CAPACITY  ((size_t)32)

/* --- Bit layout for FengManagedHeader.cycle_state ---------------------- */

#define CYC_COLOR_MASK     ((uint32_t)0x3u)
#define CYC_COLOR_BLACK    ((uint32_t)0x0u)
#define CYC_COLOR_GREY     ((uint32_t)0x1u)
#define CYC_COLOR_WHITE    ((uint32_t)0x2u)
/* PURPLE (0x3) is reserved for a future incremental collector. */

#define CYC_FLAG_BUFFERED  ((uint32_t)0x4u)
#define CYC_FLAG_VISITED   ((uint32_t)0x8u) /* used by collect_white pass */

static inline uint32_t cyc_color(const FengManagedHeader *h) {
    return h->cycle_state & CYC_COLOR_MASK;
}

static inline void cyc_set_color(FengManagedHeader *h, uint32_t color) {
    h->cycle_state = (h->cycle_state & ~CYC_COLOR_MASK) | (color & CYC_COLOR_MASK);
}

/* --- Collector state --------------------------------------------------- */

static pthread_once_t g_cycle_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_cycle_mutex;

static FengManagedHeader **g_candidates = NULL;
static size_t g_candidate_count = 0U;
static size_t g_candidate_capacity = 0U;
static size_t g_threshold = 0U;

static void cycle_init_once(void) {
    pthread_mutexattr_t attr;
    int rc;

    rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        feng_panic("feng_cycle: pthread_mutexattr_init failed (%d)", rc);
    }
    rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (rc != 0) {
        feng_panic("feng_cycle: pthread_mutexattr_settype failed (%d)", rc);
    }
    rc = pthread_mutex_init(&g_cycle_mutex, &attr);
    if (rc != 0) {
        feng_panic("feng_cycle: pthread_mutex_init failed (%d)", rc);
    }
    pthread_mutexattr_destroy(&attr);

    /* Threshold: read once on first lock acquisition. The env var lookup is
     * stable across the process lifetime; mid-run changes are intentionally
     * ignored to keep the trigger deterministic. */
    const char *env = getenv("FENG_GC_THRESHOLD");
    g_threshold = FENG_CYCLE_DEFAULT_THRESHOLD;
    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != NULL && *end == '\0' && v >= 1ULL && v <= (unsigned long long)SIZE_MAX) {
            g_threshold = (size_t)v;
        }
    }
}

void feng_cycle_lock(void) {
    pthread_once(&g_cycle_once, cycle_init_once);
    int rc = pthread_mutex_lock(&g_cycle_mutex);
    if (rc != 0) {
        feng_panic("feng_cycle: pthread_mutex_lock failed (%d)", rc);
    }
}

void feng_cycle_unlock(void) {
    int rc = pthread_mutex_unlock(&g_cycle_mutex);
    if (rc != 0) {
        feng_panic("feng_cycle: pthread_mutex_unlock failed (%d)", rc);
    }
}

/* --- Candidate buffer --------------------------------------------------- */

static void candidates_reserve_one(void) {
    if (g_candidate_count < g_candidate_capacity) {
        return;
    }
    size_t new_cap = (g_candidate_capacity == 0U)
                     ? FENG_CYCLE_INITIAL_CAPACITY
                     : g_candidate_capacity * 2U;
    FengManagedHeader **new_buf = (FengManagedHeader **)realloc(
        g_candidates, new_cap * sizeof(*new_buf));
    if (new_buf == NULL) {
        feng_panic("feng_cycle: out of memory growing candidate buffer to %zu", new_cap);
    }
    g_candidates = new_buf;
    g_candidate_capacity = new_cap;
}

void feng_cycle_enqueue_candidate(FengManagedHeader *header) {
    if (header == NULL) {
        return;
    }
    /* Already buffered → no need to add again. The flag is mutated only under
     * the collector mutex, so this read is race-free given our calling
     * contract (caller holds feng_cycle_lock). */
    if (header->cycle_state & CYC_FLAG_BUFFERED) {
        return;
    }
    candidates_reserve_one();
    g_candidates[g_candidate_count++] = header;
    header->cycle_state |= CYC_FLAG_BUFFERED;

    if (g_candidate_count >= g_threshold) {
        feng_cycle_collect_locked();
    }
}

/* Scrub `header` from the candidate buffer. Called by feng_release when an
 * object's refcount drops to zero while the BUFFERED flag is set: leaving
 * the stale pointer in the buffer would crash the next collection cycle. */
void feng_cycle_remove_candidate(FengManagedHeader *header) {
    if (header == NULL) {
        return;
    }
    if (!(header->cycle_state & CYC_FLAG_BUFFERED)) {
        return;
    }
    for (size_t i = 0U; i < g_candidate_count; ++i) {
        if (g_candidates[i] == header) {
            /* Swap-with-last to avoid O(N) shifting. Order is not material;
             * the collector treats the buffer as a set. */
            g_candidates[i] = g_candidates[g_candidate_count - 1U];
            --g_candidate_count;
            break;
        }
    }
    header->cycle_state &= ~CYC_FLAG_BUFFERED;
}

/* --- Child traversal --------------------------------------------------- */

/* Visit every managed child slot of `header` and pass each non-NULL pointer
 * to `visit(child, ctx)`. The traversal handles all four built-in tags:
 *
 *   OBJECT/CLOSURE — uses descriptor->managed_fields (static trace table
 *                    emitted by codegen).
 *   ARRAY          — iterates items[0..length) when element_is_managed.
 *   STRING         — has no managed children.
 *
 * Note: array/string descriptors are themselves !is_potentially_cyclic, so
 * arrays/strings never enter the candidate buffer directly. They are only
 * encountered as transitively-reachable children of a user object whose
 * cyclic SCC includes them. */
static void cyc_for_each_child(FengManagedHeader *header,
                               void (*visit)(FengManagedHeader *child, void *ctx),
                               void *ctx) {
    if (header == NULL || header->desc == NULL) {
        return;
    }

    switch (header->tag) {
        case FENG_TYPE_TAG_OBJECT:
        case FENG_TYPE_TAG_CLOSURE: {
            const FengTypeDescriptor *desc = header->desc;
            for (size_t i = 0U; i < desc->managed_field_count; ++i) {
                size_t off = desc->managed_fields[i].offset;
                FengManagedHeader **slot =
                    (FengManagedHeader **)((char *)header + off);
                FengManagedHeader *child = *slot;
                if (child != NULL) {
                    visit(child, ctx);
                }
            }
            break;
        }
        case FENG_TYPE_TAG_ARRAY: {
            struct FengArray *arr = (struct FengArray *)header;
            if (!arr->element_is_managed || arr->items == NULL) {
                break;
            }
            FengManagedHeader **slots = (FengManagedHeader **)arr->items;
            for (size_t i = 0U; i < arr->length; ++i) {
                FengManagedHeader *child = slots[i];
                if (child != NULL) {
                    visit(child, ctx);
                }
            }
            break;
        }
        case FENG_TYPE_TAG_STRING:
            break;
    }
}

/* --- Bacon-Rajan trial deletion ---------------------------------------- */

/* Helpers that mutate refcount during STW. We use atomic ops so that the
 * memory model stays consistent with concurrent retain/release on acyclic
 * objects (which never enter the collector but may share cache lines with
 * candidate objects in pathological layouts). */

static inline uint32_t cyc_load_rc(FengManagedHeader *h) {
    return atomic_load_explicit((_Atomic uint32_t *)&h->refcount,
                                memory_order_relaxed);
}

static inline void cyc_dec_rc(FengManagedHeader *h) {
    (void)atomic_fetch_sub_explicit((_Atomic uint32_t *)&h->refcount,
                                    (uint32_t)1, memory_order_relaxed);
}

static inline void cyc_inc_rc(FengManagedHeader *h) {
    (void)atomic_fetch_add_explicit((_Atomic uint32_t *)&h->refcount,
                                    (uint32_t)1, memory_order_relaxed);
}

static void mark_grey_visit(FengManagedHeader *child, void *ctx);

static void mark_grey(FengManagedHeader *h) {
    if (h == NULL) {
        return;
    }
    /* Immortal objects never participate in cycle collection. */
    if (h->refcount == FENG_REFCOUNT_IMMORTAL) {
        return;
    }
    if (cyc_color(h) == CYC_COLOR_GREY) {
        return;
    }
    cyc_set_color(h, CYC_COLOR_GREY);
    cyc_for_each_child(h, mark_grey_visit, NULL);
}

static void mark_grey_visit(FengManagedHeader *child, void *ctx) {
    (void)ctx;
    if (child == NULL || child->refcount == FENG_REFCOUNT_IMMORTAL) {
        return;
    }
    cyc_dec_rc(child);
    mark_grey(child);
}

static void scan_black_visit(FengManagedHeader *child, void *ctx);

static void scan_black(FengManagedHeader *h) {
    cyc_set_color(h, CYC_COLOR_BLACK);
    cyc_for_each_child(h, scan_black_visit, NULL);
}

static void scan_black_visit(FengManagedHeader *child, void *ctx) {
    (void)ctx;
    if (child == NULL || child->refcount == FENG_REFCOUNT_IMMORTAL) {
        return;
    }
    cyc_inc_rc(child);
    if (cyc_color(child) != CYC_COLOR_BLACK) {
        scan_black(child);
    }
}

static void scan_visit(FengManagedHeader *child, void *ctx);

static void scan(FengManagedHeader *h) {
    if (h == NULL || cyc_color(h) != CYC_COLOR_GREY) {
        return;
    }
    if (cyc_load_rc(h) > 0U) {
        scan_black(h);
        return;
    }
    cyc_set_color(h, CYC_COLOR_WHITE);
    cyc_for_each_child(h, scan_visit, NULL);
}

static void scan_visit(FengManagedHeader *child, void *ctx) {
    (void)ctx;
    scan(child);
}

/* Walk the white set rooted at `h`, accumulating each unique white into
 * `out`. Uses the VISITED flag to dedupe. */
typedef struct {
    FengManagedHeader **buf;
    size_t count;
    size_t capacity;
} WhiteList;

static void white_list_push(WhiteList *wl, FengManagedHeader *h) {
    if (wl->count >= wl->capacity) {
        size_t new_cap = (wl->capacity == 0U) ? 16U : wl->capacity * 2U;
        FengManagedHeader **nb = (FengManagedHeader **)realloc(
            wl->buf, new_cap * sizeof(*nb));
        if (nb == NULL) {
            feng_panic("feng_cycle: out of memory growing white list to %zu", new_cap);
        }
        wl->buf = nb;
        wl->capacity = new_cap;
    }
    wl->buf[wl->count++] = h;
}

static void gather_white_visit(FengManagedHeader *child, void *ctx);

static void gather_white(FengManagedHeader *h, WhiteList *wl) {
    if (h == NULL) {
        return;
    }
    if (cyc_color(h) != CYC_COLOR_WHITE) {
        return;
    }
    if (h->cycle_state & CYC_FLAG_VISITED) {
        return;
    }
    h->cycle_state |= CYC_FLAG_VISITED;
    white_list_push(wl, h);
    cyc_for_each_child(h, gather_white_visit, wl);
}

static void gather_white_visit(FengManagedHeader *child, void *ctx) {
    gather_white(child, (WhiteList *)ctx);
}

/* ---- Phase 1B-4: writable-slot traversal for sanitisation ------------- */

/* Same iteration shape as cyc_for_each_child, but the visitor receives the
 * address of the field slot in addition to the current child pointer so it
 * can rewrite the slot in place (used to NULL out survivor → free-set
 * dangling references in Phase 1.5). */
static void cyc_for_each_child_slot(FengManagedHeader *header,
                                    void (*visit)(FengManagedHeader **slot,
                                                   FengManagedHeader *child,
                                                   void *ctx),
                                    void *ctx) {
    if (header == NULL || header->desc == NULL) {
        return;
    }
    switch (header->tag) {
        case FENG_TYPE_TAG_OBJECT:
        case FENG_TYPE_TAG_CLOSURE: {
            const FengTypeDescriptor *desc = header->desc;
            for (size_t i = 0U; i < desc->managed_field_count; ++i) {
                size_t off = desc->managed_fields[i].offset;
                FengManagedHeader **slot =
                    (FengManagedHeader **)((char *)header + off);
                visit(slot, *slot, ctx);
            }
            break;
        }
        case FENG_TYPE_TAG_ARRAY: {
            struct FengArray *arr = (struct FengArray *)header;
            if (!arr->element_is_managed || arr->items == NULL) {
                break;
            }
            FengManagedHeader **slots = (FengManagedHeader **)arr->items;
            for (size_t i = 0U; i < arr->length; ++i) {
                visit(&slots[i], slots[i], ctx);
            }
            break;
        }
        case FENG_TYPE_TAG_STRING:
            break;
    }
}

static inline bool is_white(const FengManagedHeader *h) {
    return h != NULL
        && h->refcount != FENG_REFCOUNT_IMMORTAL
        && cyc_color(h) == CYC_COLOR_WHITE;
}

/* ---- Phase 0: restore intra-white in-refcount ------------------------- */
/*
 * After mark_grey/scan, every white object's refcount has been decremented
 * once per inbound intra-grey-component edge AND never restored (scan_black
 * only restored objects on the BLACK closure). To make the §13.2 Phase 1.5
 * resurrection check meaningful — "actual rc > intra-component in-degree"
 * — we re-inflate each white's refcount by exactly the number of inbound
 * edges from other whites. After Phase 0 a white's refcount equals its
 * "would-be" rc if the component were live: external rc (== 0 here, since
 * the cycle is otherwise unreachable) + intra-white in-edges. */
static void phase0_restore_visit(FengManagedHeader *child, void *ctx) {
    (void)ctx;
    if (is_white(child)) {
        cyc_inc_rc(child);
    }
}

static void phase0_restore_intra_white_rc(WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        cyc_for_each_child(wl->buf[i], phase0_restore_visit, NULL);
    }
}

/* ---- Phase 1: invoke user finalizers --------------------------------- */
/*
 * The §13.2 cycle path runs ONLY user-declared finalizers
 * (descriptor->finalizer); the codegen-emitted release_children callback is
 * NEVER invoked here because the component is being reclaimed as a unit and
 * per-field releases would dec refcounts of objects about to be freed
 * alongside (and underflow them). Built-in tags (string/array) have no user
 * finalizer and are skipped. */
static void phase1_run_finalizers(WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        if (h->desc != NULL && h->desc->finalizer != NULL) {
            h->desc->finalizer((void *)h);
        }
    }
}

/* ---- Phase 1.5: resurrection re-check + survivor BFS ------------------ */

typedef struct {
    /* Per-white auxiliary slot. Indexed in lockstep with WhiteList.buf. */
    size_t intra_in;     /* intra-white in-degree, recomputed after Phase 1 */
} WhiteAux;

/* Find the index of `h` in wl. Linear scan; the white set is bounded by
 * candidate-buffer-rooted closure size which is small relative to total
 * heap, and this only runs during STW collection, not on the ARC fast
 * path. */
static size_t white_index_of(const WhiteList *wl, const FengManagedHeader *h) {
    for (size_t i = 0U; i < wl->count; ++i) {
        if (wl->buf[i] == h) {
            return i;
        }
    }
    return SIZE_MAX;
}

typedef struct {
    const WhiteList *wl;
    WhiteAux *aux;
} CountCtx;

static void count_intra_in_visit(FengManagedHeader *child, void *ctx) {
    if (!is_white(child)) {
        return;
    }
    CountCtx *cc = (CountCtx *)ctx;
    size_t idx = white_index_of(cc->wl, child);
    if (idx != SIZE_MAX) {
        cc->aux[idx].intra_in += 1U;
    }
}

static void phase15_compute_intra_in(WhiteList *wl, WhiteAux *aux) {
    for (size_t i = 0U; i < wl->count; ++i) {
        aux[i].intra_in = 0U;
    }
    CountCtx cc = { wl, aux };
    for (size_t i = 0U; i < wl->count; ++i) {
        cyc_for_each_child(wl->buf[i], count_intra_in_visit, &cc);
    }
}

/* BFS along intra-white out-edges starting from each externally-resurrected
 * white. Survivors are tagged with CYC_FLAG_VISITED (which is otherwise
 * unused at this point — gather_white cleared it before we got here). */
static void phase15_mark_survivors_bfs(WhiteList *wl, WhiteAux *aux,
                                       FengManagedHeader **queue,
                                       size_t *queue_size_out) {
    size_t qhead = 0U;
    size_t qtail = 0U;

    /* Seed: every white whose actual rc exceeds its intra-white in-degree
     * has gained an external reference (resurrection) during Phase 1. */
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        if (cyc_load_rc(h) > aux[i].intra_in) {
            h->cycle_state |= CYC_FLAG_VISITED; /* SURVIVED */
            queue[qtail++] = h;
        }
    }

    while (qhead < qtail) {
        FengManagedHeader *s = queue[qhead++];
        const FengTypeDescriptor *desc = s->desc;
        /* Walk children inline to enqueue any white not yet survived. */
        if (s->tag == FENG_TYPE_TAG_OBJECT || s->tag == FENG_TYPE_TAG_CLOSURE) {
            for (size_t k = 0U; desc != NULL && k < desc->managed_field_count; ++k) {
                FengManagedHeader **slot =
                    (FengManagedHeader **)((char *)s + desc->managed_fields[k].offset);
                FengManagedHeader *c = *slot;
                if (is_white(c) && !(c->cycle_state & CYC_FLAG_VISITED)) {
                    c->cycle_state |= CYC_FLAG_VISITED;
                    queue[qtail++] = c;
                }
            }
        } else if (s->tag == FENG_TYPE_TAG_ARRAY) {
            struct FengArray *arr = (struct FengArray *)s;
            if (arr->element_is_managed && arr->items != NULL) {
                FengManagedHeader **slots = (FengManagedHeader **)arr->items;
                for (size_t k = 0U; k < arr->length; ++k) {
                    FengManagedHeader *c = slots[k];
                    if (is_white(c) && !(c->cycle_state & CYC_FLAG_VISITED)) {
                        c->cycle_state |= CYC_FLAG_VISITED;
                        queue[qtail++] = c;
                    }
                }
            }
        }
    }
    *queue_size_out = qtail;
}

static inline bool is_survivor(const FengManagedHeader *h) {
    return is_white(h) && (h->cycle_state & CYC_FLAG_VISITED);
}

static inline bool is_free_member(const FengManagedHeader *h) {
    return is_white(h) && !(h->cycle_state & CYC_FLAG_VISITED);
}

/* Sanitise pass:
 *   (a) Survivor → free edges: NULL the slot in the survivor so it cannot
 *       UAF the about-to-be-freed object on subsequent field reads. We do
 *       NOT touch the free object's rc (it dies regardless).
 *   (b) Free → survivor edges: dec the survivor's rc once per such edge so
 *       the survivor's post-collection rc reflects only refs from objects
 *       that still exist (other survivors + true externals).
 * (a) handlers run as the survivor pass; (b) as the free pass. */
static void survivor_sanitise_visit(FengManagedHeader **slot,
                                    FengManagedHeader *child,
                                    void *ctx) {
    (void)ctx;
    if (is_free_member(child)) {
        *slot = NULL; /* sever; do not dec child.rc — child is being freed */
    }
}

static void free_sanitise_visit(FengManagedHeader **slot,
                                FengManagedHeader *child,
                                void *ctx) {
    (void)slot;
    (void)ctx;
    if (is_survivor(child)) {
        /* Free → survivor edge dies with us. Dec survivor's rc so its
         * post-collection rc is correct. */
        cyc_dec_rc(child);
    }
}

static void phase15_sanitise(WhiteList *wl) {
    /* Survivor → free pass first so we don't accidentally leave a survivor
     * pointing into about-to-be-freed memory if Phase 2 reorders. */
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        if (is_survivor(h)) {
            cyc_for_each_child_slot(h, survivor_sanitise_visit, NULL);
        }
    }
    /* Free → survivor: dec survivor rcs. */
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        if (is_free_member(h)) {
            cyc_for_each_child_slot(h, free_sanitise_visit, NULL);
        }
    }
}

static void phase15_restore_survivor_state(WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        if (is_survivor(h)) {
            cyc_set_color(h, CYC_COLOR_BLACK);
            h->cycle_state &= ~(CYC_FLAG_VISITED | CYC_FLAG_BUFFERED);
        }
    }
}

/* ---- Phase 2: free unsurvived whites --------------------------------- */
/*
 * No release_children invocation: the only remaining out-edges from a free
 * member point either to other free members (their memory is also being
 * freed; no rc maintenance required) or to BLACK external objects whose
 * refcount was already balanced by mark_grey's decrement of this dying
 * edge. Survivor → free and free → survivor edges have been handled by
 * phase15_sanitise. Built-in tags (array's items buffer) get their
 * tag-specific heap freed. */
static void phase2_free_unsurvived(WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        if (!is_free_member(h)) {
            continue;
        }
        switch (h->tag) {
            case FENG_TYPE_TAG_ARRAY: {
                struct FengArray *arr = (struct FengArray *)h;
                free(arr->items);
                arr->items = NULL;
                free(h);
                break;
            }
            case FENG_TYPE_TAG_STRING:
            case FENG_TYPE_TAG_OBJECT:
            case FENG_TYPE_TAG_CLOSURE:
                free(h);
                break;
        }
    }
}

/* ---- Whole-set free for the no-finalizer fast path ------------------- */
/*
 * When no white has a user finalizer there is no resurrection window, so we
 * skip Phase 0/1/1.5 entirely and free the whole white set. Same correctness
 * argument as phase2: BLACK external children's rc is already correct; intra-
 * white edges die together. */
static void free_whole_white_set(WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        FengManagedHeader *h = wl->buf[i];
        switch (h->tag) {
            case FENG_TYPE_TAG_ARRAY: {
                struct FengArray *arr = (struct FengArray *)h;
                free(arr->items);
                arr->items = NULL;
                free(h);
                break;
            }
            case FENG_TYPE_TAG_STRING:
            case FENG_TYPE_TAG_OBJECT:
            case FENG_TYPE_TAG_CLOSURE:
                free(h);
                break;
        }
    }
}

static bool whites_contain_finalizer(const WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        const FengManagedHeader *h = wl->buf[i];
        if (h->desc != NULL && h->desc->finalizer != NULL) {
            return true;
        }
    }
    return false;
}

/* Clear the VISITED flag on every white. gather_white set it to dedupe the
 * collection walk; we re-purpose the bit as the "survived" marker in Phase
 * 1.5, so we must ensure the slate is clean before then. */
static void clear_visited_on_whites(WhiteList *wl) {
    for (size_t i = 0U; i < wl->count; ++i) {
        wl->buf[i]->cycle_state &= ~CYC_FLAG_VISITED;
    }
}

void feng_cycle_collect_locked(void) {
    if (g_candidate_count == 0U) {
        return;
    }

    /* Snapshot the candidate buffer and clear it. Mid-collection feng_release
     * calls (from user finalizers in Phase 1) may legitimately enqueue new
     * candidates against the empty buffer; those will be processed by a
     * subsequent collection cycle, not folded into the current one. */
    size_t cand_count = g_candidate_count;
    FengManagedHeader **cands = g_candidates;
    g_candidates = NULL;
    g_candidate_count = 0U;
    g_candidate_capacity = 0U;

    /* Mark phase: dec rc on every reachable child, recursively. */
    for (size_t i = 0U; i < cand_count; ++i) {
        FengManagedHeader *h = cands[i];
        h->cycle_state &= ~CYC_FLAG_BUFFERED;
        mark_grey(h);
    }

    /* Scan phase: partition GREY → BLACK (externally referenced, restored)
     * vs WHITE (only intra-component refs, refcount left at 0). */
    for (size_t i = 0U; i < cand_count; ++i) {
        scan(cands[i]);
    }

    /* Gather the unique white set rooted at the candidates. gather_white
     * sets CYC_FLAG_VISITED on each enrolled white; we clear it immediately
     * because Phase 1.5 reuses the same bit as the "survived" marker. */
    WhiteList whites = { NULL, 0U, 0U };
    for (size_t i = 0U; i < cand_count; ++i) {
        gather_white(cands[i], &whites);
    }
    free(cands);

    if (whites.count == 0U) {
        free(whites.buf);
        return;
    }
    clear_visited_on_whites(&whites);

    /* Fast path: no user finalizers in the white set → no resurrection
     * window → free the whole component as a unit. */
    if (!whites_contain_finalizer(&whites)) {
        free_whole_white_set(&whites);
        free(whites.buf);
        return;
    }

    /* Slow path (§13.2 cycle-with-finalizer two-phase collection):
     *   Phase 0   : restore intra-white refcount inflation so Phase 1.5's
     *               "rc > intra-white in-degree" comparison is meaningful.
     *   Phase 1   : invoke each white's user finalizer exactly once.
     *   Phase 1.5 : recompute intra-white in-degrees (finalizers may have
     *               mutated field slots), seed survivor BFS from whites
     *               with rc > intra-in-degree, propagate along intra-white
     *               out-edges, sanitise survivor↔free edges.
     *   Phase 2   : free the unsurvived (free_set) members. */

    phase0_restore_intra_white_rc(&whites);
    phase1_run_finalizers(&whites);

    WhiteAux *aux = (WhiteAux *)calloc(whites.count, sizeof(*aux));
    if (aux == NULL) {
        feng_panic("feng_cycle: out of memory for %zu WhiteAux entries",
                   whites.count);
    }
    phase15_compute_intra_in(&whites, aux);

    /* BFS queue is bounded by white count. */
    FengManagedHeader **queue =
        (FengManagedHeader **)calloc(whites.count, sizeof(*queue));
    if (queue == NULL) {
        feng_panic("feng_cycle: out of memory for %zu BFS queue entries",
                   whites.count);
    }
    size_t survivor_count = 0U;
    phase15_mark_survivors_bfs(&whites, aux, queue, &survivor_count);
    free(queue);
    free(aux);

    phase15_sanitise(&whites);
    phase15_restore_survivor_state(&whites);
    phase2_free_unsurvived(&whites);

    free(whites.buf);
}

void feng_cycle_runtime_shutdown(void) {
    feng_cycle_lock();
    if (g_candidate_count > 0U) {
        feng_cycle_collect_locked();
    }
    free(g_candidates);
    g_candidates = NULL;
    g_candidate_count = 0U;
    g_candidate_capacity = 0U;
    feng_cycle_unlock();
}
