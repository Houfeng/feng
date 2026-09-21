#include "../throw_constraint_helpers.h"
#include "semantic/exception_effects.h"

/* Locate a declaration by its source name without relying on registration order. */
static const FengDecl *effects_function(const FengProgram *program, const char *name) {
    for (size_t i = 0U; i < program->declaration_count; ++i) {
        const FengDecl *decl = program->declarations[i];
        if (decl->kind != FENG_DECL_FUNCTION)
            continue;
        FengSlice candidate = decl->as.function_decl.name;
        if (candidate.length == strlen(name) && memcmp(candidate.data, name, candidate.length) == 0)
            return decl;
    }
    return NULL;
}

/* Assert the public semantic summary rather than the implementation's term layout. */
static void effects_expect(const char *body, const char *name, const char *expected) {
    char source[16384];
    snprintf(source, sizeof source, "module effects;%s", body);
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    const FengDecl *decl = effects_function(unit.program, name);
    THROW_CHECK(decl != NULL);
    const FengExceptionTemplate *summary = feng_semantic_exception_template(unit.analysis, decl);
    THROW_CHECK(summary != NULL);
    char *types = feng_exception_format(summary->graph, summary->effects);
    THROW_CHECK(types != NULL);
    if (strcmp(types, expected) != 0)
        fprintf(stderr, "%s: expected '%s', got '%s'\n%s\n", name, expected, types, source);
    THROW_CHECK(strcmp(types, expected) == 0);
    free(types);
    throw_constraint_dispose(&unit);
}

/* Basic composition, branch conservatism, catch ownership and precise rethrow. */
static void effects_control_flow(void) {
    effects_expect("func run(){}", "run", "none");
    effects_expect("func run(){throw 1;}", "run", "i64");
    effects_expect("func run(){if false{throw \"x\";}}", "run", "string");
    effects_expect("func fail(){throw \"x\";}func run(){fail();}", "run", "string");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch ex:string{}}", "run", "none");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch ex:i32{}}", "run", "string");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch{}}", "run", "none");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch{throw;}}", "run", "string");
    effects_expect("func fail(flag:bool){if flag{throw \"x\";}throw 1;}"
                   "func run(){try fail(true) catch ex:string{} catch{throw;}}",
                   "run", "i64");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch ex:i32{throw 1;}}", "run", "string");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch{throw 1;}}", "run", "i64");
    effects_expect("func fail(){throw \"x\";}func run(){try (try fail() catch{throw;}) catch ex:string{}}",
                   "run", "none");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch ex:string{} fail();}", "run",
                   "string");
    effects_expect("func fail(){throw \"x\";}func run(){defer{fail();}}", "run", "string");
    effects_expect("func run(){if false{throw \"x\";}else{throw true;}throw \"duplicate\";}", "run",
                   "bool, string");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch e:string{throw e;} catch{}}", "run",
                   "string");
    effects_expect("func fail(){throw \"x\";}func again(){throw true;}"
                   "func run(){try fail() catch{again();}}",
                   "run", "bool");
    effects_expect("func fail(){throw \"x\";}func again(){throw true;}"
                   "func run(){try fail() catch{try again() catch e:bool{} throw;}}",
                   "run", "string");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch{if false{throw;}}}", "run",
                   "string");
    effects_expect("func fail(){throw \"x\";}func run(){try fail() catch{try fail() catch{}}}", "run",
                   "none");
    effects_expect("func fail():bool{throw true;}func run(){while fail(){}}", "run", "bool");
    effects_expect("func run(){while false{throw \"x\";}}", "run", "string");
    effects_expect("func fail():i32{throw \"x\";}func run(){for var i:i32=0;i<0;i=fail(){}}", "run",
                   "string");
    effects_expect("func run(x:bool){match x{true{throw \"x\";}else{throw true;}}}", "run", "bool, string");
}

