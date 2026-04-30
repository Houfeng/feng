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

typedef enum FengSemanticTypeFactKind {
    FENG_SEMANTIC_TYPE_FACT_UNKNOWN = 0,
    FENG_SEMANTIC_TYPE_FACT_BUILTIN,
    FENG_SEMANTIC_TYPE_FACT_TYPE_REF,
    FENG_SEMANTIC_TYPE_FACT_DECL
} FengSemanticTypeFactKind;

typedef struct FengSemanticTypeFact {
    const void *site;
    FengSemanticTypeFactKind kind;
    FengSlice builtin_name;
    const FengTypeRef *type_ref;
    const FengDecl *type_decl;
} FengSemanticTypeFact;

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
    FengSemanticTypeFact *type_facts;
    size_t type_fact_count;
    size_t type_fact_capacity;
    FengSpecRelation *spec_relations;
    size_t spec_relation_count;
    size_t spec_relation_capacity;
    FengSpecCoercionSite *spec_coercion_sites;
    size_t spec_coercion_site_count;
    size_t spec_coercion_site_capacity;
    struct FengSpecDefaultBinding *spec_default_bindings;
    size_t spec_default_binding_count;
    size_t spec_default_binding_capacity;
    struct FengSpecMemberAccess *spec_member_accesses;
    size_t spec_member_access_count;
    size_t spec_member_access_capacity;
    struct FengSpecWitness *spec_witnesses;
    size_t spec_witness_count;
    size_t spec_witness_capacity;
    struct FengSpecEquality *spec_equalities;
    size_t spec_equality_count;
    size_t spec_equality_capacity;
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

bool feng_semantic_record_type_fact(const FengSemanticAnalysis *analysis,
                                    const void *site,
                                    FengSemanticTypeFactKind kind,
                                    FengSlice builtin_name,
                                    const FengTypeRef *type_ref,
                                    const FengDecl *type_decl);

const FengSemanticTypeFact *feng_semantic_lookup_type_fact(const FengSemanticAnalysis *analysis,
                                                           const void *site);

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

/* --- SpecDefaultBinding (Phase S2-a, §6.3 / §9.3) --------------------- */

/* Syntactic position of a default-witness site. Each enumerator covers one
 * production path where the language admits a spec-typed slot without an
 * explicit initializer/value, requiring the spec's default witness instead.
 *
 *   LOCAL_BINDING — `let s: S;` / `var s: S;` at statement scope (and the
 *                   equivalent global binding decl form).
 *   TYPE_FIELD    — A `let`/`var` field of a `type` whose declared type is
 *                   a spec and whose initializer is omitted at the member
 *                   declaration site. */
typedef enum FengSpecDefaultBindingPosition {
    FENG_SPEC_DEFAULT_BINDING_POSITION_LOCAL_BINDING = 0,
    FENG_SPEC_DEFAULT_BINDING_POSITION_TYPE_FIELD
} FengSpecDefaultBindingPosition;

/* One entry per default-witness site. Indexed by the AST node whose absent
 * initializer triggered the site:
 *
 *   LOCAL_BINDING — key is the FengBinding* (which lives inside the parent
 *                   FengStmt or FengDecl; the binding pointer is stable for
 *                   the lifetime of the parser arena and uniquely
 *                   identifies the site).
 *   TYPE_FIELD    — key is the FengTypeMember* of the field.
 *
 * `form` mirrors SpecCoercionSite::form: object-form vs callable-form spec.
 * The witness itself (which value to use) is computed in a later phase
 * (§6.5) — this sidecar only marks the site so the witness pass can find
 * it. */
typedef struct FengSpecDefaultBinding {
    const void *site;
    FengSpecDefaultBindingPosition position;
    FengSpecCoercionForm form;
    const FengDecl *spec_decl;
} FengSpecDefaultBinding;

