/* Require Feng naming while retaining native tag/payload expansion and member values. */
static void dap_union_expect_value(DapTestSession *session, DapTestJson value,
                                    const char *key, const char *type, const char *integer) {
    long reference = dap_test_json_integer(dap_test_json_get(value, "variablesReference"));
    char *arguments;
    char *response;
    DapTestJson variables;
    DapTestJson payload;

    ASSERT(dap_test_json_string_is(dap_test_json_get(value, key), type));
    ASSERT(dap_test_json_string_is(dap_test_json_get(value, "type"), type));
    ASSERT(reference > 0 && reference < 0x40000000);
    if (dap_test_json_get(value, "namedVariables").begin != NULL) {
        ASSERT(dap_test_json_integer(dap_test_json_get(value, "namedVariables")) == 2);
    }
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    response = dap_test_request(session, "variables", arguments);
    variables = dap_test_json_get(dap_test_body(response), "variables");
    ASSERT(dap_test_json_at(variables, 2U).begin == NULL);
    dap_callable_find_variable(variables, "tag");
    payload = dap_callable_find_variable(variables, "payload");
    reference = dap_test_json_integer(dap_test_json_get(payload, "variablesReference"));
    ASSERT(reference > 0 && reference < 0x40000000);
    free(arguments);
    free(response);
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    response = dap_test_request(session, "variables", arguments);
    variables = dap_test_json_get(dap_test_body(response), "variables");
    payload = dap_callable_find_variable(variables, "m0");
    dap_callable_find_variable(variables, "m1");
    if (integer != NULL) ASSERT(dap_test_json_string_is(dap_test_json_get(payload, "value"), integer));
    free(arguments);
    free(response);
}

/* Check native filtering, visible pagination, unrelated fields and stop-scoped reference cleanup. */
static void test_dap_union_display_protocol(void) {
    static const char *const kNames[] = {"choice", "generic", "leaf", "foreign"};
    static const char *const kTypes[] = {"union_test.Choice", "union_test.Result<i64>", "union_test.Choice", "ForeignStruct"};
    char template_path[] = "temp/feng_cli_union_protocol_XXXXXX";
    char *workspace = mkdtemp(template_path);
    char *root = workspace != NULL ? realpath(workspace, NULL) : NULL;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *log_path;
    char *script = read_text_file("test/debug/union_backend.js");
    char *arguments;
    char *response;
    char *error = NULL;
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping source;
    DapTestSession session;

    ASSERT(root != NULL);
    source_path = path_join(root, "main.ff");
    binary_path = path_join(root, "program");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(root, "backend");
    log_path = path_join(root, "dap.log");
    write_text_file(source_path, "module union_test;\nfunc main(args: string[]) {}\n");
    write_text_file(binary_path, "union display fixture");
    write_executable_text_file(backend_path, script);
    source = (FengCodegenMapingSourceMapping){source_path, "union_test", root};
    ASSERT(feng_codegen_maping_info_add_frame(&info, "backend_main", "main", FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    for (size_t index = 0U; index < sizeof(kNames) / sizeof(kNames[0]); ++index) {
        ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
            "backend_main", kNames[index], kNames[index], NULL, kTypes[index], FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    }
    ASSERT(feng_debug_write_fd(fd_path, binary_path, &source, 1U, &info, &error));
    dap_test_start(&session, root, backend_path, log_path);
    free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\"}"));
    arguments = dup_printf("{\"program\":\"%s\"}", binary_path);
    free(dap_test_request(&session, "launch", arguments));
    free(arguments);
    free(dap_test_request(&session, "stackTrace", "{\"threadId\":1}"));
    free(dap_test_request(&session, "scopes", "{\"frameId\":7}"));
    for (size_t index = 0U; index < 2U; ++index) {
        for (size_t hover = 0U; hover < 2U; ++hover) {
            arguments = dup_printf("{\"expression\":\"%s\",\"frameId\":7,\"context\":\"%s\"}",
                                    kNames[index], hover ? "hover" : "watch");
            response = dap_test_request(&session, "evaluate", arguments);
            dap_union_expect_value(&session, dap_test_body(response), "result", kTypes[index], "42");
            free(arguments);
            free(response);
        }
    }
    response = dap_test_request(&session, "variables", "{\"variablesReference\":101}");
    for (size_t index = 0U; index < 2U; ++index) {
        dap_union_expect_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), kNames[index]), "value", kTypes[index], "42");
    }
    dap_callable_expect_leaf(dap_callable_find_variable(dap_test_json_get(dap_test_body(response), "variables"),
        "leaf"), "value", kTypes[2], kTypes[2]);
    free(response);
    for (size_t start = 0U; start < 3U; ++start) {
        arguments = dup_printf("{\"variablesReference\":16,\"filter\":\"named\",\"start\":%zu,\"count\":1}", start);
        response = dap_test_request(&session, "variables", arguments);
        DapTestJson values = dap_test_json_get(dap_test_body(response), "variables");
        ASSERT(dap_test_json_at(values, 1U).begin == NULL);
        if (start < 2U) {
            ASSERT(dap_test_json_string_is(dap_test_json_get(dap_test_json_at(values, 0U), "name"),
                                            start == 0U ? "tag" : "payload"));
        } else ASSERT(dap_test_json_at(values, 0U).begin == NULL);
        free(arguments);
        free(response);
    }
    response = dap_test_request(&session, "variables", "{\"variablesReference\":16,\"filter\":\"indexed\"}");
    ASSERT(dap_test_json_at(dap_test_json_get(dap_test_body(response), "variables"), 0U).begin == NULL);
    free(response);
    response = dap_test_request(&session, "variables", "{\"variablesReference\":18}");
    dap_callable_find_variable(dap_test_json_get(dap_test_body(response), "variables"), "_fwd");
    free(response);
    {
        int sequence = dap_test_send(&session, "variables", "{\"variablesReference\":16,\"format\":{\"hex\":true}}");
        const char *cursor;
        DapTestJson success;
        free(dap_test_wait(&session, 0, "output", NULL));
        free(dap_test_request(&session, "continue", "{\"threadId\":1}"));
        response = dap_test_wait(&session, sequence, NULL, NULL);
        cursor = response;
        success = dap_test_json_get(dap_test_json_next(&cursor), "success");
        ASSERT(success.end - success.begin == 5 && memcmp(success.begin, "false", 5U) == 0);
        free(response);
    }
    free(dap_test_request(&session, "scopes", "{\"frameId\":7}"));
    response = dap_test_request(&session, "variables", "{\"variablesReference\":16}");
    dap_callable_find_variable(dap_test_json_get(dap_test_body(response), "variables"), "_fwd");
    free(response);
    dap_test_finish(&session);
    printf("dap union protocol: Feng names, native payloads, paging and stale references verified\n");
    feng_codegen_maping_info_dispose(&info);
    free(script);
    free(source_path);
    free(binary_path);
    free(fd_path);
    free(backend_path);
    free(log_path);
    free(root);
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
}

