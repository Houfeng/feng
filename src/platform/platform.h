#ifndef FENG_PLATFORM_PLATFORM_H
#define FENG_PLATFORM_PLATFORM_H

#include <stdbool.h>

/*
 * Platform identifier helpers (P5 distribution support).
 *
 * Centralises the os-arch normalisation (compile-time macros → Feng platform
 * identifiers) and static-library name/path conventions used by the host C
 * compiler driver, the .fb archive writer, and the dependency manager. All
 * functions return malloc'd strings that the caller must free.
 */

const char *feng_platform_static_library_suffix(void);
char *feng_platform_static_library_file_name(const char *library_name);
bool feng_platform_is_static_library_path(const char *path);
bool feng_platform_static_library_matches_name(const char *path,
                                                const char *library_name);
bool feng_platform_detect_host_target(char **out_host_target,
                                       char **out_error_message);

/*
 * Detect the complete native platform identifier, including the Linux ABI.
 * The returned platform string and optional error message are owned by the caller.
 */
bool feng_platform_detect_host_platform(char **out_host_platform,
                                        char **out_error_message);

/* Return the Clang target triple for one supported complete platform identifier. */
const char *feng_platform_clang_target(const char *platform);

#endif /* FENG_PLATFORM_PLATFORM_H */
