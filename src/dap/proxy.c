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
    FENG_DAP_READ_PENDING = 3,
} FengDapReadStatus;

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

/* One pending JSON string-literal replacement inside a framed DAP payload. */
typedef struct FengDapJsonStringReplacement {
    size_t start_offset;
    size_t end_offset;
    char *replacement;
} FengDapJsonStringReplacement;

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

/* Grow a dynamic byte buffer and append one byte range. */
static bool proxy_append_bytes(char **buffer,
                               size_t *length,
                               size_t *capacity,
                               const char *bytes,
                               size_t byte_count) {
    char *resized;

    if (byte_count == 0U) {
        return true;
    }
    if (*length + byte_count + 1U >= *capacity) {
        size_t new_capacity = *capacity == 0U ? 64U : *capacity;

        while (*length + byte_count + 1U >= new_capacity) {
            new_capacity *= 2U;
        }
        resized = (char *)realloc(*buffer, new_capacity);
        if (resized == NULL) {
            return false;
        }
        *buffer = resized;
        *capacity = new_capacity;
    }
    memcpy(*buffer + *length, bytes, byte_count);
    *length += byte_count;
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

/* Forward one existing framed DAP message without rewriting its payload. */
static bool proxy_write_framed_message(int fd,
                                       const FengDapMessage *message,
                                       int error_fd,
                                       const char *context) {
    if (message == NULL || message->frame == NULL) {
        proxy_report_error(error_fd, context, "missing DAP frame");
        return false;
    }
    return proxy_write_all(fd,
                           (const unsigned char *)message->frame,
                           message->frame_length,
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

/* Attempt to parse one complete buffered DAP message without reading more bytes. */
static FengDapReadStatus proxy_reader_try_read_buffered_message(FengDapMessageReader *reader,
                                                                FengDapMessage *out_message,
                                                                int error_fd) {
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
            return FENG_DAP_READ_PENDING;
        }

        {
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
    return FENG_DAP_READ_PENDING;
}

/* Read the next framed DAP message from one reader. */
static FengDapReadStatus proxy_reader_read_message(FengDapMessageReader *reader,
                                                   FengDapMessage *out_message,
                                                   int error_fd) {
    for (;;) {
        FengDapReadStatus status = proxy_reader_try_read_buffered_message(reader,
                                                                          out_message,
                                                                          error_fd);

        if (status != FENG_DAP_READ_PENDING) {
            return status;
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

/* Fallback member lookup for small known object slices when structured lookup misses. */
static bool proxy_json_find_object_member_fallback(const char *json,
                                                   size_t json_length,
                                                   const char *key,
                                                   const char **out_value_start,
                                                   const char **out_value_end) {
    const char *cursor = json;
    const char *end = json + json_length;
    size_t key_length;

    if (key == NULL) {
        return false;
    }
    key_length = strlen(key);
    while (cursor < end) {
        const char *match = strstr(cursor, key);
        const char *quoted_start;
        const char *after_key;
        const char *value_start;
        const char *value_end;

        if (match == NULL || match >= end) {
            return false;
        }
        quoted_start = match - 1;
        if (quoted_start < json || *quoted_start != '"') {
            cursor = match + 1;
            continue;
        }
        if (match + key_length >= end || match[key_length] != '"') {
            cursor = match + 1;
            continue;
        }
        after_key = proxy_json_skip_whitespace(match + key_length + 1U, end);
        if (after_key >= end || *after_key != ':') {
            cursor = match + 1;
            continue;
        }
        value_start = proxy_json_skip_whitespace(after_key + 1U, end);
        if (!proxy_json_skip_value(value_start, end, &value_end)) {
            cursor = match + 1;
            continue;
        }
        *out_value_start = value_start;
        *out_value_end = value_end;
        return true;
    }
    return false;
}

/* Find one named object member, falling back to a restricted raw key search when needed. */
static bool proxy_json_find_object_member_loose(const char *json,
                                                size_t json_length,
                                                const char *key,
                                                const char **out_value_start,
                                                const char **out_value_end) {
    return proxy_json_find_object_member(json,
                                         json_length,
                                         key,
                                         out_value_start,
                                         out_value_end) ||
           proxy_json_find_object_member_fallback(json,
                                                  json_length,
                                                  key,
                                                  out_value_start,
                                                  out_value_end);
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

/* Validate one launch request and optionally keep the loaded `.fd` artifact alive. */
static bool proxy_validate_launch_request(const FengDapMessage *launch_message,
                                          FengDebugArtifact *out_artifact,
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
    if (out_artifact != NULL) {
        feng_debug_artifact_dispose(out_artifact);
        memset(out_artifact, 0, sizeof(*out_artifact));
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
            *out_error_detail = proxy_dup_printf("%s",
                                                 debug_error != NULL ? debug_error : "failed to load debug sidecar");
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
    if (out_artifact != NULL) {
        *out_artifact = artifact;
        memset(&artifact, 0, sizeof(artifact));
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

/* Join two POSIX-style path segments. */
static char *proxy_path_join(const char *lhs, const char *rhs) {
    size_t lhs_length = strlen(lhs);
    size_t rhs_length = strlen(rhs);
    bool need_separator = lhs_length > 0U && lhs[lhs_length - 1U] != '/';

    return proxy_dup_printf("%s%s%s",
                            lhs,
                            need_separator ? "/" : "",
                            rhs_length > 0U ? rhs : "");
}

/* Resolve one path relative to a root directory using the same boundary rules as codegen. */
static bool proxy_path_relative_from_root(const char *root,
                                          const char *path,
                                          char **out_relative) {
    size_t root_length;
    const char *relative_start;

    *out_relative = NULL;
    if (root == NULL || path == NULL) {
        return false;
    }

    root_length = strlen(root);
    while (root_length > 1U && root[root_length - 1U] == '/') {
        root_length--;
    }
    if (root_length == 0U) {
        return false;
    }
    if (strncmp(path, root, root_length) != 0) {
        return false;
    }
    if (root_length == 1U && root[0] == '/') {
        relative_start = path[0] == '/' ? path + 1 : path;
    } else {
        if (path[root_length] != '/') {
            return false;
        }
        relative_start = path + root_length + 1U;
    }
    while (*relative_start == '/') {
        ++relative_start;
    }
    if (*relative_start == '\0') {
        return false;
    }

    *out_relative = proxy_dup_printf("%s", relative_start);
    return *out_relative != NULL;
}

/* Map one editor local file path to a package URI using `.fd.PKGS`. */
static bool proxy_local_path_to_package_uri(const FengDebugArtifact *artifact,
                                            const char *local_path,
                                            char **out_uri,
                                            char **out_error_detail) {
    char *resolved_path = NULL;
    char *candidate_uri = NULL;
    size_t match_count = 0U;

    if (out_uri != NULL) {
        free(*out_uri);
        *out_uri = NULL;
    }
    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (artifact == NULL || local_path == NULL || out_uri == NULL) {
        return false;
    }

    resolved_path = realpath(local_path, NULL);
    if (resolved_path == NULL) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("failed to resolve breakpoint source path %s: %s",
                                                 local_path,
                                                 strerror(errno));
        }
        return false;
    }

    for (size_t index = 0U; index < artifact->package_count; ++index) {
        char *relative_path = NULL;
        char *uri = NULL;
        char *resolved_root = NULL;
        const char *package_root = artifact->packages[index].local_root_path;

        resolved_root = realpath(package_root, NULL);
        if (resolved_root != NULL) {
            package_root = resolved_root;
        }

        if (!proxy_path_relative_from_root(package_root,
                                           resolved_path,
                                           &relative_path)) {
            free(resolved_root);
            continue;
        }
        uri = proxy_dup_printf("%s://%s",
                               artifact->packages[index].package_name,
                               relative_path);
        free(relative_path);
        free(resolved_root);
        if (uri == NULL) {
            free(resolved_path);
            proxy_report_error(STDERR_FILENO,
                               "failed to rewrite setBreakpoints path",
                               "out of memory");
            free(candidate_uri);
            return false;
        }
        match_count += 1U;
        if (match_count == 1U) {
            candidate_uri = uri;
            continue;
        }
        free(uri);
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("breakpoint source path %s matches multiple debug packages",
                                                 resolved_path);
        }
        free(candidate_uri);
        free(resolved_path);
        return false;
    }

    if (match_count == 0U) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("breakpoint source path %s is not part of the debug closure",
                                                 resolved_path);
        }
        free(resolved_path);
        return false;
    }

    *out_uri = candidate_uri;
    free(resolved_path);
    return true;
}

/* Map one package URI back to a local file path using `.fd.PKGS`. */
static bool proxy_package_uri_to_local_path(const FengDebugArtifact *artifact,
                                            const char *uri,
                                            char **out_local_path) {
    const char *scheme_separator;
    char *package_name = NULL;
    char *local_path = NULL;
    bool ok = false;

    if (out_local_path != NULL) {
        free(*out_local_path);
        *out_local_path = NULL;
    }
    if (artifact == NULL || uri == NULL || out_local_path == NULL) {
        return false;
    }

    scheme_separator = strstr(uri, "://");
    if (scheme_separator == NULL || scheme_separator == uri || scheme_separator[3] == '\0') {
        return false;
    }
    package_name = proxy_dup_bytes((const unsigned char *)uri,
                                   (size_t)(scheme_separator - uri));
    if (package_name == NULL) {
        return false;
    }
    for (size_t index = 0U; index < artifact->package_count; ++index) {
        if (strcmp(artifact->packages[index].package_name, package_name) != 0) {
            continue;
        }
        local_path = proxy_path_join(artifact->packages[index].local_root_path,
                                     scheme_separator + 3U);
        if (local_path == NULL) {
            free(package_name);
            return false;
        }
        *out_local_path = local_path;
        ok = true;
        break;
    }
    free(package_name);
    return ok;
}

/* Release one owned JSON replacement entry. */
static void proxy_json_string_replacement_dispose(FengDapJsonStringReplacement *replacement) {
    if (replacement == NULL) {
        return;
    }
    free(replacement->replacement);
    replacement->replacement = NULL;
    replacement->start_offset = 0U;
    replacement->end_offset = 0U;
}

/* Release one dynamic JSON replacement list. */
static void proxy_json_string_replacements_dispose(FengDapJsonStringReplacement *replacements,
                                                   size_t replacement_count) {
    if (replacements == NULL) {
        return;
    }
    for (size_t index = 0U; index < replacement_count; ++index) {
        proxy_json_string_replacement_dispose(&replacements[index]);
    }
    free(replacements);
}

/* Insert one JSON replacement entry while keeping the list ordered by offset. */
static bool proxy_insert_json_string_replacement(FengDapJsonStringReplacement **replacements,
                                                 size_t *replacement_count,
                                                 size_t *replacement_capacity,
                                                 size_t start_offset,
                                                 size_t end_offset,
                                                 char *replacement) {
    FengDapJsonStringReplacement *grown;
    size_t new_capacity;
    size_t insert_at;

    if (*replacement_count == *replacement_capacity) {
        new_capacity = *replacement_capacity == 0U ? 4U : (*replacement_capacity * 2U);
        grown = (FengDapJsonStringReplacement *)realloc(*replacements,
                                                        new_capacity * sizeof(**replacements));
        if (grown == NULL) {
            return false;
        }
        *replacements = grown;
        *replacement_capacity = new_capacity;
    }

    insert_at = *replacement_count;
    while (insert_at > 0U && (*replacements)[insert_at - 1U].start_offset > start_offset) {
        (*replacements)[insert_at] = (*replacements)[insert_at - 1U];
        --insert_at;
    }
    (*replacements)[insert_at].start_offset = start_offset;
    (*replacements)[insert_at].end_offset = end_offset;
    (*replacements)[insert_at].replacement = replacement;
    *replacement_count += 1U;
    return true;
}

/* Find one frame mapping by backend symbol inside the loaded debug artifact. */
static const FengCodegenMapingFrameRecord *proxy_find_frame_record(const FengDebugArtifact *artifact,
                                                                   const char *backend_symbol) {
    size_t index;

    if (artifact == NULL || backend_symbol == NULL) {
        return NULL;
    }
    for (index = 0U; index < artifact->info.frame_count; ++index) {
        if (strcmp(artifact->info.frames[index].backend_symbol, backend_symbol) == 0) {
            return artifact->info.frames + index;
        }
    }
    return NULL;
}

/* Materialize a new JSON payload after applying one or more string-literal replacements. */
static bool proxy_apply_json_string_replacements(const char *json,
                                                 size_t json_length,
                                                 const FengDapJsonStringReplacement *replacements,
                                                 size_t replacement_count,
                                                 char **out_json,
                                                 int error_fd,
                                                 const char *context) {
    char *rewritten = NULL;
    size_t rewritten_length = 0U;
    size_t rewritten_capacity = 0U;
    size_t cursor = 0U;

    *out_json = NULL;
    if (replacement_count == 0U) {
        return true;
    }
    for (size_t index = 0U; index < replacement_count; ++index) {
        char *escaped_replacement;
        size_t escaped_length;

        if (replacements[index].start_offset < cursor ||
            replacements[index].end_offset < replacements[index].start_offset ||
            replacements[index].end_offset > json_length) {
            free(rewritten);
            proxy_report_error(error_fd, context, "invalid JSON replacement bounds");
            return false;
        }
        if (!proxy_append_bytes(&rewritten,
                                &rewritten_length,
                                &rewritten_capacity,
                                json + cursor,
                                replacements[index].start_offset - cursor)) {
            free(rewritten);
            proxy_report_error(error_fd, context, "out of memory");
            return false;
        }
        escaped_replacement = proxy_json_escape(replacements[index].replacement);
        if (escaped_replacement == NULL) {
            free(rewritten);
            proxy_report_error(error_fd, context, "out of memory");
            return false;
        }
        escaped_length = strlen(escaped_replacement);
        if (!proxy_append_byte(&rewritten, &rewritten_length, &rewritten_capacity, '"') ||
            !proxy_append_bytes(&rewritten,
                                &rewritten_length,
                                &rewritten_capacity,
                                escaped_replacement,
                                escaped_length) ||
            !proxy_append_byte(&rewritten, &rewritten_length, &rewritten_capacity, '"')) {
            free(escaped_replacement);
            free(rewritten);
            proxy_report_error(error_fd, context, "out of memory");
            return false;
        }
        free(escaped_replacement);
        cursor = replacements[index].end_offset;
    }
    if (!proxy_append_bytes(&rewritten,
                            &rewritten_length,
                            &rewritten_capacity,
                            json + cursor,
                            json_length - cursor) ||
        !proxy_append_byte(&rewritten, &rewritten_length, &rewritten_capacity, '\0')) {
        free(rewritten);
        proxy_report_error(error_fd, context, "out of memory");
        return false;
    }
    rewritten[rewritten_length - 1U] = '\0';
    *out_json = rewritten;
    return true;
}

/* Replace one arbitrary JSON span with already-materialized JSON text. */
static bool proxy_replace_json_span(const char *json,
                                    size_t json_length,
                                    size_t start_offset,
                                    size_t end_offset,
                                    const char *replacement,
                                    char **out_json,
                                    int error_fd,
                                    const char *context) {
    char *rewritten = NULL;
    size_t rewritten_length = 0U;
    size_t rewritten_capacity = 0U;

    *out_json = NULL;
    if (json == NULL || replacement == NULL || start_offset > end_offset || end_offset > json_length) {
        proxy_report_error(error_fd, context, "invalid JSON span replacement");
        return false;
    }
    if (!proxy_append_bytes(&rewritten,
                            &rewritten_length,
                            &rewritten_capacity,
                            json,
                            start_offset) ||
        !proxy_append_bytes(&rewritten,
                            &rewritten_length,
                            &rewritten_capacity,
                            replacement,
                            strlen(replacement)) ||
        !proxy_append_bytes(&rewritten,
                            &rewritten_length,
                            &rewritten_capacity,
                            json + end_offset,
                            json_length - end_offset) ||
        !proxy_append_byte(&rewritten, &rewritten_length, &rewritten_capacity, '\0')) {
        free(rewritten);
        proxy_report_error(error_fd, context, "out of memory");
        return false;
    }
    rewritten[rewritten_length - 1U] = '\0';
    *out_json = rewritten;
    return true;
}

/* Parse one unsigned integer from an already-located JSON value span. */
static bool proxy_json_parse_u64_range(const char *value_start,
                                       const char *value_end,
                                       uint64_t *out_value) {
    char number_buffer[32];
    char *endptr = NULL;
    size_t trimmed_length;
    unsigned long long value;

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

/* Locate `arguments.source.path` inside one setBreakpoints request payload. */
static bool proxy_json_get_set_breakpoints_path_range(const char *json,
                                                      size_t json_length,
                                                      const char **out_path_start,
                                                      const char **out_path_end) {
    const char *arguments_start;
    const char *arguments_end;
    const char *source_start;
    const char *source_end;

    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "arguments",
                                             &arguments_start,
                                             &arguments_end)) {
        return false;
    }
    if (!proxy_json_find_object_member_loose(arguments_start,
                                             (size_t)(arguments_end - arguments_start),
                                             "source",
                                             &source_start,
                                             &source_end)) {
        return false;
    }
    return proxy_json_find_object_member_loose(source_start,
                                               (size_t)(source_end - source_start),
                                               "path",
                                               out_path_start,
                                               out_path_end);
}

/* Rewrite `setBreakpoints.arguments.source.path` from local path to package URI. */
static bool proxy_rewrite_set_breakpoints_request_payload(const char *json,
                                                          size_t json_length,
                                                          const FengDebugArtifact *artifact,
                                                          char **out_json,
                                                          char **out_error_detail,
                                                          int error_fd) {
    const char *path_start;
    const char *path_end;
    const char *after_string;
    char *source_path = NULL;
    char *package_uri = NULL;
    FengDapJsonStringReplacement replacement = {0};
    bool ok = false;

    *out_json = NULL;
    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (!proxy_json_get_set_breakpoints_path_range(json,
                                                   json_length,
                                                   &path_start,
                                                   &path_end)) {
        return true;
    }
    if (!proxy_json_parse_string_copy(path_start, path_end, &source_path, &after_string) ||
        proxy_json_skip_whitespace(after_string, path_end) != path_end) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("setBreakpoints arguments.source.path must be a string");
        }
        return false;
    }
    if (strstr(source_path, "://") != NULL) {
        ok = true;
        goto cleanup;
    }
    if (!proxy_local_path_to_package_uri(artifact,
                                         source_path,
                                         &package_uri,
                                         out_error_detail)) {
        goto cleanup;
    }
    replacement.start_offset = (size_t)(path_start - json);
    replacement.end_offset = (size_t)(path_end - json);
    replacement.replacement = package_uri;
    package_uri = NULL;
    if (!proxy_apply_json_string_replacements(json,
                                              json_length,
                                              &replacement,
                                              1U,
                                              out_json,
                                              error_fd,
                                              "failed to rewrite setBreakpoints path")) {
        goto cleanup;
    }
    ok = true;

cleanup:
    free(source_path);
    free(package_uri);
    proxy_json_string_replacement_dispose(&replacement);
    return ok;
}

/* Rewrite one stackTrace frame payload using .fd name/path mappings. */
static bool proxy_rewrite_stack_trace_frame_payload(const char *frame_json,
                                                    size_t frame_length,
                                                    const FengDebugArtifact *artifact,
                                                    const FengCodegenMapingFrameRecord **out_frame_record,
                                                    char **out_json,
                                                    int error_fd) {
    const char *name_start;
    const char *name_end;
    const char *source_start;
    const char *source_end;
    const char *path_start;
    const char *path_end;
    const char *after_string;
    const FengCodegenMapingFrameRecord *frame_record = NULL;
    FengDapJsonStringReplacement *replacements = NULL;
    size_t replacement_count = 0U;
    size_t replacement_capacity = 0U;
    char *backend_symbol = NULL;
    char *package_uri = NULL;
    char *local_path = NULL;
    bool ok = false;

    *out_json = NULL;
    if (out_frame_record != NULL) {
        *out_frame_record = NULL;
    }
    if (artifact == NULL) {
        return true;
    }

    if (proxy_json_find_object_member_loose(frame_json,
                                            frame_length,
                                            "name",
                                            &name_start,
                                            &name_end) &&
        proxy_json_parse_string_copy(name_start, name_end, &backend_symbol, &after_string) &&
        proxy_json_skip_whitespace(after_string, name_end) == name_end) {
        frame_record = proxy_find_frame_record(artifact, backend_symbol);
        if (out_frame_record != NULL) {
            *out_frame_record = frame_record;
        }
        if (frame_record != NULL &&
            frame_record->display_name != NULL &&
            strcmp(frame_record->display_name, backend_symbol) != 0) {
            char *display_name = proxy_dup_printf("%s", frame_record->display_name);

            if (display_name == NULL ||
                !proxy_insert_json_string_replacement(&replacements,
                                                      &replacement_count,
                                                      &replacement_capacity,
                                                      (size_t)(name_start - frame_json),
                                                      (size_t)(name_end - frame_json),
                                                      display_name)) {
                free(display_name);
                proxy_report_error(error_fd,
                                   "failed to rewrite stackTrace frame",
                                   "out of memory");
                goto cleanup;
            }
        }
    }
    if (proxy_json_find_object_member_loose(frame_json,
                                            frame_length,
                                            "source",
                                            &source_start,
                                            &source_end) &&
        proxy_json_find_object_member_loose(source_start,
                                            (size_t)(source_end - source_start),
                                            "path",
                                            &path_start,
                                            &path_end) &&
        proxy_json_parse_string_copy(path_start, path_end, &package_uri, &after_string) &&
        proxy_json_skip_whitespace(after_string, path_end) == path_end &&
        proxy_package_uri_to_local_path(artifact, package_uri, &local_path)) {
        if (!proxy_insert_json_string_replacement(&replacements,
                                                  &replacement_count,
                                                  &replacement_capacity,
                                                  (size_t)(path_start - frame_json),
                                                  (size_t)(path_end - frame_json),
                                                  local_path)) {
            proxy_report_error(error_fd,
                               "failed to rewrite stackTrace frame",
                               "out of memory");
            goto cleanup;
        }
        local_path = NULL;
    }
    if (!proxy_apply_json_string_replacements(frame_json,
                                              frame_length,
                                              replacements,
                                              replacement_count,
                                              out_json,
                                              error_fd,
                                              "failed to rewrite stackTrace frame")) {
        goto cleanup;
    }
    ok = true;

cleanup:
    free(backend_symbol);
    free(package_uri);
    free(local_path);
    proxy_json_string_replacements_dispose(replacements, replacement_count);
    return ok;
}

/* Rewrite stackTrace response source paths and frame names to editor-facing values. */
static bool proxy_rewrite_stack_trace_response_payload(const char *json,
                                                       size_t json_length,
                                                       const FengDebugArtifact *artifact,
                                                       char **out_json,
                                                       int error_fd) {
    const char *body_start;
    const char *body_end;
    const char *frames_start;
    const char *frames_end;
    const char *total_frames_start;
    const char *total_frames_end;
    const char *cursor;
    char *rewritten_frames = NULL;
    size_t rewritten_frames_length = 0U;
    size_t rewritten_frames_capacity = 0U;
    char *payload_with_frames = NULL;
    char *payload_with_total = NULL;
    size_t original_frame_count = 0U;
    size_t visible_frame_count = 0U;
    bool frames_changed = false;
    bool ok = false;

    *out_json = NULL;
    if (artifact == NULL) {
        return true;
    }
    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "body",
                                             &body_start,
                                             &body_end) ||
        !proxy_json_find_object_member_loose(body_start,
                                             (size_t)(body_end - body_start),
                                             "stackFrames",
                                             &frames_start,
                                             &frames_end)) {
        return true;
    }
    cursor = proxy_json_skip_whitespace(frames_start, frames_end);
    if (cursor >= frames_end || *cursor != '[') {
        return true;
    }
    if (!proxy_append_byte(&rewritten_frames,
                           &rewritten_frames_length,
                           &rewritten_frames_capacity,
                           '[')) {
        proxy_report_error(error_fd,
                           "failed to rewrite stackTrace response",
                           "out of memory");
        goto cleanup;
    }

    cursor = proxy_json_skip_whitespace(cursor + 1, frames_end);
    while (cursor < frames_end && *cursor != ']') {
        const char *frame_start = cursor;
        const char *frame_end;
        const FengCodegenMapingFrameRecord *frame_record = NULL;
        char *frame_json = NULL;
        char *rewritten_frame_json = NULL;
        const char *visible_frame_json;
        size_t frame_json_length;

        if (!proxy_json_skip_value(frame_start, frames_end, &frame_end)) {
            ok = true;
            goto cleanup;
        }
        frame_json_length = (size_t)(frame_end - frame_start);
        frame_json = proxy_dup_bytes((const unsigned char *)frame_start, frame_json_length);
        if (frame_json == NULL ||
            !proxy_rewrite_stack_trace_frame_payload(frame_json,
                                                     frame_json_length,
                                                     artifact,
                                                     &frame_record,
                                                     &rewritten_frame_json,
                                                     error_fd)) {
            free(frame_json);
            free(rewritten_frame_json);
            goto cleanup;
        }
        original_frame_count += 1U;
        if (rewritten_frame_json != NULL) {
            frames_changed = true;
        }
        if (frame_record != NULL && frame_record->policy == FENG_CODEGEN_MAPING_FRAME_HIDDEN) {
            frames_changed = true;
            free(frame_json);
            free(rewritten_frame_json);
            cursor = proxy_json_skip_whitespace(frame_end, frames_end);
            if (cursor < frames_end && *cursor == ',') {
                cursor = proxy_json_skip_whitespace(cursor + 1, frames_end);
                continue;
            }
            if (cursor < frames_end && *cursor == ']') {
                break;
            }
            ok = true;
            goto cleanup;
        }

        visible_frame_json = rewritten_frame_json != NULL ? rewritten_frame_json : frame_json;
        if (visible_frame_count > 0U &&
            !proxy_append_byte(&rewritten_frames,
                               &rewritten_frames_length,
                               &rewritten_frames_capacity,
                               ',')) {
            free(frame_json);
            free(rewritten_frame_json);
            proxy_report_error(error_fd,
                               "failed to rewrite stackTrace response",
                               "out of memory");
            goto cleanup;
        }
        if (!proxy_append_bytes(&rewritten_frames,
                                &rewritten_frames_length,
                                &rewritten_frames_capacity,
                                visible_frame_json,
                                strlen(visible_frame_json))) {
            free(frame_json);
            free(rewritten_frame_json);
            proxy_report_error(error_fd,
                               "failed to rewrite stackTrace response",
                               "out of memory");
            goto cleanup;
        }
        visible_frame_count += 1U;
        free(frame_json);
        free(rewritten_frame_json);

        cursor = proxy_json_skip_whitespace(frame_end, frames_end);
        if (cursor < frames_end && *cursor == ',') {
            cursor = proxy_json_skip_whitespace(cursor + 1, frames_end);
            continue;
        }
        if (cursor < frames_end && *cursor == ']') {
            break;
        }
        ok = true;
        goto cleanup;
    }

    if (!proxy_append_byte(&rewritten_frames,
                           &rewritten_frames_length,
                           &rewritten_frames_capacity,
                           ']') ||
        !proxy_append_byte(&rewritten_frames,
                           &rewritten_frames_length,
                           &rewritten_frames_capacity,
                           '\0')) {
        proxy_report_error(error_fd,
                           "failed to rewrite stackTrace response",
                           "out of memory");
        goto cleanup;
    }
    rewritten_frames[rewritten_frames_length - 1U] = '\0';

    if (!frames_changed) {
        ok = true;
        goto cleanup;
    }

    if (!proxy_replace_json_span(json,
                                 json_length,
                                 (size_t)(frames_start - json),
                                 (size_t)(frames_end - json),
                                 rewritten_frames,
                                 &payload_with_frames,
                                 error_fd,
                                 "failed to rewrite stackTrace response")) {
        goto cleanup;
    }

    if (proxy_json_find_object_member_loose(payload_with_frames,
                                            strlen(payload_with_frames),
                                            "body",
                                            &body_start,
                                            &body_end) &&
        proxy_json_find_object_member_loose(body_start,
                                            (size_t)(body_end - body_start),
                                            "totalFrames",
                                            &total_frames_start,
                                            &total_frames_end)) {
        uint64_t total_frames = 0U;

        if (proxy_json_parse_u64_range(total_frames_start, total_frames_end, &total_frames) &&
            total_frames == (uint64_t)original_frame_count) {
            char *visible_total = proxy_dup_printf("%zu", visible_frame_count);

            if (visible_total == NULL ||
                !proxy_replace_json_span(payload_with_frames,
                                         strlen(payload_with_frames),
                                         (size_t)(total_frames_start - payload_with_frames),
                                         (size_t)(total_frames_end - payload_with_frames),
                                         visible_total,
                                         &payload_with_total,
                                         error_fd,
                                         "failed to rewrite stackTrace response")) {
                free(visible_total);
                goto cleanup;
            }
            free(visible_total);
        }
    }

    *out_json = payload_with_total != NULL ? payload_with_total : payload_with_frames;
    payload_with_total = NULL;
    payload_with_frames = NULL;
    ok = true;

cleanup:
    free(rewritten_frames);
    free(payload_with_frames);
    free(payload_with_total);
    return ok;
}

