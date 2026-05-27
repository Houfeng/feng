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
#define FENG_DAP_PROXY_INTERNAL_REQUEST_SEQ_BASE UINT64_C(1000000)
#define FENG_DAP_SYNTHETIC_REF_BASE UINT64_C(0x40000000)
#define PROXY_IS_SYNTHETIC_REF(ref) ((ref) >= FENG_DAP_SYNTHETIC_REF_BASE)
#define FENG_DAP_SYNTHETIC_ARRAY_ELEMENT_LIMIT 256U
#define FENG_DAP_SYNTHETIC_MAX_DEPTH 3U

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

/* Tracks one visible stack frame id to backend symbol binding. */
typedef struct FengDapFrameBinding {
    uint64_t frame_id;
    char *backend_symbol;
} FengDapFrameBinding;

/* Remembers one in-flight scopes request until its response arrives. */
typedef struct FengDapPendingScopeRequest {
    uint64_t request_seq;
    uint64_t frame_id;
} FengDapPendingScopeRequest;

/* Maps one scope variablesReference back to the owning frame id. */
typedef struct FengDapScopeBinding {
    uint64_t variables_reference;
    uint64_t frame_id;
} FengDapScopeBinding;

/* Remembers one in-flight variables request until its response arrives. */
typedef struct FengDapPendingVariablesRequest {
    uint64_t request_seq;
    uint64_t variables_reference;
} FengDapPendingVariablesRequest;

/* One internally-evaluated user variable value override. */
typedef struct FengDapEvaluatedVariable {
    char *result;
    char *type;
    uint64_t variables_reference;
    bool has_result;
    bool has_type;
    bool has_variables_reference;
} FengDapEvaluatedVariable;

typedef enum FengDapSyntheticRefKind {
    FENG_DAP_SYNTHETIC_ARRAY = 1,
    FENG_DAP_SYNTHETIC_TYPE = 2,
} FengDapSyntheticRefKind;

/* One proxy-owned variablesReference that expands Feng semantic children. */
typedef struct FengDapSyntheticRef {
    uint64_t ref_id;
    FengDapSyntheticRefKind kind;
    uint64_t frame_id;
    char *parent_read_expr;
    char *element_display_type;
    char *type_display_name;
    uint64_t element_count;
    unsigned depth;
} FengDapSyntheticRef;

/* Session-scoped relay state used by stack/variables rewrites. */
typedef struct FengDapRelayState {
    FengDapFrameBinding *frame_bindings;
    size_t frame_binding_count;
    size_t frame_binding_capacity;
    FengDapPendingScopeRequest *pending_scope_requests;
    size_t pending_scope_request_count;
    size_t pending_scope_request_capacity;
    FengDapScopeBinding *scope_bindings;
    size_t scope_binding_count;
    size_t scope_binding_capacity;
    FengDapPendingVariablesRequest *pending_variables_requests;
    size_t pending_variables_request_count;
    size_t pending_variables_request_capacity;
    FengDapSyntheticRef *synthetic_refs;
    size_t synthetic_ref_count;
    size_t synthetic_ref_capacity;
    uint64_t next_synthetic_ref;
    uint64_t next_internal_request_seq;
} FengDapRelayState;

static const char *proxy_json_skip_whitespace(const char *cursor, const char *end);
static char *proxy_dup_printf(const char *fmt, ...);
static bool proxy_process_backend_relay_message(const FengDapMessage *message,
                                                FengDapMessageReader *backend_reader,
                                                int backend_stdin_fd,
                                                int output_fd,
                                                const FengDebugArtifact *artifact,
                                                FengDapRelayState *state,
                                                int error_fd);

static void proxy_evaluated_variable_dispose(FengDapEvaluatedVariable *value) {
    if (value == NULL) {
        return;
    }
    free(value->result);
    free(value->type);
    memset(value, 0, sizeof(*value));
}

static void proxy_synthetic_ref_dispose(FengDapSyntheticRef *ref) {
    if (ref == NULL) {
        return;
    }
    free(ref->parent_read_expr);
    free(ref->element_display_type);
    free(ref->type_display_name);
    memset(ref, 0, sizeof(*ref));
}

static void proxy_relay_state_dispose(FengDapRelayState *state) {
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0U; index < state->frame_binding_count; ++index) {
        free(state->frame_bindings[index].backend_symbol);
    }
    for (index = 0U; index < state->synthetic_ref_count; ++index) {
        proxy_synthetic_ref_dispose(&state->synthetic_refs[index]);
    }
    free(state->frame_bindings);
    free(state->pending_scope_requests);
    free(state->scope_bindings);
    free(state->pending_variables_requests);
    free(state->synthetic_refs);
    memset(state, 0, sizeof(*state));
}

static void proxy_relay_state_clear_stopped_context(FengDapRelayState *state) {
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0U; index < state->frame_binding_count; ++index) {
        free(state->frame_bindings[index].backend_symbol);
        state->frame_bindings[index].backend_symbol = NULL;
    }
    for (index = 0U; index < state->synthetic_ref_count; ++index) {
        proxy_synthetic_ref_dispose(&state->synthetic_refs[index]);
    }
    state->frame_binding_count = 0U;
    state->pending_scope_request_count = 0U;
    state->scope_binding_count = 0U;
    state->pending_variables_request_count = 0U;
    state->synthetic_ref_count = 0U;
}

static bool proxy_command_resumes_execution(const char *command) {
    if (command == NULL) {
        return false;
    }
    return strcmp(command, "continue") == 0 ||
           strcmp(command, "next") == 0 ||
           strcmp(command, "stepIn") == 0 ||
           strcmp(command, "stepOut") == 0 ||
           strcmp(command, "stepBack") == 0 ||
           strcmp(command, "reverseContinue") == 0 ||
           strcmp(command, "restartFrame") == 0 ||
           strcmp(command, "goto") == 0;
}

static bool proxy_relay_state_set_frame_binding(FengDapRelayState *state,
                                                uint64_t frame_id,
                                                const char *backend_symbol) {
    FengDapFrameBinding *grown;
    size_t new_capacity;
    size_t index;
    char *backend_symbol_copy;

    if (state == NULL || backend_symbol == NULL) {
        return true;
    }
    for (index = 0U; index < state->frame_binding_count; ++index) {
        if (state->frame_bindings[index].frame_id == frame_id) {
            backend_symbol_copy = proxy_dup_printf("%s", backend_symbol);
            if (backend_symbol_copy == NULL) {
                return false;
            }
            free(state->frame_bindings[index].backend_symbol);
            state->frame_bindings[index].backend_symbol = backend_symbol_copy;
            return true;
        }
    }
    if (state->frame_binding_count == state->frame_binding_capacity) {
        new_capacity = state->frame_binding_capacity == 0U ? 8U : state->frame_binding_capacity * 2U;
        grown = (FengDapFrameBinding *)realloc(state->frame_bindings,
                                               new_capacity * sizeof(*state->frame_bindings));
        if (grown == NULL) {
            return false;
        }
        state->frame_bindings = grown;
        state->frame_binding_capacity = new_capacity;
    }
    backend_symbol_copy = proxy_dup_printf("%s", backend_symbol);
    if (backend_symbol_copy == NULL) {
        return false;
    }
    state->frame_bindings[state->frame_binding_count].frame_id = frame_id;
    state->frame_bindings[state->frame_binding_count].backend_symbol = backend_symbol_copy;
    state->frame_binding_count += 1U;
    return true;
}

static const char *proxy_relay_state_find_frame_binding(const FengDapRelayState *state,
                                                        uint64_t frame_id) {
    size_t index;

    if (state == NULL) {
        return NULL;
    }
    for (index = 0U; index < state->frame_binding_count; ++index) {
        if (state->frame_bindings[index].frame_id == frame_id) {
            return state->frame_bindings[index].backend_symbol;
        }
    }
    return NULL;
}

static bool proxy_relay_state_record_pending_scope_request(FengDapRelayState *state,
                                                           uint64_t request_seq,
                                                           uint64_t frame_id) {
    FengDapPendingScopeRequest *grown;
    size_t new_capacity;
    size_t index;

    if (state == NULL) {
        return true;
    }
    for (index = 0U; index < state->pending_scope_request_count; ++index) {
        if (state->pending_scope_requests[index].request_seq == request_seq) {
            state->pending_scope_requests[index].frame_id = frame_id;
            return true;
        }
    }
    if (state->pending_scope_request_count == state->pending_scope_request_capacity) {
        new_capacity = state->pending_scope_request_capacity == 0U
                           ? 8U
                           : state->pending_scope_request_capacity * 2U;
        grown = (FengDapPendingScopeRequest *)realloc(
            state->pending_scope_requests,
            new_capacity * sizeof(*state->pending_scope_requests));
        if (grown == NULL) {
            return false;
        }
        state->pending_scope_requests = grown;
        state->pending_scope_request_capacity = new_capacity;
    }
    state->pending_scope_requests[state->pending_scope_request_count].request_seq = request_seq;
    state->pending_scope_requests[state->pending_scope_request_count].frame_id = frame_id;
    state->pending_scope_request_count += 1U;
    return true;
}

static bool proxy_relay_state_take_pending_scope_request(FengDapRelayState *state,
                                                         uint64_t request_seq,
                                                         uint64_t *out_frame_id) {
    size_t index;

    if (state == NULL || out_frame_id == NULL) {
        return false;
    }
    for (index = 0U; index < state->pending_scope_request_count; ++index) {
        if (state->pending_scope_requests[index].request_seq == request_seq) {
            *out_frame_id = state->pending_scope_requests[index].frame_id;
            state->pending_scope_request_count -= 1U;
            if (index < state->pending_scope_request_count) {
                state->pending_scope_requests[index] =
                    state->pending_scope_requests[state->pending_scope_request_count];
            }
            return true;
        }
    }
    return false;
}

static bool proxy_relay_state_set_scope_binding(FengDapRelayState *state,
                                                uint64_t variables_reference,
                                                uint64_t frame_id) {
    FengDapScopeBinding *grown;
    size_t new_capacity;
    size_t index;

    if (state == NULL) {
        return true;
    }
    for (index = 0U; index < state->scope_binding_count; ++index) {
        if (state->scope_bindings[index].variables_reference == variables_reference) {
            state->scope_bindings[index].frame_id = frame_id;
            return true;
        }
    }
    if (state->scope_binding_count == state->scope_binding_capacity) {
        new_capacity = state->scope_binding_capacity == 0U ? 8U : state->scope_binding_capacity * 2U;
        grown = (FengDapScopeBinding *)realloc(state->scope_bindings,
                                               new_capacity * sizeof(*state->scope_bindings));
        if (grown == NULL) {
            return false;
        }
        state->scope_bindings = grown;
        state->scope_binding_capacity = new_capacity;
    }
    state->scope_bindings[state->scope_binding_count].variables_reference = variables_reference;
    state->scope_bindings[state->scope_binding_count].frame_id = frame_id;
    state->scope_binding_count += 1U;
    return true;
}

static bool proxy_relay_state_find_scope_binding(const FengDapRelayState *state,
                                                 uint64_t variables_reference,
                                                 uint64_t *out_frame_id) {
    size_t index;

    if (state == NULL || out_frame_id == NULL) {
        return false;
    }
    for (index = 0U; index < state->scope_binding_count; ++index) {
        if (state->scope_bindings[index].variables_reference == variables_reference) {
            *out_frame_id = state->scope_bindings[index].frame_id;
            return true;
        }
    }
    return false;
}

static bool proxy_relay_state_record_pending_variables_request(FengDapRelayState *state,
                                                               uint64_t request_seq,
                                                               uint64_t variables_reference) {
    FengDapPendingVariablesRequest *grown;
    size_t new_capacity;
    size_t index;

    if (state == NULL) {
        return true;
    }
    for (index = 0U; index < state->pending_variables_request_count; ++index) {
        if (state->pending_variables_requests[index].request_seq == request_seq) {
            state->pending_variables_requests[index].variables_reference = variables_reference;
            return true;
        }
    }
    if (state->pending_variables_request_count == state->pending_variables_request_capacity) {
        new_capacity = state->pending_variables_request_capacity == 0U
                           ? 8U
                           : state->pending_variables_request_capacity * 2U;
        grown = (FengDapPendingVariablesRequest *)realloc(
            state->pending_variables_requests,
            new_capacity * sizeof(*state->pending_variables_requests));
        if (grown == NULL) {
            return false;
        }
        state->pending_variables_requests = grown;
        state->pending_variables_request_capacity = new_capacity;
    }
    state->pending_variables_requests[state->pending_variables_request_count].request_seq = request_seq;
    state->pending_variables_requests[state->pending_variables_request_count].variables_reference = variables_reference;
    state->pending_variables_request_count += 1U;
    return true;
}

static bool proxy_relay_state_take_pending_variables_request(FengDapRelayState *state,
                                                             uint64_t request_seq,
                                                             uint64_t *out_variables_reference) {
    size_t index;

    if (state == NULL || out_variables_reference == NULL) {
        return false;
    }
    for (index = 0U; index < state->pending_variables_request_count; ++index) {
        if (state->pending_variables_requests[index].request_seq == request_seq) {
            *out_variables_reference = state->pending_variables_requests[index].variables_reference;
            state->pending_variables_request_count -= 1U;
            if (index < state->pending_variables_request_count) {
                state->pending_variables_requests[index] =
                    state->pending_variables_requests[state->pending_variables_request_count];
            }
            return true;
        }
    }
    return false;
}

static uint64_t proxy_relay_state_next_internal_request_seq(FengDapRelayState *state) {
    uint64_t next_seq;

    if (state == NULL) {
        return FENG_DAP_PROXY_INTERNAL_REQUEST_SEQ_BASE;
    }
    if (state->next_internal_request_seq < FENG_DAP_PROXY_INTERNAL_REQUEST_SEQ_BASE) {
        state->next_internal_request_seq = FENG_DAP_PROXY_INTERNAL_REQUEST_SEQ_BASE;
    }
    next_seq = state->next_internal_request_seq;
    state->next_internal_request_seq += 1U;
    if (state->next_internal_request_seq == 0U) {
        state->next_internal_request_seq = FENG_DAP_PROXY_INTERNAL_REQUEST_SEQ_BASE;
    }
    return next_seq;
}

