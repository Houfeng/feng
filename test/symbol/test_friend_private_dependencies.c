#include "../friend_generic_helpers.h"
#include "archive/zip.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"
#include <sys/stat.h>
#include <unistd.h>

/* Export a complete source package, then destroy every source/semantic object. */
static void private_ft_export(const char *const *sources, size_t count,
    const FengSemanticImportedModuleQuery *query, const char *public_root, const char *workspace_root) {
    FengProgram **programs = calloc(count, sizeof(*programs));
    FRIEND_CHECK(programs != NULL);
    for (size_t i = 0U; i < count; ++i) programs[i] = friend_generic_parse(sources[i]);
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze_with_options((const FengProgram *const *)programs, count,
        &options, &analysis, &errors, &error_count);
    for (size_t i = 0U; i < error_count; ++i) fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
    FRIEND_CHECK(ok && error_count == 0U);
    FengSymbolError error = {0};
    FengSymbolExportOptions output = {.public_root = public_root, .workspace_root = workspace_root};
    ok = feng_symbol_export_analysis(analysis, &output, &error);
    if (!ok) fprintf(stderr, "private dependency export: %s\n", error.message);
    FRIEND_CHECK(ok);
    feng_symbol_error_free(&error);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    for (size_t i = 0U; i < count; ++i) feng_program_free(programs[i]);
    free(programs);
}

/* Resolve a test module through the ordinary provider inventory. */
static const FengSymbolImportedModule *private_ft_module(FengSymbolProvider *provider, const char *name) {
    char buffer[256];
    FRIEND_CHECK(strlen(name) < sizeof buffer);
    strcpy(buffer, name);
    FengSlice segments[16];
    size_t count = 0U;
    char *start = buffer;
    for (char *cursor = buffer;; ++cursor) {
        if (*cursor != '.' && *cursor != '\0') continue;
        FRIEND_CHECK(count < sizeof segments / sizeof *segments);
        segments[count++] = (FengSlice){.data = start, .length = (size_t)(cursor - start)};
        if (*cursor == '\0') break;
        start = cursor + 1;
    }
    return feng_symbol_provider_find_module(provider, segments, count);
}

/* Inspect stored identities independently of ordinary source visibility. */
static const FengSymbolDeclView *private_ft_decl(const FengSymbolImportedModule *module, const char *name) {
    for (size_t i = 0U; i < feng_symbol_module_decl_count(module); ++i) {
        const FengSymbolDeclView *decl = feng_symbol_module_decl_at(module, i);
        FengSlice spelling = feng_symbol_decl_name(decl);
        if (spelling.length == strlen(name) && memcmp(spelling.data, name, spelling.length) == 0) return decl;
    }
    return NULL;
}

/* Find the imported AST owner used by the public read-only Semantic query. */
static const FengDecl *private_ft_semantic_type(const FengSemanticAnalysis *analysis, const char *name) {
    for (size_t m = 0U; m < analysis->module_count; ++m) {
        const FengSemanticModule *module = &analysis->modules[m];
        for (size_t p = 0U; p < module->program_count; ++p) {
            const FengProgram *program = module->programs[p];
            for (size_t d = 0U; d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind == FENG_DECL_TYPE && decl->as.type_decl.name.length == strlen(name) &&
                    memcmp(decl->as.type_decl.name.data, name, strlen(name)) == 0) return decl;
            }
        }
    }
    return NULL;
}

/* Build a source-free bundle using only the selected module tables. */
static void private_ft_bundle(const char *path, const char *root, const char *const *modules, size_t count) {
    FengZipWriter writer = {0};
    char *error = NULL;
    FRIEND_CHECK(feng_zip_writer_open(path, &writer, &error));
    for (size_t i = 0U; i < count; ++i) {
        char input[768], entry[256];
        FRIEND_CHECK(snprintf(input, sizeof input, "%s/%s.ft", root, modules[i]) > 0);
        FRIEND_CHECK(snprintf(entry, sizeof entry, "mod/%s.ft", modules[i]) > 0);
        FRIEND_CHECK(feng_zip_writer_add_file(&writer, entry, input, FENG_ZIP_COMPRESSION_DEFLATE, &error));
    }
    FRIEND_CHECK(feng_zip_writer_finalize(&writer, &error));
    feng_zip_writer_dispose(&writer);
    feng_zip_free(error);
}

