#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#define FENG_COLOR_RED "\x1b[31m"
#define FENG_COLOR_RESET "\x1b[0m"

static bool stream_supports_color(FILE *stream) {
    const char *force_color = getenv("CLICOLOR_FORCE");
    const char *no_color = getenv("NO_COLOR");

    if (no_color != NULL && no_color[0] != '\0') {
        return false;
    }
    if (force_color != NULL && force_color[0] != '\0' && strcmp(force_color, "0") != 0) {
        return true;
    }

    return isatty(fileno(stream)) != 0;
}

static void set_stream_color(FILE *stream, bool enabled, const char *color) {
    if (enabled) {
        fputs(color, stream);
    }
}

static void reset_stream_color(FILE *stream, bool enabled) {
    if (enabled) {
        fputs(FENG_COLOR_RESET, stream);
    }
}

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s lex <file>\n", program);
    fprintf(stderr, "  %s parse <file>\n", program);
    fprintf(stderr, "  %s semantic <file> [more files...]\n", program);
}

typedef struct LoadedSource {
    const char *path;
    char *source;
    size_t source_length;
    FengProgram *program;
} LoadedSource;

static char *read_entire_file(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;
    size_t read_size;

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0L) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1U, (size_t)size, file);
    fclose(file);

    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    *out_length = (size_t)size;
    return buffer;
}

static void fprint_escaped_slice(FILE *stream, const char *text, size_t length) {
    size_t index;

    fputc('"', stream);
    for (index = 0; index < length; ++index) {
        unsigned char c = (unsigned char)text[index];

        switch (c) {
            case '\\':
                fputs("\\\\", stream);
                break;
            case '"':
                fputs("\\\"", stream);
                break;
            case '\n':
                fputs("\\n", stream);
                break;
            case '\r':
                fputs("\\r", stream);
                break;
            case '\t':
                fputs("\\t", stream);
                break;
            default:
                if (c >= 32U && c <= 126U) {
                    fputc((int)c, stream);
                } else {
                    fprintf(stream, "\\x%02X", c);
                }
                break;
        }
    }
    fputc('"', stream);
}

static void print_escaped_slice(const char *text, size_t length) {
    fprint_escaped_slice(stdout, text, length);
}

static void fprint_token_summary(FILE *stream, const FengToken *token) {
    if (token->kind == FENG_TOKEN_EOF) {
        fputs("EOF", stream);
        return;
    }

    fputs(feng_token_kind_name(token->kind), stream);
    if (token->kind == FENG_TOKEN_ANNOTATION) {
        fputc(' ', stream);
        fputs(feng_annotation_kind_name(token->annotation_kind), stream);
    }
    if (token->length > 0U) {
        fputc(' ', stream);
        fprint_escaped_slice(stream, token->lexeme, token->length);
    }
}

static unsigned int count_digits(unsigned int value) {
    unsigned int digits = 1U;

    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }

    return digits;
}

static void print_error_context(FILE *stream,
                                const char *source,
                                size_t source_length,
                                const FengToken *token) {
    size_t index;
    size_t line_start = 0U;
    size_t line_end = source_length;
    unsigned int current_line = 1U;
    unsigned int line_no = token->line > 0U ? token->line : 1U;
    unsigned int digits = count_digits(line_no);
    bool use_color = stream_supports_color(stream);

    for (index = 0U; index < source_length; ++index) {
        if (current_line == line_no) {
            line_start = index;
            break;
        }
        if (source[index] == '\n') {
            ++current_line;
            line_start = index + 1U;
        }
    }

    line_end = line_start;
    while (line_end < source_length && source[line_end] != '\n') {
        ++line_end;
    }
    if (line_end > line_start && source[line_end - 1U] == '\r') {
        --line_end;
    }

    fprintf(stream, "  got: ");
    fprint_token_summary(stream, token);
    fputc('\n', stream);

    set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*u | ", digits, line_no);
    fwrite(source + line_start, 1U, line_end - line_start, stream);
    reset_stream_color(stream, use_color);
    fputc('\n', stream);

    set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*s | ", digits, "");
    if (token->kind == FENG_TOKEN_EOF) {
        for (index = line_start; index < line_end; ++index) {
            fputc(source[index] == '\t' ? '\t' : ' ', stream);
        }
    } else if (token->offset >= line_start && token->offset <= line_end) {
        for (index = line_start; index < token->offset; ++index) {
            fputc(source[index] == '\t' ? '\t' : ' ', stream);
        }
    } else {
        unsigned int column = token->column > 0U ? token->column - 1U : 0U;

        for (index = 0U; index < (size_t)column; ++index) {
            fputc(' ', stream);
        }
    }
    fputc('^', stream);
    reset_stream_color(stream, use_color);
    fputc('\n', stream);
}

static void print_diagnostic(FILE *stream,
                             const char *path,
                             const char *kind,
                             const char *message,
                             const FengToken *token,
                             const char *source,
                             size_t source_length) {
    bool use_color = stream_supports_color(stream);

    fprintf(stream, "%s:", path);
    set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "%u:%u", token->line, token->column);
    reset_stream_color(stream, use_color);
    fprintf(stream, ": %s: %s\n", kind, message != NULL ? message : "unknown error");

    if (source != NULL) {
        print_error_context(stream, source, source_length, token);
    }
}

static const LoadedSource *find_loaded_source(const LoadedSource *sources,
                                              size_t source_count,
                                              const char *path) {
    size_t index;

    for (index = 0U; index < source_count; ++index) {
        if (sources[index].path != NULL && strcmp(sources[index].path, path) == 0) {
            return &sources[index];
        }
    }

    return NULL;
}

