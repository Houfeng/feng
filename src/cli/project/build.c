#include "cli/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cli/deps/manager.h"
#include "cli/project/common.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s build [<path>] [--release]\n", program);
}

static bool parse_args(const char *program,
                       int argc,
                       char **argv,
                       const char **out_path,
                       bool *out_release) {
    int index;

    *out_path = NULL;
    *out_release = false;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program);
            return false;
        }
        if (strcmp(arg, "--release") == 0) {
            *out_release = true;
            continue;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program);
            return false;
        }
        if (*out_path != NULL) {
            fprintf(stderr, "build accepts at most one <path> argument\n");
            print_usage(program);
            return false;
        }
        *out_path = arg;
    }

    return true;
}

int feng_cli_project_build_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    bool release = false;
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    int rc;

    if (!parse_args(program, argc, argv, &path_arg, &release)) {
        return 1;
    }
    if (!feng_cli_project_open(path_arg, &context, &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_project_error_dispose(&error);
        return 1;
    }
    if (!feng_cli_deps_resolve_for_manifest(program,
                                            context.manifest_path,
                                            false,
                                            &resolved,
                                            &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_deps_resolved_dispose(&resolved);
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return 1;
    }
    rc = feng_cli_project_invoke_direct_compile_with_packages(program,
                                                              &context,
                                                              release,
                                                              resolved.package_count,
                                                              (const char *const *)resolved.package_paths);
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_context_dispose(&context);
    return rc;
}
