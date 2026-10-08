/* Included by analyzer.c. Deferred checks are reduced to finite predicates on
 * parameter roots and signature visibility. Propagating these predicates (not
 * recursively materializing generic bodies) also terminates for recursive and
 * mutually recursive generic calls whose type arguments grow. */

/* A parameter slot, a nominal declaration, or a non-nominal concrete root. */
typedef struct FriendValidationAtom {
    const FengDecl *decl;
    size_t parameter;
    bool open;
    bool unresolved;
} FriendValidationAtom;

/* One owner-bound predicate. Signature predicates recursively flatten their
 * target, so substitution cannot create an unbounded predicate expression. */
typedef struct FriendValidationCondition {
    bool signature;
    FriendValidationAtom subject;
    FriendValidationAtom target;
    const FengTypeRef *subject_type_ref; /* Borrowed diagnostic spelling, excluded from predicate identity. */
    const FengTypeRef *target_type_ref; /* Original signature spelling, never a temporary substituted root. */
    const struct FengFriendMemberInfo *origin;
    const FengSemanticModule *consumer_module;
} FriendValidationCondition;

/* A declaration's finite set of conditions, in owner-then-callable slots. */
typedef struct FriendValidationSummary {
    const FengDecl *decl;
    const FengTypeMember *member;
    ResolveContext context;
    FengTypeParam *parameters;
    size_t parameter_count;
    size_t owner_parameter_count;
    FriendValidationCondition *conditions;
    size_t condition_count;
    size_t condition_capacity;
} FriendValidationSummary;

/* The summaries are stable after initial enumeration; only condition arrays grow. */
typedef struct FriendValidationGraph {
    FengSemanticAnalysis *analysis;
    FriendValidationSummary *summaries;
    size_t count;
    size_t capacity;
    bool changed;
} FriendValidationGraph;

/* Recreate only the declaration's lexical lookup environment, never its body. */
static bool friend_validation_prepare_context(ResolveContext *context) {
    size_t alias_capacity = 0U;
    VisibleTypeEntry *visible = NULL;
    AliasEntry *aliases = NULL;
    ImportedModuleEntry *imports = NULL;
    context->pointer_size = context->analysis->pointer_size;
    bool ok = context->program != NULL && context->module != NULL &&
        build_friend_query_visible_types(context->analysis, context->module, context->program,
            &visible, &context->visible_type_count) &&
        build_program_aliases(context->analysis, context->program,
            &aliases, &context->alias_count, &alias_capacity) &&
        build_friend_query_imported_modules(context->analysis, context->program,
            &imports, &context->imported_module_count);
    context->visible_types = visible;
    context->aliases = aliases;
    context->imported_modules = imports;
    return ok;
}

/* Dispose temporary query tables independently of retained semantic facts. */
static void friend_validation_free_context(ResolveContext *context) {
    free((void *)context->visible_types);
    free((void *)context->aliases);
    free((void *)context->imported_modules);
}

/* Compare atoms independently of their diagnostic origin. */
static bool friend_validation_atoms_equal(FriendValidationAtom a, FriendValidationAtom b) {
    return a.decl == b.decl && a.parameter == b.parameter && a.open == b.open && a.unresolved == b.unresolved;
}

/* Extract a root without treating a method parameter as an owner parameter. */
static FriendValidationAtom friend_validation_atom(FriendValidationSummary *summary,
                                                   const FengTypeRef *ref) {
    FriendValidationAtom atom = {.parameter = SIZE_MAX};
    if (ref == NULL || ref->kind != FENG_TYPE_REF_NAMED) return atom;
    if (ref->as.named.segment_count == 1U && is_builtin_type_name(ref->as.named.segments[0])) return atom;
    if (ref->resolution_decl == NULL && ref->as.named.segment_count == 1U &&
        ref->as.named.type_arg_count == 0U && !is_builtin_type_name(ref->as.named.segments[0])) {
        atom.open = true;
        for (size_t i = 0U; i < summary->parameter_count; ++i) {
            if (slice_equals(ref->as.named.segments[0], summary->parameters[i].name)) {
                atom.parameter = i;
                return atom;
            }
        }
        atom.decl = resolve_type_ref_decl(&summary->context, ref);
        atom.open = false;
        atom.unresolved = atom.decl == NULL;
        return atom;
    }
    atom.decl = resolve_type_ref_decl(&summary->context, ref);
    atom.unresolved = atom.decl == NULL;
    return atom;
}

