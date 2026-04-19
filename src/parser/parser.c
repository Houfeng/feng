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

static FengTypeRef *new_type_ref(Parser *parser, FengTypeRefKind kind) {
    FengTypeRef *type_ref = (FengTypeRef *)calloc(1U, sizeof(*type_ref));

    if (type_ref == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }

    type_ref->kind = kind;
    return type_ref;
}

static FengTypeRef *parse_type_ref(Parser *parser) {
    FengTypeRef *type_ref;
    size_t pointer_count = 0U;

    while (parser_match(parser, FENG_TOKEN_STAR)) {
        ++pointer_count;
    }

    type_ref = new_type_ref(parser, FENG_TYPE_REF_NAMED);
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
        FengTypeRef *wrapper = new_type_ref(parser, FENG_TYPE_REF_POINTER);

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

        wrapper = new_type_ref(parser, FENG_TYPE_REF_ARRAY);
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

            param.mutability = FENG_MUTABILITY_DEFAULT;
            param.type = NULL;
            if (parser_match(parser, FENG_TOKEN_KW_LET)) {
                param.mutability = FENG_MUTABILITY_LET;
            } else if (parser_match(parser, FENG_TOKEN_KW_VAR)) {
                param.mutability = FENG_MUTABILITY_VAR;
            }

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

static FengCallableSignature parse_callable_signature(Parser *parser, FengSlice name) {
    FengCallableSignature callable;

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

    if (parser_check(parser, FENG_TOKEN_LBRACE)) {
        callable.body = parse_block(parser);
    } else if (!parser_expect(parser,
                              FENG_TOKEN_SEMICOLON,
                              "function declarations must end with ';' or provide a body '{...}'")) {
        return callable;
    }

    return callable;
}

static FengDecl *new_decl(Parser *parser, FengDeclKind kind) {
    FengDecl *decl = (FengDecl *)calloc(1U, sizeof(*decl));

    if (decl == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }

    decl->kind = kind;
    return decl;
}

static FengTypeMember *new_type_member(Parser *parser, FengTypeMemberKind kind) {
    FengTypeMember *member = (FengTypeMember *)calloc(1U, sizeof(*member));

    if (member == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }

    member->kind = kind;
    return member;
}

static FengDecl *parse_type_declaration(Parser *parser,
                                        FengVisibility visibility,
                                        bool is_extern,
                                        FengAnnotation *annotations,
                                        size_t annotation_count) {
    FengDecl *decl = new_decl(parser, FENG_DECL_TYPE);
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
        decl->as.type_decl.form = FENG_TYPE_DECL_FUNCTION;
        if (!parse_parameters(parser,
                              &decl->as.type_decl.as.function.params,
                              &decl->as.type_decl.as.function.param_count)) {
            free_decl(decl);
            return NULL;
        }
        if (!parser_expect(parser,
                   FENG_TOKEN_COLON,
                   "function type declarations require ':' before the return type")) {
            free_decl(decl);
            return NULL;
        }
        decl->as.type_decl.as.function.return_type = parse_type_ref(parser);
        if (decl->as.type_decl.as.function.return_type == NULL) {
            free_decl(decl);
            return NULL;
        }
        if (!parser_expect(parser,
                   FENG_TOKEN_SEMICOLON,
                   "function type declarations must end with ';'")) {
            free_decl(decl);
            return NULL;
        }
        return decl;
    }

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "type declarations must use '{...}' for object form or '(...) : ...;' for function form")) {
        free_decl(decl);
        return NULL;
    }

    decl->as.type_decl.form = FENG_TYPE_DECL_OBJECT;
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

            member = new_type_member(parser, FENG_TYPE_MEMBER_FIELD);
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
            FengTypeMemberKind member_kind = FENG_TYPE_MEMBER_METHOD;

            if (is_extern) {
                free_annotations(member_annotations, member_annotation_count);
                (void)parser_error_current(
                    parser,
                    "extern type object form only supports fields; methods require a non-extern type");
                free_decl(decl);
                return NULL;
            }
            if (!parser_expect_identifier_like(parser,
                                               &name,
                                               false,
                                               "expected a method or constructor name after 'fn'")) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            callable = parse_callable_signature(parser, name);
            if (parser->error.message != NULL) {
                free_annotations(member_annotations, member_annotation_count);
                free_decl(decl);
                return NULL;
            }

            if (slice_equals(name, type_name) && callable.return_type == NULL) {
                member_kind = FENG_TYPE_MEMBER_CONSTRUCTOR;
            }

            member = new_type_member(parser, member_kind);
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
                    "type members cannot start with 'extern'; use 'fn' for methods or 'let'/'var' for fields");
            } else {
                (void)parser_error_current(parser,
                                           "expected type member declaration: 'let', 'var', or 'fn'");
            }
            free_decl(decl);
            return NULL;
        }

        if (!APPEND_VALUE(parser,
                          decl->as.type_decl.as.object.members,
                          decl->as.type_decl.as.object.member_count,
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

static FengDecl *parse_function_declaration(Parser *parser,
                                            FengVisibility visibility,
                                            bool is_extern,
                                            FengAnnotation *annotations,
                                            size_t annotation_count) {
    FengDecl *decl = new_decl(parser, FENG_DECL_FUNCTION);
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

    decl->as.function_decl = parse_callable_signature(parser, name);
    if (parser->error.message != NULL) {
        free_decl(decl);
        return NULL;
    }

    return decl;
}

static FengDecl *parse_global_binding(Parser *parser,
                                      FengMutability mutability,
                                      FengAnnotation *annotations,
                                      size_t annotation_count) {
    FengDecl *decl = new_decl(parser, FENG_DECL_GLOBAL_BINDING);

    if (decl == NULL) {
        free_annotations(annotations, annotation_count);
        return NULL;
    }

    decl->annotations = annotations;
    decl->annotation_count = annotation_count;
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
        return parse_global_binding(parser, FENG_MUTABILITY_LET, annotations, annotation_count);
    }
    if (parser_match(parser, FENG_TOKEN_KW_VAR)) {
        return parse_global_binding(parser, FENG_MUTABILITY_VAR, annotations, annotation_count);
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
    if (parser_match(parser, FENG_TOKEN_KW_FN)) {
        return parse_function_declaration(parser, visibility, is_extern, annotations, annotation_count);
    }

    free_annotations(annotations, annotation_count);
    (void)parser_error_current(parser,
                               "expected top-level declaration: 'let', 'var', 'extern fn', 'type', or 'fn'");
    return NULL;
}

static FengExpr *new_expr(Parser *parser, FengExprKind kind) {
    FengExpr *expr = (FengExpr *)calloc(1U, sizeof(*expr));

    if (expr == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }
    expr->kind = kind;
    return expr;
}

static FengStmt *new_stmt(Parser *parser, FengStmtKind kind) {
    FengStmt *stmt = (FengStmt *)calloc(1U, sizeof(*stmt));

    if (stmt == NULL) {
        (void)parser_error_current(parser, "out of memory");
        return NULL;
    }
    stmt->kind = kind;
    return stmt;
}

static FengBlock *new_block(Parser *parser) {
    FengBlock *block = (FengBlock *)calloc(1U, sizeof(*block));

    if (block == NULL) {
        (void)parser_error_current(parser, "out of memory");
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
                return parser->tokens[index + 1U].kind == FENG_TOKEN_ARROW && (saw_colon || is_empty);
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
    FengExpr *expr = new_expr(parser, FENG_EXPR_OBJECT_LITERAL);
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
    FengExpr *expr = new_expr(parser, FENG_EXPR_ARRAY_LITERAL);
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
    FengExpr *expr = new_expr(parser, FENG_EXPR_LAMBDA);

    if (expr == NULL) {
        return NULL;
    }

    if (!parse_parameters(parser, &expr->as.lambda.params, &expr->as.lambda.param_count)) {
        free_expr(expr);
        return NULL;
    }
    if (!parser_expect(parser,
                       FENG_TOKEN_ARROW,
                       "lambda expressions must use '->' after the parameter list")) {
        free_expr(expr);
        return NULL;
    }

    expr->as.lambda.body = parse_expression(parser);
    if (expr->as.lambda.body == NULL) {
        free_expr(expr);
        return NULL;
    }

    return expr;
}

static FengExpr *parse_if_expression(Parser *parser) {
    FengExpr *condition = parse_expression(parser);
    size_t mark;
    FengExpr *expr;

    if (condition == NULL) {
        return NULL;
    }

    mark = parser->current;

    expr = new_expr(parser, FENG_EXPR_IF);
    if (expr == NULL) {
        free_expr(condition);
        return NULL;
    }
    expr->as.if_expr.condition = condition;

    if (parser_expect(parser,
                      FENG_TOKEN_LBRACE,
                      "if expressions must use '{...}' after the condition") &&
        (expr->as.if_expr.then_expr = parse_expression(parser)) != NULL &&
        parser_expect(parser,
                      FENG_TOKEN_RBRACE,
                      "expected '}' to close the true branch of if expression") &&
        parser_expect(parser, FENG_TOKEN_KW_ELSE, "if expressions require an 'else' branch") &&
        parser_expect(parser,
                      FENG_TOKEN_LBRACE,
                      "if expression else branch must use '{...}'") &&
        (expr->as.if_expr.else_expr = parse_expression(parser)) != NULL &&
        parser_expect(parser,
                      FENG_TOKEN_RBRACE,
                      "expected '}' to close the else branch of if expression")) {
        return expr;
    }

    expr->as.if_expr.condition = NULL;
    free_expr(expr);
    parser->current = mark;
    parser->error.message = NULL;

    expr = new_expr(parser, FENG_EXPR_MATCH);
    if (expr == NULL) {
        free_expr(condition);
        return NULL;
    }
    expr->as.match_expr.target = condition;

    if (!parser_expect(parser,
                       FENG_TOKEN_LBRACE,
                       "match expressions must use '{...}' after the target expression")) {
        free_expr(expr);
        return NULL;
    }

    {
    size_t case_capacity = 0U;
    while (!parser_check(parser, FENG_TOKEN_RBRACE) && !parser_is_at_end(parser)) {
        if (parser_match(parser, FENG_TOKEN_KW_ELSE)) {
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "expected ':' after else in match expression")) {
                free_expr(expr);
                return NULL;
            }
            expr->as.match_expr.else_expr = parse_expression(parser);
            if (expr->as.match_expr.else_expr == NULL) {
                free_expr(expr);
                return NULL;
            }
        } else {
            FengMatchCase match_case;

            match_case.label = parse_expression(parser);
            if (match_case.label == NULL) {
                free_expr(expr);
                return NULL;
            }
            if (!parser_expect(parser,
                               FENG_TOKEN_COLON,
                               "expected ':' after match case label")) {
                free_expr(match_case.label);
                free_expr(expr);
                return NULL;
            }
            match_case.value = parse_expression(parser);
            if (match_case.value == NULL) {
                free_expr(match_case.label);
                free_expr(expr);
                return NULL;
            }
            if (!APPEND_VALUE(parser,
                              expr->as.match_expr.cases,
                              expr->as.match_expr.case_count,
                              case_capacity,
                              match_case)) {
                free_expr(match_case.label);
                free_expr(match_case.value);
                free_expr(expr);
                return NULL;
            }
        }

        if (!parser_match(parser, FENG_TOKEN_COMMA)) {
            break;
        }
    }
    }

    if (expr->as.match_expr.else_expr == NULL) {
        (void)parser_error_current(parser, "match expressions require an else branch");
        free_expr(expr);
        return NULL;
    }
    if (!parser_expect(parser, FENG_TOKEN_RBRACE, "expected '}' to close match expression")) {
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
        FengExpr *expr = new_expr(parser, FENG_EXPR_CAST);

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
            expr = new_expr(parser, FENG_EXPR_IDENTIFIER);
            if (expr != NULL) {
                expr->as.identifier = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_KW_SELF:
            expr = new_expr(parser, FENG_EXPR_SELF);
            if (expr != NULL) {
                expr->as.identifier = slice_from_token(&token);
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_BOOL:
            expr = new_expr(parser, FENG_EXPR_BOOL);
            if (expr != NULL) {
                expr->as.boolean = token.value.boolean;
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_INTEGER:
            expr = new_expr(parser, FENG_EXPR_INTEGER);
            if (expr != NULL) {
                expr->as.integer = token.value.integer;
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_FLOAT:
            expr = new_expr(parser, FENG_EXPR_FLOAT);
            if (expr != NULL) {
                expr->as.floating = token.value.floating;
                (void)parser_advance(parser);
            }
            return expr;
        case FENG_TOKEN_STRING:
            expr = new_expr(parser, FENG_EXPR_STRING);
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
            return parse_if_expression(parser);
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
            FengExpr *call = new_expr(parser, FENG_EXPR_CALL);
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
            FengExpr *member = new_expr(parser, FENG_EXPR_MEMBER);

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
            FengExpr *index = new_expr(parser, FENG_EXPR_INDEX);

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
    if (parser_match(parser, FENG_TOKEN_NOT) || parser_match(parser, FENG_TOKEN_MINUS)) {
        FengExpr *expr = new_expr(parser, FENG_EXPR_UNARY);

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
                FengExpr *binary = new_expr(parser, FENG_EXPR_BINARY);

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

static FengExpr *parse_comparison(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_LT, FENG_TOKEN_LE, FENG_TOKEN_GT, FENG_TOKEN_GE};
    return parse_binary_series(parser, parse_additive, operators, sizeof(operators) / sizeof(operators[0]));
}

static FengExpr *parse_equality(Parser *parser) {
    static const FengTokenKind operators[] = {FENG_TOKEN_EQ, FENG_TOKEN_NE};
    return parse_binary_series(parser, parse_comparison, operators, sizeof(operators) / sizeof(operators[0]));
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
    FengBlock *block = new_block(parser);
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
    FengStmt *stmt = new_stmt(parser, FENG_STMT_IF);
    size_t capacity = 0U;

    if (stmt == NULL) {
        return NULL;
    }

    for (;;) {
        FengIfClause clause;

        clause.condition = parse_expression(parser);
        if (clause.condition == NULL) {
            free_stmt(stmt);
            return NULL;
        }
        clause.block = parse_block(parser);
        if (clause.block == NULL) {
            free_expr(clause.condition);
            free_stmt(stmt);
            return NULL;
        }
        if (!APPEND_VALUE(parser, stmt->as.if_stmt.clauses, stmt->as.if_stmt.clause_count, capacity, clause)) {
            free_expr(clause.condition);
            free_block(clause.block);
            free_stmt(stmt);
            return NULL;
        }

        if (!parser_match(parser, FENG_TOKEN_KW_ELSE)) {
            break;
        }
        if (parser_match(parser, FENG_TOKEN_KW_IF)) {
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

static FengStmt *parse_while_statement(Parser *parser) {
    FengStmt *stmt = new_stmt(parser, FENG_STMT_WHILE);

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
    FengStmt *stmt = new_stmt(parser, FENG_STMT_FOR);

    if (stmt == NULL) {
        return NULL;
    }

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
    FengStmt *stmt = new_stmt(parser, FENG_STMT_TRY);

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

        stmt = new_stmt(parser, FENG_STMT_BINDING);
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

    stmt = new_stmt(parser, FENG_STMT_EXPR);
    if (stmt == NULL) {
        return NULL;
    }

    stmt->as.expr = parse_expression(parser);
    if (stmt->as.expr == NULL) {
        free_stmt(stmt);
        return NULL;
    }

    if (terminator != FENG_TOKEN_EOF && parser_match(parser, FENG_TOKEN_ASSIGN)) {
        FengStmt *assign = new_stmt(parser, FENG_STMT_ASSIGN);

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
        stmt = new_stmt(parser, FENG_STMT_BLOCK);
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
        stmt = new_stmt(parser, FENG_STMT_RETURN);
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
        stmt = new_stmt(parser, FENG_STMT_THROW);
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
        stmt = new_stmt(parser, FENG_STMT_BREAK);
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
        stmt = new_stmt(parser, FENG_STMT_CONTINUE);
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
            free_expr(expr->as.lambda.body);
            break;
        case FENG_EXPR_CAST:
            free_type_ref(expr->as.cast.type);
            free_expr(expr->as.cast.value);
            break;
        case FENG_EXPR_IF:
            free_expr(expr->as.if_expr.condition);
            free_expr(expr->as.if_expr.then_expr);
            free_expr(expr->as.if_expr.else_expr);
            break;
        case FENG_EXPR_MATCH:
            free_expr(expr->as.match_expr.target);
            for (index = 0U; index < expr->as.match_expr.case_count; ++index) {
                free_expr(expr->as.match_expr.cases[index].label);
                free_expr(expr->as.match_expr.cases[index].value);
            }
            free(expr->as.match_expr.cases);
            free_expr(expr->as.match_expr.else_expr);
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
        case FENG_STMT_WHILE:
            free_expr(stmt->as.while_stmt.condition);
            free_block(stmt->as.while_stmt.body);
            break;
        case FENG_STMT_FOR:
            free_stmt(stmt->as.for_stmt.init);
            free_expr(stmt->as.for_stmt.condition);
            free_stmt(stmt->as.for_stmt.update);
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
            if (decl->as.type_decl.form == FENG_TYPE_DECL_OBJECT) {
                for (index = 0U; index < decl->as.type_decl.as.object.member_count; ++index) {
                    free_type_member(decl->as.type_decl.as.object.members[index]);
                }
                free(decl->as.type_decl.as.object.members);
            } else {
                free_parameters(decl->as.type_decl.as.function.params,
                                decl->as.type_decl.as.function.param_count);
                free_type_ref(decl->as.type_decl.as.function.return_type);
            }
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
