#ifndef FENG_CLI_LSP_DOCUMENT_STORE_H
#define FENG_CLI_LSP_DOCUMENT_STORE_H

#include <stdbool.h>
#include <stddef.h>

/* Line-start offsets for exactly one current document text. */
typedef struct FengLspLineIndex {
    size_t *offsets;
    size_t count;
    size_t capacity;
} FengLspLineIndex;

/* Builds a replacement line index and preserves the old index on failure. */
bool feng_lsp_line_index_rebuild(FengLspLineIndex *index, const char *text);

/* Releases all line offsets owned by an index. */
void feng_lsp_line_index_dispose(FengLspLineIndex *index);

/* Converts an LSP UTF-16 position to a byte offset in indexed current text. */
size_t feng_lsp_line_index_offset(const FengLspLineIndex *index,
                                  const char *text,
                                  unsigned int line,
                                  unsigned int character);

#endif /* FENG_CLI_LSP_DOCUMENT_STORE_H */
