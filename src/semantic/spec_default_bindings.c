/* Phase S2-a — SpecDefaultBinding sidecar.
 *
 * Stores one entry per AST node that the resolver confirmed as a
 * default-witness site for a spec-typed slot. See
 * dev/feng-spec-semantic-draft.md §6.3 / §9.3 / §10 Phase S2.
 *
 * The table is populated incrementally during analysis (callers from
 * analyzer.c invoke the record API each time resolve_binding observes a
 * binding without an initializer whose declared type resolves to a spec,
 * and similarly for type fields without a default). Lookups are linear;
 * default-witness sites are sparse so the O(N) scan is acceptable at this
 * stage and avoids a hash-table dependency.
 *
 * Mutation re-uses the cast-away-const pattern established in
 * spec_coercion_sites.c: the public APIs accept
 * `const FengSemanticAnalysis *` to match the analyzer's ResolveContext
 * shape but mutate the underlying object through a single-line cast.
 */

#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static FengSpecDefaultBinding *find_entry_mut(FengSemanticAnalysis *analysis,
                                              const void *site) {
    if (analysis == NULL || site == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_default_binding_count; ++i) {
        if (analysis->spec_default_bindings[i].site == site) {
            return &analysis->spec_default_bindings[i];
        }
    }
    return NULL;
}

static FengSpecDefaultBinding *reserve_entry_slot(FengSemanticAnalysis *analysis,
                                                  const void *site) {
    FengSpecDefaultBinding *existing = find_entry_mut(analysis, site);
    if (existing != NULL) {
        return existing;
    }
    if (analysis->spec_default_binding_count == analysis->spec_default_binding_capacity) {
        size_t new_cap = analysis->spec_default_binding_capacity == 0U
                             ? 16U
                             : analysis->spec_default_binding_capacity * 2U;
        FengSpecDefaultBinding *grown = realloc(analysis->spec_default_bindings,
                                                new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        analysis->spec_default_bindings = grown;
        analysis->spec_default_binding_capacity = new_cap;
    }
    FengSpecDefaultBinding *slot =
        &analysis->spec_default_bindings[analysis->spec_default_binding_count++];
    memset(slot, 0, sizeof(*slot));
    slot->site = site;
    return slot;
}

bool feng_semantic_record_spec_default_binding(
        const FengSemanticAnalysis *analysis_const,
        const void *site,
        FengSpecDefaultBindingPosition position,
        FengSpecCoercionForm form,
        const FengDecl *spec_decl) {
    if (analysis_const == NULL || site == NULL || spec_decl == NULL) {
        return false;
    }
    if (spec_decl->kind != FENG_DECL_SPEC) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengSpecDefaultBinding *slot = reserve_entry_slot(analysis, site);
    if (slot == NULL) {
        return false;
    }
    slot->site = site;
    slot->position = position;
    slot->form = form;
    slot->spec_decl = spec_decl;
    return true;
}

const FengSpecDefaultBinding *feng_semantic_lookup_spec_default_binding(
        const FengSemanticAnalysis *analysis,
        const void *site) {
    if (analysis == NULL || site == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_default_binding_count; ++i) {
        if (analysis->spec_default_bindings[i].site == site) {
            return &analysis->spec_default_bindings[i];
        }
    }
    return NULL;
}
