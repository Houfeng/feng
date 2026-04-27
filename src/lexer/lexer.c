#include "lexer/lexer.h"

#include <stdlib.h>
#include <string.h>

static bool lexer_at_end(const FengLexer *lexer) {
    return lexer->current >= lexer->length;
}

static char lexer_peek_raw(const FengLexer *lexer, size_t lookahead) {
    size_t index = lexer->current + lookahead;

    if (index >= lexer->length) {
        return '\0';
    }

    return lexer->source[index];
}

static char lexer_advance(FengLexer *lexer) {
    char current_char;

    if (lexer_at_end(lexer)) {
        return '\0';
    }

    current_char = lexer->source[lexer->current];

    if (current_char == '\r') {
        ++lexer->current;
        if (!lexer_at_end(lexer) && lexer->source[lexer->current] == '\n') {
            ++lexer->current;
        }
        ++lexer->line;
        lexer->column = 1U;
        return '\n';
    }

    if (current_char == '\n') {
        ++lexer->current;
        ++lexer->line;
        lexer->column = 1U;
        return '\n';
    }

    ++lexer->current;
    ++lexer->column;
    return current_char;
}

static bool lexer_match(FengLexer *lexer, char expected) {
    if (lexer_peek_raw(lexer, 0) != expected) {
        return false;
    }

    (void)lexer_advance(lexer);
    return true;
}

static bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c) {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_binary_digit(char c) {
    return c == '0' || c == '1';
}

static bool is_octal_digit(char c) {
    return c >= '0' && c <= '7';
}

static bool is_digit_of_base(char c, int base) {
    switch (base) {
        case 2:
            return is_binary_digit(c);
        case 8:
            return is_octal_digit(c);
        case 16:
            return is_hex_digit(c);
        default:
            return is_ascii_digit(c);
    }
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return 0;
}

static bool is_identifier_start(char c) {
    return is_ascii_alpha(c) || c == '_';
}

static bool is_identifier_continue(char c) {
    return is_identifier_start(c) || is_ascii_digit(c);
}

static bool is_horizontal_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static FengToken make_token(FengLexer *lexer,
                            FengTokenKind kind,
                            size_t start_offset,
                            unsigned int start_line,
                            unsigned int start_column) {
    FengToken token;

    token.kind = kind;
    token.annotation_kind = FENG_ANNOTATION_NONE;
    token.lexeme = lexer->source + start_offset;
    token.length = lexer->current - start_offset;
    token.offset = start_offset;
    token.line = start_line;
    token.column = start_column;
    token.message = NULL;
    token.value.integer = 0;
    return token;
}

static FengToken make_error(FengLexer *lexer,
                            size_t start_offset,
                            unsigned int start_line,
                            unsigned int start_column,
                            const char *message) {
    FengToken token = make_token(lexer, FENG_TOKEN_ERROR, start_offset, start_line, start_column);

    token.message = message;
    lexer->last_error = message;
    return token;
}

static int64_t parse_integer_slice(const char *text, size_t length) {
    int64_t value = 0;
    size_t index = 0;
    int base = 10;

    if (length >= 2 && text[0] == '0') {
        char p = text[1];
        if (p == 'x' || p == 'X') {
            base = 16;
            index = 2;
        } else if (p == 'b' || p == 'B') {
            base = 2;
            index = 2;
        } else if (p == 'o' || p == 'O') {
            base = 8;
            index = 2;
        }
    }

    for (; index < length; ++index) {
        char c = text[index];
        if (c == '_') {
            continue;
        }
        value = (value * (int64_t)base) + (int64_t)hex_digit_value(c);
    }

    return value;
}

static double parse_float_slice(const char *text, size_t length) {
    char stack_buffer[128];
    char *buffer = stack_buffer;
    double value;
    size_t src_index;
    size_t dst_index = 0;

    if (length + 1U > sizeof(stack_buffer)) {
        buffer = (char *)malloc(length + 1U);
        if (buffer == NULL) {
            return 0.0;
        }
    }

    for (src_index = 0; src_index < length; ++src_index) {
        char c = text[src_index];
        if (c != '_') {
            buffer[dst_index++] = c;
        }
    }
    buffer[dst_index] = '\0';
    value = strtod(buffer, NULL);

    if (buffer != stack_buffer) {
        free(buffer);
    }

    return value;
}

static bool skip_whitespace_and_comments(FengLexer *lexer, FengToken *out_error) {
    while (!lexer_at_end(lexer)) {
        char c = lexer_peek_raw(lexer, 0);

        if (is_horizontal_whitespace(c) || c == '\n' || c == '\r') {
            (void)lexer_advance(lexer);
            continue;
        }

        if (c == '/' && lexer_peek_raw(lexer, 1) == '/') {
            (void)lexer_advance(lexer);
            (void)lexer_advance(lexer);

            while (!lexer_at_end(lexer)) {
                char comment_char = lexer_peek_raw(lexer, 0);

                if (comment_char == '\n' || comment_char == '\r') {
                    break;
                }

                (void)lexer_advance(lexer);
            }
            continue;
        }

        if (c == '/' && lexer_peek_raw(lexer, 1) == '*') {
            size_t start_offset = lexer->current;
            unsigned int start_line = lexer->line;
            unsigned int start_column = lexer->column;
            bool found_terminator = false;

            (void)lexer_advance(lexer);
            (void)lexer_advance(lexer);

            while (!lexer_at_end(lexer)) {
                if (lexer_peek_raw(lexer, 0) == '*' && lexer_peek_raw(lexer, 1) == '/') {
                    (void)lexer_advance(lexer);
                    (void)lexer_advance(lexer);
                    found_terminator = true;
                    break;
                }
                (void)lexer_advance(lexer);
            }

            if (!found_terminator) {
                *out_error = make_error(lexer,
                                        start_offset,
                                        start_line,
                                        start_column,
                                        "unterminated block comment");
                return false;
            }
            continue;
        }

        break;
    }

    return true;
}

static FengToken scan_identifier_or_keyword(FengLexer *lexer,
                                            size_t start_offset,
                                            unsigned int start_line,
                                            unsigned int start_column) {
    FengToken token;
    size_t length;
    const char *text;
    FengTokenKind keyword_kind;

    while (is_identifier_continue(lexer_peek_raw(lexer, 0))) {
        (void)lexer_advance(lexer);
    }

    token = make_token(lexer, FENG_TOKEN_IDENTIFIER, start_offset, start_line, start_column);
    length = token.length;
    text = token.lexeme;

    if (length == 4U && memcmp(text, "true", 4U) == 0) {
        token.kind = FENG_TOKEN_BOOL;
        token.value.boolean = true;
        return token;
    }

    if (length == 5U && memcmp(text, "false", 5U) == 0) {
        token.kind = FENG_TOKEN_BOOL;
        token.value.boolean = false;
        return token;
    }

    if (feng_is_reserved_word(text, length)) {
        return make_error(lexer,
                          start_offset,
                          start_line,
                          start_column,
                          "reserved word cannot be used as an identifier in the current language version");
    }

    if (feng_lookup_keyword(text, length, &keyword_kind)) {
        token.kind = keyword_kind;
    }

    return token;
}

static FengToken scan_annotation(FengLexer *lexer,
                                 size_t start_offset,
                                 unsigned int start_line,
                                 unsigned int start_column) {
    FengToken token;
    FengAnnotationKind builtin_kind;

    if (!is_identifier_start(lexer_peek_raw(lexer, 0))) {
        return make_error(lexer,
                          start_offset,
                          start_line,
                          start_column,
                          "expected annotation name after '@'");
    }

    (void)lexer_advance(lexer);
    while (is_identifier_continue(lexer_peek_raw(lexer, 0))) {
        (void)lexer_advance(lexer);
    }

    token = make_token(lexer, FENG_TOKEN_ANNOTATION, start_offset, start_line, start_column);
    if (feng_lookup_builtin_annotation(token.lexeme + 1, token.length - 1U, &builtin_kind)) {
        token.annotation_kind = builtin_kind;
    } else {
        token.annotation_kind = FENG_ANNOTATION_CUSTOM;
    }

    return token;
}

static void consume_digit_run(FengLexer *lexer, int base) {
    /* Consume digits, allowing `_` only strictly between two digits. The caller
     * is responsible for guaranteeing the first character consumed before the
     * run is a digit, and for checking "at least one digit follows a prefix".
     * A trailing underscore is not consumed here, so lexeme never ends in `_`. */
    while (true) {
        char c = lexer_peek_raw(lexer, 0);
        if (is_digit_of_base(c, base)) {
            (void)lexer_advance(lexer);
            continue;
        }
        if (c == '_' && is_digit_of_base(lexer_peek_raw(lexer, 1), base)) {
            (void)lexer_advance(lexer); /* the '_' */
            (void)lexer_advance(lexer); /* next digit */
            continue;
        }
        break;
    }
}

static FengToken scan_number(FengLexer *lexer,
                             size_t start_offset,
                             unsigned int start_line,
                             unsigned int start_column) {
    FengToken token;
    bool is_float = false;
    int base = 10;
    char first = lexer->source[start_offset];

    if (first == '0') {
        char prefix = lexer_peek_raw(lexer, 0);

        if (prefix == 'x' || prefix == 'X') {
            base = 16;
            (void)lexer_advance(lexer);
            if (!is_hex_digit(lexer_peek_raw(lexer, 0))) {
                while (is_identifier_continue(lexer_peek_raw(lexer, 0))) {
                    (void)lexer_advance(lexer);
                }
                return make_error(lexer, start_offset, start_line, start_column,
                                  "invalid numeric literal");
            }
            (void)lexer_advance(lexer);
            consume_digit_run(lexer, 16);
        } else if (prefix == 'b' || prefix == 'B') {
            base = 2;
            (void)lexer_advance(lexer);
            if (!is_binary_digit(lexer_peek_raw(lexer, 0))) {
                while (is_identifier_continue(lexer_peek_raw(lexer, 0))) {
                    (void)lexer_advance(lexer);
                }
                return make_error(lexer, start_offset, start_line, start_column,
                                  "invalid numeric literal");
            }
            (void)lexer_advance(lexer);
            consume_digit_run(lexer, 2);
        } else if (prefix == 'o' || prefix == 'O') {
            base = 8;
            (void)lexer_advance(lexer);
            if (!is_octal_digit(lexer_peek_raw(lexer, 0))) {
                while (is_identifier_continue(lexer_peek_raw(lexer, 0))) {
                    (void)lexer_advance(lexer);
                }
                return make_error(lexer, start_offset, start_line, start_column,
                                  "invalid numeric literal");
            }
            (void)lexer_advance(lexer);
            consume_digit_run(lexer, 8);
        } else {
            consume_digit_run(lexer, 10);
        }
    } else {
        consume_digit_run(lexer, 10);
    }

    if (base == 10) {
        if (lexer_peek_raw(lexer, 0) == '.' && is_ascii_digit(lexer_peek_raw(lexer, 1))) {
            is_float = true;
            (void)lexer_advance(lexer); /* '.' */
            (void)lexer_advance(lexer); /* first fractional digit */
            consume_digit_run(lexer, 10);
        }

        {
            char exp_marker = lexer_peek_raw(lexer, 0);
            if (exp_marker == 'e' || exp_marker == 'E') {
                char after_marker = lexer_peek_raw(lexer, 1);
                char after_sign = (after_marker == '+' || after_marker == '-')
                                      ? lexer_peek_raw(lexer, 2)
                                      : after_marker;

                if (is_ascii_digit(after_sign)) {
                    is_float = true;
                    (void)lexer_advance(lexer); /* 'e'/'E' */
                    if (after_marker == '+' || after_marker == '-') {
                        (void)lexer_advance(lexer);
                    }
                    (void)lexer_advance(lexer); /* first exponent digit */
                    consume_digit_run(lexer, 10);
                }
            }
        }
    }

    if (is_identifier_start(lexer_peek_raw(lexer, 0))) {
        while (is_identifier_continue(lexer_peek_raw(lexer, 0))) {
            (void)lexer_advance(lexer);
        }
        return make_error(lexer,
                          start_offset,
                          start_line,
                          start_column,
                          "invalid numeric literal");
    }

    token = make_token(lexer,
                       is_float ? FENG_TOKEN_FLOAT : FENG_TOKEN_INTEGER,
                       start_offset,
                       start_line,
                       start_column);

    if (token.kind == FENG_TOKEN_INTEGER) {
        token.value.integer = parse_integer_slice(token.lexeme, token.length);
    } else {
        token.value.floating = parse_float_slice(token.lexeme, token.length);
    }

    return token;
}

static FengToken scan_string(FengLexer *lexer,
                             size_t start_offset,
                             unsigned int start_line,
                             unsigned int start_column) {
    while (!lexer_at_end(lexer)) {
        char c = lexer_peek_raw(lexer, 0);

        if (c == '"') {
            (void)lexer_advance(lexer);
            return make_token(lexer, FENG_TOKEN_STRING, start_offset, start_line, start_column);
        }

        if (c == '\n' || c == '\r') {
            return make_error(lexer,
                              start_offset,
                              start_line,
                              start_column,
                              "unterminated string literal");
        }

        if (c == '\\') {
            (void)lexer_advance(lexer);
            if (lexer_at_end(lexer)) {
                return make_error(lexer,
                                  start_offset,
                                  start_line,
                                  start_column,
                                  "unterminated string literal");
            }

            c = lexer_advance(lexer);
            switch (c) {
                case '\\':
                case '"':
                case 'n':
                case 'r':
                case 't':
                case '0':
                    break;
                default:
                    return make_error(lexer,
                                      start_offset,
                                      start_line,
                                      start_column,
                                      "invalid string escape");
            }
            continue;
        }

        (void)lexer_advance(lexer);
    }

    return make_error(lexer,
                      start_offset,
                      start_line,
                      start_column,
                      "unterminated string literal");
}

static FengToken scan_token_internal(FengLexer *lexer) {
    FengToken trivia_error;
    size_t start_offset;
    unsigned int start_line;
    unsigned int start_column;
    char c;

    if (!skip_whitespace_and_comments(lexer, &trivia_error)) {
        return trivia_error;
    }

    start_offset = lexer->current;
    start_line = lexer->line;
    start_column = lexer->column;

    if (lexer_at_end(lexer)) {
        return make_token(lexer, FENG_TOKEN_EOF, start_offset, start_line, start_column);
    }

    c = lexer_advance(lexer);

    if (is_identifier_start(c)) {
        return scan_identifier_or_keyword(lexer, start_offset, start_line, start_column);
    }

    if (is_ascii_digit(c)) {
        return scan_number(lexer, start_offset, start_line, start_column);
    }

    switch (c) {
        case '@':
            return scan_annotation(lexer, start_offset, start_line, start_column);
        case '"':
            return scan_string(lexer, start_offset, start_line, start_column);
        case '(':
            return make_token(lexer, FENG_TOKEN_LPAREN, start_offset, start_line, start_column);
        case ')':
            return make_token(lexer, FENG_TOKEN_RPAREN, start_offset, start_line, start_column);
        case '{':
            return make_token(lexer, FENG_TOKEN_LBRACE, start_offset, start_line, start_column);
        case '}':
            return make_token(lexer, FENG_TOKEN_RBRACE, start_offset, start_line, start_column);
        case '[':
            return make_token(lexer, FENG_TOKEN_LBRACKET, start_offset, start_line, start_column);
        case ']':
            return make_token(lexer, FENG_TOKEN_RBRACKET, start_offset, start_line, start_column);
        case ',':
            return make_token(lexer, FENG_TOKEN_COMMA, start_offset, start_line, start_column);
        case ':':
            return make_token(lexer, FENG_TOKEN_COLON, start_offset, start_line, start_column);
        case ';':
            return make_token(lexer, FENG_TOKEN_SEMICOLON, start_offset, start_line, start_column);
        case '.':
            if (lexer_peek_raw(lexer, 0) == '.' && lexer_peek_raw(lexer, 1) == '.') {
                (void)lexer_advance(lexer);
                (void)lexer_advance(lexer);
                return make_token(lexer, FENG_TOKEN_ELLIPSIS, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_DOT, start_offset, start_line, start_column);
        case '+':
            return make_token(lexer, FENG_TOKEN_PLUS, start_offset, start_line, start_column);
        case '*':
            return make_token(lexer, FENG_TOKEN_STAR, start_offset, start_line, start_column);
        case '/':
            return make_token(lexer, FENG_TOKEN_SLASH, start_offset, start_line, start_column);
        case '%':
            return make_token(lexer, FENG_TOKEN_PERCENT, start_offset, start_line, start_column);
        case '~':
            return make_token(lexer, FENG_TOKEN_TILDE, start_offset, start_line, start_column);
        case '-':
            if (lexer_match(lexer, '>')) {
                return make_token(lexer, FENG_TOKEN_ARROW, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_MINUS, start_offset, start_line, start_column);
        case '=':
            if (lexer_match(lexer, '=')) {
                return make_token(lexer, FENG_TOKEN_EQ, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_ASSIGN, start_offset, start_line, start_column);
        case '!':
            if (lexer_match(lexer, '=')) {
                return make_token(lexer, FENG_TOKEN_NE, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_NOT, start_offset, start_line, start_column);
        case '<':
            if (lexer_match(lexer, '=')) {
                return make_token(lexer, FENG_TOKEN_LE, start_offset, start_line, start_column);
            }
            if (lexer_match(lexer, '<')) {
                return make_token(lexer, FENG_TOKEN_SHL, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_LT, start_offset, start_line, start_column);
        case '>':
            if (lexer_match(lexer, '=')) {
                return make_token(lexer, FENG_TOKEN_GE, start_offset, start_line, start_column);
            }
            if (lexer_match(lexer, '>')) {
                return make_token(lexer, FENG_TOKEN_SHR, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_GT, start_offset, start_line, start_column);
        case '&':
            if (lexer_match(lexer, '&')) {
                return make_token(lexer, FENG_TOKEN_AND_AND, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_AMP, start_offset, start_line, start_column);
        case '|':
            if (lexer_match(lexer, '|')) {
                return make_token(lexer, FENG_TOKEN_OR_OR, start_offset, start_line, start_column);
            }
            return make_token(lexer, FENG_TOKEN_PIPE, start_offset, start_line, start_column);
        case '^':
            return make_token(lexer, FENG_TOKEN_CARET, start_offset, start_line, start_column);
        default:
            return make_error(lexer,
                              start_offset,
                              start_line,
                              start_column,
                              "unexpected character");
    }
}

void feng_lexer_init(FengLexer *lexer, const char *source, size_t length, const char *path) {
    lexer->source = source;
    lexer->length = length;
    lexer->path = path;
    feng_lexer_reset(lexer);
}

void feng_lexer_reset(FengLexer *lexer) {
    lexer->current = 0;
    lexer->line = 1U;
    lexer->column = 1U;
    lexer->last_error = NULL;
    lexer->has_peeked = 0;
    lexer->peeked.kind = FENG_TOKEN_EOF;
    lexer->peeked.annotation_kind = FENG_ANNOTATION_NONE;
    lexer->peeked.lexeme = lexer->source;
    lexer->peeked.length = 0;
    lexer->peeked.offset = 0;
    lexer->peeked.line = 1U;
    lexer->peeked.column = 1U;
    lexer->peeked.message = NULL;
    lexer->peeked.value.integer = 0;
}

FengToken feng_lexer_next(FengLexer *lexer) {
    if (lexer->has_peeked) {
        lexer->has_peeked = 0;
        return lexer->peeked;
    }

    return scan_token_internal(lexer);
}

FengToken feng_lexer_peek(FengLexer *lexer) {
    if (!lexer->has_peeked) {
        lexer->peeked = scan_token_internal(lexer);
        lexer->has_peeked = 1;
    }

    return lexer->peeked;
}

const char *feng_lexer_last_error(const FengLexer *lexer) {
    return lexer->last_error;
}

const char *feng_lexer_path(const FengLexer *lexer) {
    return lexer->path;
}
