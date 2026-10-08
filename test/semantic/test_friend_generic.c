#include "../friend_generic_helpers.h"

/* Cover direct, inferred, transitive, recursive and owner-bound uses. */
void test_friend_generic_semantics(void) {
    const char *positive[] = {
        "type Vault<F>{@friend(F) seal let x:int=7;}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader{func read(v:Vault<Reader>):int{return v.x;}}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader<T>{func read(v:Vault<Reader<T>>):int{return v.x;}}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader{}fit Reader{func read(v:Vault<Reader>):int{return v.x;}}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader{}func forward<T>(){let v=Vault<T>();}func run(){forward<Reader>();}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader{}func forward<T>(){let v=Vault<T>();}func relay<U>(){forward<U>();}func run(){relay<Reader>();}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Box<T>{}func recurse<T>(){let v=Vault<T>();recurse<Box<T>>();}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader{}type Outer<T>{let v:Vault<T>;}func run(x:Outer<Reader>){}",
        "type Vault<F>{@friend(F) seal let x:int=7;}func forward<T>(){let v=Vault<T>();}",
        "type Vault<F>{@friend(F,F) @friend(F) seal let x:int=7;}type Reader{func read(v:Vault<Reader>):int{return v.x;}}",
        "type Vault<F>{@friend(F) seal let x:int=7;}type Reader<T>{let x=Vault<Reader<T>>().x;func Reader(){let v=Vault<Reader<T>>().x;}func ~Reader(){let v=Vault<Reader<T>>().x;}}",
        "spec Parent<F>{@friend(F) seal func read():int;}spec Child<T>:Parent<T>{func visible():int;}type Reader{func read(v:Child<Reader>):int{return v.read();}}",
        "type Target<F>{}fit Target<F>{@friend(F) seal func read():int{return 7;}}type Reader{func read(v:Target<Reader>):int{return v.read();}}",
        "fit T[]{@friend(T) seal func read():int{return 7;}}type Reader{func read(v:Reader[]):int{return v.read();}}"
    };
    for (size_t i = 0U; i < sizeof positive / sizeof *positive; ++i) {
        char source[2048];
        snprintf(source, sizeof source, "module positive;%s", positive[i]);
        FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
        friend_generic_dispose(&unit);
    }
    const struct { const char *body; const char *code; } negative[] = {
        {"func run(x:Vault<int>){}", "AE1336"},
        {"func run(){let x=Vault<int>();}", "AE1336"},
        {"spec S{}func run(x:Vault<S>){}", "AE1336"},
        {"enum E{A}func run(x:Vault<E>){}", "AE1336"},
        {"func run(x:Vault<Reader[]>){}", "AE1336"},
        {"func run(x:Vault<Reader*>){}", "AE1336"},
        {"func forward<T>(){let v=Vault<T>();}func run(){forward<int>();}", "AE1336"},
        {"func forward<T>(x:T){let v=Vault<T>();}func run(){forward(1);}", "AE1336"},
        {"func forward<T>(){let v=Vault<T>();}func relay<U>(){forward<U>();}func run(){relay<int>();}", "AE1336"},
        {"type Outer<T>{let v:Vault<T>;}func run(x:Outer<int>){}", "AE1336"},
        {"type Outer<T>{func act(){let v=Vault<T>();}}func run(x:Outer<int>){x.act();}", "AE1336"},
        {"type Outer{static func act<T>(){let v=Vault<T>();}}func run(){Outer.act<int>();}", "AE1336"},
        {"func loop<T,U>(){let v=Vault<T>();loop<U,int>();}func run(){loop<Reader,Reader>();}", "AE1336"},
        {"spec Action():void;func forward<T>(){let v=Vault<T>();}func run(){let f:Action=forward<int>;}", "AE1336"},
        {"type Target<T>{}fit Target<T>{@friend(T) seal func hidden(){}}func run(v:Target<int>){}", "AE1336"},
        {"fit T[]{@friend(T) seal func hidden(){}}func run(v:int[]){}", "AE1336"},
        {"fit T[]{@friend(T) seal func hidden(){}}func create<T>(){let xs=T[:1];}func run(){create<int>();}", "AE1336"},
        {"fit T[]{@friend(T) seal func hidden(){}}func run(){let xs=int[:1];}", "AE1336"},
        {"type Other{func read(v:Vault<Reader>):int{return v.x;}}", "AE0308"}
    };
    for (size_t i = 0U; i < sizeof negative / sizeof *negative; ++i) {
        char source[2048];
        snprintf(source, sizeof source, "module negative;type Vault<F>{@friend(F) seal let x:int=7;}type Reader{}%s", negative[i].body);
        FriendGenericUnit unit = friend_generic_analyze(source, NULL, negative[i].code, NULL);
        friend_generic_dispose(&unit);
    }
    const char *nested_scope = "module owner_scope;type Box<T>{}type Owner{"
        "@friend(Box<U>) seal func hidden<U>(){}}";
    FriendGenericUnit scoped = friend_generic_analyze(nested_scope, NULL, "AE1013", "U");
    friend_generic_dispose(&scoped);
    const char *type_location =
        "module locations;\n"
        "type Vault<F> {\n"
        "  @friend(F) seal let value:int=0;\n"
        "}\n"
        "func invalid(value:Vault<i32>) {}\n";
    FriendGenericUnit type_error = friend_generic_analyze_at(
        type_location, NULL, "AE1336", "Vault", 5U, 20U,
        "instantiated @friend argument 'i32'", 3U, 3U);
    friend_generic_dispose(&type_error);
    const char *call_location =
        "module locations;\n"
        "type Vault<F> {\n"
        "  @friend(F) seal let value:int=0;\n"
        "}\n"
        "func forward<T>() { let value=Vault<T>(); }\n"
        "func invalid() {\n"
        "  forward<i32>();\n"
        "}\n";
    FriendGenericUnit call_error = friend_generic_analyze_at(
        call_location, NULL, "AE1336", "forward", 7U, 3U,
        "instantiated @friend argument 'i32'", 3U, 3U);
    friend_generic_dispose(&call_error);
    puts("friend generic semantic cases passed");
}
