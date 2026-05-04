#ifndef FENG_CLI_LSP_RUNTIME_H
#define FENG_CLI_LSP_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct FengLspRuntime FengLspRuntime;

FengLspRuntime *feng_lsp_runtime_create(void);
void feng_lsp_runtime_free(FengLspRuntime *runtime);

bool feng_lsp_runtime_handle_payload(FengLspRuntime *runtime,
                                     FILE *output,
                                     const char *payload,
                                     size_t payload_length,
                                     FILE *errors);

bool feng_lsp_runtime_should_exit(const FengLspRuntime *runtime);
int feng_lsp_runtime_exit_code(const FengLspRuntime *runtime);

#endif /* FENG_CLI_LSP_RUNTIME_H */
