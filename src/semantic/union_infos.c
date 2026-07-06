#include "semantic/semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void free_owned_type_ref(FengTypeRef *type_ref) {
    if (type_ref == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            for (size_t index = 0U; index < type_ref->as.named.type_arg_count; ++index) {
                free_owned_type_ref(type_ref->as.named.type_args[index]);
            }
            free(type_ref->as.named.segments);
            free(type_ref->as.named.type_args);
            break;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            free_owned_type_ref(type_ref->as.inner);
            break;
    }

    free(type_ref);
}


static void free_union_members(FengUnionSpecMemberInfo *members, size_t member_count) {
    if (members == NULL) {
        return;
    }
    for (size_t index = 0U; index < member_count; ++index) {
        free_owned_type_ref((FengTypeRef *)members[index].type_ref);
    }
    free(members);
}

bool feng_semantic_record_union_spec_info(
    const FengSemanticAnalysis *analysis_const,
    const FengDecl *spec_decl,
    FengUnionSpecMemberInfo *members,
    size_t member_count) {
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengUnionSpecInfo *slot = NULL;

    if (analysis == NULL || spec_decl == NULL || spec_decl->kind != FENG_DECL_SPEC ||
        spec_decl->as.spec_decl.form != FENG_SPEC_FORM_UNION) {
        free_union_members(members, member_count);
        return false;
    }

    for (size_t index = 0U; index < analysis->union_spec_info_count; ++index) {
        if (analysis->union_spec_infos[index].spec_decl == spec_decl) {
            slot = &analysis->union_spec_infos[index];
            break;
        }
    }

    if (slot == NULL) {
        if (analysis->union_spec_info_count == analysis->union_spec_info_capacity) {
            size_t new_capacity = analysis->union_spec_info_capacity == 0U
                                      ? 8U
                                      : analysis->union_spec_info_capacity * 2U;
            FengUnionSpecInfo *grown;

            if (new_capacity > SIZE_MAX / sizeof(*grown)) {
                free_union_members(members, member_count);
                return false;
            }
            grown = (FengUnionSpecInfo *)realloc(analysis->union_spec_infos,
                                                 new_capacity * sizeof(*grown));
            if (grown == NULL) {
                free_union_members(members, member_count);
                return false;
            }
            analysis->union_spec_infos = grown;
            analysis->union_spec_info_capacity = new_capacity;
        }
        slot = &analysis->union_spec_infos[analysis->union_spec_info_count++];
        slot->spec_decl = spec_decl;
        slot->members = NULL;
        slot->member_count = 0U;
    } else {
        free_union_members(slot->members, slot->member_count);
    }

    slot->members = members;
    slot->member_count = member_count;
    return true;
}

const FengUnionSpecInfo *feng_semantic_lookup_union_spec_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *spec_decl) {
    if (analysis == NULL || spec_decl == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < analysis->union_spec_info_count; ++index) {
        if (analysis->union_spec_infos[index].spec_decl == spec_decl) {
            return &analysis->union_spec_infos[index];
        }
    }
    return NULL;
}

bool feng_semantic_record_union_coercion_site(
    const FengSemanticAnalysis *analysis_const,
    const FengExpr *expr,
    const FengDecl *target_union_decl,
    const FengTypeRef *target_union_type_ref,
    size_t member_index,
    const FengTypeRef *member_type_ref,
    const size_t *path_indices,
    size_t path_length) {
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengUnionCoercionSite *slot = NULL;

    if (analysis == NULL || expr == NULL || target_union_decl == NULL ||
        target_union_decl->kind != FENG_DECL_SPEC ||
        target_union_decl->as.spec_decl.form != FENG_SPEC_FORM_UNION ||
        member_type_ref == NULL) {
        return false;
    }
    if (path_length > UNION_COERCION_MAX_PATH_DEPTH) {
        return false;
    }

    for (size_t index = 0U; index < analysis->union_coercion_site_count; ++index) {
        if (analysis->union_coercion_sites[index].expr == expr) {
            slot = &analysis->union_coercion_sites[index];
            break;
        }
    }

    if (slot == NULL) {
        if (analysis->union_coercion_site_count == analysis->union_coercion_site_capacity) {
            size_t new_capacity = analysis->union_coercion_site_capacity == 0U
                                      ? 8U
                                      : analysis->union_coercion_site_capacity * 2U;
            FengUnionCoercionSite *grown;

            if (new_capacity > SIZE_MAX / sizeof(*grown)) {
                return false;
            }
            grown = (FengUnionCoercionSite *)realloc(analysis->union_coercion_sites,
                                                     new_capacity * sizeof(*grown));
            if (grown == NULL) {
                return false;
            }
            analysis->union_coercion_sites = grown;
            analysis->union_coercion_site_capacity = new_capacity;
        }
        slot = &analysis->union_coercion_sites[analysis->union_coercion_site_count++];
    }

    slot->expr = expr;
    slot->target_union_decl = target_union_decl;
    slot->target_union_type_ref = target_union_type_ref;
    slot->member_index = member_index;
    slot->member_type_ref = member_type_ref;
    slot->path_length = path_length;
    if (path_length > 0U && path_indices != NULL) {
        memcpy(slot->path_indices, path_indices, path_length * sizeof(size_t));
    }
    return true;
}

const FengUnionCoercionSite *feng_semantic_lookup_union_coercion_site(
    const FengSemanticAnalysis *analysis,
    const FengExpr *expr) {
    if (analysis == NULL || expr == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < analysis->union_coercion_site_count; ++index) {
        if (analysis->union_coercion_sites[index].expr == expr) {
            return &analysis->union_coercion_sites[index];
        }
    }
    return NULL;
}

void feng_semantic_free_union_spec_infos(FengSemanticAnalysis *analysis) {
    if (analysis == NULL) {
        return;
    }
    for (size_t index = 0U; index < analysis->union_spec_info_count; ++index) {
        free_union_members(analysis->union_spec_infos[index].members,
                           analysis->union_spec_infos[index].member_count);
    }
    free(analysis->union_spec_infos);
    analysis->union_spec_infos = NULL;
    analysis->union_spec_info_count = 0U;
    analysis->union_spec_info_capacity = 0U;
}
