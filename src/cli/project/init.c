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

#include "archive/fm.h"
#include "archive/zip.h"
#include "cli/common.h"
#include "lexer/token.h"

/* Parsed options accepted by `feng init`. */
typedef struct InitOptions {
    const char *package_name;
    bool target_lib;
} InitOptions;

/* Result of checking whether the project directory can be initialized. */
typedef enum InitDirectoryState {
    INIT_DIRECTORY_EMPTY = 0,
    INIT_DIRECTORY_NONEMPTY,
    INIT_DIRECTORY_ERROR
} InitDirectoryState;

/* One direct dependency declared by a package bundled with Feng. */
typedef struct InitBundledDependency {
    char *bundle_path;
    char *name;
    char *version;
} InitBundledDependency;

/* Owned, deterministically ordered bundled dependencies discovered by init. */
typedef struct InitBundledDependencies {
    InitBundledDependency *items;
    size_t count;
} InitBundledDependencies;

static const char *kBinTemplate =
    "module %s;\n"
    "\n"
    "func main(args: string[]) {\n"
    "}\n";

static const char *kLibTemplate =
    "module %s;\n"
    "\n"
    "func helper(): int {\n"
    "  return 0;\n"
    "}\n";

static void print_usage(const char *program, FILE *stream) {
    if (stream == stderr) fprintf(stream, "\n");
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s init [<name>] [--target=<bin|lib>]\n", program);
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

/* Replace an init error message with one formatted, caller-owned string. */
static void set_init_errorf(char **out_error_message, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message;

    if (out_error_message == NULL) {
        return;
    }
    free(*out_error_message);
    *out_error_message = NULL;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return;
    }

    message = (char *)malloc((size_t)needed + 1U);
    if (message == NULL) {
        va_end(args_copy);
        return;
    }
    vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    *out_error_message = message;
}

