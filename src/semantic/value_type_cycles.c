/* Phase 1B value-type cycle detection (docs/engineering/feng-value-type-dev.md §3.5, §9.2).
 *
 * Value types (tuples and `@value type` decls) are laid out inline — their
 * size must be known at compile time. A value type that directly or
 * indirectly contains itself as a field would have infinite size, so the
 * language rejects such declarations at compile time.
 *
 * Ordinary (non-value) `type` decls are heap objects referenced through a
 * managed pointer (`FengManagedHeader *`), so their size is fixed
 * regardless of field types — they are NOT subject to this check.
 *
 * Edge model:
 *   - Nodes are value-type decls only (`is_tuple || is_value`).
 *   - For each field of a value-type node T, we unwrap any number of array
 *     layers to reach the leaf element type. If the leaf is a NAMED type
 *     reference that resolves to another value-type decl, we add an edge
 *     from T to that decl.
 *   - Pointer types (`*T`) contribute no edges — pointers are fixed-size.
 *   - Built-in scalar/string types, spec decls, enum decls and ordinary
 *     (managed-pointer) type decls contribute no edges.
 *   - Unresolved type names are silently ignored — the regular semantic
 *     pass has already rejected those before we run.
 *
 * After building the graph we run iterative Tarjan SCC (same pattern as
 * cyclic.c). Any node in a non-trivial SCC (self-loop or cycle of
 * size >= 2) is reported as a compile error (AE1327). We report one error
 * per offending decl so the user sees every participant in a cycle.
 *
 * This check fixes the existing tuple silent-failure bug: prior to this
 * change, a self-referential tuple produced no C output, exit 0, and no
 * diagnostic. After this change, the same input fails with a clear
 * compile error. */

#include "semantic/semantic.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Small helpers (duplicated from analyzer.c to keep this file
 *     self-contained; both are tiny and side-effect-free). ---------------- */

static bool vtc_slice_equals(FengSlice a, FengSlice b) {
    return a.length == b.length &&
           (a.length == 0U || memcmp(a.data, b.data, a.length) == 0);
}

static bool vtc_slice_equals_cstr(FengSlice a, const char *text) {
    size_t text_len = strlen(text);
    if (a.length != text_len) {
        return false;
    }
    return text_len == 0U || memcmp(a.data, text, text_len) == 0;
}

static char *vtc_format_message(const char *format, ...) {
    va_list args;
    va_list copy;
    int needed;
    char *buffer;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0) {
        va_end(copy);
        return NULL;
    }
    buffer = (char *)malloc((size_t)needed + 1U);
    if (buffer == NULL) {
        va_end(copy);
        return NULL;
    }
    (void)vsnprintf(buffer, (size_t)needed + 1U, format, copy);
    va_end(copy);
    return buffer;
}

static bool vtc_append_raw(void **items,
                           size_t *count,
                           size_t *capacity,
                           size_t item_size,
                           const void *value) {
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0U) ? 4U : (*capacity * 2U);
        void *new_items = realloc(*items, new_capacity * item_size);
        if (new_items == NULL) {
            return false;
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    memcpy((char *)(*items) + (*count * item_size), value, item_size);
    ++(*count);
    return true;
}

static bool vtc_append_error(FengSemanticError **errors,
                             size_t *error_count,
                             size_t *error_capacity,
                             const char *path,
                             FengToken token,
                             const char *code,
                             char *message) {
    FengSemanticError error;

    if (message == NULL) {
        code = "IE0001";
        message = vtc_format_message(
            "out of memory during semantic analysis");
        if (message == NULL) {
            return false;
        }
    }
    error.code = code;
    error.path = path;
    error.message = message;
    error.token = token;

    if (!vtc_append_raw((void **)errors,
                        error_count,
                        error_capacity,
                        sizeof(error),
                        &error)) {
        free(message);
        return false;
    }
    return true;
}

/* --- Node table -------------------------------------------------------- */

typedef struct VtcNode {
    const FengSemanticModule *module;
    const FengProgram        *owner_program;
    const FengDecl           *decl;          /* FENG_DECL_TYPE, is_tuple||is_value */
    /* Out-edges into edges[] (range [edge_begin, edge_end)). */
    size_t edge_begin;
    size_t edge_end;
    /* Tarjan state. */
    int    index;       /* -1 if unvisited */
    int    lowlink;
    bool   on_stack;
    bool   is_cyclic;
} VtcNode;

