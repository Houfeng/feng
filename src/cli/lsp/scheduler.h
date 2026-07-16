#ifndef FENG_CLI_LSP_SCHEDULER_H
#define FENG_CLI_LSP_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* Relative priority used only between requests separated by no notification. */
typedef enum FengLspRequestPriority {
    FENG_LSP_PRIORITY_LOW = 0,
    FENG_LSP_PRIORITY_MEDIUM,
    FENG_LSP_PRIORITY_HIGH,
    FENG_LSP_PRIORITY_HIGHEST
} FengLspRequestPriority;

/* One owned JSON-RPC payload waiting for the interaction worker. */
typedef struct FengLspScheduledRequest {
    char *payload;
    size_t payload_length;
    char *method;
    char *request_id;
    FengLspRequestPriority priority;
    bool has_id;
} FengLspScheduledRequest;

/* Thread-safe request queue preserving notification visibility boundaries. */
typedef struct FengLspRequestScheduler {
    FengLspScheduledRequest *items;
    size_t count;
    size_t capacity;
    char *active_request_id;
    bool active_cancelled;
    bool stop_requested;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} FengLspRequestScheduler;

/* Initializes an empty request scheduler. */
bool feng_lsp_scheduler_init(FengLspRequestScheduler *scheduler);

/* Stops waiters and releases all queued request data. */
void feng_lsp_scheduler_dispose(FengLspRequestScheduler *scheduler);

/* Transfers one owned request into the scheduler. */
bool feng_lsp_scheduler_submit(FengLspRequestScheduler *scheduler,
                               FengLspScheduledRequest *request);

/* Marks a request cancelled and removes it when it has not started. */
bool feng_lsp_scheduler_cancel(FengLspRequestScheduler *scheduler,
                               const char *request_id,
                               bool *out_was_queued);

/* Waits for and transfers the next request selected by priority and barriers. */
bool feng_lsp_scheduler_take(FengLspRequestScheduler *scheduler,
                             FengLspScheduledRequest *out_request);

/* Reports whether the active request has been cancelled. */
bool feng_lsp_scheduler_active_cancelled(FengLspRequestScheduler *scheduler,
                                         const char *request_id);

/* Marks the active request complete so its id is no longer cancellable. */
void feng_lsp_scheduler_finish_active(FengLspRequestScheduler *scheduler);

/* Requests worker shutdown without discarding an already active request. */
void feng_lsp_scheduler_stop(FengLspRequestScheduler *scheduler);

/* Releases all memory owned by a scheduled request. */
void feng_lsp_scheduled_request_dispose(FengLspScheduledRequest *request);

#endif /* FENG_CLI_LSP_SCHEDULER_H */
