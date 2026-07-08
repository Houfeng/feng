#include "semantic/semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool feng_semantic_record_intersection_spec_info(
    const FengSemanticAnalysis *analysis_const,
    const FengDecl *spec_decl,
    const FengDecl **flattened_members,
    size_t flattened_member_count) {
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengIntersectionSpecInfo *slot = NULL;

    if (analysis == NULL || spec_decl == NULL || spec_decl->kind != FENG_DECL_SPEC ||
        spec_decl->as.spec_decl.form != FENG_SPEC_FORM_INTERSECTION) {
        free((void *)flattened_members);
        return false;
    }

    for (size_t index = 0U; index < analysis->intersection_spec_info_count; ++index) {
        if (analysis->intersection_spec_infos[index].spec_decl == spec_decl) {
            slot = &analysis->intersection_spec_infos[index];
            break;
        }
    }

    if (slot == NULL) {
        if (analysis->intersection_spec_info_count == analysis->intersection_spec_info_capacity) {
            size_t new_capacity = analysis->intersection_spec_info_capacity == 0U
                                      ? 8U
                                      : analysis->intersection_spec_info_capacity * 2U;
            FengIntersectionSpecInfo *grown;

            if (new_capacity > SIZE_MAX / sizeof(*grown)) {
                free((void *)flattened_members);
                return false;
            }
            grown = (FengIntersectionSpecInfo *)realloc(analysis->intersection_spec_infos,
                                                        new_capacity * sizeof(*grown));
            if (grown == NULL) {
                free((void *)flattened_members);
                return false;
            }
            analysis->intersection_spec_infos = grown;
            analysis->intersection_spec_info_capacity = new_capacity;
        }
        slot = &analysis->intersection_spec_infos[analysis->intersection_spec_info_count++];
        slot->spec_decl = spec_decl;
        slot->flattened_members = NULL;
        slot->flattened_member_count = 0U;
    } else {
        free((void *)slot->flattened_members);
    }

    slot->flattened_members = flattened_members;
    slot->flattened_member_count = flattened_member_count;
    return true;
}

const FengIntersectionSpecInfo *feng_semantic_lookup_intersection_spec_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *spec_decl) {
    if (analysis == NULL || spec_decl == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < analysis->intersection_spec_info_count; ++index) {
        if (analysis->intersection_spec_infos[index].spec_decl == spec_decl) {
            return &analysis->intersection_spec_infos[index];
        }
    }
    return NULL;
}

void feng_semantic_free_intersection_spec_infos(FengSemanticAnalysis *analysis) {
    if (analysis == NULL) {
        return;
    }
    for (size_t index = 0U; index < analysis->intersection_spec_info_count; ++index) {
        free((void *)analysis->intersection_spec_infos[index].flattened_members);
    }
    free(analysis->intersection_spec_infos);
    analysis->intersection_spec_infos = NULL;
    analysis->intersection_spec_info_count = 0U;
    analysis->intersection_spec_info_capacity = 0U;
}
