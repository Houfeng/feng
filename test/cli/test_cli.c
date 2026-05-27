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
#include "debug/debug.h"
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

void feng_cli_print_usage(const char *program, FILE *stream) {
    (void)program;
    (void)stream;
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

static void write_binary_file(const char *path,
                              const unsigned char *bytes,
                              size_t length) {
    FILE *file;

    file = fopen(path, "wb");
    ASSERT(file != NULL);
    ASSERT(fwrite(bytes, 1U, length, file) == length);
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

static char *make_lib_option(const char *library_name_or_path) {
    size_t len = strlen(library_name_or_path);
    char *out = (char *)malloc(len + 7U);

    ASSERT(out != NULL);
    memcpy(out, "--lib=", 6U);
    memcpy(out + 6U, library_name_or_path, len + 1U);
    return out;
}

static char *host_static_library_file_name(const char *stem) {
    char *name = feng_fb_host_static_library_file_name(stem);

    ASSERT(name != NULL);
    return name;
}

static char *host_static_library_path(const char *dir, const char *stem) {
    char *name = host_static_library_file_name(stem);
    char *path = path_join(dir, name);

    free(name);
    return path;
}

static char *host_static_library_output_path(const char *out_dir, const char *stem) {
    char *host_target = NULL;
    char *lib_base;
    char *lib_dir;
    char *path;

    ASSERT(feng_fb_detect_host_target(&host_target, NULL));
    lib_base = path_join(out_dir, "lib");
    lib_dir = path_join(lib_base, host_target);
    free(lib_base);
    free(host_target);
    path = host_static_library_path(lib_dir, stem);
    free(lib_dir);
    return path;
}

static char *host_dynamic_library_file_name(const char *stem) {
#if defined(_WIN32)
    return dup_printf("%s.dll", stem);
#elif defined(__APPLE__)
    return dup_printf("lib%s.dylib", stem);
#else
    return dup_printf("lib%s.so", stem);
#endif
}

static char *host_dynamic_library_path(const char *dir, const char *stem) {
    char *name = host_dynamic_library_file_name(stem);
    char *path = path_join(dir, name);

    free(name);
    return path;
}

static char *host_bundle_static_library_entry_path(const char *host_target,
                                                   const char *stem) {
    char *name = host_static_library_file_name(stem);
    char *entry = dup_printf("lib/%s/%s", host_target, name);

    free(name);
    return entry;
}

static char *host_bundle_extlib_static_entry_path(const char *host_target,
                                                  const char *stem) {
    char *name = host_static_library_file_name(stem);
    char *entry = dup_printf("extlib/%s/%s", host_target, name);

    free(name);
    return entry;
}

static char *host_bundle_extlib_dynamic_entry_path(const char *host_target,
                                                   const char *stem) {
    char *name = host_dynamic_library_file_name(stem);
    char *entry = dup_printf("extlib/%s/%s", host_target, name);

    free(name);
    return entry;
}

static void write_bundle_with_file_or_die(const char *bundle_path,
                                          const char *entry_path,
                                          const char *source_path) {
    FengZipWriter writer = {0};
    char *error_message = NULL;

    ASSERT(feng_zip_writer_open(bundle_path, &writer, &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_add_file(&writer,
                                    entry_path,
                                    source_path,
                                    FENG_ZIP_COMPRESSION_STORE,
                                    &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_finalize(&writer, &error_message));
    free(error_message);
    feng_zip_writer_dispose(&writer);
}

static void write_bundle_with_bytes_or_die(const char *bundle_path,
                                           const char *entry_path,
                                           const void *bytes,
                                           size_t size) {
    FengZipWriter writer = {0};
    char *error_message = NULL;

    ASSERT(feng_zip_writer_open(bundle_path, &writer, &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_add_bytes(&writer,
                                     entry_path,
                                     bytes,
                                     size,
                                     FENG_ZIP_COMPRESSION_STORE,
                                     &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_finalize(&writer, &error_message));
    free(error_message);
    feng_zip_writer_dispose(&writer);
}

static void write_manifest_only_bundle_or_die(const char *bundle_path,
                                              const char *manifest_text) {
    FengZipWriter writer = {0};
    char *error_message = NULL;

    ASSERT(feng_zip_writer_open(bundle_path, &writer, &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_add_bytes(&writer,
                                     "feng.fm",
                                     manifest_text,
                                     strlen(manifest_text),
                                     FENG_ZIP_COMPRESSION_DEFLATE,
                                     &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_finalize(&writer, &error_message));
    free(error_message);
    feng_zip_writer_dispose(&writer);
}

static void write_library_bundle_or_die(const char *bundle_path,
                                        const char *package_name,
                                        const char *package_version,
                                        const char *library_path,
                                        const char *public_mod_root) {
    FengFbLibraryBundleSpec spec = {0};
    char *error_message = NULL;

    spec.package_path = bundle_path;
    spec.package_name = package_name;
    spec.package_version = package_version;
    spec.library_path = library_path;
    spec.public_mod_root = public_mod_root;

    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
}

static void assert_zip_ok(bool ok, char **zip_error) {
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

static int zip_contains_path_prefix(const FengZipReader *reader,
                                    const char *prefix) {
    size_t entry_count = feng_zip_reader_entry_count(reader);
    size_t prefix_length = strlen(prefix);
    size_t index;
    FengZipEntryInfo info;
    char *error_message = NULL;

    for (index = 0U; index < entry_count; ++index) {
        ASSERT(feng_zip_reader_entry_at(reader, index, &info, &error_message));
        free(error_message);
        error_message = NULL;
        if (strncmp(info.path, prefix, prefix_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static void run_command_or_die(const char *command) {
    int status = system(command);

    ASSERT(status >= 0);
    ASSERT(WIFEXITED(status));
    ASSERT(WEXITSTATUS(status) == 0);
}

static int find_first_dwarfdump_address_for_line(const char *dump_text,
                                                 const char *file_name,
                                                 unsigned target_line,
                                                 unsigned long long *out_address) {
    const char *section;
    const char *cursor;
    unsigned long long best = 0ULL;
    int found = 0;

    ASSERT(dump_text != NULL);
    ASSERT(file_name != NULL);
    ASSERT(out_address != NULL);

    section = strstr(dump_text, file_name);
    if (section == NULL) {
        return 0;
    }
    cursor = strstr(section, "Address");
    if (cursor == NULL) {
        return 0;
    }
    cursor = strchr(cursor, '\n');
    while (cursor != NULL) {
        unsigned long long address = 0ULL;
        unsigned line = 0U;
        unsigned column = 0U;
        unsigned file_index = 0U;

        cursor += 1;
        if (cursor[0] != '0' || cursor[1] != 'x') {
            break;
        }
        if (sscanf(cursor, "0x%llx %u %u %u", &address, &line, &column, &file_index) == 4 &&
            line == target_line) {
            if (!found || address < best) {
                best = address;
                found = 1;
            }
        }
        cursor = strchr(cursor, '\n');
    }

    if (found) {
        *out_address = best;
    }
    return found;
}

static void build_native_static_library_or_die(const char *source_path,
                                               const char *library_path) {
    const char *cc = getenv("CC");
    const char *ar = getenv("AR");
    char *object_path = dup_printf("%s.o", source_path);
    char *compile_command;
    char *archive_command;

    ASSERT(object_path != NULL);
    if (cc == NULL || cc[0] == '\0') {
        cc = "cc";
    }
    if (ar == NULL || ar[0] == '\0') {
        ar = "ar";
    }

    compile_command = dup_printf("%s -c \"%s\" -o \"%s\"",
                                 cc,
                                 source_path,
                                 object_path);
    archive_command = dup_printf("%s rcs \"%s\" \"%s\"",
                                 ar,
                                 library_path,
                                 object_path);
    ASSERT(compile_command != NULL);
    ASSERT(archive_command != NULL);

    run_command_or_die(compile_command);
    run_command_or_die(archive_command);
    ASSERT(unlink(object_path) == 0);

    free(archive_command);
    free(compile_command);
    free(object_path);
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
    dep_library_path = host_static_library_output_path(dep_out_dir, package_name);
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

/* Run `feng dap` while suppressing stderr for option-level tests. */
static int run_dap_quiet_stderr(int argc, char **argv) {
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

    rc = feng_cli_dap_main("feng", argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    return rc;
}

typedef int (*CliEntryFn)(const char *program, int argc, char **argv);

static char *read_text_stream(FILE *file);

static char *run_cli_entry_capture_stdout_and_stderr(CliEntryFn entry,
                                                     int argc,
                                                     char **argv,
                                                     int *out_rc,
                                                     char **out_stderr) {
    int saved_stdout;
    int saved_stderr;
    FILE *output = tmpfile();
    FILE *errors = tmpfile();
    int rc;
    char *captured_stdout;
    char *captured_stderr;

    ASSERT(output != NULL);
    ASSERT(errors != NULL);

    fflush(stdout);
    fflush(stderr);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stdout >= 0);
    ASSERT(saved_stderr >= 0);
    ASSERT(dup2(fileno(output), STDOUT_FILENO) >= 0);
    ASSERT(dup2(fileno(errors), STDERR_FILENO) >= 0);

    rc = entry("feng", argc, argv);

    fflush(stdout);
    fflush(stderr);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdout);
    close(saved_stderr);

    captured_stdout = read_text_stream(output);
    captured_stderr = read_text_stream(errors);
    fclose(output);
    fclose(errors);

    if (out_rc != NULL) {
        *out_rc = rc;
    }
    if (out_stderr != NULL) {
        *out_stderr = captured_stderr;
    } else {
        free(captured_stderr);
    }

    return captured_stdout;
}

/* Read an entire file descriptor into a newly allocated string. */
static char *read_fd_to_string(int fd) {
    size_t capacity = 256U;
    size_t length = 0U;
    char *content = (char *)malloc(capacity);

    ASSERT(content != NULL);
    for (;;) {
        ssize_t read_size;

        if (length + 1U >= capacity) {
            char *resized;

            capacity *= 2U;
            resized = (char *)realloc(content, capacity);
            ASSERT(resized != NULL);
            content = resized;
        }

        read_size = read(fd, content + length, capacity - length - 1U);
        if (read_size == 0) {
            break;
        }
        ASSERT(read_size >= 0);
        length += (size_t)read_size;
    }

    content[length] = '\0';
    return content;
}

static char *concat_owned_strings(char *lhs, char *rhs) {
    size_t lhs_len = lhs != NULL ? strlen(lhs) : 0U;
    size_t rhs_len = rhs != NULL ? strlen(rhs) : 0U;
    char *joined = (char *)malloc(lhs_len + rhs_len + 1U);

    ASSERT(joined != NULL);
    if (lhs_len > 0U) {
        memcpy(joined, lhs, lhs_len);
    }
    if (rhs_len > 0U) {
        memcpy(joined + lhs_len, rhs, rhs_len);
    }
    joined[lhs_len + rhs_len] = '\0';
    free(lhs);
    free(rhs);
    return joined;
}

static char *read_fd_until_contains(int fd, const char *needle) {
    size_t capacity = 256U;
    size_t length = 0U;
    char *content = (char *)malloc(capacity);

    ASSERT(content != NULL);
    content[0] = '\0';
    while (strstr(content, needle) == NULL) {
        ssize_t read_size;

        if (length + 64U >= capacity) {
            char *resized;

            capacity *= 2U;
            resized = (char *)realloc(content, capacity);
            ASSERT(resized != NULL);
            content = resized;
        }
        read_size = read(fd, content + length, capacity - length - 1U);
        ASSERT(read_size > 0);
        length += (size_t)read_size;
        content[length] = '\0';
    }
    return content;
}

/* Run `feng dap` with redirected stdio and a temporary PATH override. */
static char *run_dap_capture_stdout_with_path(int argc,
                                              char **argv,
                                              const char *input_text,
                                              const char *path_value,
                                              int *out_rc,
                                              char **out_stderr) {
    int input_pipe[2];
    int output_pipe[2];
    int saved_stdin;
    int saved_stdout;
    int saved_stderr;
    FILE *errors = tmpfile();
    const char *existing_path = getenv("PATH");
    char *saved_path = existing_path != NULL ? dup_cstr(existing_path) : NULL;
    char *captured_stdout;
    char *captured_stderr;
    size_t input_length = input_text != NULL ? strlen(input_text) : 0U;
    int rc;

    ASSERT(errors != NULL);
    ASSERT(pipe(input_pipe) == 0);
    ASSERT(pipe(output_pipe) == 0);
    if (path_value != NULL) {
        ASSERT(setenv("PATH", path_value, 1) == 0);
    } else {
        ASSERT(unsetenv("PATH") == 0);
    }
    if (input_length > 0U) {
        ASSERT(write(input_pipe[1], input_text, input_length) == (ssize_t)input_length);
    }
    close(input_pipe[1]);

    fflush(stdout);
    fflush(stderr);
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stdin >= 0);
    ASSERT(saved_stdout >= 0);
    ASSERT(saved_stderr >= 0);
    ASSERT(dup2(input_pipe[0], STDIN_FILENO) >= 0);
    ASSERT(dup2(output_pipe[1], STDOUT_FILENO) >= 0);
    ASSERT(dup2(fileno(errors), STDERR_FILENO) >= 0);

    rc = feng_cli_dap_main("feng", argc, argv);

    fflush(stdout);
    fflush(stderr);
    ASSERT(dup2(saved_stdin, STDIN_FILENO) >= 0);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
    close(input_pipe[0]);
    close(output_pipe[1]);

    captured_stdout = read_fd_to_string(output_pipe[0]);
    close(output_pipe[0]);
    captured_stderr = read_text_stream(errors);
    fclose(errors);

    if (saved_path != NULL) {
        ASSERT(setenv("PATH", saved_path, 1) == 0);
    } else {
        ASSERT(unsetenv("PATH") == 0);
    }
    free(saved_path);

    if (out_rc != NULL) {
        *out_rc = rc;
    }
    if (out_stderr != NULL) {
        *out_stderr = captured_stderr;
    } else {
        free(captured_stderr);
    }
    return captured_stdout;
}

static char *run_dap_interactive_capture_stdout_with_path(int argc,
                                                          char **argv,
                                                          const char *initial_input,
                                                          const char *wait_for_text,
                                                          const char *followup_input,
                                                          const char *path_value,
                                                          int *out_rc,
                                                          char **out_stderr) {
    int input_pipe[2];
    int output_pipe[2];
    int error_pipe[2];
    const char *existing_path = getenv("PATH");
    char *saved_path = existing_path != NULL ? dup_cstr(existing_path) : NULL;
    size_t initial_length = initial_input != NULL ? strlen(initial_input) : 0U;
    size_t followup_length = followup_input != NULL ? strlen(followup_input) : 0U;
    char *captured_prefix;
    char *captured_suffix;
    char *captured_stdout;
    char *captured_stderr;
    pid_t child;
    int status;

    ASSERT(pipe(input_pipe) == 0);
    ASSERT(pipe(output_pipe) == 0);
    ASSERT(pipe(error_pipe) == 0);
    if (path_value != NULL) {
        ASSERT(setenv("PATH", path_value, 1) == 0);
    } else {
        ASSERT(unsetenv("PATH") == 0);
    }

    child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        int rc;

        ASSERT(dup2(input_pipe[0], STDIN_FILENO) >= 0);
        ASSERT(dup2(output_pipe[1], STDOUT_FILENO) >= 0);
        ASSERT(dup2(error_pipe[1], STDERR_FILENO) >= 0);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        rc = feng_cli_dap_main("feng", argc, argv);
        _exit(rc);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    close(error_pipe[1]);
    if (initial_length > 0U) {
        ASSERT(write(input_pipe[1], initial_input, initial_length) == (ssize_t)initial_length);
    }
    captured_prefix = wait_for_text != NULL
                          ? read_fd_until_contains(output_pipe[0], wait_for_text)
                          : dup_cstr("");
    if (followup_length > 0U) {
        ASSERT(write(input_pipe[1], followup_input, followup_length) == (ssize_t)followup_length);
    }
    close(input_pipe[1]);

    captured_suffix = read_fd_to_string(output_pipe[0]);
    close(output_pipe[0]);
    captured_stdout = concat_owned_strings(captured_prefix, captured_suffix);
    captured_stderr = read_fd_to_string(error_pipe[0]);
    close(error_pipe[0]);

    ASSERT(waitpid(child, &status, 0) == child);
    ASSERT(WIFEXITED(status));
    if (saved_path != NULL) {
        ASSERT(setenv("PATH", saved_path, 1) == 0);
    } else {
        ASSERT(unsetenv("PATH") == 0);
    }
    free(saved_path);

    if (out_rc != NULL) {
        *out_rc = WEXITSTATUS(status);
    }
    if (out_stderr != NULL) {
        *out_stderr = captured_stderr;
    } else {
        free(captured_stderr);
    }
    return captured_stdout;
}

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

static char *build_dap_message_text(const char *json) {
    return dup_printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
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
    dep_library_path = host_static_library_output_path(dep_out_dir, "dep");
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
    dep_library_path = host_static_library_output_path(dep_out_dir, "pkgdep");
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
    b_library_path = host_static_library_output_path(b_out_dir, "pkgb");
    a_library_path = host_static_library_output_path(a_out_dir, "pkga");
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

static void test_bundle_writer_includes_extlib_and_assets_without_empty_dirs(void) {
    char template_path[] = "/tmp/feng_fb_bundle_assets_extlib_XXXXXX";
    char *workspace_dir;
    char *library_dir;
    char *library_path;
    char *mod_dir;
    char *mod_nested_dir;
    char *mod_path;
    char *extlib_root;
    char *extlib_platform_dir;
    char *extlib_static_path;
    char *extlib_dynamic_path;
    char *asset_root;
    char *asset_nested_dir;
    char *asset_config_path;
    char *asset_nested_path;
    char *empty_asset_root;
    char *bundle_path;
    char *host_target = NULL;
    char *error_message = NULL;
    char *remove_error = NULL;
    FengFbBundleDirectoryEntry asset_entries[2] = {0};
    FengFbLibraryBundleSpec spec = {0};
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *bytes = NULL;
    size_t byte_count = 0U;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    library_dir = path_join(workspace_dir, "artifacts/lib");
    library_path = host_static_library_path(library_dir, "bundle_demo");
    mod_dir = path_join(workspace_dir, "public_mod");
    mod_nested_dir = path_join(mod_dir, "test/cli");
    mod_path = path_join(mod_nested_dir, "bundle_demo.ft");
    extlib_root = path_join(workspace_dir, "extlib");
    ASSERT(feng_fb_detect_host_target(&host_target, &error_message));
    free(error_message);
    error_message = NULL;
    extlib_platform_dir = path_join(extlib_root, host_target);
    extlib_static_path = host_static_library_path(extlib_platform_dir, "helper");
    extlib_dynamic_path = host_dynamic_library_path(extlib_platform_dir, "helper");
    asset_root = path_join(workspace_dir, "asset_runtime");
    asset_nested_dir = path_join(asset_root, "nested");
    asset_config_path = path_join(asset_root, "config.json");
    asset_nested_path = path_join(asset_nested_dir, "value.txt");
    empty_asset_root = path_join(workspace_dir, "empty_asset");
    bundle_path = path_join(workspace_dir, "bundle_demo-0.1.0.fb");

    mkdir_p(library_dir);
    mkdir_p(mod_nested_dir);
    mkdir_p(extlib_platform_dir);
    mkdir_p(asset_nested_dir);
    mkdir_p(empty_asset_root);
    write_text_file(library_path, "FAKE-ARCHIVE\n");
    write_text_file(mod_path, "FT-PAYLOAD\n");
    write_text_file(extlib_static_path, "STATIC\n");
    write_text_file(extlib_dynamic_path, "DYNAMIC\n");
    write_text_file(asset_config_path, "{\"env\":\"prod\"}\n");
    write_text_file(asset_nested_path, "nested\n");

    asset_entries[0].entry_path = "runtime";
    asset_entries[0].source_root = asset_root;
    asset_entries[1].entry_path = "empty-assets";
    asset_entries[1].source_root = empty_asset_root;

    spec.package_path = bundle_path;
    spec.package_name = "bundle_demo";
    spec.package_version = "0.1.0";
    spec.library_path = library_path;
    spec.public_mod_root = mod_dir;
    spec.extlib_root = extlib_root;
    spec.asset_entries = asset_entries;
    spec.asset_entry_count = 2U;

    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
    error_message = NULL;

    ASSERT(feng_zip_reader_open(bundle_path, &reader, &zip_error));
    ASSERT(feng_zip_reader_read(&reader,
                                "mod/test/cli/bundle_demo.ft",
                                &bytes,
                                &byte_count,
                                &zip_error));
    ASSERT(byte_count == strlen("FT-PAYLOAD\n"));
    ASSERT(memcmp(bytes, "FT-PAYLOAD\n", byte_count) == 0);
    feng_zip_free(bytes);
    bytes = NULL;

    ASSERT(feng_zip_reader_read(&reader,
                                "runtime/config.json",
                                &bytes,
                                &byte_count,
                                &zip_error));
    ASSERT(memcmp(bytes, "{\"env\":\"prod\"}\n", byte_count) == 0);
    feng_zip_free(bytes);
    bytes = NULL;

    ASSERT(feng_zip_reader_read(&reader,
                                "runtime/nested/value.txt",
                                &bytes,
                                &byte_count,
                                &zip_error));
    ASSERT(memcmp(bytes, "nested\n", byte_count) == 0);
    feng_zip_free(bytes);
    bytes = NULL;

    {
        char *entry_path = host_bundle_static_library_entry_path(host_target, "bundle_demo");
        ASSERT(entry_path != NULL);
        ASSERT(feng_zip_reader_read(&reader,
                                    entry_path,
                                    &bytes,
                                    &byte_count,
                                    &zip_error));
        ASSERT(memcmp(bytes, "FAKE-ARCHIVE\n", byte_count) == 0);
        free(entry_path);
        feng_zip_free(bytes);
        bytes = NULL;
    }

    {
        char *entry_path = host_bundle_extlib_static_entry_path(host_target, "helper");
        ASSERT(entry_path != NULL);
        ASSERT(feng_zip_reader_read(&reader,
                                    entry_path,
                                    &bytes,
                                    &byte_count,
                                    &zip_error));
        ASSERT(memcmp(bytes, "STATIC\n", byte_count) == 0);
        free(entry_path);
        feng_zip_free(bytes);
        bytes = NULL;
    }

    {
        char *entry_path = host_bundle_extlib_dynamic_entry_path(host_target, "helper");
        ASSERT(entry_path != NULL);
        ASSERT(feng_zip_reader_read(&reader,
                                    entry_path,
                                    &bytes,
                                    &byte_count,
                                    &zip_error));
        ASSERT(memcmp(bytes, "DYNAMIC\n", byte_count) == 0);
        free(entry_path);
        feng_zip_free(bytes);
        bytes = NULL;
    }

    ASSERT(!zip_contains_path_prefix(&reader, "empty-assets"));

    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(empty_asset_root);
    free(asset_nested_path);
    free(asset_config_path);
    free(asset_nested_dir);
    free(asset_root);
    free(extlib_dynamic_path);
    free(extlib_static_path);
    free(extlib_platform_dir);
    free(extlib_root);
    free(host_target);
    free(mod_path);
    free(mod_nested_dir);
    free(mod_dir);
    free(library_path);
    free(library_dir);
}

static void test_direct_build_releases_bundle_extlib_dynamic_libraries_only(void) {
    char template_path[] = "/tmp/feng_cli_direct_pkg_extlib_release_XXXXXX";
    char *workspace_dir;
    char *dep_src_dir;
    char *dep_source_path;
    char *dep_out_dir;
    char *dep_library_path;
    char *dep_mod_root;
    char *bundle_path;
    char *extlib_root;
    char *extlib_platform_dir;
    char *extlib_c_source_path;
    char *extlib_dynamic_path;
    char *extlib_unused_dynamic_path;
    char *extlib_static_path;
    char *consumer_src_dir;
    char *consumer_source_path;
    char *consumer_out_dir;
    char *consumer_bin_dir;
    char *consumer_binary_path;
    char *released_dynamic_path;
    char *released_unused_dynamic_path;
    char *released_static_path;
    char *released_bundle_lib_path;
    char *dynamic_name;
    char *unused_dynamic_name;
    char *static_name;
    char *host_target = NULL;
    char *error_message = NULL;
    char *stdout_text;
    char *released_text;
    char *remove_error = NULL;
    FengFbLibraryBundleSpec spec = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_fb_detect_host_target(&host_target, &error_message));
    free(error_message);
    error_message = NULL;
    dynamic_name = host_dynamic_library_file_name("helper");
    unused_dynamic_name = host_dynamic_library_file_name("unused");
    static_name = host_static_library_file_name("helper");
    ASSERT(dynamic_name != NULL);
    ASSERT(unused_dynamic_name != NULL);
    ASSERT(static_name != NULL);

    dep_src_dir = path_join(workspace_dir, "dep/src");
    dep_source_path = path_join(dep_src_dir, "dep.ff");
    dep_out_dir = path_join(workspace_dir, "dep/build");
    dep_library_path = host_static_library_output_path(dep_out_dir, "pkgextlib");
    dep_mod_root = path_join(dep_out_dir, "mod");
    bundle_path = dup_printf("%s/pkgextlib.fb", workspace_dir);
    extlib_root = path_join(workspace_dir, "dep/extlib");
    extlib_platform_dir = path_join(extlib_root, host_target);
    extlib_c_source_path = path_join(extlib_platform_dir, "helper.c");
    extlib_dynamic_path = path_join(extlib_platform_dir, dynamic_name);
    extlib_unused_dynamic_path = path_join(extlib_platform_dir, unused_dynamic_name);
    extlib_static_path = path_join(extlib_platform_dir, static_name);
    consumer_src_dir = path_join(workspace_dir, "main/src");
    consumer_source_path = path_join(consumer_src_dir, "main.ff");
    consumer_out_dir = path_join(workspace_dir, "main/build");
    consumer_bin_dir = path_join(consumer_out_dir, "bin");
    consumer_binary_path = dup_printf("%s/main", consumer_bin_dir);
    released_dynamic_path = dup_printf("%s/%s", consumer_bin_dir, dynamic_name);
    released_unused_dynamic_path = dup_printf("%s/%s", consumer_bin_dir, unused_dynamic_name);
    released_static_path = dup_printf("%s/%s", consumer_bin_dir, static_name);
    released_bundle_lib_path = host_static_library_path(consumer_bin_dir, "pkgextlib");

    mkdir_p(dep_src_dir);
    write_text_file(dep_source_path,
                    "pu mod test.cli.pkgextlib;\n"
                    "@fastcall(\"helper\")\n"
                    "pu extern fn helper_value(): int;\n");
    {
        char *out_opt = make_out_option(dep_out_dir);
        char *name_opt = dup_printf("--name=%s", "pkgextlib");
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

    mkdir_p(extlib_platform_dir);
    write_text_file(extlib_c_source_path,
                    "int helper_value(void) {\n"
                    "  return 7;\n"
                    "}\n");
    build_native_static_library_or_die(extlib_c_source_path, extlib_static_path);
    write_text_file(extlib_dynamic_path, "DYNAMIC\n");
    write_text_file(extlib_unused_dynamic_path, "UNUSED-DYNAMIC\n");

    spec.package_path = bundle_path;
    spec.package_name = "pkgextlib";
    spec.package_version = "0.1.0";
    spec.library_path = dep_library_path;
    spec.public_mod_root = dep_mod_root;
    spec.extlib_root = extlib_root;
    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
    error_message = NULL;

    mkdir_p(consumer_src_dir);
    mkdir_p(consumer_bin_dir);
    write_text_file(consumer_source_path,
                    "mod test.cli.pkgextlibmain;\n"
                    "use test.cli.pkgextlib;\n"
                    "@cdecl(\"libc\")\n"
                    "extern fn puts(msg: string*): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if helper_value() == 7 {\n"
                    "    puts(&\"ok\");\n"
                    "  }\n"
                    "}\n");
    write_text_file(released_dynamic_path, "STALE\n");
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
    ASSERT(path_exists(released_dynamic_path));
    ASSERT(!path_exists(released_unused_dynamic_path));
    ASSERT(!path_exists(released_static_path));
    ASSERT(!path_exists(released_bundle_lib_path));
    released_text = read_text_file(released_dynamic_path);
    ASSERT(strcmp(released_text, "DYNAMIC\n") == 0);
    stdout_text = run_binary_capture_stdout_or_die(consumer_binary_path);
    ASSERT(strcmp(stdout_text, "ok\n") == 0);

    free(stdout_text);
    free(released_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(host_target);
    free(static_name);
    free(unused_dynamic_name);
    free(dynamic_name);
    free(released_bundle_lib_path);
    free(released_static_path);
    free(released_unused_dynamic_path);
    free(released_dynamic_path);
    free(consumer_binary_path);
    free(consumer_bin_dir);
    free(consumer_out_dir);
    free(consumer_source_path);
    free(consumer_src_dir);
    free(extlib_static_path);
    free(extlib_unused_dynamic_path);
    free(extlib_dynamic_path);
    free(extlib_c_source_path);
    free(extlib_platform_dir);
    free(extlib_root);
    free(bundle_path);
    free(dep_mod_root);
    free(dep_library_path);
    free(dep_out_dir);
    free(dep_source_path);
    free(dep_src_dir);
}

static void test_direct_build_links_only_used_bundle_extlib_static_libraries(void) {
    char template_path[] = "/tmp/feng_cli_direct_pkg_extlib_static_used_only_XXXXXX";
    char *workspace_dir;
    char *dep_src_dir;
    char *dep_source_path;
    char *dep_out_dir;
    char *dep_library_path;
    char *dep_mod_root;
    char *bundle_path;
    char *extlib_root;
    char *extlib_platform_dir;
    char *helper_c_source_path;
    char *unused_c_source_path;
    char *helper_static_path;
    char *unused_static_path;
    char *helper_static_name;
    char *unused_static_name;
    char *consumer_src_dir;
    char *consumer_source_path;
    char *consumer_out_dir;
    char *consumer_binary_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *cc_log_text;
    char *saved_cc = NULL;
    char *host_target = NULL;
    char *error_message = NULL;
    char *stdout_text;
    char *remove_error = NULL;
    FengFbLibraryBundleSpec spec = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_fb_detect_host_target(&host_target, &error_message));
    free(error_message);
    error_message = NULL;

    helper_static_name = host_static_library_file_name("helper");
    unused_static_name = host_static_library_file_name("unused");
    ASSERT(helper_static_name != NULL);
    ASSERT(unused_static_name != NULL);

    dep_src_dir = path_join(workspace_dir, "dep/src");
    dep_source_path = path_join(dep_src_dir, "dep.ff");
    dep_out_dir = path_join(workspace_dir, "dep/build");
    dep_library_path = host_static_library_output_path(dep_out_dir, "pkgextlibstatic");
    dep_mod_root = path_join(dep_out_dir, "mod");
    bundle_path = dup_printf("%s/pkgextlibstatic.fb", workspace_dir);
    extlib_root = path_join(workspace_dir, "dep/extlib");
    extlib_platform_dir = path_join(extlib_root, host_target);
    helper_c_source_path = path_join(extlib_platform_dir, "helper.c");
    unused_c_source_path = path_join(extlib_platform_dir, "unused.c");
    helper_static_path = path_join(extlib_platform_dir, helper_static_name);
    unused_static_path = path_join(extlib_platform_dir, unused_static_name);
    consumer_src_dir = path_join(workspace_dir, "main/src");
    consumer_source_path = path_join(consumer_src_dir, "main.ff");
    consumer_out_dir = path_join(workspace_dir, "main/build");
    consumer_binary_path = path_join(consumer_out_dir, "bin/main");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);

    mkdir_p(dep_src_dir);
    write_text_file(dep_source_path,
                    "pu mod test.cli.pkgextlibstatic;\n"
                    "@stdcall(\"helper\")\n"
                    "pu extern fn helper_value(): int;\n");
    {
        char *out_opt = make_out_option(dep_out_dir);
        char *name_opt = dup_printf("--name=%s", "pkgextlibstatic");
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

    mkdir_p(extlib_platform_dir);
    write_text_file(helper_c_source_path,
                    "int helper_value(void) {\n"
                    "  return 41;\n"
                    "}\n");
    write_text_file(unused_c_source_path,
                    "int unused_value(void) {\n"
                    "  return 0;\n"
                    "}\n");
    build_native_static_library_or_die(helper_c_source_path, helper_static_path);
    build_native_static_library_or_die(unused_c_source_path, unused_static_path);

    spec.package_path = bundle_path;
    spec.package_name = "pkgextlibstatic";
    spec.package_version = "0.1.0";
    spec.library_path = dep_library_path;
    spec.public_mod_root = dep_mod_root;
    spec.extlib_root = extlib_root;
    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
    error_message = NULL;

    mkdir_p(consumer_src_dir);
    write_text_file(consumer_source_path,
                    "mod test.cli.pkgextlibstaticconsumer;\n"
                    "use test.cli.pkgextlibstatic;\n"
                    "@cdecl(\"libc\")\n"
                    "extern fn puts(msg: string*): int;\n"
                    "fn main(args: string[]) {\n"
                    "  if helper_value() == 41 {\n"
                    "    puts(&\"ok\");\n"
                    "  } else {\n"
                    "    puts(&\"bad\");\n"
                    "  }\n"
                    "}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);
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
    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    ASSERT(path_exists(consumer_binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(strstr(cc_log_text, helper_static_name) != NULL);
    ASSERT(strstr(cc_log_text, unused_static_name) == NULL);
    stdout_text = run_binary_capture_stdout_or_die(consumer_binary_path);
    ASSERT(strcmp(stdout_text, "ok\n") == 0);

    free(stdout_text);
    free(cc_log_text);
    free(saved_cc);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(host_target);
    free(unused_static_name);
    free(helper_static_name);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(consumer_binary_path);
    free(consumer_out_dir);
    free(consumer_source_path);
    free(consumer_src_dir);
    free(unused_static_path);
    free(helper_static_path);
    free(unused_c_source_path);
    free(helper_c_source_path);
    free(extlib_platform_dir);
    free(extlib_root);
    free(bundle_path);
    free(dep_mod_root);
    free(dep_library_path);
    free(dep_out_dir);
    free(dep_source_path);
    free(dep_src_dir);
}

static void test_direct_build_maps_cli_library_name_to_link_flag(void) {
    char template_path[] = "/tmp/feng_cli_direct_cli_lib_name_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *out_dir;
    char *binary_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *cc_log_text;
    char *out_opt;
    char *lib_opt;
    char *saved_cc = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    src_dir = path_join(workspace_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    out_dir = path_join(workspace_dir, "build");
    binary_path = path_join(out_dir, "bin/main");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);

    mkdir_p(src_dir);
    write_text_file(source_path,
                    "mod test.cli.directclilibname;\n"
                    "fn main(args: string[]) {}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);

    out_opt = make_out_option(out_dir);
    lib_opt = make_lib_option("m");
    {
        char *argv[] = {
            source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            lib_opt,
        };
        ASSERT(feng_cli_direct_main("feng", 5, argv) == 0);
    }

    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    ASSERT(path_exists(binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(strstr(cc_log_text, "-lm") != NULL);

    free(saved_cc);
    free(lib_opt);
    free(out_opt);
    free(cc_log_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(binary_path);
    free(out_dir);
    free(source_path);
    free(src_dir);
}

static void test_direct_build_passes_cli_library_path_verbatim(void) {
    char template_path[] = "/tmp/feng_cli_direct_cli_lib_path_XXXXXX";
    char *workspace_dir;
    char *native_dir;
    char *native_source_path;
    char *native_library_path;
    char *src_dir;
    char *source_path;
    char *out_dir;
    char *binary_path;
    char *cc_log_path;
    char *cc_wrapper_path;
    char *cc_log_text;
    char *out_opt;
    char *saved_cc = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    native_dir = path_join(workspace_dir, "native");
    native_source_path = path_join(native_dir, "helper.c");
    native_library_path = host_static_library_path(native_dir, "helper");
    src_dir = path_join(workspace_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    out_dir = path_join(workspace_dir, "build");
    binary_path = path_join(out_dir, "bin/main");
    cc_log_path = path_join(workspace_dir, "cc.log");
    cc_wrapper_path = create_logging_cc_wrapper(workspace_dir, cc_log_path);

    mkdir_p(native_dir);
    mkdir_p(src_dir);
    write_text_file(native_source_path,
                    "int helper_unused(void) {\n"
                    "  return 1;\n"
                    "}\n");
    build_native_static_library_or_die(native_source_path, native_library_path);
    write_text_file(source_path,
                    "mod test.cli.directclilibpath;\n"
                    "fn main(args: string[]) {}\n");

    if (getenv("CC") != NULL) {
        saved_cc = dup_cstr(getenv("CC"));
    }
    ASSERT(setenv("CC", cc_wrapper_path, 1) == 0);

    out_opt = make_out_option(out_dir);
    {
        char *argv[] = {
            source_path,
            "--target=bin",
            out_opt,
            "--name=main",
            "--lib",
            native_library_path,
        };
        ASSERT(feng_cli_direct_main("feng", 6, argv) == 0);
    }

    if (saved_cc != NULL) {
        ASSERT(setenv("CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("CC") == 0);
    }

    ASSERT(path_exists(binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(strstr(cc_log_text, native_library_path) != NULL);

    free(saved_cc);
    free(out_opt);
    free(cc_log_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cc_wrapper_path);
    free(cc_log_path);
    free(binary_path);
    free(out_dir);
    free(source_path);
    free(src_dir);
    free(native_library_path);
    free(native_source_path);
    free(native_dir);
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

static void test_project_build_help_writes_stdout_and_returns_success(void) {
    char *argv[] = { "--help" };
    char *stdout_text;
    char *stderr_text = NULL;
    int rc;

    stdout_text = run_cli_entry_capture_stdout_and_stderr(feng_cli_project_build_main,
                                                          1,
                                                          argv,
                                                          &rc,
                                                          &stderr_text);

    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "Usage:\n  feng build [<path>] [--release]\n") != NULL);
    ASSERT(stderr_text[0] == '\0');

    free(stderr_text);
    free(stdout_text);
}

static void test_project_pack_help_writes_stdout_and_returns_success(void) {
    char *argv[] = { "--help" };
    char *stdout_text;
    char *stderr_text = NULL;
    int rc;

    stdout_text = run_cli_entry_capture_stdout_and_stderr(feng_cli_project_pack_main,
                                                          1,
                                                          argv,
                                                          &rc,
                                                          &stderr_text);

    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "Usage:\n  feng pack [<path>]\n") != NULL);
    ASSERT(stderr_text[0] == '\0');

    free(stderr_text);
    free(stdout_text);
}

static void test_lsp_help_returns_success(void) {
    char *argv[] = { "--help" };

    ASSERT(run_lsp_quiet_stderr(1, argv) == 0);
}

static void test_lsp_help_writes_stdout_not_stderr(void) {
    char *argv[] = { "--help" };
    char *stdout_text;
    char *stderr_text = NULL;
    int rc;

    stdout_text = run_cli_entry_capture_stdout_and_stderr(feng_cli_lsp_main,
                                                          1,
                                                          argv,
                                                          &rc,
                                                          &stderr_text);

    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "Start Feng Language Server on stdio.\n") != NULL);
    ASSERT(stderr_text[0] == '\0');

    free(stderr_text);
    free(stdout_text);
}

static void test_lsp_rejects_unknown_option(void) {
    char *argv[] = { "--bogus" };

    ASSERT(run_lsp_quiet_stderr(1, argv) != 0);
}

static void test_lsp_unknown_option_stays_on_stderr(void) {
    char *argv[] = { "--bogus" };
    char *stdout_text;
    char *stderr_text = NULL;
    int rc;

    stdout_text = run_cli_entry_capture_stdout_and_stderr(feng_cli_lsp_main,
                                                          1,
                                                          argv,
                                                          &rc,
                                                          &stderr_text);

    ASSERT(rc != 0);
    ASSERT(stdout_text[0] == '\0');
    ASSERT(strstr(stderr_text, "unknown option: --bogus\n") != NULL);
    ASSERT(strstr(stderr_text, "Usage:\n  feng lsp [--stdio]\n") != NULL);

    free(stderr_text);
    free(stdout_text);
}

/* Ensure `feng dap --help` exits successfully. */
static void test_dap_help_returns_success(void) {
    char *argv[] = { "--help" };

    ASSERT(run_dap_quiet_stderr(1, argv) == 0);
}

static void test_dap_help_writes_stdout_not_stderr(void) {
    char *argv[] = { "--help" };
    char *stdout_text;
    char *stderr_text = NULL;
    int rc;

    stdout_text = run_cli_entry_capture_stdout_and_stderr(feng_cli_dap_main,
                                                          1,
                                                          argv,
                                                          &rc,
                                                          &stderr_text);

    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "Start Feng Debug Adapter Protocol proxy on stdio.\n") != NULL);
    ASSERT(stderr_text[0] == '\0');

    free(stderr_text);
    free(stdout_text);
}

static void test_deps_add_help_writes_stdout_and_returns_success(void) {
    char *argv[] = { "add", "--help" };
    char *stdout_text;
    char *stderr_text = NULL;
    int rc;

    stdout_text = run_cli_entry_capture_stdout_and_stderr(feng_cli_deps_main,
                                                          2,
                                                          argv,
                                                          &rc,
                                                          &stderr_text);

    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "feng deps add <pkg-name> <version-or-path> [<path>]\n") != NULL);
    ASSERT(stderr_text[0] == '\0');

    free(stderr_text);
    free(stdout_text);
}


/* Ensure `feng dap` rejects unsupported command-line options. */
static void test_dap_rejects_unknown_option(void) {
    char *argv[] = { "--bogus" };

    ASSERT(run_dap_quiet_stderr(1, argv) != 0);
}

/* Ensure a validated launch starts the backend only after `.fd` verification. */
static void test_dap_validated_launch_starts_backend(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x11U};
    char template_path[] = "/tmp/feng_cli_dap_launch_ok_XXXXXX";
    char *workspace_dir;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *marker_path;
    char *backend_script;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *initialize_text;
    char *launch_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    marker_path = path_join(workspace_dir, "spawned.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               NULL,
                               0U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);
    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = dup_printf("Content-Length: %zu\\r\\n\\r\\n%s",
                                         strlen(backend_initialize_json),
                                         backend_initialize_json);
    backend_script = dup_printf("#!/bin/sh\nprintf 'spawned' > \"%s\"\nprintf '%%b' '%s'\n/bin/cat >/dev/null\n",
                                marker_path,
                                backend_initialize_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    input_text = dup_printf("%s%s", initialize_text, launch_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "\"command\":\"initialize\"") != NULL);
    ASSERT(strstr(stdout_text, "\"success\":true") != NULL);
    ASSERT(path_exists(marker_path));
    ASSERT(strcmp(stderr_text, "") == 0);

    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(launch_text);
    free(initialize_text);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(marker_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure launch falls back to `xcrun -f lldb-dap` when PATH misses lldb-dap. */
static void test_dap_resolves_backend_via_xcrun_when_path_misses(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x12U};
    char template_path[] = "/tmp/feng_cli_dap_launch_xcrun_XXXXXX";
    char *workspace_dir;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *xcrun_path;
    char *marker_path;
    char *backend_script;
    char *xcrun_script;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *initialize_text;
    char *launch_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "resolved-lldb-dap");
    xcrun_path = path_join(workspace_dir, "xcrun");
    marker_path = path_join(workspace_dir, "spawned.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               NULL,
                               0U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);
    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = dup_printf("Content-Length: %zu\\r\\n\\r\\n%s",
                                         strlen(backend_initialize_json),
                                         backend_initialize_json);
    backend_script = dup_printf("#!/bin/sh\nprintf 'spawned' > \"%s\"\nprintf '%%b' '%s'\ncat >/dev/null\n",
                                marker_path,
                                backend_initialize_text);
    xcrun_script = dup_printf("#!/bin/sh\nif [ \"$1\" = \"-f\" ] && [ \"$2\" = \"lldb-dap\" ]; then\n  printf '%%s\\n' \"%s\"\n  exit 0\nfi\nexit 1\n",
                              backend_path);
    write_executable_text_file(backend_path, backend_script);
    write_executable_text_file(xcrun_path, xcrun_script);
    path_value = dup_printf("%s:/bin:/usr/bin:/usr/sbin:/sbin", workspace_dir);

    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    input_text = dup_printf("%s%s", initialize_text, launch_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    ASSERT(strstr(stdout_text, "\"command\":\"initialize\"") != NULL);
    ASSERT(strstr(stdout_text, "\"success\":true") != NULL);
    ASSERT(path_exists(marker_path));
    ASSERT(strcmp(stderr_text, "") == 0);

    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(launch_text);
    free(initialize_text);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(xcrun_script);
    free(backend_script);
    free(marker_path);
    free(xcrun_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure launch fails locally when the `.fd` fingerprint no longer matches the binary. */
static void test_dap_rejects_fingerprint_mismatch_before_backend_spawn(void) {
    static const unsigned char kOriginalBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x21U};
    static const unsigned char kModifiedBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x22U};
    char template_path[] = "/tmp/feng_cli_dap_launch_mismatch_XXXXXX";
    char *workspace_dir;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *marker_path;
    char *backend_script;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *initialize_text;
    char *launch_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    marker_path = path_join(workspace_dir, "spawned.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kOriginalBinaryBytes, sizeof(kOriginalBinaryBytes));
    feng_codegen_maping_info_init(&info);
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               NULL,
                               0U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);
    write_binary_file(binary_path, kModifiedBinaryBytes, sizeof(kModifiedBinaryBytes));
    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = dup_printf("Content-Length: %zu\\r\\n\\r\\n%s",
                                         strlen(backend_initialize_json),
                                         backend_initialize_json);
    backend_script = dup_printf("#!/bin/sh\nprintf 'spawned' > \"%s\"\nprintf '%%b' '%s'\ncat >/dev/null\n",
                                marker_path,
                                backend_initialize_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    input_text = dup_printf("%s%s", initialize_text, launch_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc != 0);
    ASSERT(strstr(stdout_text, "\"command\":\"initialize\"") != NULL);
    ASSERT(strstr(stdout_text, "\"command\":\"launch\"") != NULL);
    ASSERT(strstr(stdout_text, "\"success\":false") != NULL);
    ASSERT(strstr(stdout_text, "fingerprint mismatch") != NULL);
    ASSERT(!path_exists(marker_path));
    ASSERT(strcmp(stderr_text, "") == 0);

    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(launch_text);
    free(initialize_text);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(marker_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure launch surfaces backend startup failures after local validation succeeds. */
static void test_dap_reports_missing_backend_after_launch_validation(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x31U};
    char template_path[] = "/tmp/feng_cli_dap_missing_backend_XXXXXX";
    char *workspace_dir;
    char *binary_path;
    char *fd_path;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *initialize_text;
    char *launch_text;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               NULL,
                               0U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    input_text = dup_printf("%s%s", initialize_text, launch_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   workspace_dir,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc != 0);
    ASSERT(strstr(stdout_text, "\"command\":\"initialize\"") != NULL);
    ASSERT(strstr(stdout_text, "\"command\":\"launch\"") != NULL);
    ASSERT(strstr(stdout_text, "failed to exec lldb-dap") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(launch_text);
    free(initialize_text);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(fd_path);
    free(binary_path);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure setBreakpoints rewrites local source paths to package URIs before forwarding. */
static void test_dap_rewrites_set_breakpoints_source_path_to_package_uri(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x41U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_breakpoints_uri_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *escaped_source_path;
    char *initialize_json;
    char *launch_json;
    char *set_breakpoints_json;
    char *initialize_text;
    char *launch_text;
    char *set_breakpoints_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_script = dup_printf("#!/bin/sh\nprintf '%%b' '%s'\ncat > \"%s\"\n",
                                backend_initialize_text,
                                requests_path);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    escaped_source_path = json_escape_text(source_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    set_breakpoints_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":1}]}}",
                                      escaped_source_path);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    set_breakpoints_text = build_dap_message_text(set_breakpoints_json);
    input_text = dup_printf("%s%s%s", initialize_text, launch_text, set_breakpoints_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"setBreakpoints\"") != NULL);
    ASSERT(strstr(requests_text, "\"path\":\"demo.pkg://main.ff\"") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(set_breakpoints_text);
    free(launch_text);
    free(initialize_text);
    free(set_breakpoints_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_source_path);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure setBreakpoints fails locally when the file is outside the debug closure. */
static void test_dap_rejects_set_breakpoints_outside_debug_closure(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x42U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_breakpoints_reject_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *other_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *escaped_other_path;
    char *initialize_json;
    char *launch_json;
    char *set_breakpoints_json;
    char *initialize_text;
    char *launch_text;
    char *set_breakpoints_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    other_path = path_join(workspace_dir, "other.ff");
    write_text_file(source_path, kSourceText);
    write_text_file(other_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_script = dup_printf("#!/bin/sh\nprintf '%%b' '%s'\ncat > \"%s\"\n",
                                backend_initialize_text,
                                requests_path);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    escaped_other_path = json_escape_text(other_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    set_breakpoints_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":1}]}}",
                                      escaped_other_path);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    set_breakpoints_text = build_dap_message_text(set_breakpoints_json);
    input_text = dup_printf("%s%s%s", initialize_text, launch_text, set_breakpoints_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(stdout_text, "\"command\":\"setBreakpoints\"") != NULL);
    ASSERT(strstr(stdout_text, "not part of the debug closure") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"setBreakpoints\"") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(set_breakpoints_text);
    free(launch_text);
    free(initialize_text);
    free(set_breakpoints_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_other_path);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(other_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure stackTrace responses rewrite backend frame names and package URIs to editor-facing values. */
static void test_dap_rewrites_stack_trace_source_path_to_local_path(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x43U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_stacktrace_path_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_script = dup_printf("#!/bin/sh\nprintf '%%b' '%s'\ncat > \"%s\"\nprintf '%%b' '%s'\n",
                                backend_initialize_text,
                                requests_path,
                                backend_stack_trace_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    input_text = dup_printf("%s%s%s", initialize_text, launch_text, stack_trace_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"stackTrace\"") != NULL);
    ASSERT(strstr(stdout_text, source_path) != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"demo.pkg.main\"") != NULL);
    ASSERT(strstr(stdout_text, "demo_pkg_main_backend") == NULL);
    ASSERT(strstr(stdout_text, "demo.pkg://main.ff") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure stackTrace also rewrites LLDB's pkg:/path variant back to the local source path. */
static void test_dap_rewrites_stack_trace_compiler_normalized_source_path(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x4aU};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_stacktrace_uri_variant_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *backend_variant_path;
    char *escaped_backend_variant_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    backend_variant_path = dup_printf("%s/demo.pkg:/main.ff", workspace_dir);
    ASSERT(fd_path != NULL);
    ASSERT(backend_variant_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    escaped_backend_variant_path = json_escape_text(backend_variant_path);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"%s\"},\"line\":3,\"column\":1}],\"totalFrames\":1}}",
                                      escaped_backend_variant_path);
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_script = dup_printf("#!/bin/sh\nprintf '%%b' '%s'\ncat > \"%s\"\nprintf '%%b' '%s'\n",
                                backend_initialize_text,
                                requests_path,
                                backend_stack_trace_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    input_text = dup_printf("%s%s%s", initialize_text, launch_text, stack_trace_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"stackTrace\"") != NULL);
    ASSERT(strstr(stdout_text, source_path) != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"demo.pkg.main\"") != NULL);
    ASSERT(strstr(stdout_text, backend_variant_path) == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(escaped_backend_variant_path);
    free(backend_variant_path);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure stackTrace hides frames marked hidden in .fd frame policy. */
static void test_dap_hides_hidden_stack_trace_frames(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x44U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_stacktrace_hidden_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "main",
                                              "main",
                                              FENG_CODEGEN_MAPING_FRAME_HIDDEN));
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":99,\"name\":\"main\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":1,\"column\":1},{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":1}],\"totalFrames\":2}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_script = dup_printf("#!/bin/sh\nprintf '%%b' '%s'\ncat > \"%s\"\nprintf '%%b' '%s'\n",
                                backend_initialize_text,
                                requests_path,
                                backend_stack_trace_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    input_text = dup_printf("%s%s%s", initialize_text, launch_text, stack_trace_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"stackTrace\"") != NULL);
    ASSERT(strstr(stdout_text, "\"id\":99") == NULL);
    ASSERT(strstr(stdout_text, "\"id\":7") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"demo.pkg.main\"") != NULL);
    ASSERT(strstr(stdout_text, "\"totalFrames\":1") != NULL);
    ASSERT(strstr(stdout_text, source_path) != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure variables responses rewrite backend variable names to Feng names. */
static void test_dap_rewrites_variables_to_feng_names(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x45U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "    let answer = 42;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_variables_names_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *scopes_json;
    char *variables_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *scopes_text;
    char *variables_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_scopes_json;
    char *backend_scopes_text;
    char *backend_variables_json;
    char *backend_variables_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_param",
                                                                   "args",
                                                                   NULL,
                                                                   "string[]",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_PARAM));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "answer",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_param\",\"evaluateName\":\"backend_param\",\"value\":\"[]\",\"type\":\"string[]\",\"variablesReference\":0},{\"name\":\"backend_local\",\"evaluateName\":\"backend_local\",\"value\":\"42\",\"type\":\"int\",\"variablesReference\":0}]}}");
    backend_variables_text = build_dap_message_text(backend_variables_json);
    backend_script = dup_printf("#!/bin/sh\nprintf '%%b' '%s'\ncat > \"%s\"\nprintf '%%b' '%s'\nprintf '%%b' '%s'\nprintf '%%b' '%s'\n",
                                backend_initialize_text,
                                requests_path,
                                backend_stack_trace_text,
                                backend_scopes_text,
                                backend_variables_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    scopes_json = dup_printf("{\"seq\":4,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":7}}");
    variables_json = dup_printf("{\"seq\":5,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":101}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    scopes_text = build_dap_message_text(scopes_json);
    variables_text = build_dap_message_text(variables_json);
    input_text = dup_printf("%s%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text,
                            scopes_text);

    stdout_text = run_dap_interactive_capture_stdout_with_path(1,
                                                               argv,
                                                               input_text,
                                                               "\"command\":\"scopes\"",
                                                               variables_text,
                                                               path_value,
                                                               &rc,
                                                               &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"stackTrace\"") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"scopes\"") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"variables\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"args\"") != NULL);
    ASSERT(strstr(stdout_text, "\"evaluateName\":\"args\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"answer\"") != NULL);
    ASSERT(strstr(stdout_text, "\"evaluateName\":\"answer\"") != NULL);
    ASSERT(strstr(stdout_text, "backend_param") == NULL);
    ASSERT(strstr(stdout_text, "backend_local") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(variables_text);
    free(scopes_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(variables_json);
    free(scopes_json);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(backend_variables_text);
    free(backend_variables_json);
    free(backend_scopes_text);
    free(backend_scopes_json);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure top-level variables keep only user mappings and surface user-facing values. */
static void test_dap_filters_backend_variables_and_rewrites_user_values(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x48U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "    for var i = 1; i <= 1000; i += 1 {\n"
        "        println(\"Hello, world!\");\n"
        "    }\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_variables_filter_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *scopes_json;
    char *variables_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *scopes_text;
    char *variables_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_scopes_json;
    char *backend_scopes_text;
    char *backend_variables_json;
    char *backend_variables_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_scopes_text;
    char *escaped_backend_variables_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_param",
                                                                   "args",
                                                                   NULL,
                                                                   "string[]",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_PARAM));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "capture_cell",
                                                                   "captured",
                                                                   "(capture_cell->value)",
                                                                   "string",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_CAPTURE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "i",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_param\",\"evaluateName\":\"backend_param\",\"value\":\"0x1000\",\"type\":\"FengArray *\",\"variablesReference\":23},{\"name\":\"capture_cell\",\"evaluateName\":\"capture_cell\",\"value\":\"0x2000\",\"type\":\"FengCaptureCell *\",\"variablesReference\":24},{\"name\":\"backend_local\",\"evaluateName\":\"backend_local\",\"value\":\"8593820128\",\"type\":\"int\",\"variablesReference\":0},{\"name\":\"_old4\",\"evaluateName\":\"_old4\",\"value\":\"22\",\"type\":\"int\",\"variablesReference\":0}]}}");
    backend_variables_text = build_dap_message_text(backend_variables_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_scopes_text = json_escape_text(backend_scopes_text);
    escaped_backend_variables_text = json_escape_text(backend_variables_text);
    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = {\n"
                                "  initialize: \"%s\",\n"
                                "  stackTrace: \"%s\",\n"
                                "  scopes: \"%s\",\n"
                                "  variables: \"%s\"\n"
                                "};\n"
                                "function frame(payload) {\n"
                                "  return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\\r\\n\\r\\n${payload}`;\n"
                                "}\n"
                                "let requests = '';\n"
                                "let buffer = Buffer.alloc(0);\n"
                                "process.stdout.write(responses.initialize);\n"
                                "process.stdin.on('data', chunk => {\n"
                                "  requests += chunk.toString('utf8');\n"
                                "  fs.writeFileSync(requestsPath, requests);\n"
                                "  buffer = Buffer.concat([buffer, chunk]);\n"
                                "  for (;;) {\n"
                                "    const sep = buffer.indexOf('\\r\\n\\r\\n');\n"
                                "    if (sep < 0) break;\n"
                                "    const header = buffer.slice(0, sep).toString('utf8');\n"
                                "    const match = /Content-Length: (\\d+)/i.exec(header);\n"
                                "    if (!match) break;\n"
                                "    const length = Number(match[1]);\n"
                                "    const frameLength = sep + 4 + length;\n"
                                "    if (buffer.length < frameLength) break;\n"
                                "    const payload = buffer.slice(sep + 4, frameLength).toString('utf8');\n"
                                "    buffer = buffer.slice(frameLength);\n"
                                "    const message = JSON.parse(payload);\n"
                                "    if (message.command === 'stackTrace') process.stdout.write(responses.stackTrace);\n"
                                "    if (message.command === 'scopes') process.stdout.write(responses.scopes);\n"
                                "    if (message.command === 'variables') process.stdout.write(responses.variables);\n"
                                "    if (message.command === 'evaluate') {\n"
                                "      let body = { result: '0', type: 'int', variablesReference: 0 };\n"
                                "      if (message.arguments.expression === '(capture_cell->value)') {\n"
                                "        body = { result: 'captured-value', type: 'string', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === 'backend_local') {\n"
                                "        body = { result: '1', type: 'int', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === 'backend_param') {\n"
                                "        body = { result: '0x1000', type: 'FengArray *', variablesReference: 23 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_array_length((const FengArray *)(backend_param))') {\n"
                                "        body = { result: '0', type: 'size_t', variablesReference: 0 };\n"
                                "      }\n"
                                "      const response = {\n"
                                "        seq: 5,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'evaluate',\n"
                                "        body\n"
                                "      };\n"
                                "      process.stdout.write(frame(JSON.stringify(response)));\n"
                                "    }\n"
                                "  }\n"
                                "});\n"
                                "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); process.exit(0); });\n",
                                requests_path,
                                escaped_backend_initialize_text,
                                escaped_backend_stack_trace_text,
                                escaped_backend_scopes_text,
                                escaped_backend_variables_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    scopes_json = dup_printf("{\"seq\":4,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":7}}");
    variables_json = dup_printf("{\"seq\":5,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":101}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    scopes_text = build_dap_message_text(scopes_json);
    variables_text = build_dap_message_text(variables_json);
    input_text = dup_printf("%s%s%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text,
                            scopes_text,
                            variables_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"variables\"") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"evaluate\"") != NULL);
    ASSERT(strstr(requests_text, "\"expression\":\"backend_param\"") != NULL);
    ASSERT(strstr(requests_text, "\"expression\":\"(size_t)feng_array_length((const FengArray *)(backend_param))\"") != NULL);
    ASSERT(strstr(requests_text, "\"expression\":\"backend_local\"") != NULL);
    ASSERT(strstr(requests_text, "\"expression\":\"(capture_cell->value)\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"args\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"string[length=0]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"captured\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"captured-value\"") != NULL);
    ASSERT(strstr(stdout_text, "\"type\":\"string\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"i\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"1\"") != NULL);
    ASSERT(strstr(stdout_text, "backend_param") == NULL);
    ASSERT(strstr(stdout_text, "capture_cell") == NULL);
    ASSERT(strstr(stdout_text, "8593820128") == NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"_old4\"") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(variables_text);
    free(scopes_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(variables_json);
    free(scopes_json);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(escaped_backend_variables_text);
    free(escaped_backend_scopes_text);
    free(escaped_backend_stack_trace_text);
    free(escaped_backend_initialize_text);
    free(backend_variables_text);
    free(backend_variables_json);
    free(backend_scopes_text);
    free(backend_scopes_json);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_dap_uses_array_element_type_name_in_value_summary(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x49U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "    println(\"Hello, world!\");\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_array_summary_type_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *scopes_json;
    char *variables_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *scopes_text;
    char *variables_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_scopes_json;
    char *backend_scopes_text;
    char *backend_variables_json;
    char *backend_variables_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_scopes_text;
    char *escaped_backend_variables_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(
        &info,
        "demo_pkg_main_backend",
        "backend_param",
        "args",
        NULL,
        "string[]",
        FENG_CODEGEN_MAPING_VARIABLE_PARAM));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":2,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_param\",\"evaluateName\":\"backend_param\",\"value\":\"0x1000\",\"type\":\"FengArray *\",\"variablesReference\":23}]}}");
    backend_variables_text = build_dap_message_text(backend_variables_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_scopes_text = json_escape_text(backend_scopes_text);
    escaped_backend_variables_text = json_escape_text(backend_variables_text);
    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = {\n"
                                "  initialize: \"%s\",\n"
                                "  stackTrace: \"%s\",\n"
                                "  scopes: \"%s\",\n"
                                "  variables: \"%s\"\n"
                                "};\n"
                                "function frame(payload) {\n"
                                "  return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\\r\\n\\r\\n${payload}`;\n"
                                "}\n"
                                "let requests = '';\n"
                                "let buffer = Buffer.alloc(0);\n"
                                "process.stdout.write(responses.initialize);\n"
                                "process.stdin.on('data', chunk => {\n"
                                "  requests += chunk.toString('utf8');\n"
                                "  fs.writeFileSync(requestsPath, requests);\n"
                                "  buffer = Buffer.concat([buffer, chunk]);\n"
                                "  for (;;) {\n"
                                "    const sep = buffer.indexOf('\\r\\n\\r\\n');\n"
                                "    if (sep < 0) break;\n"
                                "    const header = buffer.slice(0, sep).toString('utf8');\n"
                                "    const match = /Content-Length: (\\d+)/i.exec(header);\n"
                                "    if (!match) break;\n"
                                "    const length = Number(match[1]);\n"
                                "    const frameLength = sep + 4 + length;\n"
                                "    if (buffer.length < frameLength) break;\n"
                                "    const payload = buffer.slice(sep + 4, frameLength).toString('utf8');\n"
                                "    buffer = buffer.slice(frameLength);\n"
                                "    const message = JSON.parse(payload);\n"
                                "    if (message.command === 'stackTrace') process.stdout.write(responses.stackTrace);\n"
                                "    if (message.command === 'scopes') process.stdout.write(responses.scopes);\n"
                                "    if (message.command === 'variables') process.stdout.write(responses.variables);\n"
                                "    if (message.command === 'evaluate') {\n"
                                "      let body = { result: '0', type: 'int', variablesReference: 0 };\n"
                                "      if (message.arguments.expression === 'backend_param') {\n"
                                "        body = { result: '0x1000', type: 'FengArray *', variablesReference: 23 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_array_length((const FengArray *)(backend_param))') {\n"
                                "        body = { result: '0', type: 'size_t', variablesReference: 0 };\n"
                                "      }\n"
                                "      const response = {\n"
                                "        seq: 5,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'evaluate',\n"
                                "        body\n"
                                "      };\n"
                                "      process.stdout.write(frame(JSON.stringify(response)));\n"
                                "    }\n"
                                "  }\n"
                                "});\n"
                                "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); process.exit(0); });\n",
                                requests_path,
                                escaped_backend_initialize_text,
                                escaped_backend_stack_trace_text,
                                escaped_backend_scopes_text,
                                escaped_backend_variables_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    scopes_json = dup_printf("{\"seq\":4,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":7}}");
    variables_json = dup_printf("{\"seq\":5,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":101}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    scopes_text = build_dap_message_text(scopes_json);
    variables_text = build_dap_message_text(variables_json);
    input_text = dup_printf("%s%s%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text,
                            scopes_text,
                            variables_text);

    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   path_value,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"expression\":\"backend_param\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(size_t)feng_array_length((const FengArray *)(backend_param))\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"args\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"string[length=0]\"") != NULL);
    ASSERT(strstr(stdout_text, "backend_param") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(variables_text);
    free(scopes_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(variables_json);
    free(scopes_json);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(escaped_backend_variables_text);
    free(escaped_backend_scopes_text);
    free(escaped_backend_stack_trace_text);
    free(escaped_backend_initialize_text);
    free(backend_variables_text);
    free(backend_variables_json);
    free(backend_scopes_text);
    free(backend_scopes_json);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_project_build_keeps_for_body_breakpoint_after_init_in_dwarf(void) {
#if !defined(__APPLE__)
    return;
#else
    char template_path[] = "/tmp/feng_cli_dwarf_for_breakpoint_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *dwarf_path;
    char *dump_path;
    char *command;
    char *dump_text;
    char *remove_error = NULL;
    unsigned long long header_addr = 0ULL;
    unsigned long long body_addr = 0ULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "demo");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"demo\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(source_path,
                    "mod demo;\n"
                    "\n"
                    "fn main(args: string[]) {\n"
                    "    for var i = 1; i <= 3; i += 1 {\n"
                    "        let j = i;\n"
                    "    }\n"
                    "}\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    dwarf_path = path_join(project_dir, "build/bin/demo.dSYM/Contents/Resources/DWARF/demo");
    dump_path = path_join(workspace_dir, "dwarfdump.txt");
    ASSERT(path_exists(dwarf_path));

    command = dup_printf("xcrun dwarfdump --debug-line \"%s\" > \"%s\"",
                         dwarf_path,
                         dump_path);
    ASSERT(command != NULL);
    run_command_or_die(command);
    dump_text = read_text_file(dump_path);

    ASSERT(find_first_dwarfdump_address_for_line(dump_text, "main.ff", 4U, &header_addr));
    ASSERT(find_first_dwarfdump_address_for_line(dump_text, "main.ff", 5U, &body_addr));
    ASSERT(header_addr < body_addr);

    free(dump_text);
    free(command);
    free(dump_path);
    free(dwarf_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
#endif
}

/* Ensure identifier evaluate requests rewrite Feng names to backend names. */
static void test_dap_rewrites_identifier_evaluate_expression(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x46U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "    let answer = 42;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_evaluate_ident_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *evaluate_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *evaluate_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_evaluate_json;
    char *backend_evaluate_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_evaluate_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    feng_codegen_maping_info_init(&info);
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "answer",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_evaluate_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"42\",\"type\":\"int\",\"variablesReference\":0}}");
    backend_evaluate_text = build_dap_message_text(backend_evaluate_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_evaluate_text = json_escape_text(backend_evaluate_text);
    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = {\n"
                                "  initialize: \"%s\",\n"
                                "  stackTrace: \"%s\",\n"
                                "  evaluate: \"%s\"\n"
                                "};\n"
                                "let requests = '';\n"
                                "let buffer = Buffer.alloc(0);\n"
                                "process.stdout.write(responses.initialize);\n"
                                "process.stdin.on('data', chunk => {\n"
                                "  requests += chunk.toString('utf8');\n"
                                "  buffer = Buffer.concat([buffer, chunk]);\n"
                                "  for (;;) {\n"
                                "    const sep = buffer.indexOf('\\r\\n\\r\\n');\n"
                                "    if (sep < 0) break;\n"
                                "    const header = buffer.slice(0, sep).toString('utf8');\n"
                                "    const match = /Content-Length: (\\d+)/i.exec(header);\n"
                                "    if (!match) break;\n"
                                "    const length = Number(match[1]);\n"
                                "    const frameLength = sep + 4 + length;\n"
                                "    if (buffer.length < frameLength) break;\n"
                                "    const payload = buffer.slice(sep + 4, frameLength).toString('utf8');\n"
                                "    buffer = buffer.slice(frameLength);\n"
                                "    const message = JSON.parse(payload);\n"
                                "    if (message.command === 'stackTrace') process.stdout.write(responses.stackTrace);\n"
                                "    if (message.command === 'evaluate') process.stdout.write(responses.evaluate);\n"
                                "  }\n"
                                "});\n"
                                "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); });\n",
                                requests_path,
                                escaped_backend_initialize_text,
                                escaped_backend_stack_trace_text,
                                escaped_backend_evaluate_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    evaluate_json = dup_printf("{\"seq\":4,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"answer\",\"frameId\":7,\"context\":\"watch\"}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    evaluate_text = build_dap_message_text(evaluate_json);
    input_text = dup_printf("%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text);

    stdout_text = run_dap_interactive_capture_stdout_with_path(1,
                                                               argv,
                                                               input_text,
                                                               "\"command\":\"stackTrace\"",
                                                               evaluate_text,
                                                               path_value,
                                                               &rc,
                                                               &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"command\":\"evaluate\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"command\":\"evaluate\",\"arguments\":{\"expression\":\"backend_local\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"command\":\"evaluate\",\"arguments\":{\"expression\":\"answer\"") == NULL);
    ASSERT(strstr(stdout_text, "\"command\":\"evaluate\"") != NULL);
    ASSERT(strstr(stdout_text, "\"result\":\"42\"") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(evaluate_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(evaluate_json);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(escaped_backend_evaluate_text);
    free(escaped_backend_stack_trace_text);
    free(escaped_backend_initialize_text);
    free(backend_evaluate_text);
    free(backend_evaluate_json);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Run one minimal stackTrace -> evaluate DAP interaction and capture backend traffic. */
static void run_dap_evaluate_session(FengCodegenMapingInfo *info,
                                     const char *evaluate_expression,
                                     char **out_stdout_text,
                                     char **out_stderr_text,
                                     char **out_requests_text,
                                     int *out_rc) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x46U};
    static const char *kSourceText =
        "mod demo.pkg;\n"
        "fn main(args: string[]) {\n"
        "    let answer = 42;\n"
        "}\n";
    char template_path[] = "/tmp/feng_cli_dap_evaluate_phase5_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *escaped_evaluate_expression;
    char *initialize_json;
    char *launch_json;
    char *stack_trace_json;
    char *evaluate_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *evaluate_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *backend_stack_trace_json;
    char *backend_stack_trace_text;
    char *backend_evaluate_json;
    char *backend_evaluate_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_evaluate_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingSourceMapping sources[1];
    int rc;

    ASSERT(info != NULL);
    ASSERT(evaluate_expression != NULL);
    ASSERT(out_stdout_text != NULL);
    ASSERT(out_stderr_text != NULL);
    ASSERT(out_requests_text != NULL);
    ASSERT(out_rc != NULL);
    *out_stdout_text = NULL;
    *out_stderr_text = NULL;
    *out_requests_text = NULL;
    *out_rc = -1;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    mkdir_p(src_dir);
    source_path = path_join(src_dir, "main.ff");
    write_text_file(source_path, kSourceText);
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_binary_file(binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    sources[0].source_path = source_path;
    sources[0].package_name = "demo.pkg";
    sources[0].package_root = src_dir;
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_evaluate_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"42\",\"type\":\"int\",\"variablesReference\":0}}");
    backend_evaluate_text = build_dap_message_text(backend_evaluate_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_evaluate_text = json_escape_text(backend_evaluate_text);
    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = {\n"
                                "  initialize: \"%s\",\n"
                                "  stackTrace: \"%s\",\n"
                                "  evaluate: \"%s\"\n"
                                "};\n"
                                "let requests = '';\n"
                                "let buffer = Buffer.alloc(0);\n"
                                "process.stdout.write(responses.initialize);\n"
                                "process.stdin.on('data', chunk => {\n"
                                "  requests += chunk.toString('utf8');\n"
                                "  buffer = Buffer.concat([buffer, chunk]);\n"
                                "  for (;;) {\n"
                                "    const sep = buffer.indexOf('\\r\\n\\r\\n');\n"
                                "    if (sep < 0) break;\n"
                                "    const header = buffer.slice(0, sep).toString('utf8');\n"
                                "    const match = /Content-Length: (\\d+)/i.exec(header);\n"
                                "    if (!match) break;\n"
                                "    const length = Number(match[1]);\n"
                                "    const frameLength = sep + 4 + length;\n"
                                "    if (buffer.length < frameLength) break;\n"
                                "    const payload = buffer.slice(sep + 4, frameLength).toString('utf8');\n"
                                "    buffer = buffer.slice(frameLength);\n"
                                "    const message = JSON.parse(payload);\n"
                                "    if (message.command === 'stackTrace') process.stdout.write(responses.stackTrace);\n"
                                "    if (message.command === 'evaluate') process.stdout.write(responses.evaluate);\n"
                                "  }\n"
                                "});\n"
                                "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); });\n",
                                requests_path,
                                escaped_backend_initialize_text,
                                escaped_backend_stack_trace_text,
                                escaped_backend_evaluate_text);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    escaped_binary_path = json_escape_text(binary_path);
    escaped_evaluate_expression = json_escape_text(evaluate_expression);
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    evaluate_json = dup_printf("{\"seq\":4,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\",\"frameId\":7,\"context\":\"watch\"}}",
                               escaped_evaluate_expression);
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    evaluate_text = build_dap_message_text(evaluate_json);
    input_text = dup_printf("%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text);

    stdout_text = run_dap_interactive_capture_stdout_with_path(1,
                                                               argv,
                                                               input_text,
                                                               "\"command\":\"stackTrace\"",
                                                               evaluate_text,
                                                               path_value,
                                                               &rc,
                                                               &stderr_text);
    requests_text = read_text_file(requests_path);

    *out_stdout_text = stdout_text;
    *out_stderr_text = stderr_text;
    *out_requests_text = requests_text;
    *out_rc = rc;

    free(input_text);
    free(evaluate_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(evaluate_json);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_evaluate_expression);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script);
    free(escaped_backend_evaluate_text);
    free(escaped_backend_stack_trace_text);
    free(escaped_backend_initialize_text);
    free(backend_evaluate_text);
    free(backend_evaluate_json);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(fd_error);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Ensure Phase 5 evaluate rewrites member/index/arithmetic/comparison expressions. */
static void test_dap_rewrites_phase5_evaluate_expression(void) {
    char *stdout_text;
    char *stderr_text;
    char *requests_text;
    int rc;
    FengCodegenMapingInfo info = {0};

    feng_codegen_maping_info_init(&info);
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   NULL,
                                                                   "captured",
                                                                   "(_capture_cell->value)",
                                                                   "Captured",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_CAPTURE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "answer",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_items",
                                                                   "items",
                                                                   NULL,
                                                                   "int[]",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));

    run_dap_evaluate_session(&info,
                             "captured.member[3] + answer * 2 == items[0]",
                             &stdout_text,
                             &stderr_text,
                             &requests_text,
                             &rc);
    ASSERT(rc == 0);
    ASSERT(strstr(requests_text, "\"command\":\"evaluate\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((_capture_cell->value)).member[3] + backend_local * 2 == backend_items[0]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"command\":\"evaluate\"") != NULL);
    ASSERT(strstr(stdout_text, "\"result\":\"42\"") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    feng_codegen_maping_info_dispose(&info);
}

/* Ensure evaluate rejects non-constant index expressions before forwarding. */
static void test_dap_rejects_nonconstant_index_evaluate_expression(void) {
    char *stdout_text;
    char *stderr_text;
    char *requests_text;
    int rc;
    FengCodegenMapingInfo info = {0};

    feng_codegen_maping_info_init(&info);
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "answer",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_items",
                                                                   "items",
                                                                   NULL,
                                                                   "int[]",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));

    run_dap_evaluate_session(&info,
                             "items[answer]",
                             &stdout_text,
                             &stderr_text,
                             &requests_text,
                             &rc);
    ASSERT(rc == 0);
    ASSERT(strstr(requests_text, "\"command\":\"evaluate\"") == NULL);
    ASSERT(strstr(stdout_text, "\"success\":false") != NULL);
    ASSERT(strstr(stdout_text,
                  "evaluate index access must use an integer literal") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    feng_codegen_maping_info_dispose(&info);
}

/* Ensure evaluate rejects function calls before forwarding. */
static void test_dap_rejects_function_call_evaluate_expression(void) {
    char *stdout_text;
    char *stderr_text;
    char *requests_text;
    int rc;
    FengCodegenMapingInfo info = {0};

    feng_codegen_maping_info_init(&info);
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "answer",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));

    run_dap_evaluate_session(&info,
                             "answer()",
                             &stdout_text,
                             &stderr_text,
                             &requests_text,
                             &rc);
    ASSERT(rc == 0);
    ASSERT(strstr(requests_text, "\"command\":\"evaluate\"") == NULL);
    ASSERT(strstr(stdout_text, "\"success\":false") != NULL);
    ASSERT(strstr(stdout_text,
                  "function calls are not supported in Feng watch expressions") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    feng_codegen_maping_info_dispose(&info);
}

/* Ensure evaluate rejects assignments before forwarding. */
static void test_dap_rejects_assignment_evaluate_expression(void) {
    char *stdout_text;
    char *stderr_text;
    char *requests_text;
    int rc;
    FengCodegenMapingInfo info = {0};

    feng_codegen_maping_info_init(&info);
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "demo_pkg_main_backend",
                                              "demo.pkg.main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                                   "demo_pkg_main_backend",
                                                                   "backend_local",
                                                                   "answer",
                                                                   NULL,
                                                                   "int",
                                                                   FENG_CODEGEN_MAPING_VARIABLE_BINDING));

    run_dap_evaluate_session(&info,
                             "answer = 1",
                             &stdout_text,
                             &stderr_text,
                             &requests_text,
                             &rc);
    ASSERT(rc == 0);
    ASSERT(strstr(requests_text, "\"command\":\"evaluate\"") == NULL);
    ASSERT(strstr(stdout_text, "\"success\":false") != NULL);
    ASSERT(strstr(stdout_text,
                  "assignment is not supported in Feng watch expressions") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    feng_codegen_maping_info_dispose(&info);
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

static void test_manifest_parses_and_writes_assets(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "\n"
        "[assets]\n"
        "extlib: \"lib/\"\n"
        "fixtures: \"testdata/\"\n"
        "\n"
        "[dependencies]\n"
        "base: \"1.2.3\"\n"
        "\n"
        "[registry]\n"
        "url: \"https://packages.example.com/feng\"\n";
    char output_path[] = "/tmp/feng_manifest_assets_XXXXXX";
    int output_fd;
    char *manifest_text;
    char *write_error = NULL;
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(manifest.asset_count == 2U);
    ASSERT(strcmp(manifest.assets[0].target_dir, "extlib") == 0);
    ASSERT(strcmp(manifest.assets[0].source_path, "lib/") == 0);
    ASSERT(strcmp(manifest.assets[1].target_dir, "fixtures") == 0);
    ASSERT(strcmp(manifest.assets[1].source_path, "testdata/") == 0);

    output_fd = mkstemp(output_path);
    ASSERT(output_fd >= 0);
    close(output_fd);
    ASSERT(feng_cli_project_manifest_write(output_path, &manifest, &write_error));
    ASSERT(write_error == NULL);

    manifest_text = read_text_file(output_path);
    ASSERT(strcmp(manifest_text,
                  "[package]\n"
                  "name: \"demo\"\n"
                  "version: \"0.1.0\"\n"
                  "target: \"bin\"\n"
                  "src: \"src/\"\n"
                  "out: \"build/\"\n"
                  "\n"
                  "[dependencies]\n"
                  "base: \"1.2.3\"\n"
                  "\n"
                  "[assets]\n"
                  "extlib: \"lib/\"\n"
                  "fixtures: \"testdata/\"\n"
                  "\n"
                  "[registry]\n"
                  "url: \"https://packages.example.com/feng\"\n") == 0);

    unlink(output_path);
    free(manifest_text);
    free(write_error);
    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

static void test_manifest_rejects_empty_asset_value(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "\n"
        "[assets]\n"
        "extlib: \"\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(!feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(error.line == 7U);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "[assets].extlib") != NULL);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
}

static void test_manifest_rejects_duplicate_asset_key(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "\n"
        "[assets]\n"
        "extlib: \"lib/\"\n"
        "extlib: \"other/\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(!feng_cli_project_manifest_parse("/tmp/feng.fm", kManifest, &manifest, &error));
    ASSERT(error.line == 8U);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "duplicate") != NULL);

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

static void test_bundle_manifest_rejects_assets(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"1.0.0\"\n"
        "arch: \"macos-arm64\"\n"
        "abi: \"feng\"\n"
        "\n"
        "[assets]\n"
        "extlib: \"lib/\"\n";
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    ASSERT(!feng_cli_project_bundle_manifest_parse("/tmp/demo.fb:feng.fm",
                                                   kManifest,
                                                   &manifest,
                                                   &error));
    ASSERT(error.line == 7U);
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "[assets]") != NULL);

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

static void test_project_build_bin_copies_assets_and_refreshes_existing_output(void) {
    char template_path[] = "/tmp/feng_cli_build_bin_assets_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *asset_source_dir;
    char *asset_nested_dir;
    char *asset_source_path;
    char *asset_nested_path;
    char *copied_asset_path;
    char *copied_nested_path;
    char *binary_path;
    char *copied_text;
    char *nested_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "app");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    asset_source_dir = path_join(project_dir, "runtime_assets");
    asset_nested_dir = path_join(asset_source_dir, "nested");
    asset_source_path = path_join(asset_source_dir, "config.txt");
    asset_nested_path = path_join(asset_nested_dir, "data.txt");
    copied_asset_path = path_join(project_dir, "build/bin/runtime/config.txt");
    copied_nested_path = path_join(project_dir, "build/bin/runtime/nested/data.txt");
    binary_path = path_join(project_dir, "build/bin/asset_app");

    mkdir_p(src_dir);
    mkdir_p(asset_nested_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"asset_app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[assets]\n"
                    "runtime: \"runtime_assets/\"\n");
    write_text_file(source_path,
                    "mod test.cli.assets.bin;\n"
                    "fn main(args: string[]) {}\n");
    write_text_file(asset_source_path, "alpha\n");
    write_text_file(asset_nested_path, "nested-alpha\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    ASSERT(path_exists(copied_asset_path));
    ASSERT(path_exists(copied_nested_path));
    copied_text = read_text_file(copied_asset_path);
    nested_text = read_text_file(copied_nested_path);
    ASSERT(strcmp(copied_text, "alpha\n") == 0);
    ASSERT(strcmp(nested_text, "nested-alpha\n") == 0);
    free(nested_text);
    free(copied_text);

    write_text_file(asset_source_path, "beta\n");
    write_text_file(asset_nested_path, "nested-beta\n");
    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    copied_text = read_text_file(copied_asset_path);
    nested_text = read_text_file(copied_nested_path);
    ASSERT(strcmp(copied_text, "beta\n") == 0);
    ASSERT(strcmp(nested_text, "nested-beta\n") == 0);

    free(nested_text);
    free(copied_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(binary_path);
    free(copied_nested_path);
    free(copied_asset_path);
    free(asset_nested_path);
    free(asset_source_path);
    free(asset_nested_dir);
    free(asset_source_dir);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
}

static void test_project_build_lib_stages_assets_under_output_root(void) {
    char template_path[] = "/tmp/feng_cli_build_lib_assets_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *asset_source_dir;
    char *asset_nested_dir;
    char *asset_source_path;
    char *asset_nested_path;
    char *staged_asset_path;
    char *staged_nested_path;
    char *library_path;
    char *staged_text;
    char *nested_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "libproj");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    asset_source_dir = path_join(project_dir, "bundle_assets");
    asset_nested_dir = path_join(asset_source_dir, "nested");
    asset_source_path = path_join(asset_source_dir, "config.txt");
    asset_nested_path = path_join(asset_nested_dir, "data.txt");
    staged_asset_path = path_join(project_dir, "build/assets/runtime/config.txt");
    staged_nested_path = path_join(project_dir, "build/assets/runtime/nested/data.txt");
    {
        char *build_dir = path_join(project_dir, "build");
        library_path = host_static_library_output_path(build_dir, "asset_lib");
        free(build_dir);
    }

    mkdir_p(src_dir);
    mkdir_p(asset_nested_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"asset_lib\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[assets]\n"
                    "runtime: \"bundle_assets/\"\n");
    write_text_file(source_path,
                    "pu mod test.cli.assets.lib;\n"
                    "pu fn value(): int {\n"
                    "  return 1;\n"
                    "}\n");
    write_text_file(asset_source_path, "alpha\n");
    write_text_file(asset_nested_path, "nested-alpha\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(library_path));
    ASSERT(path_exists(staged_asset_path));
    ASSERT(path_exists(staged_nested_path));
    staged_text = read_text_file(staged_asset_path);
    nested_text = read_text_file(staged_nested_path);
    ASSERT(strcmp(staged_text, "alpha\n") == 0);
    ASSERT(strcmp(nested_text, "nested-alpha\n") == 0);
    free(nested_text);
    free(staged_text);

    write_text_file(asset_source_path, "beta\n");
    write_text_file(asset_nested_path, "nested-beta\n");
    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    staged_text = read_text_file(staged_asset_path);
    nested_text = read_text_file(staged_nested_path);
    ASSERT(strcmp(staged_text, "beta\n") == 0);
    ASSERT(strcmp(nested_text, "nested-beta\n") == 0);

    free(nested_text);
    free(staged_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(library_path);
    free(staged_nested_path);
    free(staged_asset_path);
    free(asset_nested_path);
    free(asset_source_path);
    free(asset_nested_dir);
    free(asset_source_dir);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
}

static void test_project_build_lib_stages_extlib_assets_without_assets_layer(void) {
    char template_path[] = "/tmp/feng_cli_build_lib_extlib_assets_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *asset_source_dir;
    char *asset_platform_dir;
    char *asset_source_path;
    char *staged_extlib_dir;
    char *staged_platform_dir;
    char *staged_asset_path;
    char *shadow_stage_dir;
    char *library_path;
    char *host_target = NULL;
    char *error_message = NULL;
    char *staged_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_fb_detect_host_target(&host_target, &error_message));
    free(error_message);
    error_message = NULL;

    project_dir = path_join(workspace_dir, "libproj");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    asset_source_dir = path_join(project_dir, "vendor_extlib");
    asset_platform_dir = path_join(asset_source_dir, host_target);
    asset_source_path = host_dynamic_library_path(asset_platform_dir, "helper");
    staged_extlib_dir = path_join(project_dir, "build/extlib");
    staged_platform_dir = path_join(staged_extlib_dir, host_target);
    staged_asset_path = host_dynamic_library_path(staged_platform_dir, "helper");
    shadow_stage_dir = path_join(project_dir, "build/assets/extlib");
    {
        char *build_dir = path_join(project_dir, "build");
        library_path = host_static_library_output_path(build_dir, "asset_extlib");
        free(build_dir);
    }

    mkdir_p(src_dir);
    mkdir_p(asset_platform_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"asset_extlib\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[assets]\n"
                    "extlib: \"vendor_extlib/\"\n");
    write_text_file(source_path,
                    "pu mod test.cli.assets.extlibstage;\n"
                    "pu fn value(): int {\n"
                    "  return 5;\n"
                    "}\n");
    write_text_file(asset_source_path, "alpha\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(library_path));
    ASSERT(path_exists(staged_asset_path));
    ASSERT(!path_exists(shadow_stage_dir));
    staged_text = read_text_file(staged_asset_path);
    ASSERT(strcmp(staged_text, "alpha\n") == 0);
    free(staged_text);

    write_text_file(asset_source_path, "beta\n");
    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(staged_asset_path));
    ASSERT(!path_exists(shadow_stage_dir));
    staged_text = read_text_file(staged_asset_path);
    ASSERT(strcmp(staged_text, "beta\n") == 0);

    free(staged_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(host_target);
    free(library_path);
    free(shadow_stage_dir);
    free(staged_asset_path);
    free(staged_platform_dir);
    free(staged_extlib_dir);
    free(asset_source_path);
    free(asset_platform_dir);
    free(asset_source_dir);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
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

static void test_project_pack_includes_staged_assets_in_bundle(void) {
    char template_path[] = "/tmp/feng_cli_pack_assets_bundle_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *asset_dir;
    char *asset_nested_dir;
    char *asset_config_path;
    char *asset_nested_path;
    char *bundle_path;
    char *remove_error = NULL;
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *bytes = NULL;
    size_t byte_count = 0U;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "libproj");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    asset_dir = path_join(project_dir, "bundle_assets");
    asset_nested_dir = path_join(asset_dir, "nested");
    asset_config_path = path_join(asset_dir, "config.txt");
    asset_nested_path = path_join(asset_nested_dir, "value.txt");
    bundle_path = path_join(project_dir, "build/asset_pack-0.1.0.fb");

    mkdir_p(src_dir);
    mkdir_p(asset_nested_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"asset_pack\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[assets]\n"
                    "runtime: \"bundle_assets/\"\n");
    write_text_file(source_path,
                    "pu mod test.cli.assetpack;\n"
                    "pu fn value(): int {\n"
                    "  return 3;\n"
                    "}\n");
    write_text_file(asset_config_path, "config\n");
    write_text_file(asset_nested_path, "nested\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(bundle_path));
    ASSERT(feng_zip_reader_open(bundle_path, &reader, &zip_error));
    ASSERT(feng_zip_reader_read(&reader,
                                "runtime/config.txt",
                                &bytes,
                                &byte_count,
                                &zip_error));
    ASSERT(memcmp(bytes, "config\n", byte_count) == 0);
    feng_zip_free(bytes);
    bytes = NULL;

    ASSERT(feng_zip_reader_read(&reader,
                                "runtime/nested/value.txt",
                                &bytes,
                                &byte_count,
                                &zip_error));
    ASSERT(memcmp(bytes, "nested\n", byte_count) == 0);
    feng_zip_free(bytes);
    bytes = NULL;

    ASSERT(!zip_contains_path_prefix(&reader, "extlib"));

    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundle_path);
    free(asset_nested_path);
    free(asset_config_path);
    free(asset_nested_dir);
    free(asset_dir);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
}

static void test_project_pack_includes_extlib_assets_without_assets_layer(void) {
    char template_path[] = "/tmp/feng_cli_pack_extlib_assets_bundle_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *asset_source_dir;
    char *asset_platform_dir;
    char *asset_source_path;
    char *shadow_stage_dir;
    char *bundle_path;
    char *host_target = NULL;
    char *error_message = NULL;
    char *remove_error = NULL;
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *bytes = NULL;
    size_t byte_count = 0U;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_fb_detect_host_target(&host_target, &error_message));
    free(error_message);
    error_message = NULL;

    project_dir = path_join(workspace_dir, "libproj");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    asset_source_dir = path_join(project_dir, "vendor_extlib");
    asset_platform_dir = path_join(asset_source_dir, host_target);
    asset_source_path = host_dynamic_library_path(asset_platform_dir, "helper");
    shadow_stage_dir = path_join(project_dir, "build/assets/extlib");
    bundle_path = path_join(project_dir, "build/asset_extlib_pack-0.1.0.fb");

    mkdir_p(src_dir);
    mkdir_p(asset_platform_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"asset_extlib_pack\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[assets]\n"
                    "extlib: \"vendor_extlib/\"\n");
    write_text_file(source_path,
                    "pu mod test.cli.assetextlibpack;\n"
                    "pu fn value(): int {\n"
                    "  return 9;\n"
                    "}\n");
    write_text_file(asset_source_path, "dynamic\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(bundle_path));
    ASSERT(!path_exists(shadow_stage_dir));
    ASSERT(feng_zip_reader_open(bundle_path, &reader, &zip_error));

    {
        char *entry_path = host_bundle_extlib_dynamic_entry_path(host_target, "helper");
        ASSERT(entry_path != NULL);
        ASSERT(feng_zip_reader_read(&reader,
                                    entry_path,
                                    &bytes,
                                    &byte_count,
                                    &zip_error));
        ASSERT(memcmp(bytes, "dynamic\n", byte_count) == 0);
        free(entry_path);
        feng_zip_free(bytes);
        bytes = NULL;
    }

    ASSERT(!zip_contains_path_prefix(&reader, "assets"));

    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(host_target);
    free(bundle_path);
    free(shadow_stage_dir);
    free(asset_source_path);
    free(asset_platform_dir);
    free(asset_source_dir);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
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
    test_manifest_parses_and_writes_assets();
    test_manifest_rejects_empty_asset_value();
    test_manifest_rejects_duplicate_asset_key();
    test_manifest_rejects_duplicate_field();
    test_project_open_collects_sources();
    test_manifest_requires_target();
    test_bundle_manifest_allows_dependencies_without_target();
    test_bundle_manifest_rejects_local_path_dependency();
    test_bundle_manifest_rejects_assets();
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
    test_project_build_help_writes_stdout_and_returns_success();
    test_project_pack_help_writes_stdout_and_returns_success();
    test_lsp_help_returns_success();
    test_lsp_help_writes_stdout_not_stderr();
    test_lsp_rejects_unknown_option();
    test_lsp_unknown_option_stays_on_stderr();
    test_dap_help_returns_success();
    test_dap_help_writes_stdout_not_stderr();
    test_dap_rejects_unknown_option();
    test_deps_add_help_writes_stdout_and_returns_success();
    test_dap_validated_launch_starts_backend();
    test_dap_resolves_backend_via_xcrun_when_path_misses();
    test_dap_rewrites_set_breakpoints_source_path_to_package_uri();
    test_dap_rejects_set_breakpoints_outside_debug_closure();
    test_dap_rewrites_stack_trace_source_path_to_local_path();
    test_dap_rewrites_stack_trace_compiler_normalized_source_path();
    test_dap_hides_hidden_stack_trace_frames();
    test_dap_rewrites_variables_to_feng_names();
    test_dap_filters_backend_variables_and_rewrites_user_values();
    test_dap_uses_array_element_type_name_in_value_summary();
    test_project_build_keeps_for_body_breakpoint_after_init_in_dwarf();
    test_dap_rewrites_identifier_evaluate_expression();
    test_dap_rewrites_phase5_evaluate_expression();
    test_dap_rejects_nonconstant_index_evaluate_expression();
    test_dap_rejects_function_call_evaluate_expression();
    test_dap_rejects_assignment_evaluate_expression();
    test_dap_rejects_fingerprint_mismatch_before_backend_spawn();
    test_dap_reports_missing_backend_after_launch_validation();
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
    test_bundle_writer_includes_extlib_and_assets_without_empty_dirs();
    test_direct_build_releases_bundle_extlib_dynamic_libraries_only();
    test_direct_build_links_only_used_bundle_extlib_static_libraries();
    test_direct_build_maps_cli_library_name_to_link_flag();
    test_direct_build_passes_cli_library_path_verbatim();
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
    test_project_build_bin_copies_assets_and_refreshes_existing_output();
    test_project_build_lib_stages_assets_under_output_root();
    test_project_build_lib_stages_extlib_assets_without_assets_layer();
    test_project_run_release_reuses_build_pipeline();
    test_project_pack_uses_release_build_and_public_ft_excludes_spans();
    test_project_pack_includes_staged_assets_in_bundle();
    test_project_pack_includes_extlib_assets_without_assets_layer();
    test_project_pack_rejects_release_flag();
    fprintf(stdout, "cli tests passed\n");
    return 0;
}
