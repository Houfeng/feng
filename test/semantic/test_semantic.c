#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static bool test_semantic_analyze(const FengProgram *const *programs,
                                  size_t program_count,
                                  FengCompileTarget target,
                                  FengSemanticAnalysis **out_analysis,
                                  FengSemanticError **out_errors,
                                  size_t *out_error_count) {
    FengSemanticAnalyzeOptions options;
    memset(&options, 0, sizeof(options));
    options.target = target;
    options.pointer_size = feng_get_host_pointer_size();
    return feng_semantic_analyze_with_options(programs, program_count,
                                              &options, out_analysis,
                                              out_errors, out_error_count);
}
#define feng_semantic_analyze test_semantic_analyze

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

typedef struct ImportedSourceFixture {
    FengProgram *program;
    FengSemanticAnalysis *analysis;
    FengSymbolGraph *graph;
    FengSymbolProvider *provider;
    FengSymbolImportedModuleCache *cache;
    FengSymbolError error;
} ImportedSourceFixture;

static void imported_source_fixture_init(ImportedSourceFixture *fixture,
                                         const char *path,
                                         const char *source) {
    const FengProgram *programs[1];
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    memset(fixture, 0, sizeof(*fixture));
    fixture->program = parse_program_or_die(path, source);
    programs[0] = fixture->program;
    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &fixture->analysis,
                                 &errors,
                                 &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_symbol_build_graph(fixture->analysis, &fixture->graph, &fixture->error));
    ASSERT(feng_symbol_provider_create(&fixture->provider, &fixture->error));
    ASSERT(feng_symbol_provider_add_graph(fixture->provider, fixture->graph, &fixture->error));
    fixture->cache = feng_symbol_imported_module_cache_create(fixture->provider);
    ASSERT(fixture->cache != NULL);
}

