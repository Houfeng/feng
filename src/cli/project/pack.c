#include "cli/cli.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive/fb.h"
#include "cli/project/common.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s pack [<path>] [--release]\n", program);
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
            fprintf(stderr, "pack accepts at most one <path> argument\n");
            print_usage(program);
            return false;
        }
        *out_path = arg;
    }

    return true;
}

static char *dup_printf(const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *out;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return NULL;
    }

    out = (char *)malloc((size_t)needed + 1U);
    if (out == NULL) {
        va_end(args_copy);
        return NULL;
    }
    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

int feng_cli_project_pack_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    bool release = false;
    FengCliProjectContext context = {0};
    FengCliProjectError project_error = {0};
    FengFbLibraryBundleSpec spec = {0};
    char *library_path = NULL;
    char *public_mod_root = NULL;
    char *error_message = NULL;
    int rc = 1;

    if (!parse_args(program, argc, argv, &path_arg, &release)) {
        return 1;
    }
    if (!feng_cli_project_open(path_arg, &context, &project_error)) {
        feng_cli_project_print_error(stderr, &project_error);
        feng_cli_project_error_dispose(&project_error);
        return 1;
    }
    if (context.manifest.target != FENG_COMPILE_TARGET_LIB) {
        fprintf(stderr,
                "error: `%s pack` requires `target:lib` in feng.fm\n",
                program);
        goto done;
    }
    rc = feng_cli_project_invoke_direct_compile(program, &context, release);
    if (rc != 0) {
        goto done;
    }

    library_path = dup_printf("%s/lib/lib%s.a",
                              context.out_root,
                              context.manifest.name);
    if (library_path == NULL) {
        fprintf(stderr, "error: out of memory while preparing package paths\n");
        rc = 1;
        goto done;
    }
    public_mod_root = dup_printf("%s/mod", context.out_root);
    if (public_mod_root == NULL) {
        fprintf(stderr, "error: out of memory while preparing package paths\n");
        rc = 1;
        goto done;
    }

    spec.package_path = context.package_path;
    spec.package_name = context.manifest.name;
    spec.package_version = context.manifest.version;
    spec.library_path = library_path;
    spec.public_mod_root = public_mod_root;

    if (!feng_fb_write_library_bundle(&spec, &error_message)) {
        fprintf(stderr,
                "error: %s\n",
                error_message != NULL ? error_message : "failed to write .fb package");
        rc = 1;
        goto done;
    }

    rc = 0;

done:
    free(error_message);
    free(public_mod_root);
    free(library_path);
    feng_cli_project_context_dispose(&context);
    feng_cli_project_error_dispose(&project_error);
    return rc;
}
