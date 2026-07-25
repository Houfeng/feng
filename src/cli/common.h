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

/*
 * Required filesystem object kind for paths resolved relative to the Feng
 * installation root.
 */
typedef enum FengCliRequiredPathKind {
    FENG_CLI_REQUIRED_REGULAR_FILE,
    FENG_CLI_REQUIRED_DIRECTORY,
    FENG_CLI_REQUIRED_EXECUTABLE
} FengCliRequiredPathKind;

/*
 * Declarative lookup policy for one host tool used by Feng.
 * Environment variable values and executable names are never shell-parsed.
 */
typedef struct FengCliHostToolStrategy {
    const char *display_name;
    const char *feng_environment_variable;
    const char *bundled_relative_path;
    const char *conventional_environment_variable;
    const char *system_executable;
} FengCliHostToolStrategy;

/* Result category for host-tool lookup before command-specific fallbacks. */
typedef enum FengCliHostToolLookupStatus {
    FENG_CLI_HOST_TOOL_FOUND,
    FENG_CLI_HOST_TOOL_ABSENT,
    FENG_CLI_HOST_TOOL_ERROR
} FengCliHostToolLookupStatus;

/*
 * Resolve the running Feng executable to a canonical absolute path.
 * The returned string and optional error message are owned by the caller.
 */
char *feng_cli_resolve_executable_path(const char *program_path, char **out_error_message);

/*
 * Resolve one safe relative path below `<feng-executable-dir>/..`.
 * The target itself does not need to exist. The returned string and optional
 * error message are owned by the caller.
 */
char *feng_cli_resolve_install_path(const char *program_path,
                                    const char *relative_path,
                                    char **out_error_message);

/*
 * Resolve and validate one required installation path.
 * The returned string and optional error message are owned by the caller.
 */
char *feng_cli_require_install_path(const char *program_path,
                                    const char *relative_path,
                                    FengCliRequiredPathKind required_kind,
                                    char **out_error_message);

/*
 * Find one executable through PATH without consulting Feng-specific
 * environment variables. The returned string is owned by the caller.
 */
char *feng_cli_find_executable_on_path(const char *program);

/* Return whether one filesystem path exists and is executable. */
bool feng_cli_path_is_executable(const char *path);

/*
 * Resolve one host tool in this order: Feng-specific environment variable,
 * bundled installation path, conventional environment variable, and PATH.
 * The returned string and optional error message are owned by the caller.
 */
char *feng_cli_resolve_host_tool(const char *program_path,
                                 const FengCliHostToolStrategy *strategy,
                                 char **out_error_message);

/*
 * Resolve one host tool while distinguishing an absent candidate chain from
 * an explicitly configured or bundled candidate that is invalid.
 */
FengCliHostToolLookupStatus feng_cli_lookup_host_tool(
    const char *program_path,
    const FengCliHostToolStrategy *strategy,
    char **out_tool_path,
    char **out_error_message);

bool feng_cli_stream_supports_color(FILE *stream);
void feng_cli_set_stream_color(FILE *stream, bool enabled, const char *color);
void feng_cli_reset_stream_color(FILE *stream, bool enabled);

char *feng_cli_read_entire_file(const char *path, size_t *out_length);

void feng_cli_fprint_escaped_slice(FILE *stream, const char *text, size_t length);
void feng_cli_print_escaped_slice(const char *text, size_t length);

void feng_cli_fprint_token_summary(FILE *stream, const FengToken *token);

void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *code,
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
