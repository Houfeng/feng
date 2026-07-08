#include "cli/compile/driver.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

#include "archive/fb.h"
#include "archive/zip.h"
#include "parser/parser.h"
#include "symbol/ft.h"
#include "symbol/provider.h"
#include "symbol/symbol.h"

/* --- small helpers ------------------------------------------------------- */

static bool path_exists(const char *path) {
    if (path == NULL) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

static char *str_dup_n(const char *s, size_t n) {
    char *out = malloc(n + 1U);
    if (out == NULL) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *str_dup_cstr(const char *s) {
    return str_dup_n(s, strlen(s));
}

static char *dup_printf(const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *out;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return NULL;
    }

    out = malloc((size_t)needed + 1U);
    if (out == NULL) {
        va_end(args_copy);
        return NULL;
    }

    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

/* Build a mkdtemp template under FENG_TEMP_DIR (falls back to /tmp). */
static char *make_temp_template(const char *suffix) {
    const char *base = getenv("FENG_TEMP_DIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    return dup_printf("%s/%s", base, suffix);
}

static char *path_join2(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    bool need_sep = (la > 0U && a[la - 1U] != '/');
    char *out = malloc(la + (need_sep ? 1U : 0U) + lb + 1U);
    if (out == NULL) return NULL;
    memcpy(out, a, la);
    size_t cursor = la;
    if (need_sep) out[cursor++] = '/';
    memcpy(out + cursor, b, lb);
    out[cursor + lb] = '\0';
    return out;
}

static char *path_join_host_static_library(const char *dir, const char *library_name) {
    char *file_name;
    char *path;

    file_name = feng_fb_host_static_library_file_name(library_name);
    if (file_name == NULL) {
        return NULL;
    }
    path = path_join2(dir, file_name);
    free(file_name);
    return path;
}

static char *host_runtime_dynamic_library_file_name(const char *library_name) {
    if (library_name == NULL || library_name[0] == '\0') {
        return NULL;
    }
#if defined(__APPLE__)
    return dup_printf("lib%s.dylib", library_name);
#elif defined(_WIN32)
    return dup_printf("%s.dll", library_name);
#elif defined(__linux__)
    return dup_printf("lib%s.so", library_name);
#else
    return NULL;
#endif
}

/* Strip the trailing path component, returning a malloc'd directory copy.
 * If the path has no separator, returns ".". */
static char *path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return str_dup_cstr(".");
    }
    size_t len = (size_t)(slash - path);
    if (len == 0U) {
        return str_dup_cstr("/");
    }
    return str_dup_n(path, len);
}

static void cleanup_empty_ir_dirs(const char *c_path) {
    char *ir_c_dir = path_dirname_dup(c_path);
    if (ir_c_dir == NULL) return;
    char *ir_dir = path_dirname_dup(ir_c_dir);
    if (ir_dir == NULL) {
        free(ir_c_dir);
        return;
    }

    if (rmdir(ir_c_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr,
                "warning: could not remove empty IR directory %s: %s\n",
                ir_c_dir, strerror(errno));
    }
    if (rmdir(ir_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr,
                "warning: could not remove empty IR directory %s: %s\n",
                ir_dir, strerror(errno));
    }

    free(ir_dir);
    free(ir_c_dir);
}

static char *replace_with_sibling_filename(const char *path, const char *filename) {
    char *dir = path_dirname_dup(path);
    char *out;

    if (dir == NULL) {
        return NULL;
    }
    out = path_join2(dir, filename);
    free(dir);
    return out;
}

static const char *path_basename(const char *path) {
    const char *slash;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool path_has_suffix(const char *path, const char *suffix) {
    size_t path_len;
    size_t suffix_len;

    if (path == NULL || suffix == NULL) {
        return false;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool set_errorf(char **out_error_message, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message;

    if (out_error_message == NULL) {
        return false;
    }

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return false;
    }

    message = malloc((size_t)needed + 1U);
    if (message == NULL) {
        va_end(args_copy);
        return false;
    }

    vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    *out_error_message = message;
    return false;
}

typedef struct BundleScanInfo {
    char *bundle_path;
    char *library_entry_path;
    char **module_names;
    size_t module_count;
    size_t module_capacity;
    char **uses;
    size_t use_count;
    size_t use_capacity;
    char **abi_libraries;
    size_t abi_library_count;
    size_t abi_library_capacity;
} BundleScanInfo;

typedef struct BundleExtlibMatch {
    size_t bundle_index;
    char *entry_path;
} BundleExtlibMatch;

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

static void bundle_extlib_match_array_dispose(BundleExtlibMatch *matches,
                                              size_t count) {
    size_t index;

    if (matches == NULL) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        free(matches[index].entry_path);
    }
    free(matches);
}

static bool string_array_contains_text(char *const *items,
                                       size_t count,
                                       const char *text) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(items[index], text) == 0) {
            return true;
        }
    }
    return false;
}

static void bundle_scan_info_dispose(BundleScanInfo *info) {
    if (info == NULL) {
        return;
    }
    free(info->bundle_path);
    free(info->library_entry_path);
    free_string_array(info->module_names, info->module_count);
    free_string_array(info->uses, info->use_count);
    free_string_array(info->abi_libraries, info->abi_library_count);
    memset(info, 0, sizeof(*info));
}

static void bundle_scan_info_array_dispose(BundleScanInfo *infos, size_t count) {
    size_t index;

    if (infos == NULL) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        bundle_scan_info_dispose(&infos[index]);
    }
    free(infos);
}

static bool string_array_push_unique(char ***items,
                                     size_t *count,
                                     size_t *capacity,
                                     const char *text,
                                     char **out_error_message) {
    size_t index;
    char **resized;
    char *copy;

    for (index = 0U; index < *count; ++index) {
        if (strcmp((*items)[index], text) == 0) {
            return true;
        }
    }

    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0U ? 4U : *capacity * 2U;
        resized = realloc(*items, new_capacity * sizeof(**items));
        if (resized == NULL) {
            return set_errorf(out_error_message, "out of memory");
        }
        *items = resized;
        *capacity = new_capacity;
    }

    copy = str_dup_cstr(text);
    if (copy == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    (*items)[(*count)++] = copy;
    return true;
}

static void remove_tree(const char *path) {
    DIR *dir;
    struct dirent *entry;

    if (path == NULL) {
        return;
    }
    dir = opendir(path);
    if (dir == NULL) {
        if (errno == ENOENT) {
            return;
        }
        (void)unlink(path);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        char *child;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        child = path_join2(path, entry->d_name);
        if (child == NULL) {
            continue;
        }
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            (void)unlink(child);
        }
        free(child);
    }

    closedir(dir);
    (void)rmdir(path);
}

static bool bundle_entry_is_public_ft(const FengZipEntryInfo *entry) {
    size_t length;

    if (entry == NULL || entry->is_directory) {
        return false;
    }
    if (strncmp(entry->path, "mod/", 4U) != 0) {
        return false;
    }
    length = strlen(entry->path);
    return length > 7U && strcmp(entry->path + length - 3U, ".ft") == 0;
}

static bool bundle_entry_is_host_library(const FengZipEntryInfo *entry,
                                         const char *host_target) {
    char *prefix;
    bool matches;
    size_t prefix_length;
    size_t path_length;

    if (entry == NULL || entry->is_directory || host_target == NULL) {
        return false;
    }
    prefix = path_join2("lib", host_target);
    if (prefix == NULL) {
        return false;
    }
    prefix_length = strlen(prefix);
    path_length = strlen(entry->path);
    matches = path_length > prefix_length + 1U &&
              strncmp(entry->path, prefix, prefix_length) == 0 &&
              entry->path[prefix_length] == '/' &&
              feng_fb_is_host_static_library_path(entry->path);
    free(prefix);
    return matches;
}

static char *module_name_from_entry_path(const char *entry_path) {
    size_t path_length;
    size_t name_length;
    char *out;
    size_t index;

    if (entry_path == NULL || strncmp(entry_path, "mod/", 4U) != 0) {
        return NULL;
    }
    path_length = strlen(entry_path);
    if (path_length <= 7U || strcmp(entry_path + path_length - 3U, ".ft") != 0) {
        return NULL;
    }

    name_length = path_length - 7U;
    out = malloc(name_length + 1U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, entry_path + 4U, name_length);
    out[name_length] = '\0';
    for (index = 0U; index < name_length; ++index) {
        if (out[index] == '/') {
            out[index] = '.';
        }
    }
    return out;
}

