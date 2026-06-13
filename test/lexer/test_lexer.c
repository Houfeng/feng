#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer/lexer.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static void assert_lexeme(const FengToken *token, const char *expected) {
    size_t expected_length = strlen(expected);

    ASSERT(token->length == expected_length);
    ASSERT(memcmp(token->lexeme, expected, expected_length) == 0);
}

static void assert_leading_doc(const FengToken *token, const char *expected) {
    size_t expected_length = expected != NULL ? strlen(expected) : 0U;

    ASSERT(token->leading_doc_length == expected_length);
    if (expected == NULL) {
        ASSERT(token->leading_doc == NULL);
        return;
    }

    ASSERT(token->leading_doc != NULL);
    ASSERT(memcmp(token->leading_doc, expected, expected_length) == 0);
}

static FengToken next_token(FengLexer *lexer, FengTokenKind kind) {
    FengToken token = feng_lexer_next(lexer);

    ASSERT(token.kind == kind);
    return token;
}

static void test_keyword_and_annotation_counts(void) {
    FengTokenKind keyword_kind;
    FengAnnotationKind annotation_kind;

    ASSERT(feng_keyword_count() == 28U);
    ASSERT(feng_reserved_word_count() == 5U);
    ASSERT(feng_builtin_annotation_count() == 7U);
    ASSERT(feng_lookup_keyword("enum", 4U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_ENUM);
    ASSERT(feng_lookup_keyword("spec", 4U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_SPEC);
    ASSERT(feng_lookup_keyword("fit", 3U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_FIT);
    ASSERT(feng_lookup_keyword("extern", 6U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_EXTERN);
    ASSERT(feng_lookup_keyword("unknown", 7U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_UNKNOWN);
    ASSERT(!feng_lookup_keyword("finally", 7U, &keyword_kind));
    ASSERT(!feng_lookup_keyword("bool", 4U, &keyword_kind));
    ASSERT(!feng_lookup_keyword("int", 3U, &keyword_kind));
    ASSERT(!feng_lookup_keyword("float", 5U, &keyword_kind));
    ASSERT(feng_is_reserved_word("class", 5U));
    ASSERT(!feng_is_reserved_word("static", 6U));
    ASSERT(feng_lookup_keyword("static", 6U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_STATIC);
    ASSERT(!feng_is_reserved_word("enum", 4U));
    ASSERT(feng_is_reserved_word("const", 5U));
    ASSERT(feng_is_reserved_word("export", 6U));
    ASSERT(!feng_is_reserved_word("fn", 2U));
    ASSERT(!feng_is_reserved_word("mod", 3U));
    ASSERT(!feng_is_reserved_word("use", 3U));
    ASSERT(feng_lookup_keyword("import", 6U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_IMPORT);
    ASSERT(feng_lookup_keyword("module", 6U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_MODULE);
    ASSERT(feng_lookup_keyword("open", 4U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_OPEN);
    ASSERT(feng_lookup_keyword("seal", 4U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_SEAL);
    ASSERT(!feng_is_reserved_word("pu", 2U));
    ASSERT(!feng_is_reserved_word("pr", 2U));
    ASSERT(feng_lookup_keyword("func", 4U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_FUNC);
    ASSERT(feng_is_reserved_word("prop", 4U));
    ASSERT(!feng_is_reserved_word("self", 4U));
    ASSERT(feng_lookup_builtin_annotation("abi", 3U, &annotation_kind));
    ASSERT(annotation_kind == FENG_ANNOTATION_ABI);
    ASSERT(!feng_lookup_builtin_annotation("fixed", 5U, &annotation_kind));
    ASSERT(feng_lookup_builtin_annotation("runtime", 7U, &annotation_kind));
    ASSERT(annotation_kind == FENG_ANNOTATION_RUNTIME);
    ASSERT(!feng_lookup_builtin_annotation("bounded", 7U, &annotation_kind));
    ASSERT(!feng_lookup_builtin_annotation("union", 5U, &annotation_kind));
    ASSERT(feng_lookup_builtin_annotation("iterable", 8U, &annotation_kind));
    ASSERT(annotation_kind == FENG_ANNOTATION_ITERABLE);
    ASSERT(feng_lookup_builtin_annotation("iterator", 8U, &annotation_kind));
    ASSERT(annotation_kind == FENG_ANNOTATION_ITERATOR);
}

static void test_reserved_words_rejected(void) {
    static const char *const reserved_words[] = {
        "class",
        "struct",
        "const",
        "export",
        "prop"
    };
    size_t index;

    for (index = 0; index < sizeof(reserved_words) / sizeof(reserved_words[0]); ++index) {
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer,
                        reserved_words[index],
                        strlen(reserved_words[index]),
                        "reserved.ff");
        token = next_token(&lexer, FENG_TOKEN_ERROR);
        assert_lexeme(&token, reserved_words[index]);
        ASSERT(strstr(token.message, "reserved word") != NULL);
    }
}

static void test_new_keywords_and_builtin_type_names(void) {
    const char *source =
        "enum spec fit func module import unknown bool string int long byte float double i32 u8 f64\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "keywords_and_types.f");

    token = next_token(&lexer, FENG_TOKEN_KW_ENUM);
    assert_lexeme(&token, "enum");
    token = next_token(&lexer, FENG_TOKEN_KW_SPEC);
    assert_lexeme(&token, "spec");
    token = next_token(&lexer, FENG_TOKEN_KW_FIT);
    assert_lexeme(&token, "fit");
    token = next_token(&lexer, FENG_TOKEN_KW_FUNC);
    assert_lexeme(&token, "func");
    token = next_token(&lexer, FENG_TOKEN_KW_MODULE);
    assert_lexeme(&token, "module");
    token = next_token(&lexer, FENG_TOKEN_KW_IMPORT);
    assert_lexeme(&token, "import");
    token = next_token(&lexer, FENG_TOKEN_KW_UNKNOWN);
    assert_lexeme(&token, "unknown");

    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "bool");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "string");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "int");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "long");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "byte");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "float");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "double");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "i32");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "u8");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "f64");
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_removed_reserved_words_are_identifiers(void) {
    const char *source = "pu pr";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "removed_reserved_words.ff");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "pu");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "pr");
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_basic_module_tokens(void) {
    const char *source =
        "open module libc.math;\n"
        "@cdecl(point_lib)\n"
        "extern func sin(x: float): float;\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "basic.f");

    token = next_token(&lexer, FENG_TOKEN_KW_OPEN);
    assert_lexeme(&token, "open");
    ASSERT(token.line == 1U && token.column == 1U);

    token = next_token(&lexer, FENG_TOKEN_KW_MODULE);
    assert_lexeme(&token, "module");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "libc");
    token = next_token(&lexer, FENG_TOKEN_DOT);
    assert_lexeme(&token, ".");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "math");
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);
    assert_lexeme(&token, ";");

    token = next_token(&lexer, FENG_TOKEN_ANNOTATION);
    ASSERT(token.annotation_kind == FENG_ANNOTATION_CDECL);
    assert_lexeme(&token, "@cdecl");
    ASSERT(token.line == 2U && token.column == 1U);

    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "point_lib");
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_KW_EXTERN);
    token = next_token(&lexer, FENG_TOKEN_KW_FUNC);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "sin");
    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "x");
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "float");
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "float");
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);
    token = next_token(&lexer, FENG_TOKEN_EOF);
    ASSERT(token.length == 0U);
}

