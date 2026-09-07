#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "semantic/semantic.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: G23 failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Analyze independent source units in either order. Every negative case fixes
 * the phase, unique diagnostic, source path and precise offending token. */
static void g23_analyze(const char *const *sources, size_t count, bool reverse,
                        const char *code, size_t error_source,
                        const char *marker, const char *lexeme) {
    FengProgram *owned[4] = {0};
    const FengProgram *programs[4] = {0};
    char paths[4][32];
    FengSemanticAnalyzeOptions options = {0};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool result;

    CHECK(count <= 4U);
    for (size_t i = 0U; i < count; ++i) {
        FengParseError error = {0};
        snprintf(paths[i], sizeof(paths[i]), "g23_source_%zu.ff", i);
        result = feng_parse_source(sources[i], strlen(sources[i]), paths[i], &owned[i], &error);
        if (!result) fprintf(stderr, "G23 parse: %s\n%s\n", error.message, sources[i]);
        CHECK(result);
        programs[reverse ? count - i - 1U : i] = owned[i];
    }
    options.target = FENG_COMPILE_TARGET_LIB;
    options.pointer_size = feng_get_host_pointer_size();
    result = feng_semantic_analyze_with_options(programs, count, &options,
                                               &analysis, &errors, &error_count);
    if (result != (code == NULL) || error_count != (code == NULL ? 0U : 1U) ||
        (code != NULL && error_count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "G23 expected %s, reverse=%d\n", code != NULL ? code : "success", reverse);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s\n", sources[i]);
        for (size_t i = 0U; i < error_count; ++i)
            fprintf(stderr, "%s:%u:%u %s %s\n", errors[i].path, errors[i].token.line,
                    errors[i].token.column, errors[i].code, errors[i].message);
    }
    CHECK(result == (code == NULL));
    CHECK(error_count == (code == NULL ? 0U : 1U));
    if (code != NULL) {
        const char *position = NULL;
        const char *cursor = sources[error_source];
        unsigned line = 1U, column = 1U;
        while ((cursor = strstr(cursor, marker)) != NULL) { position = cursor; ++cursor; }
        CHECK(position != NULL);
        for (cursor = sources[error_source]; cursor < position; ++cursor) {
            if (*cursor == '\n') { ++line; column = 1U; } else { ++column; }
        }
        if (errors[0].token.line != line || errors[0].token.column != column)
            fprintf(stderr, "G23 token expected %u:%u %s, got %u:%u: %s\n%s\n", line, column,
                    lexeme, errors[0].token.line, errors[0].token.column, errors[0].message,
                    sources[error_source]);
        CHECK(strcmp(errors[0].path, paths[error_source]) == 0);
        CHECK(strcmp(errors[0].code, code) == 0);
        CHECK(errors[0].token.line == line && errors[0].token.column == column);
        CHECK(errors[0].token.length == strlen(lexeme));
        CHECK(memcmp(errors[0].token.lexeme, lexeme, strlen(lexeme)) == 0);
    } else {
        /* Valid relations must never expose an empty implementation to Codegen. */
        for (size_t i = 0U; i < analysis->spec_witness_count; ++i) {
            const FengSpecWitness *witness = &analysis->spec_witnesses[i];
            for (size_t j = 0U; j < witness->member_count; ++j) {
                const FengSpecWitnessMember *member = &witness->members[j];
                CHECK(member->spec_member != NULL && member->impl_member != NULL);
                if (member->source_kind == FENG_SPEC_WITNESS_SOURCE_FIT_METHOD) {
                    CHECK(member->via_fit_decl != NULL && member->provider_module != NULL);
                    bool exact = false;
                    for (size_t k = 0U; k < member->via_fit_decl->as.fit_decl.member_count; ++k)
                        exact |= member->via_fit_decl->as.fit_decl.members[k] == member->impl_member;
                    CHECK(exact);
                }
            }
        }
    }
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    for (size_t i = 0U; i < count; ++i) feng_program_free(owned[i]);
}

/* Convenience entry for a minimal single-file declaration or coercion. */
static void g23_source(const char *source, const char *code,
                        const char *marker, const char *lexeme) {
    const char *sources[] = {source};
    g23_analyze(sources, 1U, false, code, 0U, marker, lexeme);
}

/* A representative of each distinct target representation, not every scalar width. */
typedef struct G23Subject {
    const char *declaration;
    const char *type;
    const char *token;
    const char *value;
} G23Subject;

