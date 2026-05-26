#include "cli/cli.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dap/proxy.h"

/* Print command usage for `feng dap`. */
static void print_usage(const char *program, FILE *stream) {
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s dap [--stdio]\n", program);
    fprintf(stream, "\n");
    fprintf(stream, "Start Feng Debug Adapter Protocol proxy on stdio.\n");
}

/* Parse `feng dap` arguments and start the transparent stdio proxy. */
int feng_cli_dap_main(const char *program, int argc, char **argv) {
    int index;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program, stdout);
            return 0;
        }
        if (strcmp(arg, "--stdio") == 0) {
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", arg);
        print_usage(program, stderr);
        return 1;
    }

#if !defined(__APPLE__)
    fprintf(stderr, "error: `feng dap` currently only supports macOS with lldb-dap\n");
    return 1;
#else
    return feng_dap_proxy_run("lldb-dap", STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
#endif
}
