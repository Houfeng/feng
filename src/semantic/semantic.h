#ifndef FENG_SEMANTIC_SEMANTIC_H
#define FENG_SEMANTIC_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "parser/parser.h"

#ifdef __cplusplus
extern "C" {
#endif


/* One secondary source location associated with a semantic diagnostic. */
typedef struct FengSemanticRelatedLocation {
    FengToken token;
    const char *path;
    char *message;
} FengSemanticRelatedLocation;

/* One semantic diagnostic and its optional structured source relations. */
typedef struct FengSemanticError {
    FengToken token;
    const char *code;
    char *message;
    const char *path;
    FengSemanticRelatedLocation *related_locations;
    size_t related_location_count;
} FengSemanticError;

typedef struct FengSemanticInfo {
    const char *path;
    char *message;
    FengToken token;
} FengSemanticInfo;

typedef enum FengSemanticModuleOrigin {
    FENG_SEMANTIC_MODULE_ORIGIN_LOCAL = 0,
    FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE
} FengSemanticModuleOrigin;

typedef struct FengSemanticModule {
    const FengSlice *segments;
    size_t segment_count;
    FengVisibility visibility;
    const FengProgram **programs;
    size_t program_count;
    size_t program_capacity;
    /* Tracks whether this module comes from the current compile input or from
     * an imported .fb bundle. Imported-package modules have no local source
     * body; codegen skips local body emission and semantic conflict passes
     * treat them as pre-resolved package output. */
    FengSemanticModuleOrigin origin;
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

typedef struct FengSemanticEnumItemInfo {
    const FengEnumItem *item;
    size_t ordinal;
    int64_t value;
} FengSemanticEnumItemInfo;

typedef struct FengSemanticEnumInfo {
    const FengDecl *enum_decl;
    const FengEnumItem *first_item;
    int64_t first_value;
    FengSemanticEnumItemInfo *items;
    size_t item_count;
    size_t item_capacity;
} FengSemanticEnumInfo;

/* Source classification for a single (type_decl, spec_decl) satisfaction
 * relation. See docs/engineering/feng-spec-semantic-delivered.md §6 / §9.1. The distinction
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

typedef enum FengSemanticSubjectKeyKind {
    FENG_SEMANTIC_SUBJECT_KEY_INVALID = 0,
    FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL,
    FENG_SEMANTIC_SUBJECT_KEY_BUILTIN,
    FENG_SEMANTIC_SUBJECT_KEY_ARRAY
} FengSemanticSubjectKeyKind;

typedef struct FengSemanticArraySubjectKey {
    const FengTypeRef *element_type_ref;
    size_t rank;
    uint64_t writable_mask;
} FengSemanticArraySubjectKey;

typedef struct FengSemanticSubjectKey {
    FengSemanticSubjectKeyKind kind;
    union {
        const FengDecl *type_decl;
        const char *builtin_canonical_name;
        FengSemanticArraySubjectKey array;
    } as;
} FengSemanticSubjectKey;

/* One relation entry per (subject_key, spec_decl) pair that has at least one
 * source (declared, transitive, or via any fit anywhere in the analysis).
 * The subject_key identifies the fit target: a user type decl, a builtin
 * canonical name, or a structured array key. Visibility filtering by consumer
 * module is the caller's responsibility — see
 * feng_semantic_spec_relation_source_visible_from. */
typedef struct FengSpecRelation {
    FengSemanticSubjectKey subject_key;
    const FengDecl *spec_decl;
    FengSpecRelationSource *sources;
    size_t source_count;
    size_t source_capacity;
} FengSpecRelation;

/* Form of a spec coercion site — see docs/engineering/feng-spec-semantic-delivered.md §6.2. */
typedef enum FengSpecCoercionForm {
    /* Concrete type → object-form spec. The site references the SpecRelation
     * picked by the resolver for this (T, S) coercion. */
    FENG_SPEC_COERCION_FORM_OBJECT = 0,
    /* Object-form child spec value -> nominal parent spec view. The subject
     * is preserved and codegen follows object_upcast_parent_indices through
     * the source witness's named direct-parent fields. */
    FENG_SPEC_COERCION_FORM_OBJECT_UPCAST,
    /* Callable value → callable-form spec / function type. Carries the
     * value-source classification per §6.2; signature is read from the
     * target callable decl. */
    FENG_SPEC_COERCION_FORM_CALLABLE,
    /* `&top_level_abi_fn` → `Foo*` where `Foo` is a callable-form `@abi`
     * spec. Semantic resolves the exact target spec and source function so
     * codegen does not have to guess which ABI function-pointer surface the
     * site chose. */
    FENG_SPEC_COERCION_FORM_ABI_FUNCTION_POINTER,
    /* Concrete type → intersection-form spec. Like FORM_OBJECT the value
     * is wrapped into a fat-spec `{ subject, witness }`, but the witness
     * is a *merged* witness assembled from the subject's per-member-spec
     * witnesses. There is no single SpecRelation justifying the coercion
     * (satisfaction is per member spec), so `relation` is NULL for this
     * form — see FengSpecCoercionSite.relation. */
    FENG_SPEC_COERCION_FORM_INTERSECTION
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

/* Object-form coercion site 的 subject 承载策略。
 * - BORROW_LOCAL: 仅允许在可证明不逃逸的临时调用点借用局部物化地址。
 * - BOX_OWNER: 需要稳定 owner 生命周期时使用 FengScalarBox。
 */
typedef enum FengSpecObjectSubjectStorageKind {
    FENG_SPEC_OBJECT_SUBJECT_STORAGE_BORROW_LOCAL = 0,
    FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER = 1
} FengSpecObjectSubjectStorageKind;

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
    /* OBJECT form: subject key of the concrete source type (user type decl,
     * builtin canonical name, or structured array key). Kind is never
     * INVALID for FORM_OBJECT sites. */
    FengSemanticSubjectKey src_subject_key;
    /* The target spec / function-type decl. Always non-NULL. */
    const FengDecl *target_spec_decl;
    /* The concrete target spec type ref at the coercion site. For generic
     * spec instances this preserves the instantiated surface (e.g.
     * `Box<int>` rather than only the open `Box` decl). Owned by the
     * analysis: record_* clones the incoming type ref into
     * analysis->coercion_owned_type_refs (which is freed recursively by
     * feng_semantic_analysis_free after codegen), so the pointer remains
     * stable across the entire compile regardless of whether the original
     * was an AST-owned ref or a resolver synthetic ref. */
    const FengTypeRef *target_spec_type_ref;
    /* OBJECT_UPCAST only: ordered direct-parent declaration indices selected
     * by semantic DFS. Each entry indexes the current spec's parent_specs;
     * codegen advances through its exact instantiated UserSpec graph. */
    size_t *object_upcast_parent_indices;
    size_t object_upcast_parent_index_count;
    /* OBJECT form only: the SpecRelation entry that justifies this coercion.
     * Always non-NULL for FORM_OBJECT (analyzer asserts the lookup succeeds
     * before recording). NULL for FORM_CALLABLE per §8.4 and NULL for
     * FORM_INTERSECTION (satisfaction is per member spec, there is no single
     * SpecRelation to point at; codegen derives the merged witness from the
     * subject's per-member witnesses directly). */
    const FengSpecRelation *relation;
    /* OBJECT/INTERSECTION form only: subject 承载策略（借用局部地址或装箱 owner）。
     * CALLABLE form 下未使用。 */
    FengSpecObjectSubjectStorageKind object_subject_storage;
    /* CALLABLE form only: classification of the value source. Unspecified
     * for FORM_OBJECT. */
    FengSpecCoercionCallableSource callable_source;
    /* CALLABLE form only: exact resolved implementation surface. Top-level
     * functions set callable_decl, method values set callable_member plus
     * callable_owner_type_decl, lambda literals set callable_lambda_expr,
     * and already-callable values leave all of these NULL. */
    const FengDecl *callable_decl;
    const FengTypeMember *callable_member;
    const FengDecl *callable_owner_type_decl;
    const FengDecl *callable_fit_decl;
    /* METHOD_VALUE only: caller-view receiver instance, preserving open
     * generic arguments such as Cell<T> or Owner<T>. */
    const FengTypeRef *callable_receiver_type_ref;
    /* TOP_LEVEL_FN / METHOD_VALUE only: explicit callable-local type
     * arguments supplied by `function<T...>` / `object.method<T...>`.
     * The pointer array and every referenced type tree are owned by the
     * semantic analysis. Empty for ordinary non-generic callable values. */
    const FengTypeRef *const *callable_type_args;
    size_t callable_type_arg_count;
    const FengExpr *callable_lambda_expr;
} FengSpecCoercionSite;

/* Normalized member metadata for a union-form spec. The analyzer flattens
 * nested union-form members, removes duplicates while preserving declaration
 * order, and stores the resulting member list here for codegen and tooling.
 * `type_ref` entries are owned by the semantic analysis object.
 * `is_nested_union` is true when the member is itself a union-form spec,
 * enabling multi-level path lookup at coercion sites. */
typedef struct FengUnionSpecMemberInfo {
    const FengTypeRef *type_ref;
    const FengDecl *resolved_decl;
    bool is_nested_union;
} FengUnionSpecMemberInfo;

typedef struct FengUnionSpecInfo {
    const FengDecl *spec_decl;
    FengUnionSpecMemberInfo *members;
    size_t member_count;
} FengUnionSpecInfo;

/* Flattened intersection spec metadata. After validation, nested intersection
 * members are expanded so that `flattened_members` holds only object-form
 * spec decls. Duplicates are removed. */
typedef struct FengIntersectionSpecInfo {
    const FengDecl *spec_decl;
    const FengDecl **flattened_members;
    size_t flattened_member_count;
} FengIntersectionSpecInfo;

/* Records a value-flow site where expression `expr` is wrapped into a
 * union-form spec and assigned one normalized active member. Exact member
 * matches are preferred by the analyzer before spec-satisfaction matches, so
 * codegen can trust `member_index` as the tag to emit.
 * `path_indices` records the multi-level path from the target union to the
 * source type through nested union members; `path_length` is the depth. */
#define UNION_COERCION_MAX_PATH_DEPTH 8U

typedef struct FengUnionCoercionSite {
    const FengExpr *expr;
    const FengDecl *target_union_decl;
    const FengTypeRef *target_union_type_ref;
    size_t member_index;
    const FengTypeRef *member_type_ref;
    size_t path_indices[UNION_COERCION_MAX_PATH_DEPTH];
    size_t path_length;
} FengUnionCoercionSite;

/* 具体化依赖的分类：aggregate（tuple/@value by-value）或 managed（type 托管对象）。 */
typedef enum FengReifiableDepKind {
    FENG_REIFIABLE_DEP_KIND_AGGREGATE = 0,
    FENG_REIFIABLE_DEP_KIND_MANAGED
} FengReifiableDepKind;

/* 单条具体化依赖记录。
 * type_ref 指向 AST 中的泛型类型引用（如 Foo<int,V>），
 * 其类型参数可含 TYPE_REF_TYPE_PARAM 节点（引用 owner 的泛型参数）。 */
typedef struct FengReifiableDep {
    FengReifiableDepKind kind;
    const FengTypeRef *type_ref;
} FengReifiableDep;

/* One statically slotted callable dependency use in a shared body. */
typedef enum FengReifiableCallableDepPurpose {
    /* The shared body directly invokes another generic shared callable. */
    FENG_REIFIABLE_CALLABLE_DEP_DIRECT_CALL = 0,
    /* The shared body forms a callable value. */
    FENG_REIFIABLE_CALLABLE_DEP_CALLABLE_VALUE
} FengReifiableCallableDepPurpose;

/* 当前共享 callable 使用的另一个 callable 相关依赖。所有类型引用均已
 * 转换为 caller 视角，但仍可包含 caller 的活动泛型参数。 */
typedef struct FengReifiableCallableDep {
    FengReifiableCallableDepPurpose purpose;
    FengResolvedCallableKind kind;
    const FengDecl *function_decl;
    const FengDecl *owner_type_decl;
    const FengTypeMember *member;
    const FengDecl *fit_decl;
    const FengTypeRef *owner_instance_type_ref;
    const FengTypeRef *const *callable_type_args;
    size_t callable_type_arg_count;
    /* CALLABLE_VALUE only: callable-form spec selected by the target-typed
     * coercion site, for example Producer<T> in `return self.read`. */
    const FengTypeRef *target_callable_type_ref;
} FengReifiableCallableDep;

/* 一个泛型声明或 callable 的全部具体化依赖。
 * owner_decl 标识顶层声明；owner_member 非 NULL 时标识该声明内的
 * callable member：
 *   - 泛型类型结构：FENG_DECL_TYPE + NULL
 *   - 类型方法：FENG_DECL_TYPE + 对应 FengTypeMember
 *   - 独立泛型函数：FENG_DECL_FUNCTION + NULL
 *   - fit 既有路径：FENG_DECL_FIT + NULL
 * deps 数组按收集顺序追加，同一类型引用不重复记录。 */
typedef struct FengReifiableDepSet {
    const FengDecl *owner_decl;
    const FengTypeMember *owner_member;
    FengReifiableDep *deps;
    size_t dep_count;
    size_t dep_capacity;
    FengReifiableCallableDep *callable_deps;
    size_t callable_dep_count;
    size_t callable_dep_capacity;
} FengReifiableDepSet;

/* Stable FT identity attached to an AST declaration/member synthesized from
 * an imported module. The pair (module_name, symbol_id) uniquely identifies
 * a callable across package symbol graphs; source_node is only a compile-time
 * lookup key and never reaches generated Feng code. */
typedef struct FengImportedSymbolIdentity {
    const void *source_node;
    const void *symbol_decl;
    char *module_name;
    uint32_t symbol_id;
} FengImportedSymbolIdentity;

typedef struct FengSemanticAnalysis {
    FengSemanticModule *modules;
    size_t module_count;
    size_t module_capacity;
    /* Host pointer size in bytes (sizeof(void *)).  Propagated from
     * FengSemanticAnalyzeOptions.pointer_size at analysis entry.  Used by
     * canonical_builtin_type_name() through ResolveContext to resolve
     * platform-dependent aliases.  See docs/engineering/feng-scalar-alias-optimize.md §6. */
    size_t pointer_size;
    FengSemanticInfo *infos;
    size_t info_count;
    size_t info_capacity;
    FengSemanticTypeMarker *type_markers;
    size_t type_marker_count;
    size_t type_marker_capacity;
    FengSemanticTypeFact *type_facts;
    size_t type_fact_count;
    size_t type_fact_capacity;
    FengSemanticEnumInfo *enum_infos;
    size_t enum_info_count;
    size_t enum_info_capacity;
    FengSpecRelation *spec_relations;
    size_t spec_relation_count;
    size_t spec_relation_capacity;
    FengSpecCoercionSite *spec_coercion_sites;
    size_t spec_coercion_site_count;
    size_t spec_coercion_site_capacity;
    FengUnionSpecInfo *union_spec_infos;
    size_t union_spec_info_count;
    size_t union_spec_info_capacity;
    FengIntersectionSpecInfo *intersection_spec_infos;
    size_t intersection_spec_info_count;
    size_t intersection_spec_info_capacity;
    FengUnionCoercionSite *union_coercion_sites;
    size_t union_coercion_site_count;
    size_t union_coercion_site_capacity;
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
    FengReifiableDepSet *reifiable_dep_sets;
    size_t reifiable_dep_set_count;
    size_t reifiable_dep_set_capacity;
    FengImportedSymbolIdentity *imported_symbol_identities;
    size_t imported_symbol_identity_count;
    size_t imported_symbol_identity_capacity;
    /* Compile-time-only normalized @friend metadata. The concrete structure
     * is private to analyzer.c: no entry is exported to FT or consumed by
     * codegen/runtime. */
    struct FengFriendMemberInfo *friend_member_infos;
    size_t friend_member_info_count;
    size_t friend_member_info_capacity;
    /* 由语义分析器合成或深拷贝、并被跨阶段元数据借用的完整 FengTypeRef 树。
     * analysis 拥有根节点及其递归 type_args / inner / segments。 */
    FengTypeRef **synthesized_type_refs;
    size_t synthesized_type_ref_count;
    size_t synthesized_type_ref_capacity;
    /* reifiable deps post-pass 为 GENERIC_TARGET 合成的轻量包装引用。
     * analysis 仅拥有包装节点及 segments；type_args 借用源码 AST。 */
    FengTypeRef **reifiable_wrapper_type_refs;
    size_t reifiable_wrapper_type_ref_count;
    size_t reifiable_wrapper_type_ref_capacity;
    /* Coercion site 拥有的 type ref 克隆（堆分配，递归包含 type_args / inner）。
     * target_spec_type_ref 可能来自 resolver 的 per-program synthetic type-ref
     * 池（在 resolver_free_scopes 中释放，先于 codegen），因此 record_* 入口将
     * 其深拷贝到此数组，由 analysis 持有，在 feng_semantic_analysis_free
     * （codegen 之后）统一递归释放。coercion site 只持有这些克隆的借用指针。 */
    FengTypeRef **coercion_owned_type_refs;
    size_t coercion_owned_type_ref_count;
    size_t coercion_owned_type_ref_capacity;
} FengSemanticAnalysis;

typedef enum FengCompileTarget {
    FENG_COMPILE_TARGET_BIN = 0, /* executable: requires a single `main(args: string[])` entry */
    FENG_COMPILE_TARGET_LIB      /* library: no main entry required */
} FengCompileTarget;

typedef struct FengSemanticImportedModuleQuery {
    const void *user;
    /* Return the pre-built FengSemanticModule for the given path, or NULL if
     * the module is not available in any registered external package.  The
     * returned pointer must remain valid for the lifetime of the analysis
     * (i.e. until feng_semantic_analysis_free is called).
     * Returning non-NULL is equivalent to "module exists"; returning NULL
     * means the module was not found and the analyzer will report an error. */
    const FengSemanticModule *(*get_module)(const void *user,
                                            const FengSlice *segments,
                                            size_t segment_count);
} FengSemanticImportedModuleQuery;

typedef struct FengSemanticAnalyzeOptions {
    FengCompileTarget target;
    const FengSemanticImportedModuleQuery *imported_modules;
    /* Host pointer size in bytes (sizeof(void *)).  Drives platform-dependent
     * alias resolution (e.g. `int` → `i32` on 32-bit, `i64` on 64-bit).
     * Caller (CLI layer) fills this from feng_get_host_pointer_size().
     * Must be non-zero.
     * Future: when cross-compilation is supported, pass target pointer size
     * via a dedicated compile option instead of host sizeof(void *). */
    size_t pointer_size;
} FengSemanticAnalyzeOptions;

bool feng_semantic_analyze_with_options(const FengProgram *const *programs,
                                        size_t program_count,
                                        const FengSemanticAnalyzeOptions *options,
                                        FengSemanticAnalysis **out_analysis,
                                        FengSemanticError **out_errors,
                                        size_t *out_error_count);

void feng_semantic_analysis_free(FengSemanticAnalysis *analysis);
void feng_semantic_errors_free(FengSemanticError *errors, size_t error_count);
void feng_semantic_infos_free(FengSemanticInfo *infos, size_t info_count);

/* Return whether `target_type` has the compile-time-only authorization to
 * access `member` as a seal mixable static method of `source_type`. The
 * query is provider-neutral: all three mix forms use their normalized
 * resolved_source_decl and imported declarations use the same AST facts. */
bool feng_semantic_type_has_mixable_seal_access(
    const FengDecl *target_type,
    const FengDecl *source_type,
    const FengTypeMember *member);

/* Return whether one source-backed member is visible through its normalized
 * @friend authorization from the enclosing type/fit method. This query is
 * read-only and compile-time-only; it exists so tooling can reuse the same
 * generic substitution and fit/package checks as semantic member lookup. */
bool feng_semantic_member_has_friend_access(
    const FengSemanticAnalysis *analysis,
    const FengProgram *program,
    const FengDecl *access_owner_decl,
    const FengTypeRef *owner_instance_type_ref,
    const FengTypeMember *member,
    const FengDecl *enclosing_decl,
    const FengTypeMember *enclosing_member);

/* Returns true if `name` is a builtin type name (standard name such as
 * i8..i64, u8..u64, f32, f64, bool, string, void).  After AST alias
 * normalization (docs/engineering/feng-scalar-alias-optimize.md §6), alias names are no
 * longer recognized because they have already been replaced in the AST. */
bool feng_semantic_is_builtin_type_name(FengSlice name);

/* Returns the host machine's pointer size in bytes (sizeof(void *)).
 * Used by the CLI layer to fill FengSemanticAnalyzeOptions.pointer_size. */
size_t feng_get_host_pointer_size(void);

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

bool feng_semantic_record_enum_item_info(const FengSemanticAnalysis *analysis,
                                         const FengDecl *enum_decl,
                                         const FengEnumItem *item,
                                         size_t ordinal,
                                         int64_t value);

const FengSemanticEnumInfo *feng_semantic_lookup_enum_info(const FengSemanticAnalysis *analysis,
                                                           const FengDecl *enum_decl);

const FengSemanticEnumItemInfo *feng_semantic_find_enum_item_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *enum_decl,
    FengSlice item_name);

/* Internal post-pass entry — populates analysis->type_markers. Idempotent.
 * Implemented in cyclic.c; declared here so analyzer.c can call it on the
 * success path of feng_semantic_analyze_with_options. */
bool feng_semantic_compute_type_cyclicity(FengSemanticAnalysis *analysis);

/* Internal post-pass entry — detects value-type cycles (docs/engineering/feng-value-
 * type-dev.md §3.5, §9.2). Value types (tuples and `@value type` decls)
 * must have a finite size; a value type that directly or indirectly
 * contains itself as a field is rejected at compile time (AE1327).
 * Ordinary (non-value) type decls are heap objects referenced through a
 * managed pointer and are not subject to this check.
 * Appends one error per cyclic decl to the shared errors buffer. Idempotent.
 * Implemented in value_type_cycles.c. Called on the success path of
 * feng_semantic_analyze_with_options, before the `finish:` label. */
bool feng_semantic_detect_value_type_cycles(FengSemanticAnalysis *analysis,
                                            FengSemanticError **errors,
                                            size_t *error_count,
                                            size_t *error_capacity);

/* Internal post-pass entry — populates analysis->spec_relations with one
 * entry per (type_decl, spec_decl) pair that has at least one source in the
 * analysis. Idempotent. Implemented in spec_relations.c. Called on the
 * success path of feng_semantic_analyze_with_options. */
bool feng_semantic_compute_spec_relations(FengSemanticAnalysis *analysis);

/* Look up the relation entry for (subject_key, spec_decl). Returns NULL if no
 * source exists anywhere in the analysis. The returned pointer is stable
 * until feng_semantic_analysis_free. */
const FengSpecRelation *feng_semantic_lookup_spec_relation(
    const FengSemanticAnalysis *analysis,
    const FengSemanticSubjectKey *subject_key,
    const FengDecl *spec_decl);

/* Returns true iff `source` is visible from a consumer file located in
 * `consumer_module` whose `use` list resolves to the modules in
 * `consumer_imports[0..consumer_import_count)`. DECLARED_* sources are
 * always visible. FIT_* sources are visible iff the fit's provider module
 * is the consumer module itself, or the fit is `open` and the consumer
 * imported the provider module. Mirrors docs/specifications/feng-fit.md §4. */
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
    const FengSemanticSubjectKey *src_subject_key,
    const FengDecl *target_spec_decl,
    const FengTypeRef *target_spec_type_ref,
    const FengSpecRelation *relation,
    FengSpecObjectSubjectStorageKind object_subject_storage);

