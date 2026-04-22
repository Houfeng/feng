#include "semantic/semantic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef enum InferredExprTypeKind {
    FENG_INFERRED_EXPR_TYPE_UNKNOWN = 0,
    FENG_INFERRED_EXPR_TYPE_BUILTIN,
    FENG_INFERRED_EXPR_TYPE_TYPE_REF,
    FENG_INFERRED_EXPR_TYPE_DECL
} InferredExprTypeKind;

typedef struct InferredExprType {
    InferredExprTypeKind kind;
    FengSlice builtin_name;
    const FengTypeRef *type_ref;
    const FengDecl *type_decl;
} InferredExprType;

typedef struct LocalNameEntry {
    FengSlice name;
    InferredExprType type;
    FengMutability mutability;
} LocalNameEntry;

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
} FunctionCallResolution;

typedef enum CallableValueResolutionKind {
    FENG_CALLABLE_VALUE_RESOLUTION_NONE = 0,
    FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE,
    FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS
} CallableValueResolutionKind;

typedef struct CallableValueResolution {
    CallableValueResolutionKind kind;
} CallableValueResolution;

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
    ScopeFrame *scopes;
    size_t scope_count;
    size_t scope_capacity;
    const FengDecl *current_type_decl;
    const FengTypeMember *current_callable_member;
    const FengCallableSignature *current_callable_signature;
    FengSlice *current_constructor_bound_names;
    size_t current_constructor_bound_count;
    size_t current_constructor_bound_capacity;
    FengTypeRef **synthetic_type_refs;
    size_t synthetic_type_ref_count;
    size_t synthetic_type_ref_capacity;
    FengSemanticError **errors;
    size_t *error_count;
    size_t *error_capacity;
} ResolveContext;

