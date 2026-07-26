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
#include "platform/platform.h"

typedef struct SourceList {
    char **items;
    size_t count;
    size_t capacity;
} SourceList;

static bool remove_tree_inner(const char *path, char **out_error_message);

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

static bool mkdir_p_path(const char *path, FengCliProjectError *error) {
    char *mutable_path;
    size_t index;

    mutable_path = dup_cstr(path);
    if (mutable_path == NULL) {
        set_error(error, path, 0U, "out of memory");
        return false;
    }
    for (index = 1U; mutable_path[index] != '\0'; ++index) {
        if (mutable_path[index] == '/') {
            mutable_path[index] = '\0';
            if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
                set_error(error,
                          path,
                          0U,
                          "failed to create directory %s: %s",
                          mutable_path,
                          strerror(errno));
                free(mutable_path);
                return false;
            }
            mutable_path[index] = '/';
        }
    }
    if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
        set_error(error,
                  path,
                  0U,
                  "failed to create directory %s: %s",
                  mutable_path,
                  strerror(errno));
        free(mutable_path);
        return false;
    }
    free(mutable_path);
    return true;
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

static bool collect_directory_entries(const char *root,
                                      SourceList *list,
                                      FengCliProjectError *error) {
    DIR *dir = opendir(root);
    struct dirent *entry;

    if (dir == NULL) {
        set_error(error, root, 0U, "failed to open directory: %s", strerror(errno));
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *name;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        name = dup_cstr(entry->d_name);
        if (name == NULL) {
            closedir(dir);
            set_error(error, root, 0U, "out of memory while scanning directory");
            return false;
        }
        if (!source_list_push(list, name)) {
            free(name);
            closedir(dir);
            set_error(error, root, 0U, "out of memory while scanning directory");
            return false;
        }
    }

    closedir(dir);
    qsort(list->items, list->count, sizeof(*list->items), compare_cstr_ptr);
    return true;
}

static bool copy_regular_file(const char *source_path,
                              const char *dest_path,
                              FengCliProjectError *error) {
    FILE *source = NULL;
    FILE *dest = NULL;
    char *parent_dir = NULL;
    char buffer[8192];
    size_t read_size;

    parent_dir = path_dirname_dup(dest_path);
    if (parent_dir == NULL) {
        set_error(error, dest_path, 0U, "out of memory");
        return false;
    }
    if (!mkdir_p_path(parent_dir, error)) {
        free(parent_dir);
        return false;
    }
    free(parent_dir);

    source = fopen(source_path, "rb");
    if (source == NULL) {
        set_error(error, source_path, 0U, "failed to open %s: %s", source_path, strerror(errno));
        return false;
    }
    dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        fclose(source);
        set_error(error, dest_path, 0U, "failed to open %s: %s", dest_path, strerror(errno));
        return false;
    }

    while ((read_size = fread(buffer, 1U, sizeof(buffer), source)) > 0U) {
        if (fwrite(buffer, 1U, read_size, dest) != read_size) {
            fclose(dest);
            fclose(source);
            set_error(error, dest_path, 0U, "failed to write %s: %s", dest_path, strerror(errno));
            return false;
        }
    }
    if (ferror(source)) {
        fclose(dest);
        fclose(source);
        set_error(error, source_path, 0U, "failed to read %s", source_path);
        return false;
    }

    fclose(dest);
    fclose(source);
    return true;
}

static bool copy_directory_recursive(const char *source_dir,
                                     const char *dest_dir,
                                     FengCliProjectError *error) {
    SourceList entries = {0};
    size_t index;

    if (!mkdir_p_path(dest_dir, error)) {
        return false;
    }
    if (!collect_directory_entries(source_dir, &entries, error)) {
        source_list_dispose(&entries);
        return false;
    }

    for (index = 0U; index < entries.count; ++index) {
        char *source_child = path_join(source_dir, entries.items[index]);
        char *dest_child = path_join(dest_dir, entries.items[index]);
        struct stat st;

        if (source_child == NULL || dest_child == NULL) {
            free(source_child);
            free(dest_child);
            source_list_dispose(&entries);
            set_error(error, dest_dir, 0U, "out of memory");
            return false;
        }
        if (stat(source_child, &st) != 0) {
            set_error(error,
                      source_child,
                      0U,
                      "failed to stat %s: %s",
                      source_child,
                      strerror(errno));
            free(dest_child);
            free(source_child);
            source_list_dispose(&entries);
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!copy_directory_recursive(source_child, dest_child, error)) {
                free(dest_child);
                free(source_child);
                source_list_dispose(&entries);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!copy_regular_file(source_child, dest_child, error)) {
                free(dest_child);
                free(source_child);
                source_list_dispose(&entries);
                return false;
            }
        } else {
            set_error(error,
                      source_child,
                      0U,
                      "unsupported asset entry type at %s",
                      source_child);
            free(dest_child);
            free(source_child);
            source_list_dispose(&entries);
            return false;
        }

        free(dest_child);
        free(source_child);
    }

    source_list_dispose(&entries);
    return true;
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