static bool annotation_kind_is_link_calling_convention(FengAnnotationKind kind) {
    return kind == FENG_ANNOTATION_CDECL || kind == FENG_ANNOTATION_STDCALL
           || kind == FENG_ANNOTATION_FASTCALL;
}

static char *decode_raw_string_literal(const char *text) {
    size_t length;
    char *out;
    size_t out_index = 0U;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    if (length < 2U || text[0] != '"' || text[length - 1U] != '"') {
        return str_dup_cstr(text);
    }
    out = (char *)malloc(length);
    if (out == NULL) {
        return NULL;
    }
    for (size_t index = 1U; index + 1U < length; ++index) {
        char ch = text[index];

        if (ch == '\\' && index + 2U < length) {
            char escaped = text[++index];

            switch (escaped) {
                case '\\':
                    out[out_index++] = '\\';
                    break;
                case '"':
                    out[out_index++] = '"';
                    break;
                default:
                    out[out_index++] = escaped;
                    break;
            }
            continue;
        }
        out[out_index++] = ch;
    }
    out[out_index] = '\0';
    return out;
}

static bool collect_graph_abi_libraries(const FengSymbolGraph *graph,
                                        BundleScanInfo *info,
                                        char **out_error_message) {
    FengSymbolProvider *provider = NULL;
    FengSymbolError symbol_error = {0};
    size_t module_index;
    bool ok = false;

    if (!feng_symbol_provider_create(&provider, &symbol_error)) {
        set_errorf(out_error_message,
                   "failed to prepare symbol provider for package link facts: %s",
                   symbol_error.message != NULL ? symbol_error.message : "unknown error");
        goto done;
    }
    if (!feng_symbol_provider_add_graph(provider, graph, &symbol_error)) {
        set_errorf(out_error_message,
                   "failed to read package link facts: %s",
                   symbol_error.message != NULL ? symbol_error.message : "unknown error");
        goto done;
    }

    for (module_index = 0U;
         module_index < feng_symbol_provider_module_count(provider);
         ++module_index) {
        const FengSymbolImportedModule *module = feng_symbol_provider_module_at(provider, module_index);
        size_t decl_index;

        for (decl_index = 0U;
             decl_index < feng_symbol_module_decl_count(module);
             ++decl_index) {
            const FengSymbolDeclView *decl = feng_symbol_module_decl_at(module, decl_index);
            const char *raw_library;
            char *library;

            if (feng_symbol_decl_kind(decl) != FENG_SYMBOL_DECL_KIND_FUNCTION ||
                !feng_symbol_decl_is_extern(decl) ||
                !annotation_kind_is_link_calling_convention(
                    feng_symbol_decl_calling_convention(decl))) {
                continue;
            }
            raw_library = feng_symbol_decl_abi_library(decl);
            if (raw_library == NULL || raw_library[0] == '\0') {
                continue;
            }
            library = decode_raw_string_literal(raw_library);
            if (library == NULL) {
                set_errorf(out_error_message, "out of memory");
                goto done;
            }
            if (!string_array_push_unique(&info->abi_libraries,
                                          &info->abi_library_count,
                                          &info->abi_library_capacity,
                                          library,
                                          out_error_message)) {
                free(library);
                goto done;
            }
            free(library);
        }
    }
    ok = true;

done:
    feng_symbol_error_free(&symbol_error);
    feng_symbol_provider_free(provider);
    return ok;
}

static bool scan_bundle_dependencies(const char *bundle_path,
                                     const char *host_target,
                                     BundleScanInfo *out_info,
                                     char **out_error_message) {
    FengZipReader reader = {0};
    char *zip_error = NULL;
    size_t entry_count;
    size_t index;

    memset(out_info, 0, sizeof(*out_info));
    out_info->bundle_path = str_dup_cstr(bundle_path);
    if (out_info->bundle_path == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }

    if (!feng_zip_reader_open(bundle_path, &reader, &zip_error)) {
        free(out_info->bundle_path);
        out_info->bundle_path = NULL;
        return set_errorf(out_error_message,
                          "failed to open bundle %s: %s",
                          bundle_path,
                          zip_error != NULL ? zip_error : "unknown error");
    }

    entry_count = feng_zip_reader_entry_count(&reader);
    for (index = 0U; index < entry_count; ++index) {
        FengZipEntryInfo entry;

        if (!feng_zip_reader_entry_at(&reader, index, &entry, &zip_error)) {
            feng_zip_reader_dispose(&reader);
            return set_errorf(out_error_message,
                              "failed to inspect bundle %s: %s",
                              bundle_path,
                              zip_error != NULL ? zip_error : "unknown error");
        }
        if (bundle_entry_is_host_library(&entry, host_target)) {
            if (out_info->library_entry_path != NULL) {
                feng_zip_reader_dispose(&reader);
                return set_errorf(out_error_message,
                                  "bundle %s contains multiple host libraries under lib/%s",
                                  bundle_path,
                                  host_target);
            }
            out_info->library_entry_path = str_dup_cstr(entry.path);
            if (out_info->library_entry_path == NULL) {
                feng_zip_reader_dispose(&reader);
                return set_errorf(out_error_message, "out of memory");
            }
            continue;
        }
        if (bundle_entry_is_public_ft(&entry)) {
            FengSymbolFtReadOptions options = {0};
            FengSymbolGraph *graph = NULL;
            FengSymbolError symbol_error = {0};
            void *data = NULL;
            size_t data_size = 0U;
            char *source_name = NULL;
            char *module_name = NULL;
            size_t module_index;

            if (!feng_zip_reader_read(&reader, entry.path, &data, &data_size, &zip_error)) {
                feng_zip_reader_dispose(&reader);
                return set_errorf(out_error_message,
                                  "failed to read %s from %s: %s",
                                  entry.path,
                                  bundle_path,
                                  zip_error != NULL ? zip_error : "unknown error");
            }

            source_name = dup_printf("%s:%s", bundle_path, entry.path);
            module_name = module_name_from_entry_path(entry.path);
            options.expected_profile = FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC;
            if (source_name == NULL || module_name == NULL) {
                feng_zip_free(data);
                free(source_name);
                free(module_name);
                feng_zip_reader_dispose(&reader);
                return set_errorf(out_error_message, "out of memory");
            }
            if (!feng_symbol_ft_read_bytes(data,
                                           data_size,
                                           source_name,
                                           &options,
                                           &graph,
                                           &symbol_error)) {
                feng_zip_free(data);
                free(source_name);
                free(module_name);
                feng_zip_reader_dispose(&reader);
                return set_errorf(out_error_message,
                                  "failed to read symbol table %s: %s",
                                  source_name,
                                  symbol_error.message != NULL ? symbol_error.message : "unknown error");
            }
            if (!string_array_push_unique(&out_info->module_names,
                                          &out_info->module_count,
                                          &out_info->module_capacity,
                                          module_name,
                                          out_error_message)) {
                feng_symbol_graph_free(graph);
                feng_zip_free(data);
                free(source_name);
                free(module_name);
                feng_zip_reader_dispose(&reader);
                return false;
            }
            for (module_index = 0U;
                 module_index < feng_symbol_graph_module_count(graph);
                 ++module_index) {
                const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, module_index);
                size_t use_index;

                for (use_index = 0U;
                     use_index < feng_symbol_module_use_count(module);
                     ++use_index) {
                    const char *use_name = feng_symbol_module_use_at(module, use_index);

                    if (use_name == NULL) {
                        continue;
                    }
                    if (!string_array_push_unique(&out_info->uses,
                                                  &out_info->use_count,
                                                  &out_info->use_capacity,
                                                  use_name,
                                                  out_error_message)) {
                        feng_symbol_graph_free(graph);
                        feng_zip_free(data);
                        free(source_name);
                        free(module_name);
                        feng_zip_reader_dispose(&reader);
                        return false;
                    }
                }
            }

            if (!collect_graph_abi_libraries(graph, out_info, out_error_message)) {
                feng_symbol_graph_free(graph);
                feng_zip_free(data);
                free(source_name);
                free(module_name);
                feng_zip_reader_dispose(&reader);
                return false;
            }

            feng_symbol_graph_free(graph);
            feng_zip_free(data);
            free(source_name);
            free(module_name);
        }
    }

    feng_zip_reader_dispose(&reader);
    if (out_info->library_entry_path == NULL) {
        return set_errorf(out_error_message,
                          "bundle %s does not contain a host static library under lib/%s",
                          bundle_path,
                          host_target);
    }
    return true;
}