/* Discard tautologies and deduplicate normalized predicates to reach a fixpoint. */
static bool friend_validation_append(FriendValidationGraph *graph,
                                     FriendValidationSummary *summary,
                                     FriendValidationCondition condition) {
    if ((!condition.signature && !condition.subject.open && condition.subject.decl != NULL &&
         condition.subject.decl->kind == FENG_DECL_TYPE) ||
        (condition.signature && !condition.target.open && !condition.target.unresolved && condition.target.decl == NULL)) return true;
    for (size_t i = 0U; i < summary->condition_count; ++i) {
        const FriendValidationCondition *item = &summary->conditions[i];
        if (item->signature == condition.signature && item->origin == condition.origin &&
            item->consumer_module == condition.consumer_module &&
            friend_validation_atoms_equal(item->subject, condition.subject) &&
            friend_validation_atoms_equal(item->target, condition.target)) return true;
    }
    graph->changed = true;
    return append_raw((void **)&summary->conditions, &summary->condition_count,
        &summary->condition_capacity, sizeof(condition), &condition);
}

/* Flatten complete signature types, including generic arguments and containers. */
static bool friend_validation_signature(FriendValidationGraph *graph,
                                        FriendValidationSummary *summary,
                                        FriendValidationCondition condition,
                                        const FengTypeRef *ref) {
    if (ref == NULL) return true;
    if (ref->kind != FENG_TYPE_REF_NAMED) {
        return friend_validation_signature(graph, summary, condition, ref->as.inner);
    }
    condition.signature = true;
    if (condition.target_type_ref == NULL) condition.target_type_ref = ref;
    condition.target = friend_validation_atom(summary, ref);
    if ((!condition.target.open || condition.target.parameter != SIZE_MAX) &&
        !friend_validation_append(graph, summary, condition)) return false;
    for (size_t i = 0U; i < ref->as.named.type_arg_count; ++i) {
        if (!friend_validation_signature(graph, summary, condition, ref->as.named.type_args[i])) return false;
    }
    return true;
}

/* Find an already enumerated lexical declaration domain. */
static FriendValidationSummary *friend_validation_find(FriendValidationGraph *graph,
    const FengDecl *decl, const FengTypeMember *member) {
    for (size_t i = 0U; i < graph->count; ++i) {
        if (graph->summaries[i].decl == decl && graph->summaries[i].member == member) return &graph->summaries[i];
    }
    return NULL;
}

/* Enumerate owner and method bindings before following any graph edge. */
static bool friend_validation_add_summary(FriendValidationGraph *graph,
    const FengDecl *decl, const FengTypeMember *member) {
    if (friend_validation_find(graph, decl, member) != NULL) return true;
    FriendValidationSummary summary = {0};
    const FengTypeParam *owner_params = NULL;
    const FengCallableSignature *callable = NULL;
    FengTypeParam array_param = {0};
    summary.decl = decl;
    summary.member = member;
    summary.context.analysis = graph->analysis;
    summary.context.module = find_decl_provider_module(graph->analysis, decl);
    summary.context.program = find_decl_provider_program(graph->analysis, decl);
    if (!friend_validation_prepare_context(&summary.context)) {
        friend_validation_free_context(&summary.context);
        return false;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        owner_params = decl->as.type_decl.type_params;
        summary.owner_parameter_count = decl->as.type_decl.type_param_count;
    } else if (decl->kind == FENG_DECL_SPEC) {
        owner_params = decl->as.spec_decl.type_params;
        summary.owner_parameter_count = decl->as.spec_decl.type_param_count;
    } else if (decl->kind == FENG_DECL_FIT) {
        const FengDecl *target = resolve_type_ref_decl(&summary.context, decl->as.fit_decl.target);
        if (target != NULL && target->kind == FENG_DECL_TYPE) {
            owner_params = target->as.type_decl.type_params;
            summary.owner_parameter_count = target->as.type_decl.type_param_count;
        } else if (feng_semantic_query_fit_implicit_type_param(graph->analysis, decl, &array_param)) {
            owner_params = &array_param;
            summary.owner_parameter_count = 1U;
        }
    } else if (decl->kind == FENG_DECL_FUNCTION) {
        callable = &decl->as.function_decl;
    }
    if (member != NULL && member->kind == FENG_TYPE_MEMBER_METHOD) callable = &member->as.callable;
    summary.parameter_count = summary.owner_parameter_count + (callable != NULL ? callable->type_param_count : 0U);
    if (summary.parameter_count > 0U) {
        summary.parameters = calloc(summary.parameter_count, sizeof(*summary.parameters));
        if (summary.parameters == NULL) {
            friend_validation_free_context(&summary.context);
            return false;
        }
        if (summary.owner_parameter_count > 0U) memcpy(summary.parameters, owner_params,
            summary.owner_parameter_count * sizeof(*summary.parameters));
        if (callable != NULL && callable->type_param_count > 0U) memcpy(
            summary.parameters + summary.owner_parameter_count, callable->type_params,
            callable->type_param_count * sizeof(*summary.parameters));
    }
    if (!append_raw((void **)&graph->summaries, &graph->count, &graph->capacity, sizeof(summary), &summary)) {
        free(summary.parameters);
        friend_validation_free_context(&summary.context);
        return false;
    }
    return true;
}

