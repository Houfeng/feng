#include "test_union_entry_order.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: union order failed: %s\n", __FILE__, __LINE__, #expr); \
    exit(1); } } while (0)

/* Analyze independently at each target width; no generated host code is
 * needed to verify the compiler's complete path and rejection decisions. */
static FengSemanticAnalysis *analyze(const char *source, const char *file,
                                     size_t width, const char *error_code,
                                     FengProgram **program) {
    FengParseError parse_error = {0};
    CHECK(feng_parse_source(source, strlen(source), file, program, &parse_error));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {0};
    options.target = FENG_COMPILE_TARGET_LIB;
    options.pointer_size = width;
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
        &analysis, &errors, &count);
    bool any_ae = error_code != NULL && strcmp(error_code, "AE") == 0;
    if (ok != (error_code == NULL) || (error_code != NULL &&
        (count == 0U || (!any_ae && (count != 1U || strcmp(errors[0].code, error_code) != 0))))) {
        fprintf(stderr, "union order source:\n%s\n", source);
        for (size_t i = 0U; i < count; ++i)
            fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(ok == (error_code == NULL));
    CHECK(any_ae ? count > 0U : count == (error_code == NULL ? 0U : 1U));
    if (error_code != NULL) {
        if (any_ae) {
            for (size_t i = 0U; i < count; ++i) CHECK(strncmp(errors[i].code, "AE", 2U) == 0);
        } else {
            CHECK(strcmp(errors[0].code, error_code) == 0);
        }
        CHECK(strcmp(errors[0].path, file) == 0);
        CHECK(errors[0].token.length > 0U);
    }
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Every entrance has an otherwise identical valid neighbor. Invalid
 * programs must parse and fail in Semantic, never in Codegen or a backend. */
static void entrance_rejections(void) {
    const char *entrances[] = {
        "let value: U = %s;",
        "func run() { var value: U = 7; value = %s; }",
        "func take(value: U) {} func run() { take(%s); }",
        "func run(): U { return %s; }",
        "type Cell { open var value: U; } func run() { let cell = Cell { value: %s }; }",
        "type Cell { open var value: U; } func run() { let cell = Cell(); cell.value = %s; }",
        "type Cell { var value: U = %s; }",
        "type Cell { func Cell(value: U) {} } func run() { Cell(%s); }",
        "type Cell { func take(value: U) {} } func run() { Cell().take(%s); }",
        "let values: U[] = [%s];",
        "func run() { let values: U[!] = [7]; values[0] = %s; }",
        "let value: U = if true { %s } else { 7 };",
        "let value: U = match true { true { %s } else { 7 } };",
        "let value: U = try 7 catch error: string { %s };"
    };
    const char *values[] = {"7", "32768", "1.5", "n"};
    for (size_t entry = 0U; entry < sizeof(entrances) / sizeof(entrances[0]); ++entry) {
        for (size_t value = 0U; value < sizeof(values) / sizeof(values[0]); ++value) {
            char body[1024], source[1536];
            snprintf(body, sizeof(body), entrances[entry], values[value]);
            snprintf(source, sizeof(source), "module unionorder;\n"
                "spec L: i16 | string; spec U: L | bool; let n: i32 = 7;\n%s\n", body);
            FengProgram *program = NULL;
            FengSemanticAnalysis *analysis = analyze(source, "union_entrance.ff",
                feng_get_host_pointer_size(), value == 0U ? NULL : "AE", &program);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
    }
}

/* Verify the actual Semantic sidecar, not just successful compilation. */
void test_assert_union_entry_path(const char *source, const char *file,
                                  const size_t *indices, size_t count) {
    for (size_t width = 4U; width <= 8U; width += 4U) {
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = analyze(source, file, width, NULL, &program);
        CHECK(analysis->union_coercion_site_count == 1U);
        const FengUnionCoercionSite *site = &analysis->union_coercion_sites[0];
        CHECK(site->path_length == count);
        for (size_t i = 0U; i < count; ++i) {
            if (site->path_indices[i] != indices[i]) fprintf(stderr, "%s\n", source);
            CHECK(site->path_indices[i] == indices[i]);
        }
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* Independent cases distinguish global BFS from per-subtree recursion,
 * retain the default-type exact pass, and preserve repeated leaf paths. */
void test_union_entry_order(void) {
    entrance_rejections();
    const struct {
        const char *declarations;
        const char *entry;
        size_t path[4];
        size_t count;
    } positives[] = {
        {"spec L: i8 | string; spec U: L | i16;", "let v: U = 7;", {1}, 1},
        {"spec D: i8 | string; spec L: D | bool; spec R: i16 | string; spec U: L | R;",
         "let v: U = 7;", {1, 0}, 2},
        {"spec L: i16 | string; spec R: i16 | bool; spec U: L | R;",
         "let v: U = 7;", {0, 0}, 2},
        {"spec L: i16 | string; spec R: i16 | bool; spec U: R | L;",
         "let v: U = 7;", {0, 0}, 2},
        {"spec L: int | string; spec U: L | f64;", "let v: U = 7;", {0, 0}, 2},
        {"spec U: f32 | f64;", "let v: U = 1.5;", {1}, 1},
        {"spec L: i16 | bool; spec U: string | L;", "let v: U = 1 + 2 * 3;", {1, 0}, 2},
        {"spec L: f32 | bool; spec U: string | L;", "let v: U = 1.0 + 2.0;", {1, 0}, 2},
        {"spec L: u8 | i16; spec U: L | bool;", "let v: U = -1;", {0, 1}, 2},
        {"spec L: i8 | i16; spec U: bool | L;", "let v: U = 128;", {1, 1}, 2},
        {"spec Box<T>: T | bool; spec U: Box<i16> | string;", "let v: U = 7;", {0, 0}, 2},
        {"spec Box<T>: T | bool; spec U: Box<Box<i16>> | string;", "let v: U = 7;", {0, 0, 0}, 3},
        {"spec A {} spec B {} type Item: A, B {} spec D: Item | string; spec U: A | D;",
         "func enter(x: Item): U { return x; }", {1, 0}, 2},
        {"spec A {} spec B {} type Item: A, B {} spec D: A | string; spec L: D | bool; spec R: B | string; spec U: L | R;",
         "func enter(x: Item): U { return x; }", {1, 0}, 2},
        {"type Item {} spec D: Item | string; spec L: D | bool; spec R: Item | string; spec U: L | R;",
         "func enter(x: Item): U { return x; }", {1, 0}, 2},
        {"spec A {} spec B {} type Item: A, B {} spec U: B | A;",
         "func enter(x: Item): U { return x; }", {0}, 1},
        {"spec Parent {} spec Child: Parent {} type Item: Child {} spec U: Parent | Child;",
         "func enter(x: Item): U { return x; }", {0}, 1},
        {"spec Parent {} spec Child: Parent {} type Item: Child {} spec U: Child | Parent;",
         "func enter(x: Item): U { return x; }", {0}, 1},
        {"spec Parent {} spec Child: Parent {} spec U: Parent | Child;",
         "func enter(x: Child): U { return x; }", {1}, 1},
        {"spec L: int | string; spec U: L | f64; spec Other: f32 | i16; "
         "func pick(v: Other, tag: bool) {} func pick(v: U, tag: string) {}",
         "func run() { pick(7, \"chosen\"); }", {0, 0}, 2},
        {"spec L: int | string; spec U: L | f64; spec Other: f32 | i16; "
         "func pick(v: U, tag: string) {} func pick(v: Other, tag: bool) {}",
         "func run() { pick(7, \"chosen\"); }", {0, 0}, 2}
    };
    for (size_t i = 0U; i < sizeof(positives) / sizeof(positives[0]); ++i) {
        char source[2048];
        snprintf(source, sizeof(source), "module unionorder;\n%s\n%s\n",
            positives[i].declarations, positives[i].entry);
        test_assert_union_entry_path(source, "union_order.ff", positives[i].path, positives[i].count);
    }
    const char *negatives[] = {
        "spec L: i8 | bool; spec U: L | string; let v: U = 128;",
        "spec L: u8 | bool; spec U: L | string; let v: U = -1;",
        "spec L: i16 | bool; spec U: L | string; let v: U = 1.5;",
        "spec L: i16 | bool; spec U: L | string; let n: int = 7; let v: U = n;",
        "spec A {} type Item {} spec U: A | string; let v: U = Item();"
    };
    for (size_t i = 0U; i < sizeof(negatives) / sizeof(negatives[0]); ++i) {
        for (size_t width = 4U; width <= 8U; width += 4U) {
            char source[1024];
            snprintf(source, sizeof(source), "module unionorder;\n%s\n", negatives[i]);
            FengProgram *program = NULL;
            FengSemanticAnalysis *analysis = analyze(source, "union_order_bad.ff", width, "AE1003", &program);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
    }
    /* Ordered members cannot resolve an otherwise invalid overload set. */
    for (size_t width = 4U; width <= 8U; width += 4U) {
        const char *source = "module unionorder; "
            "spec A: i16 | bool; spec B: i16 | string; "
            "func pick(v: A) {} func pick(v: B) {} "
            "func run() { pick(7); }";
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = analyze(source, "union_order_overload_bad.ff",
            width, "AE", &program);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    puts("Union two-pass entry semantic matrices passed");
}