/* Forward one client message to lldb-dap, rewriting setBreakpoints paths when needed. */
static bool proxy_process_client_relay_message(const FengDapMessage *message,
                                               int backend_stdin_fd,
                                               int output_fd,
                                               const FengDebugArtifact *artifact,
                                               uint64_t *next_seq,
                                               int error_fd) {
    char *type = NULL;
    char *command = NULL;
    char *rewritten_payload = NULL;
    char *error_detail = NULL;
    uint64_t request_seq = 0U;
    bool is_set_breakpoints = false;
    bool ok;

    if (proxy_json_get_string_member(message->payload,
                                     message->payload_length,
                                     "type",
                                     &type) &&
        strcmp(type, "request") == 0 &&
        proxy_json_get_string_member(message->payload,
                                     message->payload_length,
                                     "command",
                                     &command) &&
        strcmp(command, "setBreakpoints") == 0) {
        is_set_breakpoints = true;
    }
    if (is_set_breakpoints) {
        if (!proxy_rewrite_set_breakpoints_request_payload(message->payload,
                                                           message->payload_length,
                                                           artifact,
                                                           &rewritten_payload,
                                                           &error_detail,
                                                           error_fd)) {
            if (error_detail != NULL) {
                if (!proxy_json_get_u64_member(message->payload,
                                              message->payload_length,
                                              "seq",
                                              &request_seq)) {
                    free(type);
                    free(command);
                    free(error_detail);
                    free(rewritten_payload);
                    proxy_report_error(error_fd,
                                       "failed to rewrite setBreakpoints path",
                                       "missing request sequence");
                    return false;
                }
                ok = proxy_send_request_failure_response(output_fd,
                                                         command,
                                                         request_seq,
                                                         error_detail,
                                                         next_seq,
                                                         error_fd);
                free(type);
                free(command);
                free(error_detail);
                free(rewritten_payload);
                return ok;
            }
            free(type);
            free(command);
            return false;
        }
        if (rewritten_payload != NULL) {
            ok = proxy_write_message(backend_stdin_fd,
                                     rewritten_payload,
                                     error_fd,
                                     "failed to forward rewritten setBreakpoints request to lldb-dap");
            free(type);
            free(command);
            free(error_detail);
            free(rewritten_payload);
            return ok;
        }
    }

    ok = proxy_write_framed_message(backend_stdin_fd,
                                    message,
                                    error_fd,
                                    "failed to forward DAP request to lldb-dap");
    free(type);
    free(command);
    free(error_detail);
    free(rewritten_payload);
    return ok;
}

