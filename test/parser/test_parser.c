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
        "pu mod libc.math;\n"
        "use libc.base;\n"
        "use libc.extra as extra;\n"
        "let point_lib = \"./libpoint.so\";\n"
        "@cdecl(point_lib)\n"
        "extern fn point_distance(p1: Point, p2: Point): float;\n"
        "@union\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
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
    ASSERT(program->declarations[2]->annotations[0].builtin_kind == FENG_ANNOTATION_UNION);
    ASSERT(program->declarations[2]->as.type_decl.member_count == 2U);

    ASSERT(program->declarations[3]->kind == FENG_DECL_SPEC);
    ASSERT(program->declarations[3]->annotation_count == 1U);
    ASSERT(program->declarations[3]->annotations[0].builtin_kind == FENG_ANNOTATION_ABI);
    ASSERT(program->declarations[3]->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE);
    ASSERT(program->declarations[3]->as.spec_decl.as.callable.param_count == 1U);
    ASSERT(program->declarations[3]->as.spec_decl.as.callable.return_type != NULL);

    feng_program_free(program);
}

static void test_extern_rejects_non_function_top_level_declarations(void) {
    static const char *kCases[] = {
        "mod demo.main;\nextern let value: int;\n",
        "mod demo.main;\nextern type Point {\n    var x: int;\n}\n",
        "mod demo.main;\nextern enum Status {\n    ok = 0;\n}\n",
        "mod demo.main;\nextern spec Reader {\n}\n",
        "mod demo.main;\nextern fit User: Named {\n}\n"
    };

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        FengProgram *program = NULL;
        FengParseError error;

        ASSERT(!feng_parse_source(kCases[i], strlen(kCases[i]), "extern_non_fn.f", &program, &error));
        ASSERT(program == NULL);
        ASSERT(error.message != NULL);
        ASSERT(strstr(error.message,
                      "'extern' can only be applied to top-level 'fn' declarations") != NULL);
    }
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

static void test_runtime_annotation_on_extern_function(void) {
    const char *source =
        "mod demo.main;\n"
        "@runtime\n"
        "extern fn feng_string_length(value: string): long;\n";
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

static void test_enum_declarations_parse(void) {
    const char *source =
        "mod demo.enums;\n"
        "pu enum Status {\n"
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

static void test_type_field_inferred_initializers(void) {
    const char *source =
        "mod demo.fields;\n"
        "type UserType {}\n"
        "type User {\n"
        "    let id: int = 0;\n"
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
    ASSERT(user->kind == FENG_DECL_TYPE);
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
        "mod demo.docs;\n"
        "/** top fn doc */\n"
        "@bounded\n"
        "pu fn run() {}\n"
        "/** type doc */\n"
        "type User {\n"
        "    /** field doc */\n"
        "    @bounded\n"
        "    pu let id: int;\n"
        "    /** method doc */\n"
        "    pu fn info(): int {\n"
        "        return self.id;\n"
        "    }\n"
        "}\n"
        "spec Named {\n"
        "    /** spec member doc */\n"
        "    fn name(): int;\n"
        "}\n"
        "fit User {\n"
        "    /** fit member doc */\n"
        "    fn extra(): int {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(feng_parse_source(source, strlen(source), "docs_bind.f", &program, &error));
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 4U);

    assert_slice_text(program->declarations[0]->doc_comment, "/** top fn doc */");
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
        "mod demo.docs;\n"
        "/** blank line breaks */\n"
        "\n"
        "fn first() {}\n"
        "/** normal comment breaks */\n"
        "// separator\n"
        "fn second() {}\n";
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
        "extern fn print(msg: string*): void;\n";
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
        "    extern fn info(): string*;\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "extern_fn_in_type.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "type members cannot use 'extern fn'") != NULL);
    ASSERT(error.token.kind == FENG_TOKEN_KW_EXTERN);
}

