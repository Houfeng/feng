/* Phase S4 — SpecEquality sidecar.
 *
 * Stores one entry per binary `==` / `!=` AST expression whose operands the
 * resolver determined to have a spec static type. The semantic conclusion
 * is "reference-identity comparison" per dev/feng-spec-semantic-draft.md
 * §6.6 / §9.6 — codegen reads this sidecar to keep spec equality decoupled
 * from the value-equality path used by `string` / `array`.
 *
 * The table is populated by the binary-expression resolver in analyzer.c
 * after validate_binary_expr succeeds, when at least one operand resolves
 * to a FENG_DECL_SPEC. validate_binary_expr's same-type guard ensures the
 * other operand has the same spec, so a single `spec_decl` field suffices.
 *
 * Lookups are linear; equality sites against spec-typed operands are
 * sparse and the O(N) scan is acceptable at this stage. The
 * cast-away-const idiom matches the other sidecars (spec_coercion_sites.c,
 * spec_default_bindings.c, spec_member_accesses.c, spec_witnesses.c).
 */

#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static FengSpecEquality *find_entry_mut(FengSemanticAnalysis *analysis,
                                        const FengExpr *expr) {
    if (analysis == NULL || expr == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_equality_count; ++i) {
        if (analysis->spec_equalities[i].expr == expr) {
            return &analysis->spec_equalities[i];
        }
    }
    return NULL;
}

static FengSpecEquality *reserve_entry_slot(FengSemanticAnalysis *analysis,
                                            const FengExpr *expr) {
    FengSpecEquality *existing = find_entry_mut(analysis, expr);
    if (existing != NULL) {
        return existing;
    }
    if (analysis->spec_equality_count == analysis->spec_equality_capacity) {
        size_t new_cap = analysis->spec_equality_capacity == 0U
                             ? 16U
                             : analysis->spec_equality_capacity * 2U;
        FengSpecEquality *grown = realloc(analysis->spec_equalities,
                                          new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        analysis->spec_equalities = grown;
        analysis->spec_equality_capacity = new_cap;
    }
    FengSpecEquality *slot = &analysis->spec_equalities[analysis->spec_equality_count++];
    memset(slot, 0, sizeof(*slot));
    slot->expr = expr;
    return slot;
}

bool feng_semantic_record_spec_equality(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *spec_decl,
        FengSpecEqualityOp op) {
    if (analysis_const == NULL || expr == NULL || spec_decl == NULL) {
        return false;
    }
    if (spec_decl->kind != FENG_DECL_SPEC) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengSpecEquality *slot = reserve_entry_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    slot->expr = expr;
    slot->spec_decl = spec_decl;
    slot->op = op;
    return true;
}

const FengSpecEquality *feng_semantic_lookup_spec_equality(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr) {
    if (analysis_const == NULL) {
        return NULL;
    }
    return find_entry_mut((FengSemanticAnalysis *)analysis_const, expr);
}
