#include "dap/proxy.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug/debug.h"

#define FENG_DAP_PROXY_BUFFER_CAPACITY 4096U

typedef enum FengDapReadStatus {
    FENG_DAP_READ_OK = 0,
    FENG_DAP_READ_EOF = 1,
    FENG_DAP_READ_ERROR = 2,
} FengDapReadStatus;

/* Track one half-duplex relay leg between two file descriptors. */
typedef struct FengDapProxyPipe {
    int read_fd;
    int write_fd;
    bool close_write_on_finish;
    bool read_closed;
    bool write_closed;
    unsigned char *buffer;
    size_t buffer_start;
    size_t buffer_end;
    size_t buffer_capacity;
} FengDapProxyPipe;

/* Incremental DAP reader that preserves unread bytes across message parsing. */
typedef struct FengDapMessageReader {
    int fd;
    unsigned char *buffer;
    size_t buffer_start;
    size_t buffer_end;
    size_t buffer_capacity;
    bool reached_eof;
} FengDapMessageReader;

/* One fully framed DAP message. */
typedef struct FengDapMessage {
    char *frame;
    size_t frame_length;
    char *payload;
    size_t payload_length;
} FengDapMessage;

static const char *proxy_json_skip_whitespace(const char *cursor, const char *end);

/* Emit a proxy error message to the selected stderr fd. */
static void proxy_report_error(int error_fd,
                               const char *context,
                               const char *detail) {
    if (error_fd < 0 || context == NULL || detail == NULL) {
        return;
    }
    dprintf(error_fd, "%s: %s\n", context, detail);
}

/* Duplicate a printf-formatted string. */
static char *proxy_dup_printf(const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *out;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return NULL;
    }

    out = (char *)malloc((size_t)needed + 1U);
    if (out == NULL) {
        va_end(args_copy);
        return NULL;
    }
    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

/* Duplicate one byte range into a zero-terminated string. */
static char *proxy_dup_bytes(const unsigned char *bytes, size_t length) {
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    if (length > 0U) {
        memcpy(out, bytes, length);
    }
    out[length] = '\0';
    return out;
}

/* Grow a dynamic byte buffer and append one byte. */
static bool proxy_append_byte(char **buffer,
                              size_t *length,
                              size_t *capacity,
                              unsigned char byte) {
    char *resized;

    if (*length + 1U >= *capacity) {
        size_t new_capacity = *capacity == 0U ? 32U : *capacity * 2U;

        resized = (char *)realloc(*buffer, new_capacity);
        if (resized == NULL) {
            return false;
        }
        *buffer = resized;
        *capacity = new_capacity;
    }
    (*buffer)[*length] = (char)byte;
    *length += 1U;
    return true;
}

/* Append one Unicode codepoint encoded as UTF-8. */
static bool proxy_append_utf8(char **buffer,
                              size_t *length,
                              size_t *capacity,
                              unsigned int codepoint) {
    if (codepoint <= 0x7FU) {
        return proxy_append_byte(buffer, length, capacity, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7FFU) {
        return proxy_append_byte(buffer, length, capacity, (unsigned char)(0xC0U | (codepoint >> 6U))) &&
               proxy_append_byte(buffer, length, capacity, (unsigned char)(0x80U | (codepoint & 0x3FU)));
    }
    return proxy_append_byte(buffer, length, capacity, (unsigned char)(0xE0U | (codepoint >> 12U))) &&
           proxy_append_byte(buffer, length, capacity, (unsigned char)(0x80U | ((codepoint >> 6U) & 0x3FU))) &&
           proxy_append_byte(buffer, length, capacity, (unsigned char)(0x80U | (codepoint & 0x3FU)));
}

/* Escape a string for inclusion in one JSON string literal. */
static char *proxy_json_escape(const char *text) {
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    size_t index;

    if (text == NULL) {
        return proxy_dup_bytes((const unsigned char *)"", 0U);
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned char byte = (unsigned char)text[index];

        switch (byte) {
            case '\\':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, '\\')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '"':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, '"')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '\b':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, 'b')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '\f':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, 'f')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '\n':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, 'n')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '\r':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, 'r')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '\t':
                if (!proxy_append_byte(&buffer, &length, &capacity, '\\') ||
                    !proxy_append_byte(&buffer, &length, &capacity, 't')) {
                    free(buffer);
                    return NULL;
                }
                break;
            default:
                if (byte < 0x20U) {
                    char *escaped = proxy_dup_printf("\\u%04x", (unsigned int)byte);
                    size_t escape_index;

                    if (escaped == NULL) {
                        free(buffer);
                        return NULL;
                    }
                    for (escape_index = 0U; escaped[escape_index] != '\0'; ++escape_index) {
                        if (!proxy_append_byte(&buffer,
                                               &length,
                                               &capacity,
                                               (unsigned char)escaped[escape_index])) {
                            free(escaped);
                            free(buffer);
                            return NULL;
                        }
                    }
                    free(escaped);
                    break;
                }
                if (!proxy_append_byte(&buffer, &length, &capacity, byte)) {
                    free(buffer);
                    return NULL;
                }
                break;
        }
    }
    if (!proxy_append_byte(&buffer, &length, &capacity, '\0')) {
        free(buffer);
        return NULL;
    }
    length -= 1U;
    return buffer;
}

/* Write the full byte range even when short writes occur. */
static bool proxy_write_all(int fd,
                            const unsigned char *bytes,
                            size_t length,
                            int error_fd,
                            const char *context) {
    size_t written = 0U;

    while (written < length) {
        ssize_t chunk = write(fd, bytes + written, length - written);

        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            proxy_report_error(error_fd, context, strerror(errno));
            return false;
        }
        written += (size_t)chunk;
    }
    return true;
}

/* Write one framed DAP message to the selected stream. */
static bool proxy_write_message(int fd,
                                const char *json_payload,
                                int error_fd,
                                const char *context) {
    char header[64];
    int header_length;
    size_t payload_length;

    if (json_payload == NULL) {
        proxy_report_error(error_fd, context, "missing JSON payload");
        return false;
    }
    payload_length = strlen(json_payload);
    header_length = snprintf(header,
                             sizeof(header),
                             "Content-Length: %zu\r\n\r\n",
                             payload_length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        proxy_report_error(error_fd, context, "failed to format DAP message header");
        return false;
    }
    return proxy_write_all(fd,
                           (const unsigned char *)header,
                           (size_t)header_length,
                           error_fd,
                           context) &&
           proxy_write_all(fd,
                           (const unsigned char *)json_payload,
                           payload_length,
                           error_fd,
                           context);
}

