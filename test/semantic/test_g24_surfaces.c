#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** One isolated invalid surface and its complete expected diagnostic. */
typedef struct SurfaceCase { const char *name, *source, *code, *marker, *token; } SurfaceCase;

/** Assert stage, complete count, file, exact token, and line/column together. */
static bool surface_case(const SurfaceCase *test) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool parsed = feng_parse_source(test->source, strlen(test->source), "g24_surface.ff", &program, &parse);
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = false;
    if (parsed) {
        const FengProgram *programs[] = {program};
        FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
        ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    }
    const char *codes[8], *markers[8], *tokens[8];
    char *code_copy = strdup(test->code), *marker_copy = strdup(test->marker), *token_copy = strdup(test->token);
    if (code_copy == NULL || marker_copy == NULL || token_copy == NULL) exit(1);
    char *saved = NULL;
    size_t expected = 0U, marker_count = 0U, token_count = 0U;
    for (char *part = strtok_r(code_copy, ",", &saved); part != NULL; part = strtok_r(NULL, ",", &saved)) {
        if (expected == 8U) exit(1);
        codes[expected++] = part;
    }
    for (char *part = strtok_r(marker_copy, "\t", &saved); part != NULL; part = strtok_r(NULL, "\t", &saved)) {
        if (marker_count == 8U) exit(1);
        markers[marker_count++] = part;
    }
    for (char *part = strtok_r(token_copy, "\t", &saved); part != NULL; part = strtok_r(NULL, "\t", &saved)) {
        if (token_count == 8U) exit(1);
        tokens[token_count++] = part;
    }
    bool passed = parsed && !ok && count == expected && marker_count == expected && token_count == expected;
    bool matched[8] = {false};
    for (size_t i = 0U; passed && i < expected; ++i) {
        const char *position = strstr(test->source, markers[i]);
        unsigned line = 1U, column = 1U;
        if (position != NULL) for (const char *p = test->source; p < position; ++p) {
            if (*p == '\n') { ++line; column = 1U; } else ++column;
        }
        bool found = false;
        for (size_t j = 0U; position != NULL && j < count; ++j) {
            const FengSemanticError *error = &errors[j];
            if (!matched[j] && strcmp(error->code, codes[i]) == 0 && strcmp(error->path, "g24_surface.ff") == 0 &&
                error->token.line == line && error->token.column == column &&
                error->token.length == strlen(tokens[i]) && memcmp(error->token.lexeme, tokens[i], strlen(tokens[i])) == 0) {
                matched[j] = found = true;
                break;
            }
        }
        passed = found;
    }
    if (!passed) {
        fprintf(stderr, "G24 surface %s: expected %s at %s (%s); parsed=%d ok=%d count=%zu\n%s", test->name, test->code, test->marker, test->token, parsed, ok, count, test->source);
        if (!parsed) fprintf(stderr, "PARSE %s %u:%u %s\n", parse.code, parse.token.line, parse.token.column, parse.message);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %.*s %s\n", errors[i].code, errors[i].token.line, errors[i].token.column, (int)errors[i].token.length, errors[i].token.lexeme, errors[i].message);
    }
    free(code_copy); free(marker_copy); free(token_copy);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    return passed;
}

/** Legal adjacent programs must pass the same parser and Semantic pipeline. */
static bool surface_accepts(const char *source) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "g24_surface.ff", &program, &parse);
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()};
    bool ok = parsed && feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok || count != 0U) {
        fprintf(stderr, "G24 legal surface rejected:\n%s\n", source);
        if (!parsed) fprintf(stderr, "%s %s\n", parse.code, parse.message);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    return ok && count == 0U;
}

/** All six value-entry sites isolate nominal satisfaction from writability. */
static bool surface_entry_pairs(void) {
    const char *bodies[] = {"let target: Target = source;", "var target: Target = good; target = source;",
        "take(source);", "return source;", "holder.value = source;", "values[0] = source;"};
    bool passed = true;
    for (size_t form = 0U; form < 2U; ++form) {
        for (size_t bad = 0U; bad < (form == 0U ? 1U : 3U); ++bad) {
            for (size_t site = 0U; site < 6U; ++site) {
                for (size_t valid = 0U; valid < 2U; ++valid) {
                    char source[2048], name[128];
                    const char *actual = valid ? "Good" : form == 0U ? "None" : bad == 0U ? "Partial" : bad == 1U ? "None" : "Shape";
                    snprintf(source, sizeof(source), "module g24;\n"
                        "spec A { func a(): i32; } spec B { func b(): i32; }\n"
                        "type Good: A, B { func a(): i32 { return 1; } func b(): i32 { return 2; } }\n"
                        "type Partial: A { func a(): i32 { return 1; } }\n"
                        "type None {}\ntype Shape { func a(): i32 { return 1; } func b(): i32 { return 2; } }\n"
                        "spec Target: %s;\ntype Holder { open var value: Target; }\n"
                        "func take(value: Target) {}\n"
                        "func run(source: %s, good: Good, holder: Holder, values: Target[!])%s { %s }\n",
                        form == 0U ? "Good | string" : "A & B", actual, site == 3U ? ": Target" : "", bodies[site]);
                    if (valid) { if (!surface_accepts(source)) passed = false; }
                    else {
                        snprintf(name, sizeof(name), "entry form=%zu source=%s site=%zu", form, actual, site);
                        SurfaceCase test = {name, source, site == 2U ? "AE0512" : form == 0U ? "AE1003" : "AE0622",
                            site == 2U ? "take(source)" : "source;", site == 2U ? "take" : "source"};
                        if (!surface_case(&test)) passed = false;
                    }
                }
            }
        }
    }
    return passed;
}

