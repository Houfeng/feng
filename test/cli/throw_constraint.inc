/* The LSP renders ordinary root constraints from source declarations. */
static void test_lsp_throw_constraint_source(void) {
    const char *source = "open module throw_tools;\n"
        "type Owner<E: Throwable> { let error: E; }\n"
        "func raise<E: Throwable>(error: E) { throw error; }\n"
        THROW_TEST_ROOT;
    const char *initialize = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char workspace[] = "temp/throw-lsp-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *path = path_join(workspace, "main.ff");
    write_text_file(path, source);
    const char *needles[] = {"Owner<E", "raise<E", "error: E)"};
    const size_t offsets[] = {1U, 1U, 7U};
    for (size_t i = 0U; i < 3U; ++i) {
        char *output = capture_lsp_position_response_at_path(path, source, initialize,
            "textDocument/hover", needles[i], offsets[i], "E: Throwable");
        assert_lsp_test_response_contains(output, 2U, "E: Throwable");
        free(output);
    }
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    free(path);
}

/* Native FB consumption has no access to the producer's source or AST. */
static void test_throw_constraint_source_hidden_bundle(void) {
    const char *provider = "open module throw_package;\n"
        "open func raise<E:Throwable>(error:E){throw error;}\n"
        "open func relay<E:Throwable>(error:E){raise<E>(error);}\n"
        "@value open type Payload{open let message:string;open let code:i32;}\n"
        "open type Owner<E:Throwable>{open let error:E;open func raise(){throw self.error;}"
        "open func method<U:Throwable>(error:U){raise<U>(error);}"
        "open static func send<U:Throwable>(error:U){throw error;}}\n"
        THROW_TEST_ROOT
        "open fit Payload:Throwable{}";
    const char *consumer = "module throw_consumer;import throw_package;\n"
        "spec Action<T>(x:T):void;\n"
        "type Local(i32,string);\n"
        "func main(args:string[]){var count:i32=0;"
        "try raise<i32>(7) catch e:i32{if e==7{count+=1;}}"
        "try raise(29) catch e:int{if e==29{count+=1;}}"
        "try relay(\"hello\") catch e:string{if e==\"hello\"{count+=1;}}"
        "try relay(Payload{message:\"provider\",code:11}) catch e:Payload{if e.code==11&&e.message==\"provider\"{count+=1;}}"
        "let local:Local=(13,\"local\");try relay(local) catch e:Local{if e.item1==13&&e.item2==\"local\"{count+=1;}}"
        "let owner=Owner<i32>{error:17};try owner.raise() catch e:i32{if e==17{count+=1;}}"
        "try owner.method<string>(\"method\") catch e:string{if e==\"method\"{count+=1;}}"
        "try Owner<string>.send<i32>(19) catch e:i32{if e==19{count+=1;}}"
        "let call:Action<i32> = raise<i32>;try call(23) catch e:i32{if e==23{count+=1;}}"
        "if count!=9{throw \"cross-package throw constraint failure\";}}\n"
        "fit Local:Throwable{}";
    char workspace[] = "temp/throw-bundle-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *bundle = build_single_source_package_bundle(workspace, "throw_package", provider);
    char *source_path = path_join(workspace, "dep/src/dep.ff");
    ASSERT(unlink(source_path) == 0);
    compile_consumer_with_package_and_expect_stdout(workspace, bundle, consumer, "throw_bundle", "");

    /* Imported hover must reconstruct the bound from persisted symbols. */
    char *manifest = path_join(workspace, "main/feng.fm");
    write_text_file(manifest, "[package]\nname: \"throw_consumer\"\nversion: \"0.1.0\"\n"
        "target: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n\n"
        "[dependencies]\nthrow_package: \"../throw_package.fb\"\n");
    char *main_path = path_join(workspace, "main/src/hover.ff");
    const char *hover_source = "module throw_consumer;import throw_package;\nfunc use(){raise(1);}\n";
    write_text_file(main_path, hover_source);
    char *output = capture_lsp_position_response_at_path(main_path, hover_source,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}",
        "textDocument/hover", "raise(1)", 1U, "E: Throwable");
    assert_lsp_test_response_contains(output, 2U, "E: Throwable");
    free(output);
    free(main_path);
    free(manifest);
    free(source_path);
    free(bundle);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
}