/* Release a parsed DAP message. */
static void proxy_message_dispose(FengDapMessage *message) {
    if (message == NULL) {
        return;
    }
    free(message->frame);
    free(message->payload);
    memset(message, 0, sizeof(*message));
}

/* Initialize a reusable DAP message reader. */
static void proxy_reader_init(FengDapMessageReader *reader, int fd) {
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
}

/* Release heap storage owned by a DAP message reader. */
static void proxy_reader_dispose(FengDapMessageReader *reader) {
    if (reader == NULL) {
        return;
    }
    free(reader->buffer);
    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
}

/* Ensure the reader has contiguous writable tail space. */
static bool proxy_reader_ensure_capacity(FengDapMessageReader *reader, size_t extra) {
    size_t unread = reader->buffer_end - reader->buffer_start;
    unsigned char *resized;

    if (reader->buffer_capacity - reader->buffer_end >= extra) {
        return true;
    }
    if (reader->buffer_start > 0U) {
        memmove(reader->buffer,
                reader->buffer + reader->buffer_start,
                unread);
        reader->buffer_start = 0U;
        reader->buffer_end = unread;
        if (reader->buffer_capacity - reader->buffer_end >= extra) {
            return true;
        }
    }
    {
        size_t new_capacity = reader->buffer_capacity == 0U
                                  ? FENG_DAP_PROXY_BUFFER_CAPACITY
                                  : reader->buffer_capacity;

        while (new_capacity - unread < extra) {
            new_capacity *= 2U;
        }
        resized = (unsigned char *)realloc(reader->buffer, new_capacity);
        if (resized == NULL) {
            return false;
        }
        reader->buffer = resized;
        reader->buffer_capacity = new_capacity;
    }
    return true;
}

/* Read additional bytes from the underlying fd into the reader buffer. */
static bool proxy_reader_fill(FengDapMessageReader *reader, int error_fd) {
    ssize_t read_size;

    if (reader->reached_eof) {
        return true;
    }
    if (!proxy_reader_ensure_capacity(reader, FENG_DAP_PROXY_BUFFER_CAPACITY)) {
        proxy_report_error(error_fd, "failed to grow DAP reader buffer", "out of memory");
        return false;
    }
    read_size = read(reader->fd,
                     reader->buffer + reader->buffer_end,
                     reader->buffer_capacity - reader->buffer_end);
    if (read_size == 0) {
        reader->reached_eof = true;
        return true;
    }
    if (read_size < 0) {
        if (errno == EINTR) {
            return true;
        }
        proxy_report_error(error_fd, "failed to read DAP message", strerror(errno));
        return false;
    }
    reader->buffer_end += (size_t)read_size;
    return true;
}

/* Locate the end of one DAP header block if it is already buffered. */
static bool proxy_reader_find_header(const FengDapMessageReader *reader,
                                     size_t *out_header_offset,
                                     size_t *out_separator_length) {
    size_t index;

    for (index = reader->buffer_start; index + 3U < reader->buffer_end; ++index) {
        if (reader->buffer[index] == '\r' &&
            reader->buffer[index + 1U] == '\n' &&
            reader->buffer[index + 2U] == '\r' &&
            reader->buffer[index + 3U] == '\n') {
            *out_header_offset = index - reader->buffer_start;
            *out_separator_length = 4U;
            return true;
        }
    }
    for (index = reader->buffer_start; index + 1U < reader->buffer_end; ++index) {
        if (reader->buffer[index] == '\n' && reader->buffer[index + 1U] == '\n') {
            *out_header_offset = index - reader->buffer_start;
            *out_separator_length = 2U;
            return true;
        }
    }
    return false;
}

/* Parse one Content-Length header block. */
static bool proxy_parse_content_length(const unsigned char *header,
                                       size_t header_length,
                                       size_t *out_length) {
    size_t cursor = 0U;

    while (cursor < header_length) {
        size_t line_start = cursor;
        size_t line_length;
        const unsigned char *value_cursor;
        const unsigned char *line_end;
        char number_buffer[32];
        size_t number_length = 0U;
        char *endptr = NULL;
        unsigned long value;

        while (cursor < header_length && header[cursor] != '\n') {
            ++cursor;
        }
        line_length = cursor - line_start;
        if (cursor < header_length && header[cursor] == '\n') {
            ++cursor;
        }
        if (line_length > 0U && header[line_start + line_length - 1U] == '\r') {
            --line_length;
        }
        if (line_length == 0U) {
            continue;
        }
        if (line_length < 15U ||
            strncasecmp((const char *)header + line_start, "Content-Length:", 15U) != 0) {
            continue;
        }
        value_cursor = header + line_start + 15U;
        line_end = header + line_start + line_length;
        while (value_cursor < line_end && isspace(*value_cursor)) {
            ++value_cursor;
        }
        while (value_cursor < line_end && !isspace(*value_cursor)) {
            if (number_length + 1U >= sizeof(number_buffer)) {
                return false;
            }
            number_buffer[number_length++] = (char)*value_cursor++;
        }
        number_buffer[number_length] = '\0';
        value = strtoul(number_buffer, &endptr, 10);
        if (number_length == 0U || endptr == number_buffer || *endptr != '\0') {
            return false;
        }
        while (value_cursor < line_end) {
            if (!isspace(*value_cursor)) {
                return false;
            }
            ++value_cursor;
        }
        *out_length = (size_t)value;
        return true;
    }
    return false;
}

