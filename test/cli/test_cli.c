#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "archive/fb.h"
#include "platform/platform.h"
#include "archive/zip.h"
#include "cli/cli.h"
#include "cli/common.h"
#include "cli/compile/options.h"
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

/* Copy one binary test fixture without changing its contents. */
static void copy_file_or_die(const char *source_path, const char *destination_path) {
    FILE *source = fopen(source_path, "rb");
    FILE *destination;
    char buffer[8192];
    size_t read_size;

    ASSERT(source != NULL);
    destination = fopen(destination_path, "wb");
    ASSERT(destination != NULL);
    while ((read_size = fread(buffer, 1U, sizeof(buffer), source)) > 0U) {
        ASSERT(fwrite(buffer, 1U, read_size, destination) == read_size);
    }
    ASSERT(!ferror(source));
    ASSERT(fclose(destination) == 0);
    ASSERT(fclose(source) == 0);
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

/* Count exact command-line arguments in the one-argument-per-line compiler log. */
static int count_logged_arguments(const char *text, const char *argument) {
    int count = 0;
    size_t argument_len = strlen(argument);
    const char *line = text;

    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        size_t line_len = line_end != NULL ? (size_t)(line_end - line) : strlen(line);

        if (line_len == argument_len && memcmp(line, argument, argument_len) == 0) {
            count += 1;
        }
        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
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
    char *name = feng_platform_static_library_file_name(stem);

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
    char *lib_dir;
    char *path;

    lib_dir = path_join(out_dir, "lib");
    path = host_static_library_path(lib_dir, stem);
    free(lib_dir);
    return path;
}

/* Compose one project output path below build/<platform>/. */
static char *project_platform_build_path(const char *project_dir,
                                         const char *platform,
                                         const char *relative_path) {
    char *build_root;
    char *platform_root;
    char *path;

    build_root = path_join(project_dir, "build");
    platform_root = path_join(build_root, platform);
    path = path_join(platform_root, relative_path);
    free(platform_root);
    free(build_root);
    return path;
}

/* Compose one project output path below build/<host-platform>/. */
static char *project_host_build_path(const char *project_dir,
                                     const char *relative_path) {
    char *host_platform = NULL;
    char *path;

    ASSERT(feng_platform_detect_host_platform(&host_platform, NULL));
    path = project_platform_build_path(
        project_dir,
        host_platform,
        relative_path);
    free(host_platform);
    return path;
}

/* Ensure native platform detection includes the Linux ABI and target triple. */
static void test_platform_detects_complete_native_platform(void) {
    char *host_platform = NULL;
    char *error_message = NULL;
    char *dynamic_name = NULL;

    ASSERT(feng_platform_detect_host_platform(&host_platform, &error_message));
    ASSERT(error_message == NULL);
#if defined(__APPLE__)
    ASSERT(strcmp(host_platform, "macos-arm64") == 0);
    ASSERT(strcmp(feng_platform_clang_target(host_platform),
                  "arm64-apple-macosx") == 0);
#elif defined(__linux__) && defined(__aarch64__)
    ASSERT(strcmp(host_platform, "linux-arm64-gnu") == 0);
    ASSERT(strcmp(feng_platform_clang_target(host_platform),
                  "aarch64-unknown-linux-gnu") == 0);
#elif defined(__linux__) && defined(__x86_64__)
    ASSERT(strcmp(host_platform, "linux-x64-gnu") == 0);
    ASSERT(strcmp(feng_platform_clang_target(host_platform),
                  "x86_64-unknown-linux-gnu") == 0);
#endif
    ASSERT(feng_platform_clang_target("unsupported-platform") == NULL);
    ASSERT(strcmp(feng_platform_dynamic_library_suffix("macos-arm64"),
                  ".dylib") == 0);
    ASSERT(strcmp(feng_platform_dynamic_library_suffix("linux-x64-musl"),
                  ".so") == 0);
    dynamic_name = feng_platform_dynamic_library_file_name(
        "linux-arm64-gnu",
        "helper");
    ASSERT(dynamic_name != NULL);
    ASSERT(strcmp(dynamic_name, "libhelper.so") == 0);
    free(dynamic_name);
    free(error_message);
    free(host_platform);
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

/* Invoke direct compilation for the detected host platform. */
static int run_direct_for_host(int argc, char **argv) {
    char *host_platform = NULL;
    char *platform_option = NULL;
    char **full_argv = NULL;
    int index;
    int rc;

    ASSERT(feng_platform_detect_host_platform(&host_platform, NULL));
    platform_option = dup_printf("--platform=%s", host_platform);
    full_argv = (char **)calloc((size_t)argc + 1U, sizeof(*full_argv));
    ASSERT(platform_option != NULL);
    ASSERT(full_argv != NULL);
    for (index = 0; index < argc; ++index) {
        full_argv[index] = argv[index];
    }
    full_argv[argc] = platform_option;
    rc = feng_cli_direct_main("feng", argc + 1, full_argv);
    free(full_argv);
    free(platform_option);
    free(host_platform);
    return rc;
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

/* Write a manifest-only package with one marker used to identify its source. */
static void write_marked_manifest_bundle_or_die(const char *bundle_path,
                                                const char *manifest_text,
                                                const char *marker_text) {
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
    ASSERT(feng_zip_writer_add_bytes(&writer,
                                     "source-marker.txt",
                                     marker_text,
                                     strlen(marker_text),
                                     FENG_ZIP_COMPRESSION_DEFLATE,
                                     &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_writer_finalize(&writer, &error_message));
    free(error_message);
    feng_zip_writer_dispose(&writer);
}

/* Assert that one installed package contains the expected source marker. */
static void assert_bundle_source_marker(const char *bundle_path,
                                        const char *expected_marker) {
    FengZipReader reader = {0};
    char *error_message = NULL;
    void *marker_bytes = NULL;
    size_t marker_size = 0U;

    ASSERT(feng_zip_reader_open(bundle_path, &reader, &error_message));
    free(error_message);
    error_message = NULL;
    ASSERT(feng_zip_reader_read(&reader,
                                "source-marker.txt",
                                &marker_bytes,
                                &marker_size,
                                &error_message));
    free(error_message);
    ASSERT(marker_size == strlen(expected_marker));
    ASSERT(memcmp(marker_bytes, expected_marker, marker_size) == 0);
    feng_zip_free(marker_bytes);
    feng_zip_reader_dispose(&reader);
}

/* Resolve one package path below the running test binary's Feng install root. */
static char *bundled_package_path(const char *bundle_name) {
    char *relative_path = dup_printf("pkg/%s", bundle_name);
    char *error_message = NULL;
    char *resolved_path = feng_cli_resolve_install_path("feng",
                                                        relative_path,
                                                        &error_message);

    free(relative_path);
    ASSERT(resolved_path != NULL);
    ASSERT(error_message == NULL);
    return resolved_path;
}

static void write_library_bundle_or_die(const char *bundle_path,
                                        const char *package_name,
                                        const char *package_version,
                                        const char *library_path,
                                        const char *public_mod_root) {
    FengFbLibraryBundleSpec spec = {0};
    FengFbBundlePlatformArtifact artifact = {0};
    char *host_platform = NULL;
    char *error_message = NULL;

    ASSERT(feng_platform_detect_host_platform(&host_platform, &error_message));
    free(error_message);
    error_message = NULL;
    spec.package_path = bundle_path;
    spec.package_name = package_name;
    spec.package_version = package_version;
    artifact.platform = host_platform;
    artifact.library_path = library_path;
    spec.platform_artifacts = &artifact;
    spec.platform_artifact_count = 1U;
    spec.public_mod_root = public_mod_root;

    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
    free(host_platform);
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

#if defined(__APPLE__)
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
            cursor = strchr(cursor, '\n');
            continue;
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
#endif

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
        ASSERT(run_direct_for_host(4, argv) == 0);
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
        ASSERT(run_direct_for_host(5, argv) == 0);
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
    char template_path[] = "temp/feng_cli_run_output_XXXXXX";
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

    rc = run_direct_for_host(argc, argv);

    fflush(stderr);
    ASSERT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    return rc;
}

/* Verify direct mode defaults to host/build and preserves explicit target inputs. */
static void test_direct_options_default_host_and_out_and_accept_sysroot(void) {
    FengCliDirectOptions options = {0};
    char *host_platform = NULL;
    char *defaults[] = {
        "main.ff",
    };
    char *valid[] = {
        "main.ff",
        "--target=lib",
        "--platform=linux-x64-musl",
        "--sysroot=/sdk/linux-x64-musl",
        "--out=build",
    };
    char *duplicate_platform[] = {
        "main.ff",
        "--platform=macos-arm64",
        "--platform=linux-x64-gnu",
        "--out=build",
    };

    ASSERT(feng_platform_detect_host_platform(&host_platform, NULL));
    ASSERT(feng_cli_direct_options_parse(
               "feng",
               1,
               defaults,
               &options) == FENG_CLI_PARSE_OK);
    ASSERT(strcmp(options.platform, host_platform) == 0);
    ASSERT(strcmp(options.out_dir, "./build") == 0);
    feng_cli_direct_options_dispose(&options);
    free(host_platform);
    ASSERT(feng_cli_direct_options_parse(
               "feng",
               5,
               valid,
               &options) == FENG_CLI_PARSE_OK);
    ASSERT(options.target == FENG_COMPILE_TARGET_LIB);
    ASSERT(strcmp(options.platform, "linux-x64-musl") == 0);
    ASSERT(strcmp(options.sysroot, "/sdk/linux-x64-musl") == 0);
    ASSERT(strcmp(options.out_dir, "build") == 0);
    feng_cli_direct_options_dispose(&options);
    ASSERT(feng_cli_direct_options_parse(
               "feng",
               4,
               duplicate_platform,
               &options) == FENG_CLI_PARSE_ERROR);
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
static FILE *temp_file(void);

static char *run_cli_entry_capture_stdout_and_stderr(CliEntryFn entry,
                                                     int argc,
                                                     char **argv,
                                                     int *out_rc,
                                                     char **out_stderr) {
    int saved_stdout;
    int saved_stderr;
    FILE *output = temp_file();
    FILE *errors = temp_file();
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

/* Run `feng dap` with redirected stdio and temporary backend lookup overrides. */
static char *run_dap_capture_stdout_with_path(int argc,
                                              char **argv,
                                              const char *input_text,
                                              const char *path_value,
                                              const char *lldb_dap_value,
                                              int *out_rc,
                                              char **out_stderr) {
    int input_pipe[2];
    int output_pipe[2];
    int saved_stdin;
    int saved_stdout;
    int saved_stderr;
    FILE *errors = temp_file();
    const char *existing_path = getenv("PATH");
    char *saved_path = existing_path != NULL ? dup_cstr(existing_path) : NULL;
    const char *existing_lldb_dap = getenv("FENG_LLDB_DAP");
    char *saved_lldb_dap = existing_lldb_dap != NULL ? dup_cstr(existing_lldb_dap) : NULL;
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
    if (lldb_dap_value != NULL) {
        ASSERT(setenv("FENG_LLDB_DAP", lldb_dap_value, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_LLDB_DAP") == 0);
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
    if (saved_lldb_dap != NULL) {
        ASSERT(setenv("FENG_LLDB_DAP", saved_lldb_dap, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_LLDB_DAP") == 0);
    }
    free(saved_lldb_dap);

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
                                                          const char *lldb_dap_value,
                                                          int *out_rc,
                                                          char **out_stderr) {
    int input_pipe[2];
    int output_pipe[2];
    int error_pipe[2];
    const char *existing_path = getenv("PATH");
    char *saved_path = existing_path != NULL ? dup_cstr(existing_path) : NULL;
    const char *existing_lldb_dap = getenv("FENG_LLDB_DAP");
    char *saved_lldb_dap = existing_lldb_dap != NULL ? dup_cstr(existing_lldb_dap) : NULL;
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
    if (lldb_dap_value != NULL) {
        ASSERT(setenv("FENG_LLDB_DAP", lldb_dap_value, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_LLDB_DAP") == 0);
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
    if (saved_lldb_dap != NULL) {
        ASSERT(setenv("FENG_LLDB_DAP", saved_lldb_dap, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_LLDB_DAP") == 0);
    }
    free(saved_lldb_dap);

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

static char *run_dap_two_step_interactive_capture_stdout_with_path(int argc,
                                                                   char **argv,
                                                                   const char *initial_input,
                                                                   const char *first_wait_text,
                                                                   const char *first_followup_input,
                                                                   const char *second_wait_text,
                                                                   const char *second_followup_input,
                                                                   const char *path_value,
                                                                   const char *lldb_dap_value,
                                                                   int *out_rc,
                                                                   char **out_stderr) {
    int input_pipe[2];
    int output_pipe[2];
    int error_pipe[2];
    const char *existing_path = getenv("PATH");
    char *saved_path = existing_path != NULL ? dup_cstr(existing_path) : NULL;
    const char *existing_lldb_dap = getenv("FENG_LLDB_DAP");
    char *saved_lldb_dap = existing_lldb_dap != NULL ? dup_cstr(existing_lldb_dap) : NULL;
    size_t initial_length = initial_input != NULL ? strlen(initial_input) : 0U;
    size_t first_followup_length = first_followup_input != NULL ? strlen(first_followup_input) : 0U;
    size_t second_followup_length = second_followup_input != NULL ? strlen(second_followup_input) : 0U;
    char *captured_first;
    char *captured_second;
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
    if (lldb_dap_value != NULL) {
        ASSERT(setenv("FENG_LLDB_DAP", lldb_dap_value, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_LLDB_DAP") == 0);
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
    captured_first = first_wait_text != NULL
                         ? read_fd_until_contains(output_pipe[0], first_wait_text)
                         : dup_cstr("");
    if (first_followup_length > 0U) {
        ASSERT(write(input_pipe[1], first_followup_input, first_followup_length) ==
               (ssize_t)first_followup_length);
    }
    captured_second = second_wait_text != NULL
                          ? read_fd_until_contains(output_pipe[0], second_wait_text)
                          : dup_cstr("");
    if (second_followup_length > 0U) {
        ASSERT(write(input_pipe[1], second_followup_input, second_followup_length) ==
               (ssize_t)second_followup_length);
    }
    close(input_pipe[1]);

    captured_suffix = read_fd_to_string(output_pipe[0]);
    close(output_pipe[0]);
    captured_stdout = concat_owned_strings(captured_first, captured_second);
    captured_stdout = concat_owned_strings(captured_stdout, captured_suffix);
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
    if (saved_lldb_dap != NULL) {
        ASSERT(setenv("FENG_LLDB_DAP", saved_lldb_dap, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_LLDB_DAP") == 0);
    }
    free(saved_lldb_dap);

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
    FILE *errors = temp_file();
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
    FILE *errors = temp_file();
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
    char input_template[] = "temp/feng_lsp_input_XXXXXX";
    int input_fd;
    char *input_text;
    FILE *named_input;
    FILE *output = temp_file();
    FILE *errors = temp_file();
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

/* Runs one LSP session after a position request proves that its asynchronous
 * index dependency is observable through the protocol. Unsuccessful probes
 * wait briefly so the asynchronous index can progress under cold or loaded
 * test environments. */
static char *run_lsp_server_capture_after_position_ready(
    const char *initialize,
    const char *did_open,
    const char *after_open,
    const char *method,
    const char *uri,
    unsigned int line,
    unsigned int character,
    const char *ready_text,
    const char *const *requests,
    size_t request_count,
    char **out_ready_output) {
    enum {
        MAX_READY_PROBES = 200,
        READY_PROBE_DELAY_US = 25U * 1000U
    };
    int input_pipe[2];
    int output_pipe[2];
    FILE *errors = temp_file();
    FILE *input;
    pid_t child;
    size_t probe_index;
    size_t request_index;
    char *readiness_output;
    char *captured;
    char *captured_errors;
    bool ready;
    int status;

    if (out_ready_output != NULL) {
        *out_ready_output = NULL;
    }
    ASSERT(errors != NULL);
    ASSERT(pipe(input_pipe) == 0);
    ASSERT(pipe(output_pipe) == 0);
    child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        FILE *server_input;
        FILE *server_output;
        int rc;

        close(input_pipe[1]);
        close(output_pipe[0]);
        server_input = fdopen(input_pipe[0], "rb");
        server_output = fdopen(output_pipe[1], "wb");
        ASSERT(server_input != NULL);
        ASSERT(server_output != NULL);
        rc = feng_lsp_server_run(server_input, server_output, errors);
        fclose(server_input);
        fclose(server_output);
        fclose(errors);
        _exit(rc);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    input = fdopen(input_pipe[1], "wb");
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    if (after_open != NULL) {
        write_lsp_message(input, after_open);
    }
    ready = false;
    for (probe_index = 0U; probe_index < MAX_READY_PROBES; ++probe_index) {
        unsigned int probe_id = 1000U + (unsigned int)probe_index;
        unsigned int barrier_id = 2000U + (unsigned int)probe_index;
        char *probe = dup_printf(
            "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"%s\","
            "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
            "\"position\":{\"line\":%u,\"character\":%u}}}",
            probe_id,
            method,
            uri,
            line,
            character);
        char *barrier = dup_printf(
            "{\"jsonrpc\":\"2.0\",\"id\":%u,"
            "\"method\":\"feng/testReadinessBarrier\",\"params\":null}",
            barrier_id);
        char *barrier_response = dup_printf(
            "\"id\":%u,\"error\":{\"code\":-32601,"
            "\"message\":\"Method not found\"}}",
            barrier_id);

        write_lsp_message(input, probe);
        write_lsp_message(input, barrier);
        free(probe);
        free(barrier);
        ASSERT(fflush(input) == 0);
        readiness_output = read_fd_until_contains(output_pipe[0], barrier_response);
        free(barrier_response);
        ready = strstr(readiness_output, ready_text) != NULL;
        if (ready && out_ready_output != NULL) {
            *out_ready_output = readiness_output;
            readiness_output = NULL;
        }
        free(readiness_output);
        if (ready) {
            break;
        }
        if (probe_index + 1U < MAX_READY_PROBES) {
            (void)usleep(READY_PROBE_DELAY_US);
        }
    }

    for (request_index = 0U; request_index < request_count; ++request_index) {
        write_lsp_message(input, requests[request_index]);
    }
    ASSERT(fflush(input) == 0);
    fclose(input);
    captured = read_fd_to_string(output_pipe[0]);
    close(output_pipe[0]);
    ASSERT(waitpid(child, &status, 0) == child);
    captured_errors = read_text_stream(errors);
    fclose(errors);
    if (!ready || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s", captured_errors);
    }
    free(captured_errors);
    ASSERT(ready);
    ASSERT(WIFEXITED(status));
    ASSERT(WEXITSTATUS(status) == 0);
    return captured;
}

/* Runs one asserted position request after its matching readiness probe. */
static char *run_lsp_single_position_response_after_ready(
    const char *initialize,
    const char *did_open,
    const char *method,
    const char *uri,
    unsigned int line,
    unsigned int character,
    const char *ready_text,
    const char *request,
    const char *shutdown) {
    const char *requests[] = {
        request,
        shutdown,
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"
    };

    return run_lsp_server_capture_after_position_ready(initialize,
                                                        did_open,
                                                        NULL,
                                                        method,
                                                        uri,
                                                        line,
                                                        character,
                                                        ready_text,
                                                        requests,
                                                        3U,
                                                        NULL);
}

static char *file_uri_from_path(const char *path) {
    char resolved[PATH_MAX];

    if (path[0] != '/' && realpath(path, resolved) != NULL) {
        return dup_printf("file://%s", resolved);
    }
    return dup_printf("file://%s", path);
}

static FILE *temp_file(void) {
    char path[] = "temp/feng_tmpfile_XXXXXX";
    int fd = mkstemp(path);
    FILE *file;

    if (fd < 0) return NULL;
    (void)unlink(path);
    file = fdopen(fd, "w+b");
    if (file == NULL) { close(fd); return NULL; }
    return file;
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
    char template_path[] = "temp/feng_cli_direct_ir_XXXXXX";
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
                    "module test.cli.good;\n"
                    "func main(args: string[]) {}\n");

    {
        char *argv[] = {
            good_path,
            "--target=bin",
            out_dir,
            "--name=demo",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(run_direct_for_host(4, argv) == 0);
        ASSERT(!path_exists(c_path));
        free(out_opt);
    }

    write_text_file(good_path,
                    "module test.cli.good;\n"
                    "func main(args: string[]) {\n");

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
                    "module test.cli.keep;\n"
                    "func main(args: string[]) {}\n");

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
        ASSERT(run_direct_for_host(5, argv) == 0);
        ASSERT(path_exists(c_path));
        free(out_opt);
    }

    write_text_file(bad_path,
                    "module test.cli.keep;\n"
                    "func main(args: string[]) {\n");

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
    char template_path[] = "temp/feng_cli_direct_symbols_XXXXXX";
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
                    "open module test.cli.symbols;\n"
                    "open func value(): int {\n"
                    "  return 1;\n"
                    "}\n"
                    "func main(args: string[]) {}\n");

    {
        char *argv[] = {
            source_path,
            "--target=bin",
            out_dir,
            "--name=symbols",
        };
        char *out_opt = make_out_option(out_dir);
        argv[2] = out_opt;
        ASSERT(run_direct_for_host(4, argv) == 0);
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
    char template_path[] = "temp/feng_cli_direct_pkg_XXXXXX";
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
                    "open module test.cli.pkgdep;\n"
                    "open func dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(main_source_path,
                    "module test.cli.pkgmain;\n"
                    "import test.cli.pkgdep;\n"
                    "func main(args: string[]) {}\n");

    {
        char *out_opt = make_out_option(dep_out_dir);
        char *argv[] = {
            dep_source_path,
            "--target=lib",
            out_opt,
            "--name=dep",
        };
        ASSERT(run_direct_for_host(4, argv) == 0);
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
        ASSERT(run_direct_for_host(5, argv) == 0);
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
    char template_path[] = "temp/feng_cli_direct_pkg_link_XXXXXX";
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
                    "open module test.cli.pkgdep;\n"
                    "open func dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n");
    write_text_file(main_source_path,
                    "module test.cli.pkgmain;\n"
                    "import test.cli.pkgdep;\n"
                    "@cdecl(\"libc\")\n"
                    "extern func puts(msg: string*): int;\n"
                    "func main(args: string[]) {\n"
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
        ASSERT(run_direct_for_host(4, argv) == 0);
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
        ASSERT(run_direct_for_host(5, argv) == 0);
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
    char template_path[] = "temp/feng_cli_direct_pkg_sort_XXXXXX";
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
                    "open module test.cli.pkgb;\n"
                    "open func b_value(): int {\n"
                    "  return 11;\n"
                    "}\n");
    write_text_file(a_source_path,
                    "open module test.cli.pkga;\n"
                    "import test.cli.pkgb as b;\n"
                    "open func a_value(): int {\n"
                    "  return b.b_value();\n"
                    "}\n");
    write_text_file(main_source_path,
                    "module test.cli.pkgconsumer;\n"
                    "import test.cli.pkga as a;\n"
                    "@cdecl(\"libc\")\n"
                    "extern func puts(msg: string*): int;\n"
                    "func main(args: string[]) {\n"
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
        ASSERT(run_direct_for_host(4, argv) == 0);
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
        ASSERT(run_direct_for_host(5, argv) == 0);
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
        ASSERT(run_direct_for_host(6, argv) == 0);
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
    char template_path[] = "temp/feng_cli_pack_consume_XXXXXX";
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
    bundle_path = path_join(lib_project_dir, "build/pkg/pkgpack-0.1.0.fb");
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
                    "open module test.cli.packdep;\n"
                    "open func dep_value(): int {\n"
                    "  return 9;\n"
                    "}\n");
    write_text_file(consumer_source_path,
                    "module test.cli.packconsumer;\n"
                    "import test.cli.packdep;\n"
                    "@cdecl(\"libc\")\n"
                    "extern func puts(msg: string*): int;\n"
                    "func main(args: string[]) {\n"
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
        ASSERT(run_direct_for_host(5, argv) == 0);
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
    char template_path[] = "temp/feng_fb_bundle_assets_extlib_XXXXXX";
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
    FengFbBundlePlatformArtifact platform_artifact = {0};
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
    ASSERT(feng_platform_detect_host_platform(&host_target, &error_message));
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
    platform_artifact.platform = host_target;
    platform_artifact.library_path = library_path;
    platform_artifact.extlib_root = extlib_platform_dir;
    spec.platform_artifacts = &platform_artifact;
    spec.platform_artifact_count = 1U;
    spec.public_mod_root = mod_dir;
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
    char template_path[] = "temp/feng_cli_direct_pkg_extlib_release_XXXXXX";
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
    FengFbBundlePlatformArtifact platform_artifact = {0};
    FengFbLibraryBundleSpec spec = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_platform_detect_host_platform(&host_target, &error_message));
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
                    "open module test.cli.pkgextlib;\n"
                    "@fastcall(\"helper\")\n"
                    "open extern func helper_value(): int;\n");
    {
        char *out_opt = make_out_option(dep_out_dir);
        char *name_opt = dup_printf("--name=%s", "pkgextlib");
        char *argv[] = {
            dep_source_path,
            "--target=lib",
            out_opt,
            name_opt,
        };
        ASSERT(run_direct_for_host(4, argv) == 0);
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
    platform_artifact.platform = host_target;
    platform_artifact.library_path = dep_library_path;
    platform_artifact.extlib_root = extlib_platform_dir;
    spec.platform_artifacts = &platform_artifact;
    spec.platform_artifact_count = 1U;
    spec.public_mod_root = dep_mod_root;
    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
    error_message = NULL;

    mkdir_p(consumer_src_dir);
    mkdir_p(consumer_bin_dir);
    write_text_file(consumer_source_path,
                    "module test.cli.pkgextlibmain;\n"
                    "import test.cli.pkgextlib;\n"
                    "@cdecl(\"libc\")\n"
                    "extern func puts(msg: string*): int;\n"
                    "func main(args: string[]) {\n"
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
        ASSERT(run_direct_for_host(5, argv) == 0);
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
    char template_path[] = "temp/feng_cli_direct_pkg_extlib_static_used_only_XXXXXX";
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
    FengFbBundlePlatformArtifact platform_artifact = {0};
    FengFbLibraryBundleSpec spec = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_platform_detect_host_platform(&host_target, &error_message));
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
                    "open module test.cli.pkgextlibstatic;\n"
                    "@stdcall(\"helper\")\n"
                    "open extern func helper_value(): int;\n");
    {
        char *out_opt = make_out_option(dep_out_dir);
        char *name_opt = dup_printf("--name=%s", "pkgextlibstatic");
        char *argv[] = {
            dep_source_path,
            "--target=lib",
            out_opt,
            name_opt,
        };
        ASSERT(run_direct_for_host(4, argv) == 0);
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
    platform_artifact.platform = host_target;
    platform_artifact.library_path = dep_library_path;
    platform_artifact.extlib_root = extlib_platform_dir;
    spec.platform_artifacts = &platform_artifact;
    spec.platform_artifact_count = 1U;
    spec.public_mod_root = dep_mod_root;
    ASSERT(feng_fb_write_library_bundle(&spec, &error_message));
    free(error_message);
    error_message = NULL;

    mkdir_p(consumer_src_dir);
    write_text_file(consumer_source_path,
                    "module test.cli.pkgextlibstaticconsumer;\n"
                    "import test.cli.pkgextlibstatic;\n"
                    "@cdecl(\"libc\")\n"
                    "extern func puts(msg: string*): int;\n"
                    "func main(args: string[]) {\n"
                    "  if helper_value() == 41 {\n"
                    "    puts(&\"ok\");\n"
                    "  } else {\n"
                    "    puts(&\"bad\");\n"
                    "  }\n"
                    "}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);
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
        ASSERT(run_direct_for_host(5, argv) == 0);
        free(pkg_opt);
        free(out_opt);
    }
    if (saved_cc != NULL) {
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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
    char template_path[] = "temp/feng_cli_direct_cli_lib_name_XXXXXX";
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
                    "module test.cli.directclilibname;\n"
                    "func main(args: string[]) {}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);

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
        ASSERT(run_direct_for_host(5, argv) == 0);
    }

    if (saved_cc != NULL) {
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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
    char template_path[] = "temp/feng_cli_direct_cli_lib_path_XXXXXX";
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
                    "module test.cli.directclilibpath;\n"
                    "func main(args: string[]) {}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);

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
        ASSERT(run_direct_for_host(6, argv) == 0);
    }

    if (saved_cc != NULL) {
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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
    char template_path[] = "temp/feng_cli_pkg_generic_fn_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkggenericfn",
        "open module test.cli.pkggenericfn;\n"
        "open func identity<T>(value: T): T {\n"
        "  return value;\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkggenericfnmain;\n"
        "import test.cli.pkggenericfn;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "func main(args: string[]) {\n"
        "  if identity(7) == 7 { puts(&\"generic fn ok\"); }\n"
        "}\n",
        "generic_fn_main",
        "generic fn ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_direct_build_consumes_package_generic_type(void) {
    char template_path[] = "temp/feng_cli_pkg_generic_type_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkggenerictype",
        "open module test.cli.pkggenerictype;\n"
        "open type Box<T> {\n"
        "  var value: T;\n"
        "  open func setValue(next: T) {\n"
        "    self.value = next;\n"
        "  }\n"
        "  open func readValue(): T {\n"
        "    return self.value;\n"
        "  }\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkggenerictypemain;\n"
        "import test.cli.pkggenerictype;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "func main(args: string[]) {\n"
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
    char template_path[] = "temp/feng_cli_pkg_enum_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgenum",
        "open module test.cli.pkgenum;\n"
        "open enum HttpStatus {\n"
        "  Ok = 200,\n"
        "  NotFound = 404\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkgenummain;\n"
        "import test.cli.pkgenum;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "func main(args: string[]) {\n"
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
    char template_path[] = "temp/feng_cli_pkg_generic_spec_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkggenericspec",
        "open module test.cli.pkggenericspec;\n"
        "open spec Eq<T> {\n"
        "  func same(other: T): bool;\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkggenericspecmain;\n"
        "import test.cli.pkggenericspec;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "type Key: Eq<Key> {\n"
        "  var id: int;\n"
        "  func same(other: Key): bool {\n"
        "    return self.id == other.id;\n"
        "  }\n"
        "}\n"
        "func sameLocal<T: Eq<T>>(left: T, right: T): bool {\n"
        "  return left.same(right);\n"
        "}\n"
        "func main(args: string[]) {\n"
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
    char template_path[] = "temp/feng_cli_pkg_constrained_generic_fn_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgconstrainedgenericfn",
        "open module test.cli.pkgconstrainedgenericfn;\n"
        "open spec Eq<T> {\n"
        "  func same(other: T): bool;\n"
        "}\n"
        "open func sameAs<T: Eq<T>>(left: T, right: T): bool {\n"
        "  return left.same(right);\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkgconstrainedgenericfnmain;\n"
        "import test.cli.pkgconstrainedgenericfn;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "type Key: Eq<Key> {\n"
        "  var id: int;\n"
        "  func same(other: Key): bool {\n"
        "    return self.id == other.id;\n"
        "  }\n"
        "}\n"
        "func main(args: string[]) {\n"
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
    char template_path[] = "temp/feng_cli_pkg_constrained_generic_type_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgconstrainedgenerictype",
        "open module test.cli.pkgconstrainedgenerictype;\n"
        "open spec Eq<T> {\n"
        "  func same(other: T): bool;\n"
        "}\n"
        "open type MiniMap<K: Eq<K>, V> {\n"
        "  var key: K;\n"
        "  var value: V;\n"
        "  open func put(key: K, value: V) {\n"
        "    self.key = key;\n"
        "    self.value = value;\n"
        "  }\n"
        "  open func hasKey(key: K): bool {\n"
        "    return key.same(self.key);\n"
        "  }\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkgconstrainedgenerictypemain;\n"
        "import test.cli.pkgconstrainedgenerictype;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "type Key: Eq<Key> {\n"
        "  var id: int;\n"
        "  func same(other: Key): bool {\n"
        "    return self.id == other.id;\n"
        "  }\n"
        "}\n"
        "func main(args: string[]) {\n"
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

/* Verifies a binary-package consumer uses ordinary source construction before
 * copying mixed fields, while the no-source form remains direct zero-init. */
static void test_direct_build_consumes_package_mixin(void) {
    char template_path[] = "temp/feng_cli_pkg_mixin_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgmixin",
        "open module test.cli.pkgmixin;\n"
        "open spec Widget { func draw(area: int): int; }\n"
        "open type View: Widget {\n"
        "  open let initialized: int = 11;\n"
        "  open let constructorBound: int;\n"
        "  open let lateBound: int;\n"
        "  open var mutableValue: int = 3;\n"
        "  func View(value: int) { self.constructorBound = value; }\n"
        "  @mixable\n"
        "  open static func draw(target: Widget, area: int): int {\n"
        "    return area + 1;\n"
        "  }\n"
        "}\n"
        "open type ImplicitView {\n"
        "  open let implicitInitialized: int = 21;\n"
        "}\n"
        "open type PrivateImplicitView {\n"
        "  seal let hidden: int = 23;\n"
        "  open func read(): int { return self.hidden; }\n"
        "}\n"
        "open type PackageButton: Widget {\n"
        "  ...: View;\n"
        "}\n"
        "open type FitView: Widget {\n"
        "}\n"
        "open fit FitView {\n"
        "  @mixable\n"
        "  open static func draw(target: Widget, area: int): int {\n"
        "    return area + 2;\n"
        "  }\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkgmixinmain;\n"
        "import test.cli.pkgmixin;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "type Button: Widget {\n"
        "  ...: View = View(7);\n"
        "  func Button() { self.lateBound = 19; }\n"
        "}\n"
        "type ZeroButton: Widget {\n"
        "  ...: View;\n"
        "  func ZeroButton() {\n"
        "    self.initialized = 0;\n"
        "    self.constructorBound = 0;\n"
        "    self.lateBound = 0;\n"
        "  }\n"
        "}\n"
        "type ImplicitButton {\n"
        "  ...: ImplicitView = ImplicitView();\n"
        "}\n"
        "type PackageLeaf: Widget {\n"
        "  ...: PackageButton;\n"
        "}\n"
        "type FitButton: Widget {\n"
        "  ...: FitView;\n"
        "}\n"
        "func main(args: string[]) {\n"
        "  let source = View(7);\n"
        "  let implicitSource = ImplicitView();\n"
        "  let privateImplicitSource = PrivateImplicitView();\n"
        "  let button = Button();\n"
        "  let implicitButton = ImplicitButton();\n"
        "  let packageLeaf = PackageLeaf();\n"
        "  let fitSource = FitView();\n"
        "  let fitButton = FitButton();\n"
        "  let zero = ZeroButton();\n"
        "  if source.initialized == 11 && source.mutableValue == 3 &&\n"
        "     source.constructorBound == 7 && button.initialized == 11 &&\n"
        "     button.mutableValue == 3 && button.constructorBound == 7 &&\n"
        "     button.lateBound == 19 &&\n"
        "     button.draw(4) == 5 && Button.draw(button, 4) == 5 &&\n"
        "     implicitSource.implicitInitialized == 21 &&\n"
        "     implicitButton.implicitInitialized == 21 &&\n"
        "     privateImplicitSource.read() == 23 &&\n"
        "     packageLeaf.initialized == 0 &&\n"
        "     packageLeaf.mutableValue == 0 && packageLeaf.draw(4) == 5 &&\n"
        "     fitSource.draw(4) == 6 && FitView.draw(fitSource, 4) == 6 &&\n"
        "     fitButton.draw(4) == 6 && FitButton.draw(fitButton, 4) == 6 &&\n"
        "     zero.initialized == 0 && zero.mutableValue == 0 &&\n"
        "     zero.constructorBound == 0 && zero.lateBound == 0 {\n"
        "    puts(&\"package mixin ok\");\n"
        "  }\n"
        "}\n",
        "mixin_main",
        "package mixin ok\n");

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* A package consumer restores seal mix capabilities from .ft, links their
 * provider bodies, and reuses the ordinary wrapper path for all mix forms,
 * type/fit sources, explicit overrides, variadics, generics, and propagation. */
static void test_direct_build_consumes_package_mixable_seal_methods(void) {
    char template_path[] = "temp/feng_cli_pkg_mixable_seal_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    bundle_path = build_single_source_package_bundle(
        workspace_dir,
        "pkgmixableseal",
        "open module test.cli.pkgmixableseal;\n"
        "open spec Widget {}\n"
        "open spec GenericWidget {}\n"
        "open type View: Widget {\n"
        "  @mixable seal static func draw(target: Widget, value: int): int { return value + 1; }\n"
        "  @mixable seal static func collect(target: Widget, values: int...): int { return 5; }\n"
        "}\n"
        "open type Middle: Widget { ...: View; }\n"
        "open type FitView: Widget {}\n"
        "open fit FitView {\n"
        "  @mixable seal static func paint(target: Widget, value: int): int { return value + 2; }\n"
        "}\n"
        "open type GenericView<T>: GenericWidget {\n"
        "  @mixable seal static func echo(target: GenericWidget, value: T): T { return value; }\n"
        "}\n"
        "open type GenericMethodView: GenericWidget {\n"
        "  @mixable seal static func identity<T>(target: GenericWidget, value: T): T { return value; }\n"
        "}\n"
        "open type GenericFitView: GenericWidget {}\n"
        "open fit GenericFitView {\n"
        "  @mixable seal static func fitIdentity<T>(target: GenericWidget, value: T): T { return value; }\n"
        "}\n");

    compile_consumer_with_package_and_expect_stdout(
        workspace_dir,
        bundle_path,
        "module test.cli.pkgmixablesealmain;\n"
        "import test.cli.pkgmixableseal;\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "type DefaultButton: Widget {\n"
        "  ...: View;\n"
        "  open func run(value: int): int { return self.draw(value) + self.collect(1, 2, 3); }\n"
        "  open static func source(value: int): int { return View.draw(DefaultButton(), value); }\n"
        "}\n"
        "type BoundButton: Widget {\n"
        "  ...: View = View();\n"
        "  open func run(value: int): int { return self.draw(value); }\n"
        "}\n"
        "type InferredButton: Widget {\n"
        "  ... = View();\n"
        "  open func run(value: int): int { return self.draw(value); }\n"
        "}\n"
        "type Leaf: Widget {\n"
        "  ...: Middle;\n"
        "  open func run(value: int): int { return self.draw(value); }\n"
        "}\n"
        "type OverrideButton: Widget {\n"
        "  ...: View;\n"
        "  @mixable seal static func draw(target: Widget, value: int): int {\n"
        "    return View.draw(target, value) + 10;\n"
        "  }\n"
        "  open func run(value: int): int { return self.draw(value); }\n"
        "}\n"
        "type FitButton: Widget {\n"
        "  ...: FitView;\n"
        "  open func run(value: int): int { return self.paint(value); }\n"
        "}\n"
        "type GenericButton: GenericWidget {\n"
        "  ...: GenericView<int>;\n"
        "  open func run(value: int): int { return self.echo(value); }\n"
        "}\n"
        "type GenericMethodButton: GenericWidget {\n"
        "  ...: GenericMethodView;\n"
        "  open func run(value: string): string { return self.identity<string>(value); }\n"
        "}\n"
        "type GenericFitButton: GenericWidget {\n"
        "  ...: GenericFitView;\n"
        "  open func run(value: int): int { return self.fitIdentity<int>(value); }\n"
        "}\n"
        "func main(args: string[]) {\n"
        "  if DefaultButton().run(5) == 11 && DefaultButton.source(1) == 2 &&\n"
        "     BoundButton().run(2) == 3 && InferredButton().run(3) == 4 &&\n"
        "     Leaf().run(4) == 5 && OverrideButton().run(5) == 16 &&\n"
        "     FitButton().run(5) == 7 && GenericButton().run(8) == 8 &&\n"
        "     GenericMethodButton().run(\"ok\") == \"ok\" &&\n"
        "     GenericFitButton().run(9) == 9 {\n"
        "    puts(&\"package mixable seal ok\");\n"
        "  }\n"
        "}\n",
        "mixable_seal_main",
        "package mixable seal ok\n");

    /* The capability is present in package-public .ft for direct mixing, but
     * an unrelated imported-type call must still follow ordinary seal access. */
    {
        char *rejected_src_dir = path_join(workspace_dir, "rejected/src");
        char *rejected_source_path = path_join(rejected_src_dir, "main.ff");
        char *rejected_out_dir = path_join(workspace_dir, "rejected/build");
        char *out_opt = make_out_option(rejected_out_dir);
        char *pkg_opt = make_pkg_option(bundle_path);
        char *argv[] = {
            rejected_source_path,
            "--target=bin",
            out_opt,
            "--name=rejected_mixable_seal",
            pkg_opt,
        };

        mkdir_p(rejected_src_dir);
        write_text_file(
            rejected_source_path,
            "module test.cli.pkgmixablesealrejected;\n"
            "import test.cli.pkgmixableseal;\n"
            "type Other: Widget {\n"
            "  open static func run(): int { return View.draw(Other(), 1); }\n"
            "}\n"
            "func main(args: string[]) { Other.run(); }\n");
        ASSERT(run_direct_quiet_stderr(5, argv) != 0);

        free(pkg_opt);
        free(out_opt);
        free(rejected_out_dir);
        free(rejected_source_path);
        free(rejected_src_dir);
    }

    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

static void test_pack_bundle_manifest_rewrites_local_dependency_versions(void) {
    char template_path[] = "temp/feng_cli_pack_manifest_XXXXXX";
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
    bundle_path = path_join(root_project_dir, "build/pkg/rootlib-0.1.0.fb");

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
                    "open module local.dep;\n"
                    "open func value(): int {\n"
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
                    "open module root.lib;\n"
                    "open func root_value(): int {\n"
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
    char template_path[] = "temp/feng_cli_check_source_path_XXXXXX";
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
                    "open module test.cli.localdep;\n"
                    "open func dep_value(): int {\n"
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
                    "module test.cli.localdepapp;\n"
                    "\n"
                    "import test.cli.localdep;\n"
                    "\n"
                    "func main(args: string[]) {\n"
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
    char template_path[] = "temp/feng_cli_check_enum_diag_XXXXXX";
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
                    "module test.cli.enumdiag;\n"
                    "enum Status {\n"
                    "  Ok,\n"
                    "  NotFound\n"
                    "}\n"
                    "func main(args: string[]) {\n"
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
    char template_path[] = "temp/feng_cli_frontend_pkg_XXXXXX";
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

    ASSERT(mkdtemp(template_path) != NULL);
    workspace_dir = realpath(template_path, NULL);
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
                    "open module test.cli.pkgdep;\n"
                    "open func dep_value(): int {\n"
                    "  return 7;\n"
                    "}\n"
                    "func main(args: string[]) {}\n");
    write_text_file(main_source_path,
                    "module test.cli.pkgmain;\n"
                    "import test.cli.pkgdep;\n"
                    "func main(args: string[]) {}\n");

    {
        char *out_opt = make_out_option(dep_out_dir);
        char *argv[] = {
            dep_source_path,
            "--target=bin",
            out_opt,
            "--name=dep",
        };
        ASSERT(run_direct_for_host(4, argv) == 0);
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
    free(workspace_dir);
    free(bundle_path);
    free(dep_ft_path);
    free(dep_out_dir);
    free(main_source_path);
    free(dep_source_path);
    free(main_src_dir);
    free(dep_src_dir);
}

static void test_frontend_source_overlay_replaces_disk_source(void) {
    char template_path[] = "temp/feng_cli_frontend_overlay_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *main_source_path;
    char *remove_error = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengCliLoadedSource *sources = NULL;
    size_t source_count = 0U;

    static const char *kOverlaySource =
        "module overlay.demo;\n"
        "func main(args: string[]) {}\n";

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    src_dir = path_join(workspace_dir, "src");
    main_source_path = path_join(src_dir, "main.ff");

    mkdir_p(src_dir);
    write_text_file(main_source_path,
                    "module overlay.demo;\n"
                    "func main( {}\n");

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
    char template_path[] = "temp/feng_cli_frontend_overlay_dup_XXXXXX";
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
                    "module overlay.demo;\n"
                    "func main(args: string[]) {}\n");

    {
        static const char *kOverlaySource =
            "module overlay.demo;\n"
            "func main(args: string[]) {}\n";
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
    char template_path[] = "temp/feng_cli_direct_bad_pkg_XXXXXX";
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
                    "module test.cli.badpkg;\n"
                    "func main(args: string[]) {}\n");
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
    char template_path[] = "temp/feng_cli_init_bin_XXXXXX";
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
                  "module demo_app;\n"
                  "\n"
                  "import std.collections;\n"
                  "import std.io;\n"
                  "import std.numeric;\n"
                  "import std.text;\n"
                  "\n"
                  "func main(args: string[]) {\n"
                  "  let name = \"Feng\";\n"
                  "  println(\"Hello, {0}!\", name);\n"
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
    char template_path[] = "temp/feng_cli_init_lib_root_XXXXXX";
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
    expected_manifest = dup_printf(
        "[package]\n"
        "name: \"_9_demo_lib\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n"
        "platform: \"macos-arm64,linux-x64-gnu,linux-x64-musl,"
        "linux-arm64-gnu,linux-arm64-musl\"\n");
    expected_lib_text = dup_printf(
        "module _9_demo_lib;\n"
        "\n"
        "import std.collections;\n"
        "import std.io;\n"
        "import std.numeric;\n"
        "import std.text;\n"
        "\n"
        "open func hello(name: string) {\n"
        "  return string.format(\"Hello, {0}!\", name);\n"
        "}\n");
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
    char template_path[] = "temp/feng_cli_init_target_XXXXXX";
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
    char template_path[] = "temp/feng_cli_init_keyword_XXXXXX";
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
                  "module _if;\n"
                  "\n"
                  "import std.collections;\n"
                  "import std.io;\n"
                  "import std.numeric;\n"
                  "import std.text;\n"
                  "\n"
                  "func main(args: string[]) {\n"
                  "  let name = \"Feng\";\n"
                  "  println(\"Hello, {0}!\", name);\n"
                  "}\n") == 0);

    free(main_text);
    free(manifest_text);
    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(main_path);
    free(manifest_path);
}

static void test_init_rejects_non_empty_directory(void) {
    char template_path[] = "temp/feng_cli_init_nonempty_XXXXXX";
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
    ASSERT(strstr(stdout_text,
                  "Usage:\n"
                  "  feng build [<path>] [--release] [--keep-ir] "
                  "[--platform=<platform>]... [--sysroot=<path>]\n") != NULL);
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
    ASSERT(strstr(stdout_text,
                  "Usage:\n"
                  "  feng pack [<path>] [--platform=<platform>]... "
                  "[--sysroot=<path>]\n") != NULL);
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
    char template_path[] = "temp/feng_cli_dap_launch_ok_XXXXXX";
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
                                                   backend_path,
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

/* Ensure FENG_LLDB_DAP selects an explicit backend even when PATH misses it. */
static void test_dap_resolves_backend_via_feng_lldb_dap(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x12U};
    char template_path[] = "temp/feng_cli_dap_launch_env_XXXXXX";
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
    backend_path = path_join(workspace_dir, "resolved-lldb-dap");
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
    write_executable_text_file(backend_path, backend_script);
    path_value = dup_cstr("/bin:/usr/bin:/usr/sbin:/sbin");

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
                                                   backend_path,
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

/* Ensure launch fails locally when the `.fd` fingerprint no longer matches the binary. */
static void test_dap_rejects_fingerprint_mismatch_before_backend_spawn(void) {
    static const unsigned char kOriginalBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x21U};
    static const unsigned char kModifiedBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x22U};
    char template_path[] = "temp/feng_cli_dap_launch_mismatch_XXXXXX";
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
                                                   marker_path,
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

/* Ensure launch distinguishes backend lookup and process-start failures. */
static void test_dap_reports_missing_backend_after_launch_validation(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x31U};
    char template_path[] = "temp/feng_cli_dap_missing_backend_XXXXXX";
    char *workspace_dir;
    char *binary_path;
    char *fd_path;
    char *backend_path;
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
    backend_path = path_join(workspace_dir, "missing-lldb-dap");
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
                                                   backend_path,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc != 0);
    ASSERT(strstr(stdout_text, "\"command\":\"initialize\"") != NULL);
    ASSERT(strstr(stdout_text, "\"command\":\"launch\"") != NULL);
    ASSERT(strstr(stdout_text,
                  "FENG_LLDB_DAP specifies an unavailable executable") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(stderr_text);
    free(stdout_text);
    stderr_text = NULL;
    write_executable_text_file(backend_path,
                               "#!/feng-test/missing-interpreter\nexit 0\n");
    stdout_text = run_dap_capture_stdout_with_path(1,
                                                   argv,
                                                   input_text,
                                                   workspace_dir,
                                                   backend_path,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc != 0);
    ASSERT(strstr(stdout_text, "\"command\":\"launch\"") != NULL);
    ASSERT(strstr(stdout_text, "failed to exec") != NULL);
    ASSERT(strstr(stdout_text, "missing-lldb-dap") != NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(launch_text);
    free(initialize_text);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(backend_path);
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_breakpoints_uri_XXXXXX";
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
                                                   backend_path,
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_breakpoints_reject_XXXXXX";
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
                                                   backend_path,
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_stacktrace_path_XXXXXX";
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
                                                   backend_path,
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
        "module demo.pkg;\n"
        "let TEST_NAME: string = \"hello_world\";\n"
        "func main(args: string[]) {\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_stacktrace_uri_variant_XXXXXX";
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
                                                   backend_path,
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_stacktrace_hidden_XXXXXX";
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
                                                   backend_path,
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    let answer = 42;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_variables_names_XXXXXX";
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
                                "let requests = '';\n"
                                "let buffer = Buffer.alloc(0);\n"
                                "let nextSeq = 10;\n"
                                "function frame(payload) {\n"
                                "  return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\\r\\n\\r\\n${payload}`;\n"
                                "}\n"
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
                                "    if (message.command === 'scopes') process.stdout.write(responses.scopes);\n"
                                "    if (message.command === 'variables') process.stdout.write(responses.variables);\n"
                                "    if (message.command === 'evaluate') {\n"
                                "      const expression = message.arguments && message.arguments.expression;\n"
                                "      let body = null;\n"
                                "      if (expression === 'backend_param') body = { result: '[]', type: 'string[]', variablesReference: 0 };\n"
                                "      if (expression === 'backend_local') body = { result: '42', type: 'int', variablesReference: 0 };\n"
                                "      if (body !== null) {\n"
                                "        process.stdout.write(frame(JSON.stringify({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'evaluate', body })));\n"
                                "      }\n"
                                "    }\n"
                                "  }\n"
                                "});\n"
                                "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); });\n",
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
                                                               backend_path,
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

/* Ensure top-level variables keep only user mappings and surface user-facing values. */
static void test_dap_filters_backend_variables_and_rewrites_user_values(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x48U};
    static const char *kSourceText =
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    for var i = 1; i <= 1000; i += 1 {\n"
        "        println(\"Hello, world!\");\n"
        "    }\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_variables_filter_XXXXXX";
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
                                                   backend_path,
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

static void test_project_build_rewrites_module_binding_in_dap_globals(void) {
    char template_path[] = "temp/feng_cli_dap_module_globals_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *backend_source_uri;
    char *string_value_expr;
    char *string_length_expr;
    char *string_data_address_expr;
    char *escaped_binary_path;
    char *escaped_frame_backend_name;
    char *escaped_backend_source_uri;
    char *escaped_global_backend_name;
    char *escaped_string_length_expr;
    char *escaped_string_data_address_expr;
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
    char *backend_locals_json;
    char *backend_locals_text;
    char *backend_globals_json;
    char *backend_globals_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_scopes_text;
    char *escaped_backend_locals_text;
    char *escaped_backend_globals_text;
    char *backend_script;
    char *input_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengDebugArtifact artifact = {0};
    const char *frame_backend_name = NULL;
    const char *global_backend_name = NULL;
    size_t index;
    int rc;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "demo");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    binary_path = project_host_build_path(project_dir, "bin/demo");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"demo\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(source_path,
                    "module demo;\n"
                    "\n"
                    "let TEST_NAME: string = \"hello_world\";\n"
                    "\n"
                    "func main(args: string[]) {\n"
                    "    let prefix: string = TEST_NAME;\n"
                    "}\n");

    {
        char *build_argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, build_argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    ASSERT(path_exists(fd_path));
    ASSERT(feng_debug_read_fd(fd_path, &artifact, &fd_error));
    ASSERT(fd_error == NULL);
    ASSERT(artifact.package_count > 0U);

    for (index = 0U; index < artifact.info.frame_count; ++index) {
        FengCodegenMapingFrameRecord *frame = &artifact.info.frames[index];

        if (frame->display_name != NULL && strcmp(frame->display_name, "main") == 0) {
            frame_backend_name = frame->backend_symbol;
            break;
        }
    }
    ASSERT(frame_backend_name != NULL);

    for (index = 0U; index < artifact.info.variable_count; ++index) {
        FengCodegenMapingVariableRecord *variable = &artifact.info.variables[index];

        if (strcmp(variable->frame_backend_symbol, frame_backend_name) == 0 &&
            strcmp(variable->display_name, "TEST_NAME") == 0) {
            global_backend_name = variable->backend_name;
            ASSERT(strcmp(variable->display_type, "string") == 0);
            ASSERT(variable->kind == FENG_CODEGEN_MAPING_VARIABLE_BINDING);
            break;
        }
    }
    ASSERT(global_backend_name != NULL);

    backend_source_uri = dup_printf("%s://main.ff", artifact.packages[0].package_name);
    string_value_expr = dup_printf("(const char *)feng_string_data((const FengString *)(%s))",
                                   global_backend_name);
    string_length_expr = dup_printf("(size_t)feng_string_length((const FengString *)(%s))",
                                    global_backend_name);
    string_data_address_expr = dup_printf("(uintptr_t)feng_string_data((const FengString *)(%s))",
                                          global_backend_name);
    escaped_binary_path = json_escape_text(binary_path);
    escaped_frame_backend_name = json_escape_text(frame_backend_name);
    escaped_backend_source_uri = json_escape_text(backend_source_uri);
    escaped_global_backend_name = json_escape_text(global_backend_name);
    escaped_string_length_expr = json_escape_text(string_length_expr);
    escaped_string_data_address_expr = json_escape_text(string_data_address_expr);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"%s\",\"source\":{\"name\":\"main.ff\",\"path\":\"%s\"},\"line\":6,\"column\":5}],\"totalFrames\":1}}",
                                     escaped_frame_backend_name,
                                     escaped_backend_source_uri);
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false},{\"name\":\"Globals\",\"variablesReference\":102,\"expensive\":false}]}}");
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_locals_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[]}}");
    backend_locals_text = build_dap_message_text(backend_locals_json);
    backend_globals_json = dup_printf("{\"seq\":5,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"%s\",\"evaluateName\":\"%s\",\"value\":\"0x1000\",\"type\":\"FengString *\",\"variablesReference\":0}]}}",
                                     escaped_global_backend_name,
                                     escaped_global_backend_name);
    backend_globals_text = build_dap_message_text(backend_globals_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_scopes_text = json_escape_text(backend_scopes_text);
    escaped_backend_locals_text = json_escape_text(backend_locals_text);
    escaped_backend_globals_text = json_escape_text(backend_globals_text);

    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = {\n"
                                "  initialize: \"%s\",\n"
                                "  stackTrace: \"%s\",\n"
                                "  scopes: \"%s\",\n"
                                "  locals: \"%s\",\n"
                                "  globals: \"%s\"\n"
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
                                "    if (message.command === 'variables') {\n"
                                "      if (message.arguments && message.arguments.variablesReference === 102) {\n"
                                "        process.stdout.write(responses.globals);\n"
                                "      } else {\n"
                                "        process.stdout.write(responses.locals);\n"
                                "      }\n"
                                "    }\n"
                                "    if (message.command === 'evaluate') {\n"
                                "      let body = { result: '0', type: 'int', variablesReference: 0 };\n"
                                "      if (message.arguments.expression === '%s') {\n"
                                "        body = { result: '0x1000', type: 'FengString *', variablesReference: 23 };\n"
                                "      } else if (message.arguments.expression === '%s') {\n"
                                "        body = { result: '11', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '%s') {\n"
                                "        body = { result: '8192', type: 'uintptr_t', variablesReference: 0 };\n"
                                "      }\n"
                                "      const response = {\n"
                                "        seq: 6,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'evaluate',\n"
                                "        body\n"
                                "      };\n"
                                "      process.stdout.write(frame(JSON.stringify(response)));\n"
                                "    }\n"
                                "    if (message.command === 'readMemory') {\n"
                                "      const response = {\n"
                                "        seq: 7,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'readMemory',\n"
                                "        body: { address: '0x2000', data: 'aGVsbG9fd29ybGQ=' }\n"
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
                                escaped_backend_locals_text,
                                escaped_backend_globals_text,
                                escaped_global_backend_name,
                                escaped_string_length_expr,
                                escaped_string_data_address_expr);
    write_executable_text_file(backend_path, backend_script);

    path_value = dup_printf("%s:%s",
                            workspace_dir,
                            getenv("PATH") != NULL ? getenv("PATH") : "");
    initialize_json = dup_printf("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"feng\"}}");
    launch_json = dup_printf("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
                             escaped_binary_path);
    stack_trace_json = dup_printf("{\"seq\":3,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    scopes_json = dup_printf("{\"seq\":4,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":7}}");
    variables_json = dup_printf("{\"seq\":5,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":102}}");
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
                                                   backend_path,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"variablesReference\":102") != NULL);
    ASSERT(strstr(requests_text, global_backend_name) != NULL);
    ASSERT(strstr(requests_text, string_value_expr) == NULL);
    ASSERT(strstr(requests_text, string_length_expr) != NULL);
    ASSERT(strstr(requests_text, string_data_address_expr) != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"readMemory\"") != NULL);
    ASSERT(strstr(requests_text, "\"memoryReference\":\"0x2000\"") != NULL);
    ASSERT(strstr(requests_text, "\"count\":11") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"TEST_NAME\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"\\\"hello_world\\\"\"") != NULL);
    ASSERT(strstr(stdout_text, global_backend_name) == NULL);
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
    free(path_value);
    free(backend_script);
    free(escaped_backend_globals_text);
    free(escaped_backend_locals_text);
    free(escaped_backend_scopes_text);
    free(escaped_backend_stack_trace_text);
    free(escaped_backend_initialize_text);
    free(backend_globals_text);
    free(backend_globals_json);
    free(backend_locals_text);
    free(backend_locals_json);
    free(backend_scopes_text);
    free(backend_scopes_json);
    free(backend_stack_trace_text);
    free(backend_stack_trace_json);
    free(backend_initialize_text);
    free(backend_initialize_json);
    free(escaped_string_data_address_expr);
    free(escaped_string_length_expr);
    free(escaped_global_backend_name);
    free(escaped_backend_source_uri);
    free(escaped_frame_backend_name);
    free(escaped_binary_path);
    free(string_value_expr);
    free(string_length_expr);
    free(string_data_address_expr);
    free(backend_source_uri);
    feng_debug_artifact_dispose(&artifact);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(requests_path);
    free(backend_path);
    free(fd_path);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
    free(fd_error);
}

static void test_dap_reads_exact_string_bytes_without_backend_string_formatting(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x4bU};
    static const char *kSourceText =
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    let name: string = \"value\";\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_string_memory_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *backend_initialize_json;
    char *backend_stack_trace_json;
    char *backend_scopes_json;
    char *backend_variables_json;
    char *backend_initialize_text;
    char *backend_stack_trace_text;
    char *backend_scopes_text;
    char *backend_variables_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_scopes_text;
    char *escaped_backend_variables_text;
    char *backend_script;
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
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_text_file(source_path, kSourceText);
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
        "backend_name",
        "name",
        NULL,
        "string",
        FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true,\"supportsReadMemoryRequest\":true}}");
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":5}],\"totalFrames\":1}}");
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_name\",\"evaluateName\":\"backend_name\",\"value\":\"0x1000\",\"type\":\"FengString *\",\"variablesReference\":6}]}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_text = build_dap_message_text(backend_variables_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_scopes_text = json_escape_text(backend_scopes_text);
    escaped_backend_variables_text = json_escape_text(backend_variables_text);

    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = { initialize: \"%s\", stackTrace: \"%s\", scopes: \"%s\", variables: \"%s\" };\n"
                                "function frame(payload) { return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\\r\\n\\r\\n${payload}`; }\n"
                                "function respond(message, command, body) {\n"
                                "  process.stdout.write(frame(JSON.stringify({ seq: 5, type: 'response', request_seq: message.seq, success: true, command, body })));\n"
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
                                "    const match = /Content-Length: (\\d+)/i.exec(buffer.slice(0, sep).toString('utf8'));\n"
                                "    if (!match) break;\n"
                                "    const length = Number(match[1]);\n"
                                "    const frameLength = sep + 4 + length;\n"
                                "    if (buffer.length < frameLength) break;\n"
                                "    const message = JSON.parse(buffer.slice(sep + 4, frameLength).toString('utf8'));\n"
                                "    buffer = buffer.slice(frameLength);\n"
                                "    if (message.command === 'stackTrace') process.stdout.write(responses.stackTrace);\n"
                                "    if (message.command === 'scopes') process.stdout.write(responses.scopes);\n"
                                "    if (message.command === 'variables') process.stdout.write(responses.variables);\n"
                                "    if (message.command === 'evaluate') {\n"
                                "      const expression = message.arguments && message.arguments.expression;\n"
                                "      let body = { result: '0', type: 'int', variablesReference: 0 };\n"
                                "      if (expression === 'backend_name') body = { result: '0x1000', type: 'FengString *', variablesReference: 6 };\n"
                                "      if (expression === '(size_t)feng_string_length((const FengString *)(backend_name))') body = { result: '6', type: 'size_t', variablesReference: 0 };\n"
                                "      if (expression === '(uintptr_t)feng_string_data((const FengString *)(backend_name))') body = { result: '8192', type: 'uintptr_t', variablesReference: 0 };\n"
                                "      respond(message, 'evaluate', body);\n"
                                "    }\n"
                                "    if (message.command === 'readMemory') {\n"
                                "      respond(message, 'readMemory', { address: '0x2000', data: 'QQBCCkMi' });\n"
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
                                                   backend_path,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text,
                  "(const char *)feng_string_data((const FengString *)") == NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(size_t)feng_string_length((const FengString *)(backend_name))\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(uintptr_t)feng_string_data((const FengString *)(backend_name))\"") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"readMemory\"") != NULL);
    ASSERT(strstr(requests_text, "\"memoryReference\":\"0x2000\"") != NULL);
    ASSERT(strstr(requests_text, "\"count\":6") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"name\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"\\\"A\\\\0B\\\\nC\\\\\\\"\\\"\"") != NULL);
    ASSERT(strstr(stdout_text, "\"variablesReference\":0") != NULL);
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
    free(backend_scopes_text);
    free(backend_stack_trace_text);
    free(backend_initialize_text);
    free(backend_variables_json);
    free(backend_scopes_json);
    free(backend_stack_trace_json);
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

/* Ensure an invalid or oversized runtime string length cannot reach backend readMemory. */
static void test_dap_bounds_runtime_string_memory_reads(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x4cU};
    static const char *kSourceText =
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    let name: string = \"value\";\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_string_limit_XXXXXX";
    char *workspace_dir;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *fd_path;
    char *backend_path;
    char *requests_path;
    char *path_value;
    char *escaped_binary_path;
    char *backend_initialize_json;
    char *backend_stack_trace_json;
    char *backend_scopes_json;
    char *backend_variables_json;
    char *backend_initialize_text;
    char *backend_stack_trace_text;
    char *backend_scopes_text;
    char *backend_variables_text;
    char *escaped_backend_initialize_text;
    char *escaped_backend_stack_trace_text;
    char *escaped_backend_scopes_text;
    char *escaped_backend_variables_text;
    char *backend_script;
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
    binary_path = path_join(workspace_dir, "demo.bin");
    fd_path = dup_printf("%s.fd", binary_path);
    backend_path = path_join(workspace_dir, "lldb-dap");
    requests_path = path_join(workspace_dir, "requests.txt");
    ASSERT(fd_path != NULL);

    write_text_file(source_path, kSourceText);
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
        "backend_name",
        "name",
        NULL,
        "string",
        FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true,\"supportsReadMemoryRequest\":true}}");
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":3,\"column\":5}],\"totalFrames\":1}}");
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_name\",\"evaluateName\":\"backend_name\",\"value\":\"0x1000\",\"type\":\"FengString *\",\"variablesReference\":6}]}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_text = build_dap_message_text(backend_variables_json);
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    escaped_backend_stack_trace_text = json_escape_text(backend_stack_trace_text);
    escaped_backend_scopes_text = json_escape_text(backend_scopes_text);
    escaped_backend_variables_text = json_escape_text(backend_variables_text);

    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const responses = { initialize: \"%s\", stackTrace: \"%s\", scopes: \"%s\", variables: \"%s\" };\n"
                                "function frame(payload) { return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\\r\\n\\r\\n${payload}`; }\n"
                                "function respond(message, command, body) {\n"
                                "  process.stdout.write(frame(JSON.stringify({ seq: 5, type: 'response', request_seq: message.seq, success: true, command, body })));\n"
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
                                "    const match = /Content-Length: (\\d+)/i.exec(buffer.slice(0, sep).toString('utf8'));\n"
                                "    if (!match) break;\n"
                                "    const length = Number(match[1]);\n"
                                "    const frameLength = sep + 4 + length;\n"
                                "    if (buffer.length < frameLength) break;\n"
                                "    const message = JSON.parse(buffer.slice(sep + 4, frameLength).toString('utf8'));\n"
                                "    buffer = buffer.slice(frameLength);\n"
                                "    if (message.command === 'stackTrace') process.stdout.write(responses.stackTrace);\n"
                                "    if (message.command === 'scopes') process.stdout.write(responses.scopes);\n"
                                "    if (message.command === 'variables') process.stdout.write(responses.variables);\n"
                                "    if (message.command === 'evaluate') {\n"
                                "      const expression = message.arguments && message.arguments.expression;\n"
                                "      let body = { result: '0', type: 'int', variablesReference: 0 };\n"
                                "      if (expression === 'backend_name') body = { result: '0x1000', type: 'FengString *', variablesReference: 6 };\n"
                                "      if (expression === '(size_t)feng_string_length((const FengString *)(backend_name))') body = { result: '1048577', type: 'size_t', variablesReference: 0 };\n"
                                "      if (expression === '(uintptr_t)feng_string_data((const FengString *)(backend_name))') body = { result: '8192', type: 'uintptr_t', variablesReference: 0 };\n"
                                "      respond(message, 'evaluate', body);\n"
                                "    }\n"
                                "    if (message.command === 'readMemory') {\n"
                                "      respond(message, 'readMemory', { address: '0x2000', data: 'QQ==' });\n"
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
                                                   backend_path,
                                                   &rc,
                                                   &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(size_t)feng_string_length((const FengString *)(backend_name))\"") != NULL);
    ASSERT(strstr(requests_text,
                  "(uintptr_t)feng_string_data((const FengString *)(backend_name))") == NULL);
    ASSERT(strstr(requests_text, "\"command\":\"readMemory\"") == NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"name\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"string\"") != NULL);
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
    free(backend_scopes_text);
    free(backend_stack_trace_text);
    free(backend_initialize_text);
    free(backend_variables_json);
    free(backend_scopes_json);
    free(backend_stack_trace_json);
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    println(\"Hello, world!\");\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_array_summary_type_XXXXXX";
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
    char *synthetic_variables_json;
    char *synthetic_numbers_json;
    char *synthetic_matrix_json;
    char *synthetic_matrix_row_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *scopes_text;
    char *variables_text;
    char *synthetic_variables_text;
    char *synthetic_numbers_text;
    char *synthetic_matrix_text;
    char *synthetic_matrix_row_text;
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
    char *backend_script_tail;
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
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(
        &info,
        "demo_pkg_main_backend",
        "backend_nums",
        "nums",
        NULL,
        "i64[]",
        FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(
        &info,
        "demo_pkg_main_backend",
        "backend_matrix",
        "matrix",
        NULL,
        "i64[][]",
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
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":2,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_param\",\"evaluateName\":\"backend_param\",\"value\":\"0x1000\",\"type\":\"FengArray *\",\"variablesReference\":23},{\"name\":\"backend_nums\",\"evaluateName\":\"backend_nums\",\"value\":\"0x3000\",\"type\":\"FengArray *\",\"variablesReference\":24},{\"name\":\"backend_matrix\",\"evaluateName\":\"backend_matrix\",\"value\":\"0x4000\",\"type\":\"FengArray *\",\"variablesReference\":25}]}}");
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
                                "      } else if (message.arguments.expression === 'backend_nums') {\n"
                                "        body = { result: '0x3000', type: 'FengArray *', variablesReference: 24 };\n"
                                "      } else if (message.arguments.expression === 'backend_matrix') {\n"
                                "        body = { result: '0x4000', type: 'FengArray *', variablesReference: 25 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_array_length((const FengArray *)(backend_param))') {\n"
                                "        body = { result: '2', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_array_length((const FengArray *)(backend_nums))') {\n"
                                "        body = { result: '257', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_array_length((const FengArray *)(backend_matrix))') {\n"
                                "        body = { result: '1', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_array_length((const FengArray *)(((FengArray * *)feng_array_data(backend_matrix))[0]))') {\n"
                                "        body = { result: '2', type: 'size_t', variablesReference: 0 };\n",
                                requests_path,
                                escaped_backend_initialize_text,
                                escaped_backend_stack_trace_text,
                                escaped_backend_scopes_text,
                                escaped_backend_variables_text);
    backend_script_tail = dup_printf("      } else if (message.arguments.expression === '((FengString * *)feng_array_data(backend_param))[0]') {\n"
                                "        body = { result: '0x2000', type: 'FengString *', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '((FengString * *)feng_array_data(backend_param))[1]') {\n"
                                "        body = { result: '0x2008', type: 'FengString *', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_string_length((const FengString *)(((FengString * *)feng_array_data(backend_param))[0]))') {\n"
                                "        body = { result: '4', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(uintptr_t)feng_string_data((const FengString *)(((FengString * *)feng_array_data(backend_param))[0]))') {\n"
                                "        body = { result: '8192', type: 'uintptr_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_string_length((const FengString *)(((FengString * *)feng_array_data(backend_param))[1]))') {\n"
                                "        body = { result: '3', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(uintptr_t)feng_string_data((const FengString *)(((FengString * *)feng_array_data(backend_param))[1]))') {\n"
                                "        body = { result: '8200', type: 'uintptr_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '((int64_t *)feng_array_data(backend_nums))[0]') {\n"
                                "        body = { result: '10', type: 'int64_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '((int64_t *)feng_array_data(backend_nums))[1]') {\n"
                                "        body = { result: '20', type: 'int64_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '((int64_t *)feng_array_data(backend_nums))[2]') {\n"
                                "        body = { result: '30', type: 'int64_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '((FengArray * *)feng_array_data(backend_matrix))[0]') {\n"
                                "        body = { result: '0x5000', type: 'FengArray *', variablesReference: 26 };\n"
                                "      } else if (message.arguments.expression === '((int64_t *)feng_array_data(((FengArray * *)feng_array_data(backend_matrix))[0]))[0]') {\n"
                                "        body = { result: '101', type: 'int64_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '((int64_t *)feng_array_data(((FengArray * *)feng_array_data(backend_matrix))[0]))[1]') {\n"
                                "        body = { result: '102', type: 'int64_t', variablesReference: 0 };\n"
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
                                "    if (message.command === 'readMemory') {\n"
                                "      const reference = message.arguments && message.arguments.memoryReference;\n"
                                "      const data = reference === '0x2000' ? 'emVybw==' : 'b25l';\n"
                                "      process.stdout.write(frame(JSON.stringify({\n"
                                "        seq: 6,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'readMemory',\n"
                                "        body: { address: reference, data }\n"
                                "      })));\n"
                                "    }\n"
                                "  }\n"
                                "});\n"
                                "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); process.exit(0); });\n");
    backend_script = concat_owned_strings(backend_script, backend_script_tail);
    backend_script_tail = NULL;
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
    synthetic_variables_json = dup_printf("{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1073741824}}");
    synthetic_numbers_json = dup_printf("{\"seq\":7,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1073741825}}");
    synthetic_matrix_json = dup_printf("{\"seq\":8,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1073741826}}");
    synthetic_matrix_row_json = dup_printf("{\"seq\":9,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1073741827}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    scopes_text = build_dap_message_text(scopes_json);
    variables_text = build_dap_message_text(variables_json);
    synthetic_variables_text = build_dap_message_text(synthetic_variables_json);
    synthetic_numbers_text = build_dap_message_text(synthetic_numbers_json);
    synthetic_variables_text = concat_owned_strings(synthetic_variables_text, synthetic_numbers_text);
    synthetic_numbers_text = NULL;
    synthetic_matrix_text = build_dap_message_text(synthetic_matrix_json);
    synthetic_variables_text = concat_owned_strings(synthetic_variables_text, synthetic_matrix_text);
    synthetic_matrix_text = NULL;
    synthetic_matrix_row_text = build_dap_message_text(synthetic_matrix_row_json);
    synthetic_variables_text = concat_owned_strings(synthetic_variables_text, synthetic_matrix_row_text);
    synthetic_matrix_row_text = NULL;
    input_text = dup_printf("%s%s%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text,
                            scopes_text,
                            variables_text);

    stdout_text = run_dap_interactive_capture_stdout_with_path(1,
                                                               argv,
                                                               input_text,
                                                               "\"variablesReference\":1073741826",
                                                               synthetic_variables_text,
                                                               path_value,
                                                               backend_path,
                                                               &rc,
                                                               &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"expression\":\"backend_param\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(size_t)feng_array_length((const FengArray *)(backend_param))\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((FengString * *)feng_array_data(backend_param))[0]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((FengString * *)feng_array_data(backend_param))[1]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "(const char *)feng_string_data((const FengString *)") == NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(size_t)feng_string_length((const FengString *)(((FengString * *)feng_array_data(backend_param))[0]))\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(uintptr_t)feng_string_data((const FengString *)(((FengString * *)feng_array_data(backend_param))[1]))\"") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"readMemory\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((int64_t *)feng_array_data(backend_nums))[0]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((int64_t *)feng_array_data(backend_nums))[2]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((int64_t *)feng_array_data(backend_nums))[255]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((int64_t *)feng_array_data(backend_nums))[256]\"") == NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((FengArray * *)feng_array_data(backend_matrix))[0]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((int64_t *)feng_array_data(((FengArray * *)feng_array_data(backend_matrix))[0]))[0]\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((int64_t *)feng_array_data(((FengArray * *)feng_array_data(backend_matrix))[0]))[1]\"") != NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":1073741824") == NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":1073741825") == NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":1073741826") == NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":1073741827") == NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"args\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"string[length=2]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"variablesReference\":1073741824") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"nums\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"i64[length=257]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"variablesReference\":1073741825") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"matrix\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"i64[][length=1]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"variablesReference\":1073741826") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"i64[length=2]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"variablesReference\":1073741827") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"[0]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"[1]\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"10\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"20\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"30\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"101\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"102\"") != NULL);
    ASSERT(strstr(stdout_text, "truncated after 256 elements") != NULL);
    ASSERT(strstr(stdout_text, "\\\"zero\\\"") != NULL);
    ASSERT(strstr(stdout_text, "\\\"one\\\"") != NULL);
    ASSERT(strstr(stdout_text, "backend_param") == NULL);
    ASSERT(strstr(stdout_text, "backend_nums") == NULL);
    ASSERT(strstr(stdout_text, "backend_matrix") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(synthetic_variables_text);
    free(synthetic_numbers_text);
    free(synthetic_matrix_text);
    free(synthetic_matrix_row_text);
    free(variables_text);
    free(scopes_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(synthetic_matrix_row_json);
    free(synthetic_matrix_json);
    free(synthetic_numbers_json);
    free(synthetic_variables_json);
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

static void test_dap_clears_synthetic_refs_after_continue(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x4aU};
    static const char *kSourceText =
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    println(\"warmup\");\n"
        "    println(\"warmup\");\n"
        "    println(\"warmup\");\n"
        "    println(\"warmup\");\n"
        "    println(\"warmup\");\n"
        "    println(\"first stop\");\n"
        "    let i = 1;\n"
        "    println(i);\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_continue_synthetic_XXXXXX";
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
    char *globals_json;
    char *continue_json;
    char *stale_synthetic_json;
    char *stale_globals_json;
    char *second_stack_trace_json;
    char *second_scopes_json;
    char *second_variables_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *scopes_text;
    char *variables_text;
    char *globals_text;
    char *continue_text;
    char *stale_synthetic_text;
    char *stale_globals_text;
    char *second_stack_trace_text;
    char *second_scopes_text;
    char *second_variables_text;
    char *backend_initialize_json;
    char *backend_initialize_text;
    char *escaped_backend_initialize_text;
    char *backend_script;
    char *backend_script_tail;
    char *input_text;
    char *followup_text;
    char *stdout_text;
    char *stderr_text = NULL;
    char *requests_text;
    char *fd_error = NULL;
    char *remove_error = NULL;
    char *argv[] = { "--stdio" };
    FengCodegenMapingInfo info = {0};
    FengCodegenMapingSourceMapping sources[1];
    const char *continue_pos;
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
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(
        &info,
        "demo_pkg_main_backend",
        "backend_global",
        "TEST_NAME",
        NULL,
        "string",
        FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(
        &info,
        "demo_pkg_main_backend",
        "backend_i",
        "i",
        NULL,
        "i64",
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
    escaped_backend_initialize_text = json_escape_text(backend_initialize_text);
    backend_script = dup_printf("#!/usr/bin/env node\n"
                                "const fs = require('fs');\n"
                                "const requestsPath = \"%s\";\n"
                                "const initializeResponse = \"%s\";\n"
                                "function frame(payload) {\n"
                                "  return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\\r\\n\\r\\n${payload}`;\n"
                                "}\n"
                                "let requests = '';\n"
                                "let buffer = Buffer.alloc(0);\n"
                                "let nextSeq = 20;\n"
                                "let stackTraceCount = 0;\n"
                                "process.stdout.write(initializeResponse);\n"
                                "function writeMessage(message) { process.stdout.write(frame(JSON.stringify(message))); }\n"
                                "function writeVariables(message) {\n"
                                "  const ref = message.arguments && message.arguments.variablesReference;\n"
                                "  const variables = ref === 202 || ref === 204 ? [\n"
                                "    { name: 'backend_global', evaluateName: 'backend_global', value: '0x3000', type: 'FengString *', variablesReference: 24 }\n"
                                "  ] : ref === 102 ? [\n"
                                "    { name: 'backend_param', evaluateName: 'backend_param', value: '0x1000', type: 'FengArray *', variablesReference: 23 },\n"
                                "    { name: 'backend_i', evaluateName: 'backend_i', value: '1', type: 'int64_t', variablesReference: 0 }\n"
                                "  ] : [\n"
                                "    { name: 'backend_param', evaluateName: 'backend_param', value: '0x1000', type: 'FengArray *', variablesReference: 23 }\n"
                                "  ];\n"
                                "  writeMessage({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'variables', body: { variables } });\n"
                                "}\n"
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
                                "    if (message.command === 'stackTrace') {\n"
                                "      stackTraceCount += 1;\n"
                                "      const frameId = stackTraceCount === 1 ? 7 : 8;\n"
                                "      const line = stackTraceCount === 1 ? 8 : 10;\n"
                                "      writeMessage({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'stackTrace', body: { stackFrames: [{ id: frameId, name: 'demo_pkg_main_backend', source: { name: 'main.ff', path: 'demo.pkg://main.ff' }, line, column: 1 }], totalFrames: 1 } });\n"
                                "    } else if (message.command === 'scopes') {\n"
                                "      const scopeRef = message.arguments && message.arguments.frameId === 8 ? 102 : 101;\n"
                                "      const globalsRef = message.arguments && message.arguments.frameId === 8 ? 204 : 202;\n"
                                "      writeMessage({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'scopes', body: { scopes: [{ name: 'Locals', variablesReference: scopeRef, expensive: false }, { name: 'Globals', variablesReference: globalsRef, expensive: false }] } });\n"
                                "    } else if (message.command === 'variables') {\n"
                                "      writeVariables(message);\n"
                                "    } else if (message.command === 'continue') {\n"
                                "      writeMessage({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'continue', body: { allThreadsContinued: true } });\n"
                                "      writeMessage({ seq: nextSeq++, type: 'event', event: 'stopped', body: { reason: 'breakpoint', threadId: 1, allThreadsStopped: true } });\n"
                                "    } else if (message.command === 'evaluate') {\n",
                                requests_path,
                                escaped_backend_initialize_text);
    backend_script_tail = dup_printf("      const expression = message.arguments && message.arguments.expression;\n"
                                     "      let body = { result: '0', type: 'int', variablesReference: 0 };\n"
                                     "      if (expression === 'backend_param') {\n"
                                     "        body = { result: '0x1000', type: 'FengArray *', variablesReference: 23 };\n"
                                     "      } else if (expression === 'backend_global') {\n"
                                     "        body = { result: '0x3000', type: 'FengString *', variablesReference: 24 };\n"
                                     "      } else if (expression === 'backend_i') {\n"
                                     "        body = { result: '1', type: 'int64_t', variablesReference: 0 };\n"
                                     "      } else if (expression === '(size_t)feng_array_length((const FengArray *)(backend_param))') {\n"
                                     "        body = { result: '1', type: 'size_t', variablesReference: 0 };\n"
                                     "      } else if (expression === '((FengString * *)feng_array_data(backend_param))[0]') {\n"
                                     "        body = { result: '0x2000', type: 'FengString *', variablesReference: 0 };\n"
                                     "      } else if (expression === '(size_t)feng_string_length((const FengString *)(((FengString * *)feng_array_data(backend_param))[0]))') {\n"
                                     "        body = { result: '5', type: 'size_t', variablesReference: 0 };\n"
                                     "      } else if (expression === '(uintptr_t)feng_string_data((const FengString *)(((FengString * *)feng_array_data(backend_param))[0]))') {\n"
                                     "        body = { result: '8448', type: 'uintptr_t', variablesReference: 0 };\n"
                                     "      } else if (expression === '(size_t)feng_string_length((const FengString *)(backend_global))') {\n"
                                     "        body = { result: '11', type: 'size_t', variablesReference: 0 };\n"
                                     "      } else if (expression === '(uintptr_t)feng_string_data((const FengString *)(backend_global))') {\n"
                                     "        body = { result: '12544', type: 'uintptr_t', variablesReference: 0 };\n"
                                     "      }\n"
                                     "      writeMessage({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'evaluate', body });\n"
                                     "    } else if (message.command === 'readMemory') {\n"
                                     "      const reference = message.arguments && message.arguments.memoryReference;\n"
                                     "      const data = reference === '0x2100' ? 'c3RhbGU=' : 'aGVsbG9fd29ybGQ=';\n"
                                     "      writeMessage({ seq: nextSeq++, type: 'response', request_seq: message.seq, success: true, command: 'readMemory', body: { address: reference, data } });\n"
                                     "    }\n"
                                     "  }\n"
                                     "});\n"
                                     "process.stdin.on('end', () => { fs.writeFileSync(requestsPath, requests); process.exit(0); });\n");
    backend_script = concat_owned_strings(backend_script, backend_script_tail);
    backend_script_tail = NULL;
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
    globals_json = dup_printf("{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":202}}");
    continue_json = dup_printf("{\"seq\":7,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    stale_synthetic_json = dup_printf("{\"seq\":8,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1073741824}}");
    stale_globals_json = dup_printf("{\"seq\":9,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":202}}");
    second_stack_trace_json = dup_printf("{\"seq\":10,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    second_scopes_json = dup_printf("{\"seq\":11,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":8}}");
    second_variables_json = dup_printf("{\"seq\":12,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":102}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    scopes_text = build_dap_message_text(scopes_json);
    variables_text = build_dap_message_text(variables_json);
    globals_text = build_dap_message_text(globals_json);
    stale_globals_text = build_dap_message_text(stale_globals_json);
    continue_text = build_dap_message_text(continue_json);
    stale_synthetic_text = build_dap_message_text(stale_synthetic_json);
    second_stack_trace_text = build_dap_message_text(second_stack_trace_json);
    second_scopes_text = build_dap_message_text(second_scopes_json);
    second_variables_text = build_dap_message_text(second_variables_json);
    input_text = dup_printf("%s%s%s%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text,
                            scopes_text,
                            variables_text,
                            globals_text);
    followup_text = dup_printf("%s%s%s%s%s",
                               continue_text,
                               stale_synthetic_text,
                               stale_globals_text,
                               second_stack_trace_text,
                               second_scopes_text);

    stdout_text = run_dap_two_step_interactive_capture_stdout_with_path(1,
                                                                        argv,
                                                                        input_text,
                                                                        "\"name\":\"TEST_NAME\"",
                                                                        followup_text,
                                                                        "\"request_seq\":11",
                                                                        second_variables_text,
                                                                        path_value,
                                                                        backend_path,
                                                                        &rc,
                                                                        &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    continue_pos = strstr(requests_text, "\"command\":\"continue\"");
    ASSERT(continue_pos != NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":102") != NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":202") != NULL);
    ASSERT(strstr(continue_pos, "\"variablesReference\":202") == NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"((FengString * *)feng_array_data(backend_param))[0]\"") == NULL);
    ASSERT(strstr(stdout_text, "unknown synthetic variablesReference") != NULL);
    ASSERT(strstr(stdout_text, "\"request_seq\":8,\"success\":false") != NULL);
    ASSERT(strstr(stdout_text, "\"request_seq\":9,\"success\":false") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"args\"") != NULL);
    ASSERT(strstr(stdout_text,
                  "\"name\":\"TEST_NAME\",\"evaluateName\":\"TEST_NAME\",\"value\":\"\\\"hello_world\\\"\",\"type\":\"FengString *\",\"variablesReference\":0") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"i\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"1\"") != NULL);
    ASSERT(strstr(stdout_text, "backend_global") == NULL);
    ASSERT(strstr(stdout_text, "backend_i") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(followup_text);
    free(input_text);
    free(second_variables_text);
    free(second_scopes_text);
    free(second_stack_trace_text);
    free(stale_globals_text);
    free(stale_synthetic_text);
    free(continue_text);
    free(globals_text);
    free(variables_text);
    free(scopes_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(second_variables_json);
    free(second_scopes_json);
    free(second_stack_trace_json);
    free(stale_globals_json);
    free(stale_synthetic_json);
    free(continue_json);
    free(globals_json);
    free(variables_json);
    free(scopes_json);
    free(stack_trace_json);
    free(launch_json);
    free(initialize_json);
    free(escaped_binary_path);
    free(path_value);
    free(backend_script_tail);
    free(backend_script);
    free(escaped_backend_initialize_text);
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

static void test_dap_expands_user_type_fields_with_synthetic_reference(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x4aU};
    static const char *kSourceText =
        "module demo.pkg;\n"
        "type Point {\n"
        "    let x: i32;\n"
        "    let label: string;\n"
        "}\n"
        "func main(args: string[]) {\n"
        "    let point: Point = Point{x: 7, label: \"seven\"};\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_type_fields_XXXXXX";
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
    char *synthetic_variables_json;
    char *initialize_text;
    char *launch_text;
    char *stack_trace_text;
    char *scopes_text;
    char *variables_text;
    char *synthetic_variables_text;
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
        "backend_point",
        "point",
        "backend_point",
        "Point",
        FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_codegen_maping_info_add_variable_with_parent_display_type(
        &info,
        NULL,
        NULL,
        "x",
        "->x",
        "i32",
        "Point",
        FENG_CODEGEN_MAPING_VARIABLE_FIELD));
    ASSERT(feng_codegen_maping_info_add_variable_with_parent_display_type(
        &info,
        NULL,
        NULL,
        "label",
        "->label",
        "string",
        "Point",
        FENG_CODEGEN_MAPING_VARIABLE_FIELD));
    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               sources,
                               1U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    backend_initialize_json = dup_printf("{\"seq\":1,\"type\":\"response\",\"request_seq\":1,\"success\":true,\"command\":\"initialize\",\"body\":{\"supportsConfigurationDoneRequest\":true}}");
    backend_initialize_text = build_dap_message_text(backend_initialize_json);
    backend_stack_trace_json = dup_printf("{\"seq\":2,\"type\":\"response\",\"request_seq\":3,\"success\":true,\"command\":\"stackTrace\",\"body\":{\"stackFrames\":[{\"id\":7,\"name\":\"demo_pkg_main_backend\",\"source\":{\"name\":\"main.ff\",\"path\":\"demo.pkg://main.ff\"},\"line\":6,\"column\":1}],\"totalFrames\":1}}");
    backend_stack_trace_text = build_dap_message_text(backend_stack_trace_json);
    backend_scopes_json = dup_printf("{\"seq\":3,\"type\":\"response\",\"request_seq\":4,\"success\":true,\"command\":\"scopes\",\"body\":{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":101,\"expensive\":false}]}}");
    backend_scopes_text = build_dap_message_text(backend_scopes_json);
    backend_variables_json = dup_printf("{\"seq\":4,\"type\":\"response\",\"request_seq\":5,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"backend_point\",\"evaluateName\":\"backend_point\",\"value\":\"0x1000\",\"type\":\"struct Point *\",\"variablesReference\":88}]}}");
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
                                "      if (message.arguments.expression === 'backend_point') {\n"
                                "        body = { result: '0x1000', type: 'struct Point *', variablesReference: 88 };\n"
                                "      } else if (message.arguments.expression === '(backend_point)->x') {\n"
                                "        body = { result: '7', type: 'int', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(backend_point)->label') {\n"
                                "        body = { result: '0x2000', type: 'FengString *', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(size_t)feng_string_length((const FengString *)((backend_point)->label))') {\n"
                                "        body = { result: '5', type: 'size_t', variablesReference: 0 };\n"
                                "      } else if (message.arguments.expression === '(uintptr_t)feng_string_data((const FengString *)((backend_point)->label))') {\n"
                                "        body = { result: '8192', type: 'uintptr_t', variablesReference: 0 };\n"
                                "      }\n"
                                "      process.stdout.write(frame(JSON.stringify({\n"
                                "        seq: 5,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'evaluate',\n"
                                "        body\n"
                                "      })));\n"
                                "    }\n"
                                "    if (message.command === 'readMemory') {\n"
                                "      process.stdout.write(frame(JSON.stringify({\n"
                                "        seq: 6,\n"
                                "        type: 'response',\n"
                                "        request_seq: message.seq,\n"
                                "        success: true,\n"
                                "        command: 'readMemory',\n"
                                "        body: { address: '0x2000', data: 'c2V2ZW4=' }\n"
                                "      })));\n"
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
    synthetic_variables_json = dup_printf("{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1073741824}}");
    initialize_text = build_dap_message_text(initialize_json);
    launch_text = build_dap_message_text(launch_json);
    stack_trace_text = build_dap_message_text(stack_trace_json);
    scopes_text = build_dap_message_text(scopes_json);
    variables_text = build_dap_message_text(variables_json);
    synthetic_variables_text = build_dap_message_text(synthetic_variables_json);
    input_text = dup_printf("%s%s%s%s%s",
                            initialize_text,
                            launch_text,
                            stack_trace_text,
                            scopes_text,
                            variables_text);

    stdout_text = run_dap_interactive_capture_stdout_with_path(1,
                                                               argv,
                                                               input_text,
                                                               "\"variablesReference\":1073741824",
                                                               synthetic_variables_text,
                                                               path_value,
                                                               backend_path,
                                                               &rc,
                                                               &stderr_text);
    ASSERT(rc == 0);
    requests_text = read_text_file(requests_path);
    ASSERT(strstr(requests_text, "\"expression\":\"(backend_point)->x\"") != NULL);
    ASSERT(strstr(requests_text, "\"expression\":\"(backend_point)->label\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(const char *)feng_string_data((const FengString *)((backend_point)->label))\"") == NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(size_t)feng_string_length((const FengString *)((backend_point)->label))\"") != NULL);
    ASSERT(strstr(requests_text,
                  "\"expression\":\"(uintptr_t)feng_string_data((const FengString *)((backend_point)->label))\"") != NULL);
    ASSERT(strstr(requests_text, "\"command\":\"readMemory\"") != NULL);
    ASSERT(strstr(requests_text, "\"variablesReference\":1073741824") == NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"point\"") != NULL);
    ASSERT(strstr(stdout_text, "\"variablesReference\":1073741824") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"x\"") != NULL);
    ASSERT(strstr(stdout_text, "\"value\":\"7\"") != NULL);
    ASSERT(strstr(stdout_text, "\"type\":\"i32\"") != NULL);
    ASSERT(strstr(stdout_text, "\"name\":\"label\"") != NULL);
    ASSERT(strstr(stdout_text, "\\\"seven\\\"") != NULL);
    ASSERT(strstr(stdout_text, "backend_point") == NULL);
    ASSERT(strcmp(stderr_text, "") == 0);

    free(requests_text);
    free(stderr_text);
    free(stdout_text);
    free(input_text);
    free(synthetic_variables_text);
    free(variables_text);
    free(scopes_text);
    free(stack_trace_text);
    free(launch_text);
    free(initialize_text);
    free(synthetic_variables_json);
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
    char template_path[] = "temp/feng_cli_dwarf_for_breakpoint_XXXXXX";
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
                    "module demo;\n"
                    "\n"
                    "func main(args: string[]) {\n"
                    "    for var i = 1; i <= 3; i += 1 {\n"
                    "        let j = i;\n"
                    "    }\n"
                    "}\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    dwarf_path = project_host_build_path(
        project_dir,
        "bin/demo.dSYM/Contents/Resources/DWARF/demo");
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

