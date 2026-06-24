#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static void assert_slice_text(FengSlice slice, const char *expected) {
    size_t expected_length = expected != NULL ? strlen(expected) : 0U;

    ASSERT(slice.length == expected_length);
    if (expected == NULL) {
        ASSERT(slice.data == NULL);
        return;
    }

    ASSERT(slice.data != NULL);
    ASSERT(memcmp(slice.data, expected, expected_length) == 0);
}

static void test_top_level_declarations(void) {
    const char *source =
        "open module libc.math;\n"
        "import libc.base;\n"
        "import libc.extra as extra;\n"
        "let point_lib = \"./libpoint.so\";\n"
        "@cdecl(point_lib)\n"
        "extern func point_distance(p1: Point, p2: Point): float;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: i32;\n"
        "    var y: i32;\n"
        "}\n"
        "@abi\n"
        "spec PointCallback(p: Point): void;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "top_level.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_visibility == FENG_VISIBILITY_PUBLIC);
    ASSERT(program->module_segment_count == 2U);
    ASSERT(program->use_count == 2U);
    ASSERT(program->uses[1].has_alias);
    ASSERT(program->declaration_count == 4U);

    ASSERT(program->declarations[0]->kind == FENG_DECL_GLOBAL_BINDING);
    ASSERT(program->declarations[1]->kind == FENG_DECL_FUNCTION);
    ASSERT(program->declarations[1]->is_extern);
    ASSERT(program->declarations[1]->annotation_count == 1U);
    ASSERT(program->declarations[1]->annotations[0].builtin_kind == FENG_ANNOTATION_CDECL);

    ASSERT(program->declarations[2]->kind == FENG_DECL_TYPE);
    ASSERT(!program->declarations[2]->is_extern);
    ASSERT(program->declarations[2]->annotation_count == 1U);
    ASSERT(program->declarations[2]->annotations[0].builtin_kind == FENG_ANNOTATION_ABI);
    ASSERT(program->declarations[2]->as.type_decl.member_count == 2U);

    ASSERT(program->declarations[3]->kind == FENG_DECL_SPEC);
    ASSERT(program->declarations[3]->annotation_count == 1U);
    ASSERT(program->declarations[3]->annotations[0].builtin_kind == FENG_ANNOTATION_ABI);
    ASSERT(program->declarations[3]->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE);
    ASSERT(program->declarations[3]->as.spec_decl.as.callable.param_count == 1U);
    ASSERT(program->declarations[3]->as.spec_decl.as.callable.return_type != NULL);

    feng_program_free(program);
}

static void test_annotation_accepts_two_arguments(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"m\", \"fabs\")\n"
        "extern func abs_value(x: double): double;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "annotation_two_args.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    ASSERT(program->declarations[0]->kind == FENG_DECL_FUNCTION);
    ASSERT(program->declarations[0]->annotation_count == 1U);
    ASSERT(program->declarations[0]->annotations[0].builtin_kind == FENG_ANNOTATION_CDECL);
    ASSERT(program->declarations[0]->annotations[0].arg_count == 2U);
    ASSERT(program->declarations[0]->annotations[0].args[0]->kind == FENG_EXPR_STRING);
    ASSERT(program->declarations[0]->annotations[0].args[1]->kind == FENG_EXPR_STRING);
    assert_slice_text(program->declarations[0]->annotations[0].args[0]->as.string, "\"m\"");
    assert_slice_text(program->declarations[0]->annotations[0].args[1]->as.string, "\"fabs\"");

    feng_program_free(program);
}

static void test_extern_rejects_non_function_top_level_declarations(void) {
    static const char *kCases[] = {
        "module demo.main;\nextern let value: i32;\n",
        "module demo.main;\nextern type Point {\n    var x: i32;\n}\n",
        "module demo.main;\nextern enum Status {\n    ok = 0;\n}\n",
        "module demo.main;\nextern spec Reader {\n}\n",
        "module demo.main;\nextern fit User: Named {\n}\n"
    };

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(kCases[i], strlen(kCases[i]), "extern_non_fn.f", &program, &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message,
                      "'extern' can only be applied to top-level 'func' declarations") != NULL);
    }
}

