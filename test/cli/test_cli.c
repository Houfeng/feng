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

static char *run_binary_capture_stdout_or_die(const char *binary_path);

static char *build_single_source_package_bundle(const char *workspace_dir,
                                                const char *package_name,
                                                const char *source_text) {
    char *dep_src_dir;
    char *dep_source_path;
    char *dep_out_dir;
    char *dep_library_path;
    char *dep_mod_root;
    char *bundle_path;

    dep_src_dir = path_join(workspace_dir, "dep/src");
    dep_source_path = path_join(dep_src_dir, "dep.ff");
    dep_out_dir = path_join(workspace_dir, "dep/build");
    dep_library_path = dup_printf("%s/lib/lib%s.a", dep_out_dir, package_name);
    dep_mod_root = path_join(dep_out_dir, "mod");
    bundle_path = dup_printf("%s/%s.fb", workspace_dir, package_name);

    mkdir_p(dep_src_dir);
    write_text_file(dep_source_path, source_text);
    {
        char *out_opt = make_out_option(dep_out_dir);
        char *name_opt = dup_printf("--name=%s", package_name);
        char *argv[] = {
            dep_source_path,
            "--target=lib",
            out_opt,
            name_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 4, argv) == 0);
        free(name_opt);
        free(out_opt);
    }
    ASSERT(path_exists(dep_library_path));
    ASSERT(path_exists(dep_mod_root));
    write_library_bundle_or_die(bundle_path,
                                package_name,
                                "0.1.0",
                                dep_library_path,
                                dep_mod_root);

    free(dep_mod_root);
    free(dep_library_path);
    free(dep_out_dir);
    free(dep_source_path);
    free(dep_src_dir);
    return bundle_path;
}