static void test_project_build_keeps_for_body_locals_after_prefix_binding(void) {
#if !defined(__APPLE__)
    return;
#else
    char template_path[] = "temp/feng_cli_lldb_for_prefix_locals_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *binary_path;
    char *output_path;
    char *command;
    char *output_text;
    char *repo_root;
    char *std_path;
    char *manifest_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    project_dir = path_join(workspace_dir, "demo");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "main.ff");
    binary_path = project_host_build_path(project_dir, "bin/demo");
    output_path = path_join(workspace_dir, "lldb.txt");
    repo_root = getcwd(NULL, 0);
    ASSERT(repo_root != NULL);
    std_path = path_join(repo_root, "std/std");
    ASSERT(std_path != NULL);
    manifest_text = dup_printf("[package]\n"
                               "name: \"demo\"\n"
                               "version: \"0.1.0\"\n"
                               "target: \"bin\"\n"
                               "src: \"src/\"\n"
                               "out: \"build/\"\n"
                               "[dependencies]\n"
                               "std: \"%s\"\n",
                               std_path);
    ASSERT(manifest_text != NULL);

    mkdir_p(src_dir);
    write_text_file(manifest_path, manifest_text);
    write_text_file(source_path,
                    "module demo;\n"
                    "\n"
                    "import std.io;\n"
                    "\n"
                    "let TEST_NAME: string = \"hello_world\";\n"
                    "\n"
                    "func main(args: string[]) {\n"
                    "    println(\"Running test: \" + TEST_NAME);\n"
                    "    for var i = 1; i <= 3; i += 1 {\n"
                    "        println(\"Hello, world!\");\n"
                    "    }\n"
                    "}\n");

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    command = dup_printf("lldb -b -o 'breakpoint set --file main.ff --line 10' -o 'process launch' -o 'frame variable' -- \"%s\" > \"%s\"",
                         binary_path,
                         output_path);
    ASSERT(command != NULL);
    run_command_or_die(command);
    output_text = read_text_file(output_path);

    ASSERT(strstr(output_text, "_l_i_") != NULL);

    free(output_text);
    free(command);
    free(manifest_text);
    free(std_path);
    free(repo_root);
    free(output_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(binary_path);
    free(source_path);
    free(src_dir);
    free(manifest_path);
    free(project_dir);
#endif
}

