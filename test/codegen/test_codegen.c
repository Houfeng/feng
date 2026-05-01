/* Codegen multi-file (P3) regression test.
 *
 * Drives parse -> semantic -> codegen for two source files belonging to two
 * distinct modules and verifies that the generated C aggregate contains
 * symbols mangled with each module's name, that the bin-target main wrapper
 * is emitted exactly once, and that codegen surfaces the per-program
 * external/function/global declarations into a single translation unit.
 */
#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(expr)                                                                  \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static const char *kSourceA =
    "mod feng.codegen.mfa;\n"
    "\n"
    "@cdecl(\"libc\")\n"
    "extern fn puts(msg: string): int;\n"
    "\n"
    "fn helper(): int {\n"
    "    return 42;\n"
    "}\n";

static const char *kSourceB =
    "mod feng.codegen.mfb;\n"
    "\n"
    "@cdecl(\"libc\")\n"
    "extern fn puts(msg: string): int;\n"
    "\n"
    "fn main(args: string[]) {\n"
    "    puts(\"multi-file ok\");\n"
    "}\n";

static FengProgram *parse_or_die(const char *source, const char *path) {
    FengProgram *program = NULL;
    FengParseError err;
    if (!feng_parse_source(source, strlen(source), path, &program, &err)) {
        fprintf(stderr, "%s:%u:%u: parse error: %s\n",
                path, err.token.line, err.token.column, err.message);
        exit(1);
    }
    return program;
}

typedef struct ImportedSourceFixture {
    FengProgram *program;
    FengSemanticAnalysis *analysis;
    FengSymbolGraph *graph;
    FengSymbolProvider *provider;
    FengSymbolImportedModuleCache *cache;
    FengSymbolError error;
} ImportedSourceFixture;

static void imported_source_fixture_init(ImportedSourceFixture *fixture,
                                         const char *path,
                                         const char *source) {
    const FengProgram *programs[1];
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    memset(fixture, 0, sizeof(*fixture));
    fixture->program = parse_or_die(source, path);
    programs[0] = fixture->program;
    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &fixture->analysis,
                                 &errors,
                                 &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_symbol_build_graph(fixture->analysis, &fixture->graph, &fixture->error));
    ASSERT(feng_symbol_provider_create(&fixture->provider, &fixture->error));
    ASSERT(feng_symbol_provider_add_graph(fixture->provider, fixture->graph, &fixture->error));
    fixture->cache = feng_symbol_imported_module_cache_create(fixture->provider);
    ASSERT(fixture->cache != NULL);
}

static void imported_source_fixture_dispose(ImportedSourceFixture *fixture) {
    if (fixture == NULL) {
        return;
    }

    feng_symbol_imported_module_cache_free(fixture->cache);
    feng_symbol_provider_free(fixture->provider);
    feng_symbol_graph_free(fixture->graph);
    feng_semantic_analysis_free(fixture->analysis);
    feng_program_free(fixture->program);
    feng_symbol_error_free(&fixture->error);
}

static char *make_temp_dir(void) {
    char *template_path = strdup("/tmp/feng_codegen_imported_XXXXXX");
    char *result;

    ASSERT(template_path != NULL);
    result = mkdtemp(template_path);
    ASSERT(result != NULL);
    return result;
}

static int remove_dir_recursive(const char *path) {
    char command[1024];
    int written = snprintf(command, sizeof(command), "rm -rf '%s'", path);

    if (written < 0 || (size_t)written >= sizeof(command)) {
        return -1;
    }
    return system(command);
}

static void write_text_file_or_die(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    size_t length = strlen(text);

    ASSERT(file != NULL);
    ASSERT(fwrite(text, 1U, length, file) == length);
    ASSERT(fclose(file) == 0);
}

