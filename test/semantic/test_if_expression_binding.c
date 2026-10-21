#include "parser/parser.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Check lexical binding behavior without depending on standard-library fits. */
static void check_if_expression_binding(const char *name,
                                         const char *body,
                                         const char *expected_code,
                                         const char *expected_message) {
    const char *prefix =
        "open module expression.binding;\n"
        "@throwable open spec Root {}\n"
        "open fit int: Root {}\n"
        "spec Value: int | string;\n";
    size_t size = strlen(prefix) + strlen(body) + 1U;
    char *source = malloc(size);
    FengProgram *program = NULL;
    FengParseError parse = {0};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()
    };

    CHECK(source != NULL);
    snprintf(source, size, "%s%s", prefix, body);
    if (!feng_parse_source(source, strlen(source), name, &program, &parse)) {
        fprintf(stderr, "%s: %s\n%s\n", name, parse.message, source);
        CHECK(false);
    }
    const FengProgram *programs[] = {program};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
                                                &analysis, &errors, &error_count);
    bool matches = expected_code == NULL
        ? ok && error_count == 0U
        : !ok && error_count == 1U &&
          strcmp(errors[0].code, expected_code) == 0 &&
          strcmp(errors[0].message, expected_message) == 0;
    if (!matches) {
        fprintf(stderr, "%s: expected %s\n%s\n", name,
                expected_code != NULL ? expected_code : "success", source);
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "%s %u:%u %s\n", errors[i].code,
                    errors[i].token.line, errors[i].token.column, errors[i].message);
        }
    }
    CHECK(matches);
    if (expected_code != NULL) {
        CHECK(strcmp(errors[0].path, name) == 0);
        CHECK(errors[0].token.line >= 5U && errors[0].token.column > 0U);
        CHECK(errors[0].token.length == 3U);
        CHECK(strncmp(errors[0].token.lexeme, "err", 3U) == 0);
    }
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    free(source);
}

/* Direct and parser-converted if expressions share the same binding rules. */
void test_if_expression_binding_semantic(void) {
    const struct { const char *name; const char *body; } accepted[] = {
        {"direct", "func read(v: Value): int { return if v match err: int { err } else { 0 }; }"},
        {"explicit_let", "func read(v: Value): int { return if v match let err: int { err } else { 0 }; }"},
        {"mutable", "func read(v: Value): int { return if v match var err: int { err += 1; err } else { 0 }; }"},
        {"string", "func read(v: Value): string { return if v match err: string { err } else { \"empty\" }; }"},
        {"and", "func read(v: Value): int { return if v match err: int && err > 0 { err } else { 0 }; }"},
        {"two_bindings", "func read(v: Value, w: Value): int { return if v match err: int && w match rhs: int && rhs > err { err + rhs } else { 0 }; }"},
        {"grouped", "func read(v: Value): int { return if (v match err: int) { err } else { 0 }; }"},
        {"outer_shadow", "func read(v: Value): string { let err = \"outer\"; return if v match err: int { \"inner\" } else { err }; }"},
        {"shadow_result", "func read(v: Value): int { let err = \"outer\"; return if v match err: int { err } else { 0 }; }"},
        {"body_shadow", "func read(v: Value): string { return if v match err: int { let err = \"inner\"; err } else { \"outer\" }; }"},
        {"inferred", "func read(v: Value) { return if v match err: int { err } else { 0 }; } func use(v: Value): int { return read(v); }"},
        {"inferred_literals", "func read(v: Value): bool { let result = if v match err: int && err > 0 { 1 } else { 0 }; return result == 1; }"},
        {"inferred_exit", "func read(v: Value): int { let result = if v match err: int && err > 0 { 1 } else { return 0; }; return result + 1; }"},
        {"argument", "func use(v: int): int { return v; } func read(v: Value): int { return use(if v match err: int { err } else { 0 }); }"},
        {"nested", "func read(v: Value): int { return if v match err: int { if err > 0 { err } else { 0 } } else { 0 }; }"},
        {"match_tail", "func read(v: Value, state: int): int { return match state { 0 { if v match err: int { err } else { 0 } } else { 0 } }; }"},
        {"match_else_tail", "func read(v: Value, state: int): int { return match state { 0 { 0 } else { if v match err: int { err } else { 0 } } }; }"},
        {"return_branch", "func read(v: Value): int { return if v match err: int { return err; } else { 0 }; }"},
        {"throw_branch", "func read(v: Value): int { return if v match err: int { throw err; } else { 0 }; }"},
        {"both_throw", "func read(v: Value, state: int): int { return match state { 0 { if v match err: int { throw err; } else { throw 0; } } else { 0 } }; }"},
        {"else_if", "func read(v: Value, w: Value): int { return if v match err: int { err } else if w match err: int { err } else { 0 }; }"}
    };
    for (size_t i = 0U; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        check_if_expression_binding(accepted[i].name, accepted[i].body, NULL, NULL);
    }
    const struct { const char *name; const char *body; const char *code; const char *message; } rejected[] = {
        {"else_scope", "func read(v: Value): int { return if v match err: int { return 1; } else { return err; }; }", "AE0001", "undefined identifier 'err'"},
        {"after_scope", "func read(v: Value): int { let result = if v match err: int { err } else { 0 }; return err; }", "AE0001", "undefined identifier 'err'"},
        {"or_scope", "func read(v: Value): int { return if (v match err: int) || true { return err; } else { return 0; }; }", "AE0001", "undefined identifier 'err'"},
        {"not_scope", "func read(v: Value): int { return if !(v match err: int) { return err; } else { return 0; }; }", "AE0001", "undefined identifier 'err'"},
        {"match_else_scope", "func read(v: Value, state: int): int { return match state { 0 { if v match err: int { return err; } else { return 0; } } else { return err; } }; }", "AE0001", "undefined identifier 'err'"},
        {"immutable", "func read(v: Value): int { return if v match err: int { err = 2; err } else { 0 }; }", "AE0104", "assignment target 'err' is not writable"},
        {"duplicate", "func read(v: Value): int { return if v match err: int && v match err: int { err } else { 0 }; }", "AE0105", "duplicate binding 'err' in the same scope"}
    };
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        check_if_expression_binding(rejected[i].name, rejected[i].body,
                                    rejected[i].code, rejected[i].message);
    }
    puts("if expression binding semantic tests passed (28 cases)");
}