/* Ensure identifier evaluate ignores field records and rewrites Feng names to backend names. */
static void test_dap_rewrites_identifier_evaluate_expression(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x46U};
    static const char *kSourceText =
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    let answer = 42;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_evaluate_ident_XXXXXX";
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
    ASSERT(feng_codegen_maping_info_add_variable_with_parent_display_type(
        &info,
        NULL,
        NULL,
        "value",
        ".value",
        "int",
        "Demo",
        FENG_CODEGEN_MAPING_VARIABLE_FIELD));
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
                                                               backend_path,
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
        "module demo.pkg;\n"
        "func main(args: string[]) {\n"
        "    let answer = 42;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_dap_evaluate_phase5_XXXXXX";
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
                                                               backend_path,
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
        "module test.lsp;\n"
        "func main(args: string[]) {\n"
        "    let value: string = ;\n"
        "}\n";
    static const char *kGoodSource =
        "module test.lsp;\n"
        "func main(args: string[]) {\n"
        "    let value: string = \"ok\";\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_diag_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *bad_text;
    char *good_text;
    char *initialize;
    char *did_open;
    char *did_change;
    char *did_save_bad;
    char *did_save_good;
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
    did_save_bad = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
                              uri);
    did_change = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":2},\"contentChanges\":[{\"text\":\"%s\"}]}}",
                            uri,
                            good_text);
    did_save_good = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
                               uri);
    did_close = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
                           uri);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = temp_file();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, did_save_bad);
    write_lsp_message(input, did_change);
    write_lsp_message(input, did_save_good);
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
    free(did_save_good);
    free(did_change);
    free(did_save_bad);
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
        "module test.lsp;\n"
        "\n"
        "/** User record. */\n"
        "type User {\n"
        "    /** Display name. */\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "/** Formats a user label. */\n"
        "func format(user: User): string {\n"
        "    return user.name;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let label: string = format(user);\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_query_XXXXXX";
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

    input = temp_file();
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
    ASSERT(strstr(output, "\"triggerCharacters\":[\".\",\"_\",\"@\",\"a\"") != NULL);
    ASSERT(strstr(output, "\"Z\"") != NULL);
    ASSERT(strstr(output, "\"kind\":\"plaintext\"") != NULL);
    ASSERT(strstr(output, "Formats a user label.") != NULL);
    ASSERT(strstr(output, "func format(user: User): string") != NULL);
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
    char template_path[] = "temp/feng_cli_lsp_completion_incomplete_XXXXXX";
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

    input = temp_file();
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
    char template_path[] = "temp/feng_cli_lsp_hover_markup_XXXXXX";
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

    input = temp_file();
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
                                                   size_t char_offset,
                                                   const char *ready_text) {
    char *uri;
    char *escaped_text;
    char *did_open;
    char *request;
    char *shutdown;
    char *output;
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

    output = run_lsp_single_position_response_after_ready(initialize,
                                                          did_open,
                                                          method,
                                                          uri,
                                                          line,
                                                          character,
                                                          ready_text,
                                                          request,
                                                          shutdown);

    free(shutdown);
    free(request);
    free(did_open);
    free(escaped_text);
    free(uri);
    return output;
}

static void test_lsp_hover_uses_markdown_when_supported(void) {
    static const char *kSource =
        "module test.lsp.markdown;\n"
        "\n"
        "/**\n"
        " * Summarizes the CLI arguments.\n"
        " *\n"
        " * @param args The command-line arguments.\n"
        " */\n"
        "func describe(args: string[]): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let label: string = describe(args);\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]}}}}}";
    char *output = capture_lsp_hover_response(kSource,
                                              kInitialize,
                                              "let label: string = describe(args);",
                                              20U);

    ASSERT(strstr(output, "\"id\":2,\"result\":{\"contents\":{\"kind\":\"markdown\"") != NULL);
    ASSERT(strstr(output, "```feng\\nfunc describe(args: string[]): string\\n```") != NULL);
    ASSERT(strstr(output, "Summarizes the CLI arguments.") != NULL);
    ASSERT(strstr(output, "- **@param** `args` The command-line arguments.") != NULL);

    free(output);
}

static void test_lsp_hover_falls_back_to_plaintext_without_markdown_capability(void) {
    static const char *kSource =
        "module test.lsp.plaintext;\n"
        "\n"
        "/**\n"
        " * Summarizes the CLI arguments.\n"
        " *\n"
        " * @param args The command-line arguments.\n"
        " */\n"
        "func describe(args: string[]): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let label: string = describe(args);\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output = capture_lsp_hover_response(kSource,
                                              kInitialize,
                                              "let label: string = describe(args);",
                                              20U);

    ASSERT(strstr(output, "\"id\":2,\"result\":{\"contents\":{\"kind\":\"plaintext\"") != NULL);
    ASSERT(strstr(output, "func describe(args: string[]): string") != NULL);
    ASSERT(strstr(output, "@param args The command-line arguments.") != NULL);
    ASSERT(strstr(output, "**@param**") == NULL);
    ASSERT(strstr(output, "```feng") == NULL);

    free(output);
}

