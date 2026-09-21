#include "semantic/exception_solver.h"

#include <stdlib.h>
#include <string.h>

/* A canonical callable instantiation and its reverse dependency edges. */
typedef struct ExceptionInstance {
    uint32_t function;
    uint32_t parent;
    uint32_t *types, *values;
    size_t type_count, value_count;
    uint32_t effects, result;
    uint32_t *users;
    size_t user_count, user_capacity;
    bool queued;
    uint64_t hash;
} ExceptionInstance;

/* Solver state belongs to a single analysis, so published summary queries
 * never mutate this graph or trigger another whole-program analysis. */
struct FengExceptionSolver {
    FengExceptionGraph *graph;
    ExceptionInstance **instances;
    size_t instance_count, instance_capacity;
    uint32_t *slots;
    size_t slot_count;
    uint32_t *queue;
    size_t queue_count, queue_capacity, queue_head;
    FengExceptionMethodResolver resolver;
    void *user;
};

/* Per-evaluation memoization avoids repeated traversal of a shared term DAG. */
typedef struct ExceptionEvaluation {
    FengExceptionSolver *solver;
    uint32_t instance;
    uint32_t *memo;
    unsigned char *seen;
    size_t count;
} ExceptionEvaluation;

/* Checked growing vectors; failures poison the analysis graph. */
static bool es_grow(FengExceptionGraph *g, void **data, size_t *cap, size_t count, size_t width) {
    if (count <= *cap)
        return true;
    size_t next = *cap != 0U ? *cap : 16U;
    while (next < count && next <= SIZE_MAX / 2U)
        next *= 2U;
    if (next < count || next > SIZE_MAX / width) {
        g->failed = true;
        return false;
    }
    void *result = realloc(*data, next * width);
    if (result == NULL) {
        g->failed = true;
        return false;
    }
    *data = result;
    *cap = next;
    return true;
}

/* Build a reduced ordered Boolean decision; equal branches erase the test. */
static uint32_t es_decision(FengExceptionGraph *g, uint32_t atom, uint32_t low, uint32_t high) {
    return low == high ? low
                       : feng_exception_node(g, FENG_EXCEPTION_DECISION, atom, low, high, NULL, NULL, 0U);
}

/* Boolean conjunction/disjunction over a canonical type-equality BDD. */
static uint32_t es_boolean(FengExceptionGraph *g, uint32_t a, uint32_t b, bool either) {
    if (a == b)
        return a;
    if (either) {
        if (a == 1U || b == 1U)
            return 1U;
        if (a == 0U)
            return b;
        if (b == 0U)
            return a;
    } else {
        if (a == 0U || b == 0U)
            return 0U;
        if (a == 1U)
            return b;
        if (b == 1U)
            return a;
    }
    FengExceptionNode an = g->nodes[a], bn = g->nodes[b];
    uint32_t atom = an.x < bn.x ? an.x : bn.x;
    uint32_t low = es_boolean(g, an.x == atom ? an.y : a, bn.x == atom ? bn.y : b, either);
    uint32_t high = es_boolean(g, an.x == atom ? an.z : a, bn.x == atom ? bn.z : b, either);
    return es_decision(g, atom, low, high);
}

/* Complement a canonical Boolean guard. */
static uint32_t es_not(FengExceptionGraph *g, uint32_t guard) {
    if (guard <= 1U)
        return 1U - guard;
    FengExceptionNode n = g->nodes[guard];
    uint32_t low = es_not(g, n.y), high = es_not(g, n.z);
    return es_decision(g, n.x, low, high);
}

