/* Static strong-reference analysis. Source and imported declarations share
 * the same graph; concrete generic arguments are substituted before edges
 * are collected. SCCs and unknown outgoing relations are conservative cycle
 * risks. This pass changes classification only, not ownership or scheduling. */
#include "semantic/semantic.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Canonical storage-type expressions form a finite DAG, independent of graph
 * edges. Nominal arguments distinguish instances; array writability does not
 * alter owned-reference shape. Unused arguments do not create graph edges. */
typedef struct CycType {
    const FengDecl *decl;
    FengSlice builtin;
    FengTypeRefKind kind;
    size_t inner;
    size_t *args;
    size_t arg_count;
} CycType;

/* A reached storage type, with an expansion parent and iterative SCC state. */
typedef struct CycNode {
    size_t type;
    size_t parent;
    size_t edge_begin;
    size_t edge_end;
    int index;
    int lowlink;
    bool on_stack;
    bool is_cyclic;
    bool unknown;
    bool declaration_root;
} CycNode;

/* All scratch storage is owned by one analysis/query. Type zero is unknown. */
typedef struct CycGraph {
    const FengSemanticAnalysis *analysis;
    CycType *types;
    size_t type_count;
    size_t type_capacity;
    CycNode *nodes;
    size_t node_count;
    size_t node_capacity;
    size_t *edges;
    size_t edge_count;
    size_t edge_capacity;
} CycGraph;

/* Release canonical arguments, graph nodes and edge storage. */
static void cyc_graph_free(CycGraph *g) {
    for (size_t i = 0; i < g->type_count; ++i) free(g->types[i].args);
    free(g->types);
    free(g->nodes);
    free(g->edges);
}

/* Compare source spelling only where no nominal identity is involved. */
static bool cyc_slice_equals(FengSlice a, FengSlice b) {
    return a.length == b.length &&
           (a.length == 0U || memcmp(a.data, b.data, a.length) == 0);
}

/* Compare complete module paths. */
static bool cyc_path_equals(const FengSlice *a, size_t an,
                            const FengSlice *b, size_t bn) {
    if (an != bn) return false;
    for (size_t i = 0; i < an; ++i)
        if (!cyc_slice_equals(a[i], b[i])) return false;
    return true;
}

/* Return declaration name/parameters without treating signatures as storage. */
static FengSlice cyc_decl_name(const FengDecl *decl) {
    switch (decl->kind) {
        case FENG_DECL_TYPE: return decl->as.type_decl.name;
        case FENG_DECL_SPEC: return decl->as.spec_decl.name;
        case FENG_DECL_ENUM: return decl->as.enum_decl.name;
        default: return (FengSlice){0};
    }
}

/* Borrow the generic parameter list of a storage declaration. */
static const FengTypeParam *cyc_decl_params(const FengDecl *decl, size_t *count) {
    *count = 0U;
    if (decl != NULL && decl->kind == FENG_DECL_TYPE) {
        *count = decl->as.type_decl.type_param_count;
        return decl->as.type_decl.type_params;
    }
    if (decl != NULL && decl->kind == FENG_DECL_SPEC) {
        *count = decl->as.spec_decl.type_param_count;
        return decl->as.spec_decl.type_params;
    }
    return NULL;
}

/* Locate the declaration's lexical program, including source-hidden FT ASTs. */
static const FengProgram *cyc_decl_program(const CycGraph *g, const FengDecl *decl,
                                         const FengSemanticModule **module_out) {
    for (size_t m = 0; m < g->analysis->module_count; ++m) {
        const FengSemanticModule *module = &g->analysis->modules[m];
        for (size_t p = 0; p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0; d < program->declaration_count; ++d) {
                if (program->declarations[d] != decl) continue;
                if (module_out != NULL) *module_out = module;
                return program;
            }
        }
    }
    return NULL;
}

/* Resolve an unbound FT reference in its own lexical scope. Resolved nominal
 * identities take precedence; an ambiguous or absent fallback stays unknown. */