static bool proxy_relay_state_append_synthetic_ref(FengDapRelayState *state,
                                                   const FengDapSyntheticRef *ref) {
    FengDapSyntheticRef *grown;
    size_t new_capacity;

    if (state == NULL || ref == NULL) {
        return false;
    }
    if (state->synthetic_ref_count == state->synthetic_ref_capacity) {
        new_capacity = state->synthetic_ref_capacity == 0U ? 8U : state->synthetic_ref_capacity * 2U;
        grown = (FengDapSyntheticRef *)realloc(state->synthetic_refs,
                                              new_capacity * sizeof(*state->synthetic_refs));
        if (grown == NULL) {
            return false;
        }
        state->synthetic_refs = grown;
        state->synthetic_ref_capacity = new_capacity;
    }
    state->synthetic_refs[state->synthetic_ref_count] = *ref;
    state->synthetic_ref_count += 1U;
    return true;
}

static bool proxy_relay_state_register_synthetic_array(FengDapRelayState *state,
                                                       uint64_t frame_id,
                                                       const char *parent_read_expr,
                                                       const char *element_display_type,
                                                       uint64_t element_count,
                                                       unsigned depth,
                                                       uint64_t *out_ref_id) {
    FengDapSyntheticRef ref;

    if (out_ref_id != NULL) {
        *out_ref_id = 0U;
    }
    if (state == NULL || parent_read_expr == NULL || parent_read_expr[0] == '\0' ||
        element_display_type == NULL || element_display_type[0] == '\0' || out_ref_id == NULL) {
        return false;
    }
    memset(&ref, 0, sizeof(ref));
    if (state->next_synthetic_ref < FENG_DAP_SYNTHETIC_REF_BASE) {
        state->next_synthetic_ref = FENG_DAP_SYNTHETIC_REF_BASE;
    }
    ref.ref_id = state->next_synthetic_ref;
    state->next_synthetic_ref += 1U;
    if (state->next_synthetic_ref < FENG_DAP_SYNTHETIC_REF_BASE) {
        state->next_synthetic_ref = FENG_DAP_SYNTHETIC_REF_BASE;
    }
    ref.kind = FENG_DAP_SYNTHETIC_ARRAY;
    ref.frame_id = frame_id;
    ref.parent_read_expr = proxy_dup_printf("%s", parent_read_expr);
    ref.element_display_type = proxy_dup_printf("%s", element_display_type);
    ref.element_count = element_count;
    ref.depth = depth;
    if (ref.parent_read_expr == NULL || ref.element_display_type == NULL ||
        !proxy_relay_state_append_synthetic_ref(state, &ref)) {
        proxy_synthetic_ref_dispose(&ref);
        return false;
    }
    *out_ref_id = ref.ref_id;
    return true;
}

static bool proxy_relay_state_register_synthetic_type(FengDapRelayState *state,
                                                      uint64_t frame_id,
                                                      const char *parent_read_expr,
                                                      const char *type_display_name,
                                                      unsigned depth,
                                                      uint64_t *out_ref_id) {
    FengDapSyntheticRef ref;

    if (out_ref_id != NULL) {
        *out_ref_id = 0U;
    }
    if (state == NULL || parent_read_expr == NULL || parent_read_expr[0] == '\0' ||
        type_display_name == NULL || type_display_name[0] == '\0' || out_ref_id == NULL) {
        return false;
    }
    memset(&ref, 0, sizeof(ref));
    if (state->next_synthetic_ref < FENG_DAP_SYNTHETIC_REF_BASE) {
        state->next_synthetic_ref = FENG_DAP_SYNTHETIC_REF_BASE;
    }
    ref.ref_id = state->next_synthetic_ref;
    state->next_synthetic_ref += 1U;
    if (state->next_synthetic_ref < FENG_DAP_SYNTHETIC_REF_BASE) {
        state->next_synthetic_ref = FENG_DAP_SYNTHETIC_REF_BASE;
    }
    ref.kind = FENG_DAP_SYNTHETIC_TYPE;
    ref.frame_id = frame_id;
    ref.parent_read_expr = proxy_dup_printf("%s", parent_read_expr);
    ref.type_display_name = proxy_dup_printf("%s", type_display_name);
    ref.depth = depth;
    if (ref.parent_read_expr == NULL || ref.type_display_name == NULL ||
        !proxy_relay_state_append_synthetic_ref(state, &ref)) {
        proxy_synthetic_ref_dispose(&ref);
        return false;
    }
    *out_ref_id = ref.ref_id;
    return true;
}

static const FengDapSyntheticRef *proxy_relay_state_find_synthetic_ref(const FengDapRelayState *state,
                                                                       uint64_t ref_id) {
    size_t index;

    if (state == NULL || !PROXY_IS_SYNTHETIC_REF(ref_id)) {
        return NULL;
    }
    for (index = 0U; index < state->synthetic_ref_count; ++index) {
        if (state->synthetic_refs[index].ref_id == ref_id) {
            return &state->synthetic_refs[index];
        }
    }
    return NULL;
}

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

/* Opportunistically pull already-ready backend bytes without blocking. */
static bool proxy_reader_fill_if_ready(FengDapMessageReader *reader, int error_fd) {
    struct pollfd pfd;
    int poll_rc;

    if (reader == NULL || reader->fd < 0 || reader->reached_eof) {
        return true;
    }
    pfd.fd = reader->fd;
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;
    poll_rc = poll(&pfd, 1U, 0);
    if (poll_rc < 0) {
        if (errno == EINTR) {
            return true;
        }
        proxy_report_error(error_fd, "failed to poll DAP reader", strerror(errno));
        return false;
    }
    if (poll_rc == 0 || (pfd.revents & (POLLIN | POLLHUP)) == 0) {
        return true;
    }
    return proxy_reader_fill(reader, error_fd);
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
    if (scheme_separator != NULL && scheme_separator != uri && scheme_separator[3] != '\0') {
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

    for (size_t index = 0U; index < artifact->package_count; ++index) {
        const char *known_package_name = artifact->packages[index].package_name;
        size_t package_name_length;
        const char *match;

        if (known_package_name == NULL || known_package_name[0] == '\0') {
            continue;
        }

        package_name_length = strlen(known_package_name);
        match = strstr(uri, known_package_name);
        while (match != NULL) {
            if ((match == uri || match[-1] == '/') &&
                strncmp(match + package_name_length, ":/", 2U) == 0 &&
                match[package_name_length + 2U] != '\0') {
                local_path = proxy_path_join(artifact->packages[index].local_root_path,
                                             match + package_name_length + 2U);
                if (local_path == NULL) {
                    return false;
                }
                *out_local_path = local_path;
                return true;
            }
            match = strstr(match + package_name_length, known_package_name);
        }
    }

    return false;
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

/* Find one variable mapping by frame backend symbol and backend variable name. */
static const FengCodegenMapingVariableRecord *proxy_find_variable_record(
    const FengDebugArtifact *artifact,
    const char *frame_backend_symbol,
    const char *backend_name) {
    size_t index;

    if (artifact == NULL || frame_backend_symbol == NULL || backend_name == NULL) {
        return NULL;
    }
    for (index = 0U; index < artifact->info.variable_count; ++index) {
        const FengCodegenMapingVariableRecord *record = artifact->info.variables + index;

        if (record->backend_name == NULL) {
            continue;
        }
        if (strcmp(record->frame_backend_symbol, frame_backend_symbol) == 0 &&
            strcmp(record->backend_name, backend_name) == 0) {
            return record;
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

/* Parse one loose string member from the current JSON object. */
static bool proxy_json_get_string_member_loose(const char *json,
                                               size_t json_length,
                                               const char *key,
                                               char **out_value) {
    const char *value_start;
    const char *value_end;
    const char *after_string;

    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             key,
                                             &value_start,
                                             &value_end) ||
        !proxy_json_parse_string_copy(value_start, value_end, out_value, &after_string)) {
        return false;
    }
    return true;
}

/* Parse one loose unsigned integer member from the current JSON object. */
static bool proxy_json_get_u64_member_loose(const char *json,
                                            size_t json_length,
                                            const char *key,
                                            uint64_t *out_value) {
    const char *value_start;
    const char *value_end;

    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             key,
                                             &value_start,
                                             &value_end)) {
        return false;
    }
    return proxy_json_parse_u64_range(value_start, value_end, out_value);
}

/* Parse one unsigned integer request argument from the top-level DAP payload. */
static bool proxy_json_get_request_argument_u64_member(const char *json,
                                                       size_t json_length,
                                                       const char *key,
                                                       uint64_t *out_value) {
    const char *arguments_start;
    const char *arguments_end;
    const char *value_start;
    const char *value_end;

    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "arguments",
                                             &arguments_start,
                                             &arguments_end) ||
        !proxy_json_find_object_member_loose(arguments_start,
                                             (size_t)(arguments_end - arguments_start),
                                             key,
                                             &value_start,
                                             &value_end)) {
        return false;
    }
    return proxy_json_parse_u64_range(value_start, value_end, out_value);
}

/* Locate one string request argument from the top-level DAP payload. */
static bool proxy_json_get_request_argument_string_range(const char *json,
                                                         size_t json_length,
                                                         const char *key,
                                                         const char **out_value_start,
                                                         const char **out_value_end) {
    const char *arguments_start;
    const char *arguments_end;

    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "arguments",
                                             &arguments_start,
                                             &arguments_end)) {
        return false;
    }
    return proxy_json_find_object_member_loose(arguments_start,
                                               (size_t)(arguments_end - arguments_start),
                                               key,
                                               out_value_start,
                                               out_value_end);
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

/* Recognize one plain identifier-shaped token. */
static bool proxy_is_identifier_expression(const char *expression) {
    size_t index;

    if (expression == NULL || expression[0] == '\0') {
        return false;
    }
    if (!(isalpha((unsigned char)expression[0]) || expression[0] == '_')) {
        return false;
    }
    for (index = 1U; expression[index] != '\0'; ++index) {
        if (!(isalnum((unsigned char)expression[index]) || expression[index] == '_')) {
            return false;
        }
    }
    return true;
}

/* Replace one owned error-detail string with a formatted message. */
static bool proxy_set_error_detail_printf(char **out_error_detail, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *detail;

    if (out_error_detail == NULL) {
        return false;
    }
    free(*out_error_detail);
    *out_error_detail = NULL;
    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return false;
    }
    detail = (char *)malloc((size_t)needed + 1U);
    if (detail == NULL) {
        va_end(args_copy);
        return false;
    }
    vsnprintf(detail, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    *out_error_detail = detail;
    return true;
}

/* Parser state for the supported read-only Feng watch subset. */
typedef struct FengDapEvaluateParser {
    const FengDebugArtifact *artifact;
    const char *frame_backend_symbol;
    const char *expression;
    size_t length;
    size_t position;
} FengDapEvaluateParser;

static bool proxy_resolve_evaluate_identifier(const FengDebugArtifact *artifact,
                                              const char *frame_backend_symbol,
                                              const char *identifier,
                                              char **out_backend_expression,
                                              char **out_error_detail);

static bool proxy_parse_evaluate_comparison(FengDapEvaluateParser *parser,
                                            char **buffer,
                                            size_t *length,
                                            size_t *capacity,
                                            char **out_error_detail);

/* Skip insignificant whitespace in one decoded evaluate expression. */
static void proxy_evaluate_skip_whitespace(FengDapEvaluateParser *parser) {
    while (parser->position < parser->length &&
           isspace((unsigned char)parser->expression[parser->position])) {
        parser->position += 1U;
    }
}

/* Append one zero-terminated string to a dynamic byte buffer. */
static bool proxy_append_cstr(char **buffer,
                              size_t *length,
                              size_t *capacity,
                              const char *text) {
    if (text == NULL) {
        return true;
    }
    return proxy_append_bytes(buffer, length, capacity, text, strlen(text));
}

/* Append one resolved backend leaf expression, wrapping complex carriers safely. */
static bool proxy_append_evaluate_leaf_expression(char **buffer,
                                                  size_t *length,
                                                  size_t *capacity,
                                                  const char *backend_expression) {
    if (backend_expression == NULL) {
        return false;
    }
    if (proxy_is_identifier_expression(backend_expression)) {
        return proxy_append_cstr(buffer, length, capacity, backend_expression);
    }
    return proxy_append_byte(buffer, length, capacity, '(') &&
           proxy_append_cstr(buffer, length, capacity, backend_expression) &&
           proxy_append_byte(buffer, length, capacity, ')');
}

/* Parse one identifier token from the current evaluate cursor. */
static bool proxy_parse_evaluate_identifier_token(FengDapEvaluateParser *parser,
                                                  char **out_identifier,
                                                  char **out_error_detail,
                                                  const char *error_message) {
    size_t start;

    *out_identifier = NULL;
    proxy_evaluate_skip_whitespace(parser);
    if (parser->position >= parser->length ||
        !(isalpha((unsigned char)parser->expression[parser->position]) ||
          parser->expression[parser->position] == '_')) {
        proxy_set_error_detail_printf(out_error_detail, "%s", error_message);
        return false;
    }
    start = parser->position;
    parser->position += 1U;
    while (parser->position < parser->length &&
           (isalnum((unsigned char)parser->expression[parser->position]) ||
            parser->expression[parser->position] == '_')) {
        parser->position += 1U;
    }
    *out_identifier = proxy_dup_bytes((const unsigned char *)(parser->expression + start),
                                      parser->position - start);
    if (*out_identifier == NULL) {
        proxy_set_error_detail_printf(out_error_detail,
                                      "out of memory rewriting evaluate expression");
        return false;
    }
    return true;
}

/* Parse one decimal integer literal token from the current evaluate cursor. */
static bool proxy_parse_evaluate_integer_literal(FengDapEvaluateParser *parser,
                                                 bool allow_sign,
                                                 char **buffer,
                                                 size_t *length,
                                                 size_t *capacity,
                                                 char **out_error_detail,
                                                 const char *error_message) {
    size_t start;

    proxy_evaluate_skip_whitespace(parser);
    if (allow_sign && parser->position < parser->length &&
        (parser->expression[parser->position] == '+' ||
         parser->expression[parser->position] == '-')) {
        if (!proxy_append_byte(buffer,
                               length,
                               capacity,
                               (unsigned char)parser->expression[parser->position])) {
            proxy_set_error_detail_printf(out_error_detail,
                                          "out of memory rewriting evaluate expression");
            return false;
        }
        parser->position += 1U;
    }
    start = parser->position;
    while (parser->position < parser->length &&
           isdigit((unsigned char)parser->expression[parser->position])) {
        parser->position += 1U;
    }
    if (start == parser->position) {
        proxy_set_error_detail_printf(out_error_detail, "%s", error_message);
        return false;
    }
    if (!proxy_append_bytes(buffer,
                            length,
                            capacity,
                            parser->expression + start,
                            parser->position - start)) {
        proxy_set_error_detail_printf(out_error_detail,
                                      "out of memory rewriting evaluate expression");
        return false;
    }
    return true;
}

