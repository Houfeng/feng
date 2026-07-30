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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ASSERT(expr)                                                                  \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static bool test_semantic_analyze(const FengProgram *const *programs,
                                  size_t program_count,
                                  FengCompileTarget target,
                                  FengSemanticAnalysis **out_analysis,
                                  FengSemanticError **out_errors,
                                  size_t *out_error_count) {
    FengSemanticAnalyzeOptions options;
    memset(&options, 0, sizeof(options));
    options.target = target;
    options.pointer_size = feng_get_host_pointer_size();
    return feng_semantic_analyze_with_options(programs, program_count,
                                              &options, out_analysis,
                                              out_errors, out_error_count);
}
#define feng_semantic_analyze test_semantic_analyze

static const char *kSourceA =
    "module feng.codegen.mfa;\n"
    "\n"
    "@cdecl(\"libc\")\n"
    "extern func c_puts(msg: string*): int;\n"
    "\n"
    "func helper(): int {\n"
    "    return 42;\n"
    "}\n";

static const char *kSourceB =
    "module feng.codegen.mfb;\n"
    "\n"
    "@cdecl(\"libc\")\n"
    "extern func c_puts(msg: string*): int;\n"
    "\n"
    "func main(args: string[]) {\n"
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
    char *template_path = strdup("temp/feng_codegen_imported_XXXXXX");
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

/* Read one complete text file into a NUL-terminated buffer. */
static char *read_text_file_or_die(const char *path) {
    FILE *file = fopen(path, "rb");
    long length;
    char *text;

    ASSERT(file != NULL);
    ASSERT(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    ASSERT(length >= 0L);
    ASSERT(fseek(file, 0L, SEEK_SET) == 0);
    text = (char *)malloc((size_t)length + 1U);
    ASSERT(text != NULL);
    ASSERT(fread(text, 1U, (size_t)length, file) == (size_t)length);
    text[length] = '\0';
    ASSERT(fclose(file) == 0);
    return text;
}

/* Return true when nm output contains one exact undefined-symbol name. */
static bool nm_output_contains_symbol(const char *text, const char *symbol) {
    size_t symbol_length = strlen(symbol);
    const char *cursor = text;

    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *start = cursor;
        const char *end = line_end != NULL ? line_end : cursor + strlen(cursor);

        while (start < end && isspace((unsigned char)*start)) {
            ++start;
        }
        if (start < end && *start == 'U') {
            ++start;
            while (start < end && isspace((unsigned char)*start)) {
                ++start;
            }
        }
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        if ((size_t)(end - start) == symbol_length &&
            memcmp(start, symbol, symbol_length) == 0) {
            return true;
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }
    return false;
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
                    "cc -Isrc -Isrc/runtime -Ithird_party/miniz -std=gnu11 -fexceptions -Werror -c '%s' -o '%s' >/dev/null 2>&1",
                    c_path,
                    o_path) > 0);
    if (system(command) != 0) {
        fprintf(stderr, "generated C failed to compile: %s\n", command);
        ASSERT(false);
    }
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