static void test_runtime_annotation_token(void) {
    const char *source =
        "@runtime\n"
        "extern func feng_string_length(value: string): long;\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "runtime_annotation.f");

    token = next_token(&lexer, FENG_TOKEN_ANNOTATION);
    ASSERT(token.annotation_kind == FENG_ANNOTATION_RUNTIME);
    assert_lexeme(&token, "@runtime");

    token = next_token(&lexer, FENG_TOKEN_KW_EXTERN);
    token = next_token(&lexer, FENG_TOKEN_KW_FUNC);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "feng_string_length");
    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "value");
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "string");
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "long");
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_literals_and_arrow(void) {
    const char *source =
        "let ok = true && false;\n"
        "let flag: bool;\n"
        "let callable = (x: int) -> x * 2;\n"
        "let ratio = 3.14;\n"
        "let nums = [1, 2];\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "literals.f");

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "ok");
    token = next_token(&lexer, FENG_TOKEN_ASSIGN);
    token = next_token(&lexer, FENG_TOKEN_BOOL);
    ASSERT(token.value.boolean);
    token = next_token(&lexer, FENG_TOKEN_AND_AND);
    token = next_token(&lexer, FENG_TOKEN_BOOL);
    ASSERT(!token.value.boolean);
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "flag");
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "bool");
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "callable");
    token = next_token(&lexer, FENG_TOKEN_ASSIGN);
    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "x");
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "int");
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_ARROW);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "x");
    token = next_token(&lexer, FENG_TOKEN_STAR);
    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 2);
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "ratio");
    token = next_token(&lexer, FENG_TOKEN_ASSIGN);
    token = next_token(&lexer, FENG_TOKEN_FLOAT);
    ASSERT(token.value.floating > 3.13 && token.value.floating < 3.15);
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "nums");
    token = next_token(&lexer, FENG_TOKEN_ASSIGN);
    token = next_token(&lexer, FENG_TOKEN_LBRACKET);
    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 1);
    token = next_token(&lexer, FENG_TOKEN_COMMA);
    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 2);
    token = next_token(&lexer, FENG_TOKEN_RBRACKET);
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_comments_crlf_and_custom_annotations(void) {
    const char *source =
        "// comment\r\n"
        "let name = \"feng\\nlang\"; /* block\r\ncomment */\r\n"
        "@memoize\r\n"
        "func noop() {}\r\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "comments.f");

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    ASSERT(token.line == 2U && token.column == 1U);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "name");
    token = next_token(&lexer, FENG_TOKEN_ASSIGN);
    token = next_token(&lexer, FENG_TOKEN_STRING);
    assert_lexeme(&token, "\"feng\\nlang\"");
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);
    token = next_token(&lexer, FENG_TOKEN_ANNOTATION);
    ASSERT(token.annotation_kind == FENG_ANNOTATION_CUSTOM);
    assert_lexeme(&token, "@memoize");
    token = next_token(&lexer, FENG_TOKEN_KW_FUNC);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "noop");
    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_LBRACE);
    token = next_token(&lexer, FENG_TOKEN_RBRACE);
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_doc_comment_attaches_to_next_token(void) {
    const char *source =
        "/** doc for run */\n"
        "@memoize\n"
        "open func run() {}\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "doc_comments.f");

    token = next_token(&lexer, FENG_TOKEN_ANNOTATION);
    assert_leading_doc(&token, "/** doc for run */");
    token = next_token(&lexer, FENG_TOKEN_KW_OPEN);
    assert_leading_doc(&token, NULL);
    token = next_token(&lexer, FENG_TOKEN_KW_FUNC);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "run");
    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_LBRACE);
    token = next_token(&lexer, FENG_TOKEN_RBRACE);
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_doc_comment_binding_breaks_on_blank_line_and_normal_comment(void) {
    const char *source =
        "/** lost by blank line */\n"
        "\n"
        "let a: int;\n"
        "/** lost by normal comment */\n"
        "// separator\n"
        "let b: int;\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "doc_breaks.f");

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    assert_leading_doc(&token, NULL);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);

    token = next_token(&lexer, FENG_TOKEN_KW_LET);
    assert_leading_doc(&token, NULL);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    token = next_token(&lexer, FENG_TOKEN_COLON);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    token = next_token(&lexer, FENG_TOKEN_SEMICOLON);
    token = next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_error_tokens(void) {
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, "@1", 2U, "annotation_error.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "annotation") != NULL);

    feng_lexer_init(&lexer, "/* unterminated", strlen("/* unterminated"), "comment_error.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "comment") != NULL);

    feng_lexer_init(&lexer, "\"oops", strlen("\"oops"), "string_error.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "string") != NULL);

    feng_lexer_init(&lexer, "123abc", 6U, "number_error.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "numeric") != NULL);
}

static void test_hex_escape_valid(void) {
    FengLexer lexer;
    FengToken token;

    /* \x1b alone */
    feng_lexer_init(&lexer, "\"\\x1b\"", 6U, "hex_esc.f");
    token = next_token(&lexer, FENG_TOKEN_STRING);
    assert_lexeme(&token, "\"\\x1b\"");

    /* \x1b followed by normal chars */
    feng_lexer_init(&lexer, "\"\\x1b[32m\"", 10U, "hex_esc2.f");
    token = next_token(&lexer, FENG_TOKEN_STRING);
    assert_lexeme(&token, "\"\\x1b[32m\"");

    /* consecutive \xNN */
    feng_lexer_init(&lexer, "\"\\xe4\\xb8\\xad\"", 14U, "hex_esc3.f");
    token = next_token(&lexer, FENG_TOKEN_STRING);
    assert_lexeme(&token, "\"\\xe4\\xb8\\xad\"");

    /* uppercase hex digits */
    feng_lexer_init(&lexer, "\"\\xAB\\xCD\"", 10U, "hex_esc4.f");
    token = next_token(&lexer, FENG_TOKEN_STRING);
    assert_lexeme(&token, "\"\\xAB\\xCD\"");

    /* \xNN followed by hex digit char (should be separate) */
    feng_lexer_init(&lexer, "\"\\x1b1\"", 7U, "hex_esc5.f");
    token = next_token(&lexer, FENG_TOKEN_STRING);
    assert_lexeme(&token, "\"\\x1b1\"");
}

static void test_hex_escape_invalid(void) {
    FengLexer lexer;
    FengToken token;

    /* \x with only 1 hex digit */
    feng_lexer_init(&lexer, "\"\\x1\"", 5U, "hex_err1.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "\\x") != NULL);

    /* \x with no hex digits */
    feng_lexer_init(&lexer, "\"\\x\"", 4U, "hex_err2.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "\\x") != NULL);

    /* \x with non-hex character */
    feng_lexer_init(&lexer, "\"\\xGG\"", 6U, "hex_err3.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "\\x") != NULL);

    /* \x at end of string */
    feng_lexer_init(&lexer, "\"abc\\x\"", 7U, "hex_err4.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "\\x") != NULL);
}

static void test_bitwise_tokens(void) {
    FengLexer lexer;
    const char *source = "a & b | c ^ d << 1 >> 2 ~e";

    feng_lexer_init(&lexer, source, strlen(source), "bitwise.f");
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_AMP);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_PIPE);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_CARET);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_SHL);
    next_token(&lexer, FENG_TOKEN_INTEGER);
    next_token(&lexer, FENG_TOKEN_SHR);
    next_token(&lexer, FENG_TOKEN_INTEGER);
    next_token(&lexer, FENG_TOKEN_TILDE);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_compound_assignment_tokens(void) {
    FengLexer lexer;
    const char *source =
        "a += b -= c *= d /= e %= f &= g |= h ^= i <<= 1 >>= 2";

    feng_lexer_init(&lexer, source, strlen(source), "compound_assign.f");
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_PLUS_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_MINUS_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_STAR_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_SLASH_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_PERCENT_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_AMP_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_PIPE_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_CARET_ASSIGN);
    next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    next_token(&lexer, FENG_TOKEN_SHL_ASSIGN);
    next_token(&lexer, FENG_TOKEN_INTEGER);
    next_token(&lexer, FENG_TOKEN_SHR_ASSIGN);
    next_token(&lexer, FENG_TOKEN_INTEGER);
    next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_numeric_literal_bases_and_separators(void) {
    FengLexer lexer;
    FengToken token;
    const char *source =
        "255 0xFF 0xab 0x_bad 0b1111_1111 0o377 1_000_000 3.14 1.5e3 2.0e-4 1E2";

    feng_lexer_init(&lexer, source, strlen(source), "numbers.f");

    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 255);

    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 0xFF);

    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 0xAB);

    /* 0x_bad: '_' immediately after prefix -> error */
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "numeric") != NULL);

    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 0xFF);

    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 255);

    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    ASSERT(token.value.integer == 1000000);

    token = next_token(&lexer, FENG_TOKEN_FLOAT);
    ASSERT(token.value.floating == 3.14);

    token = next_token(&lexer, FENG_TOKEN_FLOAT);
    ASSERT(token.value.floating == 1500.0);

    token = next_token(&lexer, FENG_TOKEN_FLOAT);
    ASSERT(token.value.floating == 2.0e-4);

    token = next_token(&lexer, FENG_TOKEN_FLOAT);
    ASSERT(token.value.floating == 100.0);

    next_token(&lexer, FENG_TOKEN_EOF);
}

