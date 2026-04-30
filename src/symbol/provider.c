#include "symbol/provider.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "symbol/ft.h"
#include "symbol/internal.h"

static FengSlice slice_from_cstr(const char *text) {
    FengSlice slice = {0};

    if (text != NULL) {
        slice.data = text;
        slice.length = strlen(text);
    }
    return slice;
}

static bool cstr_equals_slice(const char *text, FengSlice slice) {
    return text != NULL && strlen(text) == slice.length && memcmp(text, slice.data, slice.length) == 0;
}

static bool path_has_ft_extension(const char *path) {
    size_t length = strlen(path);

    return length >= 3U && strcmp(path + length - 3U, ".ft") == 0;
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

static bool module_matches_segments(const FengSymbolImportedModule *module,
                                    const FengSlice *segments,
                                    size_t segment_count) {
    size_t index;

    if (module == NULL || module->module == NULL || module->module->segment_count != segment_count) {
        return false;
    }
    for (index = 0U; index < segment_count; ++index) {
        if (!cstr_equals_slice(module->module->segments[index], segments[index])) {
            return false;
        }
    }
    return true;
}

static bool provider_append_module(FengSymbolProvider *provider,
                                   FengSymbolImportedModule incoming,
                                   FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < provider->module_count; ++index) {
        if (module_matches_segments(&provider->modules[index],
                                    (const FengSlice *)incoming.module->segments,
                                    incoming.module->segment_count)) {
            if (incoming.profile >= provider->modules[index].profile) {
                feng_symbol_internal_imported_module_free(&provider->modules[index]);
                provider->modules[index] = incoming;
            } else {
                feng_symbol_internal_imported_module_free(&incoming);
            }
            return true;
        }
    }

    if (provider->module_count == provider->module_capacity) {
        size_t new_capacity = provider->module_capacity == 0U ? 8U : provider->module_capacity * 2U;
        FengSymbolImportedModule *grown = (FengSymbolImportedModule *)realloc(
            provider->modules,
            new_capacity * sizeof(*grown));
        if (grown == NULL) {
            feng_symbol_internal_imported_module_free(&incoming);
            return feng_symbol_internal_set_error(out_error,
                                                  incoming.module != NULL ? incoming.module->primary_path : NULL,
                                                  (FengToken){0},
                                                  "out of memory growing imported module table");
        }
        provider->modules = grown;
        provider->module_capacity = new_capacity;
    }

    provider->modules[provider->module_count++] = incoming;
    return true;
}

static const FengSymbolDeclView *module_find_public_decl_by_kind(const FengSymbolImportedModule *module,
                                                                 FengSlice name,
                                                                 FengSymbolDeclKind kind) {
    size_t index;

    if (module == NULL || module->module == NULL) {
        return NULL;
    }
    for (index = 0U; index < module->module->root_decl.member_count; ++index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[index];
        if (decl->kind == kind && decl->visibility == FENG_VISIBILITY_PUBLIC &&
            cstr_equals_slice(decl->name, name)) {
            return decl;
        }
    }
    return NULL;
}

static size_t module_count_public_values(const FengSymbolImportedModule *module, FengSlice name) {
    size_t index;
    size_t count = 0U;

    if (module == NULL || module->module == NULL) {
        return 0U;
    }
    for (index = 0U; index < module->module->root_decl.member_count; ++index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[index];
        if (decl->visibility == FENG_VISIBILITY_PUBLIC && cstr_equals_slice(decl->name, name) &&
            (decl->kind == FENG_SYMBOL_DECL_KIND_BINDING || decl->kind == FENG_SYMBOL_DECL_KIND_FUNCTION)) {
            ++count;
        }
    }
    return count;
}

static const FengSymbolDeclView *module_public_value_at(const FengSymbolImportedModule *module,
                                                        FengSlice name,
                                                        size_t index) {
    size_t cursor = 0U;
    size_t member_index;

    if (module == NULL || module->module == NULL) {
        return NULL;
    }
    for (member_index = 0U; member_index < module->module->root_decl.member_count; ++member_index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[member_index];
        if (decl->visibility == FENG_VISIBILITY_PUBLIC && cstr_equals_slice(decl->name, name) &&
            (decl->kind == FENG_SYMBOL_DECL_KIND_BINDING || decl->kind == FENG_SYMBOL_DECL_KIND_FUNCTION)) {
            if (cursor == index) {
                return decl;
            }
            ++cursor;
        }
    }
    return NULL;
}

