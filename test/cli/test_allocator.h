#ifndef FENG_TEST_ALLOCATOR_H
#define FENG_TEST_ALLOCATOR_H

/* Run independent CLI, ABI, distribution and language-behavior coverage. */
static void test_allocator_selection(void) {
#if __has_feature(address_sanitizer)
    run_command_or_die("bash test/cli/allocator.sh --asan");
#else
    run_command_or_die("bash test/cli/allocator.sh");
#endif
    run_command_or_die("bash test/cli/allocator_release.sh");
}

/* Heap selection must not change the DAP's source stops or logical values. */
static void test_allocator_dap(void) {
    const char *fixture =
        "module allocator_debug;\n"
        "type Item { let number: int; }\n"
        "func main(args: string[]) {\n"
        "  let values: int[] = [3, 7];\n"
        "  let item = Item { number: 17 };\n"
        "  let ready = values[0] + item.number;\n"
        "  if ready != 20 { throw \"allocator/debug\"; } // breakpoint: allocator_ready\n"
        "}\n";
    /* ASan keeps system and the enclosing instrumentation. The ordinary phase
     * exercises both allocators; rejection of builtin + ASan is tested above. */
    unsigned allocator_count = 2U;
#if __has_feature(address_sanitizer)
    allocator_count = 1U;
#endif
    ThinLtoTestWorkspace work = thin_lto_test_workspace();
    char *project = thin_lto_test_project(&work, "allocator_debug", "bin", fixture, NULL);
    char *source = path_join(project, "src/main.ff");
    char *escaped_source = json_escape_text(source);
    char *binary = project_host_build_path(project, "bin/allocator_debug");
    char *root = getcwd(NULL, 0);
    ASSERT(root != NULL);
    char *backend = path_join(root, "build/toolchain/llvm/bin/lldb-dap");
    for (unsigned thin = 0; thin < 2; ++thin) {
        for (unsigned builtin = 0; builtin < allocator_count; ++builtin) {
            char *argv[] = {project, thin ? "--lto=thin" : "--lto=off",
                            builtin ? "--alloc=builtin" : "--alloc=system"};
            ASSERT(feng_cli_project_build_main("feng", 3, argv) == 0);
            thin_lto_test_debug_fingerprint(binary);
            char *log = dup_printf("%s/dap-%u-%u.log", work.root, thin, builtin);
            DapTestSession session;
            dap_test_start(&session, work.root, backend, log);
            free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\"}"));
            int launch = dap_test_launch_native(&session, binary, work.root, 0);
            free(dap_test_wait(&session, 0, "initialized", NULL));
            char *arguments = dup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}",
                escaped_source, dap_test_breakpoint_line(fixture, "allocator_ready"));
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
            dap_runtime_value(values, "ready", "20");
            response = dap_runtime_children(&session, dap_callable_find_variable(values, "values"), 2U);
            DapTestJson items = dap_test_json_get(dap_test_body(response), "variables");
            dap_runtime_value(items, "[0]", "3");
            dap_runtime_value(items, "[1]", "7");
            free(response);
            long object = dap_test_json_integer(dap_test_json_get(
                dap_callable_find_variable(values, "item"), "variablesReference"));
            ASSERT(object > 0);
            arguments = dup_printf("{\"variablesReference\":%ld}", object);
            response = dap_test_request(&session, "variables", arguments);
            free(arguments);
            dap_runtime_value(dap_test_json_get(dap_test_body(response), "variables"), "number", "17");
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
    }
    free(backend); free(root); free(binary); free(escaped_source); free(source); free(project);
    thin_lto_test_dispose(&work);
    printf("allocator DAP: %u allocators x off/thin source stops, arrays and objects passed\n", allocator_count);
}
#endif