static void test_numeric_literal_rejects_trailing_underscore(void) {
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, "1_", 2U, "trailing_underscore.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "numeric") != NULL);

    feng_lexer_init(&lexer, "0x_", 3U, "prefix_underscore.f");
    token = next_token(&lexer, FENG_TOKEN_ERROR);
    ASSERT(strstr(token.message, "numeric") != NULL);
}

static void test_flow_control_tokens(void) {
    const char *source = "for let it in items 1...10";
    FengLexer lexer;
    FengToken token;
    FengTokenKind keyword_kind;

    ASSERT(feng_lookup_keyword("in", 2U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_IN);

    feng_lexer_init(&lexer, source, strlen(source), "flow_tokens.f");
    token = next_token(&lexer, FENG_TOKEN_KW_FOR);
    assert_lexeme(&token, "for");
    (void)next_token(&lexer, FENG_TOKEN_KW_LET);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "it");
    token = next_token(&lexer, FENG_TOKEN_KW_IN);
    assert_lexeme(&token, "in");
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "items");
    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    assert_lexeme(&token, "1");
    token = next_token(&lexer, FENG_TOKEN_ELLIPSIS);
    assert_lexeme(&token, "...");
    token = next_token(&lexer, FENG_TOKEN_INTEGER);
    assert_lexeme(&token, "10");
    (void)next_token(&lexer, FENG_TOKEN_EOF);
}

