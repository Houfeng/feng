#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Instrument only this test's service translation unit, leaving production
 * code and its public API unchanged. */
static int lifetime_mutex_unlock(pthread_mutex_t *mutex);
static int lifetime_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
static int lifetime_cond_timedwait(pthread_cond_t *condition,
                                 pthread_mutex_t *mutex,
                                 const struct timespec *deadline);
static void *lifetime_malloc(size_t size);
static void *lifetime_calloc(size_t count, size_t size);

#define pthread_mutex_unlock lifetime_mutex_unlock
#define pthread_cond_wait lifetime_cond_wait
#define pthread_cond_timedwait lifetime_cond_timedwait
#define malloc lifetime_malloc
#define calloc lifetime_calloc
#include "cli/lsp/service.c"
#undef calloc
#undef malloc
#undef pthread_cond_timedwait
#undef pthread_cond_wait
#undef pthread_mutex_unlock

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Exact worker boundaries at which the controlling thread requests shutdown. */
typedef enum LifetimeBoundary {
    LIFETIME_BEFORE_CLAIM,
    LIFETIME_AFTER_CLAIM,
    LIFETIME_AFTER_COMPLETION,
    LIFETIME_WAITING
} LifetimeBoundary;

/* A separate condition variable coordinates the test without sleeps or
 * production hooks. The service pointer remains valid until worker join. */
typedef struct LifetimeGate {
    FengLspService *service;
    LifetimeBoundary boundary;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool reached;
    bool resume;
} LifetimeGate;

static LifetimeGate lifetime_gate;
static _Thread_local size_t allocation_failure;
static _Thread_local size_t allocation_count;

/* CLI support objects reference the command-line usage entry point. Analysis
 * must never dispatch to it; keep the same test-owned boundary as test_cli. */
void feng_cli_print_usage(const char *program, FILE *stream) {
    (void)program;
    (void)stream;
    CHECK(false);
}

/* Signals arrival and, when no service lock is held, pauses for shutdown. */
static void lifetime_reach_boundary(bool pause) {
    CHECK(pthread_mutex_lock(&lifetime_gate.mutex) == 0);
    if (!lifetime_gate.reached) {
        lifetime_gate.reached = true;
        CHECK(pthread_cond_broadcast(&lifetime_gate.condition) == 0);
        while (pause && !lifetime_gate.resume) {
            CHECK(pthread_cond_wait(&lifetime_gate.condition,
                                    &lifetime_gate.mutex) == 0);
        }
    }
    CHECK(pthread_mutex_unlock(&lifetime_gate.mutex) == 0);
}

/* Pauses after releasing a lock at the requested ownership boundary. */
static int lifetime_mutex_unlock(pthread_mutex_t *mutex) {
    FengLspService *service = lifetime_gate.service;
    bool reached = false;

    if (service != NULL) {
        if (lifetime_gate.boundary == LIFETIME_BEFORE_CLAIM &&
            mutex == &service->analysis_mutex) {
            reached = service->running_analysis_identity == NULL &&
                      service->pending_analysis_count != 0U;
        } else if (lifetime_gate.boundary == LIFETIME_AFTER_CLAIM &&
                   mutex == &service->documents_mutex) {
            reached = service->running_analysis_identity != NULL;
        } else if (lifetime_gate.boundary == LIFETIME_AFTER_COMPLETION &&
                   mutex == &service->analysis_mutex) {
            reached = service->running_analysis_identity == NULL &&
                      service->last_successful_analysis_count != 0U;
        }
    }
    int result = pthread_mutex_unlock(mutex);
    if (reached) {
        lifetime_reach_boundary(true);
    }
    return result;
}

/* The real condition wait releases analysis_mutex atomically, so shutdown
 * cannot lose its wakeup even if the controller arrives immediately. */
static int lifetime_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    if (lifetime_gate.service != NULL &&
        lifetime_gate.boundary == LIFETIME_WAITING &&
        condition == &lifetime_gate.service->analysis_condition) {
        lifetime_reach_boundary(false);
    }
    return pthread_cond_wait(condition, mutex);
}

/* Exercises stopping during the queue's existing debounce wait. */
static int lifetime_cond_timedwait(pthread_cond_t *condition,
                                 pthread_mutex_t *mutex,
                                 const struct timespec *deadline) {
    if (lifetime_gate.service != NULL &&
        lifetime_gate.boundary == LIFETIME_WAITING &&
        condition == &lifetime_gate.service->analysis_condition) {
        lifetime_reach_boundary(false);
    }
    return pthread_cond_timedwait(condition, mutex, deadline);
}