static void imported_source_fixture_dispose(ImportedSourceFixture *fixture) {
    if (fixture == NULL) {
        return;
    }

    feng_symbol_imported_module_cache_free(fixture->cache);
    feng_symbol_provider_free(fixture->provider);
    feng_symbol_graph_free(fixture->graph);
    feng_semantic_analysis_free(fixture->analysis);
    feng_program_free(fixture->program);
    feng_symbol_error_free(&fixture->error);
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
        "open module demo.main;\n"
        "type User {}\n";
    const char *source_b =
        "open module demo.main;\n"
        "type User {}\n";
    FengProgram *program_a = parse_program_or_die("type_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("type_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "type_b.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "duplicate type declaration 'User'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_duplicate_binding_across_files_same_module(void) {
    const char *source_a =
        "module demo.main;\n"
        "let name: string = \"a\";\n";
    const char *source_b =
        "module demo.main;\n"
        "var name: string = \"b\";\n";
    FengProgram *program_a = parse_program_or_die("binding_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("binding_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "binding_b.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "duplicate top-level binding 'name'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_function_return_only_overload_error(void) {
    const char *source =
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: int): string {\n"
        "    return \"value\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("return_overload.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
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
        "module demo.main;\n"
        "spec Animal {\n"
        "    func name(): string;\n"
        "}\n"
        "type Dog {\n"
        "    let name: string;\n"
        "}\n"
        "fit Dog: Animal {\n"
        "    func name(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n"
        "func pet(a: Animal) {\n"
        "}\n"
        "func pet(d: Dog) {\n"
        "}\n";
    FengProgram *program = parse_program_or_die("overload_overlap_via_fit.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func name(): string;\n"
        "}\n"
        "spec Sized {\n"
        "    func size(): int;\n"
        "}\n"
        "type Box {\n"
        "    let label: string;\n"
        "    let count: int;\n"
        "}\n"
        "fit Box: Named {\n"
        "    func name(): string { return self.label; }\n"
        "}\n"
        "fit Box: Sized {\n"
        "    func size(): int { return self.count; }\n"
        "}\n"
        "func show(x: Named) {}\n"
        "func show(x: Sized) {}\n";
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func name(): string;\n"
        "}\n"
        "spec Sized {\n"
        "    func size(): int;\n"
        "}\n"
        "type Tag {\n"
        "    let label: string;\n"
        "}\n"
        "fit Tag: Named {\n"
        "    func name(): string { return self.label; }\n"
        "}\n"
        "type Bucket {\n"
        "    let count: int;\n"
        "}\n"
        "fit Bucket: Sized {\n"
        "    func size(): int { return self.count; }\n"
        "}\n"
        "func show(x: Named) {}\n"
        "func show(x: Sized) {}\n";
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
        "module demo.main;\n"
        "spec Animal {\n"
        "    func name(): string;\n"
        "}\n"
        "type Dog {\n"
        "    let name: string;\n"
        "}\n"
        "fit Dog: Animal {\n"
        "    func name(): string { return self.name; }\n"
        "}\n"
        "type Owner {\n"
        "    func pet(a: Animal) {}\n"
        "    func pet(d: Dog) {}\n"
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

static void test_fit_method_overload_conflicts_match_method_rules(void) {
    static const struct {
        const char *path;
        const char *source;
        const char *message;
    } cases[] = {
        {
            "fit_duplicate_method.f",
            "module demo.main;\n"
            "type Box {}\n"
            "fit Box {\n"
            "    func pick(a: int): int { return a; }\n"
            "    func pick(a: int): int { return a + 1; }\n"
            "}\n",
            "duplicate method signature"
        },
        {
            "fit_return_only_method.f",
            "module demo.main;\n"
            "type Box {}\n"
            "fit Box {\n"
            "    func pick(a: int): int { return a; }\n"
            "    func pick(a: int): bool { return true; }\n"
            "}\n",
            "cannot differ only by return type"
        },
        {
            "fit_variadic_method.f",
            "module demo.main;\n"
            "type Box {}\n"
            "fit Box {\n"
            "    func pick(a: int) {}\n"
            "    func pick(values: int...) {}\n"
            "}\n",
            "variadic method overload conflicts"
        },
        {
            "fit_static_duplicate_method.f",
            "module demo.main;\n"
            "type Box {}\n"
            "fit Box {\n"
            "    static func make(): int { return 1; }\n"
            "    static func make(): int { return 2; }\n"
            "}\n",
            "duplicate method signature"
        },
        {
            "fit_overlap_method.f",
            "module demo.main;\n"
            "spec Animal {\n"
            "    func name(): string;\n"
            "}\n"
            "type Dog {\n"
            "    let name: string;\n"
            "}\n"
            "fit Dog: Animal {\n"
            "    func name(): string { return self.name; }\n"
            "}\n"
            "type Owner {}\n"
            "fit Owner {\n"
            "    func pet(a: Animal) {}\n"
            "    func pet(d: Dog) {}\n"
            "}\n",
            "may both match the same arguments"
        }
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = parse_program_or_die(cases[i].path, cases[i].source);
        const FengProgram *programs[] = {program};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
        ASSERT(error_count >= 1U);
        ASSERT(strstr(errors[0].message, cases[i].message) != NULL);

        feng_semantic_errors_free(errors, error_count);
        feng_program_free(program);
    }
}

static void test_extern_function_accepts_module_string_library_binding(void) {
    const char *source =
        "module demo.main;\n"
        "let math_lib = \"m\";\n"
        "@cdecl(math_lib)\n"
        "extern func sin(x: float): float;\n";
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

static void test_extern_function_accepts_c_symbol_name_argument(void) {
    const char *source =
        "module demo.main;\n"
        "let c_name = \"fabs\";\n"
        "@cdecl(\"m\", c_name)\n"
        "extern func abs_value(x: double): double;\n";
    FengProgram *program = parse_program_or_die("extern_fn_c_symbol_arg_ok.f", source);
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

static void test_extern_function_accepts_imported_string_c_symbol_binding(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open let c_name = \"fabs\";\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "@cdecl(\"m\", c_name)\n"
        "extern func abs_value(x: double): double;\n";
    FengProgram *base_program = parse_program_or_die("extern_fn_imported_c_symbol_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("extern_fn_imported_c_symbol_main.f", main_source);
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

static void test_extern_function_without_calling_convention_annotation_is_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "extern func sin(x: float): float;\n";
    FengProgram *program = parse_program_or_die("extern_fn_missing_callconv_ok.f", source);
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

static void test_extern_function_accepts_string_parameter_without_c_abi_annotation(void) {
    const char *source =
        "module demo.main;\n"
        "extern func print(msg: string): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_string_param_without_c_abi_ok.f", source);
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

static void test_extern_function_accepts_string_return_without_c_abi_annotation(void) {
    const char *source =
        "module demo.main;\n"
        "extern func load(): string;\n";
    FengProgram *program = parse_program_or_die("extern_fn_string_return_without_c_abi_ok.f", source);
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

static void test_runtime_annotation_accepts_top_level_extern_function(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "extern func feng_string_length(value: string): i64;\n";
    FengProgram *program = parse_program_or_die("runtime_extern_ok.f", source);
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

static void test_runtime_annotation_rejects_non_extern_function(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "func feng_string_length(value: string): i64 {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("runtime_non_extern_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "runtime_non_extern_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "cannot use @runtime unless it is declared extern") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_runtime_annotation_rejects_c_abi_target_annotation(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "@cdecl(\"m\")\n"
        "extern func feng_string_length(value: string): i64;\n";
    FengProgram *program = parse_program_or_die("runtime_callconv_conflict_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "runtime_callconv_conflict_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "cannot combine @runtime with C ABI target annotations") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_runtime_annotation_rejects_abi_annotation(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "@abi\n"
        "extern func feng_string_length(value: string): i64;\n";
    FengProgram *program = parse_program_or_die("runtime_abi_conflict_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "runtime_abi_conflict_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "cannot combine @runtime with @abi") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_runtime_annotation_rejects_type_declaration(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "type User {\n"
        "    var name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("runtime_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "runtime_type_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "@runtime only applies to top-level extern func declarations") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_runtime_annotation_rejects_member_method(void) {
    const char *source =
        "module demo.main;\n"
        "type User {\n"
        "    @runtime\n"
        "    func name(): string {\n"
        "        return \"guest\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("runtime_member_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "runtime_member_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "@runtime only applies to top-level extern func declarations") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_annotation_accepts_type_declaration(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type Point {\n"
        "    var x: float;\n"
        "    var y: float;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_type_accepted.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_annotation_rejects_spec_declaration(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "spec Describable {\n"
        "    func describe(): string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_spec_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "value_spec_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "@value only applies to type declarations") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_annotation_rejects_function_declaration(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "func greet(): void {\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_func_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "value_func_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "@value only applies to type declarations") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_annotation_rejects_binding(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "let x: int = 1;\n";
    FengProgram *program = parse_program_or_die("value_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "value_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "@value only applies to type declarations") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* --- §9.2 value-type cycle detection ---------------------------------- */

static void test_tuple_direct_self_reference_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type A(A, int);\n";
    FengProgram *program = parse_program_or_die("tuple_self_ref.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    bool found_cycle_error = false;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            found_cycle_error = true;
            ASSERT(strcmp(errors[i].path, "tuple_self_ref.f") == 0);
            ASSERT(strstr(errors[i].message, "value-type cycle") != NULL);
            ASSERT(strstr(errors[i].message, "tuple") != NULL);
        }
    }
    ASSERT(found_cycle_error);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_tuple_indirect_cycle_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type B(C, int);\n"
        "type C(B, int);\n";
    FengProgram *program = parse_program_or_die("tuple_indirect_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    /* Both B and C participate in the cycle, so we expect two AE1327 errors. */
    size_t cycle_errors = 0U;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            ++cycle_errors;
            ASSERT(strstr(errors[i].message, "value-type cycle") != NULL);
        }
    }
    ASSERT(cycle_errors == 2U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_type_direct_self_reference_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type A {\n"
        "    var inner: A;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_self_ref.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    bool found_cycle_error = false;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            found_cycle_error = true;
            ASSERT(strstr(errors[i].message, "value-type cycle") != NULL);
            ASSERT(strstr(errors[i].message, "@value type") != NULL);
        }
    }
    ASSERT(found_cycle_error);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_type_indirect_cycle_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type B {\n"
        "    var c: C;\n"
        "}\n"
        "@value\n"
        "type C {\n"
        "    var b: B;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_indirect_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    size_t cycle_errors = 0U;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            ++cycle_errors;
        }
    }
    ASSERT(cycle_errors == 2U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_ordinary_type_self_reference_allowed(void) {
    /* Ordinary (heap-allocated) type decls may reference themselves —
     * managed-pointer fields have fixed size regardless of the referent. */
    const char *source =
        "module demo.main;\n"
        "type Node {\n"
        "    var next: Node;\n"
        "    var value: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ordinary_self_ref.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_type_holding_managed_pointer_allowed(void) {
    /* A @value type holding a managed-pointer field to an ordinary type
     * is not cyclic — the edge model only considers value-type targets. */
    const char *source =
        "module demo.main;\n"
        "type Node {\n"
        "    var value: int;\n"
        "}\n"
        "@value\n"
        "type Wrapper {\n"
        "    var node: Node;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_holds_managed.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_tuple_array_of_self_rejected(void) {
    /* `type A(A[], int)` still requires A to have finite inline size —
     * array unwrapping reaches the leaf element type, which is A itself. */
    const char *source =
        "module demo.main;\n"
        "type A(A[], int);\n";
    FengProgram *program = parse_program_or_die("tuple_array_self_ref.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    bool found_cycle_error = false;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            found_cycle_error = true;
        }
    }
    ASSERT(found_cycle_error);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_mixed_tuple_and_value_type_cycle_rejected(void) {
    /* A tuple and a @value type referencing each other form a cycle
     * among value types, regardless of which flavour each node is. */
    const char *source =
        "module demo.main;\n"
        "type T(V, int);\n"
        "@value\n"
        "type V {\n"
        "    var t: T;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("mixed_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    size_t cycle_errors = 0U;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            ++cycle_errors;
        }
    }
    ASSERT(cycle_errors == 2U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Generic type arguments can create value-type cycles that are not visible
 * from the generic declaration alone. For example, `@value type Box<T>` is
 * acyclic by itself, but `@value type Node { var box: Box<Node>; }` creates
 * a cycle because Box<Node> inlines Node which inlines Box<Node>... */

static void test_value_type_generic_arg_cycle_rejected(void) {
    /* @value type Box<T> { var value: T; } + @value type Node { var box: Box<Node>; }
     * Box<Node>.value is Node, Node.box is Box<Node> → infinite size. */
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "}\n"
        "@value\n"
        "type Node {\n"
        "    var box: Box<Node>;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_arg_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    /* Node has edges: Node→Box (base type), Node→Node (type arg). Self-loop detected. */
    bool found_node_cycle = false;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0 &&
            strstr(errors[i].message, "Node") != NULL) {
            found_node_cycle = true;
        }
    }
    ASSERT(found_node_cycle);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_type_generic_nested_arg_cycle_rejected(void) {
    /* @value type W<T> { var inner: T; } + @value type Node { var w: W<W<Node>>; }
     * W<W<Node>>.inner is W<Node>, W<Node>.inner is Node → cycle. */
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type W<T> {\n"
        "    var inner: T;\n"
        "}\n"
        "@value\n"
        "type Node {\n"
        "    var w: W<W<Node>>;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_nested_arg_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    bool found_node_cycle = false;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0 &&
            strstr(errors[i].message, "Node") != NULL) {
            found_node_cycle = true;
        }
    }
    ASSERT(found_node_cycle);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_value_type_generic_no_cycle_allowed(void) {
    /* @value type Pair<A, B> { var first: A; var second: B; }
     * @value type Point { var x: int; var y: int; }
     * var p: Pair<Point, int>; — no cycle, Pair and Point are acyclic. */
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type Pair<A, B> {\n"
        "    var first: A;\n"
        "    var second: B;\n"
        "}\n"
        "@value\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@value\n"
        "type Container {\n"
        "    var p: Pair<Point, int>;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_no_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_deep_indirect_cycle_rejected(void) {
    /* Length-4 indirect cycle A→B→C→D→A. Tarjan SCC must detect cycles of
     * any length, not just self-loops and length-2 cycles. */
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type A {\n"
        "    var b: B;\n"
        "}\n"
        "@value\n"
        "type B {\n"
        "    var c: C;\n"
        "}\n"
        "@value\n"
        "type C {\n"
        "    var d: D;\n"
        "}\n"
        "@value\n"
        "type D {\n"
        "    var a: A;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("deep_indirect_cycle.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    /* All four participants A, B, C, D must be reported. */
    size_t cycle_errors = 0U;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            ++cycle_errors;
        }
    }
    ASSERT(cycle_errors == 4U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cross_file_value_type_cycle_rejected(void) {
    /* Two files in the same module may declare value types that form a
     * cycle — the detector must see across file boundaries. */
    const char *source_a =
        "open module demo.main;\n"
        "@value\n"
        "type X {\n"
        "    var y: Y;\n"
        "}\n";
    const char *source_b =
        "open module demo.main;\n"
        "@value\n"
        "type Y {\n"
        "    var x: X;\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("cross_file_cycle_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("cross_file_cycle_b.f", source_b);
    const FengProgram *programs[] = {program_a, program_b};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    size_t cycle_errors = 0U;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            ++cycle_errors;
        }
    }
    ASSERT(cycle_errors == 2U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
}

static void test_cross_module_value_type_cycle_rejected(void) {
    /* Two modules with mutual value-type references via full-qualified
     * names form a cycle that must be detected. Both modules and types
     * must be `open` so they are visible to each other. */
    const char *base_source =
        "open module demo.base;\n"
        "@value\n"
        "open type Base {\n"
        "    var m: demo.main.Main;\n"
        "}\n";
    const char *main_source =
        "open module demo.main;\n"
        "import demo.base;\n"
        "@value\n"
        "open type Main {\n"
        "    var b: demo.base.Base;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("cross_module_cycle_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("cross_module_cycle_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    size_t cycle_errors = 0U;
    for (size_t i = 0U; i < error_count; ++i) {
        if (strcmp(errors[i].code, "AE1327") == 0) {
            ++cycle_errors;
        }
    }
    ASSERT(cycle_errors == 2U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_pointer_to_self_allowed(void) {
    /* A @value type holding a raw pointer to itself is NOT cyclic —
     * raw pointers (`*T`) are fixed-size regardless of the referent. */
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type Node {\n"
        "    var next: Node*;\n"
        "    var value: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("pointer_self_ref.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_type_spec_field_allowed(void) {
    /* A @value type holding an object-form spec field does not create
     * a value-type edge — spec fields are fat values (subject+witness). */
    const char *source =
        "module demo.main;\n"
        "spec Describable {\n"
        "    func describe(): string;\n"
        "}\n"
        "@value\n"
        "type Wrapper {\n"
        "    var d: Describable;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_spec_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_type_union_spec_field_allowed(void) {
    /* A @value type holding a union-form spec field does not create
     * a value-type cycle — union specs are tag+payload values, not
     * value-type decls. */
    const char *source =
        "module demo.main;\n"
        "spec Result: int | string;\n"
        "@value\n"
        "type Wrapper {\n"
        "    var r: Result;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_union_spec_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_type_callable_spec_field_allowed(void) {
    /* A @value type holding a callable spec field does not create a
     * value-type cycle — callable specs are closure pointers. */
    const char *source =
        "module demo.main;\n"
        "spec Callback(x: int): int;\n"
        "@value\n"
        "type Handler {\n"
        "    var cb: Callback;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_callable_spec_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_type_enum_field_allowed(void) {
    /* A @value type holding an enum field does not create a value-type
     * cycle — enum decls are not value-type decls. */
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "@value\n"
        "type Pixel {\n"
        "    var color: Color;\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_enum_field.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* --- §9.3 @value type finalizer prohibition --------------------------- */

/* @value types use value semantics (copy-on-assign); a finalizer would run
 * on every copy and risk double-free. Verify that AE1328 is reported. */
static void test_value_type_finalizer_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type Buffer {\n"
        "    var size: int;\n"
        "    func ~Buffer() {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("value_fin_rejected.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "@value") != NULL);
    ASSERT(strstr(errors[0].message, "finalizer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Ordinary (non-@value) types may still declare a finalizer. */
static void test_ordinary_type_finalizer_allowed(void) {
    const char *source =
        "module demo.main;\n"
        "type Buffer {\n"
        "    var size: int;\n"
        "    func ~Buffer() {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ordinary_fin_allowed.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_extern_function_rejects_multiple_calling_convention_annotations(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\")\n"
        "@stdcall(\"m\")\n"
        "extern func sin(x: float): float;\n";
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

static void test_extern_function_rejects_too_many_calling_convention_arguments(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\", \"fabs\", \"extra\")\n"
        "extern func abs_value(x: double): double;\n";
    FengProgram *program = parse_program_or_die("extern_fn_too_many_callconv_args_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_too_many_callconv_args_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message,
                  "fixed parameter count must be an integer literal") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_non_string_library_binding(void) {
    const char *source =
        "module demo.main;\n"
        "var math_lib = \"m\";\n"
        "@cdecl(math_lib)\n"
        "extern func sin(x: float): float;\n";
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

static void test_extern_function_rejects_non_string_c_symbol_binding(void) {
    const char *source =
        "module demo.main;\n"
        "var c_name = \"fabs\";\n"
        "@cdecl(\"m\", c_name)\n"
        "extern func abs_value(x: double): double;\n";
    FengProgram *program = parse_program_or_die("extern_fn_non_string_c_symbol_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_non_string_c_symbol_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "C function name argument must be a string literal or a visible let binding") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_accepts_imported_string_library_binding(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open let math_lib = \"m\";\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "@cdecl(math_lib)\n"
        "extern func sin(x: float): float;\n";
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
        "open module demo.base;\n"
        "open var math_lib = \"m\";\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "@cdecl(math_lib)\n"
        "extern func sin(x: float): float;\n";
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

static void test_extern_function_accepts_abi_array_parameter_type(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern func fill(values: int[]): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_array_param_ok.f", source);
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

static void test_extern_function_accepts_abi_array_return_type(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern func load(name: int): int[];\n";
    FengProgram *program = parse_program_or_die("extern_fn_array_return_ok.f", source);
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

static void test_extern_function_rejects_bare_string_parameter_type(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern func print(msg: string): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_string_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_string_param_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "parameter 'msg' type 'string' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_bare_string_return_type(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern func load(): string;\n";
    FengProgram *program = parse_program_or_die("extern_fn_string_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_string_return_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "return type 'string' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_non_abi_array_parameter_type(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\")\n"
        "extern func fill(values: string[]): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_string_array_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_string_array_param_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "parameter 'values' type 'string[]' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_non_abi_object_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"m\")\n"
        "extern func use_point(point: Point): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_non_abi_object_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fn_non_abi_object_param_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "parameter 'point' type 'Point' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_accepts_abi_object_and_callback_types(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@abi\n"
        "spec PointCallback(p: Point): int;\n"
        "@cdecl(\"m\")\n"
        "extern func run_point(point: Point, cb: PointCallback): int;\n";
    FengProgram *program = parse_program_or_die("extern_fn_abi_types_ok.f", source);
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

static void test_fixed_annotation_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "@fixed\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fixed_annotation_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "fixed_annotation_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "unknown annotation '@fixed' is not supported") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_type_accepts_abi_stable_fields(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Callback(x: int): int;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@abi\n"
        "type Slice {\n"
        "    var data: byte*;\n"
        "    var next: Point*;\n"
        "    var callback: Callback*;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_type_ok.f", source);
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

static void test_abi_type_rejects_managed_field_type(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type NameBox {\n"
        "    var name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_type_managed_field_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_type_managed_field_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "type 'NameBox' cannot be marked as @abi") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_type_rejects_inline_abi_object_field_type(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "}\n"
        "@abi\n"
        "type Box {\n"
        "    var point: Point;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_type_inline_object_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_type_inline_object_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message,
                  "field 'point' uses non-ABI-stable type 'Point'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_type_rejects_direct_array_field_type(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Box {\n"
        "    var values: int[];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_type_array_field_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_type_array_field_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected),
                 "field 'values' uses non-ABI-stable type '%s[]'", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_type_rejects_direct_callable_field_type(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Callback(x: int): int;\n"
        "@abi\n"
        "type Holder {\n"
        "    var cb: Callback;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_type_callable_field_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_type_callable_field_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "field 'cb' uses non-ABI-stable type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unknown_top_level_annotation_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "@union\n"
        "spec Cmp(a: int, b: int): int;\n";
    FengProgram *program = parse_program_or_die("unknown_top_level_annotation_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unknown_top_level_annotation_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "unknown annotation '@union' is not supported") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_bounded_annotation_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "@bounded\n"
        "type User {\n"
        "    let id: int = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("bounded_annotation_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "bounded_annotation_error.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "unknown annotation '@bounded' is not supported") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_accepts_abi_stable_signature(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_ok.f", source);
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

static void test_abi_function_accepts_fieldless_abi_type_pointer_signature(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "func roundtrip(handle: Handle*): Handle* {\n"
        "    return handle;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_fieldless_pointer_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_abi_function_rejects_fieldless_abi_type_value_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "func close(handle: Handle): int {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_fieldless_value_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_fieldless_value_param_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "function 'close' cannot be marked as @abi because parameter 'handle' uses non-ABI-stable type 'Handle'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_rejects_fieldless_abi_type_value_return(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "func make_handle(): Handle {\n"
        "    return Handle {};\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_fieldless_value_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_fieldless_value_return_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "function 'make_handle' cannot be marked as @abi because return type 'Handle' is not ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_accepts_abi_value_param_and_return(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func create_point(x: int, y: int): Point;\n"
        "@cdecl(\"c\")\n"
        "extern func point_sum(p: Point): int;\n"
        "func run() {\n"
        "    let point: Point = create_point(1, 2);\n"
        "    let total: int = point_sum(point);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("extern_abi_value_signature_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_extern_function_accepts_fieldless_abi_type_pointer_param_and_return(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func handle_open(): Handle*;\n"
        "@cdecl(\"c\")\n"
        "extern func handle_close(handle: Handle*): void;\n"
        "func run() {\n"
        "    let handle: Handle* = handle_open();\n"
        "    handle_close(handle);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("extern_fieldless_abi_pointer_signature_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_extern_function_rejects_fieldless_abi_type_value_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func handle_close(handle: Handle): void;\n";
    FengProgram *program = parse_program_or_die("extern_fieldless_abi_value_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fieldless_abi_value_param_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "parameter 'handle' type 'Handle' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_fieldless_abi_type_value_return(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func handle_open(): Handle;\n";
    FengProgram *program = parse_program_or_die("extern_fieldless_abi_value_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_fieldless_abi_value_return_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "return type 'Handle' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_extern_function_rejects_non_abi_type_pointer_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "type Handle {\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func handle_close(handle: Handle*): void;\n";
    FengProgram *program = parse_program_or_die("extern_non_abi_pointer_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "extern_non_abi_pointer_param_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message,
                  "parameter 'handle' type 'Handle*' is not C ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_accepts_abi_array_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "func sum(values: int[]): int {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_array_param_ok.f", source);
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

static void test_abi_function_rejects_parameterized_calling_convention(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "@cdecl(\"m\")\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_callconv_arg_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_callconv_arg_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "function 'cmp' cannot be marked as @abi") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_method_rejects_managed_signature_type(void) {
    const char *source =
        "module demo.main;\n"
        "type CallbackHolder {\n"
        "    @abi\n"
        "    func emit(msg: string) {\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_method_managed_signature_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_method_managed_signature_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "method 'emit' cannot be marked as @abi") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_type_accepts_abi_function_value(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Callback(x: int): int;\n"
        "@abi\n"
        "func add1(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "func run() {\n"
        "    let cb: Callback = add1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_callback_abi_fn_ok.f", source);
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

static void test_abi_function_type_rejects_plain_function_value(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Callback(x: int): int;\n"
        "func add1(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "func run() {\n"
        "    let cb: Callback = add1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_callback_plain_fn_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_callback_plain_fn_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_type_rejects_direct_lambda_value(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Callback(x: int): int;\n"
        "func run() {\n"
        "    let cb: Callback = (x: int) -> x + 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_callback_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_callback_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_type_rejects_captured_lambda_binding(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Callback(x: int): int;\n"
        "func run(base: int) {\n"
        "    let cb: Callback = (x: int) -> x + base;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_callback_captured_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_callback_captured_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'Callback'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_form_spec_rejects_abi_annotation(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Shape {\n"
        "    var x: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_spec_abi_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_spec_abi_error.f") == 0);
    ASSERT(strstr(errors[0].message,
                  "object-form spec 'Shape' cannot be marked as @abi") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unknown_member_annotation_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Shape {\n"
        "    @union\n"
        "    var x: int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unknown_member_annotation_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "unknown annotation '@union' is not supported") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_callable_spec_accepts_abi_type_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@abi\n"
        "spec PointHandler(p: Point): int;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_ok.f", source);
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

static void test_abi_callable_spec_accepts_fieldless_abi_type_pointer_signature(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "spec HandleCb(handle: Handle*): Handle*;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_fieldless_pointer_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_abi_callable_spec_accepts_abi_array_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Batch(values: int[]): int;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_array_ok.f", source);
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

static void test_abi_callable_spec_rejects_non_abi_array_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Batch(values: string[]): int;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_string_array_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Batch' cannot be marked as @abi because parameter 'values' uses non-ABI-stable type 'string[]'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_callable_spec_rejects_non_abi_type_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "type Bag {\n"
        "    var name: string;\n"
        "}\n"
        "@abi\n"
        "spec Cb(b: Bag): int;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_non_abi_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Cb' cannot be marked as @abi because parameter 'b' uses non-ABI-stable type 'Bag'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_callable_spec_rejects_fieldless_abi_type_value_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "spec HandleCb(handle: Handle): int;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_fieldless_value_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_callable_spec_fieldless_value_param_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "type 'HandleCb' cannot be marked as @abi because parameter 'handle' uses non-ABI-stable type 'Handle'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_callable_spec_rejects_object_spec_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "spec Shape {\n"
        "    var x: int;\n"
        "}\n"
        "@abi\n"
        "spec Cb(s: Shape): int;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_object_spec_param_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Cb' cannot be marked as @abi because parameter 's' uses non-ABI-stable type 'Shape'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_callable_spec_rejects_non_abi_return_type(void) {
    const char *source =
        "module demo.main;\n"
        "type Bag {\n"
        "    var name: string;\n"
        "}\n"
        "@abi\n"
        "spec Cb(x: int): Bag;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_non_abi_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Cb' cannot be marked as @abi because return type 'Bag' is not ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_callable_spec_rejects_fieldless_abi_type_value_return(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "spec HandleCb(): Handle;\n";
    FengProgram *program = parse_program_or_die("abi_callable_spec_fieldless_value_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_callable_spec_fieldless_value_return_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "type 'HandleCb' cannot be marked as @abi because return type 'Handle' is not ABI-stable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_rejects_uncaught_throw(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "func fail(): int {\n"
        "    throw \"boom\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_uncaught_throw_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_uncaught_throw_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @abi ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_allows_locally_caught_throw(void) {
    const char *source =
        "module demo.main;\n"
        "func fail(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@abi\n"
        "func recover(): int {\n"
        "    let value = try fail() catch ex: string { 0; };\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_caught_throw_ok.f", source);
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

static void test_abi_function_rejects_call_to_throwing_function(void) {
    const char *source =
        "module demo.main;\n"
        "func helper(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@abi\n"
        "func run(): int {\n"
        "    return helper();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_throwing_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_throwing_call_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @abi ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_allows_call_to_catching_function(void) {
    const char *source =
        "module demo.main;\n"
        "func fail(): int {\n"
        "        throw \"boom\";\n"
        "}\n"
        "func helper(): int {\n"
        "    let value = try fail() catch ex: string { 0; };\n"
        "    return value;\n"
        "}\n"
        "@abi\n"
        "func run(): int {\n"
        "    return helper();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_catching_call_ok.f", source);
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

static void test_abi_method_rejects_uncaught_throw(void) {
    const char *source =
        "module demo.main;\n"
        "type Worker {\n"
        "    @abi\n"
        "    func run() {\n"
        "        throw \"boom\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_method_uncaught_throw_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_method_uncaught_throw_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @abi ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_allows_unused_lambda_wrapping_throwing_call(void) {
    const char *source =
        "module demo.main;\n"
        "spec Callback(x: int): int;\n"
        "func helper(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@abi\n"
        "func run(): int {\n"
        "    let wrap: Callback = (x: int) -> helper();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_unused_lambda_throwing_call_ok.f", source);
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

static void test_abi_function_rejects_invoked_lambda_wrapping_throwing_call(void) {
    const char *source =
        "module demo.main;\n"
        "spec Callback(x: int): int;\n"
        "func helper(): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@abi\n"
        "func run(): int {\n"
        "    let wrap: Callback = (x: int) -> helper();\n"
        "    return wrap(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_invoked_lambda_throwing_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_invoked_lambda_throwing_call_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @abi ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_rejects_local_function_value_call_to_throwing_function(void) {
    const char *source =
        "module demo.main;\n"
        "spec Callback(x: int): int;\n"
        "func helper(x: int): int {\n"
        "    throw \"boom\";\n"
        "}\n"
        "@abi\n"
        "func run(): int {\n"
        "    let cb: Callback = helper;\n"
        "    return cb(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_local_function_value_throwing_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "abi_fn_local_function_value_throwing_call_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "uncaught exceptions must not cross the @abi ABI boundary") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_abi_function_allows_invoked_lambda_wrapping_catching_call(void) {
    const char *source =
        "module demo.main;\n"
        "spec Callback(x: int): int;\n"
        "func fail(): int {\n"
        "        throw \"boom\";\n"
        "}\n"
        "func helper(): int {\n"
        "    let value = try fail() catch ex: string { 0; };\n"
        "    return value;\n"
        "}\n"
        "@abi\n"
        "func run(): int {\n"
        "    let wrap: Callback = (x: int) -> helper();\n"
        "    return wrap(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_fn_invoked_lambda_catching_call_ok.f", source);
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
        "module demo.main;\n"
        "func side() {\n"
        "}\n"
        "func run() {\n"
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

static void test_break_outside_loop_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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

static void test_defer_inside_function_is_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): int {\n"
        "    var x = 1;\n"
        "    defer {\n"
        "        x = x + 1;\n"
        "    }\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_in_function.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_defer_with_return_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): int {\n"
        "    defer {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "defer_return_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE1501") == 0);
    ASSERT(strstr(errors[0].message,
                  "defer block cannot contain 'return'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_defer_with_throw_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    defer {\n"
        "        throw \"boom\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_throw_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "defer_throw_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE1502") == 0);
    ASSERT(strstr(errors[0].message,
                  "defer block cannot contain 'throw'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_defer_with_nested_defer_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    defer {\n"
        "        defer {\n"
        "            1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_nested_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "defer_nested_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE1503") == 0);
    ASSERT(strstr(errors[0].message,
                  "defer block cannot contain nested 'defer'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_defer_with_break_at_top_level_of_defer_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    while true {\n"
        "        defer {\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_break_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "defer_break_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strcmp(errors[0].code, "AE1504") == 0);
    ASSERT(strstr(errors[0].message,
                  "defer block cannot contain 'break'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_defer_with_continue_at_top_level_of_defer_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    while true {\n"
        "        defer {\n"
        "            continue;\n"
        "        }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_continue_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "defer_continue_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strcmp(errors[0].code, "AE1505") == 0);
    ASSERT(strstr(errors[0].message,
                  "defer block cannot contain 'continue'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_defer_with_break_inside_nested_loop_is_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    while true {\n"
        "        defer {\n"
        "            while true {\n"
        "                break;\n"
        "            }\n"
        "        }\n"
        "        break;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("defer_break_in_nested_loop.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_break_inside_lambda_in_loop_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Action(): void;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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

static void test_break_directly_in_if_expr_block_is_rejected(void) {
    /* break directly inside an if-expression block is invalid because the
     * expression must produce a value on every path. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    while (true) {\n"
        "        let x: bool = if (true) { break; false } else { false };\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_in_if_expr_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "break_in_if_expr_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message,
                  "'break' cannot appear directly inside an 'if' expression block") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_continue_directly_in_if_expr_block_is_rejected(void) {
    /* continue directly inside an if-expression block is invalid for the same
     * reason as break: the expression must produce a value. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    while (true) {\n"
        "        let x: bool = if (true) { true } else { continue; false };\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("continue_in_if_expr_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "continue_in_if_expr_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message,
                  "'continue' cannot appear directly inside an 'if' expression block") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_break_inside_loop_inside_if_expr_block_is_accepted(void) {
    /* A loop nested inside an if-expression block may contain break/continue
     * normally because those target the inner loop, not the if-expression. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: bool = if (true) {\n"
        "        while (true) {\n"
        "            break;\n"
        "        }\n"
        "        true\n"
        "    } else {\n"
        "        false\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_in_loop_in_if_expr_ok.f", source);
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

static void test_if_expr_trailing_if_else_in_then_branch_accepted(void) {
    /* The last statement of an if-expression branch may be an if/else
     * construct; the parser converts it to an expression form so the
     * outer if-expression can extract its value. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        if true { 1 } else { 2 }\n"
        "    } else {\n"
        "        0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_in_then_ok.f", source);
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

static void test_if_expr_trailing_if_else_in_else_branch_accepted(void) {
    /* Same as above but with the trailing if/else in the else branch. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        0\n"
        "    } else {\n"
        "        if true { 1 } else { 2 }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_in_else_ok.f", source);
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

static void test_if_expr_trailing_if_else_chain_accepted(void) {
    /* An else-if chain at the end of a branch should also be converted
     * to nested if-expressions. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        if true { 1 } else if false { 2 } else { 3 }\n"
        "    } else {\n"
        "        0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_chain_ok.f", source);
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

static void test_if_expr_trailing_match_accepted(void) {
    /* A match at the end of a branch should be treated as a
     * match expression. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let v: i32 = 1;\n"
        "    let x: i32 = if true {\n"
        "        match v {\n"
        "            1 { 10 }\n"
        "            2 { 20 }\n"
        "            else { 30 }\n"
        "        }\n"
        "    } else {\n"
        "        0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_match_ok.f", source);
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

static void test_if_expr_trailing_try_catch_accepted(void) {
    /* A try/catch at the end of a branch should be treated as a try
     * expression. */
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        try parse() catch ex: i32 { 0 }\n"
        "    } else {\n"
        "        0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_try_in_if_expr_ok.f", source);
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

static void test_if_expr_non_expr_trailing_binding_rejected(void) {
    /* A binding (let/var) at the end of an if-expression branch is not
     * an expression and cannot be converted; AE1101 must be reported. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        let y: i32 = 1;\n"
        "    } else {\n"
        "        0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_binding_in_if_expr.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "trailing_binding_in_if_expr.f") == 0);
    ASSERT(strstr(errors[0].message, "branch block must end with an expression statement") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_if_expr_trailing_if_without_else_rejected(void) {
    /* An if without else at the end of an if-expression branch cannot
     * produce a value; AE1101 must be reported for the inner if-expr. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        if true { 1 }\n"
        "    } else {\n"
        "        0\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_no_else.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "trailing_if_no_else.f") == 0);
    ASSERT(strstr(errors[0].message, "if expressions require an else branch") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_trailing_if_else_in_branch_accepted(void) {
    /* An if/else at the end of a match branch should be converted
     * to an if-expression so the branch can yield a value. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let v: i32 = 1;\n"
        "    let x: i32 = match v {\n"
        "        1 {\n"
        "            if true { 10 } else { 20 }\n"
        "        }\n"
        "        else { 0 }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_in_match_branch_ok.f", source);
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

static void test_match_trailing_if_else_in_else_branch_accepted(void) {
    /* An if/else at the end of a match else branch should also be
     * converted to an if-expression. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let v: i32 = 1;\n"
        "    let x: i32 = match v {\n"
        "        1 { 10 }\n"
        "        else {\n"
        "            if true { 20 } else { 30 }\n"
        "        }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_in_match_else_ok.f", source);
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

static void test_try_expr_trailing_if_else_in_catch_accepted(void) {
    /* An if/else at the end of a try-expression catch clause body should
     * be converted to an if-expression so the catch clause can yield a
     * value. */
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func run() {\n"
        "    let x: i32 = try parse() catch ex: i32 {\n"
        "        if true { 10 } else { 20 }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("trailing_if_in_try_catch_ok.f", source);
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

static void test_try_stmt_trailing_if_in_catch_not_converted(void) {
    /* A try STATEMENT (not expression) with a trailing if/return in the
     * catch body must NOT be converted to if-expression.  The if is used
     * for control flow, not value production. */
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func run(): i32 {\n"
        "    try parse() catch ex: i32 {\n"
        "        if true { return 10; } else { return 20; }\n"
        "    };\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_stmt_trailing_if_ok.f", source);
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

static void test_match_stmt_trailing_if_in_branch_not_converted(void) {
    /* A match STATEMENT (not expression) with a trailing if/return
     * in a branch body must NOT be converted to if-expression. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let v: i32 = 1;\n"
        "    match v {\n"
        "        1 {\n"
        "            if true { return 10; } else { return 20; }\n"
        "        }\n"
        "        else { return 0; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_stmt_trailing_if_ok.f", source);
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

static void test_break_directly_in_try_expr_catch_block_is_rejected(void) {
    /* break directly inside a catch block of a try expression is invalid
     * because the expression must produce a value on every path. */
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func run() {\n"
        "    while (true) {\n"
        "        let x: i32 = try parse() catch ex: i32 { break; 0 };\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_in_try_catch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "break_in_try_catch_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message,
                  "'break' cannot appear directly inside a catch block") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_continue_directly_in_try_expr_catch_block_is_rejected(void) {
    /* continue directly inside a catch block of a try expression is invalid
     * for the same reason: the expression must produce a value. */
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func run() {\n"
        "    while (true) {\n"
        "        let x: i32 = try parse() catch ex: i32 { continue; 0 };\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("continue_in_try_catch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "continue_in_try_catch_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message,
                  "'continue' cannot appear directly inside a catch block") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_break_inside_loop_inside_try_expr_catch_block_is_accepted(void) {
    /* A loop nested inside a catch block of a try expression may contain
     * break/continue normally because those target the inner loop. */
    const char *source =
        "module demo.main;\n"
        "func parse(): bool { return true; }\n"
        "func run() {\n"
        "    let x: bool = try parse() catch ex: bool {\n"
        "        while (true) {\n"
        "            break;\n"
        "        }\n"
        "        true\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("break_in_loop_in_try_catch_ok.f", source);
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
        "module demo.main;\n"
        "func run(p: int*) {\n"
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

static void test_throw_rejects_abi_type_value(void) {
    const char *source =
        "module demo.main;\n"
        "@abi type Handle { let id: int; }\n"
        "func run(h: Handle) {\n"
        "    throw h;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("throw_abi_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "throw_abi_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not throwable") != NULL);
    ASSERT(strstr(errors[0].message, "@abi types are ABI-bound") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_throw_accepts_string_and_managed_type(void) {
    const char *source =
        "module demo.main;\n"
        "type Err { let message: string; }\n"
        "func run_string() {\n"
        "    throw \"boom\";\n"
        "}\n"
        "func run_managed() {\n"
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

static void test_catch_unknown_allows_rethrow_only(void) {
    const char *source =
        "module demo.main;\n"
        "func parse(): int { return 1; }\n"
        "func run() {\n"
        "    try parse() catch ex: unknown { throw ex; };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("catch_unknown_rethrow_ok.f", source);
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

static void test_catch_unknown_rejects_value_use(void) {
    const char *source =
        "module demo.main;\n"
        "func parse(): int { return 1; }\n"
        "func run() {\n"
        "    try parse() catch ex: unknown { ex.message; };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("catch_unknown_value_use_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "catch_unknown_value_use_error.f") == 0);
    ASSERT(strstr(errors[0].message, "unknown catch value 'ex' can only be used") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_try_expression_catch_result_can_use_bound_value(void) {
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func id(value: i32): i32 { return value; }\n"
        "func run(): i32 {\n"
        "    let value = try parse() catch ex: i32 { id(ex); };\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_catch_bound_value_result_ok.f", source);
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

static void test_try_without_catch_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func run(): i32 {\n"
        "    let value = try parse();\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "try_without_catch_rejected.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_try_catch_statement_allows_empty_catch(void) {
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { throw 1; return 0; }\n"
        "func run() {\n"
        "    try parse() catch ex: i32 {\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_catch_statement_empty_ok.f", source);
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

static void test_try_expression_rejects_bound_value_result_mismatch(void) {
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { return 1; }\n"
        "func label(value: i32): string { return \"x\"; }\n"
        "func run(): i32 {\n"
        "    let value = try parse() catch ex: i32 { label(ex); };\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_catch_bound_value_result_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "try_catch_bound_value_result_error.f") == 0);
    ASSERT(strstr(errors[0].message, "catch clause result type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unknown_type_is_only_valid_in_catch_clause(void) {
    static const char *const cases[] = {
        "module demo.main;\nfunc run(x: unknown) {}\n",
        "module demo.main;\nfunc run(): unknown { return 1; }\n",
        "module demo.main;\ntype Box { let value: unknown; }\n",
        "module demo.main;\nfunc run() { let value: unknown = 1; }\n"
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        FengProgram *program = parse_program_or_die("unknown_type_position_error.f", cases[index]);
        const FengProgram *programs[] = {program};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
        ASSERT(error_count >= 1U);
        ASSERT(strcmp(errors[0].path, "unknown_type_position_error.f") == 0);
        ASSERT(strstr(errors[0].message, "type 'unknown' is only valid as a catch clause type") != NULL);

        feng_semantic_errors_free(errors, error_count);
        feng_program_free(program);
    }
}

static void test_throw_rejects_callable_values(void) {
    /* fn values, lambdas, member methods, and callable-form specs are not throwable. */
    static const char *const cases[] = {
        "module demo.main;\nfunc side() {}\nfunc run() { throw side; }\n",
        "module demo.main;\nfunc run() { throw () { }; }\n",
        "module demo.main;\ntype Box { func ping() {} }\nfunc run(box: Box) { throw box.ping; }\n",
        "module demo.main;\nspec Callback(): void;\nfunc run(cb: Callback) { throw cb; }\n"
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        FengProgram *program = parse_program_or_die("throw_non_exception_value_error.f", cases[index]);
        const FengProgram *programs[] = {program};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
        ASSERT(error_count >= 1U);
        ASSERT(strcmp(errors[0].path, "throw_non_exception_value_error.f") == 0);
        ASSERT(strstr(errors[0].message, "is not throwable") != NULL);

        feng_semantic_errors_free(errors, error_count);
        feng_program_free(program);
    }
}

static void test_throw_allows_spec_values(void) {
    /* spec fat values are now throwable: codegen extracts .subject and reads
     * the concrete descriptor from FengManagedHeader at throw time. */
    const char *source =
        "module demo.main;\n"
        "spec Named { func greet(): string; }\n"
        "type Foo: Named { let x: i32; func greet(): string { return \"ok\"; } }\n"
        "func run(v: Named) { throw v; }\n";
    FengProgram *program = parse_program_or_die("throw_spec_value_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_catch_rejects_non_exception_types(void) {
    static const char *const cases[] = {
        "module demo.main;\nspec Named { var name: string; }\nfunc parse(): int { return 1; }\nfunc run() { try parse() catch ex: Named { throw \"x\"; }; }\n",
        "module demo.main;\nspec Callback(): void;\nfunc parse(): int { return 1; }\nfunc run() { try parse() catch ex: Callback { throw \"x\"; }; }\n",
        "module demo.main;\nfunc parse(): int { return 1; }\nfunc run() { try parse() catch ex: int* { throw \"x\"; }; }\n"
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        FengProgram *program = parse_program_or_die("catch_non_exception_type_error.f", cases[index]);
        const FengProgram *programs[] = {program};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
        ASSERT(error_count >= 1U);
        ASSERT(strcmp(errors[0].path, "catch_non_exception_type_error.f") == 0);
        ASSERT(strstr(errors[0].message, "catch type") != NULL);
        ASSERT(strstr(errors[0].message, "is not catchable") != NULL);

        feng_semantic_errors_free(errors, error_count);
        feng_program_free(program);
    }
}

static void test_catch_without_binding_accepts_anonymous_clause(void) {
    /* catch { } (anonymous) is valid as statement or expression catch-all */
    static const char *const cases[] = {
        /* statement: anonymous catch-all */
        "module demo.main;\nfunc parse(): i32 { throw 1; return 0; }\nfunc run() { try parse() catch { }; }\n",
        /* statement: anonymous catch-all after specific catch */
        "module demo.main;\nfunc parse(): i32 { throw 1; return 0; }\nfunc run() { try parse() catch ex: i32 { } catch { }; }\n",
        /* expression: anonymous catch-all producing value */
        "module demo.main;\nfunc parse(): i32 { throw 1; return 0; }\nfunc run(): i32 { return try parse() catch { 0 }; }\n"
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        FengProgram *program = parse_program_or_die("catch_anon_ok.f", cases[index]);
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
}

static void test_catch_anonymous_must_be_last_clause(void) {
    /* anonymous catch { } must be the last catch clause */
    const char *source =
        "module demo.main;\n"
        "func parse(): i32 { throw 1; return 0; }\n"
        "func run() { try parse() catch { } catch ex: i32 { }; }\n";
    FengProgram *program = parse_program_or_die("catch_anon_not_last_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "catch_anon_not_last_error.f") == 0);
    ASSERT(strstr(errors[0].message, "must be the last catch clause") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_top_level_function_auto_infers_return_type_for_forward_call(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let value: int = add(1, 2);\n"
        "}\n"
        "func add(a: int, b: int) {\n"
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
        "module demo.main;\n"
        "func pick(flag: bool) {\n"
        "    if flag {\n"
        "        return 1;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "type Counter {\n"
        "    func value() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n"
        "func run(counter: Counter) {\n"
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
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func run(): int {\n"
        "    return base.value();\n"
        "}\n";
    const char *base_source =
        "open module demo.base;\n"
        "open func value() {\n"
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

static void test_omitted_return_function_rejects_lambda_signature_inference(void) {
    const char *source =
        "module demo.main;\n"
        "spec IntToInt(x: int): int;\n"
        "func make() {\n"
        "    return (x: int) -> x * 2;\n"
        "}\n"
        "func run(): int {\n"
        "    let callable: IntToInt = make();\n"
        "    return callable(4);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("auto_return_lambda_signature_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "auto_return_lambda_signature_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "explicit callable-form spec target type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_explicit_callable_return_accepts_lambda(void) {
    const char *source =
        "module demo.main;\n"
        "spec IntAdder(x: int, y: int): int;\n"
        "func make(): IntAdder {\n"
        "    return (x: int, y: int) -> x + y;\n"
        "}\n"
        "func run(): int {\n"
        "    let add: IntAdder = make();\n"
        "    return add(1, 2);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("explicit_callable_return_lambda_ok.f", source);
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
        "module demo.main;\n"
        "spec IntToInt(x: int): int;\n"
        "func pick(x: int) {\n"
        "    return x;\n"
        "}\n"
        "func run(): int {\n"
        "    let callable: IntToInt = pick;\n"
        "    return callable(4);\n"
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
        "module demo.main;\n"
        "func value(): int {\n"
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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "does not match expected type '%s'", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_expression_rejects_non_constant_label(void) {
    const char *source =
        "module demo.main;\n"
        "func run(value: int, other: int): int {\n"
        "    let pivot = other + 1;\n"
        "    return match value {\n"
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
        "module demo.main;\n"
        "func run(value: int): int {\n"
        "    return match value {\n"
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
        "module demo.main;\n"
        "func run(value: int): int {\n"
        "    return match value {\n"
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

static void test_untyped_lambda_binding_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let callable = (x: int) -> x * 2;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("untyped_lambda_binding_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "untyped_lambda_binding_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "explicit callable-form spec target type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_untyped_lambda_binding_cannot_later_match_named_function_type(void) {
    const char *source =
        "module demo.main;\n"
        "spec IntToInt(x: int): int;\n"
        "func run(): int {\n"
        "    let callable = (x: int) -> x * 2;\n"
        "    let typed: IntToInt = callable;\n"
        "    return typed(3);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("untyped_lambda_binding_function_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "untyped_lambda_binding_function_type_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "explicit callable-form spec target type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_module_visibility_conflict(void) {
    const char *source_a = "open module demo.main;\n";
    const char *source_b = "module demo.main;\n";
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run(): int {\n"
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run(): int {\n"
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

static void test_top_level_binding_inferred_type_is_used_by_identifier(void) {
    const char *source =
        "module demo.main;\n"
        "let TEST_NAME = \"hello_world\";\n"
        "func run(): string {\n"
        "    return \"Running test: \" + TEST_NAME;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("top_binding_inferred_identifier_ok.f", source);
    const FengProgram *programs[] = {program};
    const FengDecl *binding_decl = program->declarations[0];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengSemanticTypeFact *fact = NULL;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    fact = feng_semantic_lookup_type_fact(analysis, &binding_decl->as.binding);
    ASSERT(fact != NULL);
    ASSERT(fact->kind == FENG_SEMANTIC_TYPE_FACT_BUILTIN);
    ASSERT(fact->builtin_name.length == strlen("string"));
    ASSERT(memcmp(fact->builtin_name.data, "string", strlen("string")) == 0);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_top_level_function_call_reports_type_mismatch(void) {
    const char *source =
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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

static void test_generic_extern_call_accepts_wrapped_array_inference(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "extern func feng_array_get_length<T>(value: T[]): i64;\n"
        "func run(values: int[]): i64 {\n"
        "    return feng_array_get_length(values);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_extern_wrapped_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_extern_call_accepts_bare_type_param_inference(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "extern func same<T>(left: T, right: T): bool;\n"
        "func run(left: int, right: int): bool {\n"
        "    return same(left, right);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_extern_bare_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_extern_call_accepts_bare_type_param_return(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "extern func __test_value_identity<T>(value: T): T;\n"
        "func run(value: int): int {\n"
        "    return __test_value_identity(value);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_extern_bare_return_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_method_accepts_fit_type_param_argument(void) {
    const char *source =
        "module demo.main;\n"
        "fit T[!] {\n"
        "    func pick(value: T): i64 {\n"
        "        return (i64)1;\n"
        "    }\n"
        "}\n"
        "func run(values: int[!]): i64 {\n"
        "    return values.pick(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_method_type_param_arg_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_extern_call_rejects_conflicting_wrapped_array_inference(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "extern func same<T>(left: T[], right: T[]): i64;\n"
        "func run(left: int[], right: string[]): i64 {\n"
        "    return same(left, right);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_extern_wrapped_conflict.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "top-level function 'same' has no overload accepting 2 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_non_extern_call_does_not_expand_wrapped_array_inference(void) {
    const char *source =
        "module demo.main;\n"
        "func same<T>(values: T[]): T[] {\n"
        "    return values;\n"
        "}\n"
        "func run(values: int[]): int[] {\n"
        "    return same(values);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_non_extern_wrapped_scope.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "top-level function 'same' has no overload accepting 1 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_imported_function_call_selects_overload_by_literal_type(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "open func pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func run(): int {\n"
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

static void test_imported_generic_extern_call_accepts_wrapped_array_inference(void) {
    const char *base_source =
        "open module demo.base;\n"
        "@runtime\n"
        "open extern func feng_array_get_length<T>(value: T[]): i64;\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func run(values: int[]): i64 {\n"
        "    return base.feng_array_get_length(values);\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("imported_generic_extern_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("imported_generic_extern_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_alias_function_call_selects_overload_by_literal_type(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "open func pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func run(): int {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    func pick(a: int): int {\n"
        "        return a;\n"
        "    }\n"
        "    func pick(a: string): string {\n"
        "        return a;\n"
        "    }\n"
        "}\n"
        "func run(user: User): int {\n"
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
        "module demo.main;\n"
        "spec Picker(a: int): int;\n"
        "func run(): int {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "open module demo.base;\n"
        "open type User {\n"
        "    seal func secret(): int {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func run(user: User): int {\n"
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
        "module demo.main;\n"
        "spec IntPicker(a: int): int;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run(): int {\n"
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
        "module demo.main;\n"
        "spec IntPicker(a: int): int;\n"
        "func apply(picker: IntPicker): int {\n"
        "    return picker(1);\n"
        "}\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run(): int {\n"
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
        "module demo.main;\n"
        "spec IntPicker(a: int): int;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func make(): IntPicker {\n"
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "spec M0(): void;\n"
        "type User {\n"
        "    func say() {}\n"
        "    func say(msg: string) {}\n"
        "}\n"
        "func run(user: User) {\n"
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
        "module demo.main;\n"
        "spec M0(): void;\n"
        "type User {\n"
        "    func say() {}\n"
        "    func say(msg: string) {}\n"
        "}\n"
        "func apply(action: M0) {\n"
        "    action();\n"
        "}\n"
        "func run(user: User) {\n"
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
        "module demo.main;\n"
        "spec M0(): void;\n"
        "type User {\n"
        "    func say() {}\n"
        "    func say(msg: string) {}\n"
        "}\n"
        "func make(user: User): M0 {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    func say() {}\n"
        "    func say(msg: string) {}\n"
        "}\n"
        "func run(user: User) {\n"
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
        "module demo.main;\n"
        "spec BoolPicker(a: bool): bool;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "spec BoolPicker(a: bool): bool;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func pick(a: string): string {\n"
        "    return a;\n"
        "}\n"
        "func make(): BoolPicker {\n"
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "spec BoolMaker(a: int): bool;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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
        "open module demo.base;\n"
        "open func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "open func pick(a: string): string {\n"
        "    return a;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "spec BoolPicker(a: bool): bool;\n"
        "func accept(picker: BoolPicker) {}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "spec BoolAction(flag: bool): bool;\n"
        "type User {\n"
        "    func say() {}\n"
        "    func say(msg: string) {}\n"
        "}\n"
        "func accept(action: BoolAction) {}\n"
        "func run(user: User) {\n"
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
        "module demo.main;\n"
        "spec IntPicker(a: int): int;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func pick(a: int): int {\n"
        "    return a;\n"
        "}\n"
        "func run(): bool {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    func pick(a: int): int {\n"
        "        return a;\n"
        "    }\n"
        "}\n"
        "func run(user: User) {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    var flag: bool;\n"
        "    func update() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    var flag: bool;\n"
        "}\n"
        "func make(): User {\n"
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

static void test_type_field_inferred_initializer_member_access_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "type UserType {\n"
        "    let id: int = 7;\n"
        "}\n"
        "type User {\n"
        "    let id: int = 0;\n"
        "    let x: UserType;\n"
        "    let y = UserType();\n"
        "    let z = UserType {};\n"
        "}\n"
        "func total(): int {\n"
        "    let user = User();\n"
        "    return user.id + user.x.id + user.y.id + user.z.id;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("type_field_inferred_member_access.f", source);
    const FengProgram *programs[] = {program};
    const FengDecl *user_type = program->declarations[1];
    const FengTypeMember *field_y = user_type->as.type_decl.members[2];
    const FengTypeMember *field_z = user_type->as.type_decl.members[3];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengSemanticTypeFact *fact_y = NULL;
    const FengSemanticTypeFact *fact_z = NULL;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    fact_y = feng_semantic_lookup_type_fact(analysis, field_y);
    fact_z = feng_semantic_lookup_type_fact(analysis, field_z);
    ASSERT(fact_y != NULL);
    ASSERT(fact_y->kind == FENG_SEMANTIC_TYPE_FACT_DECL);
    ASSERT(fact_y->type_decl == program->declarations[0]);
    ASSERT(fact_z != NULL);
    ASSERT(fact_z->kind == FENG_SEMANTIC_TYPE_FACT_DECL);
    ASSERT(fact_z->type_decl == program->declarations[0]);

    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_local_let_assignment_rejects_non_writable_target(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run(total: int) {\n"
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
        "module demo.main;\n"
        "func run(var total: int) {\n"
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
        "module demo.main;\n"
        "let count: int = 0;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int = 0;\n"
        "}\n"
        "func update(var user: User) {\n"
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
        "open module demo.base;\n"
        "open let count: int = 0;\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
        "    var items: int[!] = [1, 2, 3];\n"
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
        "module demo.main;\n"
        "func run() {\n"
        "    var items: int[!] = [1, 2, 3];\n"
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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "does not match expected type '%s'", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_compound_assignment_accepts_numeric_and_bitwise_targets(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var rem: double = 7.8;\n"
        "    rem %= 3.2;\n"
        "    var mask: i32 = 8;\n"
        "    mask >>= 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("compound_assign_ok.f", source);
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

static void test_compound_assignment_rejects_string_plus_equal(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var name: string = \"a\";\n"
        "    name += \"b\";\n"
        "}\n";
    FengProgram *program = parse_program_or_die("compound_assign_string_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "compound_assign_string_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "compound assignment operator '+=' requires operands of the same numeric type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_compound_assignment_literal_adapts_to_target_type(void) {
    /* Covers numeric (+=, -=, *=, /=, %=) and bitwise (&=, |=, ^=, <<=, >>=)
     * compound operators with integer and float targets.  Each literal adapts
     * to the left-hand side scalar type — without adaptation the literal
     * defaults to int (i64 on 64-bit platforms) and the type check fails. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var n: i32 = 5;\n"
        "    n += 1;\n"
        "    n -= 2;\n"
        "    n *= 3;\n"
        "    n /= 4;\n"
        "    n %= 5;\n"
        "    var mask: i32 = 0;\n"
        "    mask &= 1;\n"
        "    mask |= 2;\n"
        "    mask ^= 4;\n"
        "    mask <<= 1;\n"
        "    mask >>= 1;\n"
        "    var x: u8 = 10;\n"
        "    x += 1;\n"
        "    x *= 255;\n"
        "    var f: f32 = 1.0;\n"
        "    f += 0.5;\n"
        "    f -= 0.25;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("compound_assign_adapt_ok.f", source);
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

static void test_compound_assignment_literal_out_of_range_reports_type_mismatch(void) {
    /* 256 does not fit u8, so adaptation fails and the literal keeps its
     * default int (i64) type — the type equality check then rejects it. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var x: u8 = 10;\n"
        "    x += 256;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("compound_assign_adapt_range.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "compound_assign_adapt_range.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "compound assignment operator '+=' requires operands of the same numeric type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_inferred_array_literal_binding_rejects_index_write_without_writable_layer(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var items = [1, 2, 3];\n"
        "    items[0] = 4;\n"
        "    let first: int = items[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("inferred_array_index_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "inferred_array_index_ok.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_inferred_array_literal_binding_rejects_non_matching_index_assignment(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var items = [1, 2, 3];\n"
        "    items[0] = true;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("inferred_array_index_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "inferred_array_index_type_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_inferred_array_literal_rejects_mixed_element_types(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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

static void test_inferred_nested_array_literal_rejects_nested_index_write_without_writable_layer(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var matrix = [[1, 2], [3, 4]];\n"
        "    matrix[0][1] = 5;\n"
        "    let value: int = matrix[1][0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("nested_array_index_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "nested_array_index_ok.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "is not writable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_empty_array_literal_binding_requires_explicit_target_type(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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

/* `T[!]` and `T[]` are distinct types; binding a writable literal to a
 * readonly slot without an explicit cast is rejected per docs §5. */
static void test_array_literal_matches_readonly_target(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var items: int[] = [1, 2, 3];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("writable_literal_to_readonly_error.f", source);
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

/* Cast may STRIP `!` (writable → readonly): allowed. */
static void test_cast_strips_writable_array_to_readonly(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var src: int[!] = [1, 2, 3];\n"
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
        "module demo.main;\n"
        "func run() {\n"
        "    var src: int[] = [1, 2, 3];\n"
        "    let view: int[!] = (int[!])src;\n"
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

/* Empty `[]` requires an explicit writable target type when binding to `T[!]`. */
static void test_empty_writable_array_literal_requires_writable_target(void) {
    const char *ok_source =
        "module demo.main;\n"
        "func run() {\n"
        "    var items: int[!] = [];\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from 'bool' to '%s' is not allowed", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_bool(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from 'string' to '%s' is not allowed", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_array_to_numeric(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from '%s[]' to '%s' is not allowed", int_canonical, int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_string(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let s: string = (string)1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_string_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from '%s' to 'string' is not allowed", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_numeric_to_array(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let xs: int[] = (int[])1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_array_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from '%s' to '%s[]' is not allowed", int_canonical, int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_string_to_bool(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "type Point { var x: int; var y: int; }\n"
        "func run() {\n"
        "    let p: Point = (Point)1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_numeric_to_object_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from '%s' to 'Point' is not allowed", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_rejects_object_to_numeric(void) {
    const char *source =
        "module demo.main;\n"
        "type Point { var x: int; var y: int; }\n"
        "func run(p: Point) {\n"
        "    let v: int = (int)p;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("cast_object_to_numeric_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "cast from 'Point' to '%s' is not allowed", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_cast_same_type_passes(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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

static void test_non_generic_array_new_colon_dimension_accepts_expected_target(void) {
    const char *source =
        "module demo.main;\n"
        "func run(n: int) {\n"
        "    var items: int[!] = int[:n];\n"
        "    items[0] = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_new_colon_dim_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_non_generic_array_new_legacy_bracket_syntax_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let items: int[!] = int[3];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_new_legacy_syntax_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count > 0U);
    ASSERT(errors[0].message != NULL);
    ASSERT(strstr(errors[0].message, "undefined") != NULL ||
           strstr(errors[0].message, "index target") != NULL);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_array_new_colon_dimension_accepts_expected_target(void) {
    const char *source =
        "module demo.main;\n"
        "type Pair<A, B> {\n"
        "    var left: A;\n"
        "    var right: B;\n"
        "}\n"
        "func run(n: int) {\n"
        "    var pairs: Pair<int, int>[!] = Pair<int, int>[:n];\n"
        "    pairs[0].left = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_array_new_colon_dim_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_array_new_legacy_bracket_syntax_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Pair<A, B> {\n"
        "    var left: A;\n"
        "    var right: B;\n"
        "}\n"
        "func run() {\n"
        "    let pairs: Pair<int, int>[!] = Pair<int, int>[3];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_array_new_legacy_syntax_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count > 0U);
    ASSERT(errors[0].message != NULL);
    ASSERT(strstr(errors[0].message, "explicit generic target") != NULL ||
           strstr(errors[0].message, "index target") != NULL);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_index_access_on_uppercase_local_name_remains_index_expression(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    var Data: int[] = [1, 2, 3];\n"
        "    let value: int = Data[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("index_upper_local_ok.f", source);
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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

static void test_unary_address_of_rejects_returned_scalar_binding(void) {
    const char *source =
        "module demo.main;\n"
        "func run(value: i32): i32* {\n"
        "    return &value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_scalar_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_scalar_return_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "borrowed data pointer formed by '&'") != NULL);
    ASSERT(strstr(errors[0].message,
                  "cannot be returned") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_returned_array_value(void) {
    const char *source =
        "module demo.main;\n"
        "func run(values: int[]): int* {\n"
        "    return &values;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_array_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_array_return_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "borrowed data pointer formed by '&'") != NULL);
    ASSERT(strstr(errors[0].message,
                  "cannot be returned") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_returned_string_value(void) {
    const char *source =
        "module demo.main;\n"
        "func run(text: string): string* {\n"
        "    return &text;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_string_return_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_string_return_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message,
                  "borrowed data pointer formed by '&'") != NULL);
    ASSERT(strstr(errors[0].message,
                  "cannot be returned") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_allows_extern_call_borrowed_data_pointer(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"c_use_i32_ptr\")\n"
        "extern func c_use_i32_ptr(p: i32*): void;\n"
        "@cdecl(\"c_use_array_ptr\")\n"
        "extern func c_use_array_ptr(p: int*): void;\n"
        "@cdecl(\"c_use_text_ptr\")\n"
        "extern func c_use_text_ptr(p: string*): void;\n"
        "func run(value: i32, values: int[], text: string) {\n"
        "    let p1: i32* = &value;\n"
        "    let p2: int* = &values;\n"
        "    let p3: string* = &text;\n"
        "    c_use_i32_ptr(p1);\n"
        "    c_use_array_ptr(p2);\n"
        "    c_use_text_ptr(p3);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_extern_call_ok.f", source);
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

static void test_unary_address_of_allows_fielded_abi_type_pointer_binding(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func use_point_ptr(p: Point*): void;\n"
        "func run(point: Point) {\n"
        "    let ptr: Point* = &point;\n"
        "    use_point_ptr(ptr);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_fielded_abi_type_ok.f", source);
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

static void test_unary_address_of_rejects_fieldless_abi_type_pointer_binding(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "func run(handle: Handle) {\n"
        "    let ptr: Handle* = &handle;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_fieldless_abi_type_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_fieldless_abi_type_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "unary operator '&' requires an ABI-compatible scalar or @abi value") != NULL);
    ASSERT(strstr(errors[0].message, "Handle") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* @value type without @abi: & should be rejected (not ABI-compatible). */
static void test_unary_address_of_rejects_value_type_without_abi(void) {
    const char *source =
        "module demo.main;\n"
        "@value\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "func run() {\n"
        "    var p: Point;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    let ptr: Point* = &p;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_value_type_no_abi_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_value_type_no_abi_error.f") == 0);
    ASSERT(errors[0].token.line == 11U);
    ASSERT(strstr(errors[0].message,
                  "unary operator '&' requires an ABI-compatible scalar or @abi value") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* @value @abi type: & should be accepted (ABI-compatible + value semantics). */
static void test_unary_address_of_allows_value_abi_type(void) {
    const char *source =
        "module demo.main;\n"
        "@value @abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "func run() {\n"
        "    var p: Point;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    let ptr: Point* = &p;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_value_abi_type_ok.f", source);
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

static void test_unary_address_of_rejects_string_to_byte_pointer_binding(void) {
    const char *source =
        "module demo.main;\n"
        "func run(text: string) {\n"
        "    let p: byte* = &text;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_string_to_byte_pointer_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_string_to_byte_pointer_error.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "u8*") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_non_extern_forwarding_via_assignment_alias(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"c_get_i32_ptr\")\n"
        "extern func c_get_i32_ptr(): i32*;\n"
        "func forward(p: i32*) {\n"
        "}\n"
        "func run(value: i32) {\n"
        "    var p: i32* = c_get_i32_ptr();\n"
        "    p = &value;\n"
        "    forward(p);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_non_extern_forward_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_non_extern_forward_error.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message,
                  "cannot be passed to non-extern callable 'forward'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_object_field_storage(void) {
    const char *source =
        "module demo.main;\n"
        "type Holder {\n"
        "    var ptr: i32*;\n"
        "}\n"
        "func run(value: i32) {\n"
        "    let p: i32* = &value;\n"
        "    let holder = Holder { ptr: p };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_object_field_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_object_field_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message,
                  "object literal field 'ptr' cannot store borrowed data pointer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_member_assignment_storage(void) {
    const char *source =
        "module demo.main;\n"
        "type Holder {\n"
        "    var ptr: i32*;\n"
        "}\n"
        "@cdecl(\"c_get_i32_ptr\")\n"
        "extern func c_get_i32_ptr(): i32*;\n"
        "func run(value: i32) {\n"
        "    let holder = Holder { ptr: c_get_i32_ptr() };\n"
        "    holder.ptr = &value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_member_assignment_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_member_assignment_error.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message,
                  "assignment target 'holder.ptr' cannot store borrowed data pointer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_accepts_top_level_abi_function_pointer_target(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@abi\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "func run() {\n"
        "    let cb: Cmp* = &cmp;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_abi_fn_ptr_ok.f", source);
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

static void test_unary_address_of_requires_explicit_function_pointer_target(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@abi\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "func run() {\n"
        "    let cb = &cmp;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_missing_target_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_missing_target_error.f") == 0);
    ASSERT(errors[0].token.line == 9U);
    ASSERT(strstr(errors[0].message,
                  "requires an explicit target Foo* type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_plain_function_pointer_target(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "func run() {\n"
        "    let cb: Cmp* = &cmp;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_plain_fn_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_plain_fn_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message,
                  "does not match expected ABI function pointer type 'Cmp*'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_method_pointer_target(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "type Box {\n"
        "    func cmp(a: int, b: int): int {\n"
        "        return a - b;\n"
        "    }\n"
        "}\n"
        "func run(box: Box) {\n"
        "    let cb: Cmp* = &box.cmp;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_method_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_method_error.f") == 0);
    ASSERT(errors[0].token.line == 10U);
    ASSERT(strstr(errors[0].message,
                  "cannot form expected ABI function pointer type 'Cmp*'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_local_lambda_pointer_target(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "spec LocalCmp(a: int, b: int): int;\n"
        "func run() {\n"
        "    let local: LocalCmp = (a: int, b: int) -> a - b;\n"
        "    let cb: Cmp* = &local;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_lambda_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "unary_address_of_lambda_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message,
                  "cannot form expected ABI function pointer type 'Cmp*'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_function_pointer_binding_is_not_directly_callable(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@cdecl(\"c_load_cmp\")\n"
        "extern func c_load_cmp(): Cmp*;\n"
        "func run() {\n"
        "    let cb: Cmp* = c_load_cmp();\n"
        "    cb(1, 2);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_pointer_direct_call_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "function_pointer_direct_call_error.f") == 0);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message,
                  "expression 'cb' is not callable") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_function_pointer_semantic_allows_field_param_and_return_flow(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@abi\n"
        "type Holder {\n"
        "    var cb: Cmp*;\n"
        "}\n"
        "@abi\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func c_register_cmp(cb: Cmp*): void;\n"
        "@cdecl(\"c\")\n"
        "extern func c_load_cmp(): Cmp*;\n"
        "func run() {\n"
        "    let cb: Cmp* = &cmp;\n"
        "    let holder: Holder = Holder{cb: cb};\n"
        "    c_register_cmp(holder.cb);\n"
        "    let other: Cmp* = c_load_cmp();\n"
        "    let copy: Holder = Holder{cb: other};\n"
        "}\n";
    FengProgram *program = parse_program_or_die("function_pointer_flow_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_unary_address_of_rejects_bound_method_pointer_target(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "type Box {\n"
        "    func cmp(a: int, b: int): int {\n"
        "        return a - b;\n"
        "    }\n"
        "}\n"
        "func run(box: Box) {\n"
        "    let method = box.cmp;\n"
        "    let cb: Cmp* = &method;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("unary_address_of_bound_method_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 2U);
    ASSERT(strcmp(errors[1].path, "unary_address_of_bound_method_error.f") == 0);
    ASSERT(errors[1].token.line == 11U);
    ASSERT(strstr(errors[1].message,
                  "cannot form expected ABI function pointer type 'Cmp*'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_binary_equality_accepts_data_pointer_operands(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: int = 1;\n"
        "    let p: int* = &x;\n"
        "    let q: int* = p;\n"
        "    let same: bool = p == q;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("pointer_equality_data_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_binary_equality_accepts_function_pointer_operands(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@abi\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "func run() {\n"
        "    let fp: Cmp* = &cmp;\n"
        "    let fq: Cmp* = fp;\n"
        "    let same: bool = fp != fq;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("pointer_equality_function_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_binary_equality_rejects_mismatched_pointer_operands(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: int = 1;\n"
        "    let p: int* = &x;\n"
        "    let bytes: byte[] = [1];\n"
        "    let q: byte* = &bytes;\n"
        "    let same: bool = p == q;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("pointer_equality_mismatch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "pointer_equality_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '==' requires operands of the same type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_binary_plus_rejects_non_matching_operands(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run(a: i32, b: i32) {\n"
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
        "module demo.main;\n"
        "func run(a: i32, b: i64) {\n"
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
        "module demo.main;\n"
        "func run(a: f32, b: f32) {\n"
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
        "module demo.main;\n"
        "func run(x: f32) {\n"
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
        "module demo.main;\n"
        "func run(a: i32) {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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

static void test_const_fold_float_modulo_by_zero_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: double = 1.0 % 0.0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_float_mod_zero.f", source);
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

static void test_float_modulo_expression_is_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let rem: double = 7.8 % 3.2;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("float_mod_ok.f", source);
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

static void test_const_fold_i64_overflow_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run(a: i32) {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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

static void test_const_fold_immutable_local_binding_requires_explicit_cast(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let n: int = 100;\n"
        "    let x: u8 = n + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("const_fold_let_prop.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_const_fold_does_not_propagate_var_binding(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "func run() {\n"
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
        "module demo.main;\n"
        "import demo.base;\n"
        "func main() {}\n";
    FengProgram *program = parse_program_or_die("missing_use_target.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strstr(errors[0].message, "import target module 'demo.base' was not found") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_imported_type_conflicts_with_local_type(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open type User {}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "type User {}\n"
        "let x: User = User{};\n";
    FengProgram *base_program = parse_program_or_die("base_type.f", base_source);
    FengProgram *main_program = parse_program_or_die("main_type_conflict.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "main_type_conflict.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "User") != NULL);
    ASSERT(strstr(errors[0].message, "ambiguous") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_imported_value_conflicts_with_local_value(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func load(): int {\n"
        "    return 0;\n"
        "}\n"
        "func main(): int {\n"
        "    return load();\n"
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
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "load") != NULL);
    ASSERT(strstr(errors[0].message, "ambiguous") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_imported_name_conflicts_between_modules(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func load(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a;\n"
        "import demo.b;\n"
        "func main(): int {\n"
        "    return load();\n"
        "}\n";
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
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "load") != NULL);
    ASSERT(strstr(errors[0].message, "ambiguous") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_import_short_names_do_not_leak_to_other_files(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *importing_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func from_import(): int {\n"
        "    return load();\n"
        "}\n";
    const char *sibling_source =
        "module demo.main;\n"
        "func from_sibling(): int {\n"
        "    return load();\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("file_scope_base.f", base_source);
    FengProgram *importing_program = parse_program_or_die("file_scope_importing.f", importing_source);
    FengProgram *sibling_program = parse_program_or_die("file_scope_sibling.f", sibling_source);
    const FengProgram *programs[] = {base_program, importing_program, sibling_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "file_scope_sibling.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strstr(errors[0].message, "undefined identifier 'load'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(importing_program);
    feng_program_free(sibling_program);
}

static void test_import_name_conflict_with_other_file_decl_does_not_error(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func helper(): int {\n"
        "    return 1;\n"
        "}\n"
        "open func test(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *importing_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func from_import(): int {\n"
        "    return helper();\n"
        "}\n";
    const char *sibling_source =
        "module demo.main;\n"
        "func test(): int {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("other_file_conflict_base.f", base_source);
    FengProgram *importing_program = parse_program_or_die("other_file_conflict_importing.f", importing_source);
    FengProgram *sibling_program = parse_program_or_die("other_file_conflict_sibling.f", sibling_source);
    const FengProgram *programs[] = {base_program, importing_program, sibling_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(base_program);
    feng_program_free(importing_program);
    feng_program_free(sibling_program);
}

static void test_alias_import_does_not_inject_short_names(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func load(): int {\n"
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

static void test_import_alias_conflicts_with_same_file_local_value(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func assert(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as helper;\n"
        "func run(): int {\n"
        "    return helper.assert();\n"
        "}\n"
        "let helper = \"\";\n";
    FengProgram *base_program = parse_program_or_die("alias_local_value_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("alias_local_value_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_local_value_main.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strcmp(errors[0].code, "AE0906") == 0);
    ASSERT(strstr(errors[0].message, "helper") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_import_alias_conflicts_with_other_file_local_value(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func assert(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *importing_source =
        "module demo.main;\n"
        "import demo.base as helper;\n"
        "func run(): int {\n"
        "    return helper.assert();\n"
        "}\n";
    const char *sibling_source =
        "module demo.main;\n"
        "let helper = \"\";\n";
    FengProgram *base_program = parse_program_or_die("alias_other_file_base.f", base_source);
    FengProgram *importing_program = parse_program_or_die("alias_other_file_main.f", importing_source);
    FengProgram *sibling_program = parse_program_or_die("alias_other_file_sibling.f", sibling_source);
    const FengProgram *programs[] = {base_program, importing_program, sibling_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_other_file_main.f") == 0);
    ASSERT(errors[0].token.line == 2U);
    ASSERT(strcmp(errors[0].code, "AE0906") == 0);
    ASSERT(strstr(errors[0].message, "current module") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(importing_program);
    feng_program_free(sibling_program);
}

static void test_import_alias_conflicts_with_imported_short_name(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func helper(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func store(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a;\n"
        "import demo.b as helper;\n"
        "func run(): int {\n"
        "    return helper.store();\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("alias_imported_name_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("alias_imported_name_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("alias_imported_name_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "alias_imported_name_main.f") == 0);
    ASSERT(errors[0].token.line == 3U);
    ASSERT(strcmp(errors[0].code, "AE0906") == 0);
    ASSERT(strstr(errors[0].message, "imported name already visible") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_import_vs_import(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func compute(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func compute(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a;\n"
        "import demo.b;\n"
        "func run(): int {\n"
        "    return compute();\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("lazy_ii_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("lazy_ii_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("lazy_ii_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "lazy_ii_main.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "compute") != NULL);
    ASSERT(strstr(errors[0].message, "ambiguous") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_import_vs_local_type(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open type Item {\n"
        "    open var id: i32;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "type Item {\n"
        "    open var name: string;\n"
        "}\n"
        "func create(): Item {\n"
        "    return Item{};\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("lazy_type_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("lazy_type_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "lazy_type_main.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "Item") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_import_vs_local_value(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open func process(): int {\n"
        "    return 10;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func process(): int {\n"
        "    return 20;\n"
        "}\n"
        "func run(): int {\n"
        "    return process();\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("lazy_val_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("lazy_val_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "lazy_val_main.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "process") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_unused_no_error(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func unused_func(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func unused_func(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a;\n"
        "import demo.b;\n"
        "func run(): int {\n"
        "    return 42;\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("lazy_unused_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("lazy_unused_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("lazy_unused_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_resolved_by_qualified_path(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func compute(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func compute(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a as a;\n"
        "import demo.b as b;\n"
        "func run(): int {\n"
        "    return a.compute() + b.compute();\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("lazy_qualified_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("lazy_qualified_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("lazy_qualified_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    bool ok = feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count);
    ASSERT(ok);
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_resolved_by_alias(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func compute(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func compute(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a as alpha;\n"
        "import demo.b;\n"
        "func run(): int {\n"
        "    return alpha.compute();\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("lazy_alias_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("lazy_alias_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("lazy_alias_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_import_vs_other_file_in_same_module(void) {
    const char *source_a =
        "module demo.core;\n"
        "func local_func(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "module demo.core;\n"
        "import demo.ext;\n"
        "func run(): int {\n"
        "    return local_func();\n"
        "}\n";
    const char *ext_source =
        "open module demo.ext;\n"
        "open func local_func(): int {\n"
        "    return 2;\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("lazy_same_module_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("lazy_same_module_b.f", source_b);
    FengProgram *ext_program = parse_program_or_die("lazy_same_module_ext.f", ext_source);
    const FengProgram *programs[] = {program_a, program_b, ext_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "lazy_same_module_b.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "local_func") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(ext_program);
}

static void test_lazy_ambiguity_spec_reference(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open spec Processor {\n"
        "    func process(): int;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open spec Processor {\n"
        "    func process(): int;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a;\n"
        "import demo.b;\n"
        "func run(p: Processor): int {\n"
        "    return p.process();\n"
        "}\n";
    FengProgram *program_a = parse_program_or_die("lazy_spec_a.f", source_a);
    FengProgram *program_b = parse_program_or_die("lazy_spec_b.f", source_b);
    FengProgram *main_program = parse_program_or_die("lazy_spec_main.f", main_source);
    const FengProgram *programs[] = {program_a, program_b, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 3U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "lazy_spec_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "Processor") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

static void test_lazy_ambiguity_cross_kind_import_func_vs_local_let(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open let config: int = 100;\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func config(): int {\n"
        "    return 200;\n"
        "}\n"
        "func run(): int {\n"
        "    return config;\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("lazy_cross_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("lazy_cross_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    bool ok = feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count);
    ASSERT(!ok);
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "lazy_cross_main.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "config") != NULL);
    ASSERT(strcmp(errors[0].path, "lazy_cross_main.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "config") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_duplicate_use_alias_in_same_file(void) {
    const char *source_a =
        "open module demo.a;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *source_b =
        "open module demo.b;\n"
        "open func store(): int {\n"
        "    return 2;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.a as tools;\n"
        "import demo.b as tools;\n"
        "func main() {}\n";
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
    ASSERT(strstr(errors[0].message, "duplicate import alias 'tools'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program_a);
    feng_program_free(program_b);
    feng_program_free(main_program);
}

typedef struct ExternalModuleQueryFixture {
    const char **segments;
    size_t segment_count;
    size_t call_count;
    /* A minimal pre-built module returned when the path matches. */
    FengSemanticModule match_module;
} ExternalModuleQueryFixture;

static const FengSemanticModule *external_module_query_get_module(
    const void *user,
    const FengSlice *segments,
    size_t segment_count) {
    ExternalModuleQueryFixture *fixture = (ExternalModuleQueryFixture *)user;
    size_t index;

    ASSERT(fixture != NULL);
    ++fixture->call_count;
    if (segment_count != fixture->segment_count) {
        return NULL;
    }
    for (index = 0U; index < segment_count; ++index) {
        const char *expected = fixture->segments[index];

        if (strlen(expected) != segments[index].length ||
            memcmp(expected, segments[index].data, segments[index].length) != 0) {
            return NULL;
        }
    }
    /* Return a pre-built module with no programs (just module existence). */
    fixture->match_module.segments = segments;
    fixture->match_module.segment_count = segment_count;
    fixture->match_module.visibility = FENG_VISIBILITY_PUBLIC;
    fixture->match_module.programs = NULL;
    fixture->match_module.program_count = 0U;
    fixture->match_module.program_capacity = 0U;
    fixture->match_module.origin = FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE;
    return &fixture->match_module;
}

static void test_unknown_use_module_rejected_without_import_query(void) {
    const char *source =
        "module demo.main;\n"
        "import vendor.math;\n"
        "func run() {}\n";
    FengProgram *program = parse_program_or_die("unknown_use_external.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "import target module 'vendor.math'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_external_use_module_accepted_via_import_query(void) {
    const char *source =
        "module demo.main;\n"
        "import vendor.math;\n"
        "func run() {}\n";
    const char *segments[] = {"vendor", "math"};
    ExternalModuleQueryFixture fixture = {segments, 2U, 0U, {0}};
    FengSemanticImportedModuleQuery query = {&fixture, external_module_query_get_module};
    FengSemanticAnalyzeOptions options = {FENG_COMPILE_TARGET_LIB, &query, sizeof(void *)};
    FengProgram *program = parse_program_or_die("known_use_external.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(fixture.call_count > 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_external_imported_function_argument_type_mismatch(void) {
    const char *external_source =
        "open module vendor.math;\n"
        "open func add(a: int, b: int): int {\n"
        "    return a + b;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.math as math;\n"
        "func run(): int {\n"
        "    return math.add(\"oops\", 1);\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_add.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_fn_mismatch_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "external_fn_mismatch_main.f") == 0);
    ASSERT(strstr(errors[0].message, "function 'math.add' has no overload accepting 2 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_function_argument_type_match(void) {
    const char *external_source =
        "open module vendor.math;\n"
        "open func add(a: int, b: int): int {\n"
        "    return a + b;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.math as math;\n"
        "func run(): int {\n"
        "    return math.add(1, 2);\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_add_ok.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_fn_ok_main.f", main_source);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_field_type_participates_in_typecheck(void) {
    const char *external_source =
        "open module vendor.model;\n"
        "open type User {\n"
        "    open let name: string;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.model as model;\n"
        "func project(user: model.User): int {\n"
        "    return user.name;\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_user.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_field_type_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "external_field_type_main.f") == 0);
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "does not match expected type '%s'", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_decl_bound_let_member_rejects_object_literal_rebind(void) {
    const char *external_source =
        "open module vendor.bound_model;\n"
        "open type User {\n"
        "    open let id: int = 1;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.bound_model as model;\n"
        "func make(): model.User {\n"
        "    return model.User { id: 2 };\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_bound_model.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_decl_bound_object_literal_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "external_decl_bound_object_literal_main.f") == 0);
    ASSERT(strstr(errors[0].message, "declaration initializer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_ctor_bound_let_member_rejects_object_literal_rebind(void) {
    const char *external_source =
        "open module vendor.ctor_bound_model;\n"
        "open type User {\n"
        "    open let id: int;\n"
        "    open func User(value: int) {\n"
        "        self.id = value;\n"
        "    }\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.ctor_bound_model as model;\n"
        "func make(): model.User {\n"
        "    return model.User(1) { id: 2 };\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_ctor_bound_model.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_ctor_bound_object_literal_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "external_ctor_bound_object_literal_main.f") == 0);
    ASSERT(strstr(errors[0].message, "already completed by constructor") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_static_members_are_visible(void) {
    const char *external_source =
        "open module vendor.static_model;\n"
        "open type Counter {\n"
        "    open static let seed: int = 1;\n"
        "    open static func make(): int {\n"
        "        return 2;\n"
        "    }\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.static_model as model;\n"
        "func run(): int {\n"
        "    return model.Counter.seed + model.Counter.make();\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_static_members.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_static_members_main.f", main_source);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_full_path_type_refs_do_not_require_use(void) {
    const char *external_source =
        "open module vendor.api;\n"
        "open type User {\n"
        "    open let name: string;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "func id(user: vendor.api.User): vendor.api.User {\n"
        "    return user;\n"
        "}\n"
        "func count(users: vendor.api.User[]): int {\n"
        "    return 0;\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_full_path_user.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_full_path_main.f", main_source);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_alias_type_ref_still_requires_use_alias(void) {
    const char *external_source =
        "open module vendor.api;\n"
        "open type User {\n"
        "    open let name: string;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "func bad(user: api.User): int {\n"
        "    return 0;\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_alias_requires_use.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_alias_requires_use_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(errors != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_declared_specs_enable_spec_coercion(void) {
    const char *external_source =
        "open module vendor.api;\n"
        "open spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "open type User: Named {\n"
        "    open let name: string;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.api as api;\n"
        "func project(user: api.User): api.Named {\n"
        "    return user;\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_named.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_spec_coercion_main.f", main_source);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_enum_item_participates_in_typecheck(void) {
    const char *external_source =
        "open module vendor.http;\n"
        "open enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.http as http;\n"
        "func run(): int {\n"
        "    let status: http.HttpStatus = http.HttpStatus.NotFound;\n"
        "    return (int)status;\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_enum.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_enum_main.f", main_source);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_enum_conflicts_with_local_type_name(void) {
    const char *external_source =
        "open module vendor.http;\n"
        "open enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.http;\n"
        "type HttpStatus {}\n"
        "let x: HttpStatus = HttpStatus{};\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    imported_source_fixture_init(&fixture, "external_enum_conflict.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_enum_conflict_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "external_enum_conflict_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strcmp(errors[0].code, "AE0005") == 0);
    ASSERT(strstr(errors[0].message, "HttpStatus") != NULL);
    ASSERT(strstr(errors[0].message, "ambiguous") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_external_imported_private_enum_is_not_visible(void) {
    const char *external_source =
        "open module vendor.http;\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import vendor.http as http;\n"
        "func run(): int {\n"
        "    return (int)http.HttpStatus.Ok;\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool saw_visibility_error = false;
    size_t error_index;

    imported_source_fixture_init(&fixture, "external_enum_private.ff", external_source);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_program_or_die("external_enum_private_main.f", main_source);
    programs[0] = program;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "external_enum_private_main.f") == 0);
    for (error_index = 0U; error_index < error_count; ++error_index) {
        if (strcmp(errors[error_index].path, "external_enum_private_main.f") == 0 &&
            strstr(errors[error_index].message, "does not export public name 'HttpStatus'") != NULL) {
            saw_visibility_error = true;
            break;
        }
    }
    ASSERT(saw_visibility_error);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_undefined_identifier_in_function_body(void) {
    const char *source =
        "module demo.main;\n"
        "func main(): int {\n"
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
        "module demo.main;\n"
        "func main(value: Missing): int {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    func read(): int {\n"
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
        "module demo.main;\n"
        "func main(): int {\n"
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
        "module demo.main;\n"
        "spec Thunk(): int;\n"
        "type User {\n"
        "    var id: int;\n"
        "    func read(): int {\n"
        "        let thunk: Thunk = () -> self.id;\n"
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
        "open module demo.base;\n"
        "open type User {}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func make(): base.User {\n"
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
        "open module demo.base;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func main(): int {\n"
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
        "open module demo.base;\n"
        "open func load(): int {\n"
        "    return 1;\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as base;\n"
        "func main() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    func read(): int {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    var name: string;\n"
        "    func display(): string;\n"
        "}\n"
        "spec Identified: Named {\n"
        "    func id(): int;\n"
        "}\n"
        "type Wrapper {\n"
        "    func process(target: Identified): int {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type Wrapper {\n"
        "    func rename(target: Named): void {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    var name: string;\n"
        "}\n"
        "type Wrapper {\n"
        "    func rename(target: Named): void {\n"
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
        "module demo.main;\n"
        "spec Mapper(x: int): int;\n"
        "type Wrapper {\n"
        "    func invoke(target: Mapper): void {\n"
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
        "module demo.main;\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "func run(): void {\n"
        "    let a: int = 1;\n"
        "    let b: i64 = 1;\n"
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
        "module demo.main;\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "func run(): void {\n"
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

static void test_numeric_literal_integer_adapts_to_explicit_float_targets(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let a: f32 = 1;\n"
        "    let b: f64 = 1;\n"
        "    let c: float = 1;\n"
        "    let d: double = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_int_to_float_targets_ok.f", source);
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

static void test_numeric_float_literal_to_integer_target_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let a: int = 1.0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_float_literal_to_int_rejected.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 3U);
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char expected[64];
        snprintf(expected, sizeof(expected), "does not match expected type '%s'", int_canonical);
        ASSERT(strstr(errors[0].message, expected) != NULL);
    }

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_typed_numeric_binding_requires_explicit_conversion_on_let_assignment(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let x: int = 1;\n"
        "    let y: f64 = x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("typed_numeric_binding_to_float_rejected.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'f64'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_numeric_literal_adapts_to_float_targets_on_var_binding(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    var a: f32 = 1;\n"
        "    var b: f64 = 1;\n"
        "    var c: f64 = 100 + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_var_binding_to_float_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_typed_numeric_binding_requires_explicit_conversion_on_var_binding(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let x: int = 1;\n"
        "    var y: f64 = x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("typed_numeric_var_binding_to_float_rejected.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'f64'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_numeric_literal_argument_adapts_to_float_targets(void) {
    const char *source =
        "module demo.main;\n"
        "type Box {\n"
        "    var value: f64;\n"
        "    func Box(v: f64) { self.value = v; }\n"
        "    func set(v: f64): void { self.value = v; }\n"
        "}\n"
        "func takes(v: f64): void {}\n"
        "func run(): void {\n"
        "    let b = Box(1);\n"
        "    b.set(1);\n"
        "    takes(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_literal_argument_to_float_ok.f", source);
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

static void test_typed_numeric_argument_requires_explicit_conversion_for_float_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "func takes(v: f64): void {}\n"
        "func run(): void {\n"
        "    let x: int = 1;\n"
        "    takes(x);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("typed_numeric_argument_to_float_rejected.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "has no overload accepting 1 argument") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_member_assignment_numeric_literal_adapts_to_float_field(void) {
    const char *source =
        "module demo.main;\n"
        "type Box {\n"
        "    var value: f64;\n"
        "}\n"
        "func run(): void {\n"
        "    var box = Box { value: 1 };\n"
        "    box.value = 1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("member_assign_numeric_literal_to_float_ok.f", source);
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

static void test_member_assignment_typed_numeric_binding_requires_explicit_conversion(void) {
    const char *source =
        "module demo.main;\n"
        "type Box {\n"
        "    var value: f64;\n"
        "}\n"
        "func run(): void {\n"
        "    var box = Box { value: 1 };\n"
        "    let x: int = 1;\n"
        "    box.value = x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("member_assign_typed_numeric_to_float_rejected.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 8U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'f64'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_numeric_constant_expression_adapts_to_float_target(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let x: f64 = 100 + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_const_expr_to_float_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_numeric_expression_with_identifier_requires_explicit_conversion(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let base: int = 100;\n"
        "    let x: f64 = base + 50;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("numeric_expr_with_identifier_to_float_rejected.f",
                                                 source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'f64'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_object_literal_reports_unknown_field(void) {
    const char *source =
        "module demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "spec Factory(): int;\n"
        "func make() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "    func User() {}\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    func User(name: string) {}\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    func User(id: i64) {}\n"
        "    func User(name: string) {}\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    func User(id: int) {}\n"
        "    func User(name: string) {}\n"
        "}\n"
        "func make(): User {\n"
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

static void test_constructor_call_matches_generic_owner_type_param(void) {
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    var val: T;\n"
        "    func Box(val: T) {\n"
        "        self.val = val;\n"
        "    }\n"
        "}\n"
        "func make(): Box<i64> {\n"
        "    return Box<i64>(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("ctor_generic_owner_type_param.f", source);
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
        "module demo.main;\n"
        "type User {\n"
        "    func User(id: int) {}\n"
        "    func User(name: string) {}\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "spec Factory(): int;\n"
        "func make() {\n"
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
        "module demo.main;\n"
        "spec CommitOptions {\n"
        "    var message: int;\n"
        "}\n"
        "func make() {\n"
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
        "open module demo.base;\n"
        "open type User {\n"
        "    seal func User() {}\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func make(): User {\n"
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

static void test_constructor_call_reports_inaccessible_imported_constructor(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open type User {\n"
        "    seal func User() {}\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func make(): User {\n"
        "    return User();\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("ctor_call_import_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("ctor_call_import_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_call_import_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "no accessible constructor accepting 0 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_object_literal_constructor_call_reports_inaccessible_imported_constructor(void) {
    const char *base_source =
        "open module demo.base;\n"
        "open type User {\n"
        "    seal func User() {}\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func make(): User {\n"
        "    return User() {};\n"
        "}\n";
    FengProgram *base_program = parse_program_or_die("ctor_objcall_import_base.f", base_source);
    FengProgram *main_program = parse_program_or_die("ctor_objcall_import_main.f", main_source);
    const FengProgram *programs[] = {base_program, main_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "ctor_objcall_import_main.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "no accessible constructor accepting 0 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(base_program);
    feng_program_free(main_program);
}

static void test_object_literal_rejects_decl_bound_let_member(void) {
    const char *source =
        "module demo.main;\n"
        "type User {\n"
        "    let id: int = 1;\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int = 1;\n"
        "    func User() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    func User() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    func update() {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    func User() {\n"
        "        self.id = 1;\n"
        "    }\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "    func User(id: int) {\n"
        "        self.id = id;\n"
        "    }\n"
        "    func User(name: string) {}\n"
        "}\n"
        "func make_ok(): User {\n"
        "    return User(\"ok\") { id: 1 };\n"
        "}\n"
        "func make_bad(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    let id: int;\n"
        "}\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    var id: int;\n"
        "}\n"
        "func make(): User {\n"
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
        "open module demo.base;\n"
        "open type User {\n"
        "    seal var secret: int;\n"
        "    open func User() {}\n"
        "}\n";
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "type User {\n"
        "    seal var secret: int;\n"
        "    func User() {}\n"
        "}\n";
    const char *source_b =
        "module demo.main;\n"
        "func make(): User {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "    func greet(): string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "    func greet(): string {\n"
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

static void test_object_form_spec_allows_method_same_name_as_spec(void) {
    /* spec 方法名 == spec 名不再视为构造器; 视为普通实例方法 */
    const char *source =
        "module demo.main;\n"
        "spec Shape {\n"
        "    func Shape(): int;\n"
        "}\n"
        "type Disk: Shape {\n"
        "    let name: string;\n"
        "    func Shape(): int { return 1; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_spec_same_name_method.f", source);
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

static void test_object_form_spec_rejects_finalizer_member(void) {
    const char *source =
        "module demo.main;\n"
        "spec Shape {\n"
        "    func ~Shape();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("object_spec_finalizer_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "object_spec_finalizer_error.f") == 0);
    ASSERT(strstr(errors[0].message,
                  "object-form spec 'Shape' cannot declare a finalizer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* --- Step 3: object-form spec static members --- */

/* type 自身静态字段 + 静态方法满足 spec 的静态成员约束 (含泛型 spec). */
static void test_type_satisfies_spec_static_members(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "    static let tag: string;\n"
        "}\n"
        "spec Configurable {\n"
        "    static var current: int;\n"
        "    static func reset(): void;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    let name: string;\n"
        "    static func make(): Widget {\n"
        "        return Widget { name: \"default\" };\n"
        "    }\n"
        "    static let tag: string = \"widget\";\n"
        "}\n"
        "type Config: Configurable {\n"
        "    static var current: int = 0;\n"
        "    static func reset(): void {\n"
        "        Config.current = 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("type_satisfies_spec_static.f", source);
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

/* fit 静态方法可以满足 spec 静态方法 (fit 仍不得声明静态字段). */
static void test_fit_satisfies_spec_static_method(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "type Gadget {\n"
    "    let id: int;\n"
        "    static let tag: string = \"gadget\";\n"
        "}\n"
        "fit Gadget: Factory<Gadget> {\n"
        "    static func make(): Gadget {\n"
        "        return Gadget { id: 0 };\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_satisfies_spec_static.f", source);
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

/* type 缺少 spec 要求的静态方法时, 满足检查应失败. */
static void test_spec_static_member_missing_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    let name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_static_missing.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "missing method") != NULL ||
          strstr(errors[0].message, "missing implementation") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* type 静态方法签名与 spec 不匹配时, 满足检查应失败. */
static void test_spec_static_member_signature_mismatch_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    let name: string;\n"
        "    static func make(): int {\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_static_sig_mismatch.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* spec 要求静态字段时, type 必须有匹配的静态字段. */
static void test_spec_static_field_missing_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    static let tag: string;\n"
        "}\n"
        "type Widget: Named {\n"
        "    let name: string;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_static_field_missing.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "missing field") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* 通过实例访问 spec 静态成员应报错. */
static void test_instance_access_of_static_member_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    static let tag: string;\n"
        "}\n"
        "type Widget: Named {\n"
        "    let name: string;\n"
        "    static let tag: string = \"w\";\n"
        "    func get_tag(): string {\n"
        "        return self.tag;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("instance_access_static.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Step 4 — 泛型约束 T.make() 类型推断. T: Factory<T> 可调用 T.make(). */
static void test_generic_param_static_method_call_type_inference(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    let value: int;\n"
        "    static func make(): Widget {\n"
        "        return Widget { value: 0 };\n"
        "    }\n"
        "}\n"
        "func create<T: Factory<T>>(): T {\n"
        "    return T.make();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_param_static_method.f", source);
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

/* Step 4 — 泛型约束 T.field 类型推断 (读). */
static void test_generic_param_static_field_read_type_inference(void) {
    const char *source =
        "module demo.main;\n"
        "spec Tagged {\n"
        "    static let tag: int;\n"
        "}\n"
        "type Widget: Tagged {\n"
        "    static let tag: int = 42;\n"
        "}\n"
        "func get_tag<T: Tagged>(): int {\n"
        "    return T.tag;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_param_static_field_read.f", source);
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

/* Step 4 — 泛型约束 T.field 类型推断 (写 var). */
static void test_generic_param_static_field_write_type_inference(void) {
    const char *source =
        "module demo.main;\n"
        "spec Configurable {\n"
        "    static var current: int;\n"
        "}\n"
        "type Config: Configurable {\n"
        "    static var current: int = 0;\n"
        "}\n"
        "func reset<T: Configurable>(value: int): void {\n"
        "    T.current = value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_param_static_field_write.f", source);
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

/* Step 4 — spec 父 spec 静态成员约束传递. 子 spec 继承父 spec 静态成员
 * 并要求 type 同时满足. */
static void test_spec_inherits_parent_static_member_constraint(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "spec Extended<T>: Factory<T> {\n"
        "}\n"
        "type Widget: Extended<Widget> {\n"
        "    let value: int;\n"
        "    static func make(): Widget {\n"
        "        return Widget { value: 0 };\n"
        "    }\n"
        "}\n"
        "func create<T: Extended<T>>(): T {\n"
        "    return T.make();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("spec_inherits_static.f", source);
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

/* Step 4 — 泛型约束 T.field 写入 let 应报错 (let 不可写). */
static void test_generic_param_static_let_field_write_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Tagged {\n"
        "    static let tag: int;\n"
        "}\n"
        "type Widget: Tagged {\n"
        "    static let tag: int = 42;\n"
        "}\n"
        "func reset<T: Tagged>(value: int): void {\n"
        "    T.tag = value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_static_let_write.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Step 4 — 泛型约束 T 调用未声明的静态方法应报错. */
static void test_generic_param_unknown_static_member_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    static func make(): Widget {\n"
        "        return Widget { value: 0 };\n"
        "    }\n"
        "    let value: int;\n"
        "}\n"
        "func create<T: Factory<T>>(): T {\n"
        "    return T.unknown();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_static_unknown.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_spec_parent_specs_must_be_spec(void) {
    const char *source =
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User: Named {\n"
        "    func greet(): int {\n"
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
        "module demo.main;\n"
        "spec Identified {\n"
        "    func id(): int;\n"
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
        "module demo.main;\n"
        "spec A {\n"
        "    func run(): int;\n"
        "}\n"
        "spec B {\n"
        "    func run(): string;\n"
        "}\n"
        "type Worker: A, B {\n"
        "    func run(): int {\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "fit User: Named {\n"
        "    func greet(): string {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
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
     * and `demo.adapter` declares a `open fit` that bridges them. Because the
     * adapter owns neither the type nor the spec, it is an orphan and its
     * `open` export must be downgraded to module-local visibility with an
     * informational note. */
    const char *src_types =
        "open module demo.types;\n"
        "open type User {}\n";
    const char *src_specs =
        "open module demo.specs;\n"
        "open spec Named {\n"
        "    func greet(): string;\n"
        "}\n";
    const char *src_adapter =
        "open module demo.adapter;\n"
        "import demo.types;\n"
        "import demo.specs;\n"
        "open fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {}\n"
        "open fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
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

/* Cross-module open fit becomes effective in the consumer when the consumer
 * imports the fit's owning module via `use`. The consumer can then call
 * the spec method on a value of the imported type. The fit module owns
 * the spec so it is not an orphan adapter. */
static void test_pu_fit_visible_after_use_enables_method_call(void) {
    const char *src_types =
        "open module demo.types;\n"
        "open type User {}\n";
    const char *src_adapter =
        "open module demo.adapter;\n"
        "import demo.types;\n"
        "open spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "open fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "open module demo.consumer;\n"
        "import demo.types;\n"
        "import demo.adapter;\n"
        "func run(): string {\n"
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

/* Without `use` of the fit's owning module, the open fit must not bridge
 * the type to the spec; calling the spec method on the type's value is
 * rejected because the contract relation is not in scope. */
static void test_pu_fit_invisible_without_use_rejects_method_call(void) {
    const char *src_types =
        "open module demo.types;\n"
        "open type User {}\n";
    const char *src_adapter =
        "open module demo.adapter;\n"
        "import demo.types;\n"
        "open spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "open fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "open module demo.consumer;\n"
        "import demo.types;\n"
        "func run(): string {\n"
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

/* Spec satisfaction at type position must consider open fits from any
 * module the consumer has `use`d, not only fits declared in the
 * consumer's own module. */
static void test_imported_pu_fit_satisfies_spec_typed_parameter(void) {
    const char *src_types =
        "open module demo.types;\n"
        "open type User {}\n";
    const char *src_adapter =
        "open module demo.adapter;\n"
        "import demo.types;\n"
        "open spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "open fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "open module demo.consumer;\n"
        "import demo.types;\n"
        "import demo.adapter;\n"
        "func use_named(n: Named): void { return; }\n"
        "func run(): void {\n"
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

/* Aliased `use` must also activate the imported module's open fit
 * contracts, even though the imported short names go through the
 * alias instead of being injected into the current scope. */
static void test_pu_fit_visible_via_alias_use(void) {
    const char *src_types =
        "open module demo.types;\n"
        "open type User {}\n";
    const char *src_adapter =
        "open module demo.adapter;\n"
        "import demo.types;\n"
        "open spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "open fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n";
    const char *src_consumer =
        "open module demo.consumer;\n"
        "import demo.types;\n"
        "import demo.adapter as adapter;\n"
        "func run(): string {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {}\n"
        "fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n"
        "func run(): string {\n"
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

static void test_fit_builtin_method_callable_on_literal(void) {
    const char *source =
        "module demo.main;\n"
        "fit i32 {\n"
        "    func double(): i32 { return self * 2; }\n"
        "}\n"
        "func run(): i32 {\n"
        "    let x: i32 = 21;\n"
        "    return x.double();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_builtin_call.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_enum_method_callable_on_item(void) {
    const char *source =
        "module demo.main;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status {\n"
        "    func code(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
        "    return Status.Failed.code();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_enum_call.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_enum_satisfies_spec_typed_parameter(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    func code(): int;\n"
        "}\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status: Named {\n"
        "    func code(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "}\n"
        "func use_named(value: Named): int {\n"
        "    return value.code();\n"
        "}\n"
        "func run(): int {\n"
        "    return use_named(Status.Ok);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_enum_spec_param.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_enum_satisfies_generic_constraint(void) {
    const char *source =
        "module demo.main;\n"
        "spec Hashable<T> {\n"
        "    func hash(): int;\n"
        "    func same(other: T): bool;\n"
        "}\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status: Hashable<Status> {\n"
        "    func hash(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "    func same(other: Status): bool {\n"
        "        return self == other;\n"
        "    }\n"
        "}\n"
        "func use_hash<K: Hashable<K>>(value: K): int {\n"
        "    return value.hash();\n"
        "}\n"
        "func run(): int {\n"
        "    return use_hash(Status.Failed);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_enum_generic_constraint.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Overload resolution rule: a generic candidate whose type argument fails the
 * declared constraint must be excluded from the candidate set. When a
 * non-generic overload is applicable, it must win; if no candidate survives,
 * the call must be rejected as ambiguous (no overload accepting arguments). */
static void test_generic_overload_constraint_excludes_candidate(void) {
    /* Non-generic `m(value: Tagged)` accepts the spec view; a second overload
     * `m<T: Tagged>(value: T)` exists. The call `m(Untagged{})` should fail
     * because Untagged does not fit Tagged, and the spec view also rejects the
     * concrete Untagged value at the non-generic overload — so neither
     * candidate matches. */
    const char *source =
        "module demo.main;\n"
        "spec Tagged {\n"
        "    func tag(): string;\n"
        "}\n"
        "type Untagged {\n"
        "    let name: string;\n"
        "}\n"
        "func m(value: Tagged): string {\n"
        "    return value.tag();\n"
        "}\n"
        "func m<T: Tagged>(value: T): string {\n"
        "    return value.tag();\n"
        "}\n"
        "func run(): string {\n"
        "    return m(Untagged { name: \"x\" });\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_overload_excluded.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    /* The call must be reported as not matching any overload (the generic
     * candidate is dropped because the constraint fails; the non-generic
     * candidate fails because Untagged does not fit Tagged). */
    ASSERT(errors[0].code != NULL);
    ASSERT(strcmp(errors[0].code, "AE0512") == 0);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Overload resolution rule: when only a generic overload is applicable and the
 * inferred type argument satisfies the constraint, the generic candidate must
 * be selected. */
static void test_generic_overload_selected_when_only_candidate(void) {
    const char *source =
        "module demo.main;\n"
        "spec Tagged {\n"
        "    func tag(): string;\n"
        "}\n"
        "type TaggedUser: Tagged {\n"
        "    let label: string;\n"
        "    func tag(): string {\n"
        "        return self.label;\n"
        "    }\n"
        "}\n"
        "func only_generic<T: Tagged>(value: T): string {\n"
        "    return value.tag();\n"
        "}\n"
        "func run(): string {\n"
        "    return only_generic(TaggedUser { label: \"ok\" });\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_overload_only.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Overload resolution rule: when both a non-generic and a generic overload are
 * applicable to the same call, the non-generic candidate must win. */
static void test_non_generic_overload_preferred_over_generic(void) {
    /* `m(value: TaggedUser)` (non-generic exact) competes with
     * `m<T: Tagged>(value: T)` (generic, also exact for TaggedUser). The
     * non-generic candidate must win. */
    const char *source =
        "module demo.main;\n"
        "spec Tagged {\n"
        "    func tag(): string;\n"
        "}\n"
        "type TaggedUser: Tagged {\n"
        "    let label: string;\n"
        "    func tag(): string {\n"
        "        return self.label;\n"
        "    }\n"
        "}\n"
        "func m(value: TaggedUser): string {\n"
        "    return \"non-generic\";\n"
        "}\n"
        "func m<T: Tagged>(value: T): string {\n"
        "    return \"generic\";\n"
        "}\n"
        "func run(): string {\n"
        "    return m(TaggedUser { label: \"ok\" });\n"
        "}\n";
    FengProgram *program = parse_program_or_die("non_generic_overload_preferred.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_enum_missing_method_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Hashable<T> {\n"
        "    func hash(): int;\n"
        "    func same(other: T): bool;\n"
        "}\n"
        "enum Status {\n"
        "    Ok\n"
        "}\n"
        "fit Status: Hashable<Status> {\n"
        "    func hash(): int {\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_enum_missing.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message,
                  "type 'Status' is missing method 'same' required by spec 'Hashable'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_enum_unknown_member_still_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Status {\n"
        "    Ok\n"
        "}\n"
        "fit Status {\n"
        "    func label(): string {\n"
        "        return \"ok\";\n"
        "    }\n"
        "}\n"
        "func run(): void {\n"
        "    Status.Ok.missing();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_enum_unknown.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "no member 'missing'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_array_method_callable_on_value(void) {
    const char *source =
        "module demo.main;\n"
        "fit int[] {\n"
        "    func head(): int { return self[0]; }\n"
        "}\n"
        "func run(): int {\n"
        "    let xs: int[] = [3, 4];\n"
        "    return xs.head();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_array_call.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* C2: fit with spec clause for builtin/array targets is now valid when a body
 * is provided.  A stub without a body (';' only) must still be rejected. */
static void test_fit_builtin_target_rejects_specs_clause_without_body(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "fit i32: Named;\n";
    FengProgram *program = parse_program_or_die("fit_builtin_specs_reject.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "requires a body") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_array_target_rejects_specs_clause_without_body(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "fit int[]: Named;\n";
    FengProgram *program = parse_program_or_die("fit_array_specs_reject.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strstr(errors[0].message, "requires a body") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_array_target_element_type_param_visible_in_body(void) {
    const char *source =
        "module demo.main;\n"
        "fit T[!] {\n"
        "    func head(): T {\n"
        "        return self[0];\n"
        "    }\n"
        "}\n"
        "func run(xs: int[!]): void {\n"
        "    return;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_array_t_scope_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_array_target_element_type_param_does_not_leak(void) {
    const char *source =
        "module demo.main;\n"
        "fit T[] {\n"
        "    func head(): T {\n"
        "        return self[0];\n"
        "    }\n"
        "}\n"
        "func bad(x: T): T {\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_array_t_scope_no_leak.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "unknown type 'T'") != NULL);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_fit_user_type_path_still_uses_current_type_decl(void) {
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    let value: T;\n"
        "}\n"
        "fit Box<T> {\n"
        "    func get(): T {\n"
        "        return self.value;\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
        "    let b: Box<int> = Box<int>();\n"
        "    return b.get();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_user_type_path_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_user_type_satisfaction_reuses_visible_fit_members(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {}\n"
        "fit User {\n"
        "    func greet(): string {\n"
        "        return \"hi\";\n"
        "    }\n"
        "}\n"
        "fit User: Named;\n";
    FengProgram *program = parse_program_or_die("fit_user_visible_fit_member_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_pu_builtin_self_fit_emits_no_orphan_info(void) {
    const char *source =
        "open module demo.main;\n"
        "open fit i32 {\n"
        "    func double(): i32 {\n"
        "        return self * 2;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fit_builtin_orphan_export.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *fit_decl = NULL;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(analysis != NULL);
    ASSERT(analysis->info_count == 0U);

    for (size_t i = 0U; i < program->declaration_count; ++i) {
        if (program->declarations[i]->kind == FENG_DECL_FIT) {
            fit_decl = program->declarations[i];
            break;
        }
    }
    ASSERT(fit_decl != NULL);
    ASSERT(fit_decl->visibility == FENG_VISIBILITY_PUBLIC);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_fit_method_unknown_member_still_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {}\n"
        "fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {\n"
        "    seal let secret: string;\n"
        "}\n"
        "fit User: Named {\n"
        "    func greet(): string {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {\n"
        "    seal func whisper(): string { return \"shh\"; }\n"
        "}\n"
        "fit User: Named {\n"
        "    func greet(): string {\n"
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
        "module demo.main;\n"
        "spec Tagged {\n"
        "    func tag_of(other: User): string;\n"
        "}\n"
        "type User {\n"
        "    seal let secret: string;\n"
        "}\n"
        "fit User: Tagged {\n"
        "    func tag_of(other: User): string {\n"
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
        "module demo.main;\n"
        "spec Builder {\n"
        "    func make(): User;\n"
        "}\n"
        "type User {\n"
        "    let name: string;\n"
        "    seal let secret: string;\n"
        "}\n"
        "fit User: Builder {\n"
        "    func make(): User {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {\n"
        "    let name: string;\n"
        "    seal let secret: string;\n"
        "    func shout(): string { return self.name; }\n"
        "}\n"
        "fit User: Named {\n"
        "    func greet(): string {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "}\n"
        "func use_named(n: Named): void {\n"
        "    return;\n"
        "}\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type Other {\n"
        "    let name: string;\n"
        "}\n"
        "func use_named(n: Named): void {\n"
        "    return;\n"
        "}\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type Other {\n"
        "    let name: string;\n"
        "}\n"
        "fit Other: Named;\n"
        "func use_named(n: Named): void {\n"
        "    return;\n"
        "}\n"
        "func run(): void {\n"
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
        "module demo.main;\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User {\n"
        "    func shout(): string { return \"HI\"; }\n"
        "}\n"
        "fit User: Named {\n"
        "    func greet(): string { return \"hi\"; }\n"
        "}\n"
        "func helper(): int { return 1; }\n"
        "func run(): int {\n"
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

static void test_static_members_semantic_resolution(void) {
    const char *source =
        "module demo.main;\n"
        "type Counter {\n"
        "    static let seed: int = 1;\n"
        "    static var current: int = 0;\n"
        "    static func make(value: int): int {\n"
        "        return value + Counter.seed;\n"
        "    }\n"
        "    func make(): int {\n"
        "        return 7;\n"
        "    }\n"
        "}\n"
        "fit string {\n"
        "    static func marker(): int {\n"
        "        return 3;\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
        "    let c: Counter = Counter();\n"
        "    let a: int = Counter.seed;\n"
        "    Counter.current = a;\n"
        "    let b: int = Counter.make(a);\n"
        "    let d: int = c.make();\n"
        "    return b + d + string.marker();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("static_semantic.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *run_decl;
    const FengDecl *counter_type = NULL;
    CallList calls = {NULL, 0U, 0U};
    const FengExpr *call_static_make;
    const FengExpr *call_fit_static_marker;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    for (size_t i = 0U; i < program->declaration_count; ++i) {
        const FengDecl *decl = program->declarations[i];
        if (decl->kind == FENG_DECL_TYPE &&
            decl->as.type_decl.name.length == 7U &&
            memcmp(decl->as.type_decl.name.data, "Counter", 7U) == 0) {
            counter_type = decl;
            break;
        }
    }
    ASSERT(counter_type != NULL);

    run_decl = find_function_decl_by_name(program, "run");
    ASSERT(run_decl != NULL);
    collect_calls_in_block(run_decl->as.function_decl.body, &calls);

    call_static_make = find_call_with_member_name(&calls, "make");
    ASSERT(call_static_make != NULL);
    ASSERT(call_static_make->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD);
    ASSERT(call_static_make->as.call.resolved_callable.owner_type_decl == counter_type);
    ASSERT(call_static_make->as.call.resolved_callable.member != NULL);
    ASSERT(call_static_make->as.call.resolved_callable.member->is_static);

    call_fit_static_marker = find_call_with_member_name(&calls, "marker");
    ASSERT(call_fit_static_marker != NULL);
    ASSERT(call_fit_static_marker->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD);
    ASSERT(call_fit_static_marker->as.call.resolved_callable.fit_decl != NULL);
    ASSERT(call_fit_static_marker->as.call.resolved_callable.member != NULL);
    ASSERT(call_fit_static_marker->as.call.resolved_callable.member->is_static);

    free(calls.items);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_static_members_semantic_resolution(void) {
    const char *source =
        "module demo.main;\n"
        "type Util {\n"
        "    static func id<T>(value: T): T {\n"
        "        return value;\n"
        "    }\n"
        "}\n"
        "type Box<T> {\n"
        "    let value: T;\n"
        "    static func make(value: T): Box<T> {\n"
        "        return Box<T> { value: value };\n"
        "    }\n"
        "}\n"
        "fit Box<T> {\n"
        "    static func of(value: T): Box<T> {\n"
        "        return Box<T> { value: value };\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
        "    let a: int = Util.id<int>(1);\n"
        "    let b: Box<int> = Box<int>.make(a);\n"
        "    let c: Box<int> = Box<int>.of(a);\n"
        "    return b.value + c.value;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("generic_static_semantic.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *run_decl;
    CallList calls = {NULL, 0U, 0U};
    const FengExpr *call_id;
    const FengExpr *call_make;
    const FengExpr *call_of;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (generic static members): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    run_decl = find_function_decl_by_name(program, "run");
    ASSERT(run_decl != NULL);
    collect_calls_in_block(run_decl->as.function_decl.body, &calls);

    call_id = find_call_with_member_name(&calls, "id");
    ASSERT(call_id != NULL);
    ASSERT(call_id->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD);
    ASSERT(call_id->as.call.resolved_callable.member != NULL);
    ASSERT(call_id->as.call.resolved_callable.member->is_static);

    call_make = find_call_with_member_name(&calls, "make");
    ASSERT(call_make != NULL);
    ASSERT(call_make->as.call.resolved_callable.kind ==
           FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD);
    ASSERT(call_make->as.call.resolved_callable.member != NULL);
    ASSERT(call_make->as.call.resolved_callable.member->is_static);

        call_of = find_call_with_member_name(&calls, "of");
        ASSERT(call_of != NULL);
        ASSERT(call_of->as.call.resolved_callable.kind ==
            FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD);
        ASSERT(call_of->as.call.resolved_callable.member != NULL);
        ASSERT(call_of->as.call.resolved_callable.member->is_static);

    free(calls.items);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_static_member_instance_access_is_rejected(void) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {
            "module demo.main;\n"
            "type Counter {\n"
            "    static let seed: int = 1;\n"
            "}\n"
            "func run(): int {\n"
            "    let c: Counter = Counter();\n"
            "    return c.seed;\n"
            "}\n",
            "must be accessed through its type"
        },
        {
            "module demo.main;\n"
            "type Counter {\n"
            "    static func make(): int { return 1; }\n"
            "}\n"
            "func run(): int {\n"
            "    let c: Counter = Counter();\n"
            "    return c.make();\n"
            "}\n",
            "must be accessed through its type"
        }
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = parse_program_or_die("static_instance_access.f", cases[i].source);
        const FengProgram *programs[] = {program};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        ASSERT(!feng_semantic_analyze(programs,
                                      1U,
                                      FENG_COMPILE_TARGET_LIB,
                                      &analysis,
                                      &errors,
                                      &error_count));
        ASSERT(error_count >= 1U);
        ASSERT(strstr(errors[0].message, cases[i].message) != NULL);
        feng_semantic_errors_free(errors, error_count);
        feng_program_free(program);
    }
}

static void test_duplicate_static_method_signature_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Counter {\n"
        "    static func make(): int { return 1; }\n"
        "    static func make(): int { return 2; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("static_duplicate_method.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "overloads") != NULL ||
           strstr(errors[0].message, "duplicate") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finalizer_basic_ok(void) {
    const char *source =
        "module demo.main;\n"
        "type Buffer {\n"
        "    open var size: int;\n"
        "    func Buffer(s: int) {\n"
        "        self.size = s;\n"
        "    }\n"
        "    func ~Buffer() {\n"
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
        "module demo.main;\n"
        "type Buffer {\n"
        "    func ~Buffer() {}\n"
        "    func ~Buffer() {}\n"
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

static void test_finalizer_rejected_on_abi_type(void) {
    const char *source =
        "module demo.main;\n"
        "@abi\n"
        "type Buffer {\n"
        "    open let size: int;\n"
        "    func ~Buffer() {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("fin_abi.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "@abi") != NULL);
    ASSERT(strstr(errors[0].message, "finalizer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_finalizer_rejects_return_with_value(void) {
    const char *source =
        "module demo.main;\n"
        "type Buffer {\n"
        "    func ~Buffer() {\n"
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
        "module demo.main;\n"
        "type Box {\n"
        "    func Box() {\n"
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
        "module demo.main;\n"
        "type Box {\n"
        "    open var v: int;\n"
        "    func Box(): void {\n"
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
        "module demo.main;\n"
        "spec IntFn(a: int): int;\n"
        "func run(): int {\n"
        "    let f: IntFn = (a: int) {\n"
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
        "module demo.main;\n"
        "spec Reader(): int;\n"
        "func run(): int {\n"
        "    let x = 1;\n"
        "    let f: Reader = () {\n"
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
    ASSERT(program->declaration_count >= 2U);
    {
        const FengDecl *decl = program->declarations[1];
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
        "module demo.main;\n"
        "spec Reader(): int;\n"
        "type User {\n"
        "    var id: int;\n"
        "    func read(): int {\n"
        "        let f: Reader = () -> self.id;\n"
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
        const FengDecl *type_decl = program->declarations[1];
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
        "module demo.main;\n"
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
        "module demo.main;\n"
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
        "module demo.main;\n"
        "type T {\n"
        "    func pick(a: int): int { return a; }\n"
        "    func pick(a: int): int { return a + 1; }\n"
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
        "module demo.main;\n"
        "type T {\n"
        "    func pick(a: int): int { return a; }\n"
        "    func pick(a: int): bool { return true; }\n"
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
        "module demo.main;\n"
        "func helper(): int { return 0; }\n";
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
        "module demo.main;\n"
        "func main(args: string[]) {\n"
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
        "module demo.main;\n"
        "func main(): int { return 0; }\n";
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
        "module demo.main;\n"
        "func main(args: string[]) { return; }\n";
    const char *source_b =
        "module demo.other;\n"
        "func main(args: string[]) { return; }\n";
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
        "module demo.main;\n"
        "func helper(): int { return 0; }\n";
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
        "module demo.main;\n"
        "func run(value: int): int {\n"
        "    return match value {\n"
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
        "module demo.main;\n"
        "func run(value: int): int {\n"
        "    return match value {\n"
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
        "module demo.main;\n"
        "func run(value: int): int {\n"
        "    return match value {\n"
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
        "module demo.main;\n"
        "func run(value: f64): int {\n"
        "    return match value {\n"
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
        "module demo.main;\n"
        "func run(value: int): int {\n"
        "    let one = 1;\n"
        "    return match value {\n"
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

static void test_match_enum_single_label_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        Color.Red { print(1); }\n"
        "        Color.Green { print(2); }\n"
        "        Color.Blue { print(3); }\n"
        "        else { print(0); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_single.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_match_enum_value_list_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404,\n"
        "    InternalError = 500\n"
        "}\n"
        "func run(status: HttpStatus) {\n"
        "    match status {\n"
        "        HttpStatus.Ok { print(200); }\n"
        "        HttpStatus.NotFound, HttpStatus.InternalError { print(500); }\n"
        "        else { print(0); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_value_list.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_match_enum_expression_form_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func describe(c: Color): int {\n"
        "    return match c {\n"
        "        Color.Red { 1; }\n"
        "        Color.Green { 2; }\n"
        "        else { 3; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_enum_expr.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_match_enum_block_tail_return_value_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func describe(c: Color): int {\n"
        "    match c {\n"
        "        Color.Red { 1; }\n"
        "        Color.Green { 2; }\n"
        "        else { 3; }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_enum_block_tail.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_match_enum_cross_enum_reference_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "enum Shape {\n"
        "    Circle,\n"
        "    Square\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        Shape.Circle { print(0); }\n"
        "        Color.Red { print(1); }\n"
        "        else { print(2); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_cross.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "match label references enum") != NULL);
    ASSERT(strstr(errors[0].message, "but target type is enum") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_enum_range_label_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        0...2 { print(0); }\n"
        "        Color.Blue { print(2); }\n"
        "        else { print(3); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_range.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "match enum target does not support range labels") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_enum_nonexistent_item_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        Color.Purple { print(0); }\n"
        "        else { print(1); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_missing_item.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "has no item") != NULL);
    ASSERT(strstr(errors[0].message, "Purple") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_enum_item_duplicate_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        Color.Red { print(1); }\n"
        "        Color.Red { print(2); }\n"
        "        else { print(0); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_dup.f", source);
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

static void test_match_enum_mixed_with_int_literal_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        0 { print(0); }\n"
        "        Color.Red { print(1); }\n"
        "        else { print(2); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_mixed_int.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "match enum target requires enum item reference label") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_enum_mixed_with_string_literal_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        \"red\" { print(0); }\n"
        "        Color.Red { print(1); }\n"
        "        else { print(2); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_mixed_str.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "match enum target requires enum item reference label") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_enum_binding_prefix_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        x: Color { print(0); }\n"
        "        Color.Red { print(1); }\n"
        "        else { print(2); }\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("match_enum_binding.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "match enum target requires enum item reference label") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_enum_explicit_values_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404,\n"
        "    InternalError = 500\n"
        "}\n"
        "func run(status: HttpStatus): int {\n"
        "    return match status {\n"
        "        HttpStatus.Ok { 200; }\n"
        "        HttpStatus.NotFound { 404; }\n"
        "        else { 500; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_enum_explicit.f", source);
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
        "module demo.main;\n"
        "func run(items: int[]) {\n"
        "    for let it in items {\n"
        "        print(it);\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
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
        "module demo.main;\n"
        "func run(value: int) {\n"
        "    for let it in value {\n"
        "        print(it);\n"
        "    }\n"
        "}\n"
        "func print(value: int) {}\n";
    FengProgram *program = parse_program_or_die("for_in_non_array.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "is not iterable (no @iterable or @iterator method found)") != NULL);

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
        "open module demo.cyc;\n"
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
        "open module demo.cyc;\n"
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
        "open module demo.cyc;\n"
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
        "open module demo.cyc;\n"
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
        "open module demo.cyc;\n"
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
    if (rel == NULL) { return false; }
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

/* Convenience wrappers that build a subject key from the caller's preferred
 * representation and forward to feng_semantic_lookup_spec_relation. */
static const FengSpecRelation *lookup_relation_for_type_decl(
        const FengSemanticAnalysis *analysis,
        const FengDecl *type_decl,
        const FengDecl *spec_decl) {
    FengSemanticSubjectKey sk = feng_semantic_subject_key_for_type_decl(type_decl);
    return feng_semantic_lookup_spec_relation(analysis, &sk, spec_decl);
}

static const FengSpecRelation *lookup_relation_for_builtin(
        const FengSemanticAnalysis *analysis,
        const char *canonical_name,
        const FengDecl *spec_decl) {
    FengSemanticSubjectKey sk = feng_semantic_subject_key_for_builtin(canonical_name);
    return feng_semantic_lookup_spec_relation(analysis, &sk, spec_decl);
}

static void test_spec_relation_declared_head_recorded(void) {
    /* `type T : S` records a single DECLARED_HEAD source for (T, S) and
     * does not invent any other relation. */
    const char *src =
        "open module demo.rel;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named { func name(): string { return \"u\"; } }\n";
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

    const FengSpecRelation *rel = lookup_relation_for_type_decl(analysis, user, named);
    ASSERT(rel != NULL);
    ASSERT(rel->source_count == 1U);
    ASSERT(relation_has_source(rel, FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD,
                               named, NULL));

    /* Reverse direction must not exist. */
    ASSERT(lookup_relation_for_type_decl(analysis, user, user) == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_relation_declared_parent_transitive(void) {
    /* `type T : Child` where `spec Child : Parent` records DECLARED_HEAD for
     * (T, Child) and DECLARED_PARENT for (T, Parent) via the head Child. */
    const char *src =
        "open module demo.rel;\n"
        "spec Parent { func p(): int; }\n"
        "spec Child: Parent { func c(): int; }\n"
        "type Both: Child {\n"
        "    func p(): int { return 1; }\n"
        "    func c(): int { return 2; }\n"
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

    const FengSpecRelation *rel_child = lookup_relation_for_type_decl(analysis, both, child);
    ASSERT(rel_child != NULL);
    ASSERT(relation_has_source(rel_child, FENG_SPEC_RELATION_SOURCE_DECLARED_HEAD,
                               child, NULL));

    const FengSpecRelation *rel_parent = lookup_relation_for_type_decl(analysis, both, parent);
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
        "open module demo.rel;\n"
        "spec Parent { func p(): int; }\n"
        "spec Child: Parent { func c(): int; }\n"
        "type Tag {}\n"
        "fit Tag: Child {\n"
        "    func p(): int { return 1; }\n"
        "    func c(): int { return 2; }\n"
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

    const FengSpecRelation *rel_child = lookup_relation_for_type_decl(analysis, tag, child);
    ASSERT(rel_child != NULL);
    ASSERT(relation_has_source(rel_child, FENG_SPEC_RELATION_SOURCE_FIT_HEAD,
                               child, fit));

    const FengSpecRelation *rel_parent = lookup_relation_for_type_decl(analysis, tag, parent);
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
    /* A `open fit` lives in module A; module B does not `use` A; module C
     * does. The relation table records the fit source unconditionally;
     * the visibility helper rejects B and accepts C. */
    const char *src_a =
        "open module demo.a;\n"
        "open spec Named { func name(): string; }\n"
        "open type Tag {}\n"
        "open fit Tag: Named { func name(): string { return \"t\"; } }\n";
    const char *src_b =
        "module demo.b;\n"
        "func unrelated() {}\n";
    const char *src_c =
        "module demo.c;\n"
        "import demo.a;\n"
        "func unrelated() {}\n";
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

    const FengSpecRelation *rel = lookup_relation_for_type_decl(analysis, tag, named);
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
static void test_spec_relation_fit_builtin_target(void) {
    /* `fit i32: Named { ... }` should record a FIT_HEAD relation with a
     * BUILTIN subject key for "i32". */
    const char *src =
        "open module demo.rel.builtin;\n"
        "open spec Named { func name(): string; }\n"
        "open fit i32: Named { func name(): string { return \"i32\"; } }\n";
    FengProgram *program = parse_program_or_die("rel_builtin.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(named != NULL);

    const FengSpecRelation *rel = lookup_relation_for_builtin(analysis, "i32", named);
    ASSERT(rel != NULL);
    ASSERT(rel->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN);
    ASSERT(rel->subject_key.as.builtin_canonical_name != NULL);
    ASSERT(strcmp(rel->subject_key.as.builtin_canonical_name, "i32") == 0);
    ASSERT(rel->source_count >= 1U);
    /* At least one source must be FIT_HEAD (no via_spec_decl since Named has
     * no parent specs in this program). */
    bool has_fit_head = false;
    for (size_t i = 0U; i < rel->source_count; ++i) {
        if (rel->sources[i].kind == FENG_SPEC_RELATION_SOURCE_FIT_HEAD) {
            has_fit_head = true;
            break;
        }
    }
    ASSERT(has_fit_head);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_relation_fit_array_target(void) {
    /* `fit i32[]: Named { ... }` should record a FIT_HEAD relation with an
     * ARRAY subject key whose element type is i32. */
    const char *src =
        "open module demo.rel.array;\n"
        "open spec Named { func name(): string; }\n"
        "open fit i32[]: Named { func name(): string { return \"arr\"; } }\n";
    FengProgram *program = parse_program_or_die("rel_array.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(named != NULL);

    /* Build the array subject key for i32[]. */
    FengSemanticSubjectKey i32_key = feng_semantic_subject_key_for_builtin("i32");
    FengSemanticSubjectKey arr_key;
    /* We don't have direct access to the fit decl's type ref here;
     * instead verify that a relation with ARRAY kind exists for the spec. */
    (void)i32_key;
    bool found_array_rel = false;
    for (size_t i = 0U; i < analysis->spec_relation_count; ++i) {
        const FengSpecRelation *r = &analysis->spec_relations[i];
        if (r->spec_decl == named &&
            r->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_ARRAY) {
            /* Verify there is at least one FIT_HEAD source. */
            for (size_t j = 0U; j < r->source_count; ++j) {
                if (r->sources[j].kind == FENG_SPEC_RELATION_SOURCE_FIT_HEAD) {
                    found_array_rel = true;
                    break;
                }
            }
        }
    }
    ASSERT(found_array_rel);
    (void)arr_key;

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
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

static const FengExpr *nth_let_initializer(const FengCallableSignature *fn,
                                           size_t binding_index) {
    size_t seen = 0U;

    for (size_t i = 0U; i < fn->body->statement_count; ++i) {
        const FengStmt *s = fn->body->statements[i];

        if (s->kind != FENG_STMT_BINDING) {
            continue;
        }
        if (seen == binding_index) {
            return s->as.binding.initializer;
        }
        ++seen;
    }
    return NULL;
}

static void test_spec_coercion_object_let_binding(void) {
    /* `let x: Named = User{...};` records an OBJECT-form coercion site on
     * the initializer expression, with the SpecRelation entry that
     * justifies the satisfaction. */
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func make(): int {\n"
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
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL);
    ASSERT(site->src_subject_key.as.type_decl == user);
    ASSERT(site->target_spec_decl == named);
    ASSERT(site->relation != NULL);
    ASSERT(site->relation->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL);
    ASSERT(site->relation->subject_key.as.type_decl == user);
    ASSERT(site->relation->spec_decl == named);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_builtin_let_binding(void) {
    /* `let x: Named = ((i32)7);` should record OBJECT coercion with BUILTIN subject
     * key once a visible `fit i32: Named { ... }` exists. */
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "fit i32: Named {\n"
        "    func name(): string { return \"i32\"; }\n"
        "}\n"
        "func make(): int {\n"
        "    let x: Named = ((i32)7);\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_builtin_let.f", src);
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
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN);
    ASSERT(site->src_subject_key.as.builtin_canonical_name != NULL);
    ASSERT(strcmp(site->src_subject_key.as.builtin_canonical_name, "i32") == 0);
    ASSERT(site->object_subject_storage == FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER);
    ASSERT(site->target_spec_decl == find_spec_decl_by_name(analysis, "Named"));
    ASSERT(site->relation != NULL);
    ASSERT(site->relation->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN);
    ASSERT(strcmp(site->relation->subject_key.as.builtin_canonical_name, "i32") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_array_let_binding(void) {
    /* `let x: Named = xs;` where xs: int[] should record OBJECT coercion with
     * ARRAY subject key when `fit int[]: Named { ... }` exists. */
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "fit int[]: Named {\n"
        "    func name(): string { return \"arr\"; }\n"
        "}\n"
        "func make(): int {\n"
        "    let xs: int[] = [1, 2];\n"
        "    let x: Named = xs;\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_array_let.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *make = find_function_decl_in_program(program, "make");
    ASSERT(make != NULL);
    const FengExpr *init = nth_let_initializer(&make->as.function_decl, 1U);
    ASSERT(init != NULL);

    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, init);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_ARRAY);
    ASSERT(site->src_subject_key.as.array.rank == 1U);
    ASSERT(site->object_subject_storage == FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER);
    ASSERT(site->target_spec_decl == find_spec_decl_by_name(analysis, "Named"));
    ASSERT(site->relation != NULL);
    ASSERT(site->relation->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_ARRAY);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_argument(void) {
    /* Argument-passing coercion: `accept(User{...})` against parameter
     * `s: Named` records an OBJECT site on the argument expression. */
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func accept(s: Named) {}\n"
        "func caller(): int {\n"
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
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL);
    ASSERT(site->src_subject_key.as.type_decl == user);
    ASSERT(site->target_spec_decl == named);
    ASSERT(site->relation != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_scalar_argument_uses_box_owner(void) {
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "fit i32: Named {\n"
        "    func name(): string { return \"i32\"; }\n"
        "}\n"
        "func accept(n: Named): string {\n"
        "    return n.name();\n"
        "}\n"
        "func caller(): string {\n"
        "    return accept(((i32)7));\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_scalar_arg.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *caller = find_function_decl_in_program(program, "caller");
    ASSERT(caller != NULL);
    const FengStmt *ret_stmt = caller->as.function_decl.body->statements[0];
    ASSERT(ret_stmt->kind == FENG_STMT_RETURN);
    const FengExpr *ret_expr = ret_stmt->as.return_value;
    ASSERT(ret_expr != NULL);
    ASSERT(ret_expr->kind == FENG_EXPR_CALL);
    ASSERT(ret_expr->as.call.arg_count == 1U);

    const FengExpr *arg = ret_expr->as.call.args[0];
    const FengSpecCoercionSite *site = feng_semantic_lookup_spec_coercion_site(analysis, arg);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_OBJECT);
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN);
    ASSERT(site->src_subject_key.as.builtin_canonical_name != NULL);
    ASSERT(strcmp(site->src_subject_key.as.builtin_canonical_name, "i32") == 0);
    ASSERT(site->object_subject_storage == FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_return(void) {
    /* Return coercion: `return User{...};` from a function returning Named
     * records an OBJECT site on the returned expression. */
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func make(): Named {\n"
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
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL);
    ASSERT(site->src_subject_key.as.type_decl == find_type_decl_by_name(analysis, "User"));
    ASSERT(site->relation != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_object_scalar_return_uses_box_owner(void) {
    const char *src =
        "open module demo.coerce;\n"
        "spec Named { func name(): string; }\n"
        "fit i32: Named {\n"
        "    func name(): string { return \"i32\"; }\n"
        "}\n"
        "func make(): Named {\n"
        "    return ((i32)7);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_scalar_ret.f", src);
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
    ASSERT(site->src_subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN);
    ASSERT(strcmp(site->src_subject_key.as.builtin_canonical_name, "i32") == 0);
    ASSERT(site->object_subject_storage == FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_coercion_callable_top_level_fn(void) {
    /* `let f: Cb = my_fn;` — top-level function bound to a callable-form
     * spec slot records a CALLABLE site classified as TOP_LEVEL_FN. */
    const char *src =
        "open module demo.coerce;\n"
        "spec Cb(x: int): int;\n"
        "func double(x: int): int { return x + x; }\n"
        "func caller(): int {\n"
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
        "open module demo.coerce;\n"
        "spec Cb(x: int): int;\n"
        "func caller(): int {\n"
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

static void test_spec_coercion_callable_lambda_argument(void) {
    const char *src =
        "open module demo.coerce.arg;\n"
        "spec Cb(x: int): int;\n"
        "func apply(cb: Cb): int {\n"
        "    return cb(4);\n"
        "}\n"
        "func caller(): int {\n"
        "    return apply((x: int) -> x + 1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("coerce_callable_lambda_arg.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *caller;
    const FengStmt *return_stmt;
    const FengExpr *call_expr;
    const FengExpr *arg;
    const FengSpecCoercionSite *site;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    caller = find_function_decl_in_program(program, "caller");
    ASSERT(caller != NULL);
    ASSERT(caller->as.function_decl.body != NULL);
    ASSERT(caller->as.function_decl.body->statement_count == 1U);
    return_stmt = caller->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    call_expr = return_stmt->as.return_value;
    ASSERT(call_expr != NULL && call_expr->kind == FENG_EXPR_CALL);
    ASSERT(call_expr->as.call.arg_count == 1U);
    arg = call_expr->as.call.args[0];
    ASSERT(arg != NULL && arg->kind == FENG_EXPR_LAMBDA);

    site = feng_semantic_lookup_spec_coercion_site(analysis, arg);
    ASSERT(site != NULL);
    ASSERT(site->form == FENG_SPEC_COERCION_FORM_CALLABLE);
    ASSERT(site->callable_source == FENG_SPEC_COERCION_CALLABLE_SOURCE_LAMBDA);
    ASSERT(site->target_spec_decl == find_spec_decl_by_name(analysis, "Cb"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_callable_spec_value_rejects_different_spec_implicit_match(void) {
    const char *src =
        "open module demo.callable.nominal;\n"
        "spec A(x: int): int;\n"
        "spec B(x: int): int;\n"
        "func double(x: int): int { return x + x; }\n"
        "func caller(): int {\n"
        "    let a: A = double;\n"
        "    let b: B = a;\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("callable_spec_implicit_nominal_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "callable_spec_implicit_nominal_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "does not match expected function type 'B'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_callable_spec_value_explicit_cast_accepts_equal_signature(void) {
    const char *src =
        "open module demo.callable.nominal;\n"
        "spec A(x: int): int;\n"
        "spec B(x: int): int;\n"
        "func double(x: int): int { return x + x; }\n"
        "func caller(): int {\n"
        "    let a: A = double;\n"
        "    let b: B = (B)a;\n"
        "    return b(2);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("callable_spec_explicit_cast_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_callable_spec_top_level_fn_still_matches_multiple_specs(void) {
    const char *src =
        "open module demo.callable.nominal;\n"
        "spec A(x: int): int;\n"
        "spec B(x: int): int;\n"
        "func double(x: int): int { return x + x; }\n"
        "func caller(): int {\n"
        "    let a: A = double;\n"
        "    let b: B = double;\n"
        "    return a(1) + b(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("callable_spec_top_level_multi_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

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
        "open module demo.defaults;\n"
        "spec Named { func name(): string; }\n"
        "func make(): int {\n"
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
        "open module demo.defaults;\n"
        "spec Cb(x: int): int;\n"
        "func make(): int {\n"
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
        "open module demo.defaults;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
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
        "open module demo.access;\n"
        "spec Named { var n: string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "}\n"
        "func read_it(s: Named): string {\n"
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
        "open module demo.access;\n"
        "spec Named { var n: string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "}\n"
        "func write_it(s: Named) {\n"
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
        "open module demo.access;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func call_it(s: Named): string {\n"
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
        "open module demo.access;\n"
        "spec Cb(x: int): int;\n"
        "func use_it(c: Cb): int {\n"
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

/* --- Phase S3: SpecWitness sidecar tests (§9.5) ---------------------- */

static void test_spec_witness_via_declared_head(void) {
    /* §9.5 (1): T satisfies S via its own declared head; each S member
     * resolves to T's own implementation. */
    const char *src =
        "open module demo.witness;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func make(): int {\n"
        "    let s: Named = User{n: \"u\"};\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("witness_head.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    FengSemanticSubjectKey user_key = feng_semantic_subject_key_for_type_decl(user);
    const FengSpecWitness *w = feng_semantic_lookup_spec_witness(analysis, &user_key, named);
    ASSERT(w != NULL);
    ASSERT(w->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_TYPE_DECL);
    ASSERT(w->subject_key.as.type_decl == user);
    ASSERT(w->spec_decl == named);
    ASSERT(w->member_count == 1U);
    ASSERT(w->members[0].source_kind == FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD);
    ASSERT(w->members[0].impl_member != NULL);
    ASSERT(w->members[0].impl_member->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(w->members[0].via_fit_decl == NULL);
    ASSERT(w->members[0].provider_module == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_witness_via_fit(void) {
    /* §9.5 (2): T satisfies S only through a fit; the witness member
     * resolves to FIT_METHOD with the fit decl/module recorded. */
    const char *src =
        "open module demo.witness;\n"
        "spec Named { func name(): string; }\n"
        "type User { var n: string; }\n"
        "fit User: Named { func name(): string { return self.n; } }\n"
        "func make(): int {\n"
        "    let s: Named = User{n: \"u\"};\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("witness_fit.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    FengSemanticSubjectKey user_key = feng_semantic_subject_key_for_type_decl(user);
    const FengSpecWitness *w = feng_semantic_lookup_spec_witness(analysis, &user_key, named);
    ASSERT(w != NULL);
    ASSERT(w->member_count == 1U);
    ASSERT(w->members[0].source_kind == FENG_SPEC_WITNESS_SOURCE_FIT_METHOD);
    ASSERT(w->members[0].impl_member != NULL);
    ASSERT(w->members[0].impl_member->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(w->members[0].via_fit_decl != NULL);
    ASSERT(w->members[0].via_fit_decl->kind == FENG_DECL_FIT);
    ASSERT(w->members[0].provider_module != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_witness_field_member(void) {
    /* §9.5: a spec field maps to T's own field via TYPE_OWN_FIELD. */
    const char *src =
        "open module demo.witness;\n"
        "spec Named { var n: string; }\n"
        "type User: Named { var n: string; }\n"
        "func make(): int {\n"
        "    let s: Named = User{n: \"u\"};\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("witness_field.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    FengSemanticSubjectKey user_key = feng_semantic_subject_key_for_type_decl(user);
    const FengSpecWitness *w = feng_semantic_lookup_spec_witness(analysis, &user_key, named);
    ASSERT(w != NULL);
    ASSERT(w->member_count == 1U);
    ASSERT(w->members[0].source_kind == FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_FIELD);
    ASSERT(w->members[0].impl_member != NULL);
    ASSERT(w->members[0].impl_member->kind == FENG_TYPE_MEMBER_FIELD);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_witness_on_demand_only(void) {
    /* §8.2: with no coercion site, no witness is materialised — even
     * though (User, Named) has a SpecRelation entry. */
    const char *src =
        "open module demo.witness;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("witness_lazy.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    FengSemanticSubjectKey user_key = feng_semantic_subject_key_for_type_decl(user);
    /* Relation exists (declared head). */
    ASSERT(lookup_relation_for_type_decl(analysis, user, named) != NULL);
    /* Witness does not — no coercion site demanded it. */
    ASSERT(feng_semantic_lookup_spec_witness(analysis, &user_key, named) == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_witness_via_generic_instantiation(void) {
    /* Generic instantiation should demand the same object-form witness that
     * direct spec coercion sites already materialize. */
    const char *src =
        "open module demo.witness;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func greet<T: Named>(value: T): string {\n"
        "    return value.name();\n"
        "}\n"
        "func run(): string {\n"
        "    let user = User{n: \"u\"};\n"
        "    return greet(user);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("witness_generic_call.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *user = find_type_decl_by_name(analysis, "User");
    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    FengSemanticSubjectKey user_key = feng_semantic_subject_key_for_type_decl(user);

    ASSERT(lookup_relation_for_type_decl(analysis, user, named) != NULL);
    ASSERT(feng_semantic_lookup_spec_witness(analysis, &user_key, named) != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_witness_subject_key_supports_builtin_and_array(void) {
    const char *src =
        "open module demo.witness;\n"
        "spec Named { func name(): string; }\n"
        "func take(xs: i32[!]): int { return 0; }\n"
        "func take2(xs: i32[!]): int { return 0; }\n"
        "func take_ro(xs: i32[]): int { return 0; }\n"
        "func take2d(xs: i32[][]): int { return 0; }\n";
    FengProgram *program = parse_program_or_die("witness_subject_keys.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *named;
    const FengDecl *take_decl;
    const FengDecl *take2_decl;
    const FengDecl *take_ro_decl;
    const FengDecl *take2d_decl;
    const FengTypeRef *take_array_ref;
    const FengTypeRef *take2_array_ref;
    const FengTypeRef *take_ro_array_ref;
    const FengTypeRef *take2d_array_ref;
    FengSemanticSubjectKey builtin_key = feng_semantic_subject_key_for_builtin("i32");
    FengSemanticSubjectKey take_array_key;
    FengSemanticSubjectKey take2_array_key;
    FengSemanticSubjectKey take_ro_array_key;
    FengSemanticSubjectKey take2d_array_key;
    FengSpecWitness *builtin_witness;
    FengSpecWitness *array_witness;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    named = find_spec_decl_by_name(analysis, "Named");
    take_decl = find_function_decl_by_name(program, "take");
    take2_decl = find_function_decl_by_name(program, "take2");
    take_ro_decl = find_function_decl_by_name(program, "take_ro");
    take2d_decl = find_function_decl_by_name(program, "take2d");
    ASSERT(named != NULL);
    ASSERT(take_decl != NULL);
    ASSERT(take2_decl != NULL);
    ASSERT(take_ro_decl != NULL);
    ASSERT(take2d_decl != NULL);

    builtin_witness = feng_semantic_reserve_spec_witness(analysis, &builtin_key, named);
    ASSERT(builtin_witness != NULL);
    ASSERT(builtin_witness->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_BUILTIN);
    ASSERT(strcmp(builtin_witness->subject_key.as.builtin_canonical_name, "i32") == 0);
    ASSERT(feng_semantic_lookup_spec_witness(analysis, &builtin_key, named) == builtin_witness);

    take_array_ref = take_decl->as.function_decl.params[0].type;
    take2_array_ref = take2_decl->as.function_decl.params[0].type;
        take_ro_array_ref = take_ro_decl->as.function_decl.params[0].type;
        take2d_array_ref = take2d_decl->as.function_decl.params[0].type;
    ASSERT(feng_semantic_subject_key_init_array_from_type_ref(&take_array_key,
                                                              take_array_ref));
    ASSERT(feng_semantic_subject_key_init_array_from_type_ref(&take2_array_key,
                                                              take2_array_ref));
        ASSERT(feng_semantic_subject_key_init_array_from_type_ref(&take_ro_array_key,
                                          take_ro_array_ref));
        ASSERT(feng_semantic_subject_key_init_array_from_type_ref(&take2d_array_key,
                                          take2d_array_ref));
    array_witness = feng_semantic_reserve_spec_witness(analysis, &take_array_key, named);
    ASSERT(array_witness != NULL);
    ASSERT(array_witness->subject_key.kind == FENG_SEMANTIC_SUBJECT_KEY_ARRAY);
    ASSERT(array_witness->subject_key.as.array.rank == 1U);
    ASSERT(array_witness->subject_key.as.array.writable_mask == 1U);
    ASSERT(feng_semantic_lookup_spec_witness(analysis, &take2_array_key, named) ==
           array_witness);
        ASSERT(feng_semantic_lookup_spec_witness(analysis, &take_ro_array_key, named) == NULL);
        ASSERT(feng_semantic_lookup_spec_witness(analysis, &take2d_array_key, named) == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_witness_multi_fit_conflict(void) {
    /* §8.1: two visible fits both provide the same (name, params, return)
     * implementation of S's method on T → conflict at first coercion. */
    const char *src =
        "open module demo.witness;\n"
        "spec Named { func name(): string; }\n"
        "type User { var n: string; }\n"
        "fit User: Named { func name(): string { return self.n; } }\n"
        "fit User: Named { func name(): string { return \"x\"; } }\n"
        "func make(): int {\n"
        "    let s: Named = User{n: \"u\"};\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("witness_conflict.f", src);
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

/* --- Phase S4 SpecEquality (§9.6) ----------------------------------- */

/* Walk the program and return the first FENG_EXPR_BINARY whose op matches
 * `op`. The §9.6 cases use a single binary expression in `make()`, so a
 * linear scan over the function's block is sufficient. */
static const FengExpr *find_binary_op_in_block(const FengBlock *block, FengTokenKind op);

static const FengExpr *find_binary_op_in_expr(const FengExpr *expr, FengTokenKind op) {
    if (expr == NULL) {
        return NULL;
    }
    if (expr->kind == FENG_EXPR_BINARY && expr->as.binary.op == op) {
        return expr;
    }
    if (expr->kind == FENG_EXPR_BINARY) {
        const FengExpr *l = find_binary_op_in_expr(expr->as.binary.left, op);
        if (l != NULL) return l;
        return find_binary_op_in_expr(expr->as.binary.right, op);
    }
    if (expr->kind == FENG_EXPR_UNARY) {
        return find_binary_op_in_expr(expr->as.unary.operand, op);
    }
    return NULL;
}

static const FengExpr *find_binary_op_in_block(const FengBlock *block, FengTokenKind op) {
    if (block == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < block->statement_count; ++i) {
        const FengStmt *stmt = block->statements[i];
        if (stmt == NULL) continue;
        const FengExpr *e = NULL;
        switch (stmt->kind) {
            case FENG_STMT_EXPR:
                e = stmt->as.expr;
                break;
            case FENG_STMT_RETURN:
                e = stmt->as.return_value;
                break;
            case FENG_STMT_BINDING:
                e = stmt->as.binding.initializer;
                break;
            case FENG_STMT_ASSIGN: {
                const FengExpr *t = find_binary_op_in_expr(stmt->as.assign.target, op);
                if (t != NULL) return t;
                e = stmt->as.assign.value;
                break;
            }
            default:
                break;
        }
        const FengExpr *found = find_binary_op_in_expr(e, op);
        if (found != NULL) return found;
    }
    return NULL;
}

static const FengExpr *find_first_binary_op_in_decls(const FengProgram *program,
                                                     FengTokenKind op) {
    if (program == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < program->declaration_count; ++i) {
        const FengDecl *d = program->declarations[i];
        if (d == NULL || d->kind != FENG_DECL_FUNCTION) continue;
        const FengExpr *found = find_binary_op_in_block(d->as.function_decl.body, op);
        if (found != NULL) return found;
    }
    return NULL;
}

static void test_spec_equality_object_eq_recorded(void) {
    /* §9.6: spec-typed operands on `==` are classified as reference
     * identity comparison; the binary expr is recorded with op=EQ and
     * spec_decl pointing at the spec. */
    const char *src =
        "open module demo.eq;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func make(): bool {\n"
        "    let a: Named = User{n: \"u\"};\n"
        "    let b: Named = User{n: \"u\"};\n"
        "    return a == b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("eq_object.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengExpr *bin = find_first_binary_op_in_decls(program, FENG_TOKEN_EQ);
    ASSERT(bin != NULL);
    const FengSpecEquality *e = feng_semantic_lookup_spec_equality(analysis, bin);
    ASSERT(e != NULL);
    ASSERT(e->op == FENG_SPEC_EQUALITY_OP_EQ);
    ASSERT(e->spec_decl != NULL);
    ASSERT(e->spec_decl->kind == FENG_DECL_SPEC);
    ASSERT(e->spec_decl == find_spec_decl_by_name(analysis, "Named"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_equality_object_neq_recorded(void) {
    /* §9.6: `!=` on spec-typed operands is recorded with op=NE; the
     * conclusion is still reference-identity comparison, only the boolean
     * polarity differs. */
    const char *src =
        "open module demo.eq;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func make(): bool {\n"
        "    let a: Named = User{n: \"u\"};\n"
        "    let b: Named = User{n: \"u\"};\n"
        "    return a != b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("neq_object.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengExpr *bin = find_first_binary_op_in_decls(program, FENG_TOKEN_NE);
    ASSERT(bin != NULL);
    const FengSpecEquality *e = feng_semantic_lookup_spec_equality(analysis, bin);
    ASSERT(e != NULL);
    ASSERT(e->op == FENG_SPEC_EQUALITY_OP_NE);
    ASSERT(e->spec_decl == find_spec_decl_by_name(analysis, "Named"));

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_equality_string_not_recorded(void) {
    /* §9.6 negative: `==` on `string` operands is the value-equality
     * path; no SpecEquality entry is recorded. This protects the existing
     * string equality semantics (regression guard for §9.7). */
    const char *src =
        "open module demo.eq;\n"
        "func make(): bool {\n"
        "    let a: string = \"u\";\n"
        "    let b: string = \"u\";\n"
        "    return a == b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("eq_string.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengExpr *bin = find_first_binary_op_in_decls(program, FENG_TOKEN_EQ);
    ASSERT(bin != NULL);
    /* No spec equality recorded — string == string stays on its own
     * value-equality path. */
    ASSERT(feng_semantic_lookup_spec_equality(analysis, bin) == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_spec_equality_int_not_recorded(void) {
    /* §9.6 negative: `==` on builtin numeric operands has no SpecEquality
     * entry. Sanity check — only spec-typed operands trigger recording. */
    const char *src =
        "open module demo.eq;\n"
        "func make(): bool {\n"
        "    let a: int = 1;\n"
        "    let b: int = 1;\n"
        "    return a == b;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("eq_int.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengExpr *bin = find_first_binary_op_in_decls(program, FENG_TOKEN_EQ);
    ASSERT(bin != NULL);
    ASSERT(feng_semantic_lookup_spec_equality(analysis, bin) == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* --- Value-kind classification tests (dev/feng-value-model-delivered.md §6.1) --- */

static const FengDecl *find_enum_decl_by_name(
        const FengSemanticAnalysis *analysis, const char *name) {
    size_t name_len = strlen(name);
    for (size_t mi = 0U; mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        for (size_t pi = 0U; pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (size_t di = 0U; di < prog->declaration_count; ++di) {
                const FengDecl *d = prog->declarations[di];
                if (d->kind != FENG_DECL_ENUM) continue;
                const FengSlice *n = &d->as.enum_decl.name;
                if (n->length == name_len &&
                    memcmp(n->data, name, name_len) == 0) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

static FengSlice slice_from_cstr(const char *literal) {
    FengSlice s = { literal, strlen(literal) };
    return s;
}

static void test_enum_info_tracks_implicit_values(void) {
    const char *src =
        "open module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound,\n"
        "    InternalError\n"
        "}\n"
        "func pick(): Status {\n"
        "    return Status.NotFound;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_info_implicit.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *status;
    const FengSemanticEnumInfo *info;
    const FengSemanticEnumItemInfo *ok_info;
    const FengSemanticEnumItemInfo *missing_info;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    status = find_enum_decl_by_name(analysis, "Status");
    ASSERT(status != NULL);
    info = feng_semantic_lookup_enum_info(analysis, status);
    ASSERT(info != NULL);
    ASSERT(info->first_item == &status->as.enum_decl.items[0]);
    ASSERT(info->first_value == 0);
    ASSERT(info->item_count == 3U);

    ok_info = feng_semantic_find_enum_item_info(analysis, status, slice_from_cstr("NotFound"));
    ASSERT(ok_info != NULL);
    ASSERT(ok_info->ordinal == 1U);
    ASSERT(ok_info->value == 1);

    missing_info = feng_semantic_find_enum_item_info(analysis, status, slice_from_cstr("Missing"));
    ASSERT(missing_info == NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_enum_info_tracks_explicit_values_and_cast_to_int(void) {
    const char *src =
        "open module demo.enums;\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "func code(): int {\n"
        "    return (int)HttpStatus.NotFound;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_info_explicit.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *status;
    const FengSemanticEnumInfo *info;
    const FengSemanticEnumItemInfo *item_info;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    status = find_enum_decl_by_name(analysis, "HttpStatus");
    ASSERT(status != NULL);
    info = feng_semantic_lookup_enum_info(analysis, status);
    ASSERT(info != NULL);
    ASSERT(info->first_item == &status->as.enum_decl.items[0]);
    ASSERT(info->first_value == 200);

    item_info = feng_semantic_find_enum_item_info(analysis, status, slice_from_cstr("NotFound"));
    ASSERT(item_info != NULL);
    ASSERT(item_info->ordinal == 1U);
    ASSERT(item_info->value == 404);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_enum_rejects_mixed_explicit_and_implicit_values(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok = 200,\n"
        "    NotFound\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_mixed_values_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_mixed_values_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message,
                  "cannot mix explicit and implicit item values") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_rejects_duplicate_item_name(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Ok\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_duplicate_name_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_duplicate_name_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "duplicate item name 'Ok'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_rejects_duplicate_item_value(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok = 200,\n"
        "    AlsoOk = 200\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_duplicate_value_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_duplicate_value_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message, "duplicate item value 200") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_rejects_int_to_enum_cast(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n"
        "func bad(): Status {\n"
        "    return (Status)1;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_cast_from_int_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_cast_from_int_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message, "to 'Status' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_relational_compare_is_rejected(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n"
        "func bad(): bool {\n"
        "    return Status.Ok < Status.NotFound;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_relational_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_relational_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '<' requires operands of the same numeric type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_is_valid_in_ordinary_type_positions(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n"
        "type Packet {\n"
        "    var status: Status;\n"
        "}\n"
        "func echo(status: Status): Status {\n"
        "    return status;\n"
        "}\n"
        "func run(history: Status[]): Status {\n"
        "    let current: Status = Status.Ok;\n"
        "    let packet: Packet = Packet { status: current };\n"
        "    let picked: Status = history[0];\n"
        "    if packet.status == current {\n"
        "        return echo(picked);\n"
        "    }\n"
        "    return current;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_ordinary_positions_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_enum_rejects_different_enum_assignment(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status { Ok, NotFound }\n"
        "enum Color { Red, Blue }\n"
        "func bad(): Status {\n"
        "    return Color.Red;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_cross_assign_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_cross_assign_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "does not match expected type 'Status'") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_rejects_different_enum_equality_compare(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status { Ok, NotFound }\n"
        "enum Color { Red, Blue }\n"
        "func bad(): bool {\n"
        "    return Status.Ok == Color.Red;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_cross_compare_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_cross_compare_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '==' requires operands of the same type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_rejects_different_enum_cast(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status { Ok, NotFound }\n"
        "enum Color { Red, Blue }\n"
        "func bad(): Color {\n"
        "    return (Color)Status.Ok;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_cross_cast_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_cross_cast_error.f") == 0);
    ASSERT(errors[0].token.line == 5U);
    ASSERT(strstr(errors[0].message, "cast from 'Status' to 'Color' is not allowed") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_arithmetic_is_rejected(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status { Ok, NotFound }\n"
        "func bad(): Status {\n"
        "    return Status.Ok + Status.NotFound;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_arithmetic_error.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "enum_arithmetic_error.f") == 0);
    ASSERT(errors[0].token.line == 4U);
    ASSERT(strstr(errors[0].message,
                  "binary operator '+' requires operands of the same numeric or string type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_enum_abi_surfaces_accept_enum_signatures(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "@cdecl(\"m\")\n"
        "extern func fetch(status: Status): Status;\n"
        "@abi\n"
        "func publish(status: Status): Status {\n"
        "    return status;\n"
        "}\n"
        "@abi\n"
        "spec StatusCb(status: Status): Status;\n"
        "@abi\n"
        "type Packet {\n"
        "    var status: Status;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_abi_surfaces_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_enum_address_of_matches_int_pointer_rules(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n"
        "@cdecl(\"m\")\n"
        "extern func use_status_ptr(status: Status*): void;\n"
        "func run(history: Status[]): Status {\n"
        "    let current: Status = history[0];\n"
        "    let ptr: Status* = &current;\n"
        "    use_status_ptr(ptr);\n"
        "    return current;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("enum_address_of_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_extern_function_accepts_enum_types(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "@cdecl(\"m\")\n"
        "extern func fetch(status: Status): Status;\n";
    FengProgram *program = parse_program_or_die("extern_enum_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_abi_type_accepts_enum_field(void) {
    const char *src =
        "module demo.enums;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n"
        "@abi\n"
        "type Packet {\n"
        "    var status: Status;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("abi_enum_field_ok.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_kind_enum_is_trivial(void) {
    const char *src =
        "open module demo.vk;\n"
        "enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n";
    FengProgram *program = parse_program_or_die("vk_enum.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengDecl *status;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    status = find_enum_decl_by_name(analysis, "Status");
    ASSERT(status != NULL);
    ASSERT(feng_semantic_value_kind_of_decl(status) == FENG_SEMANTIC_VALUE_TRIVIAL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_kind_builtin_classifies_string_as_managed_pointer(void) {
    ASSERT(feng_semantic_value_kind_of_builtin(slice_from_cstr("string")) ==
           FENG_SEMANTIC_VALUE_MANAGED_POINTER);
}

static void test_value_kind_builtin_classifies_numerics_and_bool_as_trivial(void) {
    static const char *names[] = {
        "i8",  "i16",  "i32",  "i64",
        "u8",  "u16",  "u32",  "u64",
        "f32", "f64",
        "int", "byte", "float", "double",
        "bool"
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        ASSERT(feng_semantic_value_kind_of_builtin(slice_from_cstr(names[i])) ==
               FENG_SEMANTIC_VALUE_TRIVIAL);
    }
}


    static void test_pu_builtin_self_fit_visible_after_use_enables_method_call(void) {
        const char *src_adapter =
            "open module demo.adapter;\n"
            "open fit string {\n"
            "    open func length(): i64 {\n"
            "        return 7;\n"
            "    }\n"
            "}\n";
        const char *src_consumer =
            "open module demo.consumer;\n"
            "import demo.adapter;\n"
            "func run(): i64 {\n"
            "    return \"abc\".length();\n"
            "}\n";
        FengProgram *p1 = parse_program_or_die("builtin_fit_adapter.f", src_adapter);
        FengProgram *p2 = parse_program_or_die("builtin_fit_consumer.f", src_consumer);
        const FengProgram *programs[] = {p1, p2};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        ASSERT(feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB,
                                     &analysis, &errors, &error_count));
        ASSERT(error_count == 0U);

        feng_semantic_analysis_free(analysis);
        feng_program_free(p1);
        feng_program_free(p2);
    }

    static void test_external_imported_builtin_self_fit_method_visible(void) {
        const char *external_source =
            "open module vendor.text;\n"
            "open fit string {\n"
            "    open func length(): i64 {\n"
            "        return 7;\n"
            "    }\n"
            "}\n";
        const char *main_source =
            "module demo.main;\n"
            "import vendor.text;\n"
            "func run(): i64 {\n"
            "    return \"abc\".length();\n"
            "}\n";
        ImportedSourceFixture fixture;
        FengSemanticImportedModuleQuery query;
        FengSemanticAnalyzeOptions options;
        FengProgram *program = NULL;
        const FengProgram *programs[1];
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        imported_source_fixture_init(&fixture, "external_text.ff", external_source);
        query = feng_symbol_imported_module_cache_as_query(fixture.cache);
        options.target = FENG_COMPILE_TARGET_LIB;
        options.imported_modules = &query;
        options.pointer_size = sizeof(void *);

        program = parse_program_or_die("external_text_main.f", main_source);
        programs[0] = program;
        ASSERT(feng_semantic_analyze_with_options(programs,
                                                  1U,
                                                  &options,
                                                  &analysis,
                                                  &errors,
                                                  &error_count));
        ASSERT(analysis != NULL);
        ASSERT(errors == NULL);
        ASSERT(error_count == 0U);

        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        imported_source_fixture_dispose(&fixture);
    }

    static void test_external_imported_array_self_fit_method_visible(void) {
        const char *external_source =
            "open module vendor.text;\n"
            "open fit T[] {\n"
            "    open func length(): i64 {\n"
            "        return 7;\n"
            "    }\n"
            "}\n"
            "open fit T[!] {\n"
            "    open func length(): i64 {\n"
            "        return 9;\n"
            "    }\n"
            "}\n";
        const char *main_source =
            "module demo.main;\n"
            "import vendor.text;\n"
            "func mutable_len(): i64 {\n"
            "    let values: int[] = [1, 2, 3];\n"
            "    return values.length();\n"
            "}\n"
            "func readonly_len(): i64 {\n"
            "    let values: int[!] = [1, 2, 3];\n"
            "    return values.length();\n"
            "}\n";
        ImportedSourceFixture fixture;
        FengSemanticImportedModuleQuery query;
        FengSemanticAnalyzeOptions options;
        FengProgram *program = NULL;
        const FengProgram *programs[1];
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        imported_source_fixture_init(&fixture, "external_array_length.ff", external_source);
        query = feng_symbol_imported_module_cache_as_query(fixture.cache);
        options.target = FENG_COMPILE_TARGET_LIB;
        options.imported_modules = &query;
        options.pointer_size = sizeof(void *);

        program = parse_program_or_die("external_array_length_main.f", main_source);
        programs[0] = program;
        ASSERT(feng_semantic_analyze_with_options(programs,
                                                  1U,
                                                  &options,
                                                  &analysis,
                                                  &errors,
                                                  &error_count));
        ASSERT(analysis != NULL);
        ASSERT(errors == NULL);
        ASSERT(error_count == 0U);

        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        imported_source_fixture_dispose(&fixture);
    }
static void test_value_kind_builtin_classifies_void_as_trivial(void) {
    ASSERT(feng_semantic_value_kind_of_builtin(slice_from_cstr("void")) ==
           FENG_SEMANTIC_VALUE_TRIVIAL);
}

static void test_value_kind_builtin_unknown_name_defaults_to_trivial(void) {
    /* The analyzer rejects unknown built-in spellings before this point;
     * the helper itself must still degrade safely so that a programmer
     * mistake never silently promotes an unknown name into the ARC path. */
    ASSERT(feng_semantic_value_kind_of_builtin(slice_from_cstr("notatype")) ==
           FENG_SEMANTIC_VALUE_TRIVIAL);
    FengSlice empty = { NULL, 0U };
    ASSERT(feng_semantic_value_kind_of_builtin(empty) ==
           FENG_SEMANTIC_VALUE_TRIVIAL);
}

static void test_value_kind_user_type_is_managed_pointer(void) {
    const char *src =
        "open module demo.vk;\n"
        "type Holder { let id: int; }\n";
    FengProgram *program = parse_program_or_die("vk_type.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *holder = find_type_decl_by_name(analysis, "Holder");
    ASSERT(holder != NULL);
    ASSERT(feng_semantic_value_kind_of_decl(holder) ==
           FENG_SEMANTIC_VALUE_MANAGED_POINTER);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_kind_object_form_spec_is_aggregate(void) {
    const char *src =
        "open module demo.vk;\n"
        "spec Named { let name: string; }\n";
    FengProgram *program = parse_program_or_die("vk_obj_spec.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *named = find_spec_decl_by_name(analysis, "Named");
    ASSERT(named != NULL);
    ASSERT(named->as.spec_decl.form == FENG_SPEC_FORM_OBJECT);
    ASSERT(feng_semantic_value_kind_of_decl(named) ==
           FENG_SEMANTIC_VALUE_AGGREGATE);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_kind_callable_form_spec_is_managed_pointer(void) {
    const char *src =
        "open module demo.vk;\n"
        "spec Greet(name: string): string;\n";
    FengProgram *program = parse_program_or_die("vk_call_spec.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *greet = find_spec_decl_by_name(analysis, "Greet");
    ASSERT(greet != NULL);
    ASSERT(greet->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE);
    ASSERT(feng_semantic_value_kind_of_decl(greet) ==
           FENG_SEMANTIC_VALUE_MANAGED_POINTER);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_value_kind_null_decl_is_trivial(void) {
    /* Defensive: passing NULL must not crash and must not promote into
     * the managed/aggregate paths. */
    ASSERT(feng_semantic_value_kind_of_decl(NULL) ==
           FENG_SEMANTIC_VALUE_TRIVIAL);
}

static void test_value_kind_non_type_decl_is_trivial(void) {
    /* A FENG_DECL_FUNCTION (or any non-type/non-spec decl) is not a type
     * and has no value kind; the helper degrades to TRIVIAL rather than
     * lying. */
    const char *src =
        "open module demo.vk;\n"
        "func helper() { }\n";
    FengProgram *program = parse_program_or_die("vk_fn.f", src);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    const FengDecl *fn_decl = NULL;
    for (size_t mi = 0U; fn_decl == NULL && mi < analysis->module_count; ++mi) {
        const FengSemanticModule *mod = &analysis->modules[mi];
        for (size_t pi = 0U; fn_decl == NULL && pi < mod->program_count; ++pi) {
            const FengProgram *prog = mod->programs[pi];
            for (size_t di = 0U; di < prog->declaration_count; ++di) {
                if (prog->declarations[di]->kind == FENG_DECL_FUNCTION) {
                    fn_decl = prog->declarations[di];
                    break;
                }
            }
        }
    }
    ASSERT(fn_decl != NULL);
    ASSERT(feng_semantic_value_kind_of_decl(fn_decl) ==
           FENG_SEMANTIC_VALUE_TRIVIAL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* ───────────────────────────────── G4: Generics ─────────────────────────── */

static void test_generic_function_decl_ok(void) {
    /* A generic function with a single type parameter in both the parameter
     * and the return type must analyse without errors. */
    const char *source =
        "module demo.main;\n"
        "func identity<T>(x: T): T {\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_fn_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_type_decl_ok(void) {
    /* A generic type with a single type parameter used in a field type must
     * analyse without errors. */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_type_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_spec_decl_ok(void) {
    /* A generic spec with a single type parameter used in a method return type
     * must analyse without errors. */
    const char *source =
        "module demo.main;\n"
        "spec Container<T> {\n"
        "    func fetch(): T;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_spec_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_function_call_wildcard_ok(void) {
    /* Calling a generic function whose type parameter position matches any
     * argument type (wildcard matching, G4-12) must succeed.  The result
     * binding is untyped so no return-type inference is required. */
    const char *source =
        "module demo.main;\n"
        "func identity<T>(x: T): T {\n"
        "    return x;\n"
        "}\n"
        "func check(): void {\n"
        "    let result = identity(42);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_call_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_explicit_type_args_ok(void) {
    /* Explicit type arguments with the correct arity must be accepted. */
    const char *source =
        "module demo.main;\n"
        "func identity<T>(x: T): T {\n"
        "    return x;\n"
        "}\n"
        "func check(): void {\n"
        "    let result = identity<int>(42);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_explicit_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_exact_non_generic_overload_is_preferred(void) {
    const char *source =
        "module demo.main;\n"
        "func pick(value: int): string {\n"
        "    return \"exact\";\n"
        "}\n"
        "func pick<T>(value: T): T {\n"
        "    return value;\n"
        "}\n"
        "type Picker {\n"
        "    func choose(value: int): string {\n"
        "        return \"exact\";\n"
        "    }\n"
        "    func choose<T>(value: T): T {\n"
        "        return value;\n"
        "    }\n"
        "    static func make(value: int): string {\n"
        "        return \"exact\";\n"
        "    }\n"
        "    static func make<T>(value: T): T {\n"
        "        return value;\n"
        "    }\n"
        "}\n"
        "fit string {\n"
        "    static func formatLike(value: int): string {\n"
        "        return \"exact\";\n"
        "    }\n"
        "    static func formatLike<T>(value: T): T {\n"
        "        return value;\n"
        "    }\n"
        "}\n"
        "func check(): void {\n"
        "    let topExact: string = pick(1);\n"
        "    let topGeneric: int = pick<int>(1);\n"
        "    let picker: Picker = Picker();\n"
        "    let memberExact: string = picker.choose(1);\n"
        "    let memberGeneric: int = picker.choose<int>(1);\n"
        "    let staticExact: string = Picker.make(1);\n"
        "    let staticGeneric: int = Picker.make<int>(1);\n"
        "    let fitExact: string = string.formatLike(1);\n"
        "    let fitGeneric: int = string.formatLike<int>(1);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_exact_overload_preferred.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_type_param_constraint_must_be_spec(void) {
    /* A type parameter constraint that names a *type* (not a spec) must be
     * rejected.  Only specs are legal as constraints. */
    const char *source =
        "module demo.main;\n"
        "type MyType {}\n"
        "func process<T: MyType>(x: T): void {}\n";
    FengProgram *program = parse_program_or_die("gen_bad_constraint.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "constraint must be a spec") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_type_ref_arity_too_many(void) {
    /* Supplying more type arguments than a generic type declares must be an
     * error (G4-7 arity check). */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "}\n"
        "func process(b: Box<int, bool>): void {}\n";
    FengProgram *program = parse_program_or_die("gen_arity_many.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "type argument") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_non_generic_type_with_type_args_rejected(void) {
    /* Supplying type arguments to a non-generic type must be an error. */
    const char *source =
        "module demo.main;\n"
        "type Plain {}\n"
        "func process(p: Plain<int>): void {}\n";
    FengProgram *program = parse_program_or_die("gen_non_generic.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "not a generic type") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_type_with_finalizer_rejected(void) {
    /* A generic type cannot declare a finalizer (G4-18). */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "    func ~Box() {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_fin.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "finalizer") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_explicit_type_args_adapts_tuple_literal(void) {
    /* Tuple literal arg must be adapted to the explicit type arg when
     * the type param position receives a named tuple type. */
    const char *source =
        "module demo.main;\n"
        "type Pair(i32, i32);\n"
        "func identity<T>(x: T): T {\n"
        "    return x;\n"
        "}\n"
        "func run(): Pair {\n"
        "    return identity<Pair>((1, 2));\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_tuple_adapt.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_explicit_type_args_adapts_lambda_literal(void) {
    /* Lambda literal arg must be adapted to the explicit type arg when
     * the type param position receives a callable spec type. */
    const char *source =
        "module demo.main;\n"
        "spec Handler(id: int): void;\n"
        "func invoke<H>(handler: H): void {\n"
        "}\n"
        "func noop(): void {\n"
        "}\n"
        "func check(): void {\n"
        "    invoke<Handler>((id: int) -> noop());\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_lambda_adapt.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_explicit_type_args_arity_mismatch(void) {
    /* Providing the wrong number of explicit type arguments must be rejected
     * (G4-13 arity check). */
    const char *source =
        "module demo.main;\n"
        "func identity<T>(x: T): T {\n"
        "    return x;\n"
        "}\n"
        "func check(): void {\n"
        "    let result = identity<int, bool>(42);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_explicit_bad.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "type argument") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_type_constructor_explicit_type_args_ok(void) {
    /* Type<T>() constructor with correct arity must be accepted (G4-13b). */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "}\n"
        "func run(): void {\n"
        "    let b = Box<int>();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_ctor_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_type_constructor_explicit_type_args_arity_mismatch(void) {
    /* Type<T1, T2>() on a 1-param type must be rejected with an arity error. */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "}\n"
        "func run(): void {\n"
        "    let b = Box<int, string>();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_ctor_bad.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "type argument") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_method_type_param_collides_with_type_param(void) {
    /* A method may not reuse the same type parameter name as its enclosing
     * type (G4-14 collision check). */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "    func transform<T>(): void {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_shadow.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strstr(errors[0].message, "shadows") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* G7 semantic additions */

static void test_generic_function_two_type_params_ok(void) {
    /* 正确语法七: fn pair<T, U>(a: T, b: U) infers both params from args. */
    const char *source =
        "module demo.main;\n"
        "func pair<T, U>(a: T, b: U): void {}\n"
        "func check(): void {\n"
        "    pair(1, \"hello\");\n"
        "    pair(true, 42);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_pair.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_spec_generic_parent_forwarding_ok(void) {
    /* 正确语法四 (simplified): spec MyList: List<int> — using concrete type arg
     * in parent spec is valid. */
    const char *source =
        "module demo.main;\n"
        "spec Sequence<T> {\n"
        "    func size(): int;\n"
        "}\n"
        "spec IntSequence: Sequence<int> {\n"
        "    func size(): int;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_parent_fwd.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_generic_duplicate_fn_by_type_param_name_only_rejected(void) {
    /* 错误语法五: fn foo<T>() and fn foo<U>() differ only in type param name —
     * they have identical effective signatures and must be rejected.
     * Verified as ambiguous (or duplicate) at call site. */
    const char *source =
        "module demo.main;\n"
        "func foo<T>(): void {}\n"
        "func foo<U>(): void {}\n"
        "func check(): void {\n"
        "    foo();\n"
        "}\n";
    FengProgram *program = parse_program_or_die("gen_dup_fn.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    /* Both overloads match with 0 args → ambiguous. */
    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_generic_same_name_same_arity_different_constraint_rejected(void) {
    /* 错误语法十二: type Foo<T: SpecA> and type Foo<T: SpecB> share the same
     * identity (name + arity) and must be rejected as duplicate declarations. */
    const char *source =
        "module demo.main;\n"
        "spec SpecA {}\n"
        "spec SpecB {}\n"
        "type Foo<T: SpecA> { open let value: int; }\n"
        "type Foo<T: SpecB> { open let value: int; }\n";
    FengProgram *program = parse_program_or_die("gen_dup_type.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* T1: a variadic-only function must accept 0 / 1 / N arguments. */
static void test_variadic_accepts_zero_one_many_arguments(void) {
    const char *source =
        "module demo.main;\n"
        "func sum(args: int...): int {\n"
        "    return 0;\n"
        "}\n"
        "func run(): int {\n"
        "    let zero = sum();\n"
        "    let one = sum(1);\n"
        "    return sum(zero, one, 3);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_zero_one_many_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* T2: fixed arguments remain positional, variadic suffix accepts zero or many elements. */
static void test_fixed_and_variadic_parameters_accept_calls(void) {
    const char *source =
        "module demo.main;\n"
        "func log(level: int, args: string...): int {\n"
        "    return level;\n"
        "}\n"
        "func run(): int {\n"
        "    let base = log(0);\n"
        "    return log(base, \"a\", \"b\");\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_fixed_prefix_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* T3: each variadic element must match the variadic element type. */
static void test_variadic_rejects_mismatched_element_type(void) {
    const char *source =
        "module demo.main;\n"
        "func f(args: int...): void {\n"
        "    return;\n"
        "}\n"
        "func run(): void {\n"
        "    f(\"bad\");\n"
        "    return;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_type_mismatch_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "variadic_type_mismatch_error.f") == 0);
    ASSERT(errors[0].token.line == 6U);
    ASSERT(strstr(errors[0].message,
                  "top-level function 'f' has no overload accepting 1 argument(s)") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* T5: an existing T[] value cannot be passed directly into a variadic position. */
static void test_variadic_rejects_existing_array_argument(void) {
    const char *source =
        "module demo.main;\n"
        "func f(args: int...): void {\n"
        "    return;\n"
        "}\n"
        "func run(): void {\n"
        "    let arr: int[] = [1, 2];\n"
        "    f(arr);\n"
        "    return;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_existing_array_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "variadic_existing_array_error.f") == 0);
    ASSERT(errors[0].token.line == 7U);
    ASSERT(strstr(errors[0].message,
                  "does not accept an existing array at a variadic argument position") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* T6: variadic callable-form specs accept variadic lambdas and remain callable through the spec value. */
static void test_variadic_callable_spec_lambda_call_ok(void) {
    const char *source =
        "module demo.main;\n"
        "spec Printer(args: int...): int;\n"
        "func run(): int {\n"
        "    let printer: Printer = (args: int...) {\n"
        "        return 0;\n"
        "    };\n"
        "    return printer(1, 2);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_callable_spec_lambda_ok.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Variadic overload conflict — fn foo(x: int, y: int...) conflicts with
 * fn foo(x: int, y: int) because the variadic can be called with 2 fixed args. */
static void test_variadic_overload_conflict_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func foo(x: int, y: int...): int { return x; }\n"
        "func foo(x: int, y: int): int { return y; }\n"
        "func check(): int { return foo(1, 2); }\n";
    FengProgram *program = parse_program_or_die("variadic_conflict.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* T7: fn f(x: int) conflicts with fn f(args: int...) at declaration time. */
static void test_variadic_single_fixed_and_variadic_overload_conflict_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func f(x: int): void {\n"
        "    return;\n"
        "}\n"
        "func f(args: int...): void {\n"
        "    return;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_single_conflict.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].path, "variadic_single_conflict.f") == 0);
    ASSERT(strstr(errors[0].message,
                  "variadic function overload conflicts with existing overload") != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Variadic spec satisfaction requires matching variadic flag — a spec with a
 * variadic method cannot be satisfied by a non-variadic implementation. */
static void test_variadic_spec_satisfaction_mismatch_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "spec Logger {\n"
        "    func log(values: int...): void;\n"
        "}\n"
        "type Console {}\n"
        "fit Console: Logger {\n"
        "    func log(values: int[]): void {}\n"
        "}\n";
    FengProgram *program = parse_program_or_die("variadic_spec.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void assert_single_source_semantic_ok(const char *path, const char *source) {
    FengProgram *program = parse_program_or_die(path, source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void assert_single_source_semantic_error_contains(const char *path,
                                                         const char *source,
                                                         const char *expected) {
    FengProgram *program = parse_program_or_die(path, source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool found = false;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                  &analysis, &errors, &error_count));
    ASSERT(error_count >= 1U);
    for (size_t index = 0U; index < error_count; ++index) {
        if (strstr(errors[index].message, expected) != NULL) {
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "expected semantic error containing: %s\n", expected);
        for (size_t index = 0U; index < error_count; ++index) {
            fprintf(stderr, "actual semantic error: %s\n", errors[index].message);
        }
    }
    ASSERT(found);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* ===== infix match operator tests ===== */

static void test_infix_match_value_pattern_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return x match 0;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_value_pattern.f", source);
}

static void test_infix_match_range_pattern_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return x match 1...10;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_range_pattern.f", source);
}

static void test_infix_match_multi_label_pipe_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return x match 0 | 1 | 2;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_multi_label_pipe.f", source);
}

static void test_infix_match_mixed_value_and_range_pipe_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return x match 0 | 1...9 | 100;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_mixed_value_range_pipe.f", source);
}

static void test_infix_match_string_pattern_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "func run(s: string): bool {\n"
        "    return s match \"hi\" | \"hello\";\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_string_pattern.f", source);
}

static void test_infix_match_union_member_type_pattern_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): bool {\n"
        "    return v match int;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_union_member_type.f", source);
}

static void test_infix_match_union_multi_label_type_pattern_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): bool {\n"
        "    return v match int | string;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_union_multi_label_type.f", source);
}

static void test_infix_match_union_member_binding_in_if_body_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): int {\n"
        "    if v match n: int {\n"
        "        return n;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_union_binding_if_body.f", source);
}

static void test_infix_match_union_member_binding_in_while_body_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): int {\n"
        "    var counter: Value = v;\n"
        "    var sum = 0;\n"
        "    while counter match n: int && n > 0 {\n"
        "        sum = sum + n;\n"
        "        counter = n - 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_union_binding_while_body.f", source);
}

static void test_infix_match_union_member_binding_visible_in_rhs_of_and(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): bool {\n"
        "    return v match n: int && n > 0;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_union_binding_rhs_and.f", source);
}

static void test_infix_match_union_member_binding_invisible_after_statement(void) {
    /* Binding n is only visible inside the if body; referencing it after
     * the if statement should fail with AE0001 (undefined identifier). */
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): int {\n"
        "    if v match n: int {\n"
        "        return n;\n"
        "    }\n"
        "    return n;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_union_binding_invisible_after_stmt.f",
        source,
        "undefined identifier");
}

static void test_infix_match_union_member_binding_invisible_in_or_operand(void) {
    /* `||` nephew does not propagate match bindings. */
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): bool {\n"
        "    return (v match n: int) || n > 0;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_union_binding_invisible_in_or.f",
        source,
        "undefined identifier");
}

static void test_infix_match_rejects_binding_on_value_pattern(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return x match n: 0;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_binding_on_value_pattern.f",
        source,
        "infix match binding requires all labels to be union member type patterns");
}

static void test_infix_match_rejects_binding_on_range_pattern(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return x match n: 1...10;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_binding_on_range_pattern.f",
        source,
        "infix match binding requires all labels to be union member type patterns");
}

static void test_infix_match_rejects_binding_with_mixed_type_and_value_labels(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(v: Value): bool {\n"
        "    return v match n: int | 0;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_binding_mixed_type_value.f",
        source,
        "infix match binding requires all labels to be union member type patterns");
}

static void test_infix_match_target_type_disallowed(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: f64): bool {\n"
        "    return x match 0;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_target_type_disallowed.f",
        source,
        "match target type");
}

static void test_infix_match_result_type_is_bool(void) {
    /* If result type were not bool, the && with a bool literal would fail. */
    const char *source =
        "module demo.main;\n"
        "func run(x: int): bool {\n"
        "    return (x match 0) && true;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_result_type_bool.f", source);
}

static void test_infix_match_enum_item_reference_pattern_accepted(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n"
        "func run(c: Color): bool {\n"
        "    return c match Color.Red | Color.Green;\n"
        "}\n";
    assert_single_source_semantic_ok("infix_match_enum_item_reference.f", source);
}

static void test_infix_match_enum_item_reference_rejects_cross_enum(void) {
    /* Match labels must reference the same enum as the target. */
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green\n"
        "}\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "func run(c: Color): bool {\n"
        "    return c match HttpStatus.Ok;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_enum_item_reference_cross_enum.f",
        source,
        "match label references enum");
}

static void test_infix_match_enum_item_reference_rejects_unknown_item(void) {
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green\n"
        "}\n"
        "func run(c: Color): bool {\n"
        "    return c match Color.Magenta;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_enum_item_reference_unknown_item.f",
        source,
        "has no item");
}

static void test_infix_match_enum_item_reference_rejects_binding(void) {
    /* enum item reference pattern does not support binding (AE1009). */
    const char *source =
        "module demo.main;\n"
        "enum Color {\n"
        "    Red,\n"
        "    Green\n"
        "}\n"
        "func run(c: Color): bool {\n"
        "    return c match n: Color.Red;\n"
        "}\n";
    assert_single_source_semantic_error_contains(
        "infix_match_enum_item_reference_binding.f",
        source,
        "infix match binding requires all labels to be union member type patterns");
}

static void test_infix_match_union_member_type_qualified_across_modules_accepted(void) {
    /* Regression: a qualified 2-segment type ref like `b.Error` is a
     * legitimate union member type when the target is a union-form spec
     * whose member is `b.Error`. The dispatch must NOT treat 2-segment
     * type refs as enum item references — it must consult the target's
     * static type (union vs enum), not the segment count. */
    const char *main_source =
        "module demo.main;\n"
        "import demo.base as b;\n"
        "spec Result: b.Error | int;\n"
        "func is_error(v: Result): bool {\n"
        "    return v match b.Error;\n"
        "}\n";
    const char *base_source =
        "open module demo.base;\n"
        "open type Error {}\n";
    FengProgram *main_program = parse_program_or_die("infix_match_union_qualified_main.f", main_source);
    FengProgram *base_program = parse_program_or_die("infix_match_union_qualified_base.f", base_source);
    const FengProgram *programs[] = {main_program, base_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    bool ok = feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok || error_count != 0U) {
        fprintf(stderr, "infix_match_union_qualified_main.f:\n");
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "  err: %s\n", errors[i].message);
        }
    }
    ASSERT(ok);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(main_program);
    feng_program_free(base_program);
}

static void test_infix_match_union_member_type_aliased_across_modules_accepted(void) {
    /* Fully-qualified type ref `demo.base.Error` as union member. Same
     * dispatch check: target is union, so resolve_type_ref must run. */
    const char *main_source =
        "module demo.main;\n"
        "import demo.base;\n"
        "spec Result: demo.base.Error | int;\n"
        "func is_error(v: Result): bool {\n"
        "    return v match demo.base.Error;\n"
        "}\n";
    const char *base_source =
        "open module demo.base;\n"
        "open type Error {}\n";
    FengProgram *main_program = parse_program_or_die("infix_match_union_aliased_main.f", main_source);
    FengProgram *base_program = parse_program_or_die("infix_match_union_aliased_base.f", base_source);
    const FengProgram *programs[] = {main_program, base_program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    bool ok = feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok || error_count != 0U) {
        fprintf(stderr, "infix_match_union_aliased_main.f:\n");
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "  err: %s\n", errors[i].message);
        }
    }
    ASSERT(ok);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(main_program);
    feng_program_free(base_program);
}

static bool type_ref_named_single_is(const FengTypeRef *type_ref, const char *name) {
    size_t length = strlen(name);

    return type_ref != NULL &&
           type_ref->kind == FENG_TYPE_REF_NAMED &&
           type_ref->as.named.segment_count == 1U &&
           type_ref->as.named.segments[0].length == length &&
           memcmp(type_ref->as.named.segments[0].data, name, length) == 0;
}

static void test_union_form_spec_records_normalized_members(void) {
    const char *source =
        "module demo.main;\n"
        "spec MaybeText: string | int | string;\n"
        "spec Value: MaybeText | bool | int;\n";
    FengProgram *program = parse_program_or_die("union_normalized_members.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengUnionSpecInfo *info;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(program->declaration_count == 2U);

    info = feng_semantic_lookup_union_spec_info(analysis, program->declarations[1]);
    ASSERT(info != NULL);
    ASSERT(info->member_count == 3U);
    ASSERT(type_ref_named_single_is(info->members[0].type_ref, "string"));
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        ASSERT(type_ref_named_single_is(info->members[1].type_ref, int_canonical));
    }
    ASSERT(type_ref_named_single_is(info->members[2].type_ref, "bool"));

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_union_form_spec_rejects_type_declared_spec_clause(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "type User: Value {}\n";

    assert_single_source_semantic_error_contains(
        "union_type_declared_spec_clause.f",
        source,
        "declared spec list can only contain object-form specs");
}

static void test_union_form_spec_rejects_fit_spec_clause(void) {
    const char *source =
        "module demo.main;\n"
        "type User {}\n"
        "spec Value: User | int;\n"
        "fit User: Value {}\n";

    assert_single_source_semantic_error_contains(
        "union_fit_spec_clause.f",
        source,
        "fit specs list can only contain object-form specs");
}

static void test_union_entry_records_exact_member_site(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "let value: Value = 1;\n";
    FengProgram *program = parse_program_or_die("union_entry_exact_site.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    const FengExpr *initializer;
    const FengUnionCoercionSite *site;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(program->declaration_count == 2U);
    ASSERT(program->declarations[1]->kind == FENG_DECL_GLOBAL_BINDING);

    initializer = program->declarations[1]->as.binding.initializer;
    site = feng_semantic_lookup_union_coercion_site(analysis, initializer);
    ASSERT(site != NULL);
    ASSERT(site->target_union_decl == program->declarations[0]);
    ASSERT(site->member_index == 0U);
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        ASSERT(type_ref_named_single_is(site->member_type_ref, int_canonical));
    }

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_union_entry_ambiguous_spec_member_requires_explicit_cast(void) {
    const char *source =
        "module demo.main;\n"
        "spec A {}\n"
        "spec B {}\n"
        "type Thing: A, B {}\n"
        "spec Value: A | B;\n"
        "func run(t: Thing): void {\n"
        "    let value: Value = t;\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "union_ambiguous_spec_member.f",
        source,
        "matches multiple members of union-form spec 'Value'; use an explicit cast");
}

static void test_union_entry_explicit_cast_selects_spec_member(void) {
    const char *source =
        "module demo.main;\n"
        "spec A {}\n"
        "spec B {}\n"
        "type Thing: A, B {}\n"
        "spec Value: A | B;\n"
        "func run(t: Thing): void {\n"
        "    let value: Value = (A)t;\n"
        "}\n";

    assert_single_source_semantic_ok("union_explicit_spec_member_cast.f", source);
}

static void test_union_match_accepts_type_labels(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(value: Value): int {\n"
        "    return match value {\n"
        "        int { 1; }\n"
        "        string { 2; }\n"
        "        else { 3; }\n"
        "    };\n"
        "}\n";

    assert_single_source_semantic_ok("union_match_type_labels.f", source);
}

static void test_union_match_rejects_literal_label(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(value: Value): int {\n"
        "    return match value {\n"
        "        1 { 1; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "union_match_literal_label.f",
        source,
        "union-form match labels must be union member types or 'else'");
}

static void test_union_match_rejects_range_label(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(value: Value): int {\n"
        "    return match value {\n"
        "        1...2 { 1; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "union_match_range_label.f",
        source,
        "union-form match labels must be union member types or 'else'");
}

static void test_union_match_narrows_object_spec_member(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "}\n"
        "spec Value: Named | int;\n"
        "func run(value: Value): string {\n"
        "    return match value {\n"
        "        v: Named { v.name; }\n"
        "        else { \"\"; }\n"
        "    };\n"
        "}\n";

    assert_single_source_semantic_ok("union_match_narrowed_member_access.f", source);
}

static void test_union_member_access_requires_narrowing(void) {
    const char *source =
        "module demo.main;\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "spec Value: Named | int;\n"
        "func run(value: Value): string {\n"
        "    return value.name;\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "union_member_access_without_narrowing.f",
        source,
        "must be narrowed to a single member before accessing member");
}

static void test_union_equality_requires_narrowing(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: int | string;\n"
        "func run(left: Value, right: Value): bool {\n"
        "    return left == right;\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "union_equality_without_narrowing.f",
        source,
        "requires union-form operands to be narrowed");
}

static void test_generic_union_form_accepts_concrete_member_matching(void) {
    const char *source =
        "module demo.main;\n"
        "type Error {}\n"
        "spec Result<T>: Error | T;\n"
        "func wrap_value(value: int): Result<int> {\n"
        "    return value;\n"
        "}\n"
        "func wrap_error(value: Error): Result<int> {\n"
        "    return value;\n"
        "}\n"
        "func run(value: Result<int>): int {\n"
        "    return match value {\n"
        "        v: int { v + 1; }\n"
        "        Error { 0; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";

    assert_single_source_semantic_ok("generic_union_concrete_member_match.f", source);
}

static void test_generic_union_form_rejects_mismatched_member(void) {
    const char *source =
        "module demo.main;\n"
        "type Error {}\n"
        "spec Result<T>: Error | T;\n"
        "let value: Result<int> = \"oops\";\n";
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
    char expected[64];
    snprintf(expected, sizeof(expected), "does not match expected type 'Result<%s>'", int_canonical);

    assert_single_source_semantic_error_contains(
        "generic_union_mismatched_member.f",
        source,
        expected);
}

static void test_tuple_literal_expected_contexts_pass(void) {
    const char *source =
        "module demo.main;\n"
        "type Unit();\n"
        "type Point(int, int);\n"
        "type Pair<T, U>(T, U);\n"
        "type Holder {\n"
        "    let point: Point;\n"
        "    let unit: Unit;\n"
        "}\n"
        "func take(p: Point): int { return p.item1; }\n"
        "func takeUnit(value: Unit): int { let () = value; return 1; }\n"
        "func makeUnit(): Unit { return (); }\n"
        "func make(): Point { return (1, 2); }\n"
        "func takePair(p: Pair<int, string>): int { return p.item1; }\n"
        "func run(): int {\n"
        "    let unit: Unit = ();\n"
        "    let () = unit;\n"
        "    let () = ();\n"
        "    let p: Point = (1, 2);\n"
        "    let q = (Point)(3, 4);\n"
        "    let h: Holder = Holder{point: (5, 6), unit: ()};\n"
        "    let pair: Pair<int, string> = (7, \"s\");\n"
        "    return takeUnit(()) + takeUnit(makeUnit()) + take((8, 9)) + make().item1 + p.item2 + q.item1 + h.point.item2 + pair.item1 + takePair((10, \"t\"));\n"
        "}\n";

    assert_single_source_semantic_ok("tuple_contexts.f", source);
}

static void test_tuple_literal_without_target_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): int {\n"
        "    let value = (1, 2);\n"
        "    return 0;\n"
        "}\n";

    assert_single_source_semantic_error_contains("tuple_no_target.f",
                                                 source,
                                                 "tuple literal requires");

    const char *empty_source =
        "module demo.main;\n"
        "func run(): int {\n"
        "    let value = ();\n"
        "    return 0;\n"
        "}\n";

    assert_single_source_semantic_error_contains("tuple_empty_no_target.f",
                                                 empty_source,
                                                 "tuple literal requires");
}

static void test_tuple_literal_shape_mismatch_is_rejected(void) {
    const char *arity_source =
        "module demo.main;\n"
        "type Point(int, int);\n"
        "func run(): void {\n"
        "    let p: Point = (1, 2, 3);\n"
        "}\n";
    const char *type_source =
        "module demo.main;\n"
        "type Point(int, int);\n"
        "func run(): void {\n"
        "    let p: Point = (1, \"two\");\n"
        "}\n";
    const char *empty_source =
        "module demo.main;\n"
        "type Point(int, int);\n"
        "func run(): void {\n"
        "    let p: Point = ();\n"
        "}\n";

    assert_single_source_semantic_error_contains("tuple_bad_arity.f",
                                                 arity_source,
                                                 "tuple literal has 3 element");
    assert_single_source_semantic_error_contains("tuple_bad_item.f",
                                                 type_source,
                                                 "does not match expected type");
    assert_single_source_semantic_error_contains("tuple_empty_bad_arity.f",
                                                 empty_source,
                                                 "tuple literal has 0 element");
}

static void test_tuple_named_conversion_rules(void) {
    const char *explicit_ok =
        "module demo.main;\n"
        "type UnitA();\n"
        "type UnitB();\n"
        "type A(int, string);\n"
        "type B(int, string);\n"
        "func run(): int {\n"
        "    let unit_b: UnitB = ();\n"
        "    let unit_a: UnitA = (UnitA)unit_b;\n"
        "    let b: B = (1, \"ok\");\n"
        "    let a: A = (A)b;\n"
        "    return a.item1;\n"
        "}\n";
    const char *implicit_bad =
        "module demo.main;\n"
        "type A(int, int);\n"
        "type B(int, int);\n"
        "type UnitA();\n"
        "type UnitB();\n"
        "func run(): void {\n"
        "    let unit_b: UnitB = ();\n"
        "    let unit_a: UnitA = unit_b;\n"
        "    let b: B = (1, 2);\n"
        "    let a: A = b;\n"
        "}\n";
    const char *explicit_bad =
        "module demo.main;\n"
        "type A(int, int);\n"
        "type C(int, string);\n"
        "func run(): void {\n"
        "    let c: C = (1, \"bad\");\n"
        "    let a: A = (A)c;\n"
        "}\n";

    assert_single_source_semantic_ok("tuple_explicit_cast_ok.f", explicit_ok);
    assert_single_source_semantic_error_contains("tuple_implicit_cast_bad.f",
                                                 implicit_bad,
                                                 "does not match expected type");
    assert_single_source_semantic_error_contains("tuple_explicit_cast_bad.f",
                                                 explicit_bad,
                                                 "cast from");
}

static void test_tuple_destructuring_semantics(void) {
    const char *source =
        "module demo.main;\n"
        "type Unit();\n"
        "type Point(int, int);\n"
        "func run(): int {\n"
        "    let unit: Unit = ();\n"
        "    let () = unit;\n"
        "    let p: Point = (1, 2);\n"
        "    let (x, y) = p;\n"
        "    let (a, , c) = (3, 4, 5);\n"
        "    var (, middle, ) = (6, 7, 8);\n"
        "    return x + y + a + c + middle;\n"
        "}\n";

    assert_single_source_semantic_ok("tuple_destructure_ok.f", source);
}

static void test_tuple_destructuring_non_tuple_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let (x, y) = 1;\n"
        "}\n";

    assert_single_source_semantic_error_contains("tuple_destructure_non_tuple.f",
                                                 source,
                                                 "destructuring binding initializer must be a tuple");
}

static void test_tuple_item_assignment_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Point(int, int);\n"
        "func run(): void {\n"
        "    var p: Point = (1, 2);\n"
        "    p.item1 = 3;\n"
        "}\n";

    assert_single_source_semantic_error_contains("tuple_item_assignment.f",
                                                 source,
                                                 "not writable");
}

static void test_tuple_whole_assignment_semantics(void) {
    const char *var_source =
        "module demo.main;\n"
        "type Point(int, int);\n"
        "func run(): int {\n"
        "    var p: Point = (1, 2);\n"
        "    p = (3, 4);\n"
        "    return p.item1 + p.item2;\n"
        "}\n";
    const char *let_source =
        "module demo.main;\n"
        "type Point(int, int);\n"
        "func run(): void {\n"
        "    let p: Point = (1, 2);\n"
        "    p = (3, 4);\n"
        "}\n";

    assert_single_source_semantic_ok("tuple_var_whole_assign.f", var_source);
    assert_single_source_semantic_error_contains("tuple_let_whole_assign.f",
                                                 let_source,
                                                 "not writable");
}

static void test_tuple_type_constraint_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Pair(int, int);\n"
        "func bad<T: Pair>(value: T): T { return value; }\n";

    assert_single_source_semantic_error_contains("tuple_constraint.f",
                                                 source,
                                                 "tuple type cannot be used as a constraint");
}

static void test_tuple_fit_spec_coercion_semantics(void) {
    const char *source =
        "module demo.main;\n"
        "spec Summable {\n"
        "    func sum(): int;\n"
        "}\n"
        "type Point(int, int);\n"
        "fit Point: Summable {\n"
        "    func sum(): int { return self.item1 + self.item2; }\n"
        "}\n"
        "func consume(value: Summable): int {\n"
        "    return value.sum();\n"
        "}\n"
        "func run(): int {\n"
        "    let point: Point = (1, 2);\n"
        "    let value: Summable = point;\n"
        "    return consume(point) + value.sum();\n"
        "}\n";

    assert_single_source_semantic_ok("tuple_fit_spec_coercion.f", source);
}

/* --- Iterator protocol semantic error tests --- */

static void test_iterator_multiple_iterable_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Cursor {\n"
        "    @iterator\n"
        "    func next(): Result { return (false, 0); }\n"
        "}\n"
        "type Container {\n"
        "    @iterable\n"
        "    func iter1(): Cursor { return Cursor {}; }\n"
        "    @iterable\n"
        "    func iter2(): Cursor { return Cursor {}; }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_multiple_iterable.f", source,
        "has multiple @iterable methods");
}

static void test_iterator_multiple_iterator_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Cursor {\n"
        "    @iterator\n"
        "    func next1(): Result { return (false, 0); }\n"
        "    @iterator\n"
        "    func next2(): Result { return (false, 0); }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_multiple_iterator.f", source,
        "has multiple @iterator methods");
}

static void test_iterator_both_annotations_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Cursor {\n"
        "    @iterator\n"
        "    func next(): Result { return (false, 0); }\n"
        "}\n"
        "type Thing {\n"
        "    @iterable\n"
        "    func iter(): Cursor { return Cursor {}; }\n"
        "    @iterator\n"
        "    func next(): Result { return (false, 0); }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_both_annotations.f", source,
        "cannot have both @iterable and @iterator");
}

static void test_iterator_iterable_with_params_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Cursor {\n"
        "    @iterator\n"
        "    func next(): Result { return (false, 0); }\n"
        "}\n"
        "type Container {\n"
        "    @iterable\n"
        "    func iter(n: i32): Cursor { return Cursor {}; }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_iterable_with_params.f", source,
        "@iterable method must take no parameters");
}

static void test_iterator_iterator_with_params_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Cursor {\n"
        "    @iterator\n"
        "    func next(n: i32): Result { return (false, 0); }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_iterator_with_params.f", source,
        "@iterator method must take no parameters");
}

static void test_iterator_iterable_return_no_iterator_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Plain {\n"
        "    let value: i32;\n"
        "}\n"
        "type Container {\n"
        "    @iterable\n"
        "    func iter(): Plain { return Plain { value: 0 }; }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_iterable_return_no_iterator.f", source,
        "return type of @iterable method has no @iterator method");
}

static void test_iterator_return_not_tuple_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Cursor {\n"
        "    @iterator\n"
        "    func next(): i32 { return 0; }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_return_not_tuple.f", source,
        "@iterator method must return a named tuple type of the form (bool, E)");
}

static void test_iterator_for_in_not_iterable_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "type Plain {\n"
        "    let value: i32;\n"
        "}\n"
        "func print(value: i32) {}\n"
        "func run(): void {\n"
        "    let p = Plain { value: 1 };\n"
        "    for let x in p {\n"
        "        print(x);\n"
        "    }\n"
        "}\n";

    assert_single_source_semantic_error_contains(
        "iter_for_in_not_iterable.f", source,
        "is not iterable (no @iterable or @iterator method found)");
}

static void test_iterator_basic_iterable_ok(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Cursor {\n"
        "    var pos: i32;\n"
        "    @iterator\n"
        "    func next(): Result {\n"
        "        if self.pos >= 3 { return (false, 0); }\n"
        "        let v = self.pos;\n"
        "        self.pos = self.pos + 1;\n"
        "        return (true, v);\n"
        "    }\n"
        "}\n"
        "type Container {\n"
        "    @iterable\n"
        "    func iter(): Cursor { return Cursor { pos: 0 }; }\n"
        "}\n"
        "func use(n: i32) {}\n"
        "func run(): void {\n"
        "    let c = Container {};\n"
        "    for let x in c {\n"
        "        use(x);\n"
        "    }\n"
        "}\n";

    assert_single_source_semantic_ok("iter_basic_iterable_ok.f", source);
}

static void test_iterator_self_cursor_ok(void) {
    const char *source =
        "module demo.main;\n"
        "type Result(bool, i32);\n"
        "type Counter {\n"
        "    var pos: i32;\n"
        "    @iterator\n"
        "    func next(): Result {\n"
        "        if self.pos >= 3 { return (false, 0); }\n"
        "        let v = self.pos;\n"
        "        self.pos = self.pos + 1;\n"
        "        return (true, v);\n"
        "    }\n"
        "}\n"
        "func use(n: i32) {}\n"
        "func run(): void {\n"
        "    let c = Counter { pos: 0 };\n"
        "    for let x in c {\n"
        "        use(x);\n"
        "    }\n"
        "}\n";

    assert_single_source_semantic_ok("iter_self_cursor_ok.f", source);
}

/* ===================== if/match expression throw branch tests ===================== */

static void test_if_expr_then_throw_else_value_accepted(void) {
    /* An if-expression where the then branch terminates with throw and
     * the else branch yields a value. The result type is taken from the
     * else branch. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        throw \"error\";\n"
        "    } else {\n"
        "        42\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_then_throw_else_value_ok.f", source);
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

static void test_if_expr_then_value_else_throw_accepted(void) {
    /* An if-expression where the then branch yields a value and the
     * else branch terminates with throw. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = if true {\n"
        "        42\n"
        "    } else {\n"
        "        throw \"error\";\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_then_value_else_throw_ok.f", source);
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

static void test_if_expr_both_branches_throw_accepted(void) {
    /* An if-expression where both branches terminate with throw.
     * When both branches throw, the if construct is still a statement
     * (not an expression with a value), so it is used without a trailing
     * semicolon expression wrapper. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    if true {\n"
        "        throw \"a\";\n"
        "    } else {\n"
        "        throw \"b\";\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_both_throw_ok.f", source);
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

static void test_match_expr_partial_throw_accepted(void) {
    /* A match expression where some branches throw and the rest
     * yield values. The result type is taken from the yielding
     * branches. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let v: i32 = 0;\n"
        "    let label: string = match v {\n"
        "        0 { \"zero\"; }\n"
        "        1 { throw \"unexpected one\"; }\n"
        "        else { \"other\"; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_partial_throw_ok.f", source);
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

static void test_match_expr_all_branches_throw_accepted(void) {
    /* A match expression where all branches terminate with throw.
     * When all branches throw, the match construct is used as a
     * statement (not an expression with a value). */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let v: i32 = 0;\n"
        "    match v {\n"
        "        0 { throw \"zero\"; }\n"
        "        1 { throw \"one\"; }\n"
        "        else { throw \"other\"; }\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_all_throw_ok.f", source);
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

static void test_if_expr_branch_literal_adapts_to_non_literal_branch(void) {
    /* No explicit target type: the non-literal branch determines the target.
     * The literal branch adapts to it.  Without adaptation the literal
     * defaults to int (i64 on 64-bit platforms) and the type check fails. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let x: i32 = 5;\n"
        "    let y = if true { x } else { 10 };\n"
        "    let z = if true { 10 } else { x };\n"
        "    return y + z;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_adapt_no_target_ok.f", source);
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

static void test_if_expr_branch_literal_adapts_to_explicit_target(void) {
    /* Explicit target type from binding annotation: both literal branches
     * adapt to the target type. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let a: i32 = if true { 1 } else { 2 };\n"
        "    let b: u8 = if true { 10 } else { 20 };\n"
        "    let c: f32 = if true { 1.5 } else { 2.5 };\n"
        "    return a;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_adapt_explicit_target_ok.f", source);
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

static void test_if_expr_branch_literal_out_of_range_rejected(void) {
    /* 256 does not fit u8, so adaptation fails and the literal keeps its
     * default int (i64) type — the type equality check then rejects it. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: u8 = 10;\n"
        "    let y = if true { x } else { 256 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_adapt_range_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "if_expr_adapt_range_error.f") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_if_expr_branch_both_literals_no_target_accepted(void) {
    /* Both branches are literals with no explicit target: each defaults to
     * int (i64), types match, accepted. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let y = if true { 1 } else { 2 };\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_both_literals_ok.f", source);
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

static void test_if_expr_branch_literal_adapts_to_union_member(void) {
    /* Union target type: literal branch adapts to a numeric union member;
     * non-literal branch is a valid union member. */
    const char *source =
        "module demo.main;\n"
        "spec Result: i32 | string;\n"
        "func run(): Result {\n"
        "    let r: Result = if true { 10 } else { \"error\" };\n"
        "    return r;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_adapt_union_ok.f", source);
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

static void test_if_expr_branch_literal_return_value_adapts(void) {
    /* Return value provides explicit target type: literal branches adapt. */
    const char *source =
        "module demo.main;\n"
        "func run(cond: bool): i32 {\n"
        "    return if cond { 1 } else { 2 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("if_expr_adapt_return_ok.f", source);
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

static void test_match_expr_branch_literal_adapts_to_non_literal_branch(void) {
    /* No explicit target type: the non-literal branch determines the target.
     * The literal branch adapts to it.  Without adaptation the literal
     * defaults to int (i64 on 64-bit platforms) and the type check fails. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let x: i32 = 5;\n"
        "    let y = match 1 {\n"
        "        1 { x; }\n"
        "        else { 10; }\n"
        "    };\n"
        "    let z = match 1 {\n"
        "        1 { 10; }\n"
        "        else { x; }\n"
        "    };\n"
        "    return y + z;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_expr_adapt_no_target_ok.f", source);
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

static void test_match_expr_branch_literal_adapts_to_explicit_target(void) {
    /* Explicit target type from binding annotation: all literal branches
     * adapt to the target type. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let a: i32 = match 1 {\n"
        "        1 { 1; }\n"
        "        else { 2; }\n"
        "    };\n"
        "    let b: u8 = match 1 {\n"
        "        1 { 10; }\n"
        "        else { 20; }\n"
        "    };\n"
        "    let c: f32 = match 1 {\n"
        "        1 { 1.5; }\n"
        "        else { 2.5; }\n"
        "    };\n"
        "    return a;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_expr_adapt_explicit_target_ok.f", source);
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

static void test_match_expr_branch_literal_out_of_range_rejected(void) {
    /* 256 does not fit u8, so adaptation fails and the literal keeps its
     * default int (i64) type — the type equality check then rejects it. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: u8 = 10;\n"
        "    let y = match 1 {\n"
        "        1 { x; }\n"
        "        else { 256; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_expr_adapt_range_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "match_expr_adapt_range_error.f") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_match_expr_branch_all_literals_no_target_accepted(void) {
    /* All branches are literals with no explicit target: each defaults to
     * int (i64), types match, accepted. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let y = match 1 {\n"
        "        1 { 1; }\n"
        "        else { 2; }\n"
        "    };\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_expr_all_literals_ok.f", source);
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

static void test_match_expr_branch_literal_adapts_to_union_member(void) {
    /* Union target type: literal branch adapts to a numeric union member;
     * non-literal branch is a valid union member. */
    const char *source =
        "module demo.main;\n"
        "spec Result: i32 | string;\n"
        "func run(): Result {\n"
        "    let r: Result = match 1 {\n"
        "        1 { 10; }\n"
        "        else { \"error\"; }\n"
        "    };\n"
        "    return r;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_expr_adapt_union_ok.f", source);
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

static void test_match_expr_branch_literal_return_value_adapts(void) {
    /* Return value provides explicit target type: literal branches adapt. */
    const char *source =
        "module demo.main;\n"
        "func run(cond: i32): i32 {\n"
        "    return match cond {\n"
        "        1 { 1; }\n"
        "        else { 2; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("match_expr_adapt_return_ok.f", source);
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

static void test_try_expr_catch_literal_adapts_to_body_type(void) {
    /* No explicit target type: the body expression determines the target.
     * The catch literal adapts to the body's inferred type.  Without
     * adaptation the literal defaults to int (i64 on 64-bit platforms)
     * and the type check fails. */
    const char *source =
        "module demo.main;\n"
        "func throw_i32(): i32 {\n"
        "    let v: i32 = 7;\n"
        "    throw v;\n"
        "    return 0;\n"
        "}\n"
        "func run(): i32 {\n"
        "    let y = try throw_i32() catch ex: i32 { 10 };\n"
        "    return y;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_expr_adapt_body_ok.f", source);
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

static void test_try_expr_body_literal_adapts_to_explicit_target(void) {
    /* Explicit target type from binding annotation: both body literal and
     * catch literal adapt to the target type. */
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let a: i32 = try 10 catch ex: bool { 5 };\n"
        "    let b: u8 = try 10 catch ex: bool { 20 };\n"
        "    return a;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_expr_adapt_explicit_target_ok.f", source);
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

static void test_try_expr_catch_literal_adapts_to_explicit_target(void) {
    /* Explicit target type from binding annotation: catch literal adapts
     * to the target type even when the body is a function call. */
    const char *source =
        "module demo.main;\n"
        "func f(): i32 {\n"
        "    throw 1;\n"
        "    return 0;\n"
        "}\n"
        "func run(): i32 {\n"
        "    let x: i32 = try f() catch ex: i32 { 10 };\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_expr_adapt_catch_explicit_ok.f", source);
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

static void test_try_expr_catch_literal_out_of_range_rejected(void) {
    /* 256 does not fit u8, so adaptation fails and the literal keeps its
     * default int (i64) type — the type equality check then rejects it. */
    const char *source =
        "module demo.main;\n"
        "func throw_u8(): u8 {\n"
        "    let v: u8 = 7;\n"
        "    throw v;\n"
        "    return 0;\n"
        "}\n"
        "func run() {\n"
        "    let y = try throw_u8() catch ex: u8 { 256 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_expr_adapt_range_error.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "try_expr_adapt_range_error.f") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_try_expr_catch_literal_adapts_to_union_member(void) {
    /* Union target type: literal catch result adapts to a numeric union
     * member; non-literal body is a valid union member. */
    const char *source =
        "module demo.main;\n"
        "spec Result: i32 | string;\n"
        "func f(): i32 {\n"
        "    throw 1;\n"
        "    return 0;\n"
        "}\n"
        "func run(): Result {\n"
        "    let r: Result = try f() catch ex: i32 { 10 };\n"
        "    return r;\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_expr_adapt_union_ok.f", source);
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

static void test_try_expr_catch_literal_return_value_adapts(void) {
    /* Return value provides explicit target type: catch literal adapts. */
    const char *source =
        "module demo.main;\n"
        "func f(): i32 {\n"
        "    throw 1;\n"
        "    return 0;\n"
        "}\n"
        "func run(): i32 {\n"
        "    return try f() catch ex: i32 { 10 };\n"
        "}\n";
    FengProgram *program = parse_program_or_die("try_expr_adapt_return_ok.f", source);
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

static void test_array_literal_adapts_first_non_literal(void) {
    /* No explicit target: the first non-literal element determines the target
     * type.  Subsequent literal elements adapt to it.  Without adaptation the
     * literal defaults to int (i64 on 64-bit platforms) and AE0201 fires. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = 5;\n"
        "    let a = [x, 10];\n"
        "    let b = [x, 1, 2, 3];\n"
        "    var y: u8 = 10;\n"
        "    let c = [y, 255];\n"
        "    let d: i32 = a[0];\n"
        "    let e: i32 = b[1];\n"
        "    let f: u8 = c[1];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_adapt_first_nonlit.f", source);
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

static void test_array_literal_adapts_first_element_literal(void) {
    /* First element is a literal; scan skips it to find the first non-literal
     * element.  All literal elements adapt to the non-literal's type. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = 5;\n"
        "    let a = [10, x];\n"
        "    let b = [1, 2, x, 3];\n"
        "    let c: i32 = a[0];\n"
        "    let d: i32 = b[0];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_adapt_first_lit.f", source);
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

static void test_array_literal_all_literals_no_target_accepted(void) {
    /* All elements are literals with no explicit target: each defaults to
     * int (i64), types match, accepted. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let a = [1, 2, 3];\n"
        "    let b = [1.0, 2.5, 3.14];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_adapt_all_lit.f", source);
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

static void test_array_literal_non_literal_type_mismatch_rejected(void) {
    /* Two non-literal elements with different types: no adaptation applies,
     * the type equality check rejects. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: i32 = 1;\n"
        "    let y: i64 = 2;\n"
        "    let a = [x, y];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_adapt_mismatch.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "array_adapt_mismatch.f") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_array_literal_out_of_range_rejected(void) {
    /* 256 does not fit u8, so adaptation fails and the literal keeps its
     * default int (i64) type — the type equality check then rejects. */
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    let x: u8 = 10;\n"
        "    let a = [x, 256];\n"
        "}\n";
    FengProgram *program = parse_program_or_die("array_adapt_range.f", source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, "array_adapt_range.f") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

static void test_tuple_literal_element_adapts_to_field_type(void) {
    /* Binding: small integer fields (u8, i8). */
    const char *ok_source =
        "module demo.main;\n"
        "type BytePair(u8, u8);\n"
        "type SmallPair(i8, i8);\n"
        "type MixedPair(i32, f32);\n"
        "func take(p: BytePair): u8 { return p.item1; }\n"
        "func make(): SmallPair { return (1, 2); }\n"
        "func run(): void {\n"
        "    let a: BytePair = (10, 20);\n"
        "    let b: SmallPair = (5, 6);\n"
        "    let c: MixedPair = (42, 1);\n"
        "    let d = take((1, 2));\n"
        "    let e = make();\n"
        "}\n";
    assert_single_source_semantic_ok("tuple_adapt_fields.f", ok_source);

    /* Range: 256 does not fit u8. */
    const char *range_source =
        "module demo.main;\n"
        "type BytePair(u8, u8);\n"
        "func run(): void {\n"
        "    let p: BytePair = (10, 256);\n"
        "}\n";
    FengProgram *program = parse_program_or_die("tuple_adapt_range.f", range_source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count));
    ASSERT(error_count == 1U);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

int main(void) {
    test_match_range_label_overlap_rejected();
    test_match_single_label_overlap_rejected();
    test_match_range_invalid_bounds_rejected();
    test_match_target_type_disallowed();
    test_match_let_bound_label_accepted();
    test_match_enum_single_label_accepted();
    test_match_enum_value_list_accepted();
    test_match_enum_expression_form_accepted();
    test_match_enum_block_tail_return_value_accepted();
    test_match_enum_explicit_values_accepted();
    test_match_enum_cross_enum_reference_rejected();
    test_match_enum_range_label_rejected();
    test_match_enum_nonexistent_item_rejected();
    test_match_enum_item_duplicate_rejected();
    test_match_enum_mixed_with_int_literal_rejected();
    test_match_enum_mixed_with_string_literal_rejected();
    test_match_enum_binding_prefix_rejected();
    test_infix_match_value_pattern_accepted();
    test_infix_match_range_pattern_accepted();
    test_infix_match_multi_label_pipe_accepted();
    test_infix_match_mixed_value_and_range_pipe_accepted();
    test_infix_match_string_pattern_accepted();
    test_infix_match_union_member_type_pattern_accepted();
    test_infix_match_union_multi_label_type_pattern_accepted();
    test_infix_match_union_member_binding_in_if_body_accepted();
    test_infix_match_union_member_binding_in_while_body_accepted();
    test_infix_match_union_member_binding_visible_in_rhs_of_and();
    test_infix_match_union_member_binding_invisible_after_statement();
    test_infix_match_union_member_binding_invisible_in_or_operand();
    test_infix_match_rejects_binding_on_value_pattern();
    test_infix_match_rejects_binding_on_range_pattern();
    test_infix_match_rejects_binding_with_mixed_type_and_value_labels();
    test_infix_match_target_type_disallowed();
    test_infix_match_result_type_is_bool();
    test_infix_match_enum_item_reference_pattern_accepted();
    test_infix_match_enum_item_reference_rejects_cross_enum();
    test_infix_match_enum_item_reference_rejects_unknown_item();
    test_infix_match_enum_item_reference_rejects_binding();
    test_infix_match_union_member_type_qualified_across_modules_accepted();
    test_infix_match_union_member_type_aliased_across_modules_accepted();
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
        test_spec_relation_fit_builtin_target();
        test_spec_relation_fit_array_target();
    test_spec_coercion_object_let_binding();
    test_spec_coercion_object_builtin_let_binding();
    test_spec_coercion_object_array_let_binding();
    test_spec_coercion_object_argument();
    test_spec_coercion_object_scalar_argument_uses_box_owner();
    test_spec_coercion_object_return();
    test_spec_coercion_object_scalar_return_uses_box_owner();
    test_spec_coercion_callable_top_level_fn();
    test_spec_coercion_callable_lambda();
    test_spec_coercion_callable_lambda_argument();
    test_callable_spec_value_rejects_different_spec_implicit_match();
    test_callable_spec_value_explicit_cast_accepts_equal_signature();
    test_callable_spec_top_level_fn_still_matches_multiple_specs();
    test_spec_default_local_binding_object_form();
    test_spec_default_local_binding_callable_form();
    test_spec_default_type_field_no_initializer();
    test_spec_member_access_field_read();
    test_spec_member_access_field_write();
    test_spec_member_access_method_call();
    test_spec_member_access_callable_form_rejected();
    test_spec_witness_via_declared_head();
    test_spec_witness_via_fit();
    test_spec_witness_field_member();
    test_spec_witness_on_demand_only();
    test_spec_witness_via_generic_instantiation();
    test_spec_witness_subject_key_supports_builtin_and_array();
    test_spec_witness_multi_fit_conflict();
    test_spec_equality_object_eq_recorded();
    test_spec_equality_object_neq_recorded();
    test_spec_equality_string_not_recorded();
    test_spec_equality_int_not_recorded();
    test_duplicate_type_across_files_same_module();
    test_duplicate_binding_across_files_same_module();
    test_function_return_only_overload_error();
    test_top_level_overload_overlap_via_fit_rejected();
    test_top_level_overload_overlap_via_two_specs_rejected();
    test_top_level_overload_two_specs_no_common_type_accepted();
    test_member_method_overload_overlap_via_fit_rejected();
    test_fit_method_overload_conflicts_match_method_rules();
    test_extern_function_accepts_module_string_library_binding();
    test_extern_function_accepts_c_symbol_name_argument();
    test_extern_function_accepts_imported_string_c_symbol_binding();
    test_extern_function_without_calling_convention_annotation_is_accepted();
    test_extern_function_rejects_multiple_calling_convention_annotations();
    test_extern_function_rejects_too_many_calling_convention_arguments();
    test_extern_function_rejects_non_string_library_binding();
    test_extern_function_rejects_non_string_c_symbol_binding();
    test_extern_function_accepts_imported_string_library_binding();
    test_extern_function_rejects_imported_var_library_binding();
    test_extern_function_accepts_string_parameter_without_c_abi_annotation();
    test_extern_function_accepts_string_return_without_c_abi_annotation();
    test_runtime_annotation_accepts_top_level_extern_function();
    test_runtime_annotation_rejects_non_extern_function();
    test_runtime_annotation_rejects_c_abi_target_annotation();
    test_runtime_annotation_rejects_abi_annotation();
    test_runtime_annotation_rejects_type_declaration();
    test_runtime_annotation_rejects_member_method();
    test_value_annotation_accepts_type_declaration();
    test_value_annotation_rejects_spec_declaration();
    test_value_annotation_rejects_function_declaration();
    test_value_annotation_rejects_binding();
    test_tuple_direct_self_reference_rejected();
    test_tuple_indirect_cycle_rejected();
    test_value_type_direct_self_reference_rejected();
    test_value_type_indirect_cycle_rejected();
    test_ordinary_type_self_reference_allowed();
    test_value_type_holding_managed_pointer_allowed();
    test_tuple_array_of_self_rejected();
    test_mixed_tuple_and_value_type_cycle_rejected();
    test_value_type_generic_arg_cycle_rejected();
    test_value_type_generic_nested_arg_cycle_rejected();
    test_value_type_generic_no_cycle_allowed();
    test_deep_indirect_cycle_rejected();
    test_cross_file_value_type_cycle_rejected();
    test_cross_module_value_type_cycle_rejected();
    test_pointer_to_self_allowed();
    test_value_type_spec_field_allowed();
    test_value_type_union_spec_field_allowed();
    test_value_type_callable_spec_field_allowed();
    test_value_type_enum_field_allowed();
    test_value_type_finalizer_rejected();
    test_ordinary_type_finalizer_allowed();
    test_extern_function_accepts_abi_array_parameter_type();
    test_extern_function_accepts_abi_array_return_type();
    test_extern_function_rejects_bare_string_parameter_type();
    test_extern_function_rejects_bare_string_return_type();
    test_extern_function_rejects_non_abi_array_parameter_type();
    test_extern_function_rejects_non_abi_object_parameter();
    test_extern_function_accepts_abi_object_and_callback_types();
    test_fixed_annotation_is_rejected();
    test_abi_type_accepts_abi_stable_fields();
    test_abi_type_rejects_managed_field_type();
    test_abi_type_rejects_inline_abi_object_field_type();
    test_abi_type_rejects_direct_array_field_type();
    test_abi_type_rejects_direct_callable_field_type();
    test_unknown_top_level_annotation_is_rejected();
    test_bounded_annotation_is_rejected();
    test_abi_function_accepts_abi_stable_signature();
    test_abi_function_accepts_fieldless_abi_type_pointer_signature();
    test_abi_function_rejects_fieldless_abi_type_value_parameter();
    test_abi_function_rejects_fieldless_abi_type_value_return();
    test_extern_function_accepts_abi_value_param_and_return();
    test_extern_function_accepts_fieldless_abi_type_pointer_param_and_return();
    test_extern_function_rejects_fieldless_abi_type_value_parameter();
    test_extern_function_rejects_fieldless_abi_type_value_return();
    test_extern_function_rejects_non_abi_type_pointer_parameter();
    test_abi_function_accepts_abi_array_parameter();
    test_abi_function_rejects_parameterized_calling_convention();
    test_abi_method_rejects_managed_signature_type();
    test_abi_function_type_accepts_abi_function_value();
    test_abi_function_type_rejects_plain_function_value();
    test_abi_function_type_rejects_direct_lambda_value();
    test_abi_function_type_rejects_captured_lambda_binding();
    test_object_form_spec_rejects_abi_annotation();
    test_unknown_member_annotation_is_rejected();
    test_abi_callable_spec_accepts_abi_type_parameter();
    test_abi_callable_spec_accepts_fieldless_abi_type_pointer_signature();
    test_abi_callable_spec_accepts_abi_array_parameter();
    test_abi_callable_spec_rejects_non_abi_array_parameter();
    test_abi_callable_spec_rejects_non_abi_type_parameter();
    test_abi_callable_spec_rejects_fieldless_abi_type_value_parameter();
    test_abi_callable_spec_rejects_object_spec_parameter();
    test_abi_callable_spec_rejects_non_abi_return_type();
    test_abi_callable_spec_rejects_fieldless_abi_type_value_return();
    test_abi_function_rejects_uncaught_throw();
    test_abi_function_allows_locally_caught_throw();
    test_abi_function_rejects_call_to_throwing_function();
    test_abi_function_allows_call_to_catching_function();
    test_abi_method_rejects_uncaught_throw();
    test_abi_function_allows_unused_lambda_wrapping_throwing_call();
    test_abi_function_rejects_invoked_lambda_wrapping_throwing_call();
    test_abi_function_rejects_local_function_value_call_to_throwing_function();
    test_abi_function_allows_invoked_lambda_wrapping_catching_call();
    test_throw_rejects_void_expression();
    test_break_outside_loop_is_rejected();
    test_continue_outside_loop_is_rejected();
    test_defer_inside_function_is_accepted();
    test_defer_with_return_is_rejected();
    test_defer_with_throw_is_rejected();
    test_defer_with_nested_defer_is_rejected();
    test_defer_with_break_at_top_level_of_defer_is_rejected();
    test_defer_with_continue_at_top_level_of_defer_is_rejected();
    test_defer_with_break_inside_nested_loop_is_accepted();
    test_break_inside_lambda_in_loop_is_rejected();
    test_break_and_continue_inside_for_loop_are_accepted();
    test_break_directly_in_if_expr_block_is_rejected();
    test_continue_directly_in_if_expr_block_is_rejected();
    test_break_inside_loop_inside_if_expr_block_is_accepted();
    test_if_expr_trailing_if_else_in_then_branch_accepted();
    test_if_expr_trailing_if_else_in_else_branch_accepted();
    test_if_expr_trailing_if_else_chain_accepted();
    test_if_expr_trailing_match_accepted();
    test_if_expr_trailing_try_catch_accepted();
    test_if_expr_non_expr_trailing_binding_rejected();
    test_if_expr_trailing_if_without_else_rejected();
    test_match_trailing_if_else_in_branch_accepted();
    test_match_trailing_if_else_in_else_branch_accepted();
    test_try_expr_trailing_if_else_in_catch_accepted();
    test_try_stmt_trailing_if_in_catch_not_converted();
    test_match_stmt_trailing_if_in_branch_not_converted();
    test_break_directly_in_try_expr_catch_block_is_rejected();
    test_continue_directly_in_try_expr_catch_block_is_rejected();
    test_break_inside_loop_inside_try_expr_catch_block_is_accepted();
    test_throw_rejects_pointer_value();
    test_throw_rejects_abi_type_value();
    test_throw_accepts_string_and_managed_type();
    test_catch_unknown_allows_rethrow_only();
    test_catch_unknown_rejects_value_use();
    test_try_expression_catch_result_can_use_bound_value();
    test_try_without_catch_is_rejected();
    test_try_catch_statement_allows_empty_catch();
    test_try_expression_rejects_bound_value_result_mismatch();
    test_unknown_type_is_only_valid_in_catch_clause();
    test_throw_rejects_callable_values();
    test_throw_allows_spec_values();
    test_catch_rejects_non_exception_types();
    test_catch_without_binding_accepts_anonymous_clause();
    test_catch_anonymous_must_be_last_clause();
    test_top_level_function_auto_infers_return_type_for_forward_call();
    test_top_level_function_rejects_conflicting_inferred_return_types();
    test_method_auto_infers_return_type_for_forward_call();
    test_imported_function_auto_infers_return_type_across_modules();
    test_omitted_return_function_rejects_lambda_signature_inference();
    test_explicit_callable_return_accepts_lambda();
    test_omitted_return_function_value_matches_named_function_type();
    test_explicit_non_void_return_rejects_empty_return();
    test_match_expression_rejects_non_constant_label();
    test_match_expression_rejects_incomparable_label_type();
    test_match_expression_rejects_inconsistent_result_types();
    test_untyped_lambda_binding_is_rejected();
    test_untyped_lambda_binding_cannot_later_match_named_function_type();
    test_module_visibility_conflict();
    test_valid_function_overload_by_parameter_type();
    test_top_level_function_call_selects_overload_by_literal_type();
    test_top_level_function_call_selects_overload_by_inferred_local_binding();
    test_top_level_binding_inferred_type_is_used_by_identifier();
    test_top_level_function_call_reports_type_mismatch();
    test_generic_extern_call_accepts_wrapped_array_inference();
    test_generic_extern_call_accepts_bare_type_param_inference();
    test_generic_extern_call_accepts_bare_type_param_return();
    test_fit_method_accepts_fit_type_param_argument();
    test_generic_extern_call_rejects_conflicting_wrapped_array_inference();
    test_generic_non_extern_call_does_not_expand_wrapped_array_inference();
    test_imported_function_call_selects_overload_by_literal_type();
    test_imported_generic_extern_call_accepts_wrapped_array_inference();
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
    test_type_field_inferred_initializer_member_access_accepted();
    test_local_let_assignment_rejects_non_writable_target();
    test_default_parameter_assignment_rejects_non_writable_target();
    test_var_parameter_assignment_is_writable();
    test_top_level_let_assignment_rejects_non_writable_target();
    test_instance_let_member_assignment_rejects_non_writable_target();
    test_alias_public_let_binding_assignment_rejects_non_writable_target();
    test_index_assignment_accepts_explicit_array_target();
    test_index_assignment_rejects_non_matching_array_element_type();
    test_compound_assignment_accepts_numeric_and_bitwise_targets();
    test_compound_assignment_rejects_string_plus_equal();
    test_compound_assignment_literal_adapts_to_target_type();
    test_compound_assignment_literal_out_of_range_reports_type_mismatch();
    test_inferred_array_literal_binding_rejects_index_write_without_writable_layer();
    test_inferred_array_literal_binding_rejects_non_matching_index_assignment();
    test_inferred_array_literal_rejects_mixed_element_types();
    test_inferred_nested_array_literal_rejects_nested_index_write_without_writable_layer();
    test_empty_array_literal_binding_requires_explicit_target_type();
    test_empty_array_literal_binding_accepts_explicit_target_type();
    test_index_assignment_rejects_readonly_array();
    test_array_literal_matches_readonly_target();
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
    test_non_generic_array_new_colon_dimension_accepts_expected_target();
    test_non_generic_array_new_legacy_bracket_syntax_rejected();
    test_generic_array_new_colon_dimension_accepts_expected_target();
    test_generic_array_new_legacy_bracket_syntax_rejected();
    test_index_access_on_uppercase_local_name_remains_index_expression();
    test_index_expression_rejects_float_operand();
    test_index_expression_rejects_bool_operand();
    test_index_expression_rejects_non_array_target();
    test_index_assignment_rejects_non_array_target();
    test_unary_minus_rejects_non_numeric_operand();
    test_unary_not_rejects_non_bool_operand();
    test_unary_address_of_rejects_returned_scalar_binding();
    test_unary_address_of_rejects_returned_array_value();
    test_unary_address_of_rejects_returned_string_value();
    test_unary_address_of_allows_extern_call_borrowed_data_pointer();
    test_unary_address_of_allows_fielded_abi_type_pointer_binding();
    test_unary_address_of_rejects_fieldless_abi_type_pointer_binding();
    test_unary_address_of_rejects_value_type_without_abi();
    test_unary_address_of_allows_value_abi_type();
    test_unary_address_of_rejects_string_to_byte_pointer_binding();
    test_unary_address_of_rejects_non_extern_forwarding_via_assignment_alias();
    test_unary_address_of_rejects_object_field_storage();
    test_unary_address_of_rejects_member_assignment_storage();
    test_unary_address_of_accepts_top_level_abi_function_pointer_target();
    test_unary_address_of_requires_explicit_function_pointer_target();
    test_unary_address_of_rejects_plain_function_pointer_target();
    test_unary_address_of_rejects_method_pointer_target();
    test_unary_address_of_rejects_local_lambda_pointer_target();
    test_function_pointer_binding_is_not_directly_callable();
    test_function_pointer_semantic_allows_field_param_and_return_flow();
    test_unary_address_of_rejects_bound_method_pointer_target();
    test_binary_equality_accepts_data_pointer_operands();
    test_binary_equality_accepts_function_pointer_operands();
    test_binary_equality_rejects_mismatched_pointer_operands();
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
    test_const_fold_float_modulo_by_zero_rejected();
    test_float_modulo_expression_is_accepted();
    test_const_fold_i64_overflow_rejected();
    test_const_fold_shift_amount_via_const_expr();
    test_const_fold_cast_truncation_then_target_check();
    test_const_fold_immutable_local_binding_requires_explicit_cast();
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
    test_import_short_names_do_not_leak_to_other_files();
    test_import_name_conflict_with_other_file_decl_does_not_error();
    test_alias_import_does_not_inject_short_names();
    test_import_alias_conflicts_with_same_file_local_value();
    test_import_alias_conflicts_with_other_file_local_value();
    test_import_alias_conflicts_with_imported_short_name();
    test_lazy_ambiguity_import_vs_import();
    test_lazy_ambiguity_import_vs_local_type();
    test_lazy_ambiguity_import_vs_local_value();
    test_lazy_ambiguity_unused_no_error();
    test_lazy_ambiguity_resolved_by_qualified_path();
    test_lazy_ambiguity_resolved_by_alias();
    test_lazy_ambiguity_import_vs_other_file_in_same_module();
    test_lazy_ambiguity_spec_reference();
    test_lazy_ambiguity_cross_kind_import_func_vs_local_let();
    test_duplicate_use_alias_in_same_file();
    test_unknown_use_module_rejected_without_import_query();
    test_external_use_module_accepted_via_import_query();
    test_external_imported_function_argument_type_mismatch();
    test_external_imported_function_argument_type_match();
    test_external_imported_field_type_participates_in_typecheck();
    test_external_imported_decl_bound_let_member_rejects_object_literal_rebind();
    test_external_imported_ctor_bound_let_member_rejects_object_literal_rebind();
    test_external_imported_static_members_are_visible();
    test_external_full_path_type_refs_do_not_require_use();
    test_external_alias_type_ref_still_requires_use_alias();
    test_external_imported_declared_specs_enable_spec_coercion();
    test_external_imported_enum_item_participates_in_typecheck();
    test_external_imported_enum_conflicts_with_local_type_name();
    test_external_imported_private_enum_is_not_visible();
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
    test_numeric_literal_integer_adapts_to_explicit_float_targets();
    test_numeric_float_literal_to_integer_target_is_rejected();
    test_typed_numeric_binding_requires_explicit_conversion_on_let_assignment();
    test_numeric_literal_adapts_to_float_targets_on_var_binding();
    test_typed_numeric_binding_requires_explicit_conversion_on_var_binding();
    test_numeric_literal_argument_adapts_to_float_targets();
    test_typed_numeric_argument_requires_explicit_conversion_for_float_parameter();
    test_member_assignment_numeric_literal_adapts_to_float_field();
    test_member_assignment_typed_numeric_binding_requires_explicit_conversion();
    test_numeric_constant_expression_adapts_to_float_target();
    test_numeric_expression_with_identifier_requires_explicit_conversion();
    test_object_literal_reports_unknown_field();
    test_object_literal_requires_object_type_target();
    test_object_literal_accepts_constructor_call_target();
    test_constructor_call_uses_implicit_default_constructor();
    test_constructor_call_reports_missing_zero_arg_constructor();
    test_constructor_call_selects_overload_by_literal_type();
    test_constructor_call_selects_overload_by_inferred_local_binding();
    test_constructor_call_matches_generic_owner_type_param();
    test_constructor_call_reports_type_mismatch();
    test_constructor_call_rejects_function_type();
    test_constructor_call_rejects_object_form_spec();
    test_object_literal_reports_inaccessible_imported_constructor();
    test_constructor_call_reports_inaccessible_imported_constructor();
    test_object_literal_constructor_call_reports_inaccessible_imported_constructor();
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
    test_object_form_spec_allows_method_same_name_as_spec();
    test_object_form_spec_rejects_finalizer_member();
    test_type_satisfies_spec_static_members();
    test_fit_satisfies_spec_static_method();
    test_spec_static_member_missing_rejected();
    test_spec_static_member_signature_mismatch_rejected();
    test_spec_static_field_missing_rejected();
    test_instance_access_of_static_member_rejected();
    test_generic_param_static_method_call_type_inference();
    test_generic_param_static_field_read_type_inference();
    test_generic_param_static_field_write_type_inference();
    test_spec_inherits_parent_static_member_constraint();
    test_generic_param_static_let_field_write_rejected();
    test_generic_param_unknown_static_member_rejected();
    test_union_form_spec_records_normalized_members();
    test_union_form_spec_rejects_type_declared_spec_clause();
    test_union_form_spec_rejects_fit_spec_clause();
    test_union_entry_records_exact_member_site();
    test_union_entry_ambiguous_spec_member_requires_explicit_cast();
    test_union_entry_explicit_cast_selects_spec_member();
    test_union_match_accepts_type_labels();
    test_union_match_rejects_literal_label();
    test_union_match_rejects_range_label();
    test_union_match_narrows_object_spec_member();
    test_union_member_access_requires_narrowing();
    test_union_equality_requires_narrowing();
    test_generic_union_form_accepts_concrete_member_matching();
    test_generic_union_form_rejects_mismatched_member();
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
    test_fit_builtin_method_callable_on_literal();
    test_fit_enum_method_callable_on_item();
    test_fit_enum_satisfies_spec_typed_parameter();
    test_fit_enum_satisfies_generic_constraint();
    test_generic_overload_constraint_excludes_candidate();
    test_generic_overload_selected_when_only_candidate();
    test_non_generic_overload_preferred_over_generic();
    test_fit_enum_missing_method_rejected();
    test_fit_enum_unknown_member_still_rejected();
    test_fit_array_method_callable_on_value();
    test_fit_builtin_target_rejects_specs_clause_without_body();
    test_fit_array_target_rejects_specs_clause_without_body();
    test_fit_array_target_element_type_param_visible_in_body();
    test_fit_array_target_element_type_param_does_not_leak();
    test_fit_user_type_path_still_uses_current_type_decl();
    test_fit_user_type_satisfaction_reuses_visible_fit_members();
    test_pu_builtin_self_fit_emits_no_orphan_info();
    test_pu_builtin_self_fit_visible_after_use_enables_method_call();
    test_external_imported_builtin_self_fit_method_visible();
    test_external_imported_array_self_fit_method_visible();
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
    test_static_members_semantic_resolution();
    test_generic_static_members_semantic_resolution();
    test_static_member_instance_access_is_rejected();
    test_duplicate_static_method_signature_is_rejected();
    test_finalizer_basic_ok();
    test_finalizer_rejects_multiple_per_type();
    test_finalizer_rejected_on_abi_type();
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

    test_enum_info_tracks_implicit_values();
    test_enum_info_tracks_explicit_values_and_cast_to_int();
    test_enum_rejects_mixed_explicit_and_implicit_values();
    test_enum_rejects_duplicate_item_name();
    test_enum_rejects_duplicate_item_value();
    test_enum_rejects_int_to_enum_cast();
    test_enum_relational_compare_is_rejected();
    test_enum_is_valid_in_ordinary_type_positions();
    test_enum_rejects_different_enum_assignment();
    test_enum_rejects_different_enum_equality_compare();
    test_enum_rejects_different_enum_cast();
    test_enum_arithmetic_is_rejected();
    test_enum_abi_surfaces_accept_enum_signatures();
    test_enum_address_of_matches_int_pointer_rules();
    test_extern_function_accepts_enum_types();
    test_abi_type_accepts_enum_field();
    test_value_kind_enum_is_trivial();

    test_value_kind_builtin_classifies_string_as_managed_pointer();
    test_value_kind_builtin_classifies_numerics_and_bool_as_trivial();
    test_value_kind_builtin_classifies_void_as_trivial();
    test_value_kind_builtin_unknown_name_defaults_to_trivial();
    test_value_kind_user_type_is_managed_pointer();
    test_value_kind_object_form_spec_is_aggregate();
    test_value_kind_callable_form_spec_is_managed_pointer();
    test_value_kind_null_decl_is_trivial();
    test_value_kind_non_type_decl_is_trivial();

    test_generic_function_decl_ok();
    test_generic_type_decl_ok();
    test_generic_spec_decl_ok();
    test_generic_function_call_wildcard_ok();
    test_generic_explicit_type_args_ok();
    test_generic_explicit_type_args_adapts_tuple_literal();
    test_generic_explicit_type_args_adapts_lambda_literal();
    test_generic_exact_non_generic_overload_is_preferred();
    test_generic_type_param_constraint_must_be_spec();
    test_generic_type_ref_arity_too_many();
    test_generic_non_generic_type_with_type_args_rejected();
    test_generic_type_with_finalizer_rejected();
    test_generic_explicit_type_args_arity_mismatch();
    test_generic_type_constructor_explicit_type_args_ok();
    test_generic_type_constructor_explicit_type_args_arity_mismatch();
    test_generic_method_type_param_collides_with_type_param();
    test_generic_function_two_type_params_ok();
    test_generic_spec_generic_parent_forwarding_ok();
    test_generic_duplicate_fn_by_type_param_name_only_rejected();
    test_generic_same_name_same_arity_different_constraint_rejected();
    test_tuple_literal_expected_contexts_pass();
    test_tuple_literal_without_target_is_rejected();
    test_tuple_literal_shape_mismatch_is_rejected();
    test_tuple_named_conversion_rules();
    test_tuple_destructuring_semantics();
    test_tuple_destructuring_non_tuple_is_rejected();
    test_tuple_item_assignment_is_rejected();
    test_tuple_whole_assignment_semantics();
    test_tuple_type_constraint_is_rejected();
    test_tuple_fit_spec_coercion_semantics();
    test_variadic_accepts_zero_one_many_arguments();
    test_fixed_and_variadic_parameters_accept_calls();
    test_variadic_rejects_mismatched_element_type();
    test_variadic_rejects_existing_array_argument();
    test_variadic_callable_spec_lambda_call_ok();
    test_variadic_overload_conflict_rejected();
    test_variadic_single_fixed_and_variadic_overload_conflict_rejected();
    test_variadic_spec_satisfaction_mismatch_rejected();
    test_iterator_multiple_iterable_rejected();
    test_iterator_multiple_iterator_rejected();
    test_iterator_both_annotations_rejected();
    test_iterator_iterable_with_params_rejected();
    test_iterator_iterator_with_params_rejected();
    test_iterator_iterable_return_no_iterator_rejected();
    test_iterator_return_not_tuple_rejected();
    test_iterator_for_in_not_iterable_rejected();
    test_iterator_basic_iterable_ok();
    test_iterator_self_cursor_ok();

    test_if_expr_then_throw_else_value_accepted();
    test_if_expr_then_value_else_throw_accepted();
    test_if_expr_both_branches_throw_accepted();
    test_match_expr_partial_throw_accepted();
    test_match_expr_all_branches_throw_accepted();

    test_if_expr_branch_literal_adapts_to_non_literal_branch();
    test_if_expr_branch_literal_adapts_to_explicit_target();
    test_if_expr_branch_literal_out_of_range_rejected();
    test_if_expr_branch_both_literals_no_target_accepted();
    test_if_expr_branch_literal_adapts_to_union_member();
    test_if_expr_branch_literal_return_value_adapts();

    test_match_expr_branch_literal_adapts_to_non_literal_branch();
    test_match_expr_branch_literal_adapts_to_explicit_target();
    test_match_expr_branch_literal_out_of_range_rejected();
    test_match_expr_branch_all_literals_no_target_accepted();
    test_match_expr_branch_literal_adapts_to_union_member();
    test_match_expr_branch_literal_return_value_adapts();

    test_try_expr_catch_literal_adapts_to_body_type();
    test_try_expr_body_literal_adapts_to_explicit_target();
    test_try_expr_catch_literal_adapts_to_explicit_target();
    test_try_expr_catch_literal_out_of_range_rejected();
    test_try_expr_catch_literal_adapts_to_union_member();
    test_try_expr_catch_literal_return_value_adapts();

    test_array_literal_adapts_first_non_literal();
    test_array_literal_adapts_first_element_literal();
    test_array_literal_all_literals_no_target_accepted();
    test_array_literal_non_literal_type_mismatch_rejected();
    test_array_literal_out_of_range_rejected();

    test_tuple_literal_element_adapts_to_field_type();

    puts("semantic tests passed");
    return 0;
}
