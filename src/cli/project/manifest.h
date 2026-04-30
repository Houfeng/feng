#ifndef FENG_CLI_PROJECT_MANIFEST_H
#define FENG_CLI_PROJECT_MANIFEST_H

#include <stdbool.h>

#include "semantic/semantic.h"

typedef struct FengCliProjectError {
    char *path;
    char *message;
    unsigned int line;
} FengCliProjectError;

typedef struct FengCliProjectManifest {
    char *name;
    char *version;
    FengCompileTarget target;
    char *src_path;
    char *out_path;
} FengCliProjectManifest;

bool feng_cli_project_manifest_parse(const char *manifest_path,
                                    const char *source,
                                    FengCliProjectManifest *out_manifest,
                                    FengCliProjectError *out_error);

void feng_cli_project_manifest_dispose(FengCliProjectManifest *manifest);
void feng_cli_project_error_dispose(FengCliProjectError *error);

#endif /* FENG_CLI_PROJECT_MANIFEST_H */
