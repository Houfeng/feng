#include "symbol/export.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "symbol/ft.h"
#include "symbol/internal.h"

typedef struct DeclSourceMap {
    const void *source;
    FengSymbolDeclView *decl;
} DeclSourceMap;

static FengMutability normalize_mutability(FengMutability mutability) {
    return mutability == FENG_MUTABILITY_VAR ? FENG_MUTABILITY_VAR : FENG_MUTABILITY_LET;
}

static bool visibility_is_public(FengVisibility visibility) {
    return visibility == FENG_VISIBILITY_PUBLIC;
}

static bool annotation_kind_is_calling_convention(FengAnnotationKind kind) {
    return kind == FENG_ANNOTATION_CDECL || kind == FENG_ANNOTATION_STDCALL ||
           kind == FENG_ANNOTATION_FASTCALL;
}

static bool annotations_contain_kind(const FengAnnotation *annotations,
                                     size_t annotation_count,
                                     FengAnnotationKind kind) {
    size_t index;

    for (index = 0U; index < annotation_count; ++index) {
        if (annotations[index].builtin_kind == kind) {
            return true;
        }
    }

    return false;
}

static const FengAnnotation *find_calling_convention_annotation(const FengAnnotation *annotations,
                                                                size_t annotation_count) {
    size_t index;

    for (index = 0U; index < annotation_count; ++index) {
        if (annotation_kind_is_calling_convention(annotations[index].builtin_kind)) {
            return &annotations[index];
        }
    }

    return NULL;
}

static const char *canonical_builtin_name(FengSlice name) {
    static const struct {
        const char *alias;
        const char *canonical;
    } table[] = {
        {"i8", "i8"},
        {"i16", "i16"},
        {"i32", "i32"},
        {"int", "i32"},
        {"i64", "i64"},
        {"long", "i64"},
        {"u8", "u8"},
        {"byte", "u8"},
        {"u16", "u16"},
        {"u32", "u32"},
        {"u64", "u64"},
        {"f32", "f32"},
        {"float", "f32"},
        {"f64", "f64"},
        {"double", "f64"},
        {"bool", "bool"},
        {"string", "string"},
        {"void", "void"},
    };
    size_t index;

    for (index = 0U; index < sizeof(table) / sizeof(table[0]); ++index) {
        if (strlen(table[index].alias) == name.length &&
            memcmp(table[index].alias, name.data, name.length) == 0) {
            return table[index].canonical;
        }
    }

    return NULL;
}

static int mkdirs(const char *path) {
    size_t length;
    char *buffer;
    size_t index;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    length = strlen(path);
    buffer = (char *)malloc(length + 1U);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(buffer, path, length + 1U);

    for (index = 1U; index < length; ++index) {
        if (buffer[index] == '/') {
            buffer[index] = '\0';
            if (mkdir(buffer, 0775) != 0 && errno != EEXIST) {
                int saved = errno;
                free(buffer);
                errno = saved;
                return -1;
            }
            buffer[index] = '/';
        }
    }

    if (mkdir(buffer, 0775) != 0 && errno != EEXIST) {
        int saved = errno;
        free(buffer);
        errno = saved;
        return -1;
    }

    free(buffer);
    return 0;
}

static char *path_join(const char *lhs, const char *rhs) {
    size_t lhs_length = strlen(lhs);
    size_t rhs_length = strlen(rhs);
    bool need_sep = lhs_length > 0U && lhs[lhs_length - 1U] != '/';
    char *out = (char *)malloc(lhs_length + (need_sep ? 1U : 0U) + rhs_length + 1U);
    size_t cursor = 0U;

    if (out == NULL) {
        return NULL;
    }
    memcpy(out + cursor, lhs, lhs_length);
    cursor += lhs_length;
    if (need_sep) {
        out[cursor++] = '/';
    }
    memcpy(out + cursor, rhs, rhs_length);
    cursor += rhs_length;
    out[cursor] = '\0';
    return out;
}

static char *path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t length;
    char *out;

    if (slash == NULL) {
        return feng_symbol_internal_dup_cstr(".");
    }
    length = (size_t)(slash - path);
    if (length == 0U) {
        return feng_symbol_internal_dup_cstr("/");
    }

    out = (char *)malloc(length + 1U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, path, length);
    out[length] = '\0';
    return out;
}

static bool ensure_parent_dir(const char *path, FengSymbolError *out_error) {
    char *dir = path_dirname_dup(path);
    bool ok;

    if (dir == NULL) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory preparing parent directory");
    }

    ok = mkdirs(dir) == 0;
    if (!ok) {
        feng_symbol_internal_set_error(out_error,
                                       path,
                                       (FengToken){0},
                                       "failed to create directory '%s': %s",
                                       dir,
                                       strerror(errno));
    }
    free(dir);
    return ok;
}