static void test_statements_and_expressions(void) {
    const char *source =
        "module demo.main;\n"
        "func main(args: string[]) {\n"
        "    let label = if age >= 18 { \"adult\"; } else { \"minor\"; };\n"
        "    let stage = match age { 0 { \"婴儿\"; } 18 { \"成年\"; } else { \"青年\"; } };\n"
        "    for var i = 0; i < 3; i = i + 1 {\n"
        "        if i == 1 {\n"
        "            continue;\n"
        "        } else {\n"
        "            print(i);\n"
        "        }\n"
        "    }\n"
        "    return (i32)1;\n"
        "}\n"
        "func make_adder(base: i32): IntToInt {\n"
        "    return (x: i32) -> base + x;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    FengDecl *main_decl;
    FengBlock *main_body;

    ASSERT(feng_parse_source(source, strlen(source), "control_flow.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);

    main_decl = program->declarations[0];
    ASSERT(main_decl->kind == FENG_DECL_FUNCTION);
    ASSERT(main_decl->as.function_decl.body != NULL);
    main_body = main_decl->as.function_decl.body;
    ASSERT(main_body->statement_count == 4U);

    ASSERT(main_body->statements[0]->kind == FENG_STMT_BINDING);
    ASSERT(main_body->statements[0]->as.binding.initializer->kind == FENG_EXPR_IF);
    ASSERT(main_body->statements[1]->kind == FENG_STMT_BINDING);
    ASSERT(main_body->statements[1]->as.binding.initializer->kind == FENG_EXPR_MATCH);
    ASSERT(main_body->statements[2]->kind == FENG_STMT_FOR);
    ASSERT(main_body->statements[3]->kind == FENG_STMT_RETURN);
    ASSERT(main_body->statements[3]->as.return_value->kind == FENG_EXPR_CAST);

    ASSERT(program->declarations[1]->kind == FENG_DECL_FUNCTION);
    ASSERT(program->declarations[1]->as.function_decl.body->statement_count == 1U);
    ASSERT(program->declarations[1]->as.function_decl.body->statements[0]->kind == FENG_STMT_RETURN);
    ASSERT(program->declarations[1]->as.function_decl.body->statements[0]->as.return_value->kind == FENG_EXPR_LAMBDA);

    feng_program_free(program);
}

static void test_try_block_form_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    try {\n"
        "        1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "try_block_rejected.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);

}

static void test_try_expression_with_typed_catches(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let value = try parse() catch err: ParseError { 8080; } catch problem: unknown { 9090; };\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;
    const FengExpr *try_expr;

    ASSERT(feng_parse_source(source, strlen(source), "try_expr.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);

    body = program->declarations[0]->as.function_decl.body;
    ASSERT(body->statement_count == 2U);
    ASSERT(body->statements[0]->kind == FENG_STMT_BINDING);
    try_expr = body->statements[0]->as.binding.initializer;
    ASSERT(try_expr->kind == FENG_EXPR_TRY);
    ASSERT(try_expr->as.try_expr.body->kind == FENG_EXPR_CALL);
    ASSERT(try_expr->as.try_expr.clause_count == 2U);

    assert_slice_text(try_expr->as.try_expr.clauses[0].name, "err");
    ASSERT(try_expr->as.try_expr.clauses[0].type->kind == FENG_TYPE_REF_NAMED);
    ASSERT(try_expr->as.try_expr.clauses[0].type->as.named.segment_count == 1U);
    assert_slice_text(try_expr->as.try_expr.clauses[0].type->as.named.segments[0], "ParseError");
    ASSERT(try_expr->as.try_expr.clauses[0].body->statement_count == 1U);

    assert_slice_text(try_expr->as.try_expr.clauses[1].name, "problem");
    ASSERT(try_expr->as.try_expr.clauses[1].type->kind == FENG_TYPE_REF_NAMED);
    ASSERT(try_expr->as.try_expr.clauses[1].type->as.named.segment_count == 1U);
    assert_slice_text(try_expr->as.try_expr.clauses[1].type->as.named.segments[0], "unknown");

    feng_program_free(program);
}

static void test_try_without_catch_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    try init_runtime();\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "try_no_catch.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_defer_block_parses(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    var x = 1;\n"
        "    defer {\n"
        "        x = x + 1;\n"
        "    }\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;
    const FengStmt *defer_stmt;
    const FengBlock *defer_body;

    ASSERT(feng_parse_source(source, strlen(source), "defer_block.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);

    body = program->declarations[0]->as.function_decl.body;
    /* var x = 1; defer { ... }; return x; */
    ASSERT(body->statement_count == 3U);
    ASSERT(body->statements[0]->kind == FENG_STMT_BINDING);
    defer_stmt = body->statements[1];
    ASSERT(defer_stmt->kind == FENG_STMT_DEFER);
    defer_body = defer_stmt->as.defer_block;
    ASSERT(defer_body != NULL);
    ASSERT(defer_body->statement_count == 1U);
    ASSERT(defer_body->statements[0]->kind == FENG_STMT_ASSIGN);
    ASSERT(body->statements[2]->kind == FENG_STMT_RETURN);

    feng_program_free(program);
}

static void test_defer_without_block_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run() {\n"
        "    defer cleanup();\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "defer_no_block.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_runtime_annotation_on_extern_function(void) {
    const char *source =
        "module demo.main;\n"
        "@runtime\n"
        "extern func feng_string_length(value: string): i64;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "runtime_extern.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    ASSERT(program->declarations[0]->kind == FENG_DECL_FUNCTION);
    ASSERT(program->declarations[0]->is_extern);
    ASSERT(program->declarations[0]->annotation_count == 1U);
    ASSERT(program->declarations[0]->annotations[0].builtin_kind == FENG_ANNOTATION_RUNTIME);
    ASSERT(program->declarations[0]->as.function_decl.param_count == 1U);
    ASSERT(program->declarations[0]->as.function_decl.return_type != NULL);

    feng_program_free(program);
}

static void test_member_annotations_and_constructors(void) {
    const char *source =
        "module demo.user;\n"
        "type User {\n"
        "    open var name: string;\n"
        "    @memo\n"
        "    open let id: i32 = 1;\n"
        "    open let created_at: i32;\n"
        "    @memo(created_at)\n"
        "    func User(ts: i32) {\n"
        "        self.created_at = ts;\n"
        "    }\n"
        "    open func info(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    FengDecl *decl;

    ASSERT(feng_parse_source(source, strlen(source), "members.f", &program, &error));
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl->kind == FENG_DECL_TYPE);
    ASSERT(decl->as.type_decl.member_count == 5U);
    ASSERT(decl->as.type_decl.members[1]->annotation_count == 1U);
    ASSERT(decl->as.type_decl.members[1]->annotations[0].builtin_kind == FENG_ANNOTATION_CUSTOM);
    ASSERT(decl->as.type_decl.members[2]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(decl->as.type_decl.members[3]->kind == FENG_TYPE_MEMBER_CONSTRUCTOR);
    ASSERT(decl->as.type_decl.members[3]->annotation_count == 1U);
    ASSERT(decl->as.type_decl.members[3]->annotations[0].arg_count == 1U);
    ASSERT(decl->as.type_decl.members[3]->as.callable.body != NULL);
    ASSERT(decl->as.type_decl.members[4]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(decl->as.type_decl.members[4]->as.callable.body != NULL);

    feng_program_free(program);
}

static void test_static_members_parse(void) {
    const char *source =
        "module demo.static_members;\n"
        "type Counter {\n"
        "    static let seed: i32 = 1;\n"
        "    open static var count: i32 = 0;\n"
        "    open static func create(): Counter {\n"
        "        return Counter();\n"
        "    }\n"
        "    func value(): i32 {\n"
        "        return 1;\n"
        "    }\n"
        "}\n"
        "fit Counter {\n"
        "    open static func fromSeed(): Counter {\n"
        "        return Counter();\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *counter;
    const FengDecl *fit;

    ASSERT(feng_parse_source(source, strlen(source), "static_members.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);

    counter = program->declarations[0];
    ASSERT(counter->kind == FENG_DECL_TYPE);
    ASSERT(counter->as.type_decl.member_count == 4U);
    ASSERT(counter->as.type_decl.members[0]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(counter->as.type_decl.members[0]->is_static);
    ASSERT(counter->as.type_decl.members[1]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(counter->as.type_decl.members[1]->is_static);
    ASSERT(counter->as.type_decl.members[2]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(counter->as.type_decl.members[2]->is_static);
    ASSERT(counter->as.type_decl.members[2]->visibility == FENG_VISIBILITY_PUBLIC);
    ASSERT(counter->as.type_decl.members[3]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(!counter->as.type_decl.members[3]->is_static);

    fit = program->declarations[1];
    ASSERT(fit->kind == FENG_DECL_FIT);
    ASSERT(fit->as.fit_decl.member_count == 1U);
    ASSERT(fit->as.fit_decl.members[0]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(fit->as.fit_decl.members[0]->is_static);
    ASSERT(fit->as.fit_decl.members[0]->visibility == FENG_VISIBILITY_PUBLIC);

    feng_program_free(program);
}

static void test_spec_static_members_parse(void) {
    const char *source =
        "module demo.spec_static;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "    static let tag: string;\n"
        "    static var current: i32;\n"
        "    func name(): string;\n"
        "}\n"
        "spec SameName {\n"
        "    static func SameName(): i32;\n"
        "    func SameName(): i32;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *factory;
    const FengDecl *same_name;

    ASSERT(feng_parse_source(source, strlen(source), "spec_static_members.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);

    factory = program->declarations[0];
    ASSERT(factory->kind == FENG_DECL_SPEC);
    ASSERT(factory->as.spec_decl.as.object.member_count == 4U);
    ASSERT(factory->as.spec_decl.as.object.members[0]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(factory->as.spec_decl.as.object.members[0]->is_static);
    ASSERT(factory->as.spec_decl.as.object.members[1]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(factory->as.spec_decl.as.object.members[1]->is_static);
    ASSERT(factory->as.spec_decl.as.object.members[2]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(factory->as.spec_decl.as.object.members[2]->is_static);
    ASSERT(factory->as.spec_decl.as.object.members[3]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(!factory->as.spec_decl.as.object.members[3]->is_static);

    same_name = program->declarations[1];
    ASSERT(same_name->kind == FENG_DECL_SPEC);
    ASSERT(same_name->as.spec_decl.as.object.member_count == 2U);
    /* spec 方法名 == spec 名视为普通方法,无构造器概念 */
    ASSERT(same_name->as.spec_decl.as.object.members[0]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(same_name->as.spec_decl.as.object.members[0]->is_static);
    ASSERT(same_name->as.spec_decl.as.object.members[1]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(!same_name->as.spec_decl.as.object.members[1]->is_static);

    feng_program_free(program);
}

static void test_spec_static_member_parse_errors(void) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {
            "module demo.bad;\n"
            "spec Factory {\n"
            "    static let zero: i32 = 0;\n"
            "}\n",
            "spec field declarations cannot have an initializer"
        },
        {
            "module demo.bad;\n"
            "spec Factory {\n"
            "    static func make(): i32 {}\n"
            "}\n",
            "spec method signatures must end with ';' and cannot have a body"
        }
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(cases[i].source,
                                  strlen(cases[i].source),
                                  "spec_static_member_error.f",
                                  &program,
                                  &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message, cases[i].message) != NULL);
    }
}

static void test_static_member_parse_errors(void) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {
            "module demo.bad;\n"
            "type Counter {\n"
            "    static func Counter() {}\n"
            "}\n",
            "constructors cannot be declared 'static'"
        },
        {
            "module demo.bad;\n"
            "type Counter {\n"
            "    static func ~Counter() {}\n"
            "}\n",
            "finalizers cannot be declared 'static'"
        },
        {
            "module demo.bad;\n"
            "type Counter {\n"
            "    static open let value: i32 = 0;\n"
            "}\n",
            "expected type member declaration"
        },
        {
            "module demo.bad;\n"
            "type Counter {}\n"
            "fit Counter {\n"
            "    static let value: i32 = 0;\n"
            "}\n",
            "fit blocks cannot declare 'static let' or 'static var'"
        },
        {
            "module demo.bad;\n"
            "type Counter {}\n"
            "fit Counter {\n"
            "    static var value: i32 = 0;\n"
            "}\n",
            "fit blocks cannot declare 'static let' or 'static var'"
        }
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(cases[i].source,
                                  strlen(cases[i].source),
                                  "static_member_error.f",
                                  &program,
                                  &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message, cases[i].message) != NULL);
    }
}

static void test_enum_declarations_parse(void) {
    const char *source =
        "module demo.enums;\n"
        "open enum Status {\n"
        "    Ok,\n"
        "    NotFound\n"
        "}\n"
        "enum SignedCode {\n"
        "    Ok = 200,\n"
        "    Retry = -1\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "enum_parse.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);

    ASSERT(program->declarations[0]->kind == FENG_DECL_ENUM);
    ASSERT(program->declarations[0]->visibility == FENG_VISIBILITY_PUBLIC);
    assert_slice_text(program->declarations[0]->as.enum_decl.name, "Status");
    ASSERT(program->declarations[0]->as.enum_decl.item_count == 2U);
    assert_slice_text(program->declarations[0]->as.enum_decl.items[0].name, "Ok");
    ASSERT(!program->declarations[0]->as.enum_decl.items[0].has_explicit_value);
    assert_slice_text(program->declarations[0]->as.enum_decl.items[1].name, "NotFound");
    ASSERT(!program->declarations[0]->as.enum_decl.items[1].has_explicit_value);

    ASSERT(program->declarations[1]->kind == FENG_DECL_ENUM);
    assert_slice_text(program->declarations[1]->as.enum_decl.name, "SignedCode");
    ASSERT(program->declarations[1]->as.enum_decl.item_count == 2U);
    ASSERT(program->declarations[1]->as.enum_decl.items[0].has_explicit_value);
    ASSERT(program->declarations[1]->as.enum_decl.items[0].explicit_value == 200);
    ASSERT(program->declarations[1]->as.enum_decl.items[1].has_explicit_value);
    ASSERT(program->declarations[1]->as.enum_decl.items[1].explicit_value == -1);

    feng_program_free(program);
}

static void test_ast_source_tokens(void) {
    const char *source =
        "open module demo.main;\n"
        "import demo.base;\n"
        "@memo\n"
        "func main(arg: i32) {\n"
        "    let answer: i32 = 42;\n"
        "    return answer;\n"
        "}\n"
        "type User {\n"
        "    open let id: i32 = 1;\n"
        "    func User(value: i32) {\n"
        "        self.id = value;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "tokens.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_token.line == 1U);
    ASSERT(program->uses[0].token.line == 2U);
    ASSERT(program->declarations[0]->annotations[0].token.line == 3U);
    ASSERT(program->declarations[0]->token.line == 4U);
    ASSERT(program->declarations[0]->as.function_decl.token.line == 4U);
    ASSERT(program->declarations[0]->as.function_decl.params[0].token.line == 4U);
    ASSERT(program->declarations[0]->as.function_decl.body->token.line == 4U);
    ASSERT(program->declarations[0]->as.function_decl.body->statements[0]->token.line == 5U);
    ASSERT(program->declarations[0]->as.function_decl.body->statements[1]->token.line == 6U);
    ASSERT(program->declarations[1]->token.line == 8U);
    ASSERT(program->declarations[1]->as.type_decl.members[0]->token.line == 9U);
    ASSERT(program->declarations[1]->as.type_decl.members[0]->as.field.initializer->token.line == 9U);
    ASSERT(program->declarations[1]->as.type_decl.members[1]->token.line == 10U);
    ASSERT(program->declarations[1]->as.type_decl.members[1]->as.callable.params[0].token.line == 10U);
    ASSERT(program->declarations[1]->as.type_decl.members[1]->as.callable.body->token.line == 10U);

    feng_program_free(program);
}

static void test_type_field_inferred_initializers(void) {
    const char *source =
        "module demo.fields;\n"
        "type UserType {}\n"
        "type User {\n"
        "    let id: i32 = 0;\n"
        "    let x: UserType;\n"
        "    let y = UserType();\n"
        "    let z = UserType {};\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *user = NULL;

    ASSERT(feng_parse_source(source, strlen(source), "field_infer.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);
    user = program->declarations[1];
    test_try_block_form_is_rejected(); // Ensure try block form is rejected
    ASSERT(user->as.type_decl.member_count == 4U);
    ASSERT(user->as.type_decl.members[0]->as.field.type != NULL);
    ASSERT(user->as.type_decl.members[0]->as.field.initializer != NULL);
    ASSERT(user->as.type_decl.members[1]->as.field.type != NULL);
    ASSERT(user->as.type_decl.members[1]->as.field.initializer == NULL);
    ASSERT(user->as.type_decl.members[2]->as.field.type == NULL);
    ASSERT(user->as.type_decl.members[2]->as.field.initializer != NULL);
    ASSERT(user->as.type_decl.members[2]->as.field.initializer->kind == FENG_EXPR_CALL);
    ASSERT(user->as.type_decl.members[3]->as.field.type == NULL);
    ASSERT(user->as.type_decl.members[3]->as.field.initializer != NULL);
    ASSERT(user->as.type_decl.members[3]->as.field.initializer->kind == FENG_EXPR_OBJECT_LITERAL);

    feng_program_free(program);
}

static void test_doc_comments_bind_to_declarations_and_members(void) {
    const char *source =
        "module demo.docs;\n"
        "/** top func doc */\n"
        "@memo\n"
        "open func run() {}\n"
        "/** type doc */\n"
        "type User {\n"
        "    /** field doc */\n"
        "    @memo\n"
        "    open let id: i32;\n"
        "    /** method doc */\n"
        "    open func info(): i32 {\n"
        "        return self.id;\n"
        "    }\n"
        "}\n"
        "spec Named {\n"
        "    /** spec member doc */\n"
        "    func name(): i32;\n"
        "}\n"
        "fit User {\n"
        "    /** fit member doc */\n"
        "    func extra(): i32 {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "docs_bind.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 4U);

    assert_slice_text(program->declarations[0]->doc_comment, "/** top func doc */");
    assert_slice_text(program->declarations[1]->doc_comment, "/** type doc */");
    assert_slice_text(program->declarations[1]->as.type_decl.members[0]->doc_comment,
                      "/** field doc */");
    assert_slice_text(program->declarations[1]->as.type_decl.members[1]->doc_comment,
                      "/** method doc */");
    assert_slice_text(program->declarations[2]->as.spec_decl.as.object.members[0]->doc_comment,
                      "/** spec member doc */");
    assert_slice_text(program->declarations[3]->as.fit_decl.members[0]->doc_comment,
                      "/** fit member doc */");

    feng_program_free(program);
}

static void test_doc_comments_require_immediate_declaration(void) {
    const char *source =
        "module demo.docs;\n"
        "/** blank line breaks */\n"
        "\n"
        "func first() {}\n"
        "/** normal comment breaks */\n"
        "// separator\n"
        "func second() {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "docs_invalid_bind.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);
    assert_slice_text(program->declarations[0]->doc_comment, NULL);
    assert_slice_text(program->declarations[1]->doc_comment, NULL);

    feng_program_free(program);
}

static void test_parse_error(void) {
    const char *source = "func main(args: string[]) {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "error.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_parse_error_after_annotation_semicolon(void) {
    const char *source =
        "module demo.main;\n"
        "@cdecl(\"libc\");\n"
        "extern func print(msg: string*): void;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "annotation_error.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "annotation must be followed immediately by a declaration") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_SEMICOLON);
}

static void test_parse_error_top_level_fn_missing_body(void) {
    const char *source =
        "module demo.main;\n"
        "func point_sum(a: i32, b: i32): i32;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_fn_body.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "function declarations must provide a body '{...}'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_SEMICOLON);
}

static void test_parse_error_top_level_fn_missing_body_without_return_type(void) {
    const char *source =
        "module demo.main;\n"
        "func print(msg: string);\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_fn_body_no_return.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "function declarations must provide a body '{...}'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_SEMICOLON);
}

static void test_parse_error_top_level_fn_missing_body_with_void_return(void) {
    const char *source =
        "module demo.main;\n"
        "func print(msg: string): void;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_fn_body_void_return.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "function declarations must provide a body '{...}'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_SEMICOLON);
}

static void test_parse_error_extern_fn_with_body(void) {
    const char *source =
        "module demo.main;\n"
        "extern func point_sum(a: i32, b: i32): i32 {\n"
        "    return a + b;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "extern_fn_with_body.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message,
                  "extern function declarations must end with ';' and cannot have a body '{...}'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_LBRACE);
}

static void test_parse_error_member_fn_missing_body(void) {
    const char *source =
        "module demo.user;\n"
        "type User {\n"
        "    func info(): string;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_member_fn_body.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "type methods and constructors must provide a body '{...}'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_SEMICOLON);
}

static void test_parse_error_extern_fn_inside_type(void) {
    const char *source =
        "module demo.user;\n"
        "type User {\n"
        "    extern func info(): string*;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "extern_fn_in_type.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "type members cannot use 'extern func'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_KW_EXTERN);
}

static void test_parse_error_enum_member_declaration(void) {
    const char *source =
        "module demo.enums;\n"
        "enum Bad {\n"
        "    let code: i32;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "enum_member_error.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message,
                  "enum declarations only allow item names and optional integer literal initializers") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_KW_LET);
}

static void test_parse_error_enum_initializer_expression(void) {
    const char *source =
        "module demo.enums;\n"
        "enum Bad {\n"
        "    Value = 1 + 2\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "enum_initializer_error.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "enum item initializer must be a single integer literal") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_PLUS);
}

static void test_parse_error_empty_enum(void) {
    const char *source =
        "module demo.enums;\n"
        "enum Empty {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "enum_empty_error.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "enum declarations must declare at least one item") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_RBRACE);
}

static void test_parse_error_missing_top_level_fn_keyword(void) {
    const char *source =
        "module demo.main;\n"
        "main(args: string[]) {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_fn.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "top-level function declarations must start with 'func'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_IDENTIFIER);
}

static void test_parse_error_missing_member_fn_keyword(void) {
    const char *source =
        "module demo.user;\n"
        "type User {\n"
        "    info(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_member_fn.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "type methods and constructors must start with 'func'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_IDENTIFIER);
}