static ssize_t find_module_owner_index(const BundleScanInfo *bundles,
                                       size_t bundle_count,
                                       const char *module_name,
                                       size_t *out_duplicate_bundle,
                                       size_t *out_duplicate_index) {
    size_t bundle_index;
    ssize_t found = -1;

    for (bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
        size_t module_index;

        for (module_index = 0U; module_index < bundles[bundle_index].module_count; ++module_index) {
            if (strcmp(bundles[bundle_index].module_names[module_index], module_name) != 0) {
                continue;
            }
            if (found >= 0) {
                if (out_duplicate_bundle != NULL) {
                    *out_duplicate_bundle = bundle_index;
                }
                if (out_duplicate_index != NULL) {
                    *out_duplicate_index = (size_t)found;
                }
                return -2;
            }
            found = (ssize_t)bundle_index;
        }
    }

    return found;
}

static bool topo_sort_bundles(const BundleScanInfo *bundles,
                              size_t bundle_count,
                              size_t **out_order,
                              char **out_error_message) {
    bool *edges;
    size_t *indegree;
    bool *emitted;
    size_t *order;
    size_t bundle_index;
    size_t cursor = 0U;

    *out_order = NULL;
    if (bundle_count == 0U) {
        return true;
    }

    edges = calloc(bundle_count * bundle_count, sizeof(*edges));
    indegree = calloc(bundle_count, sizeof(*indegree));
    emitted = calloc(bundle_count, sizeof(*emitted));
    order = calloc(bundle_count, sizeof(*order));
    if (edges == NULL || indegree == NULL || emitted == NULL || order == NULL) {
        free(order);
        free(emitted);
        free(indegree);
        free(edges);
        return set_errorf(out_error_message, "out of memory");
    }

    for (bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
        size_t use_index;

        for (use_index = 0U; use_index < bundles[bundle_index].use_count; ++use_index) {
            size_t duplicate_bundle = 0U;
            size_t duplicate_index = 0U;
            ssize_t owner = find_module_owner_index(bundles,
                                                    bundle_count,
                                                    bundles[bundle_index].uses[use_index],
                                                    &duplicate_bundle,
                                                    &duplicate_index);

            if (owner == -2) {
                free(order);
                free(emitted);
                free(indegree);
                free(edges);
                return set_errorf(out_error_message,
                                  "module %s is provided by both %s and %s",
                                  bundles[bundle_index].uses[use_index],
                                  bundles[duplicate_index].bundle_path,
                                  bundles[duplicate_bundle].bundle_path);
            }
            if (owner < 0 || (size_t)owner == bundle_index) {
                continue;
            }
            if (!edges[bundle_index * bundle_count + (size_t)owner]) {
                edges[bundle_index * bundle_count + (size_t)owner] = true;
                indegree[(size_t)owner] += 1U;
            }
        }
    }

    while (cursor < bundle_count) {
        bool found = false;

        for (bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
            size_t target_index;

            if (emitted[bundle_index] || indegree[bundle_index] != 0U) {
                continue;
            }
            emitted[bundle_index] = true;
            order[cursor++] = bundle_index;
            for (target_index = 0U; target_index < bundle_count; ++target_index) {
                if (edges[bundle_index * bundle_count + target_index]) {
                    indegree[target_index] -= 1U;
                }
            }
            found = true;
            break;
        }

        if (!found) {
            free(order);
            free(emitted);
            free(indegree);
            free(edges);
            return set_errorf(out_error_message,
                              "package dependency cycle detected among --pkg bundles");
        }
    }

    free(emitted);
    free(indegree);
    free(edges);
    *out_order = order;
    return true;
}

static bool extract_sorted_bundle_libraries(const BundleScanInfo *bundles,
                                            size_t bundle_count,
                                            const size_t *order,
                                            char ***out_library_paths,
                                            size_t *out_library_count,
                                            char **out_temp_dir,
                                            char **out_error_message) {
    char *template_path;
    size_t index;
    char **library_paths;

    *out_library_paths = NULL;
    *out_library_count = 0U;
    *out_temp_dir = NULL;
    if (bundle_count == 0U) {
        return true;
    }

    template_path = make_temp_template("feng_bundle_link_XXXXXX");
    library_paths = calloc(bundle_count, sizeof(*library_paths));
    if (template_path == NULL || library_paths == NULL) {
        free(library_paths);
        free(template_path);
        return set_errorf(out_error_message, "out of memory");
    }
    if (mkdtemp(template_path) == NULL) {
        free(library_paths);
        free(template_path);
        return set_errorf(out_error_message,
                          "failed to create bundle temp directory: %s",
                          strerror(errno));
    }

    for (index = 0U; index < bundle_count; ++index) {
        const BundleScanInfo *bundle = &bundles[order[index]];
        FengZipReader reader = {0};
        char *zip_error = NULL;

        library_paths[index] = dup_printf("%s/%03zu_%s",
                                          template_path,
                                          index,
                                          path_basename(bundle->library_entry_path));
        if (library_paths[index] == NULL) {
            free_string_array(library_paths, bundle_count);
            remove_tree(template_path);
            free(template_path);
            return set_errorf(out_error_message, "out of memory");
        }
        if (!feng_zip_reader_open(bundle->bundle_path, &reader, &zip_error)) {
            free_string_array(library_paths, bundle_count);
            remove_tree(template_path);
            free(template_path);
            return set_errorf(out_error_message,
                              "failed to open bundle %s: %s",
                              bundle->bundle_path,
                              zip_error != NULL ? zip_error : "unknown error");
        }
        if (!feng_zip_reader_extract(&reader,
                                     bundle->library_entry_path,
                                     library_paths[index],
                                     &zip_error)) {
            feng_zip_reader_dispose(&reader);
            free_string_array(library_paths, bundle_count);
            remove_tree(template_path);
            free(template_path);
            return set_errorf(out_error_message,
                              "failed to extract %s from %s: %s",
                              bundle->library_entry_path,
                              bundle->bundle_path,
                              zip_error != NULL ? zip_error : "unknown error");
        }
        feng_zip_reader_dispose(&reader);
    }

    *out_library_paths = library_paths;
    *out_library_count = bundle_count;
    *out_temp_dir = template_path;
    return true;
}

static bool collect_scanned_bundle_abi_libraries(const BundleScanInfo *bundles,
                                                 size_t bundle_count,
                                                 char ***out_libraries,
                                                 size_t *out_library_count,
                                                 char **out_error_message) {
    char **libraries = NULL;
    size_t library_count = 0U;
    size_t library_capacity = 0U;

    *out_libraries = NULL;
    *out_library_count = 0U;
    for (size_t bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
        const BundleScanInfo *bundle = &bundles[bundle_index];

        for (size_t library_index = 0U;
             library_index < bundle->abi_library_count;
             ++library_index) {
            if (!string_array_push_unique(&libraries,
                                          &library_count,
                                          &library_capacity,
                                          bundle->abi_libraries[library_index],
                                          out_error_message)) {
                free_string_array(libraries, library_count);
                return false;
            }
        }
    }

    *out_libraries = libraries;
    *out_library_count = library_count;
    return true;
}

