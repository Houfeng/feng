#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Each declaration form transfers the same signature ownership into its AST. */
typedef struct {
    const char *prefix;
    const char *suffix;
    const char *missing_terminator_code;
    bool has_body;
} CallableOwner;

/* Check parser diagnostics while sanitizers verify cleanup of partial trees. */
static void check_callable_failure(const char *source, const char *code) {
    FengProgram *program = NULL;
    FengParseError error = {0};

    CHECK(!feng_parse_source(source, strlen(source), "callable_cleanup.ff",
                             &program, &error));
    CHECK(program == NULL);
    if (error.code == NULL || strcmp(error.code, code) != 0) {
        fprintf(stderr, "%s\nexpected %s; got %s (%s)\n", source, code,
                error.code != NULL ? error.code : "<none>",
                error.message != NULL ? error.message : "<none>");
        exit(1);
    }
    CHECK(error.token.line > 0U && error.token.column > 0U);
}

/* Successful parsing must still retain every signature field until AST disposal. */
static void check_callable_success(const char *source, bool has_body) {
    FengProgram *program = NULL;
    FengParseError error = {0};
    const FengCallableSignature *callable = NULL;

    CHECK(feng_parse_source(source, strlen(source), "callable_cleanup.ff",
                            &program, &error));
    CHECK(program->declaration_count == 2U);
    const FengDecl *decl = program->declarations[1];
    switch (decl->kind) {
        case FENG_DECL_FUNCTION:
            callable = &decl->as.function_decl;
            break;
        case FENG_DECL_TYPE:
            CHECK(decl->as.type_decl.member_count == 1U);
            callable = &decl->as.type_decl.members[0]->as.callable;
            break;
        case FENG_DECL_SPEC:
            CHECK(decl->as.spec_decl.as.object.member_count == 1U);
            callable = &decl->as.spec_decl.as.object.members[0]->as.callable;
            break;
        case FENG_DECL_FIT:
            CHECK(decl->as.fit_decl.member_count == 1U);
            callable = &decl->as.fit_decl.members[0]->as.callable;
            break;
        default:
            CHECK(false);
    }
    CHECK(callable != NULL);
    CHECK(callable->type_param_count == 2U);
    CHECK(callable->type_params[0].constraint != NULL);
    CHECK(callable->param_count == 2U);
    CHECK(callable->return_type != NULL);
    CHECK((callable->body != NULL) == has_body);
    feng_program_free(program);
}

/* Exercise partial signatures, rejected complete signatures and valid ownership
 * transfers repeatedly; LeakSanitizer checks all allocations at process exit. */
void test_parser_callable_cleanup(void) {
    static const CallableOwner owners[] = {
        {"func work", "", "SE0518", true},
        {"extern func work", "", "SE0517", false},
        {"type Owner { func work", "}", "SE0506", true},
        {"type Owner { static func work", "}", "SE0506", true},
        {"spec Contract { func work", "}", "SE0605", false},
        {"spec Contract { static func work", "}", "SE0605", false},
        {"fit Owner { func work", "}", "SE0805", true},
        {"fit Owner { static func work", "}", "SE0805", true}
    };
    static const char *partial[] = {
        "<T: ns.Named, U",
        "<T: ns.Named, U>(value: ns.Box<T>[], other: U",
        "<T: ns.Named, U>(value: ns.Box<T>[], other: U): ns.Box<"
    };
    static const char *partial_codes[] = {"SE0610", "SE0515", "SE0002"};
    static const char *rejected[] = {
        "type Owner { static func ~Owner<T: ns.Named>() {} }",
        "type Owner { static func Owner<T: ns.Named>(value: T) {} }",
        "type Owner { func ~Owner<T: ns.Named>(value: T) {} }",
        "type Owner { func ~Owner<T: ns.Named>(): ns.Box<T>[] {} }",
        "type Owner { func Owner<T: ns.Named>(): ns.Box<T>[] {} }",
        "spec Contract { func work<T: ns.Named>(let value: T): T; }",
        "spec Contract { func work<T: ns.Named>(value: T); }",
        "extern func work<T: ns.Named>(values: T...): void;"
    };
    static const char *rejected_codes[] = {
        "SE0511", "SE0507", "SE0509", "SE0510",
        "SE0505", "SE0606", "SE0604", "SE0503"
    };
    const char *preamble = "module cleanup; "
        "func retained<T: ns.Named>(value: T): T { return value; } ";
    const char *signature =
        "<T: ns.Named, U>(value: ns.Box<T>[], other: U): ns.Box<U>[!]*";
    char source[1024];

    for (size_t round = 0U; round < 3U; ++round) {
        for (size_t i = 0U; i < sizeof owners / sizeof *owners; ++i) {
            const CallableOwner *owner = &owners[i];
            for (size_t j = 0U; j < sizeof partial / sizeof *partial; ++j) {
                snprintf(source, sizeof source, "%s%s%s%s", preamble,
                         owner->prefix, partial[j], owner->suffix);
                check_callable_failure(source, partial_codes[j]);
            }
            snprintf(source, sizeof source, "%s%s%s%s", preamble,
                     owner->prefix, signature, owner->suffix);
            check_callable_failure(source, owner->missing_terminator_code);

            snprintf(source, sizeof source,
                     "%s%s%s { let local: ns.Box<U>[]; let broken = ; }%s",
                     preamble, owner->prefix, signature, owner->suffix);
            check_callable_failure(source, owner->has_body
                                              ? "SE0006"
                                              : owner->missing_terminator_code);

            snprintf(source, sizeof source, "%s%s%s%s%s", preamble,
                     owner->prefix, signature,
                     owner->has_body ? " { let local: ns.Box<U>[]; }" : ";",
                     owner->suffix);
            check_callable_success(source, owner->has_body);
        }
        for (size_t i = 0U; i < sizeof rejected / sizeof *rejected; ++i) {
            snprintf(source, sizeof source, "%s%s", preamble, rejected[i]);
            check_callable_failure(source, rejected_codes[i]);
        }
    }
    puts("parser callable cleanup: 56 cases, 3 passes");
}