/* Release all coordinates and paths collected from the bundled package directory. */
static void bundled_dependencies_dispose(InitBundledDependencies *dependencies) {
    size_t index;

    if (dependencies == NULL) {
        return;
    }
    for (index = 0U; index < dependencies->count; ++index) {
        free(dependencies->items[index].bundle_path);
        free(dependencies->items[index].name);
        free(dependencies->items[index].version);
    }
    free(dependencies->items);
    dependencies->items = NULL;
    dependencies->count = 0U;
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

static bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_forbidden_identifier_word(const char *text, size_t length) {
    FengTokenKind keyword_kind;

    if (feng_lookup_keyword(text, length, &keyword_kind)) {
        return true;
    }
    if (feng_is_reserved_word(text, length)) {
        return true;
    }
    return (length == 4U && strncmp(text, "true", 4U) == 0)
        || (length == 5U && strncmp(text, "false", 5U) == 0);
}

static bool ensure_capacity(char **buffer, size_t *capacity, size_t needed) {
    char *grown;
    size_t new_capacity;

    if (needed + 1U <= *capacity) {
        return true;
    }

    new_capacity = *capacity == 0U ? 32U : *capacity;
    while (needed + 1U > new_capacity) {
        new_capacity *= 2U;
    }

    grown = (char *)realloc(*buffer, new_capacity);
    if (grown == NULL) {
        return false;
    }
    *buffer = grown;
    *capacity = new_capacity;
    return true;
}

static bool append_char(char **buffer, size_t *length, size_t *capacity, char c) {
    if (!ensure_capacity(buffer, capacity, *length + 1U)) {
        return false;
    }
    (*buffer)[(*length)++] = c;
    (*buffer)[*length] = '\0';
    return true;
}

static bool append_slice(char **buffer,
                         size_t *length,
                         size_t *capacity,
                         const char *text,
                         size_t text_length) {
    if (!ensure_capacity(buffer, capacity, *length + text_length)) {
        return false;
    }
    memcpy(*buffer + *length, text, text_length);
    *length += text_length;
    (*buffer)[*length] = '\0';
    return true;
}

static char *normalize_name_segment(const char *text, size_t length) {
    size_t capacity = length * 2U + 4U;
    char *out = (char *)malloc(capacity);
    size_t out_length = 0U;
    size_t index;

    if (out == NULL) {
        return NULL;
    }

    for (index = 0U; index < length; ++index) {
        char c = text[index];

        if (is_ascii_alpha(c) || c == '_') {
            out[out_length++] = c;
            continue;
        }
        if (is_ascii_digit(c)) {
            if (out_length == 0U) {
                out[out_length++] = '_';
            }
            out[out_length++] = c;
            continue;
        }
        if (out_length == 0U || out[out_length - 1U] != '_') {
            out[out_length++] = '_';
        }
    }

    if (out_length == 0U) {
        out[out_length++] = '_';
    }
    out[out_length] = '\0';

    if (is_forbidden_identifier_word(out, out_length)) {
        memmove(out + 1U, out, out_length + 1U);
        out[0] = '_';
    }
    return out;
}

static char *normalize_package_name(const char *raw_name) {
    const char *cursor;
    const char *segment_start;
    char *normalized = NULL;
    size_t normalized_length = 0U;
    size_t normalized_capacity = 0U;

    if (raw_name == NULL) {
        return dup_n("app", 3U);
    }

    segment_start = raw_name;
    for (cursor = raw_name;; ++cursor) {
        if (*cursor == '.' || *cursor == '\0') {
            if (cursor > segment_start) {
                char *segment = normalize_name_segment(segment_start,
                                                       (size_t)(cursor - segment_start));
                size_t segment_length;

                if (segment == NULL) {
                    free(normalized);
                    return NULL;
                }
                segment_length = strlen(segment);
                if (normalized_length > 0U) {
                    if (!append_char(&normalized, &normalized_length, &normalized_capacity, '.')) {
                        free(segment);
                        free(normalized);
                        return NULL;
                    }
                }
                if (!append_slice(&normalized,
                                  &normalized_length,
                                  &normalized_capacity,
                                  segment,
                                  segment_length)) {
                    free(segment);
                    free(normalized);
                    return NULL;
                }
                free(segment);
            }
            if (*cursor == '\0') {
                break;
            }
            segment_start = cursor + 1;
        }
    }

    if (normalized_length == 0U) {
        free(normalized);
        return dup_n("app", 3U);
    }
    return normalized;
}

static FengCliParseResult parse_args(const char *program, int argc, char **argv, InitOptions *out_options) {
    int index;

    out_options->package_name = NULL;
    out_options->target_lib = false;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program, stdout);
            return FENG_CLI_PARSE_HELP;
        }
        if (strncmp(arg, "--target=", 9) == 0) {
            if (!parse_target_value(arg + 9, &out_options->target_lib)) {
                fprintf(stderr, "--target must be `bin` or `lib`\n");
                print_usage(program, stderr);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strcmp(arg, "--target") == 0) {
            fprintf(stderr, "--target requires `=<bin|lib>`\n");
            print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        if (out_options->package_name != NULL) {
            fprintf(stderr, "init accepts at most one <name> argument\n");
            print_usage(program, stderr);
            return FENG_CLI_PARSE_ERROR;
        }
        out_options->package_name = arg;
    }

    return FENG_CLI_PARSE_OK;
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

/* Return whether one directory entry has the exact `.fb` suffix. */
static bool has_bundle_extension(const char *name) {
    size_t length = strlen(name);

    return length >= 3U && strcmp(name + length - 3U, ".fb") == 0;
}

/* Join one directory and direct child name without normalizing either path. */
static char *join_child_path(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);

    return dup_printf("%s%s%s",
                      directory,
                      directory_length > 0U && directory[directory_length - 1U] == '/'
                          ? ""
                          : "/",
                      name);
}

