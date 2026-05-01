/* Phase 1B static type-cyclicity analysis.
 *
 * For every user `type` declared in the analysis, build the directed graph of
 * managed references between user types and run Tarjan's strongly-connected-
 * components algorithm. Any type sitting in a non-trivial SCC (self-loop or a
 * cycle of size >= 2) is marked `is_potentially_cyclic`. Codegen forwards the
 * marker to the runtime so the cycle collector only ever inspects objects
 * whose static type can actually participate in a cycle; acyclic objects keep
 * their Phase 1A zero-overhead deterministic ARC release path.
 *
 * Edge model:
 *   - Each user `type` decl is a node.
 *   - For each field of node T, we walk the field's declared type and add an
 *     edge from T to every user-type node referenced by that field.
 *   - Container types (`T[]` and arbitrarily nested arrays) are unwrapped to
 *     their leaf element type before the lookup; an array of T is treated as
 *     a managed reference to T for cyclicity purposes.
 *   - `string`, primitive scalar types and `*T` pointer types contribute no
 *     edges. Unknown / unresolved type names are silently ignored — the
 *     normal semantic pass has already rejected those before we run.
 *
 * Closures are not user-type decls and are not part of this graph; they are
 * handled by codegen-emitted descriptors directly when 1B-3 cycle collector
 * needs to inspect them.
 *
 * Tarjan is implemented iteratively to avoid stack overflow on adversarial
 * inputs. The implementation is O(V + E). */

#include "semantic/semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- Node table -------------------------------------------------------- */

typedef struct CycNode {
    const FengSemanticModule *module;
    const FengDecl           *decl;          /* FENG_DECL_TYPE */
    /* Out-edges into Edges[] (range [edge_begin, edge_end)). */
    size_t edge_begin;
    size_t edge_end;
    /* Tarjan state. */
    int    index;       /* -1 if unvisited */
    int    lowlink;
    bool   on_stack;
    bool   is_cyclic;
} CycNode;

typedef struct CycGraph {
    CycNode *nodes;
    size_t   node_count;
    size_t   node_capacity;
    /* Flat edge target index list. */
    size_t  *edges;
    size_t   edge_count;
    size_t   edge_capacity;
} CycGraph;

static void cyc_graph_free(CycGraph *g) {
    free(g->nodes);
    free(g->edges);
    g->nodes = NULL;
    g->edges = NULL;
    g->node_count = g->node_capacity = 0U;
    g->edge_count = g->edge_capacity = 0U;
}

static bool cyc_nodes_reserve(CycGraph *g, size_t additional) {
    size_t needed = g->node_count + additional;
    if (needed <= g->node_capacity) {
        return true;
    }
    size_t new_capacity = g->node_capacity ? g->node_capacity : 8U;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            return false;
        }
        new_capacity *= 2U;
    }
    CycNode *p = (CycNode *)realloc(g->nodes, new_capacity * sizeof(*p));
    if (p == NULL) {
        return false;
    }
    g->nodes = p;
    g->node_capacity = new_capacity;
    return true;
}

static bool cyc_edges_reserve(CycGraph *g, size_t additional) {
    size_t needed = g->edge_count + additional;
    if (needed <= g->edge_capacity) {
        return true;
    }
    size_t new_capacity = g->edge_capacity ? g->edge_capacity : 16U;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            return false;
        }
        new_capacity *= 2U;
    }
    size_t *p = (size_t *)realloc(g->edges, new_capacity * sizeof(*p));
    if (p == NULL) {
        return false;
    }
    g->edges = p;
    g->edge_capacity = new_capacity;
    return true;
}

