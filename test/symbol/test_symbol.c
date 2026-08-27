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
#include "symbol/internal.h"
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

/* Object-spec member visibility survives the existing FT encoding and the
 * synthesized imported AST without exposing seal members as public members. */
static void test_object_spec_seal_member_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.spec_seal_roundtrip;\n"
        "open spec Hooks {\n"
        "    let publicField: int;\n"
        "    seal let hiddenField: int;\n"
        "    func publicMethod(): int;\n"
        "    seal static func hiddenMethod(): int;\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *hooks = NULL;
    const FengSemanticModule *semantic_module = NULL;
    const FengDecl *synth_hooks = NULL;
    size_t public_count = 0U;
    size_t seal_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("spec_seal_roundtrip.ff", kSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("spec_seal_roundtrip");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    hooks = feng_symbol_module_find_public_spec(module,
                                                slice_from_cstr("Hooks"));
    ASSERT(hooks != NULL);
    ASSERT(feng_symbol_decl_member_count(hooks) == 4U);
    ASSERT(feng_symbol_decl_find_public_member(
               hooks, slice_from_cstr("publicField")) != NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               hooks, slice_from_cstr("publicMethod")) != NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               hooks, slice_from_cstr("hiddenField")) == NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               hooks, slice_from_cstr("hiddenMethod")) == NULL);
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(hooks);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(hooks, index);

        ASSERT(member != NULL);
        if (feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE) {
            ++seal_count;
        } else {
            ++public_count;
        }
    }
    ASSERT(public_count == 2U);
    ASSERT(seal_count == 2U);

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    for (size_t index = 0U;
         index < semantic_module->programs[0]->declaration_count;
         ++index) {
        const FengDecl *decl =
            semantic_module->programs[0]->declarations[index];

        if (decl != NULL && decl->kind == FENG_DECL_SPEC &&
            slice_equals_cstr(decl->as.spec_decl.name, "Hooks")) {
            synth_hooks = decl;
            break;
        }
    }
    ASSERT(synth_hooks != NULL);
    ASSERT(synth_hooks->as.spec_decl.as.object.member_count == 4U);
    public_count = 0U;
    seal_count = 0U;
    for (size_t index = 0U;
         index < synth_hooks->as.spec_decl.as.object.member_count;
         ++index) {
        const FengTypeMember *member =
            synth_hooks->as.spec_decl.as.object.members[index];

        ASSERT(member != NULL);
        if (member->visibility == FENG_VISIBILITY_PRIVATE) {
            ++seal_count;
        } else {
            ++public_count;
        }
    }
    ASSERT(public_count == 2U);
    ASSERT(seal_count == 2U);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Package-public FT includes only seal methods selected by exported nominal
 * spec relationships. Their language visibility remains seal, and unrelated
 * seal methods stay absent for both direct type and fit implementations. */
static void test_selected_seal_spec_implementations_enter_package_ft(void) {
    static const char *kSource =
        "open module feng.test.symbol.spec_impl_dependency;\n"
        "open spec Contract {\n"
        "    seal func value(): int;\n"
        "    seal static func marker(): int;\n"
        "}\n"
        "open type Direct: Contract {\n"
        "    seal func value(): int { return 1; }\n"
        "    seal static func marker(): int { return 2; }\n"
        "    seal func unrelated(): int { return 3; }\n"
        "}\n"
        "open type FitValue {}\n"
        "open fit FitValue: Contract {\n"
        "    seal func value(): int { return 4; }\n"
        "    seal static func marker(): int { return 5; }\n"
        "    seal func unrelatedFit(): int { return 6; }\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module;
    const FengSymbolDeclView *direct;
    const FengSymbolDeclView *fit_decl;
    size_t direct_selected_count = 0U;
    size_t fit_selected_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die(
        "spec_impl_dependency.ff", kSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));
    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("spec_impl_dependency");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    direct = feng_symbol_module_find_public_type(
        module, slice_from_cstr("Direct"));
    ASSERT(direct != NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               direct, slice_from_cstr("value")) == NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               direct, slice_from_cstr("marker")) == NULL);
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(direct);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(direct, index);
        FengSlice name = feng_symbol_decl_name(member);

        ASSERT(!slice_equals_cstr(name, "unrelated"));
        if (slice_equals_cstr(name, "value") ||
            slice_equals_cstr(name, "marker")) {
            ++direct_selected_count;
            ASSERT(feng_symbol_decl_kind(member) ==
                   FENG_SYMBOL_DECL_KIND_METHOD);
            ASSERT(feng_symbol_decl_visibility(member) ==
                   FENG_VISIBILITY_PRIVATE);
        }
    }
    ASSERT(direct_selected_count == 2U);

    ASSERT(feng_symbol_module_fit_count(module) == 1U);
    fit_decl = feng_symbol_fit_decl(feng_symbol_module_fit_at(module, 0U));
    ASSERT(fit_decl != NULL);
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(fit_decl);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(fit_decl, index);
        FengSlice name = feng_symbol_decl_name(member);

        ASSERT(!slice_equals_cstr(name, "unrelatedFit"));
        if (slice_equals_cstr(name, "value") ||
            slice_equals_cstr(name, "marker")) {
            ++fit_selected_count;
            ASSERT(feng_symbol_decl_kind(member) ==
                   FENG_SYMBOL_DECL_KIND_METHOD);
            ASSERT(feng_symbol_decl_visibility(member) ==
                   FENG_VISIBILITY_PRIVATE);
        }
    }
    ASSERT(fit_selected_count == 2U);

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* The query adapter reuses one already-built Symbol graph and exposes the
 * writer's exact source-node closure, including transitive callable deps. */
static void test_package_selection_uses_existing_graph(void) {
    static const char *kSource =
        "open module feng.test.symbol.package_selection;\n"
        "open spec Producer<T>(): T;\n"
        "open spec Contract<T> { seal func value(): T; }\n"
        "open type Generic<T>: Contract<T> {\n"
        "    seal func dependency(): T { let result: T; return result; }\n"
        "    seal func value(): T {\n"
        "        let producer: Producer<T> = self.dependency;\n"
        "        return producer();\n"
        "    }\n"
        "    seal func unrelated(): T { let result: T; return result; }\n"
        "}\n";
    FengProgram *program = parse_or_die("package_selection.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolGraph *graph = NULL;
    FengSymbolPackageSelection *selection = NULL;
    FengSymbolError error = {0};
    const FengDecl *generic_decl = NULL;
    const FengTypeMember *dependency = NULL;
    const FengTypeMember *value = NULL;
    const FengTypeMember *unrelated = NULL;
    size_t decl_index;

    for (decl_index = 0U;
         decl_index < program->declaration_count;
         ++decl_index) {
        const FengDecl *candidate = program->declarations[decl_index];

        if (candidate->kind == FENG_DECL_TYPE &&
            slice_equals_cstr(candidate->as.type_decl.name, "Generic")) {
            generic_decl = candidate;
            break;
        }
    }
    ASSERT(generic_decl != NULL);
    for (size_t member_index = 0U;
         member_index < generic_decl->as.type_decl.member_count;
         ++member_index) {
        const FengTypeMember *member =
            generic_decl->as.type_decl.members[member_index];

        if (member->kind != FENG_TYPE_MEMBER_METHOD) {
            continue;
        }
        if (slice_equals_cstr(member->as.callable.name, "dependency")) {
            dependency = member;
        } else if (slice_equals_cstr(member->as.callable.name, "value")) {
            value = member;
        } else if (slice_equals_cstr(member->as.callable.name, "unrelated")) {
            unrelated = member;
        }
    }
    ASSERT(dependency != NULL);
    ASSERT(value != NULL);
    ASSERT(unrelated != NULL);
    ASSERT(feng_symbol_build_graph(analysis, &graph, &error));
    ASSERT(feng_symbol_build_package_selection(graph, &selection, &error));
    ASSERT(feng_symbol_package_selection_contains(selection, generic_decl));
    ASSERT(feng_symbol_package_selection_contains(selection, value));
    ASSERT(feng_symbol_package_selection_contains(selection, dependency));
    ASSERT(!feng_symbol_package_selection_contains(selection, unrelated));

    feng_symbol_package_selection_free(selection);
    feng_symbol_graph_free(graph);
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Static fields selected as seal spec implementations keep seal visibility
 * in package-public FT; compiler codegen reuse must not make them ordinary
 * public members. */
static void test_selected_seal_static_fields_remain_seal_in_package_ft(void) {
    static const char *kSource =
        "open module feng.test.symbol.spec_seal_static_field;\n"
        "open spec State {\n"
        "    seal static let initial: int;\n"
        "    seal static var current: int;\n"
        "}\n"
        "open type Store: State {\n"
        "    seal static let initial: int = 1;\n"
        "    seal static var current: int = 2;\n"
        "    seal static let unrelated: int = 3;\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module;
    const FengSymbolDeclView *store;
    size_t seal_static_field_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die(
        "spec_seal_static_field.ff", kSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));
    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("spec_seal_static_field");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    store = feng_symbol_module_find_public_type(
        module, slice_from_cstr("Store"));
    ASSERT(store != NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               store, slice_from_cstr("initial")) == NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               store, slice_from_cstr("current")) == NULL);
    ASSERT(feng_symbol_decl_find_public_member(
               store, slice_from_cstr("unrelated")) == NULL);
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(store);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(store, index);

        if (member != NULL &&
            feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD) {
            ++seal_static_field_count;
            ASSERT(feng_symbol_decl_is_static(member));
            ASSERT(feng_symbol_decl_visibility(member) ==
                   FENG_VISIBILITY_PRIVATE);
        }
    }
    ASSERT(seal_static_field_count == 3U);

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* A spec seal restored from package-public FT grants access to an importing
 * type implementation, but remains inaccessible to an importing top-level
 * function. The implementation member itself stays local to the consumer. */
