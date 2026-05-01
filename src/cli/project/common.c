#include "cli/project/common.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli/common.h"

typedef struct SourceList {
    char **items;
    size_t count;
    size_t capacity;
} SourceList;

static char *dup_n(const char *text, size_t length) {
    char *out = (char *)malloc(length + 1U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}

static char *dup_cstr(const char *text) {
    return dup_n(text, strlen(text));
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

    out = (char *)malloc((size_t)needed + 1U);
    if (out == NULL) {
        va_end(args_copy);
        return NULL;
    }
    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

static void set_error(FengCliProjectError *error,
                      const char *path,
                      unsigned int line,
                      const char *fmt,
                      ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message = NULL;

    if (error == NULL) {
        return;
    }

    feng_cli_project_error_dispose(error);
    error->path = path != NULL ? dup_cstr(path) : NULL;
    error->line = line;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed >= 0) {
        message = (char *)malloc((size_t)needed + 1U);
        if (message != NULL) {
            vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
        }
    }
    va_end(args_copy);
    error->message = message;
}

static bool path_is_absolute(const char *path) {
    return path != NULL && path[0] == '/';
}

static void strip_trailing_slashes(char *path) {
    size_t length;

    if (path == NULL) {
        return;
    }
    length = strlen(path);
    while (length > 1U && path[length - 1U] == '/') {
        path[length - 1U] = '\0';
        length--;
    }
}

static char *path_join(const char *lhs, const char *rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    bool need_sep = lhs_len > 0U && lhs[lhs_len - 1U] != '/';
    char *out = (char *)malloc(lhs_len + (need_sep ? 1U : 0U) + rhs_len + 1U);
    size_t cursor = 0U;

    if (out == NULL) {
        return NULL;
    }

    memcpy(out + cursor, lhs, lhs_len);
    cursor += lhs_len;
    if (need_sep) {
        out[cursor++] = '/';
    }
    memcpy(out + cursor, rhs, rhs_len);
    cursor += rhs_len;
    out[cursor] = '\0';
    return out;
}

static char *path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        return dup_cstr(".");
    }
    if (slash == path) {
        return dup_cstr("/");
    }
    return dup_n(path, (size_t)(slash - path));
}

static bool path_has_basename(const char *path, const char *basename) {
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    return strcmp(name, basename) == 0;
}