/* Rebind open templates per call, keeping catches symbolic until substitution. */
static void effects_generic_domains(void) {
    const char *prefix = "func fail<T:throw>(x:T){throw x;}";
    char source[8192];
    snprintf(source, sizeof source, "%sfunc run<T:throw>(x:T){fail(x);}", prefix);
    effects_expect(source, "run", "T");
    snprintf(source, sizeof source, "%sfunc pass<T:throw>(x:T){fail<T>(x);}func run(){pass(\"x\");}", prefix);
    effects_expect(source, "run", "string");
    snprintf(source, sizeof source,
             "%sfunc pass<T:throw>(x:T){try fail(x) catch e:string{}}func run(){pass(\"x\");}", prefix);
    effects_expect(source, "run", "none");
    snprintf(source, sizeof source,
             "%sfunc pass<T:throw>(x:T){try fail(x) catch e:string{}}func run(){pass(1);}", prefix);
    effects_expect(source, "run", "i64");
    snprintf(source, sizeof source,
             "%sfunc pass<T:throw>(x:T){try fail(x) catch e:string{throw 1;}}func run(){pass(\"x\");}",
             prefix);
    effects_expect(source, "run", "i64");
    snprintf(source, sizeof source,
             "%sfunc pass<T:throw>(x:T){try fail(x) catch e:string{throw 1;}}func run(){pass(true);}",
             prefix);
    effects_expect(source, "run", "bool");
    effects_expect("type Box<T:throw>{let value:T;func fail(){throw self.value;}}"
                   "func run(x:Box<string>){x.fail();}",
                   "run", "string");
    effects_expect("type Box<T:throw>{func fail<U:throw>(x:U){throw x;}}"
                   "func run(x:Box<string>){x.fail(true);}",
                   "run", "bool");
    effects_expect("type Box<T:throw>{let value:T;}fit Box<T>{func fail(){throw self.value;}}"
                   "func run(x:Box<string>){x.fail();}",
                   "run", "string");
    effects_expect("type Box<T:throw>{let value:T;func fail<U:throw>(x:U){"
                   "if false{throw self.value;}throw x;}}func run(x:Box<string>){x.fail(true);}",
                   "run", "bool, string");
    effects_expect("type Box<T:throw>{let value:T;func fail<U:throw>(x:U){"
                   "if false{throw self.value;}throw x;}}func run(x:Box<bool>){"
                   "try x.fail(\"x\") catch e:bool{}}",
                   "run", "string");
    effects_expect("func first<T:throw>(x:T){throw x;}func second<T:throw>(x:T){throw x;}"
                   "func run(){try first(\"x\") catch{} second(true);}",
                   "run", "bool");
    const char *orders[] = {"func before(){fail(true);}func after(){try fail(\"x\") catch e:string{}}",
                            "func after(){try fail(\"x\") catch e:string{}}func before(){fail(true);}"};
    for (size_t i = 0U; i < 2U; ++i) {
        snprintf(source, sizeof source, "%s%s", prefix, orders[i]);
        effects_expect(source, "before", "bool");
        effects_expect(source, "after", "none");
        effects_expect(source, "fail", "T");
    }
    effects_expect("func raise<T:throw>(x:T){throw x;}"
                   "func a<T:throw>(x:T){try raise(x) catch e:string{} catch{throw;}}"
                   "func b<U:throw>(x:U){a<U>(x);}func run(){b(true);}",
                   "run", "bool");
    effects_expect("func raise<T:throw>(x:T){throw x;}"
                   "func a<T:throw>(x:T){try raise(x) catch e:string{} catch{throw;}}"
                   "func b<U:throw>(x:U){a<U>(x);}func run(){b(\"x\");}",
                   "run", "none");
    /* Stage one does not relax the existing open nominal payload restriction. */
    ThrowConstraintUnit rejected = throw_constraint_analyze(
        "module effects;type Error<T>{let value:T;}func make<T>(x:Error<T>){throw x;}", NULL, "AE0076");
    throw_constraint_dispose(&rejected);
}

