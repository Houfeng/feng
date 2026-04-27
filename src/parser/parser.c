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
} Parser;

#define APPEND_VALUE(parser, items, count, capacity, value) \
    append_raw((parser), (void **)&(items), &(count), &(capacity), sizeof(*(items)), &(value))

static FengProgram *parse_program(Parser *parser);
static FengDecl *parse_declaration(Parser *parser);
static FengBlock *parse_block(Parser *parser);
static FengStmt *parse_statement(Parser *parser);
static FengStmt *parse_simple_statement(Parser *parser, FengTokenKind terminator);
static FengExpr *parse_expression(Parser *parser);
static FengExpr *parse_unary(Parser *parser);
static FengTypeRef *parse_type_ref(Parser *parser);
static void free_type_ref(FengTypeRef *type_ref);
static void free_expr(FengExpr *expr);
static void free_stmt(FengStmt *stmt);
static void free_block(FengBlock *block);
static void free_decl(FengDecl *decl);
static void free_annotations(FengAnnotation *annotations, size_t count);
static void free_parameters(FengParameter *params, size_t count);
static void free_type_member(FengTypeMember *member);

static void free_annotation_fields(FengAnnotation *annotation) {
    size_t arg_index;

    if (annotation == NULL) {
        return;
    }
    for (arg_index = 0U; arg_index < annotation->arg_count; ++arg_index) {
        free_expr(annotation->args[arg_index]);
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

static bool parser_error_at(Parser *parser, const FengToken *token, const char *message) {
    if (parser->error.message == NULL) {
        parser->error.message = message;
        parser->error.token = *token;
    }
    return false;
}

static bool parser_error_current(Parser *parser, const char *message) {
    return parser_error_at(parser, parser_current(parser), message);
}

static bool parser_expect(Parser *parser, FengTokenKind kind, const char *message) {
    if (parser_check(parser, kind)) {
        (void)parser_advance(parser);
        return true;
    }
    return parser_error_current(parser, message);
}

static bool token_is_identifier_like(const FengToken *token, bool allow_void) {
    return token->kind == FENG_TOKEN_IDENTIFIER || (allow_void && token->kind == FENG_TOKEN_KW_VOID);
}

static bool parser_expect_identifier_like(Parser *parser,
                                          FengSlice *out_name,
                                          bool allow_void,
                                          const char *message) {
    if (!token_is_identifier_like(parser_current(parser), allow_void)) {
        return parser_error_current(parser, message);
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
            parser->error.message = token.message;
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
    if (parser_match(parser, FENG_TOKEN_KW_PU)) {
        return FENG_VISIBILITY_PUBLIC;
    }
    if (parser_match(parser, FENG_TOKEN_KW_PR)) {
        return FENG_VISIBILITY_PRIVATE;
    }
    return FENG_VISIBILITY_DEFAULT;
}

static bool parse_path(Parser *parser,
                       bool allow_void,
                       FengSlice **out_segments,
                       size_t *out_count,
                       const char *message) {
    FengSlice *segments = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    FengSlice segment;

    if (!parser_expect_identifier_like(parser, &segment, allow_void, message)) {
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
        annotation.args = NULL;
        annotation.arg_count = 0U;
        (void)parser_advance(parser);

        if (parser_match(parser, FENG_TOKEN_LPAREN)) {
            size_t arg_capacity = 0U;

            if (!parser_check(parser, FENG_TOKEN_RPAREN)) {
                do {
                    FengExpr *arg = parse_expression(parser);

                    if (arg == NULL) {
                        free_annotations(annotations, count);
                        free(arg);
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
                } while (parser_match(parser, FENG_TOKEN_COMMA));
            }

            if (!parser_expect(parser,
                               FENG_TOKEN_RPAREN,
                               "expected ')' to close annotation argument list")) {
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
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }

    type_ref->token = token;
    type_ref->kind = kind;
    return type_ref;
}

static FengTypeRef *parse_type_ref(Parser *parser) {
    FengTypeRef *type_ref;
    FengToken start_token = parser_current_token(parser);
    size_t pointer_count = 0U;

    while (parser_match(parser, FENG_TOKEN_STAR)) {
        ++pointer_count;
    }

    type_ref = new_type_ref(parser, FENG_TYPE_REF_NAMED, start_token);
    if (type_ref == NULL) {
        return NULL;
    }

    if (!parse_path(parser,
                    true,
                    &type_ref->as.named.segments,
                    &type_ref->as.named.segment_count,
                    "expected a type name")) {
        free_type_ref(type_ref);
        return NULL;
    }

    while (pointer_count > 0U) {
        FengTypeRef *wrapper = new_type_ref(parser, FENG_TYPE_REF_POINTER, start_token);

        if (wrapper == NULL) {
            free_type_ref(type_ref);
            return NULL;
        }
        wrapper->as.inner = type_ref;
        type_ref = wrapper;
        --pointer_count;
    }

    while (parser_match(parser, FENG_TOKEN_LBRACKET)) {
        FengTypeRef *wrapper;

        if (!parser_expect(parser,
                   FENG_TOKEN_RBRACKET,
                   "expected ']' to close array type suffix")) {
            free_type_ref(type_ref);
            return NULL;
        }

        wrapper = new_type_ref(parser, FENG_TYPE_REF_ARRAY, start_token);
        if (wrapper == NULL) {
            free_type_ref(type_ref);
            return NULL;
        }
        wrapper->as.inner = type_ref;
        type_ref = wrapper;
    }

    return type_ref;
}

static bool parse_parameters(Parser *parser, FengParameter **out_params, size_t *out_count) {
    FengParameter *params = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    if (!parser_expect(parser, FENG_TOKEN_LPAREN, "expected '(' to start parameter list")) {
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
                                               "expected a parameter name")) {
                free_parameters(params, count);
                return false;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "expected ':' after parameter name in parameter list")) {
                free_parameters(params, count);
                return false;
            }

            param.type = parse_type_ref(parser);
            if (param.type == NULL) {
                free_parameters(params, count);
                return false;
            }

            if (!APPEND_VALUE(parser, params, count, capacity, param)) {
                free_type_ref(param.type);
                free_parameters(params, count);
                return false;
            }
        } while (parser_match(parser, FENG_TOKEN_COMMA));
    }

    if (!parser_expect(parser, FENG_TOKEN_RPAREN, "expected ')' to close parameter list")) {
        free_parameters(params, count);
        return false;
    }

    *out_params = params;
    *out_count = count;
    return true;
}

static FengBinding parse_binding_core(Parser *parser, FengMutability mutability, bool require_type) {
    FengBinding binding;

    binding.token = parser_current_token(parser);
    binding.mutability = mutability;
    binding.name.data = NULL;
    binding.name.length = 0U;
    binding.type = NULL;
    binding.initializer = NULL;

    if (!parser_expect_identifier_like(parser, &binding.name, false, "expected a binding name")) {
        return binding;
    }

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        binding.type = parse_type_ref(parser);
        if (binding.type == NULL) {
            return binding;
        }
    } else if (require_type) {
        (void)parser_error_current(parser, "type field declarations require ':' after the field name");
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
                       "binding declarations require a type annotation or an initializer");
    }

    return binding;
}

