/* Exercise lock ownership through the existing runtime entry points only. */
#include "runtime/feng_runtime_internal.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); \
} } while (0)

/* The start gate brings fresh threads to their first collector lock together.
 * visits is protected solely by the collector lock, not the start gate. */
typedef struct LockTestState {
    pthread_mutex_t gate;
    pthread_cond_t ready;
    unsigned arrived;
    bool start;
    _Atomic unsigned active;
    unsigned visits;
} LockTestState;

/* Each worker varies its nesting depth independently of its peers. */
typedef struct LockWorker {
    LockTestState *state;
    unsigned index;
} LockWorker;

/* A partial exit must not let another thread overlap the outer critical region. */
static void *lock_worker(void *argument) {
    LockWorker *worker = argument;
    LockTestState *state = worker->state;
    CHECK(pthread_mutex_lock(&state->gate) == 0);
    ++state->arrived;
    CHECK(pthread_cond_broadcast(&state->ready) == 0);
    while (!state->start) {
        CHECK(pthread_cond_wait(&state->ready, &state->gate) == 0);
    }
    CHECK(pthread_mutex_unlock(&state->gate) == 0);

    for (unsigned iteration = 0; iteration < 500U; ++iteration) {
        unsigned depth = 1U + (iteration + worker->index) % 4U;
        for (unsigned level = 0; level < depth; ++level) {
            feng_cycle_lock();
        }
        CHECK(atomic_fetch_add(&state->active, 1U) == 0U);
        ++state->visits;
        for (unsigned level = depth; level > 1U; --level) {
            feng_cycle_unlock();
            (void)sched_yield();
            CHECK(atomic_load(&state->active) == 1U);
        }
        CHECK(atomic_fetch_sub(&state->active, 1U) == 1U);
        feng_cycle_unlock();
    }
    return NULL;
}

/* Run once before any runtime lock use, and again with fresh threads after
 * shutdown, so both global initialization and per-thread publication are used. */
static void test_lock_concurrent_entry(void) {
    enum { WORKERS = 8 };
    LockTestState state = {
        .gate = PTHREAD_MUTEX_INITIALIZER,
        .ready = PTHREAD_COND_INITIALIZER,
    };
    pthread_t threads[WORKERS];
    LockWorker workers[WORKERS];
    atomic_init(&state.active, 0U);
    for (unsigned i = 0; i < WORKERS; ++i) {
        workers[i] = (LockWorker){&state, i};
        CHECK(pthread_create(&threads[i], NULL, lock_worker, &workers[i]) == 0);
    }
    CHECK(pthread_mutex_lock(&state.gate) == 0);
    while (state.arrived != WORKERS) {
        CHECK(pthread_cond_wait(&state.ready, &state.gate) == 0);
    }
    state.start = true;
    CHECK(pthread_cond_broadcast(&state.ready) == 0);
    CHECK(pthread_mutex_unlock(&state.gate) == 0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    CHECK(state.visits == WORKERS * 500U);
    CHECK(atomic_load(&state.active) == 0U);
    CHECK(pthread_cond_destroy(&state.ready) == 0);
    CHECK(pthread_mutex_destroy(&state.gate) == 0);
}

/* A potentially cyclic type is also valid when its instances form a chain. */
typedef struct LockNode {
    FengManagedHeader header;
    struct LockNode *child;
    unsigned id;
} LockNode;

static unsigned lock_next_finalizer;

/* Reentrant lock use and a temporary self owner must preserve parent-first
 * finalization and leave the outer destruction scope locked. */
static void lock_node_finalize(void *value) {
    LockNode *node = value;
    feng_cycle_lock();
    CHECK(lock_next_finalizer == node->id + 1U);
    --lock_next_finalizer;
    feng_release(feng_retain(node));
    feng_cycle_unlock();
}

/* Recursively release the same owning edge shape emitted for ordinary objects. */
static void lock_node_release_children(void *value) {
    LockNode *node = value;
    LockNode *child = node->child;
    node->child = NULL;
    feng_release(child);
}

static const FengManagedFieldDescriptor lock_node_fields[] = {
    {offsetof(LockNode, child), NULL, NULL},
};
static const FengTypeDescriptor lock_node_descriptor = {
    .name = "test.LockNode",
    .size = sizeof(LockNode),
    .finalizer = lock_node_finalize,
    .release_children = lock_node_release_children,
    .is_potentially_cyclic = true,
    .managed_field_count = 1U,
    .managed_fields = lock_node_fields,
};

/* Destruction completes synchronously even while a caller holds nested locks. */
static void test_lock_recursive_destruction(void) {
    LockNode *root = NULL;
    lock_next_finalizer = 32U;
    for (unsigned id = 0; id < 32U; ++id) {
        LockNode *node = feng_object_new(&lock_node_descriptor);
        node->child = root;
        node->id = id;
        root = node;
    }
    feng_cycle_lock();
    feng_cycle_lock();
    feng_release(root);
    CHECK(lock_next_finalizer == 0U);
    feng_cycle_runtime_shutdown();
    feng_cycle_unlock();
    feng_cycle_unlock();
}

/* Keep this registration first: the first workers must race the original once
 * initialization, without resetting production-private state for the test. */
void test_cycle_lock(void) {
    test_lock_concurrent_entry();
    test_lock_recursive_destruction();
    test_lock_recursive_destruction();
    test_lock_concurrent_entry();
    feng_cycle_runtime_shutdown();
    puts("cycle lock: concurrent first entry, nesting, destruction and shutdown reentry passed");
}