/* Root behavior must remain runnable after the producer's source is removed. */
static void test_throwable_source_hidden_bundle(void) {
    const char *provider =
        "open module throwable_package;"
        "@throwable open spec Root{func code():i32;}"
        "var released:i32=0;open func drops():i32{return released;}"
        "open type Resource{open let number:i32;open let tracked:bool;"
        "func ~Resource(){if self.tracked{released+=1;}}}"
        "open type Error<T>:Root{open let value:T;open let number:i32;"
        "func code():i32{return self.number;}}"
        "@value open type Value<T>:Root{open let value:T;open let number:i32;"
        "func code():i32{return self.number;}}"
        "open func raise<E:Root>(value:E){throw value;}"
        "func keep<E>(value:E):E{return value;}"
        "open func save<E:Root>(value:E):Root{"
        "try raise<E>(keep<E>(value)) catch error:Root{return error;}throw value;}"
        "open func openReference<T>(value:Error<T>){throw value;}"
        "open func openValue<T>(value:Value<T>){throw value;}"
        "open type Owner<T>{open let reference:Error<T>;open let value:Value<T>;"
        "open func ref(){throw self.reference;}open func val(){throw self.value;}"
        "open func method<U>(value:Value<U>){throw value;}}"
        "@value open type ValueOwner<T>{open let value:Value<T>;"
        "open func val(){throw self.value;}}";
    const char *consumer =
        "module throwable_consumer;import throwable_package;"
        "func verify(ok:bool){if !ok{throw Error<i32>{value:0,number:-1};}}"
        "func localReference<T>(value:Error<T>){throw value;}"
        "func localValue<T>(value:Value<T>){throw value;}"
        "func main(args:string[]){var caught:i32=0;"
        "{let saved=save<Value<Resource>>(Value<Resource>{"
        "value:Resource{number:7,tracked:true},number:37});"
        "verify(saved.code()==37&&drops()==0);"
        "try raise<Root>(saved) catch error:Value<Resource>{"
        "verify(error.value.number==7&&error.number==37);caught+=1;}"
        "try (try raise<Root>(saved) catch root:Root{verify(root.code()==37);throw root;})"
        "catch error:Value<Resource>{verify(error.value.number==7);caught+=1;}"
        "verify(drops()==0);}verify(drops()==1);"
        "{let saved=save<Error<Resource>>(Error<Resource>{"
        "value:Resource{number:11,tracked:true},number:41});"
        "verify(saved.code()==41&&drops()==1);"
        "try raise<Root>(saved) catch error:Error<Resource>{"
        "verify(error.value.number==11);caught+=1;}}verify(drops()==2);"
        "let reference=Error<string>{value:\"reference\",number:43};"
        "let value=Value<string>{value:\"aggregate\",number:47};"
        "try openReference(reference) catch root:Root{verify(root.code()==43);caught+=1;}"
        "try openValue(value) catch error:Value<string>{verify(error.value==\"aggregate\");caught+=1;}"
        "try localReference(reference) catch error:Error<string>{verify(error==reference);caught+=1;}"
        "try localValue(value) catch root:Root{verify(root.code()==47);caught+=1;}"
        "let owner=Owner<string>{reference:reference,value:value};"
        "try owner.ref() catch root:Root{verify(root.code()==43);caught+=1;}"
        "try owner.val() catch error:Value<string>{verify(error.value==\"aggregate\");caught+=1;}"
        "try owner.method<i32>(Value<i32>{value:53,number:59}) catch root:Root{"
        "verify(root.code()==59);caught+=1;}"
        "let aggregate=ValueOwner<string>{value:value};"
        "try aggregate.val() catch root:Root{verify(root.code()==47);caught+=1;}"
        "verify(caught==11&&drops()==2);}";
    char workspace[] = "temp/throwable-bundle-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *bundle = build_single_source_package_bundle(workspace, "throwable_package", provider);
    char *provider_path = path_join(workspace, "dep/src/dep.ff");
    ASSERT(unlink(provider_path) == 0);
    compile_consumer_with_package_and_expect_stdout(workspace, bundle, consumer, "throwable_bundle", "");
    char *source = path_join(workspace, "main/src/main.ff");
    char *out = path_join(workspace, "main/release");
    char *out_option = make_out_option(out), *package_option = make_pkg_option(bundle);
    char *argv[] = {source, "--target=bin", "--name=throwable_bundle", out_option, package_option, "--release"};
    ASSERT(run_direct_for_host(6, argv) == 0);
    char *binary = path_join(out, "bin/throwable_bundle");
    char *output = run_binary_capture_stdout_or_die(binary);
    ASSERT(strcmp(output, "") == 0);
    free(output); free(binary); free(out_option); free(package_option);
    free(out); free(source); free(provider_path); free(bundle);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    puts("throwable source-hidden FB: root witnesses, open payloads and escaping ownership passed Debug/Release");
}

/* Two source modules in one live project exercise root-cache replacement. */
typedef struct ThrowableLspEdits {
    const char *provider_uri;
    const char *consumer_uri;
} ThrowableLspEdits;

