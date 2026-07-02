#include "symbol/export.h"

#include <stdint.h>
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

static bool is_horizontal_doc_space(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
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
    /* After AST alias normalization (dev/feng-scalar-alias-optimize.md §6),
     * only canonical width-explicit names reach this function. */
    static const struct {
        const char *alias;
        const char *canonical;
    } table[] = {
        {"i8", "i8"},
        {"i16", "i16"},
        {"i32", "i32"},
        {"i64", "i64"},
        {"u8", "u8"},
        {"u16", "u16"},
        {"u32", "u32"},
        {"u64", "u64"},
        {"f32", "f32"},
        {"f64", "f64"},
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

typedef struct DocLineSpan {
    const char *start;
    size_t length;
} DocLineSpan;

static bool normalize_doc_comment(FengSlice raw,
                                  char **out_text,
                                  const char *path,
                                  FengToken token,
                                  FengSymbolError *out_error) {
    const char *body_start;
    const char *body_end;
    const char *cursor;
    DocLineSpan *lines = NULL;
    size_t line_count = 0U;
    size_t first_line = 0U;
    size_t last_line;
    size_t total_length = 0U;
    size_t index;
    char *normalized;
    size_t output_cursor = 0U;

    *out_text = NULL;
    if (raw.data == NULL || raw.length == 0U) {
        return true;
    }
    if (raw.length < 5U || memcmp(raw.data, "/**", 3U) != 0 ||
        memcmp(raw.data + raw.length - 2U, "*/", 2U) != 0) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              token,
                                              "malformed doc comment slice in symbol export");
    }

    body_start = raw.data + 3U;
    body_end = raw.data + raw.length - 2U;
    cursor = body_start;
    while (cursor < body_end) {
        const char *line_start = cursor;
        const char *line_end;
        DocLineSpan *grown;

        while (cursor < body_end && *cursor != '\n' && *cursor != '\r') {
            ++cursor;
        }
        line_end = cursor;
        if (cursor < body_end) {
            if (*cursor == '\r') {
                ++cursor;
                if (cursor < body_end && *cursor == '\n') {
                    ++cursor;
                }
            } else {
                ++cursor;
            }
        }

        while (line_start < line_end && is_horizontal_doc_space(*line_start)) {
            ++line_start;
        }
        if (line_start < line_end && *line_start == '*') {
            ++line_start;
            if (line_start < line_end && (*line_start == ' ' || *line_start == '\t')) {
                ++line_start;
            }
        }
        while (line_end > line_start && is_horizontal_doc_space(*(line_end - 1))) {
            --line_end;
        }

        grown = (DocLineSpan *)realloc(lines, (line_count + 1U) * sizeof(*lines));
        if (grown == NULL) {
            free(lines);
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  token,
                                                  "out of memory normalizing doc comment");
        }
        lines = grown;
        lines[line_count].start = line_start;
        lines[line_count].length = (size_t)(line_end - line_start);
        ++line_count;
    }

    while (first_line < line_count && lines[first_line].length == 0U) {
        ++first_line;
    }
    last_line = line_count;
    while (last_line > first_line && lines[last_line - 1U].length == 0U) {
        --last_line;
    }
    if (first_line == last_line) {
        free(lines);
        return true;
    }

    for (index = first_line; index < last_line; ++index) {
        total_length += lines[index].length;
        if (index + 1U < last_line) {
            ++total_length;
        }
    }
    normalized = (char *)malloc(total_length + 1U);
    if (normalized == NULL) {
        free(lines);
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              token,
                                              "out of memory allocating normalized doc comment");
    }

    for (index = first_line; index < last_line; ++index) {
        if (lines[index].length > 0U) {
            memcpy(normalized + output_cursor, lines[index].start, lines[index].length);
            output_cursor += lines[index].length;
        }
        if (index + 1U < last_line) {
            normalized[output_cursor++] = '\n';
        }
    }
    normalized[output_cursor] = '\0';
    free(lines);
    *out_text = normalized;
    return true;
}

