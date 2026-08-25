#include "parser/parser.h"

#include <stdlib.h>
#include <string.h>

#include "lexer/lexer.h"

typedef struct Parser {
    const char *source;
    size_t length;
    const char *path;
    FengToken *tokens;
    size_t token_count;
    size_t token_capacity;
    size_t current;
    FengParseError error;
    /* Pending `>` count from splitting a `>>` (SHR) token while parsing
     * nested generic type argument lists, e.g. Map<string, List<int>>.
     * See parser_consume_gt(). */
    int pending_gt;
    /* When true, parse_postfix skips object literal suffix detection so
     * that a `{` following an identifier is not consumed as part of an
     * object literal. Used by if-statement / if-expression condition
     * parsing to preserve the `{` for the body block. */
    bool suppress_object_literal_suffix;
} Parser;

#define APPEND_VALUE(parser, items, count, capacity, value) \
    append_raw((parser), (void **)&(items), &(count), &(capacity), sizeof(*(items)), &(value))

#define FENG_TUPLE_MAX_ITEMS 8U

static const char *const k_tuple_item_names[FENG_TUPLE_MAX_ITEMS] = {
    "item1", "item2", "item3", "item4", "item5", "item6", "item7", "item8"
};

static FengProgram *parse_program(Parser *parser);
static FengDecl *parse_declaration(Parser *parser);
static FengBlock *parse_block(Parser *parser);
static FengStmt *parse_statement(Parser *parser);
static FengStmt *parse_simple_statement(Parser *parser, FengTokenKind terminator);
static FengExpr *parse_expression(Parser *parser);
static FengExpr *parse_unary(Parser *parser);
static FengTypeRef *parse_type_ref(Parser *parser);
static FengBinding parse_binding_core(Parser *parser,
                                      FengMutability mutability,
                                      bool require_type,
                                      bool allow_destructure);
static bool parser_match(Parser *parser, FengTokenKind kind);
static void free_type_params(FengTypeParam *params, size_t count);
static void free_type_ref(FengTypeRef *type_ref);
static void free_expr(FengExpr *expr);
static void free_stmt(FengStmt *stmt);
static void free_block(FengBlock *block);
static void free_decl(FengDecl *decl);
static void free_annotations(FengAnnotation *annotations, size_t count);
static void convert_trailing_yield_stmt_to_expr(Parser *parser, FengBlock *block);
static void free_parameters(FengParameter *params, size_t count);
static void free_type_member(FengTypeMember *member);
static void free_enum_items(FengEnumItem *items, size_t count);
static void free_try_catch_clauses(FengTryCatchClause *clauses, size_t count);

/* Return whether a parsed annotation list contains the requested built-in fact. */
static bool parsed_annotations_contain_kind(const FengAnnotation *annotations,
                                            size_t annotation_count,
                                            FengAnnotationKind kind) {
    size_t annotation_index;

    for (annotation_index = 0U; annotation_index < annotation_count; ++annotation_index) {
        if (annotations[annotation_index].builtin_kind == kind) {
            return true;
        }
    }
    return false;
}

static void free_annotation_fields(FengAnnotation *annotation) {
    size_t arg_index;

    if (annotation == NULL) {
        return;
    }
    for (arg_index = 0U; arg_index < annotation->arg_count; ++arg_index) {
        if (annotation->argument_kind == FENG_ANNOTATION_ARGUMENT_TYPE) {
            free_type_ref(annotation->type_args[arg_index]);
        } else {
            free_expr(annotation->args[arg_index]);
        }
    }
    free(annotation->args);
}

static bool append_raw(Parser *parser,
                       void **items,
                       size_t *count,
                       size_t *capacity,
                       size_t item_size,
                       const void *value) {
    void *new_items;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0U) ? 4U : (*capacity * 2U);

        new_items = realloc(*items, new_capacity * item_size);
        if (new_items == NULL) {
            if (parser->error.message == NULL) {
                parser->error.code = "IE0001";
                parser->error.message = "out of memory";
                parser->error.token = parser->tokens[parser->current];
            }
            return false;
        }

        *items = new_items;
        *capacity = new_capacity;
    }

    memcpy((char *)(*items) + (*count * item_size), value, item_size);
    ++(*count);
    return true;
}

static FengSlice slice_from_token(const FengToken *token) {
    FengSlice slice;

    slice.data = token->lexeme;
    slice.length = token->length;
    return slice;
}

static bool slice_equals(FengSlice left, FengSlice right) {
    return left.length == right.length && memcmp(left.data, right.data, left.length) == 0;
}

static FengSlice doc_comment_from_token(const FengToken *token) {
    FengSlice slice = {0};

    if (token->leading_doc != NULL && token->leading_doc_length > 0U) {
        slice.data = token->leading_doc;
        slice.length = token->leading_doc_length;
    }

    return slice;
}

static const FengToken *parser_current(const Parser *parser) {
    return &parser->tokens[parser->current];
}

static const FengToken *parser_peek(const Parser *parser, size_t lookahead) {
    size_t index = parser->current + lookahead;

    if (index >= parser->token_count) {
        return &parser->tokens[parser->token_count - 1U];
    }

    return &parser->tokens[index];
}

static const FengToken *parser_previous(const Parser *parser) {
    if (parser->current == 0U) {
        return &parser->tokens[0];
    }

    return &parser->tokens[parser->current - 1U];
}

static FengToken parser_current_token(const Parser *parser) {
    return *parser_current(parser);
}

static FengToken parser_previous_token(const Parser *parser) {
    return *parser_previous(parser);
}

static bool parser_is_at_end(const Parser *parser) {
    return parser_current(parser)->kind == FENG_TOKEN_EOF;
}

static const FengToken *parser_advance(Parser *parser) {
    if (!parser_is_at_end(parser)) {
        ++parser->current;
    }
    return parser_previous(parser);
}

static bool parser_check(const Parser *parser, FengTokenKind kind) {
    return parser_current(parser)->kind == kind;
}

static bool parser_starts_callable_signature(const Parser *parser) {
    return parser_check(parser, FENG_TOKEN_IDENTIFIER) &&
           parser_peek(parser, 1U)->kind == FENG_TOKEN_LPAREN;
}

/* Consume one `>` in a generic type argument context.  Handles the case
 * where `>>` was produced as FENG_TOKEN_SHR by the lexer: the first call
 * advances past SHR and records one pending `>`, while the second call
 * satisfies that pending entry without advancing the token stream. */
static bool parser_consume_gt(Parser *parser) {
    if (parser->pending_gt > 0) {
        --parser->pending_gt;
        return true;
    }
    if (parser_check(parser, FENG_TOKEN_SHR)) {
        (void)parser_advance(parser);
        parser->pending_gt = 1;
        return true;
    }
    return parser_match(parser, FENG_TOKEN_GT);
}

static bool parser_starts_binding_without_keyword(const Parser *parser) {
    return parser_check(parser, FENG_TOKEN_IDENTIFIER) &&
           (parser_peek(parser, 1U)->kind == FENG_TOKEN_COLON ||
            parser_peek(parser, 1U)->kind == FENG_TOKEN_ASSIGN);
}

static bool parser_starts_typed_binding_without_keyword(const Parser *parser) {
    return parser_check(parser, FENG_TOKEN_IDENTIFIER) &&
           parser_peek(parser, 1U)->kind == FENG_TOKEN_COLON;
}

static bool parser_match(Parser *parser, FengTokenKind kind) {
    if (!parser_check(parser, kind)) {
        return false;
    }
    (void)parser_advance(parser);
    return true;
}

static bool token_is_assignment_operator(FengTokenKind kind) {
    switch (kind) {
        case FENG_TOKEN_ASSIGN:
        case FENG_TOKEN_PLUS_ASSIGN:
        case FENG_TOKEN_MINUS_ASSIGN:
        case FENG_TOKEN_STAR_ASSIGN:
        case FENG_TOKEN_SLASH_ASSIGN:
        case FENG_TOKEN_PERCENT_ASSIGN:
        case FENG_TOKEN_AMP_ASSIGN:
        case FENG_TOKEN_PIPE_ASSIGN:
        case FENG_TOKEN_CARET_ASSIGN:
        case FENG_TOKEN_SHL_ASSIGN:
        case FENG_TOKEN_SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

static bool parser_match_assignment_operator(Parser *parser, FengTokenKind *out_kind) {
    FengTokenKind kind = parser_current(parser)->kind;

    if (!token_is_assignment_operator(kind)) {
        return false;
    }

    (void)parser_advance(parser);
    if (out_kind != NULL) {
        *out_kind = kind;
    }
    return true;
}

static bool parser_error_at(Parser *parser, const FengToken *token, const char *code, const char *message) {
    if (parser->error.message == NULL) {
        parser->error.code = code;
        parser->error.message = message;
        parser->error.token = *token;
    }
    return false;
}

static bool parser_error_current(Parser *parser, const char *code, const char *message) {
    return parser_error_at(parser, parser_current(parser), code, message);
}

static bool parser_expect(Parser *parser, FengTokenKind kind, const char *code, const char *message) {
    if (parser_check(parser, kind)) {
        (void)parser_advance(parser);
        return true;
    }
    return parser_error_current(parser, code, message);
}

static bool token_is_identifier_like(const FengToken *token, bool allow_void) {
    return token->kind == FENG_TOKEN_IDENTIFIER ||
           (allow_void && (token->kind == FENG_TOKEN_KW_VOID ||
                           token->kind == FENG_TOKEN_KW_UNKNOWN));
}

static bool token_is_member_name_like(const FengToken *token) {
    return token->kind == FENG_TOKEN_IDENTIFIER || token->kind == FENG_TOKEN_KW_UNKNOWN;
}

static bool parser_expect_member_name(Parser *parser,
                                      FengSlice *out_name,
                                      const char *code,
                                      const char *message) {
    if (!token_is_member_name_like(parser_current(parser))) {
        return parser_error_current(parser, code, message);
    }

    *out_name = slice_from_token(parser_current(parser));
    (void)parser_advance(parser);
    return true;
}

static bool parser_expect_identifier_like(Parser *parser,
                                          FengSlice *out_name,
                                          bool allow_void,
                                          const char *code,
                                          const char *message) {
    if (!token_is_identifier_like(parser_current(parser), allow_void)) {
        return parser_error_current(parser, code, message);
    }

    *out_name = slice_from_token(parser_current(parser));
    (void)parser_advance(parser);
    return true;
}

static bool parser_tokenize(Parser *parser) {
    FengLexer lexer;

    feng_lexer_init(&lexer, parser->source, parser->length, parser->path);
    for (;;) {
        FengToken token = feng_lexer_next(&lexer);

        if (!APPEND_VALUE(parser, parser->tokens, parser->token_count, parser->token_capacity, token)) {
            return false;
        }

        if (token.kind == FENG_TOKEN_ERROR) {
            parser->error.code = token.error_code;
            parser->error.message = token.error_message;
            parser->error.token = token;
            return false;
        }

        if (token.kind == FENG_TOKEN_EOF) {
            break;
        }
    }

    return true;
}

static FengVisibility parse_visibility(Parser *parser) {
    if (parser_match(parser, FENG_TOKEN_KW_OPEN)) {
        return FENG_VISIBILITY_PUBLIC;
    }
    if (parser_match(parser, FENG_TOKEN_KW_SEAL)) {
        return FENG_VISIBILITY_PRIVATE;
    }
    return FENG_VISIBILITY_DEFAULT;
}

static bool parse_path(Parser *parser,
                       bool allow_void,
                       FengSlice **out_segments,
                       size_t *out_count,
                       const char *code,
                       const char *message) {
    FengSlice *segments = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    FengSlice segment;

    if (!parser_expect_identifier_like(parser, &segment, allow_void, code, message)) {
        free(segments);
        return false;
    }
    if (!APPEND_VALUE(parser, segments, count, capacity, segment)) {
        free(segments);
        return false;
    }

    while (parser_match(parser, FENG_TOKEN_DOT)) {
        if (!parser_expect_identifier_like(parser,
                                           &segment,
                                           false,
                                           "SE0002",
                                           "expected an identifier after '.' in a qualified name")) {
            free(segments);
            return false;
        }
        if (!APPEND_VALUE(parser, segments, count, capacity, segment)) {
            free(segments);
            return false;
        }
    }

    *out_segments = segments;
    *out_count = count;
    return true;
}

static FengAnnotation *parse_annotations(Parser *parser, size_t *out_count) {
    FengAnnotation *annotations = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    while (parser_check(parser, FENG_TOKEN_ANNOTATION)) {
        FengAnnotation annotation;
        FengToken token = *parser_current(parser);

        annotation.token = token;
        annotation.name = (FengSlice){token.lexeme + 1, token.length - 1U};
        annotation.builtin_kind = token.annotation_kind;
        annotation.argument_kind = FENG_ANNOTATION_ARGUMENT_NONE;
        annotation.args = NULL;
        annotation.arg_count = 0U;
        (void)parser_advance(parser);

        if (parser_match(parser, FENG_TOKEN_LPAREN)) {
            size_t arg_capacity = 0U;

            annotation.argument_kind =
                annotation.builtin_kind == FENG_ANNOTATION_FRIEND
                    ? FENG_ANNOTATION_ARGUMENT_TYPE
                    : FENG_ANNOTATION_ARGUMENT_EXPRESSION;

            if (!parser_check(parser, FENG_TOKEN_RPAREN)) {
                do {
                    if (annotation.argument_kind == FENG_ANNOTATION_ARGUMENT_TYPE) {
                        FengTypeRef *arg = parse_type_ref(parser);

                        if (arg == NULL) {
                            free_annotations(annotations, count);
                            free_annotation_fields(&annotation);
                            return NULL;
                        }
                        if (!APPEND_VALUE(parser,
                                          annotation.type_args,
                                          annotation.arg_count,
                                          arg_capacity,
                                          arg)) {
                            free_type_ref(arg);
                            free_annotations(annotations, count);
                            free_annotation_fields(&annotation);
                            return NULL;
                        }
                    } else {
                        FengExpr *arg = parse_expression(parser);

                        if (arg == NULL) {
                            free_annotations(annotations, count);
                            free_annotation_fields(&annotation);
                            return NULL;
                        }
                        if (!APPEND_VALUE(parser,
                                          annotation.args,
                                          annotation.arg_count,
                                          arg_capacity,
                                          arg)) {
                            free_expr(arg);
                            free_annotations(annotations, count);
                            free_annotation_fields(&annotation);
                            return NULL;
                        }
                    }
                } while (parser_match(parser, FENG_TOKEN_COMMA));
            }

            if (!parser_expect(parser,
                               FENG_TOKEN_RPAREN,
                               "SE1302", "expected ')' to close annotation argument list")) {
                free_annotations(annotations, count);
                free_annotation_fields(&annotation);
                return NULL;
            }
        }

        if (!APPEND_VALUE(parser, annotations, count, capacity, annotation)) {
            free_annotations(annotations, count);
            free_annotation_fields(&annotation);
            return NULL;
        }
    }

    *out_count = count;
    return annotations;
}

static FengTypeRef *new_type_ref(Parser *parser, FengTypeRefKind kind, FengToken token) {
    FengTypeRef *type_ref = (FengTypeRef *)calloc(1U, sizeof(*type_ref));

    if (type_ref == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
        return NULL;
    }

    type_ref->token = token;
    type_ref->kind = kind;
    return type_ref;
}

/* Release a FengTypeParam array (used for both generic declarations and
 * cleanup during parse errors). */
static void free_type_params(FengTypeParam *params, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        free_type_ref(params[index].constraint);
    }
    free(params);
}

static void free_type_arg_refs(FengTypeRef **type_args, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        free_type_ref(type_args[index]);
    }
    free(type_args);
}

/* Parse type arguments: `< T1, T2, ... >` (opening `<` already consumed).
 * The closing `>` may be produced as the first half of `>>` (SHR) and is
 * consumed via parser_consume_gt().
 * Caller must free *out_args on error if this returns false. */
static bool parse_type_args(Parser *parser,
                            FengTypeRef ***out_args,
                            size_t *out_count) {
    size_t capacity = 0U;

    *out_args = NULL;
    *out_count = 0U;
    do {
        FengTypeRef *arg = parse_type_ref(parser);

        if (arg == NULL) {
            free_type_arg_refs(*out_args, *out_count);
            *out_args = NULL;
            *out_count = 0U;
            return false;
        }
        if (!APPEND_VALUE(parser, *out_args, *out_count, capacity, arg)) {
            free_type_ref(arg);
            free_type_arg_refs(*out_args, *out_count);
            *out_args = NULL;
            *out_count = 0U;
            return false;
        }
    } while (parser_match(parser, FENG_TOKEN_COMMA));

    if (!parser_consume_gt(parser)) {
        (void)parser_error_current(parser, "SE0610", "expected '>' to close type argument list");
        free_type_arg_refs(*out_args, *out_count);
        *out_args = NULL;
        *out_count = 0U;
        return false;
    }
    return true;
}

/* Parse type parameter definitions: `< T1: Constraint1, T2, ... >`
 * (opening `<` already consumed).  Each entry may carry an optional
 * `: Constraint` bound.  Closing `>` is consumed via parser_consume_gt(). */
static bool parse_type_params(Parser *parser,
                              FengTypeParam **out_params,
                              size_t *out_count) {
    size_t capacity = 0U;

    *out_params = NULL;
    *out_count = 0U;
    do {
        FengTypeParam param;

        memset(&param, 0, sizeof(param));
        param.token = parser_current_token(parser);
        if (!parser_expect_identifier_like(parser,
                                           &param.name,
                                           false,
                                           "SE0002", "expected a type parameter name")) {
            free_type_params(*out_params, *out_count);
            *out_params = NULL;
            *out_count = 0U;
            return false;
        }
        if (parser_match(parser, FENG_TOKEN_COLON)) {
            param.constraint = parse_type_ref(parser);
            if (param.constraint == NULL) {
                free_type_params(*out_params, *out_count);
                *out_params = NULL;
                *out_count = 0U;
                return false;
            }
        }
        if (!APPEND_VALUE(parser, *out_params, *out_count, capacity, param)) {
            free_type_ref(param.constraint);
            free_type_params(*out_params, *out_count);
            *out_params = NULL;
            *out_count = 0U;
            return false;
        }
    } while (parser_match(parser, FENG_TOKEN_COMMA));

    if (!parser_consume_gt(parser)) {
        (void)parser_error_current(parser, "SE0610", "expected '>' to close type parameter list");
        free_type_params(*out_params, *out_count);
        *out_params = NULL;
        *out_count = 0U;
        return false;
    }
    return true;
}

static FengTypeRef *parse_type_ref(Parser *parser) {
    FengTypeRef *type_ref;
    FengToken start_token = parser_current_token(parser);

    type_ref = new_type_ref(parser, FENG_TYPE_REF_NAMED, start_token);
    if (type_ref == NULL) {
        return NULL;
    }

    if (!parse_path(parser,
                    true,
                    &type_ref->as.named.segments,
                    &type_ref->as.named.segment_count,
                    "SE0002", "expected a type name")) {
        free_type_ref(type_ref);
        return NULL;
    }

    /* Generic type arguments: Name<T1, T2> */
    if (parser_match(parser, FENG_TOKEN_LT)) {
        if (!parse_type_args(parser,
                             &type_ref->as.named.type_args,
                             &type_ref->as.named.type_arg_count)) {
            free_type_ref(type_ref);
            return NULL;
        }
    }

    for (;;) {
        if (parser_match(parser, FENG_TOKEN_STAR)) {
            FengTypeRef *wrapper = new_type_ref(parser, FENG_TYPE_REF_POINTER, start_token);

            if (wrapper == NULL) {
                free_type_ref(type_ref);
                return NULL;
            }
            wrapper->as.inner = type_ref;
            type_ref = wrapper;
            continue;
        }

        if (!parser_match(parser, FENG_TOKEN_LBRACKET)) {
            break;
        }

        FengTypeRef *wrapper;
        bool layer_writable = false;

        if (parser_match(parser, FENG_TOKEN_NOT)) {
            layer_writable = true;
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_RBRACKET,
                           layer_writable ? "SE0202" : "SE0202",
                           layer_writable
                               ? "expected ']' after '[!' in array type suffix"
                               : "expected ']' to close array type suffix")) {
            free_type_ref(type_ref);
            return NULL;
        }

        wrapper = new_type_ref(parser, FENG_TYPE_REF_ARRAY, start_token);
        if (wrapper == NULL) {
            free_type_ref(type_ref);
            return NULL;
        }
        wrapper->as.inner = type_ref;
        wrapper->array_element_writable = layer_writable;
        type_ref = wrapper;
    }

    return type_ref;
}

