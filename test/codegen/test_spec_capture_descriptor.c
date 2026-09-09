#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Open fixed-layout captures must use closed lifecycle metadata, not an open symbol. */
static void capture_descriptor_case(const char *source, bool default_init,
                                    void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool ok = feng_parse_source(source, strlen(source), "capture_descriptor.ff", &program, &parse);
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
    const char *operation = strstr(output.c_source, default_init
        ? "feng_aggregate_default_zero_init(&_l_saved_" : "feng_aggregate_retain(&_l_saved_");
    CHECK(operation != NULL);
    const char *end = strchr(operation, ';');
    const char *dependency = strstr(operation, "->reified_agg_deps[");
    CHECK(end != NULL && dependency != NULL && dependency < end);
    CHECK(strstr(output.c_source, "->value, &FengSpecAgg__") == NULL);
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Function- and owner-generic captures cover explicit/default and let/var bindings. */
void test_spec_capture_descriptor(void (*compile_c)(const char *)) {
    for (size_t owner = 0U; owner < 2U; ++owner) {
        for (size_t mutable = 0U; mutable < 2U; ++mutable) {
            for (size_t default_init = 0U; default_init < 2U; ++default_init) {
                char source[2048];
                snprintf(source, sizeof source,
                    "module g24.capture; spec View<A> {var item:A;} spec Reader<A>():View<A>;\n"
                    "%sfunc capture%s(value:View<A>):Reader<A> { %s saved%s; return () {return saved;}; }%s\n"
                    "func run(owner:Owner<string>, value:View<string>):Reader<string> {return %s(value);}\n",
                    owner ? "type Owner<A> {" : "type Owner<A> {} ", owner ? "" : "<A>",
                    mutable ? "var" : "let", default_init ? ":View<A>" : " = value",
                    owner ? "}" : "", owner ? "owner.capture" : "capture<string>");
                capture_descriptor_case(source, default_init != 0U, compile_c);
            }
        }
    }
    puts("open spec capture descriptor matrices passed");
}
