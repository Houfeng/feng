#include "cli/lsp/trace.h"

#include <stdlib.h>
#include <string.h>

void feng_lsp_trace_init(FengLspTrace *trace) {
    const char *value;

    if (trace == NULL) {
        return;
    }
    value = getenv("FENG_LSP_TRACE");
    trace->enabled = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

FengLspTraceEvent feng_lsp_trace_begin(const FengLspTrace *trace) {
    FengLspTraceEvent event = {0};

    if (trace != NULL && trace->enabled &&
        clock_gettime(CLOCK_MONOTONIC, &event.started_at) == 0) {
        event.active = true;
    }
    return event;
}

void feng_lsp_trace_end(const FengLspTrace *trace,
                        FengLspTraceEvent event,
                        FILE *errors,
                        const char *method,
                        const char *request_id,
                        unsigned int document_version,
                        size_t workspace_generation,
                        const char *query_path,
                        bool cache_hit,
                        size_t item_count,
                        bool cancelled) {
    struct timespec ended_at;
    double elapsed_ms;

    if (trace == NULL || !trace->enabled || !event.active || errors == NULL ||
        clock_gettime(CLOCK_MONOTONIC, &ended_at) != 0) {
        return;
    }
    elapsed_ms = (double)(ended_at.tv_sec - event.started_at.tv_sec) * 1000.0 +
                 (double)(ended_at.tv_nsec - event.started_at.tv_nsec) / 1000000.0;
    (void)fprintf(errors,
                  "lsp-trace method=%s id=%s version=%u generation=%zu "
                  "duration_ms=%.3f path=%s cache_hit=%s items=%zu cancelled=%s "
                  "sync_io=0 full_analysis=0\n",
                  method != NULL ? method : "",
                  request_id != NULL ? request_id : "",
                  document_version,
                  workspace_generation,
                  elapsed_ms,
                  query_path != NULL ? query_path : "memory",
                  cache_hit ? "true" : "false",
                  item_count,
                  cancelled ? "true" : "false");
}
