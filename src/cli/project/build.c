#include "cli/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/project/common.h"
#include "platform/platform.h"

/* Parsed project build options. Platform pointers borrow argv storage. */
typedef struct BuildOptions {
    const char *path;
    const char **platforms;
    size_t platform_count;
    const char *sysroot;
    bool release;
    bool keep_ir;
} BuildOptions;

/* Print command-specific build usage. */
static void print_usage(const char *program, FILE *stream) {
    if (stream == stderr) fprintf(stream, "\n");
    fprintf(stream, "Usage:\n");
    fprintf(stream,
            "  %s build [<path>] [--release] [--keep-ir] "
            "[--platform=<platform>]... [--sysroot=<path>]\n",
            program);
}

/* Release arrays allocated by the build option parser. */
static void build_options_dispose(BuildOptions *options) {
    if (options == NULL) {
        return;
    }
    free((void *)options->platforms);
    options->platforms = NULL;
    options->platform_count = 0U;
}

/* Parse one `feng build` invocation without opening the project. */
static FengCliParseResult parse_args(const char *program,
                                     int argc,
                                     char **argv,
                                     BuildOptions *out_options) {
    int index;

    memset(out_options, 0, sizeof(*out_options));
    out_options->platforms = argc > 0
        ? (const char **)calloc((size_t)argc, sizeof(*out_options->platforms))
        : NULL;
    if (argc > 0 && out_options->platforms == NULL) {
        fprintf(stderr, "out of memory parsing build options\n");
        return FENG_CLI_PARSE_ERROR;
    }

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program, stdout);
            build_options_dispose(out_options);
            return FENG_CLI_PARSE_HELP;
        }
        if (strcmp(arg, "--release") == 0) {
            out_options->release = true;
            continue;
        }
        if (strcmp(arg, "--keep-ir") == 0) {
            out_options->keep_ir = true;
            continue;
        }
        if (strncmp(arg, "--platform=", 11U) == 0) {
            const char *platform = arg + 11U;

            if (!feng_platform_is_valid(platform)) {
                fprintf(stderr,
                        "invalid target platform: %s\n",
                        platform[0] != '\0' ? platform : "(empty)");
                print_usage(program, stderr);
                build_options_dispose(out_options);
                return FENG_CLI_PARSE_ERROR;
            }
            out_options->platforms[out_options->platform_count++] = platform;
            continue;
        }
        if (strncmp(arg, "--sysroot=", 10U) == 0) {
            if (out_options->sysroot != NULL) {
                fprintf(stderr, "--sysroot may only be specified once\n");
                print_usage(program, stderr);
                build_options_dispose(out_options);
                return FENG_CLI_PARSE_ERROR;
            }
            out_options->sysroot = arg + 10U;
            if (out_options->sysroot[0] == '\0') {
                fprintf(stderr, "--sysroot requires a non-empty directory path\n");
                print_usage(program, stderr);
                build_options_dispose(out_options);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strncmp(arg, "--", 2U) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program, stderr);
            build_options_dispose(out_options);
            return FENG_CLI_PARSE_ERROR;
        }
        if (out_options->path != NULL) {
            fprintf(stderr, "build accepts at most one <path> argument\n");
            print_usage(program, stderr);
            build_options_dispose(out_options);
            return FENG_CLI_PARSE_ERROR;
        }
        out_options->path = arg;
    }

    return FENG_CLI_PARSE_OK;
}

int feng_cli_project_build_main(const char *program, int argc, char **argv) {
    BuildOptions options = {0};
    FengCliParseResult parse_result;
    FengCliProjectContext context = {0};
    FengCliProjectPlatformSelection selection = {0};
    FengCliProjectError error = {0};
    size_t index;
    int rc = 1;

    parse_result = parse_args(program, argc, argv, &options);
    if (parse_result != FENG_CLI_PARSE_OK) {
        return parse_result == FENG_CLI_PARSE_HELP ? 0 : 1;
    }
    if (!feng_cli_project_open(options.path, &context, &error) ||
        !feng_cli_project_select_platforms(
            &context,
            options.platforms,
            options.platform_count,
            options.sysroot,
            false,
            &selection,
            &error)) {
        feng_cli_project_print_error(stderr, &error);
        goto done;
    }

    rc = 0;
    for (index = 0U; index < selection.platform_count; ++index) {
        rc = feng_cli_project_build_platform(program,
                                             &context,
                                             selection.platforms[index],
                                             options.sysroot,
                                             options.release,
                                             options.keep_ir,
                                             &error);
        if (rc != 0) {
            feng_cli_project_print_error(stderr, &error);
            break;
        }
    }

done:
    feng_cli_project_error_dispose(&error);
    feng_cli_project_platform_selection_dispose(&selection);
    feng_cli_project_context_dispose(&context);
    build_options_dispose(&options);
    return rc;
}
