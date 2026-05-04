#include "cli/lsp/server.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum FengLspParseStatus {
    FENG_LSP_PARSE_OK = 0,
    FENG_LSP_PARSE_INVALID_JSON,
    FENG_LSP_PARSE_INVALID_REQUEST
} FengLspParseStatus;

typedef struct FengLspMessage {
    char *method;
    const char *id_start;
    const char *id_end;
    bool has_id;
} FengLspMessage;

typedef struct FengLspServerState {
    bool shutdown_requested;
    bool should_exit;
    int exit_code;
} FengLspServerState;

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

static bool slice_equals(const char *start, const char *end, const char *text) {
    size_t length = (size_t)(end - start);
    return strlen(text) == length && memcmp(start, text, length) == 0;
}

static FengLspParseStatus parse_jsonrpc_message(const char *payload,
                                                size_t payload_length,
                                                FengLspMessage *out_message) {
    const char *cursor = payload;
    const char *end = payload + payload_length;

    memset(out_message, 0, sizeof(*out_message));
    skip_ws(&cursor, end);
    if (cursor >= end || *cursor != '{') {
        return FENG_LSP_PARSE_INVALID_REQUEST;
    }
    ++cursor;
    skip_ws(&cursor, end);
    if (cursor < end && *cursor == '}') {
        return FENG_LSP_PARSE_INVALID_REQUEST;
    }

    while (cursor < end) {
        const char *key_start;
        const char *key_end;

        skip_ws(&cursor, end);
        if (!scan_json_string(&cursor, end, &key_start, &key_end)) {
            return FENG_LSP_PARSE_INVALID_JSON;
        }
        skip_ws(&cursor, end);
        if (cursor >= end || *cursor != ':') {
            return FENG_LSP_PARSE_INVALID_JSON;
        }
        ++cursor;
        skip_ws(&cursor, end);

        if (slice_equals(key_start, key_end, "method")) {
            const char *method_start;
            const char *method_end;

            if (out_message->method != NULL) {
                return FENG_LSP_PARSE_INVALID_REQUEST;
            }
            if (!scan_json_string(&cursor, end, &method_start, &method_end)) {
                return FENG_LSP_PARSE_INVALID_REQUEST;
            }
            out_message->method = dup_range(method_start, method_end);
            if (out_message->method == NULL) {
                return FENG_LSP_PARSE_INVALID_JSON;
            }
        } else if (slice_equals(key_start, key_end, "id")) {
            const char *id_start;

            if (out_message->has_id) {
                return FENG_LSP_PARSE_INVALID_REQUEST;
            }
            id_start = cursor;
            if (!skip_json_value(&cursor, end)) {
                return FENG_LSP_PARSE_INVALID_JSON;
            }
            out_message->has_id = true;
            out_message->id_start = id_start;
            out_message->id_end = cursor;
        } else {
            if (!skip_json_value(&cursor, end)) {
                return FENG_LSP_PARSE_INVALID_JSON;
            }
        }

        skip_ws(&cursor, end);
        if (cursor < end && *cursor == ',') {
            ++cursor;
            continue;
        }
        if (cursor < end && *cursor == '}') {
            ++cursor;
            break;
        }
        return FENG_LSP_PARSE_INVALID_JSON;
    }

    skip_ws(&cursor, end);
    if (cursor != end) {
        return FENG_LSP_PARSE_INVALID_JSON;
    }
    if (out_message->method == NULL) {
        return FENG_LSP_PARSE_INVALID_REQUEST;
    }
    return FENG_LSP_PARSE_OK;
}

static void dispose_message(FengLspMessage *message) {
    free(message->method);
    memset(message, 0, sizeof(*message));
}

static bool send_payload(FILE *output, const char *payload, size_t payload_length) {
    if (fprintf(output, "Content-Length: %zu\r\n\r\n", payload_length) < 0) {
        return false;
    }
    if (fwrite(payload, 1U, payload_length, output) != payload_length) {
        return false;
    }
    return fflush(output) == 0;
}