/* Compile generated C for one object format and verify its native relocation. */
static void assert_generated_native_symbol_relocation(const char *c_source,
                                                      bool target_elf,
                                                      const char *native_symbol) {
    char *tmp_dir = make_temp_dir();
    char c_path[1024];
    char o_path[1024];
    char nm_path[1024];
    char command[4096];
    char *nm_output;
    char expected[256];
    char unexpected[256];

    ASSERT(snprintf(c_path, sizeof(c_path), "%s/generated.c", tmp_dir) > 0);
    ASSERT(snprintf(o_path, sizeof(o_path), "%s/generated.o", tmp_dir) > 0);
    ASSERT(snprintf(nm_path, sizeof(nm_path), "%s/nm.txt", tmp_dir) > 0);
    write_text_file_or_die(c_path, c_source);
    if (target_elf) {
        ASSERT(snprintf(command,
                        sizeof(command),
                        "build/toolchain/llvm/bin/clang --target=x86_64-unknown-linux-gnu "
                        "--sysroot=build/toolchain/sysroot/linux-x64-gnu "
                        "-Isrc -Isrc/runtime -std=gnu11 -fexceptions -Werror "
                        "-c '%s' -o '%s' >/dev/null 2>&1",
                        c_path,
                        o_path) > 0);
    } else {
        ASSERT(snprintf(command,
                        sizeof(command),
                        "cc -Isrc -Isrc/runtime -std=gnu11 -fexceptions -Werror "
                        "-c '%s' -o '%s' >/dev/null 2>&1",
                        c_path,
                        o_path) > 0);
    }
    ASSERT(system(command) == 0);
    ASSERT(snprintf(command,
                    sizeof(command),
                    "if command -v llvm-nm >/dev/null 2>&1; then "
                    "llvm-nm -u '%s' > '%s'; else nm -u '%s' > '%s'; fi",
                    o_path,
                    nm_path,
                    o_path,
                    nm_path) > 0);
    ASSERT(system(command) == 0);
    nm_output = read_text_file_or_die(nm_path);
    ASSERT(snprintf(expected,
                    sizeof(expected),
                    target_elf ? "%s" : "_%s",
                    native_symbol) > 0);
    ASSERT(nm_output_contains_symbol(nm_output, expected));
    if (target_elf) {
        ASSERT(snprintf(unexpected,
                        sizeof(unexpected),
                        "_%s",
                        native_symbol) > 0);
        ASSERT(!nm_output_contains_symbol(nm_output, unexpected));
    }

    free(nm_output);
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

static bool span_contains(const char *start, const char *end, const char *needle) {
    size_t needle_len = strlen(needle);
    size_t span_len = (start != NULL && end != NULL && end >= start) ? (size_t)(end - start) : 0U;

    if (needle_len == 0U) return true;
    if (needle_len > span_len) return false;
    for (size_t index = 0U; index + needle_len <= span_len; ++index) {
        if (memcmp(start + index, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
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
    ASSERT(count_substr(out.c_source, "FENG_NATIVE_SYMBOL(\"c_puts\")") == 2U);
    ASSERT(count_substr(out.c_source,
                        "feng__feng__codegen__mfb__c_puts__from__") == 2U);
    ASSERT(strstr(out.c_source, "((char *)feng_string_data(") != NULL);
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
        "module feng.codegen.mfc;\n"
        "\n"
        "func other(): int { return 7; }\n";
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

static void test_private_generic_representation_same_package_codegen(void) {
    static const char *kProviderSource =
        "open module feng.codegen.private_repr.provider;\n"
        "type PrivateLeaf<T> {\n"
        "    let value: T;\n"
        "}\n"
        "type PrivateEntry<T> {\n"
        "    seal let leaf: PrivateLeaf<T>;\n"
        "    func read(): T { return self.leaf.value; }\n"
        "}\n"
        "open type ExplicitBox<T> {\n"
        "    seal let entry: PrivateEntry<T>;\n"
        "    func read(): T { return self.entry.read(); }\n"
        "}\n"
        "open type InferredBox<T> {\n"
        "    seal let entry = PrivateEntry<T>();\n"
        "    func read(): T { return self.entry.read(); }\n"
        "}\n"
        "func same_module_probe(): bool {\n"
        "    let explicit_int = ExplicitBox<int>();\n"
        "    let inferred_string = InferredBox<string>();\n"
        "    return explicit_int.read() == 0 && inferred_string.read() == \"\";\n"
        "}\n";
    static const char *kConsumerSource =
        "module feng.codegen.private_repr.consumer;\n"
        "import feng.codegen.private_repr.provider;\n"
        "func main(args: string[]) {\n"
        "    let explicit_int = ExplicitBox<int>();\n"
        "    let explicit_string = ExplicitBox<string>();\n"
        "    let inferred_int = InferredBox<int>();\n"
        "    let inferred_string = InferredBox<string>();\n"
        "    if explicit_int.read() != 0 || inferred_int.read() != 0 {\n"
        "        throw \"private generic int failure\";\n"
        "    }\n"
        "    if explicit_string.read() != \"\" || inferred_string.read() != \"\" {\n"
        "        throw \"private generic string failure\";\n"
        "    }\n"
        "}\n";
    FengProgram *provider =
        parse_or_die(kProviderSource, "private_repr_provider.ff");
    FengProgram *consumer =
        parse_or_die(kConsumerSource, "private_repr_consumer.ff");
    const FengProgram *programs[2] = {provider, consumer};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs,
                                 2U,
                                 FENG_COMPILE_TARGET_BIN,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_BIN,
                                     NULL,
                                     &out,
                                     &codegen_error));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "PrivateLeaf__G__i64") != NULL);
    ASSERT(strstr(out.c_source,
                  "PrivateLeaf__G__string") != NULL);
    ASSERT(strstr(out.c_source,
                  "PrivateEntry__G__i64") != NULL);
    ASSERT(strstr(out.c_source,
                  "PrivateEntry__G__string") != NULL);
    ASSERT(strstr(out.c_source,
                  "(void *)&((") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(provider);
    feng_program_free(consumer);
}

static void test_private_representation_cross_package_ft_codegen(void) {
    static const char *kProviderSource =
        "open module vendor.private_repr;\n"
        "open type PublicBox<T> {\n"
        "    seal let managed: HiddenManaged<T>;\n"
        "    seal let value: HiddenValue<T>;\n"
        "    seal let tuple: HiddenTuple<T>;\n"
        "    seal let enum_value: HiddenEnum;\n"
        "    seal let object_value: HiddenObject;\n"
        "    seal let callable_value: HiddenCallable;\n"
        "    seal let union_value: HiddenUnion;\n"
        "    seal let intersection_value: HiddenIntersection;\n"
        "    seal let recursive: HiddenRecursive<T>;\n"
        "}\n"
        "type HiddenManaged<T> { let value: T; }\n"
        "@value\n"
        "type HiddenValue<T> { let value: T; }\n"
        "type HiddenTuple<T>(T, string);\n"
        "enum HiddenEnum { Zero = 0, One = 1 }\n"
        "spec HiddenObject { let item: HiddenManaged<int>; }\n"
        "spec HiddenBase { func count(): int; }\n"
        "spec HiddenCallable(value: HiddenValue<int>): HiddenTuple<int>;\n"
        "spec HiddenUnion: HiddenEnum | HiddenTuple<int>;\n"
        "spec HiddenIntersection: HiddenObject & HiddenBase;\n"
        "type HiddenRecursive<T> { let child: HiddenManaged<T>; }\n"
        "@abi\n"
        "type HiddenAbi {}\n"
        "@cdecl(\"hidden_abi\", \"hidden_abi_identity\")\n"
        "extern func hiddenAbiIdentity(value: HiddenAbi*): HiddenAbi*;\n"
        "type UnusedPrivate {}\n";
    static const char *kConsumerSource =
        "module demo.private_repr;\n"
        "import vendor.private_repr;\n"
        "func main(args: string[]) {\n"
        "    let int_box = PublicBox<int>();\n"
        "    let string_box = PublicBox<string>();\n"
        "}\n";
    static const char *kInvalidConsumerSource =
        "module demo.private_repr_invalid;\n"
        "import vendor.private_repr;\n"
        "func probe() {\n"
        "    let hidden = HiddenManaged<int>();\n"
        "}\n";
    FengProgram *provider_program =
        parse_or_die(kProviderSource, "tests/private_repr_vendor.ff");
    const FengProgram *provider_programs[1] = {provider_program};
    FengSemanticAnalysis *provider_analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolExportOptions export_options = {0};
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions analyze_options = {0};
    FengSymbolError symbol_error = {0};
    FengProgram *consumer_program = NULL;
    const FengProgram *consumer_programs[1];
    FengSemanticAnalysis *consumer_analysis = NULL;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    FengProgram *invalid_program = NULL;
    const FengProgram *invalid_programs[1];
    FengSemanticAnalysis *invalid_analysis = NULL;
    bool saw_private_error = false;

    ASSERT(feng_semantic_analyze(provider_programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &provider_analysis,
                                 &errors,
                                 &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_options.public_root = public_root;
    ASSERT(feng_symbol_export_analysis(provider_analysis,
                                       &export_options,
                                       &symbol_error));
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    analyze_options.target = FENG_COMPILE_TARGET_BIN;
    analyze_options.imported_modules = &query;
    analyze_options.pointer_size = sizeof(void *);

    consumer_program =
        parse_or_die(kConsumerSource, "tests/private_repr_consumer.ff");
    consumer_programs[0] = consumer_program;
    ASSERT(feng_semantic_analyze_with_options(consumer_programs,
                                              1U,
                                              &analyze_options,
                                              &consumer_analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    feng_symbol_imported_module_cache_populate_codegen_metadata(cache,
                                                                 consumer_analysis);
    if (!feng_codegen_emit_program(consumer_analysis,
                                   FENG_COMPILE_TARGET_BIN,
                                   NULL,
                                   &output,
                                   &codegen_error)) {
        fprintf(stderr,
                "cross-package private representation codegen error: %s\n",
                codegen_error.message != NULL ? codegen_error.message
                                              : "(unknown)");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source, "HiddenManaged__G__i64") != NULL);
    ASSERT(strstr(output.c_source, "HiddenManaged__G__string") != NULL);
    ASSERT(strstr(output.c_source, "HiddenValue__G__i64") != NULL);
    ASSERT(strstr(output.c_source, "HiddenTuple__G__string") != NULL);
    ASSERT(strstr(output.c_source, "FengEnum__vendor__private_repr__HiddenEnum") != NULL);
    ASSERT(strstr(output.c_source, "HiddenCallable") != NULL);
    ASSERT(strstr(output.c_source, "HiddenUnion") != NULL);
    ASSERT(strstr(output.c_source, "HiddenIntersection") != NULL);
    ASSERT(strstr(output.c_source, "HiddenRecursive__G__i64") != NULL);
    ASSERT(strstr(output.c_source, "HiddenAbi") != NULL);
    ASSERT(strstr(output.c_source, "UnusedPrivate") == NULL);
    compile_generated_c_or_die(output.c_source);

    invalid_program =
        parse_or_die(kInvalidConsumerSource,
                     "tests/private_repr_invalid_consumer.ff");
    invalid_programs[0] = invalid_program;
    errors = NULL;
    error_count = 0U;
    analyze_options.target = FENG_COMPILE_TARGET_LIB;
    ASSERT(!feng_semantic_analyze_with_options(invalid_programs,
                                               1U,
                                               &analyze_options,
                                               &invalid_analysis,
                                               &errors,
                                               &error_count));
    for (size_t index = 0U; index < error_count; ++index) {
        if (strcmp(errors[index].code, "AE0001") == 0 &&
            strstr(errors[index].message, "HiddenManaged") != NULL) {
            saw_private_error = true;
        }
    }
    ASSERT(saw_private_error);

    feng_semantic_errors_free(errors, error_count);
    feng_program_free(invalid_program);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(consumer_analysis);
    feng_program_free(consumer_program);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    feng_semantic_analysis_free(provider_analysis);
    feng_program_free(provider_program);
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static void test_c_variadic_cross_package_ft_codegen(void) {
    static const char *kProviderSource =
        "open module vendor.c_variadic;\n"
        "@cdecl(\"c\", \"native_variadic\", 2)\n"
        "open extern func send(data: byte*, count: i32, value: f64): i32;\n";
    static const char *kConsumerSource =
        "module demo.c_variadic;\n"
        "import vendor.c_variadic;\n"
        "func call(data: byte*, count: i32, value: f64): i32 {\n"
        "    return send(data, count, value);\n"
        "}\n";
    FengProgram *provider_program =
        parse_or_die(kProviderSource, "tests/c_variadic_vendor.ff");
    const FengProgram *provider_programs[1] = {provider_program};
    FengSemanticAnalysis *provider_analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolExportOptions export_options = {0};
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions analyze_options = {0};
    FengSymbolError symbol_error = {0};
    FengProgram *consumer_program = NULL;
    const FengProgram *consumer_programs[1];
    FengSemanticAnalysis *consumer_analysis = NULL;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(provider_programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &provider_analysis,
                                 &errors,
                                 &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_options.public_root = public_root;
    ASSERT(feng_symbol_export_analysis(provider_analysis,
                                       &export_options,
                                       &symbol_error));
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    analyze_options.target = FENG_COMPILE_TARGET_LIB;
    analyze_options.imported_modules = &query;
    analyze_options.pointer_size = sizeof(void *);

    consumer_program =
        parse_or_die(kConsumerSource, "tests/c_variadic_consumer.ff");
    consumer_programs[0] = consumer_program;
    ASSERT(feng_semantic_analyze_with_options(consumer_programs,
                                              1U,
                                              &analyze_options,
                                              &consumer_analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    feng_symbol_imported_module_cache_populate_codegen_metadata(cache,
                                                                 consumer_analysis);
    ASSERT(feng_codegen_emit_program(consumer_analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "extern int32_t feng__vendor__c_variadic__send__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "(uint8_t *, int32_t, ...) FENG_NATIVE_SYMBOL(\"native_variadic\");") != NULL);
    ASSERT(strstr(output.c_source,
                  "(uint8_t *, int32_t, double) FENG_NATIVE_SYMBOL(\"native_variadic\");") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(consumer_analysis);
    feng_program_free(consumer_program);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    feng_semantic_analysis_free(provider_analysis);
    feng_program_free(provider_program);
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static void test_module_binding_lazy_ensure_init_codegen(void) {
    static const char *kSource =
        "module feng.codegen.topbind;\n"
        "let first: int = compute();\n"
        "let second: int = 41;\n"
        "func compute(): int {\n"
        "    return second + 1;\n"
        "}\n"
        "func helper(): int {\n"
        "    return first;\n"
        "}\n"
        "func main(args: string[]) {\n"
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
        "module feng.codegen.topbindaddr;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_int_ptr(p: int*): void;\n"
        "let value: int = (int)7;\n"
        "func run_case() {\n"
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
        "module feng.codegen.topbindassign;\n"
        "var current: int = (int)1;\n"
        "func run_case() {\n"
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
        "module feng.codegen.topbindobj;\n"
        "type Box {\n"
        "    let value: int;\n"
        "}\n"
        "var current: Box = Box { value: (int)1 };\n"
        "func reset(next: Box) {\n"
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

static void test_type_static_members_codegen(void) {
    static const char *kSource =
        "module feng.codegen.staticm;\n"
        "func base(): int {\n"
        "    return 41;\n"
        "}\n"
        "type Counter {\n"
        "    static let seed: int = base();\n"
        "    static var current: int = 0;\n"
        "    static func make(value: int): int {\n"
        "        return value + Counter.seed;\n"
        "    }\n"
        "}\n"
        "func run_case(): int {\n"
        "    Counter.current = Counter.make(1);\n"
        "    Counter.current += 1;\n"
        "    return Counter.current;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "staticm.ff");
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
        fprintf(stderr, "codegen error (type static members): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticm__Counter__static__seed") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticm__Counter__static__seed__inited = false;") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticm__Counter__static__seed__ensure_init(void)") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticm__Counter__static__current__ensure_init();") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticm__Counter__static__make") != NULL);
    {
        const char *method_name = strstr(out.c_source,
                                         "Feng__feng__codegen__staticm__Counter__static__make");
        const char *signature_end;

        ASSERT(method_name != NULL);
        signature_end = strchr(method_name, ')');
        ASSERT(signature_end != NULL);
        ASSERT(!span_contains(method_name, signature_end, "self"));
        ASSERT(!span_contains(method_name, signature_end, "struct Feng__feng__codegen__staticm__Counter"));
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_builtin_fit_static_method_codegen(void) {
    static const char *kSource =
        "module feng.codegen.fitstatic;\n"
        "fit string {\n"
        "    static func marker(): int {\n"
        "        return 5;\n"
        "    }\n"
        "}\n"
        "func run_case(): int {\n"
        "    return string.marker();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "fitstatic.ff");
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
        fprintf(stderr, "codegen error (builtin fit static method): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengFitBuiltin__feng__codegen__fitstatic__string__i0__marker") != NULL);
    {
        const char *method_name = strstr(out.c_source,
                                         "FengFitBuiltin__feng__codegen__fitstatic__string__i0__marker");
        const char *signature_end;

        ASSERT(method_name != NULL);
        signature_end = strchr(method_name, ')');
        ASSERT(signature_end != NULL);
        ASSERT(!span_contains(method_name, signature_end, "self"));
        ASSERT(!span_contains(method_name, signature_end, "struct FengBuiltin__string"));
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_static_methods_codegen(void) {
    static const char *kSource =
        "module feng.codegen.staticgen;\n"
        "type Util {\n"
        "    static func id<T>(value: T): T {\n"
        "        return value;\n"
        "    }\n"
        "}\n"
        "type Box<T> {\n"
        "    let value: T;\n"
        "    static func make(value: T): Box<T> {\n"
        "        return Box<T> { value: value };\n"
        "    }\n"
        "}\n"
        "fit Box<T> {\n"
        "    static func of(value: T): Box<T> {\n"
        "        return Box<T> { value: value };\n"
        "    }\n"
        "}\n"
        "func run_case(): i32 {\n"
        "    let direct: i32 = Util.id<i32>(41);\n"
        "    let boxed: Box<i32> = Box<i32>.make(1);\n"
        "    let via_fit: Box<i32> = Box<i32>.of(2);\n"
        "    return direct + boxed.value + via_fit.value;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "staticgen.ff");
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
        fprintf(stderr, "codegen error (generic static methods): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "FengGenericMethod__feng__codegen__staticgen__Util__i0__id") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticgen__Util__static__id") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengGenericMethod__feng__codegen__staticgen__Box__i0__make") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__staticgen__Box__G__i32__static__make") != NULL);
    ASSERT(strstr(out.c_source, "__of__from__i32") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_type_static_field_codegen(void) {
    /* Codegen coverage for accessing a static FIELD on a generic type with
     * explicit type arguments (`Box<i32>.tag`). The static-METHOD path is
     * covered by test_generic_static_methods_codegen; this case exercises the
     * field-read path through semantic + codegen and verifies the generated
     * C compiles. */
    static const char *kSource =
        "module feng.codegen.staticfield;\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "    static let tag: i64 = 100;\n"
        "    seal func Box(init: T) {\n"
        "        self.value = init;\n"
        "    }\n"
        "}\n"
        "func run_case(): i64 {\n"
        "    let t: i64 = Box<i32>.tag;\n"
        "    return t;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "staticfield.ff");
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
        fprintf(stderr, "codegen error (generic static field): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* The static field `tag` is a non-generic i64 on Box<T>; with T=i32
     * instantiated, the generated C must expose the type's static field slot
     * and the read expression must compile. We assert the type-instance
     * symbol fragment to ensure the static field is emitted under the
     * instantiated generic context. */
    ASSERT(strstr(out.c_source, "Box") != NULL);
    ASSERT(strstr(out.c_source, "tag") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Regression for CE0163: a generic type with an inferred field whose
 * type is a generic application (e.g. `seal let items = Inner<T>()` with no
 * explicit annotation) must resolve the field type from the semantic type
 * fact during shared-body field access, instead of degrading to CG_TYPE_VOID
 * and failing with "method call on non-object value".
 *
 * This unit test exercises the type-definition + shared-body codegen path in
 * isolation (without a concrete instantiation call site, which would exercise
 * the separate Pass 1.7 nested-concrete-instance registration path covered
 * by fcts cross-package cases).  The cross-package method-call behaviour is
 * validated by fcts `test_inferred_generic_field`. */
static void test_generic_type_inferred_field_codegen(void) {
    static const char *kSource =
        "module feng.codegen.inferred_field;\n"
        "type Inner<T> {\n"
        "    var value: T;\n"
        "    seal func Inner() {}\n"
        "    func set_value(next: T) { self.value = next; }\n"
        "    func get_value(): T { return self.value; }\n"
        "}\n"
        "type Holder<T> {\n"
        "    seal let items = Inner<T>();\n"
        "    seal func Holder() {}\n"
        "    func push(item: T) { self.items.set_value(item); }\n"
        "    func pop(): T { return self.items.get_value(); }\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "inferred_field.ff");
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
        fprintf(stderr, "codegen error (inferred generic field): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "Holder") != NULL);
    ASSERT(strstr(out.c_source, "Inner") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Regression for CE0005: a single-file lib-target program with a concrete
 * instantiation call site (`Holder<i32>()`) where the generic type has an
 * inferred generic field (`seal let items = Inner<T>()`).  The constructor
 * codegen must substitute T→i32 in the field initializer's nested
 * constructor call `Inner<T>()` → `Inner<i32>()`, resolving the concrete
 * instance rather than the open instance (which would fail with CE0005
 * outside a generic method context). */
static void test_generic_type_inferred_field_concrete_call_codegen(void) {
    static const char *kSource =
        "module feng.codegen.inferred_field_concrete;\n"
        "type Inner<T> {\n"
        "    var value: T;\n"
        "    seal func Inner() {}\n"
        "    func set_value(next: T) { self.value = next; }\n"
        "    func get_value(): T { return self.value; }\n"
        "}\n"
        "type Holder<T> {\n"
        "    seal let items = Inner<T>();\n"
        "    seal func Holder() {}\n"
        "    func push(item: T) { self.items.set_value(item); }\n"
        "    func pop(): T { return self.items.get_value(); }\n"
        "}\n"
        "func run_case(): i32 {\n"
        "    let h = Holder<i32>();\n"
        "    h.push(7);\n"
        "    return h.pop();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "inferred_field_concrete.ff");
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
        fprintf(stderr, "codegen error (inferred generic field concrete call): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "Holder") != NULL);
    ASSERT(strstr(out.c_source, "Inner") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Step 3 — spec static member codegen: witness struct slots must omit
 * _subject for static methods/fields, and thunks forward without a
 * subject cast. */
static void test_spec_static_member_witness_codegen(void) {
    static const char *kSource =
        "module feng.codegen.specstatic;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "    static let tag: string;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    let name: string;\n"
        "    static func make(): Widget {\n"
        "        return Widget { name: \"default\" };\n"
        "    }\n"
        "    static let tag: string = \"widget\";\n"
        "}\n"
        "func run_case(): string {\n"
        "    let factory: Factory<Widget> = Widget();\n"
        "    return Widget.tag;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "specstatic.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (spec static members): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (spec static members): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* The witness thunk for the static method must exist (coercion triggers
     * witness table materialisation). The static tag field thunk forwards
     * through the type's static field storage and its ensure_init helper. */
    ASSERT(strstr(out.c_source, "FengSpecThunk__feng__codegen__specstatic") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__specstatic__Widget__static__tag") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__specstatic__Widget__static__tag__ensure_init") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Step 3 — spec static field (var) witness setter thunk forwards without
 * a subject. */
static void test_spec_static_var_witness_codegen(void) {
    static const char *kSource =
        "module feng.codegen.specstaticvar;\n"
        "spec Configurable {\n"
        "    static var current: int;\n"
        "}\n"
        "type Config: Configurable {\n"
        "    static var current: int = 0;\n"
        "}\n"
        "func run_case(): int {\n"
        "    let cfg: Configurable = Config();\n"
        "    Config.current = 42;\n"
        "    return Config.current;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "specstaticvar.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (spec static var): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (spec static var): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecThunk__feng__codegen__specstaticvar") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__feng__codegen__specstaticvar__Config__static__current") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Step 4 — generic param T.make() dispatch via witness table (aggregate return
 * path; T returns a managed type so the slot signature includes void *_out). */
static void test_generic_param_static_method_codegen(void) {
    static const char *kSource =
        "module feng.codegen.genstatic;\n"
        "spec Factory<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "type Widget: Factory<Widget> {\n"
        "    let value: int;\n"
        "    static func make(): Widget {\n"
        "        return Widget { value: 0 };\n"
        "    }\n"
        "}\n"
        "func make_widget<T: Factory<T>>(): T {\n"
        "    return T.make();\n"
        "}\n"
        "func run_case(): int {\n"
        "    let factory: Factory<Widget> = Widget();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genstatic.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (genparam static method): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (genparam static method): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Witness struct slot signature for static method without subject.
     * Aggregate return: void (*make)(void *_out); — no _subject. */
    ASSERT(strstr(out.c_source, "void (*make)(void *_out);") != NULL);
    /* Dispatch call site inside the generic function body. The witness
     * dispatch is the static-method slot call without a subject cast. */
    ASSERT(strstr(out.c_source, "->make(_spec_ret") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Step 4 — generic param T.field dispatch via witness table (read). */
static void test_generic_param_static_field_read_codegen(void) {
    static const char *kSource =
        "module feng.codegen.genfieldr;\n"
        "spec Tagged {\n"
        "    static let tag: int;\n"
        "}\n"
        "type Widget: Tagged {\n"
        "    static let tag: int = 42;\n"
        "}\n"
        "func get_tag<T: Tagged>(): int {\n"
        "    return T.tag;\n"
        "}\n"
        "func run_case(): int {\n"
        "    let tagged: Tagged = Widget();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genfieldr.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (genparam field read): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (genparam field read): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Static field getter slot has no _subject parameter. */
    ASSERT(strstr(out.c_source, "(*get_tag)(void);") != NULL);
    /* Dispatch call site uses witness->get_tag() with no subject arg. */
    ASSERT(strstr(out.c_source, "->get_tag()") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Step 4 — generic param T.field dispatch via witness table (write var). */
static void test_generic_param_static_field_write_codegen(void) {
    static const char *kSource =
        "module feng.codegen.genfieldw;\n"
        "spec Configurable {\n"
        "    static var current: int;\n"
        "}\n"
        "type Config: Configurable {\n"
        "    static var current: int = 0;\n"
        "}\n"
        "func set_current<T: Configurable>(value: int): void {\n"
        "    T.current = value;\n"
        "}\n"
        "func run_case(): int {\n"
        "    let cfg: Configurable = Config();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genfieldw.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (genparam field write): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (genparam field write): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Static var setter slot signature: void (*set_X)(int32_t value); — no subject. */
    ASSERT(strstr(out.c_source, "void (*set_") != NULL);
    /* Dispatch call site uses witness->set_current(value) with no subject arg. */
    ASSERT(strstr(out.c_source, "->set_") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Step 4 — spec-to-spec slot witness for inherited static members. Child spec
 * inherits Factory's static make(), requiring the slot thunk to forward without
 * a subject. */
static void test_spec_inherited_static_slot_codegen(void) {
    static const char *kSource =
        "module feng.codegen.specinh;\n"
        "spec Base<T> {\n"
        "    static func make(): T;\n"
        "}\n"
        "spec Derived<T>: Base<T> {\n"
        "}\n"
        "type Widget: Derived<Widget> {\n"
        "    let value: int;\n"
        "    static func make(): Widget {\n"
        "        return Widget { value: 0 };\n"
        "    }\n"
        "}\n"
        "func run_case(): int {\n"
        "    let derived: Derived<Widget> = Widget();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "specinh.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (spec inherited static): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (spec inherited static): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Both spec witness structs (Base and Derived) exist with a make slot. */
    ASSERT(strstr(out.c_source, "void (*make)(void *_out);") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_module_binding_default_zero_ensure_init_codegen(void) {
    static const char *kSource =
        "module feng.codegen.topbindzero;\n"
        "type User {\n"
        "    let id: int;\n"
        "}\n"
        "let current: User;\n"
        "func read_id(): int {\n"
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
        "module feng.codegen.callconv;\n"
        "@stdcall(\"user32\")\n"
        "extern func stdcall_value(value: int): int;\n"
        "@fastcall(\"helper\")\n"
        "extern func fastcall_value(value: int): int;\n"
        "func run() {\n"
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
    ASSERT(strstr(out.c_source,
                  "FENG_EXTERN_CALLCONV_STDCALL feng__feng__codegen__callconv__stdcall_value__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "FENG_EXTERN_CALLCONV_FASTCALL feng__feng__codegen__callconv__fastcall_value__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "FENG_NATIVE_SYMBOL(\"stdcall_value\")") != NULL);
    ASSERT(strstr(out.c_source,
                  "FENG_NATIVE_SYMBOL(\"fastcall_value\")") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_extern_c_symbol_name_codegen(void) {
    static const char *kSource =
        "module feng.codegen.externsymbol;\n"
        "let c_name = \"fabs\";\n"
        "@cdecl(\"m\", c_name)\n"
        "extern func abs_value(x: double): double;\n"
        "func run(x: double): double {\n"
        "    return abs_value(x);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "extern_symbol.ff");
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
        fprintf(stderr, "codegen error (extern C symbol): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "extern double fabs(double);") == NULL);
    ASSERT(strstr(out.c_source,
                  "extern double feng__feng__codegen__externsymbol__abs_value__from__f64(double) FENG_NATIVE_SYMBOL(\"fabs\");") != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng__feng__codegen__externsymbol__abs_value__from__f64(") == 2U);
    ASSERT(strstr(out.c_source, "extern double abs_value(double);") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_extern_overload_uses_resolved_declaration_codegen(void) {
    static const char *kSource =
        "module feng.codegen.externoverload;\n"
        "@cdecl(\"c\", \"extern_i32\")\n"
        "extern func select(value: i32): i32;\n"
        "@cdecl(\"c\", \"extern_f64\")\n"
        "extern func select(value: f64): f64;\n"
        "func select_i32(value: i32): i32 {\n"
        "    return select(value);\n"
        "}\n"
        "func select_f64(value: f64): f64 {\n"
        "    return select(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "extern_overload.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    if (!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                               &analysis, &errors, &error_count)) {
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "semantic error (extern overload identity): %s\n",
                    errors[i].message ? errors[i].message : "(unknown)");
        }
        ASSERT(false);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (extern overload identity): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    {
        const char *i32_name =
            "feng__feng__codegen__externoverload__select__from__i32(";
        const char *f64_name =
            "feng__feng__codegen__externoverload__select__from__f64(";

        ASSERT(strstr(out.c_source, "FENG_NATIVE_SYMBOL(\"extern_i32\")") != NULL);
        ASSERT(strstr(out.c_source, "FENG_NATIVE_SYMBOL(\"extern_f64\")") != NULL);
        ASSERT(count_substr(out.c_source, i32_name) == 2U);
        ASSERT(count_substr(out.c_source, f64_name) == 2U);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_extern_native_symbol_alias_codegen(void) {
    static const char *kNativeSymbol = "feng_test_native_alias_symbol";
    static const char *kSource =
        "module feng.codegen.nativealias;\n"
        "@cdecl(\"c\", \"feng_test_native_alias_symbol\")\n"
        "extern func pointer_call(value: byte*): i32;\n"
        "@cdecl(\"c\", \"feng_test_native_alias_symbol\")\n"
        "extern func scalar_call(value: i32): i64;\n"
        "@cdecl(\"c\", \"feng_test_native_alias_symbol\")\n"
        "extern func floating_call(value: f64): f64;\n"
        "@cdecl(\"c\", \"feng_test_escaped_\\\"symbol\")\n"
        "extern func escaped_symbol(value: i32): i32;\n"
        "open func use_pointer(value: byte*): i32 {\n"
        "    return pointer_call(value);\n"
        "}\n"
        "open func use_scalar(value: i32): i64 {\n"
        "    return scalar_call(value);\n"
        "}\n"
        "open func use_floating(value: f64): f64 {\n"
        "    return floating_call(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "extern_native_alias.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "#define FENG_NATIVE_SYMBOL(name) __asm__(\"_\" name)") != NULL);
    ASSERT(strstr(out.c_source,
                  "#define FENG_NATIVE_SYMBOL(name) __asm__(name)") != NULL);
    ASSERT(strstr(out.c_source, "Windows/COFF is") != NULL);
    ASSERT(count_substr(out.c_source,
                        "FENG_NATIVE_SYMBOL(\"feng_test_native_alias_symbol\")") == 3U);
    ASSERT(strstr(out.c_source,
                  "FENG_NATIVE_SYMBOL(\"feng_test_escaped_\\\"symbol\")") != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng__feng__codegen__nativealias__pointer_call__from__") == 2U);
    ASSERT(count_substr(out.c_source,
                        "feng__feng__codegen__nativealias__scalar_call__from__") == 2U);
    ASSERT(count_substr(out.c_source,
                        "feng__feng__codegen__nativealias__floating_call__from__") == 2U);
    ASSERT(strstr(out.c_source, "feng_test_native_alias_symbol(") == NULL);
    compile_generated_c_or_die(out.c_source);
#if defined(__APPLE__)
    assert_generated_native_symbol_relocation(out.c_source, false, kNativeSymbol);
#endif
    assert_generated_native_symbol_relocation(out.c_source, true, kNativeSymbol);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_address_of_scalar_and_array_codegen(void) {
    static const char *kSource =
        "module feng.codegen.addr;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_i32_ptr(p: i32*): void;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_array_ptr(p: int*): void;\n"
        "func run() {\n"
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
        "module feng.codegen.abifn;\n"
        "@abi\n"
        "spec Cmp(a: int, b: int): int;\n"
        "@abi\n"
        "type Holder {\n"
        "    var cb: Cmp*;\n"
        "}\n"
        "@abi\n"
        "func cmp(a: int, b: int): int {\n"
        "    return a - b;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func c_register_cmp(cb: Cmp*): void;\n"
        "@cdecl(\"c\")\n"
        "extern func c_load_cmp(): Cmp*;\n"
        "func run() {\n"
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
                  "extern FengAbiFnPtr__feng__codegen__abifn__Cmp feng__feng__codegen__abifn__c_load_cmp__from__void(void) FENG_NATIVE_SYMBOL(\"c_load_cmp\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengAbiFnPtr__feng__codegen__abifn__Cmp cb;") != NULL);
    ASSERT(strstr(out.c_source, "&feng__feng__codegen__abifn__cmp") != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng__feng__codegen__abifn__c_load_cmp__from__void(") == 2U);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_value_pointer_codegen(void) {
    static const char *kSource =
        "module feng.codegen.abivalue;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: i32;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_point_ptr(p: Point*): void;\n"
        "@cdecl(\"c\")\n"
        "extern func c_roundtrip_point_ptr(p: Point*): Point*;\n"
        "func run() {\n"
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
                  "extern void feng__feng__codegen__abivalue__c_use_point_ptr__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "(struct Feng__feng__codegen__abivalue__Point__AbiLayout *) FENG_NATIVE_SYMBOL(\"c_use_point_ptr\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng__feng__codegen__abivalue__c_use_point_ptr__from__") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_abi_array_pointee_codegen(void) {
    static const char *kSource =
        "module feng.codegen.arraypointee;\n"
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
        "extern func c_load_bytes(): byte[]*;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_bytes(data: byte[]*): void;\n"
        "@cdecl(\"c\")\n"
        "extern func c_load_points(): Point[]*;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_points(data: Point[]*): void;\n"
        "func run() {\n"
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
                  "extern uint8_t * feng__feng__codegen__arraypointee__c_load_bytes__from__void(void) FENG_NATIVE_SYMBOL(\"c_load_bytes\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__feng__codegen__arraypointee__c_use_bytes__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "(uint8_t *) FENG_NATIVE_SYMBOL(\"c_use_bytes\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "uint8_t * data;") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__feng__codegen__arraypointee__Point__AbiLayout * feng__feng__codegen__arraypointee__c_load_points__from__void(void) FENG_NATIVE_SYMBOL(\"c_load_points\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__feng__codegen__arraypointee__c_use_points__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "(struct Feng__feng__codegen__arraypointee__Point__AbiLayout *) FENG_NATIVE_SYMBOL(\"c_use_points\");") != NULL);
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
        "module feng.codegen.opaquehandle;\n"
        "@abi\n"
        "type Handle {\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func c_make_handle(): Handle*;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_handle(handle: Handle*): void;\n"
        "@cdecl(\"c\")\n"
        "extern func c_roundtrip_handle(handle: Handle*): Handle*;\n"
        "func run() {\n"
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
                  "extern struct Feng__feng__codegen__opaquehandle__Handle * feng__feng__codegen__opaquehandle__c_make_handle__from__void(void) FENG_NATIVE_SYMBOL(\"c_make_handle\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void feng__feng__codegen__opaquehandle__c_use_handle__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "(struct Feng__feng__codegen__opaquehandle__Handle *) FENG_NATIVE_SYMBOL(\"c_use_handle\");") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern struct Feng__feng__codegen__opaquehandle__Handle * feng__feng__codegen__opaquehandle__c_roundtrip_handle__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "(struct Feng__feng__codegen__opaquehandle__Handle *) FENG_NATIVE_SYMBOL(\"c_roundtrip_handle\");") != NULL);
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
        "module feng.codegen.opaqueabifn;\n"
        "@abi\n"
        "open type Handle {\n"
        "}\n"
        "@abi\n"
        "open func roundtrip(handle: Handle*): Handle* {\n"
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
        "module feng.codegen.abivalueextern;\n"
        "@abi\n"
        "type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@cdecl(\"c\")\n"
        "extern func create_point(x: int, y: int): Point;\n"
        "@cdecl(\"c\")\n"
        "extern func point_sum(p: Point): int;\n"
        "func run() {\n"
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
                  "extern struct Feng__feng__codegen__abivalueextern__Point__AbiLayout feng__feng__codegen__abivalueextern__create_point__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "FENG_NATIVE_SYMBOL(\"create_point\")") != NULL);
    /* int is platform-dependent: i32 (int32_t) on 32-bit, i64 (int64_t) on 64-bit. */
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "extern %s feng__feng__codegen__abivalueextern__point_sum__from__",
                 int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
    ASSERT(strstr(out.c_source,
                  "FENG_NATIVE_SYMBOL(\"point_sum\")") != NULL);
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
        "module feng.codegen.runtimeextern;\n"
        "@runtime\n"
        "extern func feng_string_utf8_length(value: string): i64;\n"
        "func run(value: string): i64 {\n"
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
        "module feng.codegen.genericruntimeextern;\n"
        "@runtime\n"
        "extern func feng_array_get_length<T>(value: T[]): i64;\n"
        "func run(values: int[]): i64 {\n"
        "    return feng_array_get_length(values);\n"
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
    {
        /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
        const char *int_descriptor = sizeof(void *) >= 8U ? "feng_i64_descriptor" : "feng_i32_descriptor";
        char expected[256];
        snprintf(expected, sizeof(expected),
                 "feng_array_get_length(&(const FengGenericParamDescriptor){.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL}, values)",
                 int_descriptor);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_call_accepts_explicit_type_args(void) {
    static const char *kSource =
        "module feng.codegen.genericruntimeexternexplicit;\n"
        "@runtime\n"
        "extern func feng_array_get_length<T>(value: T[]): i64;\n"
        "func run(values: int[]): i64 {\n"
        "    return feng_array_get_length<int>(values);\n"
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
    {
        /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
        const char *int_descriptor = sizeof(void *) >= 8U ? "feng_i64_descriptor" : "feng_i32_descriptor";
        char expected[256];
        snprintf(expected, sizeof(expected),
                 "feng_array_get_length(&(const FengGenericParamDescriptor){.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL}, values)",
                 int_descriptor);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Regression for array-storage runtime contracts: generic carriers must be
 * injected, direct T values must use an address carrier, and migrate's +1
 * array result must be taken by assignment without an extra retain. */
static void test_array_storage_runtime_contract_codegen(void) {
    static const char *kSource =
        "module feng.codegen.arraystoragecontract;\n"
        "@runtime\n"
        "extern func feng_array_storage_get_capacity<T>(array: T[!]): int;\n"
        "@runtime\n"
        "extern func feng_array_storage_insert<T>(array: T[!], index: int, value: T): void;\n"
        "@runtime\n"
        "extern func feng_array_storage_remove<T>(array: T[!], index: int, count: int): void;\n"
        "@runtime\n"
        "extern func feng_array_storage_migrate<T>(array: T[!], newCapacity: int): T[!];\n"
        "func run(values: int[!], value: int): int {\n"
        "    var storage: int[!] = values;\n"
        "    let capacity = feng_array_storage_get_capacity(storage);\n"
        "    feng_array_storage_insert(storage, 0, value);\n"
        "    feng_array_storage_remove(storage, 0, 0);\n"
        "    storage = feng_array_storage_migrate(storage, capacity);\n"
        "    return feng_array_storage_get_capacity(storage);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "arraystoragecontract.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    char descriptor[256];
    const char *int_descriptor = sizeof(void *) >= 8U
                                     ? "feng_i64_descriptor"
                                     : "feng_i32_descriptor";

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);

    snprintf(descriptor,
             sizeof(descriptor),
             "&(const FengGenericParamDescriptor){.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL}",
             int_descriptor);
    ASSERT(count_substr(out.c_source, descriptor) == 5U);
    ASSERT(count_substr(out.c_source,
                        "feng_array_storage_get_capacity(") == 2U);
    ASSERT(count_substr(out.c_source,
                        "feng_array_storage_insert(") == 1U);
    ASSERT(count_substr(out.c_source,
                        "feng_array_storage_remove(") == 1U);
    ASSERT(count_substr(out.c_source,
                        "feng_array_storage_migrate(") == 1U);
    ASSERT(strstr(out.c_source, "&_rga") != NULL);
    ASSERT(strstr(out.c_source,
                  "{ void *_old = _l_storage_0; _l_storage_0 = feng_array_storage_migrate(") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_assign((void**)&_l_storage_0, feng_array_storage_migrate(") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_expression_equal_codegen(void) {
    static const char *kSource =
        "module feng.codegen.genericruntimeexprequal;\n"
        "@runtime\n"
        "extern func feng_expression_equal<T>(left: T, right: T): bool;\n"
        "func run(left: int, right: int): bool {\n"
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
    {
        /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
        const char *int_descriptor = sizeof(void *) >= 8U ? "feng_i64_descriptor" : "feng_i32_descriptor";
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[256];
        char rga_pattern[64];
        snprintf(expected, sizeof(expected),
                 "feng_expression_equal(&(const FengGenericParamDescriptor){.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL}, &_rga",
                 int_descriptor);
        ASSERT(strstr(out.c_source, expected) != NULL);
        snprintf(rga_pattern, sizeof(rga_pattern), "%s _rga", int_c_type);
        ASSERT(count_substr(out.c_source, rga_pattern) == 2U);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_runtime_extern_direct_type_param_return_codegen(void) {
    static const char *kSource =
        "module feng.codegen.genericruntimeidentityreturn;\n"
        "@runtime\n"
        "extern func __test_value_identity<T>(value: T): T;\n"
        "func run(value: int): int {\n"
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
    {
        /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
        const char *int_descriptor = sizeof(void *) >= 8U ? "feng_i64_descriptor" : "feng_i32_descriptor";
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[256];
        char rga_pattern[64];
        char rgr_pattern[64];
        snprintf(expected, sizeof(expected),
                 "__test_value_identity(&(const FengGenericParamDescriptor){.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL}, &_rga",
                 int_descriptor);
        ASSERT(strstr(out.c_source, expected) != NULL);
        ASSERT(strstr(out.c_source, ", &_rgr") != NULL);
        snprintf(rga_pattern, sizeof(rga_pattern), "%s _rga", int_c_type);
        snprintf(rgr_pattern, sizeof(rgr_pattern), "%s _rgr", int_c_type);
        ASSERT(count_substr(out.c_source, rga_pattern) == 1U);
        ASSERT(count_substr(out.c_source, rgr_pattern) == 1U);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_runtime_extern_codegen_rejects_non_contract_symbol(void) {
    static const char *kSource =
        "module feng.codegen.runtimeexternreject;\n"
        "@runtime\n"
        "extern func feng_not_contract(value: string): i64;\n"
        "func run(value: string): i64 {\n"
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

static void test_generic_function_call_no_infer_rejected(void) {
    /* A generic function called without explicit type args and without any
     * argument from which T can be inferred must be rejected (规范 §326
     * 错语法九).
     *
     * TODO: 当前实现在 codegen 阶段报 CE0308，与规范期望的 semantic 阶段
     * AE 码不符（错误码文档 docs/feng-error-codes-ce.md:129 标注
     * CE0167→CE0308 "回到AE"，但实际未回到 AE 段；src/codegen/codegen.c
     * :27356 仍用旧码 CE0308）。此处先按实际错误码断言，后续修正实现
     * （让 semantic 阶段在调用点推导失败时报 AE 码）后将此测试迁移至
     * test/semantic/test_semantic.c 并改用 AE 码断言。 */
    static const char *kSource =
        "module feng.codegen.genfninfer;\n"
        "func make<T>(): T {\n"
        "}\n"
        "func run(): void {\n"
        "    let b = make();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genfninfer.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    /* TODO: semantic 阶段当前不报错；修正后应改为 ASSERT(!ok) 并检查 AE 码。 */
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    ASSERT(!cg_ok);
    ASSERT(cgerr.code != NULL);
    ASSERT(strcmp(cgerr.code, "CE0308") == 0);
    ASSERT(strstr(cgerr.message, "cannot infer type argument") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_unsupported_pointer_pointee_reports_explicit_error(void) {
    static const char *kSource =
        "module feng.codegen.badpointee;\n"
        "type User {\n"
        "    var name: string;\n"
        "}\n"
        "func run() {\n"
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
        "module feng.codegen.abivaluefn;\n"
        "@abi\n"
        "open type Point {\n"
        "    var x: int;\n"
        "    var y: int;\n"
        "}\n"
        "@abi\n"
        "spec PointMapper(p: Point): Point;\n"
        "@abi\n"
        "open func echo(p: Point): Point {\n"
        "    return p;\n"
        "}\n"
        "func run() {\n"
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
        "module feng.codegen.exposed;\n"
        "open func public_fn(): i32 {\n"
        "    return 1;\n"
        "}\n"
        "func hidden_fn(): i32 {\n"
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
        "open module vendor.api;\n"
        "open type User {\n"
        "    open let name: string;\n"
        "}\n"
        "open func make(): User {\n"
        "    return User { name: \"hi\" };\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.api as api;\n"
        "func project() {\n"
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
    options.pointer_size = sizeof(void *);

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
        "open module vendor.api;\n"
        "open type User {\n"
        "    open let name: string;\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.api as api;\n"
        "func keep(user: api.User): api.User {\n"
        "    return user;\n"
        "}\n"
        "func count(users: api.User[]): int {\n"
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
    options.pointer_size = sizeof(void *);

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
        "open module vendor.api;\n"
        "open type User {\n"
        "    open let name: string;\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "func keep(user: vendor.api.User): vendor.api.User {\n"
        "    return user;\n"
        "}\n"
        "func count(users: vendor.api.User[]): int {\n"
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
    options.pointer_size = sizeof(void *);

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
        "open module vendor.values;\n"
        "open let count: int = 7;\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.values as values;\n"
        "func project(): int {\n"
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
    options.pointer_size = sizeof(void *);

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
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "extern %s feng__vendor__values__count;", int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
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

static void test_imported_public_static_members_codegen_compiles(void) {
    static const char *kImportedSource =
        "open module vendor.static_values;\n"
        "open type Counter {\n"
        "    open static let seed: int = 7;\n"
        "    open static var current: int = 1;\n"
        "    open static func make(): int {\n"
        "        return 2;\n"
        "    }\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.static_values as values;\n"
        "func project(next: int): int {\n"
        "    values.Counter.current = next;\n"
        "    return values.Counter.seed + values.Counter.make() + values.Counter.current;\n"
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
                                 "tests/imported_static_members_vendor.ff",
                                 kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_or_die(kConsumerSource, "tests/imported_static_members_consumer.ff");
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
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "extern %s Feng__vendor__static_values__Counter__static__seed;",
                 int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
        ASSERT(strstr(out.c_source,
                      "extern void Feng__vendor__static_values__Counter__static__seed__ensure_init(void);") != NULL);
        snprintf(expected, sizeof(expected),
                 "extern %s Feng__vendor__static_values__Counter__static__current;",
                 int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
    ASSERT(strstr(out.c_source,
                  "Feng__vendor__static_values__Counter__static__make") != NULL);
    ASSERT(strstr(out.c_source,
                  "Feng__vendor__static_values__Counter__static__current__ensure_init();") != NULL);

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_public_var_binding_read_write_codegen_compiles(void) {
    static const char *kImportedSource =
        "open module vendor.state;\n"
        "open var count: int = 1;\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.state as state;\n"
        "func project(next: int): int {\n"
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
    options.pointer_size = sizeof(void *);

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
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "extern %s feng__vendor__state__count;", int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
        ASSERT(strstr(out.c_source,
                      "extern void feng__vendor__state__count__ensure_init__from__void(void);") != NULL);
        ASSERT(count_substr(out.c_source,
                            "feng__vendor__state__count__ensure_init__from__void();") == 3U);
        snprintf(expected, sizeof(expected),
                 "feng__vendor__state__count = (%s)(next);", int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_public_binding_address_of_codegen_compiles(void) {
    static const char *kImportedSource =
        "open module vendor.values;\n"
        "open let count: int = 7;\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.values as values;\n"
        "@cdecl(\"c\")\n"
        "extern func c_use_int_ptr(p: int*): void;\n"
        "func run_case() {\n"
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
    options.pointer_size = sizeof(void *);

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
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "extern %s feng__vendor__values__count;", int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
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
        "open module vendor.values;\n"
        "open let count: int = 3 + 4;\n"
        "open var total: int = 1;\n"
        "open func read(): int {\n"
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
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "%s feng__vendor__values__count = 0;", int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
        snprintf(expected, sizeof(expected),
                 "%s feng__vendor__values__total = 0;", int_c_type);
        ASSERT(strstr(out.c_source, expected) != NULL);
        ASSERT(strstr(out.c_source,
                      "void feng__vendor__values__count__ensure_init__from__void(void);") != NULL);
        ASSERT(strstr(out.c_source,
                      "void feng__vendor__values__total__ensure_init__from__void(void);") != NULL);
        snprintf(expected, sizeof(expected),
                 "static %s feng__vendor__values__count = 0;", int_c_type);
        ASSERT(strstr(out.c_source, expected) == NULL);
        ASSERT(strstr(out.c_source,
                      "static void feng__vendor__values__count__ensure_init__from__void(void)") == NULL);
    }

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_public_binding_infers_constructor_type_codegen(void) {
    static const char *kSource =
        "open module vendor.math;\n"
        "open type Math {\n"
        "}\n"
        "open let math = Math();\n"
        "open func current(): Math {\n"
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
        "open module vendor.math;\n"
        "open type Math {\n"
        "}\n"
        "open let math = Math();\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.math as m;\n"
        "func project(): int {\n"
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
    options.pointer_size = sizeof(void *);

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
        "module feng.codegen.enumvalue;\n"
        "enum HttpStatus { Ok = 200, NotFound = 404 }\n"
        "@cdecl(\"m\")\n"
        "extern func use_status_ptr(status: HttpStatus*): void;\n"
        "type Response {\n"
        "    let status: HttpStatus;\n"
        "}\n"
        "func fallback(): HttpStatus {\n"
        "    let status: HttpStatus;\n"
        "    return status;\n"
        "}\n"
        "func roundtrip(status: HttpStatus, history: HttpStatus[]): HttpStatus {\n"
        "    let current: HttpStatus = history[0];\n"
        "    let ptr: HttpStatus* = &current;\n"
        "    use_status_ptr(ptr);\n"
        "    if status == current {\n"
        "        return status;\n"
        "    }\n"
        "    return current;\n"
        "}\n"
        "func selected(): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[128];
        snprintf(pattern, sizeof(pattern), "feng_scalar_box_new_%s", int_canonical);
        ASSERT(strstr(out.c_source, pattern) == NULL);
    }

    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_imported_enum_codegen_emits_visible_symbols(void) {
    static const char *kImportedSource =
        "open module vendor.http;\n"
        "open enum HttpStatus { Ok = 200, NotFound = 404 }\n";
    static const char *kConsumerSource =
        "module demo.enumconsumer;\n"
        "import vendor.http as http;\n"
        "func selected(): int {\n"
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
    options.pointer_size = sizeof(void *);

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

static void test_imported_enum_union_default_codegen_stays_file_scoped(void) {
    static const char *kImportedSource =
        "open module vendor.choice;\n"
        "open enum Status { Ready = 7, Done = 9 }\n"
        "open spec Choice: Status | int;\n";
    static const char *kConsumerSource =
        "module demo.choiceconsumer;\n"
        "import vendor.choice;\n"
        "func accept(value: Choice): int {\n"
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
    const char *enum_decl;
    const char *union_init;

    imported_source_fixture_init(&fixture,
                                 "tests/imported_enum_union_vendor.ff",
                                 kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_or_die(kConsumerSource, "tests/imported_enum_union_consumer.ff");
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

    enum_decl = strstr(out.c_source,
                       "typedef int32_t FengEnum__vendor__choice__Status;");
    union_init = strstr(out.c_source,
                        "static void FengSpecAggInit__vendor__choice__Choice");
    ASSERT(enum_decl != NULL);
    ASSERT(union_init != NULL);
    ASSERT(enum_decl < union_init);
    ASSERT(count_substr(out.c_source,
                        "typedef int32_t FengEnum__vendor__choice__Status;") == 1U);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_imported_generic_enum_argument_uses_canonical_identity(void) {
    static const char *kImportedSource =
        "open module vendor.event;\n"
        "open enum Status { Ready = 7, Done = 9 }\n"
        "open spec Choice<T1, T2>: T1 | T2;\n"
        "@value\n"
        "open type Event {\n"
        "    let content: Choice<Status, u32>;\n"
        "    func Event(content: Choice<Status, u32>) {\n"
        "        self.content = content;\n"
        "    }\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.eventconsumer;\n"
        "import vendor.event;\n"
        "func read(event: Event): int {\n"
        "    return 0;\n"
        "}\n";
    static const char *kCanonicalConstructor =
        "Feng__vendor__event__Event__ctor__Event__from__S_FengSpecValue__"
        "vendor__event__Choice__G__vendor__event__Status__u32";
    static const char *kCanonicalDisplayType =
        "vendor.event.Choice<vendor.event.Status, u32>";
    FengCodegenMapingSourceMapping provider_mapping = {
        .source_path = "tests/imported_generic_enum_vendor.ff",
        .package_name = "vendor",
        .package_root = "tests",
    };
    FengCodegenMapingSourceMapping consumer_mapping = {
        .source_path = "tests/imported_generic_enum_consumer.ff",
        .package_name = "demo",
        .package_root = "tests",
    };
    FengCodegenOptions provider_codegen_options = {
        .debug_source_mappings = &provider_mapping,
        .debug_source_mapping_count = 1U,
    };
    FengCodegenOptions consumer_codegen_options = {
        .debug_source_mappings = &consumer_mapping,
        .debug_source_mapping_count = 1U,
    };
    ImportedSourceFixture fixture;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions analyze_options;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput provider_out = {0};
    FengCodegenOutput consumer_out = {0};
    FengCodegenError provider_cgerr = {0};
    FengCodegenError consumer_cgerr = {0};
    bool provider_field_found = false;
    bool consumer_field_found = false;

    imported_source_fixture_init(&fixture,
                                 "tests/imported_generic_enum_vendor.ff",
                                 kImportedSource);
    ASSERT(feng_codegen_emit_program(fixture.analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     &provider_codegen_options,
                                     &provider_out,
                                     &provider_cgerr));
    ASSERT(provider_out.c_source != NULL);

    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    analyze_options.target = FENG_COMPILE_TARGET_LIB;
    analyze_options.imported_modules = &query;
    analyze_options.pointer_size = sizeof(void *);
    program = parse_or_die(kConsumerSource,
                           "tests/imported_generic_enum_consumer.ff");
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &analyze_options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     &consumer_codegen_options,
                                     &consumer_out,
                                     &consumer_cgerr));
    ASSERT(consumer_out.c_source != NULL);
    ASSERT(strstr(provider_out.c_source, kCanonicalConstructor) != NULL);
    ASSERT(strstr(consumer_out.c_source, kCanonicalConstructor) != NULL);

    for (size_t i = 0U; i < provider_out.debug_info.variable_count; ++i) {
        const FengCodegenMapingVariableRecord *variable =
            &provider_out.debug_info.variables[i];
        if (variable->kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD &&
            variable->display_name != NULL &&
            strcmp(variable->display_name, "content") == 0 &&
            variable->parent_display_type != NULL &&
            strcmp(variable->parent_display_type, "vendor.event.Event") == 0 &&
            strcmp(variable->display_type, kCanonicalDisplayType) == 0) {
            provider_field_found = true;
        }
    }
    for (size_t i = 0U; i < consumer_out.debug_info.variable_count; ++i) {
        const FengCodegenMapingVariableRecord *variable =
            &consumer_out.debug_info.variables[i];
        if (variable->kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD &&
            variable->display_name != NULL &&
            strcmp(variable->display_name, "content") == 0 &&
            variable->parent_display_type != NULL &&
            strcmp(variable->parent_display_type, "vendor.event.Event") == 0 &&
            strcmp(variable->display_type, kCanonicalDisplayType) == 0) {
            consumer_field_found = true;
        }
    }
    ASSERT(provider_field_found);
    ASSERT(consumer_field_found);
    compile_generated_c_or_die(provider_out.c_source);
    compile_generated_c_or_die(consumer_out.c_source);

    feng_codegen_output_free(&provider_out);
    feng_codegen_output_free(&consumer_out);
    feng_codegen_error_free(&provider_cgerr);
    feng_codegen_error_free(&consumer_cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    imported_source_fixture_dispose(&fixture);
}

static void test_bin_public_functions_remain_static(void) {
    static const char *kSource =
        "module feng.codegen.exportbin;\n"
        "open func public_fn(): i32 {\n"
        "    return 1;\n"
        "}\n"
        "func main(args: string[]) {\n"
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
        "module feng.codegen.dup.hello;\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "func make_hello(): string {\n"
        "    let u = User { name: \"hi\" };\n"
        "    return u.name;\n"
        "}\n";
    static const char *kDebugSrc =
        "module feng.codegen.dup.debug;\n"
        "type User {\n"
        "    let id: i32;\n"
        "}\n"
        "func make_debug(): i32 {\n"
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
        "module feng.codegen.ops;\n"
        "func run() {\n"
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
    ASSERT(strstr(out.c_source, "#include \"feng_generated.h\"") != NULL);
    ASSERT(strstr(out.c_source, "fmodf(") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_builtin_direct_call_codegen_shape(void) {
    static const char *kFitBuiltinSource =
        "module feng.codegen.fitbuiltin;\n"
        "fit int {\n"
        "    func double(): int {\n"
        "        return self * 2;\n"
        "    }\n"
        "}\n"
        "fit int[] {\n"
        "    func head(): int {\n"
        "        return self[0];\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
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
        "module feng.codegen.fit_builtin_generic_array;\n"
        "type Span<T> {\n"
        "    let origin: T[];\n"
        "    let start: i64;\n"
        "    let end: i64;\n"
        "    func Span(origin: T[], start: i64, end: i64) {\n"
        "        self.origin = origin;\n"
        "        self.start = start;\n"
        "        self.end = end;\n"
        "    }\n"
        "    func length(): i64 {\n"
        "        return self.end - self.start;\n"
        "    }\n"
        "    func get(index: i64): T {\n"
        "        return self.origin[self.start + index];\n"
        "    }\n"
        "}\n"
        "fit T[] {\n"
        "    func slice(start: i64, end: i64): Span<T> {\n"
        "        return Span<T>(self, start, end);\n"
        "    }\n"
        "    func slice(start: i64): Span<T> {\n"
        "        return self.slice(start, (i64)4);\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
        "    let values: int[] = [1, 2, 3, 4];\n"
        "    let middle = values.slice((i64)1, (i64)3);\n"
        "    let tail = values.slice((i64)1);\n"
        "    return middle.get((i64)0) + (int)middle.length() + tail.get((i64)0) + (int)tail.length();\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char span_pattern[64];
        snprintf(span_pattern, sizeof(span_pattern), "Span__G__%s", int_canonical);
        ASSERT(strstr(out.c_source, span_pattern) != NULL);
    }
    ASSERT(strstr(out.c_source, "Span__G__T") != NULL);
    ASSERT(strstr(out.c_source, "__slice__from__i64__i64") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_builtin_array_open_generic_value_return_codegen(void) {
    static const char *kSource =
        "module feng.codegen.fit_builtin_generic_value_array;\n"
        "@value\n"
        "type ValueSpan<T> {\n"
        "    let origin: T[];\n"
        "    let start: i64;\n"
        "    let end: i64;\n"
        "    func ValueSpan(origin: T[], start: i64, end: i64) {\n"
        "        self.origin = origin;\n"
        "        self.start = start;\n"
        "        self.end = end;\n"
        "    }\n"
        "    func length(): i64 { return self.end - self.start; }\n"
        "    func get(index: i64): T { return self.origin[self.start + index]; }\n"
        "}\n"
        "fit T[] {\n"
        "    func value_slice(start: i64, end: i64): ValueSpan<T> {\n"
        "        return ValueSpan<T>(self, start, end);\n"
        "    }\n"
        "    func value_slice(start: i64): ValueSpan<T> {\n"
        "        return self.value_slice(start, (i64)4);\n"
        "    }\n"
        "}\n"
        "func run(): int {\n"
        "    let values: int[] = [1, 2, 3, 4];\n"
        "    let middle: ValueSpan<int> = values.value_slice((i64)1, (i64)3);\n"
        "    let tail: ValueSpan<int> = values.value_slice((i64)1);\n"
        "    return middle.get((i64)0) + (int)middle.length() + "
        "tail.get((i64)0) + (int)tail.length();\n"
        "}\n";

    FengProgram *program = parse_or_die(
        kSource, "tests/fit_builtin_generic_value_array_codegen.ff");
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
        fprintf(stderr, "codegen error (fit builtin generic @value array): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "void FengFitBuiltin__feng__codegen__fit_builtin_generic_value_array") != NULL);
    ASSERT(strstr(out.c_source, "void *_out") != NULL);
    ASSERT(strstr(out.c_source, ".reified_agg_deps_count = ") != NULL);
    ASSERT(strstr(out.c_source,
                  "ValueSpan__G__i64__aggregate_desc") != NULL);
    ASSERT(strstr(out.c_source,
                  "ValueSpan__G__T)(FengFitBuiltin") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_builtin_and_array_object_spec_coercion_codegen(void) {
    static const char *kSource =
        "module feng.codegen.fit_builtin_spec;\n"
        "spec Named { func name(): string; }\n"
        "spec ScalarTwice { func twice_only_scalar(): i32; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "fit i32: Named {\n"
        "    func name(): string {\n"
        "        return \"i32\";\n"
        "    }\n"
        "}\n"
        "fit i32: ScalarTwice {\n"
        "    func twice_only_scalar(): i32 {\n"
        "        return self + self;\n"
        "    }\n"
        "}\n"
        "fit string: Named {\n"
        "    func name(): string {\n"
        "        return self;\n"
        "    }\n"
        "}\n"
        "fit int[]: Named {\n"
        "    func name(): string {\n"
        "        return \"arr\";\n"
        "    }\n"
        "}\n"
        "func call_name(v: Named): string {\n"
        "    return v.name();\n"
        "}\n"
        "func call_twice_direct(v: i32): i32 {\n"
        "    return v.twice_only_scalar();\n"
        "}\n"
        "func call_twice_spec(v: ScalarTwice): i32 {\n"
        "    return v.twice_only_scalar();\n"
        "}\n"
        "func make_scalar_named(): Named {\n"
        "    return ((i32)8);\n"
        "}\n"
        "func run(): int {\n"
        "    let xs: int[] = [1, 2];\n"
        "    let u: User = User{n: \"u\"};\n"
        "    let s1: Named = ((i32)7);\n"
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
        "    let f: string = call_name(((i32)9));\n"
        "    let t1: i32 = call_twice_direct(5);\n"
        "    let t2: i32 = call_twice_spec(((i32)5));\n"
        "    return (int)(t1 + t2);\n"
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
    ASSERT(count_substr(out.c_source, "struct FengScalarBox *_sb") == 4U);
    ASSERT(strstr(out.c_source, ".witness->name(") != NULL);
    ASSERT(strstr(out.c_source, "witness->witness->") == NULL);
    ASSERT(strstr(out.c_source, "static const FengTypeDescriptor feng_scalar_box_descriptor") == NULL);
    ASSERT(strstr(out.c_source, "struct FengScalarBox {") == NULL);
    ASSERT(count_substr(out.c_source, "FENG_SLOT_POINTER") >= 1U);
    ASSERT(count_substr(out.c_source, ".managed_slot_count = 1,") >= 1U);
    ASSERT(count_substr(out.c_source, "__twice_only_scalar(") >= 3U);
    ASSERT(strstr(out.c_source, "twice_only_scalar_box") == NULL);
    ASSERT(strstr(out.c_source, "subject_") != NULL);
    ASSERT(strstr(out.c_source, "_self_value = *(const int32_t *)_subject;") == NULL);
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
        "module feng.codegen.fit_enum_spec;\n"
        "spec Named { func code(): int; }\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status: Named {\n"
        "    func code(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "}\n"
        "func call_named(value: Named): int {\n"
        "    return value.code();\n"
        "}\n"
        "func run(): int {\n"
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
    ASSERT(strstr(out.c_source, "_self_value = *(const int32_t *)_subject;") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* 9.6 — verify the merged witness struct for an intersection-form spec is
 * emitted with one slot per (deduped) member-spec method. The struct name
 * follows the FengSpecWitness__<module>__<name> convention shared with
 * object-form specs. Member access on intersection-form values is 9.7
 * territory; here we only need a coercion site to register the spec and
 * trigger struct emission. */
static void test_intersection_spec_witness_struct_codegen(void) {
    static const char *kSource =
        "module feng.codegen.intersection_witness_struct;\n"
        "spec Greetable {\n"
        "    func greet(): string;\n"
        "}\n"
        "spec Displayable {\n"
        "    func display(): string;\n"
        "}\n"
        "spec Both: Greetable & Displayable;\n"
        "type User: Greetable, Displayable {\n"
        "    var name: string;\n"
        "    func greet(): string { return self.name; }\n"
        "    func display(): string { return self.name; }\n"
        "}\n"
        "func run(): int {\n"
        "    let u: User = User { name: \"hi\" };\n"
        "    let b: Both = u;\n"
        "    return (int)0;\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "intersection_witness_struct.ff");
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
                    errors[i].message ? errors[i].message : "(unknown)");
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (intersection witness struct): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Merged witness struct declaration with both member methods. */
    ASSERT(strstr(out.c_source, "struct FengSpecWitness__feng__codegen__intersection_witness_struct__Both {\n") != NULL);
    ASSERT(strstr(out.c_source, "(*greet)(void *_subject)") != NULL);
    ASSERT(strstr(out.c_source, "(*display)(void *_subject)") != NULL);
    /* Value struct follows the object-form layout: { subject, witness }. */
    ASSERT(strstr(out.c_source,
        "struct FengSpecValue__feng__codegen__intersection_witness_struct__Both { "
        "void *subject; const struct FengSpecWitness__feng__codegen__intersection_witness_struct__Both *witness; }") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* 9.6 — verify that a coercion site (`let b: Both = u;`) triggers emission
 * of a merged witness constant that aliases slots from X's per-member-spec
 * witnesses (no thunks — direct field aliasing). */
static void test_intersection_spec_witness_instance_codegen(void) {
    static const char *kSource =
        "module feng.codegen.intersection_witness_instance;\n"
        "spec Greetable {\n"
        "    func greet(): string;\n"
        "}\n"
        "spec Displayable {\n"
        "    func display(): string;\n"
        "}\n"
        "spec Both: Greetable & Displayable;\n"
        "type User: Greetable, Displayable {\n"
        "    var name: string;\n"
        "    func greet(): string { return self.name; }\n"
        "    func display(): string { return self.name; }\n"
        "}\n"
        "func run(): int {\n"
        "    let u: User = User { name: \"hi\" };\n"
        "    let b: Both = u;\n"
        "    return (int)0;\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "intersection_witness_instance.ff");
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
                    errors[i].message ? errors[i].message : "(unknown)");
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (intersection witness instance): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Merged witness constant for (User, Both) — direct slot aliasing, no
     * thunk functions emitted for the intersection. The merged witness var
     * carries the FengSpecWitness__ prefix; per-member witnesses use the
     * FengWitness__ prefix (no "Spec") matching the object-form convention. */
    ASSERT(strstr(out.c_source, "static const struct FengSpecWitness__feng__codegen__intersection_witness_instance__Both "
                                "FengSpecWitness__feng__codegen__intersection_witness_instance__User__as__"
                                "feng__codegen__intersection_witness_instance__Both = {") != NULL);
    /* Slots alias directly from User's Greetable/Displayable witnesses. */
    ASSERT(strstr(out.c_source, ".greet = FengWitness__feng__codegen__intersection_witness_instance__User__as__"
                                "feng__codegen__intersection_witness_instance__Greetable.greet") != NULL);
    ASSERT(strstr(out.c_source, ".display = FengWitness__feng__codegen__intersection_witness_instance__User__as__"
                                 "feng__codegen__intersection_witness_instance__Displayable.display") != NULL);
    /* Coercion site constructs the fat value { .subject, .witness }. */
    ASSERT(strstr(out.c_source, ".witness = &FengSpecWitness__feng__codegen__intersection_witness_instance__User__as__"
                                "feng__codegen__intersection_witness_instance__Both") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* 9.6 — verify the default-zero path for an intersection spec: an
 * uninitialized `let b: Both;` materialises a default subject and routes
 * method calls through the default witness (returning default-zero values
 * of the return type). Mirrors the object-form default-zero flow. Member
 * access on the resulting value is 9.7; here we only verify the default-
 * zero infrastructure (subject struct, descriptor, factory, default
 * witness, aggregate init fn) is emitted. */
static void test_intersection_spec_default_zero_codegen(void) {
    static const char *kSource =
        "module feng.codegen.intersection_default_zero;\n"
        "spec Greetable {\n"
        "    func greet(): string;\n"
        "}\n"
        "spec Displayable {\n"
        "    func display(): string;\n"
        "}\n"
        "spec Both: Greetable & Displayable;\n"
        "func run(): int {\n"
        "    let b: Both;\n"
        "    return (int)0;\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "intersection_default_zero.ff");
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
                    errors[i].message ? errors[i].message : "(unknown)");
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (intersection default-zero): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Default subject struct + descriptor + factory for the intersection. */
    ASSERT(strstr(out.c_source, "struct FengSpecDefault__feng__codegen__intersection_default_zero__Both__Subject {") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecDefault__feng__codegen__intersection_default_zero__Both__Subject_desc") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecDefault__feng__codegen__intersection_default_zero__Both__new_subject") != NULL);
    /* Default witness with greet/display thunks returning default-zero values. */
    ASSERT(strstr(out.c_source, "FengSpecDefaultWitness__feng__codegen__intersection_default_zero__Both") != NULL);
    /* Aggregate init fn uses the default subject factory + default witness. */
    ASSERT(strstr(out.c_source, "FengSpecAggInit__feng__codegen__intersection_default_zero__Both") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* 9.6 — verify generic intersection: `spec Comparable<T>: Eq<T> & Ord<T>;`
 * produces a merged witness struct + instance for `Comparable<User>` where
 * User satisfies Eq<User> and Ord<User>. Member access on the coerced value
 * is 9.7; the test only needs the coercion site to register the generic
 * instance and trigger merged witness emission. */
static void test_generic_intersection_spec_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_intersection;\n"
        "spec Eq<T> {\n"
        "    func equals(other: T): bool;\n"
        "}\n"
        "spec Ord<T> {\n"
        "    func compare(other: T): int;\n"
        "}\n"
        "spec Comparable<T>: Eq<T> & Ord<T>;\n"
        "type User: Eq<User>, Ord<User> {\n"
        "    let id: int;\n"
        "    func equals(other: User): bool { return self.id == other.id; }\n"
        "    func compare(other: User): int { return self.id - other.id; }\n"
        "}\n"
        "func run(): int {\n"
        "    let u: User = User { id: 1 };\n"
        "    let c: Comparable<User> = u;\n"
        "    return (int)0;\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "generic_intersection.ff");
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
                    errors[i].message ? errors[i].message : "(unknown)");
        }
        ASSERT(ok);
    }
    ASSERT(error_count == 0U);

    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic intersection): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    /* Generic-context merged witness struct carries the slot signatures
     * with the bound type arg (User). The symbol mangling includes the
     * type-arg encoding (`__G__User` style). */
    ASSERT(strstr(out.c_source, "struct FengSpecWitness__feng__codegen__generic_intersection__Comparable") != NULL);
    ASSERT(strstr(out.c_source, "(*equals)(void *_subject") != NULL);
    ASSERT(strstr(out.c_source, "(*compare)(void *_subject") != NULL);
    /* Merged witness constant aliases slots from User's Eq<User> and
     * Ord<User> witnesses. Per-member witnesses use the FengWitness__
     * prefix (no "Spec"). */
    ASSERT(strstr(out.c_source, ".equals = FengWitness__feng__codegen__generic_intersection__User__as__") != NULL);
    ASSERT(strstr(out.c_source, ".compare = FengWitness__feng__codegen__generic_intersection__User__as__") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_object_spec_thunk_subject_cast_shape_codegen(void) {
    static const char *kSource =
        "module feng.codegen.object_spec_cast;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    var n: string;\n"
        "    func name(): string { return self.n; }\n"
        "}\n"
        "func call(v: Named): string {\n"
        "    return v.name();\n"
        "}\n"
        "func run(): string {\n"
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
    "module feng.codegen.gf1;\n"
    "open func identity<T>(x: T): T { return x; }\n";

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
    "module feng.codegen.gf2;\n"
    "open type Box<T> { open let value: int; }\n";

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
    "module feng.codegen.gf3;\n"
    "func identity<T>(x: T): T { return x; }\n"
    "func use_it() { let result = identity(42); }\n";

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

static const char *kGenericManagedReturnLetBindingSrc =
    "module feng.codegen.gf_managed_ret;\n"
    "type Widget {\n"
    "    let value: int;\n"
    "}\n"
    "func make_widget<T>(): Widget {\n"
    "    return Widget { value: 7 };\n"
    "}\n"
    "var result: int;\n"
    "func main(args: string[]) {\n"
    "    let w: Widget = make_widget<int>();\n"
    "    result = w.value;\n"
    "}\n";

/* Regression: a generic function returning a managed (refcounted) type and
 * bound to a `let` local must not double-release the returned object.
 *
 * The +1 refcount from the call is owned by the out-param temp (_grN),
 * which is registered in the cleanup chain.  The `let` binding aliases
 * that pointer, so it must take its own +1 via feng_retain — otherwise
 * both the temp's cleanup and the binding's cleanup would release the
 * same +1, producing a use-after-free at scope exit.
 *
 * This invariant mirrors the existing contract for aggregate (spec fat
 * value) returns from generic calls. */
static void test_generic_managed_return_let_binding_codegen(void) {
    FengProgram *program = parse_or_die(kGenericManagedReturnLetBindingSrc,
                                        "gf_managed_ret.ff");
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
        fprintf(stderr, "codegen error (generic managed return): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);

    /* The let binding must take its own +1 refcount via feng_retain.
     * Without it, the binding aliases the out-param temp's pointer and
     * both cleanups release the same +1. */
    ASSERT(strstr(out.c_source, "feng_retain(_") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericSpecArgSrc =
    "module feng.codegen.gf4;\n"
    "spec Spec1 {}\n"
    "type User: Spec1 {}\n"
    "type MyType<T, V> {\n"
    "    let value: V;\n"
    "\n"
    "    func test(t: T): V {\n"
    "        return self.value;\n"
    "    }\n"
    "}\n"
    "\n"
    "func use_it() {\n"
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
    "module feng.codegen.gf5;\n"
    "spec Named {\n"
    "    var name: string;\n"
    "    func greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    var name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "func rename<T: Named>(user: T, next: string): string {\n"
    "    user.name = next;\n"
    "    return user.name;\n"
    "}\n"
    "func greet_generic<T: Named>(user: T): string {\n"
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
    "module feng.codegen.gf9;\n"
    "spec Named {\n"
    "    func name(): string;\n"
    "}\n"
    "spec Mapper(x: int): int;\n"
    "enum Status {\n"
    "    Ok = 200,\n"
    "    Failed = 500\n"
    "}\n"
    "type User: Named {\n"
    "    func name(): string {\n"
    "        return \"user\";\n"
    "    }\n"
    "}\n"
    "func add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "func identity<T>(value: T): T {\n"
    "    return value;\n"
    "}\n"
    "func use_it(values: int[]): int {\n"
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
                  ".kind = FENG_VALUE_TRIVIAL, .descriptor = &feng_bool_descriptor") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_TRIVIAL, .descriptor = &FengEnumDesc__feng__codegen__gf9__Status") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .descriptor = &feng_string_descriptor") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .descriptor = &feng_array_descriptor") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .descriptor = &FengTypeDesc__feng__codegen__gf9__User") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_TRIVIAL, .descriptor = &feng_pointer_descriptor") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, .descriptor = &FengSpecAgg__feng__codegen__gf9__Named") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .descriptor = &FengClosureDesc__feng__codegen__gf9__Mapper") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericAggregateFactsShapeSrc =
    "module feng.codegen.gfaggshape;\n"
    "spec Named {\n"
    "    func name(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    func name(): string {\n"
    "        return \"user\";\n"
    "    }\n"
    "}\n"
    "func identity<T>(value: T): T {\n"
    "    return value;\n"
    "}\n"
    "func use_it(value: int, text: string): int {\n"
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
    {
        /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
        const char *int_descriptor = sizeof(void *) >= 8U ? "feng_i64_descriptor" : "feng_i32_descriptor";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 ".kind = FENG_VALUE_TRIVIAL, .descriptor = &%s", int_descriptor);
        ASSERT(strstr(out.c_source, expected) != NULL);
    }
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER, .descriptor = &feng_string_descriptor") != NULL);
    ASSERT(strstr(out.c_source,
                  ".kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, .descriptor = &FengSpecAgg__feng__codegen__gfaggshape__Named") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_fit_enum_generic_constraint_codegen(void) {
    static const char *kSource =
        "module feng.codegen.fit_enum_generic;\n"
        "spec Hashable<T> {\n"
        "    func hash(): int;\n"
        "    func same(other: T): bool;\n"
        "}\n"
        "enum Status {\n"
        "    Ok,\n"
        "    Failed\n"
        "}\n"
        "fit Status: Hashable<Status> {\n"
        "    func hash(): int {\n"
        "        return (int)self;\n"
        "    }\n"
        "    func same(other: Status): bool {\n"
        "        return self == other;\n"
        "    }\n"
        "}\n"
        "func use_hash<K: Hashable<K>>(value: K): int {\n"
        "    return value.hash();\n"
        "}\n"
        "func use_same<K: Hashable<K>>(left: K, right: K): bool {\n"
        "    return left.same(right);\n"
        "}\n"
        "func run(): int {\n"
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

static void test_generic_user_fit_object_spec_coercion_codegen(void) {
    static const char *kSource =
        "module feng.codegen.fit_generic_user;\n"
        "spec Readable<T> {\n"
        "    func read(): T;\n"
        "}\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "}\n"
        "fit Box<T>: Readable<T> {\n"
        "    func read(): T {\n"
        "        return self.value;\n"
        "    }\n"
        "}\n"
        "func read_i(value: Readable<int>): int {\n"
        "    return value.read();\n"
        "}\n"
        "func run(): int {\n"
        "    let box: Box<int> = Box<int>();\n"
        "    box.value = 42;\n"
        "    return read_i(box);\n"
        "}\n";

    FengProgram *program = parse_or_die(kSource, "tests/fit_generic_user_codegen.ff");
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
        fprintf(stderr, "codegen error (generic user fit object spec): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }

    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecThunk__") != NULL);
    ASSERT(strstr(out.c_source, "__read") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecTopLevelFnSrc =
    "module feng.codegen.cb1;\n"
    "spec Mapper(x: int): int;\n"
    "func add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "func use_it(): int {\n"
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
    "module feng.codegen.cb2;\n"
    "spec Mapper(x: int): int;\n"
    "func add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "func apply<T: Mapper>(mapper: T, value: int): int {\n"
    "    return mapper(value);\n"
    "}\n"
    "func use_it(): int {\n"
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
    "module feng.codegen.gs1;\n"
    "spec Box<T> {\n"
    "    func fetch(): T;\n"
    "}\n"
    "func read_it(box: Box<int>): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char box_pattern[128];
        snprintf(box_pattern, sizeof(box_pattern),
                 "FengSpecValue__feng__codegen__gs1__Box__G__%s", int_canonical);
        ASSERT(strstr(out.c_source, box_pattern) != NULL);
    }
    ASSERT(strstr(out.c_source, "->fetch(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericCallableSpecInstanceSrc =
    "module feng.codegen.gs2;\n"
    "spec Mapper<T>(x: T): T;\n"
    "func apply(mapper: Mapper<int>): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[128];
        snprintf(pattern, sizeof(pattern),
                 "FengClosure__feng__codegen__gs2__Mapper__G__%s", int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
    }
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericObjectSpecCoercionSrc =
    "module feng.codegen.gs3;\n"
    "spec Box<T> {\n"
    "    func fetch(): T;\n"
    "}\n"
    "type IntBox: Box<int> {\n"
    "    func fetch(): int {\n"
    "        return 7;\n"
    "    }\n"
    "}\n"
    "func use_it(): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[256];
        snprintf(pattern, sizeof(pattern),
                 "FengSpecValue__feng__codegen__gs3__Box__G__%s", int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
        snprintf(pattern, sizeof(pattern),
                 "FengWitness__feng__codegen__gs3__IntBox__as__feng__codegen__gs3__Box_%s_",
                 int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
    }
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericCallableSpecCoercionSrc =
    "module feng.codegen.gs4;\n"
    "spec Mapper<T>(x: T): T;\n"
    "func add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "func use_it(): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[256];
        snprintf(pattern, sizeof(pattern),
                 "FengClosure__feng__codegen__gs4__Mapper__G__%s", int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
        snprintf(pattern, sizeof(pattern),
                 "FengCallableValue__FengClosure__feng__codegen__gs4__Mapper__G__%s",
                 int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
    }
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecMethodCoercionSrc =
    "module feng.codegen.gs5;\n"
    "spec Mapper<T>(x: T): T;\n"
    "type Adder {\n"
    "    func add1(x: int): int {\n"
    "        return x + 1;\n"
    "    }\n"
    "}\n"
    "func use_it(): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[256];
        snprintf(pattern, sizeof(pattern),
                 "FengCallableBind__FengClosure__feng__codegen__gs5__Mapper__G__%s",
                 int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
        snprintf(pattern, sizeof(pattern),
                 "feng_object_new(&FengClosureDesc__feng__codegen__gs5__Mapper__G__%s)",
                 int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
    }
    ASSERT(strstr(out.c_source, "feng_assign(&_o->_self, (void *)_self)") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kCallableSpecLambdaLocalCaptureSrc =
    "module feng.codegen.gs6;\n"
    "spec Mapper(x: int): int;\n"
    "func use_it(): int {\n"
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
    "module feng.codegen.gs7;\n"
    "spec Reader(): int;\n"
    "type Box {\n"
    "    var n: int;\n"
    "    func read(): int {\n"
    "        let reader: Reader = () -> self.n;\n"
    "        return reader();\n"
    "    }\n"
    "}\n"
    "func use_it(): int {\n"
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
    "module feng.codegen.gs8;\n"
    "spec Mapper(x: int): int;\n"
    "func apply(mapper: Mapper): int {\n"
    "    return mapper(41);\n"
    "}\n"
    "func use_it(): int {\n"
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
    "module feng.codegen.gs8;\n"
    "spec MapperA(x: int): int;\n"
    "spec MapperB(x: int): int;\n"
    "func add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "func use_it(input: MapperA): int {\n"
    "    let local: MapperA = input;\n"
    "    let remapped: MapperB = (MapperB)local;\n"
    "    return remapped(41);\n"
    "}\n"
    "func entry(): int {\n"
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
    "module feng.codegen.gs9;\n"
    "spec MapperA(x: int): int;\n"
    "spec MapperB(x: int): int;\n"
    "func add1(x: int): int {\n"
    "    return x + 1;\n"
    "}\n"
    "type Holder {\n"
    "    let mapper: MapperA;\n"
    "}\n"
    "func use_it(input: MapperA): int {\n"
    "    let local: MapperA = input;\n"
    "    let holder: Holder = Holder{mapper: local};\n"
    "    let from_field: MapperA = holder.mapper;\n"
    "    let remapped: MapperB = (MapperB)from_field;\n"
    "    return remapped(41);\n"
    "}\n"
    "func entry(): int {\n"
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
    "module feng.codegen.gf7;\n"
    "spec Named {\n"
    "    var name: string;\n"
    "    func greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    var name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "func rename<T: Named>(user: T, next: string): string {\n"
    "    user.name = next;\n"
    "    return user.greet();\n"
    "}\n"
    "func use_it(): string {\n"
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
    "module feng.codegen.sfagg1;\n"
    "spec Named {\n"
    "    func greet(): string;\n"
    "}\n"
    "spec HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "type Holder: HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "func read_child(box: HasChild): string {\n"
    "    return box.child.greet();\n"
    "}\n"
    "func write_child(box: HasChild, child: Named) {\n"
    "    box.child = child;\n"
    "}\n"
    "func use_it(): string {\n"
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
    "module feng.codegen.gf9;\n"
    "spec Named {\n"
    "    func greet(): string;\n"
    "}\n"
    "spec HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "type Holder: HasChild {\n"
    "    var child: Named;\n"
    "}\n"
    "func rewrite<T: HasChild>(box: T, next: Named): string {\n"
    "    box.child = next;\n"
    "    return box.child.greet();\n"
    "}\n"
    "func use_it(): string {\n"
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
    "module feng.codegen.ifagg1;\n"
    "spec Named {\n"
    "    func greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "func pick(flag: bool, left: Named, right: Named): Named {\n"
    "    return if flag {\n"
    "        left;\n"
    "    } else {\n"
    "        right;\n"
    "    };\n"
    "}\n"
    "func use_it(): string {\n"
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
    "module feng.codegen.matchagg1;\n"
    "spec Named {\n"
    "    func greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "func pick(tag: i32, left: Named, right: Named): Named {\n"
    "    return match tag {\n"
    "        0 { left; }\n"
    "        else { right; }\n"
    "    };\n"
    "}\n"
    "func use_it(): string {\n"
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
    "module feng.codegen.matchstmt1;\n"
    "func classify(age: i32, label: string): i32 {\n"
    "    var result: i32 = 0;\n"
    "    match age {\n"
    "        0 { result = 10; }\n"
    "        1...3 { result = 20; }\n"
    "        4, 5 { result = 30; }\n"
    "        else { result = 40; }\n"
    "    }\n"
    "    match label {\n"
    "        \"ok\" { result = result + 1; }\n"
    "        else { result = result + 2; }\n"
    "    }\n"
    "    return result;\n"
    "}\n";

static void test_match_statement_codegen(void) {
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
        fprintf(stderr, "codegen error (match statement): %s\n",
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

static const char *kEnumMatchStatementCodegenSrc =
    "module feng.codegen.enummatchstmt;\n"
    "enum Color { Red, Green, Blue }\n"
    "func classify(c: Color): i32 {\n"
    "    var result: i32 = 0;\n"
    "    match c {\n"
    "        Color.Red { result = 10; }\n"
    "        Color.Green, Color.Blue { result = 20; }\n"
    "        else { result = 30; }\n"
    "    }\n"
    "    return result;\n"
    "}\n";

static void test_enum_match_statement_codegen(void) {
    FengProgram *program = parse_or_die(kEnumMatchStatementCodegenSrc, "enummatchstmt.ff");
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
        fprintf(stderr, "codegen error (enum match statement): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* The enum item reference label lowers to a C constant comparison
     * against `EnumTypedef__ItemName`. Both branches must reference the
     * same enum typedef prefix. */
    ASSERT(strstr(out.c_source, "Color__Red") != NULL);
    ASSERT(strstr(out.c_source, "Color__Green") != NULL);
    ASSERT(strstr(out.c_source, "Color__Blue") != NULL);
    ASSERT(strstr(out.c_source, "(bool)(_mt") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kEnumMatchExprCodegenSrc =
    "module feng.codegen.enummatchexpr;\n"
    "enum Color { Red, Green, Blue }\n"
    "func classify(c: Color): i32 {\n"
    "    return match c {\n"
    "        Color.Red { 10; }\n"
    "        Color.Green { 20; }\n"
    "        else { 30; }\n"
    "    };\n"
    "}\n";

static void test_enum_match_expression_codegen(void) {
    FengProgram *program = parse_or_die(kEnumMatchExprCodegenSrc, "enummatchexpr.ff");
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
        fprintf(stderr, "codegen error (enum match expression): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "Color__Red") != NULL);
    ASSERT(strstr(out.c_source, "Color__Green") != NULL);
    ASSERT(strstr(out.c_source, "(bool)(_mt") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericAggregateReturnSrc =
    "module feng.codegen.gf6;\n"
    "spec Named {\n"
    "    func greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "func make_named<T>(name: string): Named {\n"
    "    let user: User = User{name: name};\n"
    "    return user;\n"
    "}\n"
    "func forward_named<T>(named: Named): Named {\n"
    "    return named;\n"
    "}\n"
    "func rebound_named<T>(name: string): Named {\n"
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
    ASSERT(strstr(out.c_source, "feng_aggregate_retain(&") != NULL);
    ASSERT(strstr(out.c_source, "memcpy(_out, &") != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static const char *kGenericTypeGenericMethodSrc =
    "module feng.codegen.gf8;\n"
    "type Box<T> {\n"
    "    var value: T;\n"
    "    func echo<U>(value: U): U {\n"
    "        return value;\n"
    "    }\n"
    "    func replace<U>(next: T, result: U): U {\n"
    "        self.value = next;\n"
    "        return result;\n"
    "    }\n"
    "    func current(): T {\n"
    "        return self.value;\n"
    "    }\n"
    "}\n"
    "func use_it(): int {\n"
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
    "module feng.codegen.gd13;\n"
    "type Set<T> {\n"
    "    var value: T;\n"
    "    func put(next: T) {\n"
    "        self.value = next;\n"
    "    }\n"
    "    func get(): T {\n"
    "        return self.value;\n"
    "    }\n"
    "}\n"
    "func use_it(): int {\n"
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
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[256];
        snprintf(pattern, sizeof(pattern),
                 "Feng__feng__codegen__gd13__Set__G__%s__put__from__%s",
                 int_canonical, int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
        snprintf(pattern, sizeof(pattern),
                 "Feng__feng__codegen__gd13__Set__G__%s__put__from__%s(_l_set_0",
                 int_canonical, int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
        snprintf(pattern, sizeof(pattern),
                 "Feng__feng__codegen__gd13__Set__G__%s__get__from__void(_l_set_0",
                 int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
    }
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
    "module feng.codegen.ge1;\n"
    "spec Named {\n"
    "    func greet(): string;\n"
    "}\n"
    "type User: Named {\n"
    "    let name: string;\n"
    "    func greet(): string {\n"
    "        return self.name;\n"
    "    }\n"
    "}\n"
    "func idNamed<T: Named>(value: T): T {\n"
    "    return value;\n"
    "}\n"
    "type Holder<T: Named> {\n"
    "    var value: T;\n"
    "    func set(next: T) {\n"
    "        self.value = next;\n"
    "    }\n"
    "    func read(): string {\n"
    "        return self.value.greet();\n"
    "    }\n"
    "    func relay<U: Named>(item: U): string {\n"
    "        return item.greet();\n"
    "    }\n"
    "}\n"
    "func use_it(input: Named): string {\n"
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
    "module feng.codegen.ctor1;\n"
    "type UserType {\n"
    "    var id: int;\n"
    "    let name: string;\n"
    "    func UserType() {\n"
    "        self.id = 1;\n"
    "    }\n"
    "    func UserType(next: int) {\n"
    "        self.id = next;\n"
    "    }\n"
    "}\n"
    "func use_all(): int {\n"
    "    let a: UserType = UserType() { id: 11 };\n"
    "    let b: UserType = UserType { id: 12 };\n"
    "    let c: UserType = UserType();\n"
    "    let d: UserType = UserType(7) { id: 13 };\n"
    "    let e: UserType = UserType(9);\n"
    "    return a.id + b.id + c.id + d.id + e.id;\n"
    "}\n";

static void test_empty_array_literal_codegen_uses_target_contexts(void) {
    static const char *kSource =
        "module feng.codegen.emptyarray;\n"
        "type InitializedHolder {\n"
        "    let values: int[] = [];\n"
        "}\n"
        "type LiteralHolder {\n"
        "    var values: int[];\n"
        "}\n"
        "func take(values: int[]): int {\n"
        "    return 1;\n"
        "}\n"
        "func generic_take<T>(values: T[]): int {\n"
        "    return 1;\n"
        "}\n"
        "func run(): int {\n"
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
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "feng_array_new(NULL, sizeof(%s), false, (size_t)0)", int_c_type);
        ASSERT(count_substr(out.c_source, expected) >= 4U);
    }
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
        "module feng.codegen.fieldinit;\n"
        "type UserType {\n"
        "    let id: int = 7;\n"
        "}\n"
        "type User {\n"
        "    let id: int = 0;\n"
        "    let x: UserType;\n"
        "    let y = UserType();\n"
        "    let z = UserType {};\n"
        "}\n"
        "func total(): int {\n"
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
        "module feng.codegen.fieldlambda;\n"
        "spec Reader(): int;\n"
        "type Box {\n"
        "    var n: int;\n"
        "    let read: Reader = () -> self.n;\n"
        "}\n"
        "func use_it(): int {\n"
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

static void test_callable_field_default_and_explicit_initialization_codegen(void) {
    static const char *kSource =
        "module feng.codegen.callablefielddefault;\n"
        "spec Action(value: int): void;\n"
        "func ignore(value: int): void {}\n"
        "type DefaultBox {\n"
        "    let action: Action;\n"
        "}\n"
        "type ExplicitBox {\n"
        "    let action: Action = ignore;\n"
        "}\n"
        "func use_it(): void {\n"
        "    let defaultBox = DefaultBox();\n"
        "    let explicitBox = ExplicitBox();\n"
        "    defaultBox.action(1);\n"
        "    explicitBox.action(2);\n"
        "}\n";
    static const char *kDefaultFactoryCall =
        "FengCallableDefault__feng__codegen__callablefielddefault__Action__new()";
    FengProgram *program = parse_or_die(
        kSource,
        "tests/callable_field_default_and_explicit_initialization.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    const char *use_body;
    const char *use_body_end;
    const char *default_call;

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

    use_body = strstr(out.c_source,
                      "static void feng__feng__codegen__callablefielddefault__"
                      "use_it__from__void(void) {");
    ASSERT(use_body != NULL);
    use_body_end = strstr(use_body, "\n}\n\n");
    ASSERT(use_body_end != NULL);
    default_call = strstr(use_body, kDefaultFactoryCall);
    ASSERT(default_call != NULL);
    ASSERT(default_call < use_body_end);
    default_call = strstr(default_call + strlen(kDefaultFactoryCall),
                          kDefaultFactoryCall);
    ASSERT(default_call == NULL || default_call >= use_body_end);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_void_try_expression_codegen(void) {
    static const char *kSource =
        "module feng.codegen.tryvoid;\n"
        "func fail() {\n"
        "    throw \"boom\";\n"
        "}\n"
        "func noop() {\n"
        "}\n"
        "func make_value(): i32 {\n"
        "    return 7;\n"
        "}\n"
        "func fail_i32(): i32 {\n"
        "    let payload: i32 = 1;\n"
        "    throw payload;\n"
        "    return 0;\n"
        "}\n"
        "func run_case() {\n"
        "    try fail_i32() catch ex: i32 {\n"
        "    };\n"
        "    try noop() catch {};\n"
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
    ASSERT(strstr(out.c_source, "FengExceptionFrame") == NULL);
    ASSERT(strstr(out.c_source, "setjmp") == NULL);
    ASSERT(strstr(out.c_source, "FengLSDA") != NULL);
    ASSERT(strstr(out.c_source, "feng_try_frame_push") != NULL);
    ASSERT(strstr(out.c_source, "_try_keep_lpad") != NULL);
    ASSERT(strstr(out.c_source,
                  ".cfi_personality 155, ___feng_personality_v0") != NULL);
    ASSERT(strstr(out.c_source,
                  ".cfi_personality 27, __feng_personality_v0") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_try_catch_return_codegen(void) {
    static const char *kSource =
        "module feng.codegen.tryreturn;\n"
        "func fail(): string {\n"
        "    throw \"boom\";\n"
        "    return \"unreachable\";\n"
        "}\n"
        "func catch_returns(): string {\n"
        "    let value = try fail() catch ex: string {\n"
        "        return \"catch-return\";\n"
        "    };\n"
        "    return value;\n"
        "}\n"
        "func normal_value(): string {\n"
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
    ASSERT(strstr(out.c_source, "feng_exception_pop();") == NULL);
    ASSERT(strstr(out.c_source, "setjmp") == NULL);
    ASSERT(strstr(out.c_source, "switch (feng_caught_clause())") == NULL);
    ASSERT(strstr(out.c_source, "int _try_clause_") != NULL);
    ASSERT(strstr(out.c_source, "feng_frame_pop();") != NULL);
    ASSERT(strstr(out.c_source, "feng_release_unwind_exception();") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* T2: variadic function called with zero variadic arguments — the compiler
 * must emit feng_array_new(..., 0) for the implicit empty array. */
static void test_variadic_zero_args_codegen(void) {
    static const char *kSource =
        "module feng.codegen.variadic_zero;\n"
        "func sum(values: int...): int {\n"
        "    return 0;\n"
        "}\n"
        "func run(): int {\n"
        "    return sum();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/variadic_zero.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (variadic zero args): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* An empty-variadic call must produce a zero-length array allocation. */
    ASSERT(strstr(out.c_source, "(size_t)0") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* T3+T8: variadic function called with multiple arguments — the compiler must
 * emit feng_array_new with the correct count and the generated C must compile. */
static void test_variadic_multi_args_codegen(void) {
    static const char *kSource =
        "module feng.codegen.variadic_multi;\n"
        "func sum(values: int...): int {\n"
        "    return 0;\n"
        "}\n"
        "func run(): int {\n"
        "    return sum(1, 2, 3);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/variadic_multi.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (variadic multi args): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* Three variadic args → array allocation with count 3. */
    ASSERT(strstr(out.c_source, "(size_t)3") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* T2: fixed parameters stay positional and only the variadic suffix is packed. */
static void test_variadic_fixed_prefix_codegen(void) {
    static const char *kSource =
        "module feng.codegen.variadic_fixed_prefix;\n"
        "func log(level: int, args: string...): int {\n"
        "    return level;\n"
        "}\n"
        "func run(): int {\n"
        "    return log(1, \"a\", \"b\");\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/variadic_fixed_prefix.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (variadic fixed prefix): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "(size_t)2") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* T6/C2: calling a variadic callable-form spec value must also pack variadic arguments. */
static void test_variadic_callable_spec_lambda_codegen(void) {
    static const char *kSource =
        "module feng.codegen.variadic_spec_lambda;\n"
        "spec Mapper(args: int...): int;\n"
        "func run(): int {\n"
        "    let mapper: Mapper = (args: int...) {\n"
        "        return 0;\n"
        "    };\n"
        "    return mapper(1, 2);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/variadic_spec_lambda.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (variadic callable spec lambda): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengLambda__") != NULL);
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    ASSERT(strstr(out.c_source, "(size_t)2") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_tuple_value_codegen_core(void) {
    static const char *kSource =
        "module feng.codegen.tuplecore;\n"
        "type Unit();\n"
        "type OtherUnit();\n"
        "type Point(int, int);\n"
        "type Coord(int, int);\n"
        "type Pair<T, U>(T, U);\n"
        "type Holder {\n"
        "    let point: Point;\n"
        "    let unit: Unit;\n"
        "}\n"
        "func make_unit(): Unit {\n"
        "    return ();\n"
        "}\n"
        "func unit_score(value: Unit): int {\n"
        "    let () = value;\n"
        "    return 1;\n"
        "}\n"
        "func cast_unit(value: Unit): OtherUnit {\n"
        "    return (OtherUnit)value;\n"
        "}\n"
        "func make_point(x: int, y: int): Point {\n"
        "    return (x, y);\n"
        "}\n"
        "func sum(point: Point): int {\n"
        "    return point.item1 + point.item2;\n"
        "}\n"
        "func cast_sum(point: Point): int {\n"
        "    let coord: Coord = (Coord)point;\n"
        "    return coord.item1 + coord.item2;\n"
        "}\n"
        "func destruct_literal(): int {\n"
        "    let (x, , z) = (1, 2, 3);\n"
        "    return x + z;\n"
        "}\n"
        "func destruct_call(): int {\n"
        "    let (x, y) = make_point(3, 4);\n"
        "    return x + y;\n"
        "}\n"
        "func holder_sum(): int {\n"
        "    let holder = Holder { point: (5, 6), unit: () };\n"
        "    return holder.point.item1 + holder.point.item2;\n"
        "}\n"
        "func generic_pair(): int {\n"
        "    let pair: Pair<int, int> = (7, 8);\n"
        "    return pair.item1 + pair.item2;\n"
        "}\n"
        "func replace_whole(): int {\n"
        "    var point: Point = (1, 2);\n"
        "    point = (3, 4);\n"
        "    return point.item1 + point.item2;\n"
        "}\n"
        "func unit_core(): int {\n"
        "    let unit: Unit = ();\n"
        "    let () = ();\n"
        "    let other = cast_unit(unit);\n"
        "    return unit_score(unit) + unit_score(make_unit()) + unit_score((Unit)other);\n"
        "}\n"
        "func run(): int {\n"
        "    return unit_core() + sum((1, 2)) + sum(make_point(3, 4)) + cast_sum((9, 10)) + destruct_literal() + destruct_call() + holder_sum() + generic_pair() + replace_whole();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/tuple_core_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (tuple core): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "struct Feng__feng__codegen__tuplecore__Point {") != NULL);
    ASSERT(strstr(out.c_source, "Feng__feng__codegen__tuplecore__Point__trivial_desc") != NULL);
    ASSERT(strstr(out.c_source, ".item1") != NULL);
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        char pattern[128];
        snprintf(pattern, sizeof(pattern),
                 "Feng__feng__codegen__tuplecore__Pair__G__%s__%s",
                 int_canonical, int_canonical);
        ASSERT(strstr(out.c_source, pattern) != NULL);
    }
    ASSERT(strstr(out.c_source, "feng_object_new(&FengTypeDesc__feng__codegen__tuplecore__Point") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_tuple_managed_slots_codegen(void) {
    static const char *kSource =
        "module feng.codegen.tuplemanaged;\n"
        "type Label(string, int);\n"
        "type Holder {\n"
        "    let label: Label;\n"
        "}\n"
        "func make_label(name: string): Label {\n"
        "    return (name, 7);\n"
        "}\n"
        "func number(label: Label): int {\n"
        "    return label.item2;\n"
        "}\n"
        "func from_holder(name: string): int {\n"
        "    let holder = Holder { label: (name, 9) };\n"
        "    let (text, value) = holder.label;\n"
        "    return value;\n"
        "}\n"
        "func default_label(): int {\n"
        "    let label: Label;\n"
        "    return label.item2;\n"
        "}\n"
        "func run(name: string): int {\n"
        "    let label = make_label(name);\n"
        "    return number(label) + from_holder(name) + default_label();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/tuple_managed_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (tuple managed slots): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_SLOT_POINTER") != NULL);
    ASSERT(strstr(out.c_source, "FENG_DEFAULT_INIT_FN") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_retain") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_release") != NULL);
    ASSERT(strstr(out.c_source, "Feng__feng__codegen__tuplemanaged__Label__aggregate_desc") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_union_form_spec_codegen(void) {
    static const char *kSource =
        "module feng.codegen.unionbasic;\n"
        "spec Value: int | string | bool;\n"
        "type Holder {\n"
        "    let value: Value;\n"
        "}\n"
        "spec Named {\n"
        "    func greet(): string;\n"
        "}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "    func greet(): string {\n"
        "        return self.name;\n"
        "    }\n"
        "}\n"
        "spec Display: Named | string;\n"
        "func make_int(): Value {\n"
        "    return 4;\n"
        "}\n"
        "func make_string(name: string): Value {\n"
        "    return name;\n"
        "}\n"
        "func choose(v: Value): int {\n"
        "    match v {\n"
        "        x: int { return x + 1; }\n"
        "        string { return 2; }\n"
        "        bool { return 3; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func from_holder(v: Value): int {\n"
        "    let holder = Holder { value: v };\n"
        "    return choose(holder.value);\n"
        "}\n"
        "func make_display(name: string): Display {\n"
        "    let named: Named = User { name: name };\n"
        "    return named;\n"
        "}\n"
        "func display_text(value: Display): string {\n"
        "    match value {\n"
        "        v: Named { return v.greet(); }\n"
        "        v: string { return v; }\n"
        "    }\n"
        "    return \"\";\n"
        "}\n"
        "func use_local(name: string): int {\n"
        "    let local: Value = name;\n"
        "    match local {\n"
        "        string { return 7; }\n"
        "        else { return 0; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/union_form_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (union form spec): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "uint32_t tag;") != NULL);
    ASSERT(strstr(out.c_source, "FENG_SLOT_FORWARD") != NULL);
    ASSERT(strstr(out.c_source, "FENG_SLOT_POINTER") != NULL);
    ASSERT(strstr(out.c_source, "FENG_SLOT_NESTED_AGGREGATE") != NULL);
    ASSERT(strstr(out.c_source, ".tag = 0U") != NULL);
    ASSERT(strstr(out.c_source, ".tag = 1U") != NULL);
    ASSERT(strstr(out.c_source, "payload.m0") != NULL);
    ASSERT(strstr(out.c_source, ".tag == 0U") != NULL);
    ASSERT(strstr(out.c_source, "feng_cleanup_push_aggregate") != NULL);
    ASSERT(strstr(out.c_source,
                  "value), NULL, &FengSpecAgg__feng__codegen__unionbasic__Value") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_union_form_spec_codegen(void) {
    static const char *kSource =
        "module feng.codegen.uniongeneric;\n"
        "type Error {}\n"
        "spec Result<T>: Error | T;\n"
        "func wrap_int(value: int): Result<int> {\n"
        "    return value;\n"
        "}\n"
        "func wrap_error(value: Error): Result<int> {\n"
        "    return value;\n"
        "}\n"
        "func score(value: Result<int>): int {\n"
        "    match value {\n"
        "        v: int { return v + 1; }\n"
        "        Error { return 0; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/generic_union_form_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic union form spec): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_SLOT_FORWARD") != NULL);
    ASSERT(strstr(out.c_source, ".tag = 1U") != NULL);
    ASSERT(strstr(out.c_source, "FengSpecAgg__feng__codegen__uniongeneric__Result") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_generic_union_form_match_expr_codegen(void) {
    static const char *kSource =
        "module feng.codegen.uniongenericexpr;\n"
        "type Error {}\n"
        "spec Result<T>: Error | T;\n"
        "func wrap_int(value: int): Result<int> {\n"
        "    return value;\n"
        "}\n"
        "func wrap_error(value: Error): Result<int> {\n"
        "    return value;\n"
        "}\n"
        "func score(value: Result<int>): int {\n"
        "    return match value {\n"
        "        v: int { v + 1; }\n"
        "        Error { 0; }\n"
        "        else { 0; }\n"
        "    };\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/generic_union_form_match_expr_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic union form match expr): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengSpecAgg__feng__codegen__uniongenericexpr__Result") != NULL);
    ASSERT(strstr(out.c_source, ".tag ==") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_tuple_union_cleanup_codegen(void) {
    static const char *kSource =
        "module feng.codegen.tupleunion;\n"
        "spec Choice: int | string;\n"
        "type Wrapped(Choice, string);\n"
        "func make_choice(name: string): Choice {\n"
        "    return name;\n"
        "}\n"
        "func score(value: Choice): int {\n"
        "    match value {\n"
        "        v: int { return v + 1; }\n"
        "        string { return 2; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func run(name: string): int {\n"
        "    let wrapped: Wrapped = (make_choice(name), \"fallback\");\n"
        "    return score(wrapped.item1);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/tuple_union_cleanup_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (tuple union cleanup): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_cleanup_push_aggregate") != NULL);
    ASSERT(strstr(out.c_source, ".item1, &FengSpecAgg__feng__codegen__tupleunion__Choice") != NULL);
    ASSERT(strstr(out.c_source, ".item1.subject") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_tuple_fit_codegen(void) {
    static const char *kSource =
        "module feng.codegen.tuplefit;\n"
        "spec Summable {\n"
        "    func sum(): int;\n"
        "}\n"
        "type Point(int, int);\n"
        "fit Point: Summable {\n"
        "    func sum(): int {\n"
        "        return self.item1 + self.item2;\n"
        "    }\n"
        "}\n"
        "func consume(value: Summable): int {\n"
        "    return value.sum();\n"
        "}\n"
        "func run(): int {\n"
        "    let point: Point = (5, 6);\n"
        "    let value: Summable = point;\n"
        "    return point.sum() + consume(point) + value.sum();\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/tuple_fit_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (tuple fit): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "__sum(") != NULL);
    ASSERT(strstr(out.c_source, "__spec_box") != NULL);
    ASSERT(strstr(out.c_source, "feng_object_new") != NULL);
    ASSERT(strstr(out.c_source, "tuple_box__as") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* ===== infix match operator codegen tests ===== */

static void test_infix_match_value_pattern_codegen(void) {
    static const char *kSource =
        "module feng.codegen.infixmatchval;\n"
        "func pick(x: int): bool {\n"
        "    return x match 0 | 1 | 2;\n"
        "}\n"
        "func pick_range(x: int): bool {\n"
        "    return x match 1...10;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/infix_match_value_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (infix match value): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* infix match lowers the target to a tmp and emits equality tests.
     * Integer literals are emitted as INT64_C() casts. */
    ASSERT(strstr(out.c_source, "INT64_C(0)") != NULL);
    ASSERT(strstr(out.c_source, "INT64_C(1)") != NULL);
    ASSERT(strstr(out.c_source, "INT64_C(2)") != NULL);
    ASSERT(strstr(out.c_source, "INT64_C(10)") != NULL);
    ASSERT(strstr(out.c_source, " <= ") != NULL);
    ASSERT(strstr(out.c_source, " >= ") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_infix_match_union_member_type_pattern_codegen(void) {
    static const char *kSource =
        "module feng.codegen.infixmatchunion;\n"
        "spec Value: int | string;\n"
        "func is_int(v: Value): bool {\n"
        "    return v match int;\n"
        "}\n"
        "func is_text(v: Value): bool {\n"
        "    return v match int | string;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/infix_match_union_type_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (infix match union type): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* union member type pattern lowers to .tag equality against the
     * member's tag index. */
    ASSERT(strstr(out.c_source, ".tag == 0U") != NULL);
    ASSERT(strstr(out.c_source, ".tag == 1U") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_infix_match_union_member_binding_in_if_cond_codegen(void) {
    static const char *kSource =
        "module feng.codegen.infixmatchbinding;\n"
        "spec Value: int | string;\n"
        "func consume(v: Value): int {\n"
        "    if v match n: int && n > 0 {\n"
        "        return n;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/infix_match_binding_if_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (infix match binding if): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* The binding alias `n` resolves to the active member payload field.
     * Single-member subset aliases to payload.m0 directly. */
    ASSERT(strstr(out.c_source, "payload.m0") != NULL);
    /* Condition lowers to .tag == 0U (int member index). */
    ASSERT(strstr(out.c_source, ".tag == 0U") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_infix_match_union_member_binding_in_while_cond_codegen(void) {
    static const char *kSource =
        "module feng.codegen.infixmatchwhile;\n"
        "spec Value: int | string;\n"
        "func countdown(v: Value): int {\n"
        "    var counter: Value = v;\n"
        "    var sum: int = 0;\n"
        "    while counter match n: int && n > 0 {\n"
        "        sum = sum + n;\n"
        "        counter = n - 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/infix_match_binding_while_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (infix match binding while): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* While loop re-evaluates and re-binds each iteration: the binding
     * tmp and alias must be alive across the body. */
    ASSERT(strstr(out.c_source, ".tag == 0U") != NULL);
    ASSERT(strstr(out.c_source, "payload.m0") != NULL);
    /* while body must use `break` to exit on condition false. */
    ASSERT(strstr(out.c_source, "break;") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_infix_match_unary_not_preserves_precedence_codegen(void) {
    /* Regression: `!(v match string)` must lower to `!(_mt.tag == 1U)`,
     * not `(!_mt.tag) == 1U`. Without inner parentheses on the operand,
     * C parses `!_mt.tag == 1U` as `(!_mt.tag) == 1U` due to precedence. */
    static const char *kSource =
        "module feng.codegen.infixmatchnot;\n"
        "spec Value: int | string;\n"
        "func is_not_text(v: Value): bool {\n"
        "    return !(v match string);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/infix_match_unary_not_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    {
        bool sem_ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                            &analysis, &errors, &error_count);
        if (!sem_ok) {
            for (size_t i = 0U; i < error_count; ++i) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[i].path, errors[i].token.line,
                        errors[i].token.column, errors[i].message);
            }
        }
        ASSERT(sem_ok);
    }
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (infix match unary not): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    /* The unary `!` must wrap the tag comparison atomically:
     * `(!(_mt.tag == 1U))` — verify the inner parenthesized form exists. */
    ASSERT(strstr(out.c_source, "!(_mt") != NULL);
    ASSERT(strstr(out.c_source, ".tag == 1U") != NULL);
    /* And the wrong form `(!_mt.tag == 1U)` must NOT appear. */
    ASSERT(strstr(out.c_source, "!_mt") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* ==================== Literal adaptation codegen tests ==================== */

static FengSemanticAnalysis *literal_adapt_analyze(const char *source,
                                                    const char *path) {
    FengProgram *program = parse_or_die(source, path);
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    bool ok = feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                    &analysis, &errors, &error_count);
    if (!ok) {
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                    errors[i].path, errors[i].token.line,
                    errors[i].token.column, errors[i].message);
        }
    }
    ASSERT(ok);
    ASSERT(error_count == 0U);
    free(errors);
    /* program intentionally leaked — analysis borrows its AST; the test
     * runner is a short-lived process so the leak is harmless. */
    return analysis;
}

static char *literal_adapt_codegen(FengSemanticAnalysis *analysis) {
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                        NULL, &out, &cgerr);
    if (!ok) {
        fprintf(stderr, "codegen error: %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
    }
    ASSERT(ok);
    ASSERT(out.c_source != NULL);
    return out.c_source;
}

static void test_literal_adaptation_binding_typed(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.bind;\n"
        "func f() {\n"
        "    let a: u32 = 1;\n"
        "    let b: i8 = 5;\n"
        "    let c: i64 = 42;\n"
        "    let d: u8 = 255;\n"
        "    let e: f32 = 1;\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_bind.ff");
    char *c = literal_adapt_codegen(analysis);

    ASSERT(strstr(c, "(uint32_t)UINT32_C(1)") != NULL);
    ASSERT(strstr(c, "(int8_t)INT8_C(5)") != NULL);
    ASSERT(strstr(c, "(int64_t)INT64_C(42)") != NULL);
    ASSERT(strstr(c, "(uint8_t)UINT8_C(255)") != NULL);
    ASSERT(strstr(c, "(float)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_binding_untyped(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.untyped;\n"
        "func f() {\n"
        "    let x = 123;\n"
        "    let y = 12.5;\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_untyped.ff");
    char *c = literal_adapt_codegen(analysis);

    /* Untyped integer defaults to int (i64 on 64-bit, i32 on 32-bit). */
    {
        const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
        const char *int_c_macro = sizeof(void *) >= 8U ? "INT64_C" : "INT32_C";
        char expected[64];
        snprintf(expected, sizeof(expected), "(%s)%s(123)", int_c_type, int_c_macro);
        ASSERT(strstr(c, expected) != NULL);
    }
    /* double is f64; untyped float → hex-float literal (e.g. 0x1.9p+3). */
    ASSERT(strstr(c, "0x1.9p+3") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_param_and_return(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.param;\n"
        "func g(n: i32): i32 { return n; }\n"
        "func h(): i64 { return 123; }\n"
        "func f(): i32 {\n"
        "    let r = g(10);\n"
        "    return r;\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_param.ff");
    char *c = literal_adapt_codegen(analysis);

    /* g(10) → parameter i32 → (int32_t)INT32_C(10). */
    ASSERT(strstr(c, "(int32_t)INT32_C(10)") != NULL);
    /* return 123 in h() which returns i64 → (int64_t)INT64_C(123). */
    ASSERT(strstr(c, "(int64_t)INT64_C(123)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_binary(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.binary;\n"
        "func f(): bool {\n"
        "    let n: i32 = 5;\n"
        "    let r = n + 1;\n"
        "    let cmp = n + 1 > 3;\n"
        "    return cmp;\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_binary.ff");
    char *c = literal_adapt_codegen(analysis);

    /* n + 1 where n: i32 → literal 1 adapts to i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(1)") != NULL);
    /* n + 1 > 3 → literal 3 also adapts to i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(3)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_member_and_array(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.memarr;\n"
        "type Box { var value: i32; }\n"
        "func f() {\n"
        "    let b = Box{value: 0};\n"
        "    b.value = 1;\n"
        "    let a: i32[] = [1, 2];\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_memarr.ff");
    char *c = literal_adapt_codegen(analysis);

    /* b.value = 1 (value: i32) → (int32_t)INT32_C(1). */
    ASSERT(strstr(c, "(int32_t)INT32_C(1)") != NULL);
    /* let a: i32[] = [1, 2] → each element as i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(2)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_both_literals(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.bothlit;\n"
        "func f(): bool {\n"
        "    return 10 == 20;\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_bothlit.ff");
    char *c = literal_adapt_codegen(analysis);

    /* Both sides are literals → no adaptation; default int64_t emission. */
    ASSERT(strstr(c, "(int64_t)INT64_C(10)") != NULL);
    ASSERT(strstr(c, "(int64_t)INT64_C(20)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_compound_assignment(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.compound;\n"
        "func f() {\n"
        "    var n: i32 = 5;\n"
        "    n += 1;\n"
        "    n -= 2;\n"
        "    n *= 3;\n"
        "    var x: u8 = 10;\n"
        "    x += 1;\n"
        "    x *= 255;\n"
        "    var mask: i32 = 0;\n"
        "    mask &= 1;\n"
        "    mask |= 2;\n"
        "    mask ^= 4;\n"
        "    mask <<= 1;\n"
        "    mask >>= 1;\n"
        "    var r: f32 = 1.0;\n"
        "    r += 0.5;\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_compound.ff");
    char *c = literal_adapt_codegen(analysis);

    /* n += 1 where n: i32 → literal 1 adapts to i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(1)") != NULL);
    /* n -= 2 → literal 2 adapts to i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(2)") != NULL);
    /* n *= 3 → literal 3 adapts to i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(3)") != NULL);
    /* x += 1 where x: u8 → literal 1 adapts to u8. */
    ASSERT(strstr(c, "(uint8_t)UINT8_C(1)") != NULL);
    /* x *= 255 where x: u8 → literal 255 adapts to u8. */
    ASSERT(strstr(c, "(uint8_t)UINT8_C(255)") != NULL);
    /* mask &= 1 where mask: i32 → literal 1 adapts to i32. */
    /* (already checked above via INT32_C(1)) */
    /* mask |= 2 where mask: i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(4)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_array_literal(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.array;\n"
        "func f() {\n"
        "    let x: i32 = 5;\n"
        "    let a = [x, 10];\n"
        "    let b = [10, x];\n"
        "    var y: u8 = 1;\n"
        "    let c = [y, 255];\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_array.ff");
    char *c = literal_adapt_codegen(analysis);

    /* [x, 10] where x: i32 → literal 10 adapts to i32. */
    ASSERT(strstr(c, "(int32_t)INT32_C(10)") != NULL);
    /* [y, 255] where y: u8 → literal 255 adapts to u8. */
    ASSERT(strstr(c, "(uint8_t)UINT8_C(255)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

static void test_literal_adaptation_tuple_literal(void) {
    static const char *kSource =
        "module feng.codegen.litadapt.tuple;\n"
        "type Point(i32, i32);\n"
        "type BytePair(u8, u8);\n"
        "type SmallPair(i8, i8);\n"
        "type MixedPair(i32, f32);\n"
        "func f() {\n"
        "    let p: Point = (1, 2);\n"
        "    let b: BytePair = (10, 255);\n"
        "    let s: SmallPair = (5, 6);\n"
        "    let m: MixedPair = (42, 1);\n"
        "}\n";
    FengSemanticAnalysis *analysis = literal_adapt_analyze(kSource, "lit_tuple.ff");
    char *c = literal_adapt_codegen(analysis);

    /* Point(i32, i32) → literals as int32_t. */
    ASSERT(strstr(c, "(int32_t)INT32_C(1)") != NULL);
    ASSERT(strstr(c, "(int32_t)INT32_C(2)") != NULL);
    /* BytePair(u8, u8) → literals as uint8_t. */
    ASSERT(strstr(c, "(uint8_t)UINT8_C(10)") != NULL);
    ASSERT(strstr(c, "(uint8_t)UINT8_C(255)") != NULL);
    /* SmallPair(i8, i8) → literals as int8_t. */
    ASSERT(strstr(c, "(int8_t)INT8_C(5)") != NULL);
    ASSERT(strstr(c, "(int8_t)INT8_C(6)") != NULL);
    /* MixedPair(i32, f32) → integer as int32_t, float as (float). */
    ASSERT(strstr(c, "(int32_t)INT32_C(42)") != NULL);
    ASSERT(strstr(c, "(float)") != NULL);
    compile_generated_c_or_die(c);

    feng_semantic_analysis_free(analysis);
}

int main(void) {
    (void)system("rm -rf temp");
    (void)mkdir("temp", 0755);

    test_multi_file_bin();
    test_multi_file_lib();
    test_private_generic_representation_same_package_codegen();
    test_private_representation_cross_package_ft_codegen();
    test_c_variadic_cross_package_ft_codegen();
    test_module_binding_lazy_ensure_init_codegen();
    test_address_of_module_binding_uses_storage_slot_codegen();
    test_module_scalar_var_assignment_marks_initialized_codegen();
    test_module_managed_var_assignment_marks_initialized_codegen();
    test_type_static_members_codegen();
    test_builtin_fit_static_method_codegen();
    test_generic_static_methods_codegen();
    test_generic_type_static_field_codegen();
    test_generic_type_inferred_field_codegen();
    test_generic_type_inferred_field_concrete_call_codegen();
    test_spec_static_member_witness_codegen();
    test_spec_static_var_witness_codegen();
    test_generic_param_static_method_codegen();
    test_generic_param_static_field_read_codegen();
    test_generic_param_static_field_write_codegen();
    test_spec_inherited_static_slot_codegen();
    test_module_binding_default_zero_ensure_init_codegen();
    test_extern_calling_convention_codegen();
    test_extern_c_symbol_name_codegen();
    test_extern_overload_uses_resolved_declaration_codegen();
    test_extern_native_symbol_alias_codegen();
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
    test_array_storage_runtime_contract_codegen();
    test_generic_runtime_extern_expression_equal_codegen();
    test_generic_runtime_extern_direct_type_param_return_codegen();
    test_runtime_extern_codegen_rejects_non_contract_symbol();
    test_generic_function_call_no_infer_rejected();
    test_unsupported_pointer_pointee_reports_explicit_error();
    test_abi_value_function_pointer_codegen();
    test_lib_public_functions_are_exported();
    test_bin_public_functions_remain_static();
    test_imported_feng_function_prototypes_compile();
    test_imported_alias_qualified_type_annotations_codegen_compile();
    test_imported_full_path_type_annotations_codegen_compile_without_use();
    test_imported_public_let_binding_codegen_compiles();
    test_imported_public_static_members_codegen_compiles();
    test_imported_public_var_binding_read_write_codegen_compiles();
    test_imported_public_binding_address_of_codegen_compiles();
    test_public_binding_lib_exports_slot_and_ensure_init_codegen();
    test_public_binding_infers_constructor_type_codegen();
    test_imported_public_binding_inferred_type_codegen_compiles();
    test_enum_codegen_emits_stable_symbols();
    test_imported_enum_codegen_emits_visible_symbols();
    test_imported_enum_union_default_codegen_stays_file_scoped();
    test_imported_generic_enum_argument_uses_canonical_identity();
    test_same_named_types_in_distinct_modules();
    test_float_modulo_codegen_uses_math_runtime();
    test_fit_builtin_direct_call_codegen_shape();
    test_fit_builtin_array_open_generic_return_codegen();
    test_fit_builtin_array_open_generic_value_return_codegen();
    test_fit_builtin_and_array_object_spec_coercion_codegen();
    test_fit_enum_object_spec_coercion_codegen();
    test_object_spec_thunk_subject_cast_shape_codegen();
    test_intersection_spec_witness_struct_codegen();
    test_intersection_spec_witness_instance_codegen();
    test_intersection_spec_default_zero_codegen();
    test_generic_intersection_spec_codegen();
    test_generic_fn_codegen();
    test_generic_type_decl_no_crash();
    test_generic_fn_call_codegen();
    test_generic_managed_return_let_binding_codegen();
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
    test_generic_user_fit_object_spec_coercion_codegen();
    test_generic_constrained_spec_value_codegen();
    test_spec_aggregate_field_codegen();
    test_generic_constrained_aggregate_spec_value_codegen();
    test_if_expr_aggregate_result_codegen();
    test_match_expr_aggregate_result_codegen();
    test_match_statement_codegen();
    test_enum_match_statement_codegen();
    test_enum_match_expression_codegen();
    test_generic_aggregate_return_codegen();
    test_generic_type_generic_method_codegen();
    test_generic_scalar_instance_direct_call_codegen();
    test_phase_e_aggregate_generic_arg_three_entrances_codegen();
    test_type_field_initializers_codegen();
    test_type_field_callable_lambda_initializer_codegen();
    test_callable_field_default_and_explicit_initialization_codegen();
    test_void_try_expression_codegen();
    test_try_catch_return_codegen();
    test_empty_array_literal_codegen_uses_target_contexts();
    test_user_constructor_forms_codegen();
    test_variadic_zero_args_codegen();
    test_variadic_multi_args_codegen();
    test_variadic_fixed_prefix_codegen();
    test_variadic_callable_spec_lambda_codegen();
    test_tuple_value_codegen_core();
    test_tuple_managed_slots_codegen();
    test_union_form_spec_codegen();
    test_generic_union_form_spec_codegen();
    test_generic_union_form_match_expr_codegen();
    test_tuple_union_cleanup_codegen();
    test_tuple_fit_codegen();
    test_infix_match_value_pattern_codegen();
    test_infix_match_union_member_type_pattern_codegen();
    test_infix_match_union_member_binding_in_if_cond_codegen();
    test_infix_match_union_member_binding_in_while_cond_codegen();
    test_infix_match_unary_not_preserves_precedence_codegen();
    test_literal_adaptation_binding_typed();
    test_literal_adaptation_binding_untyped();
    test_literal_adaptation_param_and_return();
    test_literal_adaptation_binary();
    test_literal_adaptation_member_and_array();
    test_literal_adaptation_both_literals();
    test_literal_adaptation_compound_assignment();
    test_literal_adaptation_array_literal();
    test_literal_adaptation_tuple_literal();
    fprintf(stdout, "codegen tests passed\n");
    return 0;
}
