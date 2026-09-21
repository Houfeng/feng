#include "semantic/exception_effects.h"
#include "semantic/exception_solver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A source callable, global value initializer, or implicit constructor.
 * All pointers into the AST/analysis are borrowed. */
typedef struct ExceptionSource {
    const void *key;
    const FengProgram *program;
    const FengDecl *owner;
    const FengTypeMember *member;
    const FengCallableSignature *signature;
    const FengExpr *lambda;
    FengTypeParam *parameters;
    size_t parameter_count, owner_parameter_count;
    uint32_t function, instance;
    bool imported, built, abi;
    FengExceptionTemplate summary;
} ExceptionSource;

/* Resolved value origins do not require repeating namespace lookup later. */
typedef struct ExceptionValueOrigin {
    const FengExpr *site;
    const void *source;
    bool type_parameter;
    FengSlice parameter_name;
} ExceptionValueOrigin;

/* A call keeps its own instantiated result and its enclosing capture mask. */
typedef struct ExceptionCallSite {
    const FengExpr *expr;
    ExceptionSource *source;
    uint32_t invocation, escaping;
    uint32_t effects, escaping_effects;
} ExceptionCallSite;

/* A call's propagation root at a cleanup boundary, before outer catches. */
typedef struct ExceptionBoundaryCall {
    const FengExpr *expression;
    uint32_t root, effects;
} ExceptionBoundaryCall;

/* Declaration-local cleanup facts share the ordinary callable solver. */
typedef struct ExceptionDeferSite {
    const FengStmt *statement;
    ExceptionSource *source;
    uint32_t root, effects;
    ExceptionBoundaryCall *calls;
    size_t call_count;
} ExceptionDeferSite;

/* Import each provider graph once, preserving recursive/shared identities. */
typedef struct ExceptionImportedGraph {
    const FengExceptionGraph *source;
    uint32_t *functions, *nodes;
} ExceptionImportedGraph;

/* Owned semantic sidecar; no runtime or generated-code representation. */
typedef struct FengExceptionAnalysis {
    FengSemanticAnalysis *analysis;
    FengExceptionGraph *graph;
    ExceptionSource **sources;
    size_t source_count, source_capacity;
    ExceptionValueOrigin *origins;
    size_t origin_count, origin_capacity;
    ExceptionCallSite *calls;
    size_t call_count, call_capacity;
    ExceptionDeferSite *defers;
    size_t defer_count, defer_capacity;
    ExceptionImportedGraph *imports;
    size_t import_count, import_capacity;
} FengExceptionAnalysis;

/* Lexical bindings carry abstract values, not runtime values. */
typedef struct ExceptionBinding {
    FengSlice name;
    uint32_t value, type;
    bool mutable;
} ExceptionBinding;

/* An expression has separate evaluation effects and returned value facts. */
typedef struct ExceptionExpression {
    uint32_t effects, value;
    uint32_t receiver;
} ExceptionExpression;

/* One callable traversal; nested blocks restore lexical bindings/catch state. */
typedef struct ExceptionBuilder {
    FengExceptionAnalysis *data;
    ExceptionSource *source;
    ExceptionBinding *bindings;
    size_t binding_count, binding_capacity;
    uint32_t caught, returns, self;
} ExceptionBuilder;

/* Copy a source slice into owned metadata. */
static char *ee_slice(FengSlice value) {
    char *text = malloc(value.length + 1U);
    if (text != NULL) {
        memcpy(text, value.data, value.length);
        text[value.length] = '\0';
    }
    return text;
}

/* Compare lexical identifiers only while resolving declaration ownership. */
static bool ee_same(FengSlice a, FengSlice b) {
    return a.length == b.length && (a.length == 0U || memcmp(a.data, b.data, a.length) == 0);
}

/* Grow a sidecar vector with overflow checking. */
static bool ee_grow(void **data, size_t *capacity, size_t count, size_t width) {
    if (count <= *capacity)
        return true;
    size_t next = *capacity != 0U ? *capacity : 16U;
    while (next < count && next <= SIZE_MAX / 2U)
        next *= 2U;
    if (next < count || next > SIZE_MAX / width)
        return false;
    void *result = realloc(*data, next * width);
    if (result == NULL)
        return false;
    *data = result;
    *capacity = next;
    return true;
}

/* Lazily allocate origins before the post-pass creates its graph. */
static FengExceptionAnalysis *ee_data(FengSemanticAnalysis *analysis) {
    if (analysis->exception_analysis == NULL) {
        analysis->exception_analysis = calloc(1U, sizeof(*analysis->exception_analysis));
        if (analysis->exception_analysis != NULL)
            analysis->exception_analysis->analysis = analysis;
    }
    return analysis->exception_analysis;
}

/* Preserve namespace resolution without treating type targets as runtime values. */
static bool ee_record_origin(const FengSemanticAnalysis *analysis, const FengExpr *site,
                             const void *source_node, bool type_parameter) {
    if (source_node == NULL)
        return true;
    FengExceptionAnalysis *data = ee_data((FengSemanticAnalysis *)analysis);
    if (data == NULL)
        return false;
    for (size_t i = 0U; i < data->origin_count; ++i) {
        if (data->origins[i].site == site) {
            data->origins[i].source = type_parameter ? NULL : source_node;
            data->origins[i].type_parameter = type_parameter;
            data->origins[i].parameter_name =
                type_parameter ? ((const FengTypeParam *)source_node)->name : (FengSlice){0};
            return true;
        }
    }
    if (!ee_grow((void **)&data->origins, &data->origin_capacity, data->origin_count + 1U,
                 sizeof(*data->origins)))
        return false;
    data->origins[data->origin_count++] =
        (ExceptionValueOrigin){site, type_parameter ? NULL : source_node, type_parameter,
                               type_parameter ? ((const FengTypeParam *)source_node)->name : (FengSlice){0}};
    return true;
}

bool feng_semantic_exception_record_value_source(const FengSemanticAnalysis *analysis, const FengExpr *site,
                                                 const void *source_node) {
    return ee_record_origin(analysis, site, source_node, false);
}

bool feng_semantic_exception_record_type_parameter(const FengSemanticAnalysis *analysis, const FengExpr *site,
                                                   const FengTypeParam *parameter) {
    return ee_record_origin(analysis, site, parameter, true);
}

/* Query an already-resolved source identity. */
static const ExceptionValueOrigin *ee_origin(const FengExceptionAnalysis *data, const FengExpr *expr) {
    for (size_t i = 0U; i < data->origin_count; ++i)
        if (data->origins[i].site == expr)
            return &data->origins[i];
    return NULL;
}

/* Look up the unique callable template belonging to an AST node. */
static ExceptionSource *ee_source(const FengExceptionAnalysis *data, const void *key) {
    for (size_t i = 0U; i < data->source_count; ++i)
        if (data->sources[i]->key == key)
            return data->sources[i];
    return NULL;
}

/* Obtain the declared nominal name; fit providers use their target separately. */
static FengSlice ee_decl_name(const FengDecl *decl) {
    switch (decl->kind) {
    case FENG_DECL_TYPE:
        return decl->as.type_decl.name;
    case FENG_DECL_SPEC:
        return decl->as.spec_decl.name;
    case FENG_DECL_ENUM:
        return decl->as.enum_decl.name;
    case FENG_DECL_FUNCTION:
        return decl->as.function_decl.name;
    case FENG_DECL_GLOBAL_BINDING:
        return decl->as.binding.name;
    default:
        return (FengSlice){"<fit>", 5U};
    }
}

/* Build a fully qualified nominal name in the owning module. */
static char *ee_qualified(const FengProgram *program, FengSlice name) {
    size_t length = name.length;
    for (size_t i = 0U; i < program->module_segment_count; ++i)
        length += program->module_segments[i].length + 1U;
    char *text = malloc(length + 1U);
    if (text == NULL)
        return NULL;
    size_t at = 0U;
    for (size_t i = 0U; i < program->module_segment_count; ++i) {
        FengSlice part = program->module_segments[i];
        memcpy(text + at, part.data, part.length);
        at += part.length;
        text[at++] = '.';
    }
    memcpy(text + at, name.data, name.length);
    text[length] = '\0';
    return text;
}

/* Find a declaration by its canonical nominal name; visibility was checked
 * by the semantic resolver before this post-pass. */
static const FengDecl *ee_named_decl(FengExceptionAnalysis *data, const char *name, size_t arity) {
    for (size_t i = 0U; i < data->analysis->module_count; ++i) {
        const FengSemanticModule *module = &data->analysis->modules[i];
        for (size_t p = 0U; p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0U; d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind != FENG_DECL_TYPE && decl->kind != FENG_DECL_SPEC &&
                    decl->kind != FENG_DECL_ENUM)
                    continue;
                size_t count = decl->kind == FENG_DECL_TYPE   ? decl->as.type_decl.type_param_count
                               : decl->kind == FENG_DECL_SPEC ? decl->as.spec_decl.type_param_count
                                                              : 0U;
                if (count != arity)
                    continue;
                char *candidate = ee_qualified(program, ee_decl_name(decl));
                if (candidate == NULL) {
                    data->graph->failed = true;
                    return NULL;
                }
                bool equal = candidate != NULL && strcmp(candidate, name) == 0;
                free(candidate);
                if (equal)
                    return decl;
            }
        }
    }
    return NULL;
}