static void test_imported_object_spec_seal_access_semantics(void) {
    static const char *kExternalSource =
        "open module vendor.hooks;\n"
        "open spec Hooks { seal func hidden(): int; }\n";
    static const char *kAllowedSource =
        "module demo.allowed;\n"
        "import vendor.hooks;\n"
        "type Worker: vendor.hooks.Hooks {\n"
        "    func hidden(): int { return 1; }\n"
        "    func use(value: vendor.hooks.Hooks): int { return value.hidden(); }\n"
        "}\n";
    static const char *kDeniedSource =
        "module demo.denied;\n"
        "import vendor.hooks;\n"
        "func use(value: vendor.hooks.Hooks): int { return value.hidden(); }\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options = {0};
    FengSymbolError symbol_error = {0};
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("external_hooks.ff", kExternalSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = feng_get_host_pointer_size();

    program = parse_or_die("imported_spec_seal_allowed.ff", kAllowedSource);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    program = parse_or_die("imported_spec_seal_denied.ff", kDeniedSource);
    programs[0] = program;
    analysis = NULL;
    errors = NULL;
    error_count = 0U;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].code, "AE0708") == 0);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* @friend is compile-time package-local metadata: FT retains the underlying
 * spec seal member but neither its synthesized AST nor a consumer fit can
 * recover the friend authorization. */
static void test_friend_metadata_is_not_exported_to_ft(void) {
    static const char *kExternalSource =
        "open module vendor.friend_surface;\n"
        "open type Helper {}\n"
        "open spec Secret {\n"
        "    @friend(Helper) seal func hidden(): int;\n"
        "}\n";
    static const char *kConsumerSource =
        "module demo.friend_consumer;\n"
        "import vendor.friend_surface;\n"
        "fit vendor.friend_surface.Helper {\n"
        "    func read(value: vendor.friend_surface.Secret): int {\n"
        "        return value.hidden();\n"
        "    }\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options = {0};
    FengSymbolError symbol_error = {0};
    FengSlice segments[2];
    const FengSemanticModule *semantic_module;
    const FengDecl *secret_decl = NULL;
    FengProgram *consumer;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("friend_surface.ff",
                                kExternalSource,
                                public_root);
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    segments[0] = slice_from_cstr("vendor");
    segments[1] = slice_from_cstr("friend_surface");
    semantic_module = query.get_module(query.user, segments, 2U);
    ASSERT(semantic_module != NULL);
    for (size_t index = 0U;
         index < semantic_module->programs[0]->declaration_count;
         ++index) {
        const FengDecl *decl = semantic_module->programs[0]->declarations[index];

        if (decl->kind == FENG_DECL_SPEC &&
            slice_equals_cstr(decl->as.spec_decl.name, "Secret")) {
            secret_decl = decl;
            break;
        }
    }
    ASSERT(secret_decl != NULL);
    ASSERT(secret_decl->as.spec_decl.as.object.member_count == 1U);
    ASSERT(secret_decl->as.spec_decl.as.object.members[0]->annotation_count == 0U);

    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = feng_get_host_pointer_size();
    consumer = parse_or_die("friend_consumer.ff", kConsumerSource);
    programs[0] = consumer;
    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               &options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].code, "AE0708") == 0);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(consumer);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* The owner package may name an imported concrete friend type and define its
 * own fit for that type. Authorization remains local because only the owner
 * analysis contains the @friend sidecar. */