static bool parse_parameters(Parser *parser, FengParameter **out_params, size_t *out_count) {
    FengParameter *params = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    if (!parser_expect(parser, FENG_TOKEN_LPAREN, "SE0515", "expected '(' to start parameter list")) {
        return false;
    }

    if (!parser_check(parser, FENG_TOKEN_RPAREN)) {
        do {
            FengParameter param;
            FengToken name_token;

            param.token = parser_current_token(parser);
            param.mutability = FENG_MUTABILITY_DEFAULT;
            param.type = NULL;
            if (parser_match(parser, FENG_TOKEN_KW_LET)) {
                param.mutability = FENG_MUTABILITY_LET;
            } else if (parser_match(parser, FENG_TOKEN_KW_VAR)) {
                param.mutability = FENG_MUTABILITY_VAR;
            }

            name_token = parser_current_token(parser);
            param.token = name_token;

            if (!parser_expect_identifier_like(parser,
                                               &param.name,
                                               false,
                                               "SE0002", "expected a parameter name")) {
                free_parameters(params, count);
                return false;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "SE0504", "expected ':' after parameter name in parameter list")) {
                free_parameters(params, count);
                return false;
            }

            param.type = parse_type_ref(parser);
            if (param.type == NULL) {
                free_parameters(params, count);
                return false;
            }

            /* Variadic parameter: T... is normalised to T[] with is_variadic=true. */
            param.is_variadic = false;
            if (parser_match(parser, FENG_TOKEN_ELLIPSIS)) {
                FengTypeRef *arr_wrapper = new_type_ref(parser, FENG_TYPE_REF_ARRAY, param.token);

                if (arr_wrapper == NULL) {
                    free_type_ref(param.type);
                    free_parameters(params, count);
                    return false;
                }
                arr_wrapper->as.inner = param.type;
                arr_wrapper->array_element_writable = false;
                param.type = arr_wrapper;
                param.is_variadic = true;
            }

            if (!APPEND_VALUE(parser, params, count, capacity, param)) {
                free_type_ref(param.type);
                free_parameters(params, count);
                return false;
            }

            /* Variadic must be the last parameter. */
            if (param.is_variadic) {
                if (parser_check(parser, FENG_TOKEN_COMMA)) {
                    (void)parser_error_current(parser,
                        "SE0502", "variadic parameter must be the last parameter");
                    free_parameters(params, count);
                    return false;
                }
                break;
            }
        } while (parser_match(parser, FENG_TOKEN_COMMA));
    }

    if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0515", "expected ')' to close parameter list")) {
        free_parameters(params, count);
        return false;
    }

    *out_params = params;
    *out_count = count;
    return true;
}

static bool parse_destructure_binding_names(Parser *parser, FengBinding *binding) {
    size_t capacity = 0U;

    if (!parser_expect(parser, FENG_TOKEN_LPAREN, "SE0110", "expected '(' to start destructuring binding")) {
        return false;
    }

    if (parser_check(parser, FENG_TOKEN_RPAREN)) {
        (void)parser_advance(parser);
        return true;
    }

    for (;;) {
        FengSlice name = {0};

        if (parser_check(parser, FENG_TOKEN_LPAREN)) {
            return parser_error_current(parser, "SE0105", "nested destructuring bindings are not supported");
        }
        if (parser_check(parser, FENG_TOKEN_IDENTIFIER)) {
            name = slice_from_token(parser_current(parser));
            (void)parser_advance(parser);
        } else if (!parser_check(parser, FENG_TOKEN_COMMA) &&
                   !parser_check(parser, FENG_TOKEN_RPAREN)) {
            return parser_error_current(parser,
                                        "SE0106", "destructuring positions must be identifiers or empty slots");
        }

        if (binding->destructure_count >= FENG_TUPLE_MAX_ITEMS) {
            return parser_error_current(parser,
                                        "SE0104", "destructuring bindings support at most 8 positions");
        }
        if (!APPEND_VALUE(parser,
                          binding->destructure_names,
                          binding->destructure_count,
                          capacity,
                          name)) {
            return false;
        }

        if (parser_check(parser, FENG_TOKEN_RPAREN)) {
            break;
        }
        if (!parser_expect(parser, FENG_TOKEN_COMMA, "SE0110", "expected ',' between destructuring positions")) {
            return false;
        }
        if (parser_check(parser, FENG_TOKEN_RPAREN)) {
            FengSlice empty = {0};

            if (binding->destructure_count >= FENG_TUPLE_MAX_ITEMS) {
                return parser_error_current(parser,
                                            "SE0104", "destructuring bindings support at most 8 positions");
            }
            if (!APPEND_VALUE(parser,
                              binding->destructure_names,
                              binding->destructure_count,
                              capacity,
                              empty)) {
                return false;
            }
            break;
        }
    }

    if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0110", "expected ')' to close destructuring binding")) {
        return false;
    }
    if (binding->destructure_count == 1U) {
        return parser_error_at(parser,
                               &binding->token,
                               "SE0104", "destructuring bindings require 0 or 2 to 8 positions");
    }
    return true;
}

static FengBinding parse_binding_core(Parser *parser,
                                      FengMutability mutability,
                                      bool require_type,
                                      bool allow_destructure) {
    FengBinding binding;

    binding.token = parser_current_token(parser);
    binding.mutability = mutability;
    binding.name.data = NULL;
    binding.name.length = 0U;
    binding.type = NULL;
    binding.initializer = NULL;
    binding.is_destructure = false;
    binding.destructure_names = NULL;
    binding.destructure_count = 0U;

    if (parser_check(parser, FENG_TOKEN_LPAREN)) {
        if (!allow_destructure) {
            (void)parser_error_current(parser, "SE0109", "destructuring is not valid in this binding context");
            return binding;
        }
        binding.is_destructure = true;
        if (!parse_destructure_binding_names(parser, &binding)) {
            return binding;
        }
        if (parser_match(parser, FENG_TOKEN_COLON)) {
            (void)parser_error_current(parser,
                                       "SE0107", "destructuring bindings cannot use a single type annotation");
            return binding;
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_ASSIGN,
                           "SE0108", "destructuring bindings require an initializer")) {
            return binding;
        }
        binding.initializer = parse_expression(parser);
        return binding;
    }

    if (!parser_expect_identifier_like(parser, &binding.name, false, "SE0002", "expected a binding name")) {
        return binding;
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        binding.type = parse_type_ref(parser);
        if (binding.type == NULL) {
            return binding;
        }
    } else if (require_type) {
        (void)parser_error_current(parser, "SE0302", "type field declarations require ':' after the field name");
        return binding;
    }

    if (parser_match(parser, FENG_TOKEN_ASSIGN)) {
        binding.initializer = parse_expression(parser);
        if (binding.initializer == NULL) {
            return binding;
        }
    }

    if (!require_type && binding.type == NULL && binding.initializer == NULL) {
        (void)parser_error_current(parser,
                       "SE0101", "binding declarations require a type annotation or an initializer");
    }

    return binding;
}

static FengCallableSignature parse_callable_signature(Parser *parser,
                                                     FengToken token,
                                                     FengSlice name,
                                                     bool require_body,
                                                     const char *body_rule_code,
                                                     const char *body_rule_message) {
    FengCallableSignature callable;

    callable.token = token;
    callable.name = name;
    callable.type_params = NULL;
    callable.type_param_count = 0U;
    callable.params = NULL;
    callable.param_count = 0U;
    callable.return_type = NULL;
    callable.body = NULL;
    callable.bound_member_names = NULL;
    callable.bound_member_count = 0U;

    /* Optional generic type parameters: func name<T: Bound, U>(...) */
    if (parser_match(parser, FENG_TOKEN_LT)) {
        if (!parse_type_params(parser,
                               &callable.type_params,
                               &callable.type_param_count)) {
            return callable;
        }
    }

    if (!parse_parameters(parser, &callable.params, &callable.param_count)) {
        return callable;
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        callable.return_type = parse_type_ref(parser);
        if (callable.return_type == NULL) {
            return callable;
        }
    }

    if (require_body) {
        if (!parser_check(parser, FENG_TOKEN_LBRACE)) {
            (void)parser_error_current(parser, body_rule_code, body_rule_message);
            return callable;
        }
        callable.body = parse_block(parser);
    } else {
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           body_rule_code,
                           body_rule_message)) {
            return callable;
        }
    }

    return callable;
}

static FengDecl *new_decl(Parser *parser,
                         FengDeclKind kind,
                         FengToken token,
                         FengSlice doc_comment) {
    FengDecl *decl = (FengDecl *)calloc(1U, sizeof(*decl));

    if (decl == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
        return NULL;
    }

    decl->token = token;
    decl->kind = kind;
    decl->doc_comment = doc_comment;
    return decl;
}

static FengTypeMember *new_type_member(Parser *parser,
                                       FengTypeMemberKind kind,
                                       FengToken token,
                                       FengSlice doc_comment) {
    FengTypeMember *member = (FengTypeMember *)calloc(1U, sizeof(*member));

    if (member == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
        return NULL;
    }

    member->token = token;
    member->kind = kind;
    member->doc_comment = doc_comment;
    return member;
}

static bool parse_spec_satisfaction_list(Parser *parser,
                                         FengTypeRef ***out_list,
                                         size_t *out_count) {
    FengTypeRef **list = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    do {
        FengTypeRef *type_ref = parse_type_ref(parser);

        if (type_ref == NULL) {
            goto fail;
        }
        if (!APPEND_VALUE(parser, list, count, capacity, type_ref)) {
            free_type_ref(type_ref);
            goto fail;
        }
    } while (parser_match(parser, FENG_TOKEN_COMMA));

    *out_list = list;
    *out_count = count;
    return true;

fail:
    {
        size_t index;
        for (index = 0U; index < count; ++index) {
            free_type_ref(list[index]);
        }
        free(list);
    }
    return false;
}

static bool append_type_ref(Parser *parser,
                            FengTypeRef ***out_list,
                            size_t *out_count,
                            size_t *capacity,
                            FengTypeRef *type_ref) {
    return APPEND_VALUE(parser, *out_list, *out_count, *capacity, type_ref);
}

static bool type_ref_is_void_named(const FengTypeRef *type_ref) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED) {
        return false;
    }
    if (type_ref->as.named.segment_count != 1U) {
        return false;
    }
    {
        FengSlice seg = type_ref->as.named.segments[0];
        return seg.length == 4U && memcmp(seg.data, "void", 4U) == 0;
    }
}

static bool parse_enum_integer_literal(Parser *parser, int64_t *out_value) {
    bool negative = false;
    const FengToken *token;
    int64_t value;

    if (out_value == NULL) {
        return false;
    }

    if (parser_match(parser, FENG_TOKEN_MINUS)) {
        negative = true;
    }

    token = parser_current(parser);
    if (token->kind != FENG_TOKEN_INTEGER) {
        (void)parser_error_current(parser, "SE0408", "enum item initializer must be an integer literal");
        return false;
    }

    value = token->value.integer;
    if (negative) {
        value = -value;
    }
    (void)parser_advance(parser);
    *out_value = value;
    return true;
}

static FengDecl *parse_enum_declaration(Parser *parser,
                                        FengSlice doc_comment,
                                        FengVisibility visibility,
                                        bool is_extern,
                                        FengAnnotation *annotations,
                                        size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl;
    FengSlice enum_name;
    size_t item_capacity = 0U;

    if (is_extern) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "SE0401", "enum declarations cannot be marked 'extern'");
        return NULL;
    }

    if (annotation_count > 0U) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser,
                                   "SE0402", "enum declarations do not support annotations in the current phase");
        return NULL;
    }

    decl = new_decl(parser, FENG_DECL_ENUM, name_token, doc_comment);
    if (decl == NULL) {
        return NULL;
    }

    decl->visibility = visibility;

    if (!parser_expect_identifier_like(parser, &enum_name, false, "SE0002", "expected an enum name")) {
        free_decl(decl);
        return NULL;
    }
    decl->as.enum_decl.name = enum_name;

    if (parser_check(parser, FENG_TOKEN_LT)) {
        (void)parser_error_current(parser, "SE0403", "enum declarations do not support generic parameters");
        free_decl(decl);
        return NULL;
    }
    if (parser_check(parser, FENG_TOKEN_COLON)) {
        (void)parser_error_current(parser, "SE0404", "enum declarations cannot declare parent specs");
        free_decl(decl);
        return NULL;
    }
    if (parser_check(parser, FENG_TOKEN_LPAREN)) {
        (void)parser_error_current(parser, "SE0405", "enum declarations cannot declare callable signatures");
        free_decl(decl);
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_LBRACE, "SE0410", "enum declarations require '{...}' after the enum name")) {
        free_decl(decl);
        return NULL;
    }
    if (parser_check(parser, FENG_TOKEN_RBRACE)) {
        (void)parser_error_current(parser, "SE0406", "enum declarations must declare at least one item");
        free_decl(decl);
        return NULL;
    }

    while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
        FengToken item_token = parser_current_token(parser);
        FengEnumItem item;

        memset(&item, 0, sizeof(item));

        if (parser_check(parser, FENG_TOKEN_KW_LET) || parser_check(parser, FENG_TOKEN_KW_VAR) ||
            parser_check(parser, FENG_TOKEN_KW_FUNC) || parser_check(parser, FENG_TOKEN_KW_EXTERN) ||
            parser_check(parser, FENG_TOKEN_KW_OPEN) || parser_check(parser, FENG_TOKEN_KW_SEAL)) {
            (void)parser_error_current(parser,
                                       "SE0407", "enum declarations only allow item names and optional integer literal initializers");
            free_decl(decl);
            return NULL;
        }
        if (parser_check(parser, FENG_TOKEN_ANNOTATION)) {
            (void)parser_error_current(parser, "SE0402", "enum items do not support annotations");
            free_decl(decl);
            return NULL;
        }

        if (!parser_expect_identifier_like(parser, &item.name, false, "SE0002", "expected an enum item name")) {
            free_decl(decl);
            return NULL;
        }
        item.token = item_token;

        if (parser_match(parser, FENG_TOKEN_ASSIGN)) {
            item.has_explicit_value = true;
            if (!parse_enum_integer_literal(parser, &item.explicit_value)) {
                free_decl(decl);
                return NULL;
            }
            if (!parser_check(parser, FENG_TOKEN_COMMA) && !parser_check(parser, FENG_TOKEN_RBRACE)) {
                (void)parser_error_current(parser,
                                           "SE0408", "enum item initializer must be a single integer literal");
                free_decl(decl);
                return NULL;
            }
        }

        if (!APPEND_VALUE(parser,
                          decl->as.enum_decl.items,
                          decl->as.enum_decl.item_count,
                          item_capacity,
                          item)) {
            free_decl(decl);
            return NULL;
        }

        if (!parser_match(parser, FENG_TOKEN_COMMA)) {
            break;
        }
        if (parser_check(parser, FENG_TOKEN_RBRACE)) {
            (void)parser_error_current(parser,
                                       "SE0409", "enum declarations do not allow a trailing ',' after the last item");
            free_decl(decl);
            return NULL;
        }
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE0410", "expected '}' to close enum body")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengTypeMember *new_tuple_field_member(Parser *parser,
                                              FengToken token,
                                              size_t index,
                                              FengTypeRef *type_ref) {
    FengTypeMember *member;
    const char *item_name;

    if (index >= FENG_TUPLE_MAX_ITEMS) {
        (void)parser_error_at(parser, &token, "SE0308", "tuple type declarations support at most 8 elements");
        return NULL;
    }

    member = new_type_member(parser, FENG_TYPE_MEMBER_FIELD, token, (FengSlice){0});
    if (member == NULL) {
        return NULL;
    }

    item_name = k_tuple_item_names[index];
    member->visibility = FENG_VISIBILITY_DEFAULT;
    member->is_static = false;
    member->as.field.mutability = FENG_MUTABILITY_LET;
    member->as.field.name.data = item_name;
    member->as.field.name.length = strlen(item_name);
    member->as.field.type = type_ref;
    member->as.field.initializer = NULL;
    return member;
}

static bool parse_tuple_type_declaration_tail(Parser *parser, FengDecl *decl) {
    size_t member_capacity = 0U;

    decl->as.type_decl.is_tuple = true;
    if (!parser_expect(parser, FENG_TOKEN_LPAREN, "SE0312", "expected '(' to start tuple type declaration")) {
        return false;
    }

    if (!parser_check(parser, FENG_TOKEN_RPAREN)) {
        do {
            FengToken element_token = parser_current_token(parser);
            FengTypeRef *element_type;
            FengTypeMember *member;

            if (decl->as.type_decl.member_count >= FENG_TUPLE_MAX_ITEMS) {
                return parser_error_current(parser,
                                            "SE0308", "tuple type declarations support at most 8 elements");
            }

            element_type = parse_type_ref(parser);
            if (element_type == NULL) {
                return false;
            }

            member = new_tuple_field_member(parser,
                                            element_token,
                                            decl->as.type_decl.member_count,
                                            element_type);
            if (member == NULL) {
                free_type_ref(element_type);
                return false;
            }
            if (!APPEND_VALUE(parser,
                              decl->as.type_decl.members,
                              decl->as.type_decl.member_count,
                              member_capacity,
                              member)) {
                free_type_member(member);
                return false;
            }
        } while (parser_match(parser, FENG_TOKEN_COMMA));
    }

    if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0312", "expected ')' to close tuple type declaration")) {
        return false;
    }
    if (decl->as.type_decl.member_count == 1U) {
        return parser_error_at(parser,
                               &decl->token,
                               "SE0308", "tuple type declarations require 0 or 2 to 8 elements");
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        if (!parse_spec_satisfaction_list(parser,
                                          &decl->as.type_decl.declared_specs,
                                          &decl->as.type_decl.declared_spec_count)) {
            return false;
        }
    }

    return parser_expect(parser,
                         FENG_TOKEN_SEMICOLON,
                         "SE0001", "tuple type declarations must end with ';'");
}

/* Parse one of the three concrete-type member-expansion directives. */
static bool parse_type_mixin_declaration(Parser *parser,
                                         size_t member_index,
                                         FengTypeMixinDecl *out_mixin) {
    FengTypeMixinDecl mixin;

    memset(&mixin, 0, sizeof(mixin));
    mixin.token = parser_current_token(parser);
    mixin.member_index = member_index;
    if (!parser_match(parser, FENG_TOKEN_ELLIPSIS)) {
        return parser_error_current(
            parser,
            "SE0313",
            "member mix declarations must use '...: Type;', '...: Type = Construction;', or '... = Construction;'");
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        mixin.source_type = parse_type_ref(parser);
        if (mixin.source_type == NULL) {
            return false;
        }
        if (parser_match(parser, FENG_TOKEN_ASSIGN)) {
            mixin.source_constructor = parse_expression(parser);
            if (mixin.source_constructor == NULL) {
                free_type_ref(mixin.source_type);
                return false;
            }
        }
    } else if (parser_match(parser, FENG_TOKEN_ASSIGN)) {
        mixin.infer_source_type = true;
        mixin.source_constructor = parse_expression(parser);
        if (mixin.source_constructor == NULL) {
            return false;
        }
    } else {
        return parser_error_current(
            parser,
            "SE0313",
            "member mix declarations must use '...: Type;', '...: Type = Construction;', or '... = Construction;'");
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_SEMICOLON,
                       "SE0001",
                       "member mix declarations must end with ';'")) {
        free_type_ref(mixin.source_type);
        free_expr(mixin.source_constructor);
        return false;
    }

    *out_mixin = mixin;
    return true;
}

