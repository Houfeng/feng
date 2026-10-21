/* Source-hidden construction uses the existing CLI build and bundle helpers. */
#ifndef FENG_TEST_VALUE_LITERAL_PARTIAL_CLEANUP_H
#define FENG_TEST_VALUE_LITERAL_PARTIAL_CLEANUP_H

/* Shared bodies and imported calls preserve cleanup across Debug/Release pairs. */
static void test_value_literal_partial_cleanup_package(void) {
    char workspace[] = "temp/value-partial-cleanup-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *provider = path_join(workspace, "provider.ff");
    char *consumer = path_join(workspace, "consumer.ff");
    copy_file_or_die("test/cli/fixtures/value_literal_partial_cleanup/main.ff", consumer);
    for (int producer_release = 0; producer_release < 2; ++producer_release) {
        copy_file_or_die("test/cli/fixtures/value_literal_partial_cleanup/provider.ff", provider);
        char *out = path_join(workspace, producer_release ? "provider-release" : "provider-debug");
        native_exception_build(provider, out, "partial_value_provider", "lib", NULL, producer_release != 0);
        char *library = host_static_library_output_path(out, "partial_value_provider");
        char *modules = path_join(out, "mod");
        char *bundle = path_join(out, "partial_value_provider.fb");
        write_library_bundle_or_die(bundle, "partial_value_provider", "0.1.0", library, modules);
        ASSERT(unlink(provider) == 0);
        for (int release = 0; release < 2; ++release) {
            char *app = dup_printf("%s/app-%d-%d", workspace, producer_release, release);
            native_exception_build(consumer, app, "partial_value_consumer", "bin", bundle, release != 0);
            char *binary = path_join(app, "bin/partial_value_consumer");
            char *output = run_binary_capture_stdout_or_die(binary);
            ASSERT(strcmp(output, "") == 0);
            free(output); free(binary); free(app);
        }
        free(bundle); free(modules); free(library); free(out);
    }
    free(consumer); free(provider);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    puts("value construction: source-hidden partial cleanup and four lifecycle build pairings passed");
}

#endif