/* Bind a signature in its provider scope, then project owner parameters into
 * the accessing fit. Method parameters remain local to the annotated method. */
static bool friend_validation_bound_signature(FriendValidationGraph *graph,
    FriendValidationSummary *target, FriendValidationCondition condition,
    const FengTypeRef *ref, const FengTypeRef *instance) {
    if (ref == NULL) return true;
    if (ref->kind != FENG_TYPE_REF_NAMED) {
        return friend_validation_bound_signature(graph, target, condition, ref->as.inner, instance);
    }
    const struct FengFriendMemberInfo *info = condition.origin;
    if (info->member->kind == FENG_TYPE_MEMBER_METHOD &&
        friend_type_ref_owner_param_index(ref, info->member->as.callable.type_params,
            info->member->as.callable.type_param_count, NULL)) return true;
    condition.target_type_ref = ref;
    FriendValidationSummary *owner = friend_validation_find(graph, info->owner_decl, NULL);
    bind_type_ref_nominal_declarations(&owner->context, ref, owner->parameters,
                                       owner->parameter_count, owner->context.program);
    FengTypeRef root = *ref;
    root.as.named.type_args = NULL;
    root.as.named.type_arg_count = 0U;
    FengTypeRef *bound = NULL;
    if (instance != NULL && owner->owner_parameter_count > 0U) {
        FengTypeRef *element[1];
        FengTypeRef *const *args = NULL;
        if (instance->kind == FENG_TYPE_REF_NAMED &&
            instance->as.named.type_arg_count == owner->owner_parameter_count) {
            args = instance->as.named.type_args;
        } else if (instance->kind == FENG_TYPE_REF_ARRAY && owner->owner_parameter_count == 1U) {
            element[0] = instance->as.inner;
            args = element;
        }
        if (args == NULL) return false;
        bound = clone_type_ref_substituting_type_params(target->context.program, &root,
            owner->parameters, owner->owner_parameter_count, args);
        if (bound == NULL) return false;
    }
    bool ok = friend_validation_signature(graph, target, condition, bound != NULL ? bound : &root);
    free_synthetic_type_ref(bound);
    for (size_t i = 0U; i < ref->as.named.type_arg_count && ok; ++i) {
        ok = friend_validation_bound_signature(graph, target, condition, ref->as.named.type_args[i], instance);
    }
    return ok;
}

/* Borrow one predicate and binding while visiting the complete member surface. */
typedef struct FriendValidationSignature {
    FriendValidationGraph *graph;
    FriendValidationSummary *summary;
    FriendValidationCondition condition;
    const FengTypeRef *instance;
} FriendValidationSignature;

