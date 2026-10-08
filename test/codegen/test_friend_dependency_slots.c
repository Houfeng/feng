#include "../friend_generic_helpers.h"
#include "codegen/codegen.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"

#include <sys/stat.h>
#include <unistd.h>

/* Compare common owner dependencies with and without annotation arguments. */
typedef struct FriendSlotCase {
    const char *source;
    const char *annotation;
    size_t runtime_dep_count;
    size_t annotated_dep_count;
} FriendSlotCase;

/* Apply the real package-public selection to shared-body emission. */
static bool friend_slot_package_contains(const void *user, const void *node) {
    return feng_symbol_package_selection_contains(user, node);
}

/* Locate the declaration whose owner and method dependency domains are tested. */
static const FengDecl *friend_slot_decl(const FriendGenericUnit *unit,
                                       const char *name) {
    for (size_t i = 0U; i < unit->program->declaration_count; ++i) {
        const FengDecl *decl = unit->program->declarations[i];
        FengSlice actual = {0};
        if (decl->kind == FENG_DECL_TYPE) actual = decl->as.type_decl.name;
        else if (decl->kind == FENG_DECL_FUNCTION) actual = decl->as.function_decl.name;
        if (actual.length == strlen(name) &&
            memcmp(actual.data, name, actual.length) == 0) {
            return decl;
        }
    }
    FRIEND_CHECK(false);
    return NULL;
}

/* Emit public shared bodies and closed descriptors without changing visibility. */
static FengCodegenOutput friend_slot_emit(const FriendGenericUnit *unit) {
    FengSymbolGraph *graph = NULL;
    FengSymbolPackageSelection *selection = NULL;
    FengSymbolError symbol_error = {0};
    FRIEND_CHECK(feng_symbol_build_graph(unit->analysis, &graph, &symbol_error));
    FRIEND_CHECK(feng_symbol_build_package_selection(graph, &selection, &symbol_error));
    FengCodegenPackageSymbolQuery query = {
        .user = selection, .contains_source_node = friend_slot_package_contains
    };
    FengCodegenOptions options = {.package_symbols = &query};
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit->analysis, FENG_COMPILE_TARGET_LIB,
                                        &options, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    FRIEND_CHECK(ok && output.c_source != NULL);
    feng_codegen_error_free(&error);
    feng_symbol_package_selection_free(selection);
    feng_symbol_graph_free(graph);
    feng_symbol_error_free(&symbol_error);
    return output;
}

/* Annotation types add static dependencies and reuse existing field entries. */
static void friend_slot_compare(const FriendSlotCase *test,
                                 void (*compile_c)(const char *)) {
    for (size_t annotated = 0U; annotated < 2U; ++annotated) {
        char source[4096];
        int length = snprintf(source, sizeof source, test->source,
                              annotated ? test->annotation : "");
        FRIEND_CHECK(length > 0 && (size_t)length < sizeof source);
        FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
        const FengReifiableDepSet *deps = feng_semantic_lookup_reifiable_dep_set(
            unit.analysis, friend_slot_decl(&unit, "Vault"));
        size_t expected = annotated ? test->annotated_dep_count : test->runtime_dep_count;
        FRIEND_CHECK(deps != NULL && deps->dep_count == expected);
        FengCodegenOutput output = friend_slot_emit(&unit);
        if (annotated && expected > 0U) {
            FRIEND_CHECK(strstr(output.c_source, ".reified_type_deps_count") != NULL ||
                         strstr(output.c_source, ".reified_agg_deps_count") != NULL);
        }
        compile_c(output.c_source);
        feng_codegen_output_free(&output);
        friend_generic_dispose(&unit);
    }
}

