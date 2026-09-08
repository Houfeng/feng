#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/** Compile an isolated source through Semantic, inspect its lowering and compile the C. */
static void view_program(const char *source, bool table, bool boxed, void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "g24_views.ff", &program, &parse);
    if (!parsed) fprintf(stderr, "%s %s\n%s", parse.code, parse.message, source);
    CHECK(parsed);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n%s", error.code, error.message, source);
    CHECK(ok && output.c_source != NULL);
    const char *c = output.c_source;
    CHECK((strstr(c, "->reified_spec_view_coercions[") != NULL) == table);
    CHECK((strstr(c, "static const FengSpecCoercionDescriptor ") != NULL) == table);
    CHECK((strstr(c, "->box_descriptor);") != NULL) == boxed);
    CHECK(strstr(c, "&(const FengFunctionDescriptor){") == NULL);
    CHECK(strstr(c, "FengSpecCoercionDescriptor *_coercion") == NULL);
    CHECK(strstr(c, "->convert(") == NULL);
    CHECK(strstr(c, "->reified_union_projections[") == NULL);
    compile_c(c);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/** Object/intersection targets, open values/references and every closed payload share the pipeline. */
void test_g24_spec_view_codegen(void (*compile_c)(const char *)) {
    const char *targets[] = {"Field<A>", "Both<A>", "Tag"};
    const char *actuals[] = {"i32", "string", "Pair", "Value<string>"};
    for (size_t reference = 0U; reference < 2U; ++reference) {
        for (size_t target = 0U; target < 3U; ++target) {
            for (size_t actual = 0U; actual < 4U; ++actual) {
                char source[2048], result[128];
                snprintf(result, sizeof(result), target == 0U ? "Field<%s>" : target == 1U ? "Both<%s>" : "%s", target == 2U ? "Tag" : actuals[actual]);
                snprintf(source, sizeof(source), "module g24;\n"
                    "spec Field<A> { var item: A; } spec Tag { func tag(): i32; } spec Both<A>: Field<A> & Tag;\n"
                    "type Pair(i32, string);\n"
                    "%stype Value<A>: Field<A>, Tag { open var item: A; func tag(): i32 { return 37; } }\n"
                    "func view<A>(source: Value<A>): %s { return source; }\n"
                    "func run(source: Value<%s>): %s { return view<%s>(source); }\n",
                    reference ? "" : "@value ", targets[target], actuals[actual], result, actuals[actual]);
                view_program(source, true, !reference, compile_c);
            }
        }
    }
    view_program("module g24; spec Field<A> { var item: A; } spec Tag {} spec Both<A>: Field<A> & Tag;\n"
        "@value type Value<A>: Field<A>, Tag { open var item: A; }\n"
        "type Owner<A> { open let field: Both<A> = Value<A> {}; func convert<B>(v: Value<B>): Both<B> { return v; } }\n"
        "func run(): Both<i32> { let owner = Owner<string>{}; return owner.convert<i32>(Value<i32>{item:37}); }\n",
        true, true, compile_c);
    view_program("module g24; spec A { var item: string; } spec B {} spec Both: A & B;\n"
        "@value type Value: A, B { open var item: string; }\n"
        "spec Child: A {} func parent(v: Child): A { return v; }\n"
        "func closed(v: Value): Both { return v; }\n"
        "func identity<T>(v: T): T { return v; } func run(v: Value): Both { return identity<Both>(closed(v)); }\n",
        false, false, compile_c);
    /* Complete spec actuals select exact closed requirements in every
     * descriptor-producing entrance, not only direct generic functions. */
    const char *calls[] = {
        "return use<string, Both<string>>(value);",
        "let owner = Store<string, Both<string>>(value); return owner.read();",
        "let callback: Reader = use<string, Both<string>>; return callback(value);",
        "let owner = Methods<string>{}; return owner.read<Both<string>>(value);",
        "return Methods<string>.read_static<Both<string>>(value);",
        "let owner = Methods<string>{}; let callback: Reader = owner.read<Both<string>>; return callback(value);",
        "let callback: Reader = Methods<string>.read_static<Both<string>>; return callback(value);"
    };
    for (size_t index = 0U; index < sizeof calls / sizeof calls[0]; ++index) {
        char source[4096];
        snprintf(source, sizeof source,
            "module g24; spec Field<A> { var item: A; func echo(value: A): A; }\n"
            "spec Tag { func tag(): i32; } spec Both<A>: Field<A> & Tag;\n"
            "spec Reader(value: Both<string>): string;\n"
            "func use<A, T: Both<A>>(value: T): A { value.item = value.echo(value.item); return value.item; }\n"
            "type Store<A, T: Both<A>> { open let value: T; func Store(value: T) { self.value = value; }\n"
            "func read(): A { return self.value.echo(self.value.item); } }\n"
            "type Methods<A> { func read<T: Both<A>>(value: T): A { return value.echo(value.item); }\n"
            "static func read_static<T: Both<A>>(value: T): A { return value.echo(value.item); } }\n"
            "func run(value: Both<string>): string { %s }\n", calls[index]);
        view_program(source, false, false, compile_c);
    }
    view_program("module g24; spec Read<A> { func read(): A; } spec Tag {}\n"
        "spec Both<A>: Read<A> & Tag; spec Child<A>: Read<A>, Tag {}\n"
        "func use<A, T: Both<A>>(value: T, hint: A): A { return value.read(); }\n"
        "func run(value: Child<string>): string { return use(value, \"hint\"); }\n",
        false, false, compile_c);
    puts("G24 spec view codegen matrices passed");
}