static void test_local_friend_fit_can_target_imported_type(void) {
    static const char *kExternalSource =
        "open module vendor.friend_helper;\n"
        "open type Helper {}\n";
    static const char *kOwnerSource =
        "module demo.friend_owner;\n"
        "import vendor.friend_helper;\n"
        "type Vault {\n"
        "    @friend(vendor.friend_helper.Helper)\n"
        "    seal let secret: int = 7;\n"
        "}\n"
        "fit vendor.friend_helper.Helper {\n"
        "    func read(vault: Vault): int { return vault.secret; }\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options = {0};
    FengSymbolError symbol_error = {0};
    FengProgram *owner;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("friend_helper.ff",
                                kExternalSource,
                                public_root);
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = feng_get_host_pointer_size();
    owner = parse_or_die("friend_owner.ff", kOwnerSource);
    programs[0] = owner;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(error_count == 0U);

    feng_semantic_analysis_free(analysis);
    feng_program_free(owner);
    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* A consumer package must not forge a local spec and use a fit to turn
 * package-public private layout skeletons into witness implementations. */
static void test_imported_type_seal_members_do_not_satisfy_consumer_fit(void) {
    static const char *kExternalSource =
        "open module vendor.vault;\n"
        "open type Vault {\n"
        "    seal let hiddenField: int = 1;\n"
        "    seal static let hiddenStaticField: int = 2;\n"
        "    seal func hiddenMethod(): int { return 3; }\n"
        "    seal static func hiddenStaticMethod(): int { return 4; }\n"
        "}\n";
    static const char *kAllowedSource =
        "module consumer.fit_implementation;\n"
        "import vendor.vault;\n"
        "spec Adapted { seal func adapted(): int; }\n"
        "fit Vault: Adapted { seal func adapted(): int { return 5; } }\n"
        "func demandWitness(value: Vault): Adapted { return value; }\n";
    static const struct {
        const char *path;
        const char *source;
        const char *expected_code;
    } kRejected[] = {
        {
            "consumer_forged_seal_field.ff",
            "module consumer.forged_field;\n"
            "import vendor.vault;\n"
            "spec Forged { seal let hiddenField: int; }\n"
            "fit Vault: Forged;\n",
            "AE0701"
        },
        {
            "consumer_forged_seal_static_field.ff",
            "module consumer.forged_static_field;\n"
            "import vendor.vault;\n"
            "spec Forged { seal static let hiddenStaticField: int; }\n"
            "fit Vault: Forged;\n",
            "AE0701"
        },
        {
            "consumer_forged_seal_method.ff",
            "module consumer.forged_method;\n"
            "import vendor.vault;\n"
            "spec Forged { seal func hiddenMethod(): int; }\n"
            "fit Vault: Forged;\n",
            "AE0705"
        },
        {
            "consumer_forged_seal_static_method.ff",
            "module consumer.forged_static_method;\n"
            "import vendor.vault;\n"
            "spec Forged { seal static func hiddenStaticMethod(): int; }\n"
            "fit Vault: Forged;\n",
            "AE0705"
        }
    };
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options = {0};
    FengSymbolError symbol_error = {0};
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    size_t index;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("external_vault.ff", kExternalSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = feng_get_host_pointer_size();

    program = parse_or_die("consumer_fit_implementation.ff", kAllowedSource);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(error_count == 0U);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    for (index = 0U; index < sizeof(kRejected) / sizeof(kRejected[0]); ++index) {
        program = parse_or_die(kRejected[index].path, kRejected[index].source);
        programs[0] = program;
        analysis = NULL;
        errors = NULL;
        error_count = 0U;
        ASSERT(!feng_semantic_analyze_with_options(programs,
                                                   1U,
                                                   &options,
                                                   &analysis,
                                                   &errors,
                                                   &error_count));
        ASSERT(error_count == 1U);
        ASSERT(strcmp(errors[0].code, kRejected[index].expected_code) == 0);
        feng_semantic_errors_free(errors, error_count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
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

/* Analyze one consumer against declarations restored from package-public FT
 * and require the expected immutable-member diagnostic. */
static void assert_ft_let_consumer_rejected(
    const FengSemanticAnalyzeOptions *options,
    const char *path,
    const char *source,
    const char *expected_code,
    const char *expected_message_fragment) {
    FengProgram *program = parse_or_die(path, source);
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, path) == 0);
    ASSERT(strcmp(errors[0].code, expected_code) == 0);
    ASSERT(strstr(errors[0].message, expected_message_fragment) != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Package-public FT must preserve every fact needed to enforce let binding
 * across all construction phases and after imported construction completes. */
static void test_let_three_phase_binding_semantics_survive_ft_roundtrip(void) {
    static const char *kProviderSource =
        "open module vendor.g05_let;\n"
        "open type DeclarationBoundLet {\n"
        "    open let value: int = 101;\n"
        "}\n"
        "open type ConstructorBoundLet {\n"
        "    open let value: int;\n"
        "    open func ConstructorBoundLet() { self.value = 202; }\n"
        "}\n"
        "open type LiteralBoundLet {\n"
        "    open let value: int;\n"
        "}\n"
        "open type DefaultUnboundLet {\n"
        "    open let value: int;\n"
        "}\n";
    static const char *kAllowedSource =
        "module consumer.g05_let_allowed;\n"
        "import vendor.g05_let as provider;\n"
        "func read(): int {\n"
        "    let declaration = provider.DeclarationBoundLet();\n"
        "    let constructor = provider.ConstructorBoundLet();\n"
        "    let literal = provider.LiteralBoundLet { value: 303 };\n"
        "    let zero = provider.DefaultUnboundLet();\n"
        "    return declaration.value + constructor.value +\n"
        "           literal.value + zero.value;\n"
        "}\n";
    /* Rejected consumers cover phase-to-phase rebinding and assignments after
     * each possible final-binding state, including the unbound zero state. */
    static const struct {
        const char *path;
        const char *source;
        const char *expected_code;
        const char *expected_message_fragment;
    } kRejected[] = {
        {
            "ft_let_decl_literal_error.ff",
            "module consumer.g05_decl_literal;\n"
            "import vendor.g05_let as provider;\n"
            "func make(): provider.DeclarationBoundLet {\n"
            "    return provider.DeclarationBoundLet { value: 1 };\n"
            "}\n",
            "AE0102",
            "declaration initializer",
        },
        {
            "ft_let_ctor_literal_error.ff",
            "module consumer.g05_ctor_literal;\n"
            "import vendor.g05_let as provider;\n"
            "func make(): provider.ConstructorBoundLet {\n"
            "    return provider.ConstructorBoundLet { value: 1 };\n"
            "}\n",
            "AE0102",
            "already completed by constructor",
        },
        {
            "ft_let_post_decl_error.ff",
            "module consumer.g05_post_decl;\n"
            "import vendor.g05_let as provider;\n"
            "func reject() {\n"
            "    var value = provider.DeclarationBoundLet();\n"
            "    value.value = 1;\n"
            "}\n",
            "AE0104",
            "is not writable",
        },
        {
            "ft_let_post_ctor_error.ff",
            "module consumer.g05_post_ctor;\n"
            "import vendor.g05_let as provider;\n"
            "func reject() {\n"
            "    var value = provider.ConstructorBoundLet();\n"
            "    value.value = 1;\n"
            "}\n",
            "AE0104",
            "is not writable",
        },
        {
            "ft_let_post_literal_error.ff",
            "module consumer.g05_post_literal;\n"
            "import vendor.g05_let as provider;\n"
            "func reject() {\n"
            "    var value = provider.LiteralBoundLet { value: 1 };\n"
            "    value.value = 2;\n"
            "}\n",
            "AE0104",
            "is not writable",
        },
        {
            "ft_let_post_default_error.ff",
            "module consumer.g05_post_default;\n"
            "import vendor.g05_let as provider;\n"
            "func reject() {\n"
            "    var value = provider.DefaultUnboundLet();\n"
            "    value.value = 1;\n"
            "}\n",
            "AE0104",
            "is not writable",
        },
    };
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options = {0};
    FengSymbolError symbol_error = {0};
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("g05_let_provider.ff",
                                kProviderSource,
                                public_root);
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));
    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = feng_get_host_pointer_size();

    program = parse_or_die("ft_let_allowed.ff", kAllowedSource);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    for (size_t index = 0U;
         index < sizeof(kRejected) / sizeof(kRejected[0]);
         ++index) {
        assert_ft_let_consumer_rejected(
            &options,
            kRejected[index].path,
            kRejected[index].source,
            kRejected[index].expected_code,
            kRejected[index].expected_message_fragment);
    }

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Analyze one constructor-availability consumer against package-public FT and
 * require the exact semantic diagnostic selected after import. */
static void assert_ft_constructor_consumer_rejected(
    const FengSemanticAnalyzeOptions *options,
    const char *path,
    const char *source,
    const char *expected_code,
    const char *expected_message_fragment) {
    FengProgram *program = parse_or_die(path, source);
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(!feng_semantic_analyze_with_options(programs,
                                               1U,
                                               options,
                                               &analysis,
                                               &errors,
                                               &error_count));
    ASSERT(error_count == 1U);
    ASSERT(strcmp(errors[0].path, path) == 0);
    ASSERT(strcmp(errors[0].code, expected_code) == 0);
    ASSERT(strstr(errors[0].message, expected_message_fragment) != NULL);

    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* CTOR13-CTOR19: package-public FT must retain the complete constructor set
 * while exposing only constructors that are public and signature-compatible. */
static void test_constructor_availability_survives_ft_roundtrip(void) {
    static const char *kProviderSource =
        "open module vendor.g05_constructor;\n"
        "open type ImplicitDefault { open var value: int; }\n"
        "open type PublicSurface {\n"
        "    open var value: int;\n"
        "    open func PublicSurface() { self.value = 10; }\n"
        "    open func PublicSurface(value: int) { self.value = value; }\n"
        "}\n"
        "open type PublicArgsOnly {\n"
        "    open var value: int;\n"
        "    open func PublicArgsOnly(value: int) { self.value = value; }\n"
        "}\n"
        "open type SealedOnly {\n"
        "    open var value: int;\n"
        "    seal func SealedOnly() { self.value = 20; }\n"
        "    seal func SealedOnly(value: int) { self.value = value; }\n"
        "    open static func create(value: int): SealedOnly { return SealedOnly(value); }\n"
        "    open func copy(): SealedOnly { return SealedOnly(self.value); }\n"
        "}\n"
        "open type SealedArgsOnly {\n"
        "    open var value: int;\n"
        "    seal func SealedArgsOnly(value: int) { self.value = value; }\n"
        "    open static func create(value: int): SealedArgsOnly { return SealedArgsOnly(value); }\n"
        "}\n"
        "open type MixedConstructors {\n"
        "    open var value: int;\n"
        "    open func MixedConstructors(value: int) { self.value = value; }\n"
        "    seal func MixedConstructors(value: string) { self.value = 30; }\n"
        "}\n";
    static const char *kAllowedSource =
        "module consumer.g05_constructor_allowed;\n"
        "import vendor.g05_constructor as provider;\n"
        "func read(): int {\n"
        "    let i0 = provider.ImplicitDefault();\n"
        "    let i1 = provider.ImplicitDefault() {};\n"
        "    let i2 = provider.ImplicitDefault() { value: 1 };\n"
        "    let i3 = provider.ImplicitDefault {};\n"
        "    let i4 = provider.ImplicitDefault { value: 2 };\n"
        "    let p0 = provider.PublicSurface();\n"
        "    let p1 = provider.PublicSurface() { value: 3 };\n"
        "    let p2 = provider.PublicSurface { value: 4 };\n"
        "    let p3 = provider.PublicSurface(5);\n"
        "    let p4 = provider.PublicSurface(6) { value: 7 };\n"
        "    let sealed = provider.SealedOnly.create(8);\n"
        "    let copied = sealed.copy();\n"
        "    let mixed = provider.MixedConstructors(9);\n"
        "    return i0.value + i1.value + i2.value + i3.value + i4.value +\n"
        "           p0.value + p1.value + p2.value + p3.value + p4.value +\n"
        "           sealed.value + copied.value + mixed.value;\n"
        "}\n";
    static const struct {
        const char *path;
        const char *source;
        const char *expected_code;
        const char *expected_message_fragment;
    } kRejected[] = {
        {
            "ft_ctor16_direct_arg.ff",
            "module consumer.g05_ctor16_direct;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.ImplicitDefault(1); }\n",
            "AE0313",
            "has no constructor accepting 1 argument(s)",
        },
        {
            "ft_ctor16_literal_arg.ff",
            "module consumer.g05_ctor16_literal;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.ImplicitDefault(1) {}; }\n",
            "AE0313",
            "has no constructor accepting 1 argument(s)",
        },
        {
            "ft_ctor17_direct_zero.ff",
            "module consumer.g05_ctor17_direct;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.PublicArgsOnly(); }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor17_literal_zero.ff",
            "module consumer.g05_ctor17_literal;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.PublicArgsOnly() {}; }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor17_shorthand_zero.ff",
            "module consumer.g05_ctor17_shorthand;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.PublicArgsOnly {}; }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor18_direct_zero.ff",
            "module consumer.g05_ctor18_direct_zero;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.SealedOnly(); }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor18_literal_zero.ff",
            "module consumer.g05_ctor18_literal_zero;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.SealedOnly() {}; }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor18_shorthand_zero.ff",
            "module consumer.g05_ctor18_shorthand_zero;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.SealedOnly {}; }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor18_direct_arg.ff",
            "module consumer.g05_ctor18_direct_arg;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.SealedOnly(1); }\n",
            "AE0315",
            "no accessible constructor accepting 1 argument(s)",
        },
        {
            "ft_ctor18_literal_arg.ff",
            "module consumer.g05_ctor18_literal_arg;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.SealedOnly(1) {}; }\n",
            "AE0315",
            "no accessible constructor accepting 1 argument(s)",
        },
        {
            "ft_ctor18_sealed_args_no_default.ff",
            "module consumer.g05_ctor18_args_only;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.SealedArgsOnly(); }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
        {
            "ft_ctor19_private_overload.ff",
            "module consumer.g05_ctor19_private;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.MixedConstructors(\"hidden\"); }\n",
            "AE0315",
            "no accessible constructor accepting 1 argument(s)",
        },
        {
            "ft_ctor19_no_default.ff",
            "module consumer.g05_ctor19_default;\n"
            "import vendor.g05_constructor as provider;\n"
            "func run() { provider.MixedConstructors(); }\n",
            "AE0315",
            "no accessible constructor accepting 0 argument(s)",
        },
    };
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query;
    FengSemanticAnalyzeOptions options = {0};
    FengSymbolError symbol_error = {0};
    FengSlice module_segments[2];
    const FengSymbolImportedModule *module;
    const FengSymbolDeclView *implicit_default;
    const FengSymbolDeclView *public_args_only;
    const FengSymbolDeclView *sealed_only;
    const FengSymbolDeclView *mixed;
    FengProgram *program = NULL;
    const FengProgram *programs[1];
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    size_t constructor_count;
    size_t public_constructor_count;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("g05_constructor_provider.ff",
                                kProviderSource,
                                public_root);
    ASSERT(feng_symbol_provider_create(&provider, &symbol_error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &symbol_error));

    module_segments[0] = slice_from_cstr("vendor");
    module_segments[1] = slice_from_cstr("g05_constructor");
    module = feng_symbol_provider_find_module(provider, module_segments, 2U);
    ASSERT(module != NULL);
    implicit_default = feng_symbol_module_find_public_type(
        module, slice_from_cstr("ImplicitDefault"));
    public_args_only = feng_symbol_module_find_public_type(
        module, slice_from_cstr("PublicArgsOnly"));
    sealed_only = feng_symbol_module_find_public_type(
        module, slice_from_cstr("SealedOnly"));
    mixed = feng_symbol_module_find_public_type(
        module, slice_from_cstr("MixedConstructors"));
    ASSERT(implicit_default != NULL);
    ASSERT(public_args_only != NULL);
    ASSERT(sealed_only != NULL);
    ASSERT(mixed != NULL);

    constructor_count = 0U;
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(implicit_default);
         ++index) {
        if (feng_symbol_decl_kind(
                feng_symbol_decl_member_at(implicit_default, index)) ==
            FENG_SYMBOL_DECL_KIND_CONSTRUCTOR) {
            ++constructor_count;
        }
    }
    ASSERT(constructor_count == 0U);

    constructor_count = 0U;
    public_constructor_count = 0U;
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(public_args_only);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(public_args_only, index);
        if (feng_symbol_decl_kind(member) != FENG_SYMBOL_DECL_KIND_CONSTRUCTOR) {
            continue;
        }
        ++constructor_count;
        if (feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PUBLIC) {
            ++public_constructor_count;
        }
    }
    ASSERT(constructor_count == 1U);
    ASSERT(public_constructor_count == 1U);

    constructor_count = 0U;
    public_constructor_count = 0U;
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(sealed_only);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(sealed_only, index);
        if (feng_symbol_decl_kind(member) != FENG_SYMBOL_DECL_KIND_CONSTRUCTOR) {
            continue;
        }
        ++constructor_count;
        if (feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PUBLIC) {
            ++public_constructor_count;
        }
    }
    ASSERT(constructor_count == 2U);
    ASSERT(public_constructor_count == 0U);
    ASSERT(feng_symbol_decl_public_member_count(
               sealed_only, slice_from_cstr("SealedOnly")) == 0U);
    ASSERT(feng_symbol_decl_find_public_member(
               sealed_only, slice_from_cstr("SealedOnly")) == NULL);

    constructor_count = 0U;
    public_constructor_count = 0U;
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(mixed);
         ++index) {
        const FengSymbolDeclView *member = feng_symbol_decl_member_at(mixed, index);
        if (feng_symbol_decl_kind(member) != FENG_SYMBOL_DECL_KIND_CONSTRUCTOR) {
            continue;
        }
        ++constructor_count;
        if (feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PUBLIC) {
            ++public_constructor_count;
        }
    }
    ASSERT(constructor_count == 2U);
    ASSERT(public_constructor_count == 1U);
    ASSERT(feng_symbol_decl_public_member_count(
               mixed, slice_from_cstr("MixedConstructors")) == 1U);
    ASSERT(feng_symbol_decl_kind(feng_symbol_decl_find_public_member(
               mixed, slice_from_cstr("MixedConstructors"))) ==
           FENG_SYMBOL_DECL_KIND_CONSTRUCTOR);

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    options.target = FENG_COMPILE_TARGET_LIB;
    options.imported_modules = &query;
    options.pointer_size = feng_get_host_pointer_size();

    program = parse_or_die("ft_ctor_allowed.ff", kAllowedSource);
    programs[0] = program;
    ASSERT(feng_semantic_analyze_with_options(programs,
                                              1U,
                                              &options,
                                              &analysis,
                                              &errors,
                                              &error_count));
    ASSERT(errors == NULL);
    ASSERT(error_count == 0U);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    for (size_t index = 0U;
         index < sizeof(kRejected) / sizeof(kRejected[0]);
         ++index) {
        assert_ft_constructor_consumer_rejected(
            &options,
            kRejected[index].path,
            kRejected[index].source,
            kRejected[index].expected_code,
            kRejected[index].expected_message_fragment);
    }

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&symbol_error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Verifies generated mixin members are exported as ordinary target members
 * while the propagated static wrapper preserves its mixable declaration fact. */
static void test_mixin_generated_members_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.mixin_roundtrip;\n"
        "open spec Widget {\n"
        "    func draw(area: int): int;\n"
        "}\n"
        "open type View: Widget {\n"
        "    open let id: int = 1;\n"
        "    @mixable\n"
        "    open static func draw(target: Widget, area: int): int {\n"
        "        return area;\n"
        "    }\n"
        "}\n"
        "open type Button: Widget {\n"
        "    ...: View = View();\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module = NULL;
    const FengSymbolDeclView *button = NULL;
    const FengSymbolDeclView *id_field = NULL;
    const FengSemanticModule *semantic_module = NULL;
    const FengDecl *semantic_button = NULL;
    size_t static_draw_count = 0U;
    size_t instance_draw_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die("mixin_roundtrip.ff", kSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("mixin_roundtrip");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    button = feng_symbol_module_find_public_type(module, slice_from_cstr("Button"));
    ASSERT(button != NULL);
    id_field = feng_symbol_decl_find_public_member(button, slice_from_cstr("id"));
    ASSERT(id_field != NULL);
    ASSERT(feng_symbol_decl_has_bounded_decl(id_field));

    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(button);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(button, index);

        if (member == NULL ||
            feng_symbol_decl_kind(member) != FENG_SYMBOL_DECL_KIND_METHOD ||
            !slice_equals_cstr(feng_symbol_decl_name(member), "draw")) {
            continue;
        }
        if (feng_symbol_decl_is_static(member)) {
            ++static_draw_count;
            ASSERT(feng_symbol_decl_is_mixable(member));
        } else {
            ++instance_draw_count;
            ASSERT(!feng_symbol_decl_is_mixable(member));
        }
    }
    ASSERT(static_draw_count == 1U);
    ASSERT(instance_draw_count == 1U);

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    for (size_t index = 0U;
         index < semantic_module->programs[0]->declaration_count;
         ++index) {
        const FengDecl *decl =
            semantic_module->programs[0]->declarations[index];

        if (decl != NULL && decl->kind == FENG_DECL_TYPE &&
            slice_equals_cstr(decl->as.type_decl.name, "Button")) {
            semantic_button = decl;
            break;
        }
    }
    ASSERT(semantic_button != NULL);
    static_draw_count = 0U;
    instance_draw_count = 0U;
    for (size_t index = 0U;
         index < semantic_button->as.type_decl.member_count;
         ++index) {
        const FengTypeMember *member =
            semantic_button->as.type_decl.members[index];

        if (member == NULL || member->kind != FENG_TYPE_MEMBER_METHOD ||
            !slice_equals_cstr(member->as.callable.name, "draw")) {
            continue;
        }
        if (member->is_static) {
            ++static_draw_count;
            ASSERT(member->is_mixable);
        } else {
            ++instance_draw_count;
            ASSERT(!member->is_mixable);
        }
    }
    ASSERT(static_draw_count == 1U);
    ASSERT(instance_draw_count == 1U);

    feng_symbol_imported_module_cache_free(cache);
    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Package-public .ft records seal static method capabilities and all fields
 * needed for layout. Field visibility plus mixable facts survive exactly, so
 * an ordinary seal field remains distinguishable from a field capability. */
static void test_mixable_seal_member_ft_roundtrip_preserves_field_facts(void) {
    static const char *kSource =
        "open module feng.test.symbol.mixable_seal_roundtrip;\n"
        "open spec Widget {}\n"
        "open type View: Widget {\n"
        "    @mixable seal var state: int = 7;\n"
        "    seal var hiddenState: int = 9;\n"
        "    @mixable seal static func draw(target: Widget, value: int): int { return value; }\n"
        "    seal static func ordinary(target: Widget): int { return 1; }\n"
        "    seal func hiddenInstance(): int { return 2; }\n"
        "}\n"
        "open type Button: Widget { ...: View; }\n"
        "open type FitView: Widget {}\n"
        "open fit FitView {\n"
        "    @mixable seal static func fitDraw(target: Widget, value: int): int { return value + 1; }\n"
        "    seal static func fitOrdinary(target: Widget): int { return 3; }\n"
        "}\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSymbolError error = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *module;
    const FengSymbolDeclView *view;
    const FengSymbolDeclView *button;
    const FengSymbolDeclView *fit_decl;
    const FengSemanticModule *semantic_module;
    const FengDecl *semantic_view = NULL;
    const FengDecl *semantic_button = NULL;
    const FengDecl *semantic_fit = NULL;
    size_t view_draw_count = 0U;
    size_t button_draw_count = 0U;
    size_t fit_draw_count = 0U;
    size_t view_state_count = 0U;
    size_t view_hidden_state_count = 0U;
    size_t button_state_count = 0U;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/mod", tmp_dir) > 0);
    export_public_source_or_die(
        "mixable_seal_roundtrip.ff", kSource, public_root);
    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_ft_root(provider,
                                            public_root,
                                            FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                            &error));
    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("mixable_seal_roundtrip");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    view = feng_symbol_module_find_public_type(module, slice_from_cstr("View"));
    button = feng_symbol_module_find_public_type(module, slice_from_cstr("Button"));
    ASSERT(view != NULL && button != NULL);

    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(view);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(view, index);

        ASSERT(!slice_equals_cstr(feng_symbol_decl_name(member), "ordinary"));
        ASSERT(!slice_equals_cstr(feng_symbol_decl_name(member), "hiddenInstance"));
        if (slice_equals_cstr(feng_symbol_decl_name(member), "state")) {
            ++view_state_count;
            ASSERT(feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD);
            ASSERT(feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE);
            ASSERT(!feng_symbol_decl_is_static(member));
            ASSERT(feng_symbol_decl_is_mixable(member));
            continue;
        }
        if (slice_equals_cstr(feng_symbol_decl_name(member), "hiddenState")) {
            ++view_hidden_state_count;
            ASSERT(feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD);
            ASSERT(feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE);
            ASSERT(!feng_symbol_decl_is_static(member));
            ASSERT(!feng_symbol_decl_is_mixable(member));
            continue;
        }
        if (!slice_equals_cstr(feng_symbol_decl_name(member), "draw")) {
            continue;
        }
        ++view_draw_count;
        ASSERT(feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_METHOD);
        ASSERT(feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE);
        ASSERT(feng_symbol_decl_is_static(member));
        ASSERT(feng_symbol_decl_is_mixable(member));
    }
    ASSERT(view_draw_count == 1U);
    ASSERT(view_state_count == 1U);
    ASSERT(view_hidden_state_count == 1U);

    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(button);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(button, index);

        if (slice_equals_cstr(feng_symbol_decl_name(member), "state")) {
            ++button_state_count;
            ASSERT(feng_symbol_decl_kind(member) == FENG_SYMBOL_DECL_KIND_FIELD);
            ASSERT(feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE);
            ASSERT(!feng_symbol_decl_is_static(member));
            ASSERT(feng_symbol_decl_is_mixable(member));
            continue;
        }
        if (!slice_equals_cstr(feng_symbol_decl_name(member), "draw")) {
            continue;
        }
        ++button_draw_count;
        ASSERT(feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE);
        ASSERT(feng_symbol_decl_is_static(member));
        ASSERT(feng_symbol_decl_is_mixable(member));
    }
    ASSERT(button_draw_count == 1U);
    ASSERT(button_state_count == 1U);

    ASSERT(feng_symbol_module_fit_count(module) == 1U);
    fit_decl = feng_symbol_fit_decl(feng_symbol_module_fit_at(module, 0U));
    ASSERT(fit_decl != NULL);
    for (size_t index = 0U;
         index < feng_symbol_decl_member_count(fit_decl);
         ++index) {
        const FengSymbolDeclView *member =
            feng_symbol_decl_member_at(fit_decl, index);

        ASSERT(!slice_equals_cstr(feng_symbol_decl_name(member), "fitOrdinary"));
        if (!slice_equals_cstr(feng_symbol_decl_name(member), "fitDraw")) {
            continue;
        }
        ++fit_draw_count;
        ASSERT(feng_symbol_decl_visibility(member) == FENG_VISIBILITY_PRIVATE);
        ASSERT(feng_symbol_decl_is_static(member));
        ASSERT(feng_symbol_decl_is_mixable(member));
    }
    ASSERT(fit_draw_count == 1U);

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    for (size_t decl_index = 0U;
         decl_index < semantic_module->programs[0]->declaration_count;
         ++decl_index) {
        const FengDecl *decl =
            semantic_module->programs[0]->declarations[decl_index];

        if (decl->kind == FENG_DECL_TYPE &&
            slice_equals_cstr(decl->as.type_decl.name, "View")) {
            semantic_view = decl;
        } else if (decl->kind == FENG_DECL_TYPE &&
                   slice_equals_cstr(decl->as.type_decl.name, "Button")) {
            semantic_button = decl;
        } else if (decl->kind == FENG_DECL_FIT) {
            semantic_fit = decl;
        }
    }
    ASSERT(semantic_view != NULL && semantic_button != NULL && semantic_fit != NULL);
    view_draw_count = 0U;
    button_draw_count = 0U;
    fit_draw_count = 0U;
    view_state_count = 0U;
    view_hidden_state_count = 0U;
    button_state_count = 0U;
    for (size_t index = 0U;
         index < semantic_view->as.type_decl.member_count;
         ++index) {
        const FengTypeMember *member = semantic_view->as.type_decl.members[index];

        ASSERT(!slice_equals_cstr(member->kind == FENG_TYPE_MEMBER_FIELD
                                      ? member->as.field.name
                                      : member->as.callable.name,
                                  "ordinary"));
        if (member->kind == FENG_TYPE_MEMBER_FIELD &&
            slice_equals_cstr(member->as.field.name, "state")) {
            ++view_state_count;
            ASSERT(member->visibility == FENG_VISIBILITY_PRIVATE);
            ASSERT(!member->is_static && member->is_mixable);
            continue;
        }
        if (member->kind == FENG_TYPE_MEMBER_FIELD &&
            slice_equals_cstr(member->as.field.name, "hiddenState")) {
            ++view_hidden_state_count;
            ASSERT(member->visibility == FENG_VISIBILITY_PRIVATE);
            ASSERT(!member->is_static && !member->is_mixable);
            continue;
        }
        if (member->kind == FENG_TYPE_MEMBER_METHOD &&
            slice_equals_cstr(member->as.callable.name, "draw")) {
            ++view_draw_count;
            ASSERT(member->visibility == FENG_VISIBILITY_PRIVATE);
            ASSERT(member->is_static && member->is_mixable);
        }
    }
    for (size_t index = 0U;
         index < semantic_button->as.type_decl.member_count;
         ++index) {
        const FengTypeMember *member = semantic_button->as.type_decl.members[index];

        if (member->kind == FENG_TYPE_MEMBER_FIELD &&
            slice_equals_cstr(member->as.field.name, "state")) {
            ++button_state_count;
            ASSERT(member->visibility == FENG_VISIBILITY_PRIVATE);
            ASSERT(!member->is_static && member->is_mixable);
            continue;
        }
        if (member->kind == FENG_TYPE_MEMBER_METHOD &&
            slice_equals_cstr(member->as.callable.name, "draw")) {
            ++button_draw_count;
            ASSERT(member->visibility == FENG_VISIBILITY_PRIVATE);
            ASSERT(member->is_static && member->is_mixable);
        }
    }
    for (size_t index = 0U;
         index < semantic_fit->as.fit_decl.member_count;
         ++index) {
        const FengTypeMember *member = semantic_fit->as.fit_decl.members[index];

        if (member->kind == FENG_TYPE_MEMBER_METHOD &&
            slice_equals_cstr(member->as.callable.name, "fitDraw")) {
            ++fit_draw_count;
            ASSERT(member->visibility == FENG_VISIBILITY_PRIVATE);
            ASSERT(member->is_static && member->is_mixable);
        }
    }
    ASSERT(view_draw_count == 1U);
    ASSERT(button_draw_count == 1U);
    ASSERT(fit_draw_count == 1U);
    ASSERT(view_state_count == 1U);
    ASSERT(view_hidden_state_count == 1U);
    ASSERT(button_state_count == 1U);

    feng_symbol_imported_module_cache_free(cache);
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

static void test_c_variadic_fixed_param_count_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.c_variadic;\n"
        "@cdecl(\"c\", \"native_fixed\", 2)\n"
        "open extern func fixed(format: byte*, count: i32, value: f64): i32;\n"
        "@stdcall(\"c\", \"native_zero\", 0)\n"
        "open extern func zero(value: i32): i32;\n"
        "@fastcall(\"c\", \"native_omitted\")\n"
        "open extern func omitted(value: i32): i32;\n";
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    char public_ft[1024];
    char bundle_path[1024];
    FengSymbolProvider *provider = NULL;
    FengSymbolError error = {0};
    FengSymbolImportedModuleCache *cache = NULL;
    FengSemanticImportedModuleQuery query = {0};
    FengSlice segments[4];
    const FengSymbolImportedModule *symbol_module = NULL;
    const FengSymbolDeclView *fixed_view = NULL;
    const FengSymbolDeclView *zero_view = NULL;
    const FengSymbolDeclView *omitted_view = NULL;
    const FengSemanticModule *semantic_module = NULL;
    const FengProgram *program = NULL;

    ASSERT(snprintf(public_root, sizeof(public_root), "%s/c_variadic_mod", tmp_dir) > 0);
    ASSERT(snprintf(public_ft,
                    sizeof(public_ft),
                    "%s/feng/test/symbol/c_variadic.ft",
                    public_root) > 0);
    ASSERT(snprintf(bundle_path, sizeof(bundle_path), "%s/c_variadic.fb", tmp_dir) > 0);

    export_public_source_or_die("c_variadic.ff", kSource, public_root);
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/feng/test/symbol/c_variadic.ft",
                                  public_ft);
    ASSERT(feng_symbol_provider_create(&provider, &error));
    ASSERT(feng_symbol_provider_add_bundle(provider, bundle_path, &error));

    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("c_variadic");
    symbol_module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(symbol_module != NULL);
    fixed_view = feng_symbol_module_find_public_value(symbol_module,
                                                       slice_from_cstr("fixed"));
    zero_view = feng_symbol_module_find_public_value(symbol_module,
                                                      slice_from_cstr("zero"));
    omitted_view = feng_symbol_module_find_public_value(symbol_module,
                                                         slice_from_cstr("omitted"));
    ASSERT(fixed_view != NULL);
    ASSERT(zero_view != NULL);
    ASSERT(omitted_view != NULL);
    ASSERT(feng_symbol_decl_abi_fixed_param_count(fixed_view) == 2U);
    ASSERT(feng_symbol_decl_abi_fixed_param_count(zero_view) == 0U);
    ASSERT(feng_symbol_decl_abi_fixed_param_count(omitted_view) == 0U);

    cache = feng_symbol_imported_module_cache_create(provider);
    ASSERT(cache != NULL);
    query = feng_symbol_imported_module_cache_as_query(cache);
    semantic_module = query.get_module(query.user, segments, 4U);
    ASSERT(semantic_module != NULL);
    ASSERT(semantic_module->program_count == 1U);
    program = semantic_module->programs[0];
    ASSERT(program != NULL);
    ASSERT(program->declaration_count == 3U);
    for (size_t index = 0U; index < program->declaration_count; ++index) {
        const FengDecl *decl = program->declarations[index];
        const FengAnnotation *annotation;

        ASSERT(decl != NULL);
        ASSERT(decl->annotation_count == 1U);
        annotation = &decl->annotations[0];
        if (slice_equals_cstr(decl->as.function_decl.name, "fixed")) {
            ASSERT(annotation->arg_count == 3U);
            ASSERT(annotation->args[2]->kind == FENG_EXPR_INTEGER);
            ASSERT(annotation->args[2]->as.integer == 2);
        } else {
            ASSERT(annotation->arg_count == 2U);
        }
    }

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

