#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lexer/lexer.h"
#include "parser/parser.h"

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
}

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

static void print_parse_error_context(FILE *stream,
                                      const char *source,
                                      size_t source_length,
                                      const FengParseError *error) {
    size_t index;
    size_t line_start = 0U;
    size_t line_end = source_length;
    unsigned int current_line = 1U;
    unsigned int line_no = error->token.line > 0U ? error->token.line : 1U;
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
    fprint_token_summary(stream, &error->token);
    fputc('\n', stream);

    set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*u | ", digits, line_no);
    fwrite(source + line_start, 1U, line_end - line_start, stream);
    reset_stream_color(stream, use_color);
    fputc('\n', stream);

    set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*s | ", digits, "");
    if (error->token.kind == FENG_TOKEN_EOF) {
        for (index = line_start; index < line_end; ++index) {
            fputc(source[index] == '\t' ? '\t' : ' ', stream);
        }
    } else if (error->token.offset >= line_start && error->token.offset <= line_end) {
        for (index = line_start; index < error->token.offset; ++index) {
            fputc(source[index] == '\t' ? '\t' : ' ', stream);
        }
    } else {
        unsigned int column = error->token.column > 0U ? error->token.column - 1U : 0U;

        for (index = 0U; index < (size_t)column; ++index) {
            fputc(' ', stream);
        }
    }
    fputc('^', stream);
    reset_stream_color(stream, use_color);
    fputc('\n', stream);
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
    bool use_color = stream_supports_color(stderr);

    if (source == NULL) {
        fprintf(stderr, "failed to read %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (!feng_parse_source(source, source_length, path, &program, &error)) {
        fprintf(stderr, "%s:", path);
        set_stream_color(stderr, use_color, FENG_COLOR_RED);
        fprintf(stderr, "%u:%u", error.token.line, error.token.column);
        reset_stream_color(stderr, use_color);
        fprintf(stderr,
            ": parse error: %s\n",
            error.message != NULL ? error.message : "unknown error");
        print_parse_error_context(stderr, source, source_length, &error);
        exit_code = 1;
    } else {
        feng_program_dump(stdout, program);
    }

    feng_program_free(program);
    free(source);
    return exit_code;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "lex") == 0) {
        return run_lex_command(argv[2]);
    }
    if (strcmp(argv[1], "parse") == 0) {
        return run_parse_command(argv[2]);
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
