/* Parameter storage is independent of its transport ABI. Writable managed
 * slots need a callee-owned copy before assignment can release their contents;
 * the incoming parameter remains protected by the caller's original token. */

/* Value reads use existing copy/retain lowering; storage uses may mutate the
 * original slot. Name shadowing is deliberately treated conservatively. */
static bool cg_parameter_expr_is_readonly(const FengExpr *expr, FengSlice name,
                                          bool value_read);
/* Include nested exits and deferred work in the proof for one parameter. */
static bool cg_parameter_block_is_readonly(const FengBlock *block, FengSlice name,
                                           bool value_read);

/* Pattern expressions may contain calls or other storage-exposing operations. */
static bool cg_parameter_labels_are_readonly(const FengMatchLabel *labels,
    size_t count, FengSlice name, bool value_read) {
    for (size_t i = 0U; i < count; ++i) {
        if (!cg_parameter_expr_is_readonly(labels[i].value, name, value_read) ||
            !cg_parameter_expr_is_readonly(labels[i].range_low, name, value_read) ||
            !cg_parameter_expr_is_readonly(labels[i].range_high, name, value_read))
            return false;
    }
    return true;
}

/* Check every match arm, including patterns and exceptional branch exits. */
static bool cg_parameter_branches_are_readonly(const FengMatchBranch *branches,
    size_t count, FengSlice name, bool value_read) {
    for (size_t i = 0U; i < count; ++i) {
        if (!cg_parameter_labels_are_readonly(branches[i].labels,
                branches[i].label_count, name, value_read) ||
            !cg_parameter_block_is_readonly(branches[i].body, name, value_read))
            return false;
    }
    return true;
}

/* Calls can mutate receivers, while explicit arguments retain ordinary value
 * semantics. Projections propagate exposure of the original storage; indexing,
 * match bindings and captures are conservative. Unknown nodes reject borrowing. */
static bool cg_parameter_expr_is_readonly(const FengExpr *expr, FengSlice name,
                                          bool value_read) {
    if (expr == NULL) return true;
    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            return value_read || expr->as.identifier.length != name.length ||
                memcmp(expr->as.identifier.data, name.data, name.length) != 0;
        case FENG_EXPR_SELF:
        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
        case FENG_EXPR_TYPE_TARGET:
            return true;
        case FENG_EXPR_ARRAY_LITERAL:
            for (size_t i = 0U; i < expr->as.array_literal.count; ++i)
                if (!cg_parameter_expr_is_readonly(expr->as.array_literal.items[i], name, value_read))
                    return false;
            return true;
        case FENG_EXPR_TUPLE_LITERAL:
            for (size_t i = 0U; i < expr->as.tuple_literal.count; ++i)
                if (!cg_parameter_expr_is_readonly(expr->as.tuple_literal.items[i], name, value_read))
                    return false;
            return true;
        case FENG_EXPR_OBJECT_LITERAL:
            if (!cg_parameter_expr_is_readonly(expr->as.object_literal.target, name, false))
                return false;
            for (size_t i = 0U; i < expr->as.object_literal.field_count; ++i)
                if (!cg_parameter_expr_is_readonly(expr->as.object_literal.fields[i].value, name, value_read))
                    return false;
            return true;
        case FENG_EXPR_GENERIC_TARGET:
            return cg_parameter_expr_is_readonly(expr->as.generic_target.target, name, value_read);
        case FENG_EXPR_CALL:
            if (!cg_parameter_expr_is_readonly(expr->as.call.callee, name, false)) return false;
            for (size_t i = 0U; i < expr->as.call.arg_count; ++i)
                if (!cg_parameter_expr_is_readonly(expr->as.call.args[i], name, value_read))
                    return false;
            return true;
        case FENG_EXPR_MEMBER:
            return cg_parameter_expr_is_readonly(expr->as.member.object, name, value_read);
        case FENG_EXPR_INDEX:
            return cg_parameter_expr_is_readonly(expr->as.index.object, name, false) &&
                cg_parameter_expr_is_readonly(expr->as.index.index, name, value_read);
        case FENG_EXPR_UNARY:
            return cg_parameter_expr_is_readonly(expr->as.unary.operand, name,
                value_read && expr->as.unary.op != FENG_TOKEN_AMP);
        case FENG_EXPR_BINARY:
            return cg_parameter_expr_is_readonly(expr->as.binary.left, name, value_read) &&
                cg_parameter_expr_is_readonly(expr->as.binary.right, name, value_read);
        case FENG_EXPR_LAMBDA:
            for (size_t i = 0U; i < expr->as.lambda.capture_count; ++i) {
                FengSlice capture = expr->as.lambda.captures[i].name;
                if (capture.length == name.length &&
                    memcmp(capture.data, name.data, name.length) == 0) return false;
            }
            return true;
        case FENG_EXPR_CAST:
            return cg_parameter_expr_is_readonly(expr->as.cast.value, name, value_read);
        case FENG_EXPR_IF:
            return cg_parameter_expr_is_readonly(expr->as.if_expr.condition, name, value_read) &&
                cg_parameter_block_is_readonly(expr->as.if_expr.then_block, name, value_read) &&
                cg_parameter_block_is_readonly(expr->as.if_expr.else_block, name, value_read);
        case FENG_EXPR_MATCH:
            return cg_parameter_expr_is_readonly(expr->as.match_expr.target, name, false) &&
                cg_parameter_branches_are_readonly(expr->as.match_expr.branches,
                    expr->as.match_expr.branch_count, name, value_read) &&
                cg_parameter_block_is_readonly(expr->as.match_expr.else_block, name, value_read);
        case FENG_EXPR_MATCH_OP:
            return cg_parameter_expr_is_readonly(expr->as.match_op.target, name, false) &&
                cg_parameter_labels_are_readonly(expr->as.match_op.labels,
                    expr->as.match_op.label_count, name, value_read);
        case FENG_EXPR_TRY:
            if (!cg_parameter_expr_is_readonly(expr->as.try_expr.body, name, value_read)) return false;
            for (size_t i = 0U; i < expr->as.try_expr.clause_count; ++i)
                if (!cg_parameter_block_is_readonly(expr->as.try_expr.clauses[i].body, name, value_read))
                    return false;
            return true;
        case FENG_EXPR_ARRAY_NEW:
            return cg_parameter_expr_is_readonly(expr->as.array_new.size, name, value_read);
    }
    return false;
}

