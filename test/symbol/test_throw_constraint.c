#include "../throw_constraint_helpers.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/ft_internal.h"
#include "symbol/imported_module.h"
#include "symbol/internal.h"
#include "symbol/provider.h"
#include <unistd.h>

/* Recursively inspect the independent bound fact in the exported graph. */
static void throw_constraint_check_decl(const FengSymbolDeclView *decl, size_t counts[3]) {
    if (feng_symbol_decl_kind(decl) == FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
        FengSlice name = feng_symbol_decl_name(decl);
        FengConstraintKind kind = feng_symbol_decl_constraint_kind(decl);
        THROW_CHECK((unsigned)kind < 3U);
        ++counts[kind];
        if (name.length == 1U && name.data[0] == 'S') THROW_CHECK(kind == FENG_CONSTRAINT_SPEC);
        else if (name.length == 1U && name.data[0] == 'A') THROW_CHECK(kind == FENG_CONSTRAINT_NONE);
        else THROW_CHECK(kind == FENG_CONSTRAINT_THROW);
    }
    for (size_t i = 0U; i < feng_symbol_decl_member_count(decl); ++i)
        throw_constraint_check_decl(feng_symbol_decl_member_at(decl, i), counts);
}

/* No-bound and spec-bound declarations coexist with independent builtin facts. */
static void throw_constraint_check_graph(const FengSymbolGraph *graph) {
    THROW_CHECK(feng_symbol_graph_module_count(graph) == 1U);
    const FengSymbolModuleGraph *module = feng_symbol_graph_module_at(graph, 0U);
    size_t counts[3] = {0};
    throw_constraint_check_decl(&module->root_decl, counts);
    THROW_CHECK(counts[FENG_CONSTRAINT_NONE] == 1U);
    THROW_CHECK(counts[FENG_CONSTRAINT_SPEC] == 1U);
    THROW_CHECK(counts[FENG_CONSTRAINT_THROW] == 7U);
}

/* Decode wire integers independently of host alignment and struct layout. */
static uint64_t throw_ft_read(const unsigned char *bytes, size_t width) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; ++i) value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}

/* Mutate exactly one wire field in otherwise valid writer output. */
static void throw_ft_write(unsigned char *bytes, size_t width, uint64_t value) {
    for (size_t i = 0U; i < width; ++i) bytes[i] = (unsigned char)(value >> (8U * i));
}

/* Locate a section through its directory entry, without fixed payload offsets. */
static size_t throw_ft_section(const unsigned char *bytes, uint16_t kind) {
    size_t directory = (size_t)throw_ft_read(bytes + 0x18, 8U);
    size_t count = (size_t)throw_ft_read(bytes + 0x0C, 2U);
    for (size_t i = 0U; i < count; ++i) {
        size_t entry = directory + i * 32U;
        if (throw_ft_read(bytes + entry, 2U) == kind) return entry;
    }
    THROW_CHECK(false);
    return 0U;
}

/* Unknown, conflicting and malformed builtin facts cannot weaken constraints. */
static void throw_constraint_corruption(const char *path) {
    FILE *file = fopen(path, "rb");
    THROW_CHECK(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    long end = ftell(file);
    THROW_CHECK(end >= 64L && fseek(file, 0L, SEEK_SET) == 0);
    size_t length = (size_t)end;
    unsigned char *original = malloc(length), *bytes = malloc(length);
    THROW_CHECK(original != NULL && bytes != NULL);
    THROW_CHECK(fread(original, 1U, length, file) == length && fclose(file) == 0);
    THROW_CHECK(original[5] == 2U && original[6] == 0U);
    size_t attrs = throw_ft_section(original, FENG_SYMBOL_FT_SEC_ATTRS);
    size_t attr_count = (size_t)throw_ft_read(original + attrs + 4U, 4U);
    size_t attr_base = (size_t)throw_ft_read(original + attrs + 8U, 8U);
    size_t bound = SIZE_MAX, second = SIZE_MAX;
    for (size_t i = 0U; i < attr_count; ++i) {
        size_t record = attr_base + i * 20U;
        if (throw_ft_read(original + record + 4U, 2U) != FENG_SYMBOL_ATTR_BUILTIN_CONSTRAINT) continue;
        if (bound == SIZE_MAX) bound = record;
        else if (second == SIZE_MAX) second = record;
    }
    THROW_CHECK(bound != SIZE_MAX && second != SIZE_MAX);
    size_t syms = throw_ft_section(original, FENG_SYMBOL_FT_SEC_SYMS);
    size_t sym_count = (size_t)throw_ft_read(original + syms + 4U, 4U);
    size_t sym_base = (size_t)throw_ft_read(original + syms + 8U, 8U);
    uint64_t owner = throw_ft_read(original + bound, 4U);
    uint64_t spec_param = 0U, spec_type = 0U, other_owner = 0U;
    size_t owner_record = SIZE_MAX;
    for (size_t i = 0U; i < sym_count; ++i) {
        size_t record = sym_base + i * 28U;
        uint64_t id = throw_ft_read(original + record, 4U);
        if (id == owner) owner_record = record;
        if (throw_ft_read(original + record + 12U, 2U) != FENG_SYMBOL_FT_SYM_KIND_TYPE_PARAM) {
            other_owner = id;
        } else if (throw_ft_read(original + record + 16U, 4U) != 0U) {
            spec_param = id;
            spec_type = throw_ft_read(original + record + 16U, 4U);
        }
    }
    THROW_CHECK(owner_record != SIZE_MAX && spec_param != 0U && other_owner != 0U);
    for (size_t variant = 0U; variant < 12U; ++variant) {
        memcpy(bytes, original, length);
        switch (variant) {
            case 0U: throw_ft_write(bytes + bound + 8U, 4U, 0U); break;
            case 1U: throw_ft_write(bytes + bound + 8U, 4U, UINT32_MAX); break;
            case 2U: throw_ft_write(bytes + bound + 6U, 2U, 1U); break;
            case 3U: throw_ft_write(bytes + bound + 12U, 4U, 1U); break;
            case 4U: throw_ft_write(bytes + bound + 16U, 4U, 1U); break;
            case 5U: throw_ft_write(bytes + bound, 4U, 0U); break;
            case 6U: throw_ft_write(bytes + bound, 4U, UINT32_MAX); break;
            case 7U: throw_ft_write(bytes + bound, 4U, other_owner); break;
            case 8U: throw_ft_write(bytes + bound, 4U, spec_param); break;
            case 9U: throw_ft_write(bytes + second, 4U, owner); break;
            case 10U: throw_ft_write(bytes + owner_record + 16U, 4U, spec_type); break;
            case 11U: break; /* Truncate the genuine file below. */
        }
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t i = (size_t)throw_ft_read(bytes + 0x20, 8U); i < length; ++i) {
            hash ^= bytes[i]; hash *= UINT64_C(1099511628211);
        }
        throw_ft_write(bytes + 0x28, 8U, hash);
        FengSymbolGraph *loaded = NULL;
        FengSymbolError error = {0};
        bool ok = feng_symbol_ft_read_bytes(bytes, length - (variant == 11U ? 1U : 0U),
            "invalid-throw.ft", NULL, &loaded, &error);
        if (ok) fprintf(stderr, "accepted builtin constraint corruption %zu\n", variant);
        THROW_CHECK(!ok && loaded == NULL && error.message != NULL);
        feng_symbol_error_free(&error);
    }
    free(bytes);
    free(original);
}

