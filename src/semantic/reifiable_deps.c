#include "semantic.h"

#include <stdlib.h>
#include <string.h>

/* ===================================================================
 * ReifiableDepSet 基础 API（get_or_create / append / lookup）
 * =================================================================== */

FengReifiableDepSet *feng_semantic_get_or_create_reifiable_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl) {
    size_t index;
    FengReifiableDepSet *slot;

    if (analysis == NULL || owner_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->reifiable_dep_set_count; ++index) {
        if (analysis->reifiable_dep_sets[index].owner_decl == owner_decl) {
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
    return slot;
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

    /* 去重：相同 type_ref 指针不重复追加。 */
    for (index = 0U; index < dep_set->dep_count; ++index) {
        if (dep_set->deps[index].type_ref == type_ref &&
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

const FengReifiableDepSet *feng_semantic_lookup_reifiable_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl) {
    size_t index;

    if (analysis == NULL || owner_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->reifiable_dep_set_count; ++index) {
        if (analysis->reifiable_dep_sets[index].owner_decl == owner_decl) {
            return &analysis->reifiable_dep_sets[index];
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
                    rd_slice_equals(rd_decl_type_name(d), name)) {
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
                    rd_slice_equals(rd_decl_type_name(d), name)) {
                    return d;
                }
            }
        }
    }
    return NULL;
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
            if (decl->as.type_decl.is_tuple) {
                *out_kind = FENG_REIFIABLE_DEP_KIND_AGGREGATE;
            } else {
                *out_kind = FENG_REIFIABLE_DEP_KIND_MANAGED;
            }
            return true;
        case FENG_DECL_SPEC:
            /* object-form / union-form spec → AGGREGATE；callable-form → 不记录
             * （callable-form spec 是闭包指针，不需要具体化描述符）。 */
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                return false;
            }
            *out_kind = FENG_REIFIABLE_DEP_KIND_AGGREGATE;
            return true;
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

/* 将合成的 FengTypeRef 存入 analysis 的 synthesized_type_refs 中管理生命周期。 */
static bool rd_store_synthesized_ref(FengSemanticAnalysis *analysis,
                                     FengTypeRef *ref) {
    if (analysis->synthesized_type_ref_count ==
        analysis->synthesized_type_ref_capacity) {
        size_t new_capacity = analysis->synthesized_type_ref_capacity == 0U
                                  ? 8U
                                  : analysis->synthesized_type_ref_capacity * 2U;
        FengTypeRef **grown = (FengTypeRef **)realloc(
            analysis->synthesized_type_refs,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        analysis->synthesized_type_refs = grown;
        analysis->synthesized_type_ref_capacity = new_capacity;
    }
    analysis->synthesized_type_refs[analysis->synthesized_type_ref_count++] =
        ref;
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

/* ---- 表达式收集（参照 inject_external_modules_from_expr 模式） ---------- */

static void collect_from_expr(CollectContext *ctx, const FengExpr *expr) {
    size_t i;

    if (expr == NULL) {
        return;
    }

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
            return;

        case FENG_EXPR_MEMBER:
            collect_from_expr(ctx, expr->as.member.object);
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
                collect_from_block(ctx, branch->body);
            }
            collect_from_block(ctx, expr->as.match_expr.else_block);
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

static void collect_for_generic_type(FengSemanticAnalysis *analysis,
                                     const FengDecl *decl) {
    CollectContext ctx;
    FengReifiableDepSet *dep_set;
    size_t i;

    if (decl->as.type_decl.type_param_count == 0U) {
        return;
    }

    dep_set = feng_semantic_get_or_create_reifiable_dep_set(analysis, decl);
    if (dep_set == NULL) {
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.analysis = analysis;
    ctx.dep_set = dep_set;
    ctx.type_params = decl->as.type_decl.type_params;
    ctx.type_param_count = decl->as.type_decl.type_param_count;

    /* 成员字段类型 + 初始化表达式。 */
    for (i = 0U; i < decl->as.type_decl.member_count; ++i) {
        const FengTypeMember *member = decl->as.type_decl.members[i];

        if (member->kind == FENG_TYPE_MEMBER_FIELD) {
            try_collect_type_ref(&ctx, member->as.field.type);
            collect_from_expr(&ctx, member->as.field.initializer);
            continue;
        }
        /* 方法 / 构造器 / finalizer。 */
        collect_from_callable(&ctx, &member->as.callable);
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

static void collect_for_fit(FengSemanticAnalysis *analysis,
                            const FengDecl *decl) {
    const FengDecl *target_type_decl;
    const FengTypeParam *type_level_params = NULL;
    size_t type_level_param_count = 0U;
    bool has_generic_context;
    FengReifiableDepSet *dep_set;
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

    dep_set = feng_semantic_get_or_create_reifiable_dep_set(analysis, decl);
    if (dep_set == NULL) {
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.analysis = analysis;
    ctx.dep_set = dep_set;
    ctx.type_params = type_level_params;
    ctx.type_param_count = type_level_param_count;

    /* 遍历各 member method。collect_from_callable 内部会合并方法级
     * type_params。 */
    for (i = 0U; i < decl->as.fit_decl.member_count; ++i) {
        const FengTypeMember *member = decl->as.fit_decl.members[i];
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
                        collect_for_generic_type(analysis, decl);
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
