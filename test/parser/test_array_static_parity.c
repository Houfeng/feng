#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Preserve qualified generic owners for all inner/outer writability pairs. */
void test_array_static_parity_parser(void) {
    const char *suffixes[] = {"[]", "[!]"};
    for (size_t inner = 0U; inner < 2U; ++inner) {
        for (size_t outer = 0U; outer < 2U; ++outer) {
            char source[512];
            snprintf(source, sizeof source,
                "module parity;func inspect<E>(){"
                "let call=ns.Box<E>%s%s.echo(7);let value=ns.Box<E>%s%s.echo;}",
                suffixes[inner], suffixes[outer], suffixes[inner], suffixes[outer]);
            FengProgram *program = NULL;
            FengParseError error = {0};
            CHECK(feng_parse_source(source, strlen(source), "parity.ff", &program, &error));
            const FengBlock *body = program->declarations[0]->as.function_decl.body;
            CHECK(body->statement_count == 2U);
            for (size_t i = 0U; i < 2U; ++i) {
                const FengExpr *member = body->statements[i]->as.binding.initializer;
                if (i == 0U) {
                    CHECK(member->kind == FENG_EXPR_CALL);
                    member = member->as.call.callee;
                }
                CHECK(member->kind == FENG_EXPR_MEMBER);
                CHECK(member->as.member.object->kind == FENG_EXPR_TYPE_TARGET);
                const FengTypeRef *type = member->as.member.object->as.type_target;
                CHECK(type->kind == FENG_TYPE_REF_ARRAY);
                CHECK(type->array_element_writable == (outer == 1U));
                type = type->as.inner;
                CHECK(type->kind == FENG_TYPE_REF_ARRAY);
                CHECK(type->array_element_writable == (inner == 1U));
                type = type->as.inner;
                CHECK(type->kind == FENG_TYPE_REF_NAMED);
                CHECK(type->as.named.segment_count == 2U && type->as.named.type_arg_count == 1U);
                const FengTypeRef *argument = type->as.named.type_args[0];
                CHECK(argument->kind == FENG_TYPE_REF_NAMED);
                CHECK(argument->as.named.segments[0].length == 1U);
                CHECK(argument->as.named.segments[0].data[0] == 'E');
            }
            feng_program_free(program);
        }
        char invalid[128];
        snprintf(invalid, sizeof invalid,
            "module parity;func run(){factory()%s.echo();}", suffixes[inner]);
        FengProgram *program = NULL;
        FengParseError error = {0};
        CHECK(!feng_parse_source(invalid, strlen(invalid), "invalid.ff", &program, &error));
        CHECK(strcmp(error.code, "SE0201") == 0);
        feng_program_free(program);
    }
    puts("array static parser writability matrix passed");
}
