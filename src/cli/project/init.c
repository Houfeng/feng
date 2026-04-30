#include "cli/cli.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct InitOptions {
    const char *package_name;
    bool target_lib;
} InitOptions;

typedef enum InitDirectoryState {
    INIT_DIRECTORY_EMPTY = 0,
    INIT_DIRECTORY_NONEMPTY,
    INIT_DIRECTORY_ERROR
} InitDirectoryState;

static const char *kBinTemplate =
    "mod %s;\n"
    "\n"
    "fn main(args: string[]) {\n"
    "}\n";

static const char *kLibTemplate =
    "mod %s;\n"
    "\n"
    "fn helper(): int {\n"
    "  return 0;\n"
    "}\n";

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s init [<name>] [--target=<bin|lib>]\n", program);
}

static char *dup_n(const char *text, size_t length) {
    char *out = (char *)malloc(length + 1U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
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
    if (needed < 0) {
        va_end(args_copy);
        return NULL;
    }

    out = (char *)malloc((size_t)needed + 1U);
    if (out == NULL) {
        va_end(args_copy);
        return NULL;
    }
    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

static bool parse_target_value(const char *value, bool *out_target_lib) {
    if (strcmp(value, "bin") == 0) {
        *out_target_lib = false;
        return true;
    }
    if (strcmp(value, "lib") == 0) {
        *out_target_lib = true;
        return true;
    }
    return false;
}

static bool parse_args(const char *program, int argc, char **argv, InitOptions *out_options) {
    int index;

    out_options->package_name = NULL;
    out_options->target_lib = false;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program);
            return false;
        }
        if (strcmp(arg, "--target") == 0) {
            const char *value;

            if (index + 1 >= argc) {
                fprintf(stderr, "--target requires `bin` or `lib`\n");
                print_usage(program);
                return false;
            }
            value = argv[++index];
            if (!parse_target_value(value, &out_options->target_lib)) {
                fprintf(stderr, "--target must be `bin` or `lib`\n");
                print_usage(program);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "--target=", 9) == 0) {
            if (!parse_target_value(arg + 9, &out_options->target_lib)) {
                fprintf(stderr, "--target must be `bin` or `lib`\n");
                print_usage(program);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program);
            return false;
        }
        if (out_options->package_name != NULL) {
            fprintf(stderr, "init accepts at most one <name> argument\n");
            print_usage(program);
            return false;
        }
        out_options->package_name = arg;
    }

    return true;
}

static char *derive_default_package_name(void) {
    char cwd_buffer[4096];
    const char *name_start;
    size_t length;

    if (getcwd(cwd_buffer, sizeof(cwd_buffer)) == NULL) {
        return NULL;
    }

    length = strlen(cwd_buffer);
    while (length > 1U && cwd_buffer[length - 1U] == '/') {
        cwd_buffer[length - 1U] = '\0';
        length--;
    }
    if (strcmp(cwd_buffer, "/") == 0) {
        return NULL;
    }

    name_start = strrchr(cwd_buffer, '/');
    name_start = name_start != NULL ? name_start + 1 : cwd_buffer;
    if (name_start[0] == '\0') {
        return NULL;
    }
    return dup_n(name_start, strlen(name_start));
}

static InitDirectoryState inspect_current_directory(char **out_error_message) {
    DIR *dir = opendir(".");
    struct dirent *entry;

    *out_error_message = NULL;
    if (dir == NULL) {
        *out_error_message = dup_printf("failed to open current directory: %s", strerror(errno));
        return INIT_DIRECTORY_ERROR;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        closedir(dir);
        return INIT_DIRECTORY_NONEMPTY;
    }

    if (closedir(dir) != 0) {
        *out_error_message = dup_printf("failed to read current directory: %s", strerror(errno));
        return INIT_DIRECTORY_ERROR;
    }
    return INIT_DIRECTORY_EMPTY;
}