/* Callable-value dependencies retain their source identity, explicit
 * callable-local arguments, owner instance and target surface through FT. */
static void test_callable_value_dependency_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.callable_value_dep;\n"
        "open spec Mapper<T>(value: T): T;\n"
        "open spec MapperAlt<T>(value: T): T;\n"
        "open spec Producer<T>(): T;\n"
        "open func identity<T>(value: T): T { return value; }\n"
        "open func make<T>(): Mapper<T> { return identity<T>; }\n"
        "open func surfaces<T>(): Mapper<T> {\n"
        "    let first: Mapper<T> = identity<T>;\n"
        "    let second: MapperAlt<T> = identity<T>;\n"
        "    second;\n"
        "    return first;\n"
        "}\n"
        "open type Reader<T> {\n"
        "    open let value: T;\n"
        "    open func read(): T { return self.value; }\n"
        "    open func reader(): Producer<T> { return self.read; }\n"
        "}\n"
        "open type MethodOwner {\n"
        "    open func identity<U>(value: U): U { return value; }\n"
        "    open func mapper<T>(): Mapper<T> { return self.identity<T>; }\n"
        "}\n"
        "open type FitOwner<T>(T, string);\n"
        "open fit FitOwner<T> {\n"
        "    open func identity<U>(value: U): U { return value; }\n"
        "}\n"
        "open func fitMapper<T, U>(owner: FitOwner<T>): Mapper<U> {\n"
        "    return owner.identity<U>;\n"
        "}\n";
    FengProgram *program = parse_or_die("callable_value_dep.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolDeclView *make_decl;
    const FengSymbolDeclView *reader_type;
    const FengSymbolDeclView *reader_method;
    const FengSymbolDeclView *method_owner_type;
    const FengSymbolDeclView *mapper_method;
    const FengSymbolDeclView *surfaces_decl;
    const FengSymbolDeclView *fit_mapper_decl;
    const FengSymbolCallableDepView *dependency;
    const FengSymbolTypeView *type;

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
    segments[3] = slice_from_cstr("callable_value_dep");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    /* The top-level dependency has no owner and retains the open T in both
     * its source callable arguments and Mapper<T> target surface. */
    make_decl = feng_symbol_module_find_public_value(
        module, slice_from_cstr("make"));
    ASSERT(make_decl != NULL);
    ASSERT(make_decl->reifiable_callable_dep_count == 1U);
    dependency = &make_decl->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind == FENG_RESOLVED_CALLABLE_FUNCTION);
    ASSERT(dependency->target_module_name != NULL);
    ASSERT(strcmp(dependency->target_module_name,
                  "feng.test.symbol.callable_value_dep") == 0);
    ASSERT(dependency->target_symbol_id != 0U);
    ASSERT(dependency->owner_instance_type == NULL);
    ASSERT(dependency->callable_type_arg_count == 1U);
    type = dependency->callable_type_args[0];
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(feng_symbol_type_type_param_ref_name(type), "T"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(type) == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(type, 0U)), "T"));

    /* A bound method preserves Reader<T> as its owner instance and carries
     * no method-local arguments when the selected read method is non-generic. */
    reader_type = feng_symbol_module_find_public_type(
        module, slice_from_cstr("Reader"));
    ASSERT(reader_type != NULL);
    reader_method = feng_symbol_decl_find_public_member(
        reader_type, slice_from_cstr("reader"));
    ASSERT(reader_method != NULL);
    ASSERT(reader_method->reifiable_callable_dep_count == 1U);
    dependency = &reader_method->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind == FENG_RESOLVED_CALLABLE_TYPE_METHOD);
    ASSERT(dependency->target_symbol_id != 0U);
    ASSERT(dependency->callable_type_arg_count == 0U);
    type = dependency->owner_instance_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(type) == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(type, 0U)), "T"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(type) == 1U);

    /* A method-level explicit target retains its own open T independently
     * from the non-generic owner instance. */
    method_owner_type = feng_symbol_module_find_public_type(
        module, slice_from_cstr("MethodOwner"));
    ASSERT(method_owner_type != NULL);
    mapper_method = feng_symbol_decl_find_public_member(
        method_owner_type, slice_from_cstr("mapper"));
    ASSERT(mapper_method != NULL);
    ASSERT(mapper_method->reifiable_callable_dep_count == 1U);
    dependency = &mapper_method->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind == FENG_RESOLVED_CALLABLE_TYPE_METHOD);
    ASSERT(dependency->callable_type_arg_count == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            dependency->callable_type_args[0]), "T"));
    ASSERT(dependency->owner_instance_type != NULL);
    ASSERT(feng_symbol_type_kind(dependency->owner_instance_type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED);

    /* The same source function entering two target surfaces must remain two
     * dependencies after export/import. The source identity is shared, while
     * each target spec and its adapter surface stays distinct. */
    surfaces_decl = feng_symbol_module_find_public_value(
        module, slice_from_cstr("surfaces"));
    ASSERT(surfaces_decl != NULL);
    ASSERT(surfaces_decl->reifiable_callable_dep_count == 2U);
    ASSERT(surfaces_decl->reifiable_callable_deps[0].target_symbol_id ==
           surfaces_decl->reifiable_callable_deps[1].target_symbol_id);
    ASSERT(surfaces_decl->reifiable_callable_deps[0].target_callable_type != NULL);
    ASSERT(surfaces_decl->reifiable_callable_deps[1].target_callable_type != NULL);
    {
        const FengSymbolTypeView *first_target =
            surfaces_decl->reifiable_callable_deps[0].target_callable_type;
        const FengSymbolTypeView *second_target =
            surfaces_decl->reifiable_callable_deps[1].target_callable_type;
        size_t first_segment_count = feng_symbol_type_segment_count(first_target);
        size_t second_segment_count = feng_symbol_type_segment_count(second_target);

        ASSERT(first_segment_count > 0U && second_segment_count > 0U);
        ASSERT(slice_equals_cstr(
            feng_symbol_type_segment_at(first_target, first_segment_count - 1U),
            "Mapper"));
        ASSERT(slice_equals_cstr(
            feng_symbol_type_segment_at(second_target, second_segment_count - 1U),
            "MapperAlt"));
    }

    /* A generic fit method retains both its generic owner T and method-local
     * U, rather than being restored as an ordinary type method dependency. */
    fit_mapper_decl = feng_symbol_module_find_public_value(
        module, slice_from_cstr("fitMapper"));
    ASSERT(fit_mapper_decl != NULL);
    ASSERT(fit_mapper_decl->reifiable_callable_dep_count == 1U);
    dependency = &fit_mapper_decl->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind == FENG_RESOLVED_CALLABLE_FIT_METHOD);
    ASSERT(dependency->callable_type_arg_count == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            dependency->callable_type_args[0]), "U"));
    ASSERT(dependency->owner_instance_type != NULL);
    ASSERT(feng_symbol_type_kind(dependency->owner_instance_type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(
               dependency->owner_instance_type) == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(
                dependency->owner_instance_type, 0U)), "T"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* A T: ObjectSpec method-value dependency uses the existing callable-dep FT
 * record unchanged and round-trips its requirement identity, receiver T and
 * target callable surface. */
static void test_constrained_generic_spec_method_value_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_spec_method_value;\n"
        "open spec Stateful { func step(delta: i32): i32; }\n"
        "open spec Stepper(delta: i32): i32;\n"
        "open func bind<T: Stateful>(value: T): Stepper {\n"
        "    return value.step;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "generic_spec_method_value.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module;
    FengSlice segments[4];
    const FengSymbolDeclView *stateful;
    const FengSymbolDeclView *step;
    const FengSymbolDeclView *bind;
    const FengSymbolCallableDepView *dependency;
    const FengSymbolTypeView *type;
    size_t segment_count;

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
    segments[3] = slice_from_cstr("generic_spec_method_value");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    stateful = feng_symbol_module_find_public_spec(
        module, slice_from_cstr("Stateful"));
    ASSERT(stateful != NULL);
    step = feng_symbol_decl_find_public_member(
        stateful, slice_from_cstr("step"));
    ASSERT(step != NULL);
    bind = feng_symbol_module_find_public_value(
        module, slice_from_cstr("bind"));
    ASSERT(bind != NULL);
    ASSERT(bind->reifiable_callable_dep_count == 1U);
    dependency = &bind->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind == FENG_RESOLVED_CALLABLE_SPEC_METHOD);
    ASSERT(dependency->target_symbol_id == step->ft_symbol_id);
    ASSERT(dependency->callable_type_arg_count == 0U);

    type = dependency->owner_instance_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(type), "T"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_NAMED);
    segment_count = feng_symbol_type_segment_count(type);
    ASSERT(segment_count > 0U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_segment_at(type, segment_count - 1U), "Stepper"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* A T: IntersectionSpec method-value dependency uses the same FT record as
 * its object-form counterpart. The exact leaf requirement, receiver T and
 * callable target round-trip without a new field, kind or format version. */
static void test_constrained_generic_intersection_method_value_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_intersection_method_value;\n"
        "open spec Stateful { func step(delta: i32): i32; }\n"
        "open spec Named { func name(): string; }\n"
        "open spec Both: Stateful & Named;\n"
        "open spec Stepper(delta: i32): i32;\n"
        "open func bind<T: Both>(value: T): Stepper {\n"
        "    return value.step;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "generic_intersection_method_value.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module;
    FengSlice segments[4];
    const FengSymbolDeclView *stateful;
    const FengSymbolDeclView *step;
    const FengSymbolDeclView *bind;
    const FengSymbolCallableDepView *dependency;
    const FengSymbolTypeView *type;
    size_t segment_count;

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
    ASSERT(feng_symbol_provider_add_ft_root(
        provider,
        public_root,
        FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
        &error));
    feng_symbol_error_free(&error);
    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("generic_intersection_method_value");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    stateful = feng_symbol_module_find_public_spec(
        module, slice_from_cstr("Stateful"));
    ASSERT(stateful != NULL);
    step = feng_symbol_decl_find_public_member(
        stateful, slice_from_cstr("step"));
    ASSERT(step != NULL);
    bind = feng_symbol_module_find_public_value(
        module, slice_from_cstr("bind"));
    ASSERT(bind != NULL);
    ASSERT(bind->reifiable_callable_dep_count == 1U);
    dependency = &bind->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind == FENG_RESOLVED_CALLABLE_SPEC_METHOD);
    ASSERT(dependency->target_symbol_id == step->ft_symbol_id);
    ASSERT(dependency->callable_type_arg_count == 0U);

    type = dependency->owner_instance_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(type), "T"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_NAMED);
    segment_count = feng_symbol_type_segment_count(type);
    ASSERT(segment_count > 0U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_segment_at(type, segment_count - 1U), "Stepper"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* A T: ObjectSpec static method-value dependency uses the existing callable-
 * dependency FT record. The roundtrip preserves the static requirement,
 * caller-view owner T and target callable without adding a record field or a
 * new serialized kind value. */
static void test_constrained_generic_spec_static_method_value_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_spec_static_method_value;\n"
        "open spec Factory { static func create(seed: i32): i32; }\n"
        "open spec Creator(seed: i32): i32;\n"
        "open func bind<T: Factory>(): Creator {\n"
        "    return T.create;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "generic_spec_static_method_value.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module;
    FengSlice segments[4];
    const FengSymbolDeclView *factory;
    const FengSymbolDeclView *create;
    const FengSymbolDeclView *bind;
    const FengSymbolCallableDepView *dependency;
    const FengSymbolTypeView *type;
    size_t segment_count;

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
    segments[3] = slice_from_cstr("generic_spec_static_method_value");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    factory = feng_symbol_module_find_public_spec(
        module, slice_from_cstr("Factory"));
    ASSERT(factory != NULL);
    create = feng_symbol_decl_find_public_member(
        factory, slice_from_cstr("create"));
    ASSERT(create != NULL);
    bind = feng_symbol_module_find_public_value(
        module, slice_from_cstr("bind"));
    ASSERT(bind != NULL);
    ASSERT(bind->reifiable_callable_dep_count == 1U);
    dependency = &bind->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind ==
           FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD);
    ASSERT(dependency->target_symbol_id == create->ft_symbol_id);
    ASSERT(dependency->callable_type_arg_count == 0U);

    type = dependency->owner_instance_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(type), "T"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_NAMED);
    segment_count = feng_symbol_type_segment_count(type);
    ASSERT(segment_count > 0U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_segment_at(type, segment_count - 1U), "Creator"));

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* T: IntersectionSpec static method-value dependencies reuse the object-form
 * static callable-dependency record. Direct, generic-parent and nested leaf
 * requirements retain their exact declarations, caller-view T and callable
 * targets across an FT export/import roundtrip. */
