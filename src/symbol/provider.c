#include "symbol/provider.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "archive/zip.h"
#include "symbol/ft.h"
#include "symbol/internal.h"

typedef enum ProviderDuplicatePolicy {
    PROVIDER_DUPLICATE_PREFER_PROFILE,
    PROVIDER_DUPLICATE_REJECT
} ProviderDuplicatePolicy;

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

static bool cstr_equals_buffer(const char *text, const char *buffer, size_t length) {
    return text != NULL && strlen(text) == length && memcmp(text, buffer, length) == 0;
}

static bool path_has_ft_extension(const char *path) {
    size_t length = strlen(path);

    return length >= 3U && strcmp(path + length - 3U, ".ft") == 0;
}

static bool bundle_entry_is_public_ft(const FengZipEntryInfo *entry) {
    return entry != NULL && !entry->is_directory && strncmp(entry->path, "mod/", 4U) == 0 &&
           entry->path[4] != '\0' && path_has_ft_extension(entry->path);
}

static char *module_name_dup(const FengSymbolModuleGraph *module) {
    size_t index;
    size_t total = 0U;
    char *name;
    size_t cursor = 0U;

    if (module == NULL || module->segment_count == 0U) {
        return feng_symbol_internal_dup_cstr("<anonymous>");
    }
    for (index = 0U; index < module->segment_count; ++index) {
        if (module->segments[index] == NULL) {
            return feng_symbol_internal_dup_cstr("<invalid>");
        }
        total += strlen(module->segments[index]);
        if (index + 1U < module->segment_count) {
            ++total;
        }
    }
    name = (char *)malloc(total + 1U);
    if (name == NULL) {
        return NULL;
    }
    for (index = 0U; index < module->segment_count; ++index) {
        size_t length = strlen(module->segments[index]);
        memcpy(name + cursor, module->segments[index], length);
        cursor += length;
        if (index + 1U < module->segment_count) {
            name[cursor++] = '.';
        }
    }
    name[cursor] = '\0';
    return name;
}

static char *bundle_entry_label_dup(const char *bundle_path, const char *entry_path) {
    size_t bundle_length = strlen(bundle_path);
    size_t entry_length = strlen(entry_path);
    char *label = (char *)malloc(bundle_length + entry_length + 2U);

    if (label == NULL) {
        return NULL;
    }
    memcpy(label, bundle_path, bundle_length);
    label[bundle_length] = '!';
    memcpy(label + bundle_length + 1U, entry_path, entry_length);
    label[bundle_length + entry_length + 1U] = '\0';
    return label;
}

static bool module_matches_bundle_entry(const FengSymbolModuleGraph *module, const char *entry_path) {
    size_t path_length = strlen(entry_path);
    const char *cursor;
    const char *limit;
    size_t segment_index = 0U;

    if (module == NULL || module->segment_count == 0U || path_length <= 7U ||
        strncmp(entry_path, "mod/", 4U) != 0 || !path_has_ft_extension(entry_path)) {
        return false;
    }
    cursor = entry_path + 4U;
    limit = entry_path + path_length - 3U;
    while (cursor < limit) {
        const char *slash = memchr(cursor, '/', (size_t)(limit - cursor));
        const char *segment_end = slash != NULL ? slash : limit;
        size_t segment_length = (size_t)(segment_end - cursor);

        if (segment_length == 0U || segment_index >= module->segment_count ||
            !cstr_equals_buffer(module->segments[segment_index], cursor, segment_length)) {
            return false;
        }
        ++segment_index;
        if (slash == NULL) {
            break;
        }
        cursor = slash + 1U;
    }
    return segment_index == module->segment_count;
}

static bool provider_reserve_modules(FengSymbolProvider *provider,
                                     size_t min_capacity,
                                     FengSymbolError *out_error) {
    FengSymbolImportedModule *grown;
    size_t new_capacity;

    if (provider->module_capacity >= min_capacity) {
        return true;
    }
    new_capacity = provider->module_capacity == 0U ? 8U : provider->module_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2U;
    }
    grown = (FengSymbolImportedModule *)realloc(provider->modules, new_capacity * sizeof(*grown));
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              NULL,
                                              (FengToken){0},
                                              "out of memory growing imported module table");
    }
    provider->modules = grown;
    provider->module_capacity = new_capacity;
    return true;
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

