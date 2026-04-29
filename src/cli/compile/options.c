#include "cli/compile/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/common.h"

bool feng_cli_legacy_compile_parse(const char *program,
                                   int argc,
                                   char **argv,
                                   FengCliLegacyCompileOptions *out) {
    out->target = FENG_COMPILE_TARGET_BIN;
    out->emit_c_path = NULL;
    out->input_path = NULL;

    int file_argc = argc;
    char **file_argv = argv;

    while (file_argc > 0 && strncmp(file_argv[0], "--", 2) == 0) {
        if (strncmp(file_argv[0], "--target", 8) == 0) {
            if (!feng_cli_parse_target_option(file_argv[0], &out->target)) {
                feng_cli_print_usage(program);
                return false;
            }
        } else if (strncmp(file_argv[0], "--emit-c=", 9) == 0) {
            out->emit_c_path = file_argv[0] + 9;
        } else {
            fprintf(stderr, "unknown option: %s\n", file_argv[0]);
            feng_cli_print_usage(program);
            return false;
        }
        file_argc -= 1;
        file_argv += 1;
    }

    if (file_argc != 1) {
        feng_cli_print_usage(program);
        return false;
    }

    out->input_path = file_argv[0];
    return true;
}

/* --- P4 direct mode option parser ----------------------------------------
 *
 * Accepted forms:
 *   feng <file> [<file>...] --target=bin --out=<dir> [--name=<artifact>] [--release] [--keep-ir]
 *
 * Flags may appear before, between, or after file arguments. `--target`
 * defaults to bin; `--target=lib` is accepted at parse time so a richer
 * downstream diagnostic can explain why it is not yet supported.
 */
bool feng_cli_direct_options_parse(const char *program,
                                   int argc,
                                   char **argv,
                                   FengCliDirectOptions *out) {
    out->target = FENG_COMPILE_TARGET_BIN;
    out->out_dir = NULL;
    out->release = false;
    out->keep_intermediate = false;
    out->artifact_name = NULL;
    out->input_count = 0;
    out->inputs = NULL;

    if (argc <= 0) {
        feng_cli_print_usage(program);
        return false;
    }

    const char **inputs = calloc((size_t)argc, sizeof(*inputs));
    if (inputs == NULL) {
        fprintf(stderr, "out of memory parsing direct compile options\n");
        return false;
    }
    int input_count = 0;

    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) {
            inputs[input_count++] = arg;
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            free(inputs);
            feng_cli_print_usage(program);
            return false;
        }
        if (strncmp(arg, "--target", 8) == 0) {
            if (!feng_cli_parse_target_option(arg, &out->target)) {
                free(inputs);
                feng_cli_print_usage(program);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "--out=", 6) == 0) {
            out->out_dir = arg + 6;
            if (out->out_dir[0] == '\0') {
                fprintf(stderr, "--out requires a non-empty directory path\n");
                free(inputs);
                return false;
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
                free(inputs);
                return false;
            }
            continue;
        }
        fprintf(stderr, "unknown option: %s\n", arg);
        free(inputs);
        feng_cli_print_usage(program);
        return false;
    }

    if (input_count == 0) {
        fprintf(stderr, "no input files\n");
        free(inputs);
        feng_cli_print_usage(program);
        return false;
    }
    if (out->out_dir == NULL) {
        fprintf(stderr, "--out=<dir> is required for direct compile mode\n");
        free(inputs);
        feng_cli_print_usage(program);
        return false;
    }

    out->input_count = input_count;
    out->inputs = inputs;
    return true;
}

void feng_cli_direct_options_dispose(FengCliDirectOptions *opts) {
    if (opts == NULL) return;
    free((void *)opts->inputs);
    opts->inputs = NULL;
    opts->input_count = 0;
}
