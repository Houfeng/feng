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

static void test_top_level_function_auto_infers_return_type_for_forward_call(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value: int = add(1, 2);\n"
        "}\n"
        "fn add(a: int, b: int) {\n"
        "    return a + b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("auto_return_forward_call_ok.f", source);
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

static void test_top_level_function_rejects_conflicting_inferred_return_types(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(flag: bool) {\n"
        "    if flag {\n"
        "        return 1;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "fn run() {\n"
        "    let value: int = pick(false);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("auto_return_conflict_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "auto_return_conflict_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "conflicting inferred return types") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_method_auto_infers_return_type_for_forward_call(void) {
    const char *source =
        "mod demo.main;\n"
        "type Counter {\n"
        "    fn value() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n"
        "fn run(counter: Counter) {\n"
        "    let value: int = counter.value();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_auto_return_forward_call_ok.f", source);
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

static void test_explicit_non_void_return_rejects_empty_return(void) {
    const char *source =
        "mod demo.main;\n"
        "fn value(): int {\n"
        "    return;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("explicit_non_void_empty_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "explicit_non_void_empty_return_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'int'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_expression_rejects_non_constant_label(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int, other: int): int {\n"
        "    return if value {\n"
        "        other + 1: 1,\n"
        "        else: 0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_non_constant_label_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "match_non_constant_label_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "match case label must be a constant expression") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_expression_rejects_incomparable_label_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    return if value {\n"
        "        \"one\": 1,\n"
        "        else: 0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_incomparable_label_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "match_incomparable_label_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "not comparable with target type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_expression_rejects_inconsistent_result_types(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    return if value {\n"
        "        1: 1,\n"
        "        else: \"zero\"\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_inconsistent_result_types_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "match_inconsistent_result_types_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "match expression branches must have the same type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_untyped_lambda_binding_is_callable(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): int {\n"
        "    let func = (x: int) -> x * 2;\n"
        "    return func(2);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("untyped_lambda_binding_call_ok.f", source);
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

static void test_untyped_lambda_binding_matches_named_function_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type IntToInt(x: int): int;\n"
        "fn run(): int {\n"
        "    let func = (x: int) -> x * 2;\n"
        "    let typed: IntToInt = func;\n"
        "    return typed(3);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("untyped_lambda_binding_function_type_ok.f", source);
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

static void test_function_typed_call_result_rejects_non_matching_binding_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type IntPicker(a: int): int;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    let picker: IntPicker = pick;\n"
        "    let flag: bool = picker(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("call_result_binding_mismatch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "call_result_binding_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_call_result_rejects_non_matching_binding_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    let flag: bool = pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("top_level_call_result_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "top_level_call_result_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_call_result_rejects_non_matching_return_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn run(): bool {\n"
        "    return pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("top_level_call_result_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "top_level_call_result_return_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_method_call_result_rejects_non_matching_binding_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    fn pick(a: int): int {\n"
        "        return a;\n"
        "    }\n"
        "}\n"
        "fn run(user: User) {\n"
        "    let flag: bool = user.pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("method_call_result_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "method_call_result_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_local_assignment_rejects_non_matching_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var flag: bool = true;\n"
        "    flag = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("local_assign_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "local_assign_type_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_member_assignment_rejects_non_matching_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var flag: bool;\n"
        "    fn set() {\n"
        "        self.flag = 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("member_assign_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "member_assign_type_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_field_value_rejects_non_matching_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var flag: bool;\n"
        "}\n"
        "fn make(): User {\n"
        "    return User { flag: 1 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_literal_field_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_literal_field_type_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_local_let_assignment_rejects_non_writable_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let count: int = 0;\n"
        "    count = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("local_let_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "local_let_assign_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_default_parameter_assignment_rejects_non_writable_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(total: int) {\n"
        "    total = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("default_param_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "default_param_assign_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_var_parameter_assignment_is_writable(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(var total: int) {\n"
        "    total = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("var_param_assign_ok.f", source);
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

static void test_top_level_let_assignment_rejects_non_writable_target(void) {
    const char *source =
        "mod demo.main;\n"
        "let count: int = 0;\n"
        "fn run() {\n"
        "    count = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("top_level_let_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "top_level_let_assign_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_instance_let_member_assignment_rejects_non_writable_target(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    let id: int = 0;\n"
        "}\n"
        "fn set(var user: User) {\n"
        "    user.id = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("instance_let_member_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "instance_let_member_assign_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_alias_public_let_binding_assignment_rejects_non_writable_target(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu let count: int = 0;\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn run() {\n"
        "    base.count = 1;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("alias_assign_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_assign_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_assign_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_index_assignment_accepts_explicit_array_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    items[0] = 4;\n"
        "    let first: int = items[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_assign_ok.f", source);
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

static void test_index_assignment_rejects_non_matching_array_element_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    items[0] = true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_assign_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "index_assign_type_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'int'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_inferred_array_literal_binding_supports_index_read_write(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items = [1, 2, 3];\n"
        "    items[0] = 4;\n"
        "    let first: int = items[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("inferred_array_index_ok.f", source);
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

static void test_inferred_array_literal_binding_rejects_non_matching_index_assignment(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items = [1, 2, 3];\n"
        "    items[0] = true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("inferred_array_index_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "inferred_array_index_type_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'int'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_inferred_array_literal_rejects_mixed_element_types(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items = [1, true];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("mixed_array_literal_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "mixed_array_literal_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "array literal element at index 1 does not match expected type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_inferred_nested_array_literal_supports_nested_index_read_write(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var matrix = [[1, 2], [3, 4]];\n"
        "    matrix[0][1] = 5;\n"
        "    let value: int = matrix[1][0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("nested_array_index_ok.f", source);
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

static void test_empty_array_literal_binding_requires_explicit_target_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items = [];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("empty_array_literal_type_context_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "empty_array_literal_type_context_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "empty array literal requires an explicit target array type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_empty_array_literal_binding_accepts_explicit_target_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("empty_array_literal_typed_ok.f", source);
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

static void test_explicit_numeric_and_exact_casts_pass(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    var index: u8 = (u8)1;\n"
        "    let value: int = items[index];\n"
        "    let small: i32 = (i32)value;\n"
        "    let ratio: float = (float)small;\n"
        "    let flag: bool = (bool)false;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_and_integer_index_ok.f", source);
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

static void test_cast_rejects_bool_to_numeric(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value: int = (int)true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_bool_to_numeric_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "cast_bool_to_numeric_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "cast from 'bool' to 'int' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_bool(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let flag: bool = (bool)1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_bool_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "cast_numeric_to_bool_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "to 'bool' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_string_to_numeric(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value: int = (int)\"12\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_string_to_numeric_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "cast_string_to_numeric_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "cast from 'string' to 'int' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_array_to_numeric(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    let value: int = (int)items;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_array_to_numeric_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "cast_array_to_numeric_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "cast from 'int[]' to 'int' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_index_expression_rejects_float_operand(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    let value: int = items[1.5];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_float_operand_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "index_float_operand_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "index expression requires an integer operand") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_index_expression_rejects_bool_operand(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    let value: int = items[true];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_bool_operand_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "index_bool_operand_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "index expression requires an integer operand") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_index_expression_rejects_non_array_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var value: int = 1;\n"
        "    let item: int = value[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_non_array_target_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "index_non_array_target_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "index expression target must have array type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_index_assignment_rejects_non_array_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var value: int = 1;\n"
        "    value[0] = 2;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_assign_non_array_target_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "index_assign_non_array_target_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "index expression target must have array type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_minus_rejects_non_numeric_operand(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    -true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_minus_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_minus_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "unary operator '-' requires a numeric operand") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_not_rejects_non_bool_operand(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    !1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_not_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_not_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "unary operator '!' requires a bool operand") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_binary_plus_rejects_non_matching_operands(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    1 + true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("binary_plus_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "binary_plus_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '+' requires operands of the same numeric or string type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_binary_and_rejects_non_bool_operands(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    1 && true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("binary_and_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "binary_and_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "binary operator '&&' requires bool operands") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_if_expression_rejects_non_bool_condition(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value = if 1 { 2 } else { 3 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_condition_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "if_expr_condition_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "if expression condition must have type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_if_expression_requires_matching_branch_types(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value = if true { 1 } else { \"two\" };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_branch_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "if_expr_branch_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "if expression branches must have the same type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_valid_unary_binary_and_if_expressions_pass(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let flag: bool = !false && 1 < 2;\n"
        "    let value: int = if flag { 1 + 2 } else { 3 + 4 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("valid_expr_type_checks_ok.f", source);
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

static void test_if_statement_rejects_non_bool_condition(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    if 1 {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_stmt_condition_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "if_stmt_condition_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "if statement condition must have type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_while_statement_rejects_non_bool_condition(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    while 1 {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("while_stmt_condition_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "while_stmt_condition_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "while statement condition must have type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_for_statement_rejects_non_bool_condition(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    for ; 1; {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("for_stmt_condition_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "for_stmt_condition_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "for statement condition must have type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_valid_statement_conditions_pass(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    if true {}\n"
        "    while false {}\n"
        "    for var i = 0; i < 1; i = i + 1 {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("valid_stmt_condition_checks_ok.f", source);
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

static void test_for_statement_accepts_empty_condition(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    for ; ; {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("for_stmt_empty_condition_ok.f", source);
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
    test_top_level_function_auto_infers_return_type_for_forward_call();
    test_top_level_function_rejects_conflicting_inferred_return_types();
    test_method_auto_infers_return_type_for_forward_call();
    test_explicit_non_void_return_rejects_empty_return();
    test_match_expression_rejects_non_constant_label();
    test_match_expression_rejects_incomparable_label_type();
    test_match_expression_rejects_inconsistent_result_types();
    test_untyped_lambda_binding_is_callable();
    test_untyped_lambda_binding_matches_named_function_type();
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
    test_function_typed_call_result_rejects_non_matching_binding_type();
    test_top_level_call_result_rejects_non_matching_binding_type();
    test_top_level_call_result_rejects_non_matching_return_type();
    test_method_call_result_rejects_non_matching_binding_type();
    test_local_assignment_rejects_non_matching_type();
    test_member_assignment_rejects_non_matching_type();
    test_object_literal_field_value_rejects_non_matching_type();
    test_local_let_assignment_rejects_non_writable_target();
    test_default_parameter_assignment_rejects_non_writable_target();
    test_var_parameter_assignment_is_writable();
    test_top_level_let_assignment_rejects_non_writable_target();
    test_instance_let_member_assignment_rejects_non_writable_target();
    test_alias_public_let_binding_assignment_rejects_non_writable_target();
    test_index_assignment_accepts_explicit_array_target();
    test_index_assignment_rejects_non_matching_array_element_type();
    test_inferred_array_literal_binding_supports_index_read_write();
    test_inferred_array_literal_binding_rejects_non_matching_index_assignment();
    test_inferred_array_literal_rejects_mixed_element_types();
    test_inferred_nested_array_literal_supports_nested_index_read_write();
    test_empty_array_literal_binding_requires_explicit_target_type();
    test_empty_array_literal_binding_accepts_explicit_target_type();
    test_explicit_numeric_and_exact_casts_pass();
    test_cast_rejects_bool_to_numeric();
    test_cast_rejects_numeric_to_bool();
    test_cast_rejects_string_to_numeric();
    test_cast_rejects_array_to_numeric();
    test_index_expression_rejects_float_operand();
    test_index_expression_rejects_bool_operand();
    test_index_expression_rejects_non_array_target();
    test_index_assignment_rejects_non_array_target();
    test_unary_minus_rejects_non_numeric_operand();
    test_unary_not_rejects_non_bool_operand();
    test_binary_plus_rejects_non_matching_operands();
    test_binary_and_rejects_non_bool_operands();
    test_if_expression_rejects_non_bool_condition();
    test_if_expression_requires_matching_branch_types();
    test_valid_unary_binary_and_if_expressions_pass();
    test_if_statement_rejects_non_bool_condition();
    test_while_statement_rejects_non_bool_condition();
    test_for_statement_rejects_non_bool_condition();
    test_valid_statement_conditions_pass();
    test_for_statement_accepts_empty_condition();
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