static void
test_intersection_constrained_static_method_value_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.intersection_static_method_value;\n"
        "open spec RootFactory<T> {\n"
        "  static func inherited(value: T): T;\n"
        "}\n"
        "open spec NumberFactory: RootFactory<i32> {\n"
        "  static func create(seed: i32): i32;\n"
        "}\n"
        "open spec TextFactory {\n"
        "  static func label(value: string): string;\n"
        "}\n"
        "open spec CombinedFactory: NumberFactory & TextFactory;\n"
        "open spec ExtraFactory {\n"
        "  static func extra(value: i32): i32;\n"
        "}\n"
        "open spec NestedFactory: CombinedFactory & ExtraFactory;\n"
        "open spec IntMapper(value: i32): i32;\n"
        "open spec TextMapper(value: string): string;\n"
        "open func bindCreate<T: NestedFactory>(): IntMapper {\n"
        "  return T.create;\n"
        "}\n"
        "open func bindInherited<T: NestedFactory>(): IntMapper {\n"
        "  return T.inherited;\n"
        "}\n"
        "open func bindLabel<T: NestedFactory>(): TextMapper {\n"
        "  return T.label;\n"
        "}\n"
        "open func bindExtra<T: NestedFactory>(): IntMapper {\n"
        "  return T.extra;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "intersection_static_method_value.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module;
    FengSlice segments[4];
    const FengSymbolDeclView *owners[4];
    const FengSymbolDeclView *members[4];
    const FengSymbolDeclView *binds[4];
    const char *owner_names[4] = {
        "NumberFactory", "RootFactory", "TextFactory", "ExtraFactory"};
    const char *member_names[4] = {
        "create", "inherited", "label", "extra"};
    const char *bind_names[4] = {
        "bindCreate", "bindInherited", "bindLabel", "bindExtra"};
    const char *target_names[4] = {
        "IntMapper", "IntMapper", "TextMapper", "IntMapper"};

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
    segments[3] = slice_from_cstr("intersection_static_method_value");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    for (size_t index = 0U; index < 4U; ++index) {
        const FengSymbolCallableDepView *dependency;
        const FengSymbolTypeView *type;
        size_t segment_count;

        owners[index] = feng_symbol_module_find_public_spec(
            module, slice_from_cstr(owner_names[index]));
        ASSERT(owners[index] != NULL);
        members[index] = feng_symbol_decl_find_public_member(
            owners[index], slice_from_cstr(member_names[index]));
        ASSERT(members[index] != NULL);
        binds[index] = feng_symbol_module_find_public_value(
            module, slice_from_cstr(bind_names[index]));
        ASSERT(binds[index] != NULL);
        ASSERT(binds[index]->reifiable_callable_dep_count == 1U);
        dependency = &binds[index]->reifiable_callable_deps[0];
        ASSERT(dependency->purpose ==
               FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
        ASSERT(dependency->kind ==
               FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD);
        ASSERT(dependency->target_symbol_id ==
               members[index]->ft_symbol_id);
        ASSERT(dependency->callable_type_arg_count == 0U);

        type = dependency->owner_instance_type;
        ASSERT(feng_symbol_type_kind(type) ==
               FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
        ASSERT(slice_equals_cstr(
            feng_symbol_type_type_param_ref_name(type), "T"));
        type = dependency->target_callable_type;
        ASSERT(feng_symbol_type_kind(type) == FENG_SYMBOL_TYPE_KIND_NAMED);
        segment_count = feng_symbol_type_segment_count(type);
        ASSERT(segment_count > 0U);
        ASSERT(slice_equals_cstr(
            feng_symbol_type_segment_at(type, segment_count - 1U),
            target_names[index]));
    }

    feng_symbol_provider_free(provider);
    feng_symbol_error_free(&error);
    (void)remove_dir_recursive(tmp_dir);
    free(tmp_dir);
}

/* Concrete type/fit static method-value dependencies reuse the existing FT
 * callable-dependency record. The roundtrip preserves the exact source
 * symbol, owner instance, method-local arguments and target callable without
 * adding a receiver or a new serialized record shape. */
static void test_static_method_value_dependency_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.static_method_value_dep;\n"
        "open spec Mapper<T>(value: T): T;\n"
        "open type StaticOwner<T> {\n"
        "  open static func echo(value: T): T { return value; }\n"
        "  open static func identity<U>(value: U): U { return value; }\n"
        "  open static func mapper(): Mapper<T> {\n"
        "    return StaticOwner<T>.echo;\n"
        "  }\n"
        "  open static func methodMapper<U>(): Mapper<U> {\n"
        "    return StaticOwner<T>.identity<U>;\n"
        "  }\n"
        "}\n"
        "open type StaticFitOwner<T> {}\n"
        "open fit StaticFitOwner<T> {\n"
        "  open static func echo(value: T): T { return value; }\n"
        "}\n"
        "open func fitMapper<T>(): Mapper<T> {\n"
        "  return StaticFitOwner<T>.echo;\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "static_method_value_dep.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolDeclView *owner_type;
    const FengSymbolDeclView *mapper_method;
    const FengSymbolDeclView *generic_mapper_method;
    const FengSymbolDeclView *fit_mapper;
    const FengSymbolCallableDepView *dependency;
    const FengSymbolTypeView *type;

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
    segments[3] = slice_from_cstr("static_method_value_dep");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    owner_type = feng_symbol_module_find_public_type(
        module, slice_from_cstr("StaticOwner"));
    ASSERT(owner_type != NULL);
    mapper_method = feng_symbol_decl_find_public_member(
        owner_type, slice_from_cstr("mapper"));
    ASSERT(mapper_method != NULL);
    ASSERT(mapper_method->reifiable_callable_dep_count == 1U);
    dependency = &mapper_method->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind ==
           FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD);
    ASSERT(dependency->target_symbol_id != 0U);
    ASSERT(dependency->callable_type_arg_count == 0U);
    type = dependency->owner_instance_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(type) == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(type, 0U)), "T"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(type) == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(type, 0U)), "T"));

    generic_mapper_method = feng_symbol_decl_find_public_member(
        owner_type, slice_from_cstr("methodMapper"));
    ASSERT(generic_mapper_method != NULL);
    ASSERT(generic_mapper_method->reifiable_callable_dep_count == 1U);
    dependency = &generic_mapper_method->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind ==
           FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD);
    ASSERT(dependency->callable_type_arg_count == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            dependency->callable_type_args[0]), "U"));
    type = dependency->target_callable_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(type, 0U)), "U"));

    fit_mapper = feng_symbol_module_find_public_value(
        module, slice_from_cstr("fitMapper"));
    ASSERT(fit_mapper != NULL);
    ASSERT(fit_mapper->reifiable_callable_dep_count == 1U);
    dependency = &fit_mapper->reifiable_callable_deps[0];
    ASSERT(dependency->purpose == FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE);
    ASSERT(dependency->kind ==
           FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD);
    ASSERT(dependency->target_symbol_id != 0U);
    ASSERT(dependency->callable_type_arg_count == 0U);
    type = dependency->owner_instance_type;
    ASSERT(feng_symbol_type_kind(type) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(type) == 1U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(type, 0U)), "T"));

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