/* Assignments and receiver-based iteration expose storage. All nested control
 * flow is inspected, so a write in any reachable or unreachable branch counts. */
static bool cg_parameter_stmt_is_readonly(const FengStmt *stmt, FengSlice name,
                                          bool value_read) {
    if (stmt == NULL) return true;
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return cg_parameter_block_is_readonly(stmt->as.block, name, value_read);
        case FENG_STMT_BINDING:
            return cg_parameter_expr_is_readonly(stmt->as.binding.initializer, name, value_read);
        case FENG_STMT_ASSIGN:
            return cg_parameter_expr_is_readonly(stmt->as.assign.target, name, false) &&
                cg_parameter_expr_is_readonly(stmt->as.assign.value, name, value_read);
        case FENG_STMT_EXPR:
        case FENG_STMT_TRY:
            return cg_parameter_expr_is_readonly(stmt->as.expr, name, value_read);
        case FENG_STMT_IF:
            for (size_t i = 0U; i < stmt->as.if_stmt.clause_count; ++i) {
                const FengIfClause *clause = &stmt->as.if_stmt.clauses[i];
                if (!cg_parameter_expr_is_readonly(clause->condition, name, value_read) ||
                    !cg_parameter_block_is_readonly(clause->block, name, value_read)) return false;
            }
            return cg_parameter_block_is_readonly(stmt->as.if_stmt.else_block, name, value_read);
        case FENG_STMT_MATCH:
            return cg_parameter_expr_is_readonly(stmt->as.match_stmt.target, name, false) &&
                cg_parameter_branches_are_readonly(stmt->as.match_stmt.branches,
                    stmt->as.match_stmt.branch_count, name, value_read) &&
                cg_parameter_block_is_readonly(stmt->as.match_stmt.else_block, name, value_read);
        case FENG_STMT_WHILE:
            return cg_parameter_expr_is_readonly(stmt->as.while_stmt.condition, name, value_read) &&
                cg_parameter_block_is_readonly(stmt->as.while_stmt.body, name, value_read);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in)
                return cg_parameter_expr_is_readonly(stmt->as.for_stmt.iter_expr, name, false) &&
                    cg_parameter_expr_is_readonly(stmt->as.for_stmt.iter_binding.initializer, name, value_read) &&
                    cg_parameter_block_is_readonly(stmt->as.for_stmt.body, name, value_read);
            return cg_parameter_stmt_is_readonly(stmt->as.for_stmt.init, name, value_read) &&
                cg_parameter_expr_is_readonly(stmt->as.for_stmt.condition, name, value_read) &&
                cg_parameter_stmt_is_readonly(stmt->as.for_stmt.update, name, value_read) &&
                cg_parameter_block_is_readonly(stmt->as.for_stmt.body, name, value_read);
        case FENG_STMT_RETURN:
            return cg_parameter_expr_is_readonly(stmt->as.return_value, name, value_read);
        case FENG_STMT_THROW:
            return cg_parameter_expr_is_readonly(stmt->as.throw_value, name, value_read);
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return true;
        case FENG_STMT_DEFER:
            return cg_parameter_block_is_readonly(stmt->as.defer_block, name, value_read);
    }
    return false;
}

