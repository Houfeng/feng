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
            collect_calls_in_expr(expr->as.if_expr.then_expr, out);
            collect_calls_in_expr(expr->as.if_expr.else_expr, out);
            break;
        case FENG_EXPR_MATCH:
            collect_calls_in_expr(expr->as.match_expr.target, out);
            for (i = 0U; i < expr->as.match_expr.case_count; ++i) {
                collect_calls_in_expr(expr->as.match_expr.cases[i].label, out);
                collect_calls_in_expr(expr->as.match_expr.cases[i].value, out);
            }
            collect_calls_in_expr(expr->as.match_expr.else_expr, out);
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
            collect_calls_in_stmt(stmt->as.for_stmt.init, out);
            collect_calls_in_expr(stmt->as.for_stmt.condition, out);
            collect_calls_in_stmt(stmt->as.for_stmt.update, out);
            collect_calls_in_block(stmt->as.for_stmt.body, out);
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_non_string_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "library argument must be a string literal or a module-level let binding") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_callback_captured_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "finally_continue_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "finally blocks cannot contain 'continue'") != NULL);

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

    ASSERT(feng_semantic_analyze(programs, 2U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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
        "    fn update() {\n"
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
        "fn update(var user: User) {\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "shift amount") != NULL);
    ASSERT(strstr(errors[0].message, "out of range") != NULL);

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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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
        "spec Factory(): int;\n"
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
        "spec Factory(): int;\n"
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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
        "    fn update() {\n"
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 3U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(analysis != NULL);
    ASSERT(analysis->info_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "no member 'farewell'") != NULL);
    feng_semantic_errors_free(errors, error_count);
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(!feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
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

    ASSERT(feng_semantic_analyze(programs, 1U, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

int main(void) {
    test_duplicate_type_across_files_same_module();
    test_duplicate_binding_across_files_same_module();
    test_function_return_only_overload_error();
    test_extern_function_accepts_module_string_library_binding();
    test_extern_function_requires_calling_convention_annotation();
    test_extern_function_rejects_multiple_calling_convention_annotations();
    test_extern_function_rejects_non_string_library_binding();
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
    test_bitwise_ops_accept_same_integer_type();
    test_bitwise_and_rejects_mismatched_integer_types();
    test_bitwise_or_rejects_non_integer_operand();
    test_unary_tilde_rejects_non_integer_operand();
    test_shift_amount_out_of_range_rejected();
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
    test_fit_method_callable_on_instance();
    test_fit_method_unknown_member_still_rejected();
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
    puts("semantic tests passed");
    return 0;
}