static FengCallableSignature parse_callable_signature(Parser *parser,
                                                     FengToken token,
                                                     FengSlice name,
                                                     bool require_body,
                                                     const char *body_rule_message) {
    FengCallableSignature callable;

    callable.token = token;
    callable.name = name;
    callable.params = NULL;
    callable.param_count = 0U;
    callable.return_type = NULL;
    callable.body = NULL;

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
            (void)parser_error_current(parser, body_rule_message);
            return callable;
        }
        callable.body = parse_block(parser);
    } else {
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           body_rule_message)) {
            return callable;
        }
    }

    return callable;
}

static FengDecl *new_decl(Parser *parser, FengDeclKind kind, FengToken token) {
    FengDecl *decl = (FengDecl *)calloc(1U, sizeof(*decl));

    if (decl == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }

    decl->token = token;
    decl->kind = kind;
    return decl;
}

static FengTypeMember *new_type_member(Parser *parser, FengTypeMemberKind kind, FengToken token) {
    FengTypeMember *member = (FengTypeMember *)calloc(1U, sizeof(*member));

    if (member == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }

    member->token = token;
    member->kind = kind;
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

static FengDecl *parse_type_declaration(Parser *parser,
                                        FengVisibility visibility,
                                        bool is_extern,
                                        FengAnnotation *annotations,
                                        size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl = new_decl(parser, FENG_DECL_TYPE, name_token);
    FengSlice type_name;

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->visibility = visibility;
    decl->is_extern = is_extern;
    decl->annotations = annotations;
    decl->annotation_count = annotation_count;

    if (!parser_expect_identifier_like(parser, &type_name, false, "expected a type name")) {
        free_decl(decl);
        return NULL;
    }
    decl->as.type_decl.name = type_name;

    if (parser_check(parser, FENG_TOKEN_LPAREN)) {
        (void)parser_error_current(
            parser,
            "callable contracts must use 'spec Name(args): ReturnType;'; 'type' no longer defines callable shapes");
        free_decl(decl);
        return NULL;
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
                       "type declarations require '{...}' after the optional spec list")) {
        free_decl(decl);
        return NULL;
    }

    {
        size_t member_capacity = 0U;

    while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
        FengAnnotation *member_annotations;
        size_t member_annotation_count = 0U;
        FengVisibility member_visibility;
        FengTypeMember *member = NULL;

        member_annotations = parse_annotations(parser, &member_annotation_count);
        if (parser->error.message != NULL) {
            free_decl(decl);
            return NULL;
        }

        member_visibility = parse_visibility(parser);

        if (member_annotation_count > 0U && parser_check(parser, FENG_TOKEN_SEMICOLON)) {
            free_annotations(member_annotations, member_annotation_count);
            (void)parser_error_current(
                parser,
                "type member annotations must be followed immediately by a field or method; remove the trailing ';'");
            free_decl(decl);
            return NULL;
        }

        if (parser_match(parser, FENG_TOKEN_KW_LET) || parser_match(parser, FENG_TOKEN_KW_VAR)) {
            FengMutability mutability = (parser_previous(parser)->kind == FENG_TOKEN_KW_LET)
                                            ? FENG_MUTABILITY_LET
                                            : FENG_MUTABILITY_VAR;
            FengBinding binding = parse_binding_core(parser, mutability, true);

            if (parser->error.message != NULL) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_SEMICOLON,
                               "type field declarations must end with ';'")) {
                free_type_ref(binding.type);
                free_expr(binding.initializer);
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            member = new_type_member(parser, FENG_TYPE_MEMBER_FIELD, binding.token);
            if (member == NULL) {
                free_type_ref(binding.type);
                free_expr(binding.initializer);
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            member->visibility = member_visibility;
            member->annotations = member_annotations;
            member->annotation_count = member_annotation_count;
            member->as.field.mutability = binding.mutability;
            member->as.field.name = binding.name;
            member->as.field.type = binding.type;
            member->as.field.initializer = binding.initializer;
        } else if (parser_match(parser, FENG_TOKEN_KW_FN)) {
            FengCallableSignature callable;
            FengSlice name;
            FengToken member_name_token = parser_current_token(parser);
            FengTypeMemberKind member_kind = FENG_TYPE_MEMBER_METHOD;
            bool is_finalizer = false;

            if (is_extern) {
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_current(
                    parser,
                    "extern type object form only supports fields; methods require a non-extern type");
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
                                               is_finalizer
                                                   ? "expected the type name after 'fn ~' to declare a finalizer"
                                                   : "expected a method or constructor name after 'fn'")) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            if (is_finalizer && !slice_equals(name, type_name)) {
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_current(
                    parser,
                    "finalizer name must match the enclosing type name");
                free_decl(decl);
                return NULL;
            }

            callable = parse_callable_signature(
                parser,
                member_name_token,
                name,
                true,
                is_finalizer
                    ? "type finalizers must provide a body '{...}'"
                    : "type methods and constructors must provide a body '{...}'");
            if (parser->error.message != NULL) {
                free_annotations(member_annotations, member_annotation_count);
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
                                          "finalizer must not declare any parameters");
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
                                          "finalizer return type must be omitted or ': void'");
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
                        "constructor must not declare a non-void return type");
                    free_decl(decl);
                    return NULL;
                }
            }

            member = new_type_member(parser, member_kind, callable.token);
            if (member == NULL) {
                free_parameters(callable.params, callable.param_count);
                free_type_ref(callable.return_type);
                free_block(callable.body);
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }
            member->visibility = member_visibility;
            member->annotations = member_annotations;
            member->annotation_count = member_annotation_count;
            member->as.callable = callable;
        } else {
            free_annotations(member_annotations, member_annotation_count);
            if (parser_starts_callable_signature(parser)) {
                (void)parser_error_current(parser,
                                           "type methods and constructors must start with 'fn'");
            } else if (parser_starts_binding_without_keyword(parser)) {
                (void)parser_error_current(parser,
                                           "type fields must start with 'let' or 'var'");
            } else if (parser_check(parser, FENG_TOKEN_KW_EXTERN)) {
                (void)parser_error_current(
                    parser,
                    "type members cannot use 'extern fn'; use 'fn' for methods or 'let'/'var' for fields");
            } else {
                (void)parser_error_current(parser,
                                           "expected type member declaration: 'let', 'var', or 'fn'");
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

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close type body")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengTypeMember *parse_spec_member(Parser *parser, FengSlice spec_name) {
    FengToken member_start = parser_current_token(parser);
    FengTypeMember *member = NULL;

    if (parser_check(parser, FENG_TOKEN_KW_PU) || parser_check(parser, FENG_TOKEN_KW_PR)) {
        (void)parser_error_current(
            parser,
            "spec members cannot declare visibility; remove 'pu' or 'pr'");
        return NULL;
    }

    if (parser_match(parser, FENG_TOKEN_KW_LET) || parser_match(parser, FENG_TOKEN_KW_VAR)) {
        FengMutability mutability = (parser_previous(parser)->kind == FENG_TOKEN_KW_LET)
                                        ? FENG_MUTABILITY_LET
                                        : FENG_MUTABILITY_VAR;
        FengBinding binding = parse_binding_core(parser, mutability, true);

        if (parser->error.message != NULL) {
            return NULL;
        }
        if (binding.initializer != NULL) {
            (void)parser_error_current(parser, "spec field declarations cannot have an initializer");
            free_type_ref(binding.type);
            free_expr(binding.initializer);
            return NULL;
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           "spec field declarations must end with ';'")) {
            free_type_ref(binding.type);
            return NULL;
        }
        member = new_type_member(parser, FENG_TYPE_MEMBER_FIELD, binding.token);
        if (member == NULL) {
            free_type_ref(binding.type);
            return NULL;
        }
        member->visibility = FENG_VISIBILITY_DEFAULT;
        member->as.field.mutability = binding.mutability;
        member->as.field.name = binding.name;
        member->as.field.type = binding.type;
        member->as.field.initializer = NULL;
        return member;
    }

    if (parser_match(parser, FENG_TOKEN_KW_FN)) {
        FengCallableSignature callable;
        FengSlice name;
        FengToken member_name_token = parser_current_token(parser);
        size_t param_index;

        if (!parser_expect_identifier_like(parser,
                                           &name,
                                           false,
                                           "expected a method name after 'fn'")) {
            return NULL;
        }
        if (slice_equals(name, spec_name)) {
            (void)parser_error_current(parser, "spec cannot declare a constructor");
            return NULL;
        }
        callable = parse_callable_signature(
            parser,
            member_name_token,
            name,
            false,
            "spec method signatures must end with ';' and cannot have a body '{...}'");
        if (parser->error.message != NULL) {
            return NULL;
        }
        for (param_index = 0U; param_index < callable.param_count; ++param_index) {
            if (callable.params[param_index].mutability != FENG_MUTABILITY_DEFAULT) {
                (void)parser_error_current(
                    parser,
                    "spec method parameters cannot use 'let' or 'var' modifiers");
                free_parameters(callable.params, callable.param_count);
                free_type_ref(callable.return_type);
                free_block(callable.body);
                return NULL;
            }
        }
        if (callable.return_type == NULL) {
            (void)parser_error_current(parser, "spec method signatures must declare a return type");
            free_parameters(callable.params, callable.param_count);
            free_block(callable.body);
            return NULL;
        }
        member = new_type_member(parser, FENG_TYPE_MEMBER_METHOD, callable.token);
        if (member == NULL) {
            free_parameters(callable.params, callable.param_count);
            free_type_ref(callable.return_type);
            free_block(callable.body);
            return NULL;
        }
        member->visibility = FENG_VISIBILITY_DEFAULT;
        member->as.callable = callable;
        return member;
    }

    (void)member_start;
    (void)parser_error_current(parser, "expected spec member declaration: 'let', 'var', or 'fn'");
    return NULL;
}

