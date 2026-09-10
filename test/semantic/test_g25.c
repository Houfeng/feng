#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "semantic/semantic.h"

/* One source-level case and its complete expected Semantic diagnostic. */
typedef struct G25Case {
    const char *id;
    const char *source;
    const char *code;
    const char *marker;
    const char *token;
    const char *message;
} G25Case;

/* One expected diagnostic, located by its source marker independently of the resolver. */
typedef struct G25Diagnostic {
    const char *code;
    const char *marker;
    const char *token;
    const char *message;
} G25Diagnostic;

/* Parse real programs and check the complete, unordered Semantic error set.
 * No Codegen stage runs here; positive cases require a complete analysis. */
static bool g25_check_source(const char *id, const char *source,
                              const char *provider_source,
                              const G25Diagnostic *expected, size_t expected_count) {
    FengProgram *program = NULL, *provider = NULL;
    FengParseError parse_error = {0};
    FengSemanticAnalyzeOptions options = {0};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    char path[128];
    bool ok = false;
    int path_length = snprintf(path, sizeof(path), "g25_%s.ff", id);

    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        fprintf(stderr, "G25 case identifier is too long\n");
        return false;
    }
    if (!feng_parse_source(source, strlen(source), path, &program, &parse_error)) {
        fprintf(stderr, "%s parse: %s\n%s\n", id, parse_error.message, source);
        goto cleanup;
    }
    if (provider_source != NULL && !feng_parse_source(provider_source,
            strlen(provider_source), "g25_provider.ff", &provider, &parse_error)) {
        fprintf(stderr, "G25 provider parse: %s\n", parse_error.message);
        goto cleanup;
    }
    const FengProgram *programs[] = {program, provider};
    options.target = FENG_COMPILE_TARGET_LIB;
    options.pointer_size = feng_get_host_pointer_size();
    bool result = feng_semantic_analyze_with_options(programs, provider == NULL ? 1U : 2U,
        &options, &analysis, &errors, &error_count);
    ok = result == (expected_count == 0U) && error_count == expected_count;
    if (expected_count == 0U) {
        ok = ok && analysis != NULL && errors == NULL;
    }
    for (size_t index = 0U; ok && index < expected_count; ++index) {
        const G25Diagnostic *diagnostic = &expected[index];
        const char *position = strstr(source, diagnostic->marker);
        unsigned line = 1U, column = 1U;
        size_t matches = 0U;

        if (position == NULL) {
            ok = false;
            break;
        }
        for (const char *p = source; p < position; ++p) {
            if (*p == '\n') { ++line; column = 1U; } else { ++column; }
        }
        for (size_t error_index = 0U; error_index < error_count; ++error_index) {
            const FengSemanticError *error = &errors[error_index];
            if (error->code != NULL && strcmp(error->code, diagnostic->code) == 0 &&
                strcmp(error->path, path) == 0 &&
                error->token.line == line && error->token.column == column &&
                error->token.length == strlen(diagnostic->token) &&
                memcmp(error->token.lexeme, diagnostic->token, strlen(diagnostic->token)) == 0 &&
                (diagnostic->message == NULL || strstr(error->message, diagnostic->message) != NULL)) {
                ++matches;
            }
        }
        ok = matches == 1U;
        if (!ok) {
            fprintf(stderr, "%s expected %s at %u:%u token %s\n",
                id, diagnostic->code, line, column, diagnostic->token);
        }
    }
    if (!ok) {
        fprintf(stderr, "G25 %s expected %zu diagnostics; got %zu\n%s\n",
            id, expected_count, error_count, source);
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u %s %s [token %.*s]\n", errors[i].path,
                errors[i].token.line, errors[i].token.column, errors[i].code,
                errors[i].message, (int)errors[i].token.length, errors[i].token.lexeme);
        }
    }
cleanup:
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    feng_program_free(provider);
    return ok;
}

/* Adapt the compact single-root matrix to the common complete-set checker. */
static bool g25_check(const G25Case *test, const char *provider_source) {
    const G25Diagnostic expected = {test->code, test->marker, test->token, test->message};
    return g25_check_source(test->id, test->source, provider_source,
                            &expected, test->code == NULL ? 0U : 1U);
}

/* A failed reference must not suppress independent roots in the same declaration. */
void test_g25_dependent_diagnostics(void) {
    /* Each source has one failed name and one separately provable invariant. */
    static const struct {
        const char *id;
        const char *source;
        G25Diagnostic expected[2];
    } cases[] = {
        {"13-type-independent-shape", "module g25;\ntype Item: Missing, i32 {}\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE0615", "i32", "i32", NULL}}},
        {"13-spec-independent-shape", "module g25;\nspec Item: Missing, i32 {}\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE0613", "i32", "i32", NULL}}},
        {"13-fit-independent-shape", "module g25;\nfit Missing: i32 {}\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE0809", "i32", "i32", NULL}}},
        {"13-type-independent-member", "module g25;\ntype Item: Missing { func check() { let value: i32 = \"wrong\"; } }\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE1003", "\"wrong\"", "\"wrong\"", NULL}}},
        {"13-fit-independent-member", "module g25;\nfit Missing { func check() { let value: i32 = \"wrong\"; } }\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE1003", "\"wrong\"", "\"wrong\"", NULL}}},
        {"13-type-independent-requirement", "module g25;\nspec Bound { func read(): i32; }\ntype Item: Missing, Bound {}\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE0705", "Item:", "Item", NULL}}},
        {"13-fit-independent-requirement", "module g25;\nspec Bound { func read(): i32; }\ntype Item {}\nfit Item: Missing, Bound {}\n",
         {{"AE1013", "Missing", "Missing", NULL}, {"AE0705", "Item:", "Item", NULL}}}
    };
    size_t failures = 0U;

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!g25_check_source(cases[i].id, cases[i].source, NULL, cases[i].expected, 2U)) {
            ++failures;
        }
    }
    if (failures != 0U) {
        fprintf(stderr, "G25: %zu independent-diagnostic cases failed\n", failures);
        exit(1);
    }
    printf("G25: %zu independent-diagnostic cases passed\n", sizeof(cases) / sizeof(cases[0]));
}

/* Real qualified and aliased references exercise the provider namespace. */
static void g25_module_diagnostics(void) {
    static const char *provider = "open module library.g25;\nopen type Box<T> { open let value:T; }\nopen type Pair<T,U> {}\nopen spec View<T> { func get():T; }\nopen func keep<T>(value:Box<Box<T[]>>):T { return value.value.value[0]; }\nopen func pick<T,U>(value:T):T { return value; }\nopen func plain(value:i32):i32 { return value; }\n";
    static const G25Case cases[] = {
        {
            "02A-alias",
            "module g25;\n"
            "import library.g25 as api;\n"
            "func use(value:api.Pair<i32>) {}\n",
            "AE1015", "api.Pair<i32>", "api", NULL
        },
        {
            "02A-qualified",
            "module g25;\n"
            "import library.g25;\n"
            "func use(value:library.g25.Box<i32,string>) {}\n",
            "AE1015", "library.g25.Box<i32,", "library", NULL
        },
        {
            "02C-module-arity",
            "module g25;\n"
            "import library.g25 as api;\n"
            "func use(value:i32) { api.pick<i32>(value); }\n",
            "AE1015", "pick<i32>", "pick", NULL
        },
        {
            "05C-module-non-generic",
            "module g25;\n"
            "import library.g25;\n"
            "func use(value:i32) { library.g25.plain<i32>(value); }\n",
            "AE1014", "plain<i32>", "plain", NULL
        },
        {
            "05F-module-value-arity",
            "module g25;\n"
            "import library.g25 as api;\n"
            "spec Mapper(value:i32):i32;\n"
            "func use() { let mapper:Mapper = api.pick<i32>; }\n",
            "AE1015", "pick<i32>", "pick", NULL
        },
        {
            "04A-imported-nested",
            "module g25;\n"
            "import library.g25 as api;\n"
            "func use(value:api.Box<api.Box<i32[]>>):i32 { return api.keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "02A-imported-valid",
            "module g25;\n"
            "import library.g25;\n"
            "func use(value:library.g25.Pair<i32,string>) {}\n",
            NULL, NULL, NULL, NULL
        }
    };
    size_t failures = 0U;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!g25_check(&cases[i], provider)) { ++failures; }
    }
    if (failures != 0U) {
        fprintf(stderr, "G25: %zu module diagnostic cases failed\n", failures);
        exit(1);
    }
    printf("G25: %zu module diagnostic cases passed\n", sizeof(cases) / sizeof(cases[0]));
}