static void test_parse_error_missing_member_binding_keyword(void) {
    const char *source =
        "module demo.user;\n"
        "type User {\n"
        "    name: string;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_member_binding_kw.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "type fields must start with 'let' or 'var'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_IDENTIFIER);
}

static void test_parse_error_missing_local_binding_keyword(void) {
    const char *source =
        "module demo.main;\n"
        "func main(args: string[]) {\n"
        "    name: string = \"Houfeng\";\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_local_binding_kw.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "local bindings must start with 'let' or 'var'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_IDENTIFIER);
}

static void test_parse_error_missing_identifier_in_qualified_name(void) {
    const char *source =
        "module demo.;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_qualified_name_part.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "expected an identifier after '.' in a qualified name") != NULL);
}

static void test_parse_error_missing_identifier_in_member_access(void) {
    const char *source =
        "module demo.main;\n"
        "func main(args: string[]) {\n"
        "    return user.;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_member_access_name.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "expected an identifier after '.' in member access") != NULL);
}

static void test_finalizer_declaration(void) {
    const char *source =
        "module demo.user;\n"
        "type Buffer {\n"
        "    open var size: i32;\n"
        "    func Buffer(s: i32) {}\n"
        "    func ~Buffer() {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    FengDecl *decl;

    ASSERT(feng_parse_source(source, strlen(source), "fin.f", &program, &error));
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl->kind == FENG_DECL_TYPE);
    ASSERT(decl->as.type_decl.member_count == 3U);
    ASSERT(decl->as.type_decl.members[1]->kind == FENG_TYPE_MEMBER_CONSTRUCTOR);
    ASSERT(decl->as.type_decl.members[2]->kind == FENG_TYPE_MEMBER_FINALIZER);
    ASSERT(decl->as.type_decl.members[2]->as.callable.param_count == 0U);
    ASSERT(decl->as.type_decl.members[2]->as.callable.return_type == NULL);

    feng_program_free(program);
}

static void test_finalizer_declaration_with_void_return(void) {
    const char *source =
        "module demo.user;\n"
        "type Buffer {\n"
        "    func ~Buffer(): void {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "fin_void.f", &program, &error));
    ASSERT(program->declarations[0]->as.type_decl.members[0]->kind == FENG_TYPE_MEMBER_FINALIZER);
    feng_program_free(program);
}

static void test_constructor_with_void_return_type(void) {
    const char *source =
        "module demo.user;\n"
        "type Box {\n"
        "    func Box(): void {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "ctor_void.f", &program, &error));
    ASSERT(program->declarations[0]->as.type_decl.members[0]->kind == FENG_TYPE_MEMBER_CONSTRUCTOR);
    feng_program_free(program);
}

static void test_parse_error_constructor_with_non_void_return(void) {
    const char *source =
        "module demo.user;\n"
        "type Box {\n"
        "    func Box(): i32 { return 0; }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "ctor_bad.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "constructor must not declare a non-void return type") != NULL);
}

static void test_parse_error_finalizer_with_params(void) {
    const char *source =
        "module demo.user;\n"
        "type Box {\n"
        "    func ~Box(x: i32) {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "fin_params.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "finalizer must not declare any parameters") != NULL);
}

static void test_parse_error_finalizer_with_non_void_return(void) {
    const char *source =
        "module demo.user;\n"
        "type Box {\n"
        "    func ~Box(): i32 { return 0; }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "fin_ret.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "finalizer return type must be omitted or ': void'") != NULL);
}

static void test_parse_error_finalizer_name_mismatch(void) {
    const char *source =
        "module demo.user;\n"
        "type Box {\n"
        "    func ~Other() {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "fin_name.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "finalizer name must match the enclosing type name") != NULL);
}

static void test_parse_error_direct_finalizer_call(void) {
    const char *source =
        "module demo.user;\n"
        "func main(args: string[]) {\n"
        "    let b: Box = Box();\n"
        "    b.~Box();\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "direct_fin.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "finalizer cannot be invoked directly via '.~'") != NULL);
}

static void test_bitwise_expr_parsing(void) {
    /* Expected precedence: a | b ^ c & d == e << f   (shift > equality > & > ^ > |) */
    const char *source =
        "module demo.bits;\n"
        "func f(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32): bool {\n"
        "    return a | b ^ c & d == e << f;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengExpr *ret;
    const FengExpr *or_rhs;
    const FengExpr *xor_rhs;
    const FengExpr *and_rhs;
    const FengExpr *eq_rhs;

    ASSERT(feng_parse_source(source, strlen(source), "bits.f", &program, &error));
    ASSERT(program != NULL);
    ret = program->declarations[0]->as.function_decl.body->statements[0]->as.return_value;
    ASSERT(ret->kind == FENG_EXPR_BINARY);
    ASSERT(ret->as.binary.op == FENG_TOKEN_PIPE);
    ASSERT(ret->as.binary.left->kind == FENG_EXPR_IDENTIFIER);
    or_rhs = ret->as.binary.right;
    ASSERT(or_rhs->kind == FENG_EXPR_BINARY && or_rhs->as.binary.op == FENG_TOKEN_CARET);
    xor_rhs = or_rhs->as.binary.right;
    ASSERT(xor_rhs->kind == FENG_EXPR_BINARY && xor_rhs->as.binary.op == FENG_TOKEN_AMP);
    and_rhs = xor_rhs->as.binary.right;
    ASSERT(and_rhs->kind == FENG_EXPR_BINARY && and_rhs->as.binary.op == FENG_TOKEN_EQ);
    eq_rhs = and_rhs->as.binary.right;
    ASSERT(eq_rhs->kind == FENG_EXPR_BINARY && eq_rhs->as.binary.op == FENG_TOKEN_SHL);

    feng_program_free(program);
}

static void test_tilde_unary_parsing(void) {
    const char *source =
        "module demo.bits;\n"
        "func f(a: i32): i32 {\n"
        "    return ~a;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengExpr *ret;

    ASSERT(feng_parse_source(source, strlen(source), "tilde.f", &program, &error));
    ASSERT(program != NULL);
    ret = program->declarations[0]->as.function_decl.body->statements[0]->as.return_value;
    ASSERT(ret->kind == FENG_EXPR_UNARY);
    ASSERT(ret->as.unary.op == FENG_TOKEN_TILDE);
    feng_program_free(program);
}

static void test_address_of_unary_parsing(void) {
    const char *source =
        "module demo.addr;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: i32;\n"
        "}\n"
        "@abi\n"
        "func cmp(a: i32, b: i32): i32 {\n"
        "    return a - b;\n"
        "}\n"
        "func run(point: Point, msg: string, arr: i32[], value: i32) {\n"
        "    let data_ptr = &value;\n"
        "    let point_ptr = &point;\n"
        "    let str_ptr = &msg;\n"
        "    let arr_ptr = &arr;\n"
        "    let fn_ptr = &cmp;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;

    ASSERT(feng_parse_source(source, strlen(source), "address_of_unary.f", &program, &error));
    ASSERT(program != NULL);

    body = program->declarations[2]->as.function_decl.body;
    ASSERT(body->statement_count == 5U);

    for (size_t index = 0U; index < body->statement_count; ++index) {
        const FengStmt *stmt = body->statements[index];

        ASSERT(stmt->kind == FENG_STMT_BINDING);
        ASSERT(stmt->as.binding.initializer != NULL);
        ASSERT(stmt->as.binding.initializer->kind == FENG_EXPR_UNARY);
        ASSERT(stmt->as.binding.initializer->as.unary.op == FENG_TOKEN_AMP);
        ASSERT(stmt->as.binding.initializer->as.unary.operand != NULL);
        ASSERT(stmt->as.binding.initializer->as.unary.operand->kind == FENG_EXPR_IDENTIFIER);
    }

    feng_program_free(program);
}

static void test_address_of_and_bitwise_and_disambiguation(void) {
    const char *source =
        "module demo.addr;\n"
        "func run(a: i32, b: i32) {\n"
        "    let mixed = &a & b;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengExpr *init;

    ASSERT(feng_parse_source(source, strlen(source), "address_of_disambiguation.f", &program, &error));
    ASSERT(program != NULL);

    init = program->declarations[0]->as.function_decl.body->statements[0]->as.binding.initializer;
    ASSERT(init->kind == FENG_EXPR_BINARY);
    ASSERT(init->as.binary.op == FENG_TOKEN_AMP);
    ASSERT(init->as.binary.left != NULL);
    ASSERT(init->as.binary.left->kind == FENG_EXPR_UNARY);
    ASSERT(init->as.binary.left->as.unary.op == FENG_TOKEN_AMP);
    ASSERT(init->as.binary.left->as.unary.operand != NULL);
    ASSERT(init->as.binary.left->as.unary.operand->kind == FENG_EXPR_IDENTIFIER);
    ASSERT(init->as.binary.right != NULL);
    ASSERT(init->as.binary.right->kind == FENG_EXPR_IDENTIFIER);

    feng_program_free(program);
}

