#include "semantic/exception_effects.h"

/* Observe delivered advisories independently of the complete semantic result. */
typedef struct ExceptionInfoCapture {
    size_t delivered;
    size_t total;
    size_t general;
    size_t boundary_errors;
} ExceptionInfoCapture;

/* Existing general information keeps its original indices and count. */
static void exception_info_capture(void *user, const FengSemanticInfo *info,
                                    size_t index, size_t count,
                                    const FengCliLoadedSource *source) {
    ExceptionInfoCapture *capture = user;
    ASSERT(source != NULL);
    ASSERT(index == capture->delivered && index < count);
    ASSERT(index == 0U || count == capture->total);
    capture->total = count;
    ++capture->delivered;
    ASSERT(strstr(info->message, "orphan fit") != NULL);
    ASSERT(strstr(info->message, "exceptions may propagate from this call:") == NULL);
    ++capture->general;
}

/* Strict boundary failures must use the error callback and the existing code. */
static void exception_info_boundary_error(void *user, const FengSemanticError *error,
                                          size_t index, size_t count,
                                          const FengCliLoadedSource *source) {
    ExceptionInfoCapture *capture = user;
    if (strcmp(error->code, "AE1313") != 0)
        fprintf(stderr, "unexpected routing diagnostic %s: %s at %s:%u\n",
                error->code, error->message, error->path, error->token.line);
    ASSERT(source != NULL && index == 0U && count == 1U);
    ASSERT(strcmp(error->code, "AE1313") == 0);
    ++capture->boundary_errors;
}

/* Inspect outgoing summaries without depending on their diagnostic routing. */
static void exception_info_assert_summary(const FengSemanticAnalysis *analysis,
                                          const FengCliLoadedSource *source,
                                          const char *name, const char *expected) {
    for (size_t i = 0U; i < source->program->declaration_count; ++i) {
        const FengDecl *decl = source->program->declarations[i];
        if (decl->kind != FENG_DECL_FUNCTION ||
            decl->as.function_decl.name.length != strlen(name) ||
            memcmp(decl->as.function_decl.name.data, name, strlen(name)) != 0) continue;
        const FengExceptionTemplate *summary = feng_semantic_exception_template(analysis, decl);
        ASSERT(summary != NULL);
        char *types = feng_exception_format(summary->graph, summary->effects);
        ASSERT(types != NULL && strcmp(types, expected) == 0);
        free(types);
        return;
    }
    ASSERT(false);
}

/* The real CLI binary covers tool commands that are outside test_cli's linkage. */
static int exception_info_run_cli(const char *program, int argc, char **argv) {
    (void)program;
    char **arguments = calloc((size_t)argc + 2U, sizeof(*arguments));
    ASSERT(arguments != NULL);
    arguments[0] = "build/bin/feng";
    for (int i = 0; i < argc; ++i) arguments[i + 1] = argv[i];
    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        execv(arguments[0], arguments);
        _exit(127);
    }
    int status = 0;
    ASSERT(waitpid(child, &status, 0) == child);
    free(arguments);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Ordinary commands are quiet about effects; general advisories and ABI errors remain. */
static void exception_info_assert_cli(CliEntryFn entry, int argc, char **argv,
                                      bool orphan, bool boundary_error) {
    int rc = -1;
    char *errors = NULL;
    char *output = run_cli_entry_capture_stdout_and_stderr(entry, argc, argv, &rc, &errors);
    if ((rc == 0) == boundary_error) fprintf(stderr, "%s\n%s\n", output, errors);
    ASSERT((rc != 0) == boundary_error);
    ASSERT(strstr(output, "exceptions may propagate from this call:") == NULL);
    ASSERT(strstr(errors, "exceptions may propagate from this call:") == NULL);
    ASSERT(!orphan || strstr(output, "orphan fit") != NULL || strstr(errors, "orphan fit") != NULL);
    if (boundary_error) {
        /* The existing CLI JSON schema carries severity/source/message,
         * while its text renderer and the semantic callback carry AE1313. */
        ASSERT(strstr(output, "AE1313") != NULL || strstr(errors, "AE1313") != NULL ||
               (strstr(output, "\"severity\": \"error\"") != NULL &&
                strstr(output, "\"source\": \"semantic\"") != NULL));
        ASSERT(strstr(output, "cannot prove an empty exception summary") != NULL ||
               strstr(errors, "cannot prove an empty exception summary") != NULL);
    }
    free(output); free(errors);
}

