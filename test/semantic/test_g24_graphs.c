#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Cross-module graphs are checked in both input orders, independently of .ft tests. */
static bool composite_graph(bool intersection, bool cyclic, bool reverse) {
    char left[512], right[512];
    snprintf(left, sizeof(left), "open module g24.left;\nimport g24.right;\n"
        "open spec Left: Right %c %s;\n", intersection ? '&' : '|', intersection ? "Base" : "i32");
    snprintf(right, sizeof(right), "open module g24.right;\nimport g24.left;\n"
        "open spec Base {}\nopen spec Right: %s %c %s;\n",
        cyclic ? "Left" : intersection ? "Base" : "i32", intersection ? '&' : '|', intersection ? "Base" : "string");
    const char *sources[] = {left, right};
    const char *paths[] = {"g24_graph_left.ff", "g24_graph_right.ff"};
    FengProgram *parsed[2] = {NULL, NULL};
    for (size_t i = 0U; i < 2U; ++i) {
        FengParseError error = {0};
        if (!feng_parse_source(sources[i], strlen(sources[i]), paths[i], &parsed[i], &error)) {
            fprintf(stderr, "G24 graph parse: %s\n%s", error.message, sources[i]);
            exit(1);
        }
    }
    const FengProgram *programs[] = {parsed[reverse ? 1U : 0U], parsed[reverse ? 0U : 1U]};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    bool ok = feng_semantic_analyze_with_options(programs, 2U, &options, &analysis, &errors, &count);
    bool passed = ok == !cyclic && count == (cyclic ? 1U : 0U);
    if (passed && cyclic) {
        const size_t first = reverse ? 1U : 0U;
        const FengToken expected = parsed[first]->declarations[first == 0U ? 0U : 1U]->token;
        passed = strcmp(errors[0].code, intersection ? "AE0614" : "AE0601") == 0 &&
            strcmp(errors[0].path, paths[first]) == 0 && errors[0].token.line == expected.line &&
            errors[0].token.column == expected.column && errors[0].token.length == expected.length &&
            memcmp(errors[0].token.lexeme, expected.lexeme, expected.length) == 0;
    }
    if (!passed) {
        fprintf(stderr, "G24 graph intersection=%d cyclic=%d reverse=%d ok=%d count=%zu\n", intersection, cyclic, reverse, ok, count);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s:%u:%u %s %s\n", errors[i].path,
            errors[i].token.line, errors[i].token.column, errors[i].code, errors[i].message);
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    for (size_t i = 0U; i < 2U; ++i) feng_program_free(parsed[i]);
    return passed;
}

/** Both composition forms reject cycles and accept shared leaves in either order. */
void test_g24_composite_graphs(void) {
    bool passed = true;
    for (size_t form = 0U; form < 2U; ++form)
        for (size_t cyclic = 0U; cyclic < 2U; ++cyclic)
            for (size_t reverse = 0U; reverse < 2U; ++reverse)
                if (!composite_graph(form != 0U, cyclic != 0U, reverse != 0U)) passed = false;
    if (!passed) exit(1);
    puts("G24 cross-module composition graph matrices passed");
}
