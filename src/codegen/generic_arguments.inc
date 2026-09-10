/* Private implementation included after the type-reification helpers. The
 * generated argument packages are static C data passed separately from the
 * unchanged runtime descriptors. No runtime registry or allocation is used. */

/* One complete actual/constraint pair in its dependency owner's open scope. */
typedef struct CGGenericArgument {
    FengTypeRef *actual;
    FengTypeRef *constraint;
    char *key;
} CGGenericArgument;

/* Stable open slots reconstructed from source or imported callable metadata. */
struct CGGenericArgumentDomain {
    const FengReifiableDepSet *set;
    FengTypeParam *params;
    FengSlice *names;
    size_t param_count;
    CGGenericArgument *arguments;
    size_t argument_count;
};

/* A shell is registered before its children to preserve static graph cycles. */
struct CGClosedGenericArguments {
    char *key;
    char *name;
};

/* Pair a generated descriptor expression with its separately passed arguments. */
struct CGGenericArgumentBinding {
    char *descriptor;
    char *arguments;
};

/* Methods own independent dependency domains, including imported fit methods.
 * Constructors and member bindings instead use the original type domain. */
static const FengReifiableDepSet *cg_member_argument_domain(CG *cg,
    const FengDecl *owner, const FengTypeMember *member) {
    if (member != NULL && member->kind == FENG_TYPE_MEMBER_METHOD) {
        for (size_t i = 0U; i < cg->analysis->reifiable_dep_set_count; ++i) {
            const FengReifiableDepSet *set = &cg->analysis->reifiable_dep_sets[i];
            if (set->owner_member == member) return set;
        }
        return NULL;
    }
    return owner != NULL ? feng_semantic_lookup_reifiable_dep_set(cg->analysis, owner) : NULL;
}

/* Type methods and fits share the nominal owner's initialization domain. */
static const FengDecl *cg_argument_owner_type(CG *cg, const FengDecl *owner) {
    if (owner->kind == FENG_DECL_TYPE) return owner;
    if (owner->kind != FENG_DECL_FIT) return NULL;
    const GenericTypeDecl *type = cg_find_generic_type_decl_from_program(cg,
        owner->as.fit_decl.target, cg_find_decl_owner_program(cg, owner));
    return type != NULL ? type->decl : NULL;
}

/* Obtain the declaration-order parameter domain used by existing reification. */
static bool cg_argument_domain_parameters(CG *cg, CGGenericArgumentDomain *domain) {
    const FengDecl *owner = domain->set->owner_decl;
    const FengTypeParam *first = NULL, *second = NULL;
    size_t first_count = 0U, second_count = 0U;
    FengTypeParam implicit;
    if (owner->kind == FENG_DECL_FUNCTION) {
        first = owner->as.function_decl.type_params;
        first_count = owner->as.function_decl.type_param_count;
    } else {
        const FengDecl *type = cg_argument_owner_type(cg, owner);
        if (owner->kind == FENG_DECL_FIT) {
            if (type == NULL && feng_semantic_query_fit_implicit_type_param(
                    cg->analysis, owner, &implicit)) {
                first = &implicit;
                first_count = 1U;
            }
        }
        if (type != NULL && type->kind == FENG_DECL_TYPE) {
            first = type->as.type_decl.type_params;
            first_count = type->as.type_decl.type_param_count;
        }
        if (domain->set->owner_member != NULL) {
            second = domain->set->owner_member->as.callable.type_params;
            second_count = domain->set->owner_member->as.callable.type_param_count;
        }
    }
    domain->param_count = first_count + second_count;
    if (domain->param_count == 0U) return true;
    domain->params = calloc(domain->param_count, sizeof(*domain->params));
    domain->names = calloc(domain->param_count, sizeof(*domain->names));
    if (domain->params == NULL || domain->names == NULL) return false;
    for (size_t i = 0U; i < domain->param_count; ++i) {
        domain->params[i] = i < first_count ? first[i] : second[i - first_count];
        domain->names[i] = domain->params[i].name;
    }
    return true;
}

