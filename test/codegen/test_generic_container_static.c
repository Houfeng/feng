#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Independently verify Semantic selection, emission and host-C validity. */
static void container_codegen(const char *const *sources, size_t count,
                               void (*compile_c)(const char *)) {
    FengProgram **programs = calloc(count, sizeof(*programs));
    CHECK(programs != NULL);
    for (size_t i = 0U; i < count; ++i) {
        FengParseError error = {0};
        bool ok = feng_parse_source(sources[i], strlen(sources[i]), "container.ff", &programs[i], &error);
        if (!ok) fprintf(stderr, "%s %u:%u %s\n", error.code, error.token.line, error.token.column, error.message);
        CHECK(ok);
    }
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze_with_options((const FengProgram *const *)programs,
        count, &options, &analysis, &errors, &error_count);
    if (!ok) for (size_t i = 0U; i < error_count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
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
    for (size_t i = 0U; i < count; ++i) feng_program_free(programs[i]);
    free(programs);
}

/* Cover renamed contexts, all static entry forms and declaration provenance. */
void test_generic_container_static_codegen(void (*compile_c)(const char *)) {
    const char *array_source =
        "module container.array;\n"
        "type Box<T>{open let value:T;}@value type Value<T>{open let value:T;}\n"
        "spec Map<T>(value:T):T;spec Factory<T>{static func echo(value:T):T;}\n"
        "fit T[]:Factory<T>{static func echo(value:T):T{return value;}"
        "static func keep<U>(value:U):U{return value;}"
        "static func own<U>(value:T,unused:U):T{return value;}"
        "static func size():i32{return 3;}static func sink(value:T):void{}"
        "static func first(values:T...):T{return values[0];}"
        "static func box(value:T):Box<T>{return Box<T>{value:value};}"
        "static func val<U>(value:U):Value<U>{return Value<U>{value:value};}}\n"
        "fit T[!]:Factory<T>{static func echo(value:T):T{return value;}}\n"
        "func bind<E>():Map<E>{return E[].echo;}"
        "func bindMethod<X,Y>():Map<Y>{return X[].keep<Y>;}"
        "func nested<E>():Map<Value<E>[!]>{return Value<E>[!][].echo;}"
        "func relay<U>(value:U):U{return U[].own<string>(value,\"unused\");}"
        "func inferred<U>(value:U):U{return U[].own(value,\"unused\");}"
        "func call<E,A:Factory<E>>(value:E):E{return A.echo(value);}"
        "func witness<E,A:Factory<E>>():Map<E>{return A.echo;}"
        "func run():i64{let closed:Map<i64> =i64[].echo;let method:Map<string> =i32[].keep<string>;"
        "let bound=bind<i64>();let explicit=bindMethod<i8,i64>();let shared=witness<i64,i64[]>();"
        "let writable:Map<i64> =i64[!].echo;let box=i64[].box(9000000001);"
        "let aggregate=i32[].val<i64>(box.value);let arr:Value<i64>[!]=[aggregate];"
        "let n=nested<i64>();i64[].sink(n(arr)[0].value);"
        "return (i64)i32[].size()+closed(bound(explicit(shared(writable(relay<i64>(inferred<i64>(7)))))))+"
        "call<i64,i64[!]>(i64[].first(11,13));}\n";
    container_codegen(&array_source, 1U, compile_c);

    const char *nominal_source =
        "module container.list;type List<T>{}spec Map<T>(value:T):T;"
        "fit List<T>{static func echo(value:T):T{return value;}"
        "static func keep<U>(value:U):U{return value;}"
        "static func own<U>(value:T,unused:U):T{return value;}}"
        "func bind<E>():Map<E>{return List<E>.echo;}"
        "func reordered<A,E>():Map<E>{return List<E>.echo;}"
        "func method<U>():Map<i64>{return List<U>.keep<i64>;}"
        "func relay<U>(value:U):U{return List<U>.own<string>(value,\"unused\");}"
        "func inferred<U>(value:U):U{return List<U>.own(value,\"unused\");}"
        "func run():i64{let a=bind<i64>();let b=method<string>();let c=reordered<i8,i64>();"
        "return a(b(c(relay<i64>(inferred<i64>(9000000001)))));}";
    container_codegen(&nominal_source, 1U, compile_c);

    const char *modules[] = {
        "open module container.types;open type Container<T>{}open spec Map<T>(value:T):T;",
        "open module container.extensions;import container.types;"
        "open fit Container<T>{static func echo(value:T):T{return value;}"
        "static func own<U>(value:T,unused:U):T{return value;}}",
        "module container.consumer;import container.types;import container.extensions;"
        "type Payload<E>{open let value:E;}"
        "func bind<U>():Map<Payload<U>>{return Container<Payload<U>>.echo;}"
        "func relay<U>(value:Payload<U>):Payload<U>{return Container<Payload<U>>.own<string>(value,\"unused\");}"
        "func run():i64{let mapper=bind<i64>();return relay<i64>(mapper(Payload<i64>{value:9000000003})).value;}"
    };
    container_codegen(modules, sizeof modules / sizeof *modules, compile_c);
    const char *constraint_source =
        "module container.constraint;spec Reader<A>{func read():A;}"
        "type Reading:Reader<i64>{func read():i64{return 9000000001;}}"
        "spec Map<T,R>(value:T):R;"
        "fit T[]{static func read<U:Reader<T>>(value:U):T{return value.read();}}"
        "func relay<X,Y:Reader<X>>(value:Y):X{return X[].read<Y>(value);}"
        "func run():i64{let reader=Reading();let f:Map<Reading,i64> =i64[].read<Reading>;"
        "return i64[].read<Reading>(reader)+f(reader)+relay<i64,Reading>(reader);}";
    container_codegen(&constraint_source, 1U, compile_c);
    puts("generic container static C and context matrices passed");
}
