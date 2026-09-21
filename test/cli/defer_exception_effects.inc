/* Capture cleanup diagnostics through the existing frontend error channel. */
typedef struct DeferDiagnosticCapture {
    size_t errors;
    size_t infos;
} DeferDiagnosticCapture;

/* The public frontend must preserve the code, primary location and call links. */
static void defer_diagnostic_error(void *user, const FengSemanticError *error,
                                   size_t index, size_t count, const FengCliLoadedSource *source) {
    DeferDiagnosticCapture *capture = user;
    ASSERT(index == 0U && count == 1U && source != NULL);
    ASSERT(strcmp(error->code, "AE1507") == 0);
    ASSERT(error->token.length == 5U && memcmp(error->token.lexeme, "defer", 5U) == 0);
    ASSERT(error->related_location_count > 0U);
    ++capture->errors;
}

/* Cleanup proof errors must never be downgraded to an information message. */
static void defer_diagnostic_info(void *user, const FengSemanticInfo *info,
                                  size_t index, size_t count, const FengCliLoadedSource *source) {
    DeferDiagnosticCapture *capture = user;
    (void)info; (void)index; (void)count; (void)source;
    ++capture->infos;
}

/* CLI text and JSON retain strict failures while ordinary propagation stays quiet. */
static void test_defer_exception_cli(void) {
    char directory[] = "temp/defer-cli-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *src = path_join(directory, "src"), *path = path_join(src, "main.ff");
    char *manifest = path_join(directory, "feng.fm"), *out = path_join(directory, "build");
    mkdir_p(src);
    write_text_file(manifest, "[package]\nname: \"defer_cli\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n");
    const char *bodies[] = {
        "func run(){fail();}",
        "func run(){defer{try fail() catch e:string{}}}",
        "func run(){defer{fail();}}",
        "func run(f:Action){defer{f();}}",
        "func run(f:Action){defer{try f() catch e:string{}}}",
        "func run(){let x=try (if true{defer{fail();}0;}else{0;}) catch{0;};}"};
    char *out_option = make_out_option(out);
    for (size_t i = 0U; i < sizeof(bodies) / sizeof(*bodies); ++i) {
        bool rejected = i >= 2U;
        char *source = dup_printf("open module cleanup.cli;spec Action():void;func fail(){throw \"x\";}%s", bodies[i]);
        write_text_file(path, source);
        char *paths[] = {path};
        FengCliFrontendInput input = {.path_count = 1U, .paths = paths, .target = FENG_COMPILE_TARGET_LIB};
        DeferDiagnosticCapture capture = {0};
        FengCliFrontendCallbacks callbacks = {.on_semantic_error = defer_diagnostic_error,
            .on_semantic_info = defer_diagnostic_info, .user = &capture};
        ASSERT((feng_cli_frontend_run(&input, &callbacks, NULL) != 0) == rejected);
        ASSERT(capture.errors == (rejected ? 1U : 0U) && capture.infos == 0U);
        for (size_t format = 0U; format < 3U; ++format) {
            char *check[] = {directory, "--format=json"};
            char *direct[] = {path, "--target=lib", "--name=defer_direct", out_option};
            int rc = -1;
            char *errors = NULL;
            char *output = run_cli_entry_capture_stdout_and_stderr(
                format == 0U ? feng_cli_direct_main : feng_cli_project_check_main,
                format == 0U ? 4 : (int)format, format == 0U ? direct : check, &rc, &errors);
            if ((rc != 0) != rejected) fprintf(stderr, "%s\n%s\n", output, errors);
            ASSERT((rc != 0) == rejected);
            ASSERT(strstr(output, "Possible exceptions:") == NULL);
            ASSERT(strstr(errors, "exceptions may propagate from this call:") == NULL);
            if (rejected) {
                ASSERT(strstr(output, "exceptions must not escape the defer block") != NULL ||
                       strstr(errors, "exceptions must not escape the defer block") != NULL);
                if (format == 2U) {
                    ASSERT(strstr(output, "\"severity\": \"error\"") != NULL);
                    ASSERT(strstr(output, "\"source\": \"semantic\"") != NULL);
                } else {
                    ASSERT(strstr(output, "AE1507") != NULL || strstr(errors, "AE1507") != NULL);
                }
            }
            free(errors); free(output);
        }
        free(source);
    }
    char *remove_error = NULL;
    ASSERT(feng_cli_project_remove_tree(directory, &remove_error));
    free(remove_error); free(src); free(path); free(manifest); free(out); free(out_option);
}

/* Sequential document versions exercise callee changes and catch-boundary edits. */
typedef struct DeferLspEdits {
    const char *uri;
    const char *const *sources;
    const char *const *types;
    size_t count;
} DeferLspEdits;

