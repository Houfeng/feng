#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "semantic/semantic.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static FengProgram *parse_program_or_die(const char *path, const char *source) {
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), path, &program, &error));
    ASSERT(program != NULL);
    return program;
}

static void test_duplicate_type_across_files_same_module(void) {
    const char *source_a =
        "pu mod demo.main;\n"
        "type User {}\n";
    const char *source_b =
        "pu mod demo.main;\n"
        "type User {}\n";
    FengProgram *program_a = parse_program_or_die("type_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("type_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(analysis == NULL);
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "type_b.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "duplicate type declaration 'User'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_duplicate_binding_across_files_same_module(void) {
    const char *source_a =
        "mod demo.main;\n"
        "let name: string = \"a\";\n";
    const char *source_b =
        "mod demo.main;\n"
        "var name: string = \"b\";\n";
    FengProgram *program_a = parse_program_or_die("binding_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("binding_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "binding_b.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "duplicate top-level binding 'name'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_function_return_only_overload_error(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: int): string {\n"
        "    return \"value\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("return_overload.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "cannot differ only by return type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_module_visibility_conflict(void) {
    const char *source_a = "pu mod demo.main;\n";
    const char *source_b = "mod demo.main;\n";
    FengProgram *program_a = parse_program_or_die("visibility_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("visibility_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "visibility_b.f") == 0);
    ASSERT(errors[0].token.line == 1U);
    ASSERT(strstr(errors[0].message, "must use the same module visibility") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_valid_function_overload_by_parameter_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("valid_overload.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_call_selects_overload_by_literal_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run(): int {\n"
        "    return pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("call_overload_literal_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_call_selects_overload_by_inferred_local_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run(): int {\n"
        "    let value = 1;\n"
        "    return pick(value);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("call_overload_local_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_call_reports_type_mismatch(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    pick(true);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("call_overload_type_mismatch.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "call_overload_type_mismatch.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message, "top-level function 'pick' has no overload accepting 1 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_imported_function_call_selects_overload_by_literal_type(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "pu fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "fn run(): int {\n"
        "    return pick(1);\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("imported_call_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("imported_call_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_alias_function_call_selects_overload_by_literal_type(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "pu fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn run(): int {\n"
        "    return base.pick(1);\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_call_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_call_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_method_call_selects_overload_by_literal_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn pick(a: int): int {\n"
        "        return a;\n"
        "    }\n"
        "    fn pick(a: string): string {\n"
        "        return a;\n"
        "    }\n"
        "}\n"
        "fn run(user: User): int {\n"
        "    return user.pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_call_overload_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_function_typed_local_binding_is_callable(void) {
    const char *source =
        "mod demo.main;\n"
        "type Picker(a: int): int;\n"
        "fn run(): int {\n"
        "    let pick: Picker = (a: int) -> a;\n"
        "    return pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_typed_local_call_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_non_callable_local_binding_reports_error(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value = 1;\n"
        "    value(2);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("non_callable_local_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "non_callable_local_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "expression 'value' is not callable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_private_method_is_inaccessible_across_modules(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu type User {\n"
        "    pr fn secret(): int {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "fn run(user: User): int {\n"
        "    return user.secret();\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("private_method_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("private_method_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "private_method_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "member 'secret' of type 'User' is not accessible") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_top_level_function_value_selects_overload_by_explicit_binding_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type IntPicker(a: int): int;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run(): int {\n"
        "    let picker: IntPicker = pick;\n"
        "    return picker(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_binding_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_value_selects_overload_by_parameter_context(void) {
    const char *source =
        "mod demo.main;\n"
        "type IntPicker(a: int): int;\n"
        "fn apply(picker: IntPicker): int {\n"
        "    return picker(1);\n"
        "}\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run(): int {\n"
        "    return apply(pick);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_arg_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_value_selects_overload_by_return_type_context(void) {
    const char *source =
        "mod demo.main;\n"
        "type IntPicker(a: int): int;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn make(): IntPicker {\n"
        "    return pick;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_return_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_value_requires_explicit_type_when_overloaded(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    let picker = pick;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_requires_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "function_value_requires_type_error.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message, "requires an explicit target function type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_method_value_selects_overload_by_explicit_binding_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type M0(): void;\n"
        "type User {\n"
        "    fn say() {}\n"
        "    fn say(msg: string) {}\n"
        "}\n"
        "fn run(user: User) {\n"
        "    let action: M0 = user.say;\n"
        "    action();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_value_binding_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_method_value_selects_overload_by_parameter_context(void) {
    const char *source =
        "mod demo.main;\n"
        "type M0(): void;\n"
        "type User {\n"
        "    fn say() {}\n"
        "    fn say(msg: string) {}\n"
        "}\n"
        "fn apply(action: M0) {\n"
        "    action();\n"
        "}\n"
        "fn run(user: User) {\n"
        "    apply(user.say);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_value_arg_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_method_value_selects_overload_by_return_type_context(void) {
    const char *source =
        "mod demo.main;\n"
        "type M0(): void;\n"
        "type User {\n"
        "    fn say() {}\n"
        "    fn say(msg: string) {}\n"
        "}\n"
        "fn make(user: User): M0 {\n"
        "    return user.say;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_value_return_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_method_value_requires_explicit_type_when_overloaded(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn say() {}\n"
        "    fn say(msg: string) {}\n"
        "}\n"
        "fn run(user: User) {\n"
        "    let action = user.say;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_value_requires_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "method_value_requires_type_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "requires an explicit target function type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_function_value_binding_rejects_non_matching_target_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type BoolPicker(a: bool): bool;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    let picker: BoolPicker = pick;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_binding_mismatch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "function_value_binding_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 10U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'BoolPicker'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_function_value_return_rejects_non_matching_target_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type BoolPicker(a: bool): bool;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "fn make(): BoolPicker {\n"
        "    return pick;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_return_mismatch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "function_value_return_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 10U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'BoolPicker'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_alias_function_value_argument_rejects_non_matching_target_type(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "pu fn pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "type BoolPicker(a: bool): bool;\n"
        "fn accept(picker: BoolPicker) {}\n"
        "fn run() {\n"
        "    accept(base.pick);\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_function_value_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_function_value_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_function_value_main.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "top-level function 'accept' has no overload accepting 1 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_method_value_argument_rejects_non_matching_target_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type BoolAction(flag: bool): bool;\n"
        "type User {\n"
        "    fn say() {}\n"
        "    fn say(msg: string) {}\n"
        "}\n"
        "fn accept(action: BoolAction) {}\n"
        "fn run(user: User) {\n"
        "    accept(user.say);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_value_argument_mismatch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "method_value_argument_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message, "top-level function 'accept' has no overload accepting 1 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_missing_use_target_module(void) {
    const char *source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "fn main() {}\n";
    FengProgram *program = parse_program_or_die("missing_use_target.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "use target module 'demo.base' was not found") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_imported_type_conflicts_with_local_type(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu type User {}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "type User {}\n";
    FengProgram *base_program = parse_program_or_die("base_type.f", base_source);
    FengProgram *main_program = parse_program_or_die("main_type_conflict.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "main_type_conflict.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "imported type 'User'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_imported_value_conflicts_with_local_value(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "fn load(): int {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("base_value.f", base_source);
    FengProgram *main_program = parse_program_or_die("main_value_conflict.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "main_value_conflict.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "imported name 'load'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_imported_name_conflicts_between_modules(void) {
    const char *source_a =
        "pu mod demo.a;\n"
        "pu fn load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "pu mod demo.b;\n"
        "pu fn load(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.a;\n"
        "use demo.b;\n"
        "fn main() {}\n";
    FengProgram *program_a = parse_program_or_die("import_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("import_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("import_conflict_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "import_conflict_main.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "imported name 'load'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_alias_import_does_not_inject_short_names(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn load(): int {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_duplicate_use_alias_in_same_file(void) {
    const char *source_a =
        "pu mod demo.a;\n"
        "pu fn load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "pu mod demo.b;\n"
        "pu fn store(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.a as tools;\n"
        "use demo.b as tools;\n"
        "fn main() {}\n";
    FengProgram *program_a = parse_program_or_die("alias_dup_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("alias_dup_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("alias_dup_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_dup_main.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "duplicate use alias 'tools'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_undefined_identifier_in_function_body(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(): int {\n"
        "    return missing;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("undefined_identifier.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "undefined_identifier.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "undefined identifier 'missing'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unknown_type_reference_in_function_signature(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(value: Missing): int {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unknown_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unknown_type.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "unknown type 'Missing'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_self_is_valid_inside_type_method(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    fn read(): int {\n"
        "        return self.id;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("self_method_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_self_is_invalid_outside_type_method(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(): int {\n"
        "    return self.id;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("self_top_level_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "self_top_level_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "'self' is only available") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_self_is_invalid_inside_lambda(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    fn read(): int {\n"
        "        let thunk = () -> self.id;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("self_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "self_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "'self' is only available") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_alias_member_access_resolves_public_names(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu type User {}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn make(): base.User {\n"
        "    return base.User {};\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_member_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_member_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_alias_member_access_reports_missing_public_name(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn main(): int {\n"
        "    return base.store();\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_missing_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_missing_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_missing_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not export public name 'store'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_alias_identifier_requires_member_access(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn main() {\n"
        "    base;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_ident_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_ident_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_ident_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "module alias 'base' must be accessed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_self_reports_unknown_member(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    fn read(): int {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("self_unknown_member.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "self_unknown_member.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "has no member 'name'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_reports_unknown_field(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "}\n"
        "fn make(): User {\n"
        "    return User { name: 1 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_literal_unknown_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_literal_unknown_field.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "object literal field 'name'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_requires_object_type_target(void) {
    const char *source =
        "mod demo.main;\n"
        "type Factory(): int;\n"
        "fn make() {\n"
        "    Factory {};\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_literal_non_object_target.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_literal_non_object_target.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "must resolve to an object type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_accepts_constructor_call_target(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    fn User() {}\n"
        "}\n"
        "fn make(): User {\n"
        "    return User() { id: 1 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_literal_ctor_target_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_constructor_call_uses_implicit_default_constructor(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {}\n"
        "fn make(): User {\n"
        "    return User();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_implicit_default_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_constructor_call_reports_missing_zero_arg_constructor(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn User(name: string) {}\n"
        "}\n"
        "fn make(): User {\n"
        "    return User();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_missing_zero_arg.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_missing_zero_arg.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "no accessible constructor accepting 0 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_call_selects_overload_by_literal_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn User(id: i64) {}\n"
        "    fn User(name: string) {}\n"
        "}\n"
        "fn make(): User {\n"
        "    return User(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_overload_literal_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_constructor_call_selects_overload_by_inferred_local_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn User(id: i64) {}\n"
        "    fn User(name: string) {}\n"
        "}\n"
        "fn make(): User {\n"
        "    let id = 1;\n"
        "    return User(id);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_overload_local_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_constructor_call_reports_type_mismatch(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn User(id: int) {}\n"
        "    fn User(name: string) {}\n"
        "}\n"
        "fn make(): User {\n"
        "    return User(true);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_type_mismatch.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_type_mismatch.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "no accessible constructor accepting 1 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_call_rejects_function_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type Factory(): int;\n"
        "fn make() {\n"
        "    Factory();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_non_object_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_non_object_type.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not an object type and cannot be constructed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_reports_inaccessible_imported_constructor(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu type User {\n"
        "    pr fn User() {}\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "fn make(): User {\n"
        "    return User {};\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("ctor_import_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("ctor_import_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_import_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "no accessible constructor accepting 0 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_object_literal_rejects_decl_bound_let_member(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int = 1;\n"
        "}\n"
        "fn make(): User {\n"
        "    return User { id: 2 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_decl_object_literal_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "let_decl_object_literal_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "declaration initializer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_rejects_decl_bound_let_member_assignment(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int = 1;\n"
        "    fn User() {\n"
        "        self.id = 2;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_decl_ctor_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "let_decl_ctor_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "declaration initializer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_rejects_repeated_let_member_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    fn User() {\n"
        "        self.id = 1;\n"
        "        self.id = 2;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_repeat_ctor_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "let_repeat_ctor_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "more than once in constructor") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_method_rejects_let_member_assignment(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    fn set() {\n"
        "        self.id = 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_method_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "let_method_assign_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "cannot be directly assigned outside constructors") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_rejects_ctor_bound_let_member(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    fn User() {\n"
        "        self.id = 1;\n"
        "    }\n"
        "}\n"
        "fn make(): User {\n"
        "    return User() { id: 2 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_ctor_object_literal_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "let_ctor_object_literal_error.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message, "already completed by constructor") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_rejects_ctor_bound_let_member_for_selected_overload(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    fn User(id: int) {\n"
        "        self.id = id;\n"
        "    }\n"
        "    fn User(name: string) {}\n"
        "}\n"
        "fn make_ok(): User {\n"
        "    return User(\"ok\") { id: 1 };\n"
        "}\n"
        "fn make_bad(): User {\n"
        "    return User(1) { id: 2 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_ctor_selected_overload_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "let_ctor_selected_overload_error.f") == 0);
    ASSERT(errors[0].token.line == 13U);
    ASSERT(strstr(errors[0].message, "already completed by constructor") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_allows_unbound_let_member(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "}\n"
        "fn make(): User {\n"
        "    return User { id: 2 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_object_literal_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_object_literal_rejects_duplicate_fields(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "}\n"
        "fn make(): User {\n"
        "    return User { id: 1, id: 2 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_literal_duplicate_field_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_literal_duplicate_field_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "duplicate object literal field 'id'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_rejects_inaccessible_private_field(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu type User {\n"
        "    pr var secret: int;\n"
        "    pu fn User() {}\n"
        "}\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "fn make(): User {\n"
        "    return User { secret: 1 };\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("object_literal_private_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("object_literal_private_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_literal_private_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not accessible for type 'User'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_object_literal_allows_private_field_inside_same_module(void) {
    const char *source_a =
        "mod demo.main;\n"
        "type User {\n"
        "    pr var secret: int;\n"
        "    fn User() {}\n"
        "}\n";
    const char *source_b =
        "mod demo.main;\n"
        "fn make(): User {\n"
        "    return User { secret: 1 };\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("object_literal_same_module_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("object_literal_same_module_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

int main(void) {
    test_duplicate_type_across_files_same_module();
    test_duplicate_binding_across_files_same_module();
    test_function_return_only_overload_error();
    test_module_visibility_conflict();
    test_valid_function_overload_by_parameter_type();
    test_top_level_function_call_selects_overload_by_literal_type();
    test_top_level_function_call_selects_overload_by_inferred_local_binding();
    test_top_level_function_call_reports_type_mismatch();
    test_imported_function_call_selects_overload_by_literal_type();
    test_alias_function_call_selects_overload_by_literal_type();
    test_method_call_selects_overload_by_literal_type();
    test_function_typed_local_binding_is_callable();
    test_non_callable_local_binding_reports_error();
    test_private_method_is_inaccessible_across_modules();
    test_top_level_function_value_selects_overload_by_explicit_binding_type();
    test_top_level_function_value_selects_overload_by_parameter_context();
    test_top_level_function_value_selects_overload_by_return_type_context();
    test_top_level_function_value_requires_explicit_type_when_overloaded();
    test_top_level_function_value_binding_rejects_non_matching_target_type();
    test_top_level_function_value_return_rejects_non_matching_target_type();
    test_method_value_selects_overload_by_explicit_binding_type();
    test_method_value_selects_overload_by_parameter_context();
    test_method_value_selects_overload_by_return_type_context();
    test_method_value_requires_explicit_type_when_overloaded();
    test_alias_function_value_argument_rejects_non_matching_target_type();
    test_method_value_argument_rejects_non_matching_target_type();
    test_missing_use_target_module();
    test_imported_type_conflicts_with_local_type();
    test_imported_value_conflicts_with_local_value();
    test_imported_name_conflicts_between_modules();
    test_alias_import_does_not_inject_short_names();
    test_duplicate_use_alias_in_same_file();
    test_undefined_identifier_in_function_body();
    test_unknown_type_reference_in_function_signature();
    test_self_is_valid_inside_type_method();
    test_self_is_invalid_outside_type_method();
    test_self_is_invalid_inside_lambda();
    test_alias_member_access_resolves_public_names();
    test_alias_member_access_reports_missing_public_name();
    test_alias_identifier_requires_member_access();
    test_self_reports_unknown_member();
    test_object_literal_reports_unknown_field();
    test_object_literal_requires_object_type_target();
    test_object_literal_accepts_constructor_call_target();
    test_constructor_call_uses_implicit_default_constructor();
    test_constructor_call_reports_missing_zero_arg_constructor();
    test_constructor_call_selects_overload_by_literal_type();
    test_constructor_call_selects_overload_by_inferred_local_binding();
    test_constructor_call_reports_type_mismatch();
    test_constructor_call_rejects_function_type();
    test_object_literal_reports_inaccessible_imported_constructor();
    test_object_literal_rejects_decl_bound_let_member();
    test_constructor_rejects_decl_bound_let_member_assignment();
    test_constructor_rejects_repeated_let_member_binding();
    test_method_rejects_let_member_assignment();
    test_object_literal_rejects_ctor_bound_let_member();
    test_object_literal_rejects_ctor_bound_let_member_for_selected_overload();
    test_object_literal_allows_unbound_let_member();
    test_object_literal_rejects_duplicate_fields();
    test_object_literal_rejects_inaccessible_private_field();
    test_object_literal_allows_private_field_inside_same_module();
    puts("semantic tests passed");
    return 0;
}
