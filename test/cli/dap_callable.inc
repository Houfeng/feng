#include <ctype.h>

/* Find exactly one named variable without depending on backend enumeration order. */
static DapTestJson dap_callable_find_variable(DapTestJson variables, const char *name) {
    DapTestJson found = {0};
    for (size_t index = 0U;; ++index) {
        DapTestJson variable = dap_test_json_at(variables, index);
        if (variable.begin == NULL) break;
        if (dap_test_json_string_is(dap_test_json_get(variable, "name"), name)) {
            ASSERT(found.begin == NULL);
            found = variable;
        }
    }
    ASSERT(found.begin != NULL);
    return found;
}

/* Require a Feng type and a non-expandable address, without any native symbol suffix. */
static void dap_callable_expect_leaf(DapTestJson value, const char *key,
                                      const char *type, const char *expected) {
    DapTestJson text = dap_test_json_get(value, key);
    ASSERT(dap_test_json_string_is(dap_test_json_get(value, "type"), type));
    ASSERT(dap_test_json_integer(dap_test_json_get(value, "variablesReference")) == 0);
    if (expected != NULL) {
        ASSERT(dap_test_json_string_is(text, expected));
    } else {
        ASSERT(text.begin != NULL && text.end - text.begin > 4);
        ASSERT(memcmp(text.begin, "\"0x", 3U) == 0 && text.end[-1] == '"');
        for (const char *cursor = text.begin + 3; cursor < text.end - 1; ++cursor) {
            ASSERT(isxdigit((unsigned char)*cursor));
        }
    }
    {
        DapTestJson named = dap_test_json_get(value, "namedVariables");
        DapTestJson indexed = dap_test_json_get(value, "indexedVariables");
        if (named.begin != NULL) ASSERT(dap_test_json_integer(named) == 0);
        if (indexed.begin != NULL) ASSERT(dap_test_json_integer(indexed) == 0);
    }
}

/* Expand exactly the source-facing carrier members and return their values. */
static char *dap_carrier_expect_children(DapTestSession *session, DapTestJson value,
                                         const char *key, const char *type, bool spec,
                                         const char *first_expected, const char *second_expected) {
    long reference = dap_test_json_integer(dap_test_json_get(value, "variablesReference"));
    char *arguments;
    char *response;
    char *signature;
    DapTestJson children;
    DapTestJson first;
    DapTestJson second;

    ASSERT(dap_test_json_string_is(dap_test_json_get(value, key), type));
    ASSERT(dap_test_json_string_is(dap_test_json_get(value, "type"), type));
    ASSERT(reference >= 0x40000000);
    {
        DapTestJson named = dap_test_json_get(value, "namedVariables");
        DapTestJson indexed = dap_test_json_get(value, "indexedVariables");
        if (named.begin != NULL) ASSERT(dap_test_json_integer(named) == 2);
        if (indexed.begin != NULL) ASSERT(dap_test_json_integer(indexed) == 0);
    }
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    response = dap_test_request(session, "variables", arguments);
    children = dap_test_json_get(dap_test_body(response), "variables");
    ASSERT(dap_test_json_at(children, 2U).begin == NULL);
    first = dap_callable_find_variable(children, spec ? "subject" : "self");
    second = dap_callable_find_variable(children, spec ? "witness" : "invoke");
    dap_callable_expect_leaf(first, "value", "pointer", first_expected);
    dap_callable_expect_leaf(second, "value", spec ? "" : "pointer", spec ? "<shadow>" : second_expected);
    first = dap_test_json_get(first, "value");
    second = dap_test_json_get(second, "value");
    if (!spec && second_expected == NULL) ASSERT(!dap_test_json_string_is(second, "0x0"));
    signature = dup_printf("%.*s|%.*s", (int)(first.end - first.begin), first.begin,
                            (int)(second.end - second.begin), second.begin);
    free(arguments);
    free(response);
    return signature;
}