typedef struct ResolvedTypeTarget {
    const FengDecl *type_decl;
    const FengSemanticModule *provider_module;
} ResolvedTypeTarget;

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
            buffer = (char *)malloc(inner_length + 3U);
            if (buffer == NULL) {
                free(inner_name);
                return NULL;
            }

            memcpy(buffer, inner_name, inner_length);
            buffer[inner_length] = '[';
            buffer[inner_length + 1U] = ']';
            buffer[inner_length + 2U] = '\0';
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
    static const char *builtin_names[] = {
        "i8",   "i16",  "i32",  "i64",  "int",
        "u8",   "u16",  "u32",  "u64",
        "f32",  "f64",  "float",
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
    if (slice_equals_cstr(name, "int") || slice_equals_cstr(name, "i64")) {
        return "i64";
    }
    if (slice_equals_cstr(name, "float") || slice_equals_cstr(name, "f64")) {
        return "f64";
    }
    if (slice_equals_cstr(name, "i8")) {
        return "i8";
    }
    if (slice_equals_cstr(name, "i16")) {
        return "i16";
    }
    if (slice_equals_cstr(name, "i32")) {
        return "i32";
    }
    if (slice_equals_cstr(name, "u8")) {
        return "u8";
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
    if (slice_equals_cstr(name, "f32")) {
        return "f32";
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

static bool module_is_visible_from(const FengSemanticModule *from, const FengSemanticModule *target) {
    return from == target || target->visibility == FENG_VISIBILITY_PUBLIC;
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

static bool resolver_append_error(ResolveContext *context, FengToken token, char *message) {
    return append_error(context->errors,
                        context->error_count,
                        context->error_capacity,
                        context->program->path,
                        token,
                        message);
}

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
                                     FengMutability mutability) {
    ScopeFrame *frame;
    LocalNameEntry entry;

    if (context->scope_count == 0U && !resolver_push_scope(context)) {
        return false;
    }

    frame = &context->scopes[context->scope_count - 1U];
    entry.name = name;
    entry.type = type;
    entry.mutability = normalize_mutability(mutability);
    return append_raw((void **)&frame->locals,
                      &frame->local_count,
                      &frame->local_capacity,
                      sizeof(entry),
                      &entry);
}

static bool resolver_add_local_typed_name(ResolveContext *context,
                                          FengSlice name,
                                          InferredExprType type,
                                          FengMutability mutability) {
    return resolver_add_local_entry(context, name, type, mutability);
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

        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return NULL;
    }

    return NULL;
}

static const FengTypeRef *synthesize_array_type_ref(ResolveContext *context,
                                                    const InferredExprType *element_type,
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
static InferredExprType infer_expr_type(ResolveContext *context, const FengExpr *expr);
static char *format_expr_target_name(const FengExpr *expr);
static char *format_inferred_expr_type_name(InferredExprType type);
static bool expr_matches_expected_type_ref(ResolveContext *context,
                                           const FengExpr *expr,
                                           const FengTypeRef *expected_type_ref);
static ResolvedTypeTarget resolve_type_target_expr(const ResolveContext *context,
                                                   const FengExpr *target_expr,
                                                   bool follow_call_callee);

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
            module_is_visible_from(context->module, &context->analysis->modules[module_index])) {
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
            return expr_type.type_ref != NULL &&
                   type_refs_semantically_equal(context, expr_type.type_ref, type_ref);

        case FENG_INFERRED_EXPR_TYPE_DECL:
            target_decl = resolve_type_ref_decl(context, type_ref);
            return target_decl != NULL && target_decl == expr_type.type_decl;

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

    return false;
}

static const FengDecl *resolve_function_type_decl(const ResolveContext *context,
                                                  const FengTypeRef *type_ref) {
    const FengDecl *type_decl = resolve_type_ref_decl(context, type_ref);

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_FUNCTION) {
        return NULL;
    }

    return type_decl;
}

static bool function_type_decl_return_matches_type_ref(const ResolveContext *context,
                                                       const FengDecl *function_type_decl,
                                                       const FengTypeRef *return_type) {
    const FengTypeRef *expected_return_type;
    bool expected_is_void;
    bool actual_is_void;

    if (function_type_decl == NULL || function_type_decl->kind != FENG_DECL_TYPE ||
        function_type_decl->as.type_decl.form != FENG_TYPE_DECL_FUNCTION) {
        return false;
    }

    expected_return_type = function_type_decl->as.type_decl.as.function.return_type;
    expected_is_void = expected_return_type == NULL || type_ref_is_void(expected_return_type);
    actual_is_void = return_type == NULL || type_ref_is_void(return_type);
    if (expected_is_void || actual_is_void) {
        return expected_is_void && actual_is_void;
    }

    return type_refs_semantically_equal(context, expected_return_type, return_type);
}

static bool function_type_decl_matches_callable_signature(const ResolveContext *context,
                                                          const FengDecl *function_type_decl,
                                                          const FengCallableSignature *callable) {
    size_t param_index;

    if (function_type_decl == NULL || callable == NULL ||
        function_type_decl->kind != FENG_DECL_TYPE ||
        function_type_decl->as.type_decl.form != FENG_TYPE_DECL_FUNCTION) {
        return false;
    }
    if (function_type_decl->as.type_decl.as.function.param_count != callable->param_count) {
        return false;
    }

    for (param_index = 0U;
         param_index < function_type_decl->as.type_decl.as.function.param_count;
         ++param_index) {
        if (!type_refs_semantically_equal(context,
                                          function_type_decl->as.type_decl.as.function.params[param_index].type,
                                          callable->params[param_index].type)) {
            return false;
        }
    }

    return function_type_decl_return_matches_type_ref(context,
                                                      function_type_decl,
                                                      callable->return_type);
}

static const char *inferred_expr_type_builtin_canonical_name(InferredExprType expr_type) {
    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
            return canonical_builtin_type_name(expr_type.builtin_name);

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return type_ref_builtin_canonical_name(expr_type.type_ref);

        case FENG_INFERRED_EXPR_TYPE_DECL:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return NULL;
    }

    return NULL;
}

static bool inferred_expr_type_is_numeric(InferredExprType expr_type) {
    const char *builtin_name = inferred_expr_type_builtin_canonical_name(expr_type);

    return builtin_name != NULL && builtin_type_name_is_numeric(slice_from_cstr(builtin_name));
}

static bool inferred_expr_type_is_string(InferredExprType expr_type) {
    const char *builtin_name = inferred_expr_type_builtin_canonical_name(expr_type);

    return builtin_name != NULL && strcmp(builtin_name, "string") == 0;
}

