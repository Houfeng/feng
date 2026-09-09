#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Compile one complete case and require fixed-path witness reads, not new metadata channels. */
static void projection_codegen_case(const char *source, void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool ok = feng_parse_source(source, strlen(source), "projection_codegen.ff", &program, &parse);
    if (!ok) fprintf(stderr, "%s %s\n%s", parse.code, parse.message, source);
    CHECK(ok);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n%s", error.code, error.message, source);
    CHECK(ok && output.c_source != NULL);
    const char *c = output.c_source;
    CHECK(strstr(c, "->component_") != NULL);
    CHECK(strstr(c, ".component_") != NULL);
    CHECK(strstr(c, "->reified_spec_view_coercions[") == NULL);
    CHECK(strstr(c, "->reified_union_projections[") == NULL);
    CHECK(strstr(c, "&(const FengGenericParamDescriptor){") == NULL);
    CHECK(strstr(c, "FengSpecSlotWitness__") == NULL);
    compile_c(c);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* All witness origins must preserve the same subject when a view is projected. */
void test_intersection_projection_codegen(void (*compile_c)(const char *)) {
    const char *prefix = "module g24.projection;\n"
        "spec Base<A> { var item: A; func echo(value: A): A; }\n"
        "spec Left<A>: Base<A> { func left(): i32; } spec Right { func right(): i32; }\n"
        "spec Both<A>: Left<A> & Right; spec Extra {} spec Nested<A>: Both<A> & Extra;\n";
    for (size_t value = 0U; value < 2U; ++value) {
        for (size_t nested = 0U; nested < 2U; ++nested) {
            char source[3072];
            snprintf(source, sizeof source, "%s%stype Item<A>: Left<A>, Right, Extra { open var item: A; "
                "func echo(value:A):A { return value; } func left():i32 {return 11;} func right():i32 {return 23;} }\n"
                "func project<A>(v:%s<A>):Base<A> {return (Base<A>)v;}\n"
                "func run(v:Item<string>):Base<string> {let view:%s<string> = v; return project<string>(view);}\n",
                prefix, value ? "@value " : "", nested ? "Nested" : "Both", nested ? "Nested" : "Both");
            projection_codegen_case(source, compile_c);
        }
    }
    char source[3072];
    snprintf(source, sizeof source, "%s"
        "func project<A>(v:Nested<A>):Both<A> { return (Both<A>)v; }\n"
        "func run(v:Nested<string>):Both<string> { return project<string>(v); }\n", prefix);
    projection_codegen_case(source, compile_c);
    snprintf(source, sizeof source, "%s"
        "func project<A>(v:Nested<A>):Base<A> { return (Base<A>)(Both<A>)v; }\n"
        "func run(v:Nested<string>):Base<string> { return project<string>(v); }\n", prefix);
    projection_codegen_case(source, compile_c);
    snprintf(source, sizeof source, "%s"
        "func run():Base<string> { let view:Nested<string>; return (Base<string>)view; }\n", prefix);
    projection_codegen_case(source, compile_c);
    projection_codegen_case("module g24.projection; spec A {} spec B {} spec Repeated:A & A & B;\n"
        "func run(v:Repeated):B {return (B)v;}\n", compile_c);
    projection_codegen_case("module g24.projection; spec A {} spec B {} spec C:A & B;\n"
        "spec Outer:C & C; func run(v:Outer):C {return (C)v;}\n", compile_c);
    puts("intersection explicit projection codegen matrices passed");
}
