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

static FengToken next_token(FengLexer *lexer, FengTokenKind kind) {
    FengToken token = feng_lexer_next(lexer);

    ASSERT(token.kind == kind);
    return token;
}

static void test_keyword_and_annotation_counts(void) {
    FengTokenKind keyword_kind;
    FengAnnotationKind annotation_kind;

    ASSERT(feng_keyword_count() == 25U);
    ASSERT(feng_reserved_word_count() == 16U);
    ASSERT(feng_builtin_annotation_count() == 6U);
    ASSERT(feng_lookup_keyword("spec", 4U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_SPEC);
    ASSERT(feng_lookup_keyword("fit", 3U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_FIT);
    ASSERT(feng_lookup_keyword("extern", 6U, &keyword_kind));
    ASSERT(keyword_kind == FENG_TOKEN_KW_EXTERN);
    ASSERT(!feng_lookup_keyword("bool", 4U, &keyword_kind));
    ASSERT(!feng_lookup_keyword("int", 3U, &keyword_kind));
    ASSERT(!feng_lookup_keyword("float", 5U, &keyword_kind));
    ASSERT(feng_is_reserved_word("class", 5U));
    ASSERT(feng_is_reserved_word("interface", 9U));
    ASSERT(feng_is_reserved_word("static", 6U));
    ASSERT(feng_is_reserved_word("enum", 4U));
    ASSERT(feng_is_reserved_word("const", 5U));
    ASSERT(feng_is_reserved_word("abstract", 8U));
    ASSERT(feng_is_reserved_word("char", 4U));
    ASSERT(feng_is_reserved_word("is", 2U));
    ASSERT(!feng_is_reserved_word("self", 4U));
    ASSERT(feng_lookup_builtin_annotation("fixed", 5U, &annotation_kind));
    ASSERT(annotation_kind == FENG_ANNOTATION_FIXED);
    ASSERT(feng_lookup_builtin_annotation("bounded", 7U, &annotation_kind));
    ASSERT(annotation_kind == FENG_ANNOTATION_BOUNDED);
}

static void test_reserved_words_rejected(void) {
    static const char *const reserved_words[] = {
        "class",
        "struct",
        "public",
        "private",
        "pub",
        "pro",
        "get",
        "set",
        "this",
        "interface",
        "static",
        "enum",
        "const",
        "abstract",
        "char",
        "is"
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
        "spec fit bool string int long byte float double i32 u8 f64\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "keywords_and_types.f");

    token = next_token(&lexer, FENG_TOKEN_KW_SPEC);
    assert_lexeme(&token, "spec");
    token = next_token(&lexer, FENG_TOKEN_KW_FIT);
    assert_lexeme(&token, "fit");

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

static void test_basic_module_tokens(void) {
    const char *source =
        "pu mod libc.math;\n"
        "@cdecl(point_lib)\n"
        "extern fn sin(x: float): float;\n";
    FengLexer lexer;
    FengToken token;

    feng_lexer_init(&lexer, source, strlen(source), "basic.f");

    token = next_token(&lexer, FENG_TOKEN_KW_PU);
    assert_lexeme(&token, "pu");
    ASSERT(token.line == 1U && token.column == 1U);

    token = next_token(&lexer, FENG_TOKEN_KW_MOD);
    assert_lexeme(&token, "mod");
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
    token = next_token(&lexer, FENG_TOKEN_KW_FN);
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

static void test_literals_and_arrow(void) {
    const char *source =
        "let ok = true && false;\n"
        "let flag: bool;\n"
        "let func = (x: int) -> x * 2;\n"
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
    assert_lexeme(&token, "func");
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
        "fn noop() {}\r\n";
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
    token = next_token(&lexer, FENG_TOKEN_KW_FN);
    token = next_token(&lexer, FENG_TOKEN_IDENTIFIER);
    assert_lexeme(&token, "noop");
    token = next_token(&lexer, FENG_TOKEN_LPAREN);
    token = next_token(&lexer, FENG_TOKEN_RPAREN);
    token = next_token(&lexer, FENG_TOKEN_LBRACE);
    token = next_token(&lexer, FENG_TOKEN_RBRACE);
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

int main(void) {
    test_keyword_and_annotation_counts();
    test_reserved_words_rejected();
    test_new_keywords_and_builtin_type_names();
    test_basic_module_tokens();
    test_literals_and_arrow();
    test_comments_crlf_and_custom_annotations();
    test_error_tokens();

    puts("lexer tests passed");
    return 0;
}
