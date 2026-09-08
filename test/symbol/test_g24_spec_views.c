#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/ft_internal.h"
#include "symbol/imported_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/** Analyze real source, optionally resolving its imports solely through persisted FT. */
static FengSemanticAnalysis *analyze(const char *source, const FengSemanticImportedModuleQuery *query, FengProgram **program) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "g24_views.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/** Wire reads are independent of native endian/alignment and internal record casts. */
static uint64_t read_le(const unsigned char *bytes, size_t width) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; ++i) value |= (uint64_t)bytes[i] << (i * 8U);
    return value;
}
/** Mutate only one selected field of an otherwise genuine writer output. */
static void write_le(unsigned char *bytes, size_t width, uint64_t value) {
    for (size_t i = 0U; i < width; ++i) bytes[i] = (unsigned char)(value >> (i * 8U));
}
/** Discover actual section offsets rather than relying on writer placement. */
static size_t section_at(const unsigned char *bytes, uint16_t kind) {
    size_t offset = (size_t)read_le(bytes + 0x18, 8U), count = (size_t)read_le(bytes + 0x0C, 2U);
    for (size_t i = 0U; i < count; ++i) if (read_le(bytes + offset + i * 32U, 2U) == kind) return offset + i * 32U;
    CHECK(false);
    return 0U;
}
/** Recompute a valid payload checksum so corruption reaches structural validation. */
static void rehash(unsigned char *bytes, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = (size_t)read_le(bytes + 0x20, 8U); i < length; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    write_le(bytes + 0x28, 8U, hash);
}
/** Read independently emitted binary data for both profiles. */
static unsigned char *read_bytes(const char *file, size_t *length) {
    FILE *f = fopen(file, "rb");
    CHECK(f != NULL && fseek(f, 0L, SEEK_END) == 0);
    long end = ftell(f);
    CHECK(end >= 64L && fseek(f, 0L, SEEK_SET) == 0);
    *length = (size_t)end;
    unsigned char *bytes = malloc(*length);
    CHECK(bytes != NULL && fread(bytes, 1U, *length, f) == *length && fclose(f) == 0);
    return bytes;
}
/** A restored graph must preserve complete open source/target identities and owners. */
static void check_pairs(const FengSymbolGraph *graph) {
    const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, 0U);
    const FengSymbolDeclView *scan = NULL;
    CHECK(module != NULL);
    for (size_t i = 0U; i < module->root_decl.member_count; ++i)
        if (strcmp(module->root_decl.members[i]->name, "scan") == 0) scan = module->root_decl.members[i];
    CHECK(scan != NULL && scan->reifiable_spec_view_coercion_count == 3U);
    CHECK(scan->reifiable_agg_dep_count == 5U && scan->reifiable_type_dep_count == 1U);
    const char *sources[] = {"Ref", "Value", "Value"};
    const char *parameters[] = {"A", "Z", "A"};
    const char *targets[] = {"Both", "Both", "Field"};
    for (size_t i = 0U; i < 3U; ++i) {
        const FengSymbolSpecViewCoercionView *view = &scan->reifiable_spec_view_coercions[i];
        const FengSymbolTypeView *refs[] = {view->source_type, view->target_type};
        for (size_t j = 0U; j < 2U; ++j) {
            CHECK(refs[j] != NULL && refs[j]->kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
            CHECK(refs[j]->as.named_generic.type_arg_count == 1U);
            const FengSymbolTypeView *arg = refs[j]->as.named_generic.type_args[0];
            CHECK(arg->kind == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF && arg->target_decl != NULL && arg->target_decl->owner == scan);
            CHECK(strcmp(arg->as.type_param_ref.name, parameters[i]) == 0);
            size_t last = refs[j]->as.named_generic.segment_count - 1U;
            CHECK(strcmp(refs[j]->as.named_generic.segments[last], j == 0U ? sources[i] : targets[i]) == 0);
        }
    }
}
/** Required shape, count, owner, ordering and types fail in the reader, not in Codegen. */
static void corruption_matrix(const unsigned char *original, size_t length) {
    size_t section = section_at(original, FENG_SYMBOL_FT_SEC_SPEC_VIEW_COERCIONS);
    size_t records = (size_t)read_le(original + section + 8U, 8U);
    size_t attrs_section = section_at(original, FENG_SYMBOL_FT_SEC_ATTRS);
    size_t attrs = (size_t)read_le(original + attrs_section + 8U, 8U);
    size_t attr_count = (size_t)read_le(original + attrs_section + 4U, 4U), count_attr = SIZE_MAX;
    for (size_t i = 0U; i < attr_count; ++i)
        if (read_le(original + attrs + i * 20U + 4U, 2U) == 14U && read_le(original + attrs + i * 20U + 8U, 4U) == 3U) count_attr = attrs + i * 20U;
    CHECK(count_attr != SIZE_MAX);
    unsigned char *bytes = malloc(length);
    CHECK(bytes != NULL);
    for (size_t variant = 0U; variant < 18U; ++variant) {
        memcpy(bytes, original, length);
        size_t supplied_length = length;
        switch (variant) {
            case 0U: write_le(bytes + section, 2U, 0xFFU); break;
            case 1U: write_le(bytes + section + 2U, 2U, 0U); break;
            case 2U: write_le(bytes + section + 0x18, 4U, 12U); break;
            case 3U: write_le(bytes + section + 0x10, 8U, 1U); break;
            case 4U: write_le(bytes + section + 0x1C, 4U, 1U); break;
            case 5U: write_le(bytes + records, 4U, 0U); break;
            case 6U: write_le(bytes + records + 4U, 4U, 1U); break;
            case 7U: write_le(bytes + records + 8U, 4U, 0U); break;
            case 8U: write_le(bytes + records + 12U, 4U, 0U); break;
            case 9U: write_le(bytes + records + 8U, 4U, UINT32_MAX); break;
            case 10U: write_le(bytes + records + 12U, 4U, UINT32_MAX); break;
            case 11U: write_le(bytes + records + 12U, 4U, read_le(bytes + records + 8U, 4U)); break;
            case 12U: write_le(bytes + count_attr + 8U, 4U, 0U); break;
            case 13U: write_le(bytes + count_attr + 8U, 4U, 2U); break;
            case 14U: write_le(bytes + count_attr + 12U, 4U, 1U); break;
            case 15U: write_le(bytes + count_attr + 4U, 2U, 0xFFU); break;
            case 16U: write_le(bytes + section + 8U, 8U, read_le(bytes + section + 8U, 8U) + 1U); break;
            case 17U: supplied_length = length - 1U; break;
        }
        rehash(bytes, length);
        FengSymbolGraph *graph = NULL;
        FengSymbolError error = {0};
        bool accepted = feng_symbol_ft_read_bytes(bytes, supplied_length, "g24_view_bad.ft", NULL, &graph, &error);
        if (accepted) fprintf(stderr, "accepted spec-view corruption %zu\n", variant);
        CHECK(!accepted && graph == NULL && error.message != NULL);
        feng_symbol_error_free(&error);
    }
    free(bytes);
}