static void test_compound_assignment_parsing(void) {
    const char *source =
        "module demo.ops;\n"
        "func run() {\n"
        "    var total: float = 7.8;\n"
        "    total %= 3.2;\n"
        "    var mask: i32 = 1;\n"
        "    mask >>= 1;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;
    const FengStmt *assign_a;
    const FengStmt *assign_b;

    ASSERT(feng_parse_source(source, strlen(source), "compound_assign_parse.f", &program, &error));
    ASSERT(program != NULL);

    body = program->declarations[0]->as.function_decl.body;
    ASSERT(body->statement_count == 4U);

    assign_a = body->statements[1];
    ASSERT(assign_a->kind == FENG_STMT_ASSIGN);
    ASSERT(assign_a->as.assign.op == FENG_TOKEN_PERCENT_ASSIGN);
    ASSERT(assign_a->as.assign.target->kind == FENG_EXPR_IDENTIFIER);
    ASSERT(assign_a->as.assign.value->kind == FENG_EXPR_FLOAT);

    assign_b = body->statements[3];
    ASSERT(assign_b->kind == FENG_STMT_ASSIGN);
    ASSERT(assign_b->as.assign.op == FENG_TOKEN_SHR_ASSIGN);
    ASSERT(assign_b->as.assign.target->kind == FENG_EXPR_IDENTIFIER);
    ASSERT(assign_b->as.assign.value->kind == FENG_EXPR_INTEGER);

    feng_program_free(program);
}

