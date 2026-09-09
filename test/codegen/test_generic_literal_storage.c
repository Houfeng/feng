#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Inspect real shared-body stores without any spec constraint or projection. */
static void literal_storage_check(const char *source, bool reified,
                                  void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "generic_literal_storage.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    CHECK(strstr(output.c_source, "->reified_constraint_projection_descriptors") == NULL);
    CHECK(strstr(output.c_source, "= (void *)(_p_value)") == NULL);
    CHECK(strstr(output.c_source, "Nested__G__T__CTX__T__release_children") == NULL);
    CHECK(strstr(output.c_source, "&Feng__literal__storage__ValueStore__G__T__CTX__T__aggregate_desc") == NULL);
    if (reified) {
        CHECK(strstr(output.c_source, "_ginit_dst") != NULL);
        CHECK(strstr(output.c_source, "->reified_field_offsets[0]") != NULL);
        CHECK(strstr(output.c_source, "->reified_field_offsets[1]") != NULL);
        CHECK(strstr(output.c_source, "->reified_field_offsets[2]") != NULL);
        CHECK(strstr(output.c_source, "memcpy(_ginit_dst") != NULL);
        CHECK(strstr(output.c_source, "feng_aggregate_assign(_ginit_dst") != NULL);
        CHECK(strstr(output.c_source, "feng_retain(_new_value)") != NULL);
        CHECK(strstr(output.c_source, "feng_release(_old_value)") != NULL);
    } else {
        CHECK(strstr(output.c_source, "_ginit_dst") == NULL);
        CHECK(strstr(output.c_source, "->reified_field_offsets[") == NULL);
    }
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Owner/method slots and fixed-layout controls exercise the same store entry. */
void test_generic_literal_storage_codegen(void (*compile_c)(const char *)) {
    literal_storage_check(
        "module literal.storage;\n"
        "type Store<T>{open let before:i64;open let value:T;open let after:string;}\n"
        "type Words(i64,i64,i64);\n"
        "@value type ValueStore<T>{open let value:T;open let after:string;}\n"
        "type Nested<T>{open let value:ValueStore<T>;}\n"
        "func nest<T>(value:ValueStore<T>):Nested<T>{return Nested<T>{value:value};}\n"
        "func save<T>(value:T):Store<T>{return Store<T>{after:\"after\",value:value,before:7};}\n"
        "func second<A,B>(a:A,value:B):Store<B>{return Store<B>{before:11,value:value,after:\"second\"};}\n"
        "type Factory<A>{open let seed:A;"
        "func save():Store<A>{return Store<A>{before:13,value:self.seed,after:\"owner\"};}"
        "func mixed<U>(value:U):Store<U>{return Store<U>{before:17,value:value,after:\"method\"};}}\n"
        "func run():i64{let words:Words=(19,23,29);let stored=save<Words>(words);"
        "let factory=Factory<string>{seed:\"seed\"};let owner=factory.save();"
        "let method=factory.mixed<i64>(31);let reordered=second<i8,string>(1,\"text\");"
        "let nested=nest<string>(ValueStore<string>{value:\"nested\",after:\"after\"});"
        "return stored.value.item3+method.value;}\n", true, compile_c);
    literal_storage_check(
        "module literal.fixed;type Store{open let before:i64;open let value:i64;open let after:string;}"
        "func run():Store{return Store{before:1,value:2,after:\"fixed\"};}", false, compile_c);
    puts("generic literal storage layout, copy protocol and fixed-layout controls passed");
}
