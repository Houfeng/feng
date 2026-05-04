#include "cli/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "cli/deps/manager.h"
#include "cli/project/common.h"

static char *dup_printf(const char *fmt, const char *value) {
    int needed = snprintf(NULL, 0, fmt, value);
    char *out;

    if (needed < 0) {
        return NULL;
    }
    out = (char *)malloc((size_t)needed + 1U);
    if (out == NULL) {
        return NULL;
    }
    snprintf(out, (size_t)needed + 1U, fmt, value);
    return out;
}

int feng_cli_project_invoke_direct_compile_with_packages(const char *program,
                                                         const FengCliProjectContext *context,
                                                         bool release,
                                                         size_t package_count,
                                                         const char *const *package_paths) {
    size_t argc = context->source_count + 3U + (release ? 1U : 0U) + package_count * 2U;
    char **argv = (char **)calloc(argc, sizeof(*argv));
    char *target_opt = NULL;
    char *out_opt = NULL;
    char *name_opt = NULL;
    size_t index;
    size_t cursor;
    int rc = 1;

    if (argv == NULL) {
        fprintf(stderr, "out of memory preparing project build arguments\n");
        return 1;
    }

    for (index = 0U; index < context->source_count; ++index) {
        argv[index] = context->source_paths[index];
    }
    target_opt = dup_printf("--target=%s",
                            context->manifest.target == FENG_COMPILE_TARGET_BIN ? "bin" : "lib");
    out_opt = dup_printf("--out=%s", context->out_root);
    name_opt = dup_printf("--name=%s", context->manifest.name);
    if (target_opt == NULL || out_opt == NULL || name_opt == NULL) {
        fprintf(stderr, "out of memory preparing project build arguments\n");
        goto cleanup;
    }

    argv[context->source_count] = target_opt;
    argv[context->source_count + 1U] = out_opt;
    argv[context->source_count + 2U] = name_opt;
    cursor = context->source_count + 3U;
    if (release) {
        argv[cursor++] = "--release";
    }
    for (index = 0U; index < package_count; ++index) {
        argv[cursor++] = "--pkg";
        argv[cursor++] = (char *)package_paths[index];
    }

    rc = feng_cli_direct_main(program, (int)cursor, argv);

cleanup:
    free(name_opt);
    free(out_opt);
    free(target_opt);
    free(argv);
    return rc;
}

int feng_cli_project_invoke_direct_compile(const char *program,
                                           const FengCliProjectContext *context,
                                           bool release) {
    return feng_cli_project_invoke_direct_compile_with_packages(program,
                                                                context,
                                                                release,
                                                                0U,
                                                                NULL);
}

bool feng_cli_project_resolve_build_dependencies(const char *program,
                                                 const FengCliProjectContext *context,
                                                 bool release,
                                                 FengCliDepsResolved *out_resolved,
                                                 FengCliProjectError *out_error) {
    if (context == NULL || out_resolved == NULL) {
        if (out_error != NULL) {
            out_error->path = NULL;
            out_error->message = NULL;
            out_error->line = 0U;
        }
        return false;
    }

    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    return feng_cli_deps_resolve_for_manifest(program,
                                              context->manifest_path,
                                              false,
                                              release,
                                              out_resolved,
                                              out_error);
}

bool feng_cli_project_prepare_build(const char *program,
                                   const char *path_arg,
                                   bool release,
                                   FengCliProjectContext *out_context,
                                   FengCliDepsResolved *out_resolved,
                                   FengCliProjectError *out_error) {
    FengCliProjectContext context = {0};

    if (out_context == NULL || out_resolved == NULL) {
        if (out_error != NULL) {
            out_error->path = NULL;
            out_error->message = NULL;
            out_error->line = 0U;
        }
        return false;
    }

    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    if (!feng_cli_project_open(path_arg, &context, out_error)) {
        return false;
    }
    if (!feng_cli_project_resolve_build_dependencies(program,
                                                     &context,
                                                     release,
                                                     out_resolved,
                                                     out_error)) {
        feng_cli_project_context_dispose(&context);
        return false;
    }

    *out_context = context;
    return true;
}

int feng_cli_project_compile_prepared(const char *program,
                                      const FengCliProjectContext *context,
                                      const FengCliDepsResolved *resolved,
                                      bool release) {
    return feng_cli_project_invoke_direct_compile_with_packages(program,
                                                                context,
                                                                release,
                                                                resolved->package_count,
                                                                (const char *const *)resolved->package_paths);
}