static const FengDecl *resolve_inferred_expr_type_decl(const ResolveContext *context,
                                                       InferredExprType expr_type) {
    switch (expr_type.kind) {
        case FENG_INFERRED_EXPR_TYPE_DECL:
            return expr_type.type_decl;

        case FENG_INFERRED_EXPR_TYPE_TYPE_REF:
            return resolve_type_ref_decl(context, expr_type.type_ref);

        case FENG_INFERRED_EXPR_TYPE_BUILTIN:
        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return NULL;
    }

    return NULL;
}

static const FengTypeMember *find_type_field_member(const FengDecl *type_decl, FengSlice name) {
    size_t member_index;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return NULL;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

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

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return NULL;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_FIELD && slice_equals(member->as.field.name, name)) {
            return member;
        }
        if (member->kind == FENG_TYPE_MEMBER_METHOD && slice_equals(member->as.callable.name, name)) {
            return member;
        }
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

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return 0U;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        if (type_decl->as.type_decl.as.object.members[member_index]->kind ==
            FENG_TYPE_MEMBER_CONSTRUCTOR) {
            ++count;
        }
    }

    return count;
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

static ConstructorResolution resolve_accessible_constructor_overload(
    ResolveContext *context,
    const FengDecl *type_decl,
    const FengSemanticModule *provider_module,
    FengExpr *const *args,
    size_t arg_count) {
    size_t member_index;
    ConstructorResolution result;

    memset(&result, 0, sizeof(result));

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return result;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_CONSTRUCTOR) {
            continue;
        }
        if (!type_member_is_accessible_from(context, provider_module, member)) {
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

    memset(&result, 0, sizeof(result));
    if (overload_set == NULL || function_type_decl == NULL) {
        return result;
    }

    for (decl_index = 0U; decl_index < overload_set->decl_count; ++decl_index) {
        const FengDecl *decl = overload_set->decls[decl_index];

        if (decl == NULL || decl->kind != FENG_DECL_FUNCTION ||
            !function_type_decl_matches_callable_signature(context,
                                                           function_type_decl,
                                                           &decl->as.function_decl)) {
            continue;
        }

        if (result.kind == FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
            result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            continue;
        }

        result.kind = FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS;
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

    memset(&result, 0, sizeof(result));
    if (module == NULL || function_type_decl == NULL) {
        return result;
    }

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind != FENG_DECL_FUNCTION || !decl_is_public(decl) ||
                !slice_equals(decl->as.function_decl.name, name) ||
                !function_type_decl_matches_callable_signature(context,
                                                               function_type_decl,
                                                               &decl->as.function_decl)) {
                continue;
            }

            if (result.kind == FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
                continue;
            }

            result.kind = FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS;
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

    memset(&result, 0, sizeof(result));
    if (type_decl == NULL || function_type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return result;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_METHOD ||
            !slice_equals(member->as.callable.name, name) ||
            !type_member_is_accessible_from(context, provider_module, member) ||
            !function_type_decl_matches_callable_signature(context,
                                                           function_type_decl,
                                                           &member->as.callable)) {
            continue;
        }

        if (result.kind == FENG_CALLABLE_VALUE_RESOLUTION_NONE) {
            result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            continue;
        }

        result.kind = FENG_CALLABLE_VALUE_RESOLUTION_AMBIGUOUS;
    }

    return result;
}

static bool function_type_parameters_match_args(ResolveContext *context,
                                                const FengDecl *type_decl,
                                                FengExpr *const *args,
                                                size_t arg_count) {
    size_t arg_index;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_FUNCTION) {
        return false;
    }
    if (type_decl->as.type_decl.as.function.param_count != arg_count) {
        return false;
    }

    for (arg_index = 0U; arg_index < arg_count; ++arg_index) {
        if (!expr_matches_expected_type_ref(context,
                                            args[arg_index],
                                            type_decl->as.type_decl.as.function.params[arg_index].type)) {
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

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return NULL;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_METHOD && slice_equals(member->as.callable.name, name)) {
            return member;
        }
    }

    return NULL;
}

static const FengTypeMember *find_accessible_type_field_member(ResolveContext *context,
                                                               const FengDecl *type_decl,
                                                               const FengSemanticModule *provider_module,
                                                               FengSlice name) {
    const FengTypeMember *member = find_type_field_member(type_decl, name);

    return type_member_is_accessible_from(context, provider_module, member) ? member : NULL;
}

