#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static bool rd_type_ref_equals(const FengTypeRef *left,
                               const FengTypeRef *right);
static bool type_ref_contains_type_param(const FengTypeRef *type_ref,
                                         const FengTypeParam *type_params,
                                         size_t type_param_count);
static bool extract_fit_target_implicit_type_param(
    const FengTypeRef *target_ref,
    FengTypeParam *out_param);

/* ===================================================================
 * ReifiableDepSet 基础 API（get_or_create / append / lookup）
 * =================================================================== */

FengReifiableDepSet *feng_semantic_get_or_create_member_reifiable_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl,
    const FengTypeMember *owner_member) {
    size_t index;
    FengReifiableDepSet *slot;

    if (analysis == NULL || owner_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->reifiable_dep_set_count; ++index) {
        if (analysis->reifiable_dep_sets[index].owner_decl == owner_decl &&
            analysis->reifiable_dep_sets[index].owner_member == owner_member) {
            return &analysis->reifiable_dep_sets[index];
        }
    }

    if (analysis->reifiable_dep_set_count == analysis->reifiable_dep_set_capacity) {
        size_t new_capacity = analysis->reifiable_dep_set_capacity == 0U
                                  ? 8U
                                  : analysis->reifiable_dep_set_capacity * 2U;
        FengReifiableDepSet *grown = (FengReifiableDepSet *)realloc(
            analysis->reifiable_dep_sets,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        analysis->reifiable_dep_sets = grown;
        analysis->reifiable_dep_set_capacity = new_capacity;
    }

    slot = &analysis->reifiable_dep_sets[analysis->reifiable_dep_set_count++];
    memset(slot, 0, sizeof(*slot));
    slot->owner_decl = owner_decl;
    slot->owner_member = owner_member;
    return slot;
}

FengReifiableDepSet *feng_semantic_get_or_create_reifiable_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl) {
    return feng_semantic_get_or_create_member_reifiable_dep_set(
        analysis, owner_decl, NULL);
}

bool feng_semantic_reifiable_dep_set_append(
    FengReifiableDepSet *dep_set,
    FengReifiableDepKind kind,
    const FengTypeRef *type_ref) {
    size_t index;
    FengReifiableDep *slot;

    if (dep_set == NULL || type_ref == NULL) {
        return false;
    }

    /* 去重：按完整类型表达式身份合并，不能依赖 AST 指针。 */
    for (index = 0U; index < dep_set->dep_count; ++index) {
        if (rd_type_ref_equals(dep_set->deps[index].type_ref, type_ref) &&
            dep_set->deps[index].kind == kind) {
            return true;
        }
    }

    if (dep_set->dep_count == dep_set->dep_capacity) {
        size_t new_capacity = dep_set->dep_capacity == 0U
                                  ? 8U
                                  : dep_set->dep_capacity * 2U;
        FengReifiableDep *grown = (FengReifiableDep *)realloc(
            dep_set->deps,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        dep_set->deps = grown;
        dep_set->dep_capacity = new_capacity;
    }

    slot = &dep_set->deps[dep_set->dep_count++];
    slot->kind = kind;
    slot->type_ref = type_ref;
    return true;
}

const FengReifiableDepSet *feng_semantic_lookup_member_reifiable_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl,
    const FengTypeMember *owner_member) {
    size_t index;

    if (analysis == NULL || owner_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->reifiable_dep_set_count; ++index) {
        if (analysis->reifiable_dep_sets[index].owner_decl == owner_decl &&
            analysis->reifiable_dep_sets[index].owner_member == owner_member) {
            return &analysis->reifiable_dep_sets[index];
        }
    }

    return NULL;
}

const FengReifiableDepSet *feng_semantic_lookup_reifiable_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl) {
    return feng_semantic_lookup_member_reifiable_dep_set(
        analysis, owner_decl, NULL);
}

