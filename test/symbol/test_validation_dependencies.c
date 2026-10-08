#include "../friend_generic_helpers.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"

#include <sys/stat.h>
#include <unistd.h>

/* One method's exact targets, with a bit per expected complete owner binding. */
typedef struct ValidationAccessCheck {
    const FengDecl *owner;
    const FengTypeMember *member;
    bool generic;
    unsigned int seen;
} ValidationAccessCheck;

/* Match the complete shape of a named reference in this fixed source fixture. */
static bool validation_named_ref(const FengTypeRef *ref, const char *name, size_t arity) {
    if (ref == NULL || ref->kind != FENG_TYPE_REF_NAMED ||
        ref->as.named.segment_count == 0U || ref->as.named.type_arg_count != arity) return false;
    FengSlice leaf = ref->as.named.segments[ref->as.named.segment_count - 1U];
    return leaf.length == strlen(name) && memcmp(leaf.data, name, leaf.length) == 0;
}

/* Locate the original/imported declaration by its fixture module and name. */
static const FengDecl *validation_type(const FengSemanticAnalysis *analysis, const char *name) {
    for (size_t m = 0U; m < analysis->module_count; ++m) {
        const FengSemanticModule *module = &analysis->modules[m];
        if (module->segment_count != 2U || module->segments[0].length != 6U ||
            memcmp(module->segments[0].data, "checks", 6U) != 0 ||
            module->segments[1].length != 10U ||
            memcmp(module->segments[1].data, "separation", 10U) != 0) continue;
        for (size_t p = 0U; p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0U; d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind == FENG_DECL_TYPE && decl->as.type_decl.name.length == strlen(name) &&
                    memcmp(decl->as.type_decl.name.data, name, strlen(name)) == 0) return decl;
            }
        }
    }
    FRIEND_CHECK(false);
    return NULL;
}

/* Verify member identity and distinguish T from Box<T>, not merely their arity. */
static bool check_selected_access(void *user, const FengDecl *owner,
    const FengTypeMember *member, const FengTypeRef *instance) {
    ValidationAccessCheck *check = user;
    FRIEND_CHECK(owner == check->owner && member == check->member);
    FRIEND_CHECK(validation_named_ref(instance, check->generic ? "AccessVault" : "Vault", 1U));
    const FengTypeRef *argument = instance->as.named.type_args[0];
    unsigned int bit = 1U;
    if (!check->generic) {
        FRIEND_CHECK(validation_named_ref(argument, "Reader", 0U));
    } else if (validation_named_ref(argument, "Box", 1U)) {
        FRIEND_CHECK(validation_named_ref(argument->as.named.type_args[0], "T", 0U));
        bit = 2U;
    } else {
        FRIEND_CHECK(validation_named_ref(argument, "T", 0U));
    }
    FRIEND_CHECK((check->seen & bit) == 0U);
    check->seen |= bit;
    return true;
}

/* Declaration types use ordinary deps; accesses deduplicate by caller and
 * complete instance, surviving provider AST disposal and repeated restoration. */
static void validation_check_ownership(const FengSemanticAnalysis *analysis) {
    size_t declarations = 0U, methods = 0U, accesses = 0U;
    unsigned int method_mask = 0U;
    const FengDecl *vault = validation_type(analysis, "Vault");
    const FengDecl *access_vault = validation_type(analysis, "AccessVault");
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
                    const FengTypeMember *method = decl->as.fit_decl.members[a];
                    FengSlice name = method->as.callable.name;
                    bool first = name.length == 5U && memcmp(name.data, "first", 5U) == 0;
                    bool second = name.length == 6U && memcmp(name.data, "second", 6U) == 0;
                    bool third = name.length == 5U && memcmp(name.data, "third", 5U) == 0;
                    ValidationAccessCheck check = {.owner = third ? access_vault : vault,
                        .generic = third};
                    check.member = check.owner->as.type_decl.members[0];
                    FRIEND_CHECK(feng_semantic_visit_friend_fit_accesses(analysis, decl,
                        method, check_selected_access, &check));
                    if (first || second || third) {
                        FRIEND_CHECK(validation_named_ref(decl->as.fit_decl.target, "Reader", 0U));
                        FRIEND_CHECK(check.seen == (third ? 3U : 1U));
                        unsigned int bit = first ? 1U : second ? 2U : 4U;
                        FRIEND_CHECK((method_mask & bit) == 0U);
                        method_mask |= bit;
                        ++methods;
                        accesses += third ? 2U : 1U;
                    } else {
                        FRIEND_CHECK(check.seen == 0U);
                    }
                }
            }
        }
    }
    FRIEND_CHECK(declarations == 2U && methods == 3U && accesses == 4U && method_mask == 7U);
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
