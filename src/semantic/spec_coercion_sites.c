/* Phase 1B — SpecCoercionSite sidecar.
 *
 * Stores one entry per AST FengExpr that the resolver confirmed as a
 * coercion into a spec slot (object-form or callable-form). See
 * docs/engineering/feng-spec-semantic-delivered.md §6.2 / §8.3 / §8.4 / §10 Phase S1.
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
 *
 * Ownership: target_spec_type_ref and structured array subject keys may be
 * borrowed from the AST or from the resolver's per-program synthetic
 * type-ref pool (freed before codegen). Stored metadata therefore clones
 * these refs into analysis-owned storage released after codegen. The clone
 * is a compiler-time cost only; no runtime overhead.
 */

#include "semantic.h"

#include <stdlib.h>
#include <string.h>

/* Free one analysis-owned type-ref clone, including every recursively owned
 * type argument and inner wrapper. Source token slices remain borrowed. */
static void free_type_ref_clone_for_analysis(FengTypeRef *ref) {
    if (ref == NULL) {
        return;
    }
    switch (ref->kind) {
        case FENG_TYPE_REF_NAMED:
            if (ref->as.named.type_args != NULL) {
                for (size_t index = 0U;
                     index < ref->as.named.type_arg_count;
                     ++index) {
                    free_type_ref_clone_for_analysis(ref->as.named.type_args[index]);
                }
            }
            free(ref->as.named.type_args);
            free(ref->as.named.segments);
            break;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            free_type_ref_clone_for_analysis(ref->as.inner);
            break;
    }
    free(ref);
}

/* Deep-clone a FengTypeRef onto the heap. Mirrors the structure handled by
 * resolver's free_synthetic_type_ref so the clone can be freed by the same
 * path. Returns NULL only on allocation failure. */
static FengTypeRef *clone_type_ref_for_analysis(const FengTypeRef *ref) {
    FengTypeRef *clone;

    if (ref == NULL) {
        return NULL;
    }
    clone = (FengTypeRef *)calloc(1U, sizeof(*clone));
    if (clone == NULL) {
        return NULL;
    }
    clone->token = ref->token;
    clone->kind = ref->kind;
    clone->resolution_program = ref->resolution_program;
    clone->array_element_writable = ref->array_element_writable;
    switch (ref->kind) {
        case FENG_TYPE_REF_NAMED:
            clone->as.named.segment_count = ref->as.named.segment_count;
            clone->as.named.type_arg_count = ref->as.named.type_arg_count;
            if (ref->as.named.segment_count > 0U) {
                clone->as.named.segments =
                    (FengSlice *)malloc(sizeof(FengSlice) * ref->as.named.segment_count);
                if (clone->as.named.segments == NULL) {
                    free_type_ref_clone_for_analysis(clone);
                    return NULL;
                }
                memcpy(clone->as.named.segments, ref->as.named.segments,
                       sizeof(FengSlice) * ref->as.named.segment_count);
            }
            if (ref->as.named.type_arg_count > 0U) {
                clone->as.named.type_args =
                    (FengTypeRef **)calloc(ref->as.named.type_arg_count,
                                            sizeof(FengTypeRef *));
                if (clone->as.named.type_args == NULL) {
                    free_type_ref_clone_for_analysis(clone);
                    return NULL;
                }
                for (size_t i = 0U; i < ref->as.named.type_arg_count; ++i) {
                    clone->as.named.type_args[i] =
                        clone_type_ref_for_analysis(ref->as.named.type_args[i]);
                    if (clone->as.named.type_args[i] == NULL) {
                        free_type_ref_clone_for_analysis(clone);
                        return NULL;
                    }
                }
            }
            return clone;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            clone->as.inner = clone_type_ref_for_analysis(ref->as.inner);
            if (clone->as.inner == NULL) {
                free_type_ref_clone_for_analysis(clone);
                return NULL;
            }
            return clone;
    }
    free_type_ref_clone_for_analysis(clone);
    return NULL;
}