/* G1-3: Verify generic-related token sequences produced by the lexer.
 * The lexer makes no special provisions for generics; `:<` is always two
 * tokens (COLON + LT) and `>>` is always SHR.  The parser resolves
 * ambiguity via pending_gt without any lexer changes. */
static void test_generic_token_sequences(void) {
    /* `:<` is always COLON followed by LT (never a merged token). */
    {
        const char *src = ":<";
        FengLexer lexer;

        feng_lexer_init(&lexer, src, strlen(src), "g1.f");
        (void)next_token(&lexer, FENG_TOKEN_COLON);
        (void)next_token(&lexer, FENG_TOKEN_LT);
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Simple generic type reference: Map<int> → IDENT LT IDENT GT */
    {
        const char *src = "Map<int>";
        FengLexer lexer;

        feng_lexer_init(&lexer, src, strlen(src), "g1.f");
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_LT);
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_GT);
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Nested generic: Map<List<int>> → IDENT LT IDENT LT IDENT SHR
     * The closing `>>` is produced as a single SHR token; the parser
     * is responsible for splitting it via pending_gt. */
    {
        const char *src = "Map<List<int>>";
        FengLexer lexer;

        feng_lexer_init(&lexer, src, strlen(src), "g1.f");
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_LT);
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_LT);
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_SHR); /* >> as a single SHR */
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Shift-right expression stays as SHR and is NOT split by the lexer. */
    {
        const char *src = "a >> b";
        FengLexer lexer;

        feng_lexer_init(&lexer, src, strlen(src), "g1.f");
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_SHR);
        (void)next_token(&lexer, FENG_TOKEN_IDENTIFIER);
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }
}