bool feng_semantic_record_imported_symbol_identity(
    FengSemanticAnalysis *analysis,
    const void *source_node,
    const void *symbol_decl,
    const char *module_name,
    uint32_t symbol_id) {
    FengImportedSymbolIdentity *grown;
    FengImportedSymbolIdentity *slot;

    if (analysis == NULL || source_node == NULL || symbol_decl == NULL ||
        module_name == NULL ||
        symbol_id == 0U) {
        return false;
    }
    for (size_t index = 0U;
         index < analysis->imported_symbol_identity_count;
         ++index) {
        if (analysis->imported_symbol_identities[index].source_node ==
            source_node) {
            return strcmp(
                       analysis->imported_symbol_identities[index].module_name,
                       module_name) == 0 &&
                   analysis->imported_symbol_identities[index].symbol_decl ==
                       symbol_decl &&
                   analysis->imported_symbol_identities[index].symbol_id ==
                       symbol_id;
        }
    }
    if (analysis->imported_symbol_identity_count ==
        analysis->imported_symbol_identity_capacity) {
        size_t new_capacity =
            analysis->imported_symbol_identity_capacity == 0U
                ? 16U
                : analysis->imported_symbol_identity_capacity * 2U;

        grown = (FengImportedSymbolIdentity *)realloc(
            analysis->imported_symbol_identities,
            new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        analysis->imported_symbol_identities = grown;
        analysis->imported_symbol_identity_capacity = new_capacity;
    }
    slot = &analysis->imported_symbol_identities[
        analysis->imported_symbol_identity_count++];
    memset(slot, 0, sizeof(*slot));
    slot->module_name = strdup(module_name);
    if (slot->module_name == NULL) {
        analysis->imported_symbol_identity_count--;
        return false;
    }
    slot->source_node = source_node;
    slot->symbol_decl = symbol_decl;
    slot->symbol_id = symbol_id;
    return true;
}

const FengImportedSymbolIdentity *
feng_semantic_lookup_imported_symbol_identity(
    const FengSemanticAnalysis *analysis,
    const void *source_node) {
    if (analysis == NULL || source_node == NULL) {
        return NULL;
    }
    for (size_t index = 0U;
         index < analysis->imported_symbol_identity_count;
         ++index) {
        if (analysis->imported_symbol_identities[index].source_node ==
            source_node) {
            return &analysis->imported_symbol_identities[index];
        }
    }
    return NULL;
}

/* ===================================================================
 * Post-pass: 收集待具体化依赖
 * =================================================================== */

/* 收集上下文：携带 analysis、owner dep_set、当前声明的 type_params。 */
typedef struct CollectContext {
    FengSemanticAnalysis *analysis;
    FengReifiableDepSet *dep_set;
    const FengTypeParam *type_params;
    size_t type_param_count;
} CollectContext;

/* ---- 基础工具 --------------------------------------------------------- */

static bool rd_slice_equals(FengSlice a, FengSlice b) {
    return a.length == b.length &&
           (a.length == 0U || memcmp(a.data, b.data, a.length) == 0);
}

static bool rd_path_equals(const FengSlice *a, size_t an,
                           const FengSlice *b, size_t bn) {
    size_t i;
    if (an != bn) {
        return false;
    }
    for (i = 0U; i < an; ++i) {
        if (!rd_slice_equals(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

/* 比较两个 caller-view 类型表达式。依赖身份必须按完整结构去重，不能依赖
 * AST 指针身份，否则显式/推断或重复出现的等价类型会得到不同 slot。 */
static bool rd_type_ref_equals(const FengTypeRef *left,
                               const FengTypeRef *right) {
    size_t index;

    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
        case FENG_TYPE_REF_NAMED:
            if (!rd_path_equals(left->as.named.segments,
                                left->as.named.segment_count,
                                right->as.named.segments,
                                right->as.named.segment_count) ||
                left->as.named.type_arg_count !=
                    right->as.named.type_arg_count) {
                return false;
            }
            for (index = 0U;
                 index < left->as.named.type_arg_count;
                 ++index) {
                if (!rd_type_ref_equals(
                        left->as.named.type_args[index],
                        right->as.named.type_args[index])) {
                    return false;
                }
            }
            return true;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            return left->array_element_writable ==
                       right->array_element_writable &&
                   rd_type_ref_equals(left->as.inner, right->as.inner);
    }
    return false;
}

/* 判断已解析 callable 是否使用共享 ABI。构造器继续使用 type descriptor
 * 路径，不属于 callable descriptor graph。 */
static bool rd_resolved_callable_uses_shared_abi(
    const FengResolvedCallable *resolved) {
    if (resolved == NULL) {
        return false;
    }
    if (resolved->kind == FENG_RESOLVED_CALLABLE_FUNCTION) {
        return resolved->function_decl != NULL &&
               resolved->function_decl->kind == FENG_DECL_FUNCTION &&
               !resolved->function_decl->is_extern &&
               resolved->function_decl->as.function_decl.type_param_count > 0U;
    }
    if (resolved->kind == FENG_RESOLVED_CALLABLE_TYPE_METHOD ||
        resolved->kind == FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD ||
        resolved->kind == FENG_RESOLVED_CALLABLE_FIT_METHOD ||
        resolved->kind == FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD) {
        FengTypeParam implicit_fit_param;

        return resolved->member != NULL &&
               (resolved->member->as.callable.type_param_count > 0U ||
                (resolved->owner_type_decl != NULL &&
                 resolved->owner_type_decl->kind == FENG_DECL_TYPE &&
                 resolved->owner_type_decl->as.type_decl.type_param_count >
                     0U) ||
                ((resolved->kind == FENG_RESOLVED_CALLABLE_FIT_METHOD ||
                  resolved->kind ==
                      FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD) &&
                 resolved->fit_decl != NULL &&
                 resolved->fit_decl->kind == FENG_DECL_FIT &&
                 extract_fit_target_implicit_type_param(
                     resolved->fit_decl->as.fit_decl.target,
                     &implicit_fit_param)));
    }
    return false;
}

/* 将 direct generic callee 追加到当前 callable dependency set。 */
static bool rd_append_callable_dep(FengReifiableDepSet *dep_set,
                                   const FengResolvedCallable *resolved) {
    FengReifiableCallableDep *slot;
    size_t index;

    if (dep_set == NULL ||
        !rd_resolved_callable_uses_shared_abi(resolved)) {
        return true;
    }
    for (index = 0U; index < dep_set->callable_dep_count; ++index) {
        const FengReifiableCallableDep *existing =
            &dep_set->callable_deps[index];
        size_t arg_index;

        if (existing->purpose != FENG_REIFIABLE_CALLABLE_DEP_DIRECT_CALL ||
            existing->kind != resolved->kind ||
            existing->function_decl != resolved->function_decl ||
            existing->owner_type_decl != resolved->owner_type_decl ||
            existing->member != resolved->member ||
            existing->fit_decl != resolved->fit_decl ||
            !rd_type_ref_equals(existing->owner_instance_type_ref,
                                resolved->owner_instance_type_ref) ||
            existing->callable_type_arg_count !=
                resolved->callable_type_arg_count) {
            continue;
        }
        for (arg_index = 0U;
             arg_index < existing->callable_type_arg_count;
             ++arg_index) {
            if (!rd_type_ref_equals(existing->callable_type_args[arg_index],
                                    resolved->callable_type_args[arg_index])) {
                break;
            }
        }
        if (arg_index == existing->callable_type_arg_count) {
            return true;
        }
    }
    if (dep_set->callable_dep_count == dep_set->callable_dep_capacity) {
        size_t new_capacity = dep_set->callable_dep_capacity == 0U
                                  ? 4U
                                  : dep_set->callable_dep_capacity * 2U;
        FengReifiableCallableDep *grown =
            (FengReifiableCallableDep *)realloc(
                dep_set->callable_deps,
                new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        dep_set->callable_deps = grown;
        dep_set->callable_dep_capacity = new_capacity;
    }
    slot = &dep_set->callable_deps[dep_set->callable_dep_count++];
    memset(slot, 0, sizeof(*slot));
    slot->purpose = FENG_REIFIABLE_CALLABLE_DEP_DIRECT_CALL;
    slot->kind = resolved->kind;
    slot->function_decl = resolved->function_decl;
    slot->owner_type_decl = resolved->owner_type_decl;
    slot->member = resolved->member;
    slot->fit_decl = resolved->fit_decl;
    slot->owner_instance_type_ref = resolved->owner_instance_type_ref;
    slot->callable_type_args = resolved->callable_type_args;
    slot->callable_type_arg_count = resolved->callable_type_arg_count;
    return true;
}

/* Append one target-typed callable-value formation dependency. Unlike a direct
 * call this dependency is always materialized: its closed descriptor combines
 * the selected implementation, target callable ABI, and (for bound methods)
 * receiver representation. */
static bool rd_append_callable_value_dep_resolved(
    FengReifiableDepSet *dep_set,
    const FengResolvedCallable *resolved,
    const FengTypeRef *target_callable_type_ref) {
    FengReifiableCallableDep *slot;
    size_t index;
    bool is_function;
    bool is_method;

    if (dep_set == NULL || resolved == NULL ||
        target_callable_type_ref == NULL) {
        return true;
    }
    is_function = resolved->kind == FENG_RESOLVED_CALLABLE_FUNCTION &&
                  resolved->function_decl != NULL &&
                  resolved->function_decl->kind == FENG_DECL_FUNCTION;
    is_method = resolved->member != NULL &&
                resolved->owner_type_decl != NULL &&
                resolved->owner_instance_type_ref != NULL &&
                (((resolved->kind == FENG_RESOLVED_CALLABLE_TYPE_METHOD ||
                   resolved->kind == FENG_RESOLVED_CALLABLE_FIT_METHOD) &&
                  resolved->owner_type_decl->kind == FENG_DECL_TYPE) ||
                 (resolved->kind == FENG_RESOLVED_CALLABLE_SPEC_METHOD &&
                  resolved->owner_type_decl->kind == FENG_DECL_SPEC));
    if (!is_function && !is_method) {
        return true;
    }
    for (index = 0U; index < dep_set->callable_dep_count; ++index) {
        const FengReifiableCallableDep *existing =
            &dep_set->callable_deps[index];
        size_t arg_index;

        if (existing->purpose == FENG_REIFIABLE_CALLABLE_DEP_CALLABLE_VALUE &&
            existing->kind == resolved->kind &&
            existing->function_decl == resolved->function_decl &&
            existing->owner_type_decl ==
                resolved->owner_type_decl &&
            existing->member == resolved->member &&
            existing->fit_decl == resolved->fit_decl &&
            rd_type_ref_equals(existing->owner_instance_type_ref,
                               resolved->owner_instance_type_ref) &&
            rd_type_ref_equals(existing->target_callable_type_ref,
                               target_callable_type_ref) &&
            existing->callable_type_arg_count ==
                resolved->callable_type_arg_count) {
            for (arg_index = 0U;
                 arg_index < existing->callable_type_arg_count;
                 ++arg_index) {
                if (!rd_type_ref_equals(
                        existing->callable_type_args[arg_index],
                        resolved->callable_type_args[arg_index])) {
                    break;
                }
            }
            if (arg_index != existing->callable_type_arg_count) {
                continue;
            }
            return true;
        }
    }
    if (dep_set->callable_dep_count == dep_set->callable_dep_capacity) {
        size_t new_capacity = dep_set->callable_dep_capacity == 0U
                                  ? 4U
                                  : dep_set->callable_dep_capacity * 2U;
        FengReifiableCallableDep *grown =
            (FengReifiableCallableDep *)realloc(
                dep_set->callable_deps,
                new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        dep_set->callable_deps = grown;
        dep_set->callable_dep_capacity = new_capacity;
    }
    slot = &dep_set->callable_deps[dep_set->callable_dep_count++];
    memset(slot, 0, sizeof(*slot));
    slot->purpose = FENG_REIFIABLE_CALLABLE_DEP_CALLABLE_VALUE;
    slot->kind = resolved->kind;
    slot->function_decl = resolved->function_decl;
    slot->owner_type_decl = resolved->owner_type_decl;
    slot->member = resolved->member;
    slot->fit_decl = resolved->fit_decl;
    slot->owner_instance_type_ref = resolved->owner_instance_type_ref;
    slot->callable_type_args = resolved->callable_type_args;
    slot->callable_type_arg_count = resolved->callable_type_arg_count;
    slot->target_callable_type_ref = target_callable_type_ref;
    return true;
}

/* Append a coercion site's callable-value dependency when any part of the
 * closed callable identity still depends on the active shared-body params. */
static bool rd_append_callable_value_dep(
    FengReifiableDepSet *dep_set,
    const FengSpecCoercionSite *site,
    const FengTypeParam *type_params,
    size_t type_param_count) {
    FengResolvedCallable resolved;

    if (site == NULL ||
        site->form != FENG_SPEC_COERCION_FORM_CALLABLE ||
        (site->callable_source !=
             FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN &&
         site->callable_source !=
             FENG_SPEC_COERCION_CALLABLE_SOURCE_METHOD_VALUE)) {
        return true;
    }
    if (!type_ref_contains_type_param(site->callable_receiver_type_ref,
                                      type_params,
                                      type_param_count) &&
        !type_ref_contains_type_param(site->target_spec_type_ref,
                                      type_params,
                                      type_param_count)) {
        size_t arg_index;

        for (arg_index = 0U;
             arg_index < site->callable_type_arg_count;
             ++arg_index) {
            if (type_ref_contains_type_param(site->callable_type_args[arg_index],
                                             type_params,
                                             type_param_count)) {
                break;
            }
        }
        if (arg_index == site->callable_type_arg_count) {
            return true;
        }
    }
    memset(&resolved, 0, sizeof(resolved));
    if (site->callable_source ==
        FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN) {
        resolved.kind = FENG_RESOLVED_CALLABLE_FUNCTION;
        resolved.function_decl = site->callable_decl;
    } else {
        resolved.kind = site->callable_owner_type_decl != NULL &&
                                site->callable_owner_type_decl->kind ==
                                    FENG_DECL_SPEC
                            ? FENG_RESOLVED_CALLABLE_SPEC_METHOD
                            : (site->callable_fit_decl != NULL
                                   ? FENG_RESOLVED_CALLABLE_FIT_METHOD
                                   : FENG_RESOLVED_CALLABLE_TYPE_METHOD);
        resolved.owner_type_decl = site->callable_owner_type_decl;
        resolved.member = site->callable_member;
        resolved.fit_decl = site->callable_fit_decl;
        resolved.owner_instance_type_ref =
            site->callable_receiver_type_ref;
    }
    resolved.callable_type_args =
        (const FengTypeRef **)site->callable_type_args;
    resolved.callable_type_arg_count = site->callable_type_arg_count;
    return rd_append_callable_value_dep_resolved(
        dep_set, &resolved, site->target_spec_type_ref);
}

bool feng_semantic_reifiable_dep_set_append_callable(
    FengReifiableDepSet *dep_set,
    const FengResolvedCallable *resolved) {
    return rd_append_callable_dep(dep_set, resolved);
}

bool feng_semantic_reifiable_dep_set_append_callable_value(
    FengReifiableDepSet *dep_set,
    const FengResolvedCallable *resolved,
    const FengTypeRef *target_callable_type_ref) {
    return rd_append_callable_value_dep_resolved(
        dep_set, resolved, target_callable_type_ref);
}

/* ---- type_ref 中是否含有对 type_params 的引用 ------------------------- */

/* 递归检查 type_ref 树中是否至少存在一个对 type_params 的引用。
 * 判定一个叶子节点为 type_param 引用的条件：
 *   kind == NAMED && segment_count == 1 && type_arg_count == 0
 *   && name 匹配某个 type_param。 */
static bool type_ref_contains_type_param(const FengTypeRef *type_ref,
                                         const FengTypeParam *type_params,
                                         size_t type_param_count) {
    size_t i;

    if (type_ref == NULL || type_params == NULL || type_param_count == 0U) {
        return false;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            /* 叶子：单 segment、无 type_args → 可能是 type_param 引用。 */
            if (type_ref->as.named.segment_count == 1U &&
                type_ref->as.named.type_arg_count == 0U) {
                for (i = 0U; i < type_param_count; ++i) {
                    if (rd_slice_equals(type_ref->as.named.segments[0],
                                        type_params[i].name)) {
                        return true;
                    }
                }
                return false;
            }
            /* 非叶子：递归检查各 type_arg。 */
            for (i = 0U; i < type_ref->as.named.type_arg_count; ++i) {
                if (type_ref_contains_type_param(type_ref->as.named.type_args[i],
                                                 type_params,
                                                 type_param_count)) {
                    return true;
                }
            }
            return false;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            return type_ref_contains_type_param(type_ref->as.inner,
                                                type_params,
                                                type_param_count);
    }

    return false;
}

/* ---- 在 analysis 中查找 named type_ref 对应的 FengDecl ----------------- */

static const FengSemanticModule *rd_find_module_by_segments(
    const FengSemanticAnalysis *analysis,
    const FengSlice *segments,
    size_t segment_count) {
    size_t mi;
    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        if (rd_path_equals(mod->segments, mod->segment_count,
                           segments, segment_count)) {
            return mod;
        }
    }
    return NULL;
}

/* 获取声明的类型名。仅处理 FENG_DECL_TYPE 和 FENG_DECL_SPEC。 */
static FengSlice rd_decl_type_name(const FengDecl *decl) {
    FengSlice empty;
    memset(&empty, 0, sizeof(empty));
    if (decl == NULL) {
        return empty;
    }
    switch (decl->kind) {
        case FENG_DECL_TYPE:
            return decl->as.type_decl.name;
        case FENG_DECL_SPEC:
            return decl->as.spec_decl.name;
        default:
            return empty;
    }
}

/* Return the generic arity participating in type/spec declaration identity. */
static size_t rd_decl_type_arity(const FengDecl *decl) {
    if (decl == NULL) {
        return 0U;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        return decl->as.type_decl.type_param_count;
    }
    if (decl->kind == FENG_DECL_SPEC) {
        return decl->as.spec_decl.type_param_count;
    }
    return 0U;
}

/* 在 analysis 所有模块中按名称查找 FENG_DECL_TYPE 或 FENG_DECL_SPEC。
 * 不做可见性过滤——语义分析已通过，此处仅用于确定 dep kind。 */
static const FengDecl *find_type_decl_by_named_ref(
    const FengSemanticAnalysis *analysis,
    const FengTypeRef *type_ref) {
    FengSlice name;
    size_t mi, pi, di;

    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count == 0U) {
        return NULL;
    }

    /* 最后一个 segment 是类型名。 */
    name = type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];

    if (type_ref->as.named.segment_count > 1U) {
        /* 多 segment：按模块路径定位。 */
        const FengSemanticModule *mod = rd_find_module_by_segments(
            analysis,
            type_ref->as.named.segments,
            type_ref->as.named.segment_count - 1U);
        if (mod == NULL) {
            return NULL;
        }
        for (pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if ((d->kind == FENG_DECL_TYPE || d->kind == FENG_DECL_SPEC) &&
                    rd_slice_equals(rd_decl_type_name(d), name) &&
                    rd_decl_type_arity(d) ==
                        type_ref->as.named.type_arg_count) {
                    return d;
                }
            }
        }
        return NULL;
    }

    /* 单 segment：在所有模块中查找。 */
    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        for (pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if ((d->kind == FENG_DECL_TYPE || d->kind == FENG_DECL_SPEC) &&
                    rd_slice_equals(rd_decl_type_name(d), name) &&
                    rd_decl_type_arity(d) ==
                        type_ref->as.named.type_arg_count) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

/* Locate the declaration domain that owns a resolved type member. This is
 * used for iterator-protocol calls synthesized after ordinary call
 * resolution, whose AST contains the selected member but no call node. */
static bool rd_find_member_owner(
    const FengSemanticAnalysis *analysis,
    const FengTypeMember *member,
    const FengDecl **out_owner_type_decl,
    const FengDecl **out_fit_decl) {
    size_t module_index;

    *out_owner_type_decl = NULL;
    *out_fit_decl = NULL;
    if (analysis == NULL || member == NULL) {
        return false;
    }
    for (module_index = 0U;
         module_index < analysis->module_count;
         ++module_index) {
        const FengSemanticModule *module =
            &analysis->modules[module_index];
        size_t program_index;

        for (program_index = 0U;
             program_index < module->program_count;
             ++program_index) {
            const FengProgram *program = module->programs[program_index];
            size_t decl_index;

            for (decl_index = 0U;
                 decl_index < program->declaration_count;
                 ++decl_index) {
                const FengDecl *decl = program->declarations[decl_index];
                FengTypeMember *const *members = NULL;
                size_t member_count = 0U;
                size_t member_index;

                if (decl->kind == FENG_DECL_TYPE) {
                    members = decl->as.type_decl.members;
                    member_count = decl->as.type_decl.member_count;
                } else if (decl->kind == FENG_DECL_FIT) {
                    members = decl->as.fit_decl.members;
                    member_count = decl->as.fit_decl.member_count;
                } else {
                    continue;
                }
                for (member_index = 0U;
                     member_index < member_count;
                     ++member_index) {
                    if (members[member_index] != member) {
                        continue;
                    }
                    if (decl->kind == FENG_DECL_TYPE) {
                        *out_owner_type_decl = decl;
                    } else {
                        *out_fit_decl = decl;
                        *out_owner_type_decl =
                            find_type_decl_by_named_ref(
                                analysis, decl->as.fit_decl.target);
                    }
                    return *out_owner_type_decl != NULL;
                }
            }
        }
    }
    return false;
}

/* Record one iterator-protocol call synthesized by for/in lowering using the
 * same resolved-call identity as an explicit instance method invocation. */
static void rd_collect_synthesized_method_call(
    CollectContext *ctx,
    const FengTypeMember *member,
    const FengTypeRef *owner_instance_type_ref) {
    FengResolvedCallable resolved;
    const FengDecl *owner_type_decl;
    const FengDecl *fit_decl;

    if (ctx == NULL || member == NULL ||
        !rd_find_member_owner(ctx->analysis,
                              member,
                              &owner_type_decl,
                              &fit_decl)) {
        return;
    }
    memset(&resolved, 0, sizeof(resolved));
    resolved.kind = fit_decl != NULL
        ? FENG_RESOLVED_CALLABLE_FIT_METHOD
        : FENG_RESOLVED_CALLABLE_TYPE_METHOD;
    resolved.owner_type_decl = owner_type_decl;
    resolved.member = member;
    resolved.fit_decl = fit_decl;
    resolved.owner_instance_type_ref = owner_instance_type_ref;
    (void)rd_append_callable_dep(ctx->dep_set, &resolved);
}

/* ---- 确定 dep kind ---------------------------------------------------- */

/* tuple type → AGGREGATE，non-tuple type → MANAGED。
 * 无法确定时返回 false（不应记录为依赖）。 */
static bool determine_dep_kind(const FengDecl *decl,
                               FengReifiableDepKind *out_kind) {
    if (decl == NULL) {
        return false;
    }
    switch (decl->kind) {
        case FENG_DECL_TYPE:
            if (decl->as.type_decl.is_tuple ||
                decl->as.type_decl.is_value) {
                *out_kind = FENG_REIFIABLE_DEP_KIND_AGGREGATE;
            } else {
                *out_kind = FENG_REIFIABLE_DEP_KIND_MANAGED;
            }
            return true;
        case FENG_DECL_SPEC:
            /* Every by-value spec form uses an aggregate descriptor as its
             * concrete lifecycle/default authority. Object/intersection
             * carriers are fixed-size fat values; a union may additionally
             * obtain its physical size from the closed descriptor. */
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT ||
                decl->as.spec_decl.form == FENG_SPEC_FORM_UNION ||
                decl->as.spec_decl.form == FENG_SPEC_FORM_INTERSECTION) {
                *out_kind = FENG_REIFIABLE_DEP_KIND_AGGREGATE;
                return true;
            }
            /* Callable-form spec values are managed closure pointers. A
             * shared body needs the concrete callable descriptor when it
             * default-initializes a generic callable instance. */
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                *out_kind = FENG_REIFIABLE_DEP_KIND_MANAGED;
                return true;
            }
            return false;
        default:
            return false;
    }
}

/* ---- 前向声明（供 GENERIC_TARGET 合成函数使用） ------------------------- */

static void try_collect_type_ref(CollectContext *ctx,
                                 const FengTypeRef *type_ref);

/* ---- GENERIC_TARGET 合成 FengTypeRef ---------------------------------- */

/* 计算表达式路径的 segment 数量（IDENTIFIER → 1，MEMBER 链 → n）。
 * 参照 codegen.c:cg_expr_path_segment_count 模式。 */
static size_t rd_expr_path_segment_count(const FengExpr *expr) {
    size_t object_count;

    if (expr == NULL) {
        return 0U;
    }
    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            return 1U;
        case FENG_EXPR_MEMBER:
            object_count = rd_expr_path_segment_count(expr->as.member.object);
            return object_count == 0U ? 0U : object_count + 1U;
        default:
            return 0U;
    }
}

/* 从表达式路径中收集 segments（按从左到右顺序）。
 * 参照 codegen.c:cg_expr_collect_path_segments 模式。 */
static bool rd_expr_collect_path_segments(const FengExpr *expr,
                                          FengSlice *segments,
                                          size_t segment_count,
                                          size_t *next_index) {
    if (expr == NULL || segments == NULL || next_index == NULL ||
        *next_index >= segment_count) {
        return false;
    }
    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            segments[(*next_index)++] = expr->as.identifier;
            return true;
        case FENG_EXPR_MEMBER:
            if (!rd_expr_collect_path_segments(expr->as.member.object,
                                               segments,
                                               segment_count,
                                               next_index) ||
                *next_index >= segment_count) {
                return false;
            }
            segments[(*next_index)++] = expr->as.member.member;
            return true;
        default:
            return false;
    }
}

