#include "cli/cli.h"

#include <stdio.h>

int feng_cli_project_pack_main(const char *program, int argc, char **argv) {
    (void)argc;
    (void)argv;

    fprintf(stderr,
            "error: `%s pack` is not yet available in this implementation step; `target=lib` build and `.fb` packaging land after the current build/check/run/clean batch.\n",
            program);
    return 1;
}
