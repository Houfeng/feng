#ifndef FENG_SEMANTIC_EXCEPTION_SOLVER_H
#define FENG_SEMANTIC_EXCEPTION_SOLVER_H

#include "semantic/exception_graph.h"

/* An implementation resolver receives an evaluated subject value and the
 * portable spec requirement. It returns a bound FUNCTION term or zero when
 * the subject/implementation remains dynamic. No runtime query is involved. */
typedef uint32_t (*FengExceptionMethodResolver)(void *user, uint32_t requirement, uint32_t subject,
                                                const uint32_t *type_args, size_t type_arg_count);

typedef struct FengExceptionSolver FengExceptionSolver;

/* One solver owns the instance cache and dependency work queue for an
 * analysis; the graph remains owned by the caller. */
FengExceptionSolver *feng_exception_solver_create(FengExceptionGraph *graph,
                                                  FengExceptionMethodResolver resolver, void *user);
void feng_exception_solver_free(FengExceptionSolver *solver);

/* Register an immutable instantiation. Empty arguments use declaration-local
 * symbolic slots, not unknown/false. IDs remain stable until solver_free. */
uint32_t feng_exception_solver_declaration(FengExceptionSolver *solver, uint32_t function);
bool feng_exception_solver_run(FengExceptionSolver *solver);
uint32_t feng_exception_solver_effects(const FengExceptionSolver *solver, uint32_t instance);
uint32_t feng_exception_solver_result(const FengExceptionSolver *solver, uint32_t instance);

/* Evaluate a declaration-local term under the selected root bindings. This
 * can discover additional instances; run the queue before publishing it. */
uint32_t feng_exception_solver_term(FengExceptionSolver *solver, uint32_t instance, uint32_t term);

#endif
