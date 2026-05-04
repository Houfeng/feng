#include "cli/cli.h"

#include <stdio.h>
#include <string.h>

#include "cli/lsp/server.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s lsp [--stdio]\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Start Feng Language Server on stdio.\n");
}

int feng_cli_lsp_main(const char *program, int argc, char **argv) {
    int index;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program);
            return 0;
        }
        if (strcmp(arg, "--stdio") == 0) {
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", arg);
        print_usage(program);
        return 1;
    }

    return feng_lsp_server_run(stdin, stdout, stderr);
}
