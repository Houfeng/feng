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
        "type PointCallback(p: Point): void;\n";
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
    ASSERT(program->declarations[2]->as.type_decl.form == FENG_TYPE_DECL_OBJECT);
    ASSERT(program->declarations[2]->as.type_decl.as.object.member_count == 2U);

    ASSERT(program->declarations[3]->kind == FENG_DECL_TYPE);
    ASSERT(program->declarations[3]->as.type_decl.form == FENG_TYPE_DECL_FUNCTION);
    ASSERT(program->declarations[3]->as.type_decl.as.function.param_count == 1U);
    ASSERT(program->declarations[3]->as.type_decl.as.function.return_type != NULL);

    feng_program_free(program);
}

static void test_statements_and_expressions(void) {
    const char *source =
        "mod demo.main;\n"
        "fn main(args: string[]) {\n"
        "    let label = if age >= 18 { \"adult\" } else { \"minor\" };\n"
        "    let stage = if age { 0: \"婴儿\", 18: \"成年\", else: \"青年\" };\n"
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
        "    fn User(ts: int);\n"
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
    ASSERT(decl->as.type_decl.as.object.member_count == 5U);
    ASSERT(decl->as.type_decl.as.object.members[1]->annotation_count == 1U);
    ASSERT(decl->as.type_decl.as.object.members[1]->annotations[0].builtin_kind == FENG_ANNOTATION_BOUNDED);
    ASSERT(decl->as.type_decl.as.object.members[2]->kind == FENG_TYPE_MEMBER_FIELD);
    ASSERT(decl->as.type_decl.as.object.members[3]->kind == FENG_TYPE_MEMBER_CONSTRUCTOR);
    ASSERT(decl->as.type_decl.as.object.members[3]->annotation_count == 1U);
    ASSERT(decl->as.type_decl.as.object.members[3]->annotations[0].arg_count == 1U);
    ASSERT(decl->as.type_decl.as.object.members[4]->kind == FENG_TYPE_MEMBER_METHOD);

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

int main(void) {
    test_top_level_declarations();
    test_statements_and_expressions();
    test_member_annotations_and_constructors();
    test_parse_error();
    test_parse_error_after_annotation_semicolon();
    test_parse_error_top_level_fn_missing_body();
    test_parse_error_extern_fn_with_body();
    test_parse_error_missing_top_level_fn_keyword();
    test_parse_error_missing_member_fn_keyword();
    test_parse_error_missing_member_binding_keyword();
    test_parse_error_missing_local_binding_keyword();
    test_parse_error_missing_identifier_in_qualified_name();
    test_parse_error_missing_identifier_in_member_access();
    puts("parser tests passed");
    return 0;
}