/* Fails one allocation in a synchronous snapshot-construction test. */
static void *lifetime_malloc(size_t size) {
    if (allocation_failure != 0U && ++allocation_count == allocation_failure) {
        return NULL;
    }
    return malloc(size);
}

/* Includes the document-array allocation in the same failure sequence. */
static void *lifetime_calloc(size_t count, size_t size) {
    if (allocation_failure != 0U && ++allocation_count == allocation_failure) {
        return NULL;
    }
    return calloc(count, size);
}

/* Allocates a padded string; the first document reproduces the CI report's
 * exact 82-byte URI, 75-byte path and 115-byte source allocations. */
static char *lifetime_text(const char *prefix, size_t size) {
    size_t length = strlen(prefix);
    char *text = malloc(size);

    CHECK(text != NULL && length < size);
    memset(text, ' ', size - 1U);
    memcpy(text, prefix, length);
    text[size - 1U] = '\0';
    return text;
}

/* Creates only the background-analysis owner; request-thread behavior remains
 * covered by the existing protocol tests. Destruction uses the real service. */
static FengLspService *lifetime_service(size_t document_count) {
    FengLspService *service = calloc(1U, sizeof(*service));

    CHECK(service != NULL);
    CHECK(pthread_mutex_init(&service->documents_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&service->analysis_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&service->protocol_output_mutex, NULL) == 0);
    CHECK(pthread_cond_init(&service->analysis_condition, NULL) == 0);
    CHECK(feng_lsp_scheduler_init(&service->request_scheduler));
    if (document_count != 0U) {
        service->documents = calloc(document_count, sizeof(service->documents[0]));
        CHECK(service->documents != NULL);
    }
    service->document_count = document_count;
    for (size_t index = 0U; index < document_count; ++index) {
        FengLspDocument *document = &service->documents[index];
        document->uri = lifetime_text("untitled:lifetime", 82U + index);
        document->path = lifetime_text("lifetime.ff", 75U + index);
        document->text = lifetime_text(
            "module lifetime;\nfunc value(): int { return 1; }\n", 115U + index);
        document->version = 1U;
    }
    return service;
}

/* Runs a real worker with a deterministic shutdown interleaving and joins it
 * before inspecting state or invoking the production service destructor. */
static void test_shutdown_at_boundary(LifetimeBoundary boundary,
                                     size_t document_count,
                                     bool project,
                                     bool pending) {
    FengLspService *service = lifetime_service(document_count);
    struct timespec deadline;
    bool cleared;
    size_t remaining;

    memset(&lifetime_gate, 0, sizeof(lifetime_gate));
    lifetime_gate.boundary = boundary;
    CHECK(pthread_mutex_init(&lifetime_gate.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&lifetime_gate.condition, NULL) == 0);
    if (pending) {
        /* Scheduling uses the ordinary queue and does not require the worker
         * to have started; all state is initialized before pthread_create. */
        service->analysis_thread_started = true;
        schedule_analysis_target(service,
            project ? FENG_LSP_PENDING_PROJECT : FENG_LSP_PENDING_DOCUMENT,
            project ? "temp/lsp-lifetime-missing/feng.fm" : service->documents[0].uri,
            1U, false, false, false);
        CHECK(service->pending_analysis_count == 1U);
        if (boundary == LIFETIME_WAITING) {
            CHECK(clock_gettime(CLOCK_REALTIME,
                                &service->pending_analyses[0].due_time) == 0);
            service->pending_analyses[0].due_time.tv_sec += 60;
        }
    }
    lifetime_gate.service = service;
    service->analysis_thread_started = true;
    CHECK(pthread_create(&service->analyzer_thread, NULL,
                         background_analyzer_main, service) == 0);

    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 30;
    CHECK(pthread_mutex_lock(&lifetime_gate.mutex) == 0);
    while (!lifetime_gate.reached) {
        CHECK(pthread_cond_timedwait(&lifetime_gate.condition,
                                     &lifetime_gate.mutex, &deadline) == 0);
    }
    CHECK(pthread_mutex_unlock(&lifetime_gate.mutex) == 0);

    CHECK(pthread_mutex_lock(&service->analysis_mutex) == 0);
    service->analysis_stop_requested = true;
    CHECK(pthread_cond_broadcast(&service->analysis_condition) == 0);
    CHECK(pthread_mutex_unlock(&service->analysis_mutex) == 0);
    CHECK(pthread_mutex_lock(&lifetime_gate.mutex) == 0);
    lifetime_gate.resume = true;
    CHECK(pthread_cond_broadcast(&lifetime_gate.condition) == 0);
    CHECK(pthread_mutex_unlock(&lifetime_gate.mutex) == 0);
    CHECK(pthread_join(service->analyzer_thread, NULL) == 0);
    service->analysis_thread_started = false;
    cleared = service->running_analysis_identity == NULL &&
              service->running_analysis_generation == 0U;
    remaining = service->pending_analysis_count;
    lifetime_gate.service = NULL;
    feng_lsp_service_free(service);
    CHECK(pthread_cond_destroy(&lifetime_gate.condition) == 0);
    CHECK(pthread_mutex_destroy(&lifetime_gate.mutex) == 0);
    CHECK(cleared);
    CHECK(remaining == (pending && (boundary == LIFETIME_BEFORE_CLAIM ||
                                   boundary == LIFETIME_WAITING) ? 1U : 0U));
}