/* Track a heap-allocated FengTypeRef under analysis->coercion_owned_type_refs
 * so it is freed (recursively) by feng_semantic_analysis_free, which runs
 * after codegen. Returns true on success; on failure the ref is freed here
 * so the caller does not need to track it. */
static bool analysis_own_type_ref(FengSemanticAnalysis *analysis, FengTypeRef *ref) {
    if (ref == NULL) {
        return true;
    }
    if (analysis->coercion_owned_type_ref_count == analysis->coercion_owned_type_ref_capacity) {
        size_t new_capacity = analysis->coercion_owned_type_ref_capacity == 0U
                                  ? 8U
                                  : analysis->coercion_owned_type_ref_capacity * 2U;
        FengTypeRef **grown = (FengTypeRef **)realloc(
            analysis->coercion_owned_type_refs,
            new_capacity * sizeof(*grown));
        if (grown == NULL) {
            free_type_ref_clone_for_analysis(ref);
            return false;
        }
        analysis->coercion_owned_type_refs = grown;
        analysis->coercion_owned_type_ref_capacity = new_capacity;
    }
    analysis->coercion_owned_type_refs[analysis->coercion_owned_type_ref_count++] = ref;
    return true;
}

/* Clone `target_spec_type_ref` into analysis-owned storage so the coercion
 * site can safely reference it across the codegen pass. On allocation
 * failure returns NULL and the caller must skip recording the site. */
static const FengTypeRef *analysis_clone_type_ref(FengSemanticAnalysis *analysis,
                                                   const FengTypeRef *target_spec_type_ref) {
    FengTypeRef *clone = clone_type_ref_for_analysis(target_spec_type_ref);
    if (clone == NULL) {
        return NULL;
    }
    if (!analysis_own_type_ref(analysis, clone)) {
        return NULL;
    }
    return clone;
}

/* Copy one subject key into analysis-owned storage. The complete array tree
 * must survive resolver-local synthetic type refs because relation, witness,
 * and coercion metadata remain observable through codegen. */
bool feng_semantic_subject_key_copy_for_analysis(
        const FengSemanticAnalysis *analysis_const,
        const FengSemanticSubjectKey *source,
        FengSemanticSubjectKey *out_key) {
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;

    if (analysis == NULL || source == NULL || out_key == NULL) {
        return false;
    }
    *out_key = *source;
    if (source->kind == FENG_SEMANTIC_SUBJECT_KEY_ARRAY &&
        source->as.array.rank > 64U) {
        const FengTypeRef *owned_array_type_ref = analysis_clone_type_ref(
            analysis, source->as.array.array_type_ref);

        if (owned_array_type_ref == NULL) {
            memset(out_key, 0, sizeof(*out_key));
            return false;
        }
        out_key->as.array.array_type_ref = owned_array_type_ref;
    }
    return true;
}

/* Own a closed array instance when no declaration-lifetime relation key can
 * supply it. The compiler-only clone survives generic-call resolver cleanup. */
bool feng_semantic_subject_key_from_array_instance(
    const FengSemanticAnalysis *analysis,
    const FengTypeRef *array_type_ref,
    FengSemanticSubjectKey *out_key) {
    const FengTypeRef *owned = analysis_clone_type_ref(
        (FengSemanticAnalysis *)analysis, array_type_ref);
    return owned != NULL && feng_semantic_subject_key_init_array_from_type_ref(out_key, owned);
}

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

/* Release arrays owned directly by one sidecar slot before replacing its
 * classification. Type-ref trees themselves remain owned by the analysis-wide
 * coercion type-ref arena and are released with the analysis. */
static void reset_site_payload(FengSpecCoercionSite *slot) {
    if (slot == NULL) {
        return;
    }
    free(slot->object_upcast_parent_indices);
    free((void *)slot->callable_type_args);
    memset(slot, 0, sizeof(*slot));
}

