/* A source position and its expected executions in the fixture's real path. */
typedef struct DapSourceBreakpointCase {
    const char *marker;
    size_t hits;
} DapSourceBreakpointCase;

/* Check native source stops without line deduplication or automatic resume of
 * unexpected stops. Every stop must have the correct source file and line. */
static void dap_test_source_breakpoint_matrix(const char *source,
    const DapSourceBreakpointCase *cases, size_t count,
    void (*inspect)(DapTestSession *, long, const char *, size_t)) {
    char directory[] = "temp/dap-source-matrix-XXXXXX";
    ASSERT(mkdtemp(directory) != NULL);
    char *root = realpath(directory, NULL), *repo = getcwd(NULL, 0);
    ASSERT(root != NULL && repo != NULL);
    char *src = path_join(root, "src"), *path = path_join(src, "main.ff");
    char *manifest = path_join(root, "feng.fm"), *log = path_join(root, "dap.log");
    char *binary = project_host_build_path(root, "bin/source_matrix");
    char *backend = path_join(repo, "build/toolchain/llvm/bin/lldb-dap");
    mkdir_p(src);
    write_text_file(path, source);
    write_text_file(manifest, "[package]\nname: \"source_matrix\"\nversion: \"0.1.0\"\n"
        "target: \"bin\"\nsrc: \"src/\"\nout: \"build/\"\n");
    char *argv[] = {root};
    ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    char *source_json = json_escape_text(path), *binary_json = json_escape_text(binary);
    char *root_json = json_escape_text(root), *list = dup_cstr("");
    unsigned *lines = calloc(count, sizeof(*lines));
    long *ids = calloc(count, sizeof(*ids));
    size_t *hits = calloc(count, sizeof(*hits)), total = 0U;
    ASSERT(lines != NULL && ids != NULL && hits != NULL);
    for (size_t i = 0U; i < count; ++i) {
        lines[i] = dap_test_breakpoint_line(source, cases[i].marker);
        char *next = dup_printf("%s%s{\"line\":%u}", list, i == 0U ? "" : ",", lines[i]);
        free(list);
        list = next;
        total += cases[i].hits;
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
    for (size_t i = 0U; i < count; ++i) {
        DapTestJson item = dap_test_json_at(breakpoints, i);
        dap_test_json_require_true(dap_test_json_get(item, "verified"));
        ASSERT(dap_test_json_integer(dap_test_json_get(item, "line")) == lines[i]);
        ids[i] = dap_test_json_integer(dap_test_json_get(item, "id"));
    }
    free(response);
    free(dap_test_request(&session, "configurationDone", "{}"));
    free(dap_test_response(&session, launch));
    for (size_t stop = 0U; stop < total; ++stop) {
        response = dap_test_wait(&session, 0, "stopped", "terminated");
        DapTestJson body = dap_test_body(response);
        ASSERT(dap_test_json_string_is(dap_test_json_get(body, "reason"), "breakpoint"));
        DapTestJson hit_ids = dap_test_json_get(body, "hitBreakpointIds");
        long id = dap_test_json_integer(dap_test_json_at(hit_ids, 0U));
        ASSERT(dap_test_json_at(hit_ids, 1U).begin == NULL);
        size_t index = 0U;
        while (index < count && ids[index] != id) ++index;
        ASSERT(index < count);
        fprintf(session.log, "Source stop %zu: %s #%zu\n", stop, cases[index].marker, hits[index] + 1U);
        ASSERT(++hits[index] <= cases[index].hits);
        long thread = dap_test_json_integer(dap_test_json_get(body, "threadId"));
        free(response);
        arguments = dup_printf("{\"threadId\":%ld}", thread);
        response = dap_test_request(&session, "stackTrace", arguments);
        DapTestJson frame = dap_test_json_at(dap_test_json_get(dap_test_body(response), "stackFrames"), 0U);
        ASSERT(dap_test_json_string_is(dap_test_json_get(dap_test_json_get(frame, "source"), "path"), path));
        ASSERT(dap_test_json_integer(dap_test_json_get(frame, "line")) == lines[index]);
        ASSERT(dap_test_json_integer(dap_test_json_get(frame, "column")) == 1);
        long frame_id = dap_test_json_integer(dap_test_json_get(frame, "id"));
        free(response);
        if (inspect != NULL) inspect(&session, frame_id, cases[index].marker, hits[index]);
        free(dap_test_request(&session, "continue", arguments));
        free(arguments);
    }
    response = dap_test_wait(&session, 0, "stopped", "terminated");
    const char *cursor = response;
    ASSERT(dap_test_json_string_is(dap_test_json_get(dap_test_json_next(&cursor), "event"), "terminated"));
    free(response);
    for (size_t i = 0U; i < count; ++i) ASSERT(hits[i] == cases[i].hits);
    response = dap_test_wait(&session, 0, "exited", NULL);
    ASSERT(dap_test_json_integer(dap_test_json_get(dap_test_body(response), "exitCode")) == 0);
    free(response);
    dap_test_finish(&session);
    printf("dap source matrix: %zu positions, %zu stops, source coordinates verified\n", count, total);
    free(lines); free(ids); free(hits); free(source_json); free(binary_json); free(root_json); free(list);
    free(backend); free(binary); free(log); free(manifest); free(path); free(src); free(repo);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(root, &error));
    free(error); free(root);
}