/* Unified type slots preserve instantiation checks across both FT profiles. */
static void friend_slot_check_ft(void) {
    FriendGenericUnit provider_unit = friend_generic_analyze(
        "open module friend_slots.provider;"
        "open type Gate<F>{@friend(F) seal let value:int=0;}"
        "open type Vault<T>{@friend(Gate<T>) seal let value:int=0;}",
        NULL, NULL, NULL);
    const FengReifiableDepSet *deps = feng_semantic_lookup_reifiable_dep_set(
        provider_unit.analysis, friend_slot_decl(&provider_unit, "Vault"));
    FRIEND_CHECK(deps != NULL && deps->dep_count == 1U);
    char directory[] = "temp/friend-slots-XXXXXX";
    (void)mkdir("temp", 0777);
    FRIEND_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    FRIEND_CHECK(feng_symbol_export_analysis(provider_unit.analysis, &options, &error));
    friend_generic_dispose(&provider_unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
        FRIEND_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC :
                            FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        FRIEND_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        FriendGenericUnit valid = friend_generic_analyze(
            "module consumer;import friend_slots.provider;"
            "type Local{}func run(v:Vault<Local>){}", &query, NULL, NULL);
        friend_generic_dispose(&valid);
        FriendGenericUnit invalid = friend_generic_analyze(
            "module consumer;import friend_slots.provider;func run(v:Vault<int>){}",
            &query, "AE1336", NULL);
        friend_generic_dispose(&invalid);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
}

/* Annotations and constraints share ordinary static dependencies and deduplication. */
void test_friend_dependency_slots(void (*compile_c)(const char *)) {
    const FriendSlotCase cases[] = {
        {"open module friend_slots;open type Reader<T>{}"
         "open type Vault<T>{%s seal let value:int=0;}"
         "open func run():Vault<int>{return Vault<int>();}",
         "@friend(Reader<T>)", 0U, 1U},
        {"open module friend_slots;open type Reader<T>{}"
         "open type Vault<T>{%s seal let value:int=0;}"
         "open func run():Vault<int>{return Vault<int>();}",
         "@friend(Reader<Reader<T[]>>)", 0U, 3U},
        {"open module friend_slots;@value open type Reader<T>{let value:T;}"
         "open type Vault<T>{%s seal let value:int=0;}"
         "open func run():Vault<int>{return Vault<int>();}",
         "@friend(Reader<T>)", 0U, 1U},
        {"open module friend_slots;open type Reader<T>{let value:T;"
         "func Reader(value:T){self.value=value;}}"
         "open type Vault<T>{%s seal let value:Reader<T>;"
         "func Vault(x:T){self.value=Reader<T>(x);}func read():T{return self.value.value;}}"
         "open func run():int{return Vault<int>(7).read();}",
         "@friend(Reader<T>)", 1U, 1U},
        {"open module friend_slots;open type Reader<T>{}"
         "@value open type Storage<T>{let value:T;func Storage(value:T){self.value=value;}}"
         "open type Vault<T>{%s seal let value:Storage<T>;"
         "func Vault(x:T){self.value=Storage<T>(x);}func read():T{return self.value.value;}}"
         "open func run():int{return Vault<int>(7).read();}",
         "@friend(Reader<T>)", 1U, 2U},
        {"open module friend_slots;open type Reader<T>{let value:T;"
         "func Reader(value:T){self.value=value;}}"
         "open type Vault<T>{%s seal let value:int=0;"
         "func read(x:T):T{return Reader<T>(x).value;}}"
         "open func run():int{return Vault<int>().read(7);}",
         "@friend(Reader<T>)", 0U, 1U}
    };
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        friend_slot_compare(&cases[i], compile_c);
    }
    FriendGenericUnit unit = friend_generic_analyze(
        "open module friend_slots;open type Reader<T>{}open type Vault<T>{"
        "@friend(Reader<Reader<T[]>>) seal func hidden():int{return 1;}"
        "@friend(Reader<T>) seal static func shared():int{return 2;}}"
        "open func run():Vault<int>{return Vault<int>();}", NULL, NULL, NULL);
    const FengReifiableDepSet *method_owner = feng_semantic_lookup_reifiable_dep_set(
        unit.analysis, friend_slot_decl(&unit, "Vault"));
    FRIEND_CHECK(method_owner != NULL && method_owner->dep_count == 4U);
    FengCodegenOutput output = friend_slot_emit(&unit);
    FRIEND_CHECK(strstr(output.c_source, ".reified_type_deps_count") != NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    friend_generic_dispose(&unit);
    unit = friend_generic_analyze(
        "open module friend_slots;open spec Surface<T>{func read():int;}"
        "open type Implementer<T>:Surface<T>{func read():int{return 1;}}"
        "open type Vault<T,F:Surface<T>>{let value:int=1;}"
        "open func use<T,F:Surface<T>>(value:F):int{return value.read();}"
        "open func run():int{let v=Vault<int,Implementer<int>>();"
        "return use<int,Implementer<int>>(Implementer<int>());}", NULL, NULL, NULL);
    const char *names[] = {"Implementer", "Vault", "use"};
    for (size_t i = 0U; i < sizeof names / sizeof *names; ++i) {
        const FengReifiableDepSet *deps = feng_semantic_lookup_reifiable_dep_set(
            unit.analysis, friend_slot_decl(&unit, names[i]));
        FRIEND_CHECK(deps != NULL && deps->dep_count == 1U);
    }
    output = friend_slot_emit(&unit);
    FRIEND_CHECK(strstr(output.c_source, ".reified_agg_deps_count") != NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    friend_generic_dispose(&unit);
    /* A closed annotation type is registered even without an open owner dep. */
    unit = friend_generic_analyze(
        "open module friend_slots;open type Reader<T>{}"
        "open type ClosedVault{@friend(Reader<i32>) seal let value:int=7;}"
        "open func run():ClosedVault{return ClosedVault();}", NULL, NULL, NULL);
    FRIEND_CHECK(feng_semantic_lookup_reifiable_dep_set(unit.analysis,
        friend_slot_decl(&unit, "ClosedVault")) == NULL);
    output = friend_slot_emit(&unit);
    FRIEND_CHECK(strstr(output.c_source, ".name = \"friend_slots.Reader<i32>\"") != NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    friend_generic_dispose(&unit);
    friend_slot_check_ft();
    puts("annotation types share static dependency slots and preserve friend checks");
}