/* Record a default-witness site. `site` MUST be the AST node pointer
 * matching `position` (FengBinding* for LOCAL_BINDING, FengTypeMember* for
 * TYPE_FIELD). `spec_decl` MUST be a FENG_DECL_SPEC. Recording the same
 * site twice replaces the earlier entry. Implemented in
 * spec_default_bindings.c. */
bool feng_semantic_record_spec_default_binding(
    const FengSemanticAnalysis *analysis,
    const void *site,
    FengSpecDefaultBindingPosition position,
    FengSpecCoercionForm form,
    const FengDecl *spec_decl);

/* Look up the default-binding entry for `site`. Returns NULL when the site
 * was not recorded. The returned pointer is stable until
 * feng_semantic_analysis_free. */
const FengSpecDefaultBinding *feng_semantic_lookup_spec_default_binding(
    const FengSemanticAnalysis *analysis,
    const void *site);

/* --- SpecMemberAccess (Phase S2-b, §6.4 / §9.4) ----------------------- */

/* Kind of access performed at a `obj.member` site whose `obj` static type is
 * an object-form spec. METHOD_CALL covers any access to a method member
 * (whether the result is invoked immediately or used as a method value).
 * FIELD_READ / FIELD_WRITE distinguish read vs assignment-target uses of a
 * field member; the analyzer records FIELD_READ at member-expression
 * resolution time and upgrades to FIELD_WRITE once the parent assignment
 * statement is observed. */
typedef enum FengSpecMemberAccessKind {
    FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_READ = 0,
    FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_WRITE,
    FENG_SPEC_MEMBER_ACCESS_KIND_METHOD_CALL
} FengSpecMemberAccessKind;

/* One entry per `obj.member` member-expression whose owner static type is
 * an object-form spec. Indexed by the FengExpr* of the member expression
 * itself. `member` points into the spec's closure of declared members
 * (returned by find_spec_object_member). `field_mutability` is meaningful
 * only for FIELD_READ / FIELD_WRITE. */
typedef struct FengSpecMemberAccess {
    const FengExpr *expr;
    const FengDecl *spec_decl;
    const FengTypeMember *member;
    FengSpecMemberAccessKind kind;
    FengMutability field_mutability;
} FengSpecMemberAccess;

/* Record a member-access site. The recorder is invoked at member-expression
 * resolution time with FIELD_READ / METHOD_CALL based on `member->kind`;
 * `feng_semantic_upgrade_spec_member_access_to_write` upgrades a previously
 * recorded FIELD_READ to FIELD_WRITE when the same expression is later
 * observed as the LHS of an assignment. Implemented in
 * spec_member_accesses.c. */
bool feng_semantic_record_spec_member_access(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *spec_decl,
    const FengTypeMember *member,
    FengSpecMemberAccessKind kind);

/* Upgrade an existing FIELD_READ entry for `expr` to FIELD_WRITE. No-op
 * when the entry does not exist (the expression's owner type is not a spec)
 * or the entry is already FIELD_WRITE / METHOD_CALL (latter is a programmer
 * error and ignored — assigning to a method value is rejected elsewhere). */
void feng_semantic_upgrade_spec_member_access_to_write(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr);

/* Look up the member-access entry for `expr`. Returns NULL when no entry
 * was recorded. */
const FengSpecMemberAccess *feng_semantic_lookup_spec_member_access(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr);

/* --- SpecWitness (Phase S3, §6.5 / §8.1 / §8.2 / §9.5) --------------- */

/* Per-member implementation source within a (T, S) witness. Each entry maps
 * one member of S's closure to the T-side implementation that satisfies it.
 *
 *   TYPE_OWN_FIELD  — the field lives on `type_decl` itself.
 *   TYPE_OWN_METHOD — the method lives on `type_decl` itself.
 *   FIT_METHOD      — the method is provided by `via_fit_decl`, owned by
 *                     `provider_module`.
 *
 * `spec_member` points into S's closure (returned by find_spec_object_member
 * when looking up by name). `impl_member` points at the chosen T-side
 * field/method (T's own member or the fit-body member). `via_fit_decl` and
 * `provider_module` are non-NULL only for FIT_METHOD. */