/* Hover exposes declaration shapes and proven outermost type categories. */
static void test_lsp_hover_type_categories_and_declaration_shapes(void) {
    static const char *kSource =
        "module test.lsp.hover_categories;\n"
        "\n"
        "spec Named {\n"
        "    let name: string;\n"
        "}\n"
        "spec EmptySpec {}\n"
        "spec Serializable {\n"
        "    func serialize(): string;\n"
        "}\n"
        "spec Mapper<T>(value: T): string;\n"
        "spec Choice: User | Point;\n"
        "spec Both: Named & Serializable;\n"
        "\n"
        "type Empty {}\n"
        "type User: Named {\n"
        "    let name: string;\n"
        "}\n"
        "@value type Point {\n"
        "    let x: f64;\n"
        "}\n"
        "type Unit();\n"
        "type Pair<T>(T, T);\n"
        "type Octet(i32, i32, i32, i32, i32, i32, i32, i32);\n"
        "\n"
        "func inspect(user: User, point: Point, pair: Pair<i32>, octet: Octet, users: User[], pointer: User*, count: i32, var total: i32): User {\n"
        "    let inferred = Point { x: 1.0 };\n"
        "    let first: i32 = octet.item1;\n"
        "    return user;\n"
        "}\n";
    static const char *kEnumSource =
        "module test.lsp.hover_enum;\n"
        "enum Color { Red = 1, Green = 2 }\n"
        "func inspect(color: Color) { let selected: Color = Color.Green; }\n";
    static const char *kConstructorSource =
        "module test.lsp.hover_constructor;\n"
        "type UserType { func UserType() {} }\n"
        "func run() { let constructed = UserType(); let literal = UserType {}; }\n";
    static const char *kPlaintextInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    static const char *kMarkdownInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"hover\":{\"contentFormat\":[\"markdown\"]}}}}}";
    char *output;

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "type Empty {}", 5U);
    ASSERT(strstr(output, "type Empty {}\\n\\nKind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "type User: Named {", 5U);
    ASSERT(strstr(output, "type User: Named {...}\\n\\nKind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "@value type Point {", 12U);
    ASSERT(strstr(output, "type Point {...}\\n\\nKind: Value Type") != NULL);
    ASSERT(strstr(output, "@value type Point") == NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "type Unit();", 5U);
    ASSERT(strstr(output, "type Unit();\\n\\nKind: Tuple Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "type Pair<T>(T, T);", 5U);
    ASSERT(strstr(output, "type Pair<T>(T, T);\\n\\nKind: Tuple Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "type Octet(i32, i32, i32, i32, i32, i32, i32, i32);",
                                        5U);
    ASSERT(strstr(output,
                  "type Octet(i32, i32, i32, i32, i32, i32, i32, i32);\\n\\nKind: Tuple Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "spec Named {", 5U);
    ASSERT(strstr(output, "spec Named {...}\\n\\nKind: Object Spec") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "spec EmptySpec {}", 5U);
    ASSERT(strstr(output, "spec EmptySpec {}\\n\\nKind: Object Spec") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "spec Mapper<T>(value: T): string;", 5U);
    ASSERT(strstr(output,
                  "spec Mapper<T>(value: T): string;\\n\\nKind: Callback Spec") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "spec Choice: User | Point;", 5U);
    ASSERT(strstr(output,
                  "spec Choice: User | Point;\\n\\nKind: Union Spec") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource, kPlaintextInitialize, "spec Both: Named & Serializable;", 5U);
    ASSERT(strstr(output,
                  "spec Both: Named & Serializable;\\n\\nKind: Intersection Spec") != NULL);
    free(output);

    output = capture_lsp_hover_response(kEnumSource,
                                        kPlaintextInitialize,
                                        "enum Color {",
                                        strlen("enum "));
    ASSERT(strstr(output, "enum Color\\n\\nKind: Enum") != NULL);
    free(output);

    output = capture_lsp_hover_response(kEnumSource,
                                        kPlaintextInitialize,
                                        "Red = 1",
                                        1U);
    ASSERT(strstr(output, "Red = 1\\n\\nKind: Enum") != NULL);
    free(output);

    output = capture_lsp_hover_response(kEnumSource,
                                        kPlaintextInitialize,
                                        "Color.Green",
                                        strlen("Color."));
    ASSERT(strstr(output, "Green = 2\\n\\nKind: Enum") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "    let name: string;",
                                        strlen("    let "));
    ASSERT(strstr(output, "let name: string\\n\\nKind: Builtin") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "func inspect(user: User",
                                        strlen("func "));
    ASSERT(strstr(output, "func inspect(user: User") != NULL);
    ASSERT(strstr(output, "Kind:") == NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "func inspect(user: User",
                                        strlen("func inspect("));
    ASSERT(strstr(output, "param let user: User\\n\\nKind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "point: Point, pair",
                                        1U);
    ASSERT(strstr(output, "param let point: Point\\n\\nKind: Value Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "users: User[]",
                                        1U);
    ASSERT(strstr(output, "param let users: User[]\\n\\nKind: Array") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "pointer: User*",
                                        1U);
    ASSERT(strstr(output, "param let pointer: User*\\n\\nKind: Pointer") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "count: i32",
                                        1U);
    ASSERT(strstr(output, "param let count: i32\\n\\nKind: Builtin") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "var total: i32",
                                        strlen("var "));
    ASSERT(strstr(output, "param var total: i32\\n\\nKind: Builtin") != NULL);
    free(output);

    output = capture_lsp_hover_response(kEnumSource,
                                        kPlaintextInitialize,
                                        "color: Color",
                                        1U);
    ASSERT(strstr(output, "param let color: Color\\n\\nKind: Enum") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "let inferred = Point",
                                        strlen("let "));
    ASSERT(strstr(output, "let inferred: Point\\n\\nKind: Value Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kConstructorSource,
                                        kPlaintextInitialize,
                                        "let constructed = UserType();",
                                        strlen("let constructed = "));
    ASSERT(strstr(output, "ctor UserType(): void") != NULL);
    ASSERT(strstr(output, "Kind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kConstructorSource,
                                        kPlaintextInitialize,
                                        "let literal = UserType {};",
                                        strlen("let literal = "));
    ASSERT(strstr(output, "Kind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "octet.item1",
                                        strlen("octet."));
    ASSERT(strstr(output, "let item1: i32\\n\\nKind: Builtin") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kMarkdownInitialize,
                                        "spec Both: Named & Serializable;",
                                        5U);
    ASSERT(strstr(output, "```feng\\nspec Both: Named & Serializable;\\n```") != NULL);
    ASSERT(strstr(output, "**Kind:** `Intersection Spec`") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kMarkdownInitialize,
                                        "func inspect(user: User",
                                        strlen("func inspect("));
    ASSERT(strstr(output, "```feng\\nparam let user: User\\n```") != NULL);
    ASSERT(strstr(output, "**Kind:** `Reference Type`") != NULL);
    free(output);

    output = capture_lsp_hover_response(
        "module test.lsp.hover_annotations;\n"
        "@value\n"
        "@abi\n"
        "type Annotated {}\n",
        kPlaintextInitialize,
        "type Annotated {}",
        5U);
    ASSERT(strstr(output, "type Annotated {}\\n\\nKind: Value Type") != NULL);
    ASSERT(strstr(output, "@value") == NULL);
    ASSERT(strstr(output, "@abi") == NULL);
    free(output);

    output = capture_lsp_hover_response(
        "module test.lsp.hover_short_names;\n"
        "func qualified<T: external.contracts.Named>(value: external.models.User): external.models.User {\n"
        "    return value;\n"
        "}\n",
        kPlaintextInitialize,
        "func qualified<T: external.contracts.Named>",
        strlen("func "));
    ASSERT(strstr(output,
                  "func qualified<T: Named>(value: User): User") != NULL);
    ASSERT(strstr(output, "external.") == NULL);
    free(output);
}

/* Hover must resolve lambda parameter types, lambda-local names, and every
 * level of a chained member access without relying on TUI-specific symbols. */
static void test_lsp_hover_lambda_scope_and_chained_members(void) {
    static const char *kSource =
        "module test.lsp.lambda_hover;\n"
        "\n"
        "spec Handler<T>(event: T): void;\n"
        "\n"
        "spec Value: i32 | string;\n"
        "\n"
        "type Event {\n"
        "    open let value: Value;\n"
        "    open func isReady(): bool { return true; }\n"
        "}\n"
        "\n"
        "type Input {\n"
        "    open var onEvent: Handler<Event>;\n"
        "}\n"
        "\n"
        "type App {\n"
        "    open let input: Input;\n"
        "    open func exit(): void {}\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let app = App();\n"
        "    app.input.onEvent = (event: Event) {\n"
        "        if event.isReady() && event.value match let value: i32 && value == 0 {\n"
        "            app.exit();\n"
        "        }\n"
        "    };\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output;

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    app.input.onEvent = (event: Event) {",
                                        strlen("    app.input."));
    ASSERT(strstr(output, "var onEvent: Handler<Event>") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    app.input.onEvent = (event: Event) {",
                                        strlen("    app.input.onEvent = (event: "));
    ASSERT(strstr(output, "type Event") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "        if event.isReady() && event.value match let value: i32 && value == 0 {",
                                        strlen("        if "));
    ASSERT(strstr(output, "let event: Event") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "        if event.isReady() && event.value match let value: i32 && value == 0 {",
                                        strlen("        if event."));
    ASSERT(strstr(output, "func isReady(): bool") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "        if event.isReady() && event.value match let value: i32 && value == 0 {",
                                        strlen("        if event.isReady() && event."));
    ASSERT(strstr(output, "let value: Value") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "            app.exit();",
                                        strlen("            app."));
    ASSERT(strstr(output, "func exit(): void") != NULL);
    free(output);
}

/* Hover resolves a lambda parameter at both its declaration and use sites.
 * Consecutive requests share one analysis session, while didChange must
 * invalidate that session so the updated parameter is resolved. */
static void test_lsp_hover_lambda_parameter_declaration_and_cache_invalidation(void) {
    static const char *kSourceBefore =
        "module test.lsp.lambda_param_cache;\n"
        "\n"
        "spec Handler<T>(event: T): void;\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let handler: Handler<i32> = (item: i32) {\n"
        "        let copy = item;\n"
        "    };\n"
        "}\n";
    static const char *kSourceAfter =
        "module test.lsp.lambda_param_cache;\n"
        "\n"
        "spec Handler<T>(event: T): void;\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let handler: Handler<i32> = (value: i32) {\n"
        "        let copy = value;\n"
        "    };\n"
        "}\n";
    char template_path[] = "temp/feng_lsp_lambda_param_cache_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *source_uri;
    char *escaped_before;
    char *escaped_after;
    char *initialize;
    char *did_open;
    char *hover_declaration;
    char *hover_use;
    char *did_change;
    char *hover_updated_declaration;
    char *shutdown;
    char *output;
    FILE *input;
    unsigned int declaration_line;
    unsigned int declaration_character;
    unsigned int use_line;
    unsigned int use_character;
    unsigned int updated_line;
    unsigned int updated_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSourceBefore);
    source_uri = file_uri_from_path(source_path);
    escaped_before = json_escape_text(kSourceBefore);
    escaped_after = json_escape_text(kSourceAfter);

    find_line_character(kSourceBefore,
                        "(item: i32)",
                        strlen("("),
                        &declaration_line,
                        &declaration_character);
    find_line_character(kSourceBefore,
                        "let copy = item;",
                        strlen("let copy = "),
                        &use_line,
                        &use_character);
    find_line_character(kSourceAfter,
                        "(value: i32)",
                        strlen("("),
                        &updated_line,
                        &updated_character);

    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          source_uri,
                          escaped_before);
    hover_declaration = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                                   "\"method\":\"textDocument/hover\","
                                   "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                   "\"position\":{\"line\":%u,\"character\":%u}}}",
                                   source_uri,
                                   declaration_line,
                                   declaration_character);
    hover_use = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,"
                           "\"method\":\"textDocument/hover\","
                           "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                           "\"position\":{\"line\":%u,\"character\":%u}}}",
                           source_uri,
                           use_line,
                           use_character);
    did_change = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
                            "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":2},"
                            "\"contentChanges\":[{\"text\":\"%s\"}]}}",
                            source_uri,
                            escaped_after);
    hover_updated_declaration = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,"
                                           "\"method\":\"textDocument/hover\","
                                           "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                                           "\"position\":{\"line\":%u,\"character\":%u}}}",
                                           source_uri,
                                           updated_line,
                                           updated_character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = temp_file();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_declaration);
    write_lsp_message(input, hover_use);
    write_lsp_message(input, did_change);
    write_lsp_message(input, hover_updated_declaration);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output,
                  "\"id\":2,\"result\":{\"contents\":{\"kind\":\"plaintext\","
                  "\"value\":\"param let item: i32\\n\\nKind: Builtin\"}}") != NULL);
    ASSERT(strstr(output,
                  "\"id\":3,\"result\":{\"contents\":{\"kind\":\"plaintext\","
                  "\"value\":\"param let item: i32\\n\\nKind: Builtin\"}}") != NULL);
    ASSERT(strstr(output,
                  "\"id\":4,\"result\":{\"contents\":{\"kind\":\"plaintext\","
                  "\"value\":\"param let value: i32\\n\\nKind: Builtin\"}}") != NULL);

    free(output);
    free(shutdown);
    free(hover_updated_declaration);
    free(did_change);
    free(hover_use);
    free(hover_declaration);
    free(did_open);
    free(initialize);
    free(escaped_after);
    free(escaped_before);
    free(source_uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

/* A failed edit never removes the last successful Hover type category. */
static void test_lsp_hover_type_category_survives_failed_edit(void) {
    static const char *kSourceBefore =
        "module test.lsp.hover_category_cache;\n"
        "\n"
        "@value\n"
        "type Point {\n"
        "    let x: i32;\n"
        "}\n";
    static const char *kSourceAfter =
        "module test.lsp.hover_category_cache;\n"
        "\n"
        "@value\n"
        "type Point {\n"
        "    let x: i32;\n"
        "}\n"
        "\n"
        "func broken(): void {\n"
        "    foo.\n";
    char template_path[] = "temp/feng_lsp_hover_category_cache_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *source_uri;
    char *escaped_before;
    char *escaped_after;
    char *initialize;
    char *did_open;
    char *hover_before;
    char *did_change;
    char *hover_after;
    char *shutdown;
    char *output;
    FILE *input;
    FILE *capture;
    int input_pipe[2];
    pid_t writer;
    unsigned int line;
    unsigned int character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSourceBefore);
    source_uri = file_uri_from_path(source_path);
    escaped_before = json_escape_text(kSourceBefore);
    escaped_after = json_escape_text(kSourceAfter);
    find_line_character(kSourceBefore, "type Point {", strlen("type "), &line, &character);

    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          source_uri,
                          escaped_before);
    hover_before = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,"
                              "\"method\":\"textDocument/hover\","
                              "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                              "\"position\":{\"line\":%u,\"character\":%u}}}",
                              source_uri,
                              line,
                              character);
    did_change = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
                            "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":2},"
                            "\"contentChanges\":[{\"text\":\"%s\"}]}}",
                            source_uri,
                            escaped_after);
    hover_after = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,"
                             "\"method\":\"textDocument/hover\","
                             "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
                             "\"position\":{\"line\":%u,\"character\":%u}}}",
                             source_uri,
                             line,
                             character);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    ASSERT(pipe(input_pipe) == 0);
    writer = fork();
    ASSERT(writer >= 0);
    if (writer == 0) {
        close(input_pipe[0]);
        input = fdopen(input_pipe[1], "wb");
        ASSERT(input != NULL);
        write_lsp_message(input, initialize);
        write_lsp_message(input, did_open);
        write_lsp_message(input, hover_before);
        ASSERT(fflush(input) == 0);
        (void)usleep(30U * 1000U);
        write_lsp_message(input, did_change);
        write_lsp_message(input, hover_after);
        write_lsp_message(input, shutdown);
        write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
        fclose(input);
        _exit(0);
    }
    close(input_pipe[1]);
    input = fdopen(input_pipe[0], "rb");
    capture = temp_file();
    ASSERT(input != NULL && capture != NULL);
    ASSERT(feng_lsp_server_run(input, capture, stderr) == 0);
    output = read_text_stream(capture);
    fclose(capture);
    fclose(input);
    ASSERT(waitpid(writer, NULL, 0) == writer);
    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":null") == NULL);
    ASSERT(count_occurrences(output,
                             "type Point {...}\\n\\nKind: Value Type") == 2);

    free(output);
    free(shutdown);
    free(hover_after);
    free(did_change);
    free(hover_before);
    free(did_open);
    free(initialize);
    free(escaped_after);
    free(escaped_before);
    free(source_uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

/* Infix-match bindings must resolve at the declaration, in a following &&
 * operand, and inside the true branch. Multi-member patterns report the
 * narrowed subset-union type rather than the original union spec name. */
static void test_lsp_hover_infix_match_binding(void) {
    static const char *kSource =
        "module test.lsp.match_binding_hover;\n"
        "\n"
        "spec Value: i32 | string;\n"
        "\n"
        "func positive(value: Value): bool {\n"
        "    if value match let item: i32 && item > 0 {\n"
        "        return item > 1;\n"
        "    }\n"
        "    return false;\n"
        "}\n"
        "\n"
        "func present(value: Value): bool {\n"
        "    if value match let item: i32 | string {\n"
        "        let copy = item;\n"
        "        return true;\n"
        "    }\n"
        "    return false;\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output;

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    if value match let item: i32 && item > 0 {",
                                        strlen("    if value match let "));
    ASSERT(strstr(output, "let item: i32") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    if value match let item: i32 && item > 0 {",
                                        strlen("    if value match let item: i32 && "));
    ASSERT(strstr(output, "let item: i32") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "        return item > 1;",
                                        strlen("        return "));
    ASSERT(strstr(output, "let item: i32") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    if value match let item: i32 | string {",
                                        strlen("    if value match let "));
    ASSERT(strstr(output, "let item: i32 | string") != NULL);
    free(output);
}

static void test_lsp_hover_type_param(void) {
    static const char *kSource =
        "module test.lsp.type_param_hover;\n"
        "\n"
        "spec Hashable<T> {\n"
        "    func hash(): u64;\n"
        "}\n"
        "\n"
        "type Map<K: Hashable<K>, V> {\n"
        "    var count: u64;\n"
        "}\n"
        "\n"
        "type Box<T> {\n"
        "    var size: u64;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output;

    /* Hover on T in type Box<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "type Box<T> {",
                                        strlen("type Box<"));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on K in type Map<K: Hashable<K>, V>: should show "generic parameter K: Hashable<K>" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "type Map<K: Hashable<K>, V> {",
                                        strlen("type Map<"));
    ASSERT(strstr(output, "generic parameter K: Hashable<K>") != NULL);
    free(output);

    /* Hover on Hashable in K: Hashable<K>: should show Hashable spec signature */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "type Map<K: Hashable<K>, V> {",
                                        strlen("type Map<K: "));
    ASSERT(strstr(output, "spec Hashable<T>") != NULL);
    free(output);

    /* Hover on K inside Hashable<K> constraint: should show "generic parameter K" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "type Map<K: Hashable<K>, V> {",
                                        strlen("type Map<K: Hashable<"));
    ASSERT(strstr(output, "generic parameter K") != NULL);
    free(output);
}

static void test_lsp_hover_type_param_extended(void) {
    static const char *kSource =
        "module test.lsp.type_param_hover_ext;\n"
        "\n"
        "type Empty {\n"
        "}\n"
        "\n"
        "spec Option<T>: Empty | T;\n"
        "\n"
        "spec Action<T>(arg1: T): void;\n"
        "\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "}\n"
        "\n"
        "func test_fn<T>(val: T): T {\n"
        "    return val;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output;

    /* Hover on T in spec Option<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "spec Option<T>: Empty | T;",
                                        strlen("spec Option<"));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T after | in spec Option<T>: Empty | T: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "spec Option<T>: Empty | T;",
                                        strlen("spec Option<T>: Empty | "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in spec Action<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "spec Action<T>(arg1: T): void;",
                                        strlen("spec Action<"));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in arg1: T (callable spec param): should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "spec Action<T>(arg1: T): void;",
                                        strlen("spec Action<T>(arg1: "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in func test_fn<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "func test_fn<T>(val: T): T {",
                                        strlen("func test_fn<"));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in val: T (function param): should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "func test_fn<T>(val: T): T {",
                                        strlen("func test_fn<T>(val: "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in return type T (function return): should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "func test_fn<T>(val: T): T {",
                                        strlen("func test_fn<T>(val: T): "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in Box<T> field type: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    var value: T;",
                                        strlen("    var value: "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);
}

static void test_lsp_hover_type_param_in_spec_member(void) {
    static const char *kSource =
        "module test.lsp.type_param_spec_member;\n"
        "\n"
        "spec Hashable<T> {\n"
        "    func hash(): u64;\n"
        "    func same(other: T): bool;\n"
        "}\n"
        "\n"
        "type Box<T> {\n"
        "    var value: T;\n"
        "    func get(): T {\n"
        "        return self.value;\n"
        "    }\n"
        "    func set(val: T) {\n"
        "        self.value = val;\n"
        "    }\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output;

    /* Hover on T in func same(other: T) inside spec Hashable<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    func same(other: T): bool;",
                                        strlen("    func same(other: "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in func get(): T inside type Box<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    func get(): T {",
                                        strlen("    func get(): "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);

    /* Hover on T in func set(val: T) inside type Box<T>: should show "generic parameter T" */
    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    func set(val: T) {",
                                        strlen("    func set(val: "));
    ASSERT(strstr(output, "generic parameter T") != NULL);
    free(output);
}

static void test_lsp_signature_displays_variadic_parameter_syntax(void) {
    static const char *kHoverSource =
        "module test.lsp.variadic_signature_hover;\n"
        "\n"
        "func log(fmt: string, args: string...): string {\n"
        "    return fmt;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let value: string = log(\"x\", \"a\");\n"
        "}\n";
    static const char *kCompletionSource =
        "module test.lsp.variadic_signature_completion;\n"
        "\n"
        "func log(fmt: string, args: string...): string {\n"
        "    return fmt;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    lo\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *hover_output = capture_lsp_hover_response(kHoverSource,
                                                    kInitialize,
                                                    "    let value: string = log(\"x\", \"a\");",
                                                    strlen("    let value: string = "));
    char *completion_output = capture_lsp_completion_response(kCompletionSource,
                                                              "    lo\n",
                                                              strlen("    lo"));

    ASSERT(strstr(hover_output, "func log(fmt: string, args: string...): string") != NULL);
    ASSERT(strstr(hover_output, "func log(fmt: string, args: string[]): string") == NULL);
    ASSERT(strstr(completion_output, "\"label\":\"log\"") != NULL);
    ASSERT(strstr(completion_output, "\"detail\":\"func log(fmt: string, args: string...): string\"") != NULL);
    ASSERT(strstr(completion_output, "\"detail\":\"func log(fmt: string, args: string[]): string\"") == NULL);

    free(completion_output);
    free(hover_output);
}

static void test_lsp_fit_member_name_param_mutability_and_return_type_navigation(void) {
    static const char *kSource =
        "module test.lsp.fitmember;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "fit User {\n"
        "    /** Tags user with prefix. */\n"
        "    func tag(let prefix: string): User {\n"
        "        let localName: string = self.name;\n"
        "        return self;\n"
        "    }\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let tagged: User = user.tag(\"hi\");\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_fit_member_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *hover_keyword;
    char *hover_method;
    char *definition_method;
    char *hover_param;
    char *hover_self_member;
    char *hover_return_type;
    char *definition_return_type;
    char *shutdown;
    char *output;
    char *expected_method_definition;
    char *expected_return_definition;
    FILE *input;
    unsigned int method_line;
    unsigned int method_character;
    unsigned int keyword_line;
    unsigned int keyword_character;
    unsigned int param_line;
    unsigned int param_character;
    unsigned int self_member_line;
    unsigned int self_member_character;
    unsigned int return_type_line;
    unsigned int return_type_character;
    unsigned int user_decl_line;
    unsigned int user_decl_character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);

    find_line_character(kSource,
                        "    func tag(let prefix: string): User {",
                        9U,
                        &method_line,
                        &method_character);
    find_line_character(kSource,
                        "    func tag(let prefix: string): User {",
                        5U,
                        &keyword_line,
                        &keyword_character);
    find_line_character(kSource,
                        "    func tag(let prefix: string): User {",
                        strlen("    func tag(let "),
                        &param_line,
                        &param_character);
    find_line_character(kSource,
                        "        let localName: string = self.name;",
                        strlen("        let localName: string = self."),
                        &self_member_line,
                        &self_member_character);
    find_line_character(kSource,
                        "    func tag(let prefix: string): User {",
                        strlen("    func tag(let prefix: string): "),
                        &return_type_line,
                        &return_type_character);
    find_line_character(kSource,
                        "type User {",
                        5U,
                        &user_decl_line,
                        &user_decl_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
                          uri,
                          escaped_text);
    hover_keyword = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                               uri,
                               keyword_line,
                               keyword_character);
    hover_method = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                              uri,
                              method_line,
                              method_character + 1U);
    definition_method = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                   uri,
                                   method_line,
                                   method_character + 1U);
    hover_param = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                             uri,
                             param_line,
                             param_character + 2U);
    hover_self_member = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                   uri,
                                   self_member_line,
                                   self_member_character + 1U);
    hover_return_type = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                   uri,
                                   return_type_line,
                                   return_type_character + 1U);
    definition_return_type = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
                                        uri,
                                        return_type_line,
                                        return_type_character + 1U);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");
    expected_method_definition = dup_printf("\"id\":4,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u",
                                         uri,
                                         method_line);
    expected_return_definition = dup_printf("\"id\":8,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
                                         uri,
                                         user_decl_line,
                                         user_decl_character);

    input = temp_file();
    ASSERT(input != NULL);
    write_lsp_message(input, initialize);
    write_lsp_message(input, did_open);
    write_lsp_message(input, hover_keyword);
    write_lsp_message(input, hover_method);
    write_lsp_message(input, definition_method);
    write_lsp_message(input, hover_param);
    write_lsp_message(input, hover_self_member);
    write_lsp_message(input, hover_return_type);
    write_lsp_message(input, definition_return_type);
    write_lsp_message(input, shutdown);
    write_lsp_message(input, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    output = run_lsp_server_capture(input);
    fclose(input);

    ASSERT(strstr(output, "\"id\":2,\"result\":") != NULL);
    ASSERT(strstr(output, "function declaration") != NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":") != NULL);
    ASSERT(strstr(output, "func tag(prefix: string): User") != NULL);
    ASSERT(strstr(output, expected_method_definition) != NULL);
    ASSERT(strstr(output, "\"id\":5,\"result\":") != NULL);
    ASSERT(strstr(output, "let prefix: string") != NULL);
    ASSERT(strstr(output, "\"id\":6,\"result\":") != NULL);
    ASSERT(strstr(output, "let name: string") != NULL);
    ASSERT(strstr(output, "\"id\":7,\"result\":") != NULL);
    ASSERT(strstr(output, "type User") != NULL);
    ASSERT(strstr(output, expected_return_definition) != NULL);

    free(output);
    free(expected_return_definition);
    free(expected_method_definition);
    free(shutdown);
    free(definition_return_type);
    free(hover_return_type);
    free(hover_self_member);
    free(hover_param);
    free(definition_method);
    free(hover_method);
    free(hover_keyword);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
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
        "module test.lsp.completiondot;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let label: string = user.;\n"
        "}\n";
    static const char *kPrefixSource =
        "module test.lsp.completionprefix;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let label: string = user.n;\n"
        "}\n";
    static const char *kInferredSource =
        "module test.lsp.completioninferred;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user = User { name: \"copilot\" };\n"
        "    let label: string = user.;\n"
        "}\n";

    assert_lsp_completion_contains_name(kDotSource, "user.;", 5U);
    assert_lsp_completion_contains_name(kPrefixSource, "user.n;", 6U);
    assert_lsp_completion_contains_name(kInferredSource, "user.;", 5U);
}

/* Completion exposes spec-seal members only in type/fit implementation
 * methods whose implementation type satisfies the receiver spec. */
static void test_lsp_spec_seal_member_completion_respects_implementation_domain(void) {
    static const char *kTypeSource =
        "module test.lsp.specsealtype;\n"
        "spec Surface {\n"
        "    func visible(): int;\n"
        "    seal func hidden(): int;\n"
        "    seal static func hiddenStatic(): int;\n"
        "}\n"
        "type Impl: Surface {\n"
        "    func visible(): int { return 1; }\n"
        "    func hidden(): int { return 2; }\n"
        "    static func hiddenStatic(): int { return 3; }\n"
        "    func use(value: Surface): int { return value.hidden(); }\n"
        "    static func useStatic<T: Surface>(): int { return T.hiddenStatic(); }\n"
        "}\n"
        "func ordinary(value: Surface): int { return value.visible(); }\n";
    static const char *kFitSource =
        "module test.lsp.specsealfit;\n"
        "spec Surface {\n"
        "    func visible(): int;\n"
        "    seal func hidden(): int;\n"
        "}\n"
        "type Impl: Surface {\n"
        "    func visible(): int { return 1; }\n"
        "    func hidden(): int { return 2; }\n"
        "}\n"
        "fit Impl {\n"
        "    func use(value: Surface): int { return value.hidden(); }\n"
        "}\n";
    char *output = capture_lsp_completion_response(kTypeSource,
                                                   "value.hidden();",
                                                   6U);

    ASSERT(strstr(output, "\"label\":\"hidden\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"visible\"") != NULL);
    free(output);

    output = capture_lsp_completion_response(kTypeSource,
                                             "value.visible();",
                                             6U);
    ASSERT(strstr(output, "\"label\":\"visible\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"hidden\"") == NULL);
    free(output);

    output = capture_lsp_completion_response(kTypeSource,
                                             "T.hiddenStatic();",
                                             2U);
    ASSERT(strstr(output, "\"label\":\"hiddenStatic\"") != NULL);
    free(output);

    output = capture_lsp_completion_response(kFitSource,
                                             "value.hidden();",
                                             6U);
    ASSERT(strstr(output, "\"label\":\"hidden\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"visible\"") != NULL);
    free(output);
}

static void test_lsp_fit_extension_member_completion_on_builtin_string(void) {
    static const char *kBindingSource =
        "module test.lsp.fitstringbinding;\n"
        "\n"
        "fit string {\n"
        "    open func ext_len(): i64 {\n"
        "        return (i64)0;\n"
        "    }\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let s: string = \"123\";\n"
        "    let n = s.;\n"
        "}\n";
    static const char *kLiteralSource =
        "module test.lsp.fitstringliteral;\n"
        "\n"
        "fit string {\n"
        "    open func ext_len(): i64 {\n"
        "        return (i64)0;\n"
        "    }\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let n = \"123\".;\n"
        "}\n";
    const char *labels[] = {"ext_len"};

    assert_lsp_completion_contains_labels(kBindingSource, "s.;", 2U, labels, 1U);
    assert_lsp_completion_contains_labels(kLiteralSource, "\"123\".;", 6U, labels, 1U);
}

static void test_lsp_enum_member_completion_survives_incomplete_member_access(void) {
    static const char *kDotSource =
        "module test.lsp.enumcompletiondot;\n"
        "\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let status: HttpStatus = HttpStatus.;\n"
        "}\n";
    static const char *kPrefixSource =
        "module test.lsp.enumcompletionprefix;\n"
        "\n"
        "enum HttpStatus {\n"
        "    Ok = 200,\n"
        "    NotFound = 404\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let status: HttpStatus = HttpStatus.N;\n"
        "}\n";
    const char *labels[] = {"Ok", "NotFound"};
    const char *prefix_labels[] = {"NotFound"};

    assert_lsp_completion_contains_labels(kDotSource, "HttpStatus.;", 11U, labels, 2U);
    assert_lsp_completion_contains_labels(kPrefixSource, "HttpStatus.N;", 12U, prefix_labels, 1U);
}

static void test_lsp_completion_uses_source_scoped_edit_context(void) {
    static const char *kMemberBeforeNextStmt =
        "module test.lsp.completioneditmember;\n"
        "\n"
        "extern func puts(msg: string*): int;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "    let age: i32;\n"
        "\n"
        "    func say(msg: string): void {\n"
        "        puts(&msg);\n"
        "    }\n"
        "}\n"
        "\n"
        "func hello_world_example(args: string[]): void {\n"
        "    let user = User {\n"
        "        name: \"Houfeng\",\n"
        "        age: 18\n"
        "    };\n"
        "    user.\n"
        "    user.say(\"Hello World: \" + user.name);\n"
        "}\n";
    static const char *kScopeBeforeClose =
        "module test.lsp.completioneditscope;\n"
        "\n"
        "extern func puts(msg: string*): int;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "    let age: i32;\n"
        "\n"
        "    func say(msg: string): void {\n"
        "        puts(&msg);\n"
        "    }\n"
        "}\n"
        "\n"
        "func hello_world_example(args: string[]): void {\n"
        "    let user = User {\n"
        "        name: \"Houfeng\",\n"
        "        age: 18\n"
        "    };\n"
        "    user.say(\"Hello World: \" + user.name);\n"
        "    \n"
        "}\n";
    static const char *kScopePrefixBeforeNextStmt =
        "module test.lsp.completioneditprefix;\n"
        "\n"
        "extern func puts(msg: string*): int;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func hello_world_example(args: string[]): void {\n"
        "    let user = User { name: \"Houfeng\" };\n"
        "    us\n"
        "    user.say(\"Hello World\");\n"
        "}\n";
    static const char *kMainPrefixBeforeClose =
        "module test.lsp.completionmainprefix;\n"
        "\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(msg: string*): int;\n"
        "\n"
        "func main(args: string[]) {\n"
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
        "module test.lsp.completionoverload;\n"
        "\n"
        "spec CommitOptions {\n"
        "    var message: i32;\n"
        "}\n"
        "\n"
        "type User {\n"
        "    func commit(options: CommitOptions): void {\n"
        "        options.message = 1;\n"
        "    }\n"
        "\n"
        "    func commit(message: i32): int {\n"
        "        return message;\n"
        "    }\n"
        "}\n"
        "\n"
        "func debug_example(args: string[]): void {\n"
        "    let user = User();\n"
        "    user.co\n"
        "}\n";
    char *output = capture_lsp_completion_response(kSource, "user.co", 7U);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"label\":\"commit\"") != NULL);
    ASSERT(strstr(output, "func commit(options: CommitOptions): void") != NULL);

    free(output);
}

