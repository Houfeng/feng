#include "../friend_generic_helpers.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"
#include <sys/stat.h>
#include <unistd.h>

/* One independently analyzed use, with an exact diagnostic site when rejected. */
typedef struct FriendSignatureCase {
    const char *source;
    const char *diagnostic;
    bool deferred;
} FriendSignatureCase;

/* The persisted grant location; public FT intentionally omits original source spans. */
typedef struct FriendSignatureOrigin {
    FengSlice path;
    FengToken token;
} FriendSignatureOrigin;

/* Distinguish substituted signatures from fixed private/public pointer controls. */
typedef enum FriendSignatureVisibility {
    FRIEND_SIGNATURE_SUBSTITUTED,
    FRIEND_SIGNATURE_PRIVATE,
    FRIEND_SIGNATURE_PUBLIC
} FriendSignatureVisibility;

/* Keep parameter and return failures separate, including recursive signature types. */
typedef struct FriendSignatureShape {
    const char *member;
    FriendSignatureVisibility visibility;
    bool pointer_argument; /* Pass a closed pointer as R; forming open R* is forbidden. */
} FriendSignatureShape;

static const FriendSignatureShape friend_signature_shapes[] = {
    {"@friend(%s) seal func inspect(value:R){}", FRIEND_SIGNATURE_SUBSTITUTED, false},
    {"seal let stored:R;@friend(%s) seal func inspect():R{return self.stored;}", FRIEND_SIGNATURE_SUBSTITUTED, false},
    {"@friend(%s) seal func inspect(value:Box<R>){}", FRIEND_SIGNATURE_SUBSTITUTED, false},
    {"seal let stored:Box<R>;@friend(%s) seal func inspect():Box<R>{return self.stored;}", FRIEND_SIGNATURE_SUBSTITUTED, false},
    {"@friend(%s) seal func inspect(value:R[]){}", FRIEND_SIGNATURE_SUBSTITUTED, false},
    {"seal let stored:R[];@friend(%s) seal func inspect():R[]{return self.stored;}", FRIEND_SIGNATURE_SUBSTITUTED, false},
    {"@friend(%s) seal func inspect(value:R){}", FRIEND_SIGNATURE_SUBSTITUTED, true},
    {"seal let stored:R;@friend(%s) seal func inspect():R{return self.stored;}", FRIEND_SIGNATURE_SUBSTITUTED, true},
    {"@friend(%s) seal func inspect(value:Hidden*){}", FRIEND_SIGNATURE_PRIVATE, false},
    {"seal let stored:Hidden*;@friend(%s) seal func inspect():Hidden*{return self.stored;}", FRIEND_SIGNATURE_PRIVATE, false},
    {"@friend(%s) seal func inspect(value:Shared*){}", FRIEND_SIGNATURE_PUBLIC, false},
    {"seal let stored:Shared*;@friend(%s) seal func inspect():Shared*{return self.stored;}", FRIEND_SIGNATURE_PUBLIC, false}
};

/* Locate a unique source spelling without coupling assertions to generated AST nodes. */
static void friend_signature_position(const char *source, const char *needle,
    unsigned int *line, unsigned int *column) {
    const char *found = strstr(source, needle);
    FRIEND_CHECK(found != NULL);
    *line = 1U;
    *column = 1U;
    for (const char *cursor = source; cursor < found; ++cursor) {
        if (*cursor == '\n') { ++*line; *column = 1U; }
        else ++*column;
    }
}

