#include "parser/parser.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/** Assert the entire diagnostic set, source path, token and exact location. */
static void binding_source(const char *source, const char *code, const char *marker, const char *token) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "g24_bindings.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    size_t expected_count = code == NULL ? 0U : 1U;
    if (code != NULL) for (const char *p = code; *p != '\0'; ++p) if (*p == ',') ++expected_count;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != (code == NULL) || count != expected_count ||
        (code != NULL && count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "G24 binding expected %s\n%s\n", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %s\n", errors[i].code,
            errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    CHECK(ok == (code == NULL));
    CHECK(count == expected_count);
    if (code != NULL) {
        const char *at = strstr(source, marker);
        unsigned line = 1U, column = 1U;
        CHECK(at != NULL);
        for (const char *p = source; p < at; ++p) { if (*p == '\n') { ++line; column = 1U; } else ++column; }
        for (size_t i = 0U; i < count; ++i) {
            bool found = false;
            for (const char *p = code; *p != '\0';) {
                const char *end = strchr(p, ',');
                size_t length = end != NULL ? (size_t)(end - p) : strlen(p);
                if (strlen(errors[i].code) == length && memcmp(errors[i].code, p, length) == 0) found = true;
                p += length + (end != NULL ? 1U : 0U);
            }
            CHECK(found);
            size_t actual_frequency = 0U, expected_frequency = 0U;
            for (size_t j = 0U; j < count; ++j) if (strcmp(errors[j].code, errors[i].code) == 0) ++actual_frequency;
            for (const char *p = code; *p != '\0';) {
                const char *end = strchr(p, ',');
                size_t length = end != NULL ? (size_t)(end - p) : strlen(p);
                if (strlen(errors[i].code) == length && memcmp(errors[i].code, p, length) == 0) ++expected_frequency;
                p += length + (end != NULL ? 1U : 0U);
            }
            CHECK(actual_frequency == expected_frequency);
            CHECK(strcmp(errors[i].path, "g24_bindings.ff") == 0);
            if (errors[i].token.line != line || errors[i].token.column != column)
                fprintf(stderr, "expected %u:%u %s; got %u:%u\n%s\n", line, column, token,
                    errors[i].token.line, errors[i].token.column, source);
            CHECK(errors[i].token.line == line && errors[i].token.column == column);
            CHECK(errors[i].token.length == strlen(token));
            CHECK(memcmp(errors[i].token.lexeme, token, strlen(token)) == 0);
        }
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/** Ordinary and open shared subjects reject exactly the same invalid paths. */
static void binding_path_pairs(void) {
    const char *labels[] = {"i32", "bool -> i32", "Inner -> bool", "Inner -> i32 -> string", "Missing"};
    const char *codes[] = {"AE0605", "AE1116", "AE1117", "AE1116", "AE1013,AE0605"};
    const char *positions[] = {"i32 {", "bool ->", "bool {", "i32 ->", "Missing {"};
    const char *tokens[] = {"i32", "bool", "bool", "i32", "Missing"};
    for (size_t shared = 0U; shared < 2U; ++shared) {
        for (size_t infix = 0U; infix < 2U; ++infix) {
            for (size_t item = 0U; item < sizeof(labels) / sizeof(labels[0]); ++item) {
                char source[1024], marker[128];
                snprintf(source, sizeof(source), "module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
                    "func run%s(value: %s) { %s %s %s }\n",
                    shared ? "<T: Outer>" : "", shared ? "T" : "Outer",
                    infix ? "if value match" : "match value {", labels[item],
                    infix ? "{}" : "{} }");
                snprintf(marker, sizeof(marker), "%s", positions[item]);
                binding_source(source, infix && item == 4U ? "AE1013,AE1013,AE0605" : codes[item], marker, tokens[item]);
            }
            char valid[1024];
            snprintf(valid, sizeof(valid), "module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
                "func run%s(value: %s) { %s item: Inner -> i32 { item; } %s }\n",
                shared ? "<T: Outer>" : "", shared ? "T" : "Outer", infix ? "if value match" : "match value {",
                infix ? "" : "}");
            binding_source(valid, NULL, NULL, NULL);
        }
    }
}

/** A second path must not broaden a generic type argument's admission. */
static void binding_callable_nominality(void) {
    binding_source("module g24;\nspec A(): i32;\nspec B(): i32;\nspec U: A | bool;\n"
        "func take<T: U>(value: T) {}\nfunc run(value: B) { take<B>(value); }\n",
        "AE0512", "take<B>", "take");
    binding_source("module g24;\nspec A(): i32;\nspec B(): i32;\nspec U: A | bool;\n"
        "func take<T: U>(value: T) {}\nfunc run(value: B) { take<A>((A)value); }\n", NULL, NULL, NULL);
}

/** Composition order and shared DAG nodes must not change cycle validation. */
static void binding_composition_graphs(void) {
    binding_source("module g24;\nspec A {}\nspec Both: Next & A;\nspec Next: Both & A;\n",
        "AE0614", "Both:", "Both");
    binding_source("module g24;\nspec Both: Left & Right;\nspec Left: A & B;\n"
        "spec Right: A & B;\nspec A {}\nspec B {}\n", NULL, NULL, NULL);
    binding_source("module g24;\nspec A {}\nspec B {}\nspec Left: A & B;\n"
        "spec Right: A & B;\nspec Both: Left & Right;\n", NULL, NULL, NULL);
}

/** Synthetic resolver refs must be copied, not retained by borrowed address. */
static void binding_conversion_ref_ownership(void) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    const char *source = "module g24;\nspec Inner: i32 | string;\nfunc run(value: Inner) {}\n";
    CHECK(feng_parse_source(source, strlen(source), "g24_owned.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    CHECK(feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count));
    CHECK(count == 0U);
    FengExpr expression = {0};
    FengSlice target_name = {"Inner", 5U}, member_name = {"i32", 3U};
    FengTypeRef target = {0}, member = {0};
    target.kind = member.kind = FENG_TYPE_REF_NAMED;
    target.as.named.segments = &target_name; target.as.named.segment_count = 1U;
    member.as.named.segments = &member_name; member.as.named.segment_count = 1U;
    size_t path[] = {0U};
    CHECK(feng_semantic_record_union_coercion_site(analysis, &expression,
        program->declarations[0], &target, 0U, &member, path, 1U));
    const FengUnionCoercionSite *site = feng_semantic_lookup_union_coercion_site(analysis, &expression);
    CHECK(site != NULL && site->target_union_type_ref != &target && site->member_type_ref != &member);
    CHECK(site->target_union_type_ref->as.named.segments != &target_name);
    target_name = (FengSlice){"string", 6U};
    member_name = (FengSlice){"bool", 4U};
    CHECK(site->target_union_type_ref->as.named.segments[0].length == 5U);
    CHECK(memcmp(site->target_union_type_ref->as.named.segments[0].data, "Inner", 5U) == 0);
    CHECK(memcmp(site->member_type_ref->as.named.segments[0].data, "i32", 3U) == 0);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/** Independent registration for the completed G24 binding diagnostics. */
void test_g24_binding_diagnostics(void) {
    binding_path_pairs();
    binding_callable_nominality();
    binding_composition_graphs();
    binding_conversion_ref_ownership();
    puts("G24 binding semantic matrices passed");
}
