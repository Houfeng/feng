#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Compile complete generated C, checking the stable existing descriptor ABI. */
static void throw_constraint_emit(const char *source, void (*compile_c)(const char *)) {
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s:%u: %s\n%s\n", error.code, error.token.line, error.message, source);
    THROW_CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    THROW_CHECK(strstr(output.c_source, "const FengGenericParamDescriptor *") != NULL);
    THROW_CHECK(strstr(output.c_source, "&(const FengGenericParamDescriptor){") == NULL);
    THROW_CHECK(strstr(output.c_source, "typedef struct FengGenericParamDescriptor") == NULL);
    THROW_CHECK(strstr(output.c_source, "typedef struct FengTypeDescriptor") == NULL);
    THROW_CHECK(strstr(output.c_source, "feng_throw(_throw_payload") != NULL);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    throw_constraint_dispose(&unit);
}

/* Bodies remain shared across all ARC categories and generic entry points. */
void test_throw_constraint_codegen(void (*compile_c)(const char *)) {
    const char *programs[] = {
        "module bodies;enum Status{One}type Pair(i32,string);@value type Value{let s:string;}"
        "type Ref{let n:i32;}type Box<T>{let value:T;}@value type ValBox<T>{let value:T;}"
        "func raise<E:throw>(error:E){throw error;}"
        "func forward<T:throw>(value:T){raise<T>(value);}"
        "func run(flag:bool,a:i8,b:i16,c:i32,d:i64,e:u8,f:u16,g:u32,h:u64,i:f32,j:f64,"
        "s:string,k:Status,p:Pair,v:Value,r:Ref,x:Box<string>,y:ValBox<string>){"
        "forward(flag);forward(a);forward(b);forward(c);forward(d);forward(e);forward(f);"
        "forward(g);forward(h);forward(i);forward(j);forward(s);forward(k);forward(p);"
        "forward(v);forward(r);forward(x);forward(y);}",
        "module owners;func raise<T:throw>(x:T){throw x;}"
        "type Holder<E:throw>{let error:E;func raise(){throw self.error;}"
        "func relay(){raise<E>(self.error);}func other<U:throw>(x:U){throw x;}"
        "static func send<U:throw>(x:U){throw x;}}"
        "@value type ValueHolder<E:throw>{let error:E;func raise(){throw self.error;}}"
        "func use<T:throw>(x:T){let h=Holder<T>{error:x};h.raise();h.relay();h.other<T>(x);"
        "Holder<T>.send<T>(x);let v=ValueHolder<T>{error:x};v.raise();}"
        "func run(){use<i32>(3);use<string>(\"value\");}",
        "module callable;spec Action<T>(value:T):void;spec Supplier<T>():T;"
        "func keep<T>(value:T):T{return value;}func raise<T:throw>(value:T){throw keep<T>(value);}"
        "type Host{func send<T:throw>(value:T){throw value;}static func emit<T:throw>(value:T){throw value;}}"
        "func run(){let f:Action<i32> = raise<i32>;f(2);let h=Host();"
        "let m:Action<string> = h.send<string>;m(\"value\");"
        "let s:Action<i32> = Host.emit<i32>;s(4);}",
        "module closure;spec Action():void;func wrap<T:throw>(value:T):Action{"
        "return (){throw value;};}func run(){let action=wrap<string>(\"value\");action();}",
        "module extensions;type Host{}fit Host{func send<T:throw>(value:T){throw value;}"
        "static func emit<T:throw>(value:T){throw value;}}"
        "func run(){let h=Host();h.send<i32>(1);Host.emit<string>(\"value\");}"
    };
    for (size_t i = 0U; i < sizeof programs / sizeof *programs; ++i) {
        throw_constraint_emit(programs[i], compile_c);
    }
}
