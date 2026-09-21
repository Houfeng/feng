#include "../throw_constraint_helpers.h"
#include "semantic/exception_effects.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/ft_internal.h"
#include "symbol/internal.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"
#include <unistd.h>

/* Read fixed-width wire words independently of the production reader. */
static uint64_t effects_word(const unsigned char *p, size_t width) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; ++i)
        value |= (uint64_t)p[i] << (i * 8U);
    return value;
}

/* Replace exactly one wire word in a valid output fixture. */
static void effects_put(unsigned char *p, size_t width, uint64_t value) {
    for (size_t i = 0U; i < width; ++i)
        p[i] = (unsigned char)(value >> (i * 8U));
}

/* Reject old, truncated, oversized, cyclic and incorrectly typed summaries. */
static void effects_corruptions(const char *path) {
    FILE *file = fopen(path, "rb");
    THROW_CHECK(file != NULL);
    THROW_CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    THROW_CHECK(size > 64);
    THROW_CHECK(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *original = malloc((size_t)size), *bytes = malloc((size_t)size);
    THROW_CHECK(original != NULL && bytes != NULL);
    THROW_CHECK(fread(original, 1U, (size_t)size, file) == (size_t)size && fclose(file) == 0);
    size_t directory = (size_t)effects_word(original + 0x18, 8U), section = SIZE_MAX, strings = SIZE_MAX;
    for (size_t i = 0U; i < effects_word(original + 0x0c, 2U); ++i) {
        if (effects_word(original + directory + i * 32U, 2U) == FENG_SYMBOL_FT_SEC_EXCEPTION_EFFECTS)
            section = directory + i * 32U;
        if (effects_word(original + directory + i * 32U, 2U) == FENG_SYMBOL_FT_SEC_STRS)
            strings = directory + i * 32U;
    }
    THROW_CHECK(section != SIZE_MAX && strings != SIZE_MAX);
    size_t base = (size_t)effects_word(original + section + 8U, 8U);
    size_t roots = (size_t)effects_word(original + base, 4U),
           funcs = (size_t)effects_word(original + base + 4U, 4U);
    size_t first_fn = base + 16U + roots * 12U, first_node = first_fn;
    for (size_t i = 0U; i < funcs; ++i)
        first_node += 28U + 4U * (size_t)effects_word(original + first_node + 4U, 4U);
    THROW_CHECK(roots > 1U && funcs > 1U && first_node < (size_t)size);
    size_t first_root_fn = (size_t)effects_word(original + base + 20U, 4U);
    size_t root_function = first_fn;
    for (size_t i = 0U; i < first_root_fn; ++i)
        root_function += 28U + 4U * (size_t)effects_word(original + root_function + 4U, 4U);
    uint32_t mismatched_roots[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    size_t at = first_fn;
    for (uint32_t i = 0U; i < funcs; ++i) {
        for (size_t shape = 0U; shape < 3U; ++shape) {
            size_t field = 4U + shape * 4U;
            if (effects_word(original + at + field, 4U) != effects_word(original + root_function + field, 4U))
                mismatched_roots[shape] = i;
        }
        at += 28U + 4U * (size_t)effects_word(original + at + 4U, 4U);
    }
    for (size_t shape = 0U; shape < 3U; ++shape)
        THROW_CHECK(mismatched_roots[shape] != UINT32_MAX);
    THROW_CHECK(effects_word(original + base + 12U, 4U) == 1U);
    size_t node_count = (size_t)effects_word(original + base + 8U, 4U);
    size_t offsets[FENG_EXCEPTION_GUARDED + 1U] = {0};
    uint32_t indices[FENG_EXCEPTION_GUARDED + 1U] = {0};
    at = first_node;
    for (uint32_t i = 0U; i < node_count; ++i) {
        uint32_t kind = (uint32_t)effects_word(original + at, 4U);
        THROW_CHECK(kind <= FENG_EXCEPTION_GUARDED);
        offsets[kind] = at;
        indices[kind] = i;
        if (kind != FENG_EXCEPTION_NAMED_TYPE)
            THROW_CHECK(effects_word(original + at + 16U, 4U) == 0U);
        at += 28U + 4U * (size_t)effects_word(original + at + 20U, 4U);
    }
    const FengExceptionNodeKind needed[] = {FENG_EXCEPTION_UNKNOWN, FENG_EXCEPTION_NAMED_TYPE,
        FENG_EXCEPTION_CALL, FENG_EXCEPTION_VALUE_PARAMETER, FENG_EXCEPTION_EQUAL_TYPE,
        FENG_EXCEPTION_DECISION, FENG_EXCEPTION_GUARDED};
    for (size_t i = 0U; i < sizeof needed / sizeof *needed; ++i)
        THROW_CHECK(offsets[needed[i]] != 0U);
    uint32_t type_name = (uint32_t)effects_word(original + offsets[FENG_EXCEPTION_NAMED_TYPE] + 16U, 4U);
    size_t string_base = (size_t)effects_word(original + strings + 8U, 8U);
    size_t string_count = (size_t)effects_word(original + strings + 4U, 4U);
    const char *forbidden[] = {"none", "bool, unknown", "dynamic callable target", "dynamic spec method",
        "abstract callable target", "mutable callable value", "mutable global callable value",
        "Possible exceptions", "uncaught exceptions must not cross the @abi ABI boundary"};
    for (size_t i = 0U; i < string_count; ++i) {
        size_t start = (size_t)effects_word(original + string_base + i * 4U, 4U);
        size_t end = (size_t)effects_word(original + string_base + (i + 1U) * 4U, 4U);
        const unsigned char *text = original + string_base + (string_count + 1U) * 4U + start;
        for (size_t j = 0U; j < sizeof forbidden / sizeof *forbidden; ++j)
            THROW_CHECK(end - start != strlen(forbidden[j]) || memcmp(text, forbidden[j], end - start) != 0);
    }
    for (size_t variant = 0U; variant < 37U; ++variant) {
        memcpy(bytes, original, (size_t)size);
        switch (variant) {
        case 0U:
            effects_put(bytes + section, 2U, 0xffU);
            break;
        case 1U:
            effects_put(bytes + section + 2U, 2U, 0U);
            break;
        case 2U:
            effects_put(bytes + section + 0x18, 4U, 4U);
            break;
        case 3U:
            effects_put(bytes + section + 4U, 4U, roots + 1U);
            break;
        case 4U:
            effects_put(bytes + base + 12U, 4U, 0U);
            break;
        case 5U:
            effects_put(bytes + base, 4U, UINT32_MAX);
            break;
        case 6U:
            effects_put(bytes + base + 16U, 4U, UINT32_MAX);
            break;
        case 7U:
            effects_put(bytes + base + 28U, 4U, effects_word(bytes + base + 16U, 4U));
            break;
        case 8U:
            effects_put(bytes + base + 20U, 4U, UINT32_MAX);
            break;
        case 9U:
            effects_put(bytes + base + 24U, 4U, UINT32_MAX);
            break;
        case 10U:
            effects_put(bytes + first_fn + 16U, 4U, UINT32_MAX);
            break;
        case 11U:
            effects_put(bytes + first_fn + 20U, 4U, UINT32_MAX);
            break;
        case 12U:
            effects_put(bytes + first_fn + 4U, 4U, UINT32_MAX);
            break;
        case 13U:
            effects_put(bytes + first_node, 4U, UINT32_MAX);
            break;
        case 14U:
            effects_put(bytes + first_node + 4U, 4U, 1U);
            break;
        case 15U:
            effects_put(bytes + first_node + 24U, 4U, 1U);
            break;
        case 16U:
            effects_put(bytes + section + 16U, 8U, effects_word(bytes + section + 16U, 8U) - 1U);
            break;
        case 17U:
        case 18U:
        case 19U:
            effects_put(bytes + base + 20U, 4U, mismatched_roots[variant - 17U]);
            break;
        case 20U:
            effects_put(bytes + root_function + 8U, 4U, effects_word(bytes + root_function + 8U, 4U) + 1U);
            break;
        case 21U:
            effects_put(bytes + base + 12U, 4U, 3U);
            break;
        case 22U:
            effects_put(bytes + offsets[FENG_EXCEPTION_UNKNOWN] + 16U, 4U, type_name);
            break;
        case 23U:
        case 24U:
        case 25U:
        case 33U:
        case 34U: {
            uint32_t root = variant == 23U ? indices[FENG_EXCEPTION_CALL]
                : variant == 24U ? indices[FENG_EXCEPTION_NAMED_TYPE]
                : variant == 25U ? 1U : variant == 33U ? indices[FENG_EXCEPTION_VALUE_PARAMETER]
                : indices[FENG_EXCEPTION_EQUAL_TYPE];
            effects_put(bytes + base + 24U, 4U, root);
            break;
        }
        case 26U:
            effects_put(bytes + offsets[FENG_EXCEPTION_EQUAL_TYPE] + 4U, 4U, 1U);
            break;
        case 27U:
            effects_put(bytes + offsets[FENG_EXCEPTION_DECISION] + 4U, 4U, 1U);
            break;
        case 28U:
            effects_put(bytes + offsets[FENG_EXCEPTION_DECISION] + 8U, 4U, indices[FENG_EXCEPTION_DECISION]);
            break;
        case 29U:
            effects_put(bytes + offsets[FENG_EXCEPTION_DECISION] + 12U, 4U, indices[FENG_EXCEPTION_UNKNOWN]);
            break;
        case 30U:
        case 32U:
        case 35U:
            effects_put(bytes + offsets[FENG_EXCEPTION_GUARDED] + 4U, 4U,
                indices[variant == 30U ? FENG_EXCEPTION_NAMED_TYPE
                    : variant == 32U ? FENG_EXCEPTION_UNKNOWN : FENG_EXCEPTION_EQUAL_TYPE]);
            break;
        case 31U:
            effects_put(bytes + offsets[FENG_EXCEPTION_GUARDED] + 8U, 4U, indices[FENG_EXCEPTION_CALL]);
            break;
        case 36U:
            effects_put(bytes + first_node + 16U, 4U, type_name);
            break;
        }
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t i = (size_t)effects_word(bytes + 0x20, 8U); i < (size_t)size; ++i)
            hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        effects_put(bytes + 0x28, 8U, hash);
        FengSymbolGraph *loaded = NULL;
        FengSymbolError error = {0};
        bool ok =
            feng_symbol_ft_read_bytes(bytes, (size_t)size, "invalid-exceptions.ft", NULL, &loaded, &error);
        if (ok)
            fprintf(stderr, "accepted exception corruption %zu\n", variant);
        THROW_CHECK(!ok && loaded == NULL && error.message != NULL);
        feng_symbol_error_free(&error);
    }
    free(original);
    free(bytes);
}

/* Assert results after the producing AST and analysis have been destroyed. */
static void effects_consumer_source(const FengSemanticImportedModuleQuery *query, const char *source,
                                    const char *expected) {
    ThrowConstraintUnit unit = throw_constraint_analyze(source, query, NULL);
    const FengExceptionTemplate *summary = NULL;
    for (size_t i = 0U; i < unit.program->declaration_count; ++i) {
        const FengDecl *d = unit.program->declarations[i];
        if (d->kind == FENG_DECL_FUNCTION && d->as.function_decl.name.length == 3U &&
            memcmp(d->as.function_decl.name.data, "run", 3U) == 0)
            summary = feng_semantic_exception_template(unit.analysis, d);
    }
    THROW_CHECK(summary != NULL);
    char *types = feng_exception_format(summary->graph, summary->effects);
    THROW_CHECK(types != NULL);
    if (strcmp(types, expected) != 0)
        fprintf(stderr, "expected '%s', got '%s'\n%s\n", expected, types, source);
    THROW_CHECK(strcmp(types, expected) == 0);
    free(types);
    throw_constraint_dispose(&unit);
}

/* Most consumers use the public API's ordinary unqualified import. */
static void effects_consumer(const FengSemanticImportedModuleQuery *query, const char *body,
                             const char *expected) {
    char source[8192];
    snprintf(source, sizeof source, "module consumer;import effects.api;%s", body);
    effects_consumer_source(query, source, expected);
}

/* Public and workspace profiles carry complete private dependencies and open slots. */
void test_exception_effects_ft(void) {
    const char *source =
        "open module effects.api;"
        "func hidden<T:throw>(x:T){throw x;}"
        "/** Structured exception metadata documentation. */"
        "open func raise<T:throw>(x:T){hidden(x);}"
        "open func filtered<T:throw>(x:T){try hidden(x) catch e:string{}}"
        "open spec Action():void;"
        "var callback:Action=(){};open func opaque(){callback();}"
        "open func invoke(f:Action){f();}"
        "open func mixed(f:Action,w:Worker){f();w.work();throw true;}"
        "open func factory<T:throw>(x:T):Action{return (){throw x;};}"
        "open type Box<T:throw>{open let value:T;open func fail(){hidden(self.value);}"
        "open func method<U:throw>(x:U){hidden(x);}"
        "open func both<U:throw>(x:U){if false{hidden(self.value);}hidden(x);}}"
        "open type Fitted<T:throw>{open let value:T;}open fit Fitted<T>{open func "
        "fail(){hidden(self.value);}}"
        "open spec Worker{func work():void;}open type W:Worker{func work():void{throw \"x\";}}"
        "open fit i32[]:Worker{func work():void{throw true;}}"
        "open spec GenericWorker{func act():void;}open fit T[]:GenericWorker{func act():void{throw true;}}"
        "open func genericWork<T:GenericWorker>(x:T){x.act();}"
        "open spec Factory{static func work():void;}open type Static:Factory{static func work():void{throw "
        "true;}}"
        "open func staticWork<T:Factory>(){T.work();}"
        "open func staticAction<T:Factory>():Action{return T.work;}"
        "func initial<T:throw>():i32{let x:T;throw x;}"
        "open let fixed:i32=initial<string>();open var changing:i32=initial<bool>();"
        "open type Lazy<T:throw>{open static let value:i32=initial<T>();"
        "open static var current:i32=initial<T>();}"
        "open spec Stored{static var value:i32;}"
        "open type Store:Stored{static var value:i32=initial<bool>();}"
        "open func read<T:Stored>():i32{return T.value;}open func write<T:Stored>(){T.value=0;}"
        "open type Failure{open let code:i32;}open func failNominal(x:Failure){throw x;}"
        "open enum Status{Failed}open type Pair(i32,string);@value open type ValueError{open let code:i32;}"
        "open func work<T:Worker>(x:T){x.work();}"
        "open func recurse<T:throw>(x:T,n:i32){if n==0{hidden(x);}else{recurse(x,n-1);}}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengSymbolError error = {0};
    FengSymbolGraph *graph = NULL;
    THROW_CHECK(feng_symbol_build_graph(unit.analysis, &graph, &error));
    char directory[] = "temp/exception-ft-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char paths[2][256];
    for (size_t i = 0U; i < 2U; ++i) {
        snprintf(paths[i], sizeof paths[i], "%s/%zu.ft", directory, i);
        THROW_CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U),
                                                i == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC
                                                        : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                                                paths[i], &error));
        effects_corruptions(paths[i]);
    }
    feng_symbol_graph_free(graph);
    throw_constraint_dispose(&unit);
    for (size_t i = 0U; i < 2U; ++i) {
        FengSymbolGraph *loaded = NULL;
        THROW_CHECK(feng_symbol_ft_read_file(paths[i], NULL, &loaded, &error));
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_graph(provider, loaded, &error));
        feng_symbol_graph_free(loaded);
        const FengSymbolImportedModule *module = feng_symbol_provider_module_at(provider, 0U);
        const char *names[] = {"raise", "filtered", "invoke", "mixed", "opaque"};
        const char *summaries[] = {"T", "T", "unknown", "bool, unknown", "unknown"};
        for (size_t n = 0U; n < sizeof names / sizeof *names; ++n) {
            FengSlice name = {names[n], strlen(names[n])};
            const FengSymbolDeclView *decl = feng_symbol_module_find_public_value(module, name);
            const FengExceptionTemplate *summary = feng_symbol_decl_exception_template(decl);
            THROW_CHECK(summary != NULL && summary->graph != NULL);
            size_t nodes_before = summary->graph->node_count;
            char *display = feng_exception_format(summary->graph, summary->effects);
            THROW_CHECK(display != NULL && strcmp(display, summaries[n]) == 0);
            free(display);
            THROW_CHECK(summary->graph->node_count == nodes_before);
            if (n == 0U) {
                FengSlice doc = feng_symbol_decl_doc(decl);
                const char *expected = "Structured exception metadata documentation.";
                THROW_CHECK(doc.length == strlen(expected) && memcmp(doc.data, expected, doc.length) == 0);
            }
        }
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        effects_consumer(&query, "func run(){raise(\"x\");}", "string");
        effects_consumer(&query, "func run<T:throw>(x:T){raise(x);}", "T");
        effects_consumer(&query, "func run<T:throw>(x:T){filtered(x);}", "T");
        effects_consumer(&query, "func run(f:Action){invoke(f);}", "unknown");
        effects_consumer(&query, "func run(){opaque();}", "unknown");
        effects_consumer(&query, "func run(){try opaque() catch e:string{}}", "unknown");
        effects_consumer(&query, "@abi func run(){try opaque() catch{}}", "none");
        effects_consumer(&query, "func run(f:Action,w:Worker){mixed(f,w);}", "bool, unknown");
        effects_consumer(&query, "func run(f:Action,w:Worker){try mixed(f,w) catch e:bool{}}", "unknown");
        effects_consumer(&query, "func run(f:Action,w:Worker){try mixed(f,w) catch{}}", "none");
        effects_consumer(&query, "func run(){filtered(\"x\");}", "none");
        effects_consumer(&query, "func run(){filtered(true);}", "bool");
        effects_consumer(&query, "func run(){invoke((){throw \"x\";});}", "string");
        effects_consumer(&query, "func run(){factory(true)();}", "bool");
        effects_consumer(&query, "func run(x:Box<string>){x.fail();}", "string");
        effects_consumer(&query, "func run(x:Box<string>){x.method(true);}", "bool");
        effects_consumer(&query, "func run(x:Box<string>){try x.both(true) catch e:string{}}", "bool");
        effects_consumer(&query, "func run(x:Box<bool>){try x.both(\"x\") catch e:bool{}}", "string");
        effects_consumer(&query, "func run(x:Fitted<string>){x.fail();}", "string");
        effects_consumer(&query, "func run(x:W){work(x);}", "string");
        effects_consumer(&query, "func run(x:i32[]){work(x);}", "bool");
        effects_consumer(&query, "func run(x:string[]){genericWork(x);}", "bool");
        effects_consumer(&query, "func run(){staticWork<Static>();}", "bool");
        effects_consumer(&query, "func run(){staticAction<Static>()();}", "bool");
        effects_consumer(&query, "func run(){let x=fixed;}", "string");
        effects_consumer(&query, "import effects.api as a;func run(){let x=a.fixed;}", "string");
        effects_consumer(&query, "func run(){changing=0;}", "bool");
        effects_consumer(&query, "func run(){let x=Lazy<string>.value;}", "string");
        effects_consumer(&query, "func run(){let x=Lazy<bool>.value;}", "bool");
        effects_consumer(&query, "func run(){Lazy<string>.current=0;}", "string");
        effects_consumer(&query, "func run(){try Lazy<string>.value catch e:string{}}", "none");
        effects_consumer(&query, "func run(){read<Store>();}", "bool");
        effects_consumer(&query, "func run(){write<Store>();}", "bool");
        effects_consumer(&query, "@abi func run(){try write<Store>() catch e:bool{}}", "none");
        effects_consumer(&query, "func run(){let f=factory(true);}", "none");
        effects_consumer(&query, "import effects.api as a;func run(){a.raise(true);}", "bool");
        effects_consumer_source(&query,
                                "module consumer;import effects.api as a;"
                                "type Failure{let code:i32;}func run(x:a.Failure){"
                                "try a.failNominal(x) catch e:Failure{}}",
                                "effects.api.Failure");
        effects_consumer(&query, "func run(){recurse(\"x\",3);}", "string");
        const char *payloads[] = {"bool",   "i8",   "i16",     "i32",        "i64",        "u8",
                                  "u16",    "u32",  "u64",     "f32",        "f64",        "string",
                                  "Status", "Pair", "Failure", "ValueError", "Box<string>"};
        for (size_t p = 0U; p < sizeof payloads / sizeof *payloads; ++p) {
            char body[512], expected[128];
            snprintf(body, sizeof body, "func run(x:%s){raise(x);}", payloads[p]);
            snprintf(expected, sizeof expected, "%s%s", p < 12U ? "" : "effects.api.", payloads[p]);
            effects_consumer(&query, body, expected);
            snprintf(body, sizeof body, "func run(x:%s){try raise<%s>(x) catch e:%s{}}", payloads[p],
                     payloads[p], payloads[p]);
            effects_consumer(&query, body, "none");
        }
        effects_consumer(&query, "@abi func run(){try raise(\"x\") catch{}}", "none");
        unit = throw_constraint_analyze("module consumer;import effects.api;@abi func run(){raise(\"x\");}",
                                        &query, "AE1313");
        throw_constraint_dispose(&unit);
        unit = throw_constraint_analyze("module consumer;import effects.api;@abi func run(){opaque();}",
                                        &query, "AE1313");
        throw_constraint_dispose(&unit);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("exception effect FT profiles, source-invisible consumers and corruption matrices passed");
}
