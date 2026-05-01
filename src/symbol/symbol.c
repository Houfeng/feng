#include "symbol/internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FENG_SYMBOL_FNV1A64_OFFSET 14695981039346656037ULL
#define FENG_SYMBOL_FNV1A64_PRIME 1099511628211ULL

typedef struct DeclClonePair {
    const FengSymbolDeclView *source;
    FengSymbolDeclView *clone;
} DeclClonePair;

static char *dup_buffer(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1U);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

char *feng_symbol_internal_dup_cstr(const char *text) {
    if (text == NULL) {
        return NULL;
    }
    return dup_buffer(text, strlen(text));
}

char *feng_symbol_internal_dup_slice(FengSlice slice) {
    if (slice.data == NULL) {
        return NULL;
    }
    return dup_buffer(slice.data, slice.length);
}

bool feng_symbol_internal_slice_equals(FengSlice lhs, FengSlice rhs) {
    return lhs.length == rhs.length &&
           (lhs.length == 0U || memcmp(lhs.data, rhs.data, lhs.length) == 0);
}

void feng_symbol_error_free(FengSymbolError *error) {
    if (error == NULL) {
        return;
    }
    free(error->message);
    error->message = NULL;
    error->path = NULL;
    memset(&error->token, 0, sizeof(error->token));
}

bool feng_symbol_internal_set_error(FengSymbolError *error,
                                    const char *path,
                                    FengToken token,
                                    const char *fmt,
                                    ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message;

    if (error == NULL) {
        return false;
    }

    feng_symbol_error_free(error);

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        error->path = path;
        error->token = token;
        error->message = feng_symbol_internal_dup_cstr("symbol operation failed");
        va_end(args_copy);
        return false;
    }

    message = (char *)malloc((size_t)needed + 1U);
    if (message == NULL) {
        error->path = path;
        error->token = token;
        error->message = feng_symbol_internal_dup_cstr("out of memory");
        va_end(args_copy);
        return false;
    }

    vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);

    error->path = path;
    error->token = token;
    error->message = message;
    return false;
}

uint64_t feng_symbol_internal_fnv1a64_extend(uint64_t seed, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t index;
    uint64_t hash = seed;

    for (index = 0U; index < length; ++index) {
        hash ^= (uint64_t)bytes[index];
        hash *= FENG_SYMBOL_FNV1A64_PRIME;
    }

    return hash;
}

uint64_t feng_symbol_internal_fnv1a64(const void *data, size_t length) {
    return feng_symbol_internal_fnv1a64_extend(FENG_SYMBOL_FNV1A64_OFFSET, data, length);
}

static void free_string_array(char **items, size_t count) {
    size_t index;

    if (items == NULL) {
        return;
    }

    for (index = 0U; index < count; ++index) {
        free(items[index]);
    }
    free(items);
}

void feng_symbol_internal_type_free(FengSymbolTypeView *type) {
    size_t index;

    if (type == NULL) {
        return;
    }

    switch (type->kind) {
        case FENG_SYMBOL_TYPE_KIND_BUILTIN:
            free(type->as.builtin.name);
            break;

        case FENG_SYMBOL_TYPE_KIND_NAMED:
            free_string_array(type->as.named.segments, type->as.named.segment_count);
            break;

        case FENG_SYMBOL_TYPE_KIND_POINTER:
            feng_symbol_internal_type_free(type->as.pointer.inner);
            break;

        case FENG_SYMBOL_TYPE_KIND_ARRAY:
            feng_symbol_internal_type_free(type->as.array.element);
            free(type->as.array.layer_writable);
            break;

        case FENG_SYMBOL_TYPE_KIND_INVALID:
        default:
            break;
    }

    (void)index;
    free(type);
}