static const G23Subject g23_subjects[] = {
    {"type Item {}\n", "Item", "Item", "Item()"},
    {"@value type Item {}\n", "Item", "Item", "Item()"},
    {"type Item(i32, string);\n", "Item", "Item", "(7, \"item\")"},
    {"enum Item { One }\n", "Item", "Item", "Item.One"},
    {"", "i32", "i32", "7"},
    {"", "string", "string", "\"item\""},
    {"", "i32[]", "i32", "[7]"},
    {"", "i32[!]", "i32", "[7]"}
};

/* SPEC02/04/05/12/15/27: relation syntax is independent of satisfaction.
 * Unused and demanded invalid relations fail at the same declaration token. */
static void g23_relation_matrix(void) {
    char source[4096], marker[128];
    for (size_t t = 0U; t < sizeof(g23_subjects) / sizeof(g23_subjects[0]); ++t) {
        const G23Subject *s = &g23_subjects[t];
        for (size_t body = 0U; body < 2U; ++body) {
            const char *tail = body ? "{}" : ";";
            snprintf(source, sizeof(source), "module g23;\n%sspec S {}\n"
                     "fit %s: S %s\nfit %s: S %s\n"
                     "func check() { let subject: %s = %s; let view: S = subject; }\n",
                     s->declaration, s->type, tail, s->type, tail, s->type, s->value);
            g23_source(source, NULL, NULL, NULL);
            for (size_t qualified = 0U; qualified < 2U; ++qualified) {
                for (size_t used = 0U; used < 2U; ++used) {
                    char use[256] = "";
                    if (used) snprintf(use, sizeof(use), "func check() { let subject: %s = %s; let view: S = subject; }\n",
                                       s->type, s->value);
                    snprintf(source, sizeof(source), "open module g23;\n%sopen spec S {}\nfit %s: S, %s %s\n%s",
                             s->declaration, s->type, qualified ? "g23.S" : "S", tail, use);
                    snprintf(marker, sizeof(marker), "%s %s", qualified ? "g23.S" : "S", tail);
                    g23_source(source, "AE0810", marker, qualified ? "g23" : "S");
                }
            }
            for (size_t member = 0U; member < 4U; ++member) {
                const char *requirements[] = {"let missing: i32;", "static var missing: i32;",
                                              "func missing(): i32;", "static func missing(): i32;"};
                for (size_t parent = 0U; parent < 2U; ++parent) {
                    for (size_t used = 0U; used < 2U; ++used) {
                        char use[256] = "";
                        if (used) snprintf(use, sizeof(use), "func check() { let subject: %s = %s; let view: S = subject; }\n",
                                           s->type, s->value);
                        snprintf(source, sizeof(source), "module g23;\n%sspec %s { %s }\n%sfit %s: S %s\n%s",
                                 s->declaration, parent ? "Parent" : "S", requirements[member],
                                 parent ? "spec S: Parent {}\n" : "", s->type, tail, use);
                        snprintf(marker, sizeof(marker), "%s: S", s->type);
                        g23_source(source, member < 2U ? "AE0701" : "AE0705", marker, s->token);
                    }
                }
            }
        }
    }
}

/* SPEC03/15/17/21: vary exactly one signature dimension, implementation
 * source, receiver face or visibility; parameter names/mutability are irrelevant. */
static void g23_signature_matrix(void) {
    static const char *implementations[] = {
        "func apply(): i32 { return 1; }",
        "func apply(a: string, b: string): i32 { return 1; }",
        "func apply(a: string, b: i32): i32 { return 1; }",
        "func apply(a: i32, b: string): string { return b; }",
        "func apply(a: i32, b: string...): i32 { return 1; }",
        "static func apply(a: i32, b: string): i32 { return 1; }",
        "seal func apply(a: i32, b: string): i32 { return 1; }",
        "func apply<U>(a: i32, b: string): i32 { return 1; }",
        "func apply(var renamed: i32, let other: string): i32 { return renamed; }"
    };
    char source[4096], marker[128];
    for (size_t t = 0U; t < sizeof(g23_subjects) / sizeof(g23_subjects[0]); ++t) {
        const G23Subject *s = &g23_subjects[t];
        for (size_t separate = 0U; separate < (t < 2U ? 3U : 2U); ++separate) {
            for (size_t i = 0U; i < sizeof(implementations) / sizeof(implementations[0]); ++i) {
                for (size_t used = 0U; used < 2U; ++used) {
                    char use[256] = "";
                    if (used) snprintf(use, sizeof(use), "func check() { let subject: %s = %s; let view: S = subject; }\n",
                                       s->type, s->value);
                    if (separate == 2U) {
                        snprintf(source, sizeof(source), "module g23;\nspec S { func apply(a: i32, b: string): i32; }\n"
                                 "%stype Item: S { %s }\n%s", t == 1U ? "@value " : "", implementations[i], use);
                    } else {
                        snprintf(source, sizeof(source), "module g23;\n%sspec S { func apply(a: i32, b: string): i32; }\n"
                                 "fit %s%s { %s }\n%s%s%s\n%s",
                                 s->declaration, s->type, separate ? "" : ": S", implementations[i],
                                 separate ? "fit " : "", separate ? s->type : "", separate ? ": S;" : "", use);
                    }
                    snprintf(marker, sizeof(marker), "%s: S", s->type);
                    g23_source(source, i == 8U ? NULL : i == 5U ? "AE0705" : i == 6U ? "AE0707" : "AE0704",
                               marker, s->token);
                }
            }
        }
    }
}