/* Source and constraint both participate in identity, including qualifiers. */
static char *cg_generic_argument_key(CG *cg, const FengTypeRef *actual,
    const FengTypeRef *constraint, const FengSlice *names, size_t count) {
    char *source = cg_callable_type_identity_key(cg, actual, names, count);
    char *target = cg_callable_type_identity_key(cg, constraint, names, count);
    Buf key;
    buf_init(&key);
    if (source != NULL && target != NULL) {
        buf_append_fmt(&key, "%zu:%s%zu:%s", strlen(source), source,
                       strlen(target), target);
    }
    free(source);
    free(target);
    return key.data;
}

/* Collect only constructed generic actuals; a bare parameter already has its
 * incoming descriptor, and fully closed actuals already use static records. */
static bool cg_argument_domain_append(CG *cg, CGGenericArgumentDomain *domain,
    const FengTypeRef *actual, const FengTypeRef *constraint) {
    CGTypeParamScope scope = {.first = domain->params, .first_count = domain->param_count};
    if (actual == NULL || !cg_type_ref_contains_type_param(actual, &scope) ||
        (actual->kind != FENG_TYPE_REF_ARRAY &&
         (actual->kind != FENG_TYPE_REF_NAMED || actual->as.named.type_arg_count == 0U))) {
        return true;
    }
    char *key = cg_generic_argument_key(cg, actual, constraint,
                                        domain->names, domain->param_count);
    if (key == NULL) return false;
    /* A selected constraint projection already provides the complete static
     * parameter record. Preserve that protocol without an extra hidden arg. */
    for (size_t i = 0U; constraint != NULL && i < domain->set->constraint_projection_count; ++i) {
        const FengConstraintProjectionDep *projection = &domain->set->constraint_projections[i];
        char *existing = cg_generic_argument_key(cg, projection->source_type_ref,
            projection->target_constraint_ref, domain->names, domain->param_count);
        if (existing == NULL) { free(key); return false; }
        bool matches = strcmp(existing, key) == 0;
        free(existing);
        if (matches) { free(key); return true; }
    }
    size_t slot = 0U;
    while (slot < domain->argument_count &&
           strcmp(domain->arguments[slot].key, key) < 0) ++slot;
    if (slot < domain->argument_count && strcmp(domain->arguments[slot].key, key) == 0) {
        free(key);
        return true;
    }
    CGGenericArgument value = {
        .actual = cg_type_ref_clone(actual),
        .constraint = constraint != NULL ? cg_type_ref_clone(constraint) : NULL,
        .key = key,
    };
    CGGenericArgument *grown = NULL;
    if (value.actual != NULL && (constraint == NULL || value.constraint != NULL) &&
        domain->argument_count < SIZE_MAX / sizeof(*grown)) {
        grown = realloc(domain->arguments, (domain->argument_count + 1U) * sizeof(*grown));
    }
    if (grown == NULL) {
        cg_type_ref_free(value.actual);
        cg_type_ref_free(value.constraint);
        free(key);
        return false;
    }
    domain->arguments = grown;
    memmove(grown + slot + 1U, grown + slot,
            (domain->argument_count - slot) * sizeof(*grown));
    grown[slot] = value;
    ++domain->argument_count;
    return true;
}

/* Reconstruct direct generic argument uses from the existing callable graph.
 * Constraint substitution is simultaneous in the callee's parameter domain. */
