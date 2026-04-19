#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer/lexer.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s lex <file>\n", program);
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

static void print_escaped_slice(const char *text, size_t length) {
    size_t index;

    putchar('"');
    for (index = 0; index < length; ++index) {
        unsigned char c = (unsigned char)text[index];

        switch (c) {
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (c >= 32U && c <= 126U) {
                    putchar((int)c);
                } else {
                    printf("\\x%02X", c);
                }
                break;
        }
    }
    putchar('"');
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

int main(int argc, char **argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "lex") == 0) {
        return run_lex_command(argv[2]);
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