static bool path_has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool source_list_push(SourceList *list, char *path) {
    char **new_items;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0U ? 8U : list->capacity * 2U;
        new_items = (char **)realloc(list->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = path;
    return true;
}

static int compare_cstr_ptr(const void *lhs, const void *rhs) {
    const char *const *lhs_ptr = (const char *const *)lhs;
    const char *const *rhs_ptr = (const char *const *)rhs;
    return strcmp(*lhs_ptr, *rhs_ptr);
}

static void source_list_dispose(SourceList *list) {
    size_t index;

    if (list == NULL) {
        return;
    }
    for (index = 0U; index < list->count; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0U;
    list->capacity = 0U;
}

static bool collect_sources_recursive(const char *root,
                                      SourceList *list,
                                      FengCliProjectError *error) {
    DIR *dir = opendir(root);
    struct dirent *entry;

    if (dir == NULL) {
        set_error(error, root, 0U, "failed to open source directory: %s", strerror(errno));
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        char *child;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        child = path_join(root, entry->d_name);
        if (child == NULL) {
            closedir(dir);
            set_error(error, root, 0U, "out of memory while collecting sources");
            return false;
        }
        if (stat(child, &st) != 0) {
            set_error(error, child, 0U, "failed to stat path: %s", strerror(errno));
            free(child);
            closedir(dir);
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!collect_sources_recursive(child, list, error)) {
                free(child);
                closedir(dir);
                return false;
            }
            free(child);
            continue;
        }

        if (S_ISREG(st.st_mode) && path_has_suffix(child, ".ff")) {
            if (!source_list_push(list, child)) {
                set_error(error, child, 0U, "out of memory while collecting sources");
                free(child);
                closedir(dir);
                return false;
            }
        } else {
            free(child);
        }
    }

    closedir(dir);
    return true;
}

bool feng_cli_project_resolve_manifest_path(const char *path_arg,
                                            char **out_manifest_path,
                                            FengCliProjectError *error) {
    struct stat st;
    char cwd_buffer[4096];
    char *base_path = NULL;
    char *manifest_path = NULL;
    char *resolved = NULL;

    *out_manifest_path = NULL;

    if (path_arg == NULL) {
        if (getcwd(cwd_buffer, sizeof(cwd_buffer)) == NULL) {
            set_error(error, NULL, 0U, "failed to resolve current directory: %s", strerror(errno));
            return false;
        }
        base_path = dup_cstr(cwd_buffer);
        if (base_path == NULL) {
            set_error(error, NULL, 0U, "out of memory");
            return false;
        }
        manifest_path = path_join(base_path, "feng.fm");
        free(base_path);
        base_path = NULL;
    } else if (stat(path_arg, &st) == 0 && S_ISDIR(st.st_mode)) {
        resolved = realpath(path_arg, NULL);
        if (resolved == NULL) {
            set_error(error, path_arg, 0U, "failed to resolve project directory: %s", strerror(errno));
            return false;
        }
        manifest_path = path_join(resolved, "feng.fm");
        free(resolved);
    } else {
        manifest_path = realpath(path_arg, NULL);
        if (manifest_path == NULL) {
            set_error(error, path_arg, 0U, "failed to resolve manifest path: %s", strerror(errno));
            return false;
        }
    }

    if (manifest_path == NULL) {
        set_error(error, path_arg, 0U, "out of memory");
        return false;
    }
    if (stat(manifest_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        set_error(error, manifest_path, 0U, "manifest file not found");
        free(manifest_path);
        return false;
    }
    *out_manifest_path = manifest_path;
    return true;
}

bool feng_cli_project_find_manifest_in_ancestors(const char *path_arg,
                                                 char **out_manifest_path,
                                                 FengCliProjectError *error) {
    struct stat st;
    char *resolved_path = NULL;
    char *current_dir = NULL;

    *out_manifest_path = NULL;

    if (path_arg == NULL) {
        return feng_cli_project_resolve_manifest_path(NULL, out_manifest_path, error);
    }
    if (stat(path_arg, &st) != 0) {
        set_error(error, path_arg, 0U, "failed to resolve project path: %s", strerror(errno));
        return false;
    }

    resolved_path = realpath(path_arg, NULL);
    if (resolved_path == NULL) {
        set_error(error, path_arg, 0U, "failed to resolve project path: %s", strerror(errno));
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        current_dir = resolved_path;
        resolved_path = NULL;
    } else if (S_ISREG(st.st_mode) && path_has_basename(resolved_path, "feng.fm")) {
        *out_manifest_path = resolved_path;
        return true;
    } else if (S_ISREG(st.st_mode)) {
        current_dir = path_dirname_dup(resolved_path);
        free(resolved_path);
        resolved_path = NULL;
        if (current_dir == NULL) {
            set_error(error, path_arg, 0U, "out of memory");
            return false;
        }
    } else {
        free(resolved_path);
        set_error(error, path_arg, 0U, "project path must point to a directory or regular file");
        return false;
    }

    while (current_dir != NULL) {
        char *candidate = path_join(current_dir, "feng.fm");

        if (candidate == NULL) {
            free(current_dir);
            set_error(error, path_arg, 0U, "out of memory");
            return false;
        }
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
            free(current_dir);
            *out_manifest_path = candidate;
            return true;
        }
        free(candidate);

        if (strcmp(current_dir, "/") == 0) {
            break;
        }

        {
            char *parent_dir = path_dirname_dup(current_dir);

            if (parent_dir == NULL) {
                free(current_dir);
                set_error(error, path_arg, 0U, "out of memory");
                return false;
            }
            if (strcmp(parent_dir, current_dir) == 0) {
                free(parent_dir);
                break;
            }
            free(current_dir);
            current_dir = parent_dir;
        }
    }

    free(current_dir);
    set_error(error, path_arg, 0U, "manifest file not found");
    return false;
}

static char *resolve_project_path(const char *project_root, const char *raw_path) {
    char *joined;

    if (path_is_absolute(raw_path)) {
        joined = dup_cstr(raw_path);
    } else {
        joined = path_join(project_root, raw_path);
    }
    if (joined != NULL) {
        strip_trailing_slashes(joined);
    }
    return joined;
}

static bool fill_output_paths(FengCliProjectContext *context, FengCliProjectError *error) {
    char *bin_dir;
    char *package_name;

    bin_dir = path_join(context->out_root, "bin");
    if (bin_dir == NULL) {
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }
    context->binary_path = path_join(bin_dir, context->manifest.name);
    free(bin_dir);
    if (context->binary_path == NULL) {
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }

    package_name = dup_printf("%s-%s.fb", context->manifest.name, context->manifest.version);
    if (package_name == NULL) {
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }
    context->package_path = path_join(context->out_root, package_name);
    free(package_name);
    if (context->package_path == NULL) {
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }
    return true;
}

bool feng_cli_project_open(const char *path_arg,
                           FengCliProjectContext *out_context,
                           FengCliProjectError *out_error) {
    FengCliProjectContext context = {0};
    char *manifest_source = NULL;
    size_t manifest_length = 0U;
    char *resolved_source_root = NULL;
    SourceList sources = {0};

    if (out_context == NULL) {
        set_error(out_error, NULL, 0U, "invalid project open request");
        return false;
    }

    if (!feng_cli_project_resolve_manifest_path(path_arg, &context.manifest_path, out_error)) {
        return false;
    }
    manifest_source = feng_cli_read_entire_file(context.manifest_path, &manifest_length);
    (void)manifest_length;
    if (manifest_source == NULL) {
        set_error(out_error, context.manifest_path, 0U, "failed to read manifest: %s", strerror(errno));
        goto fail;
    }
    if (!feng_cli_project_manifest_parse(context.manifest_path,
                                         manifest_source,
                                         &context.manifest,
                                         out_error)) {
        goto fail;
    }

    context.project_root = path_dirname_dup(context.manifest_path);
    if (context.project_root == NULL) {
        set_error(out_error, context.manifest_path, 0U, "out of memory");
        goto fail;
    }

    context.source_root = resolve_project_path(context.project_root, context.manifest.src_path);
    if (context.source_root == NULL) {
        set_error(out_error, context.project_root, 0U, "out of memory");
        goto fail;
    }
    resolved_source_root = realpath(context.source_root, NULL);
    if (resolved_source_root == NULL) {
        set_error(out_error, context.source_root, 0U, "source directory not found: %s", strerror(errno));
        goto fail;
    }
    free(context.source_root);
    context.source_root = resolved_source_root;
    resolved_source_root = NULL;

    if (!collect_sources_recursive(context.source_root, &sources, out_error)) {
        goto fail;
    }
    if (sources.count == 0U) {
        set_error(out_error, context.source_root, 0U, "no .ff source files found under source root");
        goto fail;
    }
    qsort(sources.items, sources.count, sizeof(*sources.items), compare_cstr_ptr);
    context.source_paths = sources.items;
    context.source_count = sources.count;
    sources.items = NULL;
    sources.count = 0U;
    sources.capacity = 0U;

    context.out_root = resolve_project_path(context.project_root, context.manifest.out_path);
    if (context.out_root == NULL) {
        set_error(out_error, context.project_root, 0U, "out of memory");
        goto fail;
    }
    if (!fill_output_paths(&context, out_error)) {
        goto fail;
    }

    free(manifest_source);
    *out_context = context;
    return true;

fail:
    free(resolved_source_root);
    free(manifest_source);
    source_list_dispose(&sources);
    feng_cli_project_context_dispose(&context);
    return false;
}

void feng_cli_project_context_dispose(FengCliProjectContext *context) {
    size_t index;

    if (context == NULL) {
        return;
    }

    free(context->manifest_path);
    free(context->project_root);
    free(context->source_root);
    free(context->out_root);
    free(context->binary_path);
    free(context->package_path);
    for (index = 0U; index < context->source_count; ++index) {
        free(context->source_paths[index]);
    }
    free(context->source_paths);
    feng_cli_project_manifest_dispose(&context->manifest);

    context->manifest_path = NULL;
    context->project_root = NULL;
    context->source_root = NULL;
    context->out_root = NULL;
    context->binary_path = NULL;
    context->package_path = NULL;
    context->source_paths = NULL;
    context->source_count = 0U;
}

void feng_cli_project_print_error(FILE *stream, const FengCliProjectError *error) {
    if (error == NULL || error->message == NULL) {
        return;
    }
    if (error->path != NULL) {
        if (error->line > 0U) {
            fprintf(stream, "%s:%u: %s\n", error->path, error->line, error->message);
        } else {
            fprintf(stream, "%s: %s\n", error->path, error->message);
        }
    } else {
        fprintf(stream, "%s\n", error->message);
    }
}

static bool remove_tree_inner(const char *path, char **out_error_message) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        if (out_error_message != NULL) {
            *out_error_message = dup_printf("failed to stat %s: %s", path, strerror(errno));
        }
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir == NULL) {
            if (out_error_message != NULL) {
                *out_error_message = dup_printf("failed to open %s: %s", path, strerror(errno));
            }
            return false;
        }
        while ((entry = readdir(dir)) != NULL) {
            char *child;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            child = path_join(path, entry->d_name);
            if (child == NULL) {
                closedir(dir);
                if (out_error_message != NULL) {
                    *out_error_message = dup_cstr("out of memory while cleaning project output");
                }
                return false;
            }
            if (!remove_tree_inner(child, out_error_message)) {
                free(child);
                closedir(dir);
                return false;
            }
            free(child);
        }
        closedir(dir);
        if (rmdir(path) != 0 && errno != ENOENT) {
            if (out_error_message != NULL) {
                *out_error_message = dup_printf("failed to remove directory %s: %s", path, strerror(errno));
            }
            return false;
        }
        return true;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        if (out_error_message != NULL) {
            *out_error_message = dup_printf("failed to remove %s: %s", path, strerror(errno));
        }
        return false;
    }
    return true;
}

bool feng_cli_project_remove_tree(const char *path, char **out_error_message) {
    if (out_error_message != NULL) {
        *out_error_message = NULL;
    }
    return remove_tree_inner(path, out_error_message);
}