static void compile_consumer_with_package_and_expect_stdout(const char *workspace_dir,
                                                            const char *bundle_path,
                                                            const char *source_text,
                                                            const char *binary_name,
                                                            const char *expected_stdout) {
    char *main_src_dir;
    char *main_source_path;
    char *main_out_dir;
    char *main_binary_path;
    char *stdout_text;

    main_src_dir = path_join(workspace_dir, "main/src");
    main_source_path = path_join(main_src_dir, "main.ff");
    main_out_dir = path_join(workspace_dir, "main/build");
    main_binary_path = dup_printf("%s/bin/%s", main_out_dir, binary_name);

    mkdir_p(main_src_dir);
    write_text_file(main_source_path, source_text);
    {
        char *out_opt = make_out_option(main_out_dir);
        char *name_opt = dup_printf("--name=%s", binary_name);
        char *pkg_opt = make_pkg_option(bundle_path);
        char *argv[] = {
            main_source_path,
            "--target=bin",
            out_opt,
            name_opt,
            pkg_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
        free(pkg_opt);
        free(name_opt);
        free(out_opt);
    }

    ASSERT(path_exists(main_binary_path));
    stdout_text = run_binary_capture_stdout_or_die(main_binary_path);
    ASSERT(strcmp(stdout_text, expected_stdout) == 0);

    free(stdout_text);
    free(main_binary_path);
    free(main_out_dir);
    free(main_source_path);
    free(main_src_dir);
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

static char *read_text_stream(FILE *file);

static char *run_deps_capture_stderr(int argc, char **argv, int *out_rc) {
    int saved_stderr;
    FILE *errors = tmpfile();
    int rc;
    char *captured;

    ASSERT(errors != NULL);
    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stderr >= 0);
    ASSERT(dup2(fileno(errors), STDERR_FILENO) >= 0);

    rc = feng_cli_deps_main("feng", argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    captured = read_text_stream(errors);
    fclose(errors);
    if (out_rc != NULL) {
        *out_rc = rc;
    }
    return captured;
}

static char *run_project_check_capture_stderr(int argc, char **argv, int *out_rc) {
    int saved_stderr;
    FILE *errors = tmpfile();
    int rc;
    char *captured;

    ASSERT(errors != NULL);
    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stderr >= 0);
    ASSERT(dup2(fileno(errors), STDERR_FILENO) >= 0);

    rc = feng_cli_project_check_main("feng", argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    captured = read_text_stream(errors);
    fclose(errors);
    if (out_rc != NULL) {
        *out_rc = rc;
    }
    return captured;
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

static size_t count_substring(const char *text, const char *needle) {
    size_t count = 0U;
    size_t needle_length;
    const char *cursor;

    ASSERT(text != NULL);
    ASSERT(needle != NULL);
    needle_length = strlen(needle);
    ASSERT(needle_length > 0U);
    cursor = text;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        ++count;
        cursor += needle_length;
    }
    return count;
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
                    "extern fn puts(msg: string*): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 7 { puts(&\"ok\"); } else { puts(&\"bad\"); }\n"
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
                    "extern fn puts(msg: string*): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if a.a_value() == 11 { puts(&\"ok\"); } else { puts(&\"bad\"); }\n"
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
                    "extern fn puts(msg: string*): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if dep_value() == 9 { puts(&\"ok\"); } else { puts(&\"bad\"); }\n"
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

static void test_direct_build_consumes_package_generic_function(void) {
    char template_path[] = "/tmp/feng_cli_pkg_generic_fn_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkggenericfn",
        "pu mod test.cli.pkggenericfn;\n"
        "pu fn identity<T>(value: T): T {\n"
        "  return value;\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "mod test.cli.pkggenericfnmain;\n"
        "use test.cli.pkggenericfn;\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "fn main(args: string[]) {\n"
        "  if identity(7) == 7 { puts(&\"generic fn ok\"); }\n"
        "}\n",
        "generic_fn_main",
        "generic fn ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_direct_build_consumes_package_generic_type(void) {
    char template_path[] = "/tmp/feng_cli_pkg_generic_type_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkggenerictype",
        "pu mod test.cli.pkggenerictype;\n"
        "pu type Box<T> {\n"
        "  var value: T;\n"
        "  pu fn setValue(next: T) {\n"
        "    self.value = next;\n"
        "  }\n"
        "  pu fn readValue(): T {\n"
        "    return self.value;\n"
        "  }\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "mod test.cli.pkggenerictypemain;\n"
        "use test.cli.pkggenerictype;\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "fn main(args: string[]) {\n"
        "  let box: Box<int> = Box<int>();\n"
        "  box.setValue(11);\n"
        "  if box.readValue() == 11 { puts(&\"generic type ok\"); }\n"
        "}\n",
        "generic_type_main",
        "generic type ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_direct_build_consumes_package_enum(void) {
    char template_path[] = "/tmp/feng_cli_pkg_enum_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgenum",
        "pu mod test.cli.pkgenum;\n"
        "pu enum HttpStatus {\n"
        "  Ok = 200,\n"
        "  NotFound = 404\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "mod test.cli.pkgenummain;\n"
        "use test.cli.pkgenum;\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "fn main(args: string[]) {\n"
        "  let status: HttpStatus;\n"
        "  if status == HttpStatus.Ok { puts(&\"enum package ok\"); }\n"
        "}\n",
        "enum_pkg_main",
        "enum package ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_direct_build_consumes_package_generic_spec_constraint(void) {
    char template_path[] = "/tmp/feng_cli_pkg_generic_spec_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkggenericspec",
        "pu mod test.cli.pkggenericspec;\n"
        "pu spec Eq<T> {\n"
        "  fn same(other: T): bool;\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "mod test.cli.pkggenericspecmain;\n"
        "use test.cli.pkggenericspec;\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "type Key: Eq<Key> {\n"
        "  var id: int;\n"
        "  fn same(other: Key): bool {\n"
        "    return self.id == other.id;\n"
        "  }\n"
        "}\n"
        "fn sameLocal<T: Eq<T>>(left: T, right: T): bool {\n"
        "  return left.same(right);\n"
        "}\n"
        "fn main(args: string[]) {\n"
        "  let a = Key{id: 3};\n"
        "  let b = Key{id: 3};\n"
        "  if sameLocal(a, b) { puts(&\"generic spec ok\"); }\n"
        "}\n",
        "generic_spec_main",
        "generic spec ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_direct_build_consumes_package_constrained_generic_function(void) {
    char template_path[] = "/tmp/feng_cli_pkg_constrained_generic_fn_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgconstrainedgenericfn",
        "pu mod test.cli.pkgconstrainedgenericfn;\n"
        "pu spec Eq<T> {\n"
        "  fn same(other: T): bool;\n"
        "}\n"
        "pu fn sameAs<T: Eq<T>>(left: T, right: T): bool {\n"
        "  return left.same(right);\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "mod test.cli.pkgconstrainedgenericfnmain;\n"
        "use test.cli.pkgconstrainedgenericfn;\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "type Key: Eq<Key> {\n"
        "  var id: int;\n"
        "  fn same(other: Key): bool {\n"
        "    return self.id == other.id;\n"
        "  }\n"
        "}\n"
        "fn main(args: string[]) {\n"
        "  let a = Key{id: 5};\n"
        "  let b = Key{id: 5};\n"
        "  if sameAs(a, b) { puts(&\"constrained generic fn ok\"); }\n"
        "}\n",
        "constrained_generic_fn_main",
        "constrained generic fn ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_direct_build_consumes_package_constrained_generic_type(void) {
    char template_path[] = "/tmp/feng_cli_pkg_constrained_generic_type_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgconstrainedgenerictype",
        "pu mod test.cli.pkgconstrainedgenerictype;\n"
        "pu spec Eq<T> {\n"
        "  fn same(other: T): bool;\n"
        "}\n"
        "pu type MiniMap<K: Eq<K>, V> {\n"
        "  var key: K;\n"
        "  var value: V;\n"
        "  pu fn put(key: K, value: V) {\n"
        "    self.key = key;\n"
        "    self.value = value;\n"
        "  }\n"
        "  pu fn hasKey(key: K): bool {\n"
        "    return key.same(self.key);\n"
        "  }\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "mod test.cli.pkgconstrainedgenerictypemain;\n"
        "use test.cli.pkgconstrainedgenerictype;\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "type Key: Eq<Key> {\n"
        "  var id: int;\n"
        "  fn same(other: Key): bool {\n"
        "    return self.id == other.id;\n"
        "  }\n"
        "}\n"
        "fn main(args: string[]) {\n"
        "  let a = Key{id: 8};\n"
        "  let b = Key{id: 8};\n"
        "  let map: MiniMap<Key, string> = MiniMap<Key, string>();\n"
        "  map.put(a, \"value\");\n"
        "  if map.hasKey(b) { puts(&\"constrained generic type ok\"); }\n"
        "}\n",
        "constrained_generic_type_main",
        "constrained generic type ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
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

static void test_project_check_reports_enum_semantic_error_without_unknown_type(void) {
    char template_path[] = "/tmp/feng_cli_check_enum_diag_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *stderr_text;
    char *remove_error = NULL;
    int rc = 0;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "root");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"enum_diag_app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(source_path,
                    "mod test.cli.enumdiag;\n"
                    "enum Status {\n"
                    "  Ok,\n"
                    "  NotFound\n"
                    "}\n"
                    "fn main(args: string[]) {\n"
                    "  let value: Status = (Status)1;\n"
                    "}\n");

    {
        char *argv[] = { source_path };
        stderr_text = run_project_check_capture_stderr(1, argv, &rc);
    }

    ASSERT(rc != 0);
    ASSERT(strstr(stderr_text, "to 'Status' is not allowed") != NULL);
    ASSERT(strstr(stderr_text, "unknown type 'Status'") == NULL);

    free(stderr_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
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
    ASSERT(strstr(output, "\"triggerCharacters\":[\".\",\"_\",\"a\"") != NULL);
    ASSERT(strstr(output, "\"Z\"") != NULL);
    ASSERT(strstr(output, "\"kind\":\"plaintext\"") != NULL);
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

static char *capture_lsp_completion_response(const char *source,
                                             const char *needle,
                                             size_t char_offset) {
    char template_path[] = "/tmp/feng_cli_lsp_completion_incomplete_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int line;
    unsigned int character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, source);

    find_line_character(source, needle, char_offset, &line, &character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(source);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                uri,
                                line,
                                character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
    return output;
}

static char *capture_lsp_hover_response(const char *source,
                                        const char *initialize,
                                        const char *needle,
                                        size_t char_offset) {
    char template_path[] = "/tmp/feng_cli_lsp_hover_markup_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *did_open;
    char *hover_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int line;
    unsigned int character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, source);

    find_line_character(source, needle, char_offset, &line, &character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(source);
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    hover_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                           uri,
                           line,
                           character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);
    free(shutdown);
    free(hover_req);
    free(did_open);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
    return output;
}

static char *capture_lsp_position_response_at_path(const char *source_path,
                                                   const char *source,
                                                   const char *initialize,
                                                   const char *method,
                                                   const char *needle,
                                                   size_t char_offset) {
    char *uri;
    char *escaped_text;
    char *did_open;
    char *request;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int line;
    unsigned int character;

    find_line_character(source, needle, char_offset, &line, &character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(source);
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    request = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                         method,
                         uri,
                         line,
                         character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, request);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    free(shutdown);
    free(request);
    free(did_open);
    free(escaped_text);
    free(uri);
    return output;
}

static void test_lsp_hover_uses_markdown_when_supported(void) {
    static const char *kSource =
        "mod test.lsp.markdown;\n"
        "\n"
        "/**\n"
        " * Summarizes the CLI arguments.\n"
        " *\n"
        " * @param args The command-line arguments.\n"
        " */\n"
        "fn describe(args: string[]): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let label: string = describe(args);\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]}}}}}";
    char *output = capture_lsp_hover_response(kSource,
                                              kInitialize,
                                              "let label: string = describe(args);",
                                              20U);

    ASSERT(strstr(output, "\"id\":2,\"result\":{\"contents\":{\"kind\":\"markdown\"") != NULL);
    ASSERT(strstr(output, "```feng\\nfn describe(args: string[]): string\\n```") != NULL);
    ASSERT(strstr(output, "Summarizes the CLI arguments.") != NULL);
    ASSERT(strstr(output, "- **@param** `args` The command-line arguments.") != NULL);

    free(output);
}

static void test_lsp_hover_falls_back_to_plaintext_without_markdown_capability(void) {
    static const char *kSource =
        "mod test.lsp.plaintext;\n"
        "\n"
        "/**\n"
        " * Summarizes the CLI arguments.\n"
        " *\n"
        " * @param args The command-line arguments.\n"
        " */\n"
        "fn describe(args: string[]): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let label: string = describe(args);\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output = capture_lsp_hover_response(kSource,
                                              kInitialize,
                                              "let label: string = describe(args);",
                                              20U);

    ASSERT(strstr(output, "\"id\":2,\"result\":{\"contents\":{\"kind\":\"plaintext\"") != NULL);
    ASSERT(strstr(output, "fn describe(args: string[]): string") != NULL);
    ASSERT(strstr(output, "@param args The command-line arguments.") != NULL);
    ASSERT(strstr(output, "**@param**") == NULL);
    ASSERT(strstr(output, "```feng") == NULL);

    free(output);
}

static void assert_lsp_completion_contains_labels(const char *source,
                                                  const char *needle,
                                                  size_t char_offset,
                                                  const char **labels,
                                                  size_t label_count) {
    char *output = capture_lsp_completion_response(source, needle, char_offset);
    size_t index;

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    for (index = 0U; index < label_count; ++index) {
        char *expected = dup_printf("\"label\":\"%s\"", labels[index]);

        ASSERT(strstr(output, expected) != NULL);
        free(expected);
    }

    free(output);
}

static void assert_lsp_completion_contains_name(const char *source,
                                                const char *needle,
                                                size_t char_offset) {
    const char *labels[] = {"name"};

    assert_lsp_completion_contains_labels(source, needle, char_offset, labels, 1U);
}

static void test_lsp_member_completion_survives_incomplete_member_access(void) {
    static const char *kDotSource =
        "mod test.lsp.completiondot;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let label: string = user.;\n"
        "}\n";
    static const char *kPrefixSource =
        "mod test.lsp.completionprefix;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let label: string = user.n;\n"
        "}\n";
    static const char *kInferredSource =
        "mod test.lsp.completioninferred;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user = User { name: \"copilot\" };\n"
        "    let label: string = user.;\n"
        "}\n";

    assert_lsp_completion_contains_name(kDotSource, "user.;", 5U);
    assert_lsp_completion_contains_name(kPrefixSource, "user.n;", 6U);
    assert_lsp_completion_contains_name(kInferredSource, "user.;", 5U);
}
static void test_lsp_enum_member_completion_survives_incomplete_member_access(void) {
    static const char *kDotSource =
        "mod test.lsp.enumcompletiondot;\n"
        "\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let status: HttpStatus = HttpStatus.;\n"
        "}\n";
    static const char *kPrefixSource =
        "mod test.lsp.enumcompletionprefix;\n"
        "\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let status: HttpStatus = HttpStatus.N;\n"
        "}\n";
    const char *labels[] = {"Ok", "NotFound"};
    const char *prefix_labels[] = {"NotFound"};

    assert_lsp_completion_contains_labels(kDotSource, "HttpStatus.;", 11U, labels, 2U);
    assert_lsp_completion_contains_labels(kPrefixSource, "HttpStatus.N;", 12U, prefix_labels, 1U);
}

static void test_lsp_completion_uses_source_scoped_edit_context(void) {
    static const char *kMemberBeforeNextStmt =
        "mod test.lsp.completioneditmember;\n"
        "\n"
        "extern fn puts(msg: string*): int;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "    let age: i32;\n"
        "\n"
        "    fn say(msg: string): void {\n"
        "        puts(&msg);\n"
        "    }\n"
        "}\n"
        "\n"
        "fn hello_world_example(args: string[]): void {\n"
        "    let user = User {\n"
        "        name: \"Houfeng\",\n"
        "        age: 18\n"
        "    };\n"
        "    user.\n"
        "    user.say(\"Hello World: \" + user.name);\n"
        "}\n";
    static const char *kScopeBeforeClose =
        "mod test.lsp.completioneditscope;\n"
        "\n"
        "extern fn puts(msg: string*): int;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "    let age: i32;\n"
        "\n"
        "    fn say(msg: string): void {\n"
        "        puts(&msg);\n"
        "    }\n"
        "}\n"
        "\n"
        "fn hello_world_example(args: string[]): void {\n"
        "    let user = User {\n"
        "        name: \"Houfeng\",\n"
        "        age: 18\n"
        "    };\n"
        "    user.say(\"Hello World: \" + user.name);\n"
        "    \n"
        "}\n";
    static const char *kScopePrefixBeforeNextStmt =
        "mod test.lsp.completioneditprefix;\n"
        "\n"
        "extern fn puts(msg: string*): int;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fn hello_world_example(args: string[]): void {\n"
        "    let user = User { name: \"Houfeng\" };\n"
        "    us\n"
        "    user.say(\"Hello World\");\n"
        "}\n";
    static const char *kMainPrefixBeforeClose =
        "mod test.lsp.completionmainprefix;\n"
        "\n"
        "@cdecl(\"libc\")\n"
        "extern fn puts(msg: string*): int;\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    puts(&\"hello, examples\");\n"
        "    p\n"
        "}\n";
    const char *member_labels[] = {"name", "age", "say"};
    const char *scope_labels[] = {"args", "user", "puts", "User", "hello_world_example"};
    const char *prefix_labels[] = {"user", "User", "puts", "hello_world_example"};
    const char *main_prefix_labels[] = {"args", "puts", "main"};

    assert_lsp_completion_contains_labels(kMemberBeforeNextStmt,
                                          "user.\n    user.say",
                                          5U,
                                          member_labels,
                                          sizeof(member_labels) / sizeof(member_labels[0]));
    assert_lsp_completion_contains_labels(kScopeBeforeClose,
                                          "    \n}\n",
                                          4U,
                                          scope_labels,
                                          sizeof(scope_labels) / sizeof(scope_labels[0]));
    assert_lsp_completion_contains_labels(kScopePrefixBeforeNextStmt,
                                          "us\n    user.say",
                                          2U,
                                          prefix_labels,
                                          sizeof(prefix_labels) / sizeof(prefix_labels[0]));
    assert_lsp_completion_contains_labels(kMainPrefixBeforeClose,
                                          "p\n}\n",
                                          1U,
                                          main_prefix_labels,
                                          sizeof(main_prefix_labels) / sizeof(main_prefix_labels[0]));
}

static void test_lsp_member_completion_infers_constructor_call_overloads(void) {
    static const char *kSource =
        "mod test.lsp.completionoverload;\n"
        "\n"
        "spec CommitOptions {\n"
        "    var message: i32;\n"
        "}\n"
        "\n"
        "type User {\n"
        "    fn commit(options: CommitOptions): void {\n"
        "        options.message = 1;\n"
        "    }\n"
        "\n"
        "    fn commit(message: i32): int {\n"
        "        return message;\n"
        "    }\n"
        "}\n"
        "\n"
        "fn debug_example(args: string[]): void {\n"
        "    let user = User();\n"
        "    user.co\n"
        "}\n";
    char *output = capture_lsp_completion_response(kSource, "user.co", 7U);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    ASSERT(count_occurrences(output, "\"label\":\"commit\"") == 2);
    ASSERT(strstr(output, "fn commit(options: CommitOptions): void") != NULL);
    ASSERT(strstr(output, "fn commit(message: i32): int") != NULL);

    free(output);
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

static void test_lsp_function_decl_site_definition_references_and_rename(void) {
    static const char *kSource =
        "mod test.lsp.declsite;\n"
        "\n"
        "fn helper(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    helper(1);\n"
        "    helper(2);\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_decl_site_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *definition_decl;
    char *references_decl;
    char *prepare_rename_decl;
    char *rename_decl;
    char *shutdown;
    char *output;
    char *expected_definition;
    char *expected_decl_loc;
    char *expected_first_call_loc;
    char *expected_second_call_loc;
    char *expected_prepare;
    FILE *input;
    unsigned int decl_line;
    unsigned int decl_character;
    unsigned int first_call_line;
    unsigned int first_call_character;
    unsigned int second_call_line;
    unsigned int second_call_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);

    /* Cursor is in the middle of the `helper` name token at its declaration. */
    find_line_character(kSource,
                        "fn helper(x: int): int {",
                        3U,
                        &decl_line,
                        &decl_character);
    find_line_character(kSource,
                        "    helper(1);",
                        4U,
                        &first_call_line,
                        &first_call_character);
    find_line_character(kSource,
                        "    helper(2);",
                        4U,
                        &second_call_line,
                        &second_call_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    definition_decl = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                 uri,
                                 decl_line,
                                 decl_character + 2U);
    references_decl = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/references\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"context\":{\"includeDeclaration\":true}}}",
                                 uri,
                                 decl_line,
                                 decl_character + 2U);
    prepare_rename_decl = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                     uri,
                                     decl_line,
                                     decl_character + 2U);
    rename_decl = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/rename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"newName\":\"renamed\"}}",
                             uri,
                             decl_line,
                             decl_character + 2U);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");
    expected_definition = dup_printf("\"id\":2,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                     uri,
                                     decl_line,
                                     decl_character);
    expected_decl_loc = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                   uri,
                                   decl_line,
                                   decl_character);
    expected_first_call_loc = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                         uri,
                                         first_call_line,
                                         first_call_character);
    expected_second_call_loc = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                          uri,
                                          second_call_line,
                                          second_call_character);
    expected_prepare = dup_printf("\"id\":4,\"result\":{\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                  decl_line,
                                  decl_character);

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, definition_decl);
    write_lsp_message(input, references_decl);
    write_lsp_message(input, prepare_rename_decl);
    write_lsp_message(input, rename_decl);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, expected_definition) != NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":[") != NULL);
    ASSERT(strstr(output, expected_decl_loc) != NULL);
    ASSERT(strstr(output, expected_first_call_loc) != NULL);
    ASSERT(strstr(output, expected_second_call_loc) != NULL);
    ASSERT(strstr(output, expected_prepare) != NULL);
    ASSERT(strstr(output, "\"placeholder\":\"helper\"") != NULL);
    ASSERT(strstr(output, "\"id\":5,\"result\":{\"changes\":{") != NULL);
    ASSERT(count_occurrences(output, "\"newText\":\"renamed\"") == 3);

    free(output);
    free(expected_prepare);
    free(expected_second_call_loc);
    free(expected_first_call_loc);
    free(expected_decl_loc);
    free(expected_definition);
    free(shutdown);
    free(rename_decl);
    free(prepare_rename_decl);
    free(references_decl);
    free(definition_decl);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

