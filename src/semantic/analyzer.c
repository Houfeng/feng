#include "semantic/semantic.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Returns true when `decl` denotes a callable-form spec — i.e. a function-shaped
 * contract such as `spec Name(...): T;`. Callable-form spec satisfaction is
 * structural: any function whose parameter and return shape matches the spec
 * is accepted, no declaration of intent required. Object-form spec satisfaction
 * is nominal and handled separately by type_decl_satisfies_spec_decl. */
static inline bool decl_is_function_type(const FengDecl *decl) {
    return decl != NULL && decl->kind == FENG_DECL_SPEC &&
           decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE;
}

static inline FengSlice decl_typeish_name(const FengDecl *decl) {
    if (decl->kind == FENG_DECL_SPEC) {
        return decl->as.spec_decl.name;
    }
    return decl->as.type_decl.name;
}

typedef struct SymbolEntry {
    FengSlice name;
    const FengDecl *decl;
} SymbolEntry;

typedef struct FunctionOverloadSetEntry {
    FengSlice name;
    const FengSemanticModule *provider_module;
    const FengDecl **decls;
    size_t decl_count;
    size_t decl_capacity;
} FunctionOverloadSetEntry;

typedef struct VisibleTypeEntry {
    FengSlice name;
    const FengSemanticModule *provider_module;
    const FengDecl *decl;
} VisibleTypeEntry;

typedef struct VisibleValueEntry {
    FengSlice name;
    const FengSemanticModule *provider_module;
    const FengDecl *decl;
    FengMutability mutability;
    bool is_function;
} VisibleValueEntry;

typedef struct AliasEntry {
    FengSlice alias;
    const FengSemanticModule *target_module;
    const FengUseDecl *use_decl;
} AliasEntry;

/* Per-program record of every module referenced by a `use` declaration in
 * the current file (whether short-name or aliased). Cross-module visibility
 * checks against external modules require an entry here in addition to the
 * target being declared `pu mod`. */
typedef struct ImportedModuleEntry {
    const FengSemanticModule *target_module;
} ImportedModuleEntry;

typedef enum InferredExprTypeKind {
    FENG_INFERRED_EXPR_TYPE_UNKNOWN = 0,
    FENG_INFERRED_EXPR_TYPE_BUILTIN,
    FENG_INFERRED_EXPR_TYPE_TYPE_REF,
    FENG_INFERRED_EXPR_TYPE_DECL,
    FENG_INFERRED_EXPR_TYPE_LAMBDA
} InferredExprTypeKind;

typedef struct InferredExprType {
    InferredExprTypeKind kind;
    FengSlice builtin_name;
    const FengTypeRef *type_ref;
    const FengDecl *type_decl;
    const FengExpr *lambda_expr;
} InferredExprType;

typedef struct LocalNameEntry {
    FengSlice name;
    InferredExprType type;
    FengMutability mutability;
    const FengExpr *source_expr;
} LocalNameEntry;

/* Compile-time constant evaluation result. Used by evaluate_constant_expr to model the
 * limited set of values producible by Feng's constant-folder. INT carries arbitrary i64;
 * FLOAT carries IEEE 754 double; BOOL carries a flag. */
typedef enum FengConstKind {
    FENG_CONST_NONE = 0,
    FENG_CONST_INT,
    FENG_CONST_FLOAT,
    FENG_CONST_BOOL
} FengConstKind;

typedef struct FengConstValue {
    FengConstKind kind;
    int64_t i;
    double f;
    bool b;
} FengConstValue;

/* Linked-list guard threaded through evaluate_constant_expr_inner to detect identifier
 * cycles such as `let a = b; let b = a;` without resorting to global state. */
typedef struct ConstEvalGuard {
    const FengExpr *expr;
    struct ConstEvalGuard *prev;
} ConstEvalGuard;

typedef struct ScopeFrame {
    LocalNameEntry *locals;
    size_t local_count;
    size_t local_capacity;
} ScopeFrame;

typedef enum ConstructorResolutionKind {
    FENG_CONSTRUCTOR_RESOLUTION_NONE = 0,
    FENG_CONSTRUCTOR_RESOLUTION_UNIQUE,
    FENG_CONSTRUCTOR_RESOLUTION_AMBIGUOUS
} ConstructorResolutionKind;

typedef struct ConstructorResolution {
    ConstructorResolutionKind kind;
    const FengTypeMember *constructor;
} ConstructorResolution;

typedef enum FunctionCallResolutionKind {
    FENG_FUNCTION_CALL_RESOLUTION_NONE = 0,
    FENG_FUNCTION_CALL_RESOLUTION_UNIQUE,
    FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS
} FunctionCallResolutionKind;

typedef struct FunctionCallResolution {
    FunctionCallResolutionKind kind;
    const FengDecl *decl;
    const FengCallableSignature *callable;
    const FengTypeMember *member;       /* set for type-method / fit-method */
    const FengDecl *owner_type_decl;    /* set for type-method / fit-method */
    const FengDecl *fit_decl;           /* set for fit-method */
} FunctionCallResolution;

typedef enum CallableValueResolutionKind {
    FENG_CALLABLE_VALUE_RESOLUTION_NONE = 0,
    FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE,
    FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS
} CallableValueResolutionKind;

typedef struct CallableValueResolution {
    CallableValueResolutionKind kind;
    const FengCallableSignature *callable;
    const FengExpr *lambda_expr;
} CallableValueResolution;

typedef struct CallableReturnCacheEntry {
    const FengCallableSignature *callable;
    InferredExprType return_type;
} CallableReturnCacheEntry;

typedef struct CallableReturnCache {
    CallableReturnCacheEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
    bool changed;
} CallableReturnCache;

typedef struct CallableExceptionEscapeCacheEntry {
    const FengCallableSignature *callable;
    bool escapes;
} CallableExceptionEscapeCacheEntry;

typedef struct CallableExceptionEscapeCache {
    CallableExceptionEscapeCacheEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
    bool changed;
} CallableExceptionEscapeCache;

typedef struct ResolveContext {
    const FengSemanticAnalysis *analysis;
    const FengSemanticModule *module;
    const FengProgram *program;
    const VisibleTypeEntry *visible_types;
    size_t visible_type_count;
    const VisibleValueEntry *visible_values;
    size_t visible_value_count;
    const FunctionOverloadSetEntry *function_sets;
    size_t function_set_count;
    const AliasEntry *aliases;
    size_t alias_count;
    /* Modules the current file imported via `use` (short-name or aliased).
     * Used to enforce that cross-module symbol/contract visibility requires
     * an explicit `use`, not just the target being `pu mod`. */
    const ImportedModuleEntry *imported_modules;
    size_t imported_module_count;
    ScopeFrame *scopes;
    size_t scope_count;
    size_t scope_capacity;
    const FengDecl *current_type_decl;
    /* When non-NULL, the resolver is currently inside the body of a fit-block
     * function. `current_type_decl` is set to the fit's resolved target type so
     * that `self`/instance lookups still work, and `current_fit_decl` is used
     * to enforce that fit-body code cannot reach the target type's private
     * members regardless of whether the target lives in the same package. */
    const FengDecl *current_fit_decl;
    const FengTypeMember *current_callable_member;
    const FengCallableSignature *current_callable_signature;
    InferredExprType current_callable_inferred_return_type;
    bool current_callable_saw_return;
    FengSlice *current_constructor_bound_names;
    size_t current_constructor_bound_count;
    size_t current_constructor_bound_capacity;
    FengTypeRef **synthetic_type_refs;
    size_t synthetic_type_ref_count;
    size_t synthetic_type_ref_capacity;
    CallableReturnCache *callable_return_cache;
    CallableExceptionEscapeCache *callable_exception_escape_cache;
    FengSemanticError **errors;
    size_t *error_count;
    size_t *error_capacity;
    bool current_callable_has_escaping_exception;
    size_t exception_capture_depth;
    size_t finally_depth;
    /* Number of nested `while`/`for` loop bodies currently being resolved
     * inside the active callable scope. Used to enforce that `break` and
     * `continue` can only appear inside a loop body. Reset to 0 across
     * callable / lambda boundaries so that a lambda nested in a loop cannot
     * jump out of the surrounding loop. */
    size_t loop_depth;
    /* When true, a lambda body resolved in this context may capture `self`
     * from the enclosing type. Set inside type method/constructor bodies and
     * inside callable-spec field initializers. */
    bool self_capturable;
    /* Stack of lambda capture frames, pushed by resolve_lambda_expr while a
     * lambda body is being resolved. Used to record captured outer locals and
     * self references onto FengExpr.lambda.captures. */
    struct LambdaCaptureFrame *lambda_frames;
    size_t lambda_frame_count;
    size_t lambda_frame_capacity;
} ResolveContext;

static bool resolver_append_error(ResolveContext *context, FengToken token, char *message);

typedef struct ResolvedTypeTarget {
    const FengDecl *type_decl;
    const FengSemanticModule *provider_module;
} ResolvedTypeTarget;

typedef struct FixedAbiTrace {
    const FengDecl *decl;
    const struct FixedAbiTrace *parent;
} FixedAbiTrace;

static FengSlice slice_from_cstr(const char *text) {
    FengSlice slice;

    slice.data = text;
    slice.length = strlen(text);
    return slice;
}

static bool slice_equals(FengSlice left, FengSlice right) {
    return left.length == right.length && memcmp(left.data, right.data, left.length) == 0;
}


static bool slice_equals_cstr(FengSlice slice, const char *text) {
    size_t length = strlen(text);

    return slice.length == length && memcmp(slice.data, text, length) == 0;
}

static bool path_equals(const FengSlice *left,
                        size_t left_count,
                        const FengSlice *right,
                        size_t right_count) {
    size_t index;

    if (left_count != right_count) {
        return false;
    }

    for (index = 0U; index < left_count; ++index) {
        if (!slice_equals(left[index], right[index])) {
            return false;
        }
    }

    return true;
}

static char *format_module_name(const FengSlice *segments, size_t segment_count);
static bool type_ref_is_void(const FengTypeRef *type_ref);
static bool append_raw(void **items,
                       size_t *count,
                       size_t *capacity,
                       size_t item_size,
                       const void *value);
static bool inferred_expr_types_equal(const ResolveContext *context,
                                      InferredExprType left,
                                      InferredExprType right);

static InferredExprType inferred_expr_type_unknown(void) {
    InferredExprType type;

    memset(&type, 0, sizeof(type));
    return type;
}

static InferredExprType inferred_expr_type_builtin(const char *name) {
    InferredExprType type = inferred_expr_type_unknown();

    type.kind = FENG_INFERRED_EXPR_TYPE_BUILTIN;
    type.builtin_name = slice_from_cstr(name);
    return type;
}

static InferredExprType inferred_expr_type_from_type_ref(const FengTypeRef *type_ref) {
    InferredExprType type = inferred_expr_type_unknown();

    type.kind = type_ref != NULL ? FENG_INFERRED_EXPR_TYPE_TYPE_REF : FENG_INFERRED_EXPR_TYPE_UNKNOWN;
    type.type_ref = type_ref;
    return type;
}

static InferredExprType inferred_expr_type_from_decl(const FengDecl *type_decl) {
    InferredExprType type = inferred_expr_type_unknown();

    type.kind = type_decl != NULL ? FENG_INFERRED_EXPR_TYPE_DECL : FENG_INFERRED_EXPR_TYPE_UNKNOWN;
    type.type_decl = type_decl;
    return type;
}

static InferredExprType inferred_expr_type_from_lambda(const FengExpr *lambda_expr) {
    InferredExprType type = inferred_expr_type_unknown();

    type.kind = lambda_expr != NULL ? FENG_INFERRED_EXPR_TYPE_LAMBDA
                                    : FENG_INFERRED_EXPR_TYPE_UNKNOWN;
    type.lambda_expr = lambda_expr;
    return type;
}

static InferredExprType inferred_expr_type_from_return_type_ref(const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref_is_void(type_ref)) {
        return inferred_expr_type_builtin("void");
    }

    return inferred_expr_type_from_type_ref(type_ref);
}

static void free_synthetic_type_ref(FengTypeRef *type_ref) {
    if (type_ref == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            free(type_ref->as.named.segments);
            break;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            free_synthetic_type_ref(type_ref->as.inner);
            break;
    }

    free(type_ref);
}

static bool inferred_expr_type_is_known(InferredExprType type) {
    return type.kind != FENG_INFERRED_EXPR_TYPE_UNKNOWN;
}

static const CallableReturnCacheEntry *find_callable_return_cache_entry(
    const CallableReturnCache *cache,
    const FengCallableSignature *callable) {
    size_t index;

    if (cache == NULL || callable == NULL) {
        return NULL;
    }

    for (index = 0U; index < cache->entry_count; ++index) {
        if (cache->entries[index].callable == callable) {
            return &cache->entries[index];
        }
    }

    return NULL;
}

static const CallableExceptionEscapeCacheEntry *find_callable_exception_escape_cache_entry(
    const CallableExceptionEscapeCache *cache,
    const FengCallableSignature *callable) {
    size_t index;

    if (cache == NULL || callable == NULL) {
        return NULL;
    }

    for (index = 0U; index < cache->entry_count; ++index) {
        if (cache->entries[index].callable == callable) {
            return &cache->entries[index];
        }
    }

    return NULL;
}

static CallableReturnCacheEntry *find_mutable_callable_return_cache_entry(
    CallableReturnCache *cache,
    const FengCallableSignature *callable) {
    size_t index;

    if (cache == NULL || callable == NULL) {
        return NULL;
    }

    for (index = 0U; index < cache->entry_count; ++index) {
        if (cache->entries[index].callable == callable) {
            return &cache->entries[index];
        }
    }

    return NULL;
}

static CallableExceptionEscapeCacheEntry *find_mutable_callable_exception_escape_cache_entry(
    CallableExceptionEscapeCache *cache,
    const FengCallableSignature *callable) {
    size_t index;

    if (cache == NULL || callable == NULL) {
        return NULL;
    }

    for (index = 0U; index < cache->entry_count; ++index) {
        if (cache->entries[index].callable == callable) {
            return &cache->entries[index];
        }
    }

    return NULL;
}

static InferredExprType callable_effective_return_type(const ResolveContext *context,
                                                       const FengCallableSignature *callable) {
    const CallableReturnCacheEntry *entry;

    if (callable == NULL) {
        return inferred_expr_type_unknown();
    }
    if (callable->return_type != NULL) {
        return inferred_expr_type_from_return_type_ref(callable->return_type);
    }

    entry = context != NULL
                ? find_callable_return_cache_entry(context->callable_return_cache, callable)
                : NULL;
    return entry != NULL ? entry->return_type : inferred_expr_type_unknown();
}

static bool cache_callable_return_type(ResolveContext *context,
                                       const FengCallableSignature *callable,
                                       InferredExprType return_type) {
    CallableReturnCache *cache;
    CallableReturnCacheEntry *entry;
    CallableReturnCacheEntry new_entry;

    if (context == NULL || callable == NULL || !inferred_expr_type_is_known(return_type)) {
        return true;
    }

    cache = context->callable_return_cache;
    if (cache == NULL) {
        return true;
    }

    entry = find_mutable_callable_return_cache_entry(cache, callable);
    if (entry != NULL) {
        if (inferred_expr_types_equal(context, entry->return_type, return_type)) {
            return true;
        }
        entry->return_type = return_type;
        cache->changed = true;
        return true;
    }

    new_entry.callable = callable;
    new_entry.return_type = return_type;
    if (!append_raw((void **)&cache->entries,
                    &cache->entry_count,
                    &cache->entry_capacity,
                    sizeof(new_entry),
                    &new_entry)) {
        return false;
    }

    cache->changed = true;
    return true;
}

static bool callable_may_escape_exception(const ResolveContext *context,
                                          const FengCallableSignature *callable) {
    const CallableExceptionEscapeCacheEntry *entry;

    if (callable == NULL || callable->body == NULL) {
        return false;
    }

    entry = context != NULL
                ? find_callable_exception_escape_cache_entry(
                      context->callable_exception_escape_cache, callable)
                : NULL;
    return entry != NULL && entry->escapes;
}

static bool cache_callable_exception_escape(ResolveContext *context,
                                            const FengCallableSignature *callable,
                                            bool escapes) {
    CallableExceptionEscapeCache *cache;
    CallableExceptionEscapeCacheEntry *entry;
    CallableExceptionEscapeCacheEntry new_entry;

    if (context == NULL || callable == NULL || callable->body == NULL) {
        return true;
    }

    cache = context->callable_exception_escape_cache;
    if (cache == NULL) {
        return true;
    }

    entry = find_mutable_callable_exception_escape_cache_entry(cache, callable);
    if (entry != NULL) {
        if (entry->escapes == escapes) {
            return true;
        }
        entry->escapes = escapes;
        cache->changed = true;
        return true;
    }

    new_entry.callable = callable;
    new_entry.escapes = escapes;
    if (!append_raw((void **)&cache->entries,
                    &cache->entry_count,
                    &cache->entry_capacity,
                    sizeof(new_entry),
                    &new_entry)) {
        return false;
    }

    cache->changed = true;
    return true;
}

static void free_callable_return_cache(CallableReturnCache *cache) {
    if (cache == NULL) {
        return;
    }

    free(cache->entries);
    cache->entries = NULL;
    cache->entry_count = 0U;
    cache->entry_capacity = 0U;
    cache->changed = false;
}

static void free_callable_exception_escape_cache(CallableExceptionEscapeCache *cache) {
    if (cache == NULL) {
        return;
    }

    free(cache->entries);
    cache->entries = NULL;
    cache->entry_count = 0U;
    cache->entry_capacity = 0U;
    cache->changed = false;
}

static bool append_raw(void **items,
                       size_t *count,
                       size_t *capacity,
                       size_t item_size,
                       const void *value) {
    void *new_items;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0U) ? 4U : (*capacity * 2U);

        new_items = realloc(*items, new_capacity * item_size);
        if (new_items == NULL) {
            return false;
        }

        *items = new_items;
        *capacity = new_capacity;
    }

    memcpy((char *)(*items) + (*count * item_size), value, item_size);
    ++(*count);
    return true;
}

static char *duplicate_cstr(const char *text) {
    size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length);
    return copy;
}

static char *format_type_ref_name(const FengTypeRef *type_ref) {
    char *inner_name;
    size_t inner_length;
    char *buffer;

    if (type_ref == NULL) {
        return duplicate_cstr("void");
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            return format_module_name(type_ref->as.named.segments,
                                      type_ref->as.named.segment_count);

        case FENG_TYPE_REF_POINTER:
            inner_name = format_type_ref_name(type_ref->as.inner);
            if (inner_name == NULL) {
                return NULL;
            }

            inner_length = strlen(inner_name);
            buffer = (char *)malloc(inner_length + 2U);
            if (buffer == NULL) {
                free(inner_name);
                return NULL;
            }

            memcpy(buffer, inner_name, inner_length);
            buffer[inner_length] = '*';
            buffer[inner_length + 1U] = '\0';
            free(inner_name);
            return buffer;

        case FENG_TYPE_REF_ARRAY:
            inner_name = format_type_ref_name(type_ref->as.inner);
            if (inner_name == NULL) {
                return NULL;
            }

            inner_length = strlen(inner_name);
            buffer = (char *)malloc(inner_length + 4U);
            if (buffer == NULL) {
                free(inner_name);
                return NULL;
            }

            memcpy(buffer, inner_name, inner_length);
            buffer[inner_length] = '[';
            buffer[inner_length + 1U] = ']';
            if (type_ref->array_element_writable) {
                buffer[inner_length + 2U] = '!';
                buffer[inner_length + 3U] = '\0';
            } else {
                buffer[inner_length + 2U] = '\0';
            }
            free(inner_name);
            return buffer;
    }

    return duplicate_cstr("<unknown>");
}

static char *format_message(const char *format, ...) {
    va_list args;
    va_list copy;
    int needed;
    char *buffer;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0) {
        va_end(copy);
        return NULL;
    }

    buffer = (char *)malloc((size_t)needed + 1U);
    if (buffer == NULL) {
        va_end(copy);
        return NULL;
    }

    (void)vsnprintf(buffer, (size_t)needed + 1U, format, copy);
    va_end(copy);
    return buffer;
}

static char *format_module_name(const FengSlice *segments, size_t segment_count) {
    size_t total_length = 0U;
    size_t index;
    char *buffer;
    size_t cursor = 0U;

    for (index = 0U; index < segment_count; ++index) {
        total_length += segments[index].length;
        if (index + 1U < segment_count) {
            ++total_length;
        }
    }

    buffer = (char *)malloc(total_length + 1U);
    if (buffer == NULL) {
        return NULL;
    }

    for (index = 0U; index < segment_count; ++index) {
        memcpy(buffer + cursor, segments[index].data, segments[index].length);
        cursor += segments[index].length;
        if (index + 1U < segment_count) {
            buffer[cursor] = '.';
            ++cursor;
        }
    }

    buffer[cursor] = '\0';
    return buffer;
}

static const FengToken *decl_token(const FengDecl *decl) {
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return &decl->as.binding.token;
        case FENG_DECL_TYPE:
            return &decl->token;
        case FENG_DECL_SPEC:
            return &decl->token;
        case FENG_DECL_FIT:
            return &decl->token;
        case FENG_DECL_FUNCTION:
            return &decl->as.function_decl.token;
    }

    return &decl->token;
}

static bool type_ref_is_void(const FengTypeRef *type_ref) {
    return type_ref != NULL &&
           type_ref->kind == FENG_TYPE_REF_NAMED &&
           type_ref->as.named.segment_count == 1U &&
           slice_equals_cstr(type_ref->as.named.segments[0], "void");
}

static bool type_ref_equals(const FengTypeRef *left, const FengTypeRef *right) {
    size_t index;

    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL) {
        return false;
    }
    if (left->kind != right->kind) {
        return false;
    }

    switch (left->kind) {
        case FENG_TYPE_REF_NAMED:
            if (left->as.named.segment_count != right->as.named.segment_count) {
                return false;
            }
            for (index = 0U; index < left->as.named.segment_count; ++index) {
                if (!slice_equals(left->as.named.segments[index], right->as.named.segments[index])) {
                    return false;
                }
            }
            return true;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            if (left->kind == FENG_TYPE_REF_ARRAY &&
                left->array_element_writable != right->array_element_writable) {
                return false;
            }
            return type_ref_equals(left->as.inner, right->as.inner);
    }

    return false;
}

static bool return_type_equals(const FengTypeRef *left, const FengTypeRef *right) {
    bool left_is_void = (left == NULL) || type_ref_is_void(left);
    bool right_is_void = (right == NULL) || type_ref_is_void(right);

    if (left_is_void || right_is_void) {
        return left_is_void && right_is_void;
    }

    return type_ref_equals(left, right);
}

static bool parameters_equal(const FengCallableSignature *left, const FengCallableSignature *right) {
    size_t index;

    if (left->param_count != right->param_count) {
        return false;
    }

    for (index = 0U; index < left->param_count; ++index) {
        if (!type_ref_equals(left->params[index].type, right->params[index].type)) {
            return false;
        }
    }

    return true;
}

static bool decl_is_public(const FengDecl *decl) {
    return decl->visibility == FENG_VISIBILITY_PUBLIC;
}

static size_t find_module_index_by_path(const FengSemanticAnalysis *analysis,
                                        const FengSlice *segments,
                                        size_t segment_count) {
    size_t index;

    for (index = 0U; index < analysis->module_count; ++index) {
        const FengSemanticModule *module = &analysis->modules[index];

        if (path_equals(module->segments, module->segment_count, segments, segment_count)) {
            return index;
        }
    }

    return analysis->module_count;
}

static size_t find_module_index(const FengSemanticAnalysis *analysis, const FengProgram *program) {
    return find_module_index_by_path(
        analysis, program->module_segments, program->module_segment_count);
}

/* Inject a pre-built external FengSemanticModule into analysis.
 * A fresh programs pointer-array is allocated and owned by analysis so that
 * feng_semantic_analysis_free can call free() on it without touching the
 * caller's allocations.  The FengProgram objects themselves remain owned by
 * the caller (SyntheticCtx in frontend.c). */
static bool add_external_module(FengSemanticAnalysis *analysis,
                                const FengSemanticModule *ext) {
    FengSemanticModule mod;
    size_t i;

    if (ext == NULL) {
        return true;
    }

    memset(&mod, 0, sizeof(mod));
    mod.segments = ext->segments;
    mod.segment_count = ext->segment_count;
    mod.visibility = ext->visibility;
    mod.origin = FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE;
    mod.program_count = ext->program_count;
    mod.program_capacity = ext->program_count;

    if (ext->program_count > 0U) {
        mod.programs = (const FengProgram **)calloc(ext->program_count,
                                                    sizeof(*mod.programs));
        if (mod.programs == NULL) {
            return false;
        }
        for (i = 0U; i < ext->program_count; ++i) {
            mod.programs[i] = ext->programs[i];
        }
    }

    if (!append_raw((void **)&analysis->modules,
                    &analysis->module_count,
                    &analysis->module_capacity,
                    sizeof(mod),
                    &mod)) {
        free(mod.programs);
        return false;
    }

    return true;
}

static const FengSemanticModule *find_decl_provider_module(const FengSemanticAnalysis *analysis,
                                                           const FengDecl *decl) {
    size_t module_index;

    if (analysis == NULL || decl == NULL) {
        return NULL;
    }

    for (module_index = 0U; module_index < analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &analysis->modules[module_index];
        size_t program_index;

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            const FengProgram *program = module->programs[program_index];
            size_t decl_index;

            for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
                if (program->declarations[decl_index] == decl) {
                    return module;
                }
            }
        }
    }

    return NULL;
}

static size_t find_visible_type_index(const VisibleTypeEntry *entries, size_t count, FengSlice name) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (slice_equals(entries[index].name, name)) {
            return index;
        }
    }

    return count;
}

static size_t find_visible_value_index(const VisibleValueEntry *entries, size_t count, FengSlice name) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (slice_equals(entries[index].name, name)) {
            return index;
        }
    }

    return count;
}

static size_t find_slice_index(const FengSlice *items, size_t count, FengSlice name) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (slice_equals(items[index], name)) {
            return index;
        }
    }

    return count;
}

static bool append_slice(FengSlice **items, size_t *count, size_t *capacity, FengSlice value) {
    return append_raw((void **)items, count, capacity, sizeof(value), &value);
}

static bool is_builtin_type_name(FengSlice name) {
    /* Built-in type names per docs/feng-builtin-type.md §2.
     * Aliases are: int ≡ i32, long ≡ i64, byte ≡ u8, float ≡ f32, double ≡ f64.
     * Aliases are first-class spellings — both forms are recognized as built-ins. */
    static const char *builtin_names[] = {
        "i8",   "i16",  "i32",  "i64",
        "u8",   "u16",  "u32",  "u64",
        "f32",  "f64",
        "int",  "long", "byte", "float", "double",
        "bool", "string", "void"};
    size_t index;

    for (index = 0U; index < sizeof(builtin_names) / sizeof(builtin_names[0]); ++index) {
        if (slice_equals_cstr(name, builtin_names[index])) {
            return true;
        }
    }

    return false;
}

static const char *canonical_builtin_type_name(FengSlice name) {
    /* Canonical (width-explicit) names per docs/feng-builtin-type.md §2 alias table.
     * Aliases collapse to their canonical width-explicit spelling for type identity checks. */
    if (slice_equals_cstr(name, "int") || slice_equals_cstr(name, "i32")) {
        return "i32";
    }
    if (slice_equals_cstr(name, "long") || slice_equals_cstr(name, "i64")) {
        return "i64";
    }
    if (slice_equals_cstr(name, "byte") || slice_equals_cstr(name, "u8")) {
        return "u8";
    }
    if (slice_equals_cstr(name, "float") || slice_equals_cstr(name, "f32")) {
        return "f32";
    }
    if (slice_equals_cstr(name, "double") || slice_equals_cstr(name, "f64")) {
        return "f64";
    }
    if (slice_equals_cstr(name, "i8")) {
        return "i8";
    }
    if (slice_equals_cstr(name, "i16")) {
        return "i16";
    }
    if (slice_equals_cstr(name, "u16")) {
        return "u16";
    }
    if (slice_equals_cstr(name, "u32")) {
        return "u32";
    }
    if (slice_equals_cstr(name, "u64")) {
        return "u64";
    }
    if (slice_equals_cstr(name, "bool")) {
        return "bool";
    }
    if (slice_equals_cstr(name, "string")) {
        return "string";
    }
    if (slice_equals_cstr(name, "void")) {
        return "void";
    }

    return NULL;
}

static bool builtin_type_name_is_numeric(FengSlice name) {
    const char *canonical_name = canonical_builtin_type_name(name);

    return canonical_name != NULL && strcmp(canonical_name, "bool") != 0 &&
           strcmp(canonical_name, "string") != 0 && strcmp(canonical_name, "void") != 0;
}

static bool builtin_type_name_is_integer(FengSlice name) {
    const char *canonical_name = canonical_builtin_type_name(name);

    return canonical_name != NULL &&
           (strcmp(canonical_name, "i8") == 0 || strcmp(canonical_name, "i16") == 0 ||
            strcmp(canonical_name, "i32") == 0 || strcmp(canonical_name, "i64") == 0 ||
            strcmp(canonical_name, "u8") == 0 || strcmp(canonical_name, "u16") == 0 ||
            strcmp(canonical_name, "u32") == 0 || strcmp(canonical_name, "u64") == 0);
}

/* Stronger check than the legacy "target is public" predicate: a target
 * module is *use-visible* from the current resolve context only if either
 * (a) it is the same module, or (b) the target is `pu mod` AND the current
 * file imported it via a `use` declaration. Required by docs/feng-module.md
 * to prevent ambient access to any public module without an explicit import. */
static bool module_is_use_visible_from(const ResolveContext *ctx,
                                       const FengSemanticModule *target) {
    size_t i;

    if (ctx == NULL || target == NULL) {
        return false;
    }
    if (ctx->module == target) {
        return true;
    }
    if (target->visibility != FENG_VISIBILITY_PUBLIC) {
        return false;
    }
    for (i = 0U; i < ctx->imported_module_count; ++i) {
        if (ctx->imported_modules[i].target_module == target) {
            return true;
        }
    }
    return false;
}

static const VisibleTypeEntry *find_visible_type(const VisibleTypeEntry *entries,
                                                 size_t count,
                                                 FengSlice name) {
    size_t index = find_visible_type_index(entries, count, name);

    return index < count ? &entries[index] : NULL;
}

static const FengDecl *find_visible_type_decl(const VisibleTypeEntry *entries,
                                              size_t count,
                                              FengSlice name) {
    const VisibleTypeEntry *entry = find_visible_type(entries, count, name);

    return entry != NULL ? entry->decl : NULL;
}

static const VisibleValueEntry *find_visible_value(const VisibleValueEntry *entries,
                                                   size_t count,
                                                   FengSlice name) {
    size_t index = find_visible_value_index(entries, count, name);

    return index < count ? &entries[index] : NULL;
}

static size_t find_function_overload_set_index(const FunctionOverloadSetEntry *entries,
                                               size_t count,
                                               FengSlice name) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (slice_equals(entries[index].name, name)) {
            return index;
        }
    }

    return count;
}

static const FunctionOverloadSetEntry *find_function_overload_set(
    const FunctionOverloadSetEntry *entries,
    size_t count,
    FengSlice name) {
    size_t index = find_function_overload_set_index(entries, count, name);

    return index < count ? &entries[index] : NULL;
}

static bool append_function_overload_decl(FunctionOverloadSetEntry *entry, const FengDecl *decl) {
    size_t index;

    for (index = 0U; index < entry->decl_count; ++index) {
        if (entry->decls[index] == decl) {
            return true;
        }
    }

    return append_raw((void **)&entry->decls,
                      &entry->decl_count,
                      &entry->decl_capacity,
                      sizeof(entry->decls[0]),
                      &decl);
}

static bool append_visible_function_overload(FunctionOverloadSetEntry **entries,
                                             size_t *count,
                                             size_t *capacity,
                                             const FengSemanticModule *provider_module,
                                             const FengDecl *decl) {
    size_t index = find_function_overload_set_index(*entries, *count, decl->as.function_decl.name);

    if (index == *count) {
        FunctionOverloadSetEntry entry;

        memset(&entry, 0, sizeof(entry));
        entry.name = decl->as.function_decl.name;
        entry.provider_module = provider_module;
        if (!append_raw((void **)entries, count, capacity, sizeof(entry), &entry)) {
            return false;
        }
        index = *count - 1U;
    }

    return append_function_overload_decl(&(*entries)[index], decl);
}

static void free_function_overload_sets(FunctionOverloadSetEntry *entries, size_t count) {
    size_t index;

    if (entries == NULL) {
        return;
    }

    for (index = 0U; index < count; ++index) {
        free(entries[index].decls);
    }
    free(entries);
}

static size_t find_alias_index(const AliasEntry *entries, size_t count, FengSlice alias) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (slice_equals(entries[index].alias, alias)) {
            return index;
        }
    }

    return count;
}

static const AliasEntry *find_alias(const AliasEntry *entries, size_t count, FengSlice alias) {
    size_t index = find_alias_index(entries, count, alias);

    return index < count ? &entries[index] : NULL;
}

static const FengDecl *find_module_public_type_decl(const FengSemanticModule *module, FengSlice name) {
    size_t program_index;

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind == FENG_DECL_TYPE && decl_is_public(decl) &&
                slice_equals(decl->as.type_decl.name, name)) {
                return decl;
            }
            if (decl->kind == FENG_DECL_SPEC && decl_is_public(decl) &&
                slice_equals(decl->as.spec_decl.name, name)) {
                return decl;
            }
        }
    }

    return NULL;
}

static const FengDecl *find_module_public_binding_decl(const FengSemanticModule *module, FengSlice name) {
    size_t program_index;

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind == FENG_DECL_GLOBAL_BINDING && decl_is_public(decl) &&
                slice_equals(decl->as.binding.name, name)) {
                return decl;
            }
        }
    }

    return NULL;
}

static FengMutability normalize_mutability(FengMutability mutability) {
    return mutability == FENG_MUTABILITY_VAR ? FENG_MUTABILITY_VAR : FENG_MUTABILITY_LET;
}

static bool mutability_is_writable(FengMutability mutability) {
    return normalize_mutability(mutability) == FENG_MUTABILITY_VAR;
}

static bool annotation_kind_is_calling_convention(FengAnnotationKind kind) {
    return kind == FENG_ANNOTATION_CDECL || kind == FENG_ANNOTATION_STDCALL ||
           kind == FENG_ANNOTATION_FASTCALL;
}

static bool annotations_contain_kind(const FengAnnotation *annotations,
                                     size_t annotation_count,
                                     FengAnnotationKind kind) {
    size_t annotation_index;

    for (annotation_index = 0U; annotation_index < annotation_count; ++annotation_index) {
        if (annotations[annotation_index].builtin_kind == kind) {
            return true;
        }
    }

    return false;
}

static size_t count_calling_convention_annotations(const FengAnnotation *annotations,
                                                   size_t annotation_count) {
    size_t annotation_index;
    size_t count = 0U;

    for (annotation_index = 0U; annotation_index < annotation_count; ++annotation_index) {
        if (annotation_kind_is_calling_convention(annotations[annotation_index].builtin_kind)) {
            ++count;
        }
    }

    return count;
}

static const FengAnnotation *find_calling_convention_annotation(const FengAnnotation *annotations,
                                                                size_t annotation_count) {
    size_t annotation_index;

    for (annotation_index = 0U; annotation_index < annotation_count; ++annotation_index) {
        if (annotation_kind_is_calling_convention(annotations[annotation_index].builtin_kind)) {
            return &annotations[annotation_index];
        }
    }

    return NULL;
}

static bool extern_library_annotation_arg_is_valid(ResolveContext *context, const FengExpr *expr) {
    const VisibleValueEntry *visible_value;

    if (expr == NULL) {
        return false;
    }
    if (expr->kind == FENG_EXPR_STRING) {
        return true;
    }
    if (expr->kind != FENG_EXPR_IDENTIFIER) {
        return false;
    }

    visible_value = find_visible_value(context->visible_values,
                                       context->visible_value_count,
                                       expr->as.identifier);
    if (visible_value == NULL || visible_value->is_function ||
        visible_value->decl == NULL ||
        visible_value->decl->kind != FENG_DECL_GLOBAL_BINDING) {
        return false;
    }

    return normalize_mutability(visible_value->decl->as.binding.mutability) == FENG_MUTABILITY_LET &&
           visible_value->decl->as.binding.initializer != NULL &&
           visible_value->decl->as.binding.initializer->kind == FENG_EXPR_STRING;
}

static bool validate_extern_function_annotations(ResolveContext *context, const FengDecl *decl) {
    const FengAnnotation *calling_convention;
    size_t calling_convention_count;

    if (context == NULL || decl == NULL || decl->kind != FENG_DECL_FUNCTION || !decl->is_extern) {
        return true;
    }

    if (annotations_contain_kind(decl->annotations, decl->annotation_count, FENG_ANNOTATION_FIXED)) {
        return resolver_append_error(
            context,
            decl->as.function_decl.token,
            format_message(
                "function '%.*s' cannot be marked as @fixed because extern functions declare imported C symbols",
                (int)decl->as.function_decl.name.length,
                decl->as.function_decl.name.data));
    }

    calling_convention = find_calling_convention_annotation(decl->annotations, decl->annotation_count);
    calling_convention_count =
        count_calling_convention_annotations(decl->annotations, decl->annotation_count);

    if (decl->annotation_count != 1U || calling_convention_count != 1U ||
        calling_convention == NULL || calling_convention->arg_count != 1U) {
        return resolver_append_error(
            context,
            decl->as.function_decl.token,
            format_message(
                "extern function '%.*s' must use exactly one of '@cdecl', '@stdcall', or '@fastcall' with a single library argument",
                (int)decl->as.function_decl.name.length,
                decl->as.function_decl.name.data));
    }

    if (extern_library_annotation_arg_is_valid(context, calling_convention->args[0])) {
        return true;
    }

    return resolver_append_error(
        context,
        calling_convention->token,
        format_message(
            "extern function annotation '@%.*s' library argument must be a string literal or a visible let binding initialized directly with a string literal",
            (int)calling_convention->name.length,
            calling_convention->name.data));
}

static const FengDecl *find_module_public_function_decl(const FengSemanticModule *module, FengSlice name) {
    size_t program_index;

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind == FENG_DECL_FUNCTION && decl_is_public(decl) &&
                slice_equals(decl->as.function_decl.name, name)) {
                return decl;
            }
        }
    }

    return NULL;
}

static size_t count_module_public_function_overloads(const FengSemanticModule *module,
                                                     FengSlice name) {
    size_t program_index;
    size_t count = 0U;

    if (module == NULL) {
        return 0U;
    }

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind == FENG_DECL_FUNCTION && decl_is_public(decl) &&
                slice_equals(decl->as.function_decl.name, name)) {
                ++count;
            }
        }
    }

    return count;
}

static bool module_exports_public_type(const FengSemanticModule *module, FengSlice name) {
    return find_module_public_type_decl(module, name) != NULL;
}

static bool module_exports_public_value(const FengSemanticModule *module, FengSlice name) {
    size_t program_index;

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (!decl_is_public(decl)) {
                continue;
            }

            if (decl->kind == FENG_DECL_FUNCTION && slice_equals(decl->as.function_decl.name, name)) {
                return true;
            }
            if (decl->kind == FENG_DECL_GLOBAL_BINDING && slice_equals(decl->as.binding.name, name)) {
                return true;
            }
        }
    }

    return false;
}

static bool module_exports_public_name(const FengSemanticModule *module,
                                       FengSlice name,
                                       bool *is_type,
                                       bool *is_value) {
    bool found_type = module_exports_public_type(module, name);
    bool found_value = module_exports_public_value(module, name);

    if (is_type != NULL) {
        *is_type = found_type;
    }
    if (is_value != NULL) {
        *is_value = found_value;
    }

    return found_type || found_value;
}

static bool append_error(FengSemanticError **errors,
                         size_t *error_count,
                         size_t *error_capacity,
                         const char *path,
                         FengToken token,
                         char *message);

static bool analysis_append_info(const FengSemanticAnalysis *analysis,
                                 const char *path,
                                 FengToken token,
                                 char *message);

static bool resolver_append_error(ResolveContext *context, FengToken token, char *message) {
    return append_error(context->errors,
                        context->error_count,
                        context->error_capacity,
                        context->program->path,
                        token,
                        message);
}

typedef struct LambdaCaptureFrame {
    FengExpr *lambda;          /* lambda whose captures we are collecting */
    size_t outer_scope_floor;  /* scope_count snapshot before pushing lambda's own scope */
    FengLambdaCapture *captures;
    size_t capture_count;
    size_t capture_capacity;
    bool captures_self;
} LambdaCaptureFrame;

static bool resolver_push_scope(ResolveContext *context) {
    ScopeFrame frame;

    memset(&frame, 0, sizeof(frame));
    return append_raw((void **)&context->scopes,
                      &context->scope_count,
                      &context->scope_capacity,
                      sizeof(frame),
                      &frame);
}

static void resolver_pop_scope(ResolveContext *context) {
    ScopeFrame *frame;

    if (context->scope_count == 0U) {
        return;
    }

    frame = &context->scopes[context->scope_count - 1U];
    free(frame->locals);
    --context->scope_count;
}

static bool resolver_add_local_entry(ResolveContext *context,
                                     FengSlice name,
                                     InferredExprType type,
                                     FengMutability mutability,
                                     const FengExpr *source_expr) {
    ScopeFrame *frame;
    LocalNameEntry entry;

    if (context->scope_count == 0U && !resolver_push_scope(context)) {
        return false;
    }

    frame = &context->scopes[context->scope_count - 1U];
    entry.name = name;
    entry.type = type;
    entry.mutability = normalize_mutability(mutability);
    entry.source_expr = source_expr;
    return append_raw((void **)&frame->locals,
                      &frame->local_count,
                      &frame->local_capacity,
                      sizeof(entry),
                      &entry);
}

static bool resolver_add_local_typed_name_with_source(ResolveContext *context,
                                                      FengSlice name,
                                                      InferredExprType type,
                                                      FengMutability mutability,
                                                      const FengExpr *source_expr) {
    return resolver_add_local_entry(context, name, type, mutability, source_expr);
}

static bool resolver_add_local_typed_name(ResolveContext *context,
                                          FengSlice name,
                                          InferredExprType type,
                                          FengMutability mutability) {
    return resolver_add_local_typed_name_with_source(context, name, type, mutability, NULL);
}

static const LocalNameEntry *resolver_find_local_name_entry(const ResolveContext *context,
                                                            FengSlice name) {
    size_t scope_index = context->scope_count;

    while (scope_index > 0U) {
        const ScopeFrame *frame = &context->scopes[scope_index - 1U];
        size_t local_index = frame->local_count;

        while (local_index > 0U) {
            const LocalNameEntry *entry = &frame->locals[local_index - 1U];

            if (slice_equals(entry->name, name)) {
                return entry;
            }
            --local_index;
        }
        --scope_index;
    }

    return NULL;
}

/* Same as resolver_find_local_name_entry, but also reports the 1-based scope
 * index where the binding was found (0 if not found). */
static const LocalNameEntry *resolver_find_local_name_entry_with_scope(
    const ResolveContext *context, FengSlice name, size_t *out_scope_index) {
    size_t scope_index = context->scope_count;

    while (scope_index > 0U) {
        const ScopeFrame *frame = &context->scopes[scope_index - 1U];
        size_t local_index = frame->local_count;

        while (local_index > 0U) {
            const LocalNameEntry *entry = &frame->locals[local_index - 1U];

            if (slice_equals(entry->name, name)) {
                if (out_scope_index != NULL) {
                    *out_scope_index = scope_index;
                }
                return entry;
            }
            --local_index;
        }
        --scope_index;
    }

    if (out_scope_index != NULL) {
        *out_scope_index = 0U;
    }
    return NULL;
}

static bool lambda_frame_record_local(LambdaCaptureFrame *frame,
                                      FengSlice name,
                                      FengMutability mutability) {
    size_t index;
    FengLambdaCapture entry;

    for (index = 0U; index < frame->capture_count; ++index) {
        if (frame->captures[index].kind == FENG_LAMBDA_CAPTURE_LOCAL &&
            slice_equals(frame->captures[index].name, name)) {
            return true;
        }
    }

    entry.kind = FENG_LAMBDA_CAPTURE_LOCAL;
    entry.name = name;
    entry.mutability = mutability;
    return append_raw((void **)&frame->captures,
                      &frame->capture_count,
                      &frame->capture_capacity,
                      sizeof(entry),
                      &entry);
}

/* Record an outer-local capture against every active lambda whose own scope
 * floor is at or above the binding's scope. Nested lambdas thus correctly
 * inherit captures from their enclosing lambdas. */
static bool resolver_record_local_capture(ResolveContext *context,
                                          FengSlice name,
                                          FengMutability mutability,
                                          size_t binding_scope_index) {
    size_t frame_index;

    for (frame_index = 0U; frame_index < context->lambda_frame_count; ++frame_index) {
        LambdaCaptureFrame *frame = &context->lambda_frames[frame_index];

        if (binding_scope_index <= frame->outer_scope_floor) {
            if (!lambda_frame_record_local(frame, name, mutability)) {
                return false;
            }
        }
    }
    return true;
}

static void resolver_record_self_capture(ResolveContext *context) {
    size_t frame_index;

    for (frame_index = 0U; frame_index < context->lambda_frame_count; ++frame_index) {
        context->lambda_frames[frame_index].captures_self = true;
    }
}

static bool resolver_has_local_name(const ResolveContext *context, FengSlice name) {
    return resolver_find_local_name_entry(context, name) != NULL;
}

static bool resolver_track_synthetic_type_ref(ResolveContext *context, FengTypeRef *type_ref) {
    return append_raw((void **)&context->synthetic_type_refs,
                      &context->synthetic_type_ref_count,
                      &context->synthetic_type_ref_capacity,
                      sizeof(type_ref),
                      &type_ref);
}

static FengTypeRef *clone_type_ref_for_inference(const FengTypeRef *type_ref) {
    FengTypeRef *clone;

    if (type_ref == NULL) {
        return NULL;
    }

    clone = (FengTypeRef *)calloc(1U, sizeof(*clone));
    if (clone == NULL) {
        return NULL;
    }

    clone->token = type_ref->token;
    clone->kind = type_ref->kind;
    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            clone->as.named.segment_count = type_ref->as.named.segment_count;
            if (type_ref->as.named.segment_count == 0U) {
                return clone;
            }

            clone->as.named.segments =
                (FengSlice *)malloc(sizeof(FengSlice) * type_ref->as.named.segment_count);
            if (clone->as.named.segments == NULL) {
                free(clone);
                return NULL;
            }
            memcpy(clone->as.named.segments,
                   type_ref->as.named.segments,
                   sizeof(FengSlice) * type_ref->as.named.segment_count);
            return clone;

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            clone->array_element_writable = type_ref->array_element_writable;
            clone->as.inner = clone_type_ref_for_inference(type_ref->as.inner);
            if (clone->as.inner == NULL) {
                free(clone);
                return NULL;
            }
            return clone;
    }

    free(clone);
    return NULL;
}

static FengTypeRef *create_named_type_ref_for_inference(FengToken token,
                                                        FengSlice *segments,
                                                        size_t segment_count) {
    FengTypeRef *type_ref = (FengTypeRef *)calloc(1U, sizeof(*type_ref));

    if (type_ref == NULL) {
        return NULL;
    }

    type_ref->token = token;
    type_ref->kind = FENG_TYPE_REF_NAMED;
    type_ref->as.named.segments = segments;
    type_ref->as.named.segment_count = segment_count;
    return type_ref;
}

static FengTypeRef *create_type_ref_from_inferred_type(const InferredExprType *type, FengToken token) {
    FengTypeRef *type_ref;
    FengSlice *segments;

    if (type == NULL) {
        return NULL;
    }

    switch (type->kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
            segments = (FengSlice *)malloc(sizeof(FengSlice));
            if (segments == NULL) {
                return NULL;
            }
            segments[0] = type->builtin_name;
            type_ref = create_named_type_ref_for_inference(token, segments, 1U);
            if (type_ref == NULL) {
                free(segments);
            }
            return type_ref;

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return clone_type_ref_for_inference(type->type_ref);

        case FENG_INFERRED_EXPR_TYPE_DECL:
            if (type->type_decl == NULL || type->type_decl->kind != FENG_DECL_TYPE) {
                return NULL;
            }

            segments = (FengSlice *)malloc(sizeof(FengSlice));
            if (segments == NULL) {
                return NULL;
            }
            segments[0] = type->type_decl->as.type_decl.name;
            type_ref = create_named_type_ref_for_inference(token, segments, 1U);
            if (type_ref == NULL) {
                free(segments);
            }
            return type_ref;

        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return NULL;
    }

    return NULL;
}

static const FengTypeRef *synthesize_array_type_ref(ResolveContext *context,
                                                    const InferredExprType *element_type,
                                                    bool element_writable,
                                                    FengToken token) {
    FengTypeRef *inner_type_ref;
    FengTypeRef *array_type_ref;

    if (context == NULL || element_type == NULL) {
        return NULL;
    }

    inner_type_ref = create_type_ref_from_inferred_type(element_type, token);
    if (inner_type_ref == NULL) {
        return NULL;
    }

    array_type_ref = (FengTypeRef *)calloc(1U, sizeof(*array_type_ref));
    if (array_type_ref == NULL) {
        free_synthetic_type_ref(inner_type_ref);
        return NULL;
    }

    array_type_ref->token = token;
    array_type_ref->kind = FENG_TYPE_REF_ARRAY;
    array_type_ref->array_element_writable = element_writable;
    array_type_ref->as.inner = inner_type_ref;
    if (!resolver_track_synthetic_type_ref(context, array_type_ref)) {
        free_synthetic_type_ref(array_type_ref);
        return NULL;
    }

    return array_type_ref;
}

static void resolver_free_scopes(ResolveContext *context) {
    size_t type_ref_index;

    while (context->scope_count > 0U) {
        resolver_pop_scope(context);
    }
    free(context->scopes);
    context->scopes = NULL;
    context->scope_capacity = 0U;

    for (type_ref_index = 0U; type_ref_index < context->synthetic_type_ref_count; ++type_ref_index) {
        free_synthetic_type_ref(context->synthetic_type_refs[type_ref_index]);
    }
    free(context->synthetic_type_refs);
    context->synthetic_type_refs = NULL;
    context->synthetic_type_ref_count = 0U;
    context->synthetic_type_ref_capacity = 0U;
}

static bool resolve_expr(ResolveContext *context, const FengExpr *expr, bool allow_self);
static bool resolve_block(ResolveContext *context, const FengBlock *block, bool allow_self);
static InferredExprType infer_expr_type(ResolveContext *context, const FengExpr *expr);
static bool evaluate_constant_expr(ResolveContext *context,
                                   const FengExpr *expr,
                                   FengConstValue *out);
static bool validate_expr_against_expected_type(ResolveContext *context,
                                                const FengExpr *expr,
                                                const FengTypeRef *expected_type);
static bool inferred_expr_types_equal(const ResolveContext *context,
                                      InferredExprType left,
                                      InferredExprType right);
static InferredExprType infer_lambda_body_type(ResolveContext *context, const FengExpr *expr);
static bool resolve_block_contents(ResolveContext *context,
                                   const FengBlock *block,
                                   bool allow_self);
static bool callable_return_inference_is_pending(ResolveContext *context,
                                                 const FengCallableSignature *callable);
static bool expr_type_inference_is_pending(ResolveContext *context, const FengExpr *expr);
static bool lambda_expr_matches_function_type(ResolveContext *context,
                                              const FengExpr *expr,
                                              const FengDecl *function_type_decl);
static bool lambda_expr_signature_matches_lambda_expr(ResolveContext *context,
                                                      const FengExpr *left,
                                                      const FengExpr *right);
static bool lambda_expr_parameters_match_args(ResolveContext *context,
                                              const FengExpr *expr,
                                              FengExpr *const *args,
                                              size_t arg_count);
static char *format_expr_target_name(const FengExpr *expr);
static const FengTypeRef *resolve_indexed_array_element_type_ref(ResolveContext *context,
                                                                 const FengExpr *object_expr);
static char *format_inferred_expr_type_name(InferredExprType type);
static bool expr_matches_expected_type_ref(ResolveContext *context,
                                           const FengExpr *expr,
                                           const FengTypeRef *expected_type_ref);
static CallableValueResolution resolve_expr_callable_value(ResolveContext *context,
                                                           const FengExpr *expr,
                                                           const FengTypeRef *expected_type_ref);
static ResolvedTypeTarget resolve_type_target_expr(const ResolveContext *context,
                                                   const FengExpr *target_expr,
                                                   bool follow_call_callee);
static bool type_decl_satisfies_spec_decl(const ResolveContext *ctx,
                                          const FengDecl *type_decl,
                                          const FengDecl *spec_decl);

static const AliasEntry *find_unshadowed_alias(const ResolveContext *context, FengSlice alias_name) {
    if (resolver_has_local_name(context, alias_name) ||
        find_visible_value(context->visible_values, context->visible_value_count, alias_name) != NULL ||
        find_visible_type(context->visible_types, context->visible_type_count, alias_name) != NULL) {
        return NULL;
    }

    return find_alias(context->aliases, context->alias_count, alias_name);
}

static const FengDecl *find_named_type_decl(const ResolveContext *context,
                                            const FengSlice *segments,
                                            size_t segment_count) {
    FengSlice name;

    if (segment_count == 0U) {
        return NULL;
    }

    name = segments[segment_count - 1U];
    if (segment_count == 1U) {
        if (is_builtin_type_name(name)) {
            return NULL;
        }

        return find_visible_type_decl(context->visible_types, context->visible_type_count, name);
    }

    if (segment_count == 2U) {
        const AliasEntry *alias = find_alias(context->aliases, context->alias_count, segments[0]);

        if (alias != NULL) {
            return find_module_public_type_decl(alias->target_module, segments[1]);
        }
    }

    {
        size_t module_index = find_module_index_by_path(context->analysis, segments, segment_count - 1U);

        if (module_index < context->analysis->module_count &&
            module_is_use_visible_from(context, &context->analysis->modules[module_index])) {
            return find_module_public_type_decl(&context->analysis->modules[module_index], name);
        }
    }

    return NULL;
}

static const char *type_ref_builtin_canonical_name(const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count != 1U) {
        return NULL;
    }

    return canonical_builtin_type_name(type_ref->as.named.segments[0]);
}

static const FengDecl *resolve_type_ref_decl(const ResolveContext *context,
                                             const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED) {
        return NULL;
    }
    if (type_ref->as.named.segment_count == 1U &&
        is_builtin_type_name(type_ref->as.named.segments[0])) {
        return NULL;
    }

    return find_named_type_decl(
        context, type_ref->as.named.segments, type_ref->as.named.segment_count);
}

static bool type_refs_semantically_equal(const ResolveContext *context,
                                         const FengTypeRef *left,
                                         const FengTypeRef *right) {
    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL) {
        return false;
    }
    if (left->kind != right->kind) {
        return false;
    }

    switch (left->kind) {
        case FENG_TYPE_REF_NAMED: {
            const char *left_builtin = type_ref_builtin_canonical_name(left);
            const char *right_builtin = type_ref_builtin_canonical_name(right);

            if (left_builtin != NULL || right_builtin != NULL) {
                return left_builtin != NULL && right_builtin != NULL &&
                       strcmp(left_builtin, right_builtin) == 0;
            }

            {
                const FengDecl *left_decl = resolve_type_ref_decl(context, left);
                const FengDecl *right_decl = resolve_type_ref_decl(context, right);

                if (left_decl != NULL || right_decl != NULL) {
                    return left_decl != NULL && left_decl == right_decl;
                }
            }

            return type_ref_equals(left, right);
        }

        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            if (left->kind == FENG_TYPE_REF_ARRAY &&
                left->array_element_writable != right->array_element_writable) {
                return false;
            }
            return type_refs_semantically_equal(context, left->as.inner, right->as.inner);
    }

    return false;
}

static bool inferred_expr_type_matches_type_ref(const ResolveContext *context,
                                                InferredExprType expr_type,
                                                const FengTypeRef *type_ref) {
    const char *expr_builtin;
    const char *target_builtin;
    const FengDecl *target_decl;

    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
            expr_builtin = canonical_builtin_type_name(expr_type.builtin_name);
            target_builtin = type_ref_builtin_canonical_name(type_ref);
            return expr_builtin != NULL && target_builtin != NULL &&
                   strcmp(expr_builtin, target_builtin) == 0;

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            if (expr_type.type_ref != NULL &&
                type_refs_semantically_equal(context, expr_type.type_ref, type_ref)) {
                return true;
            }
            {
                const FengDecl *src_decl = resolve_type_ref_decl(context, expr_type.type_ref);
                const FengDecl *dst_decl = resolve_type_ref_decl(context, type_ref);

                /* Object-form spec satisfaction is nominal: the source type must explicitly
                 * declare the spec (in its declared spec list, transitively, or via a visible
                 * fit). Structural shape match alone is not sufficient — duck typing is not
                 * supported here. See type_decl_satisfies_spec_decl for the lookup. */
                if (src_decl != NULL && dst_decl != NULL &&
                    src_decl->kind == FENG_DECL_TYPE &&
                    dst_decl->kind == FENG_DECL_SPEC &&
                    type_decl_satisfies_spec_decl(context, src_decl, dst_decl)) {
                    return true;
                }
            }
            return false;

        case FENG_INFERRED_EXPR_TYPE_DECL:
            target_decl = resolve_type_ref_decl(context, type_ref);
            if (target_decl != NULL && target_decl == expr_type.type_decl) {
                return true;
            }
            if (target_decl != NULL && target_decl->kind == FENG_DECL_SPEC &&
                expr_type.type_decl != NULL && expr_type.type_decl->kind == FENG_DECL_TYPE &&
                type_decl_satisfies_spec_decl(context, expr_type.type_decl, target_decl)) {
                return true;
            }
            return false;

        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
            target_decl = resolve_type_ref_decl(context, type_ref);
            if (!decl_is_function_type(target_decl)) {
                return false;
            }
            return lambda_expr_matches_function_type((ResolveContext *)context,
                                                     expr_type.lambda_expr,
                                                     target_decl);

        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return false;
    }

    return false;
}

static bool inferred_expr_types_equal(const ResolveContext *context,
                                      InferredExprType left,
                                      InferredExprType right) {
    if (left.kind == FENG_INFERRED_EXPR_TYPE_UNKNOWN ||
        right.kind == FENG_INFERRED_EXPR_TYPE_UNKNOWN) {
        return false;
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_BUILTIN &&
        right.kind == FENG_INFERRED_EXPR_TYPE_BUILTIN) {
        return strcmp(canonical_builtin_type_name(left.builtin_name),
                      canonical_builtin_type_name(right.builtin_name)) == 0;
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
        right.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF) {
        return type_refs_semantically_equal(context, left.type_ref, right.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_DECL &&
        right.kind == FENG_INFERRED_EXPR_TYPE_DECL) {
        return left.type_decl != NULL && left.type_decl == right.type_decl;
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA &&
        right.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA) {
        return lambda_expr_signature_matches_lambda_expr(
            (ResolveContext *)context, left.lambda_expr, right.lambda_expr);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_BUILTIN &&
        right.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF) {
        return inferred_expr_type_matches_type_ref(context, left, right.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
        right.kind == FENG_INFERRED_EXPR_TYPE_BUILTIN) {
        return inferred_expr_type_matches_type_ref(context, right, left.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_DECL &&
        right.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF) {
        return inferred_expr_type_matches_type_ref(context, left, right.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
        right.kind == FENG_INFERRED_EXPR_TYPE_DECL) {
        return inferred_expr_type_matches_type_ref(context, right, left.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA &&
        right.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF) {
        return inferred_expr_type_matches_type_ref(context, left, right.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
        right.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA) {
        return inferred_expr_type_matches_type_ref(context, right, left.type_ref);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA &&
        right.kind == FENG_INFERRED_EXPR_TYPE_DECL &&
        right.type_decl != NULL &&
        decl_is_function_type(right.type_decl)) {
        return lambda_expr_matches_function_type((ResolveContext *)context,
                                                 left.lambda_expr,
                                                 right.type_decl);
    }
    if (left.kind == FENG_INFERRED_EXPR_TYPE_DECL &&
        right.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA &&
        left.type_decl != NULL &&
        decl_is_function_type(left.type_decl)) {
        return lambda_expr_matches_function_type((ResolveContext *)context,
                                                 right.lambda_expr,
                                                 left.type_decl);
    }

    return false;
}

static const FengDecl *resolve_function_type_decl(const ResolveContext *context,
                                                  const FengTypeRef *type_ref) {
    const FengDecl *type_decl = resolve_type_ref_decl(context, type_ref);

    if (!decl_is_function_type(type_decl)) {
        return NULL;
    }

    return type_decl;
}

static bool inferred_expr_type_is_void(InferredExprType expr_type) {
    return expr_type.kind == FENG_INFERRED_EXPR_TYPE_BUILTIN &&
           slice_equals_cstr(expr_type.builtin_name, "void");
}

static bool function_type_decl_return_matches_inferred_type(const ResolveContext *context,
                                                            const FengDecl *function_type_decl,
                                                            InferredExprType return_type) {
    const FengTypeRef *expected_return_type;

    if (!decl_is_function_type(function_type_decl) ||
        !inferred_expr_type_is_known(return_type)) {
        return false;
    }

    expected_return_type = function_type_decl->as.spec_decl.as.callable.return_type;
    if (expected_return_type == NULL || type_ref_is_void(expected_return_type)) {
        return inferred_expr_type_is_void(return_type);
    }

    return inferred_expr_type_matches_type_ref(context, return_type, expected_return_type);
}

static bool function_type_decl_matches_callable_signature(const ResolveContext *context,
                                                          const FengDecl *function_type_decl,
                                                          const FengCallableSignature *callable) {
    size_t param_index;

    if (function_type_decl == NULL || callable == NULL ||
        !decl_is_function_type(function_type_decl)) {
        return false;
    }
    if (function_type_decl->as.spec_decl.as.callable.param_count != callable->param_count) {
        return false;
    }

    for (param_index = 0U;
         param_index < function_type_decl->as.spec_decl.as.callable.param_count;
         ++param_index) {
        if (!type_refs_semantically_equal(context,
                                          function_type_decl->as.spec_decl.as.callable.params[param_index].type,
                                          callable->params[param_index].type)) {
            return false;
        }
    }

    return function_type_decl_return_matches_inferred_type(
        context, function_type_decl, callable_effective_return_type(context, callable));
}

static bool function_type_decl_matches_callable_signature_or_is_pending(
    ResolveContext *context,
    const FengDecl *function_type_decl,
    const FengCallableSignature *callable) {
    size_t param_index;

    if (function_type_decl_matches_callable_signature(context, function_type_decl, callable)) {
        return true;
    }

    if (function_type_decl == NULL || callable == NULL ||
        !decl_is_function_type(function_type_decl)) {
        return false;
    }
    if (function_type_decl->as.spec_decl.as.callable.param_count != callable->param_count) {
        return false;
    }

    for (param_index = 0U;
         param_index < function_type_decl->as.spec_decl.as.callable.param_count;
         ++param_index) {
        if (!type_refs_semantically_equal(context,
                                          function_type_decl->as.spec_decl.as.callable.params[param_index].type,
                                          callable->params[param_index].type)) {
            return false;
        }
    }

    return callable_return_inference_is_pending(context, callable);
}

static const char *inferred_expr_type_builtin_canonical_name(InferredExprType expr_type) {
    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
            return canonical_builtin_type_name(expr_type.builtin_name);

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return type_ref_builtin_canonical_name(expr_type.type_ref);

        case FENG_INFERRED_EXPR_TYPE_DECL:
        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return NULL;
    }

    return NULL;
}

static bool inferred_expr_type_is_numeric(InferredExprType expr_type) {
    const char *builtin_name = inferred_expr_type_builtin_canonical_name(expr_type);

    return builtin_name != NULL && builtin_type_name_is_numeric(slice_from_cstr(builtin_name));
}

static bool inferred_expr_type_is_integer(InferredExprType expr_type) {
    const char *builtin_name = inferred_expr_type_builtin_canonical_name(expr_type);

    return builtin_name != NULL && builtin_type_name_is_integer(slice_from_cstr(builtin_name));
}

static bool inferred_expr_type_is_float(InferredExprType expr_type) {
    return inferred_expr_type_is_numeric(expr_type) &&
           !inferred_expr_type_is_integer(expr_type);
}

static bool inferred_expr_type_is_bool(InferredExprType expr_type) {
    const char *builtin_name = inferred_expr_type_builtin_canonical_name(expr_type);

    return builtin_name != NULL && strcmp(builtin_name, "bool") == 0;
}

static bool inferred_expr_type_is_string(InferredExprType expr_type) {
    const char *builtin_name = inferred_expr_type_builtin_canonical_name(expr_type);

    return builtin_name != NULL && strcmp(builtin_name, "string") == 0;
}

static const char *format_operator_name(FengTokenKind kind) {
    switch (kind) {
        case FENG_TOKEN_PLUS:
            return "+";
        case FENG_TOKEN_MINUS:
            return "-";
        case FENG_TOKEN_STAR:
            return "*";
        case FENG_TOKEN_SLASH:
            return "/";
        case FENG_TOKEN_PERCENT:
            return "%";
        case FENG_TOKEN_PLUS_ASSIGN:
            return "+=";
        case FENG_TOKEN_MINUS_ASSIGN:
            return "-=";
        case FENG_TOKEN_STAR_ASSIGN:
            return "*=";
        case FENG_TOKEN_SLASH_ASSIGN:
            return "/=";
        case FENG_TOKEN_PERCENT_ASSIGN:
            return "%=";
        case FENG_TOKEN_NOT:
            return "!";
        case FENG_TOKEN_TILDE:
            return "~";
        case FENG_TOKEN_LT:
            return "<";
        case FENG_TOKEN_LE:
            return "<=";
        case FENG_TOKEN_GT:
            return ">";
        case FENG_TOKEN_GE:
            return ">=";
        case FENG_TOKEN_EQ:
            return "==";
        case FENG_TOKEN_NE:
            return "!=";
        case FENG_TOKEN_AND_AND:
            return "&&";
        case FENG_TOKEN_OR_OR:
            return "||";
        case FENG_TOKEN_AMP:
            return "&";
        case FENG_TOKEN_AMP_ASSIGN:
            return "&=";
        case FENG_TOKEN_PIPE:
            return "|";
        case FENG_TOKEN_PIPE_ASSIGN:
            return "|=";
        case FENG_TOKEN_CARET:
            return "^";
        case FENG_TOKEN_CARET_ASSIGN:
            return "^=";
        case FENG_TOKEN_SHL:
            return "<<";
        case FENG_TOKEN_SHL_ASSIGN:
            return "<<=";
        case FENG_TOKEN_SHR:
            return ">>";
        case FENG_TOKEN_SHR_ASSIGN:
            return ">>=";
        default:
            return "?";
    }
}

static FengTokenKind assignment_operator_binary_operator(FengTokenKind op) {
    switch (op) {
        case FENG_TOKEN_PLUS_ASSIGN:
            return FENG_TOKEN_PLUS;
        case FENG_TOKEN_MINUS_ASSIGN:
            return FENG_TOKEN_MINUS;
        case FENG_TOKEN_STAR_ASSIGN:
            return FENG_TOKEN_STAR;
        case FENG_TOKEN_SLASH_ASSIGN:
            return FENG_TOKEN_SLASH;
        case FENG_TOKEN_PERCENT_ASSIGN:
            return FENG_TOKEN_PERCENT;
        case FENG_TOKEN_AMP_ASSIGN:
            return FENG_TOKEN_AMP;
        case FENG_TOKEN_PIPE_ASSIGN:
            return FENG_TOKEN_PIPE;
        case FENG_TOKEN_CARET_ASSIGN:
            return FENG_TOKEN_CARET;
        case FENG_TOKEN_SHL_ASSIGN:
            return FENG_TOKEN_SHL;
        case FENG_TOKEN_SHR_ASSIGN:
            return FENG_TOKEN_SHR;
        default:
            return FENG_TOKEN_ERROR;
    }
}

static bool assignment_operator_is_numeric_compound(FengTokenKind op) {
    switch (op) {
        case FENG_TOKEN_PLUS_ASSIGN:
        case FENG_TOKEN_MINUS_ASSIGN:
        case FENG_TOKEN_STAR_ASSIGN:
        case FENG_TOKEN_SLASH_ASSIGN:
        case FENG_TOKEN_PERCENT_ASSIGN:
            return true;
        default:
            return false;
    }
}

static bool assignment_operator_is_bitwise_compound(FengTokenKind op) {
    switch (op) {
        case FENG_TOKEN_AMP_ASSIGN:
        case FENG_TOKEN_PIPE_ASSIGN:
        case FENG_TOKEN_CARET_ASSIGN:
        case FENG_TOKEN_SHL_ASSIGN:
        case FENG_TOKEN_SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

static bool unary_expr_type_is_valid(FengTokenKind op, InferredExprType operand_type) {
    switch (op) {
        case FENG_TOKEN_MINUS:
            return inferred_expr_type_is_numeric(operand_type);

        case FENG_TOKEN_NOT:
            return inferred_expr_type_is_bool(operand_type);

        case FENG_TOKEN_TILDE:
            return inferred_expr_type_is_integer(operand_type);

        default:
            return false;
    }
}

static bool binary_expr_types_are_valid(ResolveContext *context,
                                        FengTokenKind op,
                                        InferredExprType left_type,
                                        InferredExprType right_type) {
    switch (op) {
        case FENG_TOKEN_PLUS:
            return inferred_expr_types_equal(context, left_type, right_type) &&
                   (inferred_expr_type_is_numeric(left_type) || inferred_expr_type_is_string(left_type));

        case FENG_TOKEN_MINUS:
        case FENG_TOKEN_STAR:
        case FENG_TOKEN_SLASH:
        case FENG_TOKEN_PERCENT:
            return inferred_expr_types_equal(context, left_type, right_type) &&
                   inferred_expr_type_is_numeric(left_type);

        case FENG_TOKEN_LT:
        case FENG_TOKEN_LE:
        case FENG_TOKEN_GT:
        case FENG_TOKEN_GE:
            return inferred_expr_types_equal(context, left_type, right_type) &&
                   inferred_expr_type_is_numeric(left_type);

        case FENG_TOKEN_EQ:
        case FENG_TOKEN_NE:
            return inferred_expr_type_is_known(left_type) &&
                   inferred_expr_type_is_known(right_type) &&
                   inferred_expr_types_equal(context, left_type, right_type);

        case FENG_TOKEN_AND_AND:
        case FENG_TOKEN_OR_OR:
            return inferred_expr_type_is_bool(left_type) && inferred_expr_type_is_bool(right_type);

        case FENG_TOKEN_AMP:
        case FENG_TOKEN_PIPE:
        case FENG_TOKEN_CARET:
        case FENG_TOKEN_SHL:
        case FENG_TOKEN_SHR:
            return inferred_expr_types_equal(context, left_type, right_type) &&
                   inferred_expr_type_is_integer(left_type);

        default:
            return false;
    }
}

static bool validate_unary_expr(ResolveContext *context, const FengExpr *expr) {
    InferredExprType operand_type;
    const char *operator_name;
    char *operand_type_name;
    char *message;

    operand_type = infer_expr_type(context, expr->as.unary.operand);
    if (unary_expr_type_is_valid(expr->as.unary.op, operand_type)) {
        return true;
    }

    operator_name = format_operator_name(expr->as.unary.op);
    operand_type_name = format_inferred_expr_type_name(operand_type);
    {
        const char *fmt;

        switch (expr->as.unary.op) {
            case FENG_TOKEN_MINUS:
                fmt = "unary operator '%s' requires a numeric operand, got '%s'";
                break;
            case FENG_TOKEN_TILDE:
                fmt = "unary operator '%s' requires an integer operand, got '%s'";
                break;
            case FENG_TOKEN_NOT:
            default:
                fmt = "unary operator '%s' requires a bool operand, got '%s'";
                break;
        }
        message = format_message(fmt,
                                 operator_name,
                                 operand_type_name != NULL ? operand_type_name : "<unknown>");
    }
    free(operand_type_name);
    return resolver_append_error(context, expr->token, message);
}

static int canonical_integer_bit_width(const char *canonical_name) {
    if (canonical_name == NULL) {
        return 0;
    }
    if (strcmp(canonical_name, "i8") == 0 || strcmp(canonical_name, "u8") == 0) {
        return 8;
    }
    if (strcmp(canonical_name, "i16") == 0 || strcmp(canonical_name, "u16") == 0) {
        return 16;
    }
    if (strcmp(canonical_name, "i32") == 0 || strcmp(canonical_name, "u32") == 0) {
        return 32;
    }
    if (strcmp(canonical_name, "i64") == 0 || strcmp(canonical_name, "u64") == 0) {
        return 64;
    }
    return 0;
}

static bool validate_integer_shift_rhs_range(ResolveContext *context,
                                             FengToken anchor,
                                             InferredExprType left_type,
                                             const FengExpr *right_expr) {
    FengConstValue shift_value;

    if (!evaluate_constant_expr(context, right_expr, &shift_value) ||
        shift_value.kind != FENG_CONST_INT) {
        return true;
    }

    int64_t shift_amount = shift_value.i;
    int bit_width = canonical_integer_bit_width(
        inferred_expr_type_builtin_canonical_name(left_type));

    if (bit_width <= 0 ||
        (shift_amount >= 0 && shift_amount < (int64_t)bit_width)) {
        return true;
    }

    char *lt_name = format_inferred_expr_type_name(left_type);
    char *msg = format_message(
        "shift amount %lld is out of range for type '%s' (must be in [0, %d))",
        (long long)shift_amount,
        lt_name != NULL ? lt_name : "<unknown>",
        bit_width);

    free(lt_name);
    return resolver_append_error(context, anchor, msg);
}

static bool validate_division_or_modulo_rhs_zero(ResolveContext *context,
                                                 FengToken anchor,
                                                 FengTokenKind op,
                                                 InferredExprType left_type,
                                                 InferredExprType right_type,
                                                 const FengExpr *right_expr,
                                                 const char *context_label) {
    FengConstValue rhs_value;
    const char *kind_word;

    if (!evaluate_constant_expr(context, right_expr, &rhs_value)) {
        return true;
    }

    if (op == FENG_TOKEN_SLASH &&
        inferred_expr_type_is_integer(left_type) &&
        inferred_expr_type_is_integer(right_type) &&
        rhs_value.kind == FENG_CONST_INT && rhs_value.i == 0) {
        kind_word = "division";
    } else if (op == FENG_TOKEN_PERCENT &&
               inferred_expr_type_is_integer(left_type) &&
               inferred_expr_type_is_integer(right_type) &&
               rhs_value.kind == FENG_CONST_INT && rhs_value.i == 0) {
        kind_word = "modulo";
    } else if (op == FENG_TOKEN_PERCENT &&
               inferred_expr_type_is_float(left_type) &&
               inferred_expr_type_is_float(right_type) &&
               rhs_value.kind == FENG_CONST_FLOAT && rhs_value.f == 0.0) {
        kind_word = "modulo";
    } else {
        return true;
    }

    return resolver_append_error(
        context,
        anchor,
        format_message("%s by zero in %s '%s' expression",
                       kind_word,
                       context_label,
                       format_operator_name(op)));
}

static bool validate_compound_assignment(ResolveContext *context, const FengStmt *stmt) {
    InferredExprType left_type = infer_expr_type(context, stmt->as.assign.target);
    InferredExprType right_type = infer_expr_type(context, stmt->as.assign.value);
    FengTokenKind binary_op = assignment_operator_binary_operator(stmt->as.assign.op);
    const char *operator_name = format_operator_name(stmt->as.assign.op);
    char *left_type_name;
    char *right_type_name;
    char *message;

    if (binary_op == FENG_TOKEN_ERROR) {
        return resolver_append_error(context,
                                     stmt->token,
                                     format_message("unsupported compound assignment operator '%s'",
                                                    operator_name));
    }

    if (assignment_operator_is_numeric_compound(stmt->as.assign.op) &&
        inferred_expr_types_equal(context, left_type, right_type) &&
        inferred_expr_type_is_numeric(left_type)) {
        if ((binary_op == FENG_TOKEN_SHL || binary_op == FENG_TOKEN_SHR) &&
            !validate_integer_shift_rhs_range(context,
                                              stmt->token,
                                              left_type,
                                              stmt->as.assign.value)) {
            return false;
        }
        return validate_division_or_modulo_rhs_zero(context,
                                                    stmt->token,
                                                    binary_op,
                                                    left_type,
                                                    right_type,
                                                    stmt->as.assign.value,
                                                    "compile-time compound");
    }

    if (assignment_operator_is_bitwise_compound(stmt->as.assign.op) &&
        inferred_expr_types_equal(context, left_type, right_type) &&
        inferred_expr_type_is_integer(left_type)) {
        if (binary_op == FENG_TOKEN_SHL || binary_op == FENG_TOKEN_SHR) {
            return validate_integer_shift_rhs_range(context,
                                                    stmt->token,
                                                    left_type,
                                                    stmt->as.assign.value);
        }
        return true;
    }

    left_type_name = format_inferred_expr_type_name(left_type);
    right_type_name = format_inferred_expr_type_name(right_type);

    if (assignment_operator_is_numeric_compound(stmt->as.assign.op)) {
        message = format_message(
            "compound assignment operator '%s' requires operands of the same numeric type, got '%s' and '%s'",
            operator_name,
            left_type_name != NULL ? left_type_name : "<unknown>",
            right_type_name != NULL ? right_type_name : "<unknown>");
    } else {
        message = format_message(
            "compound assignment operator '%s' requires operands of the same integer type, got '%s' and '%s'",
            operator_name,
            left_type_name != NULL ? left_type_name : "<unknown>",
            right_type_name != NULL ? right_type_name : "<unknown>");
    }

    free(left_type_name);
    free(right_type_name);
    return resolver_append_error(context, stmt->token, message);
}

static bool validate_binary_expr(ResolveContext *context, const FengExpr *expr) {
    InferredExprType left_type = infer_expr_type(context, expr->as.binary.left);
    InferredExprType right_type = infer_expr_type(context, expr->as.binary.right);
    const char *operator_name = format_operator_name(expr->as.binary.op);
    char *left_type_name;
    char *right_type_name;
    char *message;

    if (binary_expr_types_are_valid(context, expr->as.binary.op, left_type, right_type)) {
        if (expr->as.binary.op == FENG_TOKEN_SHL || expr->as.binary.op == FENG_TOKEN_SHR) {
            return validate_integer_shift_rhs_range(context,
                                                    expr->token,
                                                    left_type,
                                                    expr->as.binary.right);
        }
        return validate_division_or_modulo_rhs_zero(context,
                                                    expr->token,
                                                    expr->as.binary.op,
                                                    left_type,
                                                    right_type,
                                                    expr->as.binary.right,
                                                    "compile-time");
    }

    left_type_name = format_inferred_expr_type_name(left_type);
    right_type_name = format_inferred_expr_type_name(right_type);

    switch (expr->as.binary.op) {
        case FENG_TOKEN_PLUS:
            message = format_message(
                "binary operator '%s' requires operands of the same numeric or string type, got '%s' and '%s'",
                operator_name,
                left_type_name != NULL ? left_type_name : "<unknown>",
                right_type_name != NULL ? right_type_name : "<unknown>");
            break;

        case FENG_TOKEN_MINUS:
        case FENG_TOKEN_STAR:
        case FENG_TOKEN_SLASH:
        case FENG_TOKEN_PERCENT:
        case FENG_TOKEN_LT:
        case FENG_TOKEN_LE:
        case FENG_TOKEN_GT:
        case FENG_TOKEN_GE:
            message = format_message(
                "binary operator '%s' requires operands of the same numeric type, got '%s' and '%s'",
                operator_name,
                left_type_name != NULL ? left_type_name : "<unknown>",
                right_type_name != NULL ? right_type_name : "<unknown>");
            break;

        case FENG_TOKEN_EQ:
        case FENG_TOKEN_NE:
            message = format_message("binary operator '%s' requires operands of the same type, got '%s' and '%s'",
                                     operator_name,
                                     left_type_name != NULL ? left_type_name : "<unknown>",
                                     right_type_name != NULL ? right_type_name : "<unknown>");
            break;

        case FENG_TOKEN_AND_AND:
        case FENG_TOKEN_OR_OR:
            message = format_message("binary operator '%s' requires bool operands, got '%s' and '%s'",
                                     operator_name,
                                     left_type_name != NULL ? left_type_name : "<unknown>",
                                     right_type_name != NULL ? right_type_name : "<unknown>");
            break;

        case FENG_TOKEN_AMP:
        case FENG_TOKEN_PIPE:
        case FENG_TOKEN_CARET:
        case FENG_TOKEN_SHL:
        case FENG_TOKEN_SHR:
            message = format_message(
                "binary operator '%s' requires operands of the same integer type, got '%s' and '%s'",
                operator_name,
                left_type_name != NULL ? left_type_name : "<unknown>",
                right_type_name != NULL ? right_type_name : "<unknown>");
            break;

        default:
            message = format_message("binary operator '%s' is not supported in this context",
                                     operator_name);
            break;
    }

    free(left_type_name);
    free(right_type_name);
    return resolver_append_error(context, expr->token, message);
}

static const FengExpr *block_yield_expression(const FengBlock *block) {
    const FengStmt *last;

    if (block == NULL || block->statement_count == 0U) {
        return NULL;
    }
    last = block->statements[block->statement_count - 1U];
    if (last == NULL || last->kind != FENG_STMT_EXPR) {
        return NULL;
    }
    return last->as.expr;
}

static InferredExprType block_yield_inferred_type(ResolveContext *context, const FengBlock *block) {
    const FengExpr *yield = block_yield_expression(block);

    return yield != NULL ? infer_expr_type(context, yield) : inferred_expr_type_unknown();
}

static bool validate_block_yields_expression(ResolveContext *context,
                                             const FengBlock *block,
                                             FengToken anchor,
                                             const char *ctx_label) {
    if (block_yield_expression(block) != NULL) {
        return true;
    }
    return resolver_append_error(
        context,
        anchor,
        format_message("%s branch block must end with an expression statement", ctx_label));
}

static bool validate_if_expr(ResolveContext *context, const FengExpr *expr) {
    InferredExprType condition_type = infer_expr_type(context, expr->as.if_expr.condition);
    InferredExprType then_type;
    InferredExprType else_type;

    if (!inferred_expr_type_is_bool(condition_type)) {
        char *condition_type_name = format_inferred_expr_type_name(condition_type);
        char *message = format_message("if expression condition must have type 'bool', got '%s'",
                                       condition_type_name != NULL ? condition_type_name : "<unknown>");

        free(condition_type_name);
        return resolver_append_error(context, expr->token, message);
    }

    if (expr->as.if_expr.else_block == NULL) {
        return resolver_append_error(
            context,
            expr->token,
            format_message("if expressions require an else branch"));
    }

    if (!validate_block_yields_expression(context,
                                          expr->as.if_expr.then_block,
                                          expr->token,
                                          "if expression then") ||
        !validate_block_yields_expression(context,
                                          expr->as.if_expr.else_block,
                                          expr->token,
                                          "if expression else")) {
        return false;
    }

    then_type = block_yield_inferred_type(context, expr->as.if_expr.then_block);
    else_type = block_yield_inferred_type(context, expr->as.if_expr.else_block);

    if (inferred_expr_types_equal(context, then_type, else_type)) {
        return true;
    }

    {
        char *then_type_name = format_inferred_expr_type_name(then_type);
        char *else_type_name = format_inferred_expr_type_name(else_type);
        char *message = format_message("if expression branches must have the same type, got '%s' and '%s'",
                                       then_type_name != NULL ? then_type_name : "<unknown>",
                                       else_type_name != NULL ? else_type_name : "<unknown>");

        free(then_type_name);
        free(else_type_name);
        return resolver_append_error(context, expr->token, message);
    }
}

/* Match label literal extraction.
 *
 * Per docs/feng-flow.md, a match label single value must be a literal, or a `let`
 * binding whose initializer is itself a literal (no operator-based constant folding,
 * no propagation across multiple bindings). Range labels share the same rules and
 * are restricted to integer endpoints. */
typedef enum MatchConstKind {
    MATCH_CONST_INT = 0,
    MATCH_CONST_BOOL,
    MATCH_CONST_STRING
} MatchConstKind;

typedef struct MatchConstValue {
    MatchConstKind kind;
    int64_t i;
    bool b;
    FengSlice s;
    FengToken token;
} MatchConstValue;

static bool match_label_literal_from_literal_expr(const FengExpr *expr, MatchConstValue *out) {
    if (expr == NULL || out == NULL) {
        return false;
    }
    switch (expr->kind) {
        case FENG_EXPR_INTEGER:
            out->kind = MATCH_CONST_INT;
            out->i = expr->as.integer;
            out->token = expr->token;
            return true;
        case FENG_EXPR_BOOL:
            out->kind = MATCH_CONST_BOOL;
            out->b = expr->as.boolean;
            out->token = expr->token;
            return true;
        case FENG_EXPR_STRING:
            out->kind = MATCH_CONST_STRING;
            out->s = expr->as.string;
            out->token = expr->token;
            return true;
        case FENG_EXPR_UNARY:
            /* Allow `-INTEGER_LITERAL` as a single negative integer literal. */
            if (expr->as.unary.op == FENG_TOKEN_MINUS &&
                expr->as.unary.operand != NULL &&
                expr->as.unary.operand->kind == FENG_EXPR_INTEGER) {
                int64_t v = expr->as.unary.operand->as.integer;

                out->kind = MATCH_CONST_INT;
                /* Mirror unary-minus const-eval: negate via unsigned to avoid UB on
                 * INT64_MIN. */
                out->i = (int64_t)(-(uint64_t)v);
                out->token = expr->token;
                return true;
            }
            return false;
        default:
            return false;
    }
}

static bool extract_match_label_literal(ResolveContext *context,
                                        const FengExpr *expr,
                                        MatchConstValue *out) {
    if (expr == NULL) {
        return false;
    }
    if (match_label_literal_from_literal_expr(expr, out)) {
        return true;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        const LocalNameEntry *local =
            resolver_find_local_name_entry(context, expr->as.identifier);

        if (local != NULL) {
            if (local->mutability != FENG_MUTABILITY_LET || local->source_expr == NULL) {
                return false;
            }
            if (!match_label_literal_from_literal_expr(local->source_expr, out)) {
                return false;
            }
            out->token = expr->token;
            return true;
        }

        {
            const VisibleValueEntry *visible =
                find_visible_value(context->visible_values,
                                   context->visible_value_count,
                                   expr->as.identifier);

            if (visible == NULL || visible->is_function || visible->decl == NULL ||
                visible->decl->kind != FENG_DECL_GLOBAL_BINDING) {
                return false;
            }
            if (visible->decl->as.binding.mutability != FENG_MUTABILITY_LET ||
                visible->decl->as.binding.initializer == NULL) {
                return false;
            }
            if (!match_label_literal_from_literal_expr(visible->decl->as.binding.initializer, out)) {
                return false;
            }
            out->token = expr->token;
            return true;
        }
    }
    return false;
}

static bool match_target_type_is_allowed(InferredExprType target_type) {
    return inferred_expr_type_is_integer(target_type) ||
           inferred_expr_type_is_bool(target_type) ||
           inferred_expr_type_is_string(target_type);
}

static const char *match_const_kind_name(MatchConstKind kind) {
    switch (kind) {
        case MATCH_CONST_INT:
            return "integer";
        case MATCH_CONST_BOOL:
            return "bool";
        case MATCH_CONST_STRING:
            return "string";
    }
    return "unknown";
}

static bool match_const_kind_matches_target(MatchConstKind kind, InferredExprType target_type) {
    switch (kind) {
        case MATCH_CONST_INT:
            return inferred_expr_type_is_integer(target_type);
        case MATCH_CONST_BOOL:
            return inferred_expr_type_is_bool(target_type);
        case MATCH_CONST_STRING:
            return inferred_expr_type_is_string(target_type);
    }
    return false;
}

/* Aggregated label record used for cross-branch overlap detection. Single values are
 * stored as [v, v]; ranges are stored as [low, high]. The branch index is preserved
 * so that diagnostic messages can refer to "branch N" pairs. */
typedef struct MatchLabelRecord {
    size_t branch_index;
    FengToken token;
    MatchConstKind kind;
    /* INT: low/high are valid (low == high for single value). */
    int64_t low;
    int64_t high;
    /* BOOL */
    bool b;
    /* STRING */
    FengSlice s;
} MatchLabelRecord;

static bool slices_equal(FengSlice a, FengSlice b) {
    return a.length == b.length &&
           (a.length == 0U || memcmp(a.data, b.data, a.length) == 0);
}

static bool match_label_records_overlap(const MatchLabelRecord *a, const MatchLabelRecord *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case MATCH_CONST_INT:
            return !(a->high < b->low || b->high < a->low);
        case MATCH_CONST_BOOL:
            return a->b == b->b;
        case MATCH_CONST_STRING:
            return slices_equal(a->s, b->s);
    }
    return false;
}

static bool validate_match_label_record_target(ResolveContext *context,
                                               InferredExprType target_type,
                                               const MatchLabelRecord *record) {
    if (!match_const_kind_matches_target(record->kind, target_type)) {
        char *target_name = format_inferred_expr_type_name(target_type);
        char *message = format_message(
            "match label of type '%s' is not comparable with target type '%s'",
            match_const_kind_name(record->kind),
            target_name != NULL ? target_name : "<unknown>");

        free(target_name);
        return resolver_append_error(context, record->token, message);
    }
    return true;
}

static bool collect_match_branch_label_records(ResolveContext *context,
                                               const FengMatchBranch *branch,
                                               size_t branch_index,
                                               MatchLabelRecord **records,
                                               size_t *record_count,
                                               size_t *record_capacity) {
    size_t label_index;

    for (label_index = 0U; label_index < branch->label_count; ++label_index) {
        const FengMatchLabel *label = &branch->labels[label_index];
        MatchLabelRecord record;

        record.branch_index = branch_index;

        if (label->kind == FENG_MATCH_LABEL_RANGE) {
            MatchConstValue lo;
            MatchConstValue hi;

            if (!extract_match_label_literal(context, label->range_low, &lo) ||
                !extract_match_label_literal(context, label->range_high, &hi)) {
                return resolver_append_error(
                    context,
                    label->token,
                    format_message("match range label endpoints must be integer literals or 'let' bindings to integer literals"));
            }
            if (lo.kind != MATCH_CONST_INT || hi.kind != MATCH_CONST_INT) {
                return resolver_append_error(
                    context,
                    label->token,
                    format_message("match range label endpoints must be integer values"));
            }
            if (lo.i > hi.i) {
                return resolver_append_error(
                    context,
                    label->token,
                    format_message("match range label requires low <= high, got %lld and %lld",
                                   (long long)lo.i,
                                   (long long)hi.i));
            }
            record.token = label->token;
            record.kind = MATCH_CONST_INT;
            record.low = lo.i;
            record.high = hi.i;
        } else {
            MatchConstValue value;

            if (!extract_match_label_literal(context, label->value, &value)) {
                return resolver_append_error(
                    context,
                    label->value != NULL ? label->value->token : label->token,
                    format_message("match label must be a literal or a 'let' binding to a literal"));
            }
            record.token = value.token;
            record.kind = value.kind;
            switch (value.kind) {
                case MATCH_CONST_INT:
                    record.low = value.i;
                    record.high = value.i;
                    break;
                case MATCH_CONST_BOOL:
                    record.b = value.b;
                    break;
                case MATCH_CONST_STRING:
                    record.s = value.s;
                    break;
            }
        }

        {
            if (*record_count == *record_capacity) {
                size_t new_capacity = (*record_capacity == 0U) ? 4U : (*record_capacity * 2U);
                MatchLabelRecord *grown =
                    (MatchLabelRecord *)realloc(*records, new_capacity * sizeof(MatchLabelRecord));

                if (grown == NULL) {
                    return false;
                }
                *records = grown;
                *record_capacity = new_capacity;
            }
            (*records)[*record_count] = record;
            *record_count += 1U;
        }
    }
    return true;
}

static bool validate_match_label_records(ResolveContext *context,
                                         InferredExprType target_type,
                                         const MatchLabelRecord *records,
                                         size_t record_count) {
    size_t i;
    size_t j;

    for (i = 0U; i < record_count; ++i) {
        if (!validate_match_label_record_target(context, target_type, &records[i])) {
            return false;
        }
    }
    for (i = 0U; i < record_count; ++i) {
        for (j = i + 1U; j < record_count; ++j) {
            if (match_label_records_overlap(&records[i], &records[j])) {
                return resolver_append_error(
                    context,
                    records[j].token,
                    format_message("match label overlaps with an earlier label and is unreachable"));
            }
        }
    }
    return true;
}

static bool resolve_match_branch_body(ResolveContext *context,
                                      const FengMatchBranch *branch,
                                      bool allow_self) {
    if (branch == NULL || branch->body == NULL) {
        return true;
    }
    return resolve_block(context, branch->body, allow_self);
}

static bool resolve_and_validate_match_common(ResolveContext *context,
                                              const FengExpr *target,
                                              const FengMatchBranch *branches,
                                              size_t branch_count,
                                              const FengBlock *else_block,
                                              FengToken anchor,
                                              bool is_expression_form,
                                              bool allow_self) {
    InferredExprType target_type;
    MatchLabelRecord *records = NULL;
    size_t record_count = 0U;
    size_t record_capacity = 0U;
    size_t branch_index;
    bool ok = true;

    if (!resolve_expr(context, (FengExpr *)target, allow_self)) {
        return false;
    }

    target_type = infer_expr_type(context, target);
    if (inferred_expr_type_is_known(target_type) && !match_target_type_is_allowed(target_type)) {
        char *target_name = format_inferred_expr_type_name(target_type);
        char *message = format_message(
            "match target type '%s' is not allowed; allowed types are integers, 'string' and 'bool'",
            target_name != NULL ? target_name : "<unknown>");

        free(target_name);
        return resolver_append_error(context, target->token, message);
    }

    for (branch_index = 0U; branch_index < branch_count && ok; ++branch_index) {
        if (!collect_match_branch_label_records(context,
                                                &branches[branch_index],
                                                branch_index,
                                                &records,
                                                &record_count,
                                                &record_capacity)) {
            ok = false;
            break;
        }
        if (!resolve_match_branch_body(context, &branches[branch_index], allow_self)) {
            ok = false;
            break;
        }
        if (is_expression_form &&
            !validate_block_yields_expression(context,
                                              branches[branch_index].body,
                                              branches[branch_index].token,
                                              "match expression")) {
            ok = false;
            break;
        }
    }

    if (ok && inferred_expr_type_is_known(target_type)) {
        ok = validate_match_label_records(context, target_type, records, record_count);
    }

    free(records);

    if (!ok) {
        return false;
    }

    if (else_block != NULL) {
        if (!resolve_block(context, else_block, allow_self)) {
            return false;
        }
        if (is_expression_form &&
            !validate_block_yields_expression(context, else_block, anchor, "match expression else")) {
            return false;
        }
    } else if (is_expression_form) {
        return resolver_append_error(
            context,
            anchor,
            format_message("match expressions require an else branch"));
    }

    if (is_expression_form) {
        InferredExprType expected = block_yield_inferred_type(context, else_block);
        size_t i;

        if (!inferred_expr_type_is_known(expected)) {
            for (i = 0U; i < branch_count; ++i) {
                expected = block_yield_inferred_type(context, branches[i].body);
                if (inferred_expr_type_is_known(expected)) {
                    break;
                }
            }
        }
        if (inferred_expr_type_is_known(expected)) {
            for (i = 0U; i < branch_count; ++i) {
                InferredExprType branch_type =
                    block_yield_inferred_type(context, branches[i].body);

                if (!inferred_expr_type_is_known(branch_type)) {
                    continue;
                }
                if (!inferred_expr_types_equal(context, expected, branch_type)) {
                    char *expected_name = format_inferred_expr_type_name(expected);
                    char *branch_name = format_inferred_expr_type_name(branch_type);
                    char *message = format_message(
                        "match expression branches must have the same type, got '%s' and '%s'",
                        expected_name != NULL ? expected_name : "<unknown>",
                        branch_name != NULL ? branch_name : "<unknown>");

                    free(expected_name);
                    free(branch_name);
                    return resolver_append_error(context, branches[i].token, message);
                }
            }
        }
    }

    return true;
}

static bool type_ref_is_numeric(const FengTypeRef *type_ref) {
    const char *builtin_name = type_ref_builtin_canonical_name(type_ref);

    return builtin_name != NULL && builtin_type_name_is_numeric(slice_from_cstr(builtin_name));
}

static bool array_cast_writability_subset(const FengTypeRef *source,
                                          const FengTypeRef *target) {
    /* docs/feng-builtin-type.md §5: an array cast may STRIP `!` from any
     * layer but must never ADD `!`. Element type and depth must match. */
    if (source == NULL || target == NULL) {
        return false;
    }
    if (source->kind != target->kind) {
        return false;
    }
    if (source->kind != FENG_TYPE_REF_ARRAY) {
        /* Inner-most layer reached. Element types must match exactly. */
        return type_ref_equals(source, target);
    }
    if (target->array_element_writable && !source->array_element_writable) {
        return false;
    }
    return array_cast_writability_subset(source->as.inner, target->as.inner);
}

static bool cast_expr_types_are_valid(const ResolveContext *context,
                                      InferredExprType value_type,
                                      const FengTypeRef *target_type) {
    if (inferred_expr_type_matches_type_ref(context, value_type, target_type)) {
        return true;
    }
    if (inferred_expr_type_is_numeric(value_type) && type_ref_is_numeric(target_type)) {
        return true;
    }
    /* Array writability strip: source is a known array type, target is also
     * an array of identical element type and depth, and target's writability
     * mask is a subset of source's. */
    if (target_type != NULL && target_type->kind == FENG_TYPE_REF_ARRAY &&
        value_type.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
        value_type.type_ref != NULL &&
        value_type.type_ref->kind == FENG_TYPE_REF_ARRAY) {
        return array_cast_writability_subset(value_type.type_ref, target_type);
    }
    return false;
}

static bool validate_cast_expr(ResolveContext *context, const FengExpr *expr) {
    InferredExprType value_type;
    char *value_type_name;
    char *target_type_name;
    char *message;

    value_type = infer_expr_type(context, expr->as.cast.value);
    if (cast_expr_types_are_valid(context, value_type, expr->as.cast.type)) {
        return true;
    }

    value_type_name = format_inferred_expr_type_name(value_type);
    target_type_name = format_type_ref_name(expr->as.cast.type);
    message = format_message("cast from '%s' to '%s' is not allowed",
                             value_type_name != NULL ? value_type_name : "<unknown>",
                             target_type_name != NULL ? target_type_name : "<type>");
    free(value_type_name);
    free(target_type_name);
    return resolver_append_error(context, expr->token, message);
}

static bool validate_index_expr(ResolveContext *context, const FengExpr *expr) {
    InferredExprType object_type;
    char *object_type_name;
    InferredExprType index_type;
    char *index_type_name;
    char *message;

    if (resolve_indexed_array_element_type_ref(context, expr->as.index.object) == NULL) {
        object_type = infer_expr_type(context, expr->as.index.object);
        object_type_name = format_inferred_expr_type_name(object_type);
        message = format_message("index expression target must have array type, got '%s'",
                                 object_type_name != NULL ? object_type_name : "<unknown>");
        free(object_type_name);
        return resolver_append_error(context, expr->token, message) && false;
    }

    index_type = infer_expr_type(context, expr->as.index.index);
    if (inferred_expr_type_is_integer(index_type)) {
        return true;
    }

    index_type_name = format_inferred_expr_type_name(index_type);
    message = format_message("index expression requires an integer operand, got '%s'",
                             index_type_name != NULL ? index_type_name : "<unknown>");
    free(index_type_name);
    return resolver_append_error(context, expr->token, message) && false;
}

static bool validate_stmt_condition_expr(ResolveContext *context,
                                         FengToken token,
                                         const FengExpr *condition,
                                         const char *statement_kind) {
    InferredExprType condition_type;
    char *condition_type_name;
    char *message;

    if (condition == NULL) {
        return true;
    }

    condition_type = infer_expr_type(context, condition);
    if (inferred_expr_type_is_bool(condition_type)) {
        return true;
    }

    condition_type_name = format_inferred_expr_type_name(condition_type);
    message = format_message("%s condition must have type 'bool', got '%s'",
                             statement_kind,
                             condition_type_name != NULL ? condition_type_name : "<unknown>");
    free(condition_type_name);
    return resolver_append_error(context, token, message);
}

static bool validate_return_stmt(ResolveContext *context, const FengStmt *stmt) {
    InferredExprType return_type;
    char *expected_type_name;
    char *existing_type_name;
    char *return_type_name;
    char *message;

    if (context->current_callable_signature == NULL) {
        return true;
    }

    if (context->current_callable_member != NULL &&
        (context->current_callable_member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR ||
         context->current_callable_member->kind == FENG_TYPE_MEMBER_FINALIZER) &&
        stmt->as.return_value != NULL) {
        const char *member_kind_name =
            (context->current_callable_member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR)
                ? "constructor"
                : "finalizer";
        return resolver_append_error(
            context,
            stmt->token,
            format_message("%s body must use 'return;' without a value", member_kind_name));
    }

    if (context->current_callable_signature->return_type != NULL) {
        if (stmt->as.return_value == NULL) {
            if (type_ref_is_void(context->current_callable_signature->return_type)) {
                return true;
            }

            expected_type_name =
                format_type_ref_name(context->current_callable_signature->return_type);
            message = format_message("return statement does not match expected type '%s'",
                                     expected_type_name != NULL ? expected_type_name : "<type>");
            free(expected_type_name);
            return resolver_append_error(context, stmt->token, message) && false;
        }

        return validate_expr_against_expected_type(context,
                                                   stmt->as.return_value,
                                                   context->current_callable_signature->return_type);
    }

    context->current_callable_saw_return = true;
    return_type = stmt->as.return_value != NULL ? infer_expr_type(context, stmt->as.return_value)
                                                : inferred_expr_type_builtin("void");
    if (!inferred_expr_type_is_known(return_type)) {
        return true;
    }
    if (!inferred_expr_type_is_known(context->current_callable_inferred_return_type)) {
        context->current_callable_inferred_return_type = return_type;
        return true;
    }
    if (inferred_expr_types_equal(context,
                                  context->current_callable_inferred_return_type,
                                  return_type)) {
        return true;
    }

    existing_type_name =
        format_inferred_expr_type_name(context->current_callable_inferred_return_type);
    return_type_name = format_inferred_expr_type_name(return_type);
    message = format_message("callable '%.*s' has conflicting inferred return types '%s' and '%s'",
                             (int)context->current_callable_signature->name.length,
                             context->current_callable_signature->name.data,
                             existing_type_name != NULL ? existing_type_name : "<type>",
                             return_type_name != NULL ? return_type_name : "<type>");
    free(existing_type_name);
    free(return_type_name);
    return resolver_append_error(context, stmt->token, message) && false;
}

static const FengDecl *resolve_inferred_expr_type_decl(const ResolveContext *context,
                                                       InferredExprType expr_type) {
    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_DECL:
            return expr_type.type_decl;

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return resolve_type_ref_decl(context, expr_type.type_ref);

        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return NULL;
    }

    return NULL;
}

static bool record_type_fact_for_site(ResolveContext *context,
                                      const void *site,
                                      InferredExprType expr_type) {
    if (context == NULL || context->analysis == NULL || site == NULL ||
        !inferred_expr_type_is_known(expr_type)) {
        return true;
    }

    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
            return feng_semantic_record_type_fact(context->analysis,
                                                  site,
                                                  FENG_SEMANTIC_TYPE_FACT_BUILTIN,
                                                  expr_type.builtin_name,
                                                  NULL,
                                                  NULL);

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return feng_semantic_record_type_fact(context->analysis,
                                                  site,
                                                  FENG_SEMANTIC_TYPE_FACT_TYPE_REF,
                                                  (FengSlice){0},
                                                  expr_type.type_ref,
                                                  NULL);

        case FENG_INFERRED_EXPR_TYPE_DECL:
            return feng_semantic_record_type_fact(context->analysis,
                                                  site,
                                                  FENG_SEMANTIC_TYPE_FACT_DECL,
                                                  (FengSlice){0},
                                                  NULL,
                                                  expr_type.type_decl);

        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return true;
    }

    return true;
}

static const FengTypeMember *find_type_field_member(const FengDecl *type_decl, FengSlice name) {
    size_t member_index;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return NULL;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_FIELD && slice_equals(member->as.field.name, name)) {
            return member;
        }
    }

    return NULL;
}

static const FengTypeMember *find_type_let_field_member(const FengDecl *type_decl, FengSlice name) {
    const FengTypeMember *member = find_type_field_member(type_decl, name);

    if (member == NULL || member->as.field.mutability != FENG_MUTABILITY_LET) {
        return NULL;
    }

    return member;
}

static const FengTypeMember *find_instance_member(const FengDecl *type_decl, FengSlice name) {
    size_t member_index;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return NULL;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_FIELD && slice_equals(member->as.field.name, name)) {
            return member;
        }
        if (member->kind == FENG_TYPE_MEMBER_METHOD && slice_equals(member->as.callable.name, name)) {
            return member;
        }
    }

    return NULL;
}

/* Forward declaration: defined later in the spec satisfaction section. */
static bool spec_collect_closure(const ResolveContext *ctx,
                                 const FengDecl *spec_decl,
                                 const FengDecl ***out_set,
                                 size_t *out_count,
                                 size_t *out_capacity);

/* Find a field/method member declared on an object-form spec, walking parent specs.
 * Spec methods only carry signatures (no body); the returned member is suitable for
 * field-type inference, accessibility (spec members are always public), and
 * polymorphic call dispatch. Callable-form specs have no member surface and return
 * NULL here. */
static const FengTypeMember *find_spec_object_member(const ResolveContext *context,
                                                     const FengDecl *spec_decl,
                                                     FengSlice name) {
    const FengDecl **closure = NULL;
    size_t closure_count = 0U;
    size_t closure_capacity = 0U;
    const FengTypeMember *result = NULL;
    size_t i;
    size_t j;

    if (spec_decl == NULL || spec_decl->kind != FENG_DECL_SPEC ||
        spec_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
        return NULL;
    }
    if (!spec_collect_closure(context, spec_decl, &closure, &closure_count, &closure_capacity)) {
        free(closure);
        return NULL;
    }

    for (i = 0U; i < closure_count && result == NULL; ++i) {
        const FengDecl *current = closure[i];

        if (current->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            continue;
        }
        for (j = 0U; j < current->as.spec_decl.as.object.member_count; ++j) {
            const FengTypeMember *member = current->as.spec_decl.as.object.members[j];

            if (member->kind == FENG_TYPE_MEMBER_FIELD &&
                slice_equals(member->as.field.name, name)) {
                result = member;
                break;
            }
            if (member->kind == FENG_TYPE_MEMBER_METHOD &&
                slice_equals(member->as.callable.name, name)) {
                result = member;
                break;
            }
        }
    }

    free(closure);
    return result;
}

/* Find a member on either a concrete type or an object-form spec. Returns NULL
 * for callable-form specs and other declaration kinds. */
static const FengTypeMember *find_decl_instance_member(const ResolveContext *context,
                                                       const FengDecl *decl,
                                                       FengSlice name) {
    if (decl == NULL) {
        return NULL;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        return find_instance_member(decl, name);
    }
    if (decl->kind == FENG_DECL_SPEC) {
        return find_spec_object_member(context, decl, name);
    }
    return NULL;
}

static bool field_has_declaration_initializer(const FengTypeMember *member) {
    return member != NULL && member->kind == FENG_TYPE_MEMBER_FIELD &&
           member->as.field.initializer != NULL;
}

static bool expr_is_direct_self_member(const FengExpr *expr, FengSlice *out_name) {
    if (expr == NULL || expr->kind != FENG_EXPR_MEMBER || expr->as.member.object == NULL ||
        expr->as.member.object->kind != FENG_EXPR_SELF) {
        return false;
    }

    if (out_name != NULL) {
        *out_name = expr->as.member.member;
    }
    return true;
}

static const FengTypeMember *find_direct_self_let_target_member(const FengDecl *type_decl,
                                                                const FengExpr *expr) {
    FengSlice name;

    if (!expr_is_direct_self_member(expr, &name)) {
        return NULL;
    }

    return find_type_let_field_member(type_decl, name);
}

static bool type_member_is_public(const FengTypeMember *member) {
    return member != NULL && member->visibility != FENG_VISIBILITY_PRIVATE;
}

/* When the resolver is inside a fit-block function body, accessing the target
 * type's private members (`pr` fields or `pr` methods) is forbidden, regardless
 * of whether the target lives in the same package as the fit declaration.
 * Members contributed by the fit block itself live on the fit decl, not on the
 * target type, so this helper only consults the target type's own member set.
 * See docs/feng-fit.md §4 / §5. */
static bool fit_body_blocks_private_access(const ResolveContext *context,
                                           const FengDecl *owner_type_decl,
                                           const FengTypeMember *member) {
    const FengDecl *fit_target;

    if (context == NULL || context->current_fit_decl == NULL ||
        owner_type_decl == NULL || member == NULL ||
        type_member_is_public(member)) {
        return false;
    }
    if (owner_type_decl->kind != FENG_DECL_TYPE) {
        return false;
    }
    fit_target = resolve_type_ref_decl(context,
                                       context->current_fit_decl->as.fit_decl.target);
    return fit_target != NULL && fit_target == owner_type_decl;
}

static bool type_member_is_accessible_from(const ResolveContext *context,
                                           const FengSemanticModule *provider_module,
                                           const FengTypeMember *member) {
    return member != NULL &&
           (provider_module == NULL || provider_module == context->module ||
            type_member_is_public(member));
}

static size_t count_declared_constructors(const FengDecl *type_decl) {
    size_t member_index;
    size_t count = 0U;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return 0U;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        if (type_decl->as.type_decl.members[member_index]->kind ==
            FENG_TYPE_MEMBER_CONSTRUCTOR) {
            ++count;
        }
    }

    return count;
}

static bool validate_type_finalizer_constraints(ResolveContext *context, const FengDecl *type_decl) {
    size_t member_index;
    const FengTypeMember *first_finalizer = NULL;
    bool type_is_fixed;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return true;
    }

    type_is_fixed = annotations_contain_kind(type_decl->annotations,
                                             type_decl->annotation_count,
                                             FENG_ANNOTATION_FIXED);

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_FINALIZER) {
            continue;
        }

        if (type_is_fixed) {
            return resolver_append_error(
                context,
                member->token,
                format_message(
                    "type '%.*s' is marked as @fixed and cannot declare a finalizer",
                    (int)decl_typeish_name(type_decl).length,
                    decl_typeish_name(type_decl).data));
        }

        if (first_finalizer != NULL) {
            return resolver_append_error(
                context,
                member->token,
                format_message(
                    "type '%.*s' declares more than one finalizer",
                    (int)decl_typeish_name(type_decl).length,
                    decl_typeish_name(type_decl).data));
        }

        first_finalizer = member;
    }

    return true;
}

static bool function_type_decl_is_fixed_abi_callable_type(const FengDecl *function_type_decl) {
    return function_type_decl != NULL && decl_is_function_type(function_type_decl) &&
           annotations_contain_kind(function_type_decl->annotations,
                                    function_type_decl->annotation_count,
                                    FENG_ANNOTATION_FIXED);
}

static bool function_decl_is_fixed_abi_callable_value(const FengDecl *decl) {
    return decl != NULL && decl->kind == FENG_DECL_FUNCTION &&
           (decl->is_extern ||
            annotations_contain_kind(decl->annotations,
                                     decl->annotation_count,
                                     FENG_ANNOTATION_FIXED));
}

static bool method_member_is_fixed_abi_callable_value(const FengTypeMember *member) {
    return member != NULL && member->kind == FENG_TYPE_MEMBER_METHOD &&
           annotations_contain_kind(member->annotations,
                                    member->annotation_count,
                                    FENG_ANNOTATION_FIXED);
}

static bool inferred_expr_type_can_match_fixed_abi_callable_type(InferredExprType type) {
    return type.kind != FENG_INFERRED_EXPR_TYPE_LAMBDA;
}

static bool current_callable_is_inside_exception_handler(const ResolveContext *context) {
    return context != NULL && context->exception_capture_depth > 0U;
}

static void note_current_callable_exception_escape(ResolveContext *context) {
    if (context == NULL || context->current_callable_signature == NULL ||
        current_callable_is_inside_exception_handler(context)) {
        return;
    }

    context->current_callable_has_escaping_exception = true;
}

static void note_callable_exception_escape(ResolveContext *context,
                                          const FengCallableSignature *callable) {
    if (!callable_may_escape_exception(context, callable)) {
        return;
    }

    note_current_callable_exception_escape(context);
}

static bool lambda_expr_may_escape_exception(ResolveContext *context, const FengExpr *expr) {
    const FengCallableSignature *previous_callable_signature;
    bool previous_callable_has_escaping_exception;
    bool escapes = false;
    size_t param_index;
    bool ok = true;

    if (context == NULL || expr == NULL || expr->kind != FENG_EXPR_LAMBDA) {
        return false;
    }

    if (!resolver_push_scope(context)) {
        return false;
    }

    for (param_index = 0U; param_index < expr->as.lambda.param_count && ok; ++param_index) {
        ok = resolver_add_local_typed_name(
            context,
            expr->as.lambda.params[param_index].name,
            inferred_expr_type_from_type_ref(expr->as.lambda.params[param_index].type),
            expr->as.lambda.params[param_index].mutability);
    }

    previous_callable_signature = context->current_callable_signature;
    previous_callable_has_escaping_exception = context->current_callable_has_escaping_exception;
    context->current_callable_has_escaping_exception = false;
    if (ok) {
        if (expr->as.lambda.is_block_body) {
            ok = resolve_block_contents(context, expr->as.lambda.body_block, context->self_capturable);
        } else {
            ok = resolve_expr(context, expr->as.lambda.body, context->self_capturable);
        }
    }
    escapes = ok && context->current_callable_has_escaping_exception;
    context->current_callable_signature = previous_callable_signature;
    context->current_callable_has_escaping_exception = previous_callable_has_escaping_exception;

    resolver_pop_scope(context);
    return escapes;
}

static bool callable_value_expr_may_escape_exception(ResolveContext *context,
                                                     const FengExpr *expr,
                                                     const FengTypeRef *expected_type_ref,
                                                     size_t depth) {
    CallableValueResolution resolution;

    if (context == NULL || expr == NULL || depth > 32U) {
        return false;
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const LocalNameEntry *local_entry =
                resolver_find_local_name_entry(context, expr->as.identifier);

            if (local_entry != NULL) {
                const FengTypeRef *source_type_ref =
                    local_entry->type.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF
                        ? local_entry->type.type_ref
                        : expected_type_ref;

                if (local_entry->type.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA) {
                    return lambda_expr_may_escape_exception(context, local_entry->type.lambda_expr);
                }
                if (local_entry->source_expr != NULL) {
                    return callable_value_expr_may_escape_exception(
                        context, local_entry->source_expr, source_type_ref, depth + 1U);
                }
                return false;
            }

            {
                const VisibleValueEntry *visible_value =
                    find_visible_value(context->visible_values,
                                       context->visible_value_count,
                                       expr->as.identifier);

                if (visible_value != NULL && !visible_value->is_function && visible_value->decl != NULL &&
                    visible_value->decl->kind == FENG_DECL_GLOBAL_BINDING &&
                    visible_value->decl->as.binding.initializer != NULL) {
                    const FengTypeRef *source_type_ref = visible_value->decl->as.binding.type != NULL
                                                             ? visible_value->decl->as.binding.type
                                                             : expected_type_ref;

                    return callable_value_expr_may_escape_exception(context,
                                                                    visible_value->decl->as.binding.initializer,
                                                                    source_type_ref,
                                                                    depth + 1U);
                }
            }
            break;
        }

        case FENG_EXPR_MEMBER: {
            const FengExpr *object = expr->as.member.object;

            if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
                const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

                if (alias != NULL) {
                    const FengDecl *binding_decl =
                        find_module_public_binding_decl(alias->target_module, expr->as.member.member);

                    if (binding_decl != NULL && binding_decl->kind == FENG_DECL_GLOBAL_BINDING &&
                        binding_decl->as.binding.initializer != NULL) {
                        const FengTypeRef *source_type_ref = binding_decl->as.binding.type != NULL
                                                                 ? binding_decl->as.binding.type
                                                                 : expected_type_ref;

                        return callable_value_expr_may_escape_exception(
                            context,
                            binding_decl->as.binding.initializer,
                            source_type_ref,
                            depth + 1U);
                    }
                }
            }
            break;
        }

        case FENG_EXPR_LAMBDA:
            return lambda_expr_may_escape_exception(context, expr);

        case FENG_EXPR_IF: {
            const FengExpr *then_yield = block_yield_expression(expr->as.if_expr.then_block);
            const FengExpr *else_yield = block_yield_expression(expr->as.if_expr.else_block);

            return (then_yield != NULL && callable_value_expr_may_escape_exception(
                                              context, then_yield, expected_type_ref, depth + 1U)) ||
                   (else_yield != NULL && callable_value_expr_may_escape_exception(
                                              context, else_yield, expected_type_ref, depth + 1U));
        }

        case FENG_EXPR_MATCH: {
            size_t branch_index;
            const FengExpr *else_yield = block_yield_expression(expr->as.match_expr.else_block);

            for (branch_index = 0U; branch_index < expr->as.match_expr.branch_count; ++branch_index) {
                const FengExpr *branch_yield =
                    block_yield_expression(expr->as.match_expr.branches[branch_index].body);

                if (branch_yield != NULL && callable_value_expr_may_escape_exception(
                                                context, branch_yield, expected_type_ref, depth + 1U)) {
                    return true;
                }
            }

            return else_yield != NULL && callable_value_expr_may_escape_exception(
                                             context, else_yield, expected_type_ref, depth + 1U);
        }

        case FENG_EXPR_CAST:
            return callable_value_expr_may_escape_exception(
                context, expr->as.cast.value, expected_type_ref, depth + 1U);

        default:
            break;
    }

    if (expected_type_ref == NULL) {
        return false;
    }

    resolution = resolve_expr_callable_value(context, expr, expected_type_ref);
    if (resolution.kind != FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE) {
        return false;
    }
    if (resolution.callable != NULL) {
        return callable_may_escape_exception(context, resolution.callable);
    }
    if (resolution.lambda_expr != NULL) {
        return lambda_expr_may_escape_exception(context, resolution.lambda_expr);
    }

    return false;
}

static void note_callable_value_expr_exception_escape(ResolveContext *context,
                                                      const FengExpr *callee) {
    InferredExprType callee_type;
    const FengTypeRef *expected_type_ref = NULL;

    if (context == NULL || callee == NULL) {
        return;
    }

    callee_type = infer_expr_type(context, callee);
    if (callee_type.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA) {
        if (lambda_expr_may_escape_exception(context, callee_type.lambda_expr)) {
            note_current_callable_exception_escape(context);
        }
        return;
    }

    if (callee_type.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF) {
        expected_type_ref = callee_type.type_ref;
    }

    if (callable_value_expr_may_escape_exception(context, callee, expected_type_ref, 0U)) {
        note_current_callable_exception_escape(context);
    }
}

static bool callable_parameters_match_args(ResolveContext *context,
                                           const FengCallableSignature *callable,
                                           FengExpr *const *args,
                                           size_t arg_count) {
    size_t arg_index;

    if (callable->param_count != arg_count) {
        return false;
    }

    for (arg_index = 0U; arg_index < arg_count; ++arg_index) {
        if (!expr_matches_expected_type_ref(context,
                                            args[arg_index],
                                            callable->params[arg_index].type)) {
            return false;
        }
    }

    return true;
}

static bool current_stmt_is_inside_finally(const ResolveContext *context) {
    return context != NULL && context->finally_depth > 0U;
}

static bool validate_finally_forbidden_control_stmt(ResolveContext *context,
                                                    const FengStmt *stmt,
                                                    const char *keyword) {
    if (!current_stmt_is_inside_finally(context)) {
        return true;
    }

    return resolver_append_error(
        context,
        stmt != NULL ? stmt->token : context->program->module_token,
        format_message("finally blocks cannot contain '%s'", keyword));
}

static bool validate_loop_control_stmt(ResolveContext *context,
                                       const FengStmt *stmt,
                                       const char *keyword) {
    if (!validate_finally_forbidden_control_stmt(context, stmt, keyword)) {
        return false;
    }

    if (context != NULL && context->loop_depth > 0U) {
        return true;
    }

    return resolver_append_error(
        context,
        stmt != NULL ? stmt->token : context->program->module_token,
        format_message("'%s' statement is only allowed inside a 'while' or 'for' loop",
                       keyword));
}

static bool inferred_expr_type_is_throwable(const ResolveContext *context,
                                            InferredExprType type,
                                            const char **out_reason);

static bool type_ref_is_throwable(const ResolveContext *context,
                                  const FengTypeRef *type_ref,
                                  const char **out_reason) {
    const FengDecl *decl;

    if (type_ref == NULL) {
        return true;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_POINTER:
            if (out_reason != NULL) {
                *out_reason = "pointer values cannot be thrown as exceptions";
            }
            return false;

        case FENG_TYPE_REF_ARRAY:
            /* Element type does not affect throwability of the array value
             * itself: arrays are always Feng-managed. */
            return true;

        case FENG_TYPE_REF_NAMED:
            decl = resolve_type_ref_decl(context, type_ref);
            if (decl != NULL && decl->kind == FENG_DECL_TYPE &&
                annotations_contain_kind(decl->annotations,
                                         decl->annotation_count,
                                         FENG_ANNOTATION_FIXED)) {
                if (out_reason != NULL) {
                    *out_reason = "@fixed types are ABI-bound and cannot be thrown as exceptions";
                }
                return false;
            }
            return true;
    }

    return true;
}

static bool inferred_expr_type_is_throwable(const ResolveContext *context,
                                            InferredExprType type,
                                            const char **out_reason) {
    switch (type.kind) {
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            /* Unknown types are reported through other diagnostics; do not
             * pile on a second throwability error here. */
            return true;

        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
            return true;

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return type_ref_is_throwable(context, type.type_ref, out_reason);

        case FENG_INFERRED_EXPR_TYPE_DECL:
            if (type.type_decl != NULL && type.type_decl->kind == FENG_DECL_TYPE &&
                annotations_contain_kind(type.type_decl->annotations,
                                         type.type_decl->annotation_count,
                                         FENG_ANNOTATION_FIXED)) {
                if (out_reason != NULL) {
                    *out_reason = "@fixed types are ABI-bound and cannot be thrown as exceptions";
                }
                return false;
            }
            return true;
    }

    return true;
}

static bool validate_throw_stmt(ResolveContext *context, const FengStmt *stmt) {
    InferredExprType throw_type;
    const char *reason = NULL;

    if (!validate_finally_forbidden_control_stmt(context, stmt, "throw")) {
        return false;
    }

    throw_type = infer_expr_type(context, stmt != NULL ? stmt->as.throw_value : NULL);

    if (inferred_expr_type_is_known(throw_type) && inferred_expr_type_is_void(throw_type)) {
        return resolver_append_error(
            context,
            stmt->token,
            format_message("throw statement requires a non-void expression"));
    }

    if (!inferred_expr_type_is_throwable(context, throw_type, &reason)) {
        char *type_name = format_inferred_expr_type_name(throw_type);
        char *message = format_message(
            "throw expression of type '%s' is not throwable: %s",
            type_name != NULL ? type_name : "<unknown>",
            reason != NULL ? reason : "value is not a Feng-managed value");

        free(type_name);
        return resolver_append_error(context, stmt->token, message);
    }

    return true;
}


static ConstructorResolution resolve_accessible_constructor_overload(
    ResolveContext *context,
    const FengDecl *type_decl,
    const FengSemanticModule *provider_module,
    FengExpr *const *args,
    size_t arg_count) {
    size_t member_index;
    ConstructorResolution result;

    memset(&result, 0, sizeof(result));

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return result;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_CONSTRUCTOR) {
            continue;
        }
        if (!type_member_is_accessible_from(context, provider_module, member) ||
            fit_body_blocks_private_access(context, type_decl, member)) {
            continue;
        }
        if (!callable_parameters_match_args(
                context, &member->as.callable, args, arg_count)) {
            continue;
        }

        if (result.kind == FENG_CONSTRUCTOR_RESOLUTION_NONE) {
            result.kind = FENG_CONSTRUCTOR_RESOLUTION_UNIQUE;
            result.constructor = member;
            continue;
        }

        result.kind = FENG_CONSTRUCTOR_RESOLUTION_AMBIGUOUS;
        result.constructor = NULL;
    }

    return result;
}

static FunctionCallResolution resolve_top_level_function_overload(
    ResolveContext *context,
    const FunctionOverloadSetEntry *overload_set,
    FengExpr *const *args,
    size_t arg_count) {
    size_t decl_index;
    FunctionCallResolution result;

    memset(&result, 0, sizeof(result));
    if (overload_set == NULL) {
        return result;
    }

    for (decl_index = 0U; decl_index < overload_set->decl_count; ++decl_index) {
        const FengDecl *decl = overload_set->decls[decl_index];

        if (decl == NULL || decl->kind != FENG_DECL_FUNCTION) {
            continue;
        }
        if (!callable_parameters_match_args(context,
                                            &decl->as.function_decl,
                                            args,
                                            arg_count)) {
            continue;
        }

        if (result.kind == FENG_FUNCTION_CALL_RESOLUTION_NONE) {
            result.kind = FENG_FUNCTION_CALL_RESOLUTION_UNIQUE;
            result.decl = decl;
            result.callable = &decl->as.function_decl;
            continue;
        }

        result.kind = FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS;
        result.decl = NULL;
        result.callable = NULL;
    }

    return result;
}

static CallableValueResolution resolve_top_level_function_value_overload(
    ResolveContext *context,
    const FunctionOverloadSetEntry *overload_set,
    const FengDecl *function_type_decl) {
    size_t decl_index;
    CallableValueResolution result;
    bool requires_fixed_abi_callable;

    memset(&result, 0, sizeof(result));
    if (overload_set == NULL || function_type_decl == NULL) {
        return result;
    }

    requires_fixed_abi_callable =
        function_type_decl_is_fixed_abi_callable_type(function_type_decl);

    for (decl_index = 0U; decl_index < overload_set->decl_count; ++decl_index) {
        const FengDecl *decl = overload_set->decls[decl_index];

        if (decl == NULL || decl->kind != FENG_DECL_FUNCTION ||
            (requires_fixed_abi_callable && !function_decl_is_fixed_abi_callable_value(decl)) ||
            !function_type_decl_matches_callable_signature_or_is_pending(context,
                                                                         function_type_decl,
                                                                         &decl->as.function_decl)) {
            continue;
        }

        if (result.kind == FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
            result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            result.callable = &decl->as.function_decl;
            result.lambda_expr = NULL;
            continue;
        }

        result.kind = FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS;
        result.callable = NULL;
        result.lambda_expr = NULL;
    }

    return result;
}

static CallableValueResolution resolve_module_public_function_value_overload(
    ResolveContext *context,
    const FengSemanticModule *module,
    FengSlice name,
    const FengDecl *function_type_decl) {
    size_t program_index;
    CallableValueResolution result;
    bool requires_fixed_abi_callable;

    memset(&result, 0, sizeof(result));
    if (module == NULL || function_type_decl == NULL) {
        return result;
    }

    requires_fixed_abi_callable =
        function_type_decl_is_fixed_abi_callable_type(function_type_decl);

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind != FENG_DECL_FUNCTION || !decl_is_public(decl) ||
                !slice_equals(decl->as.function_decl.name, name) ||
                (requires_fixed_abi_callable && !function_decl_is_fixed_abi_callable_value(decl)) ||
                !function_type_decl_matches_callable_signature_or_is_pending(
                    context,
                    function_type_decl,
                    &decl->as.function_decl)) {
                continue;
            }

            if (result.kind == FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
                result.callable = &decl->as.function_decl;
                result.lambda_expr = NULL;
                continue;
            }

            result.kind = FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS;
            result.callable = NULL;
            result.lambda_expr = NULL;
        }
    }

    return result;
}

static CallableValueResolution resolve_accessible_method_value_overload(
    ResolveContext *context,
    const FengDecl *type_decl,
    const FengSemanticModule *provider_module,
    FengSlice name,
    const FengDecl *function_type_decl) {
    size_t member_index;
    CallableValueResolution result;
    bool requires_fixed_abi_callable;

    memset(&result, 0, sizeof(result));
    if (type_decl == NULL || function_type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return result;
    }

    requires_fixed_abi_callable =
        function_type_decl_is_fixed_abi_callable_type(function_type_decl);

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_METHOD ||
            !slice_equals(member->as.callable.name, name) ||
            !type_member_is_accessible_from(context, provider_module, member) ||
            fit_body_blocks_private_access(context, type_decl, member) ||
            (requires_fixed_abi_callable && !method_member_is_fixed_abi_callable_value(member)) ||
            !function_type_decl_matches_callable_signature_or_is_pending(context,
                                                                         function_type_decl,
                                                                         &member->as.callable)) {
            continue;
        }

        if (result.kind == FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
            result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            result.callable = &member->as.callable;
            result.lambda_expr = NULL;
            continue;
        }

        result.kind = FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS;
        result.callable = NULL;
        result.lambda_expr = NULL;
    }

    return result;
}

static bool function_type_parameters_match_args(ResolveContext *context,
                                                const FengDecl *type_decl,
                                                FengExpr *const *args,
                                                size_t arg_count) {
    size_t arg_index;

    if (!decl_is_function_type(type_decl)) {
        return false;
    }
    if (type_decl->as.spec_decl.as.callable.param_count != arg_count) {
        return false;
    }

    for (arg_index = 0U; arg_index < arg_count; ++arg_index) {
        if (!expr_matches_expected_type_ref(context,
                                            args[arg_index],
                                            type_decl->as.spec_decl.as.callable.params[arg_index].type)) {
            return false;
        }
    }

    return true;
}

static FunctionCallResolution resolve_module_public_function_overload(
    ResolveContext *context,
    const FengSemanticModule *module,
    FengSlice name,
    FengExpr *const *args,
    size_t arg_count) {
    size_t program_index;
    FunctionCallResolution result;

    memset(&result, 0, sizeof(result));
    if (module == NULL) {
        return result;
    }

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind != FENG_DECL_FUNCTION || !decl_is_public(decl) ||
                !slice_equals(decl->as.function_decl.name, name)) {
                continue;
            }
            if (!callable_parameters_match_args(context,
                                                &decl->as.function_decl,
                                                args,
                                                arg_count)) {
                continue;
            }

            if (result.kind == FENG_FUNCTION_CALL_RESOLUTION_NONE) {
                result.kind = FENG_FUNCTION_CALL_RESOLUTION_UNIQUE;
                result.decl = decl;
                result.callable = &decl->as.function_decl;
                continue;
            }

            result.kind = FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS;
            result.decl = NULL;
            result.callable = NULL;
        }
    }

    return result;
}

static const FengTypeMember *find_type_method_member(const FengDecl *type_decl, FengSlice name) {
    size_t member_index;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return NULL;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_METHOD && slice_equals(member->as.callable.name, name)) {
            return member;
        }
    }

    return NULL;
}

/* Visit every fit-body method member that targets `type_decl` and is visible from
 * the current resolve context. Callback returns false to stop iteration; iteration
 * function returns false only when stopped by the callback. */
typedef bool (*FitMethodVisitor)(const FengTypeMember *member,
                                 const FengSemanticModule *fit_module,
                                 const FengDecl *fit_decl,
                                 void *userdata);

static bool fit_decl_is_visible_from(const ResolveContext *ctx,
                                     const FengSemanticModule *fit_module,
                                     const FengDecl *fit_decl) {
    if (ctx == NULL || fit_module == NULL || fit_decl == NULL) {
        return false;
    }
    if (fit_module == ctx->module) {
        return true;
    }
    /* Cross-module fits become effective in the consumer only when (a) the
     * fit itself is `pu fit`, and (b) the consumer file imported the fit's
     * owning module via `use`. Mirrors docs/feng-fit.md §4: "其他 mod 通过
     * use 引入当前模块后，该契约关系在其作用域内生效". */
    return fit_decl->visibility == FENG_VISIBILITY_PUBLIC &&
           module_is_use_visible_from(ctx, fit_module);
}

static bool visit_visible_fit_methods_for_type(const ResolveContext *ctx,
                                               const FengDecl *type_decl,
                                               FengSlice name,
                                               bool require_name_match,
                                               FitMethodVisitor visitor,
                                               void *userdata) {
    size_t module_index;
    size_t program_index;
    size_t decl_index;
    size_t member_index;

    if (ctx == NULL || ctx->analysis == NULL || type_decl == NULL ||
        type_decl->kind != FENG_DECL_TYPE) {
        return true;
    }

    for (module_index = 0U; module_index < ctx->analysis->module_count; ++module_index) {
        const FengSemanticModule *m = &ctx->analysis->modules[module_index];

        for (program_index = 0U; program_index < m->program_count; ++program_index) {
            const FengProgram *prog = m->programs[program_index];

            if (prog == NULL) {
                continue;
            }
            for (decl_index = 0U; decl_index < prog->declaration_count; ++decl_index) {
                const FengDecl *fd = prog->declarations[decl_index];

                if (fd == NULL || fd->kind != FENG_DECL_FIT) {
                    continue;
                }
                if (!fit_decl_is_visible_from(ctx, m, fd)) {
                    continue;
                }
                if (resolve_type_ref_decl(ctx, fd->as.fit_decl.target) != type_decl) {
                    continue;
                }
                for (member_index = 0U; member_index < fd->as.fit_decl.member_count; ++member_index) {
                    const FengTypeMember *member = fd->as.fit_decl.members[member_index];

                    if (member == NULL || member->kind != FENG_TYPE_MEMBER_METHOD) {
                        continue;
                    }
                    if (require_name_match &&
                        !slice_equals(member->as.callable.name, name)) {
                        continue;
                    }
                    if (!visitor(member, m, fd, userdata)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

typedef struct FitFirstMethodCtx {
    const FengTypeMember *result;
} FitFirstMethodCtx;

static bool fit_first_method_visitor(const FengTypeMember *member,
                                     const FengSemanticModule *fit_module,
                                     const FengDecl *fit_decl,
                                     void *userdata) {
    FitFirstMethodCtx *st = (FitFirstMethodCtx *)userdata;

    (void)fit_module;
    (void)fit_decl;
    st->result = member;
    return false;
}

static const FengTypeMember *find_fit_method_member_for_type(const ResolveContext *ctx,
                                                             const FengDecl *type_decl,
                                                             FengSlice name) {
    FitFirstMethodCtx st;

    st.result = NULL;
    (void)visit_visible_fit_methods_for_type(ctx, type_decl, name, true,
                                             fit_first_method_visitor, &st);
    return st.result;
}

static const FengTypeMember *find_accessible_type_field_member(ResolveContext *context,
                                                               const FengDecl *type_decl,
                                                               const FengSemanticModule *provider_module,
                                                               FengSlice name) {
    const FengTypeMember *member = find_type_field_member(type_decl, name);

    if (!type_member_is_accessible_from(context, provider_module, member)) {
        return NULL;
    }
    if (fit_body_blocks_private_access(context, type_decl, member)) {
        return NULL;
    }
    return member;
}

static const FengTypeMember *find_accessible_type_method_member(ResolveContext *context,
                                                                const FengDecl *type_decl,
                                                                const FengSemanticModule *provider_module,
                                                                FengSlice name) {
    const FengTypeMember *member = find_type_method_member(type_decl, name);

    if (type_member_is_accessible_from(context, provider_module, member) &&
        !fit_body_blocks_private_access(context, type_decl, member)) {
        return member;
    }
    /* Fall back to any visible fit-body method for this type. */
    return find_fit_method_member_for_type(context, type_decl, name);
}

typedef struct FitMethodCountCtx {
    size_t count;
} FitMethodCountCtx;

static bool fit_method_count_visitor(const FengTypeMember *member,
                                     const FengSemanticModule *fit_module,
                                     const FengDecl *fit_decl,
                                     void *userdata) {
    FitMethodCountCtx *st = (FitMethodCountCtx *)userdata;

    (void)member;
    (void)fit_module;
    (void)fit_decl;
    ++st->count;
    return true;
}

static size_t count_accessible_method_overloads(ResolveContext *context,
                                                const FengDecl *type_decl,
                                                const FengSemanticModule *provider_module,
                                                FengSlice name) {
    size_t member_index;
    size_t count = 0U;
    FitMethodCountCtx st;

    if (type_decl == NULL) {
        return 0U;
    }

    if (type_decl->kind == FENG_DECL_SPEC) {
        const FengDecl **closure = NULL;
        size_t closure_count = 0U;
        size_t closure_capacity = 0U;
        size_t i;
        size_t j;

        if (type_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            return 0U;
        }
        if (!spec_collect_closure(context, type_decl, &closure, &closure_count, &closure_capacity)) {
            free(closure);
            return 0U;
        }
        for (i = 0U; i < closure_count; ++i) {
            const FengDecl *current = closure[i];

            if (current->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
                continue;
            }
            for (j = 0U; j < current->as.spec_decl.as.object.member_count; ++j) {
                const FengTypeMember *member = current->as.spec_decl.as.object.members[j];

                if (member->kind == FENG_TYPE_MEMBER_METHOD &&
                    slice_equals(member->as.callable.name, name)) {
                    ++count;
                }
            }
        }
        free(closure);
        return count;
    }

    if (type_decl->kind != FENG_DECL_TYPE) {
        return 0U;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_METHOD &&
            slice_equals(member->as.callable.name, name) &&
            type_member_is_accessible_from(context, provider_module, member) &&
            !fit_body_blocks_private_access(context, type_decl, member)) {
            ++count;
        }
    }

    st.count = 0U;
    (void)visit_visible_fit_methods_for_type(context, type_decl, name, true,
                                             fit_method_count_visitor, &st);
    return count + st.count;
}

typedef struct FitOverloadResolveCtx {
    ResolveContext *context;
    const FengDecl *owner_type_decl;
    FengExpr *const *args;
    size_t arg_count;
    FunctionCallResolution result;
} FitOverloadResolveCtx;

static bool fit_overload_resolve_visitor(const FengTypeMember *member,
                                         const FengSemanticModule *fit_module,
                                         const FengDecl *fit_decl,
                                         void *userdata) {
    FitOverloadResolveCtx *st = (FitOverloadResolveCtx *)userdata;

    (void)fit_module;
    if (!callable_parameters_match_args(st->context, &member->as.callable,
                                        st->args, st->arg_count)) {
        return true;
    }
    if (st->result.kind == FENG_FUNCTION_CALL_RESOLUTION_NONE) {
        st->result.kind = FENG_FUNCTION_CALL_RESOLUTION_UNIQUE;
        st->result.decl = NULL;
        st->result.callable = &member->as.callable;
        st->result.member = member;
        st->result.owner_type_decl = st->owner_type_decl;
        st->result.fit_decl = fit_decl;
        return true;
    }
    st->result.kind = FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS;
    st->result.callable = NULL;
    st->result.member = NULL;
    st->result.owner_type_decl = NULL;
    st->result.fit_decl = NULL;
    return true;
}

static FunctionCallResolution resolve_accessible_method_overload(
    ResolveContext *context,
    const FengDecl *type_decl,
    const FengSemanticModule *provider_module,
    FengSlice name,
    FengExpr *const *args,
    size_t arg_count) {
    size_t member_index;
    FunctionCallResolution result;
    FitOverloadResolveCtx st;

    memset(&result, 0, sizeof(result));
    if (type_decl == NULL) {
        return result;
    }

    if (type_decl->kind == FENG_DECL_SPEC) {
        const FengDecl **closure = NULL;
        size_t closure_count = 0U;
        size_t closure_capacity = 0U;
        size_t i;
        size_t j;

        if (type_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            return result;
        }
        if (!spec_collect_closure(context, type_decl, &closure, &closure_count, &closure_capacity)) {
            free(closure);
            return result;
        }
        for (i = 0U; i < closure_count; ++i) {
            const FengDecl *current = closure[i];

            if (current->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
                continue;
            }
            for (j = 0U; j < current->as.spec_decl.as.object.member_count; ++j) {
                const FengTypeMember *member = current->as.spec_decl.as.object.members[j];

                if (member->kind != FENG_TYPE_MEMBER_METHOD ||
                    !slice_equals(member->as.callable.name, name) ||
                    !callable_parameters_match_args(context, &member->as.callable, args, arg_count)) {
                    continue;
                }
                if (result.kind == FENG_FUNCTION_CALL_RESOLUTION_NONE) {
                    result.kind = FENG_FUNCTION_CALL_RESOLUTION_UNIQUE;
                    result.decl = NULL;
                    result.callable = &member->as.callable;
                    result.member = member;
                    result.owner_type_decl = type_decl;
                    continue;
                }
                result.kind = FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS;
                result.callable = NULL;
                result.member = NULL;
                result.owner_type_decl = NULL;
            }
        }
        free(closure);
        return result;
    }

    if (type_decl->kind != FENG_DECL_TYPE) {
        return result;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_METHOD ||
            !slice_equals(member->as.callable.name, name) ||
            !type_member_is_accessible_from(context, provider_module, member) ||
            fit_body_blocks_private_access(context, type_decl, member) ||
            !callable_parameters_match_args(context, &member->as.callable, args, arg_count)) {
            continue;
        }

        if (result.kind == FENG_FUNCTION_CALL_RESOLUTION_NONE) {
            result.kind = FENG_FUNCTION_CALL_RESOLUTION_UNIQUE;
            result.decl = NULL;
            result.callable = &member->as.callable;
            result.member = member;
            result.owner_type_decl = type_decl;
            continue;
        }

        result.kind = FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS;
        result.callable = NULL;
        result.member = NULL;
        result.owner_type_decl = NULL;
    }

    st.context = context;
    st.owner_type_decl = type_decl;
    st.args = args;
    st.arg_count = arg_count;
    st.result = result;
    (void)visit_visible_fit_methods_for_type(context, type_decl, name, true,
                                             fit_overload_resolve_visitor, &st);
    return st.result;
}

static InferredExprType resolve_expr_owner_type(ResolveContext *context,
                                                const FengExpr *expr,
                                                const FengDecl **out_type_decl,
                                                const FengSemanticModule **out_provider_module) {
    InferredExprType owner_type = infer_expr_type(context, expr);
    const FengDecl *type_decl = resolve_inferred_expr_type_decl(context, owner_type);

    if (out_type_decl != NULL) {
        *out_type_decl = type_decl;
    }
    if (out_provider_module != NULL) {
        *out_provider_module = find_decl_provider_module(context->analysis, type_decl);
    }

    return owner_type;
}

static void record_object_arg_coercion_sites(ResolveContext *context,
                                             FengExpr *const *args,
                                             size_t arg_count,
                                             const FengParameter *params,
                                             size_t param_count);

static void record_spec_member_access(ResolveContext *context,
                                      const FengExpr *expr,
                                      const FengDecl *spec_decl,
                                      const FengTypeMember *member);

static void compute_spec_witness_if_absent(ResolveContext *context,
                                           const FengDecl *type_decl,
                                           const FengDecl *spec_decl,
                                           FengToken err_token);

static bool validate_callable_typed_expr_call(ResolveContext *context,
                                              const FengExpr *callee,
                                              FengExpr *const *args,
                                              size_t arg_count) {
    InferredExprType callee_type = infer_expr_type(context, callee);
    const FengDecl *callee_type_decl = resolve_inferred_expr_type_decl(context, callee_type);
    char *target_name = NULL;

    if (callee_type_decl != NULL && decl_is_function_type(callee_type_decl)) {
        if (function_type_parameters_match_args(context, callee_type_decl, args, arg_count)) {
            note_callable_value_expr_exception_escape(context, callee);
            record_object_arg_coercion_sites(context, args, arg_count,
                                             callee_type_decl->as.spec_decl.as.callable.params,
                                             callee_type_decl->as.spec_decl.as.callable.param_count);
            return true;
        }

        target_name = format_expr_target_name(callee);
        if (!resolver_append_error(
                context,
                callee->token,
                format_message("call target '%s' has no function type overload accepting %zu argument(s)",
                               target_name != NULL ? target_name : "<expression>",
                               arg_count))) {
            free(target_name);
            return false;
        }

        free(target_name);
        return true;
    }

    if (callee_type.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA) {
        if (lambda_expr_parameters_match_args(context, callee_type.lambda_expr, args, arg_count)) {
            note_callable_value_expr_exception_escape(context, callee);
            return true;
        }

        target_name = format_expr_target_name(callee);
        if (!resolver_append_error(
                context,
                callee->token,
                format_message("call target '%s' has no function type overload accepting %zu argument(s)",
                               target_name != NULL ? target_name : "<expression>",
                               arg_count))) {
            free(target_name);
            return false;
        }

        free(target_name);
        return true;
    }

    if (!inferred_expr_type_is_known(callee_type)) {
        return true;
    }

    target_name = format_expr_target_name(callee);
    if (!resolver_append_error(context,
                               callee->token,
                               format_message("expression '%s' is not callable",
                                              target_name != NULL ? target_name : "<expression>"))) {
        free(target_name);
        return false;
    }

    free(target_name);
    return true;
}

/* Walk a block body and unify the inferred return-statement types into a
 * single result. Returns inferred_expr_type_unknown() when the block has no
 * returns (effectively void) or when return types disagree. */
static InferredExprType infer_block_return_type(ResolveContext *context, const FengBlock *block);

static InferredExprType infer_stmt_return_type(ResolveContext *context, const FengStmt *stmt) {
    InferredExprType current = inferred_expr_type_unknown();

    if (stmt == NULL) {
        return current;
    }

    switch (stmt->kind) {
        case FENG_STMT_RETURN:
            return stmt->as.return_value != NULL
                       ? infer_expr_type(context, stmt->as.return_value)
                       : inferred_expr_type_builtin("void");
        case FENG_STMT_BLOCK:
            return infer_block_return_type(context, stmt->as.block);
        case FENG_STMT_IF: {
            size_t clause_index;
            bool any_known = false;

            for (clause_index = 0U; clause_index < stmt->as.if_stmt.clause_count; ++clause_index) {
                InferredExprType branch =
                    infer_block_return_type(context, stmt->as.if_stmt.clauses[clause_index].block);

                if (inferred_expr_type_is_known(branch)) {
                    if (!any_known) {
                        current = branch;
                        any_known = true;
                    } else if (!inferred_expr_types_equal(context, current, branch)) {
                        return inferred_expr_type_unknown();
                    }
                }
            }
            if (stmt->as.if_stmt.else_block != NULL) {
                InferredExprType branch =
                    infer_block_return_type(context, stmt->as.if_stmt.else_block);

                if (inferred_expr_type_is_known(branch)) {
                    if (!any_known) {
                        current = branch;
                    } else if (!inferred_expr_types_equal(context, current, branch)) {
                        return inferred_expr_type_unknown();
                    }
                }
            }
            return current;
        }
        case FENG_STMT_WHILE:
            return infer_block_return_type(context, stmt->as.while_stmt.body);
        case FENG_STMT_MATCH: {
            size_t branch_index;
            bool any_known = false;

            for (branch_index = 0U; branch_index < stmt->as.match_stmt.branch_count; ++branch_index) {
                InferredExprType branch = infer_block_return_type(
                    context, stmt->as.match_stmt.branches[branch_index].body);

                if (inferred_expr_type_is_known(branch)) {
                    if (!any_known) {
                        current = branch;
                        any_known = true;
                    } else if (!inferred_expr_types_equal(context, current, branch)) {
                        return inferred_expr_type_unknown();
                    }
                }
            }
            if (stmt->as.match_stmt.else_block != NULL) {
                InferredExprType branch =
                    infer_block_return_type(context, stmt->as.match_stmt.else_block);

                if (inferred_expr_type_is_known(branch)) {
                    if (!any_known) {
                        current = branch;
                    } else if (!inferred_expr_types_equal(context, current, branch)) {
                        return inferred_expr_type_unknown();
                    }
                }
            }
            return current;
        }
        case FENG_STMT_FOR:
            return infer_block_return_type(context, stmt->as.for_stmt.body);
        case FENG_STMT_TRY: {
            InferredExprType try_type = infer_block_return_type(context, stmt->as.try_stmt.try_block);
            InferredExprType catch_type =
                infer_block_return_type(context, stmt->as.try_stmt.catch_block);
            InferredExprType finally_type = stmt->as.try_stmt.finally_block != NULL
                                                ? infer_block_return_type(
                                                      context, stmt->as.try_stmt.finally_block)
                                                : inferred_expr_type_unknown();

            if (inferred_expr_type_is_known(try_type)) {
                current = try_type;
            }
            if (inferred_expr_type_is_known(catch_type)) {
                if (!inferred_expr_type_is_known(current)) {
                    current = catch_type;
                } else if (!inferred_expr_types_equal(context, current, catch_type)) {
                    return inferred_expr_type_unknown();
                }
            }
            if (inferred_expr_type_is_known(finally_type)) {
                if (!inferred_expr_type_is_known(current)) {
                    current = finally_type;
                } else if (!inferred_expr_types_equal(context, current, finally_type)) {
                    return inferred_expr_type_unknown();
                }
            }
            return current;
        }
        default:
            return current;
    }
}

static InferredExprType infer_block_return_type(ResolveContext *context, const FengBlock *block) {
    size_t index;
    InferredExprType current = inferred_expr_type_unknown();
    bool any_known = false;

    if (block == NULL) {
        return current;
    }

    for (index = 0U; index < block->statement_count; ++index) {
        const FengStmt *stmt = block->statements[index];
        InferredExprType stmt_type;

        if (stmt == NULL) {
            continue;
        }
        /* Bring local bindings into scope so subsequent `return` expressions
         * can resolve identifiers introduced by `let`/`var` statements. */
        if (stmt->kind == FENG_STMT_BINDING) {
            InferredExprType binding_type = stmt->as.binding.type != NULL
                                                ? inferred_expr_type_from_type_ref(stmt->as.binding.type)
                                                : infer_expr_type(context, stmt->as.binding.initializer);
            (void)resolver_add_local_typed_name(context,
                                                stmt->as.binding.name,
                                                binding_type,
                                                stmt->as.binding.mutability);
            continue;
        }

        stmt_type = infer_stmt_return_type(context, stmt);
        if (inferred_expr_type_is_known(stmt_type)) {
            if (!any_known) {
                current = stmt_type;
                any_known = true;
            } else if (!inferred_expr_types_equal(context, current, stmt_type)) {
                return inferred_expr_type_unknown();
            }
        }
    }
    return current;
}

static InferredExprType infer_lambda_body_type(ResolveContext *context, const FengExpr *expr) {
    size_t param_index;
    bool ok = true;
    InferredExprType body_type = inferred_expr_type_unknown();

    if (expr == NULL || expr->kind != FENG_EXPR_LAMBDA) {
        return inferred_expr_type_unknown();
    }

    if (!resolver_push_scope(context)) {
        return inferred_expr_type_unknown();
    }

    for (param_index = 0U; param_index < expr->as.lambda.param_count && ok; ++param_index) {
        ok = resolver_add_local_typed_name(context,
                                           expr->as.lambda.params[param_index].name,
                                           inferred_expr_type_from_type_ref(
                                               expr->as.lambda.params[param_index].type),
                                           expr->as.lambda.params[param_index].mutability);
    }
    if (ok) {
        if (expr->as.lambda.is_block_body) {
            body_type = infer_block_return_type(context, expr->as.lambda.body_block);
            if (!inferred_expr_type_is_known(body_type)) {
                /* A block lambda with no return statements yields void. */
                body_type = inferred_expr_type_builtin("void");
            }
        } else {
            body_type = infer_expr_type(context, expr->as.lambda.body);
        }
    }

    resolver_pop_scope(context);
    return ok ? body_type : inferred_expr_type_unknown();
}

static bool lambda_expr_parameters_match_args(ResolveContext *context,
                                              const FengExpr *expr,
                                              FengExpr *const *args,
                                              size_t arg_count) {
    size_t param_index;

    if (expr == NULL || expr->kind != FENG_EXPR_LAMBDA || expr->as.lambda.param_count != arg_count) {
        return false;
    }

    for (param_index = 0U; param_index < arg_count; ++param_index) {
        if (!expr_matches_expected_type_ref(context,
                                            args[param_index],
                                            expr->as.lambda.params[param_index].type)) {
            return false;
        }
    }

    return true;
}

static InferredExprType infer_lambda_call_expr_type(ResolveContext *context,
                                                    const FengExpr *callee,
                                                    FengExpr *const *args,
                                                    size_t arg_count) {
    if (!lambda_expr_parameters_match_args(context, callee, args, arg_count)) {
        return inferred_expr_type_unknown();
    }

    return infer_lambda_body_type(context, callee);
}

static InferredExprType infer_call_expr_type(ResolveContext *context, const FengExpr *expr) {
    const FengExpr *callee;
    ResolvedTypeTarget target;
    InferredExprType callee_type;
    const FengDecl *callee_type_decl;

    if (expr == NULL || expr->kind != FENG_EXPR_CALL) {
        return inferred_expr_type_unknown();
    }

    callee = expr->as.call.callee;
    target = resolve_type_target_expr(context, callee, false);
    if (target.type_decl != NULL) {
        return inferred_expr_type_from_decl(target.type_decl);
    }
    if (callee == NULL) {
        return inferred_expr_type_unknown();
    }

    if (callee->kind == FENG_EXPR_IDENTIFIER) {
        const FunctionOverloadSetEntry *overload_set =
            find_function_overload_set(context->function_sets,
                                       context->function_set_count,
                                       callee->as.identifier);

        if (overload_set != NULL) {
            FunctionCallResolution resolution =
                resolve_top_level_function_overload(context,
                                                    overload_set,
                                                    expr->as.call.args,
                                                    expr->as.call.arg_count);

            if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE &&
                resolution.callable != NULL) {
                InferredExprType return_type =
                    callable_effective_return_type(context, resolution.callable);

                if (inferred_expr_type_is_known(return_type)) {
                    return return_type;
                }
            }
        }
    }

    if (callee->kind == FENG_EXPR_MEMBER) {
        const FengExpr *object = callee->as.member.object;

        if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
            const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

            if (alias != NULL) {
                FunctionCallResolution resolution =
                    resolve_module_public_function_overload(context,
                                                            alias->target_module,
                                                            callee->as.member.member,
                                                            expr->as.call.args,
                                                            expr->as.call.arg_count);

                if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE &&
                    resolution.callable != NULL) {
                    InferredExprType return_type =
                        callable_effective_return_type(context, resolution.callable);

                    if (inferred_expr_type_is_known(return_type)) {
                        return return_type;
                    }
                }
            }
        }

        {
            const FengDecl *owner_type_decl = NULL;
            const FengSemanticModule *provider_module = NULL;
            FunctionCallResolution resolution;

            resolve_expr_owner_type(context, object, &owner_type_decl, &provider_module);
            resolution = resolve_accessible_method_overload(context,
                                                            owner_type_decl,
                                                            provider_module,
                                                            callee->as.member.member,
                                                            expr->as.call.args,
                                                            expr->as.call.arg_count);
            if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE &&
                resolution.callable != NULL) {
                InferredExprType return_type =
                    callable_effective_return_type(context, resolution.callable);

                if (inferred_expr_type_is_known(return_type)) {
                    return return_type;
                }
            }
        }
    }

    if (callee->kind == FENG_EXPR_LAMBDA) {
        InferredExprType lambda_result = infer_lambda_call_expr_type(context,
                                                                     callee,
                                                                     expr->as.call.args,
                                                                     expr->as.call.arg_count);

        if (inferred_expr_type_is_known(lambda_result)) {
            return lambda_result;
        }
    }

    callee_type = infer_expr_type(context, callee);
    callee_type_decl = resolve_inferred_expr_type_decl(context, callee_type);
    if (callee_type.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA) {
        return infer_lambda_call_expr_type(context,
                                           callee_type.lambda_expr,
                                           expr->as.call.args,
                                           expr->as.call.arg_count);
    }
    if (callee_type_decl != NULL &&
        function_type_parameters_match_args(context,
                                            callee_type_decl,
                                            expr->as.call.args,
                                            expr->as.call.arg_count)) {
        return inferred_expr_type_from_return_type_ref(
            callee_type_decl->as.spec_decl.as.callable.return_type);
    }

    return inferred_expr_type_unknown();
}

static bool callable_return_inference_is_pending(ResolveContext *context,
                                                 const FengCallableSignature *callable) {
    return callable != NULL && callable->return_type == NULL &&
           !inferred_expr_type_is_known(callable_effective_return_type(context, callable));
}

static bool expr_type_inference_is_pending(ResolveContext *context, const FengExpr *expr) {
    size_t index;

    if (expr == NULL) {
        return false;
    }

    switch (expr->kind) {
        case FENG_EXPR_CALL: {
            const FengExpr *callee = expr->as.call.callee;

            if (callee == NULL) {
                return false;
            }

            if (callee->kind == FENG_EXPR_IDENTIFIER) {
                const FunctionOverloadSetEntry *overload_set =
                    find_function_overload_set(context->function_sets,
                                               context->function_set_count,
                                               callee->as.identifier);

                if (overload_set != NULL) {
                    FunctionCallResolution resolution =
                        resolve_top_level_function_overload(context,
                                                            overload_set,
                                                            expr->as.call.args,
                                                            expr->as.call.arg_count);

                    if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE &&
                        callable_return_inference_is_pending(context, resolution.callable)) {
                        return true;
                    }
                }
            }

            if (callee->kind == FENG_EXPR_MEMBER) {
                const FengExpr *object = callee->as.member.object;

                if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
                    const AliasEntry *alias =
                        find_unshadowed_alias(context, object->as.identifier);

                    if (alias != NULL) {
                        FunctionCallResolution resolution =
                            resolve_module_public_function_overload(context,
                                                                    alias->target_module,
                                                                    callee->as.member.member,
                                                                    expr->as.call.args,
                                                                    expr->as.call.arg_count);

                        if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE &&
                            callable_return_inference_is_pending(context, resolution.callable)) {
                            return true;
                        }
                    }
                }

                {
                    const FengDecl *owner_type_decl = NULL;
                    const FengSemanticModule *provider_module = NULL;
                    FunctionCallResolution resolution;

                    resolve_expr_owner_type(context, object, &owner_type_decl, &provider_module);
                    resolution = resolve_accessible_method_overload(context,
                                                                    owner_type_decl,
                                                                    provider_module,
                                                                    callee->as.member.member,
                                                                    expr->as.call.args,
                                                                    expr->as.call.arg_count);
                    if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE &&
                        callable_return_inference_is_pending(context, resolution.callable)) {
                        return true;
                    }
                }
            }

            if (callee->kind == FENG_EXPR_LAMBDA) {
                return expr_type_inference_is_pending(context, callee->as.lambda.body);
            }

            return expr_type_inference_is_pending(context, callee);
        }

        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (expr_type_inference_is_pending(context, expr->as.array_literal.items[index])) {
                    return true;
                }
            }
            return false;

        case FENG_EXPR_OBJECT_LITERAL:
            if (expr_type_inference_is_pending(context, expr->as.object_literal.target)) {
                return true;
            }
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                if (expr_type_inference_is_pending(context,
                                                   expr->as.object_literal.fields[index].value)) {
                    return true;
                }
            }
            return false;

        case FENG_EXPR_MEMBER:
            return expr_type_inference_is_pending(context, expr->as.member.object);

        case FENG_EXPR_INDEX:
            return expr_type_inference_is_pending(context, expr->as.index.object) ||
                   expr_type_inference_is_pending(context, expr->as.index.index);

        case FENG_EXPR_UNARY:
            return expr_type_inference_is_pending(context, expr->as.unary.operand);

        case FENG_EXPR_BINARY:
            return expr_type_inference_is_pending(context, expr->as.binary.left) ||
                   expr_type_inference_is_pending(context, expr->as.binary.right);

        case FENG_EXPR_LAMBDA:
            return expr->as.lambda.is_block_body
                       ? false /* block body return inference uses synthetic signature path */
                       : expr_type_inference_is_pending(context, expr->as.lambda.body);

        case FENG_EXPR_CAST:
            return expr_type_inference_is_pending(context, expr->as.cast.value);

        case FENG_EXPR_IF: {
            const FengExpr *then_yield = block_yield_expression(expr->as.if_expr.then_block);
            const FengExpr *else_yield = block_yield_expression(expr->as.if_expr.else_block);

            return expr_type_inference_is_pending(context, expr->as.if_expr.condition) ||
                   (then_yield != NULL && expr_type_inference_is_pending(context, then_yield)) ||
                   (else_yield != NULL && expr_type_inference_is_pending(context, else_yield));
        }

        case FENG_EXPR_MATCH: {
            const FengExpr *else_yield = block_yield_expression(expr->as.match_expr.else_block);

            if (expr_type_inference_is_pending(context, expr->as.match_expr.target) ||
                (else_yield != NULL && expr_type_inference_is_pending(context, else_yield))) {
                return true;
            }
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                const FengExpr *branch_yield =
                    block_yield_expression(expr->as.match_expr.branches[index].body);

                if (branch_yield != NULL &&
                    expr_type_inference_is_pending(context, branch_yield)) {
                    return true;
                }
            }
            return false;
        }

        case FENG_EXPR_IDENTIFIER:
        case FENG_EXPR_SELF:
        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
            return false;
    }

    return false;
}

static bool validate_instance_member_expr(ResolveContext *context, const FengExpr *expr) {
    const FengDecl *owner_type_decl = NULL;
    const FengSemanticModule *provider_module = NULL;
    const FengTypeMember *member;
    InferredExprType owner_type;
    const char *builtin_name;

    owner_type = resolve_expr_owner_type(context,
                                         expr->as.member.object,
                                         &owner_type_decl,
                                         &provider_module);
    if (!inferred_expr_type_is_known(owner_type)) {
        return true;
    }

    if (owner_type_decl == NULL) {
        builtin_name = inferred_expr_type_builtin_canonical_name(owner_type);
        if (builtin_name == NULL) {
            return true;
        }

        return resolver_append_error(
            context,
            expr->token,
            format_message("type '%s' has no member '%.*s'",
                           builtin_name,
                           (int)expr->as.member.member.length,
                           expr->as.member.member.data));
    }

    if (owner_type_decl->kind == FENG_DECL_SPEC) {
        FengSlice owner_name = decl_typeish_name(owner_type_decl);

        if (owner_type_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            return resolver_append_error(
                context,
                expr->token,
                format_message("spec '%.*s' is callable-form and has no member '%.*s'",
                               (int)owner_name.length,
                               owner_name.data,
                               (int)expr->as.member.member.length,
                               expr->as.member.member.data));
        }

        member = find_spec_object_member(context, owner_type_decl, expr->as.member.member);
        if (member != NULL) {
            /* Spec members are unconditionally public per spec rules. */
            record_spec_member_access(context, expr, owner_type_decl, member);
            return true;
        }
        return resolver_append_error(
            context,
            expr->token,
            format_message("spec '%.*s' has no member '%.*s'",
                           (int)owner_name.length,
                           owner_name.data,
                           (int)expr->as.member.member.length,
                           expr->as.member.member.data));
    }

    if (owner_type_decl->kind != FENG_DECL_TYPE) {
        FengSlice owner_name = decl_typeish_name(owner_type_decl);
        return resolver_append_error(
            context,
            expr->token,
            format_message("type '%.*s' has no member '%.*s'",
                           (int)owner_name.length,
                           owner_name.data,
                           (int)expr->as.member.member.length,
                           expr->as.member.member.data));
    }

    member = find_instance_member(owner_type_decl, expr->as.member.member);
    if (member == NULL) {
        /* Allow lookup to also find methods supplied by visible fit declarations. */
        const FengTypeMember *fit_member =
            find_fit_method_member_for_type(context, owner_type_decl, expr->as.member.member);
        if (fit_member != NULL) {
            return true;
        }
        return resolver_append_error(
            context,
            expr->token,
            format_message("type '%.*s' has no member '%.*s'",
                           (int)owner_type_decl->as.type_decl.name.length,
                           owner_type_decl->as.type_decl.name.data,
                           (int)expr->as.member.member.length,
                           expr->as.member.member.data));
    }

    if (type_member_is_accessible_from(context, provider_module, member)) {
        if (fit_body_blocks_private_access(context, owner_type_decl, member)) {
            return resolver_append_error(
                context,
                expr->token,
                format_message("fit body cannot access private member '%.*s' of target type '%.*s'",
                               member->kind == FENG_TYPE_MEMBER_FIELD ? (int)member->as.field.name.length
                                                                      : (int)member->as.callable.name.length,
                               member->kind == FENG_TYPE_MEMBER_FIELD ? member->as.field.name.data
                                                                      : member->as.callable.name.data,
                               (int)owner_type_decl->as.type_decl.name.length,
                               owner_type_decl->as.type_decl.name.data));
        }
        return true;
    }

    return resolver_append_error(
        context,
        expr->token,
        format_message("member '%.*s' of type '%.*s' is not accessible from the current module",
                       member->kind == FENG_TYPE_MEMBER_FIELD ? (int)member->as.field.name.length
                                                              : (int)member->as.callable.name.length,
                       member->kind == FENG_TYPE_MEMBER_FIELD ? member->as.field.name.data
                                                              : member->as.callable.name.data,
                       (int)owner_type_decl->as.type_decl.name.length,
                       owner_type_decl->as.type_decl.name.data));
}

static bool append_unique_slice(FengSlice **items, size_t *count, size_t *capacity, FengSlice value) {
    if (find_slice_index(*items, *count, value) < *count) {
        return true;
    }

    return append_slice(items, count, capacity, value);
}

static void resolver_clear_current_constructor_bindings(ResolveContext *context) {
    free(context->current_constructor_bound_names);
    context->current_constructor_bound_names = NULL;
    context->current_constructor_bound_count = 0U;
    context->current_constructor_bound_capacity = 0U;
}

static bool resolver_current_constructor_has_bound_name(const ResolveContext *context, FengSlice name) {
    return find_slice_index(context->current_constructor_bound_names,
                            context->current_constructor_bound_count,
                            name) < context->current_constructor_bound_count;
}

static bool resolver_current_constructor_add_bound_name(ResolveContext *context, FengSlice name) {
    return append_unique_slice(&context->current_constructor_bound_names,
                               &context->current_constructor_bound_count,
                               &context->current_constructor_bound_capacity,
                               name);
}

static bool collect_constructor_bound_lets_from_stmt(const FengDecl *type_decl,
                                                     const FengStmt *stmt,
                                                     FengSlice **bound_names,
                                                     size_t *bound_count,
                                                     size_t *bound_capacity);

static bool collect_constructor_bound_lets_from_block(const FengDecl *type_decl,
                                                      const FengBlock *block,
                                                      FengSlice **bound_names,
                                                      size_t *bound_count,
                                                      size_t *bound_capacity) {
    size_t stmt_index;

    if (block == NULL) {
        return true;
    }

    for (stmt_index = 0U; stmt_index < block->statement_count; ++stmt_index) {
        if (!collect_constructor_bound_lets_from_stmt(type_decl,
                                                      block->statements[stmt_index],
                                                      bound_names,
                                                      bound_count,
                                                      bound_capacity)) {
            return false;
        }
    }

    return true;
}

static bool collect_constructor_bound_lets_from_stmt(const FengDecl *type_decl,
                                                     const FengStmt *stmt,
                                                     FengSlice **bound_names,
                                                     size_t *bound_count,
                                                     size_t *bound_capacity) {
    size_t clause_index;
    const FengTypeMember *member;

    if (stmt == NULL) {
        return true;
    }

    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return collect_constructor_bound_lets_from_block(
                type_decl, stmt->as.block, bound_names, bound_count, bound_capacity);

        case FENG_STMT_ASSIGN:
            member = find_direct_self_let_target_member(type_decl, stmt->as.assign.target);
            if (member != NULL &&
                !append_unique_slice(bound_names, bound_count, bound_capacity, member->as.field.name)) {
                return false;
            }
            return true;

        case FENG_STMT_IF:
            for (clause_index = 0U; clause_index < stmt->as.if_stmt.clause_count; ++clause_index) {
                if (!collect_constructor_bound_lets_from_block(type_decl,
                                                               stmt->as.if_stmt.clauses[clause_index].block,
                                                               bound_names,
                                                               bound_count,
                                                               bound_capacity)) {
                    return false;
                }
            }
            return collect_constructor_bound_lets_from_block(
                type_decl, stmt->as.if_stmt.else_block, bound_names, bound_count, bound_capacity);

        case FENG_STMT_MATCH: {
            size_t branch_index;

            for (branch_index = 0U; branch_index < stmt->as.match_stmt.branch_count; ++branch_index) {
                if (!collect_constructor_bound_lets_from_block(
                        type_decl,
                        stmt->as.match_stmt.branches[branch_index].body,
                        bound_names,
                        bound_count,
                        bound_capacity)) {
                    return false;
                }
            }
            return collect_constructor_bound_lets_from_block(
                type_decl, stmt->as.match_stmt.else_block, bound_names, bound_count, bound_capacity);
        }

        case FENG_STMT_WHILE:
            return collect_constructor_bound_lets_from_block(
                type_decl, stmt->as.while_stmt.body, bound_names, bound_count, bound_capacity);

        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                return collect_constructor_bound_lets_from_block(type_decl,
                                                                 stmt->as.for_stmt.body,
                                                                 bound_names,
                                                                 bound_count,
                                                                 bound_capacity);
            }
            return collect_constructor_bound_lets_from_stmt(type_decl,
                                                            stmt->as.for_stmt.init,
                                                            bound_names,
                                                            bound_count,
                                                            bound_capacity) &&
                   collect_constructor_bound_lets_from_stmt(type_decl,
                                                            stmt->as.for_stmt.update,
                                                            bound_names,
                                                            bound_count,
                                                            bound_capacity) &&
                   collect_constructor_bound_lets_from_block(type_decl,
                                                             stmt->as.for_stmt.body,
                                                             bound_names,
                                                             bound_count,
                                                             bound_capacity);

        case FENG_STMT_TRY:
            return collect_constructor_bound_lets_from_block(
                       type_decl,
                       stmt->as.try_stmt.try_block,
                       bound_names,
                       bound_count,
                       bound_capacity) &&
                   collect_constructor_bound_lets_from_block(type_decl,
                                                             stmt->as.try_stmt.catch_block,
                                                             bound_names,
                                                             bound_count,
                                                             bound_capacity) &&
                   collect_constructor_bound_lets_from_block(type_decl,
                                                             stmt->as.try_stmt.finally_block,
                                                             bound_names,
                                                             bound_count,
                                                             bound_capacity);

        case FENG_STMT_BINDING:
        case FENG_STMT_EXPR:
        case FENG_STMT_RETURN:
        case FENG_STMT_THROW:
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return true;
    }

    return true;
}

static bool constructor_binds_let_field(const FengDecl *type_decl,
                                        const FengTypeMember *constructor,
                                        FengSlice field_name) {
    FengSlice *bound_names = NULL;
    size_t bound_count = 0U;
    size_t bound_capacity = 0U;
    bool found;

    if (constructor == NULL || constructor->kind != FENG_TYPE_MEMBER_CONSTRUCTOR) {
        return false;
    }

    if (!collect_constructor_bound_lets_from_block(type_decl,
                                                   constructor->as.callable.body,
                                                   &bound_names,
                                                   &bound_count,
                                                   &bound_capacity)) {
        free(bound_names);
        return false;
    }

    found = find_slice_index(bound_names, bound_count, field_name) < bound_count;
    free(bound_names);
    return found;
}

static bool validate_let_field_object_literal_binding(ResolveContext *context,
                                                      const FengDecl *type_decl,
                                                      const FengSemanticModule *provider_module,
                                                      const FengExpr *target_expr,
                                                      const FengObjectFieldInit *field) {
    const FengTypeMember *let_field = find_type_let_field_member(type_decl, field->name);
    const FengTypeMember *selected_constructor = NULL;
    ConstructorResolution resolution;

    if (let_field == NULL) {
        return true;
    }

    if (field_has_declaration_initializer(let_field)) {
        return resolver_append_error(
            context,
            field->token,
            format_message("object literal field '%.*s' repeats final binding of let member '%.*s' already completed by declaration initializer",
                           (int)field->name.length,
                           field->name.data,
                           (int)field->name.length,
                           field->name.data));
    }

    if (count_declared_constructors(type_decl) > 0U) {
        if (target_expr != NULL && target_expr->kind == FENG_EXPR_CALL) {
            resolution = resolve_accessible_constructor_overload(context,
                                                                 type_decl,
                                                                 provider_module,
                                                                 target_expr->as.call.args,
                                                                 target_expr->as.call.arg_count);
        } else {
            resolution = resolve_accessible_constructor_overload(
                context, type_decl, provider_module, NULL, 0U);
        }

        if (resolution.kind == FENG_CONSTRUCTOR_RESOLUTION_UNIQUE) {
            selected_constructor = resolution.constructor;
        }
    }

    if (selected_constructor != NULL && constructor_binds_let_field(type_decl, selected_constructor, field->name)) {
        return resolver_append_error(
            context,
            field->token,
            format_message("object literal field '%.*s' repeats final binding of let member '%.*s' already completed by constructor '%.*s'",
                           (int)field->name.length,
                           field->name.data,
                           (int)field->name.length,
                           field->name.data,
                           (int)selected_constructor->as.callable.name.length,
                           selected_constructor->as.callable.name.data));
    }

    return true;
}

static bool validate_self_let_assignment(ResolveContext *context, const FengStmt *stmt) {
    const FengTypeMember *field_member;

    if (context->current_type_decl == NULL) {
        return true;
    }

    field_member = find_direct_self_let_target_member(context->current_type_decl, stmt->as.assign.target);
    if (field_member == NULL) {
        return true;
    }

    if (context->current_callable_member == NULL ||
        context->current_callable_member->kind != FENG_TYPE_MEMBER_CONSTRUCTOR) {
        return resolver_append_error(
            context,
            stmt->as.assign.target->token,
            format_message("let member '%.*s' cannot be directly assigned outside constructors",
                           (int)field_member->as.field.name.length,
                           field_member->as.field.name.data));
    }

    if (field_has_declaration_initializer(field_member)) {
        return resolver_append_error(
            context,
            stmt->as.assign.target->token,
            format_message("constructor assignment repeats final binding of let member '%.*s' already completed by declaration initializer",
                           (int)field_member->as.field.name.length,
                           field_member->as.field.name.data));
    }

    if (resolver_current_constructor_has_bound_name(context, field_member->as.field.name)) {
        return resolver_append_error(
            context,
            stmt->as.assign.target->token,
            format_message("constructor assignment repeats final binding of let member '%.*s' more than once in constructor '%.*s'",
                           (int)field_member->as.field.name.length,
                           field_member->as.field.name.data,
                           (int)context->current_callable_member->as.callable.name.length,
                           context->current_callable_member->as.callable.name.data));
    }

    return resolver_current_constructor_add_bound_name(context, field_member->as.field.name);
}

static const FengTypeRef *resolve_indexed_array_element_type_ref(ResolveContext *context,
                                                                 const FengExpr *object_expr) {
    InferredExprType object_type;

    if (object_expr == NULL) {
        return NULL;
    }

    object_type = infer_expr_type(context, object_expr);
    if (object_type.kind != FENG_INFERRED_EXPR_TYPE_TYPE_REF || object_type.type_ref == NULL) {
        return NULL;
    }
    if (object_type.type_ref->kind != FENG_TYPE_REF_ARRAY) {
        return NULL;
    }

    return object_type.type_ref->as.inner;
}

static InferredExprType infer_array_literal_expr_type(ResolveContext *context, const FengExpr *expr) {
    size_t item_index;
    InferredExprType element_type;
    const FengTypeRef *array_type_ref;

    if (expr == NULL || expr->kind != FENG_EXPR_ARRAY_LITERAL || expr->as.array_literal.count == 0U) {
        return inferred_expr_type_unknown();
    }

    element_type = infer_expr_type(context, expr->as.array_literal.items[0]);
    if (!inferred_expr_type_is_known(element_type)) {
        return inferred_expr_type_unknown();
    }

    for (item_index = 1U; item_index < expr->as.array_literal.count; ++item_index) {
        InferredExprType item_type = infer_expr_type(context, expr->as.array_literal.items[item_index]);

        if (!inferred_expr_type_is_known(item_type) ||
            !inferred_expr_types_equal(context, element_type, item_type)) {
            return inferred_expr_type_unknown();
        }
    }

    array_type_ref = synthesize_array_type_ref(context, &element_type,
                                               expr->as.array_literal.element_writable,
                                               expr->token);
    return array_type_ref != NULL ? inferred_expr_type_from_type_ref(array_type_ref)
                                  : inferred_expr_type_unknown();
}

static bool validate_array_literal_expr(ResolveContext *context, const FengExpr *expr) {
    size_t item_index;
    InferredExprType element_type = inferred_expr_type_unknown();

    if (expr == NULL || expr->kind != FENG_EXPR_ARRAY_LITERAL) {
        return true;
    }

    for (item_index = 0U; item_index < expr->as.array_literal.count; ++item_index) {
        InferredExprType item_type = infer_expr_type(context, expr->as.array_literal.items[item_index]);

        if (!inferred_expr_type_is_known(item_type)) {
            continue;
        }

        if (!inferred_expr_type_is_known(element_type)) {
            element_type = item_type;
            continue;
        }

        if (!inferred_expr_types_equal(context, element_type, item_type)) {
            char *type_name = format_inferred_expr_type_name(element_type);
            bool ok = resolver_append_error(
                context,
                expr->as.array_literal.items[item_index]->token,
                format_message("array literal element at index %zu does not match expected type '%s'",
                               item_index,
                               type_name != NULL ? type_name : "<type>"));

            free(type_name);
            return ok;
        }
    }

    return true;
}

static bool append_assignment_target_not_writable_error(ResolveContext *context,
                                                        const FengExpr *target) {
    char *target_name = format_expr_target_name(target);
    bool ok = resolver_append_error(
        context,
        target->token,
        format_message("assignment target '%s' is not writable",
                       target_name != NULL ? target_name : "<expression>"));

    free(target_name);
    return ok;
}

static bool validate_assignment_target_writable(ResolveContext *context, const FengExpr *target) {
    if (target == NULL) {
        return true;
    }

    switch (target->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const LocalNameEntry *local_entry =
                resolver_find_local_name_entry(context, target->as.identifier);

            if (local_entry != NULL) {
                return mutability_is_writable(local_entry->mutability)
                           ? true
                           : append_assignment_target_not_writable_error(context, target);
            }

            {
                const VisibleValueEntry *visible_value =
                    find_visible_value(context->visible_values,
                                       context->visible_value_count,
                                       target->as.identifier);

                if (visible_value != NULL && !visible_value->is_function &&
                    visible_value->decl != NULL &&
                    visible_value->decl->kind == FENG_DECL_GLOBAL_BINDING &&
                    mutability_is_writable(visible_value->mutability)) {
                    return true;
                }
            }

            return append_assignment_target_not_writable_error(context, target);
        }

        case FENG_EXPR_MEMBER: {
            const FengExpr *object = target->as.member.object;

            if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
                const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

                if (alias != NULL) {
                    const FengDecl *binding_decl =
                        find_module_public_binding_decl(alias->target_module, target->as.member.member);

                    if (binding_decl != NULL && binding_decl->kind == FENG_DECL_GLOBAL_BINDING &&
                        mutability_is_writable(binding_decl->as.binding.mutability)) {
                        return true;
                    }

                    return append_assignment_target_not_writable_error(context, target);
                }
            }

            if (context->current_type_decl != NULL &&
                find_direct_self_let_target_member(context->current_type_decl, target) != NULL) {
                return true;
            }

            {
                const FengDecl *owner_type_decl = resolve_inferred_expr_type_decl(
                    context, infer_expr_type(context, target->as.member.object));

                if (owner_type_decl != NULL) {
                    const FengTypeMember *member =
                        find_decl_instance_member(context, owner_type_decl, target->as.member.member);

                    if (member != NULL && member->kind == FENG_TYPE_MEMBER_FIELD &&
                        member->as.field.mutability == FENG_MUTABILITY_VAR) {
                        return true;
                    }

                    if (member != NULL) {
                        return append_assignment_target_not_writable_error(context, target);
                    }
                }
            }

            return true;
        }

        case FENG_EXPR_INDEX:
            if (!validate_index_expr(context, target)) {
                return false;
            }
            /* docs/feng-builtin-type.md §5: writes via `[i] =` are only legal
             * when the indexed array layer is marked writable (`T[]!`). */
            {
                InferredExprType object_type =
                    infer_expr_type(context, target->as.index.object);
                if (object_type.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
                    object_type.type_ref != NULL &&
                    object_type.type_ref->kind == FENG_TYPE_REF_ARRAY &&
                    !object_type.type_ref->array_element_writable) {
                    return append_assignment_target_not_writable_error(context, target);
                }
            }
            return true;

        default:
            return append_assignment_target_not_writable_error(context, target);
    }
}

static char *format_expr_target_name(const FengExpr *expr) {
    if (expr == NULL) {
        return duplicate_cstr("<expression>");
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            return format_module_name(&expr->as.identifier, 1U);

        case FENG_EXPR_MEMBER: {
            char *object_name = format_expr_target_name(expr->as.member.object);
            size_t object_length = object_name != NULL ? strlen(object_name) : 12U;
            const char *fallback = "<expression>";
            char *buffer = (char *)malloc(object_length + 1U + expr->as.member.member.length + 1U);

            if (buffer == NULL) {
                free(object_name);
                return NULL;
            }

            if (object_name != NULL) {
                memcpy(buffer, object_name, object_length);
            } else {
                memcpy(buffer, fallback, object_length);
            }
            buffer[object_length] = '.';
            memcpy(buffer + object_length + 1U,
                   expr->as.member.member.data,
                   expr->as.member.member.length);
            buffer[object_length + 1U + expr->as.member.member.length] = '\0';
            free(object_name);
            return buffer;
        }

        case FENG_EXPR_CALL:
            return format_expr_target_name(expr->as.call.callee);

        default:
            return duplicate_cstr("<expression>");
    }
}

static ResolvedTypeTarget resolve_type_target_expr(const ResolveContext *context,
                                                   const FengExpr *target_expr,
                                                   bool follow_call_callee) {
    ResolvedTypeTarget result;

    memset(&result, 0, sizeof(result));
    if (target_expr == NULL) {
        return result;
    }

    switch (target_expr->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const VisibleTypeEntry *entry =
                find_visible_type(context->visible_types,
                                  context->visible_type_count,
                                  target_expr->as.identifier);

            if (entry != NULL) {
                result.type_decl = entry->decl;
                result.provider_module = entry->provider_module;
            }
            return result;
        }

        case FENG_EXPR_MEMBER:
            if (target_expr->as.member.object != NULL &&
                target_expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
                const AliasEntry *alias =
                    find_unshadowed_alias(context, target_expr->as.member.object->as.identifier);

                if (alias != NULL) {
                    result.type_decl =
                        find_module_public_type_decl(alias->target_module, target_expr->as.member.member);
                    result.provider_module = result.type_decl != NULL ? alias->target_module : NULL;
                }
            }
            return result;

        case FENG_EXPR_CALL:
            if (follow_call_callee) {
                return resolve_type_target_expr(context, target_expr->as.call.callee, true);
            }
            return result;

        default:
            return result;
    }
}

static InferredExprType infer_identifier_expr_type(ResolveContext *context, FengSlice name) {
    const LocalNameEntry *local_entry = resolver_find_local_name_entry(context, name);

    if (local_entry != NULL) {
        return local_entry->type;
    }

    {
        const VisibleValueEntry *visible_value =
            find_visible_value(context->visible_values, context->visible_value_count, name);

        if (visible_value != NULL && !visible_value->is_function &&
            visible_value->decl != NULL &&
            visible_value->decl->kind == FENG_DECL_GLOBAL_BINDING &&
            visible_value->decl->as.binding.type != NULL) {
            return inferred_expr_type_from_type_ref(visible_value->decl->as.binding.type);
        }
    }

    return inferred_expr_type_unknown();
}

static InferredExprType infer_member_expr_type(ResolveContext *context, const FengExpr *expr) {
    if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
        const AliasEntry *alias =
            find_unshadowed_alias(context, expr->as.member.object->as.identifier);

        if (alias != NULL) {
            const FengDecl *binding_decl =
                find_module_public_binding_decl(alias->target_module, expr->as.member.member);

            if (binding_decl != NULL && binding_decl->kind == FENG_DECL_GLOBAL_BINDING &&
                binding_decl->as.binding.type != NULL) {
                return inferred_expr_type_from_type_ref(binding_decl->as.binding.type);
            }

            return inferred_expr_type_unknown();
        }
    }

    const FengDecl *owner_type_decl =
        resolve_inferred_expr_type_decl(context, infer_expr_type(context, expr->as.member.object));
    const FengTypeMember *field_member;

    if (owner_type_decl == NULL) {
        return inferred_expr_type_unknown();
    }

    field_member = find_type_field_member(owner_type_decl, expr->as.member.member);
    if (field_member == NULL && owner_type_decl->kind == FENG_DECL_SPEC) {
        const FengTypeMember *spec_member =
            find_spec_object_member(context, owner_type_decl, expr->as.member.member);

        if (spec_member != NULL && spec_member->kind == FENG_TYPE_MEMBER_FIELD) {
            field_member = spec_member;
        }
    }
    if (field_member == NULL) {
        return inferred_expr_type_unknown();
    }

    return inferred_expr_type_from_type_ref(field_member->as.field.type);
}

static InferredExprType infer_expr_type(ResolveContext *context, const FengExpr *expr) {
    size_t index;

    if (expr == NULL) {
        return inferred_expr_type_unknown();
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            return infer_identifier_expr_type(context, expr->as.identifier);

        case FENG_EXPR_SELF:
            return context->current_type_decl != NULL
                       ? inferred_expr_type_from_decl(context->current_type_decl)
                       : inferred_expr_type_unknown();

        case FENG_EXPR_BOOL:
            return inferred_expr_type_builtin("bool");

        case FENG_EXPR_INTEGER:
            return inferred_expr_type_builtin("int");

        case FENG_EXPR_FLOAT:
            return inferred_expr_type_builtin("double");

        case FENG_EXPR_STRING:
            return inferred_expr_type_builtin("string");

        case FENG_EXPR_ARRAY_LITERAL:
            return infer_array_literal_expr_type(context, expr);

        case FENG_EXPR_LAMBDA:
            return inferred_expr_type_is_known(infer_lambda_body_type(context, expr))
                       ? inferred_expr_type_from_lambda(expr)
                       : inferred_expr_type_unknown();

        case FENG_EXPR_INDEX: {
            const FengTypeRef *element_type_ref =
                resolve_indexed_array_element_type_ref(context, expr->as.index.object);

            return element_type_ref != NULL &&
                           inferred_expr_type_is_integer(infer_expr_type(context, expr->as.index.index))
                       ? inferred_expr_type_from_type_ref(element_type_ref)
                                            : inferred_expr_type_unknown();
        }

        case FENG_EXPR_OBJECT_LITERAL: {
            ResolvedTypeTarget target =
                resolve_type_target_expr(context, expr->as.object_literal.target, true);

            return target.type_decl != NULL ? inferred_expr_type_from_decl(target.type_decl)
                                            : inferred_expr_type_unknown();
        }

        case FENG_EXPR_CALL:
            return infer_call_expr_type(context, expr);

        case FENG_EXPR_MEMBER:
            return infer_member_expr_type(context, expr);

        case FENG_EXPR_UNARY: {
            InferredExprType operand_type = infer_expr_type(context, expr->as.unary.operand);

            if (expr->as.unary.op == FENG_TOKEN_MINUS &&
                inferred_expr_type_is_numeric(operand_type)) {
                return operand_type;
            }
            if (expr->as.unary.op == FENG_TOKEN_NOT &&
                inferred_expr_type_is_bool(operand_type)) {
                return inferred_expr_type_builtin("bool");
            }

            return inferred_expr_type_unknown();
        }

        case FENG_EXPR_BINARY: {
            InferredExprType left_type = infer_expr_type(context, expr->as.binary.left);
            InferredExprType right_type = infer_expr_type(context, expr->as.binary.right);

            switch (expr->as.binary.op) {
                case FENG_TOKEN_PLUS:
                    if (inferred_expr_types_equal(context, left_type, right_type) &&
                        (inferred_expr_type_is_numeric(left_type) ||
                         inferred_expr_type_is_string(left_type))) {
                        return left_type;
                    }
                    return inferred_expr_type_unknown();

                case FENG_TOKEN_MINUS:
                case FENG_TOKEN_STAR:
                case FENG_TOKEN_SLASH:
                case FENG_TOKEN_PERCENT:
                    if (inferred_expr_types_equal(context, left_type, right_type) &&
                        inferred_expr_type_is_numeric(left_type)) {
                        return left_type;
                    }
                    return inferred_expr_type_unknown();

                case FENG_TOKEN_LT:
                case FENG_TOKEN_LE:
                case FENG_TOKEN_GT:
                case FENG_TOKEN_GE:
                    if (inferred_expr_types_equal(context, left_type, right_type) &&
                        inferred_expr_type_is_numeric(left_type)) {
                        return inferred_expr_type_builtin("bool");
                    }
                    return inferred_expr_type_unknown();

                case FENG_TOKEN_EQ:
                case FENG_TOKEN_NE:
                    if (inferred_expr_type_is_known(left_type) &&
                        inferred_expr_type_is_known(right_type) &&
                        inferred_expr_types_equal(context, left_type, right_type)) {
                        return inferred_expr_type_builtin("bool");
                    }
                    return inferred_expr_type_unknown();

                case FENG_TOKEN_AND_AND:
                case FENG_TOKEN_OR_OR:
                    if (inferred_expr_type_is_bool(left_type) &&
                        inferred_expr_type_is_bool(right_type)) {
                        return inferred_expr_type_builtin("bool");
                    }
                    return inferred_expr_type_unknown();

                case FENG_TOKEN_AMP:
                case FENG_TOKEN_PIPE:
                case FENG_TOKEN_CARET:
                case FENG_TOKEN_SHL:
                case FENG_TOKEN_SHR:
                    if (inferred_expr_types_equal(context, left_type, right_type) &&
                        inferred_expr_type_is_integer(left_type)) {
                        return left_type;
                    }
                    return inferred_expr_type_unknown();

                default:
                    return inferred_expr_type_unknown();
            }
        }

        case FENG_EXPR_CAST:
            return cast_expr_types_are_valid(context,
                                             infer_expr_type(context, expr->as.cast.value),
                                             expr->as.cast.type)
                       ? inferred_expr_type_from_type_ref(expr->as.cast.type)
                       : inferred_expr_type_unknown();

        case FENG_EXPR_IF: {
            InferredExprType condition_type = infer_expr_type(context, expr->as.if_expr.condition);
            InferredExprType then_type =
                block_yield_inferred_type(context, expr->as.if_expr.then_block);
            InferredExprType else_type =
                block_yield_inferred_type(context, expr->as.if_expr.else_block);

            return inferred_expr_type_is_bool(condition_type) &&
                       inferred_expr_types_equal(context, then_type, else_type)
                       ? then_type
                       : inferred_expr_type_unknown();
        }

        case FENG_EXPR_MATCH: {
            InferredExprType result_type =
                block_yield_inferred_type(context, expr->as.match_expr.else_block);

            if (!inferred_expr_type_is_known(result_type)) {
                return result_type;
            }

            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                InferredExprType branch_type =
                    block_yield_inferred_type(context, expr->as.match_expr.branches[index].body);

                if (!inferred_expr_types_equal(context, result_type, branch_type)) {
                    return inferred_expr_type_unknown();
                }
            }

            return result_type;
        }

        default:
            return inferred_expr_type_unknown();
    }
}

static bool lambda_expr_matches_function_type(ResolveContext *context,
                                              const FengExpr *expr,
                                              const FengDecl *function_type_decl) {
    size_t param_index;
    bool ok = true;
    bool matches = false;

    if (expr == NULL || expr->kind != FENG_EXPR_LAMBDA || function_type_decl == NULL ||
        !decl_is_function_type(function_type_decl)) {
        return false;
    }
    if (expr->as.lambda.param_count != function_type_decl->as.spec_decl.as.callable.param_count) {
        return false;
    }

    for (param_index = 0U; param_index < expr->as.lambda.param_count; ++param_index) {
        if (!type_refs_semantically_equal(context,
                                          expr->as.lambda.params[param_index].type,
                                          function_type_decl->as.spec_decl.as.callable.params[param_index].type)) {
            return false;
        }
    }

    if (!resolver_push_scope(context)) {
        return false;
    }

    for (param_index = 0U; param_index < expr->as.lambda.param_count && ok; ++param_index) {
        ok = resolver_add_local_typed_name(context,
                                           expr->as.lambda.params[param_index].name,
                                           inferred_expr_type_from_type_ref(
                                               expr->as.lambda.params[param_index].type),
                                           expr->as.lambda.params[param_index].mutability);
    }
    if (ok) {
        if (expr->as.lambda.is_block_body) {
            InferredExprType expected =
                inferred_expr_type_from_return_type_ref(
                    function_type_decl->as.spec_decl.as.callable.return_type);
            InferredExprType inferred =
                infer_block_return_type(context, expr->as.lambda.body_block);

            if (!inferred_expr_type_is_known(inferred)) {
                inferred = inferred_expr_type_builtin("void");
            }
            matches = inferred_expr_type_is_known(expected) &&
                      inferred_expr_types_equal(context, expected, inferred);
        } else {
            matches = expr_matches_expected_type_ref(
                context,
                expr->as.lambda.body,
                function_type_decl->as.spec_decl.as.callable.return_type);
        }
    }

    resolver_pop_scope(context);
    return ok && matches;
}

static bool lambda_expr_signature_matches_lambda_expr(ResolveContext *context,
                                                      const FengExpr *left,
                                                      const FengExpr *right) {
    size_t param_index;
    InferredExprType left_body_type;
    InferredExprType right_body_type;

    if (left == NULL || right == NULL || left->kind != FENG_EXPR_LAMBDA ||
        right->kind != FENG_EXPR_LAMBDA || left->as.lambda.param_count != right->as.lambda.param_count) {
        return false;
    }

    for (param_index = 0U; param_index < left->as.lambda.param_count; ++param_index) {
        if (!type_refs_semantically_equal(context,
                                          left->as.lambda.params[param_index].type,
                                          right->as.lambda.params[param_index].type)) {
            return false;
        }
    }

    left_body_type = infer_lambda_body_type(context, left);
    right_body_type = infer_lambda_body_type(context, right);
    return inferred_expr_type_is_known(left_body_type) &&
           inferred_expr_type_is_known(right_body_type) &&
           inferred_expr_types_equal(context, left_body_type, right_body_type);
}

static CallableValueResolution resolve_expr_callable_value(ResolveContext *context,
                                                           const FengExpr *expr,
                                                           const FengTypeRef *expected_type_ref) {
    CallableValueResolution result;
    const FengDecl *function_type_decl;
    bool requires_fixed_abi_callable;

    memset(&result, 0, sizeof(result));
    function_type_decl = resolve_function_type_decl(context, expected_type_ref);
    if (expr == NULL || function_type_decl == NULL) {
        return result;
    }

    requires_fixed_abi_callable =
        function_type_decl_is_fixed_abi_callable_type(function_type_decl);

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const LocalNameEntry *local_entry =
                resolver_find_local_name_entry(context, expr->as.identifier);
            const FunctionOverloadSetEntry *overload_set;
            InferredExprType expr_type;

            if (local_entry != NULL) {
                if (inferred_expr_type_matches_type_ref(context, local_entry->type, expected_type_ref) &&
                    (!requires_fixed_abi_callable ||
                     inferred_expr_type_can_match_fixed_abi_callable_type(local_entry->type))) {
                    result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
                    result.lambda_expr = local_entry->type.kind == FENG_INFERRED_EXPR_TYPE_LAMBDA
                                             ? local_entry->type.lambda_expr
                                             : NULL;
                }
                return result;
            }

            overload_set = find_function_overload_set(context->function_sets,
                                                      context->function_set_count,
                                                      expr->as.identifier);
            if (overload_set != NULL) {
                return resolve_top_level_function_value_overload(context,
                                                                 overload_set,
                                                                 function_type_decl);
            }

            expr_type = infer_identifier_expr_type(context, expr->as.identifier);
            if (inferred_expr_type_is_known(expr_type) &&
                inferred_expr_type_matches_type_ref(context, expr_type, expected_type_ref)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            }
            return result;
        }

        case FENG_EXPR_MEMBER: {
            const FengExpr *object = expr->as.member.object;
            InferredExprType expr_type;

            if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
                const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

                if (alias != NULL) {
                    result = resolve_module_public_function_value_overload(context,
                                                                          alias->target_module,
                                                                          expr->as.member.member,
                                                                          function_type_decl);
                    if (result.kind != FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
                        return result;
                    }

                    expr_type = infer_member_expr_type(context, expr);
                    if (inferred_expr_type_is_known(expr_type) &&
                        inferred_expr_type_matches_type_ref(context, expr_type, expected_type_ref)) {
                        result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
                    }
                    return result;
                }
            }

            {
                const FengDecl *owner_type_decl = NULL;
                const FengSemanticModule *provider_module = NULL;

                resolve_expr_owner_type(context, object, &owner_type_decl, &provider_module);
                if (owner_type_decl != NULL && owner_type_decl->kind == FENG_DECL_TYPE) {
                    result = resolve_accessible_method_value_overload(context,
                                                                      owner_type_decl,
                                                                      provider_module,
                                                                      expr->as.member.member,
                                                                      function_type_decl);
                    if (result.kind != FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
                        return result;
                    }
                }
            }

            expr_type = infer_member_expr_type(context, expr);
            if (inferred_expr_type_is_known(expr_type) &&
                inferred_expr_type_matches_type_ref(context, expr_type, expected_type_ref)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            }
            return result;
        }

        case FENG_EXPR_LAMBDA:
            if (requires_fixed_abi_callable) {
                return result;
            }
            if (lambda_expr_matches_function_type(context, expr, function_type_decl)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
                result.lambda_expr = expr;
            }
            return result;

        default: {
            InferredExprType expr_type = infer_expr_type(context, expr);

            if (inferred_expr_type_is_known(expr_type) &&
                inferred_expr_type_matches_type_ref(context, expr_type, expected_type_ref)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            } else if (expr_type_inference_is_pending(context, expr)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            }
            return result;
        }
    }
}

static bool expr_matches_expected_type_ref_when_inference_unknown(
    ResolveContext *context,
    const FengExpr *expr,
    const FengTypeRef *expected_type_ref) {
    size_t item_index;

    if (expr == NULL || expected_type_ref == NULL) {
        return false;
    }
    if (expr->kind != FENG_EXPR_ARRAY_LITERAL || expected_type_ref->kind != FENG_TYPE_REF_ARRAY) {
        return false;
    }

    /* docs/feng-builtin-type.md §5: writable mark must match exactly per layer
     * for type identity; only explicit casts may strip a `!`. */
    if (expr->as.array_literal.element_writable != expected_type_ref->array_element_writable) {
        return false;
    }

    for (item_index = 0U; item_index < expr->as.array_literal.count; ++item_index) {
        if (!expr_matches_expected_type_ref(context,
                                            expr->as.array_literal.items[item_index],
                                            expected_type_ref->as.inner)) {
            return false;
        }
    }

    return true;
}

/* Compile-time range check for an integer literal against a canonical integer target.
 * Per docs/feng-builtin-type.md §17: literals that overflow the target are compile errors. */
static bool integer_literal_fits_canonical_target(int64_t value, const char *canonical_target) {
    if (canonical_target == NULL) {
        return false;
    }
    if (strcmp(canonical_target, "i8") == 0) {
        return value >= INT8_MIN && value <= INT8_MAX;
    }
    if (strcmp(canonical_target, "i16") == 0) {
        return value >= INT16_MIN && value <= INT16_MAX;
    }
    if (strcmp(canonical_target, "i32") == 0) {
        return value >= INT32_MIN && value <= INT32_MAX;
    }
    if (strcmp(canonical_target, "i64") == 0) {
        return true;
    }
    if (strcmp(canonical_target, "u8") == 0) {
        return value >= 0 && value <= (int64_t)UINT8_MAX;
    }
    if (strcmp(canonical_target, "u16") == 0) {
        return value >= 0 && value <= (int64_t)UINT16_MAX;
    }
    if (strcmp(canonical_target, "u32") == 0) {
        return value >= 0 && value <= (int64_t)UINT32_MAX;
    }
    if (strcmp(canonical_target, "u64") == 0) {
        return value >= 0;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Compile-time constant evaluation (per docs/feng-expression.md §3.x and §6.x)
 *
 * Folds integer/float/bool literals, unary `-` `~` `!`, binary arithmetic
 * (`+ - * / %`), bitwise (`& | ^ << >>`), comparisons (`< <= > >= == !=`),
 * logical (`&& ||`), cast `(T)expr` against numeric targets, and identifier
 * references that bind immutably to a foldable initializer.
 *
 * Diagnostics emitted at compile-time:
 *   - integer division/modulo by zero
 *   - integer overflow during + - and * (per i64 semantics; range-narrowing is
 *     deferred to numeric_literal_adapts_to_target's target-type check)
 *
 * Identifier propagation honours immutability: only `let` bindings whose
 * initializer is itself foldable participate. Cycles are guarded via the
 * stack-threaded ConstEvalGuard chain.
 * --------------------------------------------------------------------------- */

static FengConstValue const_int_value(int64_t v) {
    FengConstValue r;
    r.kind = FENG_CONST_INT;
    r.i = v;
    r.f = 0.0;
    r.b = false;
    return r;
}

static FengConstValue const_float_value(double v) {
    FengConstValue r;
    r.kind = FENG_CONST_FLOAT;
    r.i = 0;
    r.f = v;
    r.b = false;
    return r;
}

static FengConstValue const_bool_value(bool v) {
    FengConstValue r;
    r.kind = FENG_CONST_BOOL;
    r.i = 0;
    r.f = 0.0;
    r.b = v;
    return r;
}

/* If either operand is FLOAT, promote the other from INT to FLOAT in-place. */
static bool promote_const_pair(FengConstValue *a, FengConstValue *b) {
    if (a->kind == FENG_CONST_FLOAT && b->kind == FENG_CONST_INT) {
        *b = const_float_value((double)b->i);
    } else if (b->kind == FENG_CONST_FLOAT && a->kind == FENG_CONST_INT) {
        *a = const_float_value((double)a->i);
    }
    return a->kind == b->kind;
}

/* Truncate `value` (treated as an unbounded integer) to the target builtin's bit pattern,
 * mirroring the C-style cast semantics promised by docs/feng-expression.md §3.4
 * ("整数到更小位宽整数的转换会按目标位宽截断高位"). The result is re-encoded as i64. */
static int64_t truncate_int_to_canonical(int64_t value, const char *canonical_target) {
    if (canonical_target == NULL) {
        return value;
    }
    uint64_t bits = (uint64_t)value;
    if (strcmp(canonical_target, "i8") == 0)  return (int64_t)(int8_t)bits;
    if (strcmp(canonical_target, "i16") == 0) return (int64_t)(int16_t)bits;
    if (strcmp(canonical_target, "i32") == 0) return (int64_t)(int32_t)bits;
    if (strcmp(canonical_target, "i64") == 0) return (int64_t)bits;
    if (strcmp(canonical_target, "u8") == 0)  return (int64_t)(uint64_t)(uint8_t)bits;
    if (strcmp(canonical_target, "u16") == 0) return (int64_t)(uint64_t)(uint16_t)bits;
    if (strcmp(canonical_target, "u32") == 0) return (int64_t)(uint64_t)(uint32_t)bits;
    if (strcmp(canonical_target, "u64") == 0) return (int64_t)bits;
    return value;
}

static bool guard_contains(const ConstEvalGuard *g, const FengExpr *expr) {
    while (g != NULL) {
        if (g->expr == expr) {
            return true;
        }
        g = g->prev;
    }
    return false;
}

static bool evaluate_constant_expr_inner(ResolveContext *context,
                                         const FengExpr *expr,
                                         FengConstValue *out,
                                         ConstEvalGuard *guard);

/* Resolve `name` to its compile-time foldable value when the binding is immutable and
 * has an initializer that itself folds. Returns false otherwise. */
static bool evaluate_constant_identifier(ResolveContext *context,
                                         FengSlice name,
                                         FengConstValue *out,
                                         ConstEvalGuard *guard) {
    const LocalNameEntry *local = resolver_find_local_name_entry(context, name);

    if (local != NULL) {
        if (local->mutability != FENG_MUTABILITY_LET || local->source_expr == NULL) {
            return false;
        }
        return evaluate_constant_expr_inner(context, local->source_expr, out, guard);
    }

    {
        const VisibleValueEntry *visible =
            find_visible_value(context->visible_values, context->visible_value_count, name);

        if (visible == NULL || visible->is_function || visible->decl == NULL ||
            visible->decl->kind != FENG_DECL_GLOBAL_BINDING) {
            return false;
        }
        if (visible->decl->as.binding.mutability != FENG_MUTABILITY_LET ||
            visible->decl->as.binding.initializer == NULL) {
            return false;
        }
        return evaluate_constant_expr_inner(context,
                                            visible->decl->as.binding.initializer,
                                            out,
                                            guard);
    }
}

static bool evaluate_constant_unary(ResolveContext *context,
                                    const FengExpr *expr,
                                    FengConstValue *out,
                                    ConstEvalGuard *guard) {
    FengConstValue operand;

    if (!evaluate_constant_expr_inner(context, expr->as.unary.operand, &operand, guard)) {
        return false;
    }

    switch (expr->as.unary.op) {
        case FENG_TOKEN_MINUS:
            if (operand.kind == FENG_CONST_INT) {
                /* Mirror extract_constant_integer_literal: use unsigned arithmetic to
                 * avoid signed-overflow UB on INT64_MIN. */
                *out = const_int_value((int64_t)(0U - (uint64_t)operand.i));
                return true;
            }
            if (operand.kind == FENG_CONST_FLOAT) {
                *out = const_float_value(-operand.f);
                return true;
            }
            return false;
        case FENG_TOKEN_TILDE:
            if (operand.kind != FENG_CONST_INT) return false;
            *out = const_int_value((int64_t)~(uint64_t)operand.i);
            return true;
        case FENG_TOKEN_NOT:
            if (operand.kind != FENG_CONST_BOOL) return false;
            *out = const_bool_value(!operand.b);
            return true;
        default:
            return false;
    }
}

/* Detect i64 overflow on +, -, *. Uses GCC/Clang builtins which both toolchains
 * targeted by Feng provide. */
static bool i64_add_overflow(int64_t a, int64_t b, int64_t *out) {
    return __builtin_add_overflow(a, b, out);
}

static bool i64_sub_overflow(int64_t a, int64_t b, int64_t *out) {
    return __builtin_sub_overflow(a, b, out);
}

static bool i64_mul_overflow(int64_t a, int64_t b, int64_t *out) {
    return __builtin_mul_overflow(a, b, out);
}

static bool report_const_eval_error(ResolveContext *context,
                                    const FengExpr *expr,
                                    const char *message) {
    char *copy = format_message("%s", message);
    return resolver_append_error(context, expr->token, copy);
}

static bool evaluate_constant_binary(ResolveContext *context,
                                     const FengExpr *expr,
                                     FengConstValue *out,
                                     ConstEvalGuard *guard) {
    FengConstValue lhs;
    FengConstValue rhs;
    FengTokenKind op = expr->as.binary.op;

    if (!evaluate_constant_expr_inner(context, expr->as.binary.left, &lhs, guard)) {
        return false;
    }
    if (!evaluate_constant_expr_inner(context, expr->as.binary.right, &rhs, guard)) {
        return false;
    }

    /* Logical operators require BOOL on both sides. */
    if (op == FENG_TOKEN_AND_AND || op == FENG_TOKEN_OR_OR) {
        if (lhs.kind != FENG_CONST_BOOL || rhs.kind != FENG_CONST_BOOL) return false;
        *out = const_bool_value(op == FENG_TOKEN_AND_AND ? (lhs.b && rhs.b) : (lhs.b || rhs.b));
        return true;
    }

    /* Bitwise & shift require INT on both sides. */
    if (op == FENG_TOKEN_AMP || op == FENG_TOKEN_PIPE || op == FENG_TOKEN_CARET) {
        if (lhs.kind != FENG_CONST_INT || rhs.kind != FENG_CONST_INT) return false;
        uint64_t l = (uint64_t)lhs.i;
        uint64_t r = (uint64_t)rhs.i;
        uint64_t v = op == FENG_TOKEN_AMP ? (l & r) : op == FENG_TOKEN_PIPE ? (l | r) : (l ^ r);
        *out = const_int_value((int64_t)v);
        return true;
    }
    if (op == FENG_TOKEN_SHL || op == FENG_TOKEN_SHR) {
        if (lhs.kind != FENG_CONST_INT || rhs.kind != FENG_CONST_INT) return false;
        if (rhs.i < 0 || rhs.i >= 64) return false; /* type-aware diagnostic handled by validator */
        if (op == FENG_TOKEN_SHL) {
            *out = const_int_value((int64_t)((uint64_t)lhs.i << (uint64_t)rhs.i));
        } else {
            /* Arithmetic shift right preserves sign for signed values; for unsigned ones
             * carried in i64 this is a known-loss operation but matches the bit-pattern
             * model used elsewhere in the evaluator. */
            *out = const_int_value(lhs.i >> rhs.i);
        }
        return true;
    }

    /* Comparisons: allow INT/FLOAT mixed (with promotion) or matching kinds. */
    if (op == FENG_TOKEN_LT || op == FENG_TOKEN_LE || op == FENG_TOKEN_GT ||
        op == FENG_TOKEN_GE || op == FENG_TOKEN_EQ || op == FENG_TOKEN_NE) {
        if ((lhs.kind == FENG_CONST_INT || lhs.kind == FENG_CONST_FLOAT) &&
            (rhs.kind == FENG_CONST_INT || rhs.kind == FENG_CONST_FLOAT)) {
            promote_const_pair(&lhs, &rhs);
            bool result;
            if (lhs.kind == FENG_CONST_INT) {
                int64_t a = lhs.i, b = rhs.i;
                switch (op) {
                    case FENG_TOKEN_LT:     result = a < b;  break;
                    case FENG_TOKEN_LE:     result = a <= b; break;
                    case FENG_TOKEN_GT:     result = a > b;  break;
                    case FENG_TOKEN_GE:     result = a >= b; break;
                    case FENG_TOKEN_EQ:     result = a == b; break;
                    default:                result = a != b; break;
                }
            } else {
                double a = lhs.f, b = rhs.f;
                switch (op) {
                    case FENG_TOKEN_LT:     result = a < b;  break;
                    case FENG_TOKEN_LE:     result = a <= b; break;
                    case FENG_TOKEN_GT:     result = a > b;  break;
                    case FENG_TOKEN_GE:     result = a >= b; break;
                    case FENG_TOKEN_EQ:     result = a == b; break;
                    default:                result = a != b; break;
                }
            }
            *out = const_bool_value(result);
            return true;
        }
        if (lhs.kind == FENG_CONST_BOOL && rhs.kind == FENG_CONST_BOOL &&
            (op == FENG_TOKEN_EQ || op == FENG_TOKEN_NE)) {
            *out = const_bool_value(op == FENG_TOKEN_EQ ? (lhs.b == rhs.b) : (lhs.b != rhs.b));
            return true;
        }
        return false;
    }

    /* Arithmetic: +, -, *, /, %. */
    if (op == FENG_TOKEN_PLUS || op == FENG_TOKEN_MINUS || op == FENG_TOKEN_STAR ||
        op == FENG_TOKEN_SLASH || op == FENG_TOKEN_PERCENT) {
        if (!(lhs.kind == FENG_CONST_INT || lhs.kind == FENG_CONST_FLOAT) ||
            !(rhs.kind == FENG_CONST_INT || rhs.kind == FENG_CONST_FLOAT)) {
            return false;
        }
        promote_const_pair(&lhs, &rhs);
        if (lhs.kind == FENG_CONST_INT) {
            int64_t result = 0;
            switch (op) {
                case FENG_TOKEN_PLUS:
                    if (i64_add_overflow(lhs.i, rhs.i, &result)) {
                        report_const_eval_error(context, expr,
                            "integer overflow in compile-time '+' expression");
                        return false;
                    }
                    break;
                case FENG_TOKEN_MINUS:
                    if (i64_sub_overflow(lhs.i, rhs.i, &result)) {
                        report_const_eval_error(context, expr,
                            "integer overflow in compile-time '-' expression");
                        return false;
                    }
                    break;
                case FENG_TOKEN_STAR:
                    if (i64_mul_overflow(lhs.i, rhs.i, &result)) {
                        report_const_eval_error(context, expr,
                            "integer overflow in compile-time '*' expression");
                        return false;
                    }
                    break;
                case FENG_TOKEN_SLASH:
                    if (rhs.i == 0) {
                        report_const_eval_error(context, expr,
                            "division by zero in compile-time '/' expression");
                        return false;
                    }
                    /* INT64_MIN / -1 overflows; treat as compile-time error for parity with
                     * the +, -, * checks above. */
                    if (lhs.i == INT64_MIN && rhs.i == -1) {
                        report_const_eval_error(context, expr,
                            "integer overflow in compile-time '/' expression");
                        return false;
                    }
                    result = lhs.i / rhs.i;
                    break;
                case FENG_TOKEN_PERCENT:
                    if (rhs.i == 0) {
                        report_const_eval_error(context, expr,
                            "modulo by zero in compile-time '%' expression");
                        return false;
                    }
                    if (lhs.i == INT64_MIN && rhs.i == -1) {
                        result = 0;
                    } else {
                        result = lhs.i % rhs.i;
                    }
                    break;
                default:
                    return false;
            }
            *out = const_int_value(result);
            return true;
        }
        /* FLOAT path: IEEE-754 semantics, no compile-time error on /0.0 (yields ±inf/NaN). */
        double a = lhs.f, b = rhs.f, result = 0.0;
        switch (op) {
            case FENG_TOKEN_PLUS:    result = a + b; break;
            case FENG_TOKEN_MINUS:   result = a - b; break;
            case FENG_TOKEN_STAR:    result = a * b; break;
            case FENG_TOKEN_SLASH:   result = a / b; break;
                case FENG_TOKEN_PERCENT:
                    if (b == 0.0) {
                        report_const_eval_error(context, expr,
                            "modulo by zero in compile-time '%' expression");
                        return false;
                    }
                    result = fmod(a, b);
                    break;
            default:                 return false;
        }
        *out = const_float_value(result);
        return true;
    }

    return false;
}

static bool evaluate_constant_cast(ResolveContext *context,
                                   const FengExpr *expr,
                                   FengConstValue *out,
                                   ConstEvalGuard *guard) {
    FengConstValue inner;
    const char *target;

    if (!evaluate_constant_expr_inner(context, expr->as.cast.value, &inner, guard)) {
        return false;
    }
    target = type_ref_builtin_canonical_name(expr->as.cast.type);
    if (target == NULL) {
        return false;
    }
    /* Numeric-to-numeric only (per docs/feng-expression.md §3.4). bool ↔ numeric is a
     * static error caught by cast_expr_types_are_valid; we don't fold across that boundary. */
    if (inner.kind == FENG_CONST_INT) {
        if (strcmp(target, "f32") == 0 || strcmp(target, "f64") == 0) {
            *out = const_float_value((double)inner.i);
            return true;
        }
        if (canonical_integer_bit_width(target) > 0) {
            *out = const_int_value(truncate_int_to_canonical(inner.i, target));
            return true;
        }
        return false;
    }
    if (inner.kind == FENG_CONST_FLOAT) {
        if (strcmp(target, "f32") == 0 || strcmp(target, "f64") == 0) {
            *out = const_float_value(inner.f);
            return true;
        }
        if (canonical_integer_bit_width(target) > 0) {
            /* C-style truncation toward zero, then narrow to target bit width. */
            int64_t truncated = (int64_t)inner.f;
            *out = const_int_value(truncate_int_to_canonical(truncated, target));
            return true;
        }
        return false;
    }
    return false;
}

static bool evaluate_constant_expr_inner(ResolveContext *context,
                                         const FengExpr *expr,
                                         FengConstValue *out,
                                         ConstEvalGuard *guard) {
    ConstEvalGuard frame;

    if (expr == NULL || out == NULL) {
        return false;
    }
    if (guard_contains(guard, expr)) {
        return false; /* cycle */
    }
    frame.expr = expr;
    frame.prev = guard;

    switch (expr->kind) {
        case FENG_EXPR_INTEGER:
            *out = const_int_value(expr->as.integer);
            return true;
        case FENG_EXPR_FLOAT:
            *out = const_float_value(expr->as.floating);
            return true;
        case FENG_EXPR_BOOL:
            *out = const_bool_value(expr->as.boolean);
            return true;
        case FENG_EXPR_IDENTIFIER:
            return evaluate_constant_identifier(context, expr->as.identifier, out, &frame);
        case FENG_EXPR_UNARY:
            return evaluate_constant_unary(context, expr, out, &frame);
        case FENG_EXPR_BINARY:
            return evaluate_constant_binary(context, expr, out, &frame);
        case FENG_EXPR_CAST:
            return evaluate_constant_cast(context, expr, out, &frame);
        default:
            return false;
    }
}

static bool evaluate_constant_expr(ResolveContext *context,
                                   const FengExpr *expr,
                                   FengConstValue *out) {
    return evaluate_constant_expr_inner(context, expr, out, NULL);
}

/* Numeric literal adaptation: an integer/float literal may be implicitly retyped to any
 * compatible numeric target type, subject to compile-time range checks. This realises the
 * "如需其他精度或宽度，必须显式标注类型" contract from docs/feng-builtin-type.md §16-§17,
 * where the explicit annotation drives the literal's effective type. */
static bool numeric_literal_adapts_to_target(ResolveContext *context,
                                             const FengExpr *expr,
                                             const FengTypeRef *target) {
    const char *canonical_target = type_ref_builtin_canonical_name(target);
    FengConstValue value;

    if (canonical_target == NULL) {
        return false;
    }
    if (!evaluate_constant_expr(context, expr, &value)) {
        return false;
    }
    if (value.kind == FENG_CONST_INT) {
        return integer_literal_fits_canonical_target(value.i, canonical_target);
    }
    if (value.kind == FENG_CONST_FLOAT) {
        return strcmp(canonical_target, "f32") == 0 || strcmp(canonical_target, "f64") == 0;
    }
    return false;
}

static bool expr_matches_expected_type_ref(ResolveContext *context,
                                           const FengExpr *expr,
                                           const FengTypeRef *expected_type_ref) {
    const FengDecl *function_type_decl = resolve_function_type_decl(context, expected_type_ref);
    InferredExprType expr_type;

    if (function_type_decl != NULL) {
        return resolve_expr_callable_value(context, expr, expected_type_ref).kind ==
               FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
    }

    /* Step 4b-γ — array literals targeting an array type drive matching
     * element-by-element so that per-element coercions (e.g. concrete object
     * to spec) succeed even when the literal's inferred element type differs
     * nominally from the expected element type. The recursion lives in
     * expr_matches_expected_type_ref_when_inference_unknown. */
    if (expr != NULL && expr->kind == FENG_EXPR_ARRAY_LITERAL &&
        expected_type_ref != NULL && expected_type_ref->kind == FENG_TYPE_REF_ARRAY) {
        return expr_matches_expected_type_ref_when_inference_unknown(context,
                                                                     expr,
                                                                     expected_type_ref);
    }

    /* For numeric literal constants targeting a built-in numeric type, the literal-adaptation
     * path (with compile-time range checking per docs/feng-builtin-type.md §17) is the sole
     * authority — falling through to the default-inferred-type path would let oversized
     * literals like `let c: i32 = 9999999999;` slip past because the literal's *default*
     * inferred type happens to match the target's canonical name. */
    if (type_ref_builtin_canonical_name(expected_type_ref) != NULL) {
        FengConstValue value;
        size_t errors_before = *context->error_count;

        if (evaluate_constant_expr(context, expr, &value) &&
            (value.kind == FENG_CONST_INT || value.kind == FENG_CONST_FLOAT)) {
            return numeric_literal_adapts_to_target(context, expr, expected_type_ref);
        }
        /* If the constant evaluator reported a compile-time error (e.g., division by zero,
         * overflow), the binding is already known invalid; treat as matched to suppress a
         * cascading inference-mismatch diagnostic. */
        if (*context->error_count > errors_before) {
            return true;
        }
    }

    expr_type = infer_expr_type(context, expr);
    if (!inferred_expr_type_is_known(expr_type)) {
        if (expr_type_inference_is_pending(context, expr)) {
            return true;
        }
        return expr_matches_expected_type_ref_when_inference_unknown(context,
                                                                     expr,
                                                                     expected_type_ref);
    }

    return inferred_expr_type_matches_type_ref(context, expr_type, expected_type_ref);
}

static bool expr_requires_explicit_function_type_context(ResolveContext *context,
                                                         const FengExpr *expr) {
    if (expr == NULL) {
        return false;
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const FunctionOverloadSetEntry *overload_set;

            if (resolver_find_local_name_entry(context, expr->as.identifier) != NULL) {
                return false;
            }

            overload_set = find_function_overload_set(context->function_sets,
                                                      context->function_set_count,
                                                      expr->as.identifier);
            return overload_set != NULL && overload_set->decl_count > 1U;
        }

        case FENG_EXPR_MEMBER: {
            const FengExpr *object = expr->as.member.object;

            if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
                const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

                if (alias != NULL) {
                    return count_module_public_function_overloads(alias->target_module,
                                                                  expr->as.member.member) > 1U;
                }
            }

            {
                const FengDecl *owner_type_decl = NULL;
                const FengSemanticModule *provider_module = NULL;

                resolve_expr_owner_type(context, object, &owner_type_decl, &provider_module);
                return count_accessible_method_overloads(context,
                                                         owner_type_decl,
                                                         provider_module,
                                                         expr->as.member.member) > 1U;
            }
        }

        default:
            return false;
    }
}

static bool expr_is_callable_value_reference(ResolveContext *context, const FengExpr *expr) {
    if (expr == NULL) {
        return false;
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const FunctionOverloadSetEntry *overload_set;

            if (resolver_find_local_name_entry(context, expr->as.identifier) != NULL) {
                return false;
            }

            overload_set = find_function_overload_set(context->function_sets,
                                                      context->function_set_count,
                                                      expr->as.identifier);
            return overload_set != NULL;
        }

        case FENG_EXPR_MEMBER: {
            const FengExpr *object = expr->as.member.object;

            if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
                const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

                if (alias != NULL) {
                    return count_module_public_function_overloads(alias->target_module,
                                                                  expr->as.member.member) > 0U;
                }
            }

            {
                const FengDecl *owner_type_decl = NULL;
                const FengSemanticModule *provider_module = NULL;

                resolve_expr_owner_type(context, object, &owner_type_decl, &provider_module);
                return count_accessible_method_overloads(context,
                                                         owner_type_decl,
                                                         provider_module,
                                                         expr->as.member.member) > 0U;
            }
        }

        default:
            return false;
    }
}

/* Phase S1b — SpecCoercionSite recording helpers (§6.2). */

static const FengDecl *concrete_type_decl_of_inferred(const ResolveContext *context,
                                                      InferredExprType expr_type) {
    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_DECL:
            if (expr_type.type_decl != NULL &&
                expr_type.type_decl->kind == FENG_DECL_TYPE) {
                return expr_type.type_decl;
            }
            return NULL;
        case FENG_INFERRED_EXPR_TYPE_TYPE_REF: {
            const FengDecl *d = resolve_type_ref_decl(context, expr_type.type_ref);
            return (d != NULL && d->kind == FENG_DECL_TYPE) ? d : NULL;
        }
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
        default:
            return NULL;
    }
}

/* Records an object-form coercion site when expected_type_ref names an
 * object-form spec and expr's static type is a concrete `type` decl that
 * satisfies it. Silent no-op for any other shape (numeric coercion, builtin
 * match, lambda body inference re-entries, etc.). */
static void record_object_spec_coercion_site_if_applicable(
        ResolveContext *context,
        const FengExpr *expr,
        const FengTypeRef *expected_type_ref) {
    const FengDecl *target_decl;
    const FengDecl *src_type_decl;
    InferredExprType expr_type;
    const FengSpecRelation *relation;

    if (context == NULL || context->analysis == NULL || expr == NULL ||
        expected_type_ref == NULL) {
        return;
    }
    /* Step 4b-γ — array literals targeting an array type get per-element
     * coercion sites recorded so codegen wraps each element into the spec
     * fat value before the array storage is shaped. */
    if (expr->kind == FENG_EXPR_ARRAY_LITERAL &&
        expected_type_ref->kind == FENG_TYPE_REF_ARRAY &&
        expected_type_ref->as.inner != NULL) {
        size_t i;
        for (i = 0U; i < expr->as.array_literal.count; ++i) {
            record_object_spec_coercion_site_if_applicable(context,
                                                            expr->as.array_literal.items[i],
                                                            expected_type_ref->as.inner);
        }
        return;
    }
    target_decl = resolve_type_ref_decl(context, expected_type_ref);
    if (target_decl == NULL || target_decl->kind != FENG_DECL_SPEC) {
        return;
    }
    /* Callable-form specs have their own callable hook (§8.4). */
    if (decl_is_function_type(target_decl)) {
        return;
    }
    expr_type = infer_expr_type(context, expr);
    src_type_decl = concrete_type_decl_of_inferred(context, expr_type);
    if (src_type_decl == NULL) {
        return;
    }
    relation = feng_semantic_lookup_spec_relation(context->analysis,
                                                  src_type_decl,
                                                  target_decl);
    if (relation == NULL) {
        /* Match must have been by exact type identity (src == target spec is
         * impossible since target is SPEC and src is TYPE), so absence here
         * means the match was a non-spec path (should not happen given the
         * SPEC target check above). Defensive no-op. */
        return;
    }
    (void)feng_semantic_record_object_spec_coercion_site(context->analysis,
                                                          expr,
                                                          src_type_decl,
                                                          target_decl,
                                                          relation);

    /* Phase S3 — materialise the (T, S) witness on first demand (§8.2). The
     * compute helper is idempotent: subsequent coercions for the same pair
     * observe the cached entry and skip recomputation, so per-coercion-site
     * conflict diagnostics fire exactly once at the first triggering site
     * (per §8.1 / §8.2 — "复用同一份 SpecWitness 结果"). */
    compute_spec_witness_if_absent(context, src_type_decl, target_decl, expr->token);
}

/* Records object-form coercion sites for each argument of a call against the
 * resolved callee parameter list. Per-argument application of
 * record_object_spec_coercion_site_if_applicable handles the predicate
 * (target is object-form spec, src is concrete type, satisfaction relation
 * exists); arguments that don't match the predicate are silently skipped. */
static void record_object_arg_coercion_sites(ResolveContext *context,
                                             FengExpr *const *args,
                                             size_t arg_count,
                                             const FengParameter *params,
                                             size_t param_count) {
    size_t i;

    if (params == NULL || args == NULL || param_count != arg_count) {
        return;
    }
    for (i = 0U; i < arg_count; ++i) {
        record_object_spec_coercion_site_if_applicable(context,
                                                        args[i],
                                                        params[i].type);
    }
}

static FengSpecCoercionCallableSource classify_callable_source(
        const ResolveContext *context, const FengExpr *expr) {
    if (expr == NULL) {
        return FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER;
    }
    switch (expr->kind) {
        case FENG_EXPR_LAMBDA:
            return FENG_SPEC_COERCION_CALLABLE_SOURCE_LAMBDA;
        case FENG_EXPR_IDENTIFIER: {
            /* If the identifier resolves to a local binding/parameter, treat
             * as a generic callable value. Otherwise, if it names a top-level
             * function (overload set), classify as TOP_LEVEL_FN. */
            if (resolver_find_local_name_entry(context, expr->as.identifier) != NULL) {
                return FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER;
            }
            if (find_function_overload_set(context->function_sets,
                                           context->function_set_count,
                                           expr->as.identifier) != NULL) {
                return FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN;
            }
            return FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER;
        }
        case FENG_EXPR_MEMBER: {
            /* `mod.fn` (alias-qualified top-level function) reports as
             * TOP_LEVEL_FN; `obj.method` reports as METHOD_VALUE. */
            const FengExpr *object = expr->as.member.object;
            if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER &&
                find_unshadowed_alias(context, object->as.identifier) != NULL) {
                return FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN;
            }
            return FENG_SPEC_COERCION_CALLABLE_SOURCE_METHOD_VALUE;
        }
        default:
            return FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER;
    }
}

static void record_callable_spec_coercion_site(ResolveContext *context,
                                               const FengExpr *expr,
                                               const FengTypeRef *expected_type_ref) {
    const FengDecl *target_decl;

    if (context == NULL || context->analysis == NULL || expr == NULL ||
        expected_type_ref == NULL) {
        return;
    }
    target_decl = resolve_function_type_decl(context, expected_type_ref);
    if (target_decl == NULL) {
        return;
    }
    (void)feng_semantic_record_callable_spec_coercion_site(
        context->analysis,
        expr,
        target_decl,
        classify_callable_source(context, expr));
}

/* Phase S2-a — SpecDefaultBinding recording helper (§6.3). Records a
 * default-witness site when `binding_type` resolves to a spec decl (object
 * or callable form). Caller is responsible for ensuring this is invoked
 * only when the slot has no initializer. Silent no-op when the type does
 * not resolve to a spec. */
static void record_spec_default_binding_if_applicable(
        ResolveContext *context,
        const void *site,
        FengSpecDefaultBindingPosition position,
        const FengTypeRef *binding_type) {
    const FengDecl *decl;
    FengSpecCoercionForm form;

    if (context == NULL || context->analysis == NULL || site == NULL ||
        binding_type == NULL) {
        return;
    }
    decl = resolve_type_ref_decl(context, binding_type);
    if (decl == NULL || decl->kind != FENG_DECL_SPEC) {
        return;
    }
    form = decl_is_function_type(decl) ? FENG_SPEC_COERCION_FORM_CALLABLE
                                       : FENG_SPEC_COERCION_FORM_OBJECT;
    (void)feng_semantic_record_spec_default_binding(context->analysis,
                                                    site,
                                                    position,
                                                    form,
                                                    decl);
}

/* Phase S2-b — SpecMemberAccess recording helper (§6.4). Records a member
 * access site for an `obj.member` expression whose owner static type is an
 * object-form spec. Recorded kind is METHOD_CALL for method members and
 * FIELD_READ for field members; the assignment statement resolver later
 * upgrades FIELD_READ to FIELD_WRITE for assignment LHS positions. */
static void record_spec_member_access(ResolveContext *context,
                                      const FengExpr *expr,
                                      const FengDecl *spec_decl,
                                      const FengTypeMember *member) {
    FengSpecMemberAccessKind kind;

    if (context == NULL || context->analysis == NULL || expr == NULL ||
        spec_decl == NULL || member == NULL) {
        return;
    }
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        kind = FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_READ;
    } else {
        kind = FENG_SPEC_MEMBER_ACCESS_KIND_METHOD_CALL;
    }
    (void)feng_semantic_record_spec_member_access(context->analysis,
                                                  expr,
                                                  spec_decl,
                                                  member,
                                                  kind);
}

/* Phase S4 — SpecEquality recording helper (§6.6). Records an equality
 * site for a binary `==` / `!=` expression whose operands' static type is
 * an object-form spec. The semantic conclusion is "reference-identity
 * comparison"; codegen consults the sidecar to keep spec equality
 * decoupled from the value-equality path used by string / array. */
static void record_spec_equality_if_applicable(ResolveContext *context,
                                               const FengExpr *expr) {
    InferredExprType left_type;
    const FengDecl *spec_decl;
    FengSpecEqualityOp op;

    if (context == NULL || context->analysis == NULL || expr == NULL) {
        return;
    }
    if (expr->kind != FENG_EXPR_BINARY) {
        return;
    }
    if (expr->as.binary.op != FENG_TOKEN_EQ && expr->as.binary.op != FENG_TOKEN_NE) {
        return;
    }
    /* validate_binary_expr has already asserted both operands have the
     * same static type and that the type is "known"; checking only the
     * left operand is sufficient. */
    left_type = infer_expr_type(context, expr->as.binary.left);
    spec_decl = resolve_inferred_expr_type_decl(context, left_type);
    if (spec_decl == NULL || spec_decl->kind != FENG_DECL_SPEC) {
        return;
    }
    /* Object-form specs only — callable-form specs cannot be operands of
     * `==` / `!=` (binary_expr_types_are_valid would not have accepted a
     * function-typed operand here). Defensive guard. */
    if (spec_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
        return;
    }
    op = expr->as.binary.op == FENG_TOKEN_EQ
             ? FENG_SPEC_EQUALITY_OP_EQ
             : FENG_SPEC_EQUALITY_OP_NE;
    (void)feng_semantic_record_spec_equality(context->analysis, expr,
                                             spec_decl, op);
}

static bool validate_function_typed_expr(ResolveContext *context,
                                         const FengExpr *expr,
                                         const FengTypeRef *expected_type_ref) {
    CallableValueResolution resolution;
    char *expr_name;
    char *type_name;

    if (expr == NULL || resolve_function_type_decl(context, expected_type_ref) == NULL) {
        return true;
    }

    resolution = resolve_expr_callable_value(context, expr, expected_type_ref);
    if (resolution.kind == FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE) {
        record_callable_spec_coercion_site(context, expr, expected_type_ref);
        return true;
    }

    expr_name = format_expr_target_name(expr);
    type_name = format_type_ref_name(expected_type_ref);

    if (resolution.kind == FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS) {
        bool ok = resolver_append_error(
            context,
            expr->token,
            format_message("expression '%s' has multiple overloads matching expected function type '%s'",
                           expr_name != NULL ? expr_name : "<expression>",
                           type_name != NULL ? type_name : "<type>"));

        free(expr_name);
        free(type_name);
        return ok;
    }

    {
        bool ok = resolver_append_error(
            context,
            expr->token,
            format_message("expression '%s' does not match expected function type '%s'",
                           expr_name != NULL ? expr_name : "<expression>",
                           type_name != NULL ? type_name : "<type>"));

        free(expr_name);
        free(type_name);
        return ok;
    }
}

static bool validate_expr_against_expected_type(ResolveContext *context,
                                                const FengExpr *expr,
                                                const FengTypeRef *expected_type_ref) {
    InferredExprType expr_type;
    char *expr_name;
    char *type_name;

    if (expr == NULL || expected_type_ref == NULL) {
        return true;
    }
    if (resolve_function_type_decl(context, expected_type_ref) != NULL) {
        return validate_function_typed_expr(context, expr, expected_type_ref);
    }
    if (expr_matches_expected_type_ref(context, expr, expected_type_ref)) {
        record_object_spec_coercion_site_if_applicable(context, expr, expected_type_ref);
        return true;
    }

    expr_type = infer_expr_type(context, expr);
    if (!inferred_expr_type_is_known(expr_type)) {
        if (!expr_is_callable_value_reference(context, expr)) {
            return true;
        }

        expr_name = format_expr_target_name(expr);
        type_name = format_type_ref_name(expected_type_ref);
        if (!resolver_append_error(context,
                                   expr->token,
                                   format_message("expression '%s' does not match expected type '%s'",
                                                  expr_name != NULL ? expr_name : "<expression>",
                                                  type_name != NULL ? type_name : "<type>"))) {
            free(expr_name);
            free(type_name);
            return false;
        }

        free(expr_name);
        free(type_name);
        return true;
    }

    expr_name = format_expr_target_name(expr);
    type_name = format_type_ref_name(expected_type_ref);
    if (!resolver_append_error(context,
                               expr->token,
                               format_message("expression '%s' does not match expected type '%s'",
                                              expr_name != NULL ? expr_name : "<expression>",
                                              type_name != NULL ? type_name : "<type>"))) {
        free(expr_name);
        free(type_name);
        return false;
    }

    free(expr_name);
    free(type_name);
    return true;
}

static char *format_inferred_expr_type_name(InferredExprType type) {
    switch (type.kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN: {
            const char *builtin_name = canonical_builtin_type_name(type.builtin_name);

            return duplicate_cstr(builtin_name != NULL ? builtin_name : "<type>");
        }

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return format_type_ref_name(type.type_ref);

        case FENG_INFERRED_EXPR_TYPE_DECL:
            if (type.type_decl != NULL && type.type_decl->kind == FENG_DECL_TYPE) {
                return format_module_name(&type.type_decl->as.type_decl.name, 1U);
            }
            return duplicate_cstr("<type>");

        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
            return duplicate_cstr("<function>");

        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return duplicate_cstr("<type>");
    }

    return duplicate_cstr("<type>");
}

static bool fixed_abi_trace_contains(const FixedAbiTrace *trace, const FengDecl *decl) {
    while (trace != NULL) {
        if (trace->decl == decl) {
            return true;
        }
        trace = trace->parent;
    }

    return false;
}

static bool type_decl_is_fixed_abi_stable(const ResolveContext *context,
                                          const FengDecl *decl,
                                          const FixedAbiTrace *trace);

static bool type_ref_is_fixed_abi_stable(const ResolveContext *context,
                                         const FengTypeRef *type_ref,
                                         bool allow_void,
                                         const FixedAbiTrace *trace) {
    const FengDecl *type_decl;
    const char *builtin_name;

    if (type_ref == NULL) {
        return allow_void;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            if (type_ref->as.named.segment_count == 1U) {
                builtin_name = canonical_builtin_type_name(type_ref->as.named.segments[0]);
                if (builtin_name != NULL) {
                    if (strcmp(builtin_name, "void") == 0) {
                        return allow_void;
                    }
                    return strcmp(builtin_name, "string") != 0;
                }
            }

            type_decl = resolve_type_ref_decl(context, type_ref);
            return type_decl_is_fixed_abi_stable(context, type_decl, trace);

        case FENG_TYPE_REF_POINTER:
            return true;

        case FENG_TYPE_REF_ARRAY:
            return false;
    }

    return false;
}

static bool inferred_expr_type_is_fixed_abi_stable(const ResolveContext *context,
                                                   InferredExprType type,
                                                   bool allow_void,
                                                   const FixedAbiTrace *trace) {
    const char *builtin_name;

    switch (type.kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
            builtin_name = canonical_builtin_type_name(type.builtin_name);
            if (builtin_name == NULL) {
                return false;
            }
            if (strcmp(builtin_name, "void") == 0) {
                return allow_void;
            }
            return strcmp(builtin_name, "string") != 0;

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return type_ref_is_fixed_abi_stable(context, type.type_ref, allow_void, trace);

        case FENG_INFERRED_EXPR_TYPE_DECL:
            return type_decl_is_fixed_abi_stable(context, type.type_decl, trace);

        case FENG_INFERRED_EXPR_TYPE_LAMBDA:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return false;
    }

    return false;
}

static bool type_decl_is_fixed_abi_stable(const ResolveContext *context,
                                          const FengDecl *decl,
                                          const FixedAbiTrace *trace) {
    FixedAbiTrace next_trace;
    size_t member_index;
    size_t param_index;

    if (decl == NULL || (decl->kind != FENG_DECL_TYPE && decl->kind != FENG_DECL_SPEC) ||
        !annotations_contain_kind(decl->annotations, decl->annotation_count, FENG_ANNOTATION_FIXED) ||
        fixed_abi_trace_contains(trace, decl)) {
        return false;
    }

    /* Object-form `spec` only constrains visible shape, not memory layout or
     * ABI value layout, so it can never be a fixed ABI-stable type even if the
     * annotation is present. The actual diagnostic is emitted by
     * `validate_fixed_type_declaration`; here we simply refuse to treat it as
     * stable so dependent types do not transitively appear ABI-stable. */
    if (decl->kind == FENG_DECL_SPEC && decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
        return false;
    }

    next_trace.decl = decl;
    next_trace.parent = trace;

    if (count_calling_convention_annotations(decl->annotations, decl->annotation_count) != 0U) {
        return false;
    }

    if (decl_is_function_type(decl)) {
        if (annotations_contain_kind(decl->annotations, decl->annotation_count, FENG_ANNOTATION_UNION)) {
            return false;
        }

        for (param_index = 0U; param_index < decl->as.spec_decl.as.callable.param_count; ++param_index) {
            if (!type_ref_is_fixed_abi_stable(context,
                                              decl->as.spec_decl.as.callable.params[param_index].type,
                                              false,
                                              &next_trace)) {
                return false;
            }
        }

        return type_ref_is_fixed_abi_stable(
            context, decl->as.spec_decl.as.callable.return_type, true, &next_trace);
    }

    for (member_index = 0U; member_index < decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = decl->as.type_decl.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_FIELD) {
            continue;
        }
        if (!type_ref_is_fixed_abi_stable(context, member->as.field.type, false, &next_trace)) {
            return false;
        }
    }

    return true;
}

static bool validate_type_member_overloads(ResolveContext *context, const FengDecl *decl) {
    size_t i;
    size_t j;
    bool ok = true;

    if (context == NULL || decl == NULL || decl->kind != FENG_DECL_TYPE) {
        return true;
    }

    for (i = 0U; i < decl->as.type_decl.member_count; ++i) {
        const FengTypeMember *mi = decl->as.type_decl.members[i];
        const FengCallableSignature *si;

        if (mi == NULL || mi->kind != FENG_TYPE_MEMBER_METHOD) {
            continue;
        }
        si = &mi->as.callable;

        for (j = 0U; j < i; ++j) {
            const FengTypeMember *mj = decl->as.type_decl.members[j];
            const FengCallableSignature *sj;

            if (mj == NULL || mj->kind != FENG_TYPE_MEMBER_METHOD) {
                continue;
            }
            sj = &mj->as.callable;
            if (!slice_equals(si->name, sj->name)) {
                continue;
            }
            if (parameters_equal(si, sj)) {
                if (return_type_equals(si->return_type, sj->return_type)) {
                    ok = resolver_append_error(
                             context,
                             si->token,
                             format_message(
                                 "duplicate method signature '%.*s' in type '%.*s'",
                                 (int)si->name.length, si->name.data,
                                 (int)decl->as.type_decl.name.length,
                                 decl->as.type_decl.name.data)) && ok;
                } else {
                    ok = resolver_append_error(
                             context,
                             si->token,
                             format_message(
                                 "method overloads in type '%.*s' cannot differ only by return type: '%.*s'",
                                 (int)decl->as.type_decl.name.length,
                                 decl->as.type_decl.name.data,
                                 (int)si->name.length, si->name.data)) && ok;
                }
            }
        }
    }

    return ok;
}

static bool validate_fixed_type_declaration(ResolveContext *context, const FengDecl *decl) {
    FixedAbiTrace trace;
    size_t callconv_count;
    size_t field_index;
    size_t param_index;
    bool has_fixed;
    bool has_union;

    if (context == NULL || decl == NULL || (decl->kind != FENG_DECL_TYPE && decl->kind != FENG_DECL_SPEC)) {
        return true;
    }

    has_fixed = annotations_contain_kind(decl->annotations, decl->annotation_count, FENG_ANNOTATION_FIXED);
    has_union = annotations_contain_kind(decl->annotations, decl->annotation_count, FENG_ANNOTATION_UNION);
    callconv_count = count_calling_convention_annotations(decl->annotations, decl->annotation_count);

    /* Object-form `spec` only describes a visible shape contract; it does not
     * fix memory layout or ABI value layout, so it cannot enter the C ABI
     * boundary. Reject @fixed, @union and calling-convention annotations on
     * object-form spec declarations with an explicit, actionable diagnostic. */
    if (decl->kind == FENG_DECL_SPEC && decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
        if (has_fixed) {
            return resolver_append_error(
                context,
                decl->token,
                format_message(
                    "object-form spec '%.*s' cannot be marked as @fixed; @fixed only applies to type declarations and callable-form spec",
                    (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
        }
        if (has_union) {
            return resolver_append_error(
                context,
                decl->token,
                format_message(
                    "spec '%.*s' cannot use @union; @union only applies to object-form @fixed type declarations",
                    (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
        }
        if (callconv_count != 0U) {
            return resolver_append_error(
                context,
                decl->token,
                format_message(
                    "spec '%.*s' cannot use calling convention annotations",
                    (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
        }
        return true;
    }

    if (!has_fixed) {
        if (has_union) {
            return resolver_append_error(
                context,
                decl->token,
                format_message(
                    "type '%.*s' cannot use @union unless it is marked as @fixed and declared in object form",
                    (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
        }
        if (callconv_count != 0U) {
            return resolver_append_error(
                context,
                decl->token,
                format_message("type '%.*s' cannot use calling convention annotations",
                               (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
        }
        return true;
    }

    if (callconv_count != 0U) {
        return resolver_append_error(
            context,
            decl->token,
            format_message(
                "type '%.*s' cannot be marked as @fixed because calling convention annotations do not apply to type declarations",
                (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
    }

    if (decl_is_function_type(decl) && has_union) {
        return resolver_append_error(
            context,
            decl->token,
            format_message(
                "type '%.*s' cannot be marked as @fixed because @union only applies to object-form @fixed type declarations",
                (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data));
    }

    trace.decl = decl;
    trace.parent = NULL;

    if (decl_is_function_type(decl)) {
        for (param_index = 0U; param_index < decl->as.spec_decl.as.callable.param_count; ++param_index) {
            const FengParameter *param = &decl->as.spec_decl.as.callable.params[param_index];
            char *type_name;
            bool ok;

            if (type_ref_is_fixed_abi_stable(context, param->type, false, &trace)) {
                continue;
            }

            type_name = format_type_ref_name(param->type);
            ok = resolver_append_error(
                context,
                param->token,
                format_message(
                    "type '%.*s' cannot be marked as @fixed because parameter '%.*s' uses non-ABI-stable type '%s'",
                    (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data,
                    (int)param->name.length,
                    param->name.data,
                    type_name != NULL ? type_name : "<type>"));
            free(type_name);
            return ok;
        }

        if (!type_ref_is_fixed_abi_stable(
                context, decl->as.spec_decl.as.callable.return_type, true, &trace)) {
            char *type_name = format_type_ref_name(decl->as.spec_decl.as.callable.return_type);
            bool ok = resolver_append_error(
                context,
                decl->token,
                format_message(
                    "type '%.*s' cannot be marked as @fixed because return type '%s' is not ABI-stable",
                    (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data,
                    type_name != NULL ? type_name : "<type>"));
            free(type_name);
            return ok;
        }

        return true;
    }

    for (field_index = 0U; field_index < decl->as.type_decl.member_count; ++field_index) {
        const FengTypeMember *member = decl->as.type_decl.members[field_index];
        char *type_name;
        bool ok;

        if (member->kind != FENG_TYPE_MEMBER_FIELD ||
            type_ref_is_fixed_abi_stable(context, member->as.field.type, false, &trace)) {
            continue;
        }

        type_name = format_type_ref_name(member->as.field.type);
        ok = resolver_append_error(
            context,
            member->token,
            format_message(
                "type '%.*s' cannot be marked as @fixed because field '%.*s' uses non-ABI-stable type '%s'",
                (int)decl_typeish_name(decl).length,
                    decl_typeish_name(decl).data,
                (int)member->as.field.name.length,
                member->as.field.name.data,
                type_name != NULL ? type_name : "<type>"));
        free(type_name);
        return ok;
    }

    return true;
}

static bool validate_fixed_callable_signature(ResolveContext *context,
                                             FengToken token,
                                             FengSlice name,
                                             const FengAnnotation *annotations,
                                             size_t annotation_count,
                                             const FengCallableSignature *callable,
                                             const char *callable_kind,
                                             bool is_extern) {
    const FengAnnotation *calling_convention;
    size_t callconv_count;
    bool has_fixed;
    bool has_union;
    size_t param_index;

    if (context == NULL || callable == NULL) {
        return true;
    }

    has_fixed = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_FIXED);
    has_union = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_UNION);
    callconv_count = count_calling_convention_annotations(annotations, annotation_count);
    calling_convention = find_calling_convention_annotation(annotations, annotation_count);

    if (!has_fixed) {
        if (has_union) {
            return resolver_append_error(
                context,
                token,
                format_message("%s '%.*s' cannot use @union",
                               callable_kind,
                               (int)name.length,
                               name.data));
        }
        if (callconv_count != 0U && !is_extern) {
            return resolver_append_error(
                context,
                calling_convention != NULL ? calling_convention->token : token,
                format_message(
                    "%s '%.*s' cannot use calling convention annotations unless it is marked as @fixed or declared extern",
                    callable_kind,
                    (int)name.length,
                    name.data));
        }
        return true;
    }

    if (is_extern) {
        return resolver_append_error(
            context,
            token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because extern functions declare imported C symbols",
                callable_kind,
                (int)name.length,
                name.data));
    }

    if (has_union) {
        return resolver_append_error(
            context,
            token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because @union only applies to object-form @fixed type declarations",
                callable_kind,
                (int)name.length,
                name.data));
    }

    if (callconv_count > 1U) {
        return resolver_append_error(
            context,
            token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because it uses more than one calling convention annotation",
                callable_kind,
                (int)name.length,
                name.data));
    }

    if (calling_convention != NULL && calling_convention->arg_count != 0U) {
        return resolver_append_error(
            context,
            calling_convention->token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because calling convention annotations on @fixed declarations must not take library arguments",
                callable_kind,
                (int)name.length,
                name.data));
    }

    for (param_index = 0U; param_index < callable->param_count; ++param_index) {
        const FengParameter *param = &callable->params[param_index];
        char *type_name;
        bool ok;

        if (type_ref_is_fixed_abi_stable(context, param->type, false, NULL)) {
            continue;
        }

        type_name = format_type_ref_name(param->type);
        ok = resolver_append_error(
            context,
            param->token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because parameter '%.*s' uses non-ABI-stable type '%s'",
                callable_kind,
                (int)name.length,
                name.data,
                (int)param->name.length,
                param->name.data,
                type_name != NULL ? type_name : "<type>"));
        free(type_name);
        return ok;
    }

    if (callable_return_inference_is_pending(context, callable)) {
        return true;
    }

    if (!inferred_expr_type_is_fixed_abi_stable(
            context, callable_effective_return_type(context, callable), true, NULL)) {
        char *type_name = format_inferred_expr_type_name(callable_effective_return_type(context, callable));
        bool ok = resolver_append_error(
            context,
            callable->return_type != NULL ? callable->return_type->token : token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because return type '%s' is not ABI-stable",
                callable_kind,
                (int)name.length,
                name.data,
                type_name != NULL ? type_name : "<type>"));
        free(type_name);
        return ok;
    }

    if (callable_may_escape_exception(context, callable)) {
        return resolver_append_error(
            context,
            token,
            format_message(
                "%s '%.*s' cannot be marked as @fixed because uncaught exceptions must not cross the @fixed ABI boundary",
                callable_kind,
                (int)name.length,
                name.data));
    }

    return true;
}

static bool type_ref_is_extern_c_abi_compatible(const ResolveContext *context,
                                                const FengTypeRef *type_ref,
                                                bool allow_void) {
    if (type_ref != NULL && type_ref->kind == FENG_TYPE_REF_NAMED &&
        type_ref->as.named.segment_count == 1U) {
        const char *builtin_name = canonical_builtin_type_name(type_ref->as.named.segments[0]);

        if (builtin_name != NULL && strcmp(builtin_name, "string") == 0) {
            return true;
        }
    }

    return type_ref_is_fixed_abi_stable(context, type_ref, allow_void, NULL);
}

static bool validate_extern_function_signature(ResolveContext *context, const FengDecl *decl) {
    const FengCallableSignature *callable;
    size_t param_index;

    if (context == NULL || decl == NULL || decl->kind != FENG_DECL_FUNCTION || !decl->is_extern) {
        return true;
    }

    callable = &decl->as.function_decl;
    for (param_index = 0U; param_index < callable->param_count; ++param_index) {
        const FengParameter *param = &callable->params[param_index];
        char *type_name;

        if (type_ref_is_extern_c_abi_compatible(context, param->type, false)) {
            continue;
        }

        type_name = format_type_ref_name(param->type);
        if (!resolver_append_error(
                context,
                param->token,
                format_message(
                    "extern function '%.*s' parameter '%.*s' type '%s' is not C ABI-stable",
                    (int)callable->name.length,
                    callable->name.data,
                    (int)param->name.length,
                    param->name.data,
                    type_name != NULL ? type_name : "<type>"))) {
            free(type_name);
            return false;
        }

        free(type_name);
        return true;
    }

    if (!type_ref_is_extern_c_abi_compatible(context, callable->return_type, true)) {
        char *type_name = format_type_ref_name(callable->return_type);
        bool ok = resolver_append_error(
            context,
            callable->token,
            format_message("extern function '%.*s' return type '%s' is not C ABI-stable",
                           (int)callable->name.length,
                           callable->name.data,
                           type_name != NULL ? type_name : "<type>"));

        free(type_name);
        return ok;
    }

    return true;
}

static bool validate_fixed_function_declaration(ResolveContext *context, const FengDecl *decl) {
    if (decl == NULL || decl->kind != FENG_DECL_FUNCTION) {
        return true;
    }

    return validate_fixed_callable_signature(context,
                                             decl->as.function_decl.token,
                                             decl->as.function_decl.name,
                                             decl->annotations,
                                             decl->annotation_count,
                                             &decl->as.function_decl,
                                             "function",
                                             decl->is_extern);
}

static bool validate_fixed_callable_member(ResolveContext *context, const FengTypeMember *member) {
    bool has_fixed;
    bool has_union;
    size_t callconv_count;

    if (member == NULL || member->kind == FENG_TYPE_MEMBER_FIELD) {
        return true;
    }

    has_fixed = annotations_contain_kind(member->annotations, member->annotation_count, FENG_ANNOTATION_FIXED);
    has_union = annotations_contain_kind(member->annotations, member->annotation_count, FENG_ANNOTATION_UNION);
    callconv_count = count_calling_convention_annotations(member->annotations, member->annotation_count);

    if (member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR) {
        if (!has_fixed && !has_union && callconv_count == 0U) {
            return true;
        }

        return resolver_append_error(
            context,
            member->token,
            format_message(
                "constructor '%.*s' cannot use ABI annotations",
                (int)member->as.callable.name.length,
                member->as.callable.name.data));
    }

    if (member->kind == FENG_TYPE_MEMBER_FINALIZER) {
        if (!has_fixed && !has_union && callconv_count == 0U) {
            return true;
        }

        return resolver_append_error(
            context,
            member->token,
            format_message(
                "finalizer '~%.*s' cannot use ABI annotations",
                (int)member->as.callable.name.length,
                member->as.callable.name.data));
    }

    return validate_fixed_callable_signature(context,
                                             member->token,
                                             member->as.callable.name,
                                             member->annotations,
                                             member->annotation_count,
                                             &member->as.callable,
                                             "method",
                                             false);
}

static bool validate_expr_against_expected_inferred_type(ResolveContext *context,
                                                         const FengExpr *expr,
                                                         InferredExprType expected_type) {
    InferredExprType expr_type;
    char *expr_name;
    char *type_name;

    if (expr == NULL || !inferred_expr_type_is_known(expected_type)) {
        return true;
    }
    if (expected_type.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF) {
        return validate_expr_against_expected_type(context, expr, expected_type.type_ref);
    }

    expr_type = infer_expr_type(context, expr);
    if (!inferred_expr_type_is_known(expr_type) ||
        inferred_expr_types_equal(context, expr_type, expected_type)) {
        return true;
    }

    expr_name = format_expr_target_name(expr);
    type_name = format_inferred_expr_type_name(expected_type);
    if (!resolver_append_error(context,
                               expr->token,
                               format_message("expression '%s' does not match expected type '%s'",
                                              expr_name != NULL ? expr_name : "<expression>",
                                              type_name != NULL ? type_name : "<type>"))) {
        free(expr_name);
        free(type_name);
        return false;
    }

    free(expr_name);
    free(type_name);
    return true;
}

static bool validate_untyped_callable_value_expr(ResolveContext *context, const FengExpr *expr) {
    char *expr_name;

    if (!expr_requires_explicit_function_type_context(context, expr)) {
        return true;
    }

    expr_name = format_expr_target_name(expr);
    if (!resolver_append_error(context,
                               expr != NULL ? expr->token : context->program->module_token,
                               format_message("expression '%s' requires an explicit target function type to resolve overloads",
                                              expr_name != NULL ? expr_name : "<expression>"))) {
        free(expr_name);
        return false;
    }

    free(expr_name);
    return true;
}

static bool validate_untyped_array_literal_expr(ResolveContext *context, const FengExpr *expr) {
    if (expr == NULL || expr->kind != FENG_EXPR_ARRAY_LITERAL || expr->as.array_literal.count != 0U) {
        return true;
    }

    return resolver_append_error(context,
                                 expr->token,
                                 format_message("empty array literal requires an explicit target array type"));
}

static bool validate_constructor_invocation(ResolveContext *context,
                                            const FengExpr *target_expr,
                                            const FengDecl *type_decl,
                                            const FengSemanticModule *provider_module,
                                            FengExpr *const *args,
                                            size_t arg_count,
                                            const FengTypeMember **out_constructor) {
    size_t declared_constructor_count;
    ConstructorResolution resolution;

    if (out_constructor != NULL) {
        *out_constructor = NULL;
    }

    if (type_decl == NULL) {
        return true;
    }

    if (type_decl->kind != FENG_DECL_TYPE) {
        FengSlice name = decl_typeish_name(type_decl);
        const char *kind_label = type_decl->kind == FENG_DECL_SPEC ? "spec" : "type";
        bool ok = resolver_append_error(
            context,
            target_expr != NULL ? target_expr->token : context->program->module_token,
            format_message("%s '%.*s' is not an object type and cannot be constructed",
                           kind_label,
                           (int)name.length,
                           name.data));

        return ok;
    }

    declared_constructor_count = count_declared_constructors(type_decl);
    if (declared_constructor_count == 0U) {
        if (arg_count == 0U) {
            return true;
        }

        return resolver_append_error(
            context,
            target_expr != NULL ? target_expr->token : context->program->module_token,
            format_message("type '%.*s' has no constructor accepting %zu argument(s)",
                           (int)type_decl->as.type_decl.name.length,
                           type_decl->as.type_decl.name.data,
                           arg_count));
    }

    resolution = resolve_accessible_constructor_overload(
        context, type_decl, provider_module, args, arg_count);
    if (resolution.kind == FENG_CONSTRUCTOR_RESOLUTION_UNIQUE) {
        note_callable_exception_escape(context,
                                       resolution.constructor != NULL
                                           ? &resolution.constructor->as.callable
                                           : NULL);
        if (out_constructor != NULL) {
            *out_constructor = resolution.constructor;
        }
        return true;
    }
    if (resolution.kind == FENG_CONSTRUCTOR_RESOLUTION_AMBIGUOUS) {
        return resolver_append_error(
            context,
            target_expr != NULL ? target_expr->token : context->program->module_token,
            format_message("type '%.*s' has multiple accessible constructors matching %zu argument(s); argument types are ambiguous",
                           (int)type_decl->as.type_decl.name.length,
                           type_decl->as.type_decl.name.data,
                           arg_count));
    }

    return resolver_append_error(
        context,
        target_expr != NULL ? target_expr->token : context->program->module_token,
        format_message("type '%.*s' has no accessible constructor accepting %zu argument(s)",
                       (int)type_decl->as.type_decl.name.length,
                       type_decl->as.type_decl.name.data,
                       arg_count));
}

static bool validate_constructor_call_expr(ResolveContext *context, const FengExpr *expr) {
    ResolvedTypeTarget target = resolve_type_target_expr(context, expr->as.call.callee, false);
    const FengTypeMember *constructor_member = NULL;

    if (target.type_decl == NULL) {
        return true;
    }

    if (!validate_constructor_invocation(context,
                                         expr->as.call.callee,
                                         target.type_decl,
                                         target.provider_module,
                                         expr->as.call.args,
                                         expr->as.call.arg_count,
                                         &constructor_member)) {
        return false;
    }

    if (target.type_decl->kind == FENG_DECL_TYPE) {
        FengExpr *mutable_expr = (FengExpr *)expr;
        mutable_expr->as.call.resolved_callable.kind = FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR;
        mutable_expr->as.call.resolved_callable.owner_type_decl = target.type_decl;
        mutable_expr->as.call.resolved_callable.member = constructor_member;
        if (constructor_member != NULL &&
            constructor_member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR) {
            record_object_arg_coercion_sites(context,
                                             expr->as.call.args,
                                             expr->as.call.arg_count,
                                             constructor_member->as.callable.params,
                                             constructor_member->as.callable.param_count);
        }
    }
    return true;
}

static void record_resolved_callable_from_resolution(
    const FengExpr *call_expr, const FunctionCallResolution *resolution) {
    FengExpr *mutable_expr = (FengExpr *)call_expr;
    FengResolvedCallable *slot;

    if (mutable_expr == NULL || mutable_expr->kind != FENG_EXPR_CALL ||
        resolution == NULL ||
        resolution->kind != FENG_FUNCTION_CALL_RESOLUTION_UNIQUE) {
        return;
    }

    slot = &mutable_expr->as.call.resolved_callable;
    if (resolution->fit_decl != NULL) {
        slot->kind = FENG_RESOLVED_CALLABLE_FIT_METHOD;
        slot->owner_type_decl = resolution->owner_type_decl;
        slot->member = resolution->member;
        slot->fit_decl = resolution->fit_decl;
        return;
    }
    if (resolution->member != NULL) {
        slot->kind = FENG_RESOLVED_CALLABLE_TYPE_METHOD;
        slot->owner_type_decl = resolution->owner_type_decl;
        slot->member = resolution->member;
        return;
    }
    if (resolution->decl != NULL) {
        slot->kind = FENG_RESOLVED_CALLABLE_FUNCTION;
        slot->function_decl = resolution->decl;
        return;
    }
}

static bool validate_function_call_expr(ResolveContext *context, const FengExpr *expr) {
    const FengExpr *callee = expr->as.call.callee;

    if (callee == NULL) {
        return true;
    }

    if (callee->kind == FENG_EXPR_MEMBER) {
        const FengExpr *object = callee->as.member.object;
        const FengDecl *owner_type_decl = NULL;
        const FengSemanticModule *provider_module = NULL;
        FunctionCallResolution resolution;

        if (object != NULL && object->kind == FENG_EXPR_IDENTIFIER) {
            const AliasEntry *alias = find_unshadowed_alias(context, object->as.identifier);

            if (alias != NULL) {
                resolution = resolve_module_public_function_overload(context,
                                                                    alias->target_module,
                                                                    callee->as.member.member,
                                                                    expr->as.call.args,
                                                                    expr->as.call.arg_count);

                if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE) {
                    note_callable_exception_escape(context, resolution.callable);
                    record_resolved_callable_from_resolution(expr, &resolution);
                    record_object_arg_coercion_sites(context,
                                                     expr->as.call.args,
                                                     expr->as.call.arg_count,
                                                     resolution.callable->params,
                                                     resolution.callable->param_count);
                    return true;
                }
                if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS) {
                    return resolver_append_error(
                        context,
                        callee->token,
                        format_message("function '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous",
                                       (int)object->as.identifier.length,
                                       object->as.identifier.data,
                                       (int)callee->as.member.member.length,
                                       callee->as.member.member.data,
                                       expr->as.call.arg_count));
                }
                if (find_module_public_function_decl(alias->target_module, callee->as.member.member) != NULL) {
                    return resolver_append_error(
                        context,
                        callee->token,
                        format_message("function '%.*s.%.*s' has no overload accepting %zu argument(s)",
                                       (int)object->as.identifier.length,
                                       object->as.identifier.data,
                                       (int)callee->as.member.member.length,
                                       callee->as.member.member.data,
                                       expr->as.call.arg_count));
                }

                return validate_callable_typed_expr_call(context,
                                                         callee,
                                                         expr->as.call.args,
                                                         expr->as.call.arg_count);
            }
        }

        resolve_expr_owner_type(context, object, &owner_type_decl, &provider_module);
        if (owner_type_decl != NULL && owner_type_decl->kind == FENG_DECL_TYPE) {
            const FengTypeMember *accessible_method =
                find_accessible_type_method_member(context,
                                                   owner_type_decl,
                                                   provider_module,
                                                   callee->as.member.member);
            const FengTypeMember *field_member =
                find_type_field_member(owner_type_decl, callee->as.member.member);
            const FengTypeMember *accessible_field =
                find_accessible_type_field_member(context,
                                                  owner_type_decl,
                                                  provider_module,
                                                  callee->as.member.member);

            resolution = resolve_accessible_method_overload(context,
                                                            owner_type_decl,
                                                            provider_module,
                                                            callee->as.member.member,
                                                            expr->as.call.args,
                                                            expr->as.call.arg_count);

            if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE) {
                note_callable_exception_escape(context, resolution.callable);
                record_resolved_callable_from_resolution(expr, &resolution);
                record_object_arg_coercion_sites(context,
                                                 expr->as.call.args,
                                                 expr->as.call.arg_count,
                                                 resolution.callable->params,
                                                 resolution.callable->param_count);
                return true;
            }
            if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS) {
                return resolver_append_error(
                    context,
                    callee->token,
                    format_message("method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous",
                                   (int)owner_type_decl->as.type_decl.name.length,
                                   owner_type_decl->as.type_decl.name.data,
                                   (int)callee->as.member.member.length,
                                   callee->as.member.member.data,
                                   expr->as.call.arg_count));
            }
            if (accessible_method != NULL) {
                return resolver_append_error(
                    context,
                    callee->token,
                    format_message("method '%.*s.%.*s' has no overload accepting %zu argument(s)",
                                   (int)owner_type_decl->as.type_decl.name.length,
                                   owner_type_decl->as.type_decl.name.data,
                                   (int)callee->as.member.member.length,
                                   callee->as.member.member.data,
                                   expr->as.call.arg_count));
            }
            if (field_member != NULL && accessible_field == NULL) {
                return true;
            }
            if (find_type_method_member(owner_type_decl, callee->as.member.member) != NULL) {
                return true;
            }
            if (accessible_field != NULL) {
                return validate_callable_typed_expr_call(context,
                                                         callee,
                                                         expr->as.call.args,
                                                         expr->as.call.arg_count);
            }
        }

        return validate_callable_typed_expr_call(context,
                                                 callee,
                                                 expr->as.call.args,
                                                 expr->as.call.arg_count);
    }

    if (callee->kind == FENG_EXPR_IDENTIFIER) {
        if (resolver_find_local_name_entry(context, callee->as.identifier) != NULL) {
            return validate_callable_typed_expr_call(context,
                                                     callee,
                                                     expr->as.call.args,
                                                     expr->as.call.arg_count);
        }

        {
            const FunctionOverloadSetEntry *overload_set =
                find_function_overload_set(context->function_sets,
                                           context->function_set_count,
                                           callee->as.identifier);

            if (overload_set != NULL) {
                FunctionCallResolution resolution =
                    resolve_top_level_function_overload(context,
                                                        overload_set,
                                                        expr->as.call.args,
                                                        expr->as.call.arg_count);

                if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_UNIQUE) {
                    note_callable_exception_escape(context, resolution.callable);
                    record_resolved_callable_from_resolution(expr, &resolution);
                    record_object_arg_coercion_sites(context,
                                                     expr->as.call.args,
                                                     expr->as.call.arg_count,
                                                     resolution.callable->params,
                                                     resolution.callable->param_count);
                    return true;
                }
                if (resolution.kind == FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS) {
                    return resolver_append_error(
                        context,
                        callee->token,
                        format_message("top-level function '%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous",
                                       (int)callee->as.identifier.length,
                                       callee->as.identifier.data,
                                       expr->as.call.arg_count));
                }

                return resolver_append_error(
                    context,
                    callee->token,
                    format_message("top-level function '%.*s' has no overload accepting %zu argument(s)",
                                   (int)callee->as.identifier.length,
                                   callee->as.identifier.data,
                                   expr->as.call.arg_count));
            }
        }

        return validate_callable_typed_expr_call(context,
                                                 callee,
                                                 expr->as.call.args,
                                                 expr->as.call.arg_count);
    }

    return validate_callable_typed_expr_call(context,
                                             callee,
                                             expr->as.call.args,
                                             expr->as.call.arg_count);
}

static bool validate_object_literal_expr(ResolveContext *context, const FengExpr *expr) {
    ResolvedTypeTarget target;
    char *target_name;
    FengSlice *seen_field_names = NULL;
    size_t seen_field_count = 0U;
    size_t seen_field_capacity = 0U;
    size_t field_index;

    target = resolve_type_target_expr(context, expr->as.object_literal.target, true);
    target_name = format_expr_target_name(expr->as.object_literal.target);

    if (target.type_decl == NULL || target.type_decl->kind != FENG_DECL_TYPE) {
        bool ok = resolver_append_error(
            context,
            expr->token,
            format_message("object literal target '%s' must resolve to an object type",
                           target_name != NULL ? target_name : "<expression>"));

        free(target_name);
        free(seen_field_names);
        return ok;
    }

    if (expr->as.object_literal.target->kind != FENG_EXPR_CALL &&
        !validate_constructor_invocation(context,
                                         expr->as.object_literal.target,
                                         target.type_decl,
                                         target.provider_module,
                                         NULL,
                                         0U,
                                         NULL)) {
        free(target_name);
        free(seen_field_names);
        return false;
    }

    for (field_index = 0U; field_index < expr->as.object_literal.field_count; ++field_index) {
        const FengObjectFieldInit *field = &expr->as.object_literal.fields[field_index];
        const FengTypeMember *field_member;

        if (find_slice_index(seen_field_names, seen_field_count, field->name) < seen_field_count) {
            bool ok = resolver_append_error(
                context,
                field->token,
                format_message("duplicate object literal field '%.*s' for type '%.*s'",
                               (int)field->name.length,
                               field->name.data,
                               (int)target.type_decl->as.type_decl.name.length,
                               target.type_decl->as.type_decl.name.data));

            free(target_name);
            free(seen_field_names);
            return ok;
        }
        if (!append_slice(&seen_field_names, &seen_field_count, &seen_field_capacity, field->name)) {
            free(target_name);
            free(seen_field_names);
            return false;
        }

        field_member = find_type_field_member(target.type_decl, field->name);
        if (field_member == NULL) {
            bool ok = resolver_append_error(
                context,
                field->token,
                format_message("object literal field '%.*s' is not a field of type '%.*s'",
                               (int)field->name.length,
                               field->name.data,
                               (int)target.type_decl->as.type_decl.name.length,
                               target.type_decl->as.type_decl.name.data));

            free(target_name);
            free(seen_field_names);
            return ok;
        }

        if (!type_member_is_accessible_from(context, target.provider_module, field_member) ||
            fit_body_blocks_private_access(context, target.type_decl, field_member)) {
            bool ok = resolver_append_error(
                context,
                field->token,
                format_message("object literal field '%.*s' is not accessible for type '%.*s'",
                               (int)field->name.length,
                               field->name.data,
                               (int)target.type_decl->as.type_decl.name.length,
                               target.type_decl->as.type_decl.name.data));

            free(target_name);
            free(seen_field_names);
            return ok;
        }

        if (!validate_let_field_object_literal_binding(context,
                                                       target.type_decl,
                                                       target.provider_module,
                                                       expr->as.object_literal.target,
                                                       field)) {
            free(target_name);
            free(seen_field_names);
            return false;
        }
    }

    free(target_name);
    free(seen_field_names);
    return true;
}

static bool resolve_self_member_expr(ResolveContext *context,
                                     const FengExpr *expr,
                                     bool allow_self) {
    if (expr->kind != FENG_EXPR_MEMBER || expr->as.member.object == NULL ||
        expr->as.member.object->kind != FENG_EXPR_SELF) {
        return false;
    }

    if (!allow_self || context->current_type_decl == NULL) {
        return resolve_expr(context, expr->as.member.object, allow_self);
    }

    if (context->lambda_frame_count > 0U) {
        resolver_record_self_capture(context);
    }

    if (find_instance_member(context->current_type_decl, expr->as.member.member) != NULL) {
        const FengTypeMember *self_member =
            find_instance_member(context->current_type_decl, expr->as.member.member);

        if (fit_body_blocks_private_access(context, context->current_type_decl, self_member)) {
            return resolver_append_error(
                context,
                expr->token,
                format_message("fit body cannot access private member '%.*s' of target type '%.*s'",
                               self_member->kind == FENG_TYPE_MEMBER_FIELD
                                   ? (int)self_member->as.field.name.length
                                   : (int)self_member->as.callable.name.length,
                               self_member->kind == FENG_TYPE_MEMBER_FIELD
                                   ? self_member->as.field.name.data
                                   : self_member->as.callable.name.data,
                               (int)context->current_type_decl->as.type_decl.name.length,
                               context->current_type_decl->as.type_decl.name.data));
        }
        return true;
    }

    if (find_fit_method_member_for_type(context, context->current_type_decl,
                                        expr->as.member.member) != NULL) {
        return true;
    }

    return resolver_append_error(
        context,
        expr->token,
        format_message("type '%.*s' has no member '%.*s'",
                       context->current_type_decl != NULL ? (int)context->current_type_decl->as.type_decl.name.length
                                                         : 0,
                       context->current_type_decl != NULL ? context->current_type_decl->as.type_decl.name.data : "",
                       (int)expr->as.member.member.length,
                       expr->as.member.member.data));
}

static bool append_error(FengSemanticError **errors,
                         size_t *error_count,
                         size_t *error_capacity,
                         const char *path,
                         FengToken token,
                         char *message) {
    FengSemanticError error;

    if (message == NULL) {
        message = duplicate_cstr("out of memory during semantic analysis");
        if (message == NULL) {
            return false;
        }
    }

    error.path = path;
    error.message = message;
    error.token = token;

    if (!append_raw((void **)errors,
                    error_count,
                    error_capacity,
                    sizeof(error),
                    &error)) {
        free(message);
        return false;
    }

    return true;
}

static bool analysis_append_info(const FengSemanticAnalysis *analysis_const,
                                 const char *path,
                                 FengToken token,
                                 char *message) {
    FengSemanticAnalysis *analysis;
    FengSemanticInfo info;

    if (analysis_const == NULL) {
        free(message);
        return true;
    }
    analysis = (FengSemanticAnalysis *)analysis_const;
    if (message == NULL) {
        message = duplicate_cstr("out of memory during semantic analysis");
        if (message == NULL) {
            return false;
        }
    }
    info.path = path;
    info.message = message;
    info.token = token;
    if (!append_raw((void **)&analysis->infos,
                    &analysis->info_count,
                    &analysis->info_capacity,
                    sizeof(info),
                    &info)) {
        free(message);
        return false;
    }
    return true;
}

static bool append_module_program(FengSemanticModule *module, const FengProgram *program) {
    return append_raw((void **)&module->programs,
                      &module->program_count,
                      &module->program_capacity,
                      sizeof(module->programs[0]),
                      &program);
}

static bool add_module(FengSemanticAnalysis *analysis, const FengProgram *program) {
    FengSemanticModule module;

    memset(&module, 0, sizeof(module));
    module.segments = program->module_segments;
    module.segment_count = program->module_segment_count;
    module.visibility = program->module_visibility;

    if (!append_module_program(&module, program)) {
        free(module.programs);
        return false;
    }

    if (!append_raw((void **)&analysis->modules,
                    &analysis->module_count,
                    &analysis->module_capacity,
                    sizeof(module),
                    &module)) {
        free(module.programs);
        return false;
    }

    return true;
}

static bool import_public_names(const FengSemanticModule *target_module,
                                const FengProgram *program,
                                const FengUseDecl *use_decl,
                                VisibleTypeEntry **visible_types,
                                size_t *visible_type_count,
                                size_t *visible_type_capacity,
                                VisibleValueEntry **visible_values,
                                size_t *visible_value_count,
                                size_t *visible_value_capacity,
                                FunctionOverloadSetEntry **function_sets,
                                size_t *function_set_count,
                                size_t *function_set_capacity,
                                FengSemanticError **errors,
                                size_t *error_count,
                                size_t *error_capacity) {
    FengSlice *seen_type_names = NULL;
    FengSlice *seen_value_names = NULL;
    size_t seen_type_count = 0U;
    size_t seen_value_count = 0U;
    size_t seen_type_capacity = 0U;
    size_t seen_value_capacity = 0U;
    char *module_name = format_module_name(target_module->segments, target_module->segment_count);
    size_t program_index;
    bool ok = true;

    for (program_index = 0U; program_index < target_module->program_count && ok; ++program_index) {
        const FengProgram *target_program = target_module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < target_program->declaration_count && ok; ++decl_index) {
            const FengDecl *decl = target_program->declarations[decl_index];
            FengSlice name;
            size_t index;

            if (!decl_is_public(decl)) {
                continue;
            }

            switch (decl->kind) {
                case FENG_DECL_TYPE: {
                    VisibleTypeEntry entry;

                    name = decl->as.type_decl.name;
                    if (find_slice_index(seen_type_names, seen_type_count, name) < seen_type_count) {
                        break;
                    }
                    if (!append_slice(&seen_type_names, &seen_type_count, &seen_type_capacity, name)) {
                        ok = false;
                        break;
                    }

                    index = find_visible_type_index(*visible_types, *visible_type_count, name);
                    if (index < *visible_type_count) {
                        if ((*visible_types)[index].provider_module == target_module) {
                            break;
                        }
                        ok = append_error(
                            errors,
                            error_count,
                            error_capacity,
                            program->path,
                            use_decl->token,
                            format_message(
                                "imported type '%.*s' from module '%s' conflicts with an existing visible type name",
                                (int)name.length,
                                name.data,
                                module_name != NULL ? module_name : "<unknown>"));
                        break;
                    }

                    entry.name = name;
                    entry.provider_module = target_module;
                    entry.decl = decl;
                    ok = append_raw((void **)visible_types,
                                    visible_type_count,
                                    visible_type_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_GLOBAL_BINDING:
                case FENG_DECL_FUNCTION: {
                    VisibleValueEntry entry;
                    bool should_append_visible_value = true;

                    name = (decl->kind == FENG_DECL_FUNCTION) ? decl->as.function_decl.name : decl->as.binding.name;
                    if (find_slice_index(seen_value_names, seen_value_count, name) < seen_value_count) {
                        should_append_visible_value = false;
                    }
                    if (should_append_visible_value &&
                        !append_slice(&seen_value_names, &seen_value_count, &seen_value_capacity, name)) {
                        ok = false;
                        break;
                    }

                    if (should_append_visible_value) {
                        index = find_visible_value_index(*visible_values, *visible_value_count, name);
                        if (index < *visible_value_count) {
                            if ((*visible_values)[index].provider_module == target_module) {
                                should_append_visible_value = false;
                            } else {
                                ok = append_error(
                                    errors,
                                    error_count,
                                    error_capacity,
                                    program->path,
                                    use_decl->token,
                                    format_message(
                                        "imported name '%.*s' from module '%s' conflicts with an existing visible value name",
                                        (int)name.length,
                                        name.data,
                                        module_name != NULL ? module_name : "<unknown>"));
                                break;
                            }
                        }
                    }

                    if (should_append_visible_value) {
                        entry.name = name;
                        entry.provider_module = target_module;
                        entry.decl = decl;
                        entry.mutability = decl->kind == FENG_DECL_GLOBAL_BINDING
                                               ? decl->as.binding.mutability
                                               : FENG_MUTABILITY_DEFAULT;
                        entry.is_function = (decl->kind == FENG_DECL_FUNCTION);
                        ok = append_raw((void **)visible_values,
                                        visible_value_count,
                                        visible_value_capacity,
                                        sizeof(entry),
                                        &entry);
                        if (!ok) {
                            break;
                        }
                    }

                    if (decl->kind == FENG_DECL_FUNCTION) {
                        ok = append_visible_function_overload(function_sets,
                                                              function_set_count,
                                                              function_set_capacity,
                                                              target_module,
                                                              decl);
                    }
                    break;
                }

                case FENG_DECL_SPEC: {
                    VisibleTypeEntry entry;

                    name = decl->as.spec_decl.name;
                    if (find_slice_index(seen_type_names, seen_type_count, name) < seen_type_count) {
                        break;
                    }
                    if (!append_slice(&seen_type_names, &seen_type_count, &seen_type_capacity, name)) {
                        ok = false;
                        break;
                    }

                    index = find_visible_type_index(*visible_types, *visible_type_count, name);
                    if (index < *visible_type_count) {
                        if ((*visible_types)[index].provider_module == target_module) {
                            break;
                        }
                        ok = append_error(
                            errors,
                            error_count,
                            error_capacity,
                            program->path,
                            use_decl->token,
                            format_message(
                                "imported type '%.*s' from module '%s' conflicts with an existing visible type name",
                                (int)name.length,
                                name.data,
                                module_name != NULL ? module_name : "<unknown>"));
                        break;
                    }

                    entry.name = name;
                    entry.provider_module = target_module;
                    entry.decl = decl;
                    ok = append_raw((void **)visible_types,
                                    visible_type_count,
                                    visible_type_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_FIT:
                    /* fit declarations are not exported as named symbols. */
                    break;
            }
        }
    }

    free(module_name);
    free(seen_type_names);
    free(seen_value_names);
    return ok;
}

static bool build_program_aliases(const FengSemanticAnalysis *analysis,
                                  const FengProgram *program,
                                  AliasEntry **aliases,
                                  size_t *alias_count,
                                  size_t *alias_capacity) {
    size_t use_index;

    for (use_index = 0U; use_index < program->use_count; ++use_index) {
        const FengUseDecl *use_decl = &program->uses[use_index];
        size_t target_index;
        AliasEntry entry;

        if (!use_decl->has_alias) {
            continue;
        }
        if (find_alias_index(*aliases, *alias_count, use_decl->alias) < *alias_count) {
            continue;
        }

        target_index = find_module_index_by_path(analysis, use_decl->segments, use_decl->segment_count);
        if (target_index == analysis->module_count) {
            continue;
        }

        entry.alias = use_decl->alias;
        entry.target_module = &analysis->modules[target_index];
        entry.use_decl = use_decl;
        if (!append_raw((void **)aliases,
                        alias_count,
                        alias_capacity,
                        sizeof(entry),
                        &entry)) {
            return false;
        }
    }

    return true;
}

static bool resolve_type_ref(ResolveContext *context, const FengTypeRef *type_ref, bool allow_void);
static bool resolve_expr(ResolveContext *context, const FengExpr *expr, bool allow_self);
static bool resolve_stmt(ResolveContext *context, const FengStmt *stmt, bool allow_self);
static bool resolve_block_contents(ResolveContext *context,
                                   const FengBlock *block,
                                   bool allow_self);

static bool resolve_named_type_ref(ResolveContext *context,
                                   const FengTypeRef *type_ref,
                                   bool allow_void) {
    const FengSlice *segments = type_ref->as.named.segments;
    size_t segment_count = type_ref->as.named.segment_count;
    FengSlice name;
    char *qualified_name;

    if (segment_count == 0U) {
        return true;
    }

    name = segments[segment_count - 1U];
    if (segment_count == 1U) {
        if (is_builtin_type_name(name)) {
            if (!allow_void && slice_equals_cstr(name, "void")) {
                return resolver_append_error(
                    context,
                    type_ref->token,
                    format_message("type 'void' is only valid as a function return type"));
            }
            return true;
        }

        if (find_named_type_decl(context, segments, segment_count) != NULL) {
            return true;
        }

        if (find_alias(context->aliases, context->alias_count, name) != NULL) {
            return resolver_append_error(
                context,
                type_ref->token,
                format_message("module alias '%.*s' cannot be used as a type by itself; use '%.*s.Name'",
                               (int)name.length,
                               name.data,
                               (int)name.length,
                               name.data));
        }
    } else if (find_named_type_decl(context, segments, segment_count) != NULL) {
        return true;
    }

    qualified_name = format_module_name(segments, segment_count);
    if (!resolver_append_error(context,
                               type_ref->token,
                               format_message("unknown type '%s'",
                                              qualified_name != NULL ? qualified_name : "<unknown>"))) {
        free(qualified_name);
        return false;
    }

    free(qualified_name);
    return true;
}

static bool resolve_type_ref(ResolveContext *context, const FengTypeRef *type_ref, bool allow_void) {
    if (type_ref == NULL) {
        return true;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            return resolve_named_type_ref(context, type_ref, allow_void);
        case FENG_TYPE_REF_POINTER:
            return resolve_type_ref(context, type_ref->as.inner, true);
        case FENG_TYPE_REF_ARRAY:
            return resolve_type_ref(context, type_ref->as.inner, false);
    }

    return true;
}

static bool resolve_alias_member_expr(ResolveContext *context, const FengExpr *expr) {
    const FengExpr *object;
    FengSlice alias_name;
    const AliasEntry *alias;
    char *module_name;

    if (expr->kind != FENG_EXPR_MEMBER) {
        return false;
    }

    object = expr->as.member.object;
    if (object == NULL || object->kind != FENG_EXPR_IDENTIFIER) {
        return false;
    }

    alias_name = object->as.identifier;
    alias = find_unshadowed_alias(context, alias_name);
    if (alias == NULL) {
        return false;
    }

    if (module_exports_public_name(alias->target_module, expr->as.member.member, NULL, NULL)) {
        return true;
    }

    module_name = format_module_name(alias->use_decl->segments, alias->use_decl->segment_count);
    if (!resolver_append_error(
            context,
            expr->token,
            format_message("module alias '%.*s' does not export public name '%.*s' from module '%s'",
                           (int)alias_name.length,
                           alias_name.data,
                           (int)expr->as.member.member.length,
                           expr->as.member.member.data,
                           module_name != NULL ? module_name : "<unknown>"))) {
        free(module_name);
        return false;
    }

    free(module_name);
    return true;
}

/* Initialise transient context state for resolving the body of a lambda or
 * function-like value. Returns the saved previous state via out parameters so
 * the caller can restore it. */
static void lambda_save_callable_context(ResolveContext *context,
                                         const FengCallableSignature **out_prev_sig,
                                         InferredExprType *out_prev_inferred_return,
                                         bool *out_prev_saw_return,
                                         bool *out_prev_escape,
                                         size_t *out_prev_exception_capture,
                                         size_t *out_prev_finally,
                                         size_t *out_prev_loop) {
    *out_prev_sig = context->current_callable_signature;
    *out_prev_inferred_return = context->current_callable_inferred_return_type;
    *out_prev_saw_return = context->current_callable_saw_return;
    *out_prev_escape = context->current_callable_has_escaping_exception;
    *out_prev_exception_capture = context->exception_capture_depth;
    *out_prev_finally = context->finally_depth;
    *out_prev_loop = context->loop_depth;
}

static void lambda_restore_callable_context(ResolveContext *context,
                                            const FengCallableSignature *prev_sig,
                                            InferredExprType prev_inferred_return,
                                            bool prev_saw_return,
                                            bool prev_escape,
                                            size_t prev_exception_capture,
                                            size_t prev_finally,
                                            size_t prev_loop) {
    context->current_callable_signature = prev_sig;
    context->current_callable_inferred_return_type = prev_inferred_return;
    context->current_callable_saw_return = prev_saw_return;
    context->current_callable_has_escaping_exception = prev_escape;
    context->exception_capture_depth = prev_exception_capture;
    context->finally_depth = prev_finally;
    context->loop_depth = prev_loop;
}

static bool resolve_lambda_expr(ResolveContext *context, const FengExpr *expr) {
    size_t param_index;
    const FengCallableSignature *prev_sig;
    InferredExprType prev_inferred_return;
    bool prev_saw_return;
    bool prev_escape;
    size_t prev_exception_capture;
    size_t prev_finally;
    size_t prev_loop;
    bool prev_self_capturable;
    bool effective_allow_self;
    LambdaCaptureFrame frame;
    FengCallableSignature synthetic_sig;
    FengExpr *mutable_expr = (FengExpr *)expr;
    bool ok;

    for (param_index = 0U; param_index < expr->as.lambda.param_count; ++param_index) {
        if (!resolve_type_ref(context, expr->as.lambda.params[param_index].type, false)) {
            return false;
        }
    }

    /* Snapshot scope_count before pushing the lambda's own scope: any local
     * binding found at scope <= floor is captured from outside. */
    memset(&frame, 0, sizeof(frame));
    frame.lambda = mutable_expr;
    frame.outer_scope_floor = context->scope_count;

    if (!append_raw((void **)&context->lambda_frames,
                    &context->lambda_frame_count,
                    &context->lambda_frame_capacity,
                    sizeof(frame),
                    &frame)) {
        return false;
    }

    if (!resolver_push_scope(context)) {
        --context->lambda_frame_count;
        return false;
    }

    lambda_save_callable_context(context,
                                 &prev_sig,
                                 &prev_inferred_return,
                                 &prev_saw_return,
                                 &prev_escape,
                                 &prev_exception_capture,
                                 &prev_finally,
                                 &prev_loop);
    prev_self_capturable = context->self_capturable;

    /* Inside the lambda body, `self` is available iff the enclosing context
     * could provide it (i.e. self_capturable was true). Nested lambdas keep
     * the same capability so they can also capture self. */
    effective_allow_self = context->self_capturable;

    context->current_callable_signature = NULL;
    context->current_callable_inferred_return_type = inferred_expr_type_unknown();
    context->current_callable_saw_return = false;
    context->current_callable_has_escaping_exception = false;
    context->exception_capture_depth = 0U;
    context->finally_depth = 0U;
    context->loop_depth = 0U;
    /* self_capturable stays the same: if the lambda body could see self, so
     * can a lambda nested inside it. */
    context->self_capturable = effective_allow_self;

    if (expr->as.lambda.is_block_body) {
        memset(&synthetic_sig, 0, sizeof(synthetic_sig));
        synthetic_sig.token = expr->token;
        synthetic_sig.params = expr->as.lambda.params;
        synthetic_sig.param_count = expr->as.lambda.param_count;
        synthetic_sig.return_type = NULL; /* inferred from the block's return statements */
        synthetic_sig.body = expr->as.lambda.body_block;
        context->current_callable_signature = &synthetic_sig;
    }

    ok = true;
    for (param_index = 0U; param_index < expr->as.lambda.param_count && ok; ++param_index) {
        ok = resolver_add_local_typed_name(
            context,
            expr->as.lambda.params[param_index].name,
            inferred_expr_type_from_type_ref(expr->as.lambda.params[param_index].type),
            expr->as.lambda.params[param_index].mutability);
    }
    if (ok) {
        if (expr->as.lambda.is_block_body) {
            ok = resolve_block_contents(context, expr->as.lambda.body_block, effective_allow_self);
        } else {
            ok = resolve_expr(context, expr->as.lambda.body, effective_allow_self);
        }
    }

    lambda_restore_callable_context(context,
                                    prev_sig,
                                    prev_inferred_return,
                                    prev_saw_return,
                                    prev_escape,
                                    prev_exception_capture,
                                    prev_finally,
                                    prev_loop);
    context->self_capturable = prev_self_capturable;

    resolver_pop_scope(context);

    /* Pop the lambda capture frame and publish its captures onto the AST so
     * later phases (and code generation) can reason about closure state. */
    {
        LambdaCaptureFrame *built = &context->lambda_frames[context->lambda_frame_count - 1U];

        if (ok) {
            free(mutable_expr->as.lambda.captures);
            mutable_expr->as.lambda.captures = built->captures;
            mutable_expr->as.lambda.capture_count = built->capture_count;
            mutable_expr->as.lambda.captures_self = built->captures_self;
        } else {
            free(built->captures);
        }
        --context->lambda_frame_count;
    }

    return ok;
}

static bool resolve_expr(ResolveContext *context, const FengExpr *expr, bool allow_self) {
    size_t index;

    if (expr == NULL) {
        return true;
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            {
                size_t local_scope = 0U;
                const LocalNameEntry *local = resolver_find_local_name_entry_with_scope(
                    context, expr->as.identifier, &local_scope);

                if (local != NULL) {
                    if (context->lambda_frame_count > 0U &&
                        !resolver_record_local_capture(context,
                                                       local->name,
                                                       local->mutability,
                                                       local_scope)) {
                        return false;
                    }
                    return true;
                }
                if (find_visible_value(context->visible_values,
                                       context->visible_value_count,
                                       expr->as.identifier) != NULL ||
                    find_visible_type(context->visible_types,
                                      context->visible_type_count,
                                      expr->as.identifier) != NULL) {
                    return true;
                }
            }

            if (find_alias(context->aliases, context->alias_count, expr->as.identifier) != NULL) {
                return resolver_append_error(
                    context,
                    expr->token,
                    format_message("module alias '%.*s' must be accessed as '%.*s.name'",
                                   (int)expr->as.identifier.length,
                                   expr->as.identifier.data,
                                   (int)expr->as.identifier.length,
                                   expr->as.identifier.data));
            }

            return resolver_append_error(context,
                                         expr->token,
                                         format_message("undefined identifier '%.*s'",
                                                        (int)expr->as.identifier.length,
                                                        expr->as.identifier.data));

        case FENG_EXPR_SELF:
            if (allow_self) {
                if (context->lambda_frame_count > 0U) {
                    resolver_record_self_capture(context);
                }
                return true;
            }

            return resolver_append_error(context,
                                         expr->token,
                                         format_message("'self' is only available inside type methods and constructors"));

        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
            return true;

        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (!resolve_expr(context, expr->as.array_literal.items[index], allow_self)) {
                    return false;
                }
            }
            return validate_array_literal_expr(context, expr);

        case FENG_EXPR_OBJECT_LITERAL:
            {
                ResolvedTypeTarget target;

                if (!resolve_expr(context, expr->as.object_literal.target, allow_self)) {
                    return false;
                }
                if (!validate_object_literal_expr(context, expr)) {
                    return false;
                }

                target = resolve_type_target_expr(context, expr->as.object_literal.target, true);
                for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                    const FengObjectFieldInit *field = &expr->as.object_literal.fields[index];
                    const FengTypeMember *field_member =
                        target.type_decl != NULL ? find_type_field_member(target.type_decl, field->name) : NULL;

                    if (!resolve_expr(context, field->value, allow_self)) {
                        return false;
                    }
                    if (field_member != NULL &&
                        !validate_expr_against_expected_type(context,
                                                            field->value,
                                                            field_member->as.field.type)) {
                        return false;
                    }
                }
                return true;
            }

        case FENG_EXPR_CALL:
            if (!resolve_expr(context, expr->as.call.callee, allow_self)) {
                return false;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                if (!resolve_expr(context, expr->as.call.args[index], allow_self)) {
                    return false;
                }
            }
            return validate_constructor_call_expr(context, expr) &&
                   validate_function_call_expr(context, expr);

        case FENG_EXPR_MEMBER:
            if (resolve_alias_member_expr(context, expr)) {
                return true;
            }
            if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_SELF) {
                return resolve_self_member_expr(context, expr, allow_self);
            }
            return resolve_expr(context, expr->as.member.object, allow_self) &&
                   validate_instance_member_expr(context, expr);

        case FENG_EXPR_INDEX:
            return resolve_expr(context, expr->as.index.object, allow_self) &&
                   resolve_expr(context, expr->as.index.index, allow_self) &&
                   validate_index_expr(context, expr);

        case FENG_EXPR_UNARY:
            return resolve_expr(context, expr->as.unary.operand, allow_self) &&
                   validate_unary_expr(context, expr);

        case FENG_EXPR_BINARY:
            if (!resolve_expr(context, expr->as.binary.left, allow_self) ||
                !resolve_expr(context, expr->as.binary.right, allow_self) ||
                !validate_binary_expr(context, expr)) {
                return false;
            }
            record_spec_equality_if_applicable(context, expr);
            return true;

        case FENG_EXPR_LAMBDA:
            return resolve_lambda_expr(context, expr);

        case FENG_EXPR_CAST:
            return resolve_type_ref(context, expr->as.cast.type, false) &&
                   resolve_expr(context, expr->as.cast.value, allow_self) &&
                   validate_cast_expr(context, expr);

        case FENG_EXPR_IF:
            if (!resolve_expr(context, expr->as.if_expr.condition, allow_self) ||
                !resolve_block(context, expr->as.if_expr.then_block, allow_self) ||
                !resolve_block(context, expr->as.if_expr.else_block, allow_self)) {
                return false;
            }
            return validate_if_expr(context, expr);

        case FENG_EXPR_MATCH:
            return resolve_and_validate_match_common(context,
                                                     expr->as.match_expr.target,
                                                     expr->as.match_expr.branches,
                                                     expr->as.match_expr.branch_count,
                                                     expr->as.match_expr.else_block,
                                                     expr->token,
                                                     true,
                                                     allow_self);
    }

    return true;
}

static bool resolve_binding(ResolveContext *context,
                            const FengBinding *binding,
                            bool allow_self,
                            bool add_to_scope) {
    InferredExprType binding_type;

    if (!resolve_type_ref(context, binding->type, false)) {
        return false;
    }
    if (!resolve_expr(context, binding->initializer, allow_self)) {
        return false;
    }
    if (binding->type != NULL) {
        if (!validate_expr_against_expected_type(context, binding->initializer, binding->type)) {
            return false;
        }
        if (binding->initializer == NULL) {
            /* Phase S2-a: `let s: S;` (or `var s: S;`) without an initializer
             * is a default-witness slot when the declared type is a spec. */
            record_spec_default_binding_if_applicable(
                context,
                binding,
                FENG_SPEC_DEFAULT_BINDING_POSITION_LOCAL_BINDING,
                binding->type);
        }
    } else {
        if (!validate_untyped_callable_value_expr(context, binding->initializer)) {
            return false;
        }
        if (!validate_untyped_array_literal_expr(context, binding->initializer)) {
            return false;
        }
    }
    binding_type = binding->type != NULL ? inferred_expr_type_from_type_ref(binding->type)
                                         : infer_expr_type(context, binding->initializer);
    if (!record_type_fact_for_site(context, binding, binding_type)) {
        return false;
    }
    if (add_to_scope) {
        return resolver_add_local_typed_name_with_source(
            context, binding->name, binding_type, binding->mutability, binding->initializer);
    }
    return true;
}

static bool resolve_block(ResolveContext *context, const FengBlock *block, bool allow_self) {
    bool ok;

    if (!resolver_push_scope(context)) {
        return false;
    }

    ok = resolve_block_contents(context, block, allow_self);
    resolver_pop_scope(context);
    return ok;
}

static bool resolve_block_with_exception_capture(ResolveContext *context,
                                                 const FengBlock *block,
                                                 bool allow_self,
                                                 bool catches_exceptions) {
    size_t previous_capture_depth;
    bool ok;

    if (context == NULL) {
        return true;
    }

    previous_capture_depth = context->exception_capture_depth;
    if (catches_exceptions) {
        context->exception_capture_depth += 1U;
    }

    ok = resolve_block(context, block, allow_self);
    context->exception_capture_depth = previous_capture_depth;
    return ok;
}

static bool resolve_block_with_finally_context(ResolveContext *context,
                                               const FengBlock *block,
                                               bool allow_self) {
    size_t previous_finally_depth;
    bool ok;

    if (context == NULL) {
        return true;
    }

    previous_finally_depth = context->finally_depth;
    context->finally_depth += 1U;
    ok = resolve_block(context, block, allow_self);
    context->finally_depth = previous_finally_depth;
    return ok;
}

static bool resolve_block_contents(ResolveContext *context,
                                   const FengBlock *block,
                                   bool allow_self) {
    size_t stmt_index;

    if (block == NULL) {
        return true;
    }

    for (stmt_index = 0U; stmt_index < block->statement_count; ++stmt_index) {
        if (!resolve_stmt(context, block->statements[stmt_index], allow_self)) {
            return false;
        }
    }

    return true;
}

static bool resolve_stmt(ResolveContext *context, const FengStmt *stmt, bool allow_self) {
    size_t clause_index;

    if (stmt == NULL) {
        return true;
    }

    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return resolve_block(context, stmt->as.block, allow_self);

        case FENG_STMT_BINDING:
            return resolve_binding(context, &stmt->as.binding, allow_self, true);

        case FENG_STMT_ASSIGN:
            if (!resolve_expr(context, stmt->as.assign.target, allow_self)) {
                return false;
            }
            /* Phase S2-b: if the LHS is a member-expression against an
             * object-form spec, the recorded site is FIELD_READ by default;
             * upgrade it to FIELD_WRITE here. No-op when the target is not
             * a recorded spec member access (e.g. plain identifier, index
             * expression, or member of a non-spec owner). */
            if (stmt->as.assign.target != NULL &&
                stmt->as.assign.target->kind == FENG_EXPR_MEMBER) {
                feng_semantic_upgrade_spec_member_access_to_write(
                    context->analysis, stmt->as.assign.target);
            }
            if (!validate_self_let_assignment(context, stmt)) {
                return false;
            }
            if (!validate_assignment_target_writable(context, stmt->as.assign.target)) {
                return false;
            }
            if (!resolve_expr(context, stmt->as.assign.value, allow_self)) {
                return false;
            }
            if (stmt->as.assign.op != FENG_TOKEN_ASSIGN) {
                return validate_compound_assignment(context, stmt);
            }
            return validate_expr_against_expected_inferred_type(
                context,
                stmt->as.assign.value,
                infer_expr_type(context, stmt->as.assign.target));

        case FENG_STMT_EXPR:
            return resolve_expr(context, stmt->as.expr, allow_self);

        case FENG_STMT_IF:
            for (clause_index = 0U; clause_index < stmt->as.if_stmt.clause_count; ++clause_index) {
                if (!resolve_expr(context, stmt->as.if_stmt.clauses[clause_index].condition, allow_self) ||
                    !validate_stmt_condition_expr(context,
                                                  stmt->token,
                                                  stmt->as.if_stmt.clauses[clause_index].condition,
                                                  "if statement") ||
                    !resolve_block(context, stmt->as.if_stmt.clauses[clause_index].block, allow_self)) {
                    return false;
                }
            }
            return resolve_block(context, stmt->as.if_stmt.else_block, allow_self);

        case FENG_STMT_MATCH:
            return resolve_and_validate_match_common(context,
                                                     stmt->as.match_stmt.target,
                                                     stmt->as.match_stmt.branches,
                                                     stmt->as.match_stmt.branch_count,
                                                     stmt->as.match_stmt.else_block,
                                                     stmt->token,
                                                     false,
                                                     allow_self);

        case FENG_STMT_WHILE: {
            bool ok;

            if (!resolve_expr(context, stmt->as.while_stmt.condition, allow_self) ||
                !validate_stmt_condition_expr(
                    context, stmt->token, stmt->as.while_stmt.condition, "while statement")) {
                return false;
            }
            context->loop_depth += 1U;
            ok = resolve_block(context, stmt->as.while_stmt.body, allow_self);
            context->loop_depth -= 1U;
            return ok;
        }

        case FENG_STMT_FOR: {
            bool ok;

            if (!resolver_push_scope(context)) {
                return false;
            }

            if (stmt->as.for_stmt.is_for_in) {
                InferredExprType iter_type;
                const FengTypeRef *element_type_ref = NULL;

                ok = resolve_expr(context, stmt->as.for_stmt.iter_expr, allow_self);
                if (ok) {
                    iter_type = infer_expr_type(context, stmt->as.for_stmt.iter_expr);
                    if (iter_type.kind == FENG_INFERRED_EXPR_TYPE_TYPE_REF &&
                        iter_type.type_ref != NULL &&
                        iter_type.type_ref->kind == FENG_TYPE_REF_ARRAY) {
                        element_type_ref = iter_type.type_ref->as.inner;
                    } else {
                        char *type_name = format_inferred_expr_type_name(iter_type);

                        ok = resolver_append_error(
                            context,
                            stmt->as.for_stmt.iter_expr->token,
                            format_message(
                                "for/in sequence must be an array 'T[]', got '%s'",
                                type_name != NULL ? type_name : "<unknown>"));
                        free(type_name);
                    }
                }
                if (ok && element_type_ref != NULL) {
                    InferredExprType element_type =
                        inferred_expr_type_from_type_ref(element_type_ref);

                    ok = resolver_add_local_typed_name(
                        context,
                        stmt->as.for_stmt.iter_binding.name,
                        element_type,
                        stmt->as.for_stmt.iter_binding.mutability);
                }
            } else {
                ok = resolve_stmt(context, stmt->as.for_stmt.init, allow_self) &&
                     resolve_expr(context, stmt->as.for_stmt.condition, allow_self) &&
                     validate_stmt_condition_expr(
                         context, stmt->token, stmt->as.for_stmt.condition, "for statement") &&
                     resolve_stmt(context, stmt->as.for_stmt.update, allow_self);
            }
            if (ok) {
                context->loop_depth += 1U;
                ok = resolve_block(context, stmt->as.for_stmt.body, allow_self);
                context->loop_depth -= 1U;
            }

            resolver_pop_scope(context);
            return ok;
        }

        case FENG_STMT_TRY:
            return resolve_block_with_exception_capture(context,
                                                        stmt->as.try_stmt.try_block,
                                                        allow_self,
                                                        stmt->as.try_stmt.catch_block != NULL) &&
                   resolve_block(context, stmt->as.try_stmt.catch_block, allow_self) &&
                   resolve_block_with_finally_context(context,
                                                     stmt->as.try_stmt.finally_block,
                                                     allow_self);

        case FENG_STMT_RETURN:
            if (!resolve_expr(context, stmt->as.return_value, allow_self)) {
                return false;
            }
            if (!validate_finally_forbidden_control_stmt(context, stmt, "return")) {
                return false;
            }
            return validate_return_stmt(context, stmt);

        case FENG_STMT_THROW:
            if (!resolve_expr(context, stmt->as.throw_value, allow_self)) {
                return false;
            }
            if (!validate_throw_stmt(context, stmt)) {
                return false;
            }
            note_current_callable_exception_escape(context);
            return true;

        case FENG_STMT_BREAK:
            return validate_loop_control_stmt(context, stmt, "break");

        case FENG_STMT_CONTINUE:
            return validate_loop_control_stmt(context, stmt, "continue");
    }

    return true;
}

static bool resolve_callable(ResolveContext *context,
                             const FengCallableSignature *callable,
                             bool allow_self) {
    size_t param_index;
    bool is_constructor = context->current_callable_member != NULL &&
                          context->current_callable_member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR;
    const FengCallableSignature *previous_callable_signature =
        context->current_callable_signature;
    InferredExprType previous_callable_inferred_return_type =
        context->current_callable_inferred_return_type;
    bool previous_callable_saw_return = context->current_callable_saw_return;
    bool previous_callable_has_escaping_exception =
        context->current_callable_has_escaping_exception;
    size_t previous_exception_capture_depth = context->exception_capture_depth;
    size_t previous_finally_depth = context->finally_depth;
    size_t previous_loop_depth = context->loop_depth;
    bool previous_self_capturable = context->self_capturable;
    bool ok;

    context->current_callable_signature = callable;
    context->current_callable_inferred_return_type = callable_effective_return_type(context, callable);
    context->current_callable_saw_return = false;
    context->current_callable_has_escaping_exception = false;
    context->exception_capture_depth = 0U;
    context->finally_depth = 0U;
    context->loop_depth = 0U;
    /* Inside a member method or constructor body, lambdas may capture self. */
    if (allow_self && context->current_type_decl != NULL) {
        context->self_capturable = true;
    }

    if (!resolve_type_ref(context, callable->return_type, true)) {
        context->current_callable_inferred_return_type = previous_callable_inferred_return_type;
        context->current_callable_saw_return = previous_callable_saw_return;
        context->current_callable_has_escaping_exception = previous_callable_has_escaping_exception;
        context->exception_capture_depth = previous_exception_capture_depth;
        context->finally_depth = previous_finally_depth;
        context->loop_depth = previous_loop_depth;
        context->current_callable_signature = previous_callable_signature;
        context->self_capturable = previous_self_capturable;
        return false;
    }

    for (param_index = 0U; param_index < callable->param_count; ++param_index) {
        if (!resolve_type_ref(context, callable->params[param_index].type, false)) {
            context->current_callable_inferred_return_type = previous_callable_inferred_return_type;
            context->current_callable_saw_return = previous_callable_saw_return;
            context->current_callable_has_escaping_exception = previous_callable_has_escaping_exception;
            context->exception_capture_depth = previous_exception_capture_depth;
            context->finally_depth = previous_finally_depth;
            context->loop_depth = previous_loop_depth;
            context->current_callable_signature = previous_callable_signature;
            context->self_capturable = previous_self_capturable;
            return false;
        }
    }

    if (callable->body == NULL) {
        context->current_callable_inferred_return_type = previous_callable_inferred_return_type;
        context->current_callable_saw_return = previous_callable_saw_return;
        context->current_callable_has_escaping_exception = previous_callable_has_escaping_exception;
        context->exception_capture_depth = previous_exception_capture_depth;
        context->finally_depth = previous_finally_depth;
        context->loop_depth = previous_loop_depth;
        context->current_callable_signature = previous_callable_signature;
        context->self_capturable = previous_self_capturable;
        return true;
    }

    if (is_constructor) {
        resolver_clear_current_constructor_bindings(context);
    }

    if (!resolver_push_scope(context)) {
        if (is_constructor) {
            resolver_clear_current_constructor_bindings(context);
        }
        context->current_callable_inferred_return_type = previous_callable_inferred_return_type;
        context->current_callable_saw_return = previous_callable_saw_return;
        context->current_callable_has_escaping_exception = previous_callable_has_escaping_exception;
        context->exception_capture_depth = previous_exception_capture_depth;
        context->finally_depth = previous_finally_depth;
        context->loop_depth = previous_loop_depth;
        context->current_callable_signature = previous_callable_signature;
        context->self_capturable = previous_self_capturable;
        return false;
    }

    ok = true;
    for (param_index = 0U; param_index < callable->param_count && ok; ++param_index) {
        ok = resolver_add_local_typed_name(
            context,
            callable->params[param_index].name,
            inferred_expr_type_from_type_ref(callable->params[param_index].type),
            callable->params[param_index].mutability);
    }
    if (ok) {
        ok = resolve_block_contents(context, callable->body, allow_self);
    }

    resolver_pop_scope(context);
    if (is_constructor) {
        resolver_clear_current_constructor_bindings(context);
    }
    if (ok && callable->return_type == NULL && !is_constructor) {
        InferredExprType inferred_return_type =
            context->current_callable_saw_return ? context->current_callable_inferred_return_type
                                                : inferred_expr_type_builtin("void");

        if (inferred_expr_type_is_known(inferred_return_type) &&
            !cache_callable_return_type(context, callable, inferred_return_type)) {
            ok = false;
        }
    }
    if (ok && !cache_callable_exception_escape(
                  context, callable, context->current_callable_has_escaping_exception)) {
        ok = false;
    }
    if (ok) {
        InferredExprType resolved_return_type = callable->return_type != NULL
                                                    ? inferred_expr_type_from_type_ref(callable->return_type)
                                                    : inferred_expr_type_builtin("void");

        if (!is_constructor && callable->return_type == NULL && context->current_callable_saw_return) {
            resolved_return_type = context->current_callable_inferred_return_type;
        }

        if (!record_type_fact_for_site(context, callable, resolved_return_type)) {
            ok = false;
        }
    }
    context->current_callable_inferred_return_type = previous_callable_inferred_return_type;
    context->current_callable_saw_return = previous_callable_saw_return;
    context->current_callable_has_escaping_exception = previous_callable_has_escaping_exception;
    context->exception_capture_depth = previous_exception_capture_depth;
    context->finally_depth = previous_finally_depth;
    context->loop_depth = previous_loop_depth;
    context->current_callable_signature = previous_callable_signature;
    context->self_capturable = previous_self_capturable;
    return ok;
}

/* ===== Contract validation: spec parent specs / type-declared specs / fit ===== */

static bool slice_eq(FengSlice a, FengSlice b) {
    return a.length == b.length &&
           (a.length == 0U || memcmp(a.data, b.data, a.length) == 0);
}

static bool spec_parent_reaches_recursive(const ResolveContext *ctx,
                                          const FengDecl *current,
                                          const FengDecl *target,
                                          const FengDecl ***visited,
                                          size_t *visited_count,
                                          size_t *visited_capacity) {
    size_t i;

    if (current == NULL || current->kind != FENG_DECL_SPEC) {
        return false;
    }

    for (i = 0U; i < current->as.spec_decl.parent_spec_count; ++i) {
        const FengDecl *next = resolve_type_ref_decl(ctx, current->as.spec_decl.parent_specs[i]);
        size_t j;
        bool seen;

        if (next == NULL || next->kind != FENG_DECL_SPEC) {
            continue;
        }
        if (next == target) {
            return true;
        }
        seen = false;
        for (j = 0U; j < *visited_count; ++j) {
            if ((*visited)[j] == next) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (!append_raw((void **)visited, visited_count, visited_capacity,
                        sizeof(**visited), &next)) {
            return false;
        }
        if (spec_parent_reaches_recursive(ctx, next, target, visited, visited_count,
                                          visited_capacity)) {
            return true;
        }
    }
    return false;
}

static bool spec_collect_closure(const ResolveContext *ctx,
                                 const FengDecl *spec_decl,
                                 const FengDecl ***out_set,
                                 size_t *out_count,
                                 size_t *out_capacity) {
    size_t i;
    size_t k;

    if (spec_decl == NULL || spec_decl->kind != FENG_DECL_SPEC) {
        return true;
    }
    for (k = 0U; k < *out_count; ++k) {
        if ((*out_set)[k] == spec_decl) {
            return true;
        }
    }
    if (!append_raw((void **)out_set, out_count, out_capacity,
                    sizeof(**out_set), &spec_decl)) {
        return false;
    }
    for (i = 0U; i < spec_decl->as.spec_decl.parent_spec_count; ++i) {
        const FengDecl *next = resolve_type_ref_decl(ctx, spec_decl->as.spec_decl.parent_specs[i]);

        if (next == NULL || next->kind != FENG_DECL_SPEC) {
            continue;
        }
        if (!spec_collect_closure(ctx, next, out_set, out_count, out_capacity)) {
            return false;
        }
    }
    return true;
}

static bool validate_spec_parent_spec_list(ResolveContext *context, const FengDecl *spec_decl) {
    size_t i;
    size_t j;

    for (i = 0U; i < spec_decl->as.spec_decl.parent_spec_count; ++i) {
        const FengTypeRef *r = spec_decl->as.spec_decl.parent_specs[i];
        const FengDecl *resolved = resolve_type_ref_decl(context, r);

        if (resolved == NULL || resolved->kind != FENG_DECL_SPEC) {
            char *target_name = format_type_ref_name(r);
            bool ok = resolver_append_error(
                context, r->token,
                format_message(
                    "spec '%.*s' parent spec list must contain only spec types but found '%s'",
                    (int)spec_decl->as.spec_decl.name.length,
                    spec_decl->as.spec_decl.name.data,
                    target_name != NULL ? target_name : "<unknown>"));

            free(target_name);
            return ok;
        }
        for (j = 0U; j < i; ++j) {
            const FengDecl *prev = resolve_type_ref_decl(context, spec_decl->as.spec_decl.parent_specs[j]);

            if (prev == resolved) {
                return resolver_append_error(
                    context, r->token,
                    format_message(
                        "spec '%.*s' lists '%.*s' more than once in its parent spec list",
                        (int)spec_decl->as.spec_decl.name.length,
                        spec_decl->as.spec_decl.name.data,
                        (int)resolved->as.spec_decl.name.length,
                        resolved->as.spec_decl.name.data));
            }
        }
    }

    {
        const FengDecl **visited = NULL;
        size_t visited_count = 0U;
        size_t visited_capacity = 0U;
        bool cycle = spec_parent_reaches_recursive(context, spec_decl, spec_decl,
                                                   &visited, &visited_count, &visited_capacity);

        free(visited);
        if (cycle) {
            return resolver_append_error(
                context, spec_decl->token,
                format_message(
                    "spec '%.*s' forms a cycle through its parent spec list",
                    (int)spec_decl->as.spec_decl.name.length,
                    spec_decl->as.spec_decl.name.data));
        }
    }
    return true;
}

static const FengTypeMember *type_find_field(const FengDecl *type_decl, FengSlice name) {
    size_t i;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return NULL;
    }
    for (i = 0U; i < type_decl->as.type_decl.member_count; ++i) {
        const FengTypeMember *m = type_decl->as.type_decl.members[i];

        if (m->kind == FENG_TYPE_MEMBER_FIELD && slice_eq(m->as.field.name, name)) {
            return m;
        }
    }
    return NULL;
}

static bool callable_signatures_match_for_satisfaction(const ResolveContext *ctx,
                                                       const FengCallableSignature *spec_sig,
                                                       const FengCallableSignature *type_sig) {
    size_t i;

    if (spec_sig->param_count != type_sig->param_count) {
        return false;
    }
    for (i = 0U; i < spec_sig->param_count; ++i) {
        if (!type_refs_semantically_equal(ctx, spec_sig->params[i].type, type_sig->params[i].type)) {
            return false;
        }
    }
    if (!type_refs_semantically_equal(ctx, spec_sig->return_type, type_sig->return_type)) {
        return false;
    }
    return true;
}

static const FengTypeMember *type_find_matching_method(
    const ResolveContext *ctx,
    const FengDecl *type_decl,
    const FengCallableSignature *spec_sig,
    const FengTypeMember *const *extra_methods,
    size_t extra_count) {
    size_t i;

    if (type_decl != NULL && type_decl->kind == FENG_DECL_TYPE) {
        for (i = 0U; i < type_decl->as.type_decl.member_count; ++i) {
            const FengTypeMember *m = type_decl->as.type_decl.members[i];

            if (m->kind == FENG_TYPE_MEMBER_METHOD &&
                slice_eq(m->as.callable.name, spec_sig->name) &&
                callable_signatures_match_for_satisfaction(ctx, spec_sig, &m->as.callable)) {
                return m;
            }
        }
    }
    for (i = 0U; i < extra_count; ++i) {
        const FengTypeMember *m = extra_methods[i];

        if (m->kind == FENG_TYPE_MEMBER_METHOD &&
            slice_eq(m->as.callable.name, spec_sig->name) &&
            callable_signatures_match_for_satisfaction(ctx, spec_sig, &m->as.callable)) {
            return m;
        }
    }
    return NULL;
}

static const FengTypeMember *type_find_method_by_name(
    const FengDecl *type_decl,
    FengSlice name,
    const FengTypeMember *const *extra_methods,
    size_t extra_count) {
    size_t i;

    if (type_decl != NULL && type_decl->kind == FENG_DECL_TYPE) {
        for (i = 0U; i < type_decl->as.type_decl.member_count; ++i) {
            const FengTypeMember *m = type_decl->as.type_decl.members[i];

            if (m->kind == FENG_TYPE_MEMBER_METHOD && slice_eq(m->as.callable.name, name)) {
                return m;
            }
        }
    }
    for (i = 0U; i < extra_count; ++i) {
        const FengTypeMember *m = extra_methods[i];

        if (m->kind == FENG_TYPE_MEMBER_METHOD && slice_eq(m->as.callable.name, name)) {
            return m;
        }
    }
    return NULL;
}

static bool verify_type_satisfies_spec(ResolveContext *ctx,
                                       const FengDecl *type_decl,
                                       const FengDecl *spec_decl,
                                       FengToken err_token,
                                       const FengTypeMember *const *extra_methods,
                                       size_t extra_count) {
    size_t i;

    if (spec_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
        return true;
    }

    for (i = 0U; i < spec_decl->as.spec_decl.as.object.member_count; ++i) {
        const FengTypeMember *spec_m = spec_decl->as.spec_decl.as.object.members[i];

        if (spec_m->kind == FENG_TYPE_MEMBER_FIELD) {
            const FengTypeMember *t = type_find_field(type_decl, spec_m->as.field.name);

            if (t == NULL) {
                return resolver_append_error(
                    ctx, err_token,
                    format_message(
                        "type '%.*s' is missing field '%.*s' required by spec '%.*s'",
                        (int)decl_typeish_name(type_decl).length,
                        decl_typeish_name(type_decl).data,
                        (int)spec_m->as.field.name.length, spec_m->as.field.name.data,
                        (int)spec_decl->as.spec_decl.name.length,
                        spec_decl->as.spec_decl.name.data));
            }
            if (t->as.field.mutability != spec_m->as.field.mutability) {
                return resolver_append_error(
                    ctx, err_token,
                    format_message(
                        "type '%.*s' field '%.*s' mutability does not match spec '%.*s' (expected '%s')",
                        (int)decl_typeish_name(type_decl).length,
                        decl_typeish_name(type_decl).data,
                        (int)spec_m->as.field.name.length, spec_m->as.field.name.data,
                        (int)spec_decl->as.spec_decl.name.length,
                        spec_decl->as.spec_decl.name.data,
                        spec_m->as.field.mutability == FENG_MUTABILITY_LET ? "let" : "var"));
            }
            if (!type_refs_semantically_equal(ctx, t->as.field.type, spec_m->as.field.type)) {
                char *expected = format_type_ref_name(spec_m->as.field.type);
                char *actual = format_type_ref_name(t->as.field.type);
                bool ok = resolver_append_error(
                    ctx, err_token,
                    format_message(
                        "type '%.*s' field '%.*s' type '%s' does not match spec '%.*s' field type '%s'",
                        (int)decl_typeish_name(type_decl).length,
                        decl_typeish_name(type_decl).data,
                        (int)spec_m->as.field.name.length, spec_m->as.field.name.data,
                        actual != NULL ? actual : "<unknown>",
                        (int)spec_decl->as.spec_decl.name.length,
                        spec_decl->as.spec_decl.name.data,
                        expected != NULL ? expected : "<unknown>"));

                free(expected);
                free(actual);
                return ok;
            }
        } else if (spec_m->kind == FENG_TYPE_MEMBER_METHOD) {
            const FengTypeMember *match = type_find_matching_method(
                ctx, type_decl, &spec_m->as.callable, extra_methods, extra_count);

            if (match == NULL) {
                const FengTypeMember *named = type_find_method_by_name(
                    type_decl, spec_m->as.callable.name, extra_methods, extra_count);

                if (named != NULL) {
                    return resolver_append_error(
                        ctx, err_token,
                        format_message(
                            "type '%.*s' method '%.*s' signature does not match spec '%.*s'",
                            (int)decl_typeish_name(type_decl).length,
                            decl_typeish_name(type_decl).data,
                            (int)spec_m->as.callable.name.length,
                            spec_m->as.callable.name.data,
                            (int)spec_decl->as.spec_decl.name.length,
                            spec_decl->as.spec_decl.name.data));
                }
                return resolver_append_error(
                    ctx, err_token,
                    format_message(
                        "type '%.*s' is missing method '%.*s' required by spec '%.*s'",
                        (int)decl_typeish_name(type_decl).length,
                        decl_typeish_name(type_decl).data,
                        (int)spec_m->as.callable.name.length,
                        spec_m->as.callable.name.data,
                        (int)spec_decl->as.spec_decl.name.length,
                        spec_decl->as.spec_decl.name.data));
            }
        }
    }
    return true;
}

/* Phase S3 — SpecWitness compute (§6.5 / §8.1 / §8.2).
 *
 * Materialises one (T, S) witness on first demand. The walk follows S's
 * member closure (deduplicated by name to mirror find_spec_object_member's
 * "first occurrence wins" semantics) and resolves each member against T's
 * visible face: T's own members + every fit visible from the resolve
 * context's module (per fit_decl_is_visible_from). Method candidates are
 * filtered by exact signature match per §6.5; ambiguity between distinct
 * visible-face implementations of the same (name, params, return) raises
 * §8.1 conflict at the triggering coercion site.
 *
 * Missing-implementation slots (no candidate at all) record a NULL
 * impl_member without emitting an error: the missing-member diagnostic is
 * already produced by verify_type_satisfies_spec / fit validation; the
 * NULL slot exists only so codegen can detect the gap. */

typedef struct WitnessFitCandidate {
    const FengTypeMember *method;
    const FengDecl *fit_decl;
    const FengSemanticModule *fit_module;
} WitnessFitCandidate;

typedef struct WitnessFitCollectCtx {
    const ResolveContext *ctx;
    const FengCallableSignature *spec_sig;
    WitnessFitCandidate *items;
    size_t count;
    size_t capacity;
    bool oom;
} WitnessFitCollectCtx;

static bool witness_fit_collect_visitor(const FengTypeMember *member,
                                        const FengSemanticModule *fit_module,
                                        const FengDecl *fit_decl,
                                        void *userdata) {
    WitnessFitCollectCtx *st = (WitnessFitCollectCtx *)userdata;

    if (!callable_signatures_match_for_satisfaction(st->ctx, st->spec_sig,
                                                    &member->as.callable)) {
        return true;
    }
    if (st->count == st->capacity) {
        size_t new_cap = st->capacity == 0U ? 4U : st->capacity * 2U;
        WitnessFitCandidate *grown = realloc(st->items, new_cap * sizeof(*grown));

        if (grown == NULL) {
            st->oom = true;
            return false;
        }
        st->items = grown;
        st->capacity = new_cap;
    }
    st->items[st->count].method = member;
    st->items[st->count].fit_decl = fit_decl;
    st->items[st->count].fit_module = fit_module;
    ++st->count;
    return true;
}

static void compute_spec_witness_if_absent(ResolveContext *context,
                                           const FengDecl *type_decl,
                                           const FengDecl *spec_decl,
                                           FengToken err_token) {
    const FengDecl **closure = NULL;
    size_t closure_count = 0U;
    size_t closure_capacity = 0U;
    FengSlice *seen_names = NULL;
    size_t seen_count = 0U;
    size_t seen_capacity = 0U;
    FengSpecWitness *witness;
    size_t ci;

    if (context == NULL || context->analysis == NULL ||
        type_decl == NULL || spec_decl == NULL) {
        return;
    }
    if (type_decl->kind != FENG_DECL_TYPE || spec_decl->kind != FENG_DECL_SPEC) {
        return;
    }
    if (spec_decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
        return;
    }
    if (feng_semantic_lookup_spec_witness(context->analysis,
                                          type_decl, spec_decl) != NULL) {
        return;
    }

    witness = feng_semantic_reserve_spec_witness(context->analysis,
                                                 type_decl, spec_decl);
    if (witness == NULL) {
        return;
    }
    if (!spec_collect_closure(context, spec_decl, &closure,
                              &closure_count, &closure_capacity)) {
        free(closure);
        return;
    }

    for (ci = 0U; ci < closure_count; ++ci) {
        const FengDecl *cur = closure[ci];
        size_t mi;

        if (cur->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            continue;
        }
        for (mi = 0U; mi < cur->as.spec_decl.as.object.member_count; ++mi) {
            const FengTypeMember *sm = cur->as.spec_decl.as.object.members[mi];
            FengSlice name;
            bool dup = false;
            size_t k;

            if (sm == NULL) {
                continue;
            }
            if (sm->kind == FENG_TYPE_MEMBER_FIELD) {
                name = sm->as.field.name;
            } else if (sm->kind == FENG_TYPE_MEMBER_METHOD) {
                name = sm->as.callable.name;
            } else {
                continue;
            }
            for (k = 0U; k < seen_count; ++k) {
                if (slice_equals(seen_names[k], name)) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (seen_count == seen_capacity) {
                size_t new_cap = seen_capacity == 0U ? 8U : seen_capacity * 2U;
                FengSlice *grown = realloc(seen_names, new_cap * sizeof(*grown));

                if (grown == NULL) {
                    free(seen_names);
                    free(closure);
                    return;
                }
                seen_names = grown;
                seen_capacity = new_cap;
            }
            seen_names[seen_count++] = name;

            if (sm->kind == FENG_TYPE_MEMBER_FIELD) {
                const FengTypeMember *t_field = type_find_field(type_decl, name);

                (void)feng_semantic_spec_witness_append_member(
                    witness, sm, t_field,
                    FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_FIELD,
                    NULL, NULL);
                continue;
            }

            /* Method member — collect signature-matching candidates from T
             * and from every visible fit. */
            {
                const FengTypeMember *t_method = type_find_matching_method(
                    context, type_decl, &sm->as.callable, NULL, 0U);
                WitnessFitCollectCtx fit_st;
                size_t total;

                fit_st.ctx = context;
                fit_st.spec_sig = &sm->as.callable;
                fit_st.items = NULL;
                fit_st.count = 0U;
                fit_st.capacity = 0U;
                fit_st.oom = false;
                (void)visit_visible_fit_methods_for_type(
                    context, type_decl, name, true,
                    witness_fit_collect_visitor, &fit_st);
                if (fit_st.oom) {
                    free(fit_st.items);
                    free(seen_names);
                    free(closure);
                    return;
                }

                total = (t_method != NULL ? 1U : 0U) + fit_st.count;
                if (total == 0U) {
                    /* Missing — diagnostic is the responsibility of the
                     * type/fit validation passes. Record a NULL slot. */
                    (void)feng_semantic_spec_witness_append_member(
                        witness, sm, NULL,
                        FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD,
                        NULL, NULL);
                } else if (total == 1U) {
                    if (t_method != NULL) {
                        (void)feng_semantic_spec_witness_append_member(
                            witness, sm, t_method,
                            FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD,
                            NULL, NULL);
                    } else {
                        (void)feng_semantic_spec_witness_append_member(
                            witness, sm, fit_st.items[0].method,
                            FENG_SPEC_WITNESS_SOURCE_FIT_METHOD,
                            fit_st.items[0].fit_decl,
                            fit_st.items[0].fit_module);
                    }
                } else {
                    /* §8.1 — multiple visible-face implementations of the
                     * same (name, params, return). Report once, at the
                     * triggering coercion site. */
                    (void)resolver_append_error(
                        context, err_token,
                        format_message(
                            "type '%.*s' has multiple visible implementations of method '%.*s' required by spec '%.*s' (one or more fits and/or the type itself)",
                            (int)decl_typeish_name(type_decl).length,
                            decl_typeish_name(type_decl).data,
                            (int)name.length, name.data,
                            (int)spec_decl->as.spec_decl.name.length,
                            spec_decl->as.spec_decl.name.data));
                    (void)feng_semantic_spec_witness_append_member(
                        witness, sm, NULL,
                        FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD,
                        NULL, NULL);
                }
                free(fit_st.items);
            }
        }
    }

    free(seen_names);
    free(closure);
}

static bool collect_type_decl_satisfied_specs(const ResolveContext *ctx,
                                              const FengDecl *type_decl,
                                              const FengDecl ***out_set,
                                              size_t *out_count,
                                              size_t *out_capacity) {
    size_t i;
    size_t p;
    size_t d;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return true;
    }

    /* Specs from the type's own declared spec list (transitive). */
    for (i = 0U; i < type_decl->as.type_decl.declared_spec_count; ++i) {
        const FengDecl *spec = resolve_type_ref_decl(ctx, type_decl->as.type_decl.declared_specs[i]);

        if (!spec_collect_closure(ctx, spec, out_set, out_count, out_capacity)) {
            return false;
        }
    }

    /* Specs from every visible fit declaration (current module + cross-module
     * `pu fit`s the consumer has imported via `use`). Mirrors docs/feng-fit.md
     * §4 — a `pu fit` activates in the importing module after `use`. */
    if (ctx->analysis != NULL) {
        size_t m_idx;

        for (m_idx = 0U; m_idx < ctx->analysis->module_count; ++m_idx) {
            const FengSemanticModule *m = &ctx->analysis->modules[m_idx];

            for (p = 0U; p < m->program_count; ++p) {
                const FengProgram *prog = m->programs[p];

                if (prog == NULL) {
                    continue;
                }
                for (d = 0U; d < prog->declaration_count; ++d) {
                    const FengDecl *fd = prog->declarations[d];
                    size_t s;

                    if (fd == NULL || fd->kind != FENG_DECL_FIT) {
                        continue;
                    }
                    if (!fit_decl_is_visible_from(ctx, m, fd)) {
                        continue;
                    }
                    if (resolve_type_ref_decl(ctx, fd->as.fit_decl.target) != type_decl) {
                        continue;
                    }
                    for (s = 0U; s < fd->as.fit_decl.spec_count; ++s) {
                        const FengDecl *spec = resolve_type_ref_decl(ctx, fd->as.fit_decl.specs[s]);

                        if (!spec_collect_closure(ctx, spec, out_set, out_count, out_capacity)) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

static bool type_decl_satisfies_spec_decl(const ResolveContext *ctx,
                                          const FengDecl *type_decl,
                                          const FengDecl *spec_decl) {
    const FengDecl **closure = NULL;
    size_t closure_count = 0U;
    size_t closure_capacity = 0U;
    size_t i;
    bool found = false;

    if (type_decl == NULL || spec_decl == NULL ||
        type_decl->kind != FENG_DECL_TYPE || spec_decl->kind != FENG_DECL_SPEC) {
        return false;
    }
    if (!collect_type_decl_satisfied_specs(ctx, type_decl, &closure,
                                           &closure_count, &closure_capacity)) {
        free(closure);
        return false;
    }
    for (i = 0U; i < closure_count; ++i) {
        if (closure[i] == spec_decl) {
            found = true;
            break;
        }
    }
    free(closure);
    return found;
}

static bool detect_cross_spec_method_conflicts(ResolveContext *ctx,
                                               const FengDecl *type_decl,
                                               const FengDecl *const *spec_set,
                                               size_t spec_count,
                                               FengToken err_token) {
    size_t i;
    size_t j;
    size_t a;
    size_t b;
    size_t p;

    for (i = 0U; i < spec_count; ++i) {
        const FengDecl *spec_a = spec_set[i];

        if (spec_a->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
            continue;
        }
        for (a = 0U; a < spec_a->as.spec_decl.as.object.member_count; ++a) {
            const FengTypeMember *ma = spec_a->as.spec_decl.as.object.members[a];

            if (ma->kind != FENG_TYPE_MEMBER_METHOD) {
                continue;
            }
            for (j = i + 1U; j < spec_count; ++j) {
                const FengDecl *spec_b = spec_set[j];

                if (spec_b->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
                    continue;
                }
                for (b = 0U; b < spec_b->as.spec_decl.as.object.member_count; ++b) {
                    const FengTypeMember *mb = spec_b->as.spec_decl.as.object.members[b];
                    bool same_params;

                    if (mb->kind != FENG_TYPE_MEMBER_METHOD ||
                        !slice_eq(ma->as.callable.name, mb->as.callable.name) ||
                        ma->as.callable.param_count != mb->as.callable.param_count) {
                        continue;
                    }
                    same_params = true;
                    for (p = 0U; p < ma->as.callable.param_count; ++p) {
                        if (!type_refs_semantically_equal(ctx,
                                                          ma->as.callable.params[p].type,
                                                          mb->as.callable.params[p].type)) {
                            same_params = false;
                            break;
                        }
                    }
                    if (!same_params) {
                        continue;
                    }
                    if (!type_refs_semantically_equal(ctx,
                                                      ma->as.callable.return_type,
                                                      mb->as.callable.return_type)) {
                        return resolver_append_error(
                            ctx, err_token,
                            format_message(
                                "type '%.*s' satisfies specs '%.*s' and '%.*s' which both declare method '%.*s' with the same parameters but different return types",
                                (int)decl_typeish_name(type_decl).length,
                                decl_typeish_name(type_decl).data,
                                (int)spec_a->as.spec_decl.name.length,
                                spec_a->as.spec_decl.name.data,
                                (int)spec_b->as.spec_decl.name.length,
                                spec_b->as.spec_decl.name.data,
                                (int)ma->as.callable.name.length,
                                ma->as.callable.name.data));
                    }
                }
            }
        }
    }
    return true;
}

/* Two parameter type references potentially overlap when there exists at
 * least one concrete type T (visible in the current analysis) that can be
 * supplied as an argument to both parameters under the visible explicit
 * contract relations (declared spec lists and `fit` declarations). This
 * mirrors docs/feng-function.md §5: "若同一重载集合中的两个候选在当前可见
 * 的显式契约关系下可能同时匹配同一实参类型，必须视为签名冲突". The check
 * is intentionally nominal — duck typing is not considered. */
static bool param_type_refs_potentially_overlap(const ResolveContext *ctx,
                                                const FengTypeRef *a,
                                                const FengTypeRef *b) {
    const FengDecl *da;
    const FengDecl *db;
    size_t module_index;
    size_t program_index;
    size_t decl_index;

    if (ctx == NULL || a == NULL || b == NULL) {
        return false;
    }
    if (type_refs_semantically_equal(ctx, a, b)) {
        return true;
    }

    da = resolve_type_ref_decl(ctx, a);
    db = resolve_type_ref_decl(ctx, b);
    if (da == NULL || db == NULL) {
        return false;
    }
    if (da == db) {
        return true;
    }
    if (da->kind == FENG_DECL_TYPE && db->kind == FENG_DECL_SPEC) {
        return type_decl_satisfies_spec_decl(ctx, da, db);
    }
    if (db->kind == FENG_DECL_TYPE && da->kind == FENG_DECL_SPEC) {
        return type_decl_satisfies_spec_decl(ctx, db, da);
    }
    if (da->kind != FENG_DECL_SPEC || db->kind != FENG_DECL_SPEC) {
        return false;
    }

    /* Both parameters are object specs — they overlap when at least one
     * concrete type in the analysis satisfies both specs (either via its
     * declared spec list, transitive parents, or a visible fit). */
    for (module_index = 0U; module_index < ctx->analysis->module_count; ++module_index) {
        const FengSemanticModule *m = &ctx->analysis->modules[module_index];

        for (program_index = 0U; program_index < m->program_count; ++program_index) {
            const FengProgram *prog = m->programs[program_index];

            if (prog == NULL) {
                continue;
            }
            for (decl_index = 0U; decl_index < prog->declaration_count; ++decl_index) {
                const FengDecl *t = prog->declarations[decl_index];

                if (t == NULL || t->kind != FENG_DECL_TYPE) {
                    continue;
                }
                if (type_decl_satisfies_spec_decl(ctx, t, da) &&
                    type_decl_satisfies_spec_decl(ctx, t, db)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool signatures_potentially_overlap(const ResolveContext *ctx,
                                           const FengCallableSignature *a,
                                           const FengCallableSignature *b) {
    size_t i;

    if (a == NULL || b == NULL || a->param_count != b->param_count) {
        return false;
    }
    for (i = 0U; i < a->param_count; ++i) {
        if (!param_type_refs_potentially_overlap(ctx, a->params[i].type, b->params[i].type)) {
            return false;
        }
    }
    return true;
}

static bool validate_type_member_overload_overlap(ResolveContext *context,
                                                  const FengDecl *decl) {
    size_t i;
    size_t j;
    bool ok = true;

    if (context == NULL || decl == NULL || decl->kind != FENG_DECL_TYPE) {
        return true;
    }

    for (i = 0U; i < decl->as.type_decl.member_count; ++i) {
        const FengTypeMember *mi = decl->as.type_decl.members[i];
        const FengCallableSignature *si;

        if (mi == NULL || mi->kind != FENG_TYPE_MEMBER_METHOD) {
            continue;
        }
        si = &mi->as.callable;

        for (j = 0U; j < i; ++j) {
            const FengTypeMember *mj = decl->as.type_decl.members[j];
            const FengCallableSignature *sj;

            if (mj == NULL || mj->kind != FENG_TYPE_MEMBER_METHOD) {
                continue;
            }
            sj = &mj->as.callable;
            if (!slice_equals(si->name, sj->name)) {
                continue;
            }
            /* Identical-parameter pairs are already reported by
             * validate_type_member_overloads with a more specific message. */
            if (parameters_equal(si, sj)) {
                continue;
            }
            if (signatures_potentially_overlap(context, si, sj)) {
                ok = resolver_append_error(
                         context,
                         si->token,
                         format_message(
                             "method overloads in type '%.*s' may both match the same arguments under visible contract relations: '%.*s'",
                             (int)decl->as.type_decl.name.length,
                             decl->as.type_decl.name.data,
                             (int)si->name.length, si->name.data)) && ok;
            }
        }
    }
    return ok;
}

static bool validate_top_level_overload_overlap(ResolveContext *context,
                                                const FengProgram *program) {
    size_t decl_index;
    size_t set_index;
    size_t k;
    bool ok = true;

    if (context == NULL || program == NULL) {
        return true;
    }

    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        const FunctionOverloadSetEntry *set;
        const FengCallableSignature *current;

        if (decl == NULL || decl->kind != FENG_DECL_FUNCTION) {
            continue;
        }
        current = &decl->as.function_decl;

        for (set_index = 0U; set_index < context->function_set_count; ++set_index) {
            if (slice_equals(context->function_sets[set_index].name, current->name)) {
                break;
            }
        }
        if (set_index == context->function_set_count) {
            continue;
        }
        set = &context->function_sets[set_index];

        for (k = 0U; k < set->decl_count; ++k) {
            const FengDecl *other = set->decls[k];
            const FengCallableSignature *other_sig;

            /* The function_sets array is populated by check_symbol_conflicts
             * in declaration order. To report each pair exactly once we stop
             * at the current decl and only compare against earlier-registered
             * candidates. */
            if (other == decl) {
                break;
            }
            if (other == NULL || other->kind != FENG_DECL_FUNCTION) {
                continue;
            }
            other_sig = &other->as.function_decl;
            /* Identical-parameter pairs are already reported by
             * check_symbol_conflicts with a more specific message. */
            if (parameters_equal(current, other_sig)) {
                continue;
            }
            if (signatures_potentially_overlap(context, current, other_sig)) {
                ok = resolver_append_error(
                         context,
                         current->token,
                         format_message(
                             "function overloads may both match the same arguments under visible contract relations: '%.*s'",
                             (int)current->name.length,
                             current->name.data)) && ok;
            }
        }
    }
    return ok;
}

static bool validate_type_declared_specs_and_satisfaction(ResolveContext *context,
                                                          const FengDecl *type_decl) {
    size_t i;
    size_t j;
    const FengDecl **closure = NULL;
    size_t closure_count = 0U;
    size_t closure_capacity = 0U;
    bool ok = true;

    for (i = 0U; i < type_decl->as.type_decl.declared_spec_count; ++i) {
        const FengTypeRef *r = type_decl->as.type_decl.declared_specs[i];
        const FengDecl *resolved = resolve_type_ref_decl(context, r);

        if (resolved == NULL || resolved->kind != FENG_DECL_SPEC) {
            char *target_name = format_type_ref_name(r);
            bool result = resolver_append_error(
                context, r->token,
                format_message(
                    "type '%.*s' declared spec list must contain only spec types but found '%s'",
                    (int)type_decl->as.type_decl.name.length,
                    type_decl->as.type_decl.name.data,
                    target_name != NULL ? target_name : "<unknown>"));

            free(target_name);
            return result;
        }
        for (j = 0U; j < i; ++j) {
            const FengDecl *prev = resolve_type_ref_decl(context, type_decl->as.type_decl.declared_specs[j]);

            if (prev == resolved) {
                return resolver_append_error(
                    context, r->token,
                    format_message(
                        "type '%.*s' lists '%.*s' more than once in its declared spec list",
                        (int)type_decl->as.type_decl.name.length,
                        type_decl->as.type_decl.name.data,
                        (int)resolved->as.spec_decl.name.length,
                        resolved->as.spec_decl.name.data));
            }
        }
    }

    for (i = 0U; i < type_decl->as.type_decl.declared_spec_count; ++i) {
        const FengDecl *spec = resolve_type_ref_decl(context, type_decl->as.type_decl.declared_specs[i]);

        if (!spec_collect_closure(context, spec, &closure, &closure_count, &closure_capacity)) {
            free(closure);
            return false;
        }
    }
    for (i = 0U; i < closure_count && ok; ++i) {
        ok = verify_type_satisfies_spec(context, type_decl, closure[i],
                                        type_decl->token, NULL, 0U);
    }
    if (ok) {
        ok = detect_cross_spec_method_conflicts(context, type_decl, closure, closure_count,
                                                type_decl->token);
    }
    free(closure);
    return ok;
}

static bool validate_fit_declaration_contracts(ResolveContext *context,
                                               const FengDecl *fit_decl) {
    const FengDecl *target = resolve_type_ref_decl(context, fit_decl->as.fit_decl.target);
    size_t i;
    size_t j;
    const FengDecl **closure = NULL;
    size_t closure_count = 0U;
    size_t closure_capacity = 0U;
    bool ok = true;
    if (target == NULL || target->kind != FENG_DECL_TYPE) {
        char *target_name = format_type_ref_name(fit_decl->as.fit_decl.target);
        bool result = resolver_append_error(
            context, fit_decl->as.fit_decl.target->token,
            format_message(
                "fit target must be a concrete type but found '%s'",
                target_name != NULL ? target_name : "<unknown>"));

        free(target_name);
        return result;
    }

    for (i = 0U; i < fit_decl->as.fit_decl.spec_count; ++i) {
        const FengTypeRef *r = fit_decl->as.fit_decl.specs[i];
        const FengDecl *resolved = resolve_type_ref_decl(context, r);

        if (resolved == NULL || resolved->kind != FENG_DECL_SPEC) {
            char *spec_name = format_type_ref_name(r);
            bool result = resolver_append_error(
                context, r->token,
                format_message(
                    "fit specs list must contain only spec types but found '%s'",
                    spec_name != NULL ? spec_name : "<unknown>"));

            free(spec_name);
            return result;
        }
        for (j = 0U; j < i; ++j) {
            const FengDecl *prev = resolve_type_ref_decl(context, fit_decl->as.fit_decl.specs[j]);

            if (prev == resolved) {
                return resolver_append_error(
                    context, r->token,
                    format_message(
                        "fit lists '%.*s' more than once in its specs clause",
                        (int)resolved->as.spec_decl.name.length,
                        resolved->as.spec_decl.name.data));
            }
        }
    }

    for (i = 0U; i < fit_decl->as.fit_decl.spec_count; ++i) {
        const FengDecl *spec = resolve_type_ref_decl(context, fit_decl->as.fit_decl.specs[i]);

        if (!spec_collect_closure(context, spec, &closure, &closure_count, &closure_capacity)) {
            free(closure);
            return false;
        }
    }
    for (i = 0U; i < closure_count && ok; ++i) {
        ok = verify_type_satisfies_spec(
            context, target, closure[i], fit_decl->token,
            (const FengTypeMember *const *)fit_decl->as.fit_decl.members,
            fit_decl->as.fit_decl.member_count);
    }
    if (ok) {
        ok = detect_cross_spec_method_conflicts(context, target, closure, closure_count,
                                                fit_decl->token);
    }
    if (ok) {
        /* Orphan-fit detection: a fit is an orphan when neither its target type
         * nor any of its specs originates in the current module. The spec mandates
         * that orphan fits cannot be exported across the package boundary, so we
         * emit an info note and downgrade `pu` to module-local visibility. */
        const FengSemanticModule *target_module =
            find_decl_provider_module(context->analysis, target);
        bool is_local = (target_module == context->module);

        for (i = 0U; i < closure_count && !is_local; ++i) {
            const FengSemanticModule *spec_module =
                find_decl_provider_module(context->analysis, closure[i]);
            if (spec_module == context->module) {
                is_local = true;
            }
        }
        if (!is_local && fit_decl->visibility == FENG_VISIBILITY_PUBLIC) {
            FengDecl *mutable_decl = (FengDecl *)fit_decl;
            char *target_name = format_type_ref_name(fit_decl->as.fit_decl.target);
            char *message = format_message(
                "orphan fit for '%s' cannot be exported; downgraded to module-local visibility",
                target_name != NULL ? target_name : "<unknown>");

            free(target_name);
            mutable_decl->visibility = FENG_VISIBILITY_PRIVATE;
            if (!analysis_append_info(context->analysis, context->program->path,
                                      fit_decl->token, message)) {
                ok = false;
            }
        }
    }
    free(closure);
    return ok;
}

static bool resolve_declaration(ResolveContext *context, const FengDecl *decl) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return resolve_binding(context, &decl->as.binding, false, false);

        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                if (!resolve_type_ref(context, decl->as.type_decl.declared_specs[index], false)) {
                    return false;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];
                const FengDecl *previous_type_decl = context->current_type_decl;
                const FengTypeMember *previous_callable_member = context->current_callable_member;

                if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                    bool field_is_callable_spec = false;
                    bool prev_self_capturable = context->self_capturable;
                    bool init_is_lambda = member->as.field.initializer != NULL &&
                                          member->as.field.initializer->kind == FENG_EXPR_LAMBDA;

                    if (!resolve_type_ref(context, member->as.field.type, false)) {
                        return false;
                    }
                    /* Per docs/feng-function.md: a callable-spec field whose
                     * initializer is a lambda may capture the enclosing
                     * type's `self`, because the lambda runs only when the
                     * callable is invoked, after the object is constructed.
                     * Direct `self` references in the initializer expression
                     * itself remain disallowed. */
                    if (init_is_lambda &&
                        resolve_function_type_decl(context, member->as.field.type) != NULL) {
                        field_is_callable_spec = true;
                        context->current_type_decl = decl;
                        context->self_capturable = true;
                    }
                    {
                        InferredExprType field_type;
                        bool init_ok = resolve_expr(context, member->as.field.initializer, false);
                        bool match_ok = init_ok &&
                                         validate_expr_against_expected_type(
                                             context,
                                             member->as.field.initializer,
                                             member->as.field.type);

                        if (field_is_callable_spec) {
                            context->self_capturable = prev_self_capturable;
                            context->current_type_decl = previous_type_decl;
                        }
                        if (!init_ok || !match_ok) {
                            return false;
                        }

                        field_type = member->as.field.type != NULL
                                         ? inferred_expr_type_from_type_ref(member->as.field.type)
                                         : infer_expr_type(context, member->as.field.initializer);
                        if (!record_type_fact_for_site(context, member, field_type)) {
                            return false;
                        }
                    }
                    if (member->as.field.initializer == NULL) {
                        /* Phase S2-a: a `let`/`var` field whose declared type
                         * is a spec and whose initializer is omitted at the
                         * member declaration site is a default-witness slot
                         * (the field may still be assigned later in the
                         * constructor; that does not change the syntactic
                         * default-witness obligation here). */
                        record_spec_default_binding_if_applicable(
                            context,
                            member,
                            FENG_SPEC_DEFAULT_BINDING_POSITION_TYPE_FIELD,
                            member->as.field.type);
                    }
                    continue;
                }

                context->current_type_decl = decl;
                context->current_callable_member = member;
                if (!resolve_callable(context, &member->as.callable, true)) {
                    context->current_callable_member = previous_callable_member;
                    context->current_type_decl = previous_type_decl;
                    return false;
                }
                if (!validate_fixed_callable_member(context, member)) {
                    context->current_callable_member = previous_callable_member;
                    context->current_type_decl = previous_type_decl;
                    return false;
                }
                context->current_callable_member = previous_callable_member;
                context->current_type_decl = previous_type_decl;
            }

            if (!validate_fixed_type_declaration(context, decl)) {
                return false;
            }
            if (!validate_type_member_overloads(context, decl)) {
                return false;
            }
            if (!validate_type_member_overload_overlap(context, decl)) {
                return false;
            }
            if (!validate_type_finalizer_constraints(context, decl)) {
                return false;
            }
            return validate_type_declared_specs_and_satisfaction(context, decl);

        case FENG_DECL_SPEC:
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                if (!resolve_type_ref(context, decl->as.spec_decl.parent_specs[index], false)) {
                    return false;
                }
            }
            if (!validate_spec_parent_spec_list(context, decl)) {
                return false;
            }
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                if (!resolve_type_ref(context, decl->as.spec_decl.as.callable.return_type, true)) {
                    return false;
                }
                for (index = 0U; index < decl->as.spec_decl.as.callable.param_count; ++index) {
                    if (!resolve_type_ref(context,
                                          decl->as.spec_decl.as.callable.params[index].type,
                                          false)) {
                        return false;
                    }
                }
                return validate_fixed_type_declaration(context, decl);
            }
            for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                const FengTypeMember *member = decl->as.spec_decl.as.object.members[index];

                if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                    if (!resolve_type_ref(context, member->as.field.type, false)) {
                        return false;
                    }
                    continue;
                }
                if (!resolve_type_ref(context, member->as.callable.return_type, true)) {
                    return false;
                }
                {
                    size_t param_index;
                    for (param_index = 0U; param_index < member->as.callable.param_count; ++param_index) {
                        if (!resolve_type_ref(context,
                                              member->as.callable.params[param_index].type,
                                              false)) {
                            return false;
                        }
                    }
                }
            }
            return validate_fixed_type_declaration(context, decl);

        case FENG_DECL_FIT: {
            size_t fit_index;
            const FengDecl *fit_target_decl;
            if (!resolve_type_ref(context, decl->as.fit_decl.target, false)) {
                return false;
            }
            for (fit_index = 0U; fit_index < decl->as.fit_decl.spec_count; ++fit_index) {
                if (!resolve_type_ref(context, decl->as.fit_decl.specs[fit_index], false)) {
                    return false;
                }
            }
            /* Resolve the fit's target type once so that fit-body methods can
             * see `self` as the target type and trigger normal instance member
             * checks. The target may legitimately fail to resolve here (e.g.
             * when the right-hand side is a built-in or unresolved name); in
             * that case `validate_fit_declaration_contracts` reports the proper
             * diagnostic and we simply skip context propagation. */
            fit_target_decl = resolve_type_ref_decl(context, decl->as.fit_decl.target);
            for (fit_index = 0U; fit_index < decl->as.fit_decl.member_count; ++fit_index) {
                const FengTypeMember *member = decl->as.fit_decl.members[fit_index];
                const FengDecl *previous_type_decl = context->current_type_decl;
                const FengDecl *previous_fit_decl = context->current_fit_decl;
                const FengTypeMember *previous_callable_member =
                    context->current_callable_member;
                bool ok;

                if (fit_target_decl != NULL && fit_target_decl->kind == FENG_DECL_TYPE) {
                    context->current_type_decl = fit_target_decl;
                }
                context->current_fit_decl = decl;
                context->current_callable_member = member;

                ok = resolve_callable(context, &member->as.callable, true);

                context->current_callable_member = previous_callable_member;
                context->current_fit_decl = previous_fit_decl;
                context->current_type_decl = previous_type_decl;

                if (!ok) {
                    return false;
                }
            }
            return validate_fit_declaration_contracts(context, decl);
        }

        case FENG_DECL_FUNCTION:
            if (!validate_extern_function_annotations(context, decl)) {
                return false;
            }
            if (!resolve_callable(context, &decl->as.function_decl, false)) {
                return false;
            }
            if (!validate_extern_function_signature(context, decl)) {
                return false;
            }
            return validate_fixed_function_declaration(context, decl);
    }

    return true;
}

static bool resolve_program_names(const FengSemanticAnalysis *analysis,
                                  const FengSemanticModule *module,
                                  const FengProgram *program,
                                  const VisibleTypeEntry *visible_types,
                                  size_t visible_type_count,
                                  const VisibleValueEntry *visible_values,
                                  size_t visible_value_count,
                                  const FunctionOverloadSetEntry *function_sets,
                                  size_t function_set_count,
                                  CallableReturnCache *callable_return_cache,
                                  CallableExceptionEscapeCache *callable_exception_escape_cache,
                                  FengSemanticError **errors,
                                  size_t *error_count,
                                  size_t *error_capacity) {
    ResolveContext context;
    AliasEntry *aliases = NULL;
    size_t alias_count = 0U;
    size_t alias_capacity = 0U;
    ImportedModuleEntry *imported_modules = NULL;
    size_t imported_module_count = 0U;
    size_t imported_module_capacity = 0U;
    size_t decl_index;
    size_t use_index;
    bool ok;

    memset(&context, 0, sizeof(context));
    context.analysis = analysis;
    context.module = module;
    context.program = program;
    context.visible_types = visible_types;
    context.visible_type_count = visible_type_count;
    context.visible_values = visible_values;
    context.visible_value_count = visible_value_count;
    context.function_sets = function_sets;
    context.function_set_count = function_set_count;
    context.callable_return_cache = callable_return_cache;
    context.callable_exception_escape_cache = callable_exception_escape_cache;
    context.errors = errors;
    context.error_count = error_count;
    context.error_capacity = error_capacity;

    ok = build_program_aliases(analysis, program, &aliases, &alias_count, &alias_capacity);
    if (!ok) {
        free(aliases);
        return false;
    }

    /* Collect every `use`-imported target module (short-name and aliased)
     * so that cross-module visibility checks can require an explicit `use`. */
    for (use_index = 0U; use_index < program->use_count && ok; ++use_index) {
        const FengUseDecl *use_decl = &program->uses[use_index];
        size_t target_index =
            find_module_index_by_path(analysis, use_decl->segments, use_decl->segment_count);
        ImportedModuleEntry entry;
        size_t scan;
        bool already = false;

        if (target_index == analysis->module_count) {
            continue;
        }
        entry.target_module = &analysis->modules[target_index];
        for (scan = 0U; scan < imported_module_count; ++scan) {
            if (imported_modules[scan].target_module == entry.target_module) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        ok = append_raw((void **)&imported_modules,
                        &imported_module_count,
                        &imported_module_capacity,
                        sizeof(entry),
                        &entry);
    }
    if (!ok) {
        free(aliases);
        free(imported_modules);
        return false;
    }

    context.aliases = aliases;
    context.alias_count = alias_count;
    context.imported_modules = imported_modules;
    context.imported_module_count = imported_module_count;

    for (decl_index = 0U; decl_index < program->declaration_count && ok; ++decl_index) {
        ok = resolve_declaration(&context, program->declarations[decl_index]);
    }

    if (ok) {
        ok = validate_top_level_overload_overlap(&context, program);
    }

    resolver_free_scopes(&context);
    free(aliases);
    free(imported_modules);
    return ok;
}

static bool check_symbol_conflicts(const FengSemanticAnalysis *analysis,
                                   const FengSemanticModule *module,
                                   CallableReturnCache *callable_return_cache,
                                   CallableExceptionEscapeCache *callable_exception_escape_cache,
                                   FengSemanticError **errors,
                                   size_t *error_count,
                                   size_t *error_capacity) {
    VisibleTypeEntry *visible_types = NULL;
    VisibleValueEntry *visible_values = NULL;
    FunctionOverloadSetEntry *function_sets = NULL;
    size_t visible_type_count = 0U;
    size_t visible_value_count = 0U;
    size_t function_set_count = 0U;
    size_t visible_type_capacity = 0U;
    size_t visible_value_capacity = 0U;
    size_t function_set_capacity = 0U;
    size_t program_index;
    bool ok = true;

    for (program_index = 0U; program_index < module->program_count && ok; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count && ok; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];
            size_t index;

            switch (decl->kind) {
                case FENG_DECL_TYPE: {
                    VisibleTypeEntry entry;

                    index = find_visible_type_index(visible_types, visible_type_count, decl->as.type_decl.name);
                    if (index < visible_type_count) {
                        char *message = format_message("duplicate type declaration '%.*s'",
                                                       (int)decl->as.type_decl.name.length,
                                                       decl->as.type_decl.name.data);

                        ok = append_error(errors,
                                          error_count,
                                          error_capacity,
                                          program->path,
                                          *decl_token(decl),
                                          message);
                        break;
                    }
                    if (!ok) {
                        break;
                    }

                    entry.name = decl->as.type_decl.name;
                    entry.provider_module = module;
                    entry.decl = decl;
                    ok = append_raw((void **)&visible_types,
                                    &visible_type_count,
                                    &visible_type_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_GLOBAL_BINDING: {
                    VisibleValueEntry entry;

                    index = find_visible_value_index(visible_values, visible_value_count, decl->as.binding.name);
                    if (index < visible_value_count) {
                        char *message;

                        if (visible_values[index].is_function) {
                            message = format_message(
                                "top-level binding '%.*s' conflicts with an existing top-level function",
                                (int)decl->as.binding.name.length,
                                decl->as.binding.name.data);
                        } else {
                            message = format_message("duplicate top-level binding '%.*s'",
                                                     (int)decl->as.binding.name.length,
                                                     decl->as.binding.name.data);
                        }

                        ok = append_error(errors,
                                          error_count,
                                          error_capacity,
                                          program->path,
                                          decl->as.binding.token,
                                          message);
                        break;
                    }
                    if (!ok) {
                        break;
                    }

                    entry.name = decl->as.binding.name;
                    entry.provider_module = module;
                    entry.decl = decl;
                    entry.mutability = decl->as.binding.mutability;
                    entry.is_function = false;
                    ok = append_raw((void **)&visible_values,
                                    &visible_value_count,
                                    &visible_value_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_FUNCTION: {
                    size_t value_index =
                        find_visible_value_index(visible_values, visible_value_count, decl->as.function_decl.name);
                    size_t function_set_index = find_function_overload_set_index(
                        function_sets, function_set_count, decl->as.function_decl.name);

                    if (value_index < visible_value_count && !visible_values[value_index].is_function) {
                        char *message = format_message(
                            "top-level function '%.*s' conflicts with an existing top-level binding",
                            (int)decl->as.function_decl.name.length,
                            decl->as.function_decl.name.data);

                        ok = append_error(errors,
                                          error_count,
                                          error_capacity,
                                          program->path,
                                          decl->as.function_decl.token,
                                          message);
                        break;
                    }

                    if (function_set_index < function_set_count) {
                        const FunctionOverloadSetEntry *function_set = &function_sets[function_set_index];

                        for (index = 0U; index < function_set->decl_count; ++index) {
                            const FengCallableSignature *existing =
                                &function_set->decls[index]->as.function_decl;

                            if (!parameters_equal(existing, &decl->as.function_decl)) {
                                continue;
                            }

                            if (return_type_equals(existing->return_type, decl->as.function_decl.return_type)) {
                                char *message = format_message("duplicate function signature '%.*s'",
                                                               (int)decl->as.function_decl.name.length,
                                                               decl->as.function_decl.name.data);

                                ok = append_error(errors,
                                                  error_count,
                                                  error_capacity,
                                                  program->path,
                                                  decl->as.function_decl.token,
                                                  message);
                            } else {
                                char *message = format_message(
                                    "function overloads cannot differ only by return type: '%.*s'",
                                    (int)decl->as.function_decl.name.length,
                                    decl->as.function_decl.name.data);

                                ok = append_error(errors,
                                                  error_count,
                                                  error_capacity,
                                                  program->path,
                                                  decl->as.function_decl.token,
                                                  message);
                            }
                            break;
                        }
                        if (!ok) {
                            break;
                        }
                        if (index < function_set->decl_count) {
                            break;
                        }
                    } else {
                        FunctionOverloadSetEntry entry;

                        memset(&entry, 0, sizeof(entry));
                        entry.name = decl->as.function_decl.name;
                        entry.provider_module = module;
                        ok = append_raw((void **)&function_sets,
                                        &function_set_count,
                                        &function_set_capacity,
                                        sizeof(entry),
                                        &entry);
                        if (!ok) {
                            break;
                        }
                        function_set_index = function_set_count - 1U;
                    }

                    if (value_index == visible_value_count) {
                        VisibleValueEntry value_entry;

                        value_entry.name = decl->as.function_decl.name;
                        value_entry.provider_module = module;
                        value_entry.decl = decl;
                        value_entry.mutability = FENG_MUTABILITY_DEFAULT;
                        value_entry.is_function = true;
                        ok = append_raw((void **)&visible_values,
                                        &visible_value_count,
                                        &visible_value_capacity,
                                        sizeof(value_entry),
                                        &value_entry);
                        if (!ok) {
                            break;
                        }
                    }

                    ok = append_visible_function_overload(&function_sets,
                                                          &function_set_count,
                                                          &function_set_capacity,
                                                          module,
                                                          decl);
                    break;
                }

                case FENG_DECL_SPEC: {
                    VisibleTypeEntry entry;

                    index = find_visible_type_index(visible_types, visible_type_count, decl->as.spec_decl.name);
                    if (index < visible_type_count) {
                        char *message = format_message("duplicate type declaration '%.*s'",
                                                       (int)decl->as.spec_decl.name.length,
                                                       decl->as.spec_decl.name.data);

                        ok = append_error(errors,
                                          error_count,
                                          error_capacity,
                                          program->path,
                                          *decl_token(decl),
                                          message);
                        break;
                    }

                    entry.name = decl->as.spec_decl.name;
                    entry.provider_module = module;
                    entry.decl = decl;
                    ok = append_raw((void **)&visible_types,
                                    &visible_type_count,
                                    &visible_type_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_FIT:
                    /* fit declarations register adapter relationships; they do not
                       introduce a new top-level name and are processed elsewhere. */
                    break;
            }
        }
    }

    for (program_index = 0U; program_index < module->program_count && ok; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t use_index;

        for (use_index = 0U; use_index < program->use_count && ok; ++use_index) {
            const FengUseDecl *use_decl = &program->uses[use_index];
            size_t prior_index;
            size_t target_index;

            if (use_decl->has_alias) {
                for (prior_index = 0U; prior_index < use_index; ++prior_index) {
                    if (program->uses[prior_index].has_alias &&
                        slice_equals(program->uses[prior_index].alias, use_decl->alias)) {
                        ok = append_error(errors,
                                          error_count,
                                          error_capacity,
                                          program->path,
                                          use_decl->token,
                                          format_message("duplicate use alias '%.*s' in the same file",
                                                         (int)use_decl->alias.length,
                                                         use_decl->alias.data));
                        break;
                    }
                }
                if (!ok) {
                    break;
                }
            }

            target_index =
                find_module_index_by_path(analysis, use_decl->segments, use_decl->segment_count);
            if (target_index == analysis->module_count) {
                char *module_name = format_module_name(use_decl->segments, use_decl->segment_count);

                ok = append_error(
                    errors,
                    error_count,
                    error_capacity,
                    program->path,
                    use_decl->token,
                    format_message("use target module '%s' was not found in current compilation input",
                                   module_name != NULL ? module_name : "<unknown>"));
                free(module_name);
                if (!ok) {
                    break;
                }
                continue;
            }

            if (use_decl->has_alias) {
                continue;
            }

            ok = import_public_names(&analysis->modules[target_index],
                                     program,
                                     use_decl,
                                     &visible_types,
                                     &visible_type_count,
                                     &visible_type_capacity,
                                     &visible_values,
                                     &visible_value_count,
                                     &visible_value_capacity,
                                     &function_sets,
                                     &function_set_count,
                                     &function_set_capacity,
                                     errors,
                                     error_count,
                                     error_capacity);
        }
    }

    for (program_index = 0U; program_index < module->program_count && ok; ++program_index) {
        ok = resolve_program_names(analysis,
                                   module,
                                   module->programs[program_index],
                                   visible_types,
                                   visible_type_count,
                                   visible_values,
                                   visible_value_count,
                                   function_sets,
                                   function_set_count,
                                   callable_return_cache,
                                   callable_exception_escape_cache,
                                   errors,
                                   error_count,
                                   error_capacity);
    }

    free(visible_types);
    free(visible_values);
    free_function_overload_sets(function_sets, function_set_count);
    return ok;
}

static size_t count_all_callables(const FengSemanticAnalysis *analysis) {
    size_t module_index;
    size_t count = 0U;

    if (analysis == NULL) {
        return 0U;
    }

    for (module_index = 0U; module_index < analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &analysis->modules[module_index];
        size_t program_index;

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            const FengProgram *program = module->programs[program_index];
            size_t decl_index;

            for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
                const FengDecl *decl = program->declarations[decl_index];

                if (decl->kind == FENG_DECL_FUNCTION) {
                    ++count;
                    continue;
                }

                if (decl->kind == FENG_DECL_TYPE) {
                    size_t member_index;

                    for (member_index = 0U;
                         member_index < decl->as.type_decl.member_count;
                         ++member_index) {
                        const FengTypeMember *member =
                            decl->as.type_decl.members[member_index];

                        if (member->kind == FENG_TYPE_MEMBER_METHOD ||
                            member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR ||
                            member->kind == FENG_TYPE_MEMBER_FINALIZER) {
                            ++count;
                        }
                    }
                }
            }
        }
    }

    return count;
}

static bool report_uninferred_callable_returns(const FengSemanticAnalysis *analysis,
                                               const CallableReturnCache *callable_return_cache,
                                               FengSemanticError **errors,
                                               size_t *error_count,
                                               size_t *error_capacity) {
    size_t module_index;
    bool ok = true;

    if (analysis == NULL) {
        return true;
    }

    for (module_index = 0U; module_index < analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &analysis->modules[module_index];
        size_t program_index;

        /* External package modules have no local bodies to infer. */
        if (module->origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
            continue;
        }

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            const FengProgram *program = module->programs[program_index];
            size_t decl_index;

            for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
                const FengDecl *decl = program->declarations[decl_index];

                if (decl->kind == FENG_DECL_FUNCTION && decl->as.function_decl.return_type == NULL) {
                    if (find_callable_return_cache_entry(callable_return_cache,
                                                         &decl->as.function_decl) == NULL) {
                        ok = append_error(
                            errors,
                            error_count,
                            error_capacity,
                            program->path,
                            decl->as.function_decl.token,
                            format_message(
                                "function '%.*s' requires an explicit return type because its return type could not be inferred",
                                (int)decl->as.function_decl.name.length,
                                decl->as.function_decl.name.data));
                        if (!ok) {
                            return false;
                        }
                    }
                    continue;
                }

                if (decl->kind == FENG_DECL_TYPE) {
                    size_t member_index;

                    for (member_index = 0U;
                         member_index < decl->as.type_decl.member_count;
                         ++member_index) {
                        const FengTypeMember *member =
                            decl->as.type_decl.members[member_index];

                        if (member->kind != FENG_TYPE_MEMBER_METHOD ||
                            member->as.callable.return_type != NULL) {
                            continue;
                        }
                        if (find_callable_return_cache_entry(callable_return_cache,
                                                             &member->as.callable) != NULL) {
                            continue;
                        }

                        ok = append_error(
                            errors,
                            error_count,
                            error_capacity,
                            program->path,
                            member->as.callable.token,
                            format_message(
                                "method '%.*s.%.*s' requires an explicit return type because its return type could not be inferred",
                                (int)decl->as.type_decl.name.length,
                                decl->as.type_decl.name.data,
                                (int)member->as.callable.name.length,
                                member->as.callable.name.data));
                        if (!ok) {
                            return false;
                        }
                    }
                }
            }
        }
    }

    return true;
}

static bool type_ref_is_named_simple(const FengTypeRef *type_ref, const char *name) {
    size_t len;

    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count != 1U) {
        return false;
    }
    len = strlen(name);
    return type_ref->as.named.segments[0].length == len &&
           memcmp(type_ref->as.named.segments[0].data, name, len) == 0;
}

static bool main_param_type_is_string_array(const FengTypeRef *type_ref) {
    return type_ref != NULL && type_ref->kind == FENG_TYPE_REF_ARRAY &&
           type_ref_is_named_simple(type_ref->as.inner, "string");
}

static bool validate_main_entry(const FengSemanticAnalysis *analysis,
                                FengCompileTarget target,
                                FengSemanticError **errors,
                                size_t *error_count,
                                size_t *error_capacity) {
    static const FengToken kEmptyToken = {0};
    const FengDecl *first_main = NULL;
    const char *first_main_path = NULL;
    size_t main_count = 0U;
    size_t module_index;
    bool ok = true;

    if (target != FENG_COMPILE_TARGET_BIN || analysis == NULL) {
        return true;
    }

    for (module_index = 0U; module_index < analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &analysis->modules[module_index];
        size_t program_index;

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            const FengProgram *program = module->programs[program_index];
            size_t decl_index;

            for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
                const FengDecl *decl = program->declarations[decl_index];
                const FengCallableSignature *sig;

                if (decl == NULL || decl->kind != FENG_DECL_FUNCTION) {
                    continue;
                }
                sig = &decl->as.function_decl;
                if (sig->name.length != 4U || memcmp(sig->name.data, "main", 4U) != 0) {
                    continue;
                }
                main_count += 1U;
                if (main_count == 1U) {
                    first_main = decl;
                    first_main_path = program->path;
                } else {
                    ok = append_error(errors, error_count, error_capacity,
                                      program->path, sig->token,
                                      format_message(
                                          "duplicate 'main' entry: target 'bin' requires exactly one 'main(args: string[])' across all programs"));
                    if (!ok) {
                        return false;
                    }
                }
            }
        }
    }

    if (main_count == 0U) {
        ok = append_error(errors, error_count, error_capacity,
                          NULL, kEmptyToken,
                          format_message(
                              "target 'bin' requires a 'main(args: string[])' entry function but none was found"));
        return ok;
    }

    {
        const FengCallableSignature *sig = &first_main->as.function_decl;

        if (sig->param_count != 1U ||
            sig->params[0].name.length != 4U ||
            memcmp(sig->params[0].name.data, "args", 4U) != 0 ||
            !main_param_type_is_string_array(sig->params[0].type)) {
            ok = append_error(errors, error_count, error_capacity,
                              first_main_path, sig->token,
                              format_message(
                                  "'main' entry must have signature 'main(args: string[])'"));
            if (!ok) {
                return false;
            }
        }
        if (sig->return_type != NULL && !type_ref_is_void(sig->return_type)) {
            ok = append_error(errors, error_count, error_capacity,
                              first_main_path, sig->token,
                              format_message(
                                  "'main' entry must return void"));
            if (!ok) {
                return false;
            }
        }
        if (decl_is_public(first_main)) {
            /* main is the program entry; visibility is irrelevant here. */
        }
    }

    return ok;
}

bool feng_semantic_analyze_with_options(const FengProgram *const *programs,
                                        size_t program_count,
                                        const FengSemanticAnalyzeOptions *options,
                                        FengSemanticAnalysis **out_analysis,
                                        FengSemanticError **out_errors,
                                        size_t *out_error_count) {
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    size_t error_capacity = 0U;
    size_t program_index;
    size_t iteration;
    size_t max_iterations;
    bool ok = true;
    CallableReturnCache callable_return_cache;
    CallableExceptionEscapeCache callable_exception_escape_cache;
    FengCompileTarget target = options != NULL ? options->target : FENG_COMPILE_TARGET_BIN;
    const FengSemanticImportedModuleQuery *imported_query =
        options != NULL ? options->imported_modules : NULL;

    memset(&callable_return_cache, 0, sizeof(callable_return_cache));
    memset(&callable_exception_escape_cache, 0, sizeof(callable_exception_escape_cache));

    analysis = (FengSemanticAnalysis *)calloc(1U, sizeof(*analysis));
    if (analysis == NULL) {
        ok = false;
        goto finish;
    }

    for (program_index = 0U; program_index < program_count && ok; ++program_index) {
        const FengProgram *program = programs[program_index];
        size_t module_index = find_module_index(analysis, program);

        if (module_index == analysis->module_count) {
            ok = add_module(analysis, program);
            continue;
        }

        if (analysis->modules[module_index].visibility != program->module_visibility) {
            char *module_name = format_module_name(program->module_segments, program->module_segment_count);
            char *message = format_message(
                "all files of module '%s' must use the same module visibility",
                module_name != NULL ? module_name : "<unknown>");

            free(module_name);
            ok = append_error(&errors,
                              &error_count,
                              &error_capacity,
                              program->path,
                              program->module_token,
                              message);
            if (!ok) {
                break;
            }
        }

        ok = append_module_program(&analysis->modules[module_index], program);
    }

    /* Phase S1a: spec satisfaction relation sidecar must be available
     * during the resolve pass so coercion-site recording (Phase S1b) can
     * link each site back to its justifying relation. The pass walks
     * declared `satisfies` clauses and `fit` decls — pure AST data that
     * does not depend on resolved types — so it is safe to run early.
     * See dev/feng-spec-semantic-draft.md §10. */
    if (ok && error_count == 0U) {
        if (!feng_semantic_compute_spec_relations(analysis)) {
            ok = false;
            goto finish;
        }
    }

    /* Pre-inject synthetic FengSemanticModule entries for any external-package
     * 'use' targets.  After injection, find_module_index_by_path() finds them
     * like any local module, so check_symbol_conflicts and build_program_aliases
     * need no special-case logic for external modules. */
    if (ok && imported_query != NULL && imported_query->get_module != NULL) {
        for (program_index = 0U; program_index < program_count && ok; ++program_index) {
            const FengProgram *prog = programs[program_index];
            size_t use_index;

            for (use_index = 0U; use_index < prog->use_count && ok; ++use_index) {
                const FengUseDecl *use_decl = &prog->uses[use_index];
                size_t target_index;
                const FengSemanticModule *ext;

                target_index = find_module_index_by_path(
                    analysis, use_decl->segments, use_decl->segment_count);
                if (target_index < analysis->module_count) {
                    continue; /* already known (local or already injected) */
                }

                ext = imported_query->get_module(
                    imported_query->user, use_decl->segments, use_decl->segment_count);
                if (ext != NULL) {
                    ok = add_external_module(analysis, ext);
                }
            }
        }
    }

    max_iterations = count_all_callables(analysis) + 1U;
    if (max_iterations == 0U) {
        max_iterations = 1U;
    }

    for (iteration = 0U; iteration < max_iterations && ok && error_count == 0U; ++iteration) {
        callable_return_cache.changed = false;
        callable_exception_escape_cache.changed = false;

        for (program_index = 0U;
             program_index < analysis->module_count && ok && error_count == 0U;
             ++program_index) {
            if (analysis->modules[program_index].origin ==
                FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
                continue; /* external modules are pre-resolved; skip semantic analysis */
            }
            ok = check_symbol_conflicts(analysis,
                                        &analysis->modules[program_index],
                                        &callable_return_cache,
                                        &callable_exception_escape_cache,
                                        &errors,
                                        &error_count,
                                        &error_capacity);
        }

        if (!callable_return_cache.changed && !callable_exception_escape_cache.changed) {
            break;
        }
    }

    if (ok && error_count == 0U) {
        ok = report_uninferred_callable_returns(analysis,
                                                &callable_return_cache,
                                                &errors,
                                                &error_count,
                                                &error_capacity);
    }

    if (ok && error_count == 0U) {
        ok = validate_main_entry(analysis, target,
                                 &errors,
                                 &error_count,
                                 &error_capacity);
    }

finish:
    if (out_error_count != NULL) {
        *out_error_count = error_count;
    }

    if (error_count > 0U || !ok) {
        if (out_analysis != NULL) {
            *out_analysis = NULL;
        }
        if (out_errors != NULL) {
            *out_errors = errors;
        } else {
            feng_semantic_errors_free(errors, error_count);
        }
        free_callable_exception_escape_cache(&callable_exception_escape_cache);
        free_callable_return_cache(&callable_return_cache);
        feng_semantic_analysis_free(analysis);
        return false;
    }

    if (out_analysis != NULL) {
        *out_analysis = analysis;
    } else {
        feng_semantic_analysis_free(analysis);
    }
    free_callable_exception_escape_cache(&callable_exception_escape_cache);
    free_callable_return_cache(&callable_return_cache);
    if (out_errors != NULL) {
        *out_errors = NULL;
    }
    /* Phase 1B: post-pass that classifies user `type` decls into acyclic vs
     * potentially-cyclic via Tarjan SCC over the managed-reference graph.
     * Failure here is treated as fatal — codegen relies on the marker table
     * being either complete or empty, never partial. */
    if (out_analysis != NULL && *out_analysis != NULL) {
        if (!feng_semantic_compute_type_cyclicity(*out_analysis)) {
            feng_semantic_analysis_free(*out_analysis);
            *out_analysis = NULL;
            return false;
        }
    }
    return true;
}

bool feng_semantic_analyze(const FengProgram *const *programs,
                           size_t program_count,
                           FengCompileTarget target,
                           FengSemanticAnalysis **out_analysis,
                           FengSemanticError **out_errors,
                           size_t *out_error_count) {
    FengSemanticAnalyzeOptions options;

    memset(&options, 0, sizeof(options));
    options.target = target;
    return feng_semantic_analyze_with_options(programs,
                                              program_count,
                                              &options,
                                              out_analysis,
                                              out_errors,
                                              out_error_count);
}

void feng_semantic_analysis_free(FengSemanticAnalysis *analysis) {
    size_t index;

    if (analysis == NULL) {
        return;
    }

    for (index = 0U; index < analysis->module_count; ++index) {
        free(analysis->modules[index].programs);
    }
    free(analysis->modules);
    free(analysis->type_markers);
    free(analysis->type_facts);
    for (index = 0U; index < analysis->spec_relation_count; ++index) {
        free(analysis->spec_relations[index].sources);
    }
    free(analysis->spec_relations);
    free(analysis->spec_coercion_sites);
    free(analysis->spec_default_bindings);
    free(analysis->spec_member_accesses);
    for (index = 0U; index < analysis->spec_witness_count; ++index) {
        free(analysis->spec_witnesses[index].members);
    }
    free(analysis->spec_witnesses);
    free(analysis->spec_equalities);
    feng_semantic_infos_free(analysis->infos, analysis->info_count);
    free(analysis);
}

void feng_semantic_errors_free(FengSemanticError *errors, size_t error_count) {
    size_t index;

    if (errors == NULL) {
        return;
    }

    for (index = 0U; index < error_count; ++index) {
        free(errors[index].message);
    }
    free(errors);
}

void feng_semantic_infos_free(FengSemanticInfo *infos, size_t info_count) {
    size_t index;

    if (infos == NULL) {
        return;
    }

    for (index = 0U; index < info_count; ++index) {
        free(infos[index].message);
    }
    free(infos);
}
