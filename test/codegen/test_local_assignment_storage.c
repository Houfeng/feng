#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Emit one ordinary program; these cases deliberately contain no intersection casts. */
static void assignment_compile(const char *source, void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "local_assignment_storage.ff", &program, &parse));
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
    CHECK(ok && output.c_source != NULL && strstr(output.c_source, "(null)") == NULL);
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Every small local-count boundary exercises realloc during RHS/store materialization. */
void test_local_assignment_storage_codegen(void (*compile_c)(const char *)) {
    char source[65536];
    size_t used = (size_t)snprintf(source, sizeof source,
        "module assignment.storage; spec View { var text:string; }\n"
        "@value type Item:View {open var text:string;} type Ref {open var text:string;}\n"
        "func read(v:Item):i32 {return 7;} func identity<T>(v:T):T {return v;}\n");
    for (size_t pads = 0U; pads < 34U; ++pads) {
        used += (size_t)snprintf(source + used, sizeof source - used,
            "func concrete%zu(v:View) {var target:View=v; var number:i32=3; var ref=Ref{};\n", pads);
        for (size_t i = 0U; i < pads; ++i)
            used += (size_t)snprintf(source + used, sizeof source - used, "let pad%zu=%zu;\n", i, i);
        used += (size_t)snprintf(source + used, sizeof source - used,
            "target=Item{text:\"new\"}; number+=read(Item{text:\"rhs\"}); ref=Ref{text:\"new\"};}\n"
            "func generic%zu<T>(first:T,second:T):T {var target=first;\n", pads);
        for (size_t i = 0U; i < pads; ++i)
            used += (size_t)snprintf(source + used, sizeof source - used, "let pad%zu=%zu;\n", i, i);
        used += (size_t)snprintf(source + used, sizeof source - used,
            "target=identity<T>(second); return target;}\n"
            "func closed%zu(v:Item):Item {return generic%zu<Item>(v,v);}\n", pads, pads);
        CHECK(used < sizeof source);
    }
    assignment_compile(source, compile_c);
    assignment_compile("module assignment.capture; spec Reader():i32; type Holder {open var reader:Reader;}\n"
        "func install(holder:Holder, reader:Reader):i32 {holder.reader=reader;return 7;}\n"
        "func run():i32 {var first:i32=3; let a=Holder{}; first=install(a,(){return first;});\n"
        "var second:i32=5; let b=Holder{}; second+=install(b,(){return second;}); return a.reader()+b.reader();}\n",
        compile_c);
    puts("local assignment storage growth and capture matrices passed");
}