static CGGenericArgumentDomain *cg_generic_argument_domain(CG *cg,
    const FengReifiableDepSet *set) {
    if (set == NULL) return NULL;
    for (size_t i = 0U; i < cg->generic_argument_domain_count; ++i) {
        if (cg->generic_argument_domains[i]->set == set) return cg->generic_argument_domains[i];
    }
    CGGenericArgumentDomain *domain = calloc(1U, sizeof(*domain));
    if (domain == NULL) goto failure;
    domain->set = set;
    CGGenericArgumentDomain **grown = realloc(cg->generic_argument_domains,
        (cg->generic_argument_domain_count + 1U) * sizeof(*grown));
    if (grown == NULL) { free(domain); goto failure; }
    cg->generic_argument_domains = grown;
    grown[cg->generic_argument_domain_count++] = domain;
    if (!cg_argument_domain_parameters(cg, domain)) goto failure;
    for (size_t i = 0U; i < set->callable_dep_count; ++i) {
        const FengReifiableCallableDep *dep = &set->callable_deps[i];
        FengTypeParam *params = NULL;
        FengTypeRef **args = NULL;
        size_t count = 0U;
        if (dep->purpose != FENG_REIFIABLE_CALLABLE_DEP_DIRECT_CALL) continue;
        if (!cg_close_callable_dep_type_args(cg, dep, NULL, 0U, NULL,
                set->owner_decl->token, &params, &args, &count)) return NULL;
        size_t first = count - dep->callable_type_arg_count;
        if (dep->owner_type_decl == NULL && dep->fit_decl != NULL) first = 0U;
        bool ok = true;
        for (size_t j = first; j < count && ok; ++j) {
            FengTypeRef *constraint = params[j].constraint != NULL
                ? cg_type_ref_substitute(params[j].constraint, params, count, args) : NULL;
            ok = (params[j].constraint == NULL || constraint != NULL) &&
                cg_argument_domain_append(cg, domain, args[j], constraint);
            cg_type_ref_free(constraint);
        }
        for (size_t j = 0U; j < count; ++j) cg_type_ref_free(args[j]);
        free(args);
        free(params);
        if (!ok) goto failure;
    }
    return domain;
failure:
    (void)cg_fail(cg, set->owner_decl->token, "IE0001",
                  "codegen: cannot collect static generic arguments");
    return NULL;
}

/* A nominal type dependency may carry field/constructor argument uses. Arrays
 * themselves have no source initializers; nested nominal types have own slots. */
static const FengReifiableDepSet *cg_argument_type_domain(CG *cg, const FengTypeRef *ref) {
    if (ref == NULL || ref->kind != FENG_TYPE_REF_NAMED ||
        ref->as.named.type_arg_count == 0U) return NULL;
    const GenericTypeDecl *type = cg_find_generic_type_decl_from_program(cg, ref,
        ref->resolution_program != NULL ? ref->resolution_program : cg->cur_program);
    return type != NULL ? feng_semantic_lookup_reifiable_dep_set(cg->analysis, type->decl) : NULL;
}

/* Walk only the active compile-time path; a cycle without direct uses needs
 * no hidden argument, while cycles reaching a use share a static graph node. */
static bool cg_generic_arguments_needed_inner(CG *cg, const FengReifiableDepSet *set,
    const FengReifiableDepSet *const *parents, size_t depth) {
    if (set == NULL) return false;
    for (size_t i = 0U; i < depth; ++i) if (parents[i] == set) return false;
    CGGenericArgumentDomain *domain = cg_generic_argument_domain(cg, set);
    if (domain == NULL) return false;
    if (domain->argument_count != 0U) return true;
    const FengReifiableDepSet **path = malloc((depth + 1U) * sizeof(*path));
    if (path == NULL) {
        (void)cg_fail(cg, set->owner_decl->token, "IE0001", "codegen: out of memory");
        return false;
    }
    if (depth != 0U) memcpy(path, parents, depth * sizeof(*path));
    path[depth] = set;
    bool needed = false;
    for (size_t i = 0U; i < set->callable_dep_count && !needed; ++i) {
        if (set->callable_deps[i].purpose == FENG_REIFIABLE_CALLABLE_DEP_DIRECT_CALL) {
            needed = cg_generic_arguments_needed_inner(cg,
                cg_callable_dep_set(cg, &set->callable_deps[i]), path, depth + 1U);
        }
    }
    for (size_t i = 0U; i < set->dep_count && !needed; ++i) {
        needed = cg_generic_arguments_needed_inner(cg,
            cg_argument_type_domain(cg, set->deps[i].type_ref), path, depth + 1U);
    }
    const FengDecl *owner_type = cg_argument_owner_type(cg, set->owner_decl);
    if (!needed && set->owner_member != NULL && owner_type != NULL) {
        needed = cg_generic_arguments_needed_inner(cg,
            feng_semantic_lookup_reifiable_dep_set(cg->analysis, owner_type), path, depth + 1U);
    }
    free(path);
    return needed;
}

/* Query the existing dependency graph before emitting a shared C signature. */
static bool cg_generic_arguments_needed(CG *cg, const FengReifiableDepSet *set) {
    return cg_generic_arguments_needed_inner(cg, set, NULL, 0U);
}

/* Preserve the same ordering as the descriptor arrays, even when distinct
 * open slots close to identical records or do not need an argument package. */