static void test_lsp_member_references_and_rename_from_object_literal_field(void) {
    static const char *kSource =
        "module test.lsp.rename;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_member_rename_XXXXXX";
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

    input = temp_file();
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
        "module test.lsp.declsite;\n"
        "\n"
        "func helper(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    helper(1);\n"
        "    helper(2);\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_decl_site_XXXXXX";
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
                        "func helper(x: int): int {",
                        5U,
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

    input = temp_file();
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

/* Verifies cross-file type and enum-item references and rename edits across
 * signatures, generic arguments, array construction, and match labels. */
static void test_lsp_type_references_cover_all_ast_positions(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"lsp_type_references\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n";
    static const char *kDeclarationSource =
        "open module test.lsp.type_references;\n"
        "\n"
        "open enum Style {\n"
        "    none = 0,\n"
        "    bold = 1\n"
        "}\n"
        "\n"
        "open type Box<T> {}\n"
        "\n"
        "open func identity<T>(value: T): T {\n"
        "    return value;\n"
        "}\n";
    static const char *kUsageSource =
        "open module test.lsp.type_references;\n"
        "\n"
        "open spec StyleHandler(value: Style): Style;\n"
        "\n"
        "open type Buffer {\n"
        "    func styleToBits(s: Style): int {\n"
        "        return match s {\n"
        "            Style.none { 0; }\n"
        "            Style.bold { 1; }\n"
        "            else { 0; }\n"
        "        };\n"
        "    }\n"
        "\n"
        "    func combine(styles: Style[]): void {}\n"
        "\n"
        "    func nested(value: Box<Style>): void {}\n"
        "\n"
        "    func build(value: Style): Style {\n"
        "        let copy = identity<Style>(value);\n"
        "        let items: Style[!] = Style[:1];\n"
        "        return copy;\n"
        "    }\n"
        "}\n";
    static const char *kUsageNeedles[] = {
        "value: Style): Style;",
        "func styleToBits(s: Style): int",
        "Style.none { 0; }",
        "func nested(value: Box<Style>): void",
        "let copy = identity<Style>(value);",
        "let items: Style[!] = Style[:1];"
    };
    char template_path[] = "temp/feng_cli_lsp_type_refs_XXXXXX";
    char *workspace_dir;
    char *manifest_path;
    char *src_dir;
    char *declaration_path;
    char *usage_path;
    char *declaration_uri;
    char *usage_uri;
    char *escaped_declaration;
    char *escaped_usage;
    char *initialize;
    char *did_open;
    char *did_open_usage;
    char *type_references;
    char *type_prepare_rename;
    char *type_rename;
    char *item_references;
    char *item_prepare_rename;
    char *item_rename;
    char *item_use_references;
    char *item_use_prepare_rename;
    char *item_use_rename;
    char *shutdown;
    char *output;
    const char *requests[11];
    unsigned int declaration_line;
    unsigned int declaration_character;
    unsigned int item_declaration_line;
    unsigned int item_declaration_character;
    unsigned int item_use_line;
    unsigned int item_use_character;
    size_t index;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    manifest_path = path_join(workspace_dir, "feng.fm");
    src_dir = path_join(workspace_dir, "src");
    declaration_path = path_join(src_dir, "declaration.ff");
    usage_path = path_join(src_dir, "usage.ff");
    mkdir_p(src_dir);
    write_text_file(manifest_path, kManifest);
    write_text_file(declaration_path, kDeclarationSource);
    write_text_file(usage_path, kUsageSource);

    find_line_character(kDeclarationSource,
                        "open enum Style {",
                        strlen("open enum "),
                        &declaration_line,
                        &declaration_character);
    find_line_character(kDeclarationSource,
                        "    none = 0,",
                        strlen("    "),
                        &item_declaration_line,
                        &item_declaration_character);
    find_line_character(kUsageSource,
                        "            Style.none { 0; }",
                        strlen("            Style."),
                        &item_use_line,
                        &item_use_character);
    declaration_uri = file_uri_from_path(declaration_path);
    usage_uri = file_uri_from_path(usage_path);
    escaped_declaration = json_escape_text(kDeclarationSource);
    escaped_usage = json_escape_text(kUsageSource);
    initialize = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
        "\"version\":1,\"text\":\"%s\"}}}",
        declaration_uri,
        escaped_declaration);
    did_open_usage = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
        "\"version\":1,\"text\":\"%s\"}}}",
        usage_uri,
        escaped_usage);
    type_references = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/references\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u},"
        "\"context\":{\"includeDeclaration\":true}}}",
        declaration_uri,
        declaration_line,
        declaration_character);
    type_prepare_rename = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/prepareRename\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u}}}",
        declaration_uri,
        declaration_line,
        declaration_character);
    type_rename = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/rename\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u},"
        "\"newName\":\"TextStyle\"}}",
        declaration_uri,
        declaration_line,
        declaration_character);
    item_references = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/references\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u},"
        "\"context\":{\"includeDeclaration\":true}}}",
        declaration_uri,
        item_declaration_line,
        item_declaration_character);
    item_prepare_rename = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/prepareRename\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u}}}",
        declaration_uri,
        item_declaration_line,
        item_declaration_character);
    item_rename = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/rename\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u},"
        "\"newName\":\"plain\"}}",
        declaration_uri,
        item_declaration_line,
        item_declaration_character);
    item_use_references = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/references\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u},"
        "\"context\":{\"includeDeclaration\":true}}}",
        usage_uri,
        item_use_line,
        item_use_character);
    item_use_prepare_rename = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"textDocument/prepareRename\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u}}}",
        usage_uri,
        item_use_line,
        item_use_character);
    item_use_rename = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"textDocument/rename\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%u,\"character\":%u},"
        "\"newName\":\"empty\"}}",
        usage_uri,
        item_use_line,
        item_use_character);
    shutdown = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"shutdown\",\"params\":null}");
    requests[0] = type_references;
    requests[1] = type_prepare_rename;
    requests[2] = type_rename;
    requests[3] = item_references;
    requests[4] = item_prepare_rename;
    requests[5] = item_rename;
    requests[6] = item_use_references;
    requests[7] = item_use_prepare_rename;
    requests[8] = item_use_rename;
    requests[9] = shutdown;
    requests[10] = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";

    output = run_lsp_server_capture_after_position_ready(initialize,
                                                         did_open,
                                                         did_open_usage,
                                                         "textDocument/references",
                                                         declaration_uri,
                                                         declaration_line,
                                                         declaration_character,
                                                         usage_uri,
                                                         requests,
                                                         11U,
                                                         NULL);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":{\"range\":") != NULL);
    ASSERT(count_occurrences(output, "\"placeholder\":\"Style\"") == 1);
    ASSERT(count_occurrences(output, "\"newText\":\"TextStyle\"") == 13);
    ASSERT(strstr(output, "\"id\":5,\"result\":[{\"uri\":") != NULL);
    ASSERT(strstr(output, "\"id\":6,\"result\":{\"range\":") != NULL);
    ASSERT(count_occurrences(output, "\"newText\":\"plain\"") == 2);
    ASSERT(strstr(output, "\"id\":8,\"result\":[{\"uri\":") != NULL);
    ASSERT(strstr(output, "\"id\":10,\"result\":{\"range\":") != NULL);
    ASSERT(count_occurrences(output, "\"placeholder\":\"none\"") == 2);
    ASSERT(count_occurrences(output, "\"newText\":\"empty\"") == 2);
    for (index = 0U; index < sizeof(kUsageNeedles) / sizeof(kUsageNeedles[0]); ++index) {
        const char *style = strstr(kUsageNeedles[index], "Style");
        unsigned int line;
        unsigned int character;
        char *expected;

        ASSERT(style != NULL);
        find_line_character(kUsageSource,
                            kUsageNeedles[index],
                            (size_t)(style - kUsageNeedles[index]),
                            &line,
                            &character);
        expected = dup_printf(
            "\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
            usage_uri,
            line,
            character);
        ASSERT(strstr(output, expected) != NULL);
        free(expected);
    }

    free(output);
    free(shutdown);
    free(item_use_rename);
    free(item_use_prepare_rename);
    free(item_use_references);
    free(item_rename);
    free(item_prepare_rename);
    free(item_references);
    free(type_rename);
    free(type_prepare_rename);
    free(type_references);
    free(did_open_usage);
    free(did_open);
    free(initialize);
    free(escaped_usage);
    free(escaped_declaration);
    free(usage_uri);
    free(declaration_uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(usage_path);
    free(declaration_path);
    free(src_dir);
    free(manifest_path);
}

