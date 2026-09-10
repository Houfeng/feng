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

/* Analyze real source; imported cases contain no provider AST or body. */
static FengSemanticAnalysis *g25_codegen_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, FengProgram **program) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "g25_codegen.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
        &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i)
        fprintf(stderr, "G25 Semantic: %u: %s %s\n",
            errors[i].token.line, errors[i].code, errors[i].message);
    CHECK(ok && count == 0U && analysis != NULL);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Positive sources must produce valid C; negative sources keep the existing
 * Codegen pointee diagnostic, never accepting a partial output as success. */
static bool g25_codegen_check(const char *name, FengSemanticAnalysis *analysis,
    const char *expected_code, void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
        NULL, &output, &error);
    bool ok = expected_code == NULL
        ? emitted && output.c_source != NULL
        : !emitted && error.code != NULL && strcmp(error.code, expected_code) == 0;
    if (!ok) fprintf(stderr, "G25 Codegen %s: expected %s, got %s (%s)\n",
        name, expected_code != NULL ? expected_code : "success",
        emitted ? "success" : error.code != NULL ? error.code : "no diagnostic",
        error.message != NULL ? error.message : "");
    if (ok && emitted) compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    return ok;
}

/* One local program and its exact success or Codegen rejection contract. */
typedef struct G25CodegenCase {
    const char *name;
    const char *source;
    const char *code;
} G25CodegenCase;

/* Export ordinary generic values, not pointers constructed from open types.
 * A legal closed pointer can occupy the entire T slot across an FT boundary. */
static const char *g25_codegen_provider =
    "open module vendor.g25pointer;\n"
    "open spec Named {}\n"
    "open spec Extra {}\n"
    "open spec Both: Named & Extra;\n"
    "@abi open type UserType:Named,Extra { open let value:i32; }\n"
    "@abi open spec Callback(value:i32):i32;\n"
    "open type BoundBox<T:Named> { open let value:T; }\n"
    "open func keep<T>(value:T):T { let saved:T = value; return saved; }\n"
    "open func make<T>():T { let value:T; return value; }\n"
    "open spec Choice: i32* | i32;\n"
    "open func constrained<T:Choice>(value:T):T { return value; }\n"
    "open func nominal<T:Named>(value:T):T { return value; }\n"
    "open func intersection<T:Both>(value:T):T { return value; }\n"
    "open func callable<T:Callback>(value:T):T { return value; }\n"
    "open func rank<T>(value:T):i32 { return 1; }\n"
    "open func rank<T,U>(value:T):i32 { return 2; }\n"
    "open func slot<T,U>(value:T):i32 { return 3; }\n"
    "open func slot<T,U>(value:U):i32 { return 4; }\n";

/* Imported declarations retain constraints after provider AST release.
 * Every negative consumer must stop at Semantic with one primary error. */