/* SPEC12/15/23/26: neither source order nor relation order changes a unique
 * implementation, and a private relation does not become public by import. */
static void g23_relation_order_and_visibility(void) {
    for (size_t target = 0U; target < 2U; ++target) {
        const char *type = target == 0U ? "Item" : "int";
        const char *canonical = target == 0U ? "Item" :
                                feng_get_host_pointer_size() == 8U ? "i64" : "i32";
        char methods[256], relations[256], second[1024];
        snprintf(methods, sizeof(methods), "fit %s { func read(): i32 { return 7; } }\n", type);
        snprintf(relations, sizeof(relations), "fit %s: S;\nfit %s: S {}\n", type, canonical);
        for (size_t order = 0U; order < 2U; ++order) {
            snprintf(second, sizeof(second), "module g23.shared;\n%s%s"
                     "func check(value: %s): i32 { let view: S = value; return view.read() + value.read(); }\n",
                     order ? relations : methods, order ? methods : relations, canonical);
            const char *sources[] = {
                "module g23.shared;\ntype Item {}\nspec S { func read(): i32; }\n", second
            };
            g23_analyze(sources, 2U, false, NULL, 0U, NULL, NULL);
            g23_analyze(sources, 2U, true, NULL, 0U, NULL, NULL);
        }
    }
    for (size_t exported = 0U; exported < 2U; ++exported) {
        char relation[256];
        snprintf(relation, sizeof(relation), "open module g23.relations;\nimport g23.api;\n%sfit Item: S;\n",
                 exported ? "open " : "");
        const char *sources[] = {
            "open module g23.api;\nopen type Item { func read(): i32 { return 7; } }\n"
            "open spec S { func read(): i32; }\n",
            relation,
            "module g23.consumer;\nimport g23.api;\nimport g23.relations;\n"
            "func check(item: Item) { let view: S = item; }\n"
        };
        g23_analyze(sources, 3U, false, exported ? NULL : "AE1003", 2U, "item;", "item");
        g23_analyze(sources, 3U, true, exported ? NULL : "AE1003", 2U, "item;", "item");
    }
}

/* SPEC12/16/23/26: one implementation and repeated relation declarations in
 * several source files/modules; aliases activate fits, import order is stable. */
static void g23_visible_fit_sources(void) {
    const char *sources[] = {
        "open module g23.api;\nopen type Item {}\nopen spec S { func read(): i32; }\n",
        "open module g23.impl;\nimport g23.api;\nopen fit Item { func read(): i32 { return 7; } }\n",
        "open module g23.relation;\nimport g23.api;\nimport g23.impl;\nopen fit Item: S;\nopen fit Item: S {}\n",
        ("module g23.consumer;\nimport g23.api;\nimport g23.impl as impl;\nimport g23.relation as relations;\n"
         "func check(item: Item): i32 { let view: S = item; return view.read() + item.read(); }\n")
    };
    g23_analyze(sources, 4U, false, NULL, 0U, NULL, NULL);
    g23_analyze(sources, 4U, true, NULL, 0U, NULL, NULL);
    sources[3] = "module g23.consumer;\nimport g23.api;\nimport g23.impl;\n"
                 "func check(item: Item) { let view: S = item; }\n";
    g23_analyze(sources, 4U, false, "AE1003", 3U, "item;", "item");
    g23_analyze(sources, 4U, true, "AE1003", 3U, "item;", "item");
}

