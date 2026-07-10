#include "platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool name_has_suffix(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);

    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

static bool host_static_library_uses_lib_prefix(void) {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

const char *feng_platform_static_library_suffix(void) {
#if defined(_WIN32)
    return ".lib";
#else
    return ".a";
#endif
}

char *feng_platform_static_library_file_name(const char *library_name) {
    if (library_name == NULL || library_name[0] == '\0') {
        return NULL;
    }
    if (host_static_library_uses_lib_prefix() && strncmp(library_name, "lib", 3U) != 0) {
        return dup_printf("lib%s%s",
                          library_name,
                          feng_platform_static_library_suffix());
    }
    return dup_printf("%s%s",
                      library_name,
                      feng_platform_static_library_suffix());
}

bool feng_platform_is_static_library_path(const char *path) {
    if (path == NULL) {
        return false;
    }
    if (name_has_suffix(path, feng_platform_static_library_suffix())) {
        return true;
    }
#if defined(_WIN32)
    return name_has_suffix(path, ".a");
#else
    return false;
#endif
}

bool feng_platform_static_library_matches_name(const char *path,
                                                const char *library_name) {
    const char *basename;
    char *expected = NULL;
    bool matches = false;

    if (path == NULL || library_name == NULL || library_name[0] == '\0') {
        return false;
    }
    basename = path_basename(path);
    expected = feng_platform_static_library_file_name(library_name);
    if (expected == NULL) {
        return false;
    }
    matches = strcmp(basename, expected) == 0;
    free(expected);
#if defined(_WIN32)
    if (!matches) {
        char *legacy = dup_printf("lib%s.a", library_name);

        if (legacy == NULL) {
            return false;
        }
        matches = strcmp(basename, legacy) == 0;
        free(legacy);
    }
#endif
    return matches;
}

bool feng_platform_detect_host_target(char **out_host_target,
                                       char **out_error_message) {
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
    return set_errorf(out_error_message, "unsupported host OS");
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    arch_name = "arm64";
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    arch_name = "x64";
#else
    return set_errorf(out_error_message, "unsupported host architecture");
#endif

    *out_host_target = dup_printf("%s-%s", os_name, arch_name);
    if (*out_host_target == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    return true;
}