/* Read the next framed DAP message from one reader. */
static FengDapReadStatus proxy_reader_read_message(FengDapMessageReader *reader,
                                                   FengDapMessage *out_message,
                                                   int error_fd) {
    for (;;) {
        size_t header_offset = 0U;
        size_t separator_length = 0U;
        size_t content_length = 0U;
        size_t frame_length;

        if (proxy_reader_find_header(reader, &header_offset, &separator_length)) {
            const unsigned char *header = reader->buffer + reader->buffer_start;

            if (!proxy_parse_content_length(header, header_offset, &content_length)) {
                proxy_report_error(error_fd, "DAP protocol error", "missing Content-Length header");
                return FENG_DAP_READ_ERROR;
            }
            frame_length = header_offset + separator_length + content_length;
            if (reader->buffer_end - reader->buffer_start < frame_length) {
                if (reader->reached_eof) {
                    proxy_report_error(error_fd,
                                       "DAP protocol error",
                                       "unexpected EOF while reading DAP payload");
                    return FENG_DAP_READ_ERROR;
                }
            } else {
                size_t payload_offset = header_offset + separator_length;

                out_message->frame = proxy_dup_bytes(reader->buffer + reader->buffer_start,
                                                     frame_length);
                out_message->payload = proxy_dup_bytes(reader->buffer + reader->buffer_start + payload_offset,
                                                       content_length);
                if (out_message->frame == NULL || out_message->payload == NULL) {
                    proxy_message_dispose(out_message);
                    proxy_report_error(error_fd,
                                       "failed to allocate DAP message",
                                       "out of memory");
                    return FENG_DAP_READ_ERROR;
                }
                out_message->frame_length = frame_length;
                out_message->payload_length = content_length;
                reader->buffer_start += frame_length;
                if (reader->buffer_start == reader->buffer_end) {
                    reader->buffer_start = 0U;
                    reader->buffer_end = 0U;
                }
                return FENG_DAP_READ_OK;
            }
        }
        if (reader->reached_eof) {
            if (reader->buffer_end == reader->buffer_start) {
                return FENG_DAP_READ_EOF;
            }
            proxy_report_error(error_fd,
                               "DAP protocol error",
                               "unexpected EOF while reading DAP headers");
            return FENG_DAP_READ_ERROR;
        }
        if (!proxy_reader_fill(reader, error_fd)) {
            return FENG_DAP_READ_ERROR;
        }
    }
}

/* Skip one JSON string literal without materializing it. */
static bool proxy_json_skip_string(const char *cursor,
                                   const char *end,
                                   const char **out_after) {
    if (cursor >= end || *cursor != '"') {
        return false;
    }
    ++cursor;
    while (cursor < end) {
        if (*cursor == '"') {
            *out_after = cursor + 1;
            return true;
        }
        if (*cursor == '\\') {
            ++cursor;
            if (cursor >= end) {
                return false;
            }
            if (*cursor == 'u') {
                size_t index;

                for (index = 0U; index < 4U; ++index) {
                    ++cursor;
                    if (cursor >= end || !isxdigit((unsigned char)*cursor)) {
                        return false;
                    }
                }
            }
        }
        ++cursor;
    }
    return false;
}

/* Parse one JSON string literal into UTF-8 text. */
static bool proxy_json_parse_string_copy(const char *cursor,
                                         const char *end,
                                         char **out_string,
                                         const char **out_after) {
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;

    if (cursor >= end || *cursor != '"') {
        return false;
    }
    ++cursor;
    while (cursor < end) {
        unsigned char ch = (unsigned char)*cursor++;

        if (ch == '"') {
            if (!proxy_append_byte(&buffer, &length, &capacity, '\0')) {
                free(buffer);
                return false;
            }
            length -= 1U;
            *out_string = buffer;
            *out_after = cursor;
            return true;
        }
        if (ch == '\\') {
            unsigned char escape;

            if (cursor >= end) {
                free(buffer);
                return false;
            }
            escape = (unsigned char)*cursor++;
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    if (!proxy_append_byte(&buffer, &length, &capacity, escape)) {
                        free(buffer);
                        return false;
                    }
                    break;
                case 'b':
                    if (!proxy_append_byte(&buffer, &length, &capacity, '\b')) {
                        free(buffer);
                        return false;
                    }
                    break;
                case 'f':
                    if (!proxy_append_byte(&buffer, &length, &capacity, '\f')) {
                        free(buffer);
                        return false;
                    }
                    break;
                case 'n':
                    if (!proxy_append_byte(&buffer, &length, &capacity, '\n')) {
                        free(buffer);
                        return false;
                    }
                    break;
                case 'r':
                    if (!proxy_append_byte(&buffer, &length, &capacity, '\r')) {
                        free(buffer);
                        return false;
                    }
                    break;
                case 't':
                    if (!proxy_append_byte(&buffer, &length, &capacity, '\t')) {
                        free(buffer);
                        return false;
                    }
                    break;
                case 'u': {
                    unsigned int codepoint = 0U;
                    size_t index;

                    for (index = 0U; index < 4U; ++index) {
                        unsigned char hex;

                        if (cursor >= end) {
                            free(buffer);
                            return false;
                        }
                        hex = (unsigned char)*cursor++;
                        codepoint <<= 4U;
                        if (hex >= '0' && hex <= '9') {
                            codepoint |= (unsigned int)(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            codepoint |= (unsigned int)(10 + hex - 'a');
                        } else if (hex >= 'A' && hex <= 'F') {
                            codepoint |= (unsigned int)(10 + hex - 'A');
                        } else {
                            free(buffer);
                            return false;
                        }
                    }
                    if (!proxy_append_utf8(&buffer, &length, &capacity, codepoint)) {
                        free(buffer);
                        return false;
                    }
                    break;
                }
                default:
                    free(buffer);
                    return false;
            }
            continue;
        }
        if (!proxy_append_byte(&buffer, &length, &capacity, ch)) {
            free(buffer);
            return false;
        }
    }
    free(buffer);
    return false;
}