/* Covers every document/manifest allocation failure and verifies that a failed
 * snapshot leaves no owned resources and does not prevent the next clone. */
static void test_snapshot_allocation_failures(size_t document_count, bool project) {
    FengLspService *service = lifetime_service(document_count);
    const FengLspDocument *primary = project ? NULL : &service->documents[0];
    const char *manifest = project ? "temp/lifetime/feng.fm" : NULL;
    size_t allocations = (project ? 1U : 0U) +
                         (document_count != 0U ? 1U + 3U * document_count : 0U);

    for (size_t failure = 1U; failure <= allocations; ++failure) {
        FengLspAnalysisTask task = {0};

        allocation_count = 0U;
        allocation_failure = failure;
        CHECK(!analysis_task_clone(service, primary, manifest, 7U, true, &task));
        allocation_failure = 0U;
        CHECK(task.documents == NULL && task.document_count == 0U);
        CHECK(task.manifest_path == NULL);
        analysis_task_dispose(&task);
        CHECK(analysis_task_clone(service, primary, manifest, 8U, false, &task));
        CHECK(task.document_count == document_count && task.generation == 8U);
        CHECK(task.primary_index == (project ? document_count : 0U));
        for (size_t index = 0U; index < document_count; ++index) {
            CHECK(task.documents[index].text != service->documents[index].text);
            CHECK(strcmp(task.documents[index].text, service->documents[index].text) == 0);
        }
        analysis_task_dispose(&task);
    }
    feng_lsp_service_free(service);
}

/* Both ordinary and sanitizer regressions execute the same deterministic cases. */
int main(void) {
    for (size_t iteration = 0U; iteration < 4U; ++iteration) {
        test_shutdown_at_boundary(LIFETIME_AFTER_CLAIM, 1U, false, true);
        test_shutdown_at_boundary(LIFETIME_AFTER_CLAIM, 3U, false, true);
        test_shutdown_at_boundary(LIFETIME_AFTER_CLAIM, 3U, true, true);
        test_shutdown_at_boundary(LIFETIME_AFTER_CLAIM, 0U, true, true);
        test_shutdown_at_boundary(LIFETIME_BEFORE_CLAIM, 1U, false, true);
        test_shutdown_at_boundary(LIFETIME_BEFORE_CLAIM, 3U, true, true);
        test_shutdown_at_boundary(LIFETIME_AFTER_COMPLETION, 1U, false, true);
        test_shutdown_at_boundary(LIFETIME_WAITING, 0U, false, false);
        test_shutdown_at_boundary(LIFETIME_WAITING, 3U, false, true);
        test_shutdown_at_boundary(LIFETIME_WAITING, 0U, true, true);
    }
    test_snapshot_allocation_failures(1U, false);
    test_snapshot_allocation_failures(3U, false);
    test_snapshot_allocation_failures(0U, true);
    test_snapshot_allocation_failures(1U, true);
    test_snapshot_allocation_failures(3U, true);
    for (size_t iteration = 0U; iteration < 16U; ++iteration) {
        FengLspService *service = feng_lsp_service_create();
        CHECK(service != NULL);
        feng_lsp_service_free(service);
    }
    puts("lsp analysis lifetime: 40 shutdown interleavings, 31 allocation failures, 16 service lifecycles passed");
    return 0;
}