/* Record an object-form child-spec -> parent-spec projection. Each entry in
 * `parent_indices` is the selected direct parent's declaration index at one
 * edge, in source-to-target order. */
bool feng_semantic_record_object_spec_upcast_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *target_spec_decl,
    const FengTypeRef *target_spec_type_ref,
    const size_t *parent_indices,
    size_t parent_index_count);

/* Record an intersection-form coercion site (`expr` of concrete type
 * `src_type_decl` flowing into a slot typed as intersection-form spec
 * `target_spec_decl`). Differs from FORM_OBJECT in that there is no single
 * SpecRelation: the subject satisfies every member spec (already verified
 * by 9.5's per-member satisfaction check), so codegen derives the merged
 * witness from the subject's per-member witnesses. All non-NULL pointers
 * must be valid; recording replaces any prior entry for the same `expr`.
 *
 * Implemented in spec_coercion_sites.c. */
bool feng_semantic_record_intersection_spec_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengSemanticSubjectKey *src_subject_key,
    const FengDecl *target_spec_decl,
    const FengTypeRef *target_spec_type_ref,
    FengSpecObjectSubjectStorageKind object_subject_storage);

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
    const FengTypeRef *target_spec_type_ref,
    FengSpecCoercionCallableSource callable_source,
    const FengDecl *callable_decl,
    const FengTypeMember *callable_member,
    const FengDecl *callable_owner_type_decl,
    const FengDecl *callable_fit_decl,
    const FengTypeRef *callable_receiver_type_ref,
    const FengTypeRef *const *callable_type_args,
    size_t callable_type_arg_count,
    const FengExpr *callable_lambda_expr);