static bool collect_bundle_link_libraries(const char *const *bundle_paths,
                                          size_t bundle_count,
                                          const char *host_target,
                                          char ***out_library_paths,
                                          size_t *out_library_count,
                                          char ***out_abi_libraries,
                                          size_t *out_abi_library_count,
                                          char **out_temp_dir,
                                          char **out_error_message) {
    BundleScanInfo *bundles;
    size_t *order = NULL;
    size_t index;
    bool ok = false;

    bundles = calloc(bundle_count, sizeof(*bundles));
    if (bundles == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }

    for (index = 0U; index < bundle_count; ++index) {
        if (!scan_bundle_dependencies(bundle_paths[index],
                                      host_target,
                                      &bundles[index],
                                      out_error_message)) {
            goto done;
        }
    }
    if (!topo_sort_bundles(bundles, bundle_count, &order, out_error_message)) {
        goto done;
    }
    if (!collect_scanned_bundle_abi_libraries(bundles,
                                              bundle_count,
                                              out_abi_libraries,
                                              out_abi_library_count,
                                              out_error_message)) {
        goto done;
    }
    if (!extract_sorted_bundle_libraries(bundles,
                                         bundle_count,
                                         order,
                                         out_library_paths,
                                         out_library_count,
                                         out_temp_dir,
                                         out_error_message)) {
        free_string_array(*out_abi_libraries, *out_abi_library_count);
        *out_abi_libraries = NULL;
        *out_abi_library_count = 0U;
        goto done;
    }

    ok = true;

done:
    free(order);
    bundle_scan_info_array_dispose(bundles, bundle_count);
    return ok;
}

static bool collect_bundle_extlib_static_libraries(const char *const *bundle_paths,
                                                   size_t bundle_count,
                                                   const char *host_target,
                                                   char *const *library_names,
                                                   size_t library_count,
                                                   char ***out_library_paths,
                                                   size_t *out_library_count,
                                                   char ***out_satisfied_names,
                                                   size_t *out_satisfied_count,
                                                   char **out_temp_dir,
                                                   char **out_error_message) {
    BundleExtlibMatch *matches = NULL;
    char *entry_prefix = NULL;
    char *template_path = NULL;
    char **library_paths = NULL;
    char **satisfied_names = NULL;
    size_t prefix_length = 0U;
    size_t bundle_index;
    size_t lib_index;
    size_t match_count = 0U;
    size_t extracted_count = 0U;
    bool ok = false;

    *out_library_paths = NULL;
    *out_library_count = 0U;
    *out_satisfied_names = NULL;
    *out_satisfied_count = 0U;
    *out_temp_dir = NULL;
    if (bundle_count == 0U || library_count == 0U) {
        return true;
    }
    if (host_target == NULL || host_target[0] == '\0') {
        return set_errorf(out_error_message,
                          "host target is required to prepare bundle extlib static libraries");
    }

    matches = calloc(library_count, sizeof(*matches));
    entry_prefix = dup_printf("extlib/%s/", host_target);
    if (matches == NULL || entry_prefix == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    prefix_length = strlen(entry_prefix);

    for (bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
        FengZipReader reader = {0};
        char *zip_error = NULL;
        size_t entry_count;
        size_t entry_index;

        if (!feng_zip_reader_open(bundle_paths[bundle_index], &reader, &zip_error)) {
            set_errorf(out_error_message,
                       "failed to open bundle %s while resolving extlib static libraries: %s",
                       bundle_paths[bundle_index],
                       zip_error != NULL ? zip_error : "unknown error");
            free(zip_error);
            goto done;
        }

        entry_count = feng_zip_reader_entry_count(&reader);
        for (entry_index = 0U; entry_index < entry_count; ++entry_index) {
            FengZipEntryInfo info = {0};

            zip_error = NULL;
            if (!feng_zip_reader_entry_at(&reader, entry_index, &info, &zip_error)) {
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "failed to inspect bundle entry in %s while resolving extlib static libraries: %s",
                           bundle_paths[bundle_index],
                           zip_error != NULL ? zip_error : "unknown error");
                free(zip_error);
                goto done;
            }
            free(zip_error);

            if (info.is_directory || strncmp(info.path, entry_prefix, prefix_length) != 0) {
                continue;
            }
            for (lib_index = 0U; lib_index < library_count; ++lib_index) {
                char *entry_copy;

                if (!feng_fb_host_static_library_matches_name(info.path, library_names[lib_index])) {
                    continue;
                }
                if (matches[lib_index].entry_path != NULL) {
                    feng_zip_reader_dispose(&reader);
                    set_errorf(out_error_message,
                               "multiple --pkg bundles provide extlib static library for %s: %s and %s",
                               library_names[lib_index],
                               bundle_paths[matches[lib_index].bundle_index],
                               bundle_paths[bundle_index]);
                    goto done;
                }
                entry_copy = str_dup_cstr(info.path);
                if (entry_copy == NULL) {
                    feng_zip_reader_dispose(&reader);
                    set_errorf(out_error_message, "out of memory");
                    goto done;
                }
                matches[lib_index].bundle_index = bundle_index;
                matches[lib_index].entry_path = entry_copy;
                match_count += 1U;
            }
        }

        feng_zip_reader_dispose(&reader);
    }

    if (match_count == 0U) {
        ok = true;
        goto done;
    }

    template_path = make_temp_template("feng_bundle_extlib_link_XXXXXX");
    library_paths = calloc(match_count, sizeof(*library_paths));
    satisfied_names = calloc(match_count, sizeof(*satisfied_names));
    if (template_path == NULL || library_paths == NULL || satisfied_names == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    if (mkdtemp(template_path) == NULL) {
        set_errorf(out_error_message,
                   "failed to create bundle extlib temp directory: %s",
                   strerror(errno));
        goto done;
    }

    for (lib_index = 0U; lib_index < library_count; ++lib_index) {
        FengZipReader reader = {0};
        char *zip_error = NULL;

        if (matches[lib_index].entry_path == NULL) {
            continue;
        }
        satisfied_names[extracted_count] = str_dup_cstr(library_names[lib_index]);
        library_paths[extracted_count] = dup_printf("%s/%03zu_%s",
                                                    template_path,
                                                    extracted_count,
                                                    path_basename(matches[lib_index].entry_path));
        if (satisfied_names[extracted_count] == NULL || library_paths[extracted_count] == NULL) {
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
        if (!feng_zip_reader_open(bundle_paths[matches[lib_index].bundle_index], &reader, &zip_error)) {
            set_errorf(out_error_message,
                       "failed to open bundle %s while extracting extlib static library %s: %s",
                       bundle_paths[matches[lib_index].bundle_index],
                       matches[lib_index].entry_path,
                       zip_error != NULL ? zip_error : "unknown error");
            free(zip_error);
            goto done;
        }
        if (!feng_zip_reader_extract(&reader,
                                     matches[lib_index].entry_path,
                                     library_paths[extracted_count],
                                     &zip_error)) {
            feng_zip_reader_dispose(&reader);
            set_errorf(out_error_message,
                       "failed to extract %s from %s: %s",
                       matches[lib_index].entry_path,
                       bundle_paths[matches[lib_index].bundle_index],
                       zip_error != NULL ? zip_error : "unknown error");
            free(zip_error);
            goto done;
        }
        feng_zip_reader_dispose(&reader);
        extracted_count += 1U;
    }

    *out_library_paths = library_paths;
    *out_library_count = extracted_count;
    *out_satisfied_names = satisfied_names;
    *out_satisfied_count = extracted_count;
    *out_temp_dir = template_path;
    library_paths = NULL;
    satisfied_names = NULL;
    template_path = NULL;
    ok = true;

done:
    free(entry_prefix);
    bundle_extlib_match_array_dispose(matches, library_count);
    if (!ok) {
        free_string_array(satisfied_names, extracted_count);
        free_string_array(library_paths, extracted_count);
        remove_tree(template_path);
        free(template_path);
    }
    return ok;
}

static const char *host_runtime_dynamic_library_suffix(void) {
#if defined(__APPLE__)
    return ".dylib";
#elif defined(_WIN32)
    return ".dll";
#elif defined(__linux__)
    return ".so";
#else
    return NULL;
#endif
}

static bool extract_bundle_runtime_dynamic_libraries(const char *const *bundle_paths,
                                                     size_t bundle_count,
                                                     const char *host_target,
                                                     char *const *library_names,
                                                     size_t library_count,
                                                     const char *binary_path,
                                                     char **out_error_message) {
    const char *suffix;
    char *binary_dir = NULL;
    char *entry_prefix = NULL;
    char **released_names = NULL;
    size_t released_count = 0U;
    size_t released_capacity = 0U;
    size_t bundle_index;
    size_t prefix_len;
    bool ok = false;

    if (bundle_count == 0U || library_count == 0U) {
        return true;
    }
    if (host_target == NULL || host_target[0] == '\0') {
        return set_errorf(out_error_message,
                          "host target is required to release bundle extlib dynamic libraries");
    }
    if (binary_path == NULL || binary_path[0] == '\0') {
        return set_errorf(out_error_message,
                          "binary output path is required to release bundle extlib dynamic libraries");
    }

    suffix = host_runtime_dynamic_library_suffix();
    if (suffix == NULL) {
        return set_errorf(out_error_message,
                          "unsupported host OS for bundle extlib dynamic library release");
    }

    binary_dir = path_dirname_dup(binary_path);
    entry_prefix = dup_printf("extlib/%s/", host_target);
    if (binary_dir == NULL || entry_prefix == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    prefix_len = strlen(entry_prefix);

    for (bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
        FengZipReader reader = {0};
        char *zip_error = NULL;
        size_t entry_index;
        size_t entry_count;

        if (!feng_zip_reader_open(bundle_paths[bundle_index], &reader, &zip_error)) {
            set_errorf(out_error_message,
                       "failed to open bundle %s while releasing extlib dynamic libraries: %s",
                       bundle_paths[bundle_index],
                       zip_error != NULL ? zip_error : "unknown error");
            free(zip_error);
            goto done;
        }

        entry_count = feng_zip_reader_entry_count(&reader);
        for (entry_index = 0U; entry_index < entry_count; ++entry_index) {
            FengZipEntryInfo info = {0};
            const char *basename;
            char *dest_path = NULL;
            struct stat st;

            zip_error = NULL;
            if (!feng_zip_reader_entry_at(&reader, entry_index, &info, &zip_error)) {
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "failed to inspect bundle entry in %s while releasing extlib dynamic libraries: %s",
                           bundle_paths[bundle_index],
                           zip_error != NULL ? zip_error : "unknown error");
                free(zip_error);
                goto done;
            }
            free(zip_error);
            zip_error = NULL;

            if (info.is_directory) {
                continue;
            }
            if (strncmp(info.path, entry_prefix, prefix_len) != 0) {
                continue;
            }
            if (!path_has_suffix(info.path, suffix)) {
                continue;
            }
            {
                bool matched = false;
                size_t lib_index;

                for (lib_index = 0U; lib_index < library_count; ++lib_index) {
                    char *expected = host_runtime_dynamic_library_file_name(library_names[lib_index]);

                    if (expected != NULL && strcmp(path_basename(info.path), expected) == 0) {
                        matched = true;
                    }
                    free(expected);
                    if (matched) {
                        break;
                    }
                }
                if (!matched) {
                    continue;
                }
            }

            basename = path_basename(info.path);
            if (basename == NULL || basename[0] == '\0') {
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "invalid extlib bundle entry path: %s",
                           info.path);
                goto done;
            }
            if (string_array_contains_text(released_names, released_count, basename)) {
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "runtime extlib filename collision across --pkg bundles: %s",
                           basename);
                goto done;
            }
            if (!string_array_push_unique(&released_names,
                                          &released_count,
                                          &released_capacity,
                                          basename,
                                          out_error_message)) {
                feng_zip_reader_dispose(&reader);
                goto done;
            }

            dest_path = path_join2(binary_dir, basename);
            if (dest_path == NULL) {
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message, "out of memory");
                goto done;
            }
            if (strcmp(dest_path, binary_path) == 0) {
                free(dest_path);
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "runtime extlib output conflicts with binary path: %s",
                           basename);
                goto done;
            }
            if (stat(dest_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                free(dest_path);
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "runtime extlib output conflicts with existing directory: %s",
                           basename);
                goto done;
            }

            zip_error = NULL;
            if (!feng_zip_reader_extract(&reader, info.path, dest_path, &zip_error)) {
                free(dest_path);
                feng_zip_reader_dispose(&reader);
                set_errorf(out_error_message,
                           "failed to extract runtime extlib %s from %s: %s",
                           info.path,
                           bundle_paths[bundle_index],
                           zip_error != NULL ? zip_error : "unknown error");
                free(zip_error);
                goto done;
            }
            free(dest_path);
        }

        feng_zip_reader_dispose(&reader);
    }

    ok = true;

