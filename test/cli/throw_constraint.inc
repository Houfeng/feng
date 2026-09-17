/* The LSP renders builtin constraints from source without inventing a type. */
static void test_lsp_throw_constraint_source(void) {
    const char *source = "module throw_tools;\n"
        "type Owner<E: throw> { let error: E; }\n"
        "func raise<E: throw>(error: E) { throw error; }\n";
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
            "textDocument/hover", needles[i], offsets[i], "E: throw");
        assert_lsp_test_response_contains(output, 2U, "E: throw");
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
        "open func raise<E:throw>(error:E){throw error;}\n"
        "open func relay<E:throw>(error:E){raise<E>(error);}\n"
        "@value open type Payload{open let message:string;open let code:i32;}\n"
        "open type Owner<E:throw>{open let error:E;open func raise(){throw self.error;}"
        "open func method<U:throw>(error:U){raise<U>(error);}"
        "open static func send<U:throw>(error:U){throw error;}}\n";
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
        "if count!=9{throw \"cross-package throw constraint failure\";}}\n";
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
        "textDocument/hover", "raise(1)", 1U, "E: throw");
    assert_lsp_test_response_contains(output, 2U, "E: throw");
    free(output);
    free(main_path);
    free(manifest);
    free(source_path);
    free(bundle);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
}
