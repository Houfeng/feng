#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Static resolution records the exact fit and complete structural owner. */
static void container_static_selections(void) {
    const char *source = "module selections;"
        "fit T[]{static func echo(value:T):T{return value;}}"
        "fit T[!]{static func echo(value:T):T{return value;}}"
        "func run(){let left=i32[].echo(1);let right=i32[!].echo(2);}";
    FengProgram *program = NULL;
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "selections.ff", &program, &parse));
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    CHECK(feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count));
    CHECK(count == 0U);
    const FengBlock *body = program->declarations[2]->as.function_decl.body;
    for (size_t i = 0U; i < 2U; ++i) {
        const FengExpr *call = body->statements[i]->as.binding.initializer;
        const FengResolvedCallable *selected = &call->as.call.resolved_callable;
        CHECK(selected->kind == FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD);
        CHECK(selected->fit_decl == program->declarations[i]);
        CHECK(selected->member == selected->fit_decl->as.fit_decl.members[0]);
        CHECK(selected->owner_type_decl == NULL && selected->owner_instance_type_ref != NULL);
        CHECK(selected->owner_instance_type_ref->kind == FENG_TYPE_REF_ARRAY);
        CHECK(selected->owner_instance_type_ref->array_element_writable == (i == 1U));
        CHECK(selected->owner_instance_type_ref->as.inner->kind == FENG_TYPE_REF_NAMED);
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Invalid type-level access must stop at Semantic with one precise diagnostic. */
void test_generic_container_static_semantics(void) {
    container_static_selections();
    const char *bodies[] = {
        "func run(){Missing[].echo(1);}",
        "func run(){i32[].absent(1);}",
        "func run(){i32[!].echo(1);}",
        "func run(){i32[].instance();}",
        "func run(){i32[].echo(\"wrong\");}",
        "func run(){i32[].keep<i32,string>(1);}",
        "spec Mapper<T>(value:T):T;func run(){let f:Mapper<string> =i32[].echo;}",
        "func run(){let array:i32[]=[1];array[].echo(1);}",
    };
    const char *codes[] = {"AE1013", "AE0309", "AE0309", "AE0309", "AE0512", "AE1015", "AE0522", "AE1013"};
    for (size_t i = 0U; i < sizeof bodies / sizeof *bodies; ++i) {
        char source[1024];
        snprintf(source, sizeof source,
            "module container;fit T[]{static func echo(value:T):T{return value;}"
            "static func keep<U>(value:U):U{return value;}func instance():i32{return 1;}}\n%s\n", bodies[i]);
        FengProgram *program = NULL;
        FengParseError parse = {0};
        CHECK(feng_parse_source(source, strlen(source), "container.ff", &program, &parse));
        const FengProgram *programs[] = {program};
        FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
            .pointer_size = feng_get_host_pointer_size()};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
        if (ok || count != 1U || strcmp(errors[0].code, codes[i]) != 0) {
            fprintf(stderr, "case %zu expected %s\n", i, codes[i]);
            for (size_t j = 0U; j < count; ++j) fprintf(stderr, "%s %s\n", errors[j].code, errors[j].message);
        }
        CHECK(!ok && count == 1U && strcmp(errors[0].code, codes[i]) == 0);
        CHECK(errors[0].token.line == 2U && errors[0].token.column > 0U);
        if (i == 4U) CHECK(strstr(errors[0].message, "i32[].echo") != NULL);
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    puts("generic container static semantic boundaries passed");
}