/* Record an ABI function-pointer address site. This is the `&top_level_fn`
 * counterpart to callable-form coercion sites: semantic resolves the exact
 * callable-form `@abi spec` target and the referenced top-level function,
 * and codegen later lowers the site as a plain C function pointer rather
 * than as a closure value.
 *
 * Implemented in spec_coercion_sites.c. */
bool feng_semantic_record_abi_function_pointer_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *target_spec_decl,
    const FengTypeRef *target_spec_type_ref,
    const FengDecl *callable_decl);

/* Look up the recorded coercion site for `expr`. Returns NULL when no site
 * was recorded (either the expression is not a coercion site, or the
 * resolver did not visit it as one). The returned pointer is stable until
 * feng_semantic_analysis_free. */
const FengSpecCoercionSite *feng_semantic_lookup_spec_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr);

/* --- Union-form spec metadata ---------------------------------------- */

/* Takes ownership of `members` and each member's `type_ref`. Re-recording the
 * same `spec_decl` replaces the previous member list. */
bool feng_semantic_record_union_spec_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *spec_decl,
    FengUnionSpecMemberInfo *members,
    size_t member_count);

const FengUnionSpecInfo *feng_semantic_lookup_union_spec_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *spec_decl);

bool feng_semantic_record_union_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr,
    const FengDecl *target_union_decl,
    const FengTypeRef *target_union_type_ref,
    size_t member_index,
    const FengTypeRef *member_type_ref,
    const size_t *path_indices,
    size_t path_length);