/* Open pointer construction is a declaration error in every type-reference
 * position, including unused bodies and nested type/array wrappers. */
static void g25_open_pointer_diagnostics(void) {
    static const struct {
        const char *shape;
        const char *marker;
        const char *token;
    } shapes[] = {
        {"T*", "T*", "T"},
        {"T**", "T**", "T"},
        {"T***", "T***", "T"},
        {"T[]*", "T[]*", "T"},
        {"T[!]*", "T[!]*", "T"},
        {"T*[]", "T*[]", "T"},
        {"T*[!]", "T*[!]", "T"},
        {"Box<T>*", "Box<T>*", "Box"},
        {"Box<Box<T[]>>*", "Box<Box<T[]>>*", "Box"},
        {"Box<T*>[]", "T*>[]", "T"},
        {"Box<T[]*>", "T[]*>", "T"},
        {"Box<T[]>*[]", "Box<T[]>*[]", "Box"}
    };
    static const struct {
        const char *id;
        const char *source;
    } surfaces[] = {
        {"parameter", "func keep<T>(value:%s) {}\n"},
        {"return", "func keep<T>():%s { throw 1; }\n"},
        {"unused-local", "func keep<T>() { let pointer:%s; }\n"},
        {"field", "type Host<T> { let pointer:%s; }\n"},
        {"owner-method", "type Host<T> { func keep() { let pointer:%s; } }\n"},
        {"method", "type Host { func keep<T>(value:%s) {} }\n"},
        {"static-method", "type Host { static func keep<T>(value:%s) {} }\n"},
        {"fit", "type Host<T> {}\nfit Host<T> { func keep() { let pointer:%s; } }\n"},
        {"spec-field", "spec Host<T> { let pointer:%s; }\n"},
        {"spec-method", "spec Host<T> { func keep(value:%s):void; }\n"},
        {"callable-parameter", "spec Host<T>(value:%s):void;\n"},
        {"callable-return", "spec Host<T>():%s;\n"},
        {"union-member", "spec Host<T>: %s | int;\n"}
    };
    size_t failures = 0U, count = 0U;
    for (size_t i = 0U; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
        for (size_t j = 0U; j < sizeof(surfaces) / sizeof(surfaces[0]); ++j) {
            char body[512], source[1024], id[96];
            int a = snprintf(body, sizeof(body), surfaces[j].source, shapes[i].shape);
            int b = snprintf(source, sizeof(source), "module g25;\ntype Box<T> {}\n%s", body);
            int c = snprintf(id, sizeof(id), "open-pointer-%zu-%s", i, surfaces[j].id);
            if (a < 0 || (size_t)a >= sizeof(body) || b < 0 || (size_t)b >= sizeof(source) ||
                c < 0 || (size_t)c >= sizeof(id)) {
                fprintf(stderr, "G25 open pointer fixture overflow\n");
                exit(1);
            }
            const G25Case test = {id, source, "AE0333", shapes[i].marker, shapes[i].token,
                                 "cannot be formed from an open generic pointee"};
            failures += !g25_check(&test, NULL);
            ++count;
        }
    }
    static const G25Case boundaries[] = {
        {"open-pointer-constrained", "module g25;\nspec Named {}\nfunc keep<T:Named>(value:T*) {}\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-called-scalar", "module g25;\nfunc keep<T>() { let pointer:T*; }\nfunc use() { keep<int>(); }\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-called-object", "module g25;\ntype Bad { let value:string; }\nfunc keep<T>() { let pointer:T*; }\nfunc use() { keep<Bad>(); }\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-explicit-argument", "module g25;\nfunc id<U>(value:U):U { return value; }\nfunc keep<T>(value:T) { id<T*>(value); }\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-target-owner", "module g25;\ntype Box<T> {}\nfunc keep<T>() { let value=Box<T*>(); }\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-array-target", "module g25;\ntype Box<T> {}\nfunc keep<T>() { let value=Box<T*>[:1]; }\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-cast", "module g25;\nfunc keep<T>(value:T) { let pointer=(T*)value; }\n",
         "AE0333", "T*", "T", NULL},
        {"open-pointer-constraint-reference", "module g25;\ntype Box<T,U:T*> {}\n",
         "AE0333", "T*", "T", NULL},
        {"pointer-unknown-inner", "module g25;\nfunc keep<T>(value:Missing<T>*) {}\n",
         "AE1013", "Missing<T>", "Missing", NULL},
        {"pointer-unknown-nested-argument", "module g25;\ntype Box<T,U> {}\nfunc keep<T>(value:Box<T,Missing>*) {}\n",
         "AE1013", "Missing>", "Missing", NULL},
        {"pointer-array-target-unknown", "module g25;\ntype Box<T> {}\nfunc keep() { let value=Box<Missing>[:1]; }\n",
         "AE1013", "Missing>", "Missing", NULL},
        {"pointer-closed-invalid-index", "module g25;\ntype Box<T> {}\nfunc keep() { let value=Box<int>[1]; }\n",
         "AE1016", "Box<int>", "Box", NULL},
        {"pointer-closed-array-new", "module g25;\ntype Box<T> {}\nfunc keep() { let value=Box<int*>[:1]; }\n",
         NULL, NULL, NULL, NULL},
        {"pointer-closed-unconstrained", "module g25;\nfunc id<T>(value:T):T { return value; }\nfunc keep(value:int*):int* { return id<int*>(id(value)); }\n",
         NULL, NULL, NULL, NULL},
        {"pointer-closed-forwarding", "module g25;\nfunc id<T>(value:T):T { return value; }\nfunc relay<U>(value:U):U { return id<U>(value); }\nfunc keep(value:int*):int* { return relay(value); }\n",
         NULL, NULL, NULL, NULL},
        {"pointer-closed-nested-inference", "module g25;\ntype Box<T> { let value:T; }\nfunc id<T>(value:Box<T[]>):T[] { return value.value; }\nfunc keep(value:Box<int*[]>):int*[] { return id(value); }\n",
         NULL, NULL, NULL, NULL},
        {"pointer-closed-inside-generic", "module g25;\nfunc keep<T>(value:T) { let pointer:int*; }\n",
         NULL, NULL, NULL, NULL}
    };
    for (size_t i = 0U; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        failures += !g25_check(&boundaries[i], NULL);
        ++count;
    }
    static const char *independent = "module g25;\nfunc keep<T>() { let pointer:T*; let value:int=\"wrong\"; }\n";
    const G25Diagnostic expected[] = {
        {"AE0333", "T*", "T", NULL}, {"AE1003", "\"wrong\"", "\"wrong\"", NULL}
    };
    failures += !g25_check_source("open-pointer-independent", independent, NULL, expected, 2U);
    ++count;
    static const char *independent_index = "module g25;\ntype Box<T> {}\nfunc keep<T>() { let value=Box<T*>[missing]; }\n";
    const G25Diagnostic index_errors[] = {
        {"AE0333", "T*", "T", NULL}, {"AE0001", "missing", "missing", NULL},
        {"AE1016", "Box<T*>", "Box", NULL}
    };
    failures += !g25_check_source("open-pointer-independent-index", independent_index, NULL, index_errors, 3U);
    ++count;
    static const char *invalid_index = "module g25;\ntype Box<T> {}\nfunc keep<T>() { let value=Box<T*>[1]; }\n";
    const G25Diagnostic target_errors[] = {
        {"AE0333", "T*", "T", NULL}, {"AE1016", "Box<T*>", "Box", NULL}
    };
    failures += !g25_check_source("open-pointer-invalid-index", invalid_index, NULL, target_errors, 2U);
    ++count;
    for (size_t depth = 1U; depth <= 64U; ++depth) {
        char shape[512], source[1024], id[96];
        for (size_t i = 0U; i < depth; ++i) memcpy(shape + i * 4U, "Box<", 4U);
        memcpy(shape + depth * 4U, "T*", 2U);
        memset(shape + depth * 4U + 2U, '>', depth);
        shape[depth * 5U + 2U] = '\0';
        int a = snprintf(source, sizeof(source),
            "module g25;\ntype Box<T> {}\nfunc keep<T>(value:%s) {}\n", shape);
        int b = snprintf(id, sizeof(id), "open-pointer-nested-%zu", depth);
        if (a < 0 || (size_t)a >= sizeof(source) || b < 0 || (size_t)b >= sizeof(id)) {
            fprintf(stderr, "G25 nested open pointer fixture overflow\n");
            exit(1);
        }
        const G25Case test = {id, source, "AE0333", "T*", "T", NULL};
        failures += !g25_check(&test, NULL);
        ++count;
    }
    if (failures != 0U) {
        fprintf(stderr, "G25 open pointers: %zu of %zu cases failed\n", failures, count);
        exit(1);
    }
    printf("G25: %zu open pointer boundary cases passed\n", count);
}

