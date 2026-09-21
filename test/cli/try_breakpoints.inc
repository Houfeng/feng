/* A source breakpoint belongs to evaluation, not generated EH scaffolding. */
static void test_dap_try_breakpoints_once_per_evaluation(void) {
    const char *source =
        "module try_breakpoints;\n"
        "@cdecl(\"libc\",\"exit\") extern func quit(code:i32):void;\n"
        "var seen:int=0;var sum:int=0;\n"
        "func identity<T>(value:T):T{return value;}\n"
        "func fail():void{throw \"handled\";}\n"
        "func plain():void{for var i=0;i<2;i+=1{\n"
        "  try identity(i) catch { // breakpoint: plain\n"
        "    quit(11);\n"
        "  }\n"
        "}}\n"
        "func result():void{for var i=0;i<2;i+=1{\n"
        "  let value=try identity(i) catch { // breakpoint: result\n"
        "    0;\n"
        "  };\n"
        "  sum+=value;\n"
        "}}\n"
        "func cleanup():void{defer{for var i=0;i<2;i+=1{\n"
        "  try identity(i) catch { // breakpoint: cleanup\n"
        "    quit(12);\n"
        "  }\n"
        "}}}\n"
        "func caught():void{for var i=0;i<2;i+=1{\n"
        "  try fail() catch { // breakpoint: caught\n"
        "    seen+=1; // breakpoint: handler\n"
        "  }\n"
        "}}\n"
        "func multiline():void{for var i=0;i<2;i+=1{\n"
        "  let chosen=try (if identity(i)==0 { // breakpoint: if_entry\n"
        "    identity(i); // breakpoint: if_then\n"
        "  } else {\n"
        "    identity(i); // breakpoint: if_else\n"
        "  }) catch {quit(14);0;};\n"
        "  let selected=try (match identity(i) { // breakpoint: match_entry\n"
        "    0 {\n"
        "      identity(i); // breakpoint: match_zero\n"
        "    }\n"
        "    2 {\n"
        "      identity(i); // breakpoint: match_unreached\n"
        "    }\n"
        "    else {\n"
        "      identity(i); // breakpoint: match_other\n"
        "    }\n"
        "  }) catch {quit(15);0;};\n"
        "  if chosen!=i || selected!=i{quit(16);}\n"
        "}}\n"
        "func main(args:string[]):void{plain();result();cleanup();caught();multiline();"
        "if sum!=1 || seen!=2{quit(13);}}\n";
    const char *markers[] = {"plain", "result", "cleanup", "caught", "handler",
        "if_entry", "if_then", "if_else", "match_entry", "match_zero", "match_other",
        "match_unreached"};
    const size_t marker_count = sizeof(markers) / sizeof(*markers);
    const size_t order[] = {0U, 0U, 1U, 1U, 2U, 2U, 3U, 4U, 3U, 4U,
        5U, 6U, 8U, 9U, 5U, 7U, 8U, 10U};
    char directory[] = "temp/dap-try-breakpoints-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *root = realpath(directory, NULL), *repo = getcwd(NULL, 0);
    ASSERT(root != NULL && repo != NULL);
    char *src = path_join(root, "src"), *path = path_join(src, "main.ff");
    char *manifest = path_join(root, "feng.fm"), *log = path_join(root, "dap.log");
    char *binary = project_host_build_path(root, "bin/try_breakpoints");
    char *backend = path_join(repo, "build/toolchain/llvm/bin/lldb-dap");
    mkdir_p(src);
    write_text_file(path, source);
    write_text_file(manifest, "[package]\nname: \"try_breakpoints\"\nversion: \"0.1.0\"\n"
        "target: \"bin\"\nsrc: \"src/\"\nout: \"build/\"\n");
    char *argv[] = {root};
    ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    char *source_json = json_escape_text(path), *binary_json = json_escape_text(binary);
    char *root_json = json_escape_text(root), *list = dup_cstr("");
    unsigned lines[sizeof(markers) / sizeof(*markers)];
    long ids[sizeof(markers) / sizeof(*markers)];
    for (size_t i = 0U; i < marker_count; ++i) {
        lines[i] = dap_test_breakpoint_line(source, markers[i]);
        char *next = dup_printf("%s%s{\"line\":%u}", list, i == 0U ? "" : ",", lines[i]);
        free(list);
        list = next;
    }
    DapTestSession session;
    dap_test_start(&session, root, backend, log);
    free(dap_test_request(&session, "initialize", "{\"adapterID\":\"feng\","
        "\"linesStartAt1\":true,\"columnsStartAt1\":true,\"pathFormat\":\"path\"}"));
    char *arguments = dup_printf("{\"program\":\"%s\",\"cwd\":\"%s\"}", binary_json, root_json);
    int launch = dap_test_send(&session, "launch", arguments);
    free(arguments);
    free(dap_test_wait(&session, 0, "initialized", NULL));
    arguments = dup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[%s]}", source_json, list);
    char *response = dap_test_request(&session, "setBreakpoints", arguments);
    free(arguments);
    DapTestJson breakpoints = dap_test_json_get(dap_test_body(response), "breakpoints");
    for (size_t i = 0U; i < marker_count; ++i) {
        DapTestJson item = dap_test_json_at(breakpoints, i);
        dap_test_json_require_true(dap_test_json_get(item, "verified"));
        ASSERT(dap_test_json_integer(dap_test_json_get(item, "line")) == lines[i]);
        ids[i] = dap_test_json_integer(dap_test_json_get(item, "id"));
    }
    free(response);
    free(dap_test_request(&session, "configurationDone", "{}"));
    free(dap_test_response(&session, launch));
    for (size_t i = 0U; i < sizeof(order) / sizeof(*order); ++i) {
        fprintf(session.log, "Checking try stop %zu (%s)\n", i, markers[order[i]]);
        response = dap_test_wait(&session, 0, "stopped", "terminated");
        const char *cursor = response;
        DapTestJson event = dap_test_json_next(&cursor), body = dap_test_json_get(event, "body");
        ASSERT(dap_test_json_string_is(dap_test_json_get(event, "event"), "stopped"));
        ASSERT(dap_test_json_string_is(dap_test_json_get(body, "reason"), "breakpoint"));
        DapTestJson hits = dap_test_json_get(body, "hitBreakpointIds");
        ASSERT(dap_test_json_integer(dap_test_json_at(hits, 0U)) == ids[order[i]]);
        ASSERT(dap_test_json_at(hits, 1U).begin == NULL);
        long thread = dap_test_json_integer(dap_test_json_get(body, "threadId"));
        free(response);
        arguments = dup_printf("{\"threadId\":%ld}", thread);
        free(dap_test_request(&session, "continue", arguments));
        free(arguments);
    }
    response = dap_test_wait(&session, 0, "stopped", "terminated");
    const char *cursor = response;
    DapTestJson event = dap_test_json_next(&cursor);
    ASSERT(dap_test_json_string_is(dap_test_json_get(event, "event"), "terminated"));
    free(response);
    response = dap_test_wait(&session, 0, "exited", NULL);
    ASSERT(dap_test_json_integer(dap_test_json_get(dap_test_body(response), "exitCode")) == 0);
    free(response);
    dap_test_finish(&session);
    free(source_json); free(binary_json); free(root_json); free(list);
    free(backend); free(binary); free(log); free(manifest); free(path); free(src); free(repo);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(root, &error));
    free(error); free(root);
    puts("dap try breakpoints: statements, values, cleanup, handlers and multiline branches preserve source stops");
}