/* Verify primitive/reference alternatives, generic unions and object fields using real LLDB. */
static void test_dap_union_display_real_backend(void) {
    static const char *const kNames[] = {"number", "text", "result"};
    static const char *const kIntegers[] = {"42", NULL, "7"};
    const char *types[3] = {0};
    char template_path[] = "temp/feng_cli_union_real_XXXXXX";
    char *workspace = mkdtemp(template_path);
    char *root = workspace != NULL ? realpath(workspace, NULL) : NULL;
    char *repo = getcwd(NULL, 0);
    char *source_dir;
    char *source_path;
    char *manifest_path;
    char *binary_path;
    char *backend_path;
    char *log_path;
    char *fixture = read_text_file("test/debug/union_debug_values.ff");
    char *text;
    char *arguments;
    char *response;
    char *locals;
    char *error = NULL;
    long thread_id;
    long frame_id;
    long reference = 0;
    int launch_seq;
    FengDebugArtifact artifact = {0};
    DapTestSession session;

    ASSERT(root != NULL && repo != NULL);
    source_dir = path_join(root, "src");
    source_path = path_join(source_dir, "main.ff");
    manifest_path = path_join(root, "feng.fm");
    binary_path = project_host_build_path(root, "bin/union_debug_values");
    backend_path = path_join(repo, "build/toolchain/llvm/bin/lldb-dap");
    log_path = path_join(root, "dap.log");
    text = dup_printf("[package]\nname: \"union_debug_values\"\nversion: \"0.1.0\"\n"
        "target: \"bin\"\nsrc: \"src/\"\nout: \"build/\"\n[dependencies]\nstd: \"%s/std/std\"\n", repo);
    mkdir_p(source_dir);
    write_text_file(source_path, fixture);
    write_text_file(manifest_path, text);
    free(text);
    {
        char *argv[] = {root};
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }
    text = dup_printf("%s.fd", binary_path);
    ASSERT(feng_debug_read_fd(text, &artifact, &error));
    free(text);
    for (size_t index = 0U; index < 3U; ++index) {
        for (size_t record = 0U; record < artifact.info.variable_count; ++record) {
            if (strcmp(artifact.info.variables[record].display_name, kNames[index]) == 0) {
                types[index] = artifact.info.variables[record].display_type;
                break;
            }
        }
        ASSERT(types[index] != NULL && strstr(types[index], "union_debug_values.") == types[index]);
    }
    dap_test_start(&session, root, backend_path, log_path);
    free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\"}"));
    arguments = dup_printf("{\"program\":\"%s\"}", binary_path);
    launch_seq = dap_test_send(&session, "launch", arguments);
    free(arguments);
    free(dap_test_wait(&session, 0, "initialized", NULL));
    arguments = dup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}",
                            source_path, dap_test_breakpoint_line(fixture, "ready"));
    response = dap_test_request(&session, "setBreakpoints", arguments);
    dap_test_json_require_true(dap_test_json_get(dap_test_json_at(
        dap_test_json_get(dap_test_body(response), "breakpoints"), 0U), "verified"));
    free(arguments);
    free(response);
    free(dap_test_request(&session, "configurationDone", "{}"));
    free(dap_test_response(&session, launch_seq));
    response = dap_test_wait(&session, 0, "stopped", "terminated");
    thread_id = dap_test_json_integer(dap_test_json_get(dap_test_body(response), "threadId"));
    free(response);
    arguments = dup_printf("{\"threadId\":%ld}", thread_id);
    response = dap_test_request(&session, "stackTrace", arguments);
    free(arguments);
    frame_id = dap_test_json_integer(dap_test_json_get(dap_test_json_at(
        dap_test_json_get(dap_test_body(response), "stackFrames"), 0U), "id"));
    free(response);
    arguments = dup_printf("{\"frameId\":%ld}", frame_id);
    response = dap_test_request(&session, "scopes", arguments);
    free(arguments);
    for (size_t index = 0U;; ++index) {
        DapTestJson scope = dap_test_json_at(dap_test_json_get(dap_test_body(response), "scopes"), index);
        if (scope.begin == NULL) break;
        if (dap_test_json_string_is(dap_test_json_get(scope, "name"), "Locals")) {
            reference = dap_test_json_integer(dap_test_json_get(scope, "variablesReference"));
        }
    }
    ASSERT(reference > 0);
    free(response);
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    locals = dap_test_request(&session, "variables", arguments);
    free(arguments);
    for (size_t index = 0U; index < 3U; ++index) {
        dap_union_expect_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(locals), "variables"), kNames[index]), "value", types[index], kIntegers[index]);
        for (size_t hover = 0U; hover < 2U; ++hover) {
            arguments = dup_printf("{\"expression\":\"%s\",\"frameId\":%ld,\"context\":\"%s\"}",
                                    kNames[index], frame_id, hover ? "hover" : "watch");
            response = dap_test_request(&session, "evaluate", arguments);
            dap_union_expect_value(&session, dap_test_body(response), "result", types[index], kIntegers[index]);
            free(arguments);
            free(response);
        }
    }
    reference = dap_test_json_integer(dap_test_json_get(dap_callable_find_variable(
        dap_test_json_get(dap_test_body(locals), "variables"), "holder"), "variablesReference"));
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    response = dap_test_request(&session, "variables", arguments);
    dap_union_expect_value(&session, dap_callable_find_variable(
        dap_test_json_get(dap_test_body(response), "variables"), "choice"), "value", types[0], "42");
    dap_union_expect_value(&session, dap_callable_find_variable(
        dap_test_json_get(dap_test_body(response), "variables"), "result"), "value", types[2], "7");
    free(arguments);
    free(response);
    free(locals);
    arguments = dup_printf("{\"threadId\":%ld}", thread_id);
    free(dap_test_request(&session, "continue", arguments));
    free(arguments);
    free(dap_test_wait(&session, 0, "terminated", "stopped"));
    response = dap_test_wait(&session, 0, "exited", NULL);
    ASSERT(dap_test_json_integer(dap_test_json_get(dap_test_body(response), "exitCode")) == 0);
    free(response);
    dap_test_finish(&session);
    printf("dap union values: Feng names, generic unions, object fields and watch/hover verified\n");
    feng_debug_artifact_dispose(&artifact);
    free(source_dir);
    free(source_path);
    free(manifest_path);
    free(binary_path);
    free(backend_path);
    free(log_path);
    free(fixture);
    free(repo);
    free(root);
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
}
