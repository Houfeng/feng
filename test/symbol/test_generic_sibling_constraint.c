#include "../generic_sibling_constraint_helpers.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/imported_module.h"
#include "symbol/internal.h"
#include <unistd.h>

/* Find an exact child without assuming writer or reader member ordering. */
static const FengSymbolDeclView *sibling_symbol(const FengSymbolDeclView *owner, const char *name) {
    for (size_t i = 0U; i < owner->member_count; ++i)
        if (strcmp(owner->members[i]->name, name) == 0) return owner->members[i];
    SIBLING_CHECK(false);
    return NULL;
}

/* The bound's argument must retain the sibling's declaration identity. */
static void sibling_symbol_bound(const FengSymbolDeclView *owner,
    const char *bounded_name, const char *argument_name) {
    SIBLING_CHECK(owner->type_param_count == 2U);
    const FengSymbolDeclView *bounded = sibling_symbol(owner, bounded_name);
    const FengSymbolDeclView *argument = sibling_symbol(owner, argument_name);
    SIBLING_CHECK(bounded->kind == FENG_SYMBOL_DECL_KIND_TYPE_PARAM && bounded->owner == owner);
    SIBLING_CHECK(argument->kind == FENG_SYMBOL_DECL_KIND_TYPE_PARAM && argument->owner == owner);
    SIBLING_CHECK(feng_symbol_decl_constraint_kind(bounded) == FENG_CONSTRAINT_SPEC);
    SIBLING_CHECK(feng_symbol_decl_constraint_kind(argument) == FENG_CONSTRAINT_NONE);
    const FengSymbolTypeView *bound = bounded->value_type;
    SIBLING_CHECK(bound != NULL && bound->kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    SIBLING_CHECK(bound->as.named_generic.type_arg_count == 1U);
    const FengSymbolTypeView *ref = bound->as.named_generic.type_args[0];
    SIBLING_CHECK(ref->kind == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    SIBLING_CHECK(strcmp(ref->as.type_param_ref.name, argument_name) == 0);
    SIBLING_CHECK(ref->target_decl == argument);
}

/* Inspect real export graphs before and after both FT serialization profiles. */
static void sibling_symbol_graph(const FengSymbolGraph *graph) {
    SIBLING_CHECK(feng_symbol_graph_module_count(graph) == 1U);
    const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, 0U);
    const char *first[] = {"first", "FirstOwner", "FirstBound"};
    const char *later[] = {"later", "LaterOwner", "LaterBound"};
    for (size_t i = 0U; i < sizeof first / sizeof *first; ++i) {
        sibling_symbol_bound(sibling_symbol(&module->root_decl, first[i]), "U", "T");
        sibling_symbol_bound(sibling_symbol(&module->root_decl, later[i]), "T", "U");
    }
    sibling_symbol_bound(sibling_symbol(&module->root_decl, "relayLater"), "Y", "X");
    const FengSymbolDeclView *methods = sibling_symbol(&module->root_decl, "Methods");
    sibling_symbol_bound(sibling_symbol(methods, "first"), "U", "T");
    sibling_symbol_bound(sibling_symbol(methods, "later"), "T", "U");
}

/* Consumers validate restored bounds after all provider ASTs are destroyed. */
void test_generic_sibling_constraint_ft(void) {
    SiblingConstraintUnit unit = sibling_analyze(sibling_declarations(), NULL, NULL, NULL);
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    SIBLING_CHECK(feng_symbol_build_graph(unit.analysis, &graph, &error));
    sibling_symbol_graph(graph);
    char directory[] = "temp/sibling-ft-XXXXXX";
    SIBLING_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256], file[256];
    SIBLING_CHECK(snprintf(roots[0], sizeof roots[0], "%s/public", directory) > 0);
    SIBLING_CHECK(snprintf(roots[1], sizeof roots[1], "%s/workspace", directory) > 0);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    SIBLING_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    for (size_t profile = 1U; profile <= 2U; ++profile) {
        SIBLING_CHECK(snprintf(file, sizeof file, "%s/roundtrip%zu.ft", directory, profile) > 0);
        SIBLING_CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U),
            (FengSymbolProfile)profile, file, &error));
        FengSymbolGraph *loaded = NULL;
        SIBLING_CHECK(feng_symbol_ft_read_file(file, NULL, &loaded, &error));
        sibling_symbol_graph(loaded);
        feng_symbol_graph_free(loaded);
    }
    feng_symbol_graph_free(graph);
    sibling_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        SIBLING_CHECK(feng_symbol_provider_create(&provider, &error));
        SIBLING_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        SIBLING_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        /* Each negative changes one actual while keeping the provider fixed. */
        const struct {
            const char *body;
            const char *code;
            const char *token;
        } cases[] = {
            {"func run(x:Ref<i32>){first<i32,Ref<i32>>(x);later<Ref<i32>,i32>(x);inferFirst(x,(i32)1);inferLater(x,(i32)2);}", NULL, NULL},
            {"func run(x:Ref<string>){let a:string=first(x);let b:string=later(x);}", NULL, NULL},
            {"func run<Y:Child<X>,X>(x:Y):X{return relayFirst<X,Y>(x);}", NULL, NULL},
            {"func run<X,Y:Child<X>>(x:Y):X{return relayLater<Y,X>(x);}", NULL, NULL},
            {"type U{}func run<T,U:Box<T>>(x:U):T{return x.get();}", NULL, NULL},
            {"type T{}func run<T:Box<U>,U>(x:T):U{return x.get();}", NULL, NULL},
            {"func run<Nominal>():i32{return nominal().marker();}", NULL, NULL},
            {"func run(x:Ref<i32>){let a=FirstOwner<i32,Ref<i32>>{item:x};let b=LaterOwner<Ref<i32>,i32>{item:x};a.get();b.get();}", NULL, NULL},
            {"func run(x:Ref<i32>){let m=Methods<string>();m.first(x,(i32)1);Methods<string>.later(x,(i32)2);m.fitLater(x,(i32)3);Methods<string>.fitFirst(x,(i32)4);}", NULL, NULL},
            {"type Payload{}func run<T:Box<Payload>,Payload>(x:T):Payload{return later<T,Payload>(x);}", NULL, NULL},
            {"func bad(x:Ref<i32>){first<string,Ref<i32>>(x);}", "AE0512", "first"},
            {"func bad(x:Ref<i32>){later<Ref<i32>,string>(x);}", "AE0512", "later"},
            {"func bad<T,U>(x:U):T{return first<T,U>(x);}", "AE0512", "first"},
            {"func bad<T,U>(x:T):U{return later<T,U>(x);}", "AE0512", "later"},
            {"func bad(x:FirstOwner<string,Ref<i32>>){}", "AE0710", "Ref"},
            {"func bad(x:LaterOwner<Ref<i32>,string>){}", "AE0710", "Ref"}
        };
        for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
            char *source = sibling_source("module consumer;import sibling.bounds;\n", cases[i].body);
            unit = sibling_analyze(source, &query, cases[i].code, cases[i].token);
            sibling_dispose(&unit);
            free(source);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("generic sibling constraint FT profiles and consumers passed");
}