/* Type equality is exact for closed types and symbolic for open slots. */
static uint32_t es_equal_type(FengExceptionGraph *g, uint32_t a, uint32_t b) {
    if (a == b)
        return 1U;
    FengExceptionNode an = g->nodes[a], bn = g->nodes[b];
    if (an.kind == FENG_EXCEPTION_TYPE_PARAMETER || bn.kind == FENG_EXCEPTION_TYPE_PARAMETER) {
        uint32_t atom =
            feng_exception_node(g, FENG_EXCEPTION_EQUAL_TYPE, a < b ? a : b, a < b ? b : a, 0, NULL, NULL, 0);
        return es_decision(g, atom, 0U, 1U);
    }
    if (an.kind != FENG_EXCEPTION_NAMED_TYPE || bn.kind != FENG_EXCEPTION_NAMED_TYPE ||
        an.arg_count != bn.arg_count || strcmp(an.name, bn.name) != 0)
        return 0U;
    uint32_t equal = 1U;
    for (size_t i = 0U; i < an.arg_count && equal != 0U; ++i)
        equal = es_boolean(g, equal, es_equal_type(g, an.args[i], bn.args[i]), false);
    return equal;
}

/* Apply one Boolean condition to every exception alternative. */
static uint32_t es_guard(FengExceptionGraph *g, uint32_t guard, uint32_t effects) {
    if (guard == 0U || effects == 0U)
        return 0U;
    if (guard == 1U)
        return effects;
    FengExceptionNode n = g->nodes[effects];
    if (n.kind == FENG_EXCEPTION_UNION) {
        uint32_t result = 0U;
        for (size_t i = 0U; i < n.arg_count; ++i)
            result = feng_exception_union(g, result, es_guard(g, guard, n.args[i]));
        return result;
    }
    if (n.kind == FENG_EXCEPTION_GUARDED)
        return es_guard(g, es_boolean(g, guard, n.x, false), n.y);
    return feng_exception_node(g, FENG_EXCEPTION_GUARDED, guard, effects, 0, NULL, NULL, 0);
}

/* Merge effects by payload identity and excluded-type set, joining guards. */
static uint32_t es_union(FengExceptionGraph *g, uint32_t a, uint32_t b) {
    if (a == 0U || a == b)
        return b;
    if (b == 0U)
        return a;
    FengExceptionNode bn = g->nodes[b];
    if (bn.kind == FENG_EXCEPTION_UNION) {
        for (size_t i = 0U; i < bn.arg_count; ++i)
            a = es_union(g, a, bn.args[i]);
        return a;
    }
    uint32_t bg = bn.kind == FENG_EXCEPTION_GUARDED ? bn.x : 1U;
    uint32_t bv = bn.kind == FENG_EXCEPTION_GUARDED ? bn.y : b;
    FengExceptionNode an = g->nodes[a];
    const uint32_t *items = an.kind == FENG_EXCEPTION_UNION ? an.args : &a;
    size_t count = an.kind == FENG_EXCEPTION_UNION ? an.arg_count : 1U;
    uint32_t result = 0U;
    bool merged = false;
    for (size_t i = 0U; i < count; ++i) {
        FengExceptionNode item = g->nodes[items[i]];
        uint32_t ag = item.kind == FENG_EXCEPTION_GUARDED ? item.x : 1U;
        uint32_t av = item.kind == FENG_EXCEPTION_GUARDED ? item.y : items[i];
        uint32_t next = items[i];
        if (av == bv) {
            next = es_guard(g, es_boolean(g, ag, bg, true), av);
            merged = true;
        }
        result = feng_exception_union(g, result, next);
    }
    return merged ? result : feng_exception_union(g, result, b);
}

/* The guard under which an effect set can contain at least one exception. */
static uint32_t es_nonempty(FengExceptionGraph *g, uint32_t effects) {
    if (effects == 0U)
        return 0U;
    FengExceptionNode n = g->nodes[effects];
    if (n.kind == FENG_EXCEPTION_GUARDED)
        return n.x;
    if (n.kind != FENG_EXCEPTION_UNION)
        return 1U;
    uint32_t result = 0U;
    for (size_t i = 0U; i < n.arg_count; ++i)
        result = es_boolean(g, result, es_nonempty(g, n.args[i]), true);
    return result;
}

/* Node IDs give a deterministic set order for unknown-type exclusions. */
static int es_compare_id(const void *a, const void *b) {
    uint32_t av = *(const uint32_t *)a, bv = *(const uint32_t *)b;
    return (av > bv) - (av < bv);
}

