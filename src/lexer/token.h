#ifndef FENG_LEXER_TOKEN_H
#define FENG_LEXER_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FENG_KEYWORD_LIST(X) \
    X(TYPE, "type") \
    X(SPEC, "spec") \
    X(FIT, "fit") \
    X(EXTERN, "extern") \
    X(FN, "fn") \
    X(LET, "let") \
    X(VAR, "var") \
    X(PU, "pu") \
    X(PR, "pr") \
    X(SELF, "self") \
    X(MOD, "mod") \
    X(USE, "use") \
    X(AS, "as") \
    X(IF, "if") \
    X(ELSE, "else") \
    X(WHILE, "while") \
    X(FOR, "for") \
    X(BREAK, "break") \
    X(CONTINUE, "continue") \
    X(TRY, "try") \
    X(CATCH, "catch") \
    X(FINALLY, "finally") \
    X(THROW, "throw") \
    X(RETURN, "return") \
    X(VOID, "void")

#define FENG_RESERVED_WORD_LIST(X) \
    X(CLASS, "class") \
    X(STRUCT, "struct") \
    X(PUBLIC, "public") \
    X(PRIVATE, "private") \
    X(PUB, "pub") \
    X(PRO, "pro") \
    X(GET, "get") \
    X(SET, "set") \
    X(THIS, "this") \
    X(INTERFACE, "interface") \
    X(STATIC, "static") \
    X(ENUM, "enum") \
    X(CONST, "const") \
    X(ABSTRACT, "abstract") \
    X(CHAR, "char") \
    X(IS, "is")

#define FENG_BUILTIN_ANNOTATION_LIST(X) \
    X(FIXED, "fixed") \
    X(UNION, "union") \
    X(CDECL, "cdecl") \
    X(STDCALL, "stdcall") \
    X(FASTCALL, "fastcall") \
    X(BOUNDED, "bounded")

typedef enum FengTokenKind {
    FENG_TOKEN_EOF = 0,
    FENG_TOKEN_ERROR,
    FENG_TOKEN_IDENTIFIER,
    FENG_TOKEN_ANNOTATION,
    FENG_TOKEN_INTEGER,
    FENG_TOKEN_FLOAT,
    FENG_TOKEN_STRING,
    FENG_TOKEN_BOOL,

#define FENG_DECLARE_KEYWORD_TOKEN(name, text) FENG_TOKEN_KW_##name,
    FENG_KEYWORD_LIST(FENG_DECLARE_KEYWORD_TOKEN)
#undef FENG_DECLARE_KEYWORD_TOKEN

    FENG_TOKEN_LPAREN,
    FENG_TOKEN_RPAREN,
    FENG_TOKEN_LBRACE,
    FENG_TOKEN_RBRACE,
    FENG_TOKEN_LBRACKET,
    FENG_TOKEN_RBRACKET,
    FENG_TOKEN_COMMA,
    FENG_TOKEN_COLON,
    FENG_TOKEN_SEMICOLON,
    FENG_TOKEN_DOT,
    FENG_TOKEN_PLUS,
    FENG_TOKEN_MINUS,
    FENG_TOKEN_STAR,
    FENG_TOKEN_SLASH,
    FENG_TOKEN_PERCENT,
    FENG_TOKEN_ASSIGN,
    FENG_TOKEN_NOT,
    FENG_TOKEN_LT,
    FENG_TOKEN_LE,
    FENG_TOKEN_GT,
    FENG_TOKEN_GE,
    FENG_TOKEN_EQ,
    FENG_TOKEN_NE,
    FENG_TOKEN_AND_AND,
    FENG_TOKEN_OR_OR,
    FENG_TOKEN_ARROW,
    FENG_TOKEN_TILDE
} FengTokenKind;

typedef enum FengAnnotationKind {
    FENG_ANNOTATION_NONE = 0,
    FENG_ANNOTATION_CUSTOM,
#define FENG_DECLARE_ANNOTATION_KIND(name, text) FENG_ANNOTATION_##name,
    FENG_BUILTIN_ANNOTATION_LIST(FENG_DECLARE_ANNOTATION_KIND)
#undef FENG_DECLARE_ANNOTATION_KIND
} FengAnnotationKind;

typedef struct FengToken {
    FengTokenKind kind;
    FengAnnotationKind annotation_kind;
    const char *lexeme;
    size_t length;
    size_t offset;
    unsigned int line;
    unsigned int column;
    const char *message;
    union {
        int64_t integer;
        double floating;
        bool boolean;
    } value;
} FengToken;

typedef struct FengKeywordSpec {
    const char *text;
    size_t length;
    FengTokenKind kind;
} FengKeywordSpec;

typedef struct FengReservedWordSpec {
    const char *text;
    size_t length;
} FengReservedWordSpec;

typedef struct FengAnnotationSpec {
    const char *text;
    size_t length;
    FengAnnotationKind kind;
} FengAnnotationSpec;

const char *feng_token_kind_name(FengTokenKind kind);
const char *feng_annotation_kind_name(FengAnnotationKind kind);

size_t feng_keyword_count(void);
const FengKeywordSpec *feng_keywords(void);
bool feng_lookup_keyword(const char *text, size_t length, FengTokenKind *out_kind);

size_t feng_reserved_word_count(void);
const FengReservedWordSpec *feng_reserved_words(void);
bool feng_is_reserved_word(const char *text, size_t length);

size_t feng_builtin_annotation_count(void);
const FengAnnotationSpec *feng_builtin_annotations(void);
bool feng_lookup_builtin_annotation(const char *text, size_t length, FengAnnotationKind *out_kind);

bool feng_token_is_keyword(FengTokenKind kind);

#ifdef __cplusplus
}
#endif

#endif
