#ifndef FENG_CLI_PROJECT_COMMON_H
#define FENG_CLI_PROJECT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cli/deps/manager.h"
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

bool feng_cli_project_find_manifest_in_ancestors(const char *path_arg,
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

bool feng_cli_project_prepare_build(const char *program,
                                   const char *path_arg,
                                   bool release,
                                   FengCliProjectContext *out_context,
                                   FengCliDepsResolved *out_resolved,
                                   FengCliProjectError *out_error);

int feng_cli_project_compile_prepared(const char *program,
                                      const FengCliProjectContext *context,
                                      const FengCliDepsResolved *resolved,
                                      bool release);

#endif /* FENG_CLI_PROJECT_COMMON_H */
