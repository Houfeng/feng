#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "archive/zip.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/imported_module.h"
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

static void assert_zip_ok(bool ok, char **zip_error) {
    if (!ok) {
        fprintf(stderr,
                "zip operation failed: %s\n",
                zip_error != NULL && *zip_error != NULL ? *zip_error : "unknown error");
        if (zip_error != NULL) {
            free(*zip_error);
            *zip_error = NULL;
        }
        ASSERT(false);
    }
    if (zip_error != NULL) {
        free(*zip_error);
        *zip_error = NULL;
    }
}

static void export_public_source_or_die(const char *path,
                                        const char *source,
                                        const char *public_root) {
    FengProgram *program = parse_or_die(path, source);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    FengSymbolExportOptions options = {0};

    options.public_root = public_root;
    options.emit_docs = true;
    if (!feng_symbol_export_analysis(analysis, &options, &error)) {
        fprintf(stderr,
                "symbol export failed: %s\n",
                error.message != NULL ? error.message : "unknown error");
        ASSERT(false);
    }
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void export_public_sources_or_die(const char *const *paths,
                                         const char *const *sources,
                                         size_t source_count,
                                         const char *public_root) {
    FengProgram **programs = NULL;
    const FengProgram **program_views = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSymbolError error = {0};
    FengSymbolExportOptions options = {0};
    size_t index;

    ASSERT(source_count > 0U);
    programs = (FengProgram **)calloc(source_count, sizeof(*programs));
    program_views = (const FengProgram **)calloc(source_count, sizeof(*program_views));
    ASSERT(programs != NULL);
    ASSERT(program_views != NULL);

    for (index = 0U; index < source_count; ++index) {
        programs[index] = parse_or_die(paths[index], sources[index]);
        program_views[index] = programs[index];
    }

    ASSERT(feng_semantic_analyze(program_views,
                                 source_count,
                                 FENG_COMPILE_TARGET_LIB,
                                 &analysis,
                                 &errors,
                                 &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    options.public_root = public_root;
    options.emit_docs = true;
    ASSERT(feng_symbol_export_analysis(analysis, &options, &error));

    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    for (index = 0U; index < source_count; ++index) {
        feng_program_free(programs[index]);
    }
    free(program_views);
    free(programs);
}

static void write_bundle_with_file_or_die(const char *bundle_path,
                                          const char *entry_path,
                                          const char *source_path) {
    FengZipWriter writer = {0};
    char *zip_error = NULL;

    assert_zip_ok(feng_zip_writer_open(bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_file(&writer,
                                           entry_path,
                                           source_path,
                                           FENG_ZIP_COMPRESSION_DEFLATE,
                                           &zip_error),
                  &zip_error);
    assert_zip_ok(feng_zip_writer_finalize(&writer, &zip_error), &zip_error);
    feng_zip_writer_dispose(&writer);
}

static void write_bundle_with_bytes_or_die(const char *bundle_path,
                                           const char *entry_path,
                                           const void *data,
                                           size_t data_size) {
    FengZipWriter writer = {0};
    char *zip_error = NULL;

    assert_zip_ok(feng_zip_writer_open(bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_bytes(&writer,
                                            entry_path,
                                            data,
                                            data_size,
                                            FENG_ZIP_COMPRESSION_DEFLATE,
                                            &zip_error),
                  &zip_error);
    assert_zip_ok(feng_zip_writer_finalize(&writer, &zip_error), &zip_error);
    feng_zip_writer_dispose(&writer);
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

static void test_roundtrip_public_module_docs(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.docs;\n"
        "/**\n"
        " * Adds two integers.\n"
        " * Keeps Feng doc newlines.\n"
        " */\n"
        "pu fn add(a: int, b: int): int { return a + b; }\n"
        "/** User record */\n"
        "pu type User {\n"
        "    /** Stable identifier */\n"
        "    pu let id: int;\n"
        "}\n";

    FengProgram *program = parse_or_die("docs_roundtrip.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    FengSymbolExportOptions options = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *add_decl = NULL;
    const FengSymbolDeclView *user_decl = NULL;
    const FengSymbolDeclView *id_decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    options.public_root = public_root;
    options.emit_docs = true;
    ASSERT(feng_symbol_export_analysis(analysis, &options, &error));

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("docs");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    add_decl = feng_symbol_module_find_public_value(module, slice_from_cstr("add"));
    ASSERT(add_decl != NULL);
    ASSERT(slice_equals_cstr(feng_symbol_decl_doc(add_decl),
                             "Adds two integers.\nKeeps Feng doc newlines."));

    user_decl = feng_symbol_module_find_public_type(module, slice_from_cstr("User"));
    ASSERT(user_decl != NULL);
    ASSERT(slice_equals_cstr(feng_symbol_decl_doc(user_decl), "User record"));

    id_decl = feng_symbol_decl_find_public_member(user_decl, slice_from_cstr("id"));
    ASSERT(id_decl != NULL);
    ASSERT(slice_equals_cstr(feng_symbol_decl_doc(id_decl), "Stable identifier"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
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

static void test_provider_loads_bundle_public_module(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.bundle;\n"
        "pu fn answer(): int { return 42; }\n";

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *answer_decl = NULL;
    const FengSymbolDeclView *public_decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/bundle_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/bundle.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/provider_bundle.fb", tmp_dir) > 0);

    export_public_source_or_die("bundle.ff", kSource, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/bundle.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    if (!feng_symbol_provider_add_bundle(provider, bundle_path, &error)) {
        fprintf(stderr,
                "add_bundle failed: %s (path=%s)\n",
                error.message != NULL ? error.message : "(no message)",
                error.path != NULL ? error.path : "(no path)");
        ASSERT(false);
    }

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("bundle");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_public_decl_count(module) == 1U);
    public_decl = feng_symbol_module_public_decl_at(module, 0U);
    ASSERT(public_decl != NULL);
    ASSERT(feng_symbol_decl_kind(public_decl) == FENG_SYMBOL_DECL_KIND_FUNCTION);
    ASSERT(slice_equals_cstr(feng_symbol_decl_name(public_decl), "answer"));
    ASSERT(feng_symbol_module_public_decl_at(module, 1U) == NULL);
    answer_decl = feng_symbol_module_find_public_value(module, slice_from_cstr("answer"));
    ASSERT(answer_decl != NULL);
    ASSERT(feng_symbol_decl_kind(answer_decl) == FENG_SYMBOL_DECL_KIND_FUNCTION);
    ASSERT(feng_symbol_decl_param_count(answer_decl) == 0U);
    ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(feng_symbol_decl_return_type(answer_decl)), "i32"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_provider_rejects_duplicate_bundle_module(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.conflict;\n"
        "pu fn marker(): int { return 1; }\n";

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char first_bundle[1024];
    char second_bundle[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSlice segments[4];

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/conflict_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/conflict.ft",
                    public_root) > 0);
    ASSERT(snprintf(first_bundle, sizeof(first_bundle), "%s/first.fb", tmp_dir) > 0);
    ASSERT(snprintf(second_bundle, sizeof(second_bundle), "%s/second.fb", tmp_dir) > 0);

    export_public_source_or_die("conflict.ff", kSource, public_root);
    write_bundle_with_file_or_die(first_bundle,
                                  "mod/feng/test/symbol/conflict.ft",
                                  public_ft);
    write_bundle_with_file_or_die(second_bundle,
                                  "mod/feng/test/symbol/conflict.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, first_bundle, &error));
    ASSERT(!feng_symbol_provider_add_bundle(provider, second_bundle, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "duplicate imported module") != NULL);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("conflict");
    ASSERT(feng_symbol_provider_find_module(provider, segments, 4U) != NULL);

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_provider_rejects_bad_bundle_symbol_entry(void) {
    static const char kBadBytes[] = "XXXX";

    char *tmp_dir = make_temp_dir();
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};

    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/bad_symbol.fb", tmp_dir) > 0);
    write_bundle_with_bytes_or_die(bundle_path,
                                   "mod/feng/test/symbol/bad.ft",
                                   kBadBytes,
                                   sizeof(kBadBytes) - 1U);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(!feng_symbol_provider_add_bundle(provider, bundle_path, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "failed to load bundle symbol entry") != NULL);

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_imported_module_cache_keeps_synthesized_modules_alive(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.imported_cache;\n"
        "pu fn answer(): int { return 42; }\n";

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSlice segments[4];
    const FengSemanticModule *first = NULL;
    const FengSemanticModule *second = NULL;
    const FengProgram *program = NULL;
    const FengDecl *decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/imported_cache_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/imported_cache.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/imported_cache.fb", tmp_dir) > 0);

    export_public_source_or_die("imported_cache.ff", kSource, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/imported_cache.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &error));

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    ASSERT(query.user == cache);
    ASSERT(query.get_module != NULL);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("imported_cache");

    first = query.get_module(query.user, segments, 4U);
    ASSERT(first != NULL);
    ASSERT(first->origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE);
    ASSERT(first->program_count == 1U);
    second = query.get_module(query.user, segments, 4U);
    ASSERT(second == first);

    ASSERT(query.get_module(query.user, segments, 3U) == NULL);

    feng_symbol_provider_free(provider);
    provider = NULL;

    program = first->programs[0];
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl != NULL);
    ASSERT(decl->kind == FENG_DECL_FUNCTION);
    ASSERT(slice_equals_cstr(decl->as.function_decl.name, "answer"));

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_imported_module_cache_keeps_bundle_fit_modules_alive(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.bundle_fit;\n"
        "pu fit string {\n"
        "    pu fn length(): i64 { return 1; }\n"
        "}\n";

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSlice segments[4];
    const FengSemanticModule *module = NULL;
    const FengProgram *program = NULL;
    const FengDecl *decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/imported_bundle_fit_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/bundle_fit.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/imported_bundle_fit.fb", tmp_dir) > 0);

    export_public_source_or_die("bundle_fit.ff", kSource, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/bundle_fit.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &error));

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("bundle_fit");
    module = query.get_module(query.user, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(module->program_count == 1U);

    program = module->programs[0];
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl != NULL);
    ASSERT(decl->kind == FENG_DECL_FIT);
    ASSERT(decl->as.fit_decl.member_count == 1U);
    ASSERT(decl->as.fit_decl.members[0] != NULL);
    ASSERT(decl->as.fit_decl.members[0]->kind == FENG_TYPE_MEMBER_METHOD);
    ASSERT(slice_equals_cstr(decl->as.fit_decl.members[0]->as.callable.name, "length"));

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_imported_module_cache_keeps_multi_file_bundle_fit_modules_alive(void) {
    static const char *kPaths[] = {
        "bundle_multi_fit_type.ff",
        "bundle_multi_fit_ext.ff",
    };
    static const char *kSources[] = {
        "pu mod feng.test.symbol.bundle_multi_fit;\n"
        "pu type Marker {}\n",
        "pu mod feng.test.symbol.bundle_multi_fit;\n"
        "pu fit string {\n"
        "    pu fn length(): i64 { return 1; }\n"
        "}\n",
    };

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSlice segments[4];
    const FengSemanticModule *module = NULL;
    const FengProgram *program = NULL;
    bool found_type = false;
    bool found_fit = false;
    size_t index;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/imported_bundle_multi_fit_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/bundle_multi_fit.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/imported_bundle_multi_fit.fb", tmp_dir) > 0);

    export_public_sources_or_die(kPaths, kSources, 2U, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/bundle_multi_fit.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &error));

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("bundle_multi_fit");
    module = query.get_module(query.user, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(module->program_count == 1U);

    program = module->programs[0];
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 2U);
    for (index = 0U; index < program->declaration_count; ++index) {
        const FengDecl *decl = program->declarations[index];

        ASSERT(decl != NULL);
        if (decl->kind == FENG_DECL_TYPE) {
            found_type = true;
        }
        if (decl->kind == FENG_DECL_FIT) {
            found_fit = true;
            ASSERT(decl->as.fit_decl.member_count == 1U);
            ASSERT(decl->as.fit_decl.members[0] != NULL);
            ASSERT(decl->as.fit_decl.members[0]->kind == FENG_TYPE_MEMBER_METHOD);
            ASSERT(slice_equals_cstr(decl->as.fit_decl.members[0]->as.callable.name, "length"));
        }
    }
    ASSERT(found_type);
    ASSERT(found_fit);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_generic_function_ft_roundtrip(void) {
    /* pu fn identity<T>(x: T): T
     * After roundtrip: function decl should have type_param_count == 1,
     * and the parameter type / return type should be TYPE_PARAM_REF with name "T". */
    static const char *kSource =
        "pu mod feng.test.symbol.generic_fn;\n"
        "\n"
        "pu fn identity<T>(x: T): T { return x; }\n";

    FengProgram *program = parse_or_die("generic_fn.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolDeclView *fn_decl = NULL;
    const FengSymbolTypeView *param_type = NULL;
    const FengSymbolTypeView *return_type = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                             public_root,
                                             FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                             &error));
    feng_symbol_error_free(&error);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("generic_fn");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    fn_decl = feng_symbol_module_find_public_value(module, slice_from_cstr("identity"));
    ASSERT(fn_decl != NULL);
    ASSERT(feng_symbol_decl_kind(fn_decl) == FENG_SYMBOL_DECL_KIND_FUNCTION);
    ASSERT(feng_symbol_decl_type_param_count(fn_decl) == 1U);
    ASSERT(feng_symbol_decl_param_count(fn_decl) == 1U);

    param_type = feng_symbol_decl_param_type(fn_decl, 0U);
    ASSERT(param_type != NULL);
    ASSERT(feng_symbol_type_kind(param_type) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(param_type), "T"));

    return_type = feng_symbol_decl_return_type(fn_decl);
    ASSERT(return_type != NULL);
    ASSERT(feng_symbol_type_kind(return_type) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(return_type), "T"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_generic_type_ft_roundtrip(void) {
    /* pu type Box<T> { pu let value: T; }
     * After roundtrip: type decl should have type_param_count == 1,
     * and the field type should be TYPE_PARAM_REF with name "T". */
    static const char *kSource =
        "pu mod feng.test.symbol.generic_type;\n"
        "\n"
        "pu type Box<T> { pu let value: T; }\n";

    FengProgram *program = parse_or_die("generic_type.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolDeclView *type_decl = NULL;
    const FengSymbolDeclView *value_field = NULL;
    const FengSymbolTypeView *field_type = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                             public_root,
                                             FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                             &error));
    feng_symbol_error_free(&error);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("generic_type");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    type_decl = feng_symbol_module_find_public_type(module, slice_from_cstr("Box"));
    ASSERT(type_decl != NULL);
    ASSERT(feng_symbol_decl_kind(type_decl) == FENG_SYMBOL_DECL_KIND_TYPE);
    ASSERT(feng_symbol_decl_type_param_count(type_decl) == 1U);

    value_field = feng_symbol_decl_find_public_member(type_decl, slice_from_cstr("value"));
    ASSERT(value_field != NULL);
    ASSERT(feng_symbol_decl_kind(value_field) == FENG_SYMBOL_DECL_KIND_FIELD);

    field_type = feng_symbol_decl_value_type(value_field);
    ASSERT(field_type != NULL);
    ASSERT(feng_symbol_type_kind(field_type) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(field_type), "T"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_generic_fit_ft_roundtrip(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.generic_fit;\n"
        "\n"
        "pu spec Reader<T> { fn read(): T; }\n"
        "pu type Box<T> { pu let value: T; }\n"
        "pu fit Box<T>: Reader<T> {\n"
        "    pu fn read(): T { return self.value; }\n"
        "}\n";

    FengProgram *program = parse_or_die("generic_fit.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolFitView *fit_view = NULL;
    const FengSymbolDeclView *fit_decl = NULL;
    const FengSymbolTypeView *target_type = NULL;
    const FengSymbolTypeView *target_arg = NULL;
    const FengSymbolTypeView *spec_type = NULL;
    const FengSymbolTypeView *spec_arg = NULL;
    const FengSymbolDeclView *read_method = NULL;
    const FengSymbolTypeView *read_return = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                             public_root,
                                             FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                             &error));
    feng_symbol_error_free(&error);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("generic_fit");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_fit_count(module) == 1U);

    fit_view = feng_symbol_module_fit_at(module, 0U);
    ASSERT(fit_view != NULL);
    fit_decl = feng_symbol_fit_decl(fit_view);
    ASSERT(fit_decl != NULL);
    ASSERT(feng_symbol_decl_kind(fit_decl) == FENG_SYMBOL_DECL_KIND_FIT);

    target_type = feng_symbol_decl_fit_target(fit_decl);
    ASSERT(target_type != NULL);
    ASSERT(feng_symbol_type_kind(target_type) == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(target_type) == 1U);
    target_arg = feng_symbol_type_generic_arg_at(target_type, 0U);
    ASSERT(target_arg != NULL);
    ASSERT(feng_symbol_type_kind(target_arg) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(target_arg), "T"));

    ASSERT(feng_symbol_decl_declared_spec_count(fit_decl) == 1U);
    spec_type = feng_symbol_decl_declared_spec_at(fit_decl, 0U);
    ASSERT(spec_type != NULL);
    ASSERT(feng_symbol_type_kind(spec_type) == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(spec_type) == 1U);
    spec_arg = feng_symbol_type_generic_arg_at(spec_type, 0U);
    ASSERT(spec_arg != NULL);
    ASSERT(feng_symbol_type_kind(spec_arg) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(spec_arg), "T"));

    ASSERT(feng_symbol_decl_member_count(fit_decl) == 1U);
    read_method = feng_symbol_decl_member_at(fit_decl, 0U);
    ASSERT(read_method != NULL);
    ASSERT(feng_symbol_decl_kind(read_method) == FENG_SYMBOL_DECL_KIND_METHOD);
    read_return = feng_symbol_decl_return_type(read_method);
    ASSERT(read_return != NULL);
    ASSERT(feng_symbol_type_kind(read_return) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(read_return), "T"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_fit_builtin_and_array_target_nodes_ft_roundtrip(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.fit_target_nodes;\n"
        "\n"
        "pu spec Label { fn label(): string; }\n"
        "pu fit int: Label {\n"
        "    pu fn label(): string { return \"i32\"; }\n"
        "}\n"
        "pu fit string: Label {\n"
        "    pu fn label(): string { return self; }\n"
        "}\n"
        "pu fit int[]: Label {\n"
        "    pu fn label(): string { return \"arr_ro\"; }\n"
        "}\n"
        "pu fit int[!]: Label {\n"
        "    pu fn label(): string { return \"arr_rw\"; }\n"
        "}\n";

    FengProgram *program = parse_or_die("fit_target_nodes.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    size_t fit_index = 0U;
    size_t saw_i32 = 0U;
    size_t saw_string = 0U;
    size_t saw_arr_ro = 0U;
    size_t saw_arr_rw = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));
    feng_symbol_error_free(&error);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("fit_target_nodes");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_fit_count(module) == 4U);

    for (fit_index = 0U; fit_index < feng_symbol_module_fit_count(module); ++fit_index) {
        const FengSymbolFitView *fit_view = feng_symbol_module_fit_at(module, fit_index);
        const FengSymbolDeclView *fit_decl = feng_symbol_fit_decl(fit_view);
        const FengSymbolTypeView *target_type = feng_symbol_decl_fit_target(fit_decl);

        ASSERT(fit_decl != NULL);
        ASSERT(feng_symbol_decl_kind(fit_decl) == FENG_SYMBOL_DECL_KIND_FIT);
        ASSERT(target_type != NULL);

        if (feng_symbol_type_kind(target_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN) {
            FengSlice builtin = feng_symbol_type_builtin_name(target_type);
            if (slice_equals_cstr(builtin, "i32")) {
                saw_i32++;
            } else if (slice_equals_cstr(builtin, "string")) {
                saw_string++;
            } else {
                ASSERT(false);
            }
            continue;
        }

        ASSERT(feng_symbol_type_kind(target_type) == FENG_SYMBOL_TYPE_KIND_ARRAY);
        ASSERT(feng_symbol_type_array_rank(target_type) == 1U);
        {
            const FengSymbolTypeView *elem = feng_symbol_type_inner(target_type);
            ASSERT(elem != NULL);
            ASSERT(feng_symbol_type_kind(elem) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
            ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(elem), "i32"));
        }
        if (feng_symbol_type_array_layer_writable(target_type, 0U)) {
            saw_arr_rw++;
        } else {
            saw_arr_ro++;
        }
    }

    ASSERT(saw_i32 == 1U);
    ASSERT(saw_string == 1U);
    ASSERT(saw_arr_ro == 1U);
    ASSERT(saw_arr_rw == 1U);

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_fit_array_type_param_target_ft_roundtrip(void) {
    static const char *kSource =
        "pu mod feng.test.symbol.fit_array_type_param_target;\n"
        "\n"
        "pu spec ArrTag<T> { fn count(): int; }\n"
        "pu fit T[!]: ArrTag<T> {\n"
        "    pu fn count(): int { return 0; }\n"
        "}\n";

    FengProgram *program = parse_or_die("fit_array_type_param_target.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolFitView *fit_view = NULL;
    const FengSymbolDeclView *fit_decl = NULL;
    const FengSymbolTypeView *target_type = NULL;
    const FengSymbolTypeView *target_element = NULL;
    const FengSymbolTypeView *spec_type = NULL;
    const FengSymbolTypeView *spec_arg = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    {
        FengSymbolExportOptions options = {0};
        options.public_root = public_root;
        ASSERT(feng_symbol_export_analysis(analysis, &options, &error));
    }
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));
    feng_symbol_error_free(&error);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("fit_array_type_param_target");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_fit_count(module) == 1U);

    fit_view = feng_symbol_module_fit_at(module, 0U);
    ASSERT(fit_view != NULL);
    fit_decl = feng_symbol_fit_decl(fit_view);
    ASSERT(fit_decl != NULL);
    ASSERT(feng_symbol_decl_kind(fit_decl) == FENG_SYMBOL_DECL_KIND_FIT);

    target_type = feng_symbol_decl_fit_target(fit_decl);
    ASSERT(target_type != NULL);
    ASSERT(feng_symbol_type_kind(target_type) == FENG_SYMBOL_TYPE_KIND_ARRAY);
    ASSERT(feng_symbol_type_array_rank(target_type) == 1U);
    ASSERT(feng_symbol_type_array_layer_writable(target_type, 0U));
    target_element = feng_symbol_type_inner(target_type);
    ASSERT(target_element != NULL);
    ASSERT(feng_symbol_type_kind(target_element) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(target_element), "T"));

    ASSERT(feng_symbol_decl_declared_spec_count(fit_decl) == 1U);
    spec_type = feng_symbol_decl_declared_spec_at(fit_decl, 0U);
    ASSERT(spec_type != NULL);
    ASSERT(feng_symbol_type_kind(spec_type) == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(spec_type) == 1U);
    spec_arg = feng_symbol_type_generic_arg_at(spec_type, 0U);
    ASSERT(spec_arg != NULL);
    ASSERT(feng_symbol_type_kind(spec_arg) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(spec_arg), "T"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

int main(void) {
    test_roundtrip_public_module();
    test_roundtrip_public_module_docs();
    test_private_module_skipped();
    test_reader_rejects_bad_magic();
    test_provider_loads_bundle_public_module();
    test_provider_rejects_duplicate_bundle_module();
    test_provider_rejects_bad_bundle_symbol_entry();
    test_imported_module_cache_keeps_synthesized_modules_alive();
    test_imported_module_cache_keeps_bundle_fit_modules_alive();
    test_imported_module_cache_keeps_multi_file_bundle_fit_modules_alive();
    test_generic_function_ft_roundtrip();
    test_generic_type_ft_roundtrip();
    test_generic_fit_ft_roundtrip();
    test_fit_builtin_and_array_target_nodes_ft_roundtrip();
    test_fit_array_type_param_target_ft_roundtrip();
    fprintf(stdout, "symbol tests passed\n");
    return 0;
}