/* SPEC17/27: closed array owner instances and their witness implementations
 * must be distinct and keep all element substitutions after resolver cleanup. */
static void g23_generic_array_instances(void) {
    const char *source =
        "module g23;\n"
        "spec Head<T> { func head(): T; }\n"
        "fit T[] { func head(): T { return self[0]; } }\n"
        "fit T[!] { func head(): T { return self[0]; } }\n"
        "fit T[]: Head<T>;\nfit T[!]: Head<T> {}\n"
        "func check(numbers: i32[], words: string[!]) {\n"
        " let first: Head<i32> = numbers; let second: Head<string> = words;\n"
        " let n: i32 = first.head(); let s: string = second.head();\n}\n";
    g23_source(source, NULL, NULL, NULL);
    g23_source("module g23;\nspec Head<T> { func head(): T; }\n"
               "fit T[] { func head(): T { return self[0]; } }\nfit T[]: Head<T>;\n"
               "func check(numbers: i32[]) { let wrong: Head<string> = numbers; }\n",
               "AE1003", "numbers;", "numbers");
    g23_source("module g23;\nspec Head { func head(): i32; }\n"
               "fit T[] { func head(): T { return self[0]; } }\nfit i32[]: Head;\n"
               "func check(numbers: i32[]) { let view: Head = numbers; let n = view.head(); }\n",
               NULL, NULL, NULL);
}

/* SPEC01/07/08/11/13: diagnostic identities for declaration boundaries that
 * previously had message-only or count-only assertions. */