static FengDecl *parse_spec_declaration(Parser *parser,
                                        FengVisibility visibility,
                                        bool is_extern,
                                        FengAnnotation *annotations,
                                        size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl;
    FengSlice spec_name;

    if (is_extern) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "'extern' cannot be applied to a 'spec' declaration");
        return NULL;
    }

    decl = new_decl(parser, FENG_DECL_SPEC, name_token);
    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }
    decl->visibility = visibility;
    decl->is_extern = false;
    decl->annotations = annotations;
    decl->annotation_count = annotation_count;

    if (!parser_expect_identifier_like(parser, &spec_name, false, "expected a spec name after 'spec'")) {
        free_decl(decl);
        return NULL;
    }
    decl->as.spec_decl.name = spec_name;

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
                    "spec callable parameters cannot use 'let' or 'var' modifiers");
                free_decl(decl);
                return NULL;
            }
        }
        if (!parser_expect(parser,
                           FENG_TOKEN_COLON,
                           "spec callable declarations require ':' before the return type")) {
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
                           "spec callable declarations must end with ';'")) {
            free_decl(decl);
            return NULL;
        }
        return decl;
    }

    decl->as.spec_decl.form = FENG_SPEC_FORM_OBJECT;

    if (parser_match(parser, FENG_TOKEN_COLON)) {
        if (!parse_spec_satisfaction_list(parser,
                                &decl->as.spec_decl.parent_specs,
                                &decl->as.spec_decl.parent_spec_count)) {
            free_decl(decl);
            return NULL;
        }
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "spec object declarations require '{...}' after the optional spec list")) {
        free_decl(decl);
        return NULL;
    }

    {
        size_t member_capacity = 0U;

        while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
            FengTypeMember *member = parse_spec_member(parser, spec_name);

            if (member == NULL) {
                free_decl(decl);
                return NULL;
            }
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

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close spec body")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengTypeMember *parse_fit_method_member(Parser *parser) {
    FengVisibility visibility = parse_visibility(parser);
    FengToken member_start = parser_current_token(parser);
    FengCallableSignature callable;
    FengSlice name;
    FengTypeMember *member;

    if (parser_check(parser, FENG_TOKEN_KW_LET) || parser_check(parser, FENG_TOKEN_KW_VAR)) {
        (void)parser_error_current(
            parser,
            "fit blocks cannot declare 'let' or 'var' fields; declare them on the original type");
        return NULL;
    }

    if (!parser_match(parser, FENG_TOKEN_KW_FN)) {
        (void)parser_error_current(parser, "fit block members must start with 'fn'");
        return NULL;
    }

    if (!parser_expect_identifier_like(parser, &name, false, "expected a method name after 'fn'")) {
        return NULL;
    }
    callable = parse_callable_signature(
        parser,
        member_start,
        name,
        true,
        "fit block methods must provide a body '{...}'");
    if (parser->error.message != NULL) {
        return NULL;
    }
    member = new_type_member(parser, FENG_TYPE_MEMBER_METHOD, callable.token);
    if (member == NULL) {
        free_parameters(callable.params, callable.param_count);
        free_type_ref(callable.return_type);
        free_block(callable.body);
        return NULL;
    }
    member->visibility = visibility;
    member->as.callable = callable;
    return member;
}

