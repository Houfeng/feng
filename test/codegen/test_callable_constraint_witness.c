#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <unistd.h>

/* Ordinary constraints deliberately contain no exception-root declarations. */
static const char callable_bounds_provider[] =
    "open module callable.bounds;"
    "open spec Root{}open fit i32:Root{}open fit string:Root{}"
    "open spec Related<T>{}open fit i32:Related<string>{}"
    "open spec Action<T>(value:T):void;"
    "open func accept<T:Root>(value:T){}"
    "open func sibling<A,B:Related<A>>(value:B){}"
    "open type Host<O>{"
    "open func send<T:Root>(value:T){}"
    "open static func emit<T:Root>(value:T){}"
    "open func related<T:Related<O>>(value:T){}}"
    "open type Extended<O>{}"
    "open fit Extended<O>{"
    "open func send<T:Root>(value:T){}"
    "open static func emit<T:Root>(value:T){}"
    "open func related<T:Related<O>>(value:T){}}";

/* Each source has only one use, so unrelated calls cannot prepare its witness. */
static const char *const callable_bounds_cases[] = {
    "func run(){accept<i32>(1);}",
    "func run(){let f:Action<i32> = accept<i32>;f(1);}",
    "func run(){let h=Host<string>();let f:Action<string> = h.send<string>;f(\"x\");}",
    "func run(){let f:Action<i32> = Host<string>.emit<i32>;f(1);}",
    "func run(){let h=Extended<string>();let f:Action<string> = h.send<string>;f(\"x\");}",
    "func run(){let f:Action<i32> = Extended<string>.emit<i32>;f(1);}",
    "func run(){let h=Host<string>();let f:Action<i32> = h.related<i32>;f(1);}",
    "func run(){let h=Extended<string>();let f:Action<i32> = h.related<i32>;f(1);}",
    "func run(){let f:Action<i32> = sibling<string,i32>;f(1);}",
};

/* Missing Semantic witnesses must fail this test before C compilation. */
static void callable_bounds_emit(ThrowConstraintUnit *unit,
                                  void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit->analysis, FENG_COMPILE_TARGET_LIB,
                                         NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
}

/* Local source and both source-invisible FT profiles use identical clients. */
void test_callable_constraint_witness_codegen(void (*compile_c)(const char *)) {
    size_t capacity = sizeof(callable_bounds_provider) + 512U;
    char *source = malloc(capacity);
    THROW_CHECK(source != NULL);
    for (size_t i = 0U; i < sizeof(callable_bounds_cases) / sizeof(callable_bounds_cases[0]); ++i) {
        snprintf(source, capacity, "%s%s", callable_bounds_provider, callable_bounds_cases[i]);
        ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
        callable_bounds_emit(&unit, compile_c);
        throw_constraint_dispose(&unit);
    }

    ThrowConstraintUnit producer = throw_constraint_analyze(callable_bounds_provider, NULL, NULL);
    char directory[] = "temp/callable-bounds-ft-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof(roots[0]), "%s/public", directory);
    snprintf(roots[1], sizeof(roots[1]), "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    THROW_CHECK(feng_symbol_export_analysis(producer.analysis, &options, &error));
    throw_constraint_dispose(&producer);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
            &error));
        for (size_t i = 0U; i < sizeof(callable_bounds_cases) / sizeof(callable_bounds_cases[0]); ++i) {
            FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
            THROW_CHECK(cache != NULL);
            FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
            snprintf(source, capacity, "module consumer;import callable.bounds;%s", callable_bounds_cases[i]);
            ThrowConstraintUnit unit = throw_constraint_analyze(source, &query, NULL);
            THROW_CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, unit.analysis));
            callable_bounds_emit(&unit, compile_c);
            throw_constraint_dispose(&unit);
            feng_symbol_imported_module_cache_free(cache);
        }
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    free(source);
    puts("callable constraint witnesses: isolated source and FT cases passed");
}