/* Skip one JSON value range. */
static bool proxy_json_skip_value(const char *cursor,
                                  const char *end,
                                  const char **out_after) {
    cursor = proxy_json_skip_whitespace(cursor, end);
    if (cursor >= end) {
        return false;
    }
    if (*cursor == '"') {
        return proxy_json_skip_string(cursor, end, out_after);
    }
    if (*cursor == '{') {
        ++cursor;
        cursor = proxy_json_skip_whitespace(cursor, end);
        if (cursor < end && *cursor == '}') {
            *out_after = cursor + 1;
            return true;
        }
        while (cursor < end) {
            const char *after_key;
            const char *after_value;

            if (!proxy_json_skip_string(cursor, end, &after_key)) {
                return false;
            }
            after_key = proxy_json_skip_whitespace(after_key, end);
            if (after_key >= end || *after_key != ':') {
                return false;
            }
            if (!proxy_json_skip_value(after_key + 1, end, &after_value)) {
                return false;
            }
            cursor = proxy_json_skip_whitespace(after_value, end);
            if (cursor < end && *cursor == ',') {
                cursor = proxy_json_skip_whitespace(cursor + 1, end);
                continue;
            }
            if (cursor < end && *cursor == '}') {
                *out_after = cursor + 1;
                return true;
            }
            return false;
        }
        return false;
    }
    if (*cursor == '[') {
        ++cursor;
        cursor = proxy_json_skip_whitespace(cursor, end);
        if (cursor < end && *cursor == ']') {
            *out_after = cursor + 1;
            return true;
        }
        while (cursor < end) {
            const char *after_value;

            if (!proxy_json_skip_value(cursor, end, &after_value)) {
                return false;
            }
            cursor = proxy_json_skip_whitespace(after_value, end);
            if (cursor < end && *cursor == ',') {
                cursor = proxy_json_skip_whitespace(cursor + 1, end);
                continue;
            }
            if (cursor < end && *cursor == ']') {
                *out_after = cursor + 1;
                return true;
            }
            return false;
        }
        return false;
    }
    while (cursor < end &&
           !isspace((unsigned char)*cursor) &&
           *cursor != ',' &&
           *cursor != '}' &&
           *cursor != ']') {
        ++cursor;
    }
    *out_after = cursor;
    return true;
}

/* Skip ASCII whitespace in one JSON document. */
static const char *proxy_json_skip_whitespace(const char *cursor, const char *end) {
    while (cursor < end && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

/* Find one named top-level object member. */
static bool proxy_json_find_object_member(const char *json,
                                          size_t json_length,
                                          const char *key,
                                          const char **out_value_start,
                                          const char **out_value_end) {
    const char *cursor = proxy_json_skip_whitespace(json, json + json_length);
    const char *end = json + json_length;

    if (cursor >= end || *cursor != '{') {
        return false;
    }
    cursor = proxy_json_skip_whitespace(cursor + 1, end);
    if (cursor < end && *cursor == '}') {
        return false;
    }
    while (cursor < end) {
        char *member_key = NULL;
        const char *after_key;
        const char *value_start;
        const char *value_end;
        bool matched;

        if (!proxy_json_parse_string_copy(cursor, end, &member_key, &after_key)) {
            return false;
        }
        after_key = proxy_json_skip_whitespace(after_key, end);
        if (after_key >= end || *after_key != ':') {
            free(member_key);
            return false;
        }
        value_start = proxy_json_skip_whitespace(after_key + 1, end);
        if (!proxy_json_skip_value(value_start, end, &value_end)) {
            free(member_key);
            return false;
        }
        matched = strcmp(member_key, key) == 0;
        free(member_key);
        if (matched) {
            *out_value_start = value_start;
            *out_value_end = value_end;
            return true;
        }
        cursor = proxy_json_skip_whitespace(value_end, end);
        if (cursor < end && *cursor == ',') {
            cursor = proxy_json_skip_whitespace(cursor + 1, end);
            continue;
        }
        return false;
    }
    return false;
}

/* Parse one string member from the top-level object. */
static bool proxy_json_get_string_member(const char *json,
                                         size_t json_length,
                                         const char *key,
                                         char **out_value) {
    const char *value_start;
    const char *value_end;
    const char *after_string;

    if (!proxy_json_find_object_member(json,
                                       json_length,
                                       key,
                                       &value_start,
                                       &value_end)) {
        return false;
    }
    if (!proxy_json_parse_string_copy(value_start, value_end, out_value, &after_string)) {
        return false;
    }
    after_string = proxy_json_skip_whitespace(after_string, value_end);
    return after_string == value_end;
}

/* Parse one unsigned integer member from the top-level object. */
static bool proxy_json_get_u64_member(const char *json,
                                      size_t json_length,
                                      const char *key,
                                      uint64_t *out_value) {
    const char *value_start;
    const char *value_end;
    char number_buffer[32];
    char *endptr = NULL;
    unsigned long long value;
    size_t trimmed_length;

    if (!proxy_json_find_object_member(json,
                                       json_length,
                                       key,
                                       &value_start,
                                       &value_end)) {
        return false;
    }
    value_start = proxy_json_skip_whitespace(value_start, value_end);
    while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
        --value_end;
    }
    trimmed_length = (size_t)(value_end - value_start);
    if (trimmed_length == 0U || trimmed_length >= sizeof(number_buffer)) {
        return false;
    }
    memcpy(number_buffer, value_start, trimmed_length);
    number_buffer[trimmed_length] = '\0';
    value = strtoull(number_buffer, &endptr, 10);
    if (endptr == number_buffer || *endptr != '\0') {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

/* Parse one boolean member from the top-level object. */
static bool proxy_json_get_bool_member(const char *json,
                                       size_t json_length,
                                       const char *key,
                                       bool *out_value) {
    const char *value_start;
    const char *value_end;

    if (!proxy_json_find_object_member(json,
                                       json_length,
                                       key,
                                       &value_start,
                                       &value_end)) {
        return false;
    }
    value_start = proxy_json_skip_whitespace(value_start, value_end);
    while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
        --value_end;
    }
    if ((size_t)(value_end - value_start) == 4U && strncmp(value_start, "true", 4U) == 0) {
        *out_value = true;
        return true;
    }
    if ((size_t)(value_end - value_start) == 5U && strncmp(value_start, "false", 5U) == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

/* Extract `arguments.program` from one DAP launch request. */
static bool proxy_json_get_launch_program(const char *json,
                                          size_t json_length,
                                          char **out_program_path) {
    const char *arguments_start;
    const char *arguments_end;
    const char *program_start;
    const char *program_end;
    const char *after_string;

    if (!proxy_json_find_object_member(json,
                                       json_length,
                                       "arguments",
                                       &arguments_start,
                                       &arguments_end)) {
        return false;
    }
    if (!proxy_json_find_object_member(arguments_start,
                                       (size_t)(arguments_end - arguments_start),
                                       "program",
                                       &program_start,
                                       &program_end)) {
        return false;
    }
    if (!proxy_json_parse_string_copy(program_start, program_end, out_program_path, &after_string)) {
        return false;
    }
    after_string = proxy_json_skip_whitespace(after_string, program_end);
    return after_string == program_end;
}

/* Send a local initialize response before the native backend starts. */
static bool proxy_send_initialize_response(int output_fd,
                                           uint64_t request_seq,
                                           uint64_t *next_seq,
                                           int error_fd) {
    char *json_payload = proxy_dup_printf(
        "{\"seq\":%llu,\"type\":\"response\",\"request_seq\":%llu,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}",
        (unsigned long long)(*next_seq)++,
        (unsigned long long)request_seq);
    bool ok;

    if (json_payload == NULL) {
        proxy_report_error(error_fd, "failed to format initialize response", "out of memory");
        return false;
    }
    ok = proxy_write_message(output_fd,
                             json_payload,
                             error_fd,
                             "failed to write initialize response");
    free(json_payload);
    return ok;
}

/* Send one local request failure response back to the editor. */
static bool proxy_send_request_failure_response(int output_fd,
                                                const char *command,
                                                uint64_t request_seq,
                                                const char *detail,
                                                uint64_t *next_seq,
                                                int error_fd) {
    char *escaped_command = proxy_json_escape(command != NULL ? command : "request");
    char *escaped_detail = proxy_json_escape(detail != NULL ? detail : "request failed");
    char *json_payload;
    bool ok;

    if (escaped_command == NULL || escaped_detail == NULL) {
        free(escaped_command);
        free(escaped_detail);
        proxy_report_error(error_fd, "failed to format request failure response", "out of memory");
        return false;
    }
    json_payload = proxy_dup_printf(
        "{\"seq\":%llu,\"type\":\"response\",\"request_seq\":%llu,\"success\":false,\"command\":\"%s\",\"message\":\"request failed\",\"body\":{\"error\":{\"id\":1,\"format\":\"%s\"}}}",
        (unsigned long long)(*next_seq)++,
        (unsigned long long)request_seq,
        escaped_command,
        escaped_detail);
    free(escaped_command);
    free(escaped_detail);
    if (json_payload == NULL) {
        proxy_report_error(error_fd, "failed to format request failure response", "out of memory");
        return false;
    }
    ok = proxy_write_message(output_fd,
                             json_payload,
                             error_fd,
                             "failed to write request failure response");
    free(json_payload);
    return ok;
}

/* Validate one launch request against the final `.fd` sidecar on disk. */
static bool proxy_validate_launch_request(const FengDapMessage *launch_message,
                                          char **out_error_detail) {
    char *program_path = NULL;
    char *fd_path = NULL;
    char *debug_error = NULL;
    char *fingerprint_error = NULL;
    FengDebugArtifact artifact = {0};
    uint64_t fingerprint;
    bool ok = false;

    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (launch_message == NULL || launch_message->payload == NULL) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("launch request payload is required");
        }
        return false;
    }
    if (!proxy_json_get_launch_program(launch_message->payload,
                                       launch_message->payload_length,
                                       &program_path) ||
        program_path == NULL ||
        program_path[0] == '\0') {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("launch request must provide arguments.program");
        }
        goto cleanup;
    }
    fd_path = proxy_dup_printf("%s.fd", program_path);
    if (fd_path == NULL) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("out of memory building debug sidecar path");
        }
        goto cleanup;
    }
    if (!feng_debug_read_fd(fd_path, &artifact, &debug_error)) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("%s", debug_error != NULL ? debug_error : "failed to load debug sidecar");
        }
        goto cleanup;
    }
    fingerprint = feng_debug_fnv1a64_file(program_path, &fingerprint_error);
    if (fingerprint_error != NULL) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("%s", fingerprint_error);
        }
        goto cleanup;
    }
    if (artifact.binary_fingerprint != fingerprint) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("debug sidecar fingerprint mismatch for %s", program_path);
        }
        goto cleanup;
    }
    ok = true;

