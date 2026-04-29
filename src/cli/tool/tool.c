#include "cli/tool/tool.h"

#include <stdio.h>
#include <string.h>

#include "cli/cli.h"

void feng_cli_tool_print_usage(const char *program, FILE *stream) {
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s tool lex <file>\n", program);
    fprintf(stream, "  %s tool parse <file>\n", program);
    fprintf(stream, "  %s tool semantic [--target=bin|lib] <file> [more files...]\n", program);
    fprintf(stream, "  %s tool check [--target=bin|lib] <file> [more files...]\n", program);
}

int feng_cli_tool_main(const char *program, int argc, char **argv) {
    if (argc < 1) {
        feng_cli_tool_print_usage(program, stderr);
        return 1;
    }

    const char *sub = argv[0];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (strcmp(sub, "lex") == 0) {
        return feng_cli_tool_lex_main(program, sub_argc, sub_argv);
    }
    if (strcmp(sub, "parse") == 0) {
        return feng_cli_tool_parse_main(program, sub_argc, sub_argv);
    }
    if (strcmp(sub, "semantic") == 0) {
        return feng_cli_tool_semantic_main(program, sub_argc, sub_argv);
    }
    if (strcmp(sub, "check") == 0) {
        return feng_cli_tool_check_main(program, sub_argc, sub_argv);
    }

    fprintf(stderr, "unknown tool subcommand: %s\n", sub);
    feng_cli_tool_print_usage(program, stderr);
    return 1;
}
