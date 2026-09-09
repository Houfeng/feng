#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Static callable fields must consume the type target before value emission. */
static void static_callable_binding_case(bool value_owner, bool mutable,
                                         void (*compile_c)(const char *)) {
    char source[1600];
    snprintf(source, sizeof source,
        "module g24.static_callable; spec Reader<T>(value:T):i32;\n"
        "func read<T>(value:T):i32 { return 29; }\n"
        "%stype Owner<T> { open static %s callback:Reader<T> = read<T>; }\n"
        "func shared<T>(value:T):i32 { return Owner<T>.callback(value); }\n"
        "func run(value:string):i32 { return Owner<string>.callback(value) + shared<string>(value); }\n",
        value_owner ? "@value " : "", mutable ? "var" : "let");
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "static_callable_binding.ff", &program, &parse));
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
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Reference/value owners and immutable/mutable fields share the same call route. */
void test_static_callable_binding(void (*compile_c)(const char *)) {
    for (size_t value = 0U; value < 2U; ++value) {
        for (size_t mutable = 0U; mutable < 2U; ++mutable) {
            static_callable_binding_case(value != 0U, mutable != 0U, compile_c);
        }
    }
    puts("static callable binding matrices passed");
}
