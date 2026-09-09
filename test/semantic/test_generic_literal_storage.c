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

/* Ordinary fields and constructor-bound let fields retain their existing rules. */
static const char *literal_provider =
    "open module literal.provider;\n"
    "open type Store<T>{open let value:T;}\n"
    "open type Bound<T>{open let value:T;func Bound(value:T){self.value=value;}}\n"
    "open type Mutable<T>{open var value:T;func Mutable(value:T){self.value=value;}}\n";

/* Analyze source-local or imported types and reject invalid code in Semantic. */
static FengSemanticAnalysis *literal_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, const char *expected,
    FengProgram **program) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "literal_semantics.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if ((expected == NULL && !ok) || (expected != NULL &&
        (ok || count != 1U || strcmp(errors[0].code, expected) != 0))) {
        fprintf(stderr, "expected %s\n%s\n", expected != NULL ? expected : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %s\n",
            errors[i].code, errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    if (expected == NULL) CHECK(ok && count == 0U);
    else {
        CHECK(!ok && count == 1U && strcmp(errors[0].code, expected) == 0);
        CHECK(strcmp(errors[0].path, "literal_semantics.ff") == 0);
        CHECK(errors[0].token.line > 0U && errors[0].token.column > 0U);
        const char *line = source;
        for (size_t i = 1U; i < errors[0].token.line; ++i) {
            line = strchr(line, '\n');
            CHECK(line != NULL);
            ++line;
        }
        CHECK(errors[0].token.length > 0U);
        CHECK(memcmp(line + errors[0].token.column - 1U, errors[0].token.lexeme,
                     errors[0].token.length) == 0);
    }
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Each negative differs from a legal open store in one semantic property. */
static void literal_matrix(const FengSemanticImportedModuleQuery *query) {
    const char *bodies[] = {
        ("func save<T>(v:T):Store<T>{return Store<T>{value:v};}\n"
         "func replace<T>(old:T,v:T):Mutable<T>{return Mutable<T>(old){value:v};}\n"),
        "func bad<T>(v:T):Store<T>{return Store<T>{value:\"wrong\"};}\n",
        "func bad<T,U>(v:U):Store<T>{return Store<T>{value:v};}\n",
        "func bad<T>(v:T):Bound<T>{return Bound<T>(v){value:v};}\n",
        "func bad<T>(v:T):Store<T>{return Store<T>{value:v,value:v};}\n",
        "func bad<T>(box:Store<T>,v:T){box.value=v;}\n"
    };
    const char *codes[] = {NULL, "AE1003", "AE1003", "AE0102", "AE1005", "AE0104"};
    for (size_t i = 0U; i < sizeof bodies / sizeof *bodies; ++i) {
        char source[1600];
        snprintf(source, sizeof source, "%s%s", query != NULL
            ? "module literal.consumer;import literal.provider;\n" : literal_provider, bodies[i]);
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = literal_analyze(source, query, codes[i], &program);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* Repeat the same positive/negative boundary through both real .ft profiles. */
void test_generic_literal_storage_semantics(void) {
    literal_matrix(NULL);
    CHECK(mkdir("temp", 0777) == 0 || errno == EEXIST);
    char directory[] = "temp/literal-semantics-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = literal_analyze(literal_provider, NULL, NULL, &program);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &options, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    for (size_t i = 0U; i < 2U; ++i) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, roots[i], i == 0U
            ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        literal_matrix(&query);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("generic literal semantic boundaries and persisted package profiles passed");
}