cleanup:
    free(program_path);
    free(fd_path);
    free(debug_error);
    free(fingerprint_error);
    feng_debug_artifact_dispose(&artifact);
    return ok;
}

/* Return whether the relay leg still has bytes buffered for output. */
static bool proxy_pipe_has_pending_output(const FengDapProxyPipe *pipe_state) {
    return pipe_state != NULL && pipe_state->buffer_start < pipe_state->buffer_end;
}

/* Ensure the relay pipe has enough writable space for more bytes. */
static bool proxy_pipe_ensure_capacity(FengDapProxyPipe *pipe_state, size_t extra) {
    size_t unread = pipe_state->buffer_end - pipe_state->buffer_start;
    unsigned char *resized;

    if (pipe_state->buffer_capacity - pipe_state->buffer_end >= extra) {
        return true;
    }
    if (pipe_state->buffer_start > 0U) {
        memmove(pipe_state->buffer,
                pipe_state->buffer + pipe_state->buffer_start,
                unread);
        pipe_state->buffer_start = 0U;
        pipe_state->buffer_end = unread;
        if (pipe_state->buffer_capacity - pipe_state->buffer_end >= extra) {
            return true;
        }
    }
    {
        size_t new_capacity = pipe_state->buffer_capacity == 0U
                                  ? FENG_DAP_PROXY_BUFFER_CAPACITY
                                  : pipe_state->buffer_capacity;

        while (new_capacity - unread < extra) {
            new_capacity *= 2U;
        }
        resized = (unsigned char *)realloc(pipe_state->buffer, new_capacity);
        if (resized == NULL) {
            return false;
        }
        pipe_state->buffer = resized;
        pipe_state->buffer_capacity = new_capacity;
    }
    return true;
}

/* Initialize one relay leg, optionally seeding buffered bytes. */
static bool proxy_pipe_init(FengDapProxyPipe *pipe_state,
                            int read_fd,
                            int write_fd,
                            bool close_write_on_finish,
                            const unsigned char *initial_bytes,
                            size_t initial_length,
                            bool read_closed) {
    memset(pipe_state, 0, sizeof(*pipe_state));
    pipe_state->read_fd = read_fd;
    pipe_state->write_fd = write_fd;
    pipe_state->close_write_on_finish = close_write_on_finish;
    pipe_state->read_closed = read_closed;
    if (initial_length > 0U) {
        if (!proxy_pipe_ensure_capacity(pipe_state, initial_length)) {
            return false;
        }
        memcpy(pipe_state->buffer, initial_bytes, initial_length);
        pipe_state->buffer_end = initial_length;
    }
    return true;
}

/* Release heap storage owned by one relay leg. */
static void proxy_pipe_dispose(FengDapProxyPipe *pipe_state) {
    if (pipe_state == NULL) {
        return;
    }
    free(pipe_state->buffer);
    memset(pipe_state, 0, sizeof(*pipe_state));
}

