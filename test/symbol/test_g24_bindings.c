#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/imported_module.h"
#include "symbol/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/** Analyze independent source against an optional binary module query. */
static FengSemanticAnalysis *analyze(const char *source, const FengSemanticImportedModuleQuery *query,
    FengProgram **program, const char *expected, const char *marker, const char *token) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "g24_consumer.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != (expected == NULL) || count != (expected == NULL ? 0U : 1U)) {
        fprintf(stderr, "%s\n", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %s\n", errors[i].code,
            errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    CHECK(ok == (expected == NULL) && count == (expected == NULL ? 0U : 1U));
    if (expected != NULL) {
        const char *at = strstr(source, marker);
        unsigned line = 1U, column = 1U;
        CHECK(at != NULL);
        for (const char *p = source; p < at; ++p) { if (*p == '\n') { ++line; column = 1U; } else ++column; }
        if (strcmp(errors[0].code, expected) != 0 || errors[0].token.line != line || errors[0].token.column != column)
            fprintf(stderr, "FT expected %s %u:%u; got %s %u:%u\n%s\n", expected, line, column,
                errors[0].code, errors[0].token.line, errors[0].token.column, source);
        CHECK(strcmp(errors[0].code, expected) == 0);
        CHECK(strcmp(errors[0].path, "g24_consumer.ff") == 0);
        CHECK(errors[0].token.line == line && errors[0].token.column == column);
        CHECK(errors[0].token.length == strlen(token) && memcmp(errors[0].token.lexeme, token, strlen(token)) == 0);
    }
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/** Verify open subject/result references survive producer AST destruction. */
void test_g24_binding_ft(void) {
    const char *producer_format =
        "open module g24.ftbinding;\n"
        "open spec Choice<X>: X | bool;\n"
        "open spec Left { func get(): i32; }\n"
        "open spec Right { func tag(): i32; }\n"
        "open spec Both: Left & Right;\n"
        "open type Complete: Left, Right { func get(): i32 { return 17; } func tag(): i32 { return 29; } }\n"
        "open type Partial: Left { func get(): i32 { return 31; } }\n"
        "open spec Hidden { seal func secret(): i32; }\n"
        "open spec Empty {}\nopen spec HiddenBoth: Hidden & Empty;\n"
        "open func relay<T: Choice<i32>>(value: T): T { return value; }\n"
        "open func constrain<T: Both>(value: T): T { return value; }\n"
        "open func read<X>(value: Choice<X>, fallback: X): X { return match value { item: X { item } else { fallback } }; }\n"
        "open spec Normalized: Choice<string> | g24.ftbinding.Choice<string> | int | %s | Choice<i32> | Choice<i32>;\n";
    char producer[4096];
    const char *platform_int = feng_get_host_pointer_size() == 8U ? "i64" : "i32";
    CHECK(snprintf(producer, sizeof producer, producer_format, platform_int) < (int)sizeof producer);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = analyze(producer, NULL, &program, NULL, NULL, NULL);
    const FengUnionSpecInfo *normalized = feng_semantic_lookup_union_spec_info(
        analysis, program->declarations[program->declaration_count - 1U]);
    CHECK(normalized != NULL && normalized->member_count == 3U);
    CHECK(normalized->members[0].is_nested_union && normalized->members[2].is_nested_union);
    CHECK(!normalized->members[1].is_nested_union);
    char directory[] = "temp/g24_binding_ft_XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    FengSymbolExportOptions export = {.public_root = directory};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &export, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    FengSymbolProvider *provider = NULL;
    CHECK(feng_symbol_provider_create(&provider, &error));
    CHECK(feng_symbol_provider_add_ft_root(provider, directory, FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
    /* COMPOSITE08: normalized identity/order is preserved after the producer
     * AST is destroyed; closed arguments remain part of member identity. */
    FengSlice segments[] = {{"g24", 3U}, {"ftbinding", 9U}};
    const FengSymbolImportedModule *module = feng_symbol_provider_find_module(provider, segments, 2U);
    CHECK(module != NULL);
    const FengSymbolDeclView *normalized_decl = feng_symbol_module_find_public_spec(
        module, (FengSlice){"Normalized", 10U});
    CHECK(normalized_decl != NULL && feng_symbol_decl_union_member_count(normalized_decl) == 3U);
    const char *member_types[] = {"string", platform_int, "i32"};
    for (size_t index = 0U; index < 3U; ++index) {
        const FengSymbolTypeView *member = feng_symbol_decl_union_member_at(normalized_decl, index);
        CHECK(feng_symbol_type_generic_arg_count(member) == (index == 1U ? 0U : 1U));
        const FengSymbolTypeView *leaf = index == 1U ? member : feng_symbol_type_generic_arg_at(member, 0U);
        CHECK(feng_symbol_type_kind(leaf) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
        FengSlice name = feng_symbol_type_builtin_name(leaf);
        CHECK(name.length == strlen(member_types[index]) &&
              memcmp(name.data, member_types[index], name.length) == 0);
    }
    FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
    CHECK(cache != NULL);
    FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
    const char *valid =
        "module consumer;\nimport g24.ftbinding;\n"
        "func run(value: Complete): Complete { return constrain(value); }\n"
        "func scalar(value: Choice<i32>): i32 { return read<i32>(value, -1); }\n"
        "func text(value: Choice<string>): string { return read<string>(value, \"fallback\"); }\n"
        "func merged(value: Choice<bool>): bool { return read<bool>(value, false); }\n";
    analysis = analyze(valid, &query, &program, NULL, NULL, NULL);
    CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
    bool found = false;
    for (size_t i = 0U; i < analysis->reifiable_dep_set_count; ++i) {
        const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
        for (size_t j = 0U; j < set->union_projection_count; ++j) {
            const FengUnionProjectionDep *dep = &set->union_projections[j];
            if (dep->subject_type_ref->kind == FENG_TYPE_REF_NAMED &&
                dep->subject_type_ref->as.named.type_arg_count == 1U) {
                CHECK(dep->result_type_ref != NULL && dep->path_count == 1U);
                CHECK(dep->result_type_ref->as.named.type_arg_count == 0U);
                CHECK(feng_semantic_union_projection_slot(set, dep) == j);
                found = true;
            }
        }
    }
    CHECK(found);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    const char *invalid[] = {
        "func bad(value: string) { relay(value); }\n",
        "func bad(value: Partial) { constrain(value); }\n",
        "func bad(value: Choice<i32>) { match value { string {} } }\n",
        "func bad(value: HiddenBoth): i32 { return value.secret(); }\n"
    };
    const char *codes[] = {"AE0512", "AE0512", "AE0605", "AE0708"};
    const char *markers[] = {"relay(value)", "constrain(value)", "string {}", "secret()"};
    const char *tokens[] = {"relay", "constrain", "string", "secret"};
    for (size_t i = 0U; i < 4U; ++i) {
        char source[512];
        snprintf(source, sizeof(source), "module consumer;\nimport g24.ftbinding;\n%s", invalid[i]);
        analysis = analyze(source, &query, &program, codes[i], markers[i], tokens[i]);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    puts("G24 binding FT and consumer diagnostics passed");
}
