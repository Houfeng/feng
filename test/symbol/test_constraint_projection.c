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

/* Producer and consumer never share ASTs or declaration addresses. */
static FengSemanticAnalysis *projection_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, bool positive, FengProgram **program) {
    FengParseError parse = {0};
    CHECK(feng_parse_source(source, strlen(source), "projection_ft.ff", program, &parse));
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != positive) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok == positive);
    if (positive) CHECK(count == 0U);
    else {
        CHECK(count == 1U && strcmp(errors[0].code, "AE0512") == 0 && errors[0].token.line == 3U);
        const char *body = strchr(strchr(source, '\n') + 1, '\n') + 1;
        const char *call = strstr(body, "return ");
        CHECK(call != NULL);
        call += strlen("return ");
        const char *end = strchr(call, '<');
        CHECK(end != NULL && strcmp(errors[0].path, "projection_ft.ff") == 0);
        CHECK(errors[0].token.column == (size_t)(call - body) + 1U);
        CHECK(errors[0].token.length == (size_t)(end - call));
        CHECK(memcmp(errors[0].token.lexeme, call, (size_t)(end - call)) == 0);
    }
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Decode actual file bytes independently of C record layout. */
static uint64_t projection_read(const unsigned char *bytes, size_t width) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; ++i) value |= (uint64_t)bytes[i] << (i * 8U);
    return value;
}
/* Change one field while preserving all other genuine writer output. */
static void projection_write(unsigned char *bytes, size_t width, uint64_t value) {
    for (size_t i = 0U; i < width; ++i) bytes[i] = (unsigned char)(value >> (i * 8U));
}
/* Locate sections through the wire header, not assumed placement. */
static size_t projection_section(const unsigned char *bytes, uint16_t kind) {
    size_t offset = (size_t)projection_read(bytes + 0x18, 8U);
    size_t count = (size_t)projection_read(bytes + 0x0C, 2U);
    for (size_t i = 0U; i < count; ++i)
        if (projection_read(bytes + offset + i * 32U, 2U) == kind) return offset + i * 32U;
    CHECK(false);
    return 0U;
}
/* A valid checksum ensures each corruption reaches structural validation. */
static void projection_rehash(unsigned char *bytes, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = (size_t)projection_read(bytes + 0x20, 8U); i < length; ++i) {
        hash ^= bytes[i]; hash *= UINT64_C(1099511628211);
    }
    projection_write(bytes + 0x28, 8U, hash);
}
/* Load both public and workspace files from the production writer. */
static unsigned char *projection_bytes(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    long end = ftell(file);
    CHECK(end >= 64L && fseek(file, 0L, SEEK_SET) == 0);
    *length = (size_t)end;
    unsigned char *bytes = malloc(*length);
    CHECK(bytes != NULL && fread(bytes, 1U, *length, file) == *length && fclose(file) == 0);
    return bytes;
}
/* Slots are declaration-bound: Z precedes A even though lexical sorting differs. */
static void projection_check_graph(const FengSymbolGraph *graph) {
    const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, 0U);
    const FengSymbolDeclView *scan = NULL;
    CHECK(module != NULL);
    for (size_t i = 0U; i < module->root_decl.member_count; ++i)
        if (strcmp(module->root_decl.members[i]->name, "scan") == 0) scan = module->root_decl.members[i];
    CHECK(scan != NULL && scan->reifiable_constraint_projection_count == 4U);
    const char *sources[] = {"Z", "Z", "Z", "A"};
    const char *targets[] = {"Left", "Other", "Right", "Right"};
    for (size_t i = 0U; i < 4U; ++i) {
        const FengSymbolConstraintProjectionView *dep = &scan->reifiable_constraint_projections[i];
        CHECK(dep->source_type->kind == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
        CHECK(strcmp(dep->source_type->as.type_param_ref.name, sources[i]) == 0);
        CHECK(dep->source_type->target_decl != NULL && dep->source_type->target_decl->owner == scan);
        const FengSymbolTypeView *target = dep->target_type;
        if (i == 0U) {
            CHECK(target->kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
            CHECK(strcmp(target->as.named_generic.segments[target->as.named_generic.segment_count - 1U], targets[i]) == 0);
            CHECK(target->as.named_generic.type_arg_count == 1U);
            CHECK(strcmp(target->as.named_generic.type_args[0]->as.builtin.name, "i32") == 0);
        } else {
            CHECK(target->kind == FENG_SYMBOL_TYPE_KIND_NAMED);
            CHECK(strcmp(target->as.named.segments[target->as.named.segment_count - 1U], targets[i]) == 0);
        }
    }
}
/* Missing sections, corrupt types, owners, counts and duplicate slots are rejected. */
static void projection_corruption(const unsigned char *original, size_t length) {
    size_t section = projection_section(original, FENG_SYMBOL_FT_SEC_CONSTRAINT_PROJECTIONS);
    size_t records = (size_t)projection_read(original + section + 8U, 8U);
    size_t attrs_section = projection_section(original, FENG_SYMBOL_FT_SEC_ATTRS);
    size_t attrs = (size_t)projection_read(original + attrs_section + 8U, 8U);
    size_t attr_count = (size_t)projection_read(original + attrs_section + 4U, 4U);
    size_t count_attr = SIZE_MAX, scan_record = SIZE_MAX;
    for (size_t i = 0U; i < attr_count; ++i)
        if (projection_read(original + attrs + i * 20U + 4U, 2U) == 15U &&
            projection_read(original + attrs + i * 20U + 8U, 4U) == 4U) count_attr = attrs + i * 20U;
    CHECK(count_attr != SIZE_MAX);
    uint64_t owner = projection_read(original + count_attr, 4U);
    size_t record_count = (size_t)projection_read(original + section + 4U, 4U);
    for (size_t i = 0U; i < record_count; ++i)
        if (projection_read(original + records + i * 16U, 4U) == owner &&
            projection_read(original + records + i * 16U + 4U, 4U) == 0U) scan_record = records + i * 16U;
    CHECK(scan_record != SIZE_MAX);
    unsigned char *bytes = malloc(length);
    CHECK(bytes != NULL);
    for (size_t variant = 0U; variant < 21U; ++variant) {
        memcpy(bytes, original, length);
        size_t supplied_length = length;
        switch (variant) {
            case 0U: projection_write(bytes + section, 2U, 0xFFU); break;
            case 1U: projection_write(bytes + section + 2U, 2U, 0U); break;
            case 2U: projection_write(bytes + section + 0x18, 4U, 12U); break;
            case 3U: projection_write(bytes + section + 0x10, 8U, 1U); break;
            case 4U: projection_write(bytes + section + 0x1C, 4U, 1U); break;
            case 5U: projection_write(bytes + scan_record, 4U, 0U); break;
            case 6U: projection_write(bytes + scan_record + 4U, 4U, 1U); break;
            case 7U: projection_write(bytes + scan_record + 8U, 4U, 0U); break;
            case 8U: projection_write(bytes + scan_record + 12U, 4U, 0U); break;
            case 9U: projection_write(bytes + scan_record + 8U, 4U, UINT32_MAX); break;
            case 10U: projection_write(bytes + scan_record + 12U, 4U, UINT32_MAX); break;
            case 11U: projection_write(bytes + scan_record + 12U, 4U, projection_read(bytes + scan_record + 8U, 4U)); break;
            case 12U: projection_write(bytes + count_attr + 8U, 4U, 0U); break;
            case 13U: projection_write(bytes + count_attr + 8U, 4U, 3U); break;
            case 14U: projection_write(bytes + count_attr + 12U, 4U, 1U); break;
            case 15U: projection_write(bytes + count_attr + 4U, 2U, 0xFFU); break;
            case 16U: projection_write(bytes + section + 8U, 8U, records + 1U); break;
            case 17U: supplied_length = length - 1U; break;
            case 18U: projection_write(bytes + scan_record + 16U + 4U, 4U, 0U); break;
            case 19U: projection_write(bytes + scan_record + 4U, 4U, UINT32_MAX); break;
            case 20U: projection_write(bytes + scan_record, 4U, UINT32_MAX); break;
        }
        projection_rehash(bytes, length);
        FengSymbolGraph *graph = NULL;
        FengSymbolError error = {0};
        bool ok = feng_symbol_ft_read_bytes(bytes, supplied_length, "bad_projection.ft", NULL, &graph, &error);
        if (ok) fprintf(stderr, "accepted constraint projection corruption %zu\n", variant);
        CHECK(!ok && graph == NULL && error.message != NULL);
        feng_symbol_error_free(&error);
    }
    free(bytes);
}

/* Both real profiles round-trip identical slots, fit parameters and negative rules. */
void test_constraint_projection_ft(void) {
    const char *prefix = "open module projection.ft;\n"
        "open spec Left<A>{func echo(value:A):A;} open spec Right{func right():i32;}\n"
        "open spec Extra{func extra():i32;} open spec Both<A>:Left<A>&Right;\n"
        "open spec All<A>:Both<A>&Extra; open spec Other:Extra&Right;\n"
        "open func left<A,T:Left<A>>(value:T,item:A):A{return value.echo(item);}\n"
        "open func right<T:Right>(value:T):i32{return value.right();}\n"
        "open func other<T:Other>(value:T):i32{return value.extra()+value.right();}\n"
        "open func zero<T>():T{let value:T;return value;}\n"
        "open type Owner<T:Both<i32>>{open let item:T;open let ready=right<T>(zero<T>());}\n"
        "open fit Owner<T>{func projected():i32{return right<T>(self.item);}}\n";
    const char *bodies[] = {
        "let r=right<A>(a);let l=left<i32,Z>(z,1);let s=other<Z>(z);return right(z)+r+l+s;",
        "let s=other<Z>(z);let l=left<i32,Z>(z,1);let r=right(a);return right<Z>(z)+r+l+s;"
    };
    char directory[] = "temp/constraint-ft-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    for (size_t order = 0U; order < 2U; ++order) {
        char source[2048], file[256], roots[2][256];
        snprintf(source, sizeof source, "%sopen func scan<Z:All<i32>,A:All<string>>(z:Z,a:A):i32{%s}\n", prefix, bodies[order]);
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = projection_analyze(source, NULL, true, &program);
        FengSymbolGraph *graph = NULL;
        FengSymbolError error = {0};
        CHECK(feng_symbol_build_graph(analysis, &graph, &error));
        projection_check_graph(graph);
        snprintf(roots[0], sizeof roots[0], "%s/public%zu", directory, order);
        snprintf(roots[1], sizeof roots[1], "%s/workspace%zu", directory, order);
        FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
        CHECK(feng_symbol_export_analysis(analysis, &options, &error));
        for (size_t profile = 1U; profile <= 2U; ++profile) {
            snprintf(file, sizeof file, "%s/projection.ft", directory);
            CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U), (FengSymbolProfile)profile, file, &error));
            size_t length = 0U;
            unsigned char *bytes = projection_bytes(file, &length);
            CHECK(bytes[5] == 2U && bytes[6] == 0U);
            FengSymbolGraph *loaded = NULL;
            CHECK(feng_symbol_ft_read_bytes(bytes, length, file, NULL, &loaded, &error));
            projection_check_graph(loaded);
            feng_symbol_graph_free(loaded);
            projection_corruption(bytes, length);
            free(bytes);
        }
        feng_symbol_graph_free(graph);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        for (size_t profile = 0U; profile < 2U; ++profile) {
            FengSymbolProvider *provider = NULL;
            CHECK(feng_symbol_provider_create(&provider, &error));
            CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
                profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
            FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
            CHECK(cache != NULL);
            FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
            analysis = projection_analyze("module consumer; import projection.ft;\n"
                "func forward<Z:All<i32>,A:All<string>>(z:Z,a:A):i32{return scan<Z,A>(z,a);}\n", &query, true, &program);
            CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
            size_t found = 0U;
            for (size_t i = 0U; i < analysis->reifiable_dep_set_count; ++i) {
                const FengReifiableDepSet *set = &analysis->reifiable_dep_sets[i];
                if (set->constraint_projection_count != 4U) continue;
                ++found;
                for (size_t j = 0U; j < 4U; ++j)
                    CHECK(feng_semantic_constraint_projection_slot(set, &set->constraint_projections[j]) == j);
            }
            CHECK(found == 1U);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
            const char *negative[] = {
                "func bad<T>(value:T):i32{return right<T>(value);}",
                "func bad<T:Right>(value:T):i32{return left<i32,T>(value,1);}",
                "func bad<T:Both<i32>>(value:T):string{return left<string,T>(value,\"bad\");}",
                "func bad<T:Both<i32>>(value:T):i32{return other<T>(value);}"
            };
            for (size_t i = 0U; i < sizeof negative / sizeof *negative; ++i) {
                char consumer[512];
                snprintf(consumer, sizeof consumer, "module consumer;\nimport projection.ft;\n%s\n", negative[i]);
                analysis = projection_analyze(consumer, &query, false, &program);
                feng_semantic_analysis_free(analysis);
                feng_program_free(program);
            }
            feng_symbol_imported_module_cache_free(cache);
            feng_symbol_provider_free(provider);
        }
        feng_symbol_error_free(&error);
    }
    puts("constraint projection FT profiles, slots and corruption matrices passed");
}
