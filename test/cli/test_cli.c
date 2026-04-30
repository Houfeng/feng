#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli/cli.h"
#include "cli/project/common.h"
#include "cli/project/manifest.h"

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
    fprintf(stdout, "cli tests passed\n");
    return 0;
}
