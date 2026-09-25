/* Compile through the production driver, including its normal/release choice. */
static void native_exception_build(const char *source, const char *out,
                                   const char *name, const char *target,
                                   const char *bundle, bool release) {
    char *out_opt = make_out_option(out);
    char *name_opt = dup_printf("--name=%s", name);
    char *target_opt = dup_printf("--target=%s", target);
    char *pkg_opt = bundle != NULL ? make_pkg_option(bundle) : NULL;
    char *argv[7] = {(char *)source, target_opt, out_opt, name_opt};
    int argc = 4;
    if (pkg_opt != NULL) argv[argc++] = pkg_opt;
    if (release) argv[argc++] = "--release";
    ASSERT(run_direct_for_host(argc, argv) == 0);
    free(pkg_opt);
    free(target_opt);
    free(name_opt);
    free(out_opt);
}

/* Keep the original S11 reproducer independent of std and test-framework code. */
static void test_native_exception_release_reproducer(void) {
    static const char source[] =
        "module release_catch_audit;\n"
        "@cdecl(\"libc\", \"exit\") extern func quit(code:i32):void;\n"
        "spec Callback():void;\n"
        "func fail(value:string):void{throw value;}\n"
        "func invoke(callback:Callback):void{callback();}\n"
        "func main(args:string[]):void{invoke((){var observed=\"\";"
        "try fail(\"header\") catch error:string{observed=error;"
        "let error=\"body\";observed=error;}if observed!=\"body\"{quit(1);}});}\n";
    char workspace[] = "temp/native-eh-reproducer-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *path = path_join(workspace, "main.ff");
    write_text_file(path, source);
    for (int release = 0; release < 2; ++release) {
        char *out = path_join(workspace, release ? "release" : "default");
        char *binary = path_join(out, "bin/native_reproducer");
        native_exception_build(path, out, "native_reproducer", "bin", NULL, release != 0);
        char *text = run_binary_capture_stdout_or_die(binary);
        ASSERT(strcmp(text, "") == 0);
        free(text);
        free(binary);
        free(out);
    }
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    free(path);
}

/* Native tables and descriptor identity must compose across every optimization
 * pairing without access to the producer source or its in-memory analysis. */
static void test_native_exception_source_hidden_optimization_matrix(void) {
    static const char provider[] =
        "open module native.package;\n"
        "@value open type Payload{open let text:string;open let code:int;}\n"
        "open func raise<T:throw>(value:T):T{throw value;}\n"
        "open func relay(value:int):int{return raise<int>(value);}\n"
        "open type Owner<T:throw>{open let value:T;open func fail():T{throw self.value;}"
        "open func send<U:throw>(other:U):U{throw other;}}\n";
    static const char consumer[] =
        "module native.consumer;import native.package;\n"
        "@cdecl(\"libc\") extern func puts(message:string*):int;\n"
        "spec Action(value:int):int;type Pair(int,string);\n"
        "func main(args:string[]){var count=0;"
        "try(try relay(7) catch e:string{0;})catch e:int{if e==7{count+=1;}}"
        "try raise<string>(\"text\")catch e:string{if e==\"text\"{count+=1;}}"
        "try raise(Payload{text:\"payload\",code:11})catch e:Payload{"
        "if e.code==11&&e.text==\"payload\"{count+=1;}}"
        "let pair:Pair=(13,\"pair\");try raise(pair)catch e:Pair{"
        "if e.item1==13&&e.item2==\"pair\"{count+=1;}}"
        "let owner=Owner<int>{value:17};try owner.fail()catch e:int{if e==17{count+=1;}}"
        "try owner.send<string>(\"method\")catch e:string{if e==\"method\"{count+=1;}}"
        "let callback:Action=raise<int>;try callback(19)catch e:int{if e==19{count+=1;}}"
        "try(try relay(23)catch{throw;})catch e:int{if e==23{count+=1;}}"
        "if count!=8{throw \"native package mismatch\";}puts(&\"native-package\");}\n";
    for (int producer_release = 0; producer_release < 2; ++producer_release) {
        char workspace[] = "temp/native-eh-package-XXXXXX";
        ASSERT(mkdtemp(workspace) != NULL);
        char *source = path_join(workspace, "provider.ff");
        char *out = path_join(workspace, "provider-out");
        char *library = host_static_library_output_path(out, "native_package");
        char *modules = path_join(out, "mod");
        char *bundle = path_join(workspace, "native_package.fb");
        char *main_source = path_join(workspace, "main.ff");
        write_text_file(source, provider);
        native_exception_build(source, out, "native_package", "lib", NULL, producer_release != 0);
        write_library_bundle_or_die(bundle, "native_package", "0.1.0", library, modules);
        ASSERT(unlink(source) == 0);
        write_text_file(main_source, consumer);
        for (int consumer_release = 0; consumer_release < 2; ++consumer_release) {
            char *main_out = path_join(workspace, consumer_release ? "release" : "default");
            char *binary = path_join(main_out, "bin/native_consumer");
            native_exception_build(main_source, main_out, "native_consumer", "bin", bundle,
                                   consumer_release != 0);
            char *text = run_binary_capture_stdout_or_die(binary);
            ASSERT(strcmp(text, "native-package\n") == 0);
            free(text);
            free(binary);
            free(main_out);
        }
        char *error = NULL;
        ASSERT(feng_cli_project_remove_tree(workspace, &error));
        free(error);
        free(main_source);
        free(bundle);
        free(modules);
        free(library);
        free(out);
        free(source);
    }
    puts("native Feng EH: S11 default/release and four source-hidden optimization pairings passed");
}