static bool append_decl_pointer(FengSymbolDeclView ***items,
                                size_t *count,
                                FengSymbolDeclView *item,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    FengSymbolDeclView **grown = (FengSymbolDeclView **)realloc(*items,
                                                                (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing declaration list");
    }
    *items = grown;
    (*items)[(*count)++] = item;
    return true;
}

static bool append_type_pointer(FengSymbolTypeView ***items,
                                size_t *count,
                                FengSymbolTypeView *item,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    FengSymbolTypeView **grown = (FengSymbolTypeView **)realloc(*items,
                                                                (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing type list");
    }
    *items = grown;
    (*items)[(*count)++] = item;
    return true;
}

static bool append_param(FengSymbolParamView **items,
                         size_t *count,
                         FengSymbolParamView param,
                         const char *path,
                         FengToken token,
                         FengSymbolError *out_error) {
    FengSymbolParamView *grown = (FengSymbolParamView *)realloc(*items,
                                                                (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing parameter list");
    }
    *items = grown;
    (*items)[(*count)++] = param;
    return true;
}

static bool append_relation(FengSymbolRelation **items,
                            size_t *count,
                            FengSymbolRelation relation,
                            const char *path,
                            FengToken token,
                            FengSymbolError *out_error) {
    FengSymbolRelation *grown = (FengSymbolRelation *)realloc(*items,
                                                              (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing relation list");
    }
    *items = grown;
    (*items)[(*count)++] = relation;
    return true;
}

static bool append_source_map(DeclSourceMap **items,
                              size_t *count,
                              const void *source,
                              FengSymbolDeclView *decl,
                              const char *path,
                              FengToken token,
                              FengSymbolError *out_error) {
    DeclSourceMap *grown = (DeclSourceMap *)realloc(*items, (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing declaration source map");
    }
    *items = grown;
    (*items)[*count].source = source;
    (*items)[*count].decl = decl;
    ++(*count);
    return true;
}

static FengSymbolDeclView *find_source_decl(const DeclSourceMap *items,
                                            size_t count,
                                            const void *source) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (items[index].source == source) {
            return items[index].decl;
        }
    }
    return NULL;
}

static bool append_unique_string(char ***items,
                                 size_t *count,
                                 const char *value,
                                 const char *path,
                                 FengToken token,
                                 FengSymbolError *out_error) {
    size_t index;
    char **grown;

    for (index = 0U; index < *count; ++index) {
        if (strcmp((*items)[index], value) == 0) {
            return true;
        }
    }

    grown = (char **)realloc(*items, (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing string list");
    }
    *items = grown;
    (*items)[*count] = feng_symbol_internal_dup_cstr(value);
    if ((*items)[*count] == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning string entry");
    }
    ++(*count);
    return true;
}

static char *join_segments(const FengSlice *segments, size_t segment_count) {
    size_t total = 0U;
    size_t index;
    char *out;
    size_t cursor = 0U;

    if (segment_count == 0U) {
        return feng_symbol_internal_dup_cstr("");
    }

    for (index = 0U; index < segment_count; ++index) {
        total += segments[index].length;
    }
    total += segment_count > 0U ? segment_count - 1U : 0U;

    out = (char *)malloc(total + 1U);
    if (out == NULL) {
        return NULL;
    }

    for (index = 0U; index < segment_count; ++index) {
        if (index > 0U) {
            out[cursor++] = '.';
        }
        memcpy(out + cursor, segments[index].data, segments[index].length);
        cursor += segments[index].length;
    }
    out[cursor] = '\0';
    return out;
}

static FengSymbolTypeView *new_type(FengSymbolTypeKind kind,
                                    const char *path,
                                    FengToken token,
                                    FengSymbolError *out_error) {
    FengSymbolTypeView *type = (FengSymbolTypeView *)calloc(1U, sizeof(*type));

    if (type == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory allocating type node");
        return NULL;
    }
    type->kind = kind;
    return type;
}

static FengSymbolDeclView *new_decl(FengSymbolDeclKind kind,
                                    FengVisibility visibility,
                                    FengMutability mutability,
                                    const char *name,
                                    const char *path,
                                    FengToken token,
                                    FengSymbolError *out_error) {
    FengSymbolDeclView *decl = (FengSymbolDeclView *)calloc(1U, sizeof(*decl));

    if (decl == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory allocating declaration node");
        return NULL;
    }

    decl->kind = kind;
    decl->visibility = visibility;
    decl->mutability = normalize_mutability(mutability);
    decl->token = token;
    decl->name = feng_symbol_internal_dup_cstr(name);
    decl->path = feng_symbol_internal_dup_cstr(path);
    if ((name != NULL && decl->name == NULL) || (path != NULL && decl->path == NULL)) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning declaration metadata");
        feng_symbol_internal_decl_free_members(decl);
        free(decl);
        return NULL;
    }
    return decl;
}

static const FengSemanticModule *find_decl_owner_module(const FengSemanticAnalysis *analysis,
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

static FengSymbolTypeView *build_named_type_from_decl(const FengSemanticAnalysis *analysis,
                                                      const FengDecl *decl,
                                                      const char *path,
                                                      FengToken token,
                                                      FengSymbolError *out_error) {
    const FengSemanticModule *module = find_decl_owner_module(analysis, decl);
    size_t segment_count;
    FengSymbolTypeView *type;
    size_t index;

    if (module == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "could not locate owner module for inferred declaration type");
        return NULL;
    }

    segment_count = module->segment_count + 1U;
    type = new_type(FENG_SYMBOL_TYPE_KIND_NAMED, path, token, out_error);
    if (type == NULL) {
        return NULL;
    }
    type->as.named.segment_count = segment_count;
    type->as.named.segments = (char **)calloc(segment_count, sizeof(*type->as.named.segments));
    if (type->as.named.segments == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory building inferred named type");
        feng_symbol_internal_type_free(type);
        return NULL;
    }

    for (index = 0U; index < module->segment_count; ++index) {
        type->as.named.segments[index] = feng_symbol_internal_dup_slice(module->segments[index]);
        if (type->as.named.segments[index] == NULL) {
            feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning module segment for inferred type");
            feng_symbol_internal_type_free(type);
            return NULL;
        }
    }

    if (decl->kind == FENG_DECL_TYPE) {
        type->as.named.segments[module->segment_count] =
            feng_symbol_internal_dup_slice(decl->as.type_decl.name);
    } else if (decl->kind == FENG_DECL_SPEC) {
        type->as.named.segments[module->segment_count] =
            feng_symbol_internal_dup_slice(decl->as.spec_decl.name);
    }
    if (type->as.named.segments[module->segment_count] == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning inferred declaration name");
        feng_symbol_internal_type_free(type);
        return NULL;
    }

    return type;
}

static FengSymbolTypeView *build_type_from_type_ref(const FengTypeRef *type_ref,
                                                    const char *path,
                                                    FengToken token,
                                                    FengSymbolError *out_error) {
    FengSymbolTypeView *type;
    size_t index;

    if (type_ref == NULL) {
        return NULL;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED: {
            const char *builtin_name = NULL;

            if (type_ref->as.named.segment_count == 1U) {
                builtin_name = canonical_builtin_name(type_ref->as.named.segments[0]);
            }
            if (builtin_name != NULL) {
                type = new_type(FENG_SYMBOL_TYPE_KIND_BUILTIN, path, token, out_error);
                if (type == NULL) {
                    return NULL;
                }
                type->as.builtin.name = feng_symbol_internal_dup_cstr(builtin_name);
                if (type->as.builtin.name == NULL) {
                    feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning builtin name");
                    feng_symbol_internal_type_free(type);
                    return NULL;
                }
                return type;
            }

            type = new_type(FENG_SYMBOL_TYPE_KIND_NAMED, path, token, out_error);
            if (type == NULL) {
                return NULL;
            }
            type->as.named.segment_count = type_ref->as.named.segment_count;
            type->as.named.segments = (char **)calloc(type->as.named.segment_count,
                                                      sizeof(*type->as.named.segments));
            if (type->as.named.segments == NULL) {
                feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning named type segments");
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            for (index = 0U; index < type->as.named.segment_count; ++index) {
                type->as.named.segments[index] =
                    feng_symbol_internal_dup_slice(type_ref->as.named.segments[index]);
                if (type->as.named.segments[index] == NULL) {
                    feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning named type segment");
                    feng_symbol_internal_type_free(type);
                    return NULL;
                }
            }
            return type;
        }

        case FENG_TYPE_REF_POINTER:
            type = new_type(FENG_SYMBOL_TYPE_KIND_POINTER, path, token, out_error);
            if (type == NULL) {
                return NULL;
            }
            type->as.pointer.inner = build_type_from_type_ref(type_ref->as.inner,
                                                              path,
                                                              token,
                                                              out_error);
            if (type_ref->as.inner != NULL && type->as.pointer.inner == NULL) {
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            return type;

        case FENG_TYPE_REF_ARRAY: {
            const FengTypeRef *cursor = type_ref;
            size_t rank = 0U;
            size_t array_index = 0U;

            while (cursor != NULL && cursor->kind == FENG_TYPE_REF_ARRAY) {
                ++rank;
                cursor = cursor->as.inner;
            }

            type = new_type(FENG_SYMBOL_TYPE_KIND_ARRAY, path, token, out_error);
            if (type == NULL) {
                return NULL;
            }
            type->as.array.rank = rank;
            type->as.array.layer_writable = (bool *)calloc(rank, sizeof(*type->as.array.layer_writable));
            if (type->as.array.layer_writable == NULL) {
                feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning array mutability bitmap");
                feng_symbol_internal_type_free(type);
                return NULL;
            }

            cursor = type_ref;
            while (cursor != NULL && cursor->kind == FENG_TYPE_REF_ARRAY) {
                type->as.array.layer_writable[array_index++] = cursor->array_element_writable;
                cursor = cursor->as.inner;
            }
            type->as.array.element = build_type_from_type_ref(cursor, path, token, out_error);
            if (cursor != NULL && type->as.array.element == NULL) {
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            return type;
        }
    }

    return NULL;
}

static FengSymbolTypeView *build_type_from_fact(const FengSemanticAnalysis *analysis,
                                                const FengSemanticTypeFact *fact,
                                                const char *path,
                                                FengToken token,
                                                FengSymbolError *out_error) {
    FengSymbolTypeView *type;

    if (fact == NULL) {
        return NULL;
    }

    switch (fact->kind) {
        case FENG_SEMANTIC_TYPE_FACT_BUILTIN:
            type = new_type(FENG_SYMBOL_TYPE_KIND_BUILTIN, path, token, out_error);
            if (type == NULL) {
                return NULL;
            }
            type->as.builtin.name = feng_symbol_internal_dup_slice(fact->builtin_name);
            if (fact->builtin_name.data != NULL && type->as.builtin.name == NULL) {
                feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning inferred builtin name");
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            return type;

        case FENG_SEMANTIC_TYPE_FACT_TYPE_REF:
            return build_type_from_type_ref(fact->type_ref, path, token, out_error);

        case FENG_SEMANTIC_TYPE_FACT_DECL:
            return build_named_type_from_decl(analysis, fact->type_decl, path, token, out_error);

        case FENG_SEMANTIC_TYPE_FACT_UNKNOWN:
        default:
            break;
    }

    return NULL;
}

static bool fill_declared_specs(FengSymbolDeclView *decl,
                                const FengTypeRef *const *specs,
                                size_t spec_count,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < spec_count; ++index) {
        FengSymbolTypeView *type = build_type_from_type_ref(specs[index], path, token, out_error);
        if (specs[index] != NULL && type == NULL) {
            return false;
        }
        if (!append_type_pointer(&decl->declared_specs,
                                 &decl->declared_spec_count,
                                 type,
                                 path,
                                 token,
                                 out_error)) {
            feng_symbol_internal_type_free(type);
            return false;
        }
    }

    return true;
}

static bool fill_params(FengSymbolDeclView *decl,
                        const FengParameter *params,
                        size_t param_count,
                        const char *path,
                        FengToken token,
                        FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < param_count; ++index) {
        FengSymbolParamView param = {0};

        param.token = params[index].token;
        param.mutability = normalize_mutability(params[index].mutability);
        param.name = feng_symbol_internal_dup_slice(params[index].name);
        param.type = build_type_from_type_ref(params[index].type, path, token, out_error);
        if ((params[index].name.data != NULL && param.name == NULL) ||
            (params[index].type != NULL && param.type == NULL)) {
            free(param.name);
            feng_symbol_internal_type_free(param.type);
            return feng_symbol_internal_set_error(out_error, path, token, "out of memory building parameter signature");
        }

        if (!append_param(&decl->params,
                          &decl->param_count,
                          param,
                          path,
                          token,
                          out_error)) {
            free(param.name);
            feng_symbol_internal_type_free(param.type);
            return false;
        }
    }

    return true;
}

static const FengDecl *find_module_global_string_binding(const FengSemanticModule *module,
                                                         FengSlice name) {
    size_t program_index;

    if (module == NULL) {
        return NULL;
    }

    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind != FENG_DECL_GLOBAL_BINDING ||
                !feng_symbol_internal_slice_equals(decl->as.binding.name, name) ||
                normalize_mutability(decl->as.binding.mutability) != FENG_MUTABILITY_LET ||
                decl->as.binding.initializer == NULL ||
                decl->as.binding.initializer->kind != FENG_EXPR_STRING) {
                continue;
            }

            return decl;
        }
    }

    return NULL;
}

static char *resolve_abi_library(const FengSemanticModule *module,
                                 const FengAnnotation *annotation,
                                 const char *path,
                                 FengToken token,
                                 FengSymbolError *out_error) {
    const FengExpr *arg;
    const FengDecl *binding_decl;

    if (annotation == NULL || annotation->arg_count == 0U) {
        return NULL;
    }

    arg = annotation->args[0];
    if (arg == NULL) {
        return NULL;
    }
    if (arg->kind == FENG_EXPR_STRING) {
        return feng_symbol_internal_dup_slice(arg->as.string);
    }
    if (arg->kind == FENG_EXPR_IDENTIFIER) {
        binding_decl = find_module_global_string_binding(module, arg->as.identifier);
        if (binding_decl != NULL) {
            return feng_symbol_internal_dup_slice(binding_decl->as.binding.initializer->as.string);
        }
    }

    feng_symbol_internal_set_error(out_error,
                                   path,
                                   token,
                                   "extern callable annotation library must resolve to a string literal");
    return NULL;
}

static bool field_is_bounded_decl(const FengTypeMember *member) {
    return member != NULL && member->kind == FENG_TYPE_MEMBER_FIELD &&
           normalize_mutability(member->as.field.mutability) == FENG_MUTABILITY_LET &&
           (member->as.field.initializer != NULL ||
            annotations_contain_kind(member->annotations,
                                     member->annotation_count,
                                     FENG_ANNOTATION_BOUNDED));
}

typedef struct BuildContext {
    const FengSemanticAnalysis *analysis;
    const FengSemanticModule *module;
    FengSymbolModuleGraph *graph;
    DeclSourceMap *source_map;
    size_t source_count;
} BuildContext;

static FengSymbolDeclView *new_decl_from_slice(FengSymbolDeclKind kind,
                                               FengVisibility visibility,
                                               FengMutability mutability,
                                               FengSlice name,
                                               const char *path,
                                               FengToken token,
                                               FengSymbolError *out_error) {
    char *name_copy = feng_symbol_internal_dup_slice(name);
    FengSymbolDeclView *decl;

    if (name.data != NULL && name_copy == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning declaration name");
        return NULL;
    }
    decl = new_decl(kind, visibility, mutability, name_copy, path, token, out_error);
    free(name_copy);
    return decl;
}

static bool register_source_decl(BuildContext *ctx,
                                 const void *source,
                                 FengSymbolDeclView *decl,
                                 const char *path,
                                 FengToken token,
                                 FengSymbolError *out_error) {
    return append_source_map(&ctx->source_map,
                             &ctx->source_count,
                             source,
                             decl,
                             path,
                             token,
                             out_error);
}

static bool append_member_decl(FengSymbolDeclView *owner,
                               FengSymbolDeclView *member,
                               const char *path,
                               FengToken token,
                               FengSymbolError *out_error) {
    member->owner = owner;
    return append_decl_pointer(&owner->members, &owner->member_count, member, path, token, out_error);
}

static bool apply_decl_annotations(FengSymbolDeclView *decl,
                                   const FengSemanticModule *module,
                                   const FengAnnotation *annotations,
                                   size_t annotation_count,
                                   bool allow_library,
                                   const char *path,
                                   FengToken token,
                                   FengSymbolError *out_error) {
    const FengAnnotation *callconv = find_calling_convention_annotation(annotations, annotation_count);

    decl->fixed_annotated = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_FIXED);
    decl->union_annotated = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_UNION);
    if (callconv != NULL) {
        decl->calling_convention = callconv->builtin_kind;
        if (allow_library && callconv->arg_count > 0U) {
            decl->abi_library = resolve_abi_library(module, callconv, path, token, out_error);
            if (decl->abi_library == NULL) {
                return false;
            }
        }
    }
    return true;
}

