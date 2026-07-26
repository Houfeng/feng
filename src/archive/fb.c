#include "archive/fb.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "archive/zip.h"
#include "platform/platform.h"

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

    message = (char *)malloc((size_t)needed + 1U);
    if (message == NULL) {
        va_end(args_copy);
        return false;
    }
    vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    *out_error_message = message;
    return false;
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

static bool appendf(char **buffer, size_t *length, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *resized;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return false;
    }
    resized = (char *)realloc(*buffer, *length + (size_t)needed + 1U);
    if (resized == NULL) {
        va_end(args_copy);
        return false;
    }
    *buffer = resized;
    vsnprintf(*buffer + *length, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    *length += (size_t)needed;
    return true;
}

static const char *path_basename(const char *path) {
    const char *slash;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool file_exists(const char *path) {
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const char *path) {
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Ensure the parent directory of one output file exists. */
static bool ensure_parent_directory(const char *path, char **out_error_message) {
    char *parent;
    char *cursor;
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        return true;
    }
    parent = dup_printf("%.*s", (int)(slash - path), path);
    if (parent == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    for (cursor = parent + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(parent, 0775) != 0 && errno != EEXIST) {
                bool result = set_errorf(out_error_message,
                                         "failed to create directory %s: %s",
                                         parent,
                                         strerror(errno));
                free(parent);
                return result;
            }
            *cursor = '/';
        }
    }
    if (parent[0] != '\0' && mkdir(parent, 0775) != 0 && errno != EEXIST) {
        bool result = set_errorf(out_error_message,
                                 "failed to create directory %s: %s",
                                 parent,
                                 strerror(errno));
        free(parent);
        return result;
    }
    free(parent);
    return true;
}

static int compare_strings(const void *a, const void *b) {
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcmp(*lhs, *rhs);
}

static bool name_has_suffix(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);

    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

static bool include_mod_file(const char *name) {
    return name_has_suffix(name, ".ft");
}

static bool include_any_regular_file(const char *name) {
    (void)name;
    return true;
}