static const FengTypeMember *find_accessible_type_method_member(ResolveContext *context,
                                                                const FengDecl *type_decl,
                                                                const FengSemanticModule *provider_module,
                                                                FengSlice name) {
    const FengTypeMember *member = find_type_method_member(type_decl, name);

    return type_member_is_accessible_from(context, provider_module, member) ? member : NULL;
}

static size_t count_accessible_method_overloads(ResolveContext *context,
                                                const FengDecl *type_decl,
                                                const FengSemanticModule *provider_module,
                                                FengSlice name) {
    size_t member_index;
    size_t count = 0U;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return 0U;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

        if (member->kind == FENG_TYPE_MEMBER_METHOD &&
            slice_equals(member->as.callable.name, name) &&
            type_member_is_accessible_from(context, provider_module, member)) {
            ++count;
        }
    }

    return count;
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

    memset(&result, 0, sizeof(result));
    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE ||
        type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return result;
    }

    for (member_index = 0U; member_index < type_decl->as.type_decl.as.object.member_count; ++member_index) {
        const FengTypeMember *member = type_decl->as.type_decl.as.object.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_METHOD ||
            !slice_equals(member->as.callable.name, name) ||
            !type_member_is_accessible_from(context, provider_module, member) ||
            !callable_parameters_match_args(context, &member->as.callable, args, arg_count)) {
            continue;
        }

        if (result.kind == FENG_FUNCTION_CALL_RESOLUTION_NONE) {
            result.kind = FENG_FUNCTION_CALL_RESOLUTION_UNIQUE;
            result.decl = NULL;
            result.callable = &member->as.callable;
            continue;
        }

        result.kind = FENG_FUNCTION_CALL_RESOLUTION_AMBIGUOUS;
        result.callable = NULL;
    }

    return result;
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

