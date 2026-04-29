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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    test_multi_file_bin();
    test_multi_file_lib();
    fprintf(stdout, "codegen tests passed\n");
    return 0;
}