static void test_lambda_block_body_parses(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let f = (a: i32) {\n"
        "        let b = a + 1;\n"
        "        return b;\n"
        "    };\n"
        "    return f(0);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *binding_stmt;
    const FengExpr *lambda_expr;

    ASSERT(feng_parse_source(source, strlen(source), "lambda_block.f", &program, &error));
    ASSERT(program != NULL);

    binding_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(binding_stmt->kind == FENG_STMT_BINDING);
    lambda_expr = binding_stmt->as.binding.initializer;
    ASSERT(lambda_expr->kind == FENG_EXPR_LAMBDA);
    ASSERT(lambda_expr->as.lambda.is_block_body);
    ASSERT(lambda_expr->as.lambda.body == NULL);
    ASSERT(lambda_expr->as.lambda.body_block != NULL);
    ASSERT(lambda_expr->as.lambda.body_block->statement_count == 2U);

    feng_program_free(program);
}

static void test_lambda_block_body_with_arrow_is_rejected(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): i32 {\n"
        "    let f = (a: i32) -> {\n"
        "        return a;\n"
        "    };\n"
        "    return f(0);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "lambda_arrow_block.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(strstr(error.message, "multi-line lambda") != NULL);
}

static void test_match_with_range_and_list_labels(void) {
    const char *source =
        "module demo.main;\n"
        "func run(age: i32): string {\n"
        "    return match age {\n"
        "        0 { \"婴儿\"; }\n"
        "        1...17 { \"未成年\"; }\n"
        "        18, 20, 22 { \"青年\"; }\n"
        "        else { \"其他\"; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *return_stmt;
    const FengExpr *match_expr;

    ASSERT(feng_parse_source(source, strlen(source), "match_labels.f", &program, &error));
    ASSERT(program != NULL);
    return_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    match_expr = return_stmt->as.return_value;
    ASSERT(match_expr->kind == FENG_EXPR_MATCH);
    ASSERT(match_expr->as.match_expr.branch_count == 3U);
    /* Branch 0: single literal 0 */
    ASSERT(match_expr->as.match_expr.branches[0].label_count == 1U);
    ASSERT(match_expr->as.match_expr.branches[0].labels[0].kind == FENG_MATCH_LABEL_VALUE);
    /* Branch 1: range 1...17 */
    ASSERT(match_expr->as.match_expr.branches[1].label_count == 1U);
    ASSERT(match_expr->as.match_expr.branches[1].labels[0].kind == FENG_MATCH_LABEL_RANGE);
    /* Branch 2: list 18, 20, 22 */
    ASSERT(match_expr->as.match_expr.branches[2].label_count == 3U);
    ASSERT(match_expr->as.match_expr.branches[2].labels[0].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_expr->as.match_expr.branches[2].labels[1].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_expr->as.match_expr.branches[2].labels[2].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_expr->as.match_expr.else_block != NULL);

    feng_program_free(program);
}

static void test_match_statement_form(void) {
    const char *source =
        "module demo.main;\n"
        "func run(age: i32) {\n"
        "    match age {\n"
        "        0 { print(\"zero\"); }\n"
        "        1...10 { print(\"small\"); }\n"
        "        else { print(\"other\"); }\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *match_stmt;

    ASSERT(feng_parse_source(source, strlen(source), "match_stmt.f", &program, &error));
    ASSERT(program != NULL);
    match_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(match_stmt->kind == FENG_STMT_MATCH);
    ASSERT(match_stmt->as.match_stmt.branch_count == 2U);
    ASSERT(match_stmt->as.match_stmt.else_block != NULL);

    feng_program_free(program);
}

static void test_match_enum_item_reference_labels_parse(void) {
    /* `EnumName.ItemName` is parsed as a FENG_MATCH_LABEL_TYPE whose type_ref
     * is a multi-segment named type. The semantic layer distinguishes enum
     * item references from union member type labels based on the target type
     * (enum vs union-form spec); the parser does not need to decide. */
    const char *source =
        "module demo.main;\n"
        "func run(c: Color) {\n"
        "    match c {\n"
        "        Color.Red { print(\"red\"); }\n"
        "        Color.Green, Color.Blue { print(\"other\"); }\n"
        "        else { print(\"unknown\"); }\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *match_stmt;
    const FengMatchBranch *branch0;
    const FengMatchBranch *branch1;

    ASSERT(feng_parse_source(source, strlen(source), "match_enum.f", &program, &error));
    ASSERT(program != NULL);
    match_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(match_stmt->kind == FENG_STMT_MATCH);
    ASSERT(match_stmt->as.match_stmt.branch_count == 2U);
    ASSERT(match_stmt->as.match_stmt.else_block != NULL);

    /* Branch 0: single enum item reference Color.Red */
    branch0 = &match_stmt->as.match_stmt.branches[0];
    ASSERT(branch0->label_count == 1U);
    ASSERT(branch0->labels[0].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(branch0->labels[0].type != NULL);
    ASSERT(branch0->labels[0].type->kind == FENG_TYPE_REF_NAMED);
    ASSERT(branch0->labels[0].type->as.named.segment_count == 2U);
    assert_slice_text(branch0->labels[0].type->as.named.segments[0], "Color");
    assert_slice_text(branch0->labels[0].type->as.named.segments[1], "Red");

    /* Branch 1: value list Color.Green, Color.Blue */
    branch1 = &match_stmt->as.match_stmt.branches[1];
    ASSERT(branch1->label_count == 2U);
    ASSERT(branch1->labels[0].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(branch1->labels[0].type->as.named.segment_count == 2U);
    assert_slice_text(branch1->labels[0].type->as.named.segments[1], "Green");
    ASSERT(branch1->labels[1].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(branch1->labels[1].type->as.named.segment_count == 2U);
    assert_slice_text(branch1->labels[1].type->as.named.segments[1], "Blue");

    feng_program_free(program);
}

static void test_union_spec_declaration_parses(void) {
    const char *source =
        "module demo.union;\n"
        "spec Value: i32 | string | Pair<i32, i32>[];\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *decl;

    ASSERT(feng_parse_source(source, strlen(source), "union_spec.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl->kind == FENG_DECL_SPEC);
    ASSERT(decl->as.spec_decl.form == FENG_SPEC_FORM_UNION);
    ASSERT(decl->as.spec_decl.as.union_form.member_count == 3U);
    ASSERT(decl->as.spec_decl.as.union_form.members[0]->kind == FENG_TYPE_REF_NAMED);
    assert_slice_text(decl->as.spec_decl.as.union_form.members[0]->as.named.segments[0], "i32");
    ASSERT(decl->as.spec_decl.as.union_form.members[1]->kind == FENG_TYPE_REF_NAMED);
    assert_slice_text(decl->as.spec_decl.as.union_form.members[1]->as.named.segments[0], "string");
    ASSERT(decl->as.spec_decl.as.union_form.members[2]->kind == FENG_TYPE_REF_ARRAY);
    ASSERT(decl->as.spec_decl.as.union_form.members[2]->as.inner->kind == FENG_TYPE_REF_NAMED);
    ASSERT(decl->as.spec_decl.as.union_form.members[2]->as.inner->as.named.type_arg_count == 2U);

    feng_program_free(program);
}

static void test_union_spec_rejects_void_member(void) {
    const char *source =
        "module demo.union;\n"
        "spec Bad: i32 | void;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "union_void_member.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "union-form spec members cannot be 'void'") != NULL);
}

static void test_match_type_labels_parse(void) {
    const char *source =
        "module demo.union;\n"
        "func run(value: Value) {\n"
        "    match value {\n"
        "        i32 { print(1); }\n"
        "        UserType, pkg.Named { print(2); }\n"
        "        Box<i32>[] { print(3); }\n"
        "        else { print(4); }\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *stmt;
    const FengMatchBranch *branch;

    ASSERT(feng_parse_source(source, strlen(source), "match_type_labels.f", &program, &error));
    ASSERT(program != NULL);
    stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_MATCH);
    ASSERT(stmt->as.match_stmt.branch_count == 3U);
    ASSERT(stmt->as.match_stmt.branches[0].labels[0].kind == FENG_MATCH_LABEL_TYPE);
    assert_slice_text(stmt->as.match_stmt.branches[0].labels[0].type->as.named.segments[0], "i32");
    branch = &stmt->as.match_stmt.branches[1];
    ASSERT(branch->label_count == 2U);
    ASSERT(branch->labels[0].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(branch->labels[0].value != NULL);
    ASSERT(branch->labels[1].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(branch->labels[1].type->as.named.segment_count == 2U);
    branch = &stmt->as.match_stmt.branches[2];
    ASSERT(branch->labels[0].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(branch->labels[0].type->kind == FENG_TYPE_REF_ARRAY);

    feng_program_free(program);
}

static void test_match_binding_prefix_parse(void) {
    const char *source =
        "module demo.union;\n"
        "func run(value: Value) {\n"
        "    match value {\n"
        "        x: i32 { print(x); }\n"
        "        let s: string { print(s); }\n"
        "        var b: bool { b = true; }\n"
        "        UserType { print(1); }\n"
        "        else { print(0); }\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *stmt;
    const FengMatchBranch *branch;

    ASSERT(feng_parse_source(source, strlen(source), "match_binding.f", &program, &error));
    ASSERT(program != NULL);
    stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_MATCH);
    ASSERT(stmt->as.match_stmt.branch_count == 4U);

    /* Branch 0: x: int { ... } — bare binding prefix */
    branch = &stmt->as.match_stmt.branches[0];
    ASSERT(branch->has_binding);
    assert_slice_text(branch->binding_name, "x");
    ASSERT(branch->binding_mutability == FENG_MUTABILITY_LET);
    ASSERT(branch->label_count == 1U);
    ASSERT(branch->labels[0].kind == FENG_MATCH_LABEL_TYPE);

    /* Branch 1: let s: string { ... } — explicit let */
    branch = &stmt->as.match_stmt.branches[1];
    ASSERT(branch->has_binding);
    assert_slice_text(branch->binding_name, "s");
    ASSERT(branch->binding_mutability == FENG_MUTABILITY_LET);
    ASSERT(branch->label_count == 1U);

    /* Branch 2: var b: bool { ... } — explicit var */
    branch = &stmt->as.match_stmt.branches[2];
    ASSERT(branch->has_binding);
    assert_slice_text(branch->binding_name, "b");
    ASSERT(branch->binding_mutability == FENG_MUTABILITY_VAR);
    ASSERT(branch->label_count == 1U);

    /* Branch 3: UserType { ... } — no binding */
    branch = &stmt->as.match_stmt.branches[3];
    ASSERT(!branch->has_binding);
    ASSERT(branch->label_count == 1U);
    ASSERT(branch->labels[0].kind == FENG_MATCH_LABEL_TYPE);

    /* else block: no binding */
    ASSERT(stmt->as.match_stmt.else_block != NULL);

    feng_program_free(program);
}

static void test_match_binding_prefix_expression_form(void) {
    const char *source =
        "module demo.union;\n"
        "func run(value: Value): i32 {\n"
        "    return match value {\n"
        "        v: i32 { v + 1; }\n"
        "        string { 0; }\n"
        "        else { -1; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *return_stmt;
    const FengExpr *match_expr;
    const FengMatchBranch *branch;

    ASSERT(feng_parse_source(source, strlen(source), "match_binding_expr.f", &program, &error));
    ASSERT(program != NULL);
    return_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    match_expr = return_stmt->as.return_value;
    ASSERT(match_expr->kind == FENG_EXPR_MATCH);
    ASSERT(match_expr->as.match_expr.branch_count == 2U);

    branch = &match_expr->as.match_expr.branches[0];
    ASSERT(branch->has_binding);
    assert_slice_text(branch->binding_name, "v");

    branch = &match_expr->as.match_expr.branches[1];
    ASSERT(!branch->has_binding);

    feng_program_free(program);
}

static void test_for_in_loop(void) {
    const char *source =
        "module demo.main;\n"
        "func run(items: i32[]) {\n"
        "    for let it in items {\n"
        "        print(it);\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *for_stmt;

    ASSERT(feng_parse_source(source, strlen(source), "for_in.f", &program, &error));
    ASSERT(program != NULL);
    for_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(for_stmt->kind == FENG_STMT_FOR);
    ASSERT(for_stmt->as.for_stmt.is_for_in);
    ASSERT(for_stmt->as.for_stmt.iter_expr != NULL);
    ASSERT(for_stmt->as.for_stmt.body != NULL);

    feng_program_free(program);
}

static void test_block_yield_omits_trailing_semicolon(void) {
    /* Per docs/feng-flow.md: trailing ';' on the last expression statement
     * of a block may be omitted. */
    const char *source =
        "module demo.main;\n"
        "func run(value: i32): i32 {\n"
        "    let x = if value > 0 { 1 } else { 2 };\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "block_yield.f", &program, &error));
    ASSERT(program != NULL);

    feng_program_free(program);
}

static void test_non_generic_array_new_uses_colon_dimension_syntax(void) {
    const char *source =
        "module demo.main;\n"
        "type Counter {\n"
        "    var value: i32;\n"
        "}\n"
        "func run(n: i32) {\n"
        "    let a: Counter[!] = Counter[:3];\n"
        "    let b: i32[!] = i32[:n];\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;
    const FengStmt *stmt_a;
    const FengStmt *stmt_b;

    ASSERT(feng_parse_source(source, strlen(source), "array_new_colon_dim.f", &program, &error));
    ASSERT(program != NULL);
    body = program->declarations[1]->as.function_decl.body;
    ASSERT(body->statement_count == 2U);

    stmt_a = body->statements[0];
    ASSERT(stmt_a->kind == FENG_STMT_BINDING);
    ASSERT(stmt_a->as.binding.initializer->kind == FENG_EXPR_ARRAY_NEW);

    stmt_b = body->statements[1];
    ASSERT(stmt_b->kind == FENG_STMT_BINDING);
    ASSERT(stmt_b->as.binding.initializer->kind == FENG_EXPR_ARRAY_NEW);

    feng_program_free(program);
}

static void test_generic_array_new_uses_colon_dimension_syntax(void) {
    const char *source =
        "module demo.main;\n"
        "type Pair<A, B> {\n"
        "    var left: A;\n"
        "    var right: B;\n"
        "}\n"
        "func run() {\n"
        "    let pairs: Pair<i32, i32>[!] = Pair<i32, i32>[:2];\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *stmt;
    const FengExpr *init;

    ASSERT(feng_parse_source(source, strlen(source), "generic_array_new_colon_dim.f", &program, &error));
    ASSERT(program != NULL);
    stmt = program->declarations[1]->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_BINDING);
    init = stmt->as.binding.initializer;
    ASSERT(init != NULL);
    ASSERT(init->kind == FENG_EXPR_ARRAY_NEW);
    ASSERT(init->as.array_new.element_type != NULL);
    ASSERT(init->as.array_new.element_type->kind == FENG_TYPE_REF_NAMED);
    ASSERT(init->as.array_new.element_type->as.named.type_arg_count == 2U);

    feng_program_free(program);
}

static void test_index_expression_is_unambiguous_and_remains_value_brackets(void) {
    const char *source =
        "module demo.main;\n"
        "func run(items: i32[]): i32 {\n"
        "    return items[0];\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengExpr *ret;

    ASSERT(feng_parse_source(source, strlen(source), "index_unambiguous.f", &program, &error));
    ASSERT(program != NULL);
    ret = program->declarations[0]->as.function_decl.body->statements[0]->as.return_value;
    ASSERT(ret->kind == FENG_EXPR_INDEX);

    feng_program_free(program);
}

static void test_non_generic_type_brackets_without_colon_parses_as_index(void) {
    const char *source =
        "module demo.main;\n"
        "type Counter {\n"
        "    var value: i32;\n"
        "}\n"
        "func run() {\n"
        "    let x: Counter[!] = Counter[3];\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *stmt;
    const FengExpr *init;

    ASSERT(feng_parse_source(source, strlen(source), "array_new_without_colon_is_index.f", &program, &error));
    ASSERT(program != NULL);
    stmt = program->declarations[1]->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_BINDING);
    init = stmt->as.binding.initializer;
    ASSERT(init != NULL);
    ASSERT(init->kind == FENG_EXPR_INDEX);

    feng_program_free(program);
}

static void test_generic_type_brackets_without_colon_parses_as_index(void) {
    const char *source =
        "module demo.main;\n"
        "type Pair<A, B> {\n"
        "    var left: A;\n"
        "    var right: B;\n"
        "}\n"
        "func run() {\n"
        "    let pairs: Pair<i32, i32>[!] = Pair<i32, i32>[2];\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *stmt;
    const FengExpr *init;

    ASSERT(feng_parse_source(source,
                             strlen(source),
                             "generic_array_new_without_colon_is_index.f",
                             &program,
                             &error));
    ASSERT(program != NULL);
    stmt = program->declarations[1]->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_BINDING);
    init = stmt->as.binding.initializer;
    ASSERT(init != NULL);
    ASSERT(init->kind == FENG_EXPR_INDEX);
    ASSERT(init->as.index.object != NULL);
    ASSERT(init->as.index.object->kind == FENG_EXPR_GENERIC_TARGET);

    feng_program_free(program);
}

/* G3-9: Parser tests for generic declarations and type references. */

static void test_generic_type_declaration(void) {
    /* type Box<T> with one unconstrained type parameter */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "generic_type.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    ASSERT(program->declarations[0]->kind == FENG_DECL_TYPE);
    ASSERT(program->declarations[0]->as.type_decl.type_param_count == 1U);
    ASSERT(program->declarations[0]->as.type_decl.type_params[0].constraint == NULL);
    assert_slice_text(program->declarations[0]->as.type_decl.type_params[0].name, "T");

    feng_program_free(program);
}

static void test_generic_type_declaration_with_constraint(void) {
    /* type Wrapper<T: Named> with a constrained type parameter */
    const char *source =
        "module demo.main;\n"
        "type Wrapper<T: Named> {\n"
        "    var inner: T;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "generic_type_constrained.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    ASSERT(program->declarations[0]->as.type_decl.type_param_count == 1U);
    ASSERT(program->declarations[0]->as.type_decl.type_params[0].constraint != NULL);
    assert_slice_text(program->declarations[0]->as.type_decl.type_params[0].name, "T");
    assert_slice_text(program->declarations[0]->as.type_decl.type_params[0].constraint->as.named.segments[0], "Named");

    feng_program_free(program);
}

static void test_generic_spec_declaration(void) {
    /* spec Container<T> with one type parameter */
    const char *source =
        "module demo.main;\n"
        "spec Container<T> {\n"
        "    func fetch(): T;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "generic_spec.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    ASSERT(program->declarations[0]->kind == FENG_DECL_SPEC);
    ASSERT(program->declarations[0]->as.spec_decl.type_param_count == 1U);
    assert_slice_text(program->declarations[0]->as.spec_decl.type_params[0].name, "T");

    feng_program_free(program);
}

static void test_generic_function_declaration(void) {
    /* func identity<T>(value: T): T with one type parameter */
    const char *source =
        "module demo.main;\n"
        "func identity<T>(value: T): T {\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "generic_fn.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    ASSERT(program->declarations[0]->kind == FENG_DECL_FUNCTION);
    ASSERT(program->declarations[0]->as.function_decl.type_param_count == 1U);
    assert_slice_text(program->declarations[0]->as.function_decl.type_params[0].name, "T");
    ASSERT(program->declarations[0]->as.function_decl.type_params[0].constraint == NULL);

    feng_program_free(program);
}

static void test_generic_function_multi_type_params(void) {
    /* func zip<A, B: Named>(a: A, b: B): A with two type parameters */
    const char *source =
        "module demo.main;\n"
        "func zip<A, B: Named>(a: A, b: B): A {\n"
        "    return a;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "generic_fn2.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declarations[0]->as.function_decl.type_param_count == 2U);
    assert_slice_text(program->declarations[0]->as.function_decl.type_params[0].name, "A");
    ASSERT(program->declarations[0]->as.function_decl.type_params[0].constraint == NULL);
    assert_slice_text(program->declarations[0]->as.function_decl.type_params[1].name, "B");
    ASSERT(program->declarations[0]->as.function_decl.type_params[1].constraint != NULL);

    feng_program_free(program);
}

static void test_generic_type_ref_with_args(void) {
    /* Let binding with generic type ref: let x: Map<string, int>; */
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let x: Map<string, i32>;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengTypeRef *type_ref;

    ASSERT(feng_parse_source(source, strlen(source), "generic_ref.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    ASSERT(fn_decl->kind == FENG_DECL_FUNCTION);
    ASSERT(fn_decl->as.function_decl.body->statement_count == 1U);
    stmt = fn_decl->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_BINDING);
    type_ref = stmt->as.binding.type;
    ASSERT(type_ref != NULL);
    ASSERT(type_ref->kind == FENG_TYPE_REF_NAMED);
    ASSERT(type_ref->as.named.type_arg_count == 2U);

    feng_program_free(program);
}

static void test_generic_type_ref_nested(void) {
    /* Nested generic type ref: Map<string, List<int>> handled via pending_gt */
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    let x: Map<string, List<i32>>;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengTypeRef *outer;
    const FengTypeRef *inner;

    ASSERT(feng_parse_source(source, strlen(source), "generic_ref_nested.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    stmt = fn_decl->as.function_decl.body->statements[0];
    outer = stmt->as.binding.type;
    ASSERT(outer->as.named.type_arg_count == 2U);
    /* Second type arg is List<int> */
    inner = outer->as.named.type_args[1];
    ASSERT(inner->kind == FENG_TYPE_REF_NAMED);
    ASSERT(inner->as.named.type_arg_count == 1U);
    assert_slice_text(inner->as.named.segments[0], "List");

    feng_program_free(program);
}

static void test_cast_with_generic_array_target(void) {
    const char *source =
        "module demo.main;\n"
        "type Entry<K, V> {\n"
        "    let key: K;\n"
        "    let value: V;\n"
        "}\n"
        "func run<K, V>(items: Entry<K, V>[!]): Entry<K, V>[] {\n"
        "    return (Entry<K, V>[])items;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;

    ASSERT(feng_parse_source(source, strlen(source), "cast_generic_array_target.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[1];
    ASSERT(fn_decl->kind == FENG_DECL_FUNCTION);
    ASSERT(fn_decl->as.function_decl.body->statement_count == 1U);
    stmt = fn_decl->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_RETURN);
    ASSERT(stmt->as.return_value->kind == FENG_EXPR_CAST);
    ASSERT(stmt->as.return_value->as.cast.type != NULL);
    ASSERT(stmt->as.return_value->as.cast.type->kind == FENG_TYPE_REF_ARRAY);
    ASSERT(stmt->as.return_value->as.cast.type->as.inner != NULL);
    ASSERT(stmt->as.return_value->as.cast.type->as.inner->kind == FENG_TYPE_REF_NAMED);
    ASSERT(stmt->as.return_value->as.cast.type->as.inner->as.named.type_arg_count == 2U);

    feng_program_free(program);
}

static void test_explicit_generic_call(void) {
    /* Explicit generic call: callee<int>(arg) */
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    callee<i32>(42);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengExpr *call;

    ASSERT(feng_parse_source(source, strlen(source), "generic_call.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    stmt = fn_decl->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_EXPR);
    call = stmt->as.expr;
    ASSERT(call->kind == FENG_EXPR_CALL);
    ASSERT(call->as.call.has_explicit_type_args);
    ASSERT(call->as.call.explicit_type_arg_count == 1U);
    ASSERT(call->as.call.arg_count == 1U);

    feng_program_free(program);
}

static void test_explicit_generic_call_multi_type_args(void) {
    /* Explicit generic call with two type args: callee<A, B>(x, y) */
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    callee<A, B>(x, y);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengExpr *call;

    ASSERT(feng_parse_source(source, strlen(source), "generic_call2.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    stmt = fn_decl->as.function_decl.body->statements[0];
    call = stmt->as.expr;
    ASSERT(call->kind == FENG_EXPR_CALL);
    ASSERT(call->as.call.has_explicit_type_args);
    ASSERT(call->as.call.explicit_type_arg_count == 2U);
    ASSERT(call->as.call.arg_count == 2U);

    feng_program_free(program);
}

/* G7 parser additions */

static void test_explicit_generic_type_constructor_call(void) {
    /* 正确语法六-b: Type<T1, T2>() generic type constructor call. */
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    Map<string, i32>();\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengExpr *call;

    ASSERT(feng_parse_source(source, strlen(source), "generic_ctor.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    stmt = fn_decl->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_EXPR);
    call = stmt->as.expr;
    ASSERT(call->kind == FENG_EXPR_CALL);
    ASSERT(call->as.call.has_explicit_type_args);
    ASSERT(call->as.call.explicit_type_arg_count == 2U);
    ASSERT(call->as.call.arg_count == 0U);

    feng_program_free(program);
}

static void test_postfix_pointer_type_refs(void) {
    const char *source =
        "module demo.main;\n"
        "func run(a: Point*[], b: i32[]*, c: byte*) {}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengCallableSignature *callable;
    const FengTypeRef *type_a;
    const FengTypeRef *type_b;
    const FengTypeRef *type_c;

    ASSERT(feng_parse_source(source, strlen(source), "postfix_ptr_ref.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    ASSERT(fn_decl->kind == FENG_DECL_FUNCTION);
    callable = &fn_decl->as.function_decl;
    ASSERT(callable->param_count == 3U);

    type_a = callable->params[0].type;
    ASSERT(type_a != NULL);
    ASSERT(type_a->kind == FENG_TYPE_REF_ARRAY);
    ASSERT(type_a->as.inner != NULL);
    ASSERT(type_a->as.inner->kind == FENG_TYPE_REF_POINTER);
    ASSERT(type_a->as.inner->as.inner != NULL);
    ASSERT(type_a->as.inner->as.inner->kind == FENG_TYPE_REF_NAMED);
    assert_slice_text(type_a->as.inner->as.inner->as.named.segments[0], "Point");

    type_b = callable->params[1].type;
    ASSERT(type_b != NULL);
    ASSERT(type_b->kind == FENG_TYPE_REF_POINTER);
    ASSERT(type_b->as.inner != NULL);
    ASSERT(type_b->as.inner->kind == FENG_TYPE_REF_ARRAY);
    ASSERT(type_b->as.inner->as.inner != NULL);
    ASSERT(type_b->as.inner->as.inner->kind == FENG_TYPE_REF_NAMED);
    assert_slice_text(type_b->as.inner->as.inner->as.named.segments[0], "i32");

    type_c = callable->params[2].type;
    ASSERT(type_c != NULL);
    ASSERT(type_c->kind == FENG_TYPE_REF_POINTER);
    ASSERT(type_c->as.inner != NULL);
    ASSERT(type_c->as.inner->kind == FENG_TYPE_REF_NAMED);
    assert_slice_text(type_c->as.inner->as.named.segments[0], "byte");

    feng_program_free(program);
}

static void test_generic_method_uses_both_outer_and_method_type_params(void) {
    /* 正确语法二: method in generic type uses outer type param T and own param U. */
    const char *source =
        "module demo.main;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "    func map<U>(x: U): T {\n"
        "        return self.value;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *type_decl;
    const FengTypeMember *method;

    ASSERT(feng_parse_source(source, strlen(source), "generic_method.f", &program, &error));
    ASSERT(program != NULL);
    type_decl = program->declarations[0];
    ASSERT(type_decl->kind == FENG_DECL_TYPE);
    ASSERT(type_decl->as.type_decl.type_param_count == 1U);
    /* Second member (index 1) is the method `map`. */
    method = type_decl->as.type_decl.members[1];
    ASSERT(method->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(method->as.callable.type_param_count == 1U);
    assert_slice_text(method->as.callable.type_params[0].name, "U");

    feng_program_free(program);
}

static void test_generic_parse_error_colon_angle_in_type_position(void) {
    /* 旧语法 `:<T>` in non-call position (type declaration head) must fail. */
    const char *source =
        "module demo.main;\n"
        "type Box:<T> {\n"
        "    open let value: i32;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "generic_err_colon.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_generic_parse_error_nested_colon_angle_in_type_arg(void) {
    /* 旧语法 `foo<Map:<int>>(x)` — nested `:<` inside type arg list must fail. */
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    foo<Map:<i32>>(x);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "generic_err_nested.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_generic_parse_error_missing_closing_gt(void) {
    /* type Box<T { ... } — missing `>` */
    const char *source =
        "module demo.main;\n"
        "type Box<T {\n"
        "    var value: T;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "generic_err.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_generic_parse_error_missing_type_param_name(void) {
    /* func foo<, T>() — missing type parameter name */
    const char *source =
        "module demo.main;\n"
        "func foo<, T>(): void {\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "generic_err2.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_generic_target_expression_argument_parses(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    callee(a<b>);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengExpr *call;

    ASSERT(feng_parse_source(source, strlen(source), "generic_target_arg.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    stmt = fn_decl->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_EXPR);
    call = stmt->as.expr;
    ASSERT(call->kind == FENG_EXPR_CALL);
    ASSERT(call->as.call.arg_count == 1U);
    ASSERT(call->as.call.args[0]->kind == FENG_EXPR_GENERIC_TARGET);
    ASSERT(call->as.call.args[0]->as.generic_target.type_arg_count == 1U);

    feng_program_free(program);
}

static void test_generic_type_target_member_parses(void) {
    const char *source =
        "module demo.main;\n"
        "func run(): void {\n"
        "    Box<i32>.make(1);\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengStmt *stmt;
    const FengExpr *call;
    const FengExpr *member;

    ASSERT(feng_parse_source(source, strlen(source), "generic_type_target_member.f", &program, &error));
    ASSERT(program != NULL);
    fn_decl = program->declarations[0];
    stmt = fn_decl->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_EXPR);
    call = stmt->as.expr;
    ASSERT(call->kind == FENG_EXPR_CALL);
    member = call->as.call.callee;
    ASSERT(member->kind == FENG_EXPR_MEMBER);
    ASSERT(member->as.member.object->kind == FENG_EXPR_GENERIC_TARGET);
    ASSERT(member->as.member.object->as.generic_target.type_arg_count == 1U);

    feng_program_free(program);
}

/* T1: func sum(values: int...): int — variadic parameter parses and is
 * normalised to is_variadic=true with type int[]. */
static void test_variadic_parameter_parses(void) {
    const char *source =
        "module demo.main;\n"
        "func sum(values: i32...): i32 {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *fn_decl;
    const FengParameter *param;

    ASSERT(feng_parse_source(source, strlen(source), "variadic_param.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);

    fn_decl = program->declarations[0];
    ASSERT(fn_decl->kind == FENG_DECL_FUNCTION);
    ASSERT(fn_decl->as.function_decl.param_count == 1U);

    param = &fn_decl->as.function_decl.params[0];
    ASSERT(param->is_variadic);
    /* Type is normalised to int[] (FENG_TYPE_REF_ARRAY wrapping int). */
    ASSERT(param->type != NULL);
    ASSERT(param->type->kind == FENG_TYPE_REF_ARRAY);
    ASSERT(param->type->as.inner != NULL);
    ASSERT(param->type->as.inner->kind == FENG_TYPE_REF_NAMED);
    ASSERT(param->type->as.inner->as.named.segment_count == 1U);
    assert_slice_text(param->type->as.inner->as.named.segments[0], "i32");

    feng_program_free(program);
}

/* T4: variadic parameter must be the last parameter. */
static void test_parse_error_variadic_not_last(void) {
    const char *source =
        "module demo.main;\n"
        "func bad(x: i32..., y: i32): i32 {\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "variadic_not_last.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "variadic parameter must be the last parameter") != NULL);
}

/* T5: extern func cannot use variadic parameters. */
static void test_parse_error_extern_fn_variadic(void) {
    const char *source =
        "module demo.main;\n"
        "extern func bad(x: i32...): i32;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "extern_variadic.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "extern function declarations cannot use variadic parameters") != NULL);
}

static void test_tuple_type_declaration_parses(void) {
    const char *source =
        "module demo.tuple;\n"
        "type Unit();\n"
        "type Point(float, float);\n"
        "type Pair<T, U>(T, U);\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *point;
    const FengDecl *pair;
    const FengDecl *unit;

    ASSERT(feng_parse_source(source, strlen(source), "tuple_type.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 3U);

    unit = program->declarations[0];
    ASSERT(unit->kind == FENG_DECL_TYPE);
    ASSERT(unit->as.type_decl.is_tuple);
    ASSERT(unit->as.type_decl.member_count == 0U);

    point = program->declarations[1];
    ASSERT(point->kind == FENG_DECL_TYPE);
    ASSERT(point->as.type_decl.is_tuple);
    ASSERT(point->as.type_decl.member_count == 2U);
    ASSERT(point->as.type_decl.members[0]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(point->as.type_decl.members[0]->as.field.mutability == FENG_MUTABILITY_LET);
    assert_slice_text(point->as.type_decl.members[0]->as.field.name, "item1");
    assert_slice_text(point->as.type_decl.members[1]->as.field.name, "item2");
    assert_slice_text(point->as.type_decl.members[0]->as.field.type->as.named.segments[0], "float");

    pair = program->declarations[2];
    ASSERT(pair->kind == FENG_DECL_TYPE);
    ASSERT(pair->as.type_decl.is_tuple);
    ASSERT(pair->as.type_decl.type_param_count == 2U);
    ASSERT(pair->as.type_decl.member_count == 2U);
    assert_slice_text(pair->as.type_decl.members[0]->as.field.type->as.named.segments[0], "T");
    assert_slice_text(pair->as.type_decl.members[1]->as.field.type->as.named.segments[0], "U");

    feng_program_free(program);
}

static void test_tuple_type_arity_errors(void) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {
            "module demo.tuple;\n"
            "type Single(i32);\n",
            "tuple type declarations require 0 or 2 to 8 elements"
        },
        {
            "module demo.tuple;\n"
            "type TooMany(i32, i32, i32, i32, i32, i32, i32, i32, i32);\n",
            "tuple type declarations support at most 8 elements"
        }
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(cases[i].source,
                                  strlen(cases[i].source),
                                  "tuple_type_arity_error.f",
                                  &program,
                                  &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message, cases[i].message) != NULL);
    }
}

static void test_tuple_literal_and_grouped_expression_parse(void) {
    const char *source =
        "module demo.tuple;\n"
        "type Point(i32, i32);\n"
        "func run() {\n"
        "    let grouped = (1);\n"
        "    let tupled: Point = (1, 2);\n"
        "    let casted = (Point)(1, 2);\n"
        "    let empty: Unit = ();\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;
    const FengExpr *grouped;
    const FengExpr *tupled;
    const FengExpr *casted;
    const FengExpr *empty;

    ASSERT(feng_parse_source(source, strlen(source), "tuple_literal_parse.f", &program, &error));
    ASSERT(program != NULL);
    body = program->declarations[1]->as.function_decl.body;
    ASSERT(body->statement_count == 4U);

    grouped = body->statements[0]->as.binding.initializer;
    ASSERT(grouped->kind == FENG_EXPR_INTEGER);

    tupled = body->statements[1]->as.binding.initializer;
    ASSERT(tupled->kind == FENG_EXPR_TUPLE_LITERAL);
    ASSERT(tupled->as.tuple_literal.count == 2U);

    casted = body->statements[2]->as.binding.initializer;
    ASSERT(casted->kind == FENG_EXPR_CAST);
    ASSERT(casted->as.cast.value->kind == FENG_EXPR_TUPLE_LITERAL);
    ASSERT(casted->as.cast.value->as.tuple_literal.count == 2U);

    empty = body->statements[3]->as.binding.initializer;
    ASSERT(empty->kind == FENG_EXPR_TUPLE_LITERAL);
    ASSERT(empty->as.tuple_literal.count == 0U);

    feng_program_free(program);
}

static void test_tuple_literal_arity_errors(void) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {
            "module demo.tuple;\n"
            "func run() { let one = (1,); }\n",
            "tuple literals require an expression after ','"
        },
        {
            "module demo.tuple;\n"
            "func run() { let many = (1, 2, 3, 4, 5, 6, 7, 8, 9); }\n",
            "tuple literals support at most 8 elements"
        }
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(cases[i].source,
                                  strlen(cases[i].source),
                                  "tuple_literal_arity_error.f",
                                  &program,
                                  &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message, cases[i].message) != NULL);
    }
}

static void test_destructuring_binding_parse(void) {
    const char *source =
        "module demo.tuple;\n"
        "func run(point: Point, tuple: Triple) {\n"
        "    let () = ();\n"
        "    let (x, , z) = point;\n"
        "    var (, middle, ) = tuple;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengBlock *body;
    const FengBinding *first;
    const FengBinding *second;
    const FengBinding *empty;

    ASSERT(feng_parse_source(source, strlen(source), "destructure_parse.f", &program, &error));
    ASSERT(program != NULL);
    body = program->declarations[0]->as.function_decl.body;
    ASSERT(body->statement_count == 3U);

    empty = &body->statements[0]->as.binding;
    ASSERT(empty->is_destructure);
    ASSERT(empty->destructure_count == 0U);
    ASSERT(empty->initializer->kind == FENG_EXPR_TUPLE_LITERAL);
    ASSERT(empty->initializer->as.tuple_literal.count == 0U);

    first = &body->statements[1]->as.binding;
    ASSERT(first->is_destructure);
    ASSERT(first->destructure_count == 3U);
    assert_slice_text(first->destructure_names[0], "x");
    assert_slice_text(first->destructure_names[1], NULL);
    assert_slice_text(first->destructure_names[2], "z");
    ASSERT(first->initializer->kind == FENG_EXPR_IDENTIFIER);

    second = &body->statements[2]->as.binding;
    ASSERT(second->is_destructure);
    ASSERT(second->mutability == FENG_MUTABILITY_VAR);
    ASSERT(second->destructure_count == 3U);
    assert_slice_text(second->destructure_names[0], NULL);
    assert_slice_text(second->destructure_names[1], "middle");
    assert_slice_text(second->destructure_names[2], NULL);

    feng_program_free(program);
}

static void test_destructuring_binding_errors(void) {
    static const struct {
        const char *source;
        const char *message;
    } cases[] = {
        {
            "module demo.tuple;\n"
            "func run(value: Tuple) { let (x) = value; }\n",
            "destructuring bindings require 0 or 2 to 8 positions"
        },
        {
            "module demo.tuple;\n"
            "func run(value: Tuple) { let (x, (y, z)) = value; }\n",
            "nested destructuring bindings are not supported"
        },
        {
            "module demo.tuple;\n"
            "func run(value: Tuple) { let (x, y): Tuple = value; }\n",
            "destructuring bindings cannot use a single type annotation"
        }
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(cases[i].source,
                                  strlen(cases[i].source),
                                  "destructure_error.f",
                                  &program,
                                  &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message, cases[i].message) != NULL);
    }
}

static void test_empty_source_file(void) {
    const char *source = "";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, 0U, "empty.ff", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_segments == NULL);
    ASSERT(program->module_segment_count == 0U);
    ASSERT(program->use_count == 0U);
    ASSERT(program->declaration_count == 0U);

    feng_program_free(program);
}

static void test_whitespace_only_source_file(void) {
    const char *source = "   \n\t\n  \r\n  ";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "whitespace.ff", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_segments == NULL);
    ASSERT(program->module_segment_count == 0U);
    ASSERT(program->use_count == 0U);
    ASSERT(program->declaration_count == 0U);

    feng_program_free(program);
}

static void test_line_comment_only_source_file(void) {
    const char *source = "// this is a comment\n// another comment\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "line_comment.ff", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_segments == NULL);
    ASSERT(program->module_segment_count == 0U);
    ASSERT(program->use_count == 0U);
    ASSERT(program->declaration_count == 0U);

    feng_program_free(program);
}

static void test_block_comment_only_source_file(void) {
    const char *source = "/* block comment\n   spanning multiple lines */";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "block_comment.ff", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_segments == NULL);
    ASSERT(program->module_segment_count == 0U);
    ASSERT(program->use_count == 0U);
    ASSERT(program->declaration_count == 0U);

    feng_program_free(program);
}

static void test_mixed_comments_only_source_file(void) {
    const char *source =
        "// line comment\n"
        "/* block comment */\n"
        "   \n"
        "// another line comment\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "mixed_comments.ff", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->module_segments == NULL);
    ASSERT(program->module_segment_count == 0U);
    ASSERT(program->use_count == 0U);
    ASSERT(program->declaration_count == 0U);

    feng_program_free(program);
}

static void test_non_empty_source_without_module_is_rejected(void) {
    static const char *kCases[] = {
        "fn main() {}\n",
        "let x = 42;\n",
        "type Foo { var x: i32; }\n",
        "import std.io;\n",
    };
    size_t i;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(kCases[i], strlen(kCases[i]), "no_module.ff", &program, &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message, "source file must begin with module declaration") != NULL);
    }
}

static void test_visibility_without_module_is_rejected(void) {
    const char *source = "open fn main() {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "visibility_no_module.ff", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "source file must begin with module declaration") != NULL);
}

static void test_infix_match_op_simple_value_parses(void) {
    const char *source =
        "module demo.main;\n"
        "func run(x: i32): bool {\n"
        "    return x match 0;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *return_stmt;
    const FengExpr *match_op;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_value.f", &program, &error));
    ASSERT(program != NULL);
    return_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 1U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_op->as.match_op.has_binding == false);

    feng_program_free(program);
}

static void test_infix_match_op_range_and_type_parses(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: i32 | string;\n"
        "func run_int(x: i32): bool {\n"
        "    return x match 1...10;\n"
        "}\n"
        "func run_union(v: Value): bool {\n"
        "    return v match UserType;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *func;
    const FengStmt *return_stmt;
    const FengExpr *match_op;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_range_type.f", &program, &error));
    ASSERT(program != NULL);

    /* decl[0] = spec Value; decl[1] = run_int; decl[2] = run_union */

    /* First function: range label */
    func = program->declarations[1];
    ASSERT(func->kind == FENG_DECL_FUNCTION);
    return_stmt = func->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 1U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_RANGE);

    /* Second function: type label */
    func = program->declarations[2];
    ASSERT(func->kind == FENG_DECL_FUNCTION);
    return_stmt = func->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 1U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_TYPE);

    feng_program_free(program);
}

static void test_infix_match_op_multi_label_pipe_parses(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: i32 | string;\n"
        "func run_int(x: i32): bool {\n"
        "    return x match 0 | 1 | 2;\n"
        "}\n"
        "func run_int_mix(x: i32): bool {\n"
        "    return x match 0 | 1...10 | 100;\n"
        "}\n"
        "func run_union(v: Value): bool {\n"
        "    return v match Type1 | Type2;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *func;
    const FengStmt *return_stmt;
    const FengExpr *match_op;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_pipe.f", &program, &error));
    ASSERT(program != NULL);

    /* decl[0]=spec; decl[1]=run_int; decl[2]=run_int_mix; decl[3]=run_union */

    /* Function 1: x match 0 | 1 | 2 (3 value labels) */
    func = program->declarations[1];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 3U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_op->as.match_op.labels[1].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_op->as.match_op.labels[2].kind == FENG_MATCH_LABEL_VALUE);

    /* Function 2: x match 0 | 1...10 | 100 (3 mixed labels) */
    func = program->declarations[2];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 3U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_VALUE);
    ASSERT(match_op->as.match_op.labels[1].kind == FENG_MATCH_LABEL_RANGE);
    ASSERT(match_op->as.match_op.labels[2].kind == FENG_MATCH_LABEL_VALUE);

    /* Function 3: v match Type1 | Type2 (2 type labels) */
    func = program->declarations[3];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 2U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(match_op->as.match_op.labels[1].kind == FENG_MATCH_LABEL_TYPE);

    feng_program_free(program);
}

static void test_infix_match_op_binding_parses(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: i32 | string;\n"
        "func run_implicit(v: Value): bool {\n"
        "    return v match x: UserType;\n"
        "}\n"
        "func run_let(v: Value): bool {\n"
        "    return v match let x: UserType;\n"
        "}\n"
        "func run_var(v: Value): bool {\n"
        "    return v match var x: UserType;\n"
        "}\n"
        "func run_multi(v: Value): bool {\n"
        "    return v match x: Type1 | Type2;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *func;
    const FengStmt *return_stmt;
    const FengExpr *match_op;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_binding.f", &program, &error));
    ASSERT(program != NULL);

    /* decl[0]=spec; decl[1..4]=functions in order */

    /* Implicit let: v match x: UserType */
    func = program->declarations[1];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.has_binding == true);
    assert_slice_text(match_op->as.match_op.binding_name, "x");
    ASSERT(match_op->as.match_op.binding_mutability == FENG_MUTABILITY_LET);
    ASSERT(match_op->as.match_op.label_count == 1U);

    /* Explicit let: v match let x: UserType */
    func = program->declarations[2];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.has_binding == true);
    ASSERT(match_op->as.match_op.binding_mutability == FENG_MUTABILITY_LET);

    /* Explicit var: v match var x: UserType */
    func = program->declarations[3];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.has_binding == true);
    ASSERT(match_op->as.match_op.binding_mutability == FENG_MUTABILITY_VAR);

    /* Multi-label binding: v match x: Type1 | Type2 */
    func = program->declarations[4];
    return_stmt = func->as.function_decl.body->statements[0];
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.has_binding == true);
    ASSERT(match_op->as.match_op.label_count == 2U);
    ASSERT(match_op->as.match_op.labels[0].kind == FENG_MATCH_LABEL_TYPE);
    ASSERT(match_op->as.match_op.labels[1].kind == FENG_MATCH_LABEL_TYPE);

    feng_program_free(program);
}

