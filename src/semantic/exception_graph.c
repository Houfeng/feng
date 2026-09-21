#include "semantic/exception_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy optional textual metadata without retaining source buffers. */
static char *eg_copy(const char *text) {
    if (text == NULL)
        return NULL;
    size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

/* Checked geometric storage growth shared by graph containers. */
static bool eg_grow(void **data, size_t *capacity, size_t count, size_t width) {
    if (count <= *capacity)
        return true;
    size_t next = *capacity != 0U ? *capacity : 16U;
    while (next < count) {
        if (next > SIZE_MAX / 2U)
            return false;
        next *= 2U;
    }
    if (next > SIZE_MAX / width)
        return false;
    void *grown = realloc(*data, next * width);
    if (grown == NULL)
        return false;
    *data = grown;
    *capacity = next;
    return true;
}

/* Hash bytes in a platform-independent order; hashes are not serialized. */
static uint64_t eg_hash_bytes(uint64_t hash, const void *data, size_t count) {
    const unsigned char *bytes = data;
    for (size_t i = 0U; i < count; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Compare a term with a prospective intern key. */
static bool eg_same(const FengExceptionNode *node, FengExceptionNodeKind kind, uint32_t x, uint32_t y,
                    uint32_t z, const char *name, const uint32_t *args, size_t count) {
    return node->kind == kind && node->x == x && node->y == y && node->z == z && node->arg_count == count &&
           ((node->name == NULL && name == NULL) ||
            (node->name != NULL && name != NULL && strcmp(node->name, name) == 0)) &&
           (count == 0U || memcmp(node->args, args, count * sizeof(*args)) == 0);
}

/* Rebuild the open-addressing interner before its load factor reaches 1/2. */
static bool eg_rehash(FengExceptionGraph *graph, size_t count) {
    uint32_t *slots = calloc(count, sizeof(*slots));
    if (slots == NULL)
        return false;
    for (size_t i = 0U; i < graph->node_count; ++i) {
        size_t at = (size_t)graph->nodes[i].hash & (count - 1U);
        while (slots[at] != 0U)
            at = (at + 1U) & (count - 1U);
        slots[at] = (uint32_t)i + 1U;
    }
    free(graph->node_slots);
    graph->node_slots = slots;
    graph->node_slot_count = count;
    return true;
}

uint32_t feng_exception_node(FengExceptionGraph *graph, FengExceptionNodeKind kind, uint32_t x, uint32_t y,
                             uint32_t z, const char *name, const uint32_t *args, size_t arg_count) {
    if (graph == NULL || graph->failed)
        return 0U;
    if ((kind != FENG_EXCEPTION_NAMED_TYPE && name != NULL) ||
        arg_count > SIZE_MAX / sizeof(*args) || (arg_count != 0U && args == NULL) ||
        graph->node_count >= UINT32_MAX - 1U) {
        graph->failed = true;
        return 0U;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = eg_hash_bytes(hash, &kind, sizeof(kind));
    hash = eg_hash_bytes(hash, &x, sizeof(x));
    hash = eg_hash_bytes(hash, &y, sizeof(y));
    hash = eg_hash_bytes(hash, &z, sizeof(z));
    hash = eg_hash_bytes(hash, args, arg_count * sizeof(*args));
    if (name != NULL)
        hash = eg_hash_bytes(hash, name, strlen(name) + 1U);
    if (graph->node_slot_count == 0U || graph->node_count + 1U > graph->node_slot_count / 2U) {
        size_t count = graph->node_slot_count != 0U ? graph->node_slot_count * 2U : 64U;
        if (count < graph->node_slot_count || !eg_rehash(graph, count)) {
            graph->failed = true;
            return 0U;
        }
    }
    size_t at = (size_t)hash & (graph->node_slot_count - 1U);
    while (graph->node_slots[at] != 0U) {
        uint32_t id = graph->node_slots[at] - 1U;
        const FengExceptionNode *node = &graph->nodes[id];
        if (node->hash == hash && eg_same(node, kind, x, y, z, name, args, arg_count))
            return id;
        at = (at + 1U) & (graph->node_slot_count - 1U);
    }
    if (!eg_grow((void **)&graph->nodes, &graph->node_capacity, graph->node_count + 1U,
                 sizeof(*graph->nodes))) {
        graph->failed = true;
        return 0U;
    }
    FengExceptionNode node = {0};
    node.kind = kind;
    node.x = x;
    node.y = y;
    node.z = z;
    node.hash = hash;
    node.name = eg_copy(name);
    node.args = arg_count != 0U ? malloc(arg_count * sizeof(*args)) : NULL;
    if ((name != NULL && node.name == NULL) || (arg_count != 0U && node.args == NULL)) {
        free(node.name);
        free(node.args);
        graph->failed = true;
        return 0U;
    }
    if (arg_count != 0U)
        memcpy(node.args, args, arg_count * sizeof(*args));
    node.arg_count = arg_count;
    uint32_t id = (uint32_t)graph->node_count++;
    graph->nodes[id] = node;
    graph->node_slots[at] = id + 1U;
    return id;
}

FengExceptionGraph *feng_exception_graph_create(void) {
    FengExceptionGraph *graph = calloc(1U, sizeof(*graph));
    if (graph == NULL)
        return NULL;
    graph->references = 1U;
    (void)feng_exception_node(graph, FENG_EXCEPTION_EMPTY, 0, 0, 0, NULL, NULL, 0);
    (void)feng_exception_node(graph, FENG_EXCEPTION_TRUE, 0, 0, 0, NULL, NULL, 0);
    if (graph->failed) {
        feng_exception_graph_release(graph);
        return NULL;
    }
    return graph;
}

void feng_exception_graph_retain(FengExceptionGraph *graph) {
    if (graph != NULL)
        ++graph->references;
}

void feng_exception_graph_release(FengExceptionGraph *graph) {
    if (graph == NULL || --graph->references != 0U)
        return;
    for (size_t i = 0U; i < graph->node_count; ++i) {
        free(graph->nodes[i].name);
        free(graph->nodes[i].args);
    }
    for (size_t i = 0U; i < graph->function_count; ++i) {
        FengExceptionFunction *fn = &graph->functions[i];
        free(fn->name);
        for (size_t j = 0U; j < fn->type_count; ++j)
            free(fn->type_names[j]);
        free(fn->type_names);
    }
    free(graph->nodes);
    free(graph->node_slots);
    free(graph->functions);
    free(graph);
}

/* Node IDs provide a deterministic total order within one graph. */
static int eg_compare_id(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

uint32_t feng_exception_union(FengExceptionGraph *graph, uint32_t a, uint32_t b) {
    if (a == 0U || a == b)
        return b;
    if (b == 0U)
        return a;
    FengExceptionNode left = graph->nodes[a], right = graph->nodes[b];
    size_t an = left.kind == FENG_EXCEPTION_UNION ? left.arg_count : 1U;
    size_t bn = right.kind == FENG_EXCEPTION_UNION ? right.arg_count : 1U;
    if (an > SIZE_MAX / sizeof(uint32_t) - bn) {
        graph->failed = true;
        return 0U;
    }
    uint32_t *items = malloc((an + bn) * sizeof(*items));
    if (items == NULL) {
        graph->failed = true;
        return 0U;
    }
    if (left.kind == FENG_EXCEPTION_UNION)
        memcpy(items, left.args, an * sizeof(*items));
    else
        items[0] = a;
    if (right.kind == FENG_EXCEPTION_UNION)
        memcpy(items + an, right.args, bn * sizeof(*items));
    else
        items[an] = b;
    qsort(items, an + bn, sizeof(*items), eg_compare_id);
    size_t count = 0U;
    for (size_t i = 0U; i < an + bn; ++i)
        if (count == 0U || items[i] != items[count - 1U])
            items[count++] = items[i];
    uint32_t result = count == 1U
                          ? items[0]
                          : feng_exception_node(graph, FENG_EXCEPTION_UNION, 0, 0, 0, NULL, items, count);
    free(items);
    return result;
}

uint32_t feng_exception_unknown(FengExceptionGraph *graph) {
    return feng_exception_node(graph, FENG_EXCEPTION_UNKNOWN, 0, 0, 0, NULL, NULL, 0);
}

uint32_t feng_exception_function_add(FengExceptionGraph *graph, const char *name,
                                     const char *const *type_names, size_t type_count, size_t parameter_count,
                                     size_t capture_count) {
    if (graph->failed || graph->function_count >= UINT32_MAX || type_count > SIZE_MAX / sizeof(char *) ||
        !eg_grow((void **)&graph->functions, &graph->function_capacity, graph->function_count + 1U,
                 sizeof(*graph->functions))) {
        graph->failed = true;
        return UINT32_MAX;
    }
    FengExceptionFunction fn = {0};
    fn.name = eg_copy(name != NULL ? name : "<callable>");
    fn.type_names = type_count != 0U ? calloc(type_count, sizeof(*fn.type_names)) : NULL;
    fn.type_count = type_count;
    fn.parameter_count = parameter_count;
    fn.capture_count = capture_count;
    if (fn.name == NULL || (type_count != 0U && fn.type_names == NULL))
        graph->failed = true;
    for (size_t i = 0U; i < type_count && !graph->failed; ++i) {
        fn.type_names[i] = eg_copy(type_names[i]);
        if (fn.type_names[i] == NULL)
            graph->failed = true;
    }
    if (graph->failed) {
        if (fn.type_names != NULL)
            for (size_t i = 0U; i < type_count; ++i)
                free(fn.type_names[i]);
        free(fn.type_names);
        free(fn.name);
        return UINT32_MAX;
    }
    uint32_t id = (uint32_t)graph->function_count++;
    graph->functions[id] = fn;
    return id;
}

/* Import an acyclic term arena; callable edges, unlike term edges, may cycle. */
bool feng_exception_graph_import(FengExceptionGraph *target, const FengExceptionGraph *source,
                                 uint32_t **out_functions, uint32_t **out_nodes) {
    uint32_t *functions = calloc(source->function_count, sizeof(*functions));
    uint32_t *nodes = calloc(source->node_count, sizeof(*nodes));
    if ((source->function_count != 0U && functions == NULL) || nodes == NULL) {
        free(functions);
        free(nodes);
        target->failed = true;
        return false;
    }
    for (size_t i = 0U; i < source->function_count; ++i) {
        const FengExceptionFunction *fn = &source->functions[i];
        functions[i] = feng_exception_function_add(target, fn->name, (const char *const *)fn->type_names,
                                                   fn->type_count, fn->parameter_count, fn->capture_count);
    }
    for (size_t i = 0U; i < source->node_count && !target->failed; ++i) {
        FengExceptionNode n = source->nodes[i];
        uint32_t x = n.x, y = n.y, z = n.z;
        switch (n.kind) {
        case FENG_EXCEPTION_TYPE_PARAMETER:
        case FENG_EXCEPTION_VALUE_PARAMETER:
            x = functions[x];
            break;
        case FENG_EXCEPTION_FUNCTION:
        case FENG_EXCEPTION_METHOD:
            x = functions[x];
            y = nodes[y];
            z = nodes[z];
            break;
        default:
            x = nodes[x];
            y = nodes[y];
            z = nodes[z];
            break;
        }
        uint32_t *args = n.arg_count != 0U ? malloc(n.arg_count * sizeof(*args)) : NULL;
        if (n.arg_count != 0U && args == NULL) {
            target->failed = true;
            break;
        }
        for (size_t j = 0U; j < n.arg_count; ++j)
            args[j] = nodes[n.args[j]];
        nodes[i] = feng_exception_node(target, n.kind, x, y, z, n.name, args, n.arg_count);
        free(args);
    }
    for (size_t i = 0U; i < source->function_count && !target->failed; ++i) {
        target->functions[functions[i]].effects = nodes[source->functions[i].effects];
        target->functions[functions[i]].result = nodes[source->functions[i].result];
    }
    if (target->failed) {
        free(functions);
        free(nodes);
        return false;
    }
    *out_functions = functions;
    *out_nodes = nodes;
    return true;
}

/* Extraction queues function bodies separately from their shells, breaking
 * recursive callable edges while retaining acyclic term ordering. */
typedef struct ExceptionExtraction {
    const FengExceptionGraph *source;
    FengExceptionGraph *target;
    uint32_t *functions, *nodes, *queue;
    size_t queue_count;
} ExceptionExtraction;

/* Reserve an original declaration exactly once in the export closure. */
static uint32_t eg_extract_function(ExceptionExtraction *e, uint32_t id) {
    if (e->functions[id] != UINT32_MAX)
        return e->functions[id];
    const FengExceptionFunction *fn = &e->source->functions[id];
    uint32_t result = feng_exception_function_add(e->target, fn->name, (const char *const *)fn->type_names,
                                                  fn->type_count, fn->parameter_count, fn->capture_count);
    if (result != UINT32_MAX) {
        e->functions[id] = result;
        e->queue[e->queue_count++] = id;
    }
    return result;
}

/* Copy one reachable term and all child terms in postorder. */
static uint32_t eg_extract_node(ExceptionExtraction *e, uint32_t id) {
    if (e->target->failed)
        return 0U;
    if (e->nodes[id] != UINT32_MAX)
        return e->nodes[id];
    FengExceptionNode n = e->source->nodes[id];
    uint32_t x = n.x, y = n.y, z = n.z;
    if (n.kind == FENG_EXCEPTION_TYPE_PARAMETER || n.kind == FENG_EXCEPTION_VALUE_PARAMETER) {
        x = eg_extract_function(e, x);
    } else if (n.kind == FENG_EXCEPTION_FUNCTION || n.kind == FENG_EXCEPTION_METHOD) {
        x = eg_extract_function(e, x);
        y = eg_extract_node(e, y);
        z = eg_extract_node(e, z);
    } else {
        x = eg_extract_node(e, x);
        y = eg_extract_node(e, y);
        z = eg_extract_node(e, z);
    }
    uint32_t *args = n.arg_count != 0U ? malloc(n.arg_count * sizeof(*args)) : NULL;
    if (n.arg_count != 0U && args == NULL) {
        e->target->failed = true;
        return 0U;
    }
    for (size_t i = 0U; i < n.arg_count; ++i)
        args[i] = eg_extract_node(e, n.args[i]);
    uint32_t result = feng_exception_node(e->target, n.kind, x, y, z, n.name, args, n.arg_count);
    free(args);
    e->nodes[id] = result;
    return result;
}

FengExceptionGraph *feng_exception_graph_extract(const FengExceptionGraph *source, const uint32_t *roots,
                                                 const uint32_t *effects, size_t root_count,
                                                 uint32_t *out_roots, uint32_t *out_effects) {
    for (size_t i = 0U; i < root_count; ++i)
        if (roots[i] >= source->function_count || effects[i] >= source->node_count)
            return NULL;
    ExceptionExtraction e = {0};
    e.source = source;
    e.target = feng_exception_graph_create();
    e.functions = malloc(source->function_count * sizeof(*e.functions));
    e.nodes = malloc(source->node_count * sizeof(*e.nodes));
    e.queue = malloc(source->function_count * sizeof(*e.queue));
    if (e.target == NULL || e.nodes == NULL ||
        (source->function_count != 0U && (e.functions == NULL || e.queue == NULL))) {
        feng_exception_graph_release(e.target);
        free(e.functions);
        free(e.nodes);
        free(e.queue);
        return NULL;
    }
    for (size_t i = 0U; i < source->node_count; ++i)
        e.nodes[i] = UINT32_MAX;
    for (size_t i = 0U; i < source->function_count; ++i)
        e.functions[i] = UINT32_MAX;
    e.nodes[0] = 0U;
    e.nodes[1] = 1U;
    for (size_t i = 0U; i < root_count; ++i)
        out_roots[i] = eg_extract_function(&e, roots[i]);
    for (size_t i = 0U; i < root_count && !e.target->failed; ++i)
        out_effects[i] = eg_extract_node(&e, effects[i]);
    for (size_t i = 0U; i < e.queue_count && !e.target->failed; ++i) {
        uint32_t original = e.queue[i], target = e.functions[original];
        uint32_t effects = eg_extract_node(&e, source->functions[original].effects);
        uint32_t result = eg_extract_node(&e, source->functions[original].result);
        e.target->functions[target].effects = effects;
        e.target->functions[target].result = result;
    }
    free(e.functions);
    free(e.nodes);
    free(e.queue);
    if (e.target->failed) {
        feng_exception_graph_release(e.target);
        return NULL;
    }
    return e.target;
}

bool feng_exception_template_copy(FengExceptionTemplate *target, const FengExceptionTemplate *source) {
    *target = (FengExceptionTemplate){0};
    if (source == NULL || source->graph == NULL)
        return true;
    *target = *source;
    feng_exception_graph_retain(target->graph);
    return true;
}

void feng_exception_template_free(FengExceptionTemplate *summary) {
    if (summary == NULL)
        return;
    feng_exception_graph_release(summary->graph);
    *summary = (FengExceptionTemplate){0};
}

/* Expand a temporary type-name buffer with checked arithmetic. */
static bool eg_text(char **text, size_t *length, size_t *capacity, const char *piece) {
    size_t count = strlen(piece);
    if (count > SIZE_MAX - *length - 1U || !eg_grow((void **)text, capacity, *length + count + 1U, 1U))
        return false;
    memcpy(*text + *length, piece, count + 1U);
    *length += count;
    return true;
}

/* Render one type identity, preserving nested generic argument boundaries. */
static bool eg_format_type(const FengExceptionGraph *graph, uint32_t id, char **text, size_t *length,
                           size_t *capacity) {
    const FengExceptionNode *node = &graph->nodes[id];
    switch (node->kind) {
    case FENG_EXCEPTION_TYPE_PARAMETER: {
        const FengExceptionFunction *fn = &graph->functions[node->x];
        return eg_text(text, length, capacity, fn->type_names[node->y]);
    }
    case FENG_EXCEPTION_NAMED_TYPE:
        if (!eg_text(text, length, capacity, node->name))
            return false;
        if (node->arg_count != 0U && !eg_text(text, length, capacity, "<"))
            return false;
        for (size_t i = 0U; i < node->arg_count; ++i) {
            if ((i != 0U && !eg_text(text, length, capacity, ", ")) ||
                !eg_format_type(graph, node->args[i], text, length, capacity))
                return false;
        }
        return node->arg_count == 0U || eg_text(text, length, capacity, ">");
    default:
        return eg_text(text, length, capacity, "unknown");
    }
}

/* Temporary type names; ownership never escapes into the immutable graph. */
typedef struct ExceptionTypeNames {
    char **items;
    size_t count, capacity;
} ExceptionTypeNames;

/* A solved guard contributes its possible types, not its proof condition. */
static bool eg_collect_type_names(const FengExceptionGraph *graph, uint32_t id,
                                  ExceptionTypeNames *names) {
    const FengExceptionNode *node = &graph->nodes[id];
    if (node->kind == FENG_EXCEPTION_EMPTY)
        return true;
    if (node->kind == FENG_EXCEPTION_UNION) {
        for (size_t i = 0U; i < node->arg_count; ++i)
            if (!eg_collect_type_names(graph, node->args[i], names))
                return false;
        return true;
    }
    if (node->kind == FENG_EXCEPTION_GUARDED || node->kind == FENG_EXCEPTION_THROW)
        return eg_collect_type_names(graph,
            node->kind == FENG_EXCEPTION_GUARDED ? node->y : node->x, names);
    if (!eg_grow((void **)&names->items, &names->capacity, names->count + 1U, sizeof(*names->items)))
        return false;
    char *text = NULL;
    size_t length = 0U, capacity = 0U;
    if (!eg_format_type(graph, id, &text, &length, &capacity)) {
        free(text);
        return false;
    }
    names->items[names->count++] = text;
    return true;
}

/* Stable ordering groups equal type names, including filtered unknown sets. */
static int eg_compare_type_names(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

char *feng_exception_format(const FengExceptionGraph *graph, uint32_t effects) {
    if (graph == NULL || effects >= graph->node_count)
        return NULL;
    ExceptionTypeNames names = {0};
    char *text = NULL;
    size_t length = 0U, capacity = 0U;
    bool ok = eg_collect_type_names(graph, effects, &names);
    if (ok && names.count == 0U)
        ok = eg_text(&text, &length, &capacity, "none");
    if (ok && names.count > 1U)
        qsort(names.items, names.count, sizeof(*names.items), eg_compare_type_names);
    for (size_t i = 0U; i < names.count && ok; ++i) {
        if (i != 0U && strcmp(names.items[i - 1U], names.items[i]) == 0)
            continue;
        ok = (length == 0U || eg_text(&text, &length, &capacity, ", ")) &&
             eg_text(&text, &length, &capacity, names.items[i]);
    }
    for (size_t i = 0U; i < names.count; ++i)
        free(names.items[i]);
    free(names.items);
    if (!ok) {
        free(text);
        return NULL;
    }
    return text;
}
