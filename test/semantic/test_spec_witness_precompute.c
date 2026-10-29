#include "parser/parser.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Keep the provider's resolved declarations alive throughout consumer analysis. */
typedef struct WitnessImportFixture {
    FengProgram *program;
    FengSemanticAnalysis *analysis;
    FengSemanticModule module;
} WitnessImportFixture;

/* Parse an independent source file without borrowing a different file's path. */
static FengProgram *witness_parse(const char *path, const char *source) {
    FengProgram *program = NULL;
    FengParseError error = {0};

    CHECK(feng_parse_source(source, strlen(source), path, &program, &error));
    return program;
}

/* Expose a pre-resolved package module through the production import boundary. */
static const FengSemanticModule *witness_get_module(
    const void *user, const FengSlice *segments, size_t segment_count) {
    const WitnessImportFixture *fixture = user;

    if (segment_count != fixture->module.segment_count) {
        return NULL;
    }
    for (size_t index = 0U; index < segment_count; ++index) {
        FengSlice expected = fixture->module.segments[index];

        if (segments[index].length != expected.length ||
            memcmp(segments[index].data, expected.data, expected.length) != 0) {
            return NULL;
        }
    }
    return &fixture->module;
}

/* Build the provider locally, then mark its resolved module as package-owned. */
static void witness_import_init(WitnessImportFixture *fixture, const char *source) {
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = 8U,
    };

    memset(fixture, 0, sizeof(*fixture));
    fixture->program = witness_parse("witness_external.ff", source);
    const FengProgram *programs[] = {fixture->program};
    CHECK(feng_semantic_analyze_with_options(programs, 1U, &options,
        &fixture->analysis, &errors, &count));
    CHECK(count == 0U && fixture->analysis->module_count == 1U);
    fixture->module = fixture->analysis->modules[0];
    fixture->module.origin = FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE;
    feng_semantic_errors_free(errors, count);
}

/* Release borrowed import storage only after consumer diagnostics are checked. */
static void witness_import_free(WitnessImportFixture *fixture) {
    feng_semantic_analysis_free(fixture->analysis);
    feng_program_free(fixture->program);
}

/* Assert failure propagation and the complete primary diagnostic location. */
static void witness_analyze(const FengProgram *const *programs, size_t program_count,
    const FengSemanticImportedModuleQuery *query, size_t pointer_size,
    const char *code, size_t expected_count, const char *path,
    unsigned line, unsigned column, const char *token) {
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .imported_modules = query,
        .pointer_size = pointer_size,
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, program_count, &options,
        &analysis, &errors, &count);

    if (ok != (code == NULL) || count != expected_count) {
        fprintf(stderr, "%s: expected %s (%zu), got %zu diagnostics\n",
            programs[0]->path, code != NULL ? code : "success", expected_count, count);
        for (size_t index = 0U; index < count; ++index) {
            fprintf(stderr, "%s %s:%u:%u %s\n", errors[index].code,
                errors[index].path != NULL ? errors[index].path : "<unknown>",
                errors[index].token.line, errors[index].token.column, errors[index].message);
        }
        CHECK(false);
    }
    CHECK((analysis != NULL) == ok);
    for (size_t index = 0U; index < count; ++index) {
        if (errors[index].token.line != line || errors[index].token.column != column) {
            fprintf(stderr, "%s: expected %u:%u, got %u:%u (%s)\n", path,
                line, column, errors[index].token.line, errors[index].token.column,
                errors[index].message);
        }
        CHECK(strcmp(errors[index].code, code) == 0);
        CHECK(errors[index].path != NULL && strcmp(errors[index].path, path) == 0);
        CHECK(errors[index].token.line == line);
        CHECK(errors[index].token.column == column);
        CHECK(errors[index].token.length == strlen(token));
        CHECK(memcmp(errors[index].token.lexeme, token, strlen(token)) == 0);
        if (strcmp(code, "AE0804") == 0) {
            CHECK(strstr(errors[index].message, "multiple visible implementations") != NULL);
            CHECK(strstr(errors[index].message, "'label'") != NULL);
        }
    }
    feng_semantic_errors_free(errors, count);
    feng_semantic_analysis_free(analysis);
}

/* Exercise aliases, exact targets and arrays with both visibility spellings and
 * file orders. The first file has no fit, so its path cannot mask a bad origin. */
