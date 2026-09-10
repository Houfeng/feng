#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* A successful emitter is insufficient: every legal fixture must also pass
 * the host C compiler, including nominal tags and descriptor pointer types. */
static void check_valid_program(const char *source,
                                void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "valid_codegen.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(),
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(
        programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) {
        for (size_t i = 0U; i < count; ++i)
            fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    /* Address-form parameters denote the value itself, not a pointer slot. */
    CHECK(strstr(output.c_source, "memcpy(_out, (void *)&(value),") == NULL);
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Direct and callable-value paths close T, arrays, reference and value owners.
 * Forwarding uses different parameter names to expose accidental slot reuse. */
static void check_builtin_generic_methods(void (*compile_c)(const char *)) {
    check_valid_program(
        "module valid.methods;\n"
        "type Box<T>{open let value:T;}\n"
        "@value type Value<T>{open let value:T;}\n"
        "type Words(i64,i64,i64);\n"
        "spec Copy<T>(value:T):T;\n"
        "spec Pack<T>(value:T):Box<T>;\n"
        "spec PackValue<T>(value:T):Value<T>;\n"
        "spec PackArray<T>(value:T):T[];\n"
        "spec CopyValue<T>(value:Value<T>):Value<T>;\n"
        "fit i32 {\n"
        "static func keep<T>(value:T):T{return value;}\n"
        "static func array<T>(value:T):T[]{return [value];}\n"
        "static func object<T>(value:T):Box<T>{return Box<T>{value:value};}\n"
        "static func value<T>(value:T):Value<T>{return Value<T>{value:value};}\n"
        "static func pass<T>(value:Value<T>):Value<T>{return value;}\n"
        "static func read<T>(value:Box<T>):T{return value.value;}\n"
        "static func second<A,B>(first:A,value:B):Box<B>{return Box<B>{value:value};}\n"
        "}\n"
        "func relay<U>(value:U):Value<U>{let f:Copy<U> =i32.keep<U>;"
        "return i32.value<U>(f(value));}\n"
        "func run():i64{\n"
        "let direct=i32.keep<i8>(7);let inferred=i32.keep(direct);"
        "let wide=i32.keep<i64>(9000000001);let text=i32.keep<string>(\"text\");"
        "let words:Words=(11,13,17);let actual=i32.keep<Words>(words);"
        "let array=i32.array<Words>(words);let object=i32.object<Words>(words);"
        "let value=i32.value<Words>(words);let copied=i32.pass<Words>(value);"
        "let read=i32.read<Words>(object);let second=i32.second<i8,Words>(1,words);"
        "let f:Copy<i64> =i32.keep<i64>;let p:Pack<Words> =i32.object<Words>;"
        "let v:PackValue<Words> =i32.value<Words>;let a:PackArray<Words> =i32.array<Words>;"
        "let c:CopyValue<Words> =i32.pass<Words>;"
        "let nested=relay<Value<Words>>(value);"
        "return f(wide)+p(words).value.item3+v(words).value.item2+"
        "a(words)[0].item1+c(value).value.item1+nested.value.value.item3; }\n",
        compile_c);

    check_valid_program(
        "module valid.instance;\n"
        "type Box<T>{open let value:T;}\n"
        "@value type Value<T>{open let value:T;}\n"
        "fit A[]{\n"
        "func pick<B>(value:B):Value<B>{return Value<B>{value:value};}\n"
        "func own<B>(value:B):Box<A>{return Box<A>{value:self[0]};}\n"
        "}\n"
        "fit string{static func keep<T>(value:T):T{return value;}"
        "func choose<T>(value:T):T{return value;}}\n"
        "func relay<X,Y>(items:X[],value:Y):Value<Y>{return items.pick<Y>(value);}\n"
        "func run():i64{let items:i32[]=[3];let one=items.pick<i64>(9000000001);"
        "let two=items.pick(7);let own=items.own<string>(\"unused\");"
        "let forwarded=relay<i32,i64>(items,11);"
        "return one.value+forwarded.value+\"text\".choose<i64>(string.keep<i64>(13));}\n",
        compile_c);
}

/* Append generated source with an explicit bound so chain sizes are easy to
 * change without relying on a fixed compiler or test-buffer branch limit. */
static void append_valid_source(char *source, size_t capacity, size_t *length,
                                 const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(source + *length, capacity - *length, format, arguments);
    va_end(arguments);
    CHECK(written >= 0 && (size_t)written < capacity - *length);
    *length += (size_t)written;
}

/* Exercise both sides of the old limit and a longer mixed chain. The second
 * if also checks that wrapper metadata was restored to the enclosing scope. */
static void check_long_if_chain(size_t count, bool mixed,
                                 void (*compile_c)(const char *)) {
    size_t capacity = 1024U + count * 128U;
    size_t length = 0U;
    char *source = calloc(capacity, 1U);
    CHECK(source != NULL);
    append_valid_source(source, capacity, &length,
        "module valid.flow;func probe(items:i32[]):bool{return items[0]==0;}"
        "func run(value:i32):i32{var result:i32=0;if probe([1]){result=1;}\n");
    for (size_t i = 0U; i < count; ++i) {
        append_valid_source(source, capacity, &length,
            mixed && i % 2U == 0U
                ? "else if value==%zu {result=2;}\n"
                : "else if probe([%zu]) {result=3;}\n", i + 1U);
    }
    append_valid_source(source, capacity, &length,
        "else{result=4;}if probe([0]){result+=5;}return result;}\n");
    check_valid_program(source, compile_c);
    free(source);
}

/* Independent coverage for the reviewed legal-program codegen failures. */
void test_valid_program_codegen(void (*compile_c)(const char *)) {
    check_builtin_generic_methods(compile_c);
    const size_t boundaries[] = {63U, 64U, 65U, 128U};
    for (size_t i = 0U; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i)
        check_long_if_chain(boundaries[i], false, compile_c);
    check_long_if_chain(160U, true, compile_c);
    puts("valid generic builtin methods and unbounded if wrappers passed");
}