const FengUnionCoercionSite *feng_semantic_lookup_union_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr);

void feng_semantic_free_union_spec_infos(FengSemanticAnalysis *analysis);

/* --- Intersection-form spec metadata ---------------------------------- */

/* Takes ownership of `flattened_members`. Re-recording the same `spec_decl`
 * replaces the previous member list. */
bool feng_semantic_record_intersection_spec_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *spec_decl,
    const FengDecl **flattened_members,
    size_t flattened_member_count);

const FengIntersectionSpecInfo *feng_semantic_lookup_intersection_spec_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *spec_decl);

void feng_semantic_free_intersection_spec_infos(FengSemanticAnalysis *analysis);

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

/* One witness entry per (subject_key, spec_decl) pair that has been demanded
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
    FengSemanticSubjectKey subject_key;
    const FengDecl *spec_decl;
    FengSpecWitnessMember *members;
    size_t member_count;
    size_t member_capacity;
} FengSpecWitness;

FengSemanticSubjectKey feng_semantic_subject_key_for_type_decl(
    const FengDecl *type_decl);
FengSemanticSubjectKey feng_semantic_subject_key_for_builtin(
    const char *builtin_canonical_name);
bool feng_semantic_subject_key_init_array_from_type_ref(
    FengSemanticSubjectKey *out_key,
    const FengTypeRef *type_ref);

/* Look up the witness entry for (subject_key, spec_decl). Returns NULL when
 * no coercion has yet demanded this (T, S) pair (per §8.2). */
