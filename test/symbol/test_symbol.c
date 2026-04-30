#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/provider.h"
#include "symbol/symbol.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static FengSlice slice_from_cstr(const char *text) {
    FengSlice slice;

    slice.data = text;
    slice.length = strlen(text);
    return slice;
}

static bool slice_equals_cstr(FengSlice slice, const char *text) {
    size_t length = strlen(text);

    return slice.length == length && memcmp(slice.data, text, length) == 0;
}

static FengProgram *parse_or_die(const char *path, const char *source) {
    FengProgram *program = NULL;
    FengParseError error;

    if (!feng_parse_source(source, strlen(source), path, &program, &error)) {
        fprintf(stderr,
                "parse failed for %s at %u:%u: %s\n",
                path,
                error.token.line,
                error.token.column,
                error.message != NULL ? error.message : "unknown parse error");
        exit(1);
    }
    ASSERT(program != NULL);
    return program;
}

static FengSemanticAnalysis *analyze_or_die(const FengProgram *program) {
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    programs[0] = program;
    if (!feng_semantic_analyze(programs, 1U, FENG_COMPILE_TARGET_LIB, &analysis, &errors, &error_count)) {
        fprintf(stderr, "semantic analysis failed with %zu error(s)\n", error_count);
        if (errors != NULL && error_count > 0U && errors[0].message != NULL) {
            fprintf(stderr, "first error: %s\n", errors[0].message);
        }
        exit(1);
    }
    ASSERT(error_count == 0U);
    ASSERT(analysis != NULL);
    return analysis;
}

static char *make_temp_dir(void) {
    char *template_path = strdup("/tmp/feng_symbol_roundtrip_XXXXXX");
    char *result;

    ASSERT(template_path != NULL);
    result = mkdtemp(template_path);
    ASSERT(result != NULL);
    return result;
}

static int remove_dir_recursive(const char *path) {
    /* Best-effort: defer to /bin/rm via system to keep the test concise.
     * The temp directory lives under /tmp and contains only artefacts the
     * test itself produced, so we accept the small risk in test code. */
    char command[1024];
    int written = snprintf(command, sizeof(command), "rm -rf '%s'", path);

    if (written < 0 || (size_t)written >= sizeof(command)) {
        return -1;
    }
    return system(command);
}

