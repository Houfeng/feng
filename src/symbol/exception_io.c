#include "symbol/exception_io.h"

#include <stdlib.h>
#include <string.h>

/* The root's third word is a solved node ID, never a string-pool ID. */
enum { EI_STRUCTURED_ROOTS = 1U };

/* A checked byte stream; the cursor never advances past the supplied span. */
typedef struct ExceptionBytes {
    unsigned char *data;
    size_t size, capacity, cursor;
    bool failed;
} ExceptionBytes;

/* Append one fixed little-endian word, checking every size operation. */
static void ei_put(ExceptionBytes *b, uint32_t value) {
    if (b->failed)
        return;
    if (b->size > SIZE_MAX - 4U) {
        b->failed = true;
        return;
    }
    if (b->size + 4U > b->capacity) {
        size_t next = b->capacity != 0U ? b->capacity * 2U : 256U;
        if (next < b->capacity || next < b->size + 4U) {
            b->failed = true;
            return;
        }
        unsigned char *data = realloc(b->data, next);
        if (data == NULL) {
            b->failed = true;
            return;
        }
        b->data = data;
        b->capacity = next;
    }
    for (unsigned i = 0U; i < 4U; ++i)
        b->data[b->size++] = (unsigned char)(value >> (i * 8U));
}

/* Read a word without unaligned access or trusting a count from the file. */
static uint32_t ei_get(ExceptionBytes *b) {
    if (b->failed || b->cursor > b->size || b->size - b->cursor < 4U) {
        b->failed = true;
        return 0U;
    }
    uint32_t result = 0U;
    for (unsigned i = 0U; i < 4U; ++i)
        result |= (uint32_t)b->data[b->cursor++] << (i * 8U);
    return result;
}

/* Intern required strings, with zero reserved for absent metadata. */
static uint32_t ei_string(ExceptionBytes *b, FengExceptionInternString intern, void *user, const char *text) {
    if (text == NULL)
        return 0U;
    uint32_t id = intern(user, text);
    if (id == 0U)
        b->failed = true;
    return id;
}

bool feng_symbol_exception_write(const FengSymbolExceptionRoot *roots, size_t count,
                                 FengExceptionInternString intern, void *user, unsigned char **out_data,
                                 size_t *out_size, const char *path, FengSymbolError *error) {
    *out_data = NULL;
    *out_size = 0U;
    ExceptionBytes bytes = {0};
    FengExceptionGraph *graph = NULL;
    uint32_t *ids = NULL, *mapped = NULL, *effects = NULL, *mapped_effects = NULL;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(*ids))
        goto fail;
    if (count != 0U) {
        ids = malloc(count * sizeof(*ids));
        mapped = malloc(count * sizeof(*mapped));
        effects = malloc(count * sizeof(*effects));
        mapped_effects = malloc(count * sizeof(*mapped_effects));
    }
    if (count != 0U && (ids == NULL || mapped == NULL || effects == NULL || mapped_effects == NULL))
        goto fail;
    if (count != 0U) {
        for (size_t i = 0U; i < count; ++i) {
            if (roots[i].summary.graph != roots[0].summary.graph || roots[i].summary.graph == NULL)
                goto fail;
            ids[i] = roots[i].summary.function;
            effects[i] = roots[i].summary.effects;
        }
        graph = feng_exception_graph_extract(roots[0].summary.graph, ids, effects, count,
                                             mapped, mapped_effects);
    } else
        graph = feng_exception_graph_create();
    if (graph == NULL)
        goto fail;
    ei_put(&bytes, (uint32_t)count);
    ei_put(&bytes, (uint32_t)graph->function_count);
    ei_put(&bytes, (uint32_t)graph->node_count);
    ei_put(&bytes, EI_STRUCTURED_ROOTS);
    for (size_t i = 0U; i < count; ++i) {
        ei_put(&bytes, roots[i].symbol);
        ei_put(&bytes, mapped[i]);
        ei_put(&bytes, mapped_effects[i]);
    }
    for (size_t i = 0U; i < graph->function_count; ++i) {
        const FengExceptionFunction *fn = &graph->functions[i];
        if (fn->type_count > UINT32_MAX || fn->parameter_count > UINT32_MAX || fn->capture_count > UINT32_MAX)
            goto fail;
        ei_put(&bytes, ei_string(&bytes, intern, user, fn->name));
        ei_put(&bytes, (uint32_t)fn->type_count);
        ei_put(&bytes, (uint32_t)fn->parameter_count);
        ei_put(&bytes, (uint32_t)fn->capture_count);
        ei_put(&bytes, fn->effects);
        ei_put(&bytes, fn->result);
        ei_put(&bytes, 0U);
        for (size_t j = 0U; j < fn->type_count; ++j)
            ei_put(&bytes, ei_string(&bytes, intern, user, fn->type_names[j]));
    }
    for (size_t i = 0U; i < graph->node_count; ++i) {
        const FengExceptionNode *node = &graph->nodes[i];
        if (node->arg_count > UINT32_MAX || node->kind > FENG_EXCEPTION_GUARDED ||
            (node->kind != FENG_EXCEPTION_NAMED_TYPE && node->name != NULL))
            goto fail;
        ei_put(&bytes, (uint32_t)node->kind);
        ei_put(&bytes, node->x);
        ei_put(&bytes, node->y);
        ei_put(&bytes, node->z);
        ei_put(&bytes, ei_string(&bytes, intern, user, node->name));
        ei_put(&bytes, (uint32_t)node->arg_count);
        ei_put(&bytes, 0U);
        for (size_t j = 0U; j < node->arg_count; ++j)
            ei_put(&bytes, node->args[j]);
    }
    if (bytes.failed)
        goto fail;
    feng_exception_graph_release(graph);
    free(ids);
    free(mapped);
    free(effects);
    free(mapped_effects);
    *out_data = bytes.data;
    *out_size = bytes.size;
    return true;