/** Narrowed subsets and intersections expose only their declared static surface. */
static bool surface_view_boundaries(void) {
    const char *union_bodies[] = {
        "match v { V { v.field; } }",
        "match v { item: V, string { item.field; } }",
        "match v { item: V, string { item.get(); } }",
        "match v { item: V, string { item == item; } }",
        "if v match item: V || item.field == 0 {}",
        "if !(v match item: V) { item.field; }"
    };
    const char *markers[] = {"field;", "field;", "get();", "==", "item.field", "item.field"};
    const char *tokens[] = {"field", "field", "get", "==", "item", "item"};
    bool passed = true;
    for (size_t shared = 0U; shared < 2U; ++shared) {
        for (size_t body = 0U; body < sizeof(union_bodies) / sizeof(union_bodies[0]); ++body) {
            char source[1024];
            snprintf(source, sizeof(source), "module g24;\n"
                "type V { open var field: i32; func get(): i32 { return 0; } }\n"
                "spec U: V | string | bool;\nfunc run%s(v: %s) { %s }\n",
                shared ? "<T: U>" : "", shared ? "T" : "U", union_bodies[body]);
            SurfaceCase test = {"union view boundary", source,
                body < 4U ? "AE0604" : body == 4U ? "AE0001,AE1019,AE1019,AE1102" : "AE0001",
                body == 4U ? "item.field\t==\t||\t||" : markers[body],
                body == 4U ? "item\t==\t||\t||" : tokens[body]};
            if (!surface_case(&test)) passed = false;
        }
        for (size_t multiple = 0U; multiple < 2U; ++multiple) {
            char source[1024];
            snprintf(source, sizeof(source), "module g24;\ntype V { open let field: i32; }\n"
                "spec U: V | string%s;\nfunc run%s(v: %s) { match v { string {} else { v.field; } } }\n",
                multiple ? " | bool" : "", shared ? "<T: U>" : "", shared ? "T" : "U");
            SurfaceCase test = {"else never retypes original", source, "AE0604", "field; }", "field"};
            if (!surface_case(&test)) passed = false;
            snprintf(source, sizeof(source), "module g24;\ntype V { open let field: i32; }\n"
                "spec U: V | string%s;\nfunc run%s(v: %s) { match v { string {} else { match v { item: V { item.field; } } } } }\n",
                multiple ? " | bool" : "", shared ? "<T: U>" : "", shared ? "T" : "U");
            if (!surface_accepts(source)) passed = false;
        }
    }
    const SurfaceCase cases[] = {
        {"intersection missing field", "module g24;\nspec A {} spec B {} spec Both: A & B;\nfunc run(v: Both) { v.missing; }\n", "AE1008", "missing;", "missing"},
        {"intersection instance static", "module g24;\nspec A { static let count: i32; } spec B {} spec Both: A & B;\nfunc run(v: Both) { v.count; }\n", "AE1008", "count; }", "count"},
        {"intersection type match", "module g24;\nspec A {} spec B {} spec Both: A & B;\nfunc run(v: Both) { match v { A {} } }\n", "AE0050", "v { A", "v"},
        {"value let cannot implement var", "module g24;\nspec A { var field: i32; } spec B {} spec Both: A & B;\n@value type V: A, B { open let field: i32; }\n", "AE0702", "V: A", "V"},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) if (!surface_case(&cases[i])) passed = false;
    return passed;
}