/* 存储仅拥有根节点和 segments 的 GENERIC_TARGET 包装引用。其 type_args
 * 借用源码 AST，必须与 analysis 持有的完整类型树使用不同的析构规则。 */
static bool rd_store_synthesized_ref(FengSemanticAnalysis *analysis,
                                     FengTypeRef *ref) {
    if (analysis->reifiable_wrapper_type_ref_count ==
        analysis->reifiable_wrapper_type_ref_capacity) {
        size_t new_capacity = analysis->reifiable_wrapper_type_ref_capacity == 0U
                                  ? 8U
                                  : analysis->reifiable_wrapper_type_ref_capacity * 2U;
        FengTypeRef **grown = (FengTypeRef **)realloc(
            analysis->reifiable_wrapper_type_refs,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        analysis->reifiable_wrapper_type_refs = grown;
        analysis->reifiable_wrapper_type_ref_capacity = new_capacity;
    }
    analysis->reifiable_wrapper_type_refs[
        analysis->reifiable_wrapper_type_ref_count++] = ref;
    return true;
}

/* 从 GENERIC_TARGET 表达式合成等效的 FengTypeRef 并尝试收集。
 * AST 中 GENERIC_TARGET 将类型名放在表达式层（IDENTIFIER/MEMBER），
 * 类型参数放在 type_args[] 中，不存在整体的 FengTypeRef 节点。
 * 此函数合成一个堆分配的 FengTypeRef（kind=NAMED，segments 从表达式路径提取，
 * type_args 直接引用 GENERIC_TARGET 的 type_args），交由 try_collect_type_ref
 * 处理，并将合成节点存入 dep_set 的 synthesized_refs 中管理生命周期。 */
static void rd_try_collect_generic_target(CollectContext *ctx,
                                          const FengExpr *expr) {
    size_t segment_count;
    FengSlice *segments;
    size_t next_index;
    FengTypeRef *ref;

    if (expr == NULL || ctx == NULL || ctx->dep_set == NULL ||
        expr->kind != FENG_EXPR_GENERIC_TARGET ||
        expr->as.generic_target.type_arg_count == 0U) {
        return;
    }

    /* 提取路径 segments。 */
    segment_count = rd_expr_path_segment_count(expr->as.generic_target.target);
    if (segment_count == 0U) {
        return;
    }

    segments = (FengSlice *)calloc(segment_count, sizeof(*segments));
    if (segments == NULL) {
        return;
    }

    next_index = 0U;
    if (!rd_expr_collect_path_segments(expr->as.generic_target.target,
                                       segments, segment_count, &next_index) ||
        next_index != segment_count) {
        free(segments);
        return;
    }

    /* 合成 FengTypeRef。 */
    ref = (FengTypeRef *)calloc(1U, sizeof(*ref));
    if (ref == NULL) {
        free(segments);
        return;
    }

    ref->token = expr->token;
    ref->kind = FENG_TYPE_REF_NAMED;
    ref->as.named.segments = segments;
    ref->as.named.segment_count = segment_count;
    /* type_args 直接引用 GENERIC_TARGET 的 type_args（AST 节点，非拥有）。 */
    ref->as.named.type_args = expr->as.generic_target.type_args;
    ref->as.named.type_arg_count = expr->as.generic_target.type_arg_count;

    /* 存入 analysis 管理生命周期。 */
    if (!rd_store_synthesized_ref(ctx->analysis, ref)) {
        free(segments);
        free(ref);
        return;
    }

    /* 尝试收集合成的 type_ref（含 type_param 引用才会实际记录）。 */
    try_collect_type_ref(ctx, ref);
}

/* ---- 前向声明 ---------------------------------------------------------- */

static void collect_from_expr(CollectContext *ctx, const FengExpr *expr);
static void collect_from_block(CollectContext *ctx, const FengBlock *block);
static void collect_from_stmt(CollectContext *ctx, const FengStmt *stmt);

/* 收集 match 分支标签引用的类型。类型绑定与普通类型标签共用 labels，
 * `A -> B` 链式标签则需要逐层收集完整的 type_chain。 */
static void collect_from_match_branch_labels(CollectContext *ctx,
                                             const FengMatchBranch *branch) {
    size_t label_index;

    if (ctx == NULL || branch == NULL) {
        return;
    }
    for (label_index = 0U; label_index < branch->label_count; ++label_index) {
        const FengMatchLabel *label = &branch->labels[label_index];
        size_t chain_index;

        if (label->kind != FENG_MATCH_LABEL_TYPE) {
            continue;
        }
        if (label->type_chain_count > 0U) {
            for (chain_index = 0U;
                 chain_index < label->type_chain_count;
                 ++chain_index) {
                try_collect_type_ref(ctx, label->type_chain[chain_index]);
            }
        } else {
            try_collect_type_ref(ctx, label->type);
        }
    }
}

/* ---- 对单个 type_ref 尝试收集 ------------------------------------------ */

/* 递归扫描 type_ref 树。对每个 named type_ref（type_arg_count > 0 且含
 * type_param 引用），查找基础类型并追加为依赖。同时递归检查 type_args 中
 * 嵌套的泛型实例以及 ARRAY/POINTER 的 inner。 */
static void try_collect_type_ref(CollectContext *ctx,
                                 const FengTypeRef *type_ref) {
    size_t i;

    if (type_ref == NULL || ctx == NULL || ctx->dep_set == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            /* 当前 type_ref 是泛型实例（有 type_args）且含 type_param 引用？ */
            if (type_ref->as.named.type_arg_count > 0U &&
                type_ref_contains_type_param(type_ref,
                                             ctx->type_params,
                                             ctx->type_param_count)) {
                const FengDecl *base_decl =
                    find_type_decl_by_named_ref(ctx->analysis, type_ref);
                FengReifiableDepKind kind;

                if (determine_dep_kind(base_decl, &kind)) {
                    feng_semantic_reifiable_dep_set_append(
                        ctx->dep_set, kind, type_ref);
                }
            }
            /* 递归检查 type_args 中可能嵌套的泛型实例。 */
            for (i = 0U; i < type_ref->as.named.type_arg_count; ++i) {
                try_collect_type_ref(ctx, type_ref->as.named.type_args[i]);
            }
            break;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            try_collect_type_ref(ctx, type_ref->as.inner);
            break;
    }
}

/* ---- binding 收集 ------------------------------------------------------ */

static void collect_from_binding(CollectContext *ctx,
                                 const FengBinding *binding) {
    if (binding == NULL) {
        return;
    }
    try_collect_type_ref(ctx, binding->type);
    collect_from_expr(ctx, binding->initializer);
}

/* ---- 释放合成 type_ref 树 ----------------------------------------------- */

static void rd_free_synthesized_type_ref(FengTypeRef *type_ref) {
    size_t i;

    if (type_ref == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            for (i = 0U; i < type_ref->as.named.type_arg_count; ++i) {
                rd_free_synthesized_type_ref(type_ref->as.named.type_args[i]);
            }
            free(type_ref->as.named.segments);
            free(type_ref->as.named.type_args);
            break;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            rd_free_synthesized_type_ref(type_ref->as.inner);
            break;
    }

    free(type_ref);
}

/* ---- 递归克隆 type_ref 并将形参名替换为对应实参 ------------------------- */

/* 递归深拷贝 type_ref，将对 type_params[i].name 的叶节点引用替换为
 * type_args[i] 的完整克隆。等效于 analyzer.c 中同名函数的独立实现。 */
static FengTypeRef *rd_clone_type_ref_substituting(
    const FengTypeRef *type_ref,
    const FengTypeParam *type_params,
    size_t type_param_count,
    const FengTypeRef *const *type_args) {
    FengTypeRef *clone;
    size_t i;

    if (type_ref == NULL) {
        return NULL;
    }

    /* 叶子节点：单 segment、无 type_args → 可能是 type_param 引用。 */
    if (type_ref->kind == FENG_TYPE_REF_NAMED &&
        type_ref->as.named.segment_count == 1U &&
        type_ref->as.named.type_arg_count == 0U) {
        for (i = 0U; i < type_param_count; ++i) {
            if (rd_slice_equals(type_ref->as.named.segments[0],
                                type_params[i].name)) {
                /* 匹配形参——递归克隆对应的实参。 */
                return rd_clone_type_ref_substituting(
                    type_args[i], type_params, 0U, NULL);
            }
        }
    }

    clone = (FengTypeRef *)calloc(1U, sizeof(*clone));
    if (clone == NULL) {
        return NULL;
    }
    clone->token = type_ref->token;
    clone->kind = type_ref->kind;

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            clone->as.named.segment_count = type_ref->as.named.segment_count;
            clone->as.named.type_arg_count = type_ref->as.named.type_arg_count;
            if (type_ref->as.named.segment_count > 0U) {
                clone->as.named.segments =
                    (FengSlice *)malloc(sizeof(FengSlice) *
                                       type_ref->as.named.segment_count);
                if (clone->as.named.segments == NULL) {
                    free(clone);
                    return NULL;
                }
                memcpy(clone->as.named.segments,
                       type_ref->as.named.segments,
                       sizeof(FengSlice) * type_ref->as.named.segment_count);
            }
            if (type_ref->as.named.type_arg_count > 0U) {
                clone->as.named.type_args =
                    (FengTypeRef **)calloc(type_ref->as.named.type_arg_count,
                                          sizeof(FengTypeRef *));
                if (clone->as.named.type_args == NULL) {
                    rd_free_synthesized_type_ref(clone);
                    return NULL;
                }
                for (i = 0U; i < type_ref->as.named.type_arg_count; ++i) {
                    clone->as.named.type_args[i] =
                        rd_clone_type_ref_substituting(
                            type_ref->as.named.type_args[i],
                            type_params,
                            type_param_count,
                            type_args);
                    if (clone->as.named.type_args[i] == NULL) {
                        rd_free_synthesized_type_ref(clone);
                        return NULL;
                    }
                }
            }
            return clone;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            clone->array_element_writable = type_ref->array_element_writable;
            clone->as.inner = rd_clone_type_ref_substituting(
                type_ref->as.inner,
                type_params,
                type_param_count,
                type_args);
            if (clone->as.inner == NULL) {
                free(clone);
                return NULL;
            }
            return clone;
    }

    free(clone);
    return NULL;
}

