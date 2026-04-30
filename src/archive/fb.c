#include "archive/fb.h"

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

static bool write_bundle_to_path(const FengFbLibraryBundleSpec *spec,
                                 const char *host_target,
                                 const char *archive_path,
                                 char **out_error_message) {
    FengZipWriter writer = {0};
    char *manifest = NULL;
    char *host_lib_dir = NULL;
    char *library_entry_path = NULL;
    bool ok = false;

    manifest = dup_printf("[package]\nname: \"%s\"\nversion: \"%s\"\narch: \"%s\"\nabi: \"feng\"\n",
                          spec->package_name,
                          spec->package_version,
                          host_target);
    if (manifest == NULL) {
        set_errorf(out_error_message, "out of memory");
        goto done;
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