/** Stronger isolated counterparts to the preexisting broad surface tests. */
void test_g24_surface_diagnostics(void) {
    const SurfaceCase cases[] = {
        {"union unknown", "module g24;\nspec U: Missing | i32;\n", "AE1013", "Missing", "Missing"},
        {"union intersection", "module g24;\nspec A {} spec B {} spec Both: A & B;\nspec U: Both | i32;\n", "AE0622,AE0603", "Both |\tU:", "Both\tU"},
        {"intersection i32", "module g24;\nspec A {}\nspec Both: A & i32;\n", "AE0621", "i32;", "i32"},
        {"intersection i32[]", "module g24;\nspec A {}\nspec Both: A & i32[];\n", "AE0621", "i32[];", "i32"},
        {"intersection Value", "module g24;\ntype Value {}\nspec A {}\nspec Both: A & Value;\n", "AE0621", "Value;", "Value"},
        {"intersection Callback", "module g24;\nspec Callback(): i32;\nspec A {}\nspec Both: A & Callback;\n", "AE0621", "Callback;", "Callback"},
        {"intersection Union", "module g24;\nspec Union: i32 | string;\nspec A {}\nspec Both: A & Union;\n", "AE0621", "Union;", "Union"},
        {"intersection Missing", "module g24;\nspec A {}\nspec Both: A & Missing;\n", "AE1013,AE0621", "Missing;\tMissing;", "Missing\tMissing"},
        {"union self cycle", "module g24;\nspec U: U | i32;\n", "AE0601", "U:", "U"},
        {"intersection self cycle", "module g24;\nspec A {}\nspec Both: Both & A;\n", "AE0614", "Both:", "Both"},
        {"union disallowed type", "module g24;\nspec U: i32 | string;\ntype V: U {}\n", "AE0615", "U {", "U"},
        {"union disallowed fit", "module g24;\nspec U: i32 | string;\ntype V {}\nfit V: U {}\n", "AE0809", "U {", "U"},
        {"union disallowed parent", "module g24;\nspec U: i32 | string;\nspec V: U {}\n", "AE0613", "U {", "U"},
        {"intersection method conflict", "module g24;\nspec A { func f(): i32; } spec B { func f(): string; }\nspec Both: A & B;\n", "AE0706", "Both:", "Both"},
        {"ordinary v.field;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { v.field; }\n", "AE0604", "field;", "field"},
        {"ordinary v.field = 1;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { v.field = 1; }\n", "AE0604", "field =", "field"},
        {"ordinary v.get();", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { v.get(); }\n", "AE0604", "get();", "get"},
        {"ordinary let f = v.get;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { let f = v.get; }\n", "AE0604", "get;", "get"},
        {"ordinary v == v;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { v == v; }\n", "AE0604", "==", "=="},
        {"ordinary v != v;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { v != v; }\n", "AE0604", "!=", "!="},
        {"immutable narrowed false", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { match v { item: V { item = V(); } } }\n", "AE0104", "item =", "item"},
        {"outside binding false", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { if v match item: V {} item; }\n", "AE0001", "item;", "item"},
        {"else binding false", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run(v: U) { if v match item: V {} else { item; } }\n", "AE0001", "item;", "item"},
        {"shared v.field;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { v.field; }\n", "AE0604", "field;", "field"},
        {"shared v.field = 1;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { v.field = 1; }\n", "AE0604", "field =", "field"},
        {"shared v.get();", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { v.get(); }\n", "AE0604", "get();", "get"},
        {"shared let f = v.get;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { let f = v.get; }\n", "AE0604", "get;", "get"},
        {"shared v == v;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { v == v; }\n", "AE0604", "==", "=="},
        {"shared v != v;", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { v != v; }\n", "AE0604", "!=", "!="},
        {"immutable narrowed true", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { match v { item: V { item = V(); } } }\n", "AE0104", "item =", "item"},
        {"outside binding true", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { if v match item: V {} item; }\n", "AE0001", "item;", "item"},
        {"else binding true", "module g24;\ntype V { open var field: i32; func get(): i32 { return 0; } }\nspec U: V | string;\nfunc run<T: U>(v: T) { if v match item: V {} else { item; } }\n", "AE0001", "item;", "item"},
        {"duplicate same branch", "module g24;\nspec U: i32 | string;\nfunc run(v: U) { match v { i32, i32 {} } }\n", "AE0607", "i32 {}", "i32"},
        {"duplicate later branch", "module g24;\nspec U: i32 | string;\nfunc run(v: U) { match v { i32 {} i32 { } } }\n", "AE0607", "i32 { }", "i32"},
        {"value label", "module g24;\nspec U: i32 | string;\nfunc run(v: U) { match v { 7 {} } }\n", "AE0605", "7 {}", "7"},
        {"range label", "module g24;\nspec U: i32 | string;\nfunc run(v: U) { match v { 1...3 {} } }\n", "AE0605", "1...", "1"},
    };
    bool ok = true;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) if (!surface_case(&cases[i])) ok = false;
    if (!surface_entry_pairs()) ok = false;
    if (!surface_view_boundaries()) ok = false;
    if (!ok) exit(1);
    puts("G24 surface diagnostic matrices passed");
}
