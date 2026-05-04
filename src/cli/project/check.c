#include "cli/cli.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cli/common.h"
#include "cli/deps/manager.h"
#include "cli/frontend.h"
#include "cli/project/common.h"

typedef enum CheckOutputFormat {
    CHECK_OUTPUT_TEXT = 0,
    CHECK_OUTPUT_JSON
} CheckOutputFormat;

typedef struct JsonState {
    bool first;
} JsonState;

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s check [<path>] [--format <text|json>]\n", program);
}

static void fprint_json_string(FILE *stream, const char *text) {
    if (text == NULL) {
        fputs("null", stream);
        return;
    }
    feng_cli_fprint_escaped_slice(stream, text, strlen(text));
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
    fprintf(stream,
            ",\n    \"line\": %u,\n    \"col\": %u,\n    \"end_col\": %u,\n",
            line,
            col,
            col + (unsigned int)(token_length > 0U ? token_length : 1U));
    fprintf(stream,
            "    \"severity\": \"%s\",\n    \"source\": \"%s\",\n",
            severity,
            source_kind);
    fputs("    \"message\": ", stream);
    fprint_json_string(stream, message != NULL ? message : "unknown error");
    fputs("\n  }", stream);
}

static void on_text_parse_error(void *user,
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

static void on_text_semantic_error(void *user,
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

static void on_text_semantic_info(void *user,
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

static void on_json_parse_error(void *user,
                                const char *path,
                                const FengParseError *error,
                                const FengCliLoadedSource *source) {
    JsonState *state = (JsonState *)user;
    (void)source;
    fprint_check_entry(stdout,
                       &state->first,
                       path,
                       error->token.line,
                       error->token.column,
                       error->token.length,
                       "error",
                       "parse",
                       error->message);
}

static void on_json_semantic_error(void *user,
                                   const FengSemanticError *error,
                                   size_t error_index,
                                   size_t error_count,
                                   const FengCliLoadedSource *source) {
    JsonState *state = (JsonState *)user;
    (void)error_index;
    (void)error_count;
    (void)source;
    fprint_check_entry(stdout,
                       &state->first,
                       error->path,
                       error->token.line,
                       error->token.column,
                       error->token.length,
                       "error",
                       "semantic",
                       error->message);
}

static void on_json_semantic_info(void *user,
                                  const FengSemanticInfo *info,
                                  size_t info_index,
                                  size_t info_count,
                                  const FengCliLoadedSource *source) {
    JsonState *state = (JsonState *)user;
    (void)info_index;
    (void)info_count;
    (void)source;
    fprint_check_entry(stdout,
                       &state->first,
                       info->path,
                       info->token.line,
                       info->token.column,
                       info->token.length,
                       "info",
                       "semantic",
                       info->message);
}

static bool parse_args(const char *program,
                       int argc,
                       char **argv,
                       const char **out_path,
                       CheckOutputFormat *out_format) {
    int index;

    *out_path = NULL;
    *out_format = CHECK_OUTPUT_TEXT;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program);
            return false;
        }
        if (strcmp(arg, "--format") == 0) {
            const char *value;

            if (index + 1 >= argc) {
                fprintf(stderr, "--format requires a value\n");
                print_usage(program);
                return false;
            }
            value = argv[++index];
            if (strcmp(value, "text") == 0) {
                *out_format = CHECK_OUTPUT_TEXT;
            } else if (strcmp(value, "json") == 0) {
                *out_format = CHECK_OUTPUT_JSON;
            } else {
                fprintf(stderr, "invalid --format value '%s'\n", value);
                print_usage(program);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "--format=", 9) == 0) {
            const char *value = arg + 9;
            if (strcmp(value, "text") == 0) {
                *out_format = CHECK_OUTPUT_TEXT;
            } else if (strcmp(value, "json") == 0) {
                *out_format = CHECK_OUTPUT_JSON;
            } else {
                fprintf(stderr, "invalid --format value '%s'\n", value);
                print_usage(program);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program);
            return false;
        }
        if (*out_path != NULL) {
            fprintf(stderr, "check accepts at most one <path> argument\n");
            print_usage(program);
            return false;
        }
        *out_path = arg;
    }

    return true;
}

int feng_cli_project_check_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    CheckOutputFormat format = CHECK_OUTPUT_TEXT;
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    char *manifest_path = NULL;
    FengCliFrontendInput input;
    FengCliFrontendCallbacks callbacks;
    JsonState json_state = { .first = true };
    int rc;

    if (!parse_args(program, argc, argv, &path_arg, &format)) {
        return 1;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(path_arg, &manifest_path, &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_project_error_dispose(&error);
        return 1;
    }
    if (!feng_cli_project_open(manifest_path, &context, &error)) {
        free(manifest_path);
        feng_cli_project_print_error(stderr, &error);
        feng_cli_project_error_dispose(&error);
        return 1;
    }
    free(manifest_path);
    if (!feng_cli_deps_resolve_for_manifest(program,
                                            context.manifest_path,
                                            false,
                                            false,
                                            &resolved,
                                            &error)) {
        feng_cli_project_print_error(stderr, &error);
        feng_cli_deps_resolved_dispose(&resolved);
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return 1;
    }

    input.path_count = (int)context.source_count;
    input.paths = context.source_paths;
    input.target = context.manifest.target;
    input.package_path_count = (int)resolved.package_count;
    input.package_paths = (const char **)resolved.package_paths;

    if (format == CHECK_OUTPUT_JSON) {
        fputs("[\n", stdout);
        callbacks.on_parse_error = on_json_parse_error;
        callbacks.on_semantic_error = on_json_semantic_error;
        callbacks.on_semantic_info = on_json_semantic_info;
        callbacks.user = &json_state;
    } else {
        callbacks.on_parse_error = on_text_parse_error;
        callbacks.on_semantic_error = on_text_semantic_error;
        callbacks.on_semantic_info = on_text_semantic_info;
        callbacks.user = NULL;
    }

    rc = feng_cli_frontend_run(&input, &callbacks, NULL);

    if (format == CHECK_OUTPUT_JSON) {
        fputs("\n]\n", stdout);
    }

    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_context_dispose(&context);
    feng_cli_project_error_dispose(&error);
    return rc;
}