static FengDecl *parse_fit_declaration(Parser *parser,
                                       FengVisibility visibility,
                                       bool is_extern,
                                       FengAnnotation *annotations,
                                       size_t annotation_count) {
    FengToken start = parser_current_token(parser);
    FengDecl *decl;

    if (is_extern) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "'extern' cannot be applied to a 'fit' declaration");
        return NULL;
    }
    if (annotation_count > 0U) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "annotations cannot be applied to 'fit' declarations");
        return NULL;
    }
    if (visibility == FENG_VISIBILITY_PRIVATE) {
        (void)parser_error_current(parser, "fit declarations cannot use 'pr'");
        return NULL;
    }

    decl = new_decl(parser, FENG_DECL_FIT, start);
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
        if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close fit body")) {
            free_decl(decl);
            return NULL;
        }
    } else {
        decl->as.fit_decl.has_body = false;
        if (!parser_expect(parser,
                           FENG_TOKEN_SEMICOLON,
                           "fit declarations without a body must end with ';'")) {
            free_decl(decl);
            return NULL;
        }
    }

    if (decl->as.fit_decl.spec_count == 0U && !decl->as.fit_decl.has_body) {
        (void)parser_error_current(parser, "fit declarations must include a spec list, a body block, or both");
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_function_declaration(Parser *parser,
                                            FengVisibility visibility,
                                            bool is_extern,
                                            FengAnnotation *annotations,
                                            size_t annotation_count) {
    FengToken name_token = parser_current_token(parser);
    FengDecl *decl = new_decl(parser, FENG_DECL_FUNCTION, name_token);
    FengSlice name;

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->visibility = visibility;
    decl->is_extern = is_extern;
    decl->annotations = annotations;
    decl->annotation_count = annotation_count;

    if (!parser_expect_identifier_like(parser, &name, false, "expected a function name after 'fn'")) {
        free_decl(decl);
        return NULL;
    }

    decl->as.function_decl = parse_callable_signature(
        parser,
        name_token,
        name,
        !is_extern,
        is_extern ? "extern function declarations must end with ';' and cannot have a body '{...}'"
                  : "function declarations must provide a body '{...}'");
    if (parser->error.message != NULL) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_global_binding(Parser *parser,
                                      FengVisibility visibility,
                                      FengMutability mutability,
                                      FengAnnotation *annotations,
                                      size_t annotation_count) {
    FengDecl *decl = new_decl(parser,
                              FENG_DECL_GLOBAL_BINDING,
                              parser_current_token(parser));

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->annotations = annotations;
    decl->annotation_count = annotation_count;
    decl->visibility = visibility;
    decl->as.binding = parse_binding_core(parser, mutability, false);
    if (parser->error.message != NULL) {
        free_decl(decl);
        return NULL;
    }

    if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "top-level bindings must end with ';'")) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_declaration(Parser *parser) {
    FengAnnotation *annotations;
    size_t annotation_count = 0U;
    FengVisibility visibility;
    bool is_extern = false;

    annotations = parse_annotations(parser, &annotation_count);
    if (parser->error.message != NULL) {
        return NULL;
    }

    visibility = parse_visibility(parser);

    if (parser_match(parser, FENG_TOKEN_KW_LET)) {
        return parse_global_binding(
            parser, visibility, FENG_MUTABILITY_LET, annotations, annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_VAR)) {
        return parse_global_binding(
            parser, visibility, FENG_MUTABILITY_VAR, annotations, annotation_count);
    }

    if (parser_match(parser, FENG_TOKEN_KW_EXTERN)) {
        is_extern = true;
    }

    if (annotation_count > 0U && parser_check(parser, FENG_TOKEN_SEMICOLON)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(
            parser,
            "annotation must be followed immediately by a declaration; remove the trailing ';'");
        return NULL;
    }

    if (is_extern && parser_starts_callable_signature(parser)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "extern function declarations must start with 'extern fn'");
        return NULL;
    }

    if (parser_starts_callable_signature(parser)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "top-level function declarations must start with 'fn'");
        return NULL;
    }

    if (parser_starts_binding_without_keyword(parser)) {
        free_annotations(annotations, annotation_count);
        (void)parser_error_current(parser, "top-level bindings must start with 'let' or 'var'");
        return NULL;
    }

    if (parser_match(parser, FENG_TOKEN_KW_TYPE)) {
        return parse_type_declaration(parser, visibility, is_extern, annotations, annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_SPEC)) {
        return parse_spec_declaration(parser, visibility, is_extern, annotations, annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_FIT)) {
        return parse_fit_declaration(parser, visibility, is_extern, annotations, annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_FN)) {
        return parse_function_declaration(parser, visibility, is_extern, annotations, annotation_count);
    }

    free_annotations(annotations, annotation_count);
    (void)parser_error_current(parser,
                               "expected top-level declaration: 'let', 'var', 'extern fn', 'type', 'spec', 'fit', or 'fn'");
    return NULL;
}