/* Filter only the protected expression. Unknown sources retain exclusions,
 * allowing an outer typed catch to see what an inner one already removed. */
static uint32_t es_filter(FengExceptionGraph *g, uint32_t effects, uint32_t type, bool select) {
    if (effects == 0U)
        return 0U;
    FengExceptionNode n = g->nodes[effects];
    if (n.kind == FENG_EXCEPTION_UNION) {
        uint32_t result = 0U;
        for (size_t i = 0U; i < n.arg_count; ++i)
            result = es_union(g, result, es_filter(g, n.args[i], type, select));
        return result;
    }
    if (n.kind == FENG_EXCEPTION_GUARDED)
        return es_guard(g, n.x, es_filter(g, n.y, type, select));
    if (n.kind == FENG_EXCEPTION_THROW) {
        uint32_t match = es_equal_type(g, n.x, type);
        uint32_t thrown =
            select ? feng_exception_node(g, FENG_EXCEPTION_THROW, type, 0, 0, NULL, NULL, 0) : effects;
        return es_guard(g, select ? match : es_not(g, match), thrown);
    }
    if (n.kind == FENG_EXCEPTION_UNKNOWN) {
        if (select) {
            uint32_t guard = 1U;
            for (size_t i = 0U; i < n.arg_count; ++i)
                guard = es_boolean(g, guard, es_not(g, es_equal_type(g, type, n.args[i])), false);
            return es_guard(g, guard,
                            feng_exception_node(g, FENG_EXCEPTION_THROW, type, 0, 0, NULL, NULL, 0));
        }
        for (size_t i = 0U; i < n.arg_count; ++i)
            if (n.args[i] == type)
                return effects;
        uint32_t *excluded = malloc((n.arg_count + 1U) * sizeof(*excluded));
        if (excluded == NULL) {
            g->failed = true;
            return 0U;
        }
        if (n.arg_count != 0U)
            memcpy(excluded, n.args, n.arg_count * sizeof(*excluded));
        excluded[n.arg_count] = type;
        qsort(excluded, n.arg_count + 1U, sizeof(*excluded), es_compare_id);
        uint32_t result =
            feng_exception_node(g, FENG_EXCEPTION_UNKNOWN, 0, 0, 0, n.name, excluded, n.arg_count + 1U);
        free(excluded);
        return result;
    }
    g->failed = true;
    return 0U;
}

/* Queue a changed dependency only once, preserving stable instance IDs. */
static void es_enqueue(FengExceptionSolver *s, uint32_t id) {
    ExceptionInstance *instance = s->instances[id];
    if (instance->queued || s->graph->failed)
        return;
    if (!es_grow(s->graph, (void **)&s->queue, &s->queue_capacity, s->queue_count + 1U, sizeof(*s->queue)))
        return;
    instance->queued = true;
    s->queue[s->queue_count++] = id;
}

/* Hash a binding tuple; IDs already encode exact structural type identity. */
static uint64_t es_hash(uint32_t function, const uint32_t *types, size_t tn, const uint32_t *values,
                        size_t vn) {
    uint64_t hash = UINT64_C(14695981039346656037) ^ function;
    for (size_t i = 0U; i < tn; ++i)
        hash = (hash ^ types[i]) * UINT64_C(1099511628211);
    hash = (hash ^ tn) * UINT64_C(1099511628211);
    for (size_t i = 0U; i < vn; ++i)
        hash = (hash ^ values[i]) * UINT64_C(1099511628211);
    return (hash ^ vn) * UINT64_C(1099511628211);
}

/* Grow the instance identity table without invalidating instance storage. */
static bool es_rehash(FengExceptionSolver *s, size_t count) {
    uint32_t *slots = calloc(count, sizeof(*slots));
    if (slots == NULL) {
        s->graph->failed = true;
        return false;
    }
    for (size_t i = 0U; i < s->instance_count; ++i) {
        size_t at = (size_t)s->instances[i]->hash & (count - 1U);
        while (slots[at] != 0U)
            at = (at + 1U) & (count - 1U);
        slots[at] = (uint32_t)i + 1U;
    }
    free(s->slots);
    s->slots = slots;
    s->slot_count = count;
    return true;
}