/* Convert canonical type syntax once to declaration/slot terms. */
static uint32_t ee_type_canonical(ExceptionBuilder *b, const FengTypeRef *type) {
    FengExceptionGraph *g = b->data->graph;
    if (type == NULL)
        return feng_exception_unknown(g);
    if (type->kind != FENG_TYPE_REF_NAMED) {
        uint32_t inner = ee_type_canonical(b, type->as.inner);
        const char *name = type->kind == FENG_TYPE_REF_POINTER ? "*"
                           : type->array_element_writable      ? "[!]"
                                                               : "[]";
        return feng_exception_node(g, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0, name, &inner, 1U);
    }
    if (type->as.named.segment_count == 1U && type->as.named.type_arg_count == 0U) {
        for (size_t i = b->source->parameter_count; i > 0U; --i)
            if (ee_same(b->source->parameters[i - 1U].name, type->as.named.segments[0]))
                return feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER, b->source->function,
                                           (uint32_t)(i - 1U), 0, NULL, NULL, 0);
    }
    size_t length = 0U;
    for (size_t i = 0U; i < type->as.named.segment_count; ++i)
        length += type->as.named.segments[i].length + (i != 0U);
    char *name = malloc(length + 1U);
    size_t count = type->as.named.type_arg_count;
    uint32_t *args = count != 0U ? malloc(count * sizeof(*args)) : NULL;
    if (name == NULL || (count != 0U && args == NULL)) {
        free(name);
        free(args);
        g->failed = true;
        return 0U;
    }
    size_t at = 0U;
    for (size_t i = 0U; i < type->as.named.segment_count; ++i) {
        if (i != 0U)
            name[at++] = '.';
        FengSlice part = type->as.named.segments[i];
        memcpy(name + at, part.data, part.length);
        at += part.length;
    }
    name[at] = '\0';
    for (size_t i = 0U; i < count; ++i)
        args[i] = ee_type_canonical(b, type->as.named.type_args[i]);
    uint32_t result = feng_exception_node(g, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0, name, args, count);
    free(name);
    free(args);
    return result;
}

/* Reuse normal type resolution for aliases, builtins and provider identity. */
static uint32_t ee_type(ExceptionBuilder *b, const FengTypeRef *type) {
    if (type == NULL)
        return feng_exception_unknown(b->data->graph);
    const FengTypeRef *canonical = feng_semantic_canonical_reifiable_type_ref(
        b->data->analysis, b->source->owner, b->source->parameters, b->source->parameter_count, type);
    if (canonical == NULL) {
        b->data->graph->failed = true;
        return 0U;
    }
    return ee_type_canonical(b, canonical);
}

/* Construct the nominal type term for an already-resolved declaration. */
static uint32_t ee_decl_type(ExceptionBuilder *b, const FengDecl *decl) {
    const FengProgram *program = NULL;
    for (size_t m = 0U; m < b->data->analysis->module_count && program == NULL; ++m) {
        const FengSemanticModule *module = &b->data->analysis->modules[m];
        for (size_t p = 0U; p < module->program_count && program == NULL; ++p)
            for (size_t d = 0U; d < module->programs[p]->declaration_count; ++d)
                if (module->programs[p]->declarations[d] == decl) {
                    program = module->programs[p];
                    break;
                }
    }
    if (program == NULL)
        return feng_exception_unknown(b->data->graph);
    char *name = ee_qualified(program, ee_decl_name(decl));
    if (name == NULL) {
        b->data->graph->failed = true;
        return 0U;
    }
    uint32_t result = feng_exception_node(b->data->graph, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0, name, NULL, 0);
    free(name);
    return result;
}

/* Read the type inferred by the ordinary resolver, without guessing literals. */
static uint32_t ee_site_type(ExceptionBuilder *b, const void *site) {
    const FengSemanticTypeFact *fact = feng_semantic_lookup_type_fact(b->data->analysis, site);
    if (fact == NULL)
        return 0U;
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_TYPE_REF)
        return ee_type(b, fact->type_ref);
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_DECL)
        return ee_decl_type(b, fact->type_decl);
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_BUILTIN) {
        char *name = ee_slice(fact->builtin_name);
        if (name == NULL) {
            b->data->graph->failed = true;
            return 0U;
        }
        uint32_t result =
            feng_exception_node(b->data->graph, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0, name, NULL, 0);
        free(name);
        return result;
    }
    return 0U;
}

/* Lookup the nearest lexical binding, preserving shadowed identities. */
static ExceptionBinding *ee_binding(ExceptionBuilder *b, FengSlice name) {
    for (size_t i = b->binding_count; i > 0U; --i)
        if (ee_same(b->bindings[i - 1U].name, name))
            return &b->bindings[i - 1U];
    return NULL;
}

/* Add one lexical value; mutable callable locations are conservatively
 * unknown when their value can be changed through control flow or capture. */
static void ee_bind(ExceptionBuilder *b, FengSlice name, uint32_t value, uint32_t type, bool mutable) {
    if (!ee_grow((void **)&b->bindings, &b->binding_capacity, b->binding_count + 1U, sizeof(*b->bindings))) {
        b->data->graph->failed = true;
        return;
    }
    b->bindings[b->binding_count++] = (ExceptionBinding){name, value, type, mutable};
}

/* Return a value's static subject type without evaluating anything. */
static uint32_t ee_value_type(FengExceptionGraph *g, uint32_t type) {
    return type != 0U ? feng_exception_node(g, FENG_EXCEPTION_VALUE_TYPE, type, 0, 0, NULL, NULL, 0)
                      : feng_exception_unknown(g);
}

/* Allocate a declaration shell, or bind a provider-neutral imported graph. */
static ExceptionSource *ee_register(FengExceptionAnalysis *data, const FengSemanticModule *module,
                                    const FengProgram *program, const FengDecl *owner,
                                    const FengTypeMember *member, bool imported) {
    const void *key = member != NULL ? (const void *)member : (const void *)owner;
    ExceptionSource *existing = ee_source(data, key);
    if (existing != NULL)
        return existing;
    ExceptionSource *source = calloc(1U, sizeof(*source));
    if (source == NULL || !ee_grow((void **)&data->sources, &data->source_capacity, data->source_count + 1U,
                                   sizeof(*data->sources))) {
        free(source);
        data->graph->failed = true;
        return NULL;
    }
    source->key = key;
    source->program = program;
    source->owner = owner;
    source->member = member;
    source->imported = imported;
    source->signature = member != NULL && member->kind != FENG_TYPE_MEMBER_FIELD ? &member->as.callable
                        : member == NULL && owner->kind == FENG_DECL_FUNCTION    ? &owner->as.function_decl
                                                                                 : NULL;
    const FengTypeParam *owner_params = NULL;
    size_t on = 0U;
    FengTypeParam implicit = {0};
    if (owner->kind == FENG_DECL_TYPE) {
        owner_params = owner->as.type_decl.type_params;
        on = owner->as.type_decl.type_param_count;
    }
    if (owner->kind == FENG_DECL_SPEC) {
        owner_params = owner->as.spec_decl.type_params;
        on = owner->as.spec_decl.type_param_count;
    }
    if (owner->kind == FENG_DECL_FIT && owner->as.fit_decl.target->kind == FENG_TYPE_REF_NAMED) {
        ExceptionBuilder lookup = {0};
        lookup.data = data;
        lookup.source = source;
        uint32_t target_type = ee_type(&lookup, owner->as.fit_decl.target);
        FengExceptionNode target = data->graph->nodes[target_type];
        const FengDecl *decl = target.kind == FENG_EXCEPTION_NAMED_TYPE
                                   ? ee_named_decl(data, target.name, target.arg_count)
                                   : NULL;
        if (decl != NULL && decl->kind == FENG_DECL_TYPE) {
            owner_params = decl->as.type_decl.type_params;
            on = decl->as.type_decl.type_param_count;
        }
    }
    if (owner->kind == FENG_DECL_FIT &&
        feng_semantic_query_fit_implicit_type_param(data->analysis, owner, &implicit)) {
        owner_params = &implicit;
        on = 1U;
    }
    size_t mn = source->signature != NULL ? source->signature->type_param_count : 0U;
    source->parameter_count = on + mn;
    source->owner_parameter_count = on;
    source->parameters = on + mn != 0U ? calloc(on + mn, sizeof(*source->parameters)) : NULL;
    char **names = on + mn != 0U ? calloc(on + mn, sizeof(*names)) : NULL;
    if (on + mn != 0U && (source->parameters == NULL || names == NULL))
        data->graph->failed = true;
    for (size_t i = 0U; i < on + mn && !data->graph->failed; ++i) {
        source->parameters[i] = i < on ? owner_params[i] : source->signature->type_params[i - on];
        names[i] = ee_slice(source->parameters[i].name);
        if (names[i] == NULL)
            data->graph->failed = true;
    }
    char *name = ee_qualified(program, ee_decl_name(owner));
    if (member != NULL && name != NULL) {
        FengTypeMember *const *members = owner->kind == FENG_DECL_TYPE ? owner->as.type_decl.members
                                         : owner->kind == FENG_DECL_SPEC
                                             ? owner->as.spec_decl.as.object.members
                                             : owner->as.fit_decl.members;
        size_t member_index = 0U;
        while (members[member_index] != member)
            ++member_index;
        FengSlice member_name =
            member->kind == FENG_TYPE_MEMBER_FIELD ? member->as.field.name : member->as.callable.name;
        size_t length = strlen(name) + member_name.length + 80U;
        char *full = malloc(length);
        if (full != NULL)
            snprintf(full, length, "%s[%zu].%.*s#%zu", name, on, (int)member_name.length, member_name.data,
                     member_index);
        free(name);
        name = full;
    }
    if (name == NULL)
        data->graph->failed = true;
    size_t captures = member != NULL && !member->is_static ? 1U : 0U;
    const FengExceptionTemplate *external =
        imported && module->get_exception_template != NULL
            ? module->get_exception_template(module->exception_metadata_user, key)
            : NULL;
    if (external != NULL && external->graph != NULL) {
        ExceptionImportedGraph *mapping = NULL;
        for (size_t i = 0U; i < data->import_count; ++i)
            if (data->imports[i].source == external->graph) {
                mapping = &data->imports[i];
                break;
            }
        if (mapping == NULL && ee_grow((void **)&data->imports, &data->import_capacity,
                                       data->import_count + 1U, sizeof(*data->imports))) {
            mapping = &data->imports[data->import_count++];
            *mapping = (ExceptionImportedGraph){0};
            mapping->source = external->graph;
            if (!feng_exception_graph_import(data->graph, external->graph, &mapping->functions,
                                             &mapping->nodes))
                mapping = NULL;
        }
        if (mapping == NULL)
            data->graph->failed = true;
        else
            source->function = mapping->functions[external->function];
        source->built = true;
    } else {
        source->function = feng_exception_function_add(
            data->graph, name, (const char *const *)names, on + mn,
            source->signature != NULL ? source->signature->param_count : 0U, captures);
    }
    if (names != NULL)
        for (size_t i = 0U; i < on + mn; ++i)
            free(names[i]);
    free(names);
    free(name);
    const FengAnnotation *annotations = member != NULL ? member->annotations : owner->annotations;
    size_t annotation_count = member != NULL ? member->annotation_count : owner->annotation_count;
    for (size_t i = 0U; i < annotation_count; ++i)
        if (source->signature != NULL && annotations[i].builtin_kind == FENG_ANNOTATION_ABI)
            source->abi = true;
    data->sources[data->source_count++] = source;
    return source;
}