static FengDecl *parse_type_declaration(Parser *parser,
                                        FengSlice doc_comment,
                                        FengVisibility visibility,
                                        bool is_extern,
                                        FengAnnotation *annotations,
                                        size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl = new_decl(parser, FENG_DECL_TYPE, name_token, doc_comment);
    FengSlice type_name;

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->visibility = visibility;
    decl->is_extern = is_extern;
    decl->annotations = annotations;
    decl->annotation_count = annotation_count;

    /* Detect @value annotation on type declarations (value semantics). */
    {
        size_t annot_index;
        for (annot_index = 0U; annot_index < annotation_count; ++annot_index) {
            if (annotations[annot_index].builtin_kind == FENG_ANNOTATION_VALUE) {
                decl->as.type_decl.is_value = true;
                break;
            }
        }
    }

    if (!parser_expect_identifier_like(parser, &type_name, false, "SE0002", "expected a type name")) {
        free_decl(decl);
        return NULL;
    }
    decl->as.type_decl.name = type_name;

    /* Optional generic type parameters: type Name<T: Bound, U> { ... } */
    if (parser_match(parser, FENG_TOKEN_LT)) {
        if (!parse_type_params(parser,
                               &decl->as.type_decl.type_params,
                               &decl->as.type_decl.type_param_count)) {
            free_decl(decl);
            return NULL;
        }
    }

    if (parser_check(parser, FENG_TOKEN_LPAREN)) {
        if (!parse_tuple_type_declaration_tail(parser, decl)) {
            free_decl(decl);
            return NULL;
        }
        return decl;
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        if (!parse_spec_satisfaction_list(parser,
                                &decl->as.type_decl.declared_specs,
                                &decl->as.type_decl.declared_spec_count)) {
            free_decl(decl);
            return NULL;
        }
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "SE0301", "type declarations require '{...}' after the optional spec list")) {
        free_decl(decl);
        return NULL;
    }

    {
        size_t member_capacity = 0U;
        size_t mixin_capacity = 0U;

    while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
        FengAnnotation *member_annotations;
        size_t member_annotation_count = 0U;
        FengVisibility member_visibility;
        bool is_static;
        FengSlice member_doc_comment = doc_comment_from_token(parser_current(parser));
        FengTypeMember *member = NULL;

        member_annotations = parse_annotations(parser, &member_annotation_count);
        if (parser->error.message != NULL) {
            free_decl(decl);
            return NULL;
        }

        member_visibility = parse_visibility(parser);
        is_static = parser_match(parser, FENG_TOKEN_KW_STATIC);

        if (member_annotation_count > 0U && parser_check(parser, FENG_TOKEN_SEMICOLON)) {
            free_annotations(member_annotations, member_annotation_count);
            (void)parser_error_current(
                parser,
                "SE0307", "type member annotations must be followed immediately by a field or method; remove the trailing ';'");
            free_decl(decl);
            return NULL;
        }

        if (parser_check(parser, FENG_TOKEN_ELLIPSIS)) {
            FengTypeMixinDecl mixin;

            if (member_annotation_count != 0U ||
                member_visibility != FENG_VISIBILITY_DEFAULT ||
                is_static) {
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_current(
                    parser,
                    "SE0313",
                    "member mix declarations cannot use annotations, visibility, or 'static'");
                free_decl(decl);
                return NULL;
            }
            free_annotations(member_annotations, member_annotation_count);
            if (!parse_type_mixin_declaration(parser,
                                              decl->as.type_decl.member_count,
                                              &mixin)) {
                free_decl(decl);
                return NULL;
            }
            if (!APPEND_VALUE(parser,
                              decl->as.type_decl.mixins,
                              decl->as.type_decl.mixin_count,
                              mixin_capacity,
                              mixin)) {
                free_type_ref(mixin.source_type);
                free_expr(mixin.source_constructor);
                free_decl(decl);
                return NULL;
            }
            continue;
        }

        if (parser_match(parser, FENG_TOKEN_KW_LET) || parser_match(parser, FENG_TOKEN_KW_VAR)) {
            FengMutability mutability = (parser_previous(parser)->kind == FENG_TOKEN_KW_LET)
                                            ? FENG_MUTABILITY_LET
                                            : FENG_MUTABILITY_VAR;
            FengBinding binding = parse_binding_core(parser, mutability, false, false);

            if (parser->error.message != NULL) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_SEMICOLON,
                               "SE0001", "type field declarations must end with ';'")) {
                free_type_ref(binding.type);
                free_expr(binding.initializer);
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            member = new_type_member(parser,
                                     FENG_TYPE_MEMBER_FIELD,
                                     binding.token,
                                     member_doc_comment);
            if (member == NULL) {
                free_type_ref(binding.type);
                free_expr(binding.initializer);
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            member->visibility = member_visibility;
            member->is_static = is_static;
            member->annotations = member_annotations;
            member->annotation_count = member_annotation_count;
            member->is_mixable = parsed_annotations_contain_kind(
                member_annotations,
                member_annotation_count,
                FENG_ANNOTATION_MIXABLE);
            member->as.field.mutability = binding.mutability;
            member->as.field.name = binding.name;
            member->as.field.type = binding.type;
            member->as.field.initializer = binding.initializer;
            member->as.field.declaration_bound = binding.initializer != NULL;
        } else if (parser_match(parser, FENG_TOKEN_KW_FUNC)) {
            FengCallableSignature callable;
            FengSlice name;
            FengToken member_name_token = parser_current_token(parser);
            FengTypeMemberKind member_kind = FENG_TYPE_MEMBER_METHOD;
            bool is_finalizer = false;

            if (is_extern) {
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_current(
                    parser,
                    "SE0306", "extern type object form only supports fields; methods require a non-extern type");
                free_decl(decl);
                return NULL;
            }

            if (parser_match(parser, FENG_TOKEN_TILDE)) {
                is_finalizer = true;
                member_name_token = parser_previous_token(parser);
            }

            if (!parser_expect_identifier_like(parser,
                                               &name,
                                               false,
                                               is_finalizer ? "SE0002" : "SE0508",
                                               is_finalizer
                                                   ? "expected the type name after 'func ~' to declare a finalizer"
                                                   : "expected a method or constructor name after 'func'")) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            if (is_finalizer && !slice_equals(name, type_name)) {
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_current(
                    parser,
                    "SE0508", "finalizer name must match the enclosing type name");
                free_decl(decl);
                return NULL;
            }

            callable = parse_callable_signature(
                parser,
                member_name_token,
                name,
                true,
                is_finalizer ? "SE0518" : "SE0506",
                is_finalizer
                    ? "type finalizers must provide a body '{...}'"
                    : "type methods and constructors must provide a body '{...}'");
            if (parser->error.message != NULL) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            if (is_static && is_finalizer) {
                free_parameters(callable.params, callable.param_count);
                free_type_ref(callable.return_type);
                free_block(callable.body);
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_at(parser,
                                      &callable.token,
                                      "SE0511", "finalizers cannot be declared 'static'");
                free_decl(decl);
                return NULL;
            }

            if (is_static && slice_equals(name, type_name)) {
                free_parameters(callable.params, callable.param_count);
                free_type_ref(callable.return_type);
                free_block(callable.body);
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_at(parser,
                                      &callable.token,
                                      "SE0507", "constructors cannot be declared 'static'");
                free_decl(decl);
                return NULL;
            }

            if (is_finalizer) {
                if (callable.param_count != 0U) {
                    free_parameters(callable.params, callable.param_count);
                    free_type_ref(callable.return_type);
                    free_block(callable.body);
                    free_annotations(member_annotations, member_annotation_count);
                    (void)parser_error_at(parser,
                                          &callable.token,
                                          "SE0509", "finalizer must not declare any parameters");
                    free_decl(decl);
                    return NULL;
                }
                if (callable.return_type != NULL && !type_ref_is_void_named(callable.return_type)) {
                    free_parameters(callable.params, callable.param_count);
                    free_type_ref(callable.return_type);
                    free_block(callable.body);
                    free_annotations(member_annotations, member_annotation_count);
                    (void)parser_error_at(parser,
                                          &callable.token,
                                          "SE0510", "finalizer return type must be omitted or ': void'");
                    free_decl(decl);
                    return NULL;
                }
                member_kind = FENG_TYPE_MEMBER_FINALIZER;
            } else if (slice_equals(name, type_name)) {
                if (callable.return_type == NULL || type_ref_is_void_named(callable.return_type)) {
                    member_kind = FENG_TYPE_MEMBER_CONSTRUCTOR;
                } else {
                    free_parameters(callable.params, callable.param_count);
                    free_type_ref(callable.return_type);
                    free_block(callable.body);
                    free_annotations(member_annotations, member_annotation_count);
                    (void)parser_error_at(
                        parser,
                        &callable.token,
                        "SE0505", "constructor must not declare a non-void return type");
                    free_decl(decl);
                    return NULL;
                }
            }

            member = new_type_member(parser,
                                     member_kind,
                                     callable.token,
                                     member_doc_comment);
            if (member == NULL) {
                free_parameters(callable.params, callable.param_count);
                free_type_ref(callable.return_type);
                free_block(callable.body);
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            member->visibility = member_visibility;
            member->is_static = is_static;
            member->annotations = member_annotations;
            member->annotation_count = member_annotation_count;
            member->is_mixable = parsed_annotations_contain_kind(
                member_annotations,
                member_annotation_count,
                FENG_ANNOTATION_MIXABLE);
            member->as.callable = callable;
        } else {
            free_annotations(member_annotations, member_annotation_count);
            if (parser_starts_callable_signature(parser)) {
                (void)parser_error_current(parser,
                                           "SE0304", "type methods and constructors must start with 'func'");
            } else if (parser_starts_binding_without_keyword(parser)) {
                (void)parser_error_current(parser,
                                           "SE0303", "type fields must start with 'let' or 'var'");
            } else if (parser_check(parser, FENG_TOKEN_KW_EXTERN)) {
                (void)parser_error_current(
                    parser,
                    "SE0305", "type members cannot use 'extern func'; use 'func' for methods or 'let'/'var' for fields");
            } else {
                (void)parser_error_current(parser,
                                           "SE0003", "expected type member declaration: 'let', 'var', 'func', or 'static'");
            }
            free_decl(decl);
            return NULL;
        }

        if (!APPEND_VALUE(parser,
                          decl->as.type_decl.members,
                          decl->as.type_decl.member_count,
                          member_capacity,
                          member)) {
            free_type_member(member);
            free_decl(decl);
            return NULL;
        }
    }
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE0312", "expected '}' to close type body")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengTypeMember *parse_spec_member(Parser *parser) {
    FengToken member_start = parser_current_token(parser);
    FengSlice doc_comment = doc_comment_from_token(&member_start);
    FengTypeMember *member = NULL;
    FengVisibility member_visibility = parse_visibility(parser);
    bool is_static = false;

    is_static = parser_match(parser, FENG_TOKEN_KW_STATIC);

    if (parser_match(parser, FENG_TOKEN_KW_LET) || parser_match(parser, FENG_TOKEN_KW_VAR)) {
        FengMutability mutability = (parser_previous(parser)->kind == FENG_TOKEN_KW_LET)
                                        ? FENG_MUTABILITY_LET
                                        : FENG_MUTABILITY_VAR;
        FengBinding binding = parse_binding_core(parser, mutability, true, false);

        if (parser->error.message != NULL) {
            return NULL;
        }
        if (binding.initializer != NULL) {
            (void)parser_error_current(parser, "SE0603", "spec field declarations cannot have an initializer");
            free_type_ref(binding.type);
            free_expr(binding.initializer);
            return NULL;
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           "SE0001", "spec field declarations must end with ';'")) {
            free_type_ref(binding.type);
            return NULL;
        }
        member = new_type_member(parser,
                     FENG_TYPE_MEMBER_FIELD,
                     binding.token,
                     doc_comment);
        if (member == NULL) {
            free_type_ref(binding.type);
            return NULL;
        }
        member->visibility = member_visibility;
        member->is_static = is_static;
        member->as.field.mutability = binding.mutability;
        member->as.field.name = binding.name;
        member->as.field.type = binding.type;
        member->as.field.initializer = NULL;
        return member;
    }

    if (parser_match(parser, FENG_TOKEN_KW_FUNC)) {
        FengCallableSignature callable;
        FengSlice name;
        FengToken member_name_token = parser_current_token(parser);
        FengTypeMemberKind member_kind = FENG_TYPE_MEMBER_METHOD;
        bool is_finalizer = false;
        size_t param_index;

        if (parser_match(parser, FENG_TOKEN_TILDE)) {
            is_finalizer = true;
            member_name_token = parser_previous_token(parser);
        }

        if (!parser_expect_identifier_like(parser,
                                           &name,
                                           false,
                                           is_finalizer ? "SE0002" : "SE0002",
                                           is_finalizer
                                               ? "expected a finalizer name after 'func ~'"
                                               : "expected a method name after 'func'")) {
            return NULL;
        }
        callable = parse_callable_signature(
            parser,
            member_name_token,
            name,
            false,
            "SE0605", "spec method signatures must end with ';' and cannot have a body '{...}'");
        if (parser->error.message != NULL) {
            return NULL;
        }
        for (param_index = 0U; param_index < callable.param_count; ++param_index) {
            if (callable.params[param_index].mutability != FENG_MUTABILITY_DEFAULT) {
                (void)parser_error_current(
                    parser,
                    "SE0606", "spec method parameters cannot use 'let' or 'var' modifiers");
                free_parameters(callable.params, callable.param_count);
                free_type_ref(callable.return_type);
                free_block(callable.body);
                return NULL;
            }
        }
        if (is_finalizer) {
            member_kind = FENG_TYPE_MEMBER_FINALIZER;
        }
        if (member_kind == FENG_TYPE_MEMBER_METHOD && callable.return_type == NULL) {
            (void)parser_error_current(parser, "SE0604", "spec method signatures must declare a return type");
            free_parameters(callable.params, callable.param_count);
            free_block(callable.body);
            return NULL;
        }
        member = new_type_member(parser,
                     member_kind,
                     callable.token,
                     doc_comment);
        if (member == NULL) {
            free_parameters(callable.params, callable.param_count);
            free_type_ref(callable.return_type);
            free_block(callable.body);
            return NULL;
        }
        member->visibility = member_visibility;
        member->is_static = is_static;
        member->as.callable = callable;
        return member;
    }

    (void)member_start;
    (void)parser_error_current(parser, "SE0003", "expected spec member declaration: 'let', 'var', or 'func'");
    return NULL;
}