static FengSymbolTypeView *build_site_type(const FengSemanticAnalysis *analysis,
                                           const void *site,
                                           const FengTypeRef *explicit_type,
                                           const char *path,
                                           FengToken token,
                                           FengSymbolError *out_error) {
    const FengSemanticTypeFact *fact;

    if (explicit_type != NULL) {
        return build_type_from_type_ref(explicit_type, path, token, out_error);
    }

    fact = feng_semantic_lookup_type_fact(analysis, site);
    if (fact == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "missing semantic type fact for exported declaration");
        return NULL;
    }

    return build_type_from_fact(analysis, fact, path, token, out_error);
}

static FengSymbolTypeView *build_callable_return_type(const FengSemanticAnalysis *analysis,
                                                      const void *site,
                                                      const FengTypeRef *explicit_type,
                                                      bool fallback_void,
                                                      const char *path,
                                                      FengToken token,
                                                      FengSymbolError *out_error) {
    FengSymbolTypeView *type = build_site_type(analysis, site, explicit_type, path, token, out_error);

    if (type != NULL || explicit_type != NULL || !fallback_void) {
        return type;
    }

    type = new_type(FENG_SYMBOL_TYPE_KIND_BUILTIN, path, token, out_error);
    if (type == NULL) {
        return NULL;
    }
    type->as.builtin.name = feng_symbol_internal_dup_cstr("void");
    if (type->as.builtin.name == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning builtin name");
        feng_symbol_internal_type_free(type);
        return NULL;
    }
    return type;
}