/* Build an expression/block's abstract behavior after ordinary resolution. */
static ExceptionExpression ee_expr(ExceptionBuilder *b, const FengExpr *expr);
static ExceptionExpression ee_block(ExceptionBuilder *b, const FengBlock *block);
static uint32_t ee_stmt(ExceptionBuilder *b, const FengStmt *stmt);
static void ee_build_source(FengExceptionAnalysis *data, ExceptionSource *source);

/* Unify a validated fit target structurally, binding only its owner domain. */
static bool ee_match_owner(FengExceptionGraph *g, uint32_t pattern, uint32_t actual, uint32_t function,
                           uint32_t *arguments, size_t count) {
    FengExceptionNode p = g->nodes[pattern], a = g->nodes[actual];
    if (p.kind == FENG_EXCEPTION_TYPE_PARAMETER && p.x == function && p.y < count) {
        if (arguments[p.y] != 0U && arguments[p.y] != actual)
            return false;
        arguments[p.y] = actual;
        return true;
    }
    if (pattern == actual)
        return true;
    if (p.kind != FENG_EXCEPTION_NAMED_TYPE || a.kind != FENG_EXCEPTION_NAMED_TYPE ||
        p.arg_count != a.arg_count || strcmp(p.name, a.name) != 0)
        return false;
    for (size_t i = 0U; i < p.arg_count; ++i)
        if (!ee_match_owner(g, p.args[i], a.args[i], function, arguments, count))
            return false;
    return true;
}

/* Bind a fit through its complete target; nominal owners use declaration slots. */
static bool ee_owner_arguments(FengExceptionAnalysis *data, ExceptionSource *source, uint32_t type,
                               uint32_t *arguments) {
    FengExceptionGraph *g = data->graph;
    if (source->owner->kind == FENG_DECL_FIT) {
        ExceptionBuilder lookup = {0};
        lookup.data = data;
        lookup.source = source;
        uint32_t pattern = ee_type(&lookup, source->owner->as.fit_decl.target);
        return ee_match_owner(g, pattern, type, source->function, arguments, source->owner_parameter_count);
    }
    FengExceptionNode actual = g->nodes[type];
    if (actual.kind != FENG_EXCEPTION_NAMED_TYPE || actual.arg_count < source->owner_parameter_count)
        return false;
    for (size_t i = 0U; i < source->owner_parameter_count; ++i)
        arguments[i] = actual.args[i];
    return true;
}

/* Bind a resolved callable using both owner and method argument domains. */
static uint32_t ee_callable(ExceptionBuilder *b, const FengResolvedCallable *resolved, uint32_t receiver) {
    FengExceptionGraph *g = b->data->graph;
    const void *key = resolved->kind == FENG_RESOLVED_CALLABLE_FUNCTION
                          ? (const void *)resolved->function_decl
                          : (const void *)resolved->member;
    if (key == NULL && resolved->kind == FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR)
        key = resolved->owner_type_decl;
    ExceptionSource *target = ee_source(b->data, key);
    if (target == NULL)
        return feng_exception_unknown(g);
    FengExceptionFunction fn = g->functions[target->function];
    size_t count = fn.type_count + fn.capture_count;
    uint32_t *args = count != 0U ? calloc(count, sizeof(*args)) : NULL;
    if (count != 0U && args == NULL) {
        g->failed = true;
        return 0U;
    }
    const FengTypeRef *owner = resolved->owner_instance_type_ref;
    for (size_t i = 0U; i < target->owner_parameter_count; ++i) {
        const FengTypeRef *actual =
            owner != NULL && owner->kind == FENG_TYPE_REF_NAMED && i < owner->as.named.type_arg_count
                ? owner->as.named.type_args[i]
            : owner != NULL && owner->kind == FENG_TYPE_REF_ARRAY && i == 0U ? owner->as.inner
                                                                             : NULL;
        args[i] = actual != NULL ? ee_type(b, actual)
                                 : feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER, target->function,
                                                       (uint32_t)i, 0, NULL, NULL, 0);
    }
    if (target->owner->kind == FENG_DECL_FIT && owner != NULL) {
        for (size_t i = 0U; i < target->owner_parameter_count; ++i)
            args[i] = 0U;
        if (!ee_owner_arguments(b->data, target, ee_type(b, owner), args))
            for (size_t i = 0U; i < target->owner_parameter_count; ++i)
                args[i] = feng_exception_unknown(g);
    }
    for (size_t i = target->owner_parameter_count; i < fn.type_count; ++i) {
        size_t slot = i - target->owner_parameter_count;
        args[i] = slot < resolved->callable_type_arg_count
                      ? ee_type(b, resolved->callable_type_args[slot])
                      : feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER, target->function, (uint32_t)i,
                                            0, NULL, NULL, 0);
    }
    for (size_t i = fn.type_count; i < count; ++i)
        args[i] = receiver;
    uint32_t result;
    if (resolved->kind == FENG_RESOLVED_CALLABLE_SPEC_METHOD ||
        resolved->kind == FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD) {
        if (receiver == 0U)
            receiver = ee_value_type(g, owner != NULL ? ee_type(b, owner) : 0U);
        result = feng_exception_node(g, FENG_EXCEPTION_METHOD, target->function, receiver, 0, NULL, args,
                                     fn.type_count);
    } else
        result = feng_exception_node(g, FENG_EXCEPTION_FUNCTION, target->function, 0, 0, NULL, args, count);
    free(args);
    return result;
}

/* Track an actual invocation and its enclosing capture transformations. */
static void ee_call_site(ExceptionBuilder *b, const FengExpr *expr, uint32_t node) {
    FengExceptionAnalysis *data = b->data;
    for (size_t i = 0U; i < data->call_count; ++i) {
        ExceptionCallSite *site = &data->calls[i];
        if (site->source != b->source || site->expr != expr)
            continue;
        site->invocation = feng_exception_union(data->graph, site->invocation, node);
        site->escaping = feng_exception_union(data->graph, site->escaping, node);
        return;
    }
    if (!ee_grow((void **)&data->calls, &data->call_capacity, data->call_count + 1U, sizeof(*data->calls))) {
        data->graph->failed = true;
        return;
    }
    data->calls[data->call_count++] = (ExceptionCallSite){
        .expr = expr, .source = b->source, .invocation = node, .escaping = node
    };
}

/* Replace one protected-effect input in a catch transfer expression. Term
 * edges are acyclic; callable dependencies are left as declaration edges. */
static uint32_t ee_replace(FengExceptionGraph *g, uint32_t root, uint32_t from, uint32_t to, uint32_t *memo,
                           size_t memo_count) {
    if (root == from)
        return to;
    if (root == 0U || root >= memo_count)
        return root;
    if (memo[root] != UINT32_MAX)
        return memo[root];
    FengExceptionNode n = g->nodes[root];
    if (n.kind != FENG_EXCEPTION_UNION && n.kind != FENG_EXCEPTION_EXCLUDE &&
        n.kind != FENG_EXCEPTION_SELECT && n.kind != FENG_EXCEPTION_WHEN)
        return root;
    uint32_t x = n.kind == FENG_EXCEPTION_UNION ? 0U : ee_replace(g, n.x, from, to, memo, memo_count);
    uint32_t y = n.kind == FENG_EXCEPTION_WHEN ? ee_replace(g, n.y, from, to, memo, memo_count) : n.y;
    uint32_t result = 0U;
    if (n.kind == FENG_EXCEPTION_UNION) {
        for (size_t i = 0U; i < n.arg_count; ++i)
            result = feng_exception_union(g, result, ee_replace(g, n.args[i], from, to, memo, memo_count));
    } else
        result = feng_exception_node(g, n.kind, x, y, n.z, n.name, n.args, n.arg_count);
    memo[root] = result;
    return result;
}

