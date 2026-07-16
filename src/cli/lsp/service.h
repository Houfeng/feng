#ifndef FENG_CLI_LSP_SERVICE_H
#define FENG_CLI_LSP_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct FengLspService FengLspService;

/* Creates an LSP service that owns protocol and workspace state. */
FengLspService *feng_lsp_service_create(void);

/* Releases an LSP service and all workspace state owned by it. */
void feng_lsp_service_free(FengLspService *service);

/* Dispatches one complete JSON-RPC payload. */
bool feng_lsp_service_handle_payload(FengLspService *service,
                                     FILE *output,
                                     const char *payload,
                                     size_t payload_length,
                                     FILE *errors);

/* Returns whether the service has received the LSP exit notification. */
bool feng_lsp_service_should_exit(const FengLspService *service);

/* Returns the process exit code selected by the LSP shutdown sequence. */
int feng_lsp_service_exit_code(const FengLspService *service);

#endif /* FENG_CLI_LSP_SERVICE_H */
