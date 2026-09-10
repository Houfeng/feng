#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Expand owner markers without changing the scenario between array forms. */
static char *array_static_source(const char *template, const char *suffix) {
    size_t length = strlen(suffix);
    char *source = malloc(strlen(template) * length + 1U);
    CHECK(source != NULL);
    char *out = source;
    for (const char *cursor = template; *cursor != '\0'; ++cursor) {
        if (*cursor == '$') {
            memcpy(out, suffix, length);
            out += length;
        } else {
            *out++ = *cursor;
        }
    }
    *out = '\0';
    return source;
}

/* Compile each equivalent source through Semantic, Codegen and host C checks. */
void test_array_static_parity_codegen(void (*compile_c)(const char *)) {
    const char *template =
        "module parity;type Box<T>{open let value:T;}@value type Value<T>{open let value:T;}"
        "spec Map<T>(v:T):T;spec Rest<T>(v:T...):T;"
        "spec Reader<T>{func read():T;}type Reading:Reader<i64>{func read():i64{return 9000000031;}}"
        "spec ReadMap<T,R>(v:T):R;spec Factory<T>{static func echo(v:T):T;}"
        "fit T$:Factory<T>{"
        "static func echo(v:T):T{return v;}static func keep<U>(v:U):U{return v;}"
        "static func own<U>(v:T,unused:U):T{return v;}"
        "static func read<U:Reader<T>>(v:U):T{return v.read();}"
        "static func first(v:T...):T{return v[0];}"
        "static func box(v:T):Box<T>{return Box<T>{value:v};}"
        "static func value<U>(v:U):Value<U>{return Value<U>{value:v};}"
        "static func array(v:T):T$ {return [v];}"
        "static func tag():i32{return 7;}static func sink(v:T):void{}}"
        "func bind<E>():Map<E>{return E$.echo;}"
        "func same<T>():Map<T>{return T$.echo;}"
        "func reordered<A,E>():Map<E>{return E$.echo;}"
        "func method<X,Y>():Map<Y>{return X$.keep<Y>;}"
        "func own<U>(v:U):U{return U$.own<string>(v,\"unused\");}"
        "func inferred<U>(v:U):U{return U$.own(v,\"unused\");}"
        "func forward<E>(v:E...):E{return E$.first(...v);}"
        "func read<X,Y:Reader<X>>(v:Y):X{return X$.read<Y>(v);}"
        "func specCall<E,A:Factory<E>>(v:E):E{return A.echo(v);}"
        "func specValue<E,A:Factory<E>>():Map<E>{return A.echo;}"
        "func run():i64{"
        "let a=bind<i64>();let b=same<i64>();let c=reordered<i8,i64>();"
        "let d=method<i8,i64>();let e:Map<i64> =i64$.echo;"
        "let f:Map<string> =i8$.keep<string>;let g=specValue<i64,i64$>();"
        "let variadic:Rest<i64> =i64$.first;let items:i64[]=[11,13];"
        "let reader=Reading();let rm:ReadMap<Reading,i64> =i64$.read<Reading>;"
        "let box=i64$.box(9000000033);let value=i8$.value<i64>(box.value);"
        "let nested:Value<i64>[!]=[value];let copy=Value<i64>[!]$.echo(nested);"
        "let array=i64$.array(copy[0].value);i64$.sink(array[0]);"
        "return a(b(c(d(e(g(own<i64>(inferred<i64>(17))))))))+"
        "specCall<i64,i64$>(19)+variadic(23,29)+variadic(...items)+forward<i64>(...items)+"
        "read<i64,Reading>(reader)+i64$.read(reader)+rm(reader)+(i64)i64$.tag();}";
    const char *suffixes[] = {"[]", "[!]"};
    for (size_t form = 0U; form < 2U; ++form) {
        char *source = array_static_source(template, suffixes[form]);
        FengProgram *program = NULL;
        FengParseError parse = {0};
        bool ok = feng_parse_source(source, strlen(source), "parity.ff", &program, &parse);
        if (!ok) fprintf(stderr, "%s %s\n", parse.code, parse.message);
        CHECK(ok);
        const FengProgram *programs[] = {program};
        FengSemanticAnalyzeOptions options = {
            .target = FENG_COMPILE_TARGET_LIB,
            .pointer_size = feng_get_host_pointer_size()
        };
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        ok = feng_semantic_analyze_with_options(
            programs, 1U, &options, &analysis, &errors, &count);
        if (!ok) for (size_t i = 0U; i < count; ++i) {
            fprintf(stderr, "%s %s %s\n", suffixes[form], errors[i].code, errors[i].message);
        }
        CHECK(ok && count == 0U);
        FengCodegenOutput output = {0};
        FengCodegenError error = {0};
        ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
        if (!ok) fprintf(stderr, "%s %s %s\n", suffixes[form], error.code, error.message);
        CHECK(ok && output.c_source != NULL);
        compile_c(output.c_source);
        feng_codegen_error_free(&error);
        feng_codegen_output_free(&output);
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        free(source);
    }
    puts("array static codegen parity matrix passed");
}
