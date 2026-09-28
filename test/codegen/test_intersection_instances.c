#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Analyze a fresh program with either local declarations or persisted symbols. */
static FengSemanticAnalysis *intersection_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, FengProgram **program) {
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "intersection_instances.ff", program, &parse);
    if (!parsed) fprintf(stderr, "%u:%u %s\n%s\n", parse.token.line, parse.token.column, parse.message, source);
    CHECK(parsed);
    const FengProgram *programs[] = {*program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    CHECK(ok && count == 0U);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Require code generation and strict host C compilation for each metadata path. */
static void intersection_emit(FengSemanticAnalysis *analysis, bool defaults,
    void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    if (defaults) CHECK(strstr(output.c_source, "FengSpecDefaultProjection__") != NULL);
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
}

/* No producer AST survives the public/workspace FT round trip. */
static void intersection_instance_packages(void (*compile_c)(const char *)) {
    const char *declarations =
        "open spec Parent<T>{func take(value:T):T;static func build(value:T):T;}"
        "open spec Number:Parent<i32>{}open spec Text:Parent<string>{}"
        "open spec Both:Parent<i32>&Parent<string>;open spec Extra{}"
        "open spec Nested:Both&Extra;open spec Generic<A,B>:Parent<A>&Parent<B>;"
        "open type Ref:Number,Text,Extra{func take(value:i32):i32{return value;}"
        "func take(value:string):string{return value;}static func build(value:i32):i32{return value;}"
        "static func build(value:string):string{return value;}}"
        "@value open type Value:Number,Text,Extra{func take(value:i32):i32{return value;}"
        "func take(value:string):string{return value;}static func build(value:i32):i32{return value;}"
        "static func build(value:string):string{return value;}}"
        "open func shared<T:Both>(value:T):string{return value.take(\"x\");}"
        "open func sharedStatic<T:Both>():i32{return T.build((i32)1);}";
    const char *calls =
        "func exercise():i32{let ref=Ref();let val=Value();"
        "let a:Nested=ref;let b:Nested=val;let c:Generic<i32,string> =ref;let d:Generic<i32,string> =val;"
        "((Parent<i32>)a).take((i32)1);((Parent<string>)b).take(\"x\");"
        "c.take((i32)1);d.take(\"x\");shared(ref);shared(val);"
        "return sharedStatic<Ref>()+sharedStatic<Value>();}";
    char source[4096];
    CHECK(snprintf(source, sizeof source, "open module instance.api;%s%s", declarations, calls) < (int)sizeof source);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = intersection_analyze(source, NULL, &program);
    /* The shared call must carry the selected leaf instance, not a name-only
     * reference to the first occurrence of the generic declaration. */
    const FengResolvedCallable *selected = NULL;
    for (size_t i = 0U; i < program->declaration_count; ++i) {
        const FengDecl *decl = program->declarations[i];
        if (decl->kind == FENG_DECL_FUNCTION && decl->as.function_decl.name.length == 6U &&
            memcmp(decl->as.function_decl.name.data, "shared", 6U) == 0) {
            const FengStmt *statement = decl->as.function_decl.body->statements[0];
            CHECK(statement->kind == FENG_STMT_RETURN);
            selected = &statement->as.return_value->as.call.resolved_callable;
        }
    }
    CHECK(selected != NULL && selected->kind == FENG_RESOLVED_CALLABLE_SPEC_METHOD);
    const FengTypeRef *owner = selected->owner_instance_type_ref;
    CHECK(owner != NULL && owner->kind == FENG_TYPE_REF_NAMED && owner->as.named.type_arg_count == 1U);
    const FengTypeRef *argument = owner->as.named.type_args[0];
    CHECK(argument->kind == FENG_TYPE_REF_NAMED && argument->as.named.segment_count == 1U &&
        argument->as.named.segments[0].length == 6U && memcmp(argument->as.named.segments[0].data, "string", 6U) == 0);
    intersection_emit(analysis, true, compile_c);
    char directory[] = "temp/intersection-ft-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char public_root[256], workspace_root[256];
    snprintf(public_root, sizeof public_root, "%s/public", directory);
    snprintf(workspace_root, sizeof workspace_root, "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &options, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, profile ? workspace_root : public_root,
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        CHECK(snprintf(source, sizeof source, "module instance.consumer;import instance.api;%s", calls) < (int)sizeof source);
        analysis = intersection_analyze(source, &query, &program);
        CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
        intersection_emit(analysis, false, compile_c);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
}

/* Closed component arguments must survive regardless of the outer arity. */
void test_intersection_instances_codegen(void (*compile_c)(const char *)) {
    static const char *const declarations[] = {
        "spec Both: Parent<i32> & Parent<string>;",
        "spec Generic<A,B>: Parent<A> & Parent<B>; spec Both: Generic<i32,string> & Extra;",
        "spec Inner: Parent<i32> & Parent<string>; spec Both: Inner & Extra;",
        "spec Inner: Parent<i32> & Parent<string>; spec Middle: Inner & Extra; spec Both: Middle & Inner;"
    };
    for (size_t i = 0U; i < sizeof(declarations) / sizeof(declarations[0]); ++i) {
        char source[2048];
        snprintf(source, sizeof source, "module intersection.instances;"
            "spec Parent<T>{func take(value:T):T;static func build(value:T):T;}spec Extra{}%s"
            "func zero():Both{let value:Both;return value;}"
            "func left(value:Both):Parent<i32>{return (Parent<i32>)value;}"
            "func right(value:Both):Parent<string>{return (Parent<string>)value;}"
            "func call(value:Both):string{value.take((i32)1);return value.take(\"x\");}", declarations[i]);
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = intersection_analyze(source, NULL, &program);
        intersection_emit(analysis, true, compile_c);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    intersection_instance_packages(compile_c);
    puts("intersection instances: closed and nested component codegen passed");
}
