#ifndef FENG_LEXER_LEXER_H
#define FENG_LEXER_LEXER_H

#include <stddef.h>

#include "lexer/token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengLexer {
    const char *source;
    size_t length;
    const char *path;
    size_t current;
    unsigned int line;
    unsigned int column;
    const char *last_error;
    const char *pending_doc;
    size_t pending_doc_length;
    unsigned int pending_doc_line_breaks;
    int has_peeked;
    FengToken peeked;
} FengLexer;

void feng_lexer_init(FengLexer *lexer, const char *source, size_t length, const char *path);
void feng_lexer_reset(FengLexer *lexer);
FengToken feng_lexer_next(FengLexer *lexer);
FengToken feng_lexer_peek(FengLexer *lexer);
const char *feng_lexer_last_error(const FengLexer *lexer);
const char *feng_lexer_path(const FengLexer *lexer);

#ifdef __cplusplus
}
#endif

#endif
