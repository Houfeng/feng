#ifndef FENG_TEST_CYCLE_BORROW_H
#define FENG_TEST_CYCLE_BORROW_H

/* Keep normal driver selection and sanitizer overrides for each build mode. */
static void cycle_borrow_build(const char *source, const char *out, const char *name,
                               const char *target, const char *bundle, unsigned mode) {
    char *out_arg = make_out_option(out);
    char *name_arg = dup_printf("--name=%s", name);
    char *target_arg = dup_printf("--target=%s", target);
    char *pkg_arg = bundle != NULL ? make_pkg_option(bundle) : NULL;
    char *args[9] = {(char *)source, out_arg, name_arg, target_arg, "--keep-ir",
                    mode & 1U ? "--lto=thin" : "--lto=off"};
    int count = 6;
    if (mode & 2U) args[count++] = "--release";
    if (pkg_arg != NULL) args[count++] = pkg_arg;
    ASSERT(run_direct_for_host(count, args) == 0);
    free(pkg_arg); free(target_arg); free(name_arg); free(out_arg);
}

/* A cross-package borrowed read must not rely on LTO to remove extra owners. */
static void cycle_borrow_check_emission(const char *out, const char *function) {
    char *path = path_join(out, "ir/c/feng.c");
    char *source = read_text_file(path);
    ASSERT(source != NULL);
    bool found = false;
    for (const char *at = source; (at = strstr(at, function)) != NULL; ++at) {
        const char *brace = strchr(at, '{');
        const char *line = strchr(at, '\n');
        if (brace == NULL || line == NULL || brace > line) continue;
        const char *end = strstr(brace, "\n}");
        ASSERT(end != NULL);
        char *body = strndup(brace, (size_t)(end - brace));
        ASSERT(body != NULL);
        ASSERT(strstr(body, "feng_aggregate_retain(") == NULL);
        ASSERT(strstr(body, "feng_cleanup_push_aggregate(") == NULL);
        ASSERT(strstr(body, "feng_retain(") == NULL);
        free(body); found = true; break;
    }
    ASSERT(found);
    free(source); free(path);
}

/* Source-hidden provider/consumer combinations cover both LTO modes and both
 * optimization levels. Threshold one checks candidate completeness after live
 * scans and resurrection, without requiring redundant borrow-triggered scans. */
static void test_cycle_borrow_package(void) {
    char workspace[] = "temp/cycle-borrow-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *provider = path_join(workspace, "provider.ff");
    char *consumer = path_join(workspace, "consumer.ff");
    copy_file_or_die("test/cli/fixtures/cycle_borrow/main.ff", consumer);
    const char *threshold = getenv("FENG_GC_THRESHOLD");
    char *saved = threshold != NULL ? strdup(threshold) : NULL;
    ASSERT(setenv("FENG_GC_THRESHOLD", "1", 1) == 0);
    for (unsigned p = 0; p < 4U; ++p) {
        copy_file_or_die("test/cli/fixtures/cycle_borrow/provider.ff", provider);
        char *out = dup_printf("%s/provider-%u", workspace, p);
        cycle_borrow_build(provider, out, "cycle_borrow_provider", "lib", NULL, p);
        cycle_borrow_check_emission(out, "__borrowRead__");
        cycle_borrow_check_emission(out, "__borrowInject__");
        char *library = host_static_library_output_path(out, "cycle_borrow_provider");
        char *modules = path_join(out, "mod");
        char *bundle = path_join(out, "cycle_borrow_provider.fb");
        write_library_bundle_or_die(bundle, "cycle_borrow_provider", "0.1.0", library, modules);
        ASSERT(unlink(provider) == 0);
        for (unsigned c = 0; c < 4U; ++c) {
            char *out_app = dup_printf("%s/app-%u-%u", workspace, p, c);
            cycle_borrow_build(consumer, out_app, "cycle_borrow_consumer", "bin", bundle, c);
            cycle_borrow_check_emission(out_app, "__importedRead__");
            char *binary = path_join(out_app, "bin/cycle_borrow_consumer");
            char *output = run_binary_capture_stdout_or_die(binary);
            ASSERT(strcmp(output, "") == 0);
            free(output); free(binary); free(out_app);
        }
        free(bundle); free(modules); free(library); free(out);
    }
    if (saved != NULL) ASSERT(setenv("FENG_GC_THRESHOLD", saved, 1) == 0);
    else ASSERT(unsetenv("FENG_GC_THRESHOLD") == 0);
    free(saved); free(consumer); free(provider);
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    puts("cycle borrow: sixteen source-hidden mode pairs, emission and real-owner cycle reclamation passed");
}

#endif
