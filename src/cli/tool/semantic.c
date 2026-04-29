#include "cli/tool/tool.h"

#include <stdio.h>
#include <string.h>

#include "cli/common.h"
#include "cli/frontend.h"

static void on_parse_error(void *user,
                           const char *path,
                           const FengParseError *error,
                           const FengCliLoadedSource *source) {
    (void)user;
    feng_cli_print_diagnostic(stderr,
                              path,
                              "parse error",
                              error->message,
                              &error->token,
                              source != NULL ? source->source : NULL,
                              source != NULL ? source->source_length : 0U);
}

static void on_semantic_error(void *user,
                              const FengSemanticError *error,
                              size_t error_index,
                              size_t error_count,
                              const FengCliLoadedSource *source) {
    (void)user;
    (void)error_count;
    if (error_index > 0U) {
        fputc('\n', stderr);
    }
    feng_cli_print_diagnostic(stderr,
                              error->path,
                              "semantic error",
                              error->message,
                              &error->token,
                              source != NULL ? source->source : NULL,
                              source != NULL ? source->source_length : 0U);
}

static void on_semantic_info(void *user,
                             const FengSemanticInfo *info,
                             size_t info_index,
                             size_t info_count,
                             const FengCliLoadedSource *source) {
    (void)user;
    (void)info_index;
    (void)info_count;
    feng_cli_print_diagnostic(stderr,
                              info->path,
                              "info",
                              info->message,
                              &info->token,
                              source != NULL ? source->source : NULL,
                              source != NULL ? source->source_length : 0U);
}

int feng_cli_tool_semantic_main(const char *program, int argc, char **argv) {
    FengCompileTarget target = FENG_COMPILE_TARGET_BIN;
    int file_argc = argc;
    char **file_argv = argv;

    if (file_argc > 0 && strncmp(file_argv[0], "--target", 8) == 0) {
        if (!feng_cli_parse_target_option(file_argv[0], &target)) {
            feng_cli_tool_print_usage(program, stderr);
            return 1;
        }
        file_argc -= 1;
        file_argv += 1;
    }
    if (file_argc <= 0) {
        feng_cli_tool_print_usage(program, stderr);
        return 1;
    }

    FengCliFrontendInput input = {
        .path_count = file_argc,
        .paths = file_argv,
        .target = target,
    };
    FengCliFrontendCallbacks callbacks = {
        .on_parse_error = on_parse_error,
        .on_semantic_error = on_semantic_error,
        .on_semantic_info = on_semantic_info,
        .user = NULL,
    };

    return feng_cli_frontend_run(&input, &callbacks, NULL);
}