static const FengDecl *cyc_resolve_named(const CycGraph *g,
    const FengProgram *program, const FengTypeRef *ref) {
    if (ref->resolution_decl != NULL) return ref->resolution_decl;
    if (ref->resolution_program != NULL) program = ref->resolution_program;
    if (program == NULL || ref->as.named.segment_count == 0U) return NULL;
    const FengSlice *segments = ref->as.named.segments;
    size_t count = ref->as.named.segment_count;
    const FengDecl *found = NULL;
    bool found_local = false;
    for (size_t m = 0; m < g->analysis->module_count; ++m) {
        const FengSemanticModule *module = &g->analysis->modules[m];
        bool local = cyc_path_equals(program->module_segments,
            program->module_segment_count, module->segments, module->segment_count);
        bool visible = count == 1U && local;
        if (count > 1U) visible = cyc_path_equals(segments, count - 1U,
                                                module->segments, module->segment_count);
        for (size_t u = 0; u < program->use_count && !visible; ++u) {
            const FengUseDecl *use = &program->uses[u];
            if (!cyc_path_equals(use->segments, use->segment_count,
                                  module->segments, module->segment_count)) continue;
            visible = (count == 1U && !use->has_alias) ||
                (count == 2U && use->has_alias && cyc_slice_equals(segments[0], use->alias));
        }
        if (!visible) continue;
        for (size_t p = 0; p < module->program_count; ++p) {
            const FengProgram *owner = module->programs[p];
            for (size_t d = 0; d < owner->declaration_count; ++d) {
                const FengDecl *decl = owner->declarations[d];
                size_t arity;
                (void)cyc_decl_params(decl, &arity);
                if (!cyc_slice_equals(cyc_decl_name(decl), segments[count - 1U]) ||
                    arity != ref->as.named.type_arg_count) continue;
                if (!local && decl->visibility != FENG_VISIBILITY_PUBLIC) continue;
                if (count == 1U && found_local && !local) continue;
                if (count == 1U && local && !found_local) found = NULL;
                if (found != NULL && found != decl) return NULL;
                found = decl;
                found_local = local;
            }
        }
    }
    return found;
}

/* Intern one type expression, consuming its owned argument array. */
static size_t cyc_type_intern(CycGraph *g, CycType type) {
    for (size_t i = 1; i < g->type_count; ++i) {
        const CycType *existing = &g->types[i];
        if (existing->decl != type.decl || existing->kind != type.kind ||
            existing->inner != type.inner || existing->arg_count != type.arg_count ||
            !cyc_slice_equals(existing->builtin, type.builtin)) continue;
        if (type.arg_count != 0U &&
            memcmp(existing->args, type.args, type.arg_count * sizeof(*type.args)) != 0) continue;
        free(type.args);
        return i;
    }
    if (g->type_count == g->type_capacity) {
        size_t cap = g->type_capacity == 0U ? 16U : g->type_capacity * 2U;
        if (cap < g->type_capacity || cap > SIZE_MAX / sizeof(*g->types)) {
            free(type.args);
            return SIZE_MAX;
        }
        CycType *types = realloc(g->types, cap * sizeof(*types));
        if (types == NULL) { free(type.args); return SIZE_MAX; }
        g->types = types;
        g->type_capacity = cap;
    }
    g->types[g->type_count] = type;
    return g->type_count++;
}

/* Initialize the sole unknown expression; it is never an acyclic leaf. */
static bool cyc_graph_init(CycGraph *g, const FengSemanticAnalysis *analysis) {
    g->analysis = analysis;
    return cyc_type_intern(g, (CycType){0}) == 0U;
}

/* Substitute only the active declaration's formal parameters. Arguments have
 * already been resolved in their caller's scope, so substitution cannot capture
 * another declaration's parameter/name. */
static size_t cyc_type_ref(CycGraph *g, const FengProgram *program,
    const FengTypeRef *ref, const FengTypeParam *params, const size_t *args,
    size_t param_count) {
    if (ref == NULL) return 0U;
    if (ref->kind != FENG_TYPE_REF_NAMED) {
        size_t inner = cyc_type_ref(g, program, ref->as.inner, params, args, param_count);
        if (inner == SIZE_MAX) return SIZE_MAX;
        return cyc_type_intern(g, (CycType){.kind = ref->kind, .inner = inner});
    }
    if (ref->resolution_decl == NULL && ref->as.named.segment_count == 1U) {
        FengSlice name = ref->as.named.segments[0];
        if (ref->as.named.type_arg_count == 0U) {
            for (size_t i = 0; i < param_count; ++i)
                if (cyc_slice_equals(name, params[i].name)) return args[i];
            if (feng_semantic_is_builtin_type_name(name))
                return cyc_type_intern(g, (CycType){.builtin = name});
        }
    }
    const FengDecl *decl = cyc_resolve_named(g, program, ref);
    if (decl == NULL) return 0U;
    CycType type = {.decl = decl, .arg_count = ref->as.named.type_arg_count};
    if (type.arg_count > 0U) {
        type.args = calloc(type.arg_count, sizeof(*type.args));
        if (type.args == NULL) return SIZE_MAX;
        for (size_t i = 0; i < type.arg_count; ++i) {
            type.args[i] = cyc_type_ref(g, program, ref->as.named.type_args[i],
                                       params, args, param_count);
            if (type.args[i] == SIZE_MAX) { free(type.args); return SIZE_MAX; }
        }
    }
    return cyc_type_intern(g, type);
}