/* Inferred nominal facts already carry identity and need no temporary syntax. */
static bool friend_validation_signature_surface(void *user,
    const FengSemanticTypeFact *fact) {
    const FriendValidationSignature *signature = user;
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_DECL && fact->type_decl != NULL) {
        FriendValidationCondition condition = signature->condition;
        condition.signature = true;
        condition.target = (FriendValidationAtom){.decl = fact->type_decl, .parameter = SIZE_MAX};
        condition.target_type_ref = NULL;
        return friend_validation_append(signature->graph, signature->summary, condition);
    }
    return fact->kind != FENG_SEMANTIC_TYPE_FACT_TYPE_REF ||
        friend_validation_bound_signature(signature->graph, signature->summary,
            signature->condition, fact->type_ref, signature->instance);
}

/* Share signature enumeration while retaining the condition graph's binding. */
static bool friend_validation_member_signature(FriendValidationGraph *graph,
    FriendValidationSummary *summary, FriendValidationCondition condition,
    const FengTypeRef *instance) {
    FriendValidationSignature signature = {graph, summary, condition, instance};
    return visit_member_signature_types(graph->analysis, condition.origin->member,
        SIGNATURE_PARAMETERS_FIRST, friend_validation_signature_surface, &signature);
}

/* Preserve each friend's declaration obligation even when its member is unused. */
static bool friend_validation_seed_member(FriendValidationGraph *graph,
    const struct FengFriendMemberInfo *info) {
    FriendValidationSummary *summary = friend_validation_find(graph, info->owner_decl, NULL);
    if (summary == NULL) return false;
    for (size_t f = 0U; f < info->friend_type_count; ++f) {
        FriendValidationCondition condition = {0};
        condition.origin = info;
        condition.subject = friend_validation_atom(summary, info->friend_types[f]->source_ref);
        condition.subject_type_ref = info->friend_types[f]->source_ref;
        condition.target.parameter = SIZE_MAX;
        if (!friend_validation_append(graph, summary, condition)) return false;
        if (!friend_validation_member_signature(graph, summary, condition, NULL)) return false;
    }
    return true;
}

/* Apply one edge to normalized conditions. Only target parameters can expand
 * into multiple predicates, one for each nested signature type. */
static bool friend_validation_transfer(FriendValidationGraph *graph,
    FriendValidationSummary *target, const FriendValidationSummary *source,
    const FengTypeRef *const *arguments, size_t argument_count) {
    size_t count = source->condition_count;
    for (size_t i = 0U; i < count; ++i) {
        FriendValidationCondition condition = source->conditions[i];
        if (condition.subject.open && condition.subject.parameter < argument_count) {
            condition.subject_type_ref = arguments[condition.subject.parameter];
            condition.subject = friend_validation_atom(target, condition.subject_type_ref);
        }
        if (condition.signature && condition.target.open && condition.target.parameter < argument_count) {
            if (!friend_validation_signature(graph, target, condition, arguments[condition.target.parameter])) return false;
        } else if (!friend_validation_append(graph, target, condition)) return false;
    }
    return true;
}

/* An effective generic fit shares its target's binding but retains its own
 * member declarations and obligations. Invisible fits contribute no conditions. */
static bool friend_validation_fit_edges(FriendValidationGraph *graph,
    FriendValidationSummary *target, const FengTypeRef *ref) {
    const FengDecl *owner = ref->kind == FENG_TYPE_REF_NAMED
        ? friend_validation_atom(target, ref).decl : NULL;
    for (size_t i = 0U; i < graph->count; ++i) {
        FriendValidationSummary *fit = &graph->summaries[i];
        if (fit->decl->kind != FENG_DECL_FIT || fit->member != NULL || fit->condition_count == 0U ||
            !fit_decl_is_visible_from(&target->context, fit->context.module, fit->decl) ||
            !fit_decl_target_matches_owner_type(&fit->context, fit->decl, owner,
                                                inferred_expr_type_from_type_ref(ref))) continue;
        const FengTypeRef *element[1] = {ref->kind == FENG_TYPE_REF_ARRAY ? ref->as.inner : NULL};
        const FengTypeRef *const *args = ref->kind == FENG_TYPE_REF_NAMED
            ? (const FengTypeRef *const *)ref->as.named.type_args : element;
        size_t count = ref->kind == FENG_TYPE_REF_NAMED ? ref->as.named.type_arg_count : 1U;
        if (count == fit->parameter_count && !friend_validation_transfer(graph, target, fit, args, count)) return false;
    }
    return true;
}

