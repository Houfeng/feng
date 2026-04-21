#include "semantic/semantic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SymbolEntry {
    FengSlice name;
    const FengDecl *decl;
} SymbolEntry;

typedef struct FunctionEntry {
    FengSlice name;
    const FengDecl *decl;
} FunctionEntry;

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

static size_t find_module_index(const FengSemanticAnalysis *analysis, const FengProgram *program) {
    size_t index;

    for (index = 0U; index < analysis->module_count; ++index) {
        const FengSemanticModule *module = &analysis->modules[index];

        if (path_equals(module->segments,
                        module->segment_count,
                        program->module_segments,
                        program->module_segment_count)) {
            return index;
        }
    }

    return analysis->module_count;
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

static bool check_symbol_conflicts(const FengSemanticModule *module,
                                   FengSemanticError **errors,
                                   size_t *error_count,
                                   size_t *error_capacity) {
    SymbolEntry *types = NULL;
    SymbolEntry *bindings = NULL;
    FunctionEntry *functions = NULL;
    size_t type_count = 0U;
    size_t binding_count = 0U;
    size_t function_count = 0U;
    size_t type_capacity = 0U;
    size_t binding_capacity = 0U;
    size_t function_capacity = 0U;
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
                    SymbolEntry entry;

                    for (index = 0U; index < type_count; ++index) {
                        if (slice_equals(types[index].name, decl->as.type_decl.name)) {
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
                    }
                    if (!ok) {
                        break;
                    }
                    if (index < type_count) {
                        break;
                    }

                    entry.name = decl->as.type_decl.name;
                    entry.decl = decl;
                    ok = append_raw((void **)&types,
                                    &type_count,
                                    &type_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_GLOBAL_BINDING: {
                    SymbolEntry entry;

                    for (index = 0U; index < binding_count; ++index) {
                        if (slice_equals(bindings[index].name, decl->as.binding.name)) {
                            char *message = format_message("duplicate top-level binding '%.*s'",
                                                           (int)decl->as.binding.name.length,
                                                           decl->as.binding.name.data);

                            ok = append_error(errors,
                                              error_count,
                                              error_capacity,
                                              program->path,
                                              decl->as.binding.token,
                                              message);
                            break;
                        }
                    }
                    if (!ok) {
                        break;
                    }
                    if (index < binding_count) {
                        break;
                    }

                    entry.name = decl->as.binding.name;
                    entry.decl = decl;
                    ok = append_raw((void **)&bindings,
                                    &binding_count,
                                    &binding_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }

                case FENG_DECL_FUNCTION: {
                    FunctionEntry entry;

                    for (index = 0U; index < function_count; ++index) {
                        const FengCallableSignature *existing = &functions[index].decl->as.function_decl;

                        if (!slice_equals(functions[index].name, decl->as.function_decl.name)) {
                            continue;
                        }
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
                    if (index < function_count) {
                        break;
                    }

                    entry.name = decl->as.function_decl.name;
                    entry.decl = decl;
                    ok = append_raw((void **)&functions,
                                    &function_count,
                                    &function_capacity,
                                    sizeof(entry),
                                    &entry);
                    break;
                }
            }
        }
    }

    free(types);
    free(bindings);
    free(functions);
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
        ok = check_symbol_conflicts(&analysis->modules[program_index],
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