/* Create a declaration root. Missing open arguments remain unknown; a closed
 * caller supplies each argument with its original resolution provenance. */
static size_t cyc_root_type(CycGraph *g, const FengDecl *decl,
    const FengProgram *argument_program, const FengTypeRef *const *args, size_t count) {
    if (decl == NULL) return 0U;
    size_t arity;
    (void)cyc_decl_params(decl, &arity);
    if (count != 0U && (count != arity || args == NULL)) return 0U;
    CycType type = {.decl = decl, .arg_count = arity};
    if (arity > 0U) {
        type.args = calloc(arity, sizeof(*type.args));
        if (type.args == NULL) return SIZE_MAX;
        for (size_t i = 0; i < count; ++i) {
            type.args[i] = cyc_type_ref(g, argument_program, args[i], NULL, NULL, 0U);
            if (type.args[i] == SIZE_MAX) { free(type.args); return SIZE_MAX; }
        }
    }
    return cyc_type_intern(g, type);
}

/* A strict syntactic subterm permits finite nesting such as Box<Box<int>>.
 * Type expressions are interned bottom-up and cannot contain graph cycles. */
static bool cyc_type_contains(const CycGraph *g, size_t outer, size_t inner) {
    if (outer == inner) return true;
    if (outer < inner) return false;
    const CycType *type = &g->types[outer];
    if (type->kind != FENG_TYPE_REF_NAMED)
        return cyc_type_contains(g, type->inner, inner);
    for (size_t i = 0; i < type->arg_count; ++i)
        if (cyc_type_contains(g, type->args[i], inner)) return true;
    return false;
}

/* Reuse exact instances. A changing recursive instantiation is widened to
 * unknown unless its expression strictly shrinks, ensuring finite expansion. */
static size_t cyc_node(CycGraph *g, size_t type, size_t parent) {
    if (type == SIZE_MAX) return SIZE_MAX;
    for (size_t i = 0; i < g->node_count; ++i)
        if (g->nodes[i].type == type) return i;
    const FengDecl *decl = g->types[type].decl;
    if (decl != NULL) {
        for (size_t a = parent; a != SIZE_MAX; a = g->nodes[a].parent) {
            size_t ancestor = g->nodes[a].type;
            if (g->types[ancestor].decl == decl &&
                !cyc_type_contains(g, ancestor, type))
                return cyc_node(g, 0U, parent);
        }
    }
    if (g->node_count == g->node_capacity) {
        size_t cap = g->node_capacity == 0U ? 16U : g->node_capacity * 2U;
        if (cap < g->node_capacity || cap > INT_MAX || cap > SIZE_MAX / sizeof(*g->nodes))
            return SIZE_MAX;
        CycNode *nodes = realloc(g->nodes, cap * sizeof(*nodes));
        if (nodes == NULL) return SIZE_MAX;
        g->nodes = nodes;
        g->node_capacity = cap;
    }
    g->nodes[g->node_count] = (CycNode){.type = type, .parent = parent,
                                      .index = -1, .unknown = type == 0U};
    return g->node_count++;
}

/* Add one owned storage relation, deduplicating this node's contiguous edges. */
static bool cyc_edge(CycGraph *g, size_t source, size_t type) {
    size_t target = cyc_node(g, type, source);
    if (target == SIZE_MAX) return false;
    for (size_t e = g->nodes[source].edge_begin; e < g->edge_count; ++e)
        if (g->edges[e] == target) return true;
    if (g->edge_count == g->edge_capacity) {
        size_t cap = g->edge_capacity == 0U ? 16U : g->edge_capacity * 2U;
        if (cap < g->edge_capacity || cap > SIZE_MAX / sizeof(*g->edges)) return false;
        size_t *edges = realloc(g->edges, cap * sizeof(*edges));
        if (edges == NULL) return false;
        g->edges = edges;
        g->edge_capacity = cap;
    }
    g->edges[g->edge_count++] = target;
    return true;
}