/* Return whether the relay leg has fully drained and no longer needs polling. */
static bool proxy_pipe_is_done(const FengDapProxyPipe *pipe_state) {
    if (pipe_state == NULL) {
        return true;
    }
    if (!pipe_state->read_closed) {
        return false;
    }
    if (proxy_pipe_has_pending_output(pipe_state)) {
        return false;
    }
    return !pipe_state->close_write_on_finish || pipe_state->write_closed;
}

/* Close the downstream writer once upstream EOF has been fully drained. */
static void proxy_pipe_finish_write(FengDapProxyPipe *pipe_state) {
    if (pipe_state == NULL || pipe_state->write_closed || !pipe_state->close_write_on_finish) {
        return;
    }
    if (!pipe_state->read_closed || proxy_pipe_has_pending_output(pipe_state)) {
        return;
    }
    close(pipe_state->write_fd);
    pipe_state->write_closed = true;
}

/* Read the next chunk from the upstream side into the relay buffer. */
static bool proxy_pipe_read_into_buffer(FengDapProxyPipe *pipe_state, int error_fd) {
    ssize_t read_size;

    if (pipe_state == NULL || pipe_state->read_closed) {
        return true;
    }
    if (!proxy_pipe_ensure_capacity(pipe_state, FENG_DAP_PROXY_BUFFER_CAPACITY)) {
        proxy_report_error(error_fd, "failed to grow DAP relay buffer", "out of memory");
        return false;
    }
    read_size = read(pipe_state->read_fd,
                     pipe_state->buffer + pipe_state->buffer_end,
                     pipe_state->buffer_capacity - pipe_state->buffer_end);
    if (read_size == 0) {
        pipe_state->read_closed = true;
        proxy_pipe_finish_write(pipe_state);
        return true;
    }
    if (read_size < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return true;
        }
        proxy_report_error(error_fd, "failed to read dap proxy stream", strerror(errno));
        return false;
    }

    pipe_state->buffer_end += (size_t)read_size;
    return true;
}

/* Flush buffered bytes to the downstream side. */
static bool proxy_pipe_write_from_buffer(FengDapProxyPipe *pipe_state, int error_fd) {
    ssize_t written;

    if (pipe_state == NULL || pipe_state->write_closed || !proxy_pipe_has_pending_output(pipe_state)) {
        return true;
    }

    written = write(pipe_state->write_fd,
                    pipe_state->buffer + pipe_state->buffer_start,
                    pipe_state->buffer_end - pipe_state->buffer_start);
    if (written < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return true;
        }
        if (errno == EPIPE && pipe_state->close_write_on_finish) {
            pipe_state->write_closed = true;
            pipe_state->read_closed = true;
            pipe_state->buffer_start = 0U;
            pipe_state->buffer_end = 0U;
            return true;
        }
        proxy_report_error(error_fd, "failed to write dap proxy stream", strerror(errno));
        return false;
    }

    pipe_state->buffer_start += (size_t)written;
    if (pipe_state->buffer_start == pipe_state->buffer_end) {
        pipe_state->buffer_start = 0U;
        pipe_state->buffer_end = 0U;
        proxy_pipe_finish_write(pipe_state);
    }
    return true;
}

/* Relay bytes between the editor stdio and the native backend pipes. */
static bool proxy_relay_streams(int input_fd,
                                int output_fd,
                                int child_stdin_fd,
                                int child_stdout_fd,
                                const unsigned char *initial_inbound,
                                size_t initial_inbound_length,
                                bool inbound_read_closed,
                                const unsigned char *initial_outbound,
                                size_t initial_outbound_length,
                                bool outbound_read_closed,
                                int error_fd) {
    FengDapProxyPipe inbound;
    FengDapProxyPipe outbound;

    if (!proxy_pipe_init(&inbound,
                         input_fd,
                         child_stdin_fd,
                         true,
                         initial_inbound,
                         initial_inbound_length,
                         inbound_read_closed) ||
        !proxy_pipe_init(&outbound,
                         child_stdout_fd,
                         output_fd,
                         false,
                         initial_outbound,
                         initial_outbound_length,
                         outbound_read_closed)) {
        proxy_pipe_dispose(&inbound);
        proxy_pipe_dispose(&outbound);
        proxy_report_error(error_fd, "failed to initialize DAP relay buffers", "out of memory");
        return false;
    }
    proxy_pipe_finish_write(&inbound);
    proxy_pipe_finish_write(&outbound);

    while (!proxy_pipe_is_done(&inbound) || !proxy_pipe_is_done(&outbound)) {
        struct pollfd poll_fds[4];
        int inbound_read_slot = -1;
        int inbound_write_slot = -1;
        int outbound_read_slot = -1;
        int outbound_write_slot = -1;
        nfds_t poll_count = 0U;
        int poll_rc;

        if (!inbound.read_closed) {
            inbound_read_slot = (int)poll_count;
            poll_fds[poll_count].fd = inbound.read_fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }
        if (!inbound.write_closed && proxy_pipe_has_pending_output(&inbound)) {
            inbound_write_slot = (int)poll_count;
            poll_fds[poll_count].fd = inbound.write_fd;
            poll_fds[poll_count].events = POLLOUT | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }
        if (!outbound.read_closed) {
            outbound_read_slot = (int)poll_count;
            poll_fds[poll_count].fd = outbound.read_fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }
        if (proxy_pipe_has_pending_output(&outbound)) {
            outbound_write_slot = (int)poll_count;
            poll_fds[poll_count].fd = outbound.write_fd;
            poll_fds[poll_count].events = POLLOUT | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }

        if (poll_count == 0U) {
            break;
        }

        poll_rc = poll(poll_fds, poll_count, -1);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            proxy_pipe_dispose(&inbound);
            proxy_pipe_dispose(&outbound);
            proxy_report_error(error_fd, "failed to poll dap proxy streams", strerror(errno));
            return false;
        }

        if (inbound_read_slot >= 0 && (poll_fds[inbound_read_slot].revents & (POLLIN | POLLHUP)) != 0) {
            if (!proxy_pipe_read_into_buffer(&inbound, error_fd)) {
                proxy_pipe_dispose(&inbound);
                proxy_pipe_dispose(&outbound);
                return false;
            }
        }
        if (outbound_read_slot >= 0 && (poll_fds[outbound_read_slot].revents & (POLLIN | POLLHUP)) != 0) {
            if (!proxy_pipe_read_into_buffer(&outbound, error_fd)) {
                proxy_pipe_dispose(&inbound);
                proxy_pipe_dispose(&outbound);
                return false;
            }
        }
        if (inbound_write_slot >= 0 && (poll_fds[inbound_write_slot].revents & (POLLOUT | POLLHUP)) != 0) {
            if (!proxy_pipe_write_from_buffer(&inbound, error_fd)) {
                proxy_pipe_dispose(&inbound);
                proxy_pipe_dispose(&outbound);
                return false;
            }
        }
        if (outbound_write_slot >= 0 && (poll_fds[outbound_write_slot].revents & (POLLOUT | POLLHUP)) != 0) {
            if (!proxy_pipe_write_from_buffer(&outbound, error_fd)) {
                proxy_pipe_dispose(&inbound);
                proxy_pipe_dispose(&outbound);
                return false;
            }
        }
    }

    proxy_pipe_dispose(&inbound);
    proxy_pipe_dispose(&outbound);
    return true;
}