static void test_lsp_rename_accepts_identifier_end_position(void) {
    static const char *kSource =
        "module test.lsp.renameend;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_rename_end_XXXXXX";
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

    input = temp_file();
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
        "module test.lsp.broken;\n"
        "\n"
        "func helper(x: int): int {\n"
        "    return x + 1;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    helper(1);\n"
        "    helper(2);\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_broken_XXXXXX";
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
                        "func helper(x: int): int {",
                        5U,
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

    input = temp_file();
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
        "module test.lsp.libonly;\n"
        "\n"
        "/** A counter type with no main function. */\n"
        "open type Counter {\n"
        "    /** The count field. */\n"
        "    open let count: int;\n"
        "    /** Returns double the count. */\n"
        "    open func double(): int {\n"
        "        return self.count * 2;\n"
        "    }\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_libonly_XXXXXX";
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

    find_line_character(kSource, "    open let count: int;", 11U,
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

    input = temp_file();
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
    char template_path[] = "temp/feng_cli_lsp_unicode_XXXXXX";
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
        "module test.lsp.unicode;\\n"
        "/** \\u6d4b\\u8bd5 */\\n"
        "open type Tag { open let name: string; }\\n";

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);

    source_path = path_join(workspace_dir, "tag.ff");
    /* Write the actual source to disk so the LSP can find it; the on-disk
       version uses real UTF-8, the in-message version uses \\uXXXX. */
    write_text_file(source_path,
                    "module test.lsp.unicode;\n"
                    "/** \xe6\xb5\x8b\xe8\xaf\x95 */\n"
                    "open type Tag { open let name: string; }\n");

    uri = file_uri_from_path(source_path);

    initialize = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    /* didOpen text uses \\uXXXX escapes — the server must handle them. */
    did_open = dup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                          "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\","
                          "\"version\":1,\"text\":\"%s\"}}}",
                          uri, kSourceEscaped);
    shutdown = dup_printf("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    input = temp_file();
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
        "open module test.cli.cachedep;\n"
        "\n"
        "/** User from cache. */\n"
        "open type User {\n"
        "    /** Display name. */\n"
        "    let name: string;\n"
        "}\n";
    static const char *kBrokenSharedSource =
        "open module test.cli.cachedep;\n"
        "\n"
        "open type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "open func broken(user: User): string {\n";
    static const char *kMainSource =
        "module test.cli.cachemain;\n"
        "import test.cli.cachedep;\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"copilot\" };\n"
        "    let mirror: string = user.name;\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_cache_XXXXXX";
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
    const char *requests[6];
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

    requests[0] = hover_type;
    requests[1] = definition_type;
    requests[2] = hover_field;
    requests[3] = completion_field;
    requests[4] = shutdown;
    requests[5] = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";
    output = run_lsp_server_capture_after_position_ready(initialize,
                                                         did_open,
                                                         NULL,
                                                         "textDocument/hover",
                                                         main_uri,
                                                         type_line,
                                                         type_character,
                                                         "User from cache.",
                                                         requests,
                                                         6U,
                                                         NULL);

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

/* Verify manifest platform sets preserve order and reject invalid duplicates. */
static void test_manifest_parses_writes_and_validates_platform_set(void) {
    static const char *kManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "platform: \"linux-x64-musl,macos-arm64\"\n";
    static const char *kDuplicateManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "platform: \"macos-arm64,macos-arm64\"\n";
    static const char *kIncompleteManifest =
        "[package]\n"
        "name: \"demo\"\n"
        "version: \"0.1.0\"\n"
        "target: \"lib\"\n"
        "platform: \"linux-x64\"\n";
    char template_path[] = "temp/feng_cli_manifest_platform_XXXXXX";
    char *workspace_dir;
    char *manifest_path;
    char *written_text;
    char *write_error = NULL;
    char *remove_error = NULL;
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    manifest_path = path_join(workspace_dir, "feng.fm");

    ASSERT(feng_cli_project_manifest_parse(
        manifest_path,
        kManifest,
        &manifest,
        &error));
    ASSERT(manifest.platform_count == 2U);
    ASSERT(strcmp(manifest.platforms[0], "linux-x64-musl") == 0);
    ASSERT(strcmp(manifest.platforms[1], "macos-arm64") == 0);
    ASSERT(feng_cli_project_manifest_write(
        manifest_path,
        &manifest,
        &write_error));
    ASSERT(write_error == NULL);
    written_text = read_text_file(manifest_path);
    ASSERT(strstr(
               written_text,
               "platform: \"linux-x64-musl,macos-arm64\"\n") != NULL);
    free(written_text);
    feng_cli_project_manifest_dispose(&manifest);

    ASSERT(!feng_cli_project_manifest_parse(
        manifest_path,
        kDuplicateManifest,
        &manifest,
        &error));
    ASSERT(strstr(error.message, "duplicate platform") != NULL);
    feng_cli_project_error_dispose(&error);
    ASSERT(!feng_cli_project_manifest_parse(
        manifest_path,
        kIncompleteManifest,
        &manifest,
        &error));
    ASSERT(strstr(error.message, "invalid platform") != NULL);

    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(write_error);
    free(manifest_path);
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
    char output_path[] = "temp/feng_manifest_assets_XXXXXX";
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
    char template_path[] = "temp/feng_cli_project_XXXXXX";
    char *project_dir;
    char *src_dir;
    char *nested_dir;
    char *manifest_path;
    char *main_path;
    char *helper_path;
    char *host_platform = NULL;
    char *binary_path = NULL;
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
    write_text_file(main_path, "module demo.main;\nfunc main(args: string[]) {}\n");
    write_text_file(helper_path, "module demo.main;\nfunc helper(): int { return 1; }\n");

    ASSERT(feng_cli_project_open(project_dir, &context, &error));
    ASSERT(strcmp(context.manifest.name, "demo") == 0);
    ASSERT(strcmp(context.manifest.version, "0.1.0") == 0);
    ASSERT(context.source_count == 2U);
    ASSERT(strstr(context.out_root, "/dist") != NULL);
    ASSERT(feng_platform_detect_host_platform(&host_platform, NULL));
    binary_path = feng_cli_project_platform_binary_path(&context, host_platform);
    ASSERT(binary_path != NULL);
    ASSERT(strstr(binary_path, "/dist/") != NULL);
    ASSERT(strstr(binary_path, "/bin/demo") != NULL);
    ASSERT(strstr(context.package_path, "/dist/pkg/demo-0.1.0.fb") != NULL);
    ASSERT(strcmp(context.source_paths[0], context.source_paths[1]) < 0);
    ASSERT((path_ends_with(context.source_paths[0], "/src/main.ff")
            && path_ends_with(context.source_paths[1], "/src/nested/helper.ff"))
           || (path_ends_with(context.source_paths[0], "/src/nested/helper.ff")
               && path_ends_with(context.source_paths[1], "/src/main.ff")));

    feng_cli_project_context_dispose(&context);
    free(binary_path);
    free(host_platform);
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
        "platform: \"macos-arm64\"\n"
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
    ASSERT(manifest.platform_count == 1U);
    ASSERT(strcmp(manifest.platforms[0], "macos-arm64") == 0);
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
        "platform: \"macos-arm64\"\n"
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
        "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_remote_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(bundle_a,
                                      "[package]\n"
                                      "name: \"dep_a\"\n"
                                      "version: \"1.0.0\"\n"
                                      "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_local_XXXXXX";
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
    expected_bundle_path = path_join(dep_dir, "build/pkg/local_dep-0.1.0.fb");

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
                    "module local.dep;\n"
                    "open func value(): int { return 1; }\n");
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

/* Verify graph growth preserves a local project's resolved transitive dependencies. */
static void test_deps_resolve_preserves_nested_local_project_dependencies(void) {
    char template_path[] = "temp/feng_cli_deps_nested_local_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *parent_dir;
    char *parent_src_dir;
    char *leaf_dir;
    char *leaf_src_dir;
    char *project_manifest_path;
    char *parent_manifest_path;
    char *parent_source_path;
    char *leaf_manifest_path;
    char *leaf_source_path;
    char *parent_bundle_path;
    char *leaf_bundle_path;
    char *resolved_parent_bundle_path = NULL;
    char *resolved_leaf_bundle_path = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    parent_dir = path_join(workspace_dir, "local_parent");
    parent_src_dir = path_join(parent_dir, "src");
    leaf_dir = path_join(workspace_dir, "local_leaf");
    leaf_src_dir = path_join(leaf_dir, "src");
    project_manifest_path = path_join(project_dir, "feng.fm");
    parent_manifest_path = path_join(parent_dir, "feng.fm");
    parent_source_path = path_join(parent_src_dir, "lib.ff");
    leaf_manifest_path = path_join(leaf_dir, "feng.fm");
    leaf_source_path = path_join(leaf_src_dir, "lib.ff");
    parent_bundle_path = path_join(parent_dir,
                                   "build/pkg/local_parent-0.1.0.fb");
    leaf_bundle_path = path_join(leaf_dir,
                                 "build/pkg/local_leaf-0.1.0.fb");

    mkdir_p(project_dir);
    mkdir_p(parent_src_dir);
    mkdir_p(leaf_src_dir);
    write_text_file(leaf_manifest_path,
                    "[package]\n"
                    "name: \"local_leaf\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(leaf_source_path,
                    "module local.leaf;\n"
                    "open func leaf_value(): int { return 1; }\n");
    write_text_file(parent_manifest_path,
                    "[package]\n"
                    "name: \"local_parent\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_leaf: \"../local_leaf\"\n");
    write_text_file(parent_source_path,
                    "module local.parent;\n"
                    "open func parent_value(): int { return 2; }\n");
    write_text_file(project_manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_parent: \"../local_parent\"\n");

    ASSERT(feng_cli_deps_resolve_for_manifest("feng",
                                              project_manifest_path,
                                              false,
                                              false,
                                              &resolved,
                                              &error));
    ASSERT(resolved.package_count == 2U);
    resolved_parent_bundle_path = realpath(parent_bundle_path, NULL);
    resolved_leaf_bundle_path = realpath(leaf_bundle_path, NULL);
    ASSERT(resolved_parent_bundle_path != NULL);
    ASSERT(resolved_leaf_bundle_path != NULL);
    ASSERT((strcmp(resolved.package_paths[0], resolved_parent_bundle_path) == 0 &&
            strcmp(resolved.package_paths[1], resolved_leaf_bundle_path) == 0) ||
           (strcmp(resolved.package_paths[1], resolved_parent_bundle_path) == 0 &&
            strcmp(resolved.package_paths[0], resolved_leaf_bundle_path) == 0));

    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(resolved_leaf_bundle_path);
    free(resolved_parent_bundle_path);
    free(leaf_bundle_path);
    free(parent_bundle_path);
    free(leaf_source_path);
    free(leaf_manifest_path);
    free(parent_source_path);
    free(parent_manifest_path);
    free(project_manifest_path);
    free(leaf_src_dir);
    free(leaf_dir);
    free(parent_src_dir);
    free(parent_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

static void test_deps_resolve_requires_registry_for_remote_dependency(void) {
    char template_path[] = "temp/feng_cli_deps_no_registry_XXXXXX";
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
    char template_path[] = "temp/feng_cli_deps_global_registry_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_conflict_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(common_v2_path,
                                      "[package]\n"
                                      "name: \"common\"\n"
                                      "version: \"2.0.0\"\n"
                                      "platform: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(dep_a_path,
                                      "[package]\n"
                                      "name: \"dep_a\"\n"
                                      "version: \"1.0.0\"\n"
                                      "platform: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "common: \"1.0.0\"\n");
    write_manifest_only_bundle_or_die(dep_b_path,
                                      "[package]\n"
                                      "name: \"dep_b\"\n"
                                      "version: \"1.0.0\"\n"
                                      "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_cycle_XXXXXX";
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
                    "open module test.cli.cyclea;\n"
                    "open func value(): int {\n"
                    "  return 1;\n"
                    "}\n");
    write_text_file(dep_b_source_path,
                    "open module test.cli.cycleb;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_deps_add_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_add_local_XXXXXX";
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
    dep_bundle_path = path_join(dep_dir, "build/pkg/local_dep-0.1.0.fb");

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
                    "open module test.cli.addlocal;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_deps_add_local_name_XXXXXX";
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
                    "open module test.cli.addlocalname;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_deps_add_local_target_XXXXXX";
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
                    "module test.cli.addlocaltarget;\n"
                    "func main(args: string[]) {}\n");

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
    char template_path[] = "temp/feng_cli_deps_remove_XXXXXX";
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
    char template_path[] = "temp/feng_cli_deps_install_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_install_missing_bundle_XXXXXX";
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
    char template_path[] = "temp/feng_cli_deps_install_invalid_bundle_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
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
    char template_path[] = "temp/feng_cli_deps_add_local_bundle_error_XXXXXX";
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
    char template_path[] = "temp/feng_cli_deps_install_local_error_XXXXXX";
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
    char template_path[] = "temp/feng_cli_deps_install_cache_prefix_XXXXXX";
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
    char template_path[] = "temp/feng_cli_deps_install_force_XXXXXX";
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
                                      "platform: \"macos-arm64\"\n"
                                      "abi: \"feng\"\n");
    assert_zip_ok(feng_zip_writer_open(registry_bundle_path, &writer, &zip_error), &zip_error);
    assert_zip_ok(feng_zip_writer_add_bytes(&writer,
                                            "feng.fm",
                                            "[package]\n"
                                            "name: \"remote_dep\"\n"
                                            "version: \"1.0.0\"\n"
                                            "platform: \"macos-arm64\"\n"
                                            "abi: \"feng\"\n",
                                            strlen("[package]\n"
                                                   "name: \"remote_dep\"\n"
                                                   "version: \"1.0.0\"\n"
                                                   "platform: \"macos-arm64\"\n"
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

/* Verify no-registry installation and recursive dependencies use bundled packages. */
static void test_deps_install_uses_bundled_packages_recursively(void) {
    char template_path[] = "temp/feng_cli_deps_bundled_recursive_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *bundled_root;
    char *bundled_parent;
    char *bundled_leaf;
    char *parent_cache;
    char *leaf_cache;
    char *saved_home = NULL;
    char *resolve_error = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");
    parent_cache = path_join(workspace_dir,
                             ".feng/cache/bundled_recursive_parent-1.0.0.fb");
    leaf_cache = path_join(workspace_dir,
                           ".feng/cache/bundled_recursive_leaf-2.0.0.fb");
    bundled_root = feng_cli_resolve_install_path("feng", "pkg", &resolve_error);
    ASSERT(bundled_root != NULL);
    ASSERT(resolve_error == NULL);
    bundled_parent = bundled_package_path("bundled_recursive_parent-1.0.0.fb");
    bundled_leaf = bundled_package_path("bundled_recursive_leaf-2.0.0.fb");

    mkdir_p(project_dir);
    mkdir_p(bundled_root);
    ASSERT(unlink(bundled_parent) == 0 || errno == ENOENT);
    ASSERT(unlink(bundled_leaf) == 0 || errno == ENOENT);
    write_manifest_only_bundle_or_die(
        bundled_leaf,
        "[package]\n"
        "name: \"bundled_recursive_leaf\"\n"
        "version: \"2.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(
        bundled_parent,
        "[package]\n"
        "name: \"bundled_recursive_parent\"\n"
        "version: \"1.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n"
        "\n"
        "[dependencies]\n"
        "bundled_recursive_leaf: \"2.0.0\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "bundled_recursive_parent: \"1.0.0\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);
    ASSERT(feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    ASSERT(path_exists(parent_cache));
    ASSERT(path_exists(leaf_cache));
    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    ASSERT(unlink(bundled_parent) == 0);
    ASSERT(unlink(bundled_leaf) == 0);
    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(leaf_cache);
    free(parent_cache);
    free(bundled_leaf);
    free(bundled_parent);
    free(bundled_root);
    free(manifest_path);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

/* Verify registry precedence and a definite registry miss falling back to pkg/. */
static void test_deps_install_selects_registry_before_bundled_fallback(void) {
    char template_path[] = "temp/feng_cli_deps_bundled_precedence_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *registry_dir;
    char *packages_dir;
    char *manifest_path;
    char *registry_preferred;
    char *bundled_preferred;
    char *bundled_fallback;
    char *preferred_cache;
    char *fallback_cache;
    char *bundled_root;
    char *saved_home = NULL;
    char *resolve_error = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};
    static const char *kPreferredManifest =
        "[package]\n"
        "name: \"bundled_precedence_dep\"\n"
        "version: \"1.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n";

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    registry_dir = path_join(workspace_dir, "registry");
    packages_dir = path_join(registry_dir, "packages");
    manifest_path = path_join(project_dir, "feng.fm");
    registry_preferred = path_join(packages_dir,
                                   "bundled_precedence_dep-1.0.0.fb");
    bundled_preferred = bundled_package_path("bundled_precedence_dep-1.0.0.fb");
    bundled_fallback = bundled_package_path("bundled_registry_fallback-2.0.0.fb");
    preferred_cache = path_join(workspace_dir,
                                ".feng/cache/bundled_precedence_dep-1.0.0.fb");
    fallback_cache = path_join(workspace_dir,
                               ".feng/cache/bundled_registry_fallback-2.0.0.fb");
    bundled_root = feng_cli_resolve_install_path("feng", "pkg", &resolve_error);
    ASSERT(bundled_root != NULL);
    ASSERT(resolve_error == NULL);

    mkdir_p(project_dir);
    mkdir_p(packages_dir);
    mkdir_p(bundled_root);
    ASSERT(unlink(bundled_preferred) == 0 || errno == ENOENT);
    ASSERT(unlink(bundled_fallback) == 0 || errno == ENOENT);
    write_marked_manifest_bundle_or_die(registry_preferred,
                                        kPreferredManifest,
                                        "registry");
    write_marked_manifest_bundle_or_die(bundled_preferred,
                                        kPreferredManifest,
                                        "bundled");
    write_marked_manifest_bundle_or_die(
        bundled_fallback,
        "[package]\n"
        "name: \"bundled_registry_fallback\"\n"
        "version: \"2.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n",
        "fallback");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "bundled_precedence_dep: \"1.0.0\"\n"
                    "bundled_registry_fallback: \"2.0.0\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"../registry\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);
    ASSERT(feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    assert_bundle_source_marker(preferred_cache, "registry");
    assert_bundle_source_marker(fallback_cache, "fallback");
    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    ASSERT(unlink(bundled_preferred) == 0);
    ASSERT(unlink(bundled_fallback) == 0);
    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundled_root);
    free(fallback_cache);
    free(preferred_cache);
    free(bundled_fallback);
    free(bundled_preferred);
    free(registry_preferred);
    free(manifest_path);
    free(packages_dir);
    free(registry_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

/* Verify an HTTP 404 is treated as absence and may use the bundled fallback. */
static void test_deps_install_http_404_uses_bundled_fallback(void) {
    char template_path[] = "temp/feng_cli_deps_bundled_http_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *mock_bin_dir;
    char *mock_curl;
    char *manifest_path;
    char *bundled_root;
    char *bundled_path;
    char *cache_path;
    char *saved_home = NULL;
    char *saved_path = NULL;
    char *mock_path;
    char *resolve_error = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    mock_bin_dir = path_join(workspace_dir, "mock-bin");
    mock_curl = path_join(mock_bin_dir, "curl");
    manifest_path = path_join(project_dir, "feng.fm");
    bundled_root = feng_cli_resolve_install_path("feng", "pkg", &resolve_error);
    ASSERT(bundled_root != NULL);
    ASSERT(resolve_error == NULL);
    bundled_path = bundled_package_path("bundled_http_fallback-1.0.0.fb");
    cache_path = path_join(workspace_dir,
                           ".feng/cache/bundled_http_fallback-1.0.0.fb");

    mkdir_p(project_dir);
    mkdir_p(mock_bin_dir);
    mkdir_p(bundled_root);
    ASSERT(unlink(bundled_path) == 0 || errno == ENOENT);
    write_executable_text_file(
        mock_curl,
        "#!/bin/sh\n"
        "output=\"\"\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = \"-o\" ]; then\n"
        "    shift\n"
        "    output=\"$1\"\n"
        "  fi\n"
        "  shift\n"
        "done\n"
        ": > \"$output\"\n"
        "printf '\\nFENG_HTTP_STATUS:404\\n'\n"
        "exit 22\n");
    write_manifest_only_bundle_or_die(
        bundled_path,
        "[package]\n"
        "name: \"bundled_http_fallback\"\n"
        "version: \"1.0.0\"\n"
        "platform: \"macos-arm64\"\n"
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
                    "bundled_http_fallback: \"1.0.0\"\n"
                    "\n"
                    "[registry]\n"
                    "url: \"https://registry.example/feng\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    if (getenv("PATH") != NULL) {
        saved_path = dup_cstr(getenv("PATH"));
    }
    mock_path = dup_printf("%s:%s",
                           mock_bin_dir,
                           saved_path != NULL ? saved_path : "");
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);
    ASSERT(setenv("PATH", mock_path, 1) == 0);
    ASSERT(feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    ASSERT(path_exists(cache_path));
    if (saved_path != NULL) {
        ASSERT(setenv("PATH", saved_path, 1) == 0);
    } else {
        ASSERT(unsetenv("PATH") == 0);
    }
    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    ASSERT(unlink(bundled_path) == 0);
    free(mock_path);
    free(saved_path);
    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(bundled_path);
    free(bundled_root);
    free(manifest_path);
    free(mock_curl);
    free(mock_bin_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

/* Verify invalid bundled packages never publish and --force refreshes from pkg/. */
static void test_deps_install_validates_and_force_refreshes_bundled_package(void) {
    char template_path[] = "temp/feng_cli_deps_bundled_force_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *invalid_bundled;
    char *force_bundled;
    char *invalid_cache;
    char *force_cache;
    char *cache_root;
    char *bundled_root;
    char *saved_home = NULL;
    char *resolve_error = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};
    static const char *kForceManifest =
        "[package]\n"
        "name: \"bundled_force_dep\"\n"
        "version: \"1.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n";

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    manifest_path = path_join(project_dir, "feng.fm");
    invalid_bundled = bundled_package_path("bundled_invalid_dep-1.0.0.fb");
    force_bundled = bundled_package_path("bundled_force_dep-1.0.0.fb");
    invalid_cache = path_join(workspace_dir,
                              ".feng/cache/bundled_invalid_dep-1.0.0.fb");
    force_cache = path_join(workspace_dir,
                            ".feng/cache/bundled_force_dep-1.0.0.fb");
    cache_root = path_join(workspace_dir, ".feng/cache");
    bundled_root = feng_cli_resolve_install_path("feng", "pkg", &resolve_error);
    ASSERT(bundled_root != NULL);
    ASSERT(resolve_error == NULL);

    mkdir_p(project_dir);
    mkdir_p(cache_root);
    mkdir_p(bundled_root);
    ASSERT(unlink(invalid_bundled) == 0 || errno == ENOENT);
    ASSERT(unlink(force_bundled) == 0 || errno == ENOENT);
    write_manifest_only_bundle_or_die(
        invalid_bundled,
        "[package]\n"
        "name: \"other_dep\"\n"
        "version: \"1.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n");
    write_marked_manifest_bundle_or_die(force_cache, kForceManifest, "cached");
    write_marked_manifest_bundle_or_die(force_bundled, kForceManifest, "bundled");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "bundled_invalid_dep: \"1.0.0\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);
    ASSERT(!feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    ASSERT(error.message != NULL);
    ASSERT(strstr(error.message, "bundled_invalid_dep@1.0.0") != NULL);
    ASSERT(strstr(error.message, invalid_bundled) != NULL);
    ASSERT(strstr(error.message, "dependency name mismatch") != NULL);
    ASSERT(!path_exists(invalid_cache));
    feng_cli_project_error_dispose(&error);

    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "bundled_force_dep: \"1.0.0\"\n");
    ASSERT(feng_cli_deps_install_for_manifest("feng", manifest_path, true, &error));
    assert_bundle_source_marker(force_cache, "bundled");
    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    ASSERT(unlink(invalid_bundled) == 0);
    ASSERT(unlink(force_bundled) == 0);
    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundled_root);
    free(cache_root);
    free(force_cache);
    free(invalid_cache);
    free(force_bundled);
    free(invalid_bundled);
    free(manifest_path);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

/* Verify local bundle and local project dependencies recurse through pkg/. */
static void test_deps_install_local_dependencies_use_bundled_transitives(void) {
    char template_path[] = "temp/feng_cli_deps_bundled_local_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *local_bundle_dir;
    char *local_project_dir;
    char *local_project_src;
    char *manifest_path;
    char *local_bundle_path;
    char *local_project_manifest;
    char *bundled_bundle_dep;
    char *bundled_project_dep;
    char *bundle_dep_cache;
    char *project_dep_cache;
    char *local_cache;
    char *local_project_build;
    char *bundled_root;
    char *saved_home = NULL;
    char *resolve_error = NULL;
    char *remove_error = NULL;
    FengCliProjectError error = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    project_dir = path_join(workspace_dir, "project");
    local_bundle_dir = path_join(workspace_dir, "local_bundle");
    local_project_dir = path_join(workspace_dir, "local_project");
    local_project_src = path_join(local_project_dir, "src");
    manifest_path = path_join(project_dir, "feng.fm");
    local_bundle_path = path_join(local_bundle_dir, "local_bundle.fb");
    local_project_manifest = path_join(local_project_dir, "feng.fm");
    bundled_bundle_dep = bundled_package_path("bundled_from_local_bundle-1.0.0.fb");
    bundled_project_dep = bundled_package_path("bundled_from_local_project-2.0.0.fb");
    bundle_dep_cache = path_join(workspace_dir,
                                 ".feng/cache/bundled_from_local_bundle-1.0.0.fb");
    project_dep_cache = path_join(workspace_dir,
                                  ".feng/cache/bundled_from_local_project-2.0.0.fb");
    local_cache = path_join(workspace_dir, ".feng/cache/local_bundle-0.1.0.fb");
    local_project_build = path_join(local_project_dir, "build");
    bundled_root = feng_cli_resolve_install_path("feng", "pkg", &resolve_error);
    ASSERT(bundled_root != NULL);
    ASSERT(resolve_error == NULL);

    mkdir_p(project_dir);
    mkdir_p(local_bundle_dir);
    mkdir_p(local_project_src);
    mkdir_p(bundled_root);
    ASSERT(unlink(bundled_bundle_dep) == 0 || errno == ENOENT);
    ASSERT(unlink(bundled_project_dep) == 0 || errno == ENOENT);
    write_manifest_only_bundle_or_die(
        bundled_bundle_dep,
        "[package]\n"
        "name: \"bundled_from_local_bundle\"\n"
        "version: \"1.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(
        bundled_project_dep,
        "[package]\n"
        "name: \"bundled_from_local_project\"\n"
        "version: \"2.0.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n");
    write_manifest_only_bundle_or_die(
        local_bundle_path,
        "[package]\n"
        "name: \"local_bundle\"\n"
        "version: \"0.1.0\"\n"
        "platform: \"macos-arm64\"\n"
        "abi: \"feng\"\n"
        "\n"
        "[dependencies]\n"
        "bundled_from_local_bundle: \"1.0.0\"\n");
    write_text_file(local_project_manifest,
                    "[package]\n"
                    "name: \"local_project\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "bundled_from_local_project: \"2.0.0\"\n");
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "local_bundle: \"../local_bundle/local_bundle.fb\"\n"
                    "local_project: \"../local_project\"\n");

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);
    ASSERT(feng_cli_deps_install_for_manifest("feng", manifest_path, false, &error));
    ASSERT(path_exists(bundle_dep_cache));
    ASSERT(path_exists(project_dep_cache));
    ASSERT(!path_exists(local_cache));
    ASSERT(!path_exists(local_project_build));
    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }

    ASSERT(unlink(bundled_bundle_dep) == 0);
    ASSERT(unlink(bundled_project_dep) == 0);
    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(bundled_root);
    free(local_project_build);
    free(local_cache);
    free(project_dep_cache);
    free(bundle_dep_cache);
    free(bundled_project_dep);
    free(bundled_bundle_dep);
    free(local_project_manifest);
    free(local_bundle_path);
    free(manifest_path);
    free(local_project_src);
    free(local_project_dir);
    free(local_bundle_dir);
    free(project_dir);
    feng_cli_project_error_dispose(&error);
}

/* Verify independent install and direct build both consume pkg/ through cache. */
static void test_project_build_installs_bundled_package_into_cache(void) {
    char template_path[] = "temp/feng_cli_build_bundled_package_XXXXXX";
    char *workspace_dir;
    char *library_dir;
    char *library_src_dir;
    char *library_manifest;
    char *library_source;
    char *packed_bundle;
    char *project_dir;
    char *project_src_dir;
    char *project_manifest;
    char *project_source;
    char *project_build_dir;
    char *binary_path;
    char *bundled_root;
    char *bundled_path;
    char *cache_path;
    char *saved_home = NULL;
    char *resolve_error = NULL;
    char *remove_error = NULL;
    FengCliDepsResolved resolved = {0};
    FengCliProjectError error = {0};

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    library_dir = path_join(workspace_dir, "library");
    library_src_dir = path_join(library_dir, "src");
    library_manifest = path_join(library_dir, "feng.fm");
    library_source = path_join(library_src_dir, "lib.ff");
    packed_bundle = path_join(library_dir,
                              "build/pkg/bundled_build_dep-1.0.0.fb");
    project_dir = path_join(workspace_dir, "project");
    project_src_dir = path_join(project_dir, "src");
    project_manifest = path_join(project_dir, "feng.fm");
    project_source = path_join(project_src_dir, "main.ff");
    project_build_dir = path_join(project_dir, "build");
    binary_path = project_host_build_path(project_dir, "bin/bundled_build_app");
    bundled_root = feng_cli_resolve_install_path("feng", "pkg", &resolve_error);
    ASSERT(bundled_root != NULL);
    ASSERT(resolve_error == NULL);
    bundled_path = bundled_package_path("bundled_build_dep-1.0.0.fb");
    cache_path = path_join(workspace_dir,
                           ".feng/cache/bundled_build_dep-1.0.0.fb");

    mkdir_p(library_src_dir);
    mkdir_p(project_src_dir);
    mkdir_p(bundled_root);
    ASSERT(unlink(bundled_path) == 0 || errno == ENOENT);
    write_text_file(library_manifest,
                    "[package]\n"
                    "name: \"bundled_build_dep\"\n"
                    "version: \"1.0.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(library_source,
                    "open module test.cli.bundledbuilddep;\n"
                    "open func bundled_value(): int { return 17; }\n");
    write_text_file(project_manifest,
                    "[package]\n"
                    "name: \"bundled_build_app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "bundled_build_dep: \"1.0.0\"\n");
    write_text_file(project_source,
                    "module test.cli.bundledbuildapp;\n"
                    "import test.cli.bundledbuilddep;\n"
                    "func main(args: string[]) {\n"
                    "  if bundled_value() == 17 {}\n"
                    "}\n");

    {
        char *argv[] = { library_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(packed_bundle));
    copy_file_or_die(packed_bundle, bundled_path);

    if (getenv("HOME") != NULL) {
        saved_home = dup_cstr(getenv("HOME"));
    }
    ASSERT(setenv("HOME", workspace_dir, 1) == 0);

    ASSERT(feng_cli_deps_install_for_manifest("feng",
                                              project_manifest,
                                              false,
                                              &error));
    ASSERT(path_exists(cache_path));
    ASSERT(feng_cli_deps_resolve_for_manifest("feng",
                                              project_manifest,
                                              false,
                                              false,
                                              &resolved,
                                              &error));
    ASSERT(resolved.package_count == 1U);
    ASSERT(strcmp(resolved.package_paths[0], cache_path) == 0);
    feng_cli_deps_resolved_dispose(&resolved);
    ASSERT(unlink(bundled_path) == 0);
    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(binary_path));

    ASSERT(feng_cli_project_remove_tree(project_build_dir, &remove_error));
    free(remove_error);
    remove_error = NULL;
    ASSERT(unlink(cache_path) == 0);
    copy_file_or_die(packed_bundle, bundled_path);
    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(cache_path));
    ASSERT(path_exists(binary_path));

    if (saved_home != NULL) {
        ASSERT(setenv("HOME", saved_home, 1) == 0);
    } else {
        ASSERT(unsetenv("HOME") == 0);
    }
    ASSERT(unlink(bundled_path) == 0);

    free(saved_home);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(cache_path);
    free(bundled_path);
    free(bundled_root);
    free(binary_path);
    free(project_build_dir);
    free(project_source);
    free(project_manifest);
    free(project_src_dir);
    free(project_dir);
    free(packed_bundle);
    free(library_source);
    free(library_manifest);
    free(library_src_dir);
    free(library_dir);
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_project_error_dispose(&error);
}

/* Verify unified project platform selection, whitelist, and sysroot rules. */
static void test_project_platform_selection_rules(void) {
    char *declared_platforms[] = {
        "linux-x64-musl",
        "macos-arm64",
    };
    const char *single_request[] = {
        "macos-arm64",
    };
    const char *duplicate_request[] = {
        "linux-x64-musl",
        "linux-x64-musl",
    };
    FengCliProjectContext context = {0};
    FengCliProjectPlatformSelection selection = {0};
    FengCliProjectError error = {0};
    char *host_platform = NULL;

    context.manifest_path = "/project/feng.fm";
    context.manifest.platforms = declared_platforms;
    context.manifest.platform_count = 2U;

    ASSERT(feng_cli_project_select_platforms(
        &context,
        NULL,
        0U,
        NULL,
        false,
        &selection,
        &error));
    ASSERT(selection.platform_count == 2U);
    ASSERT(strcmp(selection.platforms[0], "linux-x64-musl") == 0);
    ASSERT(strcmp(selection.platforms[1], "macos-arm64") == 0);
    feng_cli_project_platform_selection_dispose(&selection);

    ASSERT(feng_cli_project_select_platforms(
        &context,
        single_request,
        1U,
        "/sdk/macos",
        false,
        &selection,
        &error));
    ASSERT(selection.platform_count == 1U);
    ASSERT(strcmp(selection.platforms[0], "macos-arm64") == 0);
    feng_cli_project_platform_selection_dispose(&selection);

    ASSERT(!feng_cli_project_select_platforms(
        &context,
        NULL,
        0U,
        "/sdk/ambiguous",
        false,
        &selection,
        &error));
    ASSERT(strstr(error.message, "exactly one") != NULL);
    feng_cli_project_error_dispose(&error);

    ASSERT(!feng_cli_project_select_platforms(
        &context,
        duplicate_request,
        2U,
        NULL,
        false,
        &selection,
        &error));
    ASSERT(strstr(error.message, "more than once") != NULL);
    feng_cli_project_error_dispose(&error);

    context.manifest.platforms = NULL;
    context.manifest.platform_count = 0U;
    ASSERT(feng_cli_project_select_platforms(
        &context,
        NULL,
        0U,
        NULL,
        true,
        &selection,
        &error));
    ASSERT(feng_platform_detect_host_platform(&host_platform, NULL));
    ASSERT(selection.platform_count == 1U);
    ASSERT(strcmp(selection.platforms[0], host_platform) == 0);

    free(host_platform);
    feng_cli_project_platform_selection_dispose(&selection);
    feng_cli_project_error_dispose(&error);
}

/* Verify repeated project platforms build and pack only the requested set. */
static void test_project_build_and_pack_multiple_platforms(void) {
    char template_path[] = "temp/feng_cli_project_multi_platform_XXXXXX";
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *linux_library_dir;
    char *macos_library_dir;
    char *linux_library_path;
    char *macos_library_path;
    char *library_name;
    char *bundle_path;
    char *bundle_manifest = NULL;
    char *remove_error = NULL;
    char *zip_error = NULL;
    void *manifest_bytes = NULL;
    size_t manifest_size = 0U;
    FengZipReader reader = {0};

    project_dir = mkdtemp(template_path);
    ASSERT(project_dir != NULL);
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    linux_library_dir = project_platform_build_path(
        project_dir,
        "linux-x64-musl",
        "lib");
    macos_library_dir = project_platform_build_path(
        project_dir,
        "macos-arm64",
        "lib");
    library_name = host_static_library_file_name("crosslib");
    linux_library_path = path_join(linux_library_dir, library_name);
    macos_library_path = path_join(macos_library_dir, library_name);
    bundle_path = path_join(project_dir, "build/pkg/crosslib-0.1.0.fb");

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"crosslib\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "platform: \"macos-arm64,linux-x64-musl\"\n");
    write_text_file(source_path,
                    "open module test.cli.crosslib;\n"
                    "open func value(): int { return 1; }\n");

    {
        char *argv[] = {
            project_dir,
            "--platform=linux-x64-musl",
            "--platform=macos-arm64",
        };
        ASSERT(feng_cli_project_build_main("feng", 3, argv) == 0);
    }
    ASSERT(path_exists(linux_library_path));
    ASSERT(path_exists(macos_library_path));
    {
        char *argv[] = {
            project_dir,
            "--sysroot=/sdk/ambiguous",
        };
        ASSERT(feng_cli_project_build_main("feng", 2, argv) != 0);
        ASSERT(feng_cli_project_pack_main("feng", 2, argv) != 0);
    }

    {
        char *argv[] = {
            project_dir,
            "--platform=linux-x64-musl",
            "--platform=macos-arm64",
        };
        ASSERT(feng_cli_project_pack_main("feng", 3, argv) == 0);
    }
    ASSERT(path_exists(bundle_path));
    ASSERT(feng_zip_reader_open(bundle_path, &reader, &zip_error));
    ASSERT(feng_zip_reader_read(
        &reader,
        "feng.fm",
        &manifest_bytes,
        &manifest_size,
        &zip_error));
    bundle_manifest = (char *)malloc(manifest_size + 1U);
    ASSERT(bundle_manifest != NULL);
    memcpy(bundle_manifest, manifest_bytes, manifest_size);
    bundle_manifest[manifest_size] = '\0';
    ASSERT(strstr(
               bundle_manifest,
               "platform: \"linux-x64-musl,macos-arm64\"\n") != NULL);
    ASSERT(zip_contains_path_prefix(&reader, "lib/linux-x64-musl/"));
    ASSERT(zip_contains_path_prefix(&reader, "lib/macos-arm64/"));

    feng_zip_free(manifest_bytes);
    feng_zip_reader_dispose(&reader);
    ASSERT(feng_cli_project_remove_tree(project_dir, &remove_error));
    free(remove_error);
    free(bundle_manifest);
    free(zip_error);
    free(bundle_path);
    free(library_name);
    free(macos_library_path);
    free(linux_library_path);
    free(macos_library_dir);
    free(linux_library_dir);
    free(source_path);
    free(src_dir);
    free(manifest_path);
}

/* Verify run does not expose target-platform or sysroot options. */
static void test_project_run_rejects_platform_and_sysroot_options(void) {
    char *platform_argv[] = {
        "--platform=macos-arm64",
    };
    char *sysroot_argv[] = {
        "--sysroot=/sdk",
    };

    ASSERT(feng_cli_project_run_main("feng", 1, platform_argv) != 0);
    ASSERT(feng_cli_project_run_main("feng", 1, sysroot_argv) != 0);
}

static void test_project_build_default_uses_debug_friendly_flags(void) {
    char template_path[] = "temp/feng_cli_build_debug_flags_XXXXXX";
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
    binary_path = project_host_build_path(project_dir, "bin/app");

    mkdir_p(src_dir);
    write_text_file(manifest_path,
                    "[package]\n"
                    "name: \"app\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(source_path,
                    "module test.cli.debugflags;\n"
                    "func main(args: string[]) {}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);

    {
        char *argv[] = { project_dir };
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }

    ASSERT(path_exists(binary_path));
    cc_log_text = read_text_file(cc_log_path);
    ASSERT(count_occurrences(cc_log_text, "__CMD__") >= 1);
    ASSERT(count_occurrences(cc_log_text, "-O0") >= 1);
    ASSERT(count_logged_arguments(cc_log_text, "-g") >= 1);
    ASSERT(count_occurrences(cc_log_text, "-DNDEBUG") == 0);

    if (saved_cc != NULL) {
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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
    char template_path[] = "temp/feng_cli_build_release_flags_XXXXXX";
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
    binary_path = project_host_build_path(root_project_dir, "bin/release_app");
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
                    "open module test.cli.releasedep;\n"
                    "open func dep_value(): int {\n"
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
                    "module test.cli.releaseapp;\n"
                    "import test.cli.releasedep;\n"
                    "func main(args: string[]) {\n"
                    "  if dep_value() == 7 {\n"
                    "  }\n"
                    "}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);

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
    ASSERT(count_logged_arguments(cc_log_text, "-g") == 0);

    if (saved_cc != NULL) {
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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
    char template_path[] = "temp/feng_cli_build_bin_assets_XXXXXX";
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
    copied_asset_path = project_host_build_path(
        project_dir,
        "bin/runtime/config.txt");
    copied_nested_path = project_host_build_path(
        project_dir,
        "bin/runtime/nested/data.txt");
    binary_path = project_host_build_path(project_dir, "bin/asset_app");

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
                    "module test.cli.assets.bin;\n"
                    "func main(args: string[]) {}\n");
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
    char template_path[] = "temp/feng_cli_build_lib_assets_XXXXXX";
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
    staged_asset_path = project_host_build_path(
        project_dir,
        "assets/runtime/config.txt");
    staged_nested_path = project_host_build_path(
        project_dir,
        "assets/runtime/nested/data.txt");
    {
        char *library_dir = project_host_build_path(project_dir, "lib");
        library_path = host_static_library_path(library_dir, "asset_lib");
        free(library_dir);
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
                    "open module test.cli.assets.lib;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_build_lib_extlib_assets_XXXXXX";
    char *workspace_dir;
    char *project_dir;
    char *manifest_path;
    char *src_dir;
    char *source_path;
    char *asset_source_dir;
    char *asset_platform_dir;
    char *asset_source_path;
    char *staged_extlib_dir;
    char *staged_asset_path;
    char *shadow_stage_dir;
    char *library_path;
    char *host_target = NULL;
    char *error_message = NULL;
    char *staged_text;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    ASSERT(feng_platform_detect_host_platform(&host_target, &error_message));
    free(error_message);
    error_message = NULL;

    project_dir = path_join(workspace_dir, "libproj");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    asset_source_dir = path_join(project_dir, "vendor_extlib");
    asset_platform_dir = path_join(asset_source_dir, host_target);
    asset_source_path = host_dynamic_library_path(asset_platform_dir, "helper");
    staged_extlib_dir = project_host_build_path(project_dir, "extlib");
    staged_asset_path = host_dynamic_library_path(staged_extlib_dir, "helper");
    shadow_stage_dir = project_host_build_path(project_dir, "assets/extlib");
    {
        char *library_dir = project_host_build_path(project_dir, "lib");
        library_path = host_static_library_path(library_dir, "asset_extlib");
        free(library_dir);
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
                    "open module test.cli.assets.extlibstage;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_run_release_flags_XXXXXX";
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
    binary_path = project_host_build_path(root_project_dir, "bin/run_app");
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
                    "open module test.cli.rundep;\n"
                    "open func dep_value(): int {\n"
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
                    "module test.cli.runapp;\n"
                    "import test.cli.rundep;\n"
                    "func main(args: string[]) {\n"
                    "  if dep_value() == 7 {\n"
                    "  }\n"
                    "}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);

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
    ASSERT(count_logged_arguments(cc_log_text, "-g") == 0);

    if (saved_cc != NULL) {
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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

/* Verify imported generic descriptors drive cycle collection end to end. */
static void test_project_run_collects_cross_package_generic_cycle(void) {
    char template_path[] = "temp/feng_cli_generic_cycle_XXXXXX";
    char *workspace_dir;
    char *repo_root;
    char *std_project_dir;
    char *dep_project_dir;
    char *dep_manifest_path;
    char *dep_src_dir;
    char *dep_source_path;
    char *root_project_dir;
    char *root_manifest_path;
    char *root_src_dir;
    char *root_source_path;
    char *root_manifest_text;
    char *saved_threshold = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    repo_root = getcwd(NULL, 0);
    ASSERT(repo_root != NULL);
    std_project_dir = path_join(repo_root, "std/std");

    dep_project_dir = path_join(workspace_dir, "dep");
    dep_manifest_path = path_join(dep_project_dir, "feng.fm");
    dep_src_dir = path_join(dep_project_dir, "src");
    dep_source_path = path_join(dep_src_dir, "cycle.ff");
    root_project_dir = path_join(workspace_dir, "root");
    root_manifest_path = path_join(root_project_dir, "feng.fm");
    root_src_dir = path_join(root_project_dir, "src");
    root_source_path = path_join(root_src_dir, "main.ff");
    root_manifest_text = dup_printf(
        "[package]\n"
        "name: \"generic_cycle_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n"
        "\n"
        "[dependencies]\n"
        "std: \"%s\"\n"
        "generic_cycle_dep: \"../dep\"\n",
        std_project_dir);
    ASSERT(root_manifest_text != NULL);

    mkdir_p(dep_src_dir);
    mkdir_p(root_src_dir);
    write_text_file(dep_manifest_path,
                    "[package]\n"
                    "name: \"generic_cycle_dep\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(
        dep_source_path,
        "open module test.cli.genericcycle.provider;\n"
        "\n"
        "/** Left endpoint of an imported generic object cycle. */\n"
        "open type GenericCycleLeft<T> {\n"
        "  /** Managed payload whose representation is selected by T. */\n"
        "  open let payload: T;\n"
        "\n"
        "  /** Right endpoints connected through an array-managed edge. */\n"
        "  open var rights: GenericCycleRight<T>[] = [];\n"
        "\n"
        "  /** Construct an unconnected left endpoint. */\n"
        "  func GenericCycleLeft(payload: T) {\n"
        "    self.payload = payload;\n"
        "  }\n"
        "\n"
        "  /** Connect and return the opposite endpoint. */\n"
        "  open func connect(right: GenericCycleRight<T>): GenericCycleRight<T> {\n"
        "    self.rights = [right];\n"
        "    return self.rights[0];\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Right endpoint of an imported generic object cycle. */\n"
        "open type GenericCycleRight<T> {\n"
        "  /** Managed payload whose representation is selected by T. */\n"
        "  open let payload: T;\n"
        "\n"
        "  /** Left endpoints connected through an array-managed edge. */\n"
        "  open var lefts: GenericCycleLeft<T>[] = [];\n"
        "\n"
        "  /** Construct an unconnected right endpoint. */\n"
        "  func GenericCycleRight(payload: T) {\n"
        "    self.payload = payload;\n"
        "  }\n"
        "\n"
        "  /** Connect and return the opposite endpoint. */\n"
        "  open func connect(left: GenericCycleLeft<T>): GenericCycleLeft<T> {\n"
        "    self.lefts = [left];\n"
        "    return self.lefts[0];\n"
        "  }\n"
        "}\n");
    write_text_file(root_manifest_path, root_manifest_text);
    write_text_file(
        root_source_path,
        "module test.cli.genericcycle.consumer;\n"
        "\n"
        "import std.test;\n"
        "import test.cli.genericcycle.provider;\n"
        "\n"
        "/** Consumer-owned payload used to observe collection of the cycle. */\n"
        "type GenericCycleLeaf {\n"
        "  /** Explicit non-default leaves created by this test. */\n"
        "  static var created: i64 = 0;\n"
        "\n"
        "  /** Explicit non-default leaves finalized by the collector. */\n"
        "  static var finalized: i64 = 0;\n"
        "\n"
        "  /** Marker distinguishing explicit leaves from default values. */\n"
        "  let marker: string;\n"
        "\n"
        "  /** Construct one observable payload. */\n"
        "  func GenericCycleLeaf(marker: string) {\n"
        "    self.marker = marker;\n"
        "    GenericCycleLeaf.created += 1;\n"
        "  }\n"
        "\n"
        "  /** Record collection of an explicit payload. */\n"
        "  func ~GenericCycleLeaf() {\n"
        "    if self.marker != \"\" {\n"
        "      GenericCycleLeaf.finalized += 1;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  /** Reset observable lifecycle state. */\n"
        "  static func reset() {\n"
        "    GenericCycleLeaf.created = 0;\n"
        "    GenericCycleLeaf.finalized = 0;\n"
        "  }\n"
        "\n"
        "  /** Return the number of explicit leaves created. */\n"
        "  static func createdCount(): i64 {\n"
        "    return GenericCycleLeaf.created;\n"
        "  }\n"
        "\n"
        "  /** Return the number of explicit leaves finalized. */\n"
        "  static func finalizedCount(): i64 {\n"
        "    return GenericCycleLeaf.finalized;\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Create a real cycle and drop every external reference naturally. */\n"
        "func createUnreachableGenericCycle(): void {\n"
        "  let left = GenericCycleLeft<GenericCycleLeaf>(\n"
        "    GenericCycleLeaf(\"left\")\n"
        "  );\n"
        "  let right = GenericCycleRight<GenericCycleLeaf>(\n"
        "    GenericCycleLeaf(\"right\")\n"
        "  );\n"
        "  let returnedRight = left.connect(right);\n"
        "  let returnedLeft = right.connect(left);\n"
        "\n"
        "  assert(returnedRight.payload.marker == \"right\" &&\n"
        "         returnedLeft.payload.marker == \"left\",\n"
        "         \"both imported generic graph directions remain readable\");\n"
        "}\n"
        "\n"
        "/** Run the isolated generic cycle collection verification. */\n"
        "func main(args: string[]) {\n"
        "  GenericCycleLeaf.reset();\n"
        "  createUnreachableGenericCycle();\n"
        "  assert(GenericCycleLeaf.createdCount() == 2,\n"
        "         \"the graph creates exactly two explicit payloads\");\n"
        "  assert(GenericCycleLeaf.finalizedCount() == 2,\n"
        "         \"the unreachable imported generic cycle releases both payloads\");\n"
        "}\n");

    if (getenv("FENG_GC_THRESHOLD") != NULL) {
        saved_threshold = dup_cstr(getenv("FENG_GC_THRESHOLD"));
    }
    ASSERT(setenv("FENG_GC_THRESHOLD", "1", 1) == 0);
    {
        char *argv[] = { root_project_dir };
        ASSERT(feng_cli_project_run_main("feng", 1, argv) == 0);
    }
    if (saved_threshold != NULL) {
        ASSERT(setenv("FENG_GC_THRESHOLD", saved_threshold, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_GC_THRESHOLD") == 0);
    }

    free(saved_threshold);
    free(root_manifest_text);
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
    free(std_project_dir);
    free(repo_root);
}

/* Build and execute one four-package generic diamond twice with opposite root
 * dependency order. Both providers recover the same common generic
 * declarations, while each consumer supplies the only concrete closing type. */
static void test_project_build_closes_multi_provider_generic_diamond(void) {
    static const char *kCommonSource =
        "open module test.cli.genericdiamond.common;\n"
        "\n"
        "/** Descriptor-sized aggregate shared by both providers. */\n"
        "@value\n"
        "open type DiamondBox<T> {\n"
        "  /** Generic payload closed only by the final consumer. */\n"
        "  open let value: T;\n"
        "\n"
        "  /** Provider traversal marker. */\n"
        "  open let marker: string;\n"
        "\n"
        "  /** Construct one complete aggregate. */\n"
        "  func DiamondBox(value: T, marker: string) {\n"
        "    self.value = value;\n"
        "    self.marker = marker;\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Object-form contract whose witness returns the generic payload. */\n"
        "open spec DiamondNamed<T> {\n"
        "  /** Return the closed payload. */\n"
        "  func value(): T;\n"
        "}\n"
        "\n"
        "/** Callable-form contract transported through both providers. */\n"
        "open spec DiamondMapper<T>(value: T): T;\n"
        "\n"
        "/** Managed generic node combining every common representation. */\n"
        "open type DiamondNode<T> {\n"
        "  /** Aggregate payload. */\n"
        "  open let box: DiamondBox<T>;\n"
        "\n"
        "  /** Object-spec payload and witness. */\n"
        "  open let named: DiamondNamed<T>;\n"
        "\n"
        "  /** Callable payload. */\n"
        "  open let mapper: DiamondMapper<T>;\n"
        "\n"
        "  /** Construct the complete common representation graph. */\n"
        "  func DiamondNode(\n"
        "    box: DiamondBox<T>,\n"
        "    named: DiamondNamed<T>,\n"
        "    mapper: DiamondMapper<T>\n"
        "  ) {\n"
        "    self.box = box;\n"
        "    self.named = named;\n"
        "    self.mapper = mapper;\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Descriptor-sized value receiver used by imported shared bodies. */\n"
        "@value\n"
        "open type DiamondValueNode<T> {\n"
        "  /** Aggregate payload makes the closed receiver layout dynamic. */\n"
        "  open let box: DiamondBox<T>;\n"
        "\n"
        "  /** Fixed-size callable field follows the dynamic aggregate field. */\n"
        "  open let mapper: DiamondMapper<T>;\n"
        "\n"
        "  /** Construct one complete value receiver. */\n"
        "  func DiamondValueNode(\n"
        "    box: DiamondBox<T>,\n"
        "    mapper: DiamondMapper<T>\n"
        "  ) {\n"
        "    self.box = box;\n"
        "    self.mapper = mapper;\n"
        "  }\n"
        "}\n";
    static const char *kProviderASource =
        "open module test.cli.genericdiamond.provider_a;\n"
        "\n"
        "import test.cli.genericdiamond.common;\n"
        "\n"
        "/** Identity body used as a generic callable value. */\n"
        "open func providerAIdentity<T>(value: T): T { return value; }\n"
        "\n"
        "/** Construct the common representation graph in provider A. */\n"
        "open func providerANode<T>(\n"
        "  value: T,\n"
        "  named: DiamondNamed<T>\n"
        "): DiamondNode<T> {\n"
        "  let mapper: DiamondMapper<T> = providerAIdentity<T>;\n"
        "  return DiamondNode<T>(DiamondBox<T>(value, \"A\"), named, mapper);\n"
        "}\n"
        "\n"
        "/** Forward a node returned by the other provider. */\n"
        "open func providerAForward<T>(node: DiamondNode<T>): DiamondNode<T> {\n"
        "  return node;\n"
        "}\n"
        "\n"
        "/** Forward an object-spec value without changing its witness. */\n"
        "open func providerANamed<T>(named: DiamondNamed<T>): DiamondNamed<T> {\n"
        "  return named;\n"
        "}\n"
        "\n"
        "/** Construct a descriptor-sized value receiver in provider A. */\n"
        "open func providerAValueNode<T>(value: T): DiamondValueNode<T> {\n"
        "  let mapper: DiamondMapper<T> = providerAIdentity<T>;\n"
        "  return DiamondValueNode<T>(DiamondBox<T>(value, \"V\"), mapper);\n"
        "}\n";
    static const char *kProviderBSource =
        "open module test.cli.genericdiamond.provider_b;\n"
        "\n"
        "import test.cli.genericdiamond.common;\n"
        "\n"
        "/** Rebuild a provider-A node after invoking its common callable. */\n"
        "open func providerBTransform<T>(node: DiamondNode<T>): DiamondNode<T> {\n"
        "  let mapped = node.mapper(node.box.value);\n"
        "  return DiamondNode<T>(\n"
        "    DiamondBox<T>(mapped, node.box.marker + \"B\"),\n"
        "    node.named,\n"
        "    node.mapper\n"
        "  );\n"
        "}\n"
        "\n"
        "/** Invoke a callable formed by the other provider. */\n"
        "open func providerBApply<T>(mapper: DiamondMapper<T>, value: T): T {\n"
        "  return mapper(value);\n"
        "}\n"
        "\n"
        "/** Dispatch through an object-spec witness formed by the consumer. */\n"
        "open func providerBRead<T>(named: DiamondNamed<T>): T {\n"
        "  return named.value();\n"
        "}\n"
        "\n"
        "/** Invoke a callable field through a descriptor-sized value receiver. */\n"
        "open func providerBValueApply<T>(node: DiamondValueNode<T>): T {\n"
        "  return node.mapper(node.box.value);\n"
        "}\n";
    static const char *kConsumerSource =
        "module test.cli.genericdiamond.consumer;\n"
        "\n"
        "import test.cli.genericdiamond.common;\n"
        "import test.cli.genericdiamond.provider_a;\n"
        "import test.cli.genericdiamond.provider_b;\n"
        "\n"
        "@cdecl(\"libc\")\n"
        "extern func puts(message: string*): i32;\n"
        "\n"
        "/** Consumer-only descriptor-sized closing type. */\n"
        "@value\n"
        "type DiamondConsumerValue {\n"
        "  /** First managed payload. */\n"
        "  let first: string;\n"
        "\n"
        "  /** Second managed payload. */\n"
        "  let second: string;\n"
        "\n"
        "  /** Scalar tail distinguishing values. */\n"
        "  let number: i64;\n"
        "\n"
        "  /** Construct one complete consumer payload. */\n"
        "  func DiamondConsumerValue(first: string, second: string, number: i64) {\n"
        "    self.first = first;\n"
        "    self.second = second;\n"
        "    self.number = number;\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Consumer-owned implementation of the common generic contract. */\n"
        "type DiamondConsumerNamed: DiamondNamed<DiamondConsumerValue> {\n"
        "  /** Value returned through the imported witness. */\n"
        "  let stored: DiamondConsumerValue;\n"
        "\n"
        "  /** Construct one consumer witness subject. */\n"
        "  func DiamondConsumerNamed(stored: DiamondConsumerValue) {\n"
        "    self.stored = stored;\n"
        "  }\n"
        "\n"
        "  /** Implement the common generic contract. */\n"
        "  func value(): DiamondConsumerValue { return self.stored; }\n"
        "}\n"
        "\n"
        "/** Execute values through both branches of the package diamond. */\n"
        "func main(args: string[]) {\n"
        "  let named: DiamondNamed<DiamondConsumerValue> =\n"
        "    DiamondConsumerNamed(\n"
        "      DiamondConsumerValue(\"named-first\", \"named-second\", 41)\n"
        "    );\n"
        "  let fromA = providerANode<DiamondConsumerValue>(\n"
        "    DiamondConsumerValue(\"node-first\", \"node-second\", 42),\n"
        "    named\n"
        "  );\n"
        "  let fromB = providerBTransform<DiamondConsumerValue>(fromA);\n"
        "  let returned = providerAForward<DiamondConsumerValue>(fromB);\n"
        "  let mapped = providerBApply<DiamondConsumerValue>(\n"
        "    returned.mapper,\n"
        "    DiamondConsumerValue(\"map-first\", \"map-second\", 43)\n"
        "  );\n"
        "  let namedValue = providerBRead<DiamondConsumerValue>(\n"
        "    providerANamed<DiamondConsumerValue>(returned.named)\n"
        "  );\n"
        "  let valueNode = providerAValueNode<DiamondConsumerValue>(\n"
        "    DiamondConsumerValue(\"value-first\", \"value-second\", 44)\n"
        "  );\n"
        "  let valueMapped =\n"
        "    providerBValueApply<DiamondConsumerValue>(valueNode);\n"
        "  if returned.box.marker == \"AB\" &&\n"
        "     returned.box.value.first == \"node-first\" &&\n"
        "     returned.box.value.second == \"node-second\" &&\n"
        "     returned.box.value.number == 42 &&\n"
        "     mapped.first == \"map-first\" &&\n"
        "     mapped.second == \"map-second\" && mapped.number == 43 &&\n"
        "     namedValue.first == \"named-first\" &&\n"
        "     namedValue.second == \"named-second\" &&\n"
        "     namedValue.number == 41 && valueNode.box.marker == \"V\" &&\n"
        "     valueMapped.first == \"value-first\" &&\n"
        "     valueMapped.second == \"value-second\" &&\n"
        "     valueMapped.number == 44 {\n"
        "    puts(&\"diamond-ok\");\n"
        "  } else {\n"
        "    puts(&\"diamond-bad\");\n"
        "  }\n"
        "}\n";
    char template_path[] = "temp/feng_cli_generic_diamond_XXXXXX";
    char *workspace_dir;
    char *common_dir;
    char *common_src_dir;
    char *common_manifest_path;
    char *common_source_path;
    char *common_bundle_path;
    char *provider_a_dir;
    char *provider_a_src_dir;
    char *provider_a_manifest_path;
    char *provider_a_source_path;
    char *provider_a_bundle_path;
    char *provider_b_dir;
    char *provider_b_src_dir;
    char *provider_b_manifest_path;
    char *provider_b_source_path;
    char *provider_b_bundle_path;
    char *consumer_ab_dir;
    char *consumer_ab_src_dir;
    char *consumer_ab_manifest_path;
    char *consumer_ab_source_path;
    char *consumer_ab_binary_path;
    char *consumer_ba_dir;
    char *consumer_ba_src_dir;
    char *consumer_ba_manifest_path;
    char *consumer_ba_source_path;
    char *consumer_ba_binary_path;
    char *consumer_ab_output;
    char *consumer_ba_output;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    common_dir = path_join(workspace_dir, "common");
    common_src_dir = path_join(common_dir, "src");
    common_manifest_path = path_join(common_dir, "feng.fm");
    common_source_path = path_join(common_src_dir, "common.ff");
    common_bundle_path = path_join(
        common_dir, "build/pkg/diamond_common-0.1.0.fb");
    provider_a_dir = path_join(workspace_dir, "provider_a");
    provider_a_src_dir = path_join(provider_a_dir, "src");
    provider_a_manifest_path = path_join(provider_a_dir, "feng.fm");
    provider_a_source_path = path_join(provider_a_src_dir, "provider_a.ff");
    provider_a_bundle_path = path_join(
        provider_a_dir, "build/pkg/diamond_provider_a-0.1.0.fb");
    provider_b_dir = path_join(workspace_dir, "provider_b");
    provider_b_src_dir = path_join(provider_b_dir, "src");
    provider_b_manifest_path = path_join(provider_b_dir, "feng.fm");
    provider_b_source_path = path_join(provider_b_src_dir, "provider_b.ff");
    provider_b_bundle_path = path_join(
        provider_b_dir, "build/pkg/diamond_provider_b-0.1.0.fb");
    consumer_ab_dir = path_join(workspace_dir, "consumer_ab");
    consumer_ab_src_dir = path_join(consumer_ab_dir, "src");
    consumer_ab_manifest_path = path_join(consumer_ab_dir, "feng.fm");
    consumer_ab_source_path = path_join(consumer_ab_src_dir, "main.ff");
    consumer_ab_binary_path = project_host_build_path(
        consumer_ab_dir, "bin/diamond_consumer_ab");
    consumer_ba_dir = path_join(workspace_dir, "consumer_ba");
    consumer_ba_src_dir = path_join(consumer_ba_dir, "src");
    consumer_ba_manifest_path = path_join(consumer_ba_dir, "feng.fm");
    consumer_ba_source_path = path_join(consumer_ba_src_dir, "main.ff");
    consumer_ba_binary_path = project_host_build_path(
        consumer_ba_dir, "bin/diamond_consumer_ba");

    mkdir_p(common_src_dir);
    mkdir_p(provider_a_src_dir);
    mkdir_p(provider_b_src_dir);
    mkdir_p(consumer_ab_src_dir);
    mkdir_p(consumer_ba_src_dir);
    write_text_file(common_manifest_path,
                    "[package]\n"
                    "name: \"diamond_common\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(common_source_path, kCommonSource);
    write_text_file(provider_a_manifest_path,
                    "[package]\n"
                    "name: \"diamond_provider_a\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "diamond_common: \"../common\"\n");
    write_text_file(provider_a_source_path, kProviderASource);
    write_text_file(provider_b_manifest_path,
                    "[package]\n"
                    "name: \"diamond_provider_b\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "diamond_common: \"../common\"\n");
    write_text_file(provider_b_source_path, kProviderBSource);
    write_text_file(consumer_ab_manifest_path,
                    "[package]\n"
                    "name: \"diamond_consumer_ab\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "diamond_provider_a: \"../provider_a\"\n"
                    "diamond_provider_b: \"../provider_b\"\n");
    write_text_file(consumer_ab_source_path, kConsumerSource);
    write_text_file(consumer_ba_manifest_path,
                    "[package]\n"
                    "name: \"diamond_consumer_ba\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"bin\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "diamond_provider_b: \"../provider_b\"\n"
                    "diamond_provider_a: \"../provider_a\"\n");
    write_text_file(consumer_ba_source_path, kConsumerSource);

    {
        char *argv[] = {consumer_ab_dir};
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(common_bundle_path));
    ASSERT(path_exists(provider_a_bundle_path));
    ASSERT(path_exists(provider_b_bundle_path));
    ASSERT(path_exists(consumer_ab_binary_path));
    consumer_ab_output =
        run_binary_capture_stdout_or_die(consumer_ab_binary_path);
    ASSERT(strcmp(consumer_ab_output, "diamond-ok\n") == 0);

    {
        char *argv[] = {consumer_ba_dir};
        ASSERT(feng_cli_project_build_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(consumer_ba_binary_path));
    consumer_ba_output =
        run_binary_capture_stdout_or_die(consumer_ba_binary_path);
    ASSERT(strcmp(consumer_ba_output, "diamond-ok\n") == 0);

    free(consumer_ba_output);
    free(consumer_ab_output);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(consumer_ba_binary_path);
    free(consumer_ba_source_path);
    free(consumer_ba_manifest_path);
    free(consumer_ba_src_dir);
    free(consumer_ba_dir);
    free(consumer_ab_binary_path);
    free(consumer_ab_source_path);
    free(consumer_ab_manifest_path);
    free(consumer_ab_src_dir);
    free(consumer_ab_dir);
    free(provider_b_bundle_path);
    free(provider_b_source_path);
    free(provider_b_manifest_path);
    free(provider_b_src_dir);
    free(provider_b_dir);
    free(provider_a_bundle_path);
    free(provider_a_source_path);
    free(provider_a_manifest_path);
    free(provider_a_src_dir);
    free(provider_a_dir);
    free(common_bundle_path);
    free(common_source_path);
    free(common_manifest_path);
    free(common_src_dir);
    free(common_dir);
}

/* Verify a closed generic finalizer receives its owner descriptor when the
 * cycle collector, rather than ordinary ARC, discovers the object. */
static void test_project_run_collects_generic_finalizer_cycle(void) {
    char template_path[] = "temp/feng_cli_generic_finalizer_cycle_XXXXXX";
    char *workspace_dir;
    char *repo_root;
    char *std_project_dir;
    char *project_manifest_path;
    char *project_src_dir;
    char *project_source_path;
    char *project_manifest_text;
    char *saved_threshold = NULL;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    repo_root = getcwd(NULL, 0);
    ASSERT(repo_root != NULL);
    std_project_dir = path_join(repo_root, "std/std");
    project_manifest_path = path_join(workspace_dir, "feng.fm");
    project_src_dir = path_join(workspace_dir, "src");
    project_source_path = path_join(project_src_dir, "main.ff");
    project_manifest_text = dup_printf(
        "[package]\n"
        "name: \"generic_finalizer_cycle_app\"\n"
        "version: \"0.1.0\"\n"
        "target: \"bin\"\n"
        "src: \"src/\"\n"
        "out: \"build/\"\n"
        "\n"
        "[dependencies]\n"
        "std: \"%s\"\n",
        std_project_dir);
    ASSERT(project_manifest_text != NULL);

    mkdir_p(project_src_dir);
    write_text_file(project_manifest_path, project_manifest_text);
    write_text_file(
        project_source_path,
        "module test.cli.genericfinalizercycle;\n"
        "\n"
        "import std.test;\n"
        "\n"
        "/** Descriptor-sized payload closing the generic cycle nodes. */\n"
        "@value\n"
        "type GenericFinalizerWideValue {\n"
        "  /** First managed payload slot. */\n"
        "  let first: string;\n"
        "\n"
        "  /** Second managed payload slot. */\n"
        "  let second: string;\n"
        "\n"
        "  /** Construct one observable wide payload. */\n"
        "  func GenericFinalizerWideValue(first: string, second: string) {\n"
        "    self.first = first;\n"
        "    self.second = second;\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Generic dependency instantiated inside the node finalizer. */\n"
        "type GenericFinalizerBox<T> {\n"
        "  /** Reified payload storage forcing the closed box layout. */\n"
        "  var value: T;\n"
        "\n"
        "  /** Marker following the reified payload in physical layout. */\n"
        "  let marker: string;\n"
        "\n"
        "  /** Construct the finalizer-local dependency. */\n"
        "  func GenericFinalizerBox() {\n"
        "    self.marker = \"generic-finalizer\";\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Observable state outside the unreachable object graph. */\n"
        "type GenericFinalizerState {\n"
        "  static var finalized: i64 = 0;\n"
        "\n"
        "  /** Reset the finalizer counter. */\n"
        "  static func reset(): void { GenericFinalizerState.finalized = 0; }\n"
        "\n"
        "  /** Record one closed generic node finalizer. */\n"
        "  static func record(): void { GenericFinalizerState.finalized += 1; }\n"
        "\n"
        "  /** Return the observed finalizer count. */\n"
        "  static func count(): i64 { return GenericFinalizerState.finalized; }\n"
        "}\n"
        "\n"
        "/** Generic reference node participating in a two-object cycle. */\n"
        "type GenericFinalizerNode<T> {\n"
        "  /** Reified payload retained by this node. */\n"
        "  let payload: T;\n"
        "\n"
        "  /** Opposite nodes forming managed cycle edges. */\n"
        "  var nexts: GenericFinalizerNode<T>[] = [];\n"
        "\n"
        "  /** Construct one disconnected node. */\n"
        "  func GenericFinalizerNode(payload: T) { self.payload = payload; }\n"
        "\n"
        "  /** Connect this node to its opposite endpoint. */\n"
        "  func connect(next: GenericFinalizerNode<T>): void { self.nexts = [next]; }\n"
        "\n"
        "  /** Exercise a type-owner dependency while recording finalization. */\n"
        "  func ~GenericFinalizerNode() {\n"
        "    let box = GenericFinalizerBox<T>();\n"
        "    if box.marker == \"generic-finalizer\" {\n"
        "      GenericFinalizerState.record();\n"
        "    }\n"
        "  }\n"
        "}\n"
        "\n"
        "/** Create a closed generic cycle and release all external roots. */\n"
        "func createGenericFinalizerCycle(): void {\n"
        "  let left = GenericFinalizerNode<GenericFinalizerWideValue>(\n"
        "    GenericFinalizerWideValue(\"left-first\", \"left-second\")\n"
        "  );\n"
        "  let right = GenericFinalizerNode<GenericFinalizerWideValue>(\n"
        "    GenericFinalizerWideValue(\"right-first\", \"right-second\")\n"
        "  );\n"
        "  left.connect(right);\n"
        "  right.connect(left);\n"
        "}\n"
        "\n"
        "/** Run the isolated generic-finalizer cycle verification. */\n"
        "func main(args: string[]) {\n"
        "  GenericFinalizerState.reset();\n"
        "  createGenericFinalizerCycle();\n"
        "  assert(GenericFinalizerState.count() == 2,\n"
        "         \"cycle collection invokes each closed generic finalizer once\");\n"
        "}\n");

    if (getenv("FENG_GC_THRESHOLD") != NULL) {
        saved_threshold = dup_cstr(getenv("FENG_GC_THRESHOLD"));
    }
    ASSERT(setenv("FENG_GC_THRESHOLD", "1", 1) == 0);
    {
        char *argv[] = { workspace_dir };
        ASSERT(feng_cli_project_run_main("feng", 1, argv) == 0);
    }
    if (saved_threshold != NULL) {
        ASSERT(setenv("FENG_GC_THRESHOLD", saved_threshold, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_GC_THRESHOLD") == 0);
    }

    free(saved_threshold);
    free(project_manifest_text);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(project_source_path);
    free(project_src_dir);
    free(project_manifest_path);
    free(std_project_dir);
    free(repo_root);
}

static void test_project_pack_uses_release_build_and_public_ft_excludes_spans(void) {
    char template_path[] = "temp/feng_cli_pack_release_flags_XXXXXX";
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
    bundle_path = path_join(root_project_dir, "build/pkg/rootlib-0.1.0.fb");
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
                    "open module local.dep;\n"
                    "open func value(): int {\n"
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
                    "open module test.cli.packroot;\n"
                    "open func root_value(): int {\n"
                    "  return 2;\n"
                    "}\n");

    if (getenv("FENG_CC") != NULL) {
        saved_cc = dup_cstr(getenv("FENG_CC"));
    }
    ASSERT(setenv("FENG_CC", cc_wrapper_path, 1) == 0);

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
    ASSERT(count_logged_arguments(cc_log_text, "-g") == 0);

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
        ASSERT(setenv("FENG_CC", saved_cc, 1) == 0);
    } else {
        ASSERT(unsetenv("FENG_CC") == 0);
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
    char template_path[] = "temp/feng_cli_pack_assets_bundle_XXXXXX";
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
    bundle_path = path_join(project_dir, "build/pkg/asset_pack-0.1.0.fb");

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
                    "open module test.cli.assetpack;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_pack_extlib_assets_bundle_XXXXXX";
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
    ASSERT(feng_platform_detect_host_platform(&host_target, &error_message));
    free(error_message);
    error_message = NULL;

    project_dir = path_join(workspace_dir, "libproj");
    manifest_path = path_join(project_dir, "feng.fm");
    src_dir = path_join(project_dir, "src");
    source_path = path_join(src_dir, "lib.ff");
    asset_source_dir = path_join(project_dir, "vendor_extlib");
    asset_platform_dir = path_join(asset_source_dir, host_target);
    asset_source_path = host_dynamic_library_path(asset_platform_dir, "helper");
    shadow_stage_dir = project_host_build_path(project_dir, "assets/extlib");
    bundle_path = path_join(project_dir, "build/pkg/asset_extlib_pack-0.1.0.fb");

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
                    "open module test.cli.assetextlibpack;\n"
                    "open func value(): int {\n"
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
    char template_path[] = "temp/feng_cli_pack_no_release_flag_XXXXXX";
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
                    "open module test.cli.packnorelease;\n"
                    "open func value(): int {\n"
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
        "module test.lsp.localrhs;\n"
        "\n"
        "func check(n: int): int {\n"
        "    let doubled = if n > 0 { n + n; } else { 0; };\n"
        "    return doubled;\n"
        "}\n"
        "\n"
        "func loop_sum(n: int): int {\n"
        "    var acc: int = 0;\n"
        "    for var i = 0; i < n; i += 1 {\n"
        "        acc = acc + i;\n"
        "    }\n"
        "    return acc;\n"
        "}\n";
    char template_path[] = "temp/feng_lsp_localrhs_XXXXXX";
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

    input = temp_file();
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
    /* int maps to i64 on 64-bit, i32 on 32-bit — assert the platform-correct name. */
    if (sizeof(void *) >= 8) {
        ASSERT(strstr(output, "let n: i64") != NULL || strstr(output, "var n: i64") != NULL);
    } else {
        ASSERT(strstr(output, "let n: i32") != NULL || strstr(output, "var n: i32") != NULL);
    }

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

static void test_lsp_hover_uses_inferred_top_level_binding_type(void) {
    static const char *kSource =
        "module test.lsp.inferred_binding;\n"
        "\n"
        "let TEST_NAME = \"hello_world\";\n"
        "\n"
        "func message(): string {\n"
        "    return \"Running test: \" + TEST_NAME;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let label: string = message();\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output = capture_lsp_hover_response(kSource,
                                              kInitialize,
                                              "let TEST_NAME = \"hello_world\";",
                                              strlen("let "));

    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output, "let TEST_NAME: string") != NULL);
    ASSERT(strstr(output, "let TEST_NAME: void") == NULL);

    free(output);
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
        "open module test.lsp.usepath.lib;\n"
        "\n"
        "open func value(): int {\n"
        "    return 1;\n"
        "}\n";
    /* Main source ends with an incomplete `import` path — the cursor is placed
     * right after the trailing dot to trigger path-segment completion. */
    static const char *kMainSource =
        "module test.lsp.usepath.main;\n"
        "import test.lsp.usepath.\n";
    char template_path[] = "temp/feng_lsp_usepath_XXXXXX";
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

    /* Position the cursor right after the trailing dot in `import test.lsp.usepath.`
     * (line 1, character 24). */
    find_line_character(kMainSource, "import test.lsp.usepath.", 24U, &comp_line, &comp_char);

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

    output = run_lsp_single_position_response_after_ready(
        initialize,
        did_open,
        "textDocument/completion",
        main_uri,
        comp_line,
        comp_char,
        "\"label\":\"lib\"",
        completion_req,
        shutdown);

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
        "open module a.b.c;\n";
    static const char *kDSource =
        "open module a.b.d;\n";
    static const char *kMainSource =
        "module app.main;\n"
        "import a.\n";
    char template_path[] = "temp/feng_lsp_usepath_dedup_XXXXXX";
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

    find_line_character(kMainSource, "import a.", 9U, &comp_line, &comp_char);

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

    output = run_lsp_single_position_response_after_ready(
        initialize,
        did_open,
        "textDocument/completion",
        main_uri,
        comp_line,
        comp_char,
        "\"label\":\"b\"",
        completion_req,
        shutdown);

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

/* Tests the edit-time project-scan fallback used for incomplete `import` paths.
 * Before the fix, deduplication stored slices into freed file buffers, so
 * `import foo.` could return duplicate `bar` entries when several modules shared
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
        "open module foo.bar.a;\n"
        "\n"
        "open func alpha(): int {\n"
        "    return 1;\n"
        "}\n";
    static const char *kBSource =
        "open module foo.bar.b;\n"
        "\n"
        "open func beta(): int {\n"
        "    return 2;\n"
        "}\n";
    static const char *kCSource =
        "open module foo.bar.c;\n"
        "\n"
        "open func gamma(): int {\n"
        "    return 3;\n"
        "}\n";
    static const char *kMainSource =
        "module foo.bar.current;\n"
        "import foo.\n";
    char template_path[] = "temp/feng_lsp_usepath_scan_dedup_XXXXXX";
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

    find_line_character(kMainSource, "import foo.", 11U, &comp_line, &comp_char);

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

    output = run_lsp_single_position_response_after_ready(
        initialize,
        did_open,
        "textDocument/completion",
        main_uri,
        comp_line,
        comp_char,
        "\"label\":\"bar\"",
        completion_req,
        shutdown);

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
        "open module test.lsp.imptypes;\n"
        "\n"
        "open type Widget {\n"
        "    let id: int;\n"
        "}\n"
        "\n"
        "open func make_widget(): Widget {\n"
        "    return Widget { id: 0 };\n"
        "}\n";
    static const char *kMainSource =
        "module test.lsp.impcomp.main;\n"
        "import test.lsp.imptypes;\n"
        "\n"
        "func run(): int {\n"
        "    return 0;\n"
        "}\n";
    char template_path[] = "temp/feng_lsp_impcomp_XXXXXX";
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

    output = run_lsp_single_position_response_after_ready(
        initialize,
        did_open,
        "textDocument/completion",
        main_uri,
        comp_line,
        comp_char,
        "\"label\":\"Widget\"",
        completion_req,
        shutdown);

    /* The imported type `Widget` and function `make_widget` from the `use`d
     * module must appear as completion candidates in run()'s body. */
    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"Widget\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"make_widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"Widget\",\"kind\":6,\"detail\":\"type Widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"make_widget\",\"kind\":3,\"detail\":\"func make_widget(): Widget\"") != NULL);

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
        "open module other.lib;\n"
        "\n"
        "open type Widget {\n"
        "    let id: int;\n"
        "}\n"
        "\n"
        "open func make_widget(): Widget {\n"
        "    return Widget { id: 0 };\n"
        "}\n";
    static const char *kMainSource =
        "module app.main;\n"
        "import other.lib;\n"
        "\n"
        "func helper(): int {\n"
        "    return 0;\n"
        "}\n";
    char template_path[] = "temp/feng_lsp_impcomp_degraded_XXXXXX";
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

    output = run_lsp_single_position_response_after_ready(
        initialize,
        did_open,
        "textDocument/completion",
        main_uri,
        comp_line,
        comp_char,
        "\"label\":\"Widget\"",
        completion_req,
        shutdown);

    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"helper\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"Widget\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"make_widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"Widget\",\"kind\":6,\"detail\":\"type Widget\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"make_widget\",\"kind\":3,\"detail\":\"func make_widget(): Widget\"") != NULL);

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
        "open module test.lsp.alias.loop;\n"
        "\n"
        "open func loop_example(): int {\n"
        "    return 1;\n"
        "}\n";
    static const char *kMainSource =
        "module test.lsp.alias.main;\n"
        "import test.lsp.alias.loop as lp;\n"
        "\n"
        "func run(): int {\n"
        "    lp.\n"
        "    return 0;\n"
        "}\n";
    char template_path[] = "temp/feng_lsp_alias_completion_XXXXXX";
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

    output = run_lsp_single_position_response_after_ready(
        initialize,
        did_open,
        "textDocument/completion",
        main_uri,
        comp_line,
        comp_char,
        "\"label\":\"loop_example\"",
        completion_req,
        shutdown);

    ASSERT(strstr(output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(output, "\"label\":\"loop_example\"") != NULL);
    ASSERT(strstr(output,
                  "\"label\":\"loop_example\",\"kind\":3,\"detail\":\"func loop_example(): int\"") != NULL);

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
        "open module test.lsp.pkg.collections;\n"
        "\n"
        "/**\n"
        " * Package map docs.\n"
        " */\n"
        "open type Map<K, V> {\n"
        "    /**\n"
        "     * Number of stored entries.\n"
        "     */\n"
        "    open let count: int;\n"
        "}\n"
        "\n"
        "/**\n"
        " * Package join docs.\n"
        " */\n"
        "open func join(prefix: string, parts: string...): string {\n"
        "    return prefix;\n"
        "}\n";
    static const char *kHoverSource =
        "module test.lsp.pkgconsumer.main;\n"
        "import test.lsp.pkg.collections;\n"
        "\n"
        "func consume(map: Map<string, int>): int {\n"
        "    return map.count;\n"
        "}\n"
        "\n"
        "func consume_join(): string {\n"
        "    return join(\"x\", \"a\");\n"
        "}\n";
    static const char *kUsePathSource =
        "module test.lsp.pkgconsumer.useedit;\n"
        "import test.lsp.pkg.\n"
        "\n"
        "func run(): void {}\n";
    static const char *kTypeCompletionSource =
        "module test.lsp.pkgconsumer.typeedit;\n"
        "import test.lsp.pkg.collections;\n"
        "\n"
        "func run(): void {\n"
        "    let value: Ma\n"
        "}\n";
    static const char *kCtorCompletionSource =
        "module test.lsp.pkgconsumer.ctoredit;\n"
        "import test.lsp.pkg.collections;\n"
        "\n"
        "func run(): void {\n"
        "    let value = M\n"
        "}\n";
    static const char *kBareCompletionSource =
        "module test.lsp.pkgconsumer.bareedit;\n"
        "import test.lsp.pkg.collections;\n"
        "\n"
        "func run(): void {\n"
        "    M\n"
        "}\n";
    static const char *kMemberCompletionSource =
        "module test.lsp.pkgconsumer.memberedit;\n"
        "import test.lsp.pkg.collections;\n"
        "\n"
        "func consume(map: Map<string, int>): int {\n"
        "    return map.;\n"
        "}\n";
    static const char *kFunctionCompletionSource =
        "module test.lsp.pkgconsumer.functionedit;\n"
        "import test.lsp.pkg.collections;\n"
        "\n"
        "func run(): void {\n"
        "    j\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char template_path[] = "temp/feng_lsp_external_pkg_XXXXXX";
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
    char *hover_function_output;
    char *use_completion_output;
    char *type_completion_output;
    char *ctor_completion_output;
    char *bare_completion_output;
    char *function_completion_output;
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
    bundle_path = path_join(pkg_project_dir, "build/pkg/lsp_pkgdocs-0.1.0.fb");
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
                    "lsp_pkgdocs: \"../pkgdocs/build/pkg/lsp_pkgdocs-0.1.0.fb\"\n");
    write_text_file(main_path, kHoverSource);

    hover_type_output = capture_lsp_position_response_at_path(main_path,
                                                              kHoverSource,
                                                              kInitialize,
                                                              "textDocument/hover",
                                                              "func consume(map: Map<string, int>): int {",
                                                              strlen("func consume(map: "),
                                                              "type Map<K, V>");
    hover_member_output = capture_lsp_position_response_at_path(main_path,
                                                                kHoverSource,
                                                                kInitialize,
                                                                "textDocument/hover",
                                                                "    return map.count;",
                                                                strlen("    return map."),
                                                                "Number of stored entries.");
    hover_function_output = capture_lsp_position_response_at_path(main_path,
                                                                  kHoverSource,
                                                                  kInitialize,
                                                                  "textDocument/hover",
                                                                  "    return join(\"x\", \"a\");",
                                                                  strlen("    return "),
                                                                  "Package join docs.");
    use_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                  kUsePathSource,
                                                                  kInitialize,
                                                                  "textDocument/completion",
                                                                  "import test.lsp.pkg.",
                                                                  strlen("import test.lsp.pkg."),
                                                                  "\"label\":\"collections\"");
    type_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                   kTypeCompletionSource,
                                                                   kInitialize,
                                                                   "textDocument/completion",
                                                                   "    let value: Ma",
                                                                   strlen("    let value: Ma"),
                                                                   "\"label\":\"Map<K, V>\"");
    ctor_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                   kCtorCompletionSource,
                                                                   kInitialize,
                                                                   "textDocument/completion",
                                                                   "    let value = M",
                                                                   strlen("    let value = M"),
                                                                   "\"label\":\"Map<K, V>\"");
    bare_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                   kBareCompletionSource,
                                                                   kInitialize,
                                                                   "textDocument/completion",
                                                                   "    M",
                                                                   strlen("    M"),
                                                                   "\"label\":\"Map<K, V>\"");
    function_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                       kFunctionCompletionSource,
                                                                       kInitialize,
                                                                       "textDocument/completion",
                                                                       "    j",
                                                                       strlen("    j"),
                                                                       "\"label\":\"join\"");
    member_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                     kMemberCompletionSource,
                                                                     kInitialize,
                                                                     "textDocument/completion",
                                                                     "    return map.;",
                                                                     strlen("    return map."),
                                                                     "\"label\":\"count\"");
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
                                                                             strlen("    let value = M"),
                                                                             "\"label\":\"Map<K, V>\"");
    local_dep_bare_completion_output = capture_lsp_position_response_at_path(main_path,
                                                                             kBareCompletionSource,
                                                                             kInitialize,
                                                                             "textDocument/completion",
                                                                             "    M",
                                                                             strlen("    M"),
                                                                             "\"label\":\"Map<K, V>\"");

    ASSERT(strstr(hover_type_output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(hover_type_output, "type Map<K, V>") != NULL);
    ASSERT(strstr(hover_type_output, "Package map docs.") != NULL);
    ASSERT(strstr(hover_member_output, "\"id\":2,\"result\":null") == NULL);
    if (sizeof(void *) >= 8) {
        ASSERT(strstr(hover_member_output, "let count: i64") != NULL);
    } else {
        ASSERT(strstr(hover_member_output, "let count: i32") != NULL);
    }
    ASSERT(strstr(hover_member_output, "Number of stored entries.") != NULL);
    ASSERT(strstr(hover_function_output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(hover_function_output, "func join(prefix: string, parts: string...): string") != NULL);
    ASSERT(strstr(hover_function_output, "func join(prefix: string, parts: string[]): string") == NULL);
    ASSERT(strstr(hover_function_output, "Package join docs.") != NULL);
    ASSERT(strstr(use_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(use_completion_output, "\"label\":\"collections\"") != NULL);
    ASSERT(strstr(type_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(type_completion_output, "\"label\":\"Map<K, V>\"") != NULL);
    ASSERT(strstr(type_completion_output, "\"label\":\"Map<K, V>\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(ctor_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(ctor_completion_output, "\"label\":\"Map<K, V>\"") != NULL);
    ASSERT(strstr(ctor_completion_output, "\"label\":\"Map<K, V>\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(bare_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(bare_completion_output, "\"label\":\"Map<K, V>\"") != NULL);
    ASSERT(strstr(bare_completion_output, "\"label\":\"Map<K, V>\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(function_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(function_completion_output, "\"label\":\"join\"") != NULL);
    ASSERT(strstr(function_completion_output,
                  "\"label\":\"join\",\"kind\":3,\"detail\":\"func join(prefix: string, parts: string...): string\"") != NULL);
    ASSERT(strstr(function_completion_output,
                  "\"label\":\"join\",\"kind\":3,\"detail\":\"func join(prefix: string, parts: string[]): string\"") == NULL);
    ASSERT(strstr(local_dep_ctor_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(local_dep_ctor_completion_output, "\"label\":\"Map<K, V>\"") != NULL);
    ASSERT(strstr(local_dep_ctor_completion_output, "\"label\":\"Map<K, V>\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(local_dep_bare_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(local_dep_bare_completion_output, "\"label\":\"Map<K, V>\"") != NULL);
    ASSERT(strstr(local_dep_bare_completion_output, "\"label\":\"Map<K, V>\",\"kind\":6,\"detail\":\"type Map<K, V>\"") != NULL);
    ASSERT(strstr(member_completion_output, "\"id\":2,\"result\":[]") == NULL);
    ASSERT(strstr(member_completion_output, "\"label\":\"count\"") != NULL);
    ASSERT(strstr(member_completion_output, "\"label\":\"K\"") == NULL);
    ASSERT(strstr(member_completion_output, "\"label\":\"V\"") == NULL);

    free(member_completion_output);
    free(local_dep_bare_completion_output);
    free(local_dep_ctor_completion_output);
    free(function_completion_output);
    free(bare_completion_output);
    free(ctor_completion_output);
    free(type_completion_output);
    free(use_completion_output);
    free(hover_function_output);
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

/* Persistent package symbols preserve every Hover declaration category and shape. */
static void test_lsp_package_symbol_hover_type_categories(void) {
    static const char *kPackageSource =
        "open module test.lsp.hoverpkg;\n"
        "\n"
        "open type RefType {\n"
        "    open let value: i32;\n"
        "    open func copy(value: RefType): RefType { return value; }\n"
        "}\n"
        "@value\n"
        "open type ValueType {\n"
        "    open let value: i32;\n"
        "}\n"
        "open type TupleType(i32, string);\n"
        "open enum State { Ready = 1, Done = 2 }\n"
        "open spec ObjectShape {\n"
        "    func first(): string;\n"
        "}\n"
        "open spec OtherShape {\n"
        "    func second(): string;\n"
        "}\n"
        "open spec CallbackShape(input: i32): string;\n"
        "open spec UnionShape: RefType | ValueType;\n"
        "open spec IntersectionShape: ObjectShape & OtherShape;\n";
    static const char *kConsumerSource =
        "module test.lsp.hoverconsumer;\n"
        "import test.lsp.hoverpkg;\n"
        "\n"
        "func broken(ref: RefType, value: ValueType, tuple: TupleType, state: State, object: ObjectShape, callback: CallbackShape, unionValue: UnionShape, intersectionValue: IntersectionShape): void {\n"
        "    ref.copy(ref);\n"
        "    let constructed = RefType();\n"
        "    let done: State = State.Done;\n"
        "    missing;\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char template_path[] = "temp/feng_lsp_hover_symbol_categories_XXXXXX";
    char *workspace_dir;
    char *package_dir;
    char *package_manifest;
    char *package_src_dir;
    char *package_source_path;
    char *bundle_path;
    char *consumer_dir;
    char *consumer_manifest;
    char *consumer_src_dir;
    char *consumer_source_path;
    char *output;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    package_dir = path_join(workspace_dir, "package");
    package_manifest = path_join(package_dir, "feng.fm");
    package_src_dir = path_join(package_dir, "src");
    package_source_path = path_join(package_src_dir, "types.ff");
    bundle_path = path_join(package_dir, "build/pkg/lsp_hover_types-0.1.0.fb");
    consumer_dir = path_join(workspace_dir, "consumer");
    consumer_manifest = path_join(consumer_dir, "feng.fm");
    consumer_src_dir = path_join(consumer_dir, "src");
    consumer_source_path = path_join(consumer_src_dir, "main.ff");

    mkdir_p(package_src_dir);
    mkdir_p(consumer_src_dir);
    write_text_file(package_manifest,
                    "[package]\n"
                    "name: \"lsp_hover_types\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(package_source_path, kPackageSource);
    {
        char *argv[] = { package_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(bundle_path));
    ASSERT(unlink(package_source_path) == 0);

    write_text_file(consumer_manifest,
                    "[package]\n"
                    "name: \"lsp_hover_consumer\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "lsp_hover_types: \"../package/build/pkg/lsp_hover_types-0.1.0.fb\"\n");
    write_text_file(consumer_source_path, kConsumerSource);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "ref: RefType",
                                                    strlen("ref: "),
                                                    "Kind: Reference Type");
    ASSERT(strstr(output,
                  "type RefType {...}\\n\\nKind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "ref.copy(ref)",
                                                    strlen("ref."),
                                                    "func copy(value: RefType): RefType");
    ASSERT(strstr(output, "func copy(value: RefType): RefType") != NULL);
    ASSERT(strstr(output, "test.lsp.hoverpkg.RefType") == NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "value: ValueType",
                                                    strlen("value: "),
                                                    "Kind: Value Type");
    ASSERT(strstr(output,
                  "type ValueType {...}\\n\\nKind: Value Type") != NULL);
    ASSERT(strstr(output, "@value type ValueType") == NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "tuple: TupleType",
                                                    strlen("tuple: "),
                                                    "Kind: Tuple Type");
    ASSERT(strstr(output,
                  "type TupleType(i32, string);\\n\\nKind: Tuple Type") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "state: State",
                                                    strlen("state: "),
                                                    "Kind: Enum");
    ASSERT(strstr(output, "enum State\\n\\nKind: Enum") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "State.Done",
                                                    strlen("State."),
                                                    "Kind: Enum");
    ASSERT(strstr(output, "Done") != NULL);
    ASSERT(strstr(output, "Kind: Enum") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "let constructed = RefType();",
                                                    strlen("let constructed = "),
                                                    "Kind: Reference Type");
    ASSERT(strstr(output, "Kind: Reference Type") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "object: ObjectShape",
                                                    strlen("object: "),
                                                    "Kind: Object Spec");
    ASSERT(strstr(output,
                  "spec ObjectShape {...}\\n\\nKind: Object Spec") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "callback: CallbackShape",
                                                    strlen("callback: "),
                                                    "Kind: Callback Spec");
    ASSERT(strstr(output,
                  "spec CallbackShape(input: i32): string;\\n\\nKind: Callback Spec") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "unionValue: UnionShape",
                                                    strlen("unionValue: "),
                                                    "Kind: Union Spec");
    ASSERT(strstr(output,
                  "spec UnionShape: RefType | ValueType;\\n\\nKind: Union Spec") != NULL);
    ASSERT(strstr(output, "test.lsp.hoverpkg.RefType") == NULL);
    free(output);

    output = capture_lsp_position_response_at_path(consumer_source_path,
                                                    kConsumerSource,
                                                    kInitialize,
                                                    "textDocument/hover",
                                                    "intersectionValue: IntersectionShape",
                                                    strlen("intersectionValue: "),
                                                    "Kind: Intersection Spec");
    ASSERT(strstr(output,
                  "spec IntersectionShape: ObjectShape & OtherShape;\\n\\nKind: Intersection Spec") != NULL);
    ASSERT(strstr(output, "test.lsp.hoverpkg.ObjectShape") == NULL);
    free(output);

    free(consumer_source_path);
    free(consumer_src_dir);
    free(consumer_manifest);
    free(consumer_dir);
    free(bundle_path);
    free(package_source_path);
    free(package_src_dir);
    free(package_manifest);
    free(package_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* §1.8 keyword completion regression tests.
 * Each test verifies that context-aware keywords appear (or do not
 * appear) at the expected grammar position. */
static void test_lsp_keyword_completion_top_decl_position(void) {
    static const char *kSource =
        "module test.lsp.kw.topdecl;\n"
        "\n"
        "\n";
    /* Cursor on the empty line after `module` declaration → TOP_DECL. */
    const char *labels[] = {"func", "type", "enum", "spec", "fit", "import"};

    assert_lsp_completion_contains_labels(kSource, "\n\n", 1U, labels, 6U);
}

static void test_lsp_keyword_completion_body_position(void) {
    static const char *kSource =
        "module test.lsp.kw.body;\n"
        "\n"
        "func run(): void {\n"
        "    \n"
        "}\n";
    /* Cursor inside function body → BODY keywords. */
    const char *labels[] = {"let", "var", "if", "while", "for", "return", "break", "defer"};

    assert_lsp_completion_contains_labels(kSource, "    \n}", 4U, labels, 8U);
}

static void test_lsp_keyword_completion_member_position(void) {
    static const char *kSource =
        "module test.lsp.kw.member;\n"
        "\n"
        "type User {\n"
        "    \n"
        "}\n";
    /* Cursor inside type body → MEMBER keywords. */
    const char *labels[] = {"func", "let", "var", "static"};

    assert_lsp_completion_contains_labels(kSource, "    \n}", 4U, labels, 4U);
}

static void test_lsp_keyword_completion_enum_body_no_keywords(void) {
    static const char *kSource =
        "module test.lsp.kw.enumbody;\n"
        "\n"
        "enum Color {\n"
        "    \n"
        "}\n";
    /* Cursor inside enum body → OTHER position, no keyword items. */
    char *output = capture_lsp_completion_response(kSource, "    \n}", 4U);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    /* Enum body should not contain `func` or `let` keyword items. */
    ASSERT(strstr(output, "\"label\":\"func\"") == NULL);
    ASSERT(strstr(output, "\"label\":\"let\"") == NULL);
    free(output);
}

static void test_lsp_keyword_completion_member_access_no_keywords(void) {
    static const char *kSource =
        "module test.lsp.kw.memberaccess;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let u: User = User { name: \"x\" };\n"
        "    let n = u.;\n"
        "}\n";
    /* Cursor after `u.` is a member access → no keyword items. */
    char *output = capture_lsp_completion_response(kSource, "u.;", 2U);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"label\":\"name\"") != NULL);
    /* Keywords should not appear in member access completion. */
    ASSERT(strstr(output, "\"label\":\"return\"") == NULL);
    ASSERT(strstr(output, "\"label\":\"while\"") == NULL);
    free(output);
}

/* §3.4 Snippet completion regression tests.
 * Verifies that keyword items with snippet templates include insertText
 * and insertTextFormat: 2, and that keywords don't have duplicate items. */
static void test_lsp_snippet_completion_body_if_insert_text(void) {
    static const char *kSource =
        "module test.lsp.snippet.if;\n"
        "\n"
        "func run(): void {\n"
        "    \n"
        "}\n";
    /* Cursor inside function body → BODY position.
     * The `if` item should have insertText and insertTextFormat: 2. */
    char *output = capture_lsp_completion_response(kSource, "    \n}", 4U);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    /* Verify `if` item contains insertText with snippet template. */
    ASSERT(strstr(output, "\"label\":\"if\"") != NULL);
    ASSERT(strstr(output, "\"insertText\":\"if ${1:condition} {\\n\\t$0\\n}\"") != NULL);
    ASSERT(strstr(output, "\"insertTextFormat\":2") != NULL);
    free(output);
}

static void test_lsp_snippet_completion_no_duplicate_items(void) {
    static const char *kSource =
        "module test.lsp.snippet.nodup;\n"
        "\n"
        "func run(): void {\n"
        "    \n"
        "}\n";
    /* Cursor inside function body → BODY position.
     * Each keyword should appear exactly once (no duplicate plain-text + snippet). */
    char *output = capture_lsp_completion_response(kSource, "    \n}", 4U);
    char *first_occurrence;
    char *second_occurrence;

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    /* Verify `if` appears only once (as snippet, not as both plain-text and snippet). */
    first_occurrence = strstr(output, "\"label\":\"if\"");
    ASSERT(first_occurrence != NULL);
    second_occurrence = strstr(first_occurrence + 1, "\"label\":\"if\"");
    ASSERT(second_occurrence == NULL);
    /* Verify `let` appears only once (as snippet). */
    first_occurrence = strstr(output, "\"label\":\"let\"");
    ASSERT(first_occurrence != NULL);
    second_occurrence = strstr(first_occurrence + 1, "\"label\":\"let\"");
    ASSERT(second_occurrence == NULL);
    free(output);
}

/* §2.6 annotation completion regression tests.
 * Verifies that '@' triggers annotation completion and that prefix
 * filtering returns only matching items. */
static void test_lsp_annotation_completion_all(void) {
    static const char *kSource =
        "module test.lsp.annotation.all;\n"
        "\n"
        "@\n";
    /* Cursor right after '@' → all 7 annotations. */
    const char *labels[] = {"abi", "cdecl", "stdcall", "fastcall", "runtime", "iterable", "iterator"};

    assert_lsp_completion_contains_labels(kSource, "@\n", 1U, labels, 7U);
}

static void test_lsp_annotation_completion_filter_prefix(void) {
    static const char *kSource =
        "module test.lsp.annotation.filter;\n"
        "\n"
        "@a\n";
    /* Cursor after '@a' → only 'abi' matches. */
    char *output = capture_lsp_completion_response(kSource, "@a\n", 2U);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"label\":\"abi\"") != NULL);
    /* Non-matching annotations must not appear. */
    ASSERT(strstr(output, "\"label\":\"cdecl\"") == NULL);
    ASSERT(strstr(output, "\"label\":\"runtime\"") == NULL);
    free(output);
}

/* Verifies completion and hover metadata for the mixable builtin annotation. */
static void test_lsp_mixable_annotation_completion_and_hover(void) {
    static const char *kCompletionSource =
        "module test.lsp.annotation.mixable_completion;\n"
        "\n"
        "@mix\n";
    static const char *kHoverSource =
        "module test.lsp.annotation.mixable_hover;\n"
        "spec Widget {}\n"
        "type View: Widget {\n"
        "    @mixable\n"
        "    static func draw(target: Widget): void {}\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char *output = capture_lsp_completion_response(
        kCompletionSource, "@mix\n", strlen("@mix"));

    ASSERT(strstr(output, "\"label\":\"mixable\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"runtime\"") == NULL);
    free(output);

    output = capture_lsp_hover_response(kHoverSource,
                                        kInitialize,
                                        "@mixable",
                                        strlen("@mix"));
    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output, "@mixable") != NULL);
    ASSERT(strstr(output, "mixable static method annotation") != NULL);
    free(output);
}

/* Verifies generated mixin members participate in ordinary LSP surfaces and
 * go-to-definition follows their source-member mapping. */
static void test_lsp_mixin_member_completion_hover_and_definition(void) {
    static const char *kSource =
        "module test.lsp.mixin_members;\n"
        "\n"
        "spec Widget {\n"
        "    func draw(area: int): int;\n"
        "}\n"
        "\n"
        "type View: Widget {\n"
        "    open let width: int;\n"
        "\n"
        "    @mixable\n"
        "    open static func draw(target: Widget, area: int): int {\n"
        "        return area;\n"
        "    }\n"
        "}\n"
        "\n"
        "type Button: Widget {\n"
        "    ...: View;\n"
        "}\n"
        "\n"
        "func inspect(button: Button): int {\n"
        "    let width: int = button.width;\n"
        "    return button.draw(width);\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_mixin_members_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *completion;
    char *hover_field;
    char *definition_field;
    char *definition_method;
    char *shutdown;
    char *output;
    char *expected_field_definition;
    char *expected_method_definition;
    unsigned int field_line;
    unsigned int field_character;
    unsigned int method_line;
    unsigned int method_character;
    unsigned int source_field_line;
    unsigned int source_field_character;
    unsigned int source_method_line;
    unsigned int source_method_character;
    char *remove_error = NULL;
    const char *requests[6];

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);
    find_line_character(kSource,
                        "button.width",
                        strlen("button."),
                        &field_line,
                        &field_character);
    find_line_character(kSource,
                        "button.draw",
                        strlen("button."),
                        &method_line,
                        &method_character);
    find_line_character(kSource,
                        "open let width",
                        strlen("open let "),
                        &source_field_line,
                        &source_field_character);
    find_line_character(kSource,
                        "open static func draw",
                        strlen("open static func "),
                        &source_method_line,
                        &source_method_character);

    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
        uri,
        escaped_text);
    completion = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
        uri,
        field_line,
        field_character);
    hover_field = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
        uri,
        field_line,
        field_character);
    definition_field = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
        uri,
        field_line,
        field_character);
    definition_method = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}",
        uri,
        method_line,
        method_character);
    shutdown = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");
    expected_field_definition = dup_printf(
        "\"id\":4,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
        uri,
        source_field_line,
        source_field_character);
    expected_method_definition = dup_printf(
        "\"id\":5,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
        uri,
        source_method_line,
        source_method_character);

    requests[0] = completion;
    requests[1] = hover_field;
    requests[2] = definition_field;
    requests[3] = definition_method;
    requests[4] = shutdown;
    requests[5] = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";
    output = run_lsp_server_capture_after_position_ready(initialize,
                                                          did_open,
                                                          NULL,
                                                          "textDocument/completion",
                                                          uri,
                                                          field_line,
                                                          field_character,
                                                          "\"label\":\"width\"",
                                                          requests,
                                                          6U,
                                                          NULL);

    ASSERT(strstr(output, "\"id\":2,\"result\":[") != NULL);
    ASSERT(strstr(output, "\"label\":\"width\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"draw\"") != NULL);
    ASSERT(strstr(output, "\"id\":3,\"result\":null") == NULL);
    ASSERT(strstr(output, "let width:") != NULL);
    ASSERT(strstr(output, expected_field_definition) != NULL);
    ASSERT(strstr(output, expected_method_definition) != NULL);

    free(output);
    free(expected_method_definition);
    free(expected_field_definition);
    free(shutdown);
    free(definition_method);
    free(definition_field);
    free(hover_field);
    free(completion);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