static bool send_simple_response(FILE *output,
                                 const char *id_start,
                                 size_t id_length,
                                 const char *suffix) {
    static const char *kPrefix = "{\"jsonrpc\":\"2.0\",\"id\":";
    size_t prefix_length = strlen(kPrefix);
    size_t suffix_length = strlen(suffix);
    size_t payload_length = prefix_length + id_length + suffix_length;
    char *payload = (char *)malloc(payload_length + 1U);
    bool ok;

    if (payload == NULL) {
        return false;
    }
    memcpy(payload, kPrefix, prefix_length);
    memcpy(payload + prefix_length, id_start, id_length);
    memcpy(payload + prefix_length + id_length, suffix, suffix_length + 1U);

    ok = send_payload(output, payload, payload_length);
    free(payload);
    return ok;
}

static bool send_error_response(FILE *output,
                                const char *id_start,
                                size_t id_length,
                                int code,
                                const char *message) {
    static const char *kPrefix = "{\"jsonrpc\":\"2.0\",\"id\":";
    static const char *kMiddle = ",\"error\":{\"code\":";
    static const char *kCodeSuffix = ",\"message\":\"";
    static const char *kSuffix = "\"}}";
    char code_buffer[32];
    int code_length;
    size_t prefix_length = strlen(kPrefix);
    size_t middle_length = strlen(kMiddle);
    size_t code_suffix_length = strlen(kCodeSuffix);
    size_t message_length = strlen(message);
    size_t suffix_length = strlen(kSuffix);
    size_t payload_length;
    char *payload;
    bool ok;

    code_length = snprintf(code_buffer, sizeof(code_buffer), "%d", code);
    if (code_length < 0) {
        return false;
    }

    payload_length = prefix_length + id_length + middle_length + (size_t)code_length
                     + code_suffix_length + message_length + suffix_length;
    payload = (char *)malloc(payload_length + 1U);
    if (payload == NULL) {
        return false;
    }

    memcpy(payload, kPrefix, prefix_length);
    memcpy(payload + prefix_length, id_start, id_length);
    memcpy(payload + prefix_length + id_length, kMiddle, middle_length);
    memcpy(payload + prefix_length + id_length + middle_length,
           code_buffer,
           (size_t)code_length);
    memcpy(payload + prefix_length + id_length + middle_length + (size_t)code_length,
           kCodeSuffix,
           code_suffix_length);
    memcpy(payload + prefix_length + id_length + middle_length + (size_t)code_length
               + code_suffix_length,
           message,
           message_length);
    memcpy(payload + prefix_length + id_length + middle_length + (size_t)code_length
               + code_suffix_length + message_length,
           kSuffix,
           suffix_length + 1U);

    ok = send_payload(output, payload, payload_length);
    free(payload);
    return ok;
}

static int read_header_line(FILE *input, char **out_line) {
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    int ch;

    ch = fgetc(input);
    if (ch == EOF) {
        if (ferror(input)) {
            return -1;
        }
        return 0;
    }

    while (ch != EOF && ch != '\n') {
        if (length + 1U >= capacity) {
            size_t new_capacity = capacity == 0U ? 64U : capacity * 2U;
            char *resized = (char *)realloc(buffer, new_capacity);

            if (resized == NULL) {
                free(buffer);
                return -1;
            }
            buffer = resized;
            capacity = new_capacity;
        }
        buffer[length++] = (char)ch;
        ch = fgetc(input);
    }

    if (ch == EOF && ferror(input)) {
        free(buffer);
        return -1;
    }
    if (ch == EOF && length > 0U) {
        free(buffer);
        return -1;
    }

    if (length > 0U && buffer[length - 1U] == '\r') {
        --length;
    }
    if (buffer == NULL) {
        buffer = (char *)malloc(1U);
        if (buffer == NULL) {
            return -1;
        }
    }
    buffer[length] = '\0';
    *out_line = buffer;
    return 1;
}

static bool parse_content_length(const char *line, size_t *out_length) {
    const char *cursor;
    char *endptr = NULL;
    unsigned long value;

    if (strncasecmp(line, "Content-Length:", 15U) != 0) {
        return false;
    }
    cursor = line + 15U;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0') {
        return false;
    }
    value = strtoul(cursor, &endptr, 10);
    if (endptr == cursor) {
        return false;
    }
    while (*endptr != '\0') {
        if (!isspace((unsigned char)*endptr)) {
            return false;
        }
        ++endptr;
    }
    *out_length = (size_t)value;
    return true;
}