typedef enum FengSpecWitnessSourceKind {
    FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_FIELD = 0,
    FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD,
    FENG_SPEC_WITNESS_SOURCE_FIT_METHOD
} FengSpecWitnessSourceKind;

typedef struct FengSpecWitnessMember {
    const FengTypeMember *spec_member;
    const FengTypeMember *impl_member;
    FengSpecWitnessSourceKind source_kind;
    const FengDecl *via_fit_decl;
    const FengSemanticModule *provider_module;
} FengSpecWitnessMember;

/* One witness entry per (type_decl, spec_decl) pair that has been demanded
 * by at least one coercion site (per §8.2 — on-demand cache). The members
 * array follows the iteration order of S's member closure. The entry
 * pointer is stable until feng_semantic_analysis_free.
 *
 * If S's member closure contains a member that the (T, S) visible face
 * could not unambiguously satisfy (missing implementation, or §8.1
 * multi-source ambiguity), the corresponding `members[i].impl_member` is
 * NULL. The conflict is reported as a semantic error at the coercion site
 * that triggered the witness compute; subsequent lookups simply observe
 * the NULL slot. */
typedef struct FengSpecWitness {
    const FengDecl *type_decl;
    const FengDecl *spec_decl;
    FengSpecWitnessMember *members;
    size_t member_count;
    size_t member_capacity;
} FengSpecWitness;

/* Look up the witness entry for (type_decl, spec_decl). Returns NULL when
 * no coercion has yet demanded this (T, S) pair (per §8.2). */
const FengSpecWitness *feng_semantic_lookup_spec_witness(
    const FengSemanticAnalysis *analysis,
    const FengDecl *type_decl,
    const FengDecl *spec_decl);

/* Reserve and return a fresh witness entry for (type_decl, spec_decl). The
 * caller is expected to populate `members` via
 * feng_semantic_spec_witness_append_member after reservation. Returns NULL
 * on allocation failure or when an entry already exists (callers should
 * check feng_semantic_lookup_spec_witness first). */
FengSpecWitness *feng_semantic_reserve_spec_witness(
    const FengSemanticAnalysis *analysis,
    const FengDecl *type_decl,
    const FengDecl *spec_decl);

/* Append one member entry to a witness reserved by
 * feng_semantic_reserve_spec_witness. Returns false on allocation failure.
 * `impl_member` may be NULL to record an unresolved/conflicted slot.
 * `via_fit_decl` and `provider_module` must be non-NULL iff
 * `source_kind == FIT_METHOD`. */
bool feng_semantic_spec_witness_append_member(
    FengSpecWitness *witness,
    const FengTypeMember *spec_member,
    const FengTypeMember *impl_member,
    FengSpecWitnessSourceKind source_kind,
    const FengDecl *via_fit_decl,
    const FengSemanticModule *provider_module);

/* --- SpecEquality (Phase S4, §6.6 / §9.6) ---------------------------- */

/* Operator kind for a recorded equality site. The semantic conclusion is
 * identical for both — `==` / `!=` on a spec-typed operand are reference-
 * identity comparisons (per §6.6) — so codegen reads `is_neq` only to emit
 * the right boolean polarity, not to choose a different comparison path. */
typedef enum FengSpecEqualityOp {
    FENG_SPEC_EQUALITY_OP_EQ = 0,
    FENG_SPEC_EQUALITY_OP_NE
} FengSpecEqualityOp;

/* One entry per binary `==` / `!=` expression where at least one operand's
 * static type is a spec. Validation upstream
 * (validate_binary_expr / binary_expr_types_are_valid) requires the two
 * operands to have the same static type, so when `spec_decl` is recorded
 * here both sides are guaranteed to be that same spec. The keying is the
 * binary FengExpr* itself; lookup returns NULL for non-spec equality
 * expressions. */