/* Direct mix authorization exposes seal mixable type/fit methods to
 * completion, while Hover and definition retain the ordinary source-member
 * mapping used by generated wrappers. */
static void test_lsp_mixable_seal_authorization_hover_and_definition(void) {
    static const char *kSource =
        "module test.lsp.mixin_seal_authorization;\n"
        "\n"
        "spec Widget {}\n"
        "\n"
        "type View: Widget {\n"
        "    @mixable seal static func draw(target: Widget, area: int): int {\n"
        "        return area + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "type FitView: Widget {}\n"
        "\n"
        "open fit FitView {\n"
        "    @mixable seal static func paint(target: Widget, area: int): int {\n"
        "        return area + 2;\n"
        "    }\n"
        "    @mixable open static func publicPaint(target: Widget, area: int): int {\n"
        "        return area + 3;\n"
        "    }\n"
        "}\n"
        "\n"
        "type Button: Widget {\n"
        "    ...: View;\n"
        "    open static func invoke(area: int): int {\n"
        "        return View.draw(Button(), area);\n"
        "    }\n"
        "}\n"
        "\n"
        "type FitButton: Widget {\n"
        "    ...: FitView;\n"
        "    open static func invoke(area: int): int {\n"
        "        return FitView.paint(FitButton(), area);\n"
        "    }\n"
        "}\n"
        "\n"
        "type Other: Widget {\n"
        "    open static func inspect(area: int): int {\n"
        "        return FitView.publicPaint(Other(), area);\n"
        "    }\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char template_path[] = "temp/feng_cli_lsp_mixin_seal_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *output;
    char *expected_definition;
    unsigned int source_line;
    unsigned int source_character;
    char *remove_error = NULL;

    output = capture_lsp_completion_response(kSource,
                                             "View.draw(Button(), area)",
                                             strlen("View."));
    ASSERT(strstr(output, "\"label\":\"draw\"") != NULL);
    free(output);

    output = capture_lsp_completion_response(kSource,
                                             "FitView.paint(FitButton(), area)",
                                             strlen("FitView."));
    ASSERT(strstr(output, "\"label\":\"paint\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"publicPaint\"") != NULL);
    free(output);

    output = capture_lsp_completion_response(kSource,
                                             "FitView.publicPaint(Other(), area)",
                                             strlen("FitView."));
    ASSERT(strstr(output, "\"label\":\"publicPaint\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"paint\"") == NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "View.draw(Button(), area)",
                                        strlen("View."));
    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output,
                  "func draw(target: Widget, area:") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    ...: View;",
                                        strlen("    "));
    ASSERT(strstr(output,
                  "static func draw(target: Widget, area:") != NULL);
    ASSERT(strstr(output, "func draw(area:") != NULL);
    free(output);

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);
    uri = file_uri_from_path(source_path);
    find_line_character(kSource,
                        "@mixable seal static func draw",
                        strlen("@mixable seal static func "),
                        &source_line,
                        &source_character);
    expected_definition = dup_printf(
        "\"id\":2,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u}",
        uri,
        source_line,
        source_character);
    output = capture_lsp_position_response_at_path(source_path,
                                                    kSource,
                                                    kInitialize,
                                                    "textDocument/definition",
                                                    "View.draw(Button(), area)",
                                                    strlen("View."),
                                                    "\"result\":{\"uri\":");
    ASSERT(strstr(output, expected_definition) != NULL);

    free(output);
    free(expected_definition);
    free(uri);
    free(source_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Verifies that Hover on a mixin directive presents its complete generated
 * surface while every source-side syntax position reuses ordinary Hover. */
static void test_lsp_mixin_declaration_and_source_hover(void) {
    static const char *kSource =
        "module test.lsp.mixin_declaration_hover;\n"
        "\n"
        "spec Widget {\n"
        "    func draw(area: int): int;\n"
        "}\n"
        "\n"
        "open let replacement: string = \"literal\";\n"
        "\n"
        "open func seed(): int { return 9; }\n"
        "\n"
        "type View: Widget {\n"
        "    open var mutableValue: int = 3;\n"
        "    open var label: string = \"view\";\n"
        "    seal let hidden: int = 1;\n"
        "    open static let shared: int = 2;\n"
        "\n"
        "    open func localOnly(): int { return 0; }\n"
        "    open static func ignored(target: Widget): void {}\n"
        "\n"
        "    @mixable\n"
        "    open static func draw(target: Widget, area: int): int {\n"
        "        return area;\n"
        "    }\n"
        "\n"
        "    func View(value: int) { self.mutableValue = value; }\n"
        "}\n"
        "\n"
        "type Extra {\n"
        "    open let extra: bool;\n"
        "}\n"
        "\n"
        "type Button: Widget {\n"
        "    ...: View = View(seed()) {\n"
        "        mutableValue: 12,\n"
        "        label: replacement\n"
        "    };\n"
        "    ...: Extra;\n"
        "    open var label: string;\n"
        "}\n"
        "\n"
        "type Inferred: Widget {\n"
        "    ... = View(seed());\n"
        "}\n";
    static const char *kPlaintextInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"hover\":{\"contentFormat\":[\"plaintext\"]}}}}}";
    static const char *kMarkdownInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]}}}}}";
    const char *integer_type = sizeof(void *) >= 8U ? "i64" : "i32";
    char *expected_field = dup_printf("var mutableValue: %s", integer_type);
    char *expected_wrappers = dup_printf(
        "static func draw(target: Widget, area: %s): %s\\n"
        "func draw(area: %s): %s",
        integer_type,
        integer_type,
        integer_type,
        integer_type);
    char *expected_markdown = dup_printf(
        "```feng\\nvar mutableValue: %s\\n"
        "static func draw(target: Widget, area: %s): %s\\n"
        "func draw(area: %s): %s\\n```",
        integer_type,
        integer_type,
        integer_type,
        integer_type,
        integer_type);
    char *expected_constructor = dup_printf("ctor View(value: %s): void",
                                             integer_type);
    char *expected_seed = dup_printf("func seed(): %s", integer_type);
    char *output;

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "    ...: View = View(seed()) {",
                                        strlen("    "));
    ASSERT(strstr(output, "\"id\":2,\"result\":null") == NULL);
    ASSERT(strstr(output, expected_field) != NULL);
    ASSERT(strstr(output, expected_wrappers) != NULL);
    ASSERT(strstr(output, "var label: string") == NULL);
    ASSERT(strstr(output, "hidden") == NULL);
    ASSERT(strstr(output, "shared") == NULL);
    ASSERT(strstr(output, "localOnly") == NULL);
    ASSERT(strstr(output, "ignored") == NULL);
    ASSERT(strstr(output, "extra") == NULL);
    ASSERT(strstr(output, "member mix") == NULL);
    ASSERT(strstr(output, "source: View") == NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kMarkdownInitialize,
                                        "    ...: View = View(seed()) {",
                                        strlen("    "));
    ASSERT(strstr(output, expected_markdown) != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "    ...: View = View(seed()) {",
                                        strlen("    ...: "));
    ASSERT(strstr(output, "type View") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "    ...: View = View(seed()) {",
                                        strlen("    ...: View = "));
    ASSERT(strstr(output, expected_constructor) != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "    ...: View = View(seed()) {",
                                        strlen("    ...: View = View("));
    ASSERT(strstr(output, expected_seed) != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "        mutableValue: 12,",
                                        strlen("        "));
    ASSERT(strstr(output, expected_field) != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "        label: replacement",
                                        strlen("        "));
    ASSERT(strstr(output, "var label: string") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "        label: replacement",
                                        strlen("        label: "));
    ASSERT(strstr(output, "let replacement: string") != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kPlaintextInitialize,
                                        "    ... = View(seed());",
                                        strlen("    ... = "));
    ASSERT(strstr(output, expected_constructor) != NULL);
    ASSERT(strstr(output, "type View") == NULL);
    free(output);
    free(expected_seed);
    free(expected_constructor);
    free(expected_markdown);
    free(expected_wrappers);
    free(expected_field);
}