/* Copy the required package coordinates from one parsed root manifest. */
static bool extract_bundled_coordinates(const char *bundle_path,
                                        const FengFmDocument *document,
                                        InitBundledDependency *out_dependency,
                                        char **out_error_message) {
    const char *name = NULL;
    const char *version = NULL;
    size_t index;

    for (index = 0U; index < document->entry_count; ++index) {
        const FengFmEntry *entry = &document->entries[index];

        if (strcmp(entry->section, "package") != 0) {
            continue;
        }
        if (strcmp(entry->key, "name") == 0) {
            name = entry->value;
        } else if (strcmp(entry->key, "version") == 0) {
            version = entry->value;
        }
    }

    if (name == NULL || name[0] == '\0' || version == NULL || version[0] == '\0') {
        set_init_errorf(out_error_message,
                        "bundled package manifest is missing non-empty "
                        "`[package].name` or `[package].version`: %s",
                        bundle_path);
        return false;
    }

    out_dependency->bundle_path = dup_n(bundle_path, strlen(bundle_path));
    out_dependency->name = dup_n(name, strlen(name));
    out_dependency->version = dup_n(version, strlen(version));
    if (out_dependency->bundle_path == NULL ||
        out_dependency->name == NULL ||
        out_dependency->version == NULL) {
        free(out_dependency->bundle_path);
        free(out_dependency->name);
        free(out_dependency->version);
        memset(out_dependency, 0, sizeof(*out_dependency));
        set_init_errorf(out_error_message,
                        "out of memory reading bundled package coordinates: %s",
                        bundle_path);
        return false;
    }
    return true;
}

/* Read only the root `feng.fm` needed to obtain one bundled package coordinate. */
static bool read_bundled_dependency(const char *bundle_path,
                                    InitBundledDependency *out_dependency,
                                    char **out_error_message) {
    FengZipReader reader = {0};
    FengFmDocument document = {0};
    FengFmError fm_error = {0};
    char *zip_error = NULL;
    void *manifest_bytes = NULL;
    size_t manifest_size = 0U;
    char *manifest_text = NULL;
    bool ok = false;

    memset(out_dependency, 0, sizeof(*out_dependency));
    if (!feng_zip_reader_open(bundle_path, &reader, &zip_error)) {
        set_init_errorf(out_error_message,
                        "failed to open bundled package %s: %s",
                        bundle_path,
                        zip_error != NULL ? zip_error : "unknown error");
        goto cleanup;
    }
    if (!feng_zip_reader_read(&reader,
                              "feng.fm",
                              &manifest_bytes,
                              &manifest_size,
                              &zip_error)) {
        set_init_errorf(out_error_message,
                        "failed to read bundled package manifest %s: %s",
                        bundle_path,
                        zip_error != NULL ? zip_error : "unknown error");
        goto cleanup;
    }

    if (manifest_size == SIZE_MAX) {
        set_init_errorf(out_error_message,
                        "bundled package manifest is too large: %s",
                        bundle_path);
        goto cleanup;
    }
    manifest_text = (char *)malloc(manifest_size + 1U);
    if (manifest_text == NULL) {
        set_init_errorf(out_error_message,
                        "out of memory reading bundled package manifest: %s",
                        bundle_path);
        goto cleanup;
    }
    memcpy(manifest_text, manifest_bytes, manifest_size);
    manifest_text[manifest_size] = '\0';

    if (!feng_fm_parse(bundle_path, manifest_text, &document, &fm_error)) {
        if (fm_error.line > 0U) {
            set_init_errorf(out_error_message,
                            "failed to parse bundled package manifest %s:%u: %s",
                            bundle_path,
                            fm_error.line,
                            fm_error.message != NULL ? fm_error.message : "unknown error");
        } else {
            set_init_errorf(out_error_message,
                            "failed to parse bundled package manifest %s: %s",
                            bundle_path,
                            fm_error.message != NULL ? fm_error.message : "unknown error");
        }
        goto cleanup;
    }

    ok = extract_bundled_coordinates(bundle_path,
                                     &document,
                                     out_dependency,
                                     out_error_message);

cleanup:
    free(zip_error);
    free(manifest_text);
    feng_zip_free(manifest_bytes);
    feng_fm_error_dispose(&fm_error);
    feng_fm_document_dispose(&document);
    feng_zip_reader_dispose(&reader);
    return ok;
}