/* Round-trip: write graph -> read back -> provider answers public queries. */
static void test_roundtrip_public_module(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.roundtrip;\n"
        "\n"
        "pu fn add(a: int, b: int): int { return a + b; }\n"
        "pu fn greet(name: string): string { return name; }\n";

    FengProgram *program = parse_or_die("roundtrip.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[3];
    const FengSymbolDeclView *add_decl = NULL;
    const FengSymbolDeclView *greet_decl = NULL;
    const FengSymbolTypeView *param_type = NULL;
    const FengSymbolTypeView *return_type = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);

    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        options.workspace_root = NULL;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }

    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/roundtrip.ft",
                    public_root) > 0);
    {
        struct stat st;
        ASSERT(stat(public_ft, &st) == 0);
        ASSERT(st.st_size > 0);
    }

    /* Build a fresh provider purely from the on-disk .ft to make sure the
     * read path produces a usable view independent of the in-memory graph. */
    ASSERT(feng_symbol_provider_create(&provider, &error));
    if (!feng_symbol_provider_add_ft_root(provider,
                                          public_root,
                                          FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                          &error)) {
        fprintf(stderr,
                "add_ft_root failed: %s (path=%s)\n",
                error.message != NULL ? error.message : "(no message)",
                error.path != NULL ? error.path : "(no path)");
        ASSERT(false);
    }

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    /* The third segment intentionally points at the leaf "roundtrip" via the
     * `symbol` parent — provider expects the full module path. */
    {
        FengSlice full[4];
        full[0] = slice_from_cstr("feng");
        full[1] = slice_from_cstr("test");
        full[2] = slice_from_cstr("symbol");
        full[3] = slice_from_cstr("roundtrip");
        module = feng_symbol_provider_find_module(provider, full, 4U);
    }
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_segment_count(module) == 4U);
    ASSERT(slice_equals_cstr(feng_symbol_module_segment_at(module, 0U), "feng"));
    ASSERT(slice_equals_cstr(feng_symbol_module_segment_at(module, 3U), "roundtrip"));

    add_decl = feng_symbol_module_find_public_value(module, slice_from_cstr("add"));
    ASSERT(add_decl != NULL);
    ASSERT(feng_symbol_decl_kind(add_decl) == FENG_SYMBOL_DECL_KIND_FUNCTION);
    ASSERT(feng_symbol_decl_visibility(add_decl) == FENG_VISIBILITY_PUBLIC);
    ASSERT(slice_equals_cstr(feng_symbol_decl_name(add_decl), "add"));
    ASSERT(feng_symbol_decl_param_count(add_decl) == 2U);
    ASSERT(slice_equals_cstr(feng_symbol_decl_param_name(add_decl, 0U), "a"));
    ASSERT(slice_equals_cstr(feng_symbol_decl_param_name(add_decl, 1U), "b"));

    param_type = feng_symbol_decl_param_type(add_decl, 0U);
    ASSERT(param_type != NULL);
    ASSERT(feng_symbol_type_kind(param_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
    ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(param_type), "i32"));

    return_type = feng_symbol_decl_return_type(add_decl);
    ASSERT(return_type != NULL);
    ASSERT(feng_symbol_type_kind(return_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
    ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(return_type), "i32"));

    greet_decl = feng_symbol_module_find_public_value(module, slice_from_cstr("greet"));
    ASSERT(greet_decl != NULL);
    ASSERT(feng_symbol_decl_param_count(greet_decl) == 1U);
    {
        const FengSymbolTypeView *t = feng_symbol_decl_param_type(greet_decl, 0U);
        ASSERT(t != NULL);
        ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(t), "string"));
    }

    /* Private lookups must not surface non-existent decls. */
    ASSERT(feng_symbol_module_find_public_value(module, slice_from_cstr("missing")) == NULL);

    feng_symbol_provider_free(provider);
    feng_symbol_graph_free(graph);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Private modules must not produce a public .ft file. */
static void test_private_module_skipped(void) {
    static const char *kSource =
        "mod feng.test.symbol.private_only;\n"
        "fn local(): int { return 0; }\n";

    FengProgram *program = parse_or_die("private.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char workspace_root[1024];
    char public_ft[1024];
    char workspace_ft[1024];
    struct stat st;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    ASSERT(snprintf(workspace_root, sizeof(workspace_root), "%s/obj/symbols", tmp_dir) > 0);

    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        options.workspace_root = workspace_root;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }

    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/private_only.ft",
                    public_root) > 0);
    ASSERT(snprintf(workspace_ft,
                    sizeof(workspace_ft),
                    "%s/feng/test/symbol/private_only.ft",
                    workspace_root) > 0);

    /* No public .ft is emitted for a private module. */
    ASSERT(stat(public_ft, &st) != 0 && errno == ENOENT);
    /* Workspace cache is always written. */
    ASSERT(stat(workspace_ft, &st) == 0 && st.st_size > 0);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Reader rejects files whose magic does not match. */
static void test_reader_rejects_bad_magic(void) {
    char *tmp_dir = make_temp_dir();
    char path[1024];
    FILE *fp;
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    FengSymbolFtReadOptions options = {0};

    ASSERT(snprintf(path, sizeof(path), "%s/bad.ft", tmp_dir) > 0);
    fp = fopen(path, "wb");
    ASSERT(fp != NULL);
    ASSERT(fwrite("XXXX", 1U, 4U, fp) == 4U);
    fclose(fp);

    options.expected_profile = FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC;
    ASSERT(!feng_symbol_ft_read_file(path, &options, &graph, &error));
    ASSERT(graph == NULL);
    ASSERT(error.message != NULL);

    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

int main(void) {
    test_roundtrip_public_module();
    test_private_module_skipped();
    test_reader_rejects_bad_magic();
    fprintf(stdout, "symbol tests passed\n");
    return 0;
}
