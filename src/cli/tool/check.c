#include "cli/tool/tool.h"

#include <stdio.h>
#include <string.h>

#include "cli/common.h"
#include "cli/frontend.h"

typedef struct CheckState {
    bool first;
} CheckState;

static void fprint_json_string(FILE *stream, const char *s) {
    if (s == NULL) {
        fputs("null", stream);
        return;
    }
    feng_cli_fprint_escaped_slice(stream, s, strlen(s));
}

static void fprint_check_entry(FILE *stream,
                               bool *first,
                               const char *path,
                               unsigned int line,
                               unsigned int col,
                               size_t token_length,
                               const char *severity,
                               const char *source_kind,
                               const char *message) {
    if (!*first) {
        fputs(",\n", stream);
    }
    *first = false;

    fputs("  {\n", stream);
    fputs("    \"path\": ", stream);
    fprint_json_string(stream, path);
    fprintf(stream, ",\n    \"line\": %u,\n    \"col\": %u,\n    \"end_col\": %u,\n",
            line, col, col + (unsigned int)(token_length > 0U ? token_length : 1U));
    fprintf(stream, "    \"severity\": \"%s\",\n    \"source\": \"%s\",\n", severity, source_kind);
    fputs("    \"message\": ", stream);
    fprint_json_string(stream, message != NULL ? message : "unknown error");
    fputs("\n  }", stream);
}

static void on_parse_error(void *user,
                           const char *path,
                           const FengParseError *error,
                           const FengCliLoadedSource *source) {
    CheckState *state = (CheckState *)user;
    (void)source;
    fprint_check_entry(stdout, &state->first,
                       path,
                       error->token.line,
                       error->token.column,
                       error->token.length,
                       "error", "parse",
                       error->message);
}

static void on_semantic_error(void *user,
                              const FengSemanticError *error,
                              size_t error_index,
                              size_t error_count,
                              const FengCliLoadedSource *source) {
    CheckState *state = (CheckState *)user;
    (void)error_index;
    (void)error_count;
    (void)source;
    fprint_check_entry(stdout, &state->first,
                       error->path,
                       error->token.line,
                       error->token.column,
                       error->token.length,
                       "error", "semantic",
                       error->message);
}

static void on_semantic_info(void *user,
                             const FengSemanticInfo *info,
                             size_t info_index,
                             size_t info_count,
                             const FengCliLoadedSource *source) {
    CheckState *state = (CheckState *)user;
    (void)info_index;
    (void)info_count;
    (void)source;
    fprint_check_entry(stdout, &state->first,
                       info->path,
                       info->token.line,
                       info->token.column,
                       info->token.length,
                       "info", "semantic",
                       info->message);
}

int feng_cli_tool_check_main(const char *program, int argc, char **argv) {
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

    fputs("[\n", stdout);

    CheckState state = { .first = true };
    FengCliFrontendInput input = {
        .path_count = file_argc,
        .paths = file_argv,
        .target = target,
    };
    FengCliFrontendCallbacks callbacks = {
        .on_parse_error = on_parse_error,
        .on_semantic_error = on_semantic_error,
        .on_semantic_info = on_semantic_info,
        .user = &state,
    };

    int rc = feng_cli_frontend_run(&input, &callbacks, NULL);

    fputs("\n]\n", stdout);
    return rc;
}