static void decl_dispose(FengSymbolDeclView *decl, bool free_self) {
    size_t index;

    if (decl == NULL) {
        return;
    }

    free(decl->abi_library);
    free(decl->name);
    free(decl->path);
    feng_symbol_internal_type_free(decl->value_type);
    feng_symbol_internal_type_free(decl->return_type);
    feng_symbol_internal_type_free(decl->fit_target);

    for (index = 0U; index < decl->param_count; ++index) {
        free(decl->params[index].name);
        feng_symbol_internal_type_free(decl->params[index].type);
    }
    free(decl->params);

    for (index = 0U; index < decl->declared_spec_count; ++index) {
        feng_symbol_internal_type_free(decl->declared_specs[index]);
    }
    free(decl->declared_specs);

    for (index = 0U; index < decl->member_count; ++index) {
        decl_dispose(decl->members[index], true);
    }
    free(decl->members);

    memset(decl, 0, sizeof(*decl));
    if (free_self) {
        free(decl);
    }
}

void feng_symbol_internal_decl_free_members(FengSymbolDeclView *decl) {
    decl_dispose(decl, false);
}

static bool append_decl_clone_pair(DeclClonePair **pairs,
                                   size_t *pair_count,
                                   size_t *pair_capacity,
                                   const FengSymbolDeclView *source,
                                   FengSymbolDeclView *clone,
                                   FengSymbolError *out_error) {
    DeclClonePair *resized;

    if (*pair_count == *pair_capacity) {
        size_t new_capacity = *pair_capacity == 0U ? 16U : (*pair_capacity * 2U);
        resized = (DeclClonePair *)realloc(*pairs, new_capacity * sizeof(*resized));
        if (resized == NULL) {
            return feng_symbol_internal_set_error(out_error,
                                                  NULL,
                                                  (FengToken){0},
                                                  "out of memory growing declaration clone map");
        }
        *pairs = resized;
        *pair_capacity = new_capacity;
    }

    (*pairs)[*pair_count].source = source;
    (*pairs)[*pair_count].clone = clone;
    ++(*pair_count);
    return true;
}

static FengSymbolDeclView *find_decl_clone(DeclClonePair *pairs,
                                           size_t pair_count,
                                           const FengSymbolDeclView *source) {
    size_t index;

    for (index = 0U; index < pair_count; ++index) {
        if (pairs[index].source == source) {
            return pairs[index].clone;
        }
    }

    return NULL;
}

