#include "../throw_constraint_helpers.h"

/* All source-file orders must register the same root before validating bodies. */
static void throwable_source_order(void) {
    static const char *const sources[] = {
        "open module order.base;@throwable open spec Root{}",
        "open module order.base;open type Error:Root{}",
        "open module order.left;import order.base as b;open func left(value:b.Root):b.Root{return value;}",
        "open module order.right;import order.base as b;open func right(value:b.Root):b.Root{return value;}",
        ("module order.client;import order.base;import order.base as b;import order.left;import order.right;"
         "func use(value:b.Root){throw right(left(value));}")
    };
    FengProgram *parsed[5];
    for (size_t i = 0U; i < 5U; ++i) parsed[i] = throw_constraint_parse(sources[i]);
    for (size_t reverse = 0U; reverse < 2U; ++reverse) {
        for (size_t rotation = 0U; rotation < 5U; ++rotation) {
            const FengProgram *programs[5];
            for (size_t i = 0U; i < 5U; ++i)
                programs[i] = parsed[(rotation + (reverse ? 4U - i : i)) % 5U];
            FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
                .pointer_size = feng_get_host_pointer_size()};
            FengSemanticAnalysis *analysis = NULL;
            FengSemanticError *errors = NULL;
            size_t count = 0U;
            bool ok = feng_semantic_analyze_with_options(programs, 5U, &options, &analysis, &errors, &count);
            if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
            THROW_CHECK(ok && count == 0U && analysis->exception_root == parsed[0]->declarations[0]);
            feng_semantic_analysis_free(analysis);
            feng_semantic_errors_free(errors, count);
        }
    }
    for (size_t i = 0U; i < 5U; ++i) feng_program_free(parsed[i]);
    puts("throwable source identity: ten file orders and diamond aliases passed");
}

/* Exercise the root contract through real source, including declaration-time
 * payload rejection and ordinary generic constraint evidence. */
void test_throwable_semantics(void) {
    static const struct {
        const char *source;
        const char *diagnostic;
    } cases[] = {
        {"module plain; type Item {}", NULL},
        {"module missing; func fail() { throw 1; }", "AE1410"},
        {"open module roots; @throwable open spec Root {}", NULL},
        {"module roots; @throwable open spec Root {}", "AE1408"},
        {"open module roots; @throwable spec Root {}", "AE1408"},
        {"open module roots; @throwable open spec Root<T> {}", "AE1408"},
        {"open module roots; @throwable() open spec Root {}", "AE1408"},
        {"open module roots; @throwable(1) open spec Root {}", "AE1408"},
        {"open module roots; @throwable @throwable open spec Root {}", "AE1408"},
        {"open module roots; @throwable open type Root {}", "AE1408"},
        {"open module roots; @throwable open spec Root():void;", "AE1408"},
        {"open module roots; @throwable open spec Root {} @throwable open spec Other {}", "AE1409"},
        {"open module roots; spec Child:Root {} @throwable open spec Root {}", "AE1411"},
        {"open module roots; @throwable open spec Root{} spec Other{}"
         "spec Both:Root&Other;", "AE1412"},
        {"open module roots; spec Both:Other&Root;spec Other{}"
         "@throwable open spec Root{}", "AE1412"},
        {"open module roots; spec Outer:Inner&Other;spec Inner:Root&Other;"
         "spec Other{} @throwable open spec Root{}", "AE1412"},
        {"open module roots; spec Other{} @throwable open spec Root{}"
         "type Error:Root,Other{}func fail<E:Root>(value:E){throw value;}"
         "func run(value:Error){fail<Error>(value);}", NULL},
        {"open module roots; @throwable open spec Root {} fit i32[]:Root {}", "AE1411"},
        {"open module roots; @throwable open spec Root {} type Error<T>:Root {let value:T;}"
         "func fail<T>(error:Error<T>) {throw error;}", NULL},
        {"open module roots; @throwable open spec Root {} fit int:Root;"
         "func fail() {throw 1;}", NULL},
        {"open module roots; @throwable open spec Root {} func fail() {throw 1;}", "AE0076"},
        {"open module roots; @throwable open spec Root {}"
         "func fail<E:Root>(value:E) {throw value;}", NULL},
        {"open module roots; @throwable open spec Root {}"
         "func fail<E>(value:E) {throw value;}", "AE0076"},
        {"open module roots; @throwable open spec Root {}"
         "func fail(value:Root) {throw value;} func run(value:Root) {try fail(value) catch e:Root {}}", NULL},
        {"open module roots; @throwable open spec Root {}"
         "func fail(value:Root) {throw value;} func run(value:Root) {try fail(value) catch e:Root {} catch {}}", "AE1406"},
        {"open module roots; @throwable open spec Root {} type Error<T>:Root {let value:T;}"
         "func fail<T>(error:Error<T>) {try error catch e:Error<T> {}}", "AE0179"},
        {"open module roots; @throwable open spec Root {} type Error:Root {}"
         "func fail(value:Root) {throw value;}"
         "@abi func run() {let value:Root=Error{};try fail(value) catch e:Root {}}", NULL},
        {"open module roots; @throwable open spec Root {} type Error:Root {}"
         "func fail(value:Root) {throw value;}"
         "@abi func run() {let value:Root=Error{};try fail(value) catch e:Error {}}", "AE1313"},
        {"open module roots; @throwable open spec Root {} type Error:Root {}"
         "func fail<E:Root>(value:E){throw value;}"
         "func leaf(){throw Error{};}"
         "@abi func run(){let value:Root=Error{};"
         "try fail<Root>(value) catch e:Error{leaf();} catch e:Root{}}", "AE1313"},
        {"open module roots; @throwable open spec Root {} type Error:Root {}"
         "func fail<E:Root>(value:E){throw value;}"
         "@abi func run(){let value:Root=Error{};"
         "try fail<Root>(value) catch e:Error{} catch e:Root{}}", NULL},
        {"open module roots; @throwable open spec Root {} type Error:Root {}"
         "func fail<E:Root>(value:E){throw value;}"
         "@abi func run(){try fail<Error>(Error{}) catch e:Error{}}", NULL},
        {"open module roots; @throwable open spec Root {} open spec Extra{}"
         "open spec Both:Extra&Root;type Error:Root,Extra{}"
         "func fail<E:Root>(value:E){throw value;}"
         "func forward<E:Root>(value:E){fail<E>(value);}"
         "func leaf(){throw Error{};}"
         "@abi func run(){let value:Both=Error{};"
         "try forward<Both>(value) catch e:Error{leaf();} catch e:Root{}}", "AE1412"},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ThrowConstraintUnit unit = throw_constraint_analyze(
            cases[i].source, NULL, cases[i].diagnostic);
        throw_constraint_dispose(&unit);
    }
    throwable_source_order();
}
