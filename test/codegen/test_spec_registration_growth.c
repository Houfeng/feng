#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Each declaration graph reaches a different recursive registration edge. */
typedef struct SpecGrowthCase {
    const char *name;
    const char *declarations;
} SpecGrowthCase;

/* Distinct closed types and open generic domains prevent instance deduplication
 * from hiding reallocations, without depending on the registry's capacity. */
static char *spec_growth_source(const SpecGrowthCase *test, unsigned count, bool open) {
    size_t capacity = strlen(test->declarations) + (size_t)count * 256U + 128U;
    char *source = malloc(capacity);
    THROW_CHECK(source != NULL);
    int written = snprintf(source, capacity, "module registration.growth;%s", test->declarations);
    THROW_CHECK(written >= 0 && (size_t)written < capacity);
    size_t used = (size_t)written;
    for (unsigned i = 0U; i < count; ++i) {
        if (open) {
            written = snprintf(source + used, capacity - used,
                "func use%u<A%u,V%u:Top<A%u>>(value:V%u):V%u{return value;}",
                i, i, i, i, i, i);
        } else {
            written = snprintf(source + used, capacity - used,
                "type Data%u{}func use%u(value:Top<Data%u>):Top<Data%u>{return value;}",
                i, i, i, i);
        }
        THROW_CHECK(written >= 0 && (size_t)written < capacity - used);
        used += (size_t)written;
    }
    return source;
}

/* Real source graphs exercise registration and all later consumers under the
 * compiler's sanitizer configuration; generated C must remain well formed. */
void test_spec_registration_growth_codegen(void (*compile_c)(const char *)) {
    const SpecGrowthCase cases[] = {
        {"leaf", "spec Top<T>{func get():T;}"},
        {"parents", "spec Leaf<T>{func get():T;}spec Mid<T>:Leaf<T>{}"
                    "spec Top<T>:Mid<T>{}"},
        {"field", "spec Leaf<T>{func get():T;}spec Top<T>{var value:Leaf<T>;}"},
        {"method", "spec Input<T>{func get():T;}spec Output<T>{func get():T;}"
                   "spec Top<T>{func map(value:Input<T>):Output<T>;}"},
        {"callable", "spec Input<T>{func get():T;}spec Output<T>{func get():T;}"
                     "spec Top<T>(value:Input<T>):Output<T>;"},
        {"union", "spec Leaf<T>{func get():T;}spec Top<T>:Leaf<T>|string;"},
        {"intersection", "spec First<T>{func get():T;}spec Second<T>{func echo(value:T):T;}"
                         "spec Top<T>:First<T>&Second<T>;"}
    };
    const unsigned counts[] = {1U, 8U, 33U};
    for (size_t kind = 0U; kind < sizeof cases / sizeof cases[0]; ++kind) {
        for (unsigned open = 0U; open < 2U; ++open) {
            for (size_t size = 0U; size < sizeof counts / sizeof counts[0]; ++size) {
                char *source = spec_growth_source(&cases[kind], counts[size], open != 0U);
                ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
                FengCodegenOutput output = {0};
                FengCodegenError error = {0};
                bool emitted = feng_codegen_emit_program(unit.analysis,
                    FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
                if (!emitted) fprintf(stderr, "spec growth %s/%s/%u: %s: %s\n",
                    cases[kind].name, open ? "open" : "closed", counts[size],
                    error.code, error.message);
                THROW_CHECK(emitted && output.c_source != NULL);
                compile_c(output.c_source);
                feng_codegen_output_free(&output);
                feng_codegen_error_free(&error);
                throw_constraint_dispose(&unit);
                free(source);
            }
        }
    }
    puts("spec registration: 42 open/closed recursive dependency and growth cases passed");
}
