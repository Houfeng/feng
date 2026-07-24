#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "cli/common.h"

#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                    __FILE__, __LINE__, #condition); \
            exit(1); \
        } \
    } while (0)

/* Join one absolute root and fixed relative suffix for expected-path checks. */
static char *join_expected_path(const char *root, const char *suffix) {
    size_t root_length = strlen(root);
    size_t suffix_length = strlen(suffix);
    char *path = (char *)malloc(root_length + 1U + suffix_length + 1U);

    ASSERT(path != NULL);
    memcpy(path, root, root_length);
    path[root_length] = '/';
    memcpy(path + root_length + 1U, suffix, suffix_length + 1U);
    return path;
}

/* Assert that one layout entry is a symlink with the exact relative target. */
static void assert_symlink_target(const char *path, const char *expected_target) {
    struct stat status;
    char target[512];
    ssize_t length;

    ASSERT(lstat(path, &status) == 0);
    ASSERT(S_ISLNK(status.st_mode));
    length = readlink(path, target, sizeof(target) - 1U);
    ASSERT(length >= 0);
    ASSERT((size_t)length < sizeof(target));
    target[length] = '\0';
    ASSERT(strcmp(target, expected_target) == 0);
}

/* Verify executable discovery and installation-relative path diagnostics. */
static void test_cli_installation_paths(const char *program_path) {
    char *error = NULL;
    char *executable_path = feng_cli_resolve_executable_path(program_path, &error);
    char *expected_executable = realpath("build/bin/test_cli_paths", NULL);
    char *build_root = realpath("build", NULL);
    char *expected_clang;
    char *resolved_clang;
    char *required_clang;
    char *required_sysroot;
    char *missing;
    char *escaped;
    char *shell;

    ASSERT(error == NULL);
    ASSERT(executable_path != NULL);
    ASSERT(expected_executable != NULL);
    ASSERT(strcmp(executable_path, expected_executable) == 0);
    ASSERT(build_root != NULL);

    expected_clang = join_expected_path(build_root, "toolchain/llvm/bin/clang");
    resolved_clang = feng_cli_resolve_install_path(program_path,
                                                   "toolchain/llvm/bin/clang",
                                                   &error);
    ASSERT(error == NULL);
    ASSERT(resolved_clang != NULL);
    ASSERT(strcmp(resolved_clang, expected_clang) == 0);

    required_clang = feng_cli_require_install_path(program_path,
                                                   "toolchain/llvm/bin/clang",
                                                   FENG_CLI_REQUIRED_EXECUTABLE,
                                                   &error);
    ASSERT(error == NULL);
    ASSERT(required_clang != NULL);
    ASSERT(strcmp(required_clang, expected_clang) == 0);

    required_sysroot = feng_cli_require_install_path(program_path,
                                                     "toolchain/sysroot",
                                                     FENG_CLI_REQUIRED_DIRECTORY,
                                                     &error);
    ASSERT(error == NULL);
    ASSERT(required_sysroot != NULL);

    missing = feng_cli_require_install_path(program_path,
                                            "toolchain/llvm/bin/does-not-exist",
                                            FENG_CLI_REQUIRED_EXECUTABLE,
                                            &error);
    ASSERT(missing == NULL);
    ASSERT(error != NULL);
    ASSERT(strstr(error, "required executable is missing") != NULL);
    ASSERT(strstr(error, "toolchain/llvm/bin/does-not-exist") != NULL);
    free(error);
    error = NULL;

    escaped = feng_cli_resolve_install_path(program_path, "../outside", &error);
    ASSERT(escaped == NULL);
    ASSERT(error != NULL);
    ASSERT(strstr(error, "must be relative and stay below the Feng root") != NULL);
    free(error);
    error = NULL;

    shell = feng_cli_find_executable_on_path("sh");
    ASSERT(shell != NULL);
    ASSERT(feng_cli_path_is_executable(shell));

    free(shell);
    free(required_sysroot);
    free(required_clang);
    free(resolved_clang);
    free(expected_clang);
    free(build_root);
    free(expected_executable);
    free(executable_path);
}

/* Verify the development layout visible from build/bin/feng. */
static void test_makefile_toolchain_layout(void) {
    struct stat status;
    struct utsname host;
    const char *host_os;
    const char *host_arch;
    const char *host_abi;
    char expected_llvm_target[128];

    ASSERT(uname(&host) == 0);
    host_os = strcmp(host.sysname, "Darwin") == 0 ? "macos" : "linux";
    host_arch = strcmp(host.machine, "arm64") == 0 ||
                strcmp(host.machine, "aarch64") == 0
        ? "arm64"
        : "x64";
    host_abi = strcmp(host_os, "linux") == 0 ? "-gnu" : "";
    ASSERT(snprintf(expected_llvm_target,
                    sizeof(expected_llvm_target),
                    "../../toolchain/llvm/%s-%s%s",
                    host_os,
                    host_arch,
                    host_abi) > 0);

    assert_symlink_target("build/toolchain/llvm", expected_llvm_target);
    assert_symlink_target("build/toolchain/sysroot",
                          "../../toolchain/sysroot");
    ASSERT(stat("build/toolchain/llvm/bin/clang", &status) == 0);
    ASSERT(S_ISREG(status.st_mode));
    ASSERT(stat("build/toolchain/sysroot", &status) == 0);
    ASSERT(S_ISDIR(status.st_mode));
}

/* Run the independent CLI path and development-layout regression suite. */
int main(int argc, char **argv) {
    ASSERT(argc > 0);
    test_cli_installation_paths(argv[0]);
    test_makefile_toolchain_layout();
    puts("cli path tests passed");
    return 0;
}