static FengDecl *parse_spec_declaration(Parser *parser,
                                        FengSlice doc_comment,
                                        FengVisibility visibility,
                                        bool is_extern,
                                        FengAnnotation *annotations,
                                        size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl;
    FengSlice spec_name;

    if (is_extern) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "SE0003", "'extern' cannot be applied to a 'spec' declaration");
        return NULL;
    }

    decl = new_decl(parser, FENG_DECL_SPEC, name_token, doc_comment);
    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }
    decl->visibility = visibility;
    decl->is_extern = false;
    decl->annotations = annotations;
    decl->annotation_count = annotation_count;

    if (!parser_expect_identifier_like(parser, &spec_name, false, "SE0002", "expected a spec name after 'spec'")) {
        free_decl(decl);
        return NULL;
    }
    decl->as.spec_decl.name = spec_name;

    /* Optional generic type parameters: spec Name<T: Bound, U> { ... } */
    if (parser_match(parser, FENG_TOKEN_LT)) {
        if (!parse_type_params(parser,
                               &decl->as.spec_decl.type_params,
                               &decl->as.spec_decl.type_param_count)) {
            free_decl(decl);
            return NULL;
        }
    }

    if (parser_check(parser, FENG_TOKEN_LPAREN)) {
        size_t param_index;

        decl->as.spec_decl.form = FENG_SPEC_FORM_CALLABLE;
        if (!parse_parameters(parser,
                              &decl->as.spec_decl.as.callable.params,
                              &decl->as.spec_decl.as.callable.param_count)) {
            free_decl(decl);
            return NULL;
        }
        for (param_index = 0U; param_index < decl->as.spec_decl.as.callable.param_count; ++param_index) {
            if (decl->as.spec_decl.as.callable.params[param_index].mutability != FENG_MUTABILITY_DEFAULT) {
                (void)parser_error_current(
                    parser,
                    "SE0606", "spec callable parameters cannot use 'let' or 'var' modifiers");
                free_decl(decl);
                return NULL;
            }
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_COLON,
                           "SE0607", "spec callable declarations require ':' before the return type")) {
            free_decl(decl);
            return NULL;
        }
        decl->as.spec_decl.as.callable.return_type = parse_type_ref(parser);
        if (decl->as.spec_decl.as.callable.return_type == NULL) {
            free_decl(decl);
            return NULL;
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           "SE0001", "spec callable declarations must end with ';'")) {
            free_decl(decl);
            return NULL;
        }
        return decl;
    }

    decl->as.spec_decl.form = FENG_SPEC_FORM_OBJECT;

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        FengTypeRef *first_type = parse_type_ref(parser);
        size_t list_capacity = 0U;

        if (first_type == NULL) {
            free_decl(decl);
            return NULL;
        }
        if (parser_match(parser, FENG_TOKEN_PIPE)) {
            decl->as.spec_decl.form = FENG_SPEC_FORM_UNION;
            if (type_ref_is_void_named(first_type)) {
                (void)parser_error_at(parser,
                                      &first_type->token,
                                      "SE0609", "union-form spec members cannot be 'void'");
                free_type_ref(first_type);
                free_decl(decl);
                return NULL;
            }
            if (!append_type_ref(parser,
                                 &decl->as.spec_decl.as.union_form.members,
                                 &decl->as.spec_decl.as.union_form.member_count,
                                 &list_capacity,
                                 first_type)) {
                free_type_ref(first_type);
                free_decl(decl);
                return NULL;
            }
            do {
                FengTypeRef *member_type = parse_type_ref(parser);

                if (member_type == NULL) {
                    free_decl(decl);
                    return NULL;
                }
                if (type_ref_is_void_named(member_type)) {
                    (void)parser_error_at(parser,
                                          &member_type->token,
                                          "SE0609", "union-form spec members cannot be 'void'");
                    free_type_ref(member_type);
                    free_decl(decl);
                    return NULL;
                }
                if (!append_type_ref(parser,
                                     &decl->as.spec_decl.as.union_form.members,
                                     &decl->as.spec_decl.as.union_form.member_count,
                                     &list_capacity,
                                     member_type)) {
                    free_type_ref(member_type);
                    free_decl(decl);
                    return NULL;
                }
            } while (parser_match(parser, FENG_TOKEN_PIPE));

            if (!parser_expect(parser,
                               FENG_TOKEN_SEMICOLON,
                               "SE0001", "union-form spec declarations must end with ';'")) {
                free_decl(decl);
                return NULL;
            }
            return decl;
        }

        if (parser_match(parser, FENG_TOKEN_AMP)) {
            decl->as.spec_decl.form = FENG_SPEC_FORM_INTERSECTION;
            if (!append_type_ref(parser,
                                 &decl->as.spec_decl.as.intersection_form.members,
                                 &decl->as.spec_decl.as.intersection_form.member_count,
                                 &list_capacity,
                                 first_type)) {
                free_type_ref(first_type);
                free_decl(decl);
                return NULL;
            }
            do {
                FengTypeRef *member_type = parse_type_ref(parser);

                if (member_type == NULL) {
                    free_decl(decl);
                    return NULL;
                }
                if (!append_type_ref(parser,
                                     &decl->as.spec_decl.as.intersection_form.members,
                                     &decl->as.spec_decl.as.intersection_form.member_count,
                                     &list_capacity,
                                     member_type)) {
                    free_type_ref(member_type);
                    free_decl(decl);
                    return NULL;
                }
            } while (parser_match(parser, FENG_TOKEN_AMP));

            if (!parser_expect(parser,
                               FENG_TOKEN_SEMICOLON,
                               "SE0001", "intersection-form spec declarations must end with ';'")) {
                free_decl(decl);
                return NULL;
            }
            return decl;
        }

        if (!append_type_ref(parser,
                             &decl->as.spec_decl.parent_specs,
                             &decl->as.spec_decl.parent_spec_count,
                             &list_capacity,
                             first_type)) {
            free_type_ref(first_type);
            free_decl(decl);
            return NULL;
        }
        while (parser_match(parser, FENG_TOKEN_COMMA)) {
            FengTypeRef *parent_type = parse_type_ref(parser);

            if (parent_type == NULL) {
                free_decl(decl);
                return NULL;
            }
            if (!append_type_ref(parser,
                                 &decl->as.spec_decl.parent_specs,
                                 &decl->as.spec_decl.parent_spec_count,
                                 &list_capacity,
                                 parent_type)) {
                free_type_ref(parent_type);
                free_decl(decl);
                return NULL;
            }
        }
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "SE0608", "spec object declarations require '{...}' after the optional spec list")) {
        free_decl(decl);
        return NULL;
    }

    {
        size_t member_capacity = 0U;

        while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
            FengAnnotation *member_annotations;
            size_t member_annotation_count = 0U;
            FengTypeMember *member;

            member_annotations = parse_annotations(parser, &member_annotation_count);
            if (parser->error.message != NULL) {
                free_decl(decl);
                return NULL;
            }
            member = parse_spec_member(parser);

            if (member == NULL) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            member->annotations = member_annotations;
            member->annotation_count = member_annotation_count;
            member->is_mixable = parsed_annotations_contain_kind(
                member_annotations,
                member_annotation_count,
                FENG_ANNOTATION_MIXABLE);
            if (!APPEND_VALUE(parser,
                              decl->as.spec_decl.as.object.members,
                              decl->as.spec_decl.as.object.member_count,
                              member_capacity,
                              member)) {
                free_type_member(member);
                free_decl(decl);
                return NULL;
            }
        }
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE0610", "expected '}' to close spec body")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengTypeMember *parse_fit_method_member(Parser *parser) {
    FengToken doc_token = parser_current_token(parser);
    FengSlice doc_comment = doc_comment_from_token(&doc_token);
    FengAnnotation *member_annotations = NULL;
    size_t member_annotation_count = 0U;
    FengVisibility visibility;
    bool is_static;
    FengToken member_start;
    FengCallableSignature callable;
    FengSlice name;
    FengTypeMember *member;

    member_annotations = parse_annotations(parser, &member_annotation_count);
    if (parser->error.message != NULL) {
        return NULL;
    }

    visibility = parse_visibility(parser);
    is_static = parser_match(parser, FENG_TOKEN_KW_STATIC);
    member_start = parser_current_token(parser);

    if (parser_check(parser, FENG_TOKEN_KW_LET) || parser_check(parser, FENG_TOKEN_KW_VAR)) {
        free_annotations(member_annotations, member_annotation_count);
        (void)parser_error_current(
            parser,
            is_static ? "SE0807" : "SE0807",
            is_static
                ? "fit blocks cannot declare 'static let' or 'static var'; fit only supports methods"
                : "fit blocks cannot declare 'let' or 'var' fields; declare them on the original type");
        return NULL;
    }

    if (!parser_match(parser, FENG_TOKEN_KW_FUNC)) {
        free_annotations(member_annotations, member_annotation_count);
        (void)parser_error_current(
            parser,
            is_static ? "SE0808" : "SE0808",
            is_static
                ? "fit static members must be declared with 'func'"
                : "fit block members must start with 'func'");
        return NULL;
    }

    if (!parser_expect_identifier_like(parser, &name, false, "SE0002", "expected a method name after 'func'")) {
        free_annotations(member_annotations, member_annotation_count);
        return NULL;
    }
    callable = parse_callable_signature(
        parser,
        member_start,
        name,
        true,
        "SE0805", "fit block methods must provide a body '{...}'");
    if (parser->error.message != NULL) {
        free_annotations(member_annotations, member_annotation_count);
        return NULL;
    }
    member = new_type_member(parser,
                             FENG_TYPE_MEMBER_METHOD,
                             callable.token,
                             doc_comment);
    if (member == NULL) {
        free_annotations(member_annotations, member_annotation_count);
        free_parameters(callable.params, callable.param_count);
        free_type_ref(callable.return_type);
        free_block(callable.body);
        return NULL;
    }
    member->visibility = visibility;
    member->is_static = is_static;
    member->as.callable = callable;
    member->annotations = member_annotations;
    member->annotation_count = member_annotation_count;
    member->is_mixable = parsed_annotations_contain_kind(
        member_annotations,
        member_annotation_count,
        FENG_ANNOTATION_MIXABLE);
    return member;
}

static FengDecl *parse_fit_declaration(Parser *parser,
                                       FengSlice doc_comment,
                                       FengVisibility visibility,
                                       bool is_extern,
                                       FengAnnotation *annotations,
                                       size_t annotation_count) {
    FengToken start = parser_current_token(parser);
    FengDecl *decl;

    if (is_extern) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "SE0801", "'extern' cannot be applied to a 'fit' declaration");
        return NULL;
    }
    if (annotation_count > 0U) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "SE0802", "annotations cannot be applied to 'fit' declarations");
        return NULL;
    }
    if (visibility == FENG_VISIBILITY_PRIVATE) {
        (void)parser_error_current(parser, "SE0803", "fit declarations cannot use 'seal'");
        return NULL;
    }

    decl = new_decl(parser, FENG_DECL_FIT, start, doc_comment);
    if (decl == NULL) {
        return NULL;
    }
    decl->visibility = visibility;
    decl->is_extern = false;

    decl->as.fit_decl.target = parse_type_ref(parser);
    if (decl->as.fit_decl.target == NULL) {
        free_decl(decl);
        return NULL;
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        if (!parse_spec_satisfaction_list(parser,
                                &decl->as.fit_decl.specs,
                                &decl->as.fit_decl.spec_count)) {
            free_decl(decl);
            return NULL;
        }
    }

    if (parser_match(parser, FENG_TOKEN_LBRACE)) {
        size_t member_capacity = 0U;

        decl->as.fit_decl.has_body = true;
        while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
            FengTypeMember *member = parse_fit_method_member(parser);

            if (member == NULL) {
                free_decl(decl);
                return NULL;
            }
            if (!APPEND_VALUE(parser,
                              decl->as.fit_decl.members,
                              decl->as.fit_decl.member_count,
                              member_capacity,
                              member)) {
                free_type_member(member);
                free_decl(decl);
                return NULL;
            }
        }
        if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE0806", "expected '}' to close fit body")) {
            free_decl(decl);
            return NULL;
        }
    } else {
        decl->as.fit_decl.has_body = false;
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           "SE0001", "fit declarations without a body must end with ';'")) {
            free_decl(decl);
            return NULL;
        }
    }

    if (decl->as.fit_decl.spec_count == 0U && !decl->as.fit_decl.has_body) {
        (void)parser_error_current(parser, "SE0804", "fit declarations must include a spec list, a body block, or both");
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_function_declaration(Parser *parser,
                                            FengSlice doc_comment,
                                            FengVisibility visibility,
                                            bool is_extern,
                                            FengAnnotation *annotations,
                                            size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl = new_decl(parser, FENG_DECL_FUNCTION, name_token, doc_comment);
    FengSlice name;

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->visibility = visibility;
    decl->is_extern = is_extern;
    decl->annotations = annotations;
    decl->annotation_count = annotation_count;

    if (!parser_expect_identifier_like(parser, &name, false, "SE0002", "expected a function name after 'func'")) {
        free_decl(decl);
        return NULL;
    }

    decl->as.function_decl = parse_callable_signature(
        parser,
        name_token,
        name,
        !is_extern,
        is_extern ? "SE0517" : "SE0518",
        is_extern ? "extern function declarations must end with ';' and cannot have a body '{...}'"
                  : "function declarations must provide a body '{...}'");
    if (parser->error.message != NULL) {
        free_decl(decl);
        return NULL;
    }

    /* Extern func cannot have variadic parameters. */
    if (is_extern &&
        decl->as.function_decl.param_count > 0U &&
        decl->as.function_decl.params[decl->as.function_decl.param_count - 1U].is_variadic) {
        size_t last = decl->as.function_decl.param_count - 1U;

        (void)parser_error_at(parser,
            &decl->as.function_decl.params[last].token,
            "SE0503", "extern function declarations cannot use variadic parameters");
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_global_binding(Parser *parser,
                      FengSlice doc_comment,
                      FengVisibility visibility,
                      FengMutability mutability,
                      FengAnnotation *annotations,
                      size_t annotation_count) {
    FengDecl *decl = new_decl(parser,
                              FENG_DECL_GLOBAL_BINDING,
                  parser_current_token(parser),
                  doc_comment);

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->annotations = annotations;
    decl->annotation_count = annotation_count;
    decl->visibility = visibility;
    decl->as.binding = parse_binding_core(parser, mutability, false, false);
    if (parser->error.message != NULL) {
        free_decl(decl);
        return NULL;
    }

    if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "top-level bindings must end with ';'")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_declaration(Parser *parser) {
    FengAnnotation *annotations;
    size_t annotation_count = 0U;
    FengSlice doc_comment = doc_comment_from_token(parser_current(parser));
    FengVisibility visibility;
    bool is_extern = false;

    annotations = parse_annotations(parser, &annotation_count);
    if (parser->error.message != NULL) {
        return NULL;
    }

    visibility = parse_visibility(parser);

    if (parser_match(parser, FENG_TOKEN_KW_LET)) {
        return parse_global_binding(
            parser, doc_comment, visibility, FENG_MUTABILITY_LET, annotations, annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_VAR)) {
        return parse_global_binding(
            parser, doc_comment, visibility, FENG_MUTABILITY_VAR, annotations, annotation_count);
    }

    if (parser_match(parser, FENG_TOKEN_KW_EXTERN)) {
        is_extern = true;
    }

    if (annotation_count > 0U && parser_check(parser, FENG_TOKEN_SEMICOLON)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(
            parser,
            "SE1301", "annotation must be followed immediately by a declaration; remove the trailing ';'");
        return NULL;
    }

    if (is_extern) {
        if (parser_match(parser, FENG_TOKEN_KW_FUNC)) {
            return parse_function_declaration(parser,
                                              doc_comment,
                                              visibility,
                                              true,
                                              annotations,
                                              annotation_count);
        }
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser,
                                   "SE0003", "'extern' can only be applied to top-level 'func' declarations");
        return NULL;
    }

    if (parser_starts_callable_signature(parser)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "SE0501", "top-level function declarations must start with 'func'");
        return NULL;
    }

    if (parser_starts_binding_without_keyword(parser)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "SE0102", "top-level bindings must start with 'let' or 'var'");
        return NULL;
    }

    if (parser_match(parser, FENG_TOKEN_KW_TYPE)) {
        return parse_type_declaration(parser,
                                      doc_comment,
                                      visibility,
                                      is_extern,
                                      annotations,
                                      annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_ENUM)) {
        return parse_enum_declaration(parser,
                                      doc_comment,
                                      visibility,
                                      is_extern,
                                      annotations,
                                      annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_SPEC)) {
        return parse_spec_declaration(parser,
                                      doc_comment,
                                      visibility,
                                      is_extern,
                                      annotations,
                                      annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_FIT)) {
        return parse_fit_declaration(parser,
                                     doc_comment,
                                     visibility,
                                     is_extern,
                                     annotations,
                                     annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_FUNC)) {
        return parse_function_declaration(parser,
                                          doc_comment,
                                          visibility,
                                          false,
                                          annotations,
                                          annotation_count);
    }

    free_annotations(annotations, annotation_count);
    (void)parser_error_current(parser,
                               "SE0003", "expected top-level declaration: 'let', 'var', 'extern func', 'type', 'spec', 'fit', or 'func'");
    return NULL;
}

static FengExpr *new_expr(Parser *parser, FengExprKind kind, FengToken token) {
    FengExpr *expr = (FengExpr *)calloc(1U, sizeof(*expr));

    if (expr == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
        return NULL;
    }
    expr->token = token;
    expr->kind = kind;
    return expr;
}

static FengStmt *new_stmt(Parser *parser, FengStmtKind kind, FengToken token) {
    FengStmt *stmt = (FengStmt *)calloc(1U, sizeof(*stmt));

    if (stmt == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
        return NULL;
    }
    stmt->token = token;
    stmt->kind = kind;
    return stmt;
}

static FengBlock *new_block(Parser *parser, FengToken token) {
    FengBlock *block = (FengBlock *)calloc(1U, sizeof(*block));

    if (block == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
    } else {
        block->token = token;
    }
    return block;
}

static bool token_starts_expression(FengTokenKind kind) {
    switch (kind) {
        case FENG_TOKEN_IDENTIFIER:
        case FENG_TOKEN_BOOL:
        case FENG_TOKEN_INTEGER:
        case FENG_TOKEN_FLOAT:
        case FENG_TOKEN_STRING:
        case FENG_TOKEN_LPAREN:
        case FENG_TOKEN_DOT:
        case FENG_TOKEN_LBRACKET:
        case FENG_TOKEN_MINUS:
        case FENG_TOKEN_NOT:
        case FENG_TOKEN_KW_IF:
        case FENG_TOKEN_KW_MATCH:
        case FENG_TOKEN_KW_SELF:
            return true;
        default:
            return false;
    }
}

static bool looks_like_lambda(const Parser *parser) {
    size_t index;
    size_t depth = 0U;
    bool saw_colon = false;

    if (!parser_check(parser, FENG_TOKEN_LPAREN)) {
        return false;
    }

    for (index = parser->current + 1U; index < parser->token_count; ++index) {
        FengTokenKind kind = parser->tokens[index].kind;

        if (kind == FENG_TOKEN_LPAREN) {
            ++depth;
            continue;
        }
        if (kind == FENG_TOKEN_RPAREN) {
            if (depth == 0U) {
                bool is_empty = (index == parser->current + 1U);
                FengTokenKind after = parser->tokens[index + 1U].kind;

                if (after == FENG_TOKEN_ARROW && (saw_colon || is_empty)) {
                    return true;
                }
                if (after == FENG_TOKEN_LBRACE && (saw_colon || is_empty)) {
                    return true;
                }
                return false;
            }
            --depth;
            continue;
        }
        if (depth == 0U && kind == FENG_TOKEN_COLON) {
            saw_colon = true;
        }
    }

    return false;
}

static bool looks_like_cast(const Parser *parser) {
    Parser probe = *parser;
    FengTypeRef *type_ref = NULL;

    if (!parser_check(parser, FENG_TOKEN_LPAREN)) {
        return false;
    }

    (void)parser_advance(&probe);
    type_ref = parse_type_ref(&probe);
    if (type_ref == NULL) {
        return false;
    }
    free_type_ref(type_ref);

    if (!parser_match(&probe, FENG_TOKEN_RPAREN)) {
        return false;
    }
    return token_starts_expression(parser_current(&probe)->kind);
}

static bool looks_like_object_literal(const Parser *parser) {
    if (!parser_check(parser, FENG_TOKEN_LBRACE)) {
        return false;
    }
    if (parser->tokens[parser->current + 1U].kind == FENG_TOKEN_RBRACE) {
        return true;
    }
    return parser->tokens[parser->current + 1U].kind == FENG_TOKEN_IDENTIFIER &&
           parser->tokens[parser->current + 2U].kind == FENG_TOKEN_COLON;
}

static bool expr_can_take_explicit_type_args(const FengExpr *expr) {
    return expr != NULL &&
           (expr->kind == FENG_EXPR_IDENTIFIER || expr->kind == FENG_EXPR_MEMBER);
}

static bool token_can_follow_explicit_generic_target(FengTokenKind kind) {
    switch (kind) {
        case FENG_TOKEN_LPAREN:
        case FENG_TOKEN_DOT:
        case FENG_TOKEN_LBRACKET:
        case FENG_TOKEN_LBRACE:
        case FENG_TOKEN_SEMICOLON:
        case FENG_TOKEN_COMMA:
        case FENG_TOKEN_RPAREN:
        case FENG_TOKEN_RBRACKET:
        case FENG_TOKEN_RBRACE:
            return true;
        default:
            return false;
    }
}

static bool looks_like_explicit_generic_target_suffix(const Parser *parser) {
    Parser probe = *parser;
    FengTypeRef **type_args = NULL;
    size_t type_arg_count = 0U;
    bool matched;

    if (!parser_check(parser, FENG_TOKEN_LT) || parser_peek(parser, 1U)->kind == FENG_TOKEN_GT) {
        return false;
    }

    (void)parser_advance(&probe);
    if (!parse_type_args(&probe, &type_args, &type_arg_count)) {
        free_type_arg_refs(type_args, type_arg_count);
        return false;
    }

    matched = token_can_follow_explicit_generic_target(parser_current(&probe)->kind);
    free_type_arg_refs(type_args, type_arg_count);
    return matched;
}

static FengExpr *new_call_from_explicit_generic_target(Parser *parser, FengExpr *expr) {
    FengExpr *call;

    if (expr == NULL || expr->kind != FENG_EXPR_GENERIC_TARGET) {
        return expr;
    }

    call = new_expr(parser, FENG_EXPR_CALL, expr->token);
    if (call == NULL) {
        free_expr(expr);
        return NULL;
    }

    call->as.call.callee = expr->as.generic_target.target;
    call->as.call.has_explicit_type_args = true;
    call->as.call.explicit_type_args = expr->as.generic_target.type_args;
    call->as.call.explicit_type_arg_count = expr->as.generic_target.type_arg_count;

    expr->as.generic_target.target = NULL;
    expr->as.generic_target.type_args = NULL;
    expr->as.generic_target.type_arg_count = 0U;
    free(expr);
    return call;
}

static FengExpr *parse_primary(Parser *parser);

static FengExpr *parse_object_literal_suffix(Parser *parser, FengExpr *target) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_OBJECT_LITERAL, parser_current_token(parser));
    size_t field_capacity = 0U;

    if (expr == NULL) {
        free_expr(target);
        return NULL;
    }

    expr->as.object_literal.target = target;

    if (!parser_expect(parser, FENG_TOKEN_LBRACE, "SE1002", "expected '{' to start object literal")) {
        free_expr(expr);
        return NULL;
    }

    if (!parser_check(parser, FENG_TOKEN_RBRACE)) {
        do {
            FengObjectFieldInit field;

            field.token = parser_current_token(parser);
            if (!parser_expect_identifier_like(parser,
                                               &field.name,
                                               false,
                                               "SE0002", "expected an object literal field name")) {
                free_expr(expr);
                return NULL;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "SE1001", "expected ':' after object literal field name")) {
                free_expr(expr);
                return NULL;
            }
            field.value = parse_expression(parser);
            if (field.value == NULL) {
                free_expr(expr);
                return NULL;
            }
            if (!APPEND_VALUE(parser,
                              expr->as.object_literal.fields,
                              expr->as.object_literal.field_count,
                              field_capacity,
                              field)) {
                free_expr(field.value);
                free_expr(expr);
                return NULL;
            }
        } while (parser_match(parser, FENG_TOKEN_COMMA));
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE1002", "expected '}' to close object literal")) {
        free_expr(expr);
        return NULL;
    }

    return expr;
}

