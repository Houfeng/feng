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

static int compare_strings(const void *a, const void *b) {
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcmp(*lhs, *rhs);
}

/* Recursively walks `disk_dir` and mirrors its contents into the open zip
 * `writer` under `entry_prefix/`. Only regular files whose name ends with
 * `.ft` are added; intermediate directories are emitted as zip directory
 * entries so the bundle layout matches the on-disk module tree. Entries are
 * added in lexicographic order to keep the produced bundle deterministic. */
static bool add_mod_tree(FengZipWriter *writer,
                         const char *disk_dir,
                         const char *entry_prefix,
                         char **out_error_message) {
    DIR *dir = NULL;
    struct dirent *entry;
    char **names = NULL;
    size_t name_count = 0U;
    size_t name_capacity = 0U;
    size_t index;
    bool ok = false;

    dir = opendir(disk_dir);
    if (dir == NULL) {
        set_errorf(out_error_message,
                   "failed to scan public module directory %s: %s",
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
    closedir(dir);
    dir = NULL;

    qsort(names, name_count, sizeof(char *), compare_strings);

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
            if (!feng_zip_writer_add_directory(writer, child_entry, out_error_message)) {
                free(child_disk);
                free(child_entry);
                goto done;
            }
            if (!add_mod_tree(writer, child_disk, child_entry, out_error_message)) {
                free(child_disk);
                free(child_entry);
                goto done;
            }
        } else if (S_ISREG(st.st_mode)) {
            size_t name_len = strlen(name);
            if (name_len >= 3U && strcmp(name + name_len - 3U, ".ft") == 0) {
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
        }
        free(child_disk);
        free(child_entry);
    }

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

static bool write_bundle_to_path(const FengFbLibraryBundleSpec *spec,
                                 const char *host_target,
                                 const char *archive_path,
                                 char **out_error_message) {
    FengZipWriter writer = {0};
    char *manifest = NULL;
    size_t manifest_length = 0U;
    char *host_lib_dir = NULL;
    char *library_entry_path = NULL;
    size_t index;
    bool ok = false;

    if (!appendf(&manifest,
                 &manifest_length,
                 "[package]\nname: \"%s\"\nversion: \"%s\"\narch: \"%s\"\nabi: \"feng\"\n",
                 spec->package_name,
                 spec->package_version,
                 host_target)) {
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
    host_lib_dir = dup_printf("lib/%s", host_target);
    if (host_lib_dir == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
    }
    library_entry_path = dup_printf("%s/%s",
                                    host_lib_dir,
                                    path_basename(spec->library_path));
    if (library_entry_path == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
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
    /* Phase 3 lands .fb container emission before .fi generation. Keep the
     * stable mod/ location present now so the later .fi task only needs to
     * add files beneath an already stable bundle layout. */
    if (!feng_zip_writer_add_directory(&writer, "mod", out_error_message)) {
        goto done;
    }
    if (spec->public_mod_root != NULL && dir_exists(spec->public_mod_root)) {
        if (!add_mod_tree(&writer, spec->public_mod_root, "mod", out_error_message)) {
            goto done;
        }
    }
    if (!feng_zip_writer_add_directory(&writer, "lib", out_error_message)) {
        goto done;
    }
    if (!feng_zip_writer_add_directory(&writer, host_lib_dir, out_error_message)) {
        goto done;
    }
    if (!feng_zip_writer_add_file(&writer,
                                  library_entry_path,
                                  spec->library_path,
                                  FENG_ZIP_COMPRESSION_STORE,
                                  out_error_message)) {
        goto done;
    }
    if (!feng_zip_writer_finalize(&writer, out_error_message)) {
        goto done;
    }

    ok = true;

done:
    feng_zip_writer_dispose(&writer);
    free(library_entry_path);
    free(host_lib_dir);
    free(manifest);
    return ok;
}

bool feng_fb_detect_host_target(char **out_host_target, char **out_error_message) {
    const char *os_name;
    const char *arch_name;

    if (out_host_target == NULL) {
        return set_errorf(out_error_message,
                          "host target output must not be null");
    }

#if defined(__APPLE__)
    os_name = "macos";
#elif defined(_WIN32)
    os_name = "windows";
#elif defined(__linux__)
    os_name = "linux";
#else
    return set_errorf(out_error_message, "unsupported host OS for .fb packaging");
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    arch_name = "arm64";
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    arch_name = "x64";
#else
    return set_errorf(out_error_message,
                      "unsupported host architecture for .fb packaging");
#endif

    *out_host_target = dup_printf("%s-%s", os_name, arch_name);
    if (*out_host_target == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    return true;
}

bool feng_fb_write_library_bundle(const FengFbLibraryBundleSpec *spec,
                                  char **out_error_message) {
    char *host_target = NULL;
    char *temp_path = NULL;
    int temp_fd = -1;
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
    if (!file_exists(spec->library_path)) {
        return set_errorf(out_error_message,
                          "bundle library artifact not found: %s",
                          spec->library_path != NULL ? spec->library_path : "(null)");
    }
    if (!feng_fb_detect_host_target(&host_target, out_error_message)) {
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

    if (!write_bundle_to_path(spec, host_target, temp_path, out_error_message)) {
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
    free(host_target);
    return ok;
}