/* Parse one primary evaluate atom before postfix operators. */
static bool proxy_parse_evaluate_primary(FengDapEvaluateParser *parser,
                                         char **buffer,
                                         size_t *length,
                                         size_t *capacity,
                                         char **out_error_detail) {
    char *identifier = NULL;
    char *backend_expression = NULL;

    proxy_evaluate_skip_whitespace(parser);
    if (parser->position >= parser->length) {
        proxy_set_error_detail_printf(out_error_detail,
                                      "unexpected end of Feng watch expression");
        return false;
    }
    if (parser->expression[parser->position] == '(') {
        parser->position += 1U;
        if (!proxy_append_byte(buffer, length, capacity, '(') ||
            !proxy_parse_evaluate_comparison(parser,
                                             buffer,
                                             length,
                                             capacity,
                                             out_error_detail)) {
            return false;
        }
        proxy_evaluate_skip_whitespace(parser);
        if (parser->position >= parser->length || parser->expression[parser->position] != ')') {
            proxy_set_error_detail_printf(out_error_detail,
                                          "evaluate expression is missing ')' to close a grouped subexpression");
            return false;
        }
        parser->position += 1U;
        if (!proxy_append_byte(buffer, length, capacity, ')')) {
            proxy_set_error_detail_printf(out_error_detail,
                                          "out of memory rewriting evaluate expression");
            return false;
        }
        return true;
    }
    if (isdigit((unsigned char)parser->expression[parser->position])) {
        return proxy_parse_evaluate_integer_literal(parser,
                                                    false,
                                                    buffer,
                                                    length,
                                                    capacity,
                                                    out_error_detail,
                                                    "evaluate arithmetic and comparison expressions only support integer literals");
    }
    if (!proxy_parse_evaluate_identifier_token(parser,
                                               &identifier,
                                               out_error_detail,
                                               "unsupported Feng watch expression syntax")) {
        return false;
    }
    if (!proxy_resolve_evaluate_identifier(parser->artifact,
                                           parser->frame_backend_symbol,
                                           identifier,
                                           &backend_expression,
                                           out_error_detail)) {
        free(identifier);
        return false;
    }
    free(identifier);
    if (backend_expression == NULL ||
        !proxy_append_evaluate_leaf_expression(buffer,
                                               length,
                                               capacity,
                                               backend_expression)) {
        free(backend_expression);
        if (backend_expression == NULL) {
            proxy_set_error_detail_printf(out_error_detail,
                                          "failed to resolve evaluate expression");
        } else {
            proxy_set_error_detail_printf(out_error_detail,
                                          "out of memory rewriting evaluate expression");
        }
        return false;
    }
    free(backend_expression);
    return true;
}

/* Parse one postfix expression including member and constant index access. */
static bool proxy_parse_evaluate_postfix(FengDapEvaluateParser *parser,
                                         char **buffer,
                                         size_t *length,
                                         size_t *capacity,
                                         char **out_error_detail) {
    if (!proxy_parse_evaluate_primary(parser, buffer, length, capacity, out_error_detail)) {
        return false;
    }
    for (;;) {
        char *member_name = NULL;

        proxy_evaluate_skip_whitespace(parser);
        if (parser->position >= parser->length) {
            return true;
        }
        if (parser->expression[parser->position] == '.') {
            parser->position += 1U;
            if (!proxy_append_byte(buffer, length, capacity, '.') ||
                !proxy_parse_evaluate_identifier_token(parser,
                                                       &member_name,
                                                       out_error_detail,
                                                       "evaluate member access must use an identifier after '.'") ||
                !proxy_append_cstr(buffer, length, capacity, member_name)) {
                free(member_name);
                if (member_name != NULL && out_error_detail != NULL && *out_error_detail == NULL) {
                    proxy_set_error_detail_printf(out_error_detail,
                                                  "out of memory rewriting evaluate expression");
                }
                return false;
            }
            free(member_name);
            continue;
        }
        if (parser->expression[parser->position] == '[') {
            parser->position += 1U;
            if (!proxy_append_byte(buffer, length, capacity, '[') ||
                !proxy_parse_evaluate_integer_literal(parser,
                                                     true,
                                                     buffer,
                                                     length,
                                                     capacity,
                                                     out_error_detail,
                                                     "evaluate index access must use an integer literal")) {
                return false;
            }
            proxy_evaluate_skip_whitespace(parser);
            if (parser->position >= parser->length || parser->expression[parser->position] != ']') {
                proxy_set_error_detail_printf(out_error_detail,
                                              "evaluate index access must use an integer literal enclosed by '[' and ']'"
                );
                return false;
            }
            parser->position += 1U;
            if (!proxy_append_byte(buffer, length, capacity, ']')) {
                proxy_set_error_detail_printf(out_error_detail,
                                              "out of memory rewriting evaluate expression");
                return false;
            }
            continue;
        }
        if (parser->expression[parser->position] == '(') {
            proxy_set_error_detail_printf(out_error_detail,
                                          "function calls are not supported in Feng watch expressions");
            return false;
        }
        return true;
    }
}

/* Parse one unary arithmetic expression. */
static bool proxy_parse_evaluate_unary(FengDapEvaluateParser *parser,
                                       char **buffer,
                                       size_t *length,
                                       size_t *capacity,
                                       char **out_error_detail) {
    proxy_evaluate_skip_whitespace(parser);
    if (parser->position < parser->length &&
        (parser->expression[parser->position] == '+' ||
         parser->expression[parser->position] == '-')) {
        char op = parser->expression[parser->position];

        parser->position += 1U;
        if (!proxy_append_byte(buffer, length, capacity, (unsigned char)op)) {
            proxy_set_error_detail_printf(out_error_detail,
                                          "out of memory rewriting evaluate expression");
            return false;
        }
        return proxy_parse_evaluate_unary(parser,
                                          buffer,
                                          length,
                                          capacity,
                                          out_error_detail);
    }
    if (parser->position < parser->length && parser->expression[parser->position] == '!') {
        proxy_set_error_detail_printf(out_error_detail,
                                      "logical negation is not supported in Feng watch expressions");
        return false;
    }
    return proxy_parse_evaluate_postfix(parser,
                                        buffer,
                                        length,
                                        capacity,
                                        out_error_detail);
}

/* Parse one multiplicative arithmetic expression. */
static bool proxy_parse_evaluate_multiplicative(FengDapEvaluateParser *parser,
                                                char **buffer,
                                                size_t *length,
                                                size_t *capacity,
                                                char **out_error_detail) {
    if (!proxy_parse_evaluate_unary(parser, buffer, length, capacity, out_error_detail)) {
        return false;
    }
    for (;;) {
        char op;

        proxy_evaluate_skip_whitespace(parser);
        if (parser->position >= parser->length) {
            return true;
        }
        op = parser->expression[parser->position];
        if (op != '*' && op != '/' && op != '%') {
            return true;
        }
        parser->position += 1U;
        if (!proxy_append_bytes(buffer, length, capacity, " ", 1U) ||
            !proxy_append_byte(buffer, length, capacity, (unsigned char)op) ||
            !proxy_append_bytes(buffer, length, capacity, " ", 1U) ||
            !proxy_parse_evaluate_unary(parser,
                                        buffer,
                                        length,
                                        capacity,
                                        out_error_detail)) {
            if (out_error_detail != NULL && *out_error_detail == NULL) {
                proxy_set_error_detail_printf(out_error_detail,
                                              "out of memory rewriting evaluate expression");
            }
            return false;
        }
    }
}

/* Parse one additive arithmetic expression. */
static bool proxy_parse_evaluate_additive(FengDapEvaluateParser *parser,
                                          char **buffer,
                                          size_t *length,
                                          size_t *capacity,
                                          char **out_error_detail) {
    if (!proxy_parse_evaluate_multiplicative(parser,
                                             buffer,
                                             length,
                                             capacity,
                                             out_error_detail)) {
        return false;
    }
    for (;;) {
        char op;

        proxy_evaluate_skip_whitespace(parser);
        if (parser->position >= parser->length) {
            return true;
        }
        op = parser->expression[parser->position];
        if (op != '+' && op != '-') {
            return true;
        }
        parser->position += 1U;
        if (!proxy_append_bytes(buffer, length, capacity, " ", 1U) ||
            !proxy_append_byte(buffer, length, capacity, (unsigned char)op) ||
            !proxy_append_bytes(buffer, length, capacity, " ", 1U) ||
            !proxy_parse_evaluate_multiplicative(parser,
                                                 buffer,
                                                 length,
                                                 capacity,
                                                 out_error_detail)) {
            if (out_error_detail != NULL && *out_error_detail == NULL) {
                proxy_set_error_detail_printf(out_error_detail,
                                              "out of memory rewriting evaluate expression");
            }
            return false;
        }
    }
}

/* Parse one comparison expression built from additive subexpressions. */
static bool proxy_parse_evaluate_comparison(FengDapEvaluateParser *parser,
                                            char **buffer,
                                            size_t *length,
                                            size_t *capacity,
                                            char **out_error_detail) {
    if (!proxy_parse_evaluate_additive(parser,
                                       buffer,
                                       length,
                                       capacity,
                                       out_error_detail)) {
        return false;
    }
    for (;;) {
        const char *op = NULL;
        size_t op_length = 0U;

        proxy_evaluate_skip_whitespace(parser);
        if (parser->position >= parser->length) {
            return true;
        }
        if (parser->expression[parser->position] == '=' &&
            (parser->position + 1U >= parser->length ||
             parser->expression[parser->position + 1U] != '=')) {
            proxy_set_error_detail_printf(out_error_detail,
                                          "assignment is not supported in Feng watch expressions");
            return false;
        }
        if (parser->position + 1U < parser->length) {
            if (parser->expression[parser->position] == '=' &&
                parser->expression[parser->position + 1U] == '=') {
                op = "==";
                op_length = 2U;
            } else if (parser->expression[parser->position] == '!' &&
                       parser->expression[parser->position + 1U] == '=') {
                op = "!=";
                op_length = 2U;
            } else if (parser->expression[parser->position] == '<' &&
                       parser->expression[parser->position + 1U] == '=') {
                op = "<=";
                op_length = 2U;
            } else if (parser->expression[parser->position] == '>' &&
                       parser->expression[parser->position + 1U] == '=') {
                op = ">=";
                op_length = 2U;
            }
        }
        if (op == NULL && parser->expression[parser->position] == '<') {
            op = "<";
            op_length = 1U;
        } else if (op == NULL && parser->expression[parser->position] == '>') {
            op = ">";
            op_length = 1U;
        }
        if (op == NULL) {
            return true;
        }
        parser->position += op_length;
        if (!proxy_append_bytes(buffer, length, capacity, " ", 1U) ||
            !proxy_append_bytes(buffer, length, capacity, op, op_length) ||
            !proxy_append_bytes(buffer, length, capacity, " ", 1U) ||
            !proxy_parse_evaluate_additive(parser,
                                           buffer,
                                           length,
                                           capacity,
                                           out_error_detail)) {
            if (out_error_detail != NULL && *out_error_detail == NULL) {
                proxy_set_error_detail_printf(out_error_detail,
                                              "out of memory rewriting evaluate expression");
            }
            return false;
        }
    }
}

/* Rewrite one supported Feng watch expression into a backend evaluate expression. */
static bool proxy_rewrite_evaluate_expression(const FengDebugArtifact *artifact,
                                              const char *frame_backend_symbol,
                                              const char *expression,
                                              char **out_backend_expression,
                                              char **out_error_detail) {
    FengDapEvaluateParser parser = {0};
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    bool ok = false;

    *out_backend_expression = NULL;
    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (expression == NULL || expression[0] == '\0') {
        proxy_set_error_detail_printf(out_error_detail,
                                      "evaluate arguments.expression must not be empty");
        return false;
    }
    parser.artifact = artifact;
    parser.frame_backend_symbol = frame_backend_symbol;
    parser.expression = expression;
    parser.length = strlen(expression);
    parser.position = 0U;
    if (!proxy_parse_evaluate_comparison(&parser,
                                         &buffer,
                                         &length,
                                         &capacity,
                                         out_error_detail)) {
        goto cleanup;
    }
    proxy_evaluate_skip_whitespace(&parser);
    if (parser.position != parser.length) {
        if (parser.expression[parser.position] == '=') {
            proxy_set_error_detail_printf(out_error_detail,
                                          "assignment is not supported in Feng watch expressions");
        } else {
            proxy_set_error_detail_printf(out_error_detail,
                                          "unsupported Feng watch expression syntax near '%c'",
                                          parser.expression[parser.position]);
        }
        goto cleanup;
    }
    if (!proxy_append_byte(&buffer, &length, &capacity, '\0')) {
        proxy_set_error_detail_printf(out_error_detail,
                                      "out of memory rewriting evaluate expression");
        goto cleanup;
    }
    buffer[length - 1U] = '\0';
    *out_backend_expression = buffer;
    buffer = NULL;
    ok = true;

cleanup:
    free(buffer);
    return ok;
}

/* Resolve one Feng identifier to a unique backend evaluate expression. */
static bool proxy_resolve_evaluate_identifier(const FengDebugArtifact *artifact,
                                              const char *frame_backend_symbol,
                                              const char *identifier,
                                              char **out_backend_expression,
                                              char **out_error_detail) {
    char *resolved_expression = NULL;
    size_t match_count = 0U;
    size_t index;

    *out_backend_expression = NULL;
    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (artifact == NULL || frame_backend_symbol == NULL || identifier == NULL) {
        return true;
    }
    for (index = 0U; index < artifact->info.variable_count; ++index) {
        const FengCodegenMapingVariableRecord *record = artifact->info.variables + index;
        const char *candidate_expression;

        if (strcmp(record->frame_backend_symbol, frame_backend_symbol) != 0 ||
            strcmp(record->display_name, identifier) != 0) {
            continue;
        }
        candidate_expression = record->backend_name != NULL ? record->backend_name : record->read_expr;
        if (candidate_expression == NULL || candidate_expression[0] == '\0') {
            continue;
        }
        if (resolved_expression == NULL) {
            resolved_expression = proxy_dup_printf("%s", candidate_expression);
            if (resolved_expression == NULL) {
                if (out_error_detail != NULL) {
                    *out_error_detail = proxy_dup_printf("out of memory resolving evaluate expression");
                }
                return false;
            }
            match_count = 1U;
            continue;
        }
        if (strcmp(resolved_expression, candidate_expression) != 0) {
            match_count += 1U;
        }
    }
    if (match_count == 0U) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("evaluate identifier '%s' is not available in the current Feng frame",
                                                 identifier);
        }
        free(resolved_expression);
        return false;
    }
    if (match_count > 1U) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("evaluate identifier '%s' is ambiguous in the current Feng frame",
                                                 identifier);
        }
        free(resolved_expression);
        return false;
    }
    *out_backend_expression = resolved_expression;
    return true;
}

