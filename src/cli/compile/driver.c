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
} BundleScanInfo;

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

static void bundle_scan_info_dispose(BundleScanInfo *info) {
    if (info == NULL) {
        return;
    }
    free(info->bundle_path);
    free(info->library_entry_path);
    free_string_array(info->module_names, info->module_count);
    free_string_array(info->uses, info->use_count);
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
              strcmp(entry->path + path_length - 2U, ".a") == 0;
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

            feng_symbol_graph_free(graph);
            feng_zip_free(data);
            free(source_name);
            free(module_name);
        }
    }

    feng_zip_reader_dispose(&reader);
    if (out_info->library_entry_path == NULL) {
        return set_errorf(out_error_message,
                          "bundle %s does not contain lib/%s/*.a",
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

    template_path = str_dup_cstr("/tmp/feng_bundle_link_XXXXXX");
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

static bool collect_bundle_link_libraries(const char *const *bundle_paths,
                                          size_t bundle_count,
                                          const char *host_target,
                                          char ***out_library_paths,
                                          size_t *out_library_count,
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
    if (!extract_sorted_bundle_libraries(bundles,
                                         bundle_count,
                                         order,
                                         out_library_paths,
                                         out_library_count,
                                         out_temp_dir,
                                         out_error_message)) {
        goto done;
    }

    ok = true;

done:
    free(order);
    bundle_scan_info_array_dispose(bundles, bundle_count);
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
    /* Common layout: <root>/build/bin/feng -> <root>/build/lib/libfeng_runtime.a */
    char *root = find_ancestor_with(exe_dir, "build/lib/libfeng_runtime.a");
    free(exe_dir);
    if (root == NULL) return NULL;
    char *path = path_join2(root, "build/lib/libfeng_runtime.a");
    free(root);
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

/* --- @cdecl link library mining ----------------------------------------- */

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

/* Map a Feng @cdecl library name to a host link token. Returns:
 *   NULL — implicit on POSIX (libc / c), no flag needed.
 *   non-NULL — caller-owned malloc'd `name` (without "lib" prefix) to
 *              be appended after `-l`.
 */
static char *map_library_name(const char *raw) {
    if (raw == NULL || raw[0] == '\0') return NULL;
    const char *name = raw;
    if (strncmp(name, "lib", 3) == 0) name += 3;
    if (strcmp(name, "c") == 0) return NULL; /* libc is implicit */
    if (name[0] == '\0') return NULL;
    return str_dup_cstr(name);
}

static bool string_array_contains(char *const *arr, size_t count, const char *needle) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(arr[i], needle) == 0) return true;
    }
    return false;
}

