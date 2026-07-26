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
    char *package_path;
    char **source_paths;
    size_t source_count;
    FengCliProjectManifest manifest;
} FengCliProjectContext;

/* Owned, ordered target-platform set selected for one project command. */
typedef struct FengCliProjectPlatformSelection {
    char **platforms;
    size_t platform_count;
} FengCliProjectPlatformSelection;

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
                                                         const char *platform,
                                                         const char *sysroot,
                                                         bool release,
                                                         bool keep_intermediate,
                                                         size_t package_count,
                                                         const char *const *package_paths,
                                                         size_t dependency_fd_count,
                                                         const char *const *dependency_fd_paths);

int feng_cli_project_invoke_direct_compile(const char *program,
                                           const FengCliProjectContext *context,
                                           const char *platform,
                                           const char *sysroot,
                                           bool release,
                                           bool keep_intermediate);

bool feng_cli_project_resolve_build_dependencies(const char *program,
                                                 const FengCliProjectContext *context,
                                                 const char *platform,
                                                 const char *sysroot,
                                                 bool release,
                                                 FengCliDepsResolved *out_resolved,
                                                 FengCliProjectError *out_error);

bool feng_cli_project_asset_targets_extlib(const FengCliProjectManifestAsset *asset);

/* Select and validate project target platforms using the CLI's unified rules. */
bool feng_cli_project_select_platforms(
    const FengCliProjectContext *context,
    const char *const *requested_platforms,
    size_t requested_platform_count,
    const char *sysroot,
    bool host_only,
    FengCliProjectPlatformSelection *out_selection,
    FengCliProjectError *out_error);

/* Release an owned project platform selection. */
void feng_cli_project_platform_selection_dispose(
    FengCliProjectPlatformSelection *selection);

/* Compose the exact direct-compile output root for one target platform. */
char *feng_cli_project_platform_out_root(const FengCliProjectContext *context,
                                         const char *platform);

/* Compose the executable path for one project target platform. */
char *feng_cli_project_platform_binary_path(const FengCliProjectContext *context,
                                            const char *platform);

bool feng_cli_project_stage_assets(const FengCliProjectContext *context,
                                   const char *platform,
                                   FengCliProjectError *out_error);

int feng_cli_project_compile_prepared(const char *program,
                                      const FengCliProjectContext *context,
                                      const FengCliDepsResolved *resolved,
                                      const char *platform,
                                      const char *sysroot,
                                      bool release,
                                      bool keep_intermediate);

/* Resolve dependencies, compile, and stage assets for one selected platform. */
int feng_cli_project_build_platform(const char *program,
                                    const FengCliProjectContext *context,
                                    const char *platform,
                                    const char *sysroot,
                                    bool release,
                                    bool keep_intermediate,
                                    FengCliProjectError *out_error);

#endif /* FENG_CLI_PROJECT_COMMON_H */