done:
    free_string_array(released_names, released_count);
    free(entry_prefix);
    free(binary_dir);
    return ok;
}

/* --- runtime artefact discovery ----------------------------------------- */

/* Resolve the running executable's absolute path. Returns a malloc'd
 * string on success, or NULL on failure. */
static char *resolve_executable_path(const char *argv0) {
#if defined(__APPLE__)
    uint32_t size = 0U;
    _NSGetExecutablePath(NULL, &size);
    if (size == 0U) return NULL;
    char *raw = malloc(size);
    if (raw == NULL) return NULL;
    if (_NSGetExecutablePath(raw, &size) != 0) {
        free(raw);
        return NULL;
    }
    char *real = realpath(raw, NULL);
    free(raw);
    if (real != NULL) return real;
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1U);
    if (n > 0) {
        buf[n] = '\0';
        char *real = realpath(buf, NULL);
        if (real != NULL) return real;
    }
#endif
    /* Fallback: realpath argv[0] (works when invoked with a path). */
    if (argv0 != NULL && argv0[0] != '\0') {
        char *real = realpath(argv0, NULL);
        if (real != NULL) return real;
    }
    return NULL;
}

/* Walk up from `start_dir`, looking for `<dir>/<rel>`. Returns the first
 * matching `<dir>` (malloc'd) or NULL. Caller frees. */
static char *find_ancestor_with(const char *start_dir, const char *rel) {
    char *cur = realpath(start_dir, NULL);
    if (cur == NULL) {
        cur = str_dup_cstr(start_dir);
        if (cur == NULL) return NULL;
    }
    while (cur != NULL && cur[0] != '\0') {
        char *probe = path_join2(cur, rel);
        if (probe == NULL) {
            free(cur);
            return NULL;
        }
        if (path_exists(probe)) {
            free(probe);
            return cur;
        }
        free(probe);
        if (strcmp(cur, "/") == 0) {
            free(cur);
            return NULL;
        }
        char *parent = path_dirname_dup(cur);
        free(cur);
        cur = parent;
    }
    free(cur);
    return NULL;
}

static char *locate_runtime_lib(const char *program_path) {
    const char *env = getenv("FENG_RUNTIME_LIB");
    if (env != NULL && env[0] != '\0') {
        if (!path_exists(env)) {
            fprintf(stderr,
                    "FENG_RUNTIME_LIB points to %s which does not exist\n",
                    env);
            return NULL;
        }
        return str_dup_cstr(env);
    }
    char *exe = resolve_executable_path(program_path);
    if (exe == NULL) return NULL;
    char *exe_dir = path_dirname_dup(exe);
    free(exe);
    if (exe_dir == NULL) return NULL;
    char *runtime_rel = path_join_host_static_library("build/lib", "feng_runtime");

    if (runtime_rel == NULL) {
        free(exe_dir);
        return NULL;
    }
    /* Common layout: <root>/build/bin/feng -> <root>/build/lib/<host-runtime-lib> */
    char *root = find_ancestor_with(exe_dir, runtime_rel);
    free(exe_dir);
    if (root == NULL) {
        free(runtime_rel);
        return NULL;
    }
    char *path = path_join2(root, runtime_rel);
    free(root);
    free(runtime_rel);
    return path;
}

static char *locate_runtime_include(const char *program_path) {
    const char *env = getenv("FENG_RUNTIME_INCLUDE");
    if (env != NULL && env[0] != '\0') {
        char *probe = path_join2(env, "runtime/feng_runtime.h");
        bool ok = path_exists(probe);
        free(probe);
        if (!ok) {
            fprintf(stderr,
                    "FENG_RUNTIME_INCLUDE=%s does not contain runtime/feng_runtime.h\n",
                    env);
            return NULL;
        }
        return str_dup_cstr(env);
    }
    char *exe = resolve_executable_path(program_path);
    if (exe == NULL) return NULL;
    char *exe_dir = path_dirname_dup(exe);
    free(exe);
    if (exe_dir == NULL) return NULL;
    char *root = find_ancestor_with(exe_dir, "src/runtime/feng_runtime.h");
    free(exe_dir);
    if (root == NULL) return NULL;
    char *path = path_join2(root, "src");
    free(root);
    return path;
}