static void test_infix_match_op_in_if_while_condition_parses(void) {
    const char *source =
        "module demo.main;\n"
        "spec Value: i32 | string;\n"
        "func run_if(v: Value) {\n"
        "    if v match UserType { print(1); }\n"
        "}\n"
        "func run_while(v: Value) {\n"
        "    while v match UserType { print(1); }\n"
        "}\n"
        "func run_combined(v: Value, w: Value) {\n"
        "    if v match UserType && w match OtherType { print(1); }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *func;
    const FengStmt *stmt;
    const FengExpr *cond;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_if_while.f", &program, &error));
    ASSERT(program != NULL);

    /* decl[0]=spec; decl[1..3]=functions */

    /* if condition is match_op */
    func = program->declarations[1];
    stmt = func->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_IF);
    cond = stmt->as.if_stmt.clauses[0].condition;
    ASSERT(cond->kind == FENG_EXPR_MATCH_OP);

    /* while condition is match_op */
    func = program->declarations[2];
    stmt = func->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_WHILE);
    cond = stmt->as.while_stmt.condition;
    ASSERT(cond->kind == FENG_EXPR_MATCH_OP);

    /* if condition is && of two match_op */
    func = program->declarations[3];
    stmt = func->as.function_decl.body->statements[0];
    ASSERT(stmt->kind == FENG_STMT_IF);
    cond = stmt->as.if_stmt.clauses[0].condition;
    ASSERT(cond->kind == FENG_EXPR_BINARY);
    ASSERT(cond->as.binary.op == FENG_TOKEN_AND_AND);
    ASSERT(cond->as.binary.left->kind == FENG_EXPR_MATCH_OP);
    ASSERT(cond->as.binary.right->kind == FENG_EXPR_MATCH_OP);

    feng_program_free(program);
}