/* Cover null, failed and non-pointer results, plus a late reply after resume. */
static void test_dap_callable_display_protocol(void) {
    static const char *const kNames[] = {"normal", "empty", "failed", "leaf", "view", "emptyView", "indirect", "unionValue"};
    static const char *const kTypes[] = {"callable_test.Action", "callable_test.Action", "callable_test.Action", "callable_test.Action",
        "callable_test.View", "callable_test.View", "callable_test.Action", "callable_test.Choice"};
    static const char *const kFirst[] = {"0x3000", "0x0", "<unavailable>", "0x3000", "0x3000", "0x0"};
    static const char *const kSecond[] = {"0x2000", "0x0", "<unavailable>", "0x2000", "<shadow>", "<shadow>"};
    char template_path[] = "temp/feng_cli_callable_protocol_XXXXXX";
    char *workspace = mkdtemp(template_path);
    char *root = workspace != NULL ? realpath(workspace, NULL) : NULL;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *log_path;
    char *script = read_text_file("test/debug/callable_backend.js");
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
    write_text_file(source_path, "module callable_test;\nfunc main(args: string[]) {}\n");
    write_text_file(binary_path, "callable display fixture");
    write_executable_text_file(backend_path, script);
    source = (FengCodegenMapingSourceMapping){source_path, "callable_test", root};
    ASSERT(feng_codegen_maping_info_add_frame(&info, "backend_main", "main", FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    for (size_t index = 0U; index < sizeof(kNames) / sizeof(kNames[0]); ++index) {
        ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
            "backend_main", kNames[index], kNames[index], NULL, kTypes[index],
            FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    }
    ASSERT(feng_debug_write_fd(fd_path, binary_path, &source, 1U, &info, &error));
    dap_test_start(&session, root, backend_path, log_path);
    free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\"}"));
    arguments = dup_printf("{\"program\":\"%s\"}", binary_path);
    free(dap_test_request(&session, "launch", arguments));
    free(arguments);
    free(dap_test_request(&session, "stackTrace", "{\"threadId\":1}"));
    free(dap_test_request(&session, "scopes", "{\"frameId\":7}"));
    response = dap_test_request(&session, "variables", "{\"variablesReference\":101}");
    for (size_t index = 0U; index < sizeof(kFirst) / sizeof(kFirst[0]); ++index) {
        DapTestJson variable = dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), kNames[index]);
        free(dap_carrier_expect_children(&session, variable, "value", kTypes[index],
                                          index >= 4U, kFirst[index], kSecond[index]));
    }
    ASSERT(dap_test_json_integer(dap_test_json_get(dap_callable_find_variable(
        dap_test_json_get(dap_test_body(response), "variables"), "indirect"), "variablesReference")) == 14);
    ASSERT(dap_test_json_integer(dap_test_json_get(dap_callable_find_variable(
        dap_test_json_get(dap_test_body(response), "variables"), "unionValue"), "variablesReference")) == 16);
    free(response);
    for (size_t index = 0U; index < sizeof(kFirst) / sizeof(kFirst[0]); ++index) {
        for (size_t hover = 0U; hover < 2U; ++hover) {
            arguments = dup_printf("{\"expression\":\"%s\",\"frameId\":7,\"context\":\"%s\"}",
                                    kNames[index], hover ? "hover" : "watch");
            response = dap_test_request(&session, "evaluate", arguments);
            free(dap_carrier_expect_children(&session, dap_test_body(response), "result", kTypes[index],
                                              index >= 4U, kFirst[index], kSecond[index]));
            free(arguments);
            free(response);
        }
    }
    {
        int sequence = dap_test_send(&session, "evaluate", "{\"expression\":\"(normal)\",\"frameId\":7,\"context\":\"watch\"}");
        free(dap_test_wait(&session, 0, "output", NULL));
        free(dap_test_request(&session, "continue", "{\"threadId\":1}"));
        response = dap_test_response(&session, sequence);
        dap_callable_expect_leaf(dap_test_body(response), "result", "callable_test.Action", "<unavailable>");
        free(response);
    }
    dap_test_finish(&session);
    printf("dap carrier protocol: closure/spec children, union exclusion, nulls and stale replies verified\n");
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

