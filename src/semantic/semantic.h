#ifndef FENG_SEMANTIC_SEMANTIC_H
#define FENG_SEMANTIC_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>

#include "parser/parser.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct FengSemanticError {
    const char *path;
    char *message;
    FengToken token;
} FengSemanticError;

typedef struct FengSemanticInfo {
    const char *path;
    char *message;
    FengToken token;
} FengSemanticInfo;

typedef struct FengSemanticModule {
    const FengSlice *segments;
    size_t segment_count;
    FengVisibility visibility;
    const FengProgram **programs;
    size_t program_count;
    size_t program_capacity;
} FengSemanticModule;

/* Per-`type` marker computed from the static managed-reference graph.
 * `is_potentially_cyclic` is true iff the type sits in a non-trivial strongly
 * connected component (self-loop or 2+ node cycle). Codegen consults this to
 * set FengTypeDescriptor.is_potentially_cyclic so the runtime cycle collector
 * can skip acyclic objects. */
typedef struct FengSemanticTypeMarker {
    const FengSemanticModule *module;
    const FengDecl *type_decl;
    bool is_potentially_cyclic;
} FengSemanticTypeMarker;

typedef struct FengSemanticAnalysis {
    FengSemanticModule *modules;
    size_t module_count;
    size_t module_capacity;
    FengSemanticInfo *infos;
    size_t info_count;
    size_t info_capacity;
    FengSemanticTypeMarker *type_markers;
    size_t type_marker_count;
    size_t type_marker_capacity;
} FengSemanticAnalysis;

typedef enum FengCompileTarget {
    FENG_COMPILE_TARGET_BIN = 0, /* executable: requires a single `main(args: string[])` entry */
    FENG_COMPILE_TARGET_LIB      /* library: no main entry required */
} FengCompileTarget;

bool feng_semantic_analyze(const FengProgram *const *programs,
                           size_t program_count,
                           FengCompileTarget target,
                           FengSemanticAnalysis **out_analysis,
                           FengSemanticError **out_errors,
                           size_t *out_error_count);

void feng_semantic_analysis_free(FengSemanticAnalysis *analysis);
void feng_semantic_errors_free(FengSemanticError *errors, size_t error_count);
void feng_semantic_infos_free(FengSemanticInfo *infos, size_t info_count);

/* Returns true iff `type_decl` is a `type` declaration that the static
 * managed-reference graph places in a non-trivial SCC. Returns false for any
 * unknown decl (including non-type decls and out-of-analysis decls). */
bool feng_semantic_type_is_potentially_cyclic(const FengSemanticAnalysis *analysis,
                                              const FengDecl *type_decl);

/* Internal post-pass entry — populates analysis->type_markers. Idempotent.
 * Implemented in cyclic.c; declared here so analyzer.c can call it on the
 * success path of feng_semantic_analyze. */
bool feng_semantic_compute_type_cyclicity(FengSemanticAnalysis *analysis);

#ifdef __cplusplus
}
#endif

#endif