/* Catch transfer is scoped to this protected expression. The remainder is
 * passed to an anonymous rethrow, never the original unfiltered set. */
static ExceptionExpression ee_try(ExceptionBuilder *b, const FengExpr *expr) {
    FengExceptionGraph *g = b->data->graph;
    size_t call_start = b->data->call_count;
    ExceptionExpression body = ee_expr(b, expr->as.try_expr.body);
    size_t call_end = b->data->call_count;
    uint32_t remainder = body.effects, handlers = 0U, value = body.value;
    for (size_t i = 0U; i < expr->as.try_expr.clause_count; ++i) {
        const FengTryCatchClause *clause = &expr->as.try_expr.clauses[i];
        uint32_t caught = remainder, type = clause->type != NULL ? ee_type(b, clause->type) : 0U;
        if (type != 0U) {
            caught = feng_exception_node(g, FENG_EXCEPTION_SELECT, remainder, type, 0, NULL, NULL, 0);
            remainder = feng_exception_node(g, FENG_EXCEPTION_EXCLUDE, remainder, type, 0, NULL, NULL, 0);
        } else
            remainder = 0U;
        size_t saved_count = b->binding_count;
        uint32_t saved_caught = b->caught;
        b->caught = caught;
        if (type != 0U)
            ee_bind(b, clause->name, ee_value_type(g, type), type, false);
        size_t handler_call_start = b->data->call_count;
        uint32_t saved_returns = b->returns;
        b->returns = 0U;
        ExceptionExpression handler = ee_block(b, clause->body);
        b->returns = feng_exception_union(
            g, saved_returns,
            feng_exception_node(g, FENG_EXCEPTION_WHEN, caught, b->returns, 0, NULL, NULL, 0));
        b->caught = saved_caught;
        b->binding_count = saved_count;
        uint32_t guarded =
            feng_exception_node(g, FENG_EXCEPTION_WHEN, caught, handler.effects, 0, NULL, NULL, 0);
        handlers = feng_exception_union(g, handlers, guarded);
        value = feng_exception_union(
            g, value, feng_exception_node(g, FENG_EXCEPTION_WHEN, caught, handler.value, 0, NULL, NULL, 0));
        for (size_t c = handler_call_start; c < b->data->call_count; ++c)
            if (b->data->calls[c].source == b->source)
                b->data->calls[c].escaping = feng_exception_node(
                    g, FENG_EXCEPTION_WHEN, caught, b->data->calls[c].escaping, 0, NULL, NULL, 0);
    }
    uint32_t outgoing = feng_exception_union(g, remainder, handlers);
    for (size_t c = call_start; c < call_end && body.effects != 0U; ++c) {
        ExceptionCallSite *site = &b->data->calls[c];
        if (site->source != b->source)
            continue;
        size_t count = g->node_count;
        uint32_t *memo = malloc(count * sizeof(*memo));
        if (memo == NULL) {
            g->failed = true;
            break;
        }
        for (size_t j = 0U; j < count; ++j)
            memo[j] = UINT32_MAX;
        site->escaping = ee_replace(g, outgoing, body.effects, site->escaping, memo, count);
        free(memo);
    }
    return (ExceptionExpression){outgoing, value, 0U};
}

/* Create a closure template with explicit capture slots, preserving the
 * enclosing generic domain without executing the Lambda body. */
static uint32_t ee_lambda(ExceptionBuilder *b, const FengExpr *expr) {
    FengExceptionAnalysis *data = b->data;
    FengExceptionGraph *g = data->graph;
    ExceptionSource *source = ee_source(data, expr);
    if (source == NULL) {
        source = calloc(1U, sizeof(*source));
        if (source == NULL || !ee_grow((void **)&data->sources, &data->source_capacity,
                                       data->source_count + 1U, sizeof(*data->sources))) {
            free(source);
            g->failed = true;
            return 0U;
        }
        source->key = expr;
        source->lambda = expr;
        source->program = b->source->program;
        source->owner = b->source->owner;
        source->parameter_count = b->source->parameter_count;
        source->owner_parameter_count = b->source->owner_parameter_count;
        if (source->parameter_count != 0U) {
            source->parameters = malloc(source->parameter_count * sizeof(*source->parameters));
            if (source->parameters == NULL) {
                free(source);
                g->failed = true;
                return 0U;
            }
            memcpy(source->parameters, b->source->parameters,
                   source->parameter_count * sizeof(*source->parameters));
        }
        FengExceptionFunction parent = g->functions[b->source->function];
        source->function = feng_exception_function_add(g, "<lambda>", (const char *const *)parent.type_names,
                                                       parent.type_count, expr->as.lambda.param_count,
                                                       expr->as.lambda.capture_count);
        data->sources[data->source_count++] = source;
        source->built = true;
        ExceptionBuilder child = {0};
        child.data = data;
        child.source = source;
        for (size_t i = 0U; i < expr->as.lambda.param_count; ++i) {
            const FengParameter *param = &expr->as.lambda.params[i];
            ee_bind(&child, param->name,
                    feng_exception_node(g, FENG_EXCEPTION_VALUE_PARAMETER, source->function, (uint32_t)i, 0,
                                        NULL, NULL, 0),
                    param->type != NULL ? ee_type(&child, param->type) : 0U,
                    param->mutability == FENG_MUTABILITY_VAR);
        }
        for (size_t i = 0U; i < expr->as.lambda.capture_count; ++i) {
            const FengLambdaCapture *capture = &expr->as.lambda.captures[i];
            uint32_t slot =
                feng_exception_node(g, FENG_EXCEPTION_VALUE_PARAMETER, source->function,
                                    (uint32_t)(expr->as.lambda.param_count + i), 0, NULL, NULL, 0);
            if (capture->kind == FENG_LAMBDA_CAPTURE_SELF)
                child.self = slot;
            else
                ee_bind(&child, capture->name, slot, 0U, capture->mutability == FENG_MUTABILITY_VAR);
        }
        ExceptionExpression body = expr->as.lambda.is_block_body
                                       ? ee_block(&child, expr->as.lambda.body_block)
                                       : ee_expr(&child, expr->as.lambda.body);
        g->functions[source->function].effects = body.effects;
        g->functions[source->function].result = feng_exception_union(g, child.returns, body.value);
        free(child.bindings);
    }
    size_t count = source->parameter_count + expr->as.lambda.capture_count;
    uint32_t *args = count != 0U ? calloc(count, sizeof(*args)) : NULL;
    if (count != 0U && args == NULL) {
        g->failed = true;
        return 0U;
    }
    for (size_t i = 0U; i < source->parameter_count; ++i)
        args[i] = feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER, b->source->function, (uint32_t)i, 0,
                                      NULL, NULL, 0);
    for (size_t i = 0U; i < expr->as.lambda.capture_count; ++i) {
        const FengLambdaCapture *capture = &expr->as.lambda.captures[i];
        ExceptionBinding *binding = ee_binding(b, capture->name);
        args[source->parameter_count + i] = capture->kind == FENG_LAMBDA_CAPTURE_SELF ? b->self
                                            : binding != NULL && !binding->mutable
                                                ? binding->value
                                                : feng_exception_unknown(g);
    }
    uint32_t value =
        feng_exception_node(g, FENG_EXCEPTION_FUNCTION, source->function, 0, 0, NULL, args, count);
    free(args);
    return value;
}

/* Global and static storage access can execute a lazy initializer. Keep its
 * invocation separate from a callable value returned by that initializer. */
static ExceptionExpression ee_storage_access(ExceptionBuilder *b, const FengExpr *expr,
                                             ExceptionSource *source, uint32_t receiver) {
    FengExceptionGraph *g = b->data->graph;
    ExceptionExpression result = {0};
    uint32_t callable;
    bool mutable;
    if (source->member != NULL) {
        FengResolvedCallable target = {0};
        target.kind = source->owner->kind == FENG_DECL_SPEC ? FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD
                                                            : FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD;
        target.member = source->member;
        target.owner_type_decl = source->owner;
        const FengSemanticTypeFact *fact =
            feng_semantic_lookup_type_fact(b->data->analysis, expr->as.member.object);
        target.owner_instance_type_ref = fact != NULL ? fact->type_ref : NULL;
        callable = ee_callable(b, &target, receiver);
        mutable = source->member->as.field.mutability == FENG_MUTABILITY_VAR;
    } else {
        callable = feng_exception_node(g, FENG_EXCEPTION_FUNCTION, source->function, 0, 0, NULL, NULL, 0);
        mutable = source->owner->as.binding.mutability == FENG_MUTABILITY_VAR;
    }
    result.effects = feng_exception_node(g, FENG_EXCEPTION_CALL, callable, 0, 0, NULL, NULL, 0);
    result.value = mutable
                       ? feng_exception_unknown(g)
                       : feng_exception_node(g, FENG_EXCEPTION_CALL_RESULT, callable, 0, 0, NULL, NULL, 0);
    ee_call_site(b, expr, result.effects);
    return result;
}