FengSymbolTypeView *feng_symbol_internal_type_clone(const FengSymbolTypeView *type,
                                                    FengSymbolError *out_error) {
    FengSymbolTypeView *clone;
    size_t index;

    if (type == NULL) {
        return NULL;
    }

    clone = (FengSymbolTypeView *)calloc(1U, sizeof(*clone));
    if (clone == NULL) {
        feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory cloning type");
        return NULL;
    }

    clone->kind = type->kind;
    switch (type->kind) {
        case FENG_SYMBOL_TYPE_KIND_BUILTIN:
            clone->as.builtin.name = feng_symbol_internal_dup_cstr(type->as.builtin.name);
            if (type->as.builtin.name != NULL && clone->as.builtin.name == NULL) {
                feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory cloning builtin type name");
                feng_symbol_internal_type_free(clone);
                return NULL;
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_NAMED:
            clone->as.named.segment_count = type->as.named.segment_count;
            if (clone->as.named.segment_count > 0U) {
                clone->as.named.segments = (char **)calloc(clone->as.named.segment_count,
                                                           sizeof(*clone->as.named.segments));
                if (clone->as.named.segments == NULL) {
                    feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory cloning named type segments");
                    feng_symbol_internal_type_free(clone);
                    return NULL;
                }
                for (index = 0U; index < clone->as.named.segment_count; ++index) {
                    clone->as.named.segments[index] =
                        feng_symbol_internal_dup_cstr(type->as.named.segments[index]);
                    if (clone->as.named.segments[index] == NULL) {
                        feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory cloning named type segment");
                        feng_symbol_internal_type_free(clone);
                        return NULL;
                    }
                }
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_POINTER:
            clone->as.pointer.inner = feng_symbol_internal_type_clone(type->as.pointer.inner,
                                                                      out_error);
            if (type->as.pointer.inner != NULL && clone->as.pointer.inner == NULL) {
                feng_symbol_internal_type_free(clone);
                return NULL;
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_ARRAY:
            clone->as.array.rank = type->as.array.rank;
            clone->as.array.element = feng_symbol_internal_type_clone(type->as.array.element,
                                                                      out_error);
            if (type->as.array.element != NULL && clone->as.array.element == NULL) {
                feng_symbol_internal_type_free(clone);
                return NULL;
            }
            if (clone->as.array.rank > 0U) {
                clone->as.array.layer_writable = (bool *)calloc(clone->as.array.rank,
                                                                sizeof(*clone->as.array.layer_writable));
                if (clone->as.array.layer_writable == NULL) {
                    feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory cloning array mutability bitmap");
                    feng_symbol_internal_type_free(clone);
                    return NULL;
                }
                memcpy(clone->as.array.layer_writable,
                       type->as.array.layer_writable,
                       clone->as.array.rank * sizeof(*clone->as.array.layer_writable));
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_INVALID:
        default:
            break;
    }

    return clone;
}

static FengSymbolDeclView *clone_decl_recursive(const FengSymbolDeclView *decl,
                                                FengSymbolDeclView *owner,
                                                DeclClonePair **pairs,
                                                size_t *pair_count,
                                                size_t *pair_capacity,
                                                FengSymbolError *out_error) {
    FengSymbolDeclView *clone;
    size_t index;

    if (decl == NULL) {
        return NULL;
    }

    clone = (FengSymbolDeclView *)calloc(1U, sizeof(*clone));
    if (clone == NULL) {
        feng_symbol_internal_set_error(out_error, decl->path, decl->token, "out of memory cloning declaration");
        return NULL;
    }

    *clone = *decl;
    clone->owner = owner;
    clone->abi_library = feng_symbol_internal_dup_cstr(decl->abi_library);
    clone->name = feng_symbol_internal_dup_cstr(decl->name);
    clone->path = feng_symbol_internal_dup_cstr(decl->path);
    clone->value_type = feng_symbol_internal_type_clone(decl->value_type, out_error);
    clone->return_type = feng_symbol_internal_type_clone(decl->return_type, out_error);
    clone->fit_target = feng_symbol_internal_type_clone(decl->fit_target, out_error);
    clone->params = NULL;
    clone->declared_specs = NULL;
    clone->members = NULL;

    if ((decl->abi_library != NULL && clone->abi_library == NULL) ||
        (decl->name != NULL && clone->name == NULL) ||
        (decl->path != NULL && clone->path == NULL) ||
        (decl->value_type != NULL && clone->value_type == NULL) ||
        (decl->return_type != NULL && clone->return_type == NULL) ||
        (decl->fit_target != NULL && clone->fit_target == NULL)) {
        decl_dispose(clone, true);
        return NULL;
    }

    if (!append_decl_clone_pair(pairs, pair_count, pair_capacity, decl, clone, out_error)) {
        decl_dispose(clone, true);
        return NULL;
    }

    if (decl->param_count > 0U) {
        clone->params = (FengSymbolParamView *)calloc(decl->param_count, sizeof(*clone->params));
        if (clone->params == NULL) {
            feng_symbol_internal_set_error(out_error, decl->path, decl->token, "out of memory cloning parameters");
            decl_dispose(clone, true);
            return NULL;
        }
        clone->param_count = decl->param_count;
        for (index = 0U; index < decl->param_count; ++index) {
            clone->params[index].token = decl->params[index].token;
            clone->params[index].mutability = decl->params[index].mutability;
            clone->params[index].name = feng_symbol_internal_dup_cstr(decl->params[index].name);
            clone->params[index].type = feng_symbol_internal_type_clone(decl->params[index].type,
                                                                        out_error);
            if ((decl->params[index].name != NULL && clone->params[index].name == NULL) ||
                (decl->params[index].type != NULL && clone->params[index].type == NULL)) {
                decl_dispose(clone, true);
                return NULL;
            }
        }
    }

    if (decl->declared_spec_count > 0U) {
        clone->declared_specs = (FengSymbolTypeView **)calloc(decl->declared_spec_count,
                                                              sizeof(*clone->declared_specs));
        if (clone->declared_specs == NULL) {
            feng_symbol_internal_set_error(out_error, decl->path, decl->token, "out of memory cloning declared spec list");
            decl_dispose(clone, true);
            return NULL;
        }
        clone->declared_spec_count = decl->declared_spec_count;
        for (index = 0U; index < decl->declared_spec_count; ++index) {
            clone->declared_specs[index] = feng_symbol_internal_type_clone(decl->declared_specs[index],
                                                                           out_error);
            if (decl->declared_specs[index] != NULL && clone->declared_specs[index] == NULL) {
                decl_dispose(clone, true);
                return NULL;
            }
        }
    }

    if (decl->member_count > 0U) {
        clone->members = (FengSymbolDeclView **)calloc(decl->member_count, sizeof(*clone->members));
        if (clone->members == NULL) {
            feng_symbol_internal_set_error(out_error, decl->path, decl->token, "out of memory cloning member list");
            decl_dispose(clone, true);
            return NULL;
        }
        clone->member_count = decl->member_count;
        for (index = 0U; index < decl->member_count; ++index) {
            clone->members[index] = clone_decl_recursive(decl->members[index],
                                                         clone,
                                                         pairs,
                                                         pair_count,
                                                         pair_capacity,
                                                         out_error);
            if (clone->members[index] == NULL) {
                decl_dispose(clone, true);
                return NULL;
            }
        }
    }

    return clone;
}

FengSymbolDeclView *feng_symbol_internal_decl_clone(const FengSymbolDeclView *decl,
                                                    FengSymbolError *out_error) {
    DeclClonePair *pairs = NULL;
    size_t pair_count = 0U;
    size_t pair_capacity = 0U;
    FengSymbolDeclView *clone = clone_decl_recursive(decl,
                                                     NULL,
                                                     &pairs,
                                                     &pair_count,
                                                     &pair_capacity,
                                                     out_error);

    free(pairs);
    return clone;
}

static bool copy_string_array(char ***out_items,
                              size_t *out_count,
                              char *const *items,
                              size_t count,
                              const char *path,
                              FengToken token,
                              FengSymbolError *out_error) {
    char **copy;
    size_t index;

    *out_items = NULL;
    *out_count = 0U;
    if (count == 0U) {
        return true;
    }

    copy = (char **)calloc(count, sizeof(*copy));
    if (copy == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning string array");
    }

    for (index = 0U; index < count; ++index) {
        copy[index] = feng_symbol_internal_dup_cstr(items[index]);
        if (items[index] != NULL && copy[index] == NULL) {
            free_string_array(copy, index);
            return feng_symbol_internal_set_error(out_error, path, token, "out of memory cloning string entry");
        }
    }

    *out_items = copy;
    *out_count = count;
    return true;
}

FengSymbolModuleGraph *feng_symbol_internal_module_clone(const FengSymbolModuleGraph *module,
                                                         FengSymbolProfile profile,
                                                         FengSymbolError *out_error) {
    FengSymbolModuleGraph *clone;
    DeclClonePair *pairs = NULL;
    size_t pair_count = 0U;
    size_t pair_capacity = 0U;
    size_t index;

    if (module == NULL) {
        return NULL;
    }

    clone = (FengSymbolModuleGraph *)calloc(1U, sizeof(*clone));
    if (clone == NULL) {
        feng_symbol_internal_set_error(out_error, module->primary_path, (FengToken){0}, "out of memory cloning module");
        return NULL;
    }

    clone->profile = profile;
    clone->visibility = module->visibility;
    clone->primary_path = feng_symbol_internal_dup_cstr(module->primary_path);
    if (module->primary_path != NULL && clone->primary_path == NULL) {
        feng_symbol_internal_set_error(out_error, module->primary_path, (FengToken){0}, "out of memory cloning module path");
        feng_symbol_internal_module_free(clone);
        return NULL;
    }

    if (!copy_string_array(&clone->segments,
                           &clone->segment_count,
                           module->segments,
                           module->segment_count,
                           module->primary_path,
                           (FengToken){0},
                           out_error) ||
        !copy_string_array(&clone->uses,
                           &clone->use_count,
                           module->uses,
                           module->use_count,
                           module->primary_path,
                           (FengToken){0},
                           out_error)) {
        feng_symbol_internal_module_free(clone);
        return NULL;
    }

    memset(&clone->root_decl, 0, sizeof(clone->root_decl));
    clone->root_decl.kind = module->root_decl.kind;
    clone->root_decl.visibility = module->root_decl.visibility;
    clone->root_decl.mutability = module->root_decl.mutability;
    clone->root_decl.is_extern = module->root_decl.is_extern;
    clone->root_decl.fixed_annotated = module->root_decl.fixed_annotated;
    clone->root_decl.bounded_decl = module->root_decl.bounded_decl;
    clone->root_decl.union_annotated = module->root_decl.union_annotated;
    clone->root_decl.has_doc = module->root_decl.has_doc;
    clone->root_decl.calling_convention = module->root_decl.calling_convention;
    clone->root_decl.token = module->root_decl.token;
    clone->root_decl.name = feng_symbol_internal_dup_cstr(module->root_decl.name);
    clone->root_decl.path = feng_symbol_internal_dup_cstr(module->root_decl.path);
    if ((module->root_decl.name != NULL && clone->root_decl.name == NULL) ||
        (module->root_decl.path != NULL && clone->root_decl.path == NULL) ||
        !append_decl_clone_pair(&pairs,
                                &pair_count,
                                &pair_capacity,
                                &module->root_decl,
                                &clone->root_decl,
                                out_error)) {
        feng_symbol_internal_module_free(clone);
        free(pairs);
        return NULL;
    }

    if (module->root_decl.member_count > 0U) {
        clone->root_decl.members = (FengSymbolDeclView **)calloc(module->root_decl.member_count,
                                                                 sizeof(*clone->root_decl.members));
        if (clone->root_decl.members == NULL) {
            feng_symbol_internal_set_error(out_error, module->primary_path, (FengToken){0}, "out of memory cloning top-level declaration list");
            feng_symbol_internal_module_free(clone);
            free(pairs);
            return NULL;
        }
        clone->root_decl.member_count = module->root_decl.member_count;
        for (index = 0U; index < module->root_decl.member_count; ++index) {
            clone->root_decl.members[index] = clone_decl_recursive(module->root_decl.members[index],
                                                                   &clone->root_decl,
                                                                   &pairs,
                                                                   &pair_count,
                                                                   &pair_capacity,
                                                                   out_error);
            if (clone->root_decl.members[index] == NULL) {
                feng_symbol_internal_module_free(clone);
                free(pairs);
                return NULL;
            }
        }
    }

    if (module->relation_count > 0U) {
        clone->relations = (FengSymbolRelation *)calloc(module->relation_count,
                                                        sizeof(*clone->relations));
        if (clone->relations == NULL) {
            feng_symbol_internal_set_error(out_error, module->primary_path, (FengToken){0}, "out of memory cloning relation table");
            feng_symbol_internal_module_free(clone);
            free(pairs);
            return NULL;
        }
        clone->relation_count = module->relation_count;
        for (index = 0U; index < module->relation_count; ++index) {
            clone->relations[index].kind = module->relations[index].kind;
            clone->relations[index].left = find_decl_clone(pairs, pair_count, module->relations[index].left);
            clone->relations[index].right = find_decl_clone(pairs, pair_count, module->relations[index].right);
            clone->relations[index].owner = find_decl_clone(pairs, pair_count, module->relations[index].owner);
        }
    }

    free(pairs);
    return clone;
}

void feng_symbol_internal_module_free(FengSymbolModuleGraph *module) {
    if (module == NULL) {
        return;
    }

    free(module->primary_path);
    free_string_array(module->segments, module->segment_count);
    free_string_array(module->uses, module->use_count);
    decl_dispose(&module->root_decl, false);
    free(module->relations);
    free(module);
}

void feng_symbol_internal_imported_module_free(FengSymbolImportedModule *module) {
    if (module == NULL) {
        return;
    }

    free(module->fits);
    module->fits = NULL;
    module->fit_count = 0U;
    free(module->source_path);
    module->source_path = NULL;
    feng_symbol_internal_module_free(module->module);
    module->module = NULL;
    module->profile = FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC;
}

bool feng_symbol_internal_imported_module_init_fit_views(FengSymbolImportedModule *module,
                                                         FengSymbolError *out_error) {
    size_t count = 0U;
    size_t index;
    size_t cursor = 0U;

    if (module == NULL || module->module == NULL) {
        return true;
    }

    free(module->fits);
    module->fits = NULL;
    module->fit_count = 0U;

    for (index = 0U; index < module->module->root_decl.member_count; ++index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[index];

        if (decl->kind == FENG_SYMBOL_DECL_KIND_FIT && decl->visibility == FENG_VISIBILITY_PUBLIC) {
            ++count;
        }
    }

    if (count == 0U) {
        return true;
    }

    module->fits = (FengSymbolFitView *)calloc(count, sizeof(*module->fits));
    if (module->fits == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              module->module->primary_path,
                                              (FengToken){0},
                                              "out of memory building fit view cache");
    }

    for (index = 0U; index < module->module->root_decl.member_count; ++index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[index];

        if (decl->kind == FENG_SYMBOL_DECL_KIND_FIT && decl->visibility == FENG_VISIBILITY_PUBLIC) {
            module->fits[cursor++].decl = decl;
        }
    }

    module->fit_count = count;
    return true;
}

bool feng_symbol_internal_graph_append_module(FengSymbolGraph *graph,
                                              FengSymbolModuleGraph *module,
                                              FengSymbolError *out_error) {
    FengSymbolModuleGraph **resized;
    size_t new_count;

    if (graph == NULL || module == NULL) {
        return false;
    }

    new_count = graph->module_count + 1U;
    resized = (FengSymbolModuleGraph **)realloc(graph->modules, new_count * sizeof(*resized));
    if (resized == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              module->primary_path,
                                              (FengToken){0},
                                              "out of memory growing module graph");
    }

    graph->modules = resized;
    graph->modules[graph->module_count] = module;
    graph->module_count = new_count;
    return true;
}

size_t feng_symbol_graph_module_count(const FengSymbolGraph *graph) {
    return graph != NULL ? graph->module_count : 0U;
}

const FengSymbolModuleGraph *feng_symbol_graph_module_at(const FengSymbolGraph *graph,
                                                         size_t index) {
    if (graph == NULL || index >= graph->module_count) {
        return NULL;
    }
    return graph->modules[index];
}

void feng_symbol_graph_free(FengSymbolGraph *graph) {
    size_t index;

    if (graph == NULL) {
        return;
    }

    for (index = 0U; index < graph->module_count; ++index) {
        feng_symbol_internal_module_free(graph->modules[index]);
    }
    free(graph->modules);
    free(graph);
}