static size_t cyc_node_index_of(const CycGraph *g, const FengDecl *decl) {
    /* Linear scan: typical projects have a handful of types per module. If
     * this ever becomes hot we can switch to a pointer-keyed hash table. */
    size_t i;
    for (i = 0U; i < g->node_count; ++i) {
        if (g->nodes[i].decl == decl) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* --- Type lookup ------------------------------------------------------- */

static const FengTypeRef *cyc_unwrap_array(const FengTypeRef *ref) {
    while (ref != NULL && ref->kind == FENG_TYPE_REF_ARRAY) {
        ref = ref->as.inner;
    }
    return ref;
}

/* Resolve `ref` (after array unwrapping) to a user-type node index, or
 * SIZE_MAX if it does not refer to a known user type. Resolution honours
 * the `use` aliases visible to the program that owns `decl_module`'s decls
 * — but for the simple Phase 1A surface, type names live either in the same
 * module or arrive through `use`. We approximate by scanning all modules for
 * a type whose name matches the (single-segment) reference; ambiguity is
 * already rejected by the regular semantic pass, so the first match is the
 * intended target. */
static size_t cyc_resolve_named(const CycGraph *g,
                                const FengSemanticAnalysis *analysis,
                                const FengTypeRef *ref) {
    if (ref == NULL || ref->kind != FENG_TYPE_REF_NAMED) {
        return SIZE_MAX;
    }
    /* Qualified names are not supported on the Phase 1A surface; if they
     * ever appear here, treat them as opaque (no edge). */
    if (ref->as.named.segment_count != 1U) {
        return SIZE_MAX;
    }
    const FengSlice *seg = &ref->as.named.segments[0];

    size_t mi;
    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        size_t pi;
        for (pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            size_t di;
            for (di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if (d->kind != FENG_DECL_TYPE) {
                    continue;
                }
                const FengSlice *n = &d->as.type_decl.name;
                if (n->length == seg->length &&
                    memcmp(n->data, seg->data, seg->length) == 0) {
                    return cyc_node_index_of(g, d);
                }
            }
        }
    }
    return SIZE_MAX;
}

/* --- Build phase ------------------------------------------------------- */

static bool cyc_collect_nodes(CycGraph *g, const FengSemanticAnalysis *analysis) {
    size_t mi;
    for (mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        size_t pi;
        /* External package modules have no local type bodies to analyse. */
        if (mod->origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
            continue;
        }
        for (pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            size_t di;
            for (di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if (d->kind != FENG_DECL_TYPE) {
                    continue;
                }
                if (!cyc_nodes_reserve(g, 1U)) {
                    return false;
                }
                CycNode *n = &g->nodes[g->node_count++];
                n->module = mod;
                n->decl = d;
                n->edge_begin = 0U;
                n->edge_end = 0U;
                n->index = -1;
                n->lowlink = 0;
                n->on_stack = false;
                n->is_cyclic = false;
            }
        }
    }
    return true;
}

static bool cyc_collect_edges(CycGraph *g, const FengSemanticAnalysis *analysis) {
    size_t i;
    for (i = 0U; i < g->node_count; ++i) {
        CycNode *node = &g->nodes[i];
        node->edge_begin = g->edge_count;

        const FengDecl *d = node->decl;
        size_t mc = d->as.type_decl.member_count;
        size_t k;
        for (k = 0U; k < mc; ++k) {
            const FengTypeMember *m = d->as.type_decl.members[k];
            if (m->kind != FENG_TYPE_MEMBER_FIELD) {
                continue;
            }
            const FengTypeRef *fref = cyc_unwrap_array(m->as.field.type);
            size_t target = cyc_resolve_named(g, analysis, fref);
            if (target == SIZE_MAX) {
                continue;
            }
            /* Suppress duplicate edges T->U so Tarjan's lowlink stays
             * minimal and the SCC result is unaffected. Cheap O(degree)
             * lookback over edges added so far for this node. */
            bool dup = false;
            size_t e;
            for (e = node->edge_begin; e < g->edge_count; ++e) {
                if (g->edges[e] == target) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (!cyc_edges_reserve(g, 1U)) {
                return false;
            }
            g->edges[g->edge_count++] = target;
        }

        node->edge_end = g->edge_count;
    }
    return true;
}

/* --- Tarjan SCC (iterative) ------------------------------------------- */

typedef struct CycFrame {
    size_t node;
    size_t next_edge; /* next index into g->edges[] to consider */
} CycFrame;

typedef struct CycStacks {
    CycFrame *call;
    size_t    call_top;
    size_t    call_capacity;
    size_t   *scc;
    size_t    scc_top;
    size_t    scc_capacity;
} CycStacks;

static void cyc_stacks_free(CycStacks *s) {
    free(s->call);
    free(s->scc);
    s->call = NULL;
    s->scc = NULL;
    s->call_top = s->call_capacity = 0U;
    s->scc_top = s->scc_capacity = 0U;
}

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

                /* Pop and mark. */
                while (stacks.scc_top > scc_start) {
                    size_t w = stacks.scc[--stacks.scc_top];
                    g->nodes[w].on_stack = false;
                    if (component_is_cyclic) {
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

bool feng_semantic_compute_type_cyclicity(FengSemanticAnalysis *analysis) {
    if (analysis == NULL) {
        return true;
    }

    /* Reset markers (idempotent). */
    analysis->type_marker_count = 0U;

    CycGraph g = {0};
    bool ok = true;

    if (!cyc_collect_nodes(&g, analysis)) {
        ok = false;
        goto done;
    }
    if (g.node_count == 0U) {
        goto done;
    }
    if (!cyc_collect_edges(&g, analysis)) {
        ok = false;
        goto done;
    }
    if (!cyc_run_tarjan(&g)) {
        ok = false;
        goto done;
    }

    if (!markers_reserve(analysis, g.node_count)) {
        ok = false;
        goto done;
    }
    size_t i;
    for (i = 0U; i < g.node_count; ++i) {
        FengSemanticTypeMarker *m =
            &analysis->type_markers[analysis->type_marker_count++];
        m->module = g.nodes[i].module;
        m->type_decl = g.nodes[i].decl;
        m->is_potentially_cyclic = g.nodes[i].is_cyclic;
    }

done:
    cyc_graph_free(&g);
    return ok;
}

bool feng_semantic_type_is_potentially_cyclic(const FengSemanticAnalysis *analysis,
                                              const FengDecl *type_decl) {
    if (analysis == NULL || type_decl == NULL) {
        return false;
    }
    size_t i;
    for (i = 0U; i < analysis->type_marker_count; ++i) {
        if (analysis->type_markers[i].type_decl == type_decl) {
            return analysis->type_markers[i].is_potentially_cyclic;
        }
    }
    return false;
}
