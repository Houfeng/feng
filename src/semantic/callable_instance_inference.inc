/* Infer against the original declaration while owner slots are already bound.
 * This prevents a caller's U inside an owner argument from becoming the
 * method's unrelated U. A NULL argument selects result-target inference. */
static bool callable_infer_instance_type_args(
    ResolveContext *context, const FengCallableSignature *callable,
    const FengDecl *owner_decl, const FengDecl *fit_decl, InferredExprType owner,
    const FengTypeRef *source, const FengExpr *argument,
    FengTypeRef **type_args, bool *owned_type_args) {
    CallableTypeBindings bindings;
    if (source == NULL) return argument == NULL;
    if (!callable_type_bindings_init(context, callable, owner_decl, fit_decl,
            owner, (const FengTypeRef *const *)type_args,
            callable->type_param_count, &bindings)) return false;
    size_t count = bindings.signature.type_param_count;
    bool *owned = count > 0U ? calloc(count, sizeof(*owned)) : NULL;
    FengTypeRef *scoped = clone_type_ref_for_inference(source);
    if ((count > 0U && owned == NULL) || scoped == NULL) {
        free(owned);
        free_synthetic_type_ref(scoped);
        callable_type_bindings_free(&bindings);
        return false;
    }
    bind_type_ref_resolution_program(scoped, callable_source_program(context, callable));
    bool ok = argument != NULL
        ? callable_collect_type_args_from_arg_expr(context, &bindings.signature,
            scoped, argument, bindings.args, owned)
        : callable_collect_type_args_from_target(context, false, &bindings.signature,
            scoped, bindings.args, owned);
    for (size_t i = 0U; i < callable->type_param_count; ++i) {
        size_t slot = bindings.owner_count + i;
        type_args[i] = bindings.args[slot];
        owned_type_args[i] = owned_type_args[i] || owned[slot];
    }
    free_synthetic_type_ref(scoped);
    free(owned);
    callable_type_bindings_free(&bindings);
    return ok;
}
