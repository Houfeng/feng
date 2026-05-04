#include "cli/common.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool feng_cli_stream_supports_color(FILE *stream) {
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

void feng_cli_set_stream_color(FILE *stream, bool enabled, const char *color) {
    if (enabled) {
        fputs(color, stream);
    }
}

void feng_cli_reset_stream_color(FILE *stream, bool enabled) {
    if (enabled) {
        fputs(FENG_COLOR_RESET, stream);
    }
}

char *feng_cli_read_entire_file(const char *path, size_t *out_length) {
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

void feng_cli_fprint_escaped_slice(FILE *stream, const char *text, size_t length) {
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

void feng_cli_print_escaped_slice(const char *text, size_t length) {
    feng_cli_fprint_escaped_slice(stdout, text, length);
}

void feng_cli_fprint_token_summary(FILE *stream, const FengToken *token) {
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
        feng_cli_fprint_escaped_slice(stream, token->lexeme, token->length);
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
    bool use_color = feng_cli_stream_supports_color(stream);

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
    feng_cli_fprint_token_summary(stream, token);
    fputc('\n', stream);

    feng_cli_set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*u | ", digits, line_no);
    fwrite(source + line_start, 1U, line_end - line_start, stream);
    feng_cli_reset_stream_color(stream, use_color);
    fputc('\n', stream);

    feng_cli_set_stream_color(stream, use_color, FENG_COLOR_RED);
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
    feng_cli_reset_stream_color(stream, use_color);
    fputc('\n', stream);
}

void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *kind,
                               const char *message,
                               const FengToken *token,
                               const char *source,
                               size_t source_length) {
    bool use_color = feng_cli_stream_supports_color(stream);

    fprintf(stream, "%s:", path);
    feng_cli_set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "%u:%u", token->line, token->column);
    feng_cli_reset_stream_color(stream, use_color);
    fprintf(stream, ": %s: %s\n", kind, message != NULL ? message : "unknown error");

    if (source != NULL) {
        print_error_context(stream, source, source_length, token);
    }
}

bool feng_cli_parse_target_option(const char *arg, FengCompileTarget *out_target) {
    const char *value = NULL;

    if (arg == NULL || out_target == NULL) {
        return false;
    }
    if (strncmp(arg, "--target=", 9) == 0) {
        value = arg + 9;
    } else if (strcmp(arg, "--target") == 0) {
        return false;
    } else {
        return false;
    }
    if (strcmp(value, "bin") == 0) {
        *out_target = FENG_COMPILE_TARGET_BIN;
        return true;
    }
    if (strcmp(value, "lib") == 0) {
        *out_target = FENG_COMPILE_TARGET_LIB;
        return true;
    }
    fprintf(stderr, "invalid --target value '%s' (expected 'bin' or 'lib')\n", value);
    return false;
}

const FengCliLoadedSource *feng_cli_find_loaded_source(const FengCliLoadedSource *sources,
                                                       size_t source_count,
                                                       const char *path) {
    size_t index;

    if (path == NULL) {
        return NULL;
    }
    for (index = 0U; index < source_count; ++index) {
        if (sources[index].path != NULL && strcmp(sources[index].path, path) == 0) {
            return &sources[index];
        }
    }

    return NULL;
}

void feng_cli_free_loaded_sources(FengCliLoadedSource *sources, size_t source_count) {
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