static size_t decl_count_public_members(const FengSymbolDeclView *owner, FengSlice name) {
    size_t index;
    size_t count = 0U;

    if (owner == NULL) {
        return 0U;
    }
    for (index = 0U; index < owner->member_count; ++index) {
        const FengSymbolDeclView *member = owner->members[index];
        if (member->visibility == FENG_VISIBILITY_PUBLIC && cstr_equals_slice(member->name, name)) {
            ++count;
        }
    }
    return count;
}

static const FengSymbolDeclView *decl_public_member_at(const FengSymbolDeclView *owner,
                                                       FengSlice name,
                                                       size_t index) {
    size_t cursor = 0U;
    size_t member_index;

    if (owner == NULL) {
        return NULL;
    }
    for (member_index = 0U; member_index < owner->member_count; ++member_index) {
        const FengSymbolDeclView *member = owner->members[member_index];
        if (member->visibility == FENG_VISIBILITY_PUBLIC && cstr_equals_slice(member->name, name)) {
            if (cursor == index) {
                return member;
            }
            ++cursor;
        }
    }
    return NULL;
}

static bool provider_add_graph_internal(FengSymbolProvider *provider,
                                        const FengSymbolGraph *graph,
                                        FengSymbolError *out_error) {
    size_t module_index;

    for (module_index = 0U; module_index < feng_symbol_graph_module_count(graph); ++module_index) {
        const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, module_index);
        FengSymbolImportedModule imported = {0};

        imported.profile = module->profile != 0 ? module->profile : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE;
        imported.module = feng_symbol_internal_module_clone(module, imported.profile, out_error);
        if (imported.module == NULL ||
            !feng_symbol_internal_imported_module_init_fit_views(&imported, out_error) ||
            !provider_append_module(provider, imported, out_error)) {
            if (imported.module != NULL) {
                feng_symbol_internal_imported_module_free(&imported);
            }
            return false;
        }
    }
    return true;
}

static bool provider_load_ft_tree(FengSymbolProvider *provider,
                                  const char *root_path,
                                  FengSymbolProfile profile,
                                  FengSymbolError *out_error) {
    DIR *dir;
    struct dirent *entry;

    dir = opendir(root_path);
    if (dir == NULL) {
        if (errno == ENOENT) {
            return true;
        }
        return feng_symbol_internal_set_error(out_error,
                                              root_path,
                                              (FengToken){0},
                                              "failed to open symbol root '%s': %s",
                                              root_path,
                                              strerror(errno));
    }

    while ((entry = readdir(dir)) != NULL) {
        char *path;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        path = path_join(root_path, entry->d_name);
        if (path == NULL) {
            closedir(dir);
            return feng_symbol_internal_set_error(out_error,
                                                  root_path,
                                                  (FengToken){0},
                                                  "out of memory scanning symbol root");
        }

        if (stat(path, &st) != 0) {
            free(path);
            closedir(dir);
            return feng_symbol_internal_set_error(out_error,
                                                  root_path,
                                                  (FengToken){0},
                                                  "failed to stat '%s': %s",
                                                  path,
                                                  strerror(errno));
        }

        if (S_ISDIR(st.st_mode)) {
            bool ok = provider_load_ft_tree(provider, path, profile, out_error);
            free(path);
            if (!ok) {
                closedir(dir);
                return false;
            }
            continue;
        }

        if (S_ISREG(st.st_mode) && path_has_ft_extension(path)) {
            FengSymbolFtReadOptions options = {.expected_profile = profile};
            FengSymbolGraph *graph = NULL;
            bool ok = feng_symbol_ft_read_file(path, &options, &graph, out_error) &&
                      provider_add_graph_internal(provider, graph, out_error);
            feng_symbol_graph_free(graph);
            free(path);
            if (!ok) {
                closedir(dir);
                return false;
            }
            continue;
        }

        free(path);
    }

    closedir(dir);
    return true;
}

bool feng_symbol_provider_create(FengSymbolProvider **out_provider,
                                 FengSymbolError *out_error) {
    if (out_provider == NULL) {
        return false;
    }
    *out_provider = (FengSymbolProvider *)calloc(1U, sizeof(**out_provider));
    if (*out_provider == NULL) {
        return feng_symbol_internal_set_error(out_error, NULL, (FengToken){0}, "out of memory allocating symbol provider");
    }
    return true;
}