/* Save each version and wait for its full semantic error or both empty publications. */
static void defer_lsp_apply_edits(FILE *input, int output_fd, void *user) {
    const DeferLspEdits *edits = user;
    for (size_t phase = 0U; phase < edits->count; ++phase) {
        const char *source = edits->sources[phase], *types = edits->types[phase];
        char *escaped = json_escape_text(source);
        char *change = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%zu},\"contentChanges\":[{\"text\":\"%s\"}]}}", edits->uri, phase + 2U, escaped);
        char *save = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}", edits->uri);
        write_lsp_message(input, change);
        write_lsp_message(input, save);
        bool complete = false;
        size_t empty = 0U;
        for (unsigned i = 0U; i < 240U && !complete; ++i) {
            unsigned id = 8000U + (unsigned)phase * 300U + i;
            char *request = build_lsp_test_position_request("textDocument/hover", id,
                edits->uri, source, "func run", 5U);
            char *output = send_lsp_test_request_and_wait(input, output_fd, request, id);
            ASSERT(strstr(output, "\"severity\":2") == NULL && strstr(output, "\"severity\":3") == NULL);
            if (types != NULL) {
                char *message = dup_printf("cannot prove an empty exception set: %s", types);
                complete = strstr(output, message) != NULL;
                if (complete) {
                    ASSERT(strstr(output, "\"severity\":1") != NULL);
                    ASSERT(strstr(output, "\"source\":\"semantic\"") != NULL);
                    ASSERT(strstr(output, "\"relatedInformation\":[") != NULL);
                }
                free(message);
            } else {
                ASSERT(strstr(output, "\"severity\":1") == NULL);
                empty += count_occurrences(output, "\"diagnostics\":[]");
                complete = empty >= 2U;
            }
            if (i == 239U && !complete) fprintf(stderr, "defer edit phase %zu (empty %zu)\n%s\n", phase, empty, output);
            free(output); free(request);
            if (!complete) usleep(25000U);
        }
        ASSERT(complete);
        free(save); free(change); free(escaped);
    }
}

/* A single LSP session must add, update and clear errors without changing runtime. */
static void test_defer_exception_lsp(void) {
    const char *initial = "module cleanup.lsp;func work(){}func run(){defer{work();}}func main(args:string[]){}";
    const char *sources[] = {
        "module cleanup.lsp;func work(){throw \"x\";}func run(){defer{work();}}func main(args:string[]){}",
        "module cleanup.lsp;func work(){throw \"x\";}func run(){defer{try work() catch e:string{}}}func main(args:string[]){}",
        "module cleanup.lsp;func work(){throw true;}func run(){defer{try work() catch e:string{}}}func main(args:string[]){}",
        "module cleanup.lsp;func work(){throw true;}func run(){defer{try work() catch{}}}func main(args:string[]){}",
        "module cleanup.lsp;func work(){throw true;}func run(){let x=try (if true{defer{work();}0;}else{0;}) catch{0;};}func main(args:string[]){}",
        "module cleanup.lsp;spec Action():void;func run(f:Action){defer{f();}}func main(args:string[]){}",
        "module cleanup.lsp;spec Action():void;func run(f:Action){defer{try f() catch{}}}func main(args:string[]){}"};
    const char *types[] = {"string", NULL, "bool", NULL, "bool", "unknown", NULL};
    char directory[] = "temp/defer-lsp-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *path = path_join(directory, "main.ff");
    write_text_file(path, initial);
    char *uri = file_uri_from_path(path), *escaped = json_escape_text(initial);
    char *open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}", uri, escaped);
    const char *initialize = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}";
    const char *requests[] = {"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"};
    DeferLspEdits edits = {uri, sources, types, sizeof(sources) / sizeof(*sources)};
    unsigned line, column;
    find_line_character(initial, "func run", 5U, &line, &column);
    char *ready = NULL;
    char *output = run_lsp_server_capture_after_position_ready_action(initialize, open, NULL,
        "textDocument/hover", uri, line, column, "func run()", defer_lsp_apply_edits,
        &edits, requests, 2U, &ready);
    ASSERT(ready != NULL && strstr(ready, "Possible exceptions:") == NULL);
    char *remove_error = NULL;
    ASSERT(feng_cli_project_remove_tree(directory, &remove_error));
    free(remove_error); free(output); free(ready); free(open); free(escaped); free(uri); free(path);
}

/* Source-hidden generic effects cross two packages before reaching cleanup. */
static void test_defer_exception_lsp_packages(void) {
    char directory[] = "temp/defer-lsp-packages-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *provider = effects_lsp_pack(directory, "defer_provider",
        "open module cleanup.provider;open func raise<T:throw>(x:T){throw x;}", NULL);
    char *middle = effects_lsp_pack(directory, "defer_middle",
        "open module cleanup.middle;import cleanup.provider;open func relay<T:throw>(x:T){raise(x);}", provider);
    char *consumer = path_join(directory, "consumer"), *src = path_join(consumer, "src");
    char *path = path_join(src, "main.ff"), *manifest = path_join(consumer, "feng.fm");
    mkdir_p(src);
    char *config = dup_printf("[package]\nname: \"defer_consumer\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\n[dependencies]\ndefer_provider: \"%s\"\ndefer_middle: \"%s\"\n", provider, middle);
    write_text_file(manifest, config);
    const char *initial = "module cleanup.consumer;import cleanup.middle;func run(){defer{try relay(\"x\") catch e:string{}}}";
    const char *sources[] = {
        "module cleanup.consumer;import cleanup.middle;func run(){defer{try relay(true) catch e:string{}}}",
        "module cleanup.consumer;import cleanup.middle;func run(){defer{try relay(true) catch e:bool{}}}"};
    const char *types[] = {"bool", NULL};
    write_text_file(path, initial);
    char *uri = file_uri_from_path(path), *escaped = json_escape_text(initial);
    char *open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}", uri, escaped);
    const char *initialize = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}";
    const char *requests[] = {"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"};
    DeferLspEdits edits = {uri, sources, types, 2U};
    unsigned line, column;
    find_line_character(initial, "func run", 5U, &line, &column);
    char *ready = NULL;
    char *output = run_lsp_server_capture_after_position_ready_action(initialize, open, NULL,
        "textDocument/hover", uri, line, column, "func run()", defer_lsp_apply_edits,
        &edits, requests, 2U, &ready);
    ASSERT(ready != NULL);
    char *remove_error = NULL;
    ASSERT(feng_cli_project_remove_tree(directory, &remove_error));
    free(remove_error); free(output); free(ready); free(open); free(escaped); free(uri);
    free(config); free(manifest); free(path); free(src); free(consumer); free(middle); free(provider);
}