/* Check the complete actual type at every generic constraint consumption
 * surface. Explicit pointer union members are positive, not pointee proofs. */
static void g25_pointer_constraint_diagnostics(void) {
    static const char *definitions =
        "module g25;\n"
        "spec Named {}\n"
        "spec Extra {}\n"
        "spec Both: Named & Extra;\n"
        "@abi type UserType: Named, Extra { let value:i32; }\n"
        "@abi spec Callback(value:i32):i32;\n"
        "spec ObjectChoice: UserType | int;\n"
        "spec ContractChoice: Named | int;\n"
        "spec PointerChoice: int* | int;\n"
        "spec NestedChoice: PointerChoice | string;\n";
    static const struct {
        const char *id;
        const char *constraint;
        const char *actual;
        bool accepted;
    } actuals[] = {
        {"object-pointer", "Named", "UserType*", false},
        {"object-value", "Named", "UserType", true},
        {"intersection-pointer", "Both", "UserType*", false},
        {"intersection-value", "Both", "UserType", true},
        {"callable-pointer", "Callback", "Callback*", false},
        {"callable-value", "Callback", "Callback", true},
        {"union-object-pointer", "ObjectChoice", "UserType*", false},
        {"union-object-value", "ObjectChoice", "UserType", true},
        {"union-contract-pointer", "ContractChoice", "UserType*", false},
        {"union-contract-value", "ContractChoice", "UserType", true},
        {"union-exact-pointer", "PointerChoice", "int*", true},
        {"union-wrong-pointer", "PointerChoice", "int**", false},
        {"union-nested-pointer", "NestedChoice", "int*", true},
        {"union-nested-wrong", "NestedChoice", "int**", false}
    };
    static const struct {
        const char *id;
        const char *declaration;
        const char *use;
        const char *code;
        const char *marker;
        const char *token;
    } surfaces[] = {
        {"explicit", "func identity<T:%s>(value:T):T { return value; }\n",
            "func use(value:%s) { identity<%s>(value); }\n",
            "AE0512", "identity<%s>", "identity"},
        {"inferred", "func identity<T:%s>(value:T):T { return value; }\n",
            "func use(value:%s) { identity(value); }\n",
            "AE0512", "identity(value)", "identity"},
        {"type", "type Box<T:%s> {}\n",
            "func use(value:Box<%s>) {}\n",
            "AE0710", "%s>", NULL},
        {"object-spec", "spec Box<T:%s> {}\n",
            "func use(value:Box<%s>) {}\n",
            "AE0710", "%s>", NULL},
        {"callable-spec", "spec Box<T:%s>(value:T):T;\n",
            "func use(value:Box<%s>) {}\n",
            "AE0710", "%s>", NULL},
        {"callable-value", "func identity<T:%s>(value:T):T { return value; }\n",
            "spec Mapper(value:%s):%s;\nfunc use() { let mapper:Mapper = identity<%s>; }\n",
            "AE0522", "identity<%s>", "identity"},
        {"cast", "func identity<T:%s>(value:T):T { return value; }\n",
            "spec Mapper(value:%s):%s;\nfunc use() { let mapper = (Mapper)identity<%s>; }\n",
            "AE1023", "(Mapper)", "("},
        {"method-explicit", "type Host { func identity<T:%s>(value:T):T { return value; } }\n",
            "func use(host:Host,value:%s) { host.identity<%s>(value); }\n",
            "AE0512", "identity<%s>", "identity"},
        {"method-inferred", "type Host { func identity<T:%s>(value:T):T { return value; } }\n",
            "func use(host:Host,value:%s) { host.identity(value); }\n",
            "AE0512", "identity(value)", "identity"},
        {"static", "type Host { static func identity<T:%s>(value:T):T { return value; } }\n",
            "func use(value:%s) { Host.identity<%s>(value); }\n",
            "AE0512", "identity<%s>", "identity"},
        {"fit", "type Host {}\nfit Host { func identity<T:%s>(value:T):T { return value; } }\n",
            "func use(host:Host,value:%s) { host.identity<%s>(value); }\n",
            "AE0512", "identity<%s>", "identity"}
    };
    size_t failures = 0U, count = 0U;
    for (size_t i = 0U; i < sizeof(actuals) / sizeof(actuals[0]); ++i) {
        for (size_t j = 0U; j < sizeof(surfaces) / sizeof(surfaces[0]); ++j) {
            char source[4096], declaration[512], use[512], id[112], marker[128], token[64];
            int a = snprintf(declaration, sizeof(declaration), surfaces[j].declaration,
                             actuals[i].constraint);
            int b = snprintf(use, sizeof(use), surfaces[j].use,
                             actuals[i].actual, actuals[i].actual, actuals[i].actual);
            int c = snprintf(source, sizeof(source), "%s%s%s", definitions, declaration, use);
            int d = snprintf(id, sizeof(id), "pointer-%s-%s", actuals[i].id, surfaces[j].id);
            int e = snprintf(marker, sizeof(marker), surfaces[j].marker, actuals[i].actual);
            size_t token_length = strcspn(actuals[i].actual, "*");
            if (a < 0 || (size_t)a >= sizeof(declaration) || b < 0 || (size_t)b >= sizeof(use) ||
                c < 0 || (size_t)c >= sizeof(source) || d < 0 || (size_t)d >= sizeof(id) ||
                e < 0 || (size_t)e >= sizeof(marker) || token_length >= sizeof(token)) {
                fprintf(stderr, "G25 pointer constraint fixture overflow\n");
                exit(1);
            }
            memcpy(token, actuals[i].actual, token_length);
            token[token_length] = '\0';
            const G25Case test = {id, source, actuals[i].accepted ? NULL : surfaces[j].code,
                marker, surfaces[j].token != NULL ? surfaces[j].token : token, NULL};
            failures += !g25_check(&test, NULL);
            ++count;
        }
    }
    if (failures != 0U) {
        fprintf(stderr, "G25 pointer constraints: %zu of %zu cases failed\n", failures, count);
        exit(1);
    }
    printf("G25: %zu pointer constraint cases passed\n", count);
}