typedef struct CGGenericArgumentChild {
    size_t index;
    char *key;
    char *expression;
} CGGenericArgumentChild;

/* Release temporary graph edges after their static initializers are emitted. */
static void cg_free_argument_children(CGGenericArgumentChild *children, size_t count) {
    for (size_t i = 0U; i < count; ++i) {
        free(children[i].key);
        free(children[i].expression);
    }
    free(children);
}

/* Sort owner-local edges by their existing open dependency keys. */
static int cg_compare_argument_children(const void *a, const void *b) {
    return strcmp(((const CGGenericArgumentChild *)a)->key,
                  ((const CGGenericArgumentChild *)b)->key);
}

/* Close a type initialization domain through the same type substitution used
 * to create the runtime descriptor; the argument package remains separate. */
static bool cg_close_type_arguments(CG *cg, const FengReifiableDepSet *set,
    const FengTypeRef *ref, const FengTypeParam *params, size_t count,
    FengTypeRef *const *args, const FengProgram *program, FengToken blame, char **out) {
    if (!cg_generic_arguments_needed(cg, set)) {
        *out = strdup("NULL");
        return *out != NULL;
    }
    FengTypeRef *closed = cg_type_ref_substitute(ref, params, count, args);
    bool ok = closed != NULL && closed->kind == FENG_TYPE_REF_NAMED &&
        cg_emit_closed_generic_arguments(cg, set, set->owner_decl->as.type_decl.type_params,
            set->owner_decl->as.type_decl.type_param_count, closed->as.named.type_args,
            program, blame, out);
    cg_type_ref_free(closed);
    return ok;
}

/* Emit one of the three parallel child arrays using the existing slot order. */
static bool cg_emit_generic_argument_children(CG *cg, const CGGenericArgumentDomain *domain,
    const FengTypeParam *params, size_t count, FengTypeRef *const *args,
    const FengProgram *program, FengToken blame, const char *name, size_t category,
    size_t *out_count) {
    const FengReifiableDepSet *set = domain->set;
    size_t capacity = category == 0U ? set->callable_dep_count : set->dep_count;
    CGGenericArgumentChild *children = capacity != 0U ? calloc(capacity, sizeof(*children)) : NULL;
    size_t used = 0U;
    bool ok = capacity == 0U || children != NULL;
    for (size_t i = 0U; i < capacity && ok; ++i) {
        if (category == 0U && !cg_callable_dep_requires_slot(cg, &set->callable_deps[i])) continue;
        if (category != 0U && set->deps[i].kind != (category == 1U
                ? FENG_REIFIABLE_DEP_KIND_MANAGED : FENG_REIFIABLE_DEP_KIND_AGGREGATE)) continue;
        CGGenericArgumentChild *child = &children[used++];
        child->index = i;
        child->key = category == 0U
            ? cg_callable_dep_sort_key(cg, &set->callable_deps[i], domain->names, domain->param_count)
            : cg_reifiable_sort_key(set->deps[i].type_ref, domain->names, domain->param_count, true);
        ok = child->key != NULL;
    }
    if (ok && used > 1U) qsort(children, used, sizeof(*children), cg_compare_argument_children);
    for (size_t i = 0U; i < used && ok; ++i) {
        CGGenericArgumentChild *child = &children[i];
        if (category == 0U) {
            const FengReifiableCallableDep *dep = &set->callable_deps[child->index];
            const FengReifiableDepSet *callee = cg_callable_dep_set(cg, dep);
            if (dep->purpose != FENG_REIFIABLE_CALLABLE_DEP_DIRECT_CALL ||
                !cg_generic_arguments_needed(cg, callee)) {
                child->expression = strdup("NULL");
                ok = child->expression != NULL;
                continue;
            }
            FengTypeParam *callee_params = NULL;
            FengTypeRef **callee_args = NULL;
            size_t callee_count = 0U;
            ok = cg_close_callable_dep_type_args(cg, dep, params, count, args, blame,
                    &callee_params, &callee_args, &callee_count) &&
                cg_emit_closed_generic_arguments(cg, callee, callee_params, callee_count,
                    callee_args, cg_find_decl_owner_program(cg, callee->owner_decl),
                    blame, &child->expression);
            for (size_t j = 0U; j < callee_count; ++j) cg_type_ref_free(callee_args[j]);
            free(callee_args);
            free(callee_params);
        } else {
            const FengTypeRef *ref = set->deps[child->index].type_ref;
            ok = cg_close_type_arguments(cg, cg_argument_type_domain(cg, ref), ref,
                params, count, args, program, blame, &child->expression);
        }
    }
    bool has_child_arguments = false;
    for (size_t i = 0U; ok && i < used; ++i) {
        if (strcmp(children[i].expression, "NULL") != 0) has_child_arguments = true;
    }
    if (ok && has_child_arguments) {
        const char *suffix = category == 0U ? "callable" : category == 1U ? "type" : "aggregate";
        buf_append_fmt(&cg->statics, "static const FengGenericArguments *const %s__%s[] = {", name, suffix);
        for (size_t i = 0U; i < used; ++i) {
            buf_append_fmt(&cg->statics, "%s%s", i == 0U ? "" : ", ", children[i].expression);
        }
        buf_append_cstr(&cg->statics, "};\n");
    }
    *out_count = has_child_arguments ? used : 0U;
    cg_free_argument_children(children, used);
    return ok;
}

