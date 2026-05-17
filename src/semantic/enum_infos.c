#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static bool slice_equals(FengSlice left, FengSlice right) {
    return left.length == right.length &&
           (left.length == 0U || memcmp(left.data, right.data, left.length) == 0);
}

static FengSemanticEnumInfo *find_enum_info_mut(FengSemanticAnalysis *analysis,
                                                const FengDecl *enum_decl) {
    size_t index;

    if (analysis == NULL || enum_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->enum_info_count; ++index) {
        if (analysis->enum_infos[index].enum_decl == enum_decl) {
            return &analysis->enum_infos[index];
        }
    }

    return NULL;
}




static FengSemanticEnumInfo *reserve_enum_info_slot(FengSemanticAnalysis *analysis,
                                                    const FengDecl *enum_decl) {
    FengSemanticEnumInfo *slot;

    if (analysis == NULL || enum_decl == NULL) {
        return NULL;
    }

    slot = find_enum_info_mut(analysis, enum_decl);
    if (slot != NULL) {
        return slot;
    }

    if (analysis->enum_info_count == analysis->enum_info_capacity) {
        size_t new_capacity = analysis->enum_info_capacity == 0U
                                  ? 8U
                                  : analysis->enum_info_capacity * 2U;
        FengSemanticEnumInfo *grown = (FengSemanticEnumInfo *)realloc(
            analysis->enum_infos,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        analysis->enum_infos = grown;
        analysis->enum_info_capacity = new_capacity;
    }

    slot = &analysis->enum_infos[analysis->enum_info_count++];
    memset(slot, 0, sizeof(*slot));
    slot->enum_decl = enum_decl;
    return slot;
}

static FengSemanticEnumItemInfo *find_enum_item_info_mut(FengSemanticEnumInfo *info,
                                                         const FengEnumItem *item) {
    size_t index;

    if (info == NULL || item == NULL) {
        return NULL;
    }

    for (index = 0U; index < info->item_count; ++index) {
        if (info->items[index].item == item) {
            return &info->items[index];
        }
    }

    return NULL;
}

static FengSemanticEnumItemInfo *reserve_enum_item_slot(FengSemanticEnumInfo *info,
                                                        const FengEnumItem *item) {
    FengSemanticEnumItemInfo *slot;

    if (info == NULL || item == NULL) {
        return NULL;
    }

    slot = find_enum_item_info_mut(info, item);
    if (slot != NULL) {
        return slot;
    }

    if (info->item_count == info->item_capacity) {
        size_t new_capacity = info->item_capacity == 0U ? 8U : info->item_capacity * 2U;
        FengSemanticEnumItemInfo *grown = (FengSemanticEnumItemInfo *)realloc(
            info->items,
            new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        info->items = grown;
        info->item_capacity = new_capacity;
    }

    slot = &info->items[info->item_count++];
    memset(slot, 0, sizeof(*slot));
    slot->item = item;
    return slot;
}

bool feng_semantic_record_enum_item_info(const FengSemanticAnalysis *analysis_const,
                                         const FengDecl *enum_decl,
                                         const FengEnumItem *item,
                                         size_t ordinal,
                                         int64_t value) {
    FengSemanticAnalysis *analysis;
    FengSemanticEnumInfo *info;
    FengSemanticEnumItemInfo *slot;

    if (analysis_const == NULL || enum_decl == NULL || item == NULL) {
        return false;
    }

    analysis = (FengSemanticAnalysis *)analysis_const;
    info = reserve_enum_info_slot(analysis, enum_decl);
    if (info == NULL) {
        return false;
    }

    slot = reserve_enum_item_slot(info, item);
    if (slot == NULL) {
        return false;
    }

    slot->item = item;
    slot->ordinal = ordinal;
    slot->value = value;
    if (ordinal == 0U || info->first_item == NULL) {
        info->first_item = item;
        info->first_value = value;
    }
    return true;
}

const FengSemanticEnumInfo *feng_semantic_lookup_enum_info(const FengSemanticAnalysis *analysis,
                                                           const FengDecl *enum_decl) {
    size_t index;

    if (analysis == NULL || enum_decl == NULL) {
        return NULL;
    }

    for (index = 0U; index < analysis->enum_info_count; ++index) {
        if (analysis->enum_infos[index].enum_decl == enum_decl) {
            return &analysis->enum_infos[index];
        }
    }

    return NULL;
}

const FengSemanticEnumItemInfo *feng_semantic_find_enum_item_info(
    const FengSemanticAnalysis *analysis,
    const FengDecl *enum_decl,
    FengSlice item_name) {
    const FengSemanticEnumInfo *info;
    size_t index;

    info = feng_semantic_lookup_enum_info(analysis, enum_decl);
    if (info == NULL) {
        return NULL;
    }

    for (index = 0U; index < info->item_count; ++index) {
        if (info->items[index].item != NULL &&
            slice_equals(info->items[index].item->name, item_name)) {
            return &info->items[index];
        }
    }

    return NULL;
}