static bool apply_decl_doc_comment(FengSymbolDeclView *decl,
                                   FengSlice raw_doc,
                                   const char *path,
                                   FengToken token,
                                   FengSymbolError *out_error) {
    char *normalized = NULL;

    if (!normalize_doc_comment(raw_doc, &normalized, path, token, out_error)) {
        return false;
    }
    decl->doc = normalized;
    decl->has_doc = normalized != NULL;
    return true;
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
    } else if (decl->kind == FENG_DECL_ENUM) {
        type->as.named.segments[module->segment_count] =
            feng_symbol_internal_dup_slice(decl->as.enum_decl.name);
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

/* Build context for export — moved here so helper functions can access it. */
typedef struct BuildContext {
    const FengSemanticAnalysis *analysis;
    const FengSemanticModule *module;
    const FengProgram *current_program;
    FengSymbolModuleGraph *graph;
    DeclSourceMap *source_map;
    size_t source_count;
    /* Current enclosing generic context; NULL / 0 outside generics. */
    const FengTypeParam *type_params;
    size_t type_param_count;
} BuildContext;

/* Check whether a declaration is a type-like kind (type, enum, or spec) with
 * the given name. */
static bool decl_is_type_named(const FengDecl *decl, FengSlice name) {
    return (decl->kind == FENG_DECL_TYPE &&
            feng_symbol_internal_slice_equals(decl->as.type_decl.name, name)) ||
           (decl->kind == FENG_DECL_ENUM &&
            feng_symbol_internal_slice_equals(decl->as.enum_decl.name, name)) ||
           (decl->kind == FENG_DECL_SPEC &&
            feng_symbol_internal_slice_equals(decl->as.spec_decl.name, name));
}

/* Check whether a module has a public type, spec, or enum with the given name.
 * Used to find the source module of a cross-module type reference. */
static bool module_has_public_type(const FengSemanticModule *module, FengSlice name) {
    size_t prog_index;

    if (module == NULL) {
        return false;
    }
    for (prog_index = 0U; prog_index < module->program_count; ++prog_index) {
        const FengProgram *prog = module->programs[prog_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < prog->declaration_count; ++decl_index) {
            if (prog->declarations[decl_index]->visibility == FENG_VISIBILITY_PUBLIC &&
                decl_is_type_named(prog->declarations[decl_index], name)) {
                return true;
            }
        }
    }
    return false;
}

/* Check whether the current module (being exported) defines a type with
 * the given name, regardless of visibility.  Module-local types (including
 * non-public ones) do not need path expansion because they belong to the
 * same module being exported. */
static bool current_module_has_type(const BuildContext *ctx, FengSlice name) {
    size_t prog_index;

    if (ctx == NULL || ctx->module == NULL) {
        return false;
    }
    for (prog_index = 0U; prog_index < ctx->module->program_count; ++prog_index) {
        const FengProgram *prog = ctx->module->programs[prog_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < prog->declaration_count; ++decl_index) {
            if (decl_is_type_named(prog->declarations[decl_index], name)) {
                return true;
            }
        }
    }
    return false;
}

/* Get the effective alias for a use declaration.  If has_alias is true, use
 * the explicit alias; otherwise the last segment is the implicit alias. */
static FengSlice use_effective_alias(const FengUseDecl *use_decl) {
    if (use_decl->has_alias) {
        return use_decl->alias;
    }
    if (use_decl->segment_count > 0U) {
        return use_decl->segments[use_decl->segment_count - 1U];
    }
    return (FengSlice){NULL, 0U};
}

/* Find the semantic module in the analysis matching the given segment path. */
static const FengSemanticModule *find_analysis_module(
    const FengSemanticAnalysis *analysis,
    const FengSlice *segments,
    size_t segment_count) {
    size_t index;

    if (analysis == NULL) {
        return NULL;
    }
    for (index = 0U; index < analysis->module_count; ++index) {
        const FengSemanticModule *module = &analysis->modules[index];
        size_t seg;
        bool match;

        if (module->segment_count != segment_count) {
            continue;
        }
        match = true;
        for (seg = 0U; seg < segment_count; ++seg) {
            if (!feng_symbol_internal_slice_equals(module->segments[seg],
                                                    segments[seg])) {
                match = false;
                break;
            }
        }
        if (match) {
            return module;
        }
    }
    return NULL;
}

/* Build a fully-qualified segment array by appending type_name after
 * prefix_segments[0..prefix_count-1].  Returns a newly allocated char **
 * array with (prefix_count + 1) entries, or NULL on allocation failure. */
static char **build_qualified_segments(const FengSlice *prefix_segments,
                                       size_t prefix_count,
                                       FengSlice type_name,
                                       size_t *out_count) {
    size_t total = prefix_count + 1U;
    char **result = (char **)calloc(total, sizeof(*result));
    size_t seg;

    *out_count = 0U;
    if (result == NULL) {
        return NULL;
    }
    for (seg = 0U; seg < prefix_count; ++seg) {
        result[seg] = feng_symbol_internal_dup_slice(prefix_segments[seg]);
        if (result[seg] == NULL) {
            while (seg > 0U) free(result[--seg]);
            free(result);
            return NULL;
        }
    }
    result[prefix_count] = feng_symbol_internal_dup_slice(type_name);
    if (result[prefix_count] == NULL) {
        for (seg = 0U; seg < prefix_count; ++seg) free(result[seg]);
        free(result);
        return NULL;
    }
    *out_count = total;
    return result;
}

/* Expand a type *usage* to its fully-qualified module path.
 *
 * .ft distinguishes type definitions from type usages:
 *   - Definitions (SYMS name_str): short name, scoped by owning module.
 *   - Usages (TYPS string_ref):   always fully-qualified, self-describing.
 *
 * This function handles 1-segment and 2-segment (alias-prefixed) names.
 * Builtins are skipped (return NULL).  All other names — including types
 * defined in the current module — are expanded to full paths.
 *
 * Returns a newly allocated char ** segment array and sets *out_count,
 * or NULL when no expansion is needed (builtins) or on error.
 * When a 1-segment name cannot be resolved, an export error is reported
 * through out_error. */
static char **resolve_type_ref_segments(const BuildContext *ctx,
                                        const FengSlice *segments,
                                        size_t segment_count,
                                        size_t *out_count,
                                        const char *path,
                                        FengToken token,
                                        FengSymbolError *out_error) {
    *out_count = 0U;
    if (ctx == NULL || ctx->current_program == NULL || ctx->analysis == NULL) {
        return NULL;
    }

    if (segment_count == 1U) {
        FengSlice name = segments[0];
        size_t use_index;

        if (canonical_builtin_name(name) != NULL) {
            return NULL;
        }

        /* Module-local type: qualify with the current module's path. */
        if (current_module_has_type(ctx, name) && ctx->module != NULL) {
            return build_qualified_segments(ctx->module->segments,
                                           ctx->module->segment_count,
                                           name, out_count);
        }

        /* Search imported modules for a matching public type. */
        for (use_index = 0U; use_index < ctx->current_program->use_count; ++use_index) {
            const FengUseDecl *use_decl = &ctx->current_program->uses[use_index];
            const FengSemanticModule *target = find_analysis_module(
                ctx->analysis, use_decl->segments, use_decl->segment_count);

            if (target != NULL && module_has_public_type(target, name)) {
                return build_qualified_segments(target->segments,
                                               target->segment_count,
                                               name, out_count);
            }
        }

        /* A 1-segment name that is neither builtin, module-local, nor found
         * in any imported module cannot be exported correctly. */
        {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "cannot resolve type '%.*s' to a fully-qualified path during export",
                     (int)name.length, (const char *)name.data);
            feng_symbol_internal_set_error(out_error, path, token, msg);
        }
        return NULL;
    }

    if (segment_count == 2U) {
        FengSlice alias = segments[0];
        FengSlice name = segments[1];
        size_t use_index;

        for (use_index = 0U; use_index < ctx->current_program->use_count; ++use_index) {
            const FengUseDecl *use_decl = &ctx->current_program->uses[use_index];
            FengSlice effective = use_effective_alias(use_decl);

            if (feng_symbol_internal_slice_equals(effective, alias)) {
                return build_qualified_segments(use_decl->segments,
                                               use_decl->segment_count,
                                               name, out_count);
            }
        }
        return NULL;
    }

    return NULL;
}