bool feng_cli_project_asset_targets_extlib(const FengCliProjectManifestAsset *asset) {
    return asset != NULL && strcmp(asset->target_dir, "extlib") == 0;
}

static bool fill_output_paths(FengCliProjectContext *context, FengCliProjectError *error) {
    char *package_dir;
    char *package_name;

    package_dir = path_join(context->out_root, "pkg");
    if (package_dir == NULL) {
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }

    package_name = dup_printf("%s-%s.fb", context->manifest.name, context->manifest.version);
    if (package_name == NULL) {
        free(package_dir);
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }
    context->package_path = path_join(package_dir, package_name);
    free(package_dir);
    free(package_name);
    if (context->package_path == NULL) {
        set_error(error, context->out_root, 0U, "out of memory");
        return false;
    }
    return true;
}

static bool stage_single_asset(const FengCliProjectContext *context,
                               const FengCliProjectManifestAsset *asset,
                               const char *platform,
                               const char *binary_path,
                               const char *dest_root,
                               FengCliProjectError *error) {
    char *source_path = NULL;
    char *resolved_source = NULL;
    char *dest_path = NULL;
    char *remove_error = NULL;
    struct stat st;

    if (path_is_absolute(asset->source_path)) {
        set_error(error,
                  context->manifest_path,
                  asset->line,
                  "manifest field `[assets].%s` must use a path relative to feng.fm",
                  asset->target_dir);
        goto fail;
    }

    source_path = resolve_project_path(context->project_root, asset->source_path);
    if (source_path == NULL) {
        set_error(error, context->manifest_path, asset->line, "out of memory");
        goto fail;
    }
    if (feng_cli_project_asset_targets_extlib(asset)) {
        char *platform_source = path_join(source_path, platform);

        free(source_path);
        source_path = platform_source;
        if (source_path == NULL) {
            set_error(error, context->manifest_path, asset->line, "out of memory");
            goto fail;
        }
    }
    resolved_source = realpath(source_path, NULL);
    if (resolved_source == NULL) {
        set_error(error,
                  context->manifest_path,
                  asset->line,
                  "asset source directory for `[assets].%s` not found: %s",
                  asset->target_dir,
                  strerror(errno));
        goto fail;
    }
    if (stat(resolved_source, &st) != 0) {
        set_error(error,
                  resolved_source,
                  0U,
                  "failed to stat %s: %s",
                  resolved_source,
                  strerror(errno));
        goto fail;
    }
    if (!S_ISDIR(st.st_mode)) {
        set_error(error,
                  context->manifest_path,
                  asset->line,
                  "manifest field `[assets].%s` must reference a directory",
                  asset->target_dir);
        goto fail;
    }

    dest_path = path_join(dest_root, asset->target_dir);
    if (dest_path == NULL) {
        set_error(error, dest_root, 0U, "out of memory");
        goto fail;
    }
    if (context->manifest.target == FENG_COMPILE_TARGET_BIN &&
        strcmp(dest_path, binary_path) == 0) {
        set_error(error,
                  context->manifest_path,
                  asset->line,
                  "manifest field `[assets].%s` conflicts with generated binary output",
                  asset->target_dir);
        goto fail;
    }
    if (lstat(dest_path, &st) == 0) {
        if (!remove_tree_inner(dest_path, &remove_error)) {
            set_error(error,
                      dest_path,
                      0U,
                      "%s",
                      remove_error != NULL ? remove_error : "failed to refresh asset destination");
            goto fail;
        }
        free(remove_error);
        remove_error = NULL;
    } else if (errno != ENOENT) {
        set_error(error,
                  dest_path,
                  0U,
                  "failed to stat %s: %s",
                  dest_path,
                  strerror(errno));
        goto fail;
    }

    if (!copy_directory_recursive(resolved_source, dest_path, error)) {
        goto fail;
    }

    free(dest_path);
    free(resolved_source);
    free(source_path);
    return true;

fail:
    free(remove_error);
    free(dest_path);
    free(resolved_source);
    free(source_path);
    return false;
}

