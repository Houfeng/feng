#include "cli/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cli/deps/manager.h"
#include "cli/project/common.h"

static void print_usage(const char *program, FILE *stream) {
    if (stream == stderr) fprintf(stream, "\n");
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s build [<path>] [--release] [--keep-ir]\n", program);
}

static FengCliParseResult parse_args(const char *program,
                                     int argc,
                                     char **argv,
                                     const char **out_path,
                                     bool *out_release,
                                     bool *out_keep_ir) {
    int index;

    *out_path = NULL;
    *out_release = false;
    *out_keep_ir = false;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program, stdout);
            return FENG_CLI_PARSE_HELP;
        }
        if (strcmp(arg, "--release") == 0) {
            *out_release = true;
            continue;
        }
        if (strcmp(arg, "--keep-ir") == 0) {
            *out_keep_ir = true;
            continue;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        if (*out_path != NULL) {
            fprintf(stderr, "build accepts at most one <path> argument\n");
            print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        *out_path = arg;
    }

    return FENG_CLI_PARSE_OK;
}

int feng_cli_project_build_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    bool release = false;
    bool keep_ir = false;
    FengCliParseResult parse_result;
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    int rc;

    parse_result = parse_args(program, argc, argv, &path_arg, &release, &keep_ir);
    if (parse_result != FENG_CLI_PARSE_OK) {
        return parse_result == FENG_CLI_PARSE_HELP ? 0 : 1;
    }
    if (!feng_cli_project_prepare_build(program,
                                        path_arg,
                                        release,
                                        &context,
                                        &resolved,
                                        &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_project_error_dispose(&error);
        return 1;
    }
    rc = feng_cli_project_compile_prepared(program, &context, &resolved, release, keep_ir);
    if (rc == 0 && !feng_cli_project_stage_assets(&context, &error)) {
        feng_cli_project_print_error(stderr, &error);
        rc = 1;
    }
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_context_dispose(&context);
    feng_cli_project_error_dispose(&error);
    return rc;
}
