/* Private invocation preparation. All dispatch paths evaluate operands here
 * before ABI formatting; no runtime API or generated-text recognition is used. */

/* Open a nested invocation without changing lexical scope or cleanup order. */
static void cg_call_frame_begin(CG *cg, CGCallFrame *frame) {
    *frame = (CGCallFrame){.parent = cg->call_frame, .body = cg->cur_body};
    cg->call_frame = frame;
}

/* Remember only protections introduced for this invocation. Existing owned
 * producer temporaries keep their established scope lifetime. */
static bool cg_call_frame_add_guard(CG *cg, CGOwnedLocal owner) {
    CGCallFrame *frame = cg->call_frame;
    if (frame == NULL || frame->body != cg->cur_body || owner.scope == NULL)
        return false;
    if (frame->count == frame->capacity) {
        size_t capacity = frame->capacity != 0U ? frame->capacity * 2U : 4U;
        CGOwnedLocal *guards = realloc(frame->guards, capacity * sizeof *guards);
        if (guards == NULL) return false;
        frame->guards = guards;
        frame->capacity = capacity;
    }
    frame->guards[frame->count++] = owner;
    return true;
}

/* A noncaptured local can only be rebound by code in its own invocation.
 * A suffix of direct local reads/literals has no such effects. All calls,
 * projections and control-flow expressions remain conservative. */
static bool cg_call_keeps_local_identity(CG *cg, const ExprResult *value,
                                          const FengExpr *source) {
    CGCallFrame *frame = cg->call_frame;
    const Local *local = cg_owned_local(value->local_owner);
    if (frame == NULL || frame->body != cg->cur_body || local == NULL ||
        source->kind != FENG_EXPR_IDENTIFIER || local->capture_cell_c_name != NULL ||
        local->is_storage_address || local->uses_erased_generic_storage ||
        !cg_types_equal(value->type, local->type)) return false;
    size_t next = 0U;
    for (size_t i = 0U; i < frame->argument_count; ++i) {
        if (frame->arguments[i] == source) { next = i + 1U; break; }
    }
    for (size_t i = next; i < frame->argument_count; ++i) {
        const FengExpr *argument = frame->arguments[i];
        switch (argument->kind) {
            case FENG_EXPR_BOOL:
            case FENG_EXPR_INTEGER:
            case FENG_EXPR_FLOAT:
                break;
            case FENG_EXPR_IDENTIFIER:
                if (scope_lookup(cg->cur_scope, argument->as.identifier.data,
                                 argument->as.identifier.length) == NULL) return false;
                break;
            default: return false;
        }
    }
    return true;
}

/* Freeze the complete value and attach its single owning member to the normal
 * cleanup chain. Generic identity does not change this fixed physical layout. */
static bool cg_prepare_single_owner_operand(CG *cg, ExprResult *value,
    const char *member, FengToken blame) {
    bool owned = value->owns_ref;
    if (!cg_materialize_ownership_alias(cg, value, "_call_arg")) return false;
    char *owner = cg_fresh_temp(cg, "_call_owner");
    CGType *pointer = cgtype_new(CG_TYPE_CALLABLE);
    if (owner == NULL || pointer == NULL) {
        free(owner); cgtype_free(pointer); return false;
    }
    buf_append_fmt(cg->cur_body, "    %svoid *%s = %s.%s;\n",
        cg_debug_local_attribute(cg, owner), owner, value->c_expr, member);
    if (!owned) buf_append_fmt(cg->cur_body, "    feng_retain(%s);\n", owner);
    bool ok = cg_register_local_for_cleanup(cg, owner, pointer, blame);
    /* This is the existing aggregate argument copy, whose scope lifetime is
     * observable. It is not an invocation-only managed identity guard. */
    value->owns_ref = false;
    value->local_owner = (CGOwnedLocal){0};
    free(owner); cgtype_free(pointer);
    return ok;
}

/* Protect only the managed representation of an erased value. Its aggregate
 * argument copy has a separate, pre-existing scope lifetime. Receiver and
 * explicit argument preparation share this exact conditional node contract. */