/* Intern one binding domain; a new instance starts at the least fixed point. */
static uint32_t es_instance(FengExceptionSolver *s, uint32_t function, const uint32_t *types, size_t tn,
                            const uint32_t *values, size_t vn, uint32_t parent) {
    if (s->graph->failed)
        return UINT32_MAX;
    if (s->slot_count == 0U || s->instance_count + 1U > s->slot_count / 2U)
        if (!es_rehash(s, s->slot_count != 0U ? s->slot_count * 2U : 64U))
            return UINT32_MAX;
    uint64_t hash = es_hash(function, types, tn, values, vn);
    size_t at = (size_t)hash & (s->slot_count - 1U);
    while (s->slots[at] != 0U) {
        uint32_t id = s->slots[at] - 1U;
        ExceptionInstance *it = s->instances[id];
        if (it->hash == hash && it->function == function && it->type_count == tn && it->value_count == vn &&
            (tn == 0U || memcmp(types, it->types, tn * sizeof(*types)) == 0) &&
            (vn == 0U || memcmp(values, it->values, vn * sizeof(*values)) == 0))
            return id;
        at = (at + 1U) & (s->slot_count - 1U);
    }
    ExceptionInstance *it = calloc(1U, sizeof(*it));
    if (it == NULL) {
        s->graph->failed = true;
        return UINT32_MAX;
    }
    it->types = tn != 0U ? malloc(tn * sizeof(*types)) : NULL;
    it->values = vn != 0U ? malloc(vn * sizeof(*values)) : NULL;
    if ((tn != 0U && it->types == NULL) || (vn != 0U && it->values == NULL) ||
        !es_grow(s->graph, (void **)&s->instances, &s->instance_capacity, s->instance_count + 1U,
                 sizeof(*s->instances))) {
        free(it->types);
        free(it->values);
        free(it);
        s->graph->failed = true;
        return UINT32_MAX;
    }
    if (tn != 0U)
        memcpy(it->types, types, tn * sizeof(*types));
    if (vn != 0U)
        memcpy(it->values, values, vn * sizeof(*values));
    it->type_count = tn;
    it->value_count = vn;
    it->function = function;
    it->hash = hash;
    it->parent = parent;
    uint32_t id = (uint32_t)s->instance_count++;
    s->instances[id] = it;
    s->slots[at] = id + 1U;
    es_enqueue(s, id);
    return id;
}

/* Record the reverse edge used by the dependency work queue. */
static void es_dependency(FengExceptionSolver *s, uint32_t callee, uint32_t caller) {
    ExceptionInstance *it = s->instances[callee];
    for (size_t i = 0U; i < it->user_count; ++i)
        if (it->users[i] == caller)
            return;
    if (es_grow(s->graph, (void **)&it->users, &it->user_capacity, it->user_count + 1U, sizeof(*it->users)))
        it->users[it->user_count++] = caller;
}

/* Evaluate a raw summary term in one invocation's binding domain. */
static uint32_t es_eval(ExceptionEvaluation *evaluation, uint32_t id);

/* Detect structural growth, including flattened set growth. Parameter terms
 * cannot project/destructure their arguments, so recursive construction is
 * the only source of an unbounded binding domain in this summary language. */