/* Save and observe this document's full semantic result, ignoring other URIs. */
static void throwable_lsp_save_wait(FILE *input, int output_fd, const char *uri,
    const char *expected, unsigned first_id) {
    char *save = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}", uri);
    write_lsp_message(input, save);
    free(save);
    size_t empty = 0U;
    bool complete = false;
    for (unsigned attempt = 0U; attempt < 240U && !complete; ++attempt) {
        unsigned id = first_id + attempt;
        char *request = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":%u,"
            "\"method\":\"feng/throwableBarrier\",\"params\":null}", id);
        char *output = send_lsp_test_request_and_wait(input, output_fd, request, id);
        const char *cursor = output, *header;
        while ((header = strstr(cursor, "\r\n\r\n")) != NULL) {
            cursor = header + 4;
            DapTestJson message = dap_test_json_next(&cursor);
            if (!dap_test_json_string_is(dap_test_json_get(message, "method"),
                    "textDocument/publishDiagnostics")) continue;
            DapTestJson params = dap_test_json_get(message, "params");
            if (!dap_test_json_string_is(dap_test_json_get(params, "uri"), uri)) continue;
            DapTestJson diagnostic = dap_test_json_at(dap_test_json_get(params, "diagnostics"), 0U);
            if (diagnostic.begin == NULL) {
                ++empty;
            } else {
                char *text = strndup(diagnostic.begin, (size_t)(diagnostic.end - diagnostic.begin));
                ASSERT(text != NULL);
                if (expected == NULL || strstr(text, expected) == NULL)
                    fprintf(stderr, "throwable LSP expected %s for %s\n%s\n",
                        expected != NULL ? expected : "no diagnostic", uri, text);
                ASSERT(expected != NULL && strstr(text, expected) != NULL);
                complete = true;
                free(text);
            }
        }
        if (expected == NULL && empty >= 2U) complete = true;
        if (attempt == 239U && !complete)
            fprintf(stderr, "throwable LSP did not publish %s (empty=%zu)\n%s\n", uri, empty, output);
        free(output); free(request);
        if (!complete) usleep(25000U);
    }
    ASSERT(complete);
}

/* Change only source text through the real versioned document protocol. */
static void throwable_lsp_change(FILE *input, const char *uri, const char *source, unsigned version) {
    char *escaped = json_escape_text(source);
    char *message = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%u},"
        "\"contentChanges\":[{\"text\":\"%s\"}]}}", uri, version, escaped);
    write_lsp_message(input, message);
    free(message); free(escaped);
}

/* Marker-only edits keep declaration names, member shapes and positions stable. */
static void throwable_lsp_apply_edits(FILE *input, int output_fd, void *user) {
    const ThrowableLspEdits *edits = user;
    static const char *const providers[] = {
        "open module root_edit;\n           open spec First{}\n           open spec Second{}\n",
        "open module root_edit;\n           open spec First{}\n@throwable open spec Second{}\n",
        "open module root_edit;\n           open spec First{}\n@throwable open spec Second{}\n",
        "open module root_edit;\n@throwable open spec First{}\n           open spec Second{}\n",
        "open module root_edit;\n@throwable open spec First{}\n           open spec Second{}\n",
    };
    const char *types[] = {"First", "First", "Second", "Second", "First"};
    const char *errors[] = {"throw requires a registered exception root",
        "does not satisfy the exception root", NULL, "does not satisfy the exception root", NULL};
    for (unsigned phase = 0U; phase < 5U; ++phase) {
        throwable_lsp_change(input, edits->provider_uri, providers[phase], phase + 2U);
        throwable_lsp_save_wait(input, output_fd, edits->provider_uri, NULL, 10000U + phase * 1000U);
        char *consumer = dup_printf("module root_client;import root_edit;func run(value:%s){throw value;}", types[phase]);
        throwable_lsp_change(input, edits->consumer_uri, consumer, phase + 2U);
        throwable_lsp_save_wait(input, output_fd, edits->consumer_uri, errors[phase], 10500U + phase * 1000U);
        free(consumer);
    }
}

/* Cached source facts must follow removal, movement and restoration of the marker. */
static void test_throwable_lsp_root_lifecycle(void) {
    char directory[] = "temp/throwable-lsp-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *src = path_join(directory, "src");
    mkdir_p(src);
    char *provider_path = path_join(src, "api.ff"), *consumer_path = path_join(src, "main.ff");
    char *manifest = path_join(directory, "feng.fm");
    write_text_file(manifest, "[package]\nname: \"root_lifecycle\"\nversion: \"0.1.0\"\n"
        "target: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n");
    const char *provider = "open module root_edit;\n@throwable open spec First{}\n           open spec Second{}\n";
    const char *consumer = "module root_client;import root_edit;func run(value:First){throw value;}";
    write_text_file(provider_path, provider); write_text_file(consumer_path, consumer);
    char *provider_uri = file_uri_from_path(provider_path), *consumer_uri = file_uri_from_path(consumer_path);
    char *escaped_provider = json_escape_text(provider), *escaped_consumer = json_escape_text(consumer);
    char *open_provider = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
        provider_uri, escaped_provider);
    char *open_consumer = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
        consumer_uri, escaped_consumer);
    const char *initialize = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}";
    const char *requests[] = {"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"};
    ThrowableLspEdits edits = {provider_uri, consumer_uri};
    unsigned line, column;
    find_line_character(consumer, "func run", 5U, &line, &column);
    char *output = run_lsp_server_capture_after_position_ready_action(initialize, open_consumer, open_provider,
        "textDocument/hover", consumer_uri, line, column, "func run", throwable_lsp_apply_edits,
        &edits, requests, 2U, NULL);
    free(output); free(open_provider); free(open_consumer); free(escaped_provider); free(escaped_consumer);
    free(provider_uri); free(consumer_uri); free(provider_path); free(consumer_path);
    free(manifest); free(src);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(directory, &error));
    free(error);
    puts("throwable LSP root cache: marker removal, movement and restoration passed in one live session");
}
