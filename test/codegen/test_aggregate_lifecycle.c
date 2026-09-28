#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Locate a generated definition without selecting its forward declaration. */
static char *aggregate_lifecycle_body(const char *source, const char *name) {
    for (const char *at = source; (at = strstr(at, name)) != NULL; ++at) {
        const char *line = strchr(at, '\n');
        const char *brace = strchr(at, '{');
        if (line == NULL || brace == NULL || brace > line) continue;
        const char *end = strstr(brace, "\n}");
        THROW_CHECK(end != NULL);
        char *body = strndup(brace, (size_t)(end - brace));
        THROW_CHECK(body != NULL);
        return body;
    }
    fprintf(stderr, "missing aggregate lifecycle definition: %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* Static stores and release callbacks share ordered managed leaves; dynamic
 * descriptors and initialization with old owners keep their original costs. */
void test_aggregate_lifecycle_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module aggregate.lifecycle;"
        "open type Ref{let n:int;}"
        "open spec Choice:bool|Ref;"
        "open type Holder{var item:Choice;}"
        "@value open type Pair{let first:Choice;let second:Choice;}"
        "open spec Nested:bool|Pair|Ref;"
        "open type Box{var item:Nested;}"
        "open type Initialized{var item:Choice=Ref{n:1};}"
        "open type Constructed{var item:Choice;func Constructed(){self.item=Ref{n:2};}}"
        "open func fresh(value:Choice):Holder{return Holder{item:value};}"
        "open func owned():Holder{return Holder{item:Ref{n:7}};}"
        "open func initialized(value:Choice):Initialized{return Initialized{item:value};}"
        "open func constructed(value:Choice):Constructed{return Constructed{item:value};}"
        "@value open type Open<T>{var value:T;}"
        "open func generic<T>(value:T):Open<T>{return Open<T>{value:value};}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted && output.c_source != NULL);
    compile_c(output.c_source);

    char *fresh = aggregate_lifecycle_body(output.c_source, "__fresh__");
    THROW_CHECK(strstr(fresh, "feng_aggregate_assign(") == NULL);
    THROW_CHECK(strstr(fresh, "feng_retain(") != NULL);
    THROW_CHECK(strstr(fresh, "feng_release(((*_aggregate_dst))") == NULL);
    THROW_CHECK(strstr(fresh, "_aggregate_dst != _aggregate_src") != NULL);
    THROW_CHECK(strstr(fresh, "memcpy(_aggregate_dst, _aggregate_src, sizeof *_aggregate_dst)") != NULL);
    free(fresh);

    char *owned = aggregate_lifecycle_body(output.c_source, "__owned__");
    THROW_CHECK(strstr(owned, "feng_aggregate_take(") == NULL);
    THROW_CHECK(strstr(owned, "feng_retain(") == NULL);
    THROW_CHECK(strstr(owned, "(*_aggregate_src)).payload.m1 = NULL") != NULL);
    free(owned);

    char *initialized = aggregate_lifecycle_body(output.c_source, "__initialized__");
    THROW_CHECK(strstr(initialized, "feng_release(((*_aggregate_dst)).payload.m1)") != NULL);
    free(initialized);
    char *constructed = aggregate_lifecycle_body(output.c_source, "__constructed__");
    const char *retain = strstr(constructed, "feng_retain(((*_aggregate_src)).payload.m1)");
    const char *release = strstr(constructed, "feng_release(((*_aggregate_dst)).payload.m1)");
    const char *copy = strstr(constructed, "memcpy(_aggregate_dst, _aggregate_src");
    THROW_CHECK(retain != NULL && release != NULL && copy != NULL);
    THROW_CHECK(retain < release && release < copy);
    free(constructed);

    char *nested = aggregate_lifecycle_body(output.c_source, "__Box__release_children");
    THROW_CHECK(strstr(nested, "feng_aggregate_release(") == NULL);
    THROW_CHECK(strstr(nested, "case 1U:") != NULL && strstr(nested, "case 2U:") != NULL);
    const char *first = strstr(nested, ".first");
    const char *second = strstr(nested, ".second");
    THROW_CHECK(first != NULL && second != NULL && first < second);
    free(nested);

    char *generic = aggregate_lifecycle_body(output.c_source, "__generic_G__");
    THROW_CHECK(strstr(generic, "feng_aggregate_assign(") != NULL);
    THROW_CHECK(strstr(generic, "_aggregate_dst") == NULL);
    free(generic);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    puts("aggregate lifecycle: static stores, ordered leaves and dynamic fallback passed");
}
