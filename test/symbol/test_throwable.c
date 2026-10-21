#include "../throw_constraint_helpers.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"
#include <unistd.h>

/* Export one package while preserving imported declaration identities. */
static void throwable_export_package(const char *source,
    FengSymbolImportedModuleCache *cache, const char *public_root, const char *workspace_root) {
    FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
    ThrowConstraintUnit unit = throw_constraint_analyze(source, cache == NULL ? NULL : &query, NULL);
    if (cache != NULL)
        THROW_CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, unit.analysis));
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    THROW_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    feng_symbol_error_free(&error);
    throw_constraint_dispose(&unit);
}

/* A diamond and duplicate aliases identify one root in every dependency order. */
static void throwable_dependency_order(void) {
    char directory[] = "temp/throwable-order-XXXXXX", roots[4][2][256];
    THROW_CHECK(mkdtemp(directory) != NULL);
    for (size_t package = 0U; package < 4U; ++package)
        for (size_t profile = 0U; profile < 2U; ++profile)
            snprintf(roots[package][profile], sizeof roots[package][profile],
                "%s/package-%zu/profile-%zu", directory, package, profile);
    throwable_export_package("open module diamond.base;@throwable open spec Root{}",
        NULL, roots[0][0], roots[0][1]);
    FengSymbolProvider *base = NULL;
    FengSymbolError error = {0};
    THROW_CHECK(feng_symbol_provider_create(&base, &error));
    THROW_CHECK(feng_symbol_provider_add_ft_root(base, roots[0][0],
        FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
    FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(base);
    THROW_CHECK(cache != NULL);
    throwable_export_package("open module diamond.left;import diamond.base as b;"
        "open func left(value:b.Root):b.Root{return value;}", cache, roots[1][0], roots[1][1]);
    throwable_export_package("open module diamond.right;import diamond.base as b;"
        "open func right(value:b.Root):b.Root{return value;}", cache, roots[2][0], roots[2][1]);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(base);
    throwable_export_package("open module diamond.other;@throwable open spec Root{}",
        NULL, roots[3][0], roots[3][1]);
    static const size_t orders[][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProfile kind = profile == 0U
            ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE;
        for (size_t order = 0U; order < 6U; ++order) {
            FengSymbolProvider *provider = NULL;
            THROW_CHECK(feng_symbol_provider_create(&provider, &error));
            for (size_t i = 0U; i < 3U; ++i)
                THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[orders[order][i]][profile], kind, &error));
            THROW_CHECK(feng_symbol_provider_module_count(provider) == 3U);
            cache = feng_symbol_imported_module_cache_create(provider);
            THROW_CHECK(cache != NULL);
            FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
            ThrowConstraintUnit unit = throw_constraint_analyze(
                "module consumer;import diamond.left;import diamond.right;"
                "import diamond.base;import diamond.base as b;"
                "func use(value:b.Root){throw right(left(value));}", &query, NULL);
            THROW_CHECK(unit.analysis->exception_root != NULL);
            THROW_CHECK(unit.analysis->module_count == 4U);
            throw_constraint_dispose(&unit);
            feng_symbol_imported_module_cache_free(cache);
            feng_symbol_provider_free(provider);
        }
        for (size_t reverse = 0U; reverse < 2U; ++reverse) {
            FengSymbolProvider *provider = NULL;
            THROW_CHECK(feng_symbol_provider_create(&provider, &error));
            for (size_t i = 0U; i < 2U; ++i) {
                size_t package = (i == reverse) ? 0U : 3U;
                THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[package][profile], kind, &error));
            }
            cache = feng_symbol_imported_module_cache_create(provider);
            THROW_CHECK(cache != NULL);
            FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
            ThrowConstraintUnit unit = throw_constraint_analyze("module consumer;", &query, "AE1409");
            throw_constraint_dispose(&unit);
            feng_symbol_imported_module_cache_free(cache);
            feng_symbol_provider_free(provider);
        }
    }
    feng_symbol_error_free(&error);
    puts("throwable FT identity: diamond aliases, twelve load orders and four same-name conflicts passed");
}

/* Check both profiles using a source-invisible provider, including discovery
 * without an import and conflicts with a second root in consumer source. */
void test_throwable_ft(void) {
    ThrowConstraintUnit unit = throw_constraint_analyze(
        "open module root.library; @throwable open spec Root {}"
        "open type Error<T>:Root {open let value:T;}", NULL, NULL);
    FengSymbolError error = {0};
    char directory[] = "temp/throwable-ft-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof(roots[0]), "%s/public", directory);
    snprintf(roots[1], sizeof(roots[1]), "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    THROW_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    throw_constraint_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
            &error));
        THROW_CHECK(feng_symbol_provider_module_count(provider) == 1U);
        const FengSymbolImportedModule *module = feng_symbol_provider_module_at(provider, 0U);
        THROW_CHECK(feng_symbol_module_has_exception_root(module));
        const FengSymbolDeclView *root = feng_symbol_module_find_public_spec(
            module, (FengSlice){"Root", 4U});
        THROW_CHECK(root != NULL && feng_symbol_decl_is_throwable(root));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        static const struct {
            const char *source;
            const char *diagnostic;
        } cases[] = {
            {"module consumer; type Item {}", NULL},
            {"module consumer; import root.library;"
             "func fail(value:Error<int[]>) {throw value;}", NULL},
            {"module consumer; import root.library; spec Child:Root {}", "AE1411"},
            {"module consumer; import root.library; spec Other{} spec Both:Other&Root;", "AE1412"},
            {"open module consumer; @throwable open spec Other {}", "AE1409"},
        };
        for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            unit = throw_constraint_analyze(cases[i].source, &query, cases[i].diagnostic);
            if (cases[i].diagnostic == NULL) {
                THROW_CHECK(unit.analysis->exception_root != NULL);
            }
            throw_constraint_dispose(&unit);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    /* Two independent dependencies can conflict without either being imported
     * by name. Public metadata has no source span; the consumer supplies one. */
    unit = throw_constraint_analyze(
        "open module root.other; @throwable open spec Other {}", NULL, NULL);
    THROW_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    throw_constraint_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
            &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        unit = throw_constraint_analyze("module consumer;", &query, "AE1409");
        throw_constraint_dispose(&unit);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    throwable_dependency_order();
}