/* Forward one backend message to the editor, rewriting stackTrace source paths when needed. */
static bool proxy_process_backend_relay_message(const FengDapMessage *message,
                                                int output_fd,
                                                const FengDebugArtifact *artifact,
                                                int error_fd) {
    char *type = NULL;
    char *command = NULL;
    char *rewritten_payload = NULL;
    bool ok;

    if (proxy_json_get_string_member(message->payload,
                                     message->payload_length,
                                     "type",
                                     &type) &&
        strcmp(type, "response") == 0 &&
        proxy_json_get_string_member(message->payload,
                                     message->payload_length,
                                     "command",
                                     &command) &&
        strcmp(command, "stackTrace") == 0) {
        if (!proxy_rewrite_stack_trace_response_payload(message->payload,
                                                        message->payload_length,
                                                        artifact,
                                                        &rewritten_payload,
                                                        error_fd)) {
            free(type);
            free(command);
            return false;
        }
        if (rewritten_payload != NULL) {
            ok = proxy_write_message(output_fd,
                                     rewritten_payload,
                                     error_fd,
                                     "failed to forward rewritten stackTrace response to editor");
            free(type);
            free(command);
            free(rewritten_payload);
            return ok;
        }
    }

    ok = proxy_write_framed_message(output_fd,
                                    message,
                                    error_fd,
                                    "failed to write DAP message to editor");
    free(type);
    free(command);
    free(rewritten_payload);
    return ok;
}

