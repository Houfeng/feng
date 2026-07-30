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

static void assert_builtin_type_name(const FengSymbolTypeView *type, const char *name) {
    ASSERT(type != NULL);
    ASSERT(feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
    ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(type), name));
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
    char *template_path = strdup("temp/feng_symbol_roundtrip_XXXXXX");
    char *result;

    ASSERT(template_path != NULL);
    result = mkdtemp(template_path);
    ASSERT(result != NULL);
    return result;
}

static int remove_dir_recursive(const char *path) {
    /* Best-effort: defer to /bin/rm via system to keep the test concise. */
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
        "open module feng.test.symbol.roundtrip;\n"
        "\n"
        "open func add(a: int, b: int): int { return a + b; }\n"
        "open func greet(name: string): string { return name; }\n";

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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(param_type), int_canonical));
    }

    return_type = feng_symbol_decl_return_type(add_decl);
    ASSERT(return_type != NULL);
    ASSERT(feng_symbol_type_kind(return_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(return_type), int_canonical));
    }

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

static void test_union_spec_ft_roundtrip_preserves_normalized_members(void) {
    static const char *kSource =
        "open module feng.test.symbol.union_roundtrip;\n"
        "open spec Maybe: string | int | string;\n"
        "open spec Value: Maybe | bool | int;\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *value_decl = NULL;
    const FengSemanticModule *semantic_module = NULL;
    const FengDecl *synth_value_decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("union_roundtrip.ff", kSource, public_root);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("union_roundtrip");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    value_decl = feng_symbol_module_find_public_spec(module, slice_from_cstr("Value"));
    ASSERT(value_decl != NULL);
    ASSERT(feng_symbol_decl_kind(value_decl) == FENG_SYMBOL_DECL_KIND_SPEC);
    ASSERT(feng_symbol_decl_union_member_count(value_decl) == 3U);
    /* Member 0: Maybe (nested union spec, not expanded) */
    {
        const FengSymbolTypeView *maybe_type = feng_symbol_decl_union_member_at(value_decl, 0U);
        size_t seg_count;
        ASSERT(maybe_type != NULL);
        ASSERT(feng_symbol_type_kind(maybe_type) == FENG_SYMBOL_TYPE_KIND_NAMED);
        seg_count = feng_symbol_type_segment_count(maybe_type);
        ASSERT(seg_count >= 1U);
        ASSERT(slice_equals_cstr(feng_symbol_type_segment_at(maybe_type, seg_count - 1U), "Maybe"));
    }
    /* Member 1: bool (builtin) */
    assert_builtin_type_name(feng_symbol_decl_union_member_at(value_decl, 1U), "bool");
    /* Member 2: int (builtin, platform-dependent canonical name) */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        assert_builtin_type_name(feng_symbol_decl_union_member_at(value_decl, 2U), int_canonical);
    }

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    ASSERT(semantic_module->program_count == 1U);
    ASSERT(semantic_module->programs[0]->declaration_count == 2U);
    synth_value_decl = semantic_module->programs[0]->declarations[1];
    ASSERT(synth_value_decl->kind == FENG_DECL_SPEC);
    ASSERT(synth_value_decl->as.spec_decl.form == FENG_SPEC_FORM_UNION);
    ASSERT(synth_value_decl->as.spec_decl.as.union_form.member_count == 3U);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_intersection_spec_ft_roundtrip_preserves_members(void) {
    static const char *kSource =
        "open module feng.test.symbol.intersection_roundtrip;\n"
        "open spec Greetable {\n"
        "    func greet(): string;\n"
        "}\n"
        "open spec Displayable {\n"
        "    func display(): string;\n"
        "}\n"
        "open spec GreetAndDisplay: Greetable & Displayable;\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *gd_decl = NULL;
    const FengSemanticModule *semantic_module = NULL;
    const FengDecl *synth_gd_decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("intersection_roundtrip.ff", kSource, public_root);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("intersection_roundtrip");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    gd_decl = feng_symbol_module_find_public_spec(module, slice_from_cstr("GreetAndDisplay"));
    ASSERT(gd_decl != NULL);
    ASSERT(feng_symbol_decl_kind(gd_decl) == FENG_SYMBOL_DECL_KIND_SPEC);
    ASSERT(feng_symbol_decl_spec_form(gd_decl) == FENG_SPEC_FORM_INTERSECTION);
    ASSERT(feng_symbol_decl_intersection_member_count(gd_decl) == 2U);
    {
        const FengSymbolTypeView *member0 = feng_symbol_decl_intersection_member_at(gd_decl, 0U);
        size_t seg_count;
        ASSERT(member0 != NULL);
        ASSERT(feng_symbol_type_kind(member0) == FENG_SYMBOL_TYPE_KIND_NAMED);
        seg_count = feng_symbol_type_segment_count(member0);
        ASSERT(seg_count >= 1U);
        ASSERT(slice_equals_cstr(feng_symbol_type_segment_at(member0, seg_count - 1U), "Greetable"));
    }
    {
        const FengSymbolTypeView *member1 = feng_symbol_decl_intersection_member_at(gd_decl, 1U);
        size_t seg_count;
        ASSERT(member1 != NULL);
        ASSERT(feng_symbol_type_kind(member1) == FENG_SYMBOL_TYPE_KIND_NAMED);
        seg_count = feng_symbol_type_segment_count(member1);
        ASSERT(seg_count >= 1U);
        ASSERT(slice_equals_cstr(feng_symbol_type_segment_at(member1, seg_count - 1U), "Displayable"));
    }

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    synth_gd_decl = NULL;
    for (size_t i = 0U; i < semantic_module->programs[0]->declaration_count; ++i) {
        const FengDecl *d = semantic_module->programs[0]->declarations[i];
        if (d->kind == FENG_DECL_SPEC &&
            slice_equals_cstr(d->as.spec_decl.name, "GreetAndDisplay")) {
            synth_gd_decl = d;
            break;
        }
    }
    ASSERT(synth_gd_decl != NULL);
    ASSERT(synth_gd_decl->as.spec_decl.form == FENG_SPEC_FORM_INTERSECTION);
    ASSERT(synth_gd_decl->as.spec_decl.as.intersection_form.member_count == 2U);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_bounded_decl_ft_roundtrip_uses_inferred_initializer(void) {
    static const char *kSource =
        "open module feng.test.symbol.bounded;\n"
        "open let module_id: int = 9;\n"
        "open type User {\n"
        "    open let id: int = 1;\n"
        "    open let created_at: int;\n"
        "    open static let version: int = 2;\n"
        "    func User(ts: int) {\n"
        "        self.created_at = ts;\n"
        "    }\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSlice module_segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *user_decl = NULL;
    const FengSymbolDeclView *module_id_decl = NULL;
    const FengSymbolDeclView *id_field = NULL;
    const FengSymbolDeclView *created_at_field = NULL;
    const FengSymbolDeclView *version_field = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("bounded.ff", kSource, public_root);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    module_segments[0] = slice_from_cstr("feng");
    module_segments[1] = slice_from_cstr("test");
    module_segments[2] = slice_from_cstr("symbol");
    module_segments[3] = slice_from_cstr("bounded");
    module = feng_symbol_provider_find_module(provider, module_segments, 4U);
    ASSERT(module != NULL);

    module_id_decl = feng_symbol_module_find_public_value(module, slice_from_cstr("module_id"));
    ASSERT(module_id_decl != NULL);
    ASSERT(!feng_symbol_decl_has_bounded_decl(module_id_decl));

    user_decl = feng_symbol_module_find_public_type(module, slice_from_cstr("User"));
    ASSERT(user_decl != NULL);
    id_field = feng_symbol_decl_find_public_member(user_decl, slice_from_cstr("id"));
    ASSERT(id_field != NULL);
    ASSERT(feng_symbol_decl_has_bounded_decl(id_field));

    created_at_field = feng_symbol_decl_find_public_member(user_decl, slice_from_cstr("created_at"));
    ASSERT(created_at_field != NULL);
    ASSERT(!feng_symbol_decl_has_bounded_decl(created_at_field));

    version_field = feng_symbol_decl_find_public_member(user_decl, slice_from_cstr("version"));
    ASSERT(version_field != NULL);
    ASSERT(feng_symbol_decl_is_static(version_field));
    ASSERT(!feng_symbol_decl_has_bounded_decl(version_field));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_roundtrip_public_module_docs(void) {
    static const char *kSource =
        "open module feng.test.symbol.docs;\n"
        "/**\n"
        " * Adds two integers.\n"
        " * Keeps Feng doc newlines.\n"
        " */\n"
        "open func add(a: int, b: int): int { return a + b; }\n"
        "/** User record */\n"
        "open type User {\n"
        "    /** Stable identifier */\n"
        "    open let id: int;\n"
        "    /** Static version */\n"
        "    open static let version: int = 1;\n"
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
    const FengSymbolDeclView *version_decl = NULL;

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
    ASSERT(!feng_symbol_decl_is_static(id_decl));

    version_decl = feng_symbol_decl_find_public_member(user_decl, slice_from_cstr("version"));
    ASSERT(version_decl != NULL);
    ASSERT(feng_symbol_decl_is_static(version_decl));
    ASSERT(slice_equals_cstr(feng_symbol_decl_doc(version_decl), "Static version"));

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
        "module feng.test.symbol.private_only;\n"
        "func local(): int { return 0; }\n";

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
        "open module feng.test.symbol.bundle;\n"
        "open func answer(): int { return 42; }\n";

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
    /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
    {
        const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
        ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(feng_symbol_decl_return_type(answer_decl)), int_canonical));
    }

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_enum_ft_roundtrip_exports_items_and_values(void) {
    static const char *kSource =
        "open module feng.test.symbol.enum_roundtrip;\n"
        "\n"
        "open enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n";

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *enum_decl = NULL;
    const FengSymbolDeclView *ok_decl = NULL;
    const FengSymbolDeclView *not_found_decl = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/enum_roundtrip_mod", tmp_dir) > 0);
    export_public_source_or_die("enum_roundtrip.ff", kSource, public_root);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("enum_roundtrip");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    enum_decl = feng_symbol_module_find_public_enum(module, slice_from_cstr("HttpStatus"));
    ASSERT(enum_decl != NULL);
    ASSERT(feng_symbol_decl_kind(enum_decl) == FENG_SYMBOL_DECL_KIND_ENUM);
    ASSERT(feng_symbol_module_find_public_type(module, slice_from_cstr("HttpStatus")) == NULL);
    ASSERT(feng_symbol_module_find_public_value(module, slice_from_cstr("Ok")) == NULL);
    ASSERT(feng_symbol_decl_member_count(enum_decl) == 2U);

    ok_decl = feng_symbol_decl_find_public_member(enum_decl, slice_from_cstr("Ok"));
    ASSERT(ok_decl != NULL);
    ASSERT(feng_symbol_decl_kind(ok_decl) == FENG_SYMBOL_DECL_KIND_ENUM_ITEM);
    ASSERT(feng_symbol_decl_has_enum_item_value(ok_decl));
    ASSERT(feng_symbol_decl_enum_item_ordinal(ok_decl) == 0U);
    ASSERT(feng_symbol_decl_enum_item_value(ok_decl) == 200);

    not_found_decl = feng_symbol_decl_find_public_member(enum_decl, slice_from_cstr("NotFound"));
    ASSERT(not_found_decl != NULL);
    ASSERT(feng_symbol_decl_kind(not_found_decl) == FENG_SYMBOL_DECL_KIND_ENUM_ITEM);
    ASSERT(feng_symbol_decl_has_enum_item_value(not_found_decl));
    ASSERT(feng_symbol_decl_enum_item_ordinal(not_found_decl) == 1U);
    ASSERT(feng_symbol_decl_enum_item_value(not_found_decl) == 404);

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_provider_rejects_duplicate_bundle_module(void) {
    static const char *kSource =
        "open module feng.test.symbol.conflict;\n"
        "open func marker(): int { return 1; }\n";

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
        "open module feng.test.symbol.imported_cache;\n"
        "open func answer(): int { return 42; }\n";

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

static void test_imported_module_cache_preserves_extern_c_symbol_name(void) {
    static const char *kSource =
        "open module feng.test.symbol.imported_extern_symbol;\n"
        "@cdecl(\"m\", \"fabs\")\n"
        "open extern func abs_value(x: double): double;\n";

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

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/imported_extern_symbol_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/imported_extern_symbol.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/imported_extern_symbol.fb", tmp_dir) > 0);

    export_public_source_or_die("imported_extern_symbol.ff", kSource, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/imported_extern_symbol.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &error));

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("imported_extern_symbol");
    module = query.get_module(query.user, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(module->program_count == 1U);

    program = module->programs[0];
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl != NULL);
    ASSERT(decl->kind == FENG_DECL_FUNCTION);
    ASSERT(decl->is_extern);
    ASSERT(slice_equals_cstr(decl->as.function_decl.name, "abs_value"));
    ASSERT(decl->annotation_count == 1U);
    ASSERT(decl->annotations[0].builtin_kind == FENG_ANNOTATION_CDECL);
    ASSERT(decl->annotations[0].arg_count == 2U);
    ASSERT(decl->annotations[0].args[0]->kind == FENG_EXPR_STRING);
    ASSERT(decl->annotations[0].args[1]->kind == FENG_EXPR_STRING);
    ASSERT(slice_equals_cstr(decl->annotations[0].args[0]->as.string, "\"m\""));
    ASSERT(slice_equals_cstr(decl->annotations[0].args[1]->as.string, "\"fabs\""));

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_imported_module_cache_preserves_enum_items(void) {
    static const char *kSource =
        "open module feng.test.symbol.imported_enum_cache;\n"
        "open enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
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

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/imported_enum_cache_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/imported_enum_cache.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/imported_enum_cache.fb", tmp_dir) > 0);

    export_public_source_or_die("imported_enum_cache.ff", kSource, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/imported_enum_cache.ft",
                                  public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &error));

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("imported_enum_cache");
    module = query.get_module(query.user, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(module->program_count == 1U);

    program = module->programs[0];
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 1U);
    decl = program->declarations[0];
    ASSERT(decl != NULL);
    ASSERT(decl->kind == FENG_DECL_ENUM);
    ASSERT(slice_equals_cstr(decl->as.enum_decl.name, "HttpStatus"));
    ASSERT(decl->as.enum_decl.item_count == 2U);
    ASSERT(slice_equals_cstr(decl->as.enum_decl.items[0].name, "Ok"));
    ASSERT(decl->as.enum_decl.items[0].has_explicit_value);
    ASSERT(decl->as.enum_decl.items[0].explicit_value == 200);
    ASSERT(slice_equals_cstr(decl->as.enum_decl.items[1].name, "NotFound"));
    ASSERT(decl->as.enum_decl.items[1].has_explicit_value);
    ASSERT(decl->as.enum_decl.items[1].explicit_value == 404);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_imported_enum_value_participates_in_semantic_analysis(void) {
    static const char *kExternalSource =
        "open module vendor.status;\n"
        "open enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n";
    static const char *kMainSource =
        "module demo.main;\n"
        "import vendor.status as status;\n"
        "func run(): int {\n"
        "    let value: status.HttpStatus = status.HttpStatus.NotFound;\n"
        "    return (int)value;\n"
        "}\n";

    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError symbol_error = {0};
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSemanticAnalyzeOptions options = {0};
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/imported_enum_semantic_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/vendor/status.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/imported_enum_semantic.fb", tmp_dir) > 0);

    export_public_source_or_die("external_enum.ff", kExternalSource, public_root);
    write_bundle_with_file_or_die(bundle_path, "mod/vendor/status.ft", public_ft);

    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &symbol_error));

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = sizeof(void *);

    program = parse_or_die("imported_enum_main.ff", kMainSource);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(analysis != NULL);
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_imported_module_cache_keeps_bundle_fit_modules_alive(void) {
    static const char *kSource =
        "open module feng.test.symbol.bundle_fit;\n"
        "open fit string {\n"
        "    open func length(): i64 { return 1; }\n"
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
        "open module feng.test.symbol.bundle_multi_fit;\n"
        "open type Marker {}\n",
        "open module feng.test.symbol.bundle_multi_fit;\n"
        "open fit string {\n"
        "    open func length(): i64 { return 1; }\n"
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
    /* open fn identity<T>(x: T): T
     * After roundtrip: function decl should have type_param_count == 1,
     * and the parameter type / return type should be TYPE_PARAM_REF with name "T". */
    static const char *kSource =
        "open module feng.test.symbol.generic_fn;\n"
        "\n"
        "open func identity<T>(x: T): T { return x; }\n";

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
    /* open type Box<T> { open let value: T; }
     * After roundtrip: type decl should have type_param_count == 1,
     * and the field type should be TYPE_PARAM_REF with name "T". */
    static const char *kSource =
        "open module feng.test.symbol.generic_type;\n"
        "\n"
        "open type Box<T> { open let value: T; }\n";

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

/* Regression for the bug where inferred field types that are generic
 * applications (e.g. `seal let x = Box<i32>()`) lost their type args during
 * .ft export, degrading to a plain NAMED node and breaking cross-package
 * codegen with CE0032.  Covers:
 *  - concrete generic application: seal let concrete = Box<i32>()
 *  - open (uninstantiated) generic application: open let open_arg = Box<T>()
 *    inside an enclosing generic type.
 * After roundtrip, both field value types must be NAMED_GENERIC with the
 * correct base name and type-arg count, and the open variant's type arg
 * must be a TYPE_PARAM_REF named "T". */
static void test_inferred_generic_field_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.inferred_generic_field;\n"
        "\n"
        "open type Box<T> { open let value: T; }\n"
        "\n"
        "open type ConcreteHolder {\n"
        "    seal let concrete = Box<i32>();\n"
        "}\n"
        "\n"
        "open type OpenHolder<T> {\n"
        "    seal let open_arg = Box<T>();\n"
        "}\n";

    FengProgram *program = parse_or_die("inferred_generic_field.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolDeclView *concrete_type = NULL;
    const FengSymbolDeclView *concrete_field = NULL;
    const FengSymbolTypeView *concrete_type_view = NULL;
    const FengSymbolDeclView *open_type = NULL;
    const FengSymbolDeclView *open_field = NULL;
    const FengSymbolTypeView *open_type_view = NULL;
    const FengSymbolTypeView *field_type = NULL;
    const FengSymbolTypeView *arg_type = NULL;

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
    segments[3] = slice_from_cstr("inferred_generic_field");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    /* Concrete generic application field: seal let concrete = Box<i32>() */
    concrete_type = feng_symbol_module_find_public_type(module, slice_from_cstr("ConcreteHolder"));
    ASSERT(concrete_type != NULL);
    {
        size_t mcount = feng_symbol_decl_member_count(concrete_type);
        size_t mi;
        for (mi = 0U; mi < mcount; ++mi) {
            const FengSymbolDeclView *m = feng_symbol_decl_member_at(concrete_type, mi);
            if (m != NULL &&
                slice_equals_cstr(feng_symbol_decl_name(m), "concrete")) {
                concrete_field = m;
                break;
            }
        }
    }
    ASSERT(concrete_field != NULL);
    ASSERT(feng_symbol_decl_kind(concrete_field) == FENG_SYMBOL_DECL_KIND_FIELD);
    field_type = feng_symbol_decl_value_type(concrete_field);
    ASSERT(field_type != NULL);
    ASSERT(feng_symbol_type_kind(field_type) == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(field_type) == 1U);
    arg_type = feng_symbol_type_generic_arg_at(field_type, 0U);
    ASSERT(arg_type != NULL);
    ASSERT(feng_symbol_type_kind(arg_type) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
    ASSERT(slice_equals_cstr(feng_symbol_type_builtin_name(arg_type), "i32"));

    /* Open generic application field: seal let open_arg = Box<T>()
     * inside OpenHolder<T>.  The type arg must be a TYPE_PARAM_REF named T. */
    open_type = feng_symbol_module_find_public_type(module, slice_from_cstr("OpenHolder"));
    ASSERT(open_type != NULL);
    ASSERT(feng_symbol_decl_type_param_count(open_type) == 1U);
    {
        size_t mcount = feng_symbol_decl_member_count(open_type);
        size_t mi;
        for (mi = 0U; mi < mcount; ++mi) {
            const FengSymbolDeclView *m = feng_symbol_decl_member_at(open_type, mi);
            if (m != NULL &&
                slice_equals_cstr(feng_symbol_decl_name(m), "open_arg")) {
                open_field = m;
                break;
            }
        }
    }
    ASSERT(open_field != NULL);
    ASSERT(feng_symbol_decl_kind(open_field) == FENG_SYMBOL_DECL_KIND_FIELD);
    field_type = feng_symbol_decl_value_type(open_field);
    ASSERT(field_type != NULL);
    ASSERT(feng_symbol_type_kind(field_type) == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(field_type) == 1U);
    arg_type = feng_symbol_type_generic_arg_at(field_type, 0U);
    ASSERT(arg_type != NULL);
    ASSERT(feng_symbol_type_kind(arg_type) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(arg_type), "T"));

    /* ConcreteHolder and OpenHolder should both be visible as public types
     * (sanity check that the export did not fail silently). */
    (void)concrete_type_view;
    (void)open_type_view;

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_generic_fit_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_fit;\n"
        "\n"
        "open spec Reader<T> { func read(): T; }\n"
        "open type Box<T> { open let value: T; }\n"
        "open fit Box<T>: Reader<T> {\n"
        "    open func read(): T { return self.value; }\n"
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

static void test_generic_fit_named_generic_return_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_fit_named_generic_return;\n"
        "\n"
        "open spec Slicer<T> { func slice(from: i32, to: i32): Span<T>; }\n"
        "open type Span<T> {}\n"
        "open fit Span<T>: Slicer<T> {\n"
        "    open func slice(from: i32, to: i32): Span<T> { return self; }\n"
        "}\n";

    FengProgram *program = parse_or_die("generic_fit_named_generic_return.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolFitView *fit_view = NULL;
    const FengSymbolDeclView *fit_decl = NULL;
    const FengSymbolDeclView *slice_method = NULL;
    const FengSymbolTypeView *slice_return = NULL;
    const FengSymbolTypeView *slice_arg = NULL;

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
    segments[3] = slice_from_cstr("generic_fit_named_generic_return");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_fit_count(module) == 1U);

    fit_view = feng_symbol_module_fit_at(module, 0U);
    ASSERT(fit_view != NULL);
    fit_decl = feng_symbol_fit_decl(fit_view);
    ASSERT(fit_decl != NULL);
    ASSERT(feng_symbol_decl_kind(fit_decl) == FENG_SYMBOL_DECL_KIND_FIT);

    ASSERT(feng_symbol_decl_member_count(fit_decl) == 1U);
    slice_method = feng_symbol_decl_member_at(fit_decl, 0U);
    ASSERT(slice_method != NULL);
    ASSERT(feng_symbol_decl_kind(slice_method) == FENG_SYMBOL_DECL_KIND_METHOD);
    ASSERT(slice_equals_cstr(feng_symbol_decl_name(slice_method), "slice"));

    slice_return = feng_symbol_decl_return_type(slice_method);
    ASSERT(slice_return != NULL);
    ASSERT(feng_symbol_type_kind(slice_return) == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(slice_return) == 1U);
    slice_arg = feng_symbol_type_generic_arg_at(slice_return, 0U);
    ASSERT(slice_arg != NULL);
    ASSERT(feng_symbol_type_kind(slice_arg) == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(slice_arg), "T"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

static void test_fit_builtin_and_array_target_nodes_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.fit_target_nodes;\n"
        "\n"
        "open spec Label { func label(): string; }\n"
        "open fit int: Label {\n"
        "    open func label(): string { return \"i32\"; }\n"
        "}\n"
        "open fit string: Label {\n"
        "    open func label(): string { return self; }\n"
        "}\n"
        "open fit int[]: Label {\n"
        "    open func label(): string { return \"arr_ro\"; }\n"
        "}\n"
        "open fit int[!]: Label {\n"
        "    open func label(): string { return \"arr_rw\"; }\n"
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
            /* int is platform-dependent: i32 on 32-bit, i64 on 64-bit. */
            const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
            if (slice_equals_cstr(builtin, int_canonical)) {
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
            FengSlice builtin;
            const FengSymbolTypeView *elem = feng_symbol_type_inner(target_type);
            const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";
            ASSERT(elem != NULL);
            ASSERT(feng_symbol_type_kind(elem) == FENG_SYMBOL_TYPE_KIND_BUILTIN);
            builtin = feng_symbol_type_builtin_name(elem);
            ASSERT(slice_equals_cstr(builtin, int_canonical));
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
        "open module feng.test.symbol.fit_array_type_param_target;\n"
        "\n"
        "open spec ArrTag<T> { func count(): int; }\n"
        "open fit T[!]: ArrTag<T> {\n"
        "    open func count(): int { return 0; }\n"
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

static void test_private_representation_dependency_closure_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.private_repr;\n"
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
        "type UnusedPrivate {}\n"
        "fit HiddenManaged<T> {\n"
        "    func unused(): int { return 0; }\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *public_box = NULL;
    const FengSymbolDeclView *hidden_managed = NULL;
    const FengSymbolDeclView *hidden_value = NULL;
    const FengSymbolDeclView *hidden_tuple = NULL;
    const FengSymbolDeclView *hidden_enum = NULL;
    const FengSymbolDeclView *hidden_object = NULL;
    const FengSymbolDeclView *hidden_callable = NULL;
    const FengSymbolDeclView *hidden_union = NULL;
    const FengSymbolDeclView *hidden_intersection = NULL;
    const FengSymbolDeclView *hidden_recursive = NULL;
    const FengSymbolDeclView *managed_field = NULL;
    const FengSymbolDeclView *public_type_param = NULL;
    const FengSymbolTypeView *managed_type = NULL;
    const FengSymbolTypeView *managed_arg = NULL;
    const FengSemanticModule *semantic_module = NULL;
    size_t index;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("private_repr.ff", kSource, public_root);

    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("private_repr");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_public_decl_count(module) == 1U);
    ASSERT(feng_symbol_module_fit_count(module) == 0U);
    ASSERT(feng_symbol_module_find_public_type(module,
                                               slice_from_cstr("HiddenManaged")) == NULL);

    public_box = feng_symbol_module_find_public_type(module,
                                                     slice_from_cstr("PublicBox"));
    ASSERT(public_box != NULL);
    for (index = 0U; index < feng_symbol_module_decl_count(module); ++index) {
        const FengSymbolDeclView *decl = feng_symbol_module_decl_at(module, index);
        FengSlice name = feng_symbol_decl_name(decl);

        if (slice_equals_cstr(name, "HiddenManaged")) hidden_managed = decl;
        if (slice_equals_cstr(name, "HiddenValue")) hidden_value = decl;
        if (slice_equals_cstr(name, "HiddenTuple")) hidden_tuple = decl;
        if (slice_equals_cstr(name, "HiddenEnum")) hidden_enum = decl;
        if (slice_equals_cstr(name, "HiddenObject")) hidden_object = decl;
        if (slice_equals_cstr(name, "HiddenCallable")) hidden_callable = decl;
        if (slice_equals_cstr(name, "HiddenUnion")) hidden_union = decl;
        if (slice_equals_cstr(name, "HiddenIntersection")) hidden_intersection = decl;
        if (slice_equals_cstr(name, "HiddenRecursive")) hidden_recursive = decl;
        ASSERT(!slice_equals_cstr(name, "UnusedPrivate"));
    }
    ASSERT(hidden_managed != NULL);
    ASSERT(hidden_value != NULL);
    ASSERT(hidden_tuple != NULL);
    ASSERT(hidden_enum != NULL);
    ASSERT(hidden_object != NULL);
    ASSERT(hidden_callable != NULL);
    ASSERT(hidden_union != NULL);
    ASSERT(hidden_intersection != NULL);
    ASSERT(hidden_recursive != NULL);
    ASSERT(feng_symbol_decl_visibility(hidden_managed) == FENG_VISIBILITY_PRIVATE);
    ASSERT(feng_symbol_decl_is_value_type(hidden_value));
    ASSERT(feng_symbol_decl_is_tuple(hidden_tuple));
    ASSERT(feng_symbol_decl_member_count(hidden_enum) == 2U);
    ASSERT(feng_symbol_decl_spec_form(hidden_object) == FENG_SPEC_FORM_OBJECT);
    ASSERT(feng_symbol_decl_spec_form(hidden_callable) == FENG_SPEC_FORM_CALLABLE);
    ASSERT(feng_symbol_decl_spec_form(hidden_union) == FENG_SPEC_FORM_UNION);
    ASSERT(feng_symbol_decl_spec_form(hidden_intersection) ==
           FENG_SPEC_FORM_INTERSECTION);

    for (index = 0U; index < feng_symbol_decl_member_count(public_box); ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(public_box, index);

        if (feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
            public_type_param = member;
        } else if (slice_equals_cstr(feng_symbol_decl_name(member), "managed")) {
            managed_field = member;
        }
    }
    ASSERT(public_type_param != NULL);
    ASSERT(managed_field != NULL);
    managed_type = feng_symbol_decl_value_type(managed_field);
    ASSERT(managed_type != NULL);
    ASSERT(feng_symbol_type_kind(managed_type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_target_decl(managed_type) == hidden_managed);
    managed_arg = feng_symbol_type_generic_arg_at(managed_type, 0U);
    ASSERT(managed_arg != NULL);
    ASSERT(feng_symbol_type_kind(managed_arg) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(feng_symbol_type_target_decl(managed_arg) == public_type_param);

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    ASSERT(semantic_module->program_count == 1U);
    ASSERT(semantic_module->programs[0]->declaration_count ==
           feng_symbol_module_decl_count(module));
    {
        bool found_private = false;

        for (index = 0U;
             index < semantic_module->programs[0]->declaration_count;
             ++index) {
            const FengDecl *decl = semantic_module->programs[0]->declarations[index];

            if (decl->kind == FENG_DECL_TYPE &&
                slice_equals_cstr(decl->as.type_decl.name, "HiddenManaged")) {
                ASSERT(decl->visibility == FENG_VISIBILITY_PRIVATE);
                found_private = true;
            }
        }
        ASSERT(found_private);
    }

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* A provider visibility error leaves no semantic analysis to export and no
 * package-public .ft may be created. */
static void test_signature_visibility_error_prevents_public_ft_export(void) {
    static const char *kSource =
        "open module feng.test.symbol.visibility_error;\n"
        "type Hidden {}\n"
        "open let value: Hidden;\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char ft_path[1024];
    FengProgram *program = parse_or_die("visibility_error.ff", kSource);
    const FengProgram *programs[] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSymbolExportOptions options = {0};
    FengSymbolError error = {0};

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    ASSERT(snprintf(ft_path,
                    sizeof(ft_path),
                    "%s/feng/test/symbol/visibility_error.ft",
                    public_root) > 0);
    ASSERT(!feng_semantic_analyze(programs,
                                  1U,
                                  FENG_COMPILE_TARGET_LIB,
                                  &analysis,
                                  &errors,
                                  &error_count));
    ASSERT(analysis == NULL);
    ASSERT(error_count >= 1U);
    ASSERT(strcmp(errors[0].code, "AE0327") == 0);

    options.public_root = public_root;
    ASSERT(!feng_symbol_export_analysis(analysis, &options, &error));
    ASSERT(access(ft_path, F_OK) != 0);

    feng_symbol_error_free(&error);
    feng_semantic_errors_free(errors, error_count);
    feng_program_free(program);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

int main(void) {
    (void)system("rm -rf temp");
    (void)mkdir("temp", 0755);

    test_roundtrip_public_module();
    test_union_spec_ft_roundtrip_preserves_normalized_members();
    test_intersection_spec_ft_roundtrip_preserves_members();
    test_bounded_decl_ft_roundtrip_uses_inferred_initializer();
    test_roundtrip_public_module_docs();
    test_private_module_skipped();
    test_reader_rejects_bad_magic();
    test_provider_loads_bundle_public_module();
    test_enum_ft_roundtrip_exports_items_and_values();
    test_provider_rejects_duplicate_bundle_module();
    test_provider_rejects_bad_bundle_symbol_entry();
    test_imported_module_cache_keeps_synthesized_modules_alive();
    test_imported_module_cache_preserves_extern_c_symbol_name();
    test_imported_module_cache_preserves_enum_items();
    test_imported_enum_value_participates_in_semantic_analysis();
    test_imported_module_cache_keeps_bundle_fit_modules_alive();
    test_imported_module_cache_keeps_multi_file_bundle_fit_modules_alive();
    test_generic_function_ft_roundtrip();
    test_generic_type_ft_roundtrip();
    test_inferred_generic_field_ft_roundtrip();
    test_generic_fit_ft_roundtrip();
    test_generic_fit_named_generic_return_ft_roundtrip();
    test_fit_builtin_and_array_target_nodes_ft_roundtrip();
    test_fit_array_type_param_target_ft_roundtrip();
    test_private_representation_dependency_closure_roundtrip();
    test_signature_visibility_error_prevents_public_ft_export();
    fprintf(stdout, "symbol tests passed\n");
    return 0;
}