static FengExpr *parse_array_literal(Parser *parser) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_ARRAY_LITERAL, parser_current_token(parser));
    size_t capacity = 0U;

    if (expr == NULL) {
        return NULL;
    }

    if (!parser_expect(parser, FENG_TOKEN_LBRACKET, "SE0202", "expected '[' to start array literal")) {
        free_expr(expr);
        return NULL;
    }

    if (!parser_check(parser, FENG_TOKEN_RBRACKET)) {
        do {
            FengExpr *item = parse_expression(parser);

            if (item == NULL) {
                free_expr(expr);
                return NULL;
            }
            if (!APPEND_VALUE(parser, expr->as.array_literal.items, expr->as.array_literal.count, capacity, item)) {
                free_expr(item);
                free_expr(expr);
                return NULL;
            }
        } while (parser_match(parser, FENG_TOKEN_COMMA));
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACKET, "SE0202", "expected ']' to close array literal")) {
        free_expr(expr);
        return NULL;
    }

    return expr;
}

static FengExpr *parse_lambda(Parser *parser) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_LAMBDA, parser_current_token(parser));

    if (expr == NULL) {
        return NULL;
    }

    if (!parse_parameters(parser, &expr->as.lambda.params, &expr->as.lambda.param_count)) {
        free_expr(expr);
        return NULL;
    }

    if (parser_check(parser, FENG_TOKEN_LBRACE)) {
        expr->as.lambda.is_block_body = true;
        expr->as.lambda.body_block = parse_block(parser);
        if (expr->as.lambda.body_block == NULL) {
            free_expr(expr);
            return NULL;
        }
        return expr;
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_ARROW,
                       "SE0513", "lambda expressions must use '->' before a single-expression body or '{' for a block body")) {
        free_expr(expr);
        return NULL;
    }

    if (parser_check(parser, FENG_TOKEN_LBRACE)) {
        (void)parser_error_current(
            parser,
            "SE0514", "multi-line lambda body must omit '->' and use the block form '(params) { ... }'");
        free_expr(expr);
        return NULL;
    }

    expr->as.lambda.is_block_body = false;
    expr->as.lambda.body = parse_expression(parser);
    if (expr->as.lambda.body == NULL) {
        free_expr(expr);
        return NULL;
    }

    return expr;
}

static FengExpr *parse_tuple_literal_tail(Parser *parser, FengToken token, FengExpr *first) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_TUPLE_LITERAL, token);
    size_t capacity = 0U;

    if (expr == NULL) {
        free_expr(first);
        return NULL;
    }
    if (!APPEND_VALUE(parser, expr->as.tuple_literal.items, expr->as.tuple_literal.count, capacity, first)) {
        free_expr(first);
        free_expr(expr);
        return NULL;
    }

    for (;;) {
        FengExpr *item;

        if (parser_check(parser, FENG_TOKEN_RPAREN)) {
            (void)parser_error_current(parser,
                                       "SE0310", "tuple literals require an expression after ','");
            free_expr(expr);
            return NULL;
        }
        if (expr->as.tuple_literal.count >= FENG_TUPLE_MAX_ITEMS) {
            (void)parser_error_current(parser, "SE0309", "tuple literals support at most 8 elements");
            free_expr(expr);
            return NULL;
        }

        item = parse_expression(parser);
        if (item == NULL) {
            free_expr(expr);
            return NULL;
        }
        if (!APPEND_VALUE(parser, expr->as.tuple_literal.items, expr->as.tuple_literal.count, capacity, item)) {
            free_expr(item);
            free_expr(expr);
            return NULL;
        }
        if (!parser_match(parser, FENG_TOKEN_COMMA)) {
            break;
        }
    }

    if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0312", "expected ')' to close tuple literal")) {
        free_expr(expr);
        return NULL;
    }
    return expr;
}

static FengExpr *parse_empty_tuple_literal(Parser *parser, FengToken token) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_TUPLE_LITERAL, token);

    if (expr == NULL) {
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0312", "expected ')' to close tuple literal")) {
        free_expr(expr);
        return NULL;
    }
    return expr;
}

/* ---------------- if / match shared helpers ---------------- */

static bool is_type_label_start_token(FengTokenKind kind) {
    return kind == FENG_TOKEN_IDENTIFIER || kind == FENG_TOKEN_KW_VOID ||
           kind == FENG_TOKEN_KW_UNKNOWN;
}

static FengExpr *parse_match_label_atom(Parser *parser) {
    FengToken token = *parser_current(parser);

    if (token.kind == FENG_TOKEN_MINUS) {
        FengExpr *expr = new_expr(parser, FENG_EXPR_UNARY, token);

        if (expr == NULL) {
            return NULL;
        }
        (void)parser_advance(parser);
        expr->as.unary.op = FENG_TOKEN_MINUS;
        expr->as.unary.operand = parse_match_label_atom(parser);
        if (expr->as.unary.operand == NULL) {
            free_expr(expr);
            return NULL;
        }
        return expr;
    }

    switch (token.kind) {
        case FENG_TOKEN_INTEGER: {
            FengExpr *expr = new_expr(parser, FENG_EXPR_INTEGER, token);
            if (expr != NULL) {
                expr->as.integer = token.value.integer;
                (void)parser_advance(parser);
            }
            return expr;
        }
        case FENG_TOKEN_STRING: {
            FengExpr *expr = new_expr(parser, FENG_EXPR_STRING, token);
            if (expr != NULL) {
                expr->as.string = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        }
        case FENG_TOKEN_BOOL: {
            FengExpr *expr = new_expr(parser, FENG_EXPR_BOOL, token);
            if (expr != NULL) {
                expr->as.boolean = token.value.boolean;
                (void)parser_advance(parser);
            }
            return expr;
        }
        case FENG_TOKEN_IDENTIFIER: {
            FengExpr *expr = new_expr(parser, FENG_EXPR_IDENTIFIER, token);
            if (expr != NULL) {
                expr->as.identifier = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        }
        default:
            (void)parser_error_current(parser,
                "SE1105", "match label must be an integer, string, bool literal or named constant");
            return NULL;
    }
}

/* Whether `kind` is a valid terminator for an infix match label (i.e. the
 * token that may legally follow `expr match pattern` once a single label has
 * been consumed). Includes label separators (`|`), statement/sequence
 * terminators (`;`, `,`, `)`, `]`, `}`), block openers (`{`), logical
 * operators (`&&`, `||`), comparison operators (`==`, `!=`, `<`, `<=`, `>`,
 * `>=`), assignment (`=`), and end-of-source. */
static bool infix_match_label_terminator(FengTokenKind kind) {
    switch (kind) {
        case FENG_TOKEN_PIPE:
        case FENG_TOKEN_SEMICOLON:
        case FENG_TOKEN_COMMA:
        case FENG_TOKEN_RPAREN:
        case FENG_TOKEN_RBRACKET:
        case FENG_TOKEN_RBRACE:
        case FENG_TOKEN_LBRACE:
        case FENG_TOKEN_ARROW:
        case FENG_TOKEN_AND_AND:
        case FENG_TOKEN_OR_OR:
        case FENG_TOKEN_EQ:
        case FENG_TOKEN_NE:
        case FENG_TOKEN_LT:
        case FENG_TOKEN_LE:
        case FENG_TOKEN_GT:
        case FENG_TOKEN_GE:
        case FENG_TOKEN_EOF:
            return true;
        default:
            return false;
    }
}

/* Parse an optional `->` chain after a type label. The first type is already
 * stored in out_label->type; each `-> TypeRef` appends to type_chain. */
static bool parse_match_label_chain(Parser *parser, FengMatchLabel *out_label) {
    FengTypeRef **chain = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    while (parser_check(parser, FENG_TOKEN_ARROW)) {
        FengTypeRef *next;

        (void)parser_advance(parser);
        next = parse_type_ref(parser);
        if (next == NULL) {
            for (size_t i = 0U; i < count; ++i) {
                free_type_ref(chain[i]);
            }
            free(chain);
            return false;
        }
        if (count == capacity) {
            size_t new_cap = capacity == 0U ? 4U : capacity * 2U;
            FengTypeRef **grown = (FengTypeRef **)realloc(chain, new_cap * sizeof(*grown));

            if (grown == NULL) {
                free_type_ref(next);
                for (size_t i = 0U; i < count; ++i) {
                    free_type_ref(chain[i]);
                }
                free(chain);
                return false;
            }
            chain = grown;
            capacity = new_cap;
        }
        chain[count++] = next;
    }

    out_label->type_chain = chain;
    out_label->type_chain_count = count;
    return true;
}

static bool parse_match_label(Parser *parser, FengMatchLabel *out_label, bool infix_mode) {
    FengToken token = *parser_current(parser);
    FengExpr *first;

    out_label->token = token;
    out_label->kind = FENG_MATCH_LABEL_VALUE;
    out_label->value = NULL;
    out_label->range_low = NULL;
    out_label->range_high = NULL;
    out_label->type = NULL;
    out_label->type_chain = NULL;
    out_label->type_chain_count = 0U;

    if (is_type_label_start_token(token.kind)) {
        size_t before = parser->current;
        FengTypeRef *type_ref = parse_type_ref(parser);

        if (type_ref == NULL) {
            /* In infix_mode, parse_type_ref may fail when a single-segment
             * type name is followed by `<` (e.g. `x match T < y`), because
             * `<` is greedily consumed as a generic type-arg list. Roll back
             * to the saved position and clear any error so we can fall through
             * to parse_match_label_atom (which will yield a value label).
             * Block-form mode (infix_mode == false) preserves the original
             * behavior of returning false on type_ref failure. */
            if (infix_mode) {
                parser->current = before;
                if (parser->error.message != NULL) {
                    parser->error.message = NULL;
                    parser->error.code = NULL;
                }
                /* fall through to parse_match_label_atom below */
            } else {
                return false;
            }
        } else {
            if (!parser_check(parser, FENG_TOKEN_ELLIPSIS) &&
                (parser_check(parser, FENG_TOKEN_COMMA) ||
                 parser_check(parser, FENG_TOKEN_LBRACE) ||
                 parser_check(parser, FENG_TOKEN_ARROW))) {
                out_label->kind = FENG_MATCH_LABEL_TYPE;
                out_label->type = type_ref;
                if (type_ref->kind == FENG_TYPE_REF_NAMED &&
                    type_ref->as.named.segment_count == 1U &&
                    type_ref->as.named.type_arg_count == 0U) {
                    FengExpr *fallback = new_expr(parser, FENG_EXPR_IDENTIFIER, type_ref->token);

                    if (fallback == NULL) {
                        free_type_ref(type_ref);
                        return false;
                    }
                    fallback->as.identifier = type_ref->as.named.segments[0];
                    out_label->value = fallback;
                }
                return parse_match_label_chain(parser, out_label);
            }
            /* infix_mode: also accept the wider set of infix label terminators
             * (|, ;, ), ], }, &&, ||, ==, !=, <, <=, >, >=, EOF). The range
             * marker `...` is already excluded above, so any type label followed
             * by these terminators is recognized as a union member type label;
             * semantic analysis later validates that the identifier resolves to a
             * type (or named constant) compatible with the target type. */
            if (infix_mode &&
                infix_match_label_terminator(parser_current(parser)->kind)) {
                out_label->kind = FENG_MATCH_LABEL_TYPE;
                out_label->type = type_ref;
                if (type_ref->kind == FENG_TYPE_REF_NAMED &&
                    type_ref->as.named.segment_count == 1U &&
                    type_ref->as.named.type_arg_count == 0U) {
                    FengExpr *fallback = new_expr(parser, FENG_EXPR_IDENTIFIER, type_ref->token);

                    if (fallback == NULL) {
                        free_type_ref(type_ref);
                        return false;
                    }
                    fallback->as.identifier = type_ref->as.named.segments[0];
                    out_label->value = fallback;
                }
                return parse_match_label_chain(parser, out_label);
            }
            free_type_ref(type_ref);
            parser->current = before;
        }
    }

    first = parse_match_label_atom(parser);
    if (first == NULL) {
        return false;
    }

    if (parser_check(parser, FENG_TOKEN_ELLIPSIS)) {
        FengExpr *high;

        (void)parser_advance(parser);
        high = parse_match_label_atom(parser);
        if (high == NULL) {
            free_expr(first);
            return false;
        }
        out_label->kind = FENG_MATCH_LABEL_RANGE;
        out_label->range_low = first;
        out_label->range_high = high;
        return true;
    }

    out_label->value = first;
    return true;
}

static void free_match_label_type_chain(FengMatchLabel *label) {
    if (label->type_chain != NULL) {
        for (size_t ci = 0U; ci < label->type_chain_count; ++ci) {
            free_type_ref(label->type_chain[ci]);
        }
        free(label->type_chain);
        label->type_chain = NULL;
        label->type_chain_count = 0U;
    }
}

static void free_match_branch_contents(FengMatchBranch *branch) {
    size_t i;

    if (branch == NULL) {
        return;
    }
    for (i = 0U; i < branch->label_count; ++i) {
        free_expr(branch->labels[i].value);
        free_expr(branch->labels[i].range_low);
        free_expr(branch->labels[i].range_high);
        free_type_ref(branch->labels[i].type);
        free_match_label_type_chain(&branch->labels[i]);
    }
    free(branch->labels);
    free_block(branch->body);
}

/* Free contents of a single FengMatchLabel (used by FENG_EXPR_MATCH_OP
 * which owns a label array without an enclosing FengMatchBranch). */
static void free_match_label_contents(FengMatchLabel *label) {
    if (label == NULL) {
        return;
    }
    free_expr(label->value);
    free_expr(label->range_low);
    free_expr(label->range_high);
    free_type_ref(label->type);
    free_match_label_type_chain(label);
}

/* Try to parse an optional binding prefix before match branch labels.
 * Recognised forms:
 *   [let|var] IDENT :     — explicit mutability + binding name
 *   IDENT :               — implicit let + binding name
 *
 * Returns true on success (binding parsed or no binding found).
 * On return, *out_has_binding indicates whether a binding was consumed.
 * When no binding is detected the parser position is restored and the
 * caller proceeds to parse labels normally. */
static bool parse_match_branch_binding_prefix(
    Parser *parser,
    bool *out_has_binding,
    FengSlice *out_binding_name,
    FengMutability *out_mutability) {
    size_t saved = parser->current;
    FengMutability mut = FENG_MUTABILITY_LET;

    *out_has_binding = false;
    *out_binding_name = (FengSlice){NULL, 0U};
    *out_mutability = FENG_MUTABILITY_LET;

    /* Case 1: explicit let/var keyword. */
    if (parser_check(parser, FENG_TOKEN_KW_LET) ||
        parser_check(parser, FENG_TOKEN_KW_VAR)) {
        if (parser_check(parser, FENG_TOKEN_KW_VAR)) {
            mut = FENG_MUTABILITY_VAR;
        }
        (void)parser_advance(parser);
        if (!parser_check(parser, FENG_TOKEN_IDENTIFIER)) {
            parser->current = saved;
            return true;
        }
        *out_binding_name = slice_from_token(parser_current(parser));
        (void)parser_advance(parser);
        if (!parser_check(parser, FENG_TOKEN_COLON)) {
            parser->current = saved;
            *out_binding_name = (FengSlice){NULL, 0U};
            return true;
        }
        (void)parser_advance(parser);
        *out_has_binding = true;
        *out_mutability = mut;
        return true;
    }

    /* Case 2: bare IDENT : (no let/var keyword, single-segment only). */
    if (parser_check(parser, FENG_TOKEN_IDENTIFIER)) {
        (void)parser_advance(parser);
        /* Multi-segment name (IDENT . ...) is a qualified type reference. */
        if (parser_check(parser, FENG_TOKEN_DOT)) {
            parser->current = saved;
            return true;
        }
        /* Generic type reference (IDENT < ...). */
        if (parser_check(parser, FENG_TOKEN_LT)) {
            parser->current = saved;
            return true;
        }
        if (parser_check(parser, FENG_TOKEN_COLON)) {
            FengSlice name = slice_from_token(parser_previous(parser));
            (void)parser_advance(parser);
            *out_has_binding = true;
            *out_binding_name = name;
            *out_mutability = FENG_MUTABILITY_LET;
            return true;
        }
        parser->current = saved;
    }

    return true;
}

static bool parse_match_branch(Parser *parser, FengMatchBranch *out_branch) {
    size_t label_capacity = 0U;

    out_branch->token = *parser_current(parser);
    out_branch->labels = NULL;
    out_branch->label_count = 0U;
    out_branch->body = NULL;
    out_branch->has_binding = false;
    out_branch->binding_name = (FengSlice){NULL, 0U};
    out_branch->binding_mutability = FENG_MUTABILITY_LET;

    if (!parse_match_branch_binding_prefix(parser,
                                           &out_branch->has_binding,
                                           &out_branch->binding_name,
                                           &out_branch->binding_mutability)) {
        return false;
    }

    for (;;) {
        FengMatchLabel label;

        if (!parse_match_label(parser, &label, false)) {
            free_match_branch_contents(out_branch);
            return false;
        }
        if (!APPEND_VALUE(parser, out_branch->labels, out_branch->label_count, label_capacity, label)) {
            free_match_label_contents(&label);
            free_match_branch_contents(out_branch);
            return false;
        }
        if (!parser_match(parser, FENG_TOKEN_COMMA)) {
            break;
        }
    }

    out_branch->body = parse_block(parser);
    if (out_branch->body == NULL) {
        free_match_branch_contents(out_branch);
        return false;
    }
    return true;
}

/* Parse the RHS of an infix `expr match pattern` operator (precedence same as
 * relational operators). The caller has already consumed the `match` keyword
 * and supplied the LHS in `target`. Returns a new FENG_EXPR_MATCH_OP node or
 * NULL on failure.
 *
 * Pattern syntax reuses parse_match_branch_binding_prefix (for the optional
 * `[let|var] name:` prefix) and parse_match_label (for each label). Multi-label
 * patterns use `|` as the separator (not `,`). All `|` tokens at this position
 * are consumed as label separators; any remaining `|` after the loop is left to
 * the enclosing parse_bit_or layer. */
static FengExpr *parse_infix_match_op(Parser *parser, FengToken match_token, FengExpr *target) {
    FengExpr *expr;
    size_t label_capacity = 0U;

    expr = new_expr(parser, FENG_EXPR_MATCH_OP, match_token);
    if (expr == NULL) {
        free_expr(target);
        return NULL;
    }
    expr->as.match_op.target = target;
    expr->as.match_op.labels = NULL;
    expr->as.match_op.label_count = 0U;
    expr->as.match_op.has_binding = false;
    expr->as.match_op.binding_name = (FengSlice){NULL, 0U};
    expr->as.match_op.binding_mutability = FENG_MUTABILITY_LET;

    if (!parse_match_branch_binding_prefix(parser,
                                           &expr->as.match_op.has_binding,
                                           &expr->as.match_op.binding_name,
                                           &expr->as.match_op.binding_mutability)) {
        free_expr(expr);
        return NULL;
    }

    for (;;) {
        FengMatchLabel label;

        if (!parse_match_label(parser, &label, true)) {
            free_expr(expr);
            return NULL;
        }
        if (!APPEND_VALUE(parser, expr->as.match_op.labels, expr->as.match_op.label_count, label_capacity, label)) {
            free_match_label_contents(&label);
            free_expr(expr);
            return NULL;
        }
        /* infix multi-label uses `|` (not `,`); consume all consecutive labels
         * separated by `|` here. Remaining `|` after the loop is left to
         * parse_bit_or (bitwise-or), which is well-defined because
         * `bool | int` is illegal (AE0030). */
        if (!parser_match(parser, FENG_TOKEN_PIPE)) {
            break;
        }
    }
    return expr;
}

/* Parses a match body's contents from the current token (after the `{` is
 * already consumed) up to and including the closing `}`. Branches are
 * appended to *branches, and *out_else_block receives the optional else body
 * (NULL if none). */
static bool parse_match_body(Parser *parser,
                             FengMatchBranch **branches,
                             size_t *branch_count,
                             FengBlock **out_else_block) {
    size_t branch_capacity = 0U;
    bool seen_else = false;

    *out_else_block = NULL;

    while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
        if (parser_check(parser, FENG_TOKEN_KW_ELSE)) {
            if (seen_else) {
                (void)parser_error_current(parser,
                    "SE1104", "match expression cannot declare more than one 'else' branch");
                return false;
            }
            (void)parser_advance(parser);
            *out_else_block = parse_block(parser);
            if (*out_else_block == NULL) {
                return false;
            }
            seen_else = true;
        } else {
            FengMatchBranch branch;

            if (!parse_match_branch(parser, &branch)) {
                return false;
            }
            if (!APPEND_VALUE(parser, *branches, *branch_count, branch_capacity, branch)) {
                free_match_branch_contents(&branch);
                return false;
            }
        }
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE1106", "expected '}' to close match body")) {
        return false;
    }
    return true;
}

/* Convert a trailing FENG_STMT_IF in a yield-required block into
 * FENG_STMT_EXPR(FENG_EXPR_IF).  Multi-clause (else-if chain) forms are
 * nested from back to front.  Ownership of conditions and blocks is
 * transferred from the clause array to newly created if-expressions;
 * only the clause array and old statement shell are freed. */
static void convert_if_stmt_to_if_expr(Parser *parser, FengBlock *block) {
    FengStmt *old = block->statements[block->statement_count - 1U];
    FengExpr *inner_else_block_expr = NULL;
    FengStmt *expr_stmt;

    /* Build from the innermost clause outward. */
    for (size_t i = old->as.if_stmt.clause_count; i-- > 0U; ) {
        FengExpr *if_e = new_expr(parser, FENG_EXPR_IF, old->as.if_stmt.clauses[i].token);
        if (if_e == NULL) {
            /* Conversion failed (OOM).  Leave the original statement in
             * place; the semantic analyser will report AE1101. */
            return;
        }
        if_e->as.if_expr.condition = old->as.if_stmt.clauses[i].condition;
        if_e->as.if_expr.then_block = old->as.if_stmt.clauses[i].block;
        if_e->as.if_expr.else_block = (i == old->as.if_stmt.clause_count - 1U)
                                          ? old->as.if_stmt.else_block
                                          : NULL;
        if (inner_else_block_expr != NULL) {
            FengStmt *wrapper = new_stmt(parser, FENG_STMT_EXPR, inner_else_block_expr->token);
            if (wrapper == NULL) {
                /* OOM: free the partial expression and abort conversion. */
                free_expr(if_e);
                return;
            }
            wrapper->as.expr = inner_else_block_expr;
            if_e->as.if_expr.else_block = new_block(parser, inner_else_block_expr->token);
            if (if_e->as.if_expr.else_block == NULL) {
                free_stmt(wrapper);
                free_expr(if_e);
                return;
            }
            if_e->as.if_expr.else_block->statements = (FengStmt **)malloc(sizeof(FengStmt *));
            if (if_e->as.if_expr.else_block->statements == NULL) {
                free_stmt(wrapper);
                free_expr(if_e);
                return;
            }
            if_e->as.if_expr.else_block->statements[0] = wrapper;
            if_e->as.if_expr.else_block->statement_count = 1U;
        }
        inner_else_block_expr = if_e;
    }

    expr_stmt = new_stmt(parser, FENG_STMT_EXPR, old->token);
    if (expr_stmt == NULL) {
        return;
    }
    expr_stmt->as.expr = inner_else_block_expr;

    /* Replace the old statement.  Free only the clause array and the
     * statement shell; conditions and blocks have been transferred. */
    block->statements[block->statement_count - 1U] = expr_stmt;
    free(old->as.if_stmt.clauses);
    free(old);
}

/* Convert a trailing FENG_STMT_MATCH in a yield-required block into
 * FENG_STMT_EXPR(FENG_EXPR_MATCH).  Target, branches, and else_block
 * are transferred; only the branches array pointer and old statement
 * shell are freed. */
static void convert_match_stmt_to_match_expr(Parser *parser, FengBlock *block) {
    FengStmt *old = block->statements[block->statement_count - 1U];
    FengExpr *match_e = new_expr(parser, FENG_EXPR_MATCH, old->token);
    FengStmt *expr_stmt;

    if (match_e == NULL) {
        return;
    }
    match_e->as.match_expr.target = old->as.match_stmt.target;
    match_e->as.match_expr.branches = old->as.match_stmt.branches;
    match_e->as.match_expr.branch_count = old->as.match_stmt.branch_count;
    match_e->as.match_expr.else_block = old->as.match_stmt.else_block;

    expr_stmt = new_stmt(parser, FENG_STMT_EXPR, old->token);
    if (expr_stmt == NULL) {
        /* OOM: free the expression shell but not transferred contents. */
        free(match_e);
        return;
    }
    expr_stmt->as.expr = match_e;

    block->statements[block->statement_count - 1U] = expr_stmt;
    /* Branches array and contents are now owned by match_expr; free only
     * the statement shell (no match_stmt contents to free here). */
    free(old);
}

/* Convert a trailing FENG_STMT_TRY into FENG_STMT_EXPR(FENG_EXPR_TRY).
 * The inner try expression is already built by parse_try_expression and
 * stored in stmt->as.expr; we just unwrap it. */
static void convert_try_stmt_to_try_expr(FengBlock *block) {
    FengStmt *old = block->statements[block->statement_count - 1U];
    FengExpr *try_e = old->as.expr;

    old->as.expr = NULL;
    old->kind = FENG_STMT_EXPR;
    old->as.expr = try_e;
}

/* Post-process a block whose last statement must yield a value (e.g.
 * if-expression branches, match branches, catch clauses).  If the
 * trailing statement is an if-stmt, match-stmt, or try-stmt, convert
 * it to the expression form so block_yield_expression can extract it. */
static void convert_trailing_yield_stmt_to_expr(Parser *parser, FengBlock *block) {
    FengStmt *last;

    if (block == NULL || block->statement_count == 0U) {
        return;
    }
    last = block->statements[block->statement_count - 1U];
    if (last == NULL) {
        return;
    }
    switch (last->kind) {
        case FENG_STMT_IF:
            convert_if_stmt_to_if_expr(parser, block);
            break;
        case FENG_STMT_MATCH:
            convert_match_stmt_to_match_expr(parser, block);
            break;
        case FENG_STMT_TRY:
            convert_try_stmt_to_try_expr(block);
            break;
        default:
            break;
    }
}

static FengExpr *parse_match_expression(Parser *parser, FengToken match_token) {
    FengExpr *target;
    FengExpr *expr;
    bool saved_suppress = parser->suppress_object_literal_suffix;

    parser->suppress_object_literal_suffix = true;
    target = parse_expression(parser);
    parser->suppress_object_literal_suffix = saved_suppress;

    if (target == NULL) {
        return NULL;
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "SE1101", "match expressions must use '{...}' after the target")) {
        free_expr(target);
        return NULL;
    }

    expr = new_expr(parser, FENG_EXPR_MATCH, match_token);
    if (expr == NULL) {
        free_expr(target);
        return NULL;
    }
    expr->as.match_expr.target = target;
    if (!parse_match_body(parser,
                          &expr->as.match_expr.branches,
                          &expr->as.match_expr.branch_count,
                          &expr->as.match_expr.else_block)) {
        free_expr(expr);
        return NULL;
    }
    for (size_t bi = 0U; bi < expr->as.match_expr.branch_count; ++bi) {
        convert_trailing_yield_stmt_to_expr(parser, expr->as.match_expr.branches[bi].body);
    }
    if (expr->as.match_expr.else_block == NULL) {
        (void)parser_error_at(parser, &match_token,
                              "SE1103", "match expressions require an 'else' branch");
        free_expr(expr);
        return NULL;
    }
    convert_trailing_yield_stmt_to_expr(parser, expr->as.match_expr.else_block);
    return expr;
}

