#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <unistd.h>

/* Named generic payloads deliberately leave their field parameter unconstrained. */
static const char throwable_open_provider[] =
    "open module roots.open_payload;"
    "@throwable open spec Fault{func code():i32;}"
    "open type Error<T>:Fault{open let value:T;open let number:i32;"
    "func code():i32{return self.number;}}"
    "@value open type Value<T>:Fault{open let value:T;open let number:i32;"
    "func code():i32{return self.number;}}";

/* Each independent client closes an open payload in a different descriptor domain. */
static const char *const throwable_open_cases[] = {
    "func reference<T>(value:Error<T>){throw value;}"
    "func aggregate<T>(value:Value<T>){throw value;}"
    "func run(){try reference(Error<string>{value:\"ref\",number:11}) catch e:Fault{e.code();}"
    "try aggregate(Value<string>{value:\"value\",number:13}) catch e:Value<string>{e.value;}}",
    "type Owner<T>{let reference:Error<T>;let value:Value<T>;"
    "func ref(){throw self.reference;}func val(){throw self.value;}}"
    "func run(){let owner=Owner<string>{reference:Error<string>{value:\"ref\",number:17},"
    "value:Value<string>{value:\"value\",number:19}};"
    "try owner.ref() catch e:Error<string>{e.value;}try owner.val() catch e:Fault{e.code();}}",
    "type Owner<T>{func ref<U>(value:Error<U>){throw value;}"
    "func val<U>(value:Value<U>){throw value;}}"
    "func run(){let owner=Owner<i32>();"
    "try owner.ref<string>(Error<string>{value:\"ref\",number:23}) catch e:Fault{e.code();}"
    "try owner.val<string>(Value<string>{value:\"value\",number:29}) catch e:Value<string>{e.value;}}",
    "@value type Owner<T>{let value:Value<T>;func fail(){throw self.value;}}"
    "func run(){let owner=Owner<string>{value:Value<string>{value:\"value\",number:31}};"
    "try owner.fail() catch e:Fault{e.code();}}",
};

/* Compile generated C as well as checking the exception preparation contract. */
static void throwable_open_emit(ThrowConstraintUnit *unit, void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit->analysis, FENG_COMPILE_TARGET_LIB,
                                         NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok && output.c_source != NULL);
    THROW_CHECK(strstr(output.c_source, "->reified_spec_view_coercions[") != NULL);
    THROW_CHECK(strstr(output.c_source, "feng_caught_witness()") != NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
}

/* Repeat local clients with both FT profiles, using a fresh import cache per case. */
static void throwable_open_codegen(void (*compile_c)(const char *)) {
    char source[4096], directory[] = "temp/throwable-open-XXXXXX", roots[2][256];
    THROW_CHECK(mkdtemp(directory) != NULL);
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    ThrowConstraintUnit producer = throw_constraint_analyze(throwable_open_provider, NULL, NULL);
    THROW_CHECK(feng_symbol_export_analysis(producer.analysis, &options, &error));
    throw_constraint_dispose(&producer);
    for (size_t profile = 0U; profile < 3U; ++profile) {
        FengSymbolProvider *provider = NULL;
        if (profile != 0U) {
            THROW_CHECK(feng_symbol_provider_create(&provider, &error));
            THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile - 1U],
                profile == 1U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
                &error));
        }
        for (size_t i = 0U; i < sizeof throwable_open_cases / sizeof throwable_open_cases[0]; ++i) {
            int length = snprintf(source, sizeof source, "%s%s", profile == 0U
                ? throwable_open_provider : "module consumer;import roots.open_payload;", throwable_open_cases[i]);
            THROW_CHECK(length > 0 && (size_t)length < sizeof source);
            FengSymbolImportedModuleCache *cache = profile == 0U ? NULL
                : feng_symbol_imported_module_cache_create(provider);
            THROW_CHECK(profile == 0U || cache != NULL);
            FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
            ThrowConstraintUnit unit = throw_constraint_analyze(source, cache == NULL ? NULL : &query, NULL);
            if (cache != NULL)
                THROW_CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, unit.analysis));
            throwable_open_emit(&unit, compile_c);
            throw_constraint_dispose(&unit);
            feng_symbol_imported_module_cache_free(cache);
        }
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("throwable open payloads: function, owner and method source/FT codegen passed");
}

/* Check real root preparation, generic root values and catch ownership.
 * Runtime behavior is separately exercised through native exception probes. */
void test_throwable_codegen(void (*compile_c)(const char *)) {
    static const char source[] =
        "open module roots;"
        "open spec Code{func code():i32;}"
        "@throwable open spec Fault:Code{}"
        "open spec Extra{func extra():i32;}"
        "type Error:Fault,Extra{let value:i32;"
        "func code():i32{return self.value;}func extra():i32{return 9;}}"
        "@value type Value:Fault{let text:string;func code():i32{return 23;}}"
        "fit i32:Fault{func code():i32{return self;}}"
        "func ref(value:Error){throw value;}"
        "func val(value:Value){throw value;}"
        "func scalar(value:i32){throw value;}"
        "func generic<E:Fault>(value:E){throw value;}"
        "func forward<E:Fault>(value:E){generic<E>(value);}"
        "func identity<E>(value:E):E{return value;}"
        "func result<E:Fault>(value:E){throw identity<E>(value);}"
        "type Owner<E:Fault>{let value:E;func fail(){throw self.value;}}"
        "@value type ValueOwner<E:Fault>{let value:E;func fail(){throw self.value;}}"
        "func concreteGenerics(error:Error,value:Value,number:i32){"
        "try generic<Error>(error) catch root:Fault {}"
        "try forward<Value>(value) catch root:Fault {}"
        "try result<i32>(number) catch root:Fault {}"
        "let owner=Owner<Value>{value:value};try owner.fail() catch root:Fault {}"
        "let valueOwner=ValueOwner<Error>{value:error};try valueOwner.fail() catch root:Fault {}}"
        "func view(value:Fault){throw value;}"
        "func rootGenerics(value:Fault){"
        "try generic<Fault>(value) catch root:Fault {}"
        "try forward<Fault>(value) catch root:Fault {}"
        "try result<Fault>(value) catch root:Fault {}"
        "let owner=Owner<Fault>{value:value};try owner.fail() catch root:Fault {}"
        "let valueOwner=ValueOwner<Fault>{value:value};try valueOwner.fail() catch root:Fault {}}"
        "func save():Fault{try ref(Error{value:17}) catch error:Fault{return error;}"
        "return Error{value:0};}"
        "func again(value:Fault):i32{try view(value) catch error:Error{return error.value;}"
        "catch error:Fault{return error.code();}return 0;}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                         NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok);
    compile_c(output.c_source);
    THROW_CHECK(strstr(output.c_source, "feng_caught_witness()") != NULL);
    THROW_CHECK(strstr(output.c_source, "__llvm_c_eh_typeid((const void *)NULL)") != NULL);
    THROW_CHECK(strstr(output.c_source, "->descriptor == &FengSpecAgg__roots__Fault") != NULL);
    THROW_CHECK(strstr(output.c_source, "feng_cleanup_push_aggregate(") != NULL);
    THROW_CHECK(strstr(output.c_source, "->reified_spec_view_coercions[0]") != NULL);
    THROW_CHECK(strstr(output.c_source, "_throw_formation") != NULL);
    feng_codegen_output_free(&output);
    throw_constraint_dispose(&unit);
    throwable_open_codegen(compile_c);
}