/* Materialize a separate static argument graph. Existing descriptors are
 * never extended, reinterpreted, or used as storage for these hidden args. */
static bool cg_emit_closed_generic_arguments(CG *cg, const FengReifiableDepSet *set,
    const FengTypeParam *params, size_t count, FengTypeRef *const *args,
    const FengProgram *program, FengToken blame, char **out) {
    *out = NULL;
    if (!cg_generic_arguments_needed(cg, set)) {
        *out = strdup("NULL");
        return *out != NULL;
    }
    CGGenericArgumentDomain *domain = cg_generic_argument_domain(cg, set);
    Buf key;
    buf_init(&key);
    buf_append_fmt(&key, "%p", (const void *)set);
    for (size_t i = 0U; i < count; ++i) {
        char *arg_key = cg_callable_type_identity_key(cg, args[i], NULL, 0U);
        if (arg_key == NULL) { buf_free(&key); return false; }
        buf_append_fmt(&key, "|%zu:%s", strlen(arg_key), arg_key);
        free(arg_key);
    }
    if (key.data == NULL || domain == NULL) { buf_free(&key); return false; }
    CGClosedGenericArguments *node = NULL;
    for (size_t i = 0U; i < cg->closed_generic_argument_count; ++i) {
        if (strcmp(cg->closed_generic_arguments[i]->key, key.data) == 0) {
            node = cg->closed_generic_arguments[i];
            break;
        }
    }
    if (node != NULL) {
        Buf expression;
        buf_init(&expression);
        buf_append_fmt(&expression, "&%s", node->name);
        buf_free(&key);
        *out = expression.data;
        return *out != NULL;
    }
    node = calloc(1U, sizeof(*node));
    CGClosedGenericArguments **grown = realloc(cg->closed_generic_arguments,
        (cg->closed_generic_argument_count + 1U) * sizeof(*grown));
    if (node == NULL || grown == NULL) {
        free(node);
        if (grown != NULL) cg->closed_generic_arguments = grown;
        buf_free(&key);
        return false;
    }
    cg->closed_generic_arguments = grown;
    node->key = key.data;
    Buf name;
    buf_init(&name);
    buf_append_fmt(&name, "_feng_generic_args_%zu", cg->closed_generic_argument_count);
    node->name = name.data;
    grown[cg->closed_generic_argument_count++] = node;
    if (node->name == NULL) return false;
    buf_append_fmt(&cg->headers, "static const FengGenericArguments %s;\n", node->name);
    char **descriptors = domain->argument_count != 0U
        ? calloc(domain->argument_count, sizeof(*descriptors)) : NULL;
    bool ok = domain->argument_count == 0U || descriptors != NULL;
    for (size_t i = 0U; i < domain->argument_count && ok; ++i) {
        const CGGenericArgument *dep = &domain->arguments[i];
        FengTypeRef *actual_ref = NULL, *constraint_ref = NULL;
        CGType *actual = NULL, *constraint = NULL;
        ok = cg_close_reified_use_type(cg, dep->actual, params, count, args,
                program, blame, &actual_ref, &actual) &&
            (dep->constraint == NULL || cg_close_reified_use_type(cg, dep->constraint,
                params, count, args, program, blame, &constraint_ref, &constraint)) &&
            cg_generic_descriptor_expr(cg, actual, constraint != NULL ? constraint->user_spec : NULL,
                                        &blame, &descriptors[i]);
        cg_type_ref_free(actual_ref);
        cg_type_ref_free(constraint_ref);
        cgtype_free(actual);
        cgtype_free(constraint);
    }
    size_t child_counts[3] = {0U, 0U, 0U};
    for (size_t category = 0U; category < 3U && ok; ++category) {
        ok = cg_emit_generic_argument_children(cg, domain, params, count, args,
            program, blame, node->name, category, &child_counts[category]);
    }
    char *owner_arguments = NULL;
    const FengDecl *owner_type = cg_argument_owner_type(cg, set->owner_decl);
    if (ok && set->owner_member != NULL && owner_type != NULL) {
        const FengReifiableDepSet *owner = feng_semantic_lookup_reifiable_dep_set(cg->analysis, owner_type);
        ok = cg_emit_closed_generic_arguments(cg, owner, owner_type->as.type_decl.type_params,
            owner_type->as.type_decl.type_param_count, args,
            cg_find_decl_owner_program(cg, owner_type), blame, &owner_arguments);
    }
    if (ok && domain->argument_count != 0U) {
        buf_append_fmt(&cg->statics, "static const FengGenericParamDescriptor *const %s__params[] = {", node->name);
        for (size_t i = 0U; i < domain->argument_count; ++i) {
            buf_append_fmt(&cg->statics, "%s%s", i == 0U ? "" : ", ", descriptors[i]);
        }
        buf_append_cstr(&cg->statics, "};\n");
    }
    if (ok) {
        buf_append_fmt(&cg->statics, "static const FengGenericArguments %s = {", node->name);
        if (domain->argument_count != 0U) buf_append_fmt(&cg->statics, ".params = %s__params, ", node->name);
        const char *fields[] = {"callable", "type", "aggregate"};
        for (size_t category = 0U; category < 3U; ++category) {
            if (child_counts[category] != 0U) buf_append_fmt(&cg->statics, ".%s_deps = %s__%s, ",
                fields[category], node->name, fields[category]);
        }
        buf_append_fmt(&cg->statics, ".owner = %s};\n", owner_arguments != NULL ? owner_arguments : "NULL");
        Buf expression;
        buf_init(&expression);
        buf_append_fmt(&expression, "&%s", node->name);
        *out = expression.data;
        ok = *out != NULL;
    }
    for (size_t i = 0U; i < domain->argument_count; ++i) free(descriptors != NULL ? descriptors[i] : NULL);
    free(descriptors);
    free(owner_arguments);
    return ok;
}