/* Verifies aggregate mixin Hover uses final substituted and transitive target
 * members, including wrappers contributed by a visible fit. */
static void test_lsp_mixin_hover_generics_multilevel_and_fit(void) {
    static const char *kSource =
        "module test.lsp.mixin_hover_final_surface;\n"
        "\n"
        "open type GenericView<T> {\n"
        "    open var value: T;\n"
        "}\n"
        "\n"
        "type GenericButton {\n"
        "    ...: GenericView<int>;\n"
        "}\n"
        "\n"
        "spec Widget {\n"
        "    func draw(area: int): int;\n"
        "}\n"
        "\n"
        "type View: Widget {\n"
        "    open let width: int;\n"
        "    @mixable\n"
        "    open static func draw(target: Widget, area: int): int {\n"
        "        return area;\n"
        "    }\n"
        "}\n"
        "\n"
        "type Layer: Widget {\n"
        "    ...: View;\n"
        "}\n"
        "\n"
        "type Leaf: Widget {\n"
        "    ...: Layer;\n"
        "}\n"
        "\n"
        "type FitView: Widget {}\n"
        "\n"
        "open fit FitView {\n"
        "    @mixable\n"
        "    open static func draw(target: Widget, area: int): int {\n"
        "        return area + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "type FitButton: Widget {\n"
        "    ...: FitView;\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    const char *integer_type = sizeof(void *) >= 8U ? "i64" : "i32";
    char *expected_generic = dup_printf("var value: %s", integer_type);
    char *expected_width = dup_printf("let width: %s", integer_type);
    char *expected_static = dup_printf(
        "static func draw(target: Widget, area: %s): %s",
        integer_type,
        integer_type);
    char *expected_instance = dup_printf("func draw(area: %s): %s",
                                         integer_type,
                                         integer_type);
    char *output;

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    ...: GenericView<int>;",
                                        strlen("    "));
    ASSERT(strstr(output, expected_generic) != NULL);
    ASSERT(strstr(output, "var value: T") == NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    ...: Layer;",
                                        strlen("    "));
    ASSERT(strstr(output, expected_width) != NULL);
    ASSERT(strstr(output, expected_static) != NULL);
    ASSERT(strstr(output, expected_instance) != NULL);
    free(output);

    output = capture_lsp_hover_response(kSource,
                                        kInitialize,
                                        "    ...: FitView;",
                                        strlen("    "));
    ASSERT(strstr(output, expected_static) != NULL);
    ASSERT(strstr(output, expected_instance) != NULL);
    free(output);
    free(expected_instance);
    free(expected_static);
    free(expected_width);
    free(expected_generic);
}

/* Verifies mixin Hover is identical for a source type in another project
 * source module and for a source type loaded from an external .fb package. */
static void test_lsp_mixin_hover_cross_module_and_package(void) {
    static const char *kPackageSource =
        "open module test.lsp.mixin_hover_package;\n"
        "\n"
        "open spec PackageWidget {\n"
        "    func draw(area: int): int;\n"
        "}\n"
        "\n"
        "open type PackageView: PackageWidget {\n"
        "    open var packageValue: int;\n"
        "    @mixable\n"
        "    open static func draw(target: PackageWidget, area: int): int {\n"
        "        return area;\n"
        "    }\n"
        "    open func PackageView(value: int) { self.packageValue = value; }\n"
        "}\n";
    static const char *kSharedSource =
        "open module test.lsp.mixin_hover_shared;\n"
        "\n"
        "open spec LocalWidget {\n"
        "    func draw(area: int): int;\n"
        "}\n"
        "\n"
        "open type LocalView: LocalWidget {\n"
        "    open let localValue: string;\n"
        "    @mixable\n"
        "    open static func draw(target: LocalWidget, area: int): int {\n"
        "        return area;\n"
        "    }\n"
        "}\n";
    static const char *kMainSource =
        "module test.lsp.mixin_hover_consumer;\n"
        "import test.lsp.mixin_hover_shared;\n"
        "import test.lsp.mixin_hover_package;\n"
        "\n"
        "type LocalButton: LocalWidget {\n"
        "    ...: LocalView;\n"
        "}\n"
        "\n"
        "type PackageButton: PackageWidget {\n"
        "    ...: PackageView = PackageView(3) {\n"
        "        packageValue: 4\n"
        "    };\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    const char *integer_type = sizeof(void *) >= 8U ? "i64" : "i32";
    char template_path[] = "temp/feng_lsp_mixin_hover_cross_package_XXXXXX";
    char *workspace_dir;
    char *package_dir;
    char *package_manifest;
    char *package_src_dir;
    char *package_source_path;
    char *bundle_path;
    char *consumer_dir;
    char *consumer_manifest;
    char *consumer_src_dir;
    char *shared_path;
    char *main_path;
    char *output;
    char *expected_local_static;
    char *expected_package_field;
    char *expected_package_static;
    char *expected_constructor;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    package_dir = path_join(workspace_dir, "package");
    package_manifest = path_join(package_dir, "feng.fm");
    package_src_dir = path_join(package_dir, "src");
    package_source_path = path_join(package_src_dir, "view.ff");
    bundle_path = path_join(package_dir,
                            "build/pkg/lsp_mixin_hover_package-0.1.0.fb");
    consumer_dir = path_join(workspace_dir, "consumer");
    consumer_manifest = path_join(consumer_dir, "feng.fm");
    consumer_src_dir = path_join(consumer_dir, "src");
    shared_path = path_join(consumer_src_dir, "shared.ff");
    main_path = path_join(consumer_src_dir, "main.ff");

    mkdir_p(package_src_dir);
    mkdir_p(consumer_src_dir);
    write_text_file(package_manifest,
                    "[package]\n"
                    "name: \"lsp_mixin_hover_package\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n");
    write_text_file(package_source_path, kPackageSource);
    {
        char *argv[] = { package_dir };
        ASSERT(feng_cli_project_pack_main("feng", 1, argv) == 0);
    }
    ASSERT(path_exists(bundle_path));

    write_text_file(consumer_manifest,
                    "[package]\n"
                    "name: \"lsp_mixin_hover_consumer\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "lsp_mixin_hover_package: "
                    "\"../package/build/pkg/lsp_mixin_hover_package-0.1.0.fb\"\n");
    write_text_file(shared_path, kSharedSource);
    write_text_file(main_path, kMainSource);
    expected_local_static = dup_printf(
        "static func draw(target: LocalWidget, area: %s): %s",
        integer_type,
        integer_type);
    expected_package_field = dup_printf("var packageValue: %s", integer_type);
    expected_package_static = dup_printf(
        "static func draw(target: PackageWidget, area: %s): %s",
        integer_type,
        integer_type);
    expected_constructor = dup_printf("ctor PackageView(value: %s): void",
                                       integer_type);

    output = capture_lsp_position_response_at_path(
        main_path,
        kMainSource,
        kInitialize,
        "textDocument/hover",
        "    ...: LocalView;",
        strlen("    "),
        "let localValue: string");
    ASSERT(strstr(output, "let localValue: string") != NULL);
    ASSERT(strstr(output, expected_local_static) != NULL);
    if (sizeof(void *) >= 8U) {
        ASSERT(strstr(output, "func draw(area: i64): i64") != NULL);
    } else {
        ASSERT(strstr(output, "func draw(area: i32): i32") != NULL);
    }
    free(output);

    output = capture_lsp_position_response_at_path(
        main_path,
        kMainSource,
        kInitialize,
        "textDocument/hover",
        "    ...: LocalView;",
        strlen("    ...: "),
        "type LocalView");
    ASSERT(strstr(output, "type LocalView") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(
        main_path,
        kMainSource,
        kInitialize,
        "textDocument/hover",
        "    ...: PackageView = PackageView(3) {",
        strlen("    "),
        expected_package_field);
    ASSERT(strstr(output, expected_package_field) != NULL);
    ASSERT(strstr(output, expected_package_static) != NULL);
    if (sizeof(void *) >= 8U) {
        ASSERT(strstr(output, "func draw(area: i64): i64") != NULL);
    } else {
        ASSERT(strstr(output, "func draw(area: i32): i32") != NULL);
    }
    free(output);

    output = capture_lsp_position_response_at_path(
        main_path,
        kMainSource,
        kInitialize,
        "textDocument/hover",
        "    ...: PackageView = PackageView(3) {",
        strlen("    ...: "),
        "type PackageView");
    ASSERT(strstr(output, "type PackageView") != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(
        main_path,
        kMainSource,
        kInitialize,
        "textDocument/hover",
        "    ...: PackageView = PackageView(3) {",
        strlen("    ...: PackageView = "),
        expected_constructor);
    ASSERT(strstr(output, expected_constructor) != NULL);
    free(output);

    output = capture_lsp_position_response_at_path(
        main_path,
        kMainSource,
        kInitialize,
        "textDocument/hover",
        "        packageValue: 4",
        strlen("        "),
        expected_package_field);
    ASSERT(strstr(output, expected_package_field) != NULL);
    free(output);

    free(expected_constructor);
    free(expected_package_static);
    free(expected_package_field);
    free(expected_local_static);
    free(main_path);
    free(shared_path);
    free(consumer_src_dir);
    free(consumer_manifest);
    free(consumer_dir);
    free(bundle_path);
    free(package_source_path);
    free(package_src_dir);
    free(package_manifest);
    free(package_dir);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Package completion must recover the seal mix capability from .ft without
 * turning it into an ordinary public method. */
static void test_lsp_package_mixable_seal_completion_respects_direct_target(void) {
    static const char *kProviderSource =
        "open module test.lsp.mixin_seal_package;\n"
        "open spec Widget {}\n"
        "open type View: Widget {\n"
        "    @mixable seal static func draw(target: Widget, area: int): int {\n"
        "        return area + 1;\n"
        "    }\n"
        "    @mixable open static func publicDraw(target: Widget, area: int): int {\n"
        "        return area + 2;\n"
        "    }\n"
        "    seal static func ordinary(target: Widget, area: int): int {\n"
        "        return area + 3;\n"
        "    }\n"
        "}\n";
    static const char *kAuthorizedSource =
        "module test.lsp.mixin_seal_consumer;\n"
        "import test.lsp.mixin_seal_package;\n"
        "type Button: Widget {\n"
        "    ...: View;\n"
        "    open static func inspect(area: int): int {\n"
        "        return View.draw(Button(), area);\n"
        "    }\n"
        "}\n";
    static const char *kUnauthorizedSource =
        "module test.lsp.mixin_seal_consumer;\n"
        "import test.lsp.mixin_seal_package;\n"
        "type Other: Widget {\n"
        "    open static func inspect(area: int): int {\n"
        "        return View.publicDraw(Other(), area);\n"
        "    }\n"
        "}\n";
    static const char *kInitialize =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}";
    char template_path[] = "temp/feng_lsp_package_mixin_seal_XXXXXX";
    char *workspace_dir;
    char *bundle_path;
    char *provider_source_path;
    char *consumer_dir;
    char *consumer_manifest;
    char *consumer_src_dir;
    char *consumer_source_path;
    char *output;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    bundle_path = build_single_source_package_bundle(workspace_dir,
                                                     "lsp_mixable_seal",
                                                     kProviderSource);
    provider_source_path = path_join(workspace_dir, "dep/src/dep.ff");
    ASSERT(unlink(provider_source_path) == 0);
    consumer_dir = path_join(workspace_dir, "consumer");
    consumer_manifest = path_join(consumer_dir, "feng.fm");
    consumer_src_dir = path_join(consumer_dir, "src");
    consumer_source_path = path_join(consumer_src_dir, "main.ff");
    mkdir_p(consumer_src_dir);
    write_text_file(consumer_manifest,
                    "[package]\n"
                    "name: \"lsp_mixable_seal_consumer\"\n"
                    "version: \"0.1.0\"\n"
                    "target: \"lib\"\n"
                    "src: \"src/\"\n"
                    "out: \"build/\"\n"
                    "\n"
                    "[dependencies]\n"
                    "lsp_mixable_seal: \"../lsp_mixable_seal.fb\"\n");

    write_text_file(consumer_source_path, kAuthorizedSource);
    output = capture_lsp_position_response_at_path(
        consumer_source_path,
        kAuthorizedSource,
        kInitialize,
        "textDocument/completion",
        "View.draw(Button(), area)",
        strlen("View."),
        "\"label\":\"draw\"");
    ASSERT(strstr(output, "\"label\":\"draw\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"publicDraw\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"ordinary\"") == NULL);
    free(output);

    write_text_file(consumer_source_path, kUnauthorizedSource);
    output = capture_lsp_position_response_at_path(
        consumer_source_path,
        kUnauthorizedSource,
        kInitialize,
        "textDocument/completion",
        "View.publicDraw(Other(), area)",
        strlen("View."),
        "\"label\":\"publicDraw\"");
    ASSERT(strstr(output, "\"label\":\"publicDraw\"") != NULL);
    ASSERT(strstr(output, "\"label\":\"draw\"") == NULL);
    ASSERT(strstr(output, "\"label\":\"ordinary\"") == NULL);
    free(output);

    free(consumer_source_path);
    free(consumer_src_dir);
    free(consumer_manifest);
    free(consumer_dir);
    free(provider_source_path);
    free(bundle_path);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
}

/* Verifies mixin conflicts publish their original source declaration through
 * standard LSP diagnostic relatedInformation. */
static void test_lsp_mixin_diagnostic_related_information(void) {
    static const char *kSource =
        "module test.lsp.mixin_diagnostic;\n"
        "spec Widget { func draw(area: int): int; }\n"
        "type View: Widget {\n"
        "    @mixable\n"
        "    open static func draw(target: Widget, area: int): int { return area; }\n"
        "}\n"
        "type Button: Widget {\n"
        "    ...: View;\n"
        "    func draw(area: int): int { return area; }\n"
        "}\n";
    char template_path[] = "temp/feng_cli_lsp_mixin_diag_XXXXXX";
    char *workspace_dir;
    char *source_path;
    char *uri;
    char *escaped_text;
    char *initialize;
    char *did_open;
    char *did_save;
    char *shutdown;
    char *output;
    char *diagnostic_output;
    const char *requests[2];
    unsigned int line;
    unsigned int character;
    char *remove_error = NULL;

    workspace_dir = mkdtemp(template_path);
    ASSERT(workspace_dir != NULL);
    source_path = path_join(workspace_dir, "main.ff");
    write_text_file(source_path, kSource);
    find_line_character(kSource,
                        "...: View;",
                        0U,
                        &line,
                        &character);
    uri = file_uri_from_path(source_path);
    escaped_text = json_escape_text(kSource);
    initialize = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
    did_open = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"feng\",\"version\":1,\"text\":\"%s\"}}}",
        uri,
        escaped_text);
    did_save = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
        uri);
    shutdown = dup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\",\"params\":null}");

    requests[0] = shutdown;
    requests[1] = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";
    output = run_lsp_server_capture_after_position_ready(
        initialize,
        did_open,
        did_save,
        "textDocument/hover",
        uri,
        line,
        character,
        "\"relatedInformation\":[",
        requests,
        2U,
        &diagnostic_output);
    ASSERT(diagnostic_output != NULL);
    ASSERT(strstr(diagnostic_output, "\"relatedInformation\":[") != NULL);
    ASSERT(strstr(diagnostic_output, "mixin source member is declared here") != NULL);
    ASSERT(strstr(diagnostic_output, uri) != NULL);

    free(diagnostic_output);
    free(output);
    free(shutdown);
    free(did_save);
    free(did_open);
    free(initialize);
    free(escaped_text);
    free(uri);
    ASSERT(feng_cli_project_remove_tree(workspace_dir, &remove_error));
    free(remove_error);
    free(source_path);
}

int main(void) {
    (void)system("rm -rf temp");
    (void)mkdir("temp", 0755);

    test_platform_detects_complete_native_platform();
    test_manifest_defaults();
    test_manifest_parses_writes_and_validates_platform_set();
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
    test_deps_resolve_preserves_nested_local_project_dependencies();
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
    test_deps_install_uses_bundled_packages_recursively();
    test_deps_install_selects_registry_before_bundled_fallback();
    test_deps_install_http_404_uses_bundled_fallback();
    test_deps_install_validates_and_force_refreshes_bundled_package();
    test_deps_install_local_dependencies_use_bundled_transitives();
    test_project_build_installs_bundled_package_into_cache();
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
    test_dap_resolves_backend_via_feng_lldb_dap();
    test_dap_rewrites_set_breakpoints_source_path_to_package_uri();
    test_dap_rejects_set_breakpoints_outside_debug_closure();
    test_dap_rewrites_stack_trace_source_path_to_local_path();
    test_dap_rewrites_stack_trace_compiler_normalized_source_path();
    test_dap_hides_hidden_stack_trace_frames();
    test_dap_rewrites_variables_to_feng_names();
    test_dap_filters_backend_variables_and_rewrites_user_values();
    test_project_build_rewrites_module_binding_in_dap_globals();
    test_dap_reads_exact_string_bytes_without_backend_string_formatting();
    test_dap_bounds_runtime_string_memory_reads();
    test_dap_uses_array_element_type_name_in_value_summary();
    test_dap_clears_synthetic_refs_after_continue();
    test_dap_expands_user_type_fields_with_synthetic_reference();
    test_project_build_keeps_for_body_breakpoint_after_init_in_dwarf();
    test_project_build_keeps_for_body_locals_after_prefix_binding();
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
    test_lsp_hover_type_categories_and_declaration_shapes();
    test_lsp_hover_lambda_scope_and_chained_members();
    test_lsp_hover_lambda_parameter_declaration_and_cache_invalidation();
    test_lsp_hover_type_category_survives_failed_edit();
    test_lsp_hover_infix_match_binding();
    test_lsp_hover_type_param();
    test_lsp_hover_type_param_extended();
    test_lsp_hover_type_param_in_spec_member();
    test_lsp_hover_uses_inferred_top_level_binding_type();
    test_lsp_signature_displays_variadic_parameter_syntax();
    test_lsp_fit_member_name_param_mutability_and_return_type_navigation();
    test_lsp_member_completion_survives_incomplete_member_access();
    test_lsp_spec_seal_member_completion_respects_implementation_domain();
    test_lsp_fit_extension_member_completion_on_builtin_string();
    test_lsp_enum_member_completion_survives_incomplete_member_access();
    test_lsp_completion_uses_source_scoped_edit_context();
    test_lsp_member_completion_infers_constructor_call_overloads();
    test_lsp_member_references_and_rename_from_object_literal_field();
    test_lsp_function_decl_site_definition_references_and_rename();
    test_lsp_type_references_cover_all_ast_positions();
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
    test_lsp_package_symbol_hover_type_categories();
    test_lsp_keyword_completion_top_decl_position();
    test_lsp_keyword_completion_body_position();
    test_lsp_keyword_completion_member_position();
    test_lsp_keyword_completion_enum_body_no_keywords();
    test_lsp_keyword_completion_member_access_no_keywords();
    test_lsp_snippet_completion_body_if_insert_text();
    test_lsp_snippet_completion_no_duplicate_items();
    test_lsp_annotation_completion_all();
    test_lsp_annotation_completion_filter_prefix();
    test_lsp_mixable_annotation_completion_and_hover();
    test_lsp_mixin_member_completion_hover_and_definition();
    test_lsp_mixable_seal_authorization_hover_and_definition();
    test_lsp_mixin_declaration_and_source_hover();
    test_lsp_mixin_hover_generics_multilevel_and_fit();
    test_lsp_mixin_hover_cross_module_and_package();
    test_lsp_package_mixable_seal_completion_respects_direct_target();
    test_lsp_mixin_diagnostic_related_information();
    test_direct_options_default_host_and_out_and_accept_sysroot();
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
    test_direct_build_consumes_package_mixin();
    test_direct_build_consumes_package_mixable_seal_methods();
    test_pack_bundle_manifest_rewrites_local_dependency_versions();
    test_project_check_accepts_source_file_path_and_local_dependencies();
    test_project_check_reports_enum_semantic_error_without_unknown_type();
    test_frontend_outputs_absolute_bundle_paths();
    test_frontend_source_overlay_replaces_disk_source();
    test_frontend_source_overlay_rejects_duplicate_paths();
    test_direct_build_rejects_bad_package_bundle();
    test_project_platform_selection_rules();
    test_project_build_and_pack_multiple_platforms();
    test_project_run_rejects_platform_and_sysroot_options();
    test_project_build_default_uses_debug_friendly_flags();
    test_project_build_release_propagates_to_local_dependencies();
    test_project_build_bin_copies_assets_and_refreshes_existing_output();
    test_project_build_lib_stages_assets_under_output_root();
    test_project_build_lib_stages_extlib_assets_without_assets_layer();
    test_project_run_release_reuses_build_pipeline();
    test_project_run_collects_cross_package_generic_cycle();
    test_project_build_closes_multi_provider_generic_diamond();
    test_project_run_collects_generic_finalizer_cycle();
    test_project_pack_uses_release_build_and_public_ft_excludes_spans();
    test_project_pack_includes_staged_assets_in_bundle();
    test_project_pack_includes_extlib_assets_without_assets_layer();
    test_project_pack_rejects_release_flag();
    fprintf(stdout, "cli tests passed\n");
    return 0;
}
