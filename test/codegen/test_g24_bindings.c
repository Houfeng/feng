#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/** Count exact generated operations within one independently compiled fixture. */
static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0U;
    while ((text = strstr(text, needle)) != NULL) { ++count; text += strlen(needle); }
    return count;
}

/** Inspect each actual constructor definition, excluding declarations and unrelated defaults. */
static void check_materializers(const char *c, bool expected) {
    const char *p = c;
    size_t found = 0U;
    while ((p = strstr(p, ".materialize = ")) != NULL) {
        p += strlen(".materialize = ");
        if (strncmp(p, "NULL", 4U) == 0) continue;
        const char *end = p;
        while (isalnum((unsigned char)*end) || *end == '_') ++end;
        CHECK(end > p);
        char signature[1024];
        snprintf(signature, sizeof(signature), "static void %.*s(const void *_source, void *_result) {", (int)(end - p), p);
        const char *body = strstr(c, signature);
        CHECK(body != NULL);
        const char *close = strstr(body, "\n}\n");
        CHECK(close != NULL);
        char *definition = strndup(body, (size_t)(close - body));
        CHECK(definition != NULL);
        CHECK(occurrences(definition, "memcpy(_result, &_value, sizeof(_value))") == 1U);
        CHECK(strstr(definition, "default_zero_init") == NULL);
        CHECK(strstr(definition, "feng_generic_value_size") == NULL);
        CHECK(strstr(definition, "reified_union_projections") == NULL);
        free(definition);
        ++found;
    }
    CHECK((found > 0U) == expected);
}