/* A type use imports that owner's obligations and those of nested arguments. */
static bool friend_validation_type_edge(FriendValidationGraph *graph,
    FriendValidationSummary *target, const FengTypeRef *ref) {
    if (ref == NULL) return true;
    if (!friend_validation_fit_edges(graph, target, ref)) return false;
    if (ref->kind != FENG_TYPE_REF_NAMED) return friend_validation_type_edge(graph, target, ref->as.inner);
    FriendValidationAtom atom = friend_validation_atom(target, ref);
    FriendValidationSummary *source = friend_validation_find(graph, atom.decl, NULL);
    if (source != NULL && source->parameter_count == ref->as.named.type_arg_count &&
        !friend_validation_transfer(graph, target, source,
            (const FengTypeRef *const *)ref->as.named.type_args, ref->as.named.type_arg_count)) return false;
    for (size_t i = 0U; i < ref->as.named.type_arg_count; ++i) {
        if (!friend_validation_type_edge(graph, target, ref->as.named.type_args[i])) return false;
    }
    return true;
}

/* Callable edges bind owner slots before callable slots, including array fits. */
static bool friend_validation_call_edge(FriendValidationGraph *graph,
    FriendValidationSummary *target, const FengReifiableCallableDep *call) {
    const FengDecl *owner = call->function_decl != NULL ? call->function_decl :
        (call->fit_decl != NULL ? call->fit_decl : call->owner_type_decl);
    FriendValidationSummary *source = friend_validation_find(graph, owner, call->member);
    if (!friend_validation_type_edge(graph, target, call->owner_instance_type_ref)) return false;
    for (size_t i = 0U; i < call->callable_type_arg_count; ++i) {
        if (!friend_validation_type_edge(graph, target, call->callable_type_args[i])) return false;
    }
    if (source == NULL) return true;
    const FengTypeRef **arguments = source->parameter_count > 0U
        ? calloc(source->parameter_count, sizeof(*arguments)) : NULL;
    if (source->parameter_count > 0U && arguments == NULL) return false;
    size_t count = 0U;
    const FengTypeRef *instance = call->owner_instance_type_ref;
    if (source->owner_parameter_count > 0U && instance != NULL) {
        if (instance->kind == FENG_TYPE_REF_NAMED && instance->as.named.type_arg_count == source->owner_parameter_count) {
            for (size_t i = 0U; i < source->owner_parameter_count; ++i) arguments[count++] = instance->as.named.type_args[i];
        } else if (instance->kind == FENG_TYPE_REF_ARRAY && source->owner_parameter_count == 1U) {
            arguments[count++] = instance->as.inner;
        }
    }
    for (size_t i = 0U; i < call->callable_type_arg_count && count < source->parameter_count; ++i) {
        arguments[count++] = call->callable_type_args[i];
    }
    bool ok = count != source->parameter_count || friend_validation_transfer(graph, target, source, arguments, count);
    free(arguments);
    return ok;
}

/* Keep the use-site diagnostic connected to its source or imported grant. */
static bool friend_validation_related(const FriendValidationCondition *condition,
    FengSemanticError *errors, size_t error_count) {
    const FengTypeMember *member = condition->origin->member;
    FengToken token = member->token;
    for (size_t i = 0U; i < member->annotation_count; ++i) {
        if (member->annotations[i].builtin_kind == FENG_ANNOTATION_FRIEND) {
            token = member->annotations[i].token;
            break;
        }
    }
    FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD
        ? member->as.field.name : member->as.callable.name;
    return append_latest_error_related_location(errors, error_count,
        condition->origin->owner_program->path, token,
        format_message("@friend authorization for member '%.*s' is declared here",
                       (int)name.length, name.data));
}