/* Resolve declared or inferred field storage through the existing type facts. */
static size_t cyc_field_type(CycGraph *g, const FengProgram *program,
    const FengTypeMember *field, const FengTypeParam *params,
    const size_t *args, size_t count) {
    const FengTypeRef *ref = field->as.field.type;
    if (ref != NULL) return cyc_type_ref(g, program, ref, params, args, count);
    const FengSemanticTypeFact *fact = feng_semantic_lookup_type_fact(g->analysis, field);
    if (fact == NULL) return 0U;
    switch (fact->kind) {
        case FENG_SEMANTIC_TYPE_FACT_TYPE_REF:
            return cyc_type_ref(g, program, fact->type_ref, params, args, count);
        case FENG_SEMANTIC_TYPE_FACT_BUILTIN:
            return feng_semantic_is_builtin_type_name(fact->builtin_name)
                ? cyc_type_intern(g, (CycType){.builtin = fact->builtin_name}) : 0U;
        case FENG_SEMANTIC_TYPE_FACT_DECL:
            return cyc_root_type(g, fact->type_decl, program, NULL, 0U);
        default: return 0U;
    }
}

/* Expand only instance storage. Arrays and inline aggregates retain their own
 * graph nodes; specs with an erased subject have unknown outgoing references. */
static bool cyc_collect_edges(CycGraph *g) {
    for (size_t i = 0; i < g->node_count; ++i) {
        CycType type = g->types[g->nodes[i].type];
        g->nodes[i].edge_begin = g->edge_count;
        if (type.kind == FENG_TYPE_REF_ARRAY) {
            if (!cyc_edge(g, i, type.inner)) return false;
        } else if (type.kind == FENG_TYPE_REF_NAMED && type.decl != NULL) {
            const FengDecl *decl = type.decl;
            const FengProgram *program = cyc_decl_program(g, decl, NULL);
            size_t count;
            const FengTypeParam *params = cyc_decl_params(decl, &count);
            if (program == NULL || count != type.arg_count) {
                g->nodes[i].unknown = true;
            } else if (decl->kind == FENG_DECL_TYPE) {
                for (size_t f = 0; f < decl->as.type_decl.member_count; ++f) {
                    const FengTypeMember *field = decl->as.type_decl.members[f];
                    if (field->kind != FENG_TYPE_MEMBER_FIELD || field->is_static) continue;
                    size_t target = cyc_field_type(g, program, field, params, type.args, count);
                    if (!cyc_edge(g, i, target)) return false;
                }
            } else if (decl->kind == FENG_DECL_SPEC) {
                if (decl->as.spec_decl.form == FENG_SPEC_FORM_UNION) {
                    for (size_t u = 0; u < decl->as.spec_decl.as.union_form.member_count; ++u) {
                        size_t target = cyc_type_ref(g, program,
                            decl->as.spec_decl.as.union_form.members[u], params, type.args, count);
                        if (!cyc_edge(g, i, target)) return false;
                    }
                } else {
                    g->nodes[i].unknown = true;
                }
            } else if (decl->kind != FENG_DECL_ENUM) {
                g->nodes[i].unknown = true;
            }
        }
        g->nodes[i].edge_end = g->edge_count;
    }
    return true;
}

/* --- Tarjan SCC (iterative) ------------------------------------------- */

/* Iterative DFS frame for Tarjan traversal. */
typedef struct CycFrame {
    size_t node;
    size_t next_edge; /* next index into g->edges[] to consider */
} CycFrame;

/* Explicit traversal and component stacks avoid recursive graph walking. */
typedef struct CycStacks {
    CycFrame *call;
    size_t    call_top;
    size_t    call_capacity;
    size_t   *scc;
    size_t    scc_top;
    size_t    scc_capacity;
} CycStacks;

/* Dispose both Tarjan stacks. */
static void cyc_stacks_free(CycStacks *s) {
    free(s->call);
    free(s->scc);
    s->call = NULL;
    s->scc = NULL;
    s->call_top = s->call_capacity = 0U;
    s->scc_top = s->scc_capacity = 0U;
}