/* Invocation provenance is distinct from closure creation and returned closures. */
static void effects_callable_values(void) {
    effects_expect("spec Action():void;func fail(){throw \"x\";}func run(){let f:Action=fail;}", "run",
                   "none");
    effects_expect("spec Action():void;func fail(){throw \"x\";}func run(){let f:Action=fail;f();}", "run",
                   "string");
    effects_expect("spec Action():void;func run(){let f:Action=(){throw \"x\";};f();}", "run", "string");
    effects_expect(
        "spec Action():void;func invoke(f:Action){f();}func fail(){throw \"x\";}func run(){invoke(fail);}",
        "run", "string");
    effects_expect(
        "spec Action():void;func make():Action{return (){throw \"x\";};}func run(){let f=make();f();}", "run",
        "string");
    effects_expect("spec Action():void;func make<T:throw>(x:T):Action{return (){throw x;};}func run(){let "
                   "f=make(\"x\");f();}",
                   "run", "string");
    effects_expect("func a(n:i32){if n==0{throw \"x\";}b(n-1);}func b(n:i32){a(n);}func run(){b(2);}", "run",
                   "string");
    effects_expect("func a(n:i32){b(n-1);}func b(n:i32){a(n);}func run(){b(2);}", "run", "none");
    effects_expect("func grow<T>(){grow<T[]>();}func run(){grow<i32>();}", "run", "none");
    effects_expect("func grow<T>(){if false{throw \"x\";}grow<T[]>();}func run(){grow<i32>();}", "run",
                   "string");
    effects_expect("spec Action():void;func factory():Action{let prior=factory();return (){prior();};}"
                   "func run(){let f=factory();}",
                   "run", "none");
    effects_expect("spec Action():void;func factory():Action{let prior=factory();return (){prior();};}"
                   "func run(){factory()();}",
                   "run", "unknown");
    effects_expect("spec Action():void;func fail(){throw true;}func make<T:throw>(x:T):Action{"
                   "try fail() catch e:string{return (){throw x;};} catch{} return (){};}"
                   "func run(){make(\"x\")();}",
                   "run", "none");
    effects_expect("spec Action():void;type W{static func fail(){throw \"x\";}}"
                   "func run(){let f:Action=W.fail;f();}",
                   "run", "string");
    effects_expect("spec Action():void;type W<T:throw>{let value:T;func fail(){throw self.value;}}"
                   "func run(x:W<string>){let f:Action=x.fail;f();}",
                   "run", "string");
    effects_expect("spec Action():void;type W{var f:Action;}func run(x:W){x.f();}", "run",
                   "unknown");
    effects_expect("spec Action():void;func run(){var f:Action=(){};f=(){throw true;};f();}", "run",
                   "unknown");
    effects_expect("spec Action():void;func run(f:Action){f();throw \"x\";}", "run",
                   "string, unknown");
    effects_expect("spec Action():void;func make(flag:bool):Action{if flag{return (){throw true;};}"
                   "return (){throw \"x\";};}func run(){make(false)();}",
                   "run", "bool, string");
}

/* Constructors, defaults and protocol lowering execute implicit callables. */
static void effects_implicit_calls(void) {
    effects_expect("type Error{func Error(){throw \"x\";}}func run(){let x=Error();}", "run", "string");
    effects_expect("type Error{func Error(){throw \"x\";}}func run(){let x=Error{};}", "run", "string");
    effects_expect("func fail():i32{throw \"x\";}type Item{let value=fail();}func run(){let x=Item();}",
                   "run", "string");
    effects_expect("type Result(bool,i32);type Cursor{@iterator func next():Result{throw \"x\";}}"
                   "func run(c:Cursor){for let x in c{}}",
                   "run", "string");
    effects_expect(
        "type Result(bool,i32);type Cursor{@iterator func next():Result{return (false,0);}}"
        "type Container{@iterable func iter():Cursor{throw \"x\";}}func run(c:Container){for let x in c{}}",
        "run", "string");
    effects_expect(
        "func fail():i32{throw \"x\";}@abi type Item{let value:i32=fail();}func run(){let x=Item();}", "run",
        "string");
    effects_expect("type Base{let x:i32;func Base(){throw \"x\";}}type Mixed{...:Base=Base();}"
                   "func run(){let x=Mixed();}",
                   "run", "string");
    effects_expect("type Owner<T:throw>{func Owner(x:T){throw x;}}func run(){let x=Owner<string>(\"x\");}",
                   "run", "string");
    effects_expect("func argument():i32{throw true;}func take(x:i32){}func run(){take(argument());}", "run",
                   "bool");
    effects_expect("type W{func work(){}}func receiver():W{throw true;}func run(){receiver().work();}", "run",
                   "bool");
    effects_expect("func index():i32{throw true;}func run(x:i32[]){let y=x[index()];}", "run", "bool");
    effects_expect("func fail():i32{throw true;}type Pair(i32,i32);func run(){let p:Pair=(0,fail());}", "run",
                   "bool");
    effects_expect("func fail():i32{throw true;}type Item{}type Item<T>{let value=fail();}"
                   "func run(){let item=Item<string>{};}",
                   "run", "bool");
}

