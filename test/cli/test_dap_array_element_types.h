/* Independent array-element display regression uses existing DAP helpers. */
#ifndef FENG_TEST_DAP_ARRAY_ELEMENT_TYPES_H
#define FENG_TEST_DAP_ARRAY_ELEMENT_TYPES_H

/* The union displays exactly tag/payload, with the selected member intact. */
static void dap_array_debug_value(DapTestSession *session, DapTestJson variable,
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

/* Exercise local, object and array views with default Debug and explicit LTO. */
static void test_dap_array_element_types(void) {
    ThinLtoTestWorkspace work = thin_lto_test_workspace();
    char *provider_fixture = read_text_file("test/cli/fixtures/dap_array_element_types/provider.ff");
    char *provider = thin_lto_test_project(&work, "dap_array_dep", "lib", provider_fixture, NULL);
    char *provider_bundle = path_join(provider, "build/pkg/dap_array_dep-0.1.0.fb");
    char *bundle = path_join(work.root, "dap_array_dep.fb");
    char *pack_args[] = {provider};
    ASSERT(feng_cli_project_pack_main("feng", 1, pack_args) == 0);
    char *commands = read_text_file(work.log);
    ASSERT(count_logged_arguments(commands, "-fno-eliminate-unused-debug-types") == 0);
    free(commands);
    copy_file_or_die(provider_bundle, bundle);
    char *remove_error = NULL;
    ASSERT(feng_cli_project_remove_tree(provider, &remove_error));
    free(remove_error);
    char *fixture = read_text_file("test/cli/fixtures/dap_array_element_types/main.ff");
    char *project = thin_lto_test_project(&work, "union_slot_debug", "bin", fixture,
        "[dependencies]\ndap_array_dep: \"../dap_array_dep.fb\"\n");
    char *source = path_join(project, "src/main.ff");
    char *escaped_source = json_escape_text(source);
    char *binary = project_host_build_path(project, "bin/union_slot_debug");
    char *repo = getcwd(NULL, 0);
    ASSERT(repo != NULL);
    char *backend = path_join(repo, "build/toolchain/llvm/bin/lldb-dap");
    const char *modes[] = {"", "--lto=off", "--lto=thin"};
    for (size_t mode = 0U; mode < sizeof modes / sizeof *modes; ++mode) {
        char *argv[] = {project, (char *)modes[mode]};
        write_text_file(work.log, "");
        ASSERT(feng_cli_project_build_main("feng", mode == 0U ? 1 : 2, argv) == 0);
        commands = read_text_file(work.log);
        ASSERT(count_logged_arguments(commands, "-fno-eliminate-unused-debug-types") == 1);
        free(commands);
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
        dap_array_debug_value(&session, dap_callable_find_variable(values, "small"),
            "union_slot_debug.Choice<i32>", 1U, "7");
        dap_array_debug_value(&session, dap_callable_find_variable(values, "large"),
            "union_slot_debug.Choice<i64>", 1U, "9000000001");
        long holder = dap_test_json_integer(dap_test_json_get(
            dap_callable_find_variable(values, "holder"), "variablesReference"));
        arguments = dup_printf("{\"variablesReference\":%ld}", holder);
        response = dap_test_request(&session, "variables", arguments);
        free(arguments);
        dap_array_debug_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), "value"),
            "union_slot_debug.Choice<i32>", 1U, "7");
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "items"), 2U);
        DapTestJson items = dap_test_json_get(dap_test_body(response), "variables");
        dap_array_debug_value(&session, dap_callable_find_variable(items, "[0]"),
            "union_slot_debug.Choice<i32>", 1U, "7");
        dap_array_debug_value(&session, dap_callable_find_variable(items, "[1]"),
            "union_slot_debug.Choice<i32>", 1U, "9");
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "onlyArray"), 2U);
        items = dap_test_json_get(dap_test_body(response), "variables");
        dap_array_debug_value(&session, dap_callable_find_variable(items, "[0]"),
            "union_slot_debug.Choice<u16>", 1U, "42");
        dap_array_debug_value(&session, dap_callable_find_variable(items, "[1]"),
            "union_slot_debug.Choice<u16>", 1U, "65530");
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "nested"), 2U);
        DapTestJson rows = dap_test_json_get(dap_test_body(response), "variables");
        char *row = dap_runtime_children(&session, dap_callable_find_variable(rows, "[1]"), 1U);
        dap_array_debug_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(row), "variables"), "[0]"),
            "union_slot_debug.Choice<i32>", 1U, "11");
        free(row); free(response);
        free(dap_runtime_children(&session, dap_callable_find_variable(values, "empty"), 0U));
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "defaults"), 2U);
        dap_array_debug_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), "[1]"),
            "union_slot_debug.Choice<i32>", 0U, NULL);
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "writable"), 2U);
        dap_array_debug_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), "[1]"),
            "union_slot_debug.Choice<i32>", 1U, "15");
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "pairs"), 2U);
        items = dap_test_json_get(dap_test_body(response), "variables");
        long pair = dap_test_json_integer(dap_test_json_get(
            dap_callable_find_variable(items, "[1]"), "variablesReference"));
        ASSERT(pair > 0);
        arguments = dup_printf("{\"variablesReference\":%ld}", pair);
        row = dap_test_request(&session, "variables", arguments);
        free(arguments);
        DapTestJson pair_fields = dap_test_json_get(dap_test_body(row), "variables");
        dap_runtime_value(pair_fields, "first", "3");
        dap_runtime_value(pair_fields, "second", "4");
        free(row); free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "states"), 2U);
        items = dap_test_json_get(dap_test_body(response), "variables");
        dap_runtime_value(items, "[0]", "3");
        dap_runtime_value(items, "[1]", "7");
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "holders"), 2U);
        row = dap_runtime_children(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), "[1]"), 1U);
        dap_array_debug_value(&session, dap_callable_find_variable(
            dap_test_json_get(dap_test_body(row), "variables"), "value"),
            "union_slot_debug.Choice<i32>", 1U, "23");
        free(row); free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "imported"), 2U);
        items = dap_test_json_get(dap_test_body(response), "variables");
        dap_array_debug_value(&session, dap_callable_find_variable(items, "[0]"),
            "dap_array_dep.RemoteChoice", 0U, "21");
        dap_array_debug_value(&session, dap_callable_find_variable(items, "[1]"),
            "dap_array_dep.RemoteChoice", 1U, "9000000031");
        free(response);
        response = dap_runtime_children(&session, dap_callable_find_variable(values, "importedPairs"), 2U);
        pair = dap_test_json_integer(dap_test_json_get(dap_callable_find_variable(
            dap_test_json_get(dap_test_body(response), "variables"), "[1]"), "variablesReference"));
        ASSERT(pair > 0);
        arguments = dup_printf("{\"variablesReference\":%ld}", pair);
        row = dap_test_request(&session, "variables", arguments);
        free(arguments);
        pair_fields = dap_test_json_get(dap_test_body(row), "variables");
        dap_runtime_value(pair_fields, "first", "41");
        dap_runtime_value(pair_fields, "second", "42");
        free(row); free(response); free(locals);
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
    free(bundle); free(provider_bundle); free(provider); free(provider_fixture);
    thin_lto_test_dispose(&work);
    puts("DAP array types: local/imported unions, values, objects, enums and nested/empty arrays passed Debug default/off/thin");
}

#endif
