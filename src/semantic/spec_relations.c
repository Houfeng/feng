/* Phase S1a — spec satisfaction relation sidecar.
 *
 * Builds an authoritative table of (type_decl, spec_decl) satisfaction
 * relations after the main resolve pass succeeds. Each relation entry
 * carries the full provenance chain (declared head/parent, fit head/parent
 * + provider module) so downstream stages — codegen, diagnostics,
 * conflict-by-visible-surface checks (see dev/feng-spec-semantic-draft.md
 * §8.1) — can reason about relations without re-walking the AST.
 *
 * Visibility filtering is intentionally NOT applied here. The table is
 * module-agnostic by design: it captures every globally-derivable source.
 * Callers apply per-consumer filtering via
 * feng_semantic_spec_relation_source_visible_from.
 *
 * The algorithm is a fixed-point closure over each type's declared spec
 * list and each fit decl whose target resolves to that type, expanding
 * parent_specs transitively. Cycles in parent chains are forbidden by the
 * resolver (see validate_spec_parent_spec_list) so a simple visited set
 * suffices. Complexity is O(|fits| · |specs|) in the worst case and stays
 * trivial for realistic projects. */

#include "semantic/semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- Generic decl lookup ----------------------------------------------
 *
 * The post-pass runs without a ResolveContext, so we cannot reuse
 * analyzer.c's `resolve_type_ref_decl`. Resolution here mirrors the
 * simplification used by cyclic.c: scan every program in every module for
 * a decl whose name matches the (single-segment) reference. The main
 * resolver has already rejected ambiguous / unresolved names before we
 * run, so the first match is the intended target. Qualified multi-segment
 * names are resolved by matching the trailing segment against decls in the
 * module whose path equals the leading segments. */

static bool slice_eq_cstr(const FengSlice *s, const char *literal, size_t literal_len) {
    return s->length == literal_len && memcmp(s->data, literal, literal_len) == 0;
}

static bool slices_eq(const FengSlice *a, const FengSlice *b) {
    return a->length == b->length && memcmp(a->data, b->data, a->length) == 0;
}

static const FengSemanticModule *find_module_by_segments(
    const FengSemanticAnalysis *analysis,
    const FengSlice *segments,
    size_t segment_count) {
    size_t mi;

    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *m = &analysis->modules[mi];
        size_t si;

        if (m->segment_count != segment_count) {
            continue;
        }
        for (si = 0U; si < segment_count; ++si) {
            if (!slices_eq(&m->segments[si], &segments[si])) {
                break;
            }
        }
        if (si == segment_count) {
            return m;
        }
    }
    return NULL;
}

static FengSlice decl_typeish_name(const FengDecl *d) {
    FengSlice empty = {NULL, 0U};

    if (d == NULL) {
        return empty;
    }
    switch (d->kind) {
        case FENG_DECL_TYPE:
            return d->as.type_decl.name;
        case FENG_DECL_SPEC:
            return d->as.spec_decl.name;
        default:
            return empty;
    }
}