const FengSpecWitness *feng_semantic_lookup_spec_witness(
    const FengSemanticAnalysis *analysis,
    const FengSemanticSubjectKey *subject_key,
    const FengDecl *spec_decl);

/* Reserve and return a fresh witness entry for (subject_key, spec_decl). The
 * caller is expected to populate `members` via
 * feng_semantic_spec_witness_append_member after reservation. Returns NULL
 * on allocation failure or when an entry already exists (callers should
 * check feng_semantic_lookup_spec_witness first). */
FengSpecWitness *feng_semantic_reserve_spec_witness(
    const FengSemanticAnalysis *analysis,
    const FengSemanticSubjectKey *subject_key,
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

/* --- Value-kind classification (docs/engineering/feng-value-model-delivered.md §6.1) - */

/* Runtime classification of a Feng value, mirroring runtime
 * FengValueKind in src/runtime/feng_runtime.h. Per
 * docs/engineering/feng-value-model-delivered.md §2 / §6.1 every Feng type belongs to
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
 *     docs/engineering/feng-spec-codegen-delivered.md §4).
 *   - FENG_DECL_SPEC, callable form → MANAGED_POINTER (closure pointer;
 *     callable specs are not fat values, see §8.4).
 *
 * Implemented in value_kind.c. The function does not consult the
 * analysis; the decl carries enough information on its own. */
FengSemanticValueKind feng_semantic_value_kind_of_decl(const FengDecl *decl);

/* --- ReifiableDepSet (§2.2.1 具体化依赖收集) ----------------------------- */

/* 获取或创建 owner_decl 的声明级具体化依赖集。 */
FengReifiableDepSet *feng_semantic_get_or_create_reifiable_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl);

