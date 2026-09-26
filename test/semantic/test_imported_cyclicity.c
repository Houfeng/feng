#include "../throw_constraint_helpers.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"

#include <unistd.h>

/* Check the same field graph before export and after source-independent FT
 * import, including private edges and acyclic controls. */
static void imported_cycle_expect(const FengSemanticAnalysis *analysis) {
    static const struct { const char *name; bool cyclic; } expected[] = {
        {"Leaf", false}, {"Chain", false}, {"Pointer", false},
        {"Loop", true}, {"Self", true}, {"Left", true}, {"Right", true},
        {"Outer", true}, {"Inner", true}
    };
    for (size_t e = 0; e < sizeof expected / sizeof expected[0]; ++e) {
        const FengDecl *found = NULL;
        for (size_t m = 0; m < analysis->module_count; ++m) {
            const FengSemanticModule *module = &analysis->modules[m];
            for (size_t p = 0; p < module->program_count; ++p) {
                const FengProgram *program = module->programs[p];
                for (size_t d = 0; d < program->declaration_count; ++d) {
                    const FengDecl *decl = program->declarations[d];
                    if (decl->kind != FENG_DECL_TYPE) continue;
                    FengSlice name = decl->as.type_decl.name;
                    if (strlen(expected[e].name) == name.length &&
                        memcmp(name.data, expected[e].name, name.length) == 0)
                        found = decl;
                }
            }
        }
        THROW_CHECK(found != NULL);
        bool cyclic = feng_semantic_type_is_potentially_cyclic(analysis, found);
        if (cyclic != expected[e].cyclic) fprintf(stderr, "cyclicity mismatch: %s\n", expected[e].name);
        THROW_CHECK(cyclic == expected[e].cyclic);
    }
}

/* Real FT serialization must preserve enough type structure to reuse the
 * existing SCC analysis; no new FT flag or source fallback is needed. */
void test_imported_cyclicity(void) {
    const char *source =
        "open module cycle_provider;"
        "open type Leaf{open let n:int;}"
        "open type Chain<T>{open let payload:T;let leaf:Leaf;}"
        "open type Pointer{let raw:Pointer*;}"
        "open type Loop{var peers:Loop[];}"
        "open type Self<T>{open let payload:T;var peers:Self<T>[];}"
        "open type Left<T>{var rights:Right<T>[];}"
        "open type Right<T>{var lefts:Left<T>[][];}"
        "open type Outer{seal var inner:Inner;}"
        "type Inner{var outer:Outer;}";
    ThrowConstraintUnit original = throw_constraint_analyze(source, NULL, NULL);
    imported_cycle_expect(original.analysis);
    char directory[] = "temp/imported-cycle-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    FengSymbolExportOptions options = {.public_root = directory};
    FengSymbolError error = {0};
    THROW_CHECK(feng_symbol_export_analysis(original.analysis, &options, &error));
    throw_constraint_dispose(&original);

    FengSymbolProvider *provider = NULL;
    THROW_CHECK(feng_symbol_provider_create(&provider, &error));
    THROW_CHECK(feng_symbol_provider_add_ft_root(provider, directory,
                    FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
    FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
    THROW_CHECK(cache != NULL);
    FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
    ThrowConstraintUnit consumer = throw_constraint_analyze(
        "module consumer;import cycle_provider;"
        "func visit(a:Left<int>,b:Right<string>,c:Self<Leaf>,d:Outer,e:Loop,"
        "f:Chain<Leaf>,g:Pointer):void{}", &query, NULL);
    imported_cycle_expect(consumer.analysis);
    throw_constraint_dispose(&consumer);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    char path[256];
    THROW_CHECK(snprintf(path, sizeof path, "%s/cycle_provider.ft", directory) > 0);
    THROW_CHECK(unlink(path) == 0 && rmdir(directory) == 0);
    puts("cyclicity: source/FT, generic/private/array edges and acyclic controls verified");
}