/* Remember the static pair produced at one closed reification entrance. */
static bool cg_bind_generic_arguments(CG *cg, const char *descriptor,
    const char *arguments, FengToken blame) {
    if (strcmp(arguments, "NULL") == 0) return true;
    for (size_t i = 0U; i < cg->generic_argument_binding_count; ++i) {
        CGGenericArgumentBinding *binding = &cg->generic_argument_bindings[i];
        if (strcmp(binding->descriptor, descriptor) == 0) {
            return strcmp(binding->arguments, arguments) == 0 || cg_fail(cg, blame,
                "IE0002", "codegen: inconsistent static generic argument binding");
        }
    }
    char *descriptor_copy = strdup(descriptor), *arguments_copy = strdup(arguments);
    CGGenericArgumentBinding *grown = realloc(cg->generic_argument_bindings,
        (cg->generic_argument_binding_count + 1U) * sizeof(*grown));
    if (descriptor_copy == NULL || arguments_copy == NULL || grown == NULL) {
        free(descriptor_copy);
        free(arguments_copy);
        if (grown != NULL) cg->generic_argument_bindings = grown;
        return cg_fail(cg, blame, "IE0001", "codegen: out of memory");
    }
    cg->generic_argument_bindings = grown;
    grown[cg->generic_argument_binding_count++] = (CGGenericArgumentBinding){descriptor_copy, arguments_copy};
    return true;
}

