#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Source entry shapes that must share the local-storage debug contract. */
typedef struct CallableDebugCase {
    const char *name;
    const char *source;
} CallableDebugCase;

/* Keep the source path stable for the parser, semantic facts and debug map. */
static ThrowConstraintUnit callable_debug_analyze(const char *source) {
    ThrowConstraintUnit unit = {0};
    FengParseError parse_error = {0};
    bool parsed = feng_parse_source(source, strlen(source), "/debug-storage/main.ff",
                                    &unit.program, &parse_error);
    if (!parsed) fprintf(stderr, "%s: %s\n%s\n",
                         parse_error.code, parse_error.message, source);
    THROW_CHECK(parsed);
    const FengProgram *programs[] = {unit.program};
    const FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB, .pointer_size = feng_get_host_pointer_size()
    };
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
                                                &unit.analysis, &errors, &count);
    for (size_t i = 0U; i < count; ++i)
        fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
    THROW_CHECK(ok && count == 0U);
    feng_semantic_errors_free(errors, count);
    return unit;
}

/* Resolve a storage attribute from its declaration and emitted definition. */
static bool callable_debug_storage_hidden(const char *source, const char *body,
                                          const char *storage) {
    char declaration[256], definition[80];
    unsigned attribute;
    THROW_CHECK(snprintf(declaration, sizeof declaration, " %s =", storage) > 0);
    const char *position = strstr(body, declaration);
    THROW_CHECK(position != NULL);
    while (position > body && position[-1] != '\n') --position;
    THROW_CHECK(sscanf(position, " FENG_LOCAL_DEBUG_%u", &attribute) == 1);
    THROW_CHECK(snprintf(definition, sizeof definition,
                        "#define FENG_LOCAL_DEBUG_%u ", attribute) > 0);
    position = strstr(source, definition);
    THROW_CHECK(position != NULL);
    position += strlen(definition);
    return strncmp(position, "FENG_CODEGEN_NODEBUG", 20U) == 0;
}

/* Source variables must remain visible in their own callable, including a
 * result storage adopted by a binding after its declaration was emitted. */
void test_callable_debug_storage_codegen(void (*compile_c)(const char *)) {
    const CallableDebugCase cases[] = {
        {"concrete", "type Runner{func probe():int{%s}}"
                     "func use():int{return Runner().probe();}"},
        {"method", "type Runner{func probe<T>(value:T):int{%s}}"
                   "func use():int{return Runner().probe<int>(0);}"},
        {"owner", "type Runner<T>{func probe(value:T):int{%s}}"
                  "func use():int{return Runner<int>().probe(0);}"},
        {"combined", "type Runner<T>{func probe<U>(value:T,other:U):int{%s}}"
                     "func use():int{return Runner<int>().probe<string>(0,\"x\");}"},
        {"static", "type Runner{static func probe<T>(value:T):int{%s}}"
                   "func use():int{return Runner.probe<int>(0);}"},
        {"value owner", "@value type Runner<T>{var field:int;func probe(value:T):int{%s}}"
                        "func use():int{var runner=Runner<int>();return runner.probe(0);}"},
        {"scalar fit", "fit int{func probe():int{%s}}"
                       "func use():int{let value:int=0;return value.probe();}"},
        {"array fit", "fit T[]{func probe():int{%s}}"
                      "func use():int{let value:int[]=[0];return value.probe();}"},
        {"generic fit", "fit T[]{func probe<U>(value:U):int{%s}}"
                        "func use():int{let value:int[]=[0];return value.probe<int>(0);}"},
        {"nominal fit", "type Runner<T>{}fit Runner<T>{func probe<U>(value:U):int{%s}}"
                        "func use():int{return Runner<int>().probe<string>(\"x\");}"}
    };
    const char *body =
        "var total=0;for var i=0;i<2;i=i+1{"
        "let visible=identity<int>(i);consume(\"tick\",visible);total+=visible;}"
        "let action:Step=(){let nested=2;return nested;};"
        "defer{let deferred=2;consume(\"cleanup\",deferred);}"
        "let after=total;return after+action();";
    for (size_t index = 0U; index < sizeof cases / sizeof cases[0]; ++index) {
        char declaration[4096], source[8192];
        THROW_CHECK(snprintf(declaration, sizeof declaration, cases[index].source, body) > 0);
        THROW_CHECK(snprintf(source, sizeof source,
            "module debug.storage;spec Step():int;"
            "func identity<T>(value:T):T{return value;}"
            "func consume(value:string,count:int):int{return count;}%s", declaration) > 0);
        ThrowConstraintUnit unit = callable_debug_analyze(source);
        const FengCodegenMapingSourceMapping mapping = {
            .source_path = unit.program->path, .package_root = "/debug-storage",
            .package_name = "debug_storage"
        };
        const FengCodegenOptions options = {
            .emit_line_directives = true, .debug_source_mappings = &mapping,
            .debug_source_mapping_count = 1U
        };
        FengCodegenOutput output = {0};
        FengCodegenError error = {0};
        bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                                 &options, &output, &error);
        if (!emitted) fprintf(stderr, "debug storage %s: %s: %s\n",
                              cases[index].name, error.code, error.message);
        THROW_CHECK(emitted);
        bool found_visible = false, found_after = false;
        for (size_t i = 0U; i < output.debug_info.variable_count; ++i) {
            const FengCodegenMapingVariableRecord *variable = &output.debug_info.variables[i];
            bool is_visible = strcmp(variable->display_name, "visible") == 0;
            bool is_after = strcmp(variable->display_name, "after") == 0;
            if (!is_visible && !is_after) continue;
            const FengCodegenMapingFrameRecord *frame = NULL;
            for (size_t f = 0U; f < output.debug_info.frame_count; ++f) {
                if (strcmp(output.debug_info.frames[f].backend_symbol,
                           variable->frame_backend_symbol) == 0)
                    frame = &output.debug_info.frames[f];
            }
            THROW_CHECK(frame != NULL && frame->policy == FENG_CODEGEN_MAPING_FRAME_VISIBLE);
            const char *function = output.c_source;
            do {
                function = strstr(function, frame->backend_symbol);
                THROW_CHECK(function != NULL);
                function += strlen(frame->backend_symbol);
                const char *brace = strchr(function, '{');
                const char *semicolon = strchr(function, ';');
                THROW_CHECK(brace != NULL && semicolon != NULL);
                if (brace < semicolon) break;
            } while (true);
            THROW_CHECK(!callable_debug_storage_hidden(output.c_source, function,
                                                       variable->backend_name));
            /* The argument snapshot is compiler-owned, unlike either source
             * binding. Both must use the same deferred visibility mechanism. */
            const char *argument = strstr(function, " _call_arg");
            char argument_name[80];
            THROW_CHECK(argument != NULL);
            THROW_CHECK(sscanf(argument, " %79[_a-zA-Z0-9]", argument_name) == 1);
            THROW_CHECK(callable_debug_storage_hidden(output.c_source, function,
                                                       argument_name));
            found_visible |= is_visible;
            found_after |= is_after;
        }
        if (!found_visible || !found_after)
            fprintf(stderr, "debug storage %s has no source binding\n", cases[index].name);
        THROW_CHECK(found_visible && found_after);
        compile_c(output.c_source);
        feng_codegen_output_free(&output);
        feng_codegen_error_free(&error);
        throw_constraint_dispose(&unit);
    }
    puts("callable debug storage: 10 entry shapes preserve source bindings and local visibility");
}
