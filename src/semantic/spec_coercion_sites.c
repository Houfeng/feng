/* Phase S1b — SpecCoercionSite sidecar.
 *
 * Stores one entry per AST FengExpr that the resolver confirmed as a
 * coercion into a spec slot (object-form or callable-form). See
 * dev/feng-spec-semantic-draft.md §6.2 / §8.3 / §8.4 / §10 Phase S1.
 *
 * The table is populated incrementally during analysis (callers from
 * analyzer.c invoke the record_* APIs each time validate_expr_against_-
 * expected_type / validate_function_typed_expr confirms a successful match
 * into a spec slot). Lookups are linear; coercion sites are sparse so the
 * O(N) scan is acceptable at this stage and avoids a hash table dependency.
 *
 * Mutation re-uses the cast-away-const pattern already established in
 * analyzer.c (see analysis_append_info): the public APIs accept
 * `const FengSemanticAnalysis *` to match the analyzer's ResolveContext
 * shape but mutate the underlying object through a single-line cast.
 */

#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static FengSpecCoercionSite *find_site_mut(FengSemanticAnalysis *analysis,
                                           const FengExpr *expr) {
    if (analysis == NULL || expr == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_coercion_site_count; ++i) {
        if (analysis->spec_coercion_sites[i].expr == expr) {
            return &analysis->spec_coercion_sites[i];
        }
    }
    return NULL;
}

static FengSpecCoercionSite *reserve_site_slot(FengSemanticAnalysis *analysis,
                                               const FengExpr *expr) {
    FengSpecCoercionSite *existing = find_site_mut(analysis, expr);
    if (existing != NULL) {
        return existing;
    }
    if (analysis->spec_coercion_site_count == analysis->spec_coercion_site_capacity) {
        size_t new_cap = analysis->spec_coercion_site_capacity == 0U
                             ? 16U
                             : analysis->spec_coercion_site_capacity * 2U;
        FengSpecCoercionSite *grown = realloc(analysis->spec_coercion_sites,
                                              new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        analysis->spec_coercion_sites = grown;
        analysis->spec_coercion_site_capacity = new_cap;
    }
    FengSpecCoercionSite *slot = &analysis->spec_coercion_sites[analysis->spec_coercion_site_count++];
    memset(slot, 0, sizeof(*slot));
    slot->expr = expr;
    return slot;
}

bool feng_semantic_record_object_spec_coercion_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *src_type_decl,
        const FengDecl *target_spec_decl,
        const FengSpecRelation *relation) {
    if (analysis_const == NULL || expr == NULL || src_type_decl == NULL ||
        target_spec_decl == NULL || relation == NULL) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengSpecCoercionSite *slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_OBJECT;
    slot->src_type_decl = src_type_decl;
    slot->target_spec_decl = target_spec_decl;
    slot->relation = relation;
    slot->callable_source = FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER; /* unused */
    return true;
}

bool feng_semantic_record_callable_spec_coercion_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *target_spec_decl,
        FengSpecCoercionCallableSource callable_source) {
    if (analysis_const == NULL || expr == NULL || target_spec_decl == NULL) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengSpecCoercionSite *slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_CALLABLE;
    slot->src_type_decl = NULL;
    slot->target_spec_decl = target_spec_decl;
    slot->relation = NULL;
    slot->callable_source = callable_source;
    return true;
}

const FengSpecCoercionSite *feng_semantic_lookup_spec_coercion_site(
        const FengSemanticAnalysis *analysis,
        const FengExpr *expr) {
    if (analysis == NULL || expr == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_coercion_site_count; ++i) {
        if (analysis->spec_coercion_sites[i].expr == expr) {
            return &analysis->spec_coercion_sites[i];
        }
    }
    return NULL;
}