fail:
    feng_exception_graph_release(graph);
    free(ids);
    free(mapped);
    free(effects);
    free(mapped_effects);
    free(bytes.data);
    return feng_symbol_internal_set_error(error, path, (FengToken){0},
                                          "failed to encode exception summary dependency graph");
}

/* Require the indicated term sort. UNKNOWN is never a Boolean condition;
 * EMPTY also represents Boolean false, and equality atoms only label decisions. */
enum {
    EI_TYPE = 1U, EI_VALUE = 2U, EI_EFFECT = 4U, EI_BOOL = 8U, EI_ANY = 7U,
    EI_SOLVED_EFFECT = 16U, EI_EQUALITY = 32U
};

/* Validate one record before it is interned, including its ordered children. */
static unsigned ei_node_sort(const FengExceptionGraph *g, uint32_t kind, uint32_t x, uint32_t y, uint32_t z,
                             const char *name, const uint32_t *args, size_t count, const unsigned *sorts,
                             size_t index) {
    if (kind != FENG_EXCEPTION_DECISION && z != 0U)
        return 0U;
    if (kind != FENG_EXCEPTION_NAMED_TYPE && name != NULL)
        return 0U;
    if (kind == FENG_EXCEPTION_TYPE_PARAMETER || kind == FENG_EXCEPTION_VALUE_PARAMETER) {
        if (x >= g->function_count || count != 0U)
            return 0U;
        const FengExceptionFunction *fn = &g->functions[x];
        size_t slots =
            kind == FENG_EXCEPTION_TYPE_PARAMETER ? fn->type_count : fn->parameter_count + fn->capture_count;
        return y < slots ? (kind == FENG_EXCEPTION_TYPE_PARAMETER ? EI_TYPE : EI_VALUE) : 0U;
    }
    if (kind == FENG_EXCEPTION_FUNCTION || kind == FENG_EXCEPTION_METHOD) {
        if (x >= g->function_count || y >= index)
            return 0U;
        const FengExceptionFunction *fn = &g->functions[x];
        if (count != fn->type_count + (kind == FENG_EXCEPTION_FUNCTION ? fn->capture_count : 0U))
            return 0U;
        for (size_t i = 0U; i < count; ++i)
            if ((sorts[args[i]] & (i < fn->type_count ? EI_TYPE : EI_VALUE)) == 0U)
                return 0U;
        return kind == FENG_EXCEPTION_FUNCTION ? (y == 0U ? EI_VALUE : 0U)
               : (sorts[y] & EI_VALUE) != 0U   ? EI_VALUE
                                               : 0U;
    }
    if (x >= (index != 0U ? index : 1U) || y >= (index != 0U ? index : 1U) ||
        z >= (index != 0U ? index : 1U))
        return 0U;
    switch ((FengExceptionNodeKind)kind) {
    case FENG_EXCEPTION_EMPTY:
        return index == 0U && x == 0U && y == 0U && count == 0U
                   ? EI_ANY | EI_BOOL | EI_SOLVED_EFFECT : 0U;
    case FENG_EXCEPTION_TRUE:
        return index == 1U && x == 0U && y == 0U && count == 0U ? EI_BOOL : 0U;
    case FENG_EXCEPTION_UNKNOWN:
        if (x != 0U || y != 0U)
            return 0U;
        for (size_t i = 0U; i < count; ++i)
            if ((sorts[args[i]] & EI_TYPE) == 0U)
                return 0U;
        return (count != 0U ? EI_EFFECT : EI_ANY) | EI_SOLVED_EFFECT;
    case FENG_EXCEPTION_NAMED_TYPE:
        if (x != 0U || y != 0U || name == NULL || name[0] == '\0')
            return 0U;
        for (size_t i = 0U; i < count; ++i)
            if ((sorts[args[i]] & EI_TYPE) == 0U)
                return 0U;
        return EI_TYPE;
    case FENG_EXCEPTION_UNION: {
        if (x != 0U || y != 0U || count < 2U)
            return 0U;
        unsigned sort = EI_TYPE | EI_VALUE | EI_EFFECT | EI_SOLVED_EFFECT;
        for (size_t i = 0U; i < count; ++i)
            sort &= sorts[args[i]];
        return sort;
    }
    case FENG_EXCEPTION_VALUE_TYPE:
    case FENG_EXCEPTION_THROW:
        return count == 0U && y == 0U && (sorts[x] & EI_TYPE) != 0U
                   ? (kind == FENG_EXCEPTION_THROW ? EI_EFFECT | EI_SOLVED_EFFECT : EI_VALUE)
                   : 0U;
    case FENG_EXCEPTION_TYPED_VALUE:
        return count == 0U && (sorts[x] & EI_VALUE) != 0U && (sorts[y] & EI_TYPE) != 0U ? EI_VALUE : 0U;
    case FENG_EXCEPTION_EXCLUDE:
    case FENG_EXCEPTION_SELECT:
        return count == 0U && (sorts[x] & EI_EFFECT) != 0U && (sorts[y] & EI_TYPE) != 0U ? EI_EFFECT : 0U;
    case FENG_EXCEPTION_WHEN:
        return count == 0U && (sorts[x] & EI_EFFECT) != 0U ? sorts[y] & (EI_EFFECT | EI_VALUE) : 0U;
    case FENG_EXCEPTION_EQUAL_TYPE:
        return count == 0U && (sorts[x] & EI_TYPE) != 0U && (sorts[y] & EI_TYPE) != 0U
                   ? EI_EQUALITY : 0U;
    case FENG_EXCEPTION_DECISION:
        return count == 0U && (sorts[x] & EI_EQUALITY) != 0U &&
               (sorts[y] & EI_BOOL) != 0U && (sorts[z] & EI_BOOL) != 0U ? EI_BOOL : 0U;
    case FENG_EXCEPTION_GUARDED:
        return count == 0U && (sorts[x] & EI_BOOL) != 0U
                   ? sorts[y] & (EI_EFFECT | EI_VALUE | EI_SOLVED_EFFECT) : 0U;
    case FENG_EXCEPTION_CALL:
    case FENG_EXCEPTION_CALL_RESULT:
        if (y != 0U || (sorts[x] & EI_VALUE) == 0U)
            return 0U;
        for (size_t i = 0U; i < count; ++i)
            if ((sorts[args[i]] & EI_VALUE) == 0U)
                return 0U;
        return kind == FENG_EXCEPTION_CALL ? EI_EFFECT : EI_VALUE;
    default:
        return 0U;
    }
}

