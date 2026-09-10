#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Structural static owners preserve generic arguments and each array layer;
 * ordinary index and allocation nodes keep their separate meaning. */
void test_structural_type_target_parser(void) {
    const char *source =
        "module structural;func run<T>(){\n"
        "let direct=Box<Box<T>>[!][].keep<string>(\"text\");\n"
        "let method=i32[!].echo;let qualified=some.pkg.Box<T>[].echo;\n"
        "let indexed=items[0];let made=Box<T>[][:2];}\n";
    FengProgram *program = NULL;
    FengParseError error = {0};
    CHECK(feng_parse_source(source, strlen(source), "structural.ff", &program, &error));
    const FengBlock *body = program->declarations[0]->as.function_decl.body;
    CHECK(body->statement_count == 5U);
    const FengExpr *call = body->statements[0]->as.binding.initializer;
    CHECK(call->kind == FENG_EXPR_CALL && call->as.call.explicit_type_arg_count == 1U);
    const FengExpr *target = call->as.call.callee->as.member.object;
    CHECK(target->kind == FENG_EXPR_TYPE_TARGET);
    const FengTypeRef *type = target->as.type_target;
    CHECK(type->kind == FENG_TYPE_REF_ARRAY && !type->array_element_writable);
    type = type->as.inner;
    CHECK(type->kind == FENG_TYPE_REF_ARRAY && type->array_element_writable);
    type = type->as.inner;
    CHECK(type->kind == FENG_TYPE_REF_NAMED && type->as.named.type_arg_count == 1U);
    type = type->as.named.type_args[0];
    CHECK(type->kind == FENG_TYPE_REF_NAMED && type->as.named.type_arg_count == 1U);
    CHECK(type->as.named.type_args[0]->as.named.segments[0].length == 1U);
    const FengExpr *method = body->statements[1]->as.binding.initializer;
    CHECK(method->kind == FENG_EXPR_MEMBER);
    CHECK(method->as.member.object->as.type_target->array_element_writable);
    const FengExpr *qualified = body->statements[2]->as.binding.initializer;
    type = qualified->as.member.object->as.type_target->as.inner;
    CHECK(type->as.named.segment_count == 3U && type->as.named.type_arg_count == 1U);
    CHECK(body->statements[3]->as.binding.initializer->kind == FENG_EXPR_INDEX);
    const FengExpr *made = body->statements[4]->as.binding.initializer;
    CHECK(made->kind == FENG_EXPR_ARRAY_NEW && made->as.array_new.element_type->kind == FENG_TYPE_REF_ARRAY);
    CHECK(made->as.array_new.element_type->as.inner->as.named.type_arg_count == 1U);
    feng_program_free(program);

    source = "module structural;func run(){factory()[].echo();}";
    program = NULL;
    CHECK(!feng_parse_source(source, strlen(source), "invalid_target.ff", &program, &error));
    CHECK(strcmp(error.code, "SE0201") == 0);
    feng_program_free(program);
    puts("structural static target parser passed");
}