/* GENERIC01–05: minimal independent illegal programs and legal boundaries. */
void test_g25_generic_diagnostics(void) {
    static const G25Case cases[] = {
        {
            "19-spec-arity-direct",
            "module g25;\n"
            "spec View { func tag():i32; }\n"
            "spec View<T> { func read():T; }\n"
            "type Item:View,View<i32> { func tag():i32 { return 1; } func read():i32 { return 2; } }\n"
            "func use(value:Item) { let plain:View=value; let typed:View<i32> =value; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "19-spec-arity-reversed",
            "module g25;\n"
            "spec View<T> { func read():T; }\n"
            "spec View { func tag():i32; }\n"
            "type Item:View,View<i32> { func tag():i32 { return 1; } func read():i32 { return 2; } }\n"
            "func use(value:Item) { let plain:View=value; let typed:View<i32> = value; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "19-spec-arity-fit",
            "module g25;\n"
            "spec View { func tag():i32; }\n"
            "spec View<T> { func read():T; }\n"
            "type Item { func tag():i32 { return 1; } func read():i32 { return 2; } }\n"
            "fit Item:View,View<i32>;\n"
            "func use(value:Item) { let plain:View=value; let typed:View<i32> = value; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "19-spec-arity-parent",
            "module g25;\n"
            "spec View { func tag():i32; }\n"
            "spec View<T> { func read():T; }\n"
            "spec Child<T>:View,View<T> {}\n"
            "type Item:Child<i32> { func tag():i32 { return 1; } func read():i32 { return 2; } }\n"
            "func use(value:Item) { let plain:View=value; let typed:View<i32> = value; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "20-callable-constraint-compatible",
            "module g25;\n"
            "spec Mapper<T>(value:T):T;\n"
            "spec Exact(value:i32):i32;\n"
            "func keep<T:Mapper<i32>>(value:T):T { return value; }\n"
            "func use(value:Exact) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "20-callable-owner-compatible",
            "module g25;\n"
            "spec Mapper<T>(value:T):T;\n"
            "spec Exact(value:i32):i32;\n"
            "type Box<T:Mapper<i32>> { let value:T; }\n"
            "func use(value:Box<Exact>) {}\n",
            NULL, NULL, NULL, NULL
        },
        {
            "20-callable-constraint-mismatch",
            "module g25;\n"
            "spec Mapper<T>(value:T):T;\n"
            "spec Wrong(value:string):string;\n"
            "func keep<T:Mapper<i32>>(value:T):T { return value; }\n"
            "func use(value:Wrong) { let result=keep(value); }\n",
            "AE0512", "keep(value);", "keep", NULL
        },
        {
            "04A-eight-levels",
            "module g25;\n"
            "type Box<T> { let value:T; }\n"
            "func keep<T>(value:Box<Box<Box<Box<Box<Box<Box<Box<T[]>>>>>>>>):Box<Box<Box<Box<Box<Box<Box<Box<T[]>>>>>>>> { return value; }\n"
            "func use(value:Box<Box<Box<Box<Box<Box<Box<Box<i32[]>>>>>>>>) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03H-box_assign_covariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Box<Dog>) { let result: Box<Animal> = value; }\n",
            "AE1003", "value;", "value", NULL
        },
        {
            "03H-box_assign_contravariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Box<Animal>) { let result: Box<Dog> = value; }\n",
            "AE1003", "value;", "value", NULL
        },
        {
            "03H-box_call_covariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func accept(value: Box<Animal>) {}\n"
            "func use(value: Box<Dog>) { accept(value); }\n",
            "AE0512", "accept(value)", "accept", NULL
        },
        {
            "03H-box_call_contravariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func accept(value: Box<Dog>) {}\n"
            "func use(value: Box<Animal>) { accept(value); }\n",
            "AE0512", "accept(value)", "accept", NULL
        },
        {
            "03H-box_cast_covariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Box<Dog>) { let result = (Box<Animal>)value; }\n",
            "AE1023", "(Box<Animal>)", "(", NULL
        },
        {
            "03H-box_cast_contravariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Box<Animal>) { let result = (Box<Dog>)value; }\n",
            "AE1023", "(Box<Dog>)", "(", NULL
        },
        {
            "03H-producer_assign_covariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Producer<Dog>) { let result: Producer<Animal> = value; }\n",
            "AE0522", "value;", "value", NULL
        },
        {
            "03H-consumer_assign_contravariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Consumer<Animal>) { let result: Consumer<Dog> = value; }\n",
            "AE0522", "value;", "value", NULL
        },
        {
            "03H-producer_cast_covariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Producer<Dog>) { let result = (Producer<Animal>)value; }\n",
            "AE1023", "(Producer<Animal>)", "(", NULL
        },
        {
            "03H-consumer_cast_contravariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(value: Consumer<Animal>) { let result = (Consumer<Dog>)value; }\n",
            "AE1023", "(Consumer<Dog>)", "(", NULL
        },
        {
            "03H-box_same",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func accept(value: Box<Dog>) {}\n"
            "func use(value: Box<Dog>) { let result: Box<Dog> = value; accept(result); let copy = (Box<Dog>)value; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03H-callable_same",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func use(produce: Producer<Dog>, consume: Consumer<Dog>) { let p: Producer<Dog> = produce; let c: Consumer<Dog> = consume; let pc = (Producer<Dog>)produce; let cc = (Consumer<Dog>)consume; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03H-generic_call_covariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func accept<T>(value: Box<T>) {}\n"
            "func use(value: Box<Dog>) { accept<Animal>(value); }\n",
            "AE0512", "accept<Animal>(value)", "accept", NULL
        },
        {
            "03H-generic_call_contravariant",
            "module g25.variance;\n"
            "spec Animal {}\n"
            "spec Dog: Animal {}\n"
            "type Box<T> { let value: T; }\n"
            "spec Producer<T>(): T;\n"
            "spec Consumer<T>(value: T): void;\n"
            "func accept<T>(value: Box<T>) {}\n"
            "func use(value: Box<Animal>) { accept<Dog>(value); }\n",
            "AE0512", "accept<Dog>(value)", "accept", NULL
        },
        {
            "01A-distinct-type",
            "module g25;\n"
            "type Box<T,U> { let first:T; let second:U; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01A-distinct-spec",
            "module g25;\n"
            "spec Read<T,U> { func read(value:T):U; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01A-distinct-callable",
            "module g25;\n"
            "spec Read<T,U>(value:T):U;\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01A-distinct-function",
            "module g25;\n"
            "func pick<T,U>(left:T,right:U):U { return right; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01A-distinct-fit",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func pick<T,U>(left:T,right:U):U { return right; } }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01G-fit-return",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func pick<T>(value:T):i32 { return 1; } func pick<U>(value:U):string { return \"x\"; } }\n",
            "AE0802", "pick<U>", "pick", NULL
        },
        {
            "01G-fit-variadic",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func pick<T>(value:T...) {} func pick<U>(value:U) {} }\n",
            "AE0803", "pick<U>", "pick", NULL
        },
        {
            "02A-type-parent",
            "module g25;\n"
            "spec Parent<T,U> {}\n"
            "type Child: Parent<i32> {}\n",
            "AE1015", "Parent<i32>", "Parent", NULL
        },
        {
            "02D-type-plain-first",
            "module g25;\n"
            "type Item {}\n"
            "type Item<T,U> {}\n"
            "func use(value:Item<i32>) {}\n",
            "AE1015", "Item<i32>", "Item", NULL
        },
        {
            "02D-type-generic-first",
            "module g25;\n"
            "type Item<T,U> {}\n"
            "type Item {}\n"
            "func use(value:Item<i32>) {}\n",
            "AE1015", "Item<i32>", "Item", NULL
        },
        {
            "02D-spec-plain-first",
            "module g25;\n"
            "spec Item {}\n"
            "spec Item<T,U> {}\n"
            "func use(value:Item<i32>) {}\n",
            "AE1015", "Item<i32>", "Item", NULL
        },
        {
            "02D-spec-generic-first",
            "module g25;\n"
            "spec Item<T,U> {}\n"
            "spec Item {}\n"
            "func use(value:Item<i32>) {}\n",
            "AE1015", "Item<i32>", "Item", NULL
        },
        {
            "02D-call-one-first",
            "module g25;\n"
            "func choose<T>(value:T) {}\n"
            "func choose<T,U,V>(value:T) {}\n"
            "func use(value:i32) { choose<i32,string>(value); }\n",
            "AE1015", "choose<i32,", "choose", "1, 3"
        },
        {
            "02D-call-three-first",
            "module g25;\n"
            "func choose<T,U,V>(value:T) {}\n"
            "func choose<T>(value:T) {}\n"
            "func use(value:i32) { choose<i32,string>(value); }\n",
            "AE1015", "choose<i32,", "choose", "1, 3"
        },
        {
            "02D-matching-arity-bad-value",
            "module g25;\n"
            "func choose<T>(value:string) {}\n"
            "func choose<T,U>(value:i32) {}\n"
            "func use(value:i32) { choose<i32>(value); }\n",
            "AE0512", "choose<i32>", "choose", NULL
        },
        {
            "03F-unprovided-static",
            "module g25;\n"
            "spec Bound {}\n"
            "func use<T:Bound>() { let value = T.missing; }\n",
            "AE1008", "missing;", "missing", NULL
        },
        {
            "03F-unprovided-static-call",
            "module g25;\n"
            "spec Bound {}\n"
            "func use<T:Bound>() { T.missing(); }\n",
            "AE0512", "missing()", "missing", NULL
        },
        {
            "03F-static-provided",
            "module g25;\n"
            "spec Bound { static func get():i32; }\n"
            "func use<T:Bound>():i32 { return T.get(); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03F-unconstrained",
            "module g25;\n"
            "func use<T>(value:T) { value.missing; }\n",
            "AE0306", "missing;", "missing", NULL
        },
        {
            "03F-field-provided",
            "module g25;\n"
            "spec Bound { let value:i32; }\n"
            "func use<T:Bound>(source:T):i32 { return source.value; }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03F-method-provided",
            "module g25;\n"
            "spec Bound { func get():i32; }\n"
            "func use<T:Bound>(source:T):i32 { return source.get(); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03F-method-arguments",
            "module g25;\n"
            "spec Bound { func take(value:i32):void; }\n"
            "func use<T:Bound>(source:T) { source.take(\"wrong\"); }\n",
            "AE0512", "take(\"wrong\")", "take", NULL
        },
        {
            "03F-callable-invocation",
            "module g25;\n"
            "spec Bound(value:i32):i32;\n"
            "func use<T:Bound>(source:T):i32 { return source(23); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-array",
            "module g25;\n"
            "func keep<T>(value:T[]):T[] { return value; }\n"
            "func use(value:i32[]) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-deep",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func keep<T>(value:Box<Box<T[]>>):Box<Box<T[]>> { return value; }\n"
            "func use(value:Box<Box<i32[]>>) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-array-rank",
            "module g25;\n"
            "func keep<T>(value:T[][]):T[][] { return value; }\n"
            "func use(value:i32[][]) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-pointer",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func keep<T>(value:Box<T[]>):Box<T[]> { return value; }\n"
            "func use(value:Box<i32*[]>) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-whole-slot",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func keep<T>(value:Box<T>):T { return value.value; }\n"
            "func use(value:Box<Box<i32[]>>) { let result:Box<i32[]> = keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-multiple",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "type Pair<A,B> { let first: A; let second: B; }\n"
            "func keep<T,U>(value:Pair<Box<T[]>,Box<U>>):Pair<Box<T[]>,Box<U>> { return value; }\n"
            "func use(value:Pair<Box<i32[]>,Box<string>>) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-repeated-slot",
            "module g25;\n"
            "type Pair<A,B> { let first: A; let second: B; }\n"
            "func keep<T>(value:Pair<T,T>):Pair<T,T> { return value; }\n"
            "func use(value:Pair<i32,i32>) { let result=keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-open-forward",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func keep<T>(value:Box<T[]>):Box<T[]> { return value; }\n"
            "func use<U>(value:Box<U[]>):Box<U[]> { return keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-owner-method",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "type Pair<A,B> { let first: A; let second: B; }\n"
            "type Host<T> { func keep<U>(value:Pair<T,Box<U[]>>):Pair<T,Box<U[]>> { return value; } }\n"
            "func use(host:Host<string>,value:Pair<string,Box<i32[]>>) { let result=host.keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-owner-static",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "type Pair<A,B> { let first: A; let second: B; }\n"
            "type Host<T> { static func keep<U>(value:Pair<T,Box<U[]>>):Pair<T,Box<U[]>> { return value; } }\n"
            "func use(value:Pair<string,Box<i32[]>>) { let result=Host<string>.keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-owner-fit",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "type Pair<A,B> { let first: A; let second: B; }\n"
            "type Host<T> {}\n"
            "fit Host<T> { func keep<U>(value:Pair<T,Box<U[]>>):Pair<T,Box<U[]>> { return value; } }\n"
            "func use(host:Host<string>,value:Pair<string,Box<i32[]>>) { let result=host.keep(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04A-identity-mismatch",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "type Other<T> {}\n"
            "func keep<T>(value:Box<T>) {}\n"
            "func use(value:Other<i32>) { keep(value); }\n",
            "AE0512", "keep(value);", "keep", NULL
        },
        {
            "04A-writable-mismatch",
            "module g25;\n"
            "func keep<T>(value:T[!]) {}\n"
            "func use(value:i32[]) { keep(value); }\n",
            "AE0512", "keep(value);", "keep", NULL
        },
        {
            "04A-readonly-mismatch",
            "module g25;\n"
            "func keep<T>(value:T[]) {}\n"
            "func use(value:i32[!]) { keep(value); }\n",
            "AE0512", "keep(value);", "keep", NULL
        },
        {
            "04E-deep-conflict",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func keep<T>(left:Box<Box<T[]>>,right:Box<T>) {}\n"
            "func use(left:Box<Box<i32[]>>,right:Box<string>) { keep(left,right); }\n",
            "AE0512", "keep(left,right)", "keep", NULL
        },
        {
            "04E-slot-conflict",
            "module g25;\n"
            "type Pair<A,B> { let first: A; let second: B; }\n"
            "func keep<T>(value:Pair<T,T>) {}\n"
            "func use(value:Pair<i32,string>) { keep(value); }\n",
            "AE0512", "keep(value);", "keep", NULL
        },
        {
            "04E-direct-nested-conflict",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func keep<T>(left:T,right:Box<T>) {}\n"
            "func use(left:i32,right:Box<string>) { keep(left,right); }\n",
            "AE0512", "keep(left,right)", "keep", NULL
        },
        {
            "04E-no-common-parent",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "spec Animal {}\n"
            "spec Dog:Animal {}\n"
            "func keep<T>(left:Box<T>,right:Box<T>) {}\n"
            "func use(left:Box<Dog>,right:Box<Animal>) { keep(left,right); }\n",
            "AE0512", "keep(left,right)", "keep", NULL
        },
        {
            "04E-inferred-fallback",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func choose<T>(left:Box<T>,right:Box<T>):i32 { return 1; }\n"
            "func choose(left:Box<i32>,right:Box<string>):i32 { return 2; }\n"
            "func use(left:Box<i32>,right:Box<string>) { let result=choose(left,right); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04F-deep-ambiguous",
            "module g25;\n"
            "type Box<T> { let value: T; }\n"
            "func choose<T>(left:T,right:Box<i32>) {}\n"
            "func choose<T>(left:i32,right:Box<T>) {}\n"
            "func use(value:i32,box:Box<i32>) { choose(value,box); }\n",
            "AE0511", "choose(value,box)", "choose", NULL
        },
        {
            "03E-fallback-generic",
            "module g25;\n"
            "spec Bound {}\n"
            "func choose<T:Bound>(left:T,right:i32):i32 { return 1; }\n"
            "func choose<T>(left:T,right:T):i32 { return 2; }\n"
            "func use(value:i32) { let result=choose(value,value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "12-explicit-outer-name",
            "module g25;\n"
            "spec Field<A> { var item:A; }\n"
            "spec Tag {}\n"
            "spec Both<A>:Field<A> & Tag;\n"
            "type Good<A>:Field<A>,Tag { open var item:A; }\n"
            "func take<A>(value:Both<A>) {}\n"
            "func use<A>(value:Good<A>) { take<A>(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03B-builtin",
            "module g25;\n"
            "func pick<T: i32>(value: T) {}\n",
            "AE0709", "T: i32", "T", NULL
        },
        {
            "03B-array",
            "module g25;\n"
            "spec Bound {}\n"
            "func pick<T: Bound[]>(value: T) {}\n",
            "AE0709", "T: Bound", "T", NULL
        },
        {
            "03B-pointer",
            "module g25;\n"
            "func pick<T: i32*>(value: T) {}\n",
            "AE0709", "T: i32", "T", NULL
        },
        {
            "01G-method-constraint",
            "module g25;\n"
            "spec A {}\n"
            "spec B {}\n"
            "type Host { func pick<T: A>(value: T) {} func pick<U: B>(value: U) {} }\n",
            "AE0508", "pick<U:", "pick", NULL
        },
        {
            "01G-fit-constraint",
            "module g25;\n"
            "spec A {}\n"
            "spec B {}\n"
            "type Host {}\n"
            "fit Host { func pick<T: A>(value: T) {} func pick<U: B>(value: U) {} }\n",
            "AE0801", "pick<U:", "pick", NULL
        },
        {
            "01G-nested-alpha",
            "module g25;\n"
            "type Box<T> {}\n"
            "func pick<T>(value: Box<T[]>): T { let result: T; return result; }\n"
            "func pick<U>(value: Box<U[]>): U { let result: U; return result; }\n",
            "AE0508", "pick<U>", "pick", NULL
        },
        {
            "01G-return-only",
            "module g25;\n"
            "func pick<T>(value: T): i32 { return 1; }\n"
            "func pick<U>(value: U): string { return \"x\"; }\n",
            "AE0509", "pick<U>", "pick", NULL
        },
        {
            "01G-variadic",
            "module g25;\n"
            "func pick<T>(value: T...) {}\n"
            "func pick<U>(value: U) {}\n",
            "AE0510", "pick<U>", "pick", NULL
        },
        {
            "01G-different-slots",
            "module g25;\n"
            "func pick<T,U>(value: T) {}\n"
            "func pick<A,B>(value: B) {}\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03C-callable-open-bad",
            "module g25;\n"
            "spec A(value: i32): i32;\n"
            "spec B(value: string): string;\n"
            "type Box<T: A> {}\n"
            "func use<T: B>(value: Box<T>) {}\n",
            "AE0710", "T>)", "T", NULL
        },
        {
            "03C-callable-open-good",
            "module g25;\n"
            "spec A(value: i32): i32;\n"
            "type Box<T: A> {}\n"
            "func use<T: A>(value: Box<T>) {}\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03F-unprovided-field",
            "module g25;\n"
            "spec Bound {}\n"
            "func use<T: Bound>(value: T) { value.missing; }\n",
            "AE1008", "missing;", "missing", NULL
        },
        {
            "03F-unprovided-method",
            "module g25;\n"
            "spec Bound {}\n"
            "func use<T: Bound>(value: T) { value.missing(); }\n",
            "AE1008", "missing();", "missing", NULL
        },
        {
            "05B-value-unknown",
            "module g25;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "spec Handler(value: i32): i32;\n"
            "func use() { let value: Handler = pick<Missing>; }\n",
            "AE1013", "Missing", "Missing", NULL
        },
        {
            "05B-cast-unknown",
            "module g25;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "spec Handler(value: i32): i32;\n"
            "func use() { let value = (Handler)pick<Missing>; }\n",
            "AE1013", "Missing", "Missing", NULL
        },
        {
            "01A-type",
            "module g25;\n"
            "type Box<T, T> {}\n",
            "AE1017", "T>", "T", NULL
        },
        {
            "01A-function",
            "module g25;\n"
            "func duplicate<T, T>() {}\n",
            "AE1017", "T>", "T", NULL
        },
        {
            "01A-object-spec",
            "module g25;\n"
            "spec Reader<T, T> {}\n",
            "AE1017", "T>", "T", NULL
        },
        {
            "01A-callable-spec",
            "module g25;\n"
            "spec Reader<T, T>(value: T): T;\n",
            "AE1017", "T>", "T", NULL
        },
        {
            "01A-method",
            "module g25;\n"
            "type Host { func method<T, T>() {} }\n",
            "AE1017", "T>", "T", NULL
        },
        {
            "01A-fit",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func method<T, T>() {} }\n",
            "AE1017", "T>", "T", NULL
        },
        {
            "01B-shadow",
            "module g25;\n"
            "type Host<T> { func method<T>(value: T): T { return value; } }\n",
            "AE1017", "T>(value", "T", NULL
        },
        {
            "01B-distinct",
            "module g25;\n"
            "type Host<T> { func method<U>(owner: T, value: U): U { return value; } }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01C-constructor",
            "module g25;\n"
            "type Host<T> { func Host<U>() {} }\n",
            "AE0316", "U>", "U", NULL
        },
        {
            "01C-finalizer",
            "module g25;\n"
            "type Host<T> { func ~Host<U>() {} }\n",
            "AE0316", "U>", "U", NULL
        },
        {
            "01C-owner",
            "module g25;\n"
            "type Host<T> { let value: T; func Host(value: T) { self.value = value; } func ~Host() { let copy: T = self.value; } }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "01E-type-name",
            "module g25;\n"
            "type Box<T> {}\n"
            "type Box<U> {}\n",
            "AE0213", "Box<U>", "Box", NULL
        },
        {
            "01E-type-constraint",
            "module g25;\n"
            "spec A {}\n"
            "spec B {}\n"
            "type Box<T: A> {}\n"
            "type Box<U: B> {}\n",
            "AE0213", "Box<U:", "Box", NULL
        },
        {
            "01E-spec-name",
            "module g25;\n"
            "spec Box<T> {}\n"
            "spec Box<U> {}\n",
            "AE0213", "Box<U>", "Box", NULL
        },
        {
            "01E-spec-constraint",
            "module g25;\n"
            "spec A {}\n"
            "spec B {}\n"
            "spec Box<T: A> {}\n"
            "spec Box<U: B> {}\n",
            "AE0213", "Box<U:", "Box", NULL
        },
        {
            "01F-category",
            "module g25;\n"
            "type Item<T> {}\n"
            "spec Item<T, U> {}\n",
            "AE0004", "Item<T, U>", "Item", NULL
        },
        {
            "01G-function-name",
            "module g25;\n"
            "func pick<T>(value: T) {}\n"
            "func pick<U>(value: U) {}\n",
            "AE0508", "pick<U>", "pick", NULL
        },
        {
            "01G-function-constraint",
            "module g25;\n"
            "spec A {}\n"
            "spec B {}\n"
            "func pick<T: A>(value: T) {}\n"
            "func pick<U: B>(value: U) {}\n",
            "AE0508", "pick<U:", "pick", NULL
        },
        {
            "01G-method-name",
            "module g25;\n"
            "type Host { func pick<T>(value: T) {} func pick<U>(value: U) {} }\n",
            "AE0508", "pick<U>", "pick", NULL
        },
        {
            "01G-fit-name",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func pick<T>(value: T) {} func pick<U>(value: U) {} }\n",
            "AE0801", "pick<U>", "pick", NULL
        },
        {
            "02A-field-few",
            "module g25;\n"
            "type Box<T, U> {}\n"
            "type Host { let value: Box<i32>; }\n",
            "AE1015", "Box<i32>", "Box", NULL
        },
        {
            "02A-parameter-many",
            "module g25;\n"
            "type Box<T> {}\n"
            "func use(value: Box<i32, string>) {}\n",
            "AE1015", "Box<i32,", "Box", NULL
        },
        {
            "02A-return",
            "module g25;\n"
            "type Box<T, U> {}\n"
            "spec Producer(): Box<i32>;\n",
            "AE1015", "Box<i32>", "Box", NULL
        },
        {
            "02A-parent",
            "module g25;\n"
            "spec Parent<T, U> {}\n"
            "spec Child: Parent<i32> {}\n",
            "AE1015", "Parent<i32>", "Parent", NULL
        },
        {
            "02A-fit",
            "module g25;\n"
            "type Box<T> {}\n"
            "fit Box<i32, string> {}\n",
            "AE1015", "Box<i32,", "Box", NULL
        },
        {
            "02A-nested",
            "module g25;\n"
            "type Box<T> {}\n"
            "type Outer<T> {}\n"
            "func use(value: Outer<Box<i32, string>>) {}\n",
            "AE1015", "Box<i32,", "Box", NULL
        },
        {
            "02A-array",
            "module g25;\n"
            "spec Box<T, U> {}\n"
            "func use(value: Box<i32>[]) {}\n",
            "AE1015", "Box<i32>", "Box", NULL
        },
        {
            "02B-constructor-few",
            "module g25;\n"
            "type Box<T, U> {}\n"
            "func use() { let value = Box<i32>(); }\n",
            "AE1015", "Box<i32>", "Box", NULL
        },
        {
            "02B-constructor-many",
            "module g25;\n"
            "type Box<T> { func Box(value: T) {} }\n"
            "func use() { let value = Box<i32, string>(1); }\n",
            "AE1015", "Box<i32,", "Box", NULL
        },
        {
            "02C-function-few",
            "module g25;\n"
            "func pick<T, U>(value: T) {}\n"
            "func use() { pick<i32>(1); }\n",
            "AE1015", "pick<i32>", "pick", NULL
        },
        {
            "02C-function-many",
            "module g25;\n"
            "func pick<T>(value: T) {}\n"
            "func use() { pick<i32, string>(1); }\n",
            "AE1015", "pick<i32,", "pick", NULL
        },
        {
            "02C-method-few",
            "module g25;\n"
            "type Host { func pick<T, U>(value: T) {} }\n"
            "func use(host: Host) { host.pick<i32>(1); }\n",
            "AE1015", "pick<i32>", "pick", NULL
        },
        {
            "02C-static-many",
            "module g25;\n"
            "type Host { static func pick<T>(value: T) {} }\n"
            "func use() { Host.pick<i32, string>(1); }\n",
            "AE1015", "pick<i32,", "pick", NULL
        },
        {
            "02C-fit-many",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func pick<T>(value: T) {} }\n"
            "func use(host: Host) { host.pick<i32, string>(1); }\n",
            "AE1015", "pick<i32,", "pick", NULL
        },
        {
            "02C-fit-static-few",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { static func pick<T, U>(value: T) {} }\n"
            "func use() { Host.pick<i32>(1); }\n",
            "AE1015", "pick<i32>", "pick", NULL
        },
        {
            "02E-constraint-arity",
            "module g25;\n"
            "spec Bound<T, U> {}\n"
            "func pick<T: Bound<i32>>(value: T) {}\n",
            "AE1015", "Bound<i32>", "Bound", NULL
        },
        {
            "03A-unknown",
            "module g25;\n"
            "func pick<T: Missing>(value: T) {}\n",
            "AE1013", "Missing", "Missing", NULL
        },
        {
            "03B-type",
            "module g25;\n"
            "type Bound {}\n"
            "func pick<T: Bound>(value: T) {}\n",
            "AE0709", "T: Bound", "T", NULL
        },
        {
            "03B-tuple",
            "module g25;\n"
            "type Bound(i32, i32);\n"
            "func pick<T: Bound>(value: T) {}\n",
            "AE0709", "T: Bound", "T", NULL
        },
        {
            "03B-enum",
            "module g25;\n"
            "enum Bound { First }\n"
            "func pick<T: Bound>(value: T) {}\n",
            "AE0709", "T: Bound", "T", NULL
        },
        {
            "03C-type-owner",
            "module g25;\n"
            "spec Bound {}\n"
            "type Other {}\n"
            "type Box<T: Bound> {}\n"
            "func use(value: Box<Other>) {}\n",
            "AE0710", "Other>)", "Other", NULL
        },
        {
            "03C-spec-owner",
            "module g25;\n"
            "spec Bound {}\n"
            "type Other {}\n"
            "spec Box<T: Bound> {}\n"
            "func use(value: Box<Other>) {}\n",
            "AE0710", "Other>)", "Other", NULL
        },
        {
            "03C-callable-owner",
            "module g25;\n"
            "spec Bound(value: i32): i32;\n"
            "type Box<T: Bound> {}\n"
            "func use(value: Box<string>) {}\n",
            "AE0710", "string>)", "string", NULL
        },
        {
            "03D-none",
            "module g25;\n"
            "spec Bound {}\n"
            "type Box<T: Bound> {}\n"
            "func use<T>(value: Box<T>) {}\n",
            "AE0710", "T>)", "T", NULL
        },
        {
            "03D-weaker",
            "module g25;\n"
            "spec Base {}\n"
            "spec Strong: Base {}\n"
            "type Box<T: Strong> {}\n"
            "func use<T: Base>(value: Box<T>) {}\n",
            "AE0710", "T>)", "T", NULL
        },
        {
            "03D-unrelated",
            "module g25;\n"
            "spec A {}\n"
            "spec B {}\n"
            "type Box<T: A> {}\n"
            "func use<T: B>(value: Box<T>) {}\n",
            "AE0710", "T>)", "T", NULL
        },
        {
            "03D-parent-none",
            "module g25;\n"
            "spec Bound {}\n"
            "spec Parent<T: Bound> {}\n"
            "spec Child<T>: Parent<T> {}\n",
            "AE0710", "T> {}", "T", NULL
        },
        {
            "03D-same",
            "module g25;\n"
            "spec Bound {}\n"
            "type Box<T: Bound> {}\n"
            "func use<T: Bound>(value: Box<T>) {}\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03D-strong",
            "module g25;\n"
            "spec Base {}\n"
            "spec Strong: Base {}\n"
            "spec Parent<T: Base> {}\n"
            "spec Child<T: Strong>: Parent<T> {}\n",
            NULL, NULL, NULL, NULL
        },
        {
            "03E-function-inferred",
            "module g25;\n"
            "spec Bound {}\n"
            "func pick<T: Bound>(value: T) {}\n"
            "func use(value: i32) { pick(value); }\n",
            "AE0512", "pick(value)", "pick", NULL
        },
        {
            "03E-function-explicit",
            "module g25;\n"
            "spec Bound {}\n"
            "func pick<T: Bound>(value: T) {}\n"
            "func use(value: i32) { pick<i32>(value); }\n",
            "AE0512", "pick<i32>", "pick", NULL
        },
        {
            "03E-method-inferred",
            "module g25;\n"
            "spec Bound {}\n"
            "type Host { func pick<T: Bound>(value: T) {} }\n"
            "func use(host: Host, value: i32) { host.pick(value); }\n",
            "AE0512", "pick(value)", "pick", NULL
        },
        {
            "03E-static-explicit",
            "module g25;\n"
            "spec Bound {}\n"
            "type Host { static func pick<T: Bound>(value: T) {} }\n"
            "func use(value: i32) { Host.pick<i32>(value); }\n",
            "AE0512", "pick<i32>", "pick", NULL
        },
        {
            "03H-assignment",
            "module g25;\n"
            "type Box<T> {}\n"
            "func use(value: Box<i32>) { let other: Box<string> = value; }\n",
            "AE1003", "value;", "value", NULL
        },
        {
            "03H-cast",
            "module g25;\n"
            "type Box<T> {}\n"
            "func use(value: Box<i32>) { let other = (Box<string>)value; }\n",
            "AE1023", "(Box<string>)", "(", NULL
        },
        {
            "04C-missing",
            "module g25;\n"
            "func make<T>(): T { let value: T; return value; }\n"
            "func use() { let value = make(); }\n",
            "AE0525", "make();", "make", "type argument 0"
        },
        {
            "04C-target",
            "module g25;\n"
            "func make<T>(): T { let value: T; return value; }\n"
            "func use() { let value: i32 = make(); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04D-partial",
            "module g25;\n"
            "func pick<T, U, V>(value: U): U { return value; }\n"
            "func use(value: i32) { let result = pick(value); }\n",
            "AE0525", "pick(value);", "pick", "type argument 0"
        },
        {
            "04D-middle",
            "module g25;\n"
            "func pick<T, U, V>(value: T): T { return value; }\n"
            "func use(value: i32) { let result = pick(value); }\n",
            "AE0525", "pick(value);", "pick", "type argument 1"
        },
        {
            "04D-explicit",
            "module g25;\n"
            "func pick<T, U, V>(value: U): U { return value; }\n"
            "func use(value: i32) { let result = pick<bool, i32, string>(value); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04E-conflict",
            "module g25;\n"
            "func pick<T>(left: T, right: T): T { return left; }\n"
            "func use(left: i32, right: string) { let result = pick(left, right); }\n",
            "AE0512", "pick(left,", "pick", NULL
        },
        {
            "04E-method-conflict",
            "module g25;\n"
            "type Host { func pick<T>(left: T, right: T): T { return left; } }\n"
            "func use(host: Host, left: i32, right: string) { let result = host.pick(left, right); }\n",
            "AE0512", "pick(left,", "pick", NULL
        },
        {
            "04E-same",
            "module g25;\n"
            "func pick<T>(left: T, right: T): T { return left; }\n"
            "func use(left: i32, right: i32) { let result = pick(left, right); }\n",
            NULL, NULL, NULL, NULL
        },
        {
            "04F-ambiguous",
            "module g25;\n"
            "func pick<T>(left: T, right: i32) {}\n"
            "func pick<T>(left: i32, right: T) {}\n"
            "func use(value: i32) { pick(value, value); }\n",
            "AE0511", "pick(value,", "pick", NULL
        },
        {
            "04G-unclosed-value",
            "module g25;\n"
            "spec Handler(value: i32): i32;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "func use() { let value: Handler = pick; }\n",
            "AE0522", "pick;", "pick", NULL
        },
        {
            "04G-unclosed-cast",
            "module g25;\n"
            "spec Handler(value: i32): i32;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "func use() { let value = (Handler)pick; }\n",
            "AE1023", "(Handler)", "(", NULL
        },
        {
            "04G-closed-mismatch",
            "module g25;\n"
            "spec Handler(value: i32): i32;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "func use() { let value: Handler = pick<string>; }\n",
            "AE0522", "pick<string>", "pick", NULL
        },
        {
            "04H-bare-constructor",
            "module g25;\n"
            "type Box<T> { func Box(value: T) {} }\n"
            "func use() { let value: Box<i32> = Box(1); }\n",
            "AE0315", "Box(1)", "Box", NULL
        },
        {
            "04H-bare-static",
            "module g25;\n"
            "type Box<T> { static func make(value: T): T { return value; } }\n"
            "func use(value: i32) { let result = Box.make(value); }\n",
            "AE0512", "make(value);", "make", NULL
        },
        {
            "05A-type-param",
            "module g25;\n"
            "func use<T>(value: T<i32>) {}\n",
            "AE1012", "T<i32>", "T", NULL
        },
        {
            "05A-construct-param",
            "module g25;\n"
            "func use<T>() { let value = T<i32>(); }\n",
            "AE1012", "T<i32>", "T", NULL
        },
        {
            "05B-unknown-arg",
            "module g25;\n"
            "func pick<T>(value: T) {}\n"
            "func use() { pick<Missing>(1); }\n",
            "AE1013", "Missing", "Missing", NULL
        },
        {
            "05B-unknown-nested",
            "module g25;\n"
            "type Box<T> {}\n"
            "func use(value: Box<Missing>) {}\n",
            "AE1013", "Missing", "Missing", NULL
        },
        {
            "05C-type",
            "module g25;\n"
            "type Box {}\n"
            "func use(value: Box<i32>) {}\n",
            "AE1014", "Box<i32>", "Box", NULL
        },
        {
            "05C-constructor",
            "module g25;\n"
            "type Box {}\n"
            "func use() { let value = Box<i32>(); }\n",
            "AE1014", "Box<i32>", "Box", NULL
        },
        {
            "05C-function",
            "module g25;\n"
            "func pick(value: i32) {}\n"
            "func use() { pick<i32>(1); }\n",
            "AE1014", "pick<i32>", "pick", NULL
        },
        {
            "05C-instance",
            "module g25;\n"
            "type Host { func pick(value: i32) {} }\n"
            "func use(host: Host) { host.pick<i32>(1); }\n",
            "AE1014", "pick<i32>", "pick", NULL
        },
        {
            "05C-static",
            "module g25;\n"
            "type Host { static func pick(value: i32) {} }\n"
            "func use() { Host.pick<i32>(1); }\n",
            "AE1014", "pick<i32>", "pick", NULL
        },
        {
            "05C-fit",
            "module g25;\n"
            "type Host {}\n"
            "fit Host { func pick(value: i32) {} }\n"
            "func use(host: Host) { host.pick<i32>(1); }\n",
            "AE1014", "pick<i32>", "pick", NULL
        },
        {
            "05D-expression",
            "module g25;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "func use() { pick<i32>; }\n",
            "AE1016", "pick<i32>", "pick", NULL
        },
        {
            "05D-binding",
            "module g25;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "func use() { let value = pick<i32>; }\n",
            "AE1016", "pick<i32>", "pick", NULL
        },
        {
            "05D-postfix",
            "module g25;\n"
            "func pick<T>(value: T): T { return value; }\n"
            "func use() { let value = pick<i32>[0]; }\n",
            "AE1016", "pick<i32>", "pick", NULL
        },
        {
            "05E-bare-type",
            "module g25;\n"
            "type Box<T> {}\n"
            "func use(value: Box) {}\n",
            "AE0006", "Box)", "Box", NULL
        },
        {
            "05E-bare-spec",
            "module g25;\n"
            "spec Box<T> {}\n"
            "func use(value: Box) {}\n",
            "AE0006", "Box)", "Box", NULL
        },
        {
            "05F-value-arity",
            "module g25;\n"
            "spec Handler(value: i32): i32;\n"
            "func pick<T, U>(value: T): T { return value; }\n"
            "func use() { let value: Handler = pick<i32>; }\n",
            "AE1015", "pick<i32>", "pick", NULL
        },
        {
            "05F-cast-arity",
            "module g25;\n"
            "spec Handler(value: i32): i32;\n"
            "func pick<T, U>(value: T): T { return value; }\n"
            "func use() { let value = (Handler)pick<i32>; }\n",
            "AE1015", "pick<i32>", "pick", NULL
        }
    };
    size_t failures = 0U;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!g25_check(&cases[i], NULL)) { ++failures; }
    }
    test_g25_dependent_diagnostics();
    g25_module_diagnostics();
    g25_pointer_constraint_diagnostics();
    g25_open_pointer_diagnostics();
    if (failures != 0U) {
        fprintf(stderr, "G25: %zu of %zu cases failed\n", failures, sizeof(cases) / sizeof(cases[0]));
        exit(1);
    }
    printf("G25: %zu source diagnostic cases passed\n", sizeof(cases) / sizeof(cases[0]));
}