bool feng_semantic_record_object_spec_coercion_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengSemanticSubjectKey *src_subject_key,
        const FengDecl *target_spec_decl,
        const FengTypeRef *target_spec_type_ref,
    const FengSpecRelation *relation,
    FengSpecObjectSubjectStorageKind object_subject_storage) {
    if (analysis_const == NULL || expr == NULL || src_subject_key == NULL ||
        src_subject_key->kind == FENG_SEMANTIC_SUBJECT_KEY_INVALID ||
        target_spec_decl == NULL || target_spec_type_ref == NULL || relation == NULL) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    const FengTypeRef *owned_type_ref = analysis_clone_type_ref(analysis, target_spec_type_ref);
    FengSemanticSubjectKey owned_subject_key;
    if (owned_type_ref == NULL) {
        return false;
    }
    if (!feng_semantic_subject_key_copy_for_analysis(
            analysis, src_subject_key, &owned_subject_key)) {
        return false;
    }
    FengSpecCoercionSite *slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    reset_site_payload(slot);
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_OBJECT;
    slot->src_subject_key = owned_subject_key;
    slot->target_spec_decl = target_spec_decl;
    slot->target_spec_type_ref = owned_type_ref;
    slot->relation = relation;
    slot->object_subject_storage = object_subject_storage;
    slot->callable_source = FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER; /* unused */
    return true;
}

bool feng_semantic_record_object_spec_upcast_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *target_spec_decl,
        const FengTypeRef *target_spec_type_ref,
        const size_t *parent_indices,
        size_t parent_index_count) {
    FengSemanticAnalysis *analysis;
    const FengTypeRef *owned_target_ref;
    size_t *owned_indices;
    FengSpecCoercionSite *slot;

    if (analysis_const == NULL || expr == NULL || target_spec_decl == NULL ||
        target_spec_type_ref == NULL || parent_indices == NULL ||
        parent_index_count == 0U) {
        return false;
    }
    analysis = (FengSemanticAnalysis *)analysis_const;
    owned_target_ref = analysis_clone_type_ref(analysis, target_spec_type_ref);
    if (owned_target_ref == NULL) {
        return false;
    }
    owned_indices = (size_t *)malloc(parent_index_count * sizeof(*owned_indices));
    if (owned_indices == NULL) {
        return false;
    }
    memcpy(owned_indices,
           parent_indices,
           parent_index_count * sizeof(*owned_indices));
    slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        free(owned_indices);
        return false;
    }
    reset_site_payload(slot);
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_OBJECT_UPCAST;
    slot->target_spec_decl = target_spec_decl;
    slot->target_spec_type_ref = owned_target_ref;
    slot->object_upcast_parent_indices = owned_indices;
    slot->object_upcast_parent_index_count = parent_index_count;
    slot->object_subject_storage = FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER;
    slot->callable_source = FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER;
    return true;
}

bool feng_semantic_record_callable_spec_coercion_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *target_spec_decl,
        const FengTypeRef *target_spec_type_ref,
    FengSpecCoercionCallableSource callable_source,
    const FengDecl *callable_decl,
    const FengTypeMember *callable_member,
    const FengDecl *callable_owner_type_decl,
    const FengDecl *callable_fit_decl,
    const FengTypeRef *callable_receiver_type_ref,
    const FengTypeRef *const *callable_type_args,
    size_t callable_type_arg_count,
    const FengExpr *callable_lambda_expr) {
    if (analysis_const == NULL || expr == NULL || target_spec_decl == NULL ||
        target_spec_type_ref == NULL) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    const FengTypeRef *owned_type_ref = analysis_clone_type_ref(analysis, target_spec_type_ref);
    const FengTypeRef *owned_receiver_type_ref =
        callable_receiver_type_ref != NULL
            ? analysis_clone_type_ref(analysis, callable_receiver_type_ref)
            : NULL;
    const FengTypeRef **owned_callable_type_args = NULL;
    if (owned_type_ref == NULL) {
        return false;
    }
    if (callable_receiver_type_ref != NULL &&
        owned_receiver_type_ref == NULL) {
        return false;
    }
    if (callable_type_arg_count > 0U) {
        if (callable_type_args == NULL) {
            return false;
        }
        owned_callable_type_args = (const FengTypeRef **)calloc(
            callable_type_arg_count, sizeof(*owned_callable_type_args));
        if (owned_callable_type_args == NULL) {
            return false;
        }
        for (size_t index = 0U;
             index < callable_type_arg_count;
             ++index) {
            owned_callable_type_args[index] =
                analysis_clone_type_ref(analysis, callable_type_args[index]);
            if (owned_callable_type_args[index] == NULL) {
                free(owned_callable_type_args);
                return false;
            }
        }
    }
    FengSpecCoercionSite *slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        free(owned_callable_type_args);
        return false;
    }
    reset_site_payload(slot);
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_CALLABLE;
    memset(&slot->src_subject_key, 0, sizeof(slot->src_subject_key)); /* INVALID — unused for CALLABLE */
    slot->target_spec_decl = target_spec_decl;
    slot->target_spec_type_ref = owned_type_ref;
    slot->relation = NULL;
    slot->object_subject_storage = FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER; /* unused */
    slot->callable_source = callable_source;
    slot->callable_decl = callable_decl;
    slot->callable_member = callable_member;
    slot->callable_owner_type_decl = callable_owner_type_decl;
    slot->callable_fit_decl = callable_fit_decl;
    slot->callable_receiver_type_ref = owned_receiver_type_ref;
    slot->callable_type_args = owned_callable_type_args;
    slot->callable_type_arg_count = callable_type_arg_count;
    slot->callable_lambda_expr = callable_lambda_expr;
    return true;
}