/* Core/frontend produce no exception infos; summaries, LSP and ABI errors remain. */
static void test_exception_info_routing(void) {
    char directory[] = "temp/exception-info-routing-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *bundle = effects_lsp_pack(directory, "info_provider",
        "open module info.api;open spec External{}open spec Action():void;"
        "open func opaque(f:Action){f();}open func relay(f:Action){opaque(f);}", NULL);
    char *consumer = path_join(directory, "consumer"), *src = path_join(consumer, "src");
    char *path = path_join(src, "main.ff"), *manifest = path_join(consumer, "feng.fm");
    mkdir_p(src);
    char *config = dup_printf("[package]\nname: \"info_consumer\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n[dependencies]\ninfo_provider: \"%s\"\n", bundle);
    write_text_file(manifest, config);
    const char *body = "open module info.consumer;import info.api;open fit i32:External{}"
        "var callback:Action=(){};"
        "func fail(){throw true;}func ordinary(){fail();}"
        "func higher(f:Action){relay(f);}func handled(f:Action){try higher(f) catch{}}"
        "func partial(f:Action){try higher(f) catch e:string{}}"
        "func rethrow(f:Action){try higher(f) catch{throw;}}"
        "func translated(f:Action){try higher(f) catch{throw true;}}"
        "func cleanup(f:Action){defer{higher(f);}}";
    write_text_file(path, body);
    char *paths[] = {path};
    const char *packages[] = {bundle};
    FengCliFrontendInput input = {.path_count = 1, .paths = paths, .target = FENG_COMPILE_TARGET_LIB,
        .package_path_count = 1, .package_paths = packages};
    {
        ExceptionInfoCapture capture = {0};
        FengCliFrontendCallbacks callbacks = {.on_semantic_info = exception_info_capture,
            .on_semantic_error = exception_info_boundary_error, .user = &capture};
        FengSemanticAnalysis *analysis = NULL;
        FengSymbolImportedModuleCache *cache = NULL;
        FengCliLoadedSource *sources = NULL;
        size_t source_count = 0U;
        FengCliFrontendOutputs outputs = {.out_analysis = &analysis, .out_sources = &sources,
            .out_source_count = &source_count, .out_imported_module_cache = &cache};
        ASSERT(feng_cli_frontend_run(&input, &callbacks, &outputs) == 0);
        ASSERT(capture.general == 1U && capture.boundary_errors == 0U);
        ASSERT(capture.delivered == capture.total);
        ASSERT(source_count == 1U && analysis->info_count == 1U);
        ASSERT(feng_semantic_exception_call_count(analysis) > 0U);
        exception_info_assert_summary(analysis, sources, "ordinary", "bool");
        exception_info_assert_summary(analysis, sources, "higher", "unknown");
        exception_info_assert_summary(analysis, sources, "handled", "none");
        exception_info_assert_summary(analysis, sources, "partial", "unknown");
        exception_info_assert_summary(analysis, sources, "rethrow", "unknown");
        exception_info_assert_summary(analysis, sources, "translated", "bool");
        exception_info_assert_summary(analysis, sources, "cleanup", "unknown");
        feng_semantic_analysis_free(analysis);
        feng_symbol_imported_module_cache_free(cache);
        feng_cli_free_loaded_sources(sources, source_count);
    }
    const char *initialize = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}";
    char *hover = capture_lsp_position_response_at_path(path, body, initialize,
        "textDocument/hover", "func higher", 5U, "Possible exceptions: unknown");
    ASSERT(strstr(hover, "Possible exceptions: unknown") != NULL);
    ASSERT(strstr(hover, "unknown (") == NULL);
    ASSERT(strstr(hover, "exceptions may propagate from this call:") == NULL);
    free(hover);
    char *check_text[] = {consumer}, *check_json[] = {consumer, "--format=json"};
    exception_info_assert_cli(feng_cli_project_check_main, 1, check_text, true, false);
    exception_info_assert_cli(feng_cli_project_check_main, 2, check_json, true, false);
    char *out = path_join(directory, "direct"), *out_option = make_out_option(out);
    char *pkg_option = make_pkg_option(bundle);
    char *direct[] = {path, "--target=lib", "--name=info_direct", out_option, pkg_option};
    exception_info_assert_cli(feng_cli_direct_main, 5, direct, true, false);
    exception_info_assert_cli(feng_cli_project_build_main, 1, check_text, true, false);
    exception_info_assert_cli(feng_cli_project_pack_main, 1, check_text, false, false);
    const char *boundaries[] = {
        "@abi func boundary(){higher(callback);}",
        "@abi func boundary(){try higher(callback) catch e:string{}}",
        "@abi func boundary(){try higher(callback) catch{throw;}}",
        "@abi func boundary(){ordinary();}",
        "@abi func boundary(){try higher(callback) catch{}}"};
    for (size_t i = 0U; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        bool failure = i < 4U;
        char *source = dup_printf("%s%s", body, boundaries[i]);
        write_text_file(path, source);
        {
            ExceptionInfoCapture capture = {0};
            FengCliFrontendCallbacks callbacks = {.on_semantic_info = exception_info_capture,
                .on_semantic_error = exception_info_boundary_error, .user = &capture};
            ASSERT((feng_cli_frontend_run(&input, &callbacks, NULL) != 0) == failure);
            ASSERT(capture.boundary_errors == (failure ? 1U : 0U));
        }
        exception_info_assert_cli(feng_cli_project_check_main, 1, check_text, !failure, failure);
        exception_info_assert_cli(feng_cli_project_check_main, 2, check_json, !failure, failure);
        free(source);
    }
    char *standalone = path_join(directory, "standalone.ff");
    write_text_file(standalone, "module info.standalone;spec Action():void;func relay(f:Action){f();}"
        "func top(f:Action){relay(f);}func fail(){throw true;}func call(){fail();}");
    char *semantic[] = {"tool", "semantic", "--target=lib", standalone};
    char *check[] = {"tool", "check", "--target=lib", standalone};
    exception_info_assert_cli(exception_info_run_cli, 4, semantic, false, false);
    exception_info_assert_cli(exception_info_run_cli, 4, check, false, false);
    write_text_file(standalone, "module info.standalone;spec Action():void;func relay(f:Action){f();}"
        "var callback:Action=(){};@abi func boundary(){relay(callback);}");
    exception_info_assert_cli(exception_info_run_cli, 4, semantic, false, true);
    exception_info_assert_cli(exception_info_run_cli, 4, check, false, true);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(directory, &error));
    free(error); free(standalone); free(pkg_option); free(out_option); free(out);
    free(config); free(manifest); free(path); free(src); free(consumer); free(bundle);
    puts("exception info routing, CLI silence and cross-package unknown boundaries passed");
}
