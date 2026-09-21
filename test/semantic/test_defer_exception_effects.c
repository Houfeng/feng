#include "../throw_constraint_helpers.h"
#include "semantic/exception_effects.h"

/* One source-level cleanup contract, independent of the analysis term layout. */
typedef struct DeferEffectCase {
    const char *name;
    const char *source;
    const char *code;
    const char *types;
} DeferEffectCase;

/* Exercise the public analyzer and retain error positions and type evidence. */
static void defer_effect_case(const DeferEffectCase *test) {
    const char *prelude =
        "module effects;spec Action():void;func quiet(){}"
        "func text(){throw \"text\";}func flag(){throw true;}"
        "func value():i32{throw \"value\";}func relay(f:Action){f();}"
        "func raise<T:throw>(x:T){throw x;}";
    size_t size = strlen(prelude) + strlen(test->source) + 1U;
    char *source = malloc(size);
    THROW_CHECK(source != NULL);
    snprintf(source, size, "%s%s", prelude, test->source);
    FengProgram *program = throw_constraint_parse(source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    bool matched = test->code == NULL ? ok && count == 0U : !ok && count != 0U;
    if (test->code != NULL) {
        bool found = false;
        for (size_t i = 0U; i < count; ++i) {
            if (strcmp(errors[i].code, test->code) != 0)
                continue;
            found = true;
            THROW_CHECK(errors[i].token.line > 0U && errors[i].token.column > 0U);
            if (strcmp(test->code, "AE1507") == 0) {
                THROW_CHECK(errors[i].token.length == 5U);
                THROW_CHECK(memcmp(errors[i].token.lexeme, "defer", 5U) == 0);
                if (test->types != NULL) {
                    const char *types = strstr(errors[i].message, "exception set: ");
                    matched = matched && types != NULL && strcmp(types + 15U, test->types) == 0;
                }
            }
        }
        matched = matched && found;
    }
    if (!matched) {
        fprintf(stderr, "defer case %s expected %s (%s)\n%s\n", test->name,
            test->code != NULL ? test->code : "success", test->types != NULL ? test->types : "", source);
        for (size_t i = 0U; i < count; ++i)
            fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
    }
    THROW_CHECK(matched);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    free(source);
}

/* Check direct/indirect calls, local catch transfer and every generic domain. */
static void defer_effect_matrix(void) {
    const DeferEffectCase cases[] = {
        {"empty", "func run(){defer{}}", NULL, NULL},
        {"pure", "func run(){defer{quiet();}}", NULL, NULL},
        {"ordinary propagation", "func run(){defer{quiet();}text();}", NULL, NULL},
        {"direct call", "func run(){defer{text();}}", "AE1507", "string"},
        {"mixed", "func run(){defer{text();flag();}}", "AE1507", "bool, string"},
        {"dead branch", "func run(){defer{if false{text();}}}", "AE1507", "string"},
        {"loop condition", "func predicate():bool{throw true;}func run(){defer{while predicate(){}}}", "AE1507", "bool"},
        {"call chain", "func a(){b();}func b(){text();}func run(){defer{a();}}", "AE1507", "string"},
        {"recursive throwing", "func a(n:i32){if n==0{text();}else{a(n-1);}}func run(){defer{a(2);}}", "AE1507", "string"},
        {"recursive pure", "func a(n:i32){if n>0{a(n-1);}}func run(){defer{a(2);}}", NULL, NULL},
        {"mutual recursion", "func a(n:i32){b(n);}func b(n:i32){if n>0{a(n-1);}else{text();}}func run(){defer{a(2);}}", "AE1507", "string"},
        {"typed catch", "func run(){defer{try text() catch e:string{}}}", NULL, NULL},
        {"catch all", "func run(){defer{try text() catch{}}}", NULL, NULL},
        {"wrong catch", "func run(){defer{try text() catch e:bool{}}}", "AE1507", "string"},
        {"partial catch", "func both(){text();flag();}func run(){defer{try both() catch e:string{}}}", "AE1507", "bool"},
        {"complete typed catches", "func both(){text();flag();}func run(){defer{try both() catch e:string{} catch e:bool{}}}", NULL, NULL},
        {"handler escape", "func run(){defer{try text() catch{flag();}}}", "AE1507", "bool"},
        {"unreached handler", "func run(){defer{try quiet() catch{flag();}}}", NULL, NULL},
        {"wrong handler unreachable", "func run(){defer{try text() catch e:bool{flag();} catch{}}}", NULL, NULL},
        {"nested handler caught", "func run(){defer{try text() catch{try flag() catch e:bool{}}}}", NULL, NULL},
        {"handled callee", "func handled(){try text() catch{}}func run(){defer{handled();}}", NULL, NULL},
        {"rethrowing callee", "func again(){try text() catch{throw;}}func run(){defer{again();}}", "AE1507", "string"},
        {"outer catch", "func run(){let result=try (if true{defer{text();}0;}else{0;}) catch{0;};}", "AE1507", "string"},
        {"caller catch", "func inner(){defer{text();}}func run(){try inner() catch{}}", "AE1507", "string"},
        {"abi outer catch", "@abi func run(){let result=try (if true{defer{text();}0;}else{0;}) catch{0;};}", "AE1507", "string"},
        {"abi safe cleanup", "@abi func run(){defer{try text() catch{}}}", NULL, NULL},
        {"unknown", "func run(f:Action){defer{f();}}", "AE1507", "unknown"},
        {"unknown and known", "func run(f:Action){defer{f();text();}}", "AE1507", "string, unknown"},
        {"unknown caught", "func run(f:Action){defer{try f() catch{}}}", NULL, NULL},
        {"unknown partial", "func run(f:Action){defer{try f() catch e:string{}}}", "AE1507", "unknown"},
        {"unknown handler escape", "func run(f:Action){defer{try f() catch{flag();}}}", "AE1507", "bool"},
        {"known function value", "func run(){let f:Action=text;defer{f();}}", "AE1507", "string"},
        {"known pure value", "func run(){let f:Action=quiet;defer{f();}}", NULL, NULL},
        {"known callback", "func run(){defer{relay(text);}}", "AE1507", "string"},
        {"pure callback", "func run(){defer{relay(quiet);}}", NULL, NULL},
        {"mutable callback", "func run(){var f:Action=quiet;defer{f();}f=text;}", "AE1507", "unknown"},
        {"mutable field", "type Box{var f:Action;}func run(x:Box){defer{x.f();}}", "AE1507", "unknown"},
        {"mutable field caught", "type Box{var f:Action;}func run(x:Box){defer{try x.f() catch{}}}", NULL, NULL},
        {"lambda invoked", "func run(){let f:Action=(){text();};defer{f();}}", "AE1507", "string"},
        {"lambda only created", "func run(){defer{let f:Action=(){text();};}}", NULL, NULL},
        {"cleanup inside lambda", "func run(){let f:Action=(){defer{text();}};}", "AE1507", "string"},
        {"safe cleanup inside lambda", "func run(){let f:Action=(){defer{try text() catch{}}};f();}", NULL, NULL},
        {"returned callback", "func make():Action{return text;}func run(){defer{make()();}}", "AE1507", "string"},
        {"closed generic", "func run(){defer{raise(true);}}", "AE1507", "bool"},
        {"open generic", "func run<T:throw>(x:T){defer{raise(x);}}", "AE1507", "T"},
        {"open generic caught", "func run<T:throw>(x:T){defer{try raise(x) catch{}}}", NULL, NULL},
        {"open generic conditional", "func run<T:throw>(x:T){defer{try raise(x) catch e:string{}}}", "AE1507", "T"},
        {"generic catch substitution", "func filter<T:throw>(x:T){try raise(x) catch e:string{}}func run(){defer{filter(\"x\");}}", NULL, NULL},
        {"generic remaining", "func filter<T:throw>(x:T){try raise(x) catch e:string{}}func run(){defer{filter(true);}}", "AE1507", "bool"},
        {"owner generic", "type Box<T:throw>{let x:T;func fail(){throw self.x;}}func run(x:Box<string>){defer{x.fail();}}", "AE1507", "string"},
        {"method generic", "type Box<T>{func fail<U:throw>(x:U){throw x;}}func run(x:Box<string>){defer{x.fail(true);}}", "AE1507", "bool"},
        {"owner cleanup", "type Box<T:throw>{let x:T;func run(){defer{try raise(self.x) catch{}}}}", NULL, NULL},
        {"method cleanup open", "type Box<T>{func run<U:throw>(x:U){defer{raise(x);}}}", "AE1507", "U"},
        {"dynamic spec", "spec Worker{func work():void;}func run(x:Worker){defer{x.work();}}", "AE1507", "unknown"},
        {"dynamic spec caught", "spec Worker{func work():void;}func run(x:Worker){defer{try x.work() catch{}}}", NULL, NULL},
        {"generic witness", "spec Worker{func work():void;}type W:Worker{func work():void{text();}}func work<T:Worker>(x:T){x.work();}func run(x:W){defer{work(x);}}", "AE1507", "string"},
        {"pure witness", "spec Worker{func work():void;}type W:Worker{func work():void{}}func work<T:Worker>(x:T){x.work();}func run(x:W){defer{work(x);}}", NULL, NULL},
        {"fit witness", "spec Worker{func work():void;}type W{}fit W:Worker{func work():void{flag();}}func work<T:Worker>(x:T){x.work();}func run(x:W){defer{work(x);}}", "AE1507", "bool"},
        {"indexed generic pure witness", "spec Worker{func work():void;}type W:Worker{func work():void{}}func work<T:Worker>(x:T[]){x[0].work();}func run(x:W[]){defer{work(x);}}", NULL, NULL},
        {"indexed generic throwing witness", "spec Worker{func work():void;}type W:Worker{func work():void{flag();}}func work<T:Worker>(x:T[]){x[0].work();}func run(x:W[]){defer{work(x);}}", "AE1507", "bool"},
        {"indexed generic caught witness", "spec Worker{func work():void;}type W:Worker{func work():void{flag();}}func work<T:Worker>(x:T[]){x[0].work();}func run(x:W[]){defer{try work(x) catch e:bool{}}}", NULL, NULL},
        {"indexed dynamic spec", "spec Worker{func work():void;}func run(x:Worker[]){defer{x[0].work();}}", "AE1507", "unknown"},
        {"indexed dynamic callable", "func run(x:Action[]){defer{x[0]();}}", "AE1507", "unknown"},
        {"indexed dynamic caught", "func run(x:Action[]){defer{try x[0]() catch{}}}", NULL, NULL},
        {"constructor", "type Item{func Item(){text();}}func run(){defer{let x=Item();}}", "AE1507", "string"},
        {"field initializer", "type Item{let x=value();}func run(){defer{let x=Item{};}}", "AE1507", "string"},
        {"argument evaluation", "func take(x:i32){}func run(){defer{take(value());}}", "AE1507", "string"},
        {"lazy global", "let x=value();func run(){defer{let y=x;}}", "AE1507", "string"},
        {"lazy global caught", "let x=value();func run(){defer{let y=try x catch{0;};}}", NULL, NULL},
        {"static initializer", "type Lazy{static let x=value();}func run(){defer{let y=Lazy.x;}}", "AE1507", "string"},
        {"iterator", "type Result(bool,i32);type Cursor{@iterator func next():Result{throw true;}}func run(x:Cursor){defer{for let item in x{}}}", "AE1507", "bool"},
        {"direct throw forbidden", "func run(){defer{throw \"x\";}}", "AE1502", NULL},
        {"caught direct throw forbidden", "func run(){defer{try (if true{throw \"x\";}else{}) catch{}}}", "AE1502", NULL},
        {"direct rethrow forbidden", "func run(){defer{try text() catch{throw;}}}", "AE1502", NULL},
        {"return forbidden", "func run(){defer{return;}}", "AE1501", NULL},
        {"nested defer forbidden", "func run(){defer{defer{}}}", "AE1503", NULL},
        {"inner loop controls", "func run(){defer{for var i:i32=0;i<2;i+=1{if i==0{continue;}break;}}}", NULL, NULL},
        {"multiple independent boundaries", "func run(){defer{text();}defer{flag();}}", "AE1507", NULL},
        {"N08", "type Error{let code:i32;}func fail(n:i32){throw Error{code:n};}func work():void{defer{fail(2);}fail(1);}func run(){try work() catch e:Error{}}", "AE1507", "effects.Error"},
        {"landing replacement", "type Error{let code:i32;}func fail(n:i32){throw Error{code:n};}func run():i32{return try (if true{defer{fail(2);}throw Error{code:1};}else{0;}) catch original:Error{original.code;};}", "AE1507", "effects.Error"},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(*cases); ++i)
        defer_effect_case(&cases[i]);
    printf("defer exception semantic matrix: %zu cases passed\n", sizeof(cases) / sizeof(*cases));
}

/* Errors retain independent boundaries and exclude calls handled inside each. */
static void defer_effect_locations(void) {
    const char *source = "module effects;\n"
        "func text(){throw \"x\";}\n"
        "func flag(){throw true;}\n"
        "func run(){\n"
        "  defer {\n"
        "    try text() catch {}\n"
        "    flag();\n"
        "  }\n"
        "  defer { text(); }\n"
        "}\n";
    FengProgram *program = throw_constraint_parse(source);
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    THROW_CHECK(!feng_semantic_analyze_with_options(programs, 1U, &options, NULL, &errors, &count));
    THROW_CHECK(count == 2U);
    const unsigned lines[] = {5U, 9U}, related[] = {7U, 9U};
    for (size_t i = 0U; i < count; ++i) {
        THROW_CHECK(strcmp(errors[i].code, "AE1507") == 0);
        THROW_CHECK(errors[i].token.line == lines[i] && errors[i].token.column == 3U);
        THROW_CHECK(errors[i].related_location_count == 1U);
        THROW_CHECK(errors[i].related_locations[0].token.line == related[i]);
        THROW_CHECK(strcmp(errors[i].path, errors[i].related_locations[0].path) == 0);
    }
    feng_semantic_errors_free(errors, count);
    feng_program_free(program);
}

/* Register the phase-two source and diagnostic contract suites. */
void test_defer_exception_effects_semantics(void) {
    defer_effect_matrix();
    defer_effect_locations();
}
