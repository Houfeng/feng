/* Declaration-time spec implementation selection sidecar.
 *
 * Contract validation records the exact concrete member chosen for each
 * declared nominal relationship. Provider Symbol and Codegen reuse this
 * fact when a package-public relationship depends on a seal implementation;
 * consumer compilation continues to rely on the ordinary package-public FT
 * surface. No entry is serialized or consulted for source-level access.
 */

#include "semantic.h"

#include <stdlib.h>

/* Return whether one relation owner can contribute dependencies to the
 * package-public FT surface. */
static bool relation_owner_is_package_public(
    const FengSpecImplementationSelection *selection) {
    if (selection == NULL || selection->relation_owner_module == NULL ||
        selection->relation_owner_decl == NULL) {
        return false;
    }
    if (selection->relation_owner_module->visibility !=
            FENG_VISIBILITY_PUBLIC ||
        selection->relation_owner_decl->visibility !=
            FENG_VISIBILITY_PUBLIC) {
        return false;
    }
    return selection->relation_owner_decl->kind == FENG_DECL_TYPE ||
           selection->relation_owner_decl->kind == FENG_DECL_FIT;
}

bool feng_semantic_record_spec_implementation_selection(
    const FengSemanticAnalysis *analysis_const,
    const FengSemanticModule *relation_owner_module,
    const FengDecl *relation_owner_decl,
    const FengDecl *spec_decl,
    const FengTypeMember *spec_member,
    const FengTypeMember *impl_member) {
    FengSemanticAnalysis *analysis;
    size_t index;

    if (analysis_const == NULL || relation_owner_module == NULL ||
        relation_owner_decl == NULL || spec_decl == NULL ||
        spec_member == NULL || impl_member == NULL ||
        (relation_owner_decl->kind != FENG_DECL_TYPE &&
         relation_owner_decl->kind != FENG_DECL_FIT) ||
        spec_decl->kind != FENG_DECL_SPEC) {
        return false;
    }

    analysis = (FengSemanticAnalysis *)analysis_const;
    for (index = 0U;
         index < analysis->spec_implementation_selection_count;
         ++index) {
        const FengSpecImplementationSelection *selection =
            &analysis->spec_implementation_selections[index];

        if (selection->relation_owner_module == relation_owner_module &&
            selection->relation_owner_decl == relation_owner_decl &&
            selection->spec_decl == spec_decl &&
            selection->spec_member == spec_member &&
            selection->impl_member == impl_member) {
            return true;
        }
    }

    if (analysis->spec_implementation_selection_count ==
        analysis->spec_implementation_selection_capacity) {
        size_t new_capacity =
            analysis->spec_implementation_selection_capacity == 0U
                ? 8U
                : analysis->spec_implementation_selection_capacity * 2U;
        FengSpecImplementationSelection *grown =
            (FengSpecImplementationSelection *)realloc(
                analysis->spec_implementation_selections,
                new_capacity * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        analysis->spec_implementation_selections = grown;
        analysis->spec_implementation_selection_capacity = new_capacity;
    }

    {
        FengSpecImplementationSelection *selection =
            &analysis->spec_implementation_selections[
                analysis->spec_implementation_selection_count++];

        selection->relation_owner_module = relation_owner_module;
        selection->relation_owner_decl = relation_owner_decl;
        selection->spec_decl = spec_decl;
        selection->spec_member = spec_member;
        selection->impl_member = impl_member;
    }
    return true;
}

bool feng_semantic_member_is_package_spec_implementation_dependency(
    const FengSemanticAnalysis *analysis,
    const FengTypeMember *member) {
    size_t index;

    if (analysis == NULL || member == NULL ||
        member->visibility != FENG_VISIBILITY_PRIVATE) {
        return false;
    }
    for (index = 0U;
         index < analysis->spec_implementation_selection_count;
         ++index) {
        const FengSpecImplementationSelection *selection =
            &analysis->spec_implementation_selections[index];

        if (selection->impl_member == member &&
            relation_owner_is_package_public(selection)) {
            return true;
        }
    }
    return false;
}