/* Push an explicit DFS frame. */
static bool cyc_call_push(CycStacks *s, size_t node) {
    if (s->call_top + 1U > s->call_capacity) {
        size_t cap = s->call_capacity ? s->call_capacity * 2U : 16U;
        CycFrame *p = (CycFrame *)realloc(s->call, cap * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        s->call = p;
        s->call_capacity = cap;
    }
    s->call[s->call_top++] = (CycFrame){.node = node, .next_edge = 0U};
    return true;
}

/* Record one node on the open SCC stack. */
static bool cyc_scc_push(CycStacks *s, size_t node) {
    if (s->scc_top + 1U > s->scc_capacity) {
        size_t cap = s->scc_capacity ? s->scc_capacity * 2U : 16U;
        size_t *p = (size_t *)realloc(s->scc, cap * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        s->scc = p;
        s->scc_capacity = cap;
    }
    s->scc[s->scc_top++] = node;
    return true;
}

/* Mark SCCs and propagate unknown relations through the component DAG. */
static bool cyc_run_tarjan(CycGraph *g) {
    CycStacks stacks = {0};
    int next_index = 0;
    size_t start;
    bool ok = true;

    for (start = 0U; start < g->node_count && ok; ++start) {
        if (g->nodes[start].index >= 0) {
            continue;
        }

        if (!cyc_call_push(&stacks, start)) {
            ok = false;
            break;
        }
        g->nodes[start].index = next_index;
        g->nodes[start].lowlink = next_index;
        ++next_index;
        if (!cyc_scc_push(&stacks, start)) {
            ok = false;
            break;
        }
        g->nodes[start].on_stack = true;

        while (stacks.call_top > 0U) {
            CycFrame *frame = &stacks.call[stacks.call_top - 1U];
            CycNode  *u = &g->nodes[frame->node];

            if (frame->next_edge < (u->edge_end - u->edge_begin)) {
                size_t v_idx = g->edges[u->edge_begin + frame->next_edge];
                ++frame->next_edge;
                CycNode *v = &g->nodes[v_idx];

                if (v->index < 0) {
                    /* Recurse on v. */
                    v->index = next_index;
                    v->lowlink = next_index;
                    ++next_index;
                    if (!cyc_scc_push(&stacks, v_idx)) {
                        ok = false;
                        break;
                    }
                    v->on_stack = true;
                    if (!cyc_call_push(&stacks, v_idx)) {
                        ok = false;
                        break;
                    }
                } else if (v->on_stack) {
                    if (v->index < u->lowlink) {
                        u->lowlink = v->index;
                    }
                }
                continue;
            }

            /* All edges of u processed. */
            if (u->lowlink == u->index) {
                /* Pop SCC. */
                size_t component_size = 0U;
                bool   has_self_loop = false;
                size_t scc_start = stacks.scc_top;
                /* First pass: count component size and detect self-loops on u
                 * before mutating on_stack flags. */
                size_t i;
                for (i = stacks.scc_top; i > 0U; --i) {
                    size_t w = stacks.scc[i - 1U];
                    ++component_size;
                    if (w == frame->node) {
                        scc_start = i - 1U;
                        break;
                    }
                }
                /* Detect self-loop on the singleton case: an SCC of size 1 is
                 * potentially-cyclic only if u has an edge to itself. */
                if (component_size == 1U) {
                    size_t e;
                    for (e = u->edge_begin; e < u->edge_end; ++e) {
                        if (g->edges[e] == frame->node) {
                            has_self_loop = true;
                            break;
                        }
                    }
                }
                bool component_is_cyclic = (component_size > 1U) || has_self_loop;
                bool component_unknown = false;
                /* Successor SCCs have already completed. Unknown relations
                 * propagate backwards; known descendant cycles do not. */
                for (size_t j = scc_start; j < stacks.scc_top; ++j) {
                    const CycNode *member = &g->nodes[stacks.scc[j]];
                    component_unknown |= member->unknown;
                    for (size_t e = member->edge_begin; e < member->edge_end; ++e)
                        component_unknown |= g->nodes[g->edges[e]].unknown;
                }

                /* Pop and mark. */
                while (stacks.scc_top > scc_start) {
                    size_t w = stacks.scc[--stacks.scc_top];
                    g->nodes[w].on_stack = false;
                    g->nodes[w].unknown = component_unknown;
                    if (component_is_cyclic || component_unknown) {
                        g->nodes[w].is_cyclic = true;
                    }
                }
            }

            /* Pop frame and propagate lowlink to parent. */
            size_t finished = frame->node;
            --stacks.call_top;
            if (stacks.call_top > 0U) {
                CycNode *parent = &g->nodes[stacks.call[stacks.call_top - 1U].node];
                if (g->nodes[finished].lowlink < parent->lowlink) {
                    parent->lowlink = g->nodes[finished].lowlink;
                }
            }
        }
    }

    cyc_stacks_free(&stacks);
    return ok;
}

/* --- Public API -------------------------------------------------------- */

/* Reserve the declaration summary table. */
static bool markers_reserve(FengSemanticAnalysis *analysis, size_t additional) {
    size_t needed = analysis->type_marker_count + additional;
    if (needed <= analysis->type_marker_capacity) {
        return true;
    }
    size_t cap = analysis->type_marker_capacity ? analysis->type_marker_capacity : 8U;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    FengSemanticTypeMarker *p = (FengSemanticTypeMarker *)realloc(
        analysis->type_markers, cap * sizeof(*p));
    if (p == NULL) {
        return false;
    }
    analysis->type_markers = p;
    analysis->type_marker_capacity = cap;
    return true;
}

/* Classify all declaration roots; open parameters are unknown only when a
 * stored field actually reaches them. The temporary graph also contains the
 * concrete instances reached by non-generic declarations. */
bool feng_semantic_compute_type_cyclicity(FengSemanticAnalysis *analysis) {
    if (analysis == NULL) return true;
    analysis->type_marker_count = 0U;
    CycGraph g = {0};
    bool ok = cyc_graph_init(&g, analysis);
    for (size_t m = 0; ok && m < analysis->module_count; ++m) {
        const FengSemanticModule *module = &analysis->modules[m];
        for (size_t p = 0; ok && p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0; ok && d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind != FENG_DECL_TYPE) continue;
                size_t type = cyc_root_type(&g, decl, program, NULL, 0U);
                size_t node = cyc_node(&g, type, SIZE_MAX);
                ok = node != SIZE_MAX;
                if (ok) g.nodes[node].declaration_root = true;
            }
        }
    }
    if (ok) ok = cyc_collect_edges(&g) && cyc_run_tarjan(&g);
    for (size_t i = 0; ok && i < g.node_count; ++i) {
        if (!g.nodes[i].declaration_root) continue;
        ok = markers_reserve(analysis, 1U);
        if (!ok) break;
        const FengDecl *decl = g.types[g.nodes[i].type].decl;
        const FengSemanticModule *module = NULL;
        (void)cyc_decl_program(&g, decl, &module);
        analysis->type_markers[analysis->type_marker_count++] =
            (FengSemanticTypeMarker){.module = module, .type_decl = decl,
                                     .is_potentially_cyclic = g.nodes[i].is_cyclic};
    }
    cyc_graph_free(&g);
    return ok;
}