static bool collect_sorted_names(const char *disk_dir,
                                 char ***out_names,
                                 size_t *out_count,
                                 char **out_error_message) {
    DIR *dir = NULL;
    struct dirent *entry;
    char **names = NULL;
    size_t name_count = 0U;
    size_t name_capacity = 0U;
    size_t index;
    bool ok = false;

    *out_names = NULL;
    *out_count = 0U;

    dir = opendir(disk_dir);
    if (dir == NULL) {
        set_errorf(out_error_message,
                   "failed to scan directory %s: %s",
                   disk_dir,
                   strerror(errno));
        goto done;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *copy;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (name_count == name_capacity) {
            size_t new_capacity = name_capacity == 0U ? 8U : name_capacity * 2U;
            char **resized = (char **)realloc(names, new_capacity * sizeof(char *));
            if (resized == NULL) {
                set_errorf(out_error_message, "out of memory");
                goto done;
            }
            names = resized;
            name_capacity = new_capacity;
        }
        copy = dup_printf("%s", entry->d_name);
        if (copy == NULL) {
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
        names[name_count++] = copy;
    }

    qsort(names, name_count, sizeof(char *), compare_strings);
    *out_names = names;
    *out_count = name_count;
    names = NULL;
    name_count = 0U;
    ok = true;

done:
    if (dir != NULL) {
        closedir(dir);
    }
    for (index = 0U; index < name_count; ++index) {
        free(names[index]);
    }
    free(names);
    return ok;
}

static bool tree_has_payload(const char *disk_dir,
                             bool (*include_file)(const char *name),
                             bool *out_has_payload,
                             char **out_error_message) {
    DIR *dir = NULL;
    struct dirent *entry;
    bool has_payload = false;
    bool ok = false;

    *out_has_payload = false;

    dir = opendir(disk_dir);
    if (dir == NULL) {
        set_errorf(out_error_message,
                   "failed to scan directory %s: %s",
                   disk_dir,
                   strerror(errno));
        goto done;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *child_disk = NULL;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        child_disk = dup_printf("%s/%s", disk_dir, entry->d_name);
        if (child_disk == NULL) {
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
        if (stat(child_disk, &st) != 0) {
            set_errorf(out_error_message,
                       "failed to stat %s: %s",
                       child_disk,
                       strerror(errno));
            free(child_disk);
            goto done;
        }
        if (S_ISDIR(st.st_mode)) {
            bool child_has_payload = false;

            if (!tree_has_payload(child_disk,
                                  include_file,
                                  &child_has_payload,
                                  out_error_message)) {
                free(child_disk);
                goto done;
            }
            if (child_has_payload) {
                has_payload = true;
                free(child_disk);
                break;
            }
        } else if (S_ISREG(st.st_mode) && include_file(entry->d_name)) {
            has_payload = true;
            free(child_disk);
            break;
        }
        free(child_disk);
    }

    *out_has_payload = has_payload;
    ok = true;

done:
    if (dir != NULL) {
        closedir(dir);
    }
    return ok;
}

/* Recursively walks `disk_dir` and mirrors selected regular files into the
 * open zip `writer` beneath `entry_prefix/`. When `add_root_directory` is
 * true, the root directory entry itself is emitted only if the subtree has at
 * least one included regular file, which avoids empty-directory noise in the
 * bundle. Entries are emitted in lexicographic order to keep bundles
 * deterministic. */
static bool add_tree_filtered(FengZipWriter *writer,
                              const char *disk_dir,
                              const char *entry_prefix,
                              bool (*include_file)(const char *name),
                              bool add_root_directory,
                              char **out_error_message) {
    char **names = NULL;
    size_t name_count = 0U;
    size_t index;
    bool ok = false;
    bool has_payload = false;

    if (!dir_exists(disk_dir)) {
        set_errorf(out_error_message,
                   "bundle directory not found: %s",
                   disk_dir != NULL ? disk_dir : "(null)");
        goto done;
    }
    if (add_root_directory) {
        if (!tree_has_payload(disk_dir, include_file, &has_payload, out_error_message)) {
            goto done;
        }
        if (!has_payload) {
            ok = true;
            goto done;
        }
        if (!feng_zip_writer_add_directory(writer, entry_prefix, out_error_message)) {
            goto done;
        }
    }
    if (!collect_sorted_names(disk_dir, &names, &name_count, out_error_message)) {
        goto done;
    }

    for (index = 0U; index < name_count; ++index) {
        const char *name = names[index];
        char *child_disk = dup_printf("%s/%s", disk_dir, name);
        char *child_entry = dup_printf("%s/%s", entry_prefix, name);
        struct stat st;

        if (child_disk == NULL || child_entry == NULL) {
            free(child_disk);
            free(child_entry);
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
        if (stat(child_disk, &st) != 0) {
            set_errorf(out_error_message,
                       "failed to stat %s: %s",
                       child_disk,
                       strerror(errno));
            free(child_disk);
            free(child_entry);
            goto done;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!add_tree_filtered(writer,
                                   child_disk,
                                   child_entry,
                                   include_file,
                                   true,
                                   out_error_message)) {
                free(child_disk);
                free(child_entry);
                goto done;
            }
        } else if (S_ISREG(st.st_mode) && include_file(name)) {
            if (!feng_zip_writer_add_file(writer,
                                          child_entry,
                                          child_disk,
                                          FENG_ZIP_COMPRESSION_DEFLATE,
                                          out_error_message)) {
                free(child_disk);
                free(child_entry);
                goto done;
            }
        }
        free(child_disk);
        free(child_entry);
    }

    ok = true;

done:
    for (index = 0U; index < name_count; ++index) {
        free(names[index]);
    }
    free(names);
    return ok;
}

static bool write_bundle_to_path(const FengFbLibraryBundleSpec *spec,
                                 const char *archive_path,
                                 char **out_error_message) {
    FengZipWriter writer = {0};
    char *manifest = NULL;
    size_t manifest_length = 0U;
    size_t index;
    bool ok = false;

    if (!appendf(&manifest,
                 &manifest_length,
                 "[package]\nname: \"%s\"\nversion: \"%s\"\nplatform: \"",
                 spec->package_name,
                 spec->package_version)) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    for (index = 0U; index < spec->platform_artifact_count; ++index) {
        if (!appendf(&manifest,
                     &manifest_length,
                     "%s%s",
                     index > 0U ? "," : "",
                     spec->platform_artifacts[index].platform)) {
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
    }
    if (!appendf(&manifest, &manifest_length, "\"\nabi: \"feng\"\n")) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    if (spec->dependency_count > 0U) {
        if (!appendf(&manifest, &manifest_length, "\n[dependencies]\n")) {
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
        for (index = 0U; index < spec->dependency_count; ++index) {
            if (!appendf(&manifest,
                         &manifest_length,
                         "%s: \"%s\"\n",
                         spec->dependencies[index].name,
                         spec->dependencies[index].version)) {
                set_errorf(out_error_message, "out of memory");
                goto done;
            }
        }
    }
    if (!feng_zip_writer_open(archive_path, &writer, out_error_message)) {
        goto done;
    }
    if (!feng_zip_writer_add_bytes(&writer,
                                   "feng.fm",
                                   manifest,
                                   strlen(manifest),
                                   FENG_ZIP_COMPRESSION_DEFLATE,
                                   out_error_message)) {
        goto done;
    }
    /* Keep the stable mod/ location present even when a package exports no
     * public declarations. */
    if (!feng_zip_writer_add_directory(&writer, "mod", out_error_message)) {
        goto done;
    }
    if (spec->public_mod_root != NULL && dir_exists(spec->public_mod_root)) {
        if (!add_tree_filtered(&writer,
                               spec->public_mod_root,
                               "mod",
                               include_mod_file,
                               false,
                               out_error_message)) {
            goto done;
        }
    }
    if (!feng_zip_writer_add_directory(&writer, "lib", out_error_message)) {
        goto done;
    }
    for (index = 0U; index < spec->platform_artifact_count; ++index) {
        const FengFbBundlePlatformArtifact *artifact =
            &spec->platform_artifacts[index];
        char *library_dir = dup_printf("lib/%s", artifact->platform);
        char *library_entry = library_dir != NULL
            ? dup_printf("%s/%s",
                         library_dir,
                         path_basename(artifact->library_path))
            : NULL;

        if (library_dir == NULL || library_entry == NULL) {
            free(library_entry);
            free(library_dir);
            set_errorf(out_error_message, "out of memory");
            goto done;
        }
        if (!feng_zip_writer_add_directory(&writer,
                                           library_dir,
                                           out_error_message) ||
            !feng_zip_writer_add_file(&writer,
                                      library_entry,
                                      artifact->library_path,
                                      FENG_ZIP_COMPRESSION_STORE,
                                      out_error_message)) {
            free(library_entry);
            free(library_dir);
            goto done;
        }
        free(library_entry);
        free(library_dir);

        if (artifact->extlib_root != NULL) {
            char *extlib_entry = dup_printf("extlib/%s", artifact->platform);

            if (extlib_entry == NULL) {
                set_errorf(out_error_message, "out of memory");
                goto done;
            }
            if (!add_tree_filtered(&writer,
                                   artifact->extlib_root,
                                   extlib_entry,
                                   include_any_regular_file,
                                   true,
                                   out_error_message)) {
                free(extlib_entry);
                goto done;
            }
            free(extlib_entry);
        }
    }
    for (index = 0U; index < spec->asset_entry_count; ++index) {
        const FengFbBundleDirectoryEntry *entry = &spec->asset_entries[index];

        if (!add_tree_filtered(&writer,
                               entry->source_root,
                               entry->entry_path,
                               include_any_regular_file,
                               true,
                               out_error_message)) {
            goto done;
        }
    }
    if (!feng_zip_writer_finalize(&writer, out_error_message)) {
        goto done;
    }

    ok = true;

done:
    feng_zip_writer_dispose(&writer);
    free(manifest);
    return ok;
}

bool feng_fb_write_library_bundle(const FengFbLibraryBundleSpec *spec,
                                  char **out_error_message) {
    char *temp_path = NULL;
    int temp_fd = -1;
    size_t index;
    bool ok = false;

    if (spec == NULL) {
        return set_errorf(out_error_message, "bundle spec must not be null");
    }
    if (spec->package_path == NULL || spec->package_path[0] == '\0') {
        return set_errorf(out_error_message, "bundle output path must not be empty");
    }
    if (spec->package_name == NULL || spec->package_name[0] == '\0') {
        return set_errorf(out_error_message, "bundle package name must not be empty");
    }
    if (spec->package_version == NULL || spec->package_version[0] == '\0') {
        return set_errorf(out_error_message,
                          "bundle package version must not be empty");
    }
    if (spec->platform_artifact_count == 0U ||
        spec->platform_artifacts == NULL) {
        return set_errorf(out_error_message,
                          "bundle platform artifacts must not be empty");
    }
    if (spec->asset_entry_count > 0U && spec->asset_entries == NULL) {
        return set_errorf(out_error_message,
                          "bundle asset entries must not be null when count is non-zero");
    }
    for (index = 0U; index < spec->platform_artifact_count; ++index) {
        const FengFbBundlePlatformArtifact *artifact =
            &spec->platform_artifacts[index];
        size_t prior_index;

        if (!feng_platform_is_valid(artifact->platform)) {
            return set_errorf(out_error_message,
                              "invalid bundle target platform: %s",
                              artifact->platform != NULL
                                  ? artifact->platform
                                  : "(null)");
        }
        for (prior_index = 0U; prior_index < index; ++prior_index) {
            if (strcmp(spec->platform_artifacts[prior_index].platform,
                       artifact->platform) == 0) {
                return set_errorf(out_error_message,
                                  "duplicate bundle target platform: %s",
                                  artifact->platform);
            }
        }
        if (!file_exists(artifact->library_path)) {
            return set_errorf(out_error_message,
                              "bundle library artifact not found: %s",
                              artifact->library_path != NULL
                                  ? artifact->library_path
                                  : "(null)");
        }
        if (artifact->extlib_root != NULL &&
            !dir_exists(artifact->extlib_root)) {
            return set_errorf(out_error_message,
                              "bundle extlib directory not found: %s",
                              artifact->extlib_root);
        }
    }
    for (index = 0U; index < spec->asset_entry_count; ++index) {
        const FengFbBundleDirectoryEntry *entry = &spec->asset_entries[index];

        if (entry->entry_path == NULL || entry->entry_path[0] == '\0') {
            return set_errorf(out_error_message,
                              "bundle asset entry path must not be empty");
        }
        if (entry->source_root == NULL || entry->source_root[0] == '\0') {
            return set_errorf(out_error_message,
                              "bundle asset source directory must not be empty");
        }
        if (!dir_exists(entry->source_root)) {
            return set_errorf(out_error_message,
                              "bundle asset source directory not found: %s",
                              entry->source_root);
        }
    }
    if (!ensure_parent_directory(spec->package_path, out_error_message)) {
        goto done;
    }

    temp_path = dup_printf("%s.tmp.XXXXXX", spec->package_path);
    if (temp_path == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        set_errorf(out_error_message,
                   "failed to create temporary package path for %s: %s",
                   spec->package_path,
                   strerror(errno));
        goto done;
    }
    close(temp_fd);
    temp_fd = -1;

    if (!write_bundle_to_path(spec, temp_path, out_error_message)) {
        goto done;
    }
    if (rename(temp_path, spec->package_path) != 0) {
        set_errorf(out_error_message,
                   "failed to publish bundle %s: %s",
                   spec->package_path,
                   strerror(errno));
        goto done;
    }

    ok = true;

done:
    if (temp_fd >= 0) {
        close(temp_fd);
    }
    if (!ok && temp_path != NULL) {
        unlink(temp_path);
    }
    free(temp_path);
    return ok;
}
