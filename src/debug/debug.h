#ifndef FENG_DEBUG_DEBUG_H
#define FENG_DEBUG_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maps one compiled source file to its owning package identity. */
typedef struct FengDebugSourceMapping {
    const char *source_path;
    const char *package_name;
    const char *package_root;
} FengDebugSourceMapping;

/* Resolved logical-source information derived from a source mapping entry. */
typedef struct FengDebugResolvedSource {
    char *package_name;
    char *package_root;
    char *relative_path;
    char *logical_uri;
} FengDebugResolvedSource;

/* Controls how a backend frame should be surfaced in the debugger. */
typedef enum FengDebugFramePolicy {
    FENG_DEBUG_FRAME_VISIBLE = 0,
    FENG_DEBUG_FRAME_HIDDEN = 1,
    FENG_DEBUG_FRAME_COLLAPSE = 2
} FengDebugFramePolicy;

/* Classifies a user-visible variable for scope rewriting. */
typedef enum FengDebugVariableKind {
    FENG_DEBUG_VARIABLE_PARAM = 0,
    FENG_DEBUG_VARIABLE_BINDING = 1,
    FENG_DEBUG_VARIABLE_CAPTURE = 2,
    FENG_DEBUG_VARIABLE_SELF = 3
} FengDebugVariableKind;

/* Maps one backend frame symbol to a user-facing frame name and policy. */
typedef struct FengDebugFrameRecord {
    char *backend_symbol;
    char *display_name;
    FengDebugFramePolicy policy;
} FengDebugFrameRecord;

/* Maps one backend variable slot or synthetic expression to a user variable. */
typedef struct FengDebugVariableRecord {
    char *frame_backend_symbol;
    char *backend_name;
    char *display_name;
    char *read_expr;
    FengDebugVariableKind kind;
} FengDebugVariableRecord;

/* Codegen-owned abstract debug data collected before writing a sidecar. */
typedef struct FengDebugInfo {
    FengDebugFrameRecord *frames;
    size_t frame_count;
    size_t frame_capacity;
    FengDebugVariableRecord *variables;
    size_t variable_count;
    size_t variable_capacity;
} FengDebugInfo;

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

/* Resolves one concrete source path to a logical PKG_NAME:// URI. */
bool feng_debug_resolve_source(const FengDebugSourceMapping *sources,
                               size_t source_count,
                               const char *source_path,
                               FengDebugResolvedSource *out_resolved);

/* Releases heap-owned fields inside a resolved source descriptor. */
void feng_debug_resolved_source_dispose(FengDebugResolvedSource *resolved);

/* Initializes an empty abstract debug-info container. */
void feng_debug_info_init(FengDebugInfo *info);

/* Releases every frame and variable record in an abstract debug-info container. */
void feng_debug_info_dispose(FengDebugInfo *info);

/* Appends or validates one frame record inside abstract debug info. */
bool feng_debug_info_add_frame(FengDebugInfo *info,
                               const char *backend_symbol,
                               const char *display_name,
                               FengDebugFramePolicy policy);

/* Appends or validates one variable record inside abstract debug info. */
bool feng_debug_info_add_variable(FengDebugInfo *info,
                                  const char *frame_backend_symbol,
                                  const char *backend_name,
                                  const char *display_name,
                                  const char *read_expr,
                                  FengDebugVariableKind kind);

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