/* ---- 收集 CALL 表达式返回类型中的间接泛型依赖 --------------------------- */

/* 对被调方法/函数的返回类型做参数代入，得到 caller 视角的类型引用并收集。
 * owner 和 callable 参数属于两个独立层次，必须依次完成代入。 */
static void rd_try_collect_call_return_type_dep(CollectContext *ctx,
                                                const FengExpr *expr) {
    const FengResolvedCallable *rc;
    const FengTypeRef *return_type_ref = NULL;
    const FengCallableSignature *callable = NULL;
    const FengTypeRef *current;
    FengTypeRef *owned_current = NULL;

    if (ctx == NULL || expr == NULL ||
        expr->kind != FENG_EXPR_CALL) {
        return;
    }

    rc = &expr->as.call.resolved_callable;

    /* 1. 获取被调目标的返回类型。 */
    switch (rc->kind) {
        case FENG_RESOLVED_CALLABLE_TYPE_METHOD:
        case FENG_RESOLVED_CALLABLE_FIT_METHOD:
        case FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD:
        case FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD:
        case FENG_RESOLVED_CALLABLE_SPEC_METHOD:
        case FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD:
            if (rc->member != NULL) {
                callable = &rc->member->as.callable;
                return_type_ref = rc->member->as.callable.return_type;
            }
            break;
        case FENG_RESOLVED_CALLABLE_FUNCTION:
            if (rc->function_decl != NULL) {
                callable = &rc->function_decl->as.function_decl;
                return_type_ref =
                    rc->function_decl->as.function_decl.return_type;
            }
            break;
        default:
            return;
    }
    if (return_type_ref == NULL) {
        return;
    }
    current = return_type_ref;

    /* 2. 先代入 owner 类型参数。 */
    if (rc->owner_type_decl != NULL &&
        (rc->owner_type_decl->kind == FENG_DECL_TYPE ||
         rc->owner_type_decl->kind == FENG_DECL_SPEC) &&
        rc->owner_instance_type_ref != NULL &&
        rc->owner_instance_type_ref->kind == FENG_TYPE_REF_NAMED) {
        const FengTypeParam *owner_type_params =
            rc->owner_type_decl->kind == FENG_DECL_TYPE
                ? rc->owner_type_decl->as.type_decl.type_params
                : rc->owner_type_decl->as.spec_decl.type_params;
        size_t owner_type_param_count =
            rc->owner_type_decl->kind == FENG_DECL_TYPE
                ? rc->owner_type_decl->as.type_decl.type_param_count
                : rc->owner_type_decl->as.spec_decl.type_param_count;
        const FengTypeRef *const *owner_type_args =
            (const FengTypeRef *const *)
                rc->owner_instance_type_ref->as.named.type_args;

        if (rc->owner_instance_type_ref->as.named.type_arg_count !=
            owner_type_param_count) {
            return;
        }
        if (owner_type_param_count > 0U &&
            type_ref_contains_type_param(current,
                                         owner_type_params,
                                         owner_type_param_count)) {
            owned_current = rd_clone_type_ref_substituting(
                current,
                owner_type_params,
                owner_type_param_count,
                owner_type_args);
            if (owned_current == NULL) {
                return;
            }
            current = owned_current;
        }
    }

    /* 3. 再代入方法/函数级类型参数。Semantic 记录的
     * callable_type_args 已经同时覆盖显式及推导实参。 */
    if (callable != NULL && callable->type_param_count > 0U &&
        rc->callable_type_args != NULL) {
        FengTypeRef *next;

        if (rc->callable_type_arg_count != callable->type_param_count) {
            rd_free_synthesized_type_ref(owned_current);
            return;
        }
        if (type_ref_contains_type_param(current,
                                         callable->type_params,
                                         callable->type_param_count)) {
            next = rd_clone_type_ref_substituting(
                current,
                callable->type_params,
                callable->type_param_count,
                rc->callable_type_args);
            if (next == NULL) {
                rd_free_synthesized_type_ref(owned_current);
                return;
            }
            rd_free_synthesized_type_ref(owned_current);
            owned_current = next;
            current = owned_current;
        }
    }

    /* 4. 无需代入时直接收集原始返回类型。 */
    if (owned_current == NULL) {
        try_collect_type_ref(ctx, current);
        return;
    }

    /* 5. 存入 analysis 管理生命周期。 */
    if (!rd_store_synthesized_ref(ctx->analysis, owned_current)) {
        rd_free_synthesized_type_ref(owned_current);
        return;
    }

    /* 6. 对代入后的类型调用 try_collect_type_ref。 */
    try_collect_type_ref(ctx, owned_current);
}