/** Both profiles and real producer destruction must preserve the same canonical slots. */
void test_g24_spec_view_ft(void) {
    const char *prefix = "open module g24.views;\n"
        "open spec Field<A> { var item: A; } open spec Tag { func tag(): i32; } open spec Both<A>: Field<A> & Tag;\n"
        "@value open type Value<A>: Field<A>, Tag { open var item: A; func tag(): i32 { return 1; } }\n"
        "open type Ref<A>: Field<A>, Tag { open var item: A; func tag(): i32 { return 2; } }\n"
        "@value type Hidden<A>: Field<A>, Tag { open var item: A; func tag(): i32 { return 3; } seal func unused() {} }\n";
    const char *bodies[] = {
        "let copy: g24.views.Value<A> = a; let x: Both<Z> = z; let y: Field<A> = copy; let r: Both<A> = ref; let again: Both<Z> = z;",
        "let r: Both<A> = ref; let again: Both<Z> = z; let copy: g24.views.Value<A> = a; let y: Field<A> = copy; let x: Both<Z> = z;"
    };
    char directory[] = "temp/g24-spec-views-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    for (size_t order = 0U; order < 2U; ++order) {
        char source[2048], file[256], root[256];
        snprintf(source, sizeof(source), "%sopen func scan<Z,A>(z: Value<Z>, a: Value<A>, ref: Ref<A>) { %s }\n"
            "open func factory<A>(item:A):Both<A> { return Hidden<A>{item:item}; }\n", prefix, bodies[order]);
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = analyze(source, NULL, &program);
        for (size_t i = 0U; i < analysis->reifiable_dep_set_count; ++i) {
            const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
            if (set->spec_view_coercion_count == 3U) CHECK(set->dep_count == 6U);
        }
        FengSymbolGraph *graph = NULL;
        FengSymbolError error = {0};
        CHECK(feng_symbol_build_graph(analysis, &graph, &error));
        check_pairs(graph);
        snprintf(file, sizeof(file), "%s/views.ft", directory);
        for (size_t profile = 1U; profile <= 2U; ++profile) {
            CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U), (FengSymbolProfile)profile, file, &error));
            size_t length = 0U;
            unsigned char *bytes = read_bytes(file, &length);
            CHECK(bytes[5] == 2U && bytes[6] == 0U);
            FengSymbolGraph *loaded = NULL;
            CHECK(feng_symbol_ft_read_bytes(bytes, length, file, NULL, &loaded, &error));
            check_pairs(loaded);
            if (profile == FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC) {
                const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(loaded, 0U);
                const FengSymbolDeclView *hidden = NULL;
                for (size_t i = 0U; i < module->root_decl.member_count; ++i)
                    if (strcmp(module->root_decl.members[i]->name, "Hidden") == 0) hidden = module->root_decl.members[i];
                CHECK(hidden != NULL && hidden->visibility == FENG_VISIBILITY_PRIVATE);
                bool has_implementation = false;
                for (size_t i = 0U; i < hidden->member_count; ++i) {
                    CHECK(strcmp(hidden->members[i]->name, "unused") != 0);
                    if (strcmp(hidden->members[i]->name, "tag") == 0) has_implementation = true;
                }
                CHECK(has_implementation);
            }
            feng_symbol_graph_free(loaded);
            corruption_matrix(bytes, length);
            /* The canonical pair slots, not unrelated traversal-ordered type
             * IDs or source-body locations, are the cross-order contract. */
            free(bytes);
        }
        snprintf(root, sizeof(root), "%s/mod", directory);
        FengSymbolExportOptions options = {.public_root = root};
        CHECK(feng_symbol_export_analysis(analysis, &options, &error));
        feng_symbol_graph_free(graph);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, root, FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        analysis = analyze("module consumer; import g24.views; func run() { scan<i32,string>(Value<i32>{item:17}, Value<string>{item:\"text\"}, Ref<string>{item:\"ref\"}); }", &query, &program);
        CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
        size_t found = 0U;
        for (size_t i = 0U; i < analysis->reifiable_dep_set_count; ++i) {
            const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
            if (set->spec_view_coercion_count != 3U) continue;
            ++found;
            CHECK(set->dep_count == 6U);
            for (size_t j = 0U; j < 3U; ++j) CHECK(feng_semantic_spec_view_coercion_slot(set, &set->spec_view_coercions[j]) == j);
        }
        CHECK(found == 1U);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        const char *negative = "module consumer;\nimport g24.views;\nfunc bad(value: Hidden<i32>) {}\n";
        FengParseError parse = {0};
        CHECK(feng_parse_source(negative, strlen(negative), "g24_private.ff", &program, &parse));
        const FengProgram *programs[] = {program};
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        analysis = NULL;
        FengSemanticAnalyzeOptions negative_options = {.target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size(), .imported_modules = &query};
        CHECK(!feng_semantic_analyze_with_options(programs, 1U, &negative_options, &analysis, &errors, &count));
        if (count != 1U || strcmp(errors[0].code, "AE1013") != 0)
            for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
        CHECK(count == 1U && strcmp(errors[0].code, "AE1013") == 0);
        CHECK(strcmp(errors[0].path, "g24_private.ff") == 0 && errors[0].token.line == 3U && errors[0].token.column == 17U);
        CHECK(errors[0].token.length == 6U && memcmp(errors[0].token.lexeme, "Hidden", 6U) == 0);
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
        feng_symbol_error_free(&error);
    }
    puts("G24 spec view FT matrices passed");
}