/* Lazy storage initialization participates in reads, writes and witness dispatch. */
static void effects_lazy_initializers(void) {
    const char *prelude = "func initial<T:throw>():i32{let value:T;throw value;}"
                          "let fixed:i32=initial<string>();var changing:i32=initial<bool>();"
                          "type Lazy<T:throw>{open static let value:i32=initial<T>();"
                          "open static var current:i32=initial<T>();}"
                          "spec Stored{static var value:i32;}"
                          "type Store:Stored{static var value:i32=initial<bool>();}"
                          "func read<T:Stored>():i32{return T.value;}func write<T:Stored>(){T.value=0;}";
    const char *bodies[] = {"let x=fixed;",
                            "let x=changing;",
                            "changing=0;",
                            "changing+=1;",
                            "let x=Lazy<string>.value;",
                            "let x=Lazy<bool>.value;",
                            "Lazy<bool>.current=0;",
                            "Lazy<string>.current+=1;",
                            "let x=read<Store>();",
                            "write<Store>();",
                            "try fixed catch e:string{}",
                            "try Lazy<string>.value catch e:string{}",
                            "try write<Store>() catch e:bool{}",
                            "try read<Store>() catch e:string{}"};
    const char *expected[] = {"string", "bool", "bool", "bool", "string", "bool", "bool",
                              "string", "bool", "bool", "none", "none",   "none", "bool"};
    for (size_t i = 0U; i < sizeof bodies / sizeof *bodies; ++i) {
        char source[4096];
        snprintf(source, sizeof source, "%sfunc run(){%s}", prelude, bodies[i]);
        effects_expect(source, "run", expected[i]);
    }
    effects_expect("func initial<T:throw>():i32{let x:T;throw x;}"
                   "type Lazy<T:throw>{open static let value=initial<T>();}"
                   "func relay<U:throw>():i32{return Lazy<U>.value;}func run(){relay<string>();}",
                   "run", "string");
    effects_expect("spec Action():void;func create():Action{return (){throw true;};}"
                   "let action:Action=create();func run(){let f=action;}",
                   "run", "none");
    effects_expect("spec Action():void;func create():Action{return (){throw true;};}"
                   "type Holder{open static let action:Action=create();}func run(){Holder.action();}",
                   "run", "bool");
    effects_expect("spec Action():void;type Holder{open static var action:Action=(){};}"
                   "func run(){Holder.action();}",
                   "run", "unknown");
    effects_expect("func initial():i32{throw true;}let a:i32=b;let b:i32=initial();"
                   "func run(){let x=a;}",
                   "run", "bool");
}

/* Exact nominal identities survive every legal generic payload category. */
static void effects_payload_matrix(void) {
    const char *types[] = {"bool", "i8",  "i16",    "i32",    "i64",         "u8",           "u16",
                           "u32",  "u64", "f32",    "f64",    "string",      "State",        "Pair",
                           "Ref",  "Val", "AbiRef", "AbiVal", "Box<string>", "ValueBox<i32>"};
    const char *prelude =
        "enum State{Failed}type Pair(i32,string);type Ref{let code:i32;}"
        "@value type Val{let code:i32;}@abi type AbiRef{let code:i32;}@value @abi type AbiVal{let code:i32;}"
        "type Box<T>{let value:T;}@value type ValueBox<T>{let value:T;}func raise<T:throw>(x:T){throw x;}";
    for (size_t i = 0U; i < sizeof types / sizeof *types; ++i) {
        char source[4096], expected[256];
        snprintf(source, sizeof source, "%sfunc run(x:%s){raise(x);}", prelude, types[i]);
        snprintf(expected, sizeof expected, "%s%s", i < 12U ? "" : "effects.", types[i]);
        effects_expect(source, "run", expected);
        snprintf(source, sizeof source, "%sfunc run(x:%s){try raise<%s>(x) catch e:%s{}}", prelude, types[i],
                 types[i], types[i]);
        effects_expect(source, "run", "none");
    }
    effects_expect("type A(i32,i32);type B(i32,i32);func raise<T:throw>(x:T){throw x;}func run(x:A){try "
                   "raise(x) catch e:B{}}",
                   "run", "effects.A");
    effects_expect("type Box<T>{let value:T;}func raise<T:throw>(x:T){throw x;}func run(x:Box<i32>){try "
                   "raise(x) catch e:Box<string>{}}",
                   "run", "effects.Box<i32>");
    effects_expect("spec Action():void;var f:Action=(){};func run(){f();}", "run",
                   "unknown");
    effects_expect("spec Action():void;func run(f:Action){try f() catch{}}", "run", "none");
    effects_expect("spec Action():void;func run(f:Action){try f() catch{throw;}}", "run",
                   "unknown");
    effects_expect("spec Action():void;func run(f:Action){try f() catch{throw true;}}", "run", "bool");
    effects_expect("spec Action():void;func run(f:Action){try (try f() catch e:string{}) catch "
                   "e:string{throw true;} catch{}}",
                   "run", "none");
}