/* Collect the caller-view signature required to lower a lambda into its
 * resolved callable-form spec. Lambda parameter annotations alone do not
 * describe target-only result forms such as `Pair<T, string>`: the target
 * callable signature is the authoritative ABI and layout source. */
static void rd_collect_lambda_target_signature_deps(CollectContext *ctx,
                                                    const FengExpr *expr) {
    const FengSpecCoercionSite *site;
    const FengDecl *spec_decl;
    const FengTypeRef *target_ref;
    const FengTypeRef *const *type_args = NULL;
    size_t type_arg_count = 0U;
    size_t index;

    if (ctx == NULL || expr == NULL ||
        expr->kind != FENG_EXPR_LAMBDA) {
        return;
    }
    site = feng_semantic_lookup_spec_coercion_site(ctx->analysis, expr);
    if (site == NULL || site->form != FENG_SPEC_COERCION_FORM_CALLABLE ||
        site->callable_lambda_expr != expr) {
        return;
    }
    spec_decl = site->target_spec_decl;
    target_ref = site->target_spec_type_ref;
    if (spec_decl == NULL || spec_decl->kind != FENG_DECL_SPEC ||
        spec_decl->as.spec_decl.form != FENG_SPEC_FORM_CALLABLE ||
        target_ref == NULL) {
        return;
    }

    /* The callable instance itself may be default-initialized or otherwise
     * consumed by the shared body, so retain the ordinary managed dependency
     * in addition to the signature's transitive layout dependencies. */
    try_collect_type_ref(ctx, target_ref);
    if (target_ref->kind == FENG_TYPE_REF_NAMED) {
        type_args = (const FengTypeRef *const *)
            target_ref->as.named.type_args;
        type_arg_count = target_ref->as.named.type_arg_count;
    }

    for (index = 0U;
         index <= spec_decl->as.spec_decl.as.callable.param_count;
         ++index) {
        const FengTypeRef *signature_ref =
            index < spec_decl->as.spec_decl.as.callable.param_count
                ? spec_decl->as.spec_decl.as.callable.params[index].type
                : spec_decl->as.spec_decl.as.callable.return_type;
        FengTypeRef *substituted;

        if (signature_ref == NULL) {
            continue;
        }
        if (spec_decl->as.spec_decl.type_param_count == 0U) {
            try_collect_type_ref(ctx, signature_ref);
            continue;
        }
        if (type_args == NULL ||
            type_arg_count != spec_decl->as.spec_decl.type_param_count) {
            /* Semantic resolution already validates callable arity. A
             * mismatched site cannot yield a sound caller-view dependency. */
            continue;
        }
        substituted = rd_clone_type_ref_substituting(
            signature_ref,
            spec_decl->as.spec_decl.type_params,
            spec_decl->as.spec_decl.type_param_count,
            type_args);
        if (substituted == NULL) {
            continue;
        }
        if (!rd_store_synthesized_ref(ctx->analysis, substituted)) {
            rd_free_synthesized_type_ref(substituted);
            continue;
        }
        try_collect_type_ref(ctx, substituted);
    }
}