/* User bindings remain readable after their initializer adopts private slots. */
static void dap_source_matrix_values(DapTestSession *session, long frame,
                                     const char *marker, size_t occurrence) {
    if (strcmp(marker, "values") != 0) return;
    const char *value = occurrence == 1U ? "0" : "1";
    DapTestExpectedValue expected[] = {
        {"value_if", value, "int64_t", true},
        {"value_match", value, "int64_t", true},
        {"value_try", value, "int64_t", true},
        {"direct", value, "int64_t", true}
    };
    dap_test_expect_values(session, frame, expected, sizeof(expected) / sizeof(*expected));
}

/* Cross expression forms with their source consumers using real native stops. */
static void test_dap_singleline_source_positions(void) {
    const char *source =
        "module source_matrix;\n"
        "@cdecl(\"libc\",\"exit\") extern func quit(code:i32):void;\n"
        "var total:int=0;\n"
        "func identity<T>(value:T):T{return value;}\n"
        "func consume(value:int):void{total+=value;}\n"
        "func return_if(value:int):int{\n"
        "  return if value==0 { identity(value); } else { identity(value); }; // breakpoint: return_if\n"
        "}\n"
        "func return_match(value:int):int{\n"
        "  return match value { 0 { identity(value); } else { identity(value); } }; // breakpoint: return_match\n"
        "}\n"
        "func return_try(value:int):int{\n"
        "  return try identity(value) catch { 0; }; // breakpoint: return_try\n"
        "}\n"
        "func work(value:int):void{\n"
        "  var target=0;\n"
        "  let value_if=if value==0 { identity(value); } else { identity(value); }; // breakpoint: binding_if\n"
        "  target=if value==0 { identity(value); } else { identity(value); }; // breakpoint: assign_if\n"
        "  consume(if value==0 { identity(value); } else { identity(value); }); // breakpoint: argument_if\n"
        "  (if value==0 { identity(value); } else { identity(value); }); // breakpoint: statement_if\n"
        "  if (if value==0 { identity(value); } else { identity(value); })==value {total+=1;} // breakpoint: condition_if\n"
        "  let sum_if=1+(if value==0 { identity(value); } else { identity(value); }); // breakpoint: arithmetic_if\n"
        "  let test_if=value==0 && (if value==0 { identity(value); } else { identity(value); })==value;\n"
        "  consume(return_if(value));\n"
        "  let value_match=match value { 0 { identity(value); } else { identity(value); } }; // breakpoint: binding_match\n"
        "  target=match value { 0 { identity(value); } else { identity(value); } }; // breakpoint: assign_match\n"
        "  consume(match value { 0 { identity(value); } else { identity(value); } }); // breakpoint: argument_match\n"
        "  (match value { 0 { identity(value); } else { identity(value); } }); // breakpoint: statement_match\n"
        "  if (match value { 0 { identity(value); } else { identity(value); } })==value {total+=1;} // breakpoint: condition_match\n"
        "  let sum_match=1+(match value { 0 { identity(value); } else { identity(value); } }); // breakpoint: arithmetic_match\n"
        "  let test_match=value==0 && (match value { 0 { identity(value); } else { identity(value); } })==value;\n"
        "  consume(return_match(value));\n"
        "  let value_try=try identity(value) catch { 0; }; // breakpoint: binding_try\n"
        "  target=try identity(value) catch { 0; }; // breakpoint: assign_try\n"
        "  consume(try identity(value) catch { 0; }); // breakpoint: argument_try\n"
        "  (try identity(value) catch { 0; }); // breakpoint: statement_try\n"
        "  if (try identity(value) catch { 0; })==value {total+=1;} // breakpoint: condition_try\n"
        "  let sum_try=1+(try identity(value) catch { 0; }); // breakpoint: arithmetic_try\n"
        "  let test_try=value==0 && (try identity(value) catch { 0; })==value;\n"
        "  consume(return_try(value));\n"
        "  let direct=identity(value); // breakpoint: direct_call\n"
        "  target=value+1; // breakpoint: ordinary_assignment\n"
        "  let ar=[value,value+1]; // breakpoint: array_literal\n"
        "  let element=ar[identity(value)]; // breakpoint: array_index\n"
        "  consume(value_if);consume(value_match);consume(value_try); // breakpoint: same_line\n"
        "  consume(direct); // breakpoint: values\n"
        "}\n"
        "func main(args:string[]):void{work(0);work(1);if total<0{quit(1);}}\n";
    const DapSourceBreakpointCase cases[] = {
        {"return_if", 2U},
        {"return_match", 2U},
        {"return_try", 2U},
        {"binding_if", 2U},
        {"assign_if", 2U},
        {"argument_if", 2U},
        {"statement_if", 2U},
        {"condition_if", 2U},
        {"arithmetic_if", 2U},
        {"binding_match", 2U},
        {"assign_match", 2U},
        {"argument_match", 2U},
        {"statement_match", 2U},
        {"condition_match", 2U},
        {"arithmetic_match", 2U},
        {"binding_try", 2U},
        {"assign_try", 2U},
        {"argument_try", 2U},
        {"statement_try", 2U},
        {"condition_try", 2U},
        {"arithmetic_try", 2U},
        {"direct_call", 2U},
        {"ordinary_assignment", 2U},
        {"array_literal", 2U},
        {"array_index", 2U},
        {"same_line", 2U},
        {"values", 2U}
    };
    dap_test_source_breakpoint_matrix(source, cases, sizeof(cases) / sizeof(*cases),
        dap_source_matrix_values);
}