/* --- extern calling-convention link library mining ----------------------- */

/* Decode a single string-literal annotation argument. The lexer keeps the
 * surrounding quotes; library names never contain escape sequences in
 * practice, but we still tolerate the basic `\\` and `\"` forms so we
 * never silently corrupt unusual names. */
static char *decode_string_literal(const FengExpr *expr) {
    if (expr == NULL || expr->kind != FENG_EXPR_STRING) return NULL;
    const char *raw = expr->as.string.data;
    size_t rlen = expr->as.string.length;
    if (rlen < 2U || raw[0] != '"' || raw[rlen - 1U] != '"') return NULL;
    char *out = malloc(rlen);
    if (out == NULL) return NULL;
    size_t di = 0U;
    for (size_t i = 1U; i + 1U < rlen; ++i) {
        char ch = raw[i];
        if (ch == '\\' && i + 2U < rlen) {
            char esc = raw[++i];
            switch (esc) {
                case '\\': out[di++] = '\\'; break;
                case '"':  out[di++] = '"';  break;
                default:   out[di++] = esc;  break;
            }
        } else {
            out[di++] = ch;
        }
    }
    out[di] = '\0';
    return out;
}

/* Map a Feng extern-calling-convention library name to a host link token. Returns:
 *   NULL — implicit on POSIX (libc / c), no flag needed.
 *   non-NULL — caller-owned malloc'd `name` (without "lib" prefix) to
 *              be appended after `-l`.
 */
static char *map_library_name(const char *raw) {
    if (raw == NULL || raw[0] == '\0') return NULL;
    const char *name = raw;
    if (strncmp(name, "lib", 3) == 0) name += 3;
    if (strcmp(name, "c") == 0) return NULL; /* libc is implicit */
    if (strcmp(name, "feng_runtime") == 0) {
        return NULL;
    }
    if (name[0] == '\0') return NULL;
    return str_dup_cstr(name);
}

static bool cli_link_library_is_path(const char *raw) {
    if (raw == NULL || raw[0] == '\0') return false;
    return strchr(raw, '/') != NULL
           || strchr(raw, '\\') != NULL
           || path_has_suffix(raw, ".a")
           || path_has_suffix(raw, ".so")
           || path_has_suffix(raw, ".dylib")
           || path_has_suffix(raw, ".dll")
           || path_has_suffix(raw, ".lib");
}

static char *map_cli_link_library(const char *raw) {
    const char *name = raw;
    char *flag;
    size_t need;

    if (raw == NULL || raw[0] == '\0') return NULL;
    if (cli_link_library_is_path(raw)) {
        return str_dup_cstr(raw);
    }
    if (strncmp(name, "lib", 3) == 0) {
        name += 3;
    }
    if (name[0] == '\0') return NULL;
    need = strlen(name) + 3U;
    flag = malloc(need);
    if (flag == NULL) return NULL;
    snprintf(flag, need, "-l%s", name);
    return flag;
}

static bool string_array_contains(char *const *arr, size_t count, const char *needle) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(arr[i], needle) == 0) return true;
    }
    return false;
}

/* Normalize one raw annotation library name and append it once. */
static int append_unique_link_library(char ***libs,
                                      size_t *count,
                                      const char *raw_name) {
    char *mapped;
    char **grown;

    if (libs == NULL || count == NULL) {
        return -1;
    }
    mapped = map_library_name(raw_name);
    if (mapped == NULL) {
        return 0;
    }
    if (string_array_contains(*libs, *count, mapped)) {
        free(mapped);
        return 0;
    }
    grown = realloc(*libs, (*count + 1U) * sizeof(*grown));
    if (grown == NULL) {
        free(mapped);
        return -1;
    }
    grown[*count] = mapped;
    *libs = grown;
    *count += 1U;
    return 0;
}

/* Returns 0 on success, -1 on allocation failure. */
static int collect_link_libs(const FengProgram *const *programs,
                             size_t program_count,
                             char ***out_libs,
                             size_t *out_count) {
    *out_libs = NULL;
    *out_count = 0;
    char **libs = NULL;
    for (size_t pi = 0; pi < program_count; ++pi) {
        const FengProgram *prog = programs[pi];
        if (prog == NULL) continue;
        for (size_t di = 0; di < prog->declaration_count; ++di) {
            const FengDecl *decl = prog->declarations[di];
            if (decl == NULL || !decl->is_extern) continue;
            if (decl->kind != FENG_DECL_FUNCTION) continue;
            for (size_t ai = 0; ai < decl->annotation_count; ++ai) {
                const FengAnnotation *ann = &decl->annotations[ai];
                if (!annotation_kind_is_link_calling_convention(ann->builtin_kind)) continue;
                if (ann->arg_count < 1U) continue;
                char *raw = decode_string_literal(ann->args[0]);
                if (raw == NULL) continue;
                int append_rc = append_unique_link_library(&libs, out_count, raw);
                free(raw);
                if (append_rc != 0) {
                    for (size_t k = 0; k < *out_count; ++k) free(libs[k]);
                    free(libs);
                    return -1;
                }
            }
        }
    }
    *out_libs = libs;
    return 0;
}

/* --- argv builder & spawn ------------------------------------------------ */

typedef struct ArgVec {
    char **items;
    size_t count;
    size_t cap;
} ArgVec;

static bool argv_push(ArgVec *v, const char *s) {
    if (v->count + 1U >= v->cap) {
        size_t new_cap = v->cap == 0U ? 16U : v->cap * 2U;
        char **next = realloc(v->items, new_cap * sizeof(*next));
        if (next == NULL) return false;
        v->items = next;
        v->cap = new_cap;
    }
    char *dup = str_dup_cstr(s);
    if (dup == NULL) return false;
    v->items[v->count++] = dup;
    v->items[v->count] = NULL;
    return true;
}

static void argv_free(ArgVec *v) {
    if (v->items == NULL) return;
    for (size_t i = 0; i < v->count; ++i) free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static int spawn_and_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s terminated by signal %d\n", argv[0], WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return -1;
}

static bool argv_push_mode_flags(ArgVec *av, bool release) {
    if (!argv_push(av, release ? "-O2" : "-O0")) {
        return false;
    }
    if (!release && !argv_push(av, "-g")) {
        return false;
    }
    if (release && !argv_push(av, "-DNDEBUG")) {
        return false;
    }
    return true;
}

/* Split a whitespace-separated flag string and push each token into av.
 * Used to honour FENG_CC_FLAGS so that host-tool invocations (e.g. UBSan
 * runtime linking) can be augmented without patching the compiler. */
static bool argv_push_env_flags(ArgVec *av, const char *flags) {
    if (flags == NULL || flags[0] == '\0') {
        return true;
    }
    const char *p = flags;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            ++p;
        }
        size_t len = (size_t)(p - start);
        char *token = malloc(len + 1U);
        if (token == NULL) {
            return false;
        }
        memcpy(token, start, len);
        token[len] = '\0';
        if (!argv_push(av, token)) {
            free(token);
            return false;
        }
        free(token);
    }
    return true;
}

/* --- entry --------------------------------------------------------------- */