/** Full front-end plus host C compilation; no string assertion can replace valid C. */
static void binding_program(const char *source, bool projection, bool materialize,
    bool storage, void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    if (!feng_parse_source(source, strlen(source), "g24_binding_codegen.ff", &program, &parse)) {
        fprintf(stderr, "%s\n%s\n", parse.message, source); CHECK(false);
    }
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) {
        fprintf(stderr, "%s\n", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n%s\n", error.code, error.message, source);
    CHECK(ok && output.c_source != NULL);
    const char *c = output.c_source;
    CHECK((strstr(c, "->reified_union_projections[") != NULL) == projection);
    CHECK((strstr(c, "_uowned") != NULL) == storage);
    CHECK(strstr(c, "&(const FengFunctionDescriptor){") == NULL);
    CHECK(strstr(c, "FengUnionProjection *_projection") == NULL);
    check_materializers(c, materialize);
    if (projection) CHECK(strstr(c, "static const FengUnionProjection ") != NULL);
    if (projection && !storage) {
        CHECK(strstr(c, ".materialize(_umt") == NULL);
        CHECK(strstr(c, "_usize") == NULL);
    }
    if (storage && strstr(source, "Choice<A>") == NULL) {
        CHECK(strstr(c, "_usize") == NULL);
        CHECK(strstr(c, "_urad") == NULL);
    }
    compile_c(c);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/** Every match emitter is tested with actual naked, child, whole and missed values. */
static void binding_entrance_matrix(void (*compile_c)(const char *)) {
    const char *bodies[] = {
        "return match value { item: Inner -> i32 { item } else { -1 } };",
        "match value { var item: Inner -> i32 { item += 1; return item; } else { return -1; } }",
        "let unused: i32 = match value { item: Inner -> i32 { return item; } else { return -1; } }; return unused;",
        "if value match var item: Inner -> i32 && item > 0 { item += 2; return item; } return -1;"
    };
    const char *actuals[] = {"i32", "Inner", "Outer", "string", "bool"};
    for (size_t form = 0U; form < sizeof(bodies) / sizeof(bodies[0]); ++form) {
        for (size_t actual = 0U; actual < sizeof(actuals) / sizeof(actuals[0]); ++actual) {
            char source[2048];
            snprintf(source, sizeof(source), "module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
                "func read<T: Outer>(value: T): i32 { %s }\n"
                "func run(value: %s): i32 { return read<%s>(value); }\n", bodies[form], actuals[actual], actuals[actual]);
            binding_program(source, true, false, false, compile_c);
        }
    }
    for (size_t actual = 0U; actual < sizeof(actuals) / sizeof(actuals[0]); ++actual) {
        char source[1536];
        snprintf(source, sizeof(source), "module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
            "func read<T: Outer>(value: T): Inner { return match value { item: Inner { item } else { 0 } }; }\n"
            "func run(value: %s): Inner { return read<%s>(value); }\n", actuals[actual], actuals[actual]);
        binding_program(source, true, actual == 0U || actual == 3U, true, compile_c);
    }
}

/** Nonshared counterparts must remain direct tag/payload code with no projection reads. */
static void binding_ordinary_controls(void (*compile_c)(const char *)) {
    binding_program("module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
        "func read(value: Outer): i32 { let unused: i32 = match value {"
        " item: Inner -> i32 { return item; } else { return -1; } }; return unused; }\n"
        "func subset(value: Outer): i32 { return match value { item: Inner, bool { read(item) } else { -1 } }; }\n",
        false, false, false, compile_c);
    binding_program("module g24;\nspec Reader(): i32;\nspec Other(): i32;\n"
        "func id<T>(value: T): T { return value; }\n"
        "func read(value: Other): Reader { return id<Reader>((Reader)value); }\n",
        false, false, false, compile_c);
}

/** Fixed and reified bound results plus escaping callbacks must compile independently. */
static void binding_capture_controls(void (*compile_c)(const char *)) {
    binding_program("module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\nspec Reader(): Inner;\n"
        "func capture<T: Outer>(value: T): Reader { match value { item: Inner { return () { return item; }; }"
        " else { return () { return 0; }; } } }\nfunc run(): Reader { return capture<i32>(31); }\n",
        true, true, true, compile_c);
    binding_program("module g24;\nspec Choice<A>: A | bool;\nspec Outer<A>: Choice<A> | f64;\n"
        "func read<A, T: Outer<A>>(value: T): Choice<A> { return match value { item: Choice<A> { item } else { false } }; }\n"
        "@value type V { open let text: string; }\nfunc run(value: V): Choice<V> { return read<V, V>(value); }\n",
        true, true, true, compile_c);
}

/** Open union subjects retain actual layout through every match entrance. */
static void binding_open_subjects(void (*compile_c)(const char *)) {
    const char *bodies[] = {
        "return match value { item: A { item } else { fallback } };",
        "match value { item: A { return item; } else { return fallback; } }",
        "let unused: A = match value { item: A { return item; } else { return fallback; } }; return unused;",
        "if value match item: A { return item; } return fallback;"
    };
    const char *actuals[] = {"i32", "string", "bool"};
    for (size_t body = 0U; body < 4U; ++body) for (size_t actual = 0U; actual < 3U; ++actual) {
        char source[2048];
        snprintf(source, sizeof(source), "module g24;\nspec Choice<A>: A | bool;\n"
            "func read<A>(value: Choice<A>, fallback: A): A { %s }\n"
            "func run(value: Choice<%s>, fallback: %s): %s { return read<%s>(value, fallback); }\n",
            bodies[body], actuals[actual], actuals[actual], actuals[actual], actuals[actual]);
        binding_program(source, true, false, true, compile_c);
    }
    binding_program("module g24;\ntype Box<A> {}\nspec Choice<A>: Box<A> | bool;\n"
        "func stored<A>(value: Choice<A>): Choice<A> { let copy = value; let values: Choice<A>[!] = [copy]; return values[0]; }\n"
        "func read<A>(value: Choice<A>): bool { return match value { item: Box<A> { true } else { false } }; }\n"
        "func run(value: Choice<i32>): bool { return read<i32>(stored<i32>(value)); }\n",
        true, false, false, compile_c);
}

/** Public entry for new G24 source-to-C coverage without changing old assertions. */
void test_g24_projection_bindings(void (*compile_c)(const char *)) {
    binding_entrance_matrix(compile_c);
    binding_ordinary_controls(compile_c);
    binding_capture_controls(compile_c);
    binding_open_subjects(compile_c);
    puts("G24 projection binding codegen matrices passed");
}
