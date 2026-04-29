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

/* Form of a spec coercion site — see dev/feng-spec-semantic-draft.md §6.2. */
typedef enum FengSpecCoercionForm {
    /* Concrete type → object-form spec. The site references the SpecRelation
     * picked by the resolver for this (T, S) coercion. */
    FENG_SPEC_COERCION_FORM_OBJECT = 0,
    /* Callable value → callable-form spec / function type. Carries the
     * value-source classification per §6.2; signature is read from the
     * target callable decl. */
    FENG_SPEC_COERCION_FORM_CALLABLE
} FengSpecCoercionForm;

/* Origin of the callable value being coerced to a callable-form spec. The
 * classification mirrors the dispatch in resolve_expr_callable_value and is
 * stable across multiple coercion points referring to the same value. */
typedef enum FengSpecCoercionCallableSource {
    /* A top-level (module-scope) function value, possibly overload-resolved. */
    FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN = 0,
    /* A bound method value `obj.method` taken as a callable. */
    FENG_SPEC_COERCION_CALLABLE_SOURCE_METHOD_VALUE,
    /* A lambda literal at the coercion site. */
    FENG_SPEC_COERCION_CALLABLE_SOURCE_LAMBDA,
    /* Any other callable-typed value (local binding, parameter, field,
     * member access whose static type is callable, etc.). */
    FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER
} FengSpecCoercionCallableSource;

/* Per-site decision for a single coercion point. Stored in a sidecar table
 * keyed by AST FengExpr pointer to keep parser/AST free of semantic
 * back-references. Populated incrementally during resolution by the analyzer
 * each time validate_expr_against_expected_type confirms a match into a
 * spec-typed slot.
 *
 * Per §8.4 callable-form specs do not enter SpecRelation; relation is NULL
 * for FORM_CALLABLE. For FORM_OBJECT the relation pointer is stable until
 * feng_semantic_analysis_free. */
typedef struct FengSpecCoercionSite {
    const FengExpr *expr;
    FengSpecCoercionForm form;
    /* OBJECT form: the concrete source `type` decl. Always non-NULL. */
    const FengDecl *src_type_decl;
    /* The target spec / function-type decl. Always non-NULL. */
    const FengDecl *target_spec_decl;
    /* OBJECT form only: the SpecRelation entry that justifies this coercion.
     * Always non-NULL for FORM_OBJECT (analyzer asserts the lookup succeeds
     * before recording). NULL for FORM_CALLABLE per §8.4. */
    const FengSpecRelation *relation;
    /* CALLABLE form only: classification of the value source. Unspecified
     * for FORM_OBJECT. */
    FengSpecCoercionCallableSource callable_source;
} FengSpecCoercionSite;

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
    FengSpecCoercionSite *spec_coercion_sites;
    size_t spec_coercion_site_count;
    size_t spec_coercion_site_capacity;
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

/* --- SpecCoercionSite (Phase S1b, §6.2) ------------------------------ */

/* Record an object-form coercion site (`expr` of concrete type
 * `src_type_decl` flowing into a slot typed as object-form spec
 * `target_spec_decl`). `relation` MUST be the SpecRelation entry that
 * justifies this coercion (caller is responsible for looking it up via
 * feng_semantic_lookup_spec_relation and confirming non-NULL). All four
 * pointers must be non-NULL. Recording the same `expr` twice replaces the
 * earlier entry — the analyzer is expected to call this exactly once per
 * coercion site, but the replace-on-conflict policy keeps the table
 * consistent if a re-resolution path runs.
 *
 * Implemented in spec_coercion_sites.c. */
bool feng_semantic_record_object_spec_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *src_type_decl,
    const FengDecl *target_spec_decl,
    const FengSpecRelation *relation);

/* Record a callable-form coercion site. `target_spec_decl` is the
 * callable-form spec decl (or function-type decl). `callable_source`
 * classifies the value origin per §6.2. Per §8.4 callable-form specs do
 * not enter SpecRelation, so no relation is associated.
 *
 * Implemented in spec_coercion_sites.c. */
bool feng_semantic_record_callable_spec_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *target_spec_decl,
    FengSpecCoercionCallableSource callable_source);

/* Look up the recorded coercion site for `expr`. Returns NULL when no site
 * was recorded (either the expression is not a coercion site, or the
 * resolver did not visit it as one). The returned pointer is stable until
 * feng_semantic_analysis_free. */
const FengSpecCoercionSite *feng_semantic_lookup_spec_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr);

#ifdef __cplusplus
}
#endif

#endif