static bool cg_guard_call_managed_storage(CG *cg, const char *storage,
    const char *descriptor, FengToken blame, const char *receiver_address) {
    char *subject = cg_fresh_temp(cg, "_call_subject");
    CGType *pointer = cgtype_new(CG_TYPE_CALLABLE);
    Buf condition; buf_init(&condition);
    if (descriptor == NULL || subject == NULL || pointer == NULL) {
        free(subject); cgtype_free(pointer); return false;
    }
    buf_append_fmt(&condition, "%s->kind == FENG_VALUE_MANAGED_POINTER", descriptor);
    /* The node must outlive the representation-selection block, just like
     * erased argument-copy nodes. Only activation is conditional. */
    if (!scope_add(cg->cur_scope, subject, subject, pointer, false)) {
        free(subject); cgtype_free(pointer); buf_free(&condition);
        return cg_fail(cg, blame, "IE0001", "codegen: out of memory");
    }
    pointer = NULL; /* Scope owns the cleanup metadata. */
    cg_note_cleanup_requirement(cg);
    cg_emit_current_stmt_line_directive_force(cg);
    buf_append_fmt(cg->cur_body,
        "    %svoid *%s = NULL;\n"
        "    FENG_CODEGEN_NODEBUG FengCleanupNode _cu_%s;\n"
        "    if (%s) {\n"
        "        %s = *(void *const *)%s; feng_retain(%s);\n",
        cg_debug_local_attribute(cg, subject), subject, subject, condition.data,
        subject, storage, subject);
    if (receiver_address != NULL)
        buf_append_fmt(cg->cur_body, "        %s = &%s;\n", receiver_address, subject);
    buf_append_fmt(cg->cur_body, "        feng_cleanup_push(&_cu_%s, &%s);\n",
                   subject, subject);
    buf_append_cstr(cg->cur_body, "    }\n");
    CGOwnedLocal owner = {cg->cur_scope, cg->cur_scope->count - 1U};
    owner.scope->items[owner.index].cleanup_condition_c_expr = condition.data;
    condition.data = NULL;
    bool ok = cg_call_frame_add_guard(cg, owner);
    free(subject); cgtype_free(pointer); buf_free(&condition);
    return ok;
}

/* Evaluate a value once. Stable owners may lend their managed identity; all
 * other borrowed managed/value representations get independent protection.
 * Literal constants are already evaluated and need no C storage. */
static bool cg_prepare_call_operand(CG *cg, ExprResult *value,
                                     const FengExpr *source) {
    if (value->call_operand_prepared) return true;
    if (value->type == NULL || value->c_expr == NULL) return false;
    bool managed = cgtype_is_managed(value->type);
    bool dynamic = value->type->kind == CG_TYPE_GENERIC_PARAM ||
                   cg_type_uses_reified_storage(cg, value->type);
    bool aggregate = cgtype_is_aggregate(value->type);
    CGAggregateFacts facts = {0};
    if (aggregate) cg_aggregate_facts(value->type, &facts);
    bool owned = value->owns_ref;
    bool stable = !dynamic && (managed || !aggregate
        ? (value->managed_identity_is_stable || cg_call_keeps_local_identity(cg, value, source))
        : cg_stable_result_owner(cg, value).scope != NULL);
    if (!owned && stable) {
        /* The complete owning path is immutable. Delaying this read cannot
         * change its value or repeat a producer, so its existing spelling is
         * already an evaluated operand and needs no additional local. */
    } else if (facts.retained_owner_member != NULL) {
        if (!cg_prepare_single_owner_operand(cg, value,
                facts.retained_owner_member, source->token)) return false;
    } else if (managed && !owned) {
        if (!cg_materialize_ownership_alias(cg, value, "_call_arg")) return false;
        buf_append_fmt(cg->cur_body, "    feng_retain(%s);\n", value->c_expr);
        if (!cg_register_local_for_cleanup(cg, value->c_expr, value->type,
                                           source->token)) return false;
        cg_result_adopts_last_local(cg, value);
        value->managed_identity_is_stable = true;
        if (!cg_call_frame_add_guard(cg, value->local_owner)) return false;
    } else if (!owned && value->type->kind == CG_TYPE_GENERIC_PARAM) {
        bool stable_identity = value->managed_identity_is_stable;
        /* Preserve the established aggregate copy's scope lifetime. Only an
         * unstable managed handle needs an additional invocation-only guard. */
        if (!cg_materialize_shared_callable_value_argument(cg, value, "_call_arg"))
            return false;
        cg_result_adopts_last_local(cg, value);
        if (!stable_identity && !cg_guard_call_managed_storage(cg, value->c_expr,
                value->erased_generic_descriptor_c_name, source->token, NULL))
            return false;
    } else if (owned || aggregate || dynamic) {
        char *storage = cg_materialize_to_local(cg, value, "_call_arg");
        if (storage == NULL) return false;
        free(storage);
    } else if (source->kind != FENG_EXPR_BOOL &&
               source->kind != FENG_EXPR_INTEGER &&
               source->kind != FENG_EXPR_FLOAT) {
        if (!cg_materialize_ownership_alias(cg, value, "_call_arg")) return false;
    }
    value->call_operand_prepared = true;
    return true;
}

/* Type-directed construction and semantic coercion finish before the next
 * argument starts. The result remains useful for generic type inference. */
static bool cg_emit_call_argument(CG *cg, const FengExpr *expr,
    const CGType *expected_type, ExprResult *out) {
    if (!cg_emit_expr_for_expected_type(cg, expr, expected_type, out)) return false;
    if (cg_prepare_call_operand(cg, out, expr)) return true;
    er_free(out);
    return cg_fail(cg, expr->token, "IE0001", "codegen: call operand preparation failed");
}

