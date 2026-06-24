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

/* --- Subject key helpers (local, mirrors spec_witnesses.c) ------------ */

static bool rel_slice_eq_cstr_local(FengSlice s, const char *text, size_t tlen) {
    return s.length == tlen && memcmp(s.data, text, tlen) == 0;
}

#define REL_SEQ(s, lit) rel_slice_eq_cstr_local((s), (lit), sizeof(lit) - 1U)

static const char *rel_builtin_canonical_name(FengSlice name) {
    /* After AST alias normalization (dev/feng-scalar-alias-optimize.md §6),
     * only canonical width-explicit names reach this function. */
    if (REL_SEQ(name, "i8"))                                 { return "i8"; }
    if (REL_SEQ(name, "i16"))                                { return "i16"; }
    if (REL_SEQ(name, "i32"))                                { return "i32"; }
    if (REL_SEQ(name, "i64"))                                { return "i64"; }
    if (REL_SEQ(name, "u8"))                                 { return "u8"; }
    if (REL_SEQ(name, "u16"))                                { return "u16"; }
    if (REL_SEQ(name, "u32"))                                { return "u32"; }
    if (REL_SEQ(name, "u64"))                                { return "u64"; }
    if (REL_SEQ(name, "f32"))                                { return "f32"; }
    if (REL_SEQ(name, "f64"))                                { return "f64"; }
    if (REL_SEQ(name, "bool"))                               { return "bool"; }
    if (REL_SEQ(name, "string"))                             { return "string"; }
    if (REL_SEQ(name, "void"))                               { return "void"; }
    return NULL;
}

/* Returns the canonical builtin name for a single-segment non-generic NAMED
 * type ref, or NULL if the ref is not a builtin scalar/reference name. */
static const char *rel_builtin_canonical_name_for_type_ref(
        const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count != 1U ||
        type_ref->as.named.type_arg_count != 0U) {
        return NULL;
    }
    return rel_builtin_canonical_name(type_ref->as.named.segments[0]);
}

/* Build a subject key from a fit target type ref.  Tries (in order):
 *   1. User named decl (if `resolved_type_decl` is a FENG_DECL_TYPE / ENUM).
 *   2. Builtin canonical name (single-segment NAMED ref that maps to a
 *      built-in type).
 *   3. Structured array key (ARRAY ref chain).
 * Returns false if none of the above apply (target should be skipped). */
static bool rel_build_subject_key(
        const FengDecl *resolved_type_decl,
        const FengTypeRef *target_type_ref,
        FengSemanticSubjectKey *out) {
    if (out == NULL) {
        return false;
    }
    if (resolved_type_decl != NULL &&
        (resolved_type_decl->kind == FENG_DECL_TYPE ||
         resolved_type_decl->kind == FENG_DECL_ENUM)) {
        *out = feng_semantic_subject_key_for_type_decl(resolved_type_decl);
        return true;
    }
    {
        const char *bname = rel_builtin_canonical_name_for_type_ref(target_type_ref);

        if (bname != NULL) {
            *out = feng_semantic_subject_key_for_builtin(bname);
            return true;
        }
    }
    return feng_semantic_subject_key_init_array_from_type_ref(out, target_type_ref);
}

static bool rel_type_ref_key_equal(const FengTypeRef *left, const FengTypeRef *right);
static bool rel_slices_eq(const FengSlice *a, const FengSlice *b) {
    return a->length == b->length &&
           (a->length == 0U || memcmp(a->data, b->data, a->length) == 0);
}


static bool rel_type_ref_key_equal(const FengTypeRef *left, const FengTypeRef *right) {
    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
        case FENG_TYPE_REF_NAMED: {
            const char *lb = rel_builtin_canonical_name_for_type_ref(left);
            const char *rb = rel_builtin_canonical_name_for_type_ref(right);

            if (lb != NULL || rb != NULL) {
                return lb != NULL && rb != NULL && strcmp(lb, rb) == 0;
            }
            if (left->as.named.segment_count != right->as.named.segment_count ||
                left->as.named.type_arg_count != right->as.named.type_arg_count) {
                return false;
            }
            for (size_t i = 0U; i < left->as.named.segment_count; ++i) {
                if (!rel_slices_eq(&left->as.named.segments[i],
                                   &right->as.named.segments[i])) {
                    return false;
                }
            }
            for (size_t i = 0U; i < left->as.named.type_arg_count; ++i) {
                if (!rel_type_ref_key_equal(left->as.named.type_args[i],
                                            right->as.named.type_args[i])) {
                    return false;
                }
            }
            return true;
        }
        case FENG_TYPE_REF_POINTER:
            return rel_type_ref_key_equal(left->as.inner, right->as.inner);
        case FENG_TYPE_REF_ARRAY:
            return left->array_element_writable == right->array_element_writable &&
                   rel_type_ref_key_equal(left->as.inner, right->as.inner);
        default:
            return false;
    }
}