/* ---- 表达式收集（参照 inject_external_modules_from_expr 模式） ---------- */

static void collect_from_expr(CollectContext *ctx, const FengExpr *expr) {
    size_t i;
    const FengSpecCoercionSite *callable_value_site;

    if (expr == NULL) {
        return;
    }

    callable_value_site = feng_semantic_lookup_spec_coercion_site(
        ctx->analysis, expr);
    (void)rd_append_callable_value_dep(ctx->dep_set,
                                       callable_value_site,
                                       ctx->type_params,
                                       ctx->type_param_count);

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
        case FENG_EXPR_SELF:
        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
            return;

        case FENG_EXPR_ARRAY_LITERAL:
            for (i = 0U; i < expr->as.array_literal.count; ++i) {
                collect_from_expr(ctx, expr->as.array_literal.items[i]);
            }
            return;

        case FENG_EXPR_TUPLE_LITERAL:
            for (i = 0U; i < expr->as.tuple_literal.count; ++i) {
                collect_from_expr(ctx, expr->as.tuple_literal.items[i]);
            }
            return;

        case FENG_EXPR_OBJECT_LITERAL:
            collect_from_expr(ctx, expr->as.object_literal.target);
            for (i = 0U; i < expr->as.object_literal.field_count; ++i) {
                collect_from_expr(ctx,
                                  expr->as.object_literal.fields[i].value);
            }
            return;

        case FENG_EXPR_GENERIC_TARGET:
            /* 合成整体 FengTypeRef（如 Helper<K>）并尝试收集为具体化依赖。 */
            rd_try_collect_generic_target(ctx, expr);
            collect_from_expr(ctx, expr->as.generic_target.target);
            for (i = 0U; i < expr->as.generic_target.type_arg_count; ++i) {
                try_collect_type_ref(ctx,
                                    expr->as.generic_target.type_args[i]);
            }
            return;

        case FENG_EXPR_CALL:
            collect_from_expr(ctx, expr->as.call.callee);
            for (i = 0U; i < expr->as.call.arg_count; ++i) {
                collect_from_expr(ctx, expr->as.call.args[i]);
            }
            for (i = 0U; i < expr->as.call.explicit_type_arg_count; ++i) {
                try_collect_type_ref(ctx,
                                    expr->as.call.explicit_type_args[i]);
            }
            /* Type/fit calls may require the concrete owner descriptor in a
             * shared body. A spec call uses its owner instance only to close
             * the requirement signature; dispatch already receives the
             * subject witness, so treating that spec surface as a standalone
             * aggregate dependency would invent an unrelated runtime slot.
             * rd_try_collect_call_return_type_dep() below still collects the
             * correctly substituted return-type dependencies. */
            if (expr->as.call.resolved_callable.kind !=
                    FENG_RESOLVED_CALLABLE_SPEC_METHOD &&
                expr->as.call.resolved_callable.kind !=
                    FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD) {
                try_collect_type_ref(
                    ctx,
                    expr->as.call.resolved_callable.owner_instance_type_ref);
            }
            (void)rd_append_callable_dep(
                ctx->dep_set, &expr->as.call.resolved_callable);
            rd_try_collect_call_return_type_dep(ctx, expr);
            return;

        case FENG_EXPR_MEMBER:
            collect_from_expr(ctx, expr->as.member.object);
            {
                const FengSemanticTypeFact *member_fact =
                    feng_semantic_lookup_type_fact(ctx->analysis, expr);

                if (member_fact != NULL &&
                    member_fact->kind ==
                        FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
                    try_collect_type_ref(ctx, member_fact->type_ref);
                }
            }
            return;

        case FENG_EXPR_INDEX:
            collect_from_expr(ctx, expr->as.index.object);
            collect_from_expr(ctx, expr->as.index.index);
            return;

        case FENG_EXPR_UNARY:
            collect_from_expr(ctx, expr->as.unary.operand);
            return;

        case FENG_EXPR_BINARY:
            collect_from_expr(ctx, expr->as.binary.left);
            collect_from_expr(ctx, expr->as.binary.right);
            return;

        case FENG_EXPR_LAMBDA:
            rd_collect_lambda_target_signature_deps(ctx, expr);
            for (i = 0U; i < expr->as.lambda.param_count; ++i) {
                try_collect_type_ref(ctx, expr->as.lambda.params[i].type);
            }
            collect_from_expr(ctx, expr->as.lambda.body);
            collect_from_block(ctx, expr->as.lambda.body_block);
            return;

        case FENG_EXPR_CAST:
            try_collect_type_ref(ctx, expr->as.cast.type);
            collect_from_expr(ctx, expr->as.cast.value);
            return;

        case FENG_EXPR_IF:
            collect_from_expr(ctx, expr->as.if_expr.condition);
            collect_from_block(ctx, expr->as.if_expr.then_block);
            collect_from_block(ctx, expr->as.if_expr.else_block);
            return;

        case FENG_EXPR_MATCH:
            collect_from_expr(ctx, expr->as.match_expr.target);
            for (i = 0U; i < expr->as.match_expr.branch_count; ++i) {
                const FengMatchBranch *branch =
                    &expr->as.match_expr.branches[i];
                collect_from_match_branch_labels(ctx, branch);
                collect_from_block(ctx, branch->body);
            }
            collect_from_block(ctx, expr->as.match_expr.else_block);
            return;

        case FENG_EXPR_MATCH_OP:
            collect_from_expr(ctx, expr->as.match_op.target);
            for (i = 0U; i < expr->as.match_op.label_count; ++i) {
                const FengMatchLabel *label = &expr->as.match_op.labels[i];
                switch (label->kind) {
                    case FENG_MATCH_LABEL_VALUE:
                        collect_from_expr(ctx, label->value);
                        break;
                    case FENG_MATCH_LABEL_RANGE:
                        collect_from_expr(ctx, label->range_low);
                        collect_from_expr(ctx, label->range_high);
                        break;
                    case FENG_MATCH_LABEL_TYPE:
                        try_collect_type_ref(ctx, label->type);
                        break;
                }
            }
            return;

        case FENG_EXPR_TRY:
            collect_from_expr(ctx, expr->as.try_expr.body);
            for (i = 0U; i < expr->as.try_expr.clause_count; ++i) {
                const FengTryCatchClause *clause =
                    &expr->as.try_expr.clauses[i];
                try_collect_type_ref(ctx, clause->type);
                collect_from_block(ctx, clause->body);
            }
            return;

        case FENG_EXPR_ARRAY_NEW:
            try_collect_type_ref(ctx, expr->as.array_new.element_type);
            collect_from_expr(ctx, expr->as.array_new.size);
            return;
    }
}