/* Cross expression forms with their source consumers using real native stops. */
static void test_dap_multiline_source_positions(void) {
    const char *source =
        "module source_matrix;\n"
        "func identity<T>(value:T):T{return value;}\n"
        "func consume(value:int):void{}\n"
        "func ret_if(value:int):int{\n"
        "  return if value==0 { // breakpoint: return_if\n"
        "    identity(value);\n"
        "    } else {\n"
        "    identity(value);\n"
        "  };\n"
        "}\n"
        "func ret_match(value:int):int{\n"
        "  return match value { // breakpoint: return_match\n"
        "    0 { identity(value); }\n"
        "    else { identity(value); }\n"
        "  };\n"
        "}\n"
        "func work(value:int):void{\n"
        "var target=0;\n"
        "{\n"
        "  let result=if value==0 { // breakpoint: binding_if\n"
        "    identity(value);\n"
        "    } else {\n"
        "    identity(value);\n"
        "  };\n"
        "}\n"
        "{\n"
        "  target=if value==0 { // breakpoint: assign_if\n"
        "    identity(value);\n"
        "    } else {\n"
        "    identity(value);\n"
        "  };\n"
        "}\n"
        "{\n"
        "  consume(if value==0 { // breakpoint: argument_if\n"
        "    identity(value);\n"
        "    } else {\n"
        "    identity(value);\n"
        "  });\n"
        "}\n"
        "{\n"
        "  let sum=1+(if value==0 { // breakpoint: arithmetic_if\n"
        "    identity(value);\n"
        "    } else {\n"
        "    identity(value);\n"
        "  });\n"
        "}\n"
        "consume(ret_if(value));\n"
        "{\n"
        "  let result=match value { // breakpoint: binding_match\n"
        "    0 { identity(value); }\n"
        "    else { identity(value); }\n"
        "  };\n"
        "}\n"
        "{\n"
        "  target=match value { // breakpoint: assign_match\n"
        "    0 { identity(value); }\n"
        "    else { identity(value); }\n"
        "  };\n"
        "}\n"
        "{\n"
        "  consume(match value { // breakpoint: argument_match\n"
        "    0 { identity(value); }\n"
        "    else { identity(value); }\n"
        "  });\n"
        "}\n"
        "{\n"
        "  let sum=1+(match value { // breakpoint: arithmetic_match\n"
        "    0 { identity(value); }\n"
        "    else { identity(value); }\n"
        "  });\n"
        "}\n"
        "consume(ret_match(value));\n"
        "}\n"
        "func main(args:string[]):void{work(0);work(1);}\n";
    const DapSourceBreakpointCase cases[] = {
        {"return_if", 2U},
        {"return_match", 2U},
        {"binding_if", 2U},
        {"assign_if", 2U},
        {"argument_if", 2U},
        {"arithmetic_if", 2U},
        {"binding_match", 2U},
        {"assign_match", 2U},
        {"argument_match", 2U},
        {"arithmetic_match", 2U}
    };
    dap_test_source_breakpoint_matrix(source, cases, sizeof(cases) / sizeof(*cases),
        NULL);
}

