#include "../friend_generic_helpers.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/ft_internal.h"
#include "symbol/internal.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"
#include <sys/stat.h>
#include <unistd.h>

/* Decode little-endian wire fields without relying on host alignment. */
static uint64_t friend_ft_read(const unsigned char *bytes, size_t width) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; ++i) value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}

/* Change one attribute field while retaining the surrounding genuine file. */
static void friend_ft_write(unsigned char *bytes, size_t width, uint64_t value) {
    for (size_t i = 0U; i < width; ++i) bytes[i] = (unsigned char)(value >> (8U * i));
}

/* Find a wire section through the fixed FT directory. */
static size_t friend_ft_section(const unsigned char *bytes, uint16_t kind) {
    size_t directory = (size_t)friend_ft_read(bytes + 0x18, 8U);
    size_t count = (size_t)friend_ft_read(bytes + 0x0C, 2U);
    for (size_t i = 0U; i < count; ++i) {
        size_t entry = directory + i * 32U;
        if (friend_ft_read(bytes + entry, 2U) == kind) return entry;
    }
    FRIEND_CHECK(false);
    return 0U;
}

/* Reject malformed grants/dependencies after a valid checksum, in both profiles. */
static void friend_ft_corruption(const char *path) {
    FILE *file = fopen(path, "rb");
    FRIEND_CHECK(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    long end = ftell(file);
    FRIEND_CHECK(end >= 64L && fseek(file, 0L, SEEK_SET) == 0);
    size_t length = (size_t)end;
    unsigned char *original = malloc(length), *bytes = malloc(length);
    FRIEND_CHECK(original != NULL && bytes != NULL);
    FRIEND_CHECK(fread(original, 1U, length, file) == length && fclose(file) == 0);
    FRIEND_CHECK(original[5] == 2U && original[6] == 0U);
    size_t attrs = friend_ft_section(original, FENG_SYMBOL_FT_SEC_ATTRS);
    size_t attr_count = (size_t)friend_ft_read(original + attrs + 4U, 4U);
    size_t attr_base = (size_t)friend_ft_read(original + attrs + 8U, 8U);
    size_t records[4] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    for (size_t i = 0U; i < attr_count; ++i) {
        size_t record = attr_base + i * 20U;
        uint64_t kind = friend_ft_read(original + record + 4U, 2U);
        if (kind >= 17U && kind <= 19U && records[kind - 17U] == SIZE_MAX) records[kind - 17U] = record;
        if (kind == FENG_SYMBOL_ATTR_REIFIABLE_MANAGED_DEP && records[3] == SIZE_MAX) records[3] = record;
    }
    size_t types = friend_ft_section(original, FENG_SYMBOL_FT_SEC_TYPS);
    size_t type_base = (size_t)friend_ft_read(original + types + 8U, 8U);
    size_t type_count = (size_t)friend_ft_read(original + types + 4U, 4U);
    uint64_t builtin = 0U, wrong_parameter = 0U;
    uint64_t friend_type = friend_ft_read(original + records[0] + 8U, 4U);
    for (size_t i = 0U; i < type_count; ++i) {
        uint64_t kind = friend_ft_read(original + type_base + i * 24U, 2U);
        if (kind == FENG_SYMBOL_FT_TYPE_KIND_BUILTIN) builtin = i + 1U;
        if (kind == FENG_SYMBOL_FT_TYPE_KIND_TYPE_PARAM_REF && i + 1U != friend_type) wrong_parameter = i + 1U;
    }
    FRIEND_CHECK(builtin != 0U && wrong_parameter != 0U);
    FRIEND_CHECK(records[1] == SIZE_MAX); /* New artifacts never emit retired checks. */
    records[1] = records[0];
    for (size_t kind = 0U; kind < 4U; ++kind) {
        FRIEND_CHECK(records[kind] != SIZE_MAX);
        for (size_t variant = 0U; variant < (kind == 1U ? 1U : 9U); ++variant) {
            size_t record = records[kind];
            memcpy(bytes, original, length);
            if (kind == 1U) {
                friend_ft_write(bytes + record + 4U, 2U, FENG_SYMBOL_ATTR_RETIRED_TYPE_CHECK);
            } else switch (variant) {
                case 0U: friend_ft_write(bytes + record, 4U, 0U); break;
                case 1U: friend_ft_write(bytes + record, 4U, UINT32_MAX); break;
                case 2U: friend_ft_write(bytes + record + 6U, 2U, 1U); break;
                case 3U: friend_ft_write(bytes + record + 8U, 4U, 0U); break;
                case 4U: friend_ft_write(bytes + record + 8U, 4U, UINT32_MAX); break;
                case 5U: friend_ft_write(bytes + record + 12U, 4U, UINT32_MAX); break;
                case 6U: friend_ft_write(bytes + record + 16U, 4U, UINT32_MAX); break;
                case 7U: friend_ft_write(bytes + record + 8U, 4U, kind == 0U ? builtin : wrong_parameter); break;
                case 8U: friend_ft_write(bytes + record, 4U, 1U); break; /* module cannot own a grant/check */
            }
            uint64_t hash = UINT64_C(14695981039346656037);
            for (size_t i = (size_t)friend_ft_read(bytes + 0x20, 8U); i < length; ++i) {
                hash ^= bytes[i]; hash *= UINT64_C(1099511628211);
            }
            friend_ft_write(bytes + 0x28, 8U, hash);
            FengSymbolGraph *graph = NULL;
            FengSymbolError error = {0};
            bool ok = feng_symbol_ft_read_bytes(bytes, length, "invalid-friend.ft", NULL, &graph, &error);
            if (ok) fprintf(stderr, "accepted friend corruption kind=%zu variant=%zu\n", kind, variant);
            FRIEND_CHECK(!ok && graph == NULL && error.message != NULL);
            if (kind == 1U) FRIEND_CHECK(error.message != NULL &&
                strstr(error.message, "rebuild provider") != NULL);
            feng_symbol_error_free(&error);
        }
    }
    free(original); free(bytes);
}

/* Source is destroyed before each consumer, exercising only persisted facts. */
void test_friend_generic_ft(void) {
    const char *source = "open module friends.core;"
        "open type Vault<F>{@friend(F) seal let x:int=7;@friend(F) seal func get():int{return self.x;}"
        "@friend(F) seal static let shared:int=9;@friend(F) seal static func count():int{return 11;}}"
        "open type Box<T>{open let value:Vault<T>;}"
        "open type Reader<T>{}"
        "seal type Hidden{}open type Sensitive<F>{@friend(F) seal let data:Hidden;}"
        "open spec Secret<F>{@friend(F) seal func hidden():int;}"
        "open spec Nested<T>{let value:Vault<T>;}"
        "open func forward<T>(){let v=Vault<T>();}"
        "open func relay<U>(){forward<U>();}"
        "open fit Z[]{@friend(Z) seal func arrayHidden(){}}"
        "open func arrayForward<U>(){let xs=U[:1];}"
        "open fit Reader<T>{func read(v:Vault<Reader<T>>):int{return v.x;}"
        "func check<U>(){forward<U>();}}";
    FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
    FengSymbolError error = {0};
    char directory[] = "temp/friend-ft-XXXXXX";
    (void)mkdir("temp", 0777);
    FRIEND_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    bool exported = feng_symbol_export_analysis(unit.analysis, &options, &error);
    if (!exported) fprintf(stderr, "friend export: %s\n", error.message);
    FRIEND_CHECK(exported);
    friend_generic_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        char path[512];
        snprintf(path, sizeof path, "%s/friends/core.ft", roots[profile]);
        friend_ft_corruption(path);
        FengSymbolProvider *provider = NULL;
        FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
        bool loaded = feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error);
        if (!loaded) fprintf(stderr, "friend import: %s\n", error.message);
        FRIEND_CHECK(loaded);
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        FRIEND_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        const struct { const char *source; const char *code; } cases[] = {
            {"type Local{func read(v:Vault<Local>):int{return v.x+v.get()+Vault<Local>.shared+Vault<Local>.count();}}", NULL},
            {"type Local{}fit Local{func read(v:Vault<Local>):int{return v.get();}}", NULL},
            {"fit Reader<T>{func extra(v:Vault<Reader<T>>):int{return v.x;}}", NULL},
            {"type Local{}func run(){forward<Local>();relay<Local>();}", NULL},
            {"type Local{}func run(){arrayForward<Local>();}", NULL},
            {"func run(){arrayForward<int>();}", "AE1336"},
            {"func run(x:Reader<int>,v:Vault<Reader<int>>):int{return x.read(v);}", NULL},
            {"type Local{}func run(x:Reader<int>){x.check<Local>();}", NULL},
            {"func run(x:Sensitive<Reader<int>>){}", NULL},
            {"type Local{}func run(x:Sensitive<Local>){}", "AE1338"},
            {"fit Reader<T>{func peek(v:Sensitive<Reader<T>>){let h=v.data;}}", "AE1338"},
            {"func run(x:Vault<int>){}", "AE1336"},
            {"func run(){forward<int>();}", "AE1336"},
            {"func run(){relay<int>();}", "AE1336"},
            {"spec Action():void;func run(){let f:Action=forward<int>;}", "AE1336"},
            {"func run(x:Box<int>){}", "AE1336"},
            {"func run(x:Nested<int>){}", "AE1336"},
            {"func run(x:Reader<int>){x.check<int>();}", "AE1336"},
            {"type Local{}func run(v:Vault<Local>):int{return v.x;}", "AE0308"},
            {"type Local{}type Other{func read(v:Vault<Local>):int{return v.get();}}", "AE0308"}
        };
        for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
            char consumer[2048];
            snprintf(consumer, sizeof consumer, "module consumer;import friends.core;%s", cases[i].source);
            unit = friend_generic_analyze(consumer, &query, cases[i].code, NULL);
            friend_generic_dispose(&unit);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("friend generic FT profiles and source-invisible consumers passed");
}