static bool es_contains(FengExceptionGraph *g, uint32_t tree, uint32_t part) {
    if (tree == part)
        return true;
    if (tree <= 1U || part == 0U)
        return false;
    FengExceptionNode n = g->nodes[tree], p = g->nodes[part];
    if (n.kind == FENG_EXCEPTION_UNION && p.kind == FENG_EXCEPTION_UNION) {
        bool subset = true;
        for (size_t i = 0U; i < p.arg_count && subset; ++i) {
            bool found = false;
            for (size_t j = 0U; j < n.arg_count && !found; ++j)
                found = es_contains(g, n.args[j], p.args[i]);
            subset = found;
        }
        if (subset)
            return true;
    }
    for (size_t i = 0U; i < n.arg_count; ++i)
        if (es_contains(g, n.args[i], part))
            return true;
    if (n.kind == FENG_EXCEPTION_VALUE_TYPE)
        return es_contains(g, n.x, part);
    if (n.kind == FENG_EXCEPTION_GUARDED)
        return es_contains(g, n.y, part);
    return false;
}

/* Widen only structurally growing recursive slots, retaining a diagnostic
 * reason. Ordinary generic calls keep their exact, isolated binding tuple. */
static void es_widen(FengExceptionSolver *s, uint32_t caller, uint32_t function, uint32_t *types, size_t tn,
                     uint32_t *values, size_t vn) {
    for (uint32_t parent = caller; parent != UINT32_MAX; parent = s->instances[parent]->parent) {
        const ExceptionInstance *prior = s->instances[parent];
        if (prior->function != function)
            continue;
        for (size_t i = 0U; i < tn; ++i)
            if (types[i] != prior->types[i] && es_contains(s->graph, types[i], prior->types[i]))
                types[i] = feng_exception_unknown(s->graph);
        for (size_t i = 0U; i < vn; ++i)
            if (values[i] != prior->values[i] && es_contains(s->graph, values[i], prior->values[i]))
                values[i] = feng_exception_unknown(s->graph);
    }
}

/* A recursive result must embed an old value inside a new environment;
 * adding an ordinary branch alternative is finite set growth, not nesting. */
static bool es_result_grows(FengExceptionGraph *g, uint32_t value, uint32_t prior) {
    if (value == 0U || prior == 0U || value == prior)
        return false;
    FengExceptionNode n = g->nodes[value], p = g->nodes[prior];
    if (p.kind == FENG_EXCEPTION_UNION) {
        for (size_t i = 0U; i < p.arg_count; ++i)
            if (es_result_grows(g, value, p.args[i]))
                return true;
    } else if (n.kind == FENG_EXCEPTION_UNION) {
        for (size_t i = 0U; i < n.arg_count; ++i)
            if (es_result_grows(g, n.args[i], prior))
                return true;
    } else if (n.kind == FENG_EXCEPTION_FUNCTION) {
        size_t first = g->functions[n.x].type_count;
        for (size_t i = first; i < n.arg_count; ++i)
            if (es_contains(g, n.args[i], prior))
                return true;
    } else if (n.kind == FENG_EXCEPTION_GUARDED)
        return es_result_grows(g, n.y, prior);
    return false;
}

/* Prove a dependency cycle before widening a structurally growing result. */
static bool es_on_cycle(FengExceptionSolver *s, uint32_t id) {
    size_t count = s->instance_count;
    bool *seen = calloc(count, sizeof(*seen));
    uint32_t *queue = malloc(count * sizeof(*queue));
    if (seen == NULL || queue == NULL) {
        free(seen);
        free(queue);
        s->graph->failed = true;
        return false;
    }
    size_t head = 0U, end = 0U;
    queue[end++] = id;
    seen[id] = true;
    bool found = false;
    while (head < end && !found) {
        const ExceptionInstance *it = s->instances[queue[head++]];
        for (size_t i = 0U; i < it->user_count; ++i) {
            uint32_t next = it->users[i];
            if (next == id) {
                found = true;
                break;
            }
            if (!seen[next]) {
                seen[next] = true;
                queue[end++] = next;
            }
        }
    }
    free(seen);
    free(queue);
    return found;
}

