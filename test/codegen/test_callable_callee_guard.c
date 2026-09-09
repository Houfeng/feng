#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Extract a generated definition rather than its preceding prototype. */
static char *guard_function_body(const char *source, const char *name) {
    const char *cursor = source;
    while ((cursor = strstr(cursor, name)) != NULL) {
        const char *brace = strchr(cursor, '{');
        const char *semicolon = strchr(cursor, ';');
        if (brace != NULL && (semicolon == NULL || brace < semicolon)) {
            size_t depth = 1U;
            const char *end = brace + 1;
            while (*end != '\0' && depth != 0U) {
                if (*end == '{') ++depth;
                if (*end == '}') --depth;
                ++end;
            }
            CHECK(depth == 0U);
            size_t length = (size_t)(end - brace);
            char *copy = malloc(length + 1U);
            CHECK(copy != NULL);
            memcpy(copy, brace, length);
            copy[length] = '\0';
            return copy;
        }
        ++cursor;
    }
    CHECK(false);
    return NULL;
}

/* Parse and emit a fresh program; no existing test expectations are migrated. */
static char *guard_emit(const char *source, void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "callee_guard.ff", &program, &parse);
    if (!parsed) fprintf(stderr, "%s\n%s\n", parse.message, source);
    CHECK(parsed);
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n%s", error.code, error.message, source);
    CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    char *result = strdup(output.c_source);
    CHECK(result != NULL);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    return result;
}

/* Guards are required only for unstable identities, independent of syntax. */
static void guard_identity_matrix(void (*compile_c)(const char *)) {
    const char *source =
        "module g24.callee_guards;\n"
        "spec Reader<T>(value:T):i32; func first<T>(value:T):i32{return 31;}\n"
        "type Fixed {open let callback:Reader<i32> = first<i32>;}\n"
        "type Mutable {open var callback:Reader<i32> = first<i32>;}\n"
        "type Static<T> {open static let fixed:Reader<T> = first<T>; open static var changing:Reader<T> = first<T>;}\n"
        "func stable(value:Reader<i32>):i32{return value(0);}\n"
        "func localStable(value:Reader<i32>):i32{let selected=value;return selected(0);}\n"
        "func localMutable(value:Reader<i32>):i32{var selected=value;return selected(0);}\n"
        "func fieldStable(value:Fixed):i32{return value.callback(0);}\n"
        "func fieldMutable(value:Mutable):i32{return value.callback(0);}\n"
        "func staticStable():i32{return Static<i32>.fixed(0);}\n"
        "func staticMutable():i32{return Static<i32>.changing(0);}\n"
        "func genericStable<C:Reader<i32>>(value:C):i32{return value(0);}\n"
        "func genericMutable<C:Reader<i32>>(value:C):i32{var selected:C=value;return selected(0);}\n"
        "func close(value:Reader<i32>):i32{return genericStable<Reader<i32>>(value)+genericMutable<Reader<i32>>(value);}\n";
    char *generated = guard_emit(source, compile_c);
    const char *stable[] = {"__stable__from__", "__localStable__from__", "__fieldStable__from__",
                            "__staticStable__from__", "__genericStable_G__from__"};
    const char *mutable[] = {"__localMutable__from__", "__fieldMutable__from__",
                             "__staticMutable__from__", "__genericMutable_G__from__"};
    for (size_t i = 0U; i < sizeof(stable) / sizeof(stable[0]); ++i) {
        char *body = guard_function_body(generated, stable[i]);
        CHECK(strstr(body, "feng_retain(_callee_guard") == NULL);
        free(body);
    }
    for (size_t i = 0U; i < sizeof(mutable) / sizeof(mutable[0]); ++i) {
        char *body = guard_function_body(generated, mutable[i]);
        const char *retain = strstr(body, "feng_retain(_callee_guard");
        const char *invoke = strstr(body, "->invoke(");
        const char *release = strstr(body, "feng_release(_callee_guard");
        CHECK(retain != NULL && invoke != NULL && release != NULL);
        CHECK(retain < invoke && invoke < release);
        CHECK(strstr(retain + 1, "feng_retain(_callee_guard") == NULL);
        CHECK(strstr(release + 1, "feng_release(_callee_guard") == NULL);
        free(body);
    }
    free(generated);
}

/* Many owning argument temporaries force scope reallocation and leave cleanup
 * nodes above the guard. Its normal release must not pop those argument nodes. */