static void test_infix_match_op_pipe_does_not_become_bit_or(void) {
    /* `x match 0 | 1` must parse as infix match with 2 labels, NOT as
     * `(x match 0) | 1` (bitwise-or). bool | int is illegal (AE0030) so the
     * bitwise-or interpretation would be meaningless. */
    const char *source =
        "module demo.main;\n"
        "func run(x: i32): bool {\n"
        "    return x match 0 | 1;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *return_stmt;
    const FengExpr *match_op;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_pipe_not_bitor.f", &program, &error));
    ASSERT(program != NULL);
    return_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    match_op = return_stmt->as.return_value;
    ASSERT(match_op->kind == FENG_EXPR_MATCH_OP);
    ASSERT(match_op->as.match_op.label_count == 2U);

    feng_program_free(program);
}

static void test_infix_match_op_left_associative_chains(void) {
    /* `x match a match b` is parsed as `(x match a) match b`. `b` as a value
     * pattern (true/false) is semantically legal; non-value `b` is rejected by
     * the type checker. */
    const char *source =
        "module demo.main;\n"
        "func run(x: i32): bool {\n"
        "    return x match 0 match true;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengStmt *return_stmt;
    const FengExpr *outer, *inner;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_chain.f", &program, &error));
    ASSERT(program != NULL);
    return_stmt = program->declarations[0]->as.function_decl.body->statements[0];
    ASSERT(return_stmt->kind == FENG_STMT_RETURN);
    outer = return_stmt->as.return_value;
    ASSERT(outer->kind == FENG_EXPR_MATCH_OP);
    ASSERT(outer->as.match_op.label_count == 1U);
    ASSERT(outer->as.match_op.labels[0].kind == FENG_MATCH_LABEL_VALUE);
    /* outer.target should itself be an inner match_op */
    inner = outer->as.match_op.target;
    ASSERT(inner->kind == FENG_EXPR_MATCH_OP);
    ASSERT(inner->as.match_op.label_count == 1U);

    feng_program_free(program);
}