static FengExpr *parse_if_expression(Parser *parser, FengToken if_token) {
    FengExpr *condition;
    FengExpr *expr;
    bool saved_suppress = parser->suppress_object_literal_suffix;

    parser->suppress_object_literal_suffix = true;
    condition = parse_expression(parser);
    parser->suppress_object_literal_suffix = saved_suppress;

    if (condition == NULL) {
        return NULL;
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "SE1101", "if expressions must use '{...}' after the condition")) {
        free_expr(condition);
        return NULL;
    }

    expr = new_expr(parser, FENG_EXPR_IF, if_token);
    if (expr == NULL) {
        free_expr(condition);
        return NULL;
    }
    expr->as.if_expr.condition = condition;
    expr->as.if_expr.then_block = new_block(parser, parser_current_token(parser));
    if (expr->as.if_expr.then_block == NULL) {
        free_expr(expr);
        return NULL;
    }
    {
        size_t capacity = 0U;
        while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
            FengStmt *stmt = parse_statement(parser);
            if (stmt == NULL) {
                free_expr(expr);
                return NULL;
            }
            if (!APPEND_VALUE(parser,
                              expr->as.if_expr.then_block->statements,
                              expr->as.if_expr.then_block->statement_count,
                              capacity,
                              stmt)) {
                free_stmt(stmt);
                free_expr(expr);
                return NULL;
            }
        }
    }
    convert_trailing_yield_stmt_to_expr(parser, expr->as.if_expr.then_block);
    if (!parser_expect(parser,
                       FENG_TOKEN_RBRACE,
                       "SE1106", "expected '}' to close the true branch of if expression")) {
        free_expr(expr);
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_KW_ELSE, "SE1102", "if expressions require an 'else' branch")) {
        free_expr(expr);
        return NULL;
    }
    expr->as.if_expr.else_block = parse_block(parser);
    if (expr->as.if_expr.else_block == NULL) {
        free_expr(expr);
        return NULL;
    }
    convert_trailing_yield_stmt_to_expr(parser, expr->as.if_expr.else_block);
    return expr;
}

static FengExpr *parse_try_expression(Parser *parser, FengToken try_token) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_TRY, try_token);
    size_t clause_capacity = 0U;

    if (expr == NULL) {
        return NULL;
    }

    expr->as.try_expr.body = parse_expression(parser);
    if (expr->as.try_expr.body == NULL) {
        free_expr(expr);
        return NULL;
    }

    while (parser_match(parser, FENG_TOKEN_KW_CATCH)) {
        FengTryCatchClause clause;

        memset(&clause, 0, sizeof(clause));
        clause.token = parser_previous_token(parser);

        if (!parser_check(parser, FENG_TOKEN_LBRACE)) {
            if (!parser_expect_identifier_like(parser,
                                               &clause.name,
                                               false,
                                               "SE1402", "catch clauses must bind an exception name")) {
                free_expr(expr);
                return NULL;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "SE1403", "catch clauses must include a ': Type' annotation")) {
                free_expr(expr);
                return NULL;
            }
            clause.type = parse_type_ref(parser);
            if (clause.type == NULL) {
                free_expr(expr);
                return NULL;
            }
        }
        clause.body = parse_block(parser);
        if (clause.body == NULL) {
            free_type_ref(clause.type);
            free_expr(expr);
            return NULL;
        }
        if (!APPEND_VALUE(parser,
                          expr->as.try_expr.clauses,
                          expr->as.try_expr.clause_count,
                          clause_capacity,
                          clause)) {
            free_type_ref(clause.type);
            free_block(clause.body);
            free_expr(expr);
            return NULL;
        }
    }

    if (expr->as.try_expr.clause_count == 0U) {
        (void)parser_error_at(parser, &try_token, "SE1401", "'try' requires at least one 'catch' clause");
        free_expr(expr);
        return NULL;
    }

    return expr;
}

static FengExpr *parse_group_or_cast(Parser *parser) {
    FengToken group_token = parser_current_token(parser);

    if (looks_like_lambda(parser)) {
        return parse_lambda(parser);
    }

    if (looks_like_cast(parser)) {
        FengExpr *expr = new_expr(parser, FENG_EXPR_CAST, parser_current_token(parser));

        if (expr == NULL) {
            return NULL;
        }

        if (!parser_expect(parser, FENG_TOKEN_LPAREN, "SE0004", "expected '(' to start cast expression")) {
            free_expr(expr);
            return NULL;
        }
        expr->as.cast.type = parse_type_ref(parser);
        if (expr->as.cast.type == NULL) {
            free_expr(expr);
            return NULL;
        }
        if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0004", "expected ')' after cast type")) {
            free_expr(expr);
            return NULL;
        }
        expr->as.cast.value = parse_unary(parser);
        if (expr->as.cast.value == NULL) {
            free_expr(expr);
            return NULL;
        }
        return expr;
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LPAREN,
                       "SE0516", "expected '(' to start grouped expression, cast, or lambda")) {
        return NULL;
    }

    {
        if (parser_check(parser, FENG_TOKEN_RPAREN)) {
            return parse_empty_tuple_literal(parser, group_token);
        }

        FengExpr *expr;
        bool saved_suppress = parser->suppress_object_literal_suffix;
        parser->suppress_object_literal_suffix = false;
        expr = parse_expression(parser);
        parser->suppress_object_literal_suffix = saved_suppress;

        if (expr == NULL) {
            return NULL;
        }
        if (parser_match(parser, FENG_TOKEN_COMMA)) {
            return parse_tuple_literal_tail(parser, group_token, expr);
        }
        if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0005", "expected ')' to close grouped expression")) {
            free_expr(expr);
            return NULL;
        }
        return expr;
    }
}