/* Match only expressions emitted by the existing fixed-slot descriptor
 * helpers. This preserves their source/category/index without runtime lookup. */
static char *cg_generic_arguments_for_descriptor(CG *cg, const char *descriptor) {
    if (descriptor == NULL) return NULL;
    for (size_t i = 0U; i < cg->generic_argument_binding_count; ++i) {
        if (strcmp(cg->generic_argument_bindings[i].descriptor, descriptor) == 0)
            return strdup(cg->generic_argument_bindings[i].arguments);
    }
    if (cg->generic_reified_use_deps == NULL) return NULL;
    if (strcmp(descriptor, "_desc") == 0 || strcmp(descriptor, "_func_desc") == 0)
        return strdup("_generic_args");
    if (strcmp(descriptor, "_td") == 0 || strcmp(descriptor, "_type_desc") == 0) {
        return strdup(cg->generic_callable_dep_via_desc ? "_generic_args->owner" : "_generic_args");
    }
    const size_t counts[] = {cg->generic_callable_dep_count, cg->generic_type_method_rtd_count,
                             cg->generic_type_method_rad_count};
    const char *fields[] = {"callable", "type", "agg"};
    const char *argument_fields[] = {"callable", "type", "aggregate"};
    for (size_t category = 0U; category < 3U; ++category) {
        for (size_t slot = 0U; slot < counts[category]; ++slot) {
            for (size_t cast = 0U; cast < (category == 2U ? 2U : 1U); ++cast) {
                Buf expression;
                buf_init(&expression);
                buf_append_fmt(&expression, "%s%s->reified_%s_deps[%zu]",
                    cast != 0U ? "(const FengAggregateDescriptor *)" : "",
                    cg->generic_callable_dep_via_desc ? "_desc" : "_td", fields[category], slot);
                bool matches = expression.data != NULL && strcmp(expression.data, descriptor) == 0;
                buf_free(&expression);
                if (matches) {
                    buf_append_fmt(&expression, "_generic_args->%s_deps[%zu]", argument_fields[category], slot);
                    return expression.data;
                }
            }
        }
    }
    return NULL;
}

/* Append a hidden package only to a shared entry whose domain needs it. */
static bool cg_append_generic_arguments(CG *cg, Buf *out, const FengReifiableDepSet *set,
    const char *descriptor, FengToken blame) {
    if (!cg_generic_arguments_needed(cg, set)) return !cg->failed;
    char *expression = cg_generic_arguments_for_descriptor(cg, descriptor);
    if (expression == NULL) return cg_fail(cg, blame, "IE0002",
        "codegen: missing static generic arguments for descriptor '%s'", descriptor);
    buf_append_fmt(out, ", %s", expression);
    free(expression);
    return true;
}

/* Prebind the compiler-owned package before an adapter emits its call. */
static bool cg_bind_closed_generic_arguments(CG *cg, const FengReifiableDepSet *set,
    const FengTypeParam *params, size_t count, FengTypeRef *const *args,
    const FengProgram *program, FengToken blame, const char *descriptor_name) {
    char *arguments = NULL;
    if (!cg_emit_closed_generic_arguments(cg, set, params, count, args, program,
                                         blame, &arguments)) return false;
    Buf descriptor;
    buf_init(&descriptor);
    buf_append_fmt(&descriptor, "&%s", descriptor_name);
    bool ok = descriptor.data != NULL &&
        cg_bind_generic_arguments(cg, descriptor.data, arguments, blame);
    free(arguments);
    buf_free(&descriptor);
    return ok;
}

/* Append a package associated with a file-scope descriptor symbol. */
static bool cg_append_static_generic_arguments(CG *cg, Buf *out,
    const FengReifiableDepSet *set, const char *descriptor_name, FengToken blame) {
    if (!cg_generic_arguments_needed(cg, set)) return !cg->failed;
    Buf descriptor;
    buf_init(&descriptor);
    buf_append_fmt(&descriptor, "&%s", descriptor_name);
    bool ok = cg_append_generic_arguments(cg, out, set, descriptor.data, blame);
    buf_free(&descriptor);
    return ok;
}

