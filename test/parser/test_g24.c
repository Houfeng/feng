#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** A parser boundary with an exact diagnostic and source token. */
typedef struct CompositeSyntaxCase { const char *source, *code, *marker, *token; } CompositeSyntaxCase;

/** A match-label colon inside a group is not a lambda parameter prefix. */
static bool grouped_match_and_lambda_controls(void) {
    const char *bindings[] = {"", "let ", "var "};
    bool passed = true;
    for (size_t i = 0U; i < 3U; ++i) {
        char source[512];
        snprintf(source, sizeof(source), "module g24;\n"
            "func match_group() { return !((value match %sitem: Item)); }\n"
            "func lambda_control() { return (%svalue: i32) { return value; }; }\n"
            "func empty_control() { return () { return 1; }; }\n", bindings[i], bindings[i]);
        FengProgram *program = NULL;
        FengParseError error = {0};
        if (!feng_parse_source(source, strlen(source), "g24_group.ff", &program, &error)) {
            fprintf(stderr, "G24 grouped match control %zu: %s %s\n", i, error.code, error.message);
            passed = false;
            continue;
        }
        const FengExpr *negation = program->declarations[0]->as.function_decl.body->statements[0]->as.return_value;
        const FengExpr *lambda = program->declarations[1]->as.function_decl.body->statements[0]->as.return_value;
        const FengExpr *empty = program->declarations[2]->as.function_decl.body->statements[0]->as.return_value;
        if (negation->kind != FENG_EXPR_UNARY || negation->as.unary.operand->kind != FENG_EXPR_MATCH_OP ||
            !negation->as.unary.operand->as.match_op.has_binding ||
            negation->as.unary.operand->as.match_op.binding_mutability != (i == 2U ? FENG_MUTABILITY_VAR : FENG_MUTABILITY_LET) ||
            lambda->kind != FENG_EXPR_LAMBDA || lambda->as.lambda.param_count != 1U ||
            empty->kind != FENG_EXPR_LAMBDA || empty->as.lambda.param_count != 0U) passed = false;
        feng_program_free(program);
    }
    return passed;
}

/** Source-only malformed forms: no synthetic invalid ASTs bypass the parser. */
void test_g24_composite_syntax(void) {
    const CompositeSyntaxCase cases[] = {
        {"module g24;\nspec U: i32 | ;\n", "SE0002", ";\n", ";"},
        {"module g24;\nspec Both: A & ;\n", "SE0002", ";\n", ";"},
        {"module g24;\nspec U: i32 | string", "SE0001", "", ""},
        {"module g24;\nspec Both: A & B", "SE0001", "", ""},
        {"module g24;\nspec U: i32 | string {}\n", "SE0001", "{}", "{"},
        {"module g24;\nspec Both: A & B {}\n", "SE0001", "{}", "{"},
        {"module g24;\nspec U: i32 | string { let field: i32; }\n", "SE0001", "{ let", "{"},
        {"module g24;\nspec Both: A & B { func read(): i32; }\n", "SE0001", "{ func", "{"},
        {"module g24;\nspec U: i32 | void;\n", "SE0609", "void;", "void"},
        {"module g24;\nfunc run(value: i32 | string) {}\n", "SE0515", "| string", "|"},
        {"module g24;\nfunc run(value: A & B) {}\n", "SE0515", "& B", "&"},
    };
    bool passed = true;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const CompositeSyntaxCase *test = &cases[i];
        FengProgram *program = NULL;
        FengParseError error = {0};
        bool ok = feng_parse_source(test->source, strlen(test->source), "g24_syntax.ff", &program, &error);
        const char *position = test->marker[0] == '\0' ? test->source + strlen(test->source) :
            strstr(test->source + strlen("module g24;\n"), test->marker);
        unsigned line = 1U, column = 1U;
        if (position != NULL) for (const char *p = test->source; p < position; ++p) {
            if (*p == '\n') { ++line; column = 1U; } else ++column;
        }
        if (ok || program != NULL || position == NULL || strcmp(error.code, test->code) != 0 ||
            error.token.line != line || error.token.column != column ||
            error.token.length != strlen(test->token) ||
            (error.token.length > 0U && memcmp(error.token.lexeme, test->token, error.token.length) != 0)) {
            fprintf(stderr, "G24 syntax %zu: expected %s %u:%u %s; got %s %u:%u %.*s\n%s\n",
                i, test->code, line, column, test->token, error.code, error.token.line, error.token.column,
                (int)error.token.length, error.token.lexeme, test->source);
            passed = false;
        }
        feng_program_free(program);
    }
    const char *valid = "module g24;\nspec A {} spec B {} spec Parent: A, B {}\n"
        "spec Inner: i32 | string;\nspec U<T>: T | Inner | T;\nspec Both<T>: A & B & A;\n";
    FengProgram *program = NULL;
    FengParseError error = {0};
    bool ok = feng_parse_source(valid, strlen(valid), "g24_syntax.ff", &program, &error);
    if (!ok || program == NULL || program->declaration_count != 6U ||
        program->declarations[2]->as.spec_decl.form != FENG_SPEC_FORM_OBJECT ||
        program->declarations[4]->as.spec_decl.form != FENG_SPEC_FORM_UNION ||
        program->declarations[4]->as.spec_decl.as.union_form.member_count != 3U ||
        program->declarations[5]->as.spec_decl.form != FENG_SPEC_FORM_INTERSECTION ||
        program->declarations[5]->as.spec_decl.as.intersection_form.member_count != 3U) passed = false;
    feng_program_free(program);
    if (!grouped_match_and_lambda_controls()) passed = false;
    if (!passed) exit(1);
    puts("G24 composite parser matrices passed");
}