/* Add one owned dependency to the collection. */
static bool append_bundled_dependency(InitBundledDependencies *dependencies,
                                      InitBundledDependency *dependency,
                                      char **out_error_message) {
    InitBundledDependency *resized = (InitBundledDependency *)realloc(
        dependencies->items,
        (dependencies->count + 1U) * sizeof(*dependencies->items));

    if (resized == NULL) {
        set_init_errorf(out_error_message, "out of memory collecting bundled packages");
        return false;
    }
    dependencies->items = resized;
    dependencies->items[dependencies->count++] = *dependency;
    memset(dependency, 0, sizeof(*dependency));
    return true;
}

/* Sort bundled dependencies by coordinate for deterministic manifest output. */
static int compare_bundled_dependencies(const void *lhs, const void *rhs) {
    const InitBundledDependency *left = (const InitBundledDependency *)lhs;
    const InitBundledDependency *right = (const InitBundledDependency *)rhs;
    int name_order = strcmp(left->name, right->name);

    if (name_order != 0) {
        return name_order;
    }
    return strcmp(left->version, right->version);
}

/* Discover all direct `.fb` files in the running Feng installation's `pkg/`. */
static bool collect_bundled_dependencies(const char *program,
                                         InitBundledDependencies *out_dependencies,
                                         char **out_error_message) {
    char *package_directory = NULL;
    char *resolve_error = NULL;
    DIR *directory = NULL;
    struct dirent *entry;
    bool ok = false;

    memset(out_dependencies, 0, sizeof(*out_dependencies));
    *out_error_message = NULL;

    package_directory = feng_cli_resolve_install_path(program, "pkg", &resolve_error);
    if (package_directory == NULL) {
        set_init_errorf(out_error_message,
                        "failed to locate bundled package directory: %s",
                        resolve_error != NULL ? resolve_error : "unknown error");
        goto cleanup;
    }

    directory = opendir(package_directory);
    if (directory == NULL) {
        if (errno == ENOENT) {
            ok = true;
            goto cleanup;
        }
        set_init_errorf(out_error_message,
                        "failed to open bundled package directory %s: %s",
                        package_directory,
                        strerror(errno));
        goto cleanup;
    }

    for (;;) {
        char *bundle_path;
        struct stat file_status;
        InitBundledDependency dependency = {0};

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                set_init_errorf(out_error_message,
                                "failed to read bundled package directory %s: %s",
                                package_directory,
                                strerror(errno));
                goto cleanup;
            }
            break;
        }
        if (!has_bundle_extension(entry->d_name)) {
            continue;
        }

        bundle_path = join_child_path(package_directory, entry->d_name);
        if (bundle_path == NULL) {
            set_init_errorf(out_error_message, "out of memory collecting bundled packages");
            goto cleanup;
        }
        if (stat(bundle_path, &file_status) != 0) {
            set_init_errorf(out_error_message,
                            "failed to inspect bundled package %s: %s",
                            bundle_path,
                            strerror(errno));
            free(bundle_path);
            goto cleanup;
        }
        if (!S_ISREG(file_status.st_mode)) {
            free(bundle_path);
            continue;
        }
        if (!read_bundled_dependency(bundle_path, &dependency, out_error_message)) {
            free(bundle_path);
            goto cleanup;
        }
        free(bundle_path);
        if (!append_bundled_dependency(out_dependencies,
                                       &dependency,
                                       out_error_message)) {
            free(dependency.bundle_path);
            free(dependency.name);
            free(dependency.version);
            goto cleanup;
        }
    }

    if (closedir(directory) != 0) {
        directory = NULL;
        set_init_errorf(out_error_message,
                        "failed to close bundled package directory %s: %s",
                        package_directory,
                        strerror(errno));
        goto cleanup;
    }
    directory = NULL;

    if (out_dependencies->count > 1U) {
        qsort(out_dependencies->items,
              out_dependencies->count,
              sizeof(*out_dependencies->items),
              compare_bundled_dependencies);
    }
    {
        size_t index;

        for (index = 1U; index < out_dependencies->count; ++index) {
            const InitBundledDependency *previous = &out_dependencies->items[index - 1U];
            const InitBundledDependency *current = &out_dependencies->items[index];

            if (strcmp(previous->name, current->name) == 0) {
                set_init_errorf(out_error_message,
                                "multiple bundled packages declare dependency `%s`: %s and %s",
                                current->name,
                                previous->bundle_path,
                                current->bundle_path);
                goto cleanup;
            }
        }
    }

    ok = true;

