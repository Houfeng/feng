#include "archive/fm.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *dup_n(const char *text, size_t length) {
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}


static char *dup_cstr(const char *text) {
    return dup_n(text, strlen(text));
}

static void set_error(FengFmError *error,
                      const char *path,
                      unsigned int line,
                      const char *message) {
    if (error == NULL) {
        return;
    }
    feng_fm_error_dispose(error);
    error->path = path != NULL ? dup_cstr(path) : NULL;
    error->message = message != NULL ? dup_cstr(message) : NULL;
    error->line = line;
}

static void trim_slice(const char **start, const char **end) {
    while (*start < *end && isspace((unsigned char)**start)) {
        (*start)++;
    }
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) {
        (*end)--;
    }
}

static bool slice_equals_cstr(const char *start, const char *end, const char *text) {
    size_t length = (size_t)(end - start);

    return strlen(text) == length && strncmp(start, text, length) == 0;
}

static bool is_identifier_char(char ch) {
    unsigned char value = (unsigned char)ch;

    return isalnum(value) || ch == '_' || ch == '-' || ch == '.';
}

static bool is_valid_identifier(const char *start, const char *end) {
    const char *cursor = start;

    if (start == end) {
        return false;
    }
    while (cursor < end) {
        if (!is_identifier_char(*cursor)) {
            return false;
        }
        cursor++;
    }
    return true;
}

static bool has_section(const FengFmDocument *document,
                        const char *start,
                        const char *end) {
    size_t index;

    for (index = 0U; index < document->section_count; ++index) {
        if (slice_equals_cstr(start, end, document->sections[index].name)) {
            return true;
        }
    }
    return false;
}

static bool has_entry(const FengFmDocument *document,
                      const char *section,
                      const char *key_start,
                      const char *key_end) {
    size_t index;

    for (index = 0U; index < document->entry_count; ++index) {
        if (strcmp(document->entries[index].section, section) == 0
            && slice_equals_cstr(key_start, key_end, document->entries[index].key)) {
            return true;
        }
    }
    return false;
}

static bool add_section(FengFmDocument *document,
                        const char *start,
                        const char *end,
                        const char **out_section,
                        const char *manifest_path,
                        unsigned int line,
                        FengFmError *error) {
    FengFmSection *sections;
    char *name;

    if (has_section(document, start, end)) {
        set_error(error, manifest_path, line, "duplicate manifest section");
        return false;
    }

    name = dup_n(start, (size_t)(end - start));
    if (name == NULL) {
        set_error(error, manifest_path, line, "out of memory");
        return false;
    }

    sections = (FengFmSection *)realloc(document->sections,
                                        (document->section_count + 1U) * sizeof(FengFmSection));
    if (sections == NULL) {
        free(name);
        set_error(error, manifest_path, line, "out of memory");
        return false;
    }

    document->sections = sections;
    document->sections[document->section_count].name = name;
    document->sections[document->section_count].line = line;
    *out_section = document->sections[document->section_count].name;
    document->section_count++;
    return true;
}

static bool add_entry(FengFmDocument *document,
                      const char *section,
                      const char *key_start,
                      const char *key_end,
                      const char *value_start,
                      const char *value_end,
                      const char *manifest_path,
                      unsigned int line,
                      FengFmError *error) {
    FengFmEntry *entries;
    char *key;
    char *value;

    if (has_entry(document, section, key_start, key_end)) {
        set_error(error, manifest_path, line, "duplicate manifest field");
        return false;
    }

    key = dup_n(key_start, (size_t)(key_end - key_start));
    if (key == NULL) {
        set_error(error, manifest_path, line, "out of memory");
        return false;
    }

    value = dup_n(value_start, (size_t)(value_end - value_start));
    if (value == NULL) {
        free(key);
        set_error(error, manifest_path, line, "out of memory");
        return false;
    }

    entries = (FengFmEntry *)realloc(document->entries,
                                     (document->entry_count + 1U) * sizeof(FengFmEntry));
    if (entries == NULL) {
        free(value);
        free(key);
        set_error(error, manifest_path, line, "out of memory");
        return false;
    }

    document->entries = entries;
    document->entries[document->entry_count].section = section;
    document->entries[document->entry_count].key = key;
    document->entries[document->entry_count].value = value;
    document->entries[document->entry_count].line = line;
    document->entry_count++;
    return true;
}

