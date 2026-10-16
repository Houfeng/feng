#ifndef FENG_TEST_CYCLE_TYPE_GRAPH_H
#define FENG_TEST_CYCLE_TYPE_GRAPH_H

/* Use the real driver for each independent optimization/LTO configuration. */
static void cycle_graph_build(const char *source, const char *out, const char *name,
                             const char *target, const char *bundle, unsigned mode) {
    char *out_arg = make_out_option(out);
    char *name_arg = dup_printf("--name=%s", name);
    char *target_arg = dup_printf("--target=%s", target);
    char *pkg_arg = bundle != NULL ? make_pkg_option(bundle) : NULL;
    char *args[8] = {(char *)source, out_arg, name_arg, target_arg,
                    mode & 1U ? "--lto=thin" : "--lto=off"};
    int count = 5;
    if (mode & 2U) args[count++] = "--release";
    if (pkg_arg != NULL) args[count++] = pkg_arg;
    ASSERT(run_direct_for_host(count, args) == 0);
    free(pkg_arg); free(target_arg); free(name_arg); free(out_arg);
}

/* All sixteen source-hidden provider/consumer Debug/Release x LTO pairs
 * exercise actual cycle reclamation, not only descriptor text. */
static void test_cycle_type_graph_package(void) {
    char workspace[] = "temp/cycle-type-graph-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *provider = path_join(workspace, "provider.ff");
    char *consumer = path_join(workspace, "consumer.ff");
    copy_file_or_die("test/cli/fixtures/cycle_type_graph/main.ff", consumer);
    const char *threshold = getenv("FENG_GC_THRESHOLD");
    char *saved = threshold != NULL ? strdup(threshold) : NULL;
    ASSERT(setenv("FENG_GC_THRESHOLD", "1", 1) == 0);
    for (unsigned p = 0; p < 4U; ++p) {
        copy_file_or_die("test/cli/fixtures/cycle_type_graph/provider.ff", provider);
        char *out = dup_printf("%s/provider-%u", workspace, p);
        cycle_graph_build(provider, out, "cycle_graph_provider", "lib", NULL, p);
        char *library = host_static_library_output_path(out, "cycle_graph_provider");
        char *modules = path_join(out, "mod");
        char *bundle = path_join(out, "cycle_graph_provider.fb");
        write_library_bundle_or_die(bundle, "cycle_graph_provider", "0.1.0", library, modules);
        ASSERT(unlink(provider) == 0);
        for (unsigned c = 0; c < 4U; ++c) {
            char *app = dup_printf("%s/app-%u-%u", workspace, p, c);
            cycle_graph_build(consumer, app, "cycle_graph_consumer", "bin", bundle, c);
            char *binary = path_join(app, "bin/cycle_graph_consumer");
            char *output = run_binary_capture_stdout_or_die(binary);
            ASSERT(strcmp(output, "") == 0);
            free(output); free(binary); free(app);
        }
        free(bundle); free(modules); free(library); free(out);
    }
    if (saved != NULL) ASSERT(setenv("FENG_GC_THRESHOLD", saved, 1) == 0);
    else ASSERT(unsetenv("FENG_GC_THRESHOLD") == 0);
    free(saved); free(consumer); free(provider);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    puts("cycle type graph: sixteen source-hidden optimization/LTO lifecycle pairs passed");
}

#endif
