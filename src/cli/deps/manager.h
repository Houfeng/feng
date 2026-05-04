#ifndef FENG_CLI_DEPS_MANAGER_H
#define FENG_CLI_DEPS_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "cli/project/manifest.h"

typedef struct FengCliDepsResolved {
    char **package_paths;
    size_t package_count;
} FengCliDepsResolved;

bool feng_cli_deps_normalize_direct_dependencies(const char *manifest_path,
                                                 const FengCliProjectManifest *manifest,
                                                 FengCliProjectManifestDependency **out_dependencies,
                                                 size_t *out_dependency_count,
                                                 FengCliProjectError *out_error);

void feng_cli_deps_manifest_dependency_list_dispose(
    FengCliProjectManifestDependency *dependencies,
    size_t dependency_count);

bool feng_cli_deps_install_for_manifest(const char *program,
                                        const char *manifest_path,
                                        bool force_remote,
                                        FengCliProjectError *out_error);

bool feng_cli_deps_validate_local_dependency(const char *owner_manifest_path,
                                             const char *dependency_name,
                                             const char *dependency_value,
                                             FengCliProjectError *out_error);

bool feng_cli_deps_resolve_for_manifest(const char *program,
                                        const char *manifest_path,
                                        bool force_remote,
                                        bool release,
                                        FengCliDepsResolved *out_resolved,
                                        FengCliProjectError *out_error);

void feng_cli_deps_resolved_dispose(FengCliDepsResolved *resolved);

#endif /* FENG_CLI_DEPS_MANAGER_H */