static void compile_generated_c_or_die(const char *c_source) {
    char *tmp_dir = make_temp_dir();
    char c_path[1024];
    char o_path[1024];
    char command[3072];

    ASSERT(snprintf(c_path, sizeof(c_path), "%s/generated.c", tmp_dir) > 0);
    ASSERT(snprintf(o_path, sizeof(o_path), "%s/generated.o", tmp_dir) > 0);
    write_text_file_or_die(c_path, c_source);
    ASSERT(snprintf(command,
                    sizeof(command),
                    "cc -Isrc -Ithird_party/miniz -std=c11 -Werror -c '%s' -o '%s' >/dev/null 2>&1",
                    c_path,
                    o_path) > 0);
    if (system(command) != 0) {
        fprintf(stderr, "generated C failed to compile: %s\n", command);
        ASSERT(false);
    }
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static size_t count_substr(const char *haystack, const char *needle) {
    size_t count = 0U;
    size_t nlen = strlen(needle);
    if (nlen == 0U) return 0U;
    for (const char *p = haystack; (p = strstr(p, needle)) != NULL; p += nlen) {
        count++;
    }
    return count;
}

static void test_multi_file_bin(void) {
    FengProgram *prog_a = parse_or_die(kSourceA, "tests/mfa.ff");
    FengProgram *prog_b = parse_or_die(kSourceB, "tests/mfb.ff");

    const FengProgram *programs[2] = { prog_a, prog_b };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_BIN,
                                    &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_BIN,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error: %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(out.c_source_length > 0U);

    /* Each program's module mangle (feng__<segments>) must surface in at
     * least one emitted symbol — proving both files reached the codegen
     * aggregate rather than being dropped by the legacy single-program
     * gate. */
    ASSERT(strstr(out.c_source, "feng__feng__codegen__mfa") != NULL);
    ASSERT(strstr(out.c_source, "feng__feng__codegen__mfb") != NULL);
    /* The helper from program A and main from program B must both be
     * emitted as full function definitions (i.e. with their mangled names
     * visible in the source). */
    ASSERT(strstr(out.c_source, "feng__feng__codegen__mfa__helper") != NULL);
    ASSERT(strstr(out.c_source, "feng__feng__codegen__mfb__main") != NULL);
    /* Exactly one C `main` entry wrapper for the binary. */
    ASSERT(count_substr(out.c_source, "int main(int argc, char **argv)") == 1U);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(prog_a);
    feng_program_free(prog_b);
}

static void test_multi_file_lib(void) {
    /* For lib target, no main is required and no main wrapper should be
     * emitted. Reuses the same two sources but compiles only mfa (which
     * has no main) — semantic should accept it for the lib target. */
    FengProgram *prog_a = parse_or_die(kSourceA, "tests/mfa.ff");
    /* A second helper-only program in a third module to exercise multi-file
     * aggregation in the lib target. */
    static const char *kSourceC =
        "mod feng.codegen.mfc;\n"
        "\n"
        "fn other(): int { return 7; }\n";
    FengProgram *prog_c = parse_or_die(kSourceC, "tests/mfc.ff");

    const FengProgram *programs[2] = { prog_a, prog_c };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(ok);
    }

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen (lib) error: %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* lib target: no main wrapper. */
    ASSERT(strstr(out.c_source, "int main(int argc, char **argv)") == NULL);
    /* Both module mangles surface. */
    ASSERT(strstr(out.c_source, "feng__codegen__mfa") != NULL);
    ASSERT(strstr(out.c_source, "feng__codegen__mfc") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(prog_a);
    feng_program_free(prog_c);
}

static void test_lib_public_functions_are_exported(void) {
    static const char *kSource =
        "mod feng.codegen.export;\n"
        "pu fn public_fn(): i32 {\n"
        "    return 1;\n"
        "}\n"
        "fn hidden_fn(): i32 {\n"
        "    return 2;\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/export_lib.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);

    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr);
    if (!ok) {
        fprintf(stderr, "codegen error: %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "int32_t feng__feng__codegen__export__public_fn__from__void(") != NULL);
    ASSERT(strstr(out.c_source,
                  "static int32_t feng__feng__codegen__export__public_fn__from__void(") == NULL);
    ASSERT(strstr(out.c_source,
                  "static int32_t feng__feng__codegen__export__hidden_fn__from__void(") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_imported_feng_function_prototypes_compile(void) {
    static const char *kImportedSource =
        "pu mod vendor.api;\n"
        "pu type User {\n"
        "    pu let name: string;\n"
        "}\n"
        "pu fn make(): User {\n"
        "    return User { name: \"hi\" };\n"
        "}\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "use vendor.api as api;\n"
        "fn project() {\n"
        "    api.make();\n"
        "}\n";
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    imported_source_fixture_init(&fixture, "tests/imported_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_consumer.ff");
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "struct Feng__vendor__api__User;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern const FengTypeDescriptor FengTypeDesc__vendor__api__User;") != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__vendor__api__User * feng__vendor__api__make__from__void(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__vendor__api__User * feng__vendor__api__make__from__void(void) {") == NULL);
    ASSERT(strstr(out.c_source,
                  "const FengTypeDescriptor FengTypeDesc__vendor__api__User = {") == NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_bin_public_functions_remain_static(void) {
    static const char *kSource =
        "mod feng.codegen.exportbin;\n"
        "pu fn public_fn(): i32 {\n"
        "    return 1;\n"
        "}\n"
        "fn main(args: string[]) {\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/export_bin.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_BIN,
                                    &analysis, &errors, &error_count);

    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_BIN,
                                   NULL, &out, &cgerr);
    if (!ok) {
        fprintf(stderr, "codegen error: %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "static int32_t feng__feng__codegen__exportbin__public_fn__from__void(") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Regression for the multi-file project bug where two distinct `type User`
 * declarations in two different modules were conflated by codegen because
 * the user-type lookup keyed on the simple name only. With the visibility
 * filter in place, each module's identifier `User` must resolve to its
 * OWN type decl, and field access against that type must succeed against
 * the matching field set even when the other module's `User` carries a
 * different (and incompatible) field set. */
static void test_same_named_types_in_distinct_modules(void) {
    static const char *kHelloSrc =
        "mod feng.codegen.dup.hello;\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "fn make_hello(): string {\n"
        "    let u = User { name: \"hi\" };\n"
        "    return u.name;\n"
        "}\n";
    static const char *kDebugSrc =
        "mod feng.codegen.dup.debug;\n"
        "type User {\n"
        "    let id: i32;\n"
        "}\n"
        "fn make_debug(): i32 {\n"
        "    let u = User { id: 7 };\n"
        "    return u.id;\n"
        "}\n";

    FengProgram *prog_hello = parse_or_die(kHelloSrc, "tests/dup_hello.ff");
    FengProgram *prog_debug = parse_or_die(kDebugSrc, "tests/dup_debug.ff");

    const FengProgram *programs[2] = { prog_hello, prog_debug };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze(programs, 2U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(ok);
    }

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr,
                "codegen error: path=%s msg=%s line=%u col=%u\n",
                cgerr.path ? cgerr.path : "(null)",
                cgerr.message ? cgerr.message : "(unknown)",
                cgerr.token.line, cgerr.token.column);
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* Both User types must be emitted with module-qualified C struct names. */
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__dup__hello__User") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__dup__debug__User") != NULL);
    /* The hello User must carry a `name` field; the debug User must carry an
     * `id` field. If codegen had wired the wrong type, one of these would be
     * missing on its expected struct. */
    {
        const char *hello_struct = strstr(out.c_source,
            "struct Feng__feng__codegen__dup__hello__User {");
        const char *debug_struct = strstr(out.c_source,
            "struct Feng__feng__codegen__dup__debug__User {");
        ASSERT(hello_struct != NULL);
        ASSERT(debug_struct != NULL);
        const char *hello_end = strstr(hello_struct, "};");
        const char *debug_end = strstr(debug_struct, "};");
        ASSERT(hello_end != NULL && debug_end != NULL);
        /* hello.User has only `name`; debug.User has only `id`. */
        size_t hello_len = (size_t)(hello_end - hello_struct);
        size_t debug_len = (size_t)(debug_end - debug_struct);
        ASSERT(memmem(hello_struct, hello_len, "name", 4U) != NULL);
        ASSERT(memmem(hello_struct, hello_len, "id;", 3U) == NULL);
        ASSERT(memmem(debug_struct, debug_len, "id;", 3U) != NULL);
        ASSERT(memmem(debug_struct, debug_len, "name;", 5U) == NULL);
    }

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(prog_hello);
    feng_program_free(prog_debug);
}

static void test_float_modulo_codegen_uses_math_runtime(void) {
    static const char *kOpsSrc =
        "mod feng.codegen.ops;\n"
        "fn run() {\n"
        "    var total: float = (float)7.8;\n"
        "    total %= (float)3.2;\n"
        "}\n";

    FengProgram *program = parse_or_die(kOpsSrc, "tests/ops.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error: %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "#include <math.h>") != NULL);
    ASSERT(strstr(out.c_source, "fmodf(") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

int main(void) {
    test_multi_file_bin();
    test_multi_file_lib();
    test_lib_public_functions_are_exported();
    test_bin_public_functions_remain_static();
    test_imported_feng_function_prototypes_compile();
    test_same_named_types_in_distinct_modules();
    test_float_modulo_codegen_uses_math_runtime();
    fprintf(stdout, "codegen tests passed\n");
    return 0;
}
