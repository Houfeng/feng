#define _XOPEN_SOURCE 700

#include "cli/common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

/* Replace one owned error message with formatted text. */
static void set_errorf(char **out_error_message, const char *format, ...) {
    va_list args;
    va_list args_copy;
    int required;
    char *message;

    if (out_error_message == NULL) {
        return;
    }

    va_start(args, format);
    va_copy(args_copy, args);
    required = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (required < 0) {
        va_end(args_copy);
        return;
    }

    message = (char *)malloc((size_t)required + 1U);
    if (message == NULL) {
        va_end(args_copy);
        return;
    }
    vsnprintf(message, (size_t)required + 1U, format, args_copy);
    va_end(args_copy);
    *out_error_message = message;
}

/* Duplicate one NUL-terminated string into owned storage. */
static char *duplicate_string(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

/* Return an owned dirname while preserving the filesystem root. */
static char *path_dirname(const char *path) {
    const char *separator;
    size_t length;
    char *directory;

    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    separator = strrchr(path, '/');
    if (separator == NULL) {
        return duplicate_string(".");
    }
    length = separator == path ? 1U : (size_t)(separator - path);
    directory = (char *)malloc(length + 1U);
    if (directory == NULL) {
        return NULL;
    }
    memcpy(directory, path, length);
    directory[length] = '\0';
    return directory;
}

/* Join two path components with exactly one separator. */
static char *path_join(const char *left, const char *right) {
    size_t left_length;
    size_t right_offset;
    size_t right_length;
    bool needs_separator;
    char *path;

    if (left == NULL || right == NULL) {
        return NULL;
    }
    left_length = strlen(left);
    right_offset = right[0] == '/' ? 1U : 0U;
    right_length = strlen(right + right_offset);
    needs_separator = left_length > 0U && left[left_length - 1U] != '/';

    path = (char *)malloc(left_length + (needs_separator ? 1U : 0U) + right_length + 1U);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, left, left_length);
    if (needs_separator) {
        path[left_length++] = '/';
    }
    memcpy(path + left_length, right + right_offset, right_length);
    path[left_length + right_length] = '\0';
    return path;
}

/* Reject absolute paths and parent traversal outside the installation root. */
static bool is_safe_relative_path(const char *path) {
    const char *component;

    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return false;
    }

    component = path;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t length = end != NULL
            ? (size_t)(end - component)
            : strlen(component);

        if (length == 2U && component[0] == '.' && component[1] == '.') {
            return false;
        }
        if (end == NULL) {
            break;
        }
        component = end + 1;
    }
    return true;
}

/* Resolve /proc/self/exe without imposing a fixed PATH_MAX limit. */
#if defined(__linux__)
static char *resolve_linux_executable_path(void) {
    size_t capacity = 256U;

    while (capacity <= 1024U * 1024U) {
        char *buffer = (char *)malloc(capacity);
        ssize_t length;

        if (buffer == NULL) {
            return NULL;
        }
        length = readlink("/proc/self/exe", buffer, capacity - 1U);
        if (length < 0) {
            free(buffer);
            return NULL;
        }
        if ((size_t)length < capacity - 1U) {
            char *resolved;

            buffer[length] = '\0';
            resolved = realpath(buffer, NULL);
            free(buffer);
            return resolved;
        }
        free(buffer);
        capacity *= 2U;
    }
    return NULL;
}
#endif

/* Return whether one path is a regular executable file. */
static bool path_is_regular_executable(const char *path) {
    struct stat status;

    return path != NULL &&
           path[0] != '\0' &&
           stat(path, &status) == 0 &&
           S_ISREG(status.st_mode) &&
           access(path, X_OK) == 0;
}

char *feng_cli_find_executable_on_path(const char *program) {
    const char *path_value;
    const char *segment_start;

    if (program == NULL || program[0] == '\0') {
        return NULL;
    }
    if (strchr(program, '/') != NULL) {
        return path_is_regular_executable(program) ? duplicate_string(program) : NULL;
    }

    path_value = getenv("PATH");
    if (path_value == NULL || path_value[0] == '\0') {
        return NULL;
    }

    segment_start = path_value;
    while (true) {
        const char *segment_end = strchr(segment_start, ':');
        size_t segment_length = segment_end != NULL
            ? (size_t)(segment_end - segment_start)
            : strlen(segment_start);
        const char *directory = segment_length == 0U ? "." : segment_start;
        size_t directory_length = segment_length == 0U ? 1U : segment_length;
        size_t program_length = strlen(program);
        char *candidate = (char *)malloc(directory_length + 1U + program_length + 1U);

        if (candidate != NULL) {
            memcpy(candidate, directory, directory_length);
            candidate[directory_length] = '/';
            memcpy(candidate + directory_length + 1U, program, program_length + 1U);
            if (path_is_regular_executable(candidate)) {
                return candidate;
            }
            free(candidate);
        }

        if (segment_end == NULL) {
            break;
        }
        segment_start = segment_end + 1;
    }
    return NULL;
}

