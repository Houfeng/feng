/* Compiler-only ownership proofs. Included after ExprResult: no public API,
 * runtime query, generated-text matching, or cross-package contract is added. */

/* Resolve a scope/index identity only while its declaring scope is alive. */
static const Local *cg_owned_local(CGOwnedLocal owner) {
    return owner.scope != NULL && owner.index < owner.scope->count
        ? &owner.scope->items[owner.index] : NULL;
}

/* Record an exact local read, following any previously proven stable borrow. */
static void cg_result_borrows_local(ExprResult *result, Scope *scope,
                                    const Local *local) {
    result->local_owner = (CGOwnedLocal){0};
    for (Scope *at = scope; at != NULL; at = at->parent) {
        for (size_t index = 0U; index < at->count; ++index) {
            if (&at->items[index] != local) continue;
            result->local_owner = local->borrowed_owner.scope != NULL
                ? local->borrowed_owner : (CGOwnedLocal){at, index};
            return;
        }
    }
}

/* Only a nonzero release without collector effects can be paired away.
 * This mirrors existing descriptor facts; opaque subjects/layouts remain
 * conservative. Array handles themselves do not enter the candidate buffer. */
static bool cg_ownership_pair_has_no_effect(CG *cg, const CGType *type) {
    if (type == NULL || type->kind == CG_TYPE_UNKNOWN ||
        type->kind == CG_TYPE_GENERIC_PARAM ||
        cg_type_uses_reified_storage(cg, type)) return false;
    if (cg_type_is_value_semantics(type)) {
        if (type->user == NULL) return false;
        for (size_t i = 0U; i < type->user->field_count; ++i) {
            if (!cg_ownership_pair_has_no_effect(cg, type->user->fields[i].type))
                return false;
        }
        return true;
    }
    switch (type->kind) {
        case CG_TYPE_STRING:
        case CG_TYPE_ARRAY:
            return true;
        case CG_TYPE_OBJECT:
            return type->user != NULL && type->user->decl != NULL &&
                cg_program_origin(cg, type->user->owner_program) !=
                    FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE &&
                !feng_semantic_type_is_potentially_cyclic(cg->analysis,
                                                         type->user->decl);
        case CG_TYPE_SPEC:
        case CG_TYPE_CALLABLE:
            return false;
        default:
            return true;
    }
}

/* A shallow value copy must not later release/rebind a borrowed managed slot.
 * Reference pointees may mutate; their handle identity is still unchanged.
 * ABI-addressable value storage is conservative because foreign writes are
 * outside these local facts. */
static bool cg_ownership_slots_are_immutable(const CGType *type) {
    if (type == NULL || type->kind == CG_TYPE_GENERIC_PARAM) return false;
    if (!cg_type_is_value_semantics(type)) return true;
    if (type->user == NULL || type->user->is_abi_type) return false;
    for (size_t i = 0U; i < type->user->field_count; ++i) {
        const UserField *field = &type->user->fields[i];
        if ((field->member != NULL &&
             field->member->as.field.mutability == FENG_MUTABILITY_VAR) ||
            !cg_ownership_slots_are_immutable(field->type)) return false;
    }
    return true;
}

/* Verify the token's representation and exclusive cleanup responsibility.
 * Captured/conditional/dynamic slots need more facts and use the old path. */
static const Local *cg_result_ownership_source(CG *cg, const ExprResult *result) {
    const Local *local = cg_owned_local(result->local_owner);
    if (result->owns_ref || local == NULL || local->is_param ||
        local->borrowed_owner.scope != NULL || local->cleanup_pop_only ||
        local->cleanup_condition_c_expr != NULL ||
        local->capture_cell_c_name != NULL || local->is_storage_address ||
        local->uses_erased_generic_storage || local->uses_reified_storage ||
        !cg_types_equal(local->type, result->type) ||
        (!cgtype_is_managed(local->type) && !cgtype_is_aggregate(local->type)) ||
        !cg_ownership_slots_are_immutable(local->type) ||
        !cg_ownership_pair_has_no_effect(cg, local->type)) return NULL;
    return local;
}

/* Borrow only from a stable, already owning ancestor whose lifetime covers
 * this entire use. Parameters and arbitrary field aliases are not owners. */
static CGOwnedLocal cg_stable_result_owner(CG *cg, const ExprResult *result) {
    const Local *local = cg_result_ownership_source(cg, result);
    if (local == NULL ||
        !(local->ownership_is_stable ||
          (local->binding_mutability_known && !local->binding_is_rebindable)))
        return (CGOwnedLocal){0};
    for (Scope *scope = cg->cur_scope; scope != NULL; scope = scope->parent) {
        if (scope == result->local_owner.scope) return result->local_owner;
    }
    return (CGOwnedLocal){0};
}

/* An immutable binding can share an existing token without moving its release
 * point. The source storage remains registered at its original LIFO position. */
static CGOwnedLocal cg_binding_borrow_owner(CG *cg, const ExprResult *result,
    const CGType *type, FengMutability mutability) {
    if (mutability == FENG_MUTABILITY_VAR ||
        !cg_types_equal(type, result->type)) return (CGOwnedLocal){0};
    return cg_stable_result_owner(cg, result);
}

/* Select one token to transfer at this exit only. No Local is globally marked
 * moved: other branches and all pre-exit unwind paths keep their own cleanup.
 * Defers may observe or modify source storage, including through raw aliases. */
static CGOwnedLocal cg_exit_result_owner(CG *cg, const ExprResult *result,
                                         const Scope *stop) {
    if (cg_result_ownership_source(cg, result) == NULL)
        return (CGOwnedLocal){0};
    bool exits_owner = false;
    for (const Scope *scope = cg->cur_scope; scope != stop && scope != NULL;
         scope = scope->parent) {
        if (scope->cleanup_probe) return (CGOwnedLocal){0};
        if (scope == result->local_owner.scope) exits_owner = true;
        for (size_t i = 0U; i < scope->count; ++i) {
            if (cgtype_is_defer(scope->items[i].type)) return (CGOwnedLocal){0};
        }
    }
    return exits_owner ? result->local_owner : (CGOwnedLocal){0};
}

/* Mark a fully initialized compiler temporary as a stable ownership source. */
static void cg_result_adopts_last_local(CG *cg, ExprResult *result) {
    if (cg->cur_scope->count == 0U) return;
    size_t index = cg->cur_scope->count - 1U;
    cg->cur_scope->items[index].ownership_is_stable = true;
    result->local_owner = (CGOwnedLocal){cg->cur_scope, index};
}

/* Read-only conversion consumers cannot mutate this storage. Reuse proven
 * stable storage; otherwise retain the original owning materialization. */
static char *cg_materialize_readonly_source(CG *cg, ExprResult *result,
                                            const char *prefix) {
    if (cg_stable_result_owner(cg, result).scope != NULL &&
        result->is_addressable && !result->is_storage_address)
        return strdup(result->c_expr);
    return cg_materialize_to_local(cg, result, prefix)
        ? strdup(result->c_expr) : NULL;
}
