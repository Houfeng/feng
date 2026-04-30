#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static FengSemanticTypeFact *find_entry_mut(FengSemanticAnalysis *analysis, const void *site) {
    size_t index;

    if (analysis == NULL || site == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->type_fact_count; ++index) {
        if (analysis->type_facts[index].site == site) {
            return &analysis->type_facts[index];
        }
    }

    return NULL;
}

static FengSemanticTypeFact *reserve_entry_slot(FengSemanticAnalysis *analysis, const void *site) {
    FengSemanticTypeFact *existing;

    if (analysis == NULL || site == NULL) {
        return NULL;
    }

    existing = find_entry_mut(analysis, site);
    if (existing != NULL) {
        return existing;
    }

    if (analysis->type_fact_count == analysis->type_fact_capacity) {
        size_t new_capacity = analysis->type_fact_capacity == 0U
                                  ? 16U
                                  : analysis->type_fact_capacity * 2U;
        FengSemanticTypeFact *grown = (FengSemanticTypeFact *)realloc(
            analysis->type_facts,
            new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        analysis->type_facts = grown;
        analysis->type_fact_capacity = new_capacity;
    }

    existing = &analysis->type_facts[analysis->type_fact_count++];
    memset(existing, 0, sizeof(*existing));
    existing->site = site;
    return existing;
}

bool feng_semantic_record_type_fact(const FengSemanticAnalysis *analysis_const,
                                    const void *site,
                                    FengSemanticTypeFactKind kind,
                                    FengSlice builtin_name,
                                    const FengTypeRef *type_ref,
                                    const FengDecl *type_decl) {
    FengSemanticAnalysis *analysis;
    FengSemanticTypeFact *slot;

    if (analysis_const == NULL || site == NULL) {
        return false;
    }

    analysis = (FengSemanticAnalysis *)analysis_const;
    slot = reserve_entry_slot(analysis, site);
    if (slot == NULL) {
        return false;
    }

    slot->site = site;
    slot->kind = kind;
    slot->builtin_name = builtin_name;
    slot->type_ref = type_ref;
    slot->type_decl = type_decl;
    return true;
}

const FengSemanticTypeFact *feng_semantic_lookup_type_fact(const FengSemanticAnalysis *analysis,
                                                           const void *site) {
    size_t index;

    if (analysis == NULL || site == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->type_fact_count; ++index) {
        if (analysis->type_facts[index].site == site) {
            return &analysis->type_facts[index];
        }
    }

    return NULL;
}
