#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "archive/fb.h"
#include "archive/zip.h"
#include "cli/cli.h"
#include "cli/frontend.h"
#include "cli/project/common.h"
#include "cli/project/manifest.h"
#include "symbol/imported_module.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static char *dup_cstr(const char *text) {
    size_t length = strlen(text);
    char *out = (char *)malloc(length + 1U);
    ASSERT(out != NULL);
    memcpy(out, text, length + 1U);
    return out;
}

static char *dup_printf(const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *out;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    ASSERT(needed >= 0);

    out = (char *)malloc((size_t)needed + 1U);
    ASSERT(out != NULL);
    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

void feng_cli_print_usage(const char *program) {
    (void)program;
}

static char *path_join(const char *lhs, const char *rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    int need_sep = lhs_len > 0U && lhs[lhs_len - 1U] != '/';
    char *out = (char *)malloc(lhs_len + (need_sep ? 1U : 0U) + rhs_len + 1U);
    size_t cursor = 0U;

    ASSERT(out != NULL);
    memcpy(out + cursor, lhs, lhs_len);
    cursor += lhs_len;
    if (need_sep) {
        out[cursor++] = '/';
    }
    memcpy(out + cursor, rhs, rhs_len);
    cursor += rhs_len;
    out[cursor] = '\0';
    return out;
}

static int path_ends_with(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static void mkdir_p(const char *path) {
    char *mutable_path = dup_cstr(path);
    size_t index;

    for (index = 1U; mutable_path[index] != '\0'; ++index) {
        if (mutable_path[index] == '/') {
            mutable_path[index] = '\0';
            ASSERT(mkdir(mutable_path, 0775) == 0 || errno == EEXIST);
            mutable_path[index] = '/';
        }
    }
    ASSERT(mkdir(mutable_path, 0775) == 0 || errno == EEXIST);
    free(mutable_path);
}

static void write_text_file(const char *path, const char *content) {
    FILE *file;

    file = fopen(path, "wb");
    ASSERT(file != NULL);
    ASSERT(fwrite(content, 1U, strlen(content), file) == strlen(content));
    fclose(file);
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void assert_file_magic(const char *path, const char *magic, size_t magic_len) {
    FILE *file;
    char buffer[8] = {0};

    ASSERT(magic_len <= sizeof(buffer));
    file = fopen(path, "rb");
    ASSERT(file != NULL);
    ASSERT(fread(buffer, 1U, magic_len, file) == magic_len);
    ASSERT(memcmp(buffer, magic, magic_len) == 0);
    fclose(file);
}

static char *read_text_file(const char *path) {
    FILE *file;
    long length;
    char *content;
    size_t read_size;

    file = fopen(path, "rb");
    ASSERT(file != NULL);
    ASSERT(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    ASSERT(length >= 0L);
    ASSERT(fseek(file, 0L, SEEK_SET) == 0);

    content = (char *)malloc((size_t)length + 1U);
    ASSERT(content != NULL);
    read_size = fread(content, 1U, (size_t)length, file);
    ASSERT(read_size == (size_t)length);
    content[length] = '\0';
    fclose(file);
    return content;
}

static char *make_out_option(const char *out_dir) {
    size_t len = strlen(out_dir);
    char *out = (char *)malloc(len + 7U);

    ASSERT(out != NULL);
    memcpy(out, "--out=", 6U);
    memcpy(out + 6U, out_dir, len + 1U);
    return out;
}

static char *make_pkg_option(const char *package_path) {
    size_t len = strlen(package_path);
    char *out = (char *)malloc(len + 7U);

    ASSERT(out != NULL);
    memcpy(out, "--pkg=", 6U);
    memcpy(out + 6U, package_path, len + 1U);
    return out;
}

static void assert_zip_ok(int ok, char **zip_error) {
    if (!ok) {
        fprintf(stderr,
                "zip operation failed: %s\n",
                zip_error != NULL && *zip_error != NULL ? *zip_error : "unknown error");
        if (zip_error != NULL) {
            free(*zip_error);
            *zip_error = NULL;
        }
        ASSERT(false);
    }
    if (zip_error != NULL) {
        free(*zip_error);
        *zip_error = NULL;
    }
}

static void write_bundle_with_file_or_die(const char *bundle_path,
                                          const char *entry_path,
                                          const char *source_path) {
    FengZipWriter writer = {0};
    char *zip_error = NULL;

    assert_zip_ok(feng_zip_writer_open(bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_file(&writer,
                                           entry_path,
                                           source_path,
                                           FENG_ZIP_COMPRESSION_DEFLATE,
                                           &zip_error),
                  &zip_error);
    assert_zip_ok(feng_zip_writer_finalize(&writer, &zip_error), &zip_error);
    feng_zip_writer_dispose(&writer);
}

static void write_bundle_with_bytes_or_die(const char *bundle_path,
                                           const char *entry_path,
                                           const void *data,
                                           size_t data_size) {
    FengZipWriter writer = {0};
    char *zip_error = NULL;

    assert_zip_ok(feng_zip_writer_open(bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_bytes(&writer,
                                            entry_path,
                                            data,
                                            data_size,
                                            FENG_ZIP_COMPRESSION_DEFLATE,
                                            &zip_error),
                  &zip_error);
    assert_zip_ok(feng_zip_writer_finalize(&writer, &zip_error), &zip_error);
    feng_zip_writer_dispose(&writer);
}

static void write_library_bundle_or_die(const char *bundle_path,
                                        const char *package_name,
                                        const char *package_version,
                                        const char *library_path,
                                        const char *public_mod_root) {
    FengFbLibraryBundleSpec spec = {
        .package_path = bundle_path,
        .package_name = package_name,
        .package_version = package_version,
        .library_path = library_path,
        .public_mod_root = public_mod_root,
    };
    char *error_message = NULL;

    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
}

static char *run_binary_capture_stdout_or_die(const char *binary_path) {
    char template_path[] = "/tmp/feng_cli_run_output_XXXXXX";
    char *output_dir;
    char *output_path;
    char *command;
    char *content;
    char *remove_error = NULL;
    int status;

    output_dir = mkdtemp(template_path);
    ASSERT(output_dir != NULL);
    output_path = path_join(output_dir, "stdout.txt");
    command = dup_printf("'%s' > '%s'", binary_path, output_path);
    ASSERT(command != NULL);
    status = system(command);
    ASSERT(status >= 0);
    ASSERT(WIFEXITED(status));
    ASSERT(WEXITSTATUS(status) == 0);
    content = read_text_file(output_path);
    ASSERT(feng_cli_project_remove_tree(output_dir, &remove_error));
    free(remove_error);
    free(command);
    free(output_path);
    return content;
}

static int run_direct_quiet_stderr(int argc, char **argv) {
    int saved_stderr;
    int null_fd;
    int rc;

    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stderr >= 0);
    null_fd = open("/dev/null", O_WRONLY);
    ASSERT(null_fd >= 0);
    ASSERT(dup2(null_fd, STDERR_FILENO) >= 0);
    close(null_fd);

    rc = feng_cli_direct_main("feng", argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    return rc;
}

static int run_init_quiet_stderr(int argc, char **argv) {
    int saved_stderr;
    int null_fd;
    int rc;

    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stderr >= 0);
    null_fd = open("/dev/null", O_WRONLY);
    ASSERT(null_fd >= 0);
    ASSERT(dup2(null_fd, STDERR_FILENO) >= 0);
    close(null_fd);

    rc = feng_cli_project_init_main("feng", argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    return rc;
}

static void test_direct_build_cleans_stale_ir_on_frontend_failure(void) {
    char template_path[] = "/tmp/feng_cli_direct_ir_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *good_path;
    char *bad_path;
    char *out_dir;
    char *c_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    src_dir = path_join(workspace_dir, "src");
    good_path = path_join(src_dir, "good.ff");
    bad_path = path_join(src_dir, "bad.ff");
    out_dir = path_join(workspace_dir, "out");
    c_path = path_join(out_dir, "ir/c/feng.c");

    mkdir_p(src_dir);
    write_text_file(good_path,
                    "mod test.cli.good;\n"
                    "fn main(args: string[]) {}\n");

    {
        char *argv[] = {
            good_path,
            "--target=bin",
            out_dir,
            "--name=demo",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        ASSERT(!path_exists(c_path));
        free(out_opt);
    }

    write_text_file(good_path,
                    "mod test.cli.good;\n"
                    "fn main(args: string[]) {\n");

    {
        char *argv[] = {
            good_path,
            "--target=bin",
            out_dir,
            "--name=demo",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(run_direct_quiet_stderr(4, argv) != 0);
        ASSERT(!path_exists(c_path));
        free(out_opt);
    }

    write_text_file(bad_path,
                    "mod test.cli.keep;\n"
                    "fn main(args: string[]) {}\n");

    {
        char *argv[] = {
            bad_path,
            "--target=bin",
            out_dir,
            "--name=demo",
            "--keep-ir",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
        ASSERT(path_exists(c_path));
        free(out_opt);
    }

    write_text_file(bad_path,
                    "mod test.cli.keep;\n"
                    "fn main(args: string[]) {\n");

    {
        char *argv[] = {
            bad_path,
            "--target=bin",
            out_dir,
            "--name=demo",
            "--keep-ir",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(run_direct_quiet_stderr(5, argv) != 0);
        ASSERT(path_exists(c_path));
        free(out_opt);
    }

    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(c_path);
    free(out_dir);
    free(bad_path);
    free(good_path);
    free(src_dir);
}

static void test_direct_build_emits_symbol_tables(void) {
    char template_path[] = "/tmp/feng_cli_direct_symbols_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *out_dir;
    char *public_ft_path;
    char *workspace_ft_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    src_dir = path_join(workspace_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    out_dir = path_join(workspace_dir, "out");
    public_ft_path = path_join(out_dir, "mod/test/cli/symbols.ft");
    workspace_ft_path = path_join(out_dir, "obj/symbols/test/cli/symbols.ft");

    mkdir_p(src_dir);
    write_text_file(source_path,
                    "pu mod test.cli.symbols;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n"
                    "fn main(args: string[]) {}\n");

    {
        char *argv[] = {
            source_path,
            "--target=bin",
            out_dir,
            "--name=symbols",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        free(out_opt);
    }

    ASSERT(path_exists(public_ft_path));
    ASSERT(path_exists(workspace_ft_path));
    assert_file_magic(public_ft_path, "FST1", 4U);
    assert_file_magic(workspace_ft_path, "FST1", 4U);

    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(workspace_ft_path);
    free(public_ft_path);
    free(out_dir);
    free(source_path);
    free(src_dir);
}

static void test_direct_build_accepts_package_bundle(void) {
    char template_path[] = "/tmp/feng_cli_direct_pkg_XXXXXX";
    char *workspace_dir;
    char *dep_src_dir;
    char *main_src_dir;
    char *dep_source_path;
    char *main_source_path;
    char *dep_out_dir;
    char *main_out_dir;
    char *dep_library_path;
    char *dep_mod_root;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_src_dir = path_join(workspace_dir, "dep/src");
    main_src_dir = path_join(workspace_dir, "main/src");
    dep_source_path = path_join(dep_src_dir, "dep.ff");
    main_source_path = path_join(main_src_dir, "main.ff");
    dep_out_dir = path_join(workspace_dir, "dep/build");
    main_out_dir = path_join(workspace_dir, "main/build");
    dep_library_path = path_join(dep_out_dir, "lib/libdep.a");
    dep_mod_root = path_join(dep_out_dir, "mod");
    bundle_path = path_join(workspace_dir, "pkgdep.fb");

    mkdir_p(dep_src_dir);
    mkdir_p(main_src_dir);
    write_text_file(dep_source_path,
                    "pu mod test.cli.pkgdep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(main_source_path,
                    "mod test.cli.pkgmain;\n"
                    "use test.cli.pkgdep;\n"
                    "fn main(args: string[]) {}\n");

    {
        char *out_opt = make_out_option(dep_out_dir);
        char *argv[] = {
            dep_source_path,
            "--target=lib",
            out_opt,
            "--name=dep",
        };
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        free(out_opt);
    }
    ASSERT(path_exists(dep_library_path));
    ASSERT(path_exists(dep_mod_root));
    write_library_bundle_or_die(bundle_path,
                                "dep",
                                "0.1.0",
                                dep_library_path,
                                dep_mod_root);

    {
        char *out_opt = make_out_option(main_out_dir);
        char *pkg_opt = make_pkg_option(bundle_path);
        char *argv[] = {
            main_source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            pkg_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
        free(pkg_opt);
        free(out_opt);
    }

    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(dep_mod_root);
    free(dep_library_path);
    free(main_out_dir);
    free(dep_out_dir);
    free(main_source_path);
    free(dep_source_path);
    free(main_src_dir);
    free(dep_src_dir);
}

static void test_direct_build_links_library_from_package_bundle(void) {
    char template_path[] = "/tmp/feng_cli_direct_pkg_link_XXXXXX";
    char *workspace_dir;
    char *dep_src_dir;
    char *main_src_dir;
    char *dep_source_path;
    char *main_source_path;
    char *dep_out_dir;
    char *main_out_dir;
    char *dep_library_path;
    char *dep_mod_root;
    char *main_binary_path;
    char *bundle_path;
    char *stdout_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_src_dir = path_join(workspace_dir, "dep/src");
    main_src_dir = path_join(workspace_dir, "main/src");
    dep_source_path = path_join(dep_src_dir, "dep.ff");
    main_source_path = path_join(main_src_dir, "main.ff");
    dep_out_dir = path_join(workspace_dir, "dep/build");
    main_out_dir = path_join(workspace_dir, "main/build");
    dep_library_path = path_join(dep_out_dir, "lib/libpkgdep.a");
    dep_mod_root = path_join(dep_out_dir, "mod");
    main_binary_path = path_join(main_out_dir, "bin/main");
    bundle_path = path_join(workspace_dir, "pkgdep.fb");

    mkdir_p(dep_src_dir);
    mkdir_p(main_src_dir);
    write_text_file(dep_source_path,
                    "pu mod test.cli.pkgdep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(main_source_path,
                    "mod test.cli.pkgmain;\n"
                    "use test.cli.pkgdep;\n"
                    "@cdecl(\"libc\")\n"
                    "extern fn puts(msg: string): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 7 { puts(\"ok\"); } else { puts(\"bad\"); }\n"
                    "}\n");

    {
        char *out_opt = make_out_option(dep_out_dir);
        char *argv[] = {
            dep_source_path,
            "--target=lib",
            out_opt,
            "--name=pkgdep",
        };
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        free(out_opt);
    }

    ASSERT(path_exists(dep_library_path));
    ASSERT(path_exists(dep_mod_root));
    write_library_bundle_or_die(bundle_path,
                                "pkgdep",
                                "0.1.0",
                                dep_library_path,
                                dep_mod_root);

    {
        char *out_opt = make_out_option(main_out_dir);
        char *pkg_opt = make_pkg_option(bundle_path);
        char *argv[] = {
            main_source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            pkg_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
        free(pkg_opt);
        free(out_opt);
    }

    ASSERT(path_exists(main_binary_path));
    stdout_text = run_binary_capture_stdout_or_die(main_binary_path);
    ASSERT(strcmp(stdout_text, "ok\n") == 0);

    free(stdout_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(main_binary_path);
    free(dep_mod_root);
    free(dep_library_path);
    free(main_out_dir);
    free(dep_out_dir);
    free(main_source_path);
    free(dep_source_path);
    free(main_src_dir);
    free(dep_src_dir);
}

static void test_direct_build_sorts_package_libraries_by_dependency(void) {
    char template_path[] = "/tmp/feng_cli_direct_pkg_sort_XXXXXX";
    char *workspace_dir;
    char *b_src_dir;
    char *a_src_dir;
    char *main_src_dir;
    char *b_source_path;
    char *a_source_path;
    char *main_source_path;
    char *b_out_dir;
    char *a_out_dir;
    char *main_out_dir;
    char *b_library_path;
    char *a_library_path;
    char *b_mod_root;
    char *a_mod_root;
    char *main_binary_path;
    char *b_bundle_path;
    char *a_bundle_path;
    char *stdout_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    b_src_dir = path_join(workspace_dir, "pkgb/src");
    a_src_dir = path_join(workspace_dir, "pkga/src");
    main_src_dir = path_join(workspace_dir, "main/src");
    b_source_path = path_join(b_src_dir, "b.ff");
    a_source_path = path_join(a_src_dir, "a.ff");
    main_source_path = path_join(main_src_dir, "main.ff");
    b_out_dir = path_join(workspace_dir, "pkgb/build");
    a_out_dir = path_join(workspace_dir, "pkga/build");
    main_out_dir = path_join(workspace_dir, "main/build");
    b_library_path = path_join(b_out_dir, "lib/libpkgb.a");
    a_library_path = path_join(a_out_dir, "lib/libpkga.a");
    b_mod_root = path_join(b_out_dir, "mod");
    a_mod_root = path_join(a_out_dir, "mod");
    main_binary_path = path_join(main_out_dir, "bin/main");
    b_bundle_path = path_join(workspace_dir, "pkgb.fb");
    a_bundle_path = path_join(workspace_dir, "pkga.fb");

    mkdir_p(b_src_dir);
    mkdir_p(a_src_dir);
    mkdir_p(main_src_dir);
    write_text_file(b_source_path,
                    "pu mod test.cli.pkgb;\n"
                    "pu fn b_value(): int {\n"
                    "  return 11;\n"
                    "}\n");
    write_text_file(a_source_path,
                    "pu mod test.cli.pkga;\n"
                    "use test.cli.pkgb as b;\n"
                    "pu fn a_value(): int {\n"
                    "  return b.b_value();\n"
                    "}\n");
    write_text_file(main_source_path,
                    "mod test.cli.pkgconsumer;\n"
                    "use test.cli.pkga as a;\n"
                    "@cdecl(\"libc\")\n"
                    "extern fn puts(msg: string): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if a.a_value() == 11 { puts(\"ok\"); } else { puts(\"bad\"); }\n"
                    "}\n");

    {
        char *out_opt = make_out_option(b_out_dir);
        char *argv[] = {
            b_source_path,
            "--target=lib",
            out_opt,
            "--name=pkgb",
        };
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        free(out_opt);
    }
    ASSERT(path_exists(b_library_path));
    write_library_bundle_or_die(b_bundle_path,
                                "pkgb",
                                "0.1.0",
                                b_library_path,
                                b_mod_root);

    {
        char *out_opt = make_out_option(a_out_dir);
        char *pkg_opt = make_pkg_option(b_bundle_path);
        char *argv[] = {
            a_source_path,
            "--target=lib",
            out_opt,
            "--name=pkga",
            pkg_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
        free(pkg_opt);
        free(out_opt);
    }
    ASSERT(path_exists(a_library_path));
    write_library_bundle_or_die(a_bundle_path,
                                "pkga",
                                "0.1.0",
                                a_library_path,
                                a_mod_root);

    {
        char *out_opt = make_out_option(main_out_dir);
        char *pkg_b_opt = make_pkg_option(b_bundle_path);
        char *pkg_a_opt = make_pkg_option(a_bundle_path);
        char *argv[] = {
            main_source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            pkg_b_opt,
            pkg_a_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 6, argv) == 0);
        free(pkg_a_opt);
        free(pkg_b_opt);
        free(out_opt);
    }

    ASSERT(path_exists(main_binary_path));
    stdout_text = run_binary_capture_stdout_or_die(main_binary_path);
    ASSERT(strcmp(stdout_text, "ok\n") == 0);

    free(stdout_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(a_bundle_path);
    free(b_bundle_path);
    free(main_binary_path);
    free(a_mod_root);
    free(b_mod_root);
    free(a_library_path);
    free(b_library_path);
    free(main_out_dir);
    free(a_out_dir);
    free(b_out_dir);
    free(main_source_path);
    free(a_source_path);
    free(b_source_path);
    free(main_src_dir);
    free(a_src_dir);
    free(b_src_dir);
}

static void test_project_pack_bundle_can_be_consumed(void) {
    char template_path[] = "/tmp/feng_cli_pack_consume_XXXXXX";
    char *workspace_dir;
    char *lib_project_dir;
    char *lib_manifest_path;
    char *lib_src_dir;
    char *lib_source_path;
    char *bundle_path;
    char *consumer_src_dir;
    char *consumer_source_path;
    char *consumer_out_dir;
    char *consumer_binary_path;
    char *stdout_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    lib_project_dir = path_join(workspace_dir, "packlib");
    lib_manifest_path = path_join(lib_project_dir, "feng.fm");
    lib_src_dir = path_join(lib_project_dir, "src");
    lib_source_path = path_join(lib_src_dir, "lib.ff");
    bundle_path = path_join(lib_project_dir, "build/pkgpack-0.1.0.fb");
    consumer_src_dir = path_join(workspace_dir, "consumer/src");
    consumer_source_path = path_join(consumer_src_dir, "main.ff");
    consumer_out_dir = path_join(workspace_dir, "consumer/build");
    consumer_binary_path = path_join(consumer_out_dir, "bin/main");

    mkdir_p(lib_src_dir);
    mkdir_p(consumer_src_dir);
    write_text_file(lib_manifest_path,
                    "[package]\n"
                    "name: \"pkgpack\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(lib_source_path,
                    "pu mod test.cli.packdep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 9;\n"
                    "}\n");
    write_text_file(consumer_source_path,
                    "mod test.cli.packconsumer;\n"
                    "use test.cli.packdep;\n"
                    "@cdecl(\"libc\")\n"
                    "extern fn puts(msg: string): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 9 { puts(\"ok\"); } else { puts(\"bad\"); }\n"
                    "}\n");

    {
        char *argv[] = { lib_project_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(bundle_path));

    {
        char *out_opt = make_out_option(consumer_out_dir);
        char *pkg_opt = make_pkg_option(bundle_path);
        char *argv[] = {
            consumer_source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            pkg_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
        free(pkg_opt);
        free(out_opt);
    }

    ASSERT(path_exists(consumer_binary_path));
    stdout_text = run_binary_capture_stdout_or_die(consumer_binary_path);
    ASSERT(strcmp(stdout_text, "ok\n") == 0);

    free(stdout_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(consumer_binary_path);
    free(consumer_out_dir);
    free(consumer_source_path);
    free(consumer_src_dir);
    free(bundle_path);
    free(lib_source_path);
    free(lib_src_dir);
    free(lib_manifest_path);
    free(lib_project_dir);
}

static void test_frontend_outputs_absolute_bundle_paths(void) {
    char template_path[] = "/tmp/feng_cli_frontend_pkg_XXXXXX";
    char *workspace_dir;
    char *dep_src_dir;
    char *main_src_dir;
    char *dep_source_path;
    char *main_source_path;
    char *dep_out_dir;
    char *dep_ft_path;
    char *bundle_path;
    char *expected_bundle_path;
    char *saved_cwd;
    char *remove_error = NULL;
    const char *relative_bundle_path = "pkgdep.fb";
    FengSemanticAnalysis *analysis = NULL;
    FengCliLoadedSource *sources = NULL;
    size_t source_count = 0U;
    FengSymbolImportedModuleCache *imported_module_cache = NULL;
    char **bundle_paths = NULL;
    size_t bundle_count = 0U;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_src_dir = path_join(workspace_dir, "dep/src");
    main_src_dir = path_join(workspace_dir, "main/src");
    dep_source_path = path_join(dep_src_dir, "dep.ff");
    main_source_path = path_join(main_src_dir, "main.ff");
    dep_out_dir = path_join(workspace_dir, "dep/build");
    dep_ft_path = path_join(dep_out_dir, "mod/test/cli/pkgdep.ft");
    bundle_path = path_join(workspace_dir, relative_bundle_path);

    mkdir_p(dep_src_dir);
    mkdir_p(main_src_dir);
    write_text_file(dep_source_path,
                    "pu mod test.cli.pkgdep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n"
                    "fn main(args: string[]) {}\n");
    write_text_file(main_source_path,
                    "mod test.cli.pkgmain;\n"
                    "use test.cli.pkgdep;\n"
                    "fn main(args: string[]) {}\n");

    {
        char *out_opt = make_out_option(dep_out_dir);
        char *argv[] = {
            dep_source_path,
            "--target=bin",
            out_opt,
            "--name=dep",
        };
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        free(out_opt);
    }

    ASSERT(path_exists(dep_ft_path));
    write_bundle_with_file_or_die(bundle_path,
                                  "mod/test/cli/pkgdep.ft",
                                  dep_ft_path);
    expected_bundle_path = realpath(bundle_path, NULL);
    ASSERT(expected_bundle_path != NULL);
    saved_cwd = getcwd(NULL, 0);
    ASSERT(saved_cwd != NULL);

    {
        char *paths[] = { main_source_path };
        const char *package_paths[] = { relative_bundle_path };
        FengCliFrontendInput input = {
            .path_count = 1,
            .paths = paths,
            .target = FENG_COMPILE_TARGET_BIN,
            .package_path_count = 1,
            .package_paths = package_paths,
        };
        FengCliFrontendOutputs outputs = {
            .out_analysis = &analysis,
            .out_sources = &sources,
            .out_source_count = &source_count,
            .out_imported_module_cache = &imported_module_cache,
            .out_bundle_paths = &bundle_paths,
            .out_bundle_count = &bundle_count,
        };

        ASSERT(chdir(workspace_dir) == 0);
        ASSERT(feng_cli_frontend_run(&input, NULL, &outputs) == 0);
        ASSERT(chdir(saved_cwd) == 0);
    }

    ASSERT(bundle_count == 1U);
    ASSERT(bundle_paths != NULL);
    ASSERT(bundle_paths[0] != NULL);
    ASSERT(strcmp(bundle_paths[0], expected_bundle_path) == 0);

    free(saved_cwd);
    free(expected_bundle_path);
    feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
    feng_symbol_imported_module_cache_free(imported_module_cache);
    feng_semantic_analysis_free(analysis);
    feng_cli_free_loaded_sources(sources, source_count);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(dep_ft_path);
    free(dep_out_dir);
    free(main_source_path);
    free(dep_source_path);
    free(main_src_dir);
    free(dep_src_dir);
}

static void test_direct_build_rejects_bad_package_bundle(void) {
    static const char kBadBytes[] = "XXXX";
    char template_path[] = "/tmp/feng_cli_direct_bad_pkg_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *out_dir;
    char *binary_path;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    src_dir = path_join(workspace_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    out_dir = path_join(workspace_dir, "out");
    binary_path = path_join(out_dir, "bin/main");
    bundle_path = path_join(workspace_dir, "bad.fb");

    mkdir_p(src_dir);
    write_text_file(source_path,
                    "mod test.cli.badpkg;\n"
                    "fn main(args: string[]) {}\n");
    write_bundle_with_bytes_or_die(bundle_path,
                                   "mod/test/cli/bad.ft",
                                   kBadBytes,
                                   sizeof(kBadBytes) - 1U);

    {
        char *out_opt = make_out_option(out_dir);
        char *argv[] = {
            source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            "--pkg",
            bundle_path,
        };
        ASSERT(run_direct_quiet_stderr(6, argv) != 0);
        free(out_opt);
    }
    ASSERT(!path_exists(binary_path));

    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(binary_path);
    free(out_dir);
    free(source_path);
    free(src_dir);
}

static void test_init_creates_bin_project(void) {
    char template_path[] = "/tmp/feng_cli_init_bin_XXXXXX";
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *main_path;
    char *manifest_text;
    char *main_text;
    char *remove_error = NULL;
    int saved_cwd;

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);

    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    main_path = path_join(src_dir, "main.ff");

    saved_cwd = open(".", O_RDONLY);
    ASSERT(saved_cwd >= 0);
    ASSERT(chdir(project_dir) == 0);
    {
        char *argv[] = { "demo-app" };
        ASSERT(feng_cli_project_init_main("feng", 1, argv) == 0);
    }
    ASSERT(fchdir(saved_cwd) == 0);
    close(saved_cwd);

    ASSERT(path_exists(manifest_path));
    ASSERT(path_exists(src_dir));
    ASSERT(path_exists(main_path));

    manifest_text = read_text_file(manifest_path);
    main_text = read_text_file(main_path);
    ASSERT(strcmp(manifest_text,
                  "[package]\n"
                  "name: \"demo_app\"\n"
                  "version: \"0.1.0\"\n"
                  "target: \"bin\"\n"
                  "src: \"src/\"\n"
                  "out: \"build/\"\n") == 0);
    ASSERT(strcmp(main_text,
                  "mod demo_app;\n"
                  "\n"
                  "fn main(args: string[]) {\n"
                  "}\n") == 0);

    free(main_text);
    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(main_path);
    free(src_dir);
    free(manifest_path);
}

static void test_init_creates_lib_project_using_current_directory_name(void) {
    char template_path[] = "/tmp/feng_cli_init_lib_root_XXXXXX";
    char *root_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *lib_path;
    char *manifest_text;
    char *lib_text;
    char *expected_manifest;
    char *expected_lib_text;
    char *remove_error = NULL;
    int saved_cwd;

    root_dir = mkdtemp(template_path);
    ASSERT(root_dir != NULL);
    project_dir = path_join(root_dir, "9-demo-lib");
    ASSERT(mkdir(project_dir, 0775) == 0);

    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    lib_path = path_join(src_dir, "lib.ff");
    expected_manifest = dup_printf("[package]\nname: \"_9_demo_lib\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n");
    expected_lib_text = dup_printf("mod _9_demo_lib;\n\nfn helper(): int {\n  return 0;\n}\n");
    ASSERT(expected_manifest != NULL);
    ASSERT(expected_lib_text != NULL);

    saved_cwd = open(".", O_RDONLY);
    ASSERT(saved_cwd >= 0);
    ASSERT(chdir(project_dir) == 0);
    {
        char *argv[] = { "--target=lib" };
        ASSERT(feng_cli_project_init_main("feng", 1, argv) == 0);
    }
    ASSERT(fchdir(saved_cwd) == 0);
    close(saved_cwd);

    ASSERT(path_exists(manifest_path));
    ASSERT(path_exists(src_dir));
    ASSERT(path_exists(lib_path));

    manifest_text = read_text_file(manifest_path);
    lib_text = read_text_file(lib_path);
    ASSERT(strcmp(manifest_text, expected_manifest) == 0);
    ASSERT(strcmp(lib_text, expected_lib_text) == 0);

    free(lib_text);
    free(manifest_text);
    free(expected_lib_text);
    free(expected_manifest);
    ASSERT(feng_cli_project_remove_tree(root_dir, &remove_error));
    free(remove_error);
    free(lib_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
}

static void test_init_rejects_space_separated_target_value(void) {
    char template_path[] = "/tmp/feng_cli_init_target_XXXXXX";
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *remove_error = NULL;
    int saved_cwd;

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");

    saved_cwd = open(".", O_RDONLY);
    ASSERT(saved_cwd >= 0);
    ASSERT(chdir(project_dir) == 0);
    {
        char *argv[] = { "--target", "lib" };
        ASSERT(run_init_quiet_stderr(2, argv) != 0);
    }
    ASSERT(fchdir(saved_cwd) == 0);
    close(saved_cwd);

    ASSERT(!path_exists(manifest_path));
    ASSERT(!path_exists(src_dir));

    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(src_dir);
    free(manifest_path);
}

static void test_init_prefixes_keyword_package_name(void) {
    char template_path[] = "/tmp/feng_cli_init_keyword_XXXXXX";
    char *project_dir;
    char *manifest_path;
    char *main_path;
    char *manifest_text;
    char *main_text;
    char *remove_error = NULL;
    int saved_cwd;

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);
    manifest_path = path_join(project_dir, "feng.fm");
    main_path = path_join(project_dir, "src/main.ff");

    saved_cwd = open(".", O_RDONLY);
    ASSERT(saved_cwd >= 0);
    ASSERT(chdir(project_dir) == 0);
    {
        char *argv[] = { "if" };
        ASSERT(feng_cli_project_init_main("feng", 1, argv) == 0);
    }
    ASSERT(fchdir(saved_cwd) == 0);
    close(saved_cwd);

    manifest_text = read_text_file(manifest_path);
    main_text = read_text_file(main_path);
    ASSERT(strcmp(manifest_text,
                  "[package]\n"
                  "name: \"_if\"\n"
                  "version: \"0.1.0\"\n"
                  "target: \"bin\"\n"
                  "src: \"src/\"\n"
                  "out: \"build/\"\n") == 0);
    ASSERT(strcmp(main_text,
                  "mod _if;\n"
                  "\n"
                  "fn main(args: string[]) {\n"
                  "}\n") == 0);

    free(main_text);
    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(main_path);
    free(manifest_path);
}

static void test_init_rejects_non_empty_directory(void) {
    char template_path[] = "/tmp/feng_cli_init_nonempty_XXXXXX";
    char *project_dir;
    char *existing_path;
    char *manifest_path;
    char *src_dir;
    char *remove_error = NULL;
    int saved_cwd;

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);
    existing_path = path_join(project_dir, "README.md");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    write_text_file(existing_path, "occupied\n");

    saved_cwd = open(".", O_RDONLY);
    ASSERT(saved_cwd >= 0);
    ASSERT(chdir(project_dir) == 0);
    ASSERT(run_init_quiet_stderr(0, NULL) != 0);
    ASSERT(fchdir(saved_cwd) == 0);
    close(saved_cwd);

    ASSERT(path_exists(existing_path));
    ASSERT(!path_exists(manifest_path));
    ASSERT(!path_exists(src_dir));

    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(src_dir);
    free(manifest_path);
    free(existing_path);
}

static void test_manifest_defaults(void) {
    static const char *kManifest =
        "# manifest\n"
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(strcmp(manifest.name, "demo") == 0);
    ASSERT(strcmp(manifest.version, "0.1.0") == 0);
    ASSERT(manifest.target == FENG_COMPILE_TARGET_BIN);
    ASSERT(strcmp(manifest.src_path, "src/") == 0);
    ASSERT(strcmp(manifest.out_path, "build/") == 0);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

static void test_manifest_rejects_duplicate_field(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "target: \"lib\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(!feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(error.line == 5U);
    ASSERT(error.message != NULL);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

static void test_project_open_collects_sources(void) {
    char template_path[] = "/tmp/feng_cli_project_XXXXXX";
    char *project_dir;
    char *src_dir;
    char *nested_dir;
    char *manifest_path;
    char *main_path;
    char *helper_path;
    FengCliProjectContext context = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);

    src_dir = path_join(project_dir, "src");
    nested_dir = path_join(src_dir, "nested");
    manifest_path = path_join(project_dir, "feng.fm");
    main_path = path_join(src_dir, "main.ff");
    helper_path = path_join(nested_dir, "helper.ff");

    mkdir_p(nested_dir);
    write_text_file(manifest_path,
                    "# project\n"
                    "[package]\n"
                    "name: \"demo\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"dist/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "base: \"1.0.0\"\n");
    write_text_file(main_path, "mod demo.main;\nfn main(args: string[]) {}\n");
    write_text_file(helper_path, "mod demo.main;\nfn helper(): int { return 1; }\n");

    ASSERT(feng_cli_project_open(project_dir, &context, &error));
    ASSERT(strcmp(context.manifest.name, "demo") == 0);
    ASSERT(strcmp(context.manifest.version, "0.1.0") == 0);
    ASSERT(context.source_count == 2U);
    ASSERT(strstr(context.out_root, "/dist") != NULL);
    ASSERT(strstr(context.binary_path, "/dist/bin/demo") != NULL);
    ASSERT(strstr(context.package_path, "/dist/demo-0.1.0.fb") != NULL);
    ASSERT(strcmp(context.source_paths[0], context.source_paths[1]) < 0);
    ASSERT((path_ends_with(context.source_paths[0], "/src/main.ff")
            && path_ends_with(context.source_paths[1], "/src/nested/helper.ff"))
           || (path_ends_with(context.source_paths[0], "/src/nested/helper.ff")
               && path_ends_with(context.source_paths[1], "/src/main.ff")));

    feng_cli_project_context_dispose(&context);
    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(helper_path);
    free(main_path);
    free(manifest_path);
    free(nested_dir);
    free(src_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_manifest_requires_target(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(!feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "target") != NULL);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

int main(void) {
    test_manifest_defaults();
    test_manifest_rejects_duplicate_field();
    test_project_open_collects_sources();
    test_manifest_requires_target();
    test_init_creates_bin_project();
    test_init_creates_lib_project_using_current_directory_name();
    test_init_rejects_space_separated_target_value();
    test_init_prefixes_keyword_package_name();
    test_init_rejects_non_empty_directory();
    test_direct_build_cleans_stale_ir_on_frontend_failure();
    test_direct_build_emits_symbol_tables();
    test_direct_build_accepts_package_bundle();
    test_direct_build_links_library_from_package_bundle();
    test_direct_build_sorts_package_libraries_by_dependency();
    test_project_pack_bundle_can_be_consumed();
    test_frontend_outputs_absolute_bundle_paths();
    test_direct_build_rejects_bad_package_bundle();
    fprintf(stdout, "cli tests passed\n");
    return 0;
}
