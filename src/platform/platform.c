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

/* Return the dynamic-library suffix used by one complete target platform. */
const char *feng_platform_dynamic_library_suffix(const char *platform) {
    if (feng_platform_is_macos(platform)) {
        return ".dylib";
    }
    if (feng_platform_is_linux(platform)) {
        return ".so";
    }
    if (platform != NULL && strncmp(platform, "windows-", 8U) == 0) {
        return ".dll";
    }
    return NULL;
}

/* Return the allocated dynamic-library file name for a target platform. */
char *feng_platform_dynamic_library_file_name(const char *platform,
                                              const char *library_name) {
    const char *suffix;

    if (library_name == NULL || library_name[0] == '\0') {
        return NULL;
    }
    suffix = feng_platform_dynamic_library_suffix(platform);
    if (suffix == NULL) {
        return NULL;
    }
    if (strncmp(platform, "windows-", 8U) == 0) {
        return dup_printf("%s%s", library_name, suffix);
    }
    return dup_printf("lib%s%s", library_name, suffix);
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

/* Detect the complete native platform name used by runtime and sysroot paths. */
bool feng_platform_detect_host_platform(char **out_host_platform,
                                        char **out_error_message) {
    char *host_target = NULL;

    if (out_host_platform == NULL) {
        return set_errorf(out_error_message,
                          "host platform output must not be null");
    }
    if (!feng_platform_detect_host_target(&host_target, out_error_message)) {
        return false;
    }
#if defined(__linux__)
    *out_host_platform = dup_printf("%s-gnu", host_target);
    free(host_target);
    if (*out_host_platform == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
#else
    *out_host_platform = host_target;
#endif
    return true;
}

/* Check one identifier against the complete platform matrix. */
bool feng_platform_is_valid(const char *platform) {
    static const char *const platforms[] = {
        "macos-arm64",
        "macos-x64",
        "linux-x64-gnu",
        "linux-x64-musl",
        "linux-arm64-gnu",
        "linux-arm64-musl",
        "linux-x86-gnu",
        "linux-x86-musl",
        "linux-arm-gnu",
        "linux-arm-musl",
        "windows-x64",
        "windows-arm64",
        "windows-x86",
        "windows-arm"
    };
    size_t index;

    if (platform == NULL) {
        return false;
    }
    for (index = 0U; index < sizeof(platforms) / sizeof(platforms[0]); ++index) {
        if (strcmp(platform, platforms[index]) == 0) {
            return true;
        }
    }
    return false;
}

/* Map one supported Feng platform identifier to its Clang target triple. */
const char *feng_platform_clang_target(const char *platform) {
    if (platform == NULL) {
        return NULL;
    }
    if (strcmp(platform, "macos-arm64") == 0) {
        return "arm64-apple-macosx";
    }
    if (strcmp(platform, "linux-x64-gnu") == 0) {
        return "x86_64-unknown-linux-gnu";
    }
    if (strcmp(platform, "linux-x64-musl") == 0) {
        return "x86_64-unknown-linux-musl";
    }
    if (strcmp(platform, "linux-arm64-gnu") == 0) {
        return "aarch64-unknown-linux-gnu";
    }
    if (strcmp(platform, "linux-arm64-musl") == 0) {
        return "aarch64-unknown-linux-musl";
    }
    return NULL;
}

/* Check whether a complete platform identifier targets macOS. */
bool feng_platform_is_macos(const char *platform) {
    return platform != NULL && strncmp(platform, "macos-", 6U) == 0;
}

/* Check whether a complete platform identifier targets Linux. */
bool feng_platform_is_linux(const char *platform) {
    return platform != NULL && strncmp(platform, "linux-", 6U) == 0;
}

/* Check whether a complete platform identifier targets Linux musl. */
bool feng_platform_is_linux_musl(const char *platform) {
    size_t length;

    if (!feng_platform_is_linux(platform)) {
        return false;
    }
    length = strlen(platform);
    return length >= 5U && strcmp(platform + length - 5U, "-musl") == 0;
}
