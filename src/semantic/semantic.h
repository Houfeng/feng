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

/* Source classification for a single (type_decl, spec_decl) satisfaction
 * relation. See dev/feng-spec-semantic-draft.md §6 / §9.1. The distinction
 * between HEAD and PARENT preserves the provenance chain needed for
 * diagnostics and for §8.1 visible-surface conflict checks.
 *
 *   DECLARED_HEAD    — `type T :: S { ... }` directly lists S.
 *   DECLARED_PARENT  — S is a transitive parent of some spec listed in T's
 *                      own declared spec list. `via_spec_decl` points at the
 *                      head spec from T's list that reaches S.
 *   FIT_HEAD         — `fit T :: S { ... }` directly lists S. `via_fit_decl`
 *                      points at the fit; `provider_module` is the module
 *                      owning that fit.
 *   FIT_PARENT       — S is a transitive parent of a spec listed on a fit
 *                      for T. `via_spec_decl` is the head spec from the
 *                      fit's list; `via_fit_decl` and `provider_module` are
 *                      the same as FIT_HEAD. */
typedef enum FengSpecRelationSourceKind {
    FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD = 0,
    FENG_SPEC_RELATION_SOURCE_DECLARED_PARENT,
    FENG_SPEC_RELATION_SOURCE_FIT_HEAD,
    FENG_SPEC_RELATION_SOURCE_FIT_PARENT
} FengSpecRelationSourceKind;

typedef struct FengSpecRelationSource {
    FengSpecRelationSourceKind kind;
    /* For DECLARED_HEAD / FIT_HEAD: equal to the relation's spec_decl.
     * For DECLARED_PARENT / FIT_PARENT: the head spec (from the type's
     * declared list, or from the fit's spec list) that reaches spec_decl
     * transitively through its parent_specs chain. */
    const FengDecl *via_spec_decl;
    /* NULL for DECLARED_*; the source fit decl for FIT_*. */
    const FengDecl *via_fit_decl;
    /* Module that owns via_fit_decl. NULL for DECLARED_*. */
    const FengSemanticModule *provider_module;
} FengSpecRelationSource;

/* One relation entry per (type_decl, spec_decl) pair that has at least one
 * source (declared, transitive, or via any fit anywhere in the analysis).
 * Visibility filtering by consumer module is the caller's responsibility —
 * see feng_semantic_spec_relation_source_visible_from. */
typedef struct FengSpecRelation {
    const FengDecl *type_decl;
    const FengDecl *spec_decl;
    FengSpecRelationSource *sources;
    size_t source_count;
    size_t source_capacity;
} FengSpecRelation;

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
    FengSpecRelation *spec_relations;
    size_t spec_relation_count;
    size_t spec_relation_capacity;
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

/* Internal post-pass entry — populates analysis->spec_relations with one
 * entry per (type_decl, spec_decl) pair that has at least one source in the
 * analysis. Idempotent. Implemented in spec_relations.c. Called on the
 * success path of feng_semantic_analyze. */
bool feng_semantic_compute_spec_relations(FengSemanticAnalysis *analysis);

/* Look up the relation entry for (type_decl, spec_decl). Returns NULL if no
 * source exists anywhere in the analysis. The returned pointer is stable
 * until feng_semantic_analysis_free. */
const FengSpecRelation *feng_semantic_lookup_spec_relation(
    const FengSemanticAnalysis *analysis,
    const FengDecl *type_decl,
    const FengDecl *spec_decl);

/* Returns true iff `source` is visible from a consumer file located in
 * `consumer_module` whose `use` list resolves to the modules in
 * `consumer_imports[0..consumer_import_count)`. DECLARED_* sources are
 * always visible. FIT_* sources are visible iff the fit's provider module
 * is the consumer module itself, or the fit is `pu` and the consumer
 * imported the provider module. Mirrors docs/feng-fit.md §4. */
bool feng_semantic_spec_relation_source_visible_from(
    const FengSpecRelationSource *source,
    const FengSemanticModule *consumer_module,
    const FengSemanticModule *const *consumer_imports,
    size_t consumer_import_count);

#ifdef __cplusplus
}
#endif

#endif