/* Returns 0 on success, -1 on allocation failure. */
static int collect_link_libs(const FengProgram *const *programs,
                             size_t program_count,
                             char ***out_libs,
                             size_t *out_count) {
    *out_libs = NULL;
    *out_count = 0;
    size_t cap = 0;
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
                if (ann->builtin_kind != FENG_ANNOTATION_CDECL) continue;
                if (ann->arg_count < 1U) continue;
                char *raw = decode_string_literal(ann->args[0]);
                if (raw == NULL) continue;
                char *mapped = map_library_name(raw);
                free(raw);
                if (mapped == NULL) continue;
                if (string_array_contains(libs, *out_count, mapped)) {
                    free(mapped);
                    continue;
                }
                if (*out_count == cap) {
                    size_t new_cap = cap == 0U ? 4U : cap * 2U;
                    char **new_libs = realloc(libs, new_cap * sizeof(*libs));
                    if (new_libs == NULL) {
                        free(mapped);
                        for (size_t k = 0; k < *out_count; ++k) free(libs[k]);
                        free(libs);
                        return -1;
                    }
                    libs = new_libs;
                    cap = new_cap;
                }
                libs[(*out_count)++] = mapped;
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
    char **bundle_libs = NULL;
    size_t bundle_lib_count = 0U;
    if (opts->target == FENG_COMPILE_TARGET_BIN) {
        runtime_lib = locate_runtime_lib(opts->program_path);
        if (runtime_lib == NULL) {
            fprintf(stderr,
                    "error: cannot locate libfeng_runtime.a.\n"
                    "  set FENG_RUNTIME_LIB=<path-to-libfeng_runtime.a> or run from a\n"
                    "  build tree where build/lib/libfeng_runtime.a exists.\n");
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
        if (ok && !argv_push(&av, "-std=c11")) { ok = false; }
        if (ok && !argv_push(&av, "-O2")) { ok = false; }
        if (ok && !argv_push(&av, "-Wall")) { ok = false; }
        if (ok && !argv_push(&av, "-Wextra")) { ok = false; }
        if (ok && !argv_push(&av, "-pedantic")) { ok = false; }
        /* Generated C may emit fit-helper functions that are not exercised
         * by the current program (e.g. unused coercion sites). They are
         * intentional artefacts of codegen, not user code, so silence the
         * resulting -Wunused-function noise on the host compile. */
        if (ok && !argv_push(&av, "-Wno-unused-function")) { ok = false; }
        if (ok && !argv_push(&av, "-Wno-unused-variable")) { ok = false; }
        if (ok && !argv_push(&av, "-Wno-unused-label")) { ok = false; }
        if (ok && !argv_push(&av, include_flag)) { ok = false; }
        if (ok && !argv_push(&av, opts->c_path)) { ok = false; }
        for (size_t i = 0; ok && i < bundle_lib_count; ++i) {
            if (!argv_push(&av, bundle_libs[i])) { ok = false; }
        }
        if (ok && !argv_push(&av, runtime_lib)) { ok = false; }
        if (ok && !argv_push(&av, "-lpthread")) { ok = false; }
        for (size_t i = 0; ok && i < lib_count; ++i) {
            size_t need = strlen(libs[i]) + 3U;
            char *flag = malloc(need);
            if (flag == NULL) {
                ok = false;
                break;
            }
            snprintf(flag, need, "-l%s", libs[i]);
            ok = argv_push(&av, flag);
            free(flag);
        }
        if (ok && !argv_push(&av, "-o")) { ok = false; }
        if (ok && !argv_push(&av, opts->out_path)) { ok = false; }
        if (!ok) {
            fprintf(stderr, "error: out of memory building cc argv\n");
            rc = 1;
        } else {
            rc = spawn_and_wait(av.items);
        }
        argv_free(&av);
    } else {
        object_path = replace_with_sibling_filename(opts->c_path, "feng.o");
        if (object_path == NULL) {
            fprintf(stderr, "error: out of memory composing object path\n");
            rc = 1;
        } else {
            if (!argv_push(&av, cc)) { ok = false; }
            if (ok && !argv_push(&av, "-std=c11")) { ok = false; }
            if (ok && !argv_push(&av, "-O2")) { ok = false; }
            if (ok && !argv_push(&av, "-Wall")) { ok = false; }
            if (ok && !argv_push(&av, "-Wextra")) { ok = false; }
            if (ok && !argv_push(&av, "-pedantic")) { ok = false; }
            /* See bin path above: silence unused-function noise from
             * generated fit helpers for the lib compile too. */
            if (ok && !argv_push(&av, "-Wno-unused-function")) { ok = false; }
            if (ok && !argv_push(&av, "-Wno-unused-variable")) { ok = false; }
            if (ok && !argv_push(&av, "-Wno-unused-label")) { ok = false; }
            if (ok && !argv_push(&av, include_flag)) { ok = false; }
            if (ok && !argv_push(&av, "-c")) { ok = false; }
            if (ok && !argv_push(&av, opts->c_path)) { ok = false; }
            if (ok && !argv_push(&av, "-o")) { ok = false; }
            if (ok && !argv_push(&av, object_path)) { ok = false; }
            if (!ok) {
                fprintf(stderr, "error: out of memory building cc argv\n");
                rc = 1;
            } else {
                rc = spawn_and_wait(av.items);
            }
            argv_free(&av);
        }

        if (rc == 0) {
            const char *ar = getenv("AR");
            if (ar == NULL || ar[0] == '\0') ar = "ar";
            if (!argv_push(&av, ar)) { ok = false; }
            if (ok && !argv_push(&av, "rcs")) { ok = false; }
            if (ok && !argv_push(&av, opts->out_path)) { ok = false; }
            if (ok && !argv_push(&av, object_path)) { ok = false; }
            if (!ok) {
                fprintf(stderr, "error: out of memory building archive argv\n");
                rc = 1;
            } else {
                rc = spawn_and_wait(av.items);
            }
            argv_free(&av);
        }
    }

    for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
    free(libs);
    free_string_array(bundle_libs, bundle_lib_count);
    remove_tree(bundle_temp_dir);
    free(bundle_temp_dir);
    free(host_target);
    free(bundle_error);
    free(runtime_lib);
    free(include_dir);
    free(include_flag);

    if (rc != 0) {
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