static void g25_imported_pointer_constraints(const FengSemanticImportedModuleQuery *query) {
    static const struct {
        const char *source;
        const char *code;
        const char *token;
    } cases[] = {
        {"func use(value:api.UserType*) { api.nominal<api.UserType*>(value); }\n", "AE0512", "nominal"},
        {"func use(value:api.UserType*) { api.nominal(value); }\n", "AE0512", "nominal"},
        {"func use(value:api.UserType*) { api.intersection(value); }\n", "AE0512", "intersection"},
        {"func use(value:api.Callback*) { api.callable(value); }\n", "AE0512", "callable"},
        {"func use(value:i32**) { api.constrained(value); }\n", "AE0512", "constrained"},
        {"func use(value:api.BoundBox<api.UserType*>) {}\n", "AE0710", "api"},
        {"spec Mapper(value:api.UserType*):api.UserType*;\n"
         "func use() { let mapper:Mapper = api.nominal<api.UserType*>; }\n", "AE0522", "nominal"}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char source[1024];
        int written = snprintf(source, sizeof(source),
            "module consumer; import vendor.g25pointer as api;\n%s", cases[i].source);
        CHECK(written > 0 && (size_t)written < sizeof(source));
        FengProgram *program = NULL;
        FengParseError parse = {0};
        CHECK(feng_parse_source(source, strlen(source), "g25_imported_pointer_constraint.ff", &program, &parse));
        const FengProgram *programs[] = {program};
        FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
            .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        CHECK(!feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count));
        CHECK(count == 1U && strcmp(errors[0].code, cases[i].code) == 0);
        CHECK(errors[0].token.length == strlen(cases[i].token) &&
            memcmp(errors[0].token.lexeme, cases[i].token, strlen(cases[i].token)) == 0);
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* Both public and workspace FT profiles must preserve compile-time facts
 * after the provider program and semantic analysis have been released. */
static size_t g25_codegen_imported(void (*compile_c)(const char *)) {
    CHECK(mkdir("temp", 0755) == 0 || errno == EEXIST);
    char directory[] = "temp/g25-codegen-ft-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char public_root[256], workspace_root[256];
    CHECK(snprintf(public_root, sizeof(public_root), "%s/public", directory) > 0);
    CHECK(snprintf(workspace_root, sizeof(workspace_root), "%s/workspace", directory) > 0);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = g25_codegen_analyze(g25_codegen_provider, NULL, &program);
    size_t failures = !g25_codegen_check("provider", analysis, NULL, compile_c);
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &options, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    static const G25CodegenCase cases[] = {
        {"imported-positive",
            "module consumer; import vendor.g25pointer as api;\n"
            "func relay<U>(value:U):U { return api.keep(value); }\n"
            "func use(value:i32*,data:i32[]*):i32* {\n"
            "api.rank<i32>(1); api.rank<i32,string>(1);\n"
            "api.slot<i32,string>(1); api.slot<i32,string>(\"four\");\n"
            "api.keep<i32[]*>(data); api.make<i32*>();\n"
            "api.constrained<i32*>(value); api.constrained(value);\n"
            "return relay<i32*>(api.keep<i32*>(value)); }\n", NULL},
        {"imported-invalid-return",
            "module consumer; import vendor.g25pointer as api;\n"
            "type Bad { let text:string; }\n"
            "func use() { api.make<Bad*>(); }\n", "CE0033"}
    };
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, profile ? workspace_root : public_root,
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        g25_imported_pointer_constraints(&query);
        for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            analysis = g25_codegen_analyze(cases[i].source, &query, &program);
            CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
            failures += !g25_codegen_check(cases[i].name, analysis, cases[i].code, compile_c);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    return failures;
}

/* Closed pointers remain complete generic values through storage, forwarding,
 * nested inference and imported calls. Open pointer rejection is Semantic. */
void test_g25_codegen(void (*compile_c)(const char *)) {
    static const G25CodegenCase cases[] = {
        {"pointer-storage",
            "module g25; func id<U>(value:U):U { return value; }\n"
            "func keep<T>(value:T):T { var saved:T = value;\n"
            "saved = id<T>(saved); return saved; }\n"
            "func use(value:i32*):i32* { return keep(keep<i32*>(value)); }\n", NULL},
        {"pointer-layers",
            "module g25; func keep<T>(value:T):T { return value; }\n"
            "func use(value:i32***,values:i32[]*):i32*** { keep(values); return keep(value); }\n", NULL},
        {"pointer-array",
            "module g25; func keep<T>(value:T):T { let values:T[] = [value];\n"
            "return values[0]; }\n"
            "func use(value:i32*):i32* { return keep(value); }\n", NULL},
        {"pointer-owner-and-method",
            "module g25; type Holder<T> { var value:T;\n"
            "func read():T { return self.value; }\n"
            "static func relay<U>(value:U):U { return value; } }\n"
            "fit Holder<T> { func fitRead():T { return self.value; } }\n"
            "func use(value:i32*):i32* { let box=Holder<i32*>{value:value};\n"
            "box.read(); box.fitRead(); return Holder<i32*>.relay<i32*>(value); }\n", NULL},
        {"pointer-union-constraint",
            "module g25; spec Choice:i32* | i32;\n"
            "func keep<T:Choice>(value:T):T { return value; }\n"
            "func use(value:i32*):i32* { return keep(keep<i32*>(value)); }\n", NULL},
        {"arity-and-slot-identity",
            "module g25; func rank<T>(value:T):i32 { return 1; }\n"
            "func rank<T,U>(value:T):i32 { return 2; }\n"
            "func slot<T,U>(value:T):i32 { return 3; }\n"
            "func slot<T,U>(value:U):i32 { return 4; }\n"
            "func use():i32 { return rank<i32>(1)+rank<i32,string>(1)+\n"
            "slot<i32,string>(1)+slot<i32,string>(\"four\"); }\n", NULL},
        {"closed-invalid-pointee",
            "module g25; type Bad { let text:string; }\n"
            "func use() { let pointer:Bad*; }\n", "CE0033"},
        {"generic-invalid-closed-argument",
            "module g25; type Bad { let text:string; }\n"
            "func make<T>():T { let value:T; return value; }\n"
            "func use() { make<Bad*>(); }\n", "CE0033"},
        {"generic-pointer-zero",
            "module g25; func make<T>():T { let value:T; return value; }\n"
            "func use():i32* { return make<i32*>(); }\n", NULL}
    };
    size_t failures = 0U;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = g25_codegen_analyze(cases[i].source, NULL, &program);
        failures += !g25_codegen_check(cases[i].name, analysis, cases[i].code, compile_c);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    failures += g25_codegen_imported(compile_c);
    CHECK(failures == 0U);
    puts("G25 pointer and generic callable identity Codegen matrices passed");
}