bool feng_cli_project_stage_assets(const FengCliProjectContext *context,
                                   const char *platform,
                                   FengCliProjectError *out_error) {
    size_t index;
    char *platform_out_root = NULL;
    char *binary_path = NULL;
    char *asset_stage_root = NULL;

    if (context == NULL || platform == NULL) {
        set_error(out_error, NULL, 0U, "invalid asset staging request");
        return false;
    }
    if (context->manifest.asset_count == 0U) {
        return true;
    }
    platform_out_root = feng_cli_project_platform_out_root(context, platform);
    binary_path = feng_cli_project_platform_binary_path(context, platform);
    asset_stage_root = platform_out_root != NULL
        ? path_join(platform_out_root, "assets")
        : NULL;
    if (platform_out_root == NULL || binary_path == NULL || asset_stage_root == NULL) {
        set_error(out_error, context->out_root, 0U, "out of memory");
        free(asset_stage_root);
        free(binary_path);
        free(platform_out_root);
        return false;
    }

    for (index = 0U; index < context->manifest.asset_count; ++index) {
        const FengCliProjectManifestAsset *asset = &context->manifest.assets[index];
        char *dest_root;

        if (context->manifest.target == FENG_COMPILE_TARGET_BIN) {
            dest_root = path_dirname_dup(binary_path);
        } else if (feng_cli_project_asset_targets_extlib(asset)) {
            dest_root = dup_cstr(platform_out_root);
        } else {
            dest_root = dup_cstr(asset_stage_root);
        }
        if (dest_root == NULL) {
            set_error(out_error, context->out_root, 0U, "out of memory");
            goto fail;
        }
        if (!stage_single_asset(context,
                                asset,
                                platform,
                                binary_path,
                                dest_root,
                                out_error)) {
            free(dest_root);
            goto fail;
        }
        free(dest_root);
    }
    free(asset_stage_root);
    free(binary_path);
    free(platform_out_root);
    return true;

fail:
    free(asset_stage_root);
    free(binary_path);
    free(platform_out_root);
    return false;
}

/* Return whether a manifest platform whitelist contains one platform. */
static bool manifest_contains_platform(const FengCliProjectManifest *manifest,
                                       const char *platform) {
    size_t index;

    for (index = 0U; index < manifest->platform_count; ++index) {
        if (strcmp(manifest->platforms[index], platform) == 0) {
            return true;
        }
    }
    return false;
}

/* Append one validated, unique platform to an owned selection. */
static bool platform_selection_append(FengCliProjectPlatformSelection *selection,
                                      const char *platform,
                                      const FengCliProjectContext *context,
                                      FengCliProjectError *error) {
    char **resized;
    char *copy;
    size_t index;

    if (!feng_platform_is_valid(platform)) {
        set_error(error,
                  context->manifest_path,
                  0U,
                  "invalid target platform: %s",
                  platform != NULL && platform[0] != '\0' ? platform : "(empty)");
        return false;
    }
    if (context->manifest.platform_count > 0U &&
        !manifest_contains_platform(&context->manifest, platform)) {
        set_error(error,
                  context->manifest_path,
                  0U,
                  "target platform is not declared by feng.fm: %s",
                  platform);
        return false;
    }
    for (index = 0U; index < selection->platform_count; ++index) {
        if (strcmp(selection->platforms[index], platform) == 0) {
            set_error(error,
                      context->manifest_path,
                      0U,
                      "target platform was selected more than once: %s",
                      platform);
            return false;
        }
    }
    copy = dup_cstr(platform);
    if (copy == NULL) {
        set_error(error, context->manifest_path, 0U, "out of memory");
        return false;
    }
    resized = (char **)realloc(
        selection->platforms,
        (selection->platform_count + 1U) * sizeof(*selection->platforms));
    if (resized == NULL) {
        free(copy);
        set_error(error, context->manifest_path, 0U, "out of memory");
        return false;
    }
    selection->platforms = resized;
    selection->platforms[selection->platform_count++] = copy;
    return true;
}