static bool read_payload(FILE *input, size_t payload_length, char **out_payload) {
    char *payload = (char *)malloc(payload_length + 1U);

    if (payload == NULL) {
        return false;
    }
    if (payload_length > 0U && fread(payload, 1U, payload_length, input) != payload_length) {
        free(payload);
        return false;
    }
    payload[payload_length] = '\0';
    *out_payload = payload;
    return true;
}

static bool handle_message(FILE *output,
                           const char *payload,
                           size_t payload_length,
                           FengLspServerState *state,
                           FILE *errors) {
    FengLspMessage message;
    FengLspParseStatus status = parse_jsonrpc_message(payload, payload_length, &message);
    const char *id_start = "null";
    size_t id_length = 4U;
    bool ok = true;

    if (status == FENG_LSP_PARSE_INVALID_JSON) {
        return send_error_response(output, "null", 4U, -32700, "Parse error");
    }
    if (status == FENG_LSP_PARSE_INVALID_REQUEST) {
        return send_error_response(output, "null", 4U, -32600, "Invalid Request");
    }

    if (message.has_id) {
        id_start = message.id_start;
        id_length = (size_t)(message.id_end - message.id_start);
    }

    if (strcmp(message.method, "initialize") == 0) {
        if (!message.has_id) {
            ok = send_error_response(output, "null", 4U, -32600, "Invalid Request");
        } else {
            ok = send_simple_response(output,
                                      id_start,
                                      id_length,
                                      ",\"result\":{\"capabilities\":{},\"serverInfo\":{\"name\":\"feng\"}}}");
        }
    } else if (strcmp(message.method, "shutdown") == 0) {
        if (!message.has_id) {
            ok = send_error_response(output, "null", 4U, -32600, "Invalid Request");
        } else {
            state->shutdown_requested = true;
            ok = send_simple_response(output, id_start, id_length, ",\"result\":null}");
        }
    } else if (strcmp(message.method, "exit") == 0) {
        state->should_exit = true;
        state->exit_code = state->shutdown_requested ? 0 : 1;
    } else if (strcmp(message.method, "initialized") == 0
               || strcmp(message.method, "$/cancelRequest") == 0
               || strcmp(message.method, "$/setTrace") == 0) {
        ok = true;
    } else if (message.has_id) {
        ok = send_error_response(output, id_start, id_length, -32601, "Method not found");
    }

    if (!ok) {
        fprintf(errors, "lsp protocol error: failed to write response\n");
    }
    dispose_message(&message);
    return ok;
}

int feng_lsp_server_run(FILE *input, FILE *output, FILE *errors) {
    FengLspServerState state = {0};

    while (!state.should_exit) {
        bool saw_header = false;
        bool has_length = false;
        size_t content_length = 0U;

        while (true) {
            char *line = NULL;
            int status = read_header_line(input, &line);

            if (status == 0) {
                if (!saw_header) {
                    return state.exit_code;
                }
                fprintf(errors, "lsp protocol error: unexpected EOF while reading headers\n");
                return 1;
            }
            if (status < 0) {
                fprintf(errors, "lsp protocol error: failed to read message headers\n");
                free(line);
                return 1;
            }

            saw_header = true;
            if (line[0] == '\0') {
                free(line);
                break;
            }
            if (parse_content_length(line, &content_length)) {
                has_length = true;
            }
            free(line);
        }

        if (!has_length) {
            fprintf(errors, "lsp protocol error: missing Content-Length header\n");
            return 1;
        }

        {
            char *payload = NULL;

            if (!read_payload(input, content_length, &payload)) {
                fprintf(errors, "lsp protocol error: failed to read payload\n");
                free(payload);
                return 1;
            }
            if (!handle_message(output, payload, content_length, &state, errors)) {
                free(payload);
                return 1;
            }
            free(payload);
        }
    }

    return state.exit_code;
}
