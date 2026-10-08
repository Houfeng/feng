#include "../friend_generic_helpers.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"

#include <sys/stat.h>
#include <unistd.h>

/* Inspect complete target bindings through the same records used by export. */
static bool count_selected_access(void *user, const FengDecl *owner,
    const FengTypeMember *member, const FengTypeRef *instance) {
    size_t *count = user;
    FRIEND_CHECK(owner != NULL && member != NULL && instance != NULL);
    FRIEND_CHECK(instance->kind == FENG_TYPE_REF_NAMED &&
                 instance->as.named.type_arg_count == 1U);
    ++*count;
    return true;
}

/* Declaration types use ordinary deps; accesses deduplicate by caller and
 * complete instance, surviving provider AST disposal and repeated restoration. */
static void validation_check_ownership(const FengSemanticAnalysis *analysis) {
    size_t declarations = 0U, methods = 0U, accesses = 0U;
    for (size_t i = 0U; i < analysis->reifiable_dep_set_count; ++i) {
        const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
        if (set->owner_member == NULL && set->dep_count > 0U &&
            (set->owner_decl->kind == FENG_DECL_FIT || set->owner_decl->kind == FENG_DECL_SPEC)) {
            FRIEND_CHECK(set->dep_count == 1U);
            ++declarations;
        }
    }
    for (size_t m = 0U; m < analysis->module_count; ++m) {
        const FengSemanticModule *module = &analysis->modules[m];
        for (size_t p = 0U; p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0U; d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind != FENG_DECL_FIT) continue;
                for (size_t a = 0U; a < decl->as.fit_decl.member_count; ++a) {
                    size_t count = 0U;
                    FRIEND_CHECK(feng_semantic_visit_friend_fit_accesses(analysis, decl,
                        decl->as.fit_decl.members[a], count_selected_access, &count));
                    if (count > 0U) { ++methods; accesses += count; }
                }
            }
        }
    }
    FRIEND_CHECK(declarations == 2U && methods == 3U && accesses == 4U);
}

/* Both FT profiles preserve unified dependency owners after source disposal, while
 * generic call propagation and repeated metadata restoration remain valid. */
void test_validation_dependencies_ft(void) {
    const char *source = "open module checks.separation;"
        "open type Reader{}"
        "open type Box<T>{}"
        "open type AccessVault<T>{@friend(Reader) seal let value:T;}"
        "open type Vault<F>{@friend(F) seal let value:int=0;}"
        "open type Target<T>{}"
        "open fit Target<T>{@friend(Vault<T>) seal func guarded(){}}"
        "open spec Base<F>{@friend(F) seal func hidden():void;}"
        "open spec Derived<T>:Base<T>{}"
        "open fit Reader{"
        "func first(v:Vault<Reader>):int{return v.value+v.value;}"
        "func second(v:Vault<Reader>):int{return v.value;}"
        "func third<T>(a:AccessVault<T>,b:AccessVault<Box<T>>){"
        "let x=a.value;let y=b.value;let z=a.value;}}"
        "open func consume<T>(v:Target<T>){}"
        "open func forward<T>(v:Target<T>){consume<T>(v);}";
    FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
    validation_check_ownership(unit.analysis);
    char directory[] = "temp/validation-deps-XXXXXX";
    (void)mkdir("temp", 0777);
    FRIEND_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolError error = {0};
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FRIEND_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    friend_generic_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
        FRIEND_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        FRIEND_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        unit = friend_generic_analyze(
            "module consumer;import checks.separation;"
            "func use(v:Target<Reader>,s:Derived<Reader>){forward<Reader>(v);}",
            &query, NULL, NULL);
        validation_check_ownership(unit.analysis);
        size_t runtime_count = unit.analysis->reifiable_dep_set_count;
        FRIEND_CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, unit.analysis));
        FRIEND_CHECK(unit.analysis->reifiable_dep_set_count == runtime_count);
        validation_check_ownership(unit.analysis);
        friend_generic_dispose(&unit);
        unit = friend_generic_analyze(
            "module consumer;import checks.separation;func use(v:Target<int>){}",
            &query, "AE1336", NULL);
        friend_generic_dispose(&unit);
        unit = friend_generic_analyze(
            "module consumer;import checks.separation;func use(v:Derived<int>){}",
            &query, "AE1336", NULL);
        friend_generic_dispose(&unit);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
        char path[320];
        snprintf(path, sizeof path, "%s/checks/separation.ft", roots[profile]);
        FRIEND_CHECK(unlink(path) == 0);
        snprintf(path, sizeof path, "%s/checks", roots[profile]);
        FRIEND_CHECK(rmdir(path) == 0 && rmdir(roots[profile]) == 0);
    }
    FRIEND_CHECK(rmdir(directory) == 0);
    feng_symbol_error_free(&error);
    puts("unified dependencies and bound fit accesses survive source-free FT restoration");
}