static bool cstr_equals_slice(const char *text, FengSlice slice) {
    return text != NULL && strlen(text) == slice.length && memcmp(text, slice.data, slice.length) == 0;
}

static FengSymbolDeclView *find_member_decl_by_name(const FengSymbolDeclView *owner, FengSlice name) {
    size_t index;

    if (owner == NULL) {
        return NULL;
    }
    for (index = 0U; index < owner->member_count; ++index) {
        if (cstr_equals_slice(owner->members[index]->name, name)) {
            return owner->members[index];
        }
    }
    return NULL;
}

static bool named_type_targets_current_module(const BuildContext *ctx, const FengTypeRef *type_ref) {
    size_t index;

    if (ctx == NULL || type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count == 0U) {
        return false;
    }
    if (type_ref->as.named.segment_count == 1U) {
        return true;
    }
    if (type_ref->as.named.segment_count != ctx->module->segment_count + 1U) {
        return false;
    }
    for (index = 0U; index < ctx->module->segment_count; ++index) {
        if (!feng_symbol_internal_slice_equals(type_ref->as.named.segments[index],
                                               ctx->module->segments[index])) {
            return false;
        }
    }
    return true;
}

static FengSlice named_type_leaf_name(const FengTypeRef *type_ref) {
    FengSlice empty = {0};

    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count == 0U) {
        return empty;
    }
    return type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];
}

static FengSymbolDeclView *find_local_type_like_decl(const BuildContext *ctx,
                                                     const FengTypeRef *type_ref) {
    size_t index;
    FengSlice name;

    if (!named_type_targets_current_module(ctx, type_ref)) {
        return NULL;
    }
    name = named_type_leaf_name(type_ref);
    for (index = 0U; index < ctx->graph->root_decl.member_count; ++index) {
        FengSymbolDeclView *decl = ctx->graph->root_decl.members[index];

        if ((decl->kind == FENG_SYMBOL_DECL_KIND_TYPE || decl->kind == FENG_SYMBOL_DECL_KIND_SPEC) &&
            cstr_equals_slice(decl->name, name)) {
            return decl;
        }
    }
    return NULL;
}

static bool relation_exists(const FengSymbolModuleGraph *graph,
                            FengSymbolRelationKind kind,
                            const FengSymbolDeclView *left,
                            const FengSymbolDeclView *right,
                            const FengSymbolDeclView *owner) {
    size_t index;

    for (index = 0U; index < graph->relation_count; ++index) {
        const FengSymbolRelation *relation = &graph->relations[index];
        if (relation->kind == kind && relation->left == left && relation->right == right &&
            relation->owner == owner) {
            return true;
        }
    }
    return false;
}

static bool append_unique_relation(BuildContext *ctx,
                                   FengSymbolRelationKind kind,
                                   FengSymbolDeclView *left,
                                   FengSymbolDeclView *right,
                                   FengSymbolDeclView *owner,
                                   FengToken token,
                                   FengSymbolError *out_error) {
    FengSymbolRelation relation;

    if (left == NULL || right == NULL) {
        return true;
    }
    if (relation_exists(ctx->graph, kind, left, right, owner)) {
        return true;
    }
    relation.kind = kind;
    relation.left = left;
    relation.right = right;
    relation.owner = owner;
    return append_relation(&ctx->graph->relations,
                           &ctx->graph->relation_count,
                           relation,
                           ctx->graph->primary_path,
                           token,
                           out_error);
}

static bool append_unique_slice(FengSlice **items,
                                size_t *count,
                                FengSlice value,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    size_t index;
    FengSlice *grown;

    for (index = 0U; index < *count; ++index) {
        if (feng_symbol_internal_slice_equals((*items)[index], value)) {
            return true;
        }
    }
    grown = (FengSlice *)realloc(*items, (*count + 1U) * sizeof(**items));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing slice list");
    }
    *items = grown;
    (*items)[(*count)++] = value;
    return true;
}

static bool collect_ctor_bound_names_from_stmt(const FengDecl *type_decl,
                                               const FengStmt *stmt,
                                               FengSlice **bound_names,
                                               size_t *bound_count,
                                               const char *path,
                                               FengSymbolError *out_error);

