/* Verify imported descriptors drive real collection, using hard failures
 * independent of std.test's optional active test context. */
static void test_imported_cycle_lifetime(void) {
    static const char library[] =
        "open module cycle_fixture;"
        "open type Self<T>{open let value:T;open var edges:Self<T>[]= [];}"
        "open type Left<T>{open let value:T;open var edges:Right<T>[]= [];}"
        "open type Right<T>{open let value:T;open var edges:Left<T>[]= [];}"
        "open type Plain{open let value:int;open var edges:Plain[]= []; }"
        "open type Acyclic<T>{open let value:T;}";
    static const char consumer[] =
        "module cycle_lifetime;import cycle_fixture;"
        "var made=0;var dropped=0;"
        "func require(ok:bool):void{if !ok{throw 99;}}"
        "type Ref{let n:int;func ~Ref(){if self.n!=0{dropped+=1;}}}"
        "func make(n:int):Ref{made+=1;return Ref{n:n};}"
        "func selfCycle<T>(value:T):void{let node=Self<T>{value:value};node.edges=[node];}"
        "func mutual():void{let a=Left<Ref>{value:make(1)};let b=Right<Ref>{value:make(2)};"
        "a.edges=[b];b.edges=[a];require(a.edges[0].value.n==2);require(b.edges[0].value.n==1);}"
        "func plain():void{let node=Plain{value:3};node.edges=[node];require(node.edges[0].value==3);}"
        "func acyclic():void{let node=Acyclic<Ref>{value:make(4)};require(node.value.n==4);}"
        "func main(args:string[]):void{mutual();require(made==dropped);"
        "selfCycle<Ref>(make(5));require(made==dropped);"
        "selfCycle<string>(\"x\"+\"y\");selfCycle<int>(6);plain();"
        "acyclic();require(made==dropped);}";
    char workspace[] = "temp/imported-cycle-lifetime-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *provider_path = path_join(workspace, "provider.ff");
    char *consumer_path = path_join(workspace, "consumer.ff");
    write_text_file(consumer_path, consumer);
    const char *threshold = getenv("FENG_GC_THRESHOLD");
    char *saved_threshold = threshold != NULL ? dup_cstr(threshold) : NULL;
    ASSERT(setenv("FENG_GC_THRESHOLD", "1", 1) == 0);
    for (int producer_release = 0; producer_release < 2; ++producer_release) {
        write_text_file(provider_path, library);
        char *out = path_join(workspace, producer_release ? "provider-release" : "provider-default");
        native_exception_build(provider_path, out, "cycle_fixture", "lib", NULL,
                               producer_release != 0);
        char *static_library = host_static_library_output_path(out, "cycle_fixture");
        char *modules = path_join(out, "mod");
        char *bundle = path_join(out, "cycle_fixture.fb");
        write_library_bundle_or_die(bundle, "cycle_fixture", "0.1.0", static_library, modules);
        ASSERT(unlink(provider_path) == 0);
        for (int release = 0; release < 2; ++release) {
            char *app_out = dup_printf("%s/app-%d-%d", workspace, producer_release, release);
            native_exception_build(consumer_path, app_out, "cycle_lifetime", "bin", bundle,
                                   release != 0);
            char *binary = path_join(app_out, "bin/cycle_lifetime");
            char *output = run_binary_capture_stdout_or_die(binary);
            ASSERT(strcmp(output, "") == 0);
            free(output);
            free(binary);
            free(app_out);
        }
        free(bundle);
        free(modules);
        free(static_library);
        free(out);
    }
    if (saved_threshold != NULL) ASSERT(setenv("FENG_GC_THRESHOLD", saved_threshold, 1) == 0);
    else ASSERT(unsetenv("FENG_GC_THRESHOLD") == 0);
    free(saved_threshold);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    free(consumer_path);
    free(provider_path);
    puts("imported cycles: source-hidden packages, self/mutual/generic cycles and acyclic lifetimes verified");
}