/* Known subjects reuse witness selections; opaque subjects stay unknown. */
static void effects_spec_dispatch(void) {
    effects_expect("spec Worker{func work():void;}type W:Worker{func work():void{throw \"x\";}}"
                   "func run(x:W){let w:Worker=x;w.work();}",
                   "run", "string");
    effects_expect("spec Worker{func work():void;}type W:Worker{func work():void{throw \"x\";}}"
                   "func invoke<T:Worker>(x:T){x.work();}func run(x:W){invoke(x);}",
                   "run", "string");
    effects_expect("spec Worker{func work():void;}type W:Worker{func work():void{throw \"x\";}}"
                   "func invoke(x:Worker){x.work();}func run(x:W){invoke(x);}",
                   "run", "string");
    effects_expect("spec Worker{func work():void;}type W{}fit W:Worker{func work():void{throw \"x\";}}"
                   "func invoke<T:Worker>(x:T){x.work();}func run(x:W){invoke(x);}",
                   "run", "string");
    effects_expect("spec Worker{func work():void;}fit string:Worker{func work():void{throw true;}}"
                   "func invoke<T:Worker>(x:T){x.work();}func run(){invoke(\"x\");}",
                   "run", "bool");
    effects_expect("spec Worker{func work():void;}fit i32[]:Worker{func work():void{throw true;}}"
                   "func invoke<T:Worker>(x:T){x.work();}func run(x:i32[]){invoke(x);}",
                   "run", "bool");
    effects_expect("spec Worker{func work():void;}fit T[]:Worker{func work():void{throw true;}}"
                   "func invoke<T:Worker>(x:T){x.work();}func run(x:string[]){invoke(x);}",
                   "run", "bool");
    effects_expect(
        "spec Factory{static func work():void;}type W:Factory{static func work():void{throw true;}}"
        "func invoke<T:Factory>(){T.work();}func run(){invoke<W>();}",
        "run", "bool");
    effects_expect("spec Worker{func work():void;}func run(x:Worker){x.work();}", "run",
                   "unknown");
    effects_expect("spec Worker{func work():void;}type W{}type W<T>:Worker{func work():void{throw true;}}"
                   "func invoke<T:Worker>(x:T){x.work();}func run(x:W<string>){invoke(x);}",
                   "run", "bool");
}

/* Stage one keeps ordinary propagation and rejects unproved C ABI escapes. */
static void effects_abi_boundaries(void) {
    const char *bad[] = {
        "func fail(){throw \"x\";}@abi func run(){try fail() catch e:i32{}}",
        ("spec Action():void;type Holder{var f:Action;}func invoke(h:Holder){h.f();}@abi func "
         "run(){invoke(Holder{f:(){}});}"),
        "spec Action():void;func invoke(f:Action){f();}@abi func run(){invoke((){throw \"x\";});}",
        "func initial():i32{throw true;}let value=initial();@abi func run(){let x=value;}",
        "func initial():i32{throw true;}type Lazy{open static var value=initial();}"
        "@abi func run(){Lazy.value=0;}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        char source[4096];
        snprintf(source, sizeof source, "module boundary;%s", bad[i]);
        ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, "AE1313");
        throw_constraint_dispose(&unit);
    }
    effects_expect(
        "spec Action():void;func invoke(f:Action){try f() catch{}}@abi func run(){invoke((){throw \"x\";});}",
        "run", "none");
    effects_expect("extern func foreign():void;@abi func run(){foreign();}", "run", "none");
    effects_expect("func initial():i32{throw true;}type Lazy{open static let value=initial();}"
                   "@abi func run(){try Lazy.value catch{}}",
                   "run", "none");
}