/* Rewrite evaluate.arguments.expression for the supported read-only watch subset. */
static bool proxy_rewrite_evaluate_request_payload(const char *json,
                                                   size_t json_length,
                                                   const FengDebugArtifact *artifact,
                                                   const FengDapRelayState *state,
                                                   char **out_json,
                                                   char **out_error_detail,
                                                   int error_fd) {
    const char *expression_start;
    const char *expression_end;
    const char *after_string;
    const char *frame_backend_symbol;
    uint64_t frame_id = 0U;
    char *expression = NULL;
    char *backend_expression = NULL;
    FengDapJsonStringReplacement replacement = {0};
    bool ok = false;

    *out_json = NULL;
    if (out_error_detail != NULL) {
        free(*out_error_detail);
        *out_error_detail = NULL;
    }
    if (artifact == NULL ||
        !proxy_json_get_request_argument_u64_member(json,
                                                    json_length,
                                                    "frameId",
                                                    &frame_id) ||
        frame_id == 0U) {
        return true;
    }
    frame_backend_symbol = proxy_relay_state_find_frame_binding(state, frame_id);
    if (frame_backend_symbol == NULL ||
        !proxy_json_get_request_argument_string_range(json,
                                                      json_length,
                                                      "expression",
                                                      &expression_start,
                                                      &expression_end)) {
        return true;
    }
    if (!proxy_json_parse_string_copy(expression_start, expression_end, &expression, &after_string) ||
        proxy_json_skip_whitespace(after_string, expression_end) != expression_end) {
        if (out_error_detail != NULL) {
            *out_error_detail = proxy_dup_printf("evaluate arguments.expression must be a string");
        }
        return false;
    }
    if (!proxy_rewrite_evaluate_expression(artifact,
                                           frame_backend_symbol,
                                           expression,
                                           &backend_expression,
                                           out_error_detail)) {
        goto cleanup;
    }
    if (backend_expression == NULL || strcmp(backend_expression, expression) == 0) {
        ok = true;
        goto cleanup;
    }
    replacement.start_offset = (size_t)(expression_start - json);
    replacement.end_offset = (size_t)(expression_end - json);
    replacement.replacement = backend_expression;
    backend_expression = NULL;
    if (!proxy_apply_json_string_replacements(json,
                                              json_length,
                                              &replacement,
                                              1U,
                                              out_json,
                                              error_fd,
                                              "failed to rewrite evaluate expression")) {
        goto cleanup;
    }
    ok = true;

cleanup:
    free(expression);
    free(backend_expression);
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
                                                       FengDapRelayState *state,
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
        uint64_t frame_id = 0U;
        char *frame_json = NULL;
        char *backend_symbol = NULL;
        char *rewritten_frame_json = NULL;
        const char *visible_frame_json;
        size_t frame_json_length;

        if (!proxy_json_skip_value(frame_start, frames_end, &frame_end)) {
            ok = true;
            goto cleanup;
        }
        frame_json_length = (size_t)(frame_end - frame_start);
        frame_json = proxy_dup_bytes((const unsigned char *)frame_start, frame_json_length);
        if (frame_json != NULL) {
            proxy_json_get_u64_member_loose(frame_json, frame_json_length, "id", &frame_id);
            proxy_json_get_string_member_loose(frame_json,
                                               frame_json_length,
                                               "name",
                                               &backend_symbol);
        }
        if (frame_json == NULL ||
            !proxy_rewrite_stack_trace_frame_payload(frame_json,
                                                     frame_json_length,
                                                     artifact,
                                                     &frame_record,
                                                     &rewritten_frame_json,
                                                     error_fd)) {
            free(frame_json);
            free(backend_symbol);
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
            free(backend_symbol);
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

        if (state != NULL && frame_id != 0U && backend_symbol != NULL &&
            !proxy_relay_state_set_frame_binding(state, frame_id, backend_symbol)) {
            free(frame_json);
            free(backend_symbol);
            free(rewritten_frame_json);
            proxy_report_error(error_fd,
                               "failed to rewrite stackTrace response",
                               "out of memory");
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
        free(backend_symbol);
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

/* Record scope variablesReference bindings for one scopes response. */
static bool proxy_record_scopes_response_bindings(const char *json,
                                                  size_t json_length,
                                                  uint64_t frame_id,
                                                  FengDapRelayState *state) {
    const char *body_start;
    const char *body_end;
    const char *scopes_start;
    const char *scopes_end;
    const char *cursor;

    if (state == NULL ||
        !proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "body",
                                             &body_start,
                                             &body_end) ||
        !proxy_json_find_object_member_loose(body_start,
                                             (size_t)(body_end - body_start),
                                             "scopes",
                                             &scopes_start,
                                             &scopes_end)) {
        return true;
    }
    cursor = proxy_json_skip_whitespace(scopes_start, scopes_end);
    if (cursor >= scopes_end || *cursor != '[') {
        return true;
    }

    cursor = proxy_json_skip_whitespace(cursor + 1, scopes_end);
    while (cursor < scopes_end && *cursor != ']') {
        const char *scope_start = cursor;
        const char *scope_end;
        uint64_t variables_reference = 0U;

        if (!proxy_json_skip_value(scope_start, scopes_end, &scope_end)) {
            return true;
        }
        if (proxy_json_get_u64_member_loose(scope_start,
                                            (size_t)(scope_end - scope_start),
                                            "variablesReference",
                                            &variables_reference) &&
            variables_reference != 0U &&
            !proxy_relay_state_set_scope_binding(state, variables_reference, frame_id)) {
            return false;
        }
        cursor = proxy_json_skip_whitespace(scope_end, scopes_end);
        if (cursor < scopes_end && *cursor == ',') {
            cursor = proxy_json_skip_whitespace(cursor + 1, scopes_end);
            continue;
        }
        if (cursor < scopes_end && *cursor == ']') {
            break;
        }
        return true;
    }
    return true;
}

static bool proxy_json_value_looks_pointer(const char *value) {
    size_t index;

    if (value == NULL || value[0] != '0' || (value[1] != 'x' && value[1] != 'X') || value[2] == '\0') {
        return false;
    }
    for (index = 2U; value[index] != '\0'; ++index) {
        if (!isxdigit((unsigned char)value[index])) {
            return false;
        }
    }
    return true;
}

static bool proxy_parse_u64_cstr(const char *text, uint64_t *out_value) {
    char *endptr = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || out_value == NULL) {
        return false;
    }
    errno = 0;
    value = strtoull(text, &endptr, 10);
    if (errno != 0 || endptr == text || endptr == NULL || *endptr != '\0') {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

static bool proxy_replace_object_string_member(char **json,
                                               const char *key,
                                               const char *replacement,
                                               bool *out_replaced,
                                               int error_fd,
                                               const char *context) {
    const char *value_start;
    const char *value_end;
    FengDapJsonStringReplacement span = {0};
    char *replacement_copy = NULL;
    char *rewritten = NULL;
    size_t json_length;

    if (out_replaced != NULL) {
        *out_replaced = false;
    }
    if (json == NULL || *json == NULL || key == NULL || replacement == NULL) {
        proxy_report_error(error_fd, context, "invalid JSON string member replacement");
        return false;
    }
    json_length = strlen(*json);
    if (!proxy_json_find_object_member_loose(*json, json_length, key, &value_start, &value_end)) {
        return true;
    }
    replacement_copy = proxy_dup_printf("%s", replacement);
    if (replacement_copy == NULL) {
        proxy_report_error(error_fd, context, "out of memory");
        return false;
    }
    span.start_offset = (size_t)(value_start - *json);
    span.end_offset = (size_t)(value_end - *json);
    span.replacement = replacement_copy;
    if (!proxy_apply_json_string_replacements(*json,
                                              json_length,
                                              &span,
                                              1U,
                                              &rewritten,
                                              error_fd,
                                              context)) {
        free(replacement_copy);
        return false;
    }
    free(replacement_copy);
    free(*json);
    *json = rewritten;
    if (out_replaced != NULL) {
        *out_replaced = true;
    }
    return true;
}

static bool proxy_replace_object_u64_member(char **json,
                                            const char *key,
                                            uint64_t replacement,
                                            bool *out_replaced,
                                            int error_fd,
                                            const char *context) {
    const char *value_start;
    const char *value_end;
    char *replacement_text = NULL;
    char *rewritten = NULL;
    size_t json_length;

    if (out_replaced != NULL) {
        *out_replaced = false;
    }
    if (json == NULL || *json == NULL || key == NULL) {
        proxy_report_error(error_fd, context, "invalid JSON number member replacement");
        return false;
    }
    json_length = strlen(*json);
    if (!proxy_json_find_object_member_loose(*json, json_length, key, &value_start, &value_end)) {
        return true;
    }
    replacement_text = proxy_dup_printf("%llu", (unsigned long long)replacement);
    if (replacement_text == NULL) {
        proxy_report_error(error_fd, context, "out of memory");
        return false;
    }
    if (!proxy_replace_json_span(*json,
                                 json_length,
                                 (size_t)(value_start - *json),
                                 (size_t)(value_end - *json),
                                 replacement_text,
                                 &rewritten,
                                 error_fd,
                                 context)) {
        free(replacement_text);
        return false;
    }
    free(replacement_text);
    free(*json);
    *json = rewritten;
    if (out_replaced != NULL) {
        *out_replaced = true;
    }
    return true;
}

static bool proxy_parse_evaluated_variable_response_payload(const char *json,
                                                            size_t json_length,
                                                            FengDapEvaluatedVariable *out_value) {
    const char *body_start;
    const char *body_end;
    const char *result_start;
    const char *result_end;
    const char *type_start;
    const char *type_end;
    const char *after_string;

    if (out_value == NULL) {
        return false;
    }
    proxy_evaluated_variable_dispose(out_value);
    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "body",
                                             &body_start,
                                             &body_end)) {
        return true;
    }
    if (proxy_json_find_object_member_loose(body_start,
                                            (size_t)(body_end - body_start),
                                            "result",
                                            &result_start,
                                            &result_end)) {
        if (!proxy_json_parse_string_copy(result_start, result_end, &out_value->result, &after_string) ||
            proxy_json_skip_whitespace(after_string, result_end) != result_end) {
            return false;
        }
        out_value->has_result = true;
    }
    if (proxy_json_find_object_member_loose(body_start,
                                            (size_t)(body_end - body_start),
                                            "type",
                                            &type_start,
                                            &type_end)) {
        if (!proxy_json_parse_string_copy(type_start, type_end, &out_value->type, &after_string) ||
            proxy_json_skip_whitespace(after_string, type_end) != type_end) {
            return false;
        }
        out_value->has_type = true;
    }
    if (proxy_json_get_u64_member_loose(body_start,
                                        (size_t)(body_end - body_start),
                                        "variablesReference",
                                        &out_value->variables_reference)) {
        out_value->has_variables_reference = true;
    }
    return true;
}

static bool proxy_send_internal_evaluate_request(int backend_stdin_fd,
                                                 FengDapRelayState *state,
                                                 uint64_t frame_id,
                                                 const char *expression,
                                                 uint64_t *out_request_seq,
                                                 int error_fd) {
    uint64_t request_seq;
    char *escaped_expression = NULL;
    char *payload = NULL;
    bool ok;

    if (backend_stdin_fd < 0 || expression == NULL || out_request_seq == NULL) {
        proxy_report_error(error_fd,
                           "failed to send internal evaluate request",
                           "invalid evaluate request arguments");
        return false;
    }
    request_seq = proxy_relay_state_next_internal_request_seq(state);
    escaped_expression = proxy_json_escape(expression);
    if (escaped_expression == NULL) {
        proxy_report_error(error_fd,
                           "failed to send internal evaluate request",
                           "out of memory");
        return false;
    }
    payload = proxy_dup_printf(
        "{\"seq\":%llu,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\",\"frameId\":%llu,\"context\":\"variables\"}}",
        (unsigned long long)request_seq,
        escaped_expression,
        (unsigned long long)frame_id);
    free(escaped_expression);
    if (payload == NULL) {
        proxy_report_error(error_fd,
                           "failed to send internal evaluate request",
                           "out of memory");
        return false;
    }
    ok = proxy_write_message(backend_stdin_fd,
                             payload,
                             error_fd,
                             "failed to send internal evaluate request to lldb-dap");
    free(payload);
    if (!ok) {
        return false;
    }
    *out_request_seq = request_seq;
    return true;
}

static bool proxy_collect_internal_evaluate_response(FengDapMessageReader *backend_reader,
                                                     int backend_stdin_fd,
                                                     int output_fd,
                                                     const FengDebugArtifact *artifact,
                                                     FengDapRelayState *state,
                                                     uint64_t request_seq,
                                                     FengDapEvaluatedVariable *out_value,
                                                     int error_fd) {
    for (;;) {
        FengDapMessage message = {0};
        FengDapReadStatus status;
        char *type = NULL;
        char *command = NULL;
        uint64_t response_seq = 0U;
        bool is_match = false;
        bool success = false;
        bool ok = true;

        status = proxy_reader_read_message(backend_reader, &message, error_fd);
        if (status != FENG_DAP_READ_OK) {
            proxy_message_dispose(&message);
            return false;
        }

        if (proxy_json_get_string_member(message.payload,
                                         message.payload_length,
                                         "type",
                                         &type) &&
            proxy_json_get_string_member(message.payload,
                                         message.payload_length,
                                         "command",
                                         &command) &&
            proxy_json_get_u64_member(message.payload,
                                      message.payload_length,
                                      "request_seq",
                                      &response_seq) &&
            strcmp(type, "response") == 0 &&
            strcmp(command, "evaluate") == 0 &&
            response_seq == request_seq) {
            is_match = true;
            if (proxy_json_get_bool_member(message.payload,
                                           message.payload_length,
                                           "success",
                                           &success) &&
                success) {
                ok = proxy_parse_evaluated_variable_response_payload(message.payload,
                                                                    message.payload_length,
                                                                    out_value);
            }
        }

        if (is_match) {
            free(type);
            free(command);
            proxy_message_dispose(&message);
            return ok;
        }

        ok = proxy_process_backend_relay_message(&message,
                                                 backend_reader,
                                                 backend_stdin_fd,
                                                 output_fd,
                                                 artifact,
                                                 state,
                                                 error_fd);
        free(type);
        free(command);
        proxy_message_dispose(&message);
        if (!ok) {
            return false;
        }
    }
}

static const char *proxy_variable_read_expression(const FengCodegenMapingVariableRecord *record) {
    if (record == NULL) {
        return NULL;
    }
    if (record->read_expr != NULL && record->read_expr[0] != '\0') {
        return record->read_expr;
    }
    if (record->backend_name != NULL && record->backend_name[0] != '\0') {
        return record->backend_name;
    }
    return NULL;
}

static bool proxy_type_is_exact(const char *type_text, const char *expected) {
    return type_text != NULL && expected != NULL && strcmp(type_text, expected) == 0;
}

static char *proxy_runtime_pointer_fallback_value(const char *type_text) {
    size_t length;

    if (type_text == NULL || type_text[0] == '\0') {
        return NULL;
    }
    if (proxy_type_is_exact(type_text, "FengArray *")) {
        return proxy_dup_printf("array");
    }
    if (proxy_type_is_exact(type_text, "FengString *")) {
        return proxy_dup_printf("string");
    }
    length = strlen(type_text);
    if (length > 2U && type_text[length - 1U] == '*' && type_text[length - 2U] == ' ') {
        return proxy_dup_bytes((const unsigned char *)type_text, length - 2U);
    }
    return proxy_dup_printf("%s", type_text);
}

static char *proxy_build_runtime_pointer_summary_expr(const char *type_text,
                                                      const char *read_expression) {
    if (type_text == NULL || read_expression == NULL || read_expression[0] == '\0') {
        return NULL;
    }
    if (proxy_type_is_exact(type_text, "FengArray *")) {
        return proxy_dup_printf("(size_t)feng_array_length((const FengArray *)(%s))", read_expression);
    }
    return NULL;
}

static char *proxy_build_runtime_string_value_expr(const char *read_expression) {
    if (read_expression == NULL || read_expression[0] == '\0') {
        return NULL;
    }
    return proxy_dup_printf("(const char *)feng_string_data((const FengString *)(%s))",
                            read_expression);
}

static char *proxy_dup_quoted_debug_string_value(const char *text) {
    const char *cursor;
    const char *best_start = NULL;
    const char *best_end = NULL;

    if (text == NULL) {
        return NULL;
    }
    cursor = text;
    while (*cursor != '\0') {
        const char *start;

        if (*cursor != '"') {
            ++cursor;
            continue;
        }
        start = cursor++;
        while (*cursor != '\0') {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == '"') {
                best_start = start;
                best_end = cursor;
                ++cursor;
                break;
            }
            ++cursor;
        }
    }
    if (best_start == NULL || best_end == NULL || best_end < best_start) {
        return NULL;
    }
    return proxy_dup_bytes((const unsigned char *)best_start,
                           (size_t)(best_end - best_start + 1U));
}