/* Assert the same signature obligation in source and source-free FT consumers. */
static void friend_signature_expect(const char *provider_source, bool source_mode,
    const FengSemanticImportedModuleQuery *query, const FriendSignatureOrigin *origin,
    const FriendSignatureCase *test) {
    FengProgram *provider = source_mode ? friend_generic_parse(provider_source) : NULL;
    FengProgram *consumer = NULL;
    FengParseError parse_error = {0};
    FRIEND_CHECK(feng_parse_source(test->source, strlen(test->source),
        "signature_consumer.ff", &consumer, &parse_error));
    const FengProgram *programs[] = {consumer, provider};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, source_mode ? 2U : 1U,
        &options, &analysis, &errors, &count);
    bool matched = test->diagnostic == NULL ? ok && count == 0U :
        !ok && count == 1U && strcmp(errors[0].code, "AE1338") == 0 &&
        strstr(errors[0].message, "member 'inspect' exposes type 'Hidden'") != NULL;
    if (!matched) {
        fprintf(stderr, "signature case (%s), expected %s:\n%s\n%s\n",
            source_mode ? "source" : "FT", test->diagnostic != NULL ? "AE1338" : "success",
            provider_source, test->source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %s\n",
            errors[i].code, errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    FRIEND_CHECK(matched);
    if (test->diagnostic != NULL) {
        const FengSemanticError *error = &errors[0];
        unsigned int line, column;
        friend_signature_position(test->source, test->diagnostic, &line, &column);
        size_t length = strcspn(test->diagnostic, "<");
        FRIEND_CHECK(strcmp(error->path, "signature_consumer.ff") == 0);
        FRIEND_CHECK(error->token.line == line && error->token.column == column);
        FRIEND_CHECK(error->token.length == length &&
            memcmp(error->token.lexeme, test->diagnostic, length) == 0);
        FRIEND_CHECK((strstr(error->message, "instantiated @friend") != NULL) == test->deferred);
        FRIEND_CHECK(error->related_location_count == (test->deferred ? 1U : 0U));
        if (test->deferred) {
            bool local = strstr(test->source, "@friend") != NULL;
            const FengSemanticRelatedLocation *related = &error->related_locations[0];
            if (local || source_mode) {
                friend_signature_position(local ? test->source : provider_source,
                    "@friend", &line, &column);
                FRIEND_CHECK(strcmp(related->path,
                    local ? "signature_consumer.ff" : "friend_generic.ff") == 0);
            } else {
                FRIEND_CHECK(origin != NULL && origin->path.length > 0U);
                FRIEND_CHECK(strlen(related->path) == origin->path.length &&
                    memcmp(related->path, origin->path.data, origin->path.length) == 0);
                line = origin->token.line;
                column = origin->token.column;
            }
            FRIEND_CHECK(related->token.line == line && related->token.column == column);
            FRIEND_CHECK(strstr(related->message, "@friend authorization for member 'inspect'") != NULL);
        }
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(consumer);
    feng_program_free(provider);
}

/* Export a valid open provider once, then destroy its AST before either import. */
static void friend_signature_modes(const char *provider_source,
    const FriendSignatureCase *cases, size_t count) {
    for (size_t i = 0U; i < count; ++i)
        friend_signature_expect(provider_source, true, NULL, NULL, &cases[i]);
    FriendGenericUnit unit = friend_generic_analyze(provider_source, NULL, NULL, NULL);
    char directory[] = "temp/friend-signature-XXXXXX";
    (void)mkdir("temp", 0777);
    FRIEND_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolExportOptions output = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    FRIEND_CHECK(feng_symbol_export_analysis(unit.analysis, &output, &error));
    friend_generic_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
        FRIEND_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
            &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        FRIEND_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        FriendSignatureOrigin origin = {0};
        const FengSlice segments[] = {{"signature", 9U}, {"owner", 5U}};
        const FengSymbolImportedModule *module = feng_symbol_provider_find_module(provider, segments, 2U);
        if (module != NULL) {
            const FengSymbolDeclView *vault = feng_symbol_module_find_public_type(module, (FengSlice){"Vault", 5U});
            FRIEND_CHECK(vault != NULL);
            for (size_t i = 0U; i < feng_symbol_decl_member_count(vault); ++i) {
                const FengSymbolDeclView *member = feng_symbol_decl_member_at(vault, i);
                FengSlice name = feng_symbol_decl_name(member);
                if (name.length == 7U && memcmp(name.data, "inspect", 7U) == 0) {
                    FRIEND_CHECK(feng_symbol_decl_friend_type_count(member) == 1U);
                    origin.path = feng_symbol_module_source_path(module);
                    origin.token = feng_symbol_decl_token(member);
                }
            }
            FRIEND_CHECK(origin.path.length > 0U);
        }
        for (size_t i = 0U; i < count; ++i)
            friend_signature_expect(provider_source, false, &query, &origin, &cases[i]);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
}

/* Close the signature's R inside its owner, then forward F through two generic calls. */
static void friend_signature_deferred_cases(void) {
    for (size_t shape = 0U; shape < sizeof friend_signature_shapes / sizeof *friend_signature_shapes; ++shape) {
        const FriendSignatureShape *item = &friend_signature_shapes[shape];
        char member[512], provider[2048];
        snprintf(member, sizeof member, item->member, "F");
        int length = snprintf(provider, sizeof provider,
            "open module signature.owner;\n"
            "seal type Hidden{}open type Shared{}open type LocalReader{}open type LocalGeneric<T>{}open type Box<T>{}\n"
            "open type Vault<F,R>{%s}\n"
            "open func bind<F,R>(){let value=Vault<F,R>();}\n"
            "open func relay<F,R>(){bind<F,R>();}\n"
            "open func hidden<F>(){relay<F,%s>();}\n"
            "open func shared<F>(){relay<F,%s>();}\n", member,
            item->pointer_argument ? "Hidden*" : "Hidden",
            item->pointer_argument ? "Shared*" : "Shared");
        FRIEND_CHECK(length > 0 && (size_t)length < sizeof provider);
        static const char *uses[] = {
            "func use(){hidden<LocalReader>();}",
            "func use(){hidden<LocalGeneric<Shared>>();}",
            "func use(){shared<RemoteReader>();}",
            "func use(){hidden<RemoteReader>();}",
            "func use(){hidden<RemoteGeneric<Shared>>();}",
            "func forward<F>(){hidden<F>();}"
        };
        char consumers[6][512];
        FriendSignatureCase cases[6];
        for (size_t i = 0U; i < sizeof uses / sizeof *uses; ++i) {
            length = snprintf(consumers[i], sizeof consumers[i],
                "module signature.consumer;import signature.owner;\n"
                "type RemoteReader{}type RemoteGeneric<T>{}\n%s\n", uses[i]);
            FRIEND_CHECK(length > 0 && (size_t)length < sizeof consumers[i]);
            const char *diagnostic = NULL;
            if (i == 2U && item->visibility == FRIEND_SIGNATURE_PRIVATE) diagnostic = "shared<RemoteReader>";
            if (i == 3U && item->visibility != FRIEND_SIGNATURE_PUBLIC) diagnostic = "hidden<RemoteReader>";
            if (i == 4U && item->visibility != FRIEND_SIGNATURE_PUBLIC) diagnostic = "hidden<RemoteGeneric<Shared>>";
            cases[i] = (FriendSignatureCase){consumers[i], diagnostic, diagnostic != NULL};
        }
        friend_signature_modes(provider, cases, sizeof cases / sizeof *cases);
    }
}

/* A known friend's module is fixed even when its type arguments are still open. */
static void friend_signature_known_cases(void) {
    const char *provider = "open module signature.readers;open type DirectReader{}open type GenericReader<T>{}";
    static const char *grants[] = {"LocalReader", "DirectReader", "LocalGeneric<R>", "GenericReader<R>"};
    for (size_t shape = 0U; shape < sizeof friend_signature_shapes / sizeof *friend_signature_shapes; ++shape) {
        const FriendSignatureShape *item = &friend_signature_shapes[shape];
        char consumers[8][1024];
        FriendSignatureCase cases[8];
        for (size_t grant = 0U; grant < sizeof grants / sizeof *grants; ++grant) {
            char member[512];
            snprintf(member, sizeof member, item->member, grants[grant]);
            for (size_t visible = 0U; visible < 2U; ++visible) {
                size_t i = grant * 2U + visible;
                int length = snprintf(consumers[i], sizeof consumers[i],
                    "open module signature.owner;import signature.readers;\n"
                    "seal type Hidden{}open type Shared{}open type LocalReader{}open type LocalGeneric<T>{}open type Box<T>{}\n"
                    "open type Vault<R>{%s}\n"
                    "func use(value:Vault<%s%s>){}\n", member, visible ? "Shared" : "Hidden",
                    item->pointer_argument ? "*" : "");
                FRIEND_CHECK(length > 0 && (size_t)length < sizeof consumers[i]);
                bool rejected = (grant == 1U || grant == 3U) &&
                    (item->visibility == FRIEND_SIGNATURE_PRIVATE ||
                     (item->visibility == FRIEND_SIGNATURE_SUBSTITUTED && visible == 0U));
                bool deferred = rejected && item->visibility == FRIEND_SIGNATURE_SUBSTITUTED;
                const char *diagnostic = !rejected ? NULL : !deferred ? grants[grant] :
                    item->pointer_argument ? "Vault<Hidden*>" : "Vault<Hidden>";
                cases[i] = (FriendSignatureCase){consumers[i], diagnostic, deferred};
            }
        }
        friend_signature_modes(provider, cases, sizeof cases / sizeof *cases);
    }
}

/* Cover parameter/return substitution and pointer visibility in source and both FT profiles. */
void test_friend_signature_visibility(void) {
    friend_signature_deferred_cases();
    friend_signature_known_cases();
    puts("friend signature substitution and pointer visibility: 504 source/FT cases passed");
}