/* Address ABI formatting consumes an existing snapshot without recopying it.
 * Non-operand results still use the ordinary ownership materialization. */
static char *cg_materialize_call_storage(CG *cg, ExprResult *result,
                                         const char *prefix) {
    if (result->call_operand_prepared && result->is_addressable)
        return strdup(result->c_expr);
    return cg_materialize_to_local(cg, result, prefix);
}

/* Only projections along the actual receiver address need their containing
 * allocation protected. Nested calls/branches have their own frames/scopes. */
static bool cg_call_preserves_receiver_storage(CG *cg, const FengExpr *expr) {
    CGCallFrame *frame = cg->call_frame;
    if (frame == NULL || frame->body != cg->cur_body) return false;
    const FengExpr *part = frame->receiver_source;
    while (part != NULL) {
        if (part == expr) return true;
        if (part->kind == FENG_EXPR_MEMBER) part = part->as.member.object;
        else if (part->kind == FENG_EXPR_INDEX) part = part->as.index.object;
        else break;
    }
    return false;
}

/* Explicit values copy, but implicit self refers to the selected original
 * storage. Managed receivers fix identity; generic receivers select the same
 * rule using their existing descriptor, never the placeholder C type. */
static bool cg_prepare_call_receiver(CG *cg, ExprResult *receiver,
                                      const FengExpr *source) {
    if (cgtype_is_managed(receiver->type) || receiver->type->kind == CG_TYPE_SPEC)
        return cg_prepare_call_operand(cg, receiver, source);
    if (receiver->type->kind == CG_TYPE_GENERIC_PARAM &&
        receiver->managed_identity_is_stable)
        return true; /* Immutable pointer identity or the original value self. */
    if (receiver->owns_ref || !receiver->is_addressable) {
        char *storage = cg_materialize_to_local(cg, receiver, "_receiver");
        if (storage == NULL) return false;
        free(storage);
        return true;
    }
    if (receiver->type->kind != CG_TYPE_GENERIC_PARAM &&
        (source->kind == FENG_EXPR_IDENTIFIER || source->kind == FENG_EXPR_SELF))
        return true; /* Its slot address cannot be replaced by later arguments. */
    char *address = cg_aggregate_result_address_dup(receiver);
    char *snapshot = cg_fresh_temp(cg, "_receiver_address");
    if (address == NULL || snapshot == NULL) {
        free(address); free(snapshot); return false;
    }
    cg_emit_current_stmt_line_directive_force(cg);
    buf_append_fmt(cg->cur_body, "    %svoid *%s = %s;\n",
        cg_debug_local_attribute(cg, snapshot), snapshot, address);
    free(address);
    if (receiver->type->kind == CG_TYPE_GENERIC_PARAM) {
        const char *descriptor = receiver->uses_erased_generic_storage
            ? receiver->erased_generic_descriptor_c_name
            : cg_generic_param_desc_name(cg, receiver->type->generic_param_index);
        if (!cg_guard_call_managed_storage(cg, snapshot, descriptor,
                                           source->token, snapshot)) {
            free(snapshot); return false;
        }
    }
    free(receiver->c_expr);
    if (receiver->is_storage_address) receiver->c_expr = snapshot;
    else {
        char *ctype = cg_ctype_dup(receiver->type);
        Buf value; buf_init(&value);
        if (ctype != NULL) buf_append_fmt(&value, "(*((%s *)%s))", ctype, snapshot);
        free(ctype); free(snapshot); receiver->c_expr = value.data;
    }
    return receiver->c_expr != NULL;
}

/* Execute a delayed call before dropping protections. Returning an owned C
 * value keeps its +1; finalizers cannot propagate across their ABI boundary. */
static bool cg_call_frame_finish(CG *cg, CGCallFrame *frame,
                                  ExprResult *result, bool ok) {
    if (ok && frame->count != 0U && result != NULL && !result->is_addressable) {
        if (result->type->kind == CG_TYPE_VOID) {
            cg_emit_current_stmt_line_directive_force(cg);
            buf_append_fmt(cg->cur_body, "    %s;\n", result->c_expr);
            free(result->c_expr);
            result->c_expr = strdup("((void)0)");
            ok = result->c_expr != NULL;
        } else {
            ok = cg_materialize_ownership_alias(cg, result, "_call_result");
        }
    }
    for (size_t i = frame->count; ok && i > 0U; --i) {
        CGOwnedLocal owner = frame->guards[i - 1U];
        Local *local = &owner.scope->items[owner.index];
        Scope suffix = {.parent = owner.scope, .items = local, .count = 1U};
        bool at_top = owner.index + 1U == owner.scope->count;
        cg_release_scope_values(cg, &suffix, (CGOwnedLocal){0}, at_top);
        if (at_top) scope_discard_suffix(owner.scope, owner.index);
        else local->cleanup_pop_only = true;
    }
    cg->call_frame = frame->parent;
    free(frame->guards);
    return ok;
}