bool feng_cli_path_is_executable(const char *path) {
    return path_is_regular_executable(path);
}

char *feng_cli_resolve_executable_path(const char *program_path, char **out_error_message) {
    char *resolved = NULL;
    int saved_errno = 0;

    if (out_error_message != NULL) {
        *out_error_message = NULL;
    }

#if defined(__APPLE__)
    {
        uint32_t size = 0U;

        (void)_NSGetExecutablePath(NULL, &size);
        if (size > 0U) {
            char *raw_path = (char *)malloc(size);

            if (raw_path != NULL) {
                if (_NSGetExecutablePath(raw_path, &size) == 0) {
                    resolved = realpath(raw_path, NULL);
                    saved_errno = errno;
                }
                free(raw_path);
            }
        }
    }
#elif defined(__linux__)
    resolved = resolve_linux_executable_path();
    saved_errno = errno;
#endif

    if (resolved == NULL && program_path != NULL && program_path[0] != '\0') {
        char *candidate = NULL;

        if (strchr(program_path, '/') != NULL) {
            candidate = duplicate_string(program_path);
        } else {
            candidate = feng_cli_find_executable_on_path(program_path);
        }
        if (candidate != NULL) {
            resolved = realpath(candidate, NULL);
            saved_errno = errno;
            free(candidate);
        }
    }

    if (resolved == NULL) {
        set_errorf(out_error_message,
                   "cannot resolve Feng executable path from '%s': %s",
                   program_path != NULL ? program_path : "",
                   saved_errno != 0 ? strerror(saved_errno) : "path is unavailable");
    }
    return resolved;
}

char *feng_cli_resolve_install_path(const char *program_path,
                                    const char *relative_path,
                                    char **out_error_message) {
    char *executable_path;
    char *executable_directory;
    char *install_root;
    char *resolved_path;

    if (out_error_message != NULL) {
        *out_error_message = NULL;
    }
    if (!is_safe_relative_path(relative_path)) {
        set_errorf(out_error_message,
                   "installation path must be relative and stay below the Feng root: %s",
                   relative_path != NULL ? relative_path : "(null)");
        return NULL;
    }

    executable_path = feng_cli_resolve_executable_path(program_path, out_error_message);
    if (executable_path == NULL) {
        return NULL;
    }
    executable_directory = path_dirname(executable_path);
    free(executable_path);
    if (executable_directory == NULL) {
        set_errorf(out_error_message, "out of memory resolving Feng executable directory");
        return NULL;
    }
    install_root = path_dirname(executable_directory);
    free(executable_directory);
    if (install_root == NULL) {
        set_errorf(out_error_message, "out of memory resolving Feng installation root");
        return NULL;
    }
    resolved_path = path_join(install_root, relative_path);
    free(install_root);
    if (resolved_path == NULL) {
        set_errorf(out_error_message, "out of memory resolving Feng installation path");
    }
    return resolved_path;
}

char *feng_cli_require_install_path(const char *program_path,
                                    const char *relative_path,
                                    FengCliRequiredPathKind required_kind,
                                    char **out_error_message) {
    char *path = feng_cli_resolve_install_path(program_path,
                                               relative_path,
                                               out_error_message);
    struct stat status;
    bool valid = false;
    const char *description = "path";

    if (path == NULL) {
        return NULL;
    }
    if (stat(path, &status) == 0) {
        switch (required_kind) {
            case FENG_CLI_REQUIRED_REGULAR_FILE:
                description = "regular file";
                valid = S_ISREG(status.st_mode);
                break;
            case FENG_CLI_REQUIRED_DIRECTORY:
                description = "directory";
                valid = S_ISDIR(status.st_mode);
                break;
            case FENG_CLI_REQUIRED_EXECUTABLE:
                description = "executable";
                valid = S_ISREG(status.st_mode) && access(path, X_OK) == 0;
                break;
        }
    } else {
        switch (required_kind) {
            case FENG_CLI_REQUIRED_REGULAR_FILE:
                description = "regular file";
                break;
            case FENG_CLI_REQUIRED_DIRECTORY:
                description = "directory";
                break;
            case FENG_CLI_REQUIRED_EXECUTABLE:
                description = "executable";
                break;
        }
    }

    if (!valid) {
        set_errorf(out_error_message,
                   "required %s is missing or has the wrong type: %s",
                   description,
                   path);
        free(path);
        return NULL;
    }
    return path;
}

