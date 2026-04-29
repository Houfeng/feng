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

    if (!feng_parse_source(source, strlen(source), path, &program, &error)) {
        fprintf(stderr,
                "parse failed for %s at %u:%u: %s\n",
                path,
                error.token.line,
                error.token.column,
                error.message != NULL ? error.message : "unknown parse error");
        ASSERT(false);
    }
    ASSERT(program != NULL);
    return program;
}

/* --- AST call-expr traversal helpers (used by resolved-callable tests) --- */

typedef struct CallList {
    const FengExpr **items;
    size_t count;
    size_t capacity;
} CallList;

static void call_list_push(CallList *list, const FengExpr *expr) {
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity == 0U ? 8U : list->capacity * 2U;
        const FengExpr **resized = (const FengExpr **)realloc(
            list->items, new_cap * sizeof(*resized));
        ASSERT(resized != NULL);
        list->items = resized;
        list->capacity = new_cap;
    }
    list->items[list->count++] = expr;
}

static void collect_calls_in_expr(const FengExpr *expr, CallList *out);
static void collect_calls_in_stmt(const FengStmt *stmt, CallList *out);
static void collect_calls_in_block(const FengBlock *block, CallList *out);

static void collect_calls_in_expr(const FengExpr *expr, CallList *out) {
    size_t i;
    if (expr == NULL) return;
    switch (expr->kind) {
        case FENG_EXPR_CALL:
            call_list_push(out, expr);
            collect_calls_in_expr(expr->as.call.callee, out);
            for (i = 0U; i < expr->as.call.arg_count; ++i) {
                collect_calls_in_expr(expr->as.call.args[i], out);
            }
            break;
        case FENG_EXPR_MEMBER:
            collect_calls_in_expr(expr->as.member.object, out);
            break;
        case FENG_EXPR_INDEX:
            collect_calls_in_expr(expr->as.index.object, out);
            collect_calls_in_expr(expr->as.index.index, out);
            break;
        case FENG_EXPR_UNARY:
            collect_calls_in_expr(expr->as.unary.operand, out);
            break;
        case FENG_EXPR_BINARY:
            collect_calls_in_expr(expr->as.binary.left, out);
            collect_calls_in_expr(expr->as.binary.right, out);
            break;
        case FENG_EXPR_CAST:
            collect_calls_in_expr(expr->as.cast.value, out);
            break;
        case FENG_EXPR_OBJECT_LITERAL:
            collect_calls_in_expr(expr->as.object_literal.target, out);
            for (i = 0U; i < expr->as.object_literal.field_count; ++i) {
                collect_calls_in_expr(expr->as.object_literal.fields[i].value, out);
            }
            break;
        case FENG_EXPR_ARRAY_LITERAL:
            for (i = 0U; i < expr->as.array_literal.count; ++i) {
                collect_calls_in_expr(expr->as.array_literal.items[i], out);
            }
            break;
        case FENG_EXPR_IF:
            collect_calls_in_expr(expr->as.if_expr.condition, out);
            collect_calls_in_block(expr->as.if_expr.then_block, out);
            collect_calls_in_block(expr->as.if_expr.else_block, out);
            break;
        case FENG_EXPR_MATCH:
            collect_calls_in_expr(expr->as.match_expr.target, out);
            for (i = 0U; i < expr->as.match_expr.branch_count; ++i) {
                collect_calls_in_block(expr->as.match_expr.branches[i].body, out);
            }
            collect_calls_in_block(expr->as.match_expr.else_block, out);
            break;
        case FENG_EXPR_LAMBDA:
            collect_calls_in_expr(expr->as.lambda.body, out);
            break;
        default:
            break;
    }
}

static void collect_calls_in_stmt(const FengStmt *stmt, CallList *out) {
    size_t i;
    if (stmt == NULL) return;
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            collect_calls_in_block(stmt->as.block, out);
            break;
        case FENG_STMT_BINDING:
            collect_calls_in_expr(stmt->as.binding.initializer, out);
            break;
        case FENG_STMT_ASSIGN:
            collect_calls_in_expr(stmt->as.assign.target, out);
            collect_calls_in_expr(stmt->as.assign.value, out);
            break;
        case FENG_STMT_EXPR:
            collect_calls_in_expr(stmt->as.expr, out);
            break;
        case FENG_STMT_IF:
            for (i = 0U; i < stmt->as.if_stmt.clause_count; ++i) {
                collect_calls_in_expr(stmt->as.if_stmt.clauses[i].condition, out);
                collect_calls_in_block(stmt->as.if_stmt.clauses[i].block, out);
            }
            collect_calls_in_block(stmt->as.if_stmt.else_block, out);
            break;
        case FENG_STMT_WHILE:
            collect_calls_in_expr(stmt->as.while_stmt.condition, out);
            collect_calls_in_block(stmt->as.while_stmt.body, out);
            break;
        case FENG_STMT_FOR:
            if (stmt->as.for_stmt.is_for_in) {
                collect_calls_in_expr(stmt->as.for_stmt.iter_expr, out);
            } else {
                collect_calls_in_stmt(stmt->as.for_stmt.init, out);
                collect_calls_in_expr(stmt->as.for_stmt.condition, out);
                collect_calls_in_stmt(stmt->as.for_stmt.update, out);
            }
            collect_calls_in_block(stmt->as.for_stmt.body, out);
            break;
        case FENG_STMT_MATCH:
            collect_calls_in_expr(stmt->as.match_stmt.target, out);
            for (i = 0U; i < stmt->as.match_stmt.branch_count; ++i) {
                collect_calls_in_block(stmt->as.match_stmt.branches[i].body, out);
            }
            collect_calls_in_block(stmt->as.match_stmt.else_block, out);
            break;
        case FENG_STMT_TRY:
            collect_calls_in_block(stmt->as.try_stmt.try_block, out);
            collect_calls_in_block(stmt->as.try_stmt.catch_block, out);
            collect_calls_in_block(stmt->as.try_stmt.finally_block, out);
            break;
        case FENG_STMT_RETURN:
            collect_calls_in_expr(stmt->as.return_value, out);
            break;
        case FENG_STMT_THROW:
            collect_calls_in_expr(stmt->as.throw_value, out);
            break;
        default:
            break;
    }
}

static void collect_calls_in_block(const FengBlock *block, CallList *out) {
    size_t i;
    if (block == NULL) return;
    for (i = 0U; i < block->statement_count; ++i) {
        collect_calls_in_stmt(block->statements[i], out);
    }
}

static const FengExpr *find_call_with_callee_identifier(
    const CallList *calls, const char *name) {
    size_t i;
    size_t name_len = strlen(name);
    for (i = 0U; i < calls->count; ++i) {
        const FengExpr *call = calls->items[i];
        const FengExpr *callee = call->as.call.callee;
        if (callee != NULL && callee->kind == FENG_EXPR_IDENTIFIER &&
            callee->as.identifier.length == name_len &&
            memcmp(callee->as.identifier.data, name, name_len) == 0) {
            return call;
        }
    }
    return NULL;
}