/* Missing child blocks are empty; callers reject a completely missing body. */
static bool cg_parameter_block_is_readonly(const FengBlock *block, FengSlice name,
                                           bool value_read) {
    if (block == NULL) return true;
    for (size_t i = 0U; i < block->statement_count; ++i)
        if (!cg_parameter_stmt_is_readonly(block->statements[i], name, value_read)) return false;
    return true;
}

/* Promote the latest uncaptured parameter through the ordinary owning-value
 * materializer. Captured parameters already own their capture-cell contents.
 * Scalar parameters and read-only managed handles keep their existing path. */
static bool cg_prepare_parameter_ownership(CG *cg, Scope *scope, FengToken blame,
    const FengBlock *body, const FengExpr *expression) {
    if (scope == NULL || scope != cg->cur_scope || scope->count == 0U)
        return cg_fail(cg, blame, "IE0002", "codegen: parameter storage is missing");
    const Local source = scope->items[scope->count - 1U];
    /* The shared value ABI already lends an independently owned argument
     * slot whose caller cleanup observes field replacement in that storage.
     * A by-value C parameter only copied its representation and owns no token. */
    bool mutable_value = !source.is_storage_address &&
        cg_type_is_value_semantics(source.type) &&
        !cg_ownership_slots_are_immutable(source.type);
    bool needs_owner = source.binding_is_rebindable || mutable_value;
    if (!needs_owner || (!cgtype_is_managed(source.type) &&
                        !cgtype_is_aggregate(source.type) &&
                        source.type->kind != CG_TYPE_GENERIC_PARAM)) return true;
    FengSlice binding = {source.name, strlen(source.name)};
    if ((body != NULL || expression != NULL) &&
        cg_parameter_block_is_readonly(body, binding, true) &&
        cg_parameter_expr_is_readonly(expression, binding, true)) return true;

    ExprResult value;
    er_init(&value);
    value.c_expr = strdup(source.c_name);
    value.type = cgtype_clone(source.type);
    value.is_addressable = true;
    value.is_storage_address = source.is_storage_address;
    value.uses_reified_storage = source.uses_reified_storage;
    if (source.reified_descriptor_c_name != NULL)
        value.reified_descriptor_c_name = strdup(source.reified_descriptor_c_name);
    if (source.reified_size_c_name != NULL)
        value.reified_size_c_name = strdup(source.reified_size_c_name);
    char *name = strdup(source.name);
    bool ok = name != NULL && value.c_expr != NULL && value.type != NULL &&
        (source.reified_descriptor_c_name == NULL || value.reified_descriptor_c_name != NULL) &&
        (source.reified_size_c_name == NULL || value.reified_size_c_name != NULL);
    if (ok && cgtype_is_managed(value.type)) {
        cg_emit_current_stmt_line_directive_force(cg);
        buf_append_fmt(cg->cur_body, "    feng_retain(%s);\n", value.c_expr);
        value.owns_ref = true;
    }
    if (ok) ok = cg_materialize_to_local(cg, &value, "_parameter");
    if (ok) {
        /* Scope lookup selects this independently owned binding. The original
         * incoming slot stays a non-owning parameter and has no extra cleanup. */
        Local *owned = &scope->items[scope->count - 1U];
        free(owned->name);
        owned->name = name;
        name = NULL;
        owned->binding_mutability_known = source.binding_mutability_known;
        owned->binding_is_rebindable = source.binding_is_rebindable;
        owned->ownership_is_stable = !source.binding_is_rebindable;
    }
    free(name);
    er_free(&value);
    if (!ok && !cg->failed)
        cg_fail(cg, blame, "IE0001", "codegen: out of memory preparing parameter ownership");
    return ok;
}

/* Preserve the original debug contract unless ownership introduced a new slot. */
static const char *cg_parameter_read_expr(const Scope *scope, const char *original) {
    const Local *local = &scope->items[scope->count - 1U];
    return local->is_param ? original : local->c_name;
}