char *feng_cli_resolve_host_tool(const char *program_path,
                                 const FengCliHostToolStrategy *strategy,
                                 char **out_error_message) {
    const char *environment_value;
    char *candidate;
    struct stat bundled_status;
    int bundled_errno;

    if (out_error_message != NULL) {
        *out_error_message = NULL;
    }
    if (strategy == NULL ||
        strategy->display_name == NULL ||
        strategy->display_name[0] == '\0' ||
        strategy->bundled_relative_path == NULL ||
        strategy->bundled_relative_path[0] == '\0' ||
        strategy->system_executable == NULL ||
        strategy->system_executable[0] == '\0') {
        set_errorf(out_error_message, "invalid host tool lookup specification");
        return NULL;
    }

    environment_value = strategy->feng_environment_variable != NULL
        ? getenv(strategy->feng_environment_variable)
        : NULL;
    if (environment_value != NULL && environment_value[0] != '\0') {
        candidate = feng_cli_find_executable_on_path(environment_value);
        if (candidate != NULL) {
            return candidate;
        }
        set_errorf(out_error_message,
                   "%s environment variable %s specifies an unavailable executable: %s",
                   strategy->display_name,
                   strategy->feng_environment_variable,
                   environment_value);
        return NULL;
    }

    candidate = feng_cli_resolve_install_path(program_path,
                                              strategy->bundled_relative_path,
                                              out_error_message);
    if (candidate == NULL) {
        return NULL;
    }
    if (lstat(candidate, &bundled_status) == 0) {
        if (path_is_regular_executable(candidate)) {
            return candidate;
        }
        set_errorf(out_error_message,
                   "bundled %s is present but is not an executable regular file: %s",
                   strategy->display_name,
                   candidate);
        free(candidate);
        return NULL;
    }
    bundled_errno = errno;
    free(candidate);
    if (bundled_errno != ENOENT && bundled_errno != ENOTDIR) {
        set_errorf(out_error_message,
                   "cannot inspect bundled %s at %s: %s",
                   strategy->display_name,
                   strategy->bundled_relative_path,
                   strerror(bundled_errno));
        return NULL;
    }

    environment_value = strategy->conventional_environment_variable != NULL
        ? getenv(strategy->conventional_environment_variable)
        : NULL;
    if (environment_value != NULL && environment_value[0] != '\0') {
        candidate = feng_cli_find_executable_on_path(environment_value);
        if (candidate != NULL) {
            return candidate;
        }
        set_errorf(out_error_message,
                   "%s environment variable %s specifies an unavailable executable: %s",
                   strategy->display_name,
                   strategy->conventional_environment_variable,
                   environment_value);
        return NULL;
    }

    candidate = feng_cli_find_executable_on_path(strategy->system_executable);
    if (candidate != NULL) {
        return candidate;
    }
    set_errorf(out_error_message,
               "cannot locate %s: bundled path is absent, environment variables are unset, "
               "and '%s' was not found on PATH",
               strategy->display_name,
               strategy->system_executable);
    return NULL;
}

bool feng_cli_stream_supports_color(FILE *stream) {
    const char *force_color = getenv("CLICOLOR_FORCE");
    const char *no_color = getenv("NO_COLOR");

    if (no_color != NULL && no_color[0] != '\0') {
        return false;
    }
    if (force_color != NULL && force_color[0] != '\0' && strcmp(force_color, "0") != 0) {
        return true;
    }

    return isatty(fileno(stream)) != 0;
}

void feng_cli_set_stream_color(FILE *stream, bool enabled, const char *color) {
    if (enabled) {
        fputs(color, stream);
    }
}

void feng_cli_reset_stream_color(FILE *stream, bool enabled) {
    if (enabled) {
        fputs(FENG_COLOR_RESET, stream);
    }
}

char *feng_cli_read_entire_file(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;
    size_t read_size;

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0L) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1U, (size_t)size, file);
    fclose(file);

    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    *out_length = (size_t)size;
    return buffer;
}

void feng_cli_fprint_escaped_slice(FILE *stream, const char *text, size_t length) {
    size_t index;

    fputc('"', stream);
    for (index = 0; index < length; ++index) {
        unsigned char c = (unsigned char)text[index];

        switch (c) {
            case '\\':
                fputs("\\\\", stream);
                break;
            case '"':
                fputs("\\\"", stream);
                break;
            case '\n':
                fputs("\\n", stream);
                break;
            case '\r':
                fputs("\\r", stream);
                break;
            case '\t':
                fputs("\\t", stream);
                break;
            default:
                if (c >= 32U && c <= 126U) {
                    fputc((int)c, stream);
                } else {
                    fprintf(stream, "\\x%02X", c);
                }
                break;
        }
    }
    fputc('"', stream);
}

void feng_cli_print_escaped_slice(const char *text, size_t length) {
    feng_cli_fprint_escaped_slice(stdout, text, length);
}