bool feng_fm_parse(const char *manifest_path,
                   const char *source,
                   FengFmDocument *out_document,
                   FengFmError *out_error) {
    const char *cursor = source;
    unsigned int line = 1U;
    const char *current_section = NULL;
    FengFmDocument document = {0};

    if (out_document == NULL || source == NULL) {
        set_error(out_error, manifest_path, 0U, "invalid manifest parse request");
        return false;
    }

    while (*cursor != '\0') {
        const char *line_start = cursor;
        const char *line_end = cursor;
        const char *trimmed_start;
        const char *trimmed_end;

        while (*line_end != '\0' && *line_end != '\n') {
            line_end++;
        }
        cursor = *line_end == '\n' ? line_end + 1 : line_end;

        trimmed_start = line_start;
        trimmed_end = line_end;
        trim_slice(&trimmed_start, &trimmed_end);
        if (trimmed_start == trimmed_end || *trimmed_start == '#') {
            if (*line_end == '\n') {
                line++;
            }
            continue;
        }

        if (*trimmed_start == '[') {
            const char *section_start;
            const char *section_end;

            if (trimmed_end <= trimmed_start + 1 || *(trimmed_end - 1) != ']') {
                set_error(out_error, manifest_path, line, "invalid manifest section header");
                goto fail;
            }
            section_start = trimmed_start + 1;
            section_end = trimmed_end - 1;
            trim_slice(&section_start, &section_end);
            if (!is_valid_identifier(section_start, section_end)) {
                set_error(out_error,
                          manifest_path,
                          line,
                          "manifest section name must use letters, digits, `.`, `_`, or `-`");
                goto fail;
            }
            if (!add_section(&document,
                             section_start,
                             section_end,
                             &current_section,
                             manifest_path,
                             line,
                             out_error)) {
                goto fail;
            }
        } else {
            const char *key_start;
            const char *key_end;
            const char *value_start;
            const char *value_end;
            const char *colon;
            const char *closing_quote;

            if (current_section == NULL) {
                set_error(out_error, manifest_path, line, "manifest field must appear inside a section");
                goto fail;
            }

            colon = trimmed_start;
            while (colon < trimmed_end && *colon != ':') {
                colon++;
            }
            if (colon == trimmed_end) {
                set_error(out_error, manifest_path, line, "manifest line must be `key: \"value\"`");
                goto fail;
            }

            key_start = trimmed_start;
            key_end = colon;
            trim_slice(&key_start, &key_end);
            if (!is_valid_identifier(key_start, key_end)) {
                set_error(out_error,
                          manifest_path,
                          line,
                          "manifest field name must use letters, digits, `.`, `_`, or `-`");
                goto fail;
            }

            value_start = colon + 1;
            value_end = trimmed_end;
            trim_slice(&value_start, &value_end);
            if ((size_t)(value_end - value_start) < 2U || *value_start != '"') {
                set_error(out_error, manifest_path, line, "manifest value must be a double-quoted string");
                goto fail;
            }

            closing_quote = value_start + 1;
            while (closing_quote < value_end && *closing_quote != '"') {
                closing_quote++;
            }
            if (closing_quote == value_end || closing_quote + 1 != value_end) {
                set_error(out_error, manifest_path, line, "manifest value must be a double-quoted string");
                goto fail;
            }

            if (!add_entry(&document,
                           current_section,
                           key_start,
                           key_end,
                           value_start + 1,
                           closing_quote,
                           manifest_path,
                           line,
                           out_error)) {
                goto fail;
            }
        }

        if (*line_end == '\n') {
            line++;
        }
    }

    *out_document = document;
    return true;

fail:
    feng_fm_document_dispose(&document);
    return false;
}

void feng_fm_document_dispose(FengFmDocument *document) {
    size_t index;

    if (document == NULL) {
        return;
    }
    for (index = 0U; index < document->section_count; ++index) {
        free(document->sections[index].name);
    }
    for (index = 0U; index < document->entry_count; ++index) {
        free(document->entries[index].key);
        free(document->entries[index].value);
    }
    free(document->sections);
    free(document->entries);
    document->sections = NULL;
    document->entries = NULL;
    document->section_count = 0U;
    document->entry_count = 0U;
}

void feng_fm_error_dispose(FengFmError *error) {
    if (error == NULL) {
        return;
    }
    free(error->path);
    free(error->message);
    error->path = NULL;
    error->message = NULL;
    error->line = 0U;
}