/* Missing facts cannot establish an acyclic representation. */
bool feng_semantic_type_is_potentially_cyclic(const FengSemanticAnalysis *analysis,
                                             const FengDecl *type_decl) {
    if (analysis == NULL || type_decl == NULL) return true;
    for (size_t i = 0; i < analysis->type_marker_count; ++i)
        if (analysis->type_markers[i].type_decl == type_decl)
            return analysis->type_markers[i].is_potentially_cyclic;
    return true;
}

/* Query a concrete instance without changing the declaration summary or FT.
 * Allocation/lookup failure conservatively retains collector participation. */
bool feng_semantic_type_instance_is_potentially_cyclic(
    const FengSemanticAnalysis *analysis, const FengDecl *decl,
    const FengProgram *argument_program, const FengTypeRef *const *args, size_t count) {
    if (analysis == NULL || decl == NULL) return true;
    CycGraph g = {0};
    bool cyclic = true;
    if (cyc_graph_init(&g, analysis)) {
        size_t type = cyc_root_type(&g, decl, argument_program, args, count);
        size_t node = cyc_node(&g, type, SIZE_MAX);
        if (node != SIZE_MAX && cyc_collect_edges(&g) && cyc_run_tarjan(&g))
            cyclic = g.nodes[node].is_cyclic;
    }
    cyc_graph_free(&g);
    return cyclic;
}
