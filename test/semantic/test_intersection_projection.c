#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* A declaration graph with ordered, repeated, nested and generic edges. */
static const char *projection_prefix =
    "module g24.projection;\n"
    "spec Base { var item: i32; } spec Left: Base { func left(): i32; }\n"
    "spec Right { func right(): i32; } spec Extra {}\n"
    "spec Both: Left & Right; spec Nested: Both & Extra;\n"
    "spec Reverse: Right & Left; spec Repeated: Left & Left & Right;\n"
    "spec Same: Left & Right; spec Unrelated { func right(): i32; }\n"
    "spec Choice: Left | Right; spec Child: Left, Right {}\n"
    "spec Field<A> { var item: A; } spec Leaf<A>: Field<A> {}\n"
    "spec Generic<A>: Leaf<A> & Right; spec Outer<A>: Generic<A> & Extra;\n"
    "type Holder { open var value: Right; }\n";

/* Assert the full diagnostic or the single exact Semantic projection path. */
static void projection_case(const char *body, const char *code,
                            const char *marker, const char *token,
                            const size_t *indices, size_t depth) {
    char source[4096];
    snprintf(source, sizeof source, "%s%s\n", projection_prefix, body);
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "component_projection.ff", &program, &parse);
    if (!parsed) fprintf(stderr, "%s %s\n%s", parse.code, parse.message, source);
    CHECK(parsed);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != (code == NULL) || count != (code == NULL ? 0U : 1U) ||
        (code != NULL && count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "expected %s\n%s", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i)
            fprintf(stderr, "%s %u:%u %s\n", errors[i].code, errors[i].token.line,
                    errors[i].token.column, errors[i].message);
    }
    CHECK(ok == (code == NULL) && count == (code == NULL ? 0U : 1U));
    if (code != NULL) {
        const char *at = strstr(source + strlen(projection_prefix), marker);
        unsigned line = 1U, column = 1U;
        CHECK(at != NULL);
        for (const char *p = source; p < at; ++p) {
            if (*p == '\n') { ++line; column = 1U; } else ++column;
        }
        CHECK(strcmp(errors[0].code, code) == 0);
        CHECK(strcmp(errors[0].path, "component_projection.ff") == 0);
        CHECK(errors[0].token.line == line && errors[0].token.column == column);
        CHECK(errors[0].token.length == strlen(token));
        CHECK(memcmp(errors[0].token.lexeme, token, strlen(token)) == 0);
    } else {
        size_t found = 0U;
        for (size_t i = 0U; i < analysis->spec_coercion_site_count; ++i) {
            const FengSpecCoercionSite *site = &analysis->spec_coercion_sites[i];
            if (site->form != FENG_SPEC_COERCION_FORM_INTERSECTION_UPCAST) continue;
            ++found;
            CHECK(site->target_spec_type_ref != NULL);
            CHECK(site->object_upcast_parent_index_count == depth);
            CHECK(memcmp(site->object_upcast_parent_indices, indices, depth * sizeof(*indices)) == 0);
            CHECK(site->expr != NULL && site->expr->kind != FENG_EXPR_CAST);
        }
        CHECK(found == (depth == 0U ? 0U : 1U));
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Explicit reachability is independent of implicit matching and constraint admission. */
void test_intersection_projection_semantics(void) {
    const size_t first[] = {0U}, second[] = {1U}, third[] = {2U};
    const size_t parent[] = {0U, 0U}, nested_right[] = {0U, 1U};
    const size_t nested_parent[] = {0U, 0U, 0U};
    projection_case("func run(v: Both): Left { return (Left)v; }", NULL, NULL, NULL, first, 1U);
    projection_case("func run(v: Both): Right { return (Right)v; }", NULL, NULL, NULL, second, 1U);
    projection_case("func run(v: Both): Base { return (Base)v; }", NULL, NULL, NULL, parent, 2U);
    projection_case("func run(v: Nested): Both { return (Both)v; }", NULL, NULL, NULL, first, 1U);
    projection_case("func run(v: Nested): Right { return (Right)v; }", NULL, NULL, NULL, nested_right, 2U);
    projection_case("func run(v: Nested): Base { return (Base)v; }", NULL, NULL, NULL, nested_parent, 3U);
    projection_case("func run(v: Reverse): Left { return (Left)v; }", NULL, NULL, NULL, second, 1U);
    projection_case("func run(v: Repeated): Right { return (Right)v; }", NULL, NULL, NULL, third, 1U);
    projection_case("func run(v: Repeated): Left { return (Left)v; }", NULL, NULL, NULL, first, 1U);
    projection_case("func run<A>(v: Generic<A>): Field<A> { return (Field<A>)v; }", NULL, NULL, NULL, parent, 2U);
    projection_case("func run<A>(v: Outer<A>): Field<A> { return (Field<A>)v; }", NULL, NULL, NULL, nested_parent, 3U);
    projection_case("func run(v: Generic<string>): Field<string> { return (Field<string>)v; }", NULL, NULL, NULL, parent, 2U);
    projection_case("func run(v: Both): Both { return (Both)v; }", NULL, NULL, NULL, NULL, 0U);
    projection_case("func run(v: Child): Right { let implicit: Right = v; return (Right)v; }", NULL, NULL, NULL, NULL, 0U);

    const char *bad_casts[] = {
        "func run(v: Right): Both { return (Both)v; }",
        "func run(v: Both): Unrelated { return (Unrelated)v; }",
        "func run(v: Both): Same { return (Same)v; }",
        "func run(v: Both): Nested { return (Nested)v; }",
        "func run(v: Choice): Right { return (Right)v; }",
        "func run(v: Generic<i32>): Field<string> { return (Field<string>)v; }",
        "func run<A,B>(v: Generic<A>): Field<B> { return (Field<B>)v; }"
    };
    for (size_t i = 0U; i < sizeof bad_casts / sizeof bad_casts[0]; ++i) {
        const char *cast = strstr(bad_casts[i], "return (") + strlen("return ");
        projection_case(bad_casts[i], "AE1023", cast, "(", NULL, 0U);
    }
    projection_case("func take(v: Right) {} func run(v: Both) { take(v); }",
        "AE0512", "take(v);", "take", NULL, 0U);
    projection_case("func run(v: Both) { let target: Right = v; }",
        "AE1003", "v; }", "v", NULL, 0U);
    projection_case("func run(v: Both, target: Right) { var copy = target; copy = v; }",
        "AE1003", "v; }", "v", NULL, 0U);
    projection_case("func run(v: Both): Right { return v; }",
        "AE1003", "v; }", "v", NULL, 0U);
    projection_case("func run(v: Both, target: Holder) { target.value = v; }",
        "AE1003", "v; }", "v", NULL, 0U);
    projection_case("func run(v: Both, target: Right[!]) { target[0] = v; }",
        "AE1003", "v; }", "v", NULL, 0U);
    projection_case("func run(v: Both) { let target: Right[] = [v]; }",
        "AE1003", "v];", "v", NULL, 0U);
    puts("intersection explicit projection semantic matrices passed");
}