bool feng_cli_project_select_platforms(
    const FengCliProjectContext *context,
    const char *const *requested_platforms,
    size_t requested_platform_count,
    const char *sysroot,
    bool host_only,
    FengCliProjectPlatformSelection *out_selection,
    FengCliProjectError *out_error) {
    FengCliProjectPlatformSelection selection = {0};
    char *host_platform = NULL;
    char *host_error = NULL;
    size_t index;

    if (context == NULL || out_selection == NULL) {
        set_error(out_error, NULL, 0U, "invalid project platform selection request");
        return false;
    }
    if (host_only) {
        if (requested_platform_count > 0U || sysroot != NULL) {
            set_error(out_error,
                      context->manifest_path,
                      0U,
                      "host-only project command does not accept target platform or sysroot");
            return false;
        }
        if (!feng_platform_detect_host_platform(&host_platform, &host_error)) {
            set_error(out_error,
                      context->manifest_path,
                      0U,
                      "%s",
                      host_error != NULL ? host_error : "failed to detect host platform");
            free(host_error);
            return false;
        }
        if (!platform_selection_append(&selection,
                                       host_platform,
                                       context,
                                       out_error)) {
            free(host_platform);
            return false;
        }
        free(host_platform);
    } else if (requested_platform_count > 0U) {
        for (index = 0U; index < requested_platform_count; ++index) {
            if (!platform_selection_append(&selection,
                                           requested_platforms[index],
                                           context,
                                           out_error)) {
                feng_cli_project_platform_selection_dispose(&selection);
                return false;
            }
        }
    } else if (context->manifest.platform_count > 0U) {
        for (index = 0U; index < context->manifest.platform_count; ++index) {
            if (!platform_selection_append(&selection,
                                           context->manifest.platforms[index],
                                           context,
                                           out_error)) {
                feng_cli_project_platform_selection_dispose(&selection);
                return false;
            }
        }
    } else {
        if (!feng_platform_detect_host_platform(&host_platform, &host_error)) {
            set_error(out_error,
                      context->manifest_path,
                      0U,
                      "%s",
                      host_error != NULL ? host_error : "failed to detect host platform");
            free(host_error);
            return false;
        }
        if (!platform_selection_append(&selection,
                                       host_platform,
                                       context,
                                       out_error)) {
            free(host_platform);
            return false;
        }
        free(host_platform);
    }

    if (sysroot != NULL && selection.platform_count != 1U) {
        set_error(out_error,
                  context->manifest_path,
                  0U,
                  "--sysroot requires exactly one selected target platform");
        feng_cli_project_platform_selection_dispose(&selection);
        return false;
    }
    *out_selection = selection;
    return true;
}

void feng_cli_project_platform_selection_dispose(
    FengCliProjectPlatformSelection *selection) {
    size_t index;

    if (selection == NULL) {
        return;
    }
    for (index = 0U; index < selection->platform_count; ++index) {
        free(selection->platforms[index]);
    }
    free(selection->platforms);
    selection->platforms = NULL;
    selection->platform_count = 0U;
}

char *feng_cli_project_platform_out_root(const FengCliProjectContext *context,
                                         const char *platform) {
    if (context == NULL || context->out_root == NULL || platform == NULL) {
        return NULL;
    }
    return path_join(context->out_root, platform);
}

char *feng_cli_project_platform_binary_path(const FengCliProjectContext *context,
                                            const char *platform) {
    char *platform_out_root;
    char *bin_dir;
    char *binary_path;

    if (context == NULL) {
        return NULL;
    }
    platform_out_root = feng_cli_project_platform_out_root(context, platform);
    if (platform_out_root == NULL) {
        return NULL;
    }
    bin_dir = path_join(platform_out_root, "bin");
    free(platform_out_root);
    if (bin_dir == NULL) {
        return NULL;
    }
    binary_path = path_join(bin_dir, context->manifest.name);
    free(bin_dir);
    return binary_path;
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