static bool write_all(int fd, const char *content, size_t length) {
    size_t written = 0U;

    while (written < length) {
        ssize_t step = write(fd, content + written, length - written);
        if (step < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (size_t)step;
    }
    return true;
}

static bool write_file_exclusive(const char *path,
                                 const char *content,
                                 char **out_error_message) {
    int fd;
    size_t length = strlen(content);
    bool ok = false;

    *out_error_message = NULL;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0) {
        *out_error_message = dup_printf("failed to create %s: %s", path, strerror(errno));
        return false;
    }

    if (!write_all(fd, content, length)) {
        *out_error_message = dup_printf("failed to write %s: %s", path, strerror(errno));
        goto cleanup;
    }
    if (close(fd) != 0) {
        fd = -1;
        *out_error_message = dup_printf("failed to finalize %s: %s", path, strerror(errno));
        goto cleanup;
    }
    fd = -1;
    ok = true;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    if (!ok) {
        unlink(path);
    }
    return ok;
}

int feng_cli_project_init_main(const char *program, int argc, char **argv) {
    InitOptions options = {0};
    InitDirectoryState directory_state;
    char *directory_error = NULL;
    char *derived_name = NULL;
    const char *package_name;
    char *manifest_content = NULL;
    char *write_error = NULL;
    const char *source_path;
    char *source_template = NULL;
    bool created_src_dir = false;
    bool created_manifest = false;
    bool created_source = false;
    int rc = 1;

    if (!parse_args(program, argc, argv, &options)) {
        return 1;
    }

    directory_state = inspect_current_directory(&directory_error);
    if (directory_state == INIT_DIRECTORY_ERROR) {
        fprintf(stderr, "%s\n", directory_error != NULL ? directory_error : "failed to inspect current directory");
        free(directory_error);
        return 1;
    }
    if (directory_state == INIT_DIRECTORY_NONEMPTY) {
        fprintf(stderr, "current directory is not empty\n");
        return 1;
    }

    package_name = options.package_name;
    if (package_name == NULL) {
        derived_name = derive_default_package_name();
        if (derived_name == NULL) {
            fprintf(stderr, "failed to derive package name from current directory\n");
            return 1;
        }
        package_name = derived_name;
    }

    manifest_content = dup_printf("name:%s\nversion:0.1.0\ntarget:%s\nsrc:src/\nout:build/\n",
                                  package_name,
                                  options.target_lib ? "lib" : "bin");
    if (manifest_content == NULL) {
        fprintf(stderr, "out of memory preparing project manifest\n");
        goto cleanup;
    }

    if (mkdir("src", 0775) != 0) {
        fprintf(stderr, "failed to create src: %s\n", strerror(errno));
        goto cleanup;
    }
    created_src_dir = true;

    if (!write_file_exclusive("feng.fm", manifest_content, &write_error)) {
        fprintf(stderr, "%s\n", write_error != NULL ? write_error : "failed to create feng.fm");
        free(write_error);
        write_error = NULL;
        goto cleanup;
    }
    created_manifest = true;

    source_path = options.target_lib ? "src/lib.ff" : "src/main.ff";
    source_template = dup_printf(options.target_lib ? kLibTemplate : kBinTemplate,
                                 package_name);
    if (source_template == NULL) {
        fprintf(stderr, "out of memory preparing starter source file\n");
        goto cleanup;
    }
    if (!write_file_exclusive(source_path, source_template, &write_error)) {
        fprintf(stderr, "%s\n", write_error != NULL ? write_error : "failed to create starter source file");
        free(write_error);
        write_error = NULL;
        goto cleanup;
    }
    created_source = true;

    rc = 0;

cleanup:
    if (rc != 0) {
        if (created_source) {
            unlink(source_path);
        }
        if (created_manifest) {
            unlink("feng.fm");
        }
        if (created_src_dir) {
            rmdir("src");
        }
    }
    free(write_error);
    free(source_template);
    free(manifest_content);
    free(derived_name);
    return rc;
}
