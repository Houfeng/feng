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
    "extern fn c_puts(msg: string*): int;\n"
    "\n"
    "fn helper(): int {\n"
    "    return 42;\n"
    "}\n";

static const char *kSourceB =
    "mod feng.codegen.mfb;\n"
    "\n"
    "@cdecl(\"libc\")\n"
    "extern fn c_puts(msg: string*): int;\n"
    "\n"
    "fn main(args: string[]) {\n"
    "    c_puts(&\"multi-file ok\");\n"
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
                    "cc -Isrc -Ithird_party/miniz -std=gnu11 -fexceptions -Werror -c '%s' -o '%s' >/dev/null 2>&1",
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

static void assert_builtin_subject_thunks_direct_fit_call(const char *c_source) {
    const char *cursor = c_source;
    size_t checked = 0U;

    while ((cursor = strstr(cursor, "static ")) != NULL) {
        const char *name = strstr(cursor, "FengSpecThunk__");
        const char *open = strchr(cursor, '{');
        const char *close = NULL;
        const char *subject_marker = NULL;

        if (name == NULL || (open != NULL && name > open)) {
            cursor += 7;
            continue;
        }
        subject_marker = strstr(name, "__subject_");
        if (subject_marker == NULL || (open != NULL && subject_marker > open)) {
            cursor += 7;
            continue;
        }
        if (open == NULL) {
            break;
        }
        close = strstr(open, "\n}\n\n");
        ASSERT(close != NULL);

        size_t body_len = (size_t)(close - open);
        char *body = (char *)malloc(body_len + 1U);
        ASSERT(body != NULL);
        memcpy(body, open, body_len);
        body[body_len] = '\0';

        if (strstr(body, "_self_value") == NULL && strstr(body, "_self_ref") == NULL) {
            free(body);
            cursor = close + 1;
            continue;
        }

        ASSERT(strstr(body, "FengFitBuiltin_") != NULL);
        ASSERT(strstr(body, "witness->") == NULL);
        ASSERT(strstr(body, "FengSpecThunk__") == NULL);

        free(body);
        checked++;
        cursor = close + 1;
    }

    ASSERT(checked > 0U);
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
    ASSERT(strstr(out.c_source, "c_puts(char *)") != NULL);
    ASSERT(strstr(out.c_source, "c_puts(((char *)feng_string_data(") != NULL);
    /* Exactly one C `main` entry wrapper for the binary. */
    ASSERT(count_substr(out.c_source, "int main(int argc, char **argv)") == 1U);
    compile_generated_c_or_die(out.c_source);

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

static void test_module_binding_lazy_ensure_init_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.topbind;\n"
        "let first: int = compute();\n"
        "let second: int = 41;\n"
        "fn compute(): int {\n"
        "    return second + 1;\n"
        "}\n"
        "fn helper(): int {\n"
        "    return first;\n"
        "}\n"
        "fn main(args: string[]) {\n"
        "    let result: int = helper();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "topbind.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_BIN,
                                    &analysis, &errors, &error_count);

    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line, errors[i].token.column,
                    errors[i].message);
        }
    }
    ASSERT(ok);
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_BIN,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (module binding lazy ensure_init): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "static bool _feng_g__feng__codegen__topbind__first__inited = false;") != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_ensure_g__feng__codegen__topbind__first(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_ensure_g__feng__codegen__topbind__second();") != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_ensure_g__feng__codegen__topbind__first();") != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_g__feng__codegen__topbind__first") != NULL);
    {
        const char *main_wrapper = strstr(out.c_source, "int main(int argc, char **argv) {");

        ASSERT(main_wrapper != NULL);
        ASSERT(strstr(main_wrapper, "_feng_g__feng__codegen__topbind__first =") == NULL);
        ASSERT(strstr(main_wrapper, "_feng_g__feng__codegen__topbind__second =") == NULL);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_address_of_module_binding_uses_storage_slot_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.topbindaddr;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_int_ptr(p: int*): void;\n"
        "let value: int = (int)7;\n"
        "fn run_case() {\n"
        "    c_use_int_ptr(&value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "topbindaddr.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (module binding address-of): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_ensure_g__feng__codegen__topbindaddr__value();") != NULL);
    ASSERT(strstr(out.c_source,
                  "(&(_feng_g__feng__codegen__topbindaddr__value))") != NULL);
    ASSERT(strstr(out.c_source,
                  "(&(_feng_ensure_g__feng__codegen__topbindaddr__value()))") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_module_scalar_var_assignment_marks_initialized_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.topbindassign;\n"
        "var current: int = (int)1;\n"
        "fn run_case() {\n"
        "    current = (int)7;\n"
        "    current += 1;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "topbindassign.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (module scalar var assignment): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(count_substr(out.c_source,
                        "_feng_ensure_g__feng__codegen__topbindassign__current();") == 2U);
    ASSERT(count_substr(out.c_source,
                        "_feng_g__feng__codegen__topbindassign__current__inited = true;") == 1U);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_module_managed_var_assignment_marks_initialized_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.topbindobj;\n"
        "type Box {\n"
        "    let value: int;\n"
        "}\n"
        "var current: Box = Box { value: (int)1 };\n"
        "fn reset(next: Box) {\n"
        "    current = next;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "topbindobj.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (module managed var assignment): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(count_substr(out.c_source,
                        "_feng_ensure_g__feng__codegen__topbindobj__current();") == 1U);
    ASSERT(count_substr(out.c_source,
                        "_feng_g__feng__codegen__topbindobj__current__inited = true;") == 1U);
    ASSERT(strstr(out.c_source,
                  "feng_assign((void**)&_feng_g__feng__codegen__topbindobj__current, next);") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_module_binding_default_zero_ensure_init_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.topbindzero;\n"
        "type User {\n"
        "    let id: int;\n"
        "}\n"
        "let current: User;\n"
        "fn read_id(): int {\n"
        "    return current.id;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "topbindzero.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (module default-zero ensure_init): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_ensure_g__feng__codegen__topbindzero__current(void)") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__topbindzero__User__default_zero()") != NULL);
    ASSERT(strstr(out.c_source,
                  "_feng_ensure_g__feng__codegen__topbindzero__current()") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_extern_calling_convention_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.callconv;\n"
        "@stdcall(\"user32\")\n"
        "extern fn stdcall_value(value: int): int;\n"
        "@fastcall(\"helper\")\n"
        "extern fn fastcall_value(value: int): int;\n"
        "fn run() {\n"
        "    let value = stdcall_value(1);\n"
        "    fastcall_value(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "callconv.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (extern callconv): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "#define FENG_EXTERN_CALLCONV_STDCALL") != NULL);
    ASSERT(strstr(out.c_source, "#define FENG_EXTERN_CALLCONV_FASTCALL") != NULL);
    ASSERT(strstr(out.c_source, "FENG_EXTERN_CALLCONV_STDCALL stdcall_value(") != NULL);
    ASSERT(strstr(out.c_source, "FENG_EXTERN_CALLCONV_FASTCALL fastcall_value(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_address_of_scalar_and_array_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.addr;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_i32_ptr(p: i32*): void;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_array_ptr(p: int*): void;\n"
        "fn run() {\n"
        "    let value: i32 = 7;\n"
        "    let values: int[] = [1, 2, 3];\n"
        "    c_use_i32_ptr(&value);\n"
        "    c_use_array_ptr(&values);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "addr.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (address-of scalar/array): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_array_data(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_function_pointer_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.abifn;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@abi\n"
        "type Holder {\n"
        "    var cb: Cmp*;\n"
        "}\n"
        "@abi\n"
        "fn cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern fn c_register_cmp(cb: Cmp*): void;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_load_cmp(): Cmp*;\n"
        "fn run() {\n"
        "    let cb: Cmp* = &cmp;\n"
        "    let holder: Holder = Holder{cb: cb};\n"
        "    let same: bool = cb == holder.cb;\n"
        "    c_register_cmp(cb);\n"
        "    c_register_cmp(&cmp);\n"
        "    holder.cb = c_load_cmp();\n"
        "    let different: bool = cb != holder.cb;\n"
        "    c_register_cmp(holder.cb);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "abifn.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (ABI function pointer): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "typedef ") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengAbiFnPtr__feng__codegen__abifn__Cmp") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern FengAbiFnPtr__feng__codegen__abifn__Cmp c_load_cmp(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengAbiFnPtr__feng__codegen__abifn__Cmp cb;") != NULL);
    ASSERT(strstr(out.c_source, "&feng__feng__codegen__abifn__cmp") != NULL);
    ASSERT(strstr(out.c_source, "c_load_cmp()") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_value_pointer_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.abivalue;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: i32;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_point_ptr(p: Point*): void;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_roundtrip_point_ptr(p: Point*): Point*;\n"
        "fn run() {\n"
        "    let point: Point = Point{x: 1, y: 2};\n"
        "    let handle: Point* = &point;\n"
        "    c_use_point_ptr(&point);\n"
        "    c_use_point_ptr(handle);\n"
        "    let other: Point* = c_roundtrip_point_ptr(handle);\n"
        "    let same: bool = handle == other;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "abivalue.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (ABI value pointer): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__feng__codegen__abivalue__Point__AbiLayout {") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__abivalue__Point__abi_base_offset") != NULL);
    ASSERT(strstr(out.c_source,
                  "offsetof(struct Feng__feng__codegen__abivalue__Point, x)") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__abivalue__Point__abi_ptr(") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void c_use_point_ptr(struct Feng__feng__codegen__abivalue__Point__AbiLayout *") != NULL);
    ASSERT(strstr(out.c_source,
                  "c_use_point_ptr(Feng__feng__codegen__abivalue__Point__abi_ptr(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_array_pointee_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.arraypointee;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "}\n"
        "@abi\n"
        "type ByteSpan {\n"
        "    var data: byte[]*;\n"
        "    var len: int;\n"
        "}\n"
        "@abi\n"
        "type PointSpan {\n"
        "    var data: Point[]*;\n"
        "    var len: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern fn c_load_bytes(): byte[]*;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_bytes(data: byte[]*): void;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_load_points(): Point[]*;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_points(data: Point[]*): void;\n"
        "fn run() {\n"
        "    let bytes: byte[]* = c_load_bytes();\n"
        "    let points: Point[]* = c_load_points();\n"
        "    let byte_span: ByteSpan = ByteSpan{data: bytes, len: 4};\n"
        "    let point_span: PointSpan = PointSpan{data: points, len: 2};\n"
        "    c_use_bytes(byte_span.data);\n"
        "    c_use_points(point_span.data);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "arraypointee.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (ABI array pointee): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "extern uint8_t * c_load_bytes(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void c_use_bytes(uint8_t *);") != NULL);
    ASSERT(strstr(out.c_source,
                  "uint8_t * data;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__feng__codegen__arraypointee__Point__AbiLayout * c_load_points(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void c_use_points(struct Feng__feng__codegen__arraypointee__Point__AbiLayout *);") != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__feng__codegen__arraypointee__Point__AbiLayout * data;") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fieldless_abi_pointer_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.opaquehandle;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern fn c_make_handle(): Handle*;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_handle(handle: Handle*): void;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_roundtrip_handle(handle: Handle*): Handle*;\n"
        "fn run() {\n"
        "    let handle: Handle* = c_make_handle();\n"
        "    c_use_handle(handle);\n"
        "    let other: Handle* = c_roundtrip_handle(handle);\n"
        "    let same: bool = handle == other;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "opaquehandle.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (fieldless ABI pointer): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__feng__codegen__opaquehandle__Handle;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__feng__codegen__opaquehandle__Handle * c_make_handle(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void c_use_handle(struct Feng__feng__codegen__opaquehandle__Handle *") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__feng__codegen__opaquehandle__Handle * c_roundtrip_handle(struct Feng__feng__codegen__opaquehandle__Handle *") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__opaquehandle__Handle__AbiLayout") == NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__opaquehandle__Handle__abi_ptr(") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fieldless_abi_function_surface_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.opaqueabifn;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@abi\n"
        "pu fn roundtrip(handle: Handle*): Handle* {\n"
        "    return handle;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "opaqueabifn.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (fieldless ABI function surface): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__feng__codegen__opaqueabifn__Handle;") != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__feng__codegen__opaqueabifn__Handle * feng__feng__codegen__opaqueabifn__roundtrip__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__opaqueabifn__Handle__AbiLayout") == NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__opaqueabifn__Handle__abi_ptr(") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_value_extern_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.abivalueextern;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern fn create_point(x: int, y: int): Point;\n"
        "@cdecl(\"c\")\n"
        "extern fn point_sum(p: Point): int;\n"
        "fn run() {\n"
        "    let point: Point = create_point(1, 2);\n"
        "    let total: int = point_sum(point);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "abivalueextern.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (ABI value extern): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__feng__codegen__abivalueextern__Point__AbiLayout create_point(") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern int32_t point_sum(struct Feng__feng__codegen__abivalueextern__Point__AbiLayout") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__abivalueextern__Point__abi_box(") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__abivalueextern__Point__abi_value(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_runtime_extern_codegen_uses_feng_surface_types(void) {
    static const char *kSource =
        "mod feng.codegen.runtimeextern;\n"
        "@runtime\n"
        "extern fn feng_string_utf8_length(value: string): long;\n"
        "fn run(value: string): long {\n"
        "    return feng_string_utf8_length(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "runtimeextern.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (runtime extern): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_string_utf8_length(value)") != NULL);
    ASSERT(strstr(out.c_source, "extern int64_t feng_string_utf8_length(") == NULL);
    ASSERT(strstr(out.c_source, "feng_string_utf8_length(((char *)feng_string_data(") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_call_infers_type_args(void) {
    static const char *kSource =
        "mod feng.codegen.genericruntimeextern;\n"
        "@runtime\n"
        "extern fn feng_array_length_i64<T>(value: T[]): long;\n"
        "fn run(values: int[]): long {\n"
        "    return feng_array_length_i64(values);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genericruntimeextern.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic runtime extern inference): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_array_length_i64(&(const FengGenericParamDescriptor){.size = sizeof(int32_t), .kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_I32, .aggregate = NULL, .witness = NULL}, values)") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_call_accepts_explicit_type_args(void) {
    static const char *kSource =
        "mod feng.codegen.genericruntimeexternexplicit;\n"
        "@runtime\n"
        "extern fn feng_array_length_i64<T>(value: T[]): long;\n"
        "fn run(values: int[]): long {\n"
        "    return feng_array_length_i64<int>(values);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genericruntimeexternexplicit.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic runtime extern explicit args): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_array_length_i64(&(const FengGenericParamDescriptor){.size = sizeof(int32_t), .kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_I32, .aggregate = NULL, .witness = NULL}, values)") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_expression_equal_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.genericruntimeexprequal;\n"
        "@runtime\n"
        "extern fn feng_expression_equal<T>(left: T, right: T): bool;\n"
        "fn run(left: int, right: int): bool {\n"
        "    return feng_expression_equal(left, right);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genericruntimeexprequal.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic runtime extern expression_equal): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_expression_equal(&(const FengGenericParamDescriptor){.size = sizeof(int32_t), .kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_I32, .aggregate = NULL, .witness = NULL}, &_rga") != NULL);
    ASSERT(count_substr(out.c_source, "int32_t _rga") == 2U);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_direct_type_param_return_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.genericruntimeidentityreturn;\n"
        "@runtime\n"
        "extern fn __test_value_identity<T>(value: T): T;\n"
        "fn run(value: int): int {\n"
        "    return __test_value_identity(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genericruntimeidentityreturn.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic runtime extern bare return): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "__test_value_identity(&(const FengGenericParamDescriptor){.size = sizeof(int32_t), .kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_I32, .aggregate = NULL, .witness = NULL}, &_rga") != NULL);
    ASSERT(strstr(out.c_source, ", &_rgr") != NULL);
    ASSERT(count_substr(out.c_source, "int32_t _rga") == 1U);
    ASSERT(count_substr(out.c_source, "int32_t _rgr") == 1U);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_runtime_extern_codegen_rejects_non_contract_symbol(void) {
    static const char *kSource =
        "mod feng.codegen.runtimeexternreject;\n"
        "@runtime\n"
        "extern fn feng_not_contract(value: string): long;\n"
        "fn run(value: string): long {\n"
        "    return feng_not_contract(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "runtimeexternreject.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);

    ASSERT(!cg_ok);
    ASSERT(cgerr.message != NULL);
    ASSERT(strstr(cgerr.message, "is not declared by runtime contract") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_unsupported_pointer_pointee_reports_explicit_error(void) {
    static const char *kSource =
        "mod feng.codegen.badpointee;\n"
        "type User {\n"
        "    var name: string;\n"
        "}\n"
        "fn run() {\n"
        "    let p: User*;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "badpointee.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);

    ASSERT(!cg_ok);
    ASSERT(cgerr.message != NULL);
    ASSERT(strstr(cgerr.message,
                  "does not support ABI pointer lowering") != NULL);
    ASSERT(strstr(cgerr.message,
                  "not supported in this step") == NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_value_function_pointer_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.abivaluefn;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@abi\n"
        "spec PointMapper(p: Point): Point;\n"
        "@abi\n"
        "pu fn echo(p: Point): Point {\n"
        "    return p;\n"
        "}\n"
        "fn run() {\n"
        "    let cb: PointMapper* = &echo;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "abivaluefn.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (ABI value function pointer): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "FengAbiFnPtr__feng__codegen__abivaluefn__PointMapper") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__abivaluefn__Point__abi_box(") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__abivaluefn__Point__abi_value(") != NULL);
    ASSERT(strstr(out.c_source, "__impl(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_lib_public_functions_are_exported(void) {
    static const char *kSource =
        "mod feng.codegen.exposed;\n"
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
                  "int32_t feng__feng__codegen__exposed__public_fn__from__void(") != NULL);
    ASSERT(strstr(out.c_source,
                  "static int32_t feng__feng__codegen__exposed__public_fn__from__void(") == NULL);
    ASSERT(strstr(out.c_source,
                  "static int32_t feng__feng__codegen__exposed__hidden_fn__from__void(") != NULL);

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

static void test_imported_alias_qualified_type_annotations_codegen_compile(void) {
    static const char *kImportedSource =
        "pu mod vendor.api;\n"
        "pu type User {\n"
        "    pu let name: string;\n"
        "}\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "use vendor.api as api;\n"
        "fn keep(user: api.User): api.User {\n"
        "    return user;\n"
        "}\n"
        "fn count(users: api.User[]): int {\n"
        "    return 0;\n"
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

    imported_source_fixture_init(&fixture, "tests/imported_alias_type_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_alias_type_consumer.ff");
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
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_full_path_type_annotations_codegen_compile_without_use(void) {
    static const char *kImportedSource =
        "pu mod vendor.api;\n"
        "pu type User {\n"
        "    pu let name: string;\n"
        "}\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "fn keep(user: vendor.api.User): vendor.api.User {\n"
        "    return user;\n"
        "}\n"
        "fn count(users: vendor.api.User[]): int {\n"
        "    return 0;\n"
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

    imported_source_fixture_init(&fixture, "tests/imported_full_path_type_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_full_path_type_consumer.ff");
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
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_public_let_binding_codegen_compiles(void) {
    static const char *kImportedSource =
        "pu mod vendor.values;\n"
        "pu let count: int = 7;\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "use vendor.values as values;\n"
        "fn project(): int {\n"
        "    return values.count;\n"
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

    imported_source_fixture_init(&fixture, "tests/imported_binding_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_binding_consumer.ff");
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
    ASSERT(strstr(out.c_source,
                  "extern int32_t feng__vendor__values__count;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__vendor__values__count__ensure_init__from__void(void);") != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng__vendor__values__count__ensure_init__from__void();") == 1U);
    ASSERT(strstr(out.c_source,
                  "feng__vendor__values__count") != NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_public_var_binding_read_write_codegen_compiles(void) {
    static const char *kImportedSource =
        "pu mod vendor.state;\n"
        "pu var count: int = 1;\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "use vendor.state as state;\n"
        "fn project(next: int): int {\n"
        "    let before: int = state.count;\n"
        "    state.count = next;\n"
        "    return state.count + before;\n"
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

    imported_source_fixture_init(&fixture, "tests/imported_var_binding_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_var_binding_consumer.ff");
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
    ASSERT(strstr(out.c_source,
                  "extern int32_t feng__vendor__state__count;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__vendor__state__count__ensure_init__from__void(void);") != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng__vendor__state__count__ensure_init__from__void();") == 3U);
    ASSERT(strstr(out.c_source,
                  "feng__vendor__state__count = (int32_t)(next);") != NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_public_binding_address_of_codegen_compiles(void) {
    static const char *kImportedSource =
        "pu mod vendor.values;\n"
        "pu let count: int = 7;\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "use vendor.values as values;\n"
        "@cdecl(\"c\")\n"
        "extern fn c_use_int_ptr(p: int*): void;\n"
        "fn run_case() {\n"
        "    c_use_int_ptr(&values.count);\n"
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

    imported_source_fixture_init(&fixture, "tests/imported_binding_addr_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_binding_addr_consumer.ff");
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
    ASSERT(strstr(out.c_source,
                  "extern int32_t feng__vendor__values__count;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__vendor__values__count__ensure_init__from__void(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng__vendor__values__count__ensure_init__from__void();") != NULL);
    ASSERT(strstr(out.c_source,
                  "(&(feng__vendor__values__count))") != NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_public_binding_lib_exports_slot_and_ensure_init_codegen(void) {
    static const char *kSource =
        "pu mod vendor.values;\n"
        "pu let count: int = 3 + 4;\n"
        "pu var total: int = 1;\n"
        "pu fn read(): int {\n"
        "    return count + total;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "public_binding_export.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    {
        bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                               NULL, &out, &cgerr);
        if (!cg_ok) {
            fprintf(stderr, "codegen error (public binding export): %s\n",
                    cgerr.message ? cgerr.message : "(unknown)");
            ASSERT(cg_ok);
        }
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "int32_t feng__vendor__values__count = 0;") != NULL);
    ASSERT(strstr(out.c_source,
                  "int32_t feng__vendor__values__total = 0;") != NULL);
    ASSERT(strstr(out.c_source,
                  "void feng__vendor__values__count__ensure_init__from__void(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "void feng__vendor__values__total__ensure_init__from__void(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "static int32_t feng__vendor__values__count = 0;") == NULL);
    ASSERT(strstr(out.c_source,
                  "static void feng__vendor__values__count__ensure_init__from__void(void)") == NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_public_binding_infers_constructor_type_codegen(void) {
    static const char *kSource =
        "pu mod vendor.math;\n"
        "pu type Math {\n"
        "}\n"
        "pu let math = Math();\n"
        "pu fn current(): Math {\n"
        "    return math;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "public_binding_inferred_type.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__vendor__math__Math * feng__vendor__math__math = NULL;") != NULL);
    ASSERT(strstr(out.c_source,
                  "void feng__vendor__math__math__ensure_init__from__void(void);") != NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_imported_public_binding_inferred_type_codegen_compiles(void) {
    static const char *kImportedSource =
        "pu mod vendor.math;\n"
        "pu type Math {\n"
        "}\n"
        "pu let math = Math();\n";
    static const char *kConsumerSource =
        "mod demo.main;\n"
        "use vendor.math as m;\n"
        "fn project(): int {\n"
        "    let first = m.math;\n"
        "    return 1;\n"
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

    imported_source_fixture_init(&fixture,
                                 "tests/imported_inferred_binding_vendor.ff",
                                 kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_inferred_binding_consumer.ff");
    programs[0] = program;
    {
        bool ok = feng_semantic_analyze_with_options(programs,
                                                     1U,
                                                     &options,
                                                     &analysis,
                                                     &errors,
                                                     &error_count);
        if (!ok) {
            for (size_t i = 0; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path,
                        errors[i].token.line,
                        errors[i].token.column,
                        errors[i].message);
            }
        }
        ASSERT(ok);
    }
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    {
        bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                               NULL, &out, &cgerr);
        if (!cg_ok) {
            fprintf(stderr, "codegen error (imported inferred binding): %s\n",
                    cgerr.message ? cgerr.message : "(unknown)");
        }
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__vendor__math__Math * feng__vendor__math__math;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__vendor__math__math__ensure_init__from__void(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng__vendor__math__math__ensure_init__from__void();") != NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_enum_codegen_emits_stable_symbols(void) {
    static const char *kSource =
        "mod feng.codegen.enumvalue;\n"
        "enum HttpStatus { Ok = 200, NotFound = 404 }\n"
        "@cdecl(\"m\")\n"
        "extern fn use_status_ptr(status: HttpStatus*): void;\n"
        "type Response {\n"
        "    let status: HttpStatus;\n"
        "}\n"
        "fn fallback(): HttpStatus {\n"
        "    let status: HttpStatus;\n"
        "    return status;\n"
        "}\n"
        "fn roundtrip(status: HttpStatus, history: HttpStatus[]): HttpStatus {\n"
        "    let current: HttpStatus = history[0];\n"
        "    let ptr: HttpStatus* = &current;\n"
        "    use_status_ptr(ptr);\n"
        "    if status == current {\n"
        "        return status;\n"
        "    }\n"
        "    return current;\n"
        "}\n"
        "fn selected(): int {\n"
        "    let response: Response = Response { status: HttpStatus.NotFound };\n"
        "    return (int)response.status;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/enum_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "typedef int32_t FengEnum__feng__codegen__enumvalue__HttpStatus;") != NULL);
    ASSERT(strstr(out.c_source,
                  "static const FengEnum__feng__codegen__enumvalue__HttpStatus FengEnum__feng__codegen__enumvalue__HttpStatus__Ok = ((int32_t)200);") != NULL);
    ASSERT(strstr(out.c_source,
                  "static const FengEnum__feng__codegen__enumvalue__HttpStatus FengEnum__feng__codegen__enumvalue__HttpStatus__NotFound = ((int32_t)404);") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengEnum__feng__codegen__enumvalue__HttpStatus__NotFound") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengEnum__feng__codegen__enumvalue__HttpStatus__Ok") != NULL);
    ASSERT(strstr(out.c_source, "use_status_ptr") != NULL);
    ASSERT(strstr(out.c_source, "feng_scalar_box_new_i32") == NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_imported_enum_codegen_emits_visible_symbols(void) {
    static const char *kImportedSource =
        "pu mod vendor.http;\n"
        "pu enum HttpStatus { Ok = 200, NotFound = 404 }\n";
    static const char *kConsumerSource =
        "mod demo.enumconsumer;\n"
        "use vendor.http as http;\n"
        "fn selected(): int {\n"
        "    return (int)http.HttpStatus.NotFound;\n"
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

    imported_source_fixture_init(&fixture, "tests/imported_enum_vendor.ff", kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;

    program = parse_or_die(kConsumerSource, "tests/imported_enum_consumer.ff");
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "typedef int32_t FengEnum__vendor__http__HttpStatus;") != NULL);
    ASSERT(strstr(out.c_source,
                  "static const FengEnum__vendor__http__HttpStatus FengEnum__vendor__http__HttpStatus__NotFound = ((int32_t)404);") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengEnum__vendor__http__HttpStatus__NotFound") != NULL);

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

static void test_fit_builtin_direct_call_codegen_shape(void) {
    static const char *kFitBuiltinSource =
        "mod feng.codegen.fitbuiltin;\n"
        "fit i32 {\n"
        "    fn double(): i32 {\n"
        "        return self * 2;\n"
        "    }\n"
        "}\n"
        "fit int[] {\n"
        "    fn head(): int {\n"
        "        return self[0];\n"
        "    }\n"
        "}\n"
        "fn run(): int {\n"
        "    let xs: int[] = [7, 9];\n"
        "    return 21.double() + xs.head();\n"
        "}\n";

    FengProgram *program = parse_or_die(kFitBuiltinSource, "tests/fit_builtin_codegen.ff");
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
        fprintf(stderr, "codegen error (fit builtin direct-call shape): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengFitBuiltin_") != NULL);
    ASSERT(strstr(out.c_source, "__double") != NULL);
    ASSERT(strstr(out.c_source, "__head") != NULL);
    ASSERT(strstr(out.c_source, "witness->") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_builtin_array_open_generic_return_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.fit_builtin_generic_array;\n"
        "type Span<T> {\n"
        "    let origin: T[];\n"
        "    let start: long;\n"
        "    let end: long;\n"
        "    fn Span(origin: T[], start: long, end: long) {\n"
        "        self.origin = origin;\n"
        "        self.start = start;\n"
        "        self.end = end;\n"
        "    }\n"
        "    fn length(): long {\n"
        "        return self.end - self.start;\n"
        "    }\n"
        "    fn get(index: long): T {\n"
        "        return self.origin[self.start + index];\n"
        "    }\n"
        "}\n"
        "fit T[] {\n"
        "    fn slice(start: long, end: long): Span<T> {\n"
        "        return Span<T>(self, start, end);\n"
        "    }\n"
        "    fn slice(start: long): Span<T> {\n"
        "        return self.slice(start, (long)4);\n"
        "    }\n"
        "}\n"
        "fn run(): int {\n"
        "    let values: int[] = [1, 2, 3, 4];\n"
        "    let middle = values.slice((long)1, (long)3);\n"
        "    let tail = values.slice((long)1);\n"
        "    return middle.get((long)0) + (int)middle.length() + tail.get((long)0) + (int)tail.length();\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource,
        "tests/fit_builtin_generic_array_codegen.ff");
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
        fprintf(stderr, "codegen error (fit builtin generic array): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "Span__G__i32") != NULL);
    ASSERT(strstr(out.c_source, "Span__G__T") != NULL);
    ASSERT(strstr(out.c_source, "__slice__from__i64__i64") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_builtin_and_array_object_spec_coercion_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.fit_builtin_spec;\n"
        "spec Named { fn name(): string; }\n"
        "spec ScalarTwice { fn twice_only_scalar(): int; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "fit i32: Named {\n"
        "    fn name(): string {\n"
        "        return \"i32\";\n"
        "    }\n"
        "}\n"
        "fit i32: ScalarTwice {\n"
        "    fn twice_only_scalar(): int {\n"
        "        return self + self;\n"
        "    }\n"
        "}\n"
        "fit string: Named {\n"
        "    fn name(): string {\n"
        "        return self;\n"
        "    }\n"
        "}\n"
        "fit int[]: Named {\n"
        "    fn name(): string {\n"
        "        return \"arr\";\n"
        "    }\n"
        "}\n"
        "fn call_name(v: Named): string {\n"
        "    return v.name();\n"
        "}\n"
        "fn call_twice_direct(v: int): int {\n"
        "    return v.twice_only_scalar();\n"
        "}\n"
        "fn call_twice_spec(v: ScalarTwice): int {\n"
        "    return v.twice_only_scalar();\n"
        "}\n"
        "fn make_scalar_named(): Named {\n"
        "    return (8);\n"
        "}\n"
        "fn run(): int {\n"
        "    let xs: int[] = [1, 2];\n"
        "    let u: User = User{n: \"u\"};\n"
        "    let s1: Named = (7);\n"
        "    let s2: Named = xs;\n"
        "    let s4: Named = u;\n"
        "    let s5: Named = \"str\";\n"
        "    let s3: Named = make_scalar_named();\n"
        "    let a: string = s1.name();\n"
        "    let b: string = s2.name();\n"
        "    let h: string = s4.name();\n"
        "    let i: string = s5.name();\n"
        "    let e: string = s3.name();\n"
        "    let c: string = call_name(s1);\n"
        "    let d: string = call_name(s2);\n"
        "    let g: string = call_name((\"inline\"));\n"
        "    let f: string = call_name((9));\n"
        "    let t1: int = call_twice_direct(5);\n"
        "    let t2: int = call_twice_spec((5));\n"
        "    return t1 + t2;\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/fit_builtin_spec_codegen.ff");
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
        fprintf(stderr, "codegen error (builtin/array object spec coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengFitBuiltin_") != NULL);
    ASSERT(strstr(out.c_source, "__name") != NULL);
    ASSERT(count_substr(out.c_source, "struct FengScalarBox *_sb") == 2U);
    ASSERT(strstr(out.c_source, ".witness->name(") != NULL);
    ASSERT(strstr(out.c_source, "witness->witness->") == NULL);
    ASSERT(strstr(out.c_source, "static const FengTypeDescriptor feng_scalar_box_descriptor") == NULL);
    ASSERT(strstr(out.c_source, "struct FengScalarBox {") == NULL);
    ASSERT(count_substr(out.c_source, "FENG_SLOT_POINTER") >= 1U);
    ASSERT(count_substr(out.c_source, ".managed_slot_count = 1,") >= 1U);
    ASSERT(count_substr(out.c_source, "__twice_only_scalar(") >= 3U);
    ASSERT(strstr(out.c_source, "twice_only_scalar_box") == NULL);
    ASSERT(strstr(out.c_source, "subject_") != NULL);
    ASSERT(strstr(out.c_source, "_self_value = *(const int32_t *)_subject;") != NULL);
    ASSERT(strstr(out.c_source, "_self_value = ((const struct FengScalarBox *)_subject)->payload.i32;") != NULL);
    ASSERT(strstr(out.c_source, "__twice_only_scalar(((const struct FengScalarBox *)_subject)->payload.i32") == NULL);
    ASSERT(strstr(out.c_source, "__twice_only_scalar(*(const int32_t *)_subject") == NULL);
    ASSERT(strstr(out.c_source, "_self_ref = (FengArray *)_subject;") != NULL);
    ASSERT(strstr(out.c_source, "_self_ref = (FengString *)_subject;") != NULL);
    ASSERT(strstr(out.c_source, "__name((FengArray *)_subject") == NULL);
    ASSERT(strstr(out.c_source, "__name((FengString *)_subject") == NULL);
    assert_builtin_subject_thunks_direct_fit_call(out.c_source);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_enum_object_spec_coercion_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.fit_enum_spec;\n"
        "spec Named { fn code(): int; }\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status: Named {\n"
        "    fn code(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "}\n"
        "fn call_named(value: Named): int {\n"
        "    return value.code();\n"
        "}\n"
        "fn run(): int {\n"
        "    let first: Named = Status.Ok;\n"
        "    return first.code() + call_named(Status.Failed);\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/fit_enum_spec_codegen.ff");
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
        fprintf(stderr, "codegen error (enum object spec coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengFitBuiltin_") != NULL);
    ASSERT(strstr(out.c_source, "FengScalarBox") != NULL);
    ASSERT(strstr(out.c_source, "payload.i32") != NULL);
    ASSERT(strstr(out.c_source,
                  "_self_value = ((const struct FengScalarBox *)_subject)->payload.i32;") != NULL);
    ASSERT(strstr(out.c_source, "_self_value = *(const int32_t *)_subject;") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_object_spec_thunk_subject_cast_shape_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.object_spec_cast;\n"
        "spec Named { fn name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    fn name(): string { return self.n; }\n"
        "}\n"
        "fn call(v: Named): string {\n"
        "    return v.name();\n"
        "}\n"
        "fn run(): string {\n"
        "    let u: User = User{n: \"u\"};\n"
        "    return call(u);\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/object_spec_cast_codegen.ff");
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
        fprintf(stderr, "codegen error (object spec thunk cast shape): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecThunk__") != NULL);
    ASSERT(strstr(out.c_source, " *)_subject") != NULL);
    ASSERT(strstr(out.c_source, ".witness->name(") != NULL);
    ASSERT(strstr(out.c_source, "FengScalarBox") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* ---- G6 generic codegen tests ------------------------------------------ */

static const char *kGenericFnSrc =
    "mod feng.codegen.gf1;\n"
    "pu fn identity<T>(x: T): T { return x; }\n";

static void test_generic_fn_codegen(void) {
    FengProgram *program = parse_or_die(kGenericFnSrc, "gf1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 generic fn): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengGenericParamDescriptor") != NULL);
    ASSERT(strstr(out.c_source, "void *_out") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericTypeSrc =
    "mod feng.codegen.gf2;\n"
    "pu type Box<T> { pu let value: int; }\n";

static void test_generic_type_decl_no_crash(void) {
    FengProgram *program = parse_or_die(kGenericTypeSrc, "gf2.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 generic type): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericCallSrc =
    "mod feng.codegen.gf3;\n"
    "fn identity<T>(x: T): T { return x; }\n"
    "fn use_it() { let result = identity(42); }\n";

static void test_generic_fn_call_codegen(void) {
    FengProgram *program = parse_or_die(kGenericCallSrc, "gf3.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 generic call): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_VALUE_TRIVIAL") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericSpecArgSrc =
    "mod feng.codegen.gf4;\n"
    "spec Spec1 {}\n"
    "type User: Spec1 {}\n"
    "type MyType<T, V> {\n"
    "    let value: V;\n"
    "\n"
    "    fn test(t: T): V {\n"
    "        return self.value;\n"
    "    }\n"
    "}\n"
    "\n"
    "fn use_it() {\n"
    "    let s: Spec1 = User();\n"
    "    let x = MyType<Spec1, int>();\n"
    "    x.test(s);\n"
    "}\n";

static void test_generic_spec_arg_codegen(void) {
    FengProgram *program = parse_or_die(kGenericSpecArgSrc, "gf4.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 spec generic arg): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecAgg__feng__codegen__gf4__Spec1") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericConstraintWitnessSrc =
    "mod feng.codegen.gf5;\n"
    "spec Named {\n"
    "    var name: string;\n"
    "    fn greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    var name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "fn rename<T: Named>(user: T, next: string): string {\n"
    "    user.name = next;\n"
    "    return user.name;\n"
    "}\n"
    "fn greet_generic<T: Named>(user: T): string {\n"
    "    return user.greet();\n"
    "}\n";

static void test_generic_constraint_witness_codegen(void) {
    FengProgram *program = parse_or_die(kGenericConstraintWitnessSrc, "gf5.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 generic constraint witness): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "_T->witness") != NULL);
    ASSERT(strstr(out.c_source, "get_name") != NULL);
    ASSERT(strstr(out.c_source, "set_name") != NULL);
    ASSERT(strstr(out.c_source, "->greet(") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericRuntimeTypeKindSrc =
    "mod feng.codegen.gf9;\n"
    "spec Named {\n"
    "    fn name(): string;\n"
    "}\n"
    "spec Mapper(x: int): int;\n"
    "enum Status {\n"
    "    Ok = 200,\n"
    "    Failed = 500\n"
    "}\n"
    "type User: Named {\n"
    "    fn name(): string {\n"
    "        return \"user\";\n"
    "    }\n"
    "}\n"
    "fn add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "fn identity<T>(value: T): T {\n"
    "    return value;\n"
    "}\n"
    "fn use_it(values: int[]): int {\n"
    "    let ok = identity(true);\n"
    "    let text = identity(\"hi\");\n"
    "    let arr = identity(values);\n"
    "    let user = identity(User());\n"
    "    let named: Named = User();\n"
    "    let spec_value = identity(named);\n"
    "    let mapper: Mapper = add1;\n"
    "    let callable = identity(mapper);\n"
    "    let current: Status = Status.Ok;\n"
    "    let ptr: Status* = &current;\n"
    "    let handle = identity(ptr);\n"
    "    let status = identity(Status.Failed);\n"
    "    if ok {\n"
    "        return 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

static void test_generic_runtime_type_kind_codegen(void) {
    FengProgram *program = parse_or_die(kGenericRuntimeTypeKindSrc, "gf9.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    "<unknown>",
                    errors[i].token.line,
                    errors[i].token.column,
                    errors[i].message ? errors[i].message : "(unknown)");
        }
    }
    ASSERT(ok);
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic runtime type kind): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_BOOL") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_ENUM") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .type_kind = FENG_RUNTIME_TYPE_STRING") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .type_kind = FENG_RUNTIME_TYPE_ARRAY") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .type_kind = FENG_RUNTIME_TYPE_OBJECT") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_POINTER") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, .type_kind = FENG_RUNTIME_TYPE_SPEC") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .type_kind = FENG_RUNTIME_TYPE_CALLABLE") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericAggregateFactsShapeSrc =
    "mod feng.codegen.gfaggshape;\n"
    "spec Named {\n"
    "    fn name(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    fn name(): string {\n"
    "        return \"user\";\n"
    "    }\n"
    "}\n"
    "fn identity<T>(value: T): T {\n"
    "    return value;\n"
    "}\n"
    "fn use_it(value: int, text: string): int {\n"
    "    let n = identity(value);\n"
    "    let s = identity(text);\n"
    "    let named: Named = User();\n"
    "    let agg = identity(named);\n"
    "    return n;\n"
    "}\n";

static void test_generic_aggregate_facts_shape_codegen(void) {
    FengProgram *program = parse_or_die(kGenericAggregateFactsShapeSrc, "gfaggshape.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic aggregate facts shape): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  ".size = sizeof(int32_t), .kind = FENG_VALUE_TRIVIAL, .type_kind = FENG_RUNTIME_TYPE_I32, .aggregate = NULL") != NULL);
    ASSERT(strstr(out.c_source,
                  ".size = sizeof(void *), .kind = FENG_VALUE_MANAGED_POINTER, .type_kind = FENG_RUNTIME_TYPE_STRING, .aggregate = NULL") != NULL);
    ASSERT(strstr(out.c_source,
                  ".size = sizeof(struct FengSpecValue__feng__codegen__gfaggshape__Named), .kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, .type_kind = FENG_RUNTIME_TYPE_SPEC, .aggregate = &FengSpecAgg__feng__codegen__gfaggshape__Named") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_enum_generic_constraint_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.fit_enum_generic;\n"
        "spec Hashable<T> {\n"
        "    fn hash(): int;\n"
        "    fn same(other: T): bool;\n"
        "}\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status: Hashable<Status> {\n"
        "    fn hash(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "    fn same(other: Status): bool {\n"
        "        return self == other;\n"
        "    }\n"
        "}\n"
        "fn use_hash<K: Hashable<K>>(value: K): int {\n"
        "    return value.hash();\n"
        "}\n"
        "fn use_same<K: Hashable<K>>(left: K, right: K): bool {\n"
        "    return left.same(right);\n"
        "}\n"
        "fn run(): int {\n"
        "    let _same: bool = use_same(Status.Failed, Status.Ok);\n"
        "    return use_hash(Status.Failed);\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/fit_enum_generic_codegen.ff");
    const FengProgram *programs[1] = { program };

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (enum generic constraint witness): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "_K->witness") != NULL);
    ASSERT(strstr(out.c_source, "_self_value = *(const int32_t *)_subject;") != NULL);
    ASSERT(strstr(out.c_source, "FengScalarBox") == NULL);
    ASSERT(strstr(out.c_source, "payload.i32") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecTopLevelFnSrc =
    "mod feng.codegen.cb1;\n"
    "spec Mapper(x: int): int;\n"
    "fn add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let mapper: Mapper = add1;\n"
    "    return mapper(41);\n"
    "}\n";

static void test_callable_spec_top_level_fn_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecTopLevelFnSrc, "cb1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec top-level fn): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_TYPE_TAG_CLOSURE") != NULL);
    ASSERT(strstr(out.c_source, "FengClosure__feng__codegen__cb1__Mapper") != NULL);
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericCallableConstraintSrc =
    "mod feng.codegen.cb2;\n"
    "spec Mapper(x: int): int;\n"
    "fn add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "fn apply<T: Mapper>(mapper: T, value: int): int {\n"
    "    return mapper(value);\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let mapper: Mapper = add1;\n"
    "    return apply<Mapper>(mapper, 41);\n"
    "}\n";

static void test_generic_callable_constraint_codegen(void) {
    FengProgram *program = parse_or_die(kGenericCallableConstraintSrc, "cb2.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic callable constraint): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "_T->witness") != NULL);
    ASSERT(strstr(out.c_source, ")->invoke(") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecSlotWitness__") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericObjectSpecInstanceSrc =
    "mod feng.codegen.gs1;\n"
    "spec Box<T> {\n"
    "    fn fetch(): T;\n"
    "}\n"
    "fn read_it(box: Box<int>): int {\n"
    "    return box.fetch();\n"
    "}\n";

static void test_generic_object_spec_instance_codegen(void) {
    FengProgram *program = parse_or_die(kGenericObjectSpecInstanceSrc, "gs1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic object spec instance): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecValue__feng__codegen__gs1__Box__G__int") != NULL);
    ASSERT(strstr(out.c_source, "->fetch(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericCallableSpecInstanceSrc =
    "mod feng.codegen.gs2;\n"
    "spec Mapper<T>(x: T): T;\n"
    "fn apply(mapper: Mapper<int>): int {\n"
    "    return mapper(41);\n"
    "}\n";

static void test_generic_callable_spec_instance_codegen(void) {
    FengProgram *program = parse_or_die(kGenericCallableSpecInstanceSrc, "gs2.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic callable spec instance): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengClosure__feng__codegen__gs2__Mapper__G__int") != NULL);
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericObjectSpecCoercionSrc =
    "mod feng.codegen.gs3;\n"
    "spec Box<T> {\n"
    "    fn fetch(): T;\n"
    "}\n"
    "type IntBox: Box<int> {\n"
    "    fn fetch(): int {\n"
    "        return 7;\n"
    "    }\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let box: Box<int> = IntBox();\n"
    "    return box.fetch();\n"
    "}\n";

static void test_generic_object_spec_coercion_codegen(void) {
    FengProgram *program = parse_or_die(kGenericObjectSpecCoercionSrc, "gs3.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic object spec coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecValue__feng__codegen__gs3__Box__G__int") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengWitness__feng__codegen__gs3__IntBox__as__feng__codegen__gs3__Box_int_") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericCallableSpecCoercionSrc =
    "mod feng.codegen.gs4;\n"
    "spec Mapper<T>(x: T): T;\n"
    "fn add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let mapper: Mapper<int> = add1;\n"
    "    return mapper(41);\n"
    "}\n";

static void test_generic_callable_spec_coercion_codegen(void) {
    FengProgram *program = parse_or_die(kGenericCallableSpecCoercionSrc, "gs4.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic callable spec coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengClosure__feng__codegen__gs4__Mapper__G__int") != NULL);
    ASSERT(strstr(out.c_source, "FengCallableValue__FengClosure__feng__codegen__gs4__Mapper__G__int") != NULL);
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecMethodCoercionSrc =
    "mod feng.codegen.gs5;\n"
    "spec Mapper<T>(x: T): T;\n"
    "type Adder {\n"
    "    fn add1(x: int): int {\n"
    "        return x + 1;\n"
    "    }\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let mapper: Mapper<int> = Adder().add1;\n"
    "    return mapper(41);\n"
    "}\n";

static void test_callable_spec_method_coercion_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecMethodCoercionSrc, "gs5.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec method coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "FengCallableBind__FengClosure__feng__codegen__gs5__Mapper__G__int") != NULL);
    ASSERT(strstr(out.c_source, "feng_object_new(&FengClosureDesc__feng__codegen__gs5__Mapper__G__int)") != NULL);
    ASSERT(strstr(out.c_source, "feng_assign(&_o->_self, (void *)_self)") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecLambdaLocalCaptureSrc =
    "mod feng.codegen.gs6;\n"
    "spec Mapper(x: int): int;\n"
    "fn use_it(): int {\n"
    "    var base: int = 1;\n"
    "    let mapper: Mapper = (x: int) -> x + base;\n"
    "    base = 2;\n"
    "    return mapper(40);\n"
    "}\n";

static void test_callable_spec_lambda_local_capture_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecLambdaLocalCaptureSrc, "gs6.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec lambda local capture): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengLambda__feng__codegen__gs6") != NULL);
    ASSERT(strstr(out.c_source, "FengCaptureCell__feng__codegen__gs6") != NULL);
    ASSERT(strstr(out.c_source, "feng_assign((void **)&_lambda") != NULL);
    ASSERT(strstr(out.c_source, "->value") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecLambdaSelfCaptureSrc =
    "mod feng.codegen.gs7;\n"
    "spec Reader(): int;\n"
    "type Box {\n"
    "    var n: int;\n"
    "    fn read(): int {\n"
    "        let reader: Reader = () -> self.n;\n"
    "        return reader();\n"
    "    }\n"
    "}\n"
    "fn use_it(): int {\n"
    "    return Box().read();\n"
    "}\n";

static void test_callable_spec_lambda_self_capture_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecLambdaSelfCaptureSrc, "gs7.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec lambda self capture): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengLambda__feng__codegen__gs7") != NULL);
    ASSERT(strstr(out.c_source, "FengCaptureCell__feng__codegen__gs7") != NULL);
    ASSERT(strstr(out.c_source, "->n") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecLambdaArgumentSrc =
    "mod feng.codegen.gs8;\n"
    "spec Mapper(x: int): int;\n"
    "fn apply(mapper: Mapper): int {\n"
    "    return mapper(41);\n"
    "}\n"
    "fn use_it(): int {\n"
    "    return apply((x: int) -> x + 1);\n"
    "}\n";

static void test_callable_spec_lambda_argument_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecLambdaArgumentSrc, "gs8.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec lambda argument): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengLambda__feng__codegen__gs8") != NULL);
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecOtherCoercionSrc =
    "mod feng.codegen.gs8;\n"
    "spec MapperA(x: int): int;\n"
    "spec MapperB(x: int): int;\n"
    "fn add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "fn use_it(input: MapperA): int {\n"
    "    let local: MapperA = input;\n"
    "    let remapped: MapperB = (MapperB)local;\n"
    "    return remapped(41);\n"
    "}\n"
    "fn entry(): int {\n"
    "    let start: MapperA = add1;\n"
    "    return use_it(start);\n"
    "}\n";

static void test_callable_spec_other_coercion_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecOtherCoercionSrc, "gs8.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec OTHER coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengCallableRewrap__") == NULL);
    ASSERT(strstr(out.c_source, "FengClosure__feng__codegen__gs8__MapperB") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecOtherFieldReadCoercionSrc =
    "mod feng.codegen.gs9;\n"
    "spec MapperA(x: int): int;\n"
    "spec MapperB(x: int): int;\n"
    "fn add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "type Holder {\n"
    "    let mapper: MapperA;\n"
    "}\n"
    "fn use_it(input: MapperA): int {\n"
    "    let local: MapperA = input;\n"
    "    let holder: Holder = Holder{mapper: local};\n"
    "    let from_field: MapperA = holder.mapper;\n"
    "    let remapped: MapperB = (MapperB)from_field;\n"
    "    return remapped(41);\n"
    "}\n"
    "fn entry(): int {\n"
    "    let start: MapperA = add1;\n"
    "    return use_it(start);\n"
    "}\n";

static void test_callable_spec_other_field_read_coercion_codegen(void) {
    FengProgram *program = parse_or_die(kCallableSpecOtherFieldReadCoercionSrc, "gs9.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (callable spec OTHER field-read coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengCallableRewrap__") == NULL);
    ASSERT(strstr(out.c_source, "FengClosure__feng__codegen__gs9__MapperB") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericConstrainedSpecValueSrc =
    "mod feng.codegen.gf7;\n"
    "spec Named {\n"
    "    var name: string;\n"
    "    fn greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    var name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "fn rename<T: Named>(user: T, next: string): string {\n"
    "    user.name = next;\n"
    "    return user.greet();\n"
    "}\n"
    "fn use_it(): string {\n"
    "    let named: Named = User{name: \"before\"};\n"
    "    return rename<Named>(named, \"after\");\n"
    "}\n";

static void test_generic_constrained_spec_value_codegen(void) {
    FengProgram *program = parse_or_die(kGenericConstrainedSpecValueSrc, "gf7.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 constrained spec generic arg): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecSlotWitness__") != NULL);
    ASSERT(strstr(out.c_source, "_value->witness->get_name") != NULL);
    ASSERT(strstr(out.c_source, "_value->witness->greet") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kSpecAggregateFieldSrc =
    "mod feng.codegen.sfagg1;\n"
    "spec Named {\n"
    "    fn greet(): string;\n"
    "}\n"
    "spec HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "type Holder: HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "fn read_child(box: HasChild): string {\n"
    "    return box.child.greet();\n"
    "}\n"
    "fn write_child(box: HasChild, child: Named) {\n"
    "    box.child = child;\n"
    "}\n"
    "fn use_it(): string {\n"
    "    let holder: Holder = Holder{child: User{name: \"before\"}};\n"
    "    let box: HasChild = holder;\n"
    "    write_child(box, User{name: \"after\"});\n"
    "    return read_child(box);\n"
    "}\n";

static void test_spec_aggregate_field_codegen(void) {
    FengProgram *program = parse_or_die(kSpecAggregateFieldSrc, "sfagg1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (spec aggregate field): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "__get_child") != NULL);
    ASSERT(strstr(out.c_source, "__set_child") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_assign(&((struct") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericConstrainedAggregateSpecValueSrc =
    "mod feng.codegen.gf9;\n"
    "spec Named {\n"
    "    fn greet(): string;\n"
    "}\n"
    "spec HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "type Holder: HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "fn rewrite<T: HasChild>(box: T, next: Named): string {\n"
    "    box.child = next;\n"
    "    return box.child.greet();\n"
    "}\n"
    "fn use_it(): string {\n"
    "    let holder: Holder = Holder{child: User{name: \"before\"}};\n"
    "    let box: HasChild = holder;\n"
    "    return rewrite<HasChild>(box, User{name: \"after\"});\n"
    "}\n";

static void test_generic_constrained_aggregate_spec_value_codegen(void) {
    FengProgram *program = parse_or_die(kGenericConstrainedAggregateSpecValueSrc, "gf9.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic constrained aggregate spec value): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecSlotWitness__") != NULL);
    ASSERT(strstr(out.c_source, "_value->witness->get_child") != NULL);
    ASSERT(strstr(out.c_source, "_value->witness->set_child") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kIfExprAggregateResultSrc =
    "mod feng.codegen.ifagg1;\n"
    "spec Named {\n"
    "    fn greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "fn pick(flag: bool, left: Named, right: Named): Named {\n"
    "    return if flag {\n"
    "        left;\n"
    "    } else {\n"
    "        right;\n"
    "    };\n"
    "}\n"
    "fn use_it(): string {\n"
    "    let left: Named = User{name: \"L\"};\n"
    "    let right: Named = User{name: \"R\"};\n"
    "    let selected = pick(true, left, right);\n"
    "    return selected.greet();\n"
    "}\n";

static void test_if_expr_aggregate_result_codegen(void) {
    FengProgram *program = parse_or_die(kIfExprAggregateResultSrc, "ifagg1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (if-expression aggregate result): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_default_init(&_ifv") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_assign(&_ifv") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kMatchExprAggregateResultSrc =
    "mod feng.codegen.matchagg1;\n"
    "spec Named {\n"
    "    fn greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "fn pick(tag: i32, left: Named, right: Named): Named {\n"
    "    return if tag {\n"
    "        0 { left; }\n"
    "        else { right; }\n"
    "    };\n"
    "}\n"
    "fn use_it(): string {\n"
    "    let left: Named = User{name: \"L\"};\n"
    "    let right: Named = User{name: \"R\"};\n"
    "    let selected = pick(1, left, right);\n"
    "    return selected.greet();\n"
    "}\n";

static void test_match_expr_aggregate_result_codegen(void) {
    FengProgram *program = parse_or_die(kMatchExprAggregateResultSrc, "matchagg1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (match-expression aggregate result): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_default_init(&_ifv") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_assign(&_ifv") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kIfMatchStatementCodegenSrc =
    "mod feng.codegen.matchstmt1;\n"
    "fn classify(age: i32, label: string): i32 {\n"
    "    var result = 0;\n"
    "    if age {\n"
    "        0 { result = 10; }\n"
    "        1...3 { result = 20; }\n"
    "        4, 5 { result = 30; }\n"
    "        else { result = 40; }\n"
    "    }\n"
    "    if label {\n"
    "        \"ok\" { result = result + 1; }\n"
    "        else { result = result + 2; }\n"
    "    }\n"
    "    return result;\n"
    "}\n";

static void test_if_match_statement_codegen(void) {
    FengProgram *program = parse_or_die(kIfMatchStatementCodegenSrc, "matchstmt1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (if-match statement): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "memcmp(feng_string_data(_mt") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericAggregateReturnSrc =
    "mod feng.codegen.gf6;\n"
    "spec Named {\n"
    "    fn greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "fn make_named<T>(name: string): Named {\n"
    "    let user: User = User{name: name};\n"
    "    return user;\n"
    "}\n"
    "fn forward_named<T>(named: Named): Named {\n"
    "    return named;\n"
    "}\n"
    "fn rebound_named<T>(name: string): Named {\n"
    "    return make_named<T>(name);\n"
    "}\n";

static void test_generic_aggregate_return_codegen(void) {
    FengProgram *program = parse_or_die(kGenericAggregateReturnSrc, "gf6.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 generic aggregate return): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_take(&") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_retain(&") != NULL);
    ASSERT(strstr(out.c_source, "memcpy(_out, &") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericTypeGenericMethodSrc =
    "mod feng.codegen.gf8;\n"
    "type Box<T> {\n"
    "    var value: T;\n"
    "    fn echo<U>(value: U): U {\n"
    "        return value;\n"
    "    }\n"
    "    fn replace<U>(next: T, result: U): U {\n"
    "        self.value = next;\n"
    "        return result;\n"
    "    }\n"
    "    fn current(): T {\n"
    "        return self.value;\n"
    "    }\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let box = Box<int>();\n"
    "    let first: int = box.echo(20);\n"
    "    let second: int = box.replace<int>(22, first);\n"
    "    return box.current() + second;\n"
    "}\n";

static void test_generic_type_generic_method_codegen(void) {
    FengProgram *program = parse_or_die(kGenericTypeGenericMethodSrc, "gf8.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (G6 generic type generic method): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengGenericMethod__feng__codegen__gf8__Box") != NULL);
    ASSERT(strstr(out.c_source, "const FengGenericParamDescriptor *_U") != NULL);
    ASSERT(strstr(out.c_source, "void *_out") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericScalarInstanceDirectCallSrc =
    "mod feng.codegen.gd13;\n"
    "type Set<T> {\n"
    "    var value: T;\n"
    "    fn put(next: T) {\n"
    "        self.value = next;\n"
    "    }\n"
    "    fn get(): T {\n"
    "        return self.value;\n"
    "    }\n"
    "}\n"
    "fn use_it(): int {\n"
    "    let set: Set<int> = Set<int>();\n"
    "    set.put(7);\n"
    "    return set.get();\n"
    "}\n";

static void test_generic_scalar_instance_direct_call_codegen(void) {
    FengProgram *program = parse_or_die(kGenericScalarInstanceDirectCallSrc, "gd13.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (D13 generic scalar instance direct-call): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengGenericMethod__feng__codegen__gd13__Set__i0__put") != NULL);
    ASSERT(strstr(out.c_source, "const void *_p_next") != NULL);
    ASSERT(strstr(out.c_source, "Feng__feng__codegen__gd13__Set__G__int__put__from__i32") != NULL);
    ASSERT(strstr(out.c_source, "Feng__feng__codegen__gd13__Set__G__int__put__from__i32(_l_set_0") != NULL);
    ASSERT(strstr(out.c_source, "Feng__feng__codegen__gd13__Set__G__int__get__from__void(_l_set_0") != NULL);
    ASSERT(strstr(out.c_source, "feng_scalar_box_new_") == NULL);
    ASSERT(strstr(out.c_source, "struct FengScalarBox *_sb") == NULL);
    ASSERT(strstr(out.c_source, "FengSpecThunk__") == NULL);
    ASSERT(strstr(out.c_source, "FengSpecSlotWitness__") == NULL);
    ASSERT(strstr(out.c_source, "witness->") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kPhaseEAggregateGenericArgThreeEntrancesSrc =
    "mod feng.codegen.ge1;\n"
    "spec Named {\n"
    "    fn greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    fn greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "fn idNamed<T: Named>(value: T): T {\n"
    "    return value;\n"
    "}\n"
    "type Holder<T: Named> {\n"
    "    var value: T;\n"
    "    fn set(next: T) {\n"
    "        self.value = next;\n"
    "    }\n"
    "    fn read(): string {\n"
    "        return self.value.greet();\n"
    "    }\n"
    "    fn relay<U: Named>(item: U): string {\n"
    "        return item.greet();\n"
    "    }\n"
    "}\n"
    "fn use_it(input: Named): string {\n"
    "    let holder: Holder<Named> = Holder<Named>();\n"
    "    let fromFn: Named = idNamed<Named>(input);\n"
    "    holder.set(fromFn);\n"
    "    let fromMethod: string = holder.relay<Named>(fromFn);\n"
    "    holder.read();\n"
    "    return fromMethod;\n"
    "}\n";

static void test_phase_e_aggregate_generic_arg_three_entrances_codegen(void) {
    FengProgram *program = parse_or_die(kPhaseEAggregateGenericArgThreeEntrancesSrc, "ge1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (Phase E aggregate generic arg three entrances): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecAgg__feng__codegen__ge1__Named") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecSlotWitness__") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kUserConstructorFormsSrc =
    "mod feng.codegen.ctor1;\n"
    "type UserType {\n"
    "    var id: int;\n"
    "    let name: string;\n"
    "    fn UserType() {\n"
    "        self.id = 1;\n"
    "    }\n"
    "    fn UserType(next: int) {\n"
    "        self.id = next;\n"
    "    }\n"
    "}\n"
    "fn use_all(): int {\n"
    "    let a: UserType = UserType() { id: 11 };\n"
    "    let b: UserType = UserType { id: 12 };\n"
    "    let c: UserType = UserType();\n"
    "    let d: UserType = UserType(7) { id: 13 };\n"
    "    let e: UserType = UserType(9);\n"
    "    return a.id + b.id + c.id + d.id + e.id;\n"
    "}\n";

static void test_empty_array_literal_codegen_uses_target_contexts(void) {
    static const char *kSource =
        "mod feng.codegen.emptyarray;\n"
        "type InitializedHolder {\n"
        "    let values: int[] = [];\n"
        "}\n"
        "type LiteralHolder {\n"
        "    var values: int[];\n"
        "}\n"
        "fn take(values: int[]): int {\n"
        "    return 1;\n"
        "}\n"
        "fn generic_take<T>(values: T[]): int {\n"
        "    return 1;\n"
        "}\n"
        "fn run(): int {\n"
        "    let local: int[] = [];\n"
        "    let initialized = InitializedHolder();\n"
        "    let literal = LiteralHolder { values: [] };\n"
        "    return take([]) + take(local) + take(initialized.values) +\n"
        "        take(literal.values) + generic_take<int>([]);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/empty_array_literal_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (empty array literal target contexts): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng_array_new(NULL, sizeof(int32_t), false, (size_t)0)") >= 4U);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_user_constructor_forms_codegen(void) {
    FengProgram *program = parse_or_die(kUserConstructorFormsSrc, "ctor1.ff");
    const FengProgram *programs[1] = {program};

    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (constructor forms): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "__ctor__UserType") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_type_field_initializers_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.fieldinit;\n"
        "type UserType {\n"
        "    let id: int = 7;\n"
        "}\n"
        "type User {\n"
        "    let id: int = 0;\n"
        "    let x: UserType;\n"
        "    let y = UserType();\n"
        "    let z = UserType {};\n"
        "}\n"
        "fn total(): int {\n"
        "    let user = User();\n"
        "    return user.id + user.x.id + user.y.id + user.z.id;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/type_field_initializers.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs,
                                            1U,
                                            FENG_COMPILE_TARGET_LIB,
                                            &analysis,
                                            &errors,
                                            &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path,
                        errors[i].token.line,
                        errors[i].token.column,
                        errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (type field initializers): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "struct Feng__feng__codegen__fieldinit__User *") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__fieldinit__UserType__default_zero()") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_type_field_callable_lambda_initializer_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.fieldlambda;\n"
        "spec Reader(): int;\n"
        "type Box {\n"
        "    var n: int;\n"
        "    let read: Reader = () -> self.n;\n"
        "}\n"
        "fn use_it(): int {\n"
        "    let box = Box{n: 7};\n"
        "    return box.read();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/type_field_callable_lambda_initializer.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs,
                                            1U,
                                            FENG_COMPILE_TARGET_LIB,
                                            &analysis,
                                            &errors,
                                            &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path,
                        errors[i].token.line,
                        errors[i].token.column,
                        errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (type field callable lambda initializer): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengLambda__feng__codegen__fieldlambda") != NULL);
    ASSERT(strstr(out.c_source, "FengCaptureCell__feng__codegen__fieldlambda") != NULL);
    ASSERT(strstr(out.c_source, "->read") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_void_try_expression_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.tryvoid;\n"
        "fn fail() {\n"
        "    throw \"boom\";\n"
        "}\n"
        "fn noop() {\n"
        "}\n"
        "fn run_case() {\n"
        "    try noop();\n"
        "    try fail() catch ex: string {\n"
        "        noop();\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/tryvoid.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs,
                                            1U,
                                            FENG_COMPILE_TARGET_LIB,
                                            &analysis,
                                            &errors,
                                            &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path,
                        errors[i].token.line,
                        errors[i].token.column,
                        errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (void try expression): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "void _tryv") == NULL);
    ASSERT(strstr(out.c_source, "tryvoid__noop__from__void());") != NULL);
    ASSERT(strstr(out.c_source, "FengExceptionFrame") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_try_catch_return_codegen(void) {
    static const char *kSource =
        "mod feng.codegen.tryreturn;\n"
        "fn fail(): string {\n"
        "    throw \"boom\";\n"
        "    return \"unreachable\";\n"
        "}\n"
        "fn catch_returns(): string {\n"
        "    let value = try fail() catch ex: string {\n"
        "        return \"catch-return\";\n"
        "    };\n"
        "    return value;\n"
        "}\n"
        "fn normal_value(): string {\n"
        "    let value = try \"normal\" catch ex: string {\n"
        "        return \"caught\";\n"
        "    };\n"
        "    return value;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/tryreturn.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs,
                                            1U,
                                            FENG_COMPILE_TARGET_LIB,
                                            &analysis,
                                            &errors,
                                            &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path,
                        errors[i].token.line,
                        errors[i].token.column,
                        errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (try/catch return): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_exception_pop();") != NULL);
    ASSERT(strstr(out.c_source, "feng_release_unwind_exception();") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

int main(void) {
    test_multi_file_bin();
    test_multi_file_lib();
    test_module_binding_lazy_ensure_init_codegen();
    test_address_of_module_binding_uses_storage_slot_codegen();
    test_module_scalar_var_assignment_marks_initialized_codegen();
    test_module_managed_var_assignment_marks_initialized_codegen();
    test_module_binding_default_zero_ensure_init_codegen();
    test_extern_calling_convention_codegen();
    test_address_of_scalar_and_array_codegen();
    test_abi_function_pointer_codegen();
    test_abi_value_pointer_codegen();
    test_abi_array_pointee_codegen();
    test_fieldless_abi_pointer_codegen();
    test_fieldless_abi_function_surface_codegen();
    test_abi_value_extern_codegen();
    test_runtime_extern_codegen_uses_feng_surface_types();
    test_generic_runtime_extern_call_infers_type_args();
    test_generic_runtime_extern_call_accepts_explicit_type_args();
    test_generic_runtime_extern_expression_equal_codegen();
    test_generic_runtime_extern_direct_type_param_return_codegen();
    test_runtime_extern_codegen_rejects_non_contract_symbol();
    test_unsupported_pointer_pointee_reports_explicit_error();
    test_abi_value_function_pointer_codegen();
    test_lib_public_functions_are_exported();
    test_bin_public_functions_remain_static();
    test_imported_feng_function_prototypes_compile();
    test_imported_alias_qualified_type_annotations_codegen_compile();
    test_imported_full_path_type_annotations_codegen_compile_without_use();
    test_imported_public_let_binding_codegen_compiles();
    test_imported_public_var_binding_read_write_codegen_compiles();
    test_imported_public_binding_address_of_codegen_compiles();
    test_public_binding_lib_exports_slot_and_ensure_init_codegen();
    test_public_binding_infers_constructor_type_codegen();
    test_imported_public_binding_inferred_type_codegen_compiles();
    test_enum_codegen_emits_stable_symbols();
    test_imported_enum_codegen_emits_visible_symbols();
    test_same_named_types_in_distinct_modules();
    test_float_modulo_codegen_uses_math_runtime();
    test_fit_builtin_direct_call_codegen_shape();
    test_fit_builtin_array_open_generic_return_codegen();
    test_fit_builtin_and_array_object_spec_coercion_codegen();
    test_fit_enum_object_spec_coercion_codegen();
    test_object_spec_thunk_subject_cast_shape_codegen();
    test_generic_fn_codegen();
    test_generic_type_decl_no_crash();
    test_generic_fn_call_codegen();
    test_generic_spec_arg_codegen();
    test_callable_spec_top_level_fn_codegen();
    test_generic_callable_constraint_codegen();
    test_generic_object_spec_instance_codegen();
    test_generic_callable_spec_instance_codegen();
    test_generic_object_spec_coercion_codegen();
    test_generic_callable_spec_coercion_codegen();
    test_callable_spec_method_coercion_codegen();
    test_callable_spec_lambda_local_capture_codegen();
    test_callable_spec_lambda_self_capture_codegen();
    test_callable_spec_lambda_argument_codegen();
    test_callable_spec_other_coercion_codegen();
    test_callable_spec_other_field_read_coercion_codegen();
    test_generic_constraint_witness_codegen();
    test_generic_runtime_type_kind_codegen();
    test_generic_aggregate_facts_shape_codegen();
    test_fit_enum_generic_constraint_codegen();
    test_generic_constrained_spec_value_codegen();
    test_spec_aggregate_field_codegen();
    test_generic_constrained_aggregate_spec_value_codegen();
    test_if_expr_aggregate_result_codegen();
    test_match_expr_aggregate_result_codegen();
    test_if_match_statement_codegen();
    test_generic_aggregate_return_codegen();
    test_generic_type_generic_method_codegen();
    test_generic_scalar_instance_direct_call_codegen();
    test_phase_e_aggregate_generic_arg_three_entrances_codegen();
    test_type_field_initializers_codegen();
    test_type_field_callable_lambda_initializer_codegen();
    test_void_try_expression_codegen();
    test_try_catch_return_codegen();
    test_empty_array_literal_codegen_uses_target_contexts();
    test_user_constructor_forms_codegen();
    fprintf(stdout, "codegen tests passed\n");
    return 0;
}