typedef struct VtcGraph {
    VtcNode *nodes;
    size_t   node_count;
    size_t   node_capacity;
    /* Flat edge target index list. */
    size_t  *edges;
    size_t   edge_count;
    size_t   edge_capacity;
} VtcGraph;

static void vtc_graph_free(VtcGraph *g) {
    free(g->nodes);
    free(g->edges);
    g->nodes = NULL;
    g->edges = NULL;
    g->node_count = g->node_capacity = 0U;
    g->edge_count = g->edge_capacity = 0U;
}

static bool vtc_nodes_reserve(VtcGraph *g, size_t additional) {
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
    VtcNode *p = (VtcNode *)realloc(g->nodes, new_capacity * sizeof(*p));
    if (p == NULL) {
        return false;
    }
    g->nodes = p;
    g->node_capacity = new_capacity;
    return true;
}

static bool vtc_edges_reserve(VtcGraph *g, size_t additional) {
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

/* --- Type lookup ------------------------------------------------------- */

/* Unwrap arbitrarily nested array type refs to the innermost element
 * type. `int[][]` -> `int`; `Point[]` -> `Point`; `Point` -> `Point`. */
static const FengTypeRef *vtc_unwrap_array(const FengTypeRef *ref) {
    while (ref != NULL && ref->kind == FENG_TYPE_REF_ARRAY) {
        ref = ref->as.inner;
    }
    return ref;
}

static bool vtc_path_equals(const FengSlice *a, size_t an,
                            const FengSlice *b, size_t bn) {
    if (an != bn) return false;
    for (size_t i = 0U; i < an; ++i) {
        if (!vtc_slice_equals(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

static const FengSemanticModule *vtc_find_module_by_segments(
    const FengSemanticAnalysis *analysis,
    const FengSlice *segments,
    size_t segment_count) {
    if (analysis == NULL) {
        return NULL;
    }
    for (size_t mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        if (vtc_path_equals(mod->segments, mod->segment_count,
                            segments, segment_count)) {
            return mod;
        }
    }
    return NULL;
}

static const FengSemanticModule *vtc_find_alias_target_module(
    const FengSemanticAnalysis *analysis,
    const FengProgram *owner_program,
    FengSlice alias) {
    if (analysis == NULL || owner_program == NULL) {
        return NULL;
    }
    for (size_t ui = 0U; ui < owner_program->use_count; ++ui) {
        const FengUseDecl *use_decl = &owner_program->uses[ui];
        if (!use_decl->has_alias || !vtc_slice_equals(use_decl->alias, alias)) {
            continue;
        }
        return vtc_find_module_by_segments(analysis,
                                           use_decl->segments,
                                           use_decl->segment_count);
    }
    return NULL;
}

static bool vtc_module_visible_by_full_path(const FengProgram *owner_program,
                                            const FengSemanticModule *target) {
    if (owner_program == NULL || target == NULL) {
        return false;
    }
    if (vtc_path_equals(owner_program->module_segments,
                        owner_program->module_segment_count,
                        target->segments,
                        target->segment_count)) {
        return true;
    }
    return target->visibility == FENG_VISIBILITY_PUBLIC;
}

static bool vtc_decl_visible_from_program(const FengProgram *owner_program,
                                          const FengSemanticModule *target_module,
                                          const FengDecl *decl) {
    if (owner_program == NULL || target_module == NULL || decl == NULL) {
        return false;
    }
    if (vtc_path_equals(owner_program->module_segments,
                        owner_program->module_segment_count,
                        target_module->segments,
                        target_module->segment_count)) {
        return true;
    }
    return target_module->visibility == FENG_VISIBILITY_PUBLIC &&
           decl->visibility == FENG_VISIBILITY_PUBLIC;
}

static bool vtc_is_builtin_name(FengSlice name) {
    static const char *builtins[] = {
        "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64",
        "f32", "f64",
        "bool", "string", "void"
    };
    for (size_t i = 0U; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        if (vtc_slice_equals_cstr(name, builtins[i])) {
            return true;
        }
    }
    return false;
}

/* Resolve a NAMED type ref (after array unwrapping) to a value-type node
 * index in the graph, or SIZE_MAX if it does not refer to a known value
 * type. Resolution honours `use` aliases visible to the program that owns
 * the referencing type. Mirrors cyclic.c's `cyc_resolve_named`. */
static size_t vtc_resolve_named(const VtcGraph *g,
                                const FengSemanticAnalysis *analysis,
                                const FengProgram *owner_program,
                                const FengTypeRef *ref) {
    const FengSlice *segments;
    size_t segment_count;
    FengSlice name;

    if (ref == NULL || ref->kind != FENG_TYPE_REF_NAMED) {
        return SIZE_MAX;
    }
    segments = ref->as.named.segments;
    segment_count = ref->as.named.segment_count;
    if (segment_count == 0U) {
        return SIZE_MAX;
    }

    name = segments[segment_count - 1U];

    /* Built-in scalar/string names never resolve to a value-type decl. */
    if (segment_count == 1U && vtc_is_builtin_name(name)) {
        return SIZE_MAX;
    }

    if (segment_count == 1U) {
        for (size_t ni = 0U; ni < g->node_count; ++ni) {
            const VtcNode *node = &g->nodes[ni];
            if (vtc_slice_equals(node->decl->as.type_decl.name, name) &&
                vtc_decl_visible_from_program(owner_program, node->module, node->decl)) {
                return ni;
            }
        }
        return SIZE_MAX;
    }

    if (segment_count == 2U) {
        const FengSemanticModule *alias_target =
            vtc_find_alias_target_module(analysis, owner_program, segments[0]);

        if (alias_target != NULL) {
            for (size_t ni = 0U; ni < g->node_count; ++ni) {
                const VtcNode *node = &g->nodes[ni];
                if (node->module == alias_target &&
                    vtc_slice_equals(node->decl->as.type_decl.name, name) &&
                    vtc_decl_visible_from_program(owner_program, node->module, node->decl)) {
                    return ni;
                }
            }
            return SIZE_MAX;
        }
    }

    const FengSemanticModule *target_module =
        vtc_find_module_by_segments(analysis, segments, segment_count - 1U);
    if (target_module == NULL ||
        !vtc_module_visible_by_full_path(owner_program, target_module)) {
        return SIZE_MAX;
    }
    for (size_t ni = 0U; ni < g->node_count; ++ni) {
        const VtcNode *node = &g->nodes[ni];
        if (node->module == target_module &&
            vtc_slice_equals(node->decl->as.type_decl.name, name) &&
            vtc_decl_visible_from_program(owner_program, node->module, node->decl)) {
            return ni;
        }
    }
    return SIZE_MAX;
}

/* --- Build phase ------------------------------------------------------- */

static bool decl_is_value_type(const FengDecl *d) {
    return d != NULL &&
           d->kind == FENG_DECL_TYPE &&
           (d->as.type_decl.is_tuple || d->as.type_decl.is_value);
}

static bool vtc_collect_nodes(VtcGraph *g, const FengSemanticAnalysis *analysis) {
    for (size_t mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        /* External package modules have no local type bodies to analyse. */
        if (mod->origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
            continue;
        }
        for (size_t pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (size_t di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if (!decl_is_value_type(d)) {
                    continue;
                }
                if (!vtc_nodes_reserve(g, 1U)) {
                    return false;
                }
                VtcNode *n = &g->nodes[g->node_count++];
                n->module = mod;
                n->owner_program = prog;
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

/* Add an edge from `node_index` to `target` if not already present.
 * Suppresses duplicates so Tarjan's lowlink stays minimal. */
static bool vtc_add_edge(VtcGraph *g, size_t node_index, size_t target) {
    VtcNode *node = &g->nodes[node_index];
    /* Suppress duplicate edges T->U so Tarjan's lowlink stays
     * minimal and the SCC result is unaffected. */
    for (size_t e = node->edge_begin; e < g->edge_count; ++e) {
        if (g->edges[e] == target) {
            return true;
        }
    }
    if (!vtc_edges_reserve(g, 1U)) {
        return false;
    }
    g->edges[g->edge_count++] = target;
    return true;
}

/* Recursively collect edges from a TypeRef. Unwraps arrays, resolves the
 * base type, and recursively processes type_args. This handles generic
 * value types like `Box<Node>` where `Node` is a value type passed as a
 * type argument — without this, cycles through generic instantiation
 * would be missed. */
static bool vtc_collect_type_ref_edges(VtcGraph *g,
                                       const FengSemanticAnalysis *analysis,
                                       const FengProgram *owner_program,
                                       size_t node_index,
                                       const FengTypeRef *ref) {
    if (ref == NULL) {
        return true;
    }

    /* Unwrap array layers — `T[]` still requires T to have finite size.
     * Pointer types (`*T`) are not unwrapped: a pointer has fixed size
     * regardless of its referent. */
    const FengTypeRef *unwrapped = vtc_unwrap_array(ref);

    /* Resolve the base type and add edge if it's a value-type node. */
    size_t target = vtc_resolve_named(g, analysis, owner_program, unwrapped);
    if (target != SIZE_MAX) {
        if (!vtc_add_edge(g, node_index, target)) {
            return false;
        }
    }

    /* Recursively process type_args. For `Box<Node>`, this adds an edge
     * to `Node` if it's a value type. For nested generics like
     * `Box<Wrapper<Node>>`, recursion handles the inner type_args. */
    if (unwrapped->kind == FENG_TYPE_REF_NAMED) {
        for (size_t i = 0U; i < unwrapped->as.named.type_arg_count; ++i) {
            if (!vtc_collect_type_ref_edges(g, analysis, owner_program,
                                            node_index,
                                            unwrapped->as.named.type_args[i])) {
                return false;
            }
        }
    }

    return true;
}

static bool vtc_collect_edges(VtcGraph *g, const FengSemanticAnalysis *analysis) {
    for (size_t i = 0U; i < g->node_count; ++i) {
        VtcNode *node = &g->nodes[i];
        node->edge_begin = g->edge_count;

        const FengDecl *d = node->decl;
        size_t mc = d->as.type_decl.member_count;
        for (size_t k = 0U; k < mc; ++k) {
            const FengTypeMember *m = d->as.type_decl.members[k];
            if (m->kind != FENG_TYPE_MEMBER_FIELD) {
                continue;
            }
            if (!vtc_collect_type_ref_edges(g, analysis, node->owner_program,
                                            i, m->as.field.type)) {
                return false;
            }
        }

        node->edge_end = g->edge_count;
    }
    return true;
}

/* --- Tarjan SCC (iterative) ------------------------------------------- */

typedef struct VtcFrame {
    size_t node;
    size_t next_edge;
} VtcFrame;

typedef struct VtcStacks {
    VtcFrame *call;
    size_t    call_top;
    size_t    call_capacity;
    size_t   *scc;
    size_t    scc_top;
    size_t    scc_capacity;
} VtcStacks;

static void vtc_stacks_free(VtcStacks *s) {
    free(s->call);
    free(s->scc);
    s->call = NULL;
    s->scc = NULL;
    s->call_top = s->call_capacity = 0U;
    s->scc_top = s->scc_capacity = 0U;
}

static bool vtc_call_push(VtcStacks *s, size_t node) {
    if (s->call_top + 1U > s->call_capacity) {
        size_t cap = s->call_capacity ? s->call_capacity * 2U : 16U;
        VtcFrame *p = (VtcFrame *)realloc(s->call, cap * sizeof(*p));
        if (p == NULL) {
            return false;
        }
        s->call = p;
        s->call_capacity = cap;
    }
    s->call[s->call_top++] = (VtcFrame){.node = node, .next_edge = 0U};
    return true;
}

static bool vtc_scc_push(VtcStacks *s, size_t node) {
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

static bool vtc_run_tarjan(VtcGraph *g) {
    VtcStacks stacks = {0};
    int next_index = 0;
    bool ok = true;

    for (size_t start = 0U; start < g->node_count && ok; ++start) {
        if (g->nodes[start].index >= 0) {
            continue;
        }
        if (!vtc_call_push(&stacks, start)) {
            ok = false;
            break;
        }
        g->nodes[start].index = next_index;
        g->nodes[start].lowlink = next_index;
        ++next_index;
        if (!vtc_scc_push(&stacks, start)) {
            ok = false;
            break;
        }
        g->nodes[start].on_stack = true;

        while (stacks.call_top > 0U) {
            VtcFrame *frame = &stacks.call[stacks.call_top - 1U];
            VtcNode  *u = &g->nodes[frame->node];

            if (frame->next_edge < (u->edge_end - u->edge_begin)) {
                size_t v_idx = g->edges[u->edge_begin + frame->next_edge];
                ++frame->next_edge;
                VtcNode *v = &g->nodes[v_idx];

                if (v->index < 0) {
                    v->index = next_index;
                    v->lowlink = next_index;
                    ++next_index;
                    if (!vtc_scc_push(&stacks, v_idx)) {
                        ok = false;
                        break;
                    }
                    v->on_stack = true;
                    if (!vtc_call_push(&stacks, v_idx)) {
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
                size_t component_size = 0U;
                bool   has_self_loop = false;
                size_t scc_start = stacks.scc_top;
                /* First pass: count component size and locate scc_start
                 * before mutating on_stack flags. */
                for (size_t i = stacks.scc_top; i > 0U; --i) {
                    size_t w = stacks.scc[i - 1U];
                    ++component_size;
                    if (w == frame->node) {
                        scc_start = i - 1U;
                        break;
                    }
                }
                /* Singleton SCC is cyclic only if it has a self-loop. */
                if (component_size == 1U) {
                    for (size_t e = u->edge_begin; e < u->edge_end; ++e) {
                        if (g->edges[e] == frame->node) {
                            has_self_loop = true;
                            break;
                        }
                    }
                }
                bool component_is_cyclic = (component_size > 1U) || has_self_loop;

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
                VtcNode *parent = &g->nodes[stacks.call[stacks.call_top - 1U].node];
                if (g->nodes[finished].lowlink < parent->lowlink) {
                    parent->lowlink = g->nodes[finished].lowlink;
                }
            }
        }
    }

    vtc_stacks_free(&stacks);
    return ok;
}

/* --- Error reporting --------------------------------------------------- */

/* Emit a compile error for each value-type decl that participates in a
 * cycle. The error message names the decl so the user can identify every
 * participant without having to trace through a long cycle chain. */
static bool vtc_report_errors(const VtcGraph *g,
                              FengSemanticError **errors,
                              size_t *error_count,
                              size_t *error_capacity) {
    for (size_t i = 0U; i < g->node_count; ++i) {
        const VtcNode *node = &g->nodes[i];
        if (!node->is_cyclic) {
            continue;
        }
        const char *kind = node->decl->as.type_decl.is_tuple ? "tuple" : "@value type";
        char *message = vtc_format_message(
            "%s '%.*s' participates in a value-type cycle; "
            "value types must have a finite size and cannot contain "
            "themselves directly or indirectly",
            kind,
            (int)node->decl->as.type_decl.name.length,
            node->decl->as.type_decl.name.data);
        if (!vtc_append_error(errors,
                              error_count,
                              error_capacity,
                              node->owner_program->path,
                              node->decl->token,
                              "AE1327",
                              message)) {
            return false;
        }
    }
    return true;
}

/* --- Public API -------------------------------------------------------- */

bool feng_semantic_detect_value_type_cycles(FengSemanticAnalysis *analysis,
                                            FengSemanticError **errors,
                                            size_t *error_count,
                                            size_t *error_capacity) {
    if (analysis == NULL) {
        return true;
    }

    VtcGraph g = {0};
    bool ok = true;

    if (!vtc_collect_nodes(&g, analysis)) {
        ok = false;
        goto done;
    }
    if (g.node_count == 0U) {
        goto done;
    }
    if (!vtc_collect_edges(&g, analysis)) {
        ok = false;
        goto done;
    }
    if (!vtc_run_tarjan(&g)) {
        ok = false;
        goto done;
    }
    if (!vtc_report_errors(&g, errors, error_count, error_capacity)) {
        ok = false;
        goto done;
    }

done:
    vtc_graph_free(&g);
    return ok;
}
