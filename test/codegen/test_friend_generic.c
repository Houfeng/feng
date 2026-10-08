#include "../friend_generic_helpers.h"
#include "codegen/codegen.h"
#include "symbol/export.h"

/* Adapt the real package-public selection rather than inventing export rules. */
static bool friend_package_contains(const void *user, const void *source_node) {
    return feng_symbol_package_selection_contains(user, source_node);
}

/* Authorized generic access keeps the ordinary field/callable lowering and ABI. */
void test_friend_generic_codegen(void (*compile_c)(const char *)) {
    const char *source = "open module friend_codegen;open spec Getter<T>():T;"
        "open type Vault<F,T>{@friend(F) seal var value:T;"
        "func Vault(x:T){self.value=x;}@friend(F) seal func read():T{return self.value;}"
        "@friend(F) seal static var shared:int=3;@friend(F) seal static func count():int{return 5;}"
        "@friend(F) seal func identity<U>(x:U):U{return x;}}"
        "open type Reader<T>{func read(v:Vault<Reader<T>,T>):T{"
        "let f:Getter<T> =v.read;let nested:Getter<T> =()->f();return v.identity<T>(nested());}"
        "static func count():int{return Vault<Reader<T>,T>.shared+Vault<Reader<T>,T>.count();}}"
        "open fit Reader<T>{func replace(v:Vault<Reader<T>,T>,x:T):T{v.value=x;return v.read();}}"
        "open spec Surface<T>{func read():int;}"
        "open type SurfaceValue:Surface<int>{func read():int{return 2;}}"
        "open func specValue<T>(value:Surface<T>):int{let method:Getter<int> =value.read;return method();}"
        "open func partial<T>(){let v=Vault<Reader<T>,int>(1);}"
        "open func run():int{let v=Vault<Reader<int>,int>(7);let r=Reader<int>();"
        "return r.read(v)+r.replace(v,11)+Reader<int>.count()+specValue<int>(SurfaceValue());}";
    FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
    FengSymbolGraph *graph = NULL;
    FengSymbolPackageSelection *selection = NULL;
    FengSymbolError symbol_error = {0};
    FRIEND_CHECK(feng_symbol_build_graph(unit.analysis, &graph, &symbol_error));
    FRIEND_CHECK(feng_symbol_build_package_selection(graph, &selection, &symbol_error));
    FengCodegenPackageSymbolQuery query = {.user = selection, .contains_source_node = friend_package_contains};
    FengCodegenOptions options = {.package_symbols = &query};
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB, &options, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    FRIEND_CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    feng_symbol_package_selection_free(selection);
    feng_symbol_graph_free(graph);
    feng_symbol_error_free(&symbol_error);
    friend_generic_dispose(&unit);
    puts("friend generic package surface and generated C passed");
}

/* Cross-module private layouts retain their resolved descriptor dependency. */
void test_friend_private_codegen(void (*compile_c)(const char *)) {
    FengProgram *api = friend_generic_parse(
        "open module private_codegen.api;import private_codegen.hidden;import private_codegen.collections;"
        "open type Reader<T>{seal let callbacks:Actions<Action<T>>;"
        "func read(v:Vault<Reader<T>,T>):T{return v.payload.value;}}"
        "open type Vault<F,T>{@friend(F) seal let payload:Payload<T>;"
        "func Vault(value:T){self.payload=Payload<T>(value);}}");
    FengProgram *hidden = friend_generic_parse(
        "seal module private_codegen.hidden;@value open type Payload<T>{let value:T;"
        "func Payload(value:T){self.value=value;}}open spec Action<T>(x:T):void;");
    FengProgram *collections = friend_generic_parse(
        "open module private_codegen.collections;open type Actions<T>{let items:T[];}");
    const FengProgram *programs[] = {api, hidden, collections};
    FengSemanticAnalyzeOptions semantic_options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FRIEND_CHECK(feng_semantic_analyze_with_options(programs, 3U, &semantic_options,
        &analysis, &errors, &error_count) && error_count == 0U);
    const FengDecl *reader = api->declarations[0];
    const FengReifiableDepSet *deps = feng_semantic_lookup_member_reifiable_dep_set(
        analysis, reader, reader->as.type_decl.members[1]);
    bool has_private_layout = false;
    FRIEND_CHECK(deps != NULL);
    for (size_t i = 0U; i < deps->dep_count; ++i) {
        if (deps->deps[i].kind == FENG_REIFIABLE_DEP_KIND_AGGREGATE &&
            deps->deps[i].type_ref->resolution_decl == hidden->declarations[0]) {
            has_private_layout = true;
        }
    }
    FRIEND_CHECK(has_private_layout);
    FengSymbolGraph *graph = NULL;
    FengSymbolPackageSelection *selection = NULL;
    FengSymbolError symbol_error = {0};
    FRIEND_CHECK(feng_symbol_build_graph(analysis, &graph, &symbol_error));
    FRIEND_CHECK(feng_symbol_build_package_selection(graph, &selection, &symbol_error));
    FengCodegenPackageSymbolQuery query = {.user = selection, .contains_source_node = friend_package_contains};
    FengCodegenMapingSourceMapping source_mapping = {.source_path = "friend_generic.ff",
        .package_name = "private_codegen", .package_root = "."};
    FengCodegenOptions options = {.package_symbols = &query,
        .debug_source_mappings = &source_mapping, .debug_source_mapping_count = 1U};
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, &options, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    FRIEND_CHECK(ok && output.c_source != NULL);
    FRIEND_CHECK(strstr(output.c_source, "_desc->reified_agg_deps[0]") != NULL);
    bool has_callback_field = false;
    for (size_t i = 0U; i < output.debug_info.variable_count; ++i) {
        const FengCodegenMapingVariableRecord *field = &output.debug_info.variables[i];
        if (field->kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD &&
            strcmp(field->display_name, "items") == 0 &&
            strcmp(field->parent_display_type,
                "private_codegen.collections.Actions<private_codegen.hidden.Action<T>>") == 0) {
            FRIEND_CHECK(strcmp(field->display_type, "private_codegen.hidden.Action<T>[]") == 0);
            has_callback_field = true;
        }
    }
    FRIEND_CHECK(has_callback_field);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    feng_symbol_package_selection_free(selection);
    feng_symbol_graph_free(graph);
    feng_symbol_error_free(&symbol_error);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(hidden);
    feng_program_free(collections);
    feng_program_free(api);
    puts("friend private resolved layout dependency and generated C passed");
}
