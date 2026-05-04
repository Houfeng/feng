#include "cli/lsp/server.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cli/lsp/runtime.h"


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

int feng_lsp_server_run(FILE *input, FILE *output, FILE *errors) {
    FengLspRuntime *runtime = feng_lsp_runtime_create();

    if (runtime == NULL) {
        fprintf(errors, "lsp runtime error: out of memory\n");
        return 1;
    }

    while (!feng_lsp_runtime_should_exit(runtime)) {
        bool saw_header = false;
        bool has_length = false;
        size_t content_length = 0U;

        while (true) {
            char *line = NULL;
            int status = read_header_line(input, &line);

            if (status == 0) {
                if (!saw_header) {
                    int exit_code = feng_lsp_runtime_exit_code(runtime);

                    feng_lsp_runtime_free(runtime);
                    return exit_code;
                }
                fprintf(errors, "lsp protocol error: unexpected EOF while reading headers\n");
                feng_lsp_runtime_free(runtime);
                return 1;
            }
            if (status < 0) {
                fprintf(errors, "lsp protocol error: failed to read message headers\n");
                free(line);
                feng_lsp_runtime_free(runtime);
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
                feng_lsp_runtime_free(runtime);
                return 1;
            }
            if (!feng_lsp_runtime_handle_payload(runtime,
                                                 output,
                                                 payload,
                                                 content_length,
                                                 errors)) {
                free(payload);
                feng_lsp_runtime_free(runtime);
                return 1;
            }
            free(payload);
        }
    }

    {
        int exit_code = feng_lsp_runtime_exit_code(runtime);

        feng_lsp_runtime_free(runtime);
        return exit_code;
    }
}
