/* New integration cases reuse test_cli.c's existing compiler and DAP helpers. */
#ifndef FENG_TEST_UNION_SLOT_LAYOUT_H
#define FENG_TEST_UNION_SLOT_LAYOUT_H

/* A source-hidden provider must supply closed descriptors for every consumer. */
static void test_union_slot_package_lifetime(void) {
    char workspace[] = "temp/union-slot-package-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *provider = path_join(workspace, "provider.ff");
    char *consumer = path_join(workspace, "consumer.ff");
    copy_file_or_die("test/cli/fixtures/union_slot_layout/main.ff", consumer);
    const char *threshold = getenv("FENG_GC_THRESHOLD");
    char *saved_threshold = threshold != NULL ? dup_cstr(threshold) : NULL;
    ASSERT(setenv("FENG_GC_THRESHOLD", "1", 1) == 0);
    for (int producer_release = 0; producer_release < 2; ++producer_release) {
        copy_file_or_die("fcts/fcts_lib/src/test/lib_union_slot_layout.ff", provider);
        char *out = path_join(workspace, producer_release ? "provider-release" : "provider-debug");
        native_exception_build(provider, out, "union_slot_provider", "lib", NULL, producer_release != 0);
        char *library = host_static_library_output_path(out, "union_slot_provider");
        char *modules = path_join(out, "mod");
        char *bundle = path_join(out, "union_slot_provider.fb");
        write_library_bundle_or_die(bundle, "union_slot_provider", "0.1.0", library, modules);
        ASSERT(unlink(provider) == 0);
        for (int release = 0; release < 2; ++release) {
            char *app = dup_printf("%s/app-%d-%d", workspace, producer_release, release);
            native_exception_build(consumer, app, "union_slot_consumer", "bin", bundle, release != 0);
            char *binary = path_join(app, "bin/union_slot_consumer");
            char *output = run_binary_capture_stdout_or_die(binary);
            ASSERT(strcmp(output, "") == 0);
            free(output); free(binary); free(app);
        }
        free(bundle); free(modules); free(library); free(out);
    }
    if (saved_threshold != NULL) ASSERT(setenv("FENG_GC_THRESHOLD", saved_threshold, 1) == 0);
    else ASSERT(unsetenv("FENG_GC_THRESHOLD") == 0);
    free(saved_threshold);
    free(consumer); free(provider);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    puts("union slots: source-hidden generic layouts, known cycles and resurrection passed four build pairings");
}

/* The union displays exactly tag/payload, with the selected member intact. */
static void union_slot_debug_value(DapTestSession *session, DapTestJson variable,
                                    const char *type, unsigned tag, const char *expected) {
    ASSERT(dap_test_json_string_is(dap_test_json_get(variable, "type"), type));
    long reference = dap_test_json_integer(dap_test_json_get(variable, "variablesReference"));
    ASSERT(reference > 0);
    char *arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    char *response = dap_test_request(session, "variables", arguments);
    free(arguments);
    DapTestJson fields = dap_test_json_get(dap_test_body(response), "variables");
    ASSERT(dap_test_json_at(fields, 2U).begin == NULL);
    char *tag_text = dup_printf("%u", tag);
    dap_runtime_value(fields, "tag", tag_text);
    free(tag_text);
    DapTestJson payload = dap_callable_find_variable(fields, "payload");
    reference = dap_test_json_integer(dap_test_json_get(payload, "variablesReference"));
    ASSERT(reference > 0);
    free(response);
    arguments = dup_printf("{\"variablesReference\":%ld}", reference);
    response = dap_test_request(session, "variables", arguments);
    free(arguments);
    fields = dap_test_json_get(dap_test_body(response), "variables");
    char *member = dup_printf("m%u", tag);
    if (expected != NULL) dap_runtime_value(fields, member, expected);
    else ASSERT(dap_callable_find_variable(fields, member).begin != NULL);
    free(member);
    free(response);
}