static void test_lsp_rename_accepts_identifier_end_position(void) {
    static const char *kSource =
        "mod test.lsp.renameend;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_rename_end_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *prepare_local_end;
    char *rename_local_end;
    char *prepare_use_end;
    char *prepare_field_end;
    char *shutdown;
    char *output;
    char *expected_local_prepare;
    char *expected_use_prepare;
    char *expected_field_prepare;
    FILE *input;
    unsigned int local_line;
    unsigned int local_character;
    unsigned int local_end_line;
    unsigned int local_end_character;
    unsigned int use_line;
    unsigned int use_character;
    unsigned int use_end_line;
    unsigned int use_end_character;
    unsigned int field_line;
    unsigned int field_character;
    unsigned int field_end_line;
    unsigned int field_end_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);

    find_line_character(kSource, "let user:", 4U, &local_line, &local_character);
    find_line_character(kSource, "let user:", 8U, &local_end_line, &local_end_character);
    find_line_character(kSource, "user.name", 0U, &use_line, &use_character);
    find_line_character(kSource, "user.name", 4U, &use_end_line, &use_end_character);
    find_line_character(kSource, "user.name", 5U, &field_line, &field_character);
    find_line_character(kSource, "user.name", 9U, &field_end_line, &field_end_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    prepare_local_end = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                   uri,
                                   local_end_line,
                                   local_end_character);
    rename_local_end = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/rename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"newName\":\"account\"}}",
                                  uri,
                                  local_end_line,
                                  local_end_character);
    prepare_use_end = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                 uri,
                                 use_end_line,
                                 use_end_character);
    prepare_field_end = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                   uri,
                                   field_end_line,
                                   field_end_character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    expected_local_prepare = dup_printf("\"id\":2,\"result\":{\"range\":{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}},\"placeholder\":\"user\"",
                                        local_line,
                                        local_character,
                                        local_end_line,
                                        local_end_character);
    expected_use_prepare = dup_printf("\"id\":4,\"result\":{\"range\":{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}},\"placeholder\":\"user\"",
                                      use_line,
                                      use_character,
                                      use_end_line,
                                      use_end_character);
    expected_field_prepare = dup_printf("\"id\":5,\"result\":{\"range\":{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}},\"placeholder\":\"name\"",
                                        field_line,
                                        field_character,
                                        field_end_line,
                                        field_end_character);

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, prepare_local_end);
    write_lsp_message(input, rename_local_end);
    write_lsp_message(input, prepare_use_end);
    write_lsp_message(input, prepare_field_end);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, expected_local_prepare) != NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":{\"changes\":{") != NULL);
    ASSERT(count_occurrences(output, "\"newText\":\"account\"") == 2);
    ASSERT(strstr(output, expected_use_prepare) != NULL);
    ASSERT(strstr(output, expected_field_prepare) != NULL);

    free(output);
    free(expected_field_prepare);
    free(expected_use_prepare);
    free(expected_local_prepare);
    free(shutdown);
    free(prepare_field_end);
    free(prepare_use_end);
    free(rename_local_end);
    free(prepare_local_end);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

