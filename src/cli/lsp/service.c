#include "cli/lsp/service.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cli/lsp/lsp_keywords.h"
#include "cli/lsp/lsp_annotations.h"
#include "cli/lsp/lsp_builtin_types.h"
#include "cli/lsp/document_store.h"
#include "cli/lsp/scheduler.h"
#include "cli/lsp/trace.h"

#include "cli/common.h"
#include "cli/deps/manager.h"
#include "cli/frontend.h"
#include "cli/project/common.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "platform/platform.h"
#include "semantic/semantic.h"
#include "symbol/provider.h"
#include "symbol/imported_module.h"

static const char kCancelledResponseMethod[] = "$/fengCancelledResponse";

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

/* Stable per-request data passed explicitly through completion builders. */
typedef struct FengLspRequestContext {
    const char *uri;
} FengLspRequestContext;

typedef struct FengLspString {
    char *data;
    size_t length;
    size_t capacity;
    /* Completion builders lazily attach an exact label set and item count. */
    char **completion_labels;
    size_t completion_label_count;
    size_t completion_label_capacity;
    size_t completion_item_count;
} FengLspString;

/* One indexed token range in the current document text. */
typedef struct FengLspTokenSpan {
    FengTokenKind kind;
    size_t offset;
    size_t length;
} FengLspTokenSpan;

/* Current editor-owned state for one open document. */
typedef struct FengLspDocument {
    char *uri;
    char *path;
    char *text;
    bool is_file;
    bool dirty;
    unsigned int version;
    FengProgram *current_program;
    bool current_parse_attempted;
    FengLspTokenSpan *tokens;
    size_t token_count;
    size_t token_capacity;
    bool tokens_indexed;
    FengLspLineIndex lines;
    /* Prefix still byte-identical to the published successful generation. */
    size_t successful_prefix_length;
    size_t successful_prefix_generation;
} FengLspDocument;

/* Immutable document snapshot consumed by one background analysis candidate. */
typedef struct FengLspAnalysisTask {
    FengLspDocument *documents;
    size_t document_count;
    size_t primary_index;
    size_t generation;
} FengLspAnalysisTask;

/* Parsed source module retained by the published workspace index. */
typedef struct FengLspIndexedModule {
    char **segments;
    size_t segment_count;
    char *source;
    FengProgram *program;
} FengLspIndexedModule;

/* Immutable module-path index built from workspace source files. */
typedef struct FengLspModuleIndex {
    FengLspIndexedModule *modules;
    size_t module_count;
    size_t module_capacity;
} FengLspModuleIndex;

/* One protocol-level secondary source location for a diagnostic. */
typedef struct FengLspDiagnosticRelatedEntry {
    char *path;
    char *message;
    unsigned int line;
    unsigned int column;
    unsigned int end_column;
} FengLspDiagnosticRelatedEntry;

/* One protocol diagnostic normalized from parser or semantic output. */
typedef struct FengLspDiagnosticEntry {
    char *path;
    char *message;
    const char *source;
    unsigned int line;
    unsigned int column;
    unsigned int end_column;
    int severity;
    FengLspDiagnosticRelatedEntry *related;
    size_t related_count;
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
    /* Owns every source path for the session lifetime. Each sources[i].path
     * and its program->path, when present, are bound to the corresponding
     * string in this array. session_dispose() frees the strings and array. */
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
    /* Optional borrowed workspace source index for edit-time queries that do
     * not require a complete semantic analysis. */
    const FengLspModuleIndex *source_module_index;
} FengLspAnalysisSession;

static bool publish_diagnostics(FILE *output,
                                const FengLspDiagnosticCollector *collector,
                                const char *path);

/* Dispatches one payload on the single interaction worker. */
static bool service_handle_payload_unlocked(FengLspService *service,
                                            FILE *output,
                                            const char *payload,
                                            size_t payload_length,
                                            FILE *errors);

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
    const FengExpr *match_op;
    const FengDecl *self_owner_decl;
} FengLspLocal;

typedef struct FengLspLocalList {
    FengLspLocal *items;
    size_t count;
    size_t capacity;
} FengLspLocalList;

typedef enum FengLspReceiverRootKind {
    FENG_LSP_RECEIVER_ROOT_IDENTIFIER = 0,
    FENG_LSP_RECEIVER_ROOT_STRING,
    FENG_LSP_RECEIVER_ROOT_INTEGER,
    FENG_LSP_RECEIVER_ROOT_FLOAT,
    FENG_LSP_RECEIVER_ROOT_BOOL
} FengLspReceiverRootKind;

typedef enum FengLspReceiverOperationKind {
    FENG_LSP_RECEIVER_MEMBER = 0,
    FENG_LSP_RECEIVER_CALL,
    FENG_LSP_RECEIVER_INDEX
} FengLspReceiverOperationKind;

/* One postfix operation in a text-derived member-completion receiver. */
typedef struct FengLspReceiverOperation {
    FengLspReceiverOperationKind kind;
    FengSlice member;
} FengLspReceiverOperation;

/* Lightweight receiver expression used when dirty parsing drops the final
 * member access. Arguments and index expressions remain in source text and
 * are skipped after delimiter validation. */
typedef struct FengLspReceiverChain {
    FengLspReceiverRootKind root_kind;
    FengSlice root;
    FengLspReceiverOperation *operations;
    size_t operation_count;
    size_t operation_capacity;
} FengLspReceiverChain;

typedef enum FengLspResolvedKind {
    FENG_LSP_RESOLVED_NONE = 0,
    FENG_LSP_RESOLVED_DECL,
    FENG_LSP_RESOLVED_MEMBER,
    FENG_LSP_RESOLVED_ENUM_ITEM,
    FENG_LSP_RESOLVED_PARAM,
    FENG_LSP_RESOLVED_BINDING,
    FENG_LSP_RESOLVED_MATCH_BINDING,
    FENG_LSP_RESOLVED_SELF,
    FENG_LSP_RESOLVED_TYPE_PARAM
} FengLspResolvedKind;

typedef struct FengLspResolvedTarget {
    FengLspResolvedKind kind;
    const FengDecl *decl;
    const FengTypeMember *member;
    const FengEnumItem *enum_item;
    const FengParameter *parameter;
    const FengBinding *binding;
    const FengExpr *match_op;
    const FengDecl *self_owner_decl;
    const FengTypeParam *type_param;       /* resolved type parameter */
    const FengDecl *type_param_owner;      /* decl owning the type parameter */
} FengLspResolvedTarget;

typedef struct FengLspCacheResolvedTarget {
    FengLspResolvedKind kind;
    const FengSymbolDeclView *decl;
    const FengSymbolDeclView *member;
    const FengParameter *parameter;
    const FengBinding *binding;
    const FengSymbolDeclView *self_owner_decl;
    const FengTypeParam *type_param;
    const FengSymbolDeclView *type_param_owner;
} FengLspCacheResolvedTarget;

typedef struct FengLspCacheQueryContext {
    FengProgram *program;
    FengSymbolProvider *provider;
    const FengSymbolImportedModule *current_module;
    const char *source_text;
    const FengLspModuleIndex *source_module_index;
    bool owns_program;
    bool owns_provider;
} FengLspCacheQueryContext;

/* Identifier character classification helpers defined by the completion
 * engine and reused by edit-time receiver parsing. */
static bool completion_identifier_start(char ch);
static bool completion_identifier_continue(char ch);
static bool slice_equals_cstr(FengSlice lhs, const char *rhs);

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

/* Semantic category shown beside a Hover signature when it is proven. */
typedef enum FengLspTypeCategory {
    FENG_LSP_TYPE_CATEGORY_UNKNOWN = 0,
    FENG_LSP_TYPE_CATEGORY_REFERENCE,
    FENG_LSP_TYPE_CATEGORY_VALUE,
    FENG_LSP_TYPE_CATEGORY_TUPLE,
    FENG_LSP_TYPE_CATEGORY_ENUM,
    FENG_LSP_TYPE_CATEGORY_OBJECT_SPEC,
    FENG_LSP_TYPE_CATEGORY_CALLBACK_SPEC,
    FENG_LSP_TYPE_CATEGORY_UNION_SPEC,
    FENG_LSP_TYPE_CATEGORY_INTERSECTION_SPEC,
    FENG_LSP_TYPE_CATEGORY_ARRAY,
    FENG_LSP_TYPE_CATEGORY_BUILTIN,
    FENG_LSP_TYPE_CATEGORY_POINTER
} FengLspTypeCategory;

/* Structured Hover content shared by plaintext and Markdown renderers. */
typedef struct FengLspHoverPresentation {
    FengLspString signature;
    const char *category_caption;
    const char *category_label;
    char *documentation;
} FengLspHoverPresentation;

struct FengLspService {
    FengLspDocument *documents;
    size_t document_count;
    size_t document_capacity;
    size_t document_revision;
    bool shutdown_requested;
    bool should_exit;
    int exit_code;
    FengLspMarkupKind hover_markup_kind;
    FILE *errors; /* diagnostics log; set at the start of each handle_payload call */
    /* The only long-lived semantic cache. A candidate replaces it only after
     * a complete successful analysis; edits and failed analyses never clear it. */
    FengLspAnalysisSession last_successful_analysis;
    size_t last_successful_generation;
    FengSymbolProvider *symbol_index;
    size_t symbol_index_generation;
    FengLspModuleIndex module_index;
    size_t module_index_generation;
    pthread_t analyzer_thread;
    pthread_t request_thread;
    pthread_mutex_t documents_mutex;
    pthread_mutex_t analysis_mutex;
    pthread_cond_t analysis_condition;
    pthread_mutex_t protocol_output_mutex;
    FILE *protocol_output;
    bool protocol_output_failed;
    bool analysis_thread_started;
    bool request_thread_started;
    bool analysis_stop_requested;
    bool analysis_task_pending;
    bool workspace_index_refresh_requested;
    size_t latest_scheduled_generation;
    size_t diagnostics_requested_generation;
    struct timespec analysis_due_time;
    char *pending_analysis_uri;
    FengLspRequestScheduler request_scheduler;
    FengLspTrace trace;
    bool exit_received;
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

/* Returns the first byte of the source line containing `offset`. */
static size_t receiver_line_start(const char *text, size_t offset) {
    while (offset > 0U && text[offset - 1U] != '\n') {
        --offset;
    }
    return offset;
}

/* Finds a line comment outside strings and same-line block comments. */
static size_t receiver_line_comment_start(const char *text,
                                          size_t start,
                                          size_t end) {
    size_t cursor = start;

    while (cursor + 1U < end) {
        if (text[cursor] == '"') {
            ++cursor;
            while (cursor < end) {
                if (text[cursor] == '\\' && cursor + 1U < end) {
                    cursor += 2U;
                } else if (text[cursor] == '"') {
                    ++cursor;
                    break;
                } else {
                    ++cursor;
                }
            }
            continue;
        }
        if (text[cursor] == '/' && text[cursor + 1U] == '*') {
            cursor += 2U;
            while (cursor + 1U < end &&
                   !(text[cursor] == '*' && text[cursor + 1U] == '/')) {
                ++cursor;
            }
            cursor = cursor + 1U < end ? cursor + 2U : end;
            continue;
        }
        if (text[cursor] == '/' && text[cursor + 1U] == '/') {
            return cursor;
        }
        ++cursor;
    }
    return end;
}

/* Finds the opening quote paired with a quote encountered while scanning
 * backward. Escaped quotes are ignored. */
static bool receiver_string_start_backward(const char *text,
                                           size_t quote_offset,
                                           size_t *out_start) {
    size_t cursor = quote_offset;

    while (cursor > 0U) {
        size_t slash_count = 0U;
        size_t slash_cursor;

        --cursor;
        if (text[cursor] != '"') {
            continue;
        }
        slash_cursor = cursor;
        while (slash_cursor > 0U && text[slash_cursor - 1U] == '\\') {
            --slash_cursor;
            ++slash_count;
        }
        if ((slash_count % 2U) == 0U) {
            *out_start = cursor;
            return true;
        }
    }
    return false;
}

/* Finds the opening delimiter paired with `close_offset` while ignoring
 * strings and comments. The scan is bounded by the receiver expression. */
static bool receiver_matching_open_backward(const char *text,
                                            size_t close_offset,
                                            char open,
                                            char close,
                                            size_t *out_open) {
    size_t cursor = close_offset;
    size_t line_start = receiver_line_start(text, cursor);
    size_t depth = 1U;

    while (cursor > 0U) {
        size_t line_end;
        size_t comment_start;
        char ch;

        if (cursor == line_start) {
            if (line_start == 0U) {
                break;
            }
            line_end = line_start - 1U;
            line_start = receiver_line_start(text, line_end);
            comment_start = receiver_line_comment_start(text, line_start, line_end);
            cursor = comment_start < line_end ? comment_start : line_end;
            continue;
        }
        --cursor;
        ch = text[cursor];
        if (ch == '"') {
            size_t string_start;

            if (!receiver_string_start_backward(text, cursor, &string_start)) {
                return false;
            }
            cursor = string_start;
            line_start = receiver_line_start(text, cursor);
            continue;
        }
        if (ch == '/' && cursor > 0U && text[cursor - 1U] == '*') {
            size_t comment_cursor = cursor - 1U;
            bool found = false;

            while (comment_cursor > 0U) {
                if (text[comment_cursor - 1U] == '/' && text[comment_cursor] == '*') {
                    cursor = comment_cursor - 1U;
                    line_start = receiver_line_start(text, cursor);
                    found = true;
                    break;
                }
                --comment_cursor;
            }
            if (!found) {
                return false;
            }
            continue;
        }
        if (ch == close) {
            ++depth;
        } else if (ch == open) {
            --depth;
            if (depth == 0U) {
                *out_open = cursor;
                return true;
            }
        }
    }
    return false;
}

/* Finds the start of a postfix receiver ending immediately before a member
 * dot. Supported postfix forms are member access, call, and index. */
static bool receiver_text_find_start(const char *text,
                                     size_t receiver_end,
                                     size_t *out_start) {
    size_t cursor = receiver_end;

    if (text == NULL || out_start == NULL) {
        return false;
    }
    while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
        --cursor;
    }
    while (cursor > 0U) {
        size_t start;
        size_t separator;
        char tail = text[cursor - 1U];

        if (tail == ')' || tail == ']') {
            if (!receiver_matching_open_backward(text,
                                                 cursor - 1U,
                                                 tail == ')' ? '(' : '[',
                                                 tail,
                                                 &start)) {
                return false;
            }
            cursor = start;
            while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
                --cursor;
            }
            continue;
        }
        if (completion_identifier_continue(tail)) {
            start = cursor - 1U;
            while (start > 0U && completion_identifier_continue(text[start - 1U])) {
                --start;
            }
            if (!completion_identifier_start(text[start]) &&
                !isdigit((unsigned char)text[start])) {
                return false;
            }
            cursor = start;
        } else if (tail == '"') {
            if (!receiver_string_start_backward(text, cursor - 1U, &start)) {
                return false;
            }
            cursor = start;
        } else {
            return false;
        }
        separator = cursor;
        while (separator > 0U && isspace((unsigned char)text[separator - 1U])) {
            --separator;
        }
        if (separator > 0U && text[separator - 1U] == '.') {
            cursor = separator - 1U;
            while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
                --cursor;
            }
            continue;
        }
        *out_start = cursor;
        return true;
    }
    return false;
}

/* Skips one balanced call or index suffix in a receiver chain. */
static bool receiver_skip_balanced_forward(FengSlice text,
                                           size_t open_offset,
                                           char open,
                                           char close,
                                           size_t *out_after) {
    size_t cursor = open_offset;
    size_t depth = 0U;

    while (cursor < text.length) {
        char ch = text.data[cursor];

        if (ch == '"') {
            ++cursor;
            while (cursor < text.length) {
                if (text.data[cursor] == '\\' && cursor + 1U < text.length) {
                    cursor += 2U;
                } else if (text.data[cursor] == '"') {
                    ++cursor;
                    break;
                } else {
                    ++cursor;
                }
            }
            continue;
        }
        if (ch == '/' && cursor + 1U < text.length && text.data[cursor + 1U] == '/') {
            cursor += 2U;
            while (cursor < text.length && text.data[cursor] != '\n') {
                ++cursor;
            }
            continue;
        }
        if (ch == '/' && cursor + 1U < text.length && text.data[cursor + 1U] == '*') {
            cursor += 2U;
            while (cursor + 1U < text.length &&
                   !(text.data[cursor] == '*' && text.data[cursor + 1U] == '/')) {
                ++cursor;
            }
            if (cursor + 1U >= text.length) {
                return false;
            }
            cursor += 2U;
            continue;
        }
        if (ch == open) {
            ++depth;
        } else if (ch == close) {
            if (depth == 0U) {
                return false;
            }
            --depth;
            if (depth == 0U) {
                *out_after = cursor + 1U;
                return true;
            }
        }
        ++cursor;
    }
    return false;
}

/* Releases operations owned by a receiver chain. */
static void receiver_chain_dispose(FengLspReceiverChain *chain) {
    if (chain == NULL) {
        return;
    }
    free(chain->operations);
    memset(chain, 0, sizeof(*chain));
}

/* Parses a bounded receiver expression into root + postfix operations. */
static bool receiver_chain_parse(FengSlice text, FengLspReceiverChain *chain) {
    size_t cursor = 0U;

    if (chain == NULL || text.data == NULL || text.length == 0U) {
        return false;
    }
    memset(chain, 0, sizeof(*chain));
    while (cursor < text.length && isspace((unsigned char)text.data[cursor])) {
        ++cursor;
    }
    if (cursor >= text.length) {
        return false;
    }
    if (completion_identifier_start(text.data[cursor])) {
        size_t start = cursor;

        ++cursor;
        while (cursor < text.length && completion_identifier_continue(text.data[cursor])) {
            ++cursor;
        }
        chain->root.data = text.data + start;
        chain->root.length = cursor - start;
        chain->root_kind = slice_equals_cstr(chain->root, "true") ||
                                   slice_equals_cstr(chain->root, "false")
                               ? FENG_LSP_RECEIVER_ROOT_BOOL
                               : FENG_LSP_RECEIVER_ROOT_IDENTIFIER;
    } else if (text.data[cursor] == '"') {
        size_t start = cursor++;

        while (cursor < text.length) {
            if (text.data[cursor] == '\\' && cursor + 1U < text.length) {
                cursor += 2U;
            } else if (text.data[cursor] == '"') {
                ++cursor;
                break;
            } else {
                ++cursor;
            }
        }
        if (cursor > text.length || text.data[cursor - 1U] != '"') {
            return false;
        }
        chain->root.data = text.data + start;
        chain->root.length = cursor - start;
        chain->root_kind = FENG_LSP_RECEIVER_ROOT_STRING;
    } else if (isdigit((unsigned char)text.data[cursor])) {
        size_t start = cursor;
        bool is_float = false;

        while (cursor < text.length && isdigit((unsigned char)text.data[cursor])) {
            ++cursor;
        }
        if (cursor + 1U < text.length && text.data[cursor] == '.' &&
            isdigit((unsigned char)text.data[cursor + 1U])) {
            is_float = true;
            ++cursor;
            while (cursor < text.length && isdigit((unsigned char)text.data[cursor])) {
                ++cursor;
            }
        }
        chain->root.data = text.data + start;
        chain->root.length = cursor - start;
        chain->root_kind = is_float ? FENG_LSP_RECEIVER_ROOT_FLOAT
                                    : FENG_LSP_RECEIVER_ROOT_INTEGER;
    } else {
        return false;
    }
    while (cursor < text.length) {
        FengLspReceiverOperation operation = {0};

        while (cursor < text.length && isspace((unsigned char)text.data[cursor])) {
            ++cursor;
        }
        if (cursor >= text.length) {
            break;
        }
        if (text.data[cursor] == '.') {
            size_t start;

            ++cursor;
            while (cursor < text.length && isspace((unsigned char)text.data[cursor])) {
                ++cursor;
            }
            start = cursor;
            if (cursor >= text.length || !completion_identifier_start(text.data[cursor])) {
                receiver_chain_dispose(chain);
                return false;
            }
            ++cursor;
            while (cursor < text.length && completion_identifier_continue(text.data[cursor])) {
                ++cursor;
            }
            operation.kind = FENG_LSP_RECEIVER_MEMBER;
            operation.member.data = text.data + start;
            operation.member.length = cursor - start;
        } else if (text.data[cursor] == '(' || text.data[cursor] == '[') {
            char open = text.data[cursor];
            size_t after;

            if (!receiver_skip_balanced_forward(text,
                                                cursor,
                                                open,
                                                open == '(' ? ')' : ']',
                                                &after)) {
                receiver_chain_dispose(chain);
                return false;
            }
            operation.kind = open == '(' ? FENG_LSP_RECEIVER_CALL
                                         : FENG_LSP_RECEIVER_INDEX;
            cursor = after;
        } else {
            receiver_chain_dispose(chain);
            return false;
        }
        if (!append_raw((void **)&chain->operations,
                        &chain->operation_count,
                        &chain->operation_capacity,
                        sizeof(operation),
                        &operation)) {
            receiver_chain_dispose(chain);
            return false;
        }
    }
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
    size_t index;

    for (index = 0U; index < buffer->completion_label_capacity; ++index) {
        free(buffer->completion_labels[index]);
    }
    free(buffer->completion_labels);
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
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

/* Decodes one JSON string, including the valid empty-string value. */
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
    return out.data != NULL ? out.data : dup_cstr("");
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

/* Reports cancellation for the request currently executing on this service. */
static bool request_is_cancelled(FengLspService *service, FengLspJsonValue id) {
    char *request_id;
    bool cancelled;

    if (service == NULL || id.start == NULL || id.end == NULL) {
        return false;
    }
    request_id = dup_range(id.start, id.end);
    if (request_id == NULL) {
        return false;
    }
    cancelled = feng_lsp_scheduler_active_cancelled(&service->request_scheduler,
                                                    request_id);
    free(request_id);
    return cancelled;
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

static FengLspDocument *find_document(FengLspService *service, const char *uri) {
    size_t index;

    for (index = 0U; index < service->document_count; ++index) {
        if (strcmp(service->documents[index].uri, uri) == 0) {
            return &service->documents[index];
        }
    }
    return NULL;
}

/* Releases data derived from a document text version before that text changes. */
static void document_dispose_derived(FengLspDocument *document) {
    if (document == NULL) {
        return;
    }
    feng_program_free(document->current_program);
    document->current_program = NULL;
    document->current_parse_attempted = false;
    free(document->tokens);
    document->tokens = NULL;
    document->token_count = 0U;
    document->token_capacity = 0U;
    document->tokens_indexed = false;
}

/* Parses the current document at most once for its current text version. */
static const FengProgram *ensure_document_parse(FengLspDocument *document) {
    FengParseError error = {0};

    if (document == NULL || document->text == NULL) {
        return NULL;
    }
    if (!document->current_parse_attempted) {
        document->current_parse_attempted = true;
        if (!feng_parse_source(document->text,
                               strlen(document->text),
                               document->path,
                               &document->current_program,
                               &error)) {
            feng_program_free(document->current_program);
            document->current_program = NULL;
        }
    }
    return document->current_program;
}

static bool upsert_document(FengLspService *service,
                            const char *uri,
                            const char *text,
                            unsigned int version) {
    FengLspDocument *document = find_document(service, uri);

    if (document == NULL) {
        FengLspDocument created = {0};

        created.uri = dup_cstr(uri);
        created.path = uri_to_path(uri, &created.is_file);
        created.text = dup_cstr(text != NULL ? text : "");
        created.version = version;
        if (created.uri == NULL || created.path == NULL || created.text == NULL ||
            !feng_lsp_line_index_rebuild(&created.lines, created.text) ||
            !append_raw((void **)&service->documents,
                        &service->document_count,
                        &service->document_capacity,
                        sizeof(created),
                        &created)) {
            if (service->errors != NULL) {
                fprintf(service->errors,
                        "lsp: out of memory storing document '%s'\n",
                        uri != NULL ? uri : "(null)");
            }
            free(created.uri);
            free(created.path);
            free(created.text);
            feng_lsp_line_index_dispose(&created.lines);
            return false;
        }
        ++service->document_revision;
        return true;
    }

    {
        char *updated_text = dup_cstr(text != NULL ? text : "");
        FengLspLineIndex updated_lines = {0};
        size_t old_length;
        size_t new_length;
        size_t shared_length;
        size_t shared_prefix = 0U;

        if (updated_text == NULL ||
            !feng_lsp_line_index_rebuild(&updated_lines, updated_text)) {
            if (service->errors != NULL) {
                fprintf(service->errors,
                        "lsp: out of memory updating document text for '%s'\n",
                        uri != NULL ? uri : "(null)");
            }
            free(updated_text);
            feng_lsp_line_index_dispose(&updated_lines);
            return false;
        }
        old_length = strlen(document->text);
        new_length = strlen(updated_text);
        shared_length = old_length < new_length ? old_length : new_length;
        while (shared_prefix < shared_length &&
               document->text[shared_prefix] == updated_text[shared_prefix]) {
            ++shared_prefix;
        }
        if (document->successful_prefix_length > shared_prefix) {
            document->successful_prefix_length = shared_prefix;
        }
        document_dispose_derived(document);
        free(document->text);
        feng_lsp_line_index_dispose(&document->lines);
        document->text = updated_text;
        document->lines = updated_lines;
    }
    document->dirty = true;
    document->version = version;
    ++service->document_revision;
    return true;
}

/* Mark live documents that exactly match a newly published successful task. */
static void mark_successful_document_prefixes(FengLspService *service,
                                              const FengLspAnalysisTask *task) {
    size_t document_index;

    if (service == NULL || task == NULL) {
        return;
    }
    pthread_mutex_lock(&service->documents_mutex);
    pthread_mutex_lock(&service->analysis_mutex);
    if (service->last_successful_generation != task->generation) {
        pthread_mutex_unlock(&service->analysis_mutex);
        pthread_mutex_unlock(&service->documents_mutex);
        return;
    }
    for (document_index = 0U;
         document_index < service->document_count;
         ++document_index) {
        FengLspDocument *document = &service->documents[document_index];
        size_t snapshot_index;

        for (snapshot_index = 0U; snapshot_index < task->document_count; ++snapshot_index) {
            const FengLspDocument *snapshot = &task->documents[snapshot_index];
            size_t text_length;

            if (document->path == NULL || snapshot->path == NULL ||
                strcmp(document->path, snapshot->path) != 0) {
                continue;
            }
            text_length = strlen(document->text);
            if (text_length == strlen(snapshot->text) &&
                memcmp(document->text, snapshot->text, text_length) == 0) {
                document->successful_prefix_length = text_length;
                document->successful_prefix_generation = task->generation;
            }
            break;
        }
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    pthread_mutex_unlock(&service->documents_mutex);
}

static void remove_document(FengLspService *service, const char *uri) {
    size_t index;

    for (index = 0U; index < service->document_count; ++index) {
        if (strcmp(service->documents[index].uri, uri) == 0) {
            document_dispose_derived(&service->documents[index]);
            free(service->documents[index].uri);
            free(service->documents[index].path);
            free(service->documents[index].text);
            feng_lsp_line_index_dispose(&service->documents[index].lines);
            if (index + 1U < service->document_count) {
                memmove(&service->documents[index],
                        &service->documents[index + 1U],
                        (service->document_count - index - 1U) * sizeof(service->documents[0]));
            }
            --service->document_count;
            ++service->document_revision;
            return;
        }
    }
}

static void diagnostics_dispose(FengLspDiagnosticCollector *collector) {
    size_t index;

    for (index = 0U; index < collector->count; ++index) {
        for (size_t related_index = 0U;
             related_index < collector->items[index].related_count;
             ++related_index) {
            free(collector->items[index].related[related_index].path);
            free(collector->items[index].related[related_index].message);
        }
        free(collector->items[index].related);
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

/* Append one structured secondary location to the latest diagnostic. */
static bool diagnostics_append_related(FengLspDiagnosticCollector *collector,
                                       const char *path,
                                       unsigned int line,
                                       unsigned int column,
                                       size_t token_length,
                                       const char *message) {
    FengLspDiagnosticEntry *diagnostic;
    FengLspDiagnosticRelatedEntry *grown;
    FengLspDiagnosticRelatedEntry entry = {0};

    if (collector == NULL || collector->count == 0U) {
        return false;
    }
    diagnostic = &collector->items[collector->count - 1U];
    entry.path = dup_cstr(path != NULL ? path : "");
    entry.message = dup_cstr(message != NULL ? message : "related location");
    entry.line = line == 0U ? 1U : line;
    entry.column = column == 0U ? 1U : column;
    entry.end_column =
        entry.column + (unsigned int)(token_length > 0U ? token_length : 1U);
    if (entry.path == NULL || entry.message == NULL) {
        free(entry.path);
        free(entry.message);
        return false;
    }
    grown = (FengLspDiagnosticRelatedEntry *)realloc(
        diagnostic->related,
        (diagnostic->related_count + 1U) * sizeof(*grown));
    if (grown == NULL) {
        free(entry.path);
        free(entry.message);
        return false;
    }
    diagnostic->related = grown;
    diagnostic->related[diagnostic->related_count++] = entry;
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
    if (!diagnostics_append(collector,
                            error->path,
                            error->token.line,
                            error->token.column,
                            error->token.length,
                            1,
                            "semantic",
                            error->message)) {
        return;
    }
    for (size_t index = 0U;
         index < error->related_location_count;
         ++index) {
        const FengSemanticRelatedLocation *related =
            &error->related_locations[index];

        if (!diagnostics_append_related(collector,
                                        related->path,
                                        related->token.line,
                                        related->token.column,
                                        related->token.length,
                                        related->message)) {
            return;
        }
    }
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

static bool build_overlays(const FengLspService *service,
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

    for (index = 0U; index < service->document_count; ++index) {
        const FengLspDocument *document = &service->documents[index];
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

/* Binds every loaded source and program to its session-owned path. */
static bool session_bind_owned_source_paths(FengLspAnalysisSession *session) {
    size_t index;

    if (session == NULL ||
        session->source_count != session->owned_source_path_count ||
        (session->source_count > 0U &&
         (session->sources == NULL || session->owned_source_paths == NULL))) {
        return false;
    }
    for (index = 0U; index < session->source_count; ++index) {
        const char *owned_path = session->owned_source_paths[index];

        if (owned_path == NULL || session->sources[index].path == NULL ||
            strcmp(owned_path, session->sources[index].path) != 0) {
            return false;
        }
    }

    for (index = 0U; index < session->source_count; ++index) {
        const char *owned_path = session->owned_source_paths[index];

        session->sources[index].path = owned_path;
        if (session->sources[index].program != NULL) {
            session->sources[index].program->path = owned_path;
        }
    }
    return true;
}

static bool build_standalone_session(const FengLspService *service,
                                     const FengLspDocument *document,
                                     FengLspAnalysisSession *session) {
    FengCliFrontendSourceOverlay *overlays = NULL;
    size_t overlay_count = 0U;
    char *paths[1];
    FengCliFrontendInput input = {0};
    FengCliFrontendCallbacks callbacks = {0};
    FengCliFrontendOutputs outputs = {0};

    if (!build_overlays(service, NULL, document, &overlays, &overlay_count)) {
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
    if (session->sources != NULL && session->source_count > 0U) {
        session->owned_source_paths = (char **)calloc(1U,
                                                      sizeof(session->owned_source_paths[0]));
        if (session->owned_source_paths == NULL) {
            return false;
        }
        session->owned_source_paths[0] = dup_cstr(document->path);
        if (session->owned_source_paths[0] == NULL) {
            return false;
        }
        session->owned_source_path_count = 1U;
        return session_bind_owned_source_paths(session);
    }
    return true;
}

static bool build_project_session(const FengLspService *service,
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
        return build_standalone_session(service, document, session);
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
    if (!build_overlays(service, manifest_path, document, &overlays, &overlay_count)) {
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
    /* Transfer project paths to the session before disposing the context. */
    session->owned_source_paths = context.source_paths;
    session->owned_source_path_count = context.source_count;
    context.source_paths = NULL;
    context.source_count = 0U;
    if (session->sources != NULL && session->source_count > 0U &&
        !session_bind_owned_source_paths(session)) {
        feng_cli_project_context_dispose(&context);
        feng_cli_project_error_dispose(&error);
        return false;
    }
    feng_cli_project_context_dispose(&context);
    feng_cli_project_error_dispose(&error);
    return true;
}

static bool build_analysis_session(const FengLspService *service,
                                   const FengLspDocument *document,
                                   FengLspAnalysisSession *session) {
    char *manifest_path = NULL;
    FengCliProjectError error = {0};

    memset(session, 0, sizeof(*session));
    if (document->is_file && file_exists(document->path) &&
        feng_cli_project_find_manifest_in_ancestors(document->path, &manifest_path, &error)) {
        bool ok = build_project_session(service, document, manifest_path, session);

        free(manifest_path);
        feng_cli_project_error_dispose(&error);
        return ok;
    }
    free(manifest_path);
    feng_cli_project_error_dispose(&error);
    return build_standalone_session(service, document, session);
}

static const FengProgram *find_program(const FengLspAnalysisSession *session,
                                       const char *path);

/* Releases a background analysis task and all copied document texts. */
static void analysis_task_dispose(FengLspAnalysisTask *task) {
    size_t index;

    if (task == NULL) {
        return;
    }
    for (index = 0U; index < task->document_count; ++index) {
        free(task->documents[index].uri);
        free(task->documents[index].path);
        free(task->documents[index].text);
    }
    free(task->documents);
    memset(task, 0, sizeof(*task));
}

/* Copies the current document set into an immutable candidate task. */
static bool analysis_task_clone(const FengLspService *service,
                                const FengLspDocument *primary,
                                FengLspAnalysisTask *task) {
    size_t index;

    memset(task, 0, sizeof(*task));
    if (service == NULL || primary == NULL || service->document_count == 0U) {
        return false;
    }
    task->documents = (FengLspDocument *)calloc(service->document_count,
                                                sizeof(task->documents[0]));
    if (task->documents == NULL) {
        return false;
    }
    task->document_count = service->document_count;
    task->generation = service->document_revision;
    for (index = 0U; index < service->document_count; ++index) {
        const FengLspDocument *source = &service->documents[index];
        FengLspDocument *copy = &task->documents[index];

        copy->uri = dup_cstr(source->uri);
        copy->path = dup_cstr(source->path);
        copy->text = dup_cstr(source->text);
        copy->is_file = source->is_file;
        copy->dirty = source->dirty;
        if (copy->uri == NULL || copy->path == NULL || copy->text == NULL) {
            analysis_task_dispose(task);
            return false;
        }
        if (source == primary) {
            task->primary_index = index;
        }
    }
    return true;
}

/* Builds an immutable workspace/dependency symbol index outside request paths. */
static FengSymbolProvider *build_symbol_index_candidate(const FengLspDocument *document) {
    FengSymbolProvider *provider = NULL;
    FengSymbolError symbol_error = {0};
    FengCliProjectError project_error = {0};
    FengCliProjectContext project = {0};
    FengCliDepsResolved resolved = {0};
    char *manifest_path = NULL;
    char *host_platform = NULL;
    char *platform_out_root = NULL;
    char *symbols_root = NULL;
    size_t index;

    if (document == NULL || !feng_symbol_provider_create(&provider, &symbol_error)) {
        feng_symbol_error_free(&symbol_error);
        return NULL;
    }
    if (!document->is_file || !file_exists(document->path)) {
        feng_cli_project_error_dispose(&project_error);
        free(manifest_path);
        return provider;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(document->path,
                                                     &manifest_path,
                                                     &project_error)) {
        feng_symbol_provider_free(provider);
        feng_cli_project_error_dispose(&project_error);
        free(manifest_path);
        return NULL;
    }
    if (!feng_cli_project_open(manifest_path, &project, &project_error) ||
        !feng_cli_deps_resolve_for_manifest("feng",
                                            manifest_path,
                                            false,
                                            false,
                                            &resolved,
                                            &project_error)) {
        feng_symbol_provider_free(provider);
        provider = NULL;
        goto cleanup;
    }
    for (index = 0U; index < resolved.package_count; ++index) {
        if (!feng_symbol_provider_add_bundle(provider,
                                             resolved.package_paths[index],
                                             &symbol_error)) {
            feng_symbol_provider_free(provider);
            provider = NULL;
            goto cleanup;
        }
    }
    if (feng_platform_detect_host_platform(&host_platform, NULL)) {
        platform_out_root = feng_cli_project_platform_out_root(
            &project,
            host_platform);
    }
    symbols_root = platform_out_root != NULL
        ? path_join(platform_out_root, "obj/symbols")
        : NULL;
    if (symbols_root != NULL && path_is_directory(symbols_root) &&
        !feng_symbol_provider_add_ft_root(provider,
                                          symbols_root,
                                          FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                                          &symbol_error)) {
        feng_symbol_provider_free(provider);
        provider = NULL;
    }

cleanup:
    free(symbols_root);
    free(platform_out_root);
    free(host_platform);
    free(manifest_path);
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_context_dispose(&project);
    feng_cli_project_error_dispose(&project_error);
    feng_symbol_error_free(&symbol_error);
    return provider;
}

/* Input owned by one dependency-symbol indexing worker. */
typedef struct FengLspSymbolIndexJob {
    FengLspService *service;
    const FengLspDocument *document;
    size_t generation;
} FengLspSymbolIndexJob;

/* Builds the dependency symbol provider concurrently with source indexing. */
static void *symbol_index_worker_main(void *user) {
    FengLspSymbolIndexJob *job = (FengLspSymbolIndexJob *)user;
    FengSymbolProvider *candidate = build_symbol_index_candidate(job->document);
    FengSymbolProvider *previous = NULL;

    if (candidate != NULL) {
        pthread_mutex_lock(&job->service->analysis_mutex);
        if (job->generation > job->service->symbol_index_generation) {
            previous = job->service->symbol_index;
            job->service->symbol_index = candidate;
            candidate = NULL;
            job->service->symbol_index_generation = job->generation;
        }
        pthread_cond_broadcast(&job->service->analysis_condition);
        pthread_mutex_unlock(&job->service->analysis_mutex);
    }
    feng_symbol_provider_free(previous);
    feng_symbol_provider_free(candidate);
    return NULL;
}

/* Releases all owned module-path strings in an index. */
static void module_index_dispose(FengLspModuleIndex *index) {
    size_t module_index;

    if (index == NULL) {
        return;
    }
    for (module_index = 0U; module_index < index->module_count; ++module_index) {
        FengLspIndexedModule *module = &index->modules[module_index];
        size_t segment_index;

        for (segment_index = 0U; segment_index < module->segment_count; ++segment_index) {
            free(module->segments[segment_index]);
        }
        free(module->segments);
        feng_program_free(module->program);
        free(module->source);
    }
    free(index->modules);
    memset(index, 0, sizeof(*index));
}

/* Copies one parsed module path into an immutable workspace index. */
static bool module_index_append_program(FengLspModuleIndex *index,
                                        char *source,
                                        FengProgram *program) {
    FengLspIndexedModule module = {0};
    size_t segment_index;

    if (program == NULL || program->module_segment_count == 0U) {
        return true;
    }
    module.segments = (char **)calloc(program->module_segment_count,
                                     sizeof(module.segments[0]));
    if (module.segments == NULL) {
        return false;
    }
    module.segment_count = program->module_segment_count;
    module.source = source;
    module.program = program;
    for (segment_index = 0U; segment_index < module.segment_count; ++segment_index) {
        FengSlice segment = program->module_segments[segment_index];

        module.segments[segment_index] =
            dup_range(segment.data, segment.data + segment.length);
        if (module.segments[segment_index] == NULL) {
            size_t dispose_index;

            for (dispose_index = 0U; dispose_index < module.segment_count; ++dispose_index) {
                free(module.segments[dispose_index]);
            }
            free(module.segments);
            return false;
        }
    }
    if (!append_raw((void **)&index->modules,
                    &index->module_count,
                    &index->module_capacity,
                    sizeof(module),
                    &module)) {
        for (segment_index = 0U; segment_index < module.segment_count; ++segment_index) {
            free(module.segments[segment_index]);
        }
        free(module.segments);
        return false;
    }
    return true;
}

/* Manifest identities already visited by recursive local-source indexing. */
typedef struct FengLspVisitedProjects {
    char **manifest_paths;
    size_t count;
    size_t capacity;
} FengLspVisitedProjects;

/* Releases paths used to prevent recursive local-dependency cycles. */
static void visited_projects_dispose(FengLspVisitedProjects *visited) {
    size_t index;

    for (index = 0U; index < visited->count; ++index) {
        free(visited->manifest_paths[index]);
    }
    free(visited->manifest_paths);
    memset(visited, 0, sizeof(*visited));
}

/* Recursively indexes one project and its local source dependencies. */
static bool module_index_scan_project(const char *path_arg,
                                      FengLspModuleIndex *index,
                                      FengLspVisitedProjects *visited) {
    FengCliProjectContext project = {0};
    FengCliProjectError project_error = {0};
    size_t source_index;
    bool ok = true;

    if (!feng_cli_project_open(path_arg, &project, &project_error)) {
        feng_cli_project_error_dispose(&project_error);
        return false;
    }
    for (source_index = 0U; source_index < visited->count; ++source_index) {
        if (strcmp(visited->manifest_paths[source_index], project.manifest_path) == 0) {
            feng_cli_project_context_dispose(&project);
            feng_cli_project_error_dispose(&project_error);
            return true;
        }
    }
    {
        char *manifest_copy = dup_cstr(project.manifest_path);

        if (manifest_copy == NULL ||
            !append_raw((void **)&visited->manifest_paths,
                        &visited->count,
                        &visited->capacity,
                        sizeof(manifest_copy),
                        &manifest_copy)) {
            free(manifest_copy);
            ok = false;
            goto cleanup;
        }
    }
    for (source_index = 0U; source_index < project.source_count; ++source_index) {
        const char *path = project.source_paths[source_index];
        size_t source_length = 0U;
        char *source = feng_cli_read_entire_file(path, &source_length);
        FengProgram *program = NULL;
        FengParseError parse_error = {0};

        if (source != NULL &&
            feng_parse_source(source, source_length, path, &program, &parse_error) &&
            program != NULL && program->module_segment_count > 0U) {
            if (!module_index_append_program(index, source, program)) {
                ok = false;
            } else {
                source = NULL;
                program = NULL;
            }
        }
        feng_program_free(program);
        free(source);
        if (!ok) {
            break;
        }
    }
    for (source_index = 0U;
         ok && source_index < project.manifest.dependency_count;
         ++source_index) {
        const FengCliProjectManifestDependency *dependency =
            &project.manifest.dependencies[source_index];
        char *dependency_path;

        if (!dependency->is_local_path) {
            continue;
        }
        dependency_path = path_join(project.project_root, dependency->value);
        if (dependency_path != NULL && path_is_directory(dependency_path)) {
            ok = module_index_scan_project(dependency_path, index, visited);
        }
        free(dependency_path);
    }

cleanup:
    feng_cli_project_context_dispose(&project);
    feng_cli_project_error_dispose(&project_error);
    return ok;
}

/* Builds workspace module paths in the background from current project files. */
static bool build_module_index_candidate(const FengLspDocument *document,
                                         FengLspModuleIndex *index) {
    char *manifest_path = NULL;
    FengCliProjectError project_error = {0};
    FengLspVisitedProjects visited = {0};
    bool ok;

    memset(index, 0, sizeof(*index));
    if (document == NULL || !document->is_file || !file_exists(document->path)) {
        feng_cli_project_error_dispose(&project_error);
        free(manifest_path);
        return true;
    }
    if (!feng_cli_project_find_manifest_in_ancestors(document->path,
                                                     &manifest_path,
                                                     &project_error)) {
        feng_cli_project_error_dispose(&project_error);
        free(manifest_path);
        return false;
    }
    ok = module_index_scan_project(manifest_path, index, &visited);
    free(manifest_path);
    feng_cli_project_error_dispose(&project_error);
    visited_projects_dispose(&visited);
    if (!ok) {
        module_index_dispose(index);
    }
    return ok;
}

/* Input owned by one source-module indexing worker. */
typedef struct FengLspModuleIndexJob {
    FengLspService *service;
    const FengLspDocument *document;
    size_t generation;
} FengLspModuleIndexJob;

/* Builds source-module metadata independently from full semantic analysis. */
static void *module_index_worker_main(void *user) {
    FengLspModuleIndexJob *job = (FengLspModuleIndexJob *)user;
    FengLspModuleIndex candidate = {0};
    FengLspModuleIndex previous = {0};

    if (build_module_index_candidate(job->document, &candidate)) {
        pthread_mutex_lock(&job->service->analysis_mutex);
        if (job->generation > job->service->module_index_generation) {
            previous = job->service->module_index;
            job->service->module_index = candidate;
            memset(&candidate, 0, sizeof(candidate));
            job->service->module_index_generation = job->generation;
        }
        pthread_cond_broadcast(&job->service->analysis_condition);
        pthread_mutex_unlock(&job->service->analysis_mutex);
    }
    module_index_dispose(&previous);
    module_index_dispose(&candidate);
    return NULL;
}

/* Reports whether full analysis found diagnostics beyond single-file parsing. */
static bool diagnostics_has_analysis_result(const FengLspDiagnosticCollector *collector) {
    size_t index;

    for (index = 0U; index < collector->count; ++index) {
        if (strcmp(collector->items[index].source, "parse") != 0) {
            return true;
        }
    }
    return false;
}

/* Reports whether one absolute realtime deadline is still in the future. */
static bool analysis_deadline_is_future(const struct timespec *deadline) {
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return false;
    }
    return now.tv_sec < deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec < deadline->tv_nsec);
}

/* Runs full candidates serially and atomically publishes only successful ones. */
static void *background_analyzer_main(void *user) {
    FengLspService *service = (FengLspService *)user;

    for (;;) {
        FengLspAnalysisTask task = {0};
        FengLspAnalysisSession candidate = {0};
        FengLspAnalysisSession previous = {0};
        FengLspDiagnosticCollector analysis_diagnostics = {0};
        FengLspService snapshot_service = {0};
        FengLspSymbolIndexJob symbol_job = {0};
        FengLspModuleIndexJob module_job = {0};
        pthread_t module_thread;
        bool module_thread_started = false;
        bool analysis_built = false;
        bool publish = false;
        bool refresh_workspace_index = false;
        bool task_ready = false;

        while (!task_ready) {
            int wait_status = 0;

            pthread_mutex_lock(&service->analysis_mutex);
            while (!service->analysis_stop_requested && !service->analysis_task_pending) {
                pthread_cond_wait(&service->analysis_condition, &service->analysis_mutex);
            }
            while (!service->analysis_stop_requested &&
                   service->analysis_task_pending &&
                   wait_status != ETIMEDOUT) {
                wait_status = pthread_cond_timedwait(&service->analysis_condition,
                                                     &service->analysis_mutex,
                                                     &service->analysis_due_time);
            }
            if (service->analysis_stop_requested) {
                pthread_mutex_unlock(&service->analysis_mutex);
                break;
            }
            pthread_mutex_unlock(&service->analysis_mutex);

            pthread_mutex_lock(&service->documents_mutex);
            pthread_mutex_lock(&service->analysis_mutex);
            if (service->analysis_task_pending &&
                !analysis_deadline_is_future(&service->analysis_due_time)) {
                FengLspDocument *primary = find_document(service,
                                                         service->pending_analysis_uri);

                task_ready = primary != NULL &&
                             analysis_task_clone(service, primary, &task);
                free(service->pending_analysis_uri);
                service->pending_analysis_uri = NULL;
                service->analysis_task_pending = false;
                refresh_workspace_index = service->workspace_index_refresh_requested ||
                                          service->symbol_index == NULL ||
                                          service->module_index_generation == 0U;
                service->workspace_index_refresh_requested = false;
            }
            pthread_mutex_unlock(&service->analysis_mutex);
            pthread_mutex_unlock(&service->documents_mutex);
        }
        if (service->analysis_stop_requested) {
            break;
        }

        snapshot_service.documents = task.documents;
        snapshot_service.document_count = task.document_count;
        if (refresh_workspace_index && task.primary_index < task.document_count) {
            symbol_job.service = service;
            symbol_job.document = &task.documents[task.primary_index];
            symbol_job.generation = task.generation;
            module_job.service = service;
            module_job.document = &task.documents[task.primary_index];
            module_job.generation = task.generation;
            module_thread_started =
                pthread_create(&module_thread,
                               NULL,
                               module_index_worker_main,
                               &module_job) == 0;
            /* Provider loading and full semantic analysis can touch the same
             * compiler cache artifacts. Publish the provider first instead of
             * racing those operations against each other. */
            (void)symbol_index_worker_main(&symbol_job);
        }
        /* Source-module parsing owns independent source snapshots and may run
         * concurrently with the semantic candidate. */
        if (task.primary_index < task.document_count) {
            analysis_built = build_analysis_session(&snapshot_service,
                                                    &task.documents[task.primary_index],
                                                    &candidate);
        }
        if (analysis_built) {
            analysis_diagnostics = candidate.diagnostics;
            memset(&candidate.diagnostics, 0, sizeof(candidate.diagnostics));
        }
        if (analysis_built && candidate.exit_code == 0 && candidate.analysis != NULL) {
            pthread_mutex_lock(&service->analysis_mutex);
            if (task.generation > service->last_successful_generation) {
                previous = service->last_successful_analysis;
                service->last_successful_analysis = candidate;
                memset(&candidate, 0, sizeof(candidate));
                service->last_successful_generation = task.generation;
                publish = true;
            }
            pthread_cond_broadcast(&service->analysis_condition);
            pthread_mutex_unlock(&service->analysis_mutex);
        }
        if (publish) {
            mark_successful_document_prefixes(service, &task);
            session_dispose(&previous);
        }
        if (analysis_built) {
            bool diagnostics_are_current;

            pthread_mutex_lock(&service->protocol_output_mutex);
            pthread_mutex_lock(&service->analysis_mutex);
            diagnostics_are_current =
                task.generation == service->latest_scheduled_generation &&
                task.generation == service->diagnostics_requested_generation;
            if (diagnostics_are_current) {
                service->diagnostics_requested_generation = 0U;
            }
            pthread_mutex_unlock(&service->analysis_mutex);
            if (diagnostics_are_current &&
                diagnostics_has_analysis_result(&analysis_diagnostics) &&
                service->protocol_output != NULL &&
                !publish_diagnostics(service->protocol_output,
                                     &analysis_diagnostics,
                                     task.documents[task.primary_index].path)) {
                service->protocol_output_failed = true;
            }
            pthread_mutex_unlock(&service->protocol_output_mutex);
        }
        diagnostics_dispose(&analysis_diagnostics);

        if (module_thread_started) {
            (void)pthread_join(module_thread, NULL);
        } else if (refresh_workspace_index && task.primary_index < task.document_count) {
            (void)module_index_worker_main(&module_job);
        }

        session_dispose(&candidate);
        analysis_task_dispose(&task);
    }
    return NULL;
}

/* Replaces any not-yet-started candidate with the newest document generation. */
static void schedule_background_analysis(FengLspService *service,
                                         const FengLspDocument *primary,
                                         bool diagnostics_requested,
                                         bool refresh_workspace_index,
                                         bool debounce) {
    char *candidate_uri;
    char *previous_uri;
    size_t generation;

    if (service == NULL || primary == NULL || !service->analysis_thread_started) {
        return;
    }
    candidate_uri = dup_cstr(primary->uri);
    if (candidate_uri == NULL) {
        return;
    }
    generation = service->document_revision;
    pthread_mutex_lock(&service->analysis_mutex);
    previous_uri = service->pending_analysis_uri;
    service->pending_analysis_uri = candidate_uri;
    service->analysis_task_pending = true;
    service->latest_scheduled_generation = generation;
    if (clock_gettime(CLOCK_REALTIME, &service->analysis_due_time) == 0 && debounce) {
        service->analysis_due_time.tv_nsec += 75L * 1000L * 1000L;
        if (service->analysis_due_time.tv_nsec >= 1000L * 1000L * 1000L) {
            ++service->analysis_due_time.tv_sec;
            service->analysis_due_time.tv_nsec -= 1000L * 1000L * 1000L;
        }
    } else {
        service->analysis_due_time.tv_sec = 0;
        service->analysis_due_time.tv_nsec = 0;
    }
    service->workspace_index_refresh_requested =
        service->workspace_index_refresh_requested || refresh_workspace_index;
    if (diagnostics_requested) {
        service->diagnostics_requested_generation = generation;
    }
    pthread_cond_signal(&service->analysis_condition);
    pthread_mutex_unlock(&service->analysis_mutex);
    free(previous_uri);
}

static void cache_query_context_dispose(FengLspCacheQueryContext *context) {
    if (context == NULL) {
        return;
    }
    if (context->owns_program) {
        feng_program_free(context->program);
    }
    if (context->owns_provider) {
        feng_symbol_provider_free(context->provider);
    }
    memset(context, 0, sizeof(*context));
}

/* Builds a query context using only current memory state. The caller holds
 * analysis_mutex for the full lifetime of the returned borrowed provider. */
static bool build_persistent_cache_query_context(FengLspService *service,
                                                 FengLspDocument *document,
                                                 const char *source_text,
                                                 FengLspCacheQueryContext *context) {
    FengParseError parse_error = {0};

    memset(context, 0, sizeof(*context));
    if (service == NULL || document == NULL || source_text == NULL ||
        service->symbol_index == NULL ||
        feng_symbol_provider_module_count(service->symbol_index) == 0U) {
        return false;
    }
    if (source_text == document->text) {
        context->program = (FengProgram *)ensure_document_parse(document);
    } else {
        if (!feng_parse_source(source_text,
                               strlen(source_text),
                               document->path,
                               &context->program,
                               &parse_error)) {
            return false;
        }
        context->owns_program = true;
    }
    if (context->program == NULL) {
        cache_query_context_dispose(context);
        return false;
    }
    context->provider = service->symbol_index;
    context->current_module = feng_symbol_provider_find_module(context->provider,
                                                               context->program->module_segments,
                                                               context->program->module_segment_count);
    context->source_text = source_text;
    context->source_module_index = &service->module_index;
    return true;
}

/* Gives the first cold in-memory query candidate one short frame-safe chance
 * to publish. Hot requests and current-text fast paths never enter this wait. */
static void wait_for_initial_query_state(FengLspService *service) {
    struct timespec deadline;
    int wait_status = 0;

    if (service == NULL || clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return;
    }
    deadline.tv_nsec += 16L * 1000L * 1000L;
    if (deadline.tv_nsec >= 1000L * 1000L * 1000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000L * 1000L * 1000L;
    }
    pthread_mutex_lock(&service->analysis_mutex);
    while (service->symbol_index == NULL &&
           service->last_successful_analysis.analysis == NULL &&
           service->latest_scheduled_generation > 0U &&
           wait_status != ETIMEDOUT) {
        wait_status = pthread_cond_timedwait(&service->analysis_condition,
                                             &service->analysis_mutex,
                                             &deadline);
    }
    pthread_mutex_unlock(&service->analysis_mutex);
}

/* Gives the initial source-module candidate a bounded chance to publish for
 * an import-path Completion; every subsequent lookup is memory-only. */
static void wait_for_initial_module_index(FengLspService *service) {
    struct timespec deadline;
    int wait_status = 0;

    if (service == NULL || clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return;
    }
    deadline.tv_nsec += 8L * 1000L * 1000L;
    if (deadline.tv_nsec >= 1000L * 1000L * 1000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000L * 1000L * 1000L;
    }
    pthread_mutex_lock(&service->analysis_mutex);
    while (service->module_index_generation == 0U &&
           service->latest_scheduled_generation > 0U &&
           wait_status != ETIMEDOUT) {
        wait_status = pthread_cond_timedwait(&service->analysis_condition,
                                             &service->analysis_mutex,
                                             &deadline);
    }
    pthread_mutex_unlock(&service->analysis_mutex);
}

/* Gives the initial dependency-symbol candidate a bounded publication window
 * when a source-module index could not answer an import query. */
static void wait_for_initial_symbol_index(FengLspService *service) {
    struct timespec deadline;
    int wait_status = 0;

    if (service == NULL || clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return;
    }
    deadline.tv_nsec += 16L * 1000L * 1000L;
    if (deadline.tv_nsec >= 1000L * 1000L * 1000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000L * 1000L * 1000L;
    }
    pthread_mutex_lock(&service->analysis_mutex);
    while ((service->symbol_index == NULL ||
            feng_symbol_provider_module_count(service->symbol_index) == 0U) &&
           service->latest_scheduled_generation > 0U &&
           wait_status != ETIMEDOUT) {
        wait_status = pthread_cond_timedwait(&service->analysis_condition,
                                             &service->analysis_mutex,
                                             &deadline);
    }
    pthread_mutex_unlock(&service->analysis_mutex);
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
            !string_append_json_string(json, entry->message)) {
            return false;
        }
        if (entry->related_count > 0U) {
            if (!string_append_cstr(json, ",\"relatedInformation\":[")) {
                return false;
            }
            for (size_t related_index = 0U;
                 related_index < entry->related_count;
                 ++related_index) {
                const FengLspDiagnosticRelatedEntry *related =
                    &entry->related[related_index];
                char *related_uri = path_to_file_uri(related->path);

                if (related_uri == NULL) {
                    return false;
                }
                if ((related_index > 0U && !string_append_cstr(json, ",")) ||
                    !string_append_cstr(json, "{\"location\":{\"uri\":") ||
                    !string_append_json_string(json, related_uri) ||
                    !string_append_cstr(json, ",\"range\":{\"start\":{\"line\":") ||
                    !string_append_format(
                        json, "%u", related->line > 0U ? related->line - 1U : 0U) ||
                    !string_append_cstr(json, ",\"character\":") ||
                    !string_append_format(
                        json,
                        "%u",
                        related->column > 0U ? related->column - 1U : 0U) ||
                    !string_append_cstr(json, "},\"end\":{\"line\":") ||
                    !string_append_format(
                        json, "%u", related->line > 0U ? related->line - 1U : 0U) ||
                    !string_append_cstr(json, ",\"character\":") ||
                    !string_append_format(
                        json,
                        "%u",
                        related->end_column > 0U
                            ? related->end_column - 1U
                            : 0U) ||
                    !string_append_cstr(json, "}}},\"message\":") ||
                    !string_append_json_string(json, related->message) ||
                    !string_append_cstr(json, "}")) {
                    free(related_uri);
                    return false;
                }
                free(related_uri);
            }
            if (!string_append_cstr(json, "]")) {
                return false;
            }
        }
        if (!string_append_cstr(json, "}")) {
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

static bool publish_empty_diagnostics(FILE *output, const FengLspDocument *document) {
    FengLspDiagnosticCollector collector = {0};
    bool ok = publish_diagnostics(output, &collector, document->path);

    diagnostics_dispose(&collector);
    return ok;
}

/* Publishes the current file's parse result without project I/O or semantics. */
static bool publish_current_parse_diagnostics(FILE *output,
                                              const FengLspDocument *document) {
    FengLspDiagnosticCollector collector = {0};
    FengParseError parse_error = {0};
    FengProgram *program = NULL;
    bool ok;

    if (!feng_parse_source(document->text,
                           strlen(document->text),
                           document->path,
                           &program,
                           &parse_error)) {
        (void)diagnostics_append(&collector,
                                 document->path,
                                 parse_error.token.line,
                                 parse_error.token.column,
                                 parse_error.token.length,
                                 1,
                                 "parse",
                                 parse_error.message);
    }
    ok = publish_diagnostics(output, &collector, document->path);
    feng_program_free(program);
    diagnostics_dispose(&collector);
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
    /* Infix match expressions also store the operator token while their source
     * span begins at the target expression to the left of `match`. */
    if (expr->kind == FENG_EXPR_MATCH_OP && expr->as.match_op.target != NULL) {
        return expr_start(expr->as.match_op.target);
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
        case FENG_EXPR_MATCH_OP:
            if (expr->as.match_op.target != NULL) {
                size_t target_end = expr_end(expr->as.match_op.target);

                if (target_end > end) {
                    end = target_end;
                }
            }
            for (index = 0U; index < expr->as.match_op.label_count; ++index) {
                const FengMatchLabel *label = &expr->as.match_op.labels[index];

                if (label->kind == FENG_MATCH_LABEL_VALUE && label->value != NULL) {
                    size_t value_end = expr_end(label->value);

                    if (value_end > end) {
                        end = value_end;
                    }
                } else if (label->kind == FENG_MATCH_LABEL_RANGE) {
                    size_t low_end = expr_end(label->range_low);
                    size_t high_end = expr_end(label->range_high);

                    if (low_end > end) {
                        end = low_end;
                    }
                    if (high_end > end) {
                        end = high_end;
                    }
                } else if (label->kind == FENG_MATCH_LABEL_TYPE) {
                    size_t type_index;

                    if (label->type_chain_count == 0U) {
                        size_t type_end = type_ref_end(label->type);

                        if (type_end > end) {
                            end = type_end;
                        }
                    }
                    for (type_index = 0U; type_index < label->type_chain_count; ++type_index) {
                        size_t type_end = type_ref_end(label->type_chain[type_index]);

                        if (type_end > end) {
                            end = type_end;
                        }
                    }
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
        case FENG_STMT_DEFER:
            return block_end(stmt->as.defer_block);
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
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                const FengTypeMixinDecl *mixin = &decl->as.type_decl.mixins[index];
                size_t limit = token_end_offset(mixin->token);

                if (!mixin->infer_source_type && mixin->source_type != NULL) {
                    size_t type_end = type_ref_end(mixin->source_type);

                    if (type_end > limit) {
                        limit = type_end;
                    }
                }
                if (mixin->source_constructor != NULL) {
                    size_t constructor_end = expr_end(mixin->source_constructor);

                    if (constructor_end > limit) {
                        limit = constructor_end;
                    }
                }
                if (limit > end) {
                    end = limit;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];
                size_t limit;

                if (member->mixin_origin != NULL) {
                    continue;
                }
                limit = member_end(member);
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
            } else if (decl->as.spec_decl.form == FENG_SPEC_FORM_UNION) {
                for (index = 0U; index < decl->as.spec_decl.as.union_form.member_count; ++index) {
                    size_t limit = type_ref_end(decl->as.spec_decl.as.union_form.members[index]);
                    if (limit > end) {
                        end = limit;
                    }
                }
            } else if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                for (index = 0U; index < decl->as.spec_decl.as.callable.param_count; ++index) {
                    size_t param_end = token_end_offset(decl->as.spec_decl.as.callable.params[index].token);
                    if (decl->as.spec_decl.as.callable.params[index].type != NULL) {
                        param_end = type_ref_end(decl->as.spec_decl.as.callable.params[index].type);
                    }
                    if (param_end > end) {
                        end = param_end;
                    }
                }
                if (decl->as.spec_decl.as.callable.return_type != NULL) {
                    size_t return_end = type_ref_end(decl->as.spec_decl.as.callable.return_type);
                    if (return_end > end) {
                        end = return_end;
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

/* Return whether one declaration directly owns the requested member. */
static bool declaration_contains_member(const FengDecl *decl,
                                        const FengTypeMember *member) {
    FengTypeMember *const *members = NULL;
    size_t member_count = 0U;

    if (decl == NULL || member == NULL) {
        return false;
    }
    switch (decl->kind) {
        case FENG_DECL_TYPE:
            members = decl->as.type_decl.members;
            member_count = decl->as.type_decl.member_count;
            break;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
                return false;
            }
            members = decl->as.spec_decl.as.object.members;
            member_count = decl->as.spec_decl.as.object.member_count;
            break;
        case FENG_DECL_FIT:
            members = decl->as.fit_decl.members;
            member_count = decl->as.fit_decl.member_count;
            break;
        default:
            return false;
    }
    for (size_t index = 0U; index < member_count; ++index) {
        if (members[index] == member) {
            return true;
        }
    }
    return false;
}

/* Find the program and declaration that own a semantic member pointer. */
static const FengSemanticModule *find_member_module(
    const FengLspAnalysisSession *session,
    const FengTypeMember *member,
    const FengDecl **out_decl,
    const FengProgram **out_program) {
    *out_decl = NULL;
    *out_program = NULL;
    if (member == NULL || session->analysis == NULL) {
        return NULL;
    }
    for (size_t module_index = 0U;
         module_index < session->analysis->module_count;
         ++module_index) {
        const FengSemanticModule *module =
            &session->analysis->modules[module_index];

        for (size_t program_index = 0U;
             program_index < module->program_count;
             ++program_index) {
            const FengProgram *program = module->programs[program_index];

            for (size_t decl_index = 0U;
                 decl_index < program->declaration_count;
                 ++decl_index) {
                const FengDecl *decl = program->declarations[decl_index];

                if (declaration_contains_member(decl, member)) {
                    *out_decl = decl;
                    *out_program = program;
                    return module;
                }
            }
        }
    }
    return NULL;
}

/* Follow generated mixin members back to the original source declaration. */
static const FengTypeMember *mixin_definition_source_member(
    const FengTypeMember *member) {
    while (member != NULL && member->mixin_source_member != NULL &&
           member->mixin_source_member != member) {
        member = member->mixin_source_member;
    }
    return member;
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
        .match_op = NULL,
        .self_owner_decl = self_owner_decl
    };

    return append_raw((void **)&locals->items,
                      &locals->count,
                      &locals->capacity,
                      sizeof(local),
                      &local);
}

/* Append a local introduced by an infix match expression. It has no
 * FengBinding AST node, so the owning match-op expression carries the binding
 * name, mutability, and narrowed type labels used by Hover. */
static bool local_list_push_match_binding(FengLspLocalList *locals,
                                          const FengExpr *match_op) {
    FengLspLocal local;

    if (match_op == NULL || match_op->kind != FENG_EXPR_MATCH_OP ||
        !match_op->as.match_op.has_binding) {
        return true;
    }
    memset(&local, 0, sizeof(local));
    local.kind = FENG_LSP_LOCAL_BINDING;
    local.name = match_op->as.match_op.binding_name;
    local.match_op = match_op;
    return append_raw((void **)&locals->items,
                      &locals->count,
                      &locals->capacity,
                      sizeof(local),
                      &local);
}

/* Collect infix-match bindings whose truth is guaranteed by an expression.
 * This mirrors docs/specifications/feng-flow.md: && combines bindings, while other wrappers
 * do not propagate them into a sibling expression or condition body. */
static bool collect_visible_match_op_bindings(const FengExpr *expr,
                                              FengLspLocalList *locals) {
    if (expr == NULL) {
        return true;
    }
    if (expr->kind == FENG_EXPR_BINARY && expr->as.binary.op == FENG_TOKEN_AND_AND) {
        return collect_visible_match_op_bindings(expr->as.binary.left, locals) &&
               collect_visible_match_op_bindings(expr->as.binary.right, locals);
    }
    if (expr->kind == FENG_EXPR_MATCH_OP) {
        return local_list_push_match_binding(locals, expr);
    }
    return true;
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
static bool collect_expr_locals(const FengExpr *expr,
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

/* Collect lexical locals introduced by lambdas along the expression path that
 * contains `offset`. Nested lambdas append their parameters after outer locals,
 * so find_local() naturally preserves lexical shadowing. */
static bool collect_expr_locals(const FengExpr *expr,
                                size_t offset,
                                FengLspLocalList *locals) {
    size_t index;

    if (expr == NULL || offset < expr_start(expr) || offset > expr_end(expr)) {
        return true;
    }
    switch (expr->kind) {
        case FENG_EXPR_LAMBDA:
            if (expr->as.lambda.is_block_body) {
                if (expr->as.lambda.body_block == NULL ||
                    offset < expr->as.lambda.body_block->token.offset ||
                    offset > block_end(expr->as.lambda.body_block)) {
                    return true;
                }
            } else if (expr->as.lambda.body == NULL ||
                       offset < expr_start(expr->as.lambda.body) ||
                       offset > expr_end(expr->as.lambda.body)) {
                return true;
            }
            for (index = 0U; index < expr->as.lambda.param_count; ++index) {
                if (!local_list_push(locals,
                                     FENG_LSP_LOCAL_PARAM,
                                     expr->as.lambda.params[index].name,
                                     &expr->as.lambda.params[index],
                                     NULL,
                                     NULL)) {
                    return false;
                }
            }
            return expr->as.lambda.is_block_body
                       ? collect_block_locals(expr->as.lambda.body_block, offset, locals)
                       : collect_expr_locals(expr->as.lambda.body, offset, locals);
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (offset >= expr_start(expr->as.array_literal.items[index]) &&
                    offset <= expr_end(expr->as.array_literal.items[index])) {
                    return collect_expr_locals(expr->as.array_literal.items[index], offset, locals);
                }
            }
            return true;
        case FENG_EXPR_TUPLE_LITERAL:
            for (index = 0U; index < expr->as.tuple_literal.count; ++index) {
                if (offset >= expr_start(expr->as.tuple_literal.items[index]) &&
                    offset <= expr_end(expr->as.tuple_literal.items[index])) {
                    return collect_expr_locals(expr->as.tuple_literal.items[index], offset, locals);
                }
            }
            return true;
        case FENG_EXPR_GENERIC_TARGET:
            return collect_expr_locals(expr->as.generic_target.target, offset, locals);
        case FENG_EXPR_ARRAY_NEW:
            return collect_expr_locals(expr->as.array_new.size, offset, locals);
        case FENG_EXPR_OBJECT_LITERAL:
            if (collect_expr_locals(expr->as.object_literal.target, offset, locals)) {
                for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                    const FengExpr *value = expr->as.object_literal.fields[index].value;

                    if (value != NULL && offset >= expr_start(value) && offset <= expr_end(value)) {
                        return collect_expr_locals(value, offset, locals);
                    }
                }
                return true;
            }
            return false;
        case FENG_EXPR_CALL:
            if (expr->as.call.callee != NULL &&
                offset >= expr_start(expr->as.call.callee) &&
                offset <= expr_end(expr->as.call.callee)) {
                return collect_expr_locals(expr->as.call.callee, offset, locals);
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                const FengExpr *arg = expr->as.call.args[index];

                if (arg != NULL && offset >= expr_start(arg) && offset <= expr_end(arg)) {
                    return collect_expr_locals(arg, offset, locals);
                }
            }
            return true;
        case FENG_EXPR_MEMBER:
            return collect_expr_locals(expr->as.member.object, offset, locals);
        case FENG_EXPR_INDEX:
            return offset <= expr_end(expr->as.index.object)
                       ? collect_expr_locals(expr->as.index.object, offset, locals)
                       : collect_expr_locals(expr->as.index.index, offset, locals);
        case FENG_EXPR_UNARY:
            return collect_expr_locals(expr->as.unary.operand, offset, locals);
        case FENG_EXPR_BINARY:
            if (offset <= expr_end(expr->as.binary.left)) {
                return collect_expr_locals(expr->as.binary.left, offset, locals);
            }
            if (expr->as.binary.op == FENG_TOKEN_AND_AND &&
                !collect_visible_match_op_bindings(expr->as.binary.left, locals)) {
                return false;
            }
            return collect_expr_locals(expr->as.binary.right, offset, locals);
        case FENG_EXPR_CAST:
            return collect_expr_locals(expr->as.cast.value, offset, locals);
        case FENG_EXPR_IF:
            if (expr->as.if_expr.condition != NULL &&
                offset <= expr_end(expr->as.if_expr.condition)) {
                return collect_expr_locals(expr->as.if_expr.condition, offset, locals);
            }
            if (expr->as.if_expr.then_block != NULL &&
                offset <= block_end(expr->as.if_expr.then_block)) {
                if (!collect_visible_match_op_bindings(expr->as.if_expr.condition, locals)) {
                    return false;
                }
                return collect_block_locals(expr->as.if_expr.then_block, offset, locals);
            }
            return collect_block_locals(expr->as.if_expr.else_block, offset, locals);
        case FENG_EXPR_MATCH:
            if (expr->as.match_expr.target != NULL &&
                offset <= expr_end(expr->as.match_expr.target)) {
                return collect_expr_locals(expr->as.match_expr.target, offset, locals);
            }
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                const FengBlock *body = expr->as.match_expr.branches[index].body;

                if (body != NULL && offset <= block_end(body)) {
                    return collect_block_locals(body, offset, locals);
                }
            }
            return collect_block_locals(expr->as.match_expr.else_block, offset, locals);
        case FENG_EXPR_MATCH_OP:
            return collect_expr_locals(expr->as.match_op.target, offset, locals);
        case FENG_EXPR_TRY:
            if (expr->as.try_expr.body != NULL && offset <= expr_end(expr->as.try_expr.body)) {
                return collect_expr_locals(expr->as.try_expr.body, offset, locals);
            }
            for (index = 0U; index < expr->as.try_expr.clause_count; ++index) {
                const FengBlock *body = expr->as.try_expr.clauses[index].body;

                if (body != NULL && offset <= block_end(body)) {
                    return collect_block_locals(body, offset, locals);
                }
            }
            return true;
        default:
            return true;
    }
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
        case FENG_STMT_BINDING:
            return collect_expr_locals(stmt->as.binding.initializer, offset, locals);
        case FENG_STMT_ASSIGN:
            if (stmt->as.assign.target != NULL &&
                offset <= expr_end(stmt->as.assign.target)) {
                return collect_expr_locals(stmt->as.assign.target, offset, locals);
            }
            return collect_expr_locals(stmt->as.assign.value, offset, locals);
        case FENG_STMT_EXPR:
        case FENG_STMT_TRY:
            return collect_expr_locals(stmt->as.expr, offset, locals);
        case FENG_STMT_RETURN:
            return collect_expr_locals(stmt->as.return_value, offset, locals);
        case FENG_STMT_THROW:
            return collect_expr_locals(stmt->as.throw_value, offset, locals);
        case FENG_STMT_BLOCK:
            return collect_block_locals(stmt->as.block, offset, locals);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (offset <= expr_end(stmt->as.if_stmt.clauses[index].condition)) {
                    return collect_expr_locals(stmt->as.if_stmt.clauses[index].condition,
                                               offset,
                                               locals);
                }
                if (offset <= block_end(stmt->as.if_stmt.clauses[index].block)) {
                    if (!collect_visible_match_op_bindings(stmt->as.if_stmt.clauses[index].condition,
                                                           locals)) {
                        return false;
                    }
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
                    if (stmt->as.match_stmt.branches[index].has_binding) {
                        if (!local_list_push(locals,
                                             FENG_LSP_LOCAL_BINDING,
                                             stmt->as.match_stmt.branches[index].binding_name,
                                             NULL,
                                             NULL,
                                             NULL)) {
                            return false;
                        }
                    }
                    return collect_block_locals(stmt->as.match_stmt.branches[index].body, offset, locals);
                }
            }
            return stmt->as.match_stmt.else_block != NULL
                       ? collect_block_locals(stmt->as.match_stmt.else_block, offset, locals)
                       : true;
        case FENG_STMT_WHILE:
            if (stmt->as.while_stmt.condition != NULL && offset <= expr_end(stmt->as.while_stmt.condition)) {
                return collect_expr_locals(stmt->as.while_stmt.condition, offset, locals);
            }
            if (!collect_visible_match_op_bindings(stmt->as.while_stmt.condition, locals)) {
                return false;
            }
            return stmt->as.while_stmt.body != NULL
                       ? collect_block_locals(stmt->as.while_stmt.body, offset, locals)
                       : true;
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                if (stmt->as.for_stmt.iter_expr != NULL && offset <= expr_end(stmt->as.for_stmt.iter_expr)) {
                    return collect_expr_locals(stmt->as.for_stmt.iter_expr, offset, locals);
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
                return collect_expr_locals(stmt->as.for_stmt.condition, offset, locals);
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
                const FengTypeMember *member = decl->as.type_decl.members[member_index];

                if (member->mixin_origin == NULL &&
                    offset >= member->token.offset &&
                    offset <= member_end(member)) {
                    *out_member = member;
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
                    if (stmt->as.match_stmt.branches[index].has_binding) {
                        if (!local_list_push(locals,
                                             FENG_LSP_LOCAL_BINDING,
                                             stmt->as.match_stmt.branches[index].binding_name,
                                             NULL,
                                             NULL,
                                             NULL)) {
                            return false;
                        }
                    }
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

/* Return the generic arity that participates in a type/spec declaration's
 * identity. Non-generic type-like declarations have arity zero. */
static size_t ast_decl_type_param_count(const FengDecl *decl) {
    if (decl == NULL) {
        return 0U;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        return decl->as.type_decl.type_param_count;
    }
    if (decl->kind == FENG_DECL_SPEC) {
        return decl->as.spec_decl.type_param_count;
    }
    return 0U;
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

/* Find an AST type/spec/enum declaration by its exact (name, arity) identity. */
static const FengDecl *find_module_type_decl_by_name_and_arity(
    const FengSemanticModule *module,
    FengSlice name,
    size_t type_param_count,
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
            bool is_type = decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
                           decl->kind == FENG_DECL_SPEC;

            if (!is_type || (public_only && decl->visibility != FENG_VISIBILITY_PUBLIC)) {
                continue;
            }
            if (slice_equals(decl_name(decl), name) &&
                ast_decl_type_param_count(decl) == type_param_count) {
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

/* Find an exact-arity type declaration in one parsed source program. */
static const FengDecl *find_program_type_decl_by_name_and_arity(
    const FengProgram *program,
    FengSlice name,
    size_t type_param_count,
    bool public_only) {
    size_t decl_index;

    if (program == NULL) {
        return NULL;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        bool is_type = decl->kind == FENG_DECL_TYPE || decl->kind == FENG_DECL_ENUM ||
                       decl->kind == FENG_DECL_SPEC;

        if (!is_type || (public_only && decl->visibility != FENG_VISIBILITY_PUBLIC)) {
            continue;
        }
        if (slice_equals(decl_name(decl), name) &&
            ast_decl_type_param_count(decl) == type_param_count) {
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
    if (session->source_module_index != NULL) {
        size_t module_index;

        for (module_index = 0U;
             module_index < session->source_module_index->module_count;
             ++module_index) {
            const FengProgram *program =
                session->source_module_index->modules[module_index].program;

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
    }
    return NULL;
}

/* Find an exact-arity type declaration across parsed sources of one module. */
static const FengDecl *find_loaded_module_type_decl_by_name_and_arity(
    const FengLspAnalysisSession *session,
    const FengSlice *segments,
    size_t segment_count,
    FengSlice name,
    size_t type_param_count,
    bool public_only) {
    size_t source_index;

    if (session == NULL || segments == NULL || segment_count == 0U) {
        return NULL;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        const FengProgram *program = session->sources[source_index].program;

        if (program_module_matches(program, segments, segment_count)) {
            const FengDecl *decl = find_program_type_decl_by_name_and_arity(
                program, name, type_param_count, public_only);

            if (decl != NULL) {
                return decl;
            }
        }
    }
    if (session->source_module_index != NULL) {
        size_t module_index;

        for (module_index = 0U;
             module_index < session->source_module_index->module_count;
             ++module_index) {
            const FengProgram *program =
                session->source_module_index->modules[module_index].program;

            if (program_module_matches(program, segments, segment_count)) {
                const FengDecl *decl = find_program_type_decl_by_name_and_arity(
                    program,
                    name,
                    type_param_count,
                    public_only);

                if (decl != NULL) {
                    return decl;
                }
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

/* Find a type/spec decl by name AND type parameter count (arity).
 * Only matches type/spec kinds; falls through for enums and values.
 * Returns NULL if no match or module is NULL. */
static const FengSymbolDeclView *find_symbol_module_decl_by_name_and_arity(
    const FengSymbolImportedModule *module,
    FengSlice name,
    size_t type_param_count,
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
        if (!symbol_decl_is_type(decl)) {
            continue;
        }
        if (slice_equals(feng_symbol_decl_name(decl), name) &&
            feng_symbol_decl_type_param_count(decl) == type_param_count) {
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
    size_t arity;

    if (provider == NULL || program == NULL || type_ref == NULL ||
        type_ref->kind != FENG_TYPE_REF_NAMED || type_ref->as.named.segment_count == 0U) {
        return NULL;
    }
    name = type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];
    arity = type_ref->as.named.type_arg_count;
    if (type_ref->as.named.segment_count == 1U) {
        if (feng_semantic_is_builtin_type_name(name)) {
            return NULL;
        }
        if (current_module != NULL) {
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                current_module, name, arity, false);
            if (decl != NULL) {
                return decl;
            }
            decl = find_symbol_module_decl_by_name(current_module,
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
                const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                    module, name, arity, true);
                if (decl != NULL) {
                    return decl;
                }
                decl = find_symbol_module_decl_by_name(module,
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
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                alias_module, type_ref->as.named.segments[1], arity, true);
            if (decl != NULL) {
                return decl;
            }
            return find_symbol_module_decl_by_name(alias_module,
                                                   type_ref->as.named.segments[1],
                                                   false,
                                                   true,
                                                   true);
        }
    }
    {
        const FengSymbolImportedModule *target_module = feng_symbol_provider_find_module(
            provider, type_ref->as.named.segments, type_ref->as.named.segment_count - 1U);
        const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
            target_module, name, arity, true);
        if (decl != NULL) {
            return decl;
        }
        return find_symbol_module_decl_by_name(target_module,
                                               name,
                                               false,
                                               true,
                                               true);
    }
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
    if (kind == FENG_SYMBOL_TYPE_KIND_NAMED || kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC) {
        size_t segment_count = feng_symbol_type_segment_count(type);
        size_t arity = (kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC)
                           ? feng_symbol_type_generic_arg_count(type)
                           : 0U;
        FengSlice name;
        size_t index;

        if (segment_count == 0U) {
            return NULL;
        }
        name = feng_symbol_type_segment_at(type, segment_count - 1U);
        if (segment_count == 1U) {
            if (current_module != NULL) {
                const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                    current_module, name, arity, false);
                if (decl != NULL) {
                    return decl;
                }
                decl = find_symbol_module_decl_by_name(current_module,
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
                    const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                        module, name, arity, true);
                    if (decl != NULL) {
                        return decl;
                    }
                    decl = find_symbol_module_decl_by_name(module,
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
                const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                    alias_module, feng_symbol_type_segment_at(type, 1U), arity, true);
                if (decl != NULL) {
                    return decl;
                }
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
            decl = find_symbol_module_decl_by_name_and_arity(module, name, arity, true);
            if (decl != NULL) {
                return decl;
            }
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

/* Resolve a symbol-backed type name by exact arity first, retaining name-only
 * fallback for incomplete documents whose precise declaration is unavailable. */
static const FengSymbolDeclView *resolve_symbol_type_name_with_arity(
    const FengSymbolProvider *provider,
    const FengSymbolImportedModule *current_module,
    const FengProgram *program,
    FengSlice name,
    size_t type_param_count) {
    size_t index;

    if (current_module != NULL) {
        const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
            current_module, name, type_param_count, false);

        if (decl != NULL) {
            return decl;
        }
        decl = find_symbol_module_decl_by_name(current_module,
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
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                module, name, type_param_count, true);

            if (decl != NULL) {
                return decl;
            }
            decl = find_symbol_module_decl_by_name(module,
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

/* Resolve a bare symbol-backed type name, whose use-site arity is zero. */
static const FengSymbolDeclView *resolve_symbol_type_name(const FengSymbolProvider *provider,
                                                          const FengSymbolImportedModule *current_module,
                                                          const FengProgram *program,
                                                          FengSlice name) {
    return resolve_symbol_type_name_with_arity(provider, current_module, program, name, 0U);
}

/* Resolve a symbol-backed constructor target using the arity supplied by the
 * enclosing call or explicit generic target. */
static const FengSymbolDeclView *resolve_symbol_type_constructor_expr_with_arity(
    const FengLspCacheQueryContext *context,
    const FengExpr *expr,
    size_t type_param_count) {
    if (context == NULL || expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        return resolve_symbol_type_name_with_arity(context->provider,
                                                   context->current_module,
                                                   context->program,
                                                   expr->as.identifier,
                                                   type_param_count);
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL &&
        expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
        const FengSymbolImportedModule *alias_module = find_symbol_alias_module(context->provider,
                                                                                context->program,
                                                                                expr->as.member.object->as.identifier);
        if (alias_module != NULL) {
            const FengSymbolDeclView *decl = find_symbol_module_decl_by_name_and_arity(
                alias_module, expr->as.member.member, type_param_count, true);

            if (decl != NULL) {
                return decl;
            }
            return find_symbol_module_decl_by_name(alias_module,
                                                   expr->as.member.member,
                                                   false,
                                                   true,
                                                   true);
        }
    }
    return NULL;
}

/* Derive constructor target arity from the parsed expression shape. */
static const FengSymbolDeclView *resolve_symbol_type_constructor_expr(
    const FengLspCacheQueryContext *context,
    const FengExpr *expr) {
    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_CALL) {
        size_t type_param_count = expr->as.call.has_explicit_type_args
                                      ? expr->as.call.explicit_type_arg_count
                                      : 0U;

        return resolve_symbol_type_constructor_expr_with_arity(
            context, expr->as.call.callee, type_param_count);
    }
    if (expr->kind == FENG_EXPR_GENERIC_TARGET) {
        return resolve_symbol_type_constructor_expr_with_arity(
            context,
            expr->as.generic_target.target,
            expr->as.generic_target.type_arg_count);
    }
    return resolve_symbol_type_constructor_expr_with_arity(context, expr, 0U);
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
        return resolve_symbol_type_constructor_expr(context, expr);
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
        if (feng_semantic_is_builtin_type_name(name)) {
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

/* Find an enum item by its declared name without requiring semantic analysis. */
static const FengEnumItem *find_enum_item_by_name(const FengDecl *owner_decl,
                                                  FengSlice name) {
    size_t index;

    if (owner_decl == NULL || owner_decl->kind != FENG_DECL_ENUM) {
        return NULL;
    }
    for (index = 0U; index < owner_decl->as.enum_decl.item_count; ++index) {
        const FengEnumItem *item = &owner_decl->as.enum_decl.items[index];

        if (slice_equals(item->name, name)) {
            return item;
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

/* Resolve an AST-backed type name by exact arity first, retaining name-only
 * fallback to keep edit-time behavior for incomplete sources. */
static const FengDecl *resolve_type_name_with_arity(const FengLspAnalysisSession *session,
                                                    const FengProgram *program,
                                                    FengSlice name,
                                                    size_t type_param_count) {
    const FengSemanticModule *program_module = find_program_module(session, program);
    size_t index;

    if (program_module != NULL) {
        const FengDecl *decl = find_module_type_decl_by_name_and_arity(
            program_module, name, type_param_count, false);

        if (decl != NULL) {
            return decl;
        }
        decl = find_module_decl_by_name(program_module, name, false, true, false);
        if (decl != NULL) {
            return decl;
        }
    }
    {
        const FengDecl *decl = find_loaded_module_type_decl_by_name_and_arity(
            session,
            program->module_segments,
            program->module_segment_count,
            name,
            type_param_count,
            false);

        if (decl != NULL) {
            return decl;
        }
        decl = find_loaded_module_decl_by_name(session,
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
            const FengDecl *decl = find_module_type_decl_by_name_and_arity(
                module, name, type_param_count, true);

            if (decl != NULL) {
                return decl;
            }
            decl = find_module_decl_by_name(module, name, false, true, true);
            if (decl != NULL) {
                return decl;
            }
        }
        {
            const FengDecl *decl = find_loaded_module_type_decl_by_name_and_arity(
                session,
                use_decl->segments,
                use_decl->segment_count,
                name,
                type_param_count,
                true);

            if (decl != NULL) {
                return decl;
            }
            decl = find_loaded_module_decl_by_name(session,
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

/* Resolve a bare AST-backed type name, whose use-site arity is zero. */
static const FengDecl *resolve_type_name(const FengLspAnalysisSession *session,
                                         const FengProgram *program,
                                         FengSlice name) {
    return resolve_type_name_with_arity(session, program, name, 0U);
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
                                          const char *owner_name,
                                          const FengLspRequestContext *request);

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

/* Returns the canonical builtin name if `name` is a builtin type identifier. */
static const char *builtin_name_for_identifier(FengSlice name) {
    if (slice_equals_cstr(name, "string")) { return "string"; }
    if (slice_equals_cstr(name, "int") || slice_equals_cstr(name, "i32")) { return "i32"; }
    if (slice_equals_cstr(name, "long") || slice_equals_cstr(name, "i64")) { return "i64"; }
    if (slice_equals_cstr(name, "byte") || slice_equals_cstr(name, "u8")) { return "u8"; }
    if (slice_equals_cstr(name, "float") || slice_equals_cstr(name, "f32")) { return "f32"; }
    if (slice_equals_cstr(name, "double") || slice_equals_cstr(name, "f64")) { return "f64"; }
    if (slice_equals_cstr(name, "bool")) { return "bool"; }
    if (slice_equals_cstr(name, "i8")) { return "i8"; }
    if (slice_equals_cstr(name, "i16")) { return "i16"; }
    if (slice_equals_cstr(name, "u16")) { return "u16"; }
    if (slice_equals_cstr(name, "u32")) { return "u32"; }
    if (slice_equals_cstr(name, "u64")) { return "u64"; }
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
                                                     FengLspMemberFilter filter,
                                                     const FengLspRequestContext *request) {
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
                    if (!append_member_completion_item(json, first, member, NULL, request)) {
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
                    if (!append_member_completion_item(json, first, member, NULL, request)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/* Resolve an AST-backed constructor target using its call-site arity. */
static const FengDecl *resolve_type_constructor_expr_with_arity(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengExpr *expr,
    size_t type_param_count) {
    size_t index;

    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_IDENTIFIER) {
        return resolve_type_name_with_arity(session,
                                            program,
                                            expr->as.identifier,
                                            type_param_count);
    }
    if (expr->kind == FENG_EXPR_MEMBER && expr->as.member.object != NULL &&
        expr->as.member.object->kind == FENG_EXPR_IDENTIFIER) {
        const FengSemanticModule *alias_module = find_alias_module(session,
                                                                   program,
                                                                   expr->as.member.object->as.identifier);
        if (alias_module != NULL) {
            const FengDecl *decl = find_module_type_decl_by_name_and_arity(
                alias_module, expr->as.member.member, type_param_count, true);

            if (decl != NULL) {
                return decl;
            }
            decl = find_module_decl_by_name(alias_module,
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
                const FengDecl *decl = find_loaded_module_type_decl_by_name_and_arity(
                    session,
                    use_decl->segments,
                    use_decl->segment_count,
                    expr->as.member.member,
                    type_param_count,
                    true);

                if (decl != NULL) {
                    return decl;
                }
                decl = find_loaded_module_decl_by_name(session,
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

/* Derive an AST-backed constructor target's arity from its expression. */
static const FengDecl *resolve_type_constructor_expr(const FengLspAnalysisSession *session,
                                                     const FengProgram *program,
                                                     const FengExpr *expr) {
    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_CALL) {
        size_t type_param_count = expr->as.call.has_explicit_type_args
                                      ? expr->as.call.explicit_type_arg_count
                                      : 0U;

        return resolve_type_constructor_expr_with_arity(
            session, program, expr->as.call.callee, type_param_count);
    }
    if (expr->kind == FENG_EXPR_GENERIC_TARGET) {
        return resolve_type_constructor_expr_with_arity(
            session,
            program,
            expr->as.generic_target.target,
            expr->as.generic_target.type_arg_count);
    }
    return resolve_type_constructor_expr_with_arity(session, program, expr, 0U);
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
        return resolve_type_constructor_expr(session, program, expr);
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
    if (object->kind == FENG_EXPR_MEMBER && object->as.member.object != NULL) {
        const FengDecl *owner_decl = resolve_owner_decl_from_object_expr(session,
                                                                         program,
                                                                         object->as.member.object,
                                                                         locals);
        const FengTypeMember *member = find_member_by_name(owner_decl, object->as.member.member);

        if (member == NULL) {
            return NULL;
        }
        if (member->kind == FENG_TYPE_MEMBER_FIELD) {
            return resolve_named_type_ref(session, program, member->as.field.type);
        }
        return resolve_named_type_ref(session, program, member->as.callable.return_type);
    }
    if (object->kind == FENG_EXPR_CALL && object->as.call.callee != NULL) {
        const FengExpr *callee = object->as.call.callee;

        if (callee->kind == FENG_EXPR_IDENTIFIER) {
            decl = resolve_type_constructor_expr(session, program, callee);
            if (decl != NULL) {
                return decl;
            }
            decl = resolve_value_name(session, program, callee->as.identifier);
            if (decl != NULL && decl->kind == FENG_DECL_FUNCTION) {
                return resolve_named_type_ref(session,
                                              program,
                                              decl->as.function_decl.return_type);
            }
            return NULL;
        }
        if (callee->kind == FENG_EXPR_MEMBER && callee->as.member.object != NULL) {
            const FengDecl *owner_decl = resolve_owner_decl_from_object_expr(
                session, program, callee->as.member.object, locals);
            const FengTypeMember *member = find_member_by_name(owner_decl,
                                                               callee->as.member.member);

            if (member != NULL && member->kind != FENG_TYPE_MEMBER_FIELD) {
                return resolve_named_type_ref(session,
                                              program,
                                              member->as.callable.return_type);
            }
        }
        return NULL;
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

typedef enum FengLspReceiverPendingCall {
    FENG_LSP_RECEIVER_PENDING_NONE = 0,
    FENG_LSP_RECEIVER_PENDING_CONSTRUCTOR,
    FENG_LSP_RECEIVER_PENDING_FUNCTION,
    FENG_LSP_RECEIVER_PENDING_MEMBER
} FengLspReceiverPendingCall;

/* AST-backed type state while executing a text-derived receiver chain. */
typedef struct FengLspAstReceiverState {
    const FengDecl *owner_decl;
    const FengTypeRef *type_ref;
    FengSlice builtin_name;
    FengLspReceiverPendingCall pending_call;
    const FengDecl *pending_function;
    const FengTypeMember *pending_member;
} FengLspAstReceiverState;

/* Replaces an AST receiver state with one declared type reference. */
static void ast_receiver_state_set_type(const FengLspAnalysisSession *session,
                                        const FengProgram *program,
                                        const FengTypeRef *type_ref,
                                        FengLspAstReceiverState *state) {
    const char *builtin;

    memset(state, 0, sizeof(*state));
    state->type_ref = type_ref;
    builtin = builtin_name_for_single_segment_type_ref(type_ref);
    if (builtin != NULL) {
        state->builtin_name = slice_from_cstr(builtin);
    } else if (type_ref != NULL && type_ref->kind == FENG_TYPE_REF_NAMED) {
        state->owner_decl = resolve_named_type_ref(session, program, type_ref);
    }
}

/* Resolves the root value of an AST-backed receiver chain. */
static bool ast_receiver_state_from_root(const FengLspAnalysisSession *session,
                                         const FengProgram *program,
                                         const FengLspReceiverChain *chain,
                                         const FengLspLocalList *locals,
                                         FengLspAstReceiverState *state) {
    const FengLspLocal *local;
    const FengDecl *decl;

    memset(state, 0, sizeof(*state));
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_STRING) {
        state->builtin_name = slice_from_cstr("string");
        return true;
    }
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_INTEGER) {
        state->builtin_name = slice_from_cstr("i32");
        return true;
    }
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_FLOAT) {
        state->builtin_name = slice_from_cstr("f64");
        return true;
    }
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_BOOL) {
        state->builtin_name = slice_from_cstr("bool");
        return true;
    }
    local = find_local(locals, chain->root);
    if (local != NULL) {
        if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
            ast_receiver_state_set_type(session, program, local->parameter->type, state);
            return state->owner_decl != NULL || state->type_ref != NULL ||
                   state->builtin_name.length > 0U;
        }
        if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
            const FengSemanticTypeFact *fact =
                session->analysis != NULL
                    ? feng_semantic_lookup_type_fact(session->analysis, local->binding)
                    : NULL;

            if (local->binding->type != NULL) {
                ast_receiver_state_set_type(session, program, local->binding->type, state);
            } else if (fact != NULL && fact->kind == FENG_SEMANTIC_TYPE_FACT_TYPE_REF) {
                ast_receiver_state_set_type(session, program, fact->type_ref, state);
            } else if (fact != NULL && fact->kind == FENG_SEMANTIC_TYPE_FACT_DECL) {
                state->owner_decl = fact->type_decl;
            } else if (fact != NULL && fact->kind == FENG_SEMANTIC_TYPE_FACT_BUILTIN) {
                state->builtin_name = fact->builtin_name;
            } else {
                state->owner_decl = owner_decl_from_binding(session,
                                                            program,
                                                            local->binding);
            }
            return state->owner_decl != NULL || state->type_ref != NULL ||
                   state->builtin_name.length > 0U;
        }
        if (local->kind == FENG_LSP_LOCAL_SELF) {
            FengExpr self_expr = {0};

            self_expr.kind = FENG_EXPR_SELF;
            state->owner_decl = resolve_owner_decl_from_object_expr(session,
                                                                    program,
                                                                    &self_expr,
                                                                    locals);
            return state->owner_decl != NULL;
        }
    }
    decl = resolve_value_name(session, program, chain->root);
    if (decl != NULL && decl->kind == FENG_DECL_GLOBAL_BINDING) {
        if (decl->as.binding.type != NULL) {
            ast_receiver_state_set_type(session, program, decl->as.binding.type, state);
        } else {
            state->owner_decl = owner_decl_from_binding(session,
                                                        program,
                                                        &decl->as.binding);
        }
        return state->owner_decl != NULL || state->type_ref != NULL ||
               state->builtin_name.length > 0U;
    }
    if (decl != NULL && decl->kind == FENG_DECL_FUNCTION) {
        state->pending_call = FENG_LSP_RECEIVER_PENDING_FUNCTION;
        state->pending_function = decl;
        return true;
    }
    state->owner_decl = resolve_type_name(session, program, chain->root);
    if (state->owner_decl != NULL) {
        state->pending_call = FENG_LSP_RECEIVER_PENDING_CONSTRUCTOR;
        return true;
    }
    return false;
}

/* Resolves a mixed member/call/index receiver without requiring the dirty
 * parser to preserve its final member expression. */
static const FengDecl *resolve_owner_decl_from_receiver_text(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    FengSlice receiver,
    const FengLspLocalList *locals,
    FengSlice *out_builtin_name) {
    FengLspReceiverChain chain = {0};
    FengLspAstReceiverState state = {0};
    size_t index;
    bool valid;

    if (out_builtin_name != NULL) {
        *out_builtin_name = (FengSlice){0};
    }
    if (!receiver_chain_parse(receiver, &chain) ||
        !ast_receiver_state_from_root(session, program, &chain, locals, &state)) {
        receiver_chain_dispose(&chain);
        return NULL;
    }
    valid = true;
    for (index = 0U; valid && index < chain.operation_count; ++index) {
        const FengLspReceiverOperation *operation = &chain.operations[index];

        if (operation->kind == FENG_LSP_RECEIVER_MEMBER) {
            const FengTypeMember *member;

            if (state.pending_call != FENG_LSP_RECEIVER_PENDING_NONE ||
                state.owner_decl == NULL) {
                valid = false;
                break;
            }
            member = find_member_by_name(state.owner_decl, operation->member);
            if (member == NULL) {
                valid = false;
            } else if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                ast_receiver_state_set_type(session, program, member->as.field.type, &state);
            } else {
                memset(&state, 0, sizeof(state));
                state.pending_call = FENG_LSP_RECEIVER_PENDING_MEMBER;
                state.pending_member = member;
            }
        } else if (operation->kind == FENG_LSP_RECEIVER_CALL) {
            if (state.pending_call == FENG_LSP_RECEIVER_PENDING_CONSTRUCTOR) {
                state.pending_call = FENG_LSP_RECEIVER_PENDING_NONE;
            } else if (state.pending_call == FENG_LSP_RECEIVER_PENDING_FUNCTION &&
                       state.pending_function != NULL) {
                const FengTypeRef *return_type =
                    state.pending_function->as.function_decl.return_type;

                ast_receiver_state_set_type(session, program, return_type, &state);
            } else if (state.pending_call == FENG_LSP_RECEIVER_PENDING_MEMBER &&
                       state.pending_member != NULL) {
                const FengTypeRef *return_type = state.pending_member->as.callable.return_type;

                ast_receiver_state_set_type(session, program, return_type, &state);
            } else {
                valid = false;
            }
        } else {
            if (state.pending_call != FENG_LSP_RECEIVER_PENDING_NONE ||
                state.type_ref == NULL || state.type_ref->kind != FENG_TYPE_REF_ARRAY) {
                valid = false;
            } else {
                ast_receiver_state_set_type(session, program, state.type_ref->as.inner, &state);
            }
        }
    }
    receiver_chain_dispose(&chain);
    if (!valid || state.pending_call != FENG_LSP_RECEIVER_PENDING_NONE) {
        return NULL;
    }
    if (out_builtin_name != NULL) {
        *out_builtin_name = state.builtin_name;
    }
    return state.owner_decl;
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

/* Symbol-backed type state while executing a text-derived receiver chain. */
typedef struct FengLspSymbolReceiverState {
    const FengSymbolDeclView *owner_decl;
    const FengSymbolTypeView *symbol_type;
    const FengTypeRef *ast_type_ref;
    FengSlice builtin_name;
    FengLspReceiverPendingCall pending_call;
    const FengSymbolDeclView *pending_callable;
} FengLspSymbolReceiverState;

/* Replaces a symbol receiver state with a provider-owned type view. */
static void symbol_receiver_state_set_symbol_type(
    const FengLspCacheQueryContext *context,
    const FengSymbolTypeView *type,
    FengLspSymbolReceiverState *state) {
    FengSymbolTypeKind kind;

    memset(state, 0, sizeof(*state));
    state->symbol_type = type;
    kind = feng_symbol_type_kind(type);
    if (kind == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
        state->builtin_name = feng_symbol_type_builtin_name(type);
    } else if (kind == FENG_SYMBOL_TYPE_KIND_NAMED ||
               kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC) {
        state->owner_decl = resolve_symbol_type_view(context->provider,
                                                     context->current_module,
                                                     context->program,
                                                     type);
    }
}

/* Replaces a symbol receiver state with an AST type from the current source. */
static void symbol_receiver_state_set_ast_type(
    const FengLspCacheQueryContext *context,
    const FengTypeRef *type_ref,
    FengLspSymbolReceiverState *state) {
    const char *builtin;

    memset(state, 0, sizeof(*state));
    state->ast_type_ref = type_ref;
    builtin = builtin_name_for_single_segment_type_ref(type_ref);
    if (builtin != NULL) {
        state->builtin_name = slice_from_cstr(builtin);
    } else if (type_ref != NULL && type_ref->kind == FENG_TYPE_REF_NAMED) {
        state->owner_decl = resolve_symbol_named_type_ref(context->provider,
                                                         context->current_module,
                                                         context->program,
                                                         type_ref);
    }
}

/* Resolves the root value of a symbol-backed receiver chain. */
static bool symbol_receiver_state_from_root(
    const FengLspCacheQueryContext *context,
    const FengLspReceiverChain *chain,
    const FengLspLocalList *locals,
    FengLspSymbolReceiverState *state) {
    const FengLspLocal *local;
    const FengSymbolDeclView *decl;

    memset(state, 0, sizeof(*state));
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_STRING) {
        state->builtin_name = slice_from_cstr("string");
        return true;
    }
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_INTEGER) {
        state->builtin_name = slice_from_cstr("i32");
        return true;
    }
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_FLOAT) {
        state->builtin_name = slice_from_cstr("f64");
        return true;
    }
    if (chain->root_kind == FENG_LSP_RECEIVER_ROOT_BOOL) {
        state->builtin_name = slice_from_cstr("bool");
        return true;
    }
    local = find_local(locals, chain->root);
    if (local != NULL) {
        if (local->kind == FENG_LSP_LOCAL_PARAM && local->parameter != NULL) {
            symbol_receiver_state_set_ast_type(context, local->parameter->type, state);
            return state->owner_decl != NULL || state->ast_type_ref != NULL ||
                   state->builtin_name.length > 0U;
        }
        if (local->kind == FENG_LSP_LOCAL_BINDING && local->binding != NULL) {
            if (local->binding->type != NULL) {
                symbol_receiver_state_set_ast_type(context, local->binding->type, state);
            } else {
                state->owner_decl =
                    resolve_symbol_owner_decl_from_initializer_expr(context,
                                                                    local->binding->initializer);
            }
            return state->owner_decl != NULL || state->ast_type_ref != NULL ||
                   state->builtin_name.length > 0U;
        }
        if (local->kind == FENG_LSP_LOCAL_SELF) {
            FengExpr self_expr = {0};

            self_expr.kind = FENG_EXPR_SELF;
            state->owner_decl = resolve_symbol_owner_decl_from_object_expr(context,
                                                                           &self_expr,
                                                                           locals);
            return state->owner_decl != NULL;
        }
    }
    decl = resolve_symbol_value_name(context->provider,
                                     context->current_module,
                                     context->program,
                                     chain->root);
    if (decl != NULL && feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_BINDING) {
        symbol_receiver_state_set_symbol_type(context,
                                              feng_symbol_decl_value_type(decl),
                                              state);
        return state->owner_decl != NULL || state->symbol_type != NULL ||
               state->builtin_name.length > 0U;
    }
    if (decl != NULL && feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_FUNCTION) {
        state->pending_call = FENG_LSP_RECEIVER_PENDING_FUNCTION;
        state->pending_callable = decl;
        return true;
    }
    state->owner_decl = resolve_symbol_type_name(context->provider,
                                                 context->current_module,
                                                 context->program,
                                                 chain->root);
    if (state->owner_decl != NULL) {
        state->pending_call = FENG_LSP_RECEIVER_PENDING_CONSTRUCTOR;
        return true;
    }
    return false;
}

/* Resolves a mixed member/call/index receiver against the published symbol
 * index when dirty parsing does not retain its final member access. */
static const FengSymbolDeclView *resolve_symbol_owner_decl_from_receiver_text(
    const FengLspCacheQueryContext *context,
    FengSlice receiver,
    const FengLspLocalList *locals,
    FengSlice *out_builtin_name) {
    FengLspReceiverChain chain = {0};
    FengLspSymbolReceiverState state = {0};
    size_t index;
    bool valid;

    if (out_builtin_name != NULL) {
        *out_builtin_name = (FengSlice){0};
    }
    if (!receiver_chain_parse(receiver, &chain) ||
        !symbol_receiver_state_from_root(context, &chain, locals, &state)) {
        receiver_chain_dispose(&chain);
        return NULL;
    }
    valid = true;
    for (index = 0U; valid && index < chain.operation_count; ++index) {
        const FengLspReceiverOperation *operation = &chain.operations[index];

        if (operation->kind == FENG_LSP_RECEIVER_MEMBER) {
            const FengSymbolDeclView *member;
            FengSymbolDeclKind kind;

            if (state.pending_call != FENG_LSP_RECEIVER_PENDING_NONE ||
                state.owner_decl == NULL) {
                valid = false;
                break;
            }
            member = find_symbol_type_member_by_name(state.owner_decl, operation->member);
            if (member == NULL) {
                member = find_symbol_fit_member_by_name(context,
                                                        state.owner_decl,
                                                        operation->member);
            }
            if (member == NULL) {
                valid = false;
                break;
            }
            kind = feng_symbol_decl_kind(member);
            if (kind == FENG_SYMBOL_DECL_KIND_FIELD) {
                symbol_receiver_state_set_symbol_type(context,
                                                      feng_symbol_decl_value_type(member),
                                                      &state);
            } else if (kind == FENG_SYMBOL_DECL_KIND_METHOD ||
                       kind == FENG_SYMBOL_DECL_KIND_FUNCTION) {
                memset(&state, 0, sizeof(state));
                state.pending_call = FENG_LSP_RECEIVER_PENDING_MEMBER;
                state.pending_callable = member;
            } else {
                valid = false;
            }
        } else if (operation->kind == FENG_LSP_RECEIVER_CALL) {
            if (state.pending_call == FENG_LSP_RECEIVER_PENDING_CONSTRUCTOR) {
                state.pending_call = FENG_LSP_RECEIVER_PENDING_NONE;
            } else if ((state.pending_call == FENG_LSP_RECEIVER_PENDING_FUNCTION ||
                        state.pending_call == FENG_LSP_RECEIVER_PENDING_MEMBER) &&
                       state.pending_callable != NULL) {
                symbol_receiver_state_set_symbol_type(
                    context,
                    feng_symbol_decl_return_type(state.pending_callable),
                    &state);
            } else {
                valid = false;
            }
        } else if (state.pending_call != FENG_LSP_RECEIVER_PENDING_NONE) {
            valid = false;
        } else if (state.ast_type_ref != NULL &&
                   state.ast_type_ref->kind == FENG_TYPE_REF_ARRAY) {
            symbol_receiver_state_set_ast_type(context,
                                               state.ast_type_ref->as.inner,
                                               &state);
        } else if (state.symbol_type != NULL &&
                   feng_symbol_type_kind(state.symbol_type) == FENG_SYMBOL_TYPE_KIND_ARRAY) {
            symbol_receiver_state_set_symbol_type(context,
                                                  feng_symbol_type_inner(state.symbol_type),
                                                  &state);
        } else {
            valid = false;
        }
    }
    receiver_chain_dispose(&chain);
    if (!valid || state.pending_call != FENG_LSP_RECEIVER_PENDING_NONE) {
        return NULL;
    }
    if (out_builtin_name != NULL) {
        *out_builtin_name = state.builtin_name;
    }
    return state.owner_decl;
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
        /* Bare builtin type identifiers: string., i32., etc. */
        if (local == NULL) {
            const char *builtin = builtin_name_for_identifier(expr->as.identifier);

            if (builtin != NULL) {
                return slice_from_cstr(builtin);
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
static bool resolve_type_ref_at_offset(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengTypeRef *type_ref,
                                       size_t offset,
                                       FengLspResolvedTarget *target,
                                       const FengDecl *owner_decl,
                                       const FengTypeParam *type_params,
                                       size_t type_param_count);
static FengSlice member_name_slice(const FengTypeMember *member);
static const FengExpr *find_expr_hit(const FengExpr *expr, size_t offset);
static const FengExpr *find_expr_hit_in_block(const FengBlock *block, size_t offset);

static bool find_decl_token_hit_member(const char *source_text,
                                       const FengDecl *owner_decl,
                                       const FengTypeMember *member,
                                       size_t offset,
                                       FengLspResolvedTarget *target) {
    size_t index;
    bool hit_member_name;
    bool hit_member_token;

    /* Generated mixin members have no declaration identifier in the target
     * source. Their token and name slices belong to the expansion directive
     * or source member and must not participate in raw-source declaration
     * lookup. Ordinary use sites are resolved through member expressions. */
    if (member->mixin_origin != NULL) {
        return false;
    }
    hit_member_name = offset_in_slice_from_source(source_text,
                                                  member_name_slice(member),
                                                  offset);
    hit_member_token = offset_in_token(member->token, offset);
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
    for (index = 0U; index < member->as.callable.type_param_count; ++index) {
        if (offset_in_token(member->as.callable.type_params[index].token, offset) ||
            offset_in_slice_from_source(source_text,
                                        member->as.callable.type_params[index].name,
                                        offset)) {
            target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
            target->type_param = &member->as.callable.type_params[index];
            target->type_param_owner = owner_decl;
            return true;
        }
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
            for (index = 0U; index < decl->as.function_decl.type_param_count; ++index) {
                if (offset_in_token(decl->as.function_decl.type_params[index].token, offset) ||
                    offset_in_slice_from_source(source_text,
                                                decl->as.function_decl.type_params[index].name,
                                                offset)) {
                    target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                    target->type_param = &decl->as.function_decl.type_params[index];
                    target->type_param_owner = decl;
                    return true;
                }
            }
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
            for (index = 0U; index < decl->as.enum_decl.item_count; ++index) {
                const FengEnumItem *item = &decl->as.enum_decl.items[index];

                if (offset_in_token(item->token, offset) ||
                    offset_in_slice_from_source(source_text, item->name, offset)) {
                    target->kind = FENG_LSP_RESOLVED_ENUM_ITEM;
                    target->decl = decl;
                    target->enum_item = item;
                    return true;
                }
            }
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.type_param_count; ++index) {
                if (offset_in_token(decl->as.type_decl.type_params[index].token, offset) ||
                    offset_in_slice_from_source(source_text,
                                                decl->as.type_decl.type_params[index].name,
                                                offset)) {
                    target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                    target->type_param = &decl->as.type_decl.type_params[index];
                    target->type_param_owner = decl;
                    return true;
                }
            }
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
            for (index = 0U; index < decl->as.spec_decl.type_param_count; ++index) {
                if (offset_in_token(decl->as.spec_decl.type_params[index].token, offset) ||
                    offset_in_slice_from_source(source_text,
                                                decl->as.spec_decl.type_params[index].name,
                                                offset)) {
                    target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                    target->type_param = &decl->as.spec_decl.type_params[index];
                    target->type_param_owner = decl;
                    return true;
                }
            }
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

/* Resolves the owner or item segment of a qualified enum-item type label. */
static bool resolve_enum_item_type_ref_at_offset(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengTypeRef *type_ref,
    size_t offset,
    FengLspResolvedTarget *target) {
    const FengCliLoadedSource *source;
    FengTypeRef owner_ref;
    const FengDecl *owner_decl;
    const FengEnumItem *item;
    FengSlice owner_name;
    FengSlice item_name;

    if (session == NULL || program == NULL || type_ref == NULL || target == NULL ||
        type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count < 2U) {
        return false;
    }
    owner_ref = *type_ref;
    --owner_ref.as.named.segment_count;
    owner_ref.as.named.type_args = NULL;
    owner_ref.as.named.type_arg_count = 0U;
    owner_decl = resolve_named_type_ref(session, program, &owner_ref);
    if (owner_decl == NULL || owner_decl->kind != FENG_DECL_ENUM) {
        return false;
    }
    source = find_source(session, program->path);
    if (source == NULL) {
        return false;
    }
    owner_name = owner_ref.as.named.segments[owner_ref.as.named.segment_count - 1U];
    item_name = type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];
    if (offset_in_slice_from_source(source->source, item_name, offset)) {
        item = find_enum_item_by_name(owner_decl, item_name);
        if (item == NULL) {
            return false;
        }
        target->kind = FENG_LSP_RESOLVED_ENUM_ITEM;
        target->decl = owner_decl;
        target->enum_item = item;
        return true;
    }
    if (offset_in_slice_from_source(source->source, owner_name, offset)) {
        target->kind = FENG_LSP_RESOLVED_DECL;
        target->decl = owner_decl;
        return true;
    }
    return false;
}

/* Recursively check whether offset falls within type_ref (including generic
 * type arguments, pointer inner, and array inner).  When a hit is found the
 * named type is resolved and target is populated.
 *
 * When owner_decl/type_params/type_param_count are provided, a single-segment
 * named type ref that fails module lookup is checked against the enclosing
 * type parameters — this handles references like `K` inside `Hashable<K>`. */
static bool resolve_type_ref_at_offset(const FengLspAnalysisSession *session,
                                       const FengProgram *program,
                                       const FengTypeRef *type_ref,
                                       size_t offset,
                                       FengLspResolvedTarget *target,
                                       const FengDecl *owner_decl,
                                       const FengTypeParam *type_params,
                                       size_t type_param_count) {
    size_t index;

    if (type_ref == NULL) {
        return false;
    }
    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            if (resolve_enum_item_type_ref_at_offset(session,
                                                     program,
                                                     type_ref,
                                                     offset,
                                                     target)) {
                return true;
            }
            if (offset >= type_ref->token.offset && offset <= named_type_ref_end(type_ref)) {
                const FengDecl *decl = resolve_named_type_ref(session, program, type_ref);
                if (decl != NULL) {
                    target->kind = FENG_LSP_RESOLVED_DECL;
                    target->decl = decl;
                    return true;
                }
                /* Fallback: single-segment name that didn't resolve to any
                 * module type may be a type parameter reference. */
                if (type_ref->as.named.segment_count == 1U && type_params != NULL) {
                    FengSlice seg = type_ref->as.named.segments[0];
                    for (index = 0U; index < type_param_count; ++index) {
                        FengSlice pname = type_params[index].name;
                        if (seg.length == pname.length &&
                            seg.data != NULL && pname.data != NULL &&
                            memcmp(seg.data, pname.data, seg.length) == 0) {
                            target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                            target->type_param = &type_params[index];
                            target->type_param_owner = owner_decl;
                            return true;
                        }
                    }
                }
            }
            for (index = 0U; index < type_ref->as.named.type_arg_count; ++index) {
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               type_ref->as.named.type_args[index],
                                               offset,
                                               target,
                                               owner_decl,
                                               type_params,
                                               type_param_count)) {
                    return true;
                }
            }
            return false;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            return resolve_type_ref_at_offset(session, program, type_ref->as.inner, offset, target,
                                              owner_decl, type_params, type_param_count);
    }
    return false;
}

/* Check whether offset falls on a type parameter name (or its constraint
 * TypeRef).  Returns true and populates target with TYPE_PARAM when the
 * name matches, or delegates to resolve_type_ref_at_offset for the
 * constraint TypeRef. */
static bool resolve_type_param_hit(const FengLspAnalysisSession *session,
                                   const FengProgram *program,
                                   const FengDecl *owner_decl,
                                   const FengTypeParam *type_params,
                                   size_t type_param_count,
                                   size_t offset,
                                   FengLspResolvedTarget *target) {
    size_t index;

    if (type_params == NULL) {
        return false;
    }
    for (index = 0U; index < type_param_count; ++index) {
        if (offset_in_token(type_params[index].token, offset)) {
            target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
            target->type_param = &type_params[index];
            target->type_param_owner = owner_decl;
            return true;
        }
        if (type_params[index].constraint != NULL &&
            resolve_type_ref_at_offset(session,
                                       program,
                                       type_params[index].constraint,
                                       offset,
                                       target,
                                       owner_decl,
                                       type_params,
                                       type_param_count)) {
            return true;
        }
    }
    return false;
}

/* Forward declaration for mutual recursion with find_type_ref_in_expr. */
static bool find_type_ref_in_block_exprs(const FengBlock *block,
                                         const FengProgram *program,
                                         const FengLspAnalysisSession *session,
                                         size_t offset,
                                         FengLspResolvedTarget *target,
                                         const FengDecl *owner_decl,
                                         const FengTypeParam *member_type_params,
                                         size_t member_type_param_count,
                                         const FengTypeParam *owner_type_params,
                                         size_t owner_type_param_count);

static bool find_type_ref_in_member(const FengDecl *owner_decl,
                                    const FengTypeMember *member,
                                    const FengProgram *program,
                                    const FengLspAnalysisSession *session,
                                    size_t offset,
                                    FengLspResolvedTarget *target) {
    size_t index;
    const FengTypeParam *owner_type_params = NULL;
    size_t owner_type_param_count = 0U;
    FengTypeParam inferred_type_param = {0};

    if (member == NULL || member->mixin_origin != NULL) {
        return false;
    }
    /* Extract type params from the owner decl so field type refs like T in
     * `var value: T` inside `type Box<T>` can resolve to the type parameter. */
    if (owner_decl != NULL) {
        if (owner_decl->kind == FENG_DECL_TYPE) {
            owner_type_params = owner_decl->as.type_decl.type_params;
            owner_type_param_count = owner_decl->as.type_decl.type_param_count;
        } else if (owner_decl->kind == FENG_DECL_SPEC) {
            owner_type_params = owner_decl->as.spec_decl.type_params;
            owner_type_param_count = owner_decl->as.spec_decl.type_param_count;
        } else if (owner_decl->kind == FENG_DECL_FIT) {
            /* For `fit T[]`, infer T as a type parameter from the array target. */
            FengTypeRef *fit_cursor = owner_decl->as.fit_decl.target;
            bool has_array_layer = false;

            while (fit_cursor != NULL && fit_cursor->kind == FENG_TYPE_REF_ARRAY) {
                has_array_layer = true;
                fit_cursor = fit_cursor->as.inner;
            }
            if (has_array_layer && fit_cursor != NULL &&
                fit_cursor->kind == FENG_TYPE_REF_NAMED &&
                fit_cursor->as.named.segment_count == 1U &&
                fit_cursor->as.named.type_arg_count == 0U &&
                resolve_named_type_ref(session, program, fit_cursor) == NULL) {
                inferred_type_param.token = owner_decl->token;
                inferred_type_param.name = fit_cursor->as.named.segments[0];
                inferred_type_param.constraint = NULL;
                owner_type_params = &inferred_type_param;
                owner_type_param_count = 1U;
            }
        }
    }

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return resolve_type_ref_at_offset(session, program, member->as.field.type, offset, target,
                                          owner_decl, owner_type_params, owner_type_param_count);
    }
    if (resolve_type_param_hit(session,
                               program,
                               owner_decl,
                               member->as.callable.type_params,
                               member->as.callable.type_param_count,
                               offset,
                               target)) {
        return true;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (resolve_type_ref_at_offset(session,
                                       program,
                                       member->as.callable.params[index].type,
                                       offset,
                                       target,
                                       owner_decl,
                                       member->as.callable.type_params,
                                       member->as.callable.type_param_count)) {
            return true;
        }
        /* Fallback to owner (type/spec) type params for refs like T in
         * `func same(other: T): bool` inside `spec Hashable<T>`. */
        if (resolve_type_ref_at_offset(session,
                                       program,
                                       member->as.callable.params[index].type,
                                       offset,
                                       target,
                                       owner_decl,
                                       owner_type_params,
                                       owner_type_param_count)) {
            return true;
        }
    }
    if (resolve_type_ref_at_offset(session,
                                   program,
                                   member->as.callable.return_type,
                                   offset,
                                   target,
                                   owner_decl,
                                   member->as.callable.type_params,
                                   member->as.callable.type_param_count)) {
        return true;
    }
    /* Fallback to owner (type/spec) type params for return type. */
    if (resolve_type_ref_at_offset(session,
                                   program,
                                   member->as.callable.return_type,
                                   offset,
                                   target,
                                   owner_decl,
                                   owner_type_params,
                                   owner_type_param_count)) {
        return true;
    }
    /* Check type refs inside the callable body expressions (e.g. T in
     * `Span<T>(...)` within method bodies). */
    return find_type_ref_in_block_exprs(member->as.callable.body,
                                        program, session, offset, target,
                                        owner_decl,
                                        member->as.callable.type_params,
                                        member->as.callable.type_param_count,
                                        owner_type_params,
                                        owner_type_param_count);
}

/* Resolves type references carried by one match label at the cursor offset. */
static bool resolve_match_label_type_ref_at_offset(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengMatchLabel *label,
    size_t offset,
    FengLspResolvedTarget *target,
    const FengDecl *owner_decl,
    const FengTypeParam *type_params,
    size_t type_param_count) {
    size_t index;

    if (label == NULL || label->kind != FENG_MATCH_LABEL_TYPE) {
        return false;
    }
    if (resolve_type_ref_at_offset(session,
                                   program,
                                   label->type,
                                   offset,
                                   target,
                                   owner_decl,
                                   type_params,
                                   type_param_count)) {
        return true;
    }
    for (index = 0U; index < label->type_chain_count; ++index) {
        if (resolve_type_ref_at_offset(session,
                                       program,
                                       label->type_chain[index],
                                       offset,
                                       target,
                                       owner_decl,
                                       type_params,
                                       type_param_count)) {
            return true;
        }
    }
    return false;
}

/* Walk an expression tree looking for TypeRefs inside generic targets
 * (e.g. T in `Span<T>`), explicit call type args (e.g. T in `func<T>(...)`),
 * cast types, and array-new element types.  Recurses into sub-expressions. */
static bool find_type_ref_in_expr(const FengExpr *expr,
                                  const FengProgram *program,
                                  const FengLspAnalysisSession *session,
                                  size_t offset,
                                  FengLspResolvedTarget *target,
                                  const FengDecl *owner_decl,
                                  const FengTypeParam *member_type_params,
                                  size_t member_type_param_count,
                                  const FengTypeParam *owner_type_params,
                                  size_t owner_type_param_count) {
    size_t index;

    if (expr == NULL || offset < expr_start(expr) || offset > expr_end(expr)) {
        return false;
    }
    switch (expr->kind) {
        case FENG_EXPR_GENERIC_TARGET:
            for (index = 0U; index < expr->as.generic_target.type_arg_count; ++index) {
                if (resolve_type_ref_at_offset(session, program,
                                               expr->as.generic_target.type_args[index],
                                               offset, target, owner_decl,
                                               member_type_params, member_type_param_count)) {
                    return true;
                }
                if (resolve_type_ref_at_offset(session, program,
                                               expr->as.generic_target.type_args[index],
                                               offset, target, owner_decl,
                                               owner_type_params, owner_type_param_count)) {
                    return true;
                }
            }
            return find_type_ref_in_expr(expr->as.generic_target.target,
                                         program, session, offset, target,
                                         owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_CALL:
            if (expr->as.call.has_explicit_type_args) {
                for (index = 0U; index < expr->as.call.explicit_type_arg_count; ++index) {
                    if (resolve_type_ref_at_offset(session, program,
                                                   expr->as.call.explicit_type_args[index],
                                                   offset, target, owner_decl,
                                                   member_type_params, member_type_param_count)) {
                        return true;
                    }
                    if (resolve_type_ref_at_offset(session, program,
                                                   expr->as.call.explicit_type_args[index],
                                                   offset, target, owner_decl,
                                                   owner_type_params, owner_type_param_count)) {
                        return true;
                    }
                }
            }
            if (find_type_ref_in_expr(expr->as.call.callee,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                if (find_type_ref_in_expr(expr->as.call.args[index],
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_CAST:
            if (resolve_type_ref_at_offset(session, program,
                                           expr->as.cast.type, offset, target, owner_decl,
                                           member_type_params, member_type_param_count)) {
                return true;
            }
            if (resolve_type_ref_at_offset(session, program,
                                           expr->as.cast.type, offset, target, owner_decl,
                                           owner_type_params, owner_type_param_count)) {
                return true;
            }
            return find_type_ref_in_expr(expr->as.cast.value,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_ARRAY_NEW:
            if (resolve_type_ref_at_offset(session, program,
                                           expr->as.array_new.element_type, offset, target,
                                           owner_decl,
                                           member_type_params, member_type_param_count)) {
                return true;
            }
            if (resolve_type_ref_at_offset(session, program,
                                           expr->as.array_new.element_type, offset, target,
                                           owner_decl,
                                           owner_type_params, owner_type_param_count)) {
                return true;
            }
            return find_type_ref_in_expr(expr->as.array_new.size,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (find_type_ref_in_expr(expr->as.array_literal.items[index],
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_OBJECT_LITERAL:
            if (find_type_ref_in_expr(expr->as.object_literal.target,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                if (find_type_ref_in_expr(expr->as.object_literal.fields[index].value,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_MEMBER:
            return find_type_ref_in_expr(expr->as.member.object,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_INDEX:
            if (find_type_ref_in_expr(expr->as.index.object,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            return find_type_ref_in_expr(expr->as.index.index,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_UNARY:
            return find_type_ref_in_expr(expr->as.unary.operand,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_BINARY:
            if (find_type_ref_in_expr(expr->as.binary.left,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            return find_type_ref_in_expr(expr->as.binary.right,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_MATCH_OP: {
            const FengCliLoadedSource *source = find_source(session, program->path);

            if (expr->as.match_op.has_binding && source != NULL &&
                offset_in_slice_from_source(source->source,
                                            expr->as.match_op.binding_name,
                                            offset)) {
                target->kind = FENG_LSP_RESOLVED_MATCH_BINDING;
                target->match_op = expr;
                return true;
            }
            if (find_type_ref_in_expr(expr->as.match_op.target,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            for (index = 0U; index < expr->as.match_op.label_count; ++index) {
                const FengMatchLabel *label = &expr->as.match_op.labels[index];

                if (resolve_match_label_type_ref_at_offset(
                        session,
                        program,
                        label,
                        offset,
                        target,
                        owner_decl,
                        member_type_params,
                        member_type_param_count) ||
                    resolve_match_label_type_ref_at_offset(
                        session,
                        program,
                        label,
                        offset,
                        target,
                        owner_decl,
                        owner_type_params,
                        owner_type_param_count)) {
                    return true;
                }
            }
            return false;
        }
        case FENG_EXPR_LAMBDA:
            for (index = 0U; index < expr->as.lambda.param_count; ++index) {
                if (offset_in_token(expr->as.lambda.params[index].token, offset)) {
                    target->kind = FENG_LSP_RESOLVED_PARAM;
                    target->parameter = &expr->as.lambda.params[index];
                    return true;
                }
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               expr->as.lambda.params[index].type,
                                               offset,
                                               target,
                                               owner_decl,
                                               member_type_params,
                                               member_type_param_count) ||
                    resolve_type_ref_at_offset(session,
                                               program,
                                               expr->as.lambda.params[index].type,
                                               offset,
                                               target,
                                               owner_decl,
                                               owner_type_params,
                                               owner_type_param_count)) {
                    return true;
                }
            }
            if (expr->as.lambda.is_block_body) {
                return find_type_ref_in_block_exprs(expr->as.lambda.body_block,
                                                    program, session, offset, target,
                                                    owner_decl,
                                                    member_type_params, member_type_param_count,
                                                    owner_type_params, owner_type_param_count);
            }
            return find_type_ref_in_expr(expr->as.lambda.body,
                                         program, session, offset, target, owner_decl,
                                         member_type_params, member_type_param_count,
                                         owner_type_params, owner_type_param_count);
        case FENG_EXPR_IF:
            if (find_type_ref_in_expr(expr->as.if_expr.condition,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            if (find_type_ref_in_block_exprs(expr->as.if_expr.then_block,
                                             program, session, offset, target,
                                             owner_decl,
                                             member_type_params, member_type_param_count,
                                             owner_type_params, owner_type_param_count)) {
                return true;
            }
            return find_type_ref_in_block_exprs(expr->as.if_expr.else_block,
                                                program, session, offset, target,
                                                owner_decl,
                                                member_type_params, member_type_param_count,
                                                owner_type_params, owner_type_param_count);
        case FENG_EXPR_MATCH:
            if (find_type_ref_in_expr(expr->as.match_expr.target,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                const FengMatchBranch *branch = &expr->as.match_expr.branches[index];
                size_t label_index;

                for (label_index = 0U; label_index < branch->label_count; ++label_index) {
                    const FengMatchLabel *label = &branch->labels[label_index];

                    if (resolve_match_label_type_ref_at_offset(
                            session,
                            program,
                            label,
                            offset,
                            target,
                            owner_decl,
                            member_type_params,
                            member_type_param_count) ||
                        resolve_match_label_type_ref_at_offset(
                            session,
                            program,
                            label,
                            offset,
                            target,
                            owner_decl,
                            owner_type_params,
                            owner_type_param_count)) {
                        return true;
                    }
                }
                if (find_type_ref_in_block_exprs(expr->as.match_expr.branches[index].body,
                                                 program, session, offset, target,
                                                 owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
            }
            return find_type_ref_in_block_exprs(expr->as.match_expr.else_block,
                                                program, session, offset, target,
                                                owner_decl,
                                                member_type_params, member_type_param_count,
                                                owner_type_params, owner_type_param_count);
        case FENG_EXPR_TRY:
            if (find_type_ref_in_expr(expr->as.try_expr.body,
                                      program, session, offset, target, owner_decl,
                                      member_type_params, member_type_param_count,
                                      owner_type_params, owner_type_param_count)) {
                return true;
            }
            for (index = 0U; index < expr->as.try_expr.clause_count; ++index) {
                if (find_type_ref_in_block_exprs(expr->as.try_expr.clauses[index].body,
                                                 program, session, offset, target,
                                                 owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
            }
            return false;
        default:
            return false;
    }
}

/* Walk a block looking for TypeRefs inside expressions (e.g. generic type
 * arguments like T in `Span<T>(...)` within method bodies). */
static bool find_type_ref_in_block_exprs(const FengBlock *block,
                                         const FengProgram *program,
                                         const FengLspAnalysisSession *session,
                                         size_t offset,
                                         FengLspResolvedTarget *target,
                                         const FengDecl *owner_decl,
                                         const FengTypeParam *member_type_params,
                                         size_t member_type_param_count,
                                         const FengTypeParam *owner_type_params,
                                         size_t owner_type_param_count) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return false;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        const FengStmt *stmt = block->statements[index];
        size_t branch_index;

        if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end(stmt)) {
            continue;
        }
        switch (stmt->kind) {
            case FENG_STMT_EXPR:
            case FENG_STMT_TRY:
                if (find_type_ref_in_expr(stmt->as.expr,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_ASSIGN:
                if (find_type_ref_in_expr(stmt->as.assign.target,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                if (find_type_ref_in_expr(stmt->as.assign.value,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_BINDING:
                if (find_type_ref_in_expr(stmt->as.binding.initializer,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_RETURN:
                if (find_type_ref_in_expr(stmt->as.return_value,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_THROW:
                if (find_type_ref_in_expr(stmt->as.throw_value,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_IF:
                for (branch_index = 0U; branch_index < stmt->as.if_stmt.clause_count; ++branch_index) {
                    if (find_type_ref_in_expr(stmt->as.if_stmt.clauses[branch_index].condition,
                                              program, session, offset, target, owner_decl,
                                              member_type_params, member_type_param_count,
                                              owner_type_params, owner_type_param_count)) {
                        return true;
                    }
                    if (find_type_ref_in_block_exprs(stmt->as.if_stmt.clauses[branch_index].block,
                                                     program, session, offset, target, owner_decl,
                                                     member_type_params, member_type_param_count,
                                                     owner_type_params, owner_type_param_count)) {
                        return true;
                    }
                }
                if (find_type_ref_in_block_exprs(stmt->as.if_stmt.else_block,
                                                 program, session, offset, target, owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_MATCH:
                if (find_type_ref_in_expr(stmt->as.match_stmt.target,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                for (branch_index = 0U; branch_index < stmt->as.match_stmt.branch_count; ++branch_index) {
                    const FengMatchBranch *branch =
                        &stmt->as.match_stmt.branches[branch_index];
                    size_t label_index;

                    for (label_index = 0U; label_index < branch->label_count; ++label_index) {
                        const FengMatchLabel *label = &branch->labels[label_index];

                        if (resolve_match_label_type_ref_at_offset(
                                session,
                                program,
                                label,
                                offset,
                                target,
                                owner_decl,
                                member_type_params,
                                member_type_param_count) ||
                            resolve_match_label_type_ref_at_offset(
                                session,
                                program,
                                label,
                                offset,
                                target,
                                owner_decl,
                                owner_type_params,
                                owner_type_param_count)) {
                            return true;
                        }
                    }
                    if (find_type_ref_in_block_exprs(stmt->as.match_stmt.branches[branch_index].body,
                                                     program, session, offset, target, owner_decl,
                                                     member_type_params, member_type_param_count,
                                                     owner_type_params, owner_type_param_count)) {
                        return true;
                    }
                }
                if (find_type_ref_in_block_exprs(stmt->as.match_stmt.else_block,
                                                 program, session, offset, target, owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_WHILE:
                if (find_type_ref_in_expr(stmt->as.while_stmt.condition,
                                          program, session, offset, target, owner_decl,
                                          member_type_params, member_type_param_count,
                                          owner_type_params, owner_type_param_count)) {
                    return true;
                }
                if (find_type_ref_in_block_exprs(stmt->as.while_stmt.body,
                                                 program, session, offset, target, owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_FOR:
                if (stmt->as.for_stmt.is_for_in) {
                    if (find_type_ref_in_expr(stmt->as.for_stmt.iter_expr,
                                              program, session, offset, target, owner_decl,
                                              member_type_params, member_type_param_count,
                                              owner_type_params, owner_type_param_count)) {
                        return true;
                    }
                }
                if (find_type_ref_in_block_exprs(stmt->as.for_stmt.body,
                                                 program, session, offset, target, owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_BLOCK:
                if (find_type_ref_in_block_exprs(stmt->as.block,
                                                 program, session, offset, target, owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_DEFER:
                if (find_type_ref_in_block_exprs(stmt->as.defer_block,
                                                 program, session, offset, target, owner_decl,
                                                 member_type_params, member_type_param_count,
                                                 owner_type_params, owner_type_param_count)) {
                    return true;
                }
                break;
            case FENG_STMT_BREAK:
            case FENG_STMT_CONTINUE:
                break;
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
        case FENG_STMT_BINDING:
            return resolve_type_ref_at_offset(session, program, stmt->as.binding.type, offset, target,
                                              NULL, NULL, 0U);
        case FENG_STMT_BLOCK:
            return find_block_type_ref_hit(stmt->as.block, program, session, offset, target);
        case FENG_STMT_DEFER:
            return find_block_type_ref_hit(stmt->as.defer_block, program, session, offset, target);
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
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               stmt->as.for_stmt.iter_binding.type,
                                               offset,
                                               target,
                                               NULL, NULL, 0U)) {
                    return true;
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
            return resolve_type_ref_at_offset(session, program, decl->as.binding.type, offset, target,
                                              NULL, NULL, 0U);
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_FUNCTION:
            if (resolve_type_param_hit(session,
                                       program,
                                       decl,
                                       decl->as.function_decl.type_params,
                                       decl->as.function_decl.type_param_count,
                                       offset,
                                       target)) {
                return true;
            }
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               decl->as.function_decl.params[index].type,
                                               offset,
                                               target,
                                               decl,
                                               decl->as.function_decl.type_params,
                                               decl->as.function_decl.type_param_count)) {
                    return true;
                }
            }
            if (resolve_type_ref_at_offset(session,
                                           program,
                                           decl->as.function_decl.return_type,
                                           offset,
                                           target,
                                           decl,
                                           decl->as.function_decl.type_params,
                                           decl->as.function_decl.type_param_count)) {
                return true;
            }
            if (find_block_type_ref_hit(decl->as.function_decl.body,
                                        program,
                                        session,
                                        offset,
                                        target)) {
                return true;
            }
            if (find_type_ref_in_block_exprs(decl->as.function_decl.body,
                                             program,
                                             session,
                                             offset,
                                             target,
                                             decl,
                                             decl->as.function_decl.type_params,
                                             decl->as.function_decl.type_param_count,
                                             NULL,
                                             0U)) {
                return true;
            }
            break;
        case FENG_DECL_TYPE:
            if (resolve_type_param_hit(session,
                                       program,
                                       decl,
                                       decl->as.type_decl.type_params,
                                       decl->as.type_decl.type_param_count,
                                       offset,
                                       target)) {
                return true;
            }
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               decl->as.type_decl.declared_specs[index],
                                               offset,
                                               target,
                                               decl,
                                               decl->as.type_decl.type_params,
                                               decl->as.type_decl.type_param_count)) {
                    return true;
                }
            }
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                const FengTypeMixinDecl *mixin = &decl->as.type_decl.mixins[index];

                if ((!mixin->infer_source_type &&
                     resolve_type_ref_at_offset(session,
                                                program,
                                                mixin->source_type,
                                                offset,
                                                target,
                                                decl,
                                                decl->as.type_decl.type_params,
                                                decl->as.type_decl.type_param_count)) ||
                    find_type_ref_in_expr(mixin->source_constructor,
                                          program,
                                          session,
                                          offset,
                                          target,
                                          decl,
                                          NULL,
                                          0U,
                                          decl->as.type_decl.type_params,
                                          decl->as.type_decl.type_param_count)) {
                    return true;
                }
            }
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
            if (resolve_type_param_hit(session,
                                       program,
                                       decl,
                                       decl->as.spec_decl.type_params,
                                       decl->as.spec_decl.type_param_count,
                                       offset,
                                       target)) {
                return true;
            }
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               decl->as.spec_decl.parent_specs[index],
                                               offset,
                                               target,
                                               decl,
                                               decl->as.spec_decl.type_params,
                                               decl->as.spec_decl.type_param_count)) {
                    return true;
                }
            }
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
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_UNION) {
                for (index = 0U; index < decl->as.spec_decl.as.union_form.member_count; ++index) {
                    if (resolve_type_ref_at_offset(session,
                                                   program,
                                                   decl->as.spec_decl.as.union_form.members[index],
                                                   offset,
                                                   target,
                                                   decl,
                                                   decl->as.spec_decl.type_params,
                                                   decl->as.spec_decl.type_param_count)) {
                        return true;
                    }
                }
            }
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                for (index = 0U; index < decl->as.spec_decl.as.callable.param_count; ++index) {
                    if (resolve_type_ref_at_offset(session,
                                                   program,
                                                   decl->as.spec_decl.as.callable.params[index].type,
                                                   offset,
                                                   target,
                                                   decl,
                                                   decl->as.spec_decl.type_params,
                                                   decl->as.spec_decl.type_param_count)) {
                        return true;
                    }
                }
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               decl->as.spec_decl.as.callable.return_type,
                                               offset,
                                               target,
                                               decl,
                                               decl->as.spec_decl.type_params,
                                               decl->as.spec_decl.type_param_count)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_FIT: {
            /* Infer type param from array target (e.g. T in `fit T[]`). */
            FengTypeParam fit_inferred_param = {0};
            const FengTypeParam *fit_type_params = NULL;
            size_t fit_type_param_count = 0U;
            {
                FengTypeRef *fit_cursor = decl->as.fit_decl.target;
                bool has_array_layer = false;

                while (fit_cursor != NULL && fit_cursor->kind == FENG_TYPE_REF_ARRAY) {
                    has_array_layer = true;
                    fit_cursor = fit_cursor->as.inner;
                }
                if (has_array_layer && fit_cursor != NULL &&
                    fit_cursor->kind == FENG_TYPE_REF_NAMED &&
                    fit_cursor->as.named.segment_count == 1U &&
                    fit_cursor->as.named.type_arg_count == 0U &&
                    resolve_named_type_ref(session, program, fit_cursor) == NULL) {
                    fit_inferred_param.token = decl->token;
                    fit_inferred_param.name = fit_cursor->as.named.segments[0];
                    fit_inferred_param.constraint = NULL;
                    fit_type_params = &fit_inferred_param;
                    fit_type_param_count = 1U;
                }
            }
            if (fit_type_param_count > 0U &&
                resolve_type_param_hit(session,
                                       program,
                                       decl,
                                       fit_type_params,
                                       fit_type_param_count,
                                       offset,
                                       target)) {
                return true;
            }
            if (resolve_type_ref_at_offset(session,
                                           program,
                                           decl->as.fit_decl.target,
                                           offset,
                                           target,
                                           decl,
                                           fit_type_params,
                                           fit_type_param_count)) {
                return true;
            }
            for (index = 0U; index < decl->as.fit_decl.spec_count; ++index) {
                if (resolve_type_ref_at_offset(session,
                                               program,
                                               decl->as.fit_decl.specs[index],
                                               offset,
                                               target,
                                               decl,
                                               fit_type_params,
                                               fit_type_param_count)) {
                    return true;
                }
            }
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
        case FENG_EXPR_LAMBDA:
            return expr->as.lambda.is_block_body
                       ? find_expr_hit_in_block(expr->as.lambda.body_block, offset)
                       : find_expr_hit(expr->as.lambda.body, offset);
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
        case FENG_EXPR_MATCH_OP:
            return find_expr_hit(expr->as.match_op.target, offset);
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
            case FENG_STMT_DEFER:
                hit = find_expr_hit_in_block(stmt->as.defer_block, offset);
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
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                const FengExpr *hit = find_expr_hit(
                    decl->as.type_decl.mixins[index].source_constructor,
                    offset);

                if (hit != NULL) {
                    return hit;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];
                const FengExpr *hit = member->mixin_origin != NULL
                                          ? NULL
                                          : member->kind == FENG_TYPE_MEMBER_FIELD
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
    size_t offset = 0U;

    while (text[offset] != '\0' && current_line < line) {
        if (text[offset] == '\n') {
            ++current_line;
        }
        ++offset;
    }
    if (current_line == line) {
        unsigned int utf16_units = 0U;

        while (text[offset] != '\0' && text[offset] != '\n' && utf16_units < character) {
            unsigned char lead = (unsigned char)text[offset];
            size_t byte_count = 1U;
            unsigned int codepoint = lead;

            if ((lead & 0xE0U) == 0xC0U && text[offset + 1U] != '\0') {
                byte_count = 2U;
                codepoint = ((unsigned int)(lead & 0x1FU) << 6U) |
                            ((unsigned int)text[offset + 1U] & 0x3FU);
            } else if ((lead & 0xF0U) == 0xE0U &&
                       text[offset + 1U] != '\0' && text[offset + 2U] != '\0') {
                byte_count = 3U;
                codepoint = ((unsigned int)(lead & 0x0FU) << 12U) |
                            (((unsigned int)text[offset + 1U] & 0x3FU) << 6U) |
                            ((unsigned int)text[offset + 2U] & 0x3FU);
            } else if ((lead & 0xF8U) == 0xF0U &&
                       text[offset + 1U] != '\0' && text[offset + 2U] != '\0' &&
                       text[offset + 3U] != '\0') {
                byte_count = 4U;
                codepoint = ((unsigned int)(lead & 0x07U) << 18U) |
                            (((unsigned int)text[offset + 1U] & 0x3FU) << 12U) |
                            (((unsigned int)text[offset + 2U] & 0x3FU) << 6U) |
                            ((unsigned int)text[offset + 3U] & 0x3FU);
            }
            if (utf16_units + (codepoint > 0xFFFFU ? 2U : 1U) > character) {
                break;
            }
            utf16_units += codepoint > 0xFFFFU ? 2U : 1U;
            offset += byte_count;
        }
    }
    return offset;
}

/* Converts a position using the current document's O(1) line lookup. */
static size_t document_offset_from_position(const FengLspDocument *document,
                                            unsigned int line,
                                            unsigned int character) {
    if (document == NULL) {
        return 0U;
    }
    return feng_lsp_line_index_offset(&document->lines,
                                      document->text,
                                      line,
                                      character);
}

/* Applies one LSP incremental range edit and returns a new complete text. */
static char *apply_incremental_text_edit(const char *text,
                                         unsigned int start_line,
                                         unsigned int start_character,
                                         unsigned int end_line,
                                         unsigned int end_character,
                                         const char *replacement) {
    size_t start_offset;
    size_t end_offset;
    size_t text_length;
    size_t replacement_length;
    char *updated;

    if (text == NULL || replacement == NULL) {
        return NULL;
    }
    start_offset = offset_from_position(text, start_line, start_character);
    end_offset = offset_from_position(text, end_line, end_character);
    text_length = strlen(text);
    replacement_length = strlen(replacement);
    if (start_offset > end_offset || end_offset > text_length) {
        return NULL;
    }
    updated = (char *)malloc(start_offset + replacement_length +
                             (text_length - end_offset) + 1U);
    if (updated == NULL) {
        return NULL;
    }
    memcpy(updated, text, start_offset);
    memcpy(updated + start_offset, replacement, replacement_length);
    memcpy(updated + start_offset + replacement_length,
           text + end_offset,
           text_length - end_offset + 1U);
    return updated;
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

typedef enum FengLspTypeNameStyle {
    FENG_LSP_TYPE_NAME_QUALIFIED,
    FENG_LSP_TYPE_NAME_SHORT
} FengLspTypeNameStyle;

/* Format an AST type reference with either its complete path or only its
 * final path segment. Nested type arguments inherit the same presentation. */
static bool type_ref_to_string_with_style(FengLspString *buffer,
                                          const FengTypeRef *type_ref,
                                          FengLspTypeNameStyle style) {
    size_t index;

    if (type_ref == NULL) {
        return string_append_cstr(buffer, "void");
    }
    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED: {
            size_t segment_start = style == FENG_LSP_TYPE_NAME_SHORT &&
                                   type_ref->as.named.segment_count > 0U
                                       ? type_ref->as.named.segment_count - 1U
                                       : 0U;

            for (index = segment_start;
                 index < type_ref->as.named.segment_count;
                 ++index) {
                if (index > segment_start && !string_append_cstr(buffer, ".")) {
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
                    if (!type_ref_to_string_with_style(buffer,
                                                       type_ref->as.named.type_args[index],
                                                       style)) {
                        return false;
                    }
                }
                if (!string_append_cstr(buffer, ">")) {
                    return false;
                }
            }
            return true;
        }
        case FENG_TYPE_REF_POINTER:
            return type_ref_to_string_with_style(buffer, type_ref->as.inner, style) &&
                   string_append_cstr(buffer, "*");
        case FENG_TYPE_REF_ARRAY:
            return type_ref_to_string_with_style(buffer, type_ref->as.inner, style) &&
                   string_append_cstr(buffer, type_ref->array_element_writable ? "[!]" : "[]");
    }
    return false;
}

/* Hover deliberately presents only the final segment of named type paths. */
static bool hover_type_ref_to_string(FengLspString *buffer,
                                     const FengTypeRef *type_ref) {
    return type_ref_to_string_with_style(buffer,
                                         type_ref,
                                         FENG_LSP_TYPE_NAME_SHORT);
}

static bool parameter_type_to_string_with_style(FengLspString *buffer,
                                                const FengParameter *param,
                                                FengLspTypeNameStyle style) {
    const FengTypeRef *type = param != NULL ? param->type : NULL;

    if (param != NULL && param->is_variadic) {
        if (type != NULL && type->kind == FENG_TYPE_REF_ARRAY) {
            return type_ref_to_string_with_style(buffer, type->as.inner, style) &&
                   string_append_cstr(buffer, "...");
        }
        return type_ref_to_string_with_style(buffer, type, style) &&
               string_append_cstr(buffer, "...");
    }
    return type_ref_to_string_with_style(buffer, type, style);
}

static bool semantic_type_fact_to_string_with_style(
    FengLspString *buffer,
    const FengSemanticTypeFact *fact,
    FengLspTypeNameStyle style) {
    if (fact == NULL) {
        return false;
    }
    switch (fact->kind) {
        case FENG_SEMANTIC_TYPE_FACT_BUILTIN:
            return string_append_bytes(buffer, fact->builtin_name.data, fact->builtin_name.length);
        case FENG_SEMANTIC_TYPE_FACT_TYPE_REF:
            return type_ref_to_string_with_style(buffer, fact->type_ref, style);
        case FENG_SEMANTIC_TYPE_FACT_DECL: {
            FengSlice name = decl_name(fact->type_decl);

            return string_append_bytes(buffer, name.data, name.length);
        }
        case FENG_SEMANTIC_TYPE_FACT_UNKNOWN:
            break;
    }
    return false;
}

static bool append_optional_static_type_annotation_with_style(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const void *site,
    const FengTypeRef *explicit_type,
    FengLspTypeNameStyle style) {
    if (explicit_type != NULL) {
        return string_append_cstr(buffer, ": ") &&
               type_ref_to_string_with_style(buffer, explicit_type, style);
    }
    if (session != NULL && session->analysis != NULL) {
        const FengSemanticTypeFact *fact = feng_semantic_lookup_type_fact(session->analysis, site);

        if (fact != NULL) {
            return string_append_cstr(buffer, ": ") &&
                   semantic_type_fact_to_string_with_style(buffer, fact, style);
        }
    }
    return true;
}

static bool binding_signature_to_string_with_style(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengBinding *binding,
    FengLspTypeNameStyle style) {
    const char *literal_type = NULL;

    if (!string_append_cstr(buffer,
                            binding->mutability == FENG_MUTABILITY_VAR ? "var " : "let ") ||
        !string_append_bytes(buffer, binding->name.data, binding->name.length) ||
        !append_optional_static_type_annotation_with_style(buffer,
                                                           session,
                                                           binding,
                                                           binding->type,
                                                           style)) {
        return false;
    }
    if (binding->type != NULL || (session != NULL && session->analysis != NULL) ||
        binding->initializer == NULL) {
        return true;
    }
    switch (binding->initializer->kind) {
        case FENG_EXPR_STRING:
            literal_type = "string";
            break;
        case FENG_EXPR_BOOL:
            literal_type = "bool";
            break;
        case FENG_EXPR_INTEGER:
            literal_type = "i32";
            break;
        case FENG_EXPR_FLOAT:
            literal_type = "f64";
            break;
        default:
            break;
    }
    return literal_type == NULL ||
           (string_append_cstr(buffer, ": ") &&
            string_append_cstr(buffer, literal_type));
}

/* Format the narrowed static type of an infix-match binding from its validated
 * type labels. Chain patterns bind to their deepest type; multiple labels form
 * the subset union described by docs/specifications/feng-flow.md. */
static bool match_binding_signature_to_string_with_style(
    FengLspString *buffer,
    const FengExpr *match_op,
    FengLspTypeNameStyle style) {
    size_t index;

    if (match_op == NULL || match_op->kind != FENG_EXPR_MATCH_OP ||
        !match_op->as.match_op.has_binding || match_op->as.match_op.label_count == 0U) {
        return false;
    }
    if (!string_append_cstr(buffer,
                            match_op->as.match_op.binding_mutability == FENG_MUTABILITY_VAR
                                ? "var "
                                : "let ") ||
        !string_append_bytes(buffer,
                             match_op->as.match_op.binding_name.data,
                             match_op->as.match_op.binding_name.length) ||
        !string_append_cstr(buffer, ": ")) {
        return false;
    }
    for (index = 0U; index < match_op->as.match_op.label_count; ++index) {
        const FengMatchLabel *label = &match_op->as.match_op.labels[index];
        const FengTypeRef *type_ref;

        if (label->kind != FENG_MATCH_LABEL_TYPE) {
            return false;
        }
        type_ref = label->type_chain_count > 0U
                       ? label->type_chain[label->type_chain_count - 1U]
                       : label->type;
        if ((index > 0U && !string_append_cstr(buffer, " | ")) ||
            !type_ref_to_string_with_style(buffer, type_ref, style)) {
            return false;
        }
    }
    return true;
}

static bool decl_signature_to_string_with_session(FengLspString *buffer,
                                                  const FengLspAnalysisSession *session,
                                                  const FengDecl *decl);
static bool decl_signature_to_string_with_session_and_style(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengDecl *decl,
    FengLspTypeNameStyle style);
static bool member_signature_to_string_with_session(FengLspString *buffer,
                                                    const FengLspAnalysisSession *session,
                                                    const FengTypeMember *member);
static bool member_signature_to_string_with_session_and_style(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengTypeMember *member,
    FengLspTypeNameStyle style);

static bool append_decl_type_params_with_style(FengLspString *buffer,
                                               const FengTypeParam *params,
                                               size_t count,
                                               FengLspTypeNameStyle style) {
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
                !type_ref_to_string_with_style(buffer, params[i].constraint, style)) {
                return false;
            }
        }
    }
    return string_append_cstr(buffer, ">");
}

/* Return the stable user-facing label for a proven type category. */
static const char *hover_type_category_label(FengLspTypeCategory category) {
    switch (category) {
        case FENG_LSP_TYPE_CATEGORY_REFERENCE:
            return "Reference Type";
        case FENG_LSP_TYPE_CATEGORY_VALUE:
            return "Value Type";
        case FENG_LSP_TYPE_CATEGORY_TUPLE:
            return "Tuple Type";
        case FENG_LSP_TYPE_CATEGORY_ENUM:
            return "Enum";
        case FENG_LSP_TYPE_CATEGORY_OBJECT_SPEC:
            return "Object Spec";
        case FENG_LSP_TYPE_CATEGORY_CALLBACK_SPEC:
            return "Callback Spec";
        case FENG_LSP_TYPE_CATEGORY_UNION_SPEC:
            return "Union Spec";
        case FENG_LSP_TYPE_CATEGORY_INTERSECTION_SPEC:
            return "Intersection Spec";
        case FENG_LSP_TYPE_CATEGORY_ARRAY:
            return "Array";
        case FENG_LSP_TYPE_CATEGORY_BUILTIN:
            return "Builtin";
        case FENG_LSP_TYPE_CATEGORY_POINTER:
            return "Pointer";
        case FENG_LSP_TYPE_CATEGORY_UNKNOWN:
            break;
    }
    return NULL;
}

/* Classify a declaration using semantic flags already present in the AST. */
static FengLspTypeCategory hover_category_from_decl(const FengDecl *decl) {
    if (decl == NULL) {
        return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        if (decl->as.type_decl.is_tuple) {
            return FENG_LSP_TYPE_CATEGORY_TUPLE;
        }
        return decl->as.type_decl.is_value ? FENG_LSP_TYPE_CATEGORY_VALUE
                                           : FENG_LSP_TYPE_CATEGORY_REFERENCE;
    }
    if (decl->kind == FENG_DECL_ENUM) {
        return FENG_LSP_TYPE_CATEGORY_ENUM;
    }
    if (decl->kind == FENG_DECL_SPEC) {
        switch (decl->as.spec_decl.form) {
            case FENG_SPEC_FORM_OBJECT:
                return FENG_LSP_TYPE_CATEGORY_OBJECT_SPEC;
            case FENG_SPEC_FORM_CALLABLE:
                return FENG_LSP_TYPE_CATEGORY_CALLBACK_SPEC;
            case FENG_SPEC_FORM_UNION:
                return FENG_LSP_TYPE_CATEGORY_UNION_SPEC;
            case FENG_SPEC_FORM_INTERSECTION:
                return FENG_LSP_TYPE_CATEGORY_INTERSECTION_SPEC;
        }
    }
    return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
}

/* Classify a declaration using flags from the persistent symbol index. */
static FengLspTypeCategory hover_category_from_symbol_decl(
    const FengSymbolDeclView *decl) {
    if (decl == NULL) {
        return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
    }
    if (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_TYPE) {
        if (feng_symbol_decl_is_tuple(decl)) {
            return FENG_LSP_TYPE_CATEGORY_TUPLE;
        }
        return feng_symbol_decl_is_value_type(decl) ? FENG_LSP_TYPE_CATEGORY_VALUE
                                                    : FENG_LSP_TYPE_CATEGORY_REFERENCE;
    }
    if (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_ENUM) {
        return FENG_LSP_TYPE_CATEGORY_ENUM;
    }
    if (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_SPEC) {
        switch (feng_symbol_decl_spec_form(decl)) {
            case FENG_SPEC_FORM_OBJECT:
                return FENG_LSP_TYPE_CATEGORY_OBJECT_SPEC;
            case FENG_SPEC_FORM_CALLABLE:
                return FENG_LSP_TYPE_CATEGORY_CALLBACK_SPEC;
            case FENG_SPEC_FORM_UNION:
                return FENG_LSP_TYPE_CATEGORY_UNION_SPEC;
            case FENG_SPEC_FORM_INTERSECTION:
                return FENG_LSP_TYPE_CATEGORY_INTERSECTION_SPEC;
        }
    }
    return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
}

/* Classify the outermost AST type without recursively expanding its target. */
static FengLspTypeCategory hover_category_from_type_ref(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengTypeRef *type_ref) {
    if (type_ref == NULL) {
        return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
    }
    if (type_ref->kind == FENG_TYPE_REF_ARRAY) {
        return FENG_LSP_TYPE_CATEGORY_ARRAY;
    }
    if (type_ref->kind == FENG_TYPE_REF_POINTER) {
        return FENG_LSP_TYPE_CATEGORY_POINTER;
    }
    if (type_ref->kind == FENG_TYPE_REF_NAMED &&
        type_ref->as.named.segment_count == 1U &&
        feng_semantic_is_builtin_type_name(type_ref->as.named.segments[0])) {
        return FENG_LSP_TYPE_CATEGORY_BUILTIN;
    }
    return hover_category_from_decl(resolve_named_type_ref(session, program, type_ref));
}

/* Classify a successful semantic type fact used by inferred declarations. */
static FengLspTypeCategory hover_category_from_type_fact(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengSemanticTypeFact *fact) {
    if (fact == NULL) {
        return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
    }
    switch (fact->kind) {
        case FENG_SEMANTIC_TYPE_FACT_BUILTIN:
            return FENG_LSP_TYPE_CATEGORY_BUILTIN;
        case FENG_SEMANTIC_TYPE_FACT_TYPE_REF:
            return hover_category_from_type_ref(session, program, fact->type_ref);
        case FENG_SEMANTIC_TYPE_FACT_DECL:
            return hover_category_from_decl(fact->type_decl);
        case FENG_SEMANTIC_TYPE_FACT_UNKNOWN:
            break;
    }
    return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
}

/* Append a comma-separated list of AST type references after `prefix`. */
static bool append_type_ref_list(FengLspString *buffer,
                                 const char *prefix,
                                 FengTypeRef *const *types,
                                 size_t count,
                                 const char *separator) {
    size_t index;

    if (count == 0U) {
        return true;
    }
    if (!string_append_cstr(buffer, prefix)) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if ((index > 0U && !string_append_cstr(buffer, separator)) ||
            !hover_type_ref_to_string(buffer, types[index])) {
            return false;
        }
    }
    return true;
}

/* Format the core declaration shape used only by AST-backed Hover. */
static bool decl_hover_signature_to_string(FengLspString *buffer,
                                           const FengLspAnalysisSession *session,
                                           const FengDecl *decl) {
    size_t index;

    if (decl == NULL) {
        return false;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        if (!string_append_cstr(buffer, "type ") ||
            !string_append_bytes(buffer,
                                 decl->as.type_decl.name.data,
                                 decl->as.type_decl.name.length) ||
            !append_decl_type_params_with_style(buffer,
                                                decl->as.type_decl.type_params,
                                                decl->as.type_decl.type_param_count,
                                                FENG_LSP_TYPE_NAME_SHORT)) {
            return false;
        }
        if (decl->as.type_decl.is_tuple) {
            if (!string_append_cstr(buffer, "(")) {
                return false;
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];

                if (member == NULL || member->kind != FENG_TYPE_MEMBER_FIELD ||
                    (index > 0U && !string_append_cstr(buffer, ", ")) ||
                    !hover_type_ref_to_string(buffer, member->as.field.type)) {
                    return false;
                }
            }
            if (!string_append_cstr(buffer, ")")) {
                return false;
            }
        }
        if (!append_type_ref_list(buffer,
                                  ": ",
                                  decl->as.type_decl.declared_specs,
                                  decl->as.type_decl.declared_spec_count,
                                  ", ")) {
            return false;
        }
        if (decl->as.type_decl.is_tuple) {
            return string_append_cstr(buffer, ";");
        }
        return string_append_cstr(buffer,
                                  decl->as.type_decl.member_count > 0U
                                      ? " {...}"
                                      : " {}");
    }
    if (decl->kind == FENG_DECL_SPEC) {
        if (!string_append_cstr(buffer, "spec ") ||
            !string_append_bytes(buffer,
                                 decl->as.spec_decl.name.data,
                                 decl->as.spec_decl.name.length) ||
            !append_decl_type_params_with_style(buffer,
                                                decl->as.spec_decl.type_params,
                                                decl->as.spec_decl.type_param_count,
                                                FENG_LSP_TYPE_NAME_SHORT)) {
            return false;
        }
        switch (decl->as.spec_decl.form) {
            case FENG_SPEC_FORM_OBJECT:
                return append_type_ref_list(buffer,
                                            ": ",
                                            decl->as.spec_decl.parent_specs,
                                            decl->as.spec_decl.parent_spec_count,
                                            ", ") &&
                       string_append_cstr(buffer,
                                          decl->as.spec_decl.as.object.member_count > 0U
                                              ? " {...}"
                                              : " {}");
            case FENG_SPEC_FORM_CALLABLE:
                if (!string_append_cstr(buffer, "(")) {
                    return false;
                }
                for (index = 0U;
                     index < decl->as.spec_decl.as.callable.param_count;
                     ++index) {
                    const FengParameter *param =
                        &decl->as.spec_decl.as.callable.params[index];

                    if ((index > 0U && !string_append_cstr(buffer, ", ")) ||
                        !string_append_bytes(buffer, param->name.data, param->name.length) ||
                        !string_append_cstr(buffer, ": ") ||
                        !parameter_type_to_string_with_style(
                            buffer,
                            param,
                            FENG_LSP_TYPE_NAME_SHORT)) {
                        return false;
                    }
                }
                return string_append_cstr(buffer, "): ") &&
                       hover_type_ref_to_string(
                           buffer,
                           decl->as.spec_decl.as.callable.return_type) &&
                       string_append_cstr(buffer, ";");
            case FENG_SPEC_FORM_UNION:
                return append_type_ref_list(buffer,
                                            ": ",
                                            decl->as.spec_decl.as.union_form.members,
                                            decl->as.spec_decl.as.union_form.member_count,
                                            " | ") &&
                       string_append_cstr(buffer, ";");
            case FENG_SPEC_FORM_INTERSECTION:
                return append_type_ref_list(buffer,
                                            ": ",
                                            decl->as.spec_decl.as.intersection_form.members,
                                            decl->as.spec_decl.as.intersection_form.member_count,
                                            " & ") &&
                       string_append_cstr(buffer, ";");
        }
    }
    return decl_signature_to_string_with_session_and_style(
        buffer,
        session,
        decl,
        FENG_LSP_TYPE_NAME_SHORT);
}

static bool decl_signature_to_string(FengLspString *buffer, const FengDecl *decl) {
    return decl_signature_to_string_with_session(buffer, NULL, decl);
}

static bool decl_signature_to_string_with_session_and_style(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengDecl *decl,
    FengLspTypeNameStyle style) {
    size_t index;

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return binding_signature_to_string_with_style(buffer,
                                                          session,
                                                          &decl->as.binding,
                                                          style);
        case FENG_DECL_ENUM:
            return string_append_cstr(buffer, "enum ") &&
                   string_append_bytes(buffer,
                                       decl->as.enum_decl.name.data,
                                       decl->as.enum_decl.name.length);
        case FENG_DECL_TYPE:
            return string_append_cstr(buffer, "type ") &&
                   string_append_bytes(buffer, decl->as.type_decl.name.data, decl->as.type_decl.name.length) &&
                   append_decl_type_params_with_style(buffer,
                                                      decl->as.type_decl.type_params,
                                                      decl->as.type_decl.type_param_count,
                                                      style);
        case FENG_DECL_SPEC:
            return string_append_cstr(buffer, "spec ") &&
                   string_append_bytes(buffer, decl->as.spec_decl.name.data, decl->as.spec_decl.name.length) &&
                   append_decl_type_params_with_style(buffer,
                                                      decl->as.spec_decl.type_params,
                                                      decl->as.spec_decl.type_param_count,
                                                      style);
        case FENG_DECL_FIT:
            return string_append_cstr(buffer, "fit");
        case FENG_DECL_FUNCTION:
            if (!string_append_cstr(buffer, "func ") ||
                !string_append_bytes(buffer,
                                     decl->as.function_decl.name.data,
                                     decl->as.function_decl.name.length) ||
                !append_decl_type_params_with_style(
                    buffer,
                    decl->as.function_decl.type_params,
                    decl->as.function_decl.type_param_count,
                    style) ||
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
                    !parameter_type_to_string_with_style(
                        buffer,
                        &decl->as.function_decl.params[index],
                        style)) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   type_ref_to_string_with_style(buffer,
                                                 decl->as.function_decl.return_type,
                                                 style);
    }
    return false;
}

static bool decl_signature_to_string_with_session(FengLspString *buffer,
                                                  const FengLspAnalysisSession *session,
                                                  const FengDecl *decl) {
    return decl_signature_to_string_with_session_and_style(
        buffer,
        session,
        decl,
        FENG_LSP_TYPE_NAME_QUALIFIED);
}

static bool member_signature_to_string(FengLspString *buffer, const FengTypeMember *member) {
    return member_signature_to_string_with_session(buffer, NULL, member);
}

static bool member_signature_to_string_with_session_and_style(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengTypeMember *member,
    FengLspTypeNameStyle style) {
    size_t index;

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return string_append_cstr(buffer,
                                  member->as.field.mutability == FENG_MUTABILITY_VAR ? "var " : "let ") &&
               string_append_bytes(buffer,
                                   member->as.field.name.data,
                                   member->as.field.name.length) &&
               append_optional_static_type_annotation_with_style(buffer,
                                                                 session,
                                                                 member,
                                                                 member->as.field.type,
                                                                 style);
    }
    if (!string_append_cstr(buffer,
                            member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR ? "ctor " :
                            member->kind == FENG_TYPE_MEMBER_FINALIZER ? "finalizer " : "func ") ||
        !string_append_bytes(buffer,
                             member->as.callable.name.data,
                             member->as.callable.name.length) ||
        !append_decl_type_params_with_style(buffer,
                                           member->as.callable.type_params,
                                           member->as.callable.type_param_count,
                                           style) ||
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
            !parameter_type_to_string_with_style(buffer,
                                                 &member->as.callable.params[index],
                                                 style)) {
            return false;
        }
    }
    return string_append_cstr(buffer, "): ") &&
           type_ref_to_string_with_style(buffer,
                                         member->as.callable.return_type,
                                         style);
}

static bool member_signature_to_string_with_session(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengTypeMember *member) {
    return member_signature_to_string_with_session_and_style(
        buffer,
        session,
        member,
        FENG_LSP_TYPE_NAME_QUALIFIED);
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

/* Release all storage owned by a structured Hover presentation. */
static void hover_presentation_dispose(FengLspHoverPresentation *presentation) {
    if (presentation == NULL) {
        return;
    }
    string_dispose(&presentation->signature);
    free(presentation->documentation);
    memset(presentation, 0, sizeof(*presentation));
}

/* Attach a category caption and label when the category is proven. */
static void hover_presentation_set_category(FengLspHoverPresentation *presentation,
                                            const char *caption,
                                            FengLspTypeCategory category) {
    const char *label = hover_type_category_label(category);

    if (presentation == NULL || caption == NULL || label == NULL) {
        return;
    }
    presentation->category_caption = caption;
    presentation->category_label = label;
}

/* Render structured Hover content as plaintext. */
static bool render_hover_plaintext(FengLspString *out,
                                   const FengLspHoverPresentation *presentation) {
    if (out == NULL || presentation == NULL || presentation->signature.data == NULL) {
        return false;
    }
    if (!string_append_cstr(out, presentation->signature.data)) {
        return false;
    }
    if (presentation->category_caption != NULL && presentation->category_label != NULL &&
        (!string_append_cstr(out, "\n\n") ||
         !string_append_cstr(out, presentation->category_caption) ||
         !string_append_cstr(out, ": ") ||
         !string_append_cstr(out, presentation->category_label))) {
        return false;
    }
    return presentation->documentation == NULL || presentation->documentation[0] == '\0' ||
           (string_append_cstr(out, "\n\n") &&
            string_append_cstr(out, presentation->documentation));
}

/* Render structured Hover content as Markdown without reparsing plaintext. */
static bool render_hover_markdown(FengLspString *out,
                                  const FengLspHoverPresentation *presentation) {
    if (out == NULL || presentation == NULL || presentation->signature.data == NULL) {
        return false;
    }
    if (!string_append_cstr(out, "```feng\n") ||
        !string_append_cstr(out, presentation->signature.data) ||
        !string_append_cstr(out, "\n```")) {
        return false;
    }
    if (presentation->category_caption != NULL && presentation->category_label != NULL &&
        (!string_append_cstr(out, "\n\n**") ||
         !string_append_cstr(out, presentation->category_caption) ||
         !string_append_cstr(out, ":** `") ||
         !string_append_cstr(out, presentation->category_label) ||
         !string_append_cstr(out, "`"))) {
        return false;
    }
    return presentation->documentation == NULL || presentation->documentation[0] == '\0' ||
           (string_append_cstr(out, "\n\n") &&
            append_hover_doc_markdown(out, presentation->documentation));
}

/* Build an LSP Hover result from the shared structured presentation. */
static bool build_hover_result_json(FengLspString *result,
                                    FengLspMarkupKind markup_kind,
                                    const FengLspHoverPresentation *presentation) {
    FengLspString contents = {0};
    const char *kind = markup_kind == FENG_LSP_MARKUP_MARKDOWN ? "markdown" : "plaintext";
    bool ok;

    if (result == NULL || presentation == NULL) {
        return false;
    }
    if ((markup_kind == FENG_LSP_MARKUP_MARKDOWN &&
         !render_hover_markdown(&contents, presentation)) ||
        (markup_kind == FENG_LSP_MARKUP_PLAINTEXT &&
         !render_hover_plaintext(&contents, presentation))) {
        string_dispose(&contents);
        return false;
    }
    ok = string_append_cstr(result, "{\"contents\":{\"kind\":") &&
         string_append_json_string(result, kind) &&
         string_append_cstr(result, ",\"value\":") &&
         string_append_json_string(result, contents.data) &&
         string_append_cstr(result, "}}");
    string_dispose(&contents);
    return ok;
}

/* Format an AST enum item with its deterministic integer value. */
static bool enum_item_hover_signature_to_string(
    FengLspString *buffer,
    const FengLspAnalysisSession *session,
    const FengDecl *enum_decl,
    const FengEnumItem *item) {
    const FengSemanticEnumItemInfo *info = NULL;
    size_t index;

    if (item == NULL ||
        !string_append_bytes(buffer, item->name.data, item->name.length)) {
        return false;
    }
    if (session != NULL && session->analysis != NULL) {
        info = feng_semantic_find_enum_item_info(session->analysis,
                                                 enum_decl,
                                                 item->name);
    }
    if (info != NULL) {
        return string_append_format(buffer, " = %lld", (long long)info->value);
    }
    if (item->has_explicit_value) {
        return string_append_format(buffer,
                                    " = %lld",
                                    (long long)item->explicit_value);
    }
    if (enum_decl != NULL && enum_decl->kind == FENG_DECL_ENUM) {
        for (index = 0U; index < enum_decl->as.enum_decl.item_count; ++index) {
            if (&enum_decl->as.enum_decl.items[index] == item) {
                return string_append_format(buffer,
                                            " = %lld",
                                            (long long)index);
            }
        }
    }
    return true;
}

/* Build structured Hover content from an AST-backed resolved target. */
static bool hover_presentation_for_target(const FengLspAnalysisSession *session,
                                          const FengProgram *program,
                                          const FengLspResolvedTarget *target,
                                          FengLspHoverPresentation *presentation) {
    FengLspTypeCategory category = FENG_LSP_TYPE_CATEGORY_UNKNOWN;

    if (target == NULL || presentation == NULL) {
        return false;
    }
    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            if (!decl_hover_signature_to_string(&presentation->signature,
                                                session,
                                                target->decl)) {
                return false;
            }
            presentation->documentation = normalize_doc_comment(target->decl->doc_comment);
            if (target->decl->kind == FENG_DECL_TYPE ||
                target->decl->kind == FENG_DECL_ENUM ||
                target->decl->kind == FENG_DECL_SPEC) {
                category = hover_category_from_decl(target->decl);
                hover_presentation_set_category(presentation, "Kind", category);
            } else if (target->decl->kind == FENG_DECL_GLOBAL_BINDING) {
                category = target->decl->as.binding.type != NULL
                               ? hover_category_from_type_ref(session,
                                                              program,
                                                              target->decl->as.binding.type)
                               : hover_category_from_type_fact(
                                     session,
                                     program,
                                     session != NULL && session->analysis != NULL
                                         ? feng_semantic_lookup_type_fact(
                                               session->analysis,
                                               &target->decl->as.binding)
                                         : NULL);
                hover_presentation_set_category(presentation, "Kind", category);
            }
            break;
        case FENG_LSP_RESOLVED_ENUM_ITEM:
            if (!enum_item_hover_signature_to_string(&presentation->signature,
                                                      session,
                                                      target->decl,
                                                      target->enum_item)) {
                return false;
            }
            hover_presentation_set_category(presentation,
                                            "Kind",
                                            FENG_LSP_TYPE_CATEGORY_ENUM);
            break;
        case FENG_LSP_RESOLVED_MEMBER:
            if (!member_signature_to_string_with_session_and_style(
                    &presentation->signature,
                    session,
                    target->member,
                    FENG_LSP_TYPE_NAME_SHORT)) {
                return false;
            }
            presentation->documentation = normalize_doc_comment(target->member->doc_comment);
            if (target->member->kind == FENG_TYPE_MEMBER_FIELD) {
                category = target->member->as.field.type != NULL
                               ? hover_category_from_type_ref(session,
                                                              program,
                                                              target->member->as.field.type)
                               : hover_category_from_type_fact(
                                     session,
                                     program,
                                     session != NULL && session->analysis != NULL
                                         ? feng_semantic_lookup_type_fact(session->analysis,
                                                                          target->member)
                                         : NULL);
                hover_presentation_set_category(presentation, "Kind", category);
            } else if (target->member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR) {
                hover_presentation_set_category(
                    presentation,
                    "Kind",
                    hover_category_from_decl(target->decl));
            }
            break;
        case FENG_LSP_RESOLVED_PARAM:
            if (!string_append_cstr(&presentation->signature, "param ") ||
                !string_append_cstr(&presentation->signature,
                                    target->parameter->mutability == FENG_MUTABILITY_VAR
                                        ? "var "
                                        : "let ") ||
                !string_append_bytes(&presentation->signature,
                                     target->parameter->name.data,
                                     target->parameter->name.length) ||
                !string_append_cstr(&presentation->signature, ": ") ||
                !hover_type_ref_to_string(&presentation->signature,
                                          target->parameter->type)) {
                return false;
            }
            hover_presentation_set_category(
                presentation,
                "Kind",
                hover_category_from_type_ref(session, program, target->parameter->type));
            break;
        case FENG_LSP_RESOLVED_BINDING:
            if (!binding_signature_to_string_with_style(
                    &presentation->signature,
                    session,
                    target->binding,
                    FENG_LSP_TYPE_NAME_SHORT)) {
                return false;
            }
            category = target->binding->type != NULL
                           ? hover_category_from_type_ref(session,
                                                          program,
                                                          target->binding->type)
                           : hover_category_from_type_fact(
                                 session,
                                 program,
                                 session != NULL && session->analysis != NULL
                                     ? feng_semantic_lookup_type_fact(session->analysis,
                                                                      target->binding)
                                     : NULL);
            hover_presentation_set_category(presentation, "Kind", category);
            break;
        case FENG_LSP_RESOLVED_MATCH_BINDING:
            if (!match_binding_signature_to_string_with_style(
                    &presentation->signature,
                    target->match_op,
                    FENG_LSP_TYPE_NAME_SHORT)) {
                return false;
            }
            if (target->match_op->as.match_op.label_count == 1U) {
                const FengMatchLabel *label = &target->match_op->as.match_op.labels[0];
                const FengTypeRef *type_ref = label->type_chain_count > 0U
                                                  ? label->type_chain[
                                                        label->type_chain_count - 1U]
                                                  : label->type;

                hover_presentation_set_category(
                    presentation,
                    "Kind",
                    hover_category_from_type_ref(session, program, type_ref));
            }
            break;
        case FENG_LSP_RESOLVED_SELF:
            if (!string_append_cstr(&presentation->signature, "self: ") ||
                !string_append_bytes(&presentation->signature,
                                     decl_name(target->self_owner_decl).data,
                                     decl_name(target->self_owner_decl).length)) {
                return false;
            }
            hover_presentation_set_category(presentation,
                                            "Kind",
                                            hover_category_from_decl(target->self_owner_decl));
            break;
        case FENG_LSP_RESOLVED_TYPE_PARAM:
            if (!string_append_cstr(&presentation->signature, "generic parameter ") ||
                !string_append_bytes(&presentation->signature,
                                     target->type_param->name.data,
                                     target->type_param->name.length)) {
                return false;
            }
            if (target->type_param->constraint != NULL) {
                if (!string_append_cstr(&presentation->signature, ": ") ||
                    !hover_type_ref_to_string(&presentation->signature,
                                              target->type_param->constraint)) {
                    return false;
                }
            }
            break;
        default:
            return false;
    }
    return presentation->signature.data != NULL;
}

/* Format a persistent symbol type without changing its stored qualified name. */
static bool symbol_type_to_string_with_style(FengLspString *buffer,
                                             const FengSymbolTypeView *type,
                                             FengLspTypeNameStyle style) {
    size_t index;

    if (type == NULL) {
        return string_append_cstr(buffer, "void");
    }
    switch (feng_symbol_type_kind(type)) {
        case FENG_SYMBOL_TYPE_KIND_BUILTIN: {
            FengSlice name = feng_symbol_type_builtin_name(type);
            return string_append_bytes(buffer, name.data, name.length);
        }
        case FENG_SYMBOL_TYPE_KIND_NAMED: {
            size_t segment_count = feng_symbol_type_segment_count(type);
            size_t segment_start = style == FENG_LSP_TYPE_NAME_SHORT && segment_count > 0U
                                       ? segment_count - 1U
                                       : 0U;

            for (index = segment_start; index < segment_count; ++index) {
                FengSlice segment = feng_symbol_type_segment_at(type, index);

                if (index > segment_start && !string_append_cstr(buffer, ".")) {
                    return false;
                }
                if (!string_append_bytes(buffer, segment.data, segment.length)) {
                    return false;
                }
            }
            return true;
        }
        case FENG_SYMBOL_TYPE_KIND_POINTER:
            return symbol_type_to_string_with_style(buffer,
                                                    feng_symbol_type_inner(type),
                                                    style) &&
                   string_append_cstr(buffer, "*");
        case FENG_SYMBOL_TYPE_KIND_ARRAY:
            if (!symbol_type_to_string_with_style(buffer,
                                                  feng_symbol_type_inner(type),
                                                  style)) {
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
            size_t segment_count = feng_symbol_type_segment_count(type);
            size_t segment_start = style == FENG_LSP_TYPE_NAME_SHORT && segment_count > 0U
                                       ? segment_count - 1U
                                       : 0U;

            for (index = segment_start; index < segment_count; ++index) {
                FengSlice segment = feng_symbol_type_segment_at(type, index);

                if (index > segment_start && !string_append_cstr(buffer, ".")) {
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
                    if (!symbol_type_to_string_with_style(
                            buffer,
                            feng_symbol_type_generic_arg_at(type, index),
                            style)) {
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

/* Keep qualified symbol names for non-Hover LSP features. */
static bool symbol_type_to_string(FengLspString *buffer,
                                  const FengSymbolTypeView *type) {
    return symbol_type_to_string_with_style(buffer,
                                            type,
                                            FENG_LSP_TYPE_NAME_QUALIFIED);
}

/* Hover presents the final path segment for every nested named symbol type. */
static bool symbol_hover_type_to_string(FengLspString *buffer,
                                        const FengSymbolTypeView *type) {
    return symbol_type_to_string_with_style(buffer,
                                            type,
                                            FENG_LSP_TYPE_NAME_SHORT);
}

static bool symbol_param_type_to_string_with_style(
    FengLspString *buffer,
    const FengSymbolDeclView *decl,
    size_t param_index,
    FengLspTypeNameStyle style) {
    const FengSymbolTypeView *type = feng_symbol_decl_param_type(decl, param_index);

    if (feng_symbol_decl_param_is_variadic(decl, param_index)) {
        if (type != NULL && feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_ARRAY) {
            size_t rank = feng_symbol_type_array_rank(type);
            size_t layer_index;

            if (rank > 0U) {
                if (!symbol_type_to_string_with_style(buffer,
                                                      feng_symbol_type_inner(type),
                                                      style)) {
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
        return symbol_type_to_string_with_style(buffer, type, style) &&
               string_append_cstr(buffer, "...");
    }
    return symbol_type_to_string_with_style(buffer, type, style);
}

static bool symbol_param_type_to_string(FengLspString *buffer,
                                        const FengSymbolDeclView *decl,
                                        size_t param_index) {
    return symbol_param_type_to_string_with_style(buffer,
                                                  decl,
                                                  param_index,
                                                  FENG_LSP_TYPE_NAME_QUALIFIED);
}

static bool symbol_decl_signature_to_string(FengLspString *buffer,
                                            const FengSymbolDeclView *decl);
static bool symbol_decl_signature_to_string_with_style(
    FengLspString *buffer,
    const FengSymbolDeclView *decl,
    FengLspTypeNameStyle style);

/* Classify the outermost persistent symbol type without loading new data. */
static FengLspTypeCategory hover_category_from_symbol_type(
    const FengLspCacheQueryContext *context,
    const FengSymbolTypeView *type) {
    FengSymbolTypeKind kind;

    if (context == NULL || type == NULL) {
        return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
    }
    kind = feng_symbol_type_kind(type);
    if (kind == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
        return FENG_LSP_TYPE_CATEGORY_BUILTIN;
    }
    if (kind == FENG_SYMBOL_TYPE_KIND_ARRAY) {
        return FENG_LSP_TYPE_CATEGORY_ARRAY;
    }
    if (kind == FENG_SYMBOL_TYPE_KIND_POINTER) {
        return FENG_LSP_TYPE_CATEGORY_POINTER;
    }
    if (kind == FENG_SYMBOL_TYPE_KIND_NAMED ||
        kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC) {
        return hover_category_from_symbol_decl(
            resolve_symbol_type_view(context->provider,
                                     context->current_module,
                                     context->program,
                                     type));
    }
    return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
}

/* Append symbol-backed generic parameters, including proven constraints. */
static bool append_symbol_decl_type_params(FengLspString *buffer,
                                           const FengSymbolDeclView *decl) {
    size_t member_index;
    size_t parameter_index = 0U;

    if (feng_symbol_decl_type_param_count(decl) == 0U) {
        return true;
    }
    if (!string_append_cstr(buffer, "<")) {
        return false;
    }
    for (member_index = 0U;
         member_index < feng_symbol_decl_member_count(decl);
         ++member_index) {
        const FengSymbolDeclView *member = feng_symbol_decl_member_at(decl, member_index);
        const FengSymbolTypeView *constraint;
        FengSlice name;

        if (member == NULL ||
            feng_symbol_decl_kind(member) != FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
            continue;
        }
        if (parameter_index > 0U && !string_append_cstr(buffer, ", ")) {
            return false;
        }
        name = feng_symbol_decl_name(member);
        if (!string_append_bytes(buffer, name.data, name.length)) {
            return false;
        }
        constraint = feng_symbol_decl_value_type(member);
        if (constraint != NULL &&
            (!string_append_cstr(buffer, ": ") ||
             !symbol_hover_type_to_string(buffer, constraint))) {
            return false;
        }
        ++parameter_index;
    }
    return string_append_cstr(buffer, ">");
}

/* Return whether a symbol type/spec declaration has a body member. */
static bool symbol_decl_has_body_member(const FengSymbolDeclView *decl) {
    size_t index;

    for (index = 0U; index < feng_symbol_decl_member_count(decl); ++index) {
        const FengSymbolDeclView *member = feng_symbol_decl_member_at(decl, index);
        FengSymbolDeclKind kind;

        if (member == NULL) {
            continue;
        }
        kind = feng_symbol_decl_kind(member);
        if (kind == FENG_SYMBOL_DECL_KIND_FIELD ||
            kind == FENG_SYMBOL_DECL_KIND_METHOD ||
            kind == FENG_SYMBOL_DECL_KIND_CONSTRUCTOR ||
            kind == FENG_SYMBOL_DECL_KIND_FINALIZER) {
            return true;
        }
    }
    return false;
}

/* Append symbol-backed declared specs with a caller-selected separator. */
static bool append_symbol_declared_specs(FengLspString *buffer,
                                         const FengSymbolDeclView *decl,
                                         const char *separator) {
    size_t index;
    size_t count = feng_symbol_decl_declared_spec_count(decl);

    if (count == 0U) {
        return true;
    }
    if (!string_append_cstr(buffer, ": ")) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if ((index > 0U && !string_append_cstr(buffer, separator)) ||
            !symbol_hover_type_to_string(
                buffer,
                feng_symbol_decl_declared_spec_at(decl, index))) {
            return false;
        }
    }
    return true;
}

/* Format the core declaration shape used only by symbol-backed Hover. */
static bool symbol_decl_hover_signature_to_string(FengLspString *buffer,
                                                  const FengSymbolDeclView *decl) {
    FengSymbolDeclKind kind;
    FengSlice name;
    size_t index;

    if (decl == NULL) {
        return false;
    }
    kind = feng_symbol_decl_kind(decl);
    name = feng_symbol_decl_name(decl);
    if (kind == FENG_SYMBOL_DECL_KIND_TYPE) {
        size_t tuple_item_count = 0U;

        if (!string_append_cstr(buffer, "type ") ||
            !string_append_bytes(buffer, name.data, name.length) ||
            !append_symbol_decl_type_params(buffer, decl)) {
            return false;
        }
        if (feng_symbol_decl_is_tuple(decl)) {
            if (!string_append_cstr(buffer, "(")) {
                return false;
            }
            for (index = 0U; index < feng_symbol_decl_member_count(decl); ++index) {
                const FengSymbolDeclView *member = feng_symbol_decl_member_at(decl, index);

                if (member == NULL ||
                    feng_symbol_decl_kind(member) != FENG_SYMBOL_DECL_KIND_FIELD) {
                    continue;
                }
                if ((tuple_item_count > 0U && !string_append_cstr(buffer, ", ")) ||
                    !symbol_hover_type_to_string(
                        buffer,
                        feng_symbol_decl_value_type(member))) {
                    return false;
                }
                ++tuple_item_count;
            }
            if (!string_append_cstr(buffer, ")")) {
                return false;
            }
        }
        if (!append_symbol_declared_specs(buffer, decl, ", ")) {
            return false;
        }
        if (feng_symbol_decl_is_tuple(decl)) {
            return string_append_cstr(buffer, ";");
        }
        return string_append_cstr(buffer,
                                  symbol_decl_has_body_member(decl)
                                      ? " {...}"
                                      : " {}");
    }
    if (kind == FENG_SYMBOL_DECL_KIND_SPEC) {
        FengSpecForm form = feng_symbol_decl_spec_form(decl);

        if (!string_append_cstr(buffer, "spec ") ||
            !string_append_bytes(buffer, name.data, name.length) ||
            !append_symbol_decl_type_params(buffer, decl)) {
            return false;
        }
        if (form == FENG_SPEC_FORM_OBJECT) {
            return append_symbol_declared_specs(buffer, decl, ", ") &&
                   string_append_cstr(buffer,
                                      symbol_decl_has_body_member(decl)
                                          ? " {...}"
                                          : " {}");
        }
        if (form == FENG_SPEC_FORM_CALLABLE) {
            if (!string_append_cstr(buffer, "(")) {
                return false;
            }
            for (index = 0U; index < feng_symbol_decl_param_count(decl); ++index) {
                FengSlice param_name = feng_symbol_decl_param_name(decl, index);

                if ((index > 0U && !string_append_cstr(buffer, ", ")) ||
                    !string_append_bytes(buffer, param_name.data, param_name.length) ||
                    !string_append_cstr(buffer, ": ") ||
                    !symbol_param_type_to_string_with_style(
                        buffer,
                        decl,
                        index,
                        FENG_LSP_TYPE_NAME_SHORT)) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   symbol_hover_type_to_string(
                       buffer,
                       feng_symbol_decl_return_type(decl)) &&
                   string_append_cstr(buffer, ";");
        }
        if (!string_append_cstr(buffer, ": ")) {
            return false;
        }
        if (form == FENG_SPEC_FORM_UNION) {
            for (index = 0U; index < feng_symbol_decl_union_member_count(decl); ++index) {
                if ((index > 0U && !string_append_cstr(buffer, " | ")) ||
                    !symbol_hover_type_to_string(
                        buffer,
                        feng_symbol_decl_union_member_at(decl, index))) {
                    return false;
                }
            }
        } else if (form == FENG_SPEC_FORM_INTERSECTION) {
            for (index = 0U;
                 index < feng_symbol_decl_intersection_member_count(decl);
                 ++index) {
                if ((index > 0U && !string_append_cstr(buffer, " & ")) ||
                    !symbol_hover_type_to_string(
                        buffer,
                        feng_symbol_decl_intersection_member_at(decl, index))) {
                    return false;
                }
            }
        } else {
            return false;
        }
        return string_append_cstr(buffer, ";");
    }
    return symbol_decl_signature_to_string_with_style(
        buffer,
        decl,
        FENG_LSP_TYPE_NAME_SHORT);
}

static bool symbol_decl_signature_to_string_with_style(
    FengLspString *buffer,
    const FengSymbolDeclView *decl,
    FengLspTypeNameStyle style) {
    size_t index;
    FengSlice name = feng_symbol_decl_name(decl);

    switch (feng_symbol_decl_kind(decl)) {
        case FENG_SYMBOL_DECL_KIND_BINDING:
            return string_append_cstr(buffer,
                                      feng_symbol_decl_mutability(decl) == FENG_MUTABILITY_VAR ? "var " : "let ") &&
                   string_append_bytes(buffer, name.data, name.length) &&
                   string_append_cstr(buffer, ": ") &&
                   symbol_type_to_string_with_style(buffer,
                                                    feng_symbol_decl_value_type(decl),
                                                    style);
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
                    !symbol_param_type_to_string_with_style(buffer,
                                                            decl,
                                                            index,
                                                            style)) {
                    return false;
                }
            }
            return string_append_cstr(buffer, "): ") &&
                   symbol_type_to_string_with_style(buffer,
                                                    feng_symbol_decl_return_type(decl),
                                                    style);
        }
        case FENG_SYMBOL_DECL_KIND_MODULE:
        case FENG_SYMBOL_DECL_KIND_FIELD:
        case FENG_SYMBOL_DECL_KIND_METHOD:
        case FENG_SYMBOL_DECL_KIND_CONSTRUCTOR:
        case FENG_SYMBOL_DECL_KIND_FINALIZER:
        case FENG_SYMBOL_DECL_KIND_ENUM_ITEM:
            break;
        case FENG_SYMBOL_DECL_KIND_TYPE_PARAM:
            return string_append_cstr(buffer, "generic parameter ") &&
                   string_append_bytes(buffer, name.data, name.length);
    }
    return false;
}

static bool symbol_decl_signature_to_string(FengLspString *buffer,
                                            const FengSymbolDeclView *decl) {
    return symbol_decl_signature_to_string_with_style(
        buffer,
        decl,
        FENG_LSP_TYPE_NAME_QUALIFIED);
}

static bool symbol_member_signature_to_string_with_style(
    FengLspString *buffer,
    const FengSymbolDeclView *member,
    FengLspTypeNameStyle style) {
    size_t index;
    FengSlice name = feng_symbol_decl_name(member);
    FengSymbolDeclKind kind = feng_symbol_decl_kind(member);

    if (kind == FENG_SYMBOL_DECL_KIND_FIELD) {
        return string_append_cstr(buffer,
                                  feng_symbol_decl_mutability(member) == FENG_MUTABILITY_VAR ? "var " : "let ") &&
               string_append_bytes(buffer, name.data, name.length) &&
               string_append_cstr(buffer, ": ") &&
               symbol_type_to_string_with_style(buffer,
                                                feng_symbol_decl_value_type(member),
                                                style);
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
            !symbol_param_type_to_string_with_style(buffer,
                                                    member,
                                                    index,
                                                    style)) {
            return false;
        }
    }
    return string_append_cstr(buffer, "): ") &&
           symbol_type_to_string_with_style(buffer,
                                            feng_symbol_decl_return_type(member),
                                            style);
}

static bool symbol_member_signature_to_string(
    FengLspString *buffer,
    const FengSymbolDeclView *member) {
    return symbol_member_signature_to_string_with_style(
        buffer,
        member,
        FENG_LSP_TYPE_NAME_QUALIFIED);
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
            case FENG_STMT_DEFER:
                hit = find_call_hit_in_block(stmt->as.defer_block, offset);
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
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                const FengExpr *hit = find_call_hit_expr(
                    decl->as.type_decl.mixins[index].source_constructor,
                    offset);

                if (hit != NULL) {
                    return hit;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];
                const FengExpr *hit = member->mixin_origin != NULL
                                          ? NULL
                                          : member->kind == FENG_TYPE_MEMBER_FIELD
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
                if (local->match_op != NULL) {
                    target->kind = FENG_LSP_RESOLVED_MATCH_BINDING;
                    target->match_op = local->match_op;
                } else {
                    target->kind = FENG_LSP_RESOLVED_BINDING;
                    target->binding = local->binding;
                }
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
            if (target->decl->kind == FENG_DECL_ENUM) {
                target->enum_item = find_enum_item_by_name(target->decl,
                                                           expr->as.member.member);
                if (target->enum_item != NULL) {
                    target->kind = FENG_LSP_RESOLVED_ENUM_ITEM;
                    return target->decl;
                }
            }
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
static bool find_object_field_syntax_hit_decl(const FengDecl *decl,
                                              size_t offset,
                                              const FengExpr **out_construction,
                                              FengSlice *out_name);

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
        case FENG_LSP_RESOLVED_ENUM_ITEM:
            return lhs->enum_item == rhs->enum_item && lhs->decl == rhs->decl;
        case FENG_LSP_RESOLVED_PARAM:
            return lhs->parameter == rhs->parameter;
        case FENG_LSP_RESOLVED_BINDING:
            return lhs->binding == rhs->binding;
        case FENG_LSP_RESOLVED_MATCH_BINDING:
            return lhs->match_op == rhs->match_op;
        case FENG_LSP_RESOLVED_SELF:
            return lhs->self_owner_decl == rhs->self_owner_decl;
        case FENG_LSP_RESOLVED_TYPE_PARAM:
            return lhs->type_param == rhs->type_param;
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
        case FENG_LSP_RESOLVED_ENUM_ITEM:
            return target->decl != NULL &&
                   target->decl->kind == FENG_DECL_ENUM &&
                   target->enum_item != NULL;
        case FENG_LSP_RESOLVED_PARAM:
            return target->parameter != NULL;
        case FENG_LSP_RESOLVED_BINDING:
            return target->binding != NULL;
        case FENG_LSP_RESOLVED_MATCH_BINDING:
            return false;
        case FENG_LSP_RESOLVED_NONE:
        case FENG_LSP_RESOLVED_SELF:
        case FENG_LSP_RESOLVED_TYPE_PARAM:
            return false;
    }
    return false;
}

/* Finds a declaration's source program without requiring semantic analysis. */
static const FengProgram *find_decl_owner_program_in_session(
    const FengLspAnalysisSession *session,
    const FengDecl *decl);

/* Returns whether a declaration is backed by writable workspace source. */
static bool resolved_decl_has_writable_source(const FengLspAnalysisSession *session,
                                              const FengDecl *decl) {
    const FengProgram *owner_program = NULL;

    if (session == NULL || decl == NULL) {
        return false;
    }
    (void)find_decl_module(session, decl, &owner_program);
    if (owner_program == NULL && session->analysis == NULL) {
        owner_program = find_decl_owner_program_in_session(session, decl);
    }
    return owner_program != NULL && find_source(session, owner_program->path) != NULL;
}

static bool resolved_target_can_rename(const FengLspAnalysisSession *session,
                                       const FengLspResolvedTarget *target) {
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
                target->decl->kind != FENG_DECL_ENUM &&
                target->decl->kind != FENG_DECL_FUNCTION) {
                return false;
            }
            return resolved_decl_has_writable_source(session, target->decl);
        case FENG_LSP_RESOLVED_MEMBER:
            if (target->member == NULL) {
                return false;
            }
            if (target->member->kind != FENG_TYPE_MEMBER_FIELD &&
                target->member->kind != FENG_TYPE_MEMBER_METHOD) {
                return false;
            }
            return resolved_decl_has_writable_source(session, target->decl);
        case FENG_LSP_RESOLVED_ENUM_ITEM:
            return target->decl != NULL &&
                   target->decl->kind == FENG_DECL_ENUM &&
                   target->enum_item != NULL &&
                   resolved_decl_has_writable_source(session, target->decl);
        case FENG_LSP_RESOLVED_PARAM:
        case FENG_LSP_RESOLVED_BINDING:
            return true;
        case FENG_LSP_RESOLVED_MATCH_BINDING:
            return false;
        case FENG_LSP_RESOLVED_NONE:
        case FENG_LSP_RESOLVED_SELF:
        case FENG_LSP_RESOLVED_TYPE_PARAM:
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
    if (callee->kind == FENG_EXPR_GENERIC_TARGET) {
        return call_callee_name_slice(callee->as.generic_target.target);
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

/* Forward declaration for object-field syntax traversal through nested blocks. */
static bool find_object_field_syntax_hit_block(const FengBlock *block,
                                               size_t offset,
                                               const FengExpr **out_construction,
                                               FengSlice *out_name);

/* Find an object-literal field token and return its ordinary construction
 * target. Resolution is deliberately separate so AST and persistent-symbol
 * providers can consume the same source shape without mixin-specific rules. */
static bool find_object_field_syntax_hit_expr(const FengExpr *expr,
                                              size_t offset,
                                              const FengExpr **out_construction,
                                              FengSlice *out_name) {
    size_t index;

    if (expr == NULL || offset < expr_start(expr) || offset > expr_end(expr)) {
        return false;
    }
    switch (expr->kind) {
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (find_object_field_syntax_hit_expr(expr->as.array_literal.items[index],
                                                      offset,
                                                      out_construction,
                                                      out_name)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_TUPLE_LITERAL:
            for (index = 0U; index < expr->as.tuple_literal.count; ++index) {
                if (find_object_field_syntax_hit_expr(expr->as.tuple_literal.items[index],
                                                      offset,
                                                      out_construction,
                                                      out_name)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_GENERIC_TARGET:
            return find_object_field_syntax_hit_expr(expr->as.generic_target.target,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_ARRAY_NEW:
            return find_object_field_syntax_hit_expr(expr->as.array_new.size,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_OBJECT_LITERAL:
            if (find_object_field_syntax_hit_expr(expr->as.object_literal.target,
                                                  offset,
                                                  out_construction,
                                                  out_name)) {
                return true;
            }
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                const FengObjectFieldInit *field = &expr->as.object_literal.fields[index];

                if (offset_in_token(field->token, offset)) {
                    *out_construction = expr->as.object_literal.target;
                    *out_name = field->name;
                    return true;
                }
                if (find_object_field_syntax_hit_expr(field->value,
                                                      offset,
                                                      out_construction,
                                                      out_name)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_CALL:
            if (find_object_field_syntax_hit_expr(expr->as.call.callee,
                                                  offset,
                                                  out_construction,
                                                  out_name)) {
                return true;
            }
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                if (find_object_field_syntax_hit_expr(expr->as.call.args[index],
                                                      offset,
                                                      out_construction,
                                                      out_name)) {
                    return true;
                }
            }
            return false;
        case FENG_EXPR_MEMBER:
            return find_object_field_syntax_hit_expr(expr->as.member.object,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_INDEX:
            return find_object_field_syntax_hit_expr(expr->as.index.object,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_expr(expr->as.index.index,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_UNARY:
            return find_object_field_syntax_hit_expr(expr->as.unary.operand,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_BINARY:
            return find_object_field_syntax_hit_expr(expr->as.binary.left,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_expr(expr->as.binary.right,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_LAMBDA:
            return expr->as.lambda.is_block_body
                       ? find_object_field_syntax_hit_block(expr->as.lambda.body_block,
                                                            offset,
                                                            out_construction,
                                                            out_name)
                       : find_object_field_syntax_hit_expr(expr->as.lambda.body,
                                                           offset,
                                                           out_construction,
                                                           out_name);
        case FENG_EXPR_CAST:
            return find_object_field_syntax_hit_expr(expr->as.cast.value,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_IF:
            return find_object_field_syntax_hit_expr(expr->as.if_expr.condition,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_block(expr->as.if_expr.then_block,
                                                      offset,
                                                      out_construction,
                                                      out_name) ||
                   find_object_field_syntax_hit_block(expr->as.if_expr.else_block,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_EXPR_MATCH:
            if (find_object_field_syntax_hit_expr(expr->as.match_expr.target,
                                                  offset,
                                                  out_construction,
                                                  out_name)) {
                return true;
            }
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                if (find_object_field_syntax_hit_block(
                        expr->as.match_expr.branches[index].body,
                        offset,
                        out_construction,
                        out_name)) {
                    return true;
                }
            }
            return find_object_field_syntax_hit_block(expr->as.match_expr.else_block,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_EXPR_MATCH_OP:
            return find_object_field_syntax_hit_expr(expr->as.match_op.target,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_EXPR_TRY:
            if (find_object_field_syntax_hit_expr(expr->as.try_expr.body,
                                                  offset,
                                                  out_construction,
                                                  out_name)) {
                return true;
            }
            for (index = 0U; index < expr->as.try_expr.clause_count; ++index) {
                if (find_object_field_syntax_hit_block(
                        expr->as.try_expr.clauses[index].body,
                        offset,
                        out_construction,
                        out_name)) {
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

/* Find an object-field token inside one statement. */
static bool find_object_field_syntax_hit_stmt(const FengStmt *stmt,
                                              size_t offset,
                                              const FengExpr **out_construction,
                                              FengSlice *out_name) {
    size_t index;

    if (stmt == NULL || offset < stmt->token.offset || offset > stmt_end(stmt)) {
        return false;
    }
    switch (stmt->kind) {
        case FENG_STMT_BINDING:
            return find_object_field_syntax_hit_expr(stmt->as.binding.initializer,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_STMT_ASSIGN:
            return find_object_field_syntax_hit_expr(stmt->as.assign.target,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_expr(stmt->as.assign.value,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_STMT_TRY:
        case FENG_STMT_EXPR:
            return find_object_field_syntax_hit_expr(stmt->as.expr,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_STMT_BLOCK:
            return find_object_field_syntax_hit_block(stmt->as.block,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_STMT_DEFER:
            return find_object_field_syntax_hit_block(stmt->as.defer_block,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                if (find_object_field_syntax_hit_expr(
                        stmt->as.if_stmt.clauses[index].condition,
                        offset,
                        out_construction,
                        out_name) ||
                    find_object_field_syntax_hit_block(
                        stmt->as.if_stmt.clauses[index].block,
                        offset,
                        out_construction,
                        out_name)) {
                    return true;
                }
            }
            return find_object_field_syntax_hit_block(stmt->as.if_stmt.else_block,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_STMT_MATCH:
            if (find_object_field_syntax_hit_expr(stmt->as.match_stmt.target,
                                                  offset,
                                                  out_construction,
                                                  out_name)) {
                return true;
            }
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                if (find_object_field_syntax_hit_block(
                        stmt->as.match_stmt.branches[index].body,
                        offset,
                        out_construction,
                        out_name)) {
                    return true;
                }
            }
            return find_object_field_syntax_hit_block(stmt->as.match_stmt.else_block,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_STMT_WHILE:
            return find_object_field_syntax_hit_expr(stmt->as.while_stmt.condition,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_block(stmt->as.while_stmt.body,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                return find_object_field_syntax_hit_expr(stmt->as.for_stmt.iter_expr,
                                                         offset,
                                                         out_construction,
                                                         out_name) ||
                       find_object_field_syntax_hit_block(stmt->as.for_stmt.body,
                                                          offset,
                                                          out_construction,
                                                          out_name);
            }
            return find_object_field_syntax_hit_stmt(stmt->as.for_stmt.init,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_expr(stmt->as.for_stmt.condition,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_stmt(stmt->as.for_stmt.update,
                                                     offset,
                                                     out_construction,
                                                     out_name) ||
                   find_object_field_syntax_hit_block(stmt->as.for_stmt.body,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_STMT_RETURN:
            return find_object_field_syntax_hit_expr(stmt->as.return_value,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_STMT_THROW:
            return find_object_field_syntax_hit_expr(stmt->as.throw_value,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            return false;
    }
    return false;
}

/* Find an object-field token inside one block. */
static bool find_object_field_syntax_hit_block(const FengBlock *block,
                                               size_t offset,
                                               const FengExpr **out_construction,
                                               FengSlice *out_name) {
    size_t index;

    if (block == NULL || offset < block->token.offset || offset > block_end(block)) {
        return false;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        if (find_object_field_syntax_hit_stmt(block->statements[index],
                                             offset,
                                             out_construction,
                                             out_name)) {
            return true;
        }
    }
    return false;
}

/* Find an object-field token in a declaration's original source nodes. */
static bool find_object_field_syntax_hit_decl(const FengDecl *decl,
                                              size_t offset,
                                              const FengExpr **out_construction,
                                              FengSlice *out_name) {
    size_t index;

    if (decl == NULL || offset < decl->token.offset || offset > decl_end(decl)) {
        return false;
    }
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            return find_object_field_syntax_hit_expr(decl->as.binding.initializer,
                                                     offset,
                                                     out_construction,
                                                     out_name);
        case FENG_DECL_ENUM:
            return false;
        case FENG_DECL_FUNCTION:
            return find_object_field_syntax_hit_block(decl->as.function_decl.body,
                                                      offset,
                                                      out_construction,
                                                      out_name);
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                if (find_object_field_syntax_hit_expr(
                        decl->as.type_decl.mixins[index].source_constructor,
                        offset,
                        out_construction,
                        out_name)) {
                    return true;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.type_decl.members[index];

                if (member->mixin_origin != NULL) {
                    continue;
                }
                if ((member->kind == FENG_TYPE_MEMBER_FIELD &&
                     find_object_field_syntax_hit_expr(member->as.field.initializer,
                                                       offset,
                                                       out_construction,
                                                       out_name)) ||
                    (member->kind != FENG_TYPE_MEMBER_FIELD &&
                     find_object_field_syntax_hit_block(member->as.callable.body,
                                                        offset,
                                                        out_construction,
                                                        out_name))) {
                    return true;
                }
            }
            return false;
        case FENG_DECL_SPEC:
            if (decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
                return false;
            }
            for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                const FengTypeMember *member =
                    decl->as.spec_decl.as.object.members[index];

                if ((member->kind == FENG_TYPE_MEMBER_FIELD &&
                     find_object_field_syntax_hit_expr(member->as.field.initializer,
                                                       offset,
                                                       out_construction,
                                                       out_name)) ||
                    (member->kind != FENG_TYPE_MEMBER_FIELD &&
                     find_object_field_syntax_hit_block(member->as.callable.body,
                                                        offset,
                                                        out_construction,
                                                        out_name))) {
                    return true;
                }
            }
            return false;
        case FENG_DECL_FIT:
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                const FengTypeMember *member = decl->as.fit_decl.members[index];

                if ((member->kind == FENG_TYPE_MEMBER_FIELD &&
                     find_object_field_syntax_hit_expr(member->as.field.initializer,
                                                       offset,
                                                       out_construction,
                                                       out_name)) ||
                    (member->kind != FENG_TYPE_MEMBER_FIELD &&
                     find_object_field_syntax_hit_block(member->as.callable.body,
                                                        offset,
                                                        out_construction,
                                                        out_name))) {
                    return true;
                }
            }
            return false;
    }
    return false;
}


/* Resolve an object-literal field through the AST-backed type provider. */
static bool resolve_object_field_target_decl(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengDecl *decl,
                                             size_t offset,
                                             const FengLspLocalList *locals,
                                             FengLspResolvedTarget *target) {
    const FengExpr *construction = NULL;
    const FengDecl *owner;
    FengSlice name = {0};

    if (!find_object_field_syntax_hit_decl(decl,
                                           offset,
                                           &construction,
                                           &name)) {
        return false;
    }
    owner = resolve_owner_decl_from_object_expr(session,
                                                 program,
                                                 construction,
                                                 locals);
    if (owner == NULL) {
        return false;
    }
    target->member = find_member_by_name(owner, name);
    if (target->member == NULL ||
        target->member->kind != FENG_TYPE_MEMBER_FIELD) {
        memset(target, 0, sizeof(*target));
        return false;
    }
    target->kind = FENG_LSP_RESOLVED_MEMBER;
    target->decl = owner;
    return true;
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

/* Collects declaration and type references carried by callable parameters. */
static bool collect_references_in_parameters(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengCliLoadedSource *source,
                                             const FengParameter *params,
                                             size_t param_count,
                                             bool include_declaration,
                                             const FengLspResolvedTarget *target,
                                             FengLspReferenceList *references);

/* Collects references from a contiguous list of match labels. */
static bool collect_references_in_match_labels(const FengLspAnalysisSession *session,
                                               const FengProgram *program,
                                               const FengCliLoadedSource *source,
                                               const FengDecl *owner_decl,
                                               const FengTypeMember *owner_member,
                                               const FengMatchLabel *labels,
                                               size_t label_count,
                                               const FengLspResolvedTarget *target,
                                               FengLspReferenceList *references);

/* Collects labels and bodies shared by statement and expression match forms. */
static bool collect_references_in_match_branches(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengCliLoadedSource *source,
    const FengDecl *owner_decl,
    const FengTypeMember *owner_member,
    const FengMatchBranch *branches,
    size_t branch_count,
    bool include_declaration,
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
            if (!collect_references_in_expr(session,
                                            program,
                                            source,
                                            owner_decl,
                                            owner_member,
                                            expr->as.generic_target.target,
                                            target,
                                            references)) {
                return false;
            }
            for (index = 0U; index < expr->as.generic_target.type_arg_count; ++index) {
                if (!collect_references_in_type_ref(
                        session,
                        program,
                        source,
                        expr->as.generic_target.type_args[index],
                        target,
                        references)) {
                    return false;
                }
            }
            return true;
        case FENG_EXPR_ARRAY_NEW:
            return collect_references_in_type_ref(session,
                                                  program,
                                                  source,
                                                  expr->as.array_new.element_type,
                                                  target,
                                                  references) &&
                   collect_references_in_expr(session,
                                              program,
                                              source,
                                              owner_decl,
                                              owner_member,
                                              expr->as.array_new.size,
                                              target,
                                              references);
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
            for (index = 0U; index < expr->as.call.explicit_type_arg_count; ++index) {
                if (!collect_references_in_type_ref(
                        session,
                        program,
                        source,
                        expr->as.call.explicit_type_args[index],
                        target,
                        references)) {
                    return false;
                }
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
            if (!collect_references_in_parameters(session,
                                                  program,
                                                  source,
                                                  expr->as.lambda.params,
                                                  expr->as.lambda.param_count,
                                                  false,
                                                  target,
                                                  references)) {
                return false;
            }
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
            return collect_references_in_match_branches(
                       session,
                       program,
                       source,
                       owner_decl,
                       owner_member,
                       expr->as.match_expr.branches,
                       expr->as.match_expr.branch_count,
                       false,
                       target,
                       references) &&
                   collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               expr->as.match_expr.else_block,
                                               false,
                                               target,
                                               references);
        case FENG_EXPR_MATCH_OP:
            if (!collect_references_in_expr(session,
                                            program,
                                            source,
                                            owner_decl,
                                            owner_member,
                                            expr->as.match_op.target,
                                            target,
                                            references)) {
                return false;
            }
            return collect_references_in_match_labels(session,
                                                      program,
                                                      source,
                                                      owner_decl,
                                                      owner_member,
                                                      expr->as.match_op.labels,
                                                      expr->as.match_op.label_count,
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
    size_t index;

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
        for (index = 0U; index < type_ref->as.named.type_arg_count; ++index) {
            if (!collect_references_in_type_ref(session,
                                                program,
                                                source,
                                                type_ref->as.named.type_args[index],
                                                target,
                                                references)) {
                return false;
            }
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

/* Collects parameter declarations when requested and always visits their types. */
static bool collect_references_in_parameters(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengCliLoadedSource *source,
                                             const FengParameter *params,
                                             size_t param_count,
                                             bool include_declaration,
                                             const FengLspResolvedTarget *target,
                                             FengLspReferenceList *references) {
    size_t index;

    for (index = 0U; index < param_count; ++index) {
        if (include_declaration && target != NULL &&
            target->kind == FENG_LSP_RESOLVED_PARAM &&
            target->parameter == &params[index] &&
            !reference_list_push_slice(references, source, params[index].name)) {
            return false;
        }
        if (!collect_references_in_type_ref(session,
                                            program,
                                            source,
                                            params[index].type,
                                            target,
                                            references)) {
            return false;
        }
    }
    return true;
}

/* Collects constraint type references declared by generic parameters. */
static bool collect_references_in_type_params(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengCliLoadedSource *source,
                                              const FengTypeParam *type_params,
                                              size_t type_param_count,
                                              const FengLspResolvedTarget *target,
                                              FengLspReferenceList *references) {
    size_t index;

    for (index = 0U; index < type_param_count; ++index) {
        if (!collect_references_in_type_ref(session,
                                            program,
                                            source,
                                            type_params[index].constraint,
                                            target,
                                            references)) {
            return false;
        }
    }
    return true;
}

/* Collects a match type label and its possible enum-owner prefix. */
static bool collect_references_in_match_type_label(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengCliLoadedSource *source,
    const FengTypeRef *type_ref,
    const FengLspResolvedTarget *target,
    FengLspReferenceList *references) {
    if (!collect_references_in_type_ref(session,
                                        program,
                                        source,
                                        type_ref,
                                        target,
                                        references)) {
        return false;
    }
    if (type_ref != NULL && type_ref->kind == FENG_TYPE_REF_NAMED &&
        type_ref->as.named.segment_count > 1U) {
        FengTypeRef owner_ref = *type_ref;
        const FengDecl *owner_decl;
        const FengEnumItem *enum_item;
        FengSlice item_name;
        FengLspResolvedTarget candidate = {0};

        --owner_ref.as.named.segment_count;
        owner_ref.as.named.type_args = NULL;
        owner_ref.as.named.type_arg_count = 0U;
        owner_decl = resolve_named_type_ref(session, program, &owner_ref);
        candidate.kind = FENG_LSP_RESOLVED_DECL;
        candidate.decl = owner_decl;
        if (candidate.decl != NULL &&
            !add_reference_if_match(
                references,
                source,
                owner_ref.as.named.segments[owner_ref.as.named.segment_count - 1U],
                target,
                &candidate)) {
            return false;
        }
        if (owner_decl != NULL && owner_decl->kind == FENG_DECL_ENUM) {
            item_name = type_ref->as.named.segments[type_ref->as.named.segment_count - 1U];
            enum_item = find_enum_item_by_name(owner_decl, item_name);
            if (enum_item != NULL) {
                candidate.kind = FENG_LSP_RESOLVED_ENUM_ITEM;
                candidate.decl = owner_decl;
                candidate.enum_item = enum_item;
                if (!add_reference_if_match(references,
                                            source,
                                            item_name,
                                            target,
                                            &candidate)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Collects references from one match label, including chained type labels. */
static bool collect_references_in_match_label(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengCliLoadedSource *source,
                                              const FengDecl *owner_decl,
                                              const FengTypeMember *owner_member,
                                              const FengMatchLabel *label,
                                              const FengLspResolvedTarget *target,
                                              FengLspReferenceList *references) {
    size_t index;

    if (label == NULL) {
        return true;
    }
    if (label->kind == FENG_MATCH_LABEL_VALUE) {
        return collect_references_in_expr(session,
                                          program,
                                          source,
                                          owner_decl,
                                          owner_member,
                                          label->value,
                                          target,
                                          references);
    }
    if (label->kind == FENG_MATCH_LABEL_RANGE) {
        return collect_references_in_expr(session,
                                          program,
                                          source,
                                          owner_decl,
                                          owner_member,
                                          label->range_low,
                                          target,
                                          references) &&
               collect_references_in_expr(session,
                                          program,
                                          source,
                                          owner_decl,
                                          owner_member,
                                          label->range_high,
                                          target,
                                          references);
    }
    if (label->type_chain_count == 0U) {
        return collect_references_in_match_type_label(session,
                                                      program,
                                                      source,
                                                      label->type,
                                                      target,
                                                      references) &&
               collect_references_in_expr(session,
                                          program,
                                          source,
                                          owner_decl,
                                          owner_member,
                                          label->value,
                                          target,
                                          references);
    }
    if (!collect_references_in_match_type_label(session,
                                                program,
                                                source,
                                                label->type,
                                                target,
                                                references)) {
        return false;
    }
    for (index = 0U; index < label->type_chain_count; ++index) {
        if (!collect_references_in_match_type_label(session,
                                                    program,
                                                    source,
                                                    label->type_chain[index],
                                                    target,
                                                    references)) {
            return false;
        }
    }
    return true;
}

/* Collects references from a contiguous list of match labels. */
static bool collect_references_in_match_labels(const FengLspAnalysisSession *session,
                                               const FengProgram *program,
                                               const FengCliLoadedSource *source,
                                               const FengDecl *owner_decl,
                                               const FengTypeMember *owner_member,
                                               const FengMatchLabel *labels,
                                               size_t label_count,
                                               const FengLspResolvedTarget *target,
                                               FengLspReferenceList *references) {
    size_t index;

    for (index = 0U; index < label_count; ++index) {
        if (!collect_references_in_match_label(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               &labels[index],
                                               target,
                                               references)) {
            return false;
        }
    }
    return true;
}

/* Collects references from every label and body in a match branch list. */
static bool collect_references_in_match_branches(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengCliLoadedSource *source,
    const FengDecl *owner_decl,
    const FengTypeMember *owner_member,
    const FengMatchBranch *branches,
    size_t branch_count,
    bool include_declaration,
    const FengLspResolvedTarget *target,
    FengLspReferenceList *references) {
    size_t index;

    for (index = 0U; index < branch_count; ++index) {
        if (!collect_references_in_match_labels(session,
                                                program,
                                                source,
                                                owner_decl,
                                                owner_member,
                                                branches[index].labels,
                                                branches[index].label_count,
                                                target,
                                                references) ||
            !collect_references_in_block(session,
                                         program,
                                         source,
                                         owner_decl,
                                         owner_member,
                                         branches[index].body,
                                         include_declaration,
                                         target,
                                         references)) {
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
        case FENG_STMT_DEFER:
            return collect_references_in_block(session,
                                               program,
                                               source,
                                               owner_decl,
                                               owner_member,
                                               stmt->as.defer_block,
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
            return collect_references_in_match_branches(
                       session,
                       program,
                       source,
                       owner_decl,
                       owner_member,
                       stmt->as.match_stmt.branches,
                       stmt->as.match_stmt.branch_count,
                       include_declaration,
                       target,
                       references) &&
                   collect_references_in_block(session,
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
    return collect_references_in_type_params(session,
                                             program,
                                             source,
                                             member->as.callable.type_params,
                                             member->as.callable.type_param_count,
                                             target,
                                             references) &&
           collect_references_in_parameters(session,
                                            program,
                                            source,
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
            if (include_declaration && target != NULL &&
                target->kind == FENG_LSP_RESOLVED_ENUM_ITEM &&
                target->decl == decl) {
                for (index = 0U; index < decl->as.enum_decl.item_count; ++index) {
                    const FengEnumItem *item = &decl->as.enum_decl.items[index];

                    if (target->enum_item == item &&
                        !reference_list_push_slice(references, source, item->name)) {
                        return false;
                    }
                }
            }
            return true;
        case FENG_DECL_FUNCTION:
            return collect_references_in_type_params(
                       session,
                       program,
                       source,
                       decl->as.function_decl.type_params,
                       decl->as.function_decl.type_param_count,
                       target,
                       references) &&
                   collect_references_in_parameters(
                       session,
                       program,
                       source,
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
            if (!collect_references_in_type_params(session,
                                                   program,
                                                   source,
                                                   decl->as.type_decl.type_params,
                                                   decl->as.type_decl.type_param_count,
                                                   target,
                                                   references)) {
                return false;
            }
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                const FengTypeMixinDecl *mixin = &decl->as.type_decl.mixins[index];

                if (!collect_references_in_type_ref(session,
                                                    program,
                                                    source,
                                                    mixin->source_type,
                                                    target,
                                                    references) ||
                    !collect_references_in_expr(session,
                                                program,
                                                source,
                                                decl,
                                                NULL,
                                                mixin->source_constructor,
                                                target,
                                                references)) {
                    return false;
                }
            }
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
            if (!collect_references_in_type_params(session,
                                                   program,
                                                   source,
                                                   decl->as.spec_decl.type_params,
                                                   decl->as.spec_decl.type_param_count,
                                                   target,
                                                   references)) {
                return false;
            }
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
            } else if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                return collect_references_in_parameters(
                           session,
                           program,
                           source,
                           decl->as.spec_decl.as.callable.params,
                           decl->as.spec_decl.as.callable.param_count,
                           include_declaration,
                           target,
                           references) &&
                       collect_references_in_type_ref(
                           session,
                           program,
                           source,
                           decl->as.spec_decl.as.callable.return_type,
                           target,
                           references);
            } else {
                FengTypeRef *const *members =
                    decl->as.spec_decl.form == FENG_SPEC_FORM_UNION
                        ? decl->as.spec_decl.as.union_form.members
                        : decl->as.spec_decl.as.intersection_form.members;
                size_t member_count =
                    decl->as.spec_decl.form == FENG_SPEC_FORM_UNION
                        ? decl->as.spec_decl.as.union_form.member_count
                        : decl->as.spec_decl.as.intersection_form.member_count;

                for (index = 0U; index < member_count; ++index) {
                    if (!collect_references_in_type_ref(session,
                                                        program,
                                                        source,
                                                        members[index],
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
    bool hit_member_name;
    bool hit_member_token;

    /* Generated members have no declaration spelling in the consumer source;
     * the persistent-symbol path follows the same rule as AST lookup. */
    if (member->mixin_origin != NULL) {
        return false;
    }
    hit_member_name = offset_in_slice_from_source(context->source_text,
                                                  member_name_slice(member),
                                                  offset);
    hit_member_token = offset_in_token(member->token, offset);
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
    for (index = 0U; index < member->as.callable.type_param_count; ++index) {
        if (offset_in_token(member->as.callable.type_params[index].token, offset) ||
            offset_in_slice_from_source(context->source_text,
                                        member->as.callable.type_params[index].name,
                                        offset)) {
            const FengSymbolDeclView *owner_symbol = match_ast_decl_to_symbol(context->current_module,
                                                                              context->program,
                                                                              owner_decl);

            target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
            target->type_param = &member->as.callable.type_params[index];
            target->type_param_owner = owner_symbol;
            return true;
        }
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
            for (index = 0U; index < decl->as.function_decl.type_param_count; ++index) {
                if (offset_in_token(decl->as.function_decl.type_params[index].token, offset) ||
                    offset_in_slice_from_source(context->source_text,
                                                decl->as.function_decl.type_params[index].name,
                                                offset)) {
                    const FengSymbolDeclView *symbol_decl = match_ast_decl_to_symbol(context->current_module,
                                                                                     context->program,
                                                                                     decl);

                    target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                    target->type_param = &decl->as.function_decl.type_params[index];
                    target->type_param_owner = symbol_decl;
                    return true;
                }
            }
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
            for (index = 0U; index < decl->as.enum_decl.item_count; ++index) {
                const FengEnumItem *item = &decl->as.enum_decl.items[index];

                if (offset_in_token(item->token, offset) ||
                    offset_in_slice_from_source(context->source_text,
                                                item->name,
                                                offset)) {
                    const FengSymbolDeclView *owner_symbol =
                        match_ast_decl_to_symbol(context->current_module,
                                                 context->program,
                                                 decl);
                    const FengSymbolDeclView *item_symbol =
                        find_symbol_decl_member_by_name(owner_symbol,
                                                        item->name,
                                                        false);

                    if (item_symbol != NULL &&
                        feng_symbol_decl_kind(item_symbol) ==
                            FENG_SYMBOL_DECL_KIND_ENUM_ITEM) {
                        target->kind = FENG_LSP_RESOLVED_ENUM_ITEM;
                        target->decl = owner_symbol;
                        target->member = item_symbol;
                        return true;
                    }
                }
            }
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.type_param_count; ++index) {
                if (offset_in_token(decl->as.type_decl.type_params[index].token, offset) ||
                    offset_in_slice_from_source(context->source_text,
                                                decl->as.type_decl.type_params[index].name,
                                                offset)) {
                    const FengSymbolDeclView *symbol_decl = match_ast_decl_to_symbol(context->current_module,
                                                                                     context->program,
                                                                                     decl);

                    target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                    target->type_param = &decl->as.type_decl.type_params[index];
                    target->type_param_owner = symbol_decl;
                    return true;
                }
            }
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
            for (index = 0U; index < decl->as.spec_decl.type_param_count; ++index) {
                if (offset_in_token(decl->as.spec_decl.type_params[index].token, offset) ||
                    offset_in_slice_from_source(context->source_text,
                                                decl->as.spec_decl.type_params[index].name,
                                                offset)) {
                    const FengSymbolDeclView *symbol_decl = match_ast_decl_to_symbol(context->current_module,
                                                                                     context->program,
                                                                                     decl);

                    target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                    target->type_param = &decl->as.spec_decl.type_params[index];
                    target->type_param_owner = symbol_decl;
                    return true;
                }
            }
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

/* Recursively check whether offset falls within type_ref (including generic
 * type arguments, pointer inner, and array inner) for the symbol/cache path.
 *
 * When owner_decl/type_params/type_param_count are provided, a single-segment
 * named type ref that fails module lookup is checked against the enclosing
 * type parameters — mirrors the analysis-path fallback. */
static bool resolve_symbol_type_ref_at_offset(const FengLspCacheQueryContext *context,
                                              const FengTypeRef *type_ref,
                                              size_t offset,
                                              FengLspCacheResolvedTarget *target,
                                              const FengDecl *owner_decl,
                                              const FengTypeParam *type_params,
                                              size_t type_param_count) {
    size_t index;

    if (type_ref == NULL) {
        return false;
    }
    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            if (offset >= type_ref->token.offset && offset <= named_type_ref_end(type_ref)) {
                const FengSymbolDeclView *decl = resolve_symbol_named_type_ref(context->provider,
                                                                               context->current_module,
                                                                               context->program,
                                                                               type_ref);
                if (decl != NULL) {
                    target->kind = FENG_LSP_RESOLVED_DECL;
                    target->decl = decl;
                    return true;
                }
                /* Fallback: single-segment name that didn't resolve to any
                 * module type may be a type parameter reference. */
                if (type_ref->as.named.segment_count == 1U && type_params != NULL) {
                    FengSlice seg = type_ref->as.named.segments[0];
                    for (index = 0U; index < type_param_count; ++index) {
                        FengSlice pname = type_params[index].name;
                        if (seg.length == pname.length &&
                            seg.data != NULL && pname.data != NULL &&
                            memcmp(seg.data, pname.data, seg.length) == 0) {
                            const FengSymbolDeclView *owner_symbol =
                                match_ast_decl_to_symbol(context->current_module,
                                                         context->program,
                                                         owner_decl);

                            target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
                            target->type_param = &type_params[index];
                            target->type_param_owner = owner_symbol;
                            return true;
                        }
                    }
                }
            }
            for (index = 0U; index < type_ref->as.named.type_arg_count; ++index) {
                if (resolve_symbol_type_ref_at_offset(context,
                                                      type_ref->as.named.type_args[index],
                                                      offset,
                                                      target,
                                                      owner_decl,
                                                      type_params,
                                                      type_param_count)) {
                    return true;
                }
            }
            return false;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            return resolve_symbol_type_ref_at_offset(context, type_ref->as.inner, offset, target,
                                                     owner_decl, type_params, type_param_count);
    }
    return false;
}

/* Cache-path variant of resolve_type_param_hit: checks type parameter names
 * and constraint TypeRefs, populating the cache target on match. */
static bool resolve_symbol_type_param_hit(const FengLspCacheQueryContext *context,
                                           const FengDecl *owner_decl,
                                           const FengTypeParam *type_params,
                                           size_t type_param_count,
                                           size_t offset,
                                           FengLspCacheResolvedTarget *target) {
    size_t index;

    if (type_params == NULL) {
        return false;
    }
    for (index = 0U; index < type_param_count; ++index) {
        if (offset_in_token(type_params[index].token, offset)) {
            const FengSymbolDeclView *owner_symbol = match_ast_decl_to_symbol(context->current_module,
                                                                              context->program,
                                                                              owner_decl);

            target->kind = FENG_LSP_RESOLVED_TYPE_PARAM;
            target->type_param = &type_params[index];
            target->type_param_owner = owner_symbol;
            return true;
        }
        if (type_params[index].constraint != NULL &&
            resolve_symbol_type_ref_at_offset(context,
                                              type_params[index].constraint,
                                              offset,
                                              target,
                                              owner_decl,
                                              type_params,
                                              type_param_count)) {
            return true;
        }
    }
    return false;
}

static bool find_symbol_type_ref_in_member(const FengLspCacheQueryContext *context,
                                           const FengDecl *owner_decl,
                                           const FengTypeMember *member,
                                           size_t offset,
                                           FengLspCacheResolvedTarget *target) {
    size_t index;

    if (member == NULL || member->mixin_origin != NULL) {
        return false;
    }
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        return resolve_symbol_type_ref_at_offset(context, member->as.field.type, offset, target,
                                                 NULL, NULL, 0U);
    }
    if (resolve_symbol_type_param_hit(context,
                                      owner_decl,
                                      member->as.callable.type_params,
                                      member->as.callable.type_param_count,
                                      offset,
                                      target)) {
        return true;
    }
    for (index = 0U; index < member->as.callable.param_count; ++index) {
        if (resolve_symbol_type_ref_at_offset(context,
                                              member->as.callable.params[index].type,
                                              offset,
                                              target,
                                              owner_decl,
                                              member->as.callable.type_params,
                                              member->as.callable.type_param_count)) {
            return true;
        }
    }
    return resolve_symbol_type_ref_at_offset(context,
                                             member->as.callable.return_type,
                                             offset,
                                             target,
                                             owner_decl,
                                             member->as.callable.type_params,
                                             member->as.callable.type_param_count);
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
        case FENG_STMT_BINDING:
            return resolve_symbol_type_ref_at_offset(context, stmt->as.binding.type, offset, target,
                                                     NULL, NULL, 0U);
        case FENG_STMT_BLOCK:
            return find_symbol_block_type_ref_hit(context, stmt->as.block, offset, target);
        case FENG_STMT_DEFER:
            return find_symbol_block_type_ref_hit(context, stmt->as.defer_block, offset, target);
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
                if (resolve_symbol_type_ref_at_offset(context,
                                                      stmt->as.for_stmt.iter_binding.type,
                                                      offset,
                                                      target,
                                                      NULL, NULL, 0U)) {
                    return true;
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
            return resolve_symbol_type_ref_at_offset(context, decl->as.binding.type, offset, target,
                                                     NULL, NULL, 0U);
        case FENG_DECL_ENUM:
            break;
        case FENG_DECL_FUNCTION:
            if (resolve_symbol_type_param_hit(context,
                                              decl,
                                              decl->as.function_decl.type_params,
                                              decl->as.function_decl.type_param_count,
                                              offset,
                                              target)) {
                return true;
            }
            for (index = 0U; index < decl->as.function_decl.param_count; ++index) {
                if (resolve_symbol_type_ref_at_offset(context,
                                                      decl->as.function_decl.params[index].type,
                                                      offset,
                                                      target,
                                                      decl,
                                                      decl->as.function_decl.type_params,
                                                      decl->as.function_decl.type_param_count)) {
                    return true;
                }
            }
            if (resolve_symbol_type_ref_at_offset(context,
                                                  decl->as.function_decl.return_type,
                                                  offset,
                                                  target,
                                                  decl,
                                                  decl->as.function_decl.type_params,
                                                  decl->as.function_decl.type_param_count)) {
                return true;
            }
            if (find_symbol_block_type_ref_hit(context,
                                               decl->as.function_decl.body,
                                               offset,
                                               target)) {
                return true;
            }
            break;
        case FENG_DECL_TYPE:
            if (resolve_symbol_type_param_hit(context,
                                              decl,
                                              decl->as.type_decl.type_params,
                                              decl->as.type_decl.type_param_count,
                                              offset,
                                              target)) {
                return true;
            }
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                if (resolve_symbol_type_ref_at_offset(context,
                                                      decl->as.type_decl.declared_specs[index],
                                                      offset,
                                                      target,
                                                      decl,
                                                      decl->as.type_decl.type_params,
                                                      decl->as.type_decl.type_param_count)) {
                    return true;
                }
            }
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                const FengTypeMixinDecl *mixin = &decl->as.type_decl.mixins[index];

                if (!mixin->infer_source_type &&
                    resolve_symbol_type_ref_at_offset(
                        context,
                        mixin->source_type,
                        offset,
                        target,
                        decl,
                        decl->as.type_decl.type_params,
                        decl->as.type_decl.type_param_count)) {
                    return true;
                }
            }
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                if (find_symbol_type_ref_in_member(context,
                                                   decl,
                                                   decl->as.type_decl.members[index],
                                                   offset,
                                                   target)) {
                    return true;
                }
            }
            break;
        case FENG_DECL_SPEC:
            if (resolve_symbol_type_param_hit(context,
                                              decl,
                                              decl->as.spec_decl.type_params,
                                              decl->as.spec_decl.type_param_count,
                                              offset,
                                              target)) {
                return true;
            }
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                if (resolve_symbol_type_ref_at_offset(context,
                                                      decl->as.spec_decl.parent_specs[index],
                                                      offset,
                                                      target,
                                                      decl,
                                                      decl->as.spec_decl.type_params,
                                                      decl->as.spec_decl.type_param_count)) {
                    return true;
                }
            }
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    if (find_symbol_type_ref_in_member(context,
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
            for (index = 0U; index < decl->as.fit_decl.spec_count; ++index) {
                if (resolve_symbol_type_ref_at_offset(context,
                                                      decl->as.fit_decl.specs[index],
                                                      offset,
                                                      target,
                                                      NULL, NULL, 0U)) {
                    return true;
                }
            }
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                if (find_symbol_type_ref_in_member(context,
                                                   decl,
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
                target->kind = feng_symbol_decl_kind(target->member) ==
                                       FENG_SYMBOL_DECL_KIND_ENUM_ITEM
                                   ? FENG_LSP_RESOLVED_ENUM_ITEM
                                   : FENG_LSP_RESOLVED_MEMBER;
                return target->decl;
            }
        }
    }
    return NULL;
}

/* Resolve an object-literal field through the persistent symbol provider. */
static bool resolve_symbol_object_field_target_decl(
    const FengLspCacheQueryContext *context,
    const FengDecl *decl,
    size_t offset,
    FengLspCacheResolvedTarget *target) {
    const FengExpr *construction = NULL;
    const FengSymbolDeclView *owner;
    FengSlice name = {0};

    if (!find_object_field_syntax_hit_decl(decl,
                                           offset,
                                           &construction,
                                           &name)) {
        return false;
    }
    owner = resolve_symbol_type_constructor_expr(context, construction);
    if (owner == NULL) {
        return false;
    }
    target->member = find_symbol_decl_member_by_name(owner, name, false);
    if (target->member == NULL ||
        feng_symbol_decl_kind(target->member) != FENG_SYMBOL_DECL_KIND_FIELD) {
        memset(target, 0, sizeof(*target));
        return false;
    }
    target->kind = FENG_LSP_RESOLVED_MEMBER;
    target->decl = owner;
    return true;
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
    if (resolve_symbol_object_field_target_decl(context,
                                                enclosing_decl,
                                                offset,
                                                target)) {
        local_list_dispose(&locals);
        return true;
    }
    expr = find_expr_hit_in_decl(enclosing_decl, offset);
    if (expr != NULL) {
        (void)resolve_symbol_expr_target(context, expr, &locals, target);
    }
    local_list_dispose(&locals);
    return target->kind != FENG_LSP_RESOLVED_NONE;
}

/* Forward declarations for builtin type completion helpers defined later
 * in the completion engine. */
static bool completion_context_is_type_position(const char *text,
                                                 size_t offset,
                                                 FengSlice *out_prefix);
static bool append_builtin_type_items(FengLspString *json,
                                       bool *first,
                                       FengSlice prefix);

/* Build structured Hover content for a keyword at the requested offset. */
static bool hover_presentation_for_keyword(const char *text,
                                           size_t offset,
                                           FengLspHoverPresentation *presentation) {
    size_t start;
    size_t end;
    size_t length;
    size_t pos;
    size_t index;

    if (text == NULL || presentation == NULL || offset > strlen(text)) {
        return false;
    }
    /* Expand backward from offset to find identifier start. */
    start = offset;
    while (start > 0U && completion_identifier_continue(text[start - 1U])) {
        --start;
    }
    /* Expand forward from offset to find identifier end. */
    end = offset;
    length = strlen(text);
    while (end < length && completion_identifier_continue(text[end])) {
        ++end;
    }
    if (start == end) {
        return false;
    }
    /* Reject if the first character is not a valid identifier start. */
    if (!completion_identifier_start(text[start])) {
        return false;
    }
    /* Verify the word is actually a keyword (not a plain identifier). */
    if (!feng_lookup_keyword(text + start, end - start, NULL)) {
        return NULL;
    }
    /* Search all keyword tables for a matching LspKwItem label. */
    for (pos = FENG_LSP_POS_TOP_DECL;
         pos <= FENG_LSP_POS_BODY;
         pos = (FengLspPosition)((size_t)pos + 1U)) {
        const LspKwTable *table = &KW_TABLE[(size_t)pos];
        if (table->items == NULL || table->count == 0U) {
            continue;
        }
        for (index = 0U; index < table->count; ++index) {
            const LspKwItem *item = &table->items[index];
            size_t label_len = strlen(item->label);

            if (label_len == end - start &&
                memcmp(item->label, text + start, label_len) == 0) {
                if (!string_append_bytes(&presentation->signature,
                                         text + start,
                                         end - start)) {
                    return false;
                }
                presentation->documentation = dup_cstr(item->detail);
                return presentation->documentation != NULL;
            }
        }
    }
    return false;
}

/* Build structured Hover content for a builtin annotation. */
static bool hover_presentation_for_annotation(const char *text,
                                              size_t offset,
                                              FengLspHoverPresentation *presentation) {
    size_t start;
    size_t end;
    size_t length;
    size_t at_pos;
    size_t index;

    if (text == NULL || presentation == NULL || offset > strlen(text)) {
        return false;
    }
    /* Expand backward from offset to find identifier start. */
    start = offset;
    while (start > 0U && completion_identifier_continue(text[start - 1U])) {
        --start;
    }
    /* Expand forward from offset to find identifier end. */
    end = offset;
    length = strlen(text);
    while (end < length && completion_identifier_continue(text[end])) {
        ++end;
    }
    if (start == end) {
        return false;
    }
    /* Reject if the first character is not a valid identifier start. */
    if (!completion_identifier_start(text[start])) {
        return false;
    }
    /* The character before the identifier must be '@'. */
    if (start == 0U || text[start - 1U] != '@') {
        return false;
    }
    at_pos = start - 1U;
    /* '@' must be at start of file or preceded by whitespace. */
    if (at_pos > 0U && !isspace((unsigned char)text[at_pos - 1U])) {
        return false;
    }
    /* Search the builtin annotation table for a matching label. */
    for (index = 0U; index < BUILTIN_ANNOTATION_COUNT; ++index) {
        const LspAnnotationItem *item = &BUILTIN_ANNOTATIONS[index];
        size_t label_len = strlen(item->label);

        if (label_len == end - start &&
            memcmp(item->label, text + start, label_len) == 0) {
            if (!string_append_cstr(&presentation->signature, "@") ||
                !string_append_bytes(&presentation->signature,
                                     text + start,
                                     end - start)) {
                return false;
            }
            presentation->documentation = dup_cstr(item->detail);
            return presentation->documentation != NULL;
        }
    }
    return false;
}

/* Build structured Hover content for a builtin type or alias. */
static bool hover_presentation_for_builtin_type(
    const char *text,
    size_t offset,
    FengLspHoverPresentation *presentation) {
    size_t start;
    size_t end;
    size_t length;
    size_t index;

    if (text == NULL || presentation == NULL || offset > strlen(text)) {
        return false;
    }
    /* Expand backward from offset to find identifier start. */
    start = offset;
    while (start > 0U && completion_identifier_continue(text[start - 1U])) {
        --start;
    }
    /* Expand forward from offset to find identifier end. */
    end = offset;
    length = strlen(text);
    while (end < length && completion_identifier_continue(text[end])) {
        ++end;
    }
    if (start == end) {
        return false;
    }
    /* Reject if the first character is not a valid identifier start. */
    if (!completion_identifier_start(text[start])) {
        return false;
    }
    /* Search builtin type table for a matching label. */
    for (index = 0U; index < BUILTIN_TYPE_COUNT; ++index) {
        const LspBuiltinTypeItem *item = &BUILTIN_TYPES[index];
        size_t label_len = strlen(item->label);

        if (label_len == end - start &&
            memcmp(item->label, text + start, label_len) == 0) {
            if (!string_append_bytes(&presentation->signature,
                                     text + start,
                                     end - start)) {
                return false;
            }
            presentation->documentation = dup_cstr(item->detail);
            hover_presentation_set_category(presentation,
                                            "Kind",
                                            FENG_LSP_TYPE_CATEGORY_BUILTIN);
            return presentation->documentation != NULL;
        }
    }
    /* Search builtin type alias table for a matching label. */
    for (index = 0U; index < BUILTIN_TYPE_ALIAS_COUNT; ++index) {
        const LspBuiltinTypeAliasItem *alias = &BUILTIN_TYPE_ALIASES[index];
        size_t label_len = strlen(alias->label);

        if (label_len == end - start &&
            memcmp(alias->label, text + start, label_len) == 0) {
            /* Resolve platform-dependent canonical (NULL → i32/i64 by pointer size). */
            const char *resolved = alias->canonical;

            if (resolved == NULL) {
                resolved = (sizeof(void *) == 4U) ? "i32" : "i64";
            }
            if (!string_append_bytes(&presentation->signature,
                                     text + start,
                                     end - start) ||
                !string_append_cstr(&presentation->signature, " \xe2\x86\x92 ") ||
                !string_append_cstr(&presentation->signature, resolved)) {
                return false;
            }
            presentation->documentation = dup_cstr(alias->detail);
            hover_presentation_set_category(presentation,
                                            "Kind",
                                            FENG_LSP_TYPE_CATEGORY_BUILTIN);
            return presentation->documentation != NULL;
        }
    }
    return false;
}

/* Builds the current-version token index once and reuses it for local queries. */
static bool ensure_document_token_index(FengLspDocument *document) {
    FengLexer lexer;
    FengToken token;

    if (document == NULL || document->text == NULL) {
        return false;
    }
    if (document->tokens_indexed) {
        return true;
    }
    feng_lexer_init(&lexer, document->text, strlen(document->text), document->path);
    for (;;) {
        FengLspTokenSpan span;

        token = feng_lexer_next(&lexer);
        if (token.kind == FENG_TOKEN_EOF || token.kind == FENG_TOKEN_ERROR) {
            break;
        }
        span.kind = token.kind;
        span.offset = token.offset;
        span.length = token.length;
        if (!append_raw((void **)&document->tokens,
                        &document->token_count,
                        &document->token_capacity,
                        sizeof(span),
                        &span)) {
            free(document->tokens);
            document->tokens = NULL;
            document->token_count = 0U;
            document->token_capacity = 0U;
            return false;
        }
    }
    document->tokens_indexed = true;
    return true;
}

/* Build structured Hover content for a literal from the document token index. */
static bool hover_presentation_for_literal(FengLspDocument *document,
                                           size_t offset,
                                           FengLspHoverPresentation *presentation) {
    size_t low = 0U;
    size_t high;
    const FengLspTokenSpan *token;

    if (presentation == NULL || !ensure_document_token_index(document) ||
        document->token_count == 0U) {
        return false;
    }
    high = document->token_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;

        if (document->tokens[middle].offset <= offset) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    if (low == 0U) {
        return false;
    }
    token = &document->tokens[low - 1U];
    if (offset >= token->offset + token->length) {
        return false;
    }
    switch (token->kind) {
        case FENG_TOKEN_INTEGER:
            return string_append_cstr(&presentation->signature, "integer literal");
        case FENG_TOKEN_FLOAT:
            return string_append_cstr(&presentation->signature, "float literal");
        case FENG_TOKEN_STRING:
            return string_append_cstr(&presentation->signature, "string literal");
        default:
            break;
    }
    return false;
}

/* Classify an AST type reference against the already-loaded symbol index. */
static FengLspTypeCategory hover_category_from_cache_type_ref(
    const FengLspCacheQueryContext *context,
    const FengTypeRef *type_ref) {
    if (context == NULL || type_ref == NULL) {
        return FENG_LSP_TYPE_CATEGORY_UNKNOWN;
    }
    if (type_ref->kind == FENG_TYPE_REF_ARRAY) {
        return FENG_LSP_TYPE_CATEGORY_ARRAY;
    }
    if (type_ref->kind == FENG_TYPE_REF_POINTER) {
        return FENG_LSP_TYPE_CATEGORY_POINTER;
    }
    if (type_ref->kind == FENG_TYPE_REF_NAMED &&
        type_ref->as.named.segment_count == 1U &&
        feng_semantic_is_builtin_type_name(type_ref->as.named.segments[0])) {
        return FENG_LSP_TYPE_CATEGORY_BUILTIN;
    }
    return hover_category_from_symbol_decl(
        resolve_symbol_named_type_ref(context->provider,
                                      context->current_module,
                                      context->program,
                                      type_ref));
}

/* Build structured Hover content from a persistent symbol-index target. */
static bool hover_presentation_for_cache_target(
    const FengLspCacheQueryContext *context,
    const FengLspCacheResolvedTarget *target,
    FengLspHoverPresentation *presentation) {
    FengSlice documentation = {0};

    if (context == NULL || target == NULL || presentation == NULL) {
        return false;
    }

    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            if (!symbol_decl_hover_signature_to_string(&presentation->signature,
                                                       target->decl)) {
                return false;
            }
            documentation = feng_symbol_decl_doc(target->decl);
            if (feng_symbol_decl_kind(target->decl) == FENG_SYMBOL_DECL_KIND_TYPE ||
                feng_symbol_decl_kind(target->decl) == FENG_SYMBOL_DECL_KIND_ENUM ||
                feng_symbol_decl_kind(target->decl) == FENG_SYMBOL_DECL_KIND_SPEC) {
                hover_presentation_set_category(
                    presentation,
                    "Kind",
                    hover_category_from_symbol_decl(target->decl));
            } else if (feng_symbol_decl_kind(target->decl) ==
                       FENG_SYMBOL_DECL_KIND_BINDING) {
                hover_presentation_set_category(
                    presentation,
                    "Kind",
                    hover_category_from_symbol_type(
                        context,
                        feng_symbol_decl_value_type(target->decl)));
            }
            break;
        case FENG_LSP_RESOLVED_ENUM_ITEM:
            if (!symbol_member_signature_to_string_with_style(
                    &presentation->signature,
                    target->member,
                    FENG_LSP_TYPE_NAME_SHORT)) {
                return false;
            }
            documentation = feng_symbol_decl_doc(target->member);
            hover_presentation_set_category(presentation,
                                            "Kind",
                                            FENG_LSP_TYPE_CATEGORY_ENUM);
            break;
        case FENG_LSP_RESOLVED_MEMBER:
            if (!symbol_member_signature_to_string_with_style(
                    &presentation->signature,
                    target->member,
                    FENG_LSP_TYPE_NAME_SHORT)) {
                return false;
            }
            documentation = feng_symbol_decl_doc(target->member);
            if (feng_symbol_decl_kind(target->member) == FENG_SYMBOL_DECL_KIND_FIELD) {
                hover_presentation_set_category(
                    presentation,
                    "Kind",
                    hover_category_from_symbol_type(
                        context,
                        feng_symbol_decl_value_type(target->member)));
            } else if (feng_symbol_decl_kind(target->member) ==
                       FENG_SYMBOL_DECL_KIND_CONSTRUCTOR) {
                hover_presentation_set_category(
                    presentation,
                    "Kind",
                    hover_category_from_symbol_decl(target->decl));
            }
            break;
        case FENG_LSP_RESOLVED_PARAM:
            if (!string_append_cstr(&presentation->signature, "param ") ||
                !string_append_cstr(&presentation->signature,
                                    target->parameter->mutability == FENG_MUTABILITY_VAR
                                        ? "var "
                                        : "let ") ||
                !string_append_bytes(&presentation->signature,
                                     target->parameter->name.data,
                                     target->parameter->name.length) ||
                !string_append_cstr(&presentation->signature, ": ") ||
                !hover_type_ref_to_string(&presentation->signature,
                                          target->parameter->type)) {
                return false;
            }
            hover_presentation_set_category(
                presentation,
                "Kind",
                hover_category_from_cache_type_ref(context, target->parameter->type));
            break;
        case FENG_LSP_RESOLVED_BINDING:
            if (!string_append_cstr(&presentation->signature,
                                    target->binding->mutability == FENG_MUTABILITY_VAR
                                        ? "var "
                                        : "let ") ||
                !string_append_bytes(&presentation->signature,
                                     target->binding->name.data,
                                     target->binding->name.length)) {
                return false;
            }
            if (target->binding->type != NULL &&
                (!string_append_cstr(&presentation->signature, ": ") ||
                 !hover_type_ref_to_string(&presentation->signature,
                                           target->binding->type))) {
                return false;
            }
            hover_presentation_set_category(
                presentation,
                "Kind",
                hover_category_from_cache_type_ref(context, target->binding->type));
            break;
        case FENG_LSP_RESOLVED_SELF: {
            FengSlice name = target->self_owner_decl != NULL
                                 ? feng_symbol_decl_name(target->self_owner_decl)
                                 : (FengSlice){0};

            if (!string_append_cstr(&presentation->signature, "self: ") ||
                !string_append_bytes(&presentation->signature, name.data, name.length)) {
                return false;
            }
            hover_presentation_set_category(
                presentation,
                "Kind",
                hover_category_from_symbol_decl(target->self_owner_decl));
            break;
        }
        case FENG_LSP_RESOLVED_TYPE_PARAM:
            if (!string_append_cstr(&presentation->signature, "generic parameter ") ||
                !string_append_bytes(&presentation->signature,
                                     target->type_param->name.data,
                                     target->type_param->name.length)) {
                return false;
            }
            if (target->type_param->constraint != NULL &&
                (!string_append_cstr(&presentation->signature, ": ") ||
                 !hover_type_ref_to_string(&presentation->signature,
                                           target->type_param->constraint))) {
                return false;
            }
            break;
        default:
            return false;
    }
    if (documentation.data != NULL && documentation.length > 0U) {
        presentation->documentation = dup_range(documentation.data,
                                                documentation.data + documentation.length);
        if (presentation->documentation == NULL) {
            return false;
        }
    }
    return presentation->signature.data != NULL;
}

/* Build the aggregate Hover shown for one exact `...` expansion token.
 * The final target member table is the sole source of truth because it already
 * contains conflict filtering, generic substitution, visible fit expansion,
 * and the stable generated-member order. */
static bool hover_presentation_for_mixin(const FengLspAnalysisSession *session,
                                         const FengProgram *program,
                                         size_t offset,
                                         FengLspHoverPresentation *presentation) {
    size_t decl_index;

    if (session == NULL || program == NULL || presentation == NULL) {
        return false;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        size_t mixin_index;

        if (decl == NULL || decl->kind != FENG_DECL_TYPE ||
            offset < decl->token.offset || offset > decl_end(decl)) {
            continue;
        }
        for (mixin_index = 0U; mixin_index < decl->as.type_decl.mixin_count;
             ++mixin_index) {
            const FengTypeMixinDecl *mixin = &decl->as.type_decl.mixins[mixin_index];
            bool wrote_member = false;
            size_t member_index;

            if (!offset_in_token(mixin->token, offset)) {
                continue;
            }
            for (member_index = 0U; member_index < decl->as.type_decl.member_count;
                 ++member_index) {
                const FengTypeMember *member = decl->as.type_decl.members[member_index];

                if (member == NULL || member->mixin_origin != mixin) {
                    continue;
                }
                if ((wrote_member &&
                     !string_append_cstr(&presentation->signature, "\n")) ||
                    (member->is_static &&
                     !string_append_cstr(&presentation->signature, "static ")) ||
                    !member_signature_to_string_with_session_and_style(
                        &presentation->signature,
                        session,
                        member,
                        FENG_LSP_TYPE_NAME_SHORT)) {
                    return false;
                }
                wrote_member = true;
            }
            return wrote_member;
        }
    }
    return false;
}

/* Returns whether cached source positions are exact for the current document. */
static bool analysis_matches_document(const FengLspAnalysisSession *session,
                                      const FengLspDocument *document) {
    const FengCliLoadedSource *source;
    size_t text_length;

    if (session == NULL || document == NULL || document->text == NULL) {
        return false;
    }
    source = find_source(session, document->path);
    if (source == NULL || source->source == NULL) {
        return false;
    }
    text_length = strlen(document->text);
    return source->source_length == text_length &&
           memcmp(source->source, document->text, text_length) == 0;
}

/* Return whether `offset` remains exact for the successful generation. */
static bool analysis_position_matches_document(const FengLspDocument *document,
                                               size_t successful_generation,
                                               size_t offset) {
    return document != NULL && successful_generation > 0U &&
           document->successful_prefix_generation == successful_generation &&
           offset < document->successful_prefix_length;
}

static bool handle_hover_request(FengLspService *service,
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
    const FengLspAnalysisSession *session;
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspCacheResolvedTarget cache_target = {0};
    FengCliLoadedSource current_source = {0};
    FengLspAnalysisSession current_parse = {0};
    size_t offset;
    FengLspHoverPresentation presentation = {0};
    FengLspString result = {0};
    bool has_hover;
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
    document = find_document(service, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = document_offset_from_position(document, line, character);
    /* Current-text Hover is exact and must never wait for project analysis or
     * symbol-provider construction. */
    has_hover = hover_presentation_for_keyword(document->text, offset, &presentation);
    if (!has_hover) {
        hover_presentation_dispose(&presentation);
        has_hover = hover_presentation_for_annotation(document->text,
                                                       offset,
                                                       &presentation);
    }
    if (!has_hover) {
        hover_presentation_dispose(&presentation);
        has_hover = hover_presentation_for_builtin_type(document->text,
                                                        offset,
                                                        &presentation);
    }
    if (!has_hover) {
        hover_presentation_dispose(&presentation);
        has_hover = hover_presentation_for_literal(document, offset, &presentation);
    }
    if (has_hover) {
        ok = build_hover_result_json(&result,
                                     service->hover_markup_kind,
                                     &presentation);
        hover_presentation_dispose(&presentation);
        free(uri);
        if (!ok) {
            if (service->errors != NULL) {
                fprintf(service->errors,
                        "lsp: textDocument/hover: out of memory building current-text response\n");
            }
            string_dispose(&result);
            return send_json_response(output, id, "null");
        }
        ok = send_json_response(output, id, result.data);
        string_dispose(&result);
        return ok;
    }
    hover_presentation_dispose(&presentation);
    wait_for_initial_query_state(service);
    /* Read the immutable published analysis only when its source fingerprint
     * exactly matches the current document. */
    has_hover = false;
    pthread_mutex_lock(&service->analysis_mutex);
    session = &service->last_successful_analysis;
    if (analysis_matches_document(session, document) ||
        analysis_position_matches_document(document,
                                           service->last_successful_generation,
                                           offset)) {
        program = find_program(session, document->path);
        if (program != NULL) {
            has_hover = hover_presentation_for_mixin(session,
                                                     program,
                                                     offset,
                                                     &presentation);
            if (!has_hover && resolve_target_at(session, program, offset, &target)) {
                has_hover = hover_presentation_for_target(session,
                                                           program,
                                                           &target,
                                                           &presentation);
            }
        }
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    if (has_hover) {
        ok = build_hover_result_json(&result,
                                     service->hover_markup_kind,
                                     &presentation);
        hover_presentation_dispose(&presentation);
        free(uri);
        if (!ok) {
            if (service->errors != NULL) {
                fprintf(service->errors,
                        "lsp: textDocument/hover: out of memory building response\n");
            }
            string_dispose(&result);
            return send_json_response(output, id, "null");
        }
        ok = send_json_response(output, id, result.data);
        string_dispose(&result);
        return ok;
    }
    hover_presentation_dispose(&presentation);
    program = ensure_document_parse(document);
    if (program != NULL) {
        current_source.path = document->path;
        current_source.source = document->text;
        current_source.source_length = strlen(document->text);
        current_source.program = (FengProgram *)program;
        current_parse.sources = &current_source;
        current_parse.source_count = 1U;
        memset(&target, 0, sizeof(target));
        if (resolve_target_at(&current_parse, program, offset, &target)) {
            has_hover = hover_presentation_for_target(&current_parse,
                                                       program,
                                                       &target,
                                                       &presentation);
            if (has_hover) {
                ok = build_hover_result_json(&result,
                                             service->hover_markup_kind,
                                             &presentation);
                hover_presentation_dispose(&presentation);
                free(uri);
                if (!ok) {
                    string_dispose(&result);
                    return send_json_response(output, id, "null");
                }
                ok = send_json_response(output, id, result.data);
                string_dispose(&result);
                return ok;
            }
        }
    }
    hover_presentation_dispose(&presentation);
    has_hover = false;
    pthread_mutex_lock(&service->analysis_mutex);
    if (build_persistent_cache_query_context(service,
                                             document,
                                             document->text,
                                             &cache) &&
        resolve_symbol_target_at(&cache, offset, &cache_target)) {
        has_hover = hover_presentation_for_cache_target(&cache,
                                                        &cache_target,
                                                        &presentation);
    }
    cache_query_context_dispose(&cache);
    pthread_mutex_unlock(&service->analysis_mutex);
    if (has_hover) {
        ok = build_hover_result_json(&result,
                                     service->hover_markup_kind,
                                     &presentation);
        hover_presentation_dispose(&presentation);
        free(uri);
        if (!ok) {
            string_dispose(&result);
            return send_json_response(output, id, "null");
        }
        ok = send_json_response(output, id, result.data);
        string_dispose(&result);
        return ok;
    }
    hover_presentation_dispose(&presentation);
    free(uri);
    return send_json_response(output, id, "null");
}

/* Builds a definition location from a target in published semantic analysis. */
static bool definition_location_from_analysis(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const FengLspResolvedTarget *target,
                                              FengLspString *result) {
    const FengProgram *target_program = NULL;

    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL:
            (void)find_decl_module(session, target->decl, &target_program);
            if (target_program == NULL && session->analysis == NULL) {
                target_program = program;
            }
            return location_json(result,
                                 target_program != NULL ? target_program->path : NULL,
                                 target->decl->token);
        case FENG_LSP_RESOLVED_MEMBER:
            if (target->member != NULL &&
                target->member->mixin_source_member != NULL) {
                const FengTypeMember *source_member =
                    mixin_definition_source_member(target->member);
                const FengDecl *source_decl = NULL;

                (void)find_member_module(session,
                                         source_member,
                                         &source_decl,
                                         &target_program);
                if (source_decl != NULL && target_program != NULL) {
                    return location_json(result,
                                         target_program->path,
                                         source_member->token);
                }
            }
            (void)find_decl_module(session, target->decl, &target_program);
            if (target_program == NULL && session->analysis == NULL) {
                target_program = program;
            }
            return location_json(result,
                                 target_program != NULL ? target_program->path : NULL,
                                 target->member->token);
        case FENG_LSP_RESOLVED_ENUM_ITEM:
            (void)find_decl_module(session, target->decl, &target_program);
            if (target_program == NULL && session->analysis == NULL) {
                target_program = program;
            }
            return location_json(result,
                                 target_program != NULL ? target_program->path : NULL,
                                 target->enum_item->token);
        case FENG_LSP_RESOLVED_PARAM:
            return location_json(result, program->path, target->parameter->token);
        case FENG_LSP_RESOLVED_BINDING:
            return location_json(result, program->path, target->binding->token);
        case FENG_LSP_RESOLVED_SELF:
            (void)find_decl_module(session, target->self_owner_decl, &target_program);
            if (target_program == NULL && session->analysis == NULL) {
                target_program = program;
            }
            return location_json(result,
                                 target_program != NULL ? target_program->path : NULL,
                                 target->self_owner_decl->token);
        case FENG_LSP_RESOLVED_TYPE_PARAM:
            return location_json(result, program->path, target->type_param->token);
        default:
            return false;
    }
}

/* Builds a definition location from the persistent symbol index view. */
static bool definition_location_from_cache(const FengLspCacheQueryContext *cache,
                                           const FengLspCacheResolvedTarget *target,
                                           FengLspString *result) {
    switch (target->kind) {
        case FENG_LSP_RESOLVED_DECL: {
            FengSlice path = feng_symbol_decl_path(target->decl);
            return location_json(result, path.data, feng_symbol_decl_token(target->decl));
        }
        case FENG_LSP_RESOLVED_MEMBER: {
            FengSlice path = feng_symbol_decl_path(target->member);
            return location_json(result, path.data, feng_symbol_decl_token(target->member));
        }
        case FENG_LSP_RESOLVED_ENUM_ITEM: {
            FengSlice path = feng_symbol_decl_path(target->member);
            return location_json(result, path.data, feng_symbol_decl_token(target->member));
        }
        case FENG_LSP_RESOLVED_PARAM:
            return location_json(result, cache->program->path, target->parameter->token);
        case FENG_LSP_RESOLVED_BINDING:
            return location_json(result, cache->program->path, target->binding->token);
        case FENG_LSP_RESOLVED_SELF: {
            FengSlice path = feng_symbol_decl_path(target->self_owner_decl);
            return location_json(result,
                                 path.data,
                                 feng_symbol_decl_token(target->self_owner_decl));
        }
        case FENG_LSP_RESOLVED_TYPE_PARAM:
            return location_json(result, cache->program->path, target->type_param->token);
        default:
            return false;
    }
}

/* Handles definition using only current memory state and published caches. */
static bool handle_definition_request(FengLspService *service,
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
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    FengLspResolvedTarget target = {0};
    FengLspCacheResolvedTarget cache_target = {0};
    FengCliLoadedSource current_source = {0};
    FengLspAnalysisSession current_parse = {0};
    FengLspString result = {0};
    bool found = false;
    bool ok = true;
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
    document = find_document(service, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = document_offset_from_position(document, line, character);
    pthread_mutex_lock(&service->analysis_mutex);
    if (analysis_matches_document(&service->last_successful_analysis, document)) {
        program = find_program(&service->last_successful_analysis, document->path);
        if (program != NULL &&
            resolve_target_at(&service->last_successful_analysis, program, offset, &target)) {
            found = true;
            ok = definition_location_from_analysis(&service->last_successful_analysis,
                                                   program,
                                                   &target,
                                                   &result);
        }
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    if (!found) {
        program = ensure_document_parse(document);
        if (program != NULL) {
            current_source.path = document->path;
            current_source.source = document->text;
            current_source.source_length = strlen(document->text);
            current_source.program = (FengProgram *)program;
            current_parse.sources = &current_source;
            current_parse.source_count = 1U;
            if (resolve_target_at(&current_parse, program, offset, &target)) {
                found = true;
                ok = definition_location_from_analysis(&current_parse,
                                                       program,
                                                       &target,
                                                       &result);
            }
        }
    }
    pthread_mutex_lock(&service->analysis_mutex);
    if (!found && build_persistent_cache_query_context(service,
                                                       document,
                                                       document->text,
                                                       &cache) &&
        resolve_symbol_target_at(&cache, offset, &cache_target)) {
        found = true;
        ok = definition_location_from_cache(&cache, &cache_target, &result);
    }
    cache_query_context_dispose(&cache);
    pthread_mutex_unlock(&service->analysis_mutex);
    free(uri);
    if (!found || !ok) {
        string_dispose(&result);
        return send_json_response(output, id, "null");
    }
    ok = send_json_response(output, id, result.data);
    string_dispose(&result);
    return ok;
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
    FengSlice receiver;
    FengSlice object;
    FengSlice prefix;
    FengSlice literal_builtin_name;
    FengLspPosition position;   /* grammar position for keyword completion */
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

/* Classify the grammar position of `offset` within `text` using a
 * brace-matching heuristic.  Only used when AST analysis is unavailable
 * (dirty code).  Does not handle braces inside string literals or
 * comments — a known tradeoff documented in the design spec (§3.2). */
static FengLspPosition completion_position_from_text(const char *text,
                                                     size_t offset) {
    size_t depth;
    size_t cursor;
    size_t brace_pos;
    size_t ident_end;
    size_t ident_start;
    size_t ident_len;

    if (text == NULL || offset == 0U) {
        return FENG_LSP_POS_TOP_DECL;
    }

    /* Scan backward from offset, tracking brace nesting.  Find the
     * nearest `{` that is not closed by a matching `}`. */
    depth = 0U;
    brace_pos = (size_t)-1;
    cursor = offset;
    while (cursor > 0U) {
        --cursor;
        if (text[cursor] == '}') {
            ++depth;
        } else if (text[cursor] == '{') {
            if (depth == 0U) {
                brace_pos = cursor;
                break;
            }
            --depth;
        }
    }
    if (brace_pos == (size_t)-1) {
        /* No unclosed `{` found: we are at the module top level. */
        return FENG_LSP_POS_TOP_DECL;
    }

    /* Skip whitespace before the `{` to find the preceding token. */
    cursor = brace_pos;
    while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
        --cursor;
    }
    if (cursor == 0U) {
        return FENG_LSP_POS_BODY;
    }

    /* Check for `)` immediately before `{` (function signature end). */
    if (text[cursor - 1U] == ')') {
        return FENG_LSP_POS_BODY;
    }

    /* Read the identifier before `{`.  For declarations like
     * `type Foo {`, `enum Color {`, `spec Bar {`, `fit Baz {`,
     * this is the declaration NAME, not the keyword. */
    ident_end = cursor;
    while (cursor > 0U && completion_identifier_continue(text[cursor - 1U])) {
        --cursor;
    }
    ident_start = cursor;
    ident_len = ident_end - ident_start;
    if (ident_len == 0U) {
        return FENG_LSP_POS_BODY;
    }

    /* First, check if the identifier IS a keyword itself (e.g. bare
     * `let {` or `func {` during editing). */
    if (ident_len == 4U && memcmp(text + ident_start, "func", 4U) == 0) {
        return FENG_LSP_POS_BODY;
    }
    if (ident_len == 3U && memcmp(text + ident_start, "let", 3U) == 0) {
        return FENG_LSP_POS_TOP_BIND;
    }
    if (ident_len == 3U && memcmp(text + ident_start, "var", 3U) == 0) {
        return FENG_LSP_POS_TOP_BIND;
    }

    /* The identifier is likely a declaration name.  Look further back
     * past whitespace for the declaration keyword. */
    while (cursor > 0U && isspace((unsigned char)text[cursor - 1U])) {
        --cursor;
    }
    {
        size_t kw_end = cursor;
        size_t kw_start = cursor;

        while (kw_start > 0U && completion_identifier_continue(text[kw_start - 1U])) {
            --kw_start;
        }
        {
            size_t kw_len = kw_end - kw_start;

            if (kw_len == 4U && memcmp(text + kw_start, "type", 4U) == 0) {
                return FENG_LSP_POS_MEMBER;
            }
            if (kw_len == 4U && memcmp(text + kw_start, "spec", 4U) == 0) {
                return FENG_LSP_POS_MEMBER;
            }
            if (kw_len == 3U && memcmp(text + kw_start, "fit", 3U) == 0) {
                return FENG_LSP_POS_MEMBER;
            }
            if (kw_len == 4U && memcmp(text + kw_start, "enum", 4U) == 0) {
                return FENG_LSP_POS_OTHER;
            }
            if (kw_len == 4U && memcmp(text + kw_start, "func", 4U) == 0) {
                return FENG_LSP_POS_BODY;
            }
        }
    }
    /* Inside `{}` but no recognised keyword — assume function body. */
    return FENG_LSP_POS_BODY;
}

/* Compute the grammar position from AST data.  Takes precedence over
 * the text-based heuristic when a valid AST is available.  The rules
 * are documented in the design spec (§3.1). */
static FengLspPosition completion_position_from_ast(const FengDecl *enclosing_decl,
                                                    const FengTypeMember *enclosing_member) {
    if (enclosing_decl == NULL) {
        return FENG_LSP_POS_TOP_DECL;
    }
    if (enclosing_member != NULL) {
        if (enclosing_member->kind != FENG_TYPE_MEMBER_FIELD) {
            return FENG_LSP_POS_BODY;
        }
        return FENG_LSP_POS_MEMBER;
    }
    if (enclosing_decl->kind == FENG_DECL_FUNCTION) {
        return FENG_LSP_POS_BODY;
    }
    if (enclosing_decl->kind == FENG_DECL_GLOBAL_BINDING) {
        return FENG_LSP_POS_TOP_BIND;
    }
    if (enclosing_decl->kind == FENG_DECL_ENUM) {
        return FENG_LSP_POS_OTHER;
    }
    /* type / spec / fit body without a specific member hit. */
    return FENG_LSP_POS_MEMBER;
}

static bool completion_context_from_text(const char *text,
                                         size_t offset,
                                         FengLspCompletionContext *context) {
    size_t length;
    size_t prefix_start;
    size_t receiver_start;
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
        context->position = completion_position_from_text(text, offset);
        return true;
    }
    object_end = prefix_start - 1U;
    if (!receiver_text_find_start(text, object_end, &receiver_start)) {
        return true;
    }
    context->is_member = true;
    context->receiver.data = text + receiver_start;
    context->receiver.length = object_end - receiver_start;
    context->prefix.data = text + prefix_start;
    context->prefix.length = offset - prefix_start;
    object_start = object_end;
    while (object_start > 0U && completion_identifier_continue(text[object_start - 1U])) {
        --object_start;
    }
    if (object_start == object_end) {
        /* No identifier chars before the dot.  Check for a closing string
         * quote immediately before the dot: `"hello".` */
        if (object_end > 0U && text[object_end - 1U] == '"') {
            context->literal_builtin_name = slice_from_cstr("string");
        }
        return true;
    }
    if (!completion_identifier_start(text[object_start])) {
        /* Object starts with a non-identifier character.  If it starts
         * with a digit the token is a numeric literal. */
        if (context->receiver.length == object_end - object_start &&
            isdigit((unsigned char)text[object_start])) {
            context->literal_builtin_name = slice_from_cstr("i32");
            context->object.data = text + object_start;
            context->object.length = object_end - object_start;
        }
        return true;
    }
    context->object.data = text + object_start;
    context->object.length = object_end - object_start;
    /* Detect boolean literal keywords `true` and `false`. */
    if (context->receiver.length == context->object.length &&
        (slice_equals_cstr(context->object, "true") ||
         slice_equals_cstr(context->object, "false"))) {
        context->literal_builtin_name = slice_from_cstr("bool");
    }
    /* Detect builtin type names for static member access (e.g. i32.parse). */
    if (context->receiver.length == context->object.length) {
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

/* Hashes one completion label for the request-local open-addressed set. */
static size_t completion_label_hash(FengSlice label) {
    size_t hash = (size_t)1469598103934665603ULL;
    size_t index;

    for (index = 0U; index < label.length; ++index) {
        hash ^= (unsigned char)label.data[index];
        hash *= (size_t)1099511628211ULL;
    }
    return hash;
}

/* Rebuilds the completion label table at a larger power-of-two capacity. */
static bool completion_labels_grow(FengLspString *json) {
    size_t new_capacity = json->completion_label_capacity == 0U
        ? 32U
        : json->completion_label_capacity * 2U;
    char **grown = (char **)calloc(new_capacity, sizeof(grown[0]));
    size_t index;

    if (grown == NULL) {
        return false;
    }
    for (index = 0U; index < json->completion_label_capacity; ++index) {
        char *label = json->completion_labels[index];

        if (label != NULL) {
            size_t slot = completion_label_hash(slice_from_cstr(label)) & (new_capacity - 1U);

            while (grown[slot] != NULL) {
                slot = (slot + 1U) & (new_capacity - 1U);
            }
            grown[slot] = label;
        }
    }
    free(json->completion_labels);
    json->completion_labels = grown;
    json->completion_label_capacity = new_capacity;
    return true;
}

/* Registers a label and reports whether its CompletionItem should be emitted. */
static bool completion_label_register(FengLspString *json,
                                      FengSlice label,
                                      bool *out_append) {
    size_t slot;
    char *copy;

    *out_append = false;
    if (json == NULL || label.data == NULL) {
        return false;
    }
    if (json->completion_label_capacity == 0U ||
        (json->completion_label_count + 1U) * 10U >=
            json->completion_label_capacity * 7U) {
        if (!completion_labels_grow(json)) {
            return false;
        }
    }
    slot = completion_label_hash(label) & (json->completion_label_capacity - 1U);
    while (json->completion_labels[slot] != NULL) {
        const char *existing = json->completion_labels[slot];

        if (strlen(existing) == label.length &&
            memcmp(existing, label.data, label.length) == 0) {
            return true;
        }
        slot = (slot + 1U) & (json->completion_label_capacity - 1U);
    }
    copy = dup_range(label.data, label.data + label.length);
    if (copy == NULL) {
        return false;
    }
    json->completion_labels[slot] = copy;
    ++json->completion_label_count;
    ++json->completion_item_count;
    *out_append = true;
    return true;
}

static bool append_completion_item(FengLspString *json,
                                   bool *first,
                                   FengSlice label,
                                   const char *detail,
                                   int kind) {
    char *label_text;
    bool append;
    bool ok;

    if (!completion_label_register(json, label, &append)) {
        return false;
    }
    if (!append) {
        return true;
    }
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

/* Append a completion item with Snippet support (Phase 3).
 * Generates a CompletionItem with insertText and insertTextFormat: 2.
 * The insert_text string is JSON-escaped via string_append_json_string.
 * Returns true on success, false on allocation failure. */
static bool append_completion_item_snippet(FengLspString *json,
                                            bool *first,
                                            FengSlice label,
                                            const char *detail,
                                            int kind,
                                            const char *insert_text) {
    char *label_text;
    bool append;
    bool ok;

    if (!completion_label_register(json, label, &append)) {
        return false;
    }
    if (!append) {
        return true;
    }
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
    if (insert_text != NULL) {
        if (!string_append_cstr(json, ",\"insertText\":") ||
            !string_append_json_string(json, insert_text) ||
            !string_append_cstr(json, ",\"insertTextFormat\":2")) {
            return false;
        }
    }
    return string_append_cstr(json, "}");
}

/* Append keyword completion items for the given grammar position.
 * Iterates KW_TABLE[position] and emits one item per entry.
 * Items with snippet != NULL use append_completion_item_snippet (Phase 3),
 * items with snippet == NULL use append_completion_item (plain-text).
 * Returns true on success, false on allocation failure. */
static bool append_context_keyword_items(FengLspString *json,
                                         bool *first,
                                         FengLspPosition position) {
    const LspKwTable *table;
    size_t index;

    if (position <= FENG_LSP_POS_OTHER ||
        (size_t)position >= sizeof(KW_TABLE) / sizeof(KW_TABLE[0])) {
        return true;
    }
    table = &KW_TABLE[(size_t)position];
    if (table->items == NULL || table->count == 0U) {
        return true;
    }
    for (index = 0U; index < table->count; ++index) {
        const LspKwItem *item = &table->items[index];
        bool ok;

        if (item->snippet != NULL) {
            ok = append_completion_item_snippet(json,
                                                first,
                                                slice_from_cstr(item->label),
                                                item->detail,
                                                14,
                                                item->snippet);
        } else {
            ok = append_completion_item(json,
                                        first,
                                        slice_from_cstr(item->label),
                                        item->detail,
                                        14);
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Build a complete annotation completion JSON response.
 * Iterates BUILTIN_ANNOTATIONS and emits one item per entry.
 * When prefix is non-empty, only items whose label starts with the
 * prefix are included (server-side filtering for reliable behaviour).
 * Returns true on success, false on allocation failure. */
static bool build_annotation_completion_json(FengSlice prefix,
                                             FengLspString *json) {
    bool first = true;
    size_t index;

    if (!string_append_cstr(json, "[")) {
        return false;
    }
    for (index = 0U; index < BUILTIN_ANNOTATION_COUNT; ++index) {
        const LspAnnotationItem *item = &BUILTIN_ANNOTATIONS[index];

        /* Filter by prefix when the user has typed characters after '@'. */
        if (prefix.length > 0U) {
            if (strlen(item->label) < prefix.length ||
                strncmp(item->label, prefix.data, prefix.length) != 0) {
                continue;
            }
        }
        if (!append_completion_item(json,
                                    &first,
                                    slice_from_cstr(item->label),
                                    item->detail,
                                    14)) {
            return false;
        }
    }
    return string_append_cstr(json, "]");
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
    bool append;
    bool ok;

    if (!completion_label_register(json, label, &append)) {
        return false;
    }
    if (!append) {
        return true;
    }
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
    if (json->completion_label_capacity > 0U) {
        size_t slot = completion_label_hash(label) &
                      (json->completion_label_capacity - 1U);

        while (json->completion_labels[slot] != NULL) {
            const char *existing = json->completion_labels[slot];

            if (strlen(existing) == label.length &&
                memcmp(existing, label.data, label.length) == 0) {
                *contains = true;
                break;
            }
            slot = (slot + 1U) & (json->completion_label_capacity - 1U);
        }
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

/* Builds the displayed AST declaration label, including generic type names. */
static bool ast_decl_completion_label(const FengDecl *decl,
                                      FengLspString *storage,
                                      FengSlice *out_label) {
    const FengTypeParam *type_params = NULL;
    size_t type_param_count = 0U;
    size_t index;

    *out_label = decl_name(decl);
    if (decl->kind == FENG_DECL_TYPE) {
        type_params = decl->as.type_decl.type_params;
        type_param_count = decl->as.type_decl.type_param_count;
    } else if (decl->kind == FENG_DECL_SPEC) {
        type_params = decl->as.spec_decl.type_params;
        type_param_count = decl->as.spec_decl.type_param_count;
    }
    if (type_param_count == 0U) {
        return true;
    }
    if (!string_append_bytes(storage, out_label->data, out_label->length) ||
        !string_append_cstr(storage, "<")) {
        return false;
    }
    for (index = 0U; index < type_param_count; ++index) {
        if ((index > 0U && !string_append_cstr(storage, ", ")) ||
            !string_append_bytes(storage,
                                 type_params[index].name.data,
                                 type_params[index].name.length)) {
            return false;
        }
    }
    if (!string_append_cstr(storage, ">")) {
        return false;
    }
    out_label->data = storage->data;
    out_label->length = storage->length;
    return true;
}

static bool append_decl_completion_item(FengLspString *json,
                                        bool *first,
                                        const FengDecl *decl,
                                        const char *detail,
                                        int forced_kind,
                                        const FengLspRequestContext *request) {
    FengLspString signature = {0};
    FengLspString generic_label = {0};
    FengSlice label;
    const char *item_detail = detail;
    bool ok;

    if (decl == NULL || decl->kind == FENG_DECL_FIT) {
        return true;
    }
    if (item_detail == NULL && decl_signature_to_string(&signature, decl)) {
        item_detail = signature.data;
    }
    if (!ast_decl_completion_label(decl, &generic_label, &label)) {
        string_dispose(&generic_label);
        string_dispose(&signature);
        return false;
    }
    ok = append_completion_item_with_data(json,
                                          first,
                                          label,
                                          item_detail,
                                          forced_kind > 0 ? forced_kind : completion_kind_for_decl(decl),
                                          request != NULL ? request->uri : NULL,
                                          NULL);
    string_dispose(&generic_label);
    string_dispose(&signature);
    return ok;
}

static bool append_member_completion_item(FengLspString *json,
                                          bool *first,
                                          const FengTypeMember *member,
                                          const char *owner_name,
                                          const FengLspRequestContext *request) {
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
                                          request != NULL ? request->uri : NULL,
                                          owner_name);
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
    if (session->source_module_index != NULL) {
        for (source_index = 0U;
             source_index < session->source_module_index->module_count;
             ++source_index) {
            const FengProgram *program =
                session->source_module_index->modules[source_index].program;

            if (program == NULL) {
                continue;
            }
            for (decl_index = 0U;
                 decl_index < program->declaration_count;
                 ++decl_index) {
                if (program->declarations[decl_index] == decl) {
                    return program;
                }
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

/* Pointer-identity set used while traversing source spec parent graphs. */
typedef struct FengLspSourceDeclSet {
    const FengDecl **items;
    size_t count;
    size_t capacity;
} FengLspSourceDeclSet;

/* Insert one source declaration identity into a traversal set. */
static bool source_decl_set_insert(FengLspSourceDeclSet *set,
                                   const FengDecl *decl,
                                   bool *out_inserted) {
    size_t index;

    if (set == NULL || decl == NULL || out_inserted == NULL) {
        return false;
    }
    for (index = 0U; index < set->count; ++index) {
        if (set->items[index] == decl) {
            *out_inserted = false;
            return true;
        }
    }
    if (set->count == set->capacity) {
        size_t capacity = set->capacity == 0U ? 8U : set->capacity * 2U;
        const FengDecl **items =
            (const FengDecl **)realloc(set->items, capacity * sizeof(*items));

        if (items == NULL) {
            return false;
        }
        set->items = items;
        set->capacity = capacity;
    }
    set->items[set->count++] = decl;
    *out_inserted = true;
    return true;
}

/* Check whether a source spec is the requested spec or inherits it. */
static bool source_spec_reaches(const FengLspAnalysisSession *session,
                                const FengDecl *head_spec,
                                const FengDecl *requested_spec,
                                FengLspSourceDeclSet *visited) {
    const FengProgram *head_program;
    bool inserted;
    size_t index;

    if (head_spec == requested_spec) {
        return true;
    }
    if (head_spec == NULL || requested_spec == NULL ||
        head_spec->kind != FENG_DECL_SPEC ||
        !source_decl_set_insert(visited, head_spec, &inserted) || !inserted) {
        return false;
    }
    head_program = find_decl_owner_program_in_session(session, head_spec);
    if (head_program == NULL) {
        return false;
    }
    for (index = 0U;
         index < head_spec->as.spec_decl.parent_spec_count;
         ++index) {
        const FengDecl *parent = resolve_named_type_ref(
            session, head_program, head_spec->as.spec_decl.parent_specs[index]);

        if (source_spec_reaches(session, parent, requested_spec, visited)) {
            return true;
        }
    }
    return false;
}

/* Check a source type or fit declaration's spec heads and parent closure. */
static bool source_decl_declares_spec(const FengLspAnalysisSession *session,
                                      const FengDecl *decl,
                                      const FengDecl *requested_spec) {
    const FengProgram *decl_program;
    const FengTypeRef *const *specs = NULL;
    size_t spec_count = 0U;
    FengLspSourceDeclSet visited = {0};
    size_t index;
    bool found = false;

    if (decl == NULL || requested_spec == NULL) {
        return false;
    }
    if (decl->kind == FENG_DECL_TYPE) {
        specs = (const FengTypeRef *const *)decl->as.type_decl.declared_specs;
        spec_count = decl->as.type_decl.declared_spec_count;
    } else if (decl->kind == FENG_DECL_FIT) {
        specs = (const FengTypeRef *const *)decl->as.fit_decl.specs;
        spec_count = decl->as.fit_decl.spec_count;
    } else {
        return false;
    }
    decl_program = find_decl_owner_program_in_session(session, decl);
    if (decl_program == NULL) {
        return false;
    }
    for (index = 0U; index < spec_count; ++index) {
        const FengDecl *head = resolve_named_type_ref(session,
                                                      decl_program,
                                                      specs[index]);

        if (source_spec_reaches(session, head, requested_spec, &visited)) {
            found = true;
            break;
        }
    }
    free(visited.items);
    return found;
}

/* Return whether the consumer imports a source program's module. */
static bool source_module_visible_from_program(const FengProgram *consumer,
                                               const FengProgram *provider) {
    size_t index;

    if (consumer == NULL || provider == NULL) {
        return false;
    }
    if (program_module_matches(provider,
                               consumer->module_segments,
                               consumer->module_segment_count)) {
        return true;
    }
    for (index = 0U; index < consumer->use_count; ++index) {
        if (program_module_matches(provider,
                                   consumer->uses[index].segments,
                                   consumer->uses[index].segment_count)) {
            return true;
        }
    }
    return false;
}

/* Check visible fit relations contributed by one parsed source program. */
static bool source_program_fit_satisfies_spec(
    const FengLspAnalysisSession *session,
    const FengProgram *consumer_program,
    const FengProgram *fit_program,
    const FengDecl *type_decl,
    const FengDecl *requested_spec) {
    size_t index;

    if (!source_module_visible_from_program(consumer_program, fit_program)) {
        return false;
    }
    for (index = 0U; index < fit_program->declaration_count; ++index) {
        const FengDecl *fit_decl = fit_program->declarations[index];
        const FengDecl *fit_target;
        bool same_module;

        if (fit_decl == NULL || fit_decl->kind != FENG_DECL_FIT) {
            continue;
        }
        same_module = program_module_matches(
            fit_program,
            consumer_program->module_segments,
            consumer_program->module_segment_count);
        if (!same_module && fit_decl->visibility != FENG_VISIBILITY_PUBLIC) {
            continue;
        }
        fit_target = resolve_named_type_ref(session,
                                            fit_program,
                                            fit_decl->as.fit_decl.target);
        if (fit_target == type_decl &&
            source_decl_declares_spec(session, fit_decl, requested_spec)) {
            return true;
        }
    }
    return false;
}

/* Check the parsed nominal relation for a type, including visible fits. */
static bool source_type_satisfies_spec_from_program(
    const FengLspAnalysisSession *session,
    const FengProgram *consumer_program,
    const FengDecl *type_decl,
    const FengDecl *requested_spec) {
    size_t index;

    if (session == NULL || consumer_program == NULL || type_decl == NULL ||
        type_decl->kind != FENG_DECL_TYPE || requested_spec == NULL ||
        requested_spec->kind != FENG_DECL_SPEC) {
        return false;
    }
    if (source_decl_declares_spec(session, type_decl, requested_spec)) {
        return true;
    }
    for (index = 0U; index < session->source_count; ++index) {
        if (source_program_fit_satisfies_spec(session,
                                              consumer_program,
                                              session->sources[index].program,
                                              type_decl,
                                              requested_spec)) {
            return true;
        }
    }
    if (session->source_module_index != NULL) {
        for (index = 0U;
             index < session->source_module_index->module_count;
             ++index) {
            if (source_program_fit_satisfies_spec(
                    session,
                    consumer_program,
                    session->source_module_index->modules[index].program,
                    type_decl,
                    requested_spec)) {
                return true;
            }
        }
    }
    return false;
}

/* Return whether at least one relation source is visible from this program's
 * module and imports, matching the semantic analyzer's fit visibility rule. */
static bool spec_relation_visible_from_program(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengSpecRelation *relation) {
    const FengSemanticModule *consumer_module;

    if (session == NULL || session->analysis == NULL || program == NULL ||
        relation == NULL) {
        return false;
    }
    consumer_module = find_program_module(session, program);
    for (size_t source_index = 0U;
         source_index < relation->source_count;
         ++source_index) {
        const FengSpecRelationSource *source =
            &relation->sources[source_index];

        if (feng_semantic_spec_relation_source_visible_from(
                source, consumer_module, NULL, 0U)) {
            return true;
        }
        for (size_t use_index = 0U;
             use_index < program->use_count;
             ++use_index) {
            const FengUseDecl *use_decl = &program->uses[use_index];
            const FengSemanticModule *imported = find_module_by_segments(
                session->analysis,
                use_decl->segments,
                use_decl->segment_count);

            if (imported != NULL &&
                feng_semantic_spec_relation_source_visible_from(
                    source, consumer_module, &imported, 1U)) {
                return true;
            }
        }
    }
    return false;
}

/* Check spec-seal completion visibility from one enclosing type/fit method
 * without reinterpreting the visibility of any concrete type member. */
static bool spec_seal_member_visible_from_implementation(
    const FengLspAnalysisSession *session,
    const FengProgram *program,
    const FengDecl *owner_spec,
    const FengDecl *enclosing_decl,
    const FengTypeMember *enclosing_member) {
    const FengDecl *implementation_type = NULL;
    const FengSpecRelation *relation;
    FengSemanticSubjectKey subject_key;

    if (session == NULL || program == NULL ||
        owner_spec == NULL || owner_spec->kind != FENG_DECL_SPEC ||
        enclosing_member == NULL ||
        enclosing_member->kind != FENG_TYPE_MEMBER_METHOD) {
        return false;
    }
    if (enclosing_decl != NULL && enclosing_decl->kind == FENG_DECL_TYPE) {
        implementation_type = enclosing_decl;
    } else if (enclosing_decl != NULL &&
               enclosing_decl->kind == FENG_DECL_FIT) {
        implementation_type = resolve_named_type_ref(
            session, program, enclosing_decl->as.fit_decl.target);
    }
    if (implementation_type == NULL ||
        implementation_type->kind != FENG_DECL_TYPE) {
        return false;
    }
    if (session->analysis == NULL) {
        return source_type_satisfies_spec_from_program(session,
                                                       program,
                                                       implementation_type,
                                                       owner_spec);
    }
    subject_key = feng_semantic_subject_key_for_type_decl(implementation_type);
    relation = feng_semantic_lookup_spec_relation(session->analysis,
                                                  &subject_key,
                                                  owner_spec);
    return spec_relation_visible_from_program(session, program, relation);
}

static bool symbol_spec_seal_member_visible_from_implementation(
    const FengLspCacheQueryContext *context,
    const FengSymbolDeclView *owner_spec,
    const FengDecl *enclosing_decl,
    const FengTypeMember *enclosing_member);

/* Apply ordinary type visibility or the implementation-domain rule for an
 * object-spec member shown by source-backed completion. */
static bool type_member_visible_from_program(const FengLspAnalysisSession *session,
                                             const FengProgram *program,
                                             const FengDecl *owner_decl,
                                             const FengTypeMember *member,
                                             const FengDecl *enclosing_decl,
                                             const FengTypeMember *enclosing_member,
                                             const FengLspCacheQueryContext *cache_context) {
    if (member == NULL) {
        return false;
    }
    if (owner_decl != NULL && owner_decl->kind == FENG_DECL_SPEC) {
        const FengSymbolDeclView *owner_symbol;

        if (member->visibility != FENG_VISIBILITY_PRIVATE ||
            spec_seal_member_visible_from_implementation(
                session,
                program,
                owner_decl,
                enclosing_decl,
                enclosing_member)) {
            return true;
        }
        owner_symbol = cache_context != NULL
                           ? match_ast_decl_to_symbol(cache_context->current_module,
                                                      cache_context->program,
                                                      owner_decl)
                           : NULL;
        return symbol_spec_seal_member_visible_from_implementation(
            cache_context,
            owner_symbol,
            enclosing_decl,
            enclosing_member);
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

/* Pointer-identity set used while traversing cached spec parent graphs. */
typedef struct FengLspSymbolDeclSet {
    const FengSymbolDeclView **items;
    size_t count;
    size_t capacity;
} FengLspSymbolDeclSet;

/* Insert one declaration identity into a small traversal set. */
static bool symbol_decl_set_insert(FengLspSymbolDeclSet *set,
                                   const FengSymbolDeclView *decl,
                                   bool *out_inserted) {
    size_t index;

    if (set == NULL || decl == NULL || out_inserted == NULL) {
        return false;
    }
    for (index = 0U; index < set->count; ++index) {
        if (set->items[index] == decl) {
            *out_inserted = false;
            return true;
        }
    }
    if (set->count == set->capacity) {
        size_t capacity = set->capacity == 0U ? 8U : set->capacity * 2U;
        const FengSymbolDeclView **items =
            (const FengSymbolDeclView **)realloc(set->items,
                                                capacity * sizeof(*items));

        if (items == NULL) {
            return false;
        }
        set->items = items;
        set->capacity = capacity;
    }
    set->items[set->count++] = decl;
    *out_inserted = true;
    return true;
}

/* Resolve a symbol type to its declaration, preferring its bound identity. */
static const FengSymbolDeclView *symbol_type_target_from_context(
    const FengLspCacheQueryContext *context,
    const FengSymbolTypeView *type) {
    const FengSymbolDeclView *target;

    if (context == NULL || type == NULL) {
        return NULL;
    }
    target = feng_symbol_type_target_decl(type);
    if (target != NULL) {
        return target;
    }
    return resolve_symbol_type_view(context->provider,
                                    context->current_module,
                                    context->program,
                                    type);
}

/* Check whether a declared spec is the requested spec or inherits it. */
static bool symbol_spec_reaches(const FengLspCacheQueryContext *context,
                                const FengSymbolDeclView *head_spec,
                                const FengSymbolDeclView *requested_spec,
                                FengLspSymbolDeclSet *visited) {
    bool inserted;
    size_t index;

    if (head_spec == requested_spec) {
        return true;
    }
    if (head_spec == NULL || requested_spec == NULL ||
        feng_symbol_decl_kind(head_spec) != FENG_SYMBOL_DECL_KIND_SPEC ||
        !symbol_decl_set_insert(visited, head_spec, &inserted) || !inserted) {
        return false;
    }
    for (index = 0U;
         index < feng_symbol_decl_declared_spec_count(head_spec);
         ++index) {
        const FengSymbolDeclView *parent = symbol_type_target_from_context(
            context, feng_symbol_decl_declared_spec_at(head_spec, index));

        if (symbol_spec_reaches(context, parent, requested_spec, visited)) {
            return true;
        }
    }
    return false;
}

/* Check a type/spec declaration's declared spec heads and parent closure. */
static bool symbol_decl_declares_spec(const FengLspCacheQueryContext *context,
                                      const FengSymbolDeclView *decl,
                                      const FengSymbolDeclView *requested_spec) {
    FengLspSymbolDeclSet visited = {0};
    size_t index;
    bool found = false;

    if (decl == NULL || requested_spec == NULL) {
        return false;
    }
    for (index = 0U;
         index < feng_symbol_decl_declared_spec_count(decl);
         ++index) {
        const FengSymbolDeclView *head = symbol_type_target_from_context(
            context, feng_symbol_decl_declared_spec_at(decl, index));

        if (symbol_spec_reaches(context, head, requested_spec, &visited)) {
            found = true;
            break;
        }
    }
    free(visited.items);
    return found;
}

/* Return whether a symbol module is the current module or explicitly used. */
static bool symbol_module_visible_from_program(
    const FengLspCacheQueryContext *context,
    const FengSymbolImportedModule *module) {
    size_t index;

    if (context == NULL || module == NULL) {
        return false;
    }
    if (module == context->current_module) {
        return true;
    }
    for (index = 0U; index < context->program->use_count; ++index) {
        const FengUseDecl *use_decl = &context->program->uses[index];

        if (feng_symbol_provider_find_module(context->provider,
                                             use_decl->segments,
                                             use_decl->segment_count) == module) {
            return true;
        }
    }
    return false;
}

/* Check the published nominal relation for a type, including visible fits. */
static bool symbol_type_satisfies_spec_from_context(
    const FengLspCacheQueryContext *context,
    const FengSymbolDeclView *type_decl,
    const FengSymbolDeclView *requested_spec) {
    size_t module_index;

    if (context == NULL || type_decl == NULL || requested_spec == NULL ||
        feng_symbol_decl_kind(type_decl) != FENG_SYMBOL_DECL_KIND_TYPE ||
        feng_symbol_decl_kind(requested_spec) != FENG_SYMBOL_DECL_KIND_SPEC) {
        return false;
    }
    if (symbol_decl_declares_spec(context, type_decl, requested_spec)) {
        return true;
    }
    for (module_index = 0U;
         module_index < feng_symbol_provider_module_count(context->provider);
         ++module_index) {
        const FengSymbolImportedModule *module =
            feng_symbol_provider_module_at(context->provider, module_index);
        size_t fit_index;

        if (!symbol_module_visible_from_program(context, module)) {
            continue;
        }
        for (fit_index = 0U;
             fit_index < feng_symbol_module_fit_count(module);
             ++fit_index) {
            const FengSymbolDeclView *fit_decl = feng_symbol_fit_decl(
                feng_symbol_module_fit_at(module, fit_index));
            const FengSymbolDeclView *fit_target;

            if (fit_decl == NULL ||
                (module != context->current_module &&
                 feng_symbol_decl_visibility(fit_decl) != FENG_VISIBILITY_PUBLIC)) {
                continue;
            }
            fit_target = symbol_type_target_from_context(
                context, feng_symbol_decl_fit_target(fit_decl));
            if (fit_target == type_decl &&
                symbol_decl_declares_spec(context, fit_decl, requested_spec)) {
                return true;
            }
        }
    }
    return false;
}

/* Check spec-seal visibility from a cached enclosing type/fit method. */
static bool symbol_spec_seal_member_visible_from_implementation(
    const FengLspCacheQueryContext *context,
    const FengSymbolDeclView *owner_spec,
    const FengDecl *enclosing_decl,
    const FengTypeMember *enclosing_member) {
    const FengSymbolDeclView *implementation_type = NULL;

    if (context == NULL || owner_spec == NULL || enclosing_decl == NULL ||
        enclosing_member == NULL ||
        enclosing_member->kind != FENG_TYPE_MEMBER_METHOD) {
        return false;
    }
    if (enclosing_decl->kind == FENG_DECL_TYPE) {
        implementation_type = match_ast_decl_to_symbol(context->current_module,
                                                       context->program,
                                                       enclosing_decl);
    } else if (enclosing_decl->kind == FENG_DECL_FIT) {
        const FengSymbolDeclView *fit_decl = match_ast_decl_to_symbol(
            context->current_module, context->program, enclosing_decl);

        if (fit_decl != NULL) {
            implementation_type = symbol_type_target_from_context(
                context, feng_symbol_decl_fit_target(fit_decl));
        }
    }
    return symbol_type_satisfies_spec_from_context(context,
                                                   implementation_type,
                                                   owner_spec);
}

/* Apply module visibility or the spec implementation-domain rule. */
static bool symbol_member_visible_from_context(
    const FengLspCacheQueryContext *context,
    const FengSymbolDeclView *owner_decl,
    const FengSymbolDeclView *member,
    const FengDecl *enclosing_decl,
    const FengTypeMember *enclosing_member) {
    if (member == NULL) {
        return false;
    }
    if (feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PUBLIC) {
        return true;
    }
    if (owner_decl != NULL &&
        feng_symbol_decl_kind(owner_decl) == FENG_SYMBOL_DECL_KIND_SPEC) {
        return symbol_spec_seal_member_visible_from_implementation(
            context, owner_decl, enclosing_decl, enclosing_member);
    }
    return symbol_decl_is_in_module(context->current_module, owner_decl);
}

/* Build a completion label for a symbol decl.
 * For generic type/spec decls, appends type parameter names:
 *   "Box<T>", "Box<T, U>", "Reader<T>", etc.
 * For non-generic decls, returns a copy of the plain name.
 * Caller must free the returned slice data. */
static FengSlice build_symbol_decl_completion_label(const FengSymbolDeclView *decl) {
    FengSlice name;
    FengSymbolDeclKind kind;
    size_t type_param_count;
    FengLspString label = {0};
    size_t mi;
    size_t param_index;

    if (decl == NULL) {
        return (FengSlice){0};
    }
    name = feng_symbol_decl_name(decl);
    kind = feng_symbol_decl_kind(decl);
    type_param_count = feng_symbol_decl_type_param_count(decl);
    if (type_param_count == 0U ||
        (kind != FENG_SYMBOL_DECL_KIND_TYPE && kind != FENG_SYMBOL_DECL_KIND_SPEC)) {
        char *copy = dup_range(name.data, name.data + name.length);
        if (copy == NULL) {
            return (FengSlice){0};
        }
        return (FengSlice){copy, name.length};
    }
    if (!string_append_bytes(&label, name.data, name.length) ||
        !string_append_cstr(&label, "<")) {
        string_dispose(&label);
        return (FengSlice){0};
    }
    for (mi = 0U, param_index = 0U; mi < feng_symbol_decl_member_count(decl); ++mi) {
        const FengSymbolDeclView *m = feng_symbol_decl_member_at(decl, mi);
        FengSlice pname;

        if (m == NULL || feng_symbol_decl_kind(m) != FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
            continue;
        }
        if (param_index > 0U && !string_append_cstr(&label, ", ")) {
            string_dispose(&label);
            return (FengSlice){0};
        }
        pname = feng_symbol_decl_name(m);
        if (!string_append_bytes(&label, pname.data, pname.length)) {
            string_dispose(&label);
            return (FengSlice){0};
        }
        ++param_index;
    }
    if (!string_append_cstr(&label, ">")) {
        string_dispose(&label);
        return (FengSlice){0};
    }
    {
        FengSlice result = {label.data, label.length};
        return result;
    }
}

/* Extract the base name from a completion label that may contain an arity
 * suffix "<...>".  Returns a slice pointing into `label` (no allocation).
 * Examples: "Box<T>" -> "Box", "Box<T, U>" -> "Box", "foo" -> "foo". */
static FengSlice strip_arity_suffix(FengSlice label) {
    size_t i;

    for (i = 0U; i < label.length; ++i) {
        if (label.data[i] == '<') {
            return (FengSlice){label.data, i};
        }
    }
    return label;
}

static bool append_symbol_decl_completion_item_with_label(FengLspString *json,
                                                          bool *first,
                                                          const FengSymbolDeclView *decl,
                                                          FengSlice label,
                                                          const char *detail,
                                                          int forced_kind,
                                                          const FengLspRequestContext *request) {
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
                                          label,
                                          item_detail,
                                          forced_kind > 0 ? forced_kind : kind,
                                          request != NULL ? request->uri : NULL,
                                          NULL);
    string_dispose(&signature);
    return ok;
}

static size_t count_symbol_member_overloads(const FengSymbolDeclView *owner_decl,
                                            FengSlice name) {
    size_t count = 0U;
    size_t i;
    size_t member_count;

    if (owner_decl == NULL) {
        return 0U;
    }
    member_count = feng_symbol_decl_member_count(owner_decl);
    for (i = 0U; i < member_count; ++i) {
        const FengSymbolDeclView *m = feng_symbol_decl_member_at(owner_decl, i);

        if (slice_equals(feng_symbol_decl_name(m), name)) {
            ++count;
        }
    }
    return count;
}

static bool append_symbol_member_completion_item(FengLspString *json,
                                                 bool *first,
                                                 const FengSymbolDeclView *member,
                                                 const char *owner_name,
                                                 size_t overload_count,
                                                 const FengLspRequestContext *request) {
    FengLspString signature = {0};
    int kind;
    bool ok;

    if (!symbol_decl_is_instance_member(member)) {
        return true;
    }
    kind = feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD
               ? 5
               : feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_ENUM_ITEM ? 20 : 2;
    if (!symbol_member_signature_to_string(&signature, member)) {
        return false;
    }
    if (overload_count > 1U) {
        (void)string_append_format(&signature, " (+%zu overloads)", overload_count - 1U);
    }
    ok = append_completion_item_with_data(json, first, feng_symbol_decl_name(member),
                                          signature.data, kind,
                                          request != NULL ? request->uri : NULL,
                                          owner_name);
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
                                                 int forced_kind,
                                                 const FengLspRequestContext *request) {
    size_t decl_index;

    if (program == NULL) {
        return true;
    }
    for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];

        if (decl->kind == FENG_DECL_FIT || (public_only && decl->visibility != FENG_VISIBILITY_PUBLIC)) {
            continue;
        }
        if (!append_decl_completion_item(json,
                                         first,
                                         decl,
                                         detail,
                                         forced_kind,
                                         request)) {
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
                                                    int forced_kind,
                                                    const FengLspRequestContext *request) {
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
                                                  forced_kind,
                                                  request)) {
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
                                                  int forced_kind,
                                                  const FengLspRequestContext *request) {
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
                                                  forced_kind,
                                                  request)) {
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
                                                 const FengDecl *enclosing_decl,
                                                 const FengTypeMember *enclosing_member,
                                                 FengLspMemberFilter filter,
                                                 const FengLspCacheQueryContext *cache_context,
                                                 const FengLspRequestContext *request) {
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

                if (!type_member_visible_from_program(session,
                                                      program,
                                                      owner_decl,
                                                      member,
                                                      enclosing_decl,
                                                      enclosing_member,
                                                      cache_context)) {
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
                if (!append_member_completion_item(json, first, member, owner_str, request)) {
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

                if (!type_member_visible_from_program(session,
                                                      program,
                                                      owner_decl,
                                                      member,
                                                      enclosing_decl,
                                                      enclosing_member,
                                                      cache_context)) {
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
                if (!append_member_completion_item(json, first, member, owner_str, request)) {
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

/* Returns whether a semantic snapshot contains a source module path. */
static bool session_contains_module_program(const FengLspAnalysisSession *session,
                                            const FengSlice *segments,
                                            size_t segment_count) {
    size_t source_index;

    if (session == NULL || segments == NULL || segment_count == 0U) {
        return false;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        if (program_module_matches(session->sources[source_index].program,
                                   segments,
                                   segment_count)) {
            return true;
        }
    }
    return false;
}

/* Appends declarations from an already-loaded aliased module. */
static bool append_alias_module_completion_items(FengLspString *json,
                                                 bool *first,
                                                 const FengLspAnalysisSession *session,
                                                 const FengProgram *program,
                                                 FengSlice alias_name,
                                                 bool *handled,
                                                 const FengLspRequestContext *request) {
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
        module = find_module_by_segments(session->analysis,
                                         use_decl->segments,
                                         use_decl->segment_count);
        if (module != NULL) {
            return append_semantic_module_completion_items(json,
                                                           first,
                                                           module,
                                                           true,
                                                           NULL,
                                                           -1,
                                                           request);
        }
        if (session_contains_module_program(session,
                                            use_decl->segments,
                                            use_decl->segment_count)) {
            return append_loaded_module_completion_items(json,
                                                         first,
                                                         session,
                                                         use_decl->segments,
                                                         use_decl->segment_count,
                                                         true,
                                                         NULL,
                                                         -1,
                                                         request);
        }
        return true;
    }
    return true;
}

/* Extracts the already-typed and partial segments of an import path. */
static bool extract_use_path_context(const char *text,
                                     size_t offset,
                                     FengSlice *prefix_segments,
                                     size_t *prefix_count,
                                     size_t max_prefix,
                                     FengSlice *partial) {
    FengSlice reverse_segments[16];
    size_t position = offset;
    size_t reverse_count = 0U;
    size_t segment_end;
    size_t index;

    *prefix_count = 0U;
    partial->data = NULL;
    partial->length = 0U;
    if (text == NULL) {
        return false;
    }
    segment_end = position;
    while (position > 0U && completion_identifier_continue(text[position - 1U])) {
        --position;
    }
    partial->data = text + position;
    partial->length = segment_end - position;
    while (position > 0U && text[position - 1U] == '.') {
        size_t segment_start;

        --position;
        segment_end = position;
        while (position > 0U && completion_identifier_continue(text[position - 1U])) {
            --position;
        }
        segment_start = position;
        if (segment_end == segment_start) {
            break;
        }
        if (reverse_count < 16U) {
            reverse_segments[reverse_count].data = text + segment_start;
            reverse_segments[reverse_count].length = segment_end - segment_start;
            ++reverse_count;
        }
    }
    while (position > 0U && (text[position - 1U] == ' ' || text[position - 1U] == '\t')) {
        --position;
    }
    if (position < 6U || memcmp(text + position - 6U, "import", 6U) != 0 ||
        (position > 6U && completion_identifier_continue(text[position - 7U]))) {
        return false;
    }
    {
        size_t count = reverse_count < max_prefix ? reverse_count : max_prefix;

        for (index = 0U; index < count; ++index) {
            prefix_segments[index] = reverse_segments[count - 1U - index];
        }
        *prefix_count = count;
    }
    return true;
}

/* Appends a module segment once per completion response. */
static bool append_seen_module_completion_item(FengLspString *json,
                                               bool *first,
                                               FengSlice *seen,
                                               size_t *seen_count,
                                               size_t seen_capacity,
                                               FengSlice next_segment) {
    size_t index;

    for (index = 0U; index < *seen_count; ++index) {
        if (slice_equals(seen[index], next_segment)) {
            return true;
        }
    }
    if (*seen_count < seen_capacity) {
        seen[(*seen_count)++] = next_segment;
    }
    return append_completion_item(json, first, next_segment, "module", 9);
}

/* Appends import-path segments from the persistent symbol index. */
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
    for (module_index = 0U;
         module_index < feng_symbol_provider_module_count(provider);
         ++module_index) {
        const FengSymbolImportedModule *module =
            feng_symbol_provider_module_at(provider, module_index);
        FengSlice next_segment;
        size_t index;

        if (feng_symbol_module_segment_count(module) <= prefix_count) {
            continue;
        }
        for (index = 0U; index < prefix_count; ++index) {
            if (!slice_equals(feng_symbol_module_segment_at(module, index),
                              prefix_segments[index])) {
                break;
            }
        }
        if (index < prefix_count) {
            continue;
        }
        next_segment = feng_symbol_module_segment_at(module, prefix_count);
        if (partial.length > 0U &&
            (next_segment.length < partial.length ||
             memcmp(next_segment.data, partial.data, partial.length) != 0)) {
            continue;
        }
        if (!append_seen_module_completion_item(json,
                                                first,
                                                seen,
                                                seen_count,
                                                seen_capacity,
                                                next_segment)) {
            return false;
        }
    }
    return true;
}

/* Builds import-path Completion exclusively from published in-memory indexes. */
static bool build_persistent_use_path_completion_json(const FengLspService *service,
                                                      const char *source_text,
                                                      size_t offset,
                                                      FengLspString *json) {
    FengSlice prefix_segments[16];
    size_t prefix_count = 0U;
    FengSlice partial = {0};
    FengSlice seen[64];
    size_t seen_count = 0U;
    size_t module_index;
    bool first = true;

    if (service == NULL ||
        !extract_use_path_context(source_text,
                                  offset,
                                  prefix_segments,
                                  &prefix_count,
                                  16U,
                                  &partial) ||
        !string_append_cstr(json, "[")) {
        return false;
    }
    for (module_index = 0U;
         module_index < service->module_index.module_count;
         ++module_index) {
        const FengLspIndexedModule *module = &service->module_index.modules[module_index];
        FengSlice next_segment;
        size_t segment_index;

        if (module->segment_count <= prefix_count) {
            continue;
        }
        for (segment_index = 0U; segment_index < prefix_count; ++segment_index) {
            if (!slice_equals_cstr(prefix_segments[segment_index],
                                   module->segments[segment_index])) {
                break;
            }
        }
        if (segment_index < prefix_count) {
            continue;
        }
        next_segment = slice_from_cstr(module->segments[prefix_count]);
        if (partial.length > 0U &&
            (next_segment.length < partial.length ||
             memcmp(next_segment.data, partial.data, partial.length) != 0)) {
            continue;
        }
        if (!append_seen_module_completion_item(json,
                                                &first,
                                                seen,
                                                &seen_count,
                                                64U,
                                                next_segment)) {
            return false;
        }
    }
    if (!append_provider_use_path_completion_items(json,
                                                   &first,
                                                   service->symbol_index,
                                                   prefix_segments,
                                                   prefix_count,
                                                   partial,
                                                   seen,
                                                   &seen_count,
                                                   64U) ||
        !string_append_cstr(json, "]")) {
        return false;
    }
    return !first;
}

/* Merges public declarations from imported workspace source modules. */
static bool append_module_index_imports(const FengLspService *service,
                                        const FengProgram *program,
                                        const char *source_text,
                                        size_t offset,
                                        FengLspString *json,
                                        const FengLspRequestContext *request) {
    FengLspCompletionContext completion_context = {0};
    bool first;
    size_t use_index;

    if (service == NULL || program == NULL || json == NULL || json->data == NULL ||
        json->length < 2U || json->data[json->length - 1U] != ']') {
        return false;
    }
    --json->length;
    json->data[json->length] = '\0';
    first = json->length == 1U;
    (void)completion_context_from_text(source_text, offset, &completion_context);
    for (use_index = 0U; use_index < program->use_count; ++use_index) {
        const FengUseDecl *use_decl = &program->uses[use_index];
        size_t module_index;

        if (use_decl->has_alias &&
            (!completion_context.is_member ||
             !slice_equals(use_decl->alias, completion_context.object))) {
            continue;
        }
        if (!use_decl->has_alias && completion_context.is_member) {
            continue;
        }
        for (module_index = 0U;
             module_index < service->module_index.module_count;
             ++module_index) {
            const FengProgram *indexed_program =
                service->module_index.modules[module_index].program;
            size_t decl_index;

            if (!program_module_matches(indexed_program,
                                        use_decl->segments,
                                        use_decl->segment_count)) {
                continue;
            }
            for (decl_index = 0U;
                 decl_index < indexed_program->declaration_count;
                 ++decl_index) {
                const FengDecl *decl = indexed_program->declarations[decl_index];
                FengLspString label_storage = {0};
                FengSlice label;
                bool contains = false;

                if (decl->kind == FENG_DECL_FIT ||
                    decl->visibility != FENG_VISIBILITY_PUBLIC) {
                    continue;
                }
                if (!ast_decl_completion_label(decl, &label_storage, &label) ||
                    !completion_json_contains_label(json, label, &contains)) {
                    string_dispose(&label_storage);
                    return false;
                }
                if (!contains &&
                    !append_decl_completion_item(json,
                                                 &first,
                                                 decl,
                                                 NULL,
                                                 -1,
                                                 request)) {
                    string_dispose(&label_storage);
                    return false;
                }
                string_dispose(&label_storage);
            }
        }
    }
    return string_append_cstr(json, "]");
}

/* Reports whether every import is represented by either published index.
 * The caller holds analysis_mutex. */
static bool published_indexes_cover_program_imports(const FengLspService *service,
                                                     const FengProgram *program) {
    size_t use_index;

    if (service == NULL || program == NULL || program->use_count == 0U) {
        return true;
    }
    for (use_index = 0U; use_index < program->use_count; ++use_index) {
        const FengUseDecl *use_decl = &program->uses[use_index];
        size_t module_index;
        bool found = false;

        for (module_index = 0U;
             module_index < service->module_index.module_count;
             ++module_index) {
            if (program_module_matches(service->module_index.modules[module_index].program,
                                       use_decl->segments,
                                       use_decl->segment_count)) {
                found = true;
                break;
            }
        }
        if (!found && service->symbol_index != NULL &&
            feng_symbol_provider_find_module(service->symbol_index,
                                             use_decl->segments,
                                             use_decl->segment_count) != NULL) {
            found = true;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/* Waits only for the cold index kind needed by a program's imports. */
static void wait_for_program_import_indexes(FengLspService *service,
                                            const FengProgram *program) {
    struct timespec deadline;
    int wait_status = 0;

    if (service == NULL || program == NULL || program->use_count == 0U ||
        clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return;
    }
    deadline.tv_nsec += 16L * 1000L * 1000L;
    if (deadline.tv_nsec >= 1000L * 1000L * 1000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000L * 1000L * 1000L;
    }
    pthread_mutex_lock(&service->analysis_mutex);
    while (!published_indexes_cover_program_imports(service, program) &&
           service->latest_scheduled_generation > 0U &&
           wait_status != ETIMEDOUT) {
        wait_status = pthread_cond_timedwait(&service->analysis_condition,
                                             &service->analysis_mutex,
                                             &deadline);
    }
    pthread_mutex_unlock(&service->analysis_mutex);
}

/* Appends import-path segments from an in-memory semantic snapshot. */
static bool append_use_path_completion_items(FengLspString *json,
                                             bool *first,
                                             const FengLspAnalysisSession *session,
                                             const FengSlice *prefix_segments,
                                             size_t prefix_count,
                                             FengSlice partial) {
    FengSlice seen[64];
    size_t seen_count = 0U;
    size_t source_index;

    if (session == NULL) {
        return true;
    }
    for (source_index = 0U; source_index < session->source_count; ++source_index) {
        const FengProgram *source_program = session->sources[source_index].program;
        FengSlice next_segment;
        size_t index;

        if (source_program == NULL ||
            source_program->module_segment_count <= prefix_count) {
            continue;
        }
        for (index = 0U; index < prefix_count; ++index) {
            if (!slice_equals(source_program->module_segments[index],
                              prefix_segments[index])) {
                break;
            }
        }
        if (index < prefix_count) {
            continue;
        }
        next_segment = source_program->module_segments[prefix_count];
        if (partial.length > 0U &&
            (next_segment.length < partial.length ||
             memcmp(next_segment.data, partial.data, partial.length) != 0)) {
            continue;
        }
        if (!append_seen_module_completion_item(json,
                                                first,
                                                seen,
                                                &seen_count,
                                                64U,
                                                next_segment)) {
            return false;
        }
    }
    if (session->analysis != NULL) {
        size_t module_index;

        for (module_index = 0U;
             module_index < session->analysis->module_count;
             ++module_index) {
            const FengSemanticModule *module = &session->analysis->modules[module_index];
            FengSlice next_segment;
            size_t index;

            if (module->origin != FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE ||
                module->segment_count <= prefix_count) {
                continue;
            }
            for (index = 0U; index < prefix_count; ++index) {
                if (!slice_equals(module->segments[index], prefix_segments[index])) {
                    break;
                }
            }
            if (index < prefix_count) {
                continue;
            }
            next_segment = module->segments[prefix_count];
            if (partial.length > 0U &&
                (next_segment.length < partial.length ||
                 memcmp(next_segment.data, partial.data, partial.length) != 0)) {
                continue;
            }
            if (!append_seen_module_completion_item(json,
                                                    first,
                                                    seen,
                                                    &seen_count,
                                                    64U,
                                                    next_segment)) {
                return false;
            }
        }
    }
    return true;
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

/* Find the declared constraint for a type-parameter receiver in scope. */
static const FengTypeRef *completion_type_param_constraint(
    const FengDecl *enclosing_decl,
    const FengTypeMember *enclosing_member,
    FengSlice name) {
    const FengTypeParam *params = NULL;
    size_t param_count = 0U;
    size_t index;

    if (enclosing_member != NULL &&
        enclosing_member->kind != FENG_TYPE_MEMBER_FIELD) {
        params = enclosing_member->as.callable.type_params;
        param_count = enclosing_member->as.callable.type_param_count;
    }
    for (index = 0U; index < param_count; ++index) {
        if (slice_equals(params[index].name, name)) {
            return params[index].constraint;
        }
    }
    if (enclosing_decl == NULL) {
        return NULL;
    }
    if (enclosing_decl->kind == FENG_DECL_FUNCTION) {
        params = enclosing_decl->as.function_decl.type_params;
        param_count = enclosing_decl->as.function_decl.type_param_count;
    } else if (enclosing_decl->kind == FENG_DECL_TYPE) {
        params = enclosing_decl->as.type_decl.type_params;
        param_count = enclosing_decl->as.type_decl.type_param_count;
    } else if (enclosing_decl->kind == FENG_DECL_SPEC) {
        params = enclosing_decl->as.spec_decl.type_params;
        param_count = enclosing_decl->as.spec_decl.type_param_count;
    } else {
        return NULL;
    }
    for (index = 0U; index < param_count; ++index) {
        if (slice_equals(params[index].name, name)) {
            return params[index].constraint;
        }
    }
    return NULL;
}

static bool build_completion_json(const FengLspAnalysisSession *session,
                                  const FengProgram *program,
                                  const char *source_text,
                                  size_t offset,
                                  FengLspString *json,
                                  const FengLspRequestContext *request) {
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
    completion_context.position = completion_position_from_ast(enclosing_decl, enclosing_member);
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
        bool type_param_handled = false;
        bool is_static = completion_context.is_static_access;
        bool receiver_is_simple = completion_context.receiver.length ==
                                  completion_context.object.length;

        if (completion_context.literal_builtin_name.length > 0U) {
            owner_builtin_name = completion_context.literal_builtin_name;
        } else {
            const FengTypeRef *constraint =
                receiver_is_simple && find_local(&locals, completion_context.object) == NULL
                    ? completion_type_param_constraint(enclosing_decl,
                                                       enclosing_member,
                                                       completion_context.object)
                    : NULL;

            if (constraint != NULL) {
                owner_decl = resolve_named_type_ref(session, program, constraint);
                is_static = true;
                type_param_handled = owner_decl != NULL;
            }
            if (!type_param_handled && receiver_is_simple &&
                find_local(&locals, completion_context.object) == NULL &&
                !slice_equals_cstr(completion_context.object, "self")) {
                if (!append_alias_module_completion_items(json,
                                                          &first,
                                                          session,
                                                          program,
                                                          completion_context.object,
                                                          &alias_handled,
                                                          request)) {
                    local_list_dispose(&locals);
                    return false;
                }
            }
            if (!alias_handled && !type_param_handled) {
                if (receiver_is_simple) {
                    owner_decl = resolve_owner_decl_from_object_name(session,
                                                                     program,
                                                                     completion_context.object,
                                                                     &locals);
                    (void)resolve_owner_builtin_name_from_object_name(session,
                                                                      program,
                                                                      completion_context.object,
                                                                      &locals,
                                                                      &owner_builtin_name);
                } else {
                    owner_decl = resolve_owner_decl_from_receiver_text(session,
                                                                       program,
                                                                       completion_context.receiver,
                                                                       &locals,
                                                                       &owner_builtin_name);
                }
                if (receiver_is_simple && !is_static && owner_decl != NULL &&
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
            if (!append_owner_member_completion_items(json,
                                                      &first,
                                                      session,
                                                      program,
                                                      owner_decl,
                                                      enclosing_decl,
                                                      enclosing_member,
                                                      filter,
                                                      NULL,
                                                      request)) {
                local_list_dispose(&locals);
                return false;
            }
            if (!append_owner_fit_member_completion_items(json,
                                                          &first,
                                                          session,
                                                          program,
                                                          owner_decl,
                                                          owner_builtin_name,
                                                          filter,
                                                          request)) {
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
            if (!append_semantic_module_completion_items(json,
                                                         &first,
                                                         alias_module,
                                                         true,
                                                         NULL,
                                                         -1,
                                                         request)) {
                local_list_dispose(&locals);
                return false;
            }
        } else {
            owner_decl = resolve_owner_decl_from_object_expr(session,
                                                             program,
                                                             expr->as.member.object,
                                                             &locals);
            if (!append_owner_member_completion_items(json,
                                                      &first,
                                                      session,
                                                      program,
                                                      owner_decl,
                                                      enclosing_decl,
                                                      enclosing_member,
                                                      filter,
                                                      NULL,
                                                      request)) {
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
                                                          filter,
                                                          request)) {
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
            if (!append_semantic_module_completion_items(json,
                                                         &first,
                                                         program_module,
                                                         false,
                                                         NULL,
                                                         -1,
                                                         request)) {
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
                                                          -1,
                                                          request)) {
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
                if (!append_semantic_module_completion_items(json,
                                                             &first,
                                                             module,
                                                             true,
                                                             NULL,
                                                             -1,
                                                             request)) {
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
                                                              -1,
                                                              request)) {
                local_list_dispose(&locals);
                return false;
            }
        }
        if (completion_context.position != FENG_LSP_POS_OTHER) {
            if (!append_context_keyword_items(json, &first, completion_context.position)) {
                local_list_dispose(&locals);
                return false;
            }
        }
        /* Builtin type completion: offer builtin types and aliases in type
         * annotation positions (after ':'). */
        {
            FengSlice type_prefix = {0};

            if (completion_context_is_type_position(source_text, offset, &type_prefix)) {
                if (!append_builtin_type_items(json, &first, type_prefix)) {
                    local_list_dispose(&locals);
                    return false;
                }
            }
        }
    }
    local_list_dispose(&locals);
    return string_append_cstr(json, "]");
}

/* Counts top-level CompletionItem objects in a completed JSON array. */
static size_t completion_json_item_count(const FengLspString *json) {
    size_t array_depth = 0U;
    size_t object_depth = 0U;
    size_t count = 0U;
    size_t index;
    bool in_string = false;
    bool escaped = false;

    if (json == NULL || json->data == NULL) {
        return 0U;
    }
    if (json->completion_label_capacity > 0U) {
        return json->completion_item_count;
    }
    for (index = 0U; index < json->length; ++index) {
        char ch = json->data[index];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '[') {
            ++array_depth;
        } else if (ch == ']') {
            if (array_depth > 0U) {
                --array_depth;
            }
        } else if (ch == '{') {
            if (array_depth == 1U && object_depth == 0U) {
                ++count;
            }
            ++object_depth;
        } else if (ch == '}' && object_depth > 0U) {
            --object_depth;
        }
    }
    return count;
}

static bool build_cached_completion_json(const FengLspCacheQueryContext *context,
                                         size_t offset,
                                         FengLspString *json,
                                         size_t *out_item_count,
                                         const FengLspRequestContext *request) {
    const FengDecl *enclosing_decl;
    const FengTypeMember *enclosing_member;
    FengLspLocalList locals = {0};
    const FengExpr *expr;
    const FengExpr *member_object = NULL;
    FengExpr textual_member_object = {0};
    FengLspCompletionContext completion_context = {0};
    bool textual_receiver_is_complex = false;
    FengLspPosition position;
    bool first = true;
    size_t item_count = 0U;
    size_t index;

    if (out_item_count == NULL || !string_append_cstr(json, "[")) {
        return false;
    }
    *out_item_count = 0U;
    (void)completion_context_from_text(context->source_text,
                                       offset,
                                       &completion_context);
    enclosing_decl = find_enclosing_decl_for_completion(context->source_text,
                                                        context->program,
                                                        offset,
                                                        &enclosing_member);
    position = completion_position_from_ast(enclosing_decl, enclosing_member);
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
            *out_item_count = completion_json_item_count(json);
            return true;
        }
    }
    expr = enclosing_decl != NULL ? find_expr_hit_in_decl(enclosing_decl, offset) : NULL;
    textual_receiver_is_complex = completion_context.is_member &&
                                   completion_context.receiver.length > 0U &&
                                   completion_context.receiver.length !=
                                       completion_context.object.length;
    if (completion_context.is_member &&
        completion_context.literal_builtin_name.length == 0U &&
        !textual_receiver_is_complex) {
        /* Dirty-code parsing may not preserve the member expression.  Model the
         * receiver identified from current text so the cache path cannot fall
         * through to unrelated global completion items. */
        if (slice_equals_cstr(completion_context.object, "self")) {
            textual_member_object.kind = FENG_EXPR_SELF;
        } else {
            textual_member_object.kind = FENG_EXPR_IDENTIFIER;
            textual_member_object.as.identifier = completion_context.object;
        }
        member_object = &textual_member_object;
    } else if (!completion_context.is_member &&
               expr != NULL &&
               expr->kind == FENG_EXPR_MEMBER) {
        member_object = expr->as.member.object;
    }
    if (completion_context.is_member || member_object != NULL) {
        const FengSymbolImportedModule *alias_module = NULL;
        const FengSymbolDeclView *owner_decl = NULL;
        const FengTypeRef *type_param_constraint = NULL;
        FengSlice textual_builtin_name = {0};
        FengLspMemberFilter filter = completion_context.is_static_access
                                         ? FENG_LSP_MEMBER_FILTER_STATIC
                                         : FENG_LSP_MEMBER_FILTER_INSTANCE;

        if (member_object != NULL && member_object->kind == FENG_EXPR_IDENTIFIER &&
            find_local(&locals, member_object->as.identifier) == NULL) {
            type_param_constraint = completion_type_param_constraint(
                enclosing_decl, enclosing_member, member_object->as.identifier);
            if (type_param_constraint != NULL) {
                owner_decl = resolve_symbol_named_type_ref(context->provider,
                                                           context->current_module,
                                                           context->program,
                                                           type_param_constraint);
                filter = FENG_LSP_MEMBER_FILTER_STATIC;
            } else {
                alias_module = find_symbol_alias_module(context->provider,
                                                        context->program,
                                                        member_object->as.identifier);
            }
            if (type_param_constraint == NULL && alias_module == NULL) {
                const FengSymbolDeclView *vdecl = resolve_symbol_value_name(context->provider,
                                                                             context->current_module,
                                                                             context->program,
                                                                             member_object->as.identifier);

                if (vdecl != NULL) {
                    FengSymbolDeclKind vkind = feng_symbol_decl_kind(vdecl);

                    if (vkind == FENG_SYMBOL_DECL_KIND_TYPE || vkind == FENG_SYMBOL_DECL_KIND_ENUM ||
                        vkind == FENG_SYMBOL_DECL_KIND_SPEC) {
                        filter = FENG_LSP_MEMBER_FILTER_STATIC;
                    }
                } else if (resolve_symbol_type_name(context->provider,
                                                     context->current_module,
                                                     context->program,
                                                     member_object->as.identifier) != NULL) {
                    filter = FENG_LSP_MEMBER_FILTER_STATIC;
                }
            }
        } else if (member_object != NULL && member_object->kind == FENG_EXPR_SELF) {
            filter = FENG_LSP_MEMBER_FILTER_ALL;
        }
        if (alias_module != NULL) {
            for (index = 0U; index < feng_symbol_module_public_decl_count(alias_module); ++index) {
                const FengSymbolDeclView *decl = feng_symbol_module_public_decl_at(alias_module, index);
                FengSlice label;

                if (!symbol_decl_is_completion_decl(decl)) {
                    continue;
                }
                label = build_symbol_decl_completion_label(decl);
                if (label.data == NULL) {
                    local_list_dispose(&locals);
                    return false;
                }
                if (!append_symbol_decl_completion_item_with_label(json,
                                                                  &first,
                                                                  decl,
                                                                  label,
                                                                  NULL,
                                                                  -1,
                                                                  request)) {
                    free((void *)label.data);
                    local_list_dispose(&locals);
                    return false;
                }
                free((void *)label.data);
                ++item_count;
            }
        } else {
            if (owner_decl == NULL) {
                owner_decl = textual_receiver_is_complex
                                 ? resolve_symbol_owner_decl_from_receiver_text(
                                       context,
                                       completion_context.receiver,
                                       &locals,
                                       &textual_builtin_name)
                                 : resolve_symbol_owner_decl_from_expr(context,
                                                                       member_object,
                                                                       &locals);
            }
            if (owner_decl != NULL) {
                FengSlice owner_slice = feng_symbol_decl_name(owner_decl);
                char *sym_owner_name = dup_range(owner_slice.data, owner_slice.data + owner_slice.length);

                for (index = 0U; index < feng_symbol_decl_member_count(owner_decl); ++index) {
                    const FengSymbolDeclView *member = feng_symbol_decl_member_at(owner_decl, index);
                    bool contains = false;

                    if (!symbol_member_passes_filter(member, filter)) {
                        continue;
                    }
                    if (!symbol_member_visible_from_context(context,
                                                            owner_decl,
                                                            member,
                                                            enclosing_decl,
                                                            enclosing_member)) {
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
                    {
                        size_t overloads = count_symbol_member_overloads(owner_decl, feng_symbol_decl_name(member));

                        if (!append_symbol_member_completion_item(json,
                                                                 &first,
                                                                 member,
                                                                 sym_owner_name,
                                                                 overloads,
                                                                 request)) {
                            free(sym_owner_name);
                            local_list_dispose(&locals);
                            return false;
                        }
                    }
                    ++item_count;
                }
                free(sym_owner_name);
            }
            if ((owner_decl == NULL || item_count == 0U ||
                 feng_symbol_decl_kind(owner_decl) == FENG_SYMBOL_DECL_KIND_SPEC) &&
                context->source_module_index != NULL) {
                FengLspAnalysisSession source_session = {0};
                FengCliLoadedSource source = {0};
                const FengDecl *source_owner;
                FengSlice source_builtin_name = {0};

                source_session.source_module_index = context->source_module_index;
                source.path = context->program->path;
                source.source = (char *)context->source_text;
                source.source_length = strlen(context->source_text);
                source.program = context->program;
                source_session.sources = &source;
                source_session.source_count = 1U;
                source_owner = type_param_constraint != NULL
                                   ? resolve_named_type_ref(&source_session,
                                                            context->program,
                                                            type_param_constraint)
                                   : textual_receiver_is_complex
                                         ? resolve_owner_decl_from_receiver_text(
                                               &source_session,
                                               context->program,
                                               completion_context.receiver,
                                               &locals,
                                               &source_builtin_name)
                                         : resolve_owner_decl_from_object_expr(
                                               &source_session,
                                               context->program,
                                               member_object,
                                               &locals);
                if (!append_owner_member_completion_items(json,
                                                          &first,
                                                          &source_session,
                                                          context->program,
                                                          source_owner,
                                                          enclosing_decl,
                                                          enclosing_member,
                                                          filter,
                                                          context,
                                                          request)) {
                    local_list_dispose(&locals);
                    return false;
                }
                if (textual_builtin_name.length == 0U) {
                    textual_builtin_name = source_builtin_name;
                }
                item_count = completion_json_item_count(json);
            }
            if (item_count == 0U) {
                FengSlice builtin_name = completion_context.literal_builtin_name.length > 0U
                                             ? completion_context.literal_builtin_name
                                             : textual_builtin_name.length > 0U
                                                   ? textual_builtin_name
                                                   : resolve_symbol_builtin_name_from_expr(
                                                         context,
                                                         member_object,
                                                         &locals);

                /* When the object is a bare builtin type identifier (not a local),
                 * this is a static access: string., i32., etc. */
                if (builtin_name.length > 0U &&
                    member_object != NULL &&
                    member_object->kind == FENG_EXPR_IDENTIFIER &&
                    find_local(&locals, member_object->as.identifier) == NULL) {
                    filter = FENG_LSP_MEMBER_FILTER_STATIC;
                }
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
                                if (!append_symbol_member_completion_item(json,
                                                                         &first,
                                                                         member,
                                                                         NULL,
                                                                         0U,
                                                                         request)) {
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
                FengSlice label;

                if (!symbol_decl_is_completion_decl(decl)) {
                    continue;
                }
                label = build_symbol_decl_completion_label(decl);
                if (label.data == NULL) {
                    local_list_dispose(&locals);
                    return false;
                }
                if (!append_symbol_decl_completion_item_with_label(json,
                                                                  &first,
                                                                  decl,
                                                                  label,
                                                                  NULL,
                                                                  -1,
                                                                  request)) {
                    free((void *)label.data);
                    local_list_dispose(&locals);
                    return false;
                }
                free((void *)label.data);
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
                    FengSlice label;

                    if (!symbol_decl_is_completion_decl(decl)) {
                        continue;
                    }
                    label = build_symbol_decl_completion_label(decl);
                    if (label.data == NULL) {
                        local_list_dispose(&locals);
                        return false;
                    }
                    if (!append_symbol_decl_completion_item_with_label(json,
                                                                      &first,
                                                                      decl,
                                                                      label,
                                                                      NULL,
                                                                      -1,
                                                                      request)) {
                        free((void *)label.data);
                        local_list_dispose(&locals);
                        return false;
                    }
                    free((void *)label.data);
                    ++item_count;
                }
            }
        }
        if (position != FENG_LSP_POS_OTHER) {
            if (!append_context_keyword_items(json, &first, position)) {
                local_list_dispose(&locals);
                return false;
            }
        }
        /* Builtin type completion: offer builtin types and aliases in type
         * annotation positions (after ':'). */
        {
            FengSlice type_prefix = {0};

            if (completion_context_is_type_position(context->source_text, offset, &type_prefix)) {
                if (!append_builtin_type_items(json, &first, type_prefix)) {
                    local_list_dispose(&locals);
                    return false;
                }
            }
        }
    }
    local_list_dispose(&locals);
    if (!string_append_cstr(json, "]")) {
        return false;
    }
    *out_item_count = completion_json_item_count(json);
    return true;
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

/* Detect whether the cursor is in an annotation context (@prefix).
 * Scans backwards from offset for identifier characters, then checks
 * whether the character immediately before the identifier is '@'.
 * The '@' must be at the start of the file or preceded by whitespace
 * to avoid false positives (e.g. email addresses).
 * Returns true and sets *out_prefix to the typed identifier part
 * (empty when only '@' has been typed). */
static bool completion_context_is_annotation(const char *text,
                                             size_t offset,
                                             FengSlice *out_prefix) {
    size_t length;
    size_t prefix_end;
    size_t prefix_start;
    size_t at_pos;

    if (text == NULL || out_prefix == NULL) {
        return false;
    }
    length = strlen(text);
    if (offset > length || offset == 0U) {
        return false;
    }
    /* Scan backwards for identifier characters after '@'. */
    prefix_end = offset;
    prefix_start = offset;
    while (prefix_start > 0U && completion_identifier_continue(text[prefix_start - 1U])) {
        --prefix_start;
    }
    /* The character before the identifier must be '@'. */
    if (prefix_start == 0U || text[prefix_start - 1U] != '@') {
        return false;
    }
    at_pos = prefix_start - 1U;
    /* '@' must be at start of file or preceded by whitespace. */
    if (at_pos > 0U && !isspace((unsigned char)text[at_pos - 1U])) {
        return false;
    }
    out_prefix->data = text + prefix_start;
    out_prefix->length = prefix_end - prefix_start;
    return true;
}

/* Detect whether `offset` in `text` is in a type annotation position
 * (immediately after ':' in a type annotation context like `let x: `,
 * `func foo(): `, `var field: `).  Returns true and sets `out_prefix`
 * to the partially-typed identifier when the cursor follows ':'.
 * The ':' must not be part of '::' (scope resolution). */
static bool completion_context_is_type_position(const char *text,
                                                 size_t offset,
                                                 FengSlice *out_prefix) {
    size_t length;
    size_t prefix_end;
    size_t prefix_start;
    size_t colon_pos;

    if (text == NULL || out_prefix == NULL) {
        return false;
    }
    length = strlen(text);
    if (offset > length) {
        return false;
    }
    /* Scan backwards from offset to find the end of any identifier. */
    prefix_end = offset;
    prefix_start = offset;
    while (prefix_start > 0U && completion_identifier_continue(text[prefix_start - 1U])) {
        --prefix_start;
    }
    /* Skip whitespace between the identifier and the preceding token. */
    colon_pos = prefix_start;
    while (colon_pos > 0U && isspace((unsigned char)text[colon_pos - 1U])) {
        --colon_pos;
    }
    /* The character before whitespace must be ':'. */
    if (colon_pos == 0U || text[colon_pos - 1U] != ':') {
        return false;
    }
    /* Reject '::' (scope resolution, not type annotation). */
    if (colon_pos >= 2U && text[colon_pos - 2U] == ':') {
        return false;
    }
    out_prefix->data = text + prefix_start;
    out_prefix->length = prefix_end - prefix_start;
    return true;
}

/* Append builtin type and type alias completion items filtered by `prefix`.
 * When prefix is non-empty, only items whose label starts with the prefix
 * are included.  Uses CompletionItemKind 14 (Keyword) for consistency with
 * keyword and annotation items.  Returns true on success. */
static bool append_builtin_type_items(FengLspString *json,
                                       bool *first,
                                       FengSlice prefix) {
    size_t index;

    /* Builtin types. */
    for (index = 0U; index < BUILTIN_TYPE_COUNT; ++index) {
        const LspBuiltinTypeItem *item = &BUILTIN_TYPES[index];

        if (prefix.length > 0U) {
            if (strlen(item->label) < prefix.length ||
                strncmp(item->label, prefix.data, prefix.length) != 0) {
                continue;
            }
        }
        if (!append_completion_item(json,
                                    first,
                                    slice_from_cstr(item->label),
                                    item->detail,
                                    14)) {
            return false;
        }
    }
    /* Type aliases. */
    for (index = 0U; index < BUILTIN_TYPE_ALIAS_COUNT; ++index) {
        const LspBuiltinTypeAliasItem *alias = &BUILTIN_TYPE_ALIASES[index];

        if (prefix.length > 0U) {
            if (strlen(alias->label) < prefix.length ||
                strncmp(alias->label, prefix.data, prefix.length) != 0) {
                continue;
            }
        }
        if (!append_completion_item(json,
                                    first,
                                    slice_from_cstr(alias->label),
                                    alias->detail,
                                    14)) {
            return false;
        }
    }
    return true;
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

/* Repair source for signatureHelp by closing unclosed parentheses at offset. */
static char *dup_text_with_signature_repair(const char *text, size_t offset) {
    size_t text_length;
    int depth;
    size_t i;
    char *out;

    if (text == NULL) {
        return NULL;
    }
    text_length = strlen(text);
    if (offset > text_length) {
        return NULL;
    }
    /* Count unclosed parens from start to offset. */
    depth = 0;
    for (i = 0U; i < offset; ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            if (depth > 0) {
                --depth;
            }
        }
    }
    if (depth <= 0) {
        return NULL;
    }
    /* Insert ");" at offset to close the call and terminate the statement. */
    out = (char *)malloc(text_length + 3U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, offset);
    out[offset] = ')';
    out[offset + 1U] = ';';
    memcpy(out + offset + 2U, text + offset, text_length - offset + 1U);
    return out;
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

static bool build_repaired_completion_json(FengLspService *service,
                                           const FengLspDocument *document,
                                           size_t offset,
                                           FengLspString *json,
                                           const FengLspRequestContext *request) {
    FengLspDocument repaired;
    FengLspAnalysisSession session = {0};
    const FengProgram *program;
    bool ok = false;

    if (service == NULL || document == NULL || json == NULL) {
        return false;
    }
    repaired = *document;
    repaired.text = dup_text_with_completion_repair(document->text, offset);
    if (repaired.text == NULL) {
        return false;
    }
    if (build_single_parse_session(&repaired, &session)) {
        program = find_program(&session, repaired.path);
        if (program != NULL && program->use_count > 0U) {
            wait_for_program_import_indexes(service, program);
        }
        ok = program != NULL && build_completion_json(&session,
                                                      program,
                                                      repaired.text,
                                                      offset,
                                                      json,
                                                      request);
        if (ok) {
            pthread_mutex_lock(&service->analysis_mutex);
            ok = append_module_index_imports(service,
                                             program,
                                             repaired.text,
                                             offset,
                                             json,
                                             request);
            pthread_mutex_unlock(&service->analysis_mutex);
        }
    }
    session_dispose(&session);
    free(repaired.text);
    return ok;
}

static bool handle_completion_request(FengLspService *service,
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
    FengLspAnalysisSession current_parse = {0};
    FengCliLoadedSource current_source = {0};
    const FengLspAnalysisSession *last_successful;
    FengLspCacheQueryContext cache = {0};
    const FengProgram *program;
    FengLspString json = {0};
    FengLspString cache_json = {0};
    FengLspRequestContext request = {0};
    bool ok;
    size_t offset;
    bool is_member_completion;
    bool is_use_path_completion;
    bool can_repair_completion;
    bool last_successful_has_items = false;
    bool cache_has_items = false;
    bool use_path_has_items = false;
    bool current_parse_ok = false;

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
    document = find_document(service, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "[]");
    }
    offset = document_offset_from_position(document, line, character);
    /* Annotation context takes priority over all other completion paths.
     * When the cursor follows '@', return only annotation items and skip
     * member, keyword, and identifier completion entirely. */
    {
        FengSlice annotation_prefix = {0};

        if (completion_context_is_annotation(document->text, offset, &annotation_prefix)) {
            if (build_annotation_completion_json(annotation_prefix, &json)) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
            free(uri);
            return send_json_response(output, id, "[]");
        }
    }
    is_member_completion = completion_context_is_member_access(document->text, offset);
    {
        FengSlice prefix_segments[16];
        FengSlice partial = {0};
        size_t prefix_count = 0U;

        is_use_path_completion = extract_use_path_context(document->text,
                                                          offset,
                                                          prefix_segments,
                                                          &prefix_count,
                                                          16U,
                                                          &partial);
    }
    can_repair_completion = is_member_completion || completion_repair_needs_semicolon(document->text, offset);
    request.uri = uri;
    /* A cold workspace must not parse a large document on the request path.
     * Return exact text-only candidates until a published index is ready. */
    if (!is_member_completion) {
        bool published_query_ready;

        pthread_mutex_lock(&service->analysis_mutex);
        published_query_ready = service->last_successful_analysis.analysis != NULL ||
                                service->symbol_index != NULL;
        pthread_mutex_unlock(&service->analysis_mutex);
        if (!published_query_ready) {
            FengSlice type_prefix = {0};
            bool first = true;

            if (completion_context_is_type_position(document->text, offset, &type_prefix) &&
                string_append_cstr(&json, "[") &&
                append_builtin_type_items(&json, &first, type_prefix) &&
                string_append_cstr(&json, "]") &&
                completion_json_has_items(&json)) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
        }
    }
    if (is_use_path_completion) {
        wait_for_initial_module_index(service);
    }
    pthread_mutex_lock(&service->analysis_mutex);
    use_path_has_items = build_persistent_use_path_completion_json(service,
                                                                   document->text,
                                                                   offset,
                                                                   &json);
    pthread_mutex_unlock(&service->analysis_mutex);
    if (!use_path_has_items && is_use_path_completion) {
        string_dispose(&json);
        wait_for_initial_symbol_index(service);
        pthread_mutex_lock(&service->analysis_mutex);
        use_path_has_items = build_persistent_use_path_completion_json(service,
                                                                       document->text,
                                                                       offset,
                                                                       &json);
        pthread_mutex_unlock(&service->analysis_mutex);
    }
    if (use_path_has_items) {
        free(uri);
        ok = send_json_response(output, id, json.data);
        string_dispose(&json);
        return ok;
    }
    string_dispose(&json);
    pthread_mutex_lock(&service->analysis_mutex);
    last_successful = &service->last_successful_analysis;
    if (analysis_matches_document(last_successful, document)) {
        program = find_program(last_successful, document->path);
        last_successful_has_items =
            program != NULL &&
            build_completion_json(last_successful,
                                  program,
                                  document->text,
                                  offset,
                                  &json,
                                  &request) &&
            completion_json_has_items(&json);
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    if (last_successful_has_items) {
        free(uri);
        ok = send_json_response(output, id, json.data);
        string_dispose(&json);
        return ok;
    }
    if (json.data != NULL) {
        string_dispose(&json);
    }
    program = ensure_document_parse(document);
    if (program != NULL && program->use_count > 0U) {
        wait_for_program_import_indexes(service, program);
    }
    pthread_mutex_lock(&service->analysis_mutex);
    if (build_persistent_cache_query_context(service,
                                             document,
                                             document->text,
                                             &cache)) {
        size_t cache_item_count = 0U;

        cache_has_items = build_cached_completion_json(&cache,
                                                       offset,
                                                       &cache_json,
                                                       &cache_item_count,
                                                       &request) &&
                          append_module_index_imports(service,
                                                      cache.program,
                                                      document->text,
                                                      offset,
                                                      &cache_json,
                                                      &request);
        cache_item_count = completion_json_item_count(&cache_json);
        cache_has_items = cache_has_items && cache_item_count > 0U;
    }
    cache_query_context_dispose(&cache);
    pthread_mutex_unlock(&service->analysis_mutex);
    if (cache_has_items) {
        free(uri);
        ok = send_json_response(output, id, cache_json.data);
        string_dispose(&cache_json);
        return ok;
    }
    string_dispose(&cache_json);
    if (program != NULL) {
        current_source.path = document->path;
        current_source.source = document->text;
        current_source.source_length = strlen(document->text);
        current_source.program = (FengProgram *)program;
        current_parse.sources = &current_source;
        current_parse.source_count = 1U;
        current_parse_ok = build_completion_json(&current_parse,
                                                 program,
                                                 document->text,
                                                 offset,
                                                 &json,
                                                 &request);
        if (current_parse_ok) {
            pthread_mutex_lock(&service->analysis_mutex);
            current_parse_ok = append_module_index_imports(service,
                                                           program,
                                                           document->text,
                                                           offset,
                                                           &json,
                                                           &request);
            pthread_mutex_unlock(&service->analysis_mutex);
        }
        if (current_parse_ok && completion_json_has_items(&json)) {
            free(uri);
            ok = send_json_response(output, id, json.data);
            string_dispose(&json);
            return ok;
        }
        string_dispose(&json);
    }
    /* Repair only the current document and query the already-published symbol
     * index; neither parsing nor candidate construction performs project I/O. */
    if (can_repair_completion) {
        char *repaired_text = dup_text_with_completion_repair(document->text, offset);

        if (repaired_text != NULL) {
            FengLspCacheQueryContext repair_cache = {0};
            FengLspDocument repaired_document = *document;
            FengLspAnalysisSession repair_parse = {0};
            size_t repair_item_count = 0U;

            repaired_document.text = repaired_text;
            if (build_single_parse_session(&repaired_document, &repair_parse)) {
                const FengProgram *repair_program =
                    find_program(&repair_parse, repaired_document.path);

                wait_for_program_import_indexes(service, repair_program);
            }
            session_dispose(&repair_parse);

            pthread_mutex_lock(&service->analysis_mutex);
            cache_has_items =
                build_persistent_cache_query_context(service,
                                                     document,
                                                     repaired_text,
                                                     &repair_cache) &&
                build_cached_completion_json(&repair_cache,
                                             offset,
                                             &json,
                                             &repair_item_count,
                                             &request) &&
                repair_item_count > 0U;
            cache_query_context_dispose(&repair_cache);
            pthread_mutex_unlock(&service->analysis_mutex);
            if (cache_has_items) {
                free(repaired_text);
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
            free(repaired_text);
        }
    }
    if (can_repair_completion &&
        build_repaired_completion_json(service, document, offset, &json, &request) &&
        completion_json_has_items(&json)) {
        free(uri);
        ok = send_json_response(output, id, json.data);
        string_dispose(&json);
        return ok;
    }
    string_dispose(&json);
    /* Builtin type annotation fallback: when in a type position (after ':')
     * and no other path produced results, offer builtin types and aliases. */
    {
        FengSlice type_prefix = {0};

        if (completion_context_is_type_position(document->text, offset, &type_prefix)) {
            bool first = true;

            if (string_append_cstr(&json, "[") &&
                append_builtin_type_items(&json, &first, type_prefix) &&
                string_append_cstr(&json, "]") &&
                completion_json_has_items(&json)) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
        }
    }
    /* Last-resort keyword fallback: when every other path produced no
     * items, offer position-aware keywords based on text analysis.
     * This ensures the user always sees relevant keywords even when the
     * document is too dirty for AST or cache analysis. */
    {
        FengLspPosition kw_position = completion_position_from_text(document->text, offset);

        if (!is_member_completion && kw_position != FENG_LSP_POS_OTHER) {
            bool first = true;

            if (string_append_cstr(&json, "[") &&
                append_context_keyword_items(&json, &first, kw_position) &&
                string_append_cstr(&json, "]") &&
                completion_json_has_items(&json)) {
                free(uri);
                ok = send_json_response(output, id, json.data);
                string_dispose(&json);
                return ok;
            }
            string_dispose(&json);
        }
    }
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
    FengSymbolDeclKind kind;

    if (buffer == NULL || member == NULL) {
        return false;
    }
    name = feng_symbol_decl_name(member);
    kind = feng_symbol_decl_kind(member);

    if (kind == FENG_SYMBOL_DECL_KIND_FIELD) {
        const FengSymbolTypeView *field_type = feng_symbol_decl_value_type(member);
        bool is_mutable = feng_symbol_decl_mutability(member) == FENG_MUTABILITY_VAR;

        if (!string_append_cstr(buffer, is_mutable ? "var " : "let ") ||
            !string_append_bytes(buffer, name.data, name.length)) {
            return false;
        }
        if (field_type != NULL) {
            if (!string_append_cstr(buffer, ": ") ||
                !symbol_type_to_string(buffer, field_type)) {
                return false;
            }
        }
        return true;
    }
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
        FengSlice owner_slice = strip_arity_suffix(slice_from_cstr(owner_name));
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
        FengSlice base_name = strip_arity_suffix(label_slice);
        bool has_arity = (base_name.length < label_slice.length);
        size_t arity = 0U;
        size_t count;

        /* Count type params from the label suffix "<T, U>" if present. */
        if (has_arity) {
            size_t i;
            arity = 1U;
            for (i = base_name.length; i < label_slice.length; ++i) {
                if (label_slice.data[i] == ',') {
                    ++arity;
                }
            }
        }
        count = feng_symbol_module_decl_count(context->current_module);
        /* Try exact (name, arity) match first when arity info is available. */
        if (has_arity) {
            for (index = 0U; index < count; ++index) {
                const FengSymbolDeclView *d = feng_symbol_module_decl_at(context->current_module, index);
                if (slice_equals(feng_symbol_decl_name(d), base_name) &&
                    feng_symbol_decl_type_param_count(d) == arity) {
                    FengSlice doc = feng_symbol_decl_doc(d);
                    if (doc.length > 0U) {
                        return dup_range(doc.data, doc.data + doc.length);
                    }
                    return NULL;
                }
            }
        }
        /* Fall back to name-only match. */
        for (index = 0U; index < count; ++index) {
            const FengSymbolDeclView *d = feng_symbol_module_decl_at(context->current_module, index);
            if (slice_equals(feng_symbol_decl_name(d), base_name)) {
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
        FengSlice owner_slice = strip_arity_suffix(slice_from_cstr(owner_name));
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
        FengSlice base_name = strip_arity_suffix(label_slice);
        for (decl_index = 0U; decl_index < program->declaration_count; ++decl_index) {
            const FengDecl *decl = program->declarations[decl_index];
            if (slice_equals(decl_name(decl), base_name)) {
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
    } else if (owner_name != NULL) {
        /* Builtin type: owner_decl is NULL but we can match fit targets by name. */
        FengSlice owner_name_slice = slice_from_cstr(owner_name);
        const char *builtin = builtin_name_for_identifier(owner_name_slice);

        if (builtin != NULL) {
            FengSlice builtin_slice = slice_from_cstr(builtin);

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
                    if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
                        if (!slice_equals(feng_symbol_type_builtin_name(target), builtin_slice)) {
                            continue;
                        }
                    } else if (feng_symbol_type_kind(target) == FENG_SYMBOL_TYPE_KIND_NAMED &&
                               feng_symbol_type_segment_count(target) >= 1U) {
                        FengSlice seg = feng_symbol_type_segment_at(target,
                                                                    feng_symbol_type_segment_count(target) - 1U);
                        if (!slice_equals(seg, builtin_slice)) {
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

static bool handle_signature_help_request(FengLspService *service,
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
    document = find_document(service, uri);
    if (document == NULL || document->text == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = document_offset_from_position(document, line, character);
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

        pthread_mutex_lock(&service->analysis_mutex);
        if (build_persistent_cache_query_context(service,
                                                 document,
                                                 document->text,
                                                 &cache)) {
            const FengDecl *enclosing_decl;
            const FengTypeMember *enclosing_member;

            enclosing_decl = find_enclosing_decl_for_completion(cache.source_text,
                                                                 cache.program,
                                                                 offset,
                                                                 &enclosing_member);
            if (enclosing_decl != NULL) {
                (void)collect_visible_locals_for_completion(cache.source_text,
                                                            enclosing_decl,
                                                            enclosing_member,
                                                            offset,
                                                            &locals);
            }
            ok = build_signature_help_json(&cache,
                                           method_name,
                                           owner_name_str,
                                           &locals,
                                           active_param,
                                           &json);
            local_list_dispose(&locals);
            cache_query_context_dispose(&cache);
        }
        pthread_mutex_unlock(&service->analysis_mutex);
        if (!ok) {
            /* Repaired-source fallback: close unclosed parens to make source parseable. */
            char *repaired_text = dup_text_with_signature_repair(document->text, offset);

            if (repaired_text != NULL) {
                string_dispose(&json);
                pthread_mutex_lock(&service->analysis_mutex);
                if (build_persistent_cache_query_context(service,
                                                         document,
                                                         repaired_text,
                                                         &cache)) {
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
                pthread_mutex_unlock(&service->analysis_mutex);
                free(repaired_text);
            }
        }
        if (!ok) {
            string_dispose(&json);
            /* Provider-only fallback reads the immutable workspace index. */
            pthread_mutex_lock(&service->analysis_mutex);
            if (service->symbol_index != NULL) {
                FengLspCacheQueryContext minimal = {0};

                minimal.provider = service->symbol_index;
                ok = build_signature_help_json(&minimal,
                                               method_name,
                                               owner_name_str,
                                               &locals,
                                               active_param,
                                               &json);
            }
            pthread_mutex_unlock(&service->analysis_mutex);
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

static bool handle_completion_resolve_request(FengLspService *service,
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

    document = find_document(service, uri);
    if (document != NULL) {
        FengLspCacheQueryContext cache = {0};

        pthread_mutex_lock(&service->analysis_mutex);
        if (build_persistent_cache_query_context(service,
                                                 document,
                                                 document->text,
                                                 &cache)) {
            doc = resolve_doc_from_cache(&cache, label, owner_name);
            cache_query_context_dispose(&cache);
        }
        if (doc == NULL &&
            analysis_matches_document(&service->last_successful_analysis, document)) {
            const FengProgram *program = find_program(&service->last_successful_analysis,
                                                      document->path);

            if (program != NULL) {
                doc = resolve_doc_from_session(&service->last_successful_analysis,
                                               program,
                                               label,
                                               owner_name);
            }
        }
        if (doc == NULL && owner_name != NULL && service->symbol_index != NULL) {
            FengLspCacheQueryContext minimal_cache = {0};

            minimal_cache.provider = service->symbol_index;
            doc = resolve_doc_from_cache(&minimal_cache, label, owner_name);
        }
        pthread_mutex_unlock(&service->analysis_mutex);
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

static bool handle_references_request(FengLspService *service,
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
    const FengLspAnalysisSession *session;
    const FengProgram *program;
    FengCliLoadedSource current_source = {0};
    FengLspAnalysisSession current_parse = {0};
    FengLspResolvedTarget target = {0};
    FengLspReferenceList references = {0};
    FengLspString json = {0};
    bool ok;
    bool query_ok = false;
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
    document = find_document(service, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "[]");
    }
    offset = document_offset_from_position(document, line, character);
    pthread_mutex_lock(&service->analysis_mutex);
    session = &service->last_successful_analysis;
    program = analysis_matches_document(session, document)
        ? find_program(session, document->path)
        : NULL;
    if (program != NULL && resolve_target_at(session, program, offset, &target) &&
        collect_references(session, include_declaration, &target, &references)) {
        query_ok = true;
    }
    if (query_ok && request_is_cancelled(service, id)) {
        pthread_mutex_unlock(&service->analysis_mutex);
        free(uri);
        reference_list_dispose(&references);
        return send_error_response(output, id, -32800, "Request cancelled");
    }
    if (query_ok) {
        query_ok = build_references_json(session, &references, &json);
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    if (!query_ok) {
        reference_list_dispose(&references);
        string_dispose(&json);
        memset(&target, 0, sizeof(target));
        program = ensure_document_parse(document);
        if (program != NULL) {
            current_source.path = document->path;
            current_source.source = document->text;
            current_source.source_length = strlen(document->text);
            current_source.program = (FengProgram *)program;
            current_parse.sources = &current_source;
            current_parse.source_count = 1U;
            query_ok = resolve_target_at(&current_parse, program, offset, &target) &&
                       collect_references(&current_parse,
                                          include_declaration,
                                          &target,
                                          &references);
            if (query_ok && request_is_cancelled(service, id)) {
                free(uri);
                reference_list_dispose(&references);
                return send_error_response(output, id, -32800, "Request cancelled");
            }
            query_ok = query_ok &&
                       build_references_json(&current_parse, &references, &json);
        }
    }
    free(uri);
    reference_list_dispose(&references);
    if (!query_ok) {
        string_dispose(&json);
        return send_json_response(output, id, "[]");
    }
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
    return ok;
}

/* Builds prepare-rename output from one immutable semantic or current-AST view. */
static bool build_prepare_rename_from_session(const FengLspAnalysisSession *session,
                                              const FengProgram *program,
                                              const char *path,
                                              size_t offset,
                                              FengLspReferenceList *references,
                                              FengLspString *json) {
    FengLspResolvedTarget target = {0};
    const FengLspReferenceEntry *entry;
    const FengCliLoadedSource *source;

    if (session == NULL || program == NULL ||
        !resolve_target_at(session, program, offset, &target) ||
        !resolved_target_can_rename(session, &target) ||
        !collect_references(session, true, &target, references)) {
        return false;
    }
    entry = reference_list_find_offset(references, path, offset);
    source = find_reference_source(session, entry);
    return entry != NULL && source != NULL &&
           build_prepare_rename_json(source, entry, json);
}

/* Builds rename edits from one immutable semantic or current-AST view. */
static bool build_rename_from_session(const FengLspAnalysisSession *session,
                                      const FengProgram *program,
                                      const char *path,
                                      size_t offset,
                                      const char *new_name,
                                      FengLspReferenceList *references,
                                      FengLspString *json) {
    FengLspResolvedTarget target = {0};

    return session != NULL && program != NULL &&
           resolve_target_at(session, program, offset, &target) &&
           resolved_target_can_rename(session, &target) &&
           collect_references(session, true, &target, references) &&
           reference_list_find_offset(references, path, offset) != NULL &&
           build_rename_json(session, references, new_name, json);
}

static bool handle_prepare_rename_request(FengLspService *service,
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
    const FengLspAnalysisSession *session;
    const FengProgram *program;
    FengCliLoadedSource current_source = {0};
    FengLspAnalysisSession current_parse = {0};
    FengLspReferenceList references = {0};
    FengLspString json = {0};
    bool ok;
    bool query_ok = false;
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
    document = find_document(service, uri);
    if (document == NULL) {
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = document_offset_from_position(document, line, character);
    pthread_mutex_lock(&service->analysis_mutex);
    session = &service->last_successful_analysis;
    program = analysis_matches_document(session, document)
        ? find_program(session, document->path)
        : NULL;
    query_ok = build_prepare_rename_from_session(session,
                                                 program,
                                                 document->path,
                                                 offset,
                                                 &references,
                                                 &json);
    if (query_ok && request_is_cancelled(service, id)) {
        pthread_mutex_unlock(&service->analysis_mutex);
        free(uri);
        reference_list_dispose(&references);
        return send_error_response(output, id, -32800, "Request cancelled");
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    if (!query_ok) {
        reference_list_dispose(&references);
        string_dispose(&json);
        program = ensure_document_parse(document);
        if (program != NULL) {
            current_source.path = document->path;
            current_source.source = document->text;
            current_source.source_length = strlen(document->text);
            current_source.program = (FengProgram *)program;
            current_parse.sources = &current_source;
            current_parse.source_count = 1U;
            query_ok = build_prepare_rename_from_session(&current_parse,
                                                         program,
                                                         document->path,
                                                         offset,
                                                         &references,
                                                         &json);
        }
    }
    if (query_ok && request_is_cancelled(service, id)) {
        free(uri);
        reference_list_dispose(&references);
        string_dispose(&json);
        return send_error_response(output, id, -32800, "Request cancelled");
    }
    free(uri);
    reference_list_dispose(&references);
    if (!query_ok) {
        string_dispose(&json);
        return send_json_response(output, id, "null");
    }
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
    return ok;
}

static bool handle_rename_request(FengLspService *service,
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
    const FengLspAnalysisSession *session;
    const FengProgram *program;
    FengCliLoadedSource current_source = {0};
    FengLspAnalysisSession current_parse = {0};
    FengLspReferenceList references = {0};
    FengLspString json = {0};
    bool ok;
    bool query_ok = false;
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
    document = find_document(service, uri);
    if (document == NULL) {
        free(new_name);
        free(uri);
        return send_json_response(output, id, "null");
    }
    offset = document_offset_from_position(document, line, character);
    pthread_mutex_lock(&service->analysis_mutex);
    session = &service->last_successful_analysis;
    program = analysis_matches_document(session, document)
        ? find_program(session, document->path)
        : NULL;
    query_ok = build_rename_from_session(session,
                                         program,
                                         document->path,
                                         offset,
                                         new_name,
                                         &references,
                                         &json);
    if (query_ok && request_is_cancelled(service, id)) {
        pthread_mutex_unlock(&service->analysis_mutex);
        free(new_name);
        free(uri);
        reference_list_dispose(&references);
        return send_error_response(output, id, -32800, "Request cancelled");
    }
    pthread_mutex_unlock(&service->analysis_mutex);
    if (!query_ok) {
        reference_list_dispose(&references);
        string_dispose(&json);
        program = ensure_document_parse(document);
        if (program != NULL) {
            current_source.path = document->path;
            current_source.source = document->text;
            current_source.source_length = strlen(document->text);
            current_source.program = (FengProgram *)program;
            current_parse.sources = &current_source;
            current_parse.source_count = 1U;
            query_ok = build_rename_from_session(&current_parse,
                                                 program,
                                                 document->path,
                                                 offset,
                                                 new_name,
                                                 &references,
                                                 &json);
        }
    }
    if (query_ok && request_is_cancelled(service, id)) {
        free(new_name);
        free(uri);
        reference_list_dispose(&references);
        string_dispose(&json);
        return send_error_response(output, id, -32800, "Request cancelled");
    }
    free(new_name);
    free(uri);
    reference_list_dispose(&references);
    if (!query_ok) {
        string_dispose(&json);
        return send_json_response(output, id, "null");
    }
    ok = send_json_response(output, id, json.data);
    string_dispose(&json);
    return ok;
}

/* Maps an LSP method to its interaction scheduling priority. */
static FengLspRequestPriority request_priority_for_method(const char *method) {
    if (method == NULL) {
        return FENG_LSP_PRIORITY_LOW;
    }
    if (strcmp(method, "textDocument/completion") == 0 ||
        strcmp(method, "textDocument/hover") == 0 ||
        strcmp(method, "textDocument/signatureHelp") == 0) {
        return FENG_LSP_PRIORITY_HIGHEST;
    }
    if (strcmp(method, "textDocument/definition") == 0 ||
        strcmp(method, "textDocument/prepareRename") == 0) {
        return FENG_LSP_PRIORITY_HIGH;
    }
    if (strcmp(method, "textDocument/references") == 0 ||
        strcmp(method, "textDocument/rename") == 0) {
        return FENG_LSP_PRIORITY_MEDIUM;
    }
    return FENG_LSP_PRIORITY_LOW;
}

/* Reads the current version addressed by a request for optional trace output. */
static unsigned int trace_document_version(FengLspService *service,
                                           const FengLspScheduledRequest *request) {
    FengLspMessage message = {0};
    FengLspJsonValue text_document = {0};
    FengLspJsonValue uri_value = {0};
    FengLspDocument *document;
    char *uri;
    unsigned int version = 0U;

    if (service == NULL || request == NULL ||
        parse_jsonrpc_message(request->payload,
                              request->payload_length,
                              &message) != FENG_LSP_PARSE_OK ||
        !json_object_get(message.params, "textDocument", &text_document) ||
        !json_object_get(text_document, "uri", &uri_value)) {
        message_dispose(&message);
        return 0U;
    }
    uri = json_string_dup(uri_value);
    document = uri != NULL ? find_document(service, uri) : NULL;
    if (document != NULL) {
        version = document->version;
    }
    free(uri);
    message_dispose(&message);
    return version;
}

/* Executes queued interaction payloads independently from protocol reading. */
static void *request_worker_main(void *user) {
    FengLspService *service = (FengLspService *)user;
    FengLspScheduledRequest request = {0};

    while (feng_lsp_scheduler_take(&service->request_scheduler, &request)) {
        FengLspTraceEvent trace_event = feng_lsp_trace_begin(&service->trace);
        unsigned int document_version = service->trace.enabled
            ? trace_document_version(service, &request)
            : 0U;
        size_t generation;
        bool cache_hit;
        bool cancelled;
        bool ok;

        pthread_mutex_lock(&service->protocol_output_mutex);
        if (strcmp(request.method, kCancelledResponseMethod) == 0) {
            FengLspJsonValue cancelled_id = {
                .type = FENG_LSP_JSON_INVALID,
                .start = request.payload,
                .end = request.payload + request.payload_length,
                .value_start = request.payload,
                .value_end = request.payload + request.payload_length
            };

            ok = !service->protocol_output_failed &&
                 send_error_response(service->protocol_output,
                                     cancelled_id,
                                     -32800,
                                     "Request cancelled");
        } else {
            ok = !service->protocol_output_failed &&
                 service_handle_payload_unlocked(service,
                                                 service->protocol_output,
                                                 request.payload,
                                                 request.payload_length,
                                                 service->errors);
        }
        if (!ok) {
            service->protocol_output_failed = true;
            service->exit_code = 1;
        }
        pthread_mutex_unlock(&service->protocol_output_mutex);
        cancelled = request.has_id && request.request_id != NULL &&
                    feng_lsp_scheduler_active_cancelled(&service->request_scheduler,
                                                        request.request_id);
        pthread_mutex_lock(&service->analysis_mutex);
        generation = service->document_revision;
        cache_hit = service->last_successful_analysis.analysis != NULL ||
                    service->symbol_index != NULL;
        pthread_mutex_unlock(&service->analysis_mutex);
        feng_lsp_trace_end(&service->trace,
                           trace_event,
                           service->errors,
                           request.method,
                           request.request_id,
                           document_version,
                           generation,
                           "memory-pipeline",
                           cache_hit,
                           0U,
                           cancelled);
        feng_lsp_scheduler_finish_active(&service->request_scheduler);
        feng_lsp_scheduled_request_dispose(&request);
    }
    return NULL;
}


FengLspService *feng_lsp_service_create(void) {
    FengLspService *service = (FengLspService *)calloc(1U, sizeof(FengLspService));

    if (service == NULL) {
        return NULL;
    }
    feng_lsp_trace_init(&service->trace);
    if (pthread_mutex_init(&service->documents_mutex, NULL) != 0) {
        free(service);
        return NULL;
    }
    if (pthread_mutex_init(&service->analysis_mutex, NULL) != 0) {
        pthread_mutex_destroy(&service->documents_mutex);
        free(service);
        return NULL;
    }
    if (pthread_mutex_init(&service->protocol_output_mutex, NULL) != 0) {
        pthread_mutex_destroy(&service->analysis_mutex);
        pthread_mutex_destroy(&service->documents_mutex);
        free(service);
        return NULL;
    }
    if (pthread_cond_init(&service->analysis_condition, NULL) != 0) {
        pthread_mutex_destroy(&service->protocol_output_mutex);
        pthread_mutex_destroy(&service->analysis_mutex);
        pthread_mutex_destroy(&service->documents_mutex);
        free(service);
        return NULL;
    }
    if (!feng_lsp_scheduler_init(&service->request_scheduler)) {
        pthread_cond_destroy(&service->analysis_condition);
        pthread_mutex_destroy(&service->protocol_output_mutex);
        pthread_mutex_destroy(&service->analysis_mutex);
        pthread_mutex_destroy(&service->documents_mutex);
        free(service);
        return NULL;
    }
    if (pthread_create(&service->analyzer_thread,
                       NULL,
                       background_analyzer_main,
                       service) != 0) {
        pthread_cond_destroy(&service->analysis_condition);
        pthread_mutex_destroy(&service->protocol_output_mutex);
        pthread_mutex_destroy(&service->analysis_mutex);
        pthread_mutex_destroy(&service->documents_mutex);
        feng_lsp_scheduler_dispose(&service->request_scheduler);
        free(service);
        return NULL;
    }
    service->analysis_thread_started = true;
    if (pthread_create(&service->request_thread,
                       NULL,
                       request_worker_main,
                       service) != 0) {
        pthread_mutex_lock(&service->analysis_mutex);
        service->analysis_stop_requested = true;
        pthread_cond_signal(&service->analysis_condition);
        pthread_mutex_unlock(&service->analysis_mutex);
        pthread_join(service->analyzer_thread, NULL);
        service->analysis_thread_started = false;
        feng_lsp_scheduler_dispose(&service->request_scheduler);
        pthread_cond_destroy(&service->analysis_condition);
        pthread_mutex_destroy(&service->protocol_output_mutex);
        pthread_mutex_destroy(&service->analysis_mutex);
        pthread_mutex_destroy(&service->documents_mutex);
        free(service);
        return NULL;
    }
    service->request_thread_started = true;
    return service;
}

void feng_lsp_service_free(FengLspService *service) {
    size_t index;

    if (service == NULL) {
        return;
    }
    if (service->request_thread_started) {
        feng_lsp_scheduler_stop(&service->request_scheduler);
        pthread_join(service->request_thread, NULL);
    }
    if (service->analysis_thread_started) {
        pthread_mutex_lock(&service->analysis_mutex);
        service->analysis_stop_requested = true;
        pthread_cond_signal(&service->analysis_condition);
        pthread_mutex_unlock(&service->analysis_mutex);
        pthread_join(service->analyzer_thread, NULL);
    }
    free(service->pending_analysis_uri);
    for (index = 0U; index < service->document_count; ++index) {
        document_dispose_derived(&service->documents[index]);
        free(service->documents[index].uri);
        free(service->documents[index].path);
        free(service->documents[index].text);
        feng_lsp_line_index_dispose(&service->documents[index].lines);
    }
    free(service->documents);
    session_dispose(&service->last_successful_analysis);
    feng_symbol_provider_free(service->symbol_index);
    module_index_dispose(&service->module_index);
    feng_lsp_scheduler_dispose(&service->request_scheduler);
    pthread_cond_destroy(&service->analysis_condition);
    pthread_mutex_destroy(&service->protocol_output_mutex);
    pthread_mutex_destroy(&service->analysis_mutex);
    pthread_mutex_destroy(&service->documents_mutex);
    free(service);
}

static bool handle_initialize(FengLspService *service,
                              FILE *output,
                              FengLspJsonValue id,
                              FengLspJsonValue params) {
    if (service != NULL) {
        service->hover_markup_kind = hover_markup_kind_from_initialize_params(params);
    }
    return send_json_response(output,
                              id,
                              "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":2,\"save\":{\"includeText\":false}},\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"renameProvider\":{\"prepareProvider\":true},\"completionProvider\":{\"resolveProvider\":true,\"triggerCharacters\":[\".\",\"_\",\"@\",\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\",\"r\",\"s\",\"t\",\"u\",\"v\",\"w\",\"x\",\"y\",\"z\",\"A\",\"B\",\"C\",\"D\",\"E\",\"F\",\"G\",\"H\",\"I\",\"J\",\"K\",\"L\",\"M\",\"N\",\"O\",\"P\",\"Q\",\"R\",\"S\",\"T\",\"U\",\"V\",\"W\",\"X\",\"Y\",\"Z\"]},\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]}},\"serverInfo\":{\"name\":\"feng\"}}");
}

static bool service_handle_payload_unlocked(FengLspService *service,
                                            FILE *output,
                                            const char *payload,
                                            size_t payload_length,
                                            FILE *errors) {
    FengLspMessage message = {0};
    FengLspParseStatus status = parse_jsonrpc_message(payload, payload_length, &message);

    service->errors = errors;
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
        ok = message.has_id ? handle_initialize(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "shutdown") == 0) {
        service->shutdown_requested = true;
        ok = message.has_id ? send_json_response(output, message.id, "null")
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "exit") == 0) {
        service->should_exit = true;
        service->exit_code = service->shutdown_requested ? 0 : 1;
    } else if (strcmp(message.method, "initialized") == 0 ||
               strcmp(message.method, "$/cancelRequest") == 0 ||
               strcmp(message.method, "$/setTrace") == 0) {
        ok = true;
    } else if (strcmp(message.method, "textDocument/didOpen") == 0) {
        FengLspJsonValue text_document = {0};
        FengLspJsonValue uri_value = {0};
        FengLspJsonValue text_value = {0};
        FengLspJsonValue version_value = {0};
        char *uri;
        char *text;
        unsigned int version = 0U;

        if (!json_object_get(message.params, "textDocument", &text_document) ||
            !json_object_get(text_document, "uri", &uri_value) ||
            !json_object_get(text_document, "text", &text_value)) {
            fprintf(errors, "lsp: textDocument/didOpen: missing required params\n");
            /* Malformed notification from client — log and continue; do not kill server */
        } else {
            uri = json_string_dup(uri_value);
            text = json_string_dup(text_value);
            if (json_object_get(text_document, "version", &version_value)) {
                (void)json_u32(version_value, &version);
            }
            if (uri == NULL) {
                fprintf(errors, "lsp: textDocument/didOpen: failed to decode URI\n");
            } else if (text == NULL) {
                fprintf(errors, "lsp: textDocument/didOpen: failed to decode text for '%s'\n", uri);
            } else {
                pthread_mutex_lock(&service->documents_mutex);
                if (!upsert_document(service, uri, text, version)) {
                    /* upsert_document already logged the OOM; document not tracked but server continues */
                    fprintf(errors, "lsp: textDocument/didOpen: document not tracked: '%s'\n", uri);
                } else {
                    schedule_background_analysis(service,
                                                 find_document(service, uri),
                                                 false,
                                                 true,
                                                 false);
                }
                pthread_mutex_unlock(&service->documents_mutex);
            }
            free(uri);
            free(text);
        }
    } else if (strcmp(message.method, "textDocument/didChange") == 0) {
        FengLspJsonValue text_document = {0};
        FengLspJsonValue uri_value = {0};
        FengLspJsonValue changes = {0};
        char *uri;

        if (!json_object_get(message.params, "textDocument", &text_document) ||
            !json_object_get(text_document, "uri", &uri_value) ||
            !json_object_get(message.params, "contentChanges", &changes)) {
            fprintf(errors, "lsp: textDocument/didChange: missing required params\n");
            /* Malformed notification from client — log and continue; do not kill server */
        } else {
            FengLspJsonValue version_value = {0};
            FengLspDocument *document;
            char *updated_text = NULL;
            unsigned int version = 0U;
            size_t change_index = 0U;
            bool valid_changes = true;

            uri = json_string_dup(uri_value);
            if (uri == NULL) {
                fprintf(errors, "lsp: textDocument/didChange: failed to decode URI\n");
            } else {
                document = find_document(service, uri);
                version = document != NULL ? document->version + 1U : 0U;
                if (json_object_get(text_document, "version", &version_value)) {
                    (void)json_u32(version_value, &version);
                }
                updated_text = document != NULL ? dup_cstr(document->text) : NULL;
                while (valid_changes) {
                    FengLspJsonValue change = {0};
                    FengLspJsonValue text_value = {0};
                    FengLspJsonValue range = {0};
                    char *replacement;

                    if (!json_array_get(changes, change_index, &change)) {
                        break;
                    }
                    ++change_index;
                    if (!json_object_get(change, "text", &text_value) ||
                        (replacement = json_string_dup(text_value)) == NULL) {
                        valid_changes = false;
                        break;
                    }
                    if (json_object_get(change, "range", &range)) {
                        FengLspJsonValue start = {0};
                        FengLspJsonValue end = {0};
                        FengLspJsonValue start_line_value = {0};
                        FengLspJsonValue start_char_value = {0};
                        FengLspJsonValue end_line_value = {0};
                        FengLspJsonValue end_char_value = {0};
                        unsigned int start_line;
                        unsigned int start_character;
                        unsigned int end_line;
                        unsigned int end_character;
                        char *next_text = NULL;

                        if (updated_text == NULL ||
                            !json_object_get(range, "start", &start) ||
                            !json_object_get(range, "end", &end) ||
                            !json_object_get(start, "line", &start_line_value) ||
                            !json_object_get(start, "character", &start_char_value) ||
                            !json_object_get(end, "line", &end_line_value) ||
                            !json_object_get(end, "character", &end_char_value) ||
                            !json_u32(start_line_value, &start_line) ||
                            !json_u32(start_char_value, &start_character) ||
                            !json_u32(end_line_value, &end_line) ||
                            !json_u32(end_char_value, &end_character)) {
                            valid_changes = false;
                        } else {
                            next_text = apply_incremental_text_edit(updated_text,
                                                                    start_line,
                                                                    start_character,
                                                                    end_line,
                                                                    end_character,
                                                                    replacement);
                            if (next_text == NULL) {
                                valid_changes = false;
                            } else {
                                free(updated_text);
                                updated_text = next_text;
                            }
                        }
                    } else {
                        free(updated_text);
                        updated_text = replacement;
                        replacement = NULL;
                    }
                    free(replacement);
                }
                pthread_mutex_lock(&service->documents_mutex);
                if (document == NULL || change_index == 0U || !valid_changes ||
                    updated_text == NULL ||
                    !upsert_document(service, uri, updated_text, version)) {
                    fprintf(errors,
                            "lsp: textDocument/didChange: failed to update tracked document '%s'\n",
                            uri);
                } else {
                    schedule_background_analysis(service,
                                                 find_document(service, uri),
                                                 false,
                                                 false,
                                                 true);
                }
                pthread_mutex_unlock(&service->documents_mutex);
            }
            free(uri);
            free(updated_text);
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
                FengLspDocument *saved_doc;

                pthread_mutex_lock(&service->documents_mutex);
                saved_doc = find_document(service, uri);
                if (saved_doc != NULL) {
                    saved_doc->dirty = false;
                    ok = publish_current_parse_diagnostics(output, saved_doc);
                    schedule_background_analysis(service, saved_doc, true, true, true);
                }
                pthread_mutex_unlock(&service->documents_mutex);
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
                pthread_mutex_lock(&service->documents_mutex);
                document = find_document(service, uri);
                ok = document == NULL || publish_empty_diagnostics(output, document);
                if (!ok) {
                    fprintf(errors,
                            "lsp: textDocument/didClose: failed to clear diagnostics for '%s'\n",
                            uri);
                } else {
                    remove_document(service, uri);
                }
                pthread_mutex_unlock(&service->documents_mutex);
            }
            free(uri);
        }
    } else if (strcmp(message.method, "textDocument/hover") == 0) {
        ok = message.has_id ? handle_hover_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/definition") == 0) {
        ok = message.has_id ? handle_definition_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/references") == 0) {
        ok = message.has_id ? handle_references_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/prepareRename") == 0) {
        ok = message.has_id ? handle_prepare_rename_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/rename") == 0) {
        ok = message.has_id ? handle_rename_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/completion") == 0) {
        ok = message.has_id ? handle_completion_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "completionItem/resolve") == 0) {
        ok = message.has_id ? handle_completion_resolve_request(service, output, message.id, message.params)
                            : send_error_response(output, null_id, -32600, "Invalid Request");
    } else if (strcmp(message.method, "textDocument/signatureHelp") == 0) {
        ok = message.has_id ? handle_signature_help_request(service, output, message.id, message.params)
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

bool feng_lsp_service_submit_payload(FengLspService *service,
                                     FILE *output,
                                     const char *payload,
                                     size_t payload_length,
                                     FILE *errors) {
    FengLspMessage message = {0};
    FengLspParseStatus status;
    FengLspScheduledRequest request = {0};

    if (service == NULL || output == NULL || payload == NULL || errors == NULL) {
        return false;
    }
    if (service->protocol_output == NULL) {
        pthread_mutex_lock(&service->protocol_output_mutex);
        service->protocol_output = output;
        service->errors = errors;
        pthread_mutex_unlock(&service->protocol_output_mutex);
    }
    status = parse_jsonrpc_message(payload, payload_length, &message);
    if (status == FENG_LSP_PARSE_OK && message.method != NULL &&
        strcmp(message.method, "$/cancelRequest") == 0) {
        FengLspJsonValue cancelled_id = {0};
        char *request_id = NULL;
        bool was_queued = false;

        if (json_object_get(message.params, "id", &cancelled_id)) {
            request_id = dup_range(cancelled_id.start, cancelled_id.end);
        }
        if (request_id != NULL &&
            feng_lsp_scheduler_cancel(&service->request_scheduler,
                                      request_id,
                                      &was_queued) &&
            was_queued) {
            FengLspScheduledRequest cancelled_response = {0};

            cancelled_response.payload = dup_cstr(request_id);
            cancelled_response.payload_length = strlen(request_id);
            cancelled_response.method = dup_cstr(kCancelledResponseMethod);
            cancelled_response.request_id = dup_cstr(request_id);
            cancelled_response.has_id = true;
            cancelled_response.priority = FENG_LSP_PRIORITY_HIGHEST;
            if (cancelled_response.payload == NULL ||
                cancelled_response.method == NULL ||
                cancelled_response.request_id == NULL ||
                !feng_lsp_scheduler_submit(&service->request_scheduler,
                                           &cancelled_response)) {
                feng_lsp_scheduled_request_dispose(&cancelled_response);
                free(request_id);
                message_dispose(&message);
                return false;
            }
        }
        free(request_id);
        message_dispose(&message);
        return !service->protocol_output_failed;
    }
    request.payload = dup_range(payload, payload + payload_length);
    request.payload_length = payload_length;
    if (request.payload == NULL) {
        message_dispose(&message);
        return false;
    }
    if (status == FENG_LSP_PARSE_OK) {
        request.method = dup_cstr(message.method != NULL ? message.method : "");
        request.has_id = message.has_id;
        if (message.has_id) {
            request.request_id = dup_range(message.id.start, message.id.end);
        }
        request.priority = request_priority_for_method(message.method);
        if (message.method != NULL && strcmp(message.method, "exit") == 0) {
            service->exit_received = true;
        }
    } else {
        request.method = dup_cstr("");
    }
    message_dispose(&message);
    if (request.method == NULL || (request.has_id && request.request_id == NULL) ||
        !feng_lsp_scheduler_submit(&service->request_scheduler, &request)) {
        feng_lsp_scheduled_request_dispose(&request);
        return false;
    }
    return true;
}

bool feng_lsp_service_should_exit(const FengLspService *service) {
    return service != NULL && (service->exit_received || service->should_exit);
}

int feng_lsp_service_exit_code(const FengLspService *service) {
    return service != NULL ? service->exit_code : 1;
}
