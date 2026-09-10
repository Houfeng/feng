#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Analyze source and imported signatures independently, with host layout. */
static FengSemanticAnalysis *arguments_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, FengProgram **program) {
    FengParseError parse = {0};
    bool ok = feng_parse_source(source, strlen(source), "generic_argument_static.ff", program, &parse);
    if (!ok) fprintf(stderr, "%u: %s\n", parse.token.line, parse.message);
    CHECK(ok);
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i)
        fprintf(stderr, "%u: %s %s\n", errors[i].token.line, errors[i].code, errors[i].message);
    CHECK(ok && count == 0U);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Every tested constructed actual must avoid all three local metadata layers. */
static void arguments_emit(FengSemanticAnalysis *analysis, bool closed,
    bool shared, void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%u: %s %s\n", error.token.line, error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    CHECK(strstr(output.c_source, "&(const FengGenericParamDescriptor){") == NULL);
    CHECK(strstr(output.c_source, "&(const FengTypeDescriptor){") == NULL);
    CHECK(strstr(output.c_source, "(const FengGenericParamDescriptor *const[]){") == NULL);
    if (closed) {
        CHECK(strstr(output.c_source, "static const FengGenericArguments _feng_generic_args_") != NULL);
        CHECK(strstr(output.c_source, "static const FengGenericParamDescriptor _feng_closed_generic_param_desc_") != NULL);
    }
    if (shared) CHECK(strstr(output.c_source, "_generic_args->params[") != NULL);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
}

/* Package-local names, field domains, method domains and callable adapters. */
static const char *arguments_provider =
    "open module vendor.arguments;\n"
    "open func identity<U>(value:U):U{return value;}\n"
    "open spec Producer<T>():T;\n"
    "open spec Mapper<T>(value:T):T;\n"
    "open type Final<T>{open var values:T[];func ~Final(){identity<T[]>(self.values);}}\n"
    "open type Container<T>{open var values:T[]=identity<T[]>(T[:0]);\n"
    "open static var shared:T[]=identity<T[]>(T[:0]);\n"
    "func copy():T[]{return identity<T[]>(self.values);}\n"
    "func chain():T[]{return self.copy();}\n"
    "open static func relay<U>(value:U[]):U[]{return identity<U[]>(value);}}\n"
    "open func create<T>():Container<T>{return Container<T>();}\n"
    "open func array<T>(value:T[]):T[]{return identity<T[]>(value);}\n"
    "open func container<T>(value:Container<T>):Container<T>{return identity<Container<T>>(value);}\n"
    "open func close<T>(values:T[]):Producer<T[]>{return () -> identity<T[]>(values);}\n"
    "open func nested<T>(values:Container<T[]>[][]):Container<T[]>[][]{\n"
    "return identity<Container<T[]>[][]>(values);}\n"
    "open func rank<T>(values:T[][][]):T[][][]{return identity<T[][][]>(values);}\n"
    "open func reordered<A,B>(a:A[],b:B[]):B[]{identity<A[]>(a);return identity<B[]>(b);}\n"
    "open func recursive<T>(values:T[],n:i32):T[]{\n"
    "if(n>0){return recursive<T>(values,n-1);}return identity<T[]>(values);}\n"
    "open type Step<T>(bool,T);open type CountStep(bool,int);\n"
    "@value open type Cursor<T>{open var values:T[];open var missing:T;\n"
    "open var index:int;open var total:int;\n"
    "@iterator open func next():Step<T>{let values=identity<T[]>(self.values);\n"
    "if self.index>=self.total{return (false,self.missing);}\n"
    "let value=values[self.index];self.index+=1;return (true,value);}}\n"
    "open type Sequence<T>{open var values:T[];open var missing:T;open var total:int;\n"
    "@iterable open func iter():Cursor<T>{return identity<Cursor<T>>(\n"
    "Cursor<T>{values:self.values,missing:self.missing,total:self.total});}}\n"
    "open func count<T>(values:Sequence<T>):int{\n"
    "var count=0;for let value in values{count+=1;}return count;}\n"
    "open func direct<T>(values:Sequence<T>):Cursor<T>{return values.iter();}\n"
    "open type RefCursor<T>{open var values:T[];open var total:int;open var index:int;\n"
    "@iterator open func next():CountStep{\n"
    "identity<T[]>(self.values);if self.index>=self.total{return (false,0);}\n"
    "self.index+=1;return (true,self.index);}}\n"
    "open type RefSequence<T>{open var values:T[];open var total:int;}\n"
    "open fit RefSequence<T>{@iterable open func iter():RefCursor<T>{\n"
    "return identity<RefCursor<T>>(RefCursor<T>{values:self.values,total:self.total});}}\n"
    "open func countRef<T>(values:RefSequence<T>):int{\n"
    "var count=0;for let value in values{count+=value;}return count;}\n";

/* Deliberately rename the provider's formal T to U across the FT boundary. */
static const char *arguments_consumer =
    "module consumer;import vendor.arguments;\n"
    "func relay<U>(values:U[]):U[]{return array<U>(values);}\n"
    "func shared<U>():U[]{return Container<U>.shared;}\n"
    "func run():i32[]{let box=create<i32>();let result=container<i32>(box);\n"
    "let final=Final<i32>{};\n"
    "count<string>(Sequence<string>{values:[\"a\"],missing:\"\",total:1});\n"
    "direct<string>(Sequence<string>{values:[\"a\"],missing:\"\",total:1});\n"
    "countRef<string>(RefSequence<string>{values:[\"a\"],total:1});\n"
    "let mapper:Mapper<i32[]> = Container<i32>.relay<i32>;\n"
    "let reader:Producer<i32[]> = box.copy;let maker:Producer<Container<i32>> = create<i32>;\n"
    "let closure=close<i32>(reader());mapper(shared<i32>());maker();closure();\n"
    "nested<string>(Container<string[]>[][:0]);rank<i32>(i32[][][:0]);\n"
    "reordered<i32,i32>(box.values,result.values);recursive<i32>(box.values,2);\n"
    "return relay<i32>(box.chain());}\n";

/* Persist both public and workspace FT profiles, then discard provider AST. */
static void arguments_imported(void (*compile_c)(const char *)) {
    CHECK(mkdir("temp", 0755) == 0 || errno == EEXIST);
    char directory[] = "temp/generic-arguments-ft-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char public_root[256], workspace_root[256];
    snprintf(public_root, sizeof public_root, "%s/public", directory);
    snprintf(workspace_root, sizeof workspace_root, "%s/workspace", directory);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = arguments_analyze(arguments_provider, NULL, &program);
    arguments_emit(analysis, false, true, compile_c);
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &options, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider,
            profile ? workspace_root : public_root,
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        analysis = arguments_analyze(arguments_consumer, &query, &program);
        CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
        arguments_emit(analysis, true, false, compile_c);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
}

/* Removing required metadata must fail, never reconstruct a runtime carrier. */
static void arguments_reject_missing_dependency(void) {
    const char *source = "module invalid.metadata;func id<U>(x:U):U{return x;}"
        "func relay<T>(x:T[]):T[]{return id<T[]>(x);}";
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = arguments_analyze(source, NULL, &program);
    FengReifiableDepSet *set = feng_semantic_get_or_create_reifiable_dep_set(analysis,
        program->declarations[1]);
    CHECK(set != NULL && set->callable_dep_count != 0U);
    size_t count = set->callable_dep_count;
    set->callable_dep_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    CHECK(!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error));
    CHECK(error.code != NULL && strcmp(error.code, "IE0002") == 0);
    set->callable_dep_count = count;
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Static metadata must not change constraint or array-mutability acceptance. */
static void arguments_reject_invalid_actuals(void) {
    const char *sources[] = {
        "module invalid.readonly;func id<U>(x:U):U{return x;}"
        "func relay<T>(x:T[]):T[!]{return id<T[!]>(x);}",
        "module invalid.constraint;spec Marker{func mark():i32;}"
        "func id<U:Marker>(x:U):U{return x;}func relay<T>(x:T[]):T[]{return id<T[]>(x);}",
        "module invalid.arity;func id<U>(x:U):U{return x;}"
        "func relay<T>(x:T[]):T[]{return id<T[],T[]>(x);}",
    };
    const char *codes[] = {"AE0512", "AE0512", "AE0233"};
    for (size_t i = 0U; i < sizeof sources / sizeof sources[0]; ++i) {
        FengProgram *program = NULL;
        FengParseError parse = {0};
        CHECK(feng_parse_source(sources[i], strlen(sources[i]), "invalid_actual.ff", &program, &parse));
        const FengProgram *programs[] = {program};
        FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
            .pointer_size = feng_get_host_pointer_size()};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        CHECK(!feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count));
        bool rejected_call = false;
        for (size_t j = 0U; j < count; ++j) {
            if (strcmp(errors[j].code, codes[i]) == 0) rejected_call = true;
        }
        CHECK(rejected_call);
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* Local named, aggregate, array, callable and constrained actuals share slots. */
void test_generic_argument_static_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "module arguments.local;\n"
        "func id<T>(value:T):T{return value;}\n"
        "spec Marker{func mark():i32;}\n"
        "type Container<T>:Marker{open var value:T;func mark():i32{return 7;}}\n"
        "@value type Value<T>{open var value:T;}\n"
        "spec Reader<T>():T;spec Choice<T>:T|i32;\n"
        "func constrained<U:Marker>(value:U):U{return value;}\n"
        "func mixed<T>(a:Container<T>,b:Value<T>,c:Reader<T>,d:Choice<T>):Container<T>{\n"
        "id<Value<T>>(b);id<Reader<T>>(c);id<Choice<T>>(d);id<Container<T>>(a);\n"
        "return constrained<Container<T>>(a);}\n"
        "fit T[]{func copy():T[]{return id<T[]>(self);}}\n"
        "func copied<T>(a:T[][]):T[][]{return a.copy();}\n"
        "func run():Container<string>{let a=Container<string>{value:\"a\"};\n"
        "let b=Value<string>{value:\"b\"};let c:Reader<string> = () -> \"c\";\n"
        "let d:Choice<string> = \"d\";copied<i32>(i32[][:0]);return mixed<string>(a,b,c,d);}\n";
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = arguments_analyze(source, NULL, &program);
    arguments_emit(analysis, true, true, compile_c);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    arguments_imported(compile_c);
    arguments_reject_missing_dependency();
    arguments_reject_invalid_actuals();
    puts("static constructed generic arguments and FT profiles passed");
}
