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

/* Keep producer and consumer analyses separate across the persisted FT boundary. */
static FengSemanticAnalysis *prototype_analyze(
    const char *source, const FengSemanticImportedModuleQuery *query,
    FengProgram **program) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "imported_callable_prototype.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* A provider signature has one C spelling even when callers use another formal. */
static void prototype_case(bool value_owner, void (*compile_c)(const char *)) {
    char source[2200];
    snprintf(source, sizeof source,
        "open module vendor.guard_proto;\n"
        "open spec Reader<T>(value:T):i32;\n"
        "open func read<T>(value:T):i32{return 31;}\n"
        "%sopen type Owner<T>{open var callback:Reader<T>;\n"
        "func invoke(reader:Reader<T>,value:T):i32{return reader(value);}\n"
        "func relay<U>(other:Owner<U>,reader:Reader<U>,value:U):i32{return other.invoke(reader,value);}\n"
        "open static func invoke_static(reader:Reader<T>,value:T):i32{return reader(value);}}\n",
        value_owner ? "@value " : "");
    CHECK(mkdir("temp", 0755) == 0 || errno == EEXIST);
    char directory[] = "temp/g24-imported-callable-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char public_root[256], workspace_root[256];
    snprintf(public_root, sizeof public_root, "%s/public", directory);
    snprintf(workspace_root, sizeof workspace_root, "%s/workspace", directory);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = prototype_analyze(source, NULL, &program);
    FengSymbolExportOptions export_options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError symbol_error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &export_options, &symbol_error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    const char *consumer =
        "module consumer;import vendor.guard_proto;\n"
        "func forward<U>(owner:Owner<U>,reader:Reader<U>,value:U):i32{return owner.invoke(reader,value);}\n"
        "func run():i32{let owner=Owner<i32>{};let other=Owner<string>{};"
        "let reader:Reader<i32> = read<i32>;let text:Reader<string> = read<string>;"
        "return owner.invoke(reader,0)+forward<i32>(owner,reader,0)"
        "+owner.relay<string>(other,text,\"value\")+Owner<i32>.invoke_static(reader,0);}\n";
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &symbol_error));
        CHECK(feng_symbol_provider_add_ft_root(provider, profile ? workspace_root : public_root,
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &symbol_error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        analysis = prototype_analyze(consumer, &query, &program);
        FengCodegenOutput output = {0};
        FengCodegenError error = {0};
        bool ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
        if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
        CHECK(ok && output.c_source != NULL);
        const char *needle = "void FengGenericMethod__vendor__guard_proto__Owner__m0__invoke(";
        const char *declaration = strstr(output.c_source, needle);
        CHECK(declaration != NULL && strstr(declaration + 1, needle) == NULL);
        const char *end = strchr(declaration, ';');
        const char *parameter = strstr(declaration, "FengClosure__vendor__guard_proto__Reader__G__T__CTX__T *");
        CHECK(end != NULL && parameter != NULL && parameter < end);
        compile_c(output.c_source);
        feng_codegen_error_free(&error);
        feng_codegen_output_free(&output);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&symbol_error);
}

/* Reference/value owners and both FT profiles keep the provider declaration ABI. */
void test_imported_callable_prototype(void (*compile_c)(const char *)) {
    prototype_case(false, compile_c);
    prototype_case(true, compile_c);
    puts("imported callable prototype matrices passed");
}
