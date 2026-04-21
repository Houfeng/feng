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

int main(void) {
    test_duplicate_type_across_files_same_module();
    test_duplicate_binding_across_files_same_module();
    test_function_return_only_overload_error();
    test_module_visibility_conflict();
    test_valid_function_overload_by_parameter_type();
    test_missing_use_target_module();
    test_imported_type_conflicts_with_local_type();
    test_imported_value_conflicts_with_local_value();
    test_imported_name_conflicts_between_modules();
    test_alias_import_does_not_inject_short_names();
    test_duplicate_use_alias_in_same_file();
    puts("semantic tests passed");
    return 0;
}
