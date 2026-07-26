#ifndef FENG_PLATFORM_PLATFORM_H
#define FENG_PLATFORM_PLATFORM_H

#include <stdbool.h>

/*
 * Platform identifier helpers (P5 distribution support).
 *
 * Centralises the os-arch normalisation (compile-time macros → Feng platform
 * identifiers) and library name/path conventions used by the compiler driver,
 * the .fb archive writer, and the dependency manager. Functions documented as
 * returning allocated strings transfer ownership to the caller.
 */

const char *feng_platform_static_library_suffix(void);
char *feng_platform_static_library_file_name(const char *library_name);
bool feng_platform_is_static_library_path(const char *path);
bool feng_platform_static_library_matches_name(const char *path,
                                                const char *library_name);

/* Return the dynamic-library suffix used by one complete target platform. */
const char *feng_platform_dynamic_library_suffix(const char *platform);

/* Return the allocated dynamic-library file name for a target platform. */
char *feng_platform_dynamic_library_file_name(const char *platform,
                                              const char *library_name);

bool feng_platform_detect_host_target(char **out_host_target,
                                       char **out_error_message);

/*
 * Detect the complete native platform identifier, including the Linux ABI.
 * The returned platform string and optional error message are owned by the caller.
 */
bool feng_platform_detect_host_platform(char **out_host_platform,
                                        char **out_error_message);

/* Return whether text is one complete platform identifier from the platform matrix. */
bool feng_platform_is_valid(const char *platform);

/* Return the Clang target triple for one supported complete platform identifier. */
const char *feng_platform_clang_target(const char *platform);

/* Return whether one complete platform identifier targets macOS. */
bool feng_platform_is_macos(const char *platform);

/* Return whether one complete platform identifier targets Linux. */
bool feng_platform_is_linux(const char *platform);

/* Return whether one complete platform identifier targets Linux musl. */
bool feng_platform_is_linux_musl(const char *platform);

#endif /* FENG_PLATFORM_PLATFORM_H */
