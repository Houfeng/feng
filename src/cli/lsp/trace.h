#ifndef FENG_CLI_LSP_TRACE_H
#define FENG_CLI_LSP_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* Low-overhead trace configuration for protocol performance events. */
typedef struct FengLspTrace {
    bool enabled;
} FengLspTrace;

/* Start timestamp retained only when tracing is explicitly enabled. */
typedef struct FengLspTraceEvent {
    struct timespec started_at;
    bool active;
} FengLspTraceEvent;

/* Initializes tracing from the explicit FENG_LSP_TRACE environment switch. */
void feng_lsp_trace_init(FengLspTrace *trace);

/* Begins timing one request without work when production tracing is disabled. */
FengLspTraceEvent feng_lsp_trace_begin(const FengLspTrace *trace);

/* Writes one aggregation-friendly event to stderr or the configured error log. */
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
                        bool cancelled);

#endif /* FENG_CLI_LSP_TRACE_H */
