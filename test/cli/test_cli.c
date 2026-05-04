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
#include "cli/deps/manager.h"
#include "cli/frontend.h"
#include "cli/lsp/server.h"
#include "cli/project/common.h"
#include "cli/project/manifest.h"
#include "symbol/ft_internal.h"
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

static void write_executable_text_file(const char *path, const char *content) {
    write_text_file(path, content);
    ASSERT(chmod(path, 0775) == 0);
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

static int count_occurrences(const char *text, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *cursor = text;

    ASSERT(needle_len > 0U);
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count += 1;
        cursor += needle_len;
    }
    return count;
}

static char *create_logging_cc_wrapper(const char *dir, const char *log_path) {
    char *script_path = path_join(dir, "fake-cc.sh");
    char *script_text = dup_printf("#!/bin/sh\n"
                                   "printf '__CMD__\\n' >> \"%s\"\n"
                                   "for arg in \"$@\"; do\n"
                                   "  printf '%%s\\n' \"$arg\" >> \"%s\"\n"
                                   "done\n"
                                   "exec cc \"$@\"\n",
                                   log_path,
                                   log_path);

    ASSERT(script_text != NULL);
    write_executable_text_file(script_path, script_text);
    free(script_text);
    return script_path;
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

static void write_manifest_only_bundle_or_die(const char *bundle_path, const char *manifest_text) {
    FengZipWriter writer = {0};
    char *zip_error = NULL;

    assert_zip_ok(feng_zip_writer_open(bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_bytes(&writer,
                                            "feng.fm",
                                            manifest_text,
                                            strlen(manifest_text),
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

static int run_frontend_with_overlays_quiet_stderr(
    const FengCliFrontendInput *input,
    const FengCliFrontendSourceOverlay *overlays,
    size_t overlay_count,
    const FengCliFrontendCallbacks *callbacks,
    const FengCliFrontendOutputs *outputs) {
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

    rc = feng_cli_frontend_run_with_overlays(input,
                                             overlays,
                                             overlay_count,
                                             callbacks,
                                             outputs);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    return rc;
}

static int run_lsp_quiet_stderr(int argc, char **argv) {
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

    rc = feng_cli_lsp_main("feng", argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    return rc;
}

static char *read_text_stream(FILE *file) {
    long length;
    char *content;
    size_t read_size;

    ASSERT(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    ASSERT(length >= 0L);
    ASSERT(fseek(file, 0L, SEEK_SET) == 0);
    content = (char *)malloc((size_t)length + 1U);
    ASSERT(content != NULL);
    read_size = fread(content, 1U, (size_t)length, file);
    ASSERT(read_size == (size_t)length);
    content[length] = '\0';
    return content;
}

static char *json_escape_text(const char *text) {
    size_t index;
    size_t extra = 0U;
    char *escaped;
    size_t cursor = 0U;

    for (index = 0U; text[index] != '\0'; ++index) {
        switch (text[index]) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                extra += 1U;
                break;
            default:
                break;
        }
    }
    escaped = (char *)malloc(index + extra + 1U);
    ASSERT(escaped != NULL);
    for (index = 0U; text[index] != '\0'; ++index) {
        switch (text[index]) {
            case '\\':
                escaped[cursor++] = '\\';
                escaped[cursor++] = '\\';
                break;
            case '"':
                escaped[cursor++] = '\\';
                escaped[cursor++] = '"';
                break;
            case '\n':
                escaped[cursor++] = '\\';
                escaped[cursor++] = 'n';
                break;
            case '\r':
                escaped[cursor++] = '\\';
                escaped[cursor++] = 'r';
                break;
            case '\t':
                escaped[cursor++] = '\\';
                escaped[cursor++] = 't';
                break;
            default:
                escaped[cursor++] = text[index];
                break;
        }
    }
    escaped[cursor] = '\0';
    return escaped;
}

static void write_lsp_message(FILE *input, const char *json) {
    ASSERT(fprintf(input, "Content-Length: %zu\r\n\r\n%s", strlen(json), json) >= 0);
}

static char *run_lsp_server_capture(FILE *input) {
    char input_template[] = "/tmp/feng_lsp_input_XXXXXX";
    int input_fd;
    char *input_text;
    FILE *named_input;
    FILE *output = tmpfile();
    FILE *errors = tmpfile();
    char *captured;

    ASSERT(output != NULL);
    ASSERT(errors != NULL);
    ASSERT(fseek(input, 0L, SEEK_SET) == 0);
    input_text = read_text_stream(input);
    input_fd = mkstemp(input_template);
    ASSERT(input_fd >= 0);
    named_input = fdopen(input_fd, "wb+");
    ASSERT(named_input != NULL);
    ASSERT(fwrite(input_text, 1U, strlen(input_text), named_input) == strlen(input_text));
    free(input_text);
    ASSERT(fflush(named_input) == 0);
    ASSERT(fseek(named_input, 0L, SEEK_SET) == 0);
    ASSERT(feng_lsp_server_run(named_input, output, errors) == 0);
    captured = read_text_stream(output);
    fclose(named_input);
    ASSERT(unlink(input_template) == 0);
    fclose(errors);
    fclose(output);
    return captured;
}

static char *file_uri_from_path(const char *path) {
    return dup_printf("file://%s", path);
}

static void find_line_character(const char *text,
                                const char *needle,
                                size_t char_offset,
                                unsigned int *line,
                                unsigned int *character) {
    const char *cursor = strstr(text, needle);
    const char *scan;

    ASSERT(cursor != NULL);
    cursor += char_offset;
    *line = 0U;
    *character = 0U;
    for (scan = text; scan < cursor; ++scan) {
        if (*scan == '\n') {
            *line += 1U;
            *character = 0U;
        } else {
            *character += 1U;
        }
    }
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

static void test_pack_bundle_manifest_rewrites_local_dependency_versions(void) {
    char template_path[] = "/tmp/feng_cli_pack_manifest_XXXXXX";
    char *workspace_dir;
    char *dep_project_dir;
    char *dep_manifest_path;
    char *dep_src_dir;
    char *dep_source_path;
    char *root_project_dir;
    char *root_manifest_path;
    char *root_src_dir;
    char *root_source_path;
    char *bundle_path;
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *manifest_bytes = NULL;
    size_t manifest_size = 0U;
    char *manifest_text = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_project_dir = path_join(workspace_dir, "dep");
    dep_manifest_path = path_join(dep_project_dir, "feng.fm");
    dep_src_dir = path_join(dep_project_dir, "src");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    root_src_dir = path_join(root_project_dir, "src");
    root_source_path = path_join(root_src_dir, "lib.ff");
    bundle_path = path_join(root_project_dir, "build/rootlib-0.1.0.fb");

    mkdir_p(dep_src_dir);
    mkdir_p(root_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod local.dep;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");
    write_text_file(root_manifest_path,
                    "[package]\n"
                    "name: \"rootlib\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_dep: \"../dep\"\n");
    write_text_file(root_source_path,
                    "pu mod root.lib;\n"
                    "pu fn root_value(): int {\n"
                    "  return 2;\n"
                    "}\n");

    {
        char *argv[] = { root_project_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(bundle_path));
    ASSERT(feng_zip_reader_open(bundle_path, &reader, &zip_error));
    ASSERT(feng_zip_reader_read(&reader, "feng.fm", &manifest_bytes, &manifest_size, &zip_error));
    manifest_text = (char *)malloc(manifest_size + 1U);
    ASSERT(manifest_text != NULL);
    memcpy(manifest_text, manifest_bytes, manifest_size);
    manifest_text[manifest_size] = '\0';
    ASSERT(strstr(manifest_text, "[dependencies]\nlocal_dep: \"0.1.0\"") != NULL);
    ASSERT(strstr(manifest_text, "../dep") == NULL);

    free(manifest_text);
    feng_zip_free(manifest_bytes);
    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(root_source_path);
    free(root_src_dir);
    free(root_manifest_path);
    free(root_project_dir);
    free(dep_source_path);
    free(dep_src_dir);
    free(dep_manifest_path);
    free(dep_project_dir);
}

static void test_project_check_accepts_source_file_path_and_local_dependencies(void) {
    char template_path[] = "/tmp/feng_cli_check_source_path_XXXXXX";
    char *workspace_dir;
    char *dep_project_dir;
    char *dep_manifest_path;
    char *dep_src_dir;
    char *dep_source_path;
    char *root_project_dir;
    char *root_manifest_path;
    char *root_src_dir;
    char *root_source_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_project_dir = path_join(workspace_dir, "dep");
    dep_manifest_path = path_join(dep_project_dir, "feng.fm");
    dep_src_dir = path_join(dep_project_dir, "src");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    root_src_dir = path_join(root_project_dir, "src");
    root_source_path = path_join(root_src_dir, "main.ff");

    mkdir_p(dep_src_dir);
    mkdir_p(root_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod test.cli.localdep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(root_manifest_path,
                    "[package]\n"
                    "name: \"local_dep_app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_dep: \"../dep\"\n");
    write_text_file(root_source_path,
                    "mod test.cli.localdepapp;\n"
                    "\n"
                    "use test.cli.localdep;\n"
                    "\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 7 {\n"
                    "  }\n"
                    "}\n");

    {
        char *argv[] = { root_source_path };
        ASSERT(feng_cli_project_check_main("feng", 1, argv) == 0);
    }

    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(root_source_path);
    free(root_src_dir);
    free(root_manifest_path);
    free(root_project_dir);
    free(dep_source_path);
    free(dep_src_dir);
    free(dep_manifest_path);
    free(dep_project_dir);
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

static void test_frontend_source_overlay_replaces_disk_source(void) {
    char template_path[] = "/tmp/feng_cli_frontend_overlay_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *main_source_path;
    char *remove_error = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengCliLoadedSource *sources = NULL;
    size_t source_count = 0U;

    static const char *kOverlaySource =
        "mod overlay.demo;\n"
        "fn main(args: string[]) {}\n";

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    main_source_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(main_source_path,
                    "mod overlay.demo;\n"
                    "fn main( {}\n");

    {
        char *paths[] = { main_source_path };
        FengCliFrontendInput input = {
            .path_count = 1,
            .paths = paths,
            .target = FENG_COMPILE_TARGET_BIN,
            .package_path_count = 0,
            .package_paths = NULL,
        };
        FengCliFrontendSourceOverlay overlays[] = {
            {
                .path = main_source_path,
                .source = kOverlaySource,
                .source_length = strlen(kOverlaySource),
            },
        };
        FengCliFrontendOutputs outputs = {
            .out_analysis = &analysis,
            .out_sources = &sources,
            .out_source_count = &source_count,
        };

        ASSERT(feng_cli_frontend_run_with_overlays(&input,
                                                   overlays,
                                                   1U,
                                                   NULL,
                                                   &outputs) == 0);
    }

    ASSERT(analysis != NULL);
    ASSERT(source_count == 1U);
    ASSERT(sources != NULL);
    ASSERT(strcmp(sources[0].source, kOverlaySource) == 0);

    feng_semantic_analysis_free(analysis);
    feng_cli_free_loaded_sources(sources, source_count);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(main_source_path);
    free(src_dir);
}

static void test_frontend_source_overlay_rejects_duplicate_paths(void) {
    char template_path[] = "/tmp/feng_cli_frontend_overlay_dup_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *main_source_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    main_source_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(main_source_path,
                    "mod overlay.demo;\n"
                    "fn main(args: string[]) {}\n");

    {
        static const char *kOverlaySource =
            "mod overlay.demo;\n"
            "fn main(args: string[]) {}\n";
        char *paths[] = { main_source_path };
        FengCliFrontendInput input = {
            .path_count = 1,
            .paths = paths,
            .target = FENG_COMPILE_TARGET_BIN,
            .package_path_count = 0,
            .package_paths = NULL,
        };
        FengCliFrontendSourceOverlay overlays[] = {
            {
                .path = main_source_path,
                .source = kOverlaySource,
                .source_length = strlen(kOverlaySource),
            },
            {
                .path = main_source_path,
                .source = kOverlaySource,
                .source_length = strlen(kOverlaySource),
            },
        };

        ASSERT(run_frontend_with_overlays_quiet_stderr(&input,
                                                       overlays,
                                                       2U,
                                                       NULL,
                                                       NULL) != 0);
    }

    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(main_source_path);
    free(src_dir);
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

static void test_lsp_help_returns_success(void) {
    char *argv[] = { "--help" };

    ASSERT(run_lsp_quiet_stderr(1, argv) == 0);
}

static void test_lsp_rejects_unknown_option(void) {
    char *argv[] = { "--bogus" };

    ASSERT(run_lsp_quiet_stderr(1, argv) != 0);
}

static void test_lsp_publish_diagnostics_for_open_change_and_close(void) {
    static const char *kBadSource =
        "mod test.lsp;\n"
        "fn main(args: string[]) {\n"
        "    let value: string = ;\n"
        "}\n";
    static const char *kGoodSource =
        "mod test.lsp;\n"
        "fn main(args: string[]) {\n"
        "    let value: string = \"ok\";\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_diag_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *bad_text;
    char *good_text;
    char *initialize;
    char *did_open;
    char *did_change;
    char *did_close;
    char *shutdown;
    char *output;
    FILE *input;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kGoodSource);

    uri = file_uri_from_path(source_path);
    bad_text = json_escape_text(kBadSource);
    good_text = json_escape_text(kGoodSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          bad_text);
    did_change = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":2},\"contentChanges\":[{\"text\":\"%s\"}]}}",
                            uri,
                            good_text);
    did_close = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
                           uri);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, did_change);
    write_lsp_message(input, did_close);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(count_occurrences(output, "\"method\":\"textDocument/publishDiagnostics\"") == 3);
    ASSERT(strstr(output, "\"diagnostics\":[{") != NULL);
    ASSERT(count_occurrences(output, "\"diagnostics\":[]") >= 2);

    free(output);
    free(shutdown);
    free(did_close);
    free(did_change);
    free(did_open);
    free(initialize);
    free(good_text);
    free(bad_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

static void test_lsp_hover_definition_and_completion(void) {
    static const char *kSource =
        "mod test.lsp;\n"
        "\n"
        "/** User record. */\n"
        "type User {\n"
        "    /** Display name. */\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "/** Formats a user label. */\n"
        "fn format(user: User): string {\n"
        "    return user.name;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let label: string = format(user);\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_query_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *hover_fn;
    char *definition_fn;
    char *hover_field;
    char *completion_field;
    char *shutdown;
    char *output;
    char *expected_definition;
    FILE *input;
    unsigned int fn_line;
    unsigned int fn_character;
    unsigned int field_line;
    unsigned int field_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);

    find_line_character(kSource,
                        "let label: string = format(user);",
                        20U,
                        &fn_line,
                        &fn_character);
    find_line_character(kSource,
                        "let mirror: string = user.name;",
                        26U,
                        &field_line,
                        &field_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    hover_fn = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                          uri,
                          fn_line,
                          fn_character);
    definition_fn = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                               uri,
                               fn_line,
                               fn_character);
    hover_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                             uri,
                             field_line,
                             field_character);
    completion_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                  uri,
                                  field_line,
                                  field_character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");
    expected_definition = dup_printf("\"id\":3,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":9",
                                     uri);

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_fn);
    write_lsp_message(input, definition_fn);
    write_lsp_message(input, hover_field);
    write_lsp_message(input, completion_field);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"hoverProvider\":true") != NULL);
    ASSERT(strstr(output, "\"definitionProvider\":true") != NULL);
    ASSERT(strstr(output, "\"referencesProvider\":true") != NULL);
    ASSERT(strstr(output, "\"renameProvider\":{\"prepareProvider\":true}") != NULL);
    ASSERT(strstr(output, "\"completionProvider\"") != NULL);
    ASSERT(strstr(output, "Formats a user label.") != NULL);
    ASSERT(strstr(output, "fn format(user: User): string") != NULL);
    ASSERT(strstr(output, "Display name.") != NULL);
    ASSERT(strstr(output, "let name: string") != NULL);
    ASSERT(strstr(output, expected_definition) != NULL);
    ASSERT(strstr(output, "\"id\":5,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"label\":\"name\"") != NULL);

    free(output);
    free(expected_definition);
    free(shutdown);
    free(completion_field);
    free(hover_field);
    free(definition_fn);
    free(hover_fn);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

static void test_lsp_member_references_and_rename_from_object_literal_field(void) {
    static const char *kSource =
        "mod test.lsp.rename;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_member_rename_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *definition_field;
    char *references_field;
    char *prepare_rename_field;
    char *rename_field;
    char *shutdown;
    char *output;
    char *expected_definition;
    char *expected_decl_ref;
    char *expected_use_ref;
    char *expected_prepare;
    FILE *input;
    unsigned int field_line;
    unsigned int field_character;
    unsigned int decl_line;
    unsigned int decl_character;
    unsigned int use_line;
    unsigned int use_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);

    find_line_character(kSource,
                        "let user: User = User { name: \"copilot\" };",
                        24U,
                        &field_line,
                        &field_character);
    find_line_character(kSource,
                        "let name: string;",
                        4U,
                        &decl_line,
                        &decl_character);
    find_line_character(kSource,
                        "let mirror: string = user.name;",
                        26U,
                        &use_line,
                        &use_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    definition_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                  uri,
                                  field_line,
                                  field_character);
    references_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/references\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"context\":{\"includeDeclaration\":true}}}",
                                  uri,
                                  field_line,
                                  field_character);
    prepare_rename_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                      uri,
                                      field_line,
                                      field_character);
    rename_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/rename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"newName\":\"displayName\"}}",
                              uri,
                              field_line,
                              field_character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");
    expected_definition = dup_printf("\"id\":2,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                     uri,
                                     decl_line,
                                     decl_character);
    expected_decl_ref = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                   uri,
                                   decl_line,
                                   decl_character);
    expected_use_ref = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                  uri,
                                  use_line,
                                  use_character);
    expected_prepare = dup_printf("\"id\":4,\"result\":{\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                  field_line,
                                  field_character);

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, definition_field);
    write_lsp_message(input, references_field);
    write_lsp_message(input, prepare_rename_field);
    write_lsp_message(input, rename_field);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"referencesProvider\":true") != NULL);
    ASSERT(strstr(output, "\"renameProvider\":{\"prepareProvider\":true}") != NULL);
    ASSERT(strstr(output, expected_definition) != NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":[") != NULL);
    ASSERT(strstr(output, expected_decl_ref) != NULL);
    ASSERT(strstr(output, expected_use_ref) != NULL);
    ASSERT(strstr(output, expected_prepare) != NULL);
    ASSERT(strstr(output, "\"placeholder\":\"name\"") != NULL);
    ASSERT(strstr(output, "\"id\":5,\"result\":{\"changes\":{") != NULL);
    ASSERT(count_occurrences(output, "\"newText\":\"displayName\"") == 3);
    ASSERT(strstr(output, uri) != NULL);

    free(output);
    free(expected_prepare);
    free(expected_use_ref);
    free(expected_decl_ref);
    free(expected_definition);
    free(shutdown);
    free(rename_field);
    free(prepare_rename_field);
    free(references_field);
    free(definition_field);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

