#include <stdio.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/tool/tool.h"

void feng_cli_print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s build [<path>] [--release]\n", program);
    fprintf(stderr, "  %s check [<path>] [--format <text|json>]\n", program);
    fprintf(stderr, "  %s run [<path>] [--release] [-- <program-args>...]\n", program);
    fprintf(stderr, "  %s clean [<path>]\n", program);
    fprintf(stderr, "  %s pack [<path>]\n", program);
    fprintf(stderr, "  %s <files...> --target=bin --out=<dir> [--name=<artifact>] [--release] [--keep-ir]\n", program);
    fprintf(stderr, "  %s tool compile [--target=bin|lib] [--emit-c=<path>] <file>\n", program);
    fprintf(stderr, "  %s tool lex <file>\n", program);
    fprintf(stderr, "  %s tool parse <file>\n", program);
    fprintf(stderr, "  %s tool semantic [--target=bin|lib] <file> [more files...]\n", program);
    fprintf(stderr, "  %s tool check [--target=bin|lib] <file> [more files...]\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "  Direct mode: compile one or more .ff files into <out>/bin via <out>/ir/c.\n");
    fprintf(stderr, "  --target defaults to 'bin'; '--target=lib' is reserved for `tool` analysis.\n");
    fprintf(stderr, "  --release is parsed but not yet implemented (P4).\n");
}

int main(int argc, char **argv) {
    const char *program = "feng";
    if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0') {
        const char *slash = strrchr(argv[0], '/');
        program = slash != NULL ? slash + 1 : argv[0];
    }

    if (argc < 2) {
        feng_cli_print_usage(program);
        return 1;
    }

    const char *cmd = argv[1];
    int rest_argc = argc - 2;
    char **rest_argv = argv + 2;

    if (strcmp(cmd, "tool") == 0) {
        return feng_cli_tool_main(program, rest_argc, rest_argv);
    }

    if (strcmp(cmd, "build") == 0) {
        return feng_cli_project_build_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "check") == 0) {
        return feng_cli_project_check_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "run") == 0) {
        return feng_cli_project_run_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "clean") == 0) {
        return feng_cli_project_clean_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "pack") == 0) {
        return feng_cli_project_pack_main(program, rest_argc, rest_argv);
    }

    if (strcmp(cmd, "compile") == 0
        || strcmp(cmd, "lex") == 0
        || strcmp(cmd, "parse") == 0
        || strcmp(cmd, "semantic") == 0) {
        fprintf(stderr,
                "`%s %s ...` is no longer a top-level command; use `%s tool %s ...` instead.\n",
                program, cmd, program, cmd);
        feng_cli_print_usage(program);
        return 1;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        feng_cli_print_usage(program);
        return 0;
    }

    /* Default route: top-level direct compile mode (P4). Everything from
     * argv[1] onwards is treated as direct-mode arguments (file paths and
     * flags), matching `feng <files...> --target=bin --out=<dir>`. */
    return feng_cli_direct_main(program, argc - 1, argv + 1);
}