void feng_symbol_exception_roots_free(FengSymbolExceptionRoot *roots, size_t count) {
    if (roots == NULL)
        return;
    for (size_t i = 0U; i < count; ++i)
        feng_exception_template_free(&roots[i].summary);
    free(roots);
}

bool feng_symbol_exception_read(const unsigned char *data, size_t size, uint32_t expected_count,
                                FengExceptionReadString string, void *user,
                                FengSymbolExceptionRoot **out_roots, size_t *out_count, const char *path,
                                FengSymbolError *error) {
    *out_roots = NULL;
    *out_count = 0U;
    ExceptionBytes bytes = {(unsigned char *)data, size, 0U, 0U, false};
    uint32_t rn = ei_get(&bytes), fn = ei_get(&bytes), nn = ei_get(&bytes), flags = ei_get(&bytes);
    FengExceptionGraph *graph = NULL;
    FengSymbolExceptionRoot *roots = NULL;
    uint32_t *nodes = NULL;
    unsigned *sorts = NULL;
    if (bytes.failed || flags != EI_STRUCTURED_ROOTS || rn != expected_count || nn < 2U ||
        (uint64_t)rn * 12U + (uint64_t)fn * 28U + (uint64_t)nn * 28U > size - bytes.cursor)
        goto malformed;
    roots = calloc(rn, sizeof(*roots));
    nodes = calloc(nn, sizeof(*nodes));
    sorts = calloc(nn, sizeof(*sorts));
    graph = feng_exception_graph_create();
    if ((rn != 0U && roots == NULL) || nodes == NULL || sorts == NULL || graph == NULL)
        goto malformed;
    for (uint32_t i = 0U; i < rn; ++i) {
        roots[i].symbol = ei_get(&bytes);
        roots[i].summary.function = ei_get(&bytes);
        roots[i].summary.effects = ei_get(&bytes);
        if (bytes.failed || roots[i].symbol == 0U || roots[i].summary.function >= fn ||
            roots[i].summary.effects >= nn)
            goto malformed;
        for (uint32_t j = 0U; j < i; ++j)
            if (roots[j].symbol == roots[i].symbol)
                goto malformed;
    }
    for (uint32_t i = 0U; i < fn; ++i) {
        const char *name = string(user, ei_get(&bytes));
        uint32_t tn = ei_get(&bytes), pn = ei_get(&bytes), cn = ei_get(&bytes);
        uint32_t effect = ei_get(&bytes), result = ei_get(&bytes), zero = ei_get(&bytes);
        if (bytes.failed || name == NULL || zero != 0U || tn > (bytes.size - bytes.cursor) / 4U ||
            effect >= nn || result >= nn || (uint64_t)pn + cn > UINT32_MAX)
            goto malformed;
        const char **names = tn != 0U ? calloc(tn, sizeof(*names)) : NULL;
        if (tn != 0U && names == NULL)
            goto malformed;
        bool valid = true;
        for (uint32_t j = 0U; j < tn; ++j) {
            names[j] = string(user, ei_get(&bytes));
            if (names[j] == NULL || names[j][0] == '\0')
                valid = false;
        }
        uint32_t id = valid ? feng_exception_function_add(graph, name, names, tn, pn, cn) : UINT32_MAX;
        free(names);
        if (id != i)
            goto malformed;
        graph->functions[i].effects = effect;
        graph->functions[i].result = result;
    }
    for (uint32_t i = 0U; i < nn; ++i) {
        uint32_t kind = ei_get(&bytes), x = ei_get(&bytes), y = ei_get(&bytes), z = ei_get(&bytes);
        uint32_t name_id = ei_get(&bytes), count = ei_get(&bytes), zero = ei_get(&bytes);
        const char *name = name_id != 0U ? string(user, name_id) : NULL;
        if (bytes.failed || zero != 0U || kind > FENG_EXCEPTION_GUARDED || (name_id != 0U && name == NULL) ||
            count > (bytes.size - bytes.cursor) / 4U)
            goto malformed;
        uint32_t *args = count != 0U ? malloc(count * sizeof(*args)) : NULL;
        if (count != 0U && args == NULL)
            goto malformed;
        bool valid = true;
        for (uint32_t j = 0U; j < count; ++j) {
            args[j] = ei_get(&bytes);
            if (args[j] >= i)
                valid = false;
        }
        unsigned sort = valid ? ei_node_sort(graph, kind, x, y, z, name, args, count, sorts, i) : 0U;
        if (sort == 0U) {
            free(args);
            goto malformed;
        }
        if (kind != FENG_EXCEPTION_TYPE_PARAMETER && kind != FENG_EXCEPTION_VALUE_PARAMETER) {
            if (kind != FENG_EXCEPTION_FUNCTION && kind != FENG_EXCEPTION_METHOD)
                x = nodes[x];
            y = nodes[y];
            z = nodes[z];
        }
        for (uint32_t j = 0U; j < count; ++j)
            args[j] = nodes[args[j]];
        nodes[i] = feng_exception_node(graph, (FengExceptionNodeKind)kind, x, y, z, name, args, count);
        sorts[i] = sort;
        free(args);
        if (graph->failed)
            goto malformed;
    }
    for (uint32_t i = 0U; i < fn; ++i) {
        FengExceptionFunction *f = &graph->functions[i];
        if ((sorts[f->effects] & EI_EFFECT) == 0U || (sorts[f->result] & EI_VALUE) == 0U)
            goto malformed;
        f->effects = nodes[f->effects];
        f->result = nodes[f->result];
    }
    if (bytes.failed || bytes.cursor != size)
        goto malformed;
    for (uint32_t i = 0U; i < rn; ++i) {
        if ((sorts[roots[i].summary.effects] & (EI_EFFECT | EI_SOLVED_EFFECT)) !=
            (EI_EFFECT | EI_SOLVED_EFFECT))
            goto malformed;
        roots[i].summary.effects = nodes[roots[i].summary.effects];
        roots[i].summary.graph = graph;
        feng_exception_graph_retain(graph);
    }
    feng_exception_graph_release(graph);
    free(nodes);
    free(sorts);
    *out_roots = roots;
    *out_count = rn;
    return true;
malformed:
    feng_exception_graph_release(graph);
    feng_symbol_exception_roots_free(roots, rn);
    free(nodes);
    free(sorts);
    return feng_symbol_internal_set_error(
        error, path, (FengToken){0},
        "malformed or incomplete exception summary metadata; rebuild the producing package");
}
