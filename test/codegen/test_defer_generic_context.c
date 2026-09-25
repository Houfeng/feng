#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <sys/stat.h>
#include <unistd.h>

/* Optimized compilation must also accept leaf cleanups without EH regions. */
static void defer_context_compile_release(const char *source) {
    char directory[] = "temp/defer-context-release-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char input[256], output[256], command[1024];
    snprintf(input, sizeof(input), "%s/generated.c", directory);
    snprintf(output, sizeof(output), "%s/generated.o", directory);
    FILE *file = fopen(input, "wb");
    THROW_CHECK(file != NULL);
    THROW_CHECK(fwrite(source, 1U, strlen(source), file) == strlen(source));
    THROW_CHECK(fclose(file) == 0);
    snprintf(command, sizeof(command),
        "cc " FENG_TEST_C_EH_FLAGS "-Isrc -Isrc/runtime -std=gnu11 -fexceptions -O2 -Werror -c '%s' -o '%s'", input, output);
    THROW_CHECK(system(command) == 0);
    THROW_CHECK(unlink(input) == 0 && unlink(output) == 0 && rmdir(directory) == 0);
}

/* Verify host-C validity, including every descriptor referenced by a helper. */
static void defer_context_emit(ThrowConstraintUnit *unit, void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit->analysis, FENG_COMPILE_TARGET_LIB,
                                        NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok && output.c_source != NULL);
    /* Catches require platform metadata, but cleanups must not acquire the
     * ordinary callable's additional runtime frame push/pop operations. */
    const char *cursor = output.c_source;
    while ((cursor = strstr(cursor, "static void __defer_")) != NULL) {
        const char *body = strchr(cursor, '{');
        const char *semicolon = strchr(cursor, ';');
        ++cursor;
        if (body == NULL || (semicolon != NULL && semicolon < body)) continue;
        const char *end = strstr(body, "\n}\n");
        const char *personality = strstr(body, "__llvm_c_eh_configure(");
        const char *region = strstr(body, "FengCatchContext ");
        const char *push = strstr(body, "feng_frame_push(");
        const char *frame = strstr(body, "FengFrameMarker ");
        THROW_CHECK(end != NULL);
        THROW_CHECK((personality != NULL && personality < end) ==
                    (region != NULL && region < end));
        THROW_CHECK(push == NULL || push > end);
        THROW_CHECK(frame == NULL || frame > end);
        cursor = end;
    }
    compile_c(output.c_source);
    defer_context_compile_release(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
}

/* The same body is compiled locally, exported, and called from FT-only clients. */
static const char *defer_context_provider =
    "open module cleanup.context;"
    "open spec Action():void;"
    "func consume<T>(value:T):void{}"
    "open func direct<T>(value:T):void{defer{consume(value);}}"
    "open func constructed<T>(value:T):void{defer{"
    "let array:T[]=[value];consume<T[]>(array);}}"
    "open func empty<T>():void{defer{let array:T[]=[];consume<T[]>(array);}}"
    "open func leaf():void{var value:i32=1;defer{value=2;}}"
    "open func closure<T>(value:T):Action{return (){defer{consume(value);}};}"
    "open func nested<T>(value:T):void{defer{let f:Action=(){consume(value);};f();}}"
    "open type Owner<T>{open var value:T;open func finish<U>(other:U):void{"
    "defer{consume(self.value);consume(other);consume<T[]>([self.value]);}}}"
    "@value open type ValueOwner<T>{open var value:T;open func finish<U>(other:U):void{"
    "defer{consume(self.value);consume(other);}}}"
    "open type FixedOwner{open func finish<T>(value:T):void{defer{consume(value);}}}"
    "open type Extended<T>{open let value:T;}"
    "open fit Extended<T>{open func finish<U>(other:U):void{"
    "defer{consume(self.value);consume(other);}}}"
    "open spec Factory{static func work():void;}"
    "open type Maker:Factory{open static func work():void{}}"
    "open func witness<T:Factory>():void{defer{try T.work() catch{}}}"
    "open type Pair<T>(T,string);"
    "open func handled<T:throw>(value:T):void{defer{try raise(value) catch{}}}"
    "func raise<T:throw>(value:T):void{throw value;}";

/* Closed clients exercise scalar, managed and descriptor-sized representations. */
static const char *defer_context_calls =
    "func run():void{"
    "direct((i32)7);direct(\"text\");let pair:Pair<string> = (\"a\",\"b\");direct(pair);"
    "constructed(\"text\");empty<i32>();empty<Pair<string> >();"
    "let f=closure(\"text\");f();nested(\"text\");"
    "let a=Owner<string>{value:\"owner\"};a.finish((i32)9);"
    "let b=ValueOwner<string>{value:\"owner\"};b.finish(true);"
    "let c=FixedOwner();c.finish(\"method\");"
    "let d=Extended<string>{value:\"fit\"};d.finish((i32)3);"
    "witness<Maker>();handled(\"error\");}"
    "func forward<U>(value:U):void{direct(value);constructed(value);empty<U>();}"
    "func plain():void{var value:i32=1;defer{value=2;}defer{}}";

/* Keep import consumers independent of the original AST and generic names. */
void test_defer_generic_context_codegen(void (*compile_c)(const char *)) {
    size_t capacity = strlen(defer_context_provider) + strlen(defer_context_calls) + 1U;
    char *source = malloc(capacity);
    THROW_CHECK(source != NULL);
    snprintf(source, capacity, "%s%s", defer_context_provider, defer_context_calls);
    ThrowConstraintUnit local = throw_constraint_analyze(source, NULL, NULL);
    defer_context_emit(&local, compile_c);
    throw_constraint_dispose(&local);
    free(source);

    ThrowConstraintUnit producer = throw_constraint_analyze(defer_context_provider, NULL, NULL);
    defer_context_emit(&producer, compile_c);
    char directory[] = "temp/defer-context-ft-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof(roots[0]), "%s/public", directory);
    snprintf(roots[1], sizeof(roots[1]), "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    THROW_CHECK(feng_symbol_export_analysis(producer.analysis, &options, &error));
    throw_constraint_dispose(&producer);
    capacity = strlen(defer_context_calls) + 80U;
    source = malloc(capacity);
    THROW_CHECK(source != NULL);
    snprintf(source, capacity, "module consumer;import cleanup.context;%s", defer_context_calls);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
            &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        ThrowConstraintUnit consumer = throw_constraint_analyze(source, &query, NULL);
        THROW_CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, consumer.analysis));
        defer_context_emit(&consumer, compile_c);
        throw_constraint_dispose(&consumer);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    free(source);
    puts("defer generic context source and FT codegen matrices passed");
}
