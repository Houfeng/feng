#include "cli/cli.h"

#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli/project/common.h"

static void print_usage(const char *program, FILE *stream) {
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s run [<path>] [--release] [--keep-ir] [-- <program-args>...]\n", program);
}

static FengCliParseResult parse_args(const char *program,
                                     int argc,
                                     char **argv,
                                     const char **out_path,
                                     bool *out_release,
                                     bool *out_keep_ir,
                                     int *out_program_argc,
                                     char ***out_program_argv) {
    int index;

    *out_path = NULL;
    *out_release = false;
    *out_keep_ir = false;
    *out_program_argc = 0;
    *out_program_argv = NULL;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--") == 0) {
            *out_program_argc = argc - index - 1;
            *out_program_argv = argv + index + 1;
            return FENG_CLI_PARSE_OK;
        }
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
            fprintf(stderr, "run accepts at most one <path> argument before `--`\n");
            print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        *out_path = arg;
    }

    return FENG_CLI_PARSE_OK;
}

static int execute_program(const char *binary_path, int argc, char **argv) {
    char **child_argv = (char **)calloc((size_t)argc + 2U, sizeof(*child_argv));
    pid_t child;
    int status = 0;
    int index;

    if (child_argv == NULL) {
        fprintf(stderr, "out of memory preparing program arguments\n");
        return 1;
    }
    child_argv[0] = (char *)binary_path;
    for (index = 0; index < argc; ++index) {
        child_argv[index + 1] = argv[index];
    }
    child_argv[argc + 1] = NULL;

    child = fork();
    if (child < 0) {
        fprintf(stderr, "failed to fork: %s\n", strerror(errno));
        free(child_argv);
        return 1;
    }
    if (child == 0) {
        execv(binary_path, child_argv);
        fprintf(stderr, "failed to exec %s: %s\n", binary_path, strerror(errno));
        _exit(127);
    }

    free(child_argv);
    if (waitpid(child, &status, 0) < 0) {
        fprintf(stderr, "failed to wait for %s: %s\n", binary_path, strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "program terminated by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int feng_cli_project_run_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    bool release = false;
    bool keep_ir = false;
    int program_argc = 0;
    char **program_argv = NULL;
    FengCliParseResult parse_result;
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    int rc;

    parse_result = parse_args(program, argc, argv, &path_arg, &release, &keep_ir, &program_argc, &program_argv);
    if (parse_result != FENG_CLI_PARSE_OK) {
        return parse_result == FENG_CLI_PARSE_HELP ? 0 : 1;
    }
    if (!feng_cli_project_open(path_arg, &context, &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_project_error_dispose(&error);
        return 1;
    }
    if (context.manifest.target != FENG_COMPILE_TARGET_BIN) {
        fprintf(stderr, "error: `feng run` requires a target=bin project\n");
        feng_cli_project_context_dispose(&context);
        return 1;
    }

    if (!feng_cli_project_resolve_build_dependencies(program,
                                                     &context,
                                                     release,
                                                     &resolved,
                                                     &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_deps_resolved_dispose(&resolved);
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return 1;
    }

    rc = feng_cli_project_compile_prepared(program, &context, &resolved, release, keep_ir);
    feng_cli_deps_resolved_dispose(&resolved);
    if (rc == 0) {
        rc = execute_program(context.binary_path, program_argc, program_argv);
    }

    feng_cli_project_context_dispose(&context);
    return rc;
}
