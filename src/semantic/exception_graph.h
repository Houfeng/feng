#ifndef FENG_SEMANTIC_EXCEPTION_GRAPH_H
#define FENG_SEMANTIC_EXCEPTION_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Provider-neutral compile-time terms. Zero is the empty effect/value set;
 * node one is Boolean true. References are graph-local, never runtime IDs. */
typedef enum FengExceptionNodeKind {
    FENG_EXCEPTION_EMPTY = 0,
    FENG_EXCEPTION_TRUE,
    FENG_EXCEPTION_UNKNOWN,
    FENG_EXCEPTION_UNION,
    FENG_EXCEPTION_NAMED_TYPE,
    FENG_EXCEPTION_TYPE_PARAMETER,
    FENG_EXCEPTION_VALUE_PARAMETER,
    FENG_EXCEPTION_VALUE_TYPE,
    FENG_EXCEPTION_TYPED_VALUE,
    FENG_EXCEPTION_FUNCTION,
    FENG_EXCEPTION_METHOD,
    FENG_EXCEPTION_CALL,
    FENG_EXCEPTION_CALL_RESULT,
    FENG_EXCEPTION_THROW,
    FENG_EXCEPTION_EXCLUDE,
    FENG_EXCEPTION_SELECT,
    FENG_EXCEPTION_WHEN,
    /* Normalized terms used by solved declaration and invocation results. */
    FENG_EXCEPTION_EQUAL_TYPE,
    FENG_EXCEPTION_DECISION,
    FENG_EXCEPTION_GUARDED
} FengExceptionNodeKind;

/* Immutable, hash-consed term. x/y/z are child IDs except PARAMETER (x is
 * callable ID, y is slot), FUNCTION/METHOD (x is callable ID), and DECISION
 * (x is the equality atom). args holds ordered children; UNION is a set. */
typedef struct FengExceptionNode {
    FengExceptionNodeKind kind;
    uint32_t x, y, z;
    char *name;
    uint32_t *args;
    size_t arg_count;
    uint64_t hash;
} FengExceptionNode;

/* Declaration template. Type slots are owner then callable parameters;
 * value slots are explicit parameters then closure captures and receiver.
 * Parameter names are display-only, never substitution keys. */
typedef struct FengExceptionFunction {
    char *name;
    char **type_names;
    size_t type_count;
    size_t parameter_count;
    size_t capture_count;
    uint32_t effects;
    uint32_t result;
} FengExceptionFunction;

/* Owned portable summary graph; Symbol retains it independently of the AST.
 * Once semantic analysis publishes a graph, readers only inspect it. */
typedef struct FengExceptionGraph {
    size_t references;
    FengExceptionNode *nodes;
    size_t node_count, node_capacity;
    uint32_t *node_slots;
    size_t node_slot_count;
    FengExceptionFunction *functions;
    size_t function_count, function_capacity;
    bool failed;
} FengExceptionGraph;

/* A callable's parameterized template and solved declaration result. Both
 * roots belong to graph; metadata contains no formatted or diagnostic text. */
typedef struct FengExceptionTemplate {
    FengExceptionGraph *graph;
    uint32_t function;
    uint32_t effects;
} FengExceptionTemplate;

/* Create/release a graph. Allocation failure is sticky in graph->failed. */
FengExceptionGraph *feng_exception_graph_create(void);
void feng_exception_graph_retain(FengExceptionGraph *graph);
void feng_exception_graph_release(FengExceptionGraph *graph);

/* Intern one term, copying names and arguments. Returned zero is also the
 * allocation-failure sentinel; callers must check graph->failed. */
uint32_t feng_exception_node(FengExceptionGraph *graph, FengExceptionNodeKind kind, uint32_t x, uint32_t y,
                             uint32_t z, const char *name, const uint32_t *args, size_t arg_count);
uint32_t feng_exception_union(FengExceptionGraph *graph, uint32_t a, uint32_t b);
uint32_t feng_exception_unknown(FengExceptionGraph *graph);

/* Add a declaration shell before connecting its body/dependencies, so
 * recursive and mutually recursive callables have stable identities. */
uint32_t feng_exception_function_add(FengExceptionGraph *graph, const char *name,
                                     const char *const *type_names, size_t type_count, size_t parameter_count,
                                     size_t capture_count);

/* Provider adapters clone a reachable graph by ID; function/term mappings
 * preserve shared and recursive dependencies and parameter ownership. */
bool feng_exception_graph_import(FengExceptionGraph *target, const FengExceptionGraph *source,
                                 uint32_t **out_functions, uint32_t **out_nodes);

/* Extract selected templates and solved results with their dependencies.
 * Other query caches and private bodies are not part of the resulting graph. */
FengExceptionGraph *feng_exception_graph_extract(const FengExceptionGraph *source, const uint32_t *roots,
                                                 const uint32_t *effects, size_t root_count,
                                                 uint32_t *out_roots, uint32_t *out_effects);

/* Symbol-owned handles retain their immutable graph. */
bool feng_exception_template_copy(FengExceptionTemplate *target, const FengExceptionTemplate *source);
void feng_exception_template_free(FengExceptionTemplate *summary);

/* Format a solved effect as a sorted, unique type list, including unknown.
 * Conditions are not types. Empty is "none". Caller owns text; consumers
 * format on demand without storing this text in compiler metadata. */
char *feng_exception_format(const FengExceptionGraph *graph, uint32_t effects);

#endif