/* Invoke every possible callable value, without executing closure creation. */
static uint32_t es_invoke(ExceptionEvaluation *evaluation, uint32_t value, const uint32_t *args, size_t count,
                          bool result_value) {
    FengExceptionSolver *s = evaluation->solver;
    FengExceptionGraph *g = s->graph;
    if (value == 0U)
        return 0U; /* pending result: revisited through dependencies */
    FengExceptionNode n = g->nodes[value];
    if (n.kind == FENG_EXCEPTION_GUARDED)
        return es_guard(g, n.x, es_invoke(evaluation, n.y, args, count, result_value));
    if (n.kind == FENG_EXCEPTION_UNION) {
        uint32_t result = 0U;
        for (size_t i = 0U; i < n.arg_count; ++i) {
            uint32_t item = es_invoke(evaluation, n.args[i], args, count, result_value);
            result = result_value ? feng_exception_union(g, result, item) : es_union(g, result, item);
        }
        return result;
    }
    if (n.kind == FENG_EXCEPTION_UNKNOWN)
        return value;
    if (n.kind != FENG_EXCEPTION_FUNCTION)
        return feng_exception_unknown(g);
    FengExceptionFunction fn = g->functions[n.x];
    if (n.arg_count != fn.type_count + fn.capture_count) {
        g->failed = true;
        return 0U;
    }
    size_t vn = fn.parameter_count + fn.capture_count;
    uint32_t *types = fn.type_count != 0U ? malloc(fn.type_count * sizeof(*types)) : NULL;
    uint32_t *values = vn != 0U ? calloc(vn, sizeof(*values)) : NULL;
    if ((vn != 0U && values == NULL) || (fn.type_count != 0U && types == NULL)) {
        free(types);
        free(values);
        g->failed = true;
        return 0U;
    }
    if (fn.type_count != 0U)
        memcpy(types, n.args, fn.type_count * sizeof(*types));
    for (size_t i = 0U; i < fn.parameter_count; ++i)
        values[i] = i < count ? args[i] : feng_exception_unknown(g);
    for (size_t i = 0U; i < fn.capture_count; ++i)
        values[fn.parameter_count + i] = n.args[fn.type_count + i];
    es_widen(s, evaluation->instance, n.x, types, fn.type_count, values, vn);
    uint32_t callee = es_instance(s, n.x, types, fn.type_count, values, vn, evaluation->instance);
    free(types);
    free(values);
    if (callee == UINT32_MAX)
        return 0U;
    es_dependency(s, callee, evaluation->instance);
    return result_value ? s->instances[callee]->result : s->instances[callee]->effects;
}