static void test_infix_match_op_mixed_with_relational_and_equality(void) {
    /* match is same-precedence as relational, left-associative:
     *   `x match T < y` parses as `(x match T) < y` (binary < )
     *   `x match T == y` parses as `(x match T) == y` (binary == ) */
    const char *source =
        "module demo.main;\n"
        "spec Value: i32 | string;\n"
        "func run_lt(v: Value, y: i32): bool {\n"
        "    return v match UserType < y;\n"
        "}\n"
        "func run_eq(v: Value, y: bool): bool {\n"
        "    return v match UserType == y;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;
    const FengDecl *func;
    const FengStmt *return_stmt;
    const FengExpr *bin, *left;

    ASSERT(feng_parse_source(source, strlen(source), "infix_match_rel_eq.f", &program, &error));
    ASSERT(program != NULL);

    /* decl[0]=spec; decl[1]=run_lt; decl[2]=run_eq */

    /* v match UserType < y -> binary < with left=match_op */
    func = program->declarations[1];
    return_stmt = func->as.function_decl.body->statements[0];
    bin = return_stmt->as.return_value;
    ASSERT(bin->kind == FENG_EXPR_BINARY);
    ASSERT(bin->as.binary.op == FENG_TOKEN_LT);
    left = bin->as.binary.left;
    ASSERT(left->kind == FENG_EXPR_MATCH_OP);

    /* v match UserType == y -> binary == with left=match_op */
    func = program->declarations[2];
    return_stmt = func->as.function_decl.body->statements[0];
    bin = return_stmt->as.return_value;
    ASSERT(bin->kind == FENG_EXPR_BINARY);
    ASSERT(bin->as.binary.op == FENG_TOKEN_EQ);
    left = bin->as.binary.left;
    ASSERT(left->kind == FENG_EXPR_MATCH_OP);

    feng_program_free(program);
}

int main(void) {
    test_top_level_declarations();
    test_annotation_accepts_two_arguments();
    test_extern_rejects_non_function_top_level_declarations();
    test_statements_and_expressions();
    test_try_block_form_is_rejected();
    test_try_expression_with_typed_catches();
    test_try_without_catch_is_rejected();
    test_defer_block_parses();
    test_defer_without_block_is_rejected();
    test_runtime_annotation_on_extern_function();
    test_enum_declarations_parse();
    test_match_with_range_and_list_labels();
    test_match_statement_form();
    test_match_enum_item_reference_labels_parse();
    test_union_spec_declaration_parses();
    test_union_spec_rejects_void_member();
    test_match_type_labels_parse();
    test_match_binding_prefix_parse();
    test_match_binding_prefix_expression_form();
    test_for_in_loop();
    test_block_yield_omits_trailing_semicolon();
    test_non_generic_array_new_uses_colon_dimension_syntax();
    test_generic_array_new_uses_colon_dimension_syntax();
    test_index_expression_is_unambiguous_and_remains_value_brackets();
    test_non_generic_type_brackets_without_colon_parses_as_index();
    test_generic_type_brackets_without_colon_parses_as_index();
    test_member_annotations_and_constructors();
    test_static_members_parse();
    test_spec_static_members_parse();
    test_spec_static_member_parse_errors();
    test_static_member_parse_errors();
    test_ast_source_tokens();
    test_type_field_inferred_initializers();
    test_doc_comments_bind_to_declarations_and_members();
    test_doc_comments_require_immediate_declaration();
    test_parse_error();
    test_parse_error_after_annotation_semicolon();
    test_parse_error_top_level_fn_missing_body();
    test_parse_error_top_level_fn_missing_body_without_return_type();
    test_parse_error_top_level_fn_missing_body_with_void_return();
    test_parse_error_extern_fn_with_body();
    test_parse_error_member_fn_missing_body();
    test_parse_error_extern_fn_inside_type();
    test_parse_error_enum_member_declaration();
    test_parse_error_enum_initializer_expression();
    test_parse_error_empty_enum();
    test_parse_error_missing_top_level_fn_keyword();
    test_parse_error_missing_member_fn_keyword();
    test_parse_error_missing_member_binding_keyword();
    test_parse_error_missing_local_binding_keyword();
    test_parse_error_missing_identifier_in_qualified_name();
    test_parse_error_missing_identifier_in_member_access();
    test_finalizer_declaration();
    test_finalizer_declaration_with_void_return();
    test_constructor_with_void_return_type();
    test_parse_error_constructor_with_non_void_return();
    test_parse_error_finalizer_with_params();
    test_parse_error_finalizer_with_non_void_return();
    test_parse_error_finalizer_name_mismatch();
    test_parse_error_direct_finalizer_call();
    test_bitwise_expr_parsing();
    test_tilde_unary_parsing();
    test_address_of_unary_parsing();
    test_address_of_and_bitwise_and_disambiguation();
    test_compound_assignment_parsing();
    test_lambda_block_body_parses();
    test_lambda_block_body_with_arrow_is_rejected();
    test_generic_type_declaration();
    test_generic_type_declaration_with_constraint();
    test_generic_spec_declaration();
    test_generic_function_declaration();
    test_generic_function_multi_type_params();
    test_generic_type_ref_with_args();
    test_generic_type_ref_nested();
    test_cast_with_generic_array_target();
    test_postfix_pointer_type_refs();
    test_explicit_generic_call();
    test_explicit_generic_call_multi_type_args();
    test_explicit_generic_type_constructor_call();
    test_generic_method_uses_both_outer_and_method_type_params();
    test_generic_parse_error_colon_angle_in_type_position();
    test_generic_parse_error_nested_colon_angle_in_type_arg();
    test_generic_parse_error_missing_closing_gt();
    test_generic_parse_error_missing_type_param_name();
    test_generic_target_expression_argument_parses();
    test_generic_type_target_member_parses();
    test_variadic_parameter_parses();
    test_parse_error_variadic_not_last();
    test_parse_error_extern_fn_variadic();
    test_tuple_type_declaration_parses();
    test_tuple_type_arity_errors();
    test_tuple_literal_and_grouped_expression_parse();
    test_tuple_literal_arity_errors();
    test_destructuring_binding_parse();
    test_destructuring_binding_errors();
    test_empty_source_file();
    test_whitespace_only_source_file();
    test_line_comment_only_source_file();
    test_block_comment_only_source_file();
    test_mixed_comments_only_source_file();
    test_non_empty_source_without_module_is_rejected();
    test_visibility_without_module_is_rejected();
    test_infix_match_op_simple_value_parses();
    test_infix_match_op_range_and_type_parses();
    test_infix_match_op_multi_label_pipe_parses();
    test_infix_match_op_binding_parses();
    test_infix_match_op_in_if_while_condition_parses();
    test_infix_match_op_pipe_does_not_become_bit_or();
    test_infix_match_op_left_associative_chains();
    test_infix_match_op_mixed_with_relational_and_equality();
    puts("parser tests passed");
    return 0;
}