static bool proxy_try_read_runtime_string_value(FengDapMessageReader *backend_reader,
                                                int backend_stdin_fd,
                                                int output_fd,
                                                const FengDebugArtifact *artifact,
                                                FengDapRelayState *state,
                                                uint64_t frame_id,
                                                const char *read_expression,
                                                char **out_value,
                                                int error_fd) {
    char *value_expr = NULL;
    FengDapEvaluatedVariable evaluated = {0};
    uint64_t request_seq = 0U;
    char *value = NULL;
    bool ok = false;

    if (out_value != NULL) {
        *out_value = NULL;
    }
    if (frame_id == 0U || read_expression == NULL || read_expression[0] == '\0') {
        return true;
    }
    value_expr = proxy_build_runtime_string_value_expr(read_expression);
    if (value_expr == NULL) {
        return true;
    }
    if (!proxy_send_internal_evaluate_request(backend_stdin_fd,
                                              state,
                                              frame_id,
                                              value_expr,
                                              &request_seq,
                                              error_fd) ||
        !proxy_collect_internal_evaluate_response(backend_reader,
                                                  backend_stdin_fd,
                                                  output_fd,
                                                  artifact,
                                                  state,
                                                  request_seq,
                                                  &evaluated,
                                                  error_fd)) {
        goto cleanup;
    }
    if (!evaluated.has_result || evaluated.result == NULL || evaluated.result[0] == '\0') {
        ok = true;
        goto cleanup;
    }
    value = proxy_dup_quoted_debug_string_value(evaluated.result);
    if (value != NULL && out_value != NULL) {
        *out_value = value;
        value = NULL;
    }
    ok = true;

cleanup:
    free(value_expr);
    free(value);
    proxy_evaluated_variable_dispose(&evaluated);
    return ok;
}

static char *proxy_array_display_element_type(const char *display_type) {
    size_t length;

    if (display_type == NULL || display_type[0] == '\0') {
        return NULL;
    }
    length = strlen(display_type);
    if (length > 3U &&
        display_type[length - 3U] == '[' &&
        display_type[length - 2U] == '!' &&
        display_type[length - 1U] == ']') {
        length -= 3U;
    } else if (length > 2U &&
               display_type[length - 2U] == '[' &&
               display_type[length - 1U] == ']') {
        length -= 2U;
    } else {
        return NULL;
    }
    if (length == 0U) {
        return NULL;
    }
    return proxy_dup_bytes((const unsigned char *)display_type, length);
}

static char *proxy_array_summary_element_type(const FengCodegenMapingVariableRecord *record) {
    if (record == NULL) {
        return NULL;
    }
    return proxy_array_display_element_type(record->display_type);
}

static const char *proxy_array_element_c_type(const char *element_display_type) {
    char *nested_element_type;

    if (element_display_type == NULL) {
        return NULL;
    }
    if (strcmp(element_display_type, "i64") == 0) return "int64_t";
    if (strcmp(element_display_type, "i32") == 0) return "int32_t";
    if (strcmp(element_display_type, "i16") == 0) return "int16_t";
    if (strcmp(element_display_type, "i8") == 0) return "int8_t";
    if (strcmp(element_display_type, "u64") == 0) return "uint64_t";
    if (strcmp(element_display_type, "u32") == 0) return "uint32_t";
    if (strcmp(element_display_type, "u16") == 0) return "uint16_t";
    if (strcmp(element_display_type, "u8") == 0) return "uint8_t";
    if (strcmp(element_display_type, "f64") == 0) return "double";
    if (strcmp(element_display_type, "f32") == 0) return "float";
    if (strcmp(element_display_type, "bool") == 0) return "uint8_t";
    if (strcmp(element_display_type, "string") == 0) return "FengString *";
    nested_element_type = proxy_array_display_element_type(element_display_type);
    if (nested_element_type != NULL) {
        free(nested_element_type);
        return "FengArray *";
    }
    return "void *";
}

static char *proxy_build_array_element_expr(const char *parent_read_expr,
                                            const char *element_display_type,
                                            uint64_t index) {
    const char *element_c_type = proxy_array_element_c_type(element_display_type);

    if (parent_read_expr == NULL || parent_read_expr[0] == '\0' || element_c_type == NULL) {
        return NULL;
    }
    return proxy_dup_printf("((%s *)feng_array_data(%s))[%llu]",
                            element_c_type,
                            parent_read_expr,
                            (unsigned long long)index);
}

static size_t proxy_count_field_records(const FengDebugArtifact *artifact,
                                        const char *parent_display_type) {
    size_t count = 0U;

    if (artifact == NULL || parent_display_type == NULL || parent_display_type[0] == '\0') {
        return 0U;
    }
    for (size_t index = 0U; index < artifact->info.variable_count; ++index) {
        const FengCodegenMapingVariableRecord *record = &artifact->info.variables[index];

        if (record->kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD &&
            record->parent_display_type != NULL &&
            strcmp(record->parent_display_type, parent_display_type) == 0) {
            count += 1U;
        }
    }
    return count;
}

static char *proxy_build_field_expr(const char *parent_read_expr,
                                    const char *field_read_expr) {
    if (parent_read_expr == NULL || parent_read_expr[0] == '\0' ||
        field_read_expr == NULL || field_read_expr[0] == '\0') {
        return NULL;
    }
    return proxy_dup_printf("(%s)%s", parent_read_expr, field_read_expr);
}

static bool proxy_try_summarize_runtime_pointer_value(FengDapMessageReader *backend_reader,
                                                      int backend_stdin_fd,
                                                      int output_fd,
                                                      const FengDebugArtifact *artifact,
                                                      FengDapRelayState *state,
                                                      uint64_t frame_id,
                                                      const FengCodegenMapingVariableRecord *record,
                                                      const char *read_expression,
                                                      const char *type_text,
                                                      char **out_summary,
                                                      uint64_t *out_array_length,
                                                      int error_fd) {
    char *summary_expr = NULL;
    char *array_element_type = NULL;
    FengDapEvaluatedVariable evaluated = {0};
    uint64_t request_seq = 0U;
    char *summary = NULL;
    bool ok = false;

    if (out_summary != NULL) {
        *out_summary = NULL;
    }
    if (out_array_length != NULL) {
        *out_array_length = 0U;
    }
    if (frame_id == 0U || read_expression == NULL || read_expression[0] == '\0' || type_text == NULL) {
        return true;
    }
    summary_expr = proxy_build_runtime_pointer_summary_expr(type_text, read_expression);
    if (summary_expr == NULL) {
        return true;
    }
    if (!proxy_type_is_exact(type_text, "FengArray *") &&
        !proxy_type_is_exact(type_text, "FengString *")) {
        ok = true;
        goto cleanup;
    }
    if (!proxy_send_internal_evaluate_request(backend_stdin_fd,
                                              state,
                                              frame_id,
                                              summary_expr,
                                              &request_seq,
                                              error_fd) ||
        !proxy_collect_internal_evaluate_response(backend_reader,
                                                  backend_stdin_fd,
                                                  output_fd,
                                                  artifact,
                                                  state,
                                                  request_seq,
                                                  &evaluated,
                                                  error_fd)) {
        goto cleanup;
    }
    if (!evaluated.has_result || evaluated.result == NULL || evaluated.result[0] == '\0') {
        ok = true;
        goto cleanup;
    }
    if (proxy_type_is_exact(type_text, "FengArray *")) {
        array_element_type = proxy_array_summary_element_type(record);
        if (array_element_type == NULL) {
            proxy_report_error(error_fd,
                               "failed to rewrite variables response",
                               "missing array element display type");
            goto cleanup;
        }
        if (out_array_length != NULL &&
            !proxy_parse_u64_cstr(evaluated.result, out_array_length)) {
            proxy_report_error(error_fd,
                               "failed to rewrite variables response",
                               "invalid array length result");
            goto cleanup;
        }
        summary = proxy_dup_printf("%s[length=%s]", array_element_type, evaluated.result);
    } else {
        summary = proxy_dup_printf("string[length=%s]", evaluated.result);
    }
    if (summary == NULL) {
        proxy_report_error(error_fd,
                           "failed to rewrite variables response",
                           "out of memory");
        goto cleanup;
    }
    if (out_summary != NULL) {
        *out_summary = summary;
        summary = NULL;
    }
    ok = true;

cleanup:
    free(summary_expr);
    free(array_element_type);
    free(summary);
    proxy_evaluated_variable_dispose(&evaluated);
    return ok;
}

static bool proxy_try_read_array_length(FengDapMessageReader *backend_reader,
                                        int backend_stdin_fd,
                                        int output_fd,
                                        const FengDebugArtifact *artifact,
                                        FengDapRelayState *state,
                                        uint64_t frame_id,
                                        const char *read_expression,
                                        uint64_t *out_length,
                                        int error_fd) {
    char *length_expr = NULL;
    FengDapEvaluatedVariable evaluated = {0};
    uint64_t request_seq = 0U;
    bool ok = false;

    if (out_length != NULL) {
        *out_length = 0U;
    }
    if (frame_id == 0U || read_expression == NULL || read_expression[0] == '\0' || out_length == NULL) {
        return true;
    }
    length_expr = proxy_build_runtime_pointer_summary_expr("FengArray *", read_expression);
    if (length_expr == NULL) {
        return true;
    }
    if (!proxy_send_internal_evaluate_request(backend_stdin_fd,
                                              state,
                                              frame_id,
                                              length_expr,
                                              &request_seq,
                                              error_fd) ||
        !proxy_collect_internal_evaluate_response(backend_reader,
                                                  backend_stdin_fd,
                                                  output_fd,
                                                  artifact,
                                                  state,
                                                  request_seq,
                                                  &evaluated,
                                                  error_fd)) {
        goto cleanup;
    }
    if (!evaluated.has_result || !proxy_parse_u64_cstr(evaluated.result, out_length)) {
        proxy_report_error(error_fd,
                           "failed to expand synthetic array variables",
                           "invalid array length result");
        goto cleanup;
    }
    ok = true;

cleanup:
    free(length_expr);
    proxy_evaluated_variable_dispose(&evaluated);
    return ok;
}

static bool proxy_append_u64_decimal(char **buffer,
                                     size_t *length,
                                     size_t *capacity,
                                     uint64_t value) {
    char text[32];
    int text_length = snprintf(text, sizeof(text), "%llu", (unsigned long long)value);

    if (text_length < 0 || (size_t)text_length >= sizeof(text)) {
        return false;
    }
    return proxy_append_bytes(buffer, length, capacity, text, (size_t)text_length);
}