/* Report resolved failures at the use that completed the binding. */
static bool friend_validation_report(FriendValidationGraph *graph,
    FriendValidationSummary *summary, const struct FengGenericValidationUse *use,
    FengSemanticError **errors, size_t *error_count, size_t *error_capacity) {
    for (size_t i = 0U; i < summary->condition_count; ++i) {
        const FriendValidationCondition *condition = &summary->conditions[i];
        if (condition->subject.open) continue;
        if (!condition->signature) {
            if (condition->subject.decl != NULL && condition->subject.decl->kind == FENG_DECL_TYPE) continue;
            char *argument = format_type_ref_name(condition->subject_type_ref);
            if (argument == NULL) return false;
            bool reported = append_error(errors, error_count, error_capacity, use->program->path, use->token,
                "AE1336", format_message("instantiated @friend argument '%s' must resolve to a concrete type", argument));
            free(argument);
            return reported && friend_validation_related(condition, *errors, *error_count);
        }
        if (condition->target.unresolved) {
            char *name = format_type_ref_name(condition->target_type_ref);
            if (name == NULL) return false;
            bool reported = append_error(errors, error_count, error_capacity, use->program->path, use->token,
                "AE1338", format_message("instantiated @friend signature type '%s' cannot be resolved from package metadata", name));
            free(name);
            return reported && friend_validation_related(condition, *errors, *error_count);
        }
        if (condition->target.open || condition->target.decl == NULL) continue;
        const FengSemanticModule *consumer = condition->consumer_module != NULL ? condition->consumer_module :
            find_decl_provider_module(graph->analysis, condition->subject.decl);
        if (friend_signature_decl_visible(graph->analysis, condition->origin,
                consumer, condition->target.decl)) continue;
        FengSlice member_name = condition->origin->member->kind == FENG_TYPE_MEMBER_FIELD
            ? condition->origin->member->as.field.name : condition->origin->member->as.callable.name;
        FengSlice hidden_name = decl_typeish_name(condition->target.decl);
        return append_error(errors, error_count, error_capacity, use->program->path, use->token,
            "AE1338", format_message("instantiated @friend member '%.*s' exposes type '%.*s' that is not accessible in the authorized context",
                (int)member_name.length, member_name.data, (int)hidden_name.length, hidden_name.data)) &&
            friend_validation_related(condition, *errors, *error_count);
    }
    return true;
}