/* Compare two imported packages and preserve the original friend/fit contexts. */
static void private_ft_consumers(FengSymbolProvider *provider) {
    const FengSymbolImportedModule *api = private_ft_module(provider, "privateft.api");
    const FengSymbolImportedModule *hidden = private_ft_module(provider, "privateft.hidden");
    const FengSymbolImportedModule *bridge = private_ft_module(provider, "bridge.api");
    FRIEND_CHECK(api != NULL && hidden != NULL && bridge != NULL);
    FRIEND_CHECK(feng_symbol_module_visibility(hidden) == FENG_VISIBILITY_PRIVATE);
    FRIEND_CHECK(feng_symbol_module_package_identity(api) == feng_symbol_module_package_identity(hidden));
    FRIEND_CHECK(feng_symbol_module_package_identity(api) != feng_symbol_module_package_identity(bridge));
    const FengSymbolDeclView *payload = private_ft_decl(hidden, "Payload");
    const FengSymbolDeclView *leaf = private_ft_decl(hidden, "Leaf");
    FRIEND_CHECK(payload != NULL && leaf != NULL && private_ft_decl(hidden, "Bound") != NULL);
    FRIEND_CHECK(feng_symbol_decl_visibility(payload) == FENG_VISIBILITY_PUBLIC);
    FRIEND_CHECK(feng_symbol_decl_visibility(leaf) == FENG_VISIBILITY_PRIVATE);
    FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
    FRIEND_CHECK(cache != NULL);
    FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
    const struct { const char *source; const char *code; } cases[] = {
        {"func accept(v:Vault<privateft.api.Reader>){}", NULL},
        {"func accept(v:Vault<privateft.readers.Reader>){}", NULL},
        {"func accept(v:Vault<privateft.api.Reader>){forward<privateft.api.Reader>(v);relay<privateft.api.Reader>(v);}", NULL},
        {"type Local{}func accept(v:Vault<Local>){}", "AE1338"},
        {"func accept(v:Vault<bridge.api.Reader>){}", "AE1338"},
        {"func accept(v:Carrier<bridge.api.Reader>){}", "AE1338"},
        {"func relayBad<F>(v:Vault<F>){relay<F>(v);}func run(){let x=Vault<bridge.api.Reader>();relayBad<bridge.api.Reader>(x);}", "AE1338"},
        {"fit privateft.api.Reader{func peek(v:Vault<privateft.api.Reader>){let p=v.payload;}}", "AE1338"},
        {"fit privateft.api.Reader{func peek(v:Vault<privateft.api.Reader>){let p=v.result();}}", "AE1338"},
        {"fit privateft.api.Reader{static func peek(v:Vault<privateft.api.Reader>){let p=v.payload;}}", "AE1338"},
        {"func accept(v:Vault<privateft.api.Reader>){let p=v.payload;}", "AE0308"},
        {"import privateft.hidden;", "AE0902"},
        {"import privateft.hidden as h;", "AE0902"},
        {"func accept(x:privateft.hidden.Payload<int>){}", "AE1013"},
        {"func accept(x:privateft.hidden.Leaf){}", "AE1013"}
    };
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        char source[2048];
        snprintf(source, sizeof source, "module consumer;import privateft.api;import bridge.api;%s", cases[i].source);
        FriendGenericUnit unit = friend_generic_analyze(source, &query, cases[i].code, NULL);
        friend_generic_dispose(&unit);
    }
    FriendGenericUnit completion = friend_generic_analyze(
        "module consumer;import privateft.api;fit Reader{func candidates(v:Vault<Reader>){}}",
        &query, NULL, NULL);
    const FengDecl *fit = completion.program->declarations[0];
    const FengTypeMember *candidate = fit->as.fit_decl.members[0];
    const FengTypeRef *instance = candidate->as.callable.params[0].type;
    const FengDecl *owner = private_ft_semantic_type(completion.analysis, "Vault");
    FRIEND_CHECK(owner != NULL);
    size_t checked = 0U;
    for (size_t i = 0U; i < owner->as.type_decl.member_count; ++i) {
        const FengTypeMember *member = owner->as.type_decl.members[i];
        if (!feng_semantic_member_has_friend_declaration(completion.analysis, member)) continue;
        FengSlice name = member->kind == FENG_TYPE_MEMBER_FIELD
            ? member->as.field.name : member->as.callable.name;
        bool expected = name.length == 7U && memcmp(name.data, "visible", 7U) == 0;
        FRIEND_CHECK(feng_semantic_member_has_friend_access(completion.analysis,
            completion.program, owner, instance, member, fit, candidate) == expected);
        ++checked;
    }
    FRIEND_CHECK(checked == 5U);
    friend_generic_dispose(&completion);
    feng_symbol_imported_module_cache_free(cache);
}

