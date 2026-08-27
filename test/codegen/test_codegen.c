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

/* Exact source-node set used to exercise Codegen's package-symbol query
 * boundary without coupling the Codegen unit under test to Symbol internals. */
typedef struct TestPackageSymbolSelection {
    const void *nodes[64];
    size_t node_count;
} TestPackageSymbolSelection;

/* Return whether a source declaration was selected by the test fixture. */
static bool test_package_symbol_selection_contains(const void *user,
                                                   const void *source_node) {
    const TestPackageSymbolSelection *selection =
        (const TestPackageSymbolSelection *)user;
    size_t index;

    if (selection == NULL || source_node == NULL) {
        return false;
    }
    for (index = 0U; index < selection->node_count; ++index) {
        if (selection->nodes[index] == source_node) {
            return true;
        }
    }
    return false;
}

/* Add every type/fit method with the requested name to one exact test set. */
static void test_package_symbol_selection_add_methods_named(
    TestPackageSymbolSelection *selection,
    const FengProgram *program,
    const char *name) {
    size_t decl_index;
    size_t name_length;

    ASSERT(selection != NULL);
    ASSERT(program != NULL);
    ASSERT(name != NULL);
    name_length = strlen(name);
    for (decl_index = 0U;
         decl_index < program->declaration_count;
         ++decl_index) {
        const FengDecl *decl = program->declarations[decl_index];
        FengTypeMember *const *members = NULL;
        size_t member_count = 0U;
        size_t member_index;

        if (decl->kind == FENG_DECL_TYPE) {
            members = decl->as.type_decl.members;
            member_count = decl->as.type_decl.member_count;
        } else if (decl->kind == FENG_DECL_FIT) {
            members = decl->as.fit_decl.members;
            member_count = decl->as.fit_decl.member_count;
        } else {
            continue;
        }
        for (member_index = 0U;
             member_index < member_count;
             ++member_index) {
            const FengTypeMember *member = members[member_index];

            if (member == NULL || member->kind != FENG_TYPE_MEMBER_METHOD ||
                member->as.callable.name.length != name_length ||
                memcmp(member->as.callable.name.data,
                       name,
                       name_length) != 0) {
                continue;
            }
            ASSERT(selection->node_count <
                   sizeof(selection->nodes) / sizeof(selection->nodes[0]));
            selection->nodes[selection->node_count++] = member;
        }
    }
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

/* Count complete substring occurrences inside one half-open source span. */
static size_t count_substr_in_span(const char *start,
                                   const char *end,
                                   const char *needle) {
    size_t count = 0U;
    size_t needle_length;
    const char *cursor;

    if (start == NULL || end == NULL || end < start || needle == NULL ||
        needle[0] == '\0') {
        return 0U;
    }
    needle_length = strlen(needle);
    cursor = start;
    while (cursor + needle_length <= end) {
        const char *match = strstr(cursor, needle);

        if (match == NULL || match + needle_length > end) {
            break;
        }
        count++;
        cursor = match + needle_length;
    }
    return count;
}

/* Locate the generated definition matching a complete declaration prefix.
 * Prototypes are skipped; generated function-closing braces begin at column
 * zero, which gives a stable half-open body span for structural assertions. */
static bool find_generated_function_body(const char *source,
                                         const char *declaration_prefix,
                                         const char **out_start,
                                         const char **out_end) {
    const char *cursor;
    size_t prefix_length;

    if (source == NULL || declaration_prefix == NULL || out_start == NULL ||
        out_end == NULL) {
        return false;
    }
    prefix_length = strlen(declaration_prefix);
    cursor = source;
    while ((cursor = strstr(cursor, declaration_prefix)) != NULL) {
        const char *delimiter = strpbrk(cursor + prefix_length, ";{");

        if (delimiter == NULL) {
            return false;
        }
        if (*delimiter == '{') {
            const char *end = strstr(delimiter, "\n}\n");

            if (end == NULL) {
                return false;
            }
            *out_start = delimiter + 1;
            *out_end = end;
            return true;
        }
        cursor += prefix_length;
    }
    return false;
}

/* Locate a generated function definition by a unique symbol-name fragment
 * and report whether its declaration line has internal `static` linkage. */
static bool generated_function_definition_is_static(
    const char *source,
    const char *symbol_fragment,
    bool *out_is_static) {
    const char *cursor;

    if (source == NULL || symbol_fragment == NULL || out_is_static == NULL) {
        return false;
    }
    cursor = source;
    while ((cursor = strstr(cursor, symbol_fragment)) != NULL) {
        const char *delimiter = strpbrk(cursor + strlen(symbol_fragment), ";{");

        if (delimiter == NULL) {
            return false;
        }
        if (*delimiter == '{') {
            const char *line_start = cursor;

            while (line_start > source && line_start[-1] != '\n') {
                --line_start;
            }
            *out_is_static = span_contains(line_start, cursor, "static ");
            return true;
        }
        cursor += strlen(symbol_fragment);
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
                  "->reified_field_offsets[") != NULL);
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

/* Package-public .ft metadata keeps generic fit dependencies on each method.
 * The consumer closes the owner without calling one dependent method, then
 * exercises witness, static, and method-generic paths from the restored sets. */
static void test_generic_fit_member_dependencies_cross_package_codegen(void) {
    static const char *kProviderSource =
        "open module vendor.generic_fit_member_deps;\n"
        "open type PackageFitDep<T> {\n"
        "    open let value: T;\n"
        "    func PackageFitDep(value: T) { self.value = value; }\n"
        "}\n"
        "@value\n"
        "open type PackageFitPair<T, U> {\n"
        "    open let first: T;\n"
        "    open let second: U;\n"
        "    func PackageFitPair(first: T, second: U) {\n"
        "        self.first = first; self.second = second;\n"
        "    }\n"
        "}\n"
        "open spec PackageFitSurface<T> { func read(): T; }\n"
        "open type PackageFitOwner<T> {\n"
        "    open let value: T;\n"
        "    func PackageFitOwner(value: T) { self.value = value; }\n"
        "}\n"
        "open fit PackageFitOwner<T>: PackageFitSurface<T> {\n"
        "    open func hiddenDependency(): T {\n"
        "        let dep = PackageFitDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    open func read(): T {\n"
        "        let dep = PackageFitDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    open static func transform(value: T): T {\n"
        "        let dep = PackageFitDep<T>(value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    open func wrap<U>(value: U): U {\n"
        "        let dep = PackageFitPair<T, U>(self.value, value);\n"
        "        return dep.second;\n"
        "    }\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.generic_fit_member_deps;\n"
        "import vendor.generic_fit_member_deps;\n"
        "func readPackage(value: PackageFitSurface<int>): int {\n"
        "    return value.read();\n"
        "}\n"
        "func use(): int {\n"
        "    let owner = PackageFitOwner<int>(40);\n"
        "    let view: PackageFitSurface<int> = owner;\n"
        "    let explicit = owner.wrap<string>(\"explicit\");\n"
        "    let inferred = owner.wrap(\"inferred\");\n"
        "    explicit; inferred;\n"
        "    return readPackage(view) +\n"
        "           PackageFitOwner<int>.transform(2);\n"
        "}\n";
    FengProgram *provider_program = parse_or_die(
        kProviderSource, "generic_fit_member_deps_provider.ff");
    const FengProgram *provider_programs[1] = {provider_program};
    FengSemanticAnalysis *provider_analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolExportOptions export_options = {0};
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
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

    consumer_program = parse_or_die(
        kConsumerSource, "generic_fit_member_deps_consumer.ff");
    consumer_programs[0] = consumer_program;
    ASSERT(feng_semantic_analyze_with_options(consumer_programs,
                                              1U,
                                              &analyze_options,
                                              &consumer_analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    feng_symbol_imported_module_cache_populate_codegen_metadata(
        cache, consumer_analysis);
    if (!feng_codegen_emit_program(consumer_analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &output,
                                   &codegen_error)) {
        fprintf(stderr,
                "cross-package generic fit dependency codegen error: %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "(unknown)");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  ".name = \"hiddenDependency\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(output.c_source,
                  ".name = \"read\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(output.c_source,
                  ".name = \"transform\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(output.c_source,
                  ".name = \"wrap\", "
                  ".reified_agg_deps_count = 1") != NULL);
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

/* User-type fit symbols use a target-local stable ordinal, so unrelated fits
 * cannot change the producer/consumer implementation name. */
static void test_user_fit_static_method_symbol_is_stable(void) {
    static const char *kSource =
        "module feng.codegen.fitstable;\n"
        "type Other {}\n"
        "fit Other { static func unrelated(): int { return 0; } }\n"
        "type Box {}\n"
        "open fit Box { open static func first(): int { return 1; } }\n"
        "open fit Box { open static func second(): int { return 2; } }\n"
        "func run_case(): int { return Box.first() + Box.second(); }\n";
    FengProgram *program = parse_or_die(kSource, "fitstable.ff");
    const FengProgram *programs[] = {program};
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
    ASSERT(strstr(
        out.c_source,
        "FengFitUser__feng__codegen__fitstable__"
        "Feng__feng__codegen__fitstable__Box__m0__first") != NULL);
    ASSERT(strstr(
        out.c_source,
        "FengFitUser__feng__codegen__fitstable__"
        "Feng__feng__codegen__fitstable__Box__m1__second") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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
        "    func Inner() {}\n"
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
        "    func Inner() {}\n"
        "    func set_value(next: T) { self.value = next; }\n"
        "    func get_value(): T { return self.value; }\n"
        "}\n"
        "type Holder<T> {\n"
        "    seal let items = Inner<T>();\n"
        "    func Holder() {}\n"
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

/* Implementation-side parameter bindings do not alter requirement slots or
 * introduce callable adapters; ordinary witness thunks forward the same ABI. */
static void test_object_spec_method_parameter_bindings_keep_witness_abi(void) {
    static const char *kSource =
        "module feng.codegen.specparambinding;\n"
        "spec Transformer {\n"
        "    func plain(value: int): int;\n"
        "    func fixed(value: int): int;\n"
        "    func changing(value: int): int;\n"
        "    static func plainStatic(value: int): int;\n"
        "    static func fixedStatic(value: int): int;\n"
        "    static func changingStatic(value: int): int;\n"
        "}\n"
        "type DirectTransformer: Transformer {\n"
        "    func plain(value: int): int { return value; }\n"
        "    func fixed(let value: int): int { return value; }\n"
        "    func changing(var value: int): int { value += 1; return value; }\n"
        "    static func plainStatic(value: int): int { return value; }\n"
        "    static func fixedStatic(let value: int): int { return value; }\n"
        "    static func changingStatic(var value: int): int { value += 1; return value; }\n"
        "}\n"
        "type AdaptedTransformer {}\n"
        "fit AdaptedTransformer: Transformer {\n"
        "    func plain(value: int): int { return value; }\n"
        "    func fixed(let value: int): int { return value; }\n"
        "    func changing(var value: int): int { value += 1; return value; }\n"
        "    static func plainStatic(value: int): int { return value; }\n"
        "    static func fixedStatic(let value: int): int { return value; }\n"
        "    static func changingStatic(var value: int): int { value += 1; return value; }\n"
        "}\n"
        "func directView(): Transformer { return DirectTransformer(); }\n"
        "func adaptedView(): Transformer { return AdaptedTransformer(); }\n"
        "func callStatic<T: Transformer>(value: int): int {\n"
        "    return T.changingStatic(value);\n"
        "}\n"
        "func useAll(): int {\n"
        "    return directView().changing(1) + adaptedView().changing(2) +\n"
        "        callStatic<DirectTransformer>(3) + callStatic<AdaptedTransformer>(4);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "object_spec_method_parameter_bindings_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *witness_start;
    const char *witness_end;

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
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    witness_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__specparambinding__Transformer {\n");
    ASSERT(witness_start != NULL);
    witness_end = strstr(witness_start, "};\n");
    ASSERT(witness_end != NULL);
    ASSERT(count_substr_in_span(witness_start, witness_end, "(*") == 6U);
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*plain)(void *_subject, int64_t);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*fixed)(void *_subject, int64_t);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*changing)(void *_subject, int64_t);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*plainStatic)(int64_t);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*fixedStatic)(int64_t);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*changingStatic)(int64_t);"));
    ASSERT(!span_contains(witness_start, witness_end, "let"));
    ASSERT(!span_contains(witness_start, witness_end, "var"));
    ASSERT(strstr(output.c_source, "FengCallableInvoke__") == NULL);
    ASSERT(strstr(output.c_source, "FengCallableStaticInvoke__") == NULL);
    ASSERT(strstr(output.c_source,
                  "FengSpecThunk__feng__codegen__specparambinding") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Instance and static fields with one Feng name occupy independent witness
 * slots; only the instance field contributes storage to the default subject. */
static void test_object_spec_cross_surface_fields_keep_distinct_slots(void) {
    static const char *kSource =
        "module feng.codegen.specfieldowners;\n"
        "spec Surface {\n"
        "    var value: int;\n"
        "    static var value: int;\n"
        "}\n"
        "type Impl: Surface {\n"
        "    var value: int = 1;\n"
        "    static var value: int = 2;\n"
        "}\n"
        "func updateInstance(value: Surface): int {\n"
        "    value.value += 1;\n"
        "    return value.value;\n"
        "}\n"
        "func updateStatic<T: Surface>(): int {\n"
        "    T.value += 1;\n"
        "    return T.value;\n"
        "}\n"
        "func useAll(): int {\n"
        "    let value: Surface = Impl();\n"
        "    return updateInstance(value) + updateStatic<Impl>();\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "object_spec_cross_surface_fields_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *witness_start;
    const char *witness_end;
    const char *subject_start;
    const char *subject_end;

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
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    witness_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__specfieldowners__Surface {\n");
    ASSERT(witness_start != NULL);
    witness_end = strstr(witness_start, "};\n");
    ASSERT(witness_end != NULL);
    ASSERT(count_substr_in_span(witness_start, witness_end, "(*") == 4U);
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*get_value)(void *_subject);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "void (*set_value)(void *_subject, int64_t value);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "int64_t (*get_value__feng_overload_2)(void);"));
    ASSERT(span_contains(witness_start,
                         witness_end,
                         "void (*set_value__feng_overload_2)(int64_t value);"));

    subject_start = strstr(
        output.c_source,
        "struct FengSpecDefault__feng__codegen__specfieldowners__Surface__Subject {\n");
    ASSERT(subject_start != NULL);
    subject_end = strstr(subject_start, "};\n");
    ASSERT(subject_end != NULL);
    ASSERT(count_substr_in_span(subject_start, subject_end, "int64_t value;") == 1U);
    ASSERT(!span_contains(subject_start,
                          subject_end,
                          "value__feng_overload_2"));
    ASSERT(strstr(output.c_source, "witness->get_value(") != NULL);
    ASSERT(strstr(output.c_source,
                  "witness)->get_value__feng_overload_2()") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Cross-owner field/method names and methods named after their spec keep
 * using the existing compile-time-selected field, instance and static slots. */
static void test_object_spec_special_names_keep_distinct_witness_slots(void) {
    static const char *kSource =
        "module feng.codegen.specspecialsurface;\n"
        "spec Surface {\n"
        "    var instanceOwned: int;\n"
        "    static func instanceOwned(): int;\n"
        "    static var staticOwned: int;\n"
        "    func staticOwned(): int;\n"
        "}\n"
        "type Impl: Surface {\n"
        "    var instanceOwned: int = 1;\n"
        "    static func instanceOwned(): int { return 2; }\n"
        "    static var staticOwned: int = 3;\n"
        "    func staticOwned(): int { return 4; }\n"
        "}\n"
        "func readInstance(value: Surface): int {\n"
        "    value.instanceOwned += 1;\n"
        "    return value.instanceOwned + value.staticOwned();\n"
        "}\n"
        "func readStatic<T: Surface>(): int {\n"
        "    T.staticOwned += 1;\n"
        "    return T.instanceOwned() + T.staticOwned;\n"
        "}\n"
        "spec NamedResource {\n"
        "    func NamedResource(): int;\n"
        "    static func NamedResource(): string;\n"
        "}\n"
        "type NamedImpl: NamedResource {\n"
        "    func NamedResource(): int { return 5; }\n"
        "    static func NamedResource(): string { return \"named\"; }\n"
        "}\n"
        "func readNamed(value: NamedResource): int {\n"
        "    return value.NamedResource();\n"
        "}\n"
        "func readNamedStatic<T: NamedResource>(): string {\n"
        "    return T.NamedResource();\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "object_spec_special_names_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *surface_start;
    const char *surface_end;
    const char *named_start;
    const char *named_end;

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
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    surface_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__specspecialsurface__Surface {\n");
    ASSERT(surface_start != NULL);
    surface_end = strstr(surface_start, "};\n");
    ASSERT(surface_end != NULL);
    ASSERT(count_substr_in_span(surface_start, surface_end, "(*") == 6U);
    ASSERT(span_contains(surface_start,
                         surface_end,
                         "int64_t (*get_instanceOwned)(void *_subject);"));
    ASSERT(span_contains(surface_start,
                         surface_end,
                         "void (*set_instanceOwned)(void *_subject, int64_t value);"));
    ASSERT(span_contains(surface_start,
                         surface_end,
                         "int64_t (*instanceOwned__feng_overload_2)();"));
    ASSERT(span_contains(surface_start,
                         surface_end,
                         "int64_t (*get_staticOwned)(void);"));
    ASSERT(span_contains(surface_start,
                         surface_end,
                         "void (*set_staticOwned)(int64_t value);"));
    ASSERT(span_contains(surface_start,
                         surface_end,
                         "int64_t (*staticOwned__feng_overload_2)(void *_subject);"));

    named_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__specspecialsurface__NamedResource {\n");
    ASSERT(named_start != NULL);
    named_end = strstr(named_start, "};\n");
    ASSERT(named_end != NULL);
    ASSERT(count_substr_in_span(named_start, named_end, "(*") == 2U);
    ASSERT(span_contains(named_start,
                         named_end,
                         "int64_t (*NamedResource)(void *_subject);"));
    ASSERT(count_substr_in_span(named_start,
                                named_end,
                                "(*NamedResource__feng_overload_2)(") == 1U);

    ASSERT(strstr(output.c_source,
                  ".witness->get_instanceOwned(") != NULL);
    ASSERT(strstr(output.c_source,
                  ".witness->staticOwned__feng_overload_2(") != NULL);
    ASSERT(strstr(output.c_source,
                  ")->instanceOwned__feng_overload_2()") != NULL);
    ASSERT(strstr(output.c_source,
                  ")->get_staticOwned()") != NULL);
    ASSERT(strstr(output.c_source,
                  ".witness->NamedResource(") != NULL);
    ASSERT(strstr(output.c_source,
                  ")->NamedResource__feng_overload_2()") != NULL);
    ASSERT(strstr(output.c_source, "FengCallableInvoke__") == NULL);
    ASSERT(strstr(output.c_source, "FengCallableStaticInvoke__") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Every object-form default initialization reaches a fresh-subject factory;
 * aggregate arrays carry the same init descriptor for per-element creation. */
static void test_object_spec_default_initialization_uses_fresh_subjects(void) {
    static const char *kSource =
        "module feng.codegen.specdefaultfresh;\n"
        "spec Counter {\n"
        "    var value: int;\n"
        "}\n"
        "func run(): int {\n"
        "    let first: Counter;\n"
        "    let second: Counter;\n"
        "    let values: Counter[!] = Counter[:2];\n"
        "    first.value = 1;\n"
        "    values[0].value = 2;\n"
        "    return first.value + second.value + values[0].value + values[1].value;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "object_spec_default_fresh_subjects_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *factory_start;
    const char *factory_end;
    const char *init_start;
    const char *init_end;
    const char *run_start;
    const char *run_end;

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
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    factory_start = strstr(
        output.c_source,
        "static struct FengSpecDefault__feng__codegen__specdefaultfresh__Counter__Subject *"
        "FengSpecDefault__feng__codegen__specdefaultfresh__Counter__new_subject(void) {\n");
    ASSERT(factory_start != NULL);
    factory_end = strstr(factory_start, "\n}\n");
    ASSERT(factory_end != NULL);
    ASSERT(span_contains(
        factory_start,
        factory_end,
        "feng_object_new(&FengSpecDefault__feng__codegen__specdefaultfresh__"
        "Counter__Subject_desc)"));

    init_start = strstr(
        output.c_source,
        "static void FengSpecAggInit__feng__codegen__specdefaultfresh__Counter("
        "void *_value_out) {\n");
    ASSERT(init_start != NULL);
    init_end = strstr(init_start, "\n}\n");
    ASSERT(init_end != NULL);
    ASSERT(count_substr_in_span(
               init_start,
               init_end,
               "FengSpecDefault__feng__codegen__specdefaultfresh__Counter__"
               "new_subject()") == 1U);
    ASSERT(span_contains(
        init_start,
        init_end,
        "_v->witness = &FengSpecDefaultWitness__feng__codegen__"
        "specdefaultfresh__Counter;"));

    run_start = strstr(
        output.c_source,
        "static int64_t feng__feng__codegen__specdefaultfresh__run__from__void("
        "void) {\n");
    ASSERT(run_start != NULL);
    run_end = strstr(run_start, "\n}\n");
    ASSERT(run_end != NULL);
    ASSERT(count_substr_in_span(
               run_start,
               run_end,
               "feng_aggregate_default_init(") == 2U);
    ASSERT(span_contains(
        run_start,
        run_end,
        "feng_array_new_kinded(FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, "
        "&FengSpecAgg__feng__codegen__specdefaultfresh__Counter, NULL, "
        "sizeof(struct FengSpecValue__feng__codegen__specdefaultfresh__Counter),"));
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Owned object-to-spec conversions move into persistent aggregate slots;
 * borrowed spec values keep the ordinary copying assignment path. */
static void test_object_spec_owned_subjects_move_into_persistent_slots(void) {
    static const char *kSource =
        "module feng.codegen.specownership;\n"
        "spec View { func id(): int; }\n"
        "type Resource: View {\n"
        "    let value: int;\n"
        "    func Resource(value: int) { self.value = value; }\n"
        "    func id(): int { return self.value; }\n"
        "}\n"
        "type Holder { var view: View; }\n"
        "func overwriteLocal(): int {\n"
        "    var view: View = Resource(1);\n"
        "    view = Resource(2);\n"
        "    return view.id();\n"
        "}\n"
        "func overwriteField(): int {\n"
        "    let holder = Holder { view: Resource(1) };\n"
        "    holder.view = Resource(2);\n"
        "    return holder.view.id();\n"
        "}\n"
        "func overwriteArray(): int {\n"
        "    let values: View[!] = View[:1];\n"
        "    values[0] = Resource(1);\n"
        "    values[0] = Resource(2);\n"
        "    return values[0].id();\n"
        "}\n"
        "func copyBorrowed(source: View): int {\n"
        "    var result: View;\n"
        "    result = source;\n"
        "    return result.id();\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "object_spec_owned_subject_store_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *local_start;
    const char *local_end;
    const char *field_start;
    const char *field_end;
    const char *array_start;
    const char *array_end;
    const char *borrowed_start;
    const char *borrowed_end;

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
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    local_start = strstr(
        output.c_source,
        "static int64_t feng__feng__codegen__specownership__"
        "overwriteLocal__from__void(void) {\n");
    ASSERT(local_start != NULL);
    local_end = strstr(local_start, "\n}\n");
    ASSERT(local_end != NULL);
    ASSERT(count_substr_in_span(
               local_start, local_end, "feng_aggregate_take(") == 1U);
    ASSERT(count_substr_in_span(
               local_start, local_end, "feng_cleanup_push(&_cu__t") == 0U);

    field_start = strstr(
        output.c_source,
        "static int64_t feng__feng__codegen__specownership__"
        "overwriteField__from__void(void) {\n");
    ASSERT(field_start != NULL);
    field_end = strstr(field_start, "\n}\n");
    ASSERT(field_end != NULL);
    ASSERT(count_substr_in_span(
               field_start, field_end, "feng_aggregate_take(") == 2U);
    ASSERT(count_substr_in_span(
               field_start, field_end, "feng_cleanup_push(&_cu__t") == 0U);

    array_start = strstr(
        output.c_source,
        "static int64_t feng__feng__codegen__specownership__"
        "overwriteArray__from__void(void) {\n");
    ASSERT(array_start != NULL);
    array_end = strstr(array_start, "\n}\n");
    ASSERT(array_end != NULL);
    ASSERT(count_substr_in_span(
               array_start, array_end, "feng_aggregate_take(") == 2U);
    ASSERT(count_substr_in_span(
               array_start, array_end, "feng_cleanup_push(&_cu__t") == 0U);

    borrowed_start = strstr(
        output.c_source,
        "static int64_t feng__feng__codegen__specownership__"
        "copyBorrowed__from__S_FengSpecValue__feng__codegen__specownership__"
        "View(struct FengSpecValue__feng__codegen__specownership__View source) {\n");
    ASSERT(borrowed_start != NULL);
    borrowed_end = strstr(borrowed_start, "\n}\n");
    ASSERT(borrowed_end != NULL);
    ASSERT(count_substr_in_span(
               borrowed_start, borrowed_end, "feng_aggregate_assign(") == 1U);
    ASSERT(count_substr_in_span(
               borrowed_start, borrowed_end, "feng_aggregate_take(") == 0U);
    ASSERT(strstr(output.c_source, "feng_scalar_box_new_") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Every spec-owned C tag must have file scope before any witness signature,
 * regardless of member staticness, parameter position, spec form, or source
 * declaration order. */
static void test_spec_c_tags_precede_all_witness_signatures_codegen(void) {
    static const char *kSource =
        "module feng.codegen.spectagforward;\n"
        "spec Uses {\n"
        "    static func useSelf(value: Uses): void;\n"
        "    static func useLater(value: Later): void;\n"
        "    static func useCallback(value: Callback): void;\n"
        "    static func useChoice(value: Choice): void;\n"
        "    func visit(value: Later): void;\n"
        "}\n"
        "spec Later {}\n"
        "spec Callback(value: int): void;\n"
        "spec Choice: int | string;\n";
    FengProgram *program = parse_or_die(kSource, "spectagforward.ff");
    const FengProgram *programs[1] = { program };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    if (error_count != 0U) {
        fprintf(stderr, "semantic error (spec tag forwards): %s\n",
                errors[0].message ? errors[0].message : "(unknown)");
    }
    ASSERT(error_count == 0U);

    bool cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                           NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (spec tag forwards): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);

    const char *uses_tag = strstr(
        out.c_source,
        "struct FengSpecValue__feng__codegen__spectagforward__Uses;\n");
    const char *later_tag = strstr(
        out.c_source,
        "struct FengSpecValue__feng__codegen__spectagforward__Later;\n");
    const char *callback_tag = strstr(
        out.c_source,
        "struct FengClosure__feng__codegen__spectagforward__Callback;\n");
    const char *choice_tag = strstr(
        out.c_source,
        "struct FengSpecValue__feng__codegen__spectagforward__Choice;\n");
    const char *uses_witness = strstr(
        out.c_source,
        "struct FengSpecWitness__feng__codegen__spectagforward__Uses {");

    ASSERT(uses_tag != NULL);
    ASSERT(later_tag != NULL);
    ASSERT(callback_tag != NULL);
    ASSERT(choice_tag != NULL);
    ASSERT(uses_witness != NULL);
    ASSERT(uses_tag < uses_witness);
    ASSERT(later_tag < uses_witness);
    ASSERT(callback_tag < uses_witness);
    ASSERT(choice_tag < uses_witness);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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
        char descriptor_definition[320];
        snprintf(descriptor_definition, sizeof(descriptor_definition),
                 "static const FengGenericParamDescriptor _feng_closed_generic_param_desc_0 = {.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL};",
                 int_descriptor);
        ASSERT(strstr(out.c_source, descriptor_definition) != NULL);
        ASSERT(strstr(out.c_source,
                      "feng_array_get_length(&_feng_closed_generic_param_desc_0, values)") != NULL);
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
        char descriptor_definition[320];
        snprintf(descriptor_definition, sizeof(descriptor_definition),
                 "static const FengGenericParamDescriptor _feng_closed_generic_param_desc_0 = {.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL};",
                 int_descriptor);
        ASSERT(strstr(out.c_source, descriptor_definition) != NULL);
        ASSERT(strstr(out.c_source,
                      "feng_array_get_length(&_feng_closed_generic_param_desc_0, values)") != NULL);
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
             "static const FengGenericParamDescriptor _feng_closed_generic_param_desc_0 = {.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL};",
             int_descriptor);
    ASSERT(count_substr(out.c_source, descriptor) == 1U);
    ASSERT(count_substr(out.c_source,
                        "&_feng_closed_generic_param_desc_0") == 5U);
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
        char descriptor_definition[320];
        char rga_pattern[64];
        snprintf(descriptor_definition, sizeof(descriptor_definition),
                 "static const FengGenericParamDescriptor _feng_closed_generic_param_desc_0 = {.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL};",
                 int_descriptor);
        ASSERT(strstr(out.c_source, descriptor_definition) != NULL);
        ASSERT(strstr(out.c_source,
                      "feng_expression_equal(&_feng_closed_generic_param_desc_0, &_rga") != NULL);
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
        char descriptor_definition[320];
        char rga_pattern[64];
        char rgr_pattern[64];
        snprintf(descriptor_definition, sizeof(descriptor_definition),
                 "static const FengGenericParamDescriptor _feng_closed_generic_param_desc_0 = {.kind = FENG_VALUE_TRIVIAL, .descriptor = &%s, .witness = NULL};",
                 int_descriptor);
        ASSERT(strstr(out.c_source, descriptor_definition) != NULL);
        ASSERT(strstr(out.c_source,
                      "__test_value_identity(&_feng_closed_generic_param_desc_0, &_rga") != NULL);
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

/* An array descriptor that still depends on an incoming open type parameter
 * is not a compile-time constant. Keep its generic-parameter carrier scoped
 * to the shared body instead of unsafely promoting it to file-scope data. */
static void test_open_generic_param_descriptor_remains_runtime_scoped(void) {
    static const char *kSource =
        "module feng.codegen.open_generic_param_descriptor;\n"
        "func identity<U>(value: U): U { return value; }\n"
        "func copy<T>(values: T[]): T[] {\n"
        "    return identity<T[]>(values);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "open_generic_param_descriptor_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "&(const FengGenericParamDescriptor){"
                  ".kind = FENG_VALUE_MANAGED_POINTER, "
                  ".descriptor = &(const FengTypeDescriptor){") != NULL);
    ASSERT(strstr(output.c_source,
                  ".reified_generic_params = "
                  "(const FengGenericParamDescriptor *const[]){_T}") != NULL);
    ASSERT(strstr(output.c_source,
                  "_feng_closed_generic_param_desc_") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

/* A failure while emitting a generic function body must propagate through the
 * shared cleanup path instead of returning a partial C translation as success. */
static void test_generic_function_codegen_failure_propagates(void) {
    static const char *kSource =
        "module feng.codegen.genericfailure;\n"
        "type User {\n"
        "    var name: string;\n"
        "}\n"
        "func unsupported<T>() {\n"
        "    let pointer: User*;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "genericfailure.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
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
    ASSERT(!cg_ok);
    ASSERT(cgerr.message != NULL);
    ASSERT(strstr(cgerr.message,
                  "does not support ABI pointer lowering") != NULL);

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

/* A complete module path recovers the same imported top-level declarations
 * without a lexical import and emits their ordinary external surfaces. */
static void test_full_path_values_codegen_compile_without_import(void) {
    static const char *kImportedSource =
        "open module vendor.full_path;\n"
        "open enum Mode { First, Second }\n"
        "open let seed: int = 7;\n"
        "open var current: int = 1;\n"
        "open func add(left: int, right: int): int {\n"
        "    return left + right;\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "func project(next: int): int {\n"
        "    let mode: vendor.full_path.Mode = vendor.full_path.Mode.Second;\n"
        "    vendor.full_path.current = next;\n"
        "    return vendor.full_path.add(vendor.full_path.seed, vendor.full_path.current) + (int)mode;\n"
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

    imported_source_fixture_init(
        &fixture,
        "tests/full_path_function_binding_vendor.ff",
        kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_or_die(
        kConsumerSource,
        "tests/full_path_function_binding_consumer.ff");
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
                  "feng__vendor__full_path__add__from__") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng__vendor__full_path__seed__ensure_init__from__void();") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng__vendor__full_path__current__ensure_init__from__void();") != NULL);
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

/* An imported witness may reference static fields that remain seal in FT
 * when an exported nominal spec relation selected them. Their declarations
 * reuse the same imported static-binding path as open/default fields. */
static void test_imported_selected_seal_static_fields_codegen_compiles(void) {
    static const char *kImportedSource =
        "open module vendor.seal_static_state;\n"
        "open spec State {\n"
        "    static let publicValue: int;\n"
        "    seal static let initial: int;\n"
        "    seal static var current: int;\n"
        "}\n"
        "open type Store: State {\n"
        "    static let publicValue: int = 3;\n"
        "    seal static let initial: int = 5;\n"
        "    seal static var current: int = 7;\n"
        "    seal static let unrelated: int = 11;\n"
        "    open static func advance<T: State>(): int {\n"
        "        T.current = T.current + 1;\n"
        "        return T.publicValue + T.initial + T.current;\n"
        "    }\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.main;\n"
        "import vendor.seal_static_state;\n"
        "func project(): int {\n"
        "    return Store.advance<Store>();\n"
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
    const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
    char expected[256];

    imported_source_fixture_init(
        &fixture,
        "tests/imported_seal_static_state_vendor.ff",
        kImportedSource);
    query = feng_symbol_imported_module_cache_as_query(fixture.cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_or_die(
        kConsumerSource,
        "tests/imported_seal_static_state_consumer.ff");
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
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "extern %s Feng__vendor__seal_static_state__Store__static__publicValue;",
               int_c_type) > 0);
    ASSERT(strstr(out.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "extern %s Feng__vendor__seal_static_state__Store__static__initial;",
               int_c_type) > 0);
    ASSERT(strstr(out.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "extern %s Feng__vendor__seal_static_state__Store__static__current;",
               int_c_type) > 0);
    ASSERT(strstr(out.c_source, expected) != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void Feng__vendor__seal_static_state__Store__static__initial__ensure_init(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void Feng__vendor__seal_static_state__Store__static__current__ensure_init(void);") != NULL);
    ASSERT(strstr(out.c_source,
                  "extern void Feng__vendor__seal_static_state__Store__static__unrelated__ensure_init(void);") == NULL);

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

/* Ensure imported field debug types are resolved in the field declaration's
 * program, not in the consumer program that happens to emit the field record. */
static void test_imported_field_debug_type_uses_declaring_program_context(void) {
    static const char *kCollectionsSource =
        "open module vendor.collections;\n"
        "open type Box<T> {\n"
        "    open var value: T;\n"
        "}\n";
    static const char *kEventSource =
        "open module vendor.event;\n"
        "import vendor.collections;\n"
        "open type Event {\n"
        "    open let box: Box<i32>;\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.eventconsumer;\n"
        "import vendor.event;\n"
        "func consume(event: Event): void {\n"
        "}\n";
    static const char *kParentDisplayType = "vendor.event.Event";
    static const char *kFieldDisplayType = "vendor.collections.Box<i32>";
    FengCodegenMapingSourceMapping provider_mappings[2] = {
        {
            .source_path = "tests/imported_field_context_collections.ff",
            .package_name = "vendor",
            .package_root = "tests",
        },
        {
            .source_path = "tests/imported_field_context_event.ff",
            .package_name = "vendor",
            .package_root = "tests",
        },
    };
    FengCodegenMapingSourceMapping consumer_mapping = {
        .source_path = "tests/imported_field_context_consumer.ff",
        .package_name = "demo",
        .package_root = "tests",
    };
    FengCodegenOptions provider_codegen_options = {
        .debug_source_mappings = provider_mappings,
        .debug_source_mapping_count = 2U,
    };
    FengCodegenOptions consumer_codegen_options = {
        .debug_source_mappings = &consumer_mapping,
        .debug_source_mapping_count = 1U,
    };
    FengProgram *provider_programs[2] = {
        parse_or_die(kCollectionsSource,
                     "tests/imported_field_context_collections.ff"),
        parse_or_die(kEventSource,
                     "tests/imported_field_context_event.ff"),
    };
    const FengProgram *provider_program_views[2] = {
        provider_programs[0],
        provider_programs[1],
    };
    FengProgram *consumer_program = NULL;
    const FengProgram *consumer_programs[1];
    FengSemanticAnalysis *provider_analysis = NULL;
    FengSemanticAnalysis *consumer_analysis = NULL;
    FengSemanticError *provider_errors = NULL;
    FengSemanticError *consumer_errors = NULL;
    size_t provider_error_count = 0U;
    size_t consumer_error_count = 0U;
    FengSymbolGraph *graph = NULL;
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSymbolError symbol_error = {0};
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions analyze_options = {0};
    FengCodegenOutput provider_out = {0};
    FengCodegenOutput consumer_out = {0};
    FengCodegenError provider_cgerr = {0};
    FengCodegenError consumer_cgerr = {0};
    bool provider_field_found = false;
    bool consumer_field_found = false;

    ASSERT(feng_semantic_analyze(provider_program_views,
                                 2U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &provider_analysis,
                                 &provider_errors,
                                 &provider_error_count));
    ASSERT(provider_errors == NULL);
    ASSERT(provider_error_count == 0U);
    ASSERT(feng_codegen_emit_program(provider_analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     &provider_codegen_options,
                                     &provider_out,
                                     &provider_cgerr));
    ASSERT(provider_out.c_source != NULL);
    ASSERT(feng_symbol_build_graph(provider_analysis, &graph, &symbol_error));
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_graph(provider, graph, &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);

    query = feng_symbol_imported_module_cache_as_query(cache);
    analyze_options.target = FENG_COMPILE_TARGET_LIB;
    analyze_options.imported_modules = &query;
    analyze_options.pointer_size = sizeof(void *);
    consumer_program = parse_or_die(kConsumerSource,
                                    "tests/imported_field_context_consumer.ff");
    consumer_programs[0] = consumer_program;
    ASSERT(feng_semantic_analyze_with_options(consumer_programs,
                                              1U,
                                              &analyze_options,
                                              &consumer_analysis,
                                              &consumer_errors,
                                              &consumer_error_count));
    ASSERT(consumer_errors == NULL);
    ASSERT(consumer_error_count == 0U);
    ASSERT(feng_codegen_emit_program(consumer_analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     &consumer_codegen_options,
                                     &consumer_out,
                                     &consumer_cgerr));
    ASSERT(consumer_out.c_source != NULL);

    for (size_t index = 0U; index < provider_out.debug_info.variable_count; ++index) {
        const FengCodegenMapingVariableRecord *variable =
            &provider_out.debug_info.variables[index];
        if (variable->kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD &&
            variable->display_name != NULL &&
            strcmp(variable->display_name, "box") == 0 &&
            variable->parent_display_type != NULL &&
            strcmp(variable->parent_display_type, kParentDisplayType) == 0 &&
            variable->display_type != NULL &&
            strcmp(variable->display_type, kFieldDisplayType) == 0) {
            provider_field_found = true;
        }
    }
    for (size_t index = 0U; index < consumer_out.debug_info.variable_count; ++index) {
        const FengCodegenMapingVariableRecord *variable =
            &consumer_out.debug_info.variables[index];
        if (variable->kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD &&
            variable->display_name != NULL &&
            strcmp(variable->display_name, "box") == 0 &&
            variable->parent_display_type != NULL &&
            strcmp(variable->parent_display_type, kParentDisplayType) == 0 &&
            variable->display_type != NULL &&
            strcmp(variable->display_type, kFieldDisplayType) == 0) {
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
    feng_semantic_analysis_free(consumer_analysis);
    feng_program_free(consumer_program);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_graph_free(graph);
    feng_semantic_analysis_free(provider_analysis);
    feng_program_free(provider_programs[0]);
    feng_program_free(provider_programs[1]);
    feng_symbol_error_free(&symbol_error);
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

/* String literal lowering must preserve explicit bytes while emitting only
 * stable ASCII C source. This covers embedded NUL followed by a hex digit,
 * fixed-width Feng hex escapes, high bytes, and raw CRLF bytes. */
static void test_string_literal_codegen_preserves_exact_bytes(void) {
    static const char kSource[] =
        "module feng.codegen.stringbytes;\r\n"
        "func nulThenHex(): string { return \"\\0B\"; }\r\n"
        "func exactHex(): string { return \"\\x1b1\"; }\r\n"
        "func highBytes(): string { return \"\\x80\\xff\"; }\r\n"
        "func rawCrLf(): string { return `A\r\nB`; }\r\n";
    FengProgram *program = parse_or_die(
        kSource, "string_literal_exact_bytes_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(strstr(output.c_source,
                  "feng_string_literal(\"\\000B\", 2);") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_string_literal(\"\\0331\", 2);") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_string_literal(\"\\200\\377\", 2);") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_string_literal(\"A\\015\\012B\", 4);") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Assert that one generated integer operation uses its compile-time
 * intrinsic and introduces no wrapping helper or conditional branch. The
 * ordinary Feng function frame prologue and epilogue remain out of scope. */
static void assert_integer_wrapping_body(const char *c_source,
                                         const char *symbol_fragment,
                                         const char *builtin_name) {
    const char *body_start;
    const char *body_end;

    ASSERT(find_generated_function_body(c_source,
                                        symbol_fragment,
                                        &body_start,
                                        &body_end));
    ASSERT(span_contains(body_start, body_end, builtin_name));
    ASSERT(!span_contains(body_start, body_end, "if ("));
    ASSERT(!span_contains(body_start, body_end, "feng_wrap"));
}

/* Integer wrapping is defined by compiler intrinsics whose ignored overflow
 * flag must not become a runtime check. Signed and unsigned forms share the
 * same lowering; right shifts remain direct fixed-width C operations. */
static void test_integer_runtime_semantics_codegen_is_zero_cost(void) {
    static const char *kSource =
        "open module feng.codegen.integerwrap;\n"
        "open func addI8(left: i8, right: i8): i8 { return left + right; }\n"
        "open func addU8(left: u8, right: u8): u8 { return left + right; }\n"
        "open func subtractI8(left: i8, right: i8): i8 { return left - right; }\n"
        "open func subtractU8(left: u8, right: u8): u8 { return left - right; }\n"
        "open func multiplyI32(left: i32, right: i32): i32 { return left * right; }\n"
        "open func multiplyU32(left: u32, right: u32): u32 { return left * right; }\n"
        "open func shiftI32(value: i32, distance: i32): i32 { return value >> distance; }\n"
        "open func shiftU32(value: u32, distance: u32): u32 { return value >> distance; }\n";
    FengProgram *program = parse_or_die(kSource, "integer_runtime_semantics_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *body_start;
    const char *body_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    assert_integer_wrapping_body(output.c_source,
        "feng__feng__codegen__integerwrap__addI8", "__builtin_add_overflow");
    assert_integer_wrapping_body(output.c_source,
        "feng__feng__codegen__integerwrap__addU8", "__builtin_add_overflow");
    assert_integer_wrapping_body(output.c_source,
        "feng__feng__codegen__integerwrap__subtractI8", "__builtin_sub_overflow");
    assert_integer_wrapping_body(output.c_source,
        "feng__feng__codegen__integerwrap__subtractU8", "__builtin_sub_overflow");
    assert_integer_wrapping_body(output.c_source,
        "feng__feng__codegen__integerwrap__multiplyI32", "__builtin_mul_overflow");
    assert_integer_wrapping_body(output.c_source,
        "feng__feng__codegen__integerwrap__multiplyU32", "__builtin_mul_overflow");

    ASSERT(find_generated_function_body(output.c_source,
        "feng__feng__codegen__integerwrap__shiftI32", &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end, ">>"));
    ASSERT(!span_contains(body_start, body_end, "__builtin_"));
    ASSERT(!span_contains(body_start, body_end, "if ("));
    ASSERT(!span_contains(body_start, body_end, "feng_wrap"));
    ASSERT(find_generated_function_body(output.c_source,
        "feng__feng__codegen__integerwrap__shiftU32", &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end, ">>"));
    ASSERT(!span_contains(body_start, body_end, "__builtin_"));
    ASSERT(!span_contains(body_start, body_end, "if ("));
    ASSERT(!span_contains(body_start, body_end, "feng_wrap"));
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

/* Top-level generic shared bodies must close direct managed/aggregate
 * dependencies and receive descriptor-sized derived values through the out
 * ABI without falling back to an open placeholder C layout. */
static void test_generic_shared_body_direct_dependencies_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_shared_direct;\n"
        "@value\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "    open let marker: string;\n"
        "    func Box(value: T, marker: string) {\n"
        "        self.value = value;\n"
        "        self.marker = marker;\n"
        "    }\n"
        "}\n"
        "type Bucket<T> {\n"
        "    var value: T;\n"
        "    func Bucket(value: T) { self.value = value; }\n"
        "    func get(): T { return self.value; }\n"
        "}\n"
        "func leaf<T>(value: T): Box<T> {\n"
        "    let bucket = Bucket<T>(value);\n"
        "    return Box<T>(bucket.get(), \"leaf\");\n"
        "}\n"
        "func use(): string {\n"
        "    let result = leaf<string>(\"value\");\n"
        "    return result.value + result.marker;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "generic_shared_direct.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr, "codegen error (generic shared direct deps): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengFunctionDescriptor *_desc") != NULL);
    ASSERT(strstr(out.c_source, "_desc->reified_agg_deps[") != NULL);
    ASSERT(strstr(out.c_source, "_desc->reified_type_deps[") != NULL);
    ASSERT(strstr(out.c_source, "->reified_generic_params[") != NULL);
    ASSERT(strstr(out.c_source,
                  "_Alignas(max_align_t) char _val") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Method-generic shared bodies must place the owner and callable descriptors
 * before method-level generic descriptors for both instance and static ABI. */
static void test_generic_shared_method_descriptor_order_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_shared_methods;\n"
        "@value\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "    func Box(value: T) { self.value = value; }\n"
        "}\n"
        "type Bucket<T> {\n"
        "    var value: T;\n"
        "    func Bucket(value: T) { self.value = value; }\n"
        "    func get(): T { return self.value; }\n"
        "}\n"
        "type MethodOwner {\n"
        "    func MethodOwner() {}\n"
        "    func wrap<U>(value: U): Box<U> {\n"
        "        let bucket = Bucket<U>(value);\n"
        "        return Box<U>(bucket.get());\n"
        "    }\n"
        "    static func wrapStatic<U>(value: U): Box<U> {\n"
        "        let bucket = Bucket<U>(value);\n"
        "        return Box<U>(bucket.get());\n"
        "    }\n"
        "}\n"
        "type Owner<T> {\n"
        "    var value: T;\n"
        "    func Owner(value: T) { self.value = value; }\n"
        "    func pair<U>(second: U): Box<U> {\n"
        "        let first = Bucket<T>(self.value);\n"
        "        let next = Bucket<U>(second);\n"
        "        first.get();\n"
        "        return Box<U>(next.get());\n"
        "    }\n"
        "    static func pairStatic<U>(first: T, second: U): Box<U> {\n"
        "        let ownerValue = Bucket<T>(first);\n"
        "        let methodValue = Bucket<U>(second);\n"
        "        ownerValue.get();\n"
        "        return Box<U>(methodValue.get());\n"
        "    }\n"
        "}\n"
        "func use(): string {\n"
        "    let methodOwner = MethodOwner();\n"
        "    let first = methodOwner.wrap<string>(\"first\");\n"
        "    let second = MethodOwner.wrapStatic<string>(\"second\");\n"
        "    let owner = Owner<i64>(41);\n"
        "    let third = owner.pair<string>(\"third\");\n"
        "    let fourth = Owner<i64>.pairStatic<string>(42, \"fourth\");\n"
        "    return first.value + second.value + third.value + fourth.value;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_shared_methods.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr, "codegen error (generic shared methods): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "void *_self, const FengTypeDescriptor *_type_desc, "
                  "const FengFunctionDescriptor *_desc, "
                  "const FengGenericParamDescriptor *_U") != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengTypeDescriptor *_type_desc, "
                  "const FengFunctionDescriptor *_desc, "
                  "const FengGenericParamDescriptor *_U") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A closed generic type must pre-register every non-method-generic ordinary
 * method dependency when its owner shell is formed. Direct calls used to
 * hide this omission by collecting the member dep set at the call site;
 * cover no-call, static, managed/value, witness, and open-owner controls. */
static void test_closed_generic_owner_method_dependencies_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_owner_method_dependencies;\n"
        "type ManagedDep<T> {\n"
        "    open let value: T;\n"
        "    func ManagedDep(value: T) { self.value = value; }\n"
        "}\n"
        "type AlternateDep<T> {\n"
        "    open let value: T;\n"
        "    func AlternateDep(value: T) { self.value = value; }\n"
        "}\n"
        "@value\n"
        "type AggregateDep<T> {\n"
        "    open let value: T;\n"
        "    func AggregateDep(value: T) { self.value = value; }\n"
        "}\n"
        "spec Surface<T> {\n"
        "    func read(): T;\n"
        "}\n"
        "type NoSpecOwner<T> {\n"
        "    open let value: T;\n"
        "    func NoSpecOwner(value: T) { self.value = value; }\n"
        "    func hiddenDependency(): T {\n"
        "        let dep = ManagedDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "type StaticOwner<T> {\n"
        "    func StaticOwner() {}\n"
        "    static func hiddenStatic(value: T): T {\n"
        "        let dep = ManagedDep<T>(value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "type First<T>: Surface<T> {\n"
        "    open let value: T;\n"
        "    func First(value: T) { self.value = value; }\n"
        "    func read(): T {\n"
        "        let dep = ManagedDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "type Second<T>: Surface<T> {\n"
        "    open let value: T;\n"
        "    func Second(value: T) { self.value = value; }\n"
        "    func read(): T {\n"
        "        let dep = AlternateDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "@value\n"
        "type ValueOwner<T>: Surface<T> {\n"
        "    open let value: T;\n"
        "    func ValueOwner(value: T) { self.value = value; }\n"
        "    func read(): T {\n"
        "        let dep = AggregateDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "type MethodGenericOwner<T> {\n"
        "    func MethodGenericOwner() {}\n"
        "    func wrap<U>(value: U): U {\n"
        "        let dep = ManagedDep<U>(value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "func readSurface(value: Surface<int>): int {\n"
        "    return value.read();\n"
        "}\n"
        "func makeOpen<T>(value: T): NoSpecOwner<T> {\n"
        "    return NoSpecOwner<T>(value);\n"
        "}\n"
        "func use(): int {\n"
        "    let unused = NoSpecOwner<int>(40);\n"
        "    let staticOnly = StaticOwner<int>();\n"
        "    let first: Surface<int> = First<int>(1);\n"
        "    let second: Surface<int> = Second<int>(2);\n"
        "    let aggregate: Surface<int> = ValueOwner<int>(3);\n"
        "    let methodOwner = MethodGenericOwner<int>();\n"
        "    let control = methodOwner.wrap<string>(\"control\");\n"
        "    let openControl = makeOpen<int>(4);\n"
        "    unused; staticOnly; control; openControl;\n"
        "    return readSurface(first) + readSurface(second) +\n"
        "           readSurface(aggregate);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_owner_method_dependencies.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr,
                "codegen error (closed generic owner method deps): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);

    /* The no-call instance and static methods each own a closed managed
     * dependency descriptor. Their presence proves owner registration no
     * longer relies on a resolved direct call expression. */
    ASSERT(strstr(out.c_source,
                  ".name = \"hiddenDependency\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  ".name = \"hiddenStatic\", "
                  ".reified_type_deps_count = 1") != NULL);

    /* Managed implementations and the value implementation keep separate
     * descriptor domains while all witness thunks target closed wrappers. */
    ASSERT(count_substr(out.c_source,
                        ".name = \"read\", "
                        ".reified_type_deps_count = 1") >= 2U);
    ASSERT(strstr(out.c_source,
                  ".name = \"read\", "
                  ".reified_agg_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengWitness__feng__codegen__"
                  "generic_owner_method_dependencies") != NULL);

    /* Closed ordinary wrappers keep the existing owner-type plus callable
     * descriptor ABI; the method-generic control continues to receive its
     * existing `_U` descriptor. An owner slot read is intentionally not
     * required when the method has no operation that consumes one. */
    ASSERT(strstr(out.c_source,
                  "const FengTypeDescriptor *_type_desc, "
                  "const FengFunctionDescriptor *_desc") != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengGenericParamDescriptor *_U") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Generic fit methods use the same per-member descriptor ownership as type
 * methods. Cover owner-only dependencies without a direct call, independent
 * method trees, static and witness entry points, aggregate targets, and an
 * owner-plus-method generic dependency closed at the final call site. */
static void test_closed_generic_fit_member_dependencies_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_fit_member_dependencies;\n"
        "type ManagedFitDep<T> {\n"
        "    open let value: T;\n"
        "    func ManagedFitDep(value: T) { self.value = value; }\n"
        "}\n"
        "type AlternateFitDep<T> {\n"
        "    open let value: T;\n"
        "    func AlternateFitDep(value: T) { self.value = value; }\n"
        "}\n"
        "@value\n"
        "type AggregateFitDep<T> {\n"
        "    open let value: T;\n"
        "    func AggregateFitDep(value: T) { self.value = value; }\n"
        "}\n"
        "@value\n"
        "type PairFitDep<T, U> {\n"
        "    open let first: T;\n"
        "    open let second: U;\n"
        "    func PairFitDep(first: T, second: U) {\n"
        "        self.first = first;\n"
        "        self.second = second;\n"
        "    }\n"
        "}\n"
        "spec FitSurface<T> {\n"
        "    func read(): T;\n"
        "}\n"
        "type FitOwner<T> {\n"
        "    open let value: T;\n"
        "    func FitOwner(value: T) { self.value = value; }\n"
        "}\n"
        "fit FitOwner<T>: FitSurface<T> {\n"
        "    func hiddenDependency(): T {\n"
        "        let dep = ManagedFitDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    func read(): T {\n"
        "        let dep = ManagedFitDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    func alternate(): T {\n"
        "        let dep = AlternateFitDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    static func transform(value: T): T {\n"
        "        let dep = ManagedFitDep<T>(value);\n"
        "        return dep.value;\n"
        "    }\n"
        "    func wrap<U>(value: U): U {\n"
        "        let dep = PairFitDep<T, U>(self.value, value);\n"
        "        return dep.second;\n"
        "    }\n"
        "}\n"
        "@value\n"
        "type ValueFitOwner<T> {\n"
        "    open let value: T;\n"
        "    func ValueFitOwner(value: T) { self.value = value; }\n"
        "}\n"
        "fit ValueFitOwner<T>: FitSurface<T> {\n"
        "    func read(): T {\n"
        "        let dep = AggregateFitDep<T>(self.value);\n"
        "        return dep.value;\n"
        "    }\n"
        "}\n"
        "func readFit(value: FitSurface<int>): int { return value.read(); }\n"
        "func use(): int {\n"
        "    let owner = FitOwner<int>(40);\n"
        "    let view: FitSurface<int> = owner;\n"
        "    let valueView: FitSurface<int> = ValueFitOwner<int>(2);\n"
        "    let control = owner.wrap<string>(\"method\");\n"
        "    let inferred = owner.wrap(\"inferred\");\n"
        "    control; inferred;\n"
        "    return readFit(view) + owner.alternate() +\n"
        "           FitOwner<int>.transform(1) + readFit(valueView);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_fit_member_dependencies.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr,
                "codegen error (closed generic fit member deps): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  ".name = \"hiddenDependency\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  ".name = \"alternate\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  ".name = \"transform\", "
                  ".reified_type_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  ".name = \"read\", "
                  ".reified_agg_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  ".name = \"wrap\", "
                  ".reified_agg_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengFunctionDescriptor *_desc, "
                  "const FengGenericParamDescriptor *_U") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Type-owner shared bodies use one closed type descriptor for field,
 * constructor, static-initializer, and finalizer dependencies. Open generic
 * reference fields must also use the closed descriptor's physical offsets. */
static void test_generic_type_owner_reification_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_type_owner;\n"
        "@value\n"
        "type Wide {\n"
        "    open let first: string;\n"
        "    open let second: string;\n"
        "    func Wide(first: string, second: string) {\n"
        "        self.first = first;\n"
        "        self.second = second;\n"
        "    }\n"
        "}\n"
        "type Box<T> {\n"
        "    open var value: T;\n"
        "    open let marker: string;\n"
        "    func Box(marker: string) { self.marker = marker; }\n"
        "}\n"
        "func makeBox<T>(): Box<T> { return Box<T>(\"callable\"); }\n"
        "type Owner<T> {\n"
        "    open var direct = Box<T>(\"field\");\n"
        "    open var callable = makeBox<T>();\n"
        "    open var marker: string;\n"
        "    func Owner(value: T) {\n"
        "        let box = Box<T>(\"constructor\");\n"
        "        self.direct.value = value;\n"
        "        self.marker = box.marker;\n"
        "    }\n"
        "    func ~Owner() {\n"
        "        let box = Box<T>(\"finalizer\");\n"
        "        box.marker;\n"
        "    }\n"
        "}\n"
        "type StaticOwner<T> {\n"
        "    open static let value = makeBox<T>();\n"
        "}\n"
        "func use(): string {\n"
        "    let owner = Owner<Wide>(Wide(\"left\", \"right\"));\n"
        "    let arrayBox = Box<Wide[]>(\"array\");\n"
        "    return owner.marker + StaticOwner<Wide>.value.marker + arrayBox.marker;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "generic_type_owner.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr, "codegen error (generic type owner reification): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  ".reified_callable_deps_count = 1") != NULL);
    ASSERT(strstr(out.c_source,
                  "_td->reified_callable_deps[0]") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_generic_type_descriptor(_T)") != NULL);
    ASSERT(strstr(out.c_source,
                  "->default_zero_init(") != NULL);
    ASSERT(strstr(out.c_source,
                  ".default_zero_init = feng_array_default_zero_init") != NULL);
    ASSERT(strstr(out.c_source,
                  ".default_zero_init = FengTypeDesc__feng__codegen__generic_type_owner__Box") != NULL);
    ASSERT(strstr(out.c_source,
                  "->reified_field_offsets[1]") != NULL);
    ASSERT(strstr(out.c_source,
                  "(_l_box_0)->marker") == NULL);
    ASSERT(strstr(out.c_source,
                  "__finalize(void *_self)") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Statically known arrays and ordinary reference types keep the original
 * direct default-zero expressions. Descriptor dispatch is reserved for a
 * shared body whose declared value type is an unresolved generic parameter. */
static void test_non_generic_default_zero_stays_direct_codegen(void) {
    static const char *kSource =
        "module feng.codegen.direct_default_zero;\n"
        "type Plain {\n"
        "    open let text: string;\n"
        "}\n"
        "func use(): void {\n"
        "    let values: i64[];\n"
        "    let plain: Plain;\n"
        "    values;\n"
        "    plain;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "direct_default_zero.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_array_new(NULL, sizeof(int64_t), false, (size_t)0)") != NULL);
    ASSERT(strstr(out.c_source,
                  " = Feng__feng__codegen__direct_default_zero__Plain__default_zero();") != NULL);
    ASSERT(strstr(out.c_source, "->default_zero_init(") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A fully closed generic reference type also keeps its generated direct
 * factory call. The origin's shared default-zero entry may use descriptor
 * dispatch internally, but the closed call site must not be redirected to it. */
static void test_closed_generic_default_zero_stays_direct_codegen(void) {
    static const char *kSource =
        "module feng.codegen.closed_default_zero;\n"
        "type Box<T> {\n"
        "    open let value: T;\n"
        "}\n"
        "func use(): void {\n"
        "    let box: Box<i64>;\n"
        "    box;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "closed_default_zero.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  " = Feng__feng__codegen__closed_default_zero__Box__G__i64__default_zero();") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Direct generic-parameter results use descriptor-sized address storage in
 * every shared callable path.  A wide closed value must never be lowered
 * through a pointer-sized C local, including recursive and method calls. */
static void test_generic_direct_result_uses_descriptor_sized_storage_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_direct_result_storage;\n"
        "@value\n"
        "type Wide {\n"
        "    open let first: string;\n"
        "    open let second: string;\n"
        "    open let number: i64;\n"
        "    func Wide(first: string, second: string, number: i64) {\n"
        "        self.first = first;\n"
        "        self.second = second;\n"
        "        self.number = number;\n"
        "    }\n"
        "}\n"
        "func recurse<T>(value: T, count: i64): T {\n"
        "    let current: T = value;\n"
        "    if count <= 0 { return current; }\n"
        "    return recurse<T>(current, count - 1);\n"
        "}\n"
        "type Owner {\n"
        "    func Owner() {}\n"
        "    func pass<U>(value: U): U { return value; }\n"
        "    static func passStatic<U>(value: U): U { return value; }\n"
        "}\n"
        "func use(): Wide {\n"
        "    let value = Wide(\"left\", \"right\", 7);\n"
        "    let recursive = recurse<Wide>(value, 2);\n"
        "    let owner = Owner();\n"
        "    let instance = owner.pass<Wide>(recursive);\n"
        "    return Owner.passStatic<Wide>(instance);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_direct_result_storage.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr, "codegen error (generic direct result storage): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengGenericParamDescriptor *_gpd") != NULL);
    ASSERT(strstr(out.c_source,
                  "_Alignas(max_align_t) char _gr") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_cleanup_push_aggregate(&_cu_") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_generic_aggregate_descriptor(_gpd") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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
                  ".kind = FENG_VALUE_MANAGED_POINTER, .descriptor = &_feng_closed_array_desc_") != NULL);
    ASSERT(strstr(out.c_source,
                  "static const FengTypeDescriptor _feng_closed_array_desc_") != NULL);
    ASSERT(strstr(out.c_source,
                  "&(const FengTypeDescriptor){.name = \"feng.builtin.array\"") == NULL);
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

static void test_callable_spec_reference_identity_equality_codegen(void) {
    static const char *kSource =
        "module feng.codegen.callable_equality;\n"
        "spec Action(value: int): void;\n"
        "func same(left: Action, right: Action): bool {\n"
        "    return left == right;\n"
        "}\n"
        "func different(left: Action, right: Action): bool {\n"
        "    return left != right;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "callable_equality.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, " == (void *)") != NULL);
    ASSERT(strstr(out.c_source, " != (void *)") != NULL);
    compile_generated_c_or_die(out.c_source);

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

/* Computed callables from calls, indexes, methods, constrained generic
 * results, and variadic results all bridge into the existing invoke ABI. The
 * writable-array case additionally requires a retained callee snapshot before
 * an argument can replace the original element. */
static void test_computed_callable_result_call_codegen(void) {
    static const char *kSource =
        "module feng.codegen.computed_callable_result;\n"
        "spec Reader(value: int): int;\n"
        "spec ReaderFactory(base: int): Reader;\n"
        "spec Identity<T>(value: T): T;\n"
        "spec VariadicReader(prefix: int, values: int...): int;\n"
        "func makeReader(base: int): Reader {\n"
        "  return (value: int) -> base + value;\n"
        "}\n"
        "func makeReaders(base: int): Reader[] {\n"
        "  return [makeReader(base)];\n"
        "}\n"
        "func makeReaderFactory(): ReaderFactory {\n"
        "  return (base: int) -> makeReader(base);\n"
        "}\n"
        "func makeIdentity<T>(): Identity<T> {\n"
        "  return (value: T) -> value;\n"
        "}\n"
        "func forward<T: Reader>(reader: T): T { return reader; }\n"
        "func invokeForward<T: Reader>(reader: T, value: int): int {\n"
        "  return forward<T>(reader)(value);\n"
        "}\n"
        "func makeVariadic(base: int): VariadicReader {\n"
        "  return (prefix: int, values: int...) { return base + prefix; };\n"
        "}\n"
        "type Factory {\n"
        "  let base: int;\n"
        "  func reader(offset: int): Reader {\n"
        "    return makeReader(self.base + offset);\n"
        "  }\n"
        "  static func staticReader(base: int): Reader {\n"
        "    return makeReader(base);\n"
        "  }\n"
        "}\n"
        "func factory(): Factory { return Factory { base: 10 }; }\n"
        "func replace(readers: Reader[!], next: Reader): int {\n"
        "  readers[0] = next;\n"
        "  return 1;\n"
        "}\n"
        "func returned(): int { return makeReader(20)(1); }\n"
        "func indexed(): int { return makeReaders(30)[0](2); }\n"
        "func method(): int { return factory().reader(40)(3); }\n"
        "func staticMethod(): int { return Factory.staticReader(50)(4); }\n"
        "func nested(): int { return makeReaderFactory()(60)(5); }\n"
        "func genericSpec(): int { return makeIdentity<int>()(9); }\n"
        "func variadic(): int { return makeVariadic(70)(6, 7, 8); }\n"
        "func indexedMutation(): int {\n"
        "  let original: Reader = makeReader(80);\n"
        "  let replacement: Reader = makeReader(800);\n"
        "  let readers: Reader[!] = [original];\n"
        "  return readers[0](replace(readers, replacement));\n"
        "}\n"
        "func bound(reader: Reader, value: int): int {\n"
        "  return reader(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "computed_callable_result_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &out,
                                   &cgerr)) {
        fprintf(stderr,
                "codegen error (computed callable result): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "only direct or method calls") == NULL);
    ASSERT(strstr(out.c_source, "feng_retain(_callee") != NULL);
    ASSERT(strstr(out.c_source, "feng_array_check_index") != NULL);
    ASSERT(strstr(out.c_source, "->invoke(") != NULL);
    ASSERT(strstr(out.c_source, "FENG_VALUE_MANAGED_POINTER") != NULL);
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
    ASSERT(strstr(out.c_source,
                  "FengSpecValue__feng__codegen__gs1__Box__GenericABI") != NULL);
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

/* Object-form spec fields whose value is callable must first dispatch through
 * the field getter and then through the callable ABI. Cover both an explicit
 * spec value and a direct generic parameter constrained by the same spec. */
static void test_generic_object_spec_callable_field_call_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_spec_callable_field;\n"
        "@value type Payload<T> { let value: T; }\n"
        "spec Mapper<T>(value: Payload<T>): Payload<T>;\n"
        "spec HasMapper<T> {\n"
        "    let mapper: Mapper<T>;\n"
        "}\n"
        "func applySpec<T>(holder: HasMapper<T>, value: Payload<T>): Payload<T> {\n"
        "    return holder.mapper(value);\n"
        "}\n"
        "func applyConstrained<T, H: HasMapper<T>>(\n"
        "    holder: H, value: Payload<T>\n"
        "): Payload<T> {\n"
        "    return holder.mapper(value);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource,
        "generic_object_spec_callable_field_call.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    const char *first_getter;
    const char *first_invoke;

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &out,
                                   &cgerr)) {
        fprintf(stderr,
                "codegen error (generic object spec callable field): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);
    first_getter = strstr(out.c_source, "->get_mapper(");
    first_invoke = strstr(out.c_source, "->invoke(");
    ASSERT(first_getter != NULL);
    ASSERT(first_invoke != NULL);
    ASSERT(strstr(first_getter + 1, "->get_mapper(") != NULL);
    ASSERT(strstr(first_invoke + 1, "->invoke(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

static void test_open_generic_callable_field_default_codegen(void) {
    static const char *kSource =
        "module feng.codegen.open_callable_default;\n"
        "spec Action<T>(value: T): void;\n"
        "type Slot<T> {\n"
        "    let missing: T;\n"
        "}\n"
        "type Owner<T> {\n"
        "    let slot: Slot<Action<T>>;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "open_callable_default.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
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
        ASSERT(strstr(out.c_source,
                      "FengSpecValue__feng__codegen__gs3__Box__GenericABI") != NULL);
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

/* An object-form spec method value captures the existing subject and witness
 * in one callable closure. Binding and explicit conversion share the same
 * generated support and dispatch directly through the selected witness slot. */
static void test_object_spec_method_value_codegen_uses_bound_witness(void) {
    const char *source =
        "module feng.codegen.object_spec_method_value;\n"
        "spec BaseReadable { func read(offset: int): int; }\n"
        "spec ChildReadable: BaseReadable {}\n"
        "spec Reader(offset: int): int;\n"
        "func bind(value: ChildReadable): Reader {\n"
        "    return value.read;\n"
        "}\n"
        "func bindCast(value: ChildReadable): Reader {\n"
        "    return (Reader)value.read;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "object_spec_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* Both formation sites reuse one compile-time support record. At runtime
     * each selected path performs exactly the one language-level callable
     * closure allocation and no additional object-spec box allocation. */
    ASSERT(count_substr(output.c_source,
                        "feng_object_new(&FengSpecMethodValueBind__") == 1U);
    for (const char *line = output.c_source;
         (line = strstr(line, "feng_object_new(&")) != NULL;
         ++line) {
        const char *line_end = strchr(line, '\n');

        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        ASSERT(!span_contains(line, line_end, "__spec_box"));
    }

    /* The closure retains only the existing subject; the immutable witness is
     * copied as a borrowed pointer and every call enters its resolved slot. */
    ASSERT(strstr(output.c_source,
                  "feng_assign(&_o->_self, _receiver->subject)") != NULL);
    ASSERT(strstr(output.c_source,
                  "_o->_witness = _receiver->witness") != NULL);
    ASSERT(strstr(output.c_source,
                  "_bound->_witness->read(_bound->_self, _arg0)") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* An intersection-form receiver reuses the object-spec method-value closure
 * shape while binding its existing merged witness. Different member slots are
 * selected at compile time and no component spec view is materialized. */
static void test_intersection_spec_method_value_codegen_uses_merged_witness(void) {
    const char *source =
        "module feng.codegen.intersection_spec_method_value;\n"
        "spec Readable<T> { func read(offset: T): T; }\n"
        "spec Traceable { func trace(offset: int): int; }\n"
        "spec Both<T>: Readable<T> & Traceable;\n"
        "spec Mapper(value: int): int;\n"
        "type Value: Readable<int>, Traceable {\n"
        "  let base: int;\n"
        "  func read(offset: int): int { return self.base + offset; }\n"
        "  func trace(offset: int): int { return self.base - offset; }\n"
        "}\n"
        "func bindRead(value: Both<int>): Mapper { return value.read; }\n"
        "func bindTrace(value: Both<int>): Mapper { return value.trace; }\n"
        "func run(): int {\n"
        "  let value: Both<int> = Value { base: 10 };\n"
        "  let read = bindRead(value);\n"
        "  let trace = bindTrace(value);\n"
        "  return read(2) + trace(3);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "intersection_spec_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* One support closure is emitted for each selected merged slot. Each binds
     * only the subject plus the already-built intersection witness. */
    ASSERT(count_substr(output.c_source,
                        "feng_object_new(&FengSpecMethodValueBind__") == 2U);
    ASSERT(count_substr(output.c_source,
                        "feng_assign(&_o->_self, _receiver->subject)") == 2U);
    ASSERT(count_substr(output.c_source,
                        "_o->_witness = _receiver->witness") == 2U);
    ASSERT(strstr(output.c_source,
                  "_bound->_witness->read(_bound->_self, (const void *)&_arg0, &_result)") != NULL);
    ASSERT(strstr(output.c_source,
                  "_bound->_witness->trace(_bound->_self, _arg0)") != NULL);
    ASSERT(strstr(output.c_source, "__spec_box") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Legal same-name intersection overloads keep the historical first-name
 * witness prefix and append one deterministic slot per distinct signature.
 * Direct calls and method values both consume Semantic's exact declaration
 * identity, while the merged constant aliases the original leaf witnesses. */
static void test_intersection_spec_overload_codegen_preserves_exact_slots(void) {
    const char *source =
        "module feng.codegen.intersection_spec_overload;\n"
        "spec Numeric {\n"
        "  func select(value: int): int;\n"
        "  func stable(): int;\n"
        "}\n"
        "spec Textual { func select(value: string): string; }\n"
        "spec Both: Numeric & Textual;\n"
        "spec IntMapper(value: int): int;\n"
        "spec StringMapper(value: string): string;\n"
        "type Value: Numeric, Textual {\n"
        "  func select(value: int): int { return value + 1; }\n"
        "  func select(value: string): string { return value; }\n"
        "  func stable(): int { return 7; }\n"
        "}\n"
        "func bindInt(value: Both): IntMapper { return value.select; }\n"
        "func bindString(value: Both): StringMapper { return value.select; }\n"
        "func directString(value: Both): string {\n"
        "  return value.select(\"direct\");\n"
        "}\n"
        "func run(): string {\n"
        "  let value: Both = Value {};\n"
        "  let selected = bindString(value);\n"
        "  return selected(directString(value));\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "intersection_spec_overload_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *witness_start;
    const char *witness_end;
    const char *primary_slot;
    const char *stable_slot;
    const char *overload_slot;
    const char *overload_init;
    const char *overload_init_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    witness_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__intersection_spec_overload__Both {\n");
    ASSERT(witness_start != NULL);
    witness_end = strstr(witness_start, "};\n");
    ASSERT(witness_end != NULL);
    primary_slot = strstr(witness_start, "(*select)(");
    stable_slot = strstr(witness_start, "(*stable)(");
    overload_slot = strstr(witness_start,
                           "(*select__feng_overload_2)(");
    ASSERT(primary_slot != NULL && primary_slot < witness_end);
    ASSERT(stable_slot != NULL && stable_slot < witness_end);
    ASSERT(overload_slot != NULL && overload_slot < witness_end);
    ASSERT(primary_slot < stable_slot && stable_slot < overload_slot);

    overload_init = output.c_source;
    do {
        overload_init = strstr(overload_init,
                               ".select__feng_overload_2 = ");
        ASSERT(overload_init != NULL);
        overload_init_end = strchr(overload_init, '\n');
        ASSERT(overload_init_end != NULL);
        if (span_contains(overload_init,
                          overload_init_end,
                          "__Textual.select")) {
            break;
        }
        overload_init = overload_init_end;
    } while (true);
    ASSERT(span_contains(overload_init,
                         overload_init_end,
                         "__Textual.select"));
    ASSERT(strstr(output.c_source,
                  "_bound->_witness->select__feng_overload_2(") != NULL);
    ASSERT(strstr(output.c_source,
                  ".witness->select__feng_overload_2(") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Object-form parent accumulation preserves each legal overload by exact
 * requirement identity. Equivalent generic parent/child declarations share
 * the historical primary slot with the child's concrete ABI, while instance
 * and static same-name requirements remain separate compile-time slots. */
static void test_object_spec_overload_codegen_preserves_exact_slots(void) {
    const char *source =
        "module feng.codegen.object_spec_overload;\n"
        "spec Base<T> {\n"
        "  func choose(value: T): T;\n"
        "  static func create(value: T): T;\n"
        "}\n"
        "spec Child: Base<i32> {\n"
        "  func choose(value: i32): i32;\n"
        "  func choose(value: string): string;\n"
        "  static func create(value: i32): i32;\n"
        "  static func create(value: string): string;\n"
        "}\n"
        "spec Extra: Base<bool> {\n"
        "  func choose(value: bool): bool;\n"
        "  static func create(value: bool): bool;\n"
        "}\n"
        "spec Combined: Child, Extra {}\n"
        "spec Dual { func marker(): i32; static func marker(): string; }\n"
        "spec StringMapper(value: string): string;\n"
        "type Choice: Combined, Dual {\n"
        "  func choose(value: i32): i32 { return value + 1; }\n"
        "  func choose(value: string): string { return value; }\n"
        "  func choose(value: bool): bool { return value; }\n"
        "  static func create(value: i32): i32 { return value + 1; }\n"
        "  static func create(value: string): string { return value; }\n"
        "  static func create(value: bool): bool { return value; }\n"
        "  func marker(): i32 { return 7; }\n"
        "  static func marker(): string { return \"static\"; }\n"
        "}\n"
        "func bindText(value: Combined): StringMapper { return value.choose; }\n"
        "func bindStaticText<T: Combined>(): StringMapper { return T.create; }\n"
        "func direct(value: Combined): string {\n"
        "  if (value.choose(1) == 2 && value.choose(true)) {\n"
        "    return value.choose(\"direct\");\n"
        "  }\n"
        "  return \"failed\";\n"
        "}\n"
        "func staticText<T: Combined>(): string { return T.create(\"text\"); }\n"
        "func staticBool<T: Combined>(): bool { return T.create(true); }\n"
        "func instanceMarker(value: Dual): i32 { return value.marker(); }\n"
        "func staticMarker<T: Dual>(): string { return T.marker(); }\n";
    FengProgram *program = parse_or_die(
        source, "object_spec_overload_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *child_start;
    const char *child_end;
    const char *combined_start;
    const char *combined_end;
    const char *dual_start;
    const char *dual_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    child_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__object_spec_overload__Child {\n");
    ASSERT(child_start != NULL);
    child_end = strstr(child_start, "};\n");
    ASSERT(child_end != NULL);
    ASSERT(count_substr_in_span(child_start, child_end, "(*choose)(") == 1U);
    ASSERT(count_substr_in_span(child_start, child_end,
                                "(*choose__feng_overload_2)(") == 1U);
    ASSERT(count_substr_in_span(child_start, child_end, "(*create)(") == 1U);
    ASSERT(count_substr_in_span(child_start, child_end,
                                "(*create__feng_overload_2)(") == 1U);
    ASSERT(span_contains(child_start, child_end,
                         "int32_t (*choose)(void *_subject, int32_t);"));
    ASSERT(span_contains(child_start, child_end,
                         "int32_t (*create)(int32_t);"));

    combined_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__object_spec_overload__Combined {\n");
    ASSERT(combined_start != NULL);
    combined_end = strstr(combined_start, "};\n");
    ASSERT(combined_end != NULL);
    ASSERT(count_substr_in_span(combined_start, combined_end, "(*choose)(") == 1U);
    ASSERT(count_substr_in_span(combined_start, combined_end,
                                "(*choose__feng_overload_2)(") == 1U);
    ASSERT(count_substr_in_span(combined_start, combined_end,
                                "(*choose__feng_overload_3)(") == 1U);
    ASSERT(count_substr_in_span(combined_start, combined_end, "(*create)(") == 1U);
    ASSERT(count_substr_in_span(combined_start, combined_end,
                                "(*create__feng_overload_2)(") == 1U);
    ASSERT(count_substr_in_span(combined_start, combined_end,
                                "(*create__feng_overload_3)(") == 1U);

    dual_start = strstr(
        output.c_source,
        "struct FengSpecWitness__feng__codegen__object_spec_overload__Dual {\n");
    ASSERT(dual_start != NULL);
    dual_end = strstr(dual_start, "};\n");
    ASSERT(dual_end != NULL);
    ASSERT(count_substr_in_span(dual_start, dual_end, "(*marker)(") == 1U);
    ASSERT(count_substr_in_span(dual_start, dual_end,
                                "(*marker__feng_overload_2)(") == 1U);

    ASSERT(strstr(output.c_source,
                  ".witness->choose__feng_overload_2(") != NULL);
    ASSERT(strstr(output.c_source,
                  ".witness->choose__feng_overload_3(") != NULL);
    ASSERT(strstr(output.c_source,
                  "_bound->_witness->choose__feng_overload_2(") != NULL);
    ASSERT(strstr(output.c_source,
                  ")->create__feng_overload_2(") != NULL);
    ASSERT(strstr(output.c_source,
                  ")->create__feng_overload_3(") != NULL);
    ASSERT(strstr(output.c_source,
                  ")->marker__feng_overload_2()") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Different closures of one generic parent declaration must also keep their
 * exact implementation when the parent witness is materialized independently
 * of the combined child surface. Cover both type-owned and fit-owned methods;
 * a wrong declaration-only lookup still produces valid C but forwards bool
 * storage through the i32 implementation ABI. */
static void test_object_spec_closed_parent_witness_uses_exact_implementation(void) {
    const char *source =
        "module feng.codegen.object_spec_closed_parent_witness;\n"
        "spec Base<T> {\n"
        "  func choose(value: T): T;\n"
        "  static func build(value: T): T;\n"
        "}\n"
        "spec Number: Base<i32> {}\n"
        "spec Flag: Base<bool> {}\n"
        "spec Combined: Number, Flag {}\n"
        "type Choice: Combined {\n"
        "  func choose(value: i32): i32 { return value + 1; }\n"
        "  func choose(value: bool): bool { return !value; }\n"
        "  static func build(value: i32): i32 { return value + 2; }\n"
        "  static func build(value: bool): bool { return !value; }\n"
        "}\n"
        "type FitChoice {}\n"
        "fit FitChoice: Combined {\n"
        "  func choose(value: i32): i32 { return value + 3; }\n"
        "  func choose(value: bool): bool { return value; }\n"
        "  static func build(value: i32): i32 { return value + 4; }\n"
        "  static func build(value: bool): bool { return value; }\n"
        "}\n"
        "func staticBool<T: Base<bool>>(value: bool): bool {\n"
        "  return T.build(value);\n"
        "}\n"
        "func runChoice(): bool {\n"
        "  let value: Base<bool> = Choice {};\n"
        "  return value.choose(true) || staticBool<Choice>(true);\n"
        "}\n"
        "func runFit(): bool {\n"
        "  let value: Base<bool> = FitChoice {};\n"
        "  return value.choose(true) || staticBool<FitChoice>(true);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "object_spec_closed_parent_witness_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *choice_instance;
    const char *choice_static;
    const char *fit_instance;
    const char *fit_static;
    const char *function_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    choice_instance = strstr(
        output.c_source,
        "__Choice__as__feng__codegen__object_spec_closed_parent_witness__Base_bool___choose(void *_subject, const void * p0, void *_out) {\n");
    ASSERT(choice_instance != NULL);
    function_end = strstr(choice_instance, "\n}\n");
    ASSERT(function_end != NULL);
    ASSERT(span_contains(choice_instance, function_end, "bool _ret ="));
    ASSERT(span_contains(choice_instance, function_end, "__choose__from__b("));
    ASSERT(!span_contains(choice_instance, function_end, "int32_t _ret ="));

    choice_static = strstr(
        output.c_source,
        "__Choice__as__feng__codegen__object_spec_closed_parent_witness__Base_bool___build(const void * p0, void *_out) {\n");
    ASSERT(choice_static != NULL);
    function_end = strstr(choice_static, "\n}\n");
    ASSERT(function_end != NULL);
    ASSERT(span_contains(choice_static, function_end, "bool _ret ="));
    ASSERT(span_contains(choice_static, function_end,
                         "__static__build__from__b("));

    fit_instance = strstr(
        output.c_source,
        "__FitChoice__as__feng__codegen__object_spec_closed_parent_witness__Base_bool___choose(void *_subject, const void * p0, void *_out) {\n");
    ASSERT(fit_instance != NULL);
    function_end = strstr(fit_instance, "\n}\n");
    ASSERT(function_end != NULL);
    ASSERT(span_contains(fit_instance, function_end, "bool _ret ="));
    ASSERT(span_contains(fit_instance, function_end, "__choose__from__b("));
    ASSERT(!span_contains(fit_instance, function_end, "int32_t _ret ="));

    fit_static = strstr(
        output.c_source,
        "__FitChoice__as__feng__codegen__object_spec_closed_parent_witness__Base_bool___build(const void * p0, void *_out) {\n");
    ASSERT(fit_static != NULL);
    function_end = strstr(fit_static, "\n}\n");
    ASSERT(function_end != NULL);
    ASSERT(span_contains(fit_static, function_end, "bool _ret ="));
    ASSERT(span_contains(fit_static, function_end, "__build__from__b"));
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* User and builtin fits may implement same-name instance and static
 * requirements. Their implementation symbols must remain distinct while the
 * generated witness calls retain the existing direct-call runtime shape. */
static void test_fit_instance_static_same_name_codegen_symbols_are_distinct(void) {
    const char *source =
        "module feng.codegen.fit_method_domain;\n"
        "spec Dual { func marker(): i32; static func marker(): string; }\n"
        "type Value {}\n"
        "fit Value: Dual {\n"
        "  func marker(): i32 { return 7; }\n"
        "  static func marker(): string { return \"user-static\"; }\n"
        "}\n"
        "fit i32: Dual {\n"
        "  func marker(): i32 { return self; }\n"
        "  static func marker(): string { return \"builtin-static\"; }\n"
        "}\n"
        "func instanceMarker(value: Dual): i32 { return value.marker(); }\n"
        "func staticMarker<T: Dual>(): string { return T.marker(); }\n"
        "func userRun(): string {\n"
        "  let value: Dual = Value {};\n"
        "  if (instanceMarker(value) == 7) { return staticMarker<Value>(); }\n"
        "  return \"failed\";\n"
        "}\n"
        "func builtinRun(): string {\n"
        "  let number: i32 = 9;\n"
        "  let value: Dual = number;\n"
        "  if (instanceMarker(value) == 9) { return staticMarker<i32>(); }\n"
        "  return \"failed\";\n"
        "}\n";
    FengProgram *program = parse_or_die(source, "fit_method_domain_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "__marker__from__void__static(void)") != NULL);
    ASSERT(strstr(output.c_source,
                  "__marker__from__void__static(const FengFunctionDescriptor *_desc") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A shared T: ObjectSpec binder consumes one statically slotted closed
 * descriptor. Concrete reference, trivial and descriptor-sized receivers all
 * use one closure allocation, with no object-spec box or escaping stack
 * address. */
static void test_constrained_generic_spec_method_value_codegen(void) {
    const char *source =
        "module feng.codegen.constrained_generic_spec_method_value;\n"
        "spec Stateful { func step(delta: i32): i32; }\n"
        "spec Stepper(delta: i32): i32;\n"
        "type ReferenceCounter: Stateful {\n"
        "    var value: i32;\n"
        "    func step(delta: i32): i32 {\n"
        "        self.value += delta;\n"
        "        return self.value;\n"
        "    }\n"
        "}\n"
        "fit i32: Stateful {\n"
        "    func step(delta: i32): i32 { return self + delta; }\n"
        "}\n"
        "@value type ValueCounter: Stateful {\n"
        "    let label: string;\n"
        "    var value: i32;\n"
        "    func step(delta: i32): i32 {\n"
        "        self.value += delta;\n"
        "        return self.value;\n"
        "    }\n"
        "}\n"
        "func bind<T: Stateful>(value: T): Stepper {\n"
        "    return value.step;\n"
        "}\n"
        "func run(): i32 {\n"
        "    let reference = ReferenceCounter { value: 10 };\n"
        "    let referenceStep = bind<ReferenceCounter>(reference);\n"
        "    let trivialStep = bind<i32>(40);\n"
        "    let aggregate = ValueCounter { label: \"value\", value: 50 };\n"
        "    let aggregateStep = bind<ValueCounter>(aggregate);\n"
        "    return referenceStep(1) + trivialStep(2) + aggregateStep(3);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "constrained_generic_spec_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* The shared binder contains one formation site and one representation
     * dispatch. It allocates only the final callable closure. */
    ASSERT(count_substr(output.c_source,
                        "const FengCallableValueDescriptor *_callable_value_desc") ==
           1U);
    ASSERT(count_substr(output.c_source,
                        "feng_object_new(_callable_value_desc") == 1U);
    ASSERT(count_substr(output.c_source, "switch (_T->kind)") == 1U);
    ASSERT(strstr(output.c_source, "case FENG_VALUE_TRIVIAL:") != NULL);
    ASSERT(strstr(output.c_source,
                  "case FENG_VALUE_MANAGED_POINTER:") != NULL);
    ASSERT(strstr(output.c_source,
                  "case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:") != NULL);

    /* Closing the same source dependency for all three receiver value kinds
     * produces compile-time descriptors and direct witness-slot adapters. */
    ASSERT(strstr(output.c_source,
                  ".kind = FENG_VALUE_MANAGED_POINTER") != NULL);
    ASSERT(strstr(output.c_source,
                  ".kind = FENG_VALUE_TRIVIAL") != NULL);
    ASSERT(strstr(output.c_source,
                  ".kind = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS") != NULL);
    ASSERT(count_substr(output.c_source,
                        ".aggregate_capture_offset = offsetof(") == 2U);
    ASSERT(count_substr(output.c_source,
                        ".aggregate_capture_desc =") == 1U);
    ASSERT(strstr(output.c_source,
                  "_witness->step(_bound->_self") != NULL);
    ASSERT(strstr(output.c_source,
                  "_witness->step(&_bound->_receiver") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_aggregate_release(&_o->_receiver") != NULL);
    ASSERT(strstr(output.c_source,
                  "_self = (void *)_p_value") == NULL);

    for (const char *line = output.c_source;
         (line = strstr(line, "feng_object_new(&")) != NULL;
         ++line) {
        const char *line_end = strchr(line, '\n');

        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        ASSERT(!span_contains(line, line_end, "__spec_box"));
    }
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A shared T: IntersectionSpec binder uses the same receiver-value closure as
 * MV02 and closes its adapter against the existing merged witness from MV05.
 * Reference, trivial and descriptor-sized values require no spec box, second
 * receiver allocation or runtime member lookup. */
static void test_constrained_generic_intersection_method_value_codegen(void) {
    const char *source =
        "module feng.codegen.constrained_generic_intersection_method_value;\n"
        "spec Stateful {\n"
        "  func step(delta: i32): i32;\n"
        "  func select(delta: i32): i32;\n"
        "}\n"
        "spec Named {\n"
        "  func name(): string;\n"
        "  func select(label: string): string;\n"
        "}\n"
        "spec Both: Stateful & Named;\n"
        "spec Stepper(delta: i32): i32;\n"
        "spec StringMapper(label: string): string;\n"
        "type ReferenceCounter: Stateful, Named {\n"
        "  var value: i32;\n"
        "  func step(delta: i32): i32 {\n"
        "    self.value += delta;\n"
        "    return self.value;\n"
        "  }\n"
        "  func select(delta: i32): i32 { return self.value + delta; }\n"
        "  func name(): string { return \"reference\"; }\n"
        "  func select(label: string): string { return label; }\n"
        "}\n"
        "fit i32: Stateful, Named {\n"
        "  func step(delta: i32): i32 { return self + delta; }\n"
        "  func select(delta: i32): i32 { return self + delta; }\n"
        "  func name(): string { return \"trivial\"; }\n"
        "  func select(label: string): string { return label; }\n"
        "}\n"
        "fit i32[]: Stateful, Named {\n"
        "  func step(delta: i32): i32 { return self[0] + delta; }\n"
        "  func select(delta: i32): i32 { return self[0] + delta; }\n"
        "  func name(): string { return \"array\"; }\n"
        "  func select(label: string): string { return label; }\n"
        "}\n"
        "@value type ValueCounter: Stateful, Named {\n"
        "  let label: string;\n"
        "  var value: i32;\n"
        "  func step(delta: i32): i32 {\n"
        "    self.value += delta;\n"
        "    return self.value;\n"
        "  }\n"
        "  func select(delta: i32): i32 { return self.value + delta; }\n"
        "  func name(): string { return self.label; }\n"
        "  func select(label: string): string { return label; }\n"
        "}\n"
        "func bind<T: Both>(value: T): Stepper { return value.step; }\n"
        "func bindText<T: Both>(value: T): StringMapper {\n"
        "  return value.select;\n"
        "}\n"
        "func run(): i32 {\n"
        "  let reference = ReferenceCounter { value: 10 };\n"
        "  let referenceStep = bind<ReferenceCounter>(reference);\n"
        "  let trivialStep = bind<i32>(40);\n"
        "  let arrayStep = bind<i32[]>([45]);\n"
        "  let aggregate = ValueCounter { label: \"value\", value: 50 };\n"
        "  let aggregateStep = bind<ValueCounter>(aggregate);\n"
        "  return referenceStep(1) + trivialStep(2) + arrayStep(3) + "
        "aggregateStep(4);\n"
        "}\n"
        "func runText(): string {\n"
        "  let value = ReferenceCounter { value: 60 };\n"
        "  let selected = bindText<ReferenceCounter>(value);\n"
        "  return selected(\"picked\");\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source,
        "constrained_generic_intersection_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &output,
                                   &codegen_error)) {
        fprintf(stderr,
                "MV06 intersection method-value codegen error: %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "(unknown)");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);

    /* Each shared formation site performs one representation switch and one
     * language-level callable allocation. All closed types reuse that code. */
    ASSERT(count_substr(output.c_source,
                        "const FengCallableValueDescriptor *_callable_value_desc") ==
           2U);
    ASSERT(count_substr(output.c_source,
                        "feng_object_new(_callable_value_desc") == 2U);
    ASSERT(count_substr(output.c_source, "switch (_T->kind)") == 2U);
    ASSERT(strstr(output.c_source, "case FENG_VALUE_TRIVIAL:") != NULL);
    ASSERT(strstr(output.c_source,
                  "case FENG_VALUE_MANAGED_POINTER:") != NULL);
    ASSERT(strstr(output.c_source,
                  "case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:") != NULL);

    /* Closed adapters enter the compile-time selected merged slots directly;
     * the legal string overload uses the deterministic appended slot. */
    ASSERT(strstr(output.c_source,
                  "_witness->step(_bound->_self") != NULL);
    ASSERT(strstr(output.c_source,
                  "_witness->step(&_bound->_receiver") != NULL);
    ASSERT(strstr(output.c_source,
                  "_witness->select__feng_overload_2(") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_aggregate_release(&_o->_receiver") != NULL);
    for (const char *line = output.c_source;
         (line = strstr(line, "feng_object_new(&")) != NULL;
         ++line) {
        const char *line_end = strchr(line, '\n');

        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        ASSERT(!span_contains(line, line_end, "__spec_box"));
    }
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* IC01: an intersection-constrained static call consumes the exact Semantic
 * requirement identity from the existing merged witness. Nested surfaces,
 * parent requirements, legal overload slots, and different closed T values
 * all remain receiver/subject-free. */
static void test_intersection_constrained_static_method_call_codegen(void) {
    const char *source =
        "module feng.codegen.intersection_static_call;\n"
        "spec RootFactory<T> {\n"
        "  static func inherited(value: T): T;\n"
        "}\n"
        "spec NumberFactory: RootFactory<i32> {\n"
        "  static func create(seed: i32): i32;\n"
        "  static func select(value: i32): i32;\n"
        "}\n"
        "spec TextFactory {\n"
        "  static func label(): string;\n"
        "  static func select(value: string): string;\n"
        "}\n"
        "spec Combined: NumberFactory & TextFactory;\n"
        "spec Tagged { static func tag(): string; }\n"
        "spec Nested: Combined & Tagged;\n"
        "type First: NumberFactory, TextFactory, Tagged {\n"
        "  static func inherited(value: i32): i32 { return value + 10; }\n"
        "  static func create(seed: i32): i32 { return seed + 1; }\n"
        "  static func select(value: i32): i32 { return value + 2; }\n"
        "  static func label(): string { return \"first\"; }\n"
        "  static func select(value: string): string { return value; }\n"
        "  static func tag(): string { return \"one\"; }\n"
        "}\n"
        "type Second: NumberFactory, TextFactory, Tagged {\n"
        "  static func inherited(value: i32): i32 { return value + 20; }\n"
        "  static func create(seed: i32): i32 { return seed + 3; }\n"
        "  static func select(value: i32): i32 { return value + 4; }\n"
        "  static func label(): string { return \"second\"; }\n"
        "  static func select(value: string): string { return value; }\n"
        "  static func tag(): string { return \"two\"; }\n"
        "}\n"
        "func create<T: Nested>(seed: i32): i32 { return T.create(seed); }\n"
        "func inherited<T: Nested>(value: i32): i32 {\n"
        "  return T.inherited(value);\n"
        "}\n"
        "func selectNumber<T: Nested>(value: i32): i32 {\n"
        "  return T.select(value);\n"
        "}\n"
        "func label<T: Nested>(): string { return T.label(); }\n"
        "func selectText<T: Nested>(value: string): string {\n"
        "  return T.select(value);\n"
        "}\n"
        "func run(): i32 {\n"
        "  return create<First>(1) + create<Second>(2) +\n"
        "         inherited<First>(3) + inherited<Second>(4) +\n"
        "         selectNumber<First>(5) + selectNumber<Second>(6);\n"
        "}\n"
        "func runText(): string {\n"
        "  return label<First>() + selectText<Second>(\"!\");\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "intersection_constrained_static_method_call_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *required_calls[] = {
        "->witness)->create(",
        "->witness)->inherited(",
        "->witness)->select(",
        "->witness)->label(",
        "->witness)->select__feng_overload_2("
    };

    if (!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                               &analysis, &errors, &error_count)) {
        fprintf(stderr,
                "IC01 intersection static-call semantic error: %s\n",
                error_count > 0U && errors[0].message != NULL
                    ? errors[0].message
                    : "(unknown)");
        ASSERT(false);
    }
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &output,
                                   &codegen_error)) {
        fprintf(stderr,
                "IC01 intersection static-call codegen error: %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "(unknown)");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);
    for (size_t index = 0U;
         index < sizeof(required_calls) / sizeof(required_calls[0]);
         ++index) {
        const char *call = strstr(output.c_source, required_calls[index]);
        const char *line_start;
        const char *line_end;

        ASSERT(call != NULL);
        line_start = call;
        while (line_start > output.c_source && line_start[-1] != '\n') {
            --line_start;
        }
        line_end = strchr(call, '\n');
        if (line_end == NULL) {
            line_end = output.c_source + strlen(output.c_source);
        }
        ASSERT(!span_contains(line_start, line_end, "_subject"));
        ASSERT(!span_contains(line_start, line_end, "feng_object_new"));
    }
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A concrete enum element in an array fit retains its nominal trivial
 * representation. Codegen must not synthesize a fit-local descriptor or
 * lower self[index] through the generic void-pointer element path. */
static void test_concrete_enum_array_fit_codegen(void) {
    const char *source =
        "module feng.codegen.concrete_enum_array_fit;\n"
        "enum Element { First }\n"
        "spec Readable { func read(): i32; }\n"
        "fit Element[!]: Readable {\n"
        "  func read(): i32 {\n"
        "    return if self[0] == Element.First { 1 } else { 0 };\n"
        "  }\n"
        "}\n"
        "func run(value: Element[!]): i32 { return value.read(); }\n";
    FengProgram *program = parse_or_die(
        source, "concrete_enum_array_fit_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &output,
                                   &codegen_error)) {
        fprintf(stderr,
                "concrete enum array-fit codegen error: %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "(unknown)");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "const FengGenericParamDescriptor *_Element") == NULL);
    ASSERT(strstr(output.c_source,
                  "FengEnumDesc__feng__codegen__concrete_enum_array_fit__Element") !=
           NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A shared T: ObjectSpec static method binder closes to one receiver-free
 * immortal callable per concrete owner/target pair. Its adapter enters the
 * exact witness slot without a subject; scalar and aggregate returns use the
 * existing callable ABI, including a requirement inherited from a generic
 * parent spec. */
static void test_constrained_generic_spec_static_method_value_codegen(void) {
    const char *source =
        "module feng.codegen.constrained_generic_spec_static_method_value;\n"
        "@value type Pair {\n"
        "    let first: string;\n"
        "    let second: string;\n"
        "    let value: i32;\n"
        "}\n"
        "spec Factory {\n"
        "    static func create(seed: i32): i32;\n"
        "    static func pair(): Pair;\n"
        "}\n"
        "spec Creator(seed: i32): i32;\n"
        "spec PairCreator(): Pair;\n"
        "type First: Factory {\n"
        "    static func create(seed: i32): i32 { return seed + 1; }\n"
        "    static func pair(): Pair {\n"
        "        return Pair { first: \"first\", second: \"one\", value: 11 };\n"
        "    }\n"
        "}\n"
        "type Second: Factory {\n"
        "    static func create(seed: i32): i32 { return seed + 2; }\n"
        "    static func pair(): Pair {\n"
        "        return Pair { first: \"second\", second: \"two\", value: 12 };\n"
        "    }\n"
        "}\n"
        "func bind<T: Factory>(): Creator { return T.create; }\n"
        "func bindPair<T: Factory>(): PairCreator { return T.pair; }\n"
        "func direct<T: Factory>(seed: i32): i32 { return T.create(seed); }\n"
        "spec GenericFactory<T> { static func map(value: T): T; }\n"
        "spec MappedFactory<Unused, Value>: GenericFactory<Value> {}\n"
        "spec IntMapper(value: i32): i32;\n"
        "type Inherited: MappedFactory<string, i32> {\n"
        "    static func map(value: i32): i32 { return value + 3; }\n"
        "}\n"
        "func bindInherited<T: MappedFactory<string, i32>>(): IntMapper {\n"
        "    return T.map;\n"
        "}\n"
        "func run(): i32 {\n"
        "    let first = bind<First>();\n"
        "    let second = bind<Second>();\n"
        "    let firstPair = bindPair<First>();\n"
        "    let secondPair = bindPair<Second>();\n"
        "    let inherited = bindInherited<Inherited>();\n"
        "    return first(10) + second(10) + firstPair().value +\n"
        "        secondPair().value + inherited(10) + direct<First>(1);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source,
        "constrained_generic_spec_static_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    if (!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                               &analysis, &errors, &error_count)) {
        for (size_t index = 0U; index < error_count; ++index) {
            fprintf(stderr,
                    "MV04 semantic error %s: %s\n",
                    errors[index].code,
                    errors[index].message != NULL
                        ? errors[index].message
                        : "(unknown)");
        }
        ASSERT(false);
    }
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* Five closed source/target pairs use static callable storage: two
     * scalar factories, two aggregate factories and one inherited generic
     * requirement. The shared formation sites only read descriptor slots. */
    ASSERT(count_substr(
               output.c_source,
               ".callable_value = {.static_value = &FengCallableSpecStaticValue__") ==
           5U);
    ASSERT(count_substr(output.c_source,
                        ".refcount = FENG_REFCOUNT_IMMORTAL") >= 5U);
    ASSERT(strstr(output.c_source,
                  "->callable_value.static_value") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_object_new(_callable_value_desc") == NULL);
    ASSERT(strstr(output.c_source, "switch (_T->kind)") == NULL);

    /* Static adapters do not synthesize a subject. Pair is a fixed nominal
     * return type, so its existing callable ABI returns it directly. */
    ASSERT(strstr(output.c_source,
                  "_witness->create(_arg0)") != NULL);
    ASSERT(strstr(output.c_source,
                  "return _witness->pair()") != NULL);
    ASSERT(strstr(output.c_source,
                  "_witness->map((const void *)&_arg0, &_result)") != NULL);
    ASSERT(strstr(output.c_source,
                  "return _result;") != NULL);
    ASSERT(strstr(output.c_source,
                  ".reified_callable_deps_count = 1") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* MV07 closes an intersection-constrained static method value through the
 * existing merged witness and MV04's immortal receiver-free callable. Exact
 * direct, parent, nested, overloaded and equivalent requirements must use
 * their stable merged slots without a subject or runtime member selection. */
static void test_intersection_constrained_static_method_value_codegen(void) {
    const char *source =
        "module feng.codegen.intersection_static_method_value;\n"
        "spec RootFactory<T> {\n"
        "  static func inherited(value: T): T;\n"
        "  static func duplicate(value: T): T;\n"
        "}\n"
        "spec NumberFactory: RootFactory<i32> {\n"
        "  static func create(seed: i32): i32;\n"
        "  static func select(value: i32): i32;\n"
        "  static func duplicate(value: i32): i32;\n"
        "}\n"
        "spec TextFactory {\n"
        "  static func label(): string;\n"
        "  static func select(value: string): string;\n"
        "  static func duplicate(value: i32): i32;\n"
        "}\n"
        "spec CombinedFactory: NumberFactory & TextFactory;\n"
        "spec ExtraFactory { static func extra(value: i32): i32; }\n"
        "spec NestedFactory: CombinedFactory & ExtraFactory;\n"
        "spec IntMapper(value: i32): i32;\n"
        "spec StringMapper(value: string): string;\n"
        "spec StringSupplier(): string;\n"
        "type First: NumberFactory, TextFactory, ExtraFactory {\n"
        "  static func inherited(value: i32): i32 { return value + 10; }\n"
        "  static func create(seed: i32): i32 { return seed + 1; }\n"
        "  static func select(value: i32): i32 { return value + 2; }\n"
        "  static func label(): string { return \"first\"; }\n"
        "  static func select(value: string): string { return value; }\n"
        "  static func duplicate(value: i32): i32 { return value + 3; }\n"
        "  static func extra(value: i32): i32 { return value + 4; }\n"
        "}\n"
        "type Second: NumberFactory, TextFactory, ExtraFactory {\n"
        "  static func inherited(value: i32): i32 { return value + 20; }\n"
        "  static func create(seed: i32): i32 { return seed + 5; }\n"
        "  static func select(value: i32): i32 { return value + 6; }\n"
        "  static func label(): string { return \"second\"; }\n"
        "  static func select(value: string): string { return value; }\n"
        "  static func duplicate(value: i32): i32 { return value + 7; }\n"
        "  static func extra(value: i32): i32 { return value + 8; }\n"
        "}\n"
        "func bindCreate<T: NestedFactory>(): IntMapper { return T.create; }\n"
        "func bindInherited<T: NestedFactory>(): IntMapper {\n"
        "  return T.inherited;\n"
        "}\n"
        "func bindNumber<T: NestedFactory>(): IntMapper { return T.select; }\n"
        "func bindText<T: NestedFactory>(): StringMapper { return T.select; }\n"
        "func bindLabel<T: NestedFactory>(): StringSupplier { return T.label; }\n"
        "func bindDuplicate<T: NestedFactory>(): IntMapper {\n"
        "  return T.duplicate;\n"
        "}\n"
        "func bindExtra<T: NestedFactory>(): IntMapper { return T.extra; }\n"
        "func run(): i32 {\n"
        "  let firstCreate = bindCreate<First>();\n"
        "  let secondCreate = bindCreate<Second>();\n"
        "  let inherited = bindInherited<First>();\n"
        "  let number = bindNumber<Second>();\n"
        "  let duplicate = bindDuplicate<First>();\n"
        "  let extra = bindExtra<Second>();\n"
        "  return firstCreate(1) + secondCreate(2) + inherited(3) +\n"
        "    number(4) + duplicate(5) + extra(6);\n"
        "}\n"
        "func runText(): string {\n"
        "  let text = bindText<First>();\n"
        "  let label = bindLabel<Second>();\n"
        "  return text(\"value\") + label();\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source,
        "intersection_constrained_static_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *required_calls[] = {
        "_witness->create(_arg0)",
        "_witness->inherited(",
        "_witness->select(_arg0)",
        "_witness->select__feng_overload_2(_arg0)",
        "_witness->duplicate(_arg0)",
        "_witness->extra(_arg0)",
        "return _witness->label()"
    };

    if (!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                               &analysis, &errors, &error_count)) {
        for (size_t index = 0U; index < error_count; ++index) {
            fprintf(stderr,
                    "MV07 semantic error %s: %s\n",
                    errors[index].code,
                    errors[index].message != NULL
                        ? errors[index].message
                        : "(unknown)");
        }
        ASSERT(false);
    }
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis,
                                   FENG_COMPILE_TARGET_LIB,
                                   NULL,
                                   &output,
                                   &codegen_error)) {
        fprintf(stderr,
                "MV07 codegen error: %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "(unknown)");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);
    ASSERT(count_substr(
               output.c_source,
               ".callable_value = {.static_value = &FengCallableSpecStaticValue__") ==
           8U);
    ASSERT(count_substr(output.c_source,
                        ".refcount = FENG_REFCOUNT_IMMORTAL") >= 8U);
    ASSERT(strstr(output.c_source,
                  "->callable_value.static_value") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_object_new(_callable_value_desc") == NULL);
    ASSERT(strstr(output.c_source, "switch (_T->kind)") == NULL);
    for (size_t index = 0U;
         index < sizeof(required_calls) / sizeof(required_calls[0]);
         ++index) {
        const char *call = strstr(output.c_source, required_calls[index]);
        const char *line_start;
        const char *line_end;

        if (call == NULL) {
            fprintf(stderr,
                    "MV07 missing generated call: %s\n",
                    required_calls[index]);
        }
        ASSERT(call != NULL);
        line_start = call;
        while (line_start > output.c_source && line_start[-1] != '\n') {
            --line_start;
        }
        line_end = strchr(call, '\n');
        if (line_end == NULL) {
            line_end = output.c_source + strlen(output.c_source);
        }
        ASSERT(!span_contains(line_start, line_end, "_subject"));
        ASSERT(!span_contains(line_start, line_end, "feng_object_new"));
    }
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A builtin-fit static requirement uses the same receiver-free witness for
 * direct constrained calls and MV04 method values. The thunk calls the
 * registered builtin-fit entry with its existing descriptor-first ABI. */
static void test_builtin_fit_static_spec_witness_codegen(void) {
    const char *source =
        "module feng.codegen.builtin_fit_static_spec_witness;\n"
        "spec Factory { static func create(seed: i32): i32; }\n"
        "spec Creator(seed: i32): i32;\n"
        "fit i32: Factory {\n"
        "    static func create(seed: i32): i32 { return seed + 1; }\n"
        "}\n"
        "func bind<T: Factory>(): Creator { return T.create; }\n"
        "func direct<T: Factory>(seed: i32): i32 { return T.create(seed); }\n"
        "func run(): i32 {\n"
        "    let creator = bind<i32>();\n"
        "    return creator(10) + direct<i32>(20);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "builtin_fit_static_spec_witness_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source, "FengFitBuiltin_") != NULL);
    ASSERT(strstr(output.c_source,
                  "&(const FengFunctionDescriptor){.name = \"create\"}, p0);") !=
           NULL);
    ASSERT(strstr(output.c_source, "__create(void *_subject") == NULL);
    ASSERT(strstr(output.c_source, "_witness->create(_arg0)") != NULL);
    ASSERT(strstr(output.c_source,
                  ".static_value = &FengCallableSpecStaticValue__") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_object_new(_callable_value_desc") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A constrained generic closed with an array type uses the existing array
 * descriptor and structured witness key for both direct static calls and
 * MV04 method values. The generated adapter remains receiver-free. */
static void test_array_fit_static_spec_witness_codegen(void) {
    const char *source =
        "module feng.codegen.array_fit_static_spec_witness;\n"
        "spec Factory { static func create(seed: i32): i32; }\n"
        "spec Creator(seed: i32): i32;\n"
        "fit i32[]: Factory {\n"
        "    static func create(seed: i32): i32 { return seed + 1; }\n"
        "}\n"
        "func bind<T: Factory>(): Creator { return T.create; }\n"
        "func direct<T: Factory>(seed: i32): i32 { return T.create(seed); }\n"
        "func run(): i32 {\n"
        "    let creator = bind<i32[]>();\n"
        "    return creator(10) + direct<i32[]>(20);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "array_fit_static_spec_witness_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source, "FengFitBuiltin_") != NULL);
    ASSERT(strstr(output.c_source, "__create(void *_subject") == NULL);
    ASSERT(strstr(output.c_source, "_witness->create(_arg0)") != NULL);
    ASSERT(strstr(output.c_source,
                  ".static_value = &FengCallableSpecStaticValue__") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_object_new(_callable_value_desc") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A concrete static method value is a receiver-free immortal singleton. Type,
 * fit, generic-owner and generic-method sources all call their already closed
 * thin wrapper directly; shared generic bodies consume the same descriptor
 * slot without allocating a runtime closure or performing a member lookup. */
static void test_concrete_static_method_value_codegen_uses_singletons(void) {
    const char *source =
        "module feng.codegen.concrete_static_method_value;\n"
        "spec IntMapper(value: int): int;\n"
        "spec Mapper<T>(value: T): T;\n"
        "type Math {\n"
        "  static func double(value: int): int { return value * 2; }\n"
        "}\n"
        "type ExtendedMath {}\n"
        "fit ExtendedMath {\n"
        "  static func triple(value: int): int { return value * 3; }\n"
        "}\n"
        "type GenericMath<T> {\n"
        "  static func echo(value: T): T { return value; }\n"
        "  static func identity<U>(value: U): U { return value; }\n"
        "}\n"
        "type GenericExtendedMath<T> {}\n"
        "fit GenericExtendedMath<T> {\n"
        "  static func echo(value: T): T { return value; }\n"
        "  static func identity<U>(value: U): U { return value; }\n"
        "}\n"
        "fit int {\n"
        "  static func increment(value: int): int { return value + 1; }\n"
        "}\n"
        "func makeType<T>(): Mapper<T> { return GenericMath<T>.echo; }\n"
        "func makeFit<T>(): Mapper<T> { return GenericExtendedMath<T>.echo; }\n"
        "func run(): int {\n"
        "  let owned: IntMapper = Math.double;\n"
        "  let fitted: IntMapper = ExtendedMath.triple;\n"
        "  let ownerGeneric: IntMapper = GenericMath<int>.echo;\n"
        "  let methodGeneric: IntMapper = GenericMath<string>.identity<int>;\n"
        "  let fitOwnerGeneric: IntMapper = GenericExtendedMath<int>.echo;\n"
        "  let fitMethodGeneric: IntMapper =\n"
        "    GenericExtendedMath<string>.identity<int>;\n"
        "  let builtinFit: IntMapper = int.increment;\n"
        "  let sharedType = makeType<int>();\n"
        "  let sharedFit = makeFit<int>();\n"
        "  return owned(1) + fitted(1) + ownerGeneric(1) +\n"
        "    methodGeneric(1) + fitOwnerGeneric(1) +\n"
        "    fitMethodGeneric(1) + builtinFit(1) +\n"
        "    sharedType(1) + sharedFit(1);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "concrete_static_method_value_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* Every distinct closed source/target pair owns one compile-time static
     * callable. Formation reads that pointer; neither direct nor shared
     * generic paths invoke the allocating bound-method machinery. */
    ASSERT(count_substr(
               output.c_source,
               ".callable_value = {.static_value = &FengCallableStaticValue__") ==
           9U);
    ASSERT(count_substr(output.c_source,
                        ".refcount = FENG_REFCOUNT_IMMORTAL") >= 9U);
    ASSERT(strstr(output.c_source,
                  "->callable_value.static_value") != NULL);
    ASSERT(strstr(output.c_source,
                  "feng_object_new(_callable_value_desc") == NULL);
    ASSERT(strstr(output.c_source, "FengSpecMethodValueBind__") == NULL);

    /* Adapters have no receiver and enter the selected closed type, user-fit
     * or builtin-fit wrapper. Generic method adapters also carry their fixed
     * function/type descriptors entirely in static storage. */
    ASSERT(strstr(output.c_source, "(void)_closure;") != NULL);
    ASSERT(strstr(output.c_source, "__static__double__from__") != NULL);
    ASSERT(strstr(output.c_source, "FengFitUser__") != NULL);
    ASSERT(strstr(output.c_source, "FengFitBuiltin__") != NULL);
    ASSERT(strstr(output.c_source,
                  ".reified_callable_deps_count = 1") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Closed and shared value-receiver method formation both allocate exactly one
 * callable closure and never route the receiver through a spec box. */
static void test_value_method_capture_codegen_has_direct_closure_lowering(void) {
    const char *source =
        "module feng.codegen.value_method_lowering;\n"
        "spec Reader<T>(): T;\n"
        "@value type Cell<T> {\n"
        "    var value: T;\n"
        "    func read(): T { return self.value; }\n"
        "}\n"
        "func bind<T>(value: Cell<T>): Reader<T> {\n"
        "    return value.read;\n"
        "}\n"
        "func use(): i64 {\n"
        "    let cell = Cell<i64> { value: 41 };\n"
        "    let direct: Reader<i64> = cell.read;\n"
        "    let shared = bind<i64>(cell);\n"
        "    return direct() + shared();\n"
        "}\n";
    FengProgram *program =
        parse_or_die(source, "value_method_direct_closure_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* The shared body reads one complete function descriptor and its embedded
     * callable-value metadata from one fixed slot. The retired prefix-cast
     * descriptor representation must not survive in generated C. */
    ASSERT(strstr(output.c_source, "FengMethodValueDescriptor") == NULL);
    ASSERT(count_substr(output.c_source,
                        "const FengFunctionDescriptor *_method_value_desc") ==
           1U);
    ASSERT(count_substr(output.c_source,
                        "const FengCallableValueDescriptor *_callable_value_desc") ==
           1U);
    ASSERT(count_substr(output.c_source,
                        "_desc->reified_callable_deps[") == 1U);
    ASSERT(count_substr(output.c_source,
                        "feng_object_new(_callable_value_desc") == 1U);
    ASSERT(strstr(output.c_source,
                  "_callable_value_desc3->aggregate_capture_desc") != NULL);

    /* The fixed-layout direct binder and the consumer-generated shared-body
     * adapter each contain exactly one closure allocation. At execution only
     * the selected formation path runs, so neither path performs a second
     * receiver box or closure allocation. */
    ASSERT(count_substr(output.c_source,
                        "feng_object_new(&FengCallableBind__") == 2U);

    /* Spec-box descriptors may be declared for object-spec coercion support,
     * but method-value formation must never allocate one. */
    for (const char *line = output.c_source;
         (line = strstr(line, "feng_object_new(&")) != NULL;
         ++line) {
        const char *line_end = strchr(line, '\n');

        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        ASSERT(!span_contains(line, line_end, "__spec_box"));
    }
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Generic shared bodies form top-level, reference-method and value-method
 * callable values from one typed function-descriptor representation. Static
 * function values allocate nothing, while each bound method path allocates
 * exactly its one language-level closure. */
static void test_generic_callable_value_reification_codegen(void) {
    const char *source =
        "module feng.codegen.callable_value_reification;\n"
        "spec Mapper<T>(value: T): T;\n"
        "spec Producer<T>(): T;\n"
        "func identity<T>(value: T): T { return value; }\n"
        "func makeTop<T>(): Mapper<T> { return identity<T>; }\n"
        "func makeTopCast<T>(): Mapper<T> {\n"
        "    return (Mapper<T>)identity<T>;\n"
        "}\n"
        "func direct<T>(value: T): T { return identity<T>(value); }\n"
        "type Reader<T> {\n"
        "    let value: T;\n"
        "    func Reader(value: T) { self.value = value; }\n"
        "    func read(): T { return self.value; }\n"
        "    func reader(): Producer<T> { return self.read; }\n"
        "    func mapper(): Mapper<T> { return identity<T>; }\n"
        "    func lambdaReader(): Producer<T> {\n"
        "        return () -> self.value;\n"
        "    }\n"
        "    func methodMapper<U>(): Mapper<U> { return identity<U>; }\n"
        "    func forward(reader: Reader<T>): Reader<T> { return reader; }\n"
        "}\n"
        "type Forwarder {\n"
        "    func forward<U>(reader: Reader<U>): Reader<U> { return reader; }\n"
        "    static func forwardStatic<U>(reader: Reader<U>): Reader<U> {\n"
        "        return reader;\n"
        "    }\n"
        "}\n"
        "@value type ValueReader<T> {\n"
        "    var value: T;\n"
        "    func read(): T { return self.value; }\n"
        "    func reader(): Producer<T> { return self.read; }\n"
        "    func current(): T { return self.read(); }\n"
        "}\n"
        "type StaticFactory<T> {\n"
        "    static func mapper(): Mapper<T> { return identity<T>; }\n"
        "}\n"
        "func bindReference<T>(reader: Reader<T>): Producer<T> {\n"
        "    return reader.read;\n"
        "}\n"
        "func bindValue<T>(reader: ValueReader<T>): Producer<T> {\n"
        "    return reader.read;\n"
        "}\n"
        "func makeLambda<T>(value: T): Producer<T> {\n"
        "    return () -> value;\n"
        "}\n"
        "func use(): int {\n"
        "    let top = makeTop<int>();\n"
        "    let topCast = makeTopCast<int>();\n"
        "    let reader = Reader<int>(3);\n"
        "    let reference = bindReference<int>(reader);\n"
        "    let ownerReference = reader.reader();\n"
        "    let ownerTop = reader.mapper();\n"
        "    let ownerLambda = reader.lambdaReader();\n"
        "    let methodTop = reader.methodMapper<int>();\n"
        "    let ownerForward = reader.forward(reader);\n"
        "    let methodForward = Forwarder().forward<int>(reader);\n"
        "    let staticForward = Forwarder.forwardStatic<int>(reader);\n"
        "    let value = ValueReader<int> { value: 4 };\n"
        "    let valueReference = bindValue<int>(value);\n"
        "    let valueOwnerReference = value.reader();\n"
        "    let valueCurrent = value.current();\n"
        "    let lambda = makeLambda<int>(5);\n"
        "    let staticTop = StaticFactory<int>.mapper();\n"
        "    return top(1) + topCast(2) + reference() + ownerReference() +\n"
        "           ownerTop(5) + ownerLambda() + methodTop(6) +\n"
        "           ownerForward.read() + methodForward.read() +\n"
        "           staticForward.read() +\n"
        "           valueReference() + valueOwnerReference() + valueCurrent +\n"
        "           lambda() +\n"
        "           staticTop(7) + direct<int>(8);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "generic_callable_value_reification_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *body_start;
    const char *body_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(strstr(output.c_source, "FengMethodValueDescriptor") == NULL);
    ASSERT(strstr(output.c_source, "FengCallableValueDescriptor") != NULL);
    ASSERT(strstr(output.c_source, "descriptor_factory") == NULL);
    ASSERT(strstr(output.c_source, "FENG_REFCOUNT_IMMORTAL") != NULL);

    /* A generic top-level function value is loaded from one fixed descriptor
     * slot and never allocates a closure. An explicit cast uses the identical
     * path and therefore adds no wrapper or forwarding layer. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__callable_value_reification__makeTop_G__from__void",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "callable_value.static_value") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 0U);
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__callable_value_reification__makeTopCast_G__from__void",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "callable_value.static_value") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 0U);

    /* A direct generic call keeps its prior shared-call path and never reads
     * callable-value formation metadata. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__callable_value_reification__direct_G__from__X",
        &body_start, &body_end));
    ASSERT(!span_contains(body_start, body_end, "->callable_value"));
    ASSERT(!span_contains(body_start, body_end, "static_value"));

    /* Reference receivers use one closure allocation and one retained pointer
     * assignment, with no aggregate copy or receiver box. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__callable_value_reification__bindReference_G__from__O_Feng__feng__codegen__callable_value_reification__Reader__G__T__CTX__T",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_assign(") == 1U);
    ASSERT(!span_contains(body_start, body_end, "feng_aggregate_assign("));

    /* Value receivers share the same slot and closure path, changing only the
     * required single descriptor-sized aggregate copy. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__callable_value_reification__bindValue_G__from__O_Feng__feng__codegen__callable_value_reification__ValueReader__G__T__CTX__T",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_aggregate_assign(") == 1U);
    ASSERT(span_contains(body_start, body_end,
                         "aggregate_capture_offset"));
    ASSERT(span_contains(body_start, body_end,
                         "aggregate_capture_desc"));

    /* A shared generic @value receiver is already an address. Nested self
     * calls must pass that address through directly instead of taking the
     * address of the `_self` pointer itself. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void FengGenericMethod__feng__codegen__callable_value_reification__ValueReader__i2__current",
        &body_start, &body_end));
    ASSERT(span_contains(
        body_start,
        body_end,
        "FengGenericMethod__feng__codegen__callable_value_reification__ValueReader__i0__read(_self, _td"));
    ASSERT(!span_contains(body_start, body_end, "__i0__read(&_self"));

    /* A generic static shared method also returns the same immortal top-level
     * callable value without allocating or dispatching on a source tag. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void FengGenericMethod__feng__codegen__callable_value_reification__StaticFactory__i0__mapper",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "callable_value.static_value") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 0U);

    /* Value-receiver formation must never allocate an object-spec box even
     * though reusable box descriptors can exist in the translation unit. */
    for (const char *line = output.c_source;
         (line = strstr(line, "feng_object_new(")) != NULL;
         ++line) {
        const char *line_end = strchr(line, '\n');

        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        ASSERT(!span_contains(line, line_end, "__spec_box"));
    }
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Unbound function, method and lambda casts share the typed callable-value
 * descriptor path. The same fixture also pins recursive target dependencies,
 * distinct target surfaces, and fixed-layout generic/fit method receivers. */
static void test_unbound_callable_explicit_cast_codegen(void) {
    const char *source =
        "module feng.codegen.unbound_callable_cast;\n"
        "spec Mapper<T>(value: T): T;\n"
        "spec MapperAlt<T>(value: T): T;\n"
        "spec Producer<T>(): T;\n"
        "func identity<T>(value: T): T { return value; }\n"
        "func topCast<T>(): Mapper<T> {\n"
        "    return (Mapper<T>)identity<T>;\n"
        "}\n"
        "@value type NestedBox<T> { let value: T; }\n"
        "type NestedOwner<T> {\n"
        "    let value: T;\n"
        "    func NestedOwner(value: T) { self.value = value; }\n"
        "}\n"
        "func nestedLeaf<T>(value: T): T {\n"
        "    let owner = NestedOwner<T>(value);\n"
        "    let box = NestedBox<T> { value: owner.value };\n"
        "    return box.value;\n"
        "}\n"
        "func nested<T>(value: T): T {\n"
        "    let mapper: Mapper<T> = nestedLeaf<T>;\n"
        "    return mapper(value);\n"
        "}\n"
        "func nestedMapper<T>(): Mapper<T> { return nested<T>; }\n"
        "@value type SurfacePair<T> {\n"
        "    let primary: Mapper<T>;\n"
        "    let alternate: MapperAlt<T>;\n"
        "}\n"
        "func surfaces<T>(): SurfacePair<T> {\n"
        "    let primary: Mapper<T> = nested<T>;\n"
        "    let alternate: MapperAlt<T> = nested<T>;\n"
        "    return SurfacePair<T> {\n"
        "        primary: primary, alternate: alternate\n"
        "    };\n"
        "}\n"
        "@value type Reader<T> {\n"
        "    let value: T;\n"
        "    func read(): T { return self.value; }\n"
        "    func castReader(): Producer<T> {\n"
        "        let test = (Producer<T>)self.read;\n"
        "        return test;\n"
        "    }\n"
        "}\n"
        "func lambdaCast<T>(value: T): Producer<T> {\n"
        "    return (Producer<T>)(() -> value);\n"
        "}\n"
        "type DirectOwner {\n"
        "    let marker: i64;\n"
        "    func DirectOwner(marker: i64) { self.marker = marker; }\n"
        "    func echo<U>(value: U): U { self.marker; return value; }\n"
        "    func mapper<U>(): Mapper<U> { return self.echo<U>; }\n"
        "}\n"
        "type FitOwner(i64, string);\n"
        "fit FitOwner {\n"
        "    func echo<U>(value: U): U { self.item1; return value; }\n"
        "}\n"
        "func fitMapper<U>(owner: FitOwner): Mapper<U> {\n"
        "    return owner.echo<U>;\n"
        "}\n"
        "func use(): i64 {\n"
        "    let top = topCast<i64>();\n"
        "    let reader = Reader<i64> { value: 2 };\n"
        "    let method = reader.castReader();\n"
        "    let lambda = lambdaCast<i64>(3);\n"
        "    let nestedValue = nestedMapper<i64>();\n"
        "    let pair = surfaces<i64>();\n"
        "    let direct = DirectOwner(4).mapper<i64>();\n"
        "    let fitted: FitOwner = (5, \"fit\");\n"
        "    let fittedMapper = fitMapper<i64>(fitted);\n"
        "    return top(1) + method() + lambda() + nestedValue(4) +\n"
        "           pair.primary(5) + pair.alternate(6) + direct(7) +\n"
        "           fittedMapper(8);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "unbound_callable_explicit_cast_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *body_start;
    const char *body_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(strstr(output.c_source, "FengMethodValueDescriptor") == NULL);
    ASSERT(strstr(output.c_source, "descriptor_factory") == NULL);

    /* An explicit top-level cast reads one closed descriptor slot and keeps
     * the immortal static callable path allocation-free. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__unbound_callable_cast__topCast_G__from__void",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "callable_value.static_value") == 1U);
    ASSERT(!span_contains(body_start, body_end, "feng_object_new("));

    /* The exact `(Producer<T>)self.read` form reads one method descriptor and
     * performs exactly one language-level bound-method closure allocation. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void FengGenericMethod__feng__codegen__unbound_callable_cast__Reader__i1__castReader",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 1U);

    /* The explicit lambda cast lowers directly to the target-shaped closure.
     * It retains the creating function descriptor for T and does not need a
     * second callable-dependency slot or adapter object. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__unbound_callable_cast__lambdaCast_G__from__X",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_reified_function_desc = _desc") == 1U);
    ASSERT(!span_contains(body_start, body_end,
                          "_desc->reified_callable_deps["));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 1U);

    /* One source entering two callable surfaces must retain two distinct
     * descriptor slots rather than being deduplicated by source symbol. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__unbound_callable_cast__surfaces_G__from__void",
        &body_start, &body_end));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[0]") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[1]") == 1U);

    /* A fixed-layout ordinary owner and a fixed-layout fit owner recover the
     * concrete receiver view in their generic shared bodies. */
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void FengGenericMethod__feng__codegen__unbound_callable_cast__DirectOwner__i1__echo",
        &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end,
                         "(((struct Feng__feng__codegen__unbound_callable_cast__DirectOwner *)_self))->marker"));
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void FengFitMethod__feng__codegen__unbound_callable_cast__FitOwner__fi0__echo",
        &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end,
                         "((*(struct Feng__feng__codegen__unbound_callable_cast__FitOwner *)_self)).item1"));
    ASSERT(!span_contains(body_start, body_end, "(_self).item1"));

    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

static const char *kGenericLambdaDynamicCaptureSrc =
    "module feng.codegen.generic_lambda_capture;\n"
    "spec Reader<R>(): R;\n"
    "@value\n"
    "type Pair<A, B> {\n"
    "    var first: A;\n"
    "    var second: B;\n"
    "    var label: string;\n"
    "}\n"
    "func captureDirect<T>(initial: T, replacement: T): T {\n"
    "    var captured = initial;\n"
    "    let read: Reader<T> = () -> captured;\n"
    "    captured = replacement;\n"
    "    return read();\n"
    "}\n"
    "type Owner<T> {\n"
    "    func capturePair<U>(initial: Pair<T, U>, replacement: Pair<T, U>): Pair<T, U> {\n"
    "        var captured = initial;\n"
    "        let read: Reader<Pair<T, U> > = () -> captured;\n"
    "        captured = replacement;\n"
    "        return read();\n"
    "    }\n"
    "}\n";

/* Generic lambdas preserve their creating callable's descriptor context and
 * store concrete-size captures in the capture cell's existing allocation. */
static void test_generic_lambda_dynamic_capture_codegen(void) {
    FengProgram *program = parse_or_die(kGenericLambdaDynamicCaptureSrc,
                                        "generic_lambda_capture.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic lambda dynamic capture): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengFunctionDescriptor *_reified_function_desc;") != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengTypeDescriptor *_reified_owner_desc;") != NULL);
    ASSERT(strstr(out.c_source,
                  "const FengGenericParamDescriptor *_reified_param0;") != NULL);
    ASSERT(strstr(out.c_source, "FengArray *_cap0;") != NULL);
    ASSERT(strstr(out.c_source, "feng_array_new_kinded(") != NULL);
    ASSERT(strstr(out.c_source, "feng_array_data(_lambda->_cap0)") != NULL);
    ASSERT(strstr(out.c_source, "switch (_T->kind)") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_aggregate_assign(feng_array_data(") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengCaptureCell__feng__codegen__generic_lambda_capture") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* A non-void expression lambda inherits its callable return target, allowing
 * target-only expressions such as named-tuple literals to use the same
 * expected-type lowering as an ordinary return statement. */
static void test_lambda_tuple_body_uses_callable_return_target_codegen(void) {
    static const char *kSource =
        "module feng.codegen.lambda_tuple_target;\n"
        "type Pair<T, U>(T, U);\n"
        "spec Reader<T>(): Pair<T, string>;\n"
        "func capture<T>(value: T): Reader<T> {\n"
        "    let captured = value;\n"
        "    return () -> (captured, \"targeted\");\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource,
                                        "lambda_tuple_target.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "FengLambda__feng__codegen__lambda_tuple_target") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_aggregate_assign") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* An owned fixed-layout aggregate passed directly to a shared generic
 * callable adopts its producer temporary into the current cleanup scope. */
static void test_generic_call_adopts_owned_aggregate_argument_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_owned_argument;\n"
        "@value type Payload {\n"
        "    let text: string;\n"
        "    func Payload(text: string) { self.text = text; }\n"
        "}\n"
        "func consume<T>(value: T): void {}\n"
        "func exercise(): void {\n"
        "    consume<Payload>(Payload(\"owned\"));\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource,
                                        "generic_owned_argument.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FengCleanupNode _cu__val") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng__feng__codegen__generic_owned_argument__consume_G__from__X") != NULL);
    ASSERT(strstr(out.c_source, "feng_aggregate_release(&_val") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

/* Concrete finalizers use the same callable capture preparation as methods
 * and constructors. Cover self, ordinary local, and nested-lambda capture in
 * the concrete body, plus the already-shared generic finalizer path. */
static void test_finalizer_lambda_capture_codegen(void) {
    static const char *kSource =
        "module feng.codegen.finalizer_capture;\n"
        "spec Reader(): int;\n"
        "spec ReaderFactory(): Reader;\n"
        "spec GenericReader<T>(): T;\n"
        "type Resource {\n"
        "    var value: int;\n"
        "    func ~Resource() {\n"
        "        let offset = 1;\n"
        "        let reader: Reader = () -> self.value + offset;\n"
        "        let factory: ReaderFactory = () {\n"
        "            let nested: Reader = () -> self.value + offset;\n"
        "            return nested;\n"
        "        };\n"
        "        let nested: Reader = factory();\n"
        "        reader();\n"
        "        nested();\n"
        "    }\n"
        "}\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "    func ~Box() {\n"
        "        let copy: T = self.value;\n"
        "        let fromSelf: GenericReader<T> = () -> self.value;\n"
        "        let fromLocal: GenericReader<T> = () -> copy;\n"
        "        fromSelf();\n"
        "        fromLocal();\n"
        "    }\n"
        "}\n"
        "func use_it() {\n"
        "    let resource = Resource { value: 40 };\n"
        "    let box = Box<int> { value: 41 };\n"
        "    resource.value;\n"
        "    box.value;\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "finalizer_capture.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;
    bool semantic_ok;

    semantic_ok = feng_semantic_analyze(programs,
                                        1U,
                                        FENG_COMPILE_TARGET_LIB,
                                        &analysis,
                                        &errors,
                                        &error_count);
    if (!semantic_ok) {
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr,
                    "semantic error (finalizer lambda capture): %s: %s\n",
                    errors[i].code != NULL ? errors[i].code : "(none)",
                    errors[i].message != NULL ? errors[i].message : "(unknown)");
        }
    }
    ASSERT(semantic_ok);
    ASSERT(error_count == 0U);
    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (finalizer lambda capture): %s\n",
                cgerr.message != NULL ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "FengCaptureCell__feng__codegen__finalizer_capture") != NULL);
    ASSERT(strstr(out.c_source,
                  "FengLambda__feng__codegen__finalizer_capture") != NULL);
    ASSERT(strstr(out.c_source, "__Resource__finalize(void *_self)") != NULL);
    ASSERT(strstr(out.c_source, "FengGenericMethod__feng__codegen__finalizer_capture") != NULL);
    ASSERT(strstr(out.c_source, "feng_cleanup_push(") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

static const char *kGenericChildSpecParentConstraintSrc =
    "module feng.codegen.gfparent;\n"
    "spec Parent<T> {\n"
    "    var name: string;\n"
    "    func payload(): string;\n"
    "}\n"
    "spec Child<T>: Parent<T> {}\n"
    "type User: Child<i32> {\n"
    "    var name: string;\n"
    "    func payload(): string { return self.name; }\n"
    "}\n"
    "func update<T: Parent<i32>>(value: T, next: string): T {\n"
    "    value.name = next;\n"
    "    let payload = value.payload();\n"
    "    return value;\n"
    "}\n"
    "func inferred(value: Child<i32>): Child<i32> {\n"
    "    return update(value, \"inferred\");\n"
    "}\n"
    "func explicit(value: Child<i32>): Child<i32> {\n"
    "    return update<Child<i32>>(value, \"explicit\");\n"
    "}\n";

static void test_generic_child_spec_parent_constraint_codegen(void) {
    FengProgram *program = parse_or_die(kGenericChildSpecParentConstraintSrc,
                                       "gfparent.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &out, &cgerr)) {
        fprintf(stderr,
                "codegen error (child spec generic parent constraint): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(false);
    }
    ASSERT(out.c_source != NULL);

    /* T stays Child<i32>; only its constraint dispatch surface is adapted to
     * Parent<i32>. Both inferred and explicit calls reuse one static generic
     * parameter descriptor containing the selected slot witness. */
    ASSERT(strstr(out.c_source,
                  ".descriptor = &FengSpecAgg__feng__codegen__gfparent__Child__G__i32") != NULL);
    ASSERT(count_substr(out.c_source,
                        ".witness = &FengSpecSlotWitness__") == 1U);
    ASSERT(count_substr(out.c_source,
                        "&_feng_closed_generic_param_desc_0") == 2U);
    ASSERT(strstr(out.c_source, "_value->witness->set_name") != NULL);
    ASSERT(strstr(out.c_source, "_value->witness->payload") != NULL);
    compile_generated_c_or_die(out.c_source);

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

static const char *kSpecValueFieldReceiverSrc =
    "module feng.codegen.sfvalue1;\n"
    "@value\n"
    "type Counter {\n"
    "    var value: int;\n"
    "    func add(amount: int) {\n"
    "        self.value += amount;\n"
    "    }\n"
    "}\n"
    "spec HasCounter {\n"
    "    var counter: Counter;\n"
    "}\n"
    "type Holder: HasCounter {\n"
    "    var counter: Counter;\n"
    "}\n"
    "func update(box: HasCounter) {\n"
    "    box.counter.add(1);\n"
    "    let copied = box.counter;\n"
    "    box.counter.add(copied.value);\n"
    "}\n";

/* A composite value field exposed by an object-form spec keeps one witness
 * slot, but that slot returns the implementing field address so method self
 * can mutate it in place and ordinary reads can still copy from it. */
static void test_spec_value_field_receiver_codegen(void) {
    FengProgram *program = parse_or_die(kSpecValueFieldReceiverSrc,
                                        "sfvalue1.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "void *(*borrow_counter)(void *_subject)") != NULL);
    ASSERT(strstr(out.c_source, "__borrow_counter(void *_subject)") != NULL);
    ASSERT(strstr(out.c_source, ".borrow_counter = &") != NULL);
    ASSERT(strstr(out.c_source, "witness->borrow_counter(") != NULL);
    ASSERT(strstr(out.c_source, "get_counter") == NULL);
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

static const char *kGenericExpressionJoinResultSrc =
    "module feng.codegen.generic_join;\n"
    "@value\n"
    "type Box<T> {\n"
    "    var value: T;\n"
    "    var label: string;\n"
    "}\n"
    "type Flow {\n"
    "    func pickIf<T>(flag: bool, left: Box<T>, right: Box<T>): Box<T> {\n"
    "        return if flag { left; } else { right; };\n"
    "    }\n"
    "    func pickMatch<T>(tag: i32, left: Box<T>, right: Box<T>): Box<T> {\n"
    "        return match tag { 0 { left; } else { right; } };\n"
    "    }\n"
    "    func pickTry<T>(left: Box<T>, right: Box<T>): Box<T> {\n"
    "        return try left catch ex: string { right; };\n"
    "    }\n"
    "}\n";

/* All expression joins preserve descriptor-sized address storage instead of
 * copying through the open generic placeholder aggregate. */
static void test_generic_expression_join_result_codegen(void) {
    FengProgram *program = parse_or_die(kGenericExpressionJoinResultSrc,
                                        "generic_join.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic expression join): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(count_substr(out.c_source,
                        "feng_aggregate_default_init(_ifv") >= 2U);
    ASSERT(count_substr(out.c_source,
                        "feng_aggregate_assign(_ifv") >= 4U);
    ASSERT(strstr(out.c_source,
                  "feng_aggregate_default_init(_tryv") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_aggregate_assign(_tryv") != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_aggregate_assign(&_ifv") == NULL);
    ASSERT(strstr(out.c_source,
                  "feng_aggregate_assign(&_tryv") == NULL);
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

static const char *kGenericValueConstructionReifiedStorageSrc =
    "module feng.codegen.generic_value_construction;\n"
    "spec Item {\n"
    "    func value(): int;\n"
    "}\n"
    "type ItemImpl: Item {\n"
    "    func value(): int { return 42; }\n"
    "}\n"
    "@value\n"
    "type Holder<T> {\n"
    "    let item: T;\n"
    "    func Holder(item: T) { self.item = item; }\n"
    "}\n"
    "type Factory<T> {\n"
    "    func make(item: T): Holder<T> {\n"
    "        return Holder<T>(item);\n"
    "    }\n"
    "}\n"
    "func use_factory(): int {\n"
    "    let item: Item = ItemImpl();\n"
    "    return Factory<Item>().make(item).item.value();\n"
    "}\n";

/* A shared generic body must not allocate Holder<T> with the erased C struct
 * size. Object-form Item is wider than the placeholder T slot, so the
 * constructor target must use the concrete aggregate descriptor's size. */
static void test_generic_value_construction_uses_reified_storage_codegen(void) {
    FengProgram *program = parse_or_die(
        kGenericValueConstructionReifiedStorageSrc,
        "generic_value_construction.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &out, &cgerr));
    ASSERT(out.c_source != NULL);
    const char *descriptor_decl = strstr(
        out.c_source, "const FengAggregateDescriptor *_rad");
    ASSERT(descriptor_decl != NULL);
    const char *descriptor_decl_end = strchr(descriptor_decl, '\n');
    ASSERT(descriptor_decl_end != NULL);
    const char *descriptor_read = strstr(
        descriptor_decl, "reified_agg_deps[0]");
    ASSERT(descriptor_read != NULL && descriptor_read < descriptor_decl_end);

    const char *descriptor_name_marker = strchr(descriptor_decl, '*');
    ASSERT(descriptor_name_marker != NULL);
    const char *descriptor_name_begin = descriptor_name_marker + 1U;
    const char *descriptor_name_end = strchr(descriptor_name_begin, ' ');
    ASSERT(descriptor_name_end != NULL);
    size_t descriptor_name_length =
        (size_t)(descriptor_name_end - descriptor_name_begin);
    ASSERT(descriptor_name_length > 0U && descriptor_name_length < 64U);
    char descriptor_size_pattern[80];
    int descriptor_size_pattern_length = snprintf(
        descriptor_size_pattern,
        sizeof descriptor_size_pattern,
        "%.*s->size",
        (int)descriptor_name_length,
        descriptor_name_begin);
    ASSERT(descriptor_size_pattern_length > 0 &&
           (size_t)descriptor_size_pattern_length <
               sizeof descriptor_size_pattern);

    const char *size_decl = strstr(
        descriptor_decl_end, "const size_t _rsize");
    ASSERT(size_decl != NULL);
    const char *size_decl_end = strchr(size_decl, '\n');
    ASSERT(size_decl_end != NULL);
    const char *size_read = strstr(size_decl, descriptor_size_pattern);
    ASSERT(size_read != NULL && size_read < size_decl_end);

    const char *size_name_begin = size_decl + strlen("const size_t ");
    const char *size_name_end = strchr(size_name_begin, ' ');
    ASSERT(size_name_end != NULL);
    size_t size_name_length = (size_t)(size_name_end - size_name_begin);
    ASSERT(size_name_length > 0U && size_name_length < 64U);
    char storage_size_pattern[80];
    int storage_size_pattern_length = snprintf(
        storage_size_pattern,
        sizeof storage_size_pattern,
        "[%.*s]",
        (int)size_name_length,
        size_name_begin);
    ASSERT(storage_size_pattern_length > 0 &&
           (size_t)storage_size_pattern_length < sizeof storage_size_pattern);
    char zero_fill_pattern[80];
    int zero_fill_pattern_length = snprintf(
        zero_fill_pattern,
        sizeof zero_fill_pattern,
        ", 0, %.*s)",
        (int)size_name_length,
        size_name_begin);
    ASSERT(zero_fill_pattern_length > 0 &&
           (size_t)zero_fill_pattern_length < sizeof zero_fill_pattern);

    const char *storage_decl = strstr(
        size_decl_end, "_Alignas(max_align_t) char _val");
    ASSERT(storage_decl != NULL);
    const char *storage_decl_end = strchr(storage_decl, '\n');
    ASSERT(storage_decl_end != NULL);
    const char *storage_size = strstr(storage_decl, storage_size_pattern);
    ASSERT(storage_size != NULL && storage_size < storage_decl_end);
    const char *zero_fill = strstr(storage_decl_end, zero_fill_pattern);
    ASSERT(zero_fill != NULL);
    ASSERT(strstr(out.c_source,
                  "Holder__G__T _val") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
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

/* Generic type and fit owners share one owner + method constraint scope.
 * Exercise instance/static witness dispatch, explicit and inferred method
 * arguments, nested owner arguments, and a supported instance method value;
 * the generated C must compile with the existing wrapper ABI. */
static void test_generic_owner_method_constraint_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_owner_method_constraint;\n"
        "type Box<T> {\n"
        "  let value: T;\n"
        "  func Box(value: T) { self.value = value; }\n"
        "}\n"
        "spec Surface<T> {\n"
        "  func read(): T;\n"
        "  static func echo(value: T): T;\n"
        "}\n"
        "spec IntMapper(value: int): int;\n"
        "type Value<T>: Surface<T> {\n"
        "  let value: T;\n"
        "  func Value(value: T) { self.value = value; }\n"
        "  func read(): T { return self.value; }\n"
        "  static func echo(value: T): T { return value; }\n"
        "}\n"
        "type FitValue<T> {\n"
        "  open let value: T;\n"
        "  func FitValue(value: T) { self.value = value; }\n"
        "}\n"
        "fit FitValue<T>: Surface<T> {\n"
        "  func read(): T { return self.value; }\n"
        "  static func echo(value: T): T { return value; }\n"
        "}\n"
        "type Host<T> {\n"
        "  func read<U: Surface<T>>(subject: U): T { return subject.read(); }\n"
        "  static func echo<U: Surface<T>>(value: T): T { return U.echo(value); }\n"
        "  func map<U: Surface<T>>(value: T): T { return U.echo(value); }\n"
        "  static func nested<U: Surface<Box<T>>>(value: Box<T>): Box<T> {\n"
        "    return U.echo(value);\n"
        "  }\n"
        "}\n"
        "type FitHost<T> {}\n"
        "fit FitHost<T> {\n"
        "  func read<U: Surface<T>>(subject: U): T { return subject.read(); }\n"
        "  static func echo<U: Surface<T>>(value: T): T { return U.echo(value); }\n"
        "  func map<U: Surface<T>>(value: T): T { return U.echo(value); }\n"
        "  static func nested<U: Surface<Box<T>>>(value: Box<T>): Box<T> {\n"
        "    return U.echo(value);\n"
        "  }\n"
        "}\n"
        "func use(): int {\n"
        "  let host = Host<int>();\n"
        "  let fitHost = FitHost<int>();\n"
        "  let direct = Value<int>(10);\n"
        "  let fitted = FitValue<int>(20);\n"
        "  let box = Box<int>(30);\n"
        "  let typeMapper: IntMapper = host.map<Value<int>>;\n"
        "  let fitMapper: IntMapper = fitHost.map<FitValue<int>>;\n"
        "  let nestedType = Host<int>.nested<Value<Box<int>>>(box);\n"
        "  let nestedFit = FitHost<int>.nested<FitValue<Box<int>>>(box);\n"
        "  return host.read<Value<int>>(direct) + host.read(fitted) +\n"
        "         Host<int>.echo<Value<int>>(1) +\n"
        "         fitHost.read<FitValue<int>>(fitted) + fitHost.read(direct) +\n"
        "         FitHost<int>.echo<FitValue<int>>(2) +\n"
        "         nestedType.value + nestedFit.value +\n"
        "         typeMapper(3) + fitMapper(4);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_owner_method_constraint_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "FengGenericMethod__feng__codegen__generic_owner_method_constraint__Host") != NULL);
    ASSERT(strstr(output.c_source,
                  "FengFitMethod__feng__codegen__generic_owner_method_constraint__FitHost") != NULL);
    ASSERT(strstr(output.c_source,
                  "FengSpecWitness__feng__codegen__generic_owner_method_constraint__Surface") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

/* Direct object-literal shorthand must consume the constructor selected by
 * semantic analysis for both reference and value types. */
static void test_object_literal_shorthand_invokes_selected_constructor(void) {
    static const char *kSource =
        "module feng.codegen.g05ctor;\n"
        "type RefProbe {\n"
        "    var value: int;\n"
        "    func RefProbe() { self.value = 7; }\n"
        "}\n"
        "@value\n"
        "type ValueProbe {\n"
        "    var value: int;\n"
        "    func ValueProbe() { self.value = 9; }\n"
        "}\n"
        "func makeRefShorthand(): RefProbe { return RefProbe {}; }\n"
        "func makeValueShorthand(): ValueProbe { return ValueProbe {}; }\n";
    FengProgram *program = parse_or_die(kSource, "g05_object_literal_ctor.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *body_start;
    const char *body_end;

    {
        bool semantic_ok = feng_semantic_analyze(
            programs, 1U, FENG_COMPILE_TARGET_LIB,
            &analysis, &errors, &error_count);

        if (!semantic_ok) {
            for (size_t index = 0U; index < error_count; ++index) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[index].path,
                        errors[index].token.line,
                        errors[index].token.column,
                        errors[index].message);
            }
        }
        ASSERT(semantic_ok);
    }
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(find_generated_function_body(
        output.c_source,
        "feng__feng__codegen__g05ctor__makeRefShorthand",
        &body_start,
        &body_end));
    ASSERT(count_substr_in_span(
        body_start, body_end, "__ctor__RefProbe__from__void(") == 1U);
    ASSERT(find_generated_function_body(
        output.c_source,
        "feng__feng__codegen__g05ctor__makeValueShorthand",
        &body_start,
        &body_end));
    ASSERT(count_substr_in_span(
        body_start, body_end, "__ctor__ValueProbe__from__void(") == 1U);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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
        "    }\n"
        "    try noop() catch {}\n"
        "    try fail() catch ex: string {\n"
        "        noop();\n"
        "    }\n"
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

/* A generic call result inside a try protected range uses descriptor-sized
 * C storage. The range must form its own lexical block so the landing-pad
 * preservation jump never enters the scope of that variably sized storage. */
static void test_generic_try_body_reified_storage_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generictry;\n"
        "@value\n"
        "type Box<T> {\n"
        "    let value: T;\n"
        "}\n"
        "func pass<T>(value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func recover<T>(value: T): T {\n"
        "    return try pass<T>(value) catch ex: string { value; };\n"
        "}\n"
        "func chooseIf<T>(flag: bool, left: T, right: T): T {\n"
        "    return if flag { pass<T>(left); } else { right; };\n"
        "}\n"
        "func chooseMatch<T>(tag: i32, left: T, right: T): T {\n"
        "    return match tag { 0 { pass<T>(left); } else { right; } };\n"
        "}\n"
        "func passBox<T>(value: Box<T>): Box<T> {\n"
        "    return value;\n"
        "}\n"
        "func recoverBox<T>(value: Box<T>): Box<T> {\n"
        "    return try passBox<T>(value) catch ex: string { value; };\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "generic_try.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source,
                  "feng_try_frame_push(&_try_marker") != NULL);
    ASSERT(strstr(out.c_source,
                  ";\n    {\n    _try_begin_") != NULL);
    ASSERT(strstr(out.c_source,
                  "_Alignas(max_align_t) char _gr") != NULL);
    ASSERT(count_substr(out.c_source,
                        "_Alignas(max_align_t) char _ifv") >= 2U);
    ASSERT(strstr(out.c_source,
                  "_Alignas(max_align_t) char _tryv") != NULL);
    ASSERT(strstr(out.c_source,
                  "_try_end_") != NULL);
    ASSERT(strstr(out.c_source,
                  ": ;\n    }\n    feng_frame_pop();") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Generic loops keep goto-based continue labels outside descriptor-sized VLA
 * scopes. A T[] traversal also derives a borrowed element address from the
 * cached descriptor size instead of interpreting arbitrary element bytes as
 * a pointer. */
static void test_generic_loop_reified_storage_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_loop_continue;\n"
        "type Step<T>(bool, T);\n"
        "@value\n"
        "type Cursor<T> {\n"
        "    var index: i64;\n"
        "    let value: T;\n"
        "    @iterator\n"
        "    func next(): Step<T> {\n"
        "        if self.index > 0 { return (false, self.value); }\n"
        "        self.index += 1;\n"
        "        return (true, self.value);\n"
        "    }\n"
        "}\n"
        "type Sequence<T> {\n"
        "    let value: T;\n"
        "    @iterable\n"
        "    func iter(): Cursor<T> {\n"
        "        return Cursor<T> { index: 0, value: self.value };\n"
        "    }\n"
        "}\n"
        "func choose<T>(source: Sequence<T>, values: T[], missing: T): T {\n"
        "    var result = missing;\n"
        "    var seen: i64 = 0;\n"
        "    for let value in source {\n"
        "        seen += 1;\n"
        "        if seen == 1 { continue; }\n"
        "        result = value;\n"
        "    }\n"
        "    for let value in values {\n"
        "        if seen == 1 { continue; }\n"
        "        result = value;\n"
        "    }\n"
        "    for var index = 0; index < 2; index += 1 {\n"
        "        if index == 0 { continue; }\n"
        "        result = missing;\n"
        "    }\n"
        "    return result;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "tests/generic_loop_continue.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};

    {
        bool semantic_ok = feng_semantic_analyze(programs,
                                                 1U,
                                                 FENG_COMPILE_TARGET_LIB,
                                                 &analysis,
                                                 &errors,
                                                 &error_count);
        if (!semantic_ok) {
            for (size_t index = 0U; index < error_count; ++index) {
                fprintf(stderr, "%s:%u:%u: semantic error: %s\n",
                        errors[index].path,
                        errors[index].token.line,
                        errors[index].token.column,
                        errors[index].message);
            }
        }
        ASSERT(semantic_ok);
    }
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "goto _cont_") != NULL);
    ASSERT(strstr(out.c_source,
                  "        }\n        _cont_") != NULL);
    ASSERT(strstr(out.c_source,
                  "const size_t _felem_size_") != NULL);
    ASSERT(strstr(out.c_source,
                  "(void *)((char *)feng_array_data(") != NULL);
    ASSERT(strstr(out.c_source,
                  "* _felem_size_") != NULL);
    ASSERT(strstr(out.c_source,
                  "_Alignas(max_align_t) char _cursor") != NULL);
    ASSERT(strstr(out.c_source,
                  "_Alignas(max_align_t) char _ir") != NULL);
    ASSERT(strstr(out.c_source,
                  "->reified_field_offsets[0]") != NULL);
    ASSERT(strstr(out.c_source,
                  "->reified_field_offsets[1]") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A generic iterator result may have a fixed C representation while its
 * managed-slot cleanup still depends on the closed aggregate descriptor.
 * Keep storage direct and obtain cleanup authority from the ordinary
 * reified aggregate dependency slot, never from an open static descriptor. */
static void test_generic_iterator_fixed_storage_reified_cleanup_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_iterator_fixed_cleanup;\n"
        "spec Action<T>(value: T): void;\n"
        "type Result<T>(bool, Action<T>);\n"
        "@value\n"
        "type Cursor<T> {\n"
        "    var index: i64;\n"
        "    let callback: Action<T>;\n"
        "    @iterator\n"
        "    func next(): Result<T> {\n"
        "        if self.index > 0 { return (false, self.callback); }\n"
        "        self.index += 1;\n"
        "        return (true, self.callback);\n"
        "    }\n"
        "}\n"
        "type Sequence<T> {\n"
        "    let callback: Action<T>;\n"
        "    @iterable\n"
        "    func iter(): Cursor<T> {\n"
        "        return Cursor<T> { index: 0, callback: self.callback };\n"
        "    }\n"
        "}\n"
        "func invokeAll<T>(source: Sequence<T>, value: T): void {\n"
        "    for let callback in source { callback(value); }\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_iterator_fixed_cleanup.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    const char *body_start;
    const char *body_end;
    const char *result_decl;
    const char *result_cleanup;
    const char *result_release;
    const char *line_end;

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(find_generated_function_body(
        out.c_source,
        "static void feng__feng__codegen__generic_iterator_fixed_cleanup__invokeAll_G__from__",
        &body_start,
        &body_end));

    result_decl = strstr(
        body_start,
        "struct Feng__feng__codegen__generic_iterator_fixed_cleanup__Result__G__T__CTX__T _ir");
    ASSERT(result_decl != NULL && result_decl < body_end);
    ASSERT(count_substr_in_span(body_start,
                                body_end,
                                "_Alignas(max_align_t) char _ir") == 0U);
    ASSERT(count_substr_in_span(body_start,
                                body_end,
                                "->reified_field_offsets[") == 0U);
    ASSERT(span_contains(result_decl, body_end, ".item1"));
    ASSERT(span_contains(result_decl, body_end, ".item2"));

    result_cleanup = strstr(result_decl, "feng_cleanup_push_aggregate(");
    ASSERT(result_cleanup != NULL && result_cleanup < body_end);
    line_end = strchr(result_cleanup, '\n');
    ASSERT(line_end != NULL && line_end < body_end);
    ASSERT(span_contains(result_cleanup,
                         line_end,
                         "_desc->reified_agg_deps["));

    result_release = strstr(result_cleanup, "feng_aggregate_release(&_ir");
    ASSERT(result_release != NULL && result_release < body_end);
    line_end = strchr(result_release, '\n');
    ASSERT(line_end != NULL && line_end < body_end);
    ASSERT(span_contains(result_release,
                         line_end,
                         "_desc->reified_agg_deps["));
    ASSERT(!span_contains(
        body_start,
        body_end,
        "Result__G__T__CTX__T__aggregate_desc"));
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A multi-parameter generic callable shared body keeps source parameter order
 * independent from generic descriptor order. Every open value uses the
 * existing generic-value ABI, the descriptor-sized result uses its one
 * reified aggregate slot, and direct values never acquire a spec box. */
static void test_multi_parameter_generic_callable_abi_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_multi_callable_abi;\n"
        "@value\n"
        "type Wide<T> {\n"
        "    let value: T;\n"
        "    let marker: string;\n"
        "    func Wide(value: T, marker: string) {\n"
        "        self.value = value;\n"
        "        self.marker = marker;\n"
        "    }\n"
        "}\n"
        "@value\n"
        "type Result<T, U, V> {\n"
        "    let direct: T;\n"
        "    let managed: U;\n"
        "    let wide: V;\n"
        "    let repeated: U;\n"
        "    func Result(direct: T, managed: U, wide: V, repeated: U) {\n"
        "        self.direct = direct;\n"
        "        self.managed = managed;\n"
        "        self.wide = wide;\n"
        "        self.repeated = repeated;\n"
        "    }\n"
        "}\n"
        "spec Multi<T, U, V>(managed: U, direct: T, wide: V, repeated: U): Result<T, U, V>;\n"
        "func top<T, U, V>(managed: U, direct: T, wide: V, repeated: U): Result<T, U, V> {\n"
        "    return Result<T, U, V>(direct, managed, wide, repeated);\n"
        "}\n"
        "func apply<T, U, V>(callable: Multi<T, U, V>, managed: U, direct: T, wide: V, repeated: U): Result<T, U, V> {\n"
        "    return callable(managed, direct, wide, repeated);\n"
        "}\n"
        "func use(): i64 {\n"
        "    let callable: Multi<i64, string, Wide<string>> = top<i64, string, Wide<string>>;\n"
        "    let result = apply<i64, string, Wide<string>>(\n"
        "        callable, \"first\", 41, Wide<string>(\"wide\", \"marker\"), \"last\"\n"
        "    );\n"
        "    return result.direct;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_multi_parameter_callable_abi.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    const char *body_start;
    const char *body_end;
    const char *first_u;
    const char *direct_t;
    const char *wide_v;
    const char *repeated_u;

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &out,
                                     &cgerr));
    ASSERT(out.c_source != NULL);
    ASSERT(find_generated_function_body(
        out.c_source,
        "void feng__feng__codegen__generic_multi_callable_abi__apply_G__from__",
        &body_start,
        &body_end));

    /* The shared signature receives each generic domain once and preserves
     * the declared managed/direct/wide/repeated argument order. */
    ASSERT(strstr(out.c_source,
                  "const FengFunctionDescriptor *_desc, "
                  "const FengGenericParamDescriptor *_T, "
                  "const FengGenericParamDescriptor *_U, "
                  "const FengGenericParamDescriptor *_V") != NULL);
    ASSERT(strstr(out.c_source,
                  "const void *_p_managed, const void *_p_direct, "
                  "const void *_p_wide, const void *_p_repeated, "
                  "void *_out") != NULL);

    first_u = strstr(body_start, "= _U;");
    direct_t = first_u != NULL ? strstr(first_u + 1, "= _T;") : NULL;
    wide_v = direct_t != NULL ? strstr(direct_t + 1, "= _V;") : NULL;
    repeated_u = wide_v != NULL ? strstr(wide_v + 1, "= _U;") : NULL;
    ASSERT(first_u != NULL && first_u < body_end);
    ASSERT(direct_t != NULL && direct_t < body_end);
    ASSERT(wide_v != NULL && wide_v < body_end);
    ASSERT(repeated_u != NULL && repeated_u < body_end);
    ASSERT(first_u < direct_t && direct_t < wide_v && wide_v < repeated_u);

    /* The one required callable dispatch receives four address-form generic
     * values and writes directly into descriptor-sized result storage. */
    ASSERT(count_substr_in_span(body_start, body_end, ")->invoke(") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_Alignas(max_align_t) char _call_arg") == 4U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_Alignas(max_align_t) char _call_result") ==
           1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_agg_deps[") == 1U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_type_deps[") == 0U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "_desc->reified_callable_deps[") == 0U);
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_object_new(") == 0U);
    ASSERT(strstr(out.c_source, "feng_scalar_box_new_") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
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

/* Variadic constructor calls use the same normalized array ABI as functions
 * and methods. Cover reference/value/generic construction, fixed-prefix and
 * variadic-only forms, prepacked forwarding, and a field initializer. */
static void test_variadic_constructor_codegen(void) {
    static const char *kSource =
        "module feng.codegen.variadic_constructor;\n"
        "type RefValues {\n"
        "    let values: int[];\n"
        "    func RefValues(first: int, rest: int...) {\n"
        "        self.values = rest;\n"
        "    }\n"
        "}\n"
        "@value\n"
        "type ValueValues {\n"
        "    let values: int[];\n"
        "    func ValueValues(first: int, rest: int...) {\n"
        "        self.values = rest;\n"
        "    }\n"
        "}\n"
        "type OnlyValues {\n"
        "    let values: int[];\n"
        "    func OnlyValues(values: int...) {\n"
        "        self.values = values;\n"
        "    }\n"
        "}\n"
        "type GenericValues<T> {\n"
        "    let values: T[];\n"
        "    func GenericValues(first: T, rest: T...) {\n"
        "        self.values = rest;\n"
        "    }\n"
        "}\n"
        "type Holder {\n"
        "    let values = ValueValues(1, 2, 3);\n"
        "}\n"
        "func run(existing: int[]): int {\n"
        "    let empty = RefValues(1);\n"
        "    let multiple = RefValues(1, 2, 3);\n"
        "    let forwarded = RefValues(1, ...existing);\n"
        "    let value = ValueValues(1, 2, 3);\n"
        "    let onlyEmpty = OnlyValues();\n"
        "    let onlyMultiple = OnlyValues(1, 2, 3);\n"
        "    let genericEmpty = GenericValues<int>(1);\n"
        "    let genericForwarded = GenericValues<int>(1, ...existing);\n"
        "    let holder = Holder();\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program =
        parse_or_die(kSource, "tests/variadic_constructor.ff");
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
        fprintf(stderr, "codegen error (variadic constructor): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "__ctor__RefValues") != NULL);
    ASSERT(strstr(out.c_source, "__ctor__ValueValues") != NULL);
    ASSERT(strstr(out.c_source, "__ctor__OnlyValues") != NULL);
    ASSERT(strstr(out.c_source, "GenericValues") != NULL);
    ASSERT(strstr(out.c_source, "(size_t)0") != NULL);
    ASSERT(strstr(out.c_source, "(size_t)2") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Generic instance methods normalize variadic arrays in both generic domains:
 * the owner type parameter T and the method type parameter U. */
static void test_generic_variadic_instance_method_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_variadic_method;\n"
        "type Relay<T> {\n"
        "    func ownerValues(values: T...): T[] {\n"
        "        return values;\n"
        "    }\n"
        "    func methodValues<U>(values: U...): U[] {\n"
        "        return values;\n"
        "    }\n"
        "    func ownerPair(first: T, second: T): T[] {\n"
        "        return self.ownerValues(first, second);\n"
        "    }\n"
        "    func methodPair<U>(first: U, second: U): U[] {\n"
        "        return self.methodValues<U>(first, second);\n"
        "    }\n"
        "}\n"
        "func run(existing: int[]): int {\n"
        "    let relay = Relay<int>();\n"
        "    let ownerEmpty = relay.ownerValues();\n"
        "    let ownerPacked = relay.ownerValues(1, 2, 3);\n"
        "    let ownerForwarded = relay.ownerValues(...existing);\n"
        "    let methodPacked = relay.methodValues<string>(\"a\", \"b\");\n"
        "    let strings: string[] = [\"c\", \"d\"];\n"
        "    let methodForwarded = relay.methodValues<string>(...strings);\n"
        "    let ownerPair = relay.ownerPair(4, 5);\n"
        "    let methodPair = relay.methodPair<string>(\"e\", \"f\");\n"
        "    return 0;\n"
        "}\n";
    FengProgram *program =
        parse_or_die(kSource, "tests/generic_variadic_method.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    bool analyzed = feng_semantic_analyze(programs,
                                          1U,
                                          FENG_COMPILE_TARGET_LIB,
                                          &analysis,
                                          &errors,
                                          &error_count);
    if (!analyzed) {
        for (size_t index = 0U; index < error_count; ++index) {
            fprintf(stderr,
                    "%s:%u:%u: semantic error: %s\n",
                    errors[index].path,
                    errors[index].token.line,
                    errors[index].token.column,
                    errors[index].message);
        }
    }
    ASSERT(analyzed);
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic variadic method): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "ownerValues") != NULL);
    ASSERT(strstr(out.c_source, "methodValues") != NULL);
    ASSERT(strstr(out.c_source, "feng_array_new") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Explicitly forwarded variadic arrays are passed through without emitting a
 * second feng_array_new call in free, method, static-method, or generic paths. */
static void test_prepacked_variadic_forwarding_codegen(void) {
    static const char *kSource =
        "module feng.codegen.prepacked_variadic;\n"
        "func sink(values: int...): int { return 1; }\n"
        "func forward(values: int...): int { return sink(...values); }\n"
        "func genericSink<T>(values: T...): int { return 2; }\n"
        "func genericForward<T>(values: T...): int {\n"
        "    return genericSink<T>(...values);\n"
        "}\n"
        "type Relay {\n"
        "    func sink(values: int...): int { return 3; }\n"
        "    func forward(values: int...): int { return self.sink(...values); }\n"
        "    static func sinkStatic(values: int...): int { return 4; }\n"
        "    static func forwardStatic(values: int...): int {\n"
        "        return Relay.sinkStatic(...values);\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "tests/prepacked_variadic.ff");
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
        fprintf(stderr, "codegen error (prepacked variadic): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "feng_array_new(") == NULL);
    ASSERT(strstr(out.c_source, "feng_array_new_kinded(") == NULL);
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

/* A source that reaches an object-form spec leaf through a union path must be
 * converted to the fat spec representation before the union payload is
 * initialized. Cover both direct and nested union paths. */
static void test_union_object_spec_leaf_coercion_codegen(void) {
    static const char *kSource =
        "module feng.codegen.unionleafspec;\n"
        "spec Named { func name(): string; }\n"
        "type User: Named {\n"
        "    let value: string;\n"
        "    func name(): string { return self.value; }\n"
        "}\n"
        "type Empty {}\n"
        "spec MaybeNamed: Empty | Named;\n"
        "spec NestedMaybeNamed: MaybeNamed | bool;\n"
        "func make(value: string): MaybeNamed {\n"
        "    return User { value: value };\n"
        "}\n"
        "func make_nested(value: string): NestedMaybeNamed {\n"
        "    return User { value: value };\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "tests/union_object_spec_leaf_coercion_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);

    cg_ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                      NULL, &out, &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (union object-spec leaf coercion): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(
        out.c_source,
        ".payload.m1 = ((struct FengSpecValue__feng__codegen__unionleafspec__Named){ .subject =") != NULL);
    ASSERT(strstr(
        out.c_source,
        ".witness = &FengWitness__feng__codegen__unionleafspec__User__as__feng__codegen__unionleafspec__Named") != NULL);
    ASSERT(strstr(
        out.c_source,
        ".payload.m0 = ((struct FengSpecValue__feng__codegen__unionleafspec__MaybeNamed)") != NULL);
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

/* A generic union substitutes T with i32 during semantic analysis.  The
 * adapted literal type stored on the AST must remain valid after the resolver
 * context is released and codegen begins. */
static void test_generic_union_literal_adaptation_type_lifetime(void) {
    static const char *kSource =
        "module feng.codegen.uniongenericliteral;\n"
        "spec Result<T>: string | T;\n"
        "func value(flag: bool): Result<i32> {\n"
        "    return if flag { 42 } else { throw \"error\"; };\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "tests/generic_union_literal_adaptation_type_lifetime.ff");
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
        fprintf(stderr, "codegen error (generic union literal adaptation): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "(int32_t)INT32_C(42)") != NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* A union whose payload contains a descriptor-sized generic value must use
 * the closed union/member descriptors. Aggregate array stores must also
 * preserve an address-form parameter instead of taking its address twice. */
static void test_generic_union_reified_storage_and_array_source_codegen(void) {
    static const char *kSource =
        "module feng.codegen.unionreified;\n"
        "@value\n"
        "type Box<T> {\n"
        "    let value: T;\n"
        "    let label: string;\n"
        "}\n"
        "spec Maybe<T>: string | Box<T>;\n"
        "func exercise<T>(first: Box<T>, second: Box<T>): Maybe<T> {\n"
        "    let values = [first, second];\n"
        "    var result: Maybe<T> = first;\n"
        "    result = second;\n"
        "    return result;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "tests/generic_union_reified_storage_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic reified union): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "_Alignas(max_align_t) char _union") != NULL);
    ASSERT(strstr(out.c_source,
                  ".nested = ((const FengAggregateDescriptor *)_desc->reified_agg_deps[") != NULL);
    ASSERT(strstr(out.c_source,
                  "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, _desc->reified_agg_deps[") != NULL);
    ASSERT(strstr(out.c_source, "&_p_first") == NULL);
    ASSERT(strstr(out.c_source, "&_p_second") == NULL);
    compile_generated_c_or_die(out.c_source);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&cgerr);
    feng_semantic_analysis_free(analysis);
    free(errors);
    feng_program_free(program);
}

/* Union-form generic constraints validate membership at compile time and do
 * not add a witness to any concrete generic parameter descriptor. */
static void test_generic_union_constraint_omits_runtime_witness_codegen(void) {
    static const char *kSource =
        "module feng.codegen.unionconstraint;\n"
        "type Reference { let label: string; let code: i64; }\n"
        "@value\n"
        "type Box<T> { let value: T; let label: string; let code: i64; }\n"
        "spec Choice<T>: Reference | Box<T> | i64;\n"
        "func accept<T, U: Choice<T>>(value: U): U { return value; }\n"
        "func exercise(): i64 {\n"
        "    let reference = accept<string, Reference>(\n"
        "        Reference { label: \"reference\", code: 1 });\n"
        "    let aggregate = accept<string, Box<string>>(\n"
        "        Box<string> { value: \"aggregate\", label: \"box\", code: 2 });\n"
        "    let scalar = accept<string, i64>(3);\n"
        "    return reference.code + aggregate.code + scalar;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "tests/generic_union_constraint_no_witness_codegen.ff");
    const FengProgram *programs[1] = {program};
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalysis *analysis = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    cg_ok = feng_codegen_emit_program(analysis,
                                      FENG_COMPILE_TARGET_LIB,
                                      NULL,
                                      &out,
                                      &cgerr);
    if (!cg_ok) {
        fprintf(stderr, "codegen error (generic union constraint): %s\n",
                cgerr.message ? cgerr.message : "(unknown)");
        ASSERT(cg_ok);
    }
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "FENG_VALUE_TRIVIAL") != NULL);
    ASSERT(strstr(out.c_source, "FENG_VALUE_MANAGED_POINTER") != NULL);
    ASSERT(strstr(out.c_source,
                  "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS") != NULL);
    ASSERT(count_substr(out.c_source, ".witness = NULL") >= 3U);
    ASSERT(strstr(out.c_source, ".witness = &") == NULL);
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

/* Generated code constructs one explicit source and emits ordinary wrappers. */
static void test_member_mix_fields_and_mixable_wrappers_codegen(void) {
    static const char *kSource =
        "module feng.codegen.mixin;\n"
        "open spec Widget { func draw(area: int): int; func log(args: string...): void; }\n"
        "open type View: Widget {\n"
        "    open let x: int = 1;\n"
        "    open var y: int = 2;\n"
        "    @mixable open static func draw(target: Widget, area: int): int { return area + 1; }\n"
        "    @mixable open static func log(target: Widget, args: string...): void {}\n"
        "}\n"
        "type Zero: Widget {\n"
        "    ...: View;\n"
        "    func Zero() { self.x = 0; }\n"
        "}\n"
        "type Copy: Widget { ...: View = View(); }\n"
        "type Leaf: Widget { ...: Copy; }\n"
        "func exercise(): int {\n"
        "    let zero = Zero();\n"
        "    let copy = Copy();\n"
        "    let leaf = Leaf();\n"
        "    leaf.log(\"a\", \"b\");\n"
        "    return zero.x + copy.x + leaf.draw(1);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "mixin_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    /* Only the explicit `... = View()` directive materializes a source
     * cleanup slot, even though two fields are copied from that one value. */
    ASSERT(count_substr(output.c_source,
                        "FengCleanupNode _cu__mixin_source") == 1U);
    ASSERT(strstr(output.c_source,
                  "Copy__static__draw__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "Leaf__static__draw__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "Leaf__draw__from__i64") != NULL);
    ASSERT(strstr(output.c_source,
                  "Leaf__log__from__VA_s") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Seal mixable methods lower through ordinary wrappers, while seal mixable
 * fields lower through the existing struct-field path. Both access checks
 * remain compile-time-only and add no runtime branch or alternate ABI. */
static void test_mixable_seal_wrappers_use_static_codegen_path(void) {
    static const char *kSource =
        "module feng.codegen.mixable_seal;\n"
        "spec Widget {}\n"
        "type View: Widget {\n"
        "    @mixable seal var state: int = 7;\n"
        "    @mixable seal var text: string = \"state\";\n"
        "    seal var hiddenState: int = 9;\n"
        "    @mixable seal static func draw(target: Widget, value: int): int { return value + 1; }\n"
        "    @mixable seal static func log(target: Widget, values: int...): void {}\n"
        "}\n"
        "type Button: Widget {\n"
        "    ...: View = View();\n"
        "    open func run(value: int): int { self.log(1, 2); return self.draw(value); }\n"
        "    open static func source(value: int): int { return View.draw(Button(), value); }\n"
        "    open func fields(source: View): int { source.state = 9; return self.state + source.state; }\n"
        "    open static func sourceState(source: View): int { source.state = 11; return source.state; }\n"
        "    open func sourceText(source: View): string { return source.text; }\n"
        "}\n"
        "type FitView: Widget {}\n"
        "open fit FitView {\n"
        "    @mixable seal static func paint(target: Widget, value: int): int { return value + 2; }\n"
        "}\n"
        "type FitButton: Widget {\n"
        "    ...: FitView;\n"
        "    open func run(value: int): int { return self.paint(value); }\n"
        "}\n"
        "func exercise(): int {\n"
        "    let text = Button().sourceText(View());\n"
        "    return Button().run(1) + Button.source(2) +\n"
        "        Button().fields(View()) + Button.sourceState(View()) +\n"
        "        FitButton().run(3);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "mixable_seal_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     NULL,
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "View__static__draw__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "Button__static__draw__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "Button__draw__from__i64") != NULL);
    ASSERT(strstr(output.c_source,
                  "Button__log__from__VA_i64") != NULL);
    ASSERT(strstr(output.c_source,
                  "FitView__m0__paint__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "FitButton__static__paint__from__") != NULL);
    ASSERT(strstr(output.c_source,
                  "FitButton__paint__from__i64") != NULL);
    ASSERT(strstr(output.c_source, "->state") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Seal methods and static fields selected by exported spec relationships
 * reuse their ordinary package-callable codegen paths. Unrelated seal
 * siblings retain internal linkage; generic type/fit members use stable
 * package and provider-local symbol domains. */
static void test_selected_seal_spec_implementations_use_open_codegen_path(void) {
    static const char *kSource =
        "open module feng.codegen.spec_impl_dependency;\n"
        "open spec Contract {\n"
        "    static let publicDefault: int;\n"
        "    seal static let selectedLet: int;\n"
        "    seal static var selectedVar: int;\n"
        "    seal func value(): int;\n"
        "}\n"
        "open type Direct: Contract {\n"
        "    static let publicDefault: int = 5;\n"
        "    open static let explicitOpen: int = 6;\n"
        "    seal static let selectedLet: int = 7;\n"
        "    seal static var selectedVar: int = 8;\n"
        "    seal static let unrelatedField: int = 9;\n"
        "    seal func value(): int { return 1; }\n"
        "    seal func unrelated(): int { return 2; }\n"
        "}\n"
        "open type FitValue {\n"
        "    static let publicDefault: int = 10;\n"
        "    seal static let selectedLet: int = 11;\n"
        "    seal static var selectedVar: int = 12;\n"
        "    seal static let unrelatedField: int = 13;\n"
        "}\n"
        "open fit FitValue: Contract {\n"
        "    seal func value(): int { return 3; }\n"
        "    seal func unrelatedFit(): int { return 4; }\n"
        "}\n"
        "open spec GenericContract<T> {\n"
        "    seal func value(): T;\n"
        "    seal static func marker(): T;\n"
        "}\n"
        "open type Generic<T>: GenericContract<T> {\n"
        "    seal func unrelatedBefore(): T { let result: T; return result; }\n"
        "    open func publicBefore(): T { let result: T; return result; }\n"
        "    seal func value(): T { let result: T; return result; }\n"
        "    seal static func marker(): T { let result: T; return result; }\n"
        "    seal func unrelatedAfter(): T { let result: T; return result; }\n"
        "}\n"
        "open type GenericFit<T> {}\n"
        "open fit GenericFit<T>: GenericContract<T> {\n"
        "    seal func unrelatedBefore(): T { let result: T; return result; }\n"
        "    open func publicBefore(): T { let result: T; return result; }\n"
        "    seal func value(): T { let result: T; return result; }\n"
        "    seal static func marker(): T { let result: T; return result; }\n"
        "    seal func unrelatedAfter(): T { let result: T; return result; }\n"
        "}\n"
        ;
    FengProgram *program = parse_or_die(
        kSource, "spec_impl_dependency_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    TestPackageSymbolSelection package_selection = {0};
    FengCodegenPackageSymbolQuery package_query = {
        .user = &package_selection,
        .contains_source_node = test_package_symbol_selection_contains,
    };
    FengCodegenOptions codegen_options = {
        .package_symbols = &package_query,
    };
    bool is_static = false;
    const char *int_c_type = sizeof(void *) >= 8U ? "int64_t" : "int32_t";
    char expected[256];

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    test_package_symbol_selection_add_methods_named(
        &package_selection, program, "value");
    test_package_symbol_selection_add_methods_named(
        &package_selection, program, "marker");
    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     &codegen_options,
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(generated_function_definition_is_static(
        output.c_source, "Direct__value__from__void", &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source, "Direct__unrelated__from__void", &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source, "__m0__value__from__void", &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source, "__m0__unrelatedFit__from__void", &is_static));
    ASSERT(is_static);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "%s Feng__feng__codegen__spec_impl_dependency__Direct__static__publicDefault = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "%s Feng__feng__codegen__spec_impl_dependency__Direct__static__explicitOpen = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "%s Feng__feng__codegen__spec_impl_dependency__Direct__static__selectedLet = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "%s Feng__feng__codegen__spec_impl_dependency__Direct__static__selectedVar = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "static %s Feng__feng__codegen__spec_impl_dependency__Direct__static__unrelatedField = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "%s Feng__feng__codegen__spec_impl_dependency__FitValue__static__selectedLet = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(snprintf(
               expected,
               sizeof(expected),
               "static %s Feng__feng__codegen__spec_impl_dependency__FitValue__static__unrelatedField = 0;",
               int_c_type) > 0);
    ASSERT(strstr(output.c_source, expected) != NULL);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "Direct__static__selectedLet__ensure_init",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "Direct__static__selectedVar__ensure_init",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "Direct__static__unrelatedField__ensure_init",
        &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_dependency__Generic__m0__publicBefore",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_dependency__Generic__m1__value",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_dependency__Generic__m2__marker",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_dependency__Generic__i0__unrelatedBefore",
        &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_dependency__Generic__i1__unrelatedAfter",
        &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_dependency__GenericFit__fm0__publicBefore",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_dependency__GenericFit__fm1__value",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_dependency__GenericFit__fm2__marker",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_dependency__GenericFit__fi0__unrelatedBefore",
        &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_dependency__GenericFit__fi1__unrelatedAfter",
        &is_static));
    ASSERT(is_static);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A seal helper reached only through a selected implementation's callable
 * value dependency participates in the stable package symbol domain, but it
 * is not itself a spec implementation and therefore retains internal C
 * linkage. Cover the shared type and fit package-selection path. */
static void test_reified_callable_dependency_uses_package_symbol_without_linkage(void) {
    static const char *kSource =
        "open module feng.codegen.spec_impl_callable_dependency;\n"
        "open spec Producer<T>(): T;\n"
        "open spec Contract<T> {\n"
        "    seal func value(): T;\n"
        "}\n"
        "open type Generic<T>: Contract<T> {\n"
        "    open func publicBefore(): T { let result: T; return result; }\n"
        "    seal func dependency(): T { let result: T; return result; }\n"
        "    seal func value(): T {\n"
        "        let producer: Producer<T> = self.dependency;\n"
        "        return producer();\n"
        "    }\n"
        "}\n"
        "open type GenericFit<T> {}\n"
        "open fit GenericFit<T>: Contract<T> {\n"
        "    open func publicBefore(): T { let result: T; return result; }\n"
        "    seal func dependency(): T { let result: T; return result; }\n"
        "    seal func value(): T {\n"
        "        let producer: Producer<T> = self.dependency;\n"
        "        return producer();\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "spec_impl_callable_dependency_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    TestPackageSymbolSelection package_selection = {0};
    FengCodegenPackageSymbolQuery package_query = {
        .user = &package_selection,
        .contains_source_node = test_package_symbol_selection_contains,
    };
    FengCodegenOptions codegen_options = {
        .package_symbols = &package_query,
    };
    bool is_static = false;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    test_package_symbol_selection_add_methods_named(
        &package_selection, program, "dependency");
    test_package_symbol_selection_add_methods_named(
        &package_selection, program, "value");
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     &codegen_options,
                                     &output,
                                     &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_callable_dependency__Generic__m1__dependency",
        &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengGenericMethod__feng__codegen__spec_impl_callable_dependency__Generic__m2__value",
        &is_static));
    ASSERT(!is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_callable_dependency__GenericFit__fm1__dependency",
        &is_static));
    ASSERT(is_static);
    ASSERT(generated_function_definition_is_static(
        output.c_source,
        "FengFitMethod__feng__codegen__spec_impl_callable_dependency__GenericFit__fm2__value",
        &is_static));
    ASSERT(!is_static);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Closed generic arguments reuse one file-scope descriptor, while an open
 * child-constraint parameter forwards its existing descriptor directly to a
 * prefix-compatible parent constraint without constructing an adapter. */
static void test_generic_param_descriptor_static_storage_and_forwarding(void) {
    static const char *kSource =
        "module feng.codegen.generic_param_descriptor_storage;\n"
        "spec Parent { func value(): i64; }\n"
        "spec Child: Parent {}\n"
        "type Item: Child {\n"
        "    func value(): i64 { return 41; }\n"
        "}\n"
        "func useParent<T: Parent>(value: T): i64 {\n"
        "    return value.value();\n"
        "}\n"
        "func useChild<U: Child>(value: U): i64 {\n"
        "    return useParent<U>(value);\n"
        "}\n"
        "func run(first: Item, second: Item): i64 {\n"
        "    return useChild<Item>(first) + useChild<Item>(second);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_param_descriptor_storage_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(count_substr(
               output.c_source,
               "static const FengGenericParamDescriptor "
               "_feng_closed_generic_param_desc_0 = {") == 1U);
    ASSERT(strstr(output.c_source,
                  ".descriptor = &FengTypeDesc__feng__codegen__"
                  "generic_param_descriptor_storage__Item") != NULL);
    ASSERT(strstr(output.c_source,
                  ".witness = &FengWitness__feng__codegen__"
                  "generic_param_descriptor_storage__Item__as__feng__codegen__"
                  "generic_param_descriptor_storage__Child") != NULL);
    ASSERT(count_substr(output.c_source,
                        "&_feng_closed_generic_param_desc_0") == 2U);
    ASSERT(strstr(output.c_source,
                  "generic_param_descriptor_storage__useParent_G__from__X") != NULL);
    ASSERT(strstr(output.c_source, ".kind = _U->kind") == NULL);
    ASSERT(strstr(output.c_source, ".descriptor = _U->descriptor") == NULL);
    ASSERT(strstr(output.c_source, ".witness = _U->witness") == NULL);
    ASSERT(strstr(output.c_source,
                  "generic_param_descriptor_storage__useParent_G__from__X"
                  "(&(const FengFunctionDescriptor){.name = "
                  "\"feng__feng__codegen__generic_param_descriptor_storage__"
                  "useParent_G__from__X\"}, _U,") != NULL);
    ASSERT(strstr(output.c_source,
                  "&(const FengGenericParamDescriptor){") == NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A constrained generic call must keep the exact closed UserType identity
 * when materializing its witness. Another closed instance of the same generic
 * declaration may already exist, while the requested instance appears only
 * as an explicit method type argument. Cover both type-head and fit sources. */
static void test_generic_descriptor_uses_exact_closed_spec_implementation(void) {
    static const char *kSource =
        "module feng.codegen.generic_descriptor_exact_witness;\n"
        "spec Surface<T> {\n"
        "    seal func read(): T;\n"
        "    seal static func echo(value: T): T;\n"
        "}\n"
        "type Direct<T>: Surface<T> {\n"
        "    let value: T;\n"
        "    func Direct(value: T) { self.value = value; }\n"
        "    seal func read(): T { return self.value; }\n"
        "    seal static func echo(value: T): T { return value; }\n"
        "}\n"
        "type FitValue<T> {\n"
        "    open let value: T;\n"
        "    func FitValue(value: T) { self.value = value; }\n"
        "}\n"
        "fit FitValue<T>: Surface<T> {\n"
        "    seal func read(): T { return self.value; }\n"
        "    seal static func echo(value: T): T { return value; }\n"
        "}\n"
        "type Access: Surface<int> {\n"
        "    seal func read(): int { return 0; }\n"
        "    seal static func echo(value: int): int { return value; }\n"
        "    static func invoke<U: Surface<int>>(value: int): int {\n"
        "        return U.echo(value);\n"
        "    }\n"
        "}\n"
        "func use(): int {\n"
        "    let directText = Direct<string>(\"direct\");\n"
        "    let fitText = FitValue<string>(\"fit\");\n"
        "    directText;\n"
        "    fitText;\n"
        "    return Access.invoke<Direct<int>>(21) +\n"
        "           Access.invoke<FitValue<int>>(22);\n"
        "}\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_descriptor_exact_witness_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source, "Direct_i64___as") != NULL);
    ASSERT(strstr(output.c_source, "FitValue_i64___as") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* A generic-owner shared wrapper may use its local layout view as `self`, but
 * object-spec coercion must build and cache the witness against the persistent
 * registered nominal instance rather than that stack-owned compiler view. */
static void test_generic_owner_mixable_coercion_uses_nominal_instance(void) {
    static const char *kSource =
        "module feng.codegen.generic_mixin_coercion;\n"
        "spec Widget {}\n"
        "type View<T>: Widget {\n"
        "    var value: T;\n"
        "    @mixable static func echo(target: Widget, value: T): T {\n"
        "        return value;\n"
        "    }\n"
        "}\n"
        "func use(view: View<i64>): i64 { return view.echo(41); }\n";
    FengProgram *program = parse_or_die(
        kSource, "generic_mixin_coercion_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "FengWitness__feng__codegen__generic_mixin_coercion") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

/* Object-spec upcasts use direct-parent witness fields, preserve the subject,
 * converge diamond ancestors, instantiate generic parents, and evaluate the
 * projected source expression exactly once. */
static void test_object_spec_upcast_witness_and_lowering_codegen(void) {
    static const char *kSource =
        "module feng.codegen.spec_upcast;\n"
        "spec Root { func marker(): i32; }\n"
        "spec Left: Root {}\n"
        "spec Right: Root {}\n"
        "spec Diamond: Left, Right {}\n"
        "spec GenericParent<T> { func value(): T; }\n"
        "spec GenericChild<T>: GenericParent<T> {}\n"
        "type Item: Diamond {\n"
        "    func marker(): i32 { return 7; }\n"
        "}\n"
        "type GenericItem: GenericChild<i32> {\n"
        "    func value(): i32 { return 8; }\n"
        "}\n"
        "var sourceCalls: i32 = 0;\n"
        "func makeDiamond(): Diamond {\n"
        "    sourceCalls += 1;\n"
        "    return Item {};\n"
        "}\n"
        "func exercise(value: Diamond, generic: GenericChild<i32>): Root {\n"
        "    let left: Left = value;\n"
        "    let right: Right = value;\n"
        "    let root: Root = value;\n"
        "    let explicit = (Root)value;\n"
        "    let once: Root = makeDiamond();\n"
        "    let genericParent: GenericParent<i32> = generic;\n"
        "    let concreteChild: GenericChild<i32> = GenericItem {};\n"
        "    let concreteParent: GenericParent<i32> = concreteChild;\n"
        "    return root;\n"
        "}\n";
    static const char *kDiamondWitnessStruct =
        "struct FengSpecWitness__feng__codegen__spec_upcast__Diamond {\n"
        "    int32_t (*marker)(void *_subject);\n"
        "    const struct FengSpecWitness__feng__codegen__spec_upcast__Left *parent__FengSpecWitness__feng__codegen__spec_upcast__Left;\n"
        "    const struct FengSpecWitness__feng__codegen__spec_upcast__Right *parent__FengSpecWitness__feng__codegen__spec_upcast__Right;\n"
        "};";
    static const char *kTransitivePath =
        "witness->parent__FengSpecWitness__feng__codegen__spec_upcast__Left"
        "->parent__FengSpecWitness__feng__codegen__spec_upcast__Root";
    static const char *kDiamondRootInitializer =
        ".parent__FengSpecWitness__feng__codegen__spec_upcast__Root = "
        "&FengWitness__feng__codegen__spec_upcast__Item__as__feng__codegen__spec_upcast__Root,";
    static const char *kGenericParentInitializer =
        ".parent__FengSpecWitness__feng__codegen__spec_upcast__GenericParent__GenericABI = "
        "&FengWitness__feng__codegen__spec_upcast__GenericItem__as__feng__codegen__spec_upcast__GenericParent_i32_,";
    static const char *kMakeDiamondSymbol =
        "feng__feng__codegen__spec_upcast__makeDiamond__from__void";
    FengProgram *program = parse_or_die(kSource, "spec_upcast_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &output, &codegen_error)) {
        fprintf(stderr, "codegen error (object spec upcast): %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "unknown codegen error");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);

    /* The child table has flattened dispatch slots followed by only its two
     * direct parents, in source order. Root is reached through Left/Right. */
    ASSERT(strstr(output.c_source, kDiamondWitnessStruct) != NULL);
    ASSERT(strstr(output.c_source, kTransitivePath) != NULL);

    /* Left and Right reuse the one Item-as-Root witness in the diamond. */
    ASSERT(count_substr(output.c_source, kDiamondRootInitializer) == 2U);

    /* The canonical generic parent slot points to the exact i32 witness. */
    ASSERT(strstr(output.c_source, kGenericParentInitializer) != NULL);

    /* Projection copies the same subject and changes only the witness path. */
    ASSERT(strstr(output.c_source,
                  "){ .subject = _spec_view") != NULL);
    ASSERT(strstr(output.c_source,
                  ".witness = _spec_view") != NULL);

    /* Prototype, definition, and one call are the only occurrences. A
     * duplicated projection evaluation would add another call occurrence. */
    ASSERT(count_substr(output.c_source, kMakeDiamondSymbol) == 3U);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Generic spec fields keep the declaration-selected address ABI after the
 * owner and implementation close. Cover open/closed constraint receivers,
 * ordinary spec values, inherited fields, static state initialization, and
 * exact default-parent projection identity in one generated C translation
 * unit. */
static void test_generic_spec_field_stable_address_abi_codegen(void) {
    static const char *kSource =
        "module feng.codegen.generic_spec_field_abi;\n"
        "spec Base<T> {\n"
        "    let initial: T;\n"
        "    var value: T;\n"
        "    static let empty: T;\n"
        "    static var current: T;\n"
        "}\n"
        "spec Child<T>: Base<T> {}\n"
        "type Holder<T>: Child<T> {\n"
        "    let initial: T;\n"
        "    var value: T;\n"
        "    static let empty: T;\n"
        "    static var current: T;\n"
        "    func Holder(value: T) {\n"
        "        self.initial = value;\n"
        "        self.value = value;\n"
        "    }\n"
        "}\n"
        "func openWrite<T, U: Child<T>>(subject: U, next: T): T {\n"
        "    subject.value = next;\n"
        "    U.current = next;\n"
        "    return U.current;\n"
        "}\n"
        "func closedCompound<U: Child<int>>(subject: U): int {\n"
        "    subject.value += 1;\n"
        "    U.current += 2;\n"
        "    return subject.value + U.current;\n"
        "}\n"
        "func specWrite(subject: Child<int>): int {\n"
        "    subject.value = 5;\n"
        "    subject.value += 1;\n"
        "    return subject.value;\n"
        "}\n"
        "func run_case(): int {\n"
        "    let ints = Holder<int>(1);\n"
        "    let intView: Child<int> = ints;\n"
        "    let strings = Holder<string>(\"value\");\n"
        "    let stringView: Child<string> = strings;\n"
        "    let first = openWrite<int, Holder<int>>(ints, 3);\n"
        "    let second = openWrite<string, Holder<string>>(strings, \"next\");\n"
        "    let third = closedCompound<Holder<int>>(ints);\n"
        "    if second != stringView.value { return 0; }\n"
        "    return first + third + specWrite(intView);\n"
        "}\n";
    FengProgram *program = parse_or_die(kSource, "generic_spec_field_abi.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};

    bool semantic_ok = feng_semantic_analyze(
        programs, 1U, FENG_COMPILE_TARGET_LIB,
        &analysis, &errors, &error_count);
    if (!semantic_ok || error_count != 0U) {
        for (size_t index = 0U; index < error_count; ++index) {
            fprintf(stderr,
                    "semantic error (generic spec field ABI): %s: %s\n",
                    errors[index].code != NULL ? errors[index].code : "(none)",
                    errors[index].message != NULL
                        ? errors[index].message
                        : "(unknown)");
        }
    }
    ASSERT(semantic_ok);
    ASSERT(error_count == 0U);
    if (!feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                   NULL, &output, &codegen_error)) {
        fprintf(stderr, "codegen error (generic spec field ABI): %s\n",
                codegen_error.message != NULL
                    ? codegen_error.message
                    : "unknown codegen error");
        ASSERT(false);
    }
    ASSERT(output.c_source != NULL);
    ASSERT(strstr(output.c_source,
                  "void (*get_current)(void *_out);") != NULL);
    ASSERT(strstr(output.c_source,
                  "void (*set_current)(const void * value);") != NULL);
    ASSERT(strstr(output.c_source, "->get_current(&") != NULL);
    ASSERT(strstr(output.c_source, "->borrow_value(") != NULL);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Assignment receiver stabilization retains only borrowed managed targets
 * whose identity can be invalidated by later evaluation or aggregate
 * writeback. Stable let paths and pure scalar writes remain ARC-free. */
static void test_assignment_receiver_stabilization_codegen(void) {
    static const char *source =
        "module feng.codegen.assignment_owner;\n"
        "type Cell {\n"
        "    var value: int = 0;\n"
        "    func read(): int { return self.value; }\n"
        "}\n"
        "spec View {\n"
        "    func read(): int;\n"
        "}\n"
        "type Holder {\n"
        "    var cell: Cell = Cell();\n"
        "    var values: int[!] = [0];\n"
        "}\n"
        "type AggregateHolder {\n"
        "    var view: View;\n"
        "}\n"
        "type AggregateOwner {\n"
        "    var holder: AggregateHolder = AggregateHolder();\n"
        "}\n"
        "type StableHolder {\n"
        "    let cell: Cell = Cell();\n"
        "    func assignThroughSelf() {\n"
        "        self.cell.value = sideEffect();\n"
        "    }\n"
        "}\n"
        "func sideEffect(): int { return 9; }\n"
        "func rebindCell(holder: Holder): int {\n"
        "    holder.cell = Cell();\n"
        "    return 5;\n"
        "}\n"
        "func rebindValues(holder: Holder): int {\n"
        "    holder.values = [0];\n"
        "    return 0;\n"
        "}\n"
        "func stableLet() {\n"
        "    let values: int[!] = [0];\n"
        "    values[0] = sideEffect();\n"
        "}\n"
        "func stableChain(holder: StableHolder) {\n"
        "    holder.cell.value = sideEffect();\n"
        "}\n"
        "func pureVar() {\n"
        "    var values: int[!] = [0];\n"
        "    values[0] = 7 + 2;\n"
        "}\n"
        "func ownedTemporary() {\n"
        "    Cell().value = sideEffect();\n"
        "}\n"
        "func guardedMember(holder: Holder) {\n"
        "    holder.cell.value = rebindCell(holder) + Cell().read();\n"
        "}\n"
        "func guardedIndex(holder: Holder) {\n"
        "    holder.values[rebindValues(holder)] = sideEffect();\n"
        "}\n"
        "func guardedAggregate(owner: AggregateOwner, value: View) {\n"
        "    owner.holder.view = value;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        source, "assignment_receiver_stabilization_codegen.ff");
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput output = {0};
    FengCodegenError codegen_error = {0};
    const char *body_start;
    const char *body_end;

    ASSERT(feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB,
                                 &analysis, &errors, &error_count));
    ASSERT(error_count == 0U);
    ASSERT(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB,
                                     NULL, &output, &codegen_error));
    ASSERT(output.c_source != NULL);

    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__stableLet",
        &body_start, &body_end));
    ASSERT(!span_contains(body_start, body_end,
                          "feng_retain(_assign_owner"));
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__stableChain",
        &body_start, &body_end));
    ASSERT(!span_contains(body_start, body_end,
                          "feng_retain(_assign_owner"));
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__pureVar",
        &body_start, &body_end));
    ASSERT(!span_contains(body_start, body_end,
                          "feng_retain(_assign_owner"));
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__ownedTemporary",
        &body_start, &body_end));
    ASSERT(!span_contains(body_start, body_end,
                          "feng_retain(_assign_owner"));

    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__guardedMember",
        &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end,
                         "feng_retain(_assign_owner"));
    ASSERT(span_contains(body_start, body_end,
                         "feng_cleanup_push(&_cu__assign_owner"));
    ASSERT(count_substr_in_span(body_start, body_end,
                                "feng_cleanup_pop(); feng_release(") >= 2U);
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__guardedIndex",
        &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end,
                         "feng_retain(_assign_owner"));
    ASSERT(span_contains(body_start, body_end,
                         "feng_cleanup_pop(); feng_release(_assign_owner"));
    ASSERT(find_generated_function_body(
        output.c_source,
        "static void feng__feng__codegen__assignment_owner__guardedAggregate",
        &body_start, &body_end));
    ASSERT(span_contains(body_start, body_end,
                         "feng_retain(_assign_owner"));
    ASSERT(span_contains(body_start, body_end,
                         "feng_aggregate_assign"));
    ASSERT(count_substr(output.c_source,
                        "feng_retain(_assign_owner") == 3U);
    compile_generated_c_or_die(output.c_source);

    feng_codegen_output_free(&output);
    feng_codegen_error_free(&codegen_error);
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
}

int main(void) {
    (void)system("rm -rf temp");
    (void)mkdir("temp", 0755);

    test_multi_file_bin();
    test_member_mix_fields_and_mixable_wrappers_codegen();
    test_mixable_seal_wrappers_use_static_codegen_path();
    test_selected_seal_spec_implementations_use_open_codegen_path();
    test_reified_callable_dependency_uses_package_symbol_without_linkage();
    test_generic_param_descriptor_static_storage_and_forwarding();
    test_generic_descriptor_uses_exact_closed_spec_implementation();
    test_generic_owner_mixable_coercion_uses_nominal_instance();
    test_multi_file_lib();
    test_private_generic_representation_same_package_codegen();
    test_private_representation_cross_package_ft_codegen();
    test_generic_fit_member_dependencies_cross_package_codegen();
    test_c_variadic_cross_package_ft_codegen();
    test_module_binding_lazy_ensure_init_codegen();
    test_address_of_module_binding_uses_storage_slot_codegen();
    test_module_scalar_var_assignment_marks_initialized_codegen();
    test_module_managed_var_assignment_marks_initialized_codegen();
    test_type_static_members_codegen();
    test_builtin_fit_static_method_codegen();
    test_user_fit_static_method_symbol_is_stable();
    test_generic_static_methods_codegen();
    test_generic_type_static_field_codegen();
    test_generic_type_inferred_field_codegen();
    test_generic_type_inferred_field_concrete_call_codegen();
    test_spec_static_member_witness_codegen();
    test_object_spec_method_parameter_bindings_keep_witness_abi();
    test_object_spec_cross_surface_fields_keep_distinct_slots();
    test_object_spec_special_names_keep_distinct_witness_slots();
    test_object_spec_default_initialization_uses_fresh_subjects();
    test_object_spec_owned_subjects_move_into_persistent_slots();
    test_spec_c_tags_precede_all_witness_signatures_codegen();
    test_spec_static_var_witness_codegen();
    test_generic_param_static_method_codegen();
    test_generic_param_static_field_read_codegen();
    test_generic_param_static_field_write_codegen();
    test_spec_inherited_static_slot_codegen();
    test_generic_spec_field_stable_address_abi_codegen();
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
    test_open_generic_param_descriptor_remains_runtime_scoped();
    test_runtime_extern_codegen_rejects_non_contract_symbol();
    test_unsupported_pointer_pointee_reports_explicit_error();
    test_generic_function_codegen_failure_propagates();
    test_abi_value_function_pointer_codegen();
    test_lib_public_functions_are_exported();
    test_bin_public_functions_remain_static();
    test_imported_feng_function_prototypes_compile();
    test_imported_alias_qualified_type_annotations_codegen_compile();
    test_imported_full_path_type_annotations_codegen_compile_without_use();
    test_full_path_values_codegen_compile_without_import();
    test_imported_public_let_binding_codegen_compiles();
    test_imported_public_static_members_codegen_compiles();
    test_imported_selected_seal_static_fields_codegen_compiles();
    test_imported_public_var_binding_read_write_codegen_compiles();
    test_imported_public_binding_address_of_codegen_compiles();
    test_public_binding_lib_exports_slot_and_ensure_init_codegen();
    test_public_binding_infers_constructor_type_codegen();
    test_imported_public_binding_inferred_type_codegen_compiles();
    test_enum_codegen_emits_stable_symbols();
    test_imported_enum_codegen_emits_visible_symbols();
    test_imported_enum_union_default_codegen_stays_file_scoped();
    test_imported_generic_enum_argument_uses_canonical_identity();
    test_imported_field_debug_type_uses_declaring_program_context();
    test_same_named_types_in_distinct_modules();
    test_float_modulo_codegen_uses_math_runtime();
    test_string_literal_codegen_preserves_exact_bytes();
    test_integer_runtime_semantics_codegen_is_zero_cost();
    test_fit_builtin_direct_call_codegen_shape();
    test_fit_builtin_array_open_generic_return_codegen();
    test_fit_builtin_array_open_generic_value_return_codegen();
    test_fit_builtin_and_array_object_spec_coercion_codegen();
    test_fit_enum_object_spec_coercion_codegen();
    test_object_spec_upcast_witness_and_lowering_codegen();
    test_object_spec_thunk_subject_cast_shape_codegen();
    test_intersection_spec_witness_struct_codegen();
    test_intersection_spec_witness_instance_codegen();
    test_intersection_spec_default_zero_codegen();
    test_generic_intersection_spec_codegen();
    test_generic_fn_codegen();
    test_generic_type_decl_no_crash();
    test_generic_fn_call_codegen();
    test_generic_shared_body_direct_dependencies_codegen();
    test_generic_shared_method_descriptor_order_codegen();
    test_closed_generic_owner_method_dependencies_codegen();
    test_closed_generic_fit_member_dependencies_codegen();
    test_generic_type_owner_reification_codegen();
    test_non_generic_default_zero_stays_direct_codegen();
    test_closed_generic_default_zero_stays_direct_codegen();
    test_generic_direct_result_uses_descriptor_sized_storage_codegen();
    test_generic_managed_return_let_binding_codegen();
    test_generic_spec_arg_codegen();
    test_callable_spec_top_level_fn_codegen();
    test_callable_spec_reference_identity_equality_codegen();
    test_generic_callable_constraint_codegen();
    test_computed_callable_result_call_codegen();
    test_generic_object_spec_instance_codegen();
    test_generic_callable_spec_instance_codegen();
    test_generic_object_spec_callable_field_call_codegen();
    test_open_generic_callable_field_default_codegen();
    test_generic_object_spec_coercion_codegen();
    test_generic_callable_spec_coercion_codegen();
    test_callable_spec_method_coercion_codegen();
    test_object_spec_method_value_codegen_uses_bound_witness();
    test_intersection_spec_method_value_codegen_uses_merged_witness();
    test_intersection_spec_overload_codegen_preserves_exact_slots();
    test_object_spec_overload_codegen_preserves_exact_slots();
    test_object_spec_closed_parent_witness_uses_exact_implementation();
    test_fit_instance_static_same_name_codegen_symbols_are_distinct();
    test_constrained_generic_spec_method_value_codegen();
    test_constrained_generic_intersection_method_value_codegen();
    test_intersection_constrained_static_method_call_codegen();
    test_concrete_enum_array_fit_codegen();
    test_constrained_generic_spec_static_method_value_codegen();
    test_intersection_constrained_static_method_value_codegen();
    test_builtin_fit_static_spec_witness_codegen();
    test_array_fit_static_spec_witness_codegen();
    test_concrete_static_method_value_codegen_uses_singletons();
    test_value_method_capture_codegen_has_direct_closure_lowering();
    test_generic_callable_value_reification_codegen();
    test_unbound_callable_explicit_cast_codegen();
    test_callable_spec_lambda_local_capture_codegen();
    test_generic_lambda_dynamic_capture_codegen();
    test_lambda_tuple_body_uses_callable_return_target_codegen();
    test_generic_call_adopts_owned_aggregate_argument_codegen();
    test_callable_spec_lambda_self_capture_codegen();
    test_finalizer_lambda_capture_codegen();
    test_callable_spec_lambda_argument_codegen();
    test_callable_spec_other_coercion_codegen();
    test_callable_spec_other_field_read_coercion_codegen();
    test_generic_constraint_witness_codegen();
    test_generic_runtime_type_kind_codegen();
    test_generic_aggregate_facts_shape_codegen();
    test_fit_enum_generic_constraint_codegen();
    test_generic_user_fit_object_spec_coercion_codegen();
    test_generic_constrained_spec_value_codegen();
    test_generic_child_spec_parent_constraint_codegen();
    test_spec_aggregate_field_codegen();
    test_spec_value_field_receiver_codegen();
    test_generic_constrained_aggregate_spec_value_codegen();
    test_if_expr_aggregate_result_codegen();
    test_match_expr_aggregate_result_codegen();
    test_generic_expression_join_result_codegen();
    test_match_statement_codegen();
    test_enum_match_statement_codegen();
    test_enum_match_expression_codegen();
    test_generic_aggregate_return_codegen();
    test_generic_value_construction_uses_reified_storage_codegen();
    test_generic_type_generic_method_codegen();
    test_generic_owner_method_constraint_codegen();
    test_generic_scalar_instance_direct_call_codegen();
    test_phase_e_aggregate_generic_arg_three_entrances_codegen();
    test_type_field_initializers_codegen();
    test_type_field_callable_lambda_initializer_codegen();
    test_callable_field_default_and_explicit_initialization_codegen();
    test_void_try_expression_codegen();
    test_try_catch_return_codegen();
    test_generic_try_body_reified_storage_codegen();
    test_generic_loop_reified_storage_codegen();
    test_generic_iterator_fixed_storage_reified_cleanup_codegen();
    test_multi_parameter_generic_callable_abi_codegen();
    test_empty_array_literal_codegen_uses_target_contexts();
    test_user_constructor_forms_codegen();
    test_object_literal_shorthand_invokes_selected_constructor();
    test_variadic_zero_args_codegen();
    test_variadic_multi_args_codegen();
    test_variadic_fixed_prefix_codegen();
    test_variadic_constructor_codegen();
    test_generic_variadic_instance_method_codegen();
    test_prepacked_variadic_forwarding_codegen();
    test_variadic_callable_spec_lambda_codegen();
    test_tuple_value_codegen_core();
    test_tuple_managed_slots_codegen();
    test_union_form_spec_codegen();
    test_union_object_spec_leaf_coercion_codegen();
    test_generic_union_form_spec_codegen();
    test_generic_union_literal_adaptation_type_lifetime();
    test_generic_union_reified_storage_and_array_source_codegen();
    test_generic_union_constraint_omits_runtime_witness_codegen();
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
    test_assignment_receiver_stabilization_codegen();
    fprintf(stdout, "codegen tests passed\n");
    return 0;
}