static FengSymbolTypeView *build_type_from_type_ref(const BuildContext *ctx,
                                                    const FengTypeRef *type_ref,
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
            char **resolved = NULL;
            size_t resolved_count = 0U;

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

            resolved = resolve_type_ref_segments(ctx,
                                                  type_ref->as.named.segments,
                                                  type_ref->as.named.segment_count,
                                                  &resolved_count,
                                                  path, token, out_error);

            type = new_type(FENG_SYMBOL_TYPE_KIND_NAMED, path, token, out_error);
            if (type == NULL) {
                for (index = 0U; index < resolved_count; ++index) free(resolved[index]);
                free(resolved);
                return NULL;
            }
            if (resolved != NULL) {
                type->as.named.segment_count = resolved_count;
                type->as.named.segments = resolved;
            } else {
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
            }
            return type;
        }

        case FENG_TYPE_REF_POINTER:
            type = new_type(FENG_SYMBOL_TYPE_KIND_POINTER, path, token, out_error);
            if (type == NULL) {
                return NULL;
            }
            type->as.pointer.inner = build_type_from_type_ref(ctx, type_ref->as.inner,
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
            type->as.array.element = build_type_from_type_ref(ctx, cursor, path, token, out_error);
            if (cursor != NULL && type->as.array.element == NULL) {
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            return type;
        }
    }

    return NULL;
}

/* Returns the index of the type parameter whose name matches the given segment,
 * or SIZE_MAX if not found. */
static size_t find_type_param_index(const FengTypeParam *type_params,
                                    size_t type_param_count,
                                    FengSlice segment) {
    size_t index;

    for (index = 0U; index < type_param_count; ++index) {
        if (feng_symbol_internal_slice_equals(type_params[index].name, segment)) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Like build_type_from_type_ref, but resolves single-segment named types that
 * match a type parameter to TYPE_PARAM_REF nodes, and handles named types with
 * type_args as NAMED_GENERIC nodes. */
static FengSymbolTypeView *build_type_from_type_ref_with_tparams(
    const BuildContext *ctx,
    const FengTypeRef *type_ref,
    const FengTypeParam *type_params,
    size_t type_param_count,
    const char *path,
    FengToken token,
    FengSymbolError *out_error);

static FengSymbolTypeView *build_type_from_type_ref_with_tparams(
    const BuildContext *ctx,
    const FengTypeRef *type_ref,
    const FengTypeParam *type_params,
    size_t type_param_count,
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

            /* Check if this is a single-segment name that matches a type param. */
            if (type_ref->as.named.segment_count == 1U && type_param_count > 0U) {
                size_t param_index = find_type_param_index(type_params,
                                                           type_param_count,
                                                           type_ref->as.named.segments[0]);
                if (param_index != SIZE_MAX) {
                    type = new_type(FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF, path, token, out_error);
                    if (type == NULL) {
                        return NULL;
                    }
                    type->as.type_param_ref.name =
                        feng_symbol_internal_dup_slice(type_ref->as.named.segments[0]);
                    if (type->as.type_param_ref.name == NULL) {
                        feng_symbol_internal_set_error(out_error, path, token,
                                                       "out of memory cloning type param name");
                        feng_symbol_internal_type_free(type);
                        return NULL;
                    }
                    return type;
                }
            }

            /* Check if it is a generic application (has type args). */
            if (type_ref->as.named.type_arg_count > 0U) {
                char **resolved = NULL;
                size_t resolved_count = 0U;

                resolved = resolve_type_ref_segments(ctx,
                                                      type_ref->as.named.segments,
                                                      type_ref->as.named.segment_count,
                                                      &resolved_count,
                                                      path, token, out_error);

                type = new_type(FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC, path, token, out_error);
                if (type == NULL) {
                    for (index = 0U; index < resolved_count; ++index) free(resolved[index]);
                    free(resolved);
                    return NULL;
                }
                if (resolved != NULL) {
                    type->as.named_generic.segment_count = resolved_count;
                    type->as.named_generic.segments = resolved;
                } else {
                    type->as.named_generic.segment_count = type_ref->as.named.segment_count;
                    type->as.named_generic.segments =
                        (char **)calloc(type->as.named_generic.segment_count,
                                        sizeof(*type->as.named_generic.segments));
                    if (type->as.named_generic.segments == NULL) {
                        feng_symbol_internal_set_error(out_error, path, token,
                                                       "out of memory cloning named_generic segments");
                        feng_symbol_internal_type_free(type);
                        return NULL;
                    }
                    for (index = 0U; index < type->as.named_generic.segment_count; ++index) {
                        type->as.named_generic.segments[index] =
                            feng_symbol_internal_dup_slice(type_ref->as.named.segments[index]);
                        if (type->as.named_generic.segments[index] == NULL) {
                            feng_symbol_internal_set_error(out_error, path, token,
                                                           "out of memory cloning named_generic segment");
                            feng_symbol_internal_type_free(type);
                            return NULL;
                        }
                    }
                }
                type->as.named_generic.type_arg_count = type_ref->as.named.type_arg_count;
                type->as.named_generic.type_args =
                    (FengSymbolTypeView **)calloc(type->as.named_generic.type_arg_count,
                                                  sizeof(*type->as.named_generic.type_args));
                if (type->as.named_generic.type_args == NULL) {
                    feng_symbol_internal_set_error(out_error, path, token,
                                                   "out of memory cloning named_generic type_args");
                    feng_symbol_internal_type_free(type);
                    return NULL;
                }
                for (index = 0U; index < type->as.named_generic.type_arg_count; ++index) {
                    type->as.named_generic.type_args[index] =
                        build_type_from_type_ref_with_tparams(ctx,
                                                              type_ref->as.named.type_args[index],
                                                              type_params,
                                                              type_param_count,
                                                              path,
                                                              token,
                                                              out_error);
                    if (type_ref->as.named.type_args[index] != NULL &&
                        type->as.named_generic.type_args[index] == NULL) {
                        feng_symbol_internal_type_free(type);
                        return NULL;
                    }
                }
                return type;
            }

            /* Fall back to plain named or builtin handling. */
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
                    feng_symbol_internal_set_error(out_error, path, token,
                                                   "out of memory cloning builtin name");
                    feng_symbol_internal_type_free(type);
                    return NULL;
                }
                return type;
            }

            {
                char **resolved = NULL;
                size_t resolved_count = 0U;

                resolved = resolve_type_ref_segments(ctx,
                                                      type_ref->as.named.segments,
                                                      type_ref->as.named.segment_count,
                                                      &resolved_count,
                                                      path, token, out_error);

                type = new_type(FENG_SYMBOL_TYPE_KIND_NAMED, path, token, out_error);
                if (type == NULL) {
                    for (index = 0U; index < resolved_count; ++index) free(resolved[index]);
                    free(resolved);
                    return NULL;
                }
                if (resolved != NULL) {
                    type->as.named.segment_count = resolved_count;
                    type->as.named.segments = resolved;
                } else {
                    type->as.named.segment_count = type_ref->as.named.segment_count;
                    type->as.named.segments = (char **)calloc(type->as.named.segment_count,
                                                              sizeof(*type->as.named.segments));
                    if (type->as.named.segments == NULL) {
                        feng_symbol_internal_set_error(out_error, path, token,
                                                       "out of memory cloning named type segments");
                        feng_symbol_internal_type_free(type);
                        return NULL;
                    }
                    for (index = 0U; index < type->as.named.segment_count; ++index) {
                        type->as.named.segments[index] =
                            feng_symbol_internal_dup_slice(type_ref->as.named.segments[index]);
                        if (type->as.named.segments[index] == NULL) {
                            feng_symbol_internal_set_error(out_error, path, token,
                                                           "out of memory cloning named type segment");
                            feng_symbol_internal_type_free(type);
                            return NULL;
                        }
                    }
                }
                return type;
            }
        }

        case FENG_TYPE_REF_POINTER:
            type = new_type(FENG_SYMBOL_TYPE_KIND_POINTER, path, token, out_error);
            if (type == NULL) {
                return NULL;
            }
            type->as.pointer.inner =
                build_type_from_type_ref_with_tparams(ctx,
                                                      type_ref->as.inner,
                                                      type_params,
                                                      type_param_count,
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
            type->as.array.layer_writable =
                (bool *)calloc(rank, sizeof(*type->as.array.layer_writable));
            if (type->as.array.layer_writable == NULL) {
                feng_symbol_internal_set_error(out_error, path, token,
                                               "out of memory cloning array mutability bitmap");
                feng_symbol_internal_type_free(type);
                return NULL;
            }

            cursor = type_ref;
            while (cursor != NULL && cursor->kind == FENG_TYPE_REF_ARRAY) {
                type->as.array.layer_writable[array_index++] = cursor->array_element_writable;
                cursor = cursor->as.inner;
            }
            type->as.array.element =
                build_type_from_type_ref_with_tparams(ctx,
                                                      cursor,
                                                      type_params,
                                                      type_param_count,
                                                      path,
                                                      token,
                                                      out_error);
            if (cursor != NULL && type->as.array.element == NULL) {
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            return type;
        }
    }

    return NULL;
}