/* Exercise minimal private closure selection, both profiles, and bundle provenance. */
void test_friend_private_dependencies(void) {
    const char *sources[] = {
        "seal module privateft.hidden;"
        "seal type Leaf{}open type Payload<T>{seal let leaf:Leaf;seal let next:Payload<T>[];seal let link:Payload<int>*;}"
        "open spec Bound{func value():int;}open type Unused{}seal type UnusedPrivate{}open func unused(){}",
        "open module privateft.api;import privateft.hidden;"
        "open type Reader{}"
        "open type Vault<F>{@friend(F) seal let payload:Payload<int>;"
        "@friend(F) seal let nested:Payload<int>[];"
        "@friend(F) seal func result():Payload<int>{return self.payload;}"
        "@friend(F) seal func constrained<T:Bound>(x:T){}"
        "@friend(F) seal func visible():int{return 1;}}"
        "open type Carrier<F>{seal let vault:Vault<F>;}"
        "open func forward<F>(v:Vault<F>){}"
        "open fit Reader{func inspect(v:Vault<Reader>){let p=v.payload;}}",
        "open module privateft.readers;open type Reader{}",
        "seal module privateft.unused;open type UnusedModule{}"
    };
    char directory[] = "temp/friend-private-ft-XXXXXX";
    (void)mkdir("temp", 0777);
    FRIEND_CHECK(mkdtemp(directory) != NULL);
    char roots[2][512], bridge_roots[2][512];
    for (size_t i = 0U; i < 2U; ++i) {
        snprintf(roots[i], sizeof roots[i], "%s/provider-%zu", directory, i);
        snprintf(bridge_roots[i], sizeof bridge_roots[i], "%s/bridge-%zu", directory, i);
    }
    private_ft_export(sources, sizeof sources / sizeof *sources, NULL, roots[0], roots[1]);
    FengSymbolError error = {0};
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProfile kind = profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE;
        FengSymbolProvider *provider = NULL;
        FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
        FRIEND_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile], kind, &error));
        const FengSymbolImportedModule *hidden = private_ft_module(provider, "privateft.hidden");
        FRIEND_CHECK(hidden != NULL);
        FRIEND_CHECK((private_ft_decl(hidden, "Unused") == NULL) == (profile == 0U));
        FRIEND_CHECK((private_ft_decl(hidden, "unused") == NULL) == (profile == 0U));
        FRIEND_CHECK((private_ft_module(provider, "privateft.unused") == NULL) == (profile == 0U));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        if (profile == 0U) {
            const char *bridge_sources[] = {"open module bridge.api;import privateft.api;open type Reader{}"
                "open func relay<F>(v:Vault<F>){forward<F>(v);}"};
            private_ft_export(bridge_sources, 1U, &query, bridge_roots[0], bridge_roots[1]);
        }
        feng_symbol_imported_module_cache_free(cache);
        FRIEND_CHECK(feng_symbol_provider_add_ft_root(provider, bridge_roots[profile], kind, &error));
        private_ft_consumers(provider);
        feng_symbol_provider_free(provider);
    }
    const char *provider_modules[] = {"privateft/api", "privateft/hidden", "privateft/readers"};
    const char *bridge_modules[] = {"bridge/api"};
    char bundle[512], bridge_bundle[512];
    snprintf(bundle, sizeof bundle, "%s/provider.fb", directory);
    snprintf(bridge_bundle, sizeof bridge_bundle, "%s/bridge.fb", directory);
    private_ft_bundle(bundle, roots[0], provider_modules, 3U);
    private_ft_bundle(bridge_bundle, bridge_roots[0], bridge_modules, 1U);
    FengSymbolProvider *provider = NULL;
    FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
    FRIEND_CHECK(feng_symbol_provider_add_bundle(provider, bundle, &error));
    FRIEND_CHECK(feng_symbol_provider_add_bundle(provider, bridge_bundle, &error));
    private_ft_consumers(provider);
    feng_symbol_provider_free(provider);

    /* A malformed package missing its required module must not erase obligations. */
    private_ft_bundle(bundle, roots[0], provider_modules, 1U);
    FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
    FRIEND_CHECK(feng_symbol_provider_add_bundle(provider, bundle, &error));
    FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
    FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
    FriendGenericUnit unit = friend_generic_analyze_at(
        "module consumer;import privateft.api;func accept(v:Vault<Reader>){}", &query,
        "AE1338", NULL, 0U, 0U, "cannot be resolved from package metadata", 0U, 0U);
    friend_generic_dispose(&unit);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    puts("friend private dependency closure, profiles, bundles and visibility passed");
}
