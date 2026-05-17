/* Phase S3 — SpecWitness sidecar.
 *
 * Stores one entry per (subject_key, spec_decl) pair that the analyzer has
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

static bool slice_equals_local(FengSlice left, FengSlice right) {
    return left.length == right.length &&
           (left.length == 0U || memcmp(left.data, right.data, left.length) == 0);
}

static bool slice_equals_cstr_local(FengSlice slice, const char *text) {
    size_t text_len = strlen(text);

    return slice.length == text_len &&
           (text_len == 0U || memcmp(slice.data, text, text_len) == 0);
}

static const char *canonical_builtin_type_name_local(FengSlice name) {
    if (slice_equals_cstr_local(name, "int") || slice_equals_cstr_local(name, "i32")) {
        return "i32";
    }
    if (slice_equals_cstr_local(name, "long") || slice_equals_cstr_local(name, "i64")) {
        return "i64";
    }
    if (slice_equals_cstr_local(name, "byte") || slice_equals_cstr_local(name, "u8")) {
        return "u8";
    }
    if (slice_equals_cstr_local(name, "float") || slice_equals_cstr_local(name, "f32")) {
        return "f32";
    }
    if (slice_equals_cstr_local(name, "double") || slice_equals_cstr_local(name, "f64")) {
        return "f64";
    }
    if (slice_equals_cstr_local(name, "i8")) {
        return "i8";
    }
    if (slice_equals_cstr_local(name, "i16")) {
        return "i16";
    }
    if (slice_equals_cstr_local(name, "u16")) {
        return "u16";
    }
    if (slice_equals_cstr_local(name, "u32")) {
        return "u32";
    }
    if (slice_equals_cstr_local(name, "u64")) {
        return "u64";
    }
    if (slice_equals_cstr_local(name, "bool")) {
        return "bool";
    }
    if (slice_equals_cstr_local(name, "string")) {
        return "string";
    }
    if (slice_equals_cstr_local(name, "void")) {
        return "void";
    }
    return NULL;
}

static const char *builtin_canonical_name_for_type_ref(const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count != 1U || type_ref->as.named.type_arg_count != 0U) {
        return NULL;
    }
    return canonical_builtin_type_name_local(type_ref->as.named.segments[0]);
}

static bool type_ref_subject_key_equal(const FengTypeRef *left,
                                       const FengTypeRef *right) {
    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
        case FENG_TYPE_REF_NAMED: {
            const char *left_builtin = builtin_canonical_name_for_type_ref(left);
            const char *right_builtin = builtin_canonical_name_for_type_ref(right);

            if (left_builtin != NULL || right_builtin != NULL) {
                return left_builtin != NULL && right_builtin != NULL &&
                       strcmp(left_builtin, right_builtin) == 0;
            }
            if (left->as.named.segment_count != right->as.named.segment_count ||
                left->as.named.type_arg_count != right->as.named.type_arg_count) {
                return false;
            }
            for (size_t i = 0U; i < left->as.named.segment_count; ++i) {
                if (!slice_equals_local(left->as.named.segments[i],
                                        right->as.named.segments[i])) {
                    return false;
                }
            }
            for (size_t i = 0U; i < left->as.named.type_arg_count; ++i) {
                if (!type_ref_subject_key_equal(left->as.named.type_args[i],
                                                right->as.named.type_args[i])) {
                    return false;
                }
            }
            return true;
        }
        case FENG_TYPE_REF_POINTER:
            return type_ref_subject_key_equal(left->as.inner, right->as.inner);
        case FENG_TYPE_REF_ARRAY:
            return left->array_element_writable == right->array_element_writable &&
                   type_ref_subject_key_equal(left->as.inner, right->as.inner);
        default:
            return false;
    }
}

static bool subject_key_equals(const FengSemanticSubjectKey *left,
                               const FengSemanticSubjectKey *right) {
    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
        case FENG_SEMANTIC_SUBJECT_KEY_INVALID:
            return true;
        case FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL:
            return left->as.type_decl == right->as.type_decl;
        case FENG_SEMANTIC_SUBJECT_KEY_BUILTIN:
            return left->as.builtin_canonical_name != NULL &&
                   right->as.builtin_canonical_name != NULL &&
                   strcmp(left->as.builtin_canonical_name,
                          right->as.builtin_canonical_name) == 0;
        case FENG_SEMANTIC_SUBJECT_KEY_ARRAY:
            return left->as.array.rank == right->as.array.rank &&
                   left->as.array.writable_mask == right->as.array.writable_mask &&
                   type_ref_subject_key_equal(left->as.array.element_type_ref,
                                              right->as.array.element_type_ref);
        default:
            return false;
    }
}

FengSemanticSubjectKey feng_semantic_subject_key_for_type_decl(
        const FengDecl *type_decl) {
    FengSemanticSubjectKey key;

    memset(&key, 0, sizeof(key));
    if (type_decl != NULL &&
        (type_decl->kind == FENG_DECL_TYPE || type_decl->kind == FENG_DECL_ENUM)) {
        key.kind = FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL;
        key.as.type_decl = type_decl;
    }
    return key;
}

FengSemanticSubjectKey feng_semantic_subject_key_for_builtin(
        const char *builtin_canonical_name) {
    FengSemanticSubjectKey key;

    memset(&key, 0, sizeof(key));
    if (builtin_canonical_name != NULL) {
        key.kind = FENG_SEMANTIC_SUBJECT_KEY_BUILTIN;
        key.as.builtin_canonical_name = builtin_canonical_name;
    }
    return key;
}

bool feng_semantic_subject_key_init_array_from_type_ref(
        FengSemanticSubjectKey *out_key,
        const FengTypeRef *type_ref) {
    const FengTypeRef *element_type_ref = type_ref;
    size_t rank = 0U;
    uint64_t writable_mask = 0U;

    if (out_key == NULL || type_ref == NULL) {
        return false;
    }
    memset(out_key, 0, sizeof(*out_key));
    while (element_type_ref != NULL && element_type_ref->kind == FENG_TYPE_REF_ARRAY) {
        if (rank >= 64U) {
            return false;
        }
        if (element_type_ref->array_element_writable) {
            writable_mask |= (uint64_t)1U << rank;
        }
        ++rank;
        element_type_ref = element_type_ref->as.inner;
    }
    if (rank == 0U || element_type_ref == NULL) {
        return false;
    }
    out_key->kind = FENG_SEMANTIC_SUBJECT_KEY_ARRAY;
    out_key->as.array.element_type_ref = element_type_ref;
    out_key->as.array.rank = rank;
    out_key->as.array.writable_mask = writable_mask;
    return true;
}

static FengSpecWitness *find_entry_mut(FengSemanticAnalysis *analysis,
                                       const FengSemanticSubjectKey *subject_key,
                                       const FengDecl *spec_decl) {
    if (analysis == NULL || subject_key == NULL ||
        subject_key->kind == FENG_SEMANTIC_SUBJECT_KEY_INVALID ||
        spec_decl == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < analysis->spec_witness_count; ++i) {
        FengSpecWitness *entry = &analysis->spec_witnesses[i];
        if (subject_key_equals(&entry->subject_key, subject_key) &&
            entry->spec_decl == spec_decl) {
            return entry;
        }
    }
    return NULL;
}

const FengSpecWitness *feng_semantic_lookup_spec_witness(
        const FengSemanticAnalysis *analysis_const,
        const FengSemanticSubjectKey *subject_key,
        const FengDecl *spec_decl) {
    if (analysis_const == NULL) {
        return NULL;
    }
    return find_entry_mut((FengSemanticAnalysis *)analysis_const,
                          subject_key, spec_decl);
}

FengSpecWitness *feng_semantic_reserve_spec_witness(
        const FengSemanticAnalysis *analysis_const,
        const FengSemanticSubjectKey *subject_key,
        const FengDecl *spec_decl) {
    if (analysis_const == NULL || subject_key == NULL || spec_decl == NULL) {
        return NULL;
    }
    if (subject_key->kind == FENG_SEMANTIC_SUBJECT_KEY_INVALID ||
        spec_decl->kind != FENG_DECL_SPEC) {
        return NULL;
    }
    FengSemanticAnalysis *analysis = (FengSemanticAnalysis *)analysis_const;
    if (find_entry_mut(analysis, subject_key, spec_decl) != NULL) {
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
    slot->subject_key = *subject_key;
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