static void test_parse_error_enum_member_declaration(void) {
    const char *source =
        "mod demo.enums;\n"
        "enum Bad {\n"
        "    let code: int;\n"
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
        "mod demo.enums;\n"
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
        "mod demo.enums;\n"
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

static void test_address_of_unary_parsing(void) {
    const char *source =
        "mod demo.addr;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: i32;\n"
        "}\n"
        "@abi\n"
        "fn cmp(a: i32, b: i32): i32 {\n"
        "    return a - b;\n"
        "}\n"
        "fn run(point: Point, msg: string, arr: i32[], value: i32) {\n"
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
        "mod demo.addr;\n"
        "fn run(a: i32, b: i32) {\n"
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

static void test_non_generic_array_new_uses_colon_dimension_syntax(void) {
    const char *source =
        "mod demo.main;\n"
        "type Counter {\n"
        "    var value: int;\n"
        "}\n"
        "fn run(n: int) {\n"
        "    let a: Counter[!] = Counter[:3];\n"
        "    let b: int[!] = int[:n];\n"
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
        "mod demo.main;\n"
        "type Pair<A, B> {\n"
        "    var left: A;\n"
        "    var right: B;\n"
        "}\n"
        "fn run() {\n"
        "    let pairs: Pair<int, int>[!] = Pair<int, int>[:2];\n"
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
        "mod demo.main;\n"
        "fn run(items: int[]): int {\n"
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
        "mod demo.main;\n"
        "type Counter {\n"
        "    var value: int;\n"
        "}\n"
        "fn run() {\n"
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
        "mod demo.main;\n"
        "type Pair<A, B> {\n"
        "    var left: A;\n"
        "    var right: B;\n"
        "}\n"
        "fn run() {\n"
        "    let pairs: Pair<int, int>[!] = Pair<int, int>[2];\n"
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
        "mod demo.main;\n"
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
        "mod demo.main;\n"
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
        "mod demo.main;\n"
        "spec Container<T> {\n"
        "    fn fetch(): T;\n"
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
    /* fn identity<T>(value: T): T with one type parameter */
    const char *source =
        "mod demo.main;\n"
        "fn identity<T>(value: T): T {\n"
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
    /* fn zip<A, B: Named>(a: A, b: B): A with two type parameters */
    const char *source =
        "mod demo.main;\n"
        "fn zip<A, B: Named>(a: A, b: B): A {\n"
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
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let x: Map<string, int>;\n"
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
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    let x: Map<string, List<int>>;\n"
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
        "mod demo.main;\n"
        "type Entry<K, V> {\n"
        "    let key: K;\n"
        "    let value: V;\n"
        "}\n"
        "fn run<K, V>(items: Entry<K, V>[!]): Entry<K, V>[] {\n"
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
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    callee<int>(42);\n"
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
        "mod demo.main;\n"
        "fn run(): void {\n"
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
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    Map<string, int>();\n"
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
        "mod demo.main;\n"
        "fn run(a: Point*[], b: int[]*, c: byte*) {}\n";
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
    assert_slice_text(type_b->as.inner->as.inner->as.named.segments[0], "int");

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
        "mod demo.main;\n"
        "type Box<T> {\n"
        "    pu let value: T;\n"
        "    fn map<U>(x: U): T {\n"
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
        "mod demo.main;\n"
        "type Box:<T> {\n"
        "    pu let value: int;\n"
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
        "mod demo.main;\n"
        "fn run(): void {\n"
        "    foo<Map:<int>>(x);\n"
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
        "mod demo.main;\n"
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
    /* fn foo<, T>() — missing type parameter name */
    const char *source =
        "mod demo.main;\n"
        "fn foo<, T>(): void {\n"
        "}\n";
    FengProgram *program = NULL;
    FengParseError error;

    ASSERT(!feng_parse_source(source, strlen(source), "generic_err2.f", &program, &error));
    ASSERT(program == NULL);
    ASSERT(error.message != NULL);
}

static void test_generic_target_expression_argument_parses(void) {
    const char *source =
        "mod demo.main;\n"
        "fn run(): void {\n"
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

int main(void) {
    test_top_level_declarations();
    test_extern_rejects_non_function_top_level_declarations();
    test_statements_and_expressions();
    test_runtime_annotation_on_extern_function();
    test_enum_declarations_parse();
    test_match_with_range_and_list_labels();
    test_match_statement_form();
    test_for_in_loop();
    test_block_yield_omits_trailing_semicolon();
    test_non_generic_array_new_uses_colon_dimension_syntax();
    test_generic_array_new_uses_colon_dimension_syntax();
    test_index_expression_is_unambiguous_and_remains_value_brackets();
    test_non_generic_type_brackets_without_colon_parses_as_index();
    test_generic_type_brackets_without_colon_parses_as_index();
    test_member_annotations_and_constructors();
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
    puts("parser tests passed");
    return 0;
}
