#include "../throw_constraint_helpers.h"

/* Check closed actuals at both callable and owner boundaries. */
static void throw_constraint_closed_actuals(void) {
    const char *types[] = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "string", "Status", "Pair", "Value", "Ref", "Box<string>",
        "ValueBox<string>", "AbiRef", "AbiValue", "i32[]", "i32[!]", "i32*", "Named",
        "Callback", "Choice", "Both"};
    for (size_t i = 0U; i < sizeof types / sizeof *types; ++i) {
        for (size_t owner = 0U; owner < 2U; ++owner) {
            char source[2048];
            snprintf(source, sizeof source,
                "module actuals;enum Status{One}type Pair(i32,string);"
                "@value type Value{let s:string;}type Ref{let n:i32;}"
                "type Box<T>{let value:T;}@value type ValueBox<T>{let value:T;}"
                "@abi type AbiRef{let n:i32;}@value @abi type AbiValue{let n:i32;}"
                "spec Named{let n:i32;}spec Callback():void;spec Choice:i32|string;"
                "spec Other{let s:string;}spec Both:Named&Other;"
                "func raise<E:throw>(error:E){throw error;}type Holder<E:throw>{let error:E;}"
                "%s", "");
            size_t length = strlen(source);
            if (owner) snprintf(source + length, sizeof source - length, "func run(value:Holder<%s>){}", types[i]);
            else snprintf(source + length, sizeof source - length, "func run(value:%s){raise(value);raise<%s>(value);}", types[i], types[i]);
            ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL,
                i < 20U ? NULL : owner ? "AE0710" : "AE0512");
            throw_constraint_dispose(&unit);
        }
    }
}

/* Open proof, owner/member scope, overload filtering and retained prohibitions. */
void test_throw_constraint_semantics(void) {
    throw_constraint_closed_actuals();
    const char *positive[] = {
        "func raise<E:throw>(x:E){throw x;}func pass<T:throw>(x:T){raise<T>(x);raise(x);}",
        "type Owner<T:throw>{var value:T;func raise(){throw self.value;}func method<U:throw>(x:U){throw x;}}",
        "@value type Owner<T:throw>{var value:T;func raise(){throw self.value;}}",
        "type Owner<T:throw>{func Owner(value:T){throw value;}}",
        "func keep<T>(x:T):T{return x;}func raise<E:throw>(x:E){throw keep<E>(x);}",
        "type Holder<E:throw>{let value:E;}func make<T:throw>(x:T):Holder<T>{return Holder<T>{value:x};}",
        "spec Parent<E:throw>{}spec Child<T:throw>:Parent<T>{}",
        "func choose<T:throw>(x:T):i32{return 1;}func choose(x:i32[]):i32{return 2;}func run(x:i32[]):i32{return choose(x);}",
        "spec Action():void;func check<E:throw>(x:E){let action:Action=(){throw x;};action();}",
        "type Item{}fit Item{func raise<E:throw>(x:E){throw x;}}",
        "spec Action<E:throw>(value:E):void;func run(value:Action<string>){}",
        "func raise<E:throw>(xs:E[]){throw xs[0];}",
        "func raise<E:throw>(first:E,xs:E...){throw xs[0];}",
        "func raise<E:throw>(x:E,n:i32){if n==0{throw x;}raise<E>(x,n-1);}",
        "func raise<E:throw>(x:E):i32{throw x;}@abi func recover():i32{return try raise(1) catch{0;};}",
        "func pure<E:throw>(x:E){}@abi func run(){pure(1);}",
        "type Owner<T:throw>{let x:T;}fit Owner<T>{func raise(){throw self.x;}}"
    };
    for (size_t i = 0U; i < sizeof positive / sizeof *positive; ++i) {
        char source[2048]; snprintf(source, sizeof source, "module positive;%s", positive[i]);
        ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
        throw_constraint_dispose(&unit);
    }
    const char *negative[] = {
        "func raise<E:throw>(x:E){throw x;}func bad<T>(x:T){raise(x);}",
        "spec Named{}func raise<E:throw>(x:E){throw x;}func bad<T:Named>(x:T){raise<T>(x);}",
        "func bad<T>(x:T){throw x;}",
        "spec Named{}func bad<T:Named>(x:T){throw x;}",
        "func bad<T:throw>(x:T){try bad(x) catch error:T{}}",
        "type Box<T>{let value:T;}func bad<T:throw>(x:Box<T>){throw x;}",
        "type Box<T>{let value:T;}func raise<E:throw>(x:E){throw x;}func bad<T:throw>(x:Box<T>){raise(x);}",
        "type Holder<E:throw>{let value:E;}func bad<T>(x:Holder<T>){}",
        "spec Parent<E:throw>{}spec Child<T>:Parent<T>{}",
        "func bad<T:throw>(x:T){defer{throw x;}}",
        "func bad<T:throw>(x:T){x.missing();}",
        "func bad<T:throw,T>(){}",
        "type Owner<T:throw>{func bad<T:throw>(x:T){throw x;}}",
        "type Holder<E:throw>{}func bad(x:Holder<void>){}",
        "func raise<E:throw>(x:E){throw x;}@abi func bad(){raise(1);}",
        "type Owner{func raise<E:throw>(x:E){throw x;}}func bad(xs:i32[]){Owner().raise(xs);}",
        "type Owner{static func raise<E:throw>(x:E){throw x;}}func bad(xs:i32[]){Owner.raise<i32[]>(xs);}",
        "spec Action<T>(x:T):void;func raise<E:throw>(x:E){throw x;}func bad(){let f:Action<i32[]> = raise<i32[]>;}",
        "spec Action<E:throw>(x:E):void;func bad(x:Action<i32[]>){}",
        "spec Surface{}type Source:Surface{@mixable open static func raise<E:throw>(s:Surface,x:E){throw x;}}type Mixed:Surface{...:Source;}func bad(x:i32[]){Mixed().raise(x);}"
    };
    const char *codes[] = {"AE0512", "AE0512", "AE0076", "AE0076", "AE0179", "AE0076",
        "AE0512", "AE0710", "AE0710", "AE1502", "AE0306", "AE1017", "AE1017", "AE0502",
        "AE1313", "AE0512", "AE0512", "AE0522", "AE0710", "AE0512"};
    for (size_t i = 0U; i < sizeof negative / sizeof *negative; ++i) {
        char source[2048]; snprintf(source, sizeof source, "module negative;%s", negative[i]);
        ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, codes[i]);
        throw_constraint_dispose(&unit);
    }
}