/* Verify that definition/references/rename return results even when the file
   has a semantic error, as long as the target symbol is resolvable.  The key
   fix was removing the `exit_code != 0` guard that used to return null
   immediately — now the handlers try to resolve before giving up.
   In this test the cursor is at the declaration site of `helper`.  Even though
   there is a type-mismatch semantic error elsewhere in the file, cursoring at
   the declaration itself (where LSP can resolve FENG_LSP_RESOLVED_DECL from the
   declaration token) must still succeed. */
static void test_lsp_definition_references_rename_with_broken_code(void) {
    static const char *kSource =
        "mod test.lsp.broken;\n"
        "\n"
        "fn helper(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "\n"
        "fn main(args: string[]) {\n"
        "    helper(1);\n"
        "    helper(2);\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_broken_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *definition_req;
    char *references_req;
    char *prepare_rename_req;
    char *rename_req;
    char *shutdown;
    char *output;
    char *expected_definition;
    char *expected_decl_loc;
    char *expected_first_call_loc;
    char *expected_second_call_loc;
    FILE *input;
    unsigned int decl_line;
    unsigned int decl_character;
    unsigned int first_call_line;
    unsigned int first_call_character;
    unsigned int second_call_line;
    unsigned int second_call_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);

    find_line_character(kSource,
                        "fn helper(x: int): int {",
                        3U,
                        &decl_line,
                        &decl_character);
    find_line_character(kSource,
                        "    helper(1);",
                        4U,
                        &first_call_line,
                        &first_call_character);
    find_line_character(kSource,
                        "    helper(2);",
                        4U,
                        &second_call_line,
                        &second_call_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    /* Cursor at the declaration site — definition must resolve even if
       the file is later changed to have unsaved semantic errors. */
    definition_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                uri,
                                decl_line,
                                decl_character + 2U);
    references_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/references\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"context\":{\"includeDeclaration\":true}}}",
                                uri,
                                decl_line,
                                decl_character + 2U);
    prepare_rename_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                    uri,
                                    decl_line,
                                    decl_character + 2U);
    rename_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/rename\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u},\"newName\":\"fixed\"}}",
                            uri,
                            decl_line,
                            decl_character + 2U);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    expected_definition = dup_printf("\"id\":2,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                     uri,
                                     decl_line,
                                     decl_character);
    expected_decl_loc = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                   uri,
                                   decl_line,
                                   decl_character);
    expected_first_call_loc = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                         uri,
                                         first_call_line,
                                         first_call_character);
    expected_second_call_loc = dup_printf("\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                          uri,
                                          second_call_line,
                                          second_call_character);

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, definition_req);
    write_lsp_message(input, references_req);
    write_lsp_message(input, prepare_rename_req);
    write_lsp_message(input, rename_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    /* Definition must resolve to the declaration site. */
    ASSERT(strstr(output, expected_definition) != NULL);
    /* References must include all three locations. */
    ASSERT(strstr(output, "\"id\":3,\"result\":[") != NULL);
    ASSERT(strstr(output, expected_decl_loc) != NULL);
    ASSERT(strstr(output, expected_first_call_loc) != NULL);
    ASSERT(strstr(output, expected_second_call_loc) != NULL);
    /* Rename must succeed. */
    ASSERT(strstr(output, "\"id\":5,\"result\":{\"changes\":{") != NULL);
    ASSERT(count_occurrences(output, "\"newText\":\"fixed\"") == 3);

    free(output);
    free(expected_second_call_loc);
    free(expected_first_call_loc);
    free(expected_decl_loc);
    free(expected_definition);
    free(shutdown);
    free(rename_req);
    free(prepare_rename_req);
    free(references_req);
    free(definition_req);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

static void test_lsp_no_crash_on_library_file_without_main(void) {
    /* Regression test: opening a file with no fn main in standalone mode used to
       cause a SIGSEGV because the BIN-target semantic analysis generated a
       "missing main" error with a NULL path, which was then passed to strcmp
       inside feng_cli_find_loaded_source.  The server must survive and return
       valid JSON responses for all requests. */
    static const char *kSource =
        "mod test.lsp.libonly;\n"
        "\n"
        "/** A counter type with no main function. */\n"
        "pu type Counter {\n"
        "    /** The count field. */\n"
        "    pu let count: int;\n"
        "    /** Returns double the count. */\n"
        "    pu fn double(): int {\n"
        "        return self.count * 2;\n"
        "    }\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_lsp_libonly_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *hover_req;
    char *shutdown;
    char *output;
    unsigned int field_line;
    unsigned int field_character;
    char *remove_error = NULL;
    FILE *input;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "counter.ff");
    write_text_file(source_path, kSource);

    find_line_character(kSource, "    pu let count: int;", 11U,
                        &field_line, &field_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);

    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          uri, escaped_text);
    hover_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\","
                           "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                           "\"position\":{\"line\":%u,\"character\":%u}}}",
                           uri, field_line, field_character + 7U);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    /* Server must not crash — initialize response must be present. */
    ASSERT(strstr(output, "\"id\":1,\"result\":{\"capabilities\":{") != NULL);
    /* Hover must return a valid JSON response (result may be null or a value). */
    ASSERT(strstr(output, "\"id\":2,\"result\":") != NULL);

    free(output);
    free(shutdown);
    free(hover_req);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