/* Recover the existing selected callable identity, never a runtime lookup. */
static bool cg_finish_member_generic_call(CG *cg, Buf *out,
    const FengTypeMember *member, const FengExpr *call) {
    const FengReifiableDepSet *set = cg_member_argument_domain(cg, NULL, member);
    if (cg_generic_arguments_needed(cg, set)) {
        char *descriptor = NULL;
        if (cg->in_generic_fn) {
            descriptor = cg_callable_descriptor_expr_for_call(cg, call, call->token);
        } else {
            const FengResolvedCallable *resolved = &call->as.call.resolved_callable;
            FengReifiableCallableDep dep = {
                .kind = resolved->kind, .function_decl = resolved->function_decl,
                .owner_type_decl = resolved->owner_type_decl, .member = resolved->member,
                .fit_decl = resolved->fit_decl,
                .owner_instance_type_ref = resolved->owner_instance_type_ref,
                .callable_type_args = resolved->callable_type_args,
                .callable_type_arg_count = resolved->callable_type_arg_count,
            };
            if (!cg_emit_closed_callable_dep_expr(cg, &dep, NULL, 0U, NULL,
                    cg->cur_program, call->token, &descriptor)) return false;
        }
        bool ok = descriptor != NULL &&
            cg_append_generic_arguments(cg, out, set, descriptor, call->token);
        free(descriptor);
        if (!ok) return false;
    }
    buf_append_cstr(out, ")");
    return !cg->failed;
}

/* An open constructed actual forwards its preformed static parameter record.
 * Closed actuals and direct type parameters are handled by the caller. */
static bool cg_open_generic_argument_expr(CG *cg, const CGType *actual,
    const UserSpec *constraint, FengToken blame, char **out) {
    CGGenericArgumentDomain *domain = cg_generic_argument_domain(cg, cg->generic_reified_use_deps);
    if (domain == NULL) return cg_fail(cg, blame, "IE0002", "codegen: open generic argument has no dependency owner");
    FengTypeRef *actual_ref = cg_type_ref_from_cgtype(cg, actual, blame);
    CGType constraint_type = {.kind = CG_TYPE_SPEC, .user_spec = constraint};
    FengTypeRef *constraint_ref = constraint != NULL ? cg_type_ref_from_cgtype(cg, &constraint_type, blame) : NULL;
    char *key = actual_ref != NULL && (constraint == NULL || constraint_ref != NULL)
        ? cg_generic_argument_key(cg, actual_ref, constraint_ref, domain->names, domain->param_count) : NULL;
    cg_type_ref_free(actual_ref);
    cg_type_ref_free(constraint_ref);
    bool found = false;
    for (size_t i = 0U; key != NULL && i < domain->argument_count; ++i) {
        if (strcmp(domain->arguments[i].key, key) == 0) {
            Buf expression;
            buf_init(&expression);
            buf_append_fmt(&expression, "_generic_args->params[%zu]", i);
            *out = expression.data;
            found = *out != NULL;
            break;
        }
    }
    if (!found) (void)cg_fail(cg, blame, "IE0002", "codegen: uncollected open generic argument '%s'", key != NULL ? key : "<unknown>");
    free(key);
    return found;
}

/* Release compiler caches; generated packages have program-static lifetime. */
static void cg_free_generic_arguments(CG *cg) {
    for (size_t i = 0U; i < cg->generic_argument_domain_count; ++i) {
        CGGenericArgumentDomain *domain = cg->generic_argument_domains[i];
        for (size_t j = 0U; j < domain->argument_count; ++j) {
            cg_type_ref_free(domain->arguments[j].actual);
            cg_type_ref_free(domain->arguments[j].constraint);
            free(domain->arguments[j].key);
        }
        free(domain->arguments);
        free(domain->params);
        free(domain->names);
        free(domain);
    }
    free(cg->generic_argument_domains);
    for (size_t i = 0U; i < cg->closed_generic_argument_count; ++i) {
        free(cg->closed_generic_arguments[i]->key);
        free(cg->closed_generic_arguments[i]->name);
        free(cg->closed_generic_arguments[i]);
    }
    free(cg->closed_generic_arguments);
    for (size_t i = 0U; i < cg->generic_argument_binding_count; ++i) {
        free(cg->generic_argument_bindings[i].descriptor);
        free(cg->generic_argument_bindings[i].arguments);
    }
    free(cg->generic_argument_bindings);
}