static void free_loaded_sources(LoadedSource *sources, size_t source_count) {
    size_t index;

    if (sources == NULL) {
        return;
    }

    for (index = 0U; index < source_count; ++index) {
        feng_program_free(sources[index].program);
        free(sources[index].source);
    }
    free(sources);
}

static void dump_token(const FengToken *token) {
    printf("%4u:%-4u %-14s ", token->line, token->column, feng_token_kind_name(token->kind));

    if (token->kind == FENG_TOKEN_ANNOTATION) {
        printf("%-12s ", feng_annotation_kind_name(token->annotation_kind));
    } else {
        printf("%-12s ", "-");
    }

    print_escaped_slice(token->lexeme, token->length);

    switch (token->kind) {
        case FENG_TOKEN_INTEGER:
            printf("  value=%lld", (long long)token->value.integer);
            break;
        case FENG_TOKEN_FLOAT:
            printf("  value=%.17g", token->value.floating);
            break;
        case FENG_TOKEN_BOOL:
            printf("  value=%s", token->value.boolean ? "true" : "false");
            break;
        case FENG_TOKEN_ERROR:
            if (token->message != NULL) {
                printf("  error=%s", token->message);
            }
            break;
        default:
            break;
    }

    putchar('\n');
}

static int run_lex_command(const char *path) {
    size_t source_length = 0;
    char *source = read_entire_file(path, &source_length);
    FengLexer lexer;
    int exit_code = 0;

    if (source == NULL) {
        fprintf(stderr, "failed to read %s: %s\n", path, strerror(errno));
        return 1;
    }

    feng_lexer_init(&lexer, source, source_length, path);

    for (;;) {
        FengToken token = feng_lexer_next(&lexer);

        dump_token(&token);
        if (token.kind == FENG_TOKEN_ERROR) {
            exit_code = 1;
            break;
        }
        if (token.kind == FENG_TOKEN_EOF) {
            break;
        }
    }

    free(source);
    return exit_code;
}

static int run_parse_command(const char *path) {
    size_t source_length = 0;
    char *source = read_entire_file(path, &source_length);
    FengProgram *program = NULL;
    FengParseError error;
    int exit_code = 0;

    if (source == NULL) {
        fprintf(stderr, "failed to read %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (!feng_parse_source(source, source_length, path, &program, &error)) {
        print_diagnostic(stderr,
                         path,
                         "parse error",
                         error.message,
                         &error.token,
                         source,
                         source_length);
        exit_code = 1;
    } else {
        feng_program_dump(stdout, program);
    }

    feng_program_free(program);
    free(source);
    return exit_code;
}

static int run_semantic_command(int path_count, char **paths) {
    LoadedSource *sources = NULL;
    const FengProgram **programs = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    int exit_code = 0;
    int path_index;

    sources = (LoadedSource *)calloc((size_t)path_count, sizeof(*sources));
    programs = (const FengProgram **)calloc((size_t)path_count, sizeof(*programs));
    if (sources == NULL || programs == NULL) {
        fprintf(stderr, "out of memory\n");
        free_loaded_sources(sources, (size_t)path_count);
        free(programs);
        return 1;
    }

    for (path_index = 0; path_index < path_count; ++path_index) {
        FengParseError error;

        sources[path_index].path = paths[path_index];
        sources[path_index].source = read_entire_file(paths[path_index], &sources[path_index].source_length);
        if (sources[path_index].source == NULL) {
            fprintf(stderr, "failed to read %s: %s\n", paths[path_index], strerror(errno));
            exit_code = 1;
            goto cleanup;
        }

        if (!feng_parse_source(sources[path_index].source,
                               sources[path_index].source_length,
                               paths[path_index],
                               &sources[path_index].program,
                               &error)) {
            print_diagnostic(stderr,
                             paths[path_index],
                             "parse error",
                             error.message,
                             &error.token,
                             sources[path_index].source,
                             sources[path_index].source_length);
            exit_code = 1;
            goto cleanup;
        }

        programs[path_index] = sources[path_index].program;
    }

    if (!feng_semantic_analyze(programs,
                               (size_t)path_count,
                               &analysis,
                               &errors,
                               &error_count)) {
        size_t error_index;

        if (error_count == 0U) {
            fprintf(stderr, "semantic analysis failed\n");
            exit_code = 1;
            goto cleanup;
        }

        for (error_index = 0U; error_index < error_count; ++error_index) {
            const LoadedSource *source = find_loaded_source(sources, (size_t)path_count, errors[error_index].path);

            if (error_index > 0U) {
                fputc('\n', stderr);
            }

            print_diagnostic(stderr,
                             errors[error_index].path,
                             "semantic error",
                             errors[error_index].message,
                             &errors[error_index].token,
                             source != NULL ? source->source : NULL,
                             source != NULL ? source->source_length : 0U);
        }
        exit_code = 1;
    }

    if (analysis != NULL && analysis->info_count > 0U) {
        size_t info_index;

        for (info_index = 0U; info_index < analysis->info_count; ++info_index) {
            const LoadedSource *source = find_loaded_source(
                sources, (size_t)path_count, analysis->infos[info_index].path);

            print_diagnostic(stderr,
                             analysis->infos[info_index].path,
                             "info",
                             analysis->infos[info_index].message,
                             &analysis->infos[info_index].token,
                             source != NULL ? source->source : NULL,
                             source != NULL ? source->source_length : 0U);
        }
    }

cleanup:
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    free(programs);
    free_loaded_sources(sources, (size_t)path_count);
    return exit_code;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "lex") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        return run_lex_command(argv[2]);
    }
    if (strcmp(argv[1], "parse") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        return run_parse_command(argv[2]);
    }
    if (strcmp(argv[1], "semantic") == 0) {
        return run_semantic_command(argc - 2, argv + 2);
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