int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts) {
    if (opts == NULL || opts->c_path == NULL || opts->out_path == NULL) {
        fprintf(stderr, "internal error: driver invoked with NULL options\n");
        return 2;
    }

    char *include_dir = locate_runtime_include(opts->program_path);
    if (include_dir == NULL) {
        fprintf(stderr,
                "error: cannot locate runtime headers.\n"
                "  set FENG_RUNTIME_INCLUDE=<dir-containing-runtime/feng_runtime.h>\n"
                "  or run from a build tree containing src/runtime/feng_runtime.h.\n");
        return 1;
    }

    char *runtime_lib = NULL;
    char *host_target = NULL;
    char *bundle_error = NULL;
    char *bundle_temp_dir = NULL;
    char *bundle_extlib_temp_dir = NULL;
    char **bundle_libs = NULL;
    char **bundle_abi_libs = NULL;
    char **bundle_extlib_static_libs = NULL;
    char **bundle_extlib_satisfied_libs = NULL;
    size_t bundle_lib_count = 0U;
    size_t bundle_abi_lib_count = 0U;
    size_t bundle_extlib_static_lib_count = 0U;
    size_t bundle_extlib_satisfied_lib_count = 0U;
    if (opts->target == FENG_COMPILE_TARGET_BIN) {
        runtime_lib = locate_runtime_lib(opts->program_path);
        if (runtime_lib == NULL) {
            char *runtime_basename = feng_fb_host_static_library_file_name("feng_runtime");

            fprintf(stderr,
                "error: cannot locate %s.\n"
                "  set FENG_RUNTIME_LIB=<path-to-%s> or run from a\n"
                "  build tree where build/lib/%s exists.\n",
                runtime_basename != NULL ? runtime_basename : "the Feng runtime static library",
                runtime_basename != NULL ? runtime_basename : "the Feng runtime static library",
                runtime_basename != NULL ? runtime_basename : "the Feng runtime static library");
            free(runtime_basename);
            free(include_dir);
            return 1;
        }
        if (opts->bundle_count > 0U) {
            if (!feng_fb_detect_host_target(&host_target, &bundle_error)) {
                fprintf(stderr,
                        "error: cannot determine host target for package bundles: %s\n",
                        bundle_error != NULL ? bundle_error : "unknown error");
                free(bundle_error);
                free(runtime_lib);
                free(include_dir);
                return 1;
            }
            free(bundle_error);
            bundle_error = NULL;
            if (!collect_bundle_link_libraries(opts->bundle_paths,
                                               opts->bundle_count,
                                               host_target,
                                               &bundle_libs,
                                               &bundle_lib_count,
                                               &bundle_abi_libs,
                                               &bundle_abi_lib_count,
                                               &bundle_temp_dir,
                                               &bundle_error)) {
                fprintf(stderr,
                        "error: failed to prepare package libraries: %s\n",
                        bundle_error != NULL ? bundle_error : "unknown error");
                free(bundle_error);
                free(host_target);
                free(runtime_lib);
                free(include_dir);
                return 1;
            }
        }
    }

    char **libs = NULL;
    size_t lib_count = 0;
    if (opts->target == FENG_COMPILE_TARGET_BIN
        && collect_link_libs(opts->programs, opts->program_count, &libs, &lib_count) != 0) {
        fprintf(stderr, "error: out of memory collecting link libraries\n");
        free_string_array(bundle_abi_libs, bundle_abi_lib_count);
        free_string_array(bundle_libs, bundle_lib_count);
        remove_tree(bundle_temp_dir);
        free(bundle_temp_dir);
        free(host_target);
        free(runtime_lib);
        free(include_dir);
        return 1;
    }
    if (opts->target == FENG_COMPILE_TARGET_BIN) {
        for (size_t index = 0U; index < bundle_abi_lib_count; ++index) {
            if (append_unique_link_library(&libs, &lib_count, bundle_abi_libs[index]) != 0) {
                fprintf(stderr, "error: out of memory collecting package link libraries\n");
                for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
                free(libs);
                free_string_array(bundle_abi_libs, bundle_abi_lib_count);
                free_string_array(bundle_libs, bundle_lib_count);
                remove_tree(bundle_temp_dir);
                free(bundle_temp_dir);
                free(host_target);
                free(runtime_lib);
                free(include_dir);
                return 1;
            }
        }
    }
    if (opts->target == FENG_COMPILE_TARGET_BIN
        && opts->bundle_count > 0U
        && lib_count > 0U
        && !collect_bundle_extlib_static_libraries(opts->bundle_paths,
                                                   opts->bundle_count,
                                                   host_target,
                                                   libs,
                                                   lib_count,
                                                   &bundle_extlib_static_libs,
                                                   &bundle_extlib_static_lib_count,
                                                   &bundle_extlib_satisfied_libs,
                                                   &bundle_extlib_satisfied_lib_count,
                                                   &bundle_extlib_temp_dir,
                                                   &bundle_error)) {
        fprintf(stderr,
                "error: failed to prepare package extlib static libraries: %s\n",
                bundle_error != NULL ? bundle_error : "unknown error");
        free(bundle_error);
        for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
        free(libs);
        free_string_array(bundle_abi_libs, bundle_abi_lib_count);
        free_string_array(bundle_libs, bundle_lib_count);
        remove_tree(bundle_temp_dir);
        free(bundle_temp_dir);
        free(host_target);
        free(runtime_lib);
        free(include_dir);
        return 1;
    }

    const char *cc = getenv("CC");
    if (cc == NULL || cc[0] == '\0') cc = "cc";

    int rc = 0;
    char *include_flag = NULL;
    char *object_path = NULL;
    ArgVec av = {0};
    bool ok = true;
    bool host_tool_failed = false;

    size_t include_need = strlen(include_dir) + 3U;
    include_flag = malloc(include_need);
    if (include_flag == NULL) {
        ok = false;
    } else {
        snprintf(include_flag, include_need, "-I%s", include_dir);
    }

    if (!ok) {
        fprintf(stderr, "error: out of memory building compiler argv\n");
        free(include_flag);
        for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
        free(libs);
        free_string_array(bundle_abi_libs, bundle_abi_lib_count);
        free_string_array(bundle_libs, bundle_lib_count);
        remove_tree(bundle_temp_dir);
        free(bundle_temp_dir);
        free(host_target);
        free(runtime_lib);
        free(include_dir);
        return 1;
    }

    if (opts->target == FENG_COMPILE_TARGET_BIN) {
        if (!argv_push(&av, cc)) { ok = false; }
        if (ok && !argv_push(&av, "-std=gnu11")) { ok = false; }
        if (ok && !argv_push(&av, "-fexceptions")) { ok = false; }
        if (ok && !argv_push_mode_flags(&av, opts->release)) { ok = false; }
#if defined(__linux__)
        /* Per-function/data sections let --gc-sections discard unused
         * runtime symbols at link time.  macOS clang already uses
         * subsections-via-symbols by default so this is Linux-only. */
        if (ok && opts->release && !argv_push(&av, "-ffunction-sections")) { ok = false; }
        if (ok && opts->release && !argv_push(&av, "-fdata-sections")) { ok = false; }
#endif
        if (ok && !argv_push(&av, "-Wall")) { ok = false; }
        if (ok && !argv_push(&av, "-Wextra")) { ok = false; }
        /* Generated C may emit fit-helper functions that are not exercised
         * by the current program (e.g. unused coercion sites). They are
         * intentional artefacts of codegen, not user code, so silence the
         * resulting -Wunused-function noise on the host compile. */
        if (ok && !argv_push(&av, "-Wno-unused-function")) { ok = false; }
        if (ok && !argv_push(&av, "-Wno-unused-variable")) { ok = false; }
        if (ok && !argv_push(&av, "-Wno-unused-label")) { ok = false; }
        if (ok && !argv_push(&av, "-Wno-incompatible-pointer-types")) { ok = false; }
        if (ok && !argv_push(&av, "-Wno-incompatible-library-redeclaration")) { ok = false; }
        if (ok && !argv_push(&av, include_flag)) { ok = false; }
        if (ok && !argv_push(&av, opts->c_path)) { ok = false; }
        for (size_t i = 0; ok && i < bundle_lib_count; ++i) {
            if (!argv_push(&av, bundle_libs[i])) { ok = false; }
        }
        for (size_t i = 0; ok && i < bundle_extlib_static_lib_count; ++i) {
            if (!argv_push(&av, bundle_extlib_static_libs[i])) { ok = false; }
        }
        if (ok && !argv_push(&av, runtime_lib)) { ok = false; }
        if (ok && !argv_push(&av, "-lpthread")) { ok = false; }
        for (size_t i = 0; ok && i < lib_count; ++i) {
            size_t need = strlen(libs[i]) + 3U;
            char *flag = malloc(need);

            if (string_array_contains_text(bundle_extlib_satisfied_libs,
                                           bundle_extlib_satisfied_lib_count,
                                           libs[i])) {
                continue;
            }
            if (flag == NULL) {
                ok = false;
                break;
            }
            snprintf(flag, need, "-l%s", libs[i]);
            ok = argv_push(&av, flag);
            free(flag);
        }
        for (size_t i = 0; ok && i < opts->link_lib_count; ++i) {
            char *flag = map_cli_link_library(opts->link_libs[i]);

            if (flag == NULL) {
                ok = false;
                break;
            }
            ok = argv_push(&av, flag);
            free(flag);
        }
        if (ok && opts->release) {
#if defined(__APPLE__)
            if (!argv_push(&av, "-Wl,-dead_strip")) { ok = false; }
#elif defined(__linux__)
            if (!argv_push(&av, "-Wl,--gc-sections")) { ok = false; }
#endif
            if (ok && !argv_push(&av, "-Wl,-x")) { ok = false; }
        }
        if (ok && !argv_push_env_flags(&av, getenv("FENG_CC_FLAGS"))) { ok = false; }
        if (ok && !argv_push(&av, "-o")) { ok = false; }
        if (ok && !argv_push(&av, opts->out_path)) { ok = false; }
        if (!ok) {
            fprintf(stderr, "error: out of memory building cc argv\n");
            rc = 1;
        } else {
            rc = spawn_and_wait(av.items);
            host_tool_failed = rc != 0;
        }
        argv_free(&av);
    } else {
        object_path = replace_with_sibling_filename(opts->c_path, "feng.o");
        if (object_path == NULL) {
            fprintf(stderr, "error: out of memory composing object path\n");
            rc = 1;
        } else {
            if (!argv_push(&av, cc)) { ok = false; }
            if (ok && !argv_push(&av, "-std=gnu11")) { ok = false; }
            if (ok && !argv_push(&av, "-fexceptions")) { ok = false; }
            if (ok && !argv_push_mode_flags(&av, opts->release)) { ok = false; }
            if (ok && !argv_push(&av, "-Wall")) { ok = false; }
            if (ok && !argv_push(&av, "-Wextra")) { ok = false; }
            /* See bin path above: silence unused-function noise from
             * generated fit helpers for the lib compile too. */
            if (ok && !argv_push(&av, "-Wno-unused-function")) { ok = false; }
            if (ok && !argv_push(&av, "-Wno-unused-variable")) { ok = false; }
            if (ok && !argv_push(&av, "-Wno-unused-label")) { ok = false; }
            if (ok && !argv_push(&av, "-Wno-incompatible-pointer-types")) { ok = false; }
            if (ok && !argv_push(&av, "-Wno-incompatible-library-redeclaration")) { ok = false; }
            if (ok && !argv_push(&av, include_flag)) { ok = false; }
            if (ok && !argv_push(&av, "-c")) { ok = false; }
            if (ok && !argv_push(&av, opts->c_path)) { ok = false; }
            if (ok && !argv_push_env_flags(&av, getenv("FENG_CC_FLAGS"))) { ok = false; }
            if (ok && !argv_push(&av, "-o")) { ok = false; }
            if (ok && !argv_push(&av, object_path)) { ok = false; }
            if (!ok) {
                fprintf(stderr, "error: out of memory building cc argv\n");
                rc = 1;
            } else {
                rc = spawn_and_wait(av.items);
                host_tool_failed = rc != 0;
            }
            argv_free(&av);
        }

        if (rc == 0) {
            const char *ar = getenv("AR");
            if (ar == NULL || ar[0] == '\0') ar = "ar";
#if defined(__APPLE__)
            /* On macOS, 'ar rcs' implicitly runs ranlib which emits
             *   "the table of contents is empty"
             * when the object contains no exported C symbols (e.g. an
             * archive of empty-body generic stubs).  Use 'ar rc' (no
             * implicit ranlib) followed by an explicit
             * 'ranlib -no_warning_for_no_symbols' invocation that
             * silences that diagnostic while still updating the table. */
            if (!argv_push(&av, ar)) { ok = false; }
            if (ok && !argv_push(&av, "rc")) { ok = false; }
            if (ok && !argv_push(&av, opts->out_path)) { ok = false; }
            if (ok && !argv_push(&av, object_path)) { ok = false; }
            if (!ok) {
                fprintf(stderr, "error: out of memory building archive argv\n");
                rc = 1;
            } else {
                rc = spawn_and_wait(av.items);
                host_tool_failed = rc != 0;
            }
            argv_free(&av);
            if (rc == 0) {
                const char *ranlib = getenv("RANLIB");
                if (ranlib == NULL || ranlib[0] == '\0') ranlib = "ranlib";
                if (!argv_push(&av, ranlib)) { ok = false; }
                if (ok && !argv_push(&av, "-no_warning_for_no_symbols")) { ok = false; }
                if (ok && !argv_push(&av, opts->out_path)) { ok = false; }
                if (!ok) {
                    fprintf(stderr, "error: out of memory building ranlib argv\n");
                    rc = 1;
                } else {
                    rc = spawn_and_wait(av.items);
                    host_tool_failed = rc != 0;
                }
                argv_free(&av);
            }
#else
            if (!argv_push(&av, ar)) { ok = false; }
            if (ok && !argv_push(&av, "rcs")) { ok = false; }
            if (ok && !argv_push(&av, opts->out_path)) { ok = false; }
            if (ok && !argv_push(&av, object_path)) { ok = false; }
            if (!ok) {
                fprintf(stderr, "error: out of memory building archive argv\n");
                rc = 1;
            } else {
                rc = spawn_and_wait(av.items);
                host_tool_failed = rc != 0;
            }
            argv_free(&av);
#endif
        }
    }

    if (rc == 0 && opts->target == FENG_COMPILE_TARGET_BIN && opts->bundle_count > 0U) {
        free(bundle_error);
        bundle_error = NULL;
        if (!extract_bundle_runtime_dynamic_libraries(opts->bundle_paths,
                                                      opts->bundle_count,
                                                      host_target,
                                                      libs,
                                                      lib_count,
                                                      opts->out_path,
                                                      &bundle_error)) {
            fprintf(stderr,
                    "error: failed to release package extlib dynamic libraries: %s\n",
                    bundle_error != NULL ? bundle_error : "unknown error");
            rc = 1;
        }
    }

    for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
    free(libs);
    free_string_array(bundle_extlib_satisfied_libs, bundle_extlib_satisfied_lib_count);
    free_string_array(bundle_extlib_static_libs, bundle_extlib_static_lib_count);
    remove_tree(bundle_extlib_temp_dir);
    free(bundle_extlib_temp_dir);
    free_string_array(bundle_abi_libs, bundle_abi_lib_count);
    free_string_array(bundle_libs, bundle_lib_count);
    remove_tree(bundle_temp_dir);
    free(bundle_temp_dir);
    free(host_target);
    free(bundle_error);
    free(runtime_lib);
    free(include_dir);
    free(include_flag);

    if (rc != 0) {
        if (!host_tool_failed) {
            free(object_path);
            return rc;
        }
        fprintf(stderr,
                "error: host C compiler failed (exit=%d).\n"
                "  generated C kept at: %s\n",
                rc, opts->c_path);
        free(object_path);
        return rc;
    }

    /* Success: optionally clean the IR file and collapse the now-empty
     * <out>/ir/c and <out>/ir directories. Non-empty directories are left
     * alone, which keeps future multi-artefact layouts safe. */
    if (!opts->keep_intermediate) {
        bool can_cleanup_dirs = true;
        if (object_path != NULL && unlink(object_path) != 0 && errno != ENOENT) {
            fprintf(stderr,
                    "warning: could not remove intermediate %s: %s\n",
                    object_path, strerror(errno));
            can_cleanup_dirs = false;
        }
        if (unlink(opts->c_path) != 0 && errno != ENOENT) {
            fprintf(stderr,
                    "warning: could not remove intermediate %s: %s\n",
                    opts->c_path, strerror(errno));
            can_cleanup_dirs = false;
        }
        if (can_cleanup_dirs) {
            cleanup_empty_ir_dirs(opts->c_path);
        }
    }
    free(object_path);
    return 0;
}