static bool rel_subject_key_equals(
        const FengSemanticSubjectKey *left,
        const FengSemanticSubjectKey *right) {
    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
        case FENG_SEMANTIC_SUBJECT_KEY_INVALID:
            return true;
        case FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL:
            return left->as.type_decl == right->as.type_decl;
        case FENG_SEMANTIC_SUBJECT_KEY_BUILTIN:
            return left->as.builtin_canonical_name != NULL &&
                   right->as.builtin_canonical_name != NULL &&
                   strcmp(left->as.builtin_canonical_name,
                          right->as.builtin_canonical_name) == 0;
        case FENG_SEMANTIC_SUBJECT_KEY_ARRAY:
            return left->as.array.rank == right->as.array.rank &&
                   left->as.array.writable_mask == right->as.array.writable_mask &&
                   rel_type_ref_key_equal(left->as.array.element_type_ref,
                                          right->as.array.element_type_ref);
        default:
            return false;
    }
}

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
        case FENG_DECL_ENUM:
            return d->as.enum_decl.name;
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
            if (d->kind != FENG_DECL_TYPE && d->kind != FENG_DECL_ENUM &&
                d->kind != FENG_DECL_SPEC) {
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

static FengSpecRelation *find_or_append_relation(
        FengSemanticAnalysis *analysis,
        const FengSemanticSubjectKey *subject_key,
        const FengDecl *spec_decl) {
    size_t i;

    for (i = 0U; i < analysis->spec_relation_count; ++i) {
        FengSpecRelation *r = &analysis->spec_relations[i];

        if (rel_subject_key_equals(&r->subject_key, subject_key) &&
            r->spec_decl == spec_decl) {
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

        r->subject_key = *subject_key;
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
                          const FengSemanticSubjectKey *subject_key,
                          const FengDecl *spec_decl,
                          const FengSpecRelationSource *source) {
    FengSpecRelation *r = find_or_append_relation(analysis, subject_key, spec_decl);

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

        if (p == NULL || p->kind != FENG_DECL_SPEC ||
            p->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
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
                                    const FengSemanticSubjectKey *subject_key,
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
    if (!record_source(analysis, subject_key, head_spec, &src)) {
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
        if (!record_source(analysis, subject_key, parents.specs[i], &psrc)) {
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
    FengSemanticSubjectKey subject_key =
        feng_semantic_subject_key_for_type_decl(type_decl);

    for (i = 0U; i < type_decl->as.type_decl.declared_spec_count; ++i) {
        const FengDecl *head = resolve_named_type_or_spec(
            analysis, type_decl->as.type_decl.declared_specs[i]);

        if (head == NULL || head->kind != FENG_DECL_SPEC ||
            head->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            continue;
        }
        if (!record_head_and_parents(analysis,
                                     &subject_key,
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
    const FengTypeRef *target_ref = fit_decl->as.fit_decl.target;
    const FengDecl *resolved_target = resolve_named_type_or_spec(analysis, target_ref);
    FengSemanticSubjectKey subject_key;
    size_t i;

    /* Skip fits that have no spec list (non-spec fit bodies). */
    if (fit_decl->as.fit_decl.spec_count == 0U) {
        return true;
    }
    /* Build subject key from the fit target.  Handles user types, builtin
     * canonical names, and structured array targets. */
    if (!rel_build_subject_key(resolved_target, target_ref, &subject_key)) {
        return true; /* unrecognised target form — skip gracefully */
    }
    for (i = 0U; i < fit_decl->as.fit_decl.spec_count; ++i) {
        const FengDecl *head = resolve_named_type_or_spec(
            analysis, fit_decl->as.fit_decl.specs[i]);

        if (head == NULL || head->kind != FENG_DECL_SPEC ||
            head->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            continue;
        }
        if (!record_head_and_parents(analysis,
                                     &subject_key,
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
    const FengSemanticSubjectKey *subject_key,
    const FengDecl *spec_decl) {
    size_t i;

    if (analysis == NULL || subject_key == NULL ||
        subject_key->kind == FENG_SEMANTIC_SUBJECT_KEY_INVALID ||
        spec_decl == NULL) {
        return NULL;
    }
    for (i = 0U; i < analysis->spec_relation_count; ++i) {
        const FengSpecRelation *r = &analysis->spec_relations[i];

        if (rel_subject_key_equals(&r->subject_key, subject_key) &&
            r->spec_decl == spec_decl) {
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