static uint32_t es_eval(ExceptionEvaluation *evaluation, uint32_t id) {
    FengExceptionSolver *s = evaluation->solver;
    FengExceptionGraph *g = s->graph;
    if (g->failed || id == 0U)
        return 0U;
    if (id < evaluation->count && evaluation->seen[id])
        return evaluation->memo[id];
    FengExceptionNode n = g->nodes[id];
    ExceptionInstance *instance = s->instances[evaluation->instance];
    uint32_t result = id;
    if (n.kind == FENG_EXCEPTION_TYPE_PARAMETER || n.kind == FENG_EXCEPTION_VALUE_PARAMETER) {
        if (n.x == instance->function) {
            const uint32_t *slots =
                n.kind == FENG_EXCEPTION_TYPE_PARAMETER ? instance->types : instance->values;
            size_t count =
                n.kind == FENG_EXCEPTION_TYPE_PARAMETER ? instance->type_count : instance->value_count;
            if (n.y >= count) {
                g->failed = true;
                return 0U;
            }
            result = slots[n.y];
        }
    } else if (n.kind == FENG_EXCEPTION_UNION) {
        result = 0U;
        for (size_t i = 0U; i < n.arg_count; ++i) {
            uint32_t item = es_eval(evaluation, n.args[i]);
            /* Generic unions also carry callable values; guarded effects
             * are merged when the owning function/query is finalized. */
            result = es_union(g, result, item);
        }
    } else if (n.kind == FENG_EXCEPTION_EXCLUDE || n.kind == FENG_EXCEPTION_SELECT) {
        uint32_t effects = es_eval(evaluation, n.x), type = es_eval(evaluation, n.y);
        result = es_filter(g, effects, type, n.kind == FENG_EXCEPTION_SELECT);
    } else if (n.kind == FENG_EXCEPTION_WHEN) {
        uint32_t condition = es_eval(evaluation, n.x), body = es_eval(evaluation, n.y);
        result = es_guard(g, es_nonempty(g, condition), body);
    } else if (n.kind == FENG_EXCEPTION_CALL || n.kind == FENG_EXCEPTION_CALL_RESULT) {
        uint32_t callee = es_eval(evaluation, n.x);
        uint32_t *args = n.arg_count != 0U ? malloc(n.arg_count * sizeof(*args)) : NULL;
        if (n.arg_count != 0U && args == NULL) {
            g->failed = true;
            return 0U;
        }
        for (size_t i = 0U; i < n.arg_count; ++i)
            args[i] = es_eval(evaluation, n.args[i]);
        result = es_invoke(evaluation, callee, args, n.arg_count, n.kind == FENG_EXCEPTION_CALL_RESULT);
        free(args);
    } else if (n.kind == FENG_EXCEPTION_METHOD) {
        uint32_t subject = es_eval(evaluation, n.y);
        if (subject == 0U)
            return 0U;
        uint32_t *args = n.arg_count != 0U ? malloc(n.arg_count * sizeof(*args)) : NULL;
        if (n.arg_count != 0U && args == NULL) {
            g->failed = true;
            return 0U;
        }
        for (size_t i = 0U; i < n.arg_count; ++i)
            args[i] = es_eval(evaluation, n.args[i]);
        result = s->resolver != NULL ? s->resolver(s->user, n.x, subject, args, n.arg_count) : 0U;
        if (result == 0U)
            result = feng_exception_unknown(g);
        free(args);
    } else if (n.kind == FENG_EXCEPTION_TYPED_VALUE) {
        result = es_eval(evaluation, n.x);
        if (result != 0U) {
            FengExceptionNode value = g->nodes[result];
            if (value.kind == FENG_EXCEPTION_VALUE_PARAMETER || value.kind == FENG_EXCEPTION_UNKNOWN)
                result = feng_exception_node(g, FENG_EXCEPTION_VALUE_TYPE, es_eval(evaluation, n.y), 0, 0,
                                             NULL, NULL, 0);
        }
    } else if (n.kind == FENG_EXCEPTION_NAMED_TYPE || n.kind == FENG_EXCEPTION_FUNCTION ||
               n.kind == FENG_EXCEPTION_UNKNOWN) {
        uint32_t *args = n.arg_count != 0U ? malloc(n.arg_count * sizeof(*args)) : NULL;
        if (n.arg_count != 0U && args == NULL) {
            g->failed = true;
            return 0U;
        }
        for (size_t i = 0U; i < n.arg_count; ++i)
            args[i] = es_eval(evaluation, n.args[i]);
        result = feng_exception_node(g, n.kind, n.x, n.y, n.z, n.name, args, n.arg_count);
        if (n.kind == FENG_EXCEPTION_NAMED_TYPE)
            for (size_t i = 0U; i < n.arg_count; ++i)
                if (g->nodes[args[i]].kind == FENG_EXCEPTION_UNKNOWN) {
                    result = args[i];
                    break;
                }
        free(args);
    } else if (n.kind == FENG_EXCEPTION_THROW || n.kind == FENG_EXCEPTION_VALUE_TYPE) {
        uint32_t type = es_eval(evaluation, n.x);
        result = feng_exception_node(g, n.kind, type, 0, 0, NULL, NULL, 0);
        if (n.kind == FENG_EXCEPTION_THROW && g->nodes[type].kind == FENG_EXCEPTION_UNKNOWN)
            result = type;
    }
    if (id < evaluation->count) {
        evaluation->memo[id] = result;
        evaluation->seen[id] = 1U;
    }
    return result;
}

