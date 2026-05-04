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
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/provider.h"

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

struct FengLspRuntime {
    FengLspDocument *documents;
    size_t document_count;
    size_t document_capacity;
    bool shutdown_requested;
    bool should_exit;
    int exit_code;
    FILE *errors; /* diagnostics log; set at the start of each handle_payload call */
};

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
        overlay.source = document->text;
        overlay.source_length = strlen(document->text);
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

static bool build_cache_query_context(const FengLspDocument *document,
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
    if (document == NULL || document->text == NULL || !document_matches_disk(document)) {
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
    if (symbols_root == NULL || !path_is_directory(symbols_root)) {
        goto cleanup;
    }
    if (!feng_parse_source(document->text,
                           strlen(document->text),
                           document->path,
                           &context->program,
                           &parse_error)) {
        goto cleanup;
    }
    if (!feng_symbol_provider_create(&context->provider, &symbol_error) ||
        !feng_symbol_provider_add_ft_root(context->provider,
                                          symbols_root,
                                          FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                                          &symbol_error)) {
        goto cleanup;
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
    ok = context->current_module != NULL;

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
        case FENG_STMT_TRY:
            if (stmt->as.try_stmt.try_block != NULL) {
                size_t try_end = block_end(stmt->as.try_stmt.try_block);
                if (try_end > end) {
                    end = try_end;
                }
            }
            if (stmt->as.try_stmt.catch_block != NULL) {
                size_t catch_end = block_end(stmt->as.try_stmt.catch_block);
                if (catch_end > end) {
                    end = catch_end;
                }
            }
            if (stmt->as.try_stmt.finally_block != NULL) {
                size_t finally_end = block_end(stmt->as.try_stmt.finally_block);
                if (finally_end > end) {
                    end = finally_end;
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

static const FengCliLoadedSource *find_source(const FengLspAnalysisSession *session,
                                              const char *path) {
    return feng_cli_find_loaded_source(session->sources, session->source_count, path);
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
        case FENG_STMT_TRY:
            if (stmt->as.try_stmt.try_block != NULL && offset <= block_end(stmt->as.try_stmt.try_block)) {
                return collect_block_locals(stmt->as.try_stmt.try_block, offset, locals);
            }
            if (stmt->as.try_stmt.catch_block != NULL && offset <= block_end(stmt->as.try_stmt.catch_block)) {
                return collect_block_locals(stmt->as.try_stmt.catch_block, offset, locals);
            }
            return stmt->as.try_stmt.finally_block != NULL
                       ? collect_block_locals(stmt->as.try_stmt.finally_block, offset, locals)
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

static FengSlice decl_name(const FengDecl *decl) {
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return decl->as.binding.name;
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
            bool is_type = decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_SPEC;

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

static bool symbol_decl_is_value(const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind = feng_symbol_decl_kind(decl);

    return kind == FENG_SYMBOL_DECL_KIND_BINDING || kind == FENG_SYMBOL_DECL_KIND_FUNCTION;
}

static bool symbol_decl_is_type(const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind = feng_symbol_decl_kind(decl);

    return kind == FENG_SYMBOL_DECL_KIND_TYPE || kind == FENG_SYMBOL_DECL_KIND_SPEC;
}

static bool symbol_decl_matches_ast_decl_kind(const FengSymbolDeclView *decl,
                                              const FengDecl *ast_decl) {
    if (decl == NULL || ast_decl == NULL) {
        return false;
    }
    switch (ast_decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_BINDING;
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
    if (public_only) {
        return feng_symbol_decl_find_public_member(owner, name);
    }
    count = feng_symbol_decl_member_count(owner);
    for (index = 0U; index < count; ++index) {
        const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner, index);

        if (member != NULL && slice_equals(feng_symbol_decl_name(member), name)) {
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

static const FengSymbolDeclView *resolve_symbol_owner_decl_from_object_expr(const FengLspCacheQueryContext *context,
                                                                            const FengExpr *object,
                                                                            const FengLspLocalList *locals) {
    const FengSymbolDeclView *decl;

    if (context == NULL || object == NULL) {
        return NULL;
    }
    if (object->kind == FENG_EXPR_SELF) {
        const FengLspLocal *self_local = find_local(locals, slice_from_cstr("self"));
        return self_local != NULL
                   ? match_ast_decl_to_symbol(context->current_module,
                                              context->program,
                                              self_local->self_owner_decl)
                   : NULL;
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
                return resolve_symbol_named_type_ref(context->provider,
                                                     context->current_module,
                                                     context->program,
                                                     local->binding->type);
            }
            if (local->kind == FENG_LSP_LOCAL_SELF) {
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
            feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_SPEC) {
            return decl;
        }
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
    }
    return find_module_decl_by_name(find_module_by_segments(session->analysis,
                                                            type_ref->as.named.segments,
                                                            type_ref->as.named.segment_count - 1U),
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
        return self_local != NULL ? self_local->self_owner_decl : NULL;
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
                return resolve_named_type_ref(session, program, local->binding->type);
            }
            if (local->kind == FENG_LSP_LOCAL_SELF) {
                return local->self_owner_decl;
            }
        }
    }
    decl = resolve_value_name(session, program, object->as.identifier);
    if (decl != NULL) {
        if (decl->kind == FENG_DECL_GLOBAL_BINDING) {
            return resolve_named_type_ref(session, program, decl->as.binding.type);
        }
        if (decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_SPEC) {
            return decl;
        }
    }
    return NULL;
}

static bool find_decl_token_hit(const FengDecl *decl,
                                size_t offset,
                                FengLspResolvedTarget *target);
static bool find_type_ref_hit(const FengDecl *decl,
                              const FengProgram *program,
                              const FengLspAnalysisSession *session,
                              size_t offset,
                              FengLspResolvedTarget *target);
static const FengExpr *find_expr_hit(const FengExpr *expr, size_t offset);

static bool find_decl_token_hit_member(const FengDecl *owner_decl,
                                       const FengTypeMember *member,
                                       size_t offset,
                                       FengLspResolvedTarget *target) {
    size_t index;

    if (offset_in_token(member->token, offset)) {
        target->kind = FENG_LSP_RESOLVED_MEMBER;
        target->decl = owner_decl;
        target->member = member;
        return true;
    }
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return false;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (offset_in_token(member->as.callable.params[index].token, offset)) {
            target->kind = FENG_LSP_RESOLVED_PARAM;
            target->parameter = &member->as.callable.params[index];
            return true;
        }
    }
    return false;
}

static bool find_decl_token_hit(const FengDecl *decl,
                                size_t offset,
                                FengLspResolvedTarget *target) {
    size_t index;

    if (offset_in_token(decl->token, offset)) {
        target->kind = FENG_LSP_RESOLVED_DECL;
        target->decl = decl;
        return true;
    }
    switch (decl->kind) {
        case FENG_DECL_FUNCTION:
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (offset_in_token(decl->as.function_decl.params[index].token, offset)) {
                    target->kind = FENG_LSP_RESOLVED_PARAM;
                    target->parameter = &decl->as.function_decl.params[index];
                    return true;
                }
            }
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (find_decl_token_hit_member(decl,
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
                    if (find_decl_token_hit_member(decl,
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
                if (find_decl_token_hit_member(decl,
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
        case FENG_STMT_TRY:
            if (find_block_type_ref_hit(stmt->as.try_stmt.try_block,
                                        program,
                                        session,
                                        offset,
                                        target)) {
                return true;
            }
            if (find_block_type_ref_hit(stmt->as.try_stmt.catch_block,
                                        program,
                                        session,
                                        offset,
                                        target)) {
                return true;
            }
            return find_block_type_ref_hit(stmt->as.try_stmt.finally_block,
                                           program,
                                           session,
                                           offset,
                                           target);
        case FENG_STMT_ASSIGN:
        case FENG_STMT_EXPR:
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
            break;
        }
        case FENG_EXPR_MATCH:
            return find_expr_hit(expr->as.match_expr.target, offset);
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
                    hit = find_expr_hit_in_block(stmt->as.for_stmt.body, offset);
                    if (hit == NULL) {
                        hit = find_expr_hit(stmt->as.for_stmt.condition, offset);
                    }
                }
                break;
            case FENG_STMT_TRY:
                hit = find_expr_hit_in_block(stmt->as.try_stmt.try_block, offset);
                if (hit == NULL) {
                    hit = find_expr_hit_in_block(stmt->as.try_stmt.catch_block, offset);
                }
                if (hit == NULL) {
                    hit = find_expr_hit_in_block(stmt->as.try_stmt.finally_block, offset);
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
            return true;
        case FENG_TYPE_REF_POINTER:
            return string_append_cstr(buffer, "*") && type_ref_to_string(buffer, type_ref->as.inner);
        case FENG_TYPE_REF_ARRAY:
            return type_ref_to_string(buffer, type_ref->as.inner) &&
                   string_append_cstr(buffer, type_ref->array_element_writable ? "[]!" : "[]");
    }
    return false;
}

static bool decl_signature_to_string(FengLspString *buffer, const FengDecl *decl) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return string_append_cstr(buffer,
                                      decl->as.binding.mutability == FENG_MUTABILITY_VAR ? "var " : "let ") &&
                   string_append_bytes(buffer,
                                       decl->as.binding.name.data,
                                       decl->as.binding.name.length) &&
                   string_append_cstr(buffer, ": ") &&
                   type_ref_to_string(buffer, decl->as.binding.type);
        case FENG_DECL_TYPE:
            return string_append_cstr(buffer, "type ") &&
                   string_append_bytes(buffer, decl->as.type_decl.name.data, decl->as.type_decl.name.length);
        case FENG_DECL_SPEC:
            return string_append_cstr(buffer, "spec ") &&
                   string_append_bytes(buffer, decl->as.spec_decl.name.data, decl->as.spec_decl.name.length);
        case FENG_DECL_FIT:
            return string_append_cstr(buffer, "fit");
        case FENG_DECL_FUNCTION:
            if (!string_append_cstr(buffer, "fn ") ||
                !string_append_bytes(buffer,
                                     decl->as.function_decl.name.data,
                                     decl->as.function_decl.name.length) ||
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
                    !type_ref_to_string(buffer, decl->as.function_decl.params[index].type)) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   type_ref_to_string(buffer, decl->as.function_decl.return_type);
    }
    return false;
}

static bool member_signature_to_string(FengLspString *buffer, const FengTypeMember *member) {
    size_t index;

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return string_append_cstr(buffer,
                                  member->as.field.mutability == FENG_MUTABILITY_VAR ? "var " : "let ") &&
               string_append_bytes(buffer,
                                   member->as.field.name.data,
                                   member->as.field.name.length) &&
               string_append_cstr(buffer, ": ") &&
               type_ref_to_string(buffer, member->as.field.type);
    }
    if (!string_append_cstr(buffer,
                            member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR ? "ctor " :
                            member->kind == FENG_TYPE_MEMBER_FINALIZER ? "finalizer " : "fn ") ||
        !string_append_bytes(buffer,
                             member->as.callable.name.data,
                             member->as.callable.name.length) ||
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
            !type_ref_to_string(buffer, member->as.callable.params[index].type)) {
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

    if (raw.data == NULL || raw.length < 5U || strncmp(raw.data, "/**", 3U) != 0) {
        return NULL;
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

static char *hover_text_for_target(const FengLspResolvedTarget *target) {
    FengLspString signature = {0};
    char *doc = NULL;
    char *result;

    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            if (!decl_signature_to_string(&signature, target->decl)) {
                string_dispose(&signature);
                return NULL;
            }
            doc = normalize_doc_comment(target->decl->doc_comment);
            break;
        case FENG_LSP_RESOLVED_MEMBER:
            if (!member_signature_to_string(&signature, target->member)) {
                string_dispose(&signature);
                return NULL;
            }
            doc = normalize_doc_comment(target->member->doc_comment);
            break;
        case FENG_LSP_RESOLVED_PARAM:
            if (!string_append_cstr(&signature, "param ") ||
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
                                     target->binding->name.length) ||
                !string_append_cstr(&signature, ": ") ||
                !type_ref_to_string(&signature, target->binding->type)) {
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
                                            ? "[]!"
                                            : "[]")) {
                    return false;
                }
            }
            return true;
        case FENG_SYMBOL_TYPE_KIND_INVALID:
            return false;
    }
    return false;
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
        case FENG_SYMBOL_DECL_KIND_TYPE:
            return string_append_cstr(buffer, "type ") &&
                   string_append_bytes(buffer, name.data, name.length);
        case FENG_SYMBOL_DECL_KIND_SPEC:
            return string_append_cstr(buffer, "spec ") &&
                   string_append_bytes(buffer, name.data, name.length);
        case FENG_SYMBOL_DECL_KIND_FIT:
            return string_append_cstr(buffer, "fit");
        case FENG_SYMBOL_DECL_KIND_FUNCTION:
            if (!string_append_cstr(buffer, "fn ") ||
                !string_append_bytes(buffer, name.data, name.length) ||
                !string_append_cstr(buffer, "(")) {
                return false;
            }
            for (index = 0U; index < feng_symbol_decl_param_count(decl); ++index) {
                FengSlice param_name = feng_symbol_decl_param_name(decl, index);

                if (index > 0U && !string_append_cstr(buffer, ", ")) {
                    return false;
                }
                if (!string_append_bytes(buffer, param_name.data, param_name.length) ||
                    !string_append_cstr(buffer, ": ") ||
                    !symbol_type_to_string(buffer, feng_symbol_decl_param_type(decl, index))) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   symbol_type_to_string(buffer, feng_symbol_decl_return_type(decl));
        case FENG_SYMBOL_DECL_KIND_MODULE:
        case FENG_SYMBOL_DECL_KIND_FIELD:
        case FENG_SYMBOL_DECL_KIND_METHOD:
        case FENG_SYMBOL_DECL_KIND_CONSTRUCTOR:
        case FENG_SYMBOL_DECL_KIND_FINALIZER:
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
    if (!string_append_cstr(buffer,
                            kind == FENG_SYMBOL_DECL_KIND_CONSTRUCTOR ? "ctor " :
                            kind == FENG_SYMBOL_DECL_KIND_FINALIZER ? "finalizer " : "fn ") ||
        !string_append_bytes(buffer, name.data, name.length) ||
        !string_append_cstr(buffer, "(")) {
        return false;
    }
    for (index = 0U; index < feng_symbol_decl_param_count(member); ++index) {
        FengSlice param_name = feng_symbol_decl_param_name(member, index);

        if (index > 0U && !string_append_cstr(buffer, ", ")) {
            return false;
        }
        if (!string_append_bytes(buffer, param_name.data, param_name.length) ||
            !string_append_cstr(buffer, ": ") ||
            !symbol_type_to_string(buffer, feng_symbol_decl_param_type(member, index))) {
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
            if (!string_append_cstr(&signature, "param ") ||
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
                                     target->binding->name.length) ||
                !string_append_cstr(&signature, ": ") ||
                !type_ref_to_string(&signature, target->binding->type)) {
                string_dispose(&signature);
                return NULL;
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
            offset < entry->end_offset) {
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
        case FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR:
        case FENG_RESOLVED_CALLABLE_FIT_METHOD:
            target->kind = FENG_LSP_RESOLVED_MEMBER;
            target->decl = callable->kind == FENG_RESOLVED_CALLABLE_FIT_METHOD
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
            case FENG_STMT_TRY:
                hit = find_call_hit_in_block(stmt->as.try_stmt.try_block, offset);
                if (hit == NULL) {
                    hit = find_call_hit_in_block(stmt->as.try_stmt.catch_block, offset);
                }
                if (hit == NULL) {
                    hit = find_call_hit_in_block(stmt->as.try_stmt.finally_block, offset);
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
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
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
                if (offset >= name_start && offset < name_start + stmt->as.binding.name.length) {
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
                        offset < name_start + stmt->as.for_stmt.iter_binding.name.length) {
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
        case FENG_STMT_TRY:
            return find_local_binding_at_in_block(source_text,
                                                  stmt->as.try_stmt.try_block,
                                                  offset,
                                                  target) ||
                   find_local_binding_at_in_block(source_text,
                                                  stmt->as.try_stmt.catch_block,
                                                  offset,
                                                  target) ||
                   find_local_binding_at_in_block(source_text,
                                                  stmt->as.try_stmt.finally_block,
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
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        if (find_decl_token_hit(program->declarations[decl_index], offset, target)) {
            local_list_dispose(&locals);
            return true;
        }
        if (find_type_ref_hit(program->declarations[decl_index], program, session, offset, target)) {
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
        case FENG_STMT_TRY:
            return resolve_object_field_target_block(session,
                                                     program,
                                                     stmt->as.try_stmt.try_block,
                                                     offset,
                                                     locals,
                                                     target) ||
                   resolve_object_field_target_block(session,
                                                     program,
                                                     stmt->as.try_stmt.catch_block,
                                                     offset,
                                                     locals,
                                                     target) ||
                   resolve_object_field_target_block(session,
                                                     program,
                                                     stmt->as.try_stmt.finally_block,
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
        case FENG_STMT_TRY:
            return collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               stmt->as.try_stmt.try_block,
                                               include_declaration,
                                               target,
                                               references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.try_stmt.catch_block,
                                              include_declaration,
                                              target,
                                              references) &&
                   collect_references_in_block(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              stmt->as.try_stmt.finally_block,
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

    if (offset_in_token(member->token, offset)) {
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
        if (offset_in_token(member->as.callable.params[index].token, offset)) {
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

    if (offset_in_token(decl->token, offset)) {
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
                if (offset_in_token(decl->as.function_decl.params[index].token, offset)) {
                    target->kind = FENG_LSP_RESOLVED_PARAM;
                    target->parameter = &decl->as.function_decl.params[index];
                    return true;
                }
            }
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
        case FENG_STMT_TRY:
            if (find_symbol_block_type_ref_hit(context,
                                               stmt->as.try_stmt.try_block,
                                               offset,
                                               target)) {
                return true;
            }
            if (find_symbol_block_type_ref_hit(context,
                                               stmt->as.try_stmt.catch_block,
                                               offset,
                                               target)) {
                return true;
            }
            return find_symbol_block_type_ref_hit(context,
                                                  stmt->as.try_stmt.finally_block,
                                                  offset,
                                                  target);
        case FENG_STMT_ASSIGN:
        case FENG_STMT_EXPR:
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
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL &&
        expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
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
        if (find_symbol_decl_token_hit(context,
                                       context->program->declarations[decl_index],
                                       offset,
                                       target)) {
            local_list_dispose(&locals);
            return true;
        }
        if (find_symbol_type_ref_hit(context,
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
            hover_text = hover_text_for_target(&target);
            ok = hover_text != NULL &&
                 string_append_cstr(&result, "{\"contents\":{\"kind\":\"plaintext\",\"value\":") &&
                 string_append_json_string(&result, hover_text) &&
                 string_append_cstr(&result, "}}");
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
        ok = hover_text != NULL &&
             string_append_cstr(&result, "{\"contents\":{\"kind\":\"plaintext\",\"value\":") &&
             string_append_json_string(&result, hover_text) &&
             string_append_cstr(&result, "}}");
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

static bool build_completion_json(const FengLspAnalysisSession *session,
                                  const FengProgram *program,
                                  size_t offset,
                                  FengLspString *json) {
    const FengDecl *enclosing_decl;
    const FengTypeMember *enclosing_member;
    FengLspLocalList locals = {0};
    const FengExpr *expr;
    const FengSemanticModule *program_module;
    bool first = true;
    size_t index;

    if (!string_append_cstr(json, "[")) {
        return false;
    }
    enclosing_decl = find_enclosing_decl(program, offset, &enclosing_member);
    if (enclosing_decl != NULL && !collect_visible_locals(enclosing_decl, enclosing_member, offset, &locals)) {
        local_list_dispose(&locals);
        return false;
    }
    expr = enclosing_decl != NULL ? find_expr_hit_in_decl(enclosing_decl, offset) : NULL;
    if (expr != NULL && expr->kind == FENG_EXPR_MEMBER) {
        const FengSemanticModule *alias_module = NULL;
        const FengDecl *owner_decl = NULL;

        if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_IDENTIFIER &&
            find_local(&locals, expr->as.member.object->as.identifier) == NULL) {
            alias_module = find_alias_module(session, program, expr->as.member.object->as.identifier);
        }
        if (alias_module != NULL) {
            for (index = 0U; index < alias_module->program_count; ++index) {
                size_t decl_index;
                const FengProgram *module_program = alias_module->programs[index];
                for (decl_index = 0U; decl_index < module_program->declaration_count; ++decl_index) {
                    const FengDecl *decl = module_program->declarations[decl_index];
                    if (decl->kind != FENG_DECL_FIT && decl->visibility == FENG_VISIBILITY_PUBLIC &&
                        !append_completion_item(json, &first, decl_name(decl), "module", 9)) {
                        local_list_dispose(&locals);
                        return false;
                    }
                }
            }
        } else {
            owner_decl = resolve_owner_decl_from_object_expr(session,
                                                             program,
                                                             expr->as.member.object,
                                                             &locals);
            if (owner_decl != NULL) {
                if (owner_decl->kind == FENG_DECL_TYPE) {
                    for (index = 0U; index < owner_decl->as.type_decl.member_count; ++index) {
                        const FengTypeMember *member = owner_decl->as.type_decl.members[index];
                        FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD ? member->as.field.name : member->as.callable.name;
                        if (!append_completion_item(json, &first, name, NULL, member->kind == FENG_TYPE_MEMBER_FIELD ? 5 : 2)) {
                            local_list_dispose(&locals);
                            return false;
                        }
                    }
                }
                if (owner_decl->kind == FENG_DECL_SPEC && owner_decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                    for (index = 0U; index < owner_decl->as.spec_decl.as.object.member_count; ++index) {
                        const FengTypeMember *member = owner_decl->as.spec_decl.as.object.members[index];
                        FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD ? member->as.field.name : member->as.callable.name;
                        if (!append_completion_item(json, &first, name, NULL, member->kind == FENG_TYPE_MEMBER_FIELD ? 5 : 2)) {
                            local_list_dispose(&locals);
                            return false;
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
        }
        program_module = find_program_module(session, program);
        if (program_module != NULL) {
            for (index = 0U; index < program_module->program_count; ++index) {
                size_t decl_index;
                const FengProgram *module_program = program_module->programs[index];
                for (decl_index = 0U; decl_index < module_program->declaration_count; ++decl_index) {
                    const FengDecl *decl = module_program->declarations[decl_index];
                    if (decl->kind != FENG_DECL_FIT &&
                        !append_completion_item(json, &first, decl_name(decl), NULL, decl->kind == FENG_DECL_FUNCTION ? 3 : 6)) {
                        local_list_dispose(&locals);
                        return false;
                    }
                }
            }
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
                size_t program_index;
                for (program_index = 0U; program_index < module->program_count; ++program_index) {
                    size_t decl_index;
                    const FengProgram *module_program = module->programs[program_index];
                    for (decl_index = 0U; decl_index < module_program->declaration_count; ++decl_index) {
                        const FengDecl *decl = module_program->declarations[decl_index];
                        if (decl->kind != FENG_DECL_FIT && decl->visibility == FENG_VISIBILITY_PUBLIC &&
                            !append_completion_item(json, &first, decl_name(decl), "imported", decl->kind == FENG_DECL_FUNCTION ? 3 : 6)) {
                            local_list_dispose(&locals);
                            return false;
                        }
                    }
                }
            }
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
    enclosing_decl = find_enclosing_decl(context->program, offset, &enclosing_member);
    if (enclosing_decl != NULL && !collect_visible_locals(enclosing_decl, enclosing_member, offset, &locals)) {
        local_list_dispose(&locals);
        return false;
    }
    expr = enclosing_decl != NULL ? find_expr_hit_in_decl(enclosing_decl, offset) : NULL;
    if (expr != NULL && expr->kind == FENG_EXPR_MEMBER) {
        const FengSymbolImportedModule *alias_module = NULL;
        const FengSymbolDeclView *owner_decl = NULL;

        if (expr->as.member.object != NULL && expr->as.member.object->kind == FENG_EXPR_IDENTIFIER &&
            find_local(&locals, expr->as.member.object->as.identifier) == NULL) {
            alias_module = find_symbol_alias_module(context->provider,
                                                    context->program,
                                                    expr->as.member.object->as.identifier);
        }
        if (alias_module != NULL) {
            for (index = 0U; index < feng_symbol_module_public_decl_count(alias_module); ++index) {
                const FengSymbolDeclView *decl = feng_symbol_module_public_decl_at(alias_module, index);

                if (decl == NULL || feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FIT) {
                    continue;
                }
                if (!append_completion_item(json,
                                            &first,
                                            feng_symbol_decl_name(decl),
                                            "module",
                                            9)) {
                    local_list_dispose(&locals);
                    return false;
                }
                ++item_count;
            }
        } else {
            owner_decl = resolve_symbol_owner_decl_from_object_expr(context,
                                                                    expr->as.member.object,
                                                                    &locals);
            if (owner_decl != NULL) {
                for (index = 0U; index < feng_symbol_decl_member_count(owner_decl); ++index) {
                    const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner_decl, index);
                    int kind = feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD ? 5 : 2;

                    if (!append_completion_item(json,
                                                &first,
                                                feng_symbol_decl_name(member),
                                                NULL,
                                                kind)) {
                        local_list_dispose(&locals);
                        return false;
                    }
                    ++item_count;
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
                int kind = feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FUNCTION ? 3 : 6;

                if (decl == NULL || feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FIT) {
                    continue;
                }
                if (!append_completion_item(json,
                                            &first,
                                            feng_symbol_decl_name(decl),
                                            NULL,
                                            kind)) {
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
                    int kind = feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FUNCTION ? 3 : 6;

                    if (decl == NULL || feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FIT) {
                        continue;
                    }
                    if (!append_completion_item(json,
                                                &first,
                                                feng_symbol_decl_name(decl),
                                                "imported",
                                                kind)) {
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
    if (!build_analysis_session(runtime, document, &session) || session.exit_code != 0) {
        free(uri);
        session_dispose(&session);
        return send_json_response(output, id, "[]");
    }
    program = find_program(&session, document->path);
    if (program == NULL) {
        free(uri);
        session_dispose(&session);
        return send_json_response(output, id, "[]");
    }
    ok = build_completion_json(&session, program, offset, &json);
    free(uri);
    session_dispose(&session);
    if (!ok) {
        if (runtime->errors != NULL) {
            fprintf(runtime->errors, "lsp: textDocument/completion: out of memory building response\n");
        }
        string_dispose(&json);
        return send_json_response(output, id, "[]");
    }
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
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

static bool handle_initialize(FILE *output, FengLspJsonValue id) {
    return send_json_response(output,
                              id,
                              "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":false}},\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"renameProvider\":{\"prepareProvider\":true},\"completionProvider\":{\"triggerCharacters\":[\".\"]}},\"serverInfo\":{\"name\":\"feng\"}}");
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
        ok = message.has_id ? handle_initialize(output, message.id)
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
