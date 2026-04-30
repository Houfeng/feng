#include "cli/cli.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cli/project/common.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s clean [<path>]\n", program);
}

int feng_cli_project_clean_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;
    int index;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program);
            return 1;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program);
            return 1;
        }
        if (path_arg != NULL) {
            fprintf(stderr, "clean accepts at most one <path> argument\n");
            print_usage(program);
            return 1;
        }
        path_arg = arg;
    }

    if (!feng_cli_project_open(path_arg, &context, &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_project_error_dispose(&error);
        return 1;
    }

    if (!feng_cli_project_remove_tree(context.out_root, &remove_error)) {
        fprintf(stderr, "%s\n", remove_error != NULL ? remove_error : "failed to clean project output");
        free(remove_error);
        feng_cli_project_context_dispose(&context);
        return 1;
    }

    free(remove_error);
    feng_cli_project_context_dispose(&context);
    return 0;
}