static bool proxy_append_synthetic_variable_json(char **buffer,
                                                 size_t *length,
                                                 size_t *capacity,
                                                 const char *name,
                                                 const char *value,
                                                 const char *type,
                                                 const char *evaluate_name,
                                                 uint64_t variables_reference) {
    char *escaped_name = proxy_json_escape(name != NULL ? name : "");
    char *escaped_value = proxy_json_escape(value != NULL ? value : "");
    char *escaped_type = proxy_json_escape(type != NULL ? type : "");
    char *escaped_evaluate_name = proxy_json_escape(evaluate_name != NULL ? evaluate_name : "");
    bool ok;

    if (escaped_name == NULL || escaped_value == NULL || escaped_type == NULL ||
        escaped_evaluate_name == NULL) {
        free(escaped_name);
        free(escaped_value);
        free(escaped_type);
        free(escaped_evaluate_name);
        return false;
    }
    ok = proxy_append_cstr(buffer, length, capacity, "{\"name\":\"") &&
         proxy_append_cstr(buffer, length, capacity, escaped_name) &&
         proxy_append_cstr(buffer, length, capacity, "\",\"value\":\"") &&
         proxy_append_cstr(buffer, length, capacity, escaped_value) &&
         proxy_append_cstr(buffer, length, capacity, "\",\"type\":\"") &&
         proxy_append_cstr(buffer, length, capacity, escaped_type) &&
         proxy_append_cstr(buffer, length, capacity, "\",\"evaluateName\":\"") &&
         proxy_append_cstr(buffer, length, capacity, escaped_evaluate_name) &&
         proxy_append_cstr(buffer, length, capacity, "\",\"variablesReference\":") &&
         proxy_append_u64_decimal(buffer, length, capacity, variables_reference) &&
         proxy_append_byte(buffer, length, capacity, '}');
    free(escaped_name);
    free(escaped_value);
    free(escaped_type);
    free(escaped_evaluate_name);
    return ok;
}

static bool proxy_send_synthetic_array_variables_response(FengDapMessageReader *backend_reader,
                                                          int backend_stdin_fd,
                                                          int output_fd,
                                                          const FengDebugArtifact *artifact,
                                                          FengDapRelayState *state,
                                                          const FengDapSyntheticRef *synthetic_ref,
                                                          uint64_t request_seq,
                                                          uint64_t *next_seq,
                                                          int error_fd) {
    char *variables_json = NULL;
    size_t variables_length = 0U;
    size_t variables_capacity = 0U;
    char *payload = NULL;
    uint64_t visible_count;
    bool ok = false;

    if (synthetic_ref == NULL || synthetic_ref->kind != FENG_DAP_SYNTHETIC_ARRAY) {
        return proxy_send_request_failure_response(output_fd,
                                                   "variables",
                                                   request_seq,
                                                   "unknown synthetic variablesReference",
                                                   next_seq,
                                                   error_fd);
    }
    visible_count = synthetic_ref->element_count;
    if (visible_count > FENG_DAP_SYNTHETIC_ARRAY_ELEMENT_LIMIT) {
        visible_count = FENG_DAP_SYNTHETIC_ARRAY_ELEMENT_LIMIT;
    }
    if (!proxy_append_byte(&variables_json, &variables_length, &variables_capacity, '[')) {
        proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
        goto cleanup;
    }
    for (uint64_t element_index = 0U; element_index < visible_count; ++element_index) {
        char *element_expr = NULL;
        char *element_name = NULL;
        char *element_value = NULL;
        char *element_type = NULL;
        char *nested_element_type = NULL;
        FengDapEvaluatedVariable evaluated = {0};
        uint64_t child_ref = 0U;
        uint64_t request_id = 0U;

        element_expr = proxy_build_array_element_expr(synthetic_ref->parent_read_expr,
                                                      synthetic_ref->element_display_type,
                                                      element_index);
        element_name = proxy_dup_printf("[%llu]", (unsigned long long)element_index);
        if (element_expr == NULL || element_name == NULL) {
            free(element_expr);
            free(element_name);
            proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
            goto cleanup;
        }
        if (!proxy_send_internal_evaluate_request(backend_stdin_fd,
                                                  state,
                                                  synthetic_ref->frame_id,
                                                  element_expr,
                                                  &request_id,
                                                  error_fd) ||
            !proxy_collect_internal_evaluate_response(backend_reader,
                                                      backend_stdin_fd,
                                                      output_fd,
                                                      artifact,
                                                      state,
                                                      request_id,
                                                      &evaluated,
                                                      error_fd)) {
            free(element_expr);
            free(element_name);
            proxy_evaluated_variable_dispose(&evaluated);
            goto cleanup;
        }
        element_value = evaluated.has_result && evaluated.result != NULL
            ? proxy_dup_printf("%s", evaluated.result)
            : proxy_dup_printf("");
        element_type = proxy_dup_printf("%s", synthetic_ref->element_display_type);
        if (element_value == NULL || element_type == NULL) {
            free(element_expr);
            free(element_name);
            free(element_value);
            free(element_type);
            proxy_evaluated_variable_dispose(&evaluated);
            proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
            goto cleanup;
        }
        if (strcmp(synthetic_ref->element_display_type, "string") == 0) {
            char *string_value = NULL;

            if (!proxy_try_read_runtime_string_value(backend_reader,
                                                     backend_stdin_fd,
                                                     output_fd,
                                                     artifact,
                                                     state,
                                                     synthetic_ref->frame_id,
                                                     element_expr,
                                                     &string_value,
                                                     error_fd)) {
                free(element_expr);
                free(element_name);
                free(element_value);
                free(element_type);
                proxy_evaluated_variable_dispose(&evaluated);
                goto cleanup;
            }
            if (string_value != NULL) {
                free(element_value);
                element_value = string_value;
            }
        }
        nested_element_type = proxy_array_display_element_type(synthetic_ref->element_display_type);
        if (nested_element_type != NULL && synthetic_ref->depth + 1U < FENG_DAP_SYNTHETIC_MAX_DEPTH) {
            uint64_t nested_length = 0U;

            if (!proxy_try_read_array_length(backend_reader,
                                             backend_stdin_fd,
                                             output_fd,
                                             artifact,
                                             state,
                                             synthetic_ref->frame_id,
                                             element_expr,
                                             &nested_length,
                                             error_fd) ||
                !proxy_relay_state_register_synthetic_array(state,
                                                            synthetic_ref->frame_id,
                                                            element_expr,
                                                            nested_element_type,
                                                            nested_length,
                                                            synthetic_ref->depth + 1U,
                                                            &child_ref)) {
                free(element_expr);
                free(element_name);
                free(element_value);
                free(element_type);
                free(nested_element_type);
                proxy_evaluated_variable_dispose(&evaluated);
                proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
                goto cleanup;
            }
            free(element_value);
            element_value = proxy_dup_printf("%s[length=%llu]",
                                             nested_element_type,
                                             (unsigned long long)nested_length);
            if (element_value == NULL) {
                free(element_expr);
                free(element_name);
                free(element_type);
                free(nested_element_type);
                proxy_evaluated_variable_dispose(&evaluated);
                proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
                goto cleanup;
            }
        } else if (synthetic_ref->depth + 1U < FENG_DAP_SYNTHETIC_MAX_DEPTH &&
                   proxy_count_field_records(artifact, synthetic_ref->element_display_type) > 0U &&
                   evaluated.has_result &&
                   proxy_json_value_looks_pointer(evaluated.result)) {
            if (!proxy_relay_state_register_synthetic_type(state,
                                                          synthetic_ref->frame_id,
                                                          element_expr,
                                                          synthetic_ref->element_display_type,
                                                          synthetic_ref->depth + 1U,
                                                          &child_ref)) {
                free(element_expr);
                free(element_name);
                free(element_value);
                free(element_type);
                free(nested_element_type);
                proxy_evaluated_variable_dispose(&evaluated);
                proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
                goto cleanup;
            }
            free(element_value);
            element_value = proxy_dup_printf("%s", synthetic_ref->element_display_type);
            if (element_value == NULL) {
                free(element_expr);
                free(element_name);
                free(element_type);
                free(nested_element_type);
                proxy_evaluated_variable_dispose(&evaluated);
                proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
                goto cleanup;
            }
        }
        if (element_index > 0U &&
            !proxy_append_byte(&variables_json, &variables_length, &variables_capacity, ',')) {
            free(element_expr);
            free(element_name);
            free(element_value);
            free(element_type);
            free(nested_element_type);
            proxy_evaluated_variable_dispose(&evaluated);
            proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
            goto cleanup;
        }
        if (!proxy_append_synthetic_variable_json(&variables_json,
                                                  &variables_length,
                                                  &variables_capacity,
                                                  element_name,
                                                  element_value,
                                                  element_type,
                                                  "",
                                                  child_ref)) {
            free(element_expr);
            free(element_name);
            free(element_value);
            free(element_type);
            free(nested_element_type);
            proxy_evaluated_variable_dispose(&evaluated);
            proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
            goto cleanup;
        }
        free(element_expr);
        free(element_name);
        free(element_value);
        free(element_type);
        free(nested_element_type);
        proxy_evaluated_variable_dispose(&evaluated);
    }
    if (synthetic_ref->element_count > FENG_DAP_SYNTHETIC_ARRAY_ELEMENT_LIMIT) {
        if (visible_count > 0U &&
            !proxy_append_byte(&variables_json, &variables_length, &variables_capacity, ',')) {
            proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
            goto cleanup;
        }
        if (!proxy_append_synthetic_variable_json(&variables_json,
                                                  &variables_length,
                                                  &variables_capacity,
                                                  "...",
                                                  "truncated after 256 elements",
                                                  "",
                                                  "",
                                                  0U)) {
            proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
            goto cleanup;
        }
    }
    if (!proxy_append_byte(&variables_json, &variables_length, &variables_capacity, ']') ||
        !proxy_append_byte(&variables_json, &variables_length, &variables_capacity, '\0')) {
        proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
        goto cleanup;
    }
    variables_json[variables_length - 1U] = '\0';
    payload = proxy_dup_printf(
        "{\"seq\":%llu,\"type\":\"response\",\"request_seq\":%llu,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":%s}}",
        (unsigned long long)(*next_seq)++,
        (unsigned long long)request_seq,
        variables_json);
    if (payload == NULL) {
        proxy_report_error(error_fd, "failed to expand synthetic array variables", "out of memory");
        goto cleanup;
    }
    ok = proxy_write_message(output_fd,
                             payload,
                             error_fd,
                             "failed to write synthetic array variables response");

cleanup:
    free(variables_json);
    free(payload);
    return ok;
}

static bool proxy_send_synthetic_type_variables_response(FengDapMessageReader *backend_reader,
                                                         int backend_stdin_fd,
                                                         int output_fd,
                                                         const FengDebugArtifact *artifact,
                                                         FengDapRelayState *state,
                                                         const FengDapSyntheticRef *synthetic_ref,
                                                         uint64_t request_seq,
                                                         uint64_t *next_seq,
                                                         int error_fd) {
    char *variables_json = NULL;
    size_t variables_length = 0U;
    size_t variables_capacity = 0U;
    char *payload = NULL;
    bool first = true;
    bool ok = false;

    if (synthetic_ref == NULL || synthetic_ref->kind != FENG_DAP_SYNTHETIC_TYPE ||
        synthetic_ref->type_display_name == NULL) {
        return proxy_send_request_failure_response(output_fd,
                                                   "variables",
                                                   request_seq,
                                                   "unknown synthetic variablesReference",
                                                   next_seq,
                                                   error_fd);
    }
    if (!proxy_append_byte(&variables_json, &variables_length, &variables_capacity, '[')) {
        proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
        goto cleanup;
    }
    for (size_t index = 0U; index < artifact->info.variable_count; ++index) {
        const FengCodegenMapingVariableRecord *field = &artifact->info.variables[index];
        char *field_expr = NULL;
        char *field_value = NULL;
        char *field_type = NULL;
        char *summary_text = NULL;
        FengDapEvaluatedVariable evaluated = {0};
        uint64_t child_ref = 0U;
        uint64_t array_length = 0U;
        uint64_t request_id = 0U;

        if (field->kind != FENG_CODEGEN_MAPING_VARIABLE_FIELD ||
            field->parent_display_type == NULL ||
            strcmp(field->parent_display_type, synthetic_ref->type_display_name) != 0) {
            continue;
        }
        field_expr = proxy_build_field_expr(synthetic_ref->parent_read_expr, field->read_expr);
        if (field_expr == NULL) {
            proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
            goto cleanup;
        }
        if (!proxy_send_internal_evaluate_request(backend_stdin_fd,
                                                  state,
                                                  synthetic_ref->frame_id,
                                                  field_expr,
                                                  &request_id,
                                                  error_fd) ||
            !proxy_collect_internal_evaluate_response(backend_reader,
                                                      backend_stdin_fd,
                                                      output_fd,
                                                      artifact,
                                                      state,
                                                      request_id,
                                                      &evaluated,
                                                      error_fd)) {
            free(field_expr);
            proxy_evaluated_variable_dispose(&evaluated);
            goto cleanup;
        }
        field_type = proxy_dup_printf("%s", field->display_type != NULL ? field->display_type : "");
        field_value = evaluated.has_result && evaluated.result != NULL
            ? proxy_dup_printf("%s", evaluated.result)
            : proxy_dup_printf("");
        if (field_type == NULL || field_value == NULL) {
            free(field_expr);
            free(field_type);
            free(field_value);
            proxy_evaluated_variable_dispose(&evaluated);
            proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
            goto cleanup;
        }
        if (field->display_type != NULL && strcmp(field->display_type, "string") == 0) {
            char *string_value = NULL;

            if (!proxy_try_read_runtime_string_value(backend_reader,
                                                     backend_stdin_fd,
                                                     output_fd,
                                                     artifact,
                                                     state,
                                                     synthetic_ref->frame_id,
                                                     field_expr,
                                                     &string_value,
                                                     error_fd)) {
                free(field_expr);
                free(field_type);
                free(field_value);
                proxy_evaluated_variable_dispose(&evaluated);
                goto cleanup;
            }
            if (string_value != NULL) {
                free(field_value);
                field_value = string_value;
            }
        } else if (evaluated.has_type &&
                   proxy_type_is_exact(evaluated.type, "FengArray *") &&
                   evaluated.has_result &&
                   proxy_json_value_looks_pointer(evaluated.result)) {
            if (!proxy_try_summarize_runtime_pointer_value(backend_reader,
                                                           backend_stdin_fd,
                                                           output_fd,
                                                           artifact,
                                                           state,
                                                           synthetic_ref->frame_id,
                                                           field,
                                                           field_expr,
                                                           evaluated.type,
                                                           &summary_text,
                                                           &array_length,
                                                           error_fd)) {
                free(field_expr);
                free(field_type);
                free(field_value);
                proxy_evaluated_variable_dispose(&evaluated);
                goto cleanup;
            }
            if (summary_text != NULL) {
                char *array_element_type = proxy_array_summary_element_type(field);

                if (array_element_type == NULL ||
                    !proxy_relay_state_register_synthetic_array(state,
                                                               synthetic_ref->frame_id,
                                                               field_expr,
                                                               array_element_type,
                                                               array_length,
                                                               synthetic_ref->depth + 1U,
                                                               &child_ref)) {
                    free(array_element_type);
                    free(field_expr);
                    free(field_type);
                    free(field_value);
                    free(summary_text);
                    proxy_evaluated_variable_dispose(&evaluated);
                    proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
                    goto cleanup;
                }
                free(array_element_type);
                free(field_value);
                field_value = summary_text;
                summary_text = NULL;
            }
        } else if (field->display_type != NULL &&
                   synthetic_ref->depth + 1U < FENG_DAP_SYNTHETIC_MAX_DEPTH &&
                   proxy_count_field_records(artifact, field->display_type) > 0U &&
                   evaluated.has_result &&
                   proxy_json_value_looks_pointer(evaluated.result)) {
            if (!proxy_relay_state_register_synthetic_type(state,
                                                          synthetic_ref->frame_id,
                                                          field_expr,
                                                          field->display_type,
                                                          synthetic_ref->depth + 1U,
                                                          &child_ref)) {
                free(field_expr);
                free(field_type);
                free(field_value);
                proxy_evaluated_variable_dispose(&evaluated);
                proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
                goto cleanup;
            }
            free(field_value);
            field_value = proxy_dup_printf("%s", field->display_type);
            if (field_value == NULL) {
                free(field_expr);
                free(field_type);
                proxy_evaluated_variable_dispose(&evaluated);
                proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
                goto cleanup;
            }
        }
        if (!first &&
            !proxy_append_byte(&variables_json, &variables_length, &variables_capacity, ',')) {
            free(field_expr);
            free(field_type);
            free(field_value);
            proxy_evaluated_variable_dispose(&evaluated);
            proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
            goto cleanup;
        }
        if (!proxy_append_synthetic_variable_json(&variables_json,
                                                  &variables_length,
                                                  &variables_capacity,
                                                  field->display_name,
                                                  field_value,
                                                  field_type,
                                                  "",
                                                  child_ref)) {
            free(field_expr);
            free(field_type);
            free(field_value);
            proxy_evaluated_variable_dispose(&evaluated);
            proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
            goto cleanup;
        }
        first = false;
        free(field_expr);
        free(field_type);
        free(field_value);
        proxy_evaluated_variable_dispose(&evaluated);
    }
    if (!proxy_append_byte(&variables_json, &variables_length, &variables_capacity, ']') ||
        !proxy_append_byte(&variables_json, &variables_length, &variables_capacity, '\0')) {
        proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
        goto cleanup;
    }
    variables_json[variables_length - 1U] = '\0';
    payload = proxy_dup_printf(
        "{\"seq\":%llu,\"type\":\"response\",\"request_seq\":%llu,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":%s}}",
        (unsigned long long)(*next_seq)++,
        (unsigned long long)request_seq,
        variables_json);
    if (payload == NULL) {
        proxy_report_error(error_fd, "failed to expand synthetic type variables", "out of memory");
        goto cleanup;
    }
    ok = proxy_write_message(output_fd,
                             payload,
                             error_fd,
                             "failed to write synthetic type variables response");