static void guard_scope_growth(void (*compile_c)(const char *)) {
    char source[16000];
    size_t used = (size_t)snprintf(source, sizeof(source),
        "module g24.callee_growth; type Value{open var number:i32;} spec Wide(");
    for (size_t i = 0U; i < 40U; ++i) used += (size_t)snprintf(source + used, sizeof(source) - used,
        "%sp%zu:Value", i == 0U ? "" : ",", i);
    used += (size_t)snprintf(source + used, sizeof(source) - used,
        "):i32; type Owner{open var callback:Wide;} func run(owner:Owner):i32{return owner.callback(");
    for (size_t i = 0U; i < 40U; ++i) used += (size_t)snprintf(source + used, sizeof(source) - used,
        "%sValue{number:%zu}", i == 0U ? "" : ",", i);
    used += (size_t)snprintf(source + used, sizeof(source) - used, ");}\n");
    CHECK(used < sizeof(source));
    char *generated = guard_emit(source, compile_c);
    char *body = guard_function_body(generated, "__run__from__");
    const char *retain = strstr(body, "feng_retain(_callee_guard");
    const char *invoke = strstr(body, "->invoke(");
    const char *release = strstr(body, "feng_release(_callee_guard");
    CHECK(retain != NULL && invoke != NULL && release != NULL && retain < invoke && invoke < release);
    CHECK(strstr(release + 1, "feng_release(_callee_guard") == NULL);
    CHECK(strstr(release, "feng_cleanup_pop();") != NULL);
    free(body);
    free(generated);
}

/* Re-entering a generic member signature keeps its lexical parameter slots;
 * self and mutual owner references must not append the same declaration again. */
static void guard_recursive_owner_signatures(void (*compile_c)(const char *)) {
    const char *source =
        "module g24.recursive_owner;\n"
        "type Self<T>{func read<U>(other:Self<U>):i32{return 1;}}\n"
        "type Left<T>{func read<U>(other:Right<U>):i32{return 2;}}\n"
        "type Right<T>{func read<V>(other:Left<V>):i32{return 3;}}\n"
        "func run():i32{let a=Self<i32>{};let b=Self<string>{};"
        "let left=Left<i32>{};let right=Right<string>{};"
        "return a.read<string>(b)+left.read<string>(right)+right.read<i32>(left);}\n";
    char *generated = guard_emit(source, compile_c);
    CHECK(strlen(generated) < 400000U);
    free(generated);
}

/* Descriptor selection depends on the complete instance, not just its origin.
 * Same-owner calls keep the direct _td path; all other substitutions use the
 * existing managed/aggregate dependency slot without a runtime identity test. */
static void guard_owner_descriptor_identity(void (*compile_c)(const char *)) {
    for (size_t value_owner = 0U; value_owner < 2U; ++value_owner) {
        char source[2400];
        snprintf(source, sizeof source,
            "module g24.owner_identity;\n"
            "%stype Owner<T>{open var value:T;open static var saved:T;"
            "func read():T{return self.value;}"
            "func same(other:Owner<T>):T{return other.read();}"
            "func other<U>(peer:Owner<U>):U{return peer.read();}"
            "func store<U>(value:U):U{Owner<U>.saved=value;return Owner<U>.saved;}"
            "func nested<U>(value:U[]):U[]{let peer=Owner<U[]>{value:value};return peer.read();}}\n"
            "func run():i32{let a=Owner<i32>{value:31};let b=Owner<string>{value:\"value\"};"
            "let text=a.other<string>(b);let saved=a.store<string>(text);"
            "let items=a.nested<string>([saved]);return a.same(a);}\n",
            value_owner ? "@value " : "");
        char *generated = guard_emit(source, compile_c);
        const char *dependency = value_owner ? "_desc->reified_agg_deps[" : "_desc->reified_type_deps[";
        char *same = guard_function_body(generated, "Owner__i1__same(");
        /* Aggregate parameters already have a lifecycle descriptor in the
         * prologue. Inspect the call's owner argument, not unrelated reads. */
        const char *same_call = strstr(same, "Owner__i0__read(");
        CHECK(same_call != NULL);
        const char *same_end = strchr(same_call, ';');
        const char *same_owner = strstr(same_call, ", _td,");
        CHECK(same_end != NULL && same_owner != NULL && same_owner < same_end);
        free(same);
        const char *different[] = {"Owner__i2__other(", "Owner__i3__store(", "Owner__i4__nested("};
        for (size_t index = 0U; index < sizeof(different) / sizeof(different[0]); ++index) {
            char *body = guard_function_body(generated, different[index]);
            const char *use = strstr(body, index == 1U ? "_static_desc" : "Owner__i0__read(");
            CHECK(use != NULL);
            const char *end = strchr(use, ';');
            const char *selected = strstr(use, dependency);
            CHECK(end != NULL && selected != NULL && selected < end);
            CHECK(strstr(body, "feng_alloc(") == NULL || index == 2U);
            free(body);
        }
        free(generated);
    }
}

/* Public suite registration for the callee guard's cost and cleanup invariants. */
void test_callable_callee_guard(void (*compile_c)(const char *)) {
    guard_identity_matrix(compile_c);
    guard_scope_growth(compile_c);
    guard_recursive_owner_signatures(compile_c);
    guard_owner_descriptor_identity(compile_c);
    puts("callable callee guard cost and cleanup matrices passed");
}
