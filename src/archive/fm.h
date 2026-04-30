#ifndef FENG_ARCHIVE_FM_H
#define FENG_ARCHIVE_FM_H

#include <stdbool.h>
#include <stddef.h>

typedef struct FengFmError {
    char *path;
    char *message;
    unsigned int line;
} FengFmError;

typedef struct FengFmSection {
    char *name;
    unsigned int line;
} FengFmSection;

typedef struct FengFmEntry {
    const char *section;
    char *key;
    char *value;
    unsigned int line;
} FengFmEntry;

typedef struct FengFmDocument {
    FengFmSection *sections;
    size_t section_count;
    FengFmEntry *entries;
    size_t entry_count;
} FengFmDocument;

bool feng_fm_parse(const char *manifest_path,
                   const char *source,
                   FengFmDocument *out_document,
                   FengFmError *out_error);

void feng_fm_document_dispose(FengFmDocument *document);
void feng_fm_error_dispose(FengFmError *error);

#endif /* FENG_ARCHIVE_FM_H */