FengExceptionSolver *feng_exception_solver_create(FengExceptionGraph *graph,
                                                  FengExceptionMethodResolver resolver, void *user) {
    FengExceptionSolver *s = calloc(1U, sizeof(*s));
    if (s != NULL) {
        s->graph = graph;
        s->resolver = resolver;
        s->user = user;
    }
    return s;
}

void feng_exception_solver_free(FengExceptionSolver *s) {
    if (s == NULL)
        return;
    for (size_t i = 0U; i < s->instance_count; ++i) {
        ExceptionInstance *it = s->instances[i];
        free(it->types);
        free(it->values);
        free(it->users);
        free(it);
    }
    free(s->instances);
    free(s->slots);
    free(s->queue);
    free(s);
}

uint32_t feng_exception_solver_declaration(FengExceptionSolver *s, uint32_t function) {
    FengExceptionGraph *g = s->graph;
    FengExceptionFunction fn = g->functions[function];
    size_t vn = fn.parameter_count + fn.capture_count;
    uint32_t *types = fn.type_count != 0U ? malloc(fn.type_count * sizeof(*types)) : NULL;
    uint32_t *values = vn != 0U ? malloc(vn * sizeof(*values)) : NULL;
    if ((fn.type_count != 0U && types == NULL) || (vn != 0U && values == NULL)) {
        free(types);
        free(values);
        g->failed = true;
        return UINT32_MAX;
    }
    for (size_t i = 0U; i < fn.type_count; ++i)
        types[i] =
            feng_exception_node(g, FENG_EXCEPTION_TYPE_PARAMETER, function, (uint32_t)i, 0, NULL, NULL, 0);
    for (size_t i = 0U; i < vn; ++i)
        values[i] =
            feng_exception_node(g, FENG_EXCEPTION_VALUE_PARAMETER, function, (uint32_t)i, 0, NULL, NULL, 0);
    uint32_t result = es_instance(s, function, types, fn.type_count, values, vn, UINT32_MAX);
    free(types);
    free(values);
    return result;
}

uint32_t feng_exception_solver_term(FengExceptionSolver *s, uint32_t instance, uint32_t term) {
    ExceptionEvaluation e = {0};
    e.solver = s;
    e.instance = instance;
    e.count = s->graph->node_count;
    e.memo = calloc(e.count, sizeof(*e.memo));
    e.seen = calloc(e.count, sizeof(*e.seen));
    if (e.memo == NULL || e.seen == NULL)
        s->graph->failed = true;
    uint32_t result = es_eval(&e, term);
    free(e.memo);
    free(e.seen);
    return result;
}

bool feng_exception_solver_run(FengExceptionSolver *s) {
    while (s->queue_head < s->queue_count && !s->graph->failed) {
        uint32_t id = s->queue[s->queue_head++];
        ExceptionInstance *it = s->instances[id];
        it->queued = false;
        FengExceptionFunction fn = s->graph->functions[it->function];
        uint32_t effects = feng_exception_solver_term(s, id, fn.effects);
        uint32_t value = feng_exception_solver_term(s, id, fn.result);
        effects = es_union(s->graph, it->effects, effects);
        if (s->graph->nodes[it->result].kind == FENG_EXCEPTION_UNKNOWN)
            value = it->result;
        else if (es_result_grows(s->graph, value, it->result) && es_on_cycle(s, id))
            value = feng_exception_unknown(s->graph);
        else if (s->graph->nodes[value].kind != FENG_EXCEPTION_UNKNOWN)
            value = feng_exception_union(s->graph, it->result, value);
        if (effects != it->effects || value != it->result) {
            it->effects = effects;
            it->result = value;
            for (size_t i = 0U; i < it->user_count; ++i)
                es_enqueue(s, it->users[i]);
        }
    }
    s->queue_head = 0U;
    s->queue_count = 0U;
    return !s->graph->failed;
}

uint32_t feng_exception_solver_effects(const FengExceptionSolver *s, uint32_t instance) {
    return s->instances[instance]->effects;
}

uint32_t feng_exception_solver_result(const FengExceptionSolver *s, uint32_t instance) {
    return s->instances[instance]->result;
}