bool feng_symbol_provider_add_graph(FengSymbolProvider *provider,
                                    const FengSymbolGraph *graph,
                                    FengSymbolError *out_error) {
    if (provider == NULL || graph == NULL) {
        return false;
    }
    return provider_add_graph_internal(provider, graph, out_error);
}

bool feng_symbol_provider_add_ft_root(FengSymbolProvider *provider,
                                      const char *root_path,
                                      FengSymbolProfile profile,
                                      FengSymbolError *out_error) {
    if (provider == NULL || root_path == NULL) {
        return false;
    }
    return provider_load_ft_tree(provider, root_path, profile, out_error);
}

const FengSymbolImportedModule *feng_symbol_provider_find_module(
    const FengSymbolProvider *provider,
    const FengSlice *segments,
    size_t segment_count) {
    size_t index;

    if (provider == NULL || segments == NULL || segment_count == 0U) {
        return NULL;
    }
    for (index = 0U; index < provider->module_count; ++index) {
        if (module_matches_segments(&provider->modules[index], segments, segment_count)) {
            return &provider->modules[index];
        }
    }
    return NULL;
}

const FengSymbolDeclView *feng_symbol_module_find_public_type(
    const FengSymbolImportedModule *module,
    FengSlice name) {
    return module_find_public_decl_by_kind(module, name, FENG_SYMBOL_DECL_KIND_TYPE);
}

const FengSymbolDeclView *feng_symbol_module_find_public_spec(
    const FengSymbolImportedModule *module,
    FengSlice name) {
    return module_find_public_decl_by_kind(module, name, FENG_SYMBOL_DECL_KIND_SPEC);
}

const FengSymbolDeclView *feng_symbol_module_find_public_value(
    const FengSymbolImportedModule *module,
    FengSlice name) {
    return feng_symbol_module_public_value_at(module, name, 0U);
}

size_t feng_symbol_module_public_value_count(const FengSymbolImportedModule *module,
                                             FengSlice name) {
    return module_count_public_values(module, name);
}

const FengSymbolDeclView *feng_symbol_module_public_value_at(
    const FengSymbolImportedModule *module,
    FengSlice name,
    size_t index) {
    return module_public_value_at(module, name, index);
}

const FengSymbolDeclView *feng_symbol_decl_find_public_member(
    const FengSymbolDeclView *owner,
    FengSlice name) {
    return feng_symbol_decl_public_member_at(owner, name, 0U);
}

size_t feng_symbol_decl_public_member_count(const FengSymbolDeclView *owner,
                                            FengSlice name) {
    return decl_count_public_members(owner, name);
}

const FengSymbolDeclView *feng_symbol_decl_public_member_at(const FengSymbolDeclView *owner,
                                                            FengSlice name,
                                                            size_t index) {
    return decl_public_member_at(owner, name, index);
}

size_t feng_symbol_module_fit_count(const FengSymbolImportedModule *module) {
    return module != NULL ? module->fit_count : 0U;
}

const FengSymbolFitView *feng_symbol_module_fit_at(const FengSymbolImportedModule *module,
                                                   size_t index) {
    if (module == NULL || index >= module->fit_count) {
        return NULL;
    }
    return &module->fits[index];
}

size_t feng_symbol_module_segment_count(const FengSymbolImportedModule *module) {
    return module != NULL && module->module != NULL ? module->module->segment_count : 0U;
}

FengSlice feng_symbol_module_segment_at(const FengSymbolImportedModule *module, size_t index) {
    if (module == NULL || module->module == NULL || index >= module->module->segment_count) {
        return (FengSlice){0};
    }
    return slice_from_cstr(module->module->segments[index]);
}

FengSymbolDeclKind feng_symbol_decl_kind(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->kind : FENG_SYMBOL_DECL_KIND_MODULE;
}

FengSlice feng_symbol_decl_name(const FengSymbolDeclView *decl) {
    return decl != NULL ? slice_from_cstr(decl->name) : (FengSlice){0};
}

FengVisibility feng_symbol_decl_visibility(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->visibility : FENG_VISIBILITY_PRIVATE;
}

FengMutability feng_symbol_decl_mutability(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->mutability : FENG_MUTABILITY_LET;
}