static FengExpr *new_expr(Parser *parser, FengExprKind kind, FengToken token) {
    FengExpr *expr = (FengExpr *)calloc(1U, sizeof(*expr));

    if (expr == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }
    expr->token = token;
    expr->kind = kind;
    return expr;
}

static FengStmt *new_stmt(Parser *parser, FengStmtKind kind, FengToken token) {
    FengStmt *stmt = (FengStmt *)calloc(1U, sizeof(*stmt));

    if (stmt == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }
    stmt->token = token;
    stmt->kind = kind;
    return stmt;
}

static FengBlock *new_block(Parser *parser, FengToken token) {
    FengBlock *block = (FengBlock *)calloc(1U, sizeof(*block));

    if (block == NULL) {
        (void)parser_error_current(parser, "out of memory");
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
        case FENG_TOKEN_LBRACKET:
        case FENG_TOKEN_MINUS:
        case FENG_TOKEN_NOT:
        case FENG_TOKEN_KW_IF:
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

static bool looks_like_type_ref_at(const Parser *parser, size_t *index) {
    size_t cursor = *index;

    while (parser->tokens[cursor].kind == FENG_TOKEN_STAR) {
        ++cursor;
    }

    if (!token_is_identifier_like(&parser->tokens[cursor], true)) {
        return false;
    }
    ++cursor;

    while (parser->tokens[cursor].kind == FENG_TOKEN_DOT) {
        ++cursor;
        if (!token_is_identifier_like(&parser->tokens[cursor], false)) {
            return false;
        }
        ++cursor;
    }

    while (parser->tokens[cursor].kind == FENG_TOKEN_LBRACKET) {
        ++cursor;
        if (parser->tokens[cursor].kind != FENG_TOKEN_RBRACKET) {
            return false;
        }
        ++cursor;
    }

    *index = cursor;
    return true;
}

static bool looks_like_cast(const Parser *parser) {
    size_t index;

    if (!parser_check(parser, FENG_TOKEN_LPAREN)) {
        return false;
    }

    index = parser->current + 1U;
    if (!looks_like_type_ref_at(parser, &index)) {
        return false;
    }
    if (parser->tokens[index].kind != FENG_TOKEN_RPAREN) {
        return false;
    }
    return token_starts_expression(parser->tokens[index + 1U].kind);
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

static FengExpr *parse_primary(Parser *parser);

static FengExpr *parse_object_literal_suffix(Parser *parser, FengExpr *target) {
    FengExpr *expr = new_expr(parser, FENG_EXPR_OBJECT_LITERAL, parser_current_token(parser));
    size_t field_capacity = 0U;

    if (expr == NULL) {
        free_expr(target);
        return NULL;
    }

    expr->as.object_literal.target = target;

    if (!parser_expect(parser, FENG_TOKEN_LBRACE, "expected '{' to start object literal")) {
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
                                               "expected an object literal field name")) {
                free_expr(expr);
                return NULL;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "expected ':' after object literal field name")) {
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

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close object literal")) {
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

    if (!parser_expect(parser, FENG_TOKEN_LBRACKET, "expected '[' to start array literal")) {
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

    if (!parser_expect(parser, FENG_TOKEN_RBRACKET, "expected ']' to close array literal")) {
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
                       "lambda expressions must use '->' before a single-expression body or '{' for a block body")) {
        free_expr(expr);
        return NULL;
    }

    if (parser_check(parser, FENG_TOKEN_LBRACE)) {
        (void)parser_error_current(
            parser,
            "multi-line lambda body must omit '->' and use the block form '(params) { ... }'");
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

/* ---------------- if / match shared helpers ---------------- */

static bool is_match_label_atom_token(FengTokenKind kind) {
    return kind == FENG_TOKEN_INTEGER || kind == FENG_TOKEN_STRING ||
           kind == FENG_TOKEN_BOOL || kind == FENG_TOKEN_IDENTIFIER;
}

/* Parser cursor must be positioned at the first token after the consumed '{'.
 * Returns true when the body looks like a match branch list (label/else
 * followed by '{', ',' or '...'); returns false to indicate a plain block
 * body for the conditional `if` form. */
static bool peek_match_body(Parser *parser) {
    size_t base = parser->current;
    size_t i = base;
    const FengToken *t = &parser->tokens[i];

    if (t->kind == FENG_TOKEN_KW_ELSE) {
        return true;
    }
    if (t->kind == FENG_TOKEN_MINUS) {
        ++i;
        t = &parser->tokens[i];
    }
    if (!is_match_label_atom_token(t->kind)) {
        return false;
    }
    ++i;
    t = &parser->tokens[i];
    if (t->kind == FENG_TOKEN_ELLIPSIS) {
        ++i;
        t = &parser->tokens[i];
        if (t->kind == FENG_TOKEN_MINUS) {
            ++i;
            t = &parser->tokens[i];
        }
        if (t->kind != FENG_TOKEN_INTEGER) {
            return false;
        }
        ++i;
        t = &parser->tokens[i];
    }
    return t->kind == FENG_TOKEN_COMMA || t->kind == FENG_TOKEN_LBRACE;
}

/* Parse a single label atom (literal or identifier, optionally negated). */
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
                "match label must be an integer, string, bool literal or named constant");
            return NULL;
    }
}

static bool parse_match_label(Parser *parser, FengMatchLabel *out_label) {
    FengToken token = *parser_current(parser);
    FengExpr *first;

    out_label->token = token;
    out_label->kind = FENG_MATCH_LABEL_VALUE;
    out_label->value = NULL;
    out_label->range_low = NULL;
    out_label->range_high = NULL;

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

static void free_match_branch_contents(FengMatchBranch *branch) {
    size_t i;

    if (branch == NULL) {
        return;
    }
    for (i = 0U; i < branch->label_count; ++i) {
        free_expr(branch->labels[i].value);
        free_expr(branch->labels[i].range_low);
        free_expr(branch->labels[i].range_high);
    }
    free(branch->labels);
    free_block(branch->body);
}

static bool parse_match_branch(Parser *parser, FengMatchBranch *out_branch) {
    size_t label_capacity = 0U;

    out_branch->token = *parser_current(parser);
    out_branch->labels = NULL;
    out_branch->label_count = 0U;
    out_branch->body = NULL;

    for (;;) {
        FengMatchLabel label;

        if (!parse_match_label(parser, &label)) {
            free_match_branch_contents(out_branch);
            return false;
        }
        if (!APPEND_VALUE(parser, out_branch->labels, out_branch->label_count, label_capacity, label)) {
            free_expr(label.value);
            free_expr(label.range_low);
            free_expr(label.range_high);
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
                    "match expression cannot declare more than one 'else' branch");
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

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close match body")) {
        return false;
    }
    return true;
}

static FengExpr *parse_if_expression(Parser *parser, FengToken if_token) {
    FengExpr *condition = parse_expression(parser);
    FengExpr *expr;

    if (condition == NULL) {
        return NULL;
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "if expressions must use '{...}' after the condition")) {
        free_expr(condition);
        return NULL;
    }

    if (peek_match_body(parser)) {
        expr = new_expr(parser, FENG_EXPR_MATCH, if_token);
        if (expr == NULL) {
            free_expr(condition);
            return NULL;
        }
        expr->as.match_expr.target = condition;
        if (!parse_match_body(parser,
                              &expr->as.match_expr.branches,
                              &expr->as.match_expr.branch_count,
                              &expr->as.match_expr.else_block)) {
            free_expr(expr);
            return NULL;
        }
        if (expr->as.match_expr.else_block == NULL) {
            (void)parser_error_at(parser, &if_token,
                                  "if-match expressions require an 'else' branch");
            free_expr(expr);
            return NULL;
        }
        return expr;
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
    if (!parser_expect(parser,
                       FENG_TOKEN_RBRACE,
                       "expected '}' to close the true branch of if expression")) {
        free_expr(expr);
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_KW_ELSE, "if expressions require an 'else' branch")) {
        free_expr(expr);
        return NULL;
    }
    expr->as.if_expr.else_block = parse_block(parser);
    if (expr->as.if_expr.else_block == NULL) {
        free_expr(expr);
        return NULL;
    }
    return expr;
}

static FengExpr *parse_group_or_cast(Parser *parser) {
    if (looks_like_lambda(parser)) {
        return parse_lambda(parser);
    }

    if (looks_like_cast(parser)) {
        FengExpr *expr = new_expr(parser, FENG_EXPR_CAST, parser_current_token(parser));

        if (expr == NULL) {
            return NULL;
        }

        if (!parser_expect(parser, FENG_TOKEN_LPAREN, "expected '(' to start cast expression")) {
            free_expr(expr);
            return NULL;
        }
        expr->as.cast.type = parse_type_ref(parser);
        if (expr->as.cast.type == NULL) {
            free_expr(expr);
            return NULL;
        }
        if (!parser_expect(parser, FENG_TOKEN_RPAREN, "expected ')' after cast type")) {
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
                       "expected '(' to start grouped expression, cast, or lambda")) {
        return NULL;
    }

    {
        FengExpr *expr = parse_expression(parser);

        if (expr == NULL) {
            return NULL;
        }
        if (!parser_expect(parser, FENG_TOKEN_RPAREN, "expected ')' to close grouped expression")) {
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
        default:
            (void)parser_error_current(
                parser,
                "expected expression term: identifier, literal, call, cast, lambda, or if-expression");
            return NULL;
    }
}

static FengExpr *parse_postfix(Parser *parser) {
    FengExpr *expr = parse_primary(parser);

    if (expr == NULL) {
        return NULL;
    }

    for (;;) {
        if (parser_match(parser, FENG_TOKEN_LPAREN)) {
            FengExpr *call = new_expr(parser, FENG_EXPR_CALL, expr->token);
            size_t arg_capacity = 0U;

            if (call == NULL) {
                free_expr(expr);
                return NULL;
            }
            call->as.call.callee = expr;

            if (!parser_check(parser, FENG_TOKEN_RPAREN)) {
                do {
                    FengExpr *arg = parse_expression(parser);

                    if (arg == NULL) {
                        free_expr(call);
                        return NULL;
                    }
                    if (!APPEND_VALUE(parser, call->as.call.args, call->as.call.arg_count, arg_capacity, arg)) {
                        free_expr(arg);
                        free_expr(call);
                        return NULL;
                    }
                } while (parser_match(parser, FENG_TOKEN_COMMA));
            }

            if (!parser_expect(parser, FENG_TOKEN_RPAREN, "expected ')' to close argument list")) {
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
                    "finalizer cannot be invoked directly via '.~'");
                return NULL;
            }

            member = new_expr(parser, FENG_EXPR_MEMBER, member_token);

            if (member == NULL) {
                free_expr(expr);
                return NULL;
            }
            if (!parser_expect_identifier_like(parser,
                                               &member->as.member.member,
                                               false,
                                               "expected an identifier after '.' in member access")) {
                free_expr(member);
                free_expr(expr);
                return NULL;
            }
            member->as.member.object = expr;
            expr = member;
            continue;
        }

        if (parser_match(parser, FENG_TOKEN_LBRACKET)) {
            FengExpr *index = new_expr(parser, FENG_EXPR_INDEX, expr->token);

            if (index == NULL) {
                free_expr(expr);
                return NULL;
            }
            index->as.index.object = expr;
            index->as.index.index = parse_expression(parser);
            if (index->as.index.index == NULL) {
                free_expr(index);
                return NULL;
            }
            if (!parser_expect(parser, FENG_TOKEN_RBRACKET, "expected ']' to close index expression")) {
                free_expr(index);
                return NULL;
            }
            expr = index;
            continue;
        }

        break;
    }

    if ((expr->kind == FENG_EXPR_IDENTIFIER || expr->kind == FENG_EXPR_MEMBER || expr->kind == FENG_EXPR_CALL) &&
        looks_like_object_literal(parser)) {
        return parse_object_literal_suffix(parser, expr);
    }

    return expr;
}

static FengExpr *parse_unary(Parser *parser) {
    if (parser_match(parser, FENG_TOKEN_NOT) ||
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

static FengExpr *parse_comparison(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_LT, FENG_TOKEN_LE, FENG_TOKEN_GT, FENG_TOKEN_GE};
    return parse_binary_series(parser, parse_shift, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_equality(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_EQ, FENG_TOKEN_NE};
    return parse_binary_series(parser, parse_comparison, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_bit_and(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_AMP};
    return parse_binary_series(parser, parse_equality, operators, sizeof(operators) / sizeof(operators[0]));
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
    return parse_binary_series(parser, parse_bit_or, operators, sizeof(operators) / sizeof(operators[0]));
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
    if (!parser_expect(parser, FENG_TOKEN_LBRACE, "expected '{' to start block")) {
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

    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close block")) {
        free_block(block);
        return NULL;
    }
    return block;
}

static FengStmt *parse_if_statement(Parser *parser) {
    FengToken if_token = parser_previous_token(parser);
    FengExpr *first_condition;

    /* Parse the head expression first; it may be either a boolean condition
     * (cond-if statement) or the match target (match statement). */
    first_condition = parse_expression(parser);
    if (first_condition == NULL) {
        return NULL;
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "expected '{' after if condition or match target")) {
        free_expr(first_condition);
        return NULL;
    }

    if (peek_match_body(parser)) {
        FengStmt *stmt = new_stmt(parser, FENG_STMT_MATCH, if_token);

        if (stmt == NULL) {
            free_expr(first_condition);
            return NULL;
        }
        stmt->as.match_stmt.target = first_condition;
        if (!parse_match_body(parser,
                              &stmt->as.match_stmt.branches,
                              &stmt->as.match_stmt.branch_count,
                              &stmt->as.match_stmt.else_block)) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
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
        if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close if block")) {
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

        stmt->as.for_stmt.iter_expr = parse_expression(parser);
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
                       "for statements require ';' after the initializer")) {
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
                       "for statements require ';' after the condition")) {
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

static FengStmt *parse_try_statement(Parser *parser) {
    FengStmt *stmt = new_stmt(parser, FENG_STMT_TRY, parser_previous_token(parser));

    if (stmt == NULL) {
        return NULL;
    }

    stmt->as.try_stmt.try_block = parse_block(parser);
    if (stmt->as.try_stmt.try_block == NULL) {
        free_stmt(stmt);
        return NULL;
    }

    if (parser_match(parser, FENG_TOKEN_KW_CATCH)) {
        stmt->as.try_stmt.catch_block = parse_block(parser);
        if (stmt->as.try_stmt.catch_block == NULL) {
            free_stmt(stmt);
            return NULL;
        }
    }

    if (parser_match(parser, FENG_TOKEN_KW_FINALLY)) {
        stmt->as.try_stmt.finally_block = parse_block(parser);
        if (stmt->as.try_stmt.finally_block == NULL) {
            free_stmt(stmt);
            return NULL;
        }
    }

    if (stmt->as.try_stmt.catch_block == NULL && stmt->as.try_stmt.finally_block == NULL) {
        (void)parser_error_current(parser,
                       "try statements must have at least one catch block or finally block");
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
        stmt->as.binding = parse_binding_core(parser, mutability, false);
        if (parser->error.message != NULL) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    if (parser_starts_typed_binding_without_keyword(parser)) {
        (void)parser_error_current(parser, "local bindings must start with 'let' or 'var'");
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

    if (terminator != FENG_TOKEN_EOF && parser_match(parser, FENG_TOKEN_ASSIGN)) {
        FengStmt *assign = new_stmt(parser, FENG_STMT_ASSIGN, stmt->token);

        if (assign == NULL) {
            free_stmt(stmt);
            return NULL;
        }
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
    if (parser_match(parser, FENG_TOKEN_KW_WHILE)) {
        return parse_while_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_FOR)) {
        return parse_for_statement(parser);
    }
    if (parser_match(parser, FENG_TOKEN_KW_TRY)) {
        return parse_try_statement(parser);
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
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "return statements must end with ';'")) {
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
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "throw statements must end with ';'")) {
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
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "break statements must end with ';'")) {
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
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "continue statements must end with ';'")) {
            free_stmt(stmt);
            return NULL;
        }
        return stmt;
    }

    stmt = parse_simple_statement(parser, FENG_TOKEN_SEMICOLON);
    if (stmt == NULL) {
        return NULL;
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_SEMICOLON,
                       "expression statements and local bindings must end with ';'")) {
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
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }
    program->path = parser->path;

    program->module_visibility = parse_visibility(parser);
    if (!parser_expect(parser, FENG_TOKEN_KW_MOD, "source file must begin with mod declaration")) {
        feng_program_free(program);
        return NULL;
    }
    program->module_token = parser_current_token(parser);
    if (!parse_path(parser,
                    false,
                    &program->module_segments,
                    &program->module_segment_count,
                    "expected a module path after 'mod'")) {
        feng_program_free(program);
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "mod declarations must end with ';'")) {
        feng_program_free(program);
        return NULL;
    }

    while (parser_match(parser, FENG_TOKEN_KW_USE)) {
        FengUseDecl use_decl;

        memset(&use_decl, 0, sizeof(use_decl));
        use_decl.token = parser_current_token(parser);
        if (!parse_path(parser,
                false,
                &use_decl.segments,
                &use_decl.segment_count,
                "expected a module path after 'use'")) {
            feng_program_free(program);
            return NULL;
        }
        if (parser_match(parser, FENG_TOKEN_KW_AS)) {
            use_decl.has_alias = true;
            if (!parser_expect_identifier_like(parser,
                                               &use_decl.alias,
                                               false,
                                               "expected an alias name after 'as'")) {
                free(use_decl.segments);
                feng_program_free(program);
                return NULL;
            }
        }
        if (!parser_expect(parser, FENG_TOKEN_SEMICOLON, "use declarations must end with ';'")) {
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
    if (type_ref == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            free(type_ref->as.named.segments);
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
        case FENG_EXPR_OBJECT_LITERAL:
            free_expr(expr->as.object_literal.target);
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                free_expr(expr->as.object_literal.fields[index].value);
            }
            free(expr->as.object_literal.fields);
            break;
        case FENG_EXPR_CALL:
            free_expr(expr->as.call.callee);
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                free_expr(expr->as.call.args[index]);
            }
            free(expr->as.call.args);
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
        default:
            break;
    }

    free(expr);
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
            break;
        case FENG_STMT_ASSIGN:
            free_expr(stmt->as.assign.target);
            free_expr(stmt->as.assign.value);
            break;
        case FENG_STMT_EXPR:
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
        case FENG_STMT_TRY:
            free_block(stmt->as.try_stmt.try_block);
            free_block(stmt->as.try_stmt.catch_block);
            free_block(stmt->as.try_stmt.finally_block);
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
    }

    free(stmt);
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
        free_parameters(member->as.callable.params, member->as.callable.param_count);
        free_type_ref(member->as.callable.return_type);
        free_block(member->as.callable.body);
    }

    free(member);
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
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                free_type_member(decl->as.type_decl.members[index]);
            }
            free(decl->as.type_decl.members);
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                free_type_ref(decl->as.type_decl.declared_specs[index]);
            }
            free(decl->as.type_decl.declared_specs);
            break;
        case FENG_DECL_SPEC:
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                free_type_ref(decl->as.spec_decl.parent_specs[index]);
            }
            free(decl->as.spec_decl.parent_specs);
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    free_type_member(decl->as.spec_decl.as.object.members[index]);
                }
                free(decl->as.spec_decl.as.object.members);
            } else {
                free_parameters(decl->as.spec_decl.as.callable.params,
                                decl->as.spec_decl.as.callable.param_count);
                free_type_ref(decl->as.spec_decl.as.callable.return_type);
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