static FengExpr *parse_primary(Parser *parser) {
    FengToken token = *parser_current(parser);
    FengExpr *expr;

    switch (token.kind) {
        case FENG_TOKEN_IDENTIFIER:
            expr = new_expr(parser, FENG_EXPR_IDENTIFIER, token);
            if (expr != NULL) {
                expr->as.identifier = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_KW_SELF:
            expr = new_expr(parser, FENG_EXPR_SELF, token);
            if (expr != NULL) {
                expr->as.identifier = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_BOOL:
            expr = new_expr(parser, FENG_EXPR_BOOL, token);
            if (expr != NULL) {
                expr->as.boolean = token.value.boolean;
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_INTEGER:
            expr = new_expr(parser, FENG_EXPR_INTEGER, token);
            if (expr != NULL) {
                expr->as.integer = token.value.integer;
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_FLOAT:
            expr = new_expr(parser, FENG_EXPR_FLOAT, token);
            if (expr != NULL) {
                expr->as.floating = token.value.floating;
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_STRING:
            expr = new_expr(parser, FENG_EXPR_STRING, token);
            if (expr != NULL) {
                expr->as.string = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_LBRACKET:
            return parse_array_literal(parser);
        case FENG_TOKEN_LPAREN:
            return parse_group_or_cast(parser);
        case FENG_TOKEN_KW_IF:
            (void)parser_advance(parser);
            return parse_if_expression(parser, token);
        case FENG_TOKEN_KW_MATCH:
            (void)parser_advance(parser);
            return parse_match_expression(parser, token);
        case FENG_TOKEN_KW_TRY: {
            FengExpr *try_expr;

            (void)parser_advance(parser);
            try_expr = parse_try_expression(parser, token);
            if (try_expr != NULL) {
                for (size_t ci = 0U; ci < try_expr->as.try_expr.clause_count; ++ci) {
                    convert_trailing_yield_stmt_to_expr(parser, try_expr->as.try_expr.clauses[ci].body);
                }
            }
            return try_expr;
        }
        case FENG_TOKEN_ELLIPSIS:
            (void)parser_error_current(
                parser,
                "SE1003",
                "prepacked variadic forwarding is only allowed before the final call argument");
            return NULL;
        default:
            (void)parser_error_current(
                parser,
                "SE0006", "expected expression term: identifier, literal, call, cast, lambda, if-expression, or try-expression");
            return NULL;
    }
}

static FengExpr *parse_postfix(Parser *parser) {
    FengExpr *expr = parse_primary(parser);

    if (expr == NULL) {
        return NULL;
    }

    for (;;) {
        /* Explicit generic target: callee<T1, T2> / Type<T1, T2> */
        if (expr_can_take_explicit_type_args(expr) &&
            looks_like_explicit_generic_target_suffix(parser)) {
            FengExpr *generic_target;
            size_t type_arg_count = 0U;
            FengTypeRef **type_args = NULL;

            (void)parser_advance(parser); /* consume LT */
            if (!parse_type_args(parser, &type_args, &type_arg_count)) {
                free_expr(expr);
                return NULL;
            }

            generic_target = new_expr(parser, FENG_EXPR_GENERIC_TARGET, expr->token);
            if (generic_target == NULL) {
                free_type_arg_refs(type_args, type_arg_count);
                free_expr(expr);
                return NULL;
            }

            generic_target->as.generic_target.target = expr;
            generic_target->as.generic_target.type_args = type_args;
            generic_target->as.generic_target.type_arg_count = type_arg_count;
            expr = generic_target;
            continue;
        }

        if (parser_match(parser, FENG_TOKEN_LPAREN)) {
            FengExpr *call;
            size_t arg_capacity = 0U;

            if (expr->kind == FENG_EXPR_GENERIC_TARGET) {
                call = new_call_from_explicit_generic_target(parser, expr);
                if (call == NULL) {
                    return NULL;
                }
            } else {
                call = new_expr(parser, FENG_EXPR_CALL, expr->token);
                if (call == NULL) {
                    free_expr(expr);
                    return NULL;
                }
                call->as.call.callee = expr;
            }

            if (!parser_check(parser, FENG_TOKEN_RPAREN)) {
                bool saved_suppress = parser->suppress_object_literal_suffix;
                parser->suppress_object_literal_suffix = false;
                do {
                    bool is_prepacked_variadic_arg =
                        parser_match(parser, FENG_TOKEN_ELLIPSIS);
                    FengExpr *arg = parse_expression(parser);

                    if (arg == NULL) {
                        parser->suppress_object_literal_suffix = saved_suppress;
                        free_expr(call);
                        return NULL;
                    }
                    arg->is_prepacked_variadic_arg = is_prepacked_variadic_arg;
                    if (!APPEND_VALUE(parser, call->as.call.args, call->as.call.arg_count, arg_capacity, arg)) {
                        parser->suppress_object_literal_suffix = saved_suppress;
                        free_expr(arg);
                        free_expr(call);
                        return NULL;
                    }
                    if (is_prepacked_variadic_arg &&
                        parser_check(parser, FENG_TOKEN_COMMA)) {
                        parser->suppress_object_literal_suffix = saved_suppress;
                        (void)parser_error_current(
                            parser,
                            "SE1003",
                            "prepacked variadic forwarding must be the last call argument");
                        free_expr(call);
                        return NULL;
                    }
                } while (parser_match(parser, FENG_TOKEN_COMMA));
                parser->suppress_object_literal_suffix = saved_suppress;
            }

            if (!parser_expect(parser, FENG_TOKEN_RPAREN, "SE0005", "expected ')' to close argument list")) {
                free_expr(call);
                return NULL;
            }
            expr = call;
            continue;
        }

        if (parser_match(parser, FENG_TOKEN_DOT)) {
            FengToken member_token = parser_current_token(parser);
            FengExpr *member;

            if (parser_check(parser, FENG_TOKEN_TILDE)) {
                free_expr(expr);
                (void)parser_error_current(
                    parser,
                    "SE0512", "finalizer cannot be invoked directly via '.~'");
                return NULL;
            }

            member = new_expr(parser, FENG_EXPR_MEMBER, member_token);

            if (member == NULL) {
                free_expr(expr);
                return NULL;
            }
            if (!parser_expect_member_name(parser,
                                           &member->as.member.member,
                                           "SE0002", "expected an identifier after '.' in member access")) {
                free_expr(member);
                free_expr(expr);
                return NULL;
            }
            member->as.member.object = expr;
            expr = member;
            continue;
        }

        if (parser_match(parser, FENG_TOKEN_LBRACKET)) {
            if (parser_match(parser, FENG_TOKEN_COLON)) {
                FengExpr *size_expr;
                FengExpr *arr_new;
                FengTypeRef *elem_type;
                FengSlice *seg;
                FengExpr *type_target = expr;
                FengExpr *generic_target = NULL;

                /* Array-new now consistently uses Type[:n], including Type<Args>[:n]. */
                if (expr->kind == FENG_EXPR_GENERIC_TARGET) {
                    generic_target = expr;
                    type_target = expr->as.generic_target.target;
                }

                if (type_target == NULL || type_target->kind != FENG_EXPR_IDENTIFIER) {
                    free_expr(expr);
                    (void)parser_error_current(
                        parser,
                        "SE0201", "array-new segment '[:expr]' requires a type name");
                    return NULL;
                }

                {
                    bool saved_suppress = parser->suppress_object_literal_suffix;
                    parser->suppress_object_literal_suffix = false;
                    size_expr = parse_expression(parser);
                    parser->suppress_object_literal_suffix = saved_suppress;
                }
                if (size_expr == NULL) {
                    free_expr(expr);
                    return NULL;
                }
                if (!parser_expect(parser,
                                   FENG_TOKEN_RBRACKET,
                                   "SE0202", "expected ']' after array size in '[:expr]'")) {
                    free_expr(size_expr);
                    free_expr(expr);
                    return NULL;
                }

                arr_new = new_expr(parser, FENG_EXPR_ARRAY_NEW, expr->token);
                if (arr_new == NULL) {
                    free_expr(size_expr);
                    free_expr(expr);
                    return NULL;
                }
                elem_type = new_type_ref(parser, FENG_TYPE_REF_NAMED, type_target->token);
                if (elem_type == NULL) {
                    free_expr(arr_new);
                    free_expr(size_expr);
                    free_expr(expr);
                    return NULL;
                }
                seg = (FengSlice *)malloc(sizeof *seg);
                if (seg == NULL) {
                    free_type_ref(elem_type);
                    free_expr(arr_new);
                    free_expr(size_expr);
                    free_expr(expr);
                    return NULL;
                }

                *seg = type_target->as.identifier;
                elem_type->as.named.segments = seg;
                elem_type->as.named.segment_count = 1U;
                if (generic_target != NULL) {
                    elem_type->as.named.type_args = generic_target->as.generic_target.type_args;
                    elem_type->as.named.type_arg_count = generic_target->as.generic_target.type_arg_count;
                    generic_target->as.generic_target.type_args = NULL;
                    generic_target->as.generic_target.type_arg_count = 0U;
                }
                arr_new->as.array_new.element_type = elem_type;
                arr_new->as.array_new.size = size_expr;
                free_expr(expr);
                expr = arr_new;
                continue;
            }

            /* Ordinary index expression: value[index]. */
            {
                FengExpr *inner;
                FengExpr *index;
                bool saved_suppress = parser->suppress_object_literal_suffix;
                parser->suppress_object_literal_suffix = false;
                inner = parse_expression(parser);
                parser->suppress_object_literal_suffix = saved_suppress;

                if (inner == NULL) {
                    free_expr(expr);
                    return NULL;
                }
                if (!parser_expect(parser,
                                   FENG_TOKEN_RBRACKET,
                                   "SE0202",
                                   "expected ']' to close index expression")) {
                    free_expr(inner);
                    free_expr(expr);
                    return NULL;
                }

                index = new_expr(parser, FENG_EXPR_INDEX, expr->token);
                if (index == NULL) {
                    free_expr(inner);
                    free_expr(expr);
                    return NULL;
                }
                index->as.index.object = expr;
                index->as.index.index = inner;
                expr = index;
                continue;
            }
        }

        break;
    }

    if (!parser->suppress_object_literal_suffix) {
        if (expr->kind == FENG_EXPR_GENERIC_TARGET && looks_like_object_literal(parser)) {
            expr = new_call_from_explicit_generic_target(parser, expr);
            if (expr == NULL) {
                return NULL;
            }
        }

        if ((expr->kind == FENG_EXPR_IDENTIFIER || expr->kind == FENG_EXPR_MEMBER || expr->kind == FENG_EXPR_CALL) &&
            looks_like_object_literal(parser)) {
            return parse_object_literal_suffix(parser, expr);
        }
    }

    return expr;
}

static FengExpr *parse_unary(Parser *parser) {
    if (parser_match(parser, FENG_TOKEN_NOT) ||
        parser_match(parser, FENG_TOKEN_AMP) ||
        parser_match(parser, FENG_TOKEN_MINUS) ||
        parser_match(parser, FENG_TOKEN_TILDE)) {
        FengExpr *expr = new_expr(parser, FENG_EXPR_UNARY, parser_previous_token(parser));

        if (expr == NULL) {
            return NULL;
        }
        expr->as.unary.op = parser_previous(parser)->kind;
        expr->as.unary.operand = parse_unary(parser);
        if (expr->as.unary.operand == NULL) {
            free_expr(expr);
            return NULL;
        }
        return expr;
    }

    return parse_postfix(parser);
}

static FengExpr *parse_binary_series(Parser *parser,
                                     FengExpr *(*subparser)(Parser *),
                                     const FengTokenKind *operators,
                                     size_t operator_count) {
    FengExpr *expr = subparser(parser);
    size_t index;

    if (expr == NULL) {
        return NULL;
    }

    for (;;) {
        bool matched = false;

        for (index = 0U; index < operator_count; ++index) {
            if (parser_match(parser, operators[index])) {
                FengExpr *binary = new_expr(parser, FENG_EXPR_BINARY, parser_previous_token(parser));

                if (binary == NULL) {
                    free_expr(expr);
                    return NULL;
                }
                binary->as.binary.op = parser_previous(parser)->kind;
                binary->as.binary.left = expr;
                binary->as.binary.right = subparser(parser);
                if (binary->as.binary.right == NULL) {
                    free_expr(binary);
                    return NULL;
                }
                expr = binary;
                matched = true;
                break;
            }
        }

        if (!matched) {
            return expr;
        }
    }
}

static FengExpr *parse_multiplicative(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_STAR, FENG_TOKEN_SLASH, FENG_TOKEN_PERCENT};
    return parse_binary_series(parser, parse_unary, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_additive(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_PLUS, FENG_TOKEN_MINUS};
    return parse_binary_series(parser, parse_multiplicative, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_shift(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_SHL, FENG_TOKEN_SHR};
    return parse_binary_series(parser, parse_additive, operators, sizeof(operators) / sizeof(operators[0]));
}

/* Forward declaration: parse_comparison (next level) calls parse_bit_or,
 * but parse_bit_or is defined after parse_comparison in source order. */
static FengExpr *parse_bit_or(Parser *parser);

static FengExpr *parse_comparison(Parser *parser) {
    FengExpr *expr = parse_bit_or(parser);

    if (expr == NULL) {
        return NULL;
    }

    for (;;) {
        bool matched = false;
        size_t index;
        static const FengTokenKind operators[] = {FENG_TOKEN_LT, FENG_TOKEN_LE, FENG_TOKEN_GT, FENG_TOKEN_GE};

        for (index = 0U; index < sizeof(operators) / sizeof(operators[0]); ++index) {
            if (parser_match(parser, operators[index])) {
                FengExpr *binary = new_expr(parser, FENG_EXPR_BINARY, parser_previous_token(parser));

                if (binary == NULL) {
                    free_expr(expr);
                    return NULL;
                }
                binary->as.binary.op = parser_previous(parser)->kind;
                binary->as.binary.left = expr;
                binary->as.binary.right = parse_bit_or(parser);
                if (binary->as.binary.right == NULL) {
                    free_expr(binary);
                    return NULL;
                }
                expr = binary;
                matched = true;
                break;
            }
        }
        /* infix `match` shares the same precedence level as relational
         * operators (left-associative, same loop). `match` consumes the
         * following pattern via parse_infix_match_op rather than a plain
         * sub-expression. */
        if (!matched && parser_match(parser, FENG_TOKEN_KW_MATCH)) {
            FengExpr *match_op = parse_infix_match_op(parser, parser_previous_token(parser), expr);

            if (match_op == NULL) {
                return NULL;
            }
            expr = match_op;
            matched = true;
        }
        if (!matched) {
            return expr;
        }
    }
}

static FengExpr *parse_equality(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_EQ, FENG_TOKEN_NE};
    return parse_binary_series(parser, parse_comparison, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_bit_and(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_AMP};
    return parse_binary_series(parser, parse_shift, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_bit_xor(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_CARET};
    return parse_binary_series(parser, parse_bit_and, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_bit_or(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_PIPE};
    return parse_binary_series(parser, parse_bit_xor, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_and(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_AND_AND};
    return parse_binary_series(parser, parse_equality, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_or(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_OR_OR};
    return parse_binary_series(parser, parse_and, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_expression(Parser *parser) {
    return parse_or(parser);
}

static FengBlock *parse_block(Parser *parser) {
    FengBlock *block = new_block(parser, parser_current_token(parser));
    size_t capacity = 0U;

    if (block == NULL) {
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_LBRACE, "SE0005", "expected '{' to start block")) {
        free_block(block);
        return NULL;
    }

    while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
        FengStmt *stmt = parse_statement(parser);

        if (stmt == NULL) {
            free_block(block);
            return NULL;
        }
        if (!APPEND_VALUE(parser, block->statements, block->statement_count, capacity, stmt)) {
            free_stmt(stmt);
            free_block(block);
            return NULL;
        }
    }

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE0005", "expected '}' to close block")) {
        free_block(block);
        return NULL;
    }
    return block;
}

static FengStmt *parse_match_statement(Parser *parser) {
    FengToken match_token = parser_previous_token(parser);
    FengExpr *target;
    FengStmt *stmt;
    bool saved_suppress = parser->suppress_object_literal_suffix;

    /* Suppress object literal suffix detection so the `{` is preserved for
     * the match body. */
    parser->suppress_object_literal_suffix = true;
    target = parse_expression(parser);
    parser->suppress_object_literal_suffix = saved_suppress;

    if (target == NULL) {
        return NULL;
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "SE1106", "expected '{' after match target")) {
        free_expr(target);
        return NULL;
    }

    stmt = new_stmt(parser, FENG_STMT_MATCH, match_token);
    if (stmt == NULL) {
        free_expr(target);
        return NULL;
    }
    stmt->as.match_stmt.target = target;
    if (!parse_match_body(parser,
                          &stmt->as.match_stmt.branches,
                          &stmt->as.match_stmt.branch_count,
                          &stmt->as.match_stmt.else_block)) {
        free_stmt(stmt);
        return NULL;
    }
    return stmt;
}

static FengStmt *parse_if_statement(Parser *parser) {
    FengToken if_token = parser_previous_token(parser);
    FengExpr *first_condition;
    bool saved_suppress = parser->suppress_object_literal_suffix;

    /* Parse the boolean condition. Suppress object literal suffix detection
     * so the `{` is preserved for the body. */
    parser->suppress_object_literal_suffix = true;
    first_condition = parse_expression(parser);
    parser->suppress_object_literal_suffix = saved_suppress;

    if (first_condition == NULL) {
        return NULL;
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "SE1106", "expected '{' after if condition")) {
        free_expr(first_condition);
        return NULL;
    }

    {
        FengStmt *stmt = new_stmt(parser, FENG_STMT_IF, if_token);
        size_t capacity = 0U;
        FengIfClause clause;

        if (stmt == NULL) {
            free_expr(first_condition);
            return NULL;
        }

        clause.condition = first_condition;
        clause.token = first_condition->token;
        clause.block = new_block(parser, parser_current_token(parser));
        if (clause.block == NULL) {
            free_expr(first_condition);
            free_stmt(stmt);
            return NULL;
        }
        {
            size_t block_capacity = 0U;
            while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
                FengStmt *body_stmt = parse_statement(parser);
                if (body_stmt == NULL) {
                    free_block(clause.block);
                    free_expr(first_condition);
                    free_stmt(stmt);
                    return NULL;
                }
                if (!APPEND_VALUE(parser,
                                  clause.block->statements,
                                  clause.block->statement_count,
                                  block_capacity,
                                  body_stmt)) {
                    free_stmt(body_stmt);
                    free_block(clause.block);
                    free_expr(first_condition);
                    free_stmt(stmt);
                    return NULL;
                }
            }
        }
        if (!parser_expect(parser, FENG_TOKEN_RBRACE, "SE1106", "expected '}' to close if block")) {
            free_block(clause.block);
            free_expr(first_condition);
            free_stmt(stmt);
            return NULL;
        }
        if (!APPEND_VALUE(parser,
                          stmt->as.if_stmt.clauses,
                          stmt->as.if_stmt.clause_count,
                          capacity,
                          clause)) {
            free_expr(first_condition);
            free_block(clause.block);
            free_stmt(stmt);
            return NULL;
        }

        while (parser_match(parser, FENG_TOKEN_KW_ELSE)) {
            if (parser_match(parser, FENG_TOKEN_KW_IF)) {
                FengIfClause more;

                more.condition = parse_expression(parser);
                if (more.condition == NULL) {
                    free_stmt(stmt);
                    return NULL;
                }
                more.token = more.condition->token;
                more.block = parse_block(parser);
                if (more.block == NULL) {
                    free_expr(more.condition);
                    free_stmt(stmt);
                    return NULL;
                }
                if (!APPEND_VALUE(parser,
                                  stmt->as.if_stmt.clauses,
                                  stmt->as.if_stmt.clause_count,
                                  capacity,
                                  more)) {
                    free_expr(more.condition);
                    free_block(more.block);
                    free_stmt(stmt);
                    return NULL;
                }
                continue;
            }

            stmt->as.if_stmt.else_block = parse_block(parser);
            if (stmt->as.if_stmt.else_block == NULL) {
                free_stmt(stmt);
                return NULL;
            }
            break;
        }

        return stmt;
    }
}

static FengStmt *parse_while_statement(Parser *parser) {
    FengStmt *stmt = new_stmt(parser, FENG_STMT_WHILE, parser_previous_token(parser));

    if (stmt == NULL) {
        return NULL;
    }
    stmt->as.while_stmt.condition = parse_expression(parser);
    if (stmt->as.while_stmt.condition == NULL) {
        free_stmt(stmt);
        return NULL;
    }
    stmt->as.while_stmt.body = parse_block(parser);
    if (stmt->as.while_stmt.body == NULL) {
        free_stmt(stmt);
        return NULL;
    }
    return stmt;
}

/* Parse `defer { ... }`. Per docs/specifications/feng-defer.md §3.2 the body MUST be a
 * braced block; single-statement form is not allowed. */
static FengStmt *parse_defer_statement(Parser *parser) {
    FengStmt *stmt = new_stmt(parser, FENG_STMT_DEFER, parser_previous_token(parser));
    if (stmt == NULL) {
        return NULL;
    }
    stmt->as.defer_block = parse_block(parser);
    if (stmt->as.defer_block == NULL) {
        free_stmt(stmt);
        return NULL;
    }
    return stmt;
}

static FengStmt *parse_for_statement(Parser *parser) {
    FengStmt *stmt = new_stmt(parser, FENG_STMT_FOR, parser_previous_token(parser));

    if (stmt == NULL) {
        return NULL;
    }

    /* Detect for/in: `for let|var IDENT in EXPR { ... }`. The lookahead must
     * be unambiguous because three-clause `for` may also start with `let`/`var`. */
    if ((parser_check(parser, FENG_TOKEN_KW_LET) || parser_check(parser, FENG_TOKEN_KW_VAR)) &&
        parser_peek(parser, 1U)->kind == FENG_TOKEN_IDENTIFIER &&
        parser_peek(parser, 2U)->kind == FENG_TOKEN_KW_IN) {
        FengToken kw_token = *parser_current(parser);
        FengMutability mutability = (kw_token.kind == FENG_TOKEN_KW_LET)
                                        ? FENG_MUTABILITY_LET
                                        : FENG_MUTABILITY_VAR;
        FengToken name_token;

        (void)parser_advance(parser); /* consume let/var */
        name_token = *parser_current(parser);
        (void)parser_advance(parser); /* consume identifier */
        (void)parser_advance(parser); /* consume 'in' */

        stmt->as.for_stmt.is_for_in = true;
        stmt->as.for_stmt.iter_binding.token = name_token;
        stmt->as.for_stmt.iter_binding.mutability = mutability;
        stmt->as.for_stmt.iter_binding.name = slice_from_token(&name_token);
        stmt->as.for_stmt.iter_binding.type = NULL;
        stmt->as.for_stmt.iter_binding.initializer = NULL;

        {
            bool saved_suppress = parser->suppress_object_literal_suffix;

            /* Preserve the first `{` after the unparenthesized iteration
             * expression as the loop body delimiter. Parenthesized
             * expressions re-enable object literal suffix parsing. */
            parser->suppress_object_literal_suffix = true;
            stmt->as.for_stmt.iter_expr = parse_expression(parser);
            parser->suppress_object_literal_suffix = saved_suppress;
        }
        if (stmt->as.for_stmt.iter_expr == NULL) {
            free_stmt(stmt);
            return NULL;
        }

        stmt->as.for_stmt.body = parse_block(parser);
        if (stmt->as.for_stmt.body == NULL) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    stmt->as.for_stmt.is_for_in = false;

    if (!parser_check(parser, FENG_TOKEN_SEMICOLON)) {
        stmt->as.for_stmt.init = parse_simple_statement(parser, FENG_TOKEN_SEMICOLON);
        if (stmt->as.for_stmt.init == NULL) {
            free_stmt(stmt);
            return NULL;
        }
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_SEMICOLON,
                       "SE1201", "for statements require ';' after the initializer")) {
        free_stmt(stmt);
        return NULL;
    }

    if (!parser_check(parser, FENG_TOKEN_SEMICOLON)) {
        stmt->as.for_stmt.condition = parse_expression(parser);
        if (stmt->as.for_stmt.condition == NULL) {
            free_stmt(stmt);
            return NULL;
        }
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_SEMICOLON,
                       "SE1202", "for statements require ';' after the condition")) {
        free_stmt(stmt);
        return NULL;
    }

    if (!parser_check(parser, FENG_TOKEN_LBRACE)) {
        stmt->as.for_stmt.update = parse_simple_statement(parser, FENG_TOKEN_LBRACE);
        if (stmt->as.for_stmt.update == NULL) {
            free_stmt(stmt);
            return NULL;
        }
    }

    stmt->as.for_stmt.body = parse_block(parser);
    if (stmt->as.for_stmt.body == NULL) {
        free_stmt(stmt);
        return NULL;
    }

    return stmt;
}

static FengStmt *parse_simple_statement(Parser *parser, FengTokenKind terminator) {
    FengStmt *stmt;

    if (parser_match(parser, FENG_TOKEN_KW_LET) || parser_match(parser, FENG_TOKEN_KW_VAR)) {
        FengMutability mutability = (parser_previous(parser)->kind == FENG_TOKEN_KW_LET)
                                        ? FENG_MUTABILITY_LET
                                        : FENG_MUTABILITY_VAR;

        stmt = new_stmt(parser, FENG_STMT_BINDING, parser_previous_token(parser));
        if (stmt == NULL) {
            return NULL;
        }
        stmt->as.binding = parse_binding_core(parser, mutability, false, true);
        if (parser->error.message != NULL) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    if (parser_starts_typed_binding_without_keyword(parser)) {
        (void)parser_error_current(parser, "SE0103", "local bindings must start with 'let' or 'var'");
        return NULL;
    }

    stmt = new_stmt(parser, FENG_STMT_EXPR, parser_current_token(parser));
    if (stmt == NULL) {
        return NULL;
    }

    stmt->as.expr = parse_expression(parser);
    if (stmt->as.expr == NULL) {
        free_stmt(stmt);
        return NULL;
    }

    if (terminator != FENG_TOKEN_EOF) {
        FengTokenKind assign_op;

        if (!parser_match_assignment_operator(parser, &assign_op)) {
            return stmt;
        }

        FengStmt *assign = new_stmt(parser, FENG_STMT_ASSIGN, stmt->token);

        if (assign == NULL) {
            free_stmt(stmt);
            return NULL;
        }
        assign->as.assign.op = assign_op;
        assign->as.assign.target = stmt->as.expr;
        assign->as.assign.value = parse_expression(parser);
        free(stmt);
        if (assign->as.assign.value == NULL) {
            free_stmt(assign);
            return NULL;
        }
        return assign;
    }

    return stmt;
}

static FengStmt *parse_statement(Parser *parser) {
    FengStmt *stmt;

    if (parser_check(parser, FENG_TOKEN_LBRACE)) {
        stmt = new_stmt(parser, FENG_STMT_BLOCK, parser_current_token(parser));
        if (stmt == NULL) {
            return NULL;
        }
        stmt->as.block = parse_block(parser);
        if (stmt->as.block == NULL) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    if (parser_match(parser, FENG_TOKEN_KW_IF)) {
        return parse_if_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_MATCH)) {
        return parse_match_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_WHILE)) {
        return parse_while_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_FOR)) {
        return parse_for_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_DEFER)) {
        return parse_defer_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_RETURN)) {
        stmt = new_stmt(parser, FENG_STMT_RETURN, parser_previous_token(parser));
        if (stmt == NULL) {
            return NULL;
        }
        if (!parser_check(parser, FENG_TOKEN_SEMICOLON)) {
            stmt->as.return_value = parse_expression(parser);
            if (stmt->as.return_value == NULL) {
                free_stmt(stmt);
                return NULL;
            }
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "return statements must end with ';'")) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }
    if (parser_match(parser, FENG_TOKEN_KW_THROW)) {
        stmt = new_stmt(parser, FENG_STMT_THROW, parser_previous_token(parser));
        if (stmt == NULL) {
            return NULL;
        }
        stmt->as.throw_value = parse_expression(parser);
        if (stmt->as.throw_value == NULL) {
            free_stmt(stmt);
            return NULL;
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "throw statements must end with ';'")) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }
    if (parser_match(parser, FENG_TOKEN_KW_BREAK)) {
        stmt = new_stmt(parser, FENG_STMT_BREAK, parser_previous_token(parser));
        if (stmt == NULL) {
            return NULL;
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "break statements must end with ';'")) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }
    if (parser_match(parser, FENG_TOKEN_KW_CONTINUE)) {
        stmt = new_stmt(parser, FENG_STMT_CONTINUE, parser_previous_token(parser));
        if (stmt == NULL) {
            return NULL;
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "continue statements must end with ';'")) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    if (parser_match(parser, FENG_TOKEN_KW_TRY)) {
        FengToken try_token = parser_previous_token(parser);
        stmt = new_stmt(parser, FENG_STMT_TRY, try_token);
        if (stmt == NULL) {
            return NULL;
        }
        stmt->as.expr = parse_try_expression(parser, try_token);
        if (stmt->as.expr == NULL) {
            free_stmt(stmt);
            return NULL;
        }
        /* try...catch ends with '}'; allow omitting ';' at end of block. */
        if (parser_current_token(parser).kind == FENG_TOKEN_RBRACE) {
            return stmt;
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "try statements must end with ';'")) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    stmt = parse_simple_statement(parser, FENG_TOKEN_SEMICOLON);
    if (stmt == NULL) {
        return NULL;
    }
    /* Per docs/specifications/feng-flow.md: the trailing ';' on the last expression statement
     * of a block may be omitted (the value is still the block's yield). */
    if (stmt->kind == FENG_STMT_EXPR &&
        parser_current_token(parser).kind == FENG_TOKEN_RBRACE) {
        return stmt;
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_SEMICOLON,
                       "SE0001", "expression statements and local bindings must end with ';'")) {
        free_stmt(stmt);
        return NULL;
    }
    return stmt;
}

