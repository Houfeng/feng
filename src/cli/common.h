#ifndef FENG_CLI_COMMON_H
#define FENG_CLI_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "lexer/token.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

/*
 * Shared helpers used by every CLI command:
 *   - source loading and lifecycle
 *   - terminal-aware diagnostic rendering
 *   - small parameter parsing primitives
 *
 * Keep this header minimal: it must not pull in codegen or driver
 * headers, so that lightweight tool subcommands do not drag in those
 * translation units.
 */

#define FENG_COLOR_RED "\x1b[31m"
#define FENG_COLOR_RESET "\x1b[0m"

bool feng_cli_stream_supports_color(FILE *stream);
void feng_cli_set_stream_color(FILE *stream, bool enabled, const char *color);
void feng_cli_reset_stream_color(FILE *stream, bool enabled);

char *feng_cli_read_entire_file(const char *path, size_t *out_length);

void feng_cli_fprint_escaped_slice(FILE *stream, const char *text, size_t length);
void feng_cli_print_escaped_slice(const char *text, size_t length);

void feng_cli_fprint_token_summary(FILE *stream, const FengToken *token);

void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *kind,
                               const char *message,
                               const FengToken *token,
                               const char *source,
                               size_t source_length);

bool feng_cli_parse_target_option(const char *arg, FengCompileTarget *out_target);

typedef struct FengCliLoadedSource {
    const char *path;
    char *source;
    size_t source_length;
    FengProgram *program;
} FengCliLoadedSource;

const FengCliLoadedSource *feng_cli_find_loaded_source(const FengCliLoadedSource *sources,
                                                       size_t source_count,
                                                       const char *path);

void feng_cli_free_loaded_sources(FengCliLoadedSource *sources, size_t source_count);

#endif /* FENG_CLI_COMMON_H */
