#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Closed callable arguments must survive both admission and coercion checks. */
static void callable_instance_case(const char *expression, bool valid, size_t entry) {
    char source[2600];
    const char *headers[] = {"func run(value:Reader<i32>):i32",
        "func run<C:Reader<i32>>(value:C):i32", "func run<T:View>(value:T):i32"};
    snprintf(source, sizeof(source),
        "module g24.callable_instance; spec Reader<A>(value:A):A;\n"
        "spec View {var callback:Reader<i32>;} func number():i32{return 1;}\n"
        "%s {return %s(%s);}\n", headers[entry], entry == 2U ? "value.callback" : "value", expression);
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "callable_instance.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != valid) {
        fprintf(stderr, "%s", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(ok == valid);
    CHECK(valid ? count == 0U : count > 0U);
    for (size_t i = 0U; i < count; ++i) {
        CHECK(strncmp(errors[i].code, "AE", 2U) == 0);
        CHECK(strcmp(errors[i].path, "callable_instance.ff") == 0);
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Ordinary/constrained callables and constrained fields pair positive and
 * negative scalar, if, match and try arguments with the same closed result. */
void test_callable_instance_arguments(void) {
    const char *positive[] = {"0", "if true {0} else {1}",
        "match 0 {0 {1} else {2}}", "try number() catch error:string {2}"};
    const char *negative[] = {"\"wrong\"", "if true {\"a\"} else {\"b\"}",
        "match 0 {0 {\"a\"} else {\"b\"}}", "try \"wrong\" catch error:string {\"also wrong\"}"};
    for (size_t entry = 0U; entry < 3U; ++entry) for (size_t i = 0U; i < 4U; ++i) {
        callable_instance_case(positive[i], true, entry);
        callable_instance_case(negative[i], false, entry);
    }
    puts("callable instance argument semantic matrices passed");
}
