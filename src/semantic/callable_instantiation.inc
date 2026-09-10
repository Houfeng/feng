/* One declaration-ordered binding domain for a callable and its owner.
 * Caller arguments are borrowed, so substitution never recursively captures
 * their names as parameters belonging to the declaration being instantiated. */
typedef struct CallableTypeBindings {
    FengCallableSignature signature;
    FengTypeParam *params;
    FengTypeRef **args;
    size_t owner_count;
} CallableTypeBindings;

/* Build owner/fit and method bindings together, including open method slots. */
static bool callable_type_bindings_init(
    ResolveContext *context, const FengCallableSignature *callable,
    const FengDecl *owner_decl, const FengDecl *fit_decl, InferredExprType owner,
    const FengTypeRef *const *method_args, size_t method_count,
    CallableTypeBindings *bindings) {
    const FengTypeParam *owner_params = NULL;
    FengTypeRef *const *owner_args = NULL;
    FengTypeParam array_param;
    FengTypeRef *array_arg = NULL;
    *bindings = (CallableTypeBindings){0};
    bindings->signature = *callable;
    if (owner.type_ref != NULL && owner.type_ref->kind == FENG_TYPE_REF_NAMED &&
        owner_decl != NULL) {
        if (owner_decl->kind == FENG_DECL_TYPE) {
            owner_params = owner_decl->as.type_decl.type_params;
            bindings->owner_count = owner_decl->as.type_decl.type_param_count;
        } else if (owner_decl->kind == FENG_DECL_SPEC) {
            owner_params = owner_decl->as.spec_decl.type_params;
            bindings->owner_count = owner_decl->as.spec_decl.type_param_count;
        }
        if (bindings->owner_count != owner.type_ref->as.named.type_arg_count) {
            bindings->owner_count = 0U;
        }
        owner_args = owner.type_ref->as.named.type_args;
    } else if (owner.type_ref != NULL && owner.type_ref->kind == FENG_TYPE_REF_ARRAY &&
               fit_decl_collect_array_local_type_param(context, fit_decl, &array_param)) {
        owner_params = &array_param;
        bindings->owner_count = 1U;
        array_arg = owner.type_ref->as.inner;
        owner_args = &array_arg;
    }
    size_t count = bindings->owner_count + callable->type_param_count;
    bindings->params = count > 0U ? calloc(count, sizeof(*bindings->params)) : NULL;
    bindings->args = count > 0U ? calloc(count, sizeof(*bindings->args)) : NULL;
    if (count > 0U && (bindings->params == NULL || bindings->args == NULL)) {
        free(bindings->params);
        free(bindings->args);
        return false;
    }
    for (size_t i = 0U; i < bindings->owner_count; ++i) {
        bindings->params[i] = owner_params[i];
        bindings->args[i] = owner_args[i];
    }
    for (size_t i = 0U; i < callable->type_param_count; ++i) {
        size_t slot = bindings->owner_count + i;
        bindings->params[slot] = callable->type_params[i];
        if (method_args != NULL && method_count == callable->type_param_count) {
            bindings->args[slot] = (FengTypeRef *)method_args[i];
        }
    }
    bindings->signature.type_params = bindings->params;
    bindings->signature.type_param_count = count;
    return true;
}

/* Release only the binding table; source declarations and actuals are borrowed. */
static void callable_type_bindings_free(CallableTypeBindings *bindings) {
    free(bindings->params);
    free(bindings->args);
}

/* Clone both domains in one traversal. A missing actual referenced by the
 * source yields NULL, preserving deferred constraint inference. */
static FengTypeRef *clone_type_ref_for_callable_instance(
    ResolveContext *context, const FengCallableSignature *callable,
    const FengDecl *owner_decl, const FengDecl *fit_decl, InferredExprType owner,
    const FengTypeRef *const *method_args, size_t method_count,
    const FengTypeRef *source) {
    CallableTypeBindings bindings;
    if (source == NULL) return NULL;
    if (!callable_type_bindings_init(context, callable, owner_decl, fit_decl,
            owner, method_args, method_count, &bindings)) return NULL;
    size_t count = bindings.owner_count;
    if (method_args != NULL && method_count == callable->type_param_count) {
        count += method_count;
    }
    FengTypeRef *result = clone_type_ref_substituting_type_params(
        context->program, source, bindings.params, count, bindings.args);
    callable_type_bindings_free(&bindings);
    const FengDecl *provider = fit_decl != NULL ? fit_decl : owner_decl;
    if (provider != NULL) {
        bind_type_ref_resolution_program(result,
            find_decl_provider_program(context->analysis, provider));
    }
    return result;
}

/* Keep substituted member surfaces alive for Semantic's recorded call sites. */
static const FengTypeRef *substitute_type_ref_for_callable_instance(
    ResolveContext *context, const FengCallableSignature *callable,
    const FengDecl *owner_decl, const FengDecl *fit_decl, InferredExprType owner,
    const FengTypeRef *const *method_args, size_t method_count,
    const FengTypeRef *source) {
    if (source == NULL) return NULL;
    FengTypeRef *result = clone_type_ref_for_callable_instance(context, callable,
        owner_decl, fit_decl, owner, method_args, method_count, source);
    if (result == NULL || !analysis_track_synthetic_type_ref(context->analysis, result)) {
        free_synthetic_type_ref(result);
        return source;
    }
    return result;
}