static bool module_matches_module(const FengSymbolImportedModule *existing,
                                  const FengSymbolModuleGraph *incoming) {
    size_t index;

    if (existing == NULL || existing->module == NULL || incoming == NULL ||
        existing->module->segment_count != incoming->segment_count) {
        return false;
    }
    for (index = 0U; index < incoming->segment_count; ++index) {
        const char *lhs = existing->module->segments[index];
        const char *rhs = incoming->segments[index];
        if (lhs == NULL || rhs == NULL || strcmp(lhs, rhs) != 0) {
            return false;
        }
    }
    return true;
}

static bool provider_append_module(FengSymbolProvider *provider,
                                   FengSymbolImportedModule incoming,
                                   ProviderDuplicatePolicy duplicate_policy,
                                   FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < provider->module_count; ++index) {
        if (module_matches_module(&provider->modules[index], incoming.module)) {
            if (duplicate_policy == PROVIDER_DUPLICATE_REJECT) {
                char *module_name = module_name_dup(incoming.module);
                const char *existing_source = provider->modules[index].source_path != NULL
                                                  ? provider->modules[index].source_path
                                                  : "<unknown>";
                const char *incoming_source = incoming.source_path != NULL ? incoming.source_path : "<unknown>";
                bool ok;

                if (module_name == NULL) {
                    feng_symbol_internal_imported_module_free(&incoming);
                    return feng_symbol_internal_set_error(out_error,
                                                          NULL,
                                                          (FengToken){0},
                                                          "out of memory reporting duplicate imported module");
                }
                ok = feng_symbol_internal_set_error(out_error,
                                                    NULL,
                                                    (FengToken){0},
                                                    "duplicate imported module '%s' from '%s' and '%s'",
                                                    module_name,
                                                    existing_source,
                                                    incoming_source);
                free(module_name);
                feng_symbol_internal_imported_module_free(&incoming);
                return ok;
            }
            if (incoming.profile >= provider->modules[index].profile) {
                feng_symbol_internal_imported_module_free(&provider->modules[index]);
                provider->modules[index] = incoming;
            } else {
                feng_symbol_internal_imported_module_free(&incoming);
            }
            return true;
        }
    }

    if (!provider_reserve_modules(provider, provider->module_count + 1U, out_error)) {
        feng_symbol_internal_imported_module_free(&incoming);
        return false;
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

static size_t module_public_decl_count(const FengSymbolImportedModule *module) {
    size_t index;
    size_t count = 0U;

    if (module == NULL || module->module == NULL) {
        return 0U;
    }
    for (index = 0U; index < module->module->root_decl.member_count; ++index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[index];

        if (decl != NULL && decl->visibility == FENG_VISIBILITY_PUBLIC) {
            ++count;
        }
    }
    return count;
}

static const FengSymbolDeclView *module_public_decl_at(const FengSymbolImportedModule *module,
                                                       size_t index) {
    size_t cursor = 0U;
    size_t member_index;

    if (module == NULL || module->module == NULL) {
        return NULL;
    }
    for (member_index = 0U; member_index < module->module->root_decl.member_count; ++member_index) {
        const FengSymbolDeclView *decl = module->module->root_decl.members[member_index];

        if (decl == NULL || decl->visibility != FENG_VISIBILITY_PUBLIC) {
            continue;
        }
        if (cursor == index) {
            return decl;
        }
        ++cursor;
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
                                        ProviderDuplicatePolicy duplicate_policy,
                                        const char *source_path,
                                        FengSymbolError *out_error) {
    size_t module_index;

    for (module_index = 0U; module_index < feng_symbol_graph_module_count(graph); ++module_index) {
        const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, module_index);
        FengSymbolImportedModule imported = {0};

        imported.profile = module->profile != 0 ? module->profile : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE;
        imported.module = feng_symbol_internal_module_clone(module, imported.profile, out_error);
        if (imported.module == NULL) {
            return false;
        }
        if (source_path != NULL) {
            imported.source_path = feng_symbol_internal_dup_cstr(source_path);
            if (imported.source_path == NULL) {
                feng_symbol_internal_imported_module_free(&imported);
                return feng_symbol_internal_set_error(out_error,
                                                      NULL,
                                                      (FengToken){0},
                                                      "out of memory recording imported module source");
            }
        }
        if (!feng_symbol_internal_imported_module_init_fit_views(&imported, out_error)) {
            feng_symbol_internal_imported_module_free(&imported);
            return false;
        }
        if (!provider_append_module(provider, imported, duplicate_policy, out_error)) {
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
                      provider_add_graph_internal(provider,
                                                  graph,
                                                  PROVIDER_DUPLICATE_PREFER_PROFILE,
                                                  root_path,
                                                  out_error);
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

static bool provider_set_zip_error(FengSymbolError *out_error,
                                   const char *bundle_path,
                                   const char *message,
                                   char *zip_error) {
    bool ok = feng_symbol_internal_set_error(out_error,
                                             bundle_path,
                                             (FengToken){0},
                                             "%s: %s",
                                             message,
                                             zip_error != NULL ? zip_error : "unknown archive error");
    free(zip_error);
    return ok;
}

static void provider_dispose_contents(FengSymbolProvider *provider) {
    size_t index;

    if (provider == NULL) {
        return;
    }
    for (index = 0U; index < provider->module_count; ++index) {
        feng_symbol_internal_imported_module_free(&provider->modules[index]);
    }
    free(provider->modules);
    memset(provider, 0, sizeof(*provider));
}

static bool provider_validate_bundle_graph(const FengSymbolGraph *graph,
                                           const char *bundle_path,
                                           const char *entry_path,
                                           FengSymbolError *out_error) {
    const FengSymbolModuleGraph *module;
    char *module_name;
    bool ok;

    if (feng_symbol_graph_module_count(graph) != 1U) {
        return feng_symbol_internal_set_error(out_error,
                                              bundle_path,
                                              (FengToken){0},
                                              "bundle entry '%s' must contain exactly one module symbol table",
                                              entry_path);
    }
    module = feng_symbol_graph_module_at(graph, 0U);
    if (module_matches_bundle_entry(module, entry_path)) {
        return true;
    }

    module_name = module_name_dup(module);
    if (module_name == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              bundle_path,
                                              (FengToken){0},
                                              "out of memory reporting bundle module path mismatch");
    }
    ok = feng_symbol_internal_set_error(out_error,
                                        bundle_path,
                                        (FengToken){0},
                                        "bundle entry '%s' declares module '%s'",
                                        entry_path,
                                        module_name);
    free(module_name);
    return ok;
}

static bool provider_load_bundle_entry(FengSymbolProvider *staging,
                                       const FengZipReader *reader,
                                       const char *bundle_path,
                                       const char *entry_path,
                                       FengSymbolError *out_error) {
    void *data = NULL;
    size_t data_size = 0U;
    char *zip_error = NULL;
    char *source_label = NULL;
    FengSymbolFtReadOptions options = {.expected_profile = FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC};
    FengSymbolGraph *graph = NULL;
    FengSymbolError entry_error = {0};
    bool ok;

    if (!feng_zip_reader_read(reader, entry_path, &data, &data_size, &zip_error)) {
        return provider_set_zip_error(out_error, bundle_path, "failed to read bundle symbol entry", zip_error);
    }

    source_label = bundle_entry_label_dup(bundle_path, entry_path);
    if (source_label == NULL) {
        feng_zip_free(data);
        return feng_symbol_internal_set_error(out_error,
                                              bundle_path,
                                              (FengToken){0},
                                              "out of memory describing bundle symbol entry");
    }

    if (!feng_symbol_ft_read_bytes(data, data_size, source_label, &options, &graph, &entry_error)) {
        ok = feng_symbol_internal_set_error(out_error,
                                            bundle_path,
                                            (FengToken){0},
                                            "failed to load bundle symbol entry '%s': %s",
                                            entry_path,
                                            entry_error.message != NULL ? entry_error.message : "invalid symbol table");
        feng_symbol_error_free(&entry_error);
        free(source_label);
        feng_zip_free(data);
        return ok;
    }
    feng_zip_free(data);

    if (!provider_validate_bundle_graph(graph, bundle_path, entry_path, out_error) ||
        !provider_add_graph_internal(staging,
                                     graph,
                                     PROVIDER_DUPLICATE_REJECT,
                                     source_label,
                                     out_error)) {
        feng_symbol_graph_free(graph);
        free(source_label);
        return false;
    }

    feng_symbol_graph_free(graph);
    free(source_label);
    return true;
}

static bool provider_merge_staged_modules(FengSymbolProvider *provider,
                                          FengSymbolProvider *staging,
                                          FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < staging->module_count; ++index) {
        size_t existing_index;

        for (existing_index = 0U; existing_index < provider->module_count; ++existing_index) {
            if (module_matches_module(&provider->modules[existing_index], staging->modules[index].module)) {
                char *module_name = module_name_dup(staging->modules[index].module);
                const char *existing_source = provider->modules[existing_index].source_path != NULL
                                                  ? provider->modules[existing_index].source_path
                                                  : "<unknown>";
                const char *incoming_source = staging->modules[index].source_path != NULL
                                                  ? staging->modules[index].source_path
                                                  : "<unknown>";
                bool ok;

                if (module_name == NULL) {
                    return feng_symbol_internal_set_error(out_error,
                                                          NULL,
                                                          (FengToken){0},
                                                          "out of memory reporting duplicate imported module");
                }
                ok = feng_symbol_internal_set_error(out_error,
                                                    NULL,
                                                    (FengToken){0},
                                                    "duplicate imported module '%s' from '%s' and '%s'",
                                                    module_name,
                                                    existing_source,
                                                    incoming_source);
                free(module_name);
                return ok;
            }
        }
    }

    if (!provider_reserve_modules(provider, provider->module_count + staging->module_count, out_error)) {
        return false;
    }
    for (index = 0U; index < staging->module_count; ++index) {
        provider->modules[provider->module_count++] = staging->modules[index];
        memset(&staging->modules[index], 0, sizeof(staging->modules[index]));
    }
    staging->module_count = 0U;
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
    return provider_add_graph_internal(provider,
                                       graph,
                                       PROVIDER_DUPLICATE_PREFER_PROFILE,
                                       NULL,
                                       out_error);
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

bool feng_symbol_provider_add_bundle(FengSymbolProvider *provider,
                                     const char *bundle_path,
                                     FengSymbolError *out_error) {
    FengZipReader reader = {0};
    FengSymbolProvider staging = {0};
    char *zip_error = NULL;
    size_t entry_count;
    size_t index;

    if (provider == NULL || bundle_path == NULL) {
        return false;
    }
    if (!feng_zip_reader_open(bundle_path, &reader, &zip_error)) {
        return provider_set_zip_error(out_error, bundle_path, "failed to open bundle", zip_error);
    }

    entry_count = feng_zip_reader_entry_count(&reader);
    for (index = 0U; index < entry_count; ++index) {
        FengZipEntryInfo entry;

        if (!feng_zip_reader_entry_at(&reader, index, &entry, &zip_error)) {
            provider_dispose_contents(&staging);
            feng_zip_reader_dispose(&reader);
            return provider_set_zip_error(out_error, bundle_path, "failed to inspect bundle entry", zip_error);
        }
        if (!bundle_entry_is_public_ft(&entry)) {
            continue;
        }
        if (!provider_load_bundle_entry(&staging, &reader, bundle_path, entry.path, out_error)) {
            provider_dispose_contents(&staging);
            feng_zip_reader_dispose(&reader);
            return false;
        }
    }

    feng_zip_reader_dispose(&reader);
    if (!provider_merge_staged_modules(provider, &staging, out_error)) {
        provider_dispose_contents(&staging);
        return false;
    }
    provider_dispose_contents(&staging);
    return true;
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

size_t feng_symbol_module_public_decl_count(const FengSymbolImportedModule *module) {
    return module_public_decl_count(module);
}

const FengSymbolDeclView *feng_symbol_module_public_decl_at(
    const FengSymbolImportedModule *module,
    size_t index) {
    return module_public_decl_at(module, index);
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
    if (provider == NULL) {
        return;
    }
    provider_dispose_contents(provider);
    free(provider);
}
