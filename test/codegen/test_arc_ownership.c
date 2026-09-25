#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

#include <sys/stat.h>
#include <unistd.h>

/* Keep actual lowered IR available when an ownership contract fails. */
static char *arc_read(const char *path) {
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

/* Count operations in exactly one function definition, never its declaration
 * or unrelated constructors, thunks and exceptional cleanup helpers. */
static size_t arc_calls(const char *ir, const char *name, const char *operation) {
    for (const char *at = ir; (at = strstr(at, "define ")) != NULL; ++at) {
        const char *line = strchr(at, '\n');
        const char *symbol = strstr(at, name);
        THROW_CHECK(line != NULL);
        if (symbol == NULL || symbol >= line) continue;
        const char *end = strstr(line, "\n}");
        THROW_CHECK(end != NULL);
        size_t count = 0U;
        for (const char *call = line; (call = strstr(call, operation)) != NULL && call < end;
             call += strlen(operation)) ++count;
        return count;
    }
    fprintf(stderr, "missing ARC function: %s\n", name);
    THROW_CHECK(false);
    return 0U;
}

/* Every positive also requires unchanged ownership establishment and cleanup
 * structure. A zero count alone could otherwise hide omitted object creation. */
static void arc_expect(const char *ir, const char *name, const char *operation,
                        size_t expected) {
    size_t actual = arc_calls(ir, name, operation);
    if (actual != expected)
        fprintf(stderr, "ARC %s: %s expected %zu, got %zu\n", name, operation, expected, actual);
    THROW_CHECK(actual == expected);
}

/* Clang's frontend exposes C block lifetimes before optimizer motion. Every
 * conditional call guard must still be alive at the protected invocation;
 * declaring its node inside the representation branch violates this contract. */
static void arc_expect_guard_lifetime(const char *ir) {
    const char *function = NULL;
    for (const char *at = ir; (at = strstr(at, "define ")) != NULL; ++at) {
        const char *line = strchr(at, '\n');
        const char *name = strstr(at, "__guardedField_G__");
        THROW_CHECK(line != NULL);
        if (name != NULL && name < line) { function = line; break; }
    }
    THROW_CHECK(function != NULL);
    const char *end = strstr(function, "\n}");
    THROW_CHECK(end != NULL);
    char *body = strndup(function, (size_t)(end - function));
    THROW_CHECK(body != NULL);
    const char *invocation = strstr(body, "@feng__arc__ownership__firstArgument_G__");
    THROW_CHECK(invocation != NULL);
    unsigned guards = 0U;
    for (const char *push = body; (push = strstr(push, "@feng_cleanup_push(")) != NULL; ++push) {
        const char *line = strchr(push, '\n');
        const char *node = strstr(push, "%_cu__call_subject");
        if (node == NULL || (line != NULL && node > line)) continue;
        const char *comma = strchr(node, ',');
        THROW_CHECK(comma != NULL && (line == NULL || comma < line));
        char *identity = strndup(node, (size_t)(comma - node));
        THROW_CHECK(identity != NULL && push < invocation);
        const char *allocation = strstr(body, identity);
        THROW_CHECK(allocation != NULL && allocation < push);
        THROW_CHECK(strncmp(allocation + strlen(identity), " = alloca ", 10U) == 0);
        unsigned starts = 0U, ends = 0U;
        for (const char *lifetime = body; (lifetime = strstr(lifetime, "@llvm.lifetime.")) != NULL;
             ++lifetime) {
            const char *next_line = strchr(lifetime, '\n');
            const char *operand = strstr(lifetime, identity);
            if (operand == NULL || (next_line != NULL && operand > next_line)) continue;
            if (strncmp(lifetime, "@llvm.lifetime.start.", 21U) == 0) {
                THROW_CHECK(lifetime < push);
                ++starts;
            } else if (strncmp(lifetime, "@llvm.lifetime.end.", 19U) == 0) {
                THROW_CHECK(lifetime > invocation);
                ++ends;
            }
        }
        /* An alloca without lifetime intrinsics is alive for the function.
         * Explicit block lifetimes must instead cover the complete call. */
        THROW_CHECK((starts == 0U && ends == 0U) || (starts > 0U && ends > 0U));
        free(identity);
        ++guards;
    }
    THROW_CHECK(guards > 0U);
    free(body);
}

/* Build successively larger sets of open constraint instances. Each method
 * switches the current generic domain while the outer constraints stay live. */
void test_arc_constraint_context_codegen(void (*compile_c)(const char *)) {
    const unsigned counts[] = {1U, 8U, 33U};
    for (size_t iteration = 0U; iteration < sizeof counts / sizeof counts[0]; ++iteration) {
        size_t capacity = 2048U + (size_t)counts[iteration] * 768U;
        char *source = calloc(capacity, 1U);
        THROW_CHECK(source != NULL);
        size_t used = (size_t)snprintf(source, capacity,
            "module arc.context;spec Reader<A>{func read():A;}"
            "spec Mapper<A>(value:A):A;"
            "type Reading:Reader<i64>{func read():i64{return 9000000001;}}"
            "fit T[]{static func read<U:Reader<T>>(value:U):T{return value.read();}"
            "static func map<U:Mapper<T>>(value:U,arg:T):T{return value(arg);}}"
            "type Context<A>{func nested<B:Reader<A>>(value:B):A{return A[].read<B>(value);}}");
        for (unsigned i = 0U; i < counts[iteration]; ++i) {
            used += (size_t)snprintf(source + used, capacity - used,
                "func relay%u<X%u,Y%u:Reader<X%u>>(value:Y%u):X%u{return X%u[].read<Y%u>(value);}"
                "func apply%u<X%u,Y%u:Mapper<X%u>>(value:Y%u,arg:X%u):X%u{return X%u[].map<Y%u>(value,arg);}",
                i,i,i,i,i,i,i,i, i,i,i,i,i,i,i,i,i);
            THROW_CHECK(used < capacity);
        }
        used += (size_t)snprintf(source + used, capacity - used,
            "open func run():i64{let value=Reading();let map:Mapper<i64> = (n:i64){return n;};"
            "var total=Context<i64>().nested<Reading>(value);");
        for (unsigned i = 0U; i < counts[iteration]; ++i) {
            used += (size_t)snprintf(source + used, capacity - used,
                "total+=relay%u<i64,Reading>(value);total+=apply%u<i64,Mapper<i64>>(map,3);", i, i);
            THROW_CHECK(used < capacity);
        }
        THROW_CHECK(snprintf(source + used, capacity - used, "return total;}") > 0);
        ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
        FengCodegenOutput output = {0};
        FengCodegenError error = {0};
        bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                                 NULL, &output, &error);
        if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
        THROW_CHECK(emitted && output.c_source != NULL);
        compile_c(output.c_source);
        feng_codegen_output_free(&output);
        feng_codegen_error_free(&error);
        throw_constraint_dispose(&unit);
        free(source);
    }
    puts("generic constraint identities: inactive contexts and repeated spec growth passed");
}