cleanup:
    free(variables_json);
    free(payload);
    return ok;
}

static bool proxy_rewrite_one_variable_payload(const char *variable_json,
                                               size_t variable_json_length,
                                               const char *backend_name,
                                               const FengCodegenMapingVariableRecord *record,
                                               FengDapMessageReader *backend_reader,
                                               int backend_stdin_fd,
                                               int output_fd,
                                               const FengDebugArtifact *artifact,
                                               FengDapRelayState *state,
                                               uint64_t frame_id,
                                               char **out_json,
                                               bool *out_changed,
                                               int error_fd) {
    char *current_json = NULL;
    char *evaluate_name = NULL;
    char *value_text = NULL;
    char *type_text = NULL;
    char *string_value_text = NULL;
    char *summary_text = NULL;
    char *array_element_type = NULL;
    char *fallback_value = NULL;
    FengDapEvaluatedVariable evaluated = {0};
    uint64_t variables_reference = 0U;
    uint64_t array_length = 0U;
    uint64_t synthetic_ref_id = 0U;
    bool changed = false;
    bool replaced = false;
    bool ok = false;
    const char *read_expression = proxy_variable_read_expression(record);

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (out_changed != NULL) {
        *out_changed = false;
    }
    current_json = proxy_dup_bytes((const unsigned char *)variable_json, variable_json_length);
    if (current_json == NULL) {
        proxy_report_error(error_fd,
                           "failed to rewrite variables response",
                           "out of memory");
        goto cleanup;
    }

    if (record != NULL &&
        record->display_name != NULL &&
        backend_name != NULL &&
        strcmp(record->display_name, backend_name) != 0) {
        if (!proxy_replace_object_string_member(&current_json,
                                                "name",
                                                record->display_name,
                                                &replaced,
                                                error_fd,
                                                "failed to rewrite variables response")) {
            goto cleanup;
        }
        changed = changed || replaced;

        if (proxy_json_get_string_member_loose(current_json,
                                               strlen(current_json),
                                               "evaluateName",
                                               &evaluate_name) &&
            strcmp(evaluate_name, backend_name) == 0) {
            if (!proxy_replace_object_string_member(&current_json,
                                                    "evaluateName",
                                                    record->display_name,
                                                    &replaced,
                                                    error_fd,
                                                    "failed to rewrite variables response")) {
                goto cleanup;
            }
            changed = changed || replaced;
        }
        free(evaluate_name);
        evaluate_name = NULL;
    }

    if (record != NULL && read_expression != NULL && frame_id != 0U) {
        uint64_t request_seq = 0U;

        if (!proxy_send_internal_evaluate_request(backend_stdin_fd,
                                                  state,
                                                  frame_id,
                                                  read_expression,
                                                  &request_seq,
                                                  error_fd) ||
            !proxy_collect_internal_evaluate_response(backend_reader,
                                                      backend_stdin_fd,
                                                      output_fd,
                                                      artifact,
                                                      state,
                                                      request_seq,
                                                      &evaluated,
                                                      error_fd)) {
            goto cleanup;
        }
        if (evaluated.has_result) {
            if (!proxy_replace_object_string_member(&current_json,
                                                    "value",
                                                    evaluated.result,
                                                    &replaced,
                                                    error_fd,
                                                    "failed to rewrite variables response")) {
                goto cleanup;
            }
            changed = changed || replaced;
        }
        if (evaluated.has_type) {
            if (!proxy_replace_object_string_member(&current_json,
                                                    "type",
                                                    evaluated.type,
                                                    &replaced,
                                                    error_fd,
                                                    "failed to rewrite variables response")) {
                goto cleanup;
            }
            changed = changed || replaced;
        }
        if (evaluated.has_variables_reference) {
            if (!proxy_replace_object_u64_member(&current_json,
                                                 "variablesReference",
                                                 evaluated.variables_reference,
                                                 &replaced,
                                                 error_fd,
                                                 "failed to rewrite variables response")) {
                goto cleanup;
            }
            changed = changed || replaced;
        }
    }

    if (record != NULL &&
        proxy_json_get_string_member_loose(current_json,
                                           strlen(current_json),
                                           "value",
                                           &value_text) &&
        proxy_json_get_u64_member_loose(current_json,
                                        strlen(current_json),
                                        "variablesReference",
                                        &variables_reference) &&
        variables_reference != 0U &&
        proxy_json_get_string_member_loose(current_json,
                                           strlen(current_json),
                                           "type",
                                           &type_text) &&
        type_text[0] != '\0' &&
        proxy_json_value_looks_pointer(value_text)) {
        if (proxy_type_is_exact(type_text, "FengString *")) {
            if (!proxy_try_read_runtime_string_value(backend_reader,
                                                     backend_stdin_fd,
                                                     output_fd,
                                                     artifact,
                                                     state,
                                                     frame_id,
                                                     read_expression,
                                                     &string_value_text,
                                                     error_fd)) {
                goto cleanup;
            }
            if (string_value_text != NULL) {
                if (!proxy_replace_object_string_member(&current_json,
                                                        "value",
                                                        string_value_text,
                                                        &replaced,
                                                        error_fd,
                                                        "failed to rewrite variables response")) {
                    goto cleanup;
                }
                changed = changed || replaced;
                goto finish_pointer_summary;
            }
        }
        if (!proxy_try_summarize_runtime_pointer_value(backend_reader,
                                                       backend_stdin_fd,
                                                       output_fd,
                                                       artifact,
                                                       state,
                                                       frame_id,
                                                       record,
                                                       read_expression,
                                                       type_text,
                                                       &summary_text,
                                                       &array_length,
                                                       error_fd)) {
            goto cleanup;
        }
        if (summary_text != NULL) {
            if (!proxy_replace_object_string_member(&current_json,
                                                    "value",
                                                    summary_text,
                                                    &replaced,
                                                    error_fd,
                                                    "failed to rewrite variables response")) {
                goto cleanup;
            }
            changed = changed || replaced;
            if (proxy_type_is_exact(type_text, "FengArray *")) {
                array_element_type = proxy_array_summary_element_type(record);
                if (array_element_type == NULL ||
                    !proxy_relay_state_register_synthetic_array(state,
                                                               frame_id,
                                                               read_expression,
                                                               array_element_type,
                                                               array_length,
                                                               0U,
                                                               &synthetic_ref_id) ||
                    !proxy_replace_object_u64_member(&current_json,
                                                     "variablesReference",
                                                     synthetic_ref_id,
                                                     &replaced,
                                                     error_fd,
                                                     "failed to rewrite variables response")) {
                    proxy_report_error(error_fd,
                                       "failed to rewrite variables response",
                                       "out of memory");
                    goto cleanup;
                }
                changed = changed || replaced;
            }
            goto finish_pointer_summary;
        }
        fallback_value = proxy_runtime_pointer_fallback_value(type_text);
        if (fallback_value == NULL) {
            proxy_report_error(error_fd,
                               "failed to rewrite variables response",
                               "out of memory");
            goto cleanup;
        }
        if (!proxy_replace_object_string_member(&current_json,
                                                "value",
                                                fallback_value,
                                                &replaced,
                                                error_fd,
                                                "failed to rewrite variables response")) {
            goto cleanup;
        }
        changed = changed || replaced;
        if (record->display_type != NULL &&
            proxy_count_field_records(artifact, record->display_type) > 0U) {
            if (!proxy_relay_state_register_synthetic_type(state,
                                                          frame_id,
                                                          read_expression,
                                                          record->display_type,
                                                          0U,
                                                          &synthetic_ref_id) ||
                !proxy_replace_object_u64_member(&current_json,
                                                 "variablesReference",
                                                 synthetic_ref_id,
                                                 &replaced,
                                                 error_fd,
                                                 "failed to rewrite variables response")) {
                proxy_report_error(error_fd,
                                   "failed to rewrite variables response",
                                   "out of memory");
                goto cleanup;
            }
            changed = changed || replaced;
        }
    }

finish_pointer_summary:

    if (!changed) {
        ok = true;
        goto cleanup;
    }

    if (out_json != NULL) {
        *out_json = current_json;
        current_json = NULL;
    }
    if (out_changed != NULL) {
        *out_changed = true;
    }
    ok = true;

cleanup:
    free(current_json);
    free(evaluate_name);
    free(value_text);
    free(type_text);
    free(string_value_text);
    free(summary_text);
    free(array_element_type);
    free(fallback_value);
    proxy_evaluated_variable_dispose(&evaluated);
    return ok;
}

