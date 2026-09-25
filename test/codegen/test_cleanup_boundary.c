#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

#include <sys/stat.h>
#include <unistd.h>

/* Read the host compiler's output, retaining it on an assertion failure. */
static char *boundary_read(const char *path) {
    FILE *file = fopen(path, "rb");
    THROW_CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    THROW_CHECK(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1U);
    THROW_CHECK(text != NULL && fread(text, 1U, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    THROW_CHECK(fclose(file) == 0);
    return text;
}

/* Select a native definition rather than a declaration or an inlined call. */
static char *boundary_body(const char *ir, const char *name) {
    for (const char *at = ir; (at = strstr(at, "define ")) != NULL; ++at) {
        const char *line = strchr(at, '\n');
        const char *symbol = strstr(at, name);
        THROW_CHECK(line != NULL);
        if (symbol == NULL || symbol >= line) continue;
        const char *end = strstr(line, "\n}");
        THROW_CHECK(end != NULL);
        char *body = strndup(line, (size_t)(end - line));
        THROW_CHECK(body != NULL);
        return body;
    }
    fprintf(stderr, "missing boundary fixture: %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* Retained boundaries pair their entry and normal exit; empty ones have neither. */
static void boundary_expect(const char *ir, const char *name, bool retained) {
    char *body = boundary_body(ir, name);
    bool has_push = strstr(body, "@feng_frame_push(") != NULL;
    bool has_pop = strstr(body, "@feng_frame_pop(") != NULL;
    if (has_push != retained || has_pop != retained)
        fprintf(stderr, "unexpected cleanup boundary for %s (retained=%d)\n%s\n",
                name, retained, body);
    THROW_CHECK(has_push == retained && has_pop == retained);
    if (!retained) THROW_CHECK(strstr(body, "@feng_frame_release_to(") == NULL);
    free(body);
}

/* Exercise the actual ownership emitters at O0, then validate all optimized
 * and instrumented forms. Scalar, reference and aggregate types are controls,
 * not classification inputs to the optimization. */
void test_cleanup_boundary_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module cleanup.boundary;"
        "open type Ref{let n:int;}"
        "@value open type Flat{let n:int;let m:int;}"
        "@value open type Owned{let ref:Ref;}"
        "open type Pair(int,int);"
        "open spec Action(n:int):int;"
        "open func scalar(n:int):int{if n<0{return 0;}return n+1;}"
        "open func borrowed(r:Ref):int{return r.n;}"
        "open func flat(v:Flat):int{let local=v;return local.n+local.m;}"
        "open func tuple(v:Pair):int{let local=v;let(a,b)=local;return a+b;}"
        "open func propagate(n:int):int{if n<0{throw n;}return n;}"
        "open func loops(n:int):int{var i=0;while i<n{"
        "i+=1;if i==2{continue;}if i==4{break;}}return i;}"
        "open func lazy(n:int):int{return try propagate(n) catch ex:int{ex;};}"
        "open func unmatched(n:int):int{return try propagate(n) catch ex:string{0;};}"
        "open func shortCircuit(n:int):bool{return n>0 && "
        "(try (if n==1{true;}else{throw 3;}) catch{false;});}"
        "open func early(n:int):int{try (if n==0{return 1;}else{n;}) "
        "catch ex:int{return ex;}return 2;}"
        "open func owned(n:int):int{let r=Ref{n:n};return r.n;}"
        "open func aggregate(n:int):int{let v=Owned{ref:Ref{n:n}};return v.ref.n;}"
        "open func guarded(n:int):int{return try (if n>0{let r=Ref{n:n};"
        "propagate(-r.n);}else{0;}) catch ex:int{ex;};}"
        "open func deferred(n:int):int{defer{n;}return n;}"
        "open func generic<T>(value:T):T{let local=value;return local;}"
        "open func closed(n:int):int{return generic<int>(n);}"
        "open func nested(n:int):int{return try (try propagate(n) "
        "catch ex:string{0;}) catch ex:int{ex;};}"
        "open func indirect(f:Action,n:int):int{return f(n);}"
        "open func captured(n:int):Action{return (v:int){return v+n;};}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted);
    compile_c(output.c_source);
    char directory[] = "temp/cleanup-boundary-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char input[256], result[256], command[2048];
    snprintf(input, sizeof input, "%s/generated.c", directory);
    snprintf(result, sizeof result, "%s/generated.ll", directory);
    FILE *file = fopen(input, "wb");
    THROW_CHECK(file != NULL && fputs(output.c_source, file) >= 0 && fclose(file) == 0);
    const unsigned levels[] = {0U, 2U, 3U};
    for (size_t level = 0U; level < sizeof levels / sizeof levels[0]; ++level) {
        for (unsigned sanitized = 0U; sanitized < 2U; ++sanitized) {
            snprintf(command, sizeof command,
                "cc " FENG_TEST_C_EH_FLAGS
                "-Isrc -Isrc/runtime -std=gnu11 -fexceptions -O%u -Werror "
                "%s -S -emit-llvm '%s' -o '%s'", levels[level],
                sanitized ? "-fsanitize=address,undefined -fno-sanitize-recover=all" : "", input, result);
            THROW_CHECK(system(command) == 0);
            char *ir = boundary_read(result);
            THROW_CHECK(strstr(ir, "__llvm_c_eh_") == NULL);
            THROW_CHECK(strstr(ir, "FENG_CLEANUP_BOUNDARY_") == NULL);
            const char *empty[] = {"__scalar__", "__borrowed__", "__flat__", "__tuple__",
                                    "__propagate__", "__loops__"};
            for (size_t i = 0U; i < sizeof empty / sizeof empty[0]; ++i)
                boundary_expect(ir, empty[i], false);
            const char *owned[] = {"__owned__", "__aggregate__", "__deferred__",
                                    "__lazy__", "__guarded__", "__nested__", "__generic_G__"};
            for (size_t i = 0U; i < sizeof owned / sizeof owned[0]; ++i)
                boundary_expect(ir, owned[i], true);
            char *body = boundary_body(ir, "__propagate__");
            THROW_CHECK(strstr(body, "@feng_throw(") != NULL);
            THROW_CHECK(strstr(body, "landingpad ") == NULL);
            free(body);
            if (levels[level] == 0U) {
                body = boundary_body(ir, "__lazy__");
                const char *landing = strstr(body, "landingpad ");
                const char *push = strstr(body, "@feng_try_frame_push(");
                const char *begin = strstr(body, "@feng_exception_catch_begin(");
                THROW_CHECK(landing != NULL && push != NULL && begin != NULL);
                const char *protected_call = strstr(body, "invoke i64 @feng__cleanup__boundary__propagate__");
                THROW_CHECK(protected_call != NULL && protected_call < push && push < begin);
                THROW_CHECK(strstr(push + 1, "@feng_try_frame_push(") == NULL);
                free(body);
                body = boundary_body(ir, "__guarded__");
                push = strstr(body, "@feng_try_frame_push(");
                landing = strstr(body, "landingpad ");
                THROW_CHECK(push != NULL && landing != NULL && push < landing);
                free(body);
            }
            free(ir);
        }
    }
    THROW_CHECK(unlink(input) == 0 && unlink(result) == 0 && rmdir(directory) == 0);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    puts("cleanup boundaries: ownership, lazy catches, exits, O0/O2/O3 and sanitizer IR passed");
}