/* Build value provenance and invocation effects for every expression kind. */
static ExceptionExpression ee_expr(ExceptionBuilder *b, const FengExpr *expr) {
    ExceptionExpression result = {0};
    if (expr == NULL || b->data->graph->failed)
        return result;
    FengExceptionGraph *g = b->data->graph;
    const FengSpecCoercionSite *coercion = feng_semantic_lookup_spec_coercion_site(b->data->analysis, expr);
    switch (expr->kind) {
    case FENG_EXPR_IDENTIFIER: {
        ExceptionBinding *binding = ee_binding(b, expr->as.identifier);
        if (binding != NULL) {
            result.value =
                binding->mutable ? feng_exception_unknown(g) : binding->value;
            if (result.value == 0U)
                result.value = ee_value_type(g, binding->type);
            else if (binding->type != 0U)
                result.value = feng_exception_node(g, FENG_EXCEPTION_TYPED_VALUE, result.value, binding->type,
                                                   0, NULL, NULL, 0);
            break;
        }
        const ExceptionValueOrigin *origin = ee_origin(b->data, expr);
        if (origin != NULL && origin->type_parameter) {
            for (size_t i = 0U; i < b->source->parameter_count; ++i)
                if (ee_same(b->source->parameters[i].name, origin->parameter_name)) {
                    result.value = ee_value_type(g, feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER,
                                                                        b->source->function, (uint32_t)i, 0,
                                                                        NULL, NULL, 0));
                    break;
                }
            break;
        }
        ExceptionSource *source = ee_source(b->data, origin != NULL ? origin->source : NULL);
        if (source != NULL && source->signature == NULL)
            result = ee_storage_access(b, expr, source, 0U);
        break;
    }
    case FENG_EXPR_SELF:
        result.value = b->self;
        break;
    case FENG_EXPR_LAMBDA:
        result.value = ee_lambda(b, expr);
        break;
    case FENG_EXPR_CALL: {
        ExceptionExpression callee = ee_expr(b, expr->as.call.callee);
        result.effects = callee.effects;
        size_t count = expr->as.call.arg_count;
        uint32_t *args = count != 0U ? malloc(count * sizeof(*args)) : NULL;
        if (count != 0U && args == NULL) {
            g->failed = true;
            return result;
        }
        for (size_t i = 0U; i < count; ++i) {
            ExceptionExpression arg = ee_expr(b, expr->as.call.args[i]);
            args[i] = arg.value;
            result.effects = feng_exception_union(g, result.effects, arg.effects);
        }
        const FengResolvedCallable *resolved = &expr->as.call.resolved_callable;
        uint32_t receiver = callee.receiver;
        uint32_t callable =
            resolved->kind != FENG_RESOLVED_CALLABLE_NONE ? ee_callable(b, resolved, receiver) : callee.value;
        if (callable == 0U)
            callable = feng_exception_unknown(g);
        uint32_t invocation = feng_exception_node(g, FENG_EXCEPTION_CALL, callable, 0, 0, NULL, args, count);
        result.effects = feng_exception_union(g, result.effects, invocation);
        result.value = feng_exception_node(g, FENG_EXCEPTION_CALL_RESULT, callable, 0, 0, NULL, args, count);
        if (resolved->kind == FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR)
            result.value = ee_value_type(g, ee_site_type(b, expr));
        free(args);
        ee_call_site(b, expr, invocation);
        break;
    }
    case FENG_EXPR_TRY:
        return ee_try(b, expr);
    case FENG_EXPR_IF: {
        ExceptionExpression condition = ee_expr(b, expr->as.if_expr.condition);
        ExceptionExpression left = ee_block(b, expr->as.if_expr.then_block);
        ExceptionExpression right = ee_block(b, expr->as.if_expr.else_block);
        result.effects =
            feng_exception_union(g, condition.effects, feng_exception_union(g, left.effects, right.effects));
        result.value = feng_exception_union(g, left.value, right.value);
        break;
    }
    case FENG_EXPR_MATCH: {
        result.effects = ee_expr(b, expr->as.match_expr.target).effects;
        for (size_t i = 0U; i < expr->as.match_expr.branch_count; ++i) {
            ExceptionExpression arm = ee_block(b, expr->as.match_expr.branches[i].body);
            result.effects = feng_exception_union(g, result.effects, arm.effects);
            result.value = feng_exception_union(g, result.value, arm.value);
        }
        ExceptionExpression arm = ee_block(b, expr->as.match_expr.else_block);
        result.effects = feng_exception_union(g, result.effects, arm.effects);
        result.value = feng_exception_union(g, result.value, arm.value);
        break;
    }
    case FENG_EXPR_GENERIC_TARGET:
        result = ee_expr(b, expr->as.generic_target.target);
        break;
    case FENG_EXPR_MEMBER: {
        ExceptionExpression object = ee_expr(b, expr->as.member.object);
        result.effects = object.effects;
        result.receiver = object.value;
        const ExceptionValueOrigin *origin = ee_origin(b->data, expr);
        ExceptionSource *source = ee_source(b->data, origin != NULL ? origin->source : NULL);
        if (source != NULL && source->signature == NULL &&
            g->functions[source->function].capture_count == 0U) {
            ExceptionExpression access = ee_storage_access(b, expr, source, object.value);
            result.effects = feng_exception_union(g, result.effects, access.effects);
            result.value = access.value;
        }
        break;
    }
    case FENG_EXPR_INDEX:
        result.effects = feng_exception_union(g, ee_expr(b, expr->as.index.object).effects,
                                              ee_expr(b, expr->as.index.index).effects);
        break;
    case FENG_EXPR_UNARY:
        result = ee_expr(b, expr->as.unary.operand);
        break;
    case FENG_EXPR_CAST:
        result = ee_expr(b, expr->as.cast.value);
        break;
    case FENG_EXPR_BINARY:
        result.effects = feng_exception_union(g, ee_expr(b, expr->as.binary.left).effects,
                                              ee_expr(b, expr->as.binary.right).effects);
        break;
    case FENG_EXPR_MATCH_OP:
        result.effects = ee_expr(b, expr->as.match_op.target).effects;
        break;
    case FENG_EXPR_ARRAY_NEW:
        result.effects = ee_expr(b, expr->as.array_new.size).effects;
        break;
    case FENG_EXPR_ARRAY_LITERAL:
    case FENG_EXPR_TUPLE_LITERAL: {
        size_t count = expr->kind == FENG_EXPR_ARRAY_LITERAL ? expr->as.array_literal.count
                                                             : expr->as.tuple_literal.count;
        FengExpr *const *items = expr->kind == FENG_EXPR_ARRAY_LITERAL ? expr->as.array_literal.items
                                                                       : expr->as.tuple_literal.items;
        for (size_t i = 0U; i < count; ++i)
            result.effects = feng_exception_union(g, result.effects, ee_expr(b, items[i]).effects);
        break;
    }
    case FENG_EXPR_OBJECT_LITERAL: {
        for (size_t i = 0U; i < expr->as.object_literal.field_count; ++i)
            result.effects = feng_exception_union(
                g, result.effects, ee_expr(b, expr->as.object_literal.fields[i].value).effects);
        uint32_t type = ee_site_type(b, expr);
        const FengDecl *decl = type != 0U && g->nodes[type].kind == FENG_EXCEPTION_NAMED_TYPE
                                   ? ee_named_decl(b->data, g->nodes[type].name, g->nodes[type].arg_count)
                                   : NULL;
        if (decl != NULL) {
            FengResolvedCallable resolved = {0};
            resolved.kind = FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR;
            resolved.owner_type_decl = decl;
            resolved.member = expr->as.object_literal.resolved_constructor;
            const FengSemanticTypeFact *fact = feng_semantic_lookup_type_fact(b->data->analysis, expr);
            resolved.owner_instance_type_ref = fact != NULL ? fact->type_ref : NULL;
            uint32_t fn = ee_callable(b, &resolved, ee_value_type(g, type));
            uint32_t invocation = feng_exception_node(g, FENG_EXCEPTION_CALL, fn, 0, 0, NULL, NULL, 0);
            result.effects = feng_exception_union(g, result.effects, invocation);
            ee_call_site(b, expr, invocation);
        }
        break;
    }
    case FENG_EXPR_BOOL:
    case FENG_EXPR_INTEGER:
    case FENG_EXPR_FLOAT:
    case FENG_EXPR_STRING:
    case FENG_EXPR_TYPE_TARGET:
        break;
    }
    if (coercion != NULL && coercion->form == FENG_SPEC_COERCION_FORM_CALLABLE &&
        (coercion->callable_decl != NULL || coercion->callable_member != NULL)) {
        FengResolvedCallable target = {0};
        target.kind = coercion->callable_decl != NULL ? FENG_RESOLVED_CALLABLE_FUNCTION
                      : coercion->callable_owner_type_decl != NULL &&
                              coercion->callable_owner_type_decl->kind == FENG_DECL_SPEC
                          ? (coercion->callable_member->is_static ? FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD
                                                                  : FENG_RESOLVED_CALLABLE_SPEC_METHOD)
                          : FENG_RESOLVED_CALLABLE_TYPE_METHOD;
        target.function_decl = coercion->callable_decl;
        target.member = coercion->callable_member;
        target.owner_type_decl = coercion->callable_owner_type_decl;
        target.fit_decl = coercion->callable_fit_decl;
        target.owner_instance_type_ref = coercion->callable_receiver_type_ref;
        target.callable_type_args = (const FengTypeRef **)coercion->callable_type_args;
        target.callable_type_arg_count = coercion->callable_type_arg_count;
        result.value = ee_callable(b, &target, result.receiver);
    }
    if (result.value == 0U)
        result.value = ee_value_type(g, ee_site_type(b, expr));
    return result;
}