static bool validate_callable_typed_expr_call(ResolveContext *context,
                                              const FengExpr *callee,
                                              FengExpr *const *args,
                                              size_t arg_count) {
    InferredExprType callee_type = infer_expr_type(context, callee);
    const FengDecl *callee_type_decl = resolve_inferred_expr_type_decl(context, callee_type);
    char *target_name = NULL;

    if (callee_type_decl != NULL && callee_type_decl->kind == FENG_DECL_TYPE &&
        callee_type_decl->as.type_decl.form == FENG_TYPE_DECL_FUNCTION) {
        if (function_type_parameters_match_args(context, callee_type_decl, args, arg_count)) {
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

static InferredExprType infer_lambda_call_expr_type(ResolveContext *context,
                                                    const FengExpr *callee,
                                                    FengExpr *const *args,
                                                    size_t arg_count) {
    size_t param_index;
    bool ok = true;
    InferredExprType body_type = inferred_expr_type_unknown();

    if (callee == NULL || callee->kind != FENG_EXPR_LAMBDA ||
        callee->as.lambda.param_count != arg_count) {
        return inferred_expr_type_unknown();
    }

    for (param_index = 0U; param_index < arg_count; ++param_index) {
        if (!expr_matches_expected_type_ref(context,
                                            args[param_index],
                                            callee->as.lambda.params[param_index].type)) {
            return inferred_expr_type_unknown();
        }
    }

    if (!resolver_push_scope(context)) {
        return inferred_expr_type_unknown();
    }

    for (param_index = 0U; param_index < callee->as.lambda.param_count && ok; ++param_index) {
        ok = resolver_add_local_typed_name(
            context,
            callee->as.lambda.params[param_index].name,
            inferred_expr_type_from_type_ref(callee->as.lambda.params[param_index].type),
            callee->as.lambda.params[param_index].mutability);
    }
    if (ok) {
        body_type = infer_expr_type(context, callee->as.lambda.body);
    }

    resolver_pop_scope(context);
    return ok ? body_type : inferred_expr_type_unknown();
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
                return inferred_expr_type_from_return_type_ref(resolution.callable->return_type);
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
                    return inferred_expr_type_from_return_type_ref(resolution.callable->return_type);
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
                return inferred_expr_type_from_return_type_ref(resolution.callable->return_type);
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
    if (callee_type_decl != NULL &&
        function_type_parameters_match_args(context,
                                            callee_type_decl,
                                            expr->as.call.args,
                                            expr->as.call.arg_count)) {
        return inferred_expr_type_from_return_type_ref(
            callee_type_decl->as.type_decl.as.function.return_type);
    }

    return inferred_expr_type_unknown();
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

    if (owner_type_decl->kind != FENG_DECL_TYPE ||
        owner_type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        return resolver_append_error(
            context,
            expr->token,
            format_message("type '%.*s' has no member '%.*s'",
                           owner_type_decl->kind == FENG_DECL_TYPE
                               ? (int)owner_type_decl->as.type_decl.name.length
                               : 0,
                           owner_type_decl->kind == FENG_DECL_TYPE
                               ? owner_type_decl->as.type_decl.name.data
                               : "",
                           (int)expr->as.member.member.length,
                           expr->as.member.member.data));
    }

    member = find_instance_member(owner_type_decl, expr->as.member.member);
    if (member == NULL) {
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

        case FENG_STMT_WHILE:
            return collect_constructor_bound_lets_from_block(
                type_decl, stmt->as.while_stmt.body, bound_names, bound_count, bound_capacity);

        case FENG_STMT_FOR:
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

    array_type_ref = synthesize_array_type_ref(context, &element_type, expr->token);
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
                        find_instance_member(owner_type_decl, target->as.member.member);

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
            return resolve_indexed_array_element_type_ref(context, target->as.index.object) != NULL
                       ? true
                       : resolver_append_error(
                             context,
                             target->token,
                             format_message("indexed assignment targets are not supported yet"));

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
            return inferred_expr_type_builtin("float");

        case FENG_EXPR_STRING:
            return inferred_expr_type_builtin("string");

        case FENG_EXPR_ARRAY_LITERAL:
            return infer_array_literal_expr_type(context, expr);

        case FENG_EXPR_LAMBDA:
            return inferred_expr_type_unknown();

        case FENG_EXPR_INDEX: {
            const FengTypeRef *element_type_ref =
                resolve_indexed_array_element_type_ref(context, expr->as.index.object);

            return element_type_ref != NULL ? inferred_expr_type_from_type_ref(element_type_ref)
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
            if (expr->as.unary.op == FENG_TOKEN_NOT) {
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
                case FENG_TOKEN_EQ:
                case FENG_TOKEN_NE:
                case FENG_TOKEN_AND_AND:
                case FENG_TOKEN_OR_OR:
                    return inferred_expr_type_builtin("bool");

                default:
                    return inferred_expr_type_unknown();
            }
        }

        case FENG_EXPR_CAST:
            return inferred_expr_type_from_type_ref(expr->as.cast.type);

        case FENG_EXPR_IF: {
            InferredExprType then_type = infer_expr_type(context, expr->as.if_expr.then_expr);
            InferredExprType else_type = infer_expr_type(context, expr->as.if_expr.else_expr);

            return inferred_expr_types_equal(context, then_type, else_type)
                       ? then_type
                       : inferred_expr_type_unknown();
        }

        case FENG_EXPR_MATCH: {
            InferredExprType result_type = infer_expr_type(context, expr->as.match_expr.else_expr);

            if (!inferred_expr_type_is_known(result_type)) {
                return result_type;
            }

            for (index = 0U; index < expr->as.match_expr.case_count; ++index) {
                InferredExprType case_type =
                    infer_expr_type(context, expr->as.match_expr.cases[index].value);

                if (!inferred_expr_types_equal(context, result_type, case_type)) {
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
    InferredExprType body_type = inferred_expr_type_unknown();

    if (expr == NULL || expr->kind != FENG_EXPR_LAMBDA || function_type_decl == NULL ||
        function_type_decl->kind != FENG_DECL_TYPE ||
        function_type_decl->as.type_decl.form != FENG_TYPE_DECL_FUNCTION) {
        return false;
    }
    if (expr->as.lambda.param_count != function_type_decl->as.type_decl.as.function.param_count) {
        return false;
    }

    for (param_index = 0U; param_index < expr->as.lambda.param_count; ++param_index) {
        if (!type_refs_semantically_equal(context,
                                          expr->as.lambda.params[param_index].type,
                                          function_type_decl->as.type_decl.as.function.params[param_index].type)) {
            return false;
        }
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
    if (ok) {
        body_type = infer_expr_type(context, expr->as.lambda.body);
    }

    resolver_pop_scope(context);
    if (!ok) {
        return false;
    }
    if (!inferred_expr_type_is_known(body_type)) {
        return true;
    }

    return inferred_expr_type_matches_type_ref(context,
                                               body_type,
                                               function_type_decl->as.type_decl.as.function.return_type);
}

static CallableValueResolution resolve_expr_callable_value(ResolveContext *context,
                                                           const FengExpr *expr,
                                                           const FengTypeRef *expected_type_ref) {
    CallableValueResolution result;
    const FengDecl *function_type_decl;

    memset(&result, 0, sizeof(result));
    function_type_decl = resolve_function_type_decl(context, expected_type_ref);
    if (expr == NULL || function_type_decl == NULL) {
        return result;
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER: {
            const LocalNameEntry *local_entry =
                resolver_find_local_name_entry(context, expr->as.identifier);
            const FunctionOverloadSetEntry *overload_set;
            InferredExprType expr_type;

            if (local_entry != NULL) {
                if (inferred_expr_type_matches_type_ref(context, local_entry->type, expected_type_ref)) {
                    result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
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
                if (owner_type_decl != NULL && owner_type_decl->kind == FENG_DECL_TYPE &&
                    owner_type_decl->as.type_decl.form == FENG_TYPE_DECL_OBJECT) {
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
            if (lambda_expr_matches_function_type(context, expr, function_type_decl)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            }
            return result;

        default: {
            InferredExprType expr_type = infer_expr_type(context, expr);

            if (inferred_expr_type_is_known(expr_type) &&
                inferred_expr_type_matches_type_ref(context, expr_type, expected_type_ref)) {
                result.kind = FENG_CALLABLE_VALUE_RESOLUTION_UNIQUE;
            }
            return result;
        }
    }
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

    expr_type = infer_expr_type(context, expr);
    if (!inferred_expr_type_is_known(expr_type)) {
        return true;
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
        return true;
    }

    expr_type = infer_expr_type(context, expr);
    if (!inferred_expr_type_is_known(expr_type)) {
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

        case FENG_INFERRED_EXPR_TYPE_UNKNOWN:
            return duplicate_cstr("<type>");
    }

    return duplicate_cstr("<type>");
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
                                            size_t arg_count) {
    size_t declared_constructor_count;
    ConstructorResolution resolution;

    if (type_decl == NULL) {
        return true;
    }

    if (type_decl->kind != FENG_DECL_TYPE || type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
        bool ok = resolver_append_error(
            context,
            target_expr != NULL ? target_expr->token : context->program->module_token,
            format_message("type '%.*s' is not an object type and cannot be constructed",
                           type_decl->kind == FENG_DECL_TYPE ? (int)type_decl->as.type_decl.name.length : 0,
                           type_decl->kind == FENG_DECL_TYPE ? type_decl->as.type_decl.name.data : ""));

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

    if (target.type_decl == NULL) {
        return true;
    }

    return validate_constructor_invocation(context,
                                           expr->as.call.callee,
                                           target.type_decl,
                                           target.provider_module,
                                           expr->as.call.args,
                                           expr->as.call.arg_count);
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
        if (owner_type_decl != NULL && owner_type_decl->kind == FENG_DECL_TYPE &&
            owner_type_decl->as.type_decl.form == FENG_TYPE_DECL_OBJECT) {
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

    if (target.type_decl == NULL || target.type_decl->kind != FENG_DECL_TYPE ||
        target.type_decl->as.type_decl.form != FENG_TYPE_DECL_OBJECT) {
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
                                         0U)) {
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

        if (!type_member_is_accessible_from(context, target.provider_module, field_member)) {
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

    if (find_instance_member(context->current_type_decl, expr->as.member.member) != NULL) {
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

static bool resolve_lambda_expr(ResolveContext *context, const FengExpr *expr) {
    size_t param_index;
    bool ok;

    for (param_index = 0U; param_index < expr->as.lambda.param_count; ++param_index) {
        if (!resolve_type_ref(context, expr->as.lambda.params[param_index].type, false)) {
            return false;
        }
    }

    if (!resolver_push_scope(context)) {
        return false;
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
        ok = resolve_expr(context, expr->as.lambda.body, false);
    }

    resolver_pop_scope(context);
    return ok;
}

static bool resolve_expr(ResolveContext *context, const FengExpr *expr, bool allow_self) {
    size_t index;

    if (expr == NULL) {
        return true;
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
            if (resolver_has_local_name(context, expr->as.identifier) ||
                find_visible_value(context->visible_values, context->visible_value_count, expr->as.identifier) != NULL ||
                find_visible_type(context->visible_types, context->visible_type_count, expr->as.identifier) != NULL) {
                return true;
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
                   resolve_expr(context, expr->as.index.index, allow_self);

        case FENG_EXPR_UNARY:
            return resolve_expr(context, expr->as.unary.operand, allow_self);

        case FENG_EXPR_BINARY:
            return resolve_expr(context, expr->as.binary.left, allow_self) &&
                   resolve_expr(context, expr->as.binary.right, allow_self);

        case FENG_EXPR_LAMBDA:
            return resolve_lambda_expr(context, expr);

        case FENG_EXPR_CAST:
            return resolve_type_ref(context, expr->as.cast.type, false) &&
                   resolve_expr(context, expr->as.cast.value, allow_self);

        case FENG_EXPR_IF:
            return resolve_expr(context, expr->as.if_expr.condition, allow_self) &&
                   resolve_expr(context, expr->as.if_expr.then_expr, allow_self) &&
                   resolve_expr(context, expr->as.if_expr.else_expr, allow_self);

        case FENG_EXPR_MATCH:
            if (!resolve_expr(context, expr->as.match_expr.target, allow_self)) {
                return false;
            }
            for (index = 0U; index < expr->as.match_expr.case_count; ++index) {
                if (!resolve_expr(context, expr->as.match_expr.cases[index].label, allow_self) ||
                    !resolve_expr(context, expr->as.match_expr.cases[index].value, allow_self)) {
                    return false;
                }
            }
            return resolve_expr(context, expr->as.match_expr.else_expr, allow_self);
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
    } else {
        if (!validate_untyped_callable_value_expr(context, binding->initializer)) {
            return false;
        }
        if (!validate_untyped_array_literal_expr(context, binding->initializer)) {
            return false;
        }
    }
    if (add_to_scope) {
        binding_type = binding->type != NULL ? inferred_expr_type_from_type_ref(binding->type)
                                             : infer_expr_type(context, binding->initializer);
        return resolver_add_local_typed_name(
            context, binding->name, binding_type, binding->mutability);
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
            if (!validate_self_let_assignment(context, stmt)) {
                return false;
            }
            if (!validate_assignment_target_writable(context, stmt->as.assign.target)) {
                return false;
            }
            if (!resolve_expr(context, stmt->as.assign.value, allow_self)) {
                return false;
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
                    !resolve_block(context, stmt->as.if_stmt.clauses[clause_index].block, allow_self)) {
                    return false;
                }
            }
            return resolve_block(context, stmt->as.if_stmt.else_block, allow_self);

        case FENG_STMT_WHILE:
            return resolve_expr(context, stmt->as.while_stmt.condition, allow_self) &&
                   resolve_block(context, stmt->as.while_stmt.body, allow_self);

        case FENG_STMT_FOR: {
            bool ok;

            if (!resolver_push_scope(context)) {
                return false;
            }

            ok = resolve_stmt(context, stmt->as.for_stmt.init, allow_self) &&
                 resolve_expr(context, stmt->as.for_stmt.condition, allow_self) &&
                 resolve_stmt(context, stmt->as.for_stmt.update, allow_self) &&
                 resolve_block(context, stmt->as.for_stmt.body, allow_self);

            resolver_pop_scope(context);
            return ok;
        }

        case FENG_STMT_TRY:
            return resolve_block(context, stmt->as.try_stmt.try_block, allow_self) &&
                   resolve_block(context, stmt->as.try_stmt.catch_block, allow_self) &&
                   resolve_block(context, stmt->as.try_stmt.finally_block, allow_self);

        case FENG_STMT_RETURN:
            if (!resolve_expr(context, stmt->as.return_value, allow_self)) {
                return false;
            }
            return validate_expr_against_expected_type(context,
                                                       stmt->as.return_value,
                                                       context->current_callable_signature != NULL
                                                           ? context->current_callable_signature->return_type
                                                           : NULL);

        case FENG_STMT_THROW:
            return resolve_expr(context, stmt->as.throw_value, allow_self);

        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return true;
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
    bool ok;

    context->current_callable_signature = callable;

    if (!resolve_type_ref(context, callable->return_type, true)) {
        context->current_callable_signature = previous_callable_signature;
        return false;
    }

    for (param_index = 0U; param_index < callable->param_count; ++param_index) {
        if (!resolve_type_ref(context, callable->params[param_index].type, false)) {
            context->current_callable_signature = previous_callable_signature;
            return false;
        }
    }

    if (callable->body == NULL) {
        context->current_callable_signature = previous_callable_signature;
        return true;
    }

    if (is_constructor) {
        resolver_clear_current_constructor_bindings(context);
    }

    if (!resolver_push_scope(context)) {
        if (is_constructor) {
            resolver_clear_current_constructor_bindings(context);
        }
        context->current_callable_signature = previous_callable_signature;
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
    context->current_callable_signature = previous_callable_signature;
    return ok;
}

static bool resolve_declaration(ResolveContext *context, const FengDecl *decl) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return resolve_binding(context, &decl->as.binding, false, false);

        case FENG_DECL_TYPE:
            if (decl->as.type_decl.form == FENG_TYPE_DECL_FUNCTION) {
                if (!resolve_type_ref(context, decl->as.type_decl.as.function.return_type, true)) {
                    return false;
                }
                for (index = 0U; index < decl->as.type_decl.as.function.param_count; ++index) {
                    if (!resolve_type_ref(context,
                                          decl->as.type_decl.as.function.params[index].type,
                                          false)) {
                        return false;
                    }
                }
                return true;
            }

            for (index = 0U; index < decl->as.type_decl.as.object.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.as.object.members[index];
                const FengDecl *previous_type_decl = context->current_type_decl;
                const FengTypeMember *previous_callable_member = context->current_callable_member;

                if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                    if (!resolve_type_ref(context, member->as.field.type, false) ||
                        !resolve_expr(context, member->as.field.initializer, false) ||
                        !validate_expr_against_expected_type(context,
                                                            member->as.field.initializer,
                                                            member->as.field.type)) {
                        return false;
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
                context->current_callable_member = previous_callable_member;
                context->current_type_decl = previous_type_decl;
            }

            return true;

        case FENG_DECL_FUNCTION:
            return resolve_callable(context, &decl->as.function_decl, false);
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
                                  FengSemanticError **errors,
                                  size_t *error_count,
                                  size_t *error_capacity) {
    ResolveContext context;
    AliasEntry *aliases = NULL;
    size_t alias_count = 0U;
    size_t alias_capacity = 0U;
    size_t decl_index;
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
    context.errors = errors;
    context.error_count = error_count;
    context.error_capacity = error_capacity;

    ok = build_program_aliases(analysis, program, &aliases, &alias_count, &alias_capacity);
    if (!ok) {
        free(aliases);
        return false;
    }

    context.aliases = aliases;
    context.alias_count = alias_count;

    for (decl_index = 0U; decl_index < program->declaration_count && ok; ++decl_index) {
        ok = resolve_declaration(&context, program->declarations[decl_index]);
    }

    resolver_free_scopes(&context);
    free(aliases);
    return ok;
}

static bool check_symbol_conflicts(const FengSemanticAnalysis *analysis,
                                   const FengSemanticModule *module,
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
                                   errors,
                                   error_count,
                                   error_capacity);
    }

    free(visible_types);
    free(visible_values);
    free_function_overload_sets(function_sets, function_set_count);
    return ok;
}

bool feng_semantic_analyze(const FengProgram *const *programs,
                           size_t program_count,
                           FengSemanticAnalysis **out_analysis,
                           FengSemanticError **out_errors,
                           size_t *out_error_count) {
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    size_t error_capacity = 0U;
    size_t program_index;
    bool ok = true;

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

    for (program_index = 0U; program_index < analysis->module_count && ok; ++program_index) {
        ok = check_symbol_conflicts(analysis,
                                    &analysis->modules[program_index],
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
        feng_semantic_analysis_free(analysis);
        return false;
    }

    if (out_analysis != NULL) {
        *out_analysis = analysis;
    } else {
        feng_semantic_analysis_free(analysis);
    }
    if (out_errors != NULL) {
        *out_errors = NULL;
    }
    return true;
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