static void test_raw_string_literals(void) {
    /* Test basic raw string: `\d+` should produce RAW_STRING with lexeme including delimiters */
    {
        const char *src = "`\\d+`";
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer, src, strlen(src), "raw1.f");
        token = next_token(&lexer, FENG_TOKEN_STRING);
        assert_lexeme(&token, "`\\d+`");
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Test raw string with embedded backtick: `say ``hello``` -> lexeme includes delimiters and `` */
    {
        const char *src = "`say ``hello```";
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer, src, strlen(src), "raw2.f");
        token = next_token(&lexer, FENG_TOKEN_STRING);
        assert_lexeme(&token, "`say ``hello```");
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Test empty raw string: `` */
    {
        const char *src = "``";
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer, src, strlen(src), "raw3.f");
        token = next_token(&lexer, FENG_TOKEN_STRING);
        assert_lexeme(&token, "``");
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Test raw string with newline preserved */
    {
        const char *src = "`line1\nline2`";
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer, src, strlen(src), "raw4.f");
        token = next_token(&lexer, FENG_TOKEN_STRING);
        assert_lexeme(&token, "`line1\nline2`");
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }

    /* Test raw string with literal \n (not escape) */
    {
        const char *src = "`hello\\nworld`";
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer, src, strlen(src), "raw5.f");
        token = next_token(&lexer, FENG_TOKEN_STRING);
        assert_lexeme(&token, "`hello\\nworld`");
        (void)next_token(&lexer, FENG_TOKEN_EOF);
    }
}

static void test_raw_string_unterminated(void) {
    /* Unterminated raw string should produce ERROR token */
    {
        const char *src = "`unterminated";
        FengLexer lexer;
        FengToken token;

        feng_lexer_init(&lexer, src, strlen(src), "raw_err.f");
        token = next_token(&lexer, FENG_TOKEN_ERROR);
        ASSERT(strstr(token.message, "unterminated") != NULL);
    }
}

int main(void) {
    test_keyword_and_annotation_counts();
    test_reserved_words_rejected();
    test_removed_reserved_words_are_identifiers();
    test_new_keywords_and_builtin_type_names();
    test_basic_module_tokens();
    test_runtime_annotation_token();
    test_literals_and_arrow();
    test_comments_crlf_and_custom_annotations();
    test_doc_comment_attaches_to_next_token();
    test_doc_comment_binding_breaks_on_blank_line_and_normal_comment();
    test_error_tokens();
    test_hex_escape_valid();
    test_hex_escape_invalid();
    test_bitwise_tokens();
    test_compound_assignment_tokens();
    test_numeric_literal_bases_and_separators();
    test_numeric_literal_rejects_trailing_underscore();
    test_flow_control_tokens();
    test_generic_token_sequences();
    test_raw_string_literals();
    test_raw_string_unterminated();

    puts("lexer tests passed");
    return 0;
}