/* Account for resolver-selected calls inserted by iterator lowering. */
static uint32_t ee_iterator_call(ExceptionBuilder *b, const FengExpr *site, const FengTypeMember *member,
                                 const FengTypeRef *owner, uint32_t receiver) {
    if (member == NULL)
        return 0U;
    ExceptionSource *source = ee_source(b->data, member);
    FengResolvedCallable target = {0};
    target.kind = FENG_RESOLVED_CALLABLE_TYPE_METHOD;
    target.member = member;
    target.owner_type_decl = source != NULL ? source->owner : NULL;
    target.owner_instance_type_ref = owner;
    uint32_t fn = ee_callable(b, &target, receiver);
    uint32_t call = feng_exception_node(b->data->graph, FENG_EXCEPTION_CALL, fn, 0, 0, NULL, NULL, 0);
    ee_call_site(b, site, call);
    return call;
}

/* Collect all statement evaluation paths; ordinary branch conditions never
 * prune exception alternatives. Each nested callable keeps its own boundary. */
static uint32_t ee_stmt(ExceptionBuilder *b, const FengStmt *stmt) {
    if (stmt == NULL || b->data->graph->failed)
        return 0U;
    FengExceptionGraph *g = b->data->graph;
    uint32_t result = 0U;
    switch (stmt->kind) {
    case FENG_STMT_BLOCK:
        return ee_block(b, stmt->as.block).effects;
    case FENG_STMT_BINDING: {
        const FengBinding *binding = &stmt->as.binding;
        ExceptionExpression value = ee_expr(b, binding->initializer);
        uint32_t type = binding->type != NULL ? ee_type(b, binding->type) : ee_site_type(b, binding);
        ee_bind(b, binding->name, value.value, type, binding->mutability == FENG_MUTABILITY_VAR);
        for (size_t i = 0U; i < binding->destructure_count; ++i)
            ee_bind(b, binding->destructure_names[i],
                    feng_exception_unknown(g), 0U,
                    binding->mutability == FENG_MUTABILITY_VAR);
        return value.effects;
    }
    case FENG_STMT_ASSIGN:
        return feng_exception_union(g, ee_expr(b, stmt->as.assign.target).effects,
                                    ee_expr(b, stmt->as.assign.value).effects);
    case FENG_STMT_EXPR:
    case FENG_STMT_TRY:
        return ee_expr(b, stmt->as.expr).effects;
    case FENG_STMT_RETURN: {
        ExceptionExpression value = ee_expr(b, stmt->as.return_value);
        b->returns = feng_exception_union(g, b->returns, value.value);
        return value.effects;
    }
    case FENG_STMT_THROW: {
        if (stmt->as.throw_value == NULL)
            return b->caught;
        ExceptionExpression value = ee_expr(b, stmt->as.throw_value);
        uint32_t type = ee_site_type(b, stmt->as.throw_value);
        uint32_t thrown = type != 0U ? feng_exception_node(g, FENG_EXCEPTION_THROW, type, 0, 0, NULL, NULL, 0)
                                     : feng_exception_unknown(g);
        return feng_exception_union(g, value.effects, thrown);
    }
    case FENG_STMT_DEFER: {
        FengExceptionAnalysis *data = b->data;
        size_t first_call = data->call_count;
        uint32_t effects = ee_block(b, stmt->as.defer_block).effects;
        if (!ee_grow((void **)&data->defers, &data->defer_capacity, data->defer_count + 1U,
                     sizeof(*data->defers))) {
            g->failed = true;
            return effects;
        }
        ExceptionDeferSite *site = &data->defers[data->defer_count++];
        *site = (ExceptionDeferSite){.statement = stmt, .source = b->source, .root = effects};
        for (size_t i = first_call; i < data->call_count; ++i)
            if (data->calls[i].source == b->source)
                ++site->call_count;
        if (site->call_count != 0U) {
            site->calls = calloc(site->call_count, sizeof(*site->calls));
            if (site->calls == NULL) {
                g->failed = true;
                return effects;
            }
            size_t next = 0U;
            for (size_t i = first_call; i < data->call_count; ++i) {
                const ExceptionCallSite *call = &data->calls[i];
                if (call->source == b->source)
                    site->calls[next++] = (ExceptionBoundaryCall){
                        .expression = call->expr, .root = call->escaping};
            }
        }
        return effects;
    }
    case FENG_STMT_IF:
        for (size_t i = 0U; i < stmt->as.if_stmt.clause_count; ++i) {
            result =
                feng_exception_union(g, result, ee_expr(b, stmt->as.if_stmt.clauses[i].condition).effects);
            result = feng_exception_union(g, result, ee_block(b, stmt->as.if_stmt.clauses[i].block).effects);
        }
        return feng_exception_union(g, result, ee_block(b, stmt->as.if_stmt.else_block).effects);
    case FENG_STMT_MATCH:
        result = ee_expr(b, stmt->as.match_stmt.target).effects;
        for (size_t i = 0U; i < stmt->as.match_stmt.branch_count; ++i)
            result =
                feng_exception_union(g, result, ee_block(b, stmt->as.match_stmt.branches[i].body).effects);
        return feng_exception_union(g, result, ee_block(b, stmt->as.match_stmt.else_block).effects);
    case FENG_STMT_WHILE:
        return feng_exception_union(g, ee_expr(b, stmt->as.while_stmt.condition).effects,
                                    ee_block(b, stmt->as.while_stmt.body).effects);
    case FENG_STMT_FOR: {
        size_t saved = b->binding_count;
        result = ee_stmt(b, stmt->as.for_stmt.init);
        result = feng_exception_union(g, result, ee_expr(b, stmt->as.for_stmt.condition).effects);
        ExceptionExpression iterable = ee_expr(b, stmt->as.for_stmt.iter_expr);
        result = feng_exception_union(g, result, iterable.effects);
        const FengSemanticTypeFact *iter_type =
            feng_semantic_lookup_type_fact(b->data->analysis, stmt->as.for_stmt.iter_expr);
        result = feng_exception_union(
            g, result,
            ee_iterator_call(b, stmt->as.for_stmt.iter_expr, stmt->as.for_stmt.iter_iterable_method,
                             iter_type != NULL ? iter_type->type_ref : NULL, iterable.value));
        const FengTypeRef *cursor = stmt->as.for_stmt.iter_cursor_type_ref;
        result = feng_exception_union(
            g, result,
            ee_iterator_call(b, stmt->as.for_stmt.iter_expr, stmt->as.for_stmt.iter_iterator_method, cursor,
                             cursor != NULL ? ee_value_type(g, ee_type(b, cursor)) : iterable.value));
        if (stmt->as.for_stmt.is_for_in)
            ee_bind(b, stmt->as.for_stmt.iter_binding.name,
                    feng_exception_unknown(g), 0U, true);
        result = feng_exception_union(g, result, ee_block(b, stmt->as.for_stmt.body).effects);
        result = feng_exception_union(g, result, ee_stmt(b, stmt->as.for_stmt.update));
        b->binding_count = saved;
        return result;
    }
    case FENG_STMT_BREAK:
    case FENG_STMT_CONTINUE:
        return 0U;
    }
    return result;
}

/* A block restores only lexical bindings; callable returns are accumulated
 * independently from normal expression values. */
static ExceptionExpression ee_block(ExceptionBuilder *b, const FengBlock *block) {
    ExceptionExpression result = {0};
    if (block == NULL)
        return result;
    size_t saved = b->binding_count;
    for (size_t i = 0U; i < block->statement_count; ++i) {
        const FengStmt *stmt = block->statements[i];
        if (i + 1U == block->statement_count && stmt->kind == FENG_STMT_EXPR) {
            ExceptionExpression last = ee_expr(b, stmt->as.expr);
            result.effects = feng_exception_union(b->data->graph, result.effects, last.effects);
            result.value = last.value;
        } else
            result.effects = feng_exception_union(b->data->graph, result.effects, ee_stmt(b, stmt));
    }
    b->binding_count = saved;
    return result;
}