/* Rewrite top-level scope variables to keep only user mappings and surface user values. */
static bool proxy_rewrite_variables_response_payload(const char *json,
                                                     size_t json_length,
                                                     FengDapMessageReader *backend_reader,
                                                     int backend_stdin_fd,
                                                     int output_fd,
                                                     const FengDebugArtifact *artifact,
                                                     FengDapRelayState *state,
                                                     uint64_t frame_id,
                                                     const char *frame_backend_symbol,
                                                     char **out_json,
                                                     int error_fd) {
    const char *body_start;
    const char *body_end;
    const char *variables_start;
    const char *variables_end;
    const char *cursor;
    char *rewritten_variables = NULL;
    size_t rewritten_variables_length = 0U;
    size_t rewritten_variables_capacity = 0U;
    size_t visible_variable_count = 0U;
    bool variables_changed = false;
    char *payload_with_variables = NULL;
    bool ok = false;

    *out_json = NULL;
    if (artifact == NULL || frame_backend_symbol == NULL) {
        return true;
    }
    if (!proxy_json_find_object_member_loose(json,
                                             json_length,
                                             "body",
                                             &body_start,
                                             &body_end) ||
        !proxy_json_find_object_member_loose(body_start,
                                             (size_t)(body_end - body_start),
                                             "variables",
                                             &variables_start,
                                             &variables_end)) {
        return true;
    }
    cursor = proxy_json_skip_whitespace(variables_start, variables_end);
    if (cursor >= variables_end || *cursor != '[') {
        return true;
    }
    if (!proxy_append_byte(&rewritten_variables,
                           &rewritten_variables_length,
                           &rewritten_variables_capacity,
                           '[')) {
        proxy_report_error(error_fd,
                           "failed to rewrite variables response",
                           "out of memory");
        goto cleanup;
    }

    cursor = proxy_json_skip_whitespace(cursor + 1, variables_end);
    while (cursor < variables_end && *cursor != ']') {
        const char *variable_start = cursor;
        const char *variable_end;
        const FengCodegenMapingVariableRecord *record = NULL;
        char *backend_name = NULL;
        char *variable_json = NULL;
        char *rewritten_variable_json = NULL;
        const char *visible_variable_json;
        size_t variable_json_length;
        bool variable_changed = false;

        if (!proxy_json_skip_value(variable_start, variables_end, &variable_end)) {
            ok = true;
            goto cleanup;
        }
        variable_json_length = (size_t)(variable_end - variable_start);
        variable_json = proxy_dup_bytes((const unsigned char *)variable_start, variable_json_length);
        if (variable_json == NULL) {
            proxy_report_error(error_fd,
                               "failed to rewrite variables response",
                               "out of memory");
            goto cleanup;
        }
        if (proxy_json_get_string_member_loose(variable_json,
                                               variable_json_length,
                                               "name",
                                               &backend_name)) {
            record = proxy_find_variable_record(artifact, frame_backend_symbol, backend_name);
        }
        if (record == NULL) {
            variables_changed = true;
            free(variable_json);
            free(backend_name);
            cursor = proxy_json_skip_whitespace(variable_end, variables_end);
            if (cursor < variables_end && *cursor == ',') {
                cursor = proxy_json_skip_whitespace(cursor + 1, variables_end);
                continue;
            }
            if (cursor < variables_end && *cursor == ']') {
                break;
            }
            ok = true;
            goto cleanup;
        }
        if (!proxy_rewrite_one_variable_payload(variable_start,
                                                variable_json_length,
                                                backend_name,
                                                record,
                                                backend_reader,
                                                backend_stdin_fd,
                                                output_fd,
                                                artifact,
                                                state,
                                                frame_id,
                                                &rewritten_variable_json,
                                                &variable_changed,
                                                error_fd)) {
            free(variable_json);
            free(backend_name);
            free(rewritten_variable_json);
            goto cleanup;
        }
        variables_changed = variables_changed || variable_changed;
        visible_variable_json = rewritten_variable_json != NULL ? rewritten_variable_json : variable_json;
        if (visible_variable_count > 0U &&
            !proxy_append_byte(&rewritten_variables,
                               &rewritten_variables_length,
                               &rewritten_variables_capacity,
                               ',')) {
            free(variable_json);
            free(backend_name);
            free(rewritten_variable_json);
            proxy_report_error(error_fd,
                               "failed to rewrite variables response",
                               "out of memory");
            goto cleanup;
        }
        if (!proxy_append_bytes(&rewritten_variables,
                                &rewritten_variables_length,
                                &rewritten_variables_capacity,
                                visible_variable_json,
                                strlen(visible_variable_json))) {
            free(variable_json);
            free(backend_name);
            free(rewritten_variable_json);
            proxy_report_error(error_fd,
                               "failed to rewrite variables response",
                               "out of memory");
            goto cleanup;
        }
        visible_variable_count += 1U;
        free(variable_json);
        free(backend_name);
        free(rewritten_variable_json);

        cursor = proxy_json_skip_whitespace(variable_end, variables_end);
        if (cursor < variables_end && *cursor == ',') {
            cursor = proxy_json_skip_whitespace(cursor + 1, variables_end);
            continue;
        }
        if (cursor < variables_end && *cursor == ']') {
            break;
        }
        ok = true;
        goto cleanup;
    }

    if (!proxy_append_byte(&rewritten_variables,
                           &rewritten_variables_length,
                           &rewritten_variables_capacity,
                           ']') ||
        !proxy_append_byte(&rewritten_variables,
                           &rewritten_variables_length,
                           &rewritten_variables_capacity,
                           '\0')) {
        proxy_report_error(error_fd,
                           "failed to rewrite variables response",
                           "out of memory");
        goto cleanup;
    }
    rewritten_variables[rewritten_variables_length - 1U] = '\0';

    if (!variables_changed) {
        ok = true;
        goto cleanup;
    }

    if (!proxy_replace_json_span(json,
                                 json_length,
                                 (size_t)(variables_start - json),
                                 (size_t)(variables_end - json),
                                 rewritten_variables,
                                 &payload_with_variables,
                                 error_fd,
                                 "failed to rewrite variables response")) {
        goto cleanup;
    }

    *out_json = payload_with_variables;
    payload_with_variables = NULL;
    ok = true;

cleanup:
    free(rewritten_variables);
    free(payload_with_variables);
    return ok;
}

/* Drain any already-ready backend messages so dependent client rewrites see fresh state. */
static bool proxy_drain_ready_backend_messages(FengDapMessageReader *backend_reader,
                                               int backend_stdin_fd,
                                               int output_fd,
                                               const FengDebugArtifact *artifact,
                                               FengDapRelayState *state,
                                               int error_fd) {
    for (;;) {
        FengDapMessage message = {0};
        FengDapReadStatus status;
        bool ok;

        if (!proxy_reader_fill_if_ready(backend_reader, error_fd)) {
            return false;
        }
        status = proxy_reader_try_read_buffered_message(backend_reader, &message, error_fd);
        if (status == FENG_DAP_READ_PENDING || status == FENG_DAP_READ_EOF) {
            return true;
        }
        if (status == FENG_DAP_READ_ERROR) {
            return false;
        }
        ok = proxy_process_backend_relay_message(&message,
                            backend_reader,
                            backend_stdin_fd,
                                                output_fd,
                                                artifact,
                                                state,
                                                error_fd);
        proxy_message_dispose(&message);
        if (!ok) {
            return false;
        }
    }
}

/* Forward one client message to lldb-dap, rewriting setBreakpoints paths when needed. */
static bool proxy_process_client_relay_message(const FengDapMessage *message,
                                               FengDapMessageReader *backend_reader,
                                               int backend_stdin_fd,
                                               int output_fd,
                                               const FengDebugArtifact *artifact,
                                               FengDapRelayState *state,
                                               uint64_t *next_seq,
                                               int error_fd) {
    char *type = NULL;
    char *command = NULL;
    char *rewritten_payload = NULL;
    char *error_detail = NULL;
    uint64_t request_seq = 0U;
    uint64_t frame_id = 0U;
    uint64_t variables_reference = 0U;
    bool is_set_breakpoints = false;
    bool is_evaluate = false;
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
        (strcmp(command, "setBreakpoints") == 0 || strcmp(command, "evaluate") == 0)) {
        is_set_breakpoints = strcmp(command, "setBreakpoints") == 0;
        is_evaluate = strcmp(command, "evaluate") == 0;
    }
    if (type != NULL &&
        strcmp(type, "request") == 0 &&
        command != NULL &&
        proxy_json_get_u64_member(message->payload,
                                  message->payload_length,
                                  "seq",
                                  &request_seq)) {
        if (proxy_command_resumes_execution(command)) {
            proxy_relay_state_clear_stopped_context(state);
        }
        if (strcmp(command, "scopes") == 0 &&
            proxy_json_get_request_argument_u64_member(message->payload,
                                                       message->payload_length,
                                                       "frameId",
                                                       &frame_id) &&
            !proxy_relay_state_record_pending_scope_request(state, request_seq, frame_id)) {
            free(type);
            free(command);
            proxy_report_error(error_fd,
                               "failed to record scopes request",
                               "out of memory");
            return false;
        }
        if (strcmp(command, "variables") == 0 &&
            proxy_json_get_request_argument_u64_member(message->payload,
                                                       message->payload_length,
                                                       "variablesReference",
                                                       &variables_reference) &&
            PROXY_IS_SYNTHETIC_REF(variables_reference)) {
            const FengDapSyntheticRef *synthetic_ref = proxy_relay_state_find_synthetic_ref(state,
                                                                                           variables_reference);

            if (synthetic_ref != NULL && synthetic_ref->kind == FENG_DAP_SYNTHETIC_TYPE) {
                ok = proxy_send_synthetic_type_variables_response(backend_reader,
                                                                  backend_stdin_fd,
                                                                  output_fd,
                                                                  artifact,
                                                                  state,
                                                                  synthetic_ref,
                                                                  request_seq,
                                                                  next_seq,
                                                                  error_fd);
            } else {
                ok = proxy_send_synthetic_array_variables_response(backend_reader,
                                                                   backend_stdin_fd,
                                                                   output_fd,
                                                                   artifact,
                                                                   state,
                                                                   synthetic_ref,
                                                                   request_seq,
                                                                   next_seq,
                                                                   error_fd);
            }
            free(type);
            free(command);
            return ok;
        }
        if (strcmp(command, "variables") == 0 &&
            proxy_json_get_request_argument_u64_member(message->payload,
                                                       message->payload_length,
                                                       "variablesReference",
                                                       &variables_reference) &&
            !proxy_relay_state_record_pending_variables_request(state,
                                                                request_seq,
                                                                variables_reference)) {
            free(type);
            free(command);
            proxy_report_error(error_fd,
                               "failed to record variables request",
                               "out of memory");
            return false;
        }
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
    if (is_evaluate) {
        if (!proxy_drain_ready_backend_messages(backend_reader,
                                                backend_stdin_fd,
                                                output_fd,
                                                artifact,
                                                state,
                                                error_fd)) {
            free(type);
            free(command);
            free(error_detail);
            free(rewritten_payload);
            return false;
        }
        if (!proxy_rewrite_evaluate_request_payload(message->payload,
                                                    message->payload_length,
                                                    artifact,
                                                    state,
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
                                       "failed to rewrite evaluate expression",
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
                                     "failed to forward rewritten evaluate request to lldb-dap");
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
                                                FengDapMessageReader *backend_reader,
                                                int backend_stdin_fd,
                                                int output_fd,
                                                const FengDebugArtifact *artifact,
                                                FengDapRelayState *state,
                                                int error_fd) {
    char *type = NULL;
    char *command = NULL;
    char *rewritten_payload = NULL;
    uint64_t request_seq = 0U;
    uint64_t frame_id = 0U;
    uint64_t variables_reference = 0U;
    bool success = true;
    bool ok;

    if (proxy_json_get_string_member(message->payload,
                                     message->payload_length,
                                     "type",
                                     &type) &&
        strcmp(type, "response") == 0 &&
        proxy_json_get_string_member(message->payload,
                                     message->payload_length,
                                     "command",
                                     &command)) {
        proxy_json_get_u64_member(message->payload,
                                  message->payload_length,
                                  "request_seq",
                                  &request_seq);
        proxy_json_get_bool_member(message->payload,
                                   message->payload_length,
                                   "success",
                                   &success);
        if (strcmp(command, "stackTrace") == 0) {
            if (!proxy_rewrite_stack_trace_response_payload(message->payload,
                                                            message->payload_length,
                                                            artifact,
                                                            state,
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
        } else if (strcmp(command, "scopes") == 0 &&
                   success &&
                   proxy_relay_state_take_pending_scope_request(state,
                                                                request_seq,
                                                                &frame_id) &&
                   !proxy_record_scopes_response_bindings(message->payload,
                                                          message->payload_length,
                                                          frame_id,
                                                          state)) {
            free(type);
            free(command);
            proxy_report_error(error_fd,
                               "failed to record scopes response",
                               "out of memory");
            return false;
        } else if (strcmp(command, "variables") == 0 &&
                   proxy_relay_state_take_pending_variables_request(state,
                                                                    request_seq,
                                                                    &variables_reference) &&
                   proxy_relay_state_find_scope_binding(state,
                                                        variables_reference,
                                                        &frame_id) &&
                   success) {
            const char *frame_backend_symbol = proxy_relay_state_find_frame_binding(state, frame_id);

            if (frame_backend_symbol != NULL &&
                !proxy_rewrite_variables_response_payload(message->payload,
                                                          message->payload_length,
                                                          backend_reader,
                                                          backend_stdin_fd,
                                                          output_fd,
                                                          artifact,
                                                          state,
                                                          frame_id,
                                                          frame_backend_symbol,
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
                                         "failed to forward rewritten variables response to editor");
                free(type);
                free(command);
                free(rewritten_payload);
                return ok;
            }
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
                                 FengDapRelayState *state,
                                 uint64_t *next_seq,
                                 int error_fd) {
    bool backend_stdin_closed = false;

    for (;;) {
        FengDapMessage message = {0};
        FengDapReadStatus client_status;
        FengDapReadStatus backend_status;

        if (!proxy_reader_fill_if_ready(backend_reader, error_fd)) {
            if (!backend_stdin_closed) {
                close(backend_stdin_fd);
            }
            return false;
        }

        backend_status = proxy_reader_try_read_buffered_message(backend_reader,
                                                                &message,
                                                                error_fd);
        if (backend_status == FENG_DAP_READ_OK) {
            bool ok = proxy_process_backend_relay_message(&message,
                                                          backend_reader,
                                                          backend_stdin_fd,
                                                          output_fd,
                                                          artifact,
                                                          state,
                                                          error_fd);

            proxy_message_dispose(&message);
            if (!ok) {
                return false;
            }
            if (!backend_stdin_closed &&
                client_reader->reached_eof &&
                (state == NULL || state->pending_variables_request_count == 0U)) {
                close(backend_stdin_fd);
                backend_stdin_closed = true;
            }
            continue;
        }
        if (backend_status == FENG_DAP_READ_ERROR) {
            return false;
        }

        client_status = proxy_reader_try_read_buffered_message(client_reader,
                                                               &message,
                                                               error_fd);
        if (client_status == FENG_DAP_READ_OK) {
            bool ok = proxy_process_client_relay_message(&message,
                                                         backend_reader,
                                                         backend_stdin_fd,
                                                         output_fd,
                                                         artifact,
                                                         state,
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
        if (client_status == FENG_DAP_READ_EOF &&
            !backend_stdin_closed &&
            (state == NULL || state->pending_variables_request_count == 0U)) {
            close(backend_stdin_fd);
            backend_stdin_closed = true;
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
    FengDapRelayState relay_state = {0};
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
            proxy_relay_state_dispose(&relay_state);
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
                proxy_relay_state_dispose(&relay_state);
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
                proxy_relay_state_dispose(&relay_state);
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
            proxy_relay_state_dispose(&relay_state);
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
            proxy_relay_state_dispose(&relay_state);
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
            proxy_relay_state_dispose(&relay_state);
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
            proxy_relay_state_dispose(&relay_state);
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
            proxy_relay_state_dispose(&relay_state);
            return exit_code != 0 ? exit_code : 1;
        }

        proxy_message_dispose(&initialize_request);
        proxy_message_dispose(&request);
        if (!proxy_relay_messages(&client_reader,
                                  &backend_reader,
                                  child_stdin[1],
                                  output_fd,
                                  &launch_artifact,
                                  &relay_state,
                                  &next_seq,
                                  error_fd)) {
            close(child_stdout[0]);
            exit_code = proxy_wait_for_child(child, error_fd, backend_program);
            proxy_reader_dispose(&client_reader);
            proxy_reader_dispose(&backend_reader);
            feng_debug_artifact_dispose(&launch_artifact);
            proxy_relay_state_dispose(&relay_state);
            return exit_code != 0 ? exit_code : 1;
        }
        close(child_stdout[0]);
        exit_code = proxy_wait_for_child(child, error_fd, backend_program);
        proxy_reader_dispose(&client_reader);
        proxy_reader_dispose(&backend_reader);
        feng_debug_artifact_dispose(&launch_artifact);
        proxy_relay_state_dispose(&relay_state);
        return exit_code;
    }

    proxy_message_dispose(&initialize_request);
    proxy_reader_dispose(&client_reader);
    proxy_reader_dispose(&backend_reader);
    feng_debug_artifact_dispose(&launch_artifact);
    proxy_relay_state_dispose(&relay_state);
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