/* ---- 语句收集（参照 inject_external_modules_from_stmt 模式） ------------ */

static void collect_from_stmt(CollectContext *ctx, const FengStmt *stmt) {
    size_t i;

    if (stmt == NULL) {
        return;
    }

    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            collect_from_block(ctx, stmt->as.block);
            return;

        case FENG_STMT_BINDING:
            collect_from_binding(ctx, &stmt->as.binding);
            return;

        case FENG_STMT_ASSIGN:
            collect_from_expr(ctx, stmt->as.assign.target);
            collect_from_expr(ctx, stmt->as.assign.value);
            return;

        case FENG_STMT_EXPR:
        case FENG_STMT_TRY:
            collect_from_expr(ctx, stmt->as.expr);
            return;

        case FENG_STMT_IF:
            for (i = 0U; i < stmt->as.if_stmt.clause_count; ++i) {
                collect_from_expr(ctx,
                                  stmt->as.if_stmt.clauses[i].condition);
                collect_from_block(ctx,
                                   stmt->as.if_stmt.clauses[i].block);
            }
            collect_from_block(ctx, stmt->as.if_stmt.else_block);
            return;

        case FENG_STMT_MATCH:
            collect_from_expr(ctx, stmt->as.match_stmt.target);
            for (i = 0U; i < stmt->as.match_stmt.branch_count; ++i) {
                const FengMatchBranch *branch =
                    &stmt->as.match_stmt.branches[i];
                collect_from_match_branch_labels(ctx, branch);
                collect_from_block(ctx, branch->body);
            }
            collect_from_block(ctx, stmt->as.match_stmt.else_block);
            return;

        case FENG_STMT_WHILE:
            collect_from_expr(ctx, stmt->as.while_stmt.condition);
            collect_from_block(ctx, stmt->as.while_stmt.body);
            return;

        case FENG_STMT_FOR:
            collect_from_stmt(ctx, stmt->as.for_stmt.init);
            collect_from_expr(ctx, stmt->as.for_stmt.condition);
            collect_from_stmt(ctx, stmt->as.for_stmt.update);
            collect_from_binding(ctx, &stmt->as.for_stmt.iter_binding);
            collect_from_expr(ctx, stmt->as.for_stmt.iter_expr);
            /* for-in 迭代器协议的 iter()/next() 调用由 codegen 合成，不在
             * AST 中。cursor 类型（RTD）和 result 元组类型（RAD）的依赖
             * 必须在此显式收集，否则共享体中 RTD/RAD 查找会失败。
             * iter_cursor_type_ref 和 iter_result_type_ref 由 analyzer 代入
             * 后克隆，生命周期由 Analysis 管理，此处安全读取。 */
            {
                const FengSemanticTypeFact *iter_source_fact =
                    feng_semantic_lookup_type_fact(
                        ctx->analysis, stmt->as.for_stmt.iter_expr);

                if (iter_source_fact != NULL &&
                    iter_source_fact->kind ==
                        FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
                    try_collect_type_ref(ctx, iter_source_fact->type_ref);
                    rd_collect_synthesized_method_call(
                        ctx,
                        stmt->as.for_stmt.iter_iterable_method,
                        iter_source_fact->type_ref);
                }
            }
            rd_collect_synthesized_method_call(
                ctx,
                stmt->as.for_stmt.iter_iterator_method,
                stmt->as.for_stmt.iter_cursor_type_ref);
            try_collect_type_ref(ctx, stmt->as.for_stmt.iter_cursor_type_ref);
            try_collect_type_ref(ctx, stmt->as.for_stmt.iter_result_type_ref);
            collect_from_block(ctx, stmt->as.for_stmt.body);
            return;

        case FENG_STMT_RETURN:
            collect_from_expr(ctx, stmt->as.return_value);
            return;

        case FENG_STMT_THROW:
            collect_from_expr(ctx, stmt->as.throw_value);
            return;

        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return;
        case FENG_STMT_DEFER:
            collect_from_block(ctx, stmt->as.defer_block);
            return;
    }
}

/* ---- 块收集 ------------------------------------------------------------ */

static void collect_from_block(CollectContext *ctx, const FengBlock *block) {
    size_t i;

    if (block == NULL) {
        return;
    }
    for (i = 0U; i < block->statement_count; ++i) {
        collect_from_stmt(ctx, block->statements[i]);
    }
}

/* ---- callable 收集 ----------------------------------------------------- */

/* 收集 callable 签名（params、return_type）和 body 中的泛型类型引用。
 * 若 callable 自身有方法级 type_params，将其与外层 type_params 合并。 */
static void collect_from_callable(CollectContext *ctx,
                                  const FengCallableSignature *callable) {
    const FengTypeParam *saved_params;
    size_t saved_count;
    FengTypeParam *merged;
    size_t merged_count;
    size_t i;

    if (callable == NULL) {
        return;
    }

    /* 保存当前 type_params，以便方法级参数合并后恢复。 */
    saved_params = ctx->type_params;
    saved_count = ctx->type_param_count;

    /* 若 callable 有方法级 type_params，合并到当前 scope。 */
    if (callable->type_param_count > 0U) {
        merged_count = saved_count + callable->type_param_count;
        merged = (FengTypeParam *)malloc(merged_count * sizeof(FengTypeParam));
        if (merged != NULL) {
            if (saved_count > 0U && saved_params != NULL) {
                memcpy(merged, saved_params,
                       saved_count * sizeof(FengTypeParam));
            }
            memcpy(merged + saved_count,
                   callable->type_params,
                   callable->type_param_count * sizeof(FengTypeParam));
            ctx->type_params = merged;
            ctx->type_param_count = merged_count;
        }
    }

    /* 参数类型。 */
    for (i = 0U; i < callable->param_count; ++i) {
        try_collect_type_ref(ctx, callable->params[i].type);
    }
    /* 返回值类型。 */
    try_collect_type_ref(ctx, callable->return_type);
    /* 方法体。 */
    collect_from_block(ctx, callable->body);

    /* 恢复 type_params。 */
    if (callable->type_param_count > 0U && ctx->type_params != saved_params) {
        free((void *)ctx->type_params);
        ctx->type_params = saved_params;
        ctx->type_param_count = saved_count;
    }
}

/* ---- 泛型类型的依赖收集 ------------------------------------------------ */