/* Construct a source body's summary once; imported bodies remain opaque. */
static void ee_build_source(FengExceptionAnalysis *data, ExceptionSource *source) {
    if (source->built || data->graph->failed)
        return;
    source->built = true;
    FengExceptionGraph *g = data->graph;
    ExceptionBuilder b = {0};
    b.data = data;
    b.source = source;
    FengExceptionFunction fn = g->functions[source->function];
    if (fn.capture_count != 0U)
        b.self = feng_exception_node(g, FENG_EXCEPTION_VALUE_PARAMETER, source->function,
                                     (uint32_t)fn.parameter_count, 0, NULL, NULL, 0);
    if (source->signature != NULL) {
        for (size_t i = 0U; i < source->signature->param_count; ++i) {
            const FengParameter *p = &source->signature->params[i];
            ee_bind(&b, p->name,
                    feng_exception_node(g, FENG_EXCEPTION_VALUE_PARAMETER, source->function, (uint32_t)i, 0,
                                        NULL, NULL, 0),
                    ee_type(&b, p->type), p->mutability == FENG_MUTABILITY_VAR);
        }
    }
    ExceptionExpression body = {0};
    if (source->signature != NULL) {
        if (source->signature->body != NULL)
            body = ee_block(&b, source->signature->body);
        else if (!source->owner->is_extern)
            body.effects = feng_exception_unknown(g);
    } else if (source->member != NULL) {
        body = ee_expr(&b, source->member->as.field.initializer);
    } else if (source->owner->kind == FENG_DECL_GLOBAL_BINDING) {
        body = ee_expr(&b, source->owner->as.binding.initializer);
    } else if (source->owner->kind == FENG_DECL_TYPE) {
        for (size_t i = 0U; i < source->owner->as.type_decl.mixin_count; ++i)
            body.effects = feng_exception_union(
                g, body.effects,
                ee_expr(&b, source->owner->as.type_decl.mixins[i].source_constructor).effects);
        for (size_t i = 0U; i < source->owner->as.type_decl.member_count; ++i) {
            const FengTypeMember *member = source->owner->as.type_decl.members[i];
            if (member->mixin_origin != NULL && member->mixin_origin->source_constructor != NULL)
                continue;
            if (member->kind == FENG_TYPE_MEMBER_FIELD && !member->is_static)
                body.effects =
                    feng_exception_union(g, body.effects, ee_expr(&b, member->as.field.initializer).effects);
        }
        body.value = ee_value_type(g, ee_decl_type(&b, source->owner));
    }
    if (source->member != NULL && source->member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR) {
        ExceptionSource *init = ee_source(data, source->owner);
        if (init != NULL) {
            uint32_t *args = fn.type_count != 0U ? malloc(fn.type_count * sizeof(*args)) : NULL;
            if (fn.type_count != 0U && args == NULL)
                g->failed = true;
            for (size_t i = 0U; i < fn.type_count && !g->failed; ++i)
                args[i] = feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER, source->function, (uint32_t)i,
                                              0, NULL, NULL, 0);
            uint32_t call = feng_exception_node(g, FENG_EXCEPTION_FUNCTION, init->function, 0, 0, NULL, args,
                                                fn.type_count);
            body.effects = feng_exception_union(
                g, body.effects, feng_exception_node(g, FENG_EXCEPTION_CALL, call, 0, 0, NULL, NULL, 0));
            free(args);
        }
    }
    g->functions[source->function].effects = body.effects;
    g->functions[source->function].result = feng_exception_union(g, b.returns, body.value);
    free(b.bindings);
}

/* Bind one validated implementation, preserving owner and method domains. */
static uint32_t ee_method_target(FengExceptionAnalysis *data, ExceptionSource *spec,
                                 const FengTypeMember *member, uint32_t subject, const uint32_t *type_args,
                                 size_t type_arg_count) {
    FengExceptionGraph *g = data->graph;
    ExceptionSource *impl = ee_source(data, member);
    if (impl == NULL)
        return 0U;
    FengExceptionFunction fn = g->functions[impl->function];
    size_t count = fn.type_count + fn.capture_count;
    uint32_t *args = count != 0U ? calloc(count, sizeof(*args)) : NULL;
    if (count != 0U && args == NULL) {
        g->failed = true;
        return 0U;
    }
    if (!ee_owner_arguments(data, impl, g->nodes[subject].x, args)) {
        free(args);
        return 0U;
    }
    for (size_t p = impl->owner_parameter_count; p < fn.type_count; ++p) {
        size_t slot = spec->owner_parameter_count + p - impl->owner_parameter_count;
        args[p] = slot < type_arg_count ? type_args[slot] : feng_exception_unknown(g);
    }
    for (size_t p = fn.type_count; p < count; ++p)
        args[p] = subject;
    uint32_t result =
        feng_exception_node(g, FENG_EXCEPTION_FUNCTION, impl->function, 0, 0, NULL, args, count);
    free(args);
    return result;
}

/* Resolve statically known spec subjects using the existing witness choices. */
static uint32_t ee_method(void *user, uint32_t requirement, uint32_t subject, const uint32_t *type_args,
                          size_t type_arg_count) {
    FengExceptionAnalysis *data = user;
    FengExceptionGraph *g = data->graph;
    FengExceptionNode value = g->nodes[subject];
    if (value.kind == FENG_EXCEPTION_UNION) {
        uint32_t result = 0U;
        for (size_t i = 0U; i < value.arg_count; ++i) {
            uint32_t target = ee_method(user, requirement, value.args[i], type_args, type_arg_count);
            result = feng_exception_union(
                g, result, target != 0U ? target : feng_exception_unknown(g));
        }
        return result;
    }
    if (value.kind == FENG_EXCEPTION_GUARDED) {
        uint32_t target = ee_method(user, requirement, value.y, type_args, type_arg_count);
        return feng_exception_node(g, FENG_EXCEPTION_GUARDED, value.x,
                                   target != 0U ? target : feng_exception_unknown(g),
                                   0, NULL, NULL, 0);
    }
    if (value.kind != FENG_EXCEPTION_VALUE_TYPE)
        return 0U;
    FengExceptionNode type = g->nodes[value.x];
    if (type.kind != FENG_EXCEPTION_NAMED_TYPE)
        return 0U;
    const FengDecl *decl = ee_named_decl(data, type.name, type.arg_count);
    ExceptionSource *spec = NULL;
    for (size_t i = 0U; i < data->source_count; ++i)
        if (data->sources[i]->function == requirement ||
            strcmp(g->functions[data->sources[i]->function].name, g->functions[requirement].name) == 0) {
            spec = data->sources[i];
            break;
        }
    if (spec == NULL || spec->member == NULL)
        return 0U;
    uint32_t targets = 0U;
    for (size_t i = 0U; i < data->analysis->spec_implementation_selection_count; ++i) {
        const FengSpecImplementationSelection *selection = &data->analysis->spec_implementation_selections[i];
        if (selection->spec_member != spec->member)
            continue;
        ExceptionSource *impl = ee_source(data, selection->impl_member);
        if (impl == NULL)
            continue;
        bool matches = (selection->relation_owner_decl == decl && decl != NULL) ||
                       selection->relation_owner_decl->kind == FENG_DECL_FIT;
        if (!matches)
            continue;
        targets = feng_exception_union(
            g, targets,
            ee_method_target(data, spec, selection->impl_member, subject, type_args, type_arg_count));
    }
    for (size_t i = 0U; i < data->analysis->spec_witness_count; ++i) {
        const FengSpecWitness *witness = &data->analysis->spec_witnesses[i];
        const FengSemanticSubjectKey *key = &witness->subject_key;
        bool matches = (key->kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL && key->as.type_decl == decl) ||
                       (key->kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN &&
                        strcmp(key->as.builtin_canonical_name, type.name) == 0) ||
                       key->kind == FENG_SEMANTIC_SUBJECT_KEY_ARRAY;
        if (!matches)
            continue;
        for (size_t j = 0U; j < witness->member_count; ++j) {
            const FengSpecWitnessMember *member = &witness->members[j];
            if (member->spec_member == spec->member &&
                (key->kind != FENG_SEMANTIC_SUBJECT_KEY_ARRAY || member->via_fit_decl != NULL))
                targets = feng_exception_union(
                    g, targets,
                    ee_method_target(data, spec, member->impl_member, subject, type_args, type_arg_count));
        }
    }
    return targets;
}

/* Boundary errors use the existing AE1313 contract after the fixed point. */
static bool ee_abi_error(ExceptionSource *source, FengSemanticError **errors, size_t *count, size_t *capacity,
                         const char *display) {
    if (!ee_grow((void **)errors, capacity, *count + 1U, sizeof(**errors)))
        return false;
    size_t length = strlen(display) + 160U;
    char *message = malloc(length);
    if (message == NULL)
        return false;
    snprintf(message, length,
             "uncaught exceptions must not cross the @abi ABI boundary; cannot prove an empty exception "
             "summary: %s",
             display);
    FengSemanticError error = {0};
    error.code = "AE1313";
    error.path = source->program->path;
    error.token = source->member != NULL ? source->member->token : source->owner->token;
    error.message = message;
    (*errors)[(*count)++] = error;
    return true;
}

