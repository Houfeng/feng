#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Each negative changes only the generic satisfaction relationship. */
static const char *constraint_prefix =
    "module projection_semantics;\n"
    "spec Left<A>{func echo(value:A):A;} spec Right{func right():i32;}\n"
    "spec Extra{func extra():i32;} spec Both<A>:Left<A>&Right;\n"
    "spec All<A>:Both<A>&Extra; spec Other:Extra&Right;\n"
    "func left<A,T:Left<A>>(value:T,item:A):A{return value.echo(item);}\n"
    "func right<T:Right>(value:T):i32{return value.right();}\n"
    "func other<T:Other>(value:T):i32{return value.extra()+value.right();}\n"
    "func both<A,T:Both<A>>(value:T):i32{return right<T>(value);}\n";

/* Analyze actual source without manufacturing unreachable semantic states. */
static FengSemanticAnalysis *constraint_analyze(const char *body, bool positive,
    FengProgram **program, char **source_out) {
    size_t length = strlen(constraint_prefix) + strlen(body);
    char *source = malloc(length + 1U);
    CHECK(source != NULL);
    strcpy(source, constraint_prefix);
    strcat(source, body);
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, length, "constraint_projection.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != positive) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    CHECK(ok == positive);
    if (positive) CHECK(count == 0U);
    else {
        CHECK(count == 1U);
        CHECK(strcmp(errors[0].code, "AE0512") == 0);
        CHECK(errors[0].token.line == 9U);
        const char *call = strstr(body, "return ");
        CHECK(call != NULL);
        call += strlen("return ");
        const char *end = strchr(call, '<');
        CHECK(end != NULL && strcmp(errors[0].path, "constraint_projection.ff") == 0);
        CHECK(errors[0].token.column == (size_t)(call - body) + 1U);
        CHECK(errors[0].token.length == (size_t)(end - call));
        CHECK(memcmp(errors[0].token.lexeme, call, (size_t)(end - call)) == 0);
    }
    feng_semantic_errors_free(errors, count);
    /* Tokens borrow source bytes throughout both AST and analysis lifetimes. */
    *source_out = source;
    return analysis;
}

/* Explicit and inferred type arguments share canonical open slots, not call order. */
static void constraint_open_slots(void) {
    const char *bodies[] = {
        "func scan<Z:All<i32>,A:All<string>>(z:Z,a:A):i32{"
        "let r=right<A>(a);let l=left<i32,Z>(z,1);let s=other<Z>(z);return right(z)+r+l+s;}\n",
        "func scan<Z:All<i32>,A:All<string>>(z:Z,a:A):i32{"
        "let s=other<Z>(z);let l=left<i32,Z>(z,1);let r=right(a);return right<Z>(z)+r+l+s;}\n"
    };
    for (size_t order = 0U; order < 2U; ++order) {
        FengProgram *program = NULL;
        char *source = NULL;
        FengSemanticAnalysis *analysis = constraint_analyze(bodies[order], true, &program, &source);
        const FengDecl *scan = program->declarations[program->declaration_count - 1U];
        const FengReifiableDepSet *set = feng_semantic_lookup_reifiable_dep_set(analysis, scan);
        CHECK(set != NULL && set->constraint_projection_count == 4U);
        const char *sources[] = {"Z", "Z", "Z", "A"};
        const char *targets[] = {"Left", "Other", "Right", "Right"};
        for (size_t index = 0U; index < 4U; ++index) {
            const FengConstraintProjectionDep *dep = &set->constraint_projections[index];
            CHECK(dep->source_type_ref->as.named.segment_count == 1U);
            FengSlice name = dep->source_type_ref->as.named.segments[0];
            if (name.length != 1U || name.data[0] != sources[index][0])
                fprintf(stderr, "slot %zu: source %.*s, expected %s\n", index, (int)name.length, name.data, sources[index]);
            CHECK(name.length == 1U && name.data[0] == sources[index][0]);
            size_t last = dep->target_constraint_ref->as.named.segment_count - 1U;
            name = dep->target_constraint_ref->as.named.segments[last];
            CHECK(strlen(targets[index]) == name.length && memcmp(name.data, targets[index], name.length) == 0);
            CHECK(feng_semantic_constraint_projection_slot(set, dep) == index);
        }
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        free(source);
    }
}

/* The new codegen capability must not weaken nominal or generic constraints. */
void test_constraint_projection_semantics(void) {
    constraint_open_slots();
    const char *negative[] = {
        "func bad<T>(value:T):i32{return right<T>(value);}",
        "func bad<T:Right>(value:T):i32{return left<i32,T>(value,1);}",
        "func bad<T:Right>(value:T):i32{return both<i32,T>(value);}",
        "func bad<T:Both<i32>>(value:T):string{return left<string,T>(value,\"bad\");}",
        "func bad<T:Both<i32>>(value:T):i32{return other<T>(value);}"
    };
    for (size_t index = 0U; index < sizeof negative / sizeof *negative; ++index) {
        FengProgram *program = NULL;
        char *source = NULL;
        FengSemanticAnalysis *analysis = constraint_analyze(negative[index], false, &program, &source);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        free(source);
    }
    puts("constraint projection semantic and open-slot matrices passed");
}
