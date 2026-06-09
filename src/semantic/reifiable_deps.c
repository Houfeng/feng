#include "semantic.h"

#include <stdlib.h>
#include <string.h>

FengReifiableDepSet *feng_semantic_get_or_create_reifiable_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl) {
    size_t index;
    FengReifiableDepSet *slot;

    if (analysis == NULL || owner_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->reifiable_dep_set_count; ++index) {
        if (analysis->reifiable_dep_sets[index].owner_decl == owner_decl) {
            return &analysis->reifiable_dep_sets[index];
        }
    }

    if (analysis->reifiable_dep_set_count == analysis->reifiable_dep_set_capacity) {
        size_t new_capacity = analysis->reifiable_dep_set_capacity == 0U
                                  ? 8U
                                  : analysis->reifiable_dep_set_capacity * 2U;
        FengReifiableDepSet *grown = (FengReifiableDepSet *)realloc(
            analysis->reifiable_dep_sets,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        analysis->reifiable_dep_sets = grown;
        analysis->reifiable_dep_set_capacity = new_capacity;
    }

    slot = &analysis->reifiable_dep_sets[analysis->reifiable_dep_set_count++];
    memset(slot, 0, sizeof(*slot));
    slot->owner_decl = owner_decl;
    return slot;
}

bool feng_semantic_reifiable_dep_set_append(
    FengReifiableDepSet *dep_set,
    FengReifiableDepKind kind,
    const FengTypeRef *type_ref) {
    size_t index;
    FengReifiableDep *slot;

    if (dep_set == NULL || type_ref == NULL) {
        return false;
    }

    /* 去重：相同 type_ref 指针不重复追加。 */
    for (index = 0U; index < dep_set->dep_count; ++index) {
        if (dep_set->deps[index].type_ref == type_ref &&
            dep_set->deps[index].kind == kind) {
            return true;
        }
    }

    if (dep_set->dep_count == dep_set->dep_capacity) {
        size_t new_capacity = dep_set->dep_capacity == 0U
                                  ? 8U
                                  : dep_set->dep_capacity * 2U;
        FengReifiableDep *grown = (FengReifiableDep *)realloc(
            dep_set->deps,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        dep_set->deps = grown;
        dep_set->dep_capacity = new_capacity;
    }

    slot = &dep_set->deps[dep_set->dep_count++];
    slot->kind = kind;
    slot->type_ref = type_ref;
    return true;
}

const FengReifiableDepSet *feng_semantic_lookup_reifiable_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl) {
    size_t index;

    if (analysis == NULL || owner_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->reifiable_dep_set_count; ++index) {
        if (analysis->reifiable_dep_sets[index].owner_decl == owner_decl) {
            return &analysis->reifiable_dep_sets[index];
        }
    }

    return NULL;
}
