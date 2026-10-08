#include "../friend_generic_helpers.h"
#include "codegen/codegen.h"

/* Repeated partially open calls share the provider ABI for ordinary, value and
 * variadic constructors, including operands that still use the caller's type. */
void test_friend_shared_constructor_prototypes(void (*compile_c)(const char *)) {
    const char *source = "open module shared_constructor;"
        "open type Marker<T>{}"
        "open type Box<F,T>{let value:T;func Box(value:T){self.value=value;}}"
        "@value open type ValueBox<F,T>{let value:T;func ValueBox(value:T){self.value=value;}}"
        "open type Values<F,T>{let items:T[];func Values(items:T...){self.items=items;}}"
        "open func construct<T>(value:T):int{"
        "let a=Box<Marker<T>,int>(1);let b=Box<Marker<T>,int>(2);"
        "let c=Box<Marker<T>,T>(value);let d=Box<Marker<T>,T>(value);"
        "let e=ValueBox<Marker<T>,int>(3);let f=ValueBox<Marker<T>,int>(4);"
        "let g=Values<Marker<T>,int>(5,6);let h=Values<Marker<T>,int>(7,8);"
        "return a.value+b.value+e.value+f.value+g.items[0]+h.items[0];}"
        "open func run():int{return construct<string>(\"owned\");}";
    FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
        NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    FRIEND_CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    friend_generic_dispose(&unit);
    puts("shared constructor prototypes: partial owners, value and variadic calls passed");
}
