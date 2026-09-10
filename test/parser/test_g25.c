#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"

#define G25_CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1); \
        } \
    } while (0)

/* Parse real source; every success case below also checks its AST structure. */
static FengProgram *g25_parse(const char *source) {
    FengProgram *program = NULL;
    FengParseError error = {0};

    if (!feng_parse_source(source, strlen(source), "g25_parser.ff", &program, &error)) {
        fprintf(stderr, "G25 Parser %s at %u:%u: %s\n%s\n", error.code,
                error.token.line, error.token.column, error.message, source);
        exit(1);
    }
    G25_CHECK(program != NULL);
    return program;
}

/* Check named identity and arity without rendering or reparsing expected types. */
static void g25_named(const FengTypeRef *type, const char *name, size_t arity) {
    G25_CHECK(type != NULL && type->kind == FENG_TYPE_REF_NAMED);
    G25_CHECK(type->as.named.segment_count == 1U);
    G25_CHECK(type->as.named.segments[0].length == strlen(name));
    G25_CHECK(memcmp(type->as.named.segments[0].data, name, strlen(name)) == 0);
    G25_CHECK(type->as.named.type_arg_count == arity);
}

/* Walk exactly n generic layers and require the expected unwrapped leaf. */
static void g25_nested(const FengTypeRef *type, size_t depth, const char *leaf) {
    for (size_t i = 0U; i < depth; ++i) {
        g25_named(type, "Box", 1U);
        type = type->as.named.type_args[0];
    }
    g25_named(type, leaf, 0U);
}

/* Build independent source text with adjacent closing delimiters at any depth. */
static void g25_nested_source(char *buffer, size_t capacity, size_t depth) {
    size_t length = 0U;

    G25_CHECK(depth <= (capacity - 4U) / 5U);
    for (size_t i = 0U; i < depth; ++i) {
        memcpy(buffer + length, "Box<", 4U);
        length += 4U;
    }
    memcpy(buffer + length, "i32", 3U);
    length += 3U;
    for (size_t i = 0U; i < depth; ++i) {
        buffer[length++] = '>';
    }
    buffer[length] = '\0';
}

/* Delimiters and suffixes must keep their ownership at odd, even and deep n. */
static void g25_parser_depths(void) {
    static const size_t depths[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
                                    15U, 16U, 31U, 32U, 63U, 64U};
    static const char *suffixes[] = {"", "[]", "[!]", "*", "[][!]*"};

    for (size_t d = 0U; d < sizeof(depths) / sizeof(depths[0]); ++d) {
        char nested[1024];
        g25_nested_source(nested, sizeof(nested), depths[d]);
        for (size_t s = 0U; s < sizeof(suffixes) / sizeof(suffixes[0]); ++s) {
            char source[4096];
            int length = snprintf(source, sizeof(source),
                "module g25;\nfunc check(first:i32, value:%s%s, last:string):%s {}\n",
                nested, suffixes[s], nested);
            G25_CHECK(length > 0 && (size_t)length < sizeof(source));
            FengProgram *program = g25_parse(source);
            const FengCallableSignature *callable = &program->declarations[0]->as.function_decl;
            G25_CHECK(callable->param_count == 3U);
            g25_named(callable->params[0].type, "i32", 0U);
            g25_named(callable->params[2].type, "string", 0U);
            const FengTypeRef *value_type = callable->params[1].type;
            if (s == 3U || s == 4U) {
                G25_CHECK(value_type->kind == FENG_TYPE_REF_POINTER);
                value_type = value_type->as.inner;
            }
            if (s == 1U || s == 2U || s == 4U) {
                G25_CHECK(value_type->kind == FENG_TYPE_REF_ARRAY);
                G25_CHECK(value_type->array_element_writable == (s != 1U));
                value_type = value_type->as.inner;
            }
            if (s == 4U) {
                G25_CHECK(value_type->kind == FENG_TYPE_REF_ARRAY);
                G25_CHECK(!value_type->array_element_writable);
                value_type = value_type->as.inner;
            }
            g25_nested(value_type, depths[d], "i32");
            g25_nested(callable->return_type, depths[d], "i32");
            feng_program_free(program);
        }
    }
}

/* Commas inside/outside nested types and constraints must close the right list. */
static void g25_parser_mixed_arguments(void) {
    FengProgram *program = g25_parse(
        "module g25;\n"
        "func check<T:Bound<Box<Box<i32>>>,U>(\n"
        "  left:Pair<Box<Box<i32>>,Box<Box<string>>>,\n"
        "  right:Box<Pair<Box<i32>,Box<string>>>[], last:U) {}\n");
    const FengCallableSignature *callable = &program->declarations[0]->as.function_decl;
    G25_CHECK(callable->type_param_count == 2U);
    g25_named(callable->type_params[0].constraint, "Bound", 1U);
    g25_nested(callable->type_params[0].constraint->as.named.type_args[0], 2U, "i32");
    G25_CHECK(callable->params[1].type->kind == FENG_TYPE_REF_ARRAY);
    const FengTypeRef *pair = callable->params[0].type;
    g25_named(pair, "Pair", 2U);
    g25_nested(pair->as.named.type_args[0], 2U, "i32");
    g25_nested(pair->as.named.type_args[1], 2U, "string");
    const FengTypeRef *box = callable->params[1].type->as.inner;
    g25_named(box, "Box", 1U);
    pair = box->as.named.type_args[0];
    g25_named(pair, "Pair", 2U);
    g25_nested(pair->as.named.type_args[0], 1U, "i32");
    g25_nested(pair->as.named.type_args[1], 1U, "string");
    g25_named(callable->params[2].type, "U", 0U);
    feng_program_free(program);
}