static void test_lsp_didopen_handles_unicode_escape_in_source(void) {
    /* Regression test: json_string_dup used to return NULL when the JSON text
       field contained \\uXXXX escape sequences (e.g. produced by Python's
       json.dumps with ensure_ascii=True for non-ASCII source content like
       Chinese doc comments).  The server must survive and return valid
       JSON responses. */
    char template_path[] = "/tmp/feng_cli_lsp_unicode_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *initialize;
    char *did_open;
    char *shutdown;
    char *output;
    FILE *input;
    char *remove_error = NULL;

    /* Source with a Chinese doc comment, encoded as \uXXXX in the JSON text
       field to simulate a client that escapes non-ASCII characters (e.g.
       Python json.dumps with ensure_ascii=True).  The string below is what
       the LSP server actually receives over the wire:
         backslash-u6d4b backslash-u8bd5 is the Unicode encoding of: 测试 */
    static const char *kSourceEscaped =
        "mod test.lsp.unicode;\\n"
        "/** \\u6d4b\\u8bd5 */\\n"
        "pu type Tag { pu let name: string; }\\n";

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "tag.ff");
    /* Write the actual source to disk so the LSP can find it; the on-disk
       version uses real UTF-8, the in-message version uses \\uXXXX. */
    write_text_file(source_path,
                    "mod test.lsp.unicode;\n"
                    "/** \xe6\xb5\x8b\xe8\xaf\x95 */\n"
                    "pu type Tag { pu let name: string; }\n");

    uri = file_uri_from_path(source_path);

    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    /* didOpen text uses \\uXXXX escapes — the server must handle them. */
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          uri, kSourceEscaped);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    /* Server must not exit early — initialize response and shutdown must be present. */
    ASSERT(strstr(output, "\"id\":1,\"result\":{\"capabilities\":{") != NULL);
    ASSERT(strstr(output, "\"id\":9,\"result\":null") != NULL);

    free(output);
    free(shutdown);
    free(did_open);
    free(initialize);
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
    ASSERT(strstr(output, "\"label\":\"name\"") == NULL);

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
    ASSERT(strstr(error.message, "remote_dep@1.0.0") != NULL);
    ASSERT(strstr(error.message, "no configured registry available") != NULL);

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

static void test_deps_install_reports_download_failure_with_reason(void) {
    char template_path[] = "/tmp/feng_cli_deps_install_missing_bundle_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *registry_dir;
    char *packages_dir;
    char *manifest_path;
    char *source_path;
    char *cache_path;
    char *saved_home = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    manifest_path = path_join(project_dir, "feng.fm");
    source_path = path_join(packages_dir, "remote_dep-1.0.0.fb");
    cache_path = path_join(workspace_dir, ".feng/cache/remote_dep-1.0.0.fb");

    mkdir_p(project_dir);
    mkdir_p(packages_dir);
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

    ASSERT(!feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "remote_dep@1.0.0") != NULL);
    ASSERT(strstr(error.message, "registry/packages/remote_dep-1.0.0.fb") != NULL);
    ASSERT(strstr(error.message, "No such file or directory") != NULL);
    ASSERT(!path_exists(cache_path));

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(source_path);
    free(manifest_path);
    free(packages_dir);
    free(registry_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_install_rejects_invalid_downloaded_bundle(void) {
    char template_path[] = "/tmp/feng_cli_deps_install_invalid_bundle_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *registry_dir;
    char *packages_dir;
    char *manifest_path;
    char *bundle_path;
    char *cache_path;
    char *saved_home = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};

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
                                      "name: \"other_dep\"\n"
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

    ASSERT(!feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "remote_dep@1.0.0") != NULL);
    ASSERT(strstr(error.message, "invalid package bundle") != NULL);
    ASSERT(strstr(error.message, "dependency name mismatch") != NULL);
    ASSERT(strstr(error.message, "other_dep") != NULL);
    ASSERT(!path_exists(cache_path));

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(bundle_path);
    free(manifest_path);
    free(packages_dir);
    free(registry_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_add_local_bundle_error_reports_dependency_context(void) {
    char template_path[] = "/tmp/feng_cli_deps_add_local_bundle_error_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *dep_dir;
    char *project_manifest_path;
    char *dep_bundle_path;
    char *manifest_text;
    char *stderr_text;
    char *remove_error = NULL;
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    dep_dir = path_join(workspace_dir, "dep");
    project_manifest_path = path_join(project_dir, "feng.fm");
    dep_bundle_path = path_join(dep_dir, "local_dep.fb");

    mkdir_p(project_dir);
    mkdir_p(dep_dir);
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(dep_bundle_path, "not a bundle\n");

    {
        char *argv[] = { "add", "local_dep", "../dep/local_dep.fb", project_dir };
        stderr_text = run_deps_capture_stderr(4, argv, &rc);
    }

    ASSERT(rc != 0);
    ASSERT(strstr(stderr_text,
                  "failed to validate local dependency local_dep declared as \"../dep/local_dep.fb\"") != NULL);
    ASSERT(strstr(stderr_text, "failed to open bundle") != NULL);
    manifest_text = read_text_file(project_manifest_path);
    ASSERT(strstr(manifest_text, "[dependencies]") == NULL);

    free(stderr_text);
    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_bundle_path);
    free(project_manifest_path);
    free(dep_dir);
    free(project_dir);
}

static void test_deps_install_local_dependency_error_reports_dependency_context(void) {
    char template_path[] = "/tmp/feng_cli_deps_install_local_error_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *dep_dir;
    char *project_manifest_path;
    char *dep_manifest_path;
    char *stderr_text;
    char *remove_error = NULL;
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    dep_dir = path_join(workspace_dir, "dep");
    project_manifest_path = path_join(project_dir, "feng.fm");
    dep_manifest_path = path_join(dep_dir, "feng.fm");

    mkdir_p(project_dir);
    mkdir_p(dep_dir);
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_dep: \"../dep\"\n");
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"local_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");

    {
        char *argv[] = { "install", project_dir };
        stderr_text = run_deps_capture_stderr(2, argv, &rc);
    }

    ASSERT(rc != 0);
    ASSERT(strstr(stderr_text,
                  "failed to validate local dependency local_dep declared as \"../dep\"") != NULL);
    ASSERT(strstr(stderr_text, dep_manifest_path) != NULL);
    ASSERT(strstr(stderr_text, "manifest requires `target` field") != NULL);

    free(stderr_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(dep_manifest_path);
    free(project_manifest_path);
    free(dep_dir);
    free(project_dir);
}

static void test_deps_install_hides_cache_dir_prefix_in_error_output(void) {
    char template_path[] = "/tmp/feng_cli_deps_install_cache_prefix_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *feng_path;
    char *saved_home = NULL;
    char *stderr_text;
    char *remove_error = NULL;
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");
    feng_path = path_join(workspace_dir, ".feng");

    mkdir_p(project_dir);
    write_text_file(feng_path, "not-a-directory\n");
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
        stderr_text = run_deps_capture_stderr(2, argv, &rc);
    }

    ASSERT(rc != 0);
    ASSERT(strstr(stderr_text, "failed to install remote_dep@1.0.0") != NULL);
    ASSERT(strstr(stderr_text, ".feng/cache:") == NULL);
    ASSERT(strstr(stderr_text, "/.feng/cache") == NULL);

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    free(stderr_text);
    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(feng_path);
    free(manifest_path);
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

/* Tests that hover and go-to-definition resolve correctly for local variables
 * referenced in for-loop update expressions and in the bodies of if
 * expressions.  Before the fix, find_expr_hit skipped if-expression bodies
 * and the for-loop update statement, so the LSP returned null for those
 * cursor positions. */
