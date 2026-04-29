/* Phase S2-b — SpecMemberAccess sidecar.
 *
 * Stores one entry per AST member-expression whose owner static type the
 * resolver determined to be an object-form spec. See
 * dev/feng-spec-semantic-draft.md §6.4 / §9.4 / §10 Phase S2.
 *
 * The table is populated incrementally by the analyzer:
 *   - validate_instance_member_expr records FIELD_READ / METHOD_CALL based
 *     on the resolved member kind.
 *   - The assignment statement resolver upgrades a previously-recorded
 *     FIELD_READ to FIELD_WRITE once it observes the member expression as
 *     the LHS target.
 *
 * Lookups are linear; member-access sites against spec-typed values are
 * sparse so the O(N) scan is acceptable at this stage and avoids a hash-
 * table dependency. Mutation re-uses the cast-away-const pattern from
 * spec_coercion_sites.c.
 */

#include "semantic.h"

#include <stdlib.h>
#include <string.h>

static FengSpecMemberAccess *find_entry_mut(FengSemanticAnalysis *analysis,
                                            const FengExpr *expr) {
    if (analysis == NULL || expr == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_member_access_count; ++i) {
        if (analysis->spec_member_accesses[i].expr == expr) {
            return &analysis->spec_member_accesses[i];
        }
    }
    return NULL;
}

static FengSpecMemberAccess *reserve_entry_slot(FengSemanticAnalysis *analysis,
                                                const FengExpr *expr) {
    FengSpecMemberAccess *existing = find_entry_mut(analysis, expr);
    if (existing != NULL) {
        return existing;
    }
    if (analysis->spec_member_access_count == analysis->spec_member_access_capacity) {
        size_t new_cap = analysis->spec_member_access_capacity == 0U
                             ? 16U
                             : analysis->spec_member_access_capacity * 2U;
        FengSpecMemberAccess *grown = realloc(analysis->spec_member_accesses,
                                              new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        analysis->spec_member_accesses = grown;
        analysis->spec_member_access_capacity = new_cap;
    }
    FengSpecMemberAccess *slot =
        &analysis->spec_member_accesses[analysis->spec_member_access_count++];
    memset(slot, 0, sizeof(*slot));
    slot->expr = expr;
    return slot;
}

bool feng_semantic_record_spec_member_access(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *spec_decl,
        const FengTypeMember *member,
        FengSpecMemberAccessKind kind) {
    if (analysis_const == NULL || expr == NULL || spec_decl == NULL || member == NULL) {
        return false;
    }
    if (spec_decl->kind != FENG_DECL_SPEC) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengSpecMemberAccess *slot = reserve_entry_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    slot->expr = expr;
    slot->spec_decl = spec_decl;
    slot->member = member;
    slot->kind = kind;
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        slot->field_mutability = member->as.field.mutability;
    } else {
        slot->field_mutability = FENG_MUTABILITY_LET; /* unused for METHOD_CALL */
    }
    return true;
}

void feng_semantic_upgrade_spec_member_access_to_write(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr) {
    if (analysis_const == NULL || expr == NULL) {
        return;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    FengSpecMemberAccess *slot = find_entry_mut(analysis, expr);
    if (slot == NULL) {
        return;
    }
    if (slot->kind == FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_READ) {
        slot->kind = FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_WRITE;
    }
}

const FengSpecMemberAccess *feng_semantic_lookup_spec_member_access(
        const FengSemanticAnalysis *analysis,
        const FengExpr *expr) {
    if (analysis == NULL || expr == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_member_access_count; ++i) {
        if (analysis->spec_member_accesses[i].expr == expr) {
            return &analysis->spec_member_accesses[i];
        }
    }
    return NULL;
}
