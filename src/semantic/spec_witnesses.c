/* Phase S3 — SpecWitness sidecar.
 *
 * Stores one entry per (type_decl, spec_decl) pair that the analyzer has
 * been asked to materialise (per §8.2 — on-demand cache, populated the first
 * time a coercion site for the (T, S) pair is recorded). Each entry holds
 * the per-member implementation source resolved against T's visible face
 * (T's own members + visible fits, per §8.1). See
 * dev/feng-spec-semantic-draft.md §6.5 / §9.5 / §10 Phase S3.
 *
 * Mutation is funneled through this translation unit so the analyzer never
 * touches the storage layout directly. The reserve/append split lets the
 * analyzer build up a witness incrementally while iterating S's closure
 * without intermediate scratch buffers; once reserved, the witness pointer
 * is stable until feng_semantic_analysis_free.
 *
 * Lookups are linear; (T, S) coercions are sparse so the O(N) scan is
 * acceptable at this stage and avoids a hash-table dependency. The
 * cast-away-const idiom matches the other sidecars (spec_coercion_sites.c,
 * spec_default_bindings.c, spec_member_accesses.c).
 */

#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static FengSpecWitness *find_entry_mut(FengSemanticAnalysis *analysis,
                                       const FengDecl *type_decl,
                                       const FengDecl *spec_decl) {
    if (analysis == NULL || type_decl == NULL || spec_decl == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_witness_count; ++i) {
        FengSpecWitness *entry = &analysis->spec_witnesses[i];
        if (entry->type_decl == type_decl && entry->spec_decl == spec_decl) {
            return entry;
        }
    }
    return NULL;
}

const FengSpecWitness *feng_semantic_lookup_spec_witness(
        const FengSemanticAnalysis *analysis_const,
        const FengDecl *type_decl,
        const FengDecl *spec_decl) {
    if (analysis_const == NULL) {
        return NULL;
    }
    return find_entry_mut((FengSemanticAnalysis *)analysis_const,
                          type_decl, spec_decl);
}

FengSpecWitness *feng_semantic_reserve_spec_witness(
        const FengSemanticAnalysis *analysis_const,
        const FengDecl *type_decl,
        const FengDecl *spec_decl) {
    if (analysis_const == NULL || type_decl == NULL || spec_decl == NULL) {
        return NULL;
    }
    if (type_decl->kind != FENG_DECL_TYPE || spec_decl->kind != FENG_DECL_SPEC) {
        return NULL;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    if (find_entry_mut(analysis, type_decl, spec_decl) != NULL) {
        return NULL;
    }
    if (analysis->spec_witness_count == analysis->spec_witness_capacity) {
        size_t new_cap = analysis->spec_witness_capacity == 0U
                             ? 8U
                             : analysis->spec_witness_capacity * 2U;
        FengSpecWitness *grown = realloc(analysis->spec_witnesses,
                                         new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        analysis->spec_witnesses = grown;
        analysis->spec_witness_capacity = new_cap;
    }
    FengSpecWitness *slot = &analysis->spec_witnesses[analysis->spec_witness_count++];
    memset(slot, 0, sizeof(*slot));
    slot->type_decl = type_decl;
    slot->spec_decl = spec_decl;
    slot->members = NULL;
    slot->member_count = 0U;
    slot->member_capacity = 0U;
    return slot;
}

bool feng_semantic_spec_witness_append_member(
        FengSpecWitness *witness,
        const FengTypeMember *spec_member,
        const FengTypeMember *impl_member,
        FengSpecWitnessSourceKind source_kind,
        const FengDecl *via_fit_decl,
        const FengSemanticModule *provider_module) {
    if (witness == NULL || spec_member == NULL) {
        return false;
    }
    /* via_fit_decl / provider_module are bound to FIT_METHOD. */
    if (source_kind == FENG_SPEC_WITNESS_SOURCE_FIT_METHOD) {
        if (impl_member != NULL && (via_fit_decl == NULL || provider_module == NULL)) {
            return false;
        }
    } else {
        if (via_fit_decl != NULL || provider_module != NULL) {
            return false;
        }
    }
    if (witness->member_count == witness->member_capacity) {
        size_t new_cap = witness->member_capacity == 0U
                             ? 8U
                             : witness->member_capacity * 2U;
        FengSpecWitnessMember *grown = realloc(witness->members,
                                               new_cap * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        witness->members = grown;
        witness->member_capacity = new_cap;
    }
    FengSpecWitnessMember *slot = &witness->members[witness->member_count++];
    slot->spec_member = spec_member;
    slot->impl_member = impl_member;
    slot->source_kind = source_kind;
    slot->via_fit_decl = via_fit_decl;
    slot->provider_module = provider_module;
    return true;
}
