/* Value-kind classification (dev/feng-value-model-delivered.md §6.1).
 *
 * Single source of truth for "given a Feng type, which of the three
 * runtime value categories does it belong to". The classification rule is
 * mechanical: it is a function of the AST decl kind plus, for built-ins,
 * the canonical built-in name. This file deliberately keeps that mapping
 * tiny and side-effect-free so that codegen / tooling / future spec-aware
 * passes share a single definition.
 *
 * Per the design doc:
 *   - Built-in primitives are TRIVIAL with the single exception of
 *     `string`, which is a MANAGED_POINTER (FengString *).
 *   - User `type` decls are MANAGED_POINTER (heap object + header).
 *   - Object-form `spec` decls are AGGREGATE (fat value, subject +
 *     witness).
 *   - Callable-form `spec` decls are MANAGED_POINTER (closure pointer).
 *
 * The TRIVIAL fallback used for unrecognised built-in names is the
 * defensive choice: the analyzer rejects unknown spellings before this
 * function is reached, but treating an unknown name as TRIVIAL ensures
 * the function never falsely promotes a foreign type into the ARC path.
 */

#include "semantic.h"

#include <string.h>

static bool slice_eq_cstr(FengSlice slice, const char *literal) {
    size_t literal_len = strlen(literal);
    if (slice.length != literal_len) {
        return false;
    }
    if (literal_len == 0U) {
        return true;
    }
    return memcmp(slice.data, literal, literal_len) == 0;
}

FengSemanticValueKind feng_semantic_value_kind_of_builtin(FengSlice name) {
    /* Only `string` is heap-allocated among built-ins. The remaining
     * built-in spellings (numerics, bool, void) all map to plain C scalars
     * with no managed lifetime. Aliases are checked alongside their
     * canonical forms because callers may pass either spelling unchanged
     * from the AST. */
    if (slice_eq_cstr(name, "string")) {
        return FENG_SEMANTIC_VALUE_MANAGED_POINTER;
    }
    return FENG_SEMANTIC_VALUE_TRIVIAL;
}

FengSemanticValueKind feng_semantic_value_kind_of_decl(const FengDecl *decl) {
    if (decl == NULL) {
        return FENG_SEMANTIC_VALUE_TRIVIAL;
    }
    switch (decl->kind) {
        case FENG_DECL_TYPE:
            /* Concrete user types live on the heap as
             * `FengManagedHeader` + body; values of these types are
             * passed around as a single managed pointer. */
            return FENG_SEMANTIC_VALUE_MANAGED_POINTER;
        case FENG_DECL_SPEC:
            switch (decl->as.spec_decl.form) {
                case FENG_SPEC_FORM_OBJECT:
                    /* Fat object-form spec value: { subject, witness }
                     * passed by value. The subject slot is the only
                     * managed pointer; see §3 / §8 of the design doc. */
                    return FENG_SEMANTIC_VALUE_AGGREGATE;
                case FENG_SPEC_FORM_CALLABLE:
                    /* Callable-form specs lower to a closure pointer; no
                     * fat value, no aggregate handling. */
                    return FENG_SEMANTIC_VALUE_MANAGED_POINTER;
            }
            return FENG_SEMANTIC_VALUE_TRIVIAL;
        default:
            /* Bindings / fits / functions are not types and have no
             * value-kind. Returning TRIVIAL makes accidental misuse
             * detectable at the call site without crashing the compiler. */
            return FENG_SEMANTIC_VALUE_TRIVIAL;
    }
}
