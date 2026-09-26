/* Exercise the generated native main wrapper with dynamic, nonliteral argv. */
static void test_main_argv_lifetime(void) {
    for (int explicit_void = 0; explicit_void < 2; ++explicit_void) {
        char workspace[] = "temp/argv-lifetime-XXXXXX";
        ASSERT(mkdtemp(workspace) != NULL);
        char *path = path_join(workspace, "main.ff");
        char *source = dup_printf(
            "module argv_lifetime;\n"
            "func main(args:string[])%s{\n"
            "if args[0]==\"\"||args[1]!=\"\"||args[2]!=\"plain\"{throw \"argv mismatch\";}\n"
            "var value=args[3];let saved=value;value=\"replacement\";\n"
            "if saved!=\"中文🌍\"||args[3]!=saved||value!=\"replacement\"{throw \"argv lifetime\";}\n"
            "}\n", explicit_void ? ":void" : "");
        write_text_file(path, source);
        for (int release = 0; release < 2; ++release) {
            char *out = path_join(workspace, release ? "release" : "default");
            char *binary = path_join(out, "bin/argv_lifetime");
            native_exception_build(path, out, "argv_lifetime", "bin", NULL, release != 0);
            char *command = dup_printf("'%s' '' 'plain' '中文🌍'", binary);
            ASSERT(system(command) == 0);
            free(command);
            free(binary);
            free(out);
        }
        char *error = NULL;
        ASSERT(feng_cli_project_remove_tree(workspace, &error));
        free(error);
        free(source);
        free(path);
    }
    puts("main argv: inferred/explicit void and default/release lifetimes verified");
}