/* Wait for the native backend process and normalize its exit status. */
static int proxy_wait_for_child(pid_t child, int error_fd, const char *backend_program) {
    int status = 0;

    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        proxy_report_error(error_fd, "failed to wait for dap backend", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        dprintf(error_fd,
                "%s terminated by signal %d\n",
                backend_program != NULL ? backend_program : "lldb-dap",
                WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

/* Launch the native backend and surface exec failures to the caller. */
static pid_t proxy_spawn_backend(const char *backend_program,
                                 int child_stdin[2],
                                 int child_stdout[2],
                                 int error_fd,
                                 char **out_error_detail) {
    int exec_status[2] = {-1, -1};
    pid_t child;
    char *const argv[] = {(char *)(backend_program != NULL ? backend_program : "lldb-dap"), NULL};
    int exec_errno = 0;
    ssize_t read_size;

    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (pipe(exec_status) != 0) {
        proxy_report_error(error_fd, "failed to create dap exec-status pipe", strerror(errno));
        return -1;
    }
    if (fcntl(exec_status[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(exec_status[0]);
        close(exec_status[1]);
        proxy_report_error(error_fd, "failed to configure dap exec-status pipe", strerror(errno));
        return -1;
    }

    child = fork();
    if (child < 0) {
        close(exec_status[0]);
        close(exec_status[1]);
        proxy_report_error(error_fd, "failed to fork dap backend", strerror(errno));
        return -1;
    }
    if (child == 0) {
        close(exec_status[0]);
        close(child_stdin[1]);
        close(child_stdout[0]);
        if (dup2(child_stdin[0], STDIN_FILENO) < 0 ||
            dup2(child_stdout[1], STDOUT_FILENO) < 0 ||
            (error_fd >= 0 && error_fd != STDERR_FILENO && dup2(error_fd, STDERR_FILENO) < 0)) {
            exec_errno = errno;
            write(exec_status[1], &exec_errno, sizeof(exec_errno));
            _exit(127);
        }
        close(child_stdin[0]);
        close(child_stdout[1]);
        execvp(argv[0], argv);
        exec_errno = errno;
        write(exec_status[1], &exec_errno, sizeof(exec_errno));
        _exit(127);
    }

    close(exec_status[1]);
    read_size = read(exec_status[0], &exec_errno, sizeof(exec_errno));
    close(exec_status[0]);
    if (read_size > 0) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("failed to exec %s: %s",
                                                 argv[0],
                                                 strerror(exec_errno));
        }
        waitpid(child, NULL, 0);
        return -1;
    }
    return child;
}

/* Consume and validate the backend initialize response before launch forwarding. */
static bool proxy_consume_backend_initialize_response(FengDapMessageReader *reader,
                                                      int error_fd,
                                                      char **out_error_detail) {
    FengDapMessage message = {0};
    FengDapReadStatus status;
    char *type = NULL;
    char *command = NULL;
    bool success = false;
    bool ok = false;

    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    status = proxy_reader_read_message(reader, &message, error_fd);
    if (status != FENG_DAP_READ_OK) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("lldb-dap did not return an initialize response");
        }
        goto cleanup;
    }
    if (!proxy_json_get_string_member(message.payload,
                                      message.payload_length,
                                      "type",
                                      &type) ||
        !proxy_json_get_string_member(message.payload,
                                      message.payload_length,
                                      "command",
                                      &command) ||
        !proxy_json_get_bool_member(message.payload,
                                    message.payload_length,
                                    "success",
                                    &success) ||
        strcmp(type, "response") != 0 ||
        strcmp(command, "initialize") != 0 ||
        !success) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("lldb-dap returned an invalid initialize response");
        }
        goto cleanup;
    }
    ok = true;

cleanup:
    free(type);
    free(command);
    proxy_message_dispose(&message);
    return ok;
}

