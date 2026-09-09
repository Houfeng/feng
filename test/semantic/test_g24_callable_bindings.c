#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Positive cases retain their constraints; negatives must fail before Codegen. */
static void callable_binding_case(const char *source, const char *code, const char *token) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "callable_bindings.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != (code == NULL) || count != (code == NULL ? 0U : 1U) ||
        (code != NULL && count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "expected %s\n%s", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(ok == (code == NULL) && count == (code == NULL ? 0U : 1U));
    if (code != NULL) {
        CHECK(strcmp(errors[0].code, code) == 0);
        CHECK(errors[0].token.length == strlen(token));
        CHECK(memcmp(errors[0].token.lexeme, token, strlen(token)) == 0);
        CHECK(strcmp(errors[0].path, "callable_bindings.ff") == 0);
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Different owner instances remain different language types; repairing their
 * runtime descriptor selection must not admit incompatible inputs or results. */
static void owner_instance_type_boundaries(void) {
    const char *bodies[] = {
        "func run(a:Owner<i32>,b:Owner<string>):string{return a.other<string>(b);}",
        "func run(a:Owner<i32>):string{return a.store<string>(\"value\");}",
        "func run(a:Owner<i32>,b:Owner<string>):i32{return a.other<string>(b);}",
        "func run(a:Owner<i32>,b:Owner<i32>):string{return a.other<string>(b);}",
        "func run(a:Owner<i32>):string{return a.store<string>(1);}"
    };
    for (size_t value = 0U; value < 2U; ++value) {
        for (size_t entry = 0U; entry < sizeof(bodies) / sizeof(bodies[0]); ++entry) {
            char source[1300];
            snprintf(source, sizeof source,
                "module g24.owner_boundaries;%stype Owner<T>{open var value:T;"
                "open static var saved:T;"
                "func other<U>(peer:Owner<U>):U{return peer.value;}"
                "func store<U>(next:U):U{Owner<U>.saved=next;return Owner<U>.saved;}}%s",
                value ? "@value " : "", bodies[entry]);
            FengProgram *program = NULL;
            FengParseError parse = {0};
            CHECK(feng_parse_source(source, strlen(source), "owner_boundaries.ff", &program, &parse));
            const FengProgram *programs[] = {program};
            FengSemanticAnalyzeOptions options = {
                .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
            };
            FengSemanticAnalysis *analysis = NULL;
            FengSemanticError *errors = NULL;
            size_t count = 0U;
            bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
            CHECK(ok == (entry < 2U));
            CHECK(entry < 2U ? count == 0U : count > 0U);
            for (size_t index = 0U; index < count; ++index) {
                CHECK(strncmp(errors[index].code, "AE", 2U) == 0);
                CHECK(strcmp(errors[index].path, "owner_boundaries.ff") == 0);
            }
            feng_semantic_errors_free(errors, count);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
    }
}

/* Owner/method constraints, field mutability and static arguments are independent. */
void test_g24_callable_bindings_semantics(void) {
    owner_instance_type_boundaries();
    const char *constraints[] = {": Both", ": Right", ": Left", ""};
    for (size_t constraint = 0U; constraint < 4U; ++constraint) {
        for (size_t entry = 0U; entry < 4U; ++entry) {
            char declaration[500];
            if (entry < 3U) {
                snprintf(declaration, sizeof declaration,
                    "type Owner<T%s> { open %s%s callback:Reader<T> = inner<T>; }\n",
                    constraints[constraint], entry == 2U ? "static " : "",
                    entry == 1U ? "var" : "let");
            } else {
                snprintf(declaration, sizeof declaration,
                    "type Owner<A> { func make<T%s>():Reader<T> { return inner<T>; } }\n",
                    constraints[constraint]);
            }
            char source[1000];
            snprintf(source, sizeof source,
                "module g24.callable_bindings;\n"
                "spec Left {func left():i32;} spec Right {func right():i32;}\n"
                "spec Both:Left & Right; spec Reader<T>(value:T):i32;\n"
                "func inner<T:Right>(value:T):i32 {return value.right();}\n%s",
                declaration);
            callable_binding_case(source, constraint < 2U ? NULL : "AE0522", "inner");
        }
    }
    for (size_t value = 0U; value < 2U; ++value) {
        for (size_t valid = 0U; valid < 2U; ++valid) {
            char source[600];
            snprintf(source, sizeof source,
                "module g24.callable_arguments; spec Reader<T>(value:T):i32;\n"
                "%stype Owner<T> {open static let callback:Reader<T>;}\n"
                "func run():i32 {return Owner<i32>.callback(%s);}\n",
                value ? "@value " : "", valid ? "3" : "\"wrong\"");
            callable_binding_case(source, valid ? NULL : "AE0506", "callback");
        }
    }
    puts("G24 callable binding semantic matrices passed");
}
