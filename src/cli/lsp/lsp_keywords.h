/* lsp_keywords.h - LSP keyword completion data (position-aware).
 *
 * This file declares the keyword sets used by the LSP completion engine.
 * Each FengLspPosition maps to a table of LspKwItem entries; the engine
 * iterates the table without hard-coding any keyword text in C logic.
 *
 * In Phase 1 the `snippet` field is NULL for all items (plain-text
 * completion).  Phase 3 fills `snippet` to enable Snippet templates.
 *
 * The label strings intentionally duplicate token.h FENG_KEYWORD_LIST
 * text: the two tables serve different purposes (lexer recognition vs.
 * LSP completion) and are kept independent on purpose. */

#ifndef FENG_LSP_KEYWORDS_H
#define FENG_LSP_KEYWORDS_H

#include <stddef.h>

/* Grammar position where the cursor sits.  Used to select the keyword
 * subset that makes sense at that location.  OTHER is the zero-init
 * default so that uninitialised structs skip keyword injection. */
typedef enum {
    FENG_LSP_POS_OTHER,    /* unclassified (member access, import path, enum body, ...) */
    FENG_LSP_POS_TOP_DECL, /* module top-level (declaration position) */
    FENG_LSP_POS_TOP_BIND, /* top-level binding (global let/var init expression) */
    FENG_LSP_POS_MEMBER,   /* inside type/spec/fit body (member declaration position) */
    FENG_LSP_POS_BODY      /* inside function/method body (statement position) */
} FengLspPosition;

/* Single keyword completion item. */
typedef struct {
    const char *label;   /* keyword text, e.g. "func" */
    const char *detail;  /* human-readable description, e.g. "function declaration" */
    const char *snippet; /* Snippet template; NULL means plain-text item */
} LspKwItem;

/* Position-keyed keyword table. */
typedef struct {
    const LspKwItem *items;
    size_t count;
} LspKwTable;

/* TOP_DECL: module top-level declaration position (14 items). */
static const LspKwItem TOP_DECL_KWS[] = {
    { "module",        "module declaration",      NULL },
    { "import",        "import declaration",      NULL },
    { "func",          "function declaration",    NULL },
    { "type",          "object type declaration", NULL },
    { "type-tuple",    "tuple type declaration",  NULL },
    { "enum",          "enum declaration",        NULL },
    { "spec",          "spec declaration",        NULL },
    { "spec-callable", "callable spec",           NULL },
    { "spec-union",    "union spec",              NULL },
    { "fit",           "fit declaration",         NULL },
    { "extern",        "external declaration",    NULL },
    { "open",          "visibility modifier",     NULL },
    { "seal",          "visibility modifier",     NULL },
    { "as",            "import alias",            NULL },
};

/* TOP_BIND: top-level binding init expression (17 items). */
static const LspKwItem TOP_BIND_KWS[] = {
    { "let",     "immutable binding",   NULL },
    { "var",     "mutable binding",     NULL },
    { "open",    "visibility modifier", NULL },
    { "seal",    "visibility modifier", NULL },
    { "extern",  "external declaration",NULL },
    { "if",      "conditional",         NULL },
    { "if-else", "conditional+else",    NULL },
    { "else",    "else branch",         NULL },
    { "match",   "pattern matching",    NULL },
    { "while",   "while loop",          NULL },
    { "for",     "for loop",            NULL },
    { "for-in",  "for/in iteration",    NULL },
    { "in",      "for/in keyword",      NULL },
    { "try",     "exception handling",  NULL },
    { "catch",   "exception handler",   NULL },
    { "unknown", "unknown value",       NULL },
    { "void",    "void type",           NULL },
};

/* MEMBER: inside type/spec/fit body, member declaration position (6 items). */
static const LspKwItem MEMBER_KWS[] = {
    { "func",   "method declaration",  NULL },
    { "let",    "immutable field",     NULL },
    { "var",    "mutable field",       NULL },
    { "static", "static modifier",     NULL },
    { "open",   "visibility modifier", NULL },
    { "seal",   "visibility modifier", NULL },
};

/* BODY: inside function/method body, statement position (19 items). */
static const LspKwItem BODY_KWS[] = {
    { "let",      "local immutable binding", NULL },
    { "var",      "local mutable binding",   NULL },
    { "if",       "conditional",             NULL },
    { "if-else",  "conditional+else",        NULL },
    { "else",     "else branch",             NULL },
    { "match",    "pattern matching",        NULL },
    { "while",    "while loop",              NULL },
    { "for",      "for loop",                NULL },
    { "for-in",   "for/in iteration",        NULL },
    { "in",       "for/in keyword",          NULL },
    { "break",    "break loop",              NULL },
    { "continue", "continue loop",           NULL },
    { "return",   "return from function",    NULL },
    { "throw",    "throw exception",         NULL },
    { "try",      "exception handling",      NULL },
    { "catch",    "exception handler",       NULL },
    { "defer",    "deferred execution",      NULL },
    { "unknown",  "unknown value",           NULL },
    { "void",     "void type",               NULL },
};

/* Master table indexed by FengLspPosition. */
static const LspKwTable KW_TABLE[] = {
    [FENG_LSP_POS_OTHER]    = { NULL,           0U },
    [FENG_LSP_POS_TOP_DECL] = { TOP_DECL_KWS,   sizeof(TOP_DECL_KWS)  / sizeof(TOP_DECL_KWS[0])  },
    [FENG_LSP_POS_TOP_BIND] = { TOP_BIND_KWS,   sizeof(TOP_BIND_KWS)  / sizeof(TOP_BIND_KWS[0])  },
    [FENG_LSP_POS_MEMBER]   = { MEMBER_KWS,     sizeof(MEMBER_KWS)    / sizeof(MEMBER_KWS[0])    },
    [FENG_LSP_POS_BODY]     = { BODY_KWS,       sizeof(BODY_KWS)      / sizeof(BODY_KWS[0])      },
};

#endif /* FENG_LSP_KEYWORDS_H */
