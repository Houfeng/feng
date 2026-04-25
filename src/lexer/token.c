#include "lexer/token.h"

#include <string.h>

#define FENG_KEYWORD_SPEC(name, text) {text, sizeof(text) - 1U, FENG_TOKEN_KW_##name},
static const FengKeywordSpec g_keywords[] = {
    FENG_KEYWORD_LIST(FENG_KEYWORD_SPEC)
};
#undef FENG_KEYWORD_SPEC

#define FENG_RESERVED_WORD_SPEC(name, text) {text, sizeof(text) - 1U},
static const FengReservedWordSpec g_reserved_words[] = {
    FENG_RESERVED_WORD_LIST(FENG_RESERVED_WORD_SPEC)
};
#undef FENG_RESERVED_WORD_SPEC

#define FENG_ANNOTATION_SPEC(name, text) {text, sizeof(text) - 1U, FENG_ANNOTATION_##name},
static const FengAnnotationSpec g_builtin_annotations[] = {
    FENG_BUILTIN_ANNOTATION_LIST(FENG_ANNOTATION_SPEC)
};
#undef FENG_ANNOTATION_SPEC

const char *feng_token_kind_name(FengTokenKind kind) {
    switch (kind) {
        case FENG_TOKEN_EOF:
            return "EOF";
        case FENG_TOKEN_ERROR:
            return "ERROR";
        case FENG_TOKEN_IDENTIFIER:
            return "IDENTIFIER";
        case FENG_TOKEN_ANNOTATION:
            return "ANNOTATION";
        case FENG_TOKEN_INTEGER:
            return "INTEGER";
        case FENG_TOKEN_FLOAT:
            return "FLOAT";
        case FENG_TOKEN_STRING:
            return "STRING";
        case FENG_TOKEN_BOOL:
            return "BOOL";
#define FENG_KEYWORD_NAME(name, text) \
        case FENG_TOKEN_KW_##name: \
            return "KW_" #name;
        FENG_KEYWORD_LIST(FENG_KEYWORD_NAME)
#undef FENG_KEYWORD_NAME
        case FENG_TOKEN_LPAREN:
            return "LPAREN";
        case FENG_TOKEN_RPAREN:
            return "RPAREN";
        case FENG_TOKEN_LBRACE:
            return "LBRACE";
        case FENG_TOKEN_RBRACE:
            return "RBRACE";
        case FENG_TOKEN_LBRACKET:
            return "LBRACKET";
        case FENG_TOKEN_RBRACKET:
            return "RBRACKET";
        case FENG_TOKEN_COMMA:
            return "COMMA";
        case FENG_TOKEN_COLON:
            return "COLON";
        case FENG_TOKEN_SEMICOLON:
            return "SEMICOLON";
        case FENG_TOKEN_DOT:
            return "DOT";
        case FENG_TOKEN_PLUS:
            return "PLUS";
        case FENG_TOKEN_MINUS:
            return "MINUS";
        case FENG_TOKEN_STAR:
            return "STAR";
        case FENG_TOKEN_SLASH:
            return "SLASH";
        case FENG_TOKEN_PERCENT:
            return "PERCENT";
        case FENG_TOKEN_ASSIGN:
            return "ASSIGN";
        case FENG_TOKEN_NOT:
            return "NOT";
        case FENG_TOKEN_LT:
            return "LT";
        case FENG_TOKEN_LE:
            return "LE";
        case FENG_TOKEN_GT:
            return "GT";
        case FENG_TOKEN_GE:
            return "GE";
        case FENG_TOKEN_EQ:
            return "EQ";
        case FENG_TOKEN_NE:
            return "NE";
        case FENG_TOKEN_AND_AND:
            return "AND_AND";
        case FENG_TOKEN_OR_OR:
            return "OR_OR";
        case FENG_TOKEN_AMP:
            return "AMP";
        case FENG_TOKEN_PIPE:
            return "PIPE";
        case FENG_TOKEN_CARET:
            return "CARET";
        case FENG_TOKEN_SHL:
            return "SHL";
        case FENG_TOKEN_SHR:
            return "SHR";
        case FENG_TOKEN_ARROW:
            return "ARROW";
        case FENG_TOKEN_TILDE:
            return "TILDE";
    }

    return "UNKNOWN";
}

const char *feng_annotation_kind_name(FengAnnotationKind kind) {
    switch (kind) {
        case FENG_ANNOTATION_NONE:
            return "none";
        case FENG_ANNOTATION_CUSTOM:
            return "custom";
#define FENG_ANNOTATION_NAME(name, text) \
        case FENG_ANNOTATION_##name: \
            return "@" text;
        FENG_BUILTIN_ANNOTATION_LIST(FENG_ANNOTATION_NAME)
#undef FENG_ANNOTATION_NAME
    }

    return "unknown";
}

size_t feng_keyword_count(void) {
    return sizeof(g_keywords) / sizeof(g_keywords[0]);
}

const FengKeywordSpec *feng_keywords(void) {
    return g_keywords;
}

bool feng_lookup_keyword(const char *text, size_t length, FengTokenKind *out_kind) {
    size_t index;

    for (index = 0; index < feng_keyword_count(); ++index) {
        if (g_keywords[index].length == length &&
            memcmp(g_keywords[index].text, text, length) == 0) {
            if (out_kind != NULL) {
                *out_kind = g_keywords[index].kind;
            }
            return true;
        }
    }

    return false;
}

size_t feng_reserved_word_count(void) {
    return sizeof(g_reserved_words) / sizeof(g_reserved_words[0]);
}

const FengReservedWordSpec *feng_reserved_words(void) {
    return g_reserved_words;
}

bool feng_is_reserved_word(const char *text, size_t length) {
    size_t index;

    for (index = 0; index < feng_reserved_word_count(); ++index) {
        if (g_reserved_words[index].length == length &&
            memcmp(g_reserved_words[index].text, text, length) == 0) {
            return true;
        }
    }

    return false;
}

size_t feng_builtin_annotation_count(void) {
    return sizeof(g_builtin_annotations) / sizeof(g_builtin_annotations[0]);
}

const FengAnnotationSpec *feng_builtin_annotations(void) {
    return g_builtin_annotations;
}

bool feng_lookup_builtin_annotation(const char *text, size_t length, FengAnnotationKind *out_kind) {
    size_t index;

    for (index = 0; index < feng_builtin_annotation_count(); ++index) {
        if (g_builtin_annotations[index].length == length &&
            memcmp(g_builtin_annotations[index].text, text, length) == 0) {
            if (out_kind != NULL) {
                *out_kind = g_builtin_annotations[index].kind;
            }
            return true;
        }
    }

    return false;
}

bool feng_token_is_keyword(FengTokenKind kind) {
    return kind >= FENG_TOKEN_KW_TYPE && kind <= FENG_TOKEN_KW_VOID;
}