static void g23_declaration_diagnostics(void) {
    static const struct {
        const char *source;
        const char *code;
        const char *marker;
        const char *lexeme;
    } cases[] = {
        {"module g23;\nspec S { func ~S(); }\n", "AE0620", "~S", "~"},
        {"module g23;\nspec S { func apply<U>(value: U): U; }\n", "AE0331", "U>", "U"},
        {"module g23;\nspec S { static func apply<U>(value: U): U; }\n", "AE0331", "U>", "U"},
        {"module g23;\ntype T {}\nspec S: T {}\n", "AE0613", "T {}", "T"},
        {"module g23;\nspec P(): i32;\nspec S: P {}\n", "AE0613", "P {}", "P"},
        {"module g23;\nspec P {}\nspec S: P, P {}\n", "AE0614", "P {}", "P"},
        {"module g23;\nspec S: S {}\n", "AE0614", "S: S", "S"},
        {"module g23;\ntype T {}\ntype S: T {}\n", "AE0615", "T {}", "T"},
        {"module g23;\nspec C(): i32;\ntype T: C {}\n", "AE0615", "C {}", "C"},
        {"module g23;\nspec U: i32 | string;\ntype T: U {}\n", "AE0615", "U {}", "U"},
        {"module g23;\nspec A {}\nspec B {}\nspec I: A & B;\ntype T: I {}\n", "AE0615", "I {}", "I"},
        {"module g23;\nspec P {}\ntype T: P, P {}\n", "AE0616", "P {}", "P"},
        {"module g23;\ntype T {}\nspec C(): i32;\nfit T: C;\n", "AE0809", "C;", "C"},
        {"module g23;\ntype T {}\nspec U: i32 | string;\nfit T: U;\n", "AE0809", "U;", "U"},
        {"module g23;\ntype T {}\nspec A {}\nspec B {}\nspec I: A & B;\nfit T: I;\n", "AE0809", "I;", "I"},
        {"module g23;\ntype T {}\nfit i32: T {}\n", "AE0809", "T {}", "T"},
        {"module g23;\nspec C(): i32;\nfit i32[]: C {}\n", "AE0809", "C {}", "C"},
        {"module g23;\nspec U: i32 | string;\nfit string: U {}\n", "AE0809", "U {}", "U"},
        {"module g23;\nspec A {}\nspec B {}\nspec I: A & B;\nfit i32[!]: I {}\n", "AE0809", "I {}", "I"},
        {"module g23;\nspec S {}\nfit S {}\n", "AE0811", "S {}", "S"},
        {"module g23;\nspec C(): i32;\nfit C {}\n", "AE0811", "C {}", "C"},
        {"module g23;\nspec U: i32 | string;\nfit U {}\n", "AE0811", "U {}", "U"},
        {"module g23;\nspec A {}\nspec B {}\nspec I: A & B;\nfit I {}\n", "AE0811", "I {}", "I"},
        {"module g23;\nfit i32* {}\n", "AE0811", "i32*", "i32"},
        {"module g23;\nfit void {}\n", "AE0502", "void", "void"},
        {"module g23;\nspec S { func f(): i32; func f(): i32; }\n", "AE0508", "f(): i32; }", "f"},
        {"module g23;\nspec S { static func f(): i32; static func f(): string; }\n", "AE0509", "f(): string", "f"},
        {"module g23;\nspec S { func f(x: i32): i32; func f(xs: i32...): i32; }\n", "AE0510", "f(xs", "f"},
        {"module g23;\nfit i32 { func f(): i32 { return 1; } func f(): i32 { return 1; } }\n", "AE0801", "f(): i32 { return 1; } }", "f"},
        {"module g23;\nfit i32 { func f(): i32 { return 1; } func f(): string { return \"a\"; } }\n", "AE0802", "f(): string", "f"},
        {"module g23;\nfit i32 { func f(x: i32): i32 { return 1; } func f(xs: i32...): i32 { return 1; } }\n", "AE0803", "f(xs", "f"}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
        g23_source(cases[i].source, cases[i].code, cases[i].marker, cases[i].lexeme);
}

/* SPEC09/21: exact field requirements are invariant in binding kind, type and
 * instance/static face for both declared heads and relation-only fits. */
static void g23_field_requirements(void) {
    char source[2048];
    for (size_t fit = 0U; fit < 2U; ++fit) {
        for (size_t face = 0U; face < 2U; ++face) {
            for (size_t variant = 0U; variant < 6U; ++variant) {
                const char *prefix = face ? "static " : "";
                const char *impl_prefix = variant == 4U ? (face ? "" : "static ") : prefix;
                const char *required = variant == 1U ? "var" : "let";
                const char *actual = variant == 0U ? "var" : "let";
                const char *code = variant < 2U ? "AE0702" : variant == 2U ? "AE0703" :
                                   variant == 3U ? "AE0707" : variant == 4U ? "AE0701" : NULL;
                snprintf(source, sizeof(source),
                    "module g23;\nspec S { %s%s member: i32; }\n"
                    "type Item%s { %s%s%s member: %s; }\n%s",
                    prefix, required, fit ? "" : ": S",
                    variant == 3U ? "seal " : "", impl_prefix, actual,
                    variant == 2U ? "string" : "i32", fit ? "fit Item: S;\n" : "");
                g23_source(source, code, "Item: S", "Item");
            }
        }
    }
    g23_source("module g23;\nspec Parent {}\nspec Child: Parent {}\n"
               "spec S { let member: Parent; }\ntype Item: S { let member: Child; }\n",
               "AE0703", "Item: S", "Item");
    g23_source("module g23;\nspec S<T> { let member: T; }\n"
               "type Item<T>: S<T> { let member: T; }\n", NULL, NULL, NULL);
}

/* SPEC10/11/13/14: keep declaration collisions, visible implementation
 * ambiguity and conflicting requirements as separate diagnostic roots. */
static void g23_member_conflicts(void) {
    g23_source("module g23;\nspec Parent {}\nspec Child: Parent {}\ntype Item: Child {}\n"
               "fit i32 { func read(value: Parent): i32 { return 1; } func read(value: Child): i32 { return 2; } }\n",
               "AE0805", "read(value: Child)", "read");
    g23_source("module g23;\nspec S { let field: i32; let field: i32; }\n",
               "AE0514", "field: i32; }", "field");
    g23_source("module g23;\nspec Parent { let field: i32; }\n"
               "spec S: Parent { let field: i32; }\n", "AE0514", "field: i32; }", "field");
    g23_source("module g23;\nspec Parent<T> { func read(value: T): i32; }\n"
               "spec S: Parent<i32> { func read(value: i32): string; }\n",
               "AE0509", "read(value: i32)", "read");
    g23_source("module g23;\nspec S { func read(): i32; }\ntype Item {}\n"
               "fit Item { func read(): i32 { return 1; } }\n"
               "fit Item { func read(): i32 { return 1; } }\nfit Item: S;\n"
               "func check(item: Item) { let view: S = item; }\n",
               "AE0804", "item;", "item");
    g23_source("module g23;\nspec S { func read(): i32; }\n"
               "type Item { func read(): i32 { return 1; } }\n"
               "fit Item { func read(): i32 { return 1; } }\nfit Item: S;\n"
               "func check(item: Item) { let view: S = item; }\n",
               "AE0804", "item;", "item");
}

/* Preserve the complete existing diagnostic set for intersecting invariants
 * or unresolved types, without weakening the single-root matrix helper. */
static void g23_multiple_diagnostics(void) {
    const char *sources[] = {
        ("module g23;\nspec A { func read(): i32; }\nspec B { func read(): string; }\n"
         "type Item: A, B { func read(): i32 { return 1; } }\n"),
        "module g23;\nspec A: B {}\nspec B: A {}\n",
        "module g23;\nfit Missing {}\n",
        "module g23;\nfit i32: Missing;\n",
        "module g23;\ntype Item: Missing {}\n"
    };
    const char *codes[][2] = {{"AE0704", "AE0706"}, {"AE0614", "AE0614"},
                             {"AE1013", "AE0811"}, {"AE1013", "AE0809"}, {"AE1013", "AE0615"}};
    const unsigned lines[][2] = {{4U, 4U}, {2U, 3U}, {2U, 2U}, {2U, 2U}, {2U, 2U}};
    const unsigned columns[] = {6U, 6U, 5U, 10U, 12U};
    const char *tokens[][2] = {{"Item", "Item"}, {"A", "B"}, {"Missing", "Missing"},
                              {"Missing", "Missing"}, {"Missing", "Missing"}};
    for (size_t c = 0U; c < sizeof(sources) / sizeof(sources[0]); ++c) {
        FengProgram *program = NULL;
        FengParseError parse_error = {0};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        CHECK(feng_parse_source(sources[c], strlen(sources[c]), "g23_multiple.ff", &program, &parse_error));
        const FengProgram *programs[] = {program};
        FengSemanticAnalyzeOptions options = {0};
        options.target = FENG_COMPILE_TARGET_LIB;
        options.pointer_size = feng_get_host_pointer_size();
        CHECK(!feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count));
        CHECK(count == 2U);
        for (size_t e = 0U; e < 2U; ++e) {
            size_t matches = 0U;
            for (size_t i = 0U; i < count; ++i) {
                if (strcmp(errors[i].code, codes[c][e]) == 0 && errors[i].token.line == lines[c][e]) {
                    CHECK(strcmp(errors[i].path, "g23_multiple.ff") == 0);
                    CHECK(errors[i].token.column == columns[c]);
                    CHECK(errors[i].token.length == strlen(tokens[c][e]));
                    CHECK(memcmp(errors[i].token.lexeme, tokens[c][e], strlen(tokens[c][e])) == 0);
                    ++matches;
                }
            }
            CHECK(matches == 1U);
        }
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* SPEC18: identical members without a nominal relation never imply conformance
 * in any expected-type context; adding the relation alone repairs each source. */
static void g23_nominal_use_contexts(void) {
    static const struct { const char *body, *code, *marker, *token; } cases[] = {
        {"let view: S = item;", "AE1003", "item;", "item"},
        {"var view: S; view = item;", "AE1003", "item;", "item"},
        {"consume(item);", "AE0512", "consume(item)", "consume"},
        {"return item;", "AE1003", "item;", "item"},
        {"holder.value = item;", "AE1003", "item;", "item"},
        {"values[0] = item;", "AE1003", "item;", "item"},
        {"let converted = (S)item;", "AE1023", "(S)item", "("}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (size_t relation = 0U; relation < 2U; ++relation) {
            char source[2048];
            snprintf(source, sizeof(source), "module g23;\nspec S { let number: i32; }\n"
                "type Item { let number: i32; }\ntype Holder { var value: S; }\n%s"
                "func consume(value: S) {}\nfunc check(item: Item, holder: Holder, values: S[!])%s { %s }\n",
                relation ? "fit Item: S;\n" : "", i == 3U ? ": S" : "", cases[i].body);
            g23_source(source, relation ? NULL : cases[i].code, cases[i].marker, cases[i].token);
        }
    }
}

/* Public entry registered by the normal semantic suite. */
void test_g23_spec_fit_diagnostics(void) {
    g23_relation_matrix();
    g23_signature_matrix();
    g23_visible_fit_sources();
    g23_relation_order_and_visibility();
    g23_generic_array_instances();
    g23_declaration_diagnostics();
    g23_field_requirements();
    g23_member_conflicts();
    g23_multiple_diagnostics();
    g23_nominal_use_contexts();
    puts("G23 spec/fit declaration and source matrices passed");
}