bool feng_symbol_decl_is_extern(const FengSymbolDeclView *decl) {
    return decl != NULL && decl->is_extern;
}

bool feng_symbol_decl_has_bounded_decl(const FengSymbolDeclView *decl) {
    return decl != NULL && decl->bounded_decl;
}

const FengSymbolTypeView *feng_symbol_decl_value_type(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->value_type : NULL;
}

const FengSymbolTypeView *feng_symbol_decl_return_type(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->return_type : NULL;
}

const FengSymbolTypeView *feng_symbol_decl_fit_target(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->fit_target : NULL;
}

size_t feng_symbol_decl_param_count(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->param_count : 0U;
}

FengSlice feng_symbol_decl_param_name(const FengSymbolDeclView *decl, size_t index) {
    if (decl == NULL || index >= decl->param_count) {
        return (FengSlice){0};
    }
    return slice_from_cstr(decl->params[index].name);
}

FengMutability feng_symbol_decl_param_mutability(const FengSymbolDeclView *decl, size_t index) {
    if (decl == NULL || index >= decl->param_count) {
        return FENG_MUTABILITY_LET;
    }
    return decl->params[index].mutability;
}

const FengSymbolTypeView *feng_symbol_decl_param_type(const FengSymbolDeclView *decl,
                                                      size_t index) {
    if (decl == NULL || index >= decl->param_count) {
        return NULL;
    }
    return decl->params[index].type;
}

size_t feng_symbol_decl_declared_spec_count(const FengSymbolDeclView *decl) {
    return decl != NULL ? decl->declared_spec_count : 0U;
}

const FengSymbolTypeView *feng_symbol_decl_declared_spec_at(const FengSymbolDeclView *decl,
                                                            size_t index) {
    if (decl == NULL || index >= decl->declared_spec_count) {
        return NULL;
    }
    return decl->declared_specs[index];
}

const FengSymbolDeclView *feng_symbol_fit_decl(const FengSymbolFitView *fit) {
    return fit != NULL ? fit->decl : NULL;
}

FengSymbolTypeKind feng_symbol_type_kind(const FengSymbolTypeView *type) {
    return type != NULL ? type->kind : FENG_SYMBOL_TYPE_KIND_INVALID;
}

FengSlice feng_symbol_type_builtin_name(const FengSymbolTypeView *type) {
    if (type == NULL || type->kind != FENG_SYMBOL_TYPE_KIND_BUILTIN) {
        return (FengSlice){0};
    }
    return slice_from_cstr(type->as.builtin.name);
}

size_t feng_symbol_type_segment_count(const FengSymbolTypeView *type) {
    return type != NULL && type->kind == FENG_SYMBOL_TYPE_KIND_NAMED ? type->as.named.segment_count : 0U;
}

FengSlice feng_symbol_type_segment_at(const FengSymbolTypeView *type, size_t index) {
    if (type == NULL || type->kind != FENG_SYMBOL_TYPE_KIND_NAMED ||
        index >= type->as.named.segment_count) {
        return (FengSlice){0};
    }
    return slice_from_cstr(type->as.named.segments[index]);
}

const FengSymbolTypeView *feng_symbol_type_inner(const FengSymbolTypeView *type) {
    if (type == NULL) {
        return NULL;
    }
    if (type->kind == FENG_SYMBOL_TYPE_KIND_POINTER) {
        return type->as.pointer.inner;
    }
    if (type->kind == FENG_SYMBOL_TYPE_KIND_ARRAY) {
        return type->as.array.element;
    }
    return NULL;
}

size_t feng_symbol_type_array_rank(const FengSymbolTypeView *type) {
    return type != NULL && type->kind == FENG_SYMBOL_TYPE_KIND_ARRAY ? type->as.array.rank : 0U;
}

bool feng_symbol_type_array_layer_writable(const FengSymbolTypeView *type, size_t layer_index) {
    return type != NULL && type->kind == FENG_SYMBOL_TYPE_KIND_ARRAY &&
           layer_index < type->as.array.rank && type->as.array.layer_writable[layer_index];
}

void feng_symbol_provider_free(FengSymbolProvider *provider) {
    size_t index;

    if (provider == NULL) {
        return;
    }
    for (index = 0U; index < provider->module_count; ++index) {
        feng_symbol_internal_imported_module_free(&provider->modules[index]);
    }
    free(provider->modules);
    free(provider);
}