static size_t witness_precompute_matrix(const FengSemanticImportedModuleQuery *query) {
    const struct {
        const char *left;
        const char *right;
        bool conflict;
    } cases[] = {
        {"int", NULL, true},
        {"i64", "i64", true},
        {"i32[]", "i32[]", true},
        {"i32[!]", "i32[!]", true},
        {"i32", "i64", false},
        {"i32[]", "i64[]", false},
        {"i32[]", "i32[!]", false},
    };
    size_t count = 0U;

    for (size_t width = 4U; width <= 8U; width += 4U) {
        for (size_t exported = 0U; exported < 2U; ++exported) {
            for (size_t reverse = 0U; reverse < 2U; ++reverse) {
                for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
                    const char *right = cases[index].right != NULL ? cases[index].right
                        : width == 8U ? "i64" : "i32";
                    const char *targets[] = {cases[index].left, right};
                    const char *paths[] = {"witness_left.ff", "witness_right.ff"};
                    char sources[2][256];
                    FengProgram *fits[2];
                    FengProgram *header = witness_parse("witness_header.ff",
                        "open module witness.local;\nimport witness.external;\n"
                        "open spec Label { func label(): i32; }\n");

                    for (size_t side = 0U; side < 2U; ++side) {
                        int length = snprintf(sources[side], sizeof(sources[side]),
                            "open module witness.local;\n%sfit %s: Label { "
                            "func label(): i32 { return %zu; } }\n",
                            exported ? "open " : "", targets[side], side + 1U);
                        CHECK(length > 0 && (size_t)length < sizeof(sources[side]));
                        fits[side] = witness_parse(paths[side], sources[side]);
                    }
                    const FengProgram *programs[] = {header, fits[reverse], fits[1U - reverse]};
                    const char *token = index == 0U || index == 1U || index == 4U
                        ? targets[reverse] : "i32";
                    witness_analyze(programs, 3U, query, width,
                        cases[index].conflict ? "AE0804" : NULL,
                        cases[index].conflict ? 1U : 0U,
                        paths[reverse], 2U, exported ? 10U : 5U, token);
                    feng_program_free(header);
                    feng_program_free(fits[0]);
                    feng_program_free(fits[1]);
                    ++count;
                }
            }
        }
    }
    return count;
}

/* Parent materialization and the child witness retain the same fit location. */
static void witness_precompute_parent(const FengSemanticImportedModuleQuery *query) {
    FengProgram *program = witness_parse("witness_parent.ff",
        "open module witness.local;\nimport witness.external;\n"
        "open spec Parent { func label(): i32; }\nopen spec Label: Parent {}\n"
        "open fit i64: Label { func label(): i32 { return 1; } }\n"
        "open fit i64: Label { func label(): i32 { return 2; } }\n");
    const FengProgram *programs[] = {program};

    witness_analyze(programs, 1U, query, 8U, "AE0804", 2U,
        "witness_parent.ff", 5U, 10U, "i64");
    feng_program_free(program);
}

/* Unused local conflicts, direct calls and coercions retain existing timing. */
static void witness_on_demand(void) {
    const struct {
        const char *body;
        const char *code;
        const char *token;
        unsigned column;
    } cases[] = {
        {"", NULL, NULL, 0U},
        {"func check(v: i64) { v.label(); }\n", "AE0511", "label", 24U},
        {"func check(v: i64): Label { return v; }\n", "AE0804", "v", 36U},
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char source[512];
        int length = snprintf(source, sizeof(source),
            "module witness.local;\nspec Label { func label(): i32; }\n"
            "fit i64: Label { func label(): i32 { return 1; } }\n"
            "fit i64: Label { func label(): i32 { return 2; } }\n%s", cases[index].body);
        CHECK(length > 0 && (size_t)length < sizeof(source));
        FengProgram *program = witness_parse("witness_on_demand.ff", source);
        const FengProgram *programs[] = {program};

        witness_analyze(programs, 1U, NULL, 8U, cases[index].code,
            cases[index].code != NULL ? 1U : 0U, "witness_on_demand.ff",
            5U, cases[index].column, cases[index].token);
        feng_program_free(program);
    }
}

/* Imported fits use their provider file, including for an unused relation. */
static void witness_imported_conflict(void) {
    WitnessImportFixture fixture;
    witness_import_init(&fixture,
        "open module witness.external;\nopen spec Label { func label(): i32; }\n"
        "open fit i64: Label { func label(): i32 { return 1; } }\n"
        "open fit i64: Label { func label(): i32 { return 2; } }\n");
    FengSemanticImportedModuleQuery query = {.user = &fixture, .get_module = witness_get_module};
    FengProgram *program = witness_parse("witness_consumer.ff",
        "module witness.consumer;\nimport witness.external;\n");
    const FengProgram *programs[] = {program};

    witness_analyze(programs, 1U, &query, 8U, "AE0804", 1U,
        "witness_external.ff", 3U, 10U, "i64");
    feng_program_free(program);
    witness_import_free(&fixture);
}

/* Register this compiler-diagnostic regression with the standard semantic suite. */
void test_spec_witness_precompute_diagnostics(void) {
    WitnessImportFixture fixture;
    witness_import_init(&fixture, "open module witness.external;\n");
    FengSemanticImportedModuleQuery query = {.user = &fixture, .get_module = witness_get_module};
    size_t count = witness_precompute_matrix(&query);
    witness_precompute_parent(&query);
    witness_import_free(&fixture);
    witness_on_demand();
    witness_imported_conflict();
    printf("spec witness precompute: %zu diagnostic cases passed\n", count + 5U);
}
