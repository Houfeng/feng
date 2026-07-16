#include "cli/lsp/document_store.h"

#include <stdlib.h>
#include <string.h>

/* Appends one offset to a candidate line-index allocation. */
static bool line_index_append(FengLspLineIndex *index, size_t offset) {
    size_t *grown;
    size_t capacity;

    if (index->count == index->capacity) {
        capacity = index->capacity == 0U ? 128U : index->capacity * 2U;
        grown = (size_t *)realloc(index->offsets, capacity * sizeof(index->offsets[0]));
        if (grown == NULL) {
            return false;
        }
        index->offsets = grown;
        index->capacity = capacity;
    }
    index->offsets[index->count++] = offset;
    return true;
}

/* Decodes enough UTF-8 to determine its UTF-16 code-unit width. */
static size_t utf8_codepoint_width(const char *text,
                                   size_t offset,
                                   unsigned int *out_utf16_units) {
    unsigned char lead = (unsigned char)text[offset];
    size_t bytes = 1U;
    unsigned int codepoint = lead;

    if ((lead & 0xE0U) == 0xC0U && text[offset + 1U] != '\0') {
        bytes = 2U;
        codepoint = ((unsigned int)(lead & 0x1FU) << 6U) |
                    ((unsigned int)text[offset + 1U] & 0x3FU);
    } else if ((lead & 0xF0U) == 0xE0U && text[offset + 1U] != '\0' &&
               text[offset + 2U] != '\0') {
        bytes = 3U;
        codepoint = ((unsigned int)(lead & 0x0FU) << 12U) |
                    (((unsigned int)text[offset + 1U] & 0x3FU) << 6U) |
                    ((unsigned int)text[offset + 2U] & 0x3FU);
    } else if ((lead & 0xF8U) == 0xF0U && text[offset + 1U] != '\0' &&
               text[offset + 2U] != '\0' && text[offset + 3U] != '\0') {
        bytes = 4U;
        codepoint = ((unsigned int)(lead & 0x07U) << 18U) |
                    (((unsigned int)text[offset + 1U] & 0x3FU) << 12U) |
                    (((unsigned int)text[offset + 2U] & 0x3FU) << 6U) |
                    ((unsigned int)text[offset + 3U] & 0x3FU);
    }
    *out_utf16_units = codepoint > 0xFFFFU ? 2U : 1U;
    return bytes;
}

bool feng_lsp_line_index_rebuild(FengLspLineIndex *index, const char *text) {
    FengLspLineIndex candidate = {0};
    size_t offset;

    if (index == NULL || text == NULL || !line_index_append(&candidate, 0U)) {
        feng_lsp_line_index_dispose(&candidate);
        return false;
    }
    for (offset = 0U; text[offset] != '\0'; ++offset) {
        if (text[offset] == '\n' && !line_index_append(&candidate, offset + 1U)) {
            feng_lsp_line_index_dispose(&candidate);
            return false;
        }
    }
    feng_lsp_line_index_dispose(index);
    *index = candidate;
    return true;
}

void feng_lsp_line_index_dispose(FengLspLineIndex *index) {
    if (index == NULL) {
        return;
    }
    free(index->offsets);
    memset(index, 0, sizeof(*index));
}

size_t feng_lsp_line_index_offset(const FengLspLineIndex *index,
                                  const char *text,
                                  unsigned int line,
                                  unsigned int character) {
    size_t offset;
    unsigned int utf16_units = 0U;

    if (index == NULL || text == NULL || index->count == 0U) {
        return 0U;
    }
    offset = line < index->count ? index->offsets[line] : strlen(text);
    while (text[offset] != '\0' && text[offset] != '\n' && utf16_units < character) {
        unsigned int codepoint_units;
        size_t bytes = utf8_codepoint_width(text, offset, &codepoint_units);

        if (utf16_units + codepoint_units > character) {
            break;
        }
        utf16_units += codepoint_units;
        offset += bytes;
    }
    return offset;
}
