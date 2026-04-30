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

static void test_top_level_declarations(void) {
    const char *source =
        "pu mod libc.math;\n"
        "use libc.base;\n"
        "use libc.extra as extra;\n"
        "let point_lib = \"./libpoint.so\";\n"
        "@cdecl(point_lib)\n"
        "extern fn point_distance(p1: Point, p2: Point): float;\n"
        "@union\n"
        "extern type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
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
    ASSERT(program->declarations[2]->is_extern);
    ASSERT(program->declarations[2]->annotation_count == 1U);
    ASSERT(program->declarations[2]->annotations[0].builtin_kind == FENG_ANNOTATION_UNION);
    ASSERT(program->declarations[2]->as.type_decl.member_count == 2U);

    ASSERT(program->declarations[3]->kind == FENG_DECL_SPEC);
    ASSERT(program->declarations[3]->annotation_count == 0U);
    ASSERT(program->declarations[3]->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE);
    ASSERT(program->declarations[3]->as.spec_decl.as.callable.param_count == 1U);
    ASSERT(program->declarations[3]->as.spec_decl.as.callable.return_type != NULL);

    feng_program_free(program);
}

static void test_statements_and_expressions(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(args: string[]) {\n"
        "    let label = if age >= 18 { \"adult\"; } else { \"minor\"; };\n"
        "    let stage = if age { 0 { \"婴儿\"; } 18 { \"成年\"; } else { \"青年\"; } };\n"
        "    for var i = 0; i < 3; i = i + 1 {\n"
        "        if i == 1 {\n"
        "            continue;\n"
        "        } else {\n"
        "            print(i);\n"
        "        }\n"
        "    }\n"
        "    try {\n"
        "        throw \"boom\";\n"
        "    } catch {\n"
        "        print(\"err\");\n"
        "    } finally {\n"
        "        print(\"done\");\n"
        "    }\n"
        "    return (i32)1;\n"
        "}\n"
        "fn make_adder(base: int): IntToInt {\n"
        "    return (x: int) -> base + x;\n"
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
    ASSERT(main_body->statement_count == 5U);

    ASSERT(main_body->statements[0]->kind == FENG_STMT_BINDING);
    ASSERT(main_body->statements[0]->as.binding.initializer->kind == FENG_EXPR_IF);
    ASSERT(main_body->statements[1]->kind == FENG_STMT_BINDING);
    ASSERT(main_body->statements[1]->as.binding.initializer->kind == FENG_EXPR_MATCH);
    ASSERT(main_body->statements[2]->kind == FENG_STMT_FOR);
    ASSERT(main_body->statements[3]->kind == FENG_STMT_TRY);
    ASSERT(main_body->statements[4]->kind == FENG_STMT_RETURN);
    ASSERT(main_body->statements[4]->as.return_value->kind == FENG_EXPR_CAST);

    ASSERT(program->declarations[1]->kind == FENG_DECL_FUNCTION);
    ASSERT(program->declarations[1]->as.function_decl.body->statement_count == 1U);
    ASSERT(program->declarations[1]->as.function_decl.body->statements[0]->kind == FENG_STMT_RETURN);
    ASSERT(program->declarations[1]->as.function_decl.body->statements[0]->as.return_value->kind == FENG_EXPR_LAMBDA);

    feng_program_free(program);
}

static void test_member_annotations_and_constructors(void) {
    const char *source =
        "mod demo.user;\n"
        "type User {\n"
        "    pu var name: string;\n"
        "    @bounded\n"
        "    pu let id: int;\n"
        "    pu let created_at: int;\n"
        "    @bounded(created_at)\n"
        "    fn User(ts: int) {}\n"
        "    pu fn info(): string {\n"
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
    ASSERT(decl->as.type_decl.members[1]->annotations[0].builtin_kind == FENG_ANNOTATION_BOUNDED);
    ASSERT(decl->as.type_decl.members[2]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(decl->as.type_decl.members[3]->kind == FENG_TYPE_MEMBER_CONSTRUCTOR);
    ASSERT(decl->as.type_decl.members[3]->annotation_count == 1U);
    ASSERT(decl->as.type_decl.members[3]->annotations[0].arg_count == 1U);
    ASSERT(decl->as.type_decl.members[3]->as.callable.body != NULL);
    ASSERT(decl->as.type_decl.members[4]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(decl->as.type_decl.members[4]->as.callable.body != NULL);

    feng_program_free(program);
}

static void test_ast_source_tokens(void) {
    const char *source =
        "pu mod demo.main;\n"
        "use demo.base;\n"
        "@bounded\n"
        "fn main(arg: int) {\n"
        "    let answer: int = 42;\n"
        "    return answer;\n"
        "}\n"
        "type User {\n"
        "    pu let id: int = 1;\n"
        "    fn User(value: int) {\n"
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

static void test_parse_error(void) {
    const char *source = "fn main(args: string[]) {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "error.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_parse_error_after_annotation_semicolon(void) {
    const char *source =
        "mod demo.main;\n"
        "@cdecl(\"libc\");\n"
        "extern fn print(msg: string): void;\n";
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
        "mod demo.main;\n"
        "fn point_sum(a: int, b: int): int;\n";
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
        "mod demo.main;\n"
        "fn print(msg: string);\n";
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
        "mod demo.main;\n"
        "fn print(msg: string): void;\n";
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
        "mod demo.main;\n"
        "extern fn point_sum(a: int, b: int): int {\n"
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
        "mod demo.user;\n"
        "type User {\n"
        "    fn info(): string;\n"
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
        "mod demo.user;\n"
        "type User {\n"
        "    extern fn info(): string;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "extern_fn_in_type.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "type members cannot use 'extern fn'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_KW_EXTERN);
}

static void test_parse_error_missing_top_level_fn_keyword(void) {
    const char *source =
        "mod demo.main;\n"
        "main(args: string[]) {}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_fn.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "top-level function declarations must start with 'fn'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_IDENTIFIER);
}

static void test_parse_error_missing_member_fn_keyword(void) {
    const char *source =
        "mod demo.user;\n"
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
    ASSERT(strstr(error.message, "type methods and constructors must start with 'fn'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_IDENTIFIER);
}

static void test_parse_error_missing_member_binding_keyword(void) {
    const char *source =
        "mod demo.user;\n"
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
        "mod demo.main;\n"
        "fn main(args: string[]) {\n"
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
        "mod demo.;\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "missing_qualified_name_part.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "expected an identifier after '.' in a qualified name") != NULL);
}

static void test_parse_error_missing_identifier_in_member_access(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(args: string[]) {\n"
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
        "mod demo.user;\n"
        "type Buffer {\n"
        "    pu var size: int;\n"
        "    fn Buffer(s: int) {}\n"
        "    fn ~Buffer() {}\n"
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
        "mod demo.user;\n"
        "type Buffer {\n"
        "    fn ~Buffer(): void {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "fin_void.f", &program, &error));
    ASSERT(program->declarations[0]->as.type_decl.members[0]->kind == FENG_TYPE_MEMBER_FINALIZER);
    feng_program_free(program);
}

static void test_constructor_with_void_return_type(void) {
    const char *source =
        "mod demo.user;\n"
        "type Box {\n"
        "    fn Box(): void {}\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "ctor_void.f", &program, &error));
    ASSERT(program->declarations[0]->as.type_decl.members[0]->kind == FENG_TYPE_MEMBER_CONSTRUCTOR);
    feng_program_free(program);
}

static void test_parse_error_constructor_with_non_void_return(void) {
    const char *source =
        "mod demo.user;\n"
        "type Box {\n"
        "    fn Box(): int { return 0; }\n"
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
        "mod demo.user;\n"
        "type Box {\n"
        "    fn ~Box(x: int) {}\n"
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
        "mod demo.user;\n"
        "type Box {\n"
        "    fn ~Box(): int { return 0; }\n"
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
        "mod demo.user;\n"
        "type Box {\n"
        "    fn ~Other() {}\n"
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
        "mod demo.user;\n"
        "fn main(args: string[]) {\n"
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
        "mod demo.bits;\n"
        "fn f(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32): bool {\n"
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
        "mod demo.bits;\n"
        "fn f(a: i32): i32 {\n"
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

static void test_compound_assignment_parsing(void) {
    const char *source =
        "mod demo.ops;\n"
        "fn run() {\n"
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
        "mod demo.main;\n"
        "fn run(): int {\n"
        "    let f = (a: int) {\n"
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
        "mod demo.main;\n"
        "fn run(): int {\n"
        "    let f = (a: int) -> {\n"
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
        "mod demo.main;\n"
        "fn run(age: int): string {\n"
        "    return if age {\n"
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
        "mod demo.main;\n"
        "fn run(age: int) {\n"
        "    if age {\n"
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

static void test_for_in_loop(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(items: int[]) {\n"
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
        "mod demo.main;\n"
        "fn run(value: int): int {\n"
        "    let x = if value > 0 { 1 } else { 2 };\n"
        "    return x;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "block_yield.f", &program, &error));
    ASSERT(program != NULL);

    feng_program_free(program);
}

int main(void) {
    test_top_level_declarations();
    test_statements_and_expressions();
    test_match_with_range_and_list_labels();
    test_match_statement_form();
    test_for_in_loop();
    test_block_yield_omits_trailing_semicolon();
    test_member_annotations_and_constructors();
    test_ast_source_tokens();
    test_parse_error();
    test_parse_error_after_annotation_semicolon();
    test_parse_error_top_level_fn_missing_body();
    test_parse_error_top_level_fn_missing_body_without_return_type();
    test_parse_error_top_level_fn_missing_body_with_void_return();
    test_parse_error_extern_fn_with_body();
    test_parse_error_member_fn_missing_body();
    test_parse_error_extern_fn_inside_type();
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
    test_compound_assignment_parsing();
    test_lambda_block_body_parses();
    test_lambda_block_body_with_arrow_is_rejected();
    puts("parser tests passed");
    return 0;
}