/* Compare declaration obligations in source and both source-free FT profiles. */
void test_friend_review_conditions_ft(void) {
    const char *declarations =
        "open type Reader{}"
        "seal type Hidden<F>{@friend(F) seal func hidden(){}"
        "@friend(F) seal static func other(){}}"
        "open type Carrier<T>{seal let hidden:Hidden<T>;}"
        "open type Vault<F>{@friend(F) seal let value:int=0;}"
        "open type Target<T>{}"
        "open fit Target<T>{@friend(Vault<T>) seal func hidden(){}}"
        "open spec Surface<F>{@friend(F) seal func hidden():void;}"
        "open type Adapt<T>{}"
        "open fit Adapt<T>:Surface<T>{seal func hidden():void{}}"
        "open fit T[]{@friend(Vault<T>) seal func arrayHidden(){}}"
        "open func forward<T>(v:Target<T>){}"
        "open func adapt<T>(v:Adapt<T>){}";
    const struct { const char *source; const char *code; } cases[] = {
        {"func use(v:Carrier<Reader>){}", NULL},
        {"func use(v:Carrier<int>){}", "AE1336"},
        {"func use(v:Carrier<Reader>){let x=v.hidden;}", "AE0308"},
        {"func use(v:Target<Reader>){}", NULL},
        {"func use(v:Target<int>){}", "AE1336"},
        {"func use(v:Adapt<Reader>){}", NULL},
        {"func use(v:Adapt<int>){}", "AE1336"},
        {"func use(v:Reader[]){}", NULL},
        {"func use(v:int[]){}", "AE1336"},
        {"func relay<T>(v:Target<T>){forward<T>(v);}", NULL},
        {"func relay<T>(v:Adapt<T>){adapt<T>(v);}", NULL}
    };
    char source[4096];
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        snprintf(source, sizeof source, "open module review.conditions;%s%s", declarations, cases[i].source);
        FriendGenericUnit unit = friend_generic_analyze(source, NULL, cases[i].code, NULL);
        friend_generic_dispose(&unit);
    }
    snprintf(source, sizeof source, "open module review.conditions;%s", declarations);
    FriendGenericUnit unit = friend_generic_analyze(source, NULL, NULL, NULL);
    size_t fit_checks = 0U;
    for (size_t i = 0U; i < unit.analysis->reifiable_dep_set_count; ++i) {
        const FengReifiableDepSet *set = &unit.analysis->reifiable_dep_sets[i];
        if (set->owner_decl->kind == FENG_DECL_FIT && set->owner_member == NULL) {
            FRIEND_CHECK(set->dep_count > 0U);
            ++fit_checks;
        }
    }
    FRIEND_CHECK(fit_checks == 3U);
    char directory[] = "temp/friend-review-conditions-XXXXXX";
    (void)mkdir("temp", 0777);
    FRIEND_CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengSymbolError error = {0};
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FRIEND_CHECK(feng_symbol_export_analysis(unit.analysis, &options, &error));
    friend_generic_dispose(&unit);
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
        FRIEND_CHECK(feng_symbol_provider_add_ft_root(provider, roots[profile],
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        FRIEND_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
            snprintf(source, sizeof source, "module consumer;import review.conditions;%s", cases[i].source);
            unit = friend_generic_analyze(source, &query, cases[i].code, NULL);
            friend_generic_dispose(&unit);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    puts("friend private skeleton and fit declaration conditions passed in source and both FT profiles");
}

/* Exercise restoration after external member identities become available. */
static void friend_owner_import(const FengSymbolGraph *graph,
                                const FengSymbolGraph *owners, bool expected) {
    FengSymbolError error = {0};
    FengSymbolProvider *provider = NULL;
    FRIEND_CHECK(feng_symbol_provider_create(&provider, &error));
    if (owners != NULL) FRIEND_CHECK(feng_symbol_provider_add_graph(provider, owners, &error));
    FRIEND_CHECK(feng_symbol_provider_add_graph(provider, graph, &error));
    FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
    FRIEND_CHECK(cache != NULL);
    FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
    FengProgram *program = friend_generic_parse(owners != NULL
        ? "module consumer;import review.owners;import review.users;func use(v:Reader){}"
        : "module consumer;import review.owners;func use(v:Reader){}");
    const FengProgram *programs[] = {program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = &query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (ok != expected) {
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
    }
    FRIEND_CHECK(ok == expected);
    if (expected) FRIEND_CHECK(count == 0U);
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
}

/* A valid checksum reaches owner validation in the reader or external importer. */
static void friend_owner_reject_bytes(unsigned char *bytes, size_t length,
                                      const char *path, const FengSymbolGraph *owners) {
    size_t header = (size_t)friend_ft_read(bytes + 0x20, 8U);
    friend_ft_write(bytes + 0x28, 8U, feng_symbol_internal_fnv1a64(bytes + header, length - header));
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    bool read = feng_symbol_ft_read_bytes(bytes, length, path, NULL, &graph, &error);
    FRIEND_CHECK(read == (owners != NULL));
    if (owners != NULL) friend_owner_import(graph, owners, false);
    else FRIEND_CHECK(error.message != NULL && strstr(error.message, "friend signature owner instance") != NULL);
    feng_symbol_graph_free(graph);
    feng_symbol_error_free(&error);
}

/* Change only genuine owner bindings while preserving all unrelated metadata. */
static void friend_owner_corruption(const char *path, const FengSymbolGraph *owners) {
    FILE *file = fopen(path, "rb");
    FRIEND_CHECK(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    long end = ftell(file);
    FRIEND_CHECK(end >= 64L && fseek(file, 0L, SEEK_SET) == 0);
    size_t length = (size_t)end;
    unsigned char *original = malloc(length), *bytes = malloc(length);
    FRIEND_CHECK(original != NULL && bytes != NULL);
    FRIEND_CHECK(fread(original, 1U, length, file) == length && fclose(file) == 0);
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    FRIEND_CHECK(feng_symbol_ft_read_bytes(original, length, path, NULL, &graph, &error));
    friend_owner_import(graph, owners, true);
    feng_symbol_graph_free(graph);
    size_t attrs = friend_ft_section(original, FENG_SYMBOL_FT_SEC_ATTRS);
    size_t attr_base = (size_t)friend_ft_read(original + attrs + 8U, 8U);
    size_t attr_count = (size_t)friend_ft_read(original + attrs + 4U, 4U);
    size_t types = friend_ft_section(original, FENG_SYMBOL_FT_SEC_TYPS);
    size_t type_base = (size_t)friend_ft_read(original + types + 8U, 8U);
    size_t type_count = (size_t)friend_ft_read(original + types + 4U, 4U);
    uint64_t builtin = 0U;
    for (size_t i = 0U; i < type_count; ++i) {
        if (friend_ft_read(original + type_base + i * 24U, 2U) == FENG_SYMBOL_FT_TYPE_KIND_BUILTIN) {
            builtin = i + 1U;
            break;
        }
    }
    FRIEND_CHECK(builtin != 0U);
    size_t use_count = 0U, first_record = SIZE_MAX;
    uint64_t other_nominal = 0U;
    for (size_t i = 0U; i < attr_count; ++i) {
        size_t record = attr_base + i * 20U;
        if (friend_ft_read(original + record + 4U, 2U) != FENG_SYMBOL_ATTR_FRIEND_FIT_ACCESS) continue;
        ++use_count;
        if (first_record == SIZE_MAX) first_record = record;
        else if (other_nominal == 0U) {
            uint64_t type_id = friend_ft_read(original + record + 8U, 4U);
            if (friend_ft_read(original + type_base + (type_id - 1U) * 24U, 2U) == FENG_SYMBOL_FT_TYPE_KIND_NAMED_GENERIC) {
                other_nominal = type_id;
            }
        }
        memcpy(bytes, original, length);
        friend_ft_write(bytes + record + 8U, 4U, builtin);
        friend_owner_reject_bytes(bytes, length, path, owners);
    }
    FRIEND_CHECK(use_count == 5U && other_nominal != 0U);
    uint64_t instance_id = friend_ft_read(original + first_record + 8U, 4U);
    size_t instance_record = type_base + (instance_id - 1U) * 24U;
    FRIEND_CHECK(friend_ft_read(original + instance_record, 2U) == FENG_SYMBOL_FT_TYPE_KIND_NAMED_GENERIC);
    for (size_t variant = 0U; variant < 2U; ++variant) {
        memcpy(bytes, original, length);
        if (variant == 0U) friend_ft_write(bytes + first_record + 8U, 4U, other_nominal);
        else friend_ft_write(bytes + instance_record + 0x10, 4U, 0U);
        friend_owner_reject_bytes(bytes, length, path, owners);
    }
    feng_symbol_error_free(&error);
    free(original);
    free(bytes);
}

/* Cover type/spec/fit owners and array rank binding locally and across modules. */
void test_friend_signature_owner_ft(void) {
    const char *declarations = "open module review.owners;"
        "open type Reader{}"
        "open type Vault<F>{@friend(F) seal let payload:F;}"
        "open spec Parent<F>{@friend(F) seal func hidden():int;}"
        "open spec Child<F>:Parent<F>{}"
        "open type Target<F>{}"
        "open fit Target<F>{@friend(F) seal func extended():int{return 1;}}"
        "open fit T[]{@friend(Reader) seal func arrayHidden():int{return 1;}}";
    const char *accesses = "open fit Reader{"
        "func read(v:Vault<Reader>):Reader{return v.payload;}"
        "func projected(v:Child<Reader>):int{return v.hidden();}"
        "func extension(v:Target<Reader>):int{return v.extended();}"
        "func arrays(v:int[]):int{return v.arrayHidden();}"
        "func nested(v:int[][]):int{return v.arrayHidden();}}";
    for (size_t external = 0U; external < 2U; ++external) {
        char source[4096], user_source[4096];
        snprintf(source, sizeof source, "%s%s", declarations, external == 0U ? accesses : "");
        FengProgram *programs[2] = {friend_generic_parse(source), NULL};
        if (external != 0U) {
            snprintf(user_source, sizeof user_source, "open module review.users;import review.owners;%s", accesses);
            programs[1] = friend_generic_parse(user_source);
        }
        FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
            .pointer_size = feng_get_host_pointer_size()};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticError *errors = NULL;
        size_t count = 0U;
        bool ok = feng_semantic_analyze_with_options((const FengProgram *const *)programs,
            external + 1U, &options, &analysis, &errors, &count);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
        FRIEND_CHECK(ok && count == 0U);
        char directory[] = "temp/friend-owner-ft-XXXXXX";
        (void)mkdir("temp", 0777);
        FRIEND_CHECK(mkdtemp(directory) != NULL);
        char roots[2][256];
        snprintf(roots[0], sizeof roots[0], "%s/public", directory);
        snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
        FengSymbolExportOptions output = {.public_root = roots[0], .workspace_root = roots[1]};
        FengSymbolError error = {0};
        FRIEND_CHECK(feng_symbol_export_analysis(analysis, &output, &error));
        feng_semantic_errors_free(errors, count);
        feng_semantic_analysis_free(analysis);
        for (size_t i = 0U; i <= external; ++i) feng_program_free(programs[i]);
        for (size_t profile = 0U; profile < 2U; ++profile) {
            FengSymbolGraph *owners = NULL;
            char path[512];
            if (external != 0U) {
                snprintf(path, sizeof path, "%s/review/owners.ft", roots[profile]);
                FRIEND_CHECK(feng_symbol_ft_read_file(path, NULL, &owners, &error));
            }
            snprintf(path, sizeof path, "%s/review/%s.ft", roots[profile], external != 0U ? "users" : "owners");
            friend_owner_corruption(path, owners);
            feng_symbol_graph_free(owners);
        }
        feng_symbol_error_free(&error);
    }
    puts("friend signature owner identity, arity, projections and array bindings passed in both FT profiles");
}