bool feng_semantic_collect_exception_effects(FengSemanticAnalysis *analysis) {
    FengExceptionAnalysis *data = ee_data(analysis);
    if (data == NULL)
        return false;
    data->graph = feng_exception_graph_create();
    if (data->graph == NULL)
        return false;
    for (size_t m = 0U; m < analysis->module_count; ++m) {
        const FengSemanticModule *module = &analysis->modules[m];
        bool imported = module->origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE;
        for (size_t p = 0U; p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0U; d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind == FENG_DECL_FUNCTION || decl->kind == FENG_DECL_GLOBAL_BINDING ||
                    decl->kind == FENG_DECL_TYPE)
                    (void)ee_register(data, module, program, decl, NULL, imported);
                FengTypeMember *const *members = NULL;
                size_t count = 0U;
                if (decl->kind == FENG_DECL_TYPE) {
                    members = decl->as.type_decl.members;
                    count = decl->as.type_decl.member_count;
                }
                if (decl->kind == FENG_DECL_FIT) {
                    members = decl->as.fit_decl.members;
                    count = decl->as.fit_decl.member_count;
                }
                if (decl->kind == FENG_DECL_SPEC && decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                    members = decl->as.spec_decl.as.object.members;
                    count = decl->as.spec_decl.as.object.member_count;
                }
                for (size_t i = 0U; i < count; ++i)
                    (void)ee_register(data, module, program, decl, members[i], imported);
                if (data->graph->failed)
                    return false;
            }
        }
    }
    for (size_t i = 0U; i < data->source_count; ++i)
        ee_build_source(data, data->sources[i]);
    if (data->graph->failed)
        return false;
    FengExceptionSolver *solver = feng_exception_solver_create(data->graph, ee_method, data);
    if (solver == NULL)
        return false;
    for (size_t i = 0U; i < data->source_count; ++i)
        data->sources[i]->instance = feng_exception_solver_declaration(solver, data->sources[i]->function);
    bool ok = feng_exception_solver_run(solver);
    /* A fully caught invocation can be absent from its callable's outgoing
     * root. Demand call-site results before publishing any fixed-point facts. */
    for (size_t i = 0U; i < data->call_count && ok; ++i) {
        const ExceptionCallSite *site = &data->calls[i];
        (void)feng_exception_solver_term(solver, site->source->instance, site->invocation);
        (void)feng_exception_solver_term(solver, site->source->instance, site->escaping);
    }
    /* Cleanup roots must be demanded even when an outer catch erases the
     * enclosing function's effects. Never apply that outer catch here. */
    for (size_t i = 0U; i < data->defer_count && ok; ++i) {
        const ExceptionDeferSite *site = &data->defers[i];
        (void)feng_exception_solver_term(solver, site->source->instance, site->root);
        for (size_t j = 0U; j < site->call_count; ++j)
            (void)feng_exception_solver_term(solver, site->source->instance, site->calls[j].root);
    }
    ok = ok && feng_exception_solver_run(solver);
    for (size_t i = 0U; i < data->source_count && ok; ++i) {
        ExceptionSource *source = data->sources[i];
        source->summary.effects = feng_exception_solver_effects(solver, source->instance);
        source->summary.graph = data->graph;
        source->summary.function = source->function;
        feng_exception_graph_retain(data->graph);
    }
    for (size_t i = 0U; i < data->call_count && ok; ++i) {
        ExceptionCallSite *site = &data->calls[i];
        site->effects = feng_exception_solver_term(solver, site->source->instance, site->invocation);
        site->escaping_effects = feng_exception_solver_term(solver, site->source->instance, site->escaping);
    }
    for (size_t i = 0U; i < data->defer_count && ok; ++i) {
        ExceptionDeferSite *site = &data->defers[i];
        site->effects = feng_exception_solver_term(solver, site->source->instance, site->root);
        for (size_t j = 0U; j < site->call_count; ++j)
            site->calls[j].effects =
                feng_exception_solver_term(solver, site->source->instance, site->calls[j].root);
    }
    ok = ok && !data->graph->failed;
    feng_exception_solver_free(solver);
    return ok;
}

const FengExceptionTemplate *feng_semantic_exception_template(const FengSemanticAnalysis *analysis,
                                                              const void *source_node) {
    if (analysis == NULL || analysis->exception_analysis == NULL)
        return NULL;
    ExceptionSource *source = ee_source(analysis->exception_analysis, source_node);
    return source != NULL && source->summary.graph != NULL ? &source->summary : NULL;
}

bool feng_semantic_validate_abi_exception_effects(const FengSemanticAnalysis *analysis,
                                                 FengSemanticError **errors, size_t *error_count,
                                                 size_t *error_capacity) {
    if (analysis == NULL || analysis->exception_analysis == NULL)
        return false;
    const FengExceptionAnalysis *data = analysis->exception_analysis;
    for (size_t i = 0U; i < data->source_count; ++i) {
        ExceptionSource *source = data->sources[i];
        if (!source->abi || source->imported || source->summary.effects == 0U)
            continue;
        char *types = feng_exception_format(data->graph, source->summary.effects);
        bool ok = types != NULL && ee_abi_error(source, errors, error_count, error_capacity, types);
        free(types);
        if (!ok)
            return false;
    }
    return true;
}

/* Convert cleanup facts to one error per boundary and source-related calls.
 * Diagnostic strings belong only to the error objects, never the graph. */
static bool ee_defer_error(const FengExceptionAnalysis *data, const ExceptionDeferSite *site,
                            FengSemanticError **errors, size_t *count, size_t *capacity) {
    if (!ee_grow((void **)errors, capacity, *count + 1U, sizeof(**errors)))
        return false;
    char *types = feng_exception_format(data->graph, site->effects);
    if (types == NULL)
        return false;
    const char *prefix = "exceptions must not escape the defer block; cannot prove an empty exception set: ";
    size_t length = strlen(types) + strlen(prefix) + 1U;
    FengSemanticError error = {.token = site->statement->token, .code = "AE1507",
                               .path = site->source->program->path};
    error.message = malloc(length);
    if (error.message != NULL)
        snprintf(error.message, length, "%s%s", prefix, types);
    free(types);
    if (error.message == NULL)
        return false;
    size_t related_count = 0U;
    for (size_t i = 0U; i < site->call_count; ++i)
        if (site->calls[i].effects != 0U)
            ++related_count;
    if (related_count != 0U) {
        error.related_locations = calloc(related_count, sizeof(*error.related_locations));
        if (error.related_locations == NULL) {
            free(error.message);
            return false;
        }
        const char message[] = "this call can propagate an exception out of the defer block";
        for (size_t i = 0U; i < site->call_count; ++i) {
            const ExceptionBoundaryCall *call = &site->calls[i];
            if (call->effects == 0U)
                continue;
            char *text = ee_slice((FengSlice){message, sizeof(message) - 1U});
            if (text == NULL) {
                for (size_t j = 0U; j < error.related_location_count; ++j)
                    free(error.related_locations[j].message);
                free(error.related_locations);
                free(error.message);
                return false;
            }
            error.related_locations[error.related_location_count++] = (FengSemanticRelatedLocation){
                .token = call->expression->token, .path = error.path, .message = text};
        }
    }
    (*errors)[(*count)++] = error;
    return true;
}

bool feng_semantic_validate_defer_exception_effects(const FengSemanticAnalysis *analysis,
                                                   FengSemanticError **errors, size_t *error_count,
                                                   size_t *error_capacity) {
    if (analysis == NULL || analysis->exception_analysis == NULL)
        return false;
    const FengExceptionAnalysis *data = analysis->exception_analysis;
    for (size_t i = 0U; i < data->defer_count; ++i) {
        const ExceptionDeferSite *site = &data->defers[i];
        if (site->effects != 0U && !ee_defer_error(data, site, errors, error_count, error_capacity))
            return false;
    }
    return true;
}

bool feng_semantic_exception_call_facts(const FengSemanticAnalysis *analysis, const FengExpr *call,
                                       FengExceptionCallFacts *out_facts) {
    if (out_facts == NULL)
        return false;
    *out_facts = (FengExceptionCallFacts){0};
    if (analysis == NULL || analysis->exception_analysis == NULL || call == NULL)
        return false;
    const FengExceptionAnalysis *data = analysis->exception_analysis;
    for (size_t i = 0U; i < data->call_count; ++i)
        if (data->calls[i].expr == call)
            return feng_semantic_exception_call_at(analysis, i, out_facts);
    return false;
}

size_t feng_semantic_exception_call_count(const FengSemanticAnalysis *analysis) {
    return analysis != NULL && analysis->exception_analysis != NULL
        ? analysis->exception_analysis->call_count : 0U;
}

bool feng_semantic_exception_call_at(const FengSemanticAnalysis *analysis, size_t index,
                                     FengExceptionCallFacts *out_facts) {
    if (out_facts == NULL)
        return false;
    *out_facts = (FengExceptionCallFacts){0};
    if (index >= feng_semantic_exception_call_count(analysis))
        return false;
    const FengExceptionAnalysis *data = analysis->exception_analysis;
    const ExceptionCallSite *site = &data->calls[index];
    *out_facts = (FengExceptionCallFacts){
        .path = site->source->program->path,
        .expression = site->expr,
        .graph = data->graph,
        .effects = site->effects,
        .escaping_effects = site->escaping_effects
    };
    return true;
}

void feng_semantic_exception_analysis_free(FengExceptionAnalysis *data) {
    if (data == NULL)
        return;
    for (size_t i = 0U; i < data->source_count; ++i) {
        feng_exception_template_free(&data->sources[i]->summary);
        free(data->sources[i]->parameters);
        free(data->sources[i]);
    }
    for (size_t i = 0U; i < data->import_count; ++i) {
        free(data->imports[i].functions);
        free(data->imports[i].nodes);
    }
    free(data->sources);
    free(data->origins);
    free(data->calls);
    for (size_t i = 0U; i < data->defer_count; ++i)
        free(data->defers[i].calls);
    free(data->defers);
    free(data->imports);
    feng_exception_graph_release(data->graph);
    free(data);
}
