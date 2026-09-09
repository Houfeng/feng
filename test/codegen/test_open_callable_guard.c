#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* The unreachable open default must be valid C independently of its result ABI. */
static void open_callable_guard_case(const char *result, void (*compile_c)(const char *)) {
    char source[1024];
    snprintf(source, sizeof source,
        "module g24.callable_guard; type Pair(i32,string); type Ref {} "
        "@value type Value {open var text:string;} "
        "spec Fn<T>(value:T):%s; type Holder<T> {open let callback:Fn<T>;}\n", result);
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "open_callable_guard.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n%s", error.code, error.message, source);
    CHECK(ok && output.c_source != NULL);
    const char *guard = strstr(output.c_source,
        "feng_panic(\"open generic callable default invoked without a concrete descriptor\");");
    CHECK((guard != NULL) == (strcmp(result, "void") != 0));
    if (guard != NULL) {
        const char *end = strchr(guard, '}');
        const char *dummy = strstr(guard, "return NULL;");
        CHECK(end != NULL && (dummy == NULL || dummy > end));
    }
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Fixed scalar/pointer/aggregate results and erased/void controls share one rule. */
void test_open_callable_guard(void (*compile_c)(const char *)) {
    const char *results[] = {"i32", "bool", "f64", "string", "Ref", "Pair", "Value", "T", "void"};
    for (size_t i = 0U; i < sizeof results / sizeof results[0]; ++i) {
        open_callable_guard_case(results[i], compile_c);
    }
    puts("open callable guard return ABI matrices passed");
}