/* Verify all callable producers and a nested field against the bundled LLDB. */
static void test_dap_callable_display_real_backend(void) {
    static const char *const kNames[] = {
        "parameter", "plain", "captured", "generic", "functionValue", "methodValue", "view", "combined"
    };
    char template_path[] = "temp/feng_cli_callable_real_XXXXXX";
    char *workspace = mkdtemp(template_path);
    char *root = workspace != NULL ? realpath(workspace, NULL) : NULL;
    char *repo = getcwd(NULL, 0);
    char *source_dir;
    char *source_path;
    char *manifest_path;
    char *binary_path;
    char *backend_path;
    char *log_path;
    char *fixture = read_text_file("test/debug/callable_debug_values.ff");
    char *text;
    char *arguments;
    char *response;
    char *locals;
    char *error = NULL;
    long thread_id;
    long frame_id;
    long reference = 0;
    int launch_seq;
    unsigned int line = dap_test_breakpoint_line(fixture, "ready");
    FengDebugArtifact artifact = {0};
    DapTestSession session;

    ASSERT(root != NULL && repo != NULL);
    source_dir = path_join(root, "src");
    source_path = path_join(source_dir, "main.ff");
    manifest_path = path_join(root, "feng.fm");
    binary_path = project_host_build_path(root, "bin/callable_debug_values");
    backend_path = path_join(repo, "build/toolchain/llvm/bin/lldb-dap");
    log_path = path_join(root, "dap.log");
    text = dup_printf("[package]\nname: \"callable_debug_values\"\nversion: \"0.1.0\"\n"
        "target: \"bin\"\nsrc: \"src/\"\nout: \"build/\"\n"
        "[dependencies]\nstd: \"%s/std/std\"\n", repo);
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
    dap_test_start(&session, root, backend_path, log_path);
    free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\"}"));
    arguments = dup_printf("{\"program\":\"%s\"}", binary_path);
    launch_seq = dap_test_send(&session, "launch", arguments);
    free(arguments);
    free(dap_test_wait(&session, 0, "initialized", NULL));
    arguments = dup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}", source_path, line);
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
    for (size_t index = 0U; index < sizeof(kNames) / sizeof(kNames[0]); ++index) {
        const char *display_type = NULL;
        DapTestJson variable = dap_callable_find_variable(
            dap_test_json_get(dap_test_body(locals), "variables"), kNames[index]);
        char *expected;
        for (size_t record = 0U; record < artifact.info.variable_count; ++record) {
            if (strcmp(artifact.info.variables[record].display_name, kNames[index]) == 0) {
                display_type = artifact.info.variables[record].display_type;
                break;
            }
        }
        ASSERT(display_type != NULL && strstr(display_type, "callable_debug_values.") == display_type);
        expected = dap_carrier_expect_children(&session, variable, "value", display_type, index >= 6U, NULL, NULL);
        for (size_t hover = 0U; hover < 2U; ++hover) {
            char *actual;
            arguments = dup_printf("{\"expression\":\"%s\",\"frameId\":%ld,\"context\":\"%s\"}",
                                    kNames[index], frame_id, hover ? "hover" : "watch");
            response = dap_test_request(&session, "evaluate", arguments);
            actual = dap_carrier_expect_children(&session, dap_test_body(response), "result", display_type,
                                                  index >= 6U, NULL, NULL);
            ASSERT(strcmp(actual, expected) == 0);
            free(actual);
            free(response);
            free(arguments);
        }
        free(expected);
    }
    reference = dap_test_json_integer(dap_test_json_get(dap_callable_find_variable(
        dap_test_json_get(dap_test_body(locals), "variables"), "choice"), "variablesReference"));
    ASSERT(reference > 0 && reference < 0x40000000);
    reference = dap_test_json_integer(dap_test_json_get(dap_callable_find_variable(
        dap_test_json_get(dap_test_body(locals), "variables"), "holder"), "variablesReference"));
    ASSERT(reference >= 0x40000000);
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    response = dap_test_request(&session, "variables", arguments);
    free(dap_carrier_expect_children(&session, dap_callable_find_variable(
        dap_test_json_get(dap_test_body(response), "variables"), "callback"),
        "value", "callable_debug_values.Action", false, NULL, NULL));
    free(dap_carrier_expect_children(&session, dap_callable_find_variable(
        dap_test_json_get(dap_test_body(response), "variables"), "view"),
        "value", "callable_debug_values.Applicable", true, NULL, NULL));
    free(arguments);
    free(response);
    free(locals);
    arguments = dup_printf("{\"expression\":\"calls\",\"frameId\":%ld,\"context\":\"watch\"}", frame_id);
    response = dap_test_request(&session, "evaluate", arguments);
    ASSERT(dap_test_json_string_is(dap_test_json_get(dap_test_body(response), "result"), "0"));
    free(response);
    free(arguments);
    arguments = dup_printf("{\"threadId\":%ld}", thread_id);
    free(dap_test_request(&session, "continue", arguments));
    free(arguments);
    free(dap_test_wait(&session, 0, "terminated", "stopped"));
    response = dap_test_wait(&session, 0, "exited", NULL);
    ASSERT(dap_test_json_integer(dap_test_json_get(dap_test_body(response), "exitCode")) == 0);
    free(response);
    dap_test_finish(&session);
    printf("dap carrier values: 6 callables, object/intersection specs, nested fields and read-only watch/hover verified\n");
    feng_debug_artifact_dispose(&artifact);
    free(source_dir);
    free(source_path);
    free(manifest_path);
    free(binary_path);
    free(backend_path);
    free(log_path);
    free(fixture);
    free(root);
    free(repo);
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
}