/* Both FT profiles restore constraints before source-invisible consumer checks. */
void test_throw_constraint_ft(void) {
    const char *source = "open module bound.ft;\n"
        "open spec Named{let code:i32;}"
        "open func raise<E:throw>(error:E){throw error;}"
        "open func mixed<A,S:Named,T:throw>(a:A,s:S,t:T){throw t;}"
        "open type Owner<E:throw>{open let error:E;open func raise(){throw self.error;}"
        "open func method<U:throw>(value:U){throw value;}"
        "open static func send<V:throw>(value:V){throw value;}}"
        "open spec Reader<E:throw>{func read():E;}"
        "open spec Action<E:throw>(value:E):void;";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengSymbolError error = {0};
    FengSymbolGraph *graph = NULL;
    THROW_CHECK(feng_symbol_build_graph(unit.analysis, &graph, &error));
    throw_constraint_check_graph(graph);
    char directory[] = "temp/throw-ft-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256], file[256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    THROW_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    for (size_t profile = 1U; profile <= 2U; ++profile) {
        snprintf(file, sizeof file, "%s/roundtrip%zu.ft", directory, profile);
        THROW_CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U),
            (FengSymbolProfile)profile, file, &error));
        FengSymbolGraph *loaded = NULL;
        THROW_CHECK(feng_symbol_ft_read_file(file, NULL, &loaded, &error));
        throw_constraint_check_graph(loaded);
        feng_symbol_graph_free(loaded);
        throw_constraint_corruption(file);
    }
    feng_symbol_graph_free(graph);
    throw_constraint_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        const char *cases[] = {
            "func run(){raise(1);raise<string>(\"message\");let owner=Owner<i32>{error:1};owner.raise();owner.method<string>(\"m\");Owner<string>.send<i32>(1);}",
            "func forward<T:throw>(x:T){raise<T>(x);let owner=Owner<T>{error:x};owner.raise();}",
            "spec Child<T:throw>:Reader<T>{}func run(x:Action<string>){}",
            "type Item:Named{let code:i32;}func run(x:Item){mixed<i32,Item,string>(1,x,\"m\");}",
            "func bad(xs:i32[]){raise(xs);}",
            "func bad<T>(x:T){raise<T>(x);}",
            "func bad<T:Named>(x:T){raise(x);}",
            "func bad(x:Owner<i32[]>){}",
            "func bad<T>(x:Owner<T>){}",
            "func bad(xs:i32[]){Owner<i32>().method<i32[]>(xs);}",
            "func bad(xs:i32[]){Owner<i32>.send<i32[]>(xs);}",
            "spec Child<T>:Reader<T>{}",
            "func bad(x:Action<i32[]>){}",
            "func bad<T:throw>(x:T){try raise(x) catch e:T{}}"
        };
        const char *codes[] = {NULL, NULL, NULL, NULL, "AE0512", "AE0512", "AE0512",
            "AE0710", "AE0710", "AE0512", "AE0512", "AE0710", "AE0710", "AE0179"};
        for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
            char consumer[1024];
            snprintf(consumer, sizeof consumer, "module consumer;import bound.ft;%s", cases[i]);
            unit = throw_constraint_analyze(consumer, &query, codes[i]);
            throw_constraint_dispose(&unit);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("throw constraint FT profiles and source-invisible checks passed");
}
