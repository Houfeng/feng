#ifndef FENG_CODEGEN_MAPPING_H
#define FENG_CODEGEN_MAPPING_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maps one compiled source file to its owning package identity. */
typedef struct FengCodegenMapingSourceMapping {
    const char *source_path;
    const char *package_name;
    const char *package_root;
} FengCodegenMapingSourceMapping;

/* Resolved logical-source information derived from a source mapping entry. */
typedef struct FengCodegenMapingResolvedSource {
    char *package_name;
    char *package_root;
    char *relative_path;
    char *logical_uri;
} FengCodegenMapingResolvedSource;

/* Controls how a backend frame should be surfaced in the debugger. */
typedef enum FengCodegenMapingFramePolicy {
    FENG_CODEGEN_MAPING_FRAME_VISIBLE = 0,
    FENG_CODEGEN_MAPING_FRAME_HIDDEN = 1,
    FENG_CODEGEN_MAPING_FRAME_COLLAPSE = 2
} FengCodegenMapingFramePolicy;

/* Classifies a user-visible variable for scope rewriting. */
typedef enum FengCodegenMapingVariableKind {
    FENG_CODEGEN_MAPING_VARIABLE_PARAM = 0,
    FENG_CODEGEN_MAPING_VARIABLE_BINDING = 1,
    FENG_CODEGEN_MAPING_VARIABLE_CAPTURE = 2,
    FENG_CODEGEN_MAPING_VARIABLE_SELF = 3
} FengCodegenMapingVariableKind;

/* Maps one backend frame symbol to a user-facing frame name and policy. */
typedef struct FengCodegenMapingFrameRecord {
    char *backend_symbol;
    char *display_name;
    FengCodegenMapingFramePolicy policy;
} FengCodegenMapingFrameRecord;

/* Maps one backend variable slot or synthetic expression to a user variable. */
typedef struct FengCodegenMapingVariableRecord {
    char *frame_backend_symbol;
    char *backend_name;
    char *display_name;
    char *read_expr;
    FengCodegenMapingVariableKind kind;
} FengCodegenMapingVariableRecord;

/* Codegen-owned abstract mapping data collected before any sidecar write. */
typedef struct FengCodegenMapingInfo {
    FengCodegenMapingFrameRecord *frames;
    size_t frame_count;
    size_t frame_capacity;
    FengCodegenMapingVariableRecord *variables;
    size_t variable_count;
    size_t variable_capacity;
} FengCodegenMapingInfo;

/* Resolves one concrete source path to a logical PKG_NAME:// URI. */
bool feng_codegen_maping_resolve_source(const FengCodegenMapingSourceMapping *sources,
                               size_t source_count,
                               const char *source_path,
                               FengCodegenMapingResolvedSource *out_resolved);

/* Releases heap-owned fields inside a resolved source descriptor. */
void feng_codegen_maping_resolved_source_dispose(FengCodegenMapingResolvedSource *resolved);

/* Initializes an empty abstract mapping container. */
void feng_codegen_maping_info_init(FengCodegenMapingInfo *info);

/* Releases every frame and variable record in an abstract mapping container. */
void feng_codegen_maping_info_dispose(FengCodegenMapingInfo *info);

/* Appends or validates one frame record inside abstract mapping info. */
bool feng_codegen_maping_info_add_frame(FengCodegenMapingInfo *info,
                               const char *backend_symbol,
                               const char *display_name,
                               FengCodegenMapingFramePolicy policy);

/* Appends or validates one variable record inside abstract mapping info. */
bool feng_codegen_maping_info_add_variable(FengCodegenMapingInfo *info,
                                  const char *frame_backend_symbol,
                                  const char *backend_name,
                                  const char *display_name,
                                  const char *read_expr,
                                  FengCodegenMapingVariableKind kind);

#ifdef __cplusplus
}
#endif

#endif /* FENG_CODEGEN_MAPPING_H */