static void test_lsp_hover_and_definition_local_var_rhs(void) {
    static const char *kSource =
        "mod test.lsp.localrhs;\n"
        "\n"
        "fn check(n: int): int {\n"
        "    let doubled = if n > 0 { n + n; } else { 0; };\n"
        "    return doubled;\n"
        "}\n"
        "\n"
        "fn loop_sum(n: int): int {\n"
        "    var acc: int = 0;\n"
        "    for var i = 0; i < n; i += 1 {\n"
        "        acc = acc + i;\n"
        "    }\n"
        "    return acc;\n"
        "}\n";
    char template_path[] = "/tmp/feng_lsp_localrhs_XXXXXX";
    char *workspace_dir;
    char *src_path;
    char *src_uri;
    char *escaped_source;
    char *initialize;
    char *did_open;
    char *hover_n_in_if_body;
    char *hover_i_in_for_update;
    char *def_n_in_if_body;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int n_if_line;
    unsigned int n_if_char;
    unsigned int i_update_line;
    unsigned int i_update_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    src_path = path_join(workspace_dir, "main.ff");
    write_text_file(src_path, kSource);

    /* `n + n` appears only inside the if-expression then-block; char 0 = 'n'. */
    find_line_character(kSource, "n + n", 0U, &n_if_line, &n_if_char);
    /* `i += 1` is the for-loop update; char 0 = 'i'. */
    find_line_character(kSource, "i += 1", 0U, &i_update_line, &i_update_char);

    src_uri = file_uri_from_path(src_path);
    escaped_source = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          src_uri,
                          escaped_source);
    hover_n_in_if_body = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                    "\"method\":\"textDocument/hover\","
                                    "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                    "\"position\":{\"line\":%u,\"character\":%u}}}",
                                    src_uri,
                                    n_if_line,
                                    n_if_char);
    hover_i_in_for_update = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,"
                                       "\"method\":\"textDocument/hover\","
                                       "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                       "\"position\":{\"line\":%u,\"character\":%u}}}",
                                       src_uri,
                                       i_update_line,
                                       i_update_char);
    def_n_in_if_body = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,"
                                  "\"method\":\"textDocument/definition\","
                                  "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                  "\"position\":{\"line\":%u,\"character\":%u}}}",
                                  src_uri,
                                  n_if_line,
                                  n_if_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_n_in_if_body);
    write_lsp_message(input, hover_i_in_for_update);
    write_lsp_message(input, def_n_in_if_body);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    /* Hover on `n` inside the if-expression then-block must resolve to the
     * function parameter.  Before the fix, find_expr_hit skipped if-expression
     * bodies, so the result was null. */
    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output, "param n: int") != NULL);

    /* Hover on `i` in `i += 1` (for-loop update) must resolve to the for-init
     * binding.  Before the fix, find_expr_hit_in_block skipped the update
     * statement, so the result was null. */
    ASSERT(strstr(output, "\"id\":3,\"result\":null") == NULL);
    ASSERT(strstr(output, "var i") != NULL);

    /* Go-to-definition on `n` inside the if-expression body must return a
     * non-null location. */
    ASSERT(strstr(output, "\"id\":4,\"result\":null") == NULL);

    free(output);
    free(shutdown);
    free(def_n_in_if_body);
    free(hover_i_in_for_update);
    free(hover_n_in_if_body);
    free(did_open);
    free(initialize);
    free(escaped_source);
    free(src_uri);
    free(src_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Tests that typing `use foo.bar.` in a project file triggers module-path
 * segment completion, offering the next path component of known source files.
 * Before the fix, the cursor-after-dot in a `use` statement was misidentified
 * as member access on an alias, producing an empty completion list. */
static void test_lsp_use_path_completion(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"usepath_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kLibSource =
        "pu mod test.lsp.usepath.lib;\n"
        "\n"
        "pu fn value(): int {\n"
        "    return 1;\n"
        "}\n";
    /* Main source ends with an incomplete `use` path — the cursor is placed
     * right after the trailing dot to trigger path-segment completion. */
    static const char *kMainSource =
        "mod test.lsp.usepath.main;\n"
        "use test.lsp.usepath.\n";
    char template_path[] = "/tmp/feng_lsp_usepath_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *lib_path;
    char *main_path;
    char *main_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int comp_line;
    unsigned int comp_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    lib_path = path_join(src_dir, "lib.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(lib_path, kLibSource);
    write_text_file(main_path, kMainSource);

    /* Position the cursor right after the trailing dot in `use test.lsp.usepath.`
     * (line 1, character 21). */
    find_line_character(kMainSource, "use test.lsp.usepath.", 21U, &comp_line, &comp_char);

    main_uri = file_uri_from_path(main_path);
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"textDocument/completion\","
                                "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%u,\"character\":%u}}}",
                                main_uri,
                                comp_line,
                                comp_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    /* Completion must offer the next module path segment "lib" (from lib.ff).
     * Before the fix, the `use` context was not detected and the result was
     * an empty list or member-access completions for a non-existent alias. */
    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"lib\"") != NULL);

    free(output);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(main_uri);
    free(main_path);
    free(lib_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Tests that intermediate module-path segments are deduplicated when several
 * modules share the same prefix.  For `a.b.c` and `a.b.d`, completing
 * `use a.` must offer exactly one `b`. */
static void test_lsp_use_path_completion_deduplicates_segments(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"usepath_dedup_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kCSource =
        "pu mod a.b.c;\n";
    static const char *kDSource =
        "pu mod a.b.d;\n";
    static const char *kMainSource =
        "mod app.main;\n"
        "use a.\n";
    char template_path[] = "/tmp/feng_lsp_usepath_dedup_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *c_path;
    char *d_path;
    char *main_path;
    char *main_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int comp_line;
    unsigned int comp_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    c_path = path_join(src_dir, "c.ff");
    d_path = path_join(src_dir, "d.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(c_path, kCSource);
    write_text_file(d_path, kDSource);
    write_text_file(main_path, kMainSource);

    find_line_character(kMainSource, "use a.", 6U, &comp_line, &comp_char);

    main_uri = file_uri_from_path(main_path);
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"textDocument/completion\","
                                "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%u,\"character\":%u}}}",
                                main_uri,
                                comp_line,
                                comp_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"b\"") != NULL);
    ASSERT(count_substring(output, "\"label\":\"b\"") == 1U);

    free(output);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(main_uri);
    free(main_path);
    free(d_path);
    free(c_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Tests the edit-time project-scan fallback used for incomplete `use` paths.
 * Before the fix, deduplication stored slices into freed file buffers, so
 * `use foo.` could return duplicate `bar` entries when several modules shared
 * the `foo.bar.*` prefix. */
static void test_lsp_use_path_completion_deduplicates_segments_in_project_scan(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"usepath_scan_dedup_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kASource =
        "pu mod foo.bar.a;\n"
        "\n"
        "pu fn alpha(): int {\n"
        "    return 1;\n"
        "}\n";
    static const char *kBSource =
        "pu mod foo.bar.b;\n"
        "\n"
        "pu fn beta(): int {\n"
        "    return 2;\n"
        "}\n";
    static const char *kCSource =
        "pu mod foo.bar.c;\n"
        "\n"
        "pu fn gamma(): int {\n"
        "    return 3;\n"
        "}\n";
    static const char *kMainSource =
        "mod foo.bar.current;\n"
        "use foo.\n";
    char template_path[] = "/tmp/feng_lsp_usepath_scan_dedup_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *a_path;
    char *b_path;
    char *c_path;
    char *main_path;
    char *main_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int comp_line;
    unsigned int comp_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    a_path = path_join(src_dir, "a.ff");
    b_path = path_join(src_dir, "b.ff");
    c_path = path_join(src_dir, "c.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(a_path, kASource);
    write_text_file(b_path, kBSource);
    write_text_file(c_path, kCSource);
    write_text_file(main_path, kMainSource);

    find_line_character(kMainSource, "use foo.", 8U, &comp_line, &comp_char);

    main_uri = file_uri_from_path(main_path);
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"textDocument/completion\","
                                "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%u,\"character\":%u}}}",
                                main_uri,
                                comp_line,
                                comp_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"bar\"") != NULL);
    ASSERT(count_substring(output, "\"label\":\"bar\"") == 1U);

    free(output);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(main_uri);
    free(main_path);
    free(c_path);
    free(b_path);
    free(a_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Tests that after a `use` statement imports a module, the exported types and
 * functions from that module appear as completion candidates in identifier
 * position inside the importing file's function bodies.  This exercises the
 * analysis-path branch that appends loaded-module completion items for each
 * `use` declaration in the program. */
static void test_lsp_imported_type_completion_after_use(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"impcomp_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kTypesSource =
        "pu mod test.lsp.imptypes;\n"
        "\n"
        "pu type Widget {\n"
        "    let id: int;\n"
        "}\n"
        "\n"
        "pu fn make_widget(): Widget {\n"
        "    return Widget { id: 0 };\n"
        "}\n";
    static const char *kMainSource =
        "mod test.lsp.impcomp.main;\n"
        "use test.lsp.imptypes;\n"
        "\n"
        "fn run(): int {\n"
        "    return 0;\n"
        "}\n";
    char template_path[] = "/tmp/feng_lsp_impcomp_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *types_path;
    char *main_path;
    char *main_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int comp_line;
    unsigned int comp_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    types_path = path_join(src_dir, "types.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(types_path, kTypesSource);
    write_text_file(main_path, kMainSource);

    /* Trigger completion at the beginning of `    return 0;` (inside run()'s
     * body), so the completion list includes all identifiers in scope. */
    find_line_character(kMainSource, "    return 0", 4U, &comp_line, &comp_char);

    main_uri = file_uri_from_path(main_path);
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"textDocument/completion\","
                                "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%u,\"character\":%u}}}",
                                main_uri,
                                comp_line,
                                comp_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    /* The imported type `Widget` and function `make_widget` from the `use`d
     * module must appear as completion candidates in run()'s body. */
    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"Widget\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"make_widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"Widget\",\"kind\":6,\"detail\":\"type Widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"make_widget\",\"kind\":3,\"detail\":\"fn make_widget(): Widget\"") != NULL);

    free(output);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(main_uri);
    free(main_path);
    free(types_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Tests that imported public declarations remain available to completion even
 * when project-wide analysis fails for reasons unrelated to the current file.
 * Here the package target is `bin` but the project lacks a `main` entry; the
 * current document still parses and should still surface names from
 * `use other.lib;`. */
static void test_lsp_imported_type_completion_survives_project_semantic_failure(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"impcomp_degraded_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kTypesSource =
        "pu mod other.lib;\n"
        "\n"
        "pu type Widget {\n"
        "    let id: int;\n"
        "}\n"
        "\n"
        "pu fn make_widget(): Widget {\n"
        "    return Widget { id: 0 };\n"
        "}\n";
    static const char *kMainSource =
        "mod app.main;\n"
        "use other.lib;\n"
        "\n"
        "fn helper(): int {\n"
        "    return 0;\n"
        "}\n";
    char template_path[] = "/tmp/feng_lsp_impcomp_degraded_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *types_path;
    char *main_path;
    char *main_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int comp_line;
    unsigned int comp_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    types_path = path_join(src_dir, "types.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(types_path, kTypesSource);
    write_text_file(main_path, kMainSource);

    find_line_character(kMainSource, "    return 0", 4U, &comp_line, &comp_char);

    main_uri = file_uri_from_path(main_path);
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"textDocument/completion\","
                                "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%u,\"character\":%u}}}",
                                main_uri,
                                comp_line,
                                comp_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"helper\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"Widget\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"make_widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"Widget\",\"kind\":6,\"detail\":\"type Widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"make_widget\",\"kind\":3,\"detail\":\"fn make_widget(): Widget\"") != NULL);

    free(output);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(main_uri);
    free(main_path);
    free(types_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Tests that `use foo.bar as baz;` still offers public declarations from the
 * imported module when the user is in the middle of typing `baz.`. Before the
 * fix, alias-module completion fell back to a single-file parse session and
 * lost access to sibling project sources, so the completion list was empty. */
static void test_lsp_alias_module_completion_survives_incomplete_member_access(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"alias_completion_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kLoopSource =
        "pu mod test.lsp.alias.loop;\n"
        "\n"
        "pu fn loop_example(): int {\n"
        "    return 1;\n"
        "}\n";
    static const char *kMainSource =
        "mod test.lsp.alias.main;\n"
        "use test.lsp.alias.loop as lp;\n"
        "\n"
        "fn run(): int {\n"
        "    lp.\n"
        "    return 0;\n"
        "}\n";
    char template_path[] = "/tmp/feng_lsp_alias_completion_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *loop_path;
    char *main_path;
    char *main_uri;
    char *escaped_main;
    char *initialize;
    char *did_open;
    char *completion_req;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int comp_line;
    unsigned int comp_char;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    loop_path = path_join(src_dir, "loop.ff");
    main_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(loop_path, kLoopSource);
    write_text_file(main_path, kMainSource);

    find_line_character(kMainSource, "    lp.", 7U, &comp_line, &comp_char);

    main_uri = file_uri_from_path(main_path);
    escaped_main = json_escape_text(kMainSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          main_uri,
                          escaped_main);
    completion_req = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"textDocument/completion\","
                                "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                "\"position\":{\"line\":%u,\"character\":%u}}}",
                                main_uri,
                                comp_line,
                                comp_char);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = tmpfile();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, completion_req);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"loop_example\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"loop_example\",\"kind\":3,\"detail\":\"fn loop_example(): int\"") != NULL);

    free(output);
    free(shutdown);
    free(completion_req);
    free(did_open);
    free(initialize);
    free(escaped_main);
    free(main_uri);
    free(main_path);
    free(loop_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_lsp_external_package_hover_docs_and_completion(void) {
    static const char *kPackageSource =
        "pu mod test.lsp.pkg.collections;\n"
        "\n"
        "/**\n"
        " * Package map docs.\n"
        " */\n"
        "pu type Map<K, V> {\n"
        "    /**\n"
        "     * Number of stored entries.\n"
        "     */\n"
        "    pu let count: int;\n"
        "}\n";
    static const char *kHoverSource =
        "mod test.lsp.pkgconsumer.main;\n"
        "use test.lsp.pkg.collections;\n"
        "\n"
        "fn consume(map: Map<string, int>): int {\n"
        "    return map.count;\n"
        "}\n";
    static const char *kUsePathSource =
        "mod test.lsp.pkgconsumer.useedit;\n"
        "use test.lsp.pkg.\n"
        "\n"
        "fn run(): void {}\n";
    static const char *kTypeCompletionSource =
        "mod test.lsp.pkgconsumer.typeedit;\n"
        "use test.lsp.pkg.collections;\n"
        "\n"
        "fn run(): void {\n"
        "    let value: Ma\n"
        "}\n";
    static const char *kCtorCompletionSource =
        "mod test.lsp.pkgconsumer.ctoredit;\n"
        "use test.lsp.pkg.collections;\n"
        "\n"
        "fn run(): void {\n"
        "    let value = M\n"
        "}\n";
    static const char *kBareCompletionSource =
        "mod test.lsp.pkgconsumer.bareedit;\n"
        "use test.lsp.pkg.collections;\n"
        "\n"
        "fn run(): void {\n"
        "    M\n"
        "}\n";
    static const char *kMemberCompletionSource =
        "mod test.lsp.pkgconsumer.memberedit;\n"
        "use test.lsp.pkg.collections;\n"
        "\n"
        "fn consume(map: Map<string, int>): int {\n"
        "    return map.;\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char template_path[] = "/tmp/feng_lsp_external_pkg_XXXXXX";
    char *workspace_dir;
    char *pkg_project_dir;
    char *pkg_manifest_path;
    char *pkg_src_dir;
    char *pkg_source_path;
    char *bundle_path;
    char *consumer_project_dir;
    char *consumer_manifest_path;
    char *consumer_src_dir;
    char *main_path;
    char *hover_type_output;
    char *hover_member_output;
    char *use_completion_output;
    char *type_completion_output;
    char *ctor_completion_output;
    char *bare_completion_output;
    char *local_dep_ctor_completion_output;
    char *local_dep_bare_completion_output;
    char *member_completion_output;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    pkg_project_dir = path_join(workspace_dir, "pkgdocs");
    pkg_manifest_path = path_join(pkg_project_dir, "feng.fm");
    pkg_src_dir = path_join(pkg_project_dir, "src");
    pkg_source_path = path_join(pkg_src_dir, "collections.ff");
    bundle_path = path_join(pkg_project_dir, "build/lsp_pkgdocs-0.1.0.fb");
    consumer_project_dir = path_join(workspace_dir, "consumer");
    consumer_manifest_path = path_join(consumer_project_dir, "feng.fm");
    consumer_src_dir = path_join(consumer_project_dir, "src");
    main_path = path_join(consumer_src_dir, "main.ff");

    mkdir_p(pkg_src_dir);
    mkdir_p(consumer_src_dir);
    write_text_file(pkg_manifest_path,
                    "[package]\n"
                    "name: \"lsp_pkgdocs\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(pkg_source_path, kPackageSource);
    {
        char *argv[] = { pkg_project_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(bundle_path));

    write_text_file(consumer_manifest_path,
                    "[package]\n"
                    "name: \"lsp_pkgconsumer\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "lsp_pkgdocs: \"../pkgdocs/build/lsp_pkgdocs-0.1.0.fb\"\n");
    write_text_file(main_path, kHoverSource);

    hover_type_output = capture_lsp_position_response_at_path(main_path,
                                                              kHoverSource,
                                                              kInitialize,
                                                              "textDocument/hover",
                                                              "fn consume(map: Map<string, int>): int {",
                                                              strlen("fn consume(map: "));
    hover_member_output = capture_lsp_position_response_at_path(main_path,
                                                                kHoverSource,
                                                                kInitialize,
                                                                "textDocument/hover",
                                                                "    return map.count;",
                                                                strlen("    return map."));
    use_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                  kUsePathSource,
                                                                  kInitialize,
                                                                  "textDocument/completion",
                                                                  "use test.lsp.pkg.",
                                                                  strlen("use test.lsp.pkg."));
    type_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                   kTypeCompletionSource,
                                                                   kInitialize,
                                                                   "textDocument/completion",
                                                                   "    let value: Ma",
                                                                   strlen("    let value: Ma"));
    ctor_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                   kCtorCompletionSource,
                                                                   kInitialize,
                                                                   "textDocument/completion",
                                                                   "    let value = M",
                                                                   strlen("    let value = M"));
    bare_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                   kBareCompletionSource,
                                                                   kInitialize,
                                                                   "textDocument/completion",
                                                                   "    M",
                                                                   strlen("    M"));
    member_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                     kMemberCompletionSource,
                                                                     kInitialize,
                                                                     "textDocument/completion",
                                                                     "    return map.;",
                                                                     strlen("    return map."));
    write_text_file(consumer_manifest_path,
                    "[package]\n"
                    "name: \"lsp_pkgconsumer\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "lsp_pkgdocs: \"../pkgdocs\"\n");
    local_dep_ctor_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                             kCtorCompletionSource,
                                                                             kInitialize,
                                                                             "textDocument/completion",
                                                                             "    let value = M",
                                                                             strlen("    let value = M"));
    local_dep_bare_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                             kBareCompletionSource,
                                                                             kInitialize,
                                                                             "textDocument/completion",
                                                                             "    M",
                                                                             strlen("    M"));

    ASSERT(strstr(hover_type_output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(hover_type_output, "type Map<K, V>") != NULL);
    ASSERT(strstr(hover_type_output, "Package map docs.") != NULL);
    ASSERT(strstr(hover_member_output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(hover_member_output, "let count: i32") != NULL);
    ASSERT(strstr(hover_member_output, "Number of stored entries.") != NULL);
    ASSERT(strstr(use_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(use_completion_output, "\"label\":\"collections\"") != NULL);
    ASSERT(strstr(type_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(type_completion_output, "\"label\":\"Map\"") != NULL);
    ASSERT(strstr(type_completion_output, "\"label\":\"Map\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(ctor_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(ctor_completion_output, "\"label\":\"Map\"") != NULL);
    ASSERT(strstr(ctor_completion_output, "\"label\":\"Map\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(bare_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(bare_completion_output, "\"label\":\"Map\"") != NULL);
    ASSERT(strstr(bare_completion_output, "\"label\":\"Map\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(local_dep_ctor_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(local_dep_ctor_completion_output, "\"label\":\"Map\"") != NULL);
    ASSERT(strstr(local_dep_ctor_completion_output, "\"label\":\"Map\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(local_dep_bare_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(local_dep_bare_completion_output, "\"label\":\"Map\"") != NULL);
    ASSERT(strstr(local_dep_bare_completion_output, "\"label\":\"Map\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(member_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(member_completion_output, "\"label\":\"count\"") != NULL);
    ASSERT(strstr(member_completion_output, "\"label\":\"K\"") == NULL);
    ASSERT(strstr(member_completion_output, "\"label\":\"V\"") == NULL);

    free(member_completion_output);
    free(local_dep_bare_completion_output);
    free(local_dep_ctor_completion_output);
    free(bare_completion_output);
    free(ctor_completion_output);
    free(type_completion_output);
    free(use_completion_output);
    free(hover_member_output);
    free(hover_type_output);
    free(main_path);
    free(consumer_src_dir);
    free(consumer_manifest_path);
    free(consumer_project_dir);
    free(bundle_path);
    free(pkg_source_path);
    free(pkg_src_dir);
    free(pkg_manifest_path);
    free(pkg_project_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
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
    test_deps_add_local_bundle_error_reports_dependency_context();
    test_deps_add_local_rejects_name_mismatch_before_write();
    test_deps_add_local_rejects_non_lib_target_before_write();
    test_deps_remove_updates_manifest();
    test_deps_install_populates_cache_from_registry();
    test_deps_install_local_dependency_error_reports_dependency_context();
    test_deps_install_reports_download_failure_with_reason();
    test_deps_install_rejects_invalid_downloaded_bundle();
    test_deps_install_hides_cache_dir_prefix_in_error_output();
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
    test_lsp_hover_uses_markdown_when_supported();
    test_lsp_hover_falls_back_to_plaintext_without_markdown_capability();
    test_lsp_member_completion_survives_incomplete_member_access();
        test_lsp_enum_member_completion_survives_incomplete_member_access();
    test_lsp_completion_uses_source_scoped_edit_context();
    test_lsp_member_completion_infers_constructor_call_overloads();
    test_lsp_member_references_and_rename_from_object_literal_field();
    test_lsp_function_decl_site_definition_references_and_rename();
    test_lsp_rename_accepts_identifier_end_position();
    test_lsp_definition_references_rename_with_broken_code();
    test_lsp_no_crash_on_library_file_without_main();
    test_lsp_didopen_handles_unicode_escape_in_source();
    test_lsp_project_cache_hit_survives_broken_dependency_source();
    test_lsp_hover_and_definition_local_var_rhs();
    test_lsp_use_path_completion();
    test_lsp_use_path_completion_deduplicates_segments();
    test_lsp_use_path_completion_deduplicates_segments_in_project_scan();
    test_lsp_imported_type_completion_after_use();
    test_lsp_imported_type_completion_survives_project_semantic_failure();
    test_lsp_alias_module_completion_survives_incomplete_member_access();
    test_lsp_external_package_hover_docs_and_completion();
    test_direct_build_cleans_stale_ir_on_frontend_failure();
    test_direct_build_emits_symbol_tables();
    test_direct_build_accepts_package_bundle();
    test_direct_build_links_library_from_package_bundle();
    test_direct_build_sorts_package_libraries_by_dependency();
    test_project_pack_bundle_can_be_consumed();
    test_direct_build_consumes_package_generic_function();
    test_direct_build_consumes_package_generic_type();
    test_direct_build_consumes_package_enum();
    test_direct_build_consumes_package_generic_spec_constraint();
    test_direct_build_consumes_package_constrained_generic_function();
    test_direct_build_consumes_package_constrained_generic_type();
    test_pack_bundle_manifest_rewrites_local_dependency_versions();
    test_project_check_accepts_source_file_path_and_local_dependencies();
    test_project_check_reports_enum_semantic_error_without_unknown_type();
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