static FengSymbolTypeView *build_type_from_fact(const BuildContext *ctx,
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
            return build_type_from_type_ref(ctx, fact->type_ref, path, token, out_error);

        case FENG_SEMANTIC_TYPE_FACT_DECL:
            return build_named_type_from_decl(ctx != NULL ? ctx->analysis : NULL,
                                              fact->type_decl, path, token, out_error);

        case FENG_SEMANTIC_TYPE_FACT_UNKNOWN:
        default:
            break;
    }

    return NULL;
}

static bool fill_declared_specs(const BuildContext *ctx,
                                FengSymbolDeclView *decl,
                                const FengTypeRef *const *specs,
                                size_t spec_count,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < spec_count; ++index) {
        FengSymbolTypeView *type = build_type_from_type_ref(ctx, specs[index], path, token, out_error);
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

static bool fill_declared_specs_with_tparams(const BuildContext *ctx,
                                             FengSymbolDeclView *decl,
                                             const FengTypeRef *const *specs,
                                             size_t spec_count,
                                             const FengTypeParam *type_params,
                                             size_t type_param_count,
                                             const char *path,
                                             FengToken token,
                                             FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < spec_count; ++index) {
        FengSymbolTypeView *type = build_type_from_type_ref_with_tparams(ctx,
                                                                         specs[index],
                                                                         type_params,
                                                                         type_param_count,
                                                                         path,
                                                                         token,
                                                                         out_error);
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

static bool fill_union_members_with_tparams(const BuildContext *ctx,
                                            FengSymbolDeclView *decl,
                                            const FengUnionSpecInfo *union_info,
                                            const FengTypeParam *type_params,
                                            size_t type_param_count,
                                            const char *path,
                                            FengToken token,
                                            FengSymbolError *out_error) {
    if (union_info == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              token,
                                              "missing normalized union metadata for symbol export");
    }

    for (size_t index = 0U; index < union_info->member_count; ++index) {
        FengSymbolTypeView *type = build_type_from_type_ref_with_tparams(
            ctx,
            union_info->members[index].type_ref,
            type_params,
            type_param_count,
            path,
            token,
            out_error);

        if (union_info->members[index].type_ref != NULL && type == NULL) {
            return false;
        }
        if (!append_type_pointer(&decl->union_members,
                                 &decl->union_member_count,
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

/* 从语义阶段的 FengReifiableDepSet 读取依赖，按 kind 分类填入 DeclView 的
 * reifiable_agg_deps（AGGREGATE）和 reifiable_type_deps（MANAGED）。
 * 每个依赖的 type_ref 转换为 NAMED_GENERIC 类型视图。 */
static bool fill_reifiable_deps(const BuildContext *ctx,
                                FengSymbolDeclView *decl,
                                const FengDecl *source_decl,
                                const FengTypeParam *type_params,
                                size_t type_param_count,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    const FengReifiableDepSet *dep_set;
    size_t index;

    dep_set = feng_semantic_lookup_reifiable_dep_set(ctx->analysis, source_decl);
    if (dep_set == NULL || dep_set->dep_count == 0U) {
        return true;
    }

    for (index = 0U; index < dep_set->dep_count; ++index) {
        const FengReifiableDep *dep = &dep_set->deps[index];
        FengSymbolTypeView *type = build_type_from_type_ref_with_tparams(
            ctx,
            dep->type_ref,
            type_params,
            type_param_count,
            path,
            token,
            out_error);

        if (dep->type_ref != NULL && type == NULL) {
            return false;
        }

        if (dep->kind == FENG_REIFIABLE_DEP_KIND_AGGREGATE) {
            if (!append_type_pointer(&decl->reifiable_agg_deps,
                                     &decl->reifiable_agg_dep_count,
                                     type,
                                     path,
                                     token,
                                     out_error)) {
                feng_symbol_internal_type_free(type);
                return false;
            }
        } else {
            if (!append_type_pointer(&decl->reifiable_type_deps,
                                     &decl->reifiable_type_dep_count,
                                     type,
                                     path,
                                     token,
                                     out_error)) {
                feng_symbol_internal_type_free(type);
                return false;
            }
        }
    }

    return true;
}

static bool fill_params_with_tparams(const BuildContext *ctx,
                                     FengSymbolDeclView *decl,
                                     const FengParameter *params,
                                     size_t param_count,
                                     const FengTypeParam *type_params,
                                     size_t type_param_count,
                                     const char *path,
                                     FengToken token,
                                     FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < param_count; ++index) {
        FengSymbolParamView param = {0};

        param.token = params[index].token;
        param.mutability = normalize_mutability(params[index].mutability);
        param.is_variadic = params[index].is_variadic;
        param.name = feng_symbol_internal_dup_slice(params[index].name);
        param.type = build_type_from_type_ref_with_tparams(ctx,
                                                           params[index].type,
                                                           type_params,
                                                           type_param_count,
                                                           path,
                                                           token,
                                                           out_error);
        if ((params[index].name.data != NULL && param.name == NULL) ||
            (params[index].type != NULL && param.type == NULL)) {
            free(param.name);
            feng_symbol_internal_type_free(param.type);
            return feng_symbol_internal_set_error(out_error, path, token,
                                                  "out of memory building generic parameter signature");
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

/* Forward declaration - defined later. */
static bool append_member_decl(FengSymbolDeclView *owner,
                               FengSymbolDeclView *member,
                               const char *path,
                               FengToken token,
                               FengSymbolError *out_error);
static bool apply_decl_annotations(FengSymbolDeclView *decl,
                                   const FengSemanticModule *module,
                                   const FengAnnotation *annotations,
                                   size_t annotation_count,
                                   bool allow_library,
                                   const char *path,
                                   FengToken token,
                                   FengSymbolError *out_error);

/* Append TYPE_PARAM child decls and set type_param_count on the owner decl. */
static bool emit_type_param_children(const BuildContext *ctx,
                                     FengSymbolDeclView *decl,
                                     const FengTypeParam *type_params,
                                     size_t type_param_count,
                                     const char *path,
                                     FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < type_param_count; ++index) {
        char *name = feng_symbol_internal_dup_slice(type_params[index].name);
        FengSymbolDeclView *tp_decl;

        if (type_params[index].name.data != NULL && name == NULL) {
            return feng_symbol_internal_set_error(out_error, path, type_params[index].token,
                                                  "out of memory cloning type parameter name");
        }
        tp_decl = new_decl(FENG_SYMBOL_DECL_KIND_TYPE_PARAM,
                           FENG_VISIBILITY_PUBLIC,
                           FENG_MUTABILITY_LET,
                           name,
                           path,
                           type_params[index].token,
                           out_error);
        free(name);
        if (tp_decl == NULL) {
            return false;
        }
        if (type_params[index].constraint != NULL) {
            tp_decl->value_type = build_type_from_type_ref_with_tparams(ctx,
                                                                        type_params[index].constraint,
                                                                        type_params,
                                                                        type_param_count,
                                                                        path,
                                                                        type_params[index].token,
                                                                        out_error);
            if (tp_decl->value_type == NULL) {
                feng_symbol_internal_decl_free_members(tp_decl);
                free(tp_decl);
                return false;
            }
        }
        if (!append_member_decl(decl, tp_decl, path, type_params[index].token, out_error)) {
            feng_symbol_internal_decl_free_members(tp_decl);
            free(tp_decl);
            return false;
        }
    }

    decl->type_param_count = type_param_count;
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

static char *resolve_abi_annotation_string_arg(const FengSemanticModule *module,
                                               const FengAnnotation *annotation,
                                               size_t arg_index,
                                               const char *role,
                                               const char *path,
                                               FengToken token,
                                               FengSymbolError *out_error) {
    const FengExpr *arg;
    const FengDecl *binding_decl;

    if (annotation == NULL || arg_index >= annotation->arg_count) {
        return NULL;
    }

    arg = annotation->args[arg_index];
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
                                   role != NULL
                                       ? role
                                       : "extern callable annotation argument must resolve to a string literal");
    return NULL;
}

static bool field_is_bounded_decl(const FengTypeMember *member) {
    return member != NULL && member->kind == FENG_TYPE_MEMBER_FIELD &&
           !member->is_static &&
           normalize_mutability(member->as.field.mutability) == FENG_MUTABILITY_LET &&
           (member->as.field.initializer != NULL || member->as.field.declaration_bound);
}

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

static bool enum_item_value_fits_ft_range(int64_t value) {
    return value >= (int64_t)INT32_MIN && value <= (int64_t)INT32_MAX;
}

static FengSymbolDeclView *build_enum_item_decl(const char *path,
                                                FengVisibility visibility,
                                                const FengEnumItem *item,
                                                size_t ordinal,
                                                FengSymbolError *out_error) {
    int64_t value = item->has_explicit_value ? item->explicit_value : (int64_t)ordinal;
    FengSymbolDeclView *decl;

    if (!enum_item_value_fits_ft_range(value)) {
        feng_symbol_internal_set_error(out_error,
                                       path,
                                       item->token,
                                       "enum item '%.*s' value %lld is outside the .ft int32 range",
                                       (int)item->name.length,
                                       item->name.data != NULL ? item->name.data : "",
                                       (long long)value);
        return NULL;
    }

    decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_ENUM_ITEM,
                               visibility,
                               FENG_MUTABILITY_LET,
                               item->name,
                               path,
                               item->token,
                               out_error);
    if (decl == NULL) {
        return NULL;
    }
    decl->enum_item_ordinal = ordinal;
    decl->enum_item_value = value;
    decl->has_enum_item_value = true;
    return decl;
}

static FengSymbolDeclView *build_enum_decl(BuildContext *ctx,
                                           const char *path,
                                           const FengDecl *source_decl,
                                           FengSymbolError *out_error) {
    FengSymbolDeclView *decl;
    size_t index;

    decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_ENUM,
                               source_decl->visibility,
                               FENG_MUTABILITY_LET,
                               source_decl->as.enum_decl.name,
                               path,
                               source_decl->token,
                               out_error);
    if (decl == NULL) {
        return NULL;
    }
    if (!apply_decl_doc_comment(decl, source_decl->doc_comment, path, source_decl->token, out_error) ||
        !apply_decl_annotations(decl,
                                ctx->module,
                                source_decl->annotations,
                                source_decl->annotation_count,
                                false,
                                path,
                                source_decl->token,
                                out_error) ||
        !register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
        feng_symbol_internal_decl_free_members(decl);
        free(decl);
        return NULL;
    }

    for (index = 0U; index < source_decl->as.enum_decl.item_count; ++index) {
        FengSymbolDeclView *item_decl = build_enum_item_decl(path,
                                                             source_decl->visibility,
                                                             &source_decl->as.enum_decl.items[index],
                                                             index,
                                                             out_error);
        if (item_decl == NULL ||
            !append_member_decl(decl,
                                item_decl,
                                path,
                                source_decl->as.enum_decl.items[index].token,
                                out_error)) {
            if (item_decl != NULL && item_decl->owner == NULL) {
                feng_symbol_internal_decl_free_members(item_decl);
                free(item_decl);
            }
            feng_symbol_internal_decl_free_members(decl);
            free(decl);
            return NULL;
        }
    }

    return decl;
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

    decl->abi_annotated = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_ABI);
    decl->is_iterable = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_ITERABLE);
    decl->is_iterator = annotations_contain_kind(annotations, annotation_count, FENG_ANNOTATION_ITERATOR);
    if (callconv != NULL) {
        decl->calling_convention = callconv->builtin_kind;
        if (allow_library && callconv->arg_count > 0U) {
            decl->abi_library = resolve_abi_annotation_string_arg(
                module,
                callconv,
                0U,
                "extern callable annotation library must resolve to a string literal",
                path,
                token,
                out_error);
            if (decl->abi_library == NULL) {
                return false;
            }
            if (callconv->arg_count > 1U) {
                decl->abi_symbol = resolve_abi_annotation_string_arg(
                    module,
                    callconv,
                    1U,
                    "extern callable annotation C function name must resolve to a string literal",
                    path,
                    token,
                    out_error);
                if (decl->abi_symbol == NULL) {
                    return false;
                }
            }
        }
    }
    return true;
}

static FengSymbolTypeView *build_site_type(const BuildContext *ctx,
                                           const void *site,
                                           const FengTypeRef *explicit_type,
                                           const char *path,
                                           FengToken token,
                                           FengSymbolError *out_error) {
    const FengSemanticTypeFact *fact;

    if (explicit_type != NULL) {
        return build_type_from_type_ref(ctx, explicit_type, path, token, out_error);
    }

    fact = feng_semantic_lookup_type_fact(ctx != NULL ? ctx->analysis : NULL, site);
    if (fact == NULL) {
        feng_symbol_internal_set_error(out_error, path, token, "missing semantic type fact for exported declaration");
        return NULL;
    }

    return build_type_from_fact(ctx, fact, path, token, out_error);
}

static FengSymbolTypeView *build_callable_return_type_with_tparams(
    const BuildContext *ctx,
    const void *site,
    const FengTypeRef *explicit_type,
    bool fallback_void,
    const FengTypeParam *type_params,
    size_t type_param_count,
    const char *path,
    FengToken token,
    FengSymbolError *out_error) {
    FengSymbolTypeView *type;

    if (explicit_type != NULL) {
        type = build_type_from_type_ref_with_tparams(ctx,
                                                     explicit_type,
                                                     type_params,
                                                     type_param_count,
                                                     path,
                                                     token,
                                                     out_error);
        return type;
    }
    /* Fall back to the non-tparam path for inferred return types. */
    type = build_site_type(ctx, site, explicit_type, path, token, out_error);
    if (type != NULL || !fallback_void) {
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

static FengSymbolTypeView *build_site_type_with_tparams(const BuildContext *ctx,
                                                        const void *site,
                                                        const FengTypeRef *explicit_type,
                                                        const FengTypeParam *type_params,
                                                        size_t type_param_count,
                                                        const char *path,
                                                        FengToken token,
                                                        FengSymbolError *out_error) {
    if (explicit_type != NULL) {
        return build_type_from_type_ref_with_tparams(ctx,
                                                     explicit_type,
                                                     type_params,
                                                     type_param_count,
                                                     path,
                                                     token,
                                                     out_error);
    }
    return build_site_type(ctx, site, explicit_type, path, token, out_error);
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

static const FengDecl *find_local_source_type_decl(const BuildContext *ctx,
                                                   const FengTypeRef *type_ref) {
    FengSlice name;
    size_t program_index;

    if (ctx == NULL || ctx->module == NULL ||
        !named_type_targets_current_module(ctx, type_ref)) {
        return NULL;
    }
    name = named_type_leaf_name(type_ref);
    for (program_index = 0U; program_index < ctx->module->program_count; ++program_index) {
        const FengProgram *program = ctx->module->programs[program_index];
        size_t decl_index;

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];

            if (decl->kind == FENG_DECL_TYPE &&
                feng_symbol_internal_slice_equals(decl->as.type_decl.name, name)) {
                return decl;
            }
        }
    }
    return NULL;
}

/* For `fit T[]` / `fit T[!]` targets, infer the fit-local array element type
 * parameter name when the element is a single unresolved non-builtin name. */
static bool infer_fit_array_target_type_param_name(const BuildContext *ctx,
                                                   const FengTypeRef *target_ref,
                                                   FengSlice *out_name) {
    const FengTypeRef *cursor = target_ref;
    bool has_array_layer = false;

    if (ctx == NULL || target_ref == NULL || out_name == NULL) {
        return false;
    }
    while (cursor != NULL && cursor->kind == FENG_TYPE_REF_ARRAY) {
        has_array_layer = true;
        cursor = cursor->as.inner;
    }
    if (!has_array_layer || cursor == NULL || cursor->kind != FENG_TYPE_REF_NAMED) {
        return false;
    }
    if (cursor->as.named.segment_count != 1U || cursor->as.named.type_arg_count != 0U) {
        return false;
    }
    if (canonical_builtin_name(cursor->as.named.segments[0]) != NULL) {
        return false;
    }
    if (find_local_type_like_decl(ctx, cursor) != NULL) {
        return false;
    }

    *out_name = cursor->as.named.segments[0];
    return true;
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

        case FENG_STMT_BINDING:
        case FENG_STMT_EXPR:
        case FENG_STMT_TRY:
        case FENG_STMT_RETURN:
        case FENG_STMT_THROW:
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
        case FENG_STMT_DEFER:
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
    *out_names = NULL;
    *out_count = 0U;

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
    FengVisibility member_visibility =
        owner_source_decl != NULL && owner_source_decl->kind == FENG_DECL_SPEC
            ? FENG_VISIBILITY_PUBLIC
            : member->visibility;

    if (owner_source_decl != NULL &&
        owner_source_decl->kind == FENG_DECL_TYPE &&
        owner_source_decl->as.type_decl.is_tuple &&
        member_visibility == FENG_VISIBILITY_DEFAULT) {
        member_visibility = owner_source_decl->visibility;
    }

    switch (member->kind) {
        case FENG_TYPE_MEMBER_FIELD:
            decl = new_decl_from_slice(FENG_SYMBOL_DECL_KIND_FIELD,
                                       member_visibility,
                                       member->as.field.mutability,
                                       member->as.field.name,
                                       path,
                                       member->token,
                                       out_error);
            if (decl == NULL) {
                return NULL;
            }
            if (!apply_decl_doc_comment(decl, member->doc_comment, path, member->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            decl->value_type = build_site_type_with_tparams(ctx,
                                                            member,
                                                            member->as.field.type,
                                                            ctx->type_params,
                                                            ctx->type_param_count,
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
        case FENG_TYPE_MEMBER_FINALIZER: {
            const FengCallableSignature *sig = &member->as.callable;
            /* Merge owner-level and method-level type params so that
             * method-level generic parameters (e.g. func foo<T>(...))
             * are correctly serialized as TYPE_PARAM_REF in the .ft file. */
            const FengTypeParam *effective_tparams = ctx->type_params;
            size_t effective_tparam_count = ctx->type_param_count;
            FengTypeParam *merged_tparams = NULL;

            if (sig->type_param_count > 0U) {
                size_t total = ctx->type_param_count + sig->type_param_count;
                merged_tparams = (FengTypeParam *)calloc(total, sizeof(FengTypeParam));
                if (merged_tparams == NULL) {
                    return NULL;
                }
                for (size_t ti = 0; ti < ctx->type_param_count; ++ti) {
                    merged_tparams[ti] = ctx->type_params[ti];
                }
                for (size_t ti = 0; ti < sig->type_param_count; ++ti) {
                    merged_tparams[ctx->type_param_count + ti] = sig->type_params[ti];
                }
                effective_tparams = merged_tparams;
                effective_tparam_count = total;
            }

            decl = new_decl_from_slice(member->kind == FENG_TYPE_MEMBER_METHOD
                                           ? FENG_SYMBOL_DECL_KIND_METHOD
                                           : member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR
                                                 ? FENG_SYMBOL_DECL_KIND_CONSTRUCTOR
                                                 : FENG_SYMBOL_DECL_KIND_FINALIZER,
                                       member_visibility,
                                       FENG_MUTABILITY_LET,
                                       sig->name,
                                       path,
                                       member->token,
                                       out_error);
            if (decl == NULL) {
                free(merged_tparams);
                return NULL;
            }
            if (!apply_decl_doc_comment(decl, member->doc_comment, path, member->token, out_error)) {
                free(merged_tparams);
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            /* Emit method-level type param children before params/return type. */
            if (sig->type_param_count > 0U) {
                if (!emit_type_param_children(ctx, decl,
                                              sig->type_params,
                                              sig->type_param_count,
                                              path,
                                              out_error)) {
                    free(merged_tparams);
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
            }
            decl->return_type = build_callable_return_type_with_tparams(ctx,
                                                                        sig,
                                                                        sig->return_type,
                                                                        true,
                                                                        effective_tparams,
                                                                        effective_tparam_count,
                                                                        path,
                                                                        member->token,
                                                                        out_error);
            if (decl->return_type == NULL) {
                free(merged_tparams);
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (!fill_params_with_tparams(ctx, decl,
                                          sig->params,
                                          sig->param_count,
                                          effective_tparams,
                                          effective_tparam_count,
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
                free(merged_tparams);
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            free(merged_tparams);
            break;
        }
    }

            decl->is_static = member->is_static;

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
            if (!apply_decl_doc_comment(decl, source_decl->doc_comment, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            decl->value_type = build_site_type(ctx,
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
            if (!apply_decl_doc_comment(decl, source_decl->doc_comment, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            decl->is_tuple = source_decl->as.type_decl.is_tuple;
            if (!fill_declared_specs(ctx, decl,
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
            /* Emit generic type parameter children before member fields. */
            if (source_decl->as.type_decl.type_param_count > 0U) {
                if (!emit_type_param_children(ctx, decl,
                                              source_decl->as.type_decl.type_params,
                                              source_decl->as.type_decl.type_param_count,
                                              path,
                                              out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
                ctx->type_params = source_decl->as.type_decl.type_params;
                ctx->type_param_count = source_decl->as.type_decl.type_param_count;
            }
            if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
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
                    ctx->type_params = NULL;
                    ctx->type_param_count = 0U;
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
            }
            if (!fill_reifiable_deps(ctx, decl, source_decl,
                                     ctx->type_params,
                                     ctx->type_param_count,
                                     path,
                                     source_decl->token,
                                     out_error)) {
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            ctx->type_params = NULL;
            ctx->type_param_count = 0U;
            return decl;

        case FENG_DECL_ENUM:
            return build_enum_decl(ctx, path, source_decl, out_error);

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
            decl->spec_form = source_decl->as.spec_decl.form;
            if (!apply_decl_doc_comment(decl, source_decl->doc_comment, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (!fill_declared_specs(ctx, decl,
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
            /* Emit generic type parameter children for the spec. */
            if (source_decl->as.spec_decl.type_param_count > 0U) {
                if (!emit_type_param_children(ctx, decl,
                                              source_decl->as.spec_decl.type_params,
                                              source_decl->as.spec_decl.type_param_count,
                                              path,
                                              out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
                ctx->type_params = source_decl->as.spec_decl.type_params;
                ctx->type_param_count = source_decl->as.spec_decl.type_param_count;
            }
            if (source_decl->as.spec_decl.form == FENG_SPEC_FORM_UNION) {
                if (!fill_union_members_with_tparams(ctx, decl,
                                                     feng_semantic_lookup_union_spec_info(ctx->analysis,
                                                                                         source_decl),
                                                     ctx->type_params,
                                                     ctx->type_param_count,
                                                     path,
                                                     source_decl->token,
                                                     out_error)) {
                    ctx->type_params = NULL;
                    ctx->type_param_count = 0U;
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
            } else if (source_decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                decl->return_type = build_callable_return_type_with_tparams(ctx,
                                                                            source_decl,
                                                                            source_decl->as.spec_decl.as.callable.return_type,
                                                                            true,
                                                                            ctx->type_params,
                                                                            ctx->type_param_count,
                                                                            path,
                                                                            source_decl->token,
                                                                            out_error);
                if (decl->return_type == NULL ||
                    !fill_params_with_tparams(ctx, decl,
                                              source_decl->as.spec_decl.as.callable.params,
                                              source_decl->as.spec_decl.as.callable.param_count,
                                              ctx->type_params,
                                              ctx->type_param_count,
                                              path,
                                              source_decl->token,
                                              out_error)) {
                    ctx->type_params = NULL;
                    ctx->type_param_count = 0U;
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
            } else {
                if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
                    ctx->type_params = NULL;
                    ctx->type_param_count = 0U;
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
                        ctx->type_params = NULL;
                        ctx->type_param_count = 0U;
                        feng_symbol_internal_decl_free_members(decl);
                        free(decl);
                        return NULL;
                    }
                }
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
                return decl;
            }
            break;

        case FENG_DECL_FIT: {
            char *name = fit_display_name(source_decl->as.fit_decl.target);
            const FengDecl *fit_target_source = find_local_source_type_decl(ctx,
                                                                            source_decl->as.fit_decl.target);
            FengTypeParam inferred_fit_target_type_param = {0};
            FengSlice inferred_fit_target_type_param_name = {0};
            const FengTypeParam *fit_type_params = fit_target_source != NULL
                ? fit_target_source->as.type_decl.type_params
                : NULL;
            size_t fit_type_param_count = fit_target_source != NULL
                ? fit_target_source->as.type_decl.type_param_count
                : 0U;

            if (fit_type_param_count == 0U &&
                infer_fit_array_target_type_param_name(ctx,
                                                       source_decl->as.fit_decl.target,
                                                       &inferred_fit_target_type_param_name)) {
                inferred_fit_target_type_param.token = source_decl->token;
                inferred_fit_target_type_param.name = inferred_fit_target_type_param_name;
                inferred_fit_target_type_param.constraint = NULL;
                fit_type_params = &inferred_fit_target_type_param;
                fit_type_param_count = 1U;
            }
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
            if (!apply_decl_doc_comment(decl, source_decl->doc_comment, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            ctx->type_params = fit_type_params;
            ctx->type_param_count = fit_type_param_count;
            decl->fit_target = build_type_from_type_ref_with_tparams(ctx,
                                                                     source_decl->as.fit_decl.target,
                                                                     fit_type_params,
                                                                     fit_type_param_count,
                                                                     path,
                                                                     source_decl->token,
                                                                     out_error);
            if (decl->fit_target == NULL ||
                !fill_declared_specs_with_tparams(ctx,
                    decl,
                    (const FengTypeRef *const *)source_decl->as.fit_decl.specs,
                    source_decl->as.fit_decl.spec_count,
                    fit_type_params,
                    fit_type_param_count,
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
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            if (!register_source_decl(ctx, source_decl, decl, path, source_decl->token, out_error)) {
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
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
                    ctx->type_params = NULL;
                    ctx->type_param_count = 0U;
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
            }
            if (!fill_reifiable_deps(ctx, decl, source_decl,
                                     ctx->type_params,
                                     ctx->type_param_count,
                                     path,
                                     source_decl->token,
                                     out_error)) {
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            ctx->type_params = NULL;
            ctx->type_param_count = 0U;
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
            if (!apply_decl_doc_comment(decl, source_decl->doc_comment, path, source_decl->token, out_error)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            decl->is_extern = source_decl->is_extern;
            /* Emit type parameter children if this is a generic function. */
            if (source_decl->as.function_decl.type_param_count > 0U) {
                if (!emit_type_param_children(ctx, decl,
                                              source_decl->as.function_decl.type_params,
                                              source_decl->as.function_decl.type_param_count,
                                              path,
                                              out_error)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return NULL;
                }
                ctx->type_params = source_decl->as.function_decl.type_params;
                ctx->type_param_count = source_decl->as.function_decl.type_param_count;
            }
            decl->return_type = build_callable_return_type_with_tparams(ctx,
                                                                        &source_decl->as.function_decl,
                                                                        source_decl->as.function_decl.return_type,
                                                                        true,
                                                                        ctx->type_params,
                                                                        ctx->type_param_count,
                                                                        path,
                                                                        source_decl->token,
                                                                        out_error);
            if (decl->return_type == NULL ||
                !fill_params_with_tparams(ctx, decl,
                                          source_decl->as.function_decl.params,
                                          source_decl->as.function_decl.param_count,
                                          ctx->type_params,
                                          ctx->type_param_count,
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
                                        out_error) ||
                !fill_reifiable_deps(ctx, decl, source_decl,
                                     ctx->type_params,
                                     ctx->type_param_count,
                                     path,
                                     source_decl->token,
                                     out_error)) {
                ctx->type_params = NULL;
                ctx->type_param_count = 0U;
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return NULL;
            }
            ctx->type_params = NULL;
            ctx->type_param_count = 0U;
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
                !field_decl->is_static && field_decl->mutability == FENG_MUTABILITY_LET &&
                !field_decl->bounded_decl &&
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
        const FengDecl *subject_type_decl =
            (relation->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL)
                ? relation->subject_key.as.type_decl : NULL;
        FengSymbolDeclView *left_type = find_source_decl(ctx->source_map, ctx->source_count, subject_type_decl);
        FengSymbolDeclView *right_spec = find_source_decl(ctx->source_map, ctx->source_count, relation->spec_decl);
        size_t source_index;

        if (left_type != NULL && right_spec != NULL &&
            !append_unique_relation(ctx,
                                    FENG_SYMBOL_RELATION_TYPE_IMPLEMENTS_SPEC,
                                    left_type,
                                    right_spec,
                                    left_type,
                                    subject_type_decl->token,
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
                                        source->via_fit_decl != NULL ? source->via_fit_decl->token : (subject_type_decl != NULL ? subject_type_decl->token : source->via_fit_decl->token),
                                        out_error)) {
                return false;
            }
            if (fit_decl != NULL && left_type != NULL &&
                !append_unique_relation(ctx,
                                        FENG_SYMBOL_RELATION_FIT_EXTENDS_TYPE,
                                        fit_decl,
                                        left_type,
                                        fit_decl,
                                        source->via_fit_decl != NULL ? source->via_fit_decl->token : (subject_type_decl != NULL ? subject_type_decl->token : source->via_fit_decl->token),
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

        ctx.current_program = program;
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
        if (analysis->modules[module_index].origin ==
            FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
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