/* Build a finite predicate fixpoint, then evaluate every lexical source use. */
static bool validate_generic_friend_conditions(FengSemanticAnalysis *analysis,
    FengSemanticError **errors, size_t *error_count, size_t *error_capacity) {
    if (analysis->generic_validation_record_failed) return false;
    if (analysis->friend_member_info_count == 0U) return true;
    FriendValidationGraph graph = {0};
    graph.analysis = analysis;
    bool ok = true;
    for (size_t i = 0U; i < analysis->friend_member_info_count && ok; ++i) {
        ok = friend_validation_add_summary(&graph, analysis->friend_member_infos[i].owner_decl, NULL);
    }
    for (size_t i = 0U; i < analysis->friend_member_info_count && ok; ++i) {
        const struct FengFriendMemberInfo *info = &analysis->friend_member_infos[i];
        for (size_t a = 0U; a < info->fit_access_count && ok; ++a) {
            const FriendFitAccess *access = &info->fit_accesses[a];
            ok = friend_validation_add_summary(&graph, access->fit_decl, access->callable_member);
        }
    }
    for (size_t i = 0U; i < analysis->reifiable_dep_set_count && ok; ++i) {
        const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
        ok = friend_validation_add_summary(&graph, set->owner_decl, set->owner_member);
    }
    for (size_t i = 0U; i < analysis->friend_member_info_count && ok; ++i) {
        ok = friend_validation_seed_member(&graph, &analysis->friend_member_infos[i]);
    }
    for (size_t i = 0U; i < analysis->friend_member_info_count && ok; ++i) {
        const struct FengFriendMemberInfo *info = &analysis->friend_member_infos[i];
        for (size_t a = 0U; a < info->fit_access_count && ok; ++a) {
            const FriendFitAccess *access = &info->fit_accesses[a];
            FriendValidationSummary *summary = friend_validation_find(
                &graph, access->fit_decl, access->callable_member);
            FriendValidationCondition condition = {0};
            condition.subject.parameter = SIZE_MAX;
            condition.target.parameter = SIZE_MAX;
            condition.origin = info;
            condition.consumer_module = access->module;
            ok = friend_validation_member_signature(&graph, summary, condition,
                                                     access->owner_instance_type_ref);
        }
    }
    do {
        graph.changed = false;
        for (size_t i = 0U; i < analysis->reifiable_dep_set_count && ok; ++i) {
            const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
            FriendValidationSummary *summary = friend_validation_find(&graph, set->owner_decl, set->owner_member);
            for (size_t d = 0U; d < set->dep_count && ok; ++d) {
                ok = friend_validation_type_edge(&graph, summary, set->deps[d].type_ref);
            }
        }
        /* Callable edges are shared facts; do not copy them into check sets. */
        for (size_t i = 0U; i < analysis->reifiable_dep_set_count && ok; ++i) {
            const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
            FriendValidationSummary *summary = friend_validation_find(&graph, set->owner_decl, set->owner_member);
            for (size_t d = 0U; d < set->callable_dep_count && ok; ++d) {
                ok = friend_validation_call_edge(&graph, summary, &set->callable_deps[d]);
            }
        }
    } while (ok && graph.changed);
    /* Fixed obligations are declaration errors even if the body is never called. */
    for (size_t i = 0U; i < graph.count && ok && *error_count == 0U; ++i) {
        FriendValidationSummary *summary = &graph.summaries[i];
        if (summary->context.module->origin != FENG_SEMANTIC_MODULE_ORIGIN_LOCAL) continue;
        struct FengGenericValidationUse use = {0};
        use.program = summary->context.program;
        use.module = summary->context.module;
        use.token = summary->member != NULL ? summary->member->token : summary->decl->token;
        ok = friend_validation_report(&graph, summary, &use, errors, error_count, error_capacity);
    }
    for (size_t i = 0U; i < analysis->generic_validation_use_count && ok && *error_count == 0U; ++i) {
        const struct FengGenericValidationUse *use = &analysis->generic_validation_uses[i];
        FriendValidationSummary site = {0};
        site.context.analysis = analysis;
        site.context.module = use->module;
        site.context.program = use->program;
        site.parameters = use->parameters;
        site.parameter_count = use->parameter_count;
        ok = friend_validation_prepare_context(&site.context);
        if (ok && use->type_ref != NULL) ok = friend_validation_type_edge(&graph, &site, use->type_ref);
        if (ok && use->call != NULL) {
            FengReifiableCallableDep call = {0};
            if (use->call->kind == FENG_EXPR_CALL) {
                const FengResolvedCallable *resolved = &use->call->as.call.resolved_callable;
                call.kind = resolved->kind;
                call.function_decl = resolved->function_decl;
                call.member = resolved->member;
                call.owner_type_decl = resolved->owner_type_decl;
                call.fit_decl = resolved->fit_decl;
                call.owner_instance_type_ref = resolved->owner_instance_type_ref;
                call.callable_type_args = resolved->callable_type_args;
                call.callable_type_arg_count = resolved->callable_type_arg_count;
            } else {
                const FengSpecCoercionSite *coercion = feng_semantic_lookup_spec_coercion_site(analysis, use->call);
                if (coercion != NULL && coercion->form == FENG_SPEC_COERCION_FORM_CALLABLE) {
                    call.function_decl = coercion->callable_decl;
                    call.member = coercion->callable_member;
                    call.owner_type_decl = coercion->callable_owner_type_decl;
                    call.fit_decl = coercion->callable_fit_decl;
                    call.owner_instance_type_ref = coercion->callable_receiver_type_ref;
                    call.callable_type_args = coercion->callable_type_args;
                    call.callable_type_arg_count = coercion->callable_type_arg_count;
                }
            }
            ok = friend_validation_call_edge(&graph, &site, &call);
        }
        if (ok) ok = friend_validation_report(&graph, &site, use, errors, error_count, error_capacity);
        free(site.conditions);
        friend_validation_free_context(&site.context);
        if (*error_count > 0U) break;
    }
    for (size_t i = 0U; i < graph.count; ++i) {
        free(graph.summaries[i].parameters);
        free(graph.summaries[i].conditions);
        friend_validation_free_context(&graph.summaries[i].context);
    }
    free(graph.summaries);
    return ok;
}