/* Speculative generic/cast lookahead shares the token view; shifts stay shifts. */
static void g25_parser_expression_boundaries(void) {
    FengProgram *program = g25_parse(
        "module g25;\nfunc check(a:i32,b:i32,c:i32,value:i32) {\n"
        "  let call = source<Box<Box<i32>>,string>(value);\n"
        "  let cast = (Box<Box<i32>>[])value;\n"
        "  let owner = Holder<Box<Box<i32>>>.make();\n"
        "  let shifted = a >> b;\n"
        "  let compared = a < b >> (c);\n"
        "  let matched = value match Box<Box<i32>>;\n"
        "}\n");
    const FengBlock *body = program->declarations[0]->as.function_decl.body;
    G25_CHECK(body->statement_count == 6U);
    const FengExpr *call = body->statements[0]->as.binding.initializer;
    G25_CHECK(call->kind == FENG_EXPR_CALL && call->as.call.has_explicit_type_args);
    G25_CHECK(call->as.call.explicit_type_arg_count == 2U);
    g25_nested(call->as.call.explicit_type_args[0], 2U, "i32");
    g25_named(call->as.call.explicit_type_args[1], "string", 0U);
    const FengExpr *cast = body->statements[1]->as.binding.initializer;
    G25_CHECK(cast->kind == FENG_EXPR_CAST);
    G25_CHECK(cast->as.cast.type->kind == FENG_TYPE_REF_ARRAY);
    g25_nested(cast->as.cast.type->as.inner, 2U, "i32");
    call = body->statements[2]->as.binding.initializer;
    G25_CHECK(call->kind == FENG_EXPR_CALL && call->as.call.callee->kind == FENG_EXPR_MEMBER);
    const FengExpr *target = call->as.call.callee->as.member.object;
    G25_CHECK(target->kind == FENG_EXPR_GENERIC_TARGET);
    G25_CHECK(target->as.generic_target.type_arg_count == 1U);
    g25_nested(target->as.generic_target.type_args[0], 2U, "i32");
    const FengExpr *shift = body->statements[3]->as.binding.initializer;
    G25_CHECK(shift->kind == FENG_EXPR_BINARY && shift->as.binary.op == FENG_TOKEN_SHR);
    const FengExpr *compared = body->statements[4]->as.binding.initializer;
    G25_CHECK(compared->kind == FENG_EXPR_BINARY && compared->as.binary.op == FENG_TOKEN_LT);
    G25_CHECK(compared->as.binary.right->kind == FENG_EXPR_BINARY);
    G25_CHECK(compared->as.binary.right->as.binary.op == FENG_TOKEN_SHR);
    const FengExpr *matched = body->statements[5]->as.binding.initializer;
    G25_CHECK(matched->kind == FENG_EXPR_MATCH_OP);
    G25_CHECK(matched->as.match_op.label_count == 1U);
    g25_nested(matched->as.match_op.labels[0].type, 2U, "i32");
    feng_program_free(program);
}

/* A spare split delimiter must be diagnosed at its own one-character span. */
static void g25_parser_extra_closing_delimiter(void) {
    const char *source = "module g25;\nfunc check(value:Box<i32>>) {}\n";
    const char *extra = strstr(source, ">>") + 1;
    FengProgram *program = NULL;
    FengParseError error = {0};
    G25_CHECK(!feng_parse_source(source, strlen(source), "g25_extra.ff", &program, &error));
    G25_CHECK(program == NULL);
    G25_CHECK(strcmp(error.code, "SE0515") == 0);
    G25_CHECK(error.token.kind == FENG_TOKEN_GT && error.token.length == 1U);
    G25_CHECK(error.token.lexeme == extra && error.token.offset == (size_t)(extra - source));
    G25_CHECK(error.token.line == 2U);
    G25_CHECK(error.token.column == (unsigned)(extra - strchr(source, '\n')));
}

/* G25 ISSUE-016: generic nesting has no two-level parsing special case. */
void test_g25_nested_generic_parser(void) {
    g25_parser_depths();
    g25_parser_mixed_arguments();
    g25_parser_expression_boundaries();
    g25_parser_extra_closing_delimiter();
    puts("G25 nested generic parser matrices passed (depths 1–64)");
}