static bool collect_ctor_bound_names_from_block(const FengDecl *type_decl,
                                                const FengBlock *block,
                                                FengSlice **bound_names,
                                                size_t *bound_count,
                                                const char *path,
                                                FengSymbolError *out_error) {
    size_t index;

    if (block == NULL) {
        return true;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (!collect_ctor_bound_names_from_stmt(type_decl,
                                                block->statements[index],
                                                bound_names,
                                                bound_count,
                                                path,
                                                out_error)) {
            return false;
        }
    }
    return true;
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

static const FengTypeMember *find_type_field_member(const FengDecl *type_decl, FengSlice name) {
    size_t index;

    if (type_decl == NULL || type_decl->kind != FENG_DECL_TYPE) {
        return NULL;
    }
    for (index = 0U; index < type_decl->as.type_decl.member_count; ++index) {
        const FengTypeMember *member = type_decl->as.type_decl.members[index];
        if (member->kind == FENG_TYPE_MEMBER_FIELD &&
            feng_symbol_internal_slice_equals(member->as.field.name, name)) {
            return member;
        }
    }
    return NULL;
}

static bool collect_ctor_bound_names_from_stmt(const FengDecl *type_decl,
                                               const FengStmt *stmt,
                                               FengSlice **bound_names,
                                               size_t *bound_count,
                                               const char *path,
                                               FengSymbolError *out_error) {
    size_t index;
    FengSlice name;
    const FengTypeMember *field_member;

    if (stmt == NULL) {
        return true;
    }

    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.block,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error);

        case FENG_STMT_ASSIGN:
            if (expr_is_direct_self_member(stmt->as.assign.target, &name)) {
                field_member = find_type_field_member(type_decl, name);
                if (field_member != NULL &&
                    normalize_mutability(field_member->as.field.mutability) == FENG_MUTABILITY_LET) {
                    return append_unique_slice(bound_names,
                                               bound_count,
                                               name,
                                               path,
                                               stmt->token,
                                               out_error);
                }
            }
            return true;

        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (!collect_ctor_bound_names_from_block(type_decl,
                                                         stmt->as.if_stmt.clauses[index].block,
                                                         bound_names,
                                                         bound_count,
                                                         path,
                                                         out_error)) {
                    return false;
                }
            }
            return collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.if_stmt.else_block,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error);

        case FENG_STMT_MATCH:
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (!collect_ctor_bound_names_from_block(type_decl,
                                                         stmt->as.match_stmt.branches[index].body,
                                                         bound_names,
                                                         bound_count,
                                                         path,
                                                         out_error)) {
                    return false;
                }
            }
            return collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.match_stmt.else_block,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error);

        case FENG_STMT_WHILE:
            return collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.while_stmt.body,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error);

        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                return collect_ctor_bound_names_from_block(type_decl,
                                                           stmt->as.for_stmt.body,
                                                           bound_names,
                                                           bound_count,
                                                           path,
                                                           out_error);
            }
            return collect_ctor_bound_names_from_stmt(type_decl,
                                                      stmt->as.for_stmt.init,
                                                      bound_names,
                                                      bound_count,
                                                      path,
                                                      out_error) &&
                   collect_ctor_bound_names_from_stmt(type_decl,
                                                      stmt->as.for_stmt.update,
                                                      bound_names,
                                                      bound_count,
                                                      path,
                                                      out_error) &&
                   collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.for_stmt.body,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error);

        case FENG_STMT_TRY:
            return collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.try_stmt.try_block,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error) &&
                   collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.try_stmt.catch_block,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error) &&
                   collect_ctor_bound_names_from_block(type_decl,
                                                       stmt->as.try_stmt.finally_block,
                                                       bound_names,
                                                       bound_count,
                                                       path,
                                                       out_error);

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

static bool collect_constructor_bound_names(const FengDecl *type_decl,
                                            const FengTypeMember *constructor,
                                            FengSlice **out_names,
                                            size_t *out_count,
                                            const char *path,
                                            FengSymbolError *out_error) {
    size_t annotation_index;

    *out_names = NULL;
    *out_count = 0U;

    for (annotation_index = 0U; annotation_index < constructor->annotation_count; ++annotation_index) {
        const FengAnnotation *annotation = &constructor->annotations[annotation_index];
        size_t arg_index;

        if (annotation->builtin_kind != FENG_ANNOTATION_BOUNDED || annotation->arg_count == 0U) {
            continue;
        }
        for (arg_index = 0U; arg_index < annotation->arg_count; ++arg_index) {
            const FengExpr *arg = annotation->args[arg_index];
            if (arg != NULL && arg->kind == FENG_EXPR_IDENTIFIER &&
                !append_unique_slice(out_names,
                                     out_count,
                                     arg->as.identifier,
                                     path,
                                     constructor->token,
                                     out_error)) {
                return false;
            }
        }
        return true;
    }

    return collect_ctor_bound_names_from_block(type_decl,
                                               constructor->as.callable.body,
                                               out_names,
                                               out_count,
                                               path,
                                               out_error);
}