static const FengDecl *find_decl_by_name_in_module(
    const FengSemanticModule *module,
    const FengSlice *name) {
    size_t pi;

    for (pi = 0U; pi < module->program_count; ++pi) {
        const FengProgram *prog = module->programs[pi];
        size_t di;

        if (prog == NULL) {
            continue;
        }
        for (di = 0U; di < prog->declaration_count; ++di) {
            const FengDecl *d = prog->declarations[di];

            if (d == NULL) {
                continue;
            }
            if (d->kind != FENG_DECL_TYPE && d->kind != FENG_DECL_SPEC) {
                continue;
            }
            {
                FengSlice n = decl_typeish_name(d);

                if (n.length == name->length && memcmp(n.data, name->data, n.length) == 0) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

static const FengDecl *resolve_named_type_or_spec(
    const FengSemanticAnalysis *analysis,
    const FengTypeRef *ref) {
    if (ref == NULL || ref->kind != FENG_TYPE_REF_NAMED) {
        return NULL;
    }
    if (ref->as.named.segment_count == 0U) {
        return NULL;
    }
    if (ref->as.named.segment_count > 1U) {
        const FengSemanticModule *m = find_module_by_segments(
            analysis, ref->as.named.segments, ref->as.named.segment_count - 1U);

        if (m == NULL) {
            return NULL;
        }
        return find_decl_by_name_in_module(
            m, &ref->as.named.segments[ref->as.named.segment_count - 1U]);
    }
    {
        size_t mi;
        const FengSlice *seg = &ref->as.named.segments[0];

        for (mi = 0U; mi < analysis->module_count; ++mi) {
            const FengDecl *d = find_decl_by_name_in_module(&analysis->modules[mi], seg);

            if (d != NULL) {
                /* Suppress the obvious `string` / builtin clash: `string`
                 * etc. cannot be a user spec/type name (the lexer/parser
                 * reject it), so a `slice_eq_cstr` guard is unnecessary
                 * here. The first match is correct because the main
                 * resolver already rejected ambiguous user names. */
                (void)slice_eq_cstr;
                return d;
            }
        }
    }
    return NULL;
}

/* --- Relation table mutation ------------------------------------------ */

static FengSpecRelation *find_or_append_relation(FengSemanticAnalysis *analysis,
                                                 const FengDecl *type_decl,
                                                 const FengDecl *spec_decl) {
    size_t i;

    for (i = 0U; i < analysis->spec_relation_count; ++i) {
        FengSpecRelation *r = &analysis->spec_relations[i];

        if (r->type_decl == type_decl && r->spec_decl == spec_decl) {
            return r;
        }
    }
    if (analysis->spec_relation_count == analysis->spec_relation_capacity) {
        size_t cap = analysis->spec_relation_capacity ? analysis->spec_relation_capacity * 2U : 8U;
        FengSpecRelation *p;

        if (cap > SIZE_MAX / sizeof(*p)) {
            return NULL;
        }
        p = (FengSpecRelation *)realloc(analysis->spec_relations, cap * sizeof(*p));
        if (p == NULL) {
            return NULL;
        }
        analysis->spec_relations = p;
        analysis->spec_relation_capacity = cap;
    }
    {
        FengSpecRelation *r = &analysis->spec_relations[analysis->spec_relation_count++];

        r->type_decl = type_decl;
        r->spec_decl = spec_decl;
        r->sources = NULL;
        r->source_count = 0U;
        r->source_capacity = 0U;
        return r;
    }
}

static bool source_equals(const FengSpecRelationSource *a, const FengSpecRelationSource *b) {
    return a->kind == b->kind &&
           a->via_spec_decl == b->via_spec_decl &&
           a->via_fit_decl == b->via_fit_decl &&
           a->provider_module == b->provider_module;
}

static bool relation_append_source(FengSpecRelation *relation,
                                   const FengSpecRelationSource *source) {
    size_t i;

    for (i = 0U; i < relation->source_count; ++i) {
        if (source_equals(&relation->sources[i], source)) {
            return true;
        }
    }
    if (relation->source_count == relation->source_capacity) {
        size_t cap = relation->source_capacity ? relation->source_capacity * 2U : 4U;
        FengSpecRelationSource *p;

        if (cap > SIZE_MAX / sizeof(*p)) {
            return false;
        }
        p = (FengSpecRelationSource *)realloc(relation->sources, cap * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        relation->sources = p;
        relation->source_capacity = cap;
    }
    relation->sources[relation->source_count++] = *source;
    return true;
}

static bool record_source(FengSemanticAnalysis *analysis,
                          const FengDecl *type_decl,
                          const FengDecl *spec_decl,
                          const FengSpecRelationSource *source) {
    FengSpecRelation *r = find_or_append_relation(analysis, type_decl, spec_decl);

    if (r == NULL) {
        return false;
    }
    return relation_append_source(r, source);
}

/* --- Transitive parent-spec walk -------------------------------------- */

typedef struct ParentClosure {
    const FengDecl **specs;
    size_t count;
    size_t capacity;
} ParentClosure;

static void parent_closure_free(ParentClosure *c) {
    free(c->specs);
    c->specs = NULL;
    c->count = 0U;
    c->capacity = 0U;
}

static bool parent_closure_add(ParentClosure *c, const FengDecl *spec) {
    size_t i;

    for (i = 0U; i < c->count; ++i) {
        if (c->specs[i] == spec) {
            return true;
        }
    }
    if (c->count == c->capacity) {
        size_t cap = c->capacity ? c->capacity * 2U : 4U;
        const FengDecl **p;

        if (cap > SIZE_MAX / sizeof(*p)) {
            return false;
        }
        p = (const FengDecl **)realloc(c->specs, cap * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        c->specs = p;
        c->capacity = cap;
    }
    c->specs[c->count++] = spec;
    return true;
}

/* Recursively collect every transitive parent of `head` into `out`, NOT
 * including `head` itself. The resolver has already rejected cycles in
 * spec parent chains; the visited set in `out` doubles as the cycle guard. */
static bool collect_parent_specs(const FengSemanticAnalysis *analysis,
                                 const FengDecl *head,
                                 ParentClosure *out) {
    size_t i;

    if (head == NULL || head->kind != FENG_DECL_SPEC) {
        return true;
    }
    for (i = 0U; i < head->as.spec_decl.parent_spec_count; ++i) {
        const FengDecl *p = resolve_named_type_or_spec(analysis,
                                                       head->as.spec_decl.parent_specs[i]);

        if (p == NULL || p->kind != FENG_DECL_SPEC) {
            continue;
        }
        if (!parent_closure_add(out, p)) {
            return false;
        }
        if (!collect_parent_specs(analysis, p, out)) {
            return false;
        }
    }
    return true;
}

/* --- Population ------------------------------------------------------- */

static bool record_head_and_parents(FengSemanticAnalysis *analysis,
                                    const FengDecl *type_decl,
                                    const FengDecl *head_spec,
                                    FengSpecRelationSourceKind head_kind,
                                    FengSpecRelationSourceKind parent_kind,
                                    const FengDecl *via_fit_decl,
                                    const FengSemanticModule *provider_module) {
    FengSpecRelationSource src;
    ParentClosure parents = {NULL, 0U, 0U};
    size_t i;

    src.kind = head_kind;
    src.via_spec_decl = head_spec;
    src.via_fit_decl = via_fit_decl;
    src.provider_module = provider_module;
    if (!record_source(analysis, type_decl, head_spec, &src)) {
        return false;
    }
    if (!collect_parent_specs(analysis, head_spec, &parents)) {
        parent_closure_free(&parents);
        return false;
    }
    for (i = 0U; i < parents.count; ++i) {
        FengSpecRelationSource psrc;

        psrc.kind = parent_kind;
        psrc.via_spec_decl = head_spec;
        psrc.via_fit_decl = via_fit_decl;
        psrc.provider_module = provider_module;
        if (!record_source(analysis, type_decl, parents.specs[i], &psrc)) {
            parent_closure_free(&parents);
            return false;
        }
    }
    parent_closure_free(&parents);
    return true;
}

static bool process_type_decl(FengSemanticAnalysis *analysis,
                              const FengDecl *type_decl) {
    size_t i;

    for (i = 0U; i < type_decl->as.type_decl.declared_spec_count; ++i) {
        const FengDecl *head = resolve_named_type_or_spec(
            analysis, type_decl->as.type_decl.declared_specs[i]);

        if (head == NULL || head->kind != FENG_DECL_SPEC) {
            continue;
        }
        if (!record_head_and_parents(analysis,
                                     type_decl,
                                     head,
                                     FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD,
                                     FENG_SPEC_RELATION_SOURCE_DECLARED_PARENT,
                                     NULL,
                                     NULL)) {
            return false;
        }
    }
    return true;
}

static bool process_fit_decl(FengSemanticAnalysis *analysis,
                             const FengSemanticModule *provider_module,
                             const FengDecl *fit_decl) {
    const FengDecl *target = resolve_named_type_or_spec(analysis, fit_decl->as.fit_decl.target);
    size_t i;

    if (target == NULL || target->kind != FENG_DECL_TYPE) {
        return true;
    }
    for (i = 0U; i < fit_decl->as.fit_decl.spec_count; ++i) {
        const FengDecl *head = resolve_named_type_or_spec(
            analysis, fit_decl->as.fit_decl.specs[i]);

        if (head == NULL || head->kind != FENG_DECL_SPEC) {
            continue;
        }
        if (!record_head_and_parents(analysis,
                                     target,
                                     head,
                                     FENG_SPEC_RELATION_SOURCE_FIT_HEAD,
                                     FENG_SPEC_RELATION_SOURCE_FIT_PARENT,
                                     fit_decl,
                                     provider_module)) {
            return false;
        }
    }
    return true;
}

/* --- Public API ------------------------------------------------------- */

static void spec_relations_reset(FengSemanticAnalysis *analysis) {
    size_t i;

    for (i = 0U; i < analysis->spec_relation_count; ++i) {
        free(analysis->spec_relations[i].sources);
    }
    analysis->spec_relation_count = 0U;
}

bool feng_semantic_compute_spec_relations(FengSemanticAnalysis *analysis) {
    size_t mi;

    if (analysis == NULL) {
        return true;
    }

    spec_relations_reset(analysis);

    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        size_t pi;

        for (pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            size_t di;

            if (prog == NULL) {
                continue;
            }
            for (di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];

                if (d == NULL) {
                    continue;
                }
                if (d->kind == FENG_DECL_TYPE) {
                    if (!process_type_decl(analysis, d)) {
                        return false;
                    }
                } else if (d->kind == FENG_DECL_FIT) {
                    if (!process_fit_decl(analysis, mod, d)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

const FengSpecRelation *feng_semantic_lookup_spec_relation(
    const FengSemanticAnalysis *analysis,
    const FengDecl *type_decl,
    const FengDecl *spec_decl) {
    size_t i;

    if (analysis == NULL || type_decl == NULL || spec_decl == NULL) {
        return NULL;
    }
    for (i = 0U; i < analysis->spec_relation_count; ++i) {
        const FengSpecRelation *r = &analysis->spec_relations[i];

        if (r->type_decl == type_decl && r->spec_decl == spec_decl) {
            return r;
        }
    }
    return NULL;
}

bool feng_semantic_spec_relation_source_visible_from(
    const FengSpecRelationSource *source,
    const FengSemanticModule *consumer_module,
    const FengSemanticModule *const *consumer_imports,
    size_t consumer_import_count) {
    size_t i;

    if (source == NULL) {
        return false;
    }
    if (source->kind == FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD ||
        source->kind == FENG_SPEC_RELATION_SOURCE_DECLARED_PARENT) {
        return true;
    }
    if (source->provider_module == consumer_module) {
        return true;
    }
    if (source->via_fit_decl == NULL ||
        source->via_fit_decl->visibility != FENG_VISIBILITY_PUBLIC) {
        return false;
    }
    for (i = 0U; i < consumer_import_count; ++i) {
        if (consumer_imports[i] == source->provider_module) {
            return true;
        }
    }
    return false;
}
