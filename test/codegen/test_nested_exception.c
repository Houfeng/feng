#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

/* Compile a validated fixture and preserve the C for protocol assertions. */
static char *nested_emit(const char *source, void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "nested_exception.ff", &program, &parse);
    if (!parsed) fprintf(stderr, "%s\n", parse.message);
    CHECK(parsed);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()
    };
    bool analyzed = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!analyzed) {
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(analyzed && count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s %s\n", error.code, error.message);
    CHECK(emitted && output.c_source != NULL);
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

/* Extract a definition, skipping matching forward declarations. */
static char *nested_body(const char *source, const char *name) {
    const char *cursor = source;
    while ((cursor = strstr(cursor, name)) != NULL) {
        const char *brace = strchr(cursor, '{');
        const char *semicolon = strchr(cursor, ';');
        if (brace != NULL && (semicolon == NULL || brace < semicolon)) {
            const char *end = brace + 1;
            size_t depth = 1U;
            while (*end != '\0' && depth != 0U) {
                if (*end == '{') ++depth;
                if (*end == '}') --depth;
                ++end;
            }
            CHECK(depth == 0U);
            char *result = strndup(brace, (size_t)(end - brace));
            CHECK(result != NULL);
            return result;
        }
        ++cursor;
    }
    CHECK(false);
    return NULL;
}

/* Count protocol operations before one particular control transfer. */
static size_t nested_count_before(const char *body, const char *end, const char *needle) {
    size_t count = 0U;
    const char *cursor = body;
    CHECK(end != NULL);
    while ((cursor = strstr(cursor, needle)) != NULL && cursor < end) {
        ++count;
        cursor += strlen(needle);
    }
    return count;
}

/* Scope order, rather than global catch counts, determines exit operations. */
static void nested_control_cleanup(void (*compile_c)(const char *)) {
    const char *source =
        "module nested.cleanup; func fail(){throw 1;}\n"
        "func returnBoth():int{try fail() catch {try fail() catch {return 7;}}return 0;}\n"
        "func protectedReturn():int{try fail() catch {"
        "let value=try(if true{return 9;}else{0;}) catch {0;};return value;}return 0;}\n"
        "func breakBoth(){while true{try fail() catch {try fail() catch {break;}}}}\n"
        "func continueBoth(){while true{try fail() catch {try fail() catch {continue;}}}}\n"
        "func preserveOuter(){try fail() catch {while true{break;}throw;}}\n"
        "func preserveContinue(){try fail() catch {var i=0;while i<2{i+=1;continue;}throw;}}\n";
    char *generated = nested_emit(source, compile_c);
    CHECK(strstr(generated, "feng_release_unwind_exception") == NULL);
    /* Exceptional cleanups now name logical markers in native landing pads;
     * normal exits below must still use their original lexical pop order. */
    CHECK(strstr(generated, "feng_frame_release_to") != NULL);
    CHECK(strstr(generated, "__llvm_c_eh_propagate(") != NULL);
    CHECK(strstr(generated, "feng_register_lsda") == NULL);
    CHECK(strstr(generated, "FengCatchContext _try_marker") != NULL);
    CHECK(strstr(generated, ".frame);") != NULL);
    char *body = nested_body(generated, "__returnBoth__from__");
    const char *transfer = strstr(body, "return ");
    CHECK(nested_count_before(body, transfer, "feng_exception_catch_begin(") == 2U);
    CHECK(nested_count_before(body, transfer, "feng_exception_catch_end();") == 2U);
    free(body);

    body = nested_body(generated, "__protectedReturn__from__");
    transfer = strstr(body, "return ");
    /* Ignore the first try's normal-path pop, which precedes its landing pad. */
    const char *active = strstr(body, "feng_exception_catch_begin(");
    CHECK(active != NULL);
    CHECK(nested_count_before(active, transfer, "feng_exception_catch_end();") == 1U);
    CHECK(nested_count_before(active, transfer, "feng_frame_pop();") == 2U);
    const char *try_pop = strstr(active, "feng_frame_pop();");
    const char *catch_end = strstr(active, "feng_exception_catch_end();");
    const char *function_pop = strstr(try_pop + 1, "feng_frame_pop();");
    CHECK(try_pop < catch_end && catch_end < function_pop && function_pop < transfer);
    free(body);

    const char *exiting[] = {"__breakBoth__from__", "__continueBoth__from__"};
    const char *preserving[] = {"__preserveOuter__from__", "__preserveContinue__from__"};
    const char *jumps[] = {"break;", "continue;"};
    for (size_t i = 0U; i < 2U; ++i) {
        body = nested_body(generated, exiting[i]);
        active = strstr(body, "feng_exception_catch_begin(");
        CHECK(active != NULL);
        transfer = strstr(active, jumps[i]);
        CHECK(nested_count_before(body, transfer, "feng_exception_catch_end();") == 2U);
        free(body);
        body = nested_body(generated, preserving[i]);
        const char *rethrow = strstr(body, "feng_rethrow();");
        CHECK(rethrow != NULL);
        transfer = NULL;
        const char *cursor = body;
        while ((cursor = strstr(cursor, jumps[i])) != NULL && cursor < rethrow) {
            transfer = cursor;
            ++cursor;
        }
        CHECK(nested_count_before(body, transfer, "feng_exception_catch_end();") == 0U);
        CHECK(strstr(transfer, "feng_rethrow();") != NULL);
        free(body);
    }
    free(generated);
}

/* Borrowed bindings add no ARC or allocation until a capture requires ownership. */
static void nested_binding_ownership(void (*compile_c)(const char *)) {
    const char *source =
        "module nested.bindings;type Ref{let n:int;}@value type Val{let ref:Ref;}"
        "type Pair(Ref,int);enum Status{Failed=7}spec Read():int;"
        "func failRef(){throw Ref{n:1};}func failVal(){throw Val{ref:Ref{n:2}};}"
        "func failPair(){let p:Pair=(Ref{n:3},4);throw p;}"
        "func borrowed(){try failRef() catch value:Ref{value.n;}}"
        "func capturedRef():Read{try failRef() catch value:Ref{return (){return value.n;};}"
        "return (){return 0;};}"
        "func capturedVal():Read{try failVal() catch value:Val{return (){return value.ref.n;};}"
        "return (){return 0;};}"
        "func capturedPair():Read{try failPair() catch value:Pair{return (){let(r,n)=value;return r.n+n;};}"
        "return (){return 0;};}"
        "func capturedInt():Read{try failRef() catch value:int{return (){return value;};}"
        "return (){return 0;};}"
        "func capturedEnum():Read{try failRef() catch value:Status{return (){return (int)value;};}"
        "return (){return 0;};}"
        "func capturedString():Read{try failRef() catch value:string{return (){return if value==\"text\"{1;}else{0;};};}"
        "return (){return 0;};}";
    char *generated = nested_emit(source, compile_c);
    char *borrowed = nested_body(generated, "__borrowed__from__");
    CHECK(strstr(borrowed, "feng_caught_value()") != NULL);
    CHECK(strstr(borrowed, "feng_retain(") == NULL);
    CHECK(strstr(borrowed, "feng_object_new(") == NULL);
    CHECK(strstr(borrowed, "feng_alloc(") == NULL);
    const char *begin = strstr(borrowed, "feng_exception_catch_begin(");
    const char *read = strstr(borrowed, "feng_caught_value()");
    const char *end = strstr(borrowed, "feng_exception_catch_end();");
    CHECK(begin != NULL && read != NULL && end != NULL && begin < read && read < end);
    free(borrowed);
    free(generated);
}

/* Run compiler protocol/ownership checks separately from language behavior. */
void test_nested_exception_codegen(void (*compile_c)(const char *)) {
    nested_control_cleanup(compile_c);
    nested_binding_ownership(compile_c);
    puts("nested exception codegen scope and capture tests passed");
}