/* Generic type-declared specs and generic spec parents must retain owner type
 * parameters as TYPE_PARAM_REF nodes, including reordered and nested uses. */
static void test_generic_spec_relation_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_spec_relation;\n"
        "open type Box<T> {}\n"
        "open spec Parent<T> {}\n"
        "open spec Child<T>: Parent<Box<T>> {}\n"
        "open spec Surface<A, B> {}\n"
        "open type Owner<T, U>: Surface<Box<U>, T> {}\n";
    FengProgram *program = parse_or_die(
        "generic_spec_relation.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolDeclView *child;
    const FengSymbolDeclView *owner;
    const FengSymbolTypeView *relation;
    const FengSymbolTypeView *nested;
    const FengSymbolTypeView *argument;

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
    segments[0] = slice_from_cstr("feng");
    segments[1] = slice_from_cstr("test");
    segments[2] = slice_from_cstr("symbol");
    segments[3] = slice_from_cstr("generic_spec_relation");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);

    child = feng_symbol_module_find_public_spec(
        module, slice_from_cstr("Child"));
    ASSERT(child != NULL);
    ASSERT(feng_symbol_decl_declared_spec_count(child) == 1U);
    relation = feng_symbol_decl_declared_spec_at(child, 0U);
    ASSERT(feng_symbol_type_kind(relation) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(relation) == 1U);
    nested = feng_symbol_type_generic_arg_at(relation, 0U);
    ASSERT(feng_symbol_type_kind(nested) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(nested) == 1U);
    argument = feng_symbol_type_generic_arg_at(nested, 0U);
    ASSERT(feng_symbol_type_kind(argument) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(argument), "T"));

    owner = feng_symbol_module_find_public_type(
        module, slice_from_cstr("Owner"));
    ASSERT(owner != NULL);
    ASSERT(feng_symbol_decl_declared_spec_count(owner) == 1U);
    relation = feng_symbol_decl_declared_spec_at(owner, 0U);
    ASSERT(feng_symbol_type_kind(relation) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(relation) == 2U);
    nested = feng_symbol_type_generic_arg_at(relation, 0U);
    ASSERT(feng_symbol_type_kind(nested) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(nested) == 1U);
    argument = feng_symbol_type_generic_arg_at(nested, 0U);
    ASSERT(feng_symbol_type_kind(argument) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(argument), "U"));
    argument = feng_symbol_type_generic_arg_at(relation, 1U);
    ASSERT(feng_symbol_type_kind(argument) ==
           FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(argument), "T"));

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