/* Run the launch-gated DAP session until the backend exits or launch fails. */
static int proxy_run_session(const char *backend_program,
                             int input_fd,
                             int output_fd,
                             int error_fd) {
    FengDapMessageReader client_reader;
    FengDapMessageReader backend_reader;
    FengDapMessage initialize_request = {0};
    FengDapMessage request = {0};
    bool have_initialize = false;
    uint64_t next_seq = 1U;

    proxy_reader_init(&client_reader, input_fd);
    proxy_reader_init(&backend_reader, -1);

    for (;;) {
        FengDapReadStatus status = proxy_reader_read_message(&client_reader, &request, error_fd);
        char *type = NULL;
        char *command = NULL;
        uint64_t request_seq = 0U;
        int child_stdin[2] = {-1, -1};
        int child_stdout[2] = {-1, -1};
        pid_t child = -1;
        int exit_code;
        char *error_detail = NULL;

        if (status == FENG_DAP_READ_EOF) {
            break;
        }
        if (status == FENG_DAP_READ_ERROR) {
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return 1;
        }
        if (!proxy_json_get_string_member(request.payload,
                                          request.payload_length,
                                          "type",
                                          &type) ||
            !proxy_json_get_string_member(request.payload,
                                          request.payload_length,
                                          "command",
                                          &command) ||
            !proxy_json_get_u64_member(request.payload,
                                       request.payload_length,
                                       "seq",
                                       &request_seq) ||
            strcmp(type, "request") != 0) {
            free(type);
            free(command);
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            proxy_report_error(error_fd, "DAP protocol error", "expected a request message");
            return 1;
        }

        if (strcmp(command, "initialize") == 0) {
            free(type);
            free(command);
            if (have_initialize) {
                proxy_send_request_failure_response(output_fd,
                                                    "initialize",
                                                    request_seq,
                                                    "duplicate initialize request before launch",
                                                    &next_seq,
                                                    error_fd);
                proxy_message_dispose(&request);
                proxy_message_dispose(&initialize_request);
                proxy_reader_dispose(&client_reader);
                proxy_reader_dispose(&backend_reader);
                return 1;
            }
            if (!proxy_send_initialize_response(output_fd,
                                                request_seq,
                                                &next_seq,
                                                error_fd)) {
                proxy_message_dispose(&request);
                proxy_reader_dispose(&client_reader);
                proxy_reader_dispose(&backend_reader);
                return 1;
            }
            initialize_request = request;
            memset(&request, 0, sizeof(request));
            have_initialize = true;
            continue;
        }

        if (strcmp(command, "launch") != 0) {
            proxy_send_request_failure_response(output_fd,
                                                command,
                                                request_seq,
                                                have_initialize
                                                    ? "request is not supported before launch validation completes"
                                                    : "initialize must complete before launch",
                                                &next_seq,
                                                error_fd);
            free(type);
            free(command);
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return 1;
        }

        free(type);
        free(command);
        if (!have_initialize) {
            proxy_send_request_failure_response(output_fd,
                                                "launch",
                                                request_seq,
                                                "initialize must complete before launch",
                                                &next_seq,
                                                error_fd);
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return 1;
        }
        if (!proxy_validate_launch_request(&request, &error_detail)) {
            proxy_send_request_failure_response(output_fd,
                                                "launch",
                                                request_seq,
                                                error_detail != NULL ? error_detail : "failed to validate launch request",
                                                &next_seq,
                                                error_fd);
            free(error_detail);
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return 1;
        }
        free(error_detail);
        error_detail = NULL;
        if (pipe(child_stdin) != 0 || pipe(child_stdout) != 0) {
            proxy_send_request_failure_response(output_fd,
                                                "launch",
                                                request_seq,
                                                strerror(errno),
                                                &next_seq,
                                                error_fd);
            if (child_stdin[0] >= 0) {
                close(child_stdin[0]);
                close(child_stdin[1]);
            }
            if (child_stdout[0] >= 0) {
                close(child_stdout[0]);
                close(child_stdout[1]);
            }
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return 1;
        }
        child = proxy_spawn_backend(backend_program,
                                    child_stdin,
                                    child_stdout,
                                    error_fd,
                                    &error_detail);
        if (child < 0) {
            proxy_send_request_failure_response(output_fd,
                                                "launch",
                                                request_seq,
                                                error_detail != NULL ? error_detail : "failed to start lldb-dap",
                                                &next_seq,
                                                error_fd);
            free(error_detail);
            close(child_stdin[0]);
            close(child_stdin[1]);
            close(child_stdout[0]);
            close(child_stdout[1]);
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return 1;
        }
        close(child_stdin[0]);
        close(child_stdout[1]);
        proxy_reader_init(&backend_reader, child_stdout[0]);

        if (!proxy_write_all(child_stdin[1],
                             (const unsigned char *)initialize_request.frame,
                             initialize_request.frame_length,
                             error_fd,
                             "failed to forward initialize request to lldb-dap") ||
            !proxy_consume_backend_initialize_response(&backend_reader,
                                                       error_fd,
                                                       &error_detail) ||
            !proxy_write_all(child_stdin[1],
                             (const unsigned char *)request.frame,
                             request.frame_length,
                             error_fd,
                             "failed to forward launch request to lldb-dap")) {
            proxy_send_request_failure_response(output_fd,
                                                "launch",
                                                request_seq,
                                                error_detail != NULL ? error_detail : "failed to initialize lldb-dap backend",
                                                &next_seq,
                                                error_fd);
            free(error_detail);
            close(child_stdin[1]);
            close(child_stdout[0]);
            exit_code = proxy_wait_for_child(child, error_fd, backend_program);
            proxy_message_dispose(&request);
            proxy_message_dispose(&initialize_request);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return exit_code != 0 ? exit_code : 1;
        }

        proxy_message_dispose(&initialize_request);
        proxy_message_dispose(&request);
        if (!proxy_relay_streams(input_fd,
                                 output_fd,
                                 child_stdin[1],
                                 child_stdout[0],
                                 client_reader.buffer != NULL
                                     ? client_reader.buffer + client_reader.buffer_start
                                     : NULL,
                                 client_reader.buffer_end - client_reader.buffer_start,
                                 client_reader.reached_eof,
                                 backend_reader.buffer != NULL
                                     ? backend_reader.buffer + backend_reader.buffer_start
                                     : NULL,
                                 backend_reader.buffer_end - backend_reader.buffer_start,
                                 backend_reader.reached_eof,
                                 error_fd)) {
            close(child_stdin[1]);
            close(child_stdout[0]);
            exit_code = proxy_wait_for_child(child, error_fd, backend_program);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            return exit_code != 0 ? exit_code : 1;
        }
        close(child_stdout[0]);
        exit_code = proxy_wait_for_child(child, error_fd, backend_program);
        proxy_reader_dispose(&client_reader);
        proxy_reader_dispose(&backend_reader);
        return exit_code;
    }

    proxy_message_dispose(&initialize_request);
    proxy_reader_dispose(&client_reader);
    proxy_reader_dispose(&backend_reader);
    return 0;
}

/* Run the launch-gated DAP proxy against the selected backend program. */
int feng_dap_proxy_run(const char *backend_program,
                       int input_fd,
                       int output_fd,
                       int error_fd) {
    struct sigaction old_sigpipe;
    struct sigaction ignore_sigpipe;
    int exit_code;

    if (backend_program == NULL || input_fd < 0 || output_fd < 0) {
        proxy_report_error(error_fd,
                           "invalid dap proxy configuration",
                           "backend program and stdio fds are required");
        return 1;
    }

    ignore_sigpipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sigpipe.sa_mask);
    ignore_sigpipe.sa_flags = 0;
    sigaction(SIGPIPE, &ignore_sigpipe, &old_sigpipe);

    exit_code = proxy_run_session(backend_program, input_fd, output_fd, error_fd);
    sigaction(SIGPIPE, &old_sigpipe, NULL);
    return exit_code;
}