static const FengExpr *find_call_with_member_name(
    const CallList *calls, const char *name) {
    size_t i;
    size_t name_len = strlen(name);
    for (i = 0U; i < calls->count; ++i) {
        const FengExpr *call = calls->items[i];
        const FengExpr *callee = call->as.call.callee;
        if (callee != NULL && callee->kind == FENG_EXPR_MEMBER &&
            callee->as.member.member.length == name_len &&
            memcmp(callee->as.member.member.data, name, name_len) == 0) {
            return call;
        }
    }
    return NULL;
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "cannot differ only by return type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_overload_overlap_via_fit_rejected(void) {
    /* docs/feng-function.md §5: "若同一重载集合中的两个候选在当前可见的显式
     * 契约关系下可能同时匹配同一实参类型，必须视为签名冲突". A Dog argument
     * could match both `pet(a: Animal)` and `pet(d: Dog)` because the visible
     * fit makes Dog satisfy Animal. The conflict must be reported at the
     * declaration site, not deferred to call resolution. */
    const char *source =
        "mod demo.main;\n"
        "spec Animal {\n"
        "    fn name(): string;\n"
        "}\n"
        "type Dog {\n"
        "    let name: string;\n"
        "}\n"
        "fit Dog: Animal {\n"
        "    fn name(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n"
        "fn pet(a: Animal) {\n"
        "}\n"
        "fn pet(d: Dog) {\n"
        "}\n";
    FengProgram *program = parse_program_or_die("overload_overlap_via_fit.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "function overloads may both match the same arguments") != NULL);
    ASSERT(strstr(errors[0].message, "'pet'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_overload_overlap_via_two_specs_rejected(void) {
    /* When both candidates take spec parameters and at least one visible
     * concrete type satisfies both specs, the overload set is ambiguous and
     * must be rejected at declaration time. */
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn name(): string;\n"
        "}\n"
        "spec Sized {\n"
        "    fn size(): int;\n"
        "}\n"
        "type Box {\n"
        "    let label: string;\n"
        "    let count: int;\n"
        "}\n"
        "fit Box: Named {\n"
        "    fn name(): string { return self.label; }\n"
        "}\n"
        "fit Box: Sized {\n"
        "    fn size(): int { return self.count; }\n"
        "}\n"
        "fn show(x: Named) {}\n"
        "fn show(x: Sized) {}\n";
    FengProgram *program = parse_program_or_die("overload_overlap_two_specs.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "function overloads may both match the same arguments") != NULL);
    ASSERT(strstr(errors[0].message, "'show'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_overload_two_specs_no_common_type_accepted(void) {
    /* When two spec parameters have no common satisfying type visible in
     * the analysis, the overload set is unambiguous and must not be flagged. */
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn name(): string;\n"
        "}\n"
        "spec Sized {\n"
        "    fn size(): int;\n"
        "}\n"
        "type Tag {\n"
        "    let label: string;\n"
        "}\n"
        "fit Tag: Named {\n"
        "    fn name(): string { return self.label; }\n"
        "}\n"
        "type Bucket {\n"
        "    let count: int;\n"
        "}\n"
        "fit Bucket: Sized {\n"
        "    fn size(): int { return self.count; }\n"
        "}\n"
        "fn show(x: Named) {}\n"
        "fn show(x: Sized) {}\n";
    FengProgram *program = parse_program_or_die("overload_two_specs_disjoint_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_member_method_overload_overlap_via_fit_rejected(void) {
    /* Same overlap rule applies to member method overload sets. */
    const char *source =
        "mod demo.main;\n"
        "spec Animal {\n"
        "    fn name(): string;\n"
        "}\n"
        "type Dog {\n"
        "    let name: string;\n"
        "}\n"
        "fit Dog: Animal {\n"
        "    fn name(): string { return self.name; }\n"
        "}\n"
        "type Owner {\n"
        "    fn pet(a: Animal) {}\n"
        "    fn pet(d: Dog) {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("member_overload_overlap.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "method overloads in type 'Owner' may both match the same arguments") != NULL);
    ASSERT(strstr(errors[0].message, "'pet'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_accepts_module_string_library_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "let math_lib = \"m\";\n"
        "@cdecl(math_lib)\n"
        "extern fn sin(x: float): float;\n";
    FengProgram *program = parse_program_or_die("extern_fn_module_string_binding_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_extern_function_requires_calling_convention_annotation(void) {
    const char *source =
        "mod demo.main;\n"
        "extern fn sin(x: float): float;\n";
    FengProgram *program = parse_program_or_die("extern_fn_missing_callconv_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_missing_callconv_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message,
                  "must use exactly one of '@cdecl', '@stdcall', or '@fastcall'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_multiple_calling_convention_annotations(void) {
    const char *source =
        "mod demo.main;\n"
        "@cdecl(\"m\")\n"
        "@stdcall(\"m\")\n"
        "extern fn sin(x: float): float;\n";
    FengProgram *program = parse_program_or_die("extern_fn_multiple_callconv_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_multiple_callconv_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message,
                  "must use exactly one of '@cdecl', '@stdcall', or '@fastcall'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_non_string_library_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "var math_lib = \"m\";\n"
        "@cdecl(math_lib)\n"
        "extern fn sin(x: float): float;\n";
    FengProgram *program = parse_program_or_die("extern_fn_non_string_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_non_string_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "library argument must be a string literal or a visible let binding") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_accepts_imported_string_library_binding(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu let math_lib = \"m\";\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "@cdecl(math_lib)\n"
        "extern fn sin(x: float): float;\n";
    FengProgram *base_program = parse_program_or_die("extern_fn_imported_binding_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("extern_fn_imported_binding_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_extern_function_rejects_imported_var_library_binding(void) {
    const char *base_source =
        "pu mod demo.base;\n"
        "pu var math_lib = \"m\";\n";
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base;\n"
        "@cdecl(math_lib)\n"
        "extern fn sin(x: float): float;\n";
    FengProgram *base_program = parse_program_or_die("extern_fn_imported_var_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("extern_fn_imported_var_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message,
                  "library argument must be a string literal or a visible let binding") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_extern_function_rejects_array_parameter_type(void) {
    const char *source =
        "mod demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern fn fill(values: int[]): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_array_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_array_param_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "parameter 'values' type 'int[]' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_array_return_type(void) {
    const char *source =
        "mod demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern fn load(name: int): int[];\n";
    FengProgram *program = parse_program_or_die("extern_fn_array_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_array_return_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "return type 'int[]' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_non_fixed_object_parameter(void) {
    const char *source =
        "mod demo.main;\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"m\")\n"
        "extern fn use_point(point: Point): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_non_fixed_object_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_non_fixed_object_param_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "parameter 'point' type 'Point' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_accepts_fixed_object_and_callback_types(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@fixed\n"
        "spec PointCallback(p: Point): int;\n"
        "@cdecl(\"m\")\n"
        "extern fn run_point(point: Point, cb: PointCallback): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_fixed_types_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_type_accepts_abi_stable_fields(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_type_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_type_rejects_managed_field_type(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "type NameBox {\n"
        "    var name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_type_managed_field_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_type_managed_field_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "type 'NameBox' cannot be marked as @fixed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_type_rejects_union_annotation(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "@union\n"
        "spec Cmp(a: int, b: int): int;\n";
    FengProgram *program = parse_program_or_die("fixed_function_type_union_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_function_type_union_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "type 'Cmp' cannot be marked as @fixed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_accepts_abi_stable_signature(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "fn cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_function_rejects_parameterized_calling_convention(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "@cdecl(\"m\")\n"
        "fn cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_callconv_arg_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_fn_callconv_arg_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "function 'cmp' cannot be marked as @fixed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_method_rejects_managed_signature_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type CallbackHolder {\n"
        "    @fixed\n"
        "    fn emit(msg: string) {\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_method_managed_signature_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_method_managed_signature_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "method 'emit' cannot be marked as @fixed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_type_accepts_fixed_function_value(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "spec Callback(x: int): int;\n"
        "@fixed\n"
        "fn add1(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "fn run() {\n"
        "    let cb: Callback = add1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_callback_fixed_fn_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_function_type_rejects_plain_function_value(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "spec Callback(x: int): int;\n"
        "fn add1(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "fn run() {\n"
        "    let cb: Callback = add1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_callback_plain_fn_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_callback_plain_fn_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_type_rejects_direct_lambda_value(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "spec Callback(x: int): int;\n"
        "fn run() {\n"
        "    let cb: Callback = (x: int) -> x + 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_callback_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_callback_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_type_rejects_captured_lambda_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "spec Callback(x: int): int;\n"
        "fn run(base: int) {\n"
        "    let add = (x: int) -> x + base;\n"
        "    let cb: Callback = add;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_callback_captured_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_callback_captured_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_form_spec_rejects_fixed_annotation(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "spec Shape {\n"
        "    var x: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_spec_fixed_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_spec_fixed_error.f") == 0);
    ASSERT(strstr(errors[0].message,
                  "object-form spec 'Shape' cannot be marked as @fixed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_form_spec_rejects_union_annotation(void) {
    const char *source =
        "mod demo.main;\n"
        "@union\n"
        "spec Shape {\n"
        "    var x: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_spec_union_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "spec 'Shape' cannot use @union") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_callable_spec_accepts_fixed_type_parameter(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@fixed\n"
        "spec PointHandler(p: Point): int;\n";
    FengProgram *program = parse_program_or_die("fixed_callable_spec_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_callable_spec_rejects_non_fixed_type_parameter(void) {
    const char *source =
        "mod demo.main;\n"
        "type Bag {\n"
        "    var name: string;\n"
        "}\n"
        "@fixed\n"
        "spec Cb(b: Bag): int;\n";
    FengProgram *program = parse_program_or_die("fixed_callable_spec_non_fixed_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Cb' cannot be marked as @fixed because parameter 'b' uses non-ABI-stable type 'Bag'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_callable_spec_rejects_object_spec_parameter(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Shape {\n"
        "    var x: int;\n"
        "}\n"
        "@fixed\n"
        "spec Cb(s: Shape): int;\n";
    FengProgram *program = parse_program_or_die("fixed_callable_spec_object_spec_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Cb' cannot be marked as @fixed because parameter 's' uses non-ABI-stable type 'Shape'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_callable_spec_rejects_non_fixed_return_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type Bag {\n"
        "    var name: string;\n"
        "}\n"
        "@fixed\n"
        "spec Cb(x: int): Bag;\n";
    FengProgram *program = parse_program_or_die("fixed_callable_spec_non_fixed_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Cb' cannot be marked as @fixed because return type 'Bag' is not ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_rejects_uncaught_throw(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "fn fail(): int {\n"
        "    throw \"boom\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_uncaught_throw_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_fn_uncaught_throw_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @fixed ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_allows_locally_caught_throw(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "fn recover(): int {\n"
        "    try {\n"
        "        throw \"boom\";\n"
        "    } catch {\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_caught_throw_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_function_rejects_call_to_throwing_function(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@fixed\n"
        "fn run(): int {\n"
        "    return helper();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_throwing_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_fn_throwing_call_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @fixed ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_allows_call_to_catching_function(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int {\n"
        "    try {\n"
        "        throw \"boom\";\n"
        "    } catch {\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "@fixed\n"
        "fn run(): int {\n"
        "    return helper();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_catching_call_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_method_rejects_uncaught_throw(void) {
    const char *source =
        "mod demo.main;\n"
        "type Worker {\n"
        "    @fixed\n"
        "    fn run() {\n"
        "        throw \"boom\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_method_uncaught_throw_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_method_uncaught_throw_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @fixed ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_allows_unused_lambda_wrapping_throwing_call(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@fixed\n"
        "fn run(): int {\n"
        "    let wrap = (x: int) -> helper();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_unused_lambda_throwing_call_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fixed_function_rejects_invoked_lambda_wrapping_throwing_call(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@fixed\n"
        "fn run(): int {\n"
        "    let wrap = (x: int) -> helper();\n"
        "    return wrap(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_invoked_lambda_throwing_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_fn_invoked_lambda_throwing_call_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @fixed ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_rejects_local_function_value_call_to_throwing_function(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Callback(x: int): int;\n"
        "fn helper(x: int): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@fixed\n"
        "fn run(): int {\n"
        "    let cb: Callback = helper;\n"
        "    return cb(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_local_function_value_throwing_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_fn_local_function_value_throwing_call_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @fixed ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fixed_function_allows_invoked_lambda_wrapping_catching_call(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int {\n"
        "    try {\n"
        "        throw \"boom\";\n"
        "    } catch {\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "@fixed\n"
        "fn run(): int {\n"
        "    let wrap = (x: int) -> helper();\n"
        "    return wrap(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_fn_invoked_lambda_catching_call_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_throw_rejects_void_expression(void) {
    const char *source =
        "mod demo.main;\n"
        "fn side() {\n"
        "}\n"
        "fn run() {\n"
        "    throw side();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("throw_void_expression_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "throw_void_expression_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "throw statement requires a non-void expression") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finally_rejects_return(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): int {\n"
        "    try {\n"
        "        return 1;\n"
        "    } finally {\n"
        "        return 2;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("finally_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "finally_return_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "finally blocks cannot contain 'return'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finally_rejects_throw(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    try {\n"
        "    } finally {\n"
        "        throw \"boom\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("finally_throw_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "finally_throw_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "finally blocks cannot contain 'throw'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finally_rejects_break(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    while (true) {\n"
        "        try {\n"
        "        } finally {\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("finally_break_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "finally_break_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "finally blocks cannot contain 'break'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finally_rejects_continue(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    while (true) {\n"
        "        try {\n"
        "        } finally {\n"
        "            continue;\n"
        "        }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("finally_continue_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "finally_continue_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "finally blocks cannot contain 'continue'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_break_outside_loop_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    break;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_outside_loop_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "break_outside_loop_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "'break' statement is only allowed inside a 'while' or 'for' loop") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_continue_outside_loop_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    if (true) {\n"
        "        continue;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("continue_outside_loop_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "continue_outside_loop_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message,
                  "'continue' statement is only allowed inside a 'while' or 'for' loop") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_break_inside_lambda_in_loop_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Action(): void;\n"
        "fn run() {\n"
        "    while (true) {\n"
        "        let action: Action = () { break; };\n"
        "        action();\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_in_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "break_in_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message,
                  "'break' statement is only allowed inside a 'while' or 'for' loop") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_break_and_continue_inside_for_loop_are_accepted(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    for var i = 0; i < 10; i = i + 1 {\n"
        "        if (i == 1) { continue; }\n"
        "        if (i == 5) { break; }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_continue_in_for_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_throw_rejects_pointer_value(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(p: *int) {\n"
        "    throw p;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("throw_pointer_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "throw_pointer_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "is not throwable") != NULL);
    ASSERT(strstr(errors[0].message, "pointer values cannot be thrown") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_throw_rejects_fixed_type_value(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed type Handle { let id: int; }\n"
        "fn run(h: Handle) {\n"
        "    throw h;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("throw_fixed_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "throw_fixed_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not throwable") != NULL);
    ASSERT(strstr(errors[0].message, "@fixed types are ABI-bound") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_throw_accepts_string_and_managed_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type Err { let message: string; }\n"
        "fn run_string() {\n"
        "    throw \"boom\";\n"
        "}\n"
        "fn run_managed() {\n"
        "    throw Err { message: \"x\" };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("throw_managed_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_imported_function_auto_infers_return_type_across_modules(void) {
    const char *main_source =
        "mod demo.main;\n"
        "use demo.base as base;\n"
        "fn run(): int {\n"
        "    return base.value();\n"
        "}\n";
    const char *base_source =
        "pu mod demo.base;\n"
        "pu fn value() {\n"
        "    return 1;\n"
        "}\n";
    FengProgram *main_program = parse_program_or_die("auto_return_import_main.f", main_source);
    FengProgram *base_program = parse_program_or_die("auto_return_import_base.f", base_source);
    const FengProgram *programs[] = {main_program, base_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(main_program);
    feng_program_free(base_program);
}

static void test_omitted_return_function_can_infer_lambda_signature(void) {
    const char *source =
        "mod demo.main;\n"
        "spec IntToInt(x: int): int;\n"
        "fn make() {\n"
        "    return (x: int) -> x * 2;\n"
        "}\n"
        "fn run(): int {\n"
        "    let func: IntToInt = make();\n"
        "    return func(4);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("auto_return_lambda_signature_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_omitted_return_function_value_matches_named_function_type(void) {
    const char *source =
        "mod demo.main;\n"
        "spec IntToInt(x: int): int;\n"
        "fn pick(x: int) {\n"
        "    return x;\n"
        "}\n"
        "fn run(): int {\n"
        "    let func: IntToInt = pick;\n"
        "    return func(4);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("omitted_return_function_value_named_type_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    let pivot = other + 1;\n"
        "    return if value {\n"
        "        pivot { 1; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_non_constant_label_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "match_non_constant_label_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "match label must be a literal or a 'let' binding to a literal") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_expression_rejects_incomparable_label_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    return if value {\n"
        "        \"one\" { 1; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_incomparable_label_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "        1 { 1; }\n"
        "        else { \"zero\"; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_inconsistent_result_types_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_untyped_lambda_binding_matches_named_function_type(void) {
    const char *source =
        "mod demo.main;\n"
        "spec IntToInt(x: int): int;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_function_typed_local_binding_is_callable(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Picker(a: int): int;\n"
        "fn run(): int {\n"
        "    let pick: Picker = (a: int) -> a;\n"
        "    return pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_typed_local_call_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec IntPicker(a: int): int;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_value_selects_overload_by_parameter_context(void) {
    const char *source =
        "mod demo.main;\n"
        "spec IntPicker(a: int): int;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_value_selects_overload_by_return_type_context(void) {
    const char *source =
        "mod demo.main;\n"
        "spec IntPicker(a: int): int;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec M0(): void;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_method_value_selects_overload_by_parameter_context(void) {
    const char *source =
        "mod demo.main;\n"
        "spec M0(): void;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_method_value_selects_overload_by_return_type_context(void) {
    const char *source =
        "mod demo.main;\n"
        "spec M0(): void;\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec BoolPicker(a: bool): bool;\n"
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec BoolPicker(a: bool): bool;\n"
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "function_value_return_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 10U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'BoolPicker'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_function_value_rejects_non_function_binding_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    let flag: bool = pick;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_value_non_function_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "function_value_non_function_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'bool'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_lambda_body_rejects_function_value_for_non_function_return_type(void) {
    const char *source =
        "mod demo.main;\n"
        "spec BoolMaker(a: int): bool;\n"
        "fn pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "fn run() {\n"
        "    let maker: BoolMaker = (a: int) -> pick;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("lambda_body_function_value_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "lambda_body_function_value_return_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'BoolMaker'") != NULL);

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
        "spec BoolPicker(a: bool): bool;\n"
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec BoolAction(flag: bool): bool;\n"
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec IntPicker(a: int): int;\n"
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    fn update() {\n"
        "        self.flag = 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("member_assign_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "fn update(var user: User) {\n"
        "    user.id = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("instance_let_member_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    var items: int[]! = [1, 2, 3]!;\n"
        "    items[0] = 4;\n"
        "    let first: int = items[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_assign_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    var items: int[]! = [1, 2, 3]!;\n"
        "    items[0] = true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_assign_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    var items = [1, 2, 3]!;\n"
        "    items[0] = 4;\n"
        "    let first: int = items[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("inferred_array_index_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    var items = [1, 2, 3]!;\n"
        "    items[0] = true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("inferred_array_index_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    var matrix = [[1, 2]!, [3, 4]!]!;\n"
        "    matrix[0][1] = 5;\n"
        "    let value: int = matrix[1][0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("nested_array_index_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* docs/feng-builtin-type.md §5: writing through `[i] =` is rejected when the
 * indexed array layer lacks the writable mark `!`. */
static void test_index_assignment_rejects_readonly_array(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "    items[0] = 4;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("readonly_array_index_write_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* `T[]!` and `T[]` are distinct types; binding a writable literal to a
 * readonly slot without an explicit cast is rejected per docs §5. */
static void test_writable_array_literal_does_not_match_readonly_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[] = [1, 2, 3]!;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("writable_literal_to_readonly_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Cast may STRIP `!` (writable → readonly): allowed. */
static void test_cast_strips_writable_array_to_readonly(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var src: int[]! = [1, 2, 3]!;\n"
        "    let view: int[] = (int[])src;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_strip_writable_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Cast must not ADD `!` (readonly → writable): rejected. */
static void test_cast_rejects_adding_writable_to_readonly_array(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var src: int[] = [1, 2, 3];\n"
        "    let view: int[]! = (int[]!)src;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_add_writable_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from") != NULL);
    ASSERT(strstr(errors[0].message, "is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Empty `[]!` requires an explicit writable target type. */
static void test_empty_writable_array_literal_requires_writable_target(void) {
    const char *ok_source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var items: int[]! = []!;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("empty_writable_literal_ok.f", ok_source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "cast_array_to_numeric_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "cast from 'int[]' to 'int' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_string(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let s: string = (string)1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_string_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from 'i32' to 'string' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_array(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let xs: int[] = (int[])1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_array_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from 'i32' to 'int[]' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_string_to_bool(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let b: bool = (bool)\"x\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_string_to_bool_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from 'string' to 'bool' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_bool_to_string(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let s: string = (string)true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_bool_to_string_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from 'bool' to 'string' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_object(void) {
    const char *source =
        "mod demo.main;\n"
        "type Point { var x: int; var y: int; }\n"
        "fn run() {\n"
        "    let p: Point = (Point)1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_object_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from 'i32' to 'Point' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_object_to_numeric(void) {
    const char *source =
        "mod demo.main;\n"
        "type Point { var x: int; var y: int; }\n"
        "fn run(p: Point) {\n"
        "    let v: int = (int)p;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_object_to_numeric_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "cast from 'Point' to 'int' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_same_type_passes(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let i: int = (int)1;\n"
        "    let s: string = (string)\"x\";\n"
        "    let b: bool = (bool)true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_same_type_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "binary_and_type_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "binary operator '&&' requires bool operands") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_bitwise_ops_accept_same_integer_type(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(a: i32, b: i32) {\n"
        "    let c = a & b;\n"
        "    let d = a | b;\n"
        "    let e = a ^ b;\n"
        "    let f = a << b;\n"
        "    let g = a >> b;\n"
        "    let h = ~a;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("bitwise_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_bitwise_and_rejects_mismatched_integer_types(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(a: i32, b: i64) {\n"
        "    let c = a & b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("bitwise_and_mismatch.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '&' requires operands of the same integer type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_bitwise_or_rejects_non_integer_operand(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(a: f32, b: f32) {\n"
        "    let c = a | b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("bitwise_or_float.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '|' requires operands of the same integer type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_tilde_rejects_non_integer_operand(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(x: f32) {\n"
        "    ~x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_tilde_float.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "unary operator '~' requires an integer operand") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_shift_amount_out_of_range_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(a: i32) {\n"
        "    let b = a << 32;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("shift_out_of_range.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "shift amount") != NULL);
    ASSERT(strstr(errors[0].message, "out of range") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_arithmetic_fits_narrow_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let x: u8 = 100 + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_fit.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_const_fold_arithmetic_overflows_narrow_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let x: u8 = 200 + 100;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_overflow_target.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_division_by_zero_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let x: int = 1 / 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_div_zero.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "division by zero") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_modulo_by_zero_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let x: int = 1 % 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_mod_zero.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "modulo by zero") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_i64_overflow_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let x: i64 = 9223372036854775807 + 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_i64_overflow.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "integer overflow") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_shift_amount_via_const_expr(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(a: i32) {\n"
        "    let b = a << (16 + 16);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_shift_const_expr.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "shift amount 32") != NULL);
    ASSERT(strstr(errors[0].message, "out of range for type 'i32'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_cast_truncation_then_target_check(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let x: u8 = (u8)(255 + 1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_cast_trunc.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_const_fold_propagates_immutable_local_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let n: int = 100;\n"
        "    let x: u8 = n + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_let_prop.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_const_fold_does_not_propagate_var_binding(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    var n: int = 100;\n"
        "    let x: u8 = n + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_var_no_prop.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_if_expression_rejects_non_bool_condition(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run() {\n"
        "    let value = if 1 { 2; } else { 3; };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_condition_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    let value = if true { 1; } else { \"two\"; };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_branch_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    let value: int = if flag { 1 + 2; } else { 3 + 4; };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("valid_expr_type_checks_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "self_top_level_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "'self' is only available") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_self_is_capturable_inside_method_lambda(void) {
    /* Per docs/feng-function.md, lambdas declared inside a member method
     * (or constructor) body may capture the enclosing object's `self`. */
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    fn read(): int {\n"
        "        let thunk = () -> self.id;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("self_lambda_method_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
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

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "self_unknown_member.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "has no member 'name'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_spec_typed_param_supports_field_and_method_access(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    var name: string;\n"
        "    fn display(): string;\n"
        "}\n"
        "spec Identified: Named {\n"
        "    fn id(): int;\n"
        "}\n"
        "type Wrapper {\n"
        "    fn process(target: Identified): int {\n"
        "        target.name = \"x\";\n"
        "        let s: string = target.display();\n"
        "        return target.id();\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_polymorphism.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_typed_param_rejects_let_field_assignment(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type Wrapper {\n"
        "    fn rename(target: Named): void {\n"
        "        target.name = \"x\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_let_write.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_spec_typed_param_reports_unknown_member_with_spec_name(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    var name: string;\n"
        "}\n"
        "type Wrapper {\n"
        "    fn rename(target: Named): void {\n"
        "        let unused: string = target.unknown;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_unknown_member.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "spec 'Named' has no member 'unknown'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_callable_form_spec_typed_param_rejects_member_access(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Mapper(x: int): int;\n"
        "type Wrapper {\n"
        "    fn invoke(target: Mapper): void {\n"
        "        let unused: int = target.x;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_callable_member.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "callable-form") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_numeric_literal_adapts_to_explicit_integer_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let a: i32 = 1;\n"
        "    let b: i64 = 1;\n"
        "    let c: i16 = -1;\n"
        "    let d: i8  = -128;\n"
        "    let e: u8  = 255;\n"
        "    let f: u16 = 65535;\n"
        "    let g: u32 = 4294967295;\n"
        "    let h: u64 = 0;\n"
        "    let i: f32 = 1.5;\n"
        "    let j: f64 = 1.5;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_adapt_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_numeric_literal_adapts_to_explicit_alias_targets(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let a: int = 1;\n"
        "    let b: long = 1;\n"
        "    let c: byte = 0;\n"
        "    let d: float = 1.5;\n"
        "    let e: double = 1.5;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_alias_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_numeric_literal_overflowing_target_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let a: i8 = 200;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_overflow.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'i8'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_numeric_literal_negative_to_unsigned_target_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let a: u8 = -1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_neg_unsigned.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'u8'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_numeric_literal_overflows_default_int_target(void) {
    /* Default integer literal type is `int` (i32) per docs/feng-builtin-type.md §16, so an
     * out-of-range literal must be rejected even when the target's canonical name is i32. */
    const char *source =
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let a: i32 = 9999999999;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_default_int_overflow.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'i32'") != NULL);

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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec Factory(): int;\n"
        "fn make() {\n"
        "    Factory {};\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_literal_non_object_target.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    fn User(id: int) {}\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "spec Factory(): int;\n"
        "fn make() {\n"
        "    Factory();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_non_object_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_non_object_type.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "spec 'Factory' is not an object type and cannot be constructed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_call_rejects_object_form_spec(void) {
    const char *source =
        "mod demo.main;\n"
        "spec CommitOptions {\n"
        "    var message: int;\n"
        "}\n"
        "fn make() {\n"
        "    CommitOptions();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_object_spec.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "spec 'CommitOptions' is not an object type and cannot be constructed") != NULL);

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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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
        "    fn update() {\n"
        "        self.id = 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("let_method_assign_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_spec_type_satisfaction_succeeds(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "    fn greet(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_parent_specs_must_be_spec(void) {
    const char *source =
        "mod demo.main;\n"
        "type Other {}\n"
        "spec Bad: Other {}\n";
    FengProgram *program = parse_program_or_die("spec_parent_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "spec 'Bad' parent spec list must contain only spec types") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_spec_parent_specs_rejects_duplicate(void) {
    const char *source =
        "mod demo.main;\n"
        "spec A {}\n"
        "spec B: A, A {}\n";
    FengProgram *program = parse_program_or_die("spec_parent_dup.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "lists 'A' more than once") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_spec_parent_specs_rejects_cycle(void) {
    const char *source =
        "mod demo.main;\n"
        "spec A: B {}\n"
        "spec B: A {}\n";
    FengProgram *program = parse_program_or_die("spec_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "forms a cycle") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_must_be_spec(void) {
    const char *source =
        "mod demo.main;\n"
        "type Other {}\n"
        "type User: Other {}\n";
    FengProgram *program = parse_program_or_die("type_declared_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'User' declared spec list must contain only spec types") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_rejects_duplicate(void) {
    const char *source =
        "mod demo.main;\n"
        "spec A {}\n"
        "type User: A, A {}\n";
    FengProgram *program = parse_program_or_die("type_declared_dup.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "lists 'A' more than once") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_missing_field_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type User: Named {}\n";
    FengProgram *program = parse_program_or_die("type_missing_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'User' is missing field 'name' required by spec 'Named'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_field_mutability_mismatch_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type User: Named {\n"
        "    var name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("type_field_mut.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "mutability does not match") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_method_signature_mismatch_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User: Named {\n"
        "    fn greet(): int {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("type_method_sig.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "signature does not match") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_transitive_satisfaction_required(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Identified {\n"
        "    fn id(): int;\n"
        "}\n"
        "spec Named: Identified {\n"
        "    let name: string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("type_transitive.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'User' is missing method 'id' required by spec 'Identified'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_type_declared_specs_cross_spec_method_conflict(void) {
    const char *source =
        "mod demo.main;\n"
        "spec A {\n"
        "    fn run(): int;\n"
        "}\n"
        "spec B {\n"
        "    fn run(): string;\n"
        "}\n"
        "type Worker: A, B {\n"
        "    fn run(): int {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("type_conflict.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    /* Either the missing-spec-B-method message or the conflict message is acceptable;
       both indicate the conflict was detected. */
    ASSERT(strstr(errors[0].message, "different return types") != NULL ||
           strstr(errors[0].message, "signature does not match") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_target_must_be_type(void) {
    const char *source =
        "mod demo.main;\n"
        "spec A {}\n"
        "spec B {}\n"
        "fit A: B;\n";
    FengProgram *program = parse_program_or_die("fit_target_spec.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "fit target must be a concrete type") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_specs_must_be_spec(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {}\n"
        "type Other {}\n"
        "fit User: Other;\n";
    FengProgram *program = parse_program_or_die("fit_spec_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "fit specs list must contain only spec types") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_specs_rejects_duplicate(void) {
    const char *source =
        "mod demo.main;\n"
        "spec A {}\n"
        "type User {}\n"
        "fit User: A, A;\n";
    FengProgram *program = parse_program_or_die("fit_spec_dup.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "fit lists 'A' more than once") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_body_methods_satisfy_spec(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "fit User: Named {\n"
        "    fn greet(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_body_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_missing_method_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {}\n"
        "fit User: Named;\n";
    FengProgram *program = parse_program_or_die("fit_missing.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'User' is missing method 'greet' required by spec 'Named'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_orphan_pu_fit_emits_info_and_downgrades(void) {
    /* Module `demo.types` defines the type, `demo.specs` defines the spec,
     * and `demo.adapter` declares a `pu fit` that bridges them. Because the
     * adapter owns neither the type nor the spec, it is an orphan and its
     * `pu` export must be downgraded to module-local visibility with an
     * informational note. */
    const char *src_types =
        "pu mod demo.types;\n"
        "pu type User {}\n";
    const char *src_specs =
        "pu mod demo.specs;\n"
        "pu spec Named {\n"
        "    fn greet(): string;\n"
        "}\n";
    const char *src_adapter =
        "pu mod demo.adapter;\n"
        "use demo.types;\n"
        "use demo.specs;\n"
        "pu fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n";
    FengProgram *p1 = parse_program_or_die("types.f", src_types);
    FengProgram *p2 = parse_program_or_die("specs.f", src_specs);
    FengProgram *p3 = parse_program_or_die("adapter.f", src_adapter);
    const FengProgram *programs[] = {p1, p2, p3};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(analysis != NULL);
    ASSERT(analysis->info_count == 1U);
    ASSERT(strstr(analysis->infos[0].message, "orphan fit") != NULL);
    ASSERT(strstr(analysis->infos[0].message, "downgraded to module-local") != NULL);
    ASSERT(strcmp(analysis->infos[0].path, "adapter.f") == 0);

    feng_semantic_analysis_free(analysis);
    feng_program_free(p1);
    feng_program_free(p2);
    feng_program_free(p3);
}

static void test_local_fit_emits_no_orphan_info(void) {
    /* The fit lives in the same module as its target type, so it is not an
     * orphan and no info is emitted. */
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {}\n"
        "pu fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("local_fit.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(analysis != NULL);
    ASSERT(analysis->info_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Cross-module pu fit becomes effective in the consumer when the consumer
 * imports the fit's owning module via `use`. The consumer can then call
 * the spec method on a value of the imported type. The fit module owns
 * the spec so it is not an orphan adapter. */
static void test_pu_fit_visible_after_use_enables_method_call(void) {
    const char *src_types =
        "pu mod demo.types;\n"
        "pu type User {}\n";
    const char *src_adapter =
        "pu mod demo.adapter;\n"
        "use demo.types;\n"
        "pu spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "pu fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "pu mod demo.consumer;\n"
        "use demo.types;\n"
        "use demo.adapter;\n"
        "fn run(): string {\n"
        "    let u: User = User();\n"
        "    return u.greet();\n"
        "}\n";
    FengProgram *p1 = parse_program_or_die("types.f", src_types);
    FengProgram *p3 = parse_program_or_die("adapter.f", src_adapter);
    FengProgram *p4 = parse_program_or_die("consumer.f", src_consumer);
    const FengProgram *programs[] = {p1, p3, p4};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(p1);
    feng_program_free(p3);
    feng_program_free(p4);
}

/* Without `use` of the fit's owning module, the pu fit must not bridge
 * the type to the spec; calling the spec method on the type's value is
 * rejected because the contract relation is not in scope. */
static void test_pu_fit_invisible_without_use_rejects_method_call(void) {
    const char *src_types =
        "pu mod demo.types;\n"
        "pu type User {}\n";
    const char *src_adapter =
        "pu mod demo.adapter;\n"
        "use demo.types;\n"
        "pu spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "pu fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "pu mod demo.consumer;\n"
        "use demo.types;\n"
        "fn run(): string {\n"
        "    let u: User = User();\n"
        "    return u.greet();\n"
        "}\n";
    FengProgram *p1 = parse_program_or_die("types.f", src_types);
    FengProgram *p3 = parse_program_or_die("adapter.f", src_adapter);
    FengProgram *p4 = parse_program_or_die("consumer.f", src_consumer);
    const FengProgram *programs[] = {p1, p3, p4};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "no member 'greet'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(p1);
    feng_program_free(p3);
    feng_program_free(p4);
}

/* Spec satisfaction at type position must consider pu fits from any
 * module the consumer has `use`d, not only fits declared in the
 * consumer's own module. */
static void test_imported_pu_fit_satisfies_spec_typed_parameter(void) {
    const char *src_types =
        "pu mod demo.types;\n"
        "pu type User {}\n";
    const char *src_adapter =
        "pu mod demo.adapter;\n"
        "use demo.types;\n"
        "pu spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "pu fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "pu mod demo.consumer;\n"
        "use demo.types;\n"
        "use demo.adapter;\n"
        "fn use_named(n: Named): void { return; }\n"
        "fn run(): void {\n"
        "    let u: User = User();\n"
        "    use_named(u);\n"
        "}\n";
    FengProgram *p1 = parse_program_or_die("types.f", src_types);
    FengProgram *p3 = parse_program_or_die("adapter.f", src_adapter);
    FengProgram *p4 = parse_program_or_die("consumer.f", src_consumer);
    const FengProgram *programs[] = {p1, p3, p4};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(p1);
    feng_program_free(p3);
    feng_program_free(p4);
}

/* Aliased `use` must also activate the imported module's pu fit
 * contracts, even though the imported short names go through the
 * alias instead of being injected into the current scope. */
static void test_pu_fit_visible_via_alias_use(void) {
    const char *src_types =
        "pu mod demo.types;\n"
        "pu type User {}\n";
    const char *src_adapter =
        "pu mod demo.adapter;\n"
        "use demo.types;\n"
        "pu spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "pu fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "pu mod demo.consumer;\n"
        "use demo.types;\n"
        "use demo.adapter as adapter;\n"
        "fn run(): string {\n"
        "    let u: User = User();\n"
        "    return u.greet();\n"
        "}\n";
    FengProgram *p1 = parse_program_or_die("types.f", src_types);
    FengProgram *p3 = parse_program_or_die("adapter.f", src_adapter);
    FengProgram *p4 = parse_program_or_die("consumer.f", src_consumer);
    const FengProgram *programs[] = {p1, p3, p4};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(p1);
    feng_program_free(p3);
    feng_program_free(p4);
}

static void test_fit_method_callable_on_instance(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {}\n"
        "fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n"
        "fn run(): string {\n"
        "    let u: User = User {};\n"
        "    return u.greet();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_call.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_method_unknown_member_still_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {}\n"
        "fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n"
        "fn run(): void {\n"
        "    let u: User = User {};\n"
        "    u.farewell();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_unknown.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "no member 'farewell'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_body_rejects_self_private_field_access(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {\n"
        "    pr let secret: string;\n"
        "}\n"
        "fit User: Named {\n"
        "    fn greet(): string {\n"
        "        return self.secret;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_priv_self_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message,
                  "fit body cannot access private member 'secret' of target type 'User'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_body_rejects_self_private_method_access(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {\n"
        "    pr fn whisper(): string { return \"shh\"; }\n"
        "}\n"
        "fit User: Named {\n"
        "    fn greet(): string {\n"
        "        return self.whisper();\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_priv_self_method.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message,
                  "fit body cannot access private member 'whisper' of target type 'User'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_body_rejects_other_param_private_field_access(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Tagged {\n"
        "    fn tag_of(other: User): string;\n"
        "}\n"
        "type User {\n"
        "    pr let secret: string;\n"
        "}\n"
        "fit User: Tagged {\n"
        "    fn tag_of(other: User): string {\n"
        "        return other.secret;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_priv_other_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message,
                  "fit body cannot access private member 'secret' of target type 'User'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_body_rejects_object_literal_private_field(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Builder {\n"
        "    fn make(): User;\n"
        "}\n"
        "type User {\n"
        "    let name: string;\n"
        "    pr let secret: string;\n"
        "}\n"
        "fit User: Builder {\n"
        "    fn make(): User {\n"
        "        return User { name: \"a\", secret: \"b\" };\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_priv_object_lit.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message,
                  "object literal field 'secret' is not accessible for type 'User'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_body_allows_public_member_access(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {\n"
        "    let name: string;\n"
        "    pr let secret: string;\n"
        "    fn shout(): string { return self.name; }\n"
        "}\n"
        "fit User: Named {\n"
        "    fn greet(): string {\n"
        "        return self.shout();\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_pub_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_at_type_position_accepts_satisfying_type(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "}\n"
        "fn use_named(n: Named): void {\n"
        "    return;\n"
        "}\n"
        "fn run(): void {\n"
        "    let u: User = User { name: \"a\" };\n"
        "    use_named(u);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_pos_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_at_type_position_rejects_unrelated_type(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type Other {\n"
        "    let name: string;\n"
        "}\n"
        "fn use_named(n: Named): void {\n"
        "    return;\n"
        "}\n"
        "fn run(): void {\n"
        "    let o: Other = Other { name: \"a\" };\n"
        "    use_named(o);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_pos_bad.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_spec_at_type_position_accepts_via_fit(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type Other {\n"
        "    let name: string;\n"
        "}\n"
        "fit Other: Named;\n"
        "fn use_named(n: Named): void {\n"
        "    return;\n"
        "}\n"
        "fn run(): void {\n"
        "    let o: Other = Other { name: \"a\" };\n"
        "    use_named(o);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_pos_fit.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static const FengDecl *find_function_decl_by_name(
    const FengProgram *program, const char *name) {
    size_t i;
    size_t name_len = strlen(name);
    for (i = 0U; i < program->declaration_count; ++i) {
        const FengDecl *decl = program->declarations[i];
        if (decl->kind == FENG_DECL_FUNCTION &&
            decl->as.function_decl.name.length == name_len &&
            memcmp(decl->as.function_decl.name.data, name, name_len) == 0) {
            return decl;
        }
    }
    return NULL;
}

static void test_resolved_callable_attached_to_call_exprs(void) {
    /* Exercises all four resolved-callable kinds in a single program. */
    const char *source =
        "mod demo.main;\n"
        "spec Named {\n"
        "    fn greet(): string;\n"
        "}\n"
        "type User {\n"
        "    fn shout(): string { return \"HI\"; }\n"
        "}\n"
        "fit User: Named {\n"
        "    fn greet(): string { return \"hi\"; }\n"
        "}\n"
        "fn helper(): int { return 1; }\n"
        "fn run(): int {\n"
        "    let u: User = User();\n"
        "    let a: string = u.greet();\n"
        "    let b: string = u.shout();\n"
        "    return helper();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("resolved.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *run_decl;
    const FengDecl *user_type;
    CallList calls = {NULL, 0U, 0U};
    const FengExpr *call_user;
    const FengExpr *call_greet;
    const FengExpr *call_shout;
    const FengExpr *call_helper;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    run_decl = find_function_decl_by_name(program, "run");
    ASSERT(run_decl != NULL);
    user_type = NULL;
    {
        size_t i;
        for (i = 0U; i < program->declaration_count; ++i) {
            const FengDecl *d = program->declarations[i];
            if (d->kind == FENG_DECL_TYPE &&
                d->as.type_decl.name.length == 4U &&
                memcmp(d->as.type_decl.name.data, "User", 4U) == 0) {
                user_type = d;
                break;
            }
        }
    }
    ASSERT(user_type != NULL);

    collect_calls_in_block(run_decl->as.function_decl.body, &calls);

    call_user = find_call_with_callee_identifier(&calls, "User");
    ASSERT(call_user != NULL);
    ASSERT(call_user->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR);
    ASSERT(call_user->as.call.resolved_callable.owner_type_decl == user_type);
    /* No declared constructor in User → member is NULL (implicit zero-arg). */
    ASSERT(call_user->as.call.resolved_callable.member == NULL);

    call_greet = find_call_with_member_name(&calls, "greet");
    ASSERT(call_greet != NULL);
    ASSERT(call_greet->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_FIT_METHOD);
    ASSERT(call_greet->as.call.resolved_callable.owner_type_decl == user_type);
    ASSERT(call_greet->as.call.resolved_callable.member != NULL);
    ASSERT(call_greet->as.call.resolved_callable.fit_decl != NULL);
    ASSERT(call_greet->as.call.resolved_callable.fit_decl->kind == FENG_DECL_FIT);

    call_shout = find_call_with_member_name(&calls, "shout");
    ASSERT(call_shout != NULL);
    ASSERT(call_shout->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_TYPE_METHOD);
    ASSERT(call_shout->as.call.resolved_callable.owner_type_decl == user_type);
    ASSERT(call_shout->as.call.resolved_callable.member != NULL);
    ASSERT(call_shout->as.call.resolved_callable.fit_decl == NULL);

    call_helper = find_call_with_callee_identifier(&calls, "helper");
    ASSERT(call_helper != NULL);
    ASSERT(call_helper->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_FUNCTION);
    ASSERT(call_helper->as.call.resolved_callable.function_decl != NULL);
    ASSERT(call_helper->as.call.resolved_callable.function_decl->kind ==
           FENG_DECL_FUNCTION);

    free(calls.items);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_finalizer_basic_ok(void) {
    const char *source =
        "mod demo.main;\n"
        "type Buffer {\n"
        "    pu var size: int;\n"
        "    fn Buffer(s: int) {\n"
        "        self.size = s;\n"
        "    }\n"
        "    fn ~Buffer() {\n"
        "        return;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fin_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_finalizer_rejects_multiple_per_type(void) {
    const char *source =
        "mod demo.main;\n"
        "type Buffer {\n"
        "    fn ~Buffer() {}\n"
        "    fn ~Buffer() {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fin_dup.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "declares more than one finalizer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finalizer_rejected_on_fixed_type(void) {
    const char *source =
        "mod demo.main;\n"
        "@fixed\n"
        "type Buffer {\n"
        "    pu let size: int;\n"
        "    fn ~Buffer() {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fin_fixed.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "@fixed") != NULL);
    ASSERT(strstr(errors[0].message, "finalizer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finalizer_rejects_return_with_value(void) {
    const char *source =
        "mod demo.main;\n"
        "type Buffer {\n"
        "    fn ~Buffer() {\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fin_retval.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "finalizer body must use 'return;' without a value") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_rejects_return_with_value(void) {
    const char *source =
        "mod demo.main;\n"
        "type Box {\n"
        "    fn Box() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_retval.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "constructor body must use 'return;' without a value") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_constructor_with_explicit_void_return_ok(void) {
    const char *source =
        "mod demo.main;\n"
        "type Box {\n"
        "    pu var v: int;\n"
        "    fn Box(): void {\n"
        "        self.v = 1;\n"
        "        return;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_void.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* ===== New behaviour added with --target / lambda block-body / overload checks ===== */

static void test_lambda_block_body_returns_value(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): int {\n"
        "    let f = (a: int) {\n"
        "        let b = a + 1;\n"
        "        return b;\n"
        "    };\n"
        "    return f(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("lambda_block_body_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_lambda_block_body_records_local_capture(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): int {\n"
        "    let x = 1;\n"
        "    let f = () {\n"
        "        return x;\n"
        "    };\n"
        "    return f();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("lambda_capture_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengStmt *stmt;
    const FengExpr *lambda_expr;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    /* Drill into AST: program -> fn run -> body -> stmts[1] (let f = lambda) */
    ASSERT(program->declaration_count >= 1U);
    {
        const FengDecl *decl = program->declarations[0];
        const FengCallableSignature *sig = &decl->as.function_decl;

        ASSERT(sig->body != NULL);
        ASSERT(sig->body->statement_count >= 2U);
        stmt = sig->body->statements[1];
        ASSERT(stmt->kind == FENG_STMT_BINDING);
        lambda_expr = stmt->as.binding.initializer;
        ASSERT(lambda_expr != NULL && lambda_expr->kind == FENG_EXPR_LAMBDA);
        ASSERT(lambda_expr->as.lambda.is_block_body);
        ASSERT(lambda_expr->as.lambda.capture_count == 1U);
        ASSERT(lambda_expr->as.lambda.captures[0].kind == FENG_LAMBDA_CAPTURE_LOCAL);
        ASSERT(lambda_expr->as.lambda.captures[0].name.length == 1U);
        ASSERT(lambda_expr->as.lambda.captures[0].name.data[0] == 'x');
        ASSERT(!lambda_expr->as.lambda.captures_self);
    }

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_lambda_in_method_records_self_capture(void) {
    const char *source =
        "mod demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    fn read(): int {\n"
        "        let f = () -> self.id;\n"
        "        return f();\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("lambda_self_capture_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    {
        const FengDecl *type_decl = program->declarations[0];
        const FengTypeMember *method = type_decl->as.type_decl.members[1];
        const FengStmt *binding_stmt = method->as.callable.body->statements[0];
        const FengExpr *lambda_expr = binding_stmt->as.binding.initializer;

        ASSERT(lambda_expr->kind == FENG_EXPR_LAMBDA);
        ASSERT(lambda_expr->as.lambda.captures_self);
    }

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_field_init_lambda_captures_self_when_callable_spec(void) {
    const char *source =
        "mod demo.main;\n"
        "spec Reader(): int;\n"
        "type Box {\n"
        "    var n: int;\n"
        "    let read: Reader = () -> self.n;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("field_lambda_self_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_field_init_bare_self_is_invalid(void) {
    const char *source =
        "mod demo.main;\n"
        "type Box {\n"
        "    var n: int;\n"
        "    let m: int = self.n;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("field_bare_self_err.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "self") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_duplicate_method_signature_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "type T {\n"
        "    fn pick(a: int): int { return a; }\n"
        "    fn pick(a: int): int { return a + 1; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("dup_method.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "duplicate method signature") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_method_overload_return_only_difference_is_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "type T {\n"
        "    fn pick(a: int): int { return a; }\n"
        "    fn pick(a: int): bool { return true; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ret_only_method.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "differ only by return type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_main_entry_required_for_bin_target(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int { return 0; }\n";
    FengProgram *program = parse_program_or_die("no_main.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_BIN, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "main") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_main_entry_valid_signature_passes_for_bin(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(args: string[]) {\n"
        "    return;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("main_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_BIN, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_main_entry_bad_signature_is_rejected_for_bin(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(): int { return 0; }\n";
    FengProgram *program = parse_program_or_die("main_bad_sig.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_BIN, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_multiple_main_entries_rejected_for_bin(void) {
    const char *source_a =
        "mod demo.main;\n"
        "fn main(args: string[]) { return; }\n";
    const char *source_b =
        "mod demo.other;\n"
        "fn main(args: string[]) { return; }\n";
    FengProgram *program_a = parse_program_or_die("main_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("main_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_BIN, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "duplicate 'main'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_lib_target_skips_main_check(void) {
    const char *source =
        "mod demo.main;\n"
        "fn helper(): int { return 0; }\n";
    FengProgram *program = parse_program_or_die("lib_no_main.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_match_range_label_overlap_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    return if value {\n"
        "        1...10 { 1; }\n"
        "        5...15 { 2; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_overlap_range.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "overlaps with an earlier label") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_single_label_overlap_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    return if value {\n"
        "        1, 2, 3 { 1; }\n"
        "        2 { 2; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_overlap_single.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "overlaps with an earlier label") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_range_invalid_bounds_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    return if value {\n"
        "        10...1 { 1; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_range_bounds.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_target_type_disallowed(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: f64): int {\n"
        "    return if value {\n"
        "        1 { 1; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_target_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "match target type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_let_bound_label_accepted(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    let one = 1;\n"
        "    return if value {\n"
        "        one { 100; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_let_bound.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_for_in_loop_array_accepted(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(items: int[]) {\n"
        "    for let it in items {\n"
        "        print(it);\n"
        "    }\n"
        "}\n"
        "fn print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("for_in_array.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_for_in_loop_non_array_rejected(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(value: int) {\n"
        "    for let it in value {\n"
        "        print(it);\n"
        "    }\n"
        "}\n"
        "fn print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("for_in_non_array.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "for/in sequence must be an array") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* ============================================================== */
/* Phase 1B-2: type-cyclicity SCC analysis                         */
/* ============================================================== */

static const FengDecl *find_type_decl_by_name(
        const FengSemanticAnalysis *analysis, const char *name) {
    size_t name_len = strlen(name);
    for (size_t mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        for (size_t pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (size_t di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if (d->kind != FENG_DECL_TYPE) continue;
                const FengSlice *n = &d->as.type_decl.name;
                if (n->length == name_len &&
                    memcmp(n->data, name, name_len) == 0) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

static void test_cyclicity_acyclic_chain_marks_none(void) {
    const char *src =
        "pu mod demo.cyc;\n"
        "type Leaf { let id: int; }\n"
        "type Mid { let leaf: Leaf; }\n"
        "type Top { let mid: Mid; }\n";
    FengProgram *program = parse_program_or_die("acyc.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    const FengDecl *leaf = find_type_decl_by_name(analysis, "Leaf");
    const FengDecl *mid = find_type_decl_by_name(analysis, "Mid");
    const FengDecl *top = find_type_decl_by_name(analysis, "Top");
    ASSERT(leaf && mid && top);
    ASSERT(!feng_semantic_type_is_potentially_cyclic(analysis, leaf));
    ASSERT(!feng_semantic_type_is_potentially_cyclic(analysis, mid));
    ASSERT(!feng_semantic_type_is_potentially_cyclic(analysis, top));
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_cyclicity_self_loop_marks_self(void) {
    const char *src =
        "pu mod demo.cyc;\n"
        "type Node { var next: Node; }\n"
        "type Other { let id: int; }\n";
    FengProgram *program = parse_program_or_die("self.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    const FengDecl *node = find_type_decl_by_name(analysis, "Node");
    const FengDecl *other = find_type_decl_by_name(analysis, "Other");
    ASSERT(node && other);
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, node));
    ASSERT(!feng_semantic_type_is_potentially_cyclic(analysis, other));
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_cyclicity_two_node_cycle_marks_both(void) {
    const char *src =
        "pu mod demo.cyc;\n"
        "type A { var b: B; }\n"
        "type B { var a: A; }\n"
        "type C { let id: int; }\n";
    FengProgram *program = parse_program_or_die("twocyc.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    const FengDecl *a = find_type_decl_by_name(analysis, "A");
    const FengDecl *b = find_type_decl_by_name(analysis, "B");
    const FengDecl *c = find_type_decl_by_name(analysis, "C");
    ASSERT(a && b && c);
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, a));
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, b));
    ASSERT(!feng_semantic_type_is_potentially_cyclic(analysis, c));
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_cyclicity_three_node_cycle_marks_all(void) {
    const char *src =
        "pu mod demo.cyc;\n"
        "type A { var b: B; }\n"
        "type B { var c: C; }\n"
        "type C { var a: A; }\n";
    FengProgram *program = parse_program_or_die("threecyc.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    const FengDecl *a = find_type_decl_by_name(analysis, "A");
    const FengDecl *b = find_type_decl_by_name(analysis, "B");
    const FengDecl *c = find_type_decl_by_name(analysis, "C");
    ASSERT(a && b && c);
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, a));
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, b));
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, c));
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_cyclicity_array_mediated_cycle_marks_both(void) {
    /* B contains an array of A, and A back-references B; an array of T is a
     * managed reference for cyclicity purposes, so {A,B} form an SCC. */
    const char *src =
        "pu mod demo.cyc;\n"
        "type A { var owner: B; }\n"
        "type B { var children: A[]; }\n";
    FengProgram *program = parse_program_or_die("arrcyc.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    const FengDecl *a = find_type_decl_by_name(analysis, "A");
    const FengDecl *b = find_type_decl_by_name(analysis, "B");
    ASSERT(a && b);
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, a));
    ASSERT(feng_semantic_type_is_potentially_cyclic(analysis, b));
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* --- Phase S1a: SpecRelation sidecar tests ---------------------------- */

static const FengDecl *find_spec_decl_by_name(
        const FengSemanticAnalysis *analysis, const char *name) {
    size_t name_len = strlen(name);
    for (size_t mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        for (size_t pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (size_t di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if (d->kind != FENG_DECL_SPEC) continue;
                const FengSlice *n = &d->as.spec_decl.name;
                if (n->length == name_len &&
                    memcmp(n->data, name, name_len) == 0) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

static const FengSemanticModule *find_module_by_path(
        const FengSemanticAnalysis *analysis, const char *path) {
    for (size_t mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        for (size_t pi = 0U; pi < mod->program_count; ++pi) {
            if (strcmp(mod->programs[pi]->path, path) == 0) {
                return mod;
            }
        }
    }
    return NULL;
}

static bool relation_has_source(const FengSpecRelation *rel,
                                FengSpecRelationSourceKind kind,
                                const FengDecl *via_spec_decl,
                                const FengDecl *via_fit_decl) {
    if (rel == NULL) return false;
    for (size_t i = 0U; i < rel->source_count; ++i) {
        const FengSpecRelationSource *s = &rel->sources[i];
        if (s->kind == kind &&
            s->via_spec_decl == via_spec_decl &&
            s->via_fit_decl == via_fit_decl) {
            return true;
        }
    }
    return false;
}

static void test_spec_relation_declared_head_recorded(void) {
    /* `type T : S` records a single DECLARED_HEAD source for (T, S) and
     * does not invent any other relation. */
    const char *src =
        "pu mod demo.rel;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named { fn name(): string { return \"u\"; } }\n";
    FengProgram *program = parse_program_or_die("rel_decl_head.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(user != NULL && named != NULL);

    const FengSpecRelation *rel = feng_semantic_lookup_spec_relation(analysis, user, named);
    ASSERT(rel != NULL);
    ASSERT(rel->source_count == 1U);
    ASSERT(relation_has_source(rel, FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD,
                               named, NULL));

    /* Reverse direction must not exist. */
    ASSERT(feng_semantic_lookup_spec_relation(analysis, user, user) == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_relation_declared_parent_transitive(void) {
    /* `type T : Child` where `spec Child : Parent` records DECLARED_HEAD for
     * (T, Child) and DECLARED_PARENT for (T, Parent) via the head Child. */
    const char *src =
        "pu mod demo.rel;\n"
        "spec Parent { fn p(): int; }\n"
        "spec Child: Parent { fn c(): int; }\n"
        "type Both: Child {\n"
        "    fn p(): int { return 1; }\n"
        "    fn c(): int { return 2; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("rel_decl_parent.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    const FengDecl *both = find_type_decl_by_name(analysis, "Both");
    const FengDecl *parent = find_spec_decl_by_name(analysis, "Parent");
    const FengDecl *child = find_spec_decl_by_name(analysis, "Child");
    ASSERT(both && parent && child);

    const FengSpecRelation *rel_child = feng_semantic_lookup_spec_relation(analysis, both, child);
    ASSERT(rel_child != NULL);
    ASSERT(relation_has_source(rel_child, FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD,
                               child, NULL));

    const FengSpecRelation *rel_parent = feng_semantic_lookup_spec_relation(analysis, both, parent);
    ASSERT(rel_parent != NULL);
    ASSERT(relation_has_source(rel_parent, FENG_SPEC_RELATION_SOURCE_DECLARED_PARENT,
                               child, NULL));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_relation_fit_head_and_parent(void) {
    /* `fit T : Child` produces FIT_HEAD for (T, Child) and FIT_PARENT for
     * (T, Parent), both pointing back at the same fit decl. */
    const char *src =
        "pu mod demo.rel;\n"
        "spec Parent { fn p(): int; }\n"
        "spec Child: Parent { fn c(): int; }\n"
        "type Tag {}\n"
        "fit Tag: Child {\n"
        "    fn p(): int { return 1; }\n"
        "    fn c(): int { return 2; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("rel_fit.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    const FengDecl *tag = find_type_decl_by_name(analysis, "Tag");
    const FengDecl *parent = find_spec_decl_by_name(analysis, "Parent");
    const FengDecl *child = find_spec_decl_by_name(analysis, "Child");
    ASSERT(tag && parent && child);

    /* Locate the fit decl. */
    const FengDecl *fit = NULL;
    const FengSemanticModule *mod = &analysis->modules[0];
    const FengProgram *prog = mod->programs[0];
    for (size_t di = 0U; di < prog->declaration_count; ++di) {
        if (prog->declarations[di]->kind == FENG_DECL_FIT) {
            fit = prog->declarations[di];
            break;
        }
    }
    ASSERT(fit != NULL);

    const FengSpecRelation *rel_child = feng_semantic_lookup_spec_relation(analysis, tag, child);
    ASSERT(rel_child != NULL);
    ASSERT(relation_has_source(rel_child, FENG_SPEC_RELATION_SOURCE_FIT_HEAD,
                               child, fit));

    const FengSpecRelation *rel_parent = feng_semantic_lookup_spec_relation(analysis, tag, parent);
    ASSERT(rel_parent != NULL);
    ASSERT(relation_has_source(rel_parent, FENG_SPEC_RELATION_SOURCE_FIT_PARENT,
                               child, fit));

    /* provider_module on FIT_* sources points at the fit's owning module. */
    for (size_t i = 0U; i < rel_child->source_count; ++i) {
        if (rel_child->sources[i].kind == FENG_SPEC_RELATION_SOURCE_FIT_HEAD) {
            ASSERT(rel_child->sources[i].provider_module == mod);
        }
    }

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_relation_visibility_filter(void) {
    /* A `pu fit` lives in module A; module B does not `use` A; module C
     * does. The relation table records the fit source unconditionally;
     * the visibility helper rejects B and accepts C. */
    const char *src_a =
        "pu mod demo.a;\n"
        "pu spec Named { fn name(): string; }\n"
        "pu type Tag {}\n"
        "pu fit Tag: Named { fn name(): string { return \"t\"; } }\n";
    const char *src_b =
        "mod demo.b;\n"
        "fn unrelated() {}\n";
    const char *src_c =
        "mod demo.c;\n"
        "use demo.a;\n"
        "fn unrelated() {}\n";
    FengProgram *pa = parse_program_or_die("vis_a.f", src_a);
    FengProgram *pb = parse_program_or_die("vis_b.f", src_b);
    FengProgram *pc = parse_program_or_die("vis_c.f", src_c);
    const FengProgram *programs[] = {pa, pb, pc};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *tag = find_type_decl_by_name(analysis, "Tag");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(tag && named);

    const FengSpecRelation *rel = feng_semantic_lookup_spec_relation(analysis, tag, named);
    ASSERT(rel != NULL);
    ASSERT(rel->source_count == 1U);

    const FengSemanticModule *mod_a = find_module_by_path(analysis, "vis_a.f");
    const FengSemanticModule *mod_b = find_module_by_path(analysis, "vis_b.f");
    const FengSemanticModule *mod_c = find_module_by_path(analysis, "vis_c.f");
    ASSERT(mod_a && mod_b && mod_c);

    const FengSpecRelationSource *src = &rel->sources[0];
    ASSERT(src->kind == FENG_SPEC_RELATION_SOURCE_FIT_HEAD);
    ASSERT(src->provider_module == mod_a);

    /* From A itself: visible. */
    ASSERT(feng_semantic_spec_relation_source_visible_from(src, mod_a, NULL, 0U));
    /* From B (no `use`): not visible. */
    ASSERT(!feng_semantic_spec_relation_source_visible_from(src, mod_b, NULL, 0U));
    /* From C (with `use demo.a`): visible. */
    const FengSemanticModule *c_imports[] = {mod_a};
    ASSERT(feng_semantic_spec_relation_source_visible_from(src, mod_c, c_imports, 1U));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(pa);
    feng_program_free(pb);
    feng_program_free(pc);
}

/* --- Phase S1b: SpecCoercionSite sidecar tests ----------------------- */

static const FengDecl *find_function_decl_in_program(const FengProgram *prog,
                                                     const char *name) {
    size_t name_len = strlen(name);
    for (size_t i = 0U; i < prog->declaration_count; ++i) {
        const FengDecl *d = prog->declarations[i];
        if (d->kind != FENG_DECL_FUNCTION) continue;
        if (d->as.function_decl.name.length == name_len &&
            memcmp(d->as.function_decl.name.data, name, name_len) == 0) {
            return d;
        }
    }
    return NULL;
}

static const FengExpr *first_let_initializer(const FengCallableSignature *fn) {
    for (size_t i = 0U; i < fn->body->statement_count; ++i) {
        const FengStmt *s = fn->body->statements[i];
        if (s->kind == FENG_STMT_BINDING) {
            return s->as.binding.initializer;
        }
    }
    return NULL;
}

static void test_spec_coercion_object_let_binding(void) {
    /* `let x: Named = User{...};` records an OBJECT-form coercion site on
     * the initializer expression, with the SpecRelation entry that
     * justifies the satisfaction. */
    const char *src =
        "pu mod demo.coerce;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "fn make(): int {\n"
        "    let x: Named = User{n: \"u\"};\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_let.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *make = find_function_decl_in_program(program, "make");
    ASSERT(make != NULL);
    const FengExpr *init = first_let_initializer(&make->as.function_decl);
    ASSERT(init != NULL);

    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, init);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(site->src_type_decl == user);
    ASSERT(site->target_spec_decl == named);
    ASSERT(site->relation != NULL);
    ASSERT(site->relation->type_decl == user);
    ASSERT(site->relation->spec_decl == named);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_argument(void) {
    /* Argument-passing coercion: `accept(User{...})` against parameter
     * `s: Named` records an OBJECT site on the argument expression. */
    const char *src =
        "pu mod demo.coerce;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "fn accept(s: Named) {}\n"
        "fn caller(): int {\n"
        "    accept(User{n: \"u\"});\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_arg.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *caller = find_function_decl_in_program(program, "caller");
    ASSERT(caller != NULL);
    const FengStmt *call_stmt = caller->as.function_decl.body->statements[0];
    /* expression statement: `accept(User{...});` */
    ASSERT(call_stmt->kind == FENG_STMT_EXPR);
    const FengExpr *call = call_stmt->as.expr;
    ASSERT(call->kind == FENG_EXPR_CALL);
    ASSERT(call->as.call.arg_count == 1U);
    const FengExpr *arg = call->as.call.args[0];

    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, arg);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(site->src_type_decl == user);
    ASSERT(site->target_spec_decl == named);
    ASSERT(site->relation != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_return(void) {
    /* Return coercion: `return User{...};` from a function returning Named
     * records an OBJECT site on the returned expression. */
    const char *src =
        "pu mod demo.coerce;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "fn make(): Named {\n"
        "    return User{n: \"u\"};\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_ret.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *make = find_function_decl_in_program(program, "make");
    ASSERT(make != NULL);
    const FengStmt *ret_stmt = make->as.function_decl.body->statements[0];
    ASSERT(ret_stmt->kind == FENG_STMT_RETURN);
    const FengExpr *ret_expr = ret_stmt->as.return_value;
    ASSERT(ret_expr != NULL);

    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, ret_expr);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);
    ASSERT(site->target_spec_decl == find_spec_decl_by_name(analysis, "Named"));
    ASSERT(site->src_type_decl == find_type_decl_by_name(analysis, "User"));
    ASSERT(site->relation != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_callable_top_level_fn(void) {
    /* `let f: Cb = my_fn;` — top-level function bound to a callable-form
     * spec slot records a CALLABLE site classified as TOP_LEVEL_FN. */
    const char *src =
        "pu mod demo.coerce;\n"
        "spec Cb(x: int): int;\n"
        "fn double(x: int): int { return x + x; }\n"
        "fn caller(): int {\n"
        "    let f: Cb = double;\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_callable_fn.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *caller = find_function_decl_in_program(program, "caller");
    const FengExpr *init = first_let_initializer(&caller->as.function_decl);
    ASSERT(init != NULL && init->kind == FENG_EXPR_IDENTIFIER);

    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, init);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_CALLABLE);
    ASSERT(site->callable_source == FENG_SPEC_COERCION_CALLABLE_SOURCE_TOP_LEVEL_FN);
    ASSERT(site->relation == NULL);
    ASSERT(site->target_spec_decl == find_spec_decl_by_name(analysis, "Cb"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_callable_lambda(void) {
    /* Lambda literal coerced to a callable-form spec slot records a
     * CALLABLE site classified as LAMBDA. */
    const char *src =
        "pu mod demo.coerce;\n"
        "spec Cb(x: int): int;\n"
        "fn caller(): int {\n"
        "    let f: Cb = (x: int) -> x + 1;\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_callable_lambda.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *caller = find_function_decl_in_program(program, "caller");
    const FengExpr *init = first_let_initializer(&caller->as.function_decl);
    ASSERT(init != NULL && init->kind == FENG_EXPR_LAMBDA);

    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, init);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_CALLABLE);
    ASSERT(site->callable_source == FENG_SPEC_COERCION_CALLABLE_SOURCE_LAMBDA);
    ASSERT(site->target_spec_decl == find_spec_decl_by_name(analysis, "Cb"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* --- Phase S2-a: SpecDefaultBinding sidecar tests (§9.3) ------------- */

/* Locate the FengBinding* of the first `let`/`var` statement in fn body. */
static const FengBinding *first_binding_in(const FengCallableSignature *fn) {
    for (size_t i = 0U; i < fn->body->statement_count; ++i) {
        const FengStmt *s = fn->body->statements[i];
        if (s->kind == FENG_STMT_BINDING) {
            return &s->as.binding;
        }
    }
    return NULL;
}

static void test_spec_default_local_binding_object_form(void) {
    /* `let s: Named;` (no initializer) records a LOCAL_BINDING default-
     * witness site against the object-form spec `Named`. */
    const char *src =
        "pu mod demo.defaults;\n"
        "spec Named { fn name(): string; }\n"
        "fn make(): int {\n"
        "    let s: Named;\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("default_local_object.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *make = find_function_decl_in_program(program, "make");
    ASSERT(make != NULL);
    const FengBinding *binding = first_binding_in(&make->as.function_decl);
    ASSERT(binding != NULL);
    ASSERT(binding->initializer == NULL);

    const FengSpecDefaultBinding *site =
        feng_semantic_lookup_spec_default_binding(analysis, binding);
    ASSERT(site != NULL);
    ASSERT(site->position == FENG_SPEC_DEFAULT_BINDING_POSITION_LOCAL_BINDING);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);
    ASSERT(site->spec_decl == find_spec_decl_by_name(analysis, "Named"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_default_local_binding_callable_form(void) {
    /* `let f: Cb;` (no initializer, Cb is callable-form spec) records a
     * LOCAL_BINDING default-witness site with form CALLABLE. */
    const char *src =
        "pu mod demo.defaults;\n"
        "spec Cb(x: int): int;\n"
        "fn make(): int {\n"
        "    let f: Cb;\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("default_local_callable.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *make = find_function_decl_in_program(program, "make");
    const FengBinding *binding = first_binding_in(&make->as.function_decl);
    ASSERT(binding != NULL && binding->initializer == NULL);

    const FengSpecDefaultBinding *site =
        feng_semantic_lookup_spec_default_binding(analysis, binding);
    ASSERT(site != NULL);
    ASSERT(site->position == FENG_SPEC_DEFAULT_BINDING_POSITION_LOCAL_BINDING);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_CALLABLE);
    ASSERT(site->spec_decl == find_spec_decl_by_name(analysis, "Cb"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_default_type_field_no_initializer(void) {
    /* A `var s: Named` field of `type Holder` declared without an
     * initializer at the member declaration site records a TYPE_FIELD
     * default-witness site keyed by the field's FengTypeMember*. A
     * concrete type is also given so analysis succeeds. */
    const char *src =
        "pu mod demo.defaults;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "type Holder {\n"
        "    var named: Named;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("default_field.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *holder = find_type_decl_by_name(analysis, "Holder");
    ASSERT(holder != NULL);
    ASSERT(holder->as.type_decl.member_count == 1U);
    const FengTypeMember *named_field = holder->as.type_decl.members[0];
    ASSERT(named_field->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(named_field->as.field.initializer == NULL);

    const FengSpecDefaultBinding *site =
        feng_semantic_lookup_spec_default_binding(analysis, named_field);
    ASSERT(site != NULL);
    ASSERT(site->position == FENG_SPEC_DEFAULT_BINDING_POSITION_TYPE_FIELD);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);
    ASSERT(site->spec_decl == find_spec_decl_by_name(analysis, "Named"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* --- Phase S2-b: SpecMemberAccess sidecar tests (§9.4) --------------- */

static void test_spec_member_access_field_read(void) {
    /* `s.n` where s has static type Named (object-form spec) records a
     * FIELD_READ entry with mutability VAR (matching the spec field). */
    const char *src =
        "pu mod demo.access;\n"
        "spec Named { var n: string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "}\n"
        "fn read_it(s: Named): string {\n"
        "    return s.n;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("access_read.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *fn = find_function_decl_in_program(program, "read_it");
    const FengStmt *ret = fn->as.function_decl.body->statements[0];
    ASSERT(ret->kind == FENG_STMT_RETURN);
    const FengExpr *member_expr = ret->as.return_value;
    ASSERT(member_expr != NULL && member_expr->kind == FENG_EXPR_MEMBER);

    const FengSpecMemberAccess *site =
        feng_semantic_lookup_spec_member_access(analysis, member_expr);
    ASSERT(site != NULL);
    ASSERT(site->kind == FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_READ);
    ASSERT(site->spec_decl == find_spec_decl_by_name(analysis, "Named"));
    ASSERT(site->member != NULL && site->member->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(site->field_mutability == FENG_MUTABILITY_VAR);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_member_access_field_write(void) {
    /* `s.n = "x";` upgrades the FIELD_READ entry on the LHS member-expr
     * to FIELD_WRITE. */
    const char *src =
        "pu mod demo.access;\n"
        "spec Named { var n: string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "}\n"
        "fn write_it(s: Named) {\n"
        "    s.n = \"x\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("access_write.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *fn = find_function_decl_in_program(program, "write_it");
    const FengStmt *assign = fn->as.function_decl.body->statements[0];
    ASSERT(assign->kind == FENG_STMT_ASSIGN);
    const FengExpr *target = assign->as.assign.target;
    ASSERT(target != NULL && target->kind == FENG_EXPR_MEMBER);

    const FengSpecMemberAccess *site =
        feng_semantic_lookup_spec_member_access(analysis, target);
    ASSERT(site != NULL);
    ASSERT(site->kind == FENG_SPEC_MEMBER_ACCESS_KIND_FIELD_WRITE);
    ASSERT(site->spec_decl == find_spec_decl_by_name(analysis, "Named"));
    ASSERT(site->field_mutability == FENG_MUTABILITY_VAR);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_member_access_method_call(void) {
    /* `s.name()` records a METHOD_CALL entry on the member-expression
     * (the callee of the call), pointing at the spec method. */
    const char *src =
        "pu mod demo.access;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "fn call_it(s: Named): string {\n"
        "    return s.name();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("access_call.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *fn = find_function_decl_in_program(program, "call_it");
    const FengStmt *ret = fn->as.function_decl.body->statements[0];
    ASSERT(ret->kind == FENG_STMT_RETURN);
    const FengExpr *call = ret->as.return_value;
    ASSERT(call != NULL && call->kind == FENG_EXPR_CALL);
    const FengExpr *callee = call->as.call.callee;
    ASSERT(callee != NULL && callee->kind == FENG_EXPR_MEMBER);

    const FengSpecMemberAccess *site =
        feng_semantic_lookup_spec_member_access(analysis, callee);
    ASSERT(site != NULL);
    ASSERT(site->kind == FENG_SPEC_MEMBER_ACCESS_KIND_METHOD_CALL);
    ASSERT(site->spec_decl == find_spec_decl_by_name(analysis, "Named"));
    ASSERT(site->member != NULL && site->member->kind == FENG_TYPE_MEMBER_METHOD);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_member_access_callable_form_rejected(void) {
    /* §9.4: accessing a member on a callable-form spec value is rejected
     * by the resolver and no member-access sidecar entry is recorded. */
    const char *src =
        "pu mod demo.access;\n"
        "spec Cb(x: int): int;\n"
        "fn use_it(c: Cb): int {\n"
        "    return c.bogus;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("access_callable.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    ASSERT(!ok);
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

int main(void) {
    test_match_range_label_overlap_rejected();
    test_match_single_label_overlap_rejected();
    test_match_range_invalid_bounds_rejected();
    test_match_target_type_disallowed();
    test_match_let_bound_label_accepted();
    test_for_in_loop_array_accepted();
    test_for_in_loop_non_array_rejected();
    test_cyclicity_acyclic_chain_marks_none();
    test_cyclicity_self_loop_marks_self();
    test_cyclicity_two_node_cycle_marks_both();
    test_cyclicity_three_node_cycle_marks_all();
    test_cyclicity_array_mediated_cycle_marks_both();
    test_spec_relation_declared_head_recorded();
    test_spec_relation_declared_parent_transitive();
    test_spec_relation_fit_head_and_parent();
    test_spec_relation_visibility_filter();
    test_spec_coercion_object_let_binding();
    test_spec_coercion_object_argument();
    test_spec_coercion_object_return();
    test_spec_coercion_callable_top_level_fn();
    test_spec_coercion_callable_lambda();
    test_spec_default_local_binding_object_form();
    test_spec_default_local_binding_callable_form();
    test_spec_default_type_field_no_initializer();
    test_spec_member_access_field_read();
    test_spec_member_access_field_write();
    test_spec_member_access_method_call();
    test_spec_member_access_callable_form_rejected();
    test_duplicate_type_across_files_same_module();
    test_duplicate_binding_across_files_same_module();
    test_function_return_only_overload_error();
    test_top_level_overload_overlap_via_fit_rejected();
    test_top_level_overload_overlap_via_two_specs_rejected();
    test_top_level_overload_two_specs_no_common_type_accepted();
    test_member_method_overload_overlap_via_fit_rejected();
    test_extern_function_accepts_module_string_library_binding();
    test_extern_function_requires_calling_convention_annotation();
    test_extern_function_rejects_multiple_calling_convention_annotations();
    test_extern_function_rejects_non_string_library_binding();
    test_extern_function_accepts_imported_string_library_binding();
    test_extern_function_rejects_imported_var_library_binding();
    test_extern_function_rejects_array_parameter_type();
    test_extern_function_rejects_array_return_type();
    test_extern_function_rejects_non_fixed_object_parameter();
    test_extern_function_accepts_fixed_object_and_callback_types();
    test_fixed_type_accepts_abi_stable_fields();
    test_fixed_type_rejects_managed_field_type();
    test_fixed_function_type_rejects_union_annotation();
    test_fixed_function_accepts_abi_stable_signature();
    test_fixed_function_rejects_parameterized_calling_convention();
    test_fixed_method_rejects_managed_signature_type();
    test_fixed_function_type_accepts_fixed_function_value();
    test_fixed_function_type_rejects_plain_function_value();
    test_fixed_function_type_rejects_direct_lambda_value();
    test_fixed_function_type_rejects_captured_lambda_binding();
    test_object_form_spec_rejects_fixed_annotation();
    test_object_form_spec_rejects_union_annotation();
    test_fixed_callable_spec_accepts_fixed_type_parameter();
    test_fixed_callable_spec_rejects_non_fixed_type_parameter();
    test_fixed_callable_spec_rejects_object_spec_parameter();
    test_fixed_callable_spec_rejects_non_fixed_return_type();
    test_fixed_function_rejects_uncaught_throw();
    test_fixed_function_allows_locally_caught_throw();
    test_fixed_function_rejects_call_to_throwing_function();
    test_fixed_function_allows_call_to_catching_function();
    test_fixed_method_rejects_uncaught_throw();
    test_fixed_function_allows_unused_lambda_wrapping_throwing_call();
    test_fixed_function_rejects_invoked_lambda_wrapping_throwing_call();
    test_fixed_function_rejects_local_function_value_call_to_throwing_function();
    test_fixed_function_allows_invoked_lambda_wrapping_catching_call();
    test_throw_rejects_void_expression();
    test_finally_rejects_return();
    test_finally_rejects_throw();
    test_finally_rejects_break();
    test_finally_rejects_continue();
    test_break_outside_loop_is_rejected();
    test_continue_outside_loop_is_rejected();
    test_break_inside_lambda_in_loop_is_rejected();
    test_break_and_continue_inside_for_loop_are_accepted();
    test_throw_rejects_pointer_value();
    test_throw_rejects_fixed_type_value();
    test_throw_accepts_string_and_managed_type();
    test_top_level_function_auto_infers_return_type_for_forward_call();
    test_top_level_function_rejects_conflicting_inferred_return_types();
    test_method_auto_infers_return_type_for_forward_call();
    test_imported_function_auto_infers_return_type_across_modules();
    test_omitted_return_function_can_infer_lambda_signature();
    test_omitted_return_function_value_matches_named_function_type();
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
    test_top_level_function_value_rejects_non_function_binding_type();
    test_lambda_body_rejects_function_value_for_non_function_return_type();
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
    test_index_assignment_rejects_readonly_array();
    test_writable_array_literal_does_not_match_readonly_target();
    test_cast_strips_writable_array_to_readonly();
    test_cast_rejects_adding_writable_to_readonly_array();
    test_empty_writable_array_literal_requires_writable_target();
    test_explicit_numeric_and_exact_casts_pass();
    test_cast_rejects_bool_to_numeric();
    test_cast_rejects_numeric_to_bool();
    test_cast_rejects_string_to_numeric();
    test_cast_rejects_array_to_numeric();
    test_cast_rejects_numeric_to_string();
    test_cast_rejects_numeric_to_array();
    test_cast_rejects_string_to_bool();
    test_cast_rejects_bool_to_string();
    test_cast_rejects_numeric_to_object();
    test_cast_rejects_object_to_numeric();
    test_cast_same_type_passes();
    test_index_expression_rejects_float_operand();
    test_index_expression_rejects_bool_operand();
    test_index_expression_rejects_non_array_target();
    test_index_assignment_rejects_non_array_target();
    test_unary_minus_rejects_non_numeric_operand();
    test_unary_not_rejects_non_bool_operand();
    test_binary_plus_rejects_non_matching_operands();
    test_binary_and_rejects_non_bool_operands();
    test_bitwise_ops_accept_same_integer_type();
    test_bitwise_and_rejects_mismatched_integer_types();
    test_bitwise_or_rejects_non_integer_operand();
    test_unary_tilde_rejects_non_integer_operand();
    test_shift_amount_out_of_range_rejected();
    test_const_fold_arithmetic_fits_narrow_target();
    test_const_fold_arithmetic_overflows_narrow_target();
    test_const_fold_division_by_zero_rejected();
    test_const_fold_modulo_by_zero_rejected();
    test_const_fold_i64_overflow_rejected();
    test_const_fold_shift_amount_via_const_expr();
    test_const_fold_cast_truncation_then_target_check();
    test_const_fold_propagates_immutable_local_binding();
    test_const_fold_does_not_propagate_var_binding();
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
    test_self_is_capturable_inside_method_lambda();
    test_alias_member_access_resolves_public_names();
    test_alias_member_access_reports_missing_public_name();
    test_alias_identifier_requires_member_access();
    test_self_reports_unknown_member();
    test_spec_typed_param_supports_field_and_method_access();
    test_spec_typed_param_rejects_let_field_assignment();
    test_spec_typed_param_reports_unknown_member_with_spec_name();
    test_callable_form_spec_typed_param_rejects_member_access();
    test_numeric_literal_adapts_to_explicit_integer_target();
    test_numeric_literal_adapts_to_explicit_alias_targets();
    test_numeric_literal_overflowing_target_is_rejected();
    test_numeric_literal_negative_to_unsigned_target_is_rejected();
    test_numeric_literal_overflows_default_int_target();
    test_object_literal_reports_unknown_field();
    test_object_literal_requires_object_type_target();
    test_object_literal_accepts_constructor_call_target();
    test_constructor_call_uses_implicit_default_constructor();
    test_constructor_call_reports_missing_zero_arg_constructor();
    test_constructor_call_selects_overload_by_literal_type();
    test_constructor_call_selects_overload_by_inferred_local_binding();
    test_constructor_call_reports_type_mismatch();
    test_constructor_call_rejects_function_type();
    test_constructor_call_rejects_object_form_spec();
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
    test_spec_type_satisfaction_succeeds();
    test_spec_parent_specs_must_be_spec();
    test_spec_parent_specs_rejects_duplicate();
    test_spec_parent_specs_rejects_cycle();
    test_type_declared_specs_must_be_spec();
    test_type_declared_specs_rejects_duplicate();
    test_type_declared_specs_missing_field_rejected();
    test_type_declared_specs_field_mutability_mismatch_rejected();
    test_type_declared_specs_method_signature_mismatch_rejected();
    test_type_declared_specs_transitive_satisfaction_required();
    test_type_declared_specs_cross_spec_method_conflict();
    test_fit_target_must_be_type();
    test_fit_specs_must_be_spec();
    test_fit_specs_rejects_duplicate();
    test_fit_body_methods_satisfy_spec();
    test_fit_missing_method_rejected();
    test_orphan_pu_fit_emits_info_and_downgrades();
    test_local_fit_emits_no_orphan_info();
    test_pu_fit_visible_after_use_enables_method_call();
    test_pu_fit_invisible_without_use_rejects_method_call();
    test_imported_pu_fit_satisfies_spec_typed_parameter();
    test_pu_fit_visible_via_alias_use();
    test_fit_method_callable_on_instance();
    test_fit_method_unknown_member_still_rejected();
    test_fit_body_rejects_self_private_field_access();
    test_fit_body_rejects_self_private_method_access();
    test_fit_body_rejects_other_param_private_field_access();
    test_fit_body_rejects_object_literal_private_field();
    test_fit_body_allows_public_member_access();
    test_spec_at_type_position_accepts_satisfying_type();
    test_spec_at_type_position_rejects_unrelated_type();
    test_spec_at_type_position_accepts_via_fit();
    test_resolved_callable_attached_to_call_exprs();
    test_finalizer_basic_ok();
    test_finalizer_rejects_multiple_per_type();
    test_finalizer_rejected_on_fixed_type();
    test_finalizer_rejects_return_with_value();
    test_constructor_rejects_return_with_value();
    test_constructor_with_explicit_void_return_ok();

    test_lambda_block_body_returns_value();
    test_lambda_block_body_records_local_capture();
    test_lambda_in_method_records_self_capture();
    test_field_init_lambda_captures_self_when_callable_spec();
    test_field_init_bare_self_is_invalid();
    test_duplicate_method_signature_is_rejected();
    test_method_overload_return_only_difference_is_rejected();
    test_main_entry_required_for_bin_target();
    test_main_entry_valid_signature_passes_for_bin();
    test_main_entry_bad_signature_is_rejected_for_bin();
    test_multiple_main_entries_rejected_for_bin();
    test_lib_target_skips_main_check();

    puts("semantic tests passed");
    return 0;
}