/* Exercise local and object-field views with default Debug and explicit LTO. */
static void test_union_slot_debug_layout(void) {
    ThinLtoTestWorkspace work = thin_lto_test_workspace();
    char *fixture = read_text_file("test/cli/fixtures/union_slot_layout/debug.ff");
    char *project = thin_lto_test_project(&work, "union_slot_debug", "bin", fixture, NULL);
    char *source = path_join(project, "src/main.ff");
    char *escaped_source = json_escape_text(source);
    char *binary = project_host_build_path(project, "bin/union_slot_debug");
    char *repo = getcwd(NULL, 0);
    ASSERT(repo != NULL);
    char *backend = path_join(repo, "build/toolchain/llvm/bin/lldb-dap");
    const char *modes[] = {"", "--lto=thin"};
    for (size_t mode = 0U; mode < 2U; ++mode) {
        char *argv[] = {project, (char *)modes[mode]};
        ASSERT(feng_cli_project_build_main("feng", mode == 0U ? 1 : 2, argv) == 0);
        thin_lto_test_debug_fingerprint(binary);
        char *log = dup_printf("%s/dap-%zu.log", work.root, mode);
        DapTestSession session;
        dap_test_start(&session, work.root, backend, log);
        free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\"}"));
        int launch = dap_test_launch_native(&session, binary, work.root, 0);
        free(dap_test_wait(&session, 0, "initialized", NULL));
        char *arguments = dup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}",
            escaped_source, dap_test_breakpoint_line(fixture, "ready"));
        char *response = dap_test_request(&session, "setBreakpoints", arguments);
        free(arguments);
        dap_test_json_require_true(dap_test_json_get(dap_test_json_at(
            dap_test_json_get(dap_test_body(response), "breakpoints"), 0U), "verified"));
        free(response);
        free(dap_test_request(&session, "configurationDone", "{}"));
        free(dap_test_response(&session, launch));
        DapTestStepLocation location = dap_test_step_location(&session, "breakpoint", source, "main");
        arguments = dup_printf("{\"frameId\":%ld}", location.frame_id);
        response = dap_test_request(&session, "scopes", arguments);
        free(arguments);
        DapTestJson scopes = dap_test_json_get(dap_test_body(response), "scopes");
        long locals_reference = 0;
        for (size_t i = 0U;; ++i) {
            DapTestJson scope = dap_test_json_at(scopes, i);
            if (scope.begin == NULL) break;
            if (dap_test_json_string_is(dap_test_json_get(scope, "name"), "Locals"))
                locals_reference = dap_test_json_integer(dap_test_json_get(scope, "variablesReference"));
        }
        ASSERT(locals_reference > 0);
        free(response);
        arguments = dup_printf("{\"variablesReference\":%ld}", locals_reference);
        char *locals = dap_test_request(&session, "variables", arguments);
        free(arguments);
        DapTestJson values = dap_test_json_get(dap_test_body(locals), "variables");
        union_slot_debug_value(&session, dap_callable_find_variable(values, "small"),
            "union_slot_debug.Choice<i32>", 1U, "7");
        union_slot_debug_value(&session, dap_callable_find_variable(values, "large"),
            "union_slot_debug.Choice<i64>", 1U, "9000000001");
        long holder = dap_test_json_integer(dap_test_json_get(
            dap_callable_find_variable(values, "holder"), "variablesReference"));
        arguments = dup_printf("{\"variablesReference\":%ld}", holder);
        response = dap_test_request(&session, "variables", arguments);
        free(arguments);
        union_slot_debug_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), "value"),
            "union_slot_debug.Choice<i32>", 1U, "7");
        free(response);
        free(locals);
        arguments = dup_printf("{\"threadId\":%ld}", location.thread_id);
        free(dap_test_request(&session, "continue", arguments));
        free(arguments);
        free(dap_test_wait(&session, 0, "terminated", "stopped"));
        response = dap_test_wait(&session, 0, "exited", NULL);
        ASSERT(dap_test_json_integer(dap_test_json_get(dap_test_body(response), "exitCode")) == 0);
        free(response);
        dap_test_finish(&session);
        free(log);
    }
    free(backend); free(repo); free(binary); free(escaped_source); free(source); free(project); free(fixture);
    thin_lto_test_dispose(&work);
    puts("union slots DAP: locals and fields passed Debug default/thin");
}

#endif