static void test_lsp_project_cache_hit_survives_broken_dependency_source(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"cache_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kSharedSource =
        "pu mod test.cli.cachedep;\n"
        "\n"
        "/** User from cache. */\n"
        "pu type User {\n"
        "    /** Display name. */\n"
        "    let name: string;\n"
        "}\n";
    static const char *kBrokenSharedSource =
        "pu mod test.cli.cachedep;\n"
        "\n"
        "pu type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "pu fn broken(user: User): string {\n";
    static const char *kMainSource =
        "mod test.cli.cachemain;\n"
        "use test.cli.cachedep;\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_cache_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *shared_path;
    char *main_path;
    char *main_uri;
    char *shared_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *hover_type;
    char *definition_type;
    char *hover_field;
    char *completion_field;
    char *shutdown;
    char *expected_definition;
    char *expected_definition_alt = NULL;
    char *output;
    char *shared_real_path = NULL;
    char *shared_real_uri = NULL;
    FILE *input;
    unsigned int type_line;
    unsigned int type_character;
    unsigned int field_line;
    unsigned int field_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    shared_path = path_join(src_dir, "shared.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(shared_path, kSharedSource);
    write_text_file(main_path, kMainSource);

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    write_text_file(shared_path, kBrokenSharedSource);

    find_line_character(kMainSource,
                        "let user: User = User { name: \"copilot\" };",
                        10U,
                        &type_line,
                        &type_character);
    find_line_character(kMainSource,
                        "let mirror: string = user.name;",
                        26U,
                        &field_line,
                        &field_character);

    main_uri = file_uri_from_path(main_path);
    shared_uri = file_uri_from_path(shared_path);
    shared_real_path = realpath(shared_path, NULL);
    if (shared_real_path != NULL) {
        shared_real_uri = file_uri_from_path(shared_real_path);
    }
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    hover_type = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                            main_uri,
                            type_line,
                            type_character);
    definition_type = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                 main_uri,
                                 type_line,
                                 type_character);
    hover_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                             main_uri,
                             field_line,
                             field_character);
    completion_field = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                  main_uri,
                                  field_line,
                                  field_character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");
    expected_definition = dup_printf("\"id\":3,\"result\":{\"uri\":\"%s\"", shared_uri);
    if (shared_real_uri != NULL && strcmp(shared_real_uri, shared_uri) != 0) {
        expected_definition_alt = dup_printf("\"id\":3,\"result\":{\"uri\":\"%s\"",
                                             shared_real_uri);
    }

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_type);
    write_lsp_message(input, definition_type);
    write_lsp_message(input, hover_field);
    write_lsp_message(input, completion_field);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":null") == NULL);
    ASSERT(strstr(output, "User from cache.") != NULL);
    ASSERT(strstr(output, "type User") != NULL);
    ASSERT(strstr(output, "Display name.") != NULL);
    ASSERT(strstr(output, "let name: string") != NULL);
        ASSERT(strstr(output, expected_definition) != NULL ||
            (expected_definition_alt != NULL && strstr(output, expected_definition_alt) != NULL));
    ASSERT(strstr(output, "\"id\":5,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"label\":\"name\"") != NULL);

    free(output);
        free(expected_definition_alt);
    free(expected_definition);
    free(shutdown);
    free(completion_field);
    free(hover_field);
    free(definition_type);
    free(hover_type);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(shared_real_uri);
    free(shared_real_path);
    free(shared_uri);
    free(main_uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(main_path);
    free(shared_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
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

static void test_manifest_parses_dependencies_and_registry(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "abi: \"\"\n"
        "\n"
        "[dependencies]\n"
        "base: \"1.2.3\"\n"
        "util.local: \"../util-local\"\n"
        "\n"
        "[registry]\n"
        "url: \"https://packages.example.com/feng\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(manifest.has_target);
    ASSERT(manifest.target == FENG_COMPILE_TARGET_LIB);
    ASSERT(manifest.abi != NULL);
    ASSERT(strcmp(manifest.abi, "") == 0);
    ASSERT(manifest.registry_url != NULL);
    ASSERT(strcmp(manifest.registry_url, "https://packages.example.com/feng") == 0);
    ASSERT(manifest.dependency_count == 2U);
    ASSERT(strcmp(manifest.dependencies[0].name, "base") == 0);
    ASSERT(strcmp(manifest.dependencies[0].value, "1.2.3") == 0);
    ASSERT(!manifest.dependencies[0].is_local_path);
    ASSERT(strcmp(manifest.dependencies[1].name, "util.local") == 0);
    ASSERT(strcmp(manifest.dependencies[1].value, "../util-local") == 0);
    ASSERT(manifest.dependencies[1].is_local_path);

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

static void test_bundle_manifest_allows_dependencies_without_target(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"1.0.0\"\n"
        "arch: \"macos-arm64\"\n"
        "abi: \"feng\"\n"
        "\n"
        "[dependencies]\n"
        "base: \"1.2.3\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(feng_cli_project_bundle_manifest_parse("/tmp/demo.fb:feng.fm",
                                                  kManifest,
                                                  &manifest,
                                                  &error));
    ASSERT(!manifest.has_target);
    ASSERT(manifest.arch != NULL);
    ASSERT(strcmp(manifest.arch, "macos-arm64") == 0);
    ASSERT(manifest.abi != NULL);
    ASSERT(strcmp(manifest.abi, "feng") == 0);
    ASSERT(manifest.dependency_count == 1U);
    ASSERT(strcmp(manifest.dependencies[0].name, "base") == 0);
    ASSERT(strcmp(manifest.dependencies[0].value, "1.2.3") == 0);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

static void test_bundle_manifest_rejects_local_path_dependency(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"1.0.0\"\n"
        "arch: \"macos-arm64\"\n"
        "abi: \"feng\"\n"
        "\n"
        "[dependencies]\n"
        "base: \"../base\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(!feng_cli_project_bundle_manifest_parse("/tmp/demo.fb:feng.fm",
                                                   kManifest,
                                                   &manifest,
                                                   &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "exact versions") != NULL);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_installs_remote_transitive_dependencies(void) {
    char template_path[] = "/tmp/feng_cli_deps_remote_XXXXXX";
    char *workspace_dir;
    char *registry_dir;
    char *packages_dir;
    char *project_dir;
    char *manifest_path;
    char *bundle_a;
    char *bundle_b;
    char *saved_home = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");
    bundle_a = path_join(packages_dir, "dep_a-1.0.0.fb");
    bundle_b = path_join(packages_dir, "dep_b-1.0.0.fb");

    mkdir_p(packages_dir);
    mkdir_p(project_dir);
    write_manifest_only_bundle_or_die(bundle_b,
                                      "[package]\n"
                                      "name: \"dep_b\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(bundle_a,
                                      "[package]\n"
                                      "name: \"dep_a\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "dep_b: \"1.0.0\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "dep_a: \"1.0.0\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"../registry\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    ASSERT(feng_cli_deps_resolve_for_manifest("feng", manifest_path, false, false, &resolved, &error));
    ASSERT(resolved.package_count == 2U);
    ASSERT((path_ends_with(resolved.package_paths[0], "/.feng/cache/dep_a-1.0.0.fb") &&
            path_ends_with(resolved.package_paths[1], "/.feng/cache/dep_b-1.0.0.fb")) ||
           (path_ends_with(resolved.package_paths[1], "/.feng/cache/dep_a-1.0.0.fb") &&
            path_ends_with(resolved.package_paths[0], "/.feng/cache/dep_b-1.0.0.fb")));
    ASSERT(path_exists(resolved.package_paths[0]));
    ASSERT(path_exists(resolved.package_paths[1]));

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }
    free(saved_home);
    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_b);
    free(bundle_a);
    free(manifest_path);
    free(project_dir);
    free(packages_dir);
    free(registry_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_builds_local_library_dependency(void) {
    char template_path[] = "/tmp/feng_cli_deps_local_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *dep_dir;
    char *dep_src_dir;
    char *project_manifest_path;
    char *dep_manifest_path;
    char *dep_source_path;
    char *expected_bundle_path;
    char *resolved_expected_bundle_path = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    dep_dir = path_join(workspace_dir, "local_dep");
    dep_src_dir = path_join(dep_dir, "src");
    project_manifest_path = path_join(project_dir, "feng.fm");
    dep_manifest_path = path_join(dep_dir, "feng.fm");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    expected_bundle_path = path_join(dep_dir, "build/local_dep-0.1.0.fb");

    mkdir_p(project_dir);
    mkdir_p(dep_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "mod local.dep;\n"
                    "pu fn value(): int { return 1; }\n");
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_dep: \"../local_dep\"\n");

    ASSERT(feng_cli_deps_resolve_for_manifest("feng", project_manifest_path, false, false, &resolved, &error));
    ASSERT(resolved.package_count == 1U);
    resolved_expected_bundle_path = realpath(expected_bundle_path, NULL);
    ASSERT(resolved_expected_bundle_path != NULL);
    ASSERT(strcmp(resolved.package_paths[0], resolved_expected_bundle_path) == 0);
    ASSERT(path_exists(expected_bundle_path));

    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(resolved_expected_bundle_path);
    free(expected_bundle_path);
    free(dep_source_path);
    free(dep_manifest_path);
    free(project_manifest_path);
    free(dep_src_dir);
    free(dep_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_requires_registry_for_remote_dependency(void) {
    char template_path[] = "/tmp/feng_cli_deps_no_registry_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *saved_home = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");

    mkdir_p(project_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "remote_dep: \"1.0.0\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    ASSERT(!feng_cli_deps_resolve_for_manifest("feng", manifest_path, false, false, &resolved, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "configured registry") != NULL);

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    free(saved_home);
    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(manifest_path);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_uses_global_registry_config(void) {
    char template_path[] = "/tmp/feng_cli_deps_global_registry_XXXXXX";
    char *workspace_dir;
    char *feng_dir;
    char *config_path;
    char *registry_dir;
    char *packages_dir;
    char *project_dir;
    char *manifest_path;
    char *bundle_path;
    char *saved_home = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    feng_dir = path_join(workspace_dir, ".feng");
    config_path = path_join(feng_dir, "config.fm");
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");
    bundle_path = path_join(packages_dir, "remote_dep-1.0.0.fb");

    mkdir_p(feng_dir);
    mkdir_p(packages_dir);
    mkdir_p(project_dir);
    write_text_file(config_path,
                    "[registry]\n"
                    "url: \"../registry\"\n");
    write_manifest_only_bundle_or_die(bundle_path,
                                      "[package]\n"
                                      "name: \"remote_dep\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "remote_dep: \"1.0.0\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    ASSERT(feng_cli_deps_resolve_for_manifest("feng", manifest_path, false, false, &resolved, &error));
    ASSERT(resolved.package_count == 1U);
    ASSERT(path_exists(resolved.package_paths[0]));
    ASSERT(path_ends_with(resolved.package_paths[0], "/.feng/cache/remote_dep-1.0.0.fb"));

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    free(saved_home);
    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(manifest_path);
    free(project_dir);
    free(packages_dir);
    free(registry_dir);
    free(config_path);
    free(feng_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_reports_transitive_version_conflict(void) {
    char template_path[] = "/tmp/feng_cli_deps_conflict_XXXXXX";
    char *workspace_dir;
    char *registry_dir;
    char *packages_dir;
    char *project_dir;
    char *manifest_path;
    char *common_v1_path;
    char *common_v2_path;
    char *dep_a_path;
    char *dep_b_path;
    char *saved_home = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");
    common_v1_path = path_join(packages_dir, "common-1.0.0.fb");
    common_v2_path = path_join(packages_dir, "common-2.0.0.fb");
    dep_a_path = path_join(packages_dir, "dep_a-1.0.0.fb");
    dep_b_path = path_join(packages_dir, "dep_b-1.0.0.fb");

    mkdir_p(packages_dir);
    mkdir_p(project_dir);
    write_manifest_only_bundle_or_die(common_v1_path,
                                      "[package]\n"
                                      "name: \"common\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(common_v2_path,
                                      "[package]\n"
                                      "name: \"common\"\n"
                                      "version: \"2.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(dep_a_path,
                                      "[package]\n"
                                      "name: \"dep_a\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "common: \"1.0.0\"\n");
    write_manifest_only_bundle_or_die(dep_b_path,
                                      "[package]\n"
                                      "name: \"dep_b\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "common: \"2.0.0\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "dep_a: \"1.0.0\"\n"
                    "dep_b: \"1.0.0\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"../registry\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    ASSERT(!feng_cli_deps_resolve_for_manifest("feng", manifest_path, false, false, &resolved, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "dependency version conflict") != NULL);

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    free(saved_home);
    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_b_path);
    free(dep_a_path);
    free(common_v2_path);
    free(common_v1_path);
    free(manifest_path);
    free(project_dir);
    free(packages_dir);
    free(registry_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_reports_local_dependency_cycle(void) {
    char template_path[] = "/tmp/feng_cli_deps_cycle_XXXXXX";
    char *workspace_dir;
    char *root_project_dir;
    char *root_manifest_path;
    char *dep_a_dir;
    char *dep_a_manifest_path;
    char *dep_a_src_dir;
    char *dep_a_source_path;
    char *dep_b_dir;
    char *dep_b_manifest_path;
    char *dep_b_src_dir;
    char *dep_b_source_path;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    dep_a_dir = path_join(workspace_dir, "dep_a");
    dep_a_manifest_path = path_join(dep_a_dir, "feng.fm");
    dep_a_src_dir = path_join(dep_a_dir, "src");
    dep_a_source_path = path_join(dep_a_src_dir, "lib.ff");
    dep_b_dir = path_join(workspace_dir, "dep_b");
    dep_b_manifest_path = path_join(dep_b_dir, "feng.fm");
    dep_b_src_dir = path_join(dep_b_dir, "src");
    dep_b_source_path = path_join(dep_b_src_dir, "lib.ff");

    mkdir_p(root_project_dir);
    mkdir_p(dep_a_src_dir);
    mkdir_p(dep_b_src_dir);
    write_text_file(root_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "dep_a: \"../dep_a\"\n");
    write_text_file(dep_a_manifest_path,
                    "[package]\n"
                    "name: \"dep_a\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "dep_b: \"../dep_b\"\n");
    write_text_file(dep_b_manifest_path,
                    "[package]\n"
                    "name: \"dep_b\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "dep_a: \"../dep_a\"\n");
    write_text_file(dep_a_source_path,
                    "pu mod test.cli.cyclea;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");
    write_text_file(dep_b_source_path,
                    "pu mod test.cli.cycleb;\n"
                    "pu fn value(): int {\n"
                    "  return 2;\n"
                    "}\n");

    ASSERT(!feng_cli_deps_resolve_for_manifest("feng", root_manifest_path, false, false, &resolved, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "cycle") != NULL);

    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_b_source_path);
    free(dep_b_src_dir);
    free(dep_b_manifest_path);
    free(dep_b_dir);
    free(dep_a_source_path);
    free(dep_a_src_dir);
    free(dep_a_manifest_path);
    free(dep_a_dir);
    free(root_manifest_path);
    free(root_project_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_add_remote_updates_manifest_and_cache(void) {
    char template_path[] = "/tmp/feng_cli_deps_add_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *registry_dir;
    char *packages_dir;
    char *manifest_path;
    char *bundle_path;
    char *cache_path;
    char *manifest_text;
    char *saved_home = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    manifest_path = path_join(project_dir, "feng.fm");
    bundle_path = path_join(packages_dir, "remote_dep-1.0.0.fb");
    cache_path = path_join(workspace_dir, ".feng/cache/remote_dep-1.0.0.fb");

    mkdir_p(project_dir);
    mkdir_p(packages_dir);
    write_manifest_only_bundle_or_die(bundle_path,
                                      "[package]\n"
                                      "name: \"remote_dep\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"../registry\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    {
        char *argv[] = { "add", "remote_dep", "1.0.0", project_dir };
        ASSERT(feng_cli_deps_main("feng", 4, argv) == 0);
    }

    manifest_text = read_text_file(manifest_path);
    ASSERT(strstr(manifest_text, "[dependencies]\nremote_dep: \"1.0.0\"") != NULL);
    ASSERT(path_exists(cache_path));

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }
    free(saved_home);
    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(bundle_path);
    free(manifest_path);
    free(packages_dir);
    free(registry_dir);
    free(project_dir);
}

static void test_deps_add_local_validates_then_writes_manifest(void) {
    char template_path[] = "/tmp/feng_cli_deps_add_local_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *dep_dir;
    char *dep_src_dir;
    char *project_manifest_path;
    char *dep_manifest_path;
    char *dep_source_path;
    char *dep_bundle_path;
    char *manifest_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    dep_dir = path_join(workspace_dir, "dep");
    dep_src_dir = path_join(dep_dir, "src");
    project_manifest_path = path_join(project_dir, "feng.fm");
    dep_manifest_path = path_join(dep_dir, "feng.fm");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    dep_bundle_path = path_join(dep_dir, "build/local_dep-0.1.0.fb");

    mkdir_p(project_dir);
    mkdir_p(dep_src_dir);
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod test.cli.addlocal;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");

    {
        char *argv[] = { "add", "local_dep", "../dep", project_dir };
        ASSERT(feng_cli_deps_main("feng", 4, argv) == 0);
    }

    manifest_text = read_text_file(project_manifest_path);
    ASSERT(strstr(manifest_text, "[dependencies]\nlocal_dep: \"../dep\"") != NULL);
    ASSERT(!path_exists(dep_bundle_path));

    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_bundle_path);
    free(dep_source_path);
    free(dep_manifest_path);
    free(project_manifest_path);
    free(dep_src_dir);
    free(dep_dir);
    free(project_dir);
}

static void test_deps_add_local_rejects_name_mismatch_before_write(void) {
    char template_path[] = "/tmp/feng_cli_deps_add_local_name_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *dep_dir;
    char *dep_src_dir;
    char *project_manifest_path;
    char *dep_manifest_path;
    char *dep_source_path;
    char *manifest_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    dep_dir = path_join(workspace_dir, "dep");
    dep_src_dir = path_join(dep_dir, "src");
    project_manifest_path = path_join(project_dir, "feng.fm");
    dep_manifest_path = path_join(dep_dir, "feng.fm");
    dep_source_path = path_join(dep_src_dir, "lib.ff");

    mkdir_p(project_dir);
    mkdir_p(dep_src_dir);
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"other_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod test.cli.addlocalname;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");

    {
        char *argv[] = { "add", "local_dep", "../dep", project_dir };
        ASSERT(feng_cli_deps_main("feng", 4, argv) != 0);
    }

    manifest_text = read_text_file(project_manifest_path);
    ASSERT(strstr(manifest_text, "[dependencies]") == NULL);

    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_source_path);
    free(dep_manifest_path);
    free(project_manifest_path);
    free(dep_src_dir);
    free(dep_dir);
    free(project_dir);
}

static void test_deps_add_local_rejects_non_lib_target_before_write(void) {
    char template_path[] = "/tmp/feng_cli_deps_add_local_target_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *dep_dir;
    char *dep_src_dir;
    char *project_manifest_path;
    char *dep_manifest_path;
    char *dep_source_path;
    char *manifest_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    dep_dir = path_join(workspace_dir, "dep");
    dep_src_dir = path_join(dep_dir, "src");
    project_manifest_path = path_join(project_dir, "feng.fm");
    dep_manifest_path = path_join(dep_dir, "feng.fm");
    dep_source_path = path_join(dep_src_dir, "main.ff");

    mkdir_p(project_dir);
    mkdir_p(dep_src_dir);
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "mod test.cli.addlocaltarget;\n"
                    "fn main(args: string[]) {}\n");

    {
        char *argv[] = { "add", "local_dep", "../dep", project_dir };
        ASSERT(feng_cli_deps_main("feng", 4, argv) != 0);
    }

    manifest_text = read_text_file(project_manifest_path);
    ASSERT(strstr(manifest_text, "[dependencies]") == NULL);

    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_source_path);
    free(dep_manifest_path);
    free(project_manifest_path);
    free(dep_src_dir);
    free(dep_dir);
    free(project_dir);
}

static void test_deps_remove_updates_manifest(void) {
    char template_path[] = "/tmp/feng_cli_deps_remove_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *manifest_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");

    mkdir_p(project_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "base: \"1.0.0\"\n"
                    "other: \"2.0.0\"\n");

    {
        char *argv[] = { "remove", "base", project_dir };
        ASSERT(feng_cli_deps_main("feng", 3, argv) == 0);
    }

    manifest_text = read_text_file(manifest_path);
    ASSERT(strstr(manifest_text, "base: \"1.0.0\"") == NULL);
    ASSERT(strstr(manifest_text, "other: \"2.0.0\"") != NULL);

    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(manifest_path);
    free(project_dir);
}

static void test_deps_install_populates_cache_from_registry(void) {
    char template_path[] = "/tmp/feng_cli_deps_install_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *registry_dir;
    char *packages_dir;
    char *manifest_path;
    char *bundle_path;
    char *cache_path;
    char *manifest_text;
    char *saved_home = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    manifest_path = path_join(project_dir, "feng.fm");
    bundle_path = path_join(packages_dir, "remote_dep-1.0.0.fb");
    cache_path = path_join(workspace_dir, ".feng/cache/remote_dep-1.0.0.fb");

    mkdir_p(project_dir);
    mkdir_p(packages_dir);
    write_manifest_only_bundle_or_die(bundle_path,
                                      "[package]\n"
                                      "name: \"remote_dep\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "remote_dep: \"1.0.0\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"../registry\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    {
        char *argv[] = { "install", project_dir };
        ASSERT(feng_cli_deps_main("feng", 2, argv) == 0);
    }

    ASSERT(path_exists(cache_path));
    manifest_text = read_text_file(manifest_path);
    ASSERT(strstr(manifest_text, "remote_dep: \"1.0.0\"") != NULL);

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }
    free(saved_home);
    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(bundle_path);
    free(manifest_path);
    free(packages_dir);
    free(registry_dir);
    free(project_dir);
}

static void test_deps_install_force_refreshes_cached_bundle(void) {
    char template_path[] = "/tmp/feng_cli_deps_install_force_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *registry_dir;
    char *packages_dir;
    char *manifest_path;
    char *registry_bundle_path;
    char *cache_dir;
    char *cache_path;
    char *saved_home = NULL;
    char *remove_error = NULL;
    FengZipWriter writer = {0};
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *marker_bytes = NULL;
    size_t marker_size = 0U;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    manifest_path = path_join(project_dir, "feng.fm");
    registry_bundle_path = path_join(packages_dir, "remote_dep-1.0.0.fb");
    cache_dir = path_join(workspace_dir, ".feng/cache");
    cache_path = path_join(cache_dir, "remote_dep-1.0.0.fb");

    mkdir_p(project_dir);
    mkdir_p(packages_dir);
    mkdir_p(cache_dir);
    write_manifest_only_bundle_or_die(cache_path,
                                      "[package]\n"
                                      "name: \"remote_dep\"\n"
                                      "version: \"1.0.0\"\n"
                                      "arch: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    assert_zip_ok(feng_zip_writer_open(registry_bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_bytes(&writer,
                                            "feng.fm",
                                            "[package]\n"
                                            "name: \"remote_dep\"\n"
                                            "version: \"1.0.0\"\n"
                                            "arch: \"macos-arm64\"\n"
                                            "abi: \"feng\"\n",
                                            strlen("[package]\n"
                                                   "name: \"remote_dep\"\n"
                                                   "version: \"1.0.0\"\n"
                                                   "arch: \"macos-arm64\"\n"
                                                   "abi: \"feng\"\n"),
                                            FENG_ZIP_COMPRESSION_DEFLATE,
                                            &zip_error),
                  &zip_error);
    assert_zip_ok(feng_zip_writer_add_bytes(&writer,
                                            "marker.txt",
                                            "fresh-cache",
                                            strlen("fresh-cache"),
                                            FENG_ZIP_COMPRESSION_DEFLATE,
                                            &zip_error),
                  &zip_error);
    assert_zip_ok(feng_zip_writer_finalize(&writer, &zip_error), &zip_error);
    feng_zip_writer_dispose(&writer);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "remote_dep: \"1.0.0\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"../registry\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    {
        char *argv[] = { "install", "--force", project_dir };
        ASSERT(feng_cli_deps_main("feng", 3, argv) == 0);
    }

    ASSERT(feng_zip_reader_open(cache_path, &reader, &zip_error));
    ASSERT(feng_zip_reader_read(&reader, "marker.txt", &marker_bytes, &marker_size, &zip_error));
    ASSERT(marker_size == strlen("fresh-cache"));
    ASSERT(memcmp(marker_bytes, "fresh-cache", marker_size) == 0);

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }
    free(saved_home);
    feng_zip_free(marker_bytes);
    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(cache_dir);
    free(registry_bundle_path);
    free(manifest_path);
    free(packages_dir);
    free(registry_dir);
    free(project_dir);
}

static void test_project_build_default_uses_debug_friendly_flags(void) {
    char template_path[] = "/tmp/feng_cli_build_debug_flags_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *binary_path;
    char *cc_log_text;
    char *saved_cc = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);
    binary_path = path_join(project_dir, "build/bin/app");

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(source_path,
                    "mod test.cli.debugflags;\n"
                    "fn main(args: string[]) {}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(count_occurrences(cc_log_text, "__CMD__") >= 1);
    ASSERT(count_occurrences(cc_log_text, "-O0") >= 1);
    ASSERT(count_occurrences(cc_log_text, "-g") >= 1);
    ASSERT(count_occurrences(cc_log_text, "-DNDEBUG") == 0);

    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    free(saved_cc);
    free(cc_log_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(binary_path);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
}

static void test_project_build_release_propagates_to_local_dependencies(void) {
    char template_path[] = "/tmp/feng_cli_build_release_flags_XXXXXX";
    char *workspace_dir;
    char *dep_project_dir;
    char *dep_manifest_path;
    char *dep_src_dir;
    char *dep_source_path;
    char *root_project_dir;
    char *root_manifest_path;
    char *root_src_dir;
    char *root_source_path;
    char *binary_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *cc_log_text;
    char *saved_cc = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_project_dir = path_join(workspace_dir, "dep");
    dep_manifest_path = path_join(dep_project_dir, "feng.fm");
    dep_src_dir = path_join(dep_project_dir, "src");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    root_src_dir = path_join(root_project_dir, "src");
    root_source_path = path_join(root_src_dir, "main.ff");
    binary_path = path_join(root_project_dir, "build/bin/release_app");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);

    mkdir_p(dep_src_dir);
    mkdir_p(root_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"release_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod test.cli.releasedep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(root_manifest_path,
                    "[package]\n"
                    "name: \"release_app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "release_dep: \"../dep\"\n");
    write_text_file(root_source_path,
                    "mod test.cli.releaseapp;\n"
                    "use test.cli.releasedep;\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 7 {\n"
                    "  }\n"
                    "}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);

    {
        char *argv[] = { root_project_dir, "--release" };
        ASSERT(feng_cli_project_build_main("feng", 2, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(count_occurrences(cc_log_text, "__CMD__") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-O2") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-DNDEBUG") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-O0") == 0);
    ASSERT(count_occurrences(cc_log_text, "-g") == 0);

    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    free(saved_cc);
    free(cc_log_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(binary_path);
    free(root_source_path);
    free(root_src_dir);
    free(root_manifest_path);
    free(root_project_dir);
    free(dep_source_path);
    free(dep_src_dir);
    free(dep_manifest_path);
    free(dep_project_dir);
}

static void test_project_run_release_reuses_build_pipeline(void) {
    char template_path[] = "/tmp/feng_cli_run_release_flags_XXXXXX";
    char *workspace_dir;
    char *dep_project_dir;
    char *dep_manifest_path;
    char *dep_src_dir;
    char *dep_source_path;
    char *root_project_dir;
    char *root_manifest_path;
    char *root_src_dir;
    char *root_source_path;
    char *binary_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *cc_log_text;
    char *saved_cc = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_project_dir = path_join(workspace_dir, "dep");
    dep_manifest_path = path_join(dep_project_dir, "feng.fm");
    dep_src_dir = path_join(dep_project_dir, "src");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    root_src_dir = path_join(root_project_dir, "src");
    root_source_path = path_join(root_src_dir, "main.ff");
    binary_path = path_join(root_project_dir, "build/bin/run_app");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);

    mkdir_p(dep_src_dir);
    mkdir_p(root_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"run_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod test.cli.rundep;\n"
                    "pu fn dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(root_manifest_path,
                    "[package]\n"
                    "name: \"run_app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "run_dep: \"../dep\"\n");
    write_text_file(root_source_path,
                    "mod test.cli.runapp;\n"
                    "use test.cli.rundep;\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 7 {\n"
                    "  }\n"
                    "}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);

    {
        char *argv[] = { root_project_dir, "--release" };
        ASSERT(feng_cli_project_run_main("feng", 2, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(count_occurrences(cc_log_text, "__CMD__") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-O2") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-DNDEBUG") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-O0") == 0);
    ASSERT(count_occurrences(cc_log_text, "-g") == 0);

    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    free(saved_cc);
    free(cc_log_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(binary_path);
    free(root_source_path);
    free(root_src_dir);
    free(root_manifest_path);
    free(root_project_dir);
    free(dep_source_path);
    free(dep_src_dir);
    free(dep_manifest_path);
    free(dep_project_dir);
}

static void test_project_pack_uses_release_build_and_public_ft_excludes_spans(void) {
    char template_path[] = "/tmp/feng_cli_pack_release_flags_XXXXXX";
    char *workspace_dir;
    char *dep_project_dir;
    char *dep_manifest_path;
    char *dep_src_dir;
    char *dep_source_path;
    char *root_project_dir;
    char *root_manifest_path;
    char *root_src_dir;
    char *root_source_path;
    char *bundle_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *cc_log_text;
    char *saved_cc = NULL;
    char *remove_error = NULL;
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *ft_bytes = NULL;
    size_t ft_size = 0U;
    FengSymbolFtHeader header;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    dep_project_dir = path_join(workspace_dir, "dep");
    dep_manifest_path = path_join(dep_project_dir, "feng.fm");
    dep_src_dir = path_join(dep_project_dir, "src");
    dep_source_path = path_join(dep_src_dir, "lib.ff");
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    root_src_dir = path_join(root_project_dir, "src");
    root_source_path = path_join(root_src_dir, "lib.ff");
    bundle_path = path_join(root_project_dir, "build/rootlib-0.1.0.fb");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);

    mkdir_p(dep_src_dir);
    mkdir_p(root_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_source_path,
                    "pu mod local.dep;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");
    write_text_file(root_manifest_path,
                    "[package]\n"
                    "name: \"rootlib\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_dep: \"../dep\"\n");
    write_text_file(root_source_path,
                    "pu mod test.cli.packroot;\n"
                    "pu fn root_value(): int {\n"
                    "  return 2;\n"
                    "}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);

    {
        char *argv[] = { root_project_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(bundle_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(count_occurrences(cc_log_text, "__CMD__") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-O2") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-DNDEBUG") >= 2);
    ASSERT(count_occurrences(cc_log_text, "-O0") == 0);
    ASSERT(count_occurrences(cc_log_text, "-g") == 0);

    ASSERT(feng_zip_reader_open(bundle_path, &reader, &zip_error));
    ASSERT(feng_zip_reader_read(&reader,
                                "mod/test/cli/packroot.ft",
                                &ft_bytes,
                                &ft_size,
                                &zip_error));
    ASSERT(ft_size >= sizeof(FengSymbolFtHeader));
    memcpy(&header, ft_bytes, sizeof(header));
    ASSERT(header.profile == FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC);
    ASSERT((header.flags & FENG_SYMBOL_FT_FLAG_HAS_SPANS) == 0U);

    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    free(saved_cc);
    free(cc_log_text);
    feng_zip_free(ft_bytes);
    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(bundle_path);
    free(root_source_path);
    free(root_src_dir);
    free(root_manifest_path);
    free(root_project_dir);
    free(dep_source_path);
    free(dep_src_dir);
    free(dep_manifest_path);
    free(dep_project_dir);
}

static void test_project_pack_rejects_release_flag(void) {
    char template_path[] = "/tmp/feng_cli_pack_no_release_flag_XXXXXX";
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *remove_error = NULL;

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"packlib\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(source_path,
                    "pu mod test.cli.packnorelease;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");

    {
        char *argv[] = { project_dir, "--release" };
        ASSERT(feng_cli_project_pack_main("feng", 2, argv) != 0);
    }

    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(source_path);
    free(src_dir);
    free(manifest_path);
}

int main(void) {
    test_manifest_defaults();
    test_manifest_parses_dependencies_and_registry();
    test_manifest_rejects_duplicate_field();
    test_project_open_collects_sources();
    test_manifest_requires_target();
    test_bundle_manifest_allows_dependencies_without_target();
    test_bundle_manifest_rejects_local_path_dependency();
    test_deps_resolve_requires_registry_for_remote_dependency();
    test_deps_resolve_uses_global_registry_config();
    test_deps_resolve_installs_remote_transitive_dependencies();
    test_deps_resolve_builds_local_library_dependency();
    test_deps_resolve_reports_transitive_version_conflict();
    test_deps_resolve_reports_local_dependency_cycle();
    test_deps_add_remote_updates_manifest_and_cache();
    test_deps_add_local_validates_then_writes_manifest();
    test_deps_add_local_rejects_name_mismatch_before_write();
    test_deps_add_local_rejects_non_lib_target_before_write();
    test_deps_remove_updates_manifest();
    test_deps_install_populates_cache_from_registry();
    test_deps_install_force_refreshes_cached_bundle();
    test_init_creates_bin_project();
    test_init_creates_lib_project_using_current_directory_name();
    test_init_rejects_space_separated_target_value();
    test_init_prefixes_keyword_package_name();
    test_init_rejects_non_empty_directory();
    test_lsp_help_returns_success();
    test_lsp_rejects_unknown_option();
    test_lsp_publish_diagnostics_for_open_change_and_close();
    test_lsp_hover_definition_and_completion();
    test_lsp_member_references_and_rename_from_object_literal_field();
    test_lsp_project_cache_hit_survives_broken_dependency_source();
    test_direct_build_cleans_stale_ir_on_frontend_failure();
    test_direct_build_emits_symbol_tables();
    test_direct_build_accepts_package_bundle();
    test_direct_build_links_library_from_package_bundle();
    test_direct_build_sorts_package_libraries_by_dependency();
    test_project_pack_bundle_can_be_consumed();
    test_pack_bundle_manifest_rewrites_local_dependency_versions();
    test_project_check_accepts_source_file_path_and_local_dependencies();
    test_frontend_outputs_absolute_bundle_paths();
    test_frontend_source_overlay_replaces_disk_source();
    test_frontend_source_overlay_rejects_duplicate_paths();
    test_direct_build_rejects_bad_package_bundle();
    test_project_build_default_uses_debug_friendly_flags();
    test_project_build_release_propagates_to_local_dependencies();
    test_project_run_release_reuses_build_pipeline();
    test_project_pack_uses_release_build_and_public_ft_excludes_spans();
    test_project_pack_rejects_release_flag();
    fprintf(stdout, "cli tests passed\n");
    return 0;
}