cleanup:
    if (directory != NULL) {
        closedir(directory);
    }
    if (!ok) {
        bundled_dependencies_dispose(out_dependencies);
    }
    free(resolve_error);
    free(package_directory);
    return ok;
}

/* Build the complete new-project manifest including optional bundled dependencies. */
static char *build_manifest_content(const char *package_name,
                                    bool target_lib,
                                    const InitBundledDependencies *dependencies) {
    char *content;
    size_t length;
    size_t capacity;
    size_t index;

    content = dup_printf("[package]\n"
                         "name: \"%s\"\n"
                         "version: \"0.1.0\"\n"
                         "target: \"%s\"\n"
                         "src: \"src/\"\n"
                         "out: \"build/\"\n"
                         "%s",
                         package_name,
                         target_lib ? "lib" : "bin",
                         target_lib
                             ? "platform: \"macos-arm64,linux-x64-gnu,linux-x64-musl,"
                               "linux-arm64-gnu,linux-arm64-musl\"\n"
                             : "");
    if (content == NULL) {
        return NULL;
    }
    if (dependencies->count == 0U) {
        return content;
    }

    length = strlen(content);
    capacity = length + 1U;
    if (!append_slice(&content,
                      &length,
                      &capacity,
                      "\n[dependencies]\n",
                      strlen("\n[dependencies]\n"))) {
        free(content);
        return NULL;
    }
    for (index = 0U; index < dependencies->count; ++index) {
        char *entry = dup_printf("%s: \"%s\"\n",
                                 dependencies->items[index].name,
                                 dependencies->items[index].version);

        if (entry == NULL ||
            !append_slice(&content,
                          &length,
                          &capacity,
                          entry,
                          entry != NULL ? strlen(entry) : 0U)) {
            free(entry);
            free(content);
            return NULL;
        }
        free(entry);
    }
    return content;
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
    FengCliParseResult parse_result;
    InitDirectoryState directory_state;
    char *directory_error = NULL;
    char *derived_name = NULL;
    const char *raw_package_name;
    char *package_name = NULL;
    InitBundledDependencies bundled_dependencies = {0};
    char *bundled_error = NULL;
    char *manifest_content = NULL;
    char *write_error = NULL;
    const char *source_path;
    char *source_template = NULL;
    bool created_src_dir = false;
    bool created_manifest = false;
    bool created_source = false;
    int rc = 1;

    parse_result = parse_args(program, argc, argv, &options);
    if (parse_result != FENG_CLI_PARSE_OK) {
        return parse_result == FENG_CLI_PARSE_HELP ? 0 : 1;
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

    raw_package_name = options.package_name;
    if (raw_package_name == NULL) {
        derived_name = derive_default_package_name();
        if (derived_name == NULL) {
            fprintf(stderr, "failed to derive package name from current directory\n");
            return 1;
        }
        raw_package_name = derived_name;
    }

    package_name = normalize_package_name(raw_package_name);
    if (package_name == NULL) {
        fprintf(stderr, "out of memory normalizing package name\n");
        goto cleanup;
    }

    if (!collect_bundled_dependencies(program,
                                      &bundled_dependencies,
                                      &bundled_error)) {
        fprintf(stderr,
                "%s\n",
                bundled_error != NULL
                    ? bundled_error
                    : "failed to collect bundled package dependencies");
        goto cleanup;
    }

    manifest_content = build_manifest_content(package_name,
                                              options.target_lib,
                                              &bundled_dependencies);
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
    free(bundled_error);
    bundled_dependencies_dispose(&bundled_dependencies);
    free(package_name);
    free(source_template);
    free(manifest_content);
    free(derived_name);
    return rc;
}