/* Source catch bindings and closure carriers survive internal-local hiding. */
static void dap_source_binding_values(DapTestSession *session, long frame,
                                       const char *marker, size_t occurrence) {
    if (strcmp(marker, "caught_value") == 0) {
        DapTestExpectedValue values[] = {
            {"error", "11", "int64_t", true}, {"previous", "11", "int64_t", true}
        };
        dap_test_expect_values(session, frame, values, sizeof(values) / sizeof(*values));
    } else if (strcmp(marker, "captured_value") == 0) {
        const char *value = occurrence == 1U ? "11" : "12";
        DapTestExpectedValue values[] = {
            {"shared", value, NULL, true}, {"local", value, "int64_t", true}
        };
        dap_test_expect_values(session, frame, values, sizeof(values) / sizeof(*values));
    }
}

/* Check adopted/captured storage with the public Locals, hover and watch paths. */
static void test_dap_source_binding_storage(void) {
    const char *source =
        "module source_matrix;\n"
        "spec Action():void;\n"
        "func consume(value:int):void{}\n"
        "func fail():int{throw 11;}\n"
        "func main(args:string[]):void{\n"
        "  try fail() catch error:int {\n"
        "    let previous=error;\n"
        "    consume(previous); // breakpoint: caught_value\n"
        "  }\n"
        "  var shared=10;\n"
        "  let action:Action=(){\n"
        "    shared+=1;\n"
        "    let local=shared;\n"
        "    consume(local); // breakpoint: captured_value\n"
        "  };\n"
        "  action();action();\n"
        "}\n";
    const DapSourceBreakpointCase cases[] = {
        {"caught_value", 1U}, {"captured_value", 2U}
    };
    dap_test_source_breakpoint_matrix(source, cases, sizeof(cases) / sizeof(*cases),
        dap_source_binding_values);
}
