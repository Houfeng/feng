#define _XOPEN_SOURCE 700

#include <errno.h>
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

/*
 * Saved process-environment entry used to keep tool-resolution tests isolated
 * from the developer or CI environment that launched the suite.
 */
typedef struct SavedEnvironment {
    const char *name;
    char *value;
    bool was_set;
} SavedEnvironment;

/* Save one environment variable for restoration after a test. */
static SavedEnvironment save_environment(const char *name) {
    const char *value = getenv(name);
    SavedEnvironment saved = {
        name,
        value != NULL ? strdup(value) : NULL,
        value != NULL
    };

    ASSERT(value == NULL || saved.value != NULL);
    return saved;
}

/* Restore one environment variable to its pre-test state. */
static void restore_environment(SavedEnvironment *saved) {
    ASSERT(saved != NULL);
    if (saved->was_set) {
        ASSERT(setenv(saved->name, saved->value, 1) == 0);
    } else {
        ASSERT(unsetenv(saved->name) == 0);
    }
    free(saved->value);
    saved->value = NULL;
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
    ASSERT(stat("build/toolchain/llvm/bin/llvm-ar", &status) == 0);
    ASSERT(S_ISREG(status.st_mode));
    ASSERT(stat("build/toolchain/llvm/bin/llvm-ranlib", &status) == 0);
    ASSERT(S_ISREG(status.st_mode));
    ASSERT(stat("build/toolchain/sysroot", &status) == 0);
    ASSERT(S_ISDIR(status.st_mode));
}

/* Verify the complete Feng-specific, bundled, conventional, and PATH order. */
static void test_host_tool_resolution(const char *program_path) {
    const FengCliHostToolStrategy bundled_strategy = {
        "test compiler",
        "FENG_TEST_TOOL",
        "toolchain/llvm/bin/clang",
        "TEST_TOOL",
        "sh"
    };
    const FengCliHostToolStrategy missing_bundled_strategy = {
        "test compiler",
        "FENG_TEST_TOOL",
        "toolchain/llvm/bin/does-not-exist",
        "TEST_TOOL",
        "sh"
    };
    const FengCliHostToolStrategy damaged_bundled_strategy = {
        "test compiler",
        "FENG_TEST_TOOL",
        "toolchain/tool-resolution/not-executable",
        "TEST_TOOL",
        "sh"
    };
    const FengCliHostToolStrategy missing_system_strategy = {
        "test compiler",
        "FENG_TEST_TOOL",
        "toolchain/llvm/bin/does-not-exist",
        "TEST_TOOL",
        "feng-test-system-tool-does-not-exist"
    };
    SavedEnvironment feng_tool = save_environment("FENG_TEST_TOOL");
    SavedEnvironment conventional_tool = save_environment("TEST_TOOL");
    char *expected_shell = feng_cli_find_executable_on_path("sh");
    char *expected_bundled = feng_cli_resolve_install_path(
        program_path,
        "toolchain/llvm/bin/clang",
        NULL);
    char *resolved;
    char *error = NULL;
    FILE *damaged_file;

    ASSERT(expected_shell != NULL);
    ASSERT(expected_bundled != NULL);

    ASSERT(setenv("FENG_TEST_TOOL", expected_shell, 1) == 0);
    ASSERT(setenv("TEST_TOOL", "feng-test-conventional-must-not-run", 1) == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &bundled_strategy, &error);
    ASSERT(error == NULL);
    ASSERT(resolved != NULL);
    ASSERT(strcmp(resolved, expected_shell) == 0);
    free(resolved);

    ASSERT(unsetenv("FENG_TEST_TOOL") == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &bundled_strategy, &error);
    ASSERT(error == NULL);
    ASSERT(resolved != NULL);
    ASSERT(strcmp(resolved, expected_bundled) == 0);
    free(resolved);

    ASSERT(setenv("TEST_TOOL", expected_shell, 1) == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &missing_bundled_strategy, &error);
    ASSERT(error == NULL);
    ASSERT(resolved != NULL);
    ASSERT(strcmp(resolved, expected_shell) == 0);
    free(resolved);

    ASSERT(unsetenv("TEST_TOOL") == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &missing_bundled_strategy, &error);
    ASSERT(error == NULL);
    ASSERT(resolved != NULL);
    ASSERT(strcmp(resolved, expected_shell) == 0);
    free(resolved);

    ASSERT(setenv("FENG_TEST_TOOL", "feng-test-explicit-tool-does-not-exist", 1) == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &bundled_strategy, &error);
    ASSERT(resolved == NULL);
    ASSERT(error != NULL);
    ASSERT(strstr(error, "FENG_TEST_TOOL specifies an unavailable executable") != NULL);
    free(error);
    error = NULL;

    ASSERT(unsetenv("FENG_TEST_TOOL") == 0);
    ASSERT(mkdir("build/toolchain/tool-resolution", 0700) == 0 || errno == EEXIST);
    damaged_file = fopen("build/toolchain/tool-resolution/not-executable", "wb");
    ASSERT(damaged_file != NULL);
    ASSERT(fputs("not executable\n", damaged_file) >= 0);
    ASSERT(fclose(damaged_file) == 0);
    ASSERT(chmod("build/toolchain/tool-resolution/not-executable", 0600) == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &damaged_bundled_strategy, &error);
    ASSERT(resolved == NULL);
    ASSERT(error != NULL);
    ASSERT(strstr(error, "bundled test compiler is present") != NULL);
    free(error);
    error = NULL;
    ASSERT(unlink("build/toolchain/tool-resolution/not-executable") == 0);
    ASSERT(rmdir("build/toolchain/tool-resolution") == 0);

    ASSERT(setenv("TEST_TOOL", "feng-test-conventional-tool-does-not-exist", 1) == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &missing_bundled_strategy, &error);
    ASSERT(resolved == NULL);
    ASSERT(error != NULL);
    ASSERT(strstr(error, "TEST_TOOL specifies an unavailable executable") != NULL);
    free(error);
    error = NULL;

    ASSERT(unsetenv("TEST_TOOL") == 0);
    resolved = feng_cli_resolve_host_tool(program_path, &missing_system_strategy, &error);
    ASSERT(resolved == NULL);
    ASSERT(error != NULL);
    ASSERT(strstr(error, "was not found on PATH") != NULL);
    free(error);

    free(expected_bundled);
    free(expected_shell);
    restore_environment(&conventional_tool);
    restore_environment(&feng_tool);
}

/* Run the independent CLI path and development-layout regression suite. */
int main(int argc, char **argv) {
    ASSERT(argc > 0);
    test_cli_installation_paths(argv[0]);
    test_makefile_toolchain_layout();
    test_host_tool_resolution(argv[0]);
    puts("cli path tests passed");
    return 0;
}
