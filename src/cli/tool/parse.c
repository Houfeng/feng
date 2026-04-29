#include "cli/tool/tool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/common.h"
#include "parser/parser.h"

int feng_cli_tool_parse_main(const char *program, int argc, char **argv) {
    if (argc != 1) {
        feng_cli_tool_print_usage(program, stderr);
        return 1;
    }

    const char *path = argv[0];
    size_t source_length = 0;
    char *source = feng_cli_read_entire_file(path, &source_length);
    FengProgram *program_ast = NULL;
    FengParseError error;
    int exit_code = 0;

    if (source == NULL) {
        fprintf(stderr, "failed to read %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (!feng_parse_source(source, source_length, path, &program_ast, &error)) {
        feng_cli_print_diagnostic(stderr,
                                  path,
                                  "parse error",
                                  error.message,
                                  &error.token,
                                  source,
                                  source_length);
        exit_code = 1;
    } else {
        feng_program_dump(stdout, program_ast);
    }

    feng_program_free(program_ast);
    free(source);
    return exit_code;
}