static FengProgram *parse_program(Parser *parser) {
    FengProgram *program = (FengProgram *)calloc(1U, sizeof(*program));
    size_t use_capacity = 0U;
    size_t decl_capacity = 0U;

    if (program == NULL) {
        (void)parser_error_current(parser, "IE0001", "out of memory");
        return NULL;
    }
    program->path = parser->path;

    /* Empty or comment-only source files produce a single EOF token.
     * Accept them as valid (empty) programs so that placeholder files
     * do not block compilation. */
    if (parser_is_at_end(parser)) {
        return program;
    }

    program->module_visibility = parse_visibility(parser);
    if (!parser_expect(parser, FENG_TOKEN_KW_MODULE, "SE0901", "source file must begin with module declaration")) {
        feng_program_free(program);
        return NULL;
    }
    program->module_token = parser_current_token(parser);
    if (!parse_path(parser,
                    false,
                    &program->module_segments,
                    &program->module_segment_count,
                    "SE0902", "expected a module path after 'module'")) {
        feng_program_free(program);
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "module declarations must end with ';'")) {
        feng_program_free(program);
        return NULL;
    }

    while (parser_match(parser, FENG_TOKEN_KW_IMPORT)) {
        FengUseDecl use_decl;

        memset(&use_decl, 0, sizeof(use_decl));
        use_decl.token = parser_current_token(parser);
        if (!parse_path(parser,
                false,
                &use_decl.segments,
                &use_decl.segment_count,
                "SE0902", "expected a module path after 'import'")) {
            feng_program_free(program);
            return NULL;
        }
        if (parser_match(parser, FENG_TOKEN_KW_AS)) {
            use_decl.has_alias = true;
            if (!parser_expect_identifier_like(parser,
                                               &use_decl.alias,
                                               false,
                                               "SE0002", "expected an alias name after 'as'")) {
                free(use_decl.segments);
                feng_program_free(program);
                return NULL;
            }
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "SE0001", "import declarations must end with ';'")) {
            free(use_decl.segments);
            feng_program_free(program);
            return NULL;
        }
        if (!APPEND_VALUE(parser, program->uses, program->use_count, use_capacity, use_decl)) {
            free(use_decl.segments);
            feng_program_free(program);
            return NULL;
        }
    }

    while (!parser_is_at_end(parser)) {
        FengDecl *decl = parse_declaration(parser);

        if (decl == NULL) {
            feng_program_free(program);
            return NULL;
        }
        if (!APPEND_VALUE(parser, program->declarations, program->declaration_count, decl_capacity, decl)) {
            free_decl(decl);
            feng_program_free(program);
            return NULL;
        }
    }

    return program;
}

bool feng_parse_source(const char *source,
                       size_t length,
                       const char *path,
                       FengProgram **out_program,
                       FengParseError *out_error) {
    Parser parser;
    FengProgram *program;

    memset(&parser, 0, sizeof(parser));
    parser.source = source;
    parser.length = length;
    parser.path = path;

    if (!parser_tokenize(&parser)) {
        if (out_error != NULL) {
            *out_error = parser.error;
        }
        free(parser.tokens);
        if (out_program != NULL) {
            *out_program = NULL;
        }
        return false;
    }

    program = parse_program(&parser);
    if (program == NULL) {
        if (out_error != NULL) {
            *out_error = parser.error;
        }
        free(parser.tokens);
        if (out_program != NULL) {
            *out_program = NULL;
        }
        return false;
    }

    free(parser.tokens);
    if (out_program != NULL) {
        *out_program = program;
    }
    if (out_error != NULL) {
        memset(out_error, 0, sizeof(*out_error));
    }
    return true;
}

static void free_type_ref(FengTypeRef *type_ref) {
    size_t index;

    if (type_ref == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            free(type_ref->as.named.segments);
            for (index = 0U; index < type_ref->as.named.type_arg_count; ++index) {
                free_type_ref(type_ref->as.named.type_args[index]);
            }
            free(type_ref->as.named.type_args);
            break;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            free_type_ref(type_ref->as.inner);
            break;
    }

    free(type_ref);
}

static void free_parameters(FengParameter *params, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        free_type_ref(params[index].type);
    }
    free(params);
}

static void free_annotations(FengAnnotation *annotations, size_t count) {
    size_t index;

    if (annotations == NULL) {
        return;
    }

    for (index = 0U; index < count; ++index) {
        free_annotation_fields(&annotations[index]);
    }

    free(annotations);
}

static void free_expr(FengExpr *expr) {
    size_t index;

    if (expr == NULL) {
        return;
    }

    switch (expr->kind) {
        case FENG_EXPR_ARRAY_LITERAL:
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                free_expr(expr->as.array_literal.items[index]);
            }
            free(expr->as.array_literal.items);
            break;
        case FENG_EXPR_TUPLE_LITERAL:
            for (index = 0U; index < expr->as.tuple_literal.count; ++index) {
                free_expr(expr->as.tuple_literal.items[index]);
            }
            free(expr->as.tuple_literal.items);
            break;
        case FENG_EXPR_OBJECT_LITERAL:
            free_expr(expr->as.object_literal.target);
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                free_expr(expr->as.object_literal.fields[index].value);
            }
            free(expr->as.object_literal.fields);
            break;
        case FENG_EXPR_GENERIC_TARGET:
            free_expr(expr->as.generic_target.target);
            free_type_arg_refs(expr->as.generic_target.type_args,
                               expr->as.generic_target.type_arg_count);
            break;
        case FENG_EXPR_CALL:
            free_expr(expr->as.call.callee);
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                free_expr(expr->as.call.args[index]);
            }
            free(expr->as.call.args);
            free_type_arg_refs(expr->as.call.explicit_type_args,
                               expr->as.call.explicit_type_arg_count);
            free((void *)expr->as.call.resolved_callable.callable_type_args);
            break;
        case FENG_EXPR_MEMBER:
            free_expr(expr->as.member.object);
            break;
        case FENG_EXPR_INDEX:
            free_expr(expr->as.index.object);
            free_expr(expr->as.index.index);
            break;
        case FENG_EXPR_UNARY:
            free_expr(expr->as.unary.operand);
            break;
        case FENG_EXPR_BINARY:
            free_expr(expr->as.binary.left);
            free_expr(expr->as.binary.right);
            break;
        case FENG_EXPR_LAMBDA:
            free_parameters(expr->as.lambda.params, expr->as.lambda.param_count);
            if (expr->as.lambda.is_block_body) {
                free_block(expr->as.lambda.body_block);
            } else {
                free_expr(expr->as.lambda.body);
            }
            free(expr->as.lambda.captures);
            break;
        case FENG_EXPR_CAST:
            free_type_ref(expr->as.cast.type);
            free_expr(expr->as.cast.value);
            break;
        case FENG_EXPR_ARRAY_NEW:
            free_type_ref(expr->as.array_new.element_type);
            free_expr(expr->as.array_new.size);
            break;
        case FENG_EXPR_IF:
            free_expr(expr->as.if_expr.condition);
            free_block(expr->as.if_expr.then_block);
            free_block(expr->as.if_expr.else_block);
            break;
        case FENG_EXPR_MATCH:
            free_expr(expr->as.match_expr.target);
            for (index = 0U; index < expr->as.match_expr.branch_count; ++index) {
                free_match_branch_contents(&expr->as.match_expr.branches[index]);
            }
            free(expr->as.match_expr.branches);
            free_block(expr->as.match_expr.else_block);
            break;
        case FENG_EXPR_MATCH_OP:
            free_expr(expr->as.match_op.target);
            for (index = 0U; index < expr->as.match_op.label_count; ++index) {
                free_match_label_contents(&expr->as.match_op.labels[index]);
            }
            free(expr->as.match_op.labels);
            break;
        case FENG_EXPR_TRY:
            free_expr(expr->as.try_expr.body);
            free_try_catch_clauses(expr->as.try_expr.clauses,
                                   expr->as.try_expr.clause_count);
            break;
        default:
            break;
    }

    free(expr);
}

static void free_try_catch_clauses(FengTryCatchClause *clauses, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        free_type_ref(clauses[index].type);
        free_block(clauses[index].body);
    }
    free(clauses);
}

static void free_block(FengBlock *block) {
    size_t index;

    if (block == NULL) {
        return;
    }
    for (index = 0U; index < block->statement_count; ++index) {
        free_stmt(block->statements[index]);
    }
    free(block->statements);
    free(block);
}

static void free_stmt(FengStmt *stmt) {
    size_t index;

    if (stmt == NULL) {
        return;
    }

    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            free_block(stmt->as.block);
            break;
        case FENG_STMT_BINDING:
            free_type_ref(stmt->as.binding.type);
            free_expr(stmt->as.binding.initializer);
            free(stmt->as.binding.destructure_names);
            break;
        case FENG_STMT_ASSIGN:
            free_expr(stmt->as.assign.target);
            free_expr(stmt->as.assign.value);
            break;
        case FENG_STMT_EXPR:
            free_expr(stmt->as.expr);
            break;
        case FENG_STMT_TRY:
            free_expr(stmt->as.expr);
            break;
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                free_expr(stmt->as.if_stmt.clauses[index].condition);
                free_block(stmt->as.if_stmt.clauses[index].block);
            }
            free(stmt->as.if_stmt.clauses);
            free_block(stmt->as.if_stmt.else_block);
            break;
        case FENG_STMT_MATCH:
            free_expr(stmt->as.match_stmt.target);
            for (index = 0U; index < stmt->as.match_stmt.branch_count; ++index) {
                free_match_branch_contents(&stmt->as.match_stmt.branches[index]);
            }
            free(stmt->as.match_stmt.branches);
            free_block(stmt->as.match_stmt.else_block);
            break;
        case FENG_STMT_WHILE:
            free_expr(stmt->as.while_stmt.condition);
            free_block(stmt->as.while_stmt.body);
            break;
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                free_type_ref(stmt->as.for_stmt.iter_binding.type);
                free_expr(stmt->as.for_stmt.iter_expr);
            } else {
                free_stmt(stmt->as.for_stmt.init);
                free_expr(stmt->as.for_stmt.condition);
                free_stmt(stmt->as.for_stmt.update);
            }
            free_block(stmt->as.for_stmt.body);
            break;
        case FENG_STMT_RETURN:
            free_expr(stmt->as.return_value);
            break;
        case FENG_STMT_THROW:
            free_expr(stmt->as.throw_value);
            break;
        case FENG_STMT_BREAK:
        case FENG_STMT_CONTINUE:
            break;
        case FENG_STMT_DEFER:
            free_block(stmt->as.defer_block);
            break;
    }

    free(stmt);
}

static void free_bound_member_names(FengSlice *names, size_t count) {
    size_t index;

    if (names == NULL) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        free((void *)names[index].data);
    }
    free(names);
}

static void free_type_member(FengTypeMember *member) {
    if (member == NULL) {
        return;
    }

    free_annotations(member->annotations, member->annotation_count);
    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        free_type_ref(member->as.field.type);
        free_expr(member->as.field.initializer);
    } else {
        free_type_params(member->as.callable.type_params, member->as.callable.type_param_count);
        free_parameters(member->as.callable.params, member->as.callable.param_count);
        free_type_ref(member->as.callable.return_type);
        free_block(member->as.callable.body);
        free_bound_member_names(member->as.callable.bound_member_names,
                                member->as.callable.bound_member_count);
    }

    free(member);
}

static void free_enum_items(FengEnumItem *items, size_t count) {
    (void)count;
    free(items);
}

static void free_decl(FengDecl *decl) {
    size_t index;

    if (decl == NULL) {
        return;
    }

    free_annotations(decl->annotations, decl->annotation_count);
    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            free_type_ref(decl->as.binding.type);
            free_expr(decl->as.binding.initializer);
            free(decl->as.binding.destructure_names);
            break;
        case FENG_DECL_TYPE:
            free_type_params(decl->as.type_decl.type_params,
                             decl->as.type_decl.type_param_count);
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                free_type_member(decl->as.type_decl.members[index]);
            }
            free(decl->as.type_decl.members);
            for (index = 0U; index < decl->as.type_decl.mixin_count; ++index) {
                free_type_ref(decl->as.type_decl.mixins[index].source_type);
                free_expr(decl->as.type_decl.mixins[index].source_constructor);
            }
            free(decl->as.type_decl.mixins);
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                free_type_ref(decl->as.type_decl.declared_specs[index]);
            }
            free(decl->as.type_decl.declared_specs);
            break;
        case FENG_DECL_ENUM:
            free_enum_items(decl->as.enum_decl.items, decl->as.enum_decl.item_count);
            break;
        case FENG_DECL_SPEC:
            free_type_params(decl->as.spec_decl.type_params,
                             decl->as.spec_decl.type_param_count);
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                free_type_ref(decl->as.spec_decl.parent_specs[index]);
            }
            free(decl->as.spec_decl.parent_specs);
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    free_type_member(decl->as.spec_decl.as.object.members[index]);
                }
                free(decl->as.spec_decl.as.object.members);
            } else if (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
                free_parameters(decl->as.spec_decl.as.callable.params,
                                decl->as.spec_decl.as.callable.param_count);
                free_type_ref(decl->as.spec_decl.as.callable.return_type);
            } else if (decl->as.spec_decl.form == FENG_SPEC_FORM_UNION) {
                for (index = 0U; index < decl->as.spec_decl.as.union_form.member_count; ++index) {
                    free_type_ref(decl->as.spec_decl.as.union_form.members[index]);
                }
                free(decl->as.spec_decl.as.union_form.members);
            } else if (decl->as.spec_decl.form == FENG_SPEC_FORM_INTERSECTION) {
                for (index = 0U; index < decl->as.spec_decl.as.intersection_form.member_count; ++index) {
                    free_type_ref(decl->as.spec_decl.as.intersection_form.members[index]);
                }
                free(decl->as.spec_decl.as.intersection_form.members);
            }
            break;
        case FENG_DECL_FIT:
            free_type_ref(decl->as.fit_decl.target);
            for (index = 0U; index < decl->as.fit_decl.spec_count; ++index) {
                free_type_ref(decl->as.fit_decl.specs[index]);
            }
            free(decl->as.fit_decl.specs);
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                free_type_member(decl->as.fit_decl.members[index]);
            }
            free(decl->as.fit_decl.members);
            break;
        case FENG_DECL_FUNCTION:
            free_type_params(decl->as.function_decl.type_params,
                             decl->as.function_decl.type_param_count);
            free_parameters(decl->as.function_decl.params, decl->as.function_decl.param_count);
            free_type_ref(decl->as.function_decl.return_type);
            free_block(decl->as.function_decl.body);
            break;
    }

    free(decl);
}

void feng_program_free(FengProgram *program) {
    size_t index;

    if (program == NULL) {
        return;
    }

    free(program->module_segments);
    for (index = 0U; index < program->use_count; ++index) {
        free(program->uses[index].segments);
    }
    free(program->uses);
    for (index = 0U; index < program->declaration_count; ++index) {
        free_decl(program->declarations[index]);
    }
    free(program->declarations);
    free(program);
}