/* Reifiable dependencies of generic fit methods are exported on the method
 * that owns the FengFunctionDescriptor, never merged onto the fit decl. */
static void test_generic_fit_member_dependency_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.generic_fit_member_deps;\n"
        "open type ManagedDep<T> {\n"
        "    open let value: T;\n"
        "    func ManagedDep(value: T) { self.value = value; }\n"
        "}\n"
        "@value\n"
        "open type AggregateDep<T> {\n"
        "    open let value: T;\n"
        "    func AggregateDep(value: T) { self.value = value; }\n"
        "}\n"
        "@value\n"
        "open type PairDep<T, U> {\n"
        "    open let first: T;\n"
        "    open let second: U;\n"
        "    func PairDep(first: T, second: U) {\n"
        "        self.first = first; self.second = second;\n"
        "    }\n"
        "}\n"
        "open type Host<T> {\n"
        "    open let value: T;\n"
        "    func Host(value: T) { self.value = value; }\n"
        "}\n"
        "open fit Host<T> {\n"
        "    open func managed(): T {\n"
        "        let dep = ManagedDep<T>(self.value); return dep.value;\n"
        "    }\n"
        "    open func aggregate(): T {\n"
        "        let dep = AggregateDep<T>(self.value); return dep.value;\n"
        "    }\n"
        "    open func pair<U>(value: U): U {\n"
        "        let dep = PairDep<T, U>(self.value, value);\n"
        "        return dep.second;\n"
        "    }\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "generic_fit_member_deps.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module = NULL;
    FengSlice segments[4];
    const FengSymbolFitView *fit_view;
    const FengSymbolDeclView *fit_decl;
    const FengSymbolDeclView *managed_method;
    const FengSymbolDeclView *aggregate_method;
    const FengSymbolDeclView *pair_method;
    const FengSymbolTypeView *dependency;

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
    segments[3] = slice_from_cstr("generic_fit_member_deps");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_fit_count(module) == 1U);
    fit_view = feng_symbol_module_fit_at(module, 0U);
    ASSERT(fit_view != NULL);
    fit_decl = feng_symbol_fit_decl(fit_view);
    ASSERT(fit_decl != NULL);
    ASSERT(fit_decl->reifiable_type_dep_count == 0U);
    ASSERT(fit_decl->reifiable_agg_dep_count == 0U);

    managed_method = feng_symbol_decl_find_public_member(
        fit_decl, slice_from_cstr("managed"));
    aggregate_method = feng_symbol_decl_find_public_member(
        fit_decl, slice_from_cstr("aggregate"));
    pair_method = feng_symbol_decl_find_public_member(
        fit_decl, slice_from_cstr("pair"));
    ASSERT(managed_method != NULL);
    ASSERT(aggregate_method != NULL);
    ASSERT(pair_method != NULL);
    ASSERT(managed_method->reifiable_type_dep_count == 1U);
    ASSERT(managed_method->reifiable_agg_dep_count == 0U);
    ASSERT(aggregate_method->reifiable_type_dep_count == 0U);
    ASSERT(aggregate_method->reifiable_agg_dep_count == 1U);
    ASSERT(pair_method->reifiable_type_dep_count == 0U);
    ASSERT(pair_method->reifiable_agg_dep_count == 1U);

    dependency = managed_method->reifiable_type_deps[0];
    ASSERT(feng_symbol_type_kind(dependency) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_segment_at(
            dependency,
            feng_symbol_type_segment_count(dependency) - 1U),
        "ManagedDep"));
    dependency = aggregate_method->reifiable_agg_deps[0];
    ASSERT(feng_symbol_type_kind(dependency) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_segment_at(
            dependency,
            feng_symbol_type_segment_count(dependency) - 1U),
        "AggregateDep"));
    dependency = pair_method->reifiable_agg_deps[0];
    ASSERT(feng_symbol_type_kind(dependency) ==
           FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC);
    ASSERT(feng_symbol_type_generic_arg_count(dependency) == 2U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(dependency, 0U)),
        "T"));
    ASSERT(slice_equals_cstr(
        feng_symbol_type_type_param_ref_name(
            feng_symbol_type_generic_arg_at(dependency, 1U)),
        "U"));

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