/* Core facts preserve each argument's capture filter without emitting infos. */
static void effects_call_facts(void) {
    ThrowConstraintUnit unit =
        throw_constraint_analyze("module effects;func text():i32{throw \"x\";}func flag():i32{throw true;}"
                                 "func add(x:i32,y:i32):i32{return x+y;}"
                                 "func run(){try add(text(),flag()) catch e:string{}}",
                                 NULL, NULL);
    THROW_CHECK(unit.analysis->info_count == 0U);
    size_t count = feng_semantic_exception_call_count(unit.analysis);
    THROW_CHECK(count == 3U);
    size_t escaping = 0U;
    for (size_t i = 0U; i < count; ++i) {
        FengExceptionCallFacts facts;
        THROW_CHECK(feng_semantic_exception_call_at(unit.analysis, i, &facts));
        THROW_CHECK(facts.path != NULL && facts.graph != NULL && facts.expression != NULL);
        FengExceptionCallFacts by_expression;
        THROW_CHECK(feng_semantic_exception_call_facts(unit.analysis, facts.expression, &by_expression));
        THROW_CHECK(by_expression.graph == facts.graph && by_expression.effects == facts.effects &&
                    by_expression.escaping_effects == facts.escaping_effects);
        if (facts.escaping_effects != 0U) {
            char *display = feng_exception_format(facts.graph, facts.escaping_effects);
            THROW_CHECK(display != NULL && strcmp(display, "bool") == 0);
            FengToken token = facts.expression->token;
            THROW_CHECK(token.length == 4U && memcmp(token.lexeme, "flag", 4U) == 0);
            free(display);
            ++escaping;
        }
    }
    THROW_CHECK(escaping == 1U && unit.analysis->info_count == 0U);
    FengExceptionCallFacts missing;
    THROW_CHECK(feng_semantic_exception_call_count(NULL) == 0U);
    THROW_CHECK(!feng_semantic_exception_call_at(NULL, 0U, &missing));
    THROW_CHECK(!feng_semantic_exception_call_at(unit.analysis, count, &missing));
    THROW_CHECK(missing.graph == NULL && missing.expression == NULL);
    THROW_CHECK(!feng_semantic_exception_call_at(unit.analysis, 0U, NULL));
    FengExpr unrelated = {0};
    THROW_CHECK(!feng_semantic_exception_call_facts(NULL, &unrelated, &missing));
    THROW_CHECK(!feng_semantic_exception_call_facts(unit.analysis, NULL, &missing));
    THROW_CHECK(!feng_semantic_exception_call_facts(unit.analysis, &unrelated, &missing));
    THROW_CHECK(!feng_semantic_exception_call_facts(unit.analysis, &unrelated, NULL));
    THROW_CHECK(missing.graph == NULL && missing.expression == NULL);
    throw_constraint_dispose(&unit);
}

/* A fully caught call still needs a complete instantiated Hover summary even
 * when its value is discarded and factories return multiple callable layers. */
static void effects_caught_call_queries(void) {
    const char *factories[] = {
        "spec Action():void;func create<T:throw>():Action{return (){let x:T;throw x;};}",
        "spec Action():void;spec Factory():Action;func create<T:throw>():Factory{"
        "return (){let action:Action=(){let x:T;throw x;};return action;};}"};
    const char *types[] = {"bool", "string"};
    for (size_t f = 0U; f < sizeof factories / sizeof *factories; ++f) {
        for (size_t t = 0U; t < sizeof types / sizeof *types; ++t) {
            char source[1024];
            snprintf(source, sizeof source,
                     "module effects;%sfunc run(){try create<%s>()()%s catch{} let unused=0;}", factories[f],
                     types[t], f == 0U ? "" : "()");
            ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
            const FengDecl *run = effects_function(unit.program, "run");
            const FengExpr *call = run->as.function_decl.body->statements[0]->as.expr->as.try_expr.body;
            FengExceptionCallFacts facts;
            THROW_CHECK(feng_semantic_exception_call_facts(unit.analysis, call, &facts));
            char *display = feng_exception_format(facts.graph, facts.effects);
            THROW_CHECK(display != NULL && strcmp(display, types[t]) == 0);
            free(display);
            const FengExceptionTemplate *summary = feng_semantic_exception_template(unit.analysis, run);
            THROW_CHECK(summary != NULL && summary->effects == 0U && facts.escaping_effects == 0U);
            throw_constraint_dispose(&unit);
        }
    }
}