typedef struct FengSpecEquality {
    const FengExpr *expr;
    const FengDecl *spec_decl;
    FengSpecEqualityOp op;
} FengSpecEquality;

/* Record a SpecEquality site. `expr` must be a FENG_EXPR_BINARY whose op is
 * `==` or `!=`; `spec_decl` must be a FENG_DECL_SPEC. Recording the same
 * expression twice replaces the earlier entry. Implemented in
 * spec_equalities.c. */
bool feng_semantic_record_spec_equality(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *spec_decl,
    FengSpecEqualityOp op);

/* Look up the equality entry for `expr`. Returns NULL when no entry was
 * recorded (e.g., the operands' static type is not a spec). */
const FengSpecEquality *feng_semantic_lookup_spec_equality(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr);

/* --- Value-kind classification (dev/feng-value-model-delivered.md §6.1) - */

/* Runtime classification of a Feng value, mirroring runtime
 * FengValueKind in src/runtime/feng_runtime.h. Per
 * dev/feng-value-model-delivered.md §2 / §6.1 every Feng type belongs to
 * exactly one of these three categories; codegen uses the classification
 * to pick an emit path (direct C copy / single-pointer ARC primitives /
 * the five aggregate APIs). The semantic layer is the single source of
 * truth for the classification rule — codegen consumes the helpers
 * declared below rather than re-deriving the rule.
 *
 * The enumerator values intentionally match
 * runtime/FengValueKind so that callers may pass either enum across the
 * boundary; the names are kept distinct because semantic operates on AST
 * decls / type refs whereas the runtime enum tags runtime values. */
typedef enum FengSemanticValueKind {
    FENG_SEMANTIC_VALUE_TRIVIAL = 1,
    FENG_SEMANTIC_VALUE_MANAGED_POINTER = 2,
    FENG_SEMANTIC_VALUE_AGGREGATE = 3
} FengSemanticValueKind;

/* Classify the value kind of a built-in primitive named by `name` (any
 * spelling accepted by the analyzer's built-in name table — both canonical
 * and alias forms). Per §6.1:
 *   - "string" → MANAGED_POINTER (FengString *).
 *   - any numeric (i8…u64, f32, f64, and aliases int/long/byte/float/double)
 *     and "bool" → TRIVIAL.
 *   - "void" → TRIVIAL (used only for return slots; callers must not
 *     materialize a runtime value of this kind).
 *   - any other / unknown name → TRIVIAL (defensive default; the analyzer
 *     itself rejects unknown built-in spellings before this point).
 *
 * Implemented in value_kind.c. The function is pure — it does not touch
 * any FengSemanticAnalysis state. */
FengSemanticValueKind feng_semantic_value_kind_of_builtin(FengSlice name);

/* Classify the value kind of a user-declared `type` or `spec`. `decl` MUST
 * be non-NULL and MUST be FENG_DECL_TYPE or FENG_DECL_SPEC; passing any
 * other DeclKind is a programmer error and yields TRIVIAL (defensive).
 * Per §6.1:
 *   - FENG_DECL_TYPE → MANAGED_POINTER (heap object + FengManagedHeader).
 *   - FENG_DECL_SPEC, object form → AGGREGATE (fat value: subject +
 *     witness, see fat-value mapping in
 *     dev/feng-spec-codegen-pending.md §4).
 *   - FENG_DECL_SPEC, callable form → MANAGED_POINTER (closure pointer;
 *     callable specs are not fat values, see §8.4).
 *
 * Implemented in value_kind.c. The function does not consult the
 * analysis; the decl carries enough information on its own. */
FengSemanticValueKind feng_semantic_value_kind_of_decl(const FengDecl *decl);

#ifdef __cplusplus
}
#endif

#endif