static void collect_for_type(FengSemanticAnalysis *analysis,
                             const FengDecl *decl) {
    CollectContext ctx;
    FengReifiableDepSet *dep_set;
    size_t i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.analysis = analysis;
    ctx.type_params = decl->as.type_decl.type_params;
    ctx.type_param_count = decl->as.type_decl.type_param_count;

    dep_set = NULL;
    if (ctx.type_param_count > 0U) {
        dep_set = feng_semantic_get_or_create_reifiable_dep_set(analysis, decl);
    }

    /* A member mixin source constructor executes in the instance-field
     * initialization phase.  Its open generic type/callable dependencies
     * therefore belong to the same owner descriptor set as ordinary field
     * initializers, constructors, and the finalizer. */
    if (dep_set != NULL) {
        ctx.dep_set = dep_set;
        for (i = 0U; i < decl->as.type_decl.mixin_count; ++i) {
            collect_from_expr(
                &ctx,
                decl->as.type_decl.mixins[i].source_constructor);
        }
    }

    /* 成员字段类型 + 初始化表达式。 */
    for (i = 0U; i < decl->as.type_decl.member_count; ++i) {
        const FengTypeMember *member = decl->as.type_decl.members[i];

        if (member->kind == FENG_TYPE_MEMBER_FIELD) {
            if (dep_set == NULL) {
                continue;
            }
            ctx.dep_set = dep_set;
            /* Inferred fields (field.type == NULL, e.g.
             * `let items = List<T>()`) carry their type only as a semantic
             * type fact.  Fall back to it so the generic instance type_ref
             * (e.g. List<T>) is collected as a reifiable dep; otherwise the
             * generic method body cannot find the reified type descriptor
             * when calling methods on the field (CE0007). */
            if (member->as.field.type == NULL) {
                const FengSemanticTypeFact *fact =
                    feng_semantic_lookup_type_fact(analysis, member);
                if (fact != NULL &&
                    fact->kind == FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
                    try_collect_type_ref(&ctx, fact->type_ref);
                }
            } else {
                try_collect_type_ref(&ctx, member->as.field.type);
            }
            collect_from_expr(&ctx, member->as.field.initializer);
            continue;
        }

        if (member->kind == FENG_TYPE_MEMBER_METHOD &&
            (ctx.type_param_count > 0U ||
             member->as.callable.type_param_count > 0U)) {
            /* 普通方法拥有独立的函数描述符，方法级参数不能进入静态的
             * owner type descriptor。 */
            ctx.dep_set =
                feng_semantic_get_or_create_member_reifiable_dep_set(
                    analysis, decl, member);
            if (ctx.dep_set != NULL) {
                collect_from_callable(&ctx, &member->as.callable);
            }
            continue;
        }

        if (dep_set != NULL) {
            /* 构造器与 finalizer 暂沿用既有 type descriptor ABI。 */
            ctx.dep_set = dep_set;
            collect_from_callable(&ctx, &member->as.callable);
        }
    }
}

/* ---- 独立泛型函数的依赖收集 -------------------------------------------- */

static void collect_for_generic_function(FengSemanticAnalysis *analysis,
                                         const FengDecl *decl) {
    CollectContext ctx;
    FengReifiableDepSet *dep_set;

    if (decl->as.function_decl.type_param_count == 0U) {
        return;
    }

    dep_set = feng_semantic_get_or_create_reifiable_dep_set(analysis, decl);
    if (dep_set == NULL) {
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.analysis = analysis;
    ctx.dep_set = dep_set;
    ctx.type_params = decl->as.function_decl.type_params;
    ctx.type_param_count = decl->as.function_decl.type_param_count;

    collect_from_callable(&ctx, &decl->as.function_decl);
}

/* ---- fit 泛型方法的依赖收集 -------------------------------------------- */

/* 获取 fit target type 的 type_params（仅当 target 为用户 type）。 */
static const FengDecl *find_fit_target_type_decl(
    const FengSemanticAnalysis *analysis,
    const FengDecl *fit_decl) {
    const FengTypeRef *target;
    const FengDecl *target_decl;

    target = fit_decl->as.fit_decl.target;
    if (target == NULL) {
        return NULL;
    }
    target_decl = find_type_decl_by_named_ref(analysis, target);
    if (target_decl != NULL && target_decl->kind == FENG_DECL_TYPE) {
        return target_decl;
    }
    return NULL;
}

/* 从 fit target TypeRef 中提取隐含的 fit 级类型参数。
 * 例如 `fit T[]` → 提取 T；`fit T[!]` → 提取 T。
 * 仅处理数组形式（ARRAY / POINTER+ARRAY），inner 为单 segment NAMED ref 时
 * 视为隐含类型参数。成功时写入 out_param 并返回 true。 */
static bool extract_fit_target_implicit_type_param(
    const FengTypeRef *target_ref,
    FengTypeParam *out_param) {
    const FengTypeRef *element_ref;

    if (target_ref == NULL || out_param == NULL) {
        return false;
    }

    /* fit T[!] 解析为 POINTER → ARRAY → inner，取 ARRAY 层的 inner。 */
    if (target_ref->kind == FENG_TYPE_REF_POINTER &&
        target_ref->as.inner != NULL &&
        target_ref->as.inner->kind == FENG_TYPE_REF_ARRAY) {
        element_ref = target_ref->as.inner->as.inner;
    } else if (target_ref->kind == FENG_TYPE_REF_ARRAY) {
        element_ref = target_ref->as.inner;
    } else {
        return false;
    }

    if (element_ref == NULL ||
        element_ref->kind != FENG_TYPE_REF_NAMED ||
        element_ref->as.named.segment_count != 1U ||
        element_ref->as.named.type_arg_count != 0U) {
        return false;
    }

    memset(out_param, 0, sizeof(*out_param));
    out_param->token = element_ref->token;
    out_param->name = element_ref->as.named.segments[0];
    out_param->constraint = NULL;
    return true;
}

static void collect_for_fit(FengSemanticAnalysis *analysis,
                            const FengDecl *decl) {
    const FengDecl *target_type_decl;
    const FengTypeParam *type_level_params = NULL;
    size_t type_level_param_count = 0U;
    FengTypeParam implicit_type_param;
    bool has_generic_context;
    CollectContext ctx;
    size_t i;

    /* 获取 fit target 类型的 type_params。 */
    target_type_decl = find_fit_target_type_decl(analysis, decl);
    if (target_type_decl != NULL &&
        target_type_decl->as.type_decl.type_param_count > 0U) {
        type_level_params = target_type_decl->as.type_decl.type_params;
        type_level_param_count =
            target_type_decl->as.type_decl.type_param_count;
    }

    /* 对于数组形式的 fit target（如 fit T[]、fit T[!]），从 target TypeRef
     * 中提取隐含的类型参数。 */
    if (type_level_param_count == 0U &&
        extract_fit_target_implicit_type_param(
            decl->as.fit_decl.target, &implicit_type_param)) {
        type_level_params = &implicit_type_param;
        type_level_param_count = 1U;
    }

    /* 判断 fit 是否有泛型上下文：target 类型有泛型参数，或某 member 有方法级
     * type_params。若都没有，无需收集。 */
    has_generic_context = (type_level_param_count > 0U);
    if (!has_generic_context) {
        for (i = 0U; i < decl->as.fit_decl.member_count; ++i) {
            const FengTypeMember *member = decl->as.fit_decl.members[i];
            if (member->as.callable.type_param_count > 0U) {
                has_generic_context = true;
                break;
            }
        }
    }
    if (!has_generic_context) {
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.analysis = analysis;
    ctx.type_params = type_level_params;
    ctx.type_param_count = type_level_param_count;

    /* 每个 fit 方法都有独立的 FengFunctionDescriptor，因此依赖必须按成员
     * 收集，不能合并到 fit 声明级集合中。collect_from_callable 内部会在
     * owner 泛参之外临时合并该方法自己的方法级泛参。 */
    for (i = 0U; i < decl->as.fit_decl.member_count; ++i) {
        const FengTypeMember *member = decl->as.fit_decl.members[i];

        if (member == NULL || member->kind != FENG_TYPE_MEMBER_METHOD) {
            continue;
        }
        ctx.dep_set =
            feng_semantic_get_or_create_member_reifiable_dep_set(
                analysis, decl, member);
        if (ctx.dep_set == NULL) {
            continue;
        }
        collect_from_callable(&ctx, &member->as.callable);
    }
}

/* ---- 主入口 ------------------------------------------------------------ */

bool feng_semantic_collect_reifiable_deps(FengSemanticAnalysis *analysis) {
    size_t mi, pi, di;

    if (analysis == NULL) {
        return false;
    }

    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];

        /* 仅处理本地模块，导入包的模块跳过。 */
        if (mod->origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
            continue;
        }

        for (pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];

            for (di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *decl = prog->declarations[di];

                switch (decl->kind) {
                    case FENG_DECL_TYPE:
                        collect_for_type(analysis, decl);
                        break;
                    case FENG_DECL_FUNCTION:
                        collect_for_generic_function(analysis, decl);
                        break;
                    case FENG_DECL_FIT:
                        collect_for_fit(analysis, decl);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    return true;
}
