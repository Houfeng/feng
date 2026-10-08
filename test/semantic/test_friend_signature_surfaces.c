#include "../friend_generic_helpers.h"

/* Analyze independent modules and retain exact declaration/use diagnostics. */
static FengSemanticAnalysis *friend_signature_analyze(FengProgram *provider,
    FengProgram *consumer, const char *message) {
    const FengProgram *programs[] = {provider, consumer};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 2U, &options,
        &analysis, &errors, &count);
    bool matched = message == NULL ? ok && count == 0U :
        !ok && count == 1U && strcmp(errors[0].code, "AE1338") == 0 &&
        strstr(errors[0].message, message) != NULL;
    if (!matched) {
        for (size_t i = 0U; i < count; ++i) {
            fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
        }
    }
    FRIEND_CHECK(matched);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Declaration checks and deferred checks retain their original first error. */
static void friend_signature_diagnostic_order(void) {
    FengProgram *provider = friend_generic_parse(
        "open module signature.provider;import signature.reader;"
        "seal type Parameter{}seal spec Constraint{}open type Vault{"
        "@friend(Reader) seal func read<U:Constraint>(value:Parameter):int{return 0;}}");
    FengProgram *consumer = friend_generic_parse("open module signature.reader;open type Reader{}");
    FengSemanticAnalysis *analysis = friend_signature_analyze(provider, consumer,
        "member 'read' exposes type 'Constraint'");
    feng_semantic_analysis_free(analysis);
    feng_program_free(consumer);
    feng_program_free(provider);

    provider = friend_generic_parse(
        "open module signature.provider;seal type Parameter{}seal spec Constraint{}"
        "open type Vault<F>{@friend(F) seal func read<U:Constraint>(value:Parameter):int{return 0;}}");
    consumer = friend_generic_parse("open module signature.reader;import signature.provider;"
        "open type Reader{}func bind(value:Vault<Reader>){}");
    analysis = friend_signature_analyze(provider, consumer,
        "instantiated @friend member 'read' exposes type 'Parameter'");
    feng_semantic_analysis_free(analysis);
    feng_program_free(consumer);
    feng_program_free(provider);
}

/* Inferred field and return facts carry the same deferred visibility condition. */
static void friend_signature_inferred_surfaces(void) {
    const char *members[] = {
        "@friend(F) seal let value=Hidden();",
        "@friend(F) seal func value(){return Hidden();}"
    };
    for (size_t i = 0U; i < sizeof members / sizeof *members; ++i) {
        char source[512];
        int length = snprintf(source, sizeof source,
            "open module signature.provider;seal type Hidden{}open type Vault<F>{%s}", members[i]);
        FRIEND_CHECK(length > 0 && (size_t)length < sizeof source);
        FengProgram *provider = friend_generic_parse(source);
        FengProgram *consumer = friend_generic_parse("open module signature.reader;import signature.provider;"
            "open type Reader{}func bind(value:Vault<Reader>){}");
        FengSemanticAnalysis *analysis = friend_signature_analyze(provider, consumer,
            "instantiated @friend member 'value' exposes type 'Hidden'");
        feng_semantic_analysis_free(analysis);
        feng_program_free(consumer);
        feng_program_free(provider);
    }
}

/* A foreign fit must see each signature surface; method parameters stay local. */
static void friend_signature_query_surfaces(void) {
    FengProgram *provider = friend_generic_parse(
        "open module signature.provider;seal type Hidden{}seal spec Constraint{}open type Reader{}"
        "open type Vault<F>{"
        "@friend(F) seal let explicitField:Hidden;"
        "@friend(F) seal let inferredField=Hidden();"
        "@friend(F) seal func explicitReturn():Hidden{return Hidden();}"
        "@friend(F) seal func inferredReturn(){return Hidden();}"
        "@friend(F) seal func parameter(value:Hidden):int{return 0;}"
        "@friend(F) seal func constrained<U:Constraint>():int{return 0;}"
        "@friend(F) seal func methodParam<U>(value:U):U{return value;}"
        "@friend(F) seal let visible=7;}");
    FengProgram *consumer = friend_generic_parse("open module signature.reader;import signature.provider;"
        "fit Reader{func probe(value:Vault<Reader>){}}");
    FengSemanticAnalysis *analysis = friend_signature_analyze(provider, consumer, NULL);
    const FengDecl *vault = provider->declarations[3];
    const FengDecl *fit = consumer->declarations[0];
    const FengTypeMember *probe = fit->as.fit_decl.members[0];
    const FengTypeRef *instance = probe->as.callable.params[0].type;
    const bool expected[] = {false, false, false, false, false, false, true, true};
    FRIEND_CHECK(vault->as.type_decl.member_count == sizeof expected / sizeof *expected);
    size_t fact_count = analysis->type_fact_count;
    for (size_t i = 0U; i < sizeof expected / sizeof *expected; ++i) {
        FRIEND_CHECK(feng_semantic_member_has_friend_access(analysis, consumer,
            vault, instance, vault->as.type_decl.members[i], fit, probe) == expected[i]);
    }
    FRIEND_CHECK(analysis->type_fact_count == fact_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(consumer);
    feng_program_free(provider);
}

/* Cover every shared signature source without weakening existing diagnostics. */
void test_friend_signature_surfaces(void) {
    friend_signature_diagnostic_order();
    friend_signature_inferred_surfaces();
    friend_signature_query_surfaces();
    puts("friend signature surfaces, diagnostic order and read-only queries passed");
}