void feng_cli_fprint_token_summary(FILE *stream, const FengToken *token) {
    if (token->kind == FENG_TOKEN_EOF) {
        fputs("EOF", stream);
        return;
    }

    fputs(feng_token_kind_name(token->kind), stream);
    if (token->kind == FENG_TOKEN_ANNOTATION) {
        fputc(' ', stream);
        fputs(feng_annotation_kind_name(token->annotation_kind), stream);
    }
    if (token->length > 0U) {
        fputc(' ', stream);
        feng_cli_fprint_escaped_slice(stream, token->lexeme, token->length);
    }
}

static unsigned int count_digits(unsigned int value) {
    unsigned int digits = 1U;

    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }

    return digits;
}

static void print_error_context(FILE *stream,
                                const char *source,
                                size_t source_length,
                                const FengToken *token) {
    size_t index;
    size_t line_start = 0U;
    size_t line_end = source_length;
    unsigned int current_line = 1U;
    unsigned int line_no = token->line > 0U ? token->line : 1U;
    unsigned int digits = count_digits(line_no);
    bool use_color = feng_cli_stream_supports_color(stream);

    for (index = 0U; index < source_length; ++index) {
        if (current_line == line_no) {
            line_start = index;
            break;
        }
        if (source[index] == '\n') {
            ++current_line;
            line_start = index + 1U;
        }
    }

    line_end = line_start;
    while (line_end < source_length && source[line_end] != '\n') {
        ++line_end;
    }
    if (line_end > line_start && source[line_end - 1U] == '\r') {
        --line_end;
    }

    fprintf(stream, "  got: ");
    feng_cli_fprint_token_summary(stream, token);
    fputc('\n', stream);

    feng_cli_set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*u | ", digits, line_no);
    fwrite(source + line_start, 1U, line_end - line_start, stream);
    feng_cli_reset_stream_color(stream, use_color);
    fputc('\n', stream);

    feng_cli_set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "  %*s | ", digits, "");
    if (token->kind == FENG_TOKEN_EOF) {
        for (index = line_start; index < line_end; ++index) {
            fputc(source[index] == '\t' ? '\t' : ' ', stream);
        }
    } else if (token->offset >= line_start && token->offset <= line_end) {
        for (index = line_start; index < token->offset; ++index) {
            fputc(source[index] == '\t' ? '\t' : ' ', stream);
        }
    } else {
        unsigned int column = token->column > 0U ? token->column - 1U : 0U;

        for (index = 0U; index < (size_t)column; ++index) {
            fputc(' ', stream);
        }
    }
    fputc('^', stream);
    feng_cli_reset_stream_color(stream, use_color);
    fputc('\n', stream);
}

void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *code,
                               const char *message,
                               const FengToken *token,
                               const char *source,
                               size_t source_length) {
    bool use_color = feng_cli_stream_supports_color(stream);

    fprintf(stream, "%s:", path);
    feng_cli_set_stream_color(stream, use_color, FENG_COLOR_RED);
    fprintf(stream, "%u:%u", token->line, token->column);
    feng_cli_reset_stream_color(stream, use_color);
    fprintf(stream, "\n");
    fprintf(stream, "%s: %s\n", code != NULL ? code : "IE0001", message != NULL ? message : "unknown error");

    if (source != NULL) {
        print_error_context(stream, source, source_length, token);
    }
}

bool feng_cli_parse_target_option(const char *arg, FengCompileTarget *out_target) {
    const char *value = NULL;

    if (arg == NULL || out_target == NULL) {
        return false;
    }
    if (strncmp(arg, "--target=", 9) == 0) {
        value = arg + 9;
    } else if (strcmp(arg, "--target") == 0) {
        return false;
    } else {
        return false;
    }
    if (strcmp(value, "bin") == 0) {
        *out_target = FENG_COMPILE_TARGET_BIN;
        return true;
    }
    if (strcmp(value, "lib") == 0) {
        *out_target = FENG_COMPILE_TARGET_LIB;
        return true;
    }
    fprintf(stderr, "invalid --target value '%s' (expected 'bin' or 'lib')\n", value);
    return false;
}

const FengCliLoadedSource *feng_cli_find_loaded_source(const FengCliLoadedSource *sources,
                                                       size_t source_count,
                                                       const char *path) {
    size_t index;

    if (path == NULL) {
        return NULL;
    }
    for (index = 0U; index < source_count; ++index) {
        if (sources[index].path != NULL && strcmp(sources[index].path, path) == 0) {
            return &sources[index];
        }
    }

    return NULL;
}

void feng_cli_free_loaded_sources(FengCliLoadedSource *sources, size_t source_count) {
    size_t index;

    if (sources == NULL) {
        return;
    }

    for (index = 0U; index < source_count; ++index) {
        feng_program_free(sources[index].program);
        free(sources[index].source);
    }
    free(sources);
}
