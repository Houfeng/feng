#include <stdio.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/tool/tool.h"

#ifndef FENG_CLI_VERSION
#define FENG_CLI_VERSION "dev"
#endif

static void feng_cli_print_version(const char *program, FILE *stream) {
    fprintf(stream, "%s %s\n", program, FENG_CLI_VERSION);
}

void feng_cli_print_usage(const char *program) {
    int compile_indent = (int)(2U + strlen(program) + strlen(" <files...> "));

    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <files...> [options]\n", program);
    fprintf(stderr, "  %s <command>  [options]\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Project:\n");
    fprintf(stderr, "  %s init  [<name>] [--target=<bin|lib>]\n", program);
    fprintf(stderr, "  %s build [<path>] [--release]\n", program);
    fprintf(stderr, "  %s check [<path>] [--format=<text|json>]\n", program);
    fprintf(stderr, "  %s run   [<path>] [--release] [-- <program-args>...]\n", program);
    fprintf(stderr, "  %s clean [<path>]\n", program);
    fprintf(stderr, "  %s pack  [<path>]\n", program);
    fprintf(stderr, "  %s deps  <add|remove|install> ...\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Compile:\n");
    fprintf(stderr, "  %s <files...> [--target=<bin|lib>] \n", program);
    fprintf(stderr, "%*s[--out=<dir>] \n", compile_indent, "");
    fprintf(stderr, "%*s[--name=<artifact>] \n", compile_indent, "");
    fprintf(stderr, "%*s[--release] \n", compile_indent, "");
    fprintf(stderr, "%*s[--keep-ir]\n", compile_indent, "");
    fprintf(stderr, "\n");
    fprintf(stderr, "Global options:\n");
    fprintf(stderr, "  -h, --help      Display this message.\n");
    fprintf(stderr, "  -v, --version   Display version information.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Editor:  \n");
    fprintf(stderr, "  %s lsp [--stdio]\n", program);
    fprintf(stderr, "\n");
    // fprintf(stderr, "  %s tool compile [--target=bin|lib] [--emit-c=<path>] <file>\n", program);
    // fprintf(stderr, "  %s tool lex <file>\n", program);
    // fprintf(stderr, "  %s tool parse <file>\n", program);
    // fprintf(stderr, "  %s tool semantic [--target=bin|lib] <file> [more files...]\n", program);
    // fprintf(stderr, "  %s tool check [--target=bin|lib] <file> [more files...]\n", program);
    // fprintf(stderr, "\n");
    // fprintf(stderr, "  Direct mode: compile one or more .ff files into <out>/bin via <out>/ir/c.\n");
    // fprintf(stderr, "  --target defaults to 'bin'; '--target=lib' is reserved for `tool` analysis.\n");
    // fprintf(stderr, "  --release selects the release build mode for project/direct builds.\n");
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

    if (strcmp(cmd, "init") == 0) {
        return feng_cli_project_init_main(program, rest_argc, rest_argv);
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
    if (strcmp(cmd, "deps") == 0) {
        return feng_cli_deps_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "lsp") == 0) {
        return feng_cli_lsp_main(program, rest_argc, rest_argv);
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
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        feng_cli_print_version(program, stdout);
        return 0;
    }

    /* Default route: top-level direct compile mode (P4). Everything from
     * argv[1] onwards is treated as direct-mode arguments (file paths and
     * flags), matching `feng <files...> --target=bin --out=<dir>`. */
    return feng_cli_direct_main(program, argc - 1, argv + 1);
}
