#include "cli/lsp/scheduler.h"

#include <stdlib.h>
#include <string.h>

/* Duplicates one NUL-terminated scheduler string. */
static char *scheduler_dup_cstr(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

/* Ensures the queue can hold one more request. */
static bool scheduler_reserve_one(FengLspRequestScheduler *scheduler) {
    FengLspScheduledRequest *grown;
    size_t capacity;

    if (scheduler->count < scheduler->capacity) {
        return true;
    }
    capacity = scheduler->capacity == 0U ? 16U : scheduler->capacity * 2U;
    grown = (FengLspScheduledRequest *)realloc(scheduler->items,
                                               capacity * sizeof(scheduler->items[0]));
    if (grown == NULL) {
        return false;
    }
    scheduler->items = grown;
    scheduler->capacity = capacity;
    return true;
}

/* Removes and transfers the queue entry at index. */
static FengLspScheduledRequest scheduler_remove_at(FengLspRequestScheduler *scheduler,
                                                    size_t index) {
    FengLspScheduledRequest request = scheduler->items[index];

    if (index + 1U < scheduler->count) {
        memmove(&scheduler->items[index],
                &scheduler->items[index + 1U],
                (scheduler->count - index - 1U) * sizeof(scheduler->items[0]));
    }
    --scheduler->count;
    return request;
}

/* Selects the highest-priority request before the next notification barrier. */
static size_t scheduler_next_index(const FengLspRequestScheduler *scheduler) {
    size_t best = 0U;
    size_t index;

    if (scheduler->count == 0U || !scheduler->items[0].has_id) {
        return 0U;
    }
    for (index = 1U; index < scheduler->count; ++index) {
        if (!scheduler->items[index].has_id) {
            break;
        }
        if (scheduler->items[index].priority > scheduler->items[best].priority) {
            best = index;
        }
    }
    return best;
}

bool feng_lsp_scheduler_init(FengLspRequestScheduler *scheduler) {
    if (scheduler == NULL) {
        return false;
    }
    memset(scheduler, 0, sizeof(*scheduler));
    if (pthread_mutex_init(&scheduler->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&scheduler->condition, NULL) != 0) {
        pthread_mutex_destroy(&scheduler->mutex);
        memset(scheduler, 0, sizeof(*scheduler));
        return false;
    }
    return true;
}

void feng_lsp_scheduler_dispose(FengLspRequestScheduler *scheduler) {
    size_t index;

    if (scheduler == NULL) {
        return;
    }
    feng_lsp_scheduler_stop(scheduler);
    for (index = 0U; index < scheduler->count; ++index) {
        feng_lsp_scheduled_request_dispose(&scheduler->items[index]);
    }
    free(scheduler->items);
    free(scheduler->active_request_id);
    pthread_cond_destroy(&scheduler->condition);
    pthread_mutex_destroy(&scheduler->mutex);
    memset(scheduler, 0, sizeof(*scheduler));
}

bool feng_lsp_scheduler_submit(FengLspRequestScheduler *scheduler,
                               FengLspScheduledRequest *request) {
    bool ok;

    if (scheduler == NULL || request == NULL || request->payload == NULL) {
        return false;
    }
    pthread_mutex_lock(&scheduler->mutex);
    ok = !scheduler->stop_requested && scheduler_reserve_one(scheduler);
    if (ok) {
        scheduler->items[scheduler->count++] = *request;
        memset(request, 0, sizeof(*request));
        pthread_cond_signal(&scheduler->condition);
    }
    pthread_mutex_unlock(&scheduler->mutex);
    return ok;
}

bool feng_lsp_scheduler_cancel(FengLspRequestScheduler *scheduler,
                               const char *request_id,
                               bool *out_was_queued) {
    size_t index;
    bool found = false;
    bool queued = false;

    if (out_was_queued != NULL) {
        *out_was_queued = false;
    }
    if (scheduler == NULL || request_id == NULL) {
        return false;
    }
    pthread_mutex_lock(&scheduler->mutex);
    for (index = 0U; index < scheduler->count; ++index) {
        if (scheduler->items[index].request_id != NULL &&
            strcmp(scheduler->items[index].request_id, request_id) == 0) {
            FengLspScheduledRequest removed = scheduler_remove_at(scheduler, index);

            feng_lsp_scheduled_request_dispose(&removed);
            found = true;
            queued = true;
            break;
        }
    }
    if (!found && scheduler->active_request_id != NULL &&
        strcmp(scheduler->active_request_id, request_id) == 0) {
        scheduler->active_cancelled = true;
        found = true;
    }
    pthread_mutex_unlock(&scheduler->mutex);
    if (out_was_queued != NULL) {
        *out_was_queued = queued;
    }
    return found;
}

bool feng_lsp_scheduler_take(FengLspRequestScheduler *scheduler,
                             FengLspScheduledRequest *out_request) {
    size_t index;

    if (scheduler == NULL || out_request == NULL) {
        return false;
    }
    memset(out_request, 0, sizeof(*out_request));
    pthread_mutex_lock(&scheduler->mutex);
    while (!scheduler->stop_requested && scheduler->count == 0U) {
        pthread_cond_wait(&scheduler->condition, &scheduler->mutex);
    }
    if (scheduler->count == 0U) {
        pthread_mutex_unlock(&scheduler->mutex);
        return false;
    }
    index = scheduler_next_index(scheduler);
    *out_request = scheduler_remove_at(scheduler, index);
    free(scheduler->active_request_id);
    scheduler->active_request_id = out_request->has_id
        ? scheduler_dup_cstr(out_request->request_id)
        : NULL;
    scheduler->active_cancelled = false;
    pthread_mutex_unlock(&scheduler->mutex);
    return true;
}

bool feng_lsp_scheduler_active_cancelled(FengLspRequestScheduler *scheduler,
                                         const char *request_id) {
    bool cancelled;

    if (scheduler == NULL || request_id == NULL) {
        return false;
    }
    pthread_mutex_lock(&scheduler->mutex);
    cancelled = scheduler->active_cancelled && scheduler->active_request_id != NULL &&
                strcmp(scheduler->active_request_id, request_id) == 0;
    pthread_mutex_unlock(&scheduler->mutex);
    return cancelled;
}

void feng_lsp_scheduler_finish_active(FengLspRequestScheduler *scheduler) {
    if (scheduler == NULL) {
        return;
    }
    pthread_mutex_lock(&scheduler->mutex);
    free(scheduler->active_request_id);
    scheduler->active_request_id = NULL;
    scheduler->active_cancelled = false;
    pthread_mutex_unlock(&scheduler->mutex);
}

void feng_lsp_scheduler_stop(FengLspRequestScheduler *scheduler) {
    if (scheduler == NULL) {
        return;
    }
    pthread_mutex_lock(&scheduler->mutex);
    scheduler->stop_requested = true;
    pthread_cond_broadcast(&scheduler->condition);
    pthread_mutex_unlock(&scheduler->mutex);
}

void feng_lsp_scheduled_request_dispose(FengLspScheduledRequest *request) {
    if (request == NULL) {
        return;
    }
    free(request->payload);
    free(request->method);
    free(request->request_id);
    memset(request, 0, sizeof(*request));
}