static FengSymbolDeclView *build_member_decl(BuildContext *ctx,
                                             const char *path,
                                             const FengDecl *owner_source_decl,
                                             const FengTypeMember *member,
                                             FengSymbolError *out_error) {
    FengSymbolDeclView *decl = NULL;

    switch (member->kind) {
        case FENG_TYPE_MEMBER_FIELD:
            decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_FIELD,
                                       member->visibility,
                                       member->as.field.mutability,
                                       member->as.field.name,
                                       path,
                                       member->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            decl->value_type = build_site_type(ctx->analysis,
                                               member,
                                               member->as.field.type,
                                               path,
                                               member->token,
                                               out_error);
            if (decl->value_type == NULL) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            decl->bounded_decl = field_is_bounded_decl(member);
            if (!apply_decl_annotations(decl,
                                        ctx->module,
                                        member->annotations,
                                        member->annotation_count,
                                        false,
                                        path,
                                        member->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            break;

        case FENG_TYPE_MEMBER_METHOD:
        case FENG_TYPE_MEMBER_CONSTRUCTOR:
        case FENG_TYPE_MEMBER_FINALIZER:
            decl = new_decl_from_slice(member->kind == FENG_TYPE_MEMBER_METHOD
                                           ? FENG_SYMBOL_DECL_KIND_METHOD
                                           : member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR
                                                 ? FENG_SYMBOL_DECL_KIND_CONSTRUCTOR
                                                 : FENG_SYMBOL_DECL_KIND_FINALIZER,
                                       member->visibility,
                                       FENG_MUTABILITY_LET,
                                       member->as.callable.name,
                                       path,
                                       member->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            decl->return_type = build_callable_return_type(ctx->analysis,
                                                           &member->as.callable,
                                                           member->as.callable.return_type,
                                                           true,
                                                           path,
                                                           member->token,
                                                           out_error);
            if (decl->return_type == NULL) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (!fill_params(decl,
                             member->as.callable.params,
                             member->as.callable.param_count,
                             path,
                             member->token,
                             out_error) ||
                !apply_decl_annotations(decl,
                                        ctx->module,
                                        member->annotations,
                                        member->annotation_count,
                                        false,
                                        path,
                                        member->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            break;
    }

    if (!register_source_decl(ctx, member, decl, path, member->token, out_error)) {
        feng_symbol_internal_decl_free_members(decl);
        free(decl);
        return NULL;
    }

    (void)owner_source_decl;
    return decl;
}

static char *fit_display_name(const FengTypeRef *target) {
    FengSlice leaf = named_type_leaf_name(target);

    if (leaf.data != NULL) {
        return feng_symbol_internal_dup_slice(leaf);
    }
    return feng_symbol_internal_dup_cstr("fit");
}

static FengSymbolDeclView *build_top_level_decl(BuildContext *ctx,
                                                const char *path,
                                                const FengDecl *source_decl,
                                                FengSymbolError *out_error) {
    FengSymbolDeclView *decl = NULL;
    size_t index;

    switch (source_decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_BINDING,
                                       source_decl->visibility,
                                       source_decl->as.binding.mutability,
                                       source_decl->as.binding.name,
                                       path,
                                       source_decl->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            decl->value_type = build_site_type(ctx->analysis,
                                               &source_decl->as.binding,
                                               source_decl->as.binding.type,
                                               path,
                                               source_decl->token,
                                               out_error);
            if (decl->value_type == NULL ||
                !apply_decl_annotations(decl,
                                        ctx->module,
                                        source_decl->annotations,
                                        source_decl->annotation_count,
                                        false,
                                        path,
                                        source_decl->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            break;

        case FENG_DECL_TYPE:
            decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_TYPE,
                                       source_decl->visibility,
                                       FENG_MUTABILITY_LET,
                                       source_decl->as.type_decl.name,
                                       path,
                                       source_decl->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            if (!fill_declared_specs(decl,
                                     (const FengTypeRef *const *)source_decl->as.type_decl.declared_specs,
                                     source_decl->as.type_decl.declared_spec_count,
                                     path,
                                     source_decl->token,
                                     out_error) ||
                !apply_decl_annotations(decl,
                                        ctx->module,
                                        source_decl->annotations,
                                        source_decl->annotation_count,
                                        false,
                                        path,
                                        source_decl->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            for (index = 0U; index < source_decl->as.type_decl.member_count; ++index) {
                FengSymbolDeclView *member_decl = build_member_decl(ctx,
                                                                    path,
                                                                    source_decl,
                                                                    source_decl->as.type_decl.members[index],
                                                                    out_error);
                if (member_decl == NULL ||
                    !append_member_decl(decl,
                                        member_decl,
                                        path,
                                        source_decl->as.type_decl.members[index]->token,
                                        out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
            }
            return decl;

        case FENG_DECL_SPEC:
            decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_SPEC,
                                       source_decl->visibility,
                                       FENG_MUTABILITY_LET,
                                       source_decl->as.spec_decl.name,
                                       path,
                                       source_decl->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            if (!fill_declared_specs(decl,
                                     (const FengTypeRef *const *)source_decl->as.spec_decl.parent_specs,
                                     source_decl->as.spec_decl.parent_spec_count,
                                     path,
                                     source_decl->token,
                                     out_error) ||
                !apply_decl_annotations(decl,
                                        ctx->module,
                                        source_decl->annotations,
                                        source_decl->annotation_count,
                                        true,
                                        path,
                                        source_decl->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (source_decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                decl->return_type = build_callable_return_type(ctx->analysis,
                                                               source_decl,
                                                               source_decl->as.spec_decl.as.callable.return_type,
                                                               true,
                                                               path,
                                                               source_decl->token,
                                                               out_error);
                if (decl->return_type == NULL ||
                    !fill_params(decl,
                                 source_decl->as.spec_decl.as.callable.params,
                                 source_decl->as.spec_decl.as.callable.param_count,
                                 path,
                                 source_decl->token,
                                 out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
            } else {
                if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
                for (index = 0U; index < source_decl->as.spec_decl.as.object.member_count; ++index) {
                    FengSymbolDeclView *member_decl = build_member_decl(
                        ctx,
                        path,
                        source_decl,
                        source_decl->as.spec_decl.as.object.members[index],
                        out_error);
                    if (member_decl == NULL ||
                        !append_member_decl(decl,
                                            member_decl,
                                            path,
                                            source_decl->as.spec_decl.as.object.members[index]->token,
                                            out_error)) {
                        feng_symbol_internal_decl_free_members(decl);
                        free(decl);
                        return NULL;
                    }
                }
                return decl;
            }
            break;

        case FENG_DECL_FIT: {
            char *name = fit_display_name(source_decl->as.fit_decl.target);
            if (name == NULL) {
                feng_symbol_internal_set_error(out_error, path, source_decl->token, "out of memory building fit name");
                return NULL;
            }
            decl = new_decl(FENG_SYMBOL_DECL_KIND_FIT,
                            source_decl->visibility,
                            FENG_MUTABILITY_LET,
                            name,
                            path,
                            source_decl->token,
                            out_error);
            free(name);
            if (decl == NULL) {
                return NULL;
            }
            decl->fit_target = build_type_from_type_ref(source_decl->as.fit_decl.target,
                                                        path,
                                                        source_decl->token,
                                                        out_error);
            if (decl->fit_target == NULL ||
                !fill_declared_specs(decl,
                                     (const FengTypeRef *const *)source_decl->as.fit_decl.specs,
                                     source_decl->as.fit_decl.spec_count,
                                     path,
                                     source_decl->token,
                                     out_error) ||
                !apply_decl_annotations(decl,
                                        ctx->module,
                                        source_decl->annotations,
                                        source_decl->annotation_count,
                                        false,
                                        path,
                                        source_decl->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            for (index = 0U; index < source_decl->as.fit_decl.member_count; ++index) {
                FengSymbolDeclView *member_decl = build_member_decl(ctx,
                                                                    path,
                                                                    source_decl,
                                                                    source_decl->as.fit_decl.members[index],
                                                                    out_error);
                if (member_decl == NULL ||
                    !append_member_decl(decl,
                                        member_decl,
                                        path,
                                        source_decl->as.fit_decl.members[index]->token,
                                        out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
            }
            return decl;
        }

        case FENG_DECL_FUNCTION:
            decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_FUNCTION,
                                       source_decl->visibility,
                                       FENG_MUTABILITY_LET,
                                       source_decl->as.function_decl.name,
                                       path,
                                       source_decl->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            decl->is_extern = source_decl->is_extern;
            decl->return_type = build_callable_return_type(ctx->analysis,
                                                           &source_decl->as.function_decl,
                                                           source_decl->as.function_decl.return_type,
                                                           true,
                                                           path,
                                                           source_decl->token,
                                                           out_error);
            if (decl->return_type == NULL ||
                !fill_params(decl,
                             source_decl->as.function_decl.params,
                             source_decl->as.function_decl.param_count,
                             path,
                             source_decl->token,
                             out_error) ||
                !apply_decl_annotations(decl,
                                        ctx->module,
                                        source_decl->annotations,
                                        source_decl->annotation_count,
                                        source_decl->is_extern,
                                        path,
                                        source_decl->token,
                                        out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            break;
    }

    if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
        feng_symbol_internal_decl_free_members(decl);
        free(decl);
        return NULL;
    }
    return decl;
}

static bool collect_module_uses(BuildContext *ctx, FengSymbolError *out_error) {
    size_t program_index;

    for (program_index = 0U; program_index < ctx->module->program_count; ++program_index) {
        const FengProgram *program = ctx->module->programs[program_index];
        size_t use_index;

        for (use_index = 0U; use_index < program->use_count; ++use_index) {
            char *joined = join_segments(program->uses[use_index].segments,
                                         program->uses[use_index].segment_count);
            bool ok;

            if (joined == NULL) {
                return feng_symbol_internal_set_error(out_error,
                                                      program->path,
                                                      program->uses[use_index].token,
                                                      "out of memory collecting module uses");
            }
            ok = append_unique_string(&ctx->graph->uses,
                                      &ctx->graph->use_count,
                                      joined,
                                      program->path,
                                      program->uses[use_index].token,
                                      out_error);
            free(joined);
            if (!ok) {
                return false;
            }
        }
    }

    return true;
}

static bool build_ctor_bound_relations(BuildContext *ctx,
                                       const FengDecl *type_source_decl,
                                       FengSymbolDeclView *type_decl,
                                       const char *path,
                                       FengSymbolError *out_error) {
    size_t member_index;

    for (member_index = 0U; member_index < type_source_decl->as.type_decl.member_count; ++member_index) {
        const FengTypeMember *member = type_source_decl->as.type_decl.members[member_index];
        FengSlice *names = NULL;
        size_t name_count = 0U;
        size_t name_index;
        FengSymbolDeclView *ctor_decl;

        if (member->kind != FENG_TYPE_MEMBER_CONSTRUCTOR) {
            continue;
        }
        ctor_decl = find_source_decl(ctx->source_map, ctx->source_count, member);
        if (ctor_decl == NULL) {
            continue;
        }
        if (!collect_constructor_bound_names(type_source_decl,
                                             member,
                                             &names,
                                             &name_count,
                                             path,
                                             out_error)) {
            free(names);
            return false;
        }
        for (name_index = 0U; name_index < name_count; ++name_index) {
            FengSymbolDeclView *field_decl = find_member_decl_by_name(type_decl, names[name_index]);
            if (field_decl != NULL && field_decl->kind == FENG_SYMBOL_DECL_KIND_FIELD &&
                field_decl->mutability == FENG_MUTABILITY_LET && !field_decl->bounded_decl &&
                !append_unique_relation(ctx,
                                        FENG_SYMBOL_RELATION_CTOR_BINDS_MEMBER,
                                        ctor_decl,
                                        field_decl,
                                        ctor_decl,
                                        member->token,
                                        out_error)) {
                free(names);
                return false;
            }
        }
        free(names);
    }

    return true;
}

static bool build_direct_fit_target_relations(BuildContext *ctx, FengSymbolError *out_error) {
    size_t program_index;

    for (program_index = 0U; program_index < ctx->module->program_count; ++program_index) {
        const FengProgram *program = ctx->module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *source_decl = program->declarations[decl_index];
            FengSymbolDeclView *fit_decl;
            FengSymbolDeclView *target_decl;

            if (source_decl->kind != FENG_DECL_FIT) {
                continue;
            }
            fit_decl = find_source_decl(ctx->source_map, ctx->source_count, source_decl);
            target_decl = find_local_type_like_decl(ctx, source_decl->as.fit_decl.target);
            if (!append_unique_relation(ctx,
                                        FENG_SYMBOL_RELATION_FIT_EXTENDS_TYPE,
                                        fit_decl,
                                        target_decl,
                                        fit_decl,
                                        source_decl->token,
                                        out_error)) {
                return false;
            }
        }
    }

    return true;
}

static bool build_spec_relations(BuildContext *ctx, FengSymbolError *out_error) {
    size_t relation_index;

    for (relation_index = 0U; relation_index < ctx->analysis->spec_relation_count; ++relation_index) {
        const FengSpecRelation *relation = &ctx->analysis->spec_relations[relation_index];
        FengSymbolDeclView *left_type = find_source_decl(ctx->source_map, ctx->source_count, relation->type_decl);
        FengSymbolDeclView *right_spec = find_source_decl(ctx->source_map, ctx->source_count, relation->spec_decl);
        size_t source_index;

        if (left_type != NULL && right_spec != NULL &&
            !append_unique_relation(ctx,
                                    FENG_SYMBOL_RELATION_TYPE_IMPLEMENTS_SPEC,
                                    left_type,
                                    right_spec,
                                    left_type,
                                    relation->type_decl->token,
                                    out_error)) {
            return false;
        }

        for (source_index = 0U; source_index < relation->source_count; ++source_index) {
            const FengSpecRelationSource *source = &relation->sources[source_index];
            FengSymbolDeclView *fit_decl = find_source_decl(ctx->source_map,
                                                            ctx->source_count,
                                                            source->via_fit_decl);

            if (fit_decl != NULL && right_spec != NULL &&
                !append_unique_relation(ctx,
                                        FENG_SYMBOL_RELATION_FIT_IMPLEMENTS_SPEC,
                                        fit_decl,
                                        right_spec,
                                        fit_decl,
                                        source->via_fit_decl != NULL ? source->via_fit_decl->token : relation->type_decl->token,
                                        out_error)) {
                return false;
            }
            if (fit_decl != NULL && left_type != NULL &&
                !append_unique_relation(ctx,
                                        FENG_SYMBOL_RELATION_FIT_EXTENDS_TYPE,
                                        fit_decl,
                                        left_type,
                                        fit_decl,
                                        source->via_fit_decl != NULL ? source->via_fit_decl->token : relation->type_decl->token,
                                        out_error)) {
                return false;
            }
        }
    }

    return true;
}

static FengSymbolModuleGraph *build_module_graph(const FengSemanticAnalysis *analysis,
                                                 const FengSemanticModule *module,
                                                 FengSymbolError *out_error) {
    BuildContext ctx;
    size_t index;

    memset(&ctx, 0, sizeof(ctx));
    ctx.analysis = analysis;
    ctx.module = module;
    ctx.graph = (FengSymbolModuleGraph *)calloc(1U, sizeof(*ctx.graph));
    if (ctx.graph == NULL) {
        feng_symbol_internal_set_error(out_error,
                                       module->program_count > 0U ? module->programs[0]->path : NULL,
                                       (FengToken){0},
                                       "out of memory allocating module graph");
        return NULL;
    }
    ctx.graph->profile = FENG_SYMBOL_PROFILE_WORKSPACE_CACHE;
    ctx.graph->visibility = module->visibility;
    ctx.graph->segment_count = module->segment_count;
    ctx.graph->segments = (char **)calloc(module->segment_count, sizeof(*ctx.graph->segments));
    if (module->segment_count > 0U && ctx.graph->segments == NULL) {
        feng_symbol_internal_set_error(out_error,
                                       module->program_count > 0U ? module->programs[0]->path : NULL,
                                       (FengToken){0},
                                       "out of memory cloning module segments");
        feng_symbol_internal_module_free(ctx.graph);
        return NULL;
    }
    for (index = 0U; index < module->segment_count; ++index) {
        ctx.graph->segments[index] = feng_symbol_internal_dup_slice(module->segments[index]);
        if (ctx.graph->segments[index] == NULL) {
            feng_symbol_internal_set_error(out_error,
                                           module->programs[0]->path,
                                           (FengToken){0},
                                           "out of memory cloning module segment");
            feng_symbol_internal_module_free(ctx.graph);
            return NULL;
        }
    }
    ctx.graph->primary_path = module->program_count > 0U
                                  ? feng_symbol_internal_dup_cstr(module->programs[0]->path)
                                  : NULL;
    ctx.graph->root_decl.kind = FENG_SYMBOL_DECL_KIND_MODULE;
    ctx.graph->root_decl.visibility = module->visibility;
    ctx.graph->root_decl.mutability = FENG_MUTABILITY_LET;
    ctx.graph->root_decl.token = module->program_count > 0U ? module->programs[0]->module_token
                                                            : (FengToken){0};
    ctx.graph->root_decl.path = ctx.graph->primary_path != NULL
                                    ? feng_symbol_internal_dup_cstr(ctx.graph->primary_path)
                                    : NULL;
    ctx.graph->root_decl.name = module->segment_count > 0U
                                    ? feng_symbol_internal_dup_slice(module->segments[module->segment_count - 1U])
                                    : feng_symbol_internal_dup_cstr("module");
    if ((ctx.graph->primary_path != NULL && ctx.graph->root_decl.path == NULL) ||
        ctx.graph->root_decl.name == NULL) {
        feng_symbol_internal_set_error(out_error,
                                       module->program_count > 0U ? module->programs[0]->path : NULL,
                                       (FengToken){0},
                                       "out of memory initializing module root declaration");
        feng_symbol_internal_module_free(ctx.graph);
        return NULL;
    }

    if (!collect_module_uses(&ctx, out_error)) {
        feng_symbol_internal_module_free(ctx.graph);
        free(ctx.source_map);
        return NULL;
    }

    for (index = 0U; index < module->program_count; ++index) {
        const FengProgram *program = module->programs[index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            FengSymbolDeclView *decl = build_top_level_decl(&ctx,
                                                            program->path,
                                                            program->declarations[decl_index],
                                                            out_error);
            if (decl == NULL ||
                !append_member_decl(&ctx.graph->root_decl,
                                    decl,
                                    program->path,
                                    program->declarations[decl_index]->token,
                                    out_error)) {
                feng_symbol_internal_module_free(ctx.graph);
                free(ctx.source_map);
                return NULL;
            }
        }
    }

    for (index = 0U; index < module->program_count; ++index) {
        const FengProgram *program = module->programs[index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *source_decl = program->declarations[decl_index];
            FengSymbolDeclView *decl = find_source_decl(ctx.source_map, ctx.source_count, source_decl);

            if (source_decl->kind == FENG_DECL_TYPE && decl != NULL &&
                !build_ctor_bound_relations(&ctx, source_decl, decl, program->path, out_error)) {
                feng_symbol_internal_module_free(ctx.graph);
                free(ctx.source_map);
                return NULL;
            }
        }
    }

    if (!build_direct_fit_target_relations(&ctx, out_error) ||
        !build_spec_relations(&ctx, out_error)) {
        feng_symbol_internal_module_free(ctx.graph);
        free(ctx.source_map);
        return NULL;
    }

    free(ctx.source_map);
    return ctx.graph;
}

static char *module_output_path(const char *root,
                                const FengSymbolModuleGraph *module,
                                FengSymbolError *out_error) {
    char *current;
    size_t index;
    char *filename;
    char *path;

    if (root == NULL || module == NULL || module->segment_count == 0U) {
        return NULL;
    }
    current = feng_symbol_internal_dup_cstr(root);
    if (current == NULL) {
        feng_symbol_internal_set_error(out_error, root, (FengToken){0}, "out of memory composing module output path");
        return NULL;
    }
    for (index = 0U; index + 1U < module->segment_count; ++index) {
        char *next = path_join(current, module->segments[index]);
        free(current);
        current = next;
        if (current == NULL) {
            feng_symbol_internal_set_error(out_error, root, (FengToken){0}, "out of memory composing module output path");
            return NULL;
        }
    }
    filename = (char *)malloc(strlen(module->segments[module->segment_count - 1U]) + 4U);
    if (filename == NULL) {
        free(current);
        feng_symbol_internal_set_error(out_error, root, (FengToken){0}, "out of memory composing .ft filename");
        return NULL;
    }
    strcpy(filename, module->segments[module->segment_count - 1U]);
    strcat(filename, ".ft");
    path = path_join(current, filename);
    free(filename);
    free(current);
    if (path == NULL) {
        feng_symbol_internal_set_error(out_error, root, (FengToken){0}, "out of memory composing module output path");
    }
    return path;
}

bool feng_symbol_build_graph(const FengSemanticAnalysis *analysis,
                             FengSymbolGraph **out_graph,
                             FengSymbolError *out_error) {
    FengSymbolGraph *graph;
    size_t module_index;

    if (out_graph == NULL || analysis == NULL) {
        return false;
    }

    *out_graph = NULL;
    graph = (FengSymbolGraph *)calloc(1U, sizeof(*graph));
    if (graph == NULL) {
        return feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory allocating symbol graph");
    }

    for (module_index = 0U; module_index < analysis->module_count; ++module_index) {
        FengSymbolModuleGraph *module_graph;

        /* External package modules are already compiled; they do not generate
         * new symbol-table output for the current compilation unit. */
        if (analysis->modules[module_index].is_external_package) {
            continue;
        }

        module_graph = build_module_graph(analysis,
                                          &analysis->modules[module_index],
                                          out_error);
        if (module_graph == NULL ||
            !feng_symbol_internal_graph_append_module(graph, module_graph, out_error)) {
            if (module_graph != NULL) {
                feng_symbol_internal_module_free(module_graph);
            }
            feng_symbol_graph_free(graph);
            return false;
        }
    }

    *out_graph = graph;
    return true;
}

bool feng_symbol_export_graph(const FengSymbolGraph *graph,
                              const FengSymbolExportOptions *options,
                              FengSymbolError *out_error) {
    size_t module_index;

    if (graph == NULL || options == NULL) {
        return false;
    }

    for (module_index = 0U; module_index < graph->module_count; ++module_index) {
        const FengSymbolModuleGraph *module = graph->modules[module_index];

        if (options->public_root != NULL && visibility_is_public(module->visibility)) {
            char *path = module_output_path(options->public_root, module, out_error);
            if (path == NULL) {
                return false;
            }
            if (!ensure_parent_dir(path, out_error) ||
                !feng_symbol_ft_write_module(module,
                                             FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                             path,
                                             out_error)) {
                free(path);
                return false;
            }
            free(path);
        }

        if (options->workspace_root != NULL) {
            char *path = module_output_path(options->workspace_root, module, out_error);
            if (path == NULL) {
                return false;
            }
            if (!ensure_parent_dir(path, out_error) ||
                !feng_symbol_ft_write_module(module,
                                             FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                                             path,
                                             out_error)) {
                free(path);
                return false;
            }
            free(path);
        }
    }

    return true;
}

bool feng_symbol_export_analysis(const FengSemanticAnalysis *analysis,
                                 const FengSymbolExportOptions *options,
                                 FengSymbolError *out_error) {
    FengSymbolGraph *graph = NULL;
    bool ok;

    if (analysis == NULL || options == NULL) {
        return false;
    }

    if (!feng_symbol_build_graph(analysis, &graph, out_error)) {
        return false;
    }
    ok = feng_symbol_export_graph(graph, options, out_error);
    feng_symbol_graph_free(graph);
    return ok;
}

