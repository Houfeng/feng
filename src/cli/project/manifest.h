#ifndef FENG_CLI_PROJECT_MANIFEST_H
#define FENG_CLI_PROJECT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#include "semantic/semantic.h"

typedef struct FengCliProjectError {
    char *path;
    char *message;
    unsigned int line;
} FengCliProjectError;

typedef struct FengCliProjectManifestDependency {
    char *name;
    char *value;
    unsigned int line;
    bool is_local_path;
} FengCliProjectManifestDependency;

typedef struct FengCliProjectManifest {
    char *name;
    char *version;
    bool has_target;
    FengCompileTarget target;
    char *src_path;
    char *out_path;
    char *arch;
    char *abi;
    char *registry_url;
    FengCliProjectManifestDependency *dependencies;
    size_t dependency_count;
} FengCliProjectManifest;

bool feng_cli_project_manifest_parse(const char *manifest_path,
                                     const char *source,
                                     FengCliProjectManifest *out_manifest,
                                     FengCliProjectError *out_error);

bool feng_cli_project_bundle_manifest_parse(const char *manifest_path,
                                            const char *source,
                                            FengCliProjectManifest *out_manifest,
                                            FengCliProjectError *out_error);

bool feng_cli_project_manifest_write(const char *manifest_path,
                                     const FengCliProjectManifest *manifest,
                                     char **out_error_message);

void feng_cli_project_manifest_dispose(FengCliProjectManifest *manifest);
void feng_cli_project_error_dispose(FengCliProjectError *error);

#endif /* FENG_CLI_PROJECT_MANIFEST_H */