bool feng_semantic_record_intersection_spec_coercion_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengSemanticSubjectKey *src_subject_key,
        const FengDecl *target_spec_decl,
        const FengTypeRef *target_spec_type_ref,
    FengSpecObjectSubjectStorageKind object_subject_storage) {
    if (analysis_const == NULL || expr == NULL || src_subject_key == NULL ||
        src_subject_key->kind == FENG_SEMANTIC_SUBJECT_KEY_INVALID ||
        target_spec_decl == NULL || target_spec_type_ref == NULL) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    const FengTypeRef *owned_type_ref = analysis_clone_type_ref(analysis, target_spec_type_ref);
    FengSemanticSubjectKey owned_subject_key;
    if (owned_type_ref == NULL) {
        return false;
    }
    if (!feng_semantic_subject_key_copy_for_analysis(
            analysis, src_subject_key, &owned_subject_key)) {
        return false;
    }
    FengSpecCoercionSite *slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    reset_site_payload(slot);
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_INTERSECTION;
    slot->src_subject_key = owned_subject_key;
    slot->target_spec_decl = target_spec_decl;
    slot->target_spec_type_ref = owned_type_ref;
    slot->relation = NULL;
    slot->object_subject_storage = object_subject_storage;
    slot->callable_source = FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER; /* unused */
    return true;
}

bool feng_semantic_record_abi_function_pointer_site(
        const FengSemanticAnalysis *analysis_const,
        const FengExpr *expr,
        const FengDecl *target_spec_decl,
        const FengTypeRef *target_spec_type_ref,
        const FengDecl *callable_decl) {
    if (analysis_const == NULL || expr == NULL || target_spec_decl == NULL ||
        target_spec_type_ref == NULL || callable_decl == NULL) {
        return false;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    const FengTypeRef *owned_type_ref = analysis_clone_type_ref(analysis, target_spec_type_ref);
    if (owned_type_ref == NULL) {
        return false;
    }
    FengSpecCoercionSite *slot = reserve_site_slot(analysis, expr);
    if (slot == NULL) {
        return false;
    }
    reset_site_payload(slot);
    slot->expr = expr;
    slot->form = FENG_SPEC_COERCION_FORM_ABI_FUNCTION_POINTER;
    memset(&slot->src_subject_key, 0, sizeof(slot->src_subject_key));
    slot->target_spec_decl = target_spec_decl;
    slot->target_spec_type_ref = owned_type_ref;
    slot->relation = NULL;
    slot->object_subject_storage = FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER;
    slot->callable_source = FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN;
    slot->callable_decl = callable_decl;
    slot->callable_member = NULL;
    slot->callable_owner_type_decl = NULL;
    slot->callable_fit_decl = NULL;
    slot->callable_lambda_expr = NULL;
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
