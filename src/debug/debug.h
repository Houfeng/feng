#ifndef FENG_DEBUG_DEBUG_H
#define FENG_DEBUG_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codegen/mapping.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Package-to-local-root bindings loaded from a .fd artifact. */
typedef struct FengDebugArtifactPackage {
    char *package_name;
    char *local_root_path;
} FengDebugArtifactPackage;

/* Fully loaded .fd artifact contents. */
typedef struct FengDebugArtifact {
    char *binary_path;
    uint64_t binary_fingerprint;
    FengDebugArtifactPackage *packages;
    size_t package_count;
    FengDebugInfo info;
} FengDebugArtifact;

/* Computes the stable FNV-1a fingerprint for a built binary on disk. */
uint64_t feng_debug_fnv1a64_file(const char *path, char **out_error_message);

/* Writes a `.fd` sidecar for one built binary. */
bool feng_debug_write_fd(const char *fd_path,
                         const char *binary_path,
                         const FengDebugSourceMapping *sources,
                         size_t source_count,
                         const FengDebugInfo *info,
                         char **out_error_message);

/* Loads a `.fd` sidecar from disk. */
bool feng_debug_read_fd(const char *fd_path,
                        FengDebugArtifact *out_artifact,
                        char **out_error_message);

/* Releases heap-owned fields inside a loaded debug artifact. */
void feng_debug_artifact_dispose(FengDebugArtifact *artifact);

#ifdef __cplusplus
}
#endif

#endif /* FENG_DEBUG_DEBUG_H */
