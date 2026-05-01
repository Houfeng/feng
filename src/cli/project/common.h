#ifndef FENG_CLI_PROJECT_COMMON_H
#define FENG_CLI_PROJECT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cli/project/manifest.h"

typedef struct FengCliProjectContext {
    char *manifest_path;
    char *project_root;
    char *source_root;
    char *out_root;
    char *binary_path;
    char *package_path;
    char **source_paths;
    size_t source_count;
    FengCliProjectManifest manifest;
} FengCliProjectContext;

bool feng_cli_project_open(const char *path_arg,
                           FengCliProjectContext *out_context,
                           FengCliProjectError *out_error);

bool feng_cli_project_resolve_manifest_path(const char *path_arg,
                                            char **out_manifest_path,
                                            FengCliProjectError *out_error);

void feng_cli_project_context_dispose(FengCliProjectContext *context);

void feng_cli_project_print_error(FILE *stream, const FengCliProjectError *error);

bool feng_cli_project_remove_tree(const char *path, char **out_error_message);

int feng_cli_project_invoke_direct_compile_with_packages(const char *program,
                                                         const FengCliProjectContext *context,
                                                         bool release,
                                                         size_t package_count,
                                                         const char *const *package_paths);

int feng_cli_project_invoke_direct_compile(const char *program,
                                           const FengCliProjectContext *context,
                                           bool release);

#endif /* FENG_CLI_PROJECT_COMMON_H */