/* 获取或创建 owner_decl 中 owner_member 的 callable 具体化依赖集。 */
FengReifiableDepSet *feng_semantic_get_or_create_member_reifiable_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl,
    const FengTypeMember *owner_member);

/* 向依赖集追加一条具体化依赖。相同 type_ref 不重复追加。 */
bool feng_semantic_reifiable_dep_set_append(
    FengReifiableDepSet *dep_set,
    FengReifiableDepKind kind,
    const FengTypeRef *type_ref);

/* Append one resolved direct generic callable dependency. */
bool feng_semantic_reifiable_dep_set_append_callable(
    FengReifiableDepSet *dep_set,
    const FengResolvedCallable *resolved);

/* Append one target-typed callable-value formation dependency. */
bool feng_semantic_reifiable_dep_set_append_callable_value(
    FengReifiableDepSet *dep_set,
    const FengResolvedCallable *resolved,
    const FengTypeRef *target_callable_type_ref);

/* 查找 owner_decl 的声明级具体化依赖集，不存在时返回 NULL。 */
const FengReifiableDepSet *feng_semantic_lookup_reifiable_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl);

/* 查找 owner_decl 中 owner_member 的 callable 具体化依赖集。 */
const FengReifiableDepSet *feng_semantic_lookup_member_reifiable_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl,
    const FengTypeMember *owner_member);

/* Record/lookup a stable imported FT identity for one synthesized AST node. */
bool feng_semantic_record_imported_symbol_identity(
    FengSemanticAnalysis *analysis,
    const void *source_node,
    const void *symbol_decl,
    const char *module_name,
    uint32_t symbol_id);
const FengImportedSymbolIdentity *
feng_semantic_lookup_imported_symbol_identity(
    const FengSemanticAnalysis *analysis,
    const void *source_node);

/* Post-pass：遍历所有本地模块中的泛型声明与 callable，收集待具体化
 * 依赖到 analysis->reifiable_dep_sets 侧表。
 * 在 fixpoint 循环完成后、type cyclicity 计算前调用。 */
bool feng_semantic_collect_reifiable_deps(FengSemanticAnalysis *analysis);

#ifdef __cplusplus
}
#endif

#endif
