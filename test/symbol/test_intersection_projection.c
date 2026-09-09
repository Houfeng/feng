#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Analyze either a producer or a consumer that can see only persisted FT. */
static FengSemanticAnalysis *projection_analyze(
    const char *source, const FengSemanticImportedModuleQuery *query,
    const char *expected, FengProgram **program) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "projection_ft.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size(),
        .imported_modules = query
    };
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != (expected == NULL) || count != (expected == NULL ? 0U : 1U)) {
        fprintf(stderr, "expected %s\n%s", expected != NULL ? expected : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
    }
    CHECK(ok == (expected == NULL) && count == (expected == NULL ? 0U : 1U));
    if (expected != NULL) CHECK(strcmp(errors[0].code, expected) == 0);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Both FT profiles preserve declared edges after the producer AST is destroyed. */
void test_intersection_projection_ft(void) {
    const char *producer = "open module g24.projection_library;\n"
        "open spec Parent<A> { var item:A; } open spec Child<A>:Parent<A> {}\n"
        "open spec Right { func right():i32; } open spec Extra {}\n"
        "open spec Both<A>:Child<A> & Right; open spec Whole<A>:Both<A> & Extra;\n"
        "open spec Same<A>:Child<A> & Right;\n"
        "open func project<A>(v:Whole<A>):Parent<A> {return (Parent<A>)v;}\n";
    char directory[] = "temp/intersection-projection-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char public_root[256], workspace_root[256];
    snprintf(public_root, sizeof public_root, "%s/public", directory);
    snprintf(workspace_root, sizeof workspace_root, "%s/workspace", directory);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = projection_analyze(producer, NULL, NULL, &program);
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &options, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    const char *bodies[] = {
        "func run(v:Whole<i32>):Parent<i32> {return (Parent<i32>)v;}",
        "func run(v:Whole<i32>):Both<i32> {return (Both<i32>)v;}",
        "func run<A>(v:Whole<A>):Right {return (Right)v;}",
        "func run(v:Whole<i32>):Parent<i32> {return project<i32>(v);}",
        "func run(v:Whole<i32>):Right {return v;}",
        "func take(v:Right) {} func run(v:Whole<i32>) {take(v);}",
        "func run(v:Whole<i32>):Parent<string> {return (Parent<string>)v;}",
        "func run(v:Whole<i32>):Same<i32> {return (Same<i32>)v;}"
    };
    const char *codes[] = {NULL, NULL, NULL, NULL, "AE1003", "AE0512", "AE1023", "AE1023"};
    const size_t depths[] = {3U, 1U, 2U};
    const size_t expected_paths[][3] = {{0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 1U, 0U}};
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, profile ? workspace_root : public_root,
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        for (size_t i = 0U; i < sizeof bodies / sizeof bodies[0]; ++i) {
            char source[1024];
            snprintf(source, sizeof source, "module consumer; import g24.projection_library;\n%s\n", bodies[i]);
            analysis = projection_analyze(source, &query, codes[i], &program);
            if (i < 3U) {
                const FengSpecCoercionSite *site = NULL;
                for (size_t j = 0U; j < analysis->spec_coercion_site_count; ++j) {
                    if (analysis->spec_coercion_sites[j].form == FENG_SPEC_COERCION_FORM_INTERSECTION_UPCAST) {
                        CHECK(site == NULL);
                        site = &analysis->spec_coercion_sites[j];
                    }
                }
                CHECK(site != NULL && site->object_upcast_parent_index_count == depths[i]);
                CHECK(memcmp(site->object_upcast_parent_indices, expected_paths[i], depths[i] * sizeof(size_t)) == 0);
            }
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("intersection explicit projection FT matrices passed");
}
