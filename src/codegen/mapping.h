#ifndef FENG_CODEGEN_MAPPING_H
#define FENG_CODEGEN_MAPPING_H

#include <stdbool.h>
#include <stddef.h>

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

/* Codegen-owned abstract mapping data collected before any sidecar write. */
typedef struct FengDebugInfo {
    FengDebugFrameRecord *frames;
    size_t frame_count;
    size_t frame_capacity;
    FengDebugVariableRecord *variables;
    size_t variable_count;
    size_t variable_capacity;
} FengDebugInfo;

/* Resolves one concrete source path to a logical PKG_NAME:// URI. */
bool feng_debug_resolve_source(const FengDebugSourceMapping *sources,
                               size_t source_count,
                               const char *source_path,
                               FengDebugResolvedSource *out_resolved);

/* Releases heap-owned fields inside a resolved source descriptor. */
void feng_debug_resolved_source_dispose(FengDebugResolvedSource *resolved);

/* Initializes an empty abstract mapping container. */
void feng_debug_info_init(FengDebugInfo *info);

/* Releases every frame and variable record in an abstract mapping container. */
void feng_debug_info_dispose(FengDebugInfo *info);

/* Appends or validates one frame record inside abstract mapping info. */
bool feng_debug_info_add_frame(FengDebugInfo *info,
                               const char *backend_symbol,
                               const char *display_name,
                               FengDebugFramePolicy policy);

/* Appends or validates one variable record inside abstract mapping info. */
bool feng_debug_info_add_variable(FengDebugInfo *info,
                                  const char *frame_backend_symbol,
                                  const char *backend_name,
                                  const char *display_name,
                                  const char *read_expr,
                                  FengDebugVariableKind kind);

#ifdef __cplusplus
}
#endif

#endif /* FENG_CODEGEN_MAPPING_H */
