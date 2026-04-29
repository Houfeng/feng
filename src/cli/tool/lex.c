#include "cli/tool/tool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/common.h"
#include "lexer/lexer.h"

static void dump_token(const FengToken *token) {
    printf("%4u:%-4u %-14s ", token->line, token->column, feng_token_kind_name(token->kind));

    if (token->kind == FENG_TOKEN_ANNOTATION) {
        printf("%-12s ", feng_annotation_kind_name(token->annotation_kind));
    } else {
        printf("%-12s ", "-");
    }

    feng_cli_print_escaped_slice(token->lexeme, token->length);

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

int feng_cli_tool_lex_main(const char *program, int argc, char **argv) {
    if (argc != 1) {
        feng_cli_tool_print_usage(program, stderr);
        return 1;
    }

    const char *path = argv[0];
    size_t source_length = 0;
    char *source = feng_cli_read_entire_file(path, &source_length);
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
