#include "cli/lsp/runtime.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli/common.h"
#include "cli/deps/manager.h"
#include "cli/frontend.h"
#include "cli/project/common.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/provider.h"
#include "symbol/imported_module.h"

typedef enum FengLspParseStatus {
    FENG_LSP_PARSE_OK = 0,
    FENG_LSP_PARSE_INVALID_JSON,
    FENG_LSP_PARSE_INVALID_REQUEST
} FengLspParseStatus;

typedef enum FengLspJsonType {
    FENG_LSP_JSON_INVALID = 0,
    FENG_LSP_JSON_OBJECT,
    FENG_LSP_JSON_ARRAY,
    FENG_LSP_JSON_STRING,
    FENG_LSP_JSON_NUMBER,
    FENG_LSP_JSON_BOOL,
    FENG_LSP_JSON_NULL
} FengLspJsonType;

typedef struct FengLspJsonValue {
    FengLspJsonType type;
    const char *start;
    const char *end;
    const char *value_start;
    const char *value_end;
} FengLspJsonValue;

typedef struct FengLspMessage {
    char *method;
    FengLspJsonValue id;
    FengLspJsonValue params;
    bool has_id;
} FengLspMessage;

typedef struct FengLspString {
    char *data;
    size_t length;
    size_t capacity;
} FengLspString;

typedef struct FengLspDocument {
    char *uri;
    char *path;
    char *text;
    bool is_file;
} FengLspDocument;

typedef struct FengLspDiagnosticEntry {
    char *path;
    char *message;
    const char *source;
    unsigned int line;
    unsigned int column;
    unsigned int end_column;
    int severity;
} FengLspDiagnosticEntry;

typedef struct FengLspDiagnosticCollector {
    FengLspDiagnosticEntry *items;
    size_t count;
    size_t capacity;
} FengLspDiagnosticCollector;

typedef struct FengLspAnalysisSession {
    FengLspDiagnosticCollector diagnostics;
    FengSemanticAnalysis *analysis;
    FengCliLoadedSource *sources;
    size_t source_count;
    char **bundle_paths;
    size_t bundle_count;
    /* Owned copies of source file paths for project sessions. The sources[]
     * array borrows path pointers from the project context, which is disposed
     * before the session is used. We steal source_paths from the context (set
     * context.source_paths = NULL before dispose) so they outlive the context.
     * session_dispose() frees these strings and the array. */
    char **owned_source_paths;
    size_t owned_source_path_count;
    char *manifest_path;
    bool is_project;
    int exit_code;
    /* Keeps the imported-module cache alive for the entire session lifetime so
     * that analysis->modules entries for external package types remain valid.
     * Without this, the cache is freed at the end of frontend_run_with_overlays
     * and every external-package module pointer becomes dangling. */
    FengSymbolImportedModuleCache *imported_module_cache;
} FengLspAnalysisSession;

typedef enum FengLspLocalKind {
    FENG_LSP_LOCAL_PARAM = 0,
    FENG_LSP_LOCAL_BINDING,
    FENG_LSP_LOCAL_SELF
} FengLspLocalKind;

typedef struct FengLspLocal {
    FengLspLocalKind kind;
    FengSlice name;
    const FengParameter *parameter;
    const FengBinding *binding;
    const FengDecl *self_owner_decl;
} FengLspLocal;

typedef struct FengLspLocalList {
    FengLspLocal *items;
    size_t count;
    size_t capacity;
} FengLspLocalList;

typedef enum FengLspResolvedKind {
    FENG_LSP_RESOLVED_NONE = 0,
    FENG_LSP_RESOLVED_DECL,
    FENG_LSP_RESOLVED_MEMBER,
    FENG_LSP_RESOLVED_PARAM,
    FENG_LSP_RESOLVED_BINDING,
    FENG_LSP_RESOLVED_SELF
} FengLspResolvedKind;

typedef struct FengLspResolvedTarget {
    FengLspResolvedKind kind;
    const FengDecl *decl;
    const FengTypeMember *member;
    const FengParameter *parameter;
    const FengBinding *binding;
    const FengDecl *self_owner_decl;
} FengLspResolvedTarget;

typedef struct FengLspCacheResolvedTarget {
    FengLspResolvedKind kind;
    const FengSymbolDeclView *decl;
    const FengSymbolDeclView *member;
    const FengParameter *parameter;
    const FengBinding *binding;
    const FengSymbolDeclView *self_owner_decl;
} FengLspCacheResolvedTarget;

typedef struct FengLspCacheQueryContext {
    FengProgram *program;
    FengSymbolProvider *provider;
    const FengSymbolImportedModule *current_module;
    const char *source_text;
} FengLspCacheQueryContext;

typedef struct FengLspReferenceEntry {
    const char *path;
    size_t start_offset;
    size_t end_offset;
} FengLspReferenceEntry;

typedef struct FengLspReferenceList {
    FengLspReferenceEntry *items;
    size_t count;
    size_t capacity;
} FengLspReferenceList;

typedef enum FengLspMarkupKind {
    FENG_LSP_MARKUP_PLAINTEXT = 0,
    FENG_LSP_MARKUP_MARKDOWN
} FengLspMarkupKind;

struct FengLspRuntime {
    FengLspDocument *documents;
    size_t document_count;
    size_t document_capacity;
    bool shutdown_requested;
    bool should_exit;
    int exit_code;
    FengLspMarkupKind hover_markup_kind;
    FILE *errors; /* diagnostics log; set at the start of each handle_payload call */
};

/* URI of the document currently being completed.  Set at the beginning of
 * handle_completion_request and cleared when the request finishes.  Read
 * by completion item builders to emit the data payload for resolve. */
static const char *g_completion_uri = NULL;

static bool append_raw(void **items,
                       size_t *count,
                       size_t *capacity,
                       size_t item_size,
                       const void *value) {
    void *grown;
    size_t new_capacity;

    if (*count == *capacity) {
        new_capacity = *capacity == 0U ? 8U : (*capacity * 2U);
        grown = realloc(*items, new_capacity * item_size);
        if (grown == NULL) {
            return false;
        }
        *items = grown;
        *capacity = new_capacity;
    }
    memcpy((char *)(*items) + (*count * item_size), value, item_size);
    ++(*count);
    return true;
}

static char *dup_range(const char *start, const char *end) {
    size_t length = (size_t)(end - start);
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, start, length);
    out[length] = '\0';
    return out;
}

static char *dup_cstr(const char *text) {
    return text != NULL ? dup_range(text, text + strlen(text)) : NULL;
}

static void string_dispose(FengLspString *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

static bool string_reserve(FengLspString *buffer, size_t extra) {
    char *grown;
    size_t need = buffer->length + extra + 1U;
    size_t capacity = buffer->capacity == 0U ? 128U : buffer->capacity;

    if (need <= buffer->capacity) {
        return true;
    }
    while (capacity < need) {
        capacity *= 2U;
    }
    grown = (char *)realloc(buffer->data, capacity);
    if (grown == NULL) {
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static bool string_append_bytes(FengLspString *buffer, const char *text, size_t length) {
    if (!string_reserve(buffer, length)) {
        return false;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool string_append_cstr(FengLspString *buffer, const char *text) {
    return text == NULL ? true : string_append_bytes(buffer, text, strlen(text));
}

static bool string_append_format(FengLspString *buffer, const char *fmt, ...) {
    va_list args;
    va_list copy;
    int needed;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0 || !string_reserve(buffer, (size_t)needed)) {
        va_end(copy);
        return false;
    }
    vsnprintf(buffer->data + buffer->length,
              buffer->capacity - buffer->length,
              fmt,
              copy);
    buffer->length += (size_t)needed;
    va_end(copy);
    return true;
}

static bool string_append_json_string(FengLspString *buffer, const char *text) {
    const unsigned char *cursor;

    if (!string_append_cstr(buffer, "\"")) {
        return false;
    }
    if (text != NULL) {
        for (cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
            switch (*cursor) {
                case '"':
                    if (!string_append_cstr(buffer, "\\\"")) {
                        return false;
                    }
                    break;
                case '\\':
                    if (!string_append_cstr(buffer, "\\\\")) {
                        return false;
                    }
                    break;
                case '\b':
                    if (!string_append_cstr(buffer, "\\b")) {
                        return false;
                    }
                    break;
                case '\f':
                    if (!string_append_cstr(buffer, "\\f")) {
                        return false;
                    }
                    break;
                case '\n':
                    if (!string_append_cstr(buffer, "\\n")) {
                        return false;
                    }
                    break;
                case '\r':
                    if (!string_append_cstr(buffer, "\\r")) {
                        return false;
                    }
                    break;
                case '\t':
                    if (!string_append_cstr(buffer, "\\t")) {
                        return false;
                    }
                    break;
                default:
                    if (*cursor < 0x20U) {
                        if (!string_append_format(buffer, "\\u%04x", (unsigned int)*cursor)) {
                            return false;
                        }
                    } else if (!string_append_bytes(buffer, (const char *)cursor, 1U)) {
                        return false;
                    }
                    break;
            }
        }
    }
    return string_append_cstr(buffer, "\"");
}

static void skip_ws(const char **cursor, const char *end) {
    while (*cursor < end && isspace((unsigned char)**cursor)) {
        ++(*cursor);
    }
}

static bool scan_json_string(const char **cursor,
                             const char *end,
                             const char **out_start,
                             const char **out_end) {
    const char *it;

    if (*cursor >= end || **cursor != '"') {
        return false;
    }
    it = *cursor + 1;
    while (it < end) {
        unsigned char ch = (unsigned char)*it;

        if (ch == '"') {
            *out_start = *cursor + 1;
            *out_end = it;
            *cursor = it + 1;
            return true;
        }
        if (ch == '\\') {
            ++it;
            if (it >= end) {
                return false;
            }
            if (*it == 'u') {
                size_t digit_index;

                for (digit_index = 0U; digit_index < 4U; ++digit_index) {
                    ++it;
                    if (it >= end || !isxdigit((unsigned char)*it)) {
                        return false;
                    }
                }
            }
            ++it;
            continue;
        }
        if (ch < 0x20U) {
            return false;
        }
        ++it;
    }
    return false;
}

static bool skip_json_number(const char **cursor, const char *end) {
    const char *it = *cursor;

    if (it < end && *it == '-') {
        ++it;
    }
    if (it >= end) {
        return false;
    }
    if (*it == '0') {
        ++it;
    } else {
        if (!isdigit((unsigned char)*it)) {
            return false;
        }
        while (it < end && isdigit((unsigned char)*it)) {
            ++it;
        }
    }
    if (it < end && *it == '.') {
        ++it;
        if (it >= end || !isdigit((unsigned char)*it)) {
            return false;
        }
        while (it < end && isdigit((unsigned char)*it)) {
            ++it;
        }
    }
    if (it < end && (*it == 'e' || *it == 'E')) {
        ++it;
        if (it < end && (*it == '+' || *it == '-')) {
            ++it;
        }
        if (it >= end || !isdigit((unsigned char)*it)) {
            return false;
        }
        while (it < end && isdigit((unsigned char)*it)) {
            ++it;
        }
    }
    *cursor = it;
    return true;
}

static bool skip_json_value(const char **cursor, const char *end);

static bool skip_json_object(const char **cursor, const char *end) {
    const char *key_start;
    const char *key_end;

    if (*cursor >= end || **cursor != '{') {
        return false;
    }
    ++(*cursor);
    skip_ws(cursor, end);
    if (*cursor < end && **cursor == '}') {
        ++(*cursor);
        return true;
    }
    while (*cursor < end) {
        skip_ws(cursor, end);
        if (!scan_json_string(cursor, end, &key_start, &key_end)) {
            return false;
        }
        (void)key_start;
        (void)key_end;
        skip_ws(cursor, end);
        if (*cursor >= end || **cursor != ':') {
            return false;
        }
        ++(*cursor);
        if (!skip_json_value(cursor, end)) {
            return false;
        }
        skip_ws(cursor, end);
        if (*cursor < end && **cursor == ',') {
            ++(*cursor);
            continue;
        }
        if (*cursor < end && **cursor == '}') {
            ++(*cursor);
            return true;
        }
        return false;
    }
    return false;
}

static bool skip_json_array(const char **cursor, const char *end) {
    if (*cursor >= end || **cursor != '[') {
        return false;
    }
    ++(*cursor);
    skip_ws(cursor, end);
    if (*cursor < end && **cursor == ']') {
        ++(*cursor);
        return true;
    }
    while (*cursor < end) {
        if (!skip_json_value(cursor, end)) {
            return false;
        }
        skip_ws(cursor, end);
        if (*cursor < end && **cursor == ',') {
            ++(*cursor);
            continue;
        }
        if (*cursor < end && **cursor == ']') {
            ++(*cursor);
            return true;
        }
        return false;
    }
    return false;
}

static bool skip_json_literal(const char **cursor, const char *end, const char *literal) {
    size_t length = strlen(literal);

    if ((size_t)(end - *cursor) < length) {
        return false;
    }
    if (memcmp(*cursor, literal, length) != 0) {
        return false;
    }
    *cursor += length;
    return true;
}

static bool skip_json_value(const char **cursor, const char *end) {
    skip_ws(cursor, end);
    if (*cursor >= end) {
        return false;
    }
    switch (**cursor) {
        case '{':
            return skip_json_object(cursor, end);
        case '[':
            return skip_json_array(cursor, end);
        case '"': {
            const char *text_start;
            const char *text_end;

            return scan_json_string(cursor, end, &text_start, &text_end);
        }
        case 't':
            return skip_json_literal(cursor, end, "true");
        case 'f':
            return skip_json_literal(cursor, end, "false");
        case 'n':
            return skip_json_literal(cursor, end, "null");
        default:
            return skip_json_number(cursor, end);
    }
}

static bool json_parse_value(const char **cursor,
                             const char *end,
                             FengLspJsonValue *out_value) {
    const char *start;

    skip_ws(cursor, end);
    if (*cursor >= end) {
        return false;
    }
    start = *cursor;
    memset(out_value, 0, sizeof(*out_value));
    switch (**cursor) {
        case '{':
            if (!skip_json_object(cursor, end)) {
                return false;
            }
            out_value->type = FENG_LSP_JSON_OBJECT;
            break;
        case '[':
            if (!skip_json_array(cursor, end)) {
                return false;
            }
            out_value->type = FENG_LSP_JSON_ARRAY;
            break;
        case '"':
            if (!scan_json_string(cursor, end, &out_value->value_start, &out_value->value_end)) {
                return false;
            }
            out_value->type = FENG_LSP_JSON_STRING;
            break;
        case 't':
        case 'f':
            if (!skip_json_literal(cursor, end, **cursor == 't' ? "true" : "false")) {
                return false;
            }
            out_value->type = FENG_LSP_JSON_BOOL;
            break;
        case 'n':
            if (!skip_json_literal(cursor, end, "null")) {
                return false;
            }
            out_value->type = FENG_LSP_JSON_NULL;
            break;
        default:
            if (!skip_json_number(cursor, end)) {
                return false;
            }
            out_value->type = FENG_LSP_JSON_NUMBER;
            break;
    }
    out_value->start = start;
    out_value->end = *cursor;
    if (out_value->value_start == NULL) {
        out_value->value_start = start;
        out_value->value_end = *cursor;
    }
    return true;
}

static bool json_key_equals(const char *start, const char *end, const char *text) {
    size_t length = (size_t)(end - start);

    return strlen(text) == length && memcmp(start, text, length) == 0;
}

static bool json_object_get(FengLspJsonValue object,
                            const char *key,
                            FengLspJsonValue *out_value) {
    const char *cursor;
    const char *end;

    if (object.type != FENG_LSP_JSON_OBJECT) {
        return false;
    }
    cursor = object.start + 1;
    end = object.end - 1;
    skip_ws(&cursor, end);
    while (cursor < end && *cursor != '}') {
        const char *key_start;
        const char *key_end;
        FengLspJsonValue value;

        if (!scan_json_string(&cursor, end, &key_start, &key_end)) {
            return false;
        }
        skip_ws(&cursor, end);
        if (cursor >= end || *cursor != ':') {
            return false;
        }
        ++cursor;
        if (!json_parse_value(&cursor, end, &value)) {
            return false;
        }
        if (json_key_equals(key_start, key_end, key)) {
            *out_value = value;
            return true;
        }
        skip_ws(&cursor, end);
        if (cursor < end && *cursor == ',') {
            ++cursor;
            skip_ws(&cursor, end);
        }
    }
    return false;
}

static bool json_array_get(FengLspJsonValue array,
                           size_t index,
                           FengLspJsonValue *out_value) {
    const char *cursor;
    const char *end;
    size_t current = 0U;

    if (array.type != FENG_LSP_JSON_ARRAY) {
        return false;
    }
    cursor = array.start + 1;
    end = array.end - 1;
    skip_ws(&cursor, end);
    while (cursor < end && *cursor != ']') {
        FengLspJsonValue value;

        if (!json_parse_value(&cursor, end, &value)) {
            return false;
        }
        if (current == index) {
            *out_value = value;
            return true;
        }
        ++current;
        skip_ws(&cursor, end);
        if (cursor < end && *cursor == ',') {
            ++cursor;
            skip_ws(&cursor, end);
        }
    }
    return false;
}

static bool json_string_equals(FengLspJsonValue value, const char *text) {
    size_t text_length;

    if (value.type != FENG_LSP_JSON_STRING || text == NULL) {
        return false;
    }
    text_length = strlen(text);
    return (size_t)(value.value_end - value.value_start) == text_length &&
           memcmp(value.value_start, text, text_length) == 0;
}

static bool json_hex_digit(char ch, unsigned int *out) {
    if (ch >= '0' && ch <= '9') {
        *out = (unsigned int)(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        *out = (unsigned int)(ch - 'a') + 10U;
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        *out = (unsigned int)(ch - 'A') + 10U;
        return true;
    }
    return false;
}

/* Encode a Unicode code point as UTF-8 into buf (max 4 bytes). Returns byte count or 0 on error. */
static size_t json_unicode_to_utf8(unsigned long cp, char buf[4]) {
    if (cp <= 0x7FUL) {
        buf[0] = (char)(unsigned char)cp;
        return 1U;
    }
    if (cp <= 0x7FFUL) {
        buf[0] = (char)(unsigned char)(0xC0U | (cp >> 6U));
        buf[1] = (char)(unsigned char)(0x80U | (cp & 0x3FU));
        return 2U;
    }
    if (cp <= 0xFFFFUL) {
        buf[0] = (char)(unsigned char)(0xE0U | (cp >> 12U));
        buf[1] = (char)(unsigned char)(0x80U | ((cp >> 6U) & 0x3FU));
        buf[2] = (char)(unsigned char)(0x80U | (cp & 0x3FU));
        return 3U;
    }
    if (cp <= 0x10FFFFUL) {
        buf[0] = (char)(unsigned char)(0xF0U | (cp >> 18U));
        buf[1] = (char)(unsigned char)(0x80U | ((cp >> 12U) & 0x3FU));
        buf[2] = (char)(unsigned char)(0x80U | ((cp >> 6U) & 0x3FU));
        buf[3] = (char)(unsigned char)(0x80U | (cp & 0x3FU));
        return 4U;
    }
    return 0U;
}

static char *json_string_dup(FengLspJsonValue value) {
    const char *cursor;
    FengLspString out = {0};

    if (value.type != FENG_LSP_JSON_STRING) {
        return NULL;
    }
    for (cursor = value.value_start; cursor < value.value_end; ++cursor) {
        if (*cursor != '\\') {
            if (!string_append_bytes(&out, cursor, 1U)) {
                string_dispose(&out);
                return NULL;
            }
            continue;
        }
        ++cursor;
        if (cursor >= value.value_end) {
            string_dispose(&out);
            return NULL;
        }
        switch (*cursor) {
            case '"':
            case '\\':
            case '/':
                if (!string_append_bytes(&out, cursor, 1U)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            case 'b':
                if (!string_append_bytes(&out, "\b", 1U)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            case 'f':
                if (!string_append_bytes(&out, "\f", 1U)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            case 'n':
                if (!string_append_bytes(&out, "\n", 1U)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            case 'r':
                if (!string_append_bytes(&out, "\r", 1U)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            case 't':
                if (!string_append_bytes(&out, "\t", 1U)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            case 'u': {
                /* \uXXXX — decode 4 hex digits and encode as UTF-8. */
                unsigned int d0, d1, d2, d3;
                unsigned long cp;
                char utf8[4];
                size_t utf8_len;

                if (cursor + 4 >= value.value_end) {
                    string_dispose(&out);
                    return NULL;
                }
                if (!json_hex_digit(cursor[1], &d0) ||
                    !json_hex_digit(cursor[2], &d1) ||
                    !json_hex_digit(cursor[3], &d2) ||
                    !json_hex_digit(cursor[4], &d3)) {
                    string_dispose(&out);
                    return NULL;
                }
                cp = ((unsigned long)d0 << 12U) |
                     ((unsigned long)d1 << 8U)  |
                     ((unsigned long)d2 << 4U)  |
                      (unsigned long)d3;
                /* Handle UTF-16 surrogate pairs (\uD800-\uDBFF followed by \uDC00-\uDFFF). */
                if (cp >= 0xD800UL && cp <= 0xDBFFUL) {
                    unsigned int e0, e1, e2, e3;
                    unsigned long low;

                    if (cursor + 10 >= value.value_end ||
                        cursor[5] != '\\' || cursor[6] != 'u') {
                        string_dispose(&out);
                        return NULL;
                    }
                    if (!json_hex_digit(cursor[7], &e0) ||
                        !json_hex_digit(cursor[8], &e1) ||
                        !json_hex_digit(cursor[9], &e2) ||
                        !json_hex_digit(cursor[10], &e3)) {
                        string_dispose(&out);
                        return NULL;
                    }
                    low = ((unsigned long)e0 << 12U) |
                          ((unsigned long)e1 << 8U)  |
                          ((unsigned long)e2 << 4U)  |
                           (unsigned long)e3;
                    if (low < 0xDC00UL || low > 0xDFFFUL) {
                        string_dispose(&out);
                        return NULL;
                    }
                    cp = 0x10000UL + ((cp - 0xD800UL) << 10U) + (low - 0xDC00UL);
                    cursor += 10; /* skip: 4 hex + \u + 4 hex */
                } else {
                    cursor += 4; /* skip the 4 hex digits */
                }
                utf8_len = json_unicode_to_utf8(cp, utf8);
                if (utf8_len == 0U || !string_append_bytes(&out, utf8, utf8_len)) {
                    string_dispose(&out);
                    return NULL;
                }
                break;
            }
            default:
                string_dispose(&out);
                return NULL;
        }
    }
    return out.data;
}

static FengLspParseStatus parse_jsonrpc_message(const char *payload,
                                                size_t payload_length,
                                                FengLspMessage *out_message) {
    FengLspJsonValue root = {0};
    FengLspJsonValue method = {0};
    const char *cursor = payload;
    const char *end = payload + payload_length;

    memset(out_message, 0, sizeof(*out_message));
    if (!json_parse_value(&cursor, end, &root) || root.type != FENG_LSP_JSON_OBJECT) {
        return FENG_LSP_PARSE_INVALID_REQUEST;
    }
    skip_ws(&cursor, end);
    if (cursor != end) {
        return FENG_LSP_PARSE_INVALID_JSON;
    }
    if (!json_object_get(root, "method", &method) || method.type != FENG_LSP_JSON_STRING) {
        return FENG_LSP_PARSE_INVALID_REQUEST;
    }
    out_message->method = json_string_dup(method);
    if (out_message->method == NULL) {
        return FENG_LSP_PARSE_INVALID_JSON;
    }
    out_message->has_id = json_object_get(root, "id", &out_message->id);
    (void)json_object_get(root, "params", &out_message->params);
    return FENG_LSP_PARSE_OK;
}

static void message_dispose(FengLspMessage *message) {
    free(message->method);
    memset(message, 0, sizeof(*message));
}

static bool send_payload(FILE *output, const char *payload, size_t payload_length) {
    if (fprintf(output, "Content-Length: %zu\r\n\r\n", payload_length) < 0) {
        return false;
    }
    if (payload_length > 0U && fwrite(payload, 1U, payload_length, output) != payload_length) {
        return false;
    }
    return fflush(output) == 0;
}

static bool send_json_response(FILE *output,
                               FengLspJsonValue id,
                               const char *result_json) {
    FengLspString payload = {0};
    bool ok;

    if (!string_append_cstr(&payload, "{\"jsonrpc\":\"2.0\",\"id\":") ||
        !string_append_bytes(&payload, id.start, (size_t)(id.end - id.start)) ||
        !string_append_cstr(&payload, ",\"result\":") ||
        !string_append_cstr(&payload, result_json) ||
        !string_append_cstr(&payload, "}")) {
        string_dispose(&payload);
        return false;
    }
    ok = send_payload(output, payload.data, payload.length);
    string_dispose(&payload);
    return ok;
}

static bool send_error_response(FILE *output,
                                FengLspJsonValue id,
                                int code,
                                const char *message) {
    FengLspString payload = {0};
    bool ok;

    if (!string_append_cstr(&payload, "{\"jsonrpc\":\"2.0\",\"id\":") ||
        !string_append_bytes(&payload, id.start, (size_t)(id.end - id.start)) ||
        !string_append_format(&payload,
                              ",\"error\":{\"code\":%d,\"message\":",
                              code) ||
        !string_append_json_string(&payload, message) ||
        !string_append_cstr(&payload, "}}")) {
        string_dispose(&payload);
        return false;
    }
    ok = send_payload(output, payload.data, payload.length);
    string_dispose(&payload);
    return ok;
}

static bool send_notification(FILE *output,
                              const char *method,
                              const char *params_json) {
    FengLspString payload = {0};
    bool ok;

    if (!string_append_cstr(&payload, "{\"jsonrpc\":\"2.0\",\"method\":") ||
        !string_append_json_string(&payload, method) ||
        !string_append_cstr(&payload, ",\"params\":") ||
        !string_append_cstr(&payload, params_json) ||
        !string_append_cstr(&payload, "}")) {
        string_dispose(&payload);
        return false;
    }
    ok = send_payload(output, payload.data, payload.length);
    string_dispose(&payload);
    return ok;
}

static bool file_exists(const char *path) {
    struct stat st;

    return path != NULL && stat(path, &st) == 0;
}

static bool path_is_directory(const char *path) {
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *path_join(const char *lhs, const char *rhs) {
    size_t lhs_length = strlen(lhs);
    size_t rhs_length = strlen(rhs);
    bool need_sep = lhs_length > 0U && lhs[lhs_length - 1U] != '/';
    char *out = (char *)malloc(lhs_length + (need_sep ? 1U : 0U) + rhs_length + 1U);
    size_t cursor = 0U;

    if (out == NULL) {
        return NULL;
    }
    memcpy(out + cursor, lhs, lhs_length);
    cursor += lhs_length;
    if (need_sep) {
        out[cursor++] = '/';
    }
    memcpy(out + cursor, rhs, rhs_length);
    cursor += rhs_length;
    out[cursor] = '\0';
    return out;
}

static bool document_matches_disk(const FengLspDocument *document) {
    size_t disk_length = 0U;
    char *disk_text;
    bool same;

    if (document == NULL || document->text == NULL || !document->is_file || !file_exists(document->path)) {
        return false;
    }
    disk_text = feng_cli_read_entire_file(document->path, &disk_length);
    if (disk_text == NULL) {
        return false;
    }
    same = strlen(document->text) == disk_length && memcmp(document->text, disk_text, disk_length) == 0;
    free(disk_text);
    return same;
}

static int hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static char *decode_uri_path(const char *text) {
    FengLspString out = {0};
    const unsigned char *cursor;

    for (cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        if (*cursor == '%' && isxdigit((unsigned char)cursor[1]) && isxdigit((unsigned char)cursor[2])) {
            int hi = hex_value(cursor[1]);
            int lo = hex_value(cursor[2]);
            unsigned char decoded = (unsigned char)((hi << 4) | lo);

            if (!string_append_bytes(&out, (const char *)&decoded, 1U)) {
                string_dispose(&out);
                return NULL;
            }
            cursor += 2;
            continue;
        }
        if (!string_append_bytes(&out, (const char *)cursor, 1U)) {
            string_dispose(&out);
            return NULL;
        }
    }
    return out.data;
}

static char *uri_to_path(const char *uri, bool *out_is_file) {
    char *decoded;

    *out_is_file = false;
    if (uri == NULL) {
        return NULL;
    }
    if (strncmp(uri, "file://", 7U) == 0) {
        const char *path = uri + 7U;

        *out_is_file = true;
        decoded = decode_uri_path(path);
        if (decoded == NULL) {
            return NULL;
        }
        if (decoded[0] != '/') {
            char *absolute = (char *)malloc(strlen(decoded) + 2U);

            if (absolute == NULL) {
                free(decoded);
                return NULL;
            }
            absolute[0] = '/';
            strcpy(absolute + 1, decoded);
            free(decoded);
            return absolute;
        }
        return decoded;
    }
    return dup_cstr(uri);
}

static bool uri_should_escape(unsigned char ch) {
    return !(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/');
}

static char *path_to_file_uri(const char *path) {
    FengLspString uri = {0};
    const unsigned char *cursor;

    if (!string_append_cstr(&uri, "file://")) {
        string_dispose(&uri);
        return NULL;
    }
    for (cursor = (const unsigned char *)path; *cursor != '\0'; ++cursor) {
        if (uri_should_escape(*cursor)) {
            if (!string_append_format(&uri, "%%%02X", (unsigned int)*cursor)) {
                string_dispose(&uri);
                return NULL;
            }
        } else if (!string_append_bytes(&uri, (const char *)cursor, 1U)) {
            string_dispose(&uri);
            return NULL;
        }
    }
    return uri.data;
}

static FengLspDocument *find_document(FengLspRuntime *runtime, const char *uri) {
    size_t index;

    for (index = 0U; index < runtime->document_count; ++index) {
        if (strcmp(runtime->documents[index].uri, uri) == 0) {
            return &runtime->documents[index];
        }
    }
    return NULL;
}

static bool upsert_document(FengLspRuntime *runtime,
                            const char *uri,
                            const char *text) {
    FengLspDocument *document = find_document(runtime, uri);

    if (document == NULL) {
        FengLspDocument created = {0};

        created.uri = dup_cstr(uri);
        created.path = uri_to_path(uri, &created.is_file);
        created.text = dup_cstr(text != NULL ? text : "");
        if (created.uri == NULL || created.path == NULL || created.text == NULL ||
            !append_raw((void **)&runtime->documents,
                        &runtime->document_count,
                        &runtime->document_capacity,
                        sizeof(created),
                        &created)) {
            if (runtime->errors != NULL) {
                fprintf(runtime->errors,
                        "lsp: out of memory storing document '%s'\n",
                        uri != NULL ? uri : "(null)");
            }
            free(created.uri);
            free(created.path);
            free(created.text);
            return false;
        }
        return true;
    }

    free(document->text);
    document->text = dup_cstr(text != NULL ? text : "");
    if (document->text == NULL && runtime->errors != NULL) {
        fprintf(runtime->errors,
                "lsp: out of memory updating document text for '%s'\n",
                uri != NULL ? uri : "(null)");
    }
    return document->text != NULL;
}

static void remove_document(FengLspRuntime *runtime, const char *uri) {
    size_t index;

    for (index = 0U; index < runtime->document_count; ++index) {
        if (strcmp(runtime->documents[index].uri, uri) == 0) {
            free(runtime->documents[index].uri);
            free(runtime->documents[index].path);
            free(runtime->documents[index].text);
            if (index + 1U < runtime->document_count) {
                memmove(&runtime->documents[index],
                        &runtime->documents[index + 1U],
                        (runtime->document_count - index - 1U) * sizeof(runtime->documents[0]));
            }
            --runtime->document_count;
            return;
        }
    }
}

static void diagnostics_dispose(FengLspDiagnosticCollector *collector) {
    size_t index;

    for (index = 0U; index < collector->count; ++index) {
        free(collector->items[index].path);
        free(collector->items[index].message);
    }
    free(collector->items);
    collector->items = NULL;
    collector->count = 0U;
    collector->capacity = 0U;
}

static bool diagnostics_append(FengLspDiagnosticCollector *collector,
                               const char *path,
                               unsigned int line,
                               unsigned int column,
                               size_t token_length,
                               int severity,
                               const char *source,
                               const char *message) {
    FengLspDiagnosticEntry entry = {0};

    entry.path = dup_cstr(path != NULL ? path : "");
    entry.message = dup_cstr(message != NULL ? message : "unknown error");
    entry.source = source;
    entry.line = line == 0U ? 1U : line;
    entry.column = column == 0U ? 1U : column;
    entry.end_column = entry.column + (unsigned int)(token_length > 0U ? token_length : 1U);
    entry.severity = severity;
    if (entry.path == NULL || entry.message == NULL ||
        !append_raw((void **)&collector->items,
                    &collector->count,
                    &collector->capacity,
                    sizeof(entry),
                    &entry)) {
        free(entry.path);
        free(entry.message);
        return false;
    }
    return true;
}

static void session_dispose(FengLspAnalysisSession *session) {
    size_t i;
    diagnostics_dispose(&session->diagnostics);
    feng_cli_frontend_bundle_paths_dispose(session->bundle_paths, session->bundle_count);
    feng_semantic_analysis_free(session->analysis);
    feng_cli_free_loaded_sources(session->sources, session->source_count);
    if (session->owned_source_paths != NULL) {
        for (i = 0U; i < session->owned_source_path_count; ++i) {
            free(session->owned_source_paths[i]);
        }
        free(session->owned_source_paths);
    }
    free(session->manifest_path);
    feng_symbol_imported_module_cache_free(session->imported_module_cache);
    memset(session, 0, sizeof(*session));
}

static void on_parse_error_collect(void *user,
                                   const char *path,
                                   const FengParseError *error,
                                   const FengCliLoadedSource *source) {
    FengLspDiagnosticCollector *collector = (FengLspDiagnosticCollector *)user;
    (void)source;
    (void)diagnostics_append(collector,
                             path,
                             error->token.line,
                             error->token.column,
                             error->token.length,
                             1,
                             "parse",
                             error->message);
}

static void on_semantic_error_collect(void *user,
                                      const FengSemanticError *error,
                                      size_t error_index,
                                      size_t error_count,
                                      const FengCliLoadedSource *source) {
    FengLspDiagnosticCollector *collector = (FengLspDiagnosticCollector *)user;
    (void)error_index;
    (void)error_count;
    (void)source;
    (void)diagnostics_append(collector,
                             error->path,
                             error->token.line,
                             error->token.column,
                             error->token.length,
                             1,
                             "semantic",
                             error->message);
}

static void on_semantic_info_collect(void *user,
                                     const FengSemanticInfo *info,
                                     size_t info_index,
                                     size_t info_count,
                                     const FengCliLoadedSource *source) {
    FengLspDiagnosticCollector *collector = (FengLspDiagnosticCollector *)user;
    (void)info_index;
    (void)info_count;
    (void)source;
    (void)diagnostics_append(collector,
                             info->path,
                             info->token.line,
                             info->token.column,
                             info->token.length,
                             3,
                             "semantic",
                             info->message);
}

static bool source_path_list_contains(char **paths, size_t count, const char *path) {
    char *resolved_path = NULL;
    size_t index;

    if (path == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (strcmp(paths[index], path) == 0) {
            return true;
        }
    }
    resolved_path = realpath(path, NULL);
    if (resolved_path == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (strcmp(paths[index], resolved_path) == 0) {
            free(resolved_path);
            return true;
        }
    }
    free(resolved_path);
    return false;
}

static bool same_manifest(const FengLspDocument *document, const char *manifest_path) {
    char *doc_manifest = NULL;
    FengCliProjectError error = {0};
    bool ok;

    if (!document->is_file || !file_exists(document->path)) {
        return false;
    }
    ok = feng_cli_project_find_manifest_in_ancestors(document->path, &doc_manifest, &error);
    if (!ok) {
        feng_cli_project_error_dispose(&error);
        return false;
    }
    ok = strcmp(doc_manifest, manifest_path) == 0;
    free(doc_manifest);
    feng_cli_project_error_dispose(&error);
    return ok;
}

static bool same_document_identity(const FengLspDocument *lhs, const FengLspDocument *rhs) {
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    if (lhs->uri != NULL && rhs->uri != NULL && strcmp(lhs->uri, rhs->uri) == 0) {
        return true;
    }
    return lhs->path != NULL && rhs->path != NULL && strcmp(lhs->path, rhs->path) == 0;
}

static bool build_overlays(const FengLspRuntime *runtime,
                           const char *manifest_path,
                           const FengLspDocument *primary,
                           FengCliFrontendSourceOverlay **out_overlays,
                           size_t *out_count) {
    FengCliFrontendSourceOverlay *overlays = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t index;

    if (manifest_path == NULL) {
        FengCliFrontendSourceOverlay overlay = {
            .path = primary->path,
            .source = primary->text,
            .source_length = strlen(primary->text)
        };

        overlays = (FengCliFrontendSourceOverlay *)malloc(sizeof(*overlays));
        if (overlays == NULL) {
            return false;
        }
        overlays[0] = overlay;
        *out_overlays = overlays;
        *out_count = 1U;
        return true;
    }

    for (index = 0U; index < runtime->document_count; ++index) {
        const FengLspDocument *document = &runtime->documents[index];
        FengCliFrontendSourceOverlay overlay;

        if (!same_manifest(document, manifest_path)) {
            continue;
        }
        overlay.path = document->path;
        overlay.source = same_document_identity(document, primary) ? primary->text : document->text;
        overlay.source_length = strlen(overlay.source);
        if (!append_raw((void **)&overlays,
                        &count,
                        &capacity,
                        sizeof(overlay),
                        &overlay)) {
            free(overlays);
            return false;
        }
    }
    *out_overlays = overlays;
    *out_count = count;
    return true;
}

static bool append_project_error(FengLspAnalysisSession *session,
                                 const FengLspDocument *document,
                                 const char *message,
                                 unsigned int line) {
    return diagnostics_append(&session->diagnostics,
                              document->path,
                              line,
                              1U,
                              1U,
                              1,
                              "project",
                              message);
}

static bool build_standalone_session(const FengLspRuntime *runtime,
                                     const FengLspDocument *document,
                                     FengLspAnalysisSession *session) {
    FengCliFrontendSourceOverlay *overlays = NULL;
    size_t overlay_count = 0U;
    char *paths[1];
    FengCliFrontendInput input = {0};
    FengCliFrontendCallbacks callbacks = {0};
    FengCliFrontendOutputs outputs = {0};

    if (!build_overlays(runtime, NULL, document, &overlays, &overlay_count)) {
        return false;
    }
    paths[0] = document->path;
    input.path_count = 1;
    input.paths = paths;
    /* Use LIB target for standalone analysis: LSP operates on individual files
       that may not have fn main (e.g. library modules). BIN target would
       produce spurious "missing main" errors with NULL paths that crash
       feng_cli_find_loaded_source. */
    input.target = FENG_COMPILE_TARGET_LIB;

    callbacks.on_parse_error = on_parse_error_collect;
    callbacks.on_semantic_error = on_semantic_error_collect;
    callbacks.on_semantic_info = on_semantic_info_collect;
    callbacks.user = &session->diagnostics;

    outputs.out_analysis = &session->analysis;
    outputs.out_sources = &session->sources;
    outputs.out_source_count = &session->source_count;
    outputs.out_bundle_paths = &session->bundle_paths;
    outputs.out_bundle_count = &session->bundle_count;
    outputs.out_imported_module_cache = &session->imported_module_cache;

    session->exit_code = feng_cli_frontend_run_with_overlays(&input,
                                                             overlays,
                                                             overlay_count,
                                                             &callbacks,
                                                             &outputs);
    free(overlays);
    return true;
}

static bool build_project_session(const FengLspRuntime *runtime,
                                  const FengLspDocument *document,
                                  const char *manifest_path,
                                  FengLspAnalysisSession *session) {
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    FengCliFrontendSourceOverlay *overlays = NULL;
    size_t overlay_count = 0U;
    FengCliFrontendInput input = {0};
    FengCliFrontendCallbacks callbacks = {0};
    FengCliFrontendOutputs outputs = {0};

    if (!feng_cli_project_open(manifest_path, &context, &error)) {
        (void)append_project_error(session,
                                   document,
                                   error.message != NULL ? error.message : "project open failed",
                                   error.line > 0U ? error.line : 1U);
        feng_cli_project_error_dispose(&error);
        return true;
    }
    if (!source_path_list_contains(context.source_paths, context.source_count, document->path)) {
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return build_standalone_session(runtime, document, session);
    }
    if (!feng_cli_deps_resolve_for_manifest("feng",
                                            context.manifest_path,
                                            false,
                                            false,
                                            &resolved,
                                            &error)) {
        (void)append_project_error(session,
                                   document,
                                   error.message != NULL ? error.message : "dependency resolve failed",
                                   error.line > 0U ? error.line : 1U);
        feng_cli_deps_resolved_dispose(&resolved);
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return true;
    }
    if (!build_overlays(runtime, manifest_path, document, &overlays, &overlay_count)) {
        feng_cli_deps_resolved_dispose(&resolved);
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return false;
    }

    input.path_count = (int)context.source_count;
    input.paths = context.source_paths;
    input.target = context.manifest.target;
    input.package_path_count = (int)resolved.package_count;
    input.package_paths = (const char **)resolved.package_paths;

    callbacks.on_parse_error = on_parse_error_collect;
    callbacks.on_semantic_error = on_semantic_error_collect;
    callbacks.on_semantic_info = on_semantic_info_collect;
    callbacks.user = &session->diagnostics;

    outputs.out_analysis = &session->analysis;
    outputs.out_sources = &session->sources;
    outputs.out_source_count = &session->source_count;
    outputs.out_bundle_paths = &session->bundle_paths;
    outputs.out_bundle_count = &session->bundle_count;
    outputs.out_imported_module_cache = &session->imported_module_cache;

    session->manifest_path = dup_cstr(manifest_path);
    session->is_project = true;
    session->exit_code = feng_cli_frontend_run_with_overlays(&input,
                                                             overlays,
                                                             overlay_count,
                                                             &callbacks,
                                                             &outputs);
    free(overlays);
    feng_cli_deps_resolved_dispose(&resolved);
    /* session->sources[i].path borrows pointers from context.source_paths.
     * Steal the source_paths array before disposing the context so those
     * pointers remain valid for the lifetime of the session. session_dispose()
     * will free them via owned_source_paths. Clear source_count too so
     * feng_cli_project_context_dispose does not iterate a NULL array. */
    session->owned_source_paths = context.source_paths;
    session->owned_source_path_count = context.source_count;
    context.source_paths = NULL;
    context.source_count = 0U;
    feng_cli_project_context_dispose(&context);
    feng_cli_project_error_dispose(&error);
    return true;
}

static bool build_analysis_session(const FengLspRuntime *runtime,
                                   const FengLspDocument *document,
                                   FengLspAnalysisSession *session) {
    char *manifest_path = NULL;
    FengCliProjectError error = {0};

    memset(session, 0, sizeof(*session));
    if (document->is_file && file_exists(document->path) &&
        feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &error)) {
        bool ok = build_project_session(runtime, document, manifest_path, session);

        free(manifest_path);
        feng_cli_project_error_dispose(&error);
        return ok;
    }
    free(manifest_path);
    feng_cli_project_error_dispose(&error);
    return build_standalone_session(runtime, document, session);
}

static void cache_query_context_dispose(FengLspCacheQueryContext *context) {
    if (context == NULL) {
        return;
    }
    feng_program_free(context->program);
    feng_symbol_provider_free(context->provider);
    memset(context, 0, sizeof(*context));
}

static bool build_cache_query_context_for_text(const FengLspDocument *document,
                                               const char *source_text,
                                               bool include_workspace_cache,
                                               FengLspCacheQueryContext *context) {
    char *manifest_path = NULL;
    char *symbols_root = NULL;
    FengCliProjectContext project = {0};
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    FengParseError parse_error = {0};
    FengSymbolError symbol_error = {0};
    bool ok = false;

    memset(context, 0, sizeof(*context));
    if (document == NULL || source_text == NULL) {
        return false;
    }
    if (include_workspace_cache && !document_matches_disk(document)) {
        return false;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &error)) {
        goto cleanup;
    }
    if (!feng_cli_project_open(manifest_path, &project, &error) ||
        !source_path_list_contains(project.source_paths, project.source_count, document->path)) {
        goto cleanup;
    }
    symbols_root = path_join(project.out_root, "obj/symbols");
    if (!feng_parse_source(source_text,
                           strlen(source_text),
                           document->path,
                           &context->program,
                           &parse_error)) {
        goto cleanup;
    }
    if (!feng_symbol_provider_create(&context->provider, &symbol_error)) {
        goto cleanup;
    }
    /* Add workspace symbol cache when available; it is optional — bundle
     * symbols are still accessible without it. */
    if (include_workspace_cache && symbols_root != NULL && path_is_directory(symbols_root)) {
        if (!feng_symbol_provider_add_ft_root(context->provider,
                                              symbols_root,
                                              FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                                              &symbol_error)) {
            goto cleanup;
        }
    }
    if (feng_cli_deps_resolve_for_manifest("feng",
                                           project.manifest_path,
                                           false,
                                           false,
                                           &resolved,
                                           &error)) {
        size_t index;

        for (index = 0U; index < resolved.package_count; ++index) {
            if (!feng_symbol_provider_add_bundle(context->provider,
                                                 resolved.package_paths[index],
                                                 &symbol_error)) {
                goto cleanup;
            }
        }
    }
    context->current_module = feng_symbol_provider_find_module(context->provider,
                                                               context->program->module_segments,
                                                               context->program->module_segment_count);
    context->source_text = source_text;
    /* The context is useful even when current_module is NULL: bundle symbols
     * are still accessible via the provider for hover and go-to-definition of
     * external package types. */
    ok = true;

cleanup:
    if (!ok) {
        cache_query_context_dispose(context);
    }
    feng_symbol_error_free(&symbol_error);
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_context_dispose(&project);
    feng_cli_project_error_dispose(&error);
    free(symbols_root);
    free(manifest_path);
    return ok;
}

static bool build_cache_query_context(const FengLspDocument *document,
                                      FengLspCacheQueryContext *context) {
    return build_cache_query_context_for_text(document,
                                              document != NULL ? document->text : NULL,
                                              true,
                                              context);
}

static bool diagnostics_json_for_path(const FengLspDiagnosticCollector *collector,
                                      const char *path,
                                      FengLspString *json) {
    size_t index;
    char *uri = path_to_file_uri(path);
    bool first = true;

    if (uri == NULL) {
        return false;
    }
    if (!string_append_cstr(json, "{\"uri\":") ||
        !string_append_json_string(json, uri) ||
        !string_append_cstr(json, ",\"diagnostics\":[")) {
        free(uri);
        return false;
    }
    free(uri);

    for (index = 0U; index < collector->count; ++index) {
        const FengLspDiagnosticEntry *entry = &collector->items[index];

        if (strcmp(entry->path, path) != 0) {
            continue;
        }
        if (!first && !string_append_cstr(json, ",")) {
            return false;
        }
        first = false;
        if (!string_append_cstr(json, "{\"range\":{\"start\":{\"line\":") ||
            !string_append_format(json, "%u", entry->line > 0U ? entry->line - 1U : 0U) ||
            !string_append_cstr(json, ",\"character\":") ||
            !string_append_format(json, "%u", entry->column > 0U ? entry->column - 1U : 0U) ||
            !string_append_cstr(json, "},\"end\":{\"line\":") ||
            !string_append_format(json, "%u", entry->line > 0U ? entry->line - 1U : 0U) ||
            !string_append_cstr(json, ",\"character\":") ||
            !string_append_format(json, "%u", entry->end_column > 0U ? entry->end_column - 1U : 0U) ||
            !string_append_cstr(json, "}},\"severity\":") ||
            !string_append_format(json, "%d", entry->severity) ||
            !string_append_cstr(json, ",\"source\":") ||
            !string_append_json_string(json, entry->source) ||
            !string_append_cstr(json, ",\"message\":") ||
            !string_append_json_string(json, entry->message) ||
            !string_append_cstr(json, "}")) {
            return false;
        }
    }
    return string_append_cstr(json, "]}");
}

static bool publish_diagnostics(FILE *output,
                                const FengLspDiagnosticCollector *collector,
                                const char *path) {
    FengLspString params = {0};
    bool ok = diagnostics_json_for_path(collector, path, &params) &&
              send_notification(output, "textDocument/publishDiagnostics", params.data);

    string_dispose(&params);
    return ok;
}

static bool publish_session_diagnostics(const FengLspRuntime *runtime,
                                        const FengLspDocument *primary,
                                        FILE *output,
                                        const FengLspAnalysisSession *session) {
    size_t index;

    if (!session->is_project || session->manifest_path == NULL) {
        return publish_diagnostics(output, &session->diagnostics, primary->path);
    }
    for (index = 0U; index < runtime->document_count; ++index) {
        const FengLspDocument *document = &runtime->documents[index];

        if (same_manifest(document, session->manifest_path) &&
            !publish_diagnostics(output, &session->diagnostics, document->path)) {
            return false;
        }
    }
    return true;
}

static bool publish_empty_diagnostics(FILE *output, const FengLspDocument *document) {
    FengLspDiagnosticCollector collector = {0};
    bool ok = publish_diagnostics(output, &collector, document->path);

    diagnostics_dispose(&collector);
    return ok;
}

static bool refresh_diagnostics(FengLspRuntime *runtime,
                                FILE *output,
                                const char *uri) {
    FengLspDocument *document = find_document(runtime, uri);
    FengLspAnalysisSession session = {0};
    bool ok;

    if (document == NULL) {
        return true;
    }
    if (!build_analysis_session(runtime, document, &session)) {
        if (runtime->errors != NULL) {
            fprintf(runtime->errors,
                    "lsp: refresh_diagnostics: failed to build analysis session for '%s'\n",
                    uri != NULL ? uri : "(null)");
        }
        return false;
    }
    ok = publish_session_diagnostics(runtime, document, output, &session);
    if (!ok && runtime->errors != NULL) {
        fprintf(runtime->errors,
                "lsp: refresh_diagnostics: failed to publish diagnostics for '%s'\n",
                uri != NULL ? uri : "(null)");
    }
    session_dispose(&session);
    return ok;
}

static bool json_u32(FengLspJsonValue value, unsigned int *out_number) {
    char *endptr = NULL;
    char *text;
    unsigned long parsed;
    bool ok;

    if (value.type != FENG_LSP_JSON_NUMBER) {
        return false;
    }
    text = dup_range(value.start, value.end);
    if (text == NULL) {
        return false;
    }
    if (text[0] == '-') {
        free(text);
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &endptr, 10);
    ok = endptr != NULL && *endptr == '\0' && errno != ERANGE && parsed <= UINT_MAX;
    free(text);
    if (!ok) {
        return false;
    }
    *out_number = (unsigned int)parsed;
    return true;
}

static bool json_bool(FengLspJsonValue value, bool *out_value) {
    if (value.type != FENG_LSP_JSON_BOOL || out_value == NULL) {
        return false;
    }

    *out_value = (size_t)(value.end - value.start) == 4U;
    return true;
}

static FengSlice slice_from_cstr(const char *text) {
    FengSlice slice = {0};

    if (text != NULL) {
        slice.data = text;
        slice.length = strlen(text);
    }
    return slice;
}

static bool slice_equals(FengSlice lhs, FengSlice rhs) {
    return lhs.length == rhs.length &&
           (lhs.length == 0U || memcmp(lhs.data, rhs.data, lhs.length) == 0);
}

static bool slice_equals_cstr(FengSlice lhs, const char *rhs) {
    FengSlice rhs_slice = slice_from_cstr(rhs);
    return slice_equals(lhs, rhs_slice);
}

static size_t token_end_offset(FengToken token) {
    return token.offset + (token.length > 0U ? token.length : 1U);
}

static bool offset_in_token(FengToken token, size_t offset) {
    return offset >= token.offset && offset <= token_end_offset(token);
}

static bool offset_in_slice_from_source(const char *source_text,
                                        FengSlice slice,
                                        size_t offset) {
    size_t start;

    if (source_text == NULL || slice.data == NULL) {
        return false;
    }
    start = (size_t)(slice.data - source_text);
    return offset >= start && offset <= start + slice.length;
}

static size_t named_type_ref_end(const FengTypeRef *type_ref) {
    size_t cursor = type_ref->token.offset;
    size_t index;

    for (index = 0U; index < type_ref->as.named.segment_count; ++index) {
        cursor += type_ref->as.named.segments[index].length;
        if (index + 1U < type_ref->as.named.segment_count) {
            ++cursor;
        }
    }
    return cursor;
}

static size_t type_ref_end(const FengTypeRef *type_ref);
static size_t expr_end(const FengExpr *expr);
static size_t stmt_end(const FengStmt *stmt);
static size_t block_end(const FengBlock *block);
static size_t member_end(const FengTypeMember *member);
static size_t decl_end(const FengDecl *decl);
static size_t block_end_for_source(const char *source, const FengBlock *block);
static size_t stmt_end_for_source(const char *source, const FengStmt *stmt);
static size_t member_end_for_source(const char *source, const FengTypeMember *member);
static size_t decl_end_for_source(const char *source, const FengDecl *decl);

/* Returns the byte offset of the first character of expr. For most expressions
 * this is expr->token.offset, but OBJECT_LITERAL (whose token is '{') actually
 * begins at the type-name target expression that precedes it. */
static size_t expr_start(const FengExpr *expr) {
    if (expr == NULL) {
        return 0U;
    }
    if (expr->kind == FENG_EXPR_OBJECT_LITERAL && expr->as.object_literal.target != NULL) {
        return expr->as.object_literal.target->token.offset;
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL) {
        return expr_start(expr->as.member.object);
    }
    if (expr->kind == FENG_EXPR_CALL && expr->as.call.callee != NULL) {
        return expr_start(expr->as.call.callee);
    }
    /* Binary expressions store the operator as their token, but the actual
     * span begins at the left operand.  Without this adjustment, hovering or
     * clicking on the left operand of any binary expression fails the
     * early-exit check in find_expr_hit. */
    if (expr->kind == FENG_EXPR_BINARY && expr->as.binary.left != NULL) {
        return expr_start(expr->as.binary.left);
    }
    return expr->token.offset;
}

static size_t type_ref_end(const FengTypeRef *type_ref) {
    if (type_ref == NULL) {
        return 0U;
    }
    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            return named_type_ref_end(type_ref);
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            return type_ref->as.inner != NULL ? type_ref_end(type_ref->as.inner) : token_end_offset(type_ref->token);
    }
    return token_end_offset(type_ref->token);
}

static size_t block_end(const FengBlock *block) {
    size_t end;
    size_t index;

    if (block == NULL) {
        return 0U;
    }
    end = token_end_offset(block->token);
    for (index = 0U; index < block->statement_count; ++index) {
        size_t stmt_limit = stmt_end(block->statements[index]);

        if (stmt_limit > end) {
            end = stmt_limit;
        }
    }
    return end;
}

static size_t expr_end(const FengExpr *expr) {
    size_t end;
    size_t index;

    if (expr == NULL) {
        return 0U;
    }
    end = token_end_offset(expr->token);
    switch (expr->kind) {
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                size_t item_end = expr_end(expr->as.array_literal.items[index]);

                if (item_end > end) {
                    end = item_end;
                }
            }
            break;
        case FENG_EXPR_OBJECT_LITERAL:
            if (expr->as.object_literal.target != NULL) {
                size_t target_end = expr_end(expr->as.object_literal.target);

                if (target_end > end) {
                    end = target_end;
                }
            }
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                size_t value_end = expr_end(expr->as.object_literal.fields[index].value);

                if (value_end > end) {
                    end = value_end;
                }
            }
            break;
        case FENG_EXPR_GENERIC_TARGET:
            if (expr->as.generic_target.target != NULL) {
                size_t target_end = expr_end(expr->as.generic_target.target);

                if (target_end > end) {
                    end = target_end;
                }
            }
            for (index = 0U; index < expr->as.generic_target.type_arg_count; ++index) {
                size_t type_end = type_ref_end(expr->as.generic_target.type_args[index]);

                if (type_end > end) {
                    end = type_end;
                }
            }
            break;
        case FENG_EXPR_CALL:
            if (expr->as.call.callee != NULL) {
                size_t callee_end = expr_end(expr->as.call.callee);

                if (callee_end > end) {
                    end = callee_end;
                }
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                size_t arg_end = expr_end(expr->as.call.args[index]);

                if (arg_end > end) {
                    end = arg_end;
                }
            }
            break;
        case FENG_EXPR_MEMBER:
            if (expr->as.member.object != NULL) {
                size_t object_end = expr_end(expr->as.member.object);

                if (object_end > end) {
                    end = object_end;
                }
            }
            break;
        case FENG_EXPR_INDEX:
            if (expr->as.index.object != NULL) {
                size_t object_end = expr_end(expr->as.index.object);

                if (object_end > end) {
                    end = object_end;
                }
            }
            if (expr->as.index.index != NULL) {
                size_t index_end = expr_end(expr->as.index.index);

                if (index_end > end) {
                    end = index_end;
                }
            }
            break;
        case FENG_EXPR_UNARY:
            if (expr->as.unary.operand != NULL) {
                size_t operand_end = expr_end(expr->as.unary.operand);

                if (operand_end > end) {
                    end = operand_end;
                }
            }
            break;
        case FENG_EXPR_BINARY:
            if (expr->as.binary.left != NULL) {
                size_t left_end = expr_end(expr->as.binary.left);

                if (left_end > end) {
                    end = left_end;
                }
            }
            if (expr->as.binary.right != NULL) {
                size_t right_end = expr_end(expr->as.binary.right);

                if (right_end > end) {
                    end = right_end;
                }
            }
            break;
        case FENG_EXPR_LAMBDA:
            if (expr->as.lambda.is_block_body) {
                size_t body_end = block_end(expr->as.lambda.body_block);

                if (body_end > end) {
                    end = body_end;
                }
            } else if (expr->as.lambda.body != NULL) {
                size_t body_end = expr_end(expr->as.lambda.body);

                if (body_end > end) {
                    end = body_end;
                }
            }
            break;
        case FENG_EXPR_CAST:
            if (expr->as.cast.type != NULL) {
                size_t type_end = type_ref_end(expr->as.cast.type);

                if (type_end > end) {
                    end = type_end;
                }
            }
            if (expr->as.cast.value != NULL) {
                size_t value_end = expr_end(expr->as.cast.value);

                if (value_end > end) {
                    end = value_end;
                }
            }
            break;
        case FENG_EXPR_IF:
            if (expr->as.if_expr.condition != NULL) {
                size_t cond_end = expr_end(expr->as.if_expr.condition);

                if (cond_end > end) {
                    end = cond_end;
                }
            }
            if (expr->as.if_expr.then_block != NULL) {
                size_t then_end = block_end(expr->as.if_expr.then_block);

                if (then_end > end) {
                    end = then_end;
                }
            }
            if (expr->as.if_expr.else_block != NULL) {
                size_t else_end = block_end(expr->as.if_expr.else_block);

                if (else_end > end) {
                    end = else_end;
                }
            }
            break;
        case FENG_EXPR_MATCH:
            if (expr->as.match_expr.target != NULL) {
                size_t target_end = expr_end(expr->as.match_expr.target);

                if (target_end > end) {
                    end = target_end;
                }
            }
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                size_t branch_end = block_end(expr->as.match_expr.branches[index].body);

                if (branch_end > end) {
                    end = branch_end;
                }
            }
            if (expr->as.match_expr.else_block != NULL) {
                size_t else_end = block_end(expr->as.match_expr.else_block);

                if (else_end > end) {
                    end = else_end;
                }
            }
            break;
        case FENG_EXPR_TRY:
            if (expr->as.try_expr.body != NULL) {
                size_t body_end = expr_end(expr->as.try_expr.body);

                if (body_end > end) {
                    end = body_end;
                }
            }
            for (index = 0U; index < expr->as.try_expr.clause_count; ++index) {
                size_t clause_end = block_end(expr->as.try_expr.clauses[index].body);

                if (clause_end > end) {
                    end = clause_end;
                }
            }
            break;
        default:
            break;
    }
    return end;
}

static size_t stmt_end(const FengStmt *stmt) {
    size_t end;
    size_t index;

    if (stmt == NULL) {
        return 0U;
    }
    end = token_end_offset(stmt->token);
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return block_end(stmt->as.block);
        case FENG_STMT_BINDING:
            if (stmt->as.binding.type != NULL) {
                size_t type_end = type_ref_end(stmt->as.binding.type);
                if (type_end > end) {
                    end = type_end;
                }
            }
            if (stmt->as.binding.initializer != NULL) {
                size_t init_end = expr_end(stmt->as.binding.initializer);
                if (init_end > end) {
                    end = init_end;
                }
            }
            break;
        case FENG_STMT_ASSIGN:
            if (stmt->as.assign.target != NULL) {
                size_t target_end = expr_end(stmt->as.assign.target);
                if (target_end > end) {
                    end = target_end;
                }
            }
            if (stmt->as.assign.value != NULL) {
                size_t value_end = expr_end(stmt->as.assign.value);
                if (value_end > end) {
                    end = value_end;
                }
            }
            break;
        case FENG_STMT_TRY:
        case FENG_STMT_EXPR:
            if (stmt->as.expr != NULL) {
                size_t expr_limit = expr_end(stmt->as.expr);
                if (expr_limit > end) {
                    end = expr_limit;
                }
            }
            break;
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                size_t cond_end = expr_end(stmt->as.if_stmt.clauses[index].condition);
                size_t block_limit = block_end(stmt->as.if_stmt.clauses[index].block);
                if (cond_end > end) {
                    end = cond_end;
                }
                if (block_limit > end) {
                    end = block_limit;
                }
            }
            if (stmt->as.if_stmt.else_block != NULL) {
                size_t else_end = block_end(stmt->as.if_stmt.else_block);
                if (else_end > end) {
                    end = else_end;
                }
            }
            break;
        case FENG_STMT_MATCH:
            if (stmt->as.match_stmt.target != NULL) {
                size_t target_end = expr_end(stmt->as.match_stmt.target);
                if (target_end > end) {
                    end = target_end;
                }
            }
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                size_t branch_end = block_end(stmt->as.match_stmt.branches[index].body);
                if (branch_end > end) {
                    end = branch_end;
                }
            }
            if (stmt->as.match_stmt.else_block != NULL) {
                size_t else_end = block_end(stmt->as.match_stmt.else_block);
                if (else_end > end) {
                    end = else_end;
                }
            }
            break;
        case FENG_STMT_WHILE:
            if (stmt->as.while_stmt.condition != NULL) {
                size_t cond_end = expr_end(stmt->as.while_stmt.condition);
                if (cond_end > end) {
                    end = cond_end;
                }
            }
            if (stmt->as.while_stmt.body != NULL) {
                size_t body_end = block_end(stmt->as.while_stmt.body);
                if (body_end > end) {
                    end = body_end;
                }
            }
            break;
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                if (stmt->as.for_stmt.iter_binding.type != NULL) {
                    size_t type_end = type_ref_end(stmt->as.for_stmt.iter_binding.type);
                    if (type_end > end) {
                        end = type_end;
                    }
                }
                if (stmt->as.for_stmt.iter_expr != NULL) {
                    size_t iter_end = expr_end(stmt->as.for_stmt.iter_expr);
                    if (iter_end > end) {
                        end = iter_end;
                    }
                }
            } else {
                size_t init_end = stmt_end(stmt->as.for_stmt.init);
                size_t cond_end = expr_end(stmt->as.for_stmt.condition);
                size_t update_end = stmt_end(stmt->as.for_stmt.update);
                if (init_end > end) {
                    end = init_end;
                }
                if (cond_end > end) {
                    end = cond_end;
                }
                if (update_end > end) {
                    end = update_end;
                }
            }
            if (stmt->as.for_stmt.body != NULL) {
                size_t body_end = block_end(stmt->as.for_stmt.body);
                if (body_end > end) {
                    end = body_end;
                }
            }
            break;
        case FENG_STMT_RETURN:
            if (stmt->as.return_value != NULL) {
                size_t return_end = expr_end(stmt->as.return_value);
                if (return_end > end) {
                    end = return_end;
                }
            }
            break;
        case FENG_STMT_THROW:
            if (stmt->as.throw_value != NULL) {
                size_t throw_end = expr_end(stmt->as.throw_value);
                if (throw_end > end) {
                    end = throw_end;
                }
            }
            break;
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            break;
    }
    return end;
}

static size_t member_end(const FengTypeMember *member) {
    size_t end;
    size_t index;

    if (member == NULL) {
        return 0U;
    }
    end = token_end_offset(member->token);
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        if (member->as.field.type != NULL) {
            size_t type_end = type_ref_end(member->as.field.type);
            if (type_end > end) {
                end = type_end;
            }
        }
        if (member->as.field.initializer != NULL) {
            size_t init_end = expr_end(member->as.field.initializer);
            if (init_end > end) {
                end = init_end;
            }
        }
    } else {
        for (index = 0U; index < member->as.callable.param_count; ++index) {
            size_t param_end = token_end_offset(member->as.callable.params[index].token);
            if (member->as.callable.params[index].type != NULL) {
                param_end = type_ref_end(member->as.callable.params[index].type);
            }
            if (param_end > end) {
                end = param_end;
            }
        }
        if (member->as.callable.return_type != NULL) {
            size_t return_end = type_ref_end(member->as.callable.return_type);
            if (return_end > end) {
                end = return_end;
            }
        }
        if (member->as.callable.body != NULL) {
            size_t body_end = block_end(member->as.callable.body);
            if (body_end > end) {
                end = body_end;
            }
        }
    }
    return end;
}

static size_t decl_end(const FengDecl *decl) {
    size_t end;
    size_t index;

    if (decl == NULL) {
        return 0U;
    }
    end = token_end_offset(decl->token);
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            if (decl->as.binding.type != NULL) {
                size_t type_end = type_ref_end(decl->as.binding.type);
                if (type_end > end) {
                    end = type_end;
                }
            }
            if (decl->as.binding.initializer != NULL) {
                size_t init_end = expr_end(decl->as.binding.initializer);
                if (init_end > end) {
                    end = init_end;
                }
            }
            break;
        case FENG_DECL_ENUM:
            if (decl->as.enum_decl.item_count > 0U) {
                size_t limit = token_end_offset(
                    decl->as.enum_decl.items[decl->as.enum_decl.item_count - 1U].token);

                if (limit > end) {
                    end = limit;
                }
            }
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                size_t limit = member_end(decl->as.type_decl.members[index]);
                if (limit > end) {
                    end = limit;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    size_t limit = member_end(decl->as.spec_decl.as.object.members[index]);
                    if (limit > end) {
                        end = limit;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                size_t limit = member_end(decl->as.fit_decl.members[index]);
                if (limit > end) {
                    end = limit;
                }
            }
            break;
        case FENG_DECL_FUNCTION:
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                size_t param_end = token_end_offset(decl->as.function_decl.params[index].token);
                if (decl->as.function_decl.params[index].type != NULL) {
                    param_end = type_ref_end(decl->as.function_decl.params[index].type);
                }
                if (param_end > end) {
                    end = param_end;
                }
            }
            if (decl->as.function_decl.return_type != NULL) {
                size_t return_end = type_ref_end(decl->as.function_decl.return_type);
                if (return_end > end) {
                    end = return_end;
                }
            }
            if (decl->as.function_decl.body != NULL) {
                size_t body_end = block_end(decl->as.function_decl.body);
                if (body_end > end) {
                    end = body_end;
                }
            }
            break;
    }
    return end;
}

static bool matching_brace_end_from_lbrace(const char *source,
                                           size_t lbrace_offset,
                                           size_t *out_end) {
    FengLexer lexer;
    FengToken token;
    size_t depth = 0U;
    bool started = false;

    if (source == NULL || out_end == NULL) {
        return false;
    }
    feng_lexer_init(&lexer, source, strlen(source), NULL);
    for (;;) {
        token = feng_lexer_next(&lexer);
        if (token.kind == FENG_TOKEN_EOF) {
            return false;
        }
        if (!started) {
            if (token.offset < lbrace_offset) {
                continue;
            }
            if (token.kind != FENG_TOKEN_LBRACE || token.offset != lbrace_offset) {
                return false;
            }
            started = true;
            depth = 1U;
            continue;
        }
        if (token.kind == FENG_TOKEN_LBRACE) {
            ++depth;
        } else if (token.kind == FENG_TOKEN_RBRACE) {
            --depth;
            if (depth == 0U) {
                *out_end = token_end_offset(token);
                return true;
            }
        }
    }
}

static bool matching_brace_end_after_offset(const char *source,
                                            size_t start_offset,
                                            size_t *out_end) {
    FengLexer lexer;
    FengToken token;

    if (source == NULL || out_end == NULL) {
        return false;
    }
    feng_lexer_init(&lexer, source, strlen(source), NULL);
    for (;;) {
        token = feng_lexer_next(&lexer);
        if (token.kind == FENG_TOKEN_EOF) {
            return false;
        }
        if (token.offset < start_offset) {
            continue;
        }
        if (token.kind == FENG_TOKEN_LBRACE) {
            return matching_brace_end_from_lbrace(source, token.offset, out_end);
        }
    }
}

static size_t max_size(size_t lhs, size_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

static size_t block_end_for_source(const char *source, const FengBlock *block) {
    size_t end;
    size_t close_end;

    if (block == NULL) {
        return 0U;
    }
    end = block_end(block);
    return matching_brace_end_from_lbrace(source, block->token.offset, &close_end)
               ? max_size(end, close_end)
               : end;
}

static size_t stmt_end_for_source(const char *source, const FengStmt *stmt) {
    size_t end;

    if (stmt == NULL) {
        return 0U;
    }
    end = stmt_end(stmt);
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return max_size(end, block_end_for_source(source, stmt->as.block));
        case FENG_STMT_IF: {
            size_t index;
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                end = max_size(end, block_end_for_source(source, stmt->as.if_stmt.clauses[index].block));
            }
            return max_size(end, block_end_for_source(source, stmt->as.if_stmt.else_block));
        }
        case FENG_STMT_MATCH: {
            size_t index;
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                end = max_size(end, block_end_for_source(source, stmt->as.match_stmt.branches[index].body));
            }
            return max_size(end, block_end_for_source(source, stmt->as.match_stmt.else_block));
        }
        case FENG_STMT_WHILE:
            return max_size(end, block_end_for_source(source, stmt->as.while_stmt.body));
        case FENG_STMT_FOR:
            return max_size(end, block_end_for_source(source, stmt->as.for_stmt.body));
        default:
            return end;
    }
}

static size_t member_end_for_source(const char *source, const FengTypeMember *member) {
    size_t end;

    if (member == NULL) {
        return 0U;
    }
    end = member_end(member);
    if (member->kind != FENG_TYPE_MEMBER_FIELD && member->as.callable.body != NULL) {
        end = max_size(end, block_end_for_source(source, member->as.callable.body));
    }
    return end;
}

static size_t decl_end_for_source(const char *source, const FengDecl *decl) {
    size_t end;
    size_t close_end;

    if (decl == NULL) {
        return 0U;
    }
    end = decl_end(decl);
    switch (decl->kind) {
        case FENG_DECL_ENUM:
            if (matching_brace_end_after_offset(source, token_end_offset(decl->token), &close_end)) {
                end = max_size(end, close_end);
            }
            break;
        case FENG_DECL_TYPE:
            if (matching_brace_end_after_offset(source, token_end_offset(decl->token), &close_end)) {
                end = max_size(end, close_end);
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT &&
                matching_brace_end_after_offset(source, token_end_offset(decl->token), &close_end)) {
                end = max_size(end, close_end);
            }
            break;
        case FENG_DECL_FIT:
            if (decl->as.fit_decl.has_body &&
                matching_brace_end_after_offset(source, token_end_offset(decl->token), &close_end)) {
                end = max_size(end, close_end);
            }
            break;
        case FENG_DECL_FUNCTION:
            end = max_size(end, block_end_for_source(source, decl->as.function_decl.body));
            break;
        default:
            break;
    }
    return end;
}

static const FengCliLoadedSource *find_source(const FengLspAnalysisSession *session,
                                              const char *path) {
    const FengCliLoadedSource *source = feng_cli_find_loaded_source(session->sources,
                                                                    session->source_count,
                                                                    path);
    if (source == NULL && path != NULL) {
        /* The session's source paths are canonical (resolved via realpath
         * inside feng_cli_project_open).  If the lookup above failed, the
         * caller's path may go through a symlink (e.g. /tmp -> /private/tmp
         * on macOS).  Resolve it and retry. */
        char *resolved = realpath(path, NULL);
        if (resolved != NULL) {
            source = feng_cli_find_loaded_source(session->sources,
                                                 session->source_count,
                                                 resolved);
            free(resolved);
        }
    }
    return source;
}

static const FengProgram *find_program(const FengLspAnalysisSession *session,
                                       const char *path) {
    const FengCliLoadedSource *source = find_source(session, path);
    return source != NULL ? source->program : NULL;
}

static const FengSemanticModule *find_module_by_segments(const FengSemanticAnalysis *analysis,
                                                         const FengSlice *segments,
                                                         size_t segment_count) {
    size_t index;
    size_t seg_index;

    if (analysis == NULL) {
        return NULL;
    }
    for (index = 0U; index < analysis->module_count; ++index) {
        const FengSemanticModule *module = &analysis->modules[index];
        bool same = module->segment_count == segment_count;

        for (seg_index = 0U; same && seg_index < segment_count; ++seg_index) {
            same = slice_equals(module->segments[seg_index], segments[seg_index]);
        }
        if (same) {
            return module;
        }
    }
    return NULL;
}

static const FengSemanticModule *find_program_module(const FengLspAnalysisSession *session,
                                                     const FengProgram *program) {
    size_t module_index;
    size_t program_index;

    if (session->analysis == NULL || program == NULL) {
        return NULL;
    }
    for (module_index = 0U; module_index < session->analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &session->analysis->modules[module_index];

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            if (module->programs[program_index] == program) {
                return module;
            }
        }
    }
    return NULL;
}

static const FengSemanticModule *find_decl_module(const FengLspAnalysisSession *session,
                                                  const FengDecl *decl,
                                                  const FengProgram **out_program) {
    size_t module_index;
    size_t program_index;
    size_t decl_index;

    *out_program = NULL;
    if (decl == NULL || session->analysis == NULL) {
        return NULL;
    }
    for (module_index = 0U; module_index < session->analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &session->analysis->modules[module_index];

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            const FengProgram *program = module->programs[program_index];

            for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
                if (program->declarations[decl_index] == decl) {
                    *out_program = program;
                    return module;
                }
            }
        }
    }
    return NULL;
}

static bool local_list_push(FengLspLocalList *locals,
                            FengLspLocalKind kind,
                            FengSlice name,
                            const FengParameter *parameter,
                            const FengBinding *binding,
                            const FengDecl *self_owner_decl) {
    FengLspLocal local = {
        .kind = kind,
        .name = name,
        .parameter = parameter,
        .binding = binding,
        .self_owner_decl = self_owner_decl
    };

    return append_raw((void **)&locals->items,
                      &locals->count,
                      &locals->capacity,
                      sizeof(local),
                      &local);
}

static void local_list_dispose(FengLspLocalList *locals) {
    free(locals->items);
    locals->items = NULL;
    locals->count = 0U;
    locals->capacity = 0U;
}

static const FengLspLocal *find_local(const FengLspLocalList *locals, FengSlice name) {
    size_t index = locals->count;

    while (index > 0U) {
        --index;
        if (slice_equals(locals->items[index].name, name)) {
            return &locals->items[index];
        }
    }
    return NULL;
}

static bool collect_stmt_locals(const FengStmt *stmt,
                                size_t offset,
                                FengLspLocalList *locals);

static bool collect_block_locals(const FengBlock *block,
                                 size_t offset,
                                 FengLspLocalList *locals) {
    size_t index;

    /* Out-of-range is a no-op: locals simply aren't visible at the cursor. */
    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return true;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        const FengStmt *stmt = block->statements[index];

        if (offset < stmt->token.offset) {
            break;
        }
        if (offset <= stmt_end(stmt)) {
            return collect_stmt_locals(stmt, offset, locals);
        }
        if (stmt->kind == FENG_STMT_BINDING &&
            !local_list_push(locals,
                             FENG_LSP_LOCAL_BINDING,
                             stmt->as.binding.name,
                             NULL,
                             &stmt->as.binding,
                             NULL)) {
            return false;
        }
    }
    return true;
}

static bool collect_stmt_locals(const FengStmt *stmt,
                                size_t offset,
                                FengLspLocalList *locals) {
    size_t index;

    /* Out-of-range is a no-op: locals simply aren't visible at the cursor. */
    if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end(stmt)) {
        return true;
    }
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return collect_block_locals(stmt->as.block, offset, locals);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (offset <= expr_end(stmt->as.if_stmt.clauses[index].condition)) {
                    return true;
                }
                if (offset <= block_end(stmt->as.if_stmt.clauses[index].block)) {
                    return collect_block_locals(stmt->as.if_stmt.clauses[index].block, offset, locals);
                }
            }
            return stmt->as.if_stmt.else_block != NULL
                       ? collect_block_locals(stmt->as.if_stmt.else_block, offset, locals)
                       : true;
        case FENG_STMT_MATCH:
            if (stmt->as.match_stmt.target != NULL && offset <= expr_end(stmt->as.match_stmt.target)) {
                return true;
            }
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (offset <= block_end(stmt->as.match_stmt.branches[index].body)) {
                    return collect_block_locals(stmt->as.match_stmt.branches[index].body, offset, locals);
                }
            }
            return stmt->as.match_stmt.else_block != NULL
                       ? collect_block_locals(stmt->as.match_stmt.else_block, offset, locals)
                       : true;
        case FENG_STMT_WHILE:
            if (stmt->as.while_stmt.condition != NULL && offset <= expr_end(stmt->as.while_stmt.condition)) {
                return true;
            }
            return stmt->as.while_stmt.body != NULL
                       ? collect_block_locals(stmt->as.while_stmt.body, offset, locals)
                       : true;
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                if (stmt->as.for_stmt.iter_expr != NULL && offset <= expr_end(stmt->as.for_stmt.iter_expr)) {
                    return true;
                }
                if (!local_list_push(locals,
                                     FENG_LSP_LOCAL_BINDING,
                                     stmt->as.for_stmt.iter_binding.name,
                                     NULL,
                                     &stmt->as.for_stmt.iter_binding,
                                     NULL)) {
                    return false;
                }
                return stmt->as.for_stmt.body != NULL
                           ? collect_block_locals(stmt->as.for_stmt.body, offset, locals)
                           : true;
            }
            if (stmt->as.for_stmt.init != NULL && offset <= stmt_end(stmt->as.for_stmt.init)) {
                return collect_stmt_locals(stmt->as.for_stmt.init, offset, locals);
            }
            if (stmt->as.for_stmt.init != NULL && stmt->as.for_stmt.init->kind == FENG_STMT_BINDING &&
                !local_list_push(locals,
                                 FENG_LSP_LOCAL_BINDING,
                                 stmt->as.for_stmt.init->as.binding.name,
                                 NULL,
                                 &stmt->as.for_stmt.init->as.binding,
                                 NULL)) {
                return false;
            }
            if (stmt->as.for_stmt.condition != NULL && offset <= expr_end(stmt->as.for_stmt.condition)) {
                return true;
            }
            if (stmt->as.for_stmt.update != NULL && offset <= stmt_end(stmt->as.for_stmt.update)) {
                return true;
            }
            return stmt->as.for_stmt.body != NULL
                       ? collect_block_locals(stmt->as.for_stmt.body, offset, locals)
                       : true;
        default:
            return true;
    }
}

static const FengDecl *find_enclosing_decl(const FengProgram *program,
                                           size_t offset,
                                           const FengTypeMember **out_member) {
    size_t decl_index;

    *out_member = NULL;
    if (program == NULL) {
        return NULL;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        size_t member_index;

        if (offset < decl->token.offset || offset > decl_end(decl)) {
            continue;
        }
        if (decl->kind == FENG_DECL_TYPE) {
            for (member_index = 0U; member_index < decl->as.type_decl.member_count; ++member_index) {
                if (offset >= decl->as.type_decl.members[member_index]->token.offset &&
                    offset <= member_end(decl->as.type_decl.members[member_index])) {
                    *out_member = decl->as.type_decl.members[member_index];
                    return decl;
                }
            }
        } else if (decl->kind == FENG_DECL_SPEC && decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
            for (member_index = 0U; member_index < decl->as.spec_decl.as.object.member_count; ++member_index) {
                if (offset >= decl->as.spec_decl.as.object.members[member_index]->token.offset &&
                    offset <= member_end(decl->as.spec_decl.as.object.members[member_index])) {
                    *out_member = decl->as.spec_decl.as.object.members[member_index];
                    return decl;
                }
            }
        } else if (decl->kind == FENG_DECL_FIT) {
            for (member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                if (offset >= decl->as.fit_decl.members[member_index]->token.offset &&
                    offset <= member_end(decl->as.fit_decl.members[member_index])) {
                    *out_member = decl->as.fit_decl.members[member_index];
                    return decl;
                }
            }
        }
        return decl;
    }
    return NULL;
}

static bool callable_collect_params(const FengCallableSignature *callable,
                                    FengLspLocalList *locals) {
    size_t index;

    if (callable == NULL) {
        return false;
    }
    for (index = 0U; index < callable->param_count; ++index) {
        if (!local_list_push(locals,
                             FENG_LSP_LOCAL_PARAM,
                             callable->params[index].name,
                             &callable->params[index],
                             NULL,
                             NULL)) {
            return false;
        }
    }
    return true;
}

static bool collect_visible_locals(const FengDecl *decl,
                                   const FengTypeMember *member,
                                   size_t offset,
                                   FengLspLocalList *locals) {
    if (member != NULL && member->kind != FENG_TYPE_MEMBER_FIELD) {
        if (!callable_collect_params(&member->as.callable, locals)) {
            return false;
        }
        if (!local_list_push(locals,
                             FENG_LSP_LOCAL_SELF,
                             slice_from_cstr("self"),
                             NULL,
                             NULL,
                             decl)) {
            return false;
        }
        return member->as.callable.body != NULL
                   ? collect_block_locals(member->as.callable.body, offset, locals)
                   : true;
    }
    if (decl != NULL && decl->kind == FENG_DECL_FUNCTION) {
        if (!callable_collect_params(&decl->as.function_decl, locals)) {
            return false;
        }
        return decl->as.function_decl.body != NULL
                   ? collect_block_locals(decl->as.function_decl.body, offset, locals)
                   : true;
    }
    return true;
}

static bool block_contains_offset_for_completion(const char *source,
                                                 const FengBlock *block,
                                                 size_t offset) {
    return block != NULL && offset >= block->token.offset && offset <= block_end_for_source(source, block);
}

static bool collect_stmt_locals_for_completion(const char *source,
                                               const FengStmt *stmt,
                                               size_t offset,
                                               FengLspLocalList *locals);

static bool collect_block_locals_for_completion(const char *source,
                                                const FengBlock *block,
                                                size_t offset,
                                                FengLspLocalList *locals) {
    size_t index;

    if (!block_contains_offset_for_completion(source, block, offset)) {
        return true;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        const FengStmt *stmt = block->statements[index];

        if (offset < stmt->token.offset) {
            break;
        }
        if (offset <= stmt_end_for_source(source, stmt)) {
            return collect_stmt_locals_for_completion(source, stmt, offset, locals);
        }
        if (stmt->kind == FENG_STMT_BINDING &&
            !local_list_push(locals,
                             FENG_LSP_LOCAL_BINDING,
                             stmt->as.binding.name,
                             NULL,
                             &stmt->as.binding,
                             NULL)) {
            return false;
        }
    }
    return true;
}

static bool collect_stmt_locals_for_completion(const char *source,
                                               const FengStmt *stmt,
                                               size_t offset,
                                               FengLspLocalList *locals) {
    size_t index;

    if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end_for_source(source, stmt)) {
        return true;
    }
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return collect_block_locals_for_completion(source, stmt->as.block, offset, locals);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (offset <= expr_end(stmt->as.if_stmt.clauses[index].condition)) {
                    return true;
                }
                if (block_contains_offset_for_completion(source, stmt->as.if_stmt.clauses[index].block, offset)) {
                    return collect_block_locals_for_completion(source,
                                                               stmt->as.if_stmt.clauses[index].block,
                                                               offset,
                                                               locals);
                }
            }
            return block_contains_offset_for_completion(source, stmt->as.if_stmt.else_block, offset)
                       ? collect_block_locals_for_completion(source, stmt->as.if_stmt.else_block, offset, locals)
                       : true;
        case FENG_STMT_MATCH:
            if (stmt->as.match_stmt.target != NULL && offset <= expr_end(stmt->as.match_stmt.target)) {
                return true;
            }
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (block_contains_offset_for_completion(source, stmt->as.match_stmt.branches[index].body, offset)) {
                    return collect_block_locals_for_completion(source,
                                                               stmt->as.match_stmt.branches[index].body,
                                                               offset,
                                                               locals);
                }
            }
            return block_contains_offset_for_completion(source, stmt->as.match_stmt.else_block, offset)
                       ? collect_block_locals_for_completion(source, stmt->as.match_stmt.else_block, offset, locals)
                       : true;
        case FENG_STMT_WHILE:
            if (stmt->as.while_stmt.condition != NULL && offset <= expr_end(stmt->as.while_stmt.condition)) {
                return true;
            }
            return collect_block_locals_for_completion(source, stmt->as.while_stmt.body, offset, locals);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                if (stmt->as.for_stmt.iter_expr != NULL && offset <= expr_end(stmt->as.for_stmt.iter_expr)) {
                    return true;
                }
                if (!local_list_push(locals,
                                     FENG_LSP_LOCAL_BINDING,
                                     stmt->as.for_stmt.iter_binding.name,
                                     NULL,
                                     &stmt->as.for_stmt.iter_binding,
                                     NULL)) {
                    return false;
                }
                return collect_block_locals_for_completion(source, stmt->as.for_stmt.body, offset, locals);
            }
            if (stmt->as.for_stmt.init != NULL && offset <= stmt_end_for_source(source, stmt->as.for_stmt.init)) {
                return collect_stmt_locals_for_completion(source, stmt->as.for_stmt.init, offset, locals);
            }
            if (stmt->as.for_stmt.init != NULL && stmt->as.for_stmt.init->kind == FENG_STMT_BINDING &&
                !local_list_push(locals,
                                 FENG_LSP_LOCAL_BINDING,
                                 stmt->as.for_stmt.init->as.binding.name,
                                 NULL,
                                 &stmt->as.for_stmt.init->as.binding,
                                 NULL)) {
                return false;
            }
            if (stmt->as.for_stmt.condition != NULL && offset <= expr_end(stmt->as.for_stmt.condition)) {
                return true;
            }
            if (stmt->as.for_stmt.update != NULL && offset <= stmt_end_for_source(source, stmt->as.for_stmt.update)) {
                return true;
            }
            return collect_block_locals_for_completion(source, stmt->as.for_stmt.body, offset, locals);
        default:
            return true;
    }
}

static const FengDecl *find_enclosing_decl_for_completion(const char *source,
                                                          const FengProgram *program,
                                                          size_t offset,
                                                          const FengTypeMember **out_member) {
    size_t decl_index;

    *out_member = NULL;
    if (program == NULL) {
        return NULL;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        size_t member_index;

        if (offset < decl->token.offset || offset > decl_end_for_source(source, decl)) {
            continue;
        }
        if (decl->kind == FENG_DECL_TYPE) {
            for (member_index = 0U; member_index < decl->as.type_decl.member_count; ++member_index) {
                if (offset >= decl->as.type_decl.members[member_index]->token.offset &&
                    offset <= member_end_for_source(source, decl->as.type_decl.members[member_index])) {
                    *out_member = decl->as.type_decl.members[member_index];
                    return decl;
                }
            }
        } else if (decl->kind == FENG_DECL_SPEC && decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
            for (member_index = 0U; member_index < decl->as.spec_decl.as.object.member_count; ++member_index) {
                if (offset >= decl->as.spec_decl.as.object.members[member_index]->token.offset &&
                    offset <= member_end_for_source(source, decl->as.spec_decl.as.object.members[member_index])) {
                    *out_member = decl->as.spec_decl.as.object.members[member_index];
                    return decl;
                }
            }
        } else if (decl->kind == FENG_DECL_FIT) {
            for (member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                if (offset >= decl->as.fit_decl.members[member_index]->token.offset &&
                    offset <= member_end_for_source(source, decl->as.fit_decl.members[member_index])) {
                    *out_member = decl->as.fit_decl.members[member_index];
                    return decl;
                }
            }
        }
        return decl;
    }
    return NULL;
}

static bool collect_visible_locals_for_completion(const char *source,
                                                  const FengDecl *decl,
                                                  const FengTypeMember *member,
                                                  size_t offset,
                                                  FengLspLocalList *locals) {
    if (member != NULL && member->kind != FENG_TYPE_MEMBER_FIELD) {
        if (!callable_collect_params(&member->as.callable, locals)) {
            return false;
        }
        if (!local_list_push(locals,
                             FENG_LSP_LOCAL_SELF,
                             slice_from_cstr("self"),
                             NULL,
                             NULL,
                             decl)) {
            return false;
        }
        return collect_block_locals_for_completion(source, member->as.callable.body, offset, locals);
    }
    if (decl != NULL && decl->kind == FENG_DECL_FUNCTION) {
        if (!callable_collect_params(&decl->as.function_decl, locals)) {
            return false;
        }
        return collect_block_locals_for_completion(source, decl->as.function_decl.body, offset, locals);
    }
    return true;
}

static FengSlice decl_name(const FengDecl *decl) {
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return decl->as.binding.name;
        case FENG_DECL_ENUM:
            return decl->as.enum_decl.name;
        case FENG_DECL_TYPE:
            return decl->as.type_decl.name;
        case FENG_DECL_SPEC:
            return decl->as.spec_decl.name;
        case FENG_DECL_FUNCTION:
            return decl->as.function_decl.name;
        case FENG_DECL_FIT:
            return slice_from_cstr("fit");
    }
    return (FengSlice){0};
}

static const FengDecl *find_module_decl_by_name(const FengSemanticModule *module,
                                                FengSlice name,
                                                bool values_only,
                                                bool types_only,
                                                bool public_only) {
    size_t program_index;
    size_t decl_index;

    if (module == NULL) {
        return NULL;
    }
    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        const FengProgram *program = module->programs[program_index];

        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];
            bool is_value = decl->kind == FENG_DECL_FUNCTION || decl->kind == FENG_DECL_GLOBAL_BINDING;
            bool is_type = decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
                           decl->kind == FENG_DECL_SPEC;

            if (public_only && decl->visibility != FENG_VISIBILITY_PUBLIC) {
                continue;
            }
            if (values_only && !is_value) {
                continue;
            }
            if (types_only && !is_type) {
                continue;
            }
            if (decl->kind == FENG_DECL_FIT) {
                continue;
            }
            if (slice_equals(decl_name(decl), name)) {
                return decl;
            }
        }
    }
    return NULL;
}

static const FengDecl *find_program_decl_by_name(const FengProgram *program,
                                                 FengSlice name,
                                                 bool values_only,
                                                 bool types_only,
                                                 bool public_only) {
    size_t decl_index;

    if (program == NULL) {
        return NULL;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        bool is_value = decl->kind == FENG_DECL_FUNCTION || decl->kind == FENG_DECL_GLOBAL_BINDING;
        bool is_type = decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
                       decl->kind == FENG_DECL_SPEC;

        if (public_only && decl->visibility != FENG_VISIBILITY_PUBLIC) {
            continue;
        }
        if (values_only && !is_value) {
            continue;
        }
        if (types_only && !is_type) {
            continue;
        }
        if (decl->kind == FENG_DECL_FIT) {
            continue;
        }
        if (slice_equals(decl_name(decl), name)) {
            return decl;
        }
    }
    return NULL;
}

static bool program_module_matches(const FengProgram *program,
                                   const FengSlice *segments,
                                   size_t segment_count) {
    size_t index;

    if (program == NULL || program->module_segment_count != segment_count) {
        return false;
    }
    for (index = 0U; index < segment_count; ++index) {
        if (!slice_equals(program->module_segments[index], segments[index])) {
            return false;
        }
    }
    return true;
}

static const FengDecl *find_loaded_module_decl_by_name(const FengLspAnalysisSession *session,
                                                       const FengSlice *segments,
                                                       size_t segment_count,
                                                       FengSlice name,
                                                       bool values_only,
                                                       bool types_only,
                                                       bool public_only) {
    size_t source_index;

    if (session == NULL || segments == NULL || segment_count == 0U) {
        return NULL;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        const FengProgram *program = session->sources[source_index].program;

        if (program_module_matches(program, segments, segment_count)) {
            const FengDecl *decl = find_program_decl_by_name(program,
                                                            name,
                                                            values_only,
                                                            types_only,
                                                            public_only);
            if (decl != NULL) {
                return decl;
            }
        }
    }
    return NULL;
}

static bool symbol_decl_is_value(const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind = feng_symbol_decl_kind(decl);

    return kind == FENG_SYMBOL_DECL_KIND_BINDING || kind == FENG_SYMBOL_DECL_KIND_FUNCTION;
}

static bool symbol_decl_is_type(const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind = feng_symbol_decl_kind(decl);

    return kind == FENG_SYMBOL_DECL_KIND_TYPE || kind == FENG_SYMBOL_DECL_KIND_ENUM ||
           kind == FENG_SYMBOL_DECL_KIND_SPEC;
}

static bool symbol_decl_is_completion_decl(const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind;

    if (decl == NULL) {
        return false;
    }
    kind = feng_symbol_decl_kind(decl);
    return kind == FENG_SYMBOL_DECL_KIND_BINDING ||
           kind == FENG_SYMBOL_DECL_KIND_FUNCTION ||
           kind == FENG_SYMBOL_DECL_KIND_TYPE ||
            kind == FENG_SYMBOL_DECL_KIND_ENUM ||
           kind == FENG_SYMBOL_DECL_KIND_SPEC;
}

static bool symbol_decl_is_instance_member(const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind;

    if (decl == NULL) {
        return false;
    }
    kind = feng_symbol_decl_kind(decl);
    return kind == FENG_SYMBOL_DECL_KIND_FIELD ||
            kind == FENG_SYMBOL_DECL_KIND_ENUM_ITEM ||
           kind == FENG_SYMBOL_DECL_KIND_METHOD ||
           kind == FENG_SYMBOL_DECL_KIND_CONSTRUCTOR ||
           kind == FENG_SYMBOL_DECL_KIND_FINALIZER;
}

static bool symbol_decl_matches_ast_decl_kind(const FengSymbolDeclView *decl,
                                              const FengDecl *ast_decl) {
    if (decl == NULL || ast_decl == NULL) {
        return false;
    }
    switch (ast_decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_BINDING;
        case FENG_DECL_ENUM:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_ENUM;
        case FENG_DECL_TYPE:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_TYPE;
        case FENG_DECL_SPEC:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_SPEC;
        case FENG_DECL_FIT:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FIT;
        case FENG_DECL_FUNCTION:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FUNCTION;
    }
    return false;
}

static bool symbol_decl_matches_ast_member_kind(const FengSymbolDeclView *decl,
                                                const FengTypeMember *member) {
    if (decl == NULL || member == NULL) {
        return false;
    }
    switch (member->kind) {
        case FENG_TYPE_MEMBER_FIELD:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FIELD;
        case FENG_TYPE_MEMBER_METHOD:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_METHOD;
        case FENG_TYPE_MEMBER_CONSTRUCTOR:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_CONSTRUCTOR;
        case FENG_TYPE_MEMBER_FINALIZER:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FINALIZER;
    }
    return false;
}

static const FengSymbolDeclView *find_symbol_module_decl_by_name(const FengSymbolImportedModule *module,
                                                                 FengSlice name,
                                                                 bool values_only,
                                                                 bool types_only,
                                                                 bool public_only) {
    size_t count;
    size_t index;

    if (module == NULL) {
        return NULL;
    }
    count = public_only ? feng_symbol_module_public_decl_count(module)
                        : feng_symbol_module_decl_count(module);
    for (index = 0U; index < count; ++index) {
        const FengSymbolDeclView *decl = public_only
                                             ? feng_symbol_module_public_decl_at(module, index)
                                             : feng_symbol_module_decl_at(module, index);

        if (decl == NULL || feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FIT) {
            continue;
        }
        if (public_only && feng_symbol_decl_visibility(decl) != FENG_VISIBILITY_PUBLIC) {
            continue;
        }
        if (values_only && !symbol_decl_is_value(decl)) {
            continue;
        }
        if (types_only && !symbol_decl_is_type(decl)) {
            continue;
        }
        if (slice_equals(feng_symbol_decl_name(decl), name)) {
            return decl;
        }
    }
    return NULL;
}

static const FengSymbolDeclView *find_symbol_decl_member_by_name(const FengSymbolDeclView *owner,
                                                                 FengSlice name,
                                                                 bool public_only) {
    size_t count;
    size_t index;

    if (owner == NULL) {
        return NULL;
    }
    count = feng_symbol_decl_member_count(owner);
    for (index = 0U; index < count; ++index) {
        const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner, index);

        if (member != NULL &&
            symbol_decl_is_instance_member(member) &&
            (!public_only || feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PUBLIC) &&
            slice_equals(feng_symbol_decl_name(member), name)) {
            return member;
        }
    }
    return NULL;
}

static const FengSymbolImportedModule *find_symbol_alias_module(const FengSymbolProvider *provider,
                                                                const FengProgram *program,
                                                                FengSlice alias_name) {
    size_t index;

    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];

        if (use_decl->has_alias && slice_equals(use_decl->alias, alias_name)) {
            return feng_symbol_provider_find_module(provider,
                                                    use_decl->segments,
                                                    use_decl->segment_count);
        }
    }
    return NULL;
}

static const FengSymbolDeclView *match_ast_decl_to_symbol(const FengSymbolImportedModule *module,
                                                          const FengProgram *program,
                                                          const FengDecl *decl) {
    const FengSymbolDeclView *fallback = NULL;
    FengSlice ast_name = decl_name(decl);
    size_t index;
    size_t count;

    if (module == NULL || program == NULL || decl == NULL) {
        return NULL;
    }
    count = feng_symbol_module_decl_count(module);
    for (index = 0U; index < count; ++index) {
        const FengSymbolDeclView *candidate = feng_symbol_module_decl_at(module, index);
        FengSlice candidate_path;
        FengToken candidate_token;

        if (!symbol_decl_matches_ast_decl_kind(candidate, decl)) {
            continue;
        }
        if (decl->kind != FENG_DECL_FIT && !slice_equals(feng_symbol_decl_name(candidate), ast_name)) {
            continue;
        }
        if (decl->kind == FENG_DECL_FUNCTION &&
            feng_symbol_decl_param_count(candidate) != decl->as.function_decl.param_count) {
            continue;
        }
        candidate_path = feng_symbol_decl_path(candidate);
        candidate_token = feng_symbol_decl_token(candidate);
        if (slice_equals_cstr(candidate_path, program->path) &&
            candidate_token.line == decl->token.line &&
            candidate_token.column == decl->token.column) {
            return candidate;
        }
        if (fallback == NULL) {
            fallback = candidate;
        }
    }
    return fallback;
}

static const FengSymbolDeclView *match_ast_member_to_symbol(const FengSymbolDeclView *owner,
                                                            const char *path,
                                                            const FengTypeMember *member) {
    const FengSymbolDeclView *fallback = NULL;
    FengSlice ast_name = member->kind == FENG_TYPE_MEMBER_FIELD
                             ? member->as.field.name
                             : member->as.callable.name;
    size_t index;
    size_t count;

    if (owner == NULL || member == NULL) {
        return NULL;
    }
    count = feng_symbol_decl_member_count(owner);
    for (index = 0U; index < count; ++index) {
        const FengSymbolDeclView *candidate = feng_symbol_decl_member_at(owner, index);
        FengSlice candidate_path;
        FengToken candidate_token;

        if (!symbol_decl_matches_ast_member_kind(candidate, member) ||
            !slice_equals(feng_symbol_decl_name(candidate), ast_name)) {
            continue;
        }
        if (member->kind != FENG_TYPE_MEMBER_FIELD &&
            feng_symbol_decl_param_count(candidate) != member->as.callable.param_count) {
            continue;
        }
        candidate_path = feng_symbol_decl_path(candidate);
        candidate_token = feng_symbol_decl_token(candidate);
        if (path != NULL && slice_equals_cstr(candidate_path, path) &&
            candidate_token.line == member->token.line &&
            candidate_token.column == member->token.column) {
            return candidate;
        }
        if (fallback == NULL) {
            fallback = candidate;
        }
    }
    return fallback;
}

static const FengSymbolDeclView *resolve_symbol_named_type_ref(const FengSymbolProvider *provider,
                                                               const FengSymbolImportedModule *current_module,
                                                               const FengProgram *program,
                                                               const FengTypeRef *type_ref) {
    size_t index;
    FengSlice name;

    if (provider == NULL || program == NULL || type_ref == NULL ||
        type_ref->kind != FENG_TYPE_REF_NAMED || type_ref->as.named.segment_count == 0U) {
        return NULL;
    }
    name = type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];
    if (type_ref->as.named.segment_count == 1U) {
        if (slice_equals_cstr(name, "int") || slice_equals_cstr(name, "long") ||
            slice_equals_cstr(name, "byte") || slice_equals_cstr(name, "float") ||
            slice_equals_cstr(name, "double") || slice_equals_cstr(name, "bool") ||
            slice_equals_cstr(name, "string") || slice_equals_cstr(name, "void")) {
            return NULL;
        }
        if (current_module != NULL) {
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(current_module,
                                                                             name,
                                                                             false,
                                                                             true,
                                                                             false);
            if (decl != NULL) {
                return decl;
            }
        }
        for (index = 0U; index < program->use_count; ++index) {
            const FengUseDecl *use_decl = &program->uses[index];
            const FengSymbolImportedModule *module;

            if (use_decl->has_alias) {
                continue;
            }
            module = feng_symbol_provider_find_module(provider,
                                                      use_decl->segments,
                                                      use_decl->segment_count);
            if (module != NULL) {
                const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(module,
                                                                                 name,
                                                                                 false,
                                                                                 true,
                                                                                 true);
                if (decl != NULL) {
                    return decl;
                }
            }
        }
        return NULL;
    }
    if (type_ref->as.named.segment_count == 2U) {
        const FengSymbolImportedModule *alias_module = find_symbol_alias_module(provider,
                                                                                program,
                                                                                type_ref->as.named.segments[0]);
        if (alias_module != NULL) {
            return find_symbol_module_decl_by_name(alias_module,
                                                   type_ref->as.named.segments[1],
                                                   false,
                                                   true,
                                                   true);
        }
    }
    return find_symbol_module_decl_by_name(feng_symbol_provider_find_module(provider,
                                                                            type_ref->as.named.segments,
                                                                            type_ref->as.named.segment_count - 1U),
                                           name,
                                           false,
                                           true,
                                           true);
}

static const FengSymbolDeclView *resolve_symbol_type_view(const FengSymbolProvider *provider,
                                                          const FengSymbolImportedModule *current_module,
                                                          const FengProgram *program,
                                                          const FengSymbolTypeView *type) {
    FengSymbolTypeKind kind;

    if (provider == NULL || program == NULL || type == NULL) {
        return NULL;
    }
    kind = feng_symbol_type_kind(type);
    if (kind == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
        return NULL;
    }
    if (kind == FENG_SYMBOL_TYPE_KIND_POINTER || kind == FENG_SYMBOL_TYPE_KIND_ARRAY) {
        return resolve_symbol_type_view(provider,
                                        current_module,
                                        program,
                                        feng_symbol_type_inner(type));
    }
    if (kind == FENG_SYMBOL_TYPE_KIND_NAMED) {
        size_t segment_count = feng_symbol_type_segment_count(type);
        FengSlice name;
        size_t index;

        if (segment_count == 0U) {
            return NULL;
        }
        name = feng_symbol_type_segment_at(type, segment_count - 1U);
        if (segment_count == 1U) {
            if (current_module != NULL) {
                const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(current_module,
                                                                                 name,
                                                                                 false,
                                                                                 true,
                                                                                 false);
                if (decl != NULL) {
                    return decl;
                }
            }
            for (index = 0U; index < program->use_count; ++index) {
                const FengUseDecl *use_decl = &program->uses[index];
                const FengSymbolImportedModule *module;

                if (use_decl->has_alias) {
                    continue;
                }
                module = feng_symbol_provider_find_module(provider,
                                                          use_decl->segments,
                                                          use_decl->segment_count);
                if (module != NULL) {
                    const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(module,
                                                                                     name,
                                                                                     false,
                                                                                     true,
                                                                                     true);
                    if (decl != NULL) {
                        return decl;
                    }
                }
            }
            return NULL;
        }
        if (segment_count == 2U) {
            const FengSymbolImportedModule *alias_module = find_symbol_alias_module(provider,
                                                                                    program,
                                                                                    feng_symbol_type_segment_at(type, 0U));
            if (alias_module != NULL) {
                return find_symbol_module_decl_by_name(alias_module,
                                                       feng_symbol_type_segment_at(type, 1U),
                                                       false,
                                                       true,
                                                       true);
            }
        }
        {
            FengSlice *segments = (FengSlice *)calloc(segment_count - 1U, sizeof(*segments));
            const FengSymbolImportedModule *module;
            const FengSymbolDeclView *decl;

            if (segments == NULL) {
                return NULL;
            }
            for (index = 0U; index + 1U < segment_count; ++index) {
                segments[index] = feng_symbol_type_segment_at(type, index);
            }
            module = feng_symbol_provider_find_module(provider, segments, segment_count - 1U);
            free(segments);
            decl = find_symbol_module_decl_by_name(module, name, false, true, true);
            if (decl != NULL) {
                return decl;
            }
        }
    }
    return NULL;
}

static const FengSymbolDeclView *resolve_symbol_value_name(const FengSymbolProvider *provider,
                                                           const FengSymbolImportedModule *current_module,
                                                           const FengProgram *program,
                                                           FengSlice name) {
    size_t index;

    if (current_module != NULL) {
        const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(current_module,
                                                                         name,
                                                                         true,
                                                                         false,
                                                                         false);
        if (decl != NULL) {
            return decl;
        }
    }
    if (program == NULL) {
        return NULL;
    }
    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSymbolImportedModule *module;

        if (use_decl->has_alias) {
            continue;
        }
        module = feng_symbol_provider_find_module(provider, use_decl->segments, use_decl->segment_count);
        if (module != NULL) {
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(module,
                                                                             name,
                                                                             true,
                                                                             false,
                                                                             true);
            if (decl != NULL) {
                return decl;
            }
        }
    }
    return NULL;
}

static const FengSymbolDeclView *resolve_symbol_type_name(const FengSymbolProvider *provider,
                                                          const FengSymbolImportedModule *current_module,
                                                          const FengProgram *program,
                                                          FengSlice name) {
    size_t index;

    if (current_module != NULL) {
        const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(current_module,
                                                                         name,
                                                                         false,
                                                                         true,
                                                                         false);
        if (decl != NULL) {
            return decl;
        }
    }
    if (program == NULL) {
        return NULL;
    }
    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSymbolImportedModule *module;

        if (use_decl->has_alias) {
            continue;
        }
        module = feng_symbol_provider_find_module(provider, use_decl->segments, use_decl->segment_count);
        if (module != NULL) {
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name(module,
                                                                             name,
                                                                             false,
                                                                             true,
                                                                             true);
            if (decl != NULL) {
                return decl;
            }
        }
    }
    return NULL;
}

static const FengSymbolDeclView *resolve_symbol_type_constructor_expr(const FengLspCacheQueryContext *context,
                                                                      const FengExpr *expr) {
    if (context == NULL || expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        return resolve_symbol_type_name(context->provider,
                                        context->current_module,
                                        context->program,
                                        expr->as.identifier);
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL &&
        expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
        const FengSymbolImportedModule *alias_module = find_symbol_alias_module(context->provider,
                                                                                context->program,
                                                                                expr->as.member.object->as.identifier);
        if (alias_module != NULL) {
            return find_symbol_module_decl_by_name(alias_module,
                                                   expr->as.member.member,
                                                   false,
                                                   true,
                                                   true);
        }
    }
    return NULL;
}

static const FengSymbolDeclView *resolve_symbol_owner_decl_from_initializer_expr(const FengLspCacheQueryContext *context,
                                                                                 const FengExpr *expr) {
    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_OBJECT_LITERAL) {
        return resolve_symbol_type_constructor_expr(context, expr->as.object_literal.target);
    }
    if (expr->kind == FENG_EXPR_CALL) {
        return resolve_symbol_type_constructor_expr(context, expr->as.call.callee);
    }
    return NULL;
}

static const FengSymbolDeclView *resolve_symbol_owner_decl_from_object_expr(const FengLspCacheQueryContext *context,
                                                                            const FengExpr *object,
                                                                            const FengLspLocalList *locals) {
    const FengSymbolDeclView *decl;

    if (context == NULL || object == NULL) {
        return NULL;
    }
    if (object->kind == FENG_EXPR_SELF) {
        const FengLspLocal *self_local = find_local(locals, slice_from_cstr("self"));
        if (self_local != NULL && self_local->self_owner_decl != NULL) {
            if (self_local->self_owner_decl->kind == FENG_DECL_FIT) {
                const FengSymbolDeclView *target_decl = resolve_symbol_named_type_ref(context->provider,
                                                                                      context->current_module,
                                                                                      context->program,
                                                                                      self_local->self_owner_decl->as.fit_decl.target);

                if (target_decl != NULL) {
                    return target_decl;
                }
            }
            return match_ast_decl_to_symbol(context->current_module,
                                            context->program,
                                            self_local->self_owner_decl);
        }
        return NULL;
    }
    if (object->kind != FENG_EXPR_IDENTIFIER) {
        return NULL;
    }
    {
        const FengLspLocal *local = find_local(locals, object->as.identifier);

        if (local != NULL) {
            if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
                return resolve_symbol_named_type_ref(context->provider,
                                                     context->current_module,
                                                     context->program,
                                                     local->parameter->type);
            }
            if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
                return local->binding->type != NULL
                           ? resolve_symbol_named_type_ref(context->provider,
                                                           context->current_module,
                                                           context->program,
                                                           local->binding->type)
                           : resolve_symbol_owner_decl_from_initializer_expr(context, local->binding->initializer);
            }
            if (local->kind == FENG_LSP_LOCAL_SELF) {
                if (local->self_owner_decl != NULL && local->self_owner_decl->kind == FENG_DECL_FIT) {
                    const FengSymbolDeclView *target_decl = resolve_symbol_named_type_ref(context->provider,
                                                                                          context->current_module,
                                                                                          context->program,
                                                                                          local->self_owner_decl->as.fit_decl.target);

                    if (target_decl != NULL) {
                        return target_decl;
                    }
                }
                return match_ast_decl_to_symbol(context->current_module,
                                                context->program,
                                                local->self_owner_decl);
            }
        }
    }
    decl = resolve_symbol_value_name(context->provider,
                                     context->current_module,
                                     context->program,
                                     object->as.identifier);
    if (decl != NULL) {
        if (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_BINDING) {
            return resolve_symbol_type_view(context->provider,
                                            context->current_module,
                                            context->program,
                                            feng_symbol_decl_value_type(decl));
        }
        if (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_TYPE ||
            feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_ENUM ||
            feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_SPEC) {
            return decl;
        }
    }
    decl = resolve_symbol_type_name(context->provider,
                                    context->current_module,
                                    context->program,
                                    object->as.identifier);
    if (decl != NULL &&
        (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_TYPE ||
         feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_ENUM ||
         feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_SPEC)) {
        return decl;
    }
    return NULL;
}

static const FengSemanticModule *find_alias_module(const FengLspAnalysisSession *session,
                                                   const FengProgram *program,
                                                   FengSlice alias_name) {
    size_t index;

    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];

        if (use_decl->has_alias && slice_equals(use_decl->alias, alias_name)) {
            return find_module_by_segments(session->analysis,
                                           use_decl->segments,
                                           use_decl->segment_count);
        }
    }
    return NULL;
}

static const FengDecl *resolve_named_type_ref(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengTypeRef *type_ref) {
    const FengSemanticModule *program_module;
    size_t index;
    FengSlice name;

    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED || type_ref->as.named.segment_count == 0U) {
        return NULL;
    }
    name = type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];
    if (type_ref->as.named.segment_count == 1U) {
        if (slice_equals_cstr(name, "int") || slice_equals_cstr(name, "long") ||
            slice_equals_cstr(name, "byte") || slice_equals_cstr(name, "float") ||
            slice_equals_cstr(name, "double") || slice_equals_cstr(name, "bool") ||
            slice_equals_cstr(name, "string") || slice_equals_cstr(name, "void")) {
            return NULL;
        }
        program_module = find_program_module(session, program);
        if (program_module != NULL) {
            const FengDecl *decl = find_module_decl_by_name(program_module, name, false, true, false);

            if (decl != NULL) {
                return decl;
            }
        }
        {
            const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                                  program->module_segments,
                                                                  program->module_segment_count,
                                                                  name,
                                                                  false,
                                                                  true,
                                                                  false);
            if (decl != NULL) {
                return decl;
            }
        }
        for (index = 0U; index < program->use_count; ++index) {
            const FengUseDecl *use_decl = &program->uses[index];
            const FengSemanticModule *module;

            if (use_decl->has_alias) {
                continue;
            }
            module = find_module_by_segments(session->analysis, use_decl->segments, use_decl->segment_count);
            if (module != NULL) {
                const FengDecl *decl = find_module_decl_by_name(module, name, false, true, true);

                if (decl != NULL) {
                    return decl;
                }
            }
            {
                const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                                      use_decl->segments,
                                                                      use_decl->segment_count,
                                                                      name,
                                                                      false,
                                                                      true,
                                                                      true);
                if (decl != NULL) {
                    return decl;
                }
            }
        }
        return NULL;
    }
    if (type_ref->as.named.segment_count == 2U) {
        const FengSemanticModule *alias_module = find_alias_module(session,
                                                                   program,
                                                                   type_ref->as.named.segments[0]);
        if (alias_module != NULL) {
            return find_module_decl_by_name(alias_module,
                                            type_ref->as.named.segments[1],
                                            false,
                                            true,
                                            true);
        }
        for (index = 0U; index < program->use_count; ++index) {
            const FengUseDecl *use_decl = &program->uses[index];

            if (use_decl->has_alias && slice_equals(use_decl->alias, type_ref->as.named.segments[0])) {
                const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                                      use_decl->segments,
                                                                      use_decl->segment_count,
                                                                      type_ref->as.named.segments[1],
                                                                      false,
                                                                      true,
                                                                      true);
                if (decl != NULL) {
                    return decl;
                }
            }
        }
    }
    {
        const FengDecl *decl = find_module_decl_by_name(find_module_by_segments(session->analysis,
                                                                                type_ref->as.named.segments,
                                                                                type_ref->as.named.segment_count - 1U),
                                                        name,
                                                        false,
                                                        true,
                                                        true);
        if (decl != NULL) {
            return decl;
        }
    }
    return find_loaded_module_decl_by_name(session,
                                           type_ref->as.named.segments,
                                           type_ref->as.named.segment_count - 1U,
                                           name,
                                           false,
                                           true,
                                           true);
}

static const FengTypeMember *find_member_by_name(const FengDecl *owner_decl, FengSlice name) {
    size_t index;

    if (owner_decl == NULL) {
        return NULL;
    }
    if (owner_decl->kind == FENG_DECL_TYPE) {
        for (index = 0U; index < owner_decl->as.type_decl.member_count; ++index) {
            const FengTypeMember *member = owner_decl->as.type_decl.members[index];
            FengSlice member_name = member->kind == FENG_TYPE_MEMBER_FIELD
                                        ? member->as.field.name
                                        : member->as.callable.name;
            if (slice_equals(member_name, name)) {
                return member;
            }
        }
    }
    if (owner_decl->kind == FENG_DECL_SPEC && owner_decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
        for (index = 0U; index < owner_decl->as.spec_decl.as.object.member_count; ++index) {
            const FengTypeMember *member = owner_decl->as.spec_decl.as.object.members[index];
            FengSlice member_name = member->kind == FENG_TYPE_MEMBER_FIELD
                                        ? member->as.field.name
                                        : member->as.callable.name;
            if (slice_equals(member_name, name)) {
                return member;
            }
        }
    }
    return NULL;
}

static const FengDecl *resolve_value_name(const FengLspAnalysisSession *session,
                                          const FengProgram *program,
                                          FengSlice name) {
    const FengSemanticModule *program_module = find_program_module(session, program);
    size_t index;

    if (program_module != NULL) {
        const FengDecl *decl = find_module_decl_by_name(program_module, name, true, false, false);
        if (decl != NULL) {
            return decl;
        }
    }
    {
        const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                              program->module_segments,
                                                              program->module_segment_count,
                                                              name,
                                                              true,
                                                              false,
                                                              false);
        if (decl != NULL) {
            return decl;
        }
    }
    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSemanticModule *module;

        if (use_decl->has_alias) {
            continue;
        }
        module = find_module_by_segments(session->analysis, use_decl->segments, use_decl->segment_count);
        if (module != NULL) {
            const FengDecl *decl = find_module_decl_by_name(module, name, true, false, true);
            if (decl != NULL) {
                return decl;
            }
        }
        {
            const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                                  use_decl->segments,
                                                                  use_decl->segment_count,
                                                                  name,
                                                                  true,
                                                                  false,
                                                                  true);
            if (decl != NULL) {
                return decl;
            }
        }
    }
    return NULL;
}

static const FengDecl *resolve_type_name(const FengLspAnalysisSession *session,
                                         const FengProgram *program,
                                         FengSlice name) {
    const FengSemanticModule *program_module = find_program_module(session, program);
    size_t index;

    if (program_module != NULL) {
        const FengDecl *decl = find_module_decl_by_name(program_module, name, false, true, false);
        if (decl != NULL) {
            return decl;
        }
    }
    {
        const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                              program->module_segments,
                                                              program->module_segment_count,
                                                              name,
                                                              false,
                                                              true,
                                                              false);
        if (decl != NULL) {
            return decl;
        }
    }
    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSemanticModule *module;

        if (use_decl->has_alias) {
            continue;
        }
        module = find_module_by_segments(session->analysis, use_decl->segments, use_decl->segment_count);
        if (module != NULL) {
            const FengDecl *decl = find_module_decl_by_name(module, name, false, true, true);
            if (decl != NULL) {
                return decl;
            }
        }
        {
            const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                                  use_decl->segments,
                                                                  use_decl->segment_count,
                                                                  name,
                                                                  false,
                                                                  true,
                                                                  true);
            if (decl != NULL) {
                return decl;
            }
        }
    }
    return NULL;
}

static const FengDecl *owner_decl_from_type_fact(const FengLspAnalysisSession *session,
                                                 const FengProgram *program,
                                                 const FengExpr *expr) {
    const FengSemanticTypeFact *fact;

    if (session->analysis == NULL || expr == NULL) {
        return NULL;
    }
    fact = feng_semantic_lookup_type_fact(session->analysis, expr);
    if (fact == NULL) {
        return NULL;
    }
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_DECL) {
        return fact->type_decl;
    }
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
        return resolve_named_type_ref(session, program, fact->type_ref);
    }
    return NULL;
}

static bool append_member_completion_item(FengLspString *json,
                                          bool *first,
                                          const FengTypeMember *member,
                                          const char *owner_name);

static const char *builtin_name_for_single_segment_type_ref(const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count != 1U || type_ref->as.named.type_arg_count != 0U) {
        return NULL;
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "int") ||
        slice_equals_cstr(type_ref->as.named.segments[0], "i32")) {
        return "i32";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "long") ||
        slice_equals_cstr(type_ref->as.named.segments[0], "i64")) {
        return "i64";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "byte") ||
        slice_equals_cstr(type_ref->as.named.segments[0], "u8")) {
        return "u8";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "float") ||
        slice_equals_cstr(type_ref->as.named.segments[0], "f32")) {
        return "f32";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "double") ||
        slice_equals_cstr(type_ref->as.named.segments[0], "f64")) {
        return "f64";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "i8")) {
        return "i8";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "i16")) {
        return "i16";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "u16")) {
        return "u16";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "u32")) {
        return "u32";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "u64")) {
        return "u64";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "bool")) {
        return "bool";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "string")) {
        return "string";
    }
    if (slice_equals_cstr(type_ref->as.named.segments[0], "void")) {
        return "void";
    }
    return NULL;
}

static bool builtin_name_matches_type_ref(const FengTypeRef *type_ref, FengSlice builtin_name) {
    const char *canonical = builtin_name_for_single_segment_type_ref(type_ref);

    return canonical != NULL && slice_equals_cstr(builtin_name, canonical);
}

static bool owner_builtin_name_from_type_fact(const FengLspAnalysisSession *session,
                                              const void *site,
                                              FengSlice *out_name) {
    const FengSemanticTypeFact *fact;

    if (out_name == NULL) {
        return false;
    }
    out_name->data = NULL;
    out_name->length = 0U;
    if (session == NULL || session->analysis == NULL || site == NULL) {
        return false;
    }
    fact = feng_semantic_lookup_type_fact(session->analysis, site);
    if (fact == NULL) {
        return false;
    }
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_BUILTIN) {
        *out_name = fact->builtin_name;
        return true;
    }
    if (fact->kind == FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
        const char *builtin = builtin_name_for_single_segment_type_ref(fact->type_ref);

        if (builtin != NULL) {
            *out_name = slice_from_cstr(builtin);
            return true;
        }
    }
    return false;
}

static bool resolve_owner_builtin_name_from_object_expr(const FengLspAnalysisSession *session,
                                                        const FengProgram *program,
                                                        const FengExpr *object,
                                                        const FengLspLocalList *locals,
                                                        FengSlice *out_name) {
    const FengLspLocal *local;
    const FengDecl *decl;

    if (out_name == NULL) {
        return false;
    }
    out_name->data = NULL;
    out_name->length = 0U;
    if (object == NULL) {
        return false;
    }
    if (owner_builtin_name_from_type_fact(session, object, out_name)) {
        return true;
    }
    if (object->kind == FENG_EXPR_STRING) {
        *out_name = slice_from_cstr("string");
        return true;
    }
    if (object->kind == FENG_EXPR_INTEGER) {
        *out_name = slice_from_cstr("i32");
        return true;
    }
    if (object->kind == FENG_EXPR_FLOAT) {
        *out_name = slice_from_cstr("f64");
        return true;
    }
    if (object->kind == FENG_EXPR_BOOL) {
        *out_name = slice_from_cstr("bool");
        return true;
    }
    if (object->kind != FENG_EXPR_IDENTIFIER) {
        return false;
    }
    local = find_local(locals, object->as.identifier);
    if (local != NULL) {
        if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
            const char *builtin = builtin_name_for_single_segment_type_ref(local->parameter->type);

            if (builtin != NULL) {
                *out_name = slice_from_cstr(builtin);
                return true;
            }
        }
        if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
            if (owner_builtin_name_from_type_fact(session, local->binding, out_name)) {
                return true;
            }
            if (local->binding->type != NULL) {
                const char *builtin = builtin_name_for_single_segment_type_ref(local->binding->type);

                if (builtin != NULL) {
                    *out_name = slice_from_cstr(builtin);
                    return true;
                }
            }
        }
        if (local->kind == FENG_LSP_LOCAL_SELF && local->self_owner_decl != NULL &&
            local->self_owner_decl->kind == FENG_DECL_FIT) {
            const char *builtin = builtin_name_for_single_segment_type_ref(local->self_owner_decl->as.fit_decl.target);

            if (builtin != NULL) {
                *out_name = slice_from_cstr(builtin);
                return true;
            }
        }
    }
    decl = resolve_value_name(session, program, object->as.identifier);
    if (decl != NULL && decl->kind == FENG_DECL_GLOBAL_BINDING) {
        if (owner_builtin_name_from_type_fact(session, &decl->as.binding, out_name)) {
            return true;
        }
        if (decl->as.binding.type != NULL) {
            const char *builtin = builtin_name_for_single_segment_type_ref(decl->as.binding.type);

            if (builtin != NULL) {
                *out_name = slice_from_cstr(builtin);
                return true;
            }
        }
    }
    return false;
}

static bool fit_visible_from_program(const FengLspAnalysisSession *session,
                                    const FengProgram *program,
                                    const FengDecl *fit_decl) {
    const FengProgram *fit_program = NULL;
    const FengSemanticModule *fit_module;
    size_t index;

    if (session == NULL || program == NULL || fit_decl == NULL) {
        return false;
    }
    fit_module = find_decl_module(session, fit_decl, &fit_program);
    if (fit_module == NULL || fit_program == NULL) {
        return false;
    }
    if (fit_program == program) {
        return true;
    }
    if (fit_decl->visibility != FENG_VISIBILITY_PUBLIC) {
        return false;
    }
    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSemanticModule *module = find_module_by_segments(session->analysis,
                                                                   use_decl->segments,
                                                                   use_decl->segment_count);

        if (module == fit_module) {
            return true;
        }
    }
    return false;
}

/* Forward declarations — defined later, used here for filtering and dedup. */
typedef enum FengLspMemberFilter {
    FENG_LSP_MEMBER_FILTER_ALL = 0,
    FENG_LSP_MEMBER_FILTER_STATIC,
    FENG_LSP_MEMBER_FILTER_INSTANCE
} FengLspMemberFilter;

static bool member_passes_filter(const FengTypeMember *member, FengLspMemberFilter filter);
static bool symbol_member_passes_filter(const FengSymbolDeclView *member, FengLspMemberFilter filter);
static bool completion_json_contains_label(const FengLspString *json,
                                           FengSlice label,
                                           bool *contains);

static bool append_owner_fit_member_completion_items(FengLspString *json,
                                                     bool *first,
                                                     const FengLspAnalysisSession *session,
                                                     const FengProgram *program,
                                                     const FengDecl *owner_decl,
                                                     FengSlice owner_builtin_name,
                                                     FengLspMemberFilter filter) {
    size_t module_index;
    size_t program_index;
    size_t decl_index;

    if (session == NULL || program == NULL) {
        return true;
    }
    if (session->analysis == NULL) {
        for (program_index = 0U; program_index < session->source_count; ++program_index) {
            const FengProgram *fit_program = session->sources[program_index].program;

            if (fit_program == NULL ||
                !program_module_matches(fit_program,
                                        program->module_segments,
                                        program->module_segment_count)) {
                continue;
            }
            for (decl_index = 0U; decl_index < fit_program->declaration_count; ++decl_index) {
                const FengDecl *decl = fit_program->declarations[decl_index];

                if (decl->kind != FENG_DECL_FIT) {
                    continue;
                }
                if (owner_decl != NULL) {
                    const FengDecl *resolved_target = resolve_named_type_ref(session,
                                                                             program,
                                                                             decl->as.fit_decl.target);

                    if (resolved_target != owner_decl) {
                        continue;
                    }
                } else if (owner_builtin_name.length > 0U) {
                    if (!builtin_name_matches_type_ref(decl->as.fit_decl.target, owner_builtin_name)) {
                        continue;
                    }
                } else {
                    continue;
                }
                for (size_t member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                    const FengTypeMember *member = decl->as.fit_decl.members[member_index];
                    bool contains = false;

                    if (!member_passes_filter(member, filter)) {
                        continue;
                    }
                    if (!completion_json_contains_label(json, member->as.callable.name, &contains)) {
                        return false;
                    }
                    if (contains) {
                        continue;
                    }
                    if (!append_member_completion_item(json, first, member, NULL)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    for (module_index = 0U; module_index < session->analysis->module_count; ++module_index) {
        const FengSemanticModule *module = &session->analysis->modules[module_index];

        for (program_index = 0U; program_index < module->program_count; ++program_index) {
            const FengProgram *fit_program = module->programs[program_index];

            for (decl_index = 0U; decl_index < fit_program->declaration_count; ++decl_index) {
                const FengDecl *decl = fit_program->declarations[decl_index];

                if (decl->kind != FENG_DECL_FIT || !fit_visible_from_program(session, program, decl)) {
                    continue;
                }
                if (owner_decl != NULL) {
                    const FengDecl *resolved_target = resolve_named_type_ref(session,
                                                                             program,
                                                                             decl->as.fit_decl.target);

                    if (resolved_target != owner_decl) {
                        continue;
                    }
                } else if (owner_builtin_name.length > 0U) {
                    if (!builtin_name_matches_type_ref(decl->as.fit_decl.target, owner_builtin_name)) {
                        continue;
                    }
                } else {
                    continue;
                }
                for (size_t member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                    const FengTypeMember *member = decl->as.fit_decl.members[member_index];
                    bool contains = false;

                    if (!member_passes_filter(member, filter)) {
                        continue;
                    }
                    if (!completion_json_contains_label(json, member->as.callable.name, &contains)) {
                        return false;
                    }
                    if (contains) {
                        continue;
                    }
                    if (!append_member_completion_item(json, first, member, NULL)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static const FengDecl *resolve_type_constructor_expr(const FengLspAnalysisSession *session,
                                                     const FengProgram *program,
                                                     const FengExpr *expr) {
    size_t index;

    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        return resolve_type_name(session, program, expr->as.identifier);
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL &&
        expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
        const FengSemanticModule *alias_module = find_alias_module(session,
                                                                   program,
                                                                   expr->as.member.object->as.identifier);
        if (alias_module != NULL) {
            const FengDecl *decl = find_module_decl_by_name(alias_module,
                                                            expr->as.member.member,
                                                            false,
                                                            true,
                                                            true);
            if (decl != NULL) {
                return decl;
            }
        }
        for (index = 0U; index < program->use_count; ++index) {
            const FengUseDecl *use_decl = &program->uses[index];

            if (use_decl->has_alias && slice_equals(use_decl->alias, expr->as.member.object->as.identifier)) {
                const FengDecl *decl = find_loaded_module_decl_by_name(session,
                                                                      use_decl->segments,
                                                                      use_decl->segment_count,
                                                                      expr->as.member.member,
                                                                      false,
                                                                      true,
                                                                      true);
                if (decl != NULL) {
                    return decl;
                }
            }
        }
    }
    return NULL;
}

static const FengDecl *owner_decl_from_initializer_expr(const FengLspAnalysisSession *session,
                                                        const FengProgram *program,
                                                        const FengExpr *expr) {
    const FengDecl *decl;

    if (expr == NULL) {
        return NULL;
    }
    decl = owner_decl_from_type_fact(session, program, expr);
    if (decl != NULL) {
        return decl;
    }
    if (expr->kind == FENG_EXPR_OBJECT_LITERAL) {
        return resolve_type_constructor_expr(session, program, expr->as.object_literal.target);
    }
    if (expr->kind == FENG_EXPR_CALL) {
        return resolve_type_constructor_expr(session, program, expr->as.call.callee);
    }
    return NULL;
}

/* Resolve the owner type decl from a local binding.  When the binding has an
 * explicit type annotation, resolve via the type ref.  When the type is
 * inferred (binding->type == NULL), prefer the semantic type fact recorded for
 * the binding itself and fall back to syntactic type constructors for edit-time
 * completion paths where semantic analysis is unavailable. */
static const FengDecl *owner_decl_from_binding(const FengLspAnalysisSession *session,
                                               const FengProgram *program,
                                               const FengBinding *binding) {
    const FengSemanticTypeFact *fact;

    if (binding == NULL) {
        return NULL;
    }
    if (binding->type != NULL) {
        return resolve_named_type_ref(session, program, binding->type);
    }
    if (session->analysis != NULL) {
        fact = feng_semantic_lookup_type_fact(session->analysis, binding);
        if (fact != NULL) {
            if (fact->kind == FENG_SEMANTIC_TYPE_FACT_DECL) {
                return fact->type_decl;
            }
            if (fact->kind == FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
                return resolve_named_type_ref(session, program, fact->type_ref);
            }
        }
    }
    return owner_decl_from_initializer_expr(session, program, binding->initializer);
}

static const FengDecl *resolve_owner_decl_from_object_expr(const FengLspAnalysisSession *session,
                                                           const FengProgram *program,
                                                           const FengExpr *object,
                                                           const FengLspLocalList *locals) {
    const FengDecl *decl;

    if (object == NULL) {
        return NULL;
    }
    decl = owner_decl_from_type_fact(session, program, object);
    if (decl != NULL) {
        return decl;
    }
    if (object->kind == FENG_EXPR_SELF) {
        const FengLspLocal *self_local = find_local(locals, slice_from_cstr("self"));
        if (self_local != NULL && self_local->self_owner_decl != NULL) {
            if (self_local->self_owner_decl->kind == FENG_DECL_FIT) {
                const FengDecl *target_decl = resolve_named_type_ref(session,
                                                                     program,
                                                                     self_local->self_owner_decl->as.fit_decl.target);

                if (target_decl != NULL) {
                    return target_decl;
                }
            }
            return self_local->self_owner_decl;
        }
        return NULL;
    }
    if (object->kind != FENG_EXPR_IDENTIFIER) {
        return NULL;
    }
    {
        const FengLspLocal *local = find_local(locals, object->as.identifier);

        if (local != NULL) {
            if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
                return resolve_named_type_ref(session, program, local->parameter->type);
            }
            if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
                return owner_decl_from_binding(session, program, local->binding);
            }
            if (local->kind == FENG_LSP_LOCAL_SELF) {
                if (local->self_owner_decl != NULL && local->self_owner_decl->kind == FENG_DECL_FIT) {
                    const FengDecl *target_decl = resolve_named_type_ref(session,
                                                                         program,
                                                                         local->self_owner_decl->as.fit_decl.target);

                    if (target_decl != NULL) {
                        return target_decl;
                    }
                }
                return local->self_owner_decl;
            }
        }
    }
    decl = resolve_value_name(session, program, object->as.identifier);
    if (decl != NULL) {
        if (decl->kind == FENG_DECL_GLOBAL_BINDING) {
            return resolve_named_type_ref(session, program, decl->as.binding.type);
        }
        if (decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
            decl->kind == FENG_DECL_SPEC) {
            return decl;
        }
    }
    decl = resolve_type_name(session, program, object->as.identifier);
    if (decl != NULL &&
        (decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
         decl->kind == FENG_DECL_SPEC)) {
        return decl;
    }
    return NULL;
}

/* --- Expression type inference for cache path (symbol table) --- */

/* Find a member by name in a type declaration's direct members. */
static const FengSymbolDeclView *find_symbol_type_member_by_name(
    const FengSymbolDeclView *type_decl,
    FengSlice name) {
    size_t count;
    size_t i;

    if (type_decl == NULL) {
        return NULL;
    }
    count = feng_symbol_decl_member_count(type_decl);
    for (i = 0U; i < count; ++i) {
        const FengSymbolDeclView *m = feng_symbol_decl_member_at(type_decl, i);

        if (slice_equals(feng_symbol_decl_name(m), name)) {
            return m;
        }
    }
    return NULL;
}

/* Check if a fit declaration targets the given type declaration (by name). */
static bool symbol_fit_targets_decl(const FengLspCacheQueryContext *context,
                                    const FengSymbolDeclView *fit_decl,
                                    const FengSymbolDeclView *type_decl) {
    const FengSymbolTypeView *target;
    FengSlice target_name;
    FengSlice type_name;

    if (fit_decl == NULL || type_decl == NULL) {
        return false;
    }
    target = feng_symbol_decl_fit_target(fit_decl);
    if (target == NULL) {
        return false;
    }
    type_name = feng_symbol_decl_name(type_decl);
    if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_NAMED &&
        feng_symbol_type_segment_count(target) >= 1U) {
        target_name = feng_symbol_type_segment_at(target,
                                                   feng_symbol_type_segment_count(target) - 1U);
        if (slice_equals(target_name, type_name)) {
            return true;
        }
    }
    (void)context;
    return false;
}

/* Find a member by name in all fit declarations that target the given type. */
static const FengSymbolDeclView *find_symbol_fit_member_by_name(
    const FengLspCacheQueryContext *context,
    const FengSymbolDeclView *type_decl,
    FengSlice name) {
    size_t mod_count;
    size_t mod_idx;

    if (context == NULL || context->provider == NULL || type_decl == NULL) {
        return NULL;
    }
    mod_count = feng_symbol_provider_module_count(context->provider);
    for (mod_idx = 0U; mod_idx < mod_count; ++mod_idx) {
        const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mod_idx);
        size_t fit_count = feng_symbol_module_fit_count(mod);
        size_t fit_idx;

        for (fit_idx = 0U; fit_idx < fit_count; ++fit_idx) {
            const FengSymbolFitView *fit = feng_symbol_module_fit_at(mod, fit_idx);
            const FengSymbolDeclView *fd = feng_symbol_fit_decl(fit);
            size_t count;
            size_t i;

            if (fd == NULL || !symbol_fit_targets_decl(context, fd, type_decl)) {
                continue;
            }
            count = feng_symbol_decl_member_count(fd);
            for (i = 0U; i < count; ++i) {
                const FengSymbolDeclView *m = feng_symbol_decl_member_at(fd, i);

                if (slice_equals(feng_symbol_decl_name(m), name)) {
                    return m;
                }
            }
        }
    }
    return NULL;
}

/* Forward declaration for mutual recursion. */
static const FengSymbolDeclView *resolve_symbol_owner_decl_from_expr(
    const FengLspCacheQueryContext *context,
    const FengExpr *expr,
    const FengLspLocalList *locals);

/* Resolve the return type of a call expression. */
static const FengSymbolDeclView *resolve_symbol_call_return_type(
    const FengLspCacheQueryContext *context,
    const FengExpr *call_expr,
    const FengLspLocalList *locals) {
    const FengExpr *callee;
    const FengSymbolDeclView *type_decl;
    const FengSymbolDeclView *func_decl;
    const FengSymbolTypeView *ret_type;

    if (call_expr == NULL || call_expr->as.call.callee == NULL) {
        return NULL;
    }
    callee = call_expr->as.call.callee;

    if (callee->kind == FENG_EXPR_IDENTIFIER) {
        type_decl = resolve_symbol_type_name(context->provider,
                                              context->current_module,
                                              context->program,
                                              callee->as.identifier);
        if (type_decl != NULL) {
            return type_decl;
        }
        func_decl = resolve_symbol_value_name(context->provider,
                                               context->current_module,
                                               context->program,
                                               callee->as.identifier);
        if (func_decl != NULL &&
            feng_symbol_decl_kind(func_decl) == FENG_SYMBOL_DECL_KIND_FUNCTION) {
            ret_type = feng_symbol_decl_return_type(func_decl);
            if (ret_type != NULL) {
                return resolve_symbol_type_view(context->provider,
                                                context->current_module,
                                                context->program,
                                                ret_type);
            }
        }
        return NULL;
    }

    if (callee->kind == FENG_EXPR_MEMBER && callee->as.member.object != NULL) {
        const FengSymbolDeclView *obj_type = resolve_symbol_owner_decl_from_expr(
            context, callee->as.member.object, locals);
        const FengSymbolDeclView *method;

        if (obj_type == NULL) {
            return NULL;
        }
        method = find_symbol_type_member_by_name(obj_type, callee->as.member.member);
        if (method == NULL) {
            method = find_symbol_fit_member_by_name(context, obj_type, callee->as.member.member);
        }
        if (method != NULL) {
            ret_type = feng_symbol_decl_return_type(method);
            if (ret_type != NULL) {
                return resolve_symbol_type_view(context->provider,
                                                context->current_module,
                                                context->program,
                                                ret_type);
            }
        }
        return NULL;
    }

    return NULL;
}

/* Resolve the type of a member access expression (field access). */
static const FengSymbolDeclView *resolve_symbol_member_access_type(
    const FengLspCacheQueryContext *context,
    const FengExpr *member_expr,
    const FengLspLocalList *locals) {
    const FengSymbolDeclView *obj_type;
    const FengSymbolDeclView *member;
    FengSymbolDeclKind kind;

    if (member_expr == NULL || member_expr->as.member.object == NULL) {
        return NULL;
    }
    obj_type = resolve_symbol_owner_decl_from_expr(context, member_expr->as.member.object, locals);
    if (obj_type == NULL) {
        return NULL;
    }
    member = find_symbol_type_member_by_name(obj_type, member_expr->as.member.member);
    if (member == NULL) {
        member = find_symbol_fit_member_by_name(context, obj_type, member_expr->as.member.member);
    }
    if (member == NULL) {
        return NULL;
    }
    kind = feng_symbol_decl_kind(member);
    if (kind == FENG_SYMBOL_DECL_KIND_FIELD) {
        const FengSymbolTypeView *field_type = feng_symbol_decl_value_type(member);

        if (field_type != NULL) {
            return resolve_symbol_type_view(context->provider,
                                            context->current_module,
                                            context->program,
                                            field_type);
        }
    }
    if (kind == FENG_SYMBOL_DECL_KIND_METHOD) {
        const FengSymbolTypeView *ret_type = feng_symbol_decl_return_type(member);

        if (ret_type != NULL) {
            return resolve_symbol_type_view(context->provider,
                                            context->current_module,
                                            context->program,
                                            ret_type);
        }
    }
    return NULL;
}

/* Recursively resolve the type of any expression via the symbol table. */
static const FengSymbolDeclView *resolve_symbol_owner_decl_from_expr(
    const FengLspCacheQueryContext *context,
    const FengExpr *expr,
    const FengLspLocalList *locals) {
    if (context == NULL || expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_SELF || expr->kind == FENG_EXPR_IDENTIFIER) {
        return resolve_symbol_owner_decl_from_object_expr(context, expr, locals);
    }
    if (expr->kind == FENG_EXPR_CALL) {
        return resolve_symbol_call_return_type(context, expr, locals);
    }
    if (expr->kind == FENG_EXPR_MEMBER) {
        return resolve_symbol_member_access_type(context, expr, locals);
    }
    return NULL;
}

/* Resolve the builtin type name of an expression when its type is a builtin.
 * Returns a non-empty slice (e.g. "string", "i32") when the expression resolves
 * to a builtin type, or an empty slice otherwise.  This complements
 * resolve_symbol_owner_decl_from_expr which returns NULL for builtins. */
static FengSlice resolve_symbol_builtin_name_from_expr(
    const FengLspCacheQueryContext *context,
    const FengExpr *expr,
    const FengLspLocalList *locals) {
    FengSlice empty = {0};
    const FengExpr *callee;
    const FengSymbolDeclView *obj_type;
    const FengSymbolDeclView *method;
    const FengSymbolTypeView *ret_type;

    if (context == NULL || expr == NULL) {
        return empty;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        const FengLspLocal *local = find_local(locals, expr->as.identifier);

        if (local != NULL && local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
            if (local->binding->type != NULL) {
                const char *name = builtin_name_for_single_segment_type_ref(local->binding->type);

                if (name != NULL) {
                    return slice_from_cstr(name);
                }
            }
        }
        if (local != NULL && local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
            if (local->parameter->type != NULL) {
                const char *name = builtin_name_for_single_segment_type_ref(local->parameter->type);

                if (name != NULL) {
                    return slice_from_cstr(name);
                }
            }
        }
        return empty;
    }
    if (expr->kind == FENG_EXPR_STRING) {
        return slice_from_cstr("string");
    }
    if (expr->kind == FENG_EXPR_INTEGER) {
        return slice_from_cstr("i32");
    }
    if (expr->kind == FENG_EXPR_FLOAT) {
        return slice_from_cstr("f64");
    }
    if (expr->kind == FENG_EXPR_BOOL) {
        return slice_from_cstr("bool");
    }
    if (expr->kind == FENG_EXPR_CALL) {
        callee = expr->as.call.callee;
        if (callee == NULL) {
            return empty;
        }
        if (callee->kind == FENG_EXPR_MEMBER && callee->as.member.object != NULL) {
            obj_type = resolve_symbol_owner_decl_from_expr(context, callee->as.member.object, locals);
            if (obj_type == NULL) {
                return empty;
            }
            method = find_symbol_type_member_by_name(obj_type, callee->as.member.member);
            if (method == NULL) {
                method = find_symbol_fit_member_by_name(context, obj_type, callee->as.member.member);
            }
            if (method != NULL) {
                ret_type = feng_symbol_decl_return_type(method);
                if (ret_type != NULL && feng_symbol_type_kind(ret_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                    return feng_symbol_type_builtin_name(ret_type);
                }
            }
        }
        return empty;
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL) {
        obj_type = resolve_symbol_owner_decl_from_expr(context, expr->as.member.object, locals);
        if (obj_type == NULL) {
            return empty;
        }
        method = find_symbol_type_member_by_name(obj_type, expr->as.member.member);
        if (method == NULL) {
            method = find_symbol_fit_member_by_name(context, obj_type, expr->as.member.member);
        }
        if (method != NULL) {
            FengSymbolDeclKind kind = feng_symbol_decl_kind(method);

            if (kind == FENG_SYMBOL_DECL_KIND_FIELD) {
                const FengSymbolTypeView *field_type = feng_symbol_decl_value_type(method);

                if (field_type != NULL && feng_symbol_type_kind(field_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                    return feng_symbol_type_builtin_name(field_type);
                }
            }
            if (kind == FENG_SYMBOL_DECL_KIND_METHOD) {
                ret_type = feng_symbol_decl_return_type(method);
                if (ret_type != NULL && feng_symbol_type_kind(ret_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                    return feng_symbol_type_builtin_name(ret_type);
                }
            }
        }
        return empty;
    }
    return empty;
}

static bool find_decl_token_hit(const char *source_text,
                                const FengDecl *decl,
                                size_t offset,
                                FengLspResolvedTarget *target);
static bool find_type_ref_hit(const FengDecl *decl,
                              const FengProgram *program,
                              const FengLspAnalysisSession *session,
                              size_t offset,
                              FengLspResolvedTarget *target);
static FengSlice member_name_slice(const FengTypeMember *member);
static const FengExpr *find_expr_hit(const FengExpr *expr, size_t offset);
static const FengExpr *find_expr_hit_in_block(const FengBlock *block, size_t offset);

static bool find_decl_token_hit_member(const char *source_text,
                                       const FengDecl *owner_decl,
                                       const FengTypeMember *member,
                                       size_t offset,
                                       FengLspResolvedTarget *target) {
    size_t index;
    bool hit_member_name = offset_in_slice_from_source(source_text,
                                                       member_name_slice(member),
                                                       offset);
    bool hit_member_token = offset_in_token(member->token, offset);

    if (hit_member_name ||
        (hit_member_token &&
         !(owner_decl != NULL &&
           owner_decl->kind == FENG_DECL_FIT &&
           member->kind != FENG_TYPE_MEMBER_FIELD))) {
        target->kind = FENG_LSP_RESOLVED_MEMBER;
        target->decl = owner_decl;
        target->member = member;
        return true;
    }
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return false;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (offset_in_token(member->as.callable.params[index].token, offset) ||
            offset_in_slice_from_source(source_text,
                                        member->as.callable.params[index].name,
                                        offset)) {
            target->kind = FENG_LSP_RESOLVED_PARAM;
            target->parameter = &member->as.callable.params[index];
            return true;
        }
    }
    return false;
}

static bool find_decl_token_hit(const char *source_text,
                                const FengDecl *decl,
                                size_t offset,
                                FengLspResolvedTarget *target) {
    size_t index;
    FengSlice decl_slice;

    decl_slice = decl_name(decl);
    if (offset_in_token(decl->token, offset) ||
        (decl->kind != FENG_DECL_FIT &&
         offset_in_slice_from_source(source_text, decl_slice, offset))) {
        target->kind = FENG_LSP_RESOLVED_DECL;
        target->decl = decl;
        return true;
    }
    switch (decl->kind) {
        case FENG_DECL_FUNCTION:
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (offset_in_token(decl->as.function_decl.params[index].token, offset) ||
                    offset_in_slice_from_source(source_text,
                                                decl->as.function_decl.params[index].name,
                                                offset)) {
                    target->kind = FENG_LSP_RESOLVED_PARAM;
                    target->parameter = &decl->as.function_decl.params[index];
                    return true;
                }
            }
            break;
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (find_decl_token_hit_member(source_text,
                                               decl,
                                               decl->as.type_decl.members[index],
                                               offset,
                                               target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    if (find_decl_token_hit_member(source_text,
                                                   decl,
                                                   decl->as.spec_decl.as.object.members[index],
                                                   offset,
                                                   target)) {
                        return true;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                if (find_decl_token_hit_member(source_text,
                                               decl,
                                               decl->as.fit_decl.members[index],
                                               offset,
                                               target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_GLOBAL_BINDING:
            break;
    }
    return false;
}

static bool type_ref_contains_offset(const FengTypeRef *type_ref, size_t offset) {
    return type_ref != NULL && offset >= type_ref->token.offset && offset <= type_ref_end(type_ref);
}

static bool find_type_ref_in_member(const FengDecl *owner_decl,
                                    const FengTypeMember *member,
                                    const FengProgram *program,
                                    const FengLspAnalysisSession *session,
                                    size_t offset,
                                    FengLspResolvedTarget *target) {
    size_t index;
    (void)owner_decl;

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        if (type_ref_contains_offset(member->as.field.type, offset)) {
            const FengDecl *decl = resolve_named_type_ref(session, program, member->as.field.type);
            if (decl != NULL) {
                target->kind = FENG_LSP_RESOLVED_DECL;
                target->decl = decl;
                return true;
            }
        }
        return false;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (type_ref_contains_offset(member->as.callable.params[index].type, offset)) {
            const FengDecl *decl = resolve_named_type_ref(session,
                                                          program,
                                                          member->as.callable.params[index].type);
            if (decl != NULL) {
                target->kind = FENG_LSP_RESOLVED_DECL;
                target->decl = decl;
                return true;
            }
        }
    }
    if (type_ref_contains_offset(member->as.callable.return_type, offset)) {
        const FengDecl *decl = resolve_named_type_ref(session,
                                                      program,
                                                      member->as.callable.return_type);
        if (decl != NULL) {
            target->kind = FENG_LSP_RESOLVED_DECL;
            target->decl = decl;
            return true;
        }
    }
    return false;
}

static bool find_block_type_ref_hit(const FengBlock *block,
                                    const FengProgram *program,
                                    const FengLspAnalysisSession *session,
                                    size_t offset,
                                    FengLspResolvedTarget *target);

static bool find_stmt_type_ref_hit(const FengStmt *stmt,
                                   const FengProgram *program,
                                   const FengLspAnalysisSession *session,
                                   size_t offset,
                                   FengLspResolvedTarget *target) {
    size_t index;

    if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end(stmt)) {
        return false;
    }
    switch (stmt->kind) {
        case FENG_STMT_BINDING: {
            const FengDecl *resolved;

            if (!type_ref_contains_offset(stmt->as.binding.type, offset)) {
                return false;
            }
            resolved = resolve_named_type_ref(session, program, stmt->as.binding.type);
            if (resolved == NULL) {
                return false;
            }
            target->kind = FENG_LSP_RESOLVED_DECL;
            target->decl = resolved;
            return true;
        }
        case FENG_STMT_BLOCK:
            return find_block_type_ref_hit(stmt->as.block, program, session, offset, target);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (find_block_type_ref_hit(stmt->as.if_stmt.clauses[index].block,
                                            program,
                                            session,
                                            offset,
                                            target)) {
                    return true;
                }
            }
            return find_block_type_ref_hit(stmt->as.if_stmt.else_block,
                                           program,
                                           session,
                                           offset,
                                           target);
        case FENG_STMT_MATCH:
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (find_block_type_ref_hit(stmt->as.match_stmt.branches[index].body,
                                            program,
                                            session,
                                            offset,
                                            target)) {
                    return true;
                }
            }
            return find_block_type_ref_hit(stmt->as.match_stmt.else_block,
                                           program,
                                           session,
                                           offset,
                                           target);
        case FENG_STMT_WHILE:
            return find_block_type_ref_hit(stmt->as.while_stmt.body,
                                           program,
                                           session,
                                           offset,
                                           target);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                const FengDecl *resolved;

                if (type_ref_contains_offset(stmt->as.for_stmt.iter_binding.type, offset)) {
                    resolved = resolve_named_type_ref(session,
                                                      program,
                                                      stmt->as.for_stmt.iter_binding.type);
                    if (resolved != NULL) {
                        target->kind = FENG_LSP_RESOLVED_DECL;
                        target->decl = resolved;
                        return true;
                    }
                }
            } else {
                if (find_stmt_type_ref_hit(stmt->as.for_stmt.init,
                                           program,
                                           session,
                                           offset,
                                           target)) {
                    return true;
                }
                if (find_stmt_type_ref_hit(stmt->as.for_stmt.update,
                                           program,
                                           session,
                                           offset,
                                           target)) {
                    return true;
                }
            }
            return find_block_type_ref_hit(stmt->as.for_stmt.body,
                                           program,
                                           session,
                                           offset,
                                           target);
        case FENG_STMT_ASSIGN:
        case FENG_STMT_EXPR:
        case FENG_STMT_TRY:
        case FENG_STMT_RETURN:
        case FENG_STMT_THROW:
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return false;
    }
    return false;
}

static bool find_block_type_ref_hit(const FengBlock *block,
                                    const FengProgram *program,
                                    const FengLspAnalysisSession *session,
                                    size_t offset,
                                    FengLspResolvedTarget *target) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return false;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (find_stmt_type_ref_hit(block->statements[index],
                                   program,
                                   session,
                                   offset,
                                   target)) {
            return true;
        }
    }
    return false;
}

static bool find_type_ref_hit(const FengDecl *decl,
                              const FengProgram *program,
                              const FengLspAnalysisSession *session,
                              size_t offset,
                              FengLspResolvedTarget *target) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            if (type_ref_contains_offset(decl->as.binding.type, offset)) {
                const FengDecl *resolved = resolve_named_type_ref(session, program, decl->as.binding.type);
                if (resolved != NULL) {
                    target->kind = FENG_LSP_RESOLVED_DECL;
                    target->decl = resolved;
                    return true;
                }
            }
            break;
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_FUNCTION:
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (type_ref_contains_offset(decl->as.function_decl.params[index].type, offset)) {
                    const FengDecl *resolved = resolve_named_type_ref(session,
                                                                      program,
                                                                      decl->as.function_decl.params[index].type);
                    if (resolved != NULL) {
                        target->kind = FENG_LSP_RESOLVED_DECL;
                        target->decl = resolved;
                        return true;
                    }
                }
            }
            if (type_ref_contains_offset(decl->as.function_decl.return_type, offset)) {
                const FengDecl *resolved = resolve_named_type_ref(session,
                                                                  program,
                                                                  decl->as.function_decl.return_type);
                if (resolved != NULL) {
                    target->kind = FENG_LSP_RESOLVED_DECL;
                    target->decl = resolved;
                    return true;
                }
            }
            if (find_block_type_ref_hit(decl->as.function_decl.body,
                                        program,
                                        session,
                                        offset,
                                        target)) {
                return true;
            }
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (find_type_ref_in_member(decl,
                                            decl->as.type_decl.members[index],
                                            program,
                                            session,
                                            offset,
                                            target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    if (find_type_ref_in_member(decl,
                                                decl->as.spec_decl.as.object.members[index],
                                                program,
                                                session,
                                                offset,
                                                target)) {
                        return true;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                if (find_type_ref_in_member(decl,
                                            decl->as.fit_decl.members[index],
                                            program,
                                            session,
                                            offset,
                                            target)) {
                    return true;
                }
            }
            break;
    }
    return false;
}

static const FengExpr *find_expr_hit(const FengExpr *expr, size_t offset) {
    size_t index;

    if (expr == NULL || offset < expr_start(expr) || offset > expr_end(expr)) {
        return NULL;
    }
    switch (expr->kind) {
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                const FengExpr *hit = find_expr_hit(expr->as.array_literal.items[index], offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        case FENG_EXPR_GENERIC_TARGET:
            return find_expr_hit(expr->as.generic_target.target, offset);
        case FENG_EXPR_OBJECT_LITERAL:
            if (expr->as.object_literal.target != NULL) {
                const FengExpr *hit = find_expr_hit(expr->as.object_literal.target, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                const FengExpr *hit = find_expr_hit(expr->as.object_literal.fields[index].value, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        case FENG_EXPR_CALL: {
            const FengExpr *hit = find_expr_hit(expr->as.call.callee, offset);
            if (hit != NULL) {
                return hit;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                hit = find_expr_hit(expr->as.call.args[index], offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        }
        case FENG_EXPR_MEMBER: {
            const FengExpr *hit = find_expr_hit(expr->as.member.object, offset);
            if (hit != NULL) {
                return hit;
            }
            break;
        }
        case FENG_EXPR_INDEX: {
            const FengExpr *hit = find_expr_hit(expr->as.index.object, offset);
            if (hit != NULL) {
                return hit;
            }
            hit = find_expr_hit(expr->as.index.index, offset);
            if (hit != NULL) {
                return hit;
            }
            break;
        }
        case FENG_EXPR_UNARY:
            return find_expr_hit(expr->as.unary.operand, offset);
        case FENG_EXPR_BINARY: {
            const FengExpr *hit = find_expr_hit(expr->as.binary.left, offset);
            if (hit != NULL) {
                return hit;
            }
            return find_expr_hit(expr->as.binary.right, offset);
        }
        case FENG_EXPR_CAST:
            return find_expr_hit(expr->as.cast.value, offset);
        case FENG_EXPR_IF: {
            const FengExpr *hit = find_expr_hit(expr->as.if_expr.condition, offset);
            if (hit != NULL) {
                return hit;
            }
            hit = find_expr_hit_in_block(expr->as.if_expr.then_block, offset);
            if (hit != NULL) {
                return hit;
            }
            return find_expr_hit_in_block(expr->as.if_expr.else_block, offset);
        }
        case FENG_EXPR_MATCH: {
            const FengExpr *hit = find_expr_hit(expr->as.match_expr.target, offset);
            size_t branch_index;

            if (hit != NULL) {
                return hit;
            }
            for (branch_index = 0U; branch_index < expr->as.match_expr.branch_count; ++branch_index) {
                hit = find_expr_hit_in_block(expr->as.match_expr.branches[branch_index].body, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            return find_expr_hit_in_block(expr->as.match_expr.else_block, offset);
        }
        case FENG_EXPR_TRY: {
            const FengExpr *hit = find_expr_hit(expr->as.try_expr.body, offset);
            size_t clause_index;

            if (hit != NULL) {
                return hit;
            }
            for (clause_index = 0U;
                 clause_index < expr->as.try_expr.clause_count;
                 ++clause_index) {
                hit = find_expr_hit_in_block(expr->as.try_expr.clauses[clause_index].body, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        }
        default:
            break;
    }
    if ((expr->kind == FENG_EXPR_IDENTIFIER || expr->kind == FENG_EXPR_SELF || expr->kind == FENG_EXPR_MEMBER) &&
        offset_in_token(expr->token, offset)) {
        return expr;
    }
    return NULL;
}

static const FengExpr *find_expr_hit_in_block(const FengBlock *block, size_t offset) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return NULL;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        const FengStmt *stmt = block->statements[index];
        const FengExpr *hit = NULL;

        if (offset < stmt->token.offset || offset > stmt_end(stmt)) {
            continue;
        }
        switch (stmt->kind) {
            case FENG_STMT_BINDING:
                hit = find_expr_hit(stmt->as.binding.initializer, offset);
                break;
            case FENG_STMT_ASSIGN:
                hit = find_expr_hit(stmt->as.assign.target, offset);
                if (hit == NULL) {
                    hit = find_expr_hit(stmt->as.assign.value, offset);
                }
                break;
            case FENG_STMT_TRY:
            case FENG_STMT_EXPR:
                hit = find_expr_hit(stmt->as.expr, offset);
                break;
            case FENG_STMT_BLOCK:
                hit = find_expr_hit_in_block(stmt->as.block, offset);
                break;
            case FENG_STMT_IF:
                hit = stmt->as.if_stmt.clause_count > 0
                          ? find_expr_hit(stmt->as.if_stmt.clauses[0].condition, offset)
                          : NULL;
                if (hit == NULL) {
                    size_t clause_index;
                    for (clause_index = 0U; clause_index < stmt->as.if_stmt.clause_count && hit == NULL; ++clause_index) {
                        hit = find_expr_hit_in_block(stmt->as.if_stmt.clauses[clause_index].block, offset);
                    }
                    if (hit == NULL) {
                        hit = find_expr_hit_in_block(stmt->as.if_stmt.else_block, offset);
                    }
                }
                break;
            case FENG_STMT_MATCH:
                hit = find_expr_hit(stmt->as.match_stmt.target, offset);
                if (hit == NULL) {
                    size_t branch_index;
                    for (branch_index = 0U; branch_index < stmt->as.match_stmt.branch_count && hit == NULL; ++branch_index) {
                        hit = find_expr_hit_in_block(stmt->as.match_stmt.branches[branch_index].body, offset);
                    }
                    if (hit == NULL) {
                        hit = find_expr_hit_in_block(stmt->as.match_stmt.else_block, offset);
                    }
                }
                break;
            case FENG_STMT_WHILE:
                hit = find_expr_hit(stmt->as.while_stmt.condition, offset);
                if (hit == NULL) {
                    hit = find_expr_hit_in_block(stmt->as.while_stmt.body, offset);
                }
                break;
            case FENG_STMT_FOR:
                if (stmt->as.for_stmt.is_for_in) {
                    hit = find_expr_hit(stmt->as.for_stmt.iter_expr, offset);
                    if (hit == NULL) {
                        hit = find_expr_hit_in_block(stmt->as.for_stmt.body, offset);
                    }
                } else {
                    /* Search init statement expressions (e.g. var i = expr). */
                    if (stmt->as.for_stmt.init != NULL) {
                        const FengStmt *init = stmt->as.for_stmt.init;

                        if (init->kind == FENG_STMT_BINDING) {
                            hit = find_expr_hit(init->as.binding.initializer, offset);
                        } else if (init->kind == FENG_STMT_ASSIGN) {
                            hit = find_expr_hit(init->as.assign.target, offset);
                            if (hit == NULL) {
                                hit = find_expr_hit(init->as.assign.value, offset);
                            }
                        }
                    }
                    /* Search update statement expressions (e.g. i += 1). */
                    if (hit == NULL && stmt->as.for_stmt.update != NULL) {
                        const FengStmt *update = stmt->as.for_stmt.update;

                        if (update->kind == FENG_STMT_ASSIGN) {
                            hit = find_expr_hit(update->as.assign.target, offset);
                            if (hit == NULL) {
                                hit = find_expr_hit(update->as.assign.value, offset);
                            }
                        } else if (update->kind == FENG_STMT_EXPR) {
                            hit = find_expr_hit(update->as.expr, offset);
                        }
                    }
                    if (hit == NULL) {
                        hit = find_expr_hit_in_block(stmt->as.for_stmt.body, offset);
                    }
                    if (hit == NULL) {
                        hit = find_expr_hit(stmt->as.for_stmt.condition, offset);
                    }
                }
                break;
            case FENG_STMT_RETURN:
                hit = find_expr_hit(stmt->as.return_value, offset);
                break;
            case FENG_STMT_THROW:
                hit = find_expr_hit(stmt->as.throw_value, offset);
                break;
            case FENG_STMT_BREAK:
            case FENG_STMT_CONTINUE:
                break;
        }
        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

static const FengExpr *find_expr_hit_in_decl(const FengDecl *decl, size_t offset) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return find_expr_hit(decl->as.binding.initializer, offset);
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_FUNCTION:
            return find_expr_hit_in_block(decl->as.function_decl.body, offset);
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];
                const FengExpr *hit = member->kind == FENG_TYPE_MEMBER_FIELD
                                          ? find_expr_hit(member->as.field.initializer, offset)
                                          : find_expr_hit_in_block(member->as.callable.body, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    const FengTypeMember *member = decl->as.spec_decl.as.object.members[index];
                    const FengExpr *hit = member->kind == FENG_TYPE_MEMBER_FIELD
                                              ? find_expr_hit(member->as.field.initializer, offset)
                                              : find_expr_hit_in_block(member->as.callable.body, offset);
                    if (hit != NULL) {
                        return hit;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.fit_decl.members[index];
                const FengExpr *hit = member->kind == FENG_TYPE_MEMBER_FIELD
                                          ? find_expr_hit(member->as.field.initializer, offset)
                                          : find_expr_hit_in_block(member->as.callable.body, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
    }
    return NULL;
}

static size_t offset_from_position(const char *text,
                                   unsigned int line,
                                   unsigned int character) {
    unsigned int current_line = 0U;
    unsigned int current_char = 0U;
    size_t offset = 0U;

    while (text[offset] != '\0') {
        if (current_line == line && current_char == character) {
            return offset;
        }
        if (text[offset] == '\n') {
            ++current_line;
            current_char = 0U;
        } else {
            ++current_char;
        }
        ++offset;
    }
    return offset;
}

static void position_from_offset(const char *text,
                                 size_t offset,
                                 unsigned int *out_line,
                                 unsigned int *out_character) {
    unsigned int line = 0U;
    unsigned int character = 0U;
    size_t index = 0U;

    while (text[index] != '\0' && index < offset) {
        if (text[index] == '\n') {
            ++line;
            character = 0U;
        } else {
            ++character;
        }
        ++index;
    }
    if (out_line != NULL) {
        *out_line = line;
    }
    if (out_character != NULL) {
        *out_character = character;
    }
}

static bool type_ref_to_string(FengLspString *buffer, const FengTypeRef *type_ref) {
    size_t index;

    if (type_ref == NULL) {
        return string_append_cstr(buffer, "void");
    }
    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            for (index = 0U; index < type_ref->as.named.segment_count; ++index) {
                if (index > 0U && !string_append_cstr(buffer, ".")) {
                    return false;
                }
                if (!string_append_bytes(buffer,
                                         type_ref->as.named.segments[index].data,
                                         type_ref->as.named.segments[index].length)) {
                    return false;
                }
            }
            if (type_ref->as.named.type_arg_count > 0U) {
                if (!string_append_cstr(buffer, "<")) {
                    return false;
                }
                for (index = 0U; index < type_ref->as.named.type_arg_count; ++index) {
                    if (index > 0U && !string_append_cstr(buffer, ", ")) {
                        return false;
                    }
                    if (!type_ref_to_string(buffer, type_ref->as.named.type_args[index])) {
                        return false;
                    }
                }
                if (!string_append_cstr(buffer, ">")) {
                    return false;
                }
            }
            return true;
        case FENG_TYPE_REF_POINTER:
            return string_append_cstr(buffer, "*") && type_ref_to_string(buffer, type_ref->as.inner);
        case FENG_TYPE_REF_ARRAY:
            return type_ref_to_string(buffer, type_ref->as.inner) &&
                   string_append_cstr(buffer, type_ref->array_element_writable ? "[!]" : "[]");
    }
    return false;
}

static bool parameter_type_to_string(FengLspString *buffer, const FengParameter *param) {
    const FengTypeRef *type = param != NULL ? param->type : NULL;

    if (param != NULL && param->is_variadic) {
        if (type != NULL && type->kind == FENG_TYPE_REF_ARRAY) {
            return type_ref_to_string(buffer, type->as.inner) && string_append_cstr(buffer, "...");
        }
        return type_ref_to_string(buffer, type) && string_append_cstr(buffer, "...");
    }
    return type_ref_to_string(buffer, type);
}

static bool semantic_type_fact_to_string(FengLspString *buffer,
                                         const FengSemanticTypeFact *fact) {
    if (fact == NULL) {
        return false;
    }
    switch (fact->kind) {
        case FENG_SEMANTIC_TYPE_FACT_BUILTIN:
            return string_append_bytes(buffer, fact->builtin_name.data, fact->builtin_name.length);
        case FENG_SEMANTIC_TYPE_FACT_TYPE_REF:
            return type_ref_to_string(buffer, fact->type_ref);
        case FENG_SEMANTIC_TYPE_FACT_DECL: {
            FengSlice name = decl_name(fact->type_decl);

            return string_append_bytes(buffer, name.data, name.length);
        }
        case FENG_SEMANTIC_TYPE_FACT_UNKNOWN:
            break;
    }
    return false;
}

static bool append_optional_static_type_annotation(FengLspString *buffer,
                                                  const FengLspAnalysisSession *session,
                                                  const void *site,
                                                  const FengTypeRef *explicit_type) {
    if (explicit_type != NULL) {
        return string_append_cstr(buffer, ": ") && type_ref_to_string(buffer, explicit_type);
    }
    if (session != NULL && session->analysis != NULL) {
        const FengSemanticTypeFact *fact = feng_semantic_lookup_type_fact(session->analysis, site);

        if (fact != NULL) {
            return string_append_cstr(buffer, ": ") && semantic_type_fact_to_string(buffer, fact);
        }
    }
    return true;
}

static bool binding_signature_to_string(FengLspString *buffer,
                                        const FengLspAnalysisSession *session,
                                        const FengBinding *binding) {
    return string_append_cstr(buffer,
                              binding->mutability == FENG_MUTABILITY_VAR ? "var " : "let ") &&
           string_append_bytes(buffer, binding->name.data, binding->name.length) &&
           append_optional_static_type_annotation(buffer, session, binding, binding->type);
}

static bool decl_signature_to_string_with_session(FengLspString *buffer,
                                                  const FengLspAnalysisSession *session,
                                                  const FengDecl *decl);
static bool member_signature_to_string_with_session(FengLspString *buffer,
                                                    const FengLspAnalysisSession *session,
                                                    const FengTypeMember *member);

static bool append_decl_type_params(FengLspString *buffer,
                                    const FengTypeParam *params,
                                    size_t count) {
    size_t i;

    if (count == 0U) {
        return true;
    }
    /* Defensive guard: params should always be non-NULL when count > 0, but
     * synthesized declarations may have count set without a params array if
     * synthesis fails or the decl is from an older cache. Skip gracefully. */
    if (params == NULL) {
        return true;
    }
    if (!string_append_cstr(buffer, "<")) {
        return false;
    }
    for (i = 0U; i < count; ++i) {
        if (i > 0U && !string_append_cstr(buffer, ", ")) {
            return false;
        }
        if (!string_append_bytes(buffer, params[i].name.data, params[i].name.length)) {
            return false;
        }
        if (params[i].constraint != NULL) {
            if (!string_append_cstr(buffer, ": ") ||
                !type_ref_to_string(buffer, params[i].constraint)) {
                return false;
            }
        }
    }
    return string_append_cstr(buffer, ">");
}

static bool decl_signature_to_string(FengLspString *buffer, const FengDecl *decl) {
    return decl_signature_to_string_with_session(buffer, NULL, decl);
}

static bool decl_signature_to_string_with_session(FengLspString *buffer,
                                                  const FengLspAnalysisSession *session,
                                                  const FengDecl *decl) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return binding_signature_to_string(buffer, session, &decl->as.binding);
        case FENG_DECL_ENUM:
            return string_append_cstr(buffer, "enum ") &&
                   string_append_bytes(buffer,
                                       decl->as.enum_decl.name.data,
                                       decl->as.enum_decl.name.length);
        case FENG_DECL_TYPE:
            return string_append_cstr(buffer, "type ") &&
                   string_append_bytes(buffer, decl->as.type_decl.name.data, decl->as.type_decl.name.length) &&
                   append_decl_type_params(buffer, decl->as.type_decl.type_params, decl->as.type_decl.type_param_count);
        case FENG_DECL_SPEC:
            return string_append_cstr(buffer, "spec ") &&
                   string_append_bytes(buffer, decl->as.spec_decl.name.data, decl->as.spec_decl.name.length) &&
                   append_decl_type_params(buffer, decl->as.spec_decl.type_params, decl->as.spec_decl.type_param_count);
        case FENG_DECL_FIT:
            return string_append_cstr(buffer, "fit");
        case FENG_DECL_FUNCTION:
            if (!string_append_cstr(buffer, "func ") ||
                !string_append_bytes(buffer,
                                     decl->as.function_decl.name.data,
                                     decl->as.function_decl.name.length) ||
                !append_decl_type_params(buffer, decl->as.function_decl.type_params, decl->as.function_decl.type_param_count) ||
                !string_append_cstr(buffer, "(")) {
                return false;
            }
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (index > 0U && !string_append_cstr(buffer, ", ")) {
                    return false;
                }
                if (!string_append_bytes(buffer,
                                         decl->as.function_decl.params[index].name.data,
                                         decl->as.function_decl.params[index].name.length) ||
                    !string_append_cstr(buffer, ": ") ||
                    !parameter_type_to_string(buffer, &decl->as.function_decl.params[index])) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   type_ref_to_string(buffer, decl->as.function_decl.return_type);
    }
    return false;
}

static bool member_signature_to_string(FengLspString *buffer, const FengTypeMember *member) {
    return member_signature_to_string_with_session(buffer, NULL, member);
}

static bool member_signature_to_string_with_session(FengLspString *buffer,
                                                    const FengLspAnalysisSession *session,
                                                    const FengTypeMember *member) {
    size_t index;

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return string_append_cstr(buffer,
                                  member->as.field.mutability == FENG_MUTABILITY_VAR ? "var " : "let ") &&
               string_append_bytes(buffer,
                                   member->as.field.name.data,
                                   member->as.field.name.length) &&
               append_optional_static_type_annotation(buffer, session, member, member->as.field.type);
    }
    if (!string_append_cstr(buffer,
                            member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR ? "ctor " :
                            member->kind == FENG_TYPE_MEMBER_FINALIZER ? "finalizer " : "func ") ||
        !string_append_bytes(buffer,
                             member->as.callable.name.data,
                             member->as.callable.name.length) ||
        !append_decl_type_params(buffer, member->as.callable.type_params, member->as.callable.type_param_count) ||
        !string_append_cstr(buffer, "(")) {
        return false;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (index > 0U && !string_append_cstr(buffer, ", ")) {
            return false;
        }
        if (!string_append_bytes(buffer,
                                 member->as.callable.params[index].name.data,
                                 member->as.callable.params[index].name.length) ||
            !string_append_cstr(buffer, ": ") ||
            !parameter_type_to_string(buffer, &member->as.callable.params[index])) {
            return false;
        }
    }
    return string_append_cstr(buffer, "): ") &&
           type_ref_to_string(buffer, member->as.callable.return_type);
}

static char *normalize_doc_comment(FengSlice raw) {
    FengLspString out = {0};
    const char *cursor;
    const char *end;
    bool first_line = true;

    if (raw.data == NULL || raw.length == 0U) {
        return NULL;
    }
    if (raw.length < 5U || strncmp(raw.data, "/**", 3U) != 0) {
        return dup_range(raw.data, raw.data + raw.length);
    }
    cursor = raw.data + 3U;
    end = raw.data + raw.length - 2U;
    while (cursor < end) {
        const char *line_start = cursor;
        const char *line_end = cursor;

        while (line_end < end && *line_end != '\n') {
            ++line_end;
        }
        while (line_start < line_end && (*line_start == ' ' || *line_start == '\t' || *line_start == '\r')) {
            ++line_start;
        }
        if (line_start < line_end && *line_start == '*') {
            ++line_start;
            if (line_start < line_end && (*line_start == ' ' || *line_start == '\t')) {
                ++line_start;
            }
        }
        while (line_end > line_start && (*(line_end - 1) == ' ' || *(line_end - 1) == '\t' || *(line_end - 1) == '\r')) {
            --line_end;
        }
        if (line_end > line_start) {
            if (!first_line && !string_append_cstr(&out, "\n")) {
                string_dispose(&out);
                return NULL;
            }
            if (!string_append_bytes(&out, line_start, (size_t)(line_end - line_start))) {
                string_dispose(&out);
                return NULL;
            }
            first_line = false;
        }
        cursor = line_end < end ? line_end + 1U : line_end;
    }
    return out.data;
}

static FengLspMarkupKind hover_markup_kind_from_initialize_params(FengLspJsonValue params) {
    FengLspJsonValue capabilities = {0};
    FengLspJsonValue text_document = {0};
    FengLspJsonValue hover = {0};
    FengLspJsonValue content_format = {0};
    FengLspJsonValue format = {0};
    size_t index = 0U;

    if (!json_object_get(params, "capabilities", &capabilities) ||
        !json_object_get(capabilities, "textDocument", &text_document) ||
        !json_object_get(text_document, "hover", &hover) ||
        !json_object_get(hover, "contentFormat", &content_format)) {
        return FENG_LSP_MARKUP_PLAINTEXT;
    }
    while (json_array_get(content_format, index, &format)) {
        if (json_string_equals(format, "markdown")) {
            return FENG_LSP_MARKUP_MARKDOWN;
        }
        ++index;
    }
    return FENG_LSP_MARKUP_PLAINTEXT;
}

static bool append_hover_doc_markdown(FengLspString *buffer, const char *doc_text) {
    const char *cursor = doc_text;
    bool wrote_any = false;
    bool previous_was_tag = false;
    bool pending_blank = false;

    if (buffer == NULL || doc_text == NULL) {
        return false;
    }
    while (*cursor != '\0') {
        const char *line_start = cursor;
        const char *line_end = cursor;

        while (*line_end != '\0' && *line_end != '\n') {
            ++line_end;
        }
        if (line_start == line_end) {
            pending_blank = wrote_any;
            previous_was_tag = false;
        } else if (*line_start == '@') {
            const char *tag_end = line_start + 1U;
            const char *name_start;
            const char *name_end;
            const char *desc_start;
            const char *detail_start;
            bool highlight_first_word = false;

            while (tag_end < line_end && !isspace((unsigned char)*tag_end)) {
                ++tag_end;
            }
            name_start = tag_end;
            while (name_start < line_end && isspace((unsigned char)*name_start)) {
                ++name_start;
            }
            name_end = name_start;
            while (name_end < line_end && !isspace((unsigned char)*name_end)) {
                ++name_end;
            }
            desc_start = name_end;
            while (desc_start < line_end && isspace((unsigned char)*desc_start)) {
                ++desc_start;
            }
            highlight_first_word = (size_t)(tag_end - line_start) == 6U &&
                                   strncmp(line_start, "@param", 6U) == 0;
            detail_start = highlight_first_word ? desc_start : name_start;
            if (wrote_any &&
                !string_append_cstr(buffer,
                                    pending_blank || !previous_was_tag ? "\n\n" : "\n")) {
                return false;
            }
            if (!string_append_cstr(buffer, "- **") ||
                !string_append_bytes(buffer, line_start, (size_t)(tag_end - line_start)) ||
                !string_append_cstr(buffer, "**")) {
                return false;
            }
            if (highlight_first_word && name_end > name_start) {
                if (!string_append_cstr(buffer, " `") ||
                    !string_append_bytes(buffer, name_start, (size_t)(name_end - name_start)) ||
                    !string_append_cstr(buffer, "`")) {
                    return false;
                }
            }
            if (detail_start < line_end) {
                if (!string_append_cstr(buffer, " ") ||
                    !string_append_bytes(buffer, detail_start, (size_t)(line_end - detail_start))) {
                    return false;
                }
            }
            wrote_any = true;
            previous_was_tag = true;
            pending_blank = false;
        } else {
            if (wrote_any &&
                !string_append_cstr(buffer,
                                    pending_blank || previous_was_tag ? "\n\n" : "\n")) {
                return false;
            }
            if (!string_append_bytes(buffer, line_start, (size_t)(line_end - line_start))) {
                return false;
            }
            wrote_any = true;
            previous_was_tag = false;
            pending_blank = false;
        }
        cursor = *line_end == '\n' ? line_end + 1U : line_end;
    }
    return true;
}

static char *hover_markdown_from_plaintext(const char *hover_text) {
    FengLspString out = {0};
    const char *doc = NULL;
    const char *signature_end;

    if (hover_text == NULL) {
        return NULL;
    }
    doc = strstr(hover_text, "\n\n");
    signature_end = doc != NULL ? doc : hover_text + strlen(hover_text);
    if (!string_append_cstr(&out, "```feng\n") ||
        !string_append_bytes(&out, hover_text, (size_t)(signature_end - hover_text)) ||
        !string_append_cstr(&out, "\n```")) {
        string_dispose(&out);
        return NULL;
    }
    if (doc != NULL && doc[2] != '\0') {
        if (!string_append_cstr(&out, "\n\n") ||
            !append_hover_doc_markdown(&out, doc + 2U)) {
            string_dispose(&out);
            return NULL;
        }
    }
    return out.data;
}

static bool build_hover_result_json(FengLspString *result,
                                    FengLspMarkupKind markup_kind,
                                    const char *hover_text) {
    char *markdown = NULL;
    const char *contents = hover_text;
    const char *kind = markup_kind == FENG_LSP_MARKUP_MARKDOWN ? "markdown" : "plaintext";
    bool ok;

    if (result == NULL || hover_text == NULL) {
        return false;
    }
    if (markup_kind == FENG_LSP_MARKUP_MARKDOWN) {
        markdown = hover_markdown_from_plaintext(hover_text);
        if (markdown == NULL) {
            return false;
        }
        contents = markdown;
    }
    ok = string_append_cstr(result, "{\"contents\":{\"kind\":") &&
         string_append_json_string(result, kind) &&
         string_append_cstr(result, ",\"value\":") &&
         string_append_json_string(result, contents) &&
         string_append_cstr(result, "}}");
    free(markdown);
    return ok;
}

static char *hover_text_for_target(const FengLspAnalysisSession *session,
                                   const FengProgram *program,
                                   const FengLspResolvedTarget *target) {
    FengLspString signature = {0};
    char *doc = NULL;
    char *result;

    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            if (!decl_signature_to_string_with_session(&signature, session, target->decl)) {
                string_dispose(&signature);
                return NULL;
            }
            doc = normalize_doc_comment(target->decl->doc_comment);
            break;
        case FENG_LSP_RESOLVED_MEMBER:
            if (!member_signature_to_string_with_session(&signature, session, target->member)) {
                string_dispose(&signature);
                return NULL;
            }
            doc = normalize_doc_comment(target->member->doc_comment);
            break;
        case FENG_LSP_RESOLVED_PARAM:
            if (!string_append_cstr(&signature,
                                    target->parameter->mutability == FENG_MUTABILITY_VAR
                                        ? "var "
                                        : "let ") ||
                !string_append_bytes(&signature,
                                     target->parameter->name.data,
                                     target->parameter->name.length) ||
                !string_append_cstr(&signature, ": ") ||
                !type_ref_to_string(&signature, target->parameter->type)) {
                string_dispose(&signature);
                return NULL;
            }
            break;
        case FENG_LSP_RESOLVED_BINDING:
            (void)program;
            if (!binding_signature_to_string(&signature, session, target->binding)) {
                string_dispose(&signature);
                return NULL;
            }
            break;
        case FENG_LSP_RESOLVED_SELF:
            if (!string_append_cstr(&signature, "self: ") ||
                !string_append_bytes(&signature,
                                     decl_name(target->self_owner_decl).data,
                                     decl_name(target->self_owner_decl).length)) {
                string_dispose(&signature);
                return NULL;
            }
            break;
        default:
            return NULL;
    }
    result = signature.data;
    if (doc != NULL && doc[0] != '\0') {
        if (!string_append_cstr(&signature, "\n\n") || !string_append_cstr(&signature, doc)) {
            free(doc);
            string_dispose(&signature);
            return NULL;
        }
        result = signature.data;
    }
    free(doc);
    return result;
}

static bool symbol_type_to_string(FengLspString *buffer, const FengSymbolTypeView *type) {
    size_t index;

    if (type == NULL) {
        return string_append_cstr(buffer, "void");
    }
    switch (feng_symbol_type_kind(type)) {
        case FENG_SYMBOL_TYPE_KIND_BUILTIN: {
            FengSlice name = feng_symbol_type_builtin_name(type);
            return string_append_bytes(buffer, name.data, name.length);
        }
        case FENG_SYMBOL_TYPE_KIND_NAMED:
            for (index = 0U; index < feng_symbol_type_segment_count(type); ++index) {
                FengSlice segment = feng_symbol_type_segment_at(type, index);

                if (index > 0U && !string_append_cstr(buffer, ".")) {
                    return false;
                }
                if (!string_append_bytes(buffer, segment.data, segment.length)) {
                    return false;
                }
            }
            return true;
        case FENG_SYMBOL_TYPE_KIND_POINTER:
            return string_append_cstr(buffer, "*") &&
                   symbol_type_to_string(buffer, feng_symbol_type_inner(type));
        case FENG_SYMBOL_TYPE_KIND_ARRAY:
            if (!symbol_type_to_string(buffer, feng_symbol_type_inner(type))) {
                return false;
            }
            for (index = 0U; index < feng_symbol_type_array_rank(type); ++index) {
                if (!string_append_cstr(buffer,
                                        feng_symbol_type_array_layer_writable(type, index)
                                            ? "[!]"
                                            : "[]")) {
                    return false;
                }
            }
            return true;
        case FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF: {
            FengSlice pname = feng_symbol_type_type_param_ref_name(type);
            return string_append_bytes(buffer, pname.data, pname.length);
        }
        case FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC: {
            size_t arg_count;

            for (index = 0U; index < feng_symbol_type_segment_count(type); ++index) {
                FengSlice segment = feng_symbol_type_segment_at(type, index);

                if (index > 0U && !string_append_cstr(buffer, ".")) {
                    return false;
                }
                if (!string_append_bytes(buffer, segment.data, segment.length)) {
                    return false;
                }
            }
            arg_count = feng_symbol_type_generic_arg_count(type);
            if (arg_count > 0U) {
                if (!string_append_cstr(buffer, "<")) {
                    return false;
                }
                for (index = 0U; index < arg_count; ++index) {
                    if (index > 0U && !string_append_cstr(buffer, ", ")) {
                        return false;
                    }
                    if (!symbol_type_to_string(buffer, feng_symbol_type_generic_arg_at(type, index))) {
                        return false;
                    }
                }
                if (!string_append_cstr(buffer, ">")) {
                    return false;
                }
            }
            return true;
        }
        case FENG_SYMBOL_TYPE_KIND_INVALID:
            return false;
    }
    return false;
}

static bool symbol_param_type_to_string(FengLspString *buffer,
                                        const FengSymbolDeclView *decl,
                                        size_t param_index) {
    const FengSymbolTypeView *type = feng_symbol_decl_param_type(decl, param_index);

    if (feng_symbol_decl_param_is_variadic(decl, param_index)) {
        if (type != NULL && feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_ARRAY) {
            size_t rank = feng_symbol_type_array_rank(type);
            size_t layer_index;

            if (rank > 0U) {
                if (!symbol_type_to_string(buffer, feng_symbol_type_inner(type))) {
                    return false;
                }
                for (layer_index = 1U; layer_index < rank; ++layer_index) {
                    if (!string_append_cstr(buffer,
                                            feng_symbol_type_array_layer_writable(type, layer_index)
                                                ? "[!]"
                                                : "[]")) {
                        return false;
                    }
                }
                return string_append_cstr(buffer, "...");
            }
        }
        return symbol_type_to_string(buffer, type) && string_append_cstr(buffer, "...");
    }
    return symbol_type_to_string(buffer, type);
}

static bool symbol_decl_signature_to_string(FengLspString *buffer,
                                            const FengSymbolDeclView *decl) {
    size_t index;
    FengSlice name = feng_symbol_decl_name(decl);

    switch (feng_symbol_decl_kind(decl)) {
        case FENG_SYMBOL_DECL_KIND_BINDING:
            return string_append_cstr(buffer,
                                      feng_symbol_decl_mutability(decl) == FENG_MUTABILITY_VAR ? "var " : "let ") &&
                   string_append_bytes(buffer, name.data, name.length) &&
                   string_append_cstr(buffer, ": ") &&
                   symbol_type_to_string(buffer, feng_symbol_decl_value_type(decl));
        case FENG_SYMBOL_DECL_KIND_TYPE: {
            size_t mi;

            if (!string_append_cstr(buffer, "type ") ||
                !string_append_bytes(buffer, name.data, name.length)) {
                return false;
            }
            if (feng_symbol_decl_type_param_count(decl) > 0U) {
                if (!string_append_cstr(buffer, "<")) {
                    return false;
                }
                for (mi = 0U, index = 0U; mi < feng_symbol_decl_member_count(decl); ++mi) {
                    const FengSymbolDeclView *m = feng_symbol_decl_member_at(decl, mi);

                    if (m == NULL || feng_symbol_decl_kind(m) != FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
                        continue;
                    }
                    if (index > 0U && !string_append_cstr(buffer, ", ")) {
                        return false;
                    }
                    {
                        FengSlice pname = feng_symbol_decl_name(m);
                        if (!string_append_bytes(buffer, pname.data, pname.length)) {
                            return false;
                        }
                    }
                    ++index;
                }
                if (!string_append_cstr(buffer, ">")) {
                    return false;
                }
            }
            return true;
        }
        case FENG_SYMBOL_DECL_KIND_SPEC: {
            size_t mi;

            if (!string_append_cstr(buffer, "spec ") ||
                !string_append_bytes(buffer, name.data, name.length)) {
                return false;
            }
            if (feng_symbol_decl_type_param_count(decl) > 0U) {
                if (!string_append_cstr(buffer, "<")) {
                    return false;
                }
                for (mi = 0U, index = 0U; mi < feng_symbol_decl_member_count(decl); ++mi) {
                    const FengSymbolDeclView *m = feng_symbol_decl_member_at(decl, mi);

                    if (m == NULL || feng_symbol_decl_kind(m) != FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
                        continue;
                    }
                    if (index > 0U && !string_append_cstr(buffer, ", ")) {
                        return false;
                    }
                    {
                        FengSlice pname = feng_symbol_decl_name(m);
                        if (!string_append_bytes(buffer, pname.data, pname.length)) {
                            return false;
                        }
                    }
                    ++index;
                }
                if (!string_append_cstr(buffer, ">")) {
                    return false;
                }
            }
            return true;
        }
        case FENG_SYMBOL_DECL_KIND_ENUM:
            return string_append_cstr(buffer, "enum ") &&
                   string_append_bytes(buffer, name.data, name.length);
        case FENG_SYMBOL_DECL_KIND_FIT:
            return string_append_cstr(buffer, "fit");
        case FENG_SYMBOL_DECL_KIND_FUNCTION: {
            size_t mi;

            if (!string_append_cstr(buffer, "func ") ||
                !string_append_bytes(buffer, name.data, name.length)) {
                return false;
            }
            if (feng_symbol_decl_type_param_count(decl) > 0U) {
                if (!string_append_cstr(buffer, "<")) {
                    return false;
                }
                for (mi = 0U, index = 0U; mi < feng_symbol_decl_member_count(decl); ++mi) {
                    const FengSymbolDeclView *m = feng_symbol_decl_member_at(decl, mi);

                    if (m == NULL || feng_symbol_decl_kind(m) != FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
                        continue;
                    }
                    if (index > 0U && !string_append_cstr(buffer, ", ")) {
                        return false;
                    }
                    {
                        FengSlice pname = feng_symbol_decl_name(m);
                        if (!string_append_bytes(buffer, pname.data, pname.length)) {
                            return false;
                        }
                    }
                    ++index;
                }
                if (!string_append_cstr(buffer, ">")) {
                    return false;
                }
            }
            if (!string_append_cstr(buffer, "(")) {
                return false;
            }
            for (index = 0U; index < feng_symbol_decl_param_count(decl); ++index) {
                FengSlice param_name = feng_symbol_decl_param_name(decl, index);

                if (index > 0U && !string_append_cstr(buffer, ", ")) {
                    return false;
                }
                if (!string_append_bytes(buffer, param_name.data, param_name.length) ||
                    !string_append_cstr(buffer, ": ") ||
                    !symbol_param_type_to_string(buffer, decl, index)) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   symbol_type_to_string(buffer, feng_symbol_decl_return_type(decl));
        }
        case FENG_SYMBOL_DECL_KIND_MODULE:
        case FENG_SYMBOL_DECL_KIND_FIELD:
        case FENG_SYMBOL_DECL_KIND_METHOD:
        case FENG_SYMBOL_DECL_KIND_CONSTRUCTOR:
        case FENG_SYMBOL_DECL_KIND_FINALIZER:
        case FENG_SYMBOL_DECL_KIND_ENUM_ITEM:
            break;
        case FENG_SYMBOL_DECL_KIND_TYPE_PARAM:
            break;
    }
    return false;
}

static bool symbol_member_signature_to_string(FengLspString *buffer,
                                              const FengSymbolDeclView *member) {
    size_t index;
    FengSlice name = feng_symbol_decl_name(member);
    FengSymbolDeclKind kind = feng_symbol_decl_kind(member);

    if (kind == FENG_SYMBOL_DECL_KIND_FIELD) {
        return string_append_cstr(buffer,
                                  feng_symbol_decl_mutability(member) == FENG_MUTABILITY_VAR ? "var " : "let ") &&
               string_append_bytes(buffer, name.data, name.length) &&
               string_append_cstr(buffer, ": ") &&
               symbol_type_to_string(buffer, feng_symbol_decl_value_type(member));
    }
    if (kind == FENG_SYMBOL_DECL_KIND_ENUM_ITEM) {
        if (!string_append_bytes(buffer, name.data, name.length)) {
            return false;
        }
        if (feng_symbol_decl_has_enum_item_value(member)) {
            return string_append_format(buffer,
                                        " = %lld",
                                        (long long)feng_symbol_decl_enum_item_value(member));
        }
        return true;
    }
    if (!string_append_cstr(buffer,
                            kind == FENG_SYMBOL_DECL_KIND_CONSTRUCTOR ? "ctor " :
                            kind == FENG_SYMBOL_DECL_KIND_FINALIZER ? "finalizer " : "func ") ||
        !string_append_bytes(buffer, name.data, name.length)) {
        return false;
    }
    if (feng_symbol_decl_type_param_count(member) > 0U) {
        size_t mi;

        if (!string_append_cstr(buffer, "<")) {
            return false;
        }
        for (mi = 0U, index = 0U; mi < feng_symbol_decl_member_count(member); ++mi) {
            const FengSymbolDeclView *m = feng_symbol_decl_member_at(member, mi);

            if (m == NULL || feng_symbol_decl_kind(m) != FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
                continue;
            }
            if (index > 0U && !string_append_cstr(buffer, ", ")) {
                return false;
            }
            {
                FengSlice pname = feng_symbol_decl_name(m);
                if (!string_append_bytes(buffer, pname.data, pname.length)) {
                    return false;
                }
            }
            ++index;
        }
        if (!string_append_cstr(buffer, ">")) {
            return false;
        }
    }
    if (!string_append_cstr(buffer, "(")) {
        return false;
    }
    for (index = 0U; index < feng_symbol_decl_param_count(member); ++index) {
        FengSlice param_name = feng_symbol_decl_param_name(member, index);

        if (index > 0U && !string_append_cstr(buffer, ", ")) {
            return false;
        }
        if (!string_append_bytes(buffer, param_name.data, param_name.length) ||
            !string_append_cstr(buffer, ": ") ||
            !symbol_param_type_to_string(buffer, member, index)) {
            return false;
        }
    }
    return string_append_cstr(buffer, "): ") &&
           symbol_type_to_string(buffer, feng_symbol_decl_return_type(member));
}

static char *hover_text_for_cache_target(const FengLspCacheResolvedTarget *target) {
    FengLspString signature = {0};
    FengSlice doc = {0};

    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            if (!symbol_decl_signature_to_string(&signature, target->decl)) {
                string_dispose(&signature);
                return NULL;
            }
            doc = feng_symbol_decl_doc(target->decl);
            break;
        case FENG_LSP_RESOLVED_MEMBER:
            if (!symbol_member_signature_to_string(&signature, target->member)) {
                string_dispose(&signature);
                return NULL;
            }
            doc = feng_symbol_decl_doc(target->member);
            break;
        case FENG_LSP_RESOLVED_PARAM:
            if (!string_append_cstr(&signature,
                                    target->parameter->mutability == FENG_MUTABILITY_VAR
                                        ? "var "
                                        : "let ") ||
                !string_append_bytes(&signature,
                                     target->parameter->name.data,
                                     target->parameter->name.length) ||
                !string_append_cstr(&signature, ": ") ||
                !type_ref_to_string(&signature, target->parameter->type)) {
                string_dispose(&signature);
                return NULL;
            }
            break;
        case FENG_LSP_RESOLVED_BINDING:
            if (!string_append_cstr(&signature,
                                    target->binding->mutability == FENG_MUTABILITY_VAR ? "var " : "let ") ||
                !string_append_bytes(&signature,
                                     target->binding->name.data,
                                     target->binding->name.length)) {
                string_dispose(&signature);
                return NULL;
            }
            if (target->binding->type != NULL) {
                if (!string_append_cstr(&signature, ": ") ||
                    !type_ref_to_string(&signature, target->binding->type)) {
                    string_dispose(&signature);
                    return NULL;
                }
            }
            break;
        case FENG_LSP_RESOLVED_SELF: {
            FengSlice name = target->self_owner_decl != NULL
                                 ? feng_symbol_decl_name(target->self_owner_decl)
                                 : (FengSlice){0};

            if (!string_append_cstr(&signature, "self: ") ||
                !string_append_bytes(&signature, name.data, name.length)) {
                string_dispose(&signature);
                return NULL;
            }
            break;
        }
        default:
            return NULL;
    }
    if (doc.data != NULL && doc.length > 0U) {
        if (!string_append_cstr(&signature, "\n\n") ||
            !string_append_bytes(&signature, doc.data, doc.length)) {
            string_dispose(&signature);
            return NULL;
        }
    }
    return signature.data;
}

static bool location_json(FengLspString *json, const char *path, FengToken token) {
    char *uri;

    if (path == NULL) {
        return string_append_cstr(json, "null");
    }
    uri = path_to_file_uri(path);
    if (uri == NULL) {
        return false;
    }
    if (!string_append_cstr(json, "{\"uri\":") ||
        !string_append_json_string(json, uri) ||
        !string_append_cstr(json, ",\"range\":{\"start\":{\"line\":") ||
        !string_append_format(json, "%u", token.line > 0U ? token.line - 1U : 0U) ||
        !string_append_cstr(json, ",\"character\":") ||
        !string_append_format(json, "%u", token.column > 0U ? token.column - 1U : 0U) ||
        !string_append_cstr(json, "},\"end\":{\"line\":") ||
        !string_append_format(json, "%u", token.line > 0U ? token.line - 1U : 0U) ||
        !string_append_cstr(json, ",\"character\":") ||
        !string_append_format(json, "%u", token.column > 0U ? token.column - 1U + (unsigned int)token.length : (unsigned int)token.length) ||
        !string_append_cstr(json, "}}}")) {
        free(uri);
        return false;
    }
    free(uri);
    return true;
}

static bool range_json_offsets(FengLspString *json,
                               const char *text,
                               size_t start_offset,
                               size_t end_offset) {
    unsigned int start_line;
    unsigned int start_character;
    unsigned int end_line;
    unsigned int end_character;

    if (json == NULL || text == NULL) {
        return false;
    }
    position_from_offset(text, start_offset, &start_line, &start_character);
    position_from_offset(text, end_offset, &end_line, &end_character);
    return string_append_cstr(json, "{\"start\":{\"line\":") &&
           string_append_format(json, "%u", start_line) &&
           string_append_cstr(json, ",\"character\":") &&
           string_append_format(json, "%u", start_character) &&
           string_append_cstr(json, "},\"end\":{\"line\":") &&
           string_append_format(json, "%u", end_line) &&
           string_append_cstr(json, ",\"character\":") &&
           string_append_format(json, "%u", end_character) &&
           string_append_cstr(json, "}}");
}

static bool location_json_offsets(FengLspString *json,
                                  const char *path,
                                  const char *text,
                                  size_t start_offset,
                                  size_t end_offset) {
    char *uri;

    if (path == NULL || text == NULL) {
        return string_append_cstr(json, "null");
    }
    uri = path_to_file_uri(path);
    if (uri == NULL) {
        return false;
    }
    if (!string_append_cstr(json, "{\"uri\":") ||
        !string_append_json_string(json, uri) ||
        !string_append_cstr(json, ",\"range\":") ||
        !range_json_offsets(json, text, start_offset, end_offset) ||
        !string_append_cstr(json, "}")) {
        free(uri);
        return false;
    }
    free(uri);
    return true;
}

static void reference_list_dispose(FengLspReferenceList *references) {
    if (references == NULL) {
        return;
    }
    free(references->items);
    references->items = NULL;
    references->count = 0U;
    references->capacity = 0U;
}

static bool reference_list_contains(const FengLspReferenceList *references,
                                    const char *path,
                                    size_t start_offset,
                                    size_t end_offset) {
    size_t index;

    if (references == NULL || path == NULL) {
        return false;
    }
    for (index = 0U; index < references->count; ++index) {
        const FengLspReferenceEntry *entry = &references->items[index];

        if (strcmp(entry->path, path) == 0 &&
            entry->start_offset == start_offset &&
            entry->end_offset == end_offset) {
            return true;
        }
    }
    return false;
}

static bool reference_list_push(FengLspReferenceList *references,
                                const char *path,
                                size_t start_offset,
                                size_t end_offset) {
    FengLspReferenceEntry entry;

    if (references == NULL || path == NULL || end_offset < start_offset) {
        return false;
    }
    if (reference_list_contains(references, path, start_offset, end_offset)) {
        return true;
    }
    entry.path = path;
    entry.start_offset = start_offset;
    entry.end_offset = end_offset;
    return append_raw((void **)&references->items,
                      &references->count,
                      &references->capacity,
                      sizeof(entry),
                      &entry);
}

static bool slice_offsets_in_source(const FengCliLoadedSource *source,
                                    FengSlice slice,
                                    size_t *out_start_offset,
                                    size_t *out_end_offset) {
    const char *source_end;
    const char *slice_end;

    if (source == NULL || source->source == NULL || slice.data == NULL || slice.length == 0U) {
        return false;
    }
    source_end = source->source + source->source_length;
    slice_end = slice.data + slice.length;
    if (slice.data < source->source || slice_end > source_end) {
        return false;
    }
    if (out_start_offset != NULL) {
        *out_start_offset = (size_t)(slice.data - source->source);
    }
    if (out_end_offset != NULL) {
        *out_end_offset = (size_t)(slice_end - source->source);
    }
    return true;
}

static bool reference_list_push_slice(FengLspReferenceList *references,
                                      const FengCliLoadedSource *source,
                                      FengSlice slice) {
    size_t start_offset;
    size_t end_offset;

    if (!slice_offsets_in_source(source, slice, &start_offset, &end_offset)) {
        return true;
    }
    return reference_list_push(references, source->path, start_offset, end_offset);
}

static const FengLspReferenceEntry *reference_list_find_offset(const FengLspReferenceList *references,
                                                               const char *path,
                                                               size_t offset) {
    size_t index;

    if (references == NULL || path == NULL) {
        return NULL;
    }
    for (index = 0U; index < references->count; ++index) {
        const FengLspReferenceEntry *entry = &references->items[index];

        if (strcmp(entry->path, path) == 0 &&
            offset >= entry->start_offset &&
            offset <= entry->end_offset) {
            return entry;
        }
    }
    return NULL;
}

static bool resolve_callable_target(const FengResolvedCallable *callable,
                                    FengLspResolvedTarget *target) {
    memset(target, 0, sizeof(*target));
    switch (callable->kind) {
        case FENG_RESOLVED_CALLABLE_FUNCTION:
            target->kind = FENG_LSP_RESOLVED_DECL;
            target->decl = callable->function_decl;
            return callable->function_decl != NULL;
        case FENG_RESOLVED_CALLABLE_TYPE_METHOD:
        case FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD:
        case FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR:
        case FENG_RESOLVED_CALLABLE_FIT_METHOD:
        case FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD:
            target->kind = FENG_LSP_RESOLVED_MEMBER;
            target->decl = (callable->kind == FENG_RESOLVED_CALLABLE_FIT_METHOD ||
                            callable->kind == FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD)
                               ? callable->fit_decl
                               : callable->owner_type_decl;
            target->member = callable->member;
            return target->decl != NULL && target->member != NULL;
        case FENG_RESOLVED_CALLABLE_NONE:
            return false;
    }
    return false;
}

static const FengExpr *find_call_hit_expr(const FengExpr *expr, size_t offset) {
    size_t index;

    if (expr == NULL || offset < expr_start(expr) || offset > expr_end(expr)) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_CALL && expr->as.call.callee != NULL &&
        offset >= expr->as.call.callee->token.offset && offset <= expr_end(expr->as.call.callee)) {
        return expr;
    }
    switch (expr->kind) {
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                const FengExpr *hit = find_call_hit_expr(expr->as.array_literal.items[index], offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        case FENG_EXPR_GENERIC_TARGET:
            return find_call_hit_expr(expr->as.generic_target.target, offset);
        case FENG_EXPR_OBJECT_LITERAL:
            if (expr->as.object_literal.target != NULL) {
                const FengExpr *hit = find_call_hit_expr(expr->as.object_literal.target, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                const FengExpr *hit = find_call_hit_expr(expr->as.object_literal.fields[index].value, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        case FENG_EXPR_CALL: {
            const FengExpr *hit = find_call_hit_expr(expr->as.call.callee, offset);
            if (hit != NULL) {
                return hit;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                hit = find_call_hit_expr(expr->as.call.args[index], offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        }
        case FENG_EXPR_MEMBER:
            return find_call_hit_expr(expr->as.member.object, offset);
        case FENG_EXPR_INDEX: {
            const FengExpr *hit = find_call_hit_expr(expr->as.index.object, offset);
            if (hit != NULL) {
                return hit;
            }
            return find_call_hit_expr(expr->as.index.index, offset);
        }
        case FENG_EXPR_UNARY:
            return find_call_hit_expr(expr->as.unary.operand, offset);
        case FENG_EXPR_BINARY: {
            const FengExpr *hit = find_call_hit_expr(expr->as.binary.left, offset);
            if (hit != NULL) {
                return hit;
            }
            return find_call_hit_expr(expr->as.binary.right, offset);
        }
        case FENG_EXPR_CAST:
            return find_call_hit_expr(expr->as.cast.value, offset);
        case FENG_EXPR_IF: {
            const FengExpr *hit = find_call_hit_expr(expr->as.if_expr.condition, offset);
            if (hit != NULL) {
                return hit;
            }
            break;
        }
        case FENG_EXPR_MATCH:
            return find_call_hit_expr(expr->as.match_expr.target, offset);
        case FENG_EXPR_TRY:
            return find_call_hit_expr(expr->as.try_expr.body, offset);
        default:
            break;
    }
    return NULL;
}

static const FengExpr *find_call_hit_in_block(const FengBlock *block, size_t offset) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return NULL;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        const FengStmt *stmt = block->statements[index];
        const FengExpr *hit = NULL;

        if (offset < stmt->token.offset || offset > stmt_end(stmt)) {
            continue;
        }
        switch (stmt->kind) {
            case FENG_STMT_BINDING:
                hit = find_call_hit_expr(stmt->as.binding.initializer, offset);
                break;
            case FENG_STMT_ASSIGN:
                hit = find_call_hit_expr(stmt->as.assign.target, offset);
                if (hit == NULL) {
                    hit = find_call_hit_expr(stmt->as.assign.value, offset);
                }
                break;
            case FENG_STMT_TRY:
            case FENG_STMT_EXPR:
                hit = find_call_hit_expr(stmt->as.expr, offset);
                break;
            case FENG_STMT_BLOCK:
                hit = find_call_hit_in_block(stmt->as.block, offset);
                break;
            case FENG_STMT_IF: {
                size_t clause_index;
                for (clause_index = 0U; clause_index < stmt->as.if_stmt.clause_count && hit == NULL; ++clause_index) {
                    hit = find_call_hit_expr(stmt->as.if_stmt.clauses[clause_index].condition, offset);
                    if (hit == NULL) {
                        hit = find_call_hit_in_block(stmt->as.if_stmt.clauses[clause_index].block, offset);
                    }
                }
                if (hit == NULL) {
                    hit = find_call_hit_in_block(stmt->as.if_stmt.else_block, offset);
                }
                break;
            }
            case FENG_STMT_MATCH: {
                size_t branch_index;
                hit = find_call_hit_expr(stmt->as.match_stmt.target, offset);
                for (branch_index = 0U; branch_index < stmt->as.match_stmt.branch_count && hit == NULL; ++branch_index) {
                    hit = find_call_hit_in_block(stmt->as.match_stmt.branches[branch_index].body, offset);
                }
                if (hit == NULL) {
                    hit = find_call_hit_in_block(stmt->as.match_stmt.else_block, offset);
                }
                break;
            }
            case FENG_STMT_WHILE:
                hit = find_call_hit_expr(stmt->as.while_stmt.condition, offset);
                if (hit == NULL) {
                    hit = find_call_hit_in_block(stmt->as.while_stmt.body, offset);
                }
                break;
            case FENG_STMT_FOR:
                if (stmt->as.for_stmt.is_for_in) {
                    hit = find_call_hit_expr(stmt->as.for_stmt.iter_expr, offset);
                    if (hit == NULL) {
                        hit = find_call_hit_in_block(stmt->as.for_stmt.body, offset);
                    }
                } else {
                    if (stmt->as.for_stmt.init != NULL && stmt->as.for_stmt.init->kind == FENG_STMT_EXPR) {
                        hit = find_call_hit_expr(stmt->as.for_stmt.init->as.expr, offset);
                    }
                    if (hit == NULL) {
                        hit = find_call_hit_expr(stmt->as.for_stmt.condition, offset);
                    }
                    if (hit == NULL && stmt->as.for_stmt.update != NULL && stmt->as.for_stmt.update->kind == FENG_STMT_EXPR) {
                        hit = find_call_hit_expr(stmt->as.for_stmt.update->as.expr, offset);
                    }
                    if (hit == NULL) {
                        hit = find_call_hit_in_block(stmt->as.for_stmt.body, offset);
                    }
                }
                break;
            case FENG_STMT_RETURN:
                hit = find_call_hit_expr(stmt->as.return_value, offset);
                break;
            case FENG_STMT_THROW:
                hit = find_call_hit_expr(stmt->as.throw_value, offset);
                break;
            case FENG_STMT_BREAK:
            case FENG_STMT_CONTINUE:
                break;
        }
        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

static const FengExpr *find_call_hit_in_decl(const FengDecl *decl, size_t offset) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return find_call_hit_expr(decl->as.binding.initializer, offset);
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_FUNCTION:
            return find_call_hit_in_block(decl->as.function_decl.body, offset);
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];
                const FengExpr *hit = member->kind == FENG_TYPE_MEMBER_FIELD
                                          ? find_call_hit_expr(member->as.field.initializer, offset)
                                          : find_call_hit_in_block(member->as.callable.body, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    const FengTypeMember *member = decl->as.spec_decl.as.object.members[index];
                    const FengExpr *hit = member->kind == FENG_TYPE_MEMBER_FIELD
                                              ? find_call_hit_expr(member->as.field.initializer, offset)
                                              : find_call_hit_in_block(member->as.callable.body, offset);
                    if (hit != NULL) {
                        return hit;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.fit_decl.members[index];
                const FengExpr *hit = member->kind == FENG_TYPE_MEMBER_FIELD
                                          ? find_call_hit_expr(member->as.field.initializer, offset)
                                          : find_call_hit_in_block(member->as.callable.body, offset);
                if (hit != NULL) {
                    return hit;
                }
            }
            break;
        default:
            break;
    }
    return NULL;
}

static const FengDecl *resolve_expr_target(const FengLspAnalysisSession *session,
                                           const FengProgram *program,
                                           const FengExpr *expr,
                                           const FengLspLocalList *locals,
                                           FengLspResolvedTarget *target) {
    const FengSemanticModule *program_module = find_program_module(session, program);

    memset(target, 0, sizeof(*target));
    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_SELF) {
        target->kind = FENG_LSP_RESOLVED_SELF;
        return NULL;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        const FengLspLocal *local = find_local(locals, expr->as.identifier);
        if (local != NULL) {
            if (local->kind == FENG_LSP_LOCAL_PARAM) {
                target->kind = FENG_LSP_RESOLVED_PARAM;
                target->parameter = local->parameter;
                return NULL;
            }
            if (local->kind == FENG_LSP_LOCAL_BINDING) {
                target->kind = FENG_LSP_RESOLVED_BINDING;
                target->binding = local->binding;
                return NULL;
            }
            target->kind = FENG_LSP_RESOLVED_SELF;
            target->self_owner_decl = local->self_owner_decl;
            return NULL;
        }
        target->decl = resolve_value_name(session, program, expr->as.identifier);
        if (target->decl == NULL) {
            target->decl = resolve_type_name(session, program, expr->as.identifier);
        }
        if (target->decl != NULL) {
            target->kind = FENG_LSP_RESOLVED_DECL;
            return target->decl;
        }
        return NULL;
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL) {
        if (expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
            const FengLspLocal *local = find_local(locals, expr->as.member.object->as.identifier);
            if (local == NULL && program_module != NULL &&
                find_module_decl_by_name(program_module,
                                         expr->as.member.object->as.identifier,
                                         false,
                                         false,
                                         false) == NULL) {
                const FengSemanticModule *alias_module = find_alias_module(session,
                                                                           program,
                                                                           expr->as.member.object->as.identifier);
                if (alias_module != NULL) {
                    target->decl = find_module_decl_by_name(alias_module,
                                                            expr->as.member.member,
                                                            false,
                                                            false,
                                                            true);
                    if (target->decl != NULL) {
                        target->kind = FENG_LSP_RESOLVED_DECL;
                        return target->decl;
                    }
                }
            }
        }
        if (session->analysis != NULL) {
            const FengSpecMemberAccess *spec_access = feng_semantic_lookup_spec_member_access(session->analysis, expr);
            if (spec_access != NULL) {
                target->kind = FENG_LSP_RESOLVED_MEMBER;
                target->decl = spec_access->spec_decl;
                target->member = spec_access->member;
                return spec_access->spec_decl;
            }
        }
        target->decl = resolve_owner_decl_from_object_expr(session,
                                                           program,
                                                           expr->as.member.object,
                                                           locals);
        if (target->decl != NULL) {
            target->member = find_member_by_name(target->decl, expr->as.member.member);
            if (target->member != NULL) {
                target->kind = FENG_LSP_RESOLVED_MEMBER;
                return target->decl;
            }
        }
    }
    return NULL;
}

static bool find_local_binding_at_in_block(const char *source_text,
                                           const FengBlock *block,
                                           size_t offset,
                                           FengLspResolvedTarget *target);

static bool find_local_binding_at_in_stmt(const char *source_text,
                                          const FengStmt *stmt,
                                          size_t offset,
                                          FengLspResolvedTarget *target) {
    size_t name_start;
    size_t index;

    if (stmt == NULL) {
        return false;
    }
    switch (stmt->kind) {
        case FENG_STMT_BINDING:
            if (stmt->as.binding.name.data != NULL) {
                name_start = (size_t)(stmt->as.binding.name.data - source_text);
                if (offset >= name_start && offset <= name_start + stmt->as.binding.name.length) {
                    target->kind = FENG_LSP_RESOLVED_BINDING;
                    target->binding = &stmt->as.binding;
                    return true;
                }
            }
            return false;
        case FENG_STMT_BLOCK:
            return find_local_binding_at_in_block(source_text, stmt->as.block, offset, target);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (find_local_binding_at_in_block(source_text,
                                                   stmt->as.if_stmt.clauses[index].block,
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            return find_local_binding_at_in_block(source_text,
                                                  stmt->as.if_stmt.else_block,
                                                  offset,
                                                  target);
        case FENG_STMT_MATCH:
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (find_local_binding_at_in_block(source_text,
                                                   stmt->as.match_stmt.branches[index].body,
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            return find_local_binding_at_in_block(source_text,
                                                  stmt->as.match_stmt.else_block,
                                                  offset,
                                                  target);
        case FENG_STMT_WHILE:
            return find_local_binding_at_in_block(source_text,
                                                  stmt->as.while_stmt.body,
                                                  offset,
                                                  target);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                if (stmt->as.for_stmt.iter_binding.name.data != NULL) {
                    name_start = (size_t)(stmt->as.for_stmt.iter_binding.name.data - source_text);
                    if (offset >= name_start &&
                        offset <= name_start + stmt->as.for_stmt.iter_binding.name.length) {
                        target->kind = FENG_LSP_RESOLVED_BINDING;
                        target->binding = &stmt->as.for_stmt.iter_binding;
                        return true;
                    }
                }
                return find_local_binding_at_in_block(source_text,
                                                      stmt->as.for_stmt.body,
                                                      offset,
                                                      target);
            }
            return (stmt->as.for_stmt.init != NULL &&
                    find_local_binding_at_in_stmt(source_text,
                                                  stmt->as.for_stmt.init,
                                                  offset,
                                                  target)) ||
                   find_local_binding_at_in_block(source_text,
                                                  stmt->as.for_stmt.body,
                                                  offset,
                                                  target);
        default:
            return false;
    }
}

static bool find_local_binding_at_in_block(const char *source_text,
                                           const FengBlock *block,
                                           size_t offset,
                                           FengLspResolvedTarget *target) {
    size_t index;

    if (block == NULL) {
        return false;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (find_local_binding_at_in_stmt(source_text, block->statements[index], offset, target)) {
            return true;
        }
    }
    return false;
}

static bool resolve_object_field_target_decl(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengDecl *decl,
                                             size_t offset,
                                             const FengLspLocalList *locals,
                                             FengLspResolvedTarget *target);

static bool resolve_target_at(const FengLspAnalysisSession *session,
                              const FengProgram *program,
                              size_t offset,
                              FengLspResolvedTarget *target) {
    size_t decl_index;
    const FengCliLoadedSource *source;
    const char *source_text;
    const FengDecl *enclosing_decl;
    const FengTypeMember *enclosing_member;
    FengLspLocalList locals = {0};
    const FengExpr *expr;

    memset(target, 0, sizeof(*target));
    enclosing_decl = find_enclosing_decl(program, offset, &enclosing_member);
    if (enclosing_decl == NULL) {
        return false;
    }
    if (!collect_visible_locals(enclosing_decl, enclosing_member, offset, &locals)) {
        local_list_dispose(&locals);
        return false;
    }
    source = find_source(session, program->path);
    source_text = source != NULL ? source->source : NULL;
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        if (find_type_ref_hit(program->declarations[decl_index], program, session, offset, target)) {
            local_list_dispose(&locals);
            return true;
        }
        if (find_decl_token_hit(source_text,
                                program->declarations[decl_index],
                                offset,
                                target)) {
            local_list_dispose(&locals);
            return true;
        }
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengExpr *call_hit = find_call_hit_in_decl(program->declarations[decl_index], offset);
        if (call_hit != NULL && resolve_callable_target(&call_hit->as.call.resolved_callable, target)) {
            local_list_dispose(&locals);
            return true;
        }
    }
    if (resolve_object_field_target_decl(session,
                                         program,
                                         enclosing_decl,
                                         offset,
                                         &locals,
                                         target)) {
        local_list_dispose(&locals);
        return true;
    }
    expr = find_expr_hit_in_decl(enclosing_decl, offset);
    if (expr != NULL) {
        (void)resolve_expr_target(session, program, expr, &locals, target);
    }
    if (target->kind == FENG_LSP_RESOLVED_NONE) {
        /* Cursor may be on a local binding name (declaration site, not a use-expr). */
        const FengCliLoadedSource *current_source = find_source(session, program->path);
        const FengBlock *body = NULL;

        if (enclosing_member != NULL && enclosing_member->kind != FENG_TYPE_MEMBER_FIELD) {
            body = enclosing_member->as.callable.body;
        } else if (enclosing_decl->kind == FENG_DECL_FUNCTION) {
            body = enclosing_decl->as.function_decl.body;
        }
        if (current_source != NULL && body != NULL) {
            (void)find_local_binding_at_in_block(current_source->source, body, offset, target);
        }
    }
    local_list_dispose(&locals);
    return target->kind != FENG_LSP_RESOLVED_NONE;
}

static bool resolved_targets_equal(const FengLspResolvedTarget *lhs,
                                   const FengLspResolvedTarget *rhs) {
    if (lhs == NULL || rhs == NULL || lhs->kind != rhs->kind) {
        return false;
    }
    switch (lhs->kind) {
        case FENG_LSP_RESOLVED_DECL:
            return lhs->decl == rhs->decl;
        case FENG_LSP_RESOLVED_MEMBER:
            return lhs->member == rhs->member;
        case FENG_LSP_RESOLVED_PARAM:
            return lhs->parameter == rhs->parameter;
        case FENG_LSP_RESOLVED_BINDING:
            return lhs->binding == rhs->binding;
        case FENG_LSP_RESOLVED_SELF:
            return lhs->self_owner_decl == rhs->self_owner_decl;
        case FENG_LSP_RESOLVED_NONE:
            return true;
    }
    return false;
}

static bool resolved_target_supports_references(const FengLspResolvedTarget *target) {
    if (target == NULL) {
        return false;
    }
    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            return target->decl != NULL && target->decl->kind != FENG_DECL_FIT;
        case FENG_LSP_RESOLVED_MEMBER:
            return target->member != NULL;
        case FENG_LSP_RESOLVED_PARAM:
            return target->parameter != NULL;
        case FENG_LSP_RESOLVED_BINDING:
            return target->binding != NULL;
        case FENG_LSP_RESOLVED_NONE:
        case FENG_LSP_RESOLVED_SELF:
            return false;
    }
    return false;
}

static bool resolved_target_can_rename(const FengLspAnalysisSession *session,
                                       const FengLspResolvedTarget *target) {
    const FengProgram *owner_program = NULL;

    if (!resolved_target_supports_references(target)) {
        return false;
    }
    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            if (target->decl == NULL) {
                return false;
            }
            if (target->decl->kind != FENG_DECL_GLOBAL_BINDING &&
                target->decl->kind != FENG_DECL_TYPE &&
                target->decl->kind != FENG_DECL_SPEC &&
                target->decl->kind != FENG_DECL_FUNCTION) {
                return false;
            }
            (void)find_decl_module(session, target->decl, &owner_program);
            return owner_program != NULL && find_source(session, owner_program->path) != NULL;
        case FENG_LSP_RESOLVED_MEMBER:
            if (target->member == NULL) {
                return false;
            }
            if (target->member->kind != FENG_TYPE_MEMBER_FIELD &&
                target->member->kind != FENG_TYPE_MEMBER_METHOD) {
                return false;
            }
            (void)find_decl_module(session, target->decl, &owner_program);
            return owner_program != NULL && find_source(session, owner_program->path) != NULL;
        case FENG_LSP_RESOLVED_PARAM:
        case FENG_LSP_RESOLVED_BINDING:
            return true;
        case FENG_LSP_RESOLVED_NONE:
        case FENG_LSP_RESOLVED_SELF:
            return false;
    }
    return false;
}

static bool identifier_name_is_valid(const char *name) {
    size_t length;
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!(name[0] == '_' || isalpha((unsigned char)name[0]))) {
        return false;
    }
    length = strlen(name);
    for (index = 1U; index < length; ++index) {
        if (!(name[index] == '_' || isalnum((unsigned char)name[index]))) {
            return false;
        }
    }
    if (feng_lookup_keyword(name, length, NULL) || feng_is_reserved_word(name, length)) {
        return false;
    }
    if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
        return false;
    }
    return true;
}

static FengSlice member_name_slice(const FengTypeMember *member) {
    if (member == NULL) {
        return (FengSlice){0};
    }
    return member->kind == FENG_TYPE_MEMBER_FIELD
        ? member->as.field.name
        : member->as.callable.name;
}

static FengSlice call_callee_name_slice(const FengExpr *callee) {
    if (callee == NULL) {
        return (FengSlice){0};
    }
    if (callee->kind == FENG_EXPR_IDENTIFIER) {
        return callee->as.identifier;
    }
    if (callee->kind == FENG_EXPR_MEMBER) {
        return callee->as.member.member;
    }
    return (FengSlice){0};
}

static bool add_reference_if_match(FengLspReferenceList *references,
                                   const FengCliLoadedSource *source,
                                   FengSlice slice,
                                   const FengLspResolvedTarget *expected,
                                   const FengLspResolvedTarget *candidate) {
    if (!resolved_targets_equal(expected, candidate)) {
        return true;
    }
    return reference_list_push_slice(references, source, slice);
}

static bool resolve_expr_reference_target(const FengLspAnalysisSession *session,
                                          const FengProgram *program,
                                          const FengDecl *owner_decl,
                                          const FengTypeMember *owner_member,
                                          const FengExpr *expr,
                                          FengLspResolvedTarget *target) {
    FengLspLocalList locals = {0};
    bool ok;

    memset(target, 0, sizeof(*target));
    if (expr == NULL) {
        return false;
    }
    ok = collect_visible_locals(owner_decl, owner_member, expr->token.offset, &locals);
    if (!ok) {
        local_list_dispose(&locals);
        return false;
    }
    (void)resolve_expr_target(session, program, expr, &locals, target);
    local_list_dispose(&locals);
    return target->kind != FENG_LSP_RESOLVED_NONE;
}

static bool resolve_object_field_target_block(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengBlock *block,
                                              size_t offset,
                                              const FengLspLocalList *locals,
                                              FengLspResolvedTarget *target);

static bool resolve_object_field_target_expr(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengExpr *expr,
                                             size_t offset,
                                             const FengLspLocalList *locals,
                                             FengLspResolvedTarget *target) {
    size_t index;

    if (expr == NULL || offset < expr->token.offset || offset > expr_end(expr)) {
        return false;
    }
    switch (expr->kind) {
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (resolve_object_field_target_expr(session,
                                                     program,
                                                     expr->as.array_literal.items[index],
                                                     offset,
                                                     locals,
                                                     target)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_TUPLE_LITERAL:
            for (index = 0U; index < expr->as.tuple_literal.count; ++index) {
                if (resolve_object_field_target_expr(session,
                                                     program,
                                                     expr->as.tuple_literal.items[index],
                                                     offset,
                                                     locals,
                                                     target)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_GENERIC_TARGET:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.generic_target.target,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_ARRAY_NEW:
            return resolve_object_field_target_expr(session, program,
                                                    expr->as.array_new.size,
                                                    offset, locals, target);
        case FENG_EXPR_OBJECT_LITERAL: {
            FengLspResolvedTarget owner_target = {0};

            if (resolve_object_field_target_expr(session,
                                                 program,
                                                 expr->as.object_literal.target,
                                                 offset,
                                                 locals,
                                                 target)) {
                return true;
            }
            (void)resolve_expr_target(session,
                                      program,
                                      expr->as.object_literal.target,
                                      locals,
                                      &owner_target);
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                if (offset_in_token(expr->as.object_literal.fields[index].token, offset) &&
                    owner_target.kind == FENG_LSP_RESOLVED_DECL) {
                    target->kind = FENG_LSP_RESOLVED_MEMBER;
                    target->decl = owner_target.decl;
                    target->member = find_member_by_name(owner_target.decl,
                                                         expr->as.object_literal.fields[index].name);
                    return target->member != NULL;
                }
                if (resolve_object_field_target_expr(session,
                                                     program,
                                                     expr->as.object_literal.fields[index].value,
                                                     offset,
                                                     locals,
                                                     target)) {
                    return true;
                }
            }
            return false;
        }
        case FENG_EXPR_CALL:
            if (resolve_object_field_target_expr(session,
                                                 program,
                                                 expr->as.call.callee,
                                                 offset,
                                                 locals,
                                                 target)) {
                return true;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                if (resolve_object_field_target_expr(session,
                                                     program,
                                                     expr->as.call.args[index],
                                                     offset,
                                                     locals,
                                                     target)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_MEMBER:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.member.object,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_INDEX:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.index.object,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.index.index,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_UNARY:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.unary.operand,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_BINARY:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.binary.left,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.binary.right,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_LAMBDA:
            if (expr->as.lambda.is_block_body) {
                size_t statement_index;

                for (statement_index = 0U;
                     expr->as.lambda.body_block != NULL &&
                     statement_index < expr->as.lambda.body_block->statement_count;
                     ++statement_index) {
                    const FengStmt *statement = expr->as.lambda.body_block->statements[statement_index];

                    if (statement != NULL && offset >= statement->token.offset && offset <= stmt_end(statement)) {
                        break;
                    }
                }
                return false;
            }
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.lambda.body,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_CAST:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.cast.value,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_IF:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.if_expr.condition,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_MATCH:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    expr->as.match_expr.target,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_EXPR_TRY:
            if (resolve_object_field_target_expr(session,
                                                 program,
                                                 expr->as.try_expr.body,
                                                 offset,
                                                 locals,
                                                 target)) {
                return true;
            }
            for (index = 0U; index < expr->as.try_expr.clause_count; ++index) {
                if (resolve_object_field_target_block(session,
                                                      program,
                                                      expr->as.try_expr.clauses[index].body,
                                                      offset,
                                                      locals,
                                                      target)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_IDENTIFIER:
        case FENG_EXPR_SELF:
        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
            return false;
    }
    return false;
}

static bool resolve_object_field_target_stmt(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengStmt *stmt,
                                             size_t offset,
                                             const FengLspLocalList *locals,
                                             FengLspResolvedTarget *target);

static bool resolve_object_field_target_block(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengBlock *block,
                                              size_t offset,
                                              const FengLspLocalList *locals,
                                              FengLspResolvedTarget *target) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return false;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (resolve_object_field_target_stmt(session,
                                             program,
                                             block->statements[index],
                                             offset,
                                             locals,
                                             target)) {
            return true;
        }
    }
    return false;
}

static bool resolve_object_field_target_stmt(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengStmt *stmt,
                                             size_t offset,
                                             const FengLspLocalList *locals,
                                             FengLspResolvedTarget *target) {
    size_t index;

    if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end(stmt)) {
        return false;
    }
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return resolve_object_field_target_block(session,
                                                     program,
                                                     stmt->as.block,
                                                     offset,
                                                     locals,
                                                     target);
        case FENG_STMT_BINDING:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.binding.initializer,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_ASSIGN:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.assign.target,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.assign.value,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_TRY:
        case FENG_STMT_EXPR:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.expr,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (resolve_object_field_target_expr(session,
                                                     program,
                                                     stmt->as.if_stmt.clauses[index].condition,
                                                     offset,
                                                     locals,
                                                     target) ||
                    resolve_object_field_target_block(session,
                                                      program,
                                                      stmt->as.if_stmt.clauses[index].block,
                                                      offset,
                                                      locals,
                                                      target)) {
                    return true;
                }
            }
            return resolve_object_field_target_block(session,
                                                     program,
                                                     stmt->as.if_stmt.else_block,
                                                     offset,
                                                     locals,
                                                     target);
        case FENG_STMT_MATCH:
            if (resolve_object_field_target_expr(session,
                                                 program,
                                                 stmt->as.match_stmt.target,
                                                 offset,
                                                 locals,
                                                 target)) {
                return true;
            }
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (resolve_object_field_target_block(session,
                                                      program,
                                                      stmt->as.match_stmt.branches[index].body,
                                                      offset,
                                                      locals,
                                                      target)) {
                    return true;
                }
            }
            return resolve_object_field_target_block(session,
                                                     program,
                                                     stmt->as.match_stmt.else_block,
                                                     offset,
                                                     locals,
                                                     target);
        case FENG_STMT_WHILE:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.while_stmt.condition,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_block(session,
                                                    program,
                                                    stmt->as.while_stmt.body,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                return resolve_object_field_target_expr(session,
                                                        program,
                                                        stmt->as.for_stmt.iter_expr,
                                                        offset,
                                                        locals,
                                                        target) ||
                       resolve_object_field_target_block(session,
                                                        program,
                                                        stmt->as.for_stmt.body,
                                                        offset,
                                                        locals,
                                                        target);
            }
            return resolve_object_field_target_stmt(session,
                                                    program,
                                                    stmt->as.for_stmt.init,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.for_stmt.condition,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_stmt(session,
                                                    program,
                                                    stmt->as.for_stmt.update,
                                                    offset,
                                                    locals,
                                                    target) ||
                   resolve_object_field_target_block(session,
                                                    program,
                                                    stmt->as.for_stmt.body,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_RETURN:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.return_value,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_THROW:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    stmt->as.throw_value,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return false;
    }
    return false;
}

static bool resolve_object_field_target_decl(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengDecl *decl,
                                             size_t offset,
                                             const FengLspLocalList *locals,
                                             FengLspResolvedTarget *target) {
    size_t index;

    if (decl == NULL || offset < decl->token.offset || offset > decl_end(decl)) {
        return false;
    }
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return resolve_object_field_target_expr(session,
                                                    program,
                                                    decl->as.binding.initializer,
                                                    offset,
                                                    locals,
                                                    target);
        case FENG_DECL_ENUM:
            return false;
        case FENG_DECL_FUNCTION:
            return resolve_object_field_target_block(session,
                                                     program,
                                                     decl->as.function_decl.body,
                                                     offset,
                                                     locals,
                                                     target);
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];

                if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                    if (resolve_object_field_target_expr(session,
                                                         program,
                                                         member->as.field.initializer,
                                                         offset,
                                                         locals,
                                                         target)) {
                        return true;
                    }
                } else if (resolve_object_field_target_block(session,
                                                             program,
                                                             member->as.callable.body,
                                                             offset,
                                                             locals,
                                                             target)) {
                    return true;
                }
            }
            return false;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    const FengTypeMember *member = decl->as.spec_decl.as.object.members[index];

                    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                        if (resolve_object_field_target_expr(session,
                                                             program,
                                                             member->as.field.initializer,
                                                             offset,
                                                             locals,
                                                             target)) {
                            return true;
                        }
                    } else if (resolve_object_field_target_block(session,
                                                                 program,
                                                                 member->as.callable.body,
                                                                 offset,
                                                                 locals,
                                                                 target)) {
                        return true;
                    }
                }
            }
            return false;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.fit_decl.members[index];

                if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                    if (resolve_object_field_target_expr(session,
                                                         program,
                                                         member->as.field.initializer,
                                                         offset,
                                                         locals,
                                                         target)) {
                        return true;
                    }
                } else if (resolve_object_field_target_block(session,
                                                             program,
                                                             member->as.callable.body,
                                                             offset,
                                                             locals,
                                                             target)) {
                    return true;
                }
            }
            return false;
    }
    return false;
}

static bool collect_references_in_type_ref(const FengLspAnalysisSession *session,
                                           const FengProgram *program,
                                           const FengCliLoadedSource *source,
                                           const FengTypeRef *type_ref,
                                           const FengLspResolvedTarget *target,
                                           FengLspReferenceList *references);

static bool collect_references_in_expr(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengCliLoadedSource *source,
                                       const FengDecl *owner_decl,
                                       const FengTypeMember *owner_member,
                                       const FengExpr *expr,
                                       const FengLspResolvedTarget *target,
                                       FengLspReferenceList *references);

static bool collect_references_in_stmt(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengCliLoadedSource *source,
                                       const FengDecl *owner_decl,
                                       const FengTypeMember *owner_member,
                                       const FengStmt *stmt,
                                       bool include_declaration,
                                       const FengLspResolvedTarget *target,
                                       FengLspReferenceList *references);

static bool collect_references_in_block(const FengLspAnalysisSession *session,
                                        const FengProgram *program,
                                        const FengCliLoadedSource *source,
                                        const FengDecl *owner_decl,
                                        const FengTypeMember *owner_member,
                                        const FengBlock *block,
                                        bool include_declaration,
                                        const FengLspResolvedTarget *target,
                                        FengLspReferenceList *references) {
    size_t index;

    if (block == NULL) {
        return true;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (!collect_references_in_stmt(session,
                                        program,
                                        source,
                                        owner_decl,
                                        owner_member,
                                        block->statements[index],
                                        include_declaration,
                                        target,
                                        references)) {
            return false;
        }
    }
    return true;
}

static bool collect_references_in_expr(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengCliLoadedSource *source,
                                       const FengDecl *owner_decl,
                                       const FengTypeMember *owner_member,
                                       const FengExpr *expr,
                                       const FengLspResolvedTarget *target,
                                       FengLspReferenceList *references) {
    size_t index;

    if (expr == NULL) {
        return true;
    }
    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
        case FENG_EXPR_MEMBER: {
            FengLspResolvedTarget candidate = {0};

            if (resolve_expr_reference_target(session,
                                             program,
                                             owner_decl,
                                             owner_member,
                                             expr,
                                             &candidate)) {
                FengSlice slice = expr->kind == FENG_EXPR_IDENTIFIER
                    ? expr->as.identifier
                    : expr->as.member.member;

                if (!add_reference_if_match(references, source, slice, target, &candidate)) {
                    return false;
                }
            }
            if (expr->kind == FENG_EXPR_MEMBER) {
                return collect_references_in_expr(session,
                                                  program,
                                                  source,
                                                  owner_decl,
                                                  owner_member,
                                                  expr->as.member.object,
                                                  target,
                                                  references);
            }
            return true;
        }
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (!collect_references_in_expr(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                expr->as.array_literal.items[index],
                                                target,
                                                references)) {
                    return false;
                }
            }
            return true;
        case FENG_EXPR_TUPLE_LITERAL:
            for (index = 0U; index < expr->as.tuple_literal.count; ++index) {
                if (!collect_references_in_expr(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                expr->as.tuple_literal.items[index],
                                                target,
                                                references)) {
                    return false;
                }
            }
            return true;
        case FENG_EXPR_GENERIC_TARGET:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.generic_target.target,
                                              target,
                                              references);
        case FENG_EXPR_ARRAY_NEW:
            return collect_references_in_expr(session, program, source, owner_decl,
                                              owner_member, expr->as.array_new.size,
                                              target, references);
        case FENG_EXPR_OBJECT_LITERAL:
        {
            FengLspResolvedTarget owner_target = {0};

            if (!collect_references_in_expr(session,
                                            program,
                                            source,
                                            owner_decl,
                                            owner_member,
                                            expr->as.object_literal.target,
                                            target,
                                            references)) {
                return false;
            }
            (void)resolve_expr_reference_target(session,
                                               program,
                                               owner_decl,
                                               owner_member,
                                               expr->as.object_literal.target,
                                               &owner_target);
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                if (owner_target.kind == FENG_LSP_RESOLVED_DECL) {
                    FengLspResolvedTarget field_target = {
                        .kind = FENG_LSP_RESOLVED_MEMBER,
                        .decl = owner_target.decl,
                        .member = find_member_by_name(owner_target.decl,
                                                     expr->as.object_literal.fields[index].name)
                    };

                    if (field_target.member != NULL &&
                        !add_reference_if_match(references,
                                                source,
                                                expr->as.object_literal.fields[index].name,
                                                target,
                                                &field_target)) {
                        return false;
                    }
                }
                if (!collect_references_in_expr(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                expr->as.object_literal.fields[index].value,
                                                target,
                                                references)) {
                    return false;
                }
            }
            return true;
        }
        case FENG_EXPR_CALL: {
            FengLspResolvedTarget candidate = {0};

            if (resolve_callable_target(&expr->as.call.resolved_callable, &candidate)) {
                if (!add_reference_if_match(references,
                                            source,
                                            call_callee_name_slice(expr->as.call.callee),
                                            target,
                                            &candidate)) {
                    return false;
                }
                if (expr->as.call.callee != NULL && expr->as.call.callee->kind == FENG_EXPR_MEMBER &&
                    !collect_references_in_expr(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                expr->as.call.callee->as.member.object,
                                                target,
                                                references)) {
                    return false;
                }
            } else if (!collect_references_in_expr(session,
                                                   program,
                                                   source,
                                                   owner_decl,
                                                   owner_member,
                                                   expr->as.call.callee,
                                                   target,
                                                   references)) {
                return false;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                if (!collect_references_in_expr(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                expr->as.call.args[index],
                                                target,
                                                references)) {
                    return false;
                }
            }
            return true;
        }
        case FENG_EXPR_INDEX:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.index.object,
                                              target,
                                              references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.index.index,
                                              target,
                                              references);
        case FENG_EXPR_UNARY:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.unary.operand,
                                              target,
                                              references);
        case FENG_EXPR_BINARY:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.binary.left,
                                              target,
                                              references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.binary.right,
                                              target,
                                              references);
        case FENG_EXPR_LAMBDA:
            if (expr->as.lambda.is_block_body) {
                return collect_references_in_block(session,
                                                   program,
                                                   source,
                                                   owner_decl,
                                                   owner_member,
                                                   expr->as.lambda.body_block,
                                                   false,
                                                   target,
                                                   references);
            }
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.lambda.body,
                                              target,
                                              references);
        case FENG_EXPR_CAST:
            return collect_references_in_type_ref(session,
                                                  program,
                                                  source,
                                                  expr->as.cast.type,
                                                  target,
                                                  references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.cast.value,
                                              target,
                                              references);
        case FENG_EXPR_IF:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.if_expr.condition,
                                              target,
                                              references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.if_expr.then_block,
                                              false,
                                              target,
                                              references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.if_expr.else_block,
                                              false,
                                              target,
                                              references);
        case FENG_EXPR_MATCH:
            if (!collect_references_in_expr(session,
                                            program,
                                            source,
                                            owner_decl,
                                            owner_member,
                                            expr->as.match_expr.target,
                                            target,
                                            references)) {
                return false;
            }
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                if (!collect_references_in_block(session,
                                                 program,
                                                 source,
                                                 owner_decl,
                                                 owner_member,
                                                 expr->as.match_expr.branches[index].body,
                                                 false,
                                                 target,
                                                 references)) {
                    return false;
                }
            }
            return collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               expr->as.match_expr.else_block,
                                               false,
                                               target,
                                               references);
        case FENG_EXPR_TRY:
            if (!collect_references_in_expr(session,
                                            program,
                                            source,
                                            owner_decl,
                                            owner_member,
                                            expr->as.try_expr.body,
                                            target,
                                            references)) {
                return false;
            }
            for (index = 0U; index < expr->as.try_expr.clause_count; ++index) {
                const FengTryCatchClause *clause = &expr->as.try_expr.clauses[index];

                if (!collect_references_in_type_ref(session,
                                                     program,
                                                     source,
                                                     clause->type,
                                                     target,
                                                     references) ||
                    !collect_references_in_block(session,
                                                 program,
                                                 source,
                                                 owner_decl,
                                                 owner_member,
                                                 clause->body,
                                                 false,
                                                 target,
                                                 references)) {
                    return false;
                }
            }
            return true;
        case FENG_EXPR_SELF:
        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
            return true;
    }
    return true;
}

static bool collect_references_in_type_ref(const FengLspAnalysisSession *session,
                                           const FengProgram *program,
                                           const FengCliLoadedSource *source,
                                           const FengTypeRef *type_ref,
                                           const FengLspResolvedTarget *target,
                                           FengLspReferenceList *references) {
    if (type_ref == NULL) {
        return true;
    }
    if (type_ref->kind == FENG_TYPE_REF_NAMED) {
        FengLspResolvedTarget candidate = {0};

        candidate.kind = FENG_LSP_RESOLVED_DECL;
        candidate.decl = resolve_named_type_ref(session, program, type_ref);
        if (candidate.decl != NULL &&
            !add_reference_if_match(references,
                                    source,
                                    type_ref->as.named.segments[type_ref->as.named.segment_count - 1U],
                                    target,
                                    &candidate)) {
            return false;
        }
        return true;
    }
    return collect_references_in_type_ref(session,
                                          program,
                                          source,
                                          type_ref->as.inner,
                                          target,
                                          references);
}

static bool collect_param_declarations(const FengCliLoadedSource *source,
                                       const FengParameter *params,
                                       size_t param_count,
                                       bool include_declaration,
                                       const FengLspResolvedTarget *target,
                                       FengLspReferenceList *references) {
    size_t index;

    if (!include_declaration || target == NULL || target->kind != FENG_LSP_RESOLVED_PARAM) {
        return true;
    }
    for (index = 0U; index < param_count; ++index) {
        if (target->parameter == &params[index] &&
            !reference_list_push_slice(references, source, params[index].name)) {
            return false;
        }
    }
    return true;
}

static bool collect_references_in_stmt(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengCliLoadedSource *source,
                                       const FengDecl *owner_decl,
                                       const FengTypeMember *owner_member,
                                       const FengStmt *stmt,
                                       bool include_declaration,
                                       const FengLspResolvedTarget *target,
                                       FengLspReferenceList *references) {
    size_t index;

    if (stmt == NULL) {
        return true;
    }
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            return collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               stmt->as.block,
                                               include_declaration,
                                               target,
                                               references);
        case FENG_STMT_BINDING:
            if (include_declaration && target != NULL &&
                target->kind == FENG_LSP_RESOLVED_BINDING &&
                target->binding == &stmt->as.binding &&
                !reference_list_push_slice(references, source, stmt->as.binding.name)) {
                return false;
            }
            return collect_references_in_type_ref(session,
                                                  program,
                                                  source,
                                                  stmt->as.binding.type,
                                                  target,
                                                  references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.binding.initializer,
                                              target,
                                              references);
        case FENG_STMT_ASSIGN:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.assign.target,
                                              target,
                                              references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.assign.value,
                                              target,
                                              references);
        case FENG_STMT_TRY:
        case FENG_STMT_EXPR:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.expr,
                                              target,
                                              references);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (!collect_references_in_expr(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                stmt->as.if_stmt.clauses[index].condition,
                                                target,
                                                references) ||
                    !collect_references_in_block(session,
                                                 program,
                                                 source,
                                                 owner_decl,
                                                 owner_member,
                                                 stmt->as.if_stmt.clauses[index].block,
                                                 include_declaration,
                                                 target,
                                                 references)) {
                    return false;
                }
            }
            return collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               stmt->as.if_stmt.else_block,
                                               include_declaration,
                                               target,
                                               references);
        case FENG_STMT_MATCH:
            if (!collect_references_in_expr(session,
                                            program,
                                            source,
                                            owner_decl,
                                            owner_member,
                                            stmt->as.match_stmt.target,
                                            target,
                                            references)) {
                return false;
            }
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (!collect_references_in_block(session,
                                                 program,
                                                 source,
                                                 owner_decl,
                                                 owner_member,
                                                 stmt->as.match_stmt.branches[index].body,
                                                 include_declaration,
                                                 target,
                                                 references)) {
                    return false;
                }
            }
            return collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               stmt->as.match_stmt.else_block,
                                               include_declaration,
                                               target,
                                               references);
        case FENG_STMT_WHILE:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.while_stmt.condition,
                                              target,
                                              references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.while_stmt.body,
                                              include_declaration,
                                              target,
                                              references);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                if (include_declaration && target != NULL &&
                    target->kind == FENG_LSP_RESOLVED_BINDING &&
                    target->binding == &stmt->as.for_stmt.iter_binding &&
                    !reference_list_push_slice(references, source, stmt->as.for_stmt.iter_binding.name)) {
                    return false;
                }
                return collect_references_in_type_ref(session,
                                                      program,
                                                      source,
                                                      stmt->as.for_stmt.iter_binding.type,
                                                      target,
                                                      references) &&
                       collect_references_in_expr(session,
                                                  program,
                                                  source,
                                                  owner_decl,
                                                  owner_member,
                                                  stmt->as.for_stmt.iter_expr,
                                                  target,
                                                  references) &&
                       collect_references_in_block(session,
                                                  program,
                                                  source,
                                                  owner_decl,
                                                  owner_member,
                                                  stmt->as.for_stmt.body,
                                                  include_declaration,
                                                  target,
                                                  references);
            }
            return collect_references_in_stmt(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.for_stmt.init,
                                              include_declaration,
                                              target,
                                              references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.for_stmt.condition,
                                              target,
                                              references) &&
                   collect_references_in_stmt(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.for_stmt.update,
                                              include_declaration,
                                              target,
                                              references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.for_stmt.body,
                                              include_declaration,
                                              target,
                                              references);
        case FENG_STMT_RETURN:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.return_value,
                                              target,
                                              references);
        case FENG_STMT_THROW:
            return collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.throw_value,
                                              target,
                                              references);
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return true;
    }
    return true;
}

static bool collect_references_in_member(const FengLspAnalysisSession *session,
                                         const FengProgram *program,
                                         const FengCliLoadedSource *source,
                                         const FengDecl *owner_decl,
                                         const FengTypeMember *member,
                                         bool include_declaration,
                                         const FengLspResolvedTarget *target,
                                         FengLspReferenceList *references) {
    if (member == NULL) {
        return true;
    }
    if (include_declaration && target != NULL &&
        target->kind == FENG_LSP_RESOLVED_MEMBER &&
        target->member == member &&
        !reference_list_push_slice(references, source, member_name_slice(member))) {
        return false;
    }
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return collect_references_in_type_ref(session,
                                              program,
                                              source,
                                              member->as.field.type,
                                              target,
                                              references) &&
               collect_references_in_expr(session,
                                          program,
                                          source,
                                          owner_decl,
                                          member,
                                          member->as.field.initializer,
                                          target,
                                          references);
    }
    return collect_param_declarations(source,
                                      member->as.callable.params,
                                      member->as.callable.param_count,
                                      include_declaration,
                                      target,
                                      references) &&
           collect_references_in_type_ref(session,
                                          program,
                                          source,
                                          member->as.callable.return_type,
                                          target,
                                          references) &&
           collect_references_in_block(session,
                                       program,
                                       source,
                                       owner_decl,
                                       member,
                                       member->as.callable.body,
                                       include_declaration,
                                       target,
                                       references);
}

static bool collect_references_in_decl(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengCliLoadedSource *source,
                                       const FengDecl *decl,
                                       bool include_declaration,
                                       const FengLspResolvedTarget *target,
                                       FengLspReferenceList *references) {
    size_t index;

    if (decl == NULL) {
        return true;
    }
    if (include_declaration && target != NULL &&
        target->kind == FENG_LSP_RESOLVED_DECL &&
        target->decl == decl &&
        decl->kind != FENG_DECL_FIT &&
        !reference_list_push_slice(references, source, decl_name(decl))) {
        return false;
    }
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return collect_references_in_type_ref(session,
                                                  program,
                                                  source,
                                                  decl->as.binding.type,
                                                  target,
                                                  references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              decl,
                                              NULL,
                                              decl->as.binding.initializer,
                                              target,
                                              references);
        case FENG_DECL_ENUM:
            return true;
        case FENG_DECL_FUNCTION:
            return collect_param_declarations(source,
                                              decl->as.function_decl.params,
                                              decl->as.function_decl.param_count,
                                              include_declaration,
                                              target,
                                              references) &&
                   collect_references_in_type_ref(session,
                                                  program,
                                                  source,
                                                  decl->as.function_decl.return_type,
                                                  target,
                                                  references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              decl,
                                              NULL,
                                              decl->as.function_decl.body,
                                              include_declaration,
                                              target,
                                              references);
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                if (!collect_references_in_type_ref(session,
                                                    program,
                                                    source,
                                                    decl->as.type_decl.declared_specs[index],
                                                    target,
                                                    references)) {
                    return false;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (!collect_references_in_member(session,
                                                  program,
                                                  source,
                                                  decl,
                                                  decl->as.type_decl.members[index],
                                                  include_declaration,
                                                  target,
                                                  references)) {
                    return false;
                }
            }
            return true;
        case FENG_DECL_SPEC:
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                if (!collect_references_in_type_ref(session,
                                                    program,
                                                    source,
                                                    decl->as.spec_decl.parent_specs[index],
                                                    target,
                                                    references)) {
                    return false;
                }
            }
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    if (!collect_references_in_member(session,
                                                      program,
                                                      source,
                                                      decl,
                                                      decl->as.spec_decl.as.object.members[index],
                                                      include_declaration,
                                                      target,
                                                      references)) {
                        return false;
                    }
                }
            }
            return true;
        case FENG_DECL_FIT:
            if (!collect_references_in_type_ref(session,
                                                program,
                                                source,
                                                decl->as.fit_decl.target,
                                                target,
                                                references)) {
                return false;
            }
            for (index = 0U; index < decl->as.fit_decl.spec_count; ++index) {
                if (!collect_references_in_type_ref(session,
                                                    program,
                                                    source,
                                                    decl->as.fit_decl.specs[index],
                                                    target,
                                                    references)) {
                    return false;
                }
            }
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                if (!collect_references_in_member(session,
                                                  program,
                                                  source,
                                                  decl,
                                                  decl->as.fit_decl.members[index],
                                                  include_declaration,
                                                  target,
                                                  references)) {
                    return false;
                }
            }
            return true;
    }
    return true;
}

static bool collect_references(const FengLspAnalysisSession *session,
                               bool include_declaration,
                               const FengLspResolvedTarget *target,
                               FengLspReferenceList *references) {
    size_t index;

    if (session == NULL || references == NULL || !resolved_target_supports_references(target)) {
        return false;
    }
    for (index = 0U; index < session->source_count; ++index) {
        const FengCliLoadedSource *source = &session->sources[index];
        const FengProgram *program = source->program;
        size_t decl_index;

        if (program == NULL) {
            continue;
        }
        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            if (!collect_references_in_decl(session,
                                            program,
                                            source,
                                            program->declarations[decl_index],
                                            include_declaration,
                                            target,
                                            references)) {
                return false;
            }
        }
    }
    return true;
}

static const FengCliLoadedSource *find_reference_source(const FengLspAnalysisSession *session,
                                                        const FengLspReferenceEntry *entry) {
    return session != NULL && entry != NULL ? find_source(session, entry->path) : NULL;
}

static char *dup_reference_text(const FengCliLoadedSource *source,
                                const FengLspReferenceEntry *entry) {
    if (source == NULL || entry == NULL ||
        entry->end_offset <= entry->start_offset ||
        entry->end_offset > source->source_length) {
        return NULL;
    }
    return dup_range(source->source + entry->start_offset,
                     source->source + entry->end_offset);
}

static bool append_reference_location(FengLspString *json,
                                      bool *first,
                                      const FengCliLoadedSource *source,
                                      const FengLspReferenceEntry *entry) {
    if (!*first && !string_append_cstr(json, ",")) {
        return false;
    }
    *first = false;
    return location_json_offsets(json,
                                 source->path,
                                 source->source,
                                 entry->start_offset,
                                 entry->end_offset);
}

static bool build_references_json(const FengLspAnalysisSession *session,
                                  const FengLspReferenceList *references,
                                  FengLspString *json) {
    bool first = true;
    size_t index;

    if (session == NULL || references == NULL || json == NULL ||
        !string_append_cstr(json, "[")) {
        return false;
    }
    for (index = 0U; index < references->count; ++index) {
        const FengLspReferenceEntry *entry = &references->items[index];
        const FengCliLoadedSource *source = find_reference_source(session, entry);

        if (source == NULL) {
            continue;
        }
        if (!append_reference_location(json, &first, source, entry)) {
            return false;
        }
    }
    return string_append_cstr(json, "]");
}

static bool build_prepare_rename_json(const FengCliLoadedSource *source,
                                      const FengLspReferenceEntry *entry,
                                      FengLspString *json) {
    char *placeholder;
    bool ok;

    if (source == NULL || entry == NULL) {
        return false;
    }
    placeholder = dup_reference_text(source, entry);
    if (placeholder == NULL) {
        return false;
    }
    ok = string_append_cstr(json, "{\"range\":") &&
         range_json_offsets(json, source->source, entry->start_offset, entry->end_offset) &&
         string_append_cstr(json, ",\"placeholder\":") &&
         string_append_json_string(json, placeholder) &&
         string_append_cstr(json, "}");
    free(placeholder);
    return ok;
}

static bool build_rename_json(const FengLspAnalysisSession *session,
                              const FengLspReferenceList *references,
                              const char *new_name,
                              FengLspString *json) {
    bool first_path = true;
    size_t index;

    if (session == NULL || references == NULL || new_name == NULL || json == NULL ||
        !string_append_cstr(json, "{\"changes\":{")) {
        return false;
    }
    for (index = 0U; index < references->count; ++index) {
        const FengLspReferenceEntry *entry = &references->items[index];
        const FengCliLoadedSource *source;
        char *uri;
        bool first_edit = true;
        size_t edit_index;
        size_t seen_index;

        for (seen_index = 0U; seen_index < index; ++seen_index) {
            if (strcmp(references->items[seen_index].path, entry->path) == 0) {
                break;
            }
        }
        if (seen_index != index) {
            continue;
        }
        source = find_reference_source(session, entry);
        if (source == NULL) {
            continue;
        }
        uri = path_to_file_uri(entry->path);
        if (uri == NULL) {
            return false;
        }
        if (!first_path && !string_append_cstr(json, ",")) {
            free(uri);
            return false;
        }
        first_path = false;
        if (!string_append_json_string(json, uri) || !string_append_cstr(json, ":[")) {
            free(uri);
            return false;
        }
        free(uri);
        for (edit_index = index; edit_index < references->count; ++edit_index) {
            const FengLspReferenceEntry *edit = &references->items[edit_index];

            if (strcmp(edit->path, entry->path) != 0) {
                continue;
            }
            if (!first_edit && !string_append_cstr(json, ",")) {
                return false;
            }
            first_edit = false;
            if (!string_append_cstr(json, "{\"range\":") ||
                !range_json_offsets(json,
                                    source->source,
                                    edit->start_offset,
                                    edit->end_offset) ||
                !string_append_cstr(json, ",\"newText\":") ||
                !string_append_json_string(json, new_name) ||
                !string_append_cstr(json, "}")) {
                return false;
            }
        }
        if (!string_append_cstr(json, "]")) {
            return false;
        }
    }
    return string_append_cstr(json, "}}");
}

static bool find_symbol_decl_token_hit_member(const FengLspCacheQueryContext *context,
                                              const FengDecl *owner_decl,
                                              const FengTypeMember *member,
                                              size_t offset,
                                              FengLspCacheResolvedTarget *target) {
    size_t index;
    bool hit_member_name = offset_in_slice_from_source(context->source_text,
                                                       member_name_slice(member),
                                                       offset);
    bool hit_member_token = offset_in_token(member->token, offset);

    if (hit_member_name ||
        (hit_member_token &&
         !(owner_decl != NULL &&
           owner_decl->kind == FENG_DECL_FIT &&
           member->kind != FENG_TYPE_MEMBER_FIELD))) {
        const FengSymbolDeclView *owner_symbol = match_ast_decl_to_symbol(context->current_module,
                                                                          context->program,
                                                                          owner_decl);
        const FengSymbolDeclView *member_symbol = match_ast_member_to_symbol(owner_symbol,
                                                                             context->program->path,
                                                                             member);

        if (owner_symbol != NULL && member_symbol != NULL) {
            target->kind = FENG_LSP_RESOLVED_MEMBER;
            target->decl = owner_symbol;
            target->member = member_symbol;
            return true;
        }
    }
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return false;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (offset_in_token(member->as.callable.params[index].token, offset) ||
            offset_in_slice_from_source(context->source_text,
                                        member->as.callable.params[index].name,
                                        offset)) {
            target->kind = FENG_LSP_RESOLVED_PARAM;
            target->parameter = &member->as.callable.params[index];
            return true;
        }
    }
    return false;
}

static bool find_symbol_decl_token_hit(const FengLspCacheQueryContext *context,
                                       const FengDecl *decl,
                                       size_t offset,
                                       FengLspCacheResolvedTarget *target) {
    size_t index;
    FengSlice decl_slice;

    decl_slice = decl_name(decl);
    if (offset_in_token(decl->token, offset) ||
        (decl->kind != FENG_DECL_FIT &&
         offset_in_slice_from_source(context->source_text, decl_slice, offset))) {
        const FengSymbolDeclView *symbol_decl = match_ast_decl_to_symbol(context->current_module,
                                                                         context->program,
                                                                         decl);

        if (symbol_decl != NULL) {
            target->kind = FENG_LSP_RESOLVED_DECL;
            target->decl = symbol_decl;
            return true;
        }
    }
    switch (decl->kind) {
        case FENG_DECL_FUNCTION:
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (offset_in_token(decl->as.function_decl.params[index].token, offset) ||
                    offset_in_slice_from_source(context->source_text,
                                                decl->as.function_decl.params[index].name,
                                                offset)) {
                    target->kind = FENG_LSP_RESOLVED_PARAM;
                    target->parameter = &decl->as.function_decl.params[index];
                    return true;
                }
            }
            break;
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (find_symbol_decl_token_hit_member(context,
                                                      decl,
                                                      decl->as.type_decl.members[index],
                                                      offset,
                                                      target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    if (find_symbol_decl_token_hit_member(context,
                                                          decl,
                                                          decl->as.spec_decl.as.object.members[index],
                                                          offset,
                                                          target)) {
                        return true;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                if (find_symbol_decl_token_hit_member(context,
                                                      decl,
                                                      decl->as.fit_decl.members[index],
                                                      offset,
                                                      target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_GLOBAL_BINDING:
            break;
    }
    return false;
}

static bool find_symbol_type_ref_in_member(const FengLspCacheQueryContext *context,
                                           const FengTypeMember *member,
                                           size_t offset,
                                           FengLspCacheResolvedTarget *target) {
    size_t index;

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        if (type_ref_contains_offset(member->as.field.type, offset)) {
            const FengSymbolDeclView *decl = resolve_symbol_named_type_ref(context->provider,
                                                                           context->current_module,
                                                                           context->program,
                                                                           member->as.field.type);
            if (decl != NULL) {
                target->kind = FENG_LSP_RESOLVED_DECL;
                target->decl = decl;
                return true;
            }
        }
        return false;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (type_ref_contains_offset(member->as.callable.params[index].type, offset)) {
            const FengSymbolDeclView *decl = resolve_symbol_named_type_ref(context->provider,
                                                                           context->current_module,
                                                                           context->program,
                                                                           member->as.callable.params[index].type);
            if (decl != NULL) {
                target->kind = FENG_LSP_RESOLVED_DECL;
                target->decl = decl;
                return true;
            }
        }
    }
    if (type_ref_contains_offset(member->as.callable.return_type, offset)) {
        const FengSymbolDeclView *decl = resolve_symbol_named_type_ref(context->provider,
                                                                       context->current_module,
                                                                       context->program,
                                                                       member->as.callable.return_type);
        if (decl != NULL) {
            target->kind = FENG_LSP_RESOLVED_DECL;
            target->decl = decl;
            return true;
        }
    }
    return false;
}

static bool find_symbol_block_type_ref_hit(const FengLspCacheQueryContext *context,
                                           const FengBlock *block,
                                           size_t offset,
                                           FengLspCacheResolvedTarget *target);

static bool find_symbol_stmt_type_ref_hit(const FengLspCacheQueryContext *context,
                                          const FengStmt *stmt,
                                          size_t offset,
                                          FengLspCacheResolvedTarget *target) {
    size_t index;

    if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end(stmt)) {
        return false;
    }
    switch (stmt->kind) {
        case FENG_STMT_BINDING: {
            const FengSymbolDeclView *resolved;

            if (!type_ref_contains_offset(stmt->as.binding.type, offset)) {
                return false;
            }
            resolved = resolve_symbol_named_type_ref(context->provider,
                                                     context->current_module,
                                                     context->program,
                                                     stmt->as.binding.type);
            if (resolved == NULL) {
                return false;
            }
            target->kind = FENG_LSP_RESOLVED_DECL;
            target->decl = resolved;
            return true;
        }
        case FENG_STMT_BLOCK:
            return find_symbol_block_type_ref_hit(context, stmt->as.block, offset, target);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (find_symbol_block_type_ref_hit(context,
                                                   stmt->as.if_stmt.clauses[index].block,
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            return find_symbol_block_type_ref_hit(context,
                                                  stmt->as.if_stmt.else_block,
                                                  offset,
                                                  target);
        case FENG_STMT_MATCH:
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (find_symbol_block_type_ref_hit(context,
                                                   stmt->as.match_stmt.branches[index].body,
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            return find_symbol_block_type_ref_hit(context,
                                                  stmt->as.match_stmt.else_block,
                                                  offset,
                                                  target);
        case FENG_STMT_WHILE:
            return find_symbol_block_type_ref_hit(context,
                                                  stmt->as.while_stmt.body,
                                                  offset,
                                                  target);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                const FengSymbolDeclView *resolved;

                if (type_ref_contains_offset(stmt->as.for_stmt.iter_binding.type, offset)) {
                    resolved = resolve_symbol_named_type_ref(context->provider,
                                                             context->current_module,
                                                             context->program,
                                                             stmt->as.for_stmt.iter_binding.type);
                    if (resolved != NULL) {
                        target->kind = FENG_LSP_RESOLVED_DECL;
                        target->decl = resolved;
                        return true;
                    }
                }
            } else {
                if (find_symbol_stmt_type_ref_hit(context,
                                                  stmt->as.for_stmt.init,
                                                  offset,
                                                  target)) {
                    return true;
                }
                if (find_symbol_stmt_type_ref_hit(context,
                                                  stmt->as.for_stmt.update,
                                                  offset,
                                                  target)) {
                    return true;
                }
            }
            return find_symbol_block_type_ref_hit(context,
                                                  stmt->as.for_stmt.body,
                                                  offset,
                                                  target);
        case FENG_STMT_ASSIGN:
        case FENG_STMT_EXPR:
        case FENG_STMT_TRY:
        case FENG_STMT_RETURN:
        case FENG_STMT_THROW:
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return false;
    }
    return false;
}

static bool find_symbol_block_type_ref_hit(const FengLspCacheQueryContext *context,
                                           const FengBlock *block,
                                           size_t offset,
                                           FengLspCacheResolvedTarget *target) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return false;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (find_symbol_stmt_type_ref_hit(context,
                                          block->statements[index],
                                          offset,
                                          target)) {
            return true;
        }
    }
    return false;
}

static bool find_symbol_type_ref_hit(const FengLspCacheQueryContext *context,
                                     const FengDecl *decl,
                                     size_t offset,
                                     FengLspCacheResolvedTarget *target) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            if (type_ref_contains_offset(decl->as.binding.type, offset)) {
                const FengSymbolDeclView *resolved = resolve_symbol_named_type_ref(context->provider,
                                                                                   context->current_module,
                                                                                   context->program,
                                                                                   decl->as.binding.type);
                if (resolved != NULL) {
                    target->kind = FENG_LSP_RESOLVED_DECL;
                    target->decl = resolved;
                    return true;
                }
            }
            break;
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_FUNCTION:
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (type_ref_contains_offset(decl->as.function_decl.params[index].type, offset)) {
                    const FengSymbolDeclView *resolved = resolve_symbol_named_type_ref(context->provider,
                                                                                       context->current_module,
                                                                                       context->program,
                                                                                       decl->as.function_decl.params[index].type);
                    if (resolved != NULL) {
                        target->kind = FENG_LSP_RESOLVED_DECL;
                        target->decl = resolved;
                        return true;
                    }
                }
            }
            if (type_ref_contains_offset(decl->as.function_decl.return_type, offset)) {
                const FengSymbolDeclView *resolved = resolve_symbol_named_type_ref(context->provider,
                                                                                   context->current_module,
                                                                                   context->program,
                                                                                   decl->as.function_decl.return_type);
                if (resolved != NULL) {
                    target->kind = FENG_LSP_RESOLVED_DECL;
                    target->decl = resolved;
                    return true;
                }
            }
            if (find_symbol_block_type_ref_hit(context,
                                               decl->as.function_decl.body,
                                               offset,
                                               target)) {
                return true;
            }
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (find_symbol_type_ref_in_member(context,
                                                   decl->as.type_decl.members[index],
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    if (find_symbol_type_ref_in_member(context,
                                                       decl->as.spec_decl.as.object.members[index],
                                                       offset,
                                                       target)) {
                        return true;
                    }
                }
            }
            break;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                if (find_symbol_type_ref_in_member(context,
                                                   decl->as.fit_decl.members[index],
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            break;
    }
    return false;
}

static const FengSymbolDeclView *resolve_symbol_expr_target(const FengLspCacheQueryContext *context,
                                                            const FengExpr *expr,
                                                            const FengLspLocalList *locals,
                                                            FengLspCacheResolvedTarget *target) {
    memset(target, 0, sizeof(*target));
    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_SELF) {
        const FengLspLocal *self_local = find_local(locals, slice_from_cstr("self"));

        target->kind = FENG_LSP_RESOLVED_SELF;
        target->self_owner_decl = self_local != NULL
                                      ? match_ast_decl_to_symbol(context->current_module,
                                                                 context->program,
                                                                 self_local->self_owner_decl)
                                      : NULL;
        return target->self_owner_decl;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        const FengLspLocal *local = find_local(locals, expr->as.identifier);

        if (local != NULL) {
            if (local->kind == FENG_LSP_LOCAL_PARAM) {
                target->kind = FENG_LSP_RESOLVED_PARAM;
                target->parameter = local->parameter;
                return NULL;
            }
            if (local->kind == FENG_LSP_LOCAL_BINDING) {
                target->kind = FENG_LSP_RESOLVED_BINDING;
                target->binding = local->binding;
                return NULL;
            }
            target->kind = FENG_LSP_RESOLVED_SELF;
            target->self_owner_decl = match_ast_decl_to_symbol(context->current_module,
                                                               context->program,
                                                               local->self_owner_decl);
            return target->self_owner_decl;
        }
        target->decl = resolve_symbol_value_name(context->provider,
                                                 context->current_module,
                                                 context->program,
                                                 expr->as.identifier);
        if (target->decl == NULL) {
            target->decl = resolve_symbol_type_name(context->provider,
                                                    context->current_module,
                                                    context->program,
                                                    expr->as.identifier);
        }
        if (target->decl != NULL) {
            target->kind = FENG_LSP_RESOLVED_DECL;
            return target->decl;
        }
        return NULL;
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL) {
        if (expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
            const FengLspLocal *local = find_local(locals, expr->as.member.object->as.identifier);

            if (local == NULL &&
                (context->current_module == NULL ||
                 find_symbol_module_decl_by_name(context->current_module,
                                                 expr->as.member.object->as.identifier,
                                                 false,
                                                 false,
                                                 false) == NULL)) {
                const FengSymbolImportedModule *alias_module = find_symbol_alias_module(context->provider,
                                                                                        context->program,
                                                                                        expr->as.member.object->as.identifier);
                if (alias_module != NULL) {
                    target->decl = find_symbol_module_decl_by_name(alias_module,
                                                                   expr->as.member.member,
                                                                   false,
                                                                   false,
                                                                   true);
                    if (target->decl != NULL) {
                        target->kind = FENG_LSP_RESOLVED_DECL;
                        return target->decl;
                    }
                }
            }
        }
        target->decl = resolve_symbol_owner_decl_from_object_expr(context,
                                                                  expr->as.member.object,
                                                                  locals);
        if (target->decl != NULL) {
            target->member = find_symbol_decl_member_by_name(target->decl,
                                                             expr->as.member.member,
                                                             false);
            if (target->member != NULL) {
                target->kind = FENG_LSP_RESOLVED_MEMBER;
                return target->decl;
            }
        }
    }
    return NULL;
}

static bool resolve_symbol_target_at(const FengLspCacheQueryContext *context,
                                     size_t offset,
                                     FengLspCacheResolvedTarget *target) {
    size_t decl_index;
    const FengDecl *enclosing_decl;
    const FengTypeMember *enclosing_member;
    FengLspLocalList locals = {0};
    const FengExpr *expr;

    memset(target, 0, sizeof(*target));
    enclosing_decl = find_enclosing_decl(context->program, offset, &enclosing_member);
    if (enclosing_decl == NULL) {
        return false;
    }
    if (!collect_visible_locals(enclosing_decl, enclosing_member, offset, &locals)) {
        local_list_dispose(&locals);
        return false;
    }
    for (decl_index = 0U; decl_index < context->program->declaration_count; ++decl_index) {
        if (find_symbol_type_ref_hit(context,
                                     context->program->declarations[decl_index],
                                     offset,
                                     target)) {
            local_list_dispose(&locals);
            return true;
        }
        if (find_symbol_decl_token_hit(context,
                                       context->program->declarations[decl_index],
                                       offset,
                                       target)) {
            local_list_dispose(&locals);
            return true;
        }
    }
    expr = find_expr_hit_in_decl(enclosing_decl, offset);
    if (expr != NULL) {
        (void)resolve_symbol_expr_target(context, expr, &locals, target);
    }
    local_list_dispose(&locals);
    return target->kind != FENG_LSP_RESOLVED_NONE;
}

static bool handle_hover_request(FengLspRuntime *runtime,
                                 FILE *output,
                                 FengLspJsonValue id,
                                 FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    char *uri;
    unsigned int line;
    unsigned int character;
    FengLspDocument *document;
    FengLspAnalysisSession session = {0};
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspCacheResolvedTarget cache_target = {0};
    size_t offset;
    char *hover_text;
    FengLspString result = {0};
    bool ok;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    if (uri == NULL || !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = offset_from_position(document->text, line, character);
    /* Prefer analysis path: reads live AST for up-to-date doc comments and
       signatures, works even when exit_code != 0 (best-effort). */
    if (build_analysis_session(runtime, document, &session)) {
        program = find_program(&session, document->path);
        if (program != NULL && resolve_target_at(&session, program, offset, &target)) {
            hover_text = hover_text_for_target(&session, program, &target);
                        ok = build_hover_result_json(&result,
                                                                                 runtime->hover_markup_kind,
                                                                                 hover_text);
            free(hover_text);
            free(uri);
            session_dispose(&session);
            if (!ok) {
                if (runtime->errors != NULL) {
                    fprintf(runtime->errors, "lsp: textDocument/hover: out of memory building response\n");
                }
                string_dispose(&result);
                return send_json_response(output, id, "null");
            }
            ok = send_json_response(output, id, result.data);
            string_dispose(&result);
            return ok;
        }
    }
    session_dispose(&session);
    /* Fallback to symbol cache (e.g., symbols from dependency packages or when
       analysis cannot resolve — uses pre-built .ft symbol tables). */
    if (build_cache_query_context(document, &cache) && resolve_symbol_target_at(&cache, offset, &cache_target)) {
        hover_text = hover_text_for_cache_target(&cache_target);
          ok = build_hover_result_json(&result,
                                                 runtime->hover_markup_kind,
                                                 hover_text);
        free(hover_text);
        cache_query_context_dispose(&cache);
        free(uri);
        if (!ok) {
            if (runtime->errors != NULL) {
                fprintf(runtime->errors, "lsp: textDocument/hover: out of memory building cache response\n");
            }
            string_dispose(&result);
            return send_json_response(output, id, "null");
        }
        ok = send_json_response(output, id, result.data);
        string_dispose(&result);
        return ok;
    }
    cache_query_context_dispose(&cache);
    free(uri);
    return send_json_response(output, id, "null");
}

static bool handle_definition_request(FengLspRuntime *runtime,
                                      FILE *output,
                                      FengLspJsonValue id,
                                      FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    char *uri;
    unsigned int line;
    unsigned int character;
    FengLspDocument *document;
    FengLspAnalysisSession session = {0};
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspCacheResolvedTarget cache_target = {0};
    const FengProgram *target_program = NULL;
    FengLspString result = {0};
    bool ok;
    size_t offset;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    if (uri == NULL || !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = offset_from_position(document->text, line, character);
    /* Prefer analysis path: uses live AST for accurate token positions and
       works even when exit_code != 0 (best-effort for files with errors). */
    if (build_analysis_session(runtime, document, &session)) {
        program = find_program(&session, document->path);
        if (program != NULL && resolve_target_at(&session, program, offset, &target)) {
            switch (target.kind) {
                case FENG_LSP_RESOLVED_DECL:
                    (void)find_decl_module(&session, target.decl, &target_program);
                    ok = location_json(&result,
                                       target_program != NULL ? target_program->path : NULL,
                                       target.decl->token);
                    break;
                case FENG_LSP_RESOLVED_MEMBER:
                    (void)find_decl_module(&session, target.decl, &target_program);
                    ok = location_json(&result,
                                       target_program != NULL ? target_program->path : NULL,
                                       target.member->token);
                    break;
                case FENG_LSP_RESOLVED_PARAM:
                    ok = location_json(&result, program->path, target.parameter->token);
                    break;
                case FENG_LSP_RESOLVED_BINDING:
                    ok = location_json(&result, program->path, target.binding->token);
                    break;
                case FENG_LSP_RESOLVED_SELF:
                    (void)find_decl_module(&session, target.self_owner_decl, &target_program);
                    ok = location_json(&result,
                                       target_program != NULL ? target_program->path : NULL,
                                       target.self_owner_decl->token);
                    break;
                default:
                    ok = string_append_cstr(&result, "null");
                    break;
            }
            free(uri);
            session_dispose(&session);
            if (!ok) {
                if (runtime->errors != NULL) {
                    fprintf(runtime->errors, "lsp: textDocument/definition: out of memory building response\n");
                }
                string_dispose(&result);
                return send_json_response(output, id, "null");
            }
            ok = send_json_response(output, id, result.data);
            string_dispose(&result);
            return ok;
        }
    }
    session_dispose(&session);
    /* Fallback to symbol cache (e.g., symbols from dependency packages or when
       analysis cannot resolve — uses pre-built .ft symbol tables). */
    if (build_cache_query_context(document, &cache) && resolve_symbol_target_at(&cache, offset, &cache_target)) {
        switch (cache_target.kind) {
            case FENG_LSP_RESOLVED_DECL: {
                FengSlice path = feng_symbol_decl_path(cache_target.decl);
                ok = location_json(&result, path.data, feng_symbol_decl_token(cache_target.decl));
                break;
            }
            case FENG_LSP_RESOLVED_MEMBER: {
                FengSlice path = feng_symbol_decl_path(cache_target.member);
                ok = location_json(&result, path.data, feng_symbol_decl_token(cache_target.member));
                break;
            }
            case FENG_LSP_RESOLVED_PARAM:
                ok = location_json(&result, cache.program->path, cache_target.parameter->token);
                break;
            case FENG_LSP_RESOLVED_BINDING:
                ok = location_json(&result, cache.program->path, cache_target.binding->token);
                break;
            case FENG_LSP_RESOLVED_SELF: {
                FengSlice path = feng_symbol_decl_path(cache_target.self_owner_decl);
                ok = location_json(&result, path.data, feng_symbol_decl_token(cache_target.self_owner_decl));
                break;
            }
            default:
                ok = string_append_cstr(&result, "null");
                break;
        }
        cache_query_context_dispose(&cache);
        free(uri);
        if (!ok) {
            if (runtime->errors != NULL) {
                fprintf(runtime->errors, "lsp: textDocument/definition: out of memory building cache response\n");
            }
            string_dispose(&result);
            return send_json_response(output, id, "null");
        }
        ok = send_json_response(output, id, result.data);
        string_dispose(&result);
        return ok;
    }
    cache_query_context_dispose(&cache);
    free(uri);
    return send_json_response(output, id, "null");
}

static bool member_passes_filter(const FengTypeMember *member, FengLspMemberFilter filter) {
    if (member->kind == FENG_TYPE_MEMBER_FINALIZER) {
        return false;
    }
    if (member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR) {
        return false;
    }
    switch (filter) {
        case FENG_LSP_MEMBER_FILTER_STATIC:
            return member->is_static;
        case FENG_LSP_MEMBER_FILTER_INSTANCE:
            return !member->is_static;
        case FENG_LSP_MEMBER_FILTER_ALL:
            return true;
    }
    return true;
}

static bool symbol_member_passes_filter(const FengSymbolDeclView *member, FengLspMemberFilter filter) {
    FengSymbolDeclKind kind = feng_symbol_decl_kind(member);

    if (kind == FENG_SYMBOL_DECL_KIND_FINALIZER || kind == FENG_SYMBOL_DECL_KIND_CONSTRUCTOR) {
        return false;
    }
    switch (filter) {
        case FENG_LSP_MEMBER_FILTER_STATIC:
            return feng_symbol_decl_is_static(member);
        case FENG_LSP_MEMBER_FILTER_INSTANCE:
            return !feng_symbol_decl_is_static(member);
        case FENG_LSP_MEMBER_FILTER_ALL:
            return true;
    }
    return true;
}

typedef struct FengLspCompletionContext {
    bool is_member;
    bool is_static_access;
    FengSlice object;
    FengSlice prefix;
    FengSlice literal_builtin_name;
} FengLspCompletionContext;

static bool completion_identifier_start(char ch) {
    return ch == '_' || isalpha((unsigned char)ch);
}

static bool completion_identifier_continue(char ch) {
    return ch == '_' || isalnum((unsigned char)ch);
}

/* Map a single-segment identifier to its canonical builtin type name, or
 * return NULL when the identifier is not a known builtin type. */
static const char *builtin_name_for_type_identifier(FengSlice name) {
    if (slice_equals_cstr(name, "int") || slice_equals_cstr(name, "i32")) {
        return "i32";
    }
    if (slice_equals_cstr(name, "long") || slice_equals_cstr(name, "i64")) {
        return "i64";
    }
    if (slice_equals_cstr(name, "byte") || slice_equals_cstr(name, "u8")) {
        return "u8";
    }
    if (slice_equals_cstr(name, "float") || slice_equals_cstr(name, "f32")) {
        return "f32";
    }
    if (slice_equals_cstr(name, "double") || slice_equals_cstr(name, "f64")) {
        return "f64";
    }
    if (slice_equals_cstr(name, "i8")) {
        return "i8";
    }
    if (slice_equals_cstr(name, "i16")) {
        return "i16";
    }
    if (slice_equals_cstr(name, "u16")) {
        return "u16";
    }
    if (slice_equals_cstr(name, "u32")) {
        return "u32";
    }
    if (slice_equals_cstr(name, "u64")) {
        return "u64";
    }
    if (slice_equals_cstr(name, "bool")) {
        return "bool";
    }
    if (slice_equals_cstr(name, "string")) {
        return "string";
    }
    return NULL;
}

static bool completion_context_from_text(const char *text,
                                         size_t offset,
                                         FengLspCompletionContext *context) {
    size_t length;
    size_t prefix_start;
    size_t object_start;
    size_t object_end;

    if (context == NULL) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    if (text == NULL) {
        return false;
    }
    length = strlen(text);
    if (offset > length) {
        return false;
    }
    prefix_start = offset;
    while (prefix_start > 0U && completion_identifier_continue(text[prefix_start - 1U])) {
        --prefix_start;
    }
    if (prefix_start == 0U || text[prefix_start - 1U] != '.') {
        return true;
    }
    object_end = prefix_start - 1U;
    object_start = object_end;
    while (object_start > 0U && completion_identifier_continue(text[object_start - 1U])) {
        --object_start;
    }
    if (object_start == object_end) {
        /* No identifier chars before the dot.  Check for a closing string
         * quote immediately before the dot: `"hello".` */
        if (object_end > 0U && text[object_end - 1U] == '"') {
            context->is_member = true;
            context->literal_builtin_name = slice_from_cstr("string");
            context->prefix.data = text + prefix_start;
            context->prefix.length = offset - prefix_start;
        }
        return true;
    }
    if (!completion_identifier_start(text[object_start])) {
        /* Object starts with a non-identifier character.  If it starts
         * with a digit the token is a numeric literal. */
        if (isdigit((unsigned char)text[object_start])) {
            context->is_member = true;
            context->literal_builtin_name = slice_from_cstr("i32");
            context->object.data = text + object_start;
            context->object.length = object_end - object_start;
            context->prefix.data = text + prefix_start;
            context->prefix.length = offset - prefix_start;
        }
        return true;
    }
    context->is_member = true;
    context->object.data = text + object_start;
    context->object.length = object_end - object_start;
    context->prefix.data = text + prefix_start;
    context->prefix.length = offset - prefix_start;
    /* Detect boolean literal keywords `true` and `false`. */
    if (slice_equals_cstr(context->object, "true") ||
        slice_equals_cstr(context->object, "false")) {
        context->literal_builtin_name = slice_from_cstr("bool");
    }
    /* Detect builtin type names for static member access (e.g. i32.parse). */
    {
        const char *type_builtin = builtin_name_for_type_identifier(context->object);

        if (type_builtin != NULL) {
            context->literal_builtin_name = slice_from_cstr(type_builtin);
            context->is_static_access = true;
        }
    }
    return true;
}

static int completion_kind_for_decl(const FengDecl *decl) {
    return decl != NULL && decl->kind == FENG_DECL_FUNCTION ? 3 : 6;
}

static bool append_completion_item(FengLspString *json,
                                   bool *first,
                                   FengSlice label,
                                   const char *detail,
                                   int kind) {
    char *label_text;
    bool ok;

    if (!*first && !string_append_cstr(json, ",")) {
        return false;
    }
    *first = false;
    label_text = dup_range(label.data, label.data + label.length);
    if (label_text == NULL) {
        return false;
    }
    ok = string_append_cstr(json, "{\"label\":") &&
         string_append_json_string(json, label_text) &&
         string_append_format(json, ",\"kind\":%d", kind);
    free(label_text);
    if (!ok) {
        return false;
    }
    if (detail != NULL) {
        if (!string_append_cstr(json, ",\"detail\":") || !string_append_json_string(json, detail)) {
            return false;
        }
    }
    return string_append_cstr(json, "}");
}

/* Append a completion item with a resolve data payload.  The data object
 * carries the document URI and an optional owner type name so that
 * completionItem/resolve can locate the doc comment without re-analysing
 * the whole project. */
static bool append_completion_item_with_data(FengLspString *json,
                                             bool *first,
                                             FengSlice label,
                                             const char *detail,
                                             int kind,
                                             const char *uri,
                                             const char *owner_name) {
    char *label_text;
    bool ok;

    if (!*first && !string_append_cstr(json, ",")) {
        return false;
    }
    *first = false;
    label_text = dup_range(label.data, label.data + label.length);
    if (label_text == NULL) {
        return false;
    }
    ok = string_append_cstr(json, "{\"label\":") &&
         string_append_json_string(json, label_text) &&
         string_append_format(json, ",\"kind\":%d", kind);
    if (!ok) {
        free(label_text);
        return false;
    }
    if (detail != NULL) {
        if (!string_append_cstr(json, ",\"detail\":") || !string_append_json_string(json, detail)) {
            free(label_text);
            return false;
        }
    }
    if (uri != NULL) {
        ok = string_append_cstr(json, ",\"data\":{\"uri\":") &&
             string_append_json_string(json, uri) &&
             string_append_cstr(json, ",\"label\":") &&
             string_append_json_string(json, label_text);
        if (!ok) {
            free(label_text);
            return false;
        }
        if (owner_name != NULL) {
            if (!string_append_cstr(json, ",\"owner\":") ||
                !string_append_json_string(json, owner_name)) {
                free(label_text);
                return false;
            }
        }
        if (!string_append_cstr(json, "}")) {
            free(label_text);
            return false;
        }
    }
    free(label_text);
    return string_append_cstr(json, "}");
}

static bool completion_json_contains_label(const FengLspString *json,
                                           FengSlice label,
                                           bool *contains) {
    FengLspString needle = {0};
    char *label_text;
    bool ok;

    *contains = false;
    if (json == NULL || json->data == NULL) {
        return true;
    }
    label_text = dup_range(label.data, label.data + label.length);
    if (label_text == NULL) {
        return false;
    }
        ok = string_append_cstr(&needle, "{\"label\":") &&
            string_append_json_string(&needle, label_text);
    free(label_text);
    if (!ok) {
        string_dispose(&needle);
        return false;
    }
    *contains = strstr(json->data, needle.data) != NULL;
    string_dispose(&needle);
    return true;
}

static bool append_decl_completion_item(FengLspString *json,
                                        bool *first,
                                        const FengDecl *decl,
                                        const char *detail,
                                        int forced_kind) {
    FengLspString signature = {0};
    const char *item_detail = detail;
    bool ok;

    if (decl == NULL || decl->kind == FENG_DECL_FIT) {
        return true;
    }
    if (item_detail == NULL && decl_signature_to_string(&signature, decl)) {
        item_detail = signature.data;
    }
    ok = append_completion_item_with_data(json,
                                          first,
                                          decl_name(decl),
                                          item_detail,
                                          forced_kind > 0 ? forced_kind : completion_kind_for_decl(decl),
                                          g_completion_uri,
                                          NULL);
    string_dispose(&signature);
    return ok;
}

static bool append_member_completion_item(FengLspString *json,
                                          bool *first,
                                          const FengTypeMember *member,
                                          const char *owner_name) {
    FengLspString signature = {0};
    FengSlice name;
    int kind;
    bool ok;

    if (member == NULL) {
        return true;
    }
    name = member->kind == FENG_TYPE_MEMBER_FIELD ? member->as.field.name : member->as.callable.name;
    kind = member->kind == FENG_TYPE_MEMBER_FIELD ? 5 : 2;
    ok = member_signature_to_string(&signature, member) &&
         append_completion_item_with_data(json, first, name, signature.data, kind,
                                          g_completion_uri, owner_name);
    string_dispose(&signature);
    return ok;
}

static const FengProgram *find_decl_owner_program_in_session(const FengLspAnalysisSession *session,
                                                             const FengDecl *decl) {
    size_t source_index;
    size_t decl_index;

    if (session == NULL || decl == NULL) {
        return NULL;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        const FengProgram *program = session->sources[source_index].program;

        if (program == NULL) {
            continue;
        }
        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            if (program->declarations[decl_index] == decl) {
                return program;
            }
        }
    }
    return NULL;
}

static bool decl_private_visible_from_program(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengDecl *decl) {
    const FengProgram *owner_program;

    if (program == NULL || decl == NULL) {
        return false;
    }
    owner_program = find_decl_owner_program_in_session(session, decl);
    if (owner_program == NULL) {
        return false;
    }
    return program_module_matches(owner_program,
                                  program->module_segments,
                                  program->module_segment_count);
}

static bool type_member_visible_from_program(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengDecl *owner_decl,
                                             const FengTypeMember *member) {
    if (member == NULL) {
        return false;
    }
    if (member->visibility == FENG_VISIBILITY_PUBLIC) {
        return true;
    }
    return decl_private_visible_from_program(session, program, owner_decl);
}

static bool symbol_decl_is_in_module(const FengSymbolImportedModule *module,
                                     const FengSymbolDeclView *decl) {
    size_t index;
    size_t count;

    if (module == NULL || decl == NULL) {
        return false;
    }
    count = feng_symbol_module_decl_count(module);
    for (index = 0U; index < count; ++index) {
        if (feng_symbol_module_decl_at(module, index) == decl) {
            return true;
        }
    }
    return false;
}

static bool symbol_member_visible_from_module(const FengSymbolImportedModule *current_module,
                                              const FengSymbolDeclView *owner_decl,
                                              const FengSymbolDeclView *member) {
    if (member == NULL) {
        return false;
    }
    if (feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PUBLIC) {
        return true;
    }
    return symbol_decl_is_in_module(current_module, owner_decl);
}

static bool append_symbol_decl_completion_item(FengLspString *json,
                                               bool *first,
                                               const FengSymbolDeclView *decl,
                                               const char *detail,
                                               int forced_kind) {
    FengLspString signature = {0};
    const char *item_detail = detail;
    int kind;
    bool ok;

    if (!symbol_decl_is_completion_decl(decl)) {
        return true;
    }
    kind = feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FUNCTION ? 3 : 6;
    if (item_detail == NULL && symbol_decl_signature_to_string(&signature, decl)) {
        item_detail = signature.data;
    }
    ok = append_completion_item_with_data(json,
                                          first,
                                          feng_symbol_decl_name(decl),
                                          item_detail,
                                          forced_kind > 0 ? forced_kind : kind,
                                          g_completion_uri,
                                          NULL);
    string_dispose(&signature);
    return ok;
}

static bool append_symbol_member_completion_item(FengLspString *json,
                                                 bool *first,
                                                 const FengSymbolDeclView *member,
                                                 const char *owner_name) {
    FengLspString signature = {0};
    int kind;
    bool ok;

    if (!symbol_decl_is_instance_member(member)) {
        return true;
    }
    kind = feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD
               ? 5
               : feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_ENUM_ITEM ? 20 : 2;
    ok = symbol_member_signature_to_string(&signature, member) &&
         append_completion_item_with_data(json, first, feng_symbol_decl_name(member),
                                          signature.data, kind,
                                          g_completion_uri, owner_name);
    string_dispose(&signature);
    return ok;
}

static bool append_enum_item_completion_item(FengLspString *json,
                                             bool *first,
                                             const FengEnumItem *item) {
    FengLspString detail = {0};
    bool ok;

    if (item == NULL) {
        return true;
    }
    if (!string_append_bytes(&detail, item->name.data, item->name.length)) {
        string_dispose(&detail);
        return false;
    }
    if (item->has_explicit_value &&
        !string_append_format(&detail, " = %lld", (long long)item->explicit_value)) {
        string_dispose(&detail);
        return false;
    }
    ok = append_completion_item(json, first, item->name, detail.data, 20);
    string_dispose(&detail);
    return ok;
}

static bool append_program_decl_completion_items(FengLspString *json,
                                                 bool *first,
                                                 const FengProgram *program,
                                                 bool public_only,
                                                 const char *detail,
                                                 int forced_kind) {
    size_t decl_index;

    if (program == NULL) {
        return true;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];

        if (decl->kind == FENG_DECL_FIT || (public_only && decl->visibility != FENG_VISIBILITY_PUBLIC)) {
            continue;
        }
        if (!append_decl_completion_item(json, first, decl, detail, forced_kind)) {
            return false;
        }
    }
    return true;
}

static bool append_semantic_module_completion_items(FengLspString *json,
                                                    bool *first,
                                                    const FengSemanticModule *module,
                                                    bool public_only,
                                                    const char *detail,
                                                    int forced_kind) {
    size_t program_index;

    if (module == NULL) {
        return true;
    }
    for (program_index = 0U; program_index < module->program_count; ++program_index) {
        if (!append_program_decl_completion_items(json,
                                                  first,
                                                  module->programs[program_index],
                                                  public_only,
                                                  detail,
                                                  forced_kind)) {
            return false;
        }
    }
    return true;
}

static bool append_loaded_module_completion_items(FengLspString *json,
                                                  bool *first,
                                                  const FengLspAnalysisSession *session,
                                                  const FengSlice *segments,
                                                  size_t segment_count,
                                                  bool public_only,
                                                  const char *detail,
                                                  int forced_kind) {
    size_t source_index;

    if (session == NULL || segments == NULL || segment_count == 0U) {
        return true;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        const FengProgram *loaded_program = session->sources[source_index].program;

        if (program_module_matches(loaded_program, segments, segment_count) &&
            !append_program_decl_completion_items(json,
                                                  first,
                                                  loaded_program,
                                                  public_only,
                                                  detail,
                                                  forced_kind)) {
            return false;
        }
    }
    return true;
}

static bool append_owner_member_completion_items(FengLspString *json,
                                                 bool *first,
                                                 const FengLspAnalysisSession *session,
                                                 const FengProgram *program,
                                                 const FengDecl *owner_decl,
                                                 FengLspMemberFilter filter) {
    size_t index;

    if (owner_decl == NULL) {
        return true;
    }
    {
        FengSlice owner_slice = decl_name(owner_decl);
        char *owner_str = dup_range(owner_slice.data, owner_slice.data + owner_slice.length);

        if (owner_decl->kind == FENG_DECL_TYPE) {
            for (index = 0U; index < owner_decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = owner_decl->as.type_decl.members[index];
                FengSlice member_name;
                bool contains = false;

                if (!type_member_visible_from_program(session, program, owner_decl, member)) {
                    continue;
                }
                if (!member_passes_filter(member, filter)) {
                    continue;
                }
                member_name = member->kind == FENG_TYPE_MEMBER_FIELD
                                  ? member->as.field.name : member->as.callable.name;
                if (!completion_json_contains_label(json, member_name, &contains)) {
                    free(owner_str);
                    return false;
                }
                if (contains) {
                    continue;
                }
                if (!append_member_completion_item(json, first, member, owner_str)) {
                    free(owner_str);
                    return false;
                }
            }
        }
        if (owner_decl->kind == FENG_DECL_SPEC && owner_decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
            for (index = 0U; index < owner_decl->as.spec_decl.as.object.member_count; ++index) {
                const FengTypeMember *member = owner_decl->as.spec_decl.as.object.members[index];
                FengSlice member_name;
                bool contains = false;

                if (!type_member_visible_from_program(session, program, owner_decl, member)) {
                    continue;
                }
                if (!member_passes_filter(member, filter)) {
                    continue;
                }
                member_name = member->kind == FENG_TYPE_MEMBER_FIELD
                                  ? member->as.field.name : member->as.callable.name;
                if (!completion_json_contains_label(json, member_name, &contains)) {
                    free(owner_str);
                    return false;
                }
                if (contains) {
                    continue;
                }
                if (!append_member_completion_item(json, first, member, owner_str)) {
                    free(owner_str);
                    return false;
                }
            }
        }
        free(owner_str);
    }
    if (owner_decl->kind == FENG_DECL_ENUM) {
        for (index = 0U; index < owner_decl->as.enum_decl.item_count; ++index) {
            if (!append_enum_item_completion_item(json, first, &owner_decl->as.enum_decl.items[index])) {
                return false;
            }
        }
    }
    return true;
}

static bool session_contains_module_program(const FengLspAnalysisSession *session,
                                            const FengSlice *segments,
                                            size_t segment_count);

static bool append_project_module_completion_items(FengLspString *json,
                                                   bool *first,
                                                   const char *program_path,
                                                   const FengSlice *segments,
                                                   size_t segment_count,
                                                   bool public_only,
                                                   const char *detail,
                                                   int forced_kind) {
    char *manifest_path = NULL;
    FengCliProjectError error = {0};
    FengCliProjectContext context = {0};
    char *program_resolved = NULL;
    bool ok = true;
    size_t source_index;

    if (json == NULL || first == NULL || program_path == NULL || segments == NULL ||
        segment_count == 0U || !file_exists(program_path)) {
        return true;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(program_path, &manifest_path, &error)) {
        feng_cli_project_error_dispose(&error);
        return true;
    }
    feng_cli_project_error_dispose(&error);
    if (!feng_cli_project_open(manifest_path, &context, &error)) {
        feng_cli_project_error_dispose(&error);
        free(manifest_path);
        return true;
    }
    feng_cli_project_error_dispose(&error);
    free(manifest_path);
    program_resolved = realpath(program_path, NULL);

    for (source_index = 0U; source_index < context.source_count && ok; ++source_index) {
        const char *src_path = context.source_paths[source_index];
        char *source = NULL;
        size_t source_length = 0U;
        FengProgram *scanned_program = NULL;
        FengParseError parse_error = {0};

        if ((program_resolved != NULL && strcmp(src_path, program_resolved) == 0) ||
            (program_resolved == NULL && strcmp(src_path, program_path) == 0)) {
            continue;
        }
        source = feng_cli_read_entire_file(src_path, &source_length);
        if (source == NULL) {
            continue;
        }
        if (feng_parse_source(source, source_length, src_path, &scanned_program, &parse_error) &&
            program_module_matches(scanned_program, segments, segment_count)) {
            ok = append_program_decl_completion_items(json,
                                                      first,
                                                      scanned_program,
                                                      public_only,
                                                      detail,
                                                      forced_kind);
        }
        feng_program_free(scanned_program);
        free(source);
    }

    free(program_resolved);
    feng_cli_project_context_dispose(&context);
    return ok;
}

static bool append_alias_module_completion_items(FengLspString *json,
                                                 bool *first,
                                                 const FengLspAnalysisSession *session,
                                                 const FengProgram *program,
                                                 FengSlice alias_name,
                                                 bool *handled) {
    size_t index;

    if (handled == NULL) {
        return false;
    }
    *handled = false;
    if (program == NULL) {
        return true;
    }
    for (index = 0U; index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSemanticModule *module;

        if (!use_decl->has_alias || !slice_equals(use_decl->alias, alias_name)) {
            continue;
        }
        *handled = true;
        module = find_module_by_segments(session->analysis, use_decl->segments, use_decl->segment_count);
        if (module != NULL) {
            return append_semantic_module_completion_items(json, first, module, true, NULL, -1);
        }
        if (session_contains_module_program(session, use_decl->segments, use_decl->segment_count)) {
            return append_loaded_module_completion_items(json,
                                                         first,
                                                         session,
                                                         use_decl->segments,
                                                         use_decl->segment_count,
                                                         true,
                                                         NULL,
                                                         -1);
        }
        return append_project_module_completion_items(json,
                                                      first,
                                                      program->path,
                                                      use_decl->segments,
                                                      use_decl->segment_count,
                                                      true,
                                                      NULL,
                                                      -1);
    }
    return true;
}

/* Returns true if the cursor at `offset` in `text` is inside an `import` module
 * path (e.g. `import feng.` or `import feng.examples`). When true, fills
 * `prefix_segments[0..prefix_count-1]` with the already-typed path segments
 * (the ones before the current dot, if any) and `partial` with the partial
 * segment being typed. `prefix_segments` must point to an array of at least
 * `max_prefix` elements. */
static bool extract_use_path_context(const char *text,
                                     size_t offset,
                                     FengSlice *prefix_segments,
                                     size_t *prefix_count,
                                     size_t max_prefix,
                                     FengSlice *partial) {
    size_t pos = offset;
    /* Temporary storage for segments collected in reverse order. */
    FengSlice reverse_buf[16];
    size_t reverse_count = 0U;
    size_t seg_end;
    size_t i;

    *prefix_count = 0U;
    partial->data = NULL;
    partial->length = 0U;

    if (text == NULL) {
        return false;
    }

    /* Collect the partial segment being typed (identifier chars up to cursor). */
    seg_end = pos;
    while (pos > 0U && completion_identifier_continue((unsigned char)text[pos - 1U])) {
        --pos;
    }
    partial->data = text + pos;
    partial->length = seg_end - pos;

    /* Walk backward through dot-separated identifier segments to build the
     * prefix, storing them in reverse order. */
    while (pos > 0U && text[pos - 1U] == '.') {
        size_t seg_start;

        --pos; /* skip dot */
        seg_end = pos;
        while (pos > 0U && completion_identifier_continue((unsigned char)text[pos - 1U])) {
            --pos;
        }
        seg_start = pos;
        if (seg_end == seg_start) {
            break; /* empty segment between dots — malformed, stop */
        }
        if (reverse_count < 16U) {
            reverse_buf[reverse_count].data = text + seg_start;
            reverse_buf[reverse_count].length = seg_end - seg_start;
            ++reverse_count;
        }
    }

    /* Skip any whitespace before the path. */
    while (pos > 0U && (text[pos - 1U] == ' ' || text[pos - 1U] == '\t')) {
        --pos;
    }

    /* Check for the `import` keyword immediately before the path. */
    if (pos < 6U ||
        text[pos - 6U] != 'i' || text[pos - 5U] != 'm' || text[pos - 4U] != 'p' ||
        text[pos - 3U] != 'o' || text[pos - 2U] != 'r' || text[pos - 1U] != 't') {
        return false;
    }
    /* Ensure `import` is not part of a longer identifier. */
    if (pos > 6U && completion_identifier_continue((unsigned char)text[pos - 7U])) {
        return false;
    }

    /* Write prefix segments in correct order. */
    {
        size_t count = reverse_count < max_prefix ? reverse_count : max_prefix;

        for (i = 0U; i < count; ++i) {
            prefix_segments[i] = reverse_buf[count - 1U - i];
        }
        *prefix_count = count;
    }
    return true;
}

static bool append_seen_module_completion_item(FengLspString *json,
                                               bool *first,
                                               FengSlice *seen,
                                               size_t *seen_count,
                                               size_t seen_capacity,
                                               FengSlice next_seg) {
    size_t index;

    for (index = 0U; index < *seen_count; ++index) {
        if (slice_equals(seen[index], next_seg)) {
            return true;
        }
    }
    if (*seen_count < seen_capacity) {
        seen[(*seen_count)++] = next_seg;
    }
    return append_completion_item(json, first, next_seg, "module", 9);
}

static bool append_provider_use_path_completion_items(FengLspString *json,
                                                      bool *first,
                                                      const FengSymbolProvider *provider,
                                                      const FengSlice *prefix_segments,
                                                      size_t prefix_count,
                                                      FengSlice partial,
                                                      FengSlice *seen,
                                                      size_t *seen_count,
                                                      size_t seen_capacity) {
    size_t module_index;

    if (provider == NULL) {
        return true;
    }
    for (module_index = 0U; module_index < feng_symbol_provider_module_count(provider); ++module_index) {
        const FengSymbolImportedModule *module = feng_symbol_provider_module_at(provider, module_index);
        size_t segment_count = feng_symbol_module_segment_count(module);
        FengSlice next_seg;
        size_t index;

        if (segment_count <= prefix_count) {
            continue;
        }
        for (index = 0U; index < prefix_count; ++index) {
            if (!slice_equals(feng_symbol_module_segment_at(module, index), prefix_segments[index])) {
                break;
            }
        }
        if (index < prefix_count) {
            continue;
        }
        next_seg = feng_symbol_module_segment_at(module, prefix_count);
        if (partial.length > 0U &&
            (next_seg.length < partial.length ||
             memcmp(next_seg.data, partial.data, partial.length) != 0)) {
            continue;
        }
        if (!append_seen_module_completion_item(json,
                                                first,
                                                seen,
                                                seen_count,
                                                seen_capacity,
                                                next_seg)) {
            return false;
        }
    }
    return true;
}

static bool append_bundle_use_path_completion_items(FengLspString *json,
                                                    bool *first,
                                                    char *const *bundle_paths,
                                                    size_t bundle_count,
                                                    const FengSlice *prefix_segments,
                                                    size_t prefix_count,
                                                    FengSlice partial,
                                                    FengSlice *seen,
                                                    size_t *seen_count,
                                                    size_t seen_capacity) {
    FengSymbolProvider *provider = NULL;
    FengSymbolError symbol_error = {0};
    size_t bundle_index;
    bool ok = true;

    if (bundle_count == 0U) {
        return true;
    }
    if (!feng_symbol_provider_create(&provider, &symbol_error)) {
        feng_symbol_error_free(&symbol_error);
        return false;
    }
    for (bundle_index = 0U; bundle_index < bundle_count; ++bundle_index) {
        if (!feng_symbol_provider_add_bundle(provider, bundle_paths[bundle_index], &symbol_error)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        ok = append_provider_use_path_completion_items(json,
                                                       first,
                                                       provider,
                                                       prefix_segments,
                                                       prefix_count,
                                                       partial,
                                                       seen,
                                                       seen_count,
                                                       seen_capacity);
    }
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    return ok;
}

/* Emits completion items for the next segment of an import path. All loaded
 * source files whose module path starts with `prefix_segments[0..n-1]` are
 * examined; for each one the segment at index `n` (if it starts with
 * `partial`) is offered as a completion item.  Duplicate segment names are
 * suppressed. */
static bool append_use_path_completion_items(FengLspString *json,
                                             bool *first,
                                             const FengLspAnalysisSession *session,
                                             const FengSlice *prefix_segments,
                                             size_t prefix_count,
                                             FengSlice partial) {
    size_t source_index;
    FengSlice seen[64];
    size_t seen_count = 0U;

    if (session == NULL) {
        return true;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        const FengProgram *prog = session->sources[source_index].program;
        FengSlice next_seg;
        size_t i;

        if (prog == NULL || prog->module_segment_count <= prefix_count) {
            continue;
        }
        /* Check that the first `prefix_count` segments match. */
        for (i = 0U; i < prefix_count; ++i) {
            if (!slice_equals(prog->module_segments[i], prefix_segments[i])) {
                break;
            }
        }
        if (i < prefix_count) {
            continue;
        }
        /* The segment at position `prefix_count` is the candidate. */
        next_seg = prog->module_segments[prefix_count];
        /* Filter by partial prefix. */
        if (partial.length > 0U &&
            (next_seg.length < partial.length ||
             memcmp(next_seg.data, partial.data, partial.length) != 0)) {
            continue;
        }
        if (!append_seen_module_completion_item(json, first, seen, &seen_count, 64U, next_seg)) {
            return false;
        }
    }
    /* Also enumerate imported-package modules from the analysis so that
     * bundle modules appear as import-path completion candidates. */
    if (session->analysis != NULL) {
        size_t mod_index;
        for (mod_index = 0U; mod_index < session->analysis->module_count; ++mod_index) {
            const FengSemanticModule *sem_mod = &session->analysis->modules[mod_index];
            FengSlice next_seg;
            size_t i;

            if (sem_mod->origin != FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE) {
                continue;
            }
            if (sem_mod->segment_count <= prefix_count) {
                continue;
            }
            /* Check that the first `prefix_count` segments match. */
            for (i = 0U; i < prefix_count; ++i) {
                if (!slice_equals(sem_mod->segments[i], prefix_segments[i])) {
                    break;
                }
            }
            if (i < prefix_count) {
                continue;
            }
            next_seg = sem_mod->segments[prefix_count];
            /* Filter by partial prefix. */
            if (partial.length > 0U &&
                (next_seg.length < partial.length ||
                 memcmp(next_seg.data, partial.data, partial.length) != 0)) {
                continue;
            }
            if (!append_seen_module_completion_item(json, first, seen, &seen_count, 64U, next_seg)) {
                return false;
            }
        }
    }
    return append_bundle_use_path_completion_items(json,
                                                   first,
                                                   session->bundle_paths,
                                                   session->bundle_count,
                                                   prefix_segments,
                                                   prefix_count,
                                                   partial,
                                                   seen,
                                                   &seen_count,
                                                   64U);
}

/* Scans every source file in the project (except the document being edited)
 * by parsing each file individually.  This is used as a fallback when the
 * current file has a parse error that prevents the normal analysis session
 * from loading all project sources (e.g. an incomplete `import` declaration).
 * Emits the next module-path segment for all files whose module path starts
 * with `prefix_segments[0..prefix_count-1]` and whose segment at index
 * `prefix_count` starts with `partial`. */
static bool append_use_path_items_by_project_scan(const FengLspRuntime *runtime,
                                                  const FengLspDocument *document,
                                                  const FengSlice *prefix_segments,
                                                  size_t prefix_count,
                                                  FengSlice partial,
                                                  FengLspString *json,
                                                  bool *first) {
    char *manifest_path = NULL;
    FengCliProjectError error = {0};
    FengCliProjectContext context = {0};
    char *doc_resolved = NULL;
    FengSlice seen[64];
    char *seen_storage[64] = {0};
    size_t seen_count = 0U;
    size_t i;
    bool ok = true;

    (void)runtime; /* reserved for future diagnostics */

    if (document == NULL || !document->is_file || !file_exists(document->path)) {
        return true;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &error)) {
        feng_cli_project_error_dispose(&error);
        return true; /* standalone file — no project sources to scan */
    }
    feng_cli_project_error_dispose(&error);

    if (!feng_cli_project_open(manifest_path, &context, &error)) {
        feng_cli_project_error_dispose(&error);
        free(manifest_path);
        return true;
    }
    feng_cli_project_error_dispose(&error);
    free(manifest_path);
    manifest_path = NULL;

    /* Resolve the current document's canonical path so we can skip it. */
    doc_resolved = realpath(document->path, NULL);

    for (i = 0U; i < context.source_count && ok; ++i) {
        const char *src_path = context.source_paths[i];
        char *source = NULL;
        size_t source_length = 0U;
        FengProgram *prog = NULL;
        FengParseError parse_err = {0};
        size_t j;

        /* Skip the file being edited — it may have parse errors. */
        if (doc_resolved != NULL && strcmp(src_path, doc_resolved) == 0) {
            continue;
        }
        if (doc_resolved == NULL && strcmp(src_path, document->path) == 0) {
            continue;
        }

        source = feng_cli_read_entire_file(src_path, &source_length);
        if (source == NULL) {
            continue; /* unreadable — skip */
        }

        if (feng_parse_source(source, source_length, src_path, &prog, &parse_err)) {
            if (prog != NULL && prog->module_segment_count > prefix_count) {
                bool all_match = true;
                FengSlice next_seg;
                bool already_seen = false;

                for (j = 0U; j < prefix_count; ++j) {
                    if (!slice_equals(prog->module_segments[j], prefix_segments[j])) {
                        all_match = false;
                        break;
                    }
                }
                if (all_match) {
                    next_seg = prog->module_segments[prefix_count];
                    /* Filter by partial prefix. */
                    if (partial.length == 0U ||
                        (next_seg.length >= partial.length &&
                         memcmp(next_seg.data, partial.data, partial.length) == 0)) {
                        /* Deduplicate. */
                        for (j = 0U; j < seen_count; ++j) {
                            if (slice_equals(seen[j], next_seg)) {
                                already_seen = true;
                                break;
                            }
                        }
                        if (!already_seen) {
                            if (seen_count < 64U) {
                                char *owned_seg = dup_range(next_seg.data,
                                                            next_seg.data + next_seg.length);

                                if (owned_seg == NULL) {
                                    ok = false;
                                    break;
                                }
                                seen_storage[seen_count] = owned_seg;
                                seen[seen_count].data = owned_seg;
                                seen[seen_count].length = next_seg.length;
                                ++seen_count;
                            }
                            ok = append_completion_item(json, first, next_seg, "module", 9);
                        }
                    }
                }
            }
            feng_program_free(prog);
        }
        free(source);
    }

    free(doc_resolved);
    feng_cli_project_context_dispose(&context);
    for (i = 0U; i < seen_count; ++i) {
        free(seen_storage[i]);
    }
    return ok;
}

static bool append_use_path_items_by_dependency_bundles(const FengLspDocument *document,
                                                        const FengSlice *prefix_segments,
                                                        size_t prefix_count,
                                                        FengSlice partial,
                                                        FengLspString *json,
                                                        bool *first,
                                                        FengSlice *seen,
                                                        size_t *seen_count,
                                                        size_t seen_capacity) {
    char *manifest_path = NULL;
    FengCliProjectError error = {0};
    FengCliDepsResolved resolved = {0};
    bool ok = true;

    if (document == NULL || !document->is_file || !file_exists(document->path)) {
        return true;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &error)) {
        feng_cli_project_error_dispose(&error);
        return true;
    }
    feng_cli_project_error_dispose(&error);
    if (feng_cli_deps_resolve_for_manifest("feng",
                                           manifest_path,
                                           false,
                                           false,
                                           &resolved,
                                           &error)) {
        ok = append_bundle_use_path_completion_items(json,
                                                     first,
                                                     resolved.package_paths,
                                                     resolved.package_count,
                                                     prefix_segments,
                                                     prefix_count,
                                                     partial,
                                                     seen,
                                                     seen_count,
                                                     seen_capacity);
    }
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_error_dispose(&error);
    free(manifest_path);
    return ok;
}

static bool build_use_path_fallback_completion_json(const FengLspRuntime *runtime,
                                                    const FengLspDocument *document,
                                                    size_t offset,
                                                    FengLspString *json) {
    FengSlice use_prefix[16];
    size_t use_prefix_count = 0U;
    FengSlice use_partial = {0};
    FengSlice seen[64];
    size_t seen_count = 0U;
    bool first = true;

    if (!extract_use_path_context(document != NULL ? document->text : NULL,
                                  offset,
                                  use_prefix,
                                  &use_prefix_count,
                                  16U,
                                  &use_partial)) {
        return false;
    }
    if (!string_append_cstr(json, "[") ||
        !append_use_path_items_by_dependency_bundles(document,
                                                     use_prefix,
                                                     use_prefix_count,
                                                     use_partial,
                                                     json,
                                                     &first,
                                                     seen,
                                                     &seen_count,
                                                     64U) ||
        !append_use_path_items_by_project_scan(runtime,
                                               document,
                                               use_prefix,
                                               use_prefix_count,
                                               use_partial,
                                               json,
                                               &first) ||
        !string_append_cstr(json, "]")) {
        return false;
    }
    return json->length > 2U;
}

static bool session_contains_module_program(const FengLspAnalysisSession *session,
                                            const FengSlice *segments,
                                            size_t segment_count) {
    size_t source_index;

    if (session == NULL || segments == NULL || segment_count == 0U) {
        return false;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        if (program_module_matches(session->sources[source_index].program, segments, segment_count)) {
            return true;
        }
    }
    return false;
}

/* Best-effort fallback for identifier completion after `use foo.bar;` when
 * project-level analysis failed before it could hand loaded source programs
 * back to LSP. As long as the current file still parses, reopen the project,
 * scan sibling source files on disk, and surface public declarations from any
 * imported modules that are not already present in `session->sources`. */
static bool append_project_imported_completion_items(FengLspString *json,
                                                     bool *first,
                                                     const FengLspAnalysisSession *session,
                                                     const FengProgram *program) {
    char *manifest_path = NULL;
    FengCliProjectError error = {0};
    FengCliProjectContext context = {0};
    char *program_resolved = NULL;
    bool needs_scan = false;
    bool ok = true;
    size_t use_index;
    size_t source_index;

    if (json == NULL || first == NULL || session == NULL || program == NULL ||
        program->path == NULL || program->use_count == 0U || !file_exists(program->path)) {
        return true;
    }
    for (use_index = 0U; use_index < program->use_count; ++use_index) {
        const FengUseDecl *use_decl = &program->uses[use_index];

        if (!use_decl->has_alias &&
            !session_contains_module_program(session, use_decl->segments, use_decl->segment_count)) {
            needs_scan = true;
            break;
        }
    }
    if (!needs_scan) {
        return true;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(program->path, &manifest_path, &error)) {
        feng_cli_project_error_dispose(&error);
        return true;
    }
    feng_cli_project_error_dispose(&error);
    if (!feng_cli_project_open(manifest_path, &context, &error)) {
        feng_cli_project_error_dispose(&error);
        free(manifest_path);
        return true;
    }
    feng_cli_project_error_dispose(&error);
    free(manifest_path);
    program_resolved = realpath(program->path, NULL);

    for (source_index = 0U; source_index < context.source_count && ok; ++source_index) {
        const char *src_path = context.source_paths[source_index];
        char *source = NULL;
        size_t source_length = 0U;
        FengProgram *scanned_program = NULL;
        FengParseError parse_error = {0};

        if ((program_resolved != NULL && strcmp(src_path, program_resolved) == 0) ||
            (program_resolved == NULL && strcmp(src_path, program->path) == 0)) {
            continue;
        }
        source = feng_cli_read_entire_file(src_path, &source_length);
        if (source == NULL) {
            continue;
        }
        if (feng_parse_source(source, source_length, src_path, &scanned_program, &parse_error)) {
            for (use_index = 0U; use_index < program->use_count; ++use_index) {
                const FengUseDecl *use_decl = &program->uses[use_index];

                if (use_decl->has_alias ||
                    session_contains_module_program(session,
                                                    use_decl->segments,
                                                    use_decl->segment_count)) {
                    continue;
                }
                if (program_module_matches(scanned_program,
                                           use_decl->segments,
                                           use_decl->segment_count)) {
                    ok = append_program_decl_completion_items(json,
                                                              first,
                                                              scanned_program,
                                                              true,
                                                              NULL,
                                                              -1);
                    break;
                }
            }
        }
        feng_program_free(scanned_program);
        free(source);
    }

    free(program_resolved);
    feng_cli_project_context_dispose(&context);
    return ok;
}

static bool append_bundle_imported_completion_items(FengLspString *json,
                                                    bool *first,
                                                    const FengLspAnalysisSession *session,
                                                    const FengProgram *program) {
    FengSymbolProvider *provider = NULL;
    FengSymbolError symbol_error = {0};
    bool ok = true;
    size_t index;

    if (json == NULL || first == NULL || session == NULL || program == NULL ||
        session->bundle_count == 0U || program->use_count == 0U) {
        return true;
    }
    if (!feng_symbol_provider_create(&provider, &symbol_error)) {
        feng_symbol_error_free(&symbol_error);
        return false;
    }
    for (index = 0U; index < session->bundle_count; ++index) {
        if (!feng_symbol_provider_add_bundle(provider, session->bundle_paths[index], &symbol_error)) {
            ok = false;
            break;
        }
    }
    for (index = 0U; ok && index < program->use_count; ++index) {
        const FengUseDecl *use_decl = &program->uses[index];
        const FengSymbolImportedModule *module;
        size_t decl_index;

        if (use_decl->has_alias) {
            continue;
        }
        module = feng_symbol_provider_find_module(provider, use_decl->segments, use_decl->segment_count);
        if (module == NULL) {
            continue;
        }
        for (decl_index = 0U; decl_index < feng_symbol_module_public_decl_count(module); ++decl_index) {
            const FengSymbolDeclView *decl = feng_symbol_module_public_decl_at(module, decl_index);
            bool has_label = false;

            if (!symbol_decl_is_completion_decl(decl)) {
                continue;
            }
            if (!completion_json_contains_label(json, feng_symbol_decl_name(decl), &has_label)) {
                ok = false;
                break;
            }
            if (has_label) {
                continue;
            }
            if (!append_symbol_decl_completion_item(json, first, decl, NULL, -1)) {
                ok = false;
                break;
            }
        }
    }
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    return ok;
}

static const FengDecl *resolve_owner_decl_from_object_name(const FengLspAnalysisSession *session,
                                                           const FengProgram *program,
                                                           FengSlice object_name,
                                                           const FengLspLocalList *locals) {
    const FengLspLocal *local;
    const FengDecl *decl;

    if (slice_equals_cstr(object_name, "self")) {
        local = find_local(locals, slice_from_cstr("self"));
        return local != NULL ? local->self_owner_decl : NULL;
    }
    local = find_local(locals, object_name);
    if (local != NULL) {
        if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
            return resolve_named_type_ref(session, program, local->parameter->type);
        }
        if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
            return owner_decl_from_binding(session, program, local->binding);
        }
        if (local->kind == FENG_LSP_LOCAL_SELF) {
            return local->self_owner_decl;
        }
    }
    decl = resolve_value_name(session, program, object_name);
    if (decl != NULL) {
        if (decl->kind == FENG_DECL_GLOBAL_BINDING) {
            return resolve_named_type_ref(session, program, decl->as.binding.type);
        }
        if (decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
            decl->kind == FENG_DECL_SPEC) {
            return decl;
        }
    }
    decl = resolve_type_name(session, program, object_name);
    if (decl != NULL &&
        (decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
         decl->kind == FENG_DECL_SPEC)) {
        return decl;
    }
    return NULL;
}

static bool resolve_owner_builtin_name_from_object_name(const FengLspAnalysisSession *session,
                                                        const FengProgram *program,
                                                        FengSlice object_name,
                                                        const FengLspLocalList *locals,
                                                        FengSlice *out_name) {
    const FengLspLocal *local;
    const FengDecl *decl;

    if (out_name == NULL) {
        return false;
    }
    out_name->data = NULL;
    out_name->length = 0U;
    if (slice_equals_cstr(object_name, "self")) {
        local = find_local(locals, slice_from_cstr("self"));
        if (local != NULL && local->self_owner_decl != NULL && local->self_owner_decl->kind == FENG_DECL_FIT) {
            const char *builtin = builtin_name_for_single_segment_type_ref(local->self_owner_decl->as.fit_decl.target);

            if (builtin != NULL) {
                *out_name = slice_from_cstr(builtin);
                return true;
            }
        }
        return false;
    }
    local = find_local(locals, object_name);
    if (local != NULL) {
        if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
            const char *builtin = builtin_name_for_single_segment_type_ref(local->parameter->type);

            if (builtin != NULL) {
                *out_name = slice_from_cstr(builtin);
                return true;
            }
        }
        if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
            if (owner_builtin_name_from_type_fact(session, local->binding, out_name)) {
                return true;
            }
            if (local->binding->type != NULL) {
                const char *builtin = builtin_name_for_single_segment_type_ref(local->binding->type);

                if (builtin != NULL) {
                    *out_name = slice_from_cstr(builtin);
                    return true;
                }
            }
            return resolve_owner_builtin_name_from_object_expr(session,
                                                               program,
                                                               local->binding->initializer,
                                                               locals,
                                                               out_name);
        }
    }
    decl = resolve_value_name(session, program, object_name);
    if (decl != NULL && decl->kind == FENG_DECL_GLOBAL_BINDING) {
        if (owner_builtin_name_from_type_fact(session, &decl->as.binding, out_name)) {
            return true;
        }
        if (decl->as.binding.type != NULL) {
            const char *builtin = builtin_name_for_single_segment_type_ref(decl->as.binding.type);

            if (builtin != NULL) {
                *out_name = slice_from_cstr(builtin);
                return true;
            }
        }
    }
    return false;
}

static bool build_completion_json(const FengLspAnalysisSession *session,
                                  const FengProgram *program,
                                  const char *source_text,
                                  size_t offset,
                                  FengLspString *json) {
    const FengDecl *enclosing_decl;
    const FengTypeMember *enclosing_member;
    FengLspLocalList locals = {0};
    const FengExpr *expr;
    const FengSemanticModule *program_module;
    FengLspCompletionContext completion_context = {0};
    bool first = true;
    size_t index;

    if (!string_append_cstr(json, "[")) {
        return false;
    }
    (void)completion_context_from_text(source_text, offset, &completion_context);
    enclosing_decl = find_enclosing_decl_for_completion(source_text, program, offset, &enclosing_member);
    if (enclosing_decl != NULL && !collect_visible_locals_for_completion(source_text,
                                                                         enclosing_decl,
                                                                         enclosing_member,
                                                                         offset,
                                                                         &locals)) {
        local_list_dispose(&locals);
        return false;
    }
    expr = enclosing_decl != NULL ? find_expr_hit_in_decl(enclosing_decl, offset) : NULL;
    /* Check if cursor is inside an `import` module path (e.g. `import feng.` or
     * `import feng.examples`). If so, offer module path segment completions and
     * skip the normal member/identifier completion logic. */
    {
        FengSlice use_prefix[16];
        size_t use_prefix_count = 0U;
        FengSlice use_partial = {0};

        if (extract_use_path_context(source_text,
                                     offset,
                                     use_prefix,
                                     &use_prefix_count,
                                     16U,
                                     &use_partial)) {
            bool ok = append_use_path_completion_items(json,
                                                       &first,
                                                       session,
                                                       use_prefix,
                                                       use_prefix_count,
                                                       use_partial);
            local_list_dispose(&locals);
            return ok && string_append_cstr(json, "]");
        }
    }
    if (completion_context.is_member) {
        const FengDecl *owner_decl = NULL;
        FengSlice owner_builtin_name = {0};
        bool alias_handled = false;
        bool is_static = completion_context.is_static_access;

        if (completion_context.literal_builtin_name.length > 0U) {
            owner_builtin_name = completion_context.literal_builtin_name;
        } else {
            if (find_local(&locals, completion_context.object) == NULL &&
                !slice_equals_cstr(completion_context.object, "self")) {
                if (!append_alias_module_completion_items(json,
                                                          &first,
                                                          session,
                                                          program,
                                                          completion_context.object,
                                                          &alias_handled)) {
                    local_list_dispose(&locals);
                    return false;
                }
            }
            if (!alias_handled) {
                owner_decl = resolve_owner_decl_from_object_name(session,
                                                                 program,
                                                                 completion_context.object,
                                                                 &locals);
                (void)resolve_owner_builtin_name_from_object_name(session,
                                                                  program,
                                                                  completion_context.object,
                                                                  &locals,
                                                                  &owner_builtin_name);
                if (!is_static && owner_decl != NULL &&
                    find_local(&locals, completion_context.object) == NULL &&
                    resolve_type_name(session, program, completion_context.object) != NULL) {
                    is_static = true;
                }
            }
        }
        if (!alias_handled) {
            FengLspMemberFilter filter;

            if (is_static) {
                filter = FENG_LSP_MEMBER_FILTER_STATIC;
            } else if (slice_equals_cstr(completion_context.object, "self")) {
                filter = FENG_LSP_MEMBER_FILTER_ALL;
            } else {
                filter = FENG_LSP_MEMBER_FILTER_INSTANCE;
            }
            if (!append_owner_member_completion_items(json, &first, session, program, owner_decl,
                                                     filter)) {
                local_list_dispose(&locals);
                return false;
            }
            if (!append_owner_fit_member_completion_items(json,
                                                          &first,
                                                          session,
                                                          program,
                                                          owner_decl,
                                                          owner_builtin_name,
                                                          filter)) {
                local_list_dispose(&locals);
                return false;
            }
        }
    } else if (expr != NULL && expr->kind == FENG_EXPR_MEMBER) {
        const FengSemanticModule *alias_module = NULL;
        const FengDecl *owner_decl = NULL;
        FengSlice owner_builtin_name = {0};
        FengLspMemberFilter filter = FENG_LSP_MEMBER_FILTER_INSTANCE;

        if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_IDENTIFIER &&
            find_local(&locals, expr->as.member.object->as.identifier) == NULL) {
            alias_module = find_alias_module(session, program, expr->as.member.object->as.identifier);
            if (alias_module == NULL &&
                resolve_type_name(session, program, expr->as.member.object->as.identifier) != NULL) {
                filter = FENG_LSP_MEMBER_FILTER_STATIC;
            }
        } else if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_SELF) {
            filter = FENG_LSP_MEMBER_FILTER_ALL;
        }
        if (alias_module != NULL) {
            if (!append_semantic_module_completion_items(json, &first, alias_module, true, NULL, -1)) {
                local_list_dispose(&locals);
                return false;
            }
        } else {
            owner_decl = resolve_owner_decl_from_object_expr(session,
                                                             program,
                                                             expr->as.member.object,
                                                             &locals);
            if (!append_owner_member_completion_items(json, &first, session, program, owner_decl,
                                                     filter)) {
                local_list_dispose(&locals);
                return false;
            }
            (void)resolve_owner_builtin_name_from_object_expr(session,
                                                              program,
                                                              expr->as.member.object,
                                                              &locals,
                                                              &owner_builtin_name);
            if (!append_owner_fit_member_completion_items(json,
                                                          &first,
                                                          session,
                                                          program,
                                                          owner_decl,
                                                          owner_builtin_name,
                                                          filter)) {
                local_list_dispose(&locals);
                return false;
            }
        }
    } else {
        for (index = 0U; index < locals.count; ++index) {
            if (!append_completion_item(json,
                                        &first,
                                        locals.items[index].name,
                                        locals.items[index].kind == FENG_LSP_LOCAL_SELF ? "self" : "local",
                                        6)) {
                local_list_dispose(&locals);
                return false;
            }
        }
        program_module = find_program_module(session, program);
        if (program_module != NULL) {
            if (!append_semantic_module_completion_items(json, &first, program_module, false, NULL, -1)) {
                local_list_dispose(&locals);
                return false;
            }
        } else if (!append_loaded_module_completion_items(json,
                                                          &first,
                                                          session,
                                                          program->module_segments,
                                                          program->module_segment_count,
                                                          false,
                                                          NULL,
                                                          -1)) {
            local_list_dispose(&locals);
            return false;
        }
        for (index = 0U; index < program->use_count; ++index) {
            const FengUseDecl *use_decl = &program->uses[index];
            const FengSemanticModule *module = find_module_by_segments(session->analysis,
                                                                       use_decl->segments,
                                                                       use_decl->segment_count);
            if (use_decl->has_alias) {
                if (!append_completion_item(json, &first, use_decl->alias, "module", 9)) {
                    local_list_dispose(&locals);
                    return false;
                }
                continue;
            }
            if (module != NULL) {
                if (!append_semantic_module_completion_items(json, &first, module, true, NULL, -1)) {
                    local_list_dispose(&locals);
                    return false;
                }
            } else if (!append_loaded_module_completion_items(json,
                                                              &first,
                                                              session,
                                                              use_decl->segments,
                                                              use_decl->segment_count,
                                                              true,
                                                              NULL,
                                                              -1)) {
                local_list_dispose(&locals);
                return false;
            }
        }
        if (!append_project_imported_completion_items(json, &first, session, program)) {
            local_list_dispose(&locals);
            return false;
        }
        if (!append_bundle_imported_completion_items(json, &first, session, program)) {
            local_list_dispose(&locals);
            return false;
        }
    }
    local_list_dispose(&locals);
    return string_append_cstr(json, "]");
}

static bool build_cached_completion_json(const FengLspCacheQueryContext *context,
                                         size_t offset,
                                         FengLspString *json,
                                         size_t *out_item_count) {
    const FengDecl *enclosing_decl;
    const FengTypeMember *enclosing_member;
    FengLspLocalList locals = {0};
    const FengExpr *expr;
    bool first = true;
    size_t item_count = 0U;
    size_t index;

    if (out_item_count == NULL || !string_append_cstr(json, "[")) {
        return false;
    }
    *out_item_count = 0U;
    enclosing_decl = find_enclosing_decl_for_completion(context->source_text,
                                                        context->program,
                                                        offset,
                                                        &enclosing_member);
    if (enclosing_decl != NULL && !collect_visible_locals_for_completion(context->source_text,
                                                                         enclosing_decl,
                                                                         enclosing_member,
                                                                         offset,
                                                                         &locals)) {
        local_list_dispose(&locals);
        return false;
    }
    {
        FengSlice use_prefix[16];
        size_t use_prefix_count = 0U;
        FengSlice use_partial = {0};

        if (extract_use_path_context(context->source_text,
                                     offset,
                                     use_prefix,
                                     &use_prefix_count,
                                     16U,
                                     &use_partial)) {
            FengSlice seen[64];
            size_t seen_count = 0U;
            bool ok = append_provider_use_path_completion_items(json,
                                                                &first,
                                                                context->provider,
                                                                use_prefix,
                                                                use_prefix_count,
                                                                use_partial,
                                                                seen,
                                                                &seen_count,
                                                                64U) &&
                      string_append_cstr(json, "]");

            local_list_dispose(&locals);
            if (!ok) {
                return false;
            }
            *out_item_count = first ? 0U : 1U;
            return true;
        }
    }
    expr = enclosing_decl != NULL ? find_expr_hit_in_decl(enclosing_decl, offset) : NULL;
    if (expr != NULL && expr->kind == FENG_EXPR_MEMBER) {
        const FengSymbolImportedModule *alias_module = NULL;
        const FengSymbolDeclView *owner_decl = NULL;
        FengLspMemberFilter filter = FENG_LSP_MEMBER_FILTER_INSTANCE;

        if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_IDENTIFIER &&
            find_local(&locals, expr->as.member.object->as.identifier) == NULL) {
            alias_module = find_symbol_alias_module(context->provider,
                                                    context->program,
                                                    expr->as.member.object->as.identifier);
            if (alias_module == NULL) {
                const FengSymbolDeclView *vdecl = resolve_symbol_value_name(context->provider,
                                                                             context->current_module,
                                                                             context->program,
                                                                             expr->as.member.object->as.identifier);

                if (vdecl != NULL) {
                    FengSymbolDeclKind vkind = feng_symbol_decl_kind(vdecl);

                    if (vkind == FENG_SYMBOL_DECL_KIND_TYPE || vkind == FENG_SYMBOL_DECL_KIND_ENUM ||
                        vkind == FENG_SYMBOL_DECL_KIND_SPEC) {
                        filter = FENG_LSP_MEMBER_FILTER_STATIC;
                    }
                } else if (resolve_symbol_type_name(context->provider,
                                                     context->current_module,
                                                     context->program,
                                                     expr->as.member.object->as.identifier) != NULL) {
                    filter = FENG_LSP_MEMBER_FILTER_STATIC;
                }
            }
        } else if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_SELF) {
            filter = FENG_LSP_MEMBER_FILTER_ALL;
        }
        if (alias_module != NULL) {
            for (index = 0U; index < feng_symbol_module_public_decl_count(alias_module); ++index) {
                const FengSymbolDeclView *decl = feng_symbol_module_public_decl_at(alias_module, index);

                if (!symbol_decl_is_completion_decl(decl)) {
                    continue;
                }
                if (!append_symbol_decl_completion_item(json, &first, decl, NULL, -1)) {
                    local_list_dispose(&locals);
                    return false;
                }
                ++item_count;
            }
        } else {
            owner_decl = resolve_symbol_owner_decl_from_expr(context,
                                                            expr->as.member.object,
                                                            &locals);
            if (owner_decl != NULL) {
                FengSlice owner_slice = feng_symbol_decl_name(owner_decl);
                char *sym_owner_name = dup_range(owner_slice.data, owner_slice.data + owner_slice.length);

                for (index = 0U; index < feng_symbol_decl_member_count(owner_decl); ++index) {
                    const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner_decl, index);
                    bool contains = false;

                    if (!symbol_member_passes_filter(member, filter)) {
                        continue;
                    }
                    if (!symbol_member_visible_from_module(context->current_module,
                                                           owner_decl,
                                                           member)) {
                        continue;
                    }
                    if (!completion_json_contains_label(json, feng_symbol_decl_name(member), &contains)) {
                        free(sym_owner_name);
                        local_list_dispose(&locals);
                        return false;
                    }
                    if (contains) {
                        continue;
                    }
                    if (!append_symbol_member_completion_item(json, &first, member, sym_owner_name)) {
                        free(sym_owner_name);
                        local_list_dispose(&locals);
                        return false;
                    }
                    ++item_count;
                }
                free(sym_owner_name);
            }
            if (owner_decl == NULL || item_count == 0U) {
                FengSlice builtin_name = resolve_symbol_builtin_name_from_expr(context,
                                                                               expr->as.member.object,
                                                                               &locals);

                if (builtin_name.length > 0U) {
                    size_t mod_count = feng_symbol_provider_module_count(context->provider);
                    size_t mod_idx;

                    for (mod_idx = 0U; mod_idx < mod_count; ++mod_idx) {
                        const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mod_idx);
                        size_t fit_count = feng_symbol_module_fit_count(mod);
                        size_t fit_idx;

                        for (fit_idx = 0U; fit_idx < fit_count; ++fit_idx) {
                            const FengSymbolFitView *fit = feng_symbol_module_fit_at(mod, fit_idx);
                            const FengSymbolDeclView *fd = feng_symbol_fit_decl(fit);
                            const FengSymbolTypeView *target;
                            size_t member_count;
                            size_t member_idx;

                            if (fd == NULL) {
                                continue;
                            }
                            target = feng_symbol_decl_fit_target(fd);
                            if (target == NULL) {
                                continue;
                            }
                            if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                                if (!slice_equals(feng_symbol_type_builtin_name(target), builtin_name)) {
                                    continue;
                                }
                            } else if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_NAMED &&
                                       feng_symbol_type_segment_count(target) == 1U) {
                                FengSlice seg = feng_symbol_type_segment_at(target, 0U);

                                if (!slice_equals(seg, builtin_name)) {
                                    continue;
                                }
                            } else {
                                continue;
                            }
                            member_count = feng_symbol_decl_member_count(fd);
                            for (member_idx = 0U; member_idx < member_count; ++member_idx) {
                                const FengSymbolDeclView *member = feng_symbol_decl_member_at(fd, member_idx);
                                bool contains = false;

                                if (!symbol_member_passes_filter(member, filter)) {
                                    continue;
                                }
                                if (!completion_json_contains_label(json, feng_symbol_decl_name(member), &contains)) {
                                    local_list_dispose(&locals);
                                    return false;
                                }
                                if (contains) {
                                    continue;
                                }
                                if (!append_symbol_member_completion_item(json, &first, member, NULL)) {
                                    local_list_dispose(&locals);
                                    return false;
                                }
                                ++item_count;
                            }
                        }
                    }
                }
            }
        }
    } else {
        for (index = 0U; index < locals.count; ++index) {
            if (!append_completion_item(json,
                                        &first,
                                        locals.items[index].name,
                                        locals.items[index].kind == FENG_LSP_LOCAL_SELF ? "self" : "local",
                                        6)) {
                local_list_dispose(&locals);
                return false;
            }
            ++item_count;
        }
        if (context->current_module != NULL) {
            for (index = 0U; index < feng_symbol_module_decl_count(context->current_module); ++index) {
                const FengSymbolDeclView *decl = feng_symbol_module_decl_at(context->current_module, index);

                if (!symbol_decl_is_completion_decl(decl)) {
                    continue;
                }
                if (!append_symbol_decl_completion_item(json, &first, decl, NULL, -1)) {
                    local_list_dispose(&locals);
                    return false;
                }
                ++item_count;
            }
        }
        for (index = 0U; index < context->program->use_count; ++index) {
            const FengUseDecl *use_decl = &context->program->uses[index];
            const FengSymbolImportedModule *module = feng_symbol_provider_find_module(context->provider,
                                                                                      use_decl->segments,
                                                                                      use_decl->segment_count);

            if (use_decl->has_alias) {
                if (!append_completion_item(json, &first, use_decl->alias, "module", 9)) {
                    local_list_dispose(&locals);
                    return false;
                }
                ++item_count;
                continue;
            }
            if (module != NULL) {
                size_t decl_index;

                for (decl_index = 0U; decl_index < feng_symbol_module_public_decl_count(module); ++decl_index) {
                    const FengSymbolDeclView *decl = feng_symbol_module_public_decl_at(module, decl_index);

                    if (!symbol_decl_is_completion_decl(decl)) {
                        continue;
                    }
                    if (!append_symbol_decl_completion_item(json, &first, decl, NULL, -1)) {
                        local_list_dispose(&locals);
                        return false;
                    }
                    ++item_count;
                }
            }
        }
    }
    local_list_dispose(&locals);
    *out_item_count = item_count;
    return string_append_cstr(json, "]");
}

static bool completion_json_has_items(const FengLspString *json) {
    return json != NULL && json->length > 2U;
}

static bool completion_context_is_member_dot(const char *text, size_t offset) {
    size_t length;
    size_t cursor;

    if (text == NULL || offset == 0U) {
        return false;
    }
    length = strlen(text);
    if (offset > length) {
        return false;
    }
    cursor = offset;
    while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
        --cursor;
    }
    if (cursor == 0U || text[cursor - 1U] != '.') {
        return false;
    }
    while (cursor > 1U && isspace((unsigned char)text[cursor - 2U])) {
        --cursor;
    }
    if (cursor < 2U) {
        return false;
    }
    return completion_identifier_continue(text[cursor - 2U]) ||
           text[cursor - 2U] == ')' || text[cursor - 2U] == ']' || text[cursor - 2U] == '"';
}

static bool completion_context_is_member_access(const char *text, size_t offset) {
    FengLspCompletionContext context = {0};

    return (completion_context_from_text(text, offset, &context) && context.is_member) ||
           completion_context_is_member_dot(text, offset);
}

static bool completion_repair_has_expression_tail(const char *text, size_t offset) {
    size_t cursor;
    char ch;

    if (text == NULL || offset == 0U) {
        return false;
    }
    cursor = offset;
    while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
        --cursor;
    }
    if (cursor == 0U) {
        return false;
    }
    ch = text[cursor - 1U];
    if (ch == '.') {
        return completion_context_is_member_dot(text, offset);
    }
    return completion_identifier_continue(ch) || ch == ')' || ch == ']' || ch == '"';
}

static bool completion_repair_needs_semicolon(const char *text, size_t offset) {
    size_t length;
    size_t cursor;
    bool has_expression_tail;

    if (text == NULL) {
        return false;
    }
    length = strlen(text);
    if (offset > length) {
        return false;
    }
    has_expression_tail = completion_repair_has_expression_tail(text, offset);
    if (!has_expression_tail) {
        return false;
    }
    cursor = offset;
    while (cursor < length && isspace((unsigned char)text[cursor])) {
        ++cursor;
    }
    if (cursor >= length) {
        return true;
    }
    switch (text[cursor]) {
        case ';':
        case ',':
        case ')':
        case ']':
            return false;
        case '}':
            return true;
        default:
            return true;
    }
}

static char *dup_text_with_completion_repair(const char *text, size_t offset) {
    static const char kPlaceholder[] = "__feng_completion_placeholder__";
    size_t text_length;
    size_t placeholder_length = 0U;
    size_t semicolon_length = 0U;
    bool is_member_access;
    char *out;

    if (text == NULL) {
        return NULL;
    }
    text_length = strlen(text);
    if (offset > text_length) {
        return NULL;
    }
    is_member_access = completion_context_is_member_access(text, offset);
    if (is_member_access && completion_context_is_member_dot(text, offset)) {
        placeholder_length = strlen(kPlaceholder);
    }
    if (completion_repair_needs_semicolon(text, offset)) {
        semicolon_length = 1U;
    }
    if (!is_member_access && semicolon_length == 0U) {
        return NULL;
    }
    out = (char *)malloc(text_length + placeholder_length + semicolon_length + 1U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, offset);
    if (placeholder_length > 0U) {
        memcpy(out + offset, kPlaceholder, placeholder_length);
    }
    if (semicolon_length > 0U) {
        out[offset + placeholder_length] = ';';
    }
    memcpy(out + offset + placeholder_length + semicolon_length,
           text + offset,
           text_length - offset + 1U);
    return out;
}

static bool build_single_parse_session(const FengLspDocument *document,
                                       FengLspAnalysisSession *session) {
    FengParseError parse_error = {0};

    memset(session, 0, sizeof(*session));
    if (document == NULL || document->path == NULL || document->text == NULL) {
        return false;
    }
    session->sources = (FengCliLoadedSource *)calloc(1U, sizeof(session->sources[0]));
    if (session->sources == NULL) {
        return false;
    }
    session->sources[0].path = document->path;
    session->sources[0].source = dup_cstr(document->text);
    if (session->sources[0].source == NULL) {
        session_dispose(session);
        return false;
    }
    session->sources[0].source_length = strlen(session->sources[0].source);
    if (!feng_parse_source(session->sources[0].source,
                           session->sources[0].source_length,
                           session->sources[0].path,
                           &session->sources[0].program,
                           &parse_error)) {
        session_dispose(session);
        return false;
    }
    session->source_count = 1U;
    return true;
}

/* Build completion items for a literal builtin type (e.g. i32, f64, string,
 * bool) by querying fit declarations in the symbol provider.  This path does
 * NOT require a successful parse of the source file. */
static bool build_literal_builtin_completion_json(const FengLspDocument *document,
                                                  FengSlice builtin_name,
                                                  FengLspMemberFilter filter,
                                                  FengLspString *json) {
    char *manifest_path = NULL;
    FengCliProjectContext project = {0};
    FengCliProjectError project_error = {0};
    FengCliDepsResolved resolved = {0};
    FengSymbolProvider *provider = NULL;
    FengSymbolError symbol_error = {0};
    size_t mod_count;
    size_t mod_idx;
    bool first = true;
    bool found_any = false;

    if (document == NULL || builtin_name.length == 0U || json == NULL) {
        return false;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &project_error)) {
        feng_cli_project_error_dispose(&project_error);
        free(manifest_path);
        return false;
    }
    if (!feng_cli_project_open(manifest_path, &project, &project_error)) {
        feng_cli_project_error_dispose(&project_error);
        free(manifest_path);
        return false;
    }
    if (!feng_symbol_provider_create(&provider, &symbol_error)) {
        goto cleanup;
    }
    if (feng_cli_deps_resolve_for_manifest("feng",
                                           project.manifest_path,
                                           false,
                                           false,
                                           &resolved,
                                           &project_error)) {
        size_t index;

        for (index = 0U; index < resolved.package_count; ++index) {
            (void)feng_symbol_provider_add_bundle(provider,
                                                  resolved.package_paths[index],
                                                  &symbol_error);
        }
    }
    {
        char *symbols_root = path_join(project.out_root, "obj/symbols");

        if (symbols_root != NULL && path_is_directory(symbols_root)) {
            (void)feng_symbol_provider_add_ft_root(provider,
                                                   symbols_root,
                                                   FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                                                   &symbol_error);
        }
        free(symbols_root);
    }
    if (!string_append_cstr(json, "[")) {
        goto cleanup;
    }
    mod_count = feng_symbol_provider_module_count(provider);
    for (mod_idx = 0U; mod_idx < mod_count; ++mod_idx) {
        const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(provider, mod_idx);
        size_t fit_count = feng_symbol_module_fit_count(mod);
        size_t fit_idx;

        for (fit_idx = 0U; fit_idx < fit_count; ++fit_idx) {
            const FengSymbolFitView *fit = feng_symbol_module_fit_at(mod, fit_idx);
            const FengSymbolDeclView *fit_decl = feng_symbol_fit_decl(fit);
            const FengSymbolTypeView *target;
            size_t member_count;
            size_t member_idx;

            if (fit_decl == NULL) {
                continue;
            }
            target = feng_symbol_decl_fit_target(fit_decl);
            if (target == NULL) {
                continue;
            }
            if (feng_symbol_type_kind(target) != FENG_SYMBOL_TYPE_KIND_BUILTIN ||
                !slice_equals(feng_symbol_type_builtin_name(target), builtin_name)) {
                if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_NAMED &&
                    feng_symbol_type_segment_count(target) == 1U) {
                    FengSlice seg = feng_symbol_type_segment_at(target, 0U);
                    const char *canonical = NULL;

                    if (slice_equals_cstr(seg, "int") || slice_equals_cstr(seg, "i32")) {
                        canonical = "i32";
                    } else if (slice_equals_cstr(seg, "long") || slice_equals_cstr(seg, "i64")) {
                        canonical = "i64";
                    } else if (slice_equals_cstr(seg, "double") || slice_equals_cstr(seg, "f64")) {
                        canonical = "f64";
                    } else if (slice_equals_cstr(seg, "float") || slice_equals_cstr(seg, "f32")) {
                        canonical = "f32";
                    } else if (slice_equals_cstr(seg, "byte") || slice_equals_cstr(seg, "u8")) {
                        canonical = "u8";
                    } else if (slice_equals_cstr(seg, "bool")) {
                        canonical = "bool";
                    } else if (slice_equals_cstr(seg, "string")) {
                        canonical = "string";
                    }
                    if (canonical == NULL || !slice_equals_cstr(builtin_name, canonical)) {
                        continue;
                    }
                } else {
                    continue;
                }
            }
            member_count = feng_symbol_decl_member_count(fit_decl);
            for (member_idx = 0U; member_idx < member_count; ++member_idx) {
                const FengSymbolDeclView *member = feng_symbol_decl_member_at(fit_decl, member_idx);
                bool contains = false;

                if (!symbol_member_passes_filter(member, filter)) {
                    continue;
                }
                if (!completion_json_contains_label(json, feng_symbol_decl_name(member), &contains)) {
                    goto cleanup;
                }
                if (contains) {
                    continue;
                }
                if (!append_symbol_member_completion_item(json, &first, member, NULL)) {
                    goto cleanup;
                }
                found_any = true;
            }
        }
    }
    if (!string_append_cstr(json, "]")) {
        goto cleanup;
    }

cleanup:
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_context_dispose(&project);
    feng_cli_project_error_dispose(&project_error);
    free(manifest_path);
    return found_any;
}

static bool build_repaired_completion_json(const FengLspRuntime *runtime,
                                           const FengLspDocument *document,
                                           size_t offset,
                                           FengLspString *json) {
    FengLspDocument repaired;
    FengLspAnalysisSession session = {0};
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    bool ok = false;

    if (runtime == NULL || document == NULL || json == NULL) {
        return false;
    }
    repaired = *document;
    repaired.text = dup_text_with_completion_repair(document->text, offset);
    if (repaired.text == NULL) {
        return false;
    }
    if (build_analysis_session(runtime, &repaired, &session)) {
        program = find_program(&session, repaired.path);
        ok = program != NULL && build_completion_json(&session, program, repaired.text, offset, json);
    }
    session_dispose(&session);
    if (!completion_json_has_items(json)) {
        string_dispose(json);
        ok = false;
        if (build_cache_query_context_for_text(document, repaired.text, false, &cache)) {
            size_t cache_item_count = 0U;

            ok = build_cached_completion_json(&cache, offset, json, &cache_item_count) &&
                 cache_item_count > 0U;
            cache_query_context_dispose(&cache);
        }
    }
    if (!completion_json_has_items(json)) {
        string_dispose(json);
        ok = false;
        if (build_single_parse_session(&repaired, &session)) {
            program = find_program(&session, repaired.path);
            ok = program != NULL && build_completion_json(&session, program, repaired.text, offset, json);
        }
        session_dispose(&session);
    }
    free(repaired.text);
    return ok;
}

static bool handle_completion_request(FengLspRuntime *runtime,
                                      FILE *output,
                                      FengLspJsonValue id,
                                      FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    char *uri;
    unsigned int line;
    unsigned int character;
    FengLspDocument *document;
    FengLspAnalysisSession session = {0};
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    FengLspString json = {0};
    FengLspString cache_json = {0};
    bool ok;
    size_t offset;
    bool is_member_completion;
    bool can_repair_completion;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    if (uri == NULL || !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "[]");
    }
    offset = offset_from_position(document->text, line, character);
    is_member_completion = completion_context_is_member_access(document->text, offset);
    can_repair_completion = is_member_completion || completion_repair_needs_semicolon(document->text, offset);
    g_completion_uri = uri;
    if (build_cache_query_context(document, &cache)) {

        size_t cache_item_count = 0U;

        if (build_cached_completion_json(&cache, offset, &cache_json, &cache_item_count) && cache_item_count > 0U) {
            cache_query_context_dispose(&cache);
            free(uri);
            ok = send_json_response(output, id, cache_json.data);
            string_dispose(&cache_json);
            return ok;
        }
    }
    cache_query_context_dispose(&cache);
    string_dispose(&cache_json);
    /* Fast path: for member completion on a dirty document, try the cache
     * without workspace symbols.  Bundle symbols are still available, which
     * covers external-package types like DateTime. */
    if (is_member_completion) {
        char *repaired_text = dup_text_with_completion_repair(document->text, offset);

        if (repaired_text != NULL) {
            FengLspCacheQueryContext repair_cache = {0};

            if (build_cache_query_context_for_text(document, repaired_text, false, &repair_cache)) {
                size_t repair_item_count = 0U;

                if (build_cached_completion_json(&repair_cache, offset, &json, &repair_item_count) &&
                    repair_item_count > 0U) {
                    cache_query_context_dispose(&repair_cache);
                    free(repaired_text);
                    free(uri);
                    ok = send_json_response(output, id, json.data);
                    string_dispose(&json);
                    return ok;
                }
                cache_query_context_dispose(&repair_cache);
            }
            free(repaired_text);
        }
    }
    if (build_analysis_session(runtime, document, &session)) {
        program = find_program(&session, document->path);
        if (program != NULL) {
            ok = build_completion_json(&session, program, document->text, offset, &json);
            session_dispose(&session);
            if (!ok) {
                if (runtime->errors != NULL) {
                    fprintf(runtime->errors, "lsp: textDocument/completion: out of memory building response\n");
                }
                free(uri);
                string_dispose(&json);
                return send_json_response(output, id, "[]");
            }
            if (completion_json_has_items(&json) || !can_repair_completion) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
        } else {
            session_dispose(&session);
            if (build_use_path_fallback_completion_json(runtime, document, offset, &json)) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
        }
    }
    if (can_repair_completion && build_repaired_completion_json(runtime, document, offset, &json) &&
        completion_json_has_items(&json)) {
        free(uri);
        ok = send_json_response(output, id, json.data);
        string_dispose(&json);
        return ok;
    }
    string_dispose(&json);
    if (build_single_parse_session(document, &session)) {
        program = find_program(&session, document->path);
        if (program != NULL) {
            ok = build_completion_json(&session, program, document->text, offset, &json);
            session_dispose(&session);
            if (!ok) {
                if (runtime->errors != NULL) {
                    fprintf(runtime->errors, "lsp: textDocument/completion: out of memory building parser fallback response\n");
                }
                free(uri);
                string_dispose(&json);
                return send_json_response(output, id, "[]");
            }
            if (completion_json_has_items(&json) || !can_repair_completion) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
        } else {
            session_dispose(&session);
        }
    }
    if (build_use_path_fallback_completion_json(runtime, document, offset, &json)) {
        free(uri);
        ok = send_json_response(output, id, json.data);
        string_dispose(&json);
        return ok;
    }
    string_dispose(&json);
    /* Builtin type fallback: look up fit members directly from the symbol
     * table — no parse required.  Covers literal access (`123.`, `"str".`)
     * and static access on builtin type names (`i32.`, `string.`). */
    {
        FengLspCompletionContext literal_ctx = {0};

        (void)completion_context_from_text(document->text, offset, &literal_ctx);
        if (literal_ctx.is_member && literal_ctx.literal_builtin_name.length > 0U &&
            build_literal_builtin_completion_json(document, literal_ctx.literal_builtin_name,
                                                    literal_ctx.is_static_access
                                                        ? FENG_LSP_MEMBER_FILTER_STATIC
                                                        : FENG_LSP_MEMBER_FILTER_INSTANCE,
                                                    &json)) {
            g_completion_uri = NULL;
            free(uri);
            ok = send_json_response(output, id, json.data);
            string_dispose(&json);
            return ok;
        }
        string_dispose(&json);
    }
    g_completion_uri = NULL;
    free(uri);
    return send_json_response(output, id, "[]");
}

/* Append the signature of a symbol member to a markdown buffer.
 * Produces: func name(param1: Type1, param2: Type2): ReturnType */
static bool append_symbol_member_signature(FengLspString *buffer,
                                           const FengSymbolDeclView *member) {
    size_t param_count;
    size_t i;
    const FengSymbolTypeView *ret_type;
    FengSlice name;

    if (buffer == NULL || member == NULL) {
        return false;
    }
    name = feng_symbol_decl_name(member);
    if (!string_append_cstr(buffer, "func ") ||
        !string_append_bytes(buffer, name.data, name.length) ||
        !string_append_cstr(buffer, "(")) {
        return false;
    }
    param_count = feng_symbol_decl_param_count(member);
    for (i = 0U; i < param_count; ++i) {
        FengSlice pname = feng_symbol_decl_param_name(member, i);

        if (i > 0U && !string_append_cstr(buffer, ", ")) {
            return false;
        }
        if (pname.length > 0U && !string_append_bytes(buffer, pname.data, pname.length)) {
            return false;
        }
        if (!string_append_cstr(buffer, ": ")) {
            return false;
        }
        if (!symbol_param_type_to_string(buffer, member, i)) {
            return false;
        }
    }
    if (!string_append_cstr(buffer, ")")) {
        return false;
    }
    ret_type = feng_symbol_decl_return_type(member);
    if (ret_type != NULL) {
        if (!string_append_cstr(buffer, ": ") ||
            !symbol_type_to_string(buffer, ret_type)) {
            return false;
        }
    }
    return true;
}

/* Collect all overloads of a member with the given label from a type decl
 * (direct members + fit members) and format as markdown documentation.
 * Returns a newly allocated string or NULL if no overloads found. */
static char *collect_overload_docs_from_cache(const FengLspCacheQueryContext *context,
                                              const FengSymbolDeclView *owner_decl,
                                              const char *label) {
    FengLspString result = {0};
    FengSlice label_slice = slice_from_cstr(label);
    size_t overload_count = 0U;
    size_t member_count;
    size_t index;
    size_t mod_count;
    size_t mod_idx;

    if (context == NULL || owner_decl == NULL || label == NULL) {
        return NULL;
    }
    member_count = feng_symbol_decl_member_count(owner_decl);
    for (index = 0U; index < member_count; ++index) {
        const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner_decl, index);
        FengSlice doc;

        if (!slice_equals(feng_symbol_decl_name(member), label_slice)) {
            continue;
        }
        if (overload_count > 0U) {
            if (!string_append_cstr(&result, "\n\n---\n\n")) {
                string_dispose(&result);
                return NULL;
            }
        }
        if (!string_append_cstr(&result, "```feng\n") ||
            !append_symbol_member_signature(&result, member) ||
            !string_append_cstr(&result, "\n```\n")) {
            string_dispose(&result);
            return NULL;
        }
        doc = feng_symbol_decl_doc(member);
        if (doc.length > 0U) {
            if (!string_append_bytes(&result, doc.data, doc.length)) {
                string_dispose(&result);
                return NULL;
            }
        }
        ++overload_count;
    }
    if (context->provider != NULL) {
        FengSlice owner_name = feng_symbol_decl_name(owner_decl);

        mod_count = feng_symbol_provider_module_count(context->provider);
        for (mod_idx = 0U; mod_idx < mod_count; ++mod_idx) {
            const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mod_idx);
            size_t fit_count = feng_symbol_module_fit_count(mod);
            size_t fit_idx;

            for (fit_idx = 0U; fit_idx < fit_count; ++fit_idx) {
                const FengSymbolFitView *fit = feng_symbol_module_fit_at(mod, fit_idx);
                const FengSymbolDeclView *fd = feng_symbol_fit_decl(fit);
                const FengSymbolTypeView *target;
                size_t fc;
                size_t fi;

                if (fd == NULL) {
                    continue;
                }
                target = feng_symbol_decl_fit_target(fd);
                if (target == NULL) {
                    continue;
                }
                if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_NAMED &&
                    feng_symbol_type_segment_count(target) >= 1U) {
                    FengSlice seg = feng_symbol_type_segment_at(target,
                                                                feng_symbol_type_segment_count(target) - 1U);
                    if (!slice_equals(seg, owner_name)) {
                        continue;
                    }
                } else if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                    if (!slice_equals(feng_symbol_type_builtin_name(target), owner_name)) {
                        continue;
                    }
                } else {
                    continue;
                }
                fc = feng_symbol_decl_member_count(fd);
                for (fi = 0U; fi < fc; ++fi) {
                    const FengSymbolDeclView *member = feng_symbol_decl_member_at(fd, fi);
                    FengSlice doc;

                    if (!slice_equals(feng_symbol_decl_name(member), label_slice)) {
                        continue;
                    }
                    if (overload_count > 0U) {
                        if (!string_append_cstr(&result, "\n\n---\n\n")) {
                            string_dispose(&result);
                            return NULL;
                        }
                    }
                    if (!string_append_cstr(&result, "```feng\n") ||
                        !append_symbol_member_signature(&result, member) ||
                        !string_append_cstr(&result, "\n```\n")) {
                        string_dispose(&result);
                        return NULL;
                    }
                    doc = feng_symbol_decl_doc(member);
                    if (doc.length > 0U) {
                        if (!string_append_bytes(&result, doc.data, doc.length)) {
                            string_dispose(&result);
                            return NULL;
                        }
                    }
                    ++overload_count;
                }
            }
        }
    }
    if (overload_count == 0U) {
        string_dispose(&result);
        return NULL;
    }
    return result.data;
}

/* Look up the doc comment for a declaration or member by name in a cache
 * context.  Returns a newly allocated normalized string or NULL. */
static char *resolve_doc_from_cache(const FengLspCacheQueryContext *context,
                                    const char *label,
                                    const char *owner_name) {
    size_t index;

    if (context == NULL || context->provider == NULL || label == NULL) {
        return NULL;
    }
    if (owner_name != NULL) {
        FengSlice owner_slice = slice_from_cstr(owner_name);
        const FengSymbolDeclView *owner_decl = NULL;

        if (context->current_module != NULL) {
            size_t count = feng_symbol_module_decl_count(context->current_module);
            for (index = 0U; index < count; ++index) {
                const FengSymbolDeclView *d = feng_symbol_module_decl_at(context->current_module, index);
                if (slice_equals(feng_symbol_decl_name(d), owner_slice)) {
                    owner_decl = d;
                    break;
                }
            }
        }
        if (owner_decl == NULL) {
            size_t mod_count = feng_symbol_provider_module_count(context->provider);
            size_t mod_idx;
            for (mod_idx = 0U; mod_idx < mod_count && owner_decl == NULL; ++mod_idx) {
                const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mod_idx);
                owner_decl = feng_symbol_module_find_public_type(mod, owner_slice);
            }
        }
        if (owner_decl != NULL) {
            return collect_overload_docs_from_cache(context, owner_decl, label);
        }
        return NULL;
    }
    if (context->current_module != NULL) {
        FengSlice label_slice = slice_from_cstr(label);
        size_t count = feng_symbol_module_decl_count(context->current_module);
        for (index = 0U; index < count; ++index) {
            const FengSymbolDeclView *d = feng_symbol_module_decl_at(context->current_module, index);
            if (slice_equals(feng_symbol_decl_name(d), label_slice)) {
                FengSlice doc = feng_symbol_decl_doc(d);
                if (doc.length > 0U) {
                    return dup_range(doc.data, doc.data + doc.length);
                }
                return NULL;
            }
        }
    }
    return NULL;
}

/* Look up the doc comment for a declaration or member by name in an AST
 * analysis session. */
static char *resolve_doc_from_session(const FengLspAnalysisSession *session,
                                      const FengProgram *program,
                                      const char *label,
                                      const char *owner_name) {
    size_t decl_index;
    size_t member_index;
    FengSlice label_slice;

    if (session == NULL || label == NULL) {
        return NULL;
    }
    label_slice = slice_from_cstr(label);
    if (owner_name != NULL) {
        FengSlice owner_slice = slice_from_cstr(owner_name);
        if (program != NULL) {
            for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
                const FengDecl *decl = program->declarations[decl_index];
                if (!slice_equals(decl_name(decl), owner_slice)) {
                    continue;
                }
                if (decl->kind == FENG_DECL_TYPE) {
                    for (member_index = 0U; member_index < decl->as.type_decl.member_count; ++member_index) {
                        const FengTypeMember *member = decl->as.type_decl.members[member_index];
                        FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD
                                            ? member->as.field.name
                                            : member->as.callable.name;
                        if (slice_equals(name, label_slice)) {
                            return normalize_doc_comment(member->doc_comment);
                        }
                    }
                }
                if (decl->kind == FENG_DECL_FIT) {
                    for (member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                        const FengTypeMember *member = decl->as.fit_decl.members[member_index];
                        FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD
                                            ? member->as.field.name
                                            : member->as.callable.name;
                        if (slice_equals(name, label_slice)) {
                            return normalize_doc_comment(member->doc_comment);
                        }
                    }
                }
            }
        }
        if (session->analysis != NULL) {
            size_t mod_index;
            for (mod_index = 0U; mod_index < session->analysis->module_count; ++mod_index) {
                const FengSemanticModule *module = &session->analysis->modules[mod_index];
                size_t prog_index;
                for (prog_index = 0U; prog_index < module->program_count; ++prog_index) {
                    const FengProgram *p = module->programs[prog_index];
                    for (decl_index = 0U; decl_index < p->declaration_count; ++decl_index) {
                        const FengDecl *decl = p->declarations[decl_index];
                        if (decl->kind == FENG_DECL_TYPE &&
                            slice_equals(decl_name(decl), owner_slice)) {
                            for (member_index = 0U; member_index < decl->as.type_decl.member_count; ++member_index) {
                                const FengTypeMember *member = decl->as.type_decl.members[member_index];
                                FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD
                                                    ? member->as.field.name
                                                    : member->as.callable.name;
                                if (slice_equals(name, label_slice)) {
                                    return normalize_doc_comment(member->doc_comment);
                                }
                            }
                        }
                        if (decl->kind == FENG_DECL_FIT &&
                            (builtin_name_matches_type_ref(decl->as.fit_decl.target, owner_slice) ||
                             (decl->as.fit_decl.target != NULL &&
                              decl->as.fit_decl.target->kind == FENG_TYPE_REF_NAMED &&
                              decl->as.fit_decl.target->as.named.segment_count > 0U &&
                              slice_equals(decl->as.fit_decl.target->as.named.segments[
                                  decl->as.fit_decl.target->as.named.segment_count - 1U], owner_slice)))) {
                            for (member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                                const FengTypeMember *member = decl->as.fit_decl.members[member_index];
                                FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD
                                                    ? member->as.field.name
                                                    : member->as.callable.name;
                                if (slice_equals(name, label_slice)) {
                                    return normalize_doc_comment(member->doc_comment);
                                }
                            }
                        }
                    }
                }
            }
        }
        return NULL;
    }
    if (program != NULL) {
        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];
            if (slice_equals(decl_name(decl), label_slice)) {
                return normalize_doc_comment(decl->doc_comment);
            }
        }
    }
    return NULL;
}

/* Count commas at the current nesting level to determine activeParameter. */
static size_t count_commas_at_level(const char *text, size_t open_paren, size_t cursor) {
    size_t pos = open_paren + 1U;
    size_t commas = 0U;
    int depth = 0;

    while (pos < cursor) {
        char c = text[pos];

        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (c == ',' && depth == 0) {
            ++commas;
        } else if (c == '"') {
            ++pos;
            while (pos < cursor && text[pos] != '"') {
                if (text[pos] == '\\') {
                    ++pos;
                }
                ++pos;
            }
        }
        ++pos;
    }
    return commas;
}

/* Find the offset of the unmatched '(' that starts the current call context. */
static size_t find_open_paren(const char *text, size_t cursor) {
    int depth = 0;
    size_t pos = cursor;

    while (pos > 0U) {
        --pos;
        char c = text[pos];

        if (c == ')' || c == ']' || c == '}') {
            ++depth;
        } else if (c == '(' || c == '[' || c == '{') {
            if (depth == 0 && c == '(') {
                return pos;
            }
            --depth;
        } else if (c == '"') {
            if (pos > 0U) {
                --pos;
                while (pos > 0U && text[pos] != '"') {
                    if (pos > 0U && text[pos - 1U] == '\\') {
                        --pos;
                    }
                    --pos;
                }
            }
        }
    }
    return (size_t)-1;
}

/* Extract the callee identifier/member expression text before the '(' and
 * resolve it to find the method name and owner type.  Returns the method name
 * and optionally the owner type name via output parameters. */
static bool resolve_signature_callee(const char *text,
                                     size_t open_paren,
                                     char **out_method_name,
                                     char **out_owner_name) {
    size_t end;
    size_t start;
    size_t dot_pos;
    bool has_dot = false;

    if (text == NULL || out_method_name == NULL || out_owner_name == NULL) {
        return false;
    }
    *out_method_name = NULL;
    *out_owner_name = NULL;
    end = open_paren;
    while (end > 0U && isspace((unsigned char)text[end - 1U])) {
        --end;
    }
    if (end == 0U) {
        return false;
    }
    /* Scan back for identifier chars (method name). */
    start = end;
    while (start > 0U && (isalnum((unsigned char)text[start - 1U]) || text[start - 1U] == '_')) {
        --start;
    }
    if (start == end) {
        return false;
    }
    *out_method_name = dup_range(text + start, text + end);
    if (*out_method_name == NULL) {
        return false;
    }
    /* Check if there's a dot before the method name (member access). */
    dot_pos = start;
    while (dot_pos > 0U && isspace((unsigned char)text[dot_pos - 1U])) {
        --dot_pos;
    }
    if (dot_pos > 0U && text[dot_pos - 1U] == '.') {
        has_dot = true;
        --dot_pos;
        /* Scan back for the object identifier. */
        while (dot_pos > 0U && isspace((unsigned char)text[dot_pos - 1U])) {
            --dot_pos;
        }
        /* If it ends with ')' it's a complex expression — skip owner for now. */
        if (dot_pos > 0U && text[dot_pos - 1U] != ')') {
            size_t obj_end = dot_pos;
            size_t obj_start = obj_end;

            while (obj_start > 0U && (isalnum((unsigned char)text[obj_start - 1U]) || text[obj_start - 1U] == '_')) {
                --obj_start;
            }
            if (obj_start < obj_end) {
                *out_owner_name = dup_range(text + obj_start, text + obj_end);
            }
        }
    }
    (void)has_dot;
    return true;
}

/* Collect all overload signatures of a method for the SignatureHelp response. */
static bool build_signature_help_json(const FengLspCacheQueryContext *context,
                                      const char *method_name,
                                      const char *owner_name,
                                      const FengLspLocalList *locals,
                                      size_t active_param,
                                      FengLspString *json) {
    FengSlice method_slice = slice_from_cstr(method_name);
    const FengSymbolDeclView *owner_decl = NULL;
    size_t sig_count = 0U;
    size_t mod_count;
    size_t mod_idx;
    int active_signature = 0;

    if (context == NULL || context->provider == NULL || json == NULL) {
        return false;
    }
    if (!string_append_cstr(json, "{\"signatures\":[")) {
        return false;
    }
    /* Find the owner type. */
    if (owner_name != NULL) {
        FengSlice owner_slice = slice_from_cstr(owner_name);
        const FengLspLocal *local = find_local(locals, owner_slice);

        if (local != NULL) {
            if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
                owner_decl = local->binding->type != NULL
                    ? resolve_symbol_named_type_ref(context->provider, context->current_module,
                                                    context->program, local->binding->type)
                    : resolve_symbol_owner_decl_from_initializer_expr(context, local->binding->initializer);
            } else if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
                owner_decl = resolve_symbol_named_type_ref(context->provider, context->current_module,
                                                           context->program, local->parameter->type);
            }
        }
        if (owner_decl == NULL) {
            owner_decl = resolve_symbol_type_name(context->provider, context->current_module,
                                                   context->program, owner_slice);
        }
        if (owner_decl == NULL) {
            size_t mi;
            size_t mc = feng_symbol_provider_module_count(context->provider);

            for (mi = 0U; mi < mc && owner_decl == NULL; ++mi) {
                const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mi);

                owner_decl = find_symbol_module_decl_by_name(mod, owner_slice, false, true, true);
            }
        }
    }
    if (owner_decl == NULL && owner_name == NULL) {
        /* Free function call — search by method name as function name. */
        FengSlice func_slice = slice_from_cstr(method_name);
        const FengSymbolDeclView *func_decl = resolve_symbol_value_name(context->provider,
                                                                         context->current_module,
                                                                         context->program,
                                                                         func_slice);

        if (func_decl == NULL) {
            size_t mi;
            size_t mc = feng_symbol_provider_module_count(context->provider);

            for (mi = 0U; mi < mc && func_decl == NULL; ++mi) {
                const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mi);

                func_decl = find_symbol_module_decl_by_name(mod, func_slice, true, false, true);
            }
        }
        if (func_decl != NULL && feng_symbol_decl_kind(func_decl) == FENG_SYMBOL_DECL_KIND_FUNCTION) {
            FengLspString sig_label = {0};
            size_t param_count = feng_symbol_decl_param_count(func_decl);
            size_t i;

            if (!append_symbol_member_signature(&sig_label, func_decl)) {
                string_dispose(&sig_label);
                return false;
            }
            if (sig_count > 0U && !string_append_cstr(json, ",")) {
                string_dispose(&sig_label);
                return false;
            }
            if (!string_append_cstr(json, "{\"label\":") ||
                !string_append_json_string(json, sig_label.data) ||
                !string_append_cstr(json, ",\"parameters\":[")) {
                string_dispose(&sig_label);
                return false;
            }
            for (i = 0U; i < param_count; ++i) {
                FengSlice pname = feng_symbol_decl_param_name(func_decl, i);

                if (i > 0U && !string_append_cstr(json, ",")) {
                    string_dispose(&sig_label);
                    return false;
                }
                if (!string_append_cstr(json, "{\"label\":\"") ||
                    !string_append_bytes(json, pname.data, pname.length) ||
                    !string_append_cstr(json, "\"}")) {
                    string_dispose(&sig_label);
                    return false;
                }
            }
            if (!string_append_cstr(json, "]}")) {
                string_dispose(&sig_label);
                return false;
            }
            string_dispose(&sig_label);
            ++sig_count;
        }
    }
    /* Search type direct members + fit members for overloads. */
    if (owner_decl != NULL) {
        size_t member_count = feng_symbol_decl_member_count(owner_decl);
        size_t index;

        for (index = 0U; index < member_count; ++index) {
            const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner_decl, index);
            FengLspString sig_label = {0};
            size_t param_count;
            size_t i;

            if (!slice_equals(feng_symbol_decl_name(member), method_slice)) {
                continue;
            }
            if (!append_symbol_member_signature(&sig_label, member)) {
                string_dispose(&sig_label);
                return false;
            }
            param_count = feng_symbol_decl_param_count(member);
            if (sig_count > 0U && !string_append_cstr(json, ",")) {
                string_dispose(&sig_label);
                return false;
            }
            if (!string_append_cstr(json, "{\"label\":") ||
                !string_append_json_string(json, sig_label.data) ||
                !string_append_cstr(json, ",\"parameters\":[")) {
                string_dispose(&sig_label);
                return false;
            }
            for (i = 0U; i < param_count; ++i) {
                FengSlice pname = feng_symbol_decl_param_name(member, i);

                if (i > 0U && !string_append_cstr(json, ",")) {
                    string_dispose(&sig_label);
                    return false;
                }
                if (!string_append_cstr(json, "{\"label\":\"") ||
                    !string_append_bytes(json, pname.data, pname.length) ||
                    !string_append_cstr(json, "\"}")) {
                    string_dispose(&sig_label);
                    return false;
                }
            }
            if (!string_append_cstr(json, "]}")) {
                string_dispose(&sig_label);
                return false;
            }
            string_dispose(&sig_label);
            if (sig_count == 0U || param_count > active_param) {
                active_signature = (int)sig_count;
            }
            ++sig_count;
        }
        /* Also search fit members. */
        mod_count = feng_symbol_provider_module_count(context->provider);
        for (mod_idx = 0U; mod_idx < mod_count; ++mod_idx) {
            const FengSymbolImportedModule *mod = feng_symbol_provider_module_at(context->provider, mod_idx);
            size_t fit_count = feng_symbol_module_fit_count(mod);
            size_t fit_idx;
            FengSlice owner_name_slice = feng_symbol_decl_name(owner_decl);

            for (fit_idx = 0U; fit_idx < fit_count; ++fit_idx) {
                const FengSymbolFitView *fit = feng_symbol_module_fit_at(mod, fit_idx);
                const FengSymbolDeclView *fd = feng_symbol_fit_decl(fit);
                const FengSymbolTypeView *target;
                size_t fc;
                size_t fi;

                if (fd == NULL) {
                    continue;
                }
                target = feng_symbol_decl_fit_target(fd);
                if (target == NULL) {
                    continue;
                }
                if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_NAMED &&
                    feng_symbol_type_segment_count(target) >= 1U) {
                    FengSlice seg = feng_symbol_type_segment_at(target,
                                                                feng_symbol_type_segment_count(target) - 1U);
                    if (!slice_equals(seg, owner_name_slice)) {
                        continue;
                    }
                } else if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                    if (!slice_equals(feng_symbol_type_builtin_name(target), owner_name_slice)) {
                        continue;
                    }
                } else {
                    continue;
                }
                fc = feng_symbol_decl_member_count(fd);
                for (fi = 0U; fi < fc; ++fi) {
                    const FengSymbolDeclView *member = feng_symbol_decl_member_at(fd, fi);
                    FengLspString sig_label = {0};
                    size_t param_count;
                    size_t i;

                    if (!slice_equals(feng_symbol_decl_name(member), method_slice)) {
                        continue;
                    }
                    if (!append_symbol_member_signature(&sig_label, member)) {
                        string_dispose(&sig_label);
                        return false;
                    }
                    param_count = feng_symbol_decl_param_count(member);
                    if (sig_count > 0U && !string_append_cstr(json, ",")) {
                        string_dispose(&sig_label);
                        return false;
                    }
                    if (!string_append_cstr(json, "{\"label\":") ||
                        !string_append_json_string(json, sig_label.data) ||
                        !string_append_cstr(json, ",\"parameters\":[")) {
                        string_dispose(&sig_label);
                        return false;
                    }
                    for (i = 0U; i < param_count; ++i) {
                        FengSlice pname = feng_symbol_decl_param_name(member, i);

                        if (i > 0U && !string_append_cstr(json, ",")) {
                            string_dispose(&sig_label);
                            return false;
                        }
                        if (!string_append_cstr(json, "{\"label\":\"") ||
                            !string_append_bytes(json, pname.data, pname.length) ||
                            !string_append_cstr(json, "\"}")) {
                            string_dispose(&sig_label);
                            return false;
                        }
                    }
                    if (!string_append_cstr(json, "]}")) {
                        string_dispose(&sig_label);
                        return false;
                    }
                    string_dispose(&sig_label);
                    if (active_signature == 0 && param_count > active_param) {
                        active_signature = (int)sig_count;
                    }
                    ++sig_count;
                }
            }
        }
    }
    if (!string_append_cstr(json, "],\"activeSignature\":") ||
        !string_append_format(json, "%d", active_signature) ||
        !string_append_cstr(json, ",\"activeParameter\":") ||
        !string_append_format(json, "%zu", active_param) ||
        !string_append_cstr(json, "}")) {
        return false;
    }
    return sig_count > 0U;
}

static bool handle_signature_help_request(FengLspRuntime *runtime,
                                          FILE *output,
                                          FengLspJsonValue id,
                                          FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    char *uri;
    unsigned int line;
    unsigned int character;
    FengLspDocument *document;
    size_t offset;
    size_t open_paren;
    size_t active_param;
    char *method_name = NULL;
    char *owner_name_str = NULL;
    FengLspString json = {0};
    bool ok = false;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    if (uri == NULL || !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL || document->text == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = offset_from_position(document->text, line, character);
    open_paren = find_open_paren(document->text, offset);
    if (open_paren == (size_t)-1) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    active_param = count_commas_at_level(document->text, open_paren, offset);
    if (!resolve_signature_callee(document->text, open_paren, &method_name, &owner_name_str)) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    {
        FengLspCacheQueryContext cache = {0};
        FengLspLocalList locals = {0};

        if (build_cache_query_context(document, &cache)) {
            ok = build_signature_help_json(&cache, method_name, owner_name_str, &locals, active_param, &json);
            cache_query_context_dispose(&cache);
        }
        if (!ok) {
            string_dispose(&json);
            if (build_cache_query_context_for_text(document, document->text, false, &cache)) {
                const FengDecl *enclosing_decl;
                const FengTypeMember *enclosing_member;

                enclosing_decl = find_enclosing_decl_for_completion(cache.source_text, cache.program,
                                                                     offset, &enclosing_member);
                if (enclosing_decl != NULL) {
                    (void)collect_visible_locals_for_completion(cache.source_text, enclosing_decl,
                                                                enclosing_member, offset, &locals);
                }
                ok = build_signature_help_json(&cache, method_name, owner_name_str, &locals, active_param, &json);
                local_list_dispose(&locals);
                cache_query_context_dispose(&cache);
            }
        }
        if (!ok) {
            string_dispose(&json);
            /* Provider-only fallback for bundle types. */
            {
                char *manifest_path = NULL;
                FengCliProjectContext project = {0};
                FengCliProjectError project_error = {0};
                FengCliDepsResolved resolved = {0};
                FengSymbolProvider *provider = NULL;
                FengSymbolError symbol_error = {0};

                if (feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &project_error) &&
                    feng_cli_project_open(manifest_path, &project, &project_error) &&
                    feng_symbol_provider_create(&provider, &symbol_error)) {
                    size_t dep_idx;

                    if (feng_cli_deps_resolve_for_manifest("feng", project.manifest_path,
                                                           false, false, &resolved, &project_error)) {
                        for (dep_idx = 0U; dep_idx < resolved.package_count; ++dep_idx) {
                            (void)feng_symbol_provider_add_bundle(provider,
                                                                  resolved.package_paths[dep_idx],
                                                                  &symbol_error);
                        }
                    }
                    {
                        FengLspCacheQueryContext minimal = {0};

                        minimal.provider = provider;
                        ok = build_signature_help_json(&minimal, method_name, owner_name_str,
                                                       &locals, active_param, &json);
                        minimal.provider = NULL;
                    }
                }
                feng_symbol_provider_free(provider);
                feng_symbol_error_free(&symbol_error);
                feng_cli_deps_resolved_dispose(&resolved);
                feng_cli_project_context_dispose(&project);
                feng_cli_project_error_dispose(&project_error);
                free(manifest_path);
            }
        }
    }
    free(method_name);
    free(owner_name_str);
    free(uri);
    if (ok) {
        bool result = send_json_response(output, id, json.data);

        string_dispose(&json);
        return result;
    }
    string_dispose(&json);
    return send_json_response(output, id, "null");
}

static bool handle_completion_resolve_request(FengLspRuntime *runtime,
                                              FILE *output,
                                              FengLspJsonValue id,
                                              FengLspJsonValue params) {
    FengLspJsonValue data_value = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue label_value = {0};
    FengLspJsonValue owner_value = {0};
    char *uri = NULL;
    char *label = NULL;
    char *owner_name = NULL;
    char *doc = NULL;
    FengLspDocument *document;
    FengLspString response = {0};
    bool ok;

    /* Echo the original item unchanged if we cannot resolve documentation. */
    if (!json_object_get(params, "data", &data_value)) {
        char *echo = dup_range(params.start, params.end);
        if (echo == NULL) {
            return send_error_response(output, id, -32603, "out of memory");
        }
        ok = send_json_response(output, id, echo);
        free(echo);
        return ok;
    }
    (void)json_object_get(data_value, "uri", &uri_value);
    (void)json_object_get(data_value, "label", &label_value);
    (void)json_object_get(data_value, "owner", &owner_value);
    uri = json_string_dup(uri_value);
    label = json_string_dup(label_value);
    if (owner_value.start != NULL) {
        owner_name = json_string_dup(owner_value);
    }
    if (uri == NULL || label == NULL) {
        free(uri);
        free(label);
        free(owner_name);
        ok = string_append_bytes(&response, params.start, (size_t)(params.end - params.start));
        if (!ok) {
            string_dispose(&response);
            return send_error_response(output, id, -32603, "out of memory");
        }
        ok = send_json_response(output, id, response.data);
        string_dispose(&response);
        return ok;
    }

    document = find_document(runtime, uri);
    if (document != NULL) {
        FengLspCacheQueryContext cache = {0};
        if (build_cache_query_context(document, &cache)) {
            doc = resolve_doc_from_cache(&cache, label, owner_name);
            cache_query_context_dispose(&cache);
        }
        if (doc == NULL) {
            if (build_cache_query_context_for_text(document, document->text, false, &cache)) {
                doc = resolve_doc_from_cache(&cache, label, owner_name);
                cache_query_context_dispose(&cache);
            }
        }
        if (doc == NULL) {
            FengLspAnalysisSession session = {0};
            if (build_analysis_session(runtime, document, &session)) {
                const FengProgram *program = find_program(&session, document->path);
                doc = resolve_doc_from_session(&session, program, label, owner_name);
                session_dispose(&session);
            }
        }
        if (doc == NULL && owner_name != NULL) {
            char *manifest_path = NULL;
            FengCliProjectContext project = {0};
            FengCliProjectError project_error = {0};
            FengCliDepsResolved resolved = {0};
            FengSymbolProvider *provider = NULL;
            FengSymbolError symbol_error = {0};

            if (feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &project_error) &&
                feng_cli_project_open(manifest_path, &project, &project_error) &&
                feng_symbol_provider_create(&provider, &symbol_error)) {
                size_t dep_idx;

                if (feng_cli_deps_resolve_for_manifest("feng", project.manifest_path,
                                                       false, false, &resolved, &project_error)) {
                    for (dep_idx = 0U; dep_idx < resolved.package_count; ++dep_idx) {
                        (void)feng_symbol_provider_add_bundle(provider,
                                                              resolved.package_paths[dep_idx],
                                                              &symbol_error);
                    }
                }
                {
                    FengLspCacheQueryContext minimal_cache = {0};

                    minimal_cache.provider = provider;
                    minimal_cache.current_module = NULL;
                    minimal_cache.program = NULL;
                    minimal_cache.source_text = NULL;
                    doc = resolve_doc_from_cache(&minimal_cache, label, owner_name);
                    minimal_cache.provider = NULL;
                }
            }
            feng_symbol_provider_free(provider);
            feng_symbol_error_free(&symbol_error);
            feng_cli_deps_resolved_dispose(&resolved);
            feng_cli_project_context_dispose(&project);
            feng_cli_project_error_dispose(&project_error);
            free(manifest_path);
        }
    }

    /* Reconstruct the CompletionItem with the documentation field added. */
    if (!string_append_bytes(&response, params.start, (size_t)(params.end - params.start))) {
        free(uri);
        free(label);
        free(owner_name);
        free(doc);
        string_dispose(&response);
        return send_error_response(output, id, -32603, "out of memory");
    }
    if (doc != NULL && doc[0] != '\0') {
        /* Insert documentation before the closing '}'. */
        if (response.length > 0U && response.data[response.length - 1U] == '}') {
            response.data[response.length - 1U] = '\0';
            --response.length;
        }
        ok = string_append_cstr(&response, ",\"documentation\":{\"kind\":\"markdown\",\"value\":") &&
             string_append_json_string(&response, doc) &&
             string_append_cstr(&response, "}}");
        if (!ok) {
            free(uri);
            free(label);
            free(owner_name);
            free(doc);
            string_dispose(&response);
            return send_error_response(output, id, -32603, "out of memory");
        }
    }
    free(doc);
    free(uri);
    free(label);
    free(owner_name);
    ok = send_json_response(output, id, response.data);
    string_dispose(&response);
    return ok;
}

static bool handle_references_request(FengLspRuntime *runtime,
                                      FILE *output,
                                      FengLspJsonValue id,
                                      FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    FengLspJsonValue context = {0};
    FengLspJsonValue include_decl_value = {0};
    char *uri;
    unsigned int line;
    unsigned int character;
    bool include_declaration = false;
    FengLspDocument *document;
    FengLspAnalysisSession session = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspReferenceList references = {0};
    FengLspString json = {0};
    bool ok;
    size_t offset;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    if (json_object_get(params, "context", &context) &&
        json_object_get(context, "includeDeclaration", &include_decl_value) &&
        !json_bool(include_decl_value, &include_declaration)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    if (uri == NULL || !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "[]");
    }
    offset = offset_from_position(document->text, line, character);
    if (!build_analysis_session(runtime, document, &session)) {
        if (runtime->errors != NULL) {
            fprintf(runtime->errors, "lsp: textDocument/references: out of memory building analysis session\n");
        }
        free(uri);
        session_dispose(&session);
        return send_json_response(output, id, "[]");
    }
    program = find_program(&session, document->path);
    if (program == NULL || !resolve_target_at(&session, program, offset, &target) ||
        !collect_references(&session, include_declaration, &target, &references) ||
        !build_references_json(&session, &references, &json)) {
        free(uri);
        reference_list_dispose(&references);
        session_dispose(&session);
        string_dispose(&json);
        return send_json_response(output, id, "[]");
    }
    free(uri);
    reference_list_dispose(&references);
    session_dispose(&session);
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
    return ok;
}

static bool handle_prepare_rename_request(FengLspRuntime *runtime,
                                          FILE *output,
                                          FengLspJsonValue id,
                                          FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    char *uri;
    unsigned int line;
    unsigned int character;
    FengLspDocument *document;
    FengLspAnalysisSession session = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspReferenceList references = {0};
    const FengLspReferenceEntry *entry;
    const FengCliLoadedSource *source;
    FengLspString json = {0};
    bool ok;
    size_t offset;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    if (uri == NULL || !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = offset_from_position(document->text, line, character);
    if (!build_analysis_session(runtime, document, &session)) {
        if (runtime->errors != NULL) {
            fprintf(runtime->errors, "lsp: textDocument/prepareRename: out of memory building analysis session\n");
        }
        free(uri);
        session_dispose(&session);
        return send_json_response(output, id, "null");
    }
    program = find_program(&session, document->path);
    if (program == NULL ||
        !resolve_target_at(&session, program, offset, &target) ||
        !resolved_target_can_rename(&session, &target) ||
        !collect_references(&session, true, &target, &references)) {
        free(uri);
        reference_list_dispose(&references);
        session_dispose(&session);
        return send_json_response(output, id, "null");
    }
    entry = reference_list_find_offset(&references, document->path, offset);
    source = find_reference_source(&session, entry);
    if (entry == NULL || source == NULL || !build_prepare_rename_json(source, entry, &json)) {
        if (runtime->errors != NULL && (entry == NULL || source == NULL)) {
            /* entry==NULL: target not found at offset; not an error, just no rename candidate */
        } else if (runtime->errors != NULL) {
            fprintf(runtime->errors, "lsp: textDocument/prepareRename: out of memory building response\n");
        }
        free(uri);
        reference_list_dispose(&references);
        session_dispose(&session);
        string_dispose(&json);
        return send_json_response(output, id, "null");
    }
    free(uri);
    reference_list_dispose(&references);
    session_dispose(&session);
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
    return ok;
}

static bool handle_rename_request(FengLspRuntime *runtime,
                                  FILE *output,
                                  FengLspJsonValue id,
                                  FengLspJsonValue params) {
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspJsonValue position = {0};
    FengLspJsonValue line_value = {0};
    FengLspJsonValue char_value = {0};
    FengLspJsonValue new_name_value = {0};
    char *uri;
    char *new_name;
    unsigned int line;
    unsigned int character;
    FengLspDocument *document;
    FengLspAnalysisSession session = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspReferenceList references = {0};
    FengLspString json = {0};
    bool ok;
    size_t offset;

    if (!json_object_get(params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value) ||
        !json_object_get(params, "position", &position) ||
        !json_object_get(position, "line", &line_value) ||
        !json_object_get(position, "character", &char_value) ||
        !json_object_get(params, "newName", &new_name_value)) {
        return send_error_response(output, id, -32602, "Invalid params");
    }
    uri = json_string_dup(uri_value);
    new_name = json_string_dup(new_name_value);
    if (uri == NULL || new_name == NULL ||
        !json_u32(line_value, &line) || !json_u32(char_value, &character)) {
        free(new_name);
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    if (!identifier_name_is_valid(new_name)) {
        free(new_name);
        free(uri);
        return send_error_response(output, id, -32602, "Invalid params");
    }
    document = find_document(runtime, uri);
    if (document == NULL) {
        free(new_name);
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = offset_from_position(document->text, line, character);
    if (!build_analysis_session(runtime, document, &session)) {
        if (runtime->errors != NULL) {
            fprintf(runtime->errors, "lsp: textDocument/rename: out of memory building analysis session\n");
        }
        free(new_name);
        free(uri);
        session_dispose(&session);
        return send_json_response(output, id, "null");
    }
    program = find_program(&session, document->path);
    if (program == NULL ||
        !resolve_target_at(&session, program, offset, &target) ||
        !resolved_target_can_rename(&session, &target) ||
        !collect_references(&session, true, &target, &references) ||
        reference_list_find_offset(&references, document->path, offset) == NULL ||
        !build_rename_json(&session, &references, new_name, &json)) {
        free(new_name);
        free(uri);
        reference_list_dispose(&references);
        session_dispose(&session);
        string_dispose(&json);
        return send_json_response(output, id, "null");
    }
    free(new_name);
    free(uri);
    reference_list_dispose(&references);
    session_dispose(&session);
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
    return ok;
}


FengLspRuntime *feng_lsp_runtime_create(void) {
    return (FengLspRuntime *)calloc(1U, sizeof(FengLspRuntime));
}

void feng_lsp_runtime_free(FengLspRuntime *runtime) {
    size_t index;

    if (runtime == NULL) {
        return;
    }
    for (index = 0U; index < runtime->document_count; ++index) {
        free(runtime->documents[index].uri);
        free(runtime->documents[index].path);
        free(runtime->documents[index].text);
    }
    free(runtime->documents);
    free(runtime);
}

static bool handle_initialize(FengLspRuntime *runtime,
                              FILE *output,
                              FengLspJsonValue id,
                              FengLspJsonValue params) {
    if (runtime != NULL) {
        runtime->hover_markup_kind = hover_markup_kind_from_initialize_params(params);
    }
    return send_json_response(output,
                              id,
                              "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":false}},\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"renameProvider\":{\"prepareProvider\":true},\"completionProvider\":{\"resolveProvider\":true,\"triggerCharacters\":[\".\",\"_\",\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\",\"r\",\"s\",\"t\",\"u\",\"v\",\"w\",\"x\",\"y\",\"z\",\"A\",\"B\",\"C\",\"D\",\"E\",\"F\",\"G\",\"H\",\"I\",\"J\",\"K\",\"L\",\"M\",\"N\",\"O\",\"P\",\"Q\",\"R\",\"S\",\"T\",\"U\",\"V\",\"W\",\"X\",\"Y\",\"Z\"]},\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]}},\"serverInfo\":{\"name\":\"feng\"}}");
}

bool feng_lsp_runtime_handle_payload(FengLspRuntime *runtime,
                                     FILE *output,
                                     const char *payload,
                                     size_t payload_length,
                                     FILE *errors) {
    FengLspMessage message = {0};
    FengLspParseStatus status = parse_jsonrpc_message(payload, payload_length, &message);

    runtime->errors = errors;
    static const char kNullJson[] = "null";
    FengLspJsonValue null_id = {
        .type = FENG_LSP_JSON_NULL,
        .start = kNullJson,
        .end = &kNullJson[4],
        .value_start = kNullJson,
        .value_end = &kNullJson[4]
    };
    bool ok = true;

    if (status == FENG_LSP_PARSE_INVALID_JSON) {
        return send_error_response(output, null_id, -32700, "Parse error");
    }
    if (status == FENG_LSP_PARSE_INVALID_REQUEST) {
        return send_error_response(output, null_id, -32600, "Invalid Request");
    }

    if (strcmp(message.method, "initialize") == 0) {
        ok = message.has_id ? handle_initialize(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "shutdown") == 0) {
        runtime->shutdown_requested = true;
        ok = message.has_id ? send_json_response(output, message.id, "null")
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "exit") == 0) {
        runtime->should_exit = true;
        runtime->exit_code = runtime->shutdown_requested ? 0 : 1;
    } else if (strcmp(message.method, "initialized") == 0 ||
               strcmp(message.method, "$/cancelRequest") == 0 ||
               strcmp(message.method, "$/setTrace") == 0) {
        ok = true;
    } else if (strcmp(message.method, "textDocument/didOpen") == 0) {
        FengLspJsonValue text_document = {0};
        FengLspJsonValue uri_value = {0};
        FengLspJsonValue text_value = {0};
        char *uri;
        char *text;

        if (!json_object_get(message.params, "textDocument", &text_document) ||
            !json_object_get(text_document, "uri", &uri_value) ||
            !json_object_get(text_document, "text", &text_value)) {
            fprintf(errors, "lsp: textDocument/didOpen: missing required params\n");
            /* Malformed notification from client — log and continue; do not kill server */
        } else {
            uri = json_string_dup(uri_value);
            text = json_string_dup(text_value);
            if (uri == NULL) {
                fprintf(errors, "lsp: textDocument/didOpen: failed to decode URI\n");
            } else if (text == NULL) {
                fprintf(errors, "lsp: textDocument/didOpen: failed to decode text for '%s'\n", uri);
            } else if (!upsert_document(runtime, uri, text)) {
                /* upsert_document already logged the OOM; document not tracked but server continues */
                fprintf(errors, "lsp: textDocument/didOpen: document not tracked: '%s'\n", uri);
            } else {
                ok = refresh_diagnostics(runtime, output, uri); /* I/O failure — propagate */
            }
            free(uri);
            free(text);
        }
    } else if (strcmp(message.method, "textDocument/didChange") == 0) {
        FengLspJsonValue text_document = {0};
        FengLspJsonValue uri_value = {0};
        FengLspJsonValue changes = {0};
        FengLspJsonValue first_change = {0};
        FengLspJsonValue text_value = {0};
        char *uri;
        char *text;

        if (!json_object_get(message.params, "textDocument", &text_document) ||
            !json_object_get(text_document, "uri", &uri_value) ||
            !json_object_get(message.params, "contentChanges", &changes) ||
            !json_array_get(changes, 0U, &first_change) ||
            !json_object_get(first_change, "text", &text_value)) {
            fprintf(errors, "lsp: textDocument/didChange: missing required params\n");
            /* Malformed notification from client — log and continue; do not kill server */
        } else {
            uri = json_string_dup(uri_value);
            text = json_string_dup(text_value);
            if (uri == NULL) {
                fprintf(errors, "lsp: textDocument/didChange: failed to decode URI\n");
            } else if (text == NULL) {
                fprintf(errors, "lsp: textDocument/didChange: failed to decode text for '%s'\n", uri);
            } else if (!upsert_document(runtime, uri, text)) {
                /* upsert_document already logged the OOM; document not tracked but server continues */
                fprintf(errors, "lsp: textDocument/didChange: document not tracked: '%s'\n", uri);
            } else {
                ok = refresh_diagnostics(runtime, output, uri); /* I/O failure — propagate */
            }
            free(uri);
            free(text);
        }
    } else if (strcmp(message.method, "textDocument/didSave") == 0) {
        FengLspJsonValue text_document = {0};
        FengLspJsonValue uri_value = {0};
        char *uri;

        if (!json_object_get(message.params, "textDocument", &text_document) ||
            !json_object_get(text_document, "uri", &uri_value)) {
            fprintf(errors, "lsp: textDocument/didSave: missing required params\n");
            /* Malformed notification from client — log and continue; do not kill server */
        } else {
            uri = json_string_dup(uri_value);
            if (uri == NULL) {
                fprintf(errors, "lsp: textDocument/didSave: failed to decode URI\n");
            } else {
                ok = refresh_diagnostics(runtime, output, uri); /* I/O failure — propagate */
            }
            free(uri);
        }
    } else if (strcmp(message.method, "textDocument/didClose") == 0) {
        FengLspJsonValue text_document = {0};
        FengLspJsonValue uri_value = {0};
        char *uri;
        FengLspDocument *document;

        if (!json_object_get(message.params, "textDocument", &text_document) ||
            !json_object_get(text_document, "uri", &uri_value)) {
            fprintf(errors, "lsp: textDocument/didClose: missing required params\n");
            /* Malformed notification from client — log and continue; do not kill server */
        } else {
            uri = json_string_dup(uri_value);
            if (uri == NULL) {
                fprintf(errors, "lsp: textDocument/didClose: failed to decode URI\n");
            } else {
                document = find_document(runtime, uri);
                ok = document == NULL || publish_empty_diagnostics(output, document);
                if (!ok) {
                    fprintf(errors,
                            "lsp: textDocument/didClose: failed to clear diagnostics for '%s'\n",
                            uri);
                } else {
                    remove_document(runtime, uri);
                }
            }
            free(uri);
        }
    } else if (strcmp(message.method, "textDocument/hover") == 0) {
        ok = message.has_id ? handle_hover_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/definition") == 0) {
        ok = message.has_id ? handle_definition_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/references") == 0) {
        ok = message.has_id ? handle_references_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/prepareRename") == 0) {
        ok = message.has_id ? handle_prepare_rename_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/rename") == 0) {
        ok = message.has_id ? handle_rename_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/completion") == 0) {
        ok = message.has_id ? handle_completion_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "completionItem/resolve") == 0) {
        ok = message.has_id ? handle_completion_resolve_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/signatureHelp") == 0) {
        ok = message.has_id ? handle_signature_help_request(runtime, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (message.has_id) {
        ok = send_error_response(output, message.id, -32601, "Method not found");
    }

    if (!ok) {
        fprintf(errors, "lsp protocol error: failed to handle %s\n", message.method);
    }
    message_dispose(&message);
    return ok;
}

bool feng_lsp_runtime_should_exit(const FengLspRuntime *runtime) {
    return runtime != NULL && runtime->should_exit;
}

int feng_lsp_runtime_exit_code(const FengLspRuntime *runtime) {
    return runtime != NULL ? runtime->exit_code : 1;
}