/* A concrete enum array fit remains a named element type in .ft; it must not
 * be serialized as the TYPE_PARAM_REF form reserved for `fit T[]`. */
static void test_fit_concrete_enum_array_target_ft_roundtrip(void) {
    static const char *kSource =
        "open module feng.test.symbol.fit_enum_array_target;\n"
        "open enum Element { First }\n"
        "open spec Readable { func read(): i32; }\n"
        "open fit Element[!]: Readable {\n"
        "    open func read(): i32 { return 1; }\n"
        "}\n";
    FengProgram *program = parse_or_die(
        "fit_concrete_enum_array_target.ff", kSource);
    FengSemanticAnalysis *analysis = analyze_or_die(program);
    FengSymbolError error = {0};
    char *tmp_dir = make_temp_dir();
    char public_root[1024];
    FengSymbolProvider *provider = NULL;
    const FengSymbolImportedModule *module;
    FengSlice segments[4];
    const FengSymbolFitView *fit_view;
    const FengSymbolDeclView *fit_decl;
    const FengSymbolTypeView *target_type;
    const FengSymbolTypeView *target_element;
    size_t segment_count;

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
    segments[3] = slice_from_cstr("fit_enum_array_target");
    module = feng_symbol_provider_find_module(provider, segments, 4U);
    ASSERT(module != NULL);
    ASSERT(feng_symbol_module_fit_count(module) == 1U);
    fit_view = feng_symbol_module_fit_at(module, 0U);
    ASSERT(fit_view != NULL);
    fit_decl = feng_symbol_fit_decl(fit_view);
    ASSERT(fit_decl != NULL);
    target_type = feng_symbol_decl_fit_target(fit_decl);
    ASSERT(target_type != NULL);
    ASSERT(feng_symbol_type_kind(target_type) == FENG_SYMBOL_TYPE_KIND_ARRAY);
    ASSERT(feng_symbol_type_array_rank(target_type) == 1U);
    ASSERT(feng_symbol_type_array_layer_writable(target_type, 0U));
    target_element = feng_symbol_type_inner(target_type);
    ASSERT(target_element != NULL);
    ASSERT(feng_symbol_type_kind(target_element) == FENG_SYMBOL_TYPE_KIND_NAMED);
    segment_count = feng_symbol_type_segment_count(target_element);
    ASSERT(segment_count > 0U);
    ASSERT(slice_equals_cstr(
        feng_symbol_type_segment_at(target_element, segment_count - 1U),
        "Element"));

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
    test_object_spec_seal_member_ft_roundtrip();
    test_selected_seal_spec_implementations_enter_package_ft();
    test_package_selection_uses_existing_graph();
    test_selected_seal_static_fields_remain_seal_in_package_ft();
    test_imported_object_spec_seal_access_semantics();
    test_friend_metadata_is_not_exported_to_ft();
    test_local_friend_fit_can_target_imported_type();
    test_imported_type_seal_members_do_not_satisfy_consumer_fit();
    test_bounded_decl_ft_roundtrip_uses_inferred_initializer();
    test_let_three_phase_binding_semantics_survive_ft_roundtrip();
    test_constructor_availability_survives_ft_roundtrip();
    test_mixin_generated_members_ft_roundtrip();
    test_mixable_seal_member_ft_roundtrip_preserves_field_facts();
    test_roundtrip_public_module_docs();
    test_private_module_skipped();
    test_reader_rejects_bad_magic();
    test_provider_loads_bundle_public_module();
    test_enum_ft_roundtrip_exports_items_and_values();
    test_provider_rejects_duplicate_bundle_module();
    test_provider_rejects_bad_bundle_symbol_entry();
    test_imported_module_cache_keeps_synthesized_modules_alive();
    test_imported_module_cache_preserves_extern_c_symbol_name();
    test_c_variadic_fixed_param_count_ft_roundtrip();
    test_imported_module_cache_preserves_enum_items();
    test_imported_enum_value_participates_in_semantic_analysis();
    test_imported_module_cache_keeps_bundle_fit_modules_alive();
    test_imported_module_cache_keeps_multi_file_bundle_fit_modules_alive();
    test_generic_function_ft_roundtrip();
    test_callable_value_dependency_ft_roundtrip();
    test_constrained_generic_spec_method_value_ft_roundtrip();
    test_constrained_generic_intersection_method_value_ft_roundtrip();
    test_constrained_generic_spec_static_method_value_ft_roundtrip();
    test_intersection_constrained_static_method_value_ft_roundtrip();
    test_static_method_value_dependency_ft_roundtrip();
    test_generic_type_ft_roundtrip();
    test_inferred_generic_field_ft_roundtrip();
    test_generic_spec_relation_ft_roundtrip();
    test_generic_fit_ft_roundtrip();
    test_generic_fit_member_dependency_ft_roundtrip();
    test_generic_fit_named_generic_return_ft_roundtrip();
    test_fit_builtin_and_array_target_nodes_ft_roundtrip();
    test_fit_array_type_param_target_ft_roundtrip();
    test_fit_concrete_enum_array_target_ft_roundtrip();
    test_private_representation_dependency_closure_roundtrip();
    test_signature_visibility_error_prevents_public_ft_export();
    fprintf(stdout, "symbol tests passed\n");
    return 0;
}
