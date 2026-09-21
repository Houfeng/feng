#ifndef FENG_SEMANTIC_EXCEPTION_EFFECTS_H
#define FENG_SEMANTIC_EXCEPTION_EFFECTS_H

#include "semantic/semantic.h"
#include "semantic/exception_graph.h"

/* Immutable facts for an analyzed invocation. Graph IDs denote possible
 * exceptions before and after enclosing catches; zero denotes the empty set.
 * All pointers are borrowed until the semantic analysis is released. */
typedef struct FengExceptionCallFacts {
    const char *path;
    const FengExpr *expression;
    const FengExceptionGraph *graph;
    uint32_t effects;
    uint32_t escaping_effects;
} FengExceptionCallFacts;

/* Record a resolved non-local value origin while its lexical resolver is
 * available. Local values are tracked by declaration/scope in the post-pass. */
bool feng_semantic_exception_record_value_source(const FengSemanticAnalysis *analysis, const FengExpr *site,
                                                 const void *source_node);

/* Preserve a validated generic type-level target, distinct from a value. */
bool feng_semantic_exception_record_type_parameter(const FengSemanticAnalysis *analysis, const FengExpr *site,
                                                   const FengTypeParam *parameter);

/* Build and solve neutral metadata after ordinary type/call resolution. */
bool feng_semantic_collect_exception_effects(FengSemanticAnalysis *analysis);

/* Consume completed metadata at ABI boundaries; report through the ordinary
 * diagnostic API without writing diagnostic text into exception metadata. */
bool feng_semantic_validate_abi_exception_effects(const FengSemanticAnalysis *analysis,
                                                 FengSemanticError **errors, size_t *error_count,
                                                 size_t *error_capacity);

/* Query declaration/instance information without performing analysis. */
const FengExceptionTemplate *feng_semantic_exception_template(const FengSemanticAnalysis *analysis,
                                                              const void *source_node);
bool feng_semantic_exception_call_facts(const FengSemanticAnalysis *analysis, const FengExpr *call,
                                       FengExceptionCallFacts *out_facts);

/* Enumerate solved call facts without reanalysis, validation or diagnostics. */
size_t feng_semantic_exception_call_count(const FengSemanticAnalysis *analysis);
bool feng_semantic_exception_call_at(const FengSemanticAnalysis *analysis, size_t index,
                                     FengExceptionCallFacts *out_facts);

/* Release all sidecar-owned graphs, origins and structured results. */
void feng_semantic_exception_analysis_free(struct FengExceptionAnalysis *effects);

#endif
