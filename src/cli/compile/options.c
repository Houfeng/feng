#include "cli/compile/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/common.h"
#include "platform/platform.h"

FengCliParseResult feng_cli_legacy_compile_parse(const char *program,
                                                 int argc,
                                                 char **argv,
                                                 FengCliLegacyCompileOptions *out) {
    int index;

    out->target = FENG_COMPILE_TARGET_BIN;
    out->emit_c_path = NULL;
    out->input_path = NULL;

    for (index = 0; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            feng_cli_print_usage(program, stdout);
            return FENG_CLI_PARSE_HELP;
        }
    }

    int file_argc = argc;
    char **file_argv = argv;

    while (file_argc > 0 && strncmp(file_argv[0], "--", 2) == 0) {
        if (strncmp(file_argv[0], "--target", 8) == 0) {
            if (!feng_cli_parse_target_option(file_argv[0], &out->target)) {
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
        } else if (strncmp(file_argv[0], "--emit-c=", 9) == 0) {
            out->emit_c_path = file_argv[0] + 9;
        } else {
            fprintf(stderr, "unknown option: %s\n", file_argv[0]);
            feng_cli_print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        file_argc -= 1;
        file_argv += 1;
    }

    if (file_argc != 1) {
        feng_cli_print_usage(program, stderr);
        return FENG_CLI_PARSE_ERROR;
    }

    out->input_path = file_argv[0];
    return FENG_CLI_PARSE_OK;
}

/* --- P4 direct mode option parser ----------------------------------------
 *
 * Accepted forms:
 *   feng <file> [<file>...] --target=<bin|lib>
 *        [--platform=<platform>] [--sysroot=<path>] [--out=<dir>]
 *        [--name=<artifact>] [--release] [--keep-ir]
 *        [--pkg=<.fb路径>|--pkg <.fb路径>]...
 *        [--lib=<库路径或系统库名>|--lib <库路径或系统库名>]...
 *
 * Flags may appear before, between, or after file arguments. `--target`
 * defaults to bin.
 */
FengCliParseResult feng_cli_direct_options_parse(const char *program,
                                                 int argc,
                                                 char **argv,
                                                 FengCliDirectOptions *out) {
    const char **inputs;
    const char **package_paths;
    const char **link_libs;
    int input_count = 0;
    int package_path_count = 0;
    int link_lib_count = 0;
    int index;

    out->target = FENG_COMPILE_TARGET_BIN;
    out->out_dir = "./build";
    out->release = false;
    out->keep_intermediate = false;
    out->artifact_name = NULL;
    out->platform = NULL;
    out->owned_platform = NULL;
    out->sysroot = NULL;
    out->input_count = 0;
    out->inputs = NULL;
    out->package_path_count = 0;
    out->package_paths = NULL;
    out->link_lib_count = 0;
    out->link_libs = NULL;

    if (argc <= 0) {
        feng_cli_print_usage(program, stderr);
        return FENG_CLI_PARSE_ERROR;
    }

    inputs = calloc((size_t)argc, sizeof(*inputs));
    package_paths = calloc((size_t)argc, sizeof(*package_paths));
    link_libs = calloc((size_t)argc, sizeof(*link_libs));
    if (inputs == NULL || package_paths == NULL || link_libs == NULL) {
        fprintf(stderr, "out of memory parsing direct compile options\n");
        free(link_libs);
        free(package_paths);
        free(inputs);
        return FENG_CLI_PARSE_ERROR;
    }

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];
        if (strncmp(arg, "--", 2) != 0) {
            inputs[input_count++] = arg;
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            free(link_libs);
            free(package_paths);
            free(inputs);
            feng_cli_print_usage(program, stdout);
            return FENG_CLI_PARSE_HELP;
        }
        if (strncmp(arg, "--target", 8) == 0) {
            if (!feng_cli_parse_target_option(arg, &out->target)) {
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strncmp(arg, "--out=", 6) == 0) {
            out->out_dir = arg + 6;
            if (out->out_dir[0] == '\0') {
                fprintf(stderr, "--out requires a non-empty directory path\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strncmp(arg, "--platform=", 11) == 0) {
            const char *platform = arg + 11;

            if (out->platform != NULL) {
                fprintf(stderr, "--platform may only be specified once in direct compile mode\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            if (!feng_platform_is_valid(platform)) {
                fprintf(stderr, "invalid target platform: %s\n",
                        platform[0] != '\0' ? platform : "(empty)");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            out->platform = platform;
            continue;
        }
        if (strncmp(arg, "--sysroot=", 10) == 0) {
            if (out->sysroot != NULL) {
                fprintf(stderr, "--sysroot may only be specified once\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            out->sysroot = arg + 10;
            if (out->sysroot[0] == '\0') {
                fprintf(stderr, "--sysroot requires a non-empty directory path\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strcmp(arg, "--release") == 0) {
            out->release = true;
            continue;
        }
        if (strcmp(arg, "--keep-ir") == 0) {
            out->keep_intermediate = true;
            continue;
        }
        if (strncmp(arg, "--name=", 7) == 0) {
            out->artifact_name = arg + 7;
            if (out->artifact_name[0] == '\0') {
                fprintf(stderr, "--name requires a non-empty value\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strncmp(arg, "--pkg=", 6) == 0) {
            const char *package_path = arg + 6;
            if (package_path[0] == '\0') {
                fprintf(stderr, "--pkg requires a non-empty .fb path\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            package_paths[package_path_count++] = package_path;
            continue;
        }
        if (strcmp(arg, "--pkg") == 0) {
            const char *package_path;

            if (index + 1 >= argc || argv[index + 1][0] == '\0') {
                fprintf(stderr, "--pkg requires a non-empty .fb path\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            package_path = argv[++index];
            package_paths[package_path_count++] = package_path;
            continue;
        }
        if (strncmp(arg, "--lib=", 6) == 0) {
            const char *link_lib = arg + 6;

            if (link_lib[0] == '\0') {
                fprintf(stderr, "--lib requires a non-empty library path or name\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            link_libs[link_lib_count++] = link_lib;
            continue;
        }
        if (strcmp(arg, "--lib") == 0) {
            const char *link_lib;

            if (index + 1 >= argc || argv[index + 1][0] == '\0') {
                fprintf(stderr, "--lib requires a non-empty library path or name\n");
                free(link_libs);
                free(package_paths);
                free(inputs);
                feng_cli_print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            link_lib = argv[++index];
            link_libs[link_lib_count++] = link_lib;
            continue;
        }
        fprintf(stderr, "unknown option: %s\n", arg);
        free(link_libs);
        free(package_paths);
        free(inputs);
        feng_cli_print_usage(program, stderr);
        return FENG_CLI_PARSE_ERROR;
    }

    if (input_count == 0) {
        fprintf(stderr, "no input files\n");
        free(link_libs);
        free(package_paths);
        free(inputs);
        feng_cli_print_usage(program, stderr);
        return FENG_CLI_PARSE_ERROR;
    }
    if (out->platform == NULL) {
        char *host_error = NULL;

        if (!feng_platform_detect_host_platform(&out->owned_platform,
                                                &host_error)) {
            fprintf(stderr,
                    "failed to detect host platform: %s\n",
                    host_error != NULL ? host_error : "(unknown)");
            free(host_error);
            free(link_libs);
            free(package_paths);
            free(inputs);
            feng_cli_print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        out->platform = out->owned_platform;
    }

    out->input_count = input_count;
    out->inputs = inputs;
    out->package_path_count = package_path_count;
    out->package_paths = package_paths;
    out->link_lib_count = link_lib_count;
    out->link_libs = link_libs;
    return FENG_CLI_PARSE_OK;
}

void feng_cli_direct_options_dispose(FengCliDirectOptions *opts) {
    if (opts == NULL) return;
    free(opts->owned_platform);
    opts->owned_platform = NULL;
    opts->platform = NULL;
    free((void *)opts->inputs);
    opts->inputs = NULL;
    opts->input_count = 0;
    free((void *)opts->package_paths);
    opts->package_paths = NULL;
    opts->package_path_count = 0;
    free((void *)opts->link_libs);
    opts->link_libs = NULL;
    opts->link_lib_count = 0;
}
