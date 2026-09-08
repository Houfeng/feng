#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/** Open-view eligibility remains an ordinary compile-time nominal contract check. */
static void view_case(const char *source, const char *code, const char *marker, const char *token) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "g24_views.ff", &program, &parse));
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != (code == NULL) || count != (code == NULL ? 0U : 1U) ||
        (code != NULL && count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "expected %s\n%s", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %s\n", errors[i].code, errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    CHECK(ok == (code == NULL) && count == (code == NULL ? 0U : 1U));
    if (code != NULL) {
        const char *at = strstr(source, marker);
        unsigned line = 1U, column = 1U;
        CHECK(at != NULL);
        for (const char *p = source; p < at; ++p) { if (*p == '\n') { ++line; column = 1U; } else ++column; }
        CHECK(strcmp(errors[0].code, code) == 0 && strcmp(errors[0].path, "g24_views.ff") == 0);
        CHECK(errors[0].token.line == line && errors[0].token.column == column);
        CHECK(errors[0].token.length == strlen(token) && memcmp(errors[0].token.lexeme, token, strlen(token)) == 0);
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/** Six entry sites pair legal and partial generic subjects for both value categories. */
void test_g24_spec_view_semantics(void) {
    const char *bodies[] = {"let v: Both<A> = source;", "var v: Both<A> = good; v = source;",
        "take<A>(source);", "return source;", "holder.view = source;", "values[0] = source;"};
    for (size_t value = 0U; value < 2U; ++value) {
        for (size_t site = 0U; site < 6U; ++site) {
            for (size_t valid = 0U; valid < 2U; ++valid) {
                char source[1600];
                snprintf(source, sizeof(source), "module g24;\n"
                    "spec Field<A> { var item: A; } spec Tag {} spec Both<A>: Field<A> & Tag;\n"
                    "%stype Good<A>: Field<A>, Tag { open var item: A; }\n"
                    "%stype Partial<A>: Field<A> { open var item: A; }\n"
                    "type Holder<A> { open var view: Both<A>; }\n"
                    "func take<A>(v: Both<A>) {}\n"
                    "func run<A>(source: %s<A>, good: Good<A>, holder: Holder<A>, values: Both<A>[!])%s { %s }\n",
                    value ? "@value " : "", value ? "@value " : "", valid ? "Good" : "Partial",
                    site == 3U ? ": Both<A>" : "", bodies[site]);
                view_case(source, valid ? NULL : site == 2U ? "AE0512" : "AE0622",
                    site == 2U ? "take<A>(source)" : "source;", site == 2U ? "take" : "source");
            }
        }
    }
    puts("G24 spec view semantic matrices passed");
}