/* All consumers receive type sets, irrespective of internal proof details. */
static void effects_type_summary(void) {
    effects_expect("func fail<T:throw>(x:T){throw x;}"
                   "func run<T:throw>(x:T){try fail(x) catch e:string{throw true;}}",
                   "run", "T, bool");
    effects_expect("spec Action():void;spec Worker{func work():void;}"
                   "func run(f:Action,w:Worker){f();w.work();throw \"x\";}",
                   "run", "string, unknown");
    FengExceptionGraph *graph = feng_exception_graph_create();
    THROW_CHECK(graph != NULL);
    const char *slots[] = {"T"};
    uint32_t fn = feng_exception_function_add(graph, "test", slots, 1U, 0U, 0U);
    uint32_t parameter = feng_exception_node(graph, FENG_EXCEPTION_TYPE_PARAMETER, fn, 0, 0,
                                            NULL, NULL, 0);
    uint32_t text = feng_exception_node(graph, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0, "string", NULL, 0);
    uint32_t flag = feng_exception_node(graph, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0, "bool", NULL, 0);
    uint32_t first = feng_exception_unknown(graph);
    THROW_CHECK(first == feng_exception_unknown(graph));
    uint32_t second = feng_exception_node(graph, FENG_EXCEPTION_UNKNOWN, 0, 0, 0, NULL, &text, 1U);
    THROW_CHECK(first != second);
    uint32_t box = feng_exception_node(graph, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0,
                                      "effects.Box", &first, 1U);
    uint32_t args[] = {text, box};
    uint32_t nested = feng_exception_node(graph, FENG_EXCEPTION_NAMED_TYPE, 0, 0, 0,
                                         "effects.Pair", args, 2U);
    uint32_t guarded = feng_exception_node(graph, FENG_EXCEPTION_GUARDED, 1, flag, 0, NULL, NULL, 0);
    uint32_t guarded_union = feng_exception_node(graph, FENG_EXCEPTION_GUARDED, 1,
        feng_exception_union(graph, text, flag), 0, NULL, NULL, 0);
    uint32_t raised = feng_exception_node(graph, FENG_EXCEPTION_THROW, nested, 0, 0, NULL, NULL, 0);
    uint32_t items[] = {guarded_union, second, parameter, raised, first, flag, guarded};
    uint32_t all = 0;
    for (size_t i = 0U; i < sizeof items / sizeof *items; ++i)
        all = feng_exception_union(graph, all, items[i]);
    THROW_CHECK(!graph->failed);
    size_t node_count = graph->node_count;
    char *formatted = feng_exception_format(graph, all);
    THROW_CHECK(formatted != NULL && strcmp(formatted,
        "T, bool, effects.Pair<string, effects.Box<unknown>>, string, unknown") == 0);
    free(formatted);
    formatted = feng_exception_format(graph, 0U);
    THROW_CHECK(formatted != NULL && strcmp(formatted, "none") == 0);
    free(formatted);
    THROW_CHECK(feng_exception_format(NULL, 0U) == NULL);
    THROW_CHECK(feng_exception_format(graph, (uint32_t)graph->node_count) == NULL);
    THROW_CHECK(graph->node_count == node_count && !graph->failed);
    THROW_CHECK(graph->nodes[first].name == NULL && graph->nodes[second].name == NULL);
    THROW_CHECK(graph->nodes[second].arg_count == 1U && graph->nodes[second].args[0] == text);
    THROW_CHECK(graph->nodes[guarded].kind == FENG_EXCEPTION_GUARDED);
    feng_exception_graph_release(graph);
    graph = feng_exception_graph_create();
    THROW_CHECK(graph != NULL);
    (void)feng_exception_node(graph, FENG_EXCEPTION_UNKNOWN, 0, 0, 0, "diagnostic text", NULL, 0);
    THROW_CHECK(graph->failed);
    feng_exception_graph_release(graph);
}

/* Registered once by the existing semantic test driver. */
void test_exception_effects_semantics(void) {
    effects_control_flow();
    effects_generic_domains();
    effects_callable_values();
    effects_implicit_calls();
    effects_lazy_initializers();
    effects_payload_matrix();
    effects_spec_dispatch();
    effects_abi_boundaries();
    effects_call_facts();
    effects_caught_call_queries();
    effects_type_summary();
    puts("exception effect semantic matrices passed");
}
