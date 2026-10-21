#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Compile actual generated C for value, return and throw result paths. */
void test_if_expression_binding_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module expression.binding;\n"
        "@throwable open spec Root {}\n"
        "open fit int: Root {}\n"
        "spec Value: int | string;\n"
        "type Resource { let id: int; }\n"
        "@value type Aggregate { let resource: Resource; }\n"
        "spec Objects: Resource | int;\n"
        "spec Aggregates: Aggregate | int;\n"
        "func direct(v: Value): int { return if v match err: int { err } else { 0 }; }\n"
        "func conjunction(v: Value, w: Value): int { return if v match err: int && "
        "w match rhs: int && rhs > err { err + rhs } else { 0 }; }\n"
        "func shadow(v: Value): int { let err = 7; "
        "let result = if v match err: int { let err = 9; err } else { err }; return result + err; }\n"
        "func mutable(v: Value): int { return if v match var err: int { err += 1; err } else { 0 }; }\n"
        "func matched(v: Value, state: int): int { return match state { "
        "0 { if v match err: int && err > 0 { err } else { 0 } } else { 0 } }; }\n"
        "func returned(v: Value): int { return if v match err: int { return err; } else { 0 }; }\n"
        "func raised(v: Value): int { return if v match err: int { throw err; } else { 0 }; }\n"
        "func bothRaised(v: Value, state: int): int { return match state { "
        "0 { if v match err: int { throw err; } else { throw 0; } } else { 0 } }; }\n"
        "func object(v: Objects, fallback: Resource): Resource { return if v match err: Resource "
        "&& err.id > 0 { err } else { fallback }; }\n"
        "func aggregate(v: Aggregates, fallback: Aggregate): Aggregate { return match 0 { "
        "0 { if v match err: Aggregate { err } else { fallback } } else { fallback } }; }\n"
        "func nested(v: Value): int { return if v match err: int "
        "{ if err > 0 { err } else { 0 } } else if v match err: string { 2 } else { 0 }; }\n";
    FengProgram *program = NULL;
    FengParseError parse = {0};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()
    };
    CHECK(feng_parse_source(source, strlen(source), "if_expression_binding.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
                                                &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "%s %u:%u %s\n", errors[i].code,
                    errors[i].token.line, errors[i].token.column, errors[i].message);
        }
    }
    CHECK(ok && error_count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    puts("if expression binding codegen tests passed (11 functions)");
}