/* Relay DAP messages bidirectionally after launch, with request/response rewriting hooks. */
static bool proxy_relay_messages(FengDapMessageReader *client_reader,
                                 FengDapMessageReader *backend_reader,
                                 int backend_stdin_fd,
                                 int output_fd,
                                 const FengDebugArtifact *artifact,
                                 uint64_t *next_seq,
                                 int error_fd) {
    bool backend_stdin_closed = false;

    for (;;) {
        FengDapMessage message = {0};
        FengDapReadStatus client_status;
        FengDapReadStatus backend_status;

        client_status = proxy_reader_try_read_buffered_message(client_reader,
                                                               &message,
                                                               error_fd);
        if (client_status == FENG_DAP_READ_OK) {
            bool ok = proxy_process_client_relay_message(&message,
                                                         backend_stdin_fd,
                                                         output_fd,
                                                         artifact,
                                                         next_seq,
                                                         error_fd);

            proxy_message_dispose(&message);
            if (!ok) {
                if (!backend_stdin_closed) {
                    close(backend_stdin_fd);
                }
                return false;
            }
            continue;
        }
        if (client_status == FENG_DAP_READ_ERROR) {
            if (!backend_stdin_closed) {
                close(backend_stdin_fd);
            }
            return false;
        }
        if (client_status == FENG_DAP_READ_EOF && !backend_stdin_closed) {
            close(backend_stdin_fd);
            backend_stdin_closed = true;
        }

        backend_status = proxy_reader_try_read_buffered_message(backend_reader,
                                                                &message,
                                                                error_fd);
        if (backend_status == FENG_DAP_READ_OK) {
            bool ok = proxy_process_backend_relay_message(&message,
                                                          output_fd,
                                                          artifact,
                                                          error_fd);

            proxy_message_dispose(&message);
            if (!ok) {
                return false;
            }
            continue;
        }
        if (backend_status == FENG_DAP_READ_ERROR) {
            return false;
        }

        if (client_status == FENG_DAP_READ_EOF && backend_status == FENG_DAP_READ_EOF) {
            break;
        }

        {
            struct pollfd poll_fds[2];
            int client_slot = -1;
            int backend_slot = -1;
            nfds_t poll_count = 0U;
            int poll_rc;

            if (client_status == FENG_DAP_READ_PENDING) {
                client_slot = (int)poll_count;
                poll_fds[poll_count].fd = client_reader->fd;
                poll_fds[poll_count].events = POLLIN | POLLHUP;
                poll_fds[poll_count].revents = 0;
                poll_count += 1U;
            }
            if (backend_status == FENG_DAP_READ_PENDING) {
                backend_slot = (int)poll_count;
                poll_fds[poll_count].fd = backend_reader->fd;
                poll_fds[poll_count].events = POLLIN | POLLHUP;
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
                proxy_report_error(error_fd, "failed to poll DAP messages", strerror(errno));
                return false;
            }

            if (client_slot >= 0 && (poll_fds[client_slot].revents & (POLLIN | POLLHUP)) != 0) {
                if (!proxy_reader_fill(client_reader, error_fd)) {
                    return false;
                }
            }
            if (backend_slot >= 0 && (poll_fds[backend_slot].revents & (POLLIN | POLLHUP)) != 0) {
                if (!proxy_reader_fill(backend_reader, error_fd)) {
                    return false;
                }
            }
        }
    }

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
    FengDebugArtifact launch_artifact = {0};
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
            feng_debug_artifact_dispose(&launch_artifact);
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
                feng_debug_artifact_dispose(&launch_artifact);
                return 1;
            }
            if (!proxy_send_initialize_response(output_fd,
                                                request_seq,
                                                &next_seq,
                                                error_fd)) {
                proxy_message_dispose(&request);
                proxy_reader_dispose(&client_reader);
                proxy_reader_dispose(&backend_reader);
                feng_debug_artifact_dispose(&launch_artifact);
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
            feng_debug_artifact_dispose(&launch_artifact);
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
            feng_debug_artifact_dispose(&launch_artifact);
            return 1;
        }
        if (!proxy_validate_launch_request(&request, &launch_artifact, &error_detail)) {
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
            feng_debug_artifact_dispose(&launch_artifact);
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
            feng_debug_artifact_dispose(&launch_artifact);
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
            feng_debug_artifact_dispose(&launch_artifact);
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
            feng_debug_artifact_dispose(&launch_artifact);
            return exit_code != 0 ? exit_code : 1;
        }

        proxy_message_dispose(&initialize_request);
        proxy_message_dispose(&request);
        if (!proxy_relay_messages(&client_reader,
                                  &backend_reader,
                                  child_stdin[1],
                                  output_fd,
                                  &launch_artifact,
                                  &next_seq,
                                  error_fd)) {
            close(child_stdout[0]);
            exit_code = proxy_wait_for_child(child, error_fd, backend_program);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            feng_debug_artifact_dispose(&launch_artifact);
            return exit_code != 0 ? exit_code : 1;
        }
        close(child_stdout[0]);
        exit_code = proxy_wait_for_child(child, error_fd, backend_program);
        proxy_reader_dispose(&client_reader);
        proxy_reader_dispose(&backend_reader);
        feng_debug_artifact_dispose(&launch_artifact);
        return exit_code;
    }

    proxy_message_dispose(&initialize_request);
    proxy_reader_dispose(&client_reader);
    proxy_reader_dispose(&backend_reader);
    feng_debug_artifact_dispose(&launch_artifact);
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