/* Validate whole-value ownership sources, exit-local decisions, conservative
 * dynamic/cyclic cases and both concrete/generic callable return conventions. */
void test_arc_ownership_codegen(void (*compile_c)(const char *)) {
    test_arc_constraint_context_codegen(compile_c);
    const char *source =
        "open module arc.ownership;"
        "open type Ref{let n:int;}"
        "open type Cycle{var next:Cycle[];}"
        "@value open type Value{let ref:Ref;}"
        "@value open type Mutable{var ref:Ref;}"
        "open type Pair(Ref,int);"
        "open type Other(Ref,int);"
        "open spec View{let n:int;}"
        "@value open type ViewValue:View{let n:int;let ref:Ref;}"
        "open spec Thunk():Ref;"
        "open func localReturn(n:int):Ref{let value=Ref{n:n};return value;}"
        "open func aliasReturn(n:int):Ref{let a=Ref{n:n};let b=a;let c=b;return c;}"
        "open func parameter(value:Ref):Ref{return value;}"
        "open func copiedParameter(value:Ref):Ref{let copy=value;return copy;}"
        "open func conditional(n:int):Ref{let value=Ref{n:n};if n>0{return value;}"
        "let other=Ref{n:0};return other;}"
        "open func chosen(n:int):Ref{let value=if n>0{let a=Ref{n:n};a;}"
        "else{let b=Ref{n:0};b;};return value;}"
        "open func matching(n:int):Ref{return match n{1{let a=Ref{n:1};a;}"
        "else{let b=Ref{n:2};b;}};}"
        "open func protected(n:int):Ref{return try (if n>0{let a=Ref{n:n};a;}"
        "else{throw n;}) catch ex:int{Ref{n:ex};};}"
        "open func arrayReturn(n:int):int[]{let value=[n,n+1];return value;}"
        "open func stringReturn(s:string):string{let value=s+\"!\";return value;}"
        "open func aggregateReturn(n:int):Value{let value=Value{ref:Ref{n:n}};return value;}"
        "open func mutableReturn(value:Mutable):Mutable{let copy=value;return copy;}"
        "open func tupleConvert(n:int):Other{let a:Pair=(Ref{n:n},n);let b:Other=(Other)a;return b;}"
        "open func valueBox(n:int):View{let v=ViewValue{n:n,ref:Ref{n:n}};let view:View=v;return view;}"
        "open func deferred(n:int):Ref{let value=Ref{n:n};defer{value.n;}return value;}"
        "open func changing(n:int):Ref{var value=Ref{n:n};defer{value=Ref{n:2};}return value;}"
        "open func cycle(value:Cycle):Cycle{let copy=value;return copy;}"
        "open func dynamic(value:View):View{let copy=value;return copy;}"
        "open func callable(value:Thunk):Thunk{let copy=value;return copy;}"
        "open func generic<T>(value:T):T{let copy=value;return copy;}"
        "open func genericConcrete<T>(unused:T,n:int):Ref{let value=Ref{n:n};return value;}"
        "open func instantiate(n:int):Ref{let value=Ref{n:n};let copy=generic<Ref>(value);"
        "return genericConcrete<Ref>(copy,n);}"
        "open func invokeThunk():Thunk{return (){let value=Ref{n:3};return value;};}"
        "open func readArgument(value:Ref,extra:int):int{return value.n+extra;}"
        "open func stableArgument(value:Ref):int{return readArgument(value,1);}"
        "open func ownedArgument(n:int):int{return readArgument(Ref{n:n},1);}"
        "open func reboundArgument(n:int):int{var value=Ref{n:n};"
        "return readArgument(value,if true{value=Ref{n:n+1};1;}else{0;});}"
        "open func failedArgument(n:int):int{var value=Ref{n:n};"
        "return try readArgument(value,if n>0{value=Ref{n:2};throw 3;}else{0;})"
        "catch ex:int{ex;};}"
        "open func firstArgument<T>(value:T,extra:int):T{return value;}"
        "open type ArgumentHolder<T>{open var value:T;}"
        "open func guardedField<T>(holder:ArgumentHolder<T>):T{return firstArgument<T>(holder.value,1);}"
        "open func guardedFieldInstance(n:int):Ref{return guardedField<Ref>(ArgumentHolder<Ref>{value:Ref{n:n}});}"
        "open func reboundGeneric<T>(first:T,second:T):T{var value=first;"
        "return firstArgument<T>(value,if true{value=second;1;}else{0;});}"
        "open func instantiateRebound(value:Ref):Ref{return reboundGeneric<Ref>(value,value);}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted);
    compile_c(output.c_source);
    char directory[] = "temp/arc-ownership-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char input[256], result[256], command[2048];
    snprintf(input, sizeof input, "%s/generated.c", directory);
    snprintf(result, sizeof result, "%s/generated.ll", directory);
    FILE *file = fopen(input, "wb");
    THROW_CHECK(file != NULL && fputs(output.c_source, file) >= 0 && fclose(file) == 0);
    snprintf(command, sizeof command,
        "cc " FENG_TEST_C_EH_FLAGS
        "-Isrc -Isrc/runtime -std=gnu11 -fexceptions -O1 -Werror "
        "-Xclang -disable-llvm-passes -fno-discard-value-names "
        "-S -emit-llvm '%s' -o '%s'", input, result);
    THROW_CHECK(system(command) == 0);
    char *frontend_ir = arc_read(result);
    arc_expect_guard_lifetime(frontend_ir);
    free(frontend_ir);
    const unsigned levels[] = {0U, 2U, 3U};
    for (size_t level = 0U; level < sizeof levels / sizeof levels[0]; ++level) {
        for (unsigned sanitized = 0U; sanitized < 2U; ++sanitized) {
            snprintf(command, sizeof command,
                "cc " FENG_TEST_C_EH_FLAGS
                "-Isrc -Isrc/runtime -std=gnu11 -fexceptions -O%u -Werror "
                "%s -S -emit-llvm '%s' -o '%s'", levels[level],
                sanitized ? "-fsanitize=address,undefined -fno-sanitize-recover=all" : "", input, result);
            THROW_CHECK(system(command) == 0);
            char *ir = arc_read(result);
            THROW_CHECK(strstr(ir, "__llvm_c_eh_") == NULL);
            if (level == 0U) {
                const char *transferred[] = {"__localReturn__", "__aliasReturn__", "__chosen__",
                    "__matching__", "__protected__", "__arrayReturn__", "__stringReturn__"};
                for (size_t i = 0U; i < sizeof transferred / sizeof transferred[0]; ++i)
                    arc_expect(ir, transferred[i], "@feng_retain(", 0U);
                arc_expect(ir, "__localReturn__", "@feng_object_new(", 1U);
                arc_expect(ir, "__localReturn__", "@feng_cleanup_push(", 1U);
                arc_expect(ir, "__localReturn__", "@feng_release(", 0U);
                arc_expect(ir, "__aliasReturn__", "@feng_cleanup_push(", 1U);
                arc_expect(ir, "__parameter__", "@feng_retain(", 1U);
                arc_expect(ir, "__copiedParameter__", "@feng_retain(", 1U);
                arc_expect(ir, "__conditional__", "@feng_retain(", 0U);
                arc_expect(ir, "__conditional__", "@feng_release(", 1U);
                arc_expect(ir, "__aggregateReturn__", "@feng_aggregate_retain(", 0U);
                arc_expect(ir, "__aggregateReturn__", "@feng_aggregate_release(", 0U);
                arc_expect(ir, "__deferred__", "@feng_retain(", 1U);
                arc_expect(ir, "__changing__", "@feng_retain(", 1U);
                arc_expect(ir, "__cycle__", "@feng_retain(", 2U);
                arc_expect(ir, "__callable__", "@feng_retain(", 2U);
                arc_expect(ir, "__dynamic__", "@feng_aggregate_retain(", 2U);
                arc_expect(ir, "__mutableReturn__", "@feng_aggregate_retain(", 2U);
                arc_expect(ir, "__tupleConvert__", "@feng_aggregate_retain(", 0U);
                arc_expect(ir, "__tupleConvert__", "@feng_retain(", 1U);
                arc_expect(ir, "__valueBox__", "@feng_aggregate_retain(", 1U);
                arc_expect(ir, "__genericConcrete_G__", "@feng_retain(", 0U);
                arc_expect(ir, "__generic_G__", "@feng_retain(", 2U);
                /* A05: stable/owned operands stay free of extra retains; a
                 * borrowed identity that can be rebound requires one guard. */
                arc_expect(ir, "__stableArgument__", "@feng_retain(", 0U);
                arc_expect(ir, "__ownedArgument__", "@feng_retain(", 0U);
                arc_expect(ir, "__reboundArgument__", "@feng_retain(", 1U);
                THROW_CHECK(arc_calls(ir, "__failedArgument__", "@feng_cleanup_push(") > 0U);
                THROW_CHECK(arc_calls(ir, "__reboundGeneric_G__", "@feng_retain(") > 0U);
            }
            free(ir);
        }
    }
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    fprintf(stdout, "ARC ownership: exact owners, exits, stable borrows and conservative paths passed\n");
}
