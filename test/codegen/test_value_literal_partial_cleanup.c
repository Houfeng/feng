#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Select one generated function definition without matching its prototype. */
static char *value_cleanup_body(const char *source, const char *name) {
    for (const char *at = source; (at = strstr(at, name)) != NULL; ++at) {
        const char *brace = strchr(at, '{');
        const char *semicolon = strchr(at, ';');
        if (brace == NULL || (semicolon != NULL && semicolon < brace)) continue;
        const char *end = brace + 1;
        size_t depth = 1U;
        while (*end != '\0' && depth != 0U) {
            if (*end == '{') ++depth;
            if (*end == '}') --depth;
            ++end;
        }
        THROW_CHECK(depth == 0U);
        char *body = strndup(brace, (size_t)(end - brace));
        THROW_CHECK(body != NULL);
        return body;
    }
    fprintf(stderr, "missing value cleanup function %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* Observe registration for the value storage, not unrelated call temporaries. */
static void value_cleanup_guard(const char *source, const char *name) {
    char *body = value_cleanup_body(source, name);
    const char *store = strstr(body, "feng_assign(");
    if (store == NULL) store = strstr(body, "void *_ginit_dst");
    THROW_CHECK(store != NULL);
    const char *guard = strstr(body, "_cu__val");
    THROW_CHECK(guard != NULL && guard < store);
    THROW_CHECK(strstr(store, "feng_cleanup_pop();") != NULL);
    /* Construction still initializes and returns the same storage. */
    THROW_CHECK(strstr(body, "_val") != NULL);
    free(body);
}

/* Partial values use the same ownership guard for every initializer form. */
void test_value_literal_partial_cleanup_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module value.cleanup;open type Ref{let id:int;}"
        "@value open type Pair{let ref:Ref;let n:int;}"
        "@value open type Generic<T>{let ref:T;let n:int;}"
        "@value type Scalar{let n:int;}"
        "open spec Getter():int;func quiet():int{return 3;}"
        "type Built{let n:int;func Built(n:int){self.n=n;}}"
        "func fail():int{throw \"failed\";}func echo(n:int):int{return n;}"
        "open func plain(ref:Ref):Pair{return Pair{ref:ref,n:3};}"
        "open func known(ref:Ref):Pair{return Pair{ref:ref,n:quiet()};}"
        "open func internal(ref:Ref):Pair{return Pair{ref:ref,n:try fail() catch{0;}};}"
        "open func caughtUnknown(ref:Ref,f:Getter):Pair{return Pair{ref:ref,n:try f() catch{0;}};}"
        "open func escaping(ref:Ref):Pair{return Pair{ref:ref,n:fail()};}"
        "open func argument(ref:Ref):Pair{return Pair{ref:ref,n:echo(fail())};}"
        "open func ctorArgument(ref:Ref):Pair{return Pair{ref:ref,n:(Built(fail()){}).n};}"
        "open func unknown(ref:Ref,f:Getter):Pair{return Pair{ref:ref,n:f()};}"
        "open func outside(ref:Ref){try (Pair{ref:ref,n:fail()}) catch{}}"
        "open func early(ref:Ref,stop:bool):int{"
        "let pair=Pair{ref:ref,n:if stop{return 9;}else{1;}};return pair.n;}"
        "open func genericKnown<T>(ref:T):Generic<T>{return Generic<T>{ref:ref,n:quiet()};}"
        "open func trivial(){let value=Scalar{n:fail()};}"
        "open func genericUnknown<T>(ref:T,f:Getter):Generic<T>{return Generic<T>{ref:ref,n:f()};}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    const char *guarded[] = {
        "__plain__", "__known__", "__internal__", "__caughtUnknown__", "__genericKnown_G__",
        "__escaping__", "__argument__", "__ctorArgument__", "__unknown__", "__outside__", "__genericUnknown_G__"
    };
    for (size_t i = 0U; i < sizeof guarded / sizeof guarded[0]; ++i)
        value_cleanup_guard(output.c_source, guarded[i]);
    char *early = value_cleanup_body(output.c_source, "__early__");
    const char *branch = strstr(early, "if (");
    const char *release = branch != NULL ? strstr(branch, "feng_aggregate_release(") : NULL;
    THROW_CHECK(release != NULL && strstr(release, "return ") != NULL);
    THROW_CHECK(strstr(early, "_cu__val") != NULL);
    const char *pop = strstr(branch, "feng_cleanup_pop();");
    THROW_CHECK(pop != NULL && pop < release);
    free(early);
    char *trivial = value_cleanup_body(output.c_source, "__trivial__");
    THROW_CHECK(strstr(trivial, "_cu__val") == NULL);
    free(trivial);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
}
