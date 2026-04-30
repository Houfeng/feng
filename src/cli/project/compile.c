#include "cli/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

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

int feng_cli_project_invoke_direct_compile(const char *program,
                                           const FengCliProjectContext *context,
                                           bool release) {
    size_t argc = context->source_count + 3U + (release ? 1U : 0U);
    char **argv = (char **)calloc(argc, sizeof(*argv));
    char *target_opt = NULL;
    char *out_opt = NULL;
    char *name_opt = NULL;
    size_t index;
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
    if (release) {
        argv[context->source_count + 3U] = "--release";
    }

    rc = feng_cli_direct_main(program, (int)argc, argv);

cleanup:
    free(name_opt);
    free(out_opt);
    free(target_opt);
    free(argv);
    return rc;
}
