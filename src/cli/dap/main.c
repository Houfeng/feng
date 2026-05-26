#include "cli/cli.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dap/proxy.h"

/* Duplicate a NUL-terminated string into owned heap storage. */
static char *dup_cstr(const char *text) {
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

/* Return whether one filesystem path exists and is executable. */
static bool is_executable_path(const char *file_path) {
    return file_path != NULL && file_path[0] != '\0' && access(file_path, X_OK) == 0;
}

/* Build one `<dir>/<program>` candidate path from a PATH segment. */
static char *build_path_candidate(const char *directory, size_t directory_length, const char *program) {
    size_t program_length;
    size_t total_length;
    char *candidate;

    if (program == NULL) {
        return NULL;
    }
    program_length = strlen(program);
    total_length = directory_length + 1U + program_length;
    candidate = (char *)malloc(total_length + 1U);
    if (candidate == NULL) {
        return NULL;
    }
    if (directory_length == 0U) {
        candidate[0] = '.';
        directory_length = 1U;
    } else {
        memcpy(candidate, directory, directory_length);
    }
    candidate[directory_length] = '/';
    memcpy(candidate + directory_length + 1U, program, program_length);
    candidate[total_length] = '\0';
    return candidate;
}

/* Search the current PATH for one executable program and return its full path. */
static char *find_program_on_path(const char *program) {
    const char *path_value;
    const char *segment_start;

    if (program == NULL || program[0] == '\0') {
        return NULL;
    }
    if (strchr(program, '/') != NULL) {
        return is_executable_path(program) ? dup_cstr(program) : NULL;
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
        char *candidate = build_path_candidate(segment_start, segment_length, program);

        if (candidate != NULL && is_executable_path(candidate)) {
            return candidate;
        }
        free(candidate);

        if (segment_end == NULL) {
            break;
        }
        segment_start = segment_end + 1;
    }

    return NULL;
}

/* Remove trailing newlines and spaces from one command output buffer in place. */
static void trim_trailing_ascii_whitespace(char *text) {
    size_t length;

    if (text == NULL) {
        return;
    }

    length = strlen(text);
    while (length > 0U) {
        char current = text[length - 1U];

        if (current != ' ' && current != '\t' && current != '\r' && current != '\n') {
            break;
        }
        length -= 1U;
    }
    text[length] = '\0';
}

/* Ask `xcrun -f lldb-dap` for the native backend path when PATH misses it. */
static char *try_resolve_lldb_dap_via_xcrun(void) {
    char *xcrun_program = find_program_on_path("xcrun");
    int output_pipe[2] = {-1, -1};
    pid_t child;
    char output_buffer[4096];
    size_t output_length = 0U;
    ssize_t read_size;
    int status = 0;

    if (xcrun_program == NULL) {
        return NULL;
    }
    if (pipe(output_pipe) != 0) {
        free(xcrun_program);
        return NULL;
    }

    child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        free(xcrun_program);
        return NULL;
    }
    if (child == 0) {
        char *const argv[] = {xcrun_program, "-f", "lldb-dap", NULL};
        int devnull_fd;

        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(output_pipe[1]);
        devnull_fd = open("/dev/null", O_WRONLY);
        if (devnull_fd >= 0) {
            if (dup2(devnull_fd, STDERR_FILENO) < 0) {
                close(devnull_fd);
                _exit(127);
            }
            close(devnull_fd);
        }
        execv(xcrun_program, argv);
        _exit(127);
    }

    close(output_pipe[1]);
    while (output_length + 1U < sizeof(output_buffer)) {
        read_size = read(output_pipe[0], output_buffer + output_length,
                         sizeof(output_buffer) - output_length - 1U);
        if (read_size <= 0) {
            break;
        }
        output_length += (size_t)read_size;
    }
    close(output_pipe[0]);
    free(xcrun_program);

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return NULL;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return NULL;
    }

    output_buffer[output_length] = '\0';
    trim_trailing_ascii_whitespace(output_buffer);
    if (!is_executable_path(output_buffer)) {
        return NULL;
    }
    return dup_cstr(output_buffer);
}

/* Resolve the `lldb-dap` backend using PATH first, then `xcrun` on macOS. */
static char *resolve_lldb_dap_backend_program(void) {
    char *resolved = find_program_on_path("lldb-dap");

    if (resolved != NULL) {
        return resolved;
    }
    resolved = try_resolve_lldb_dap_via_xcrun();
    if (resolved != NULL) {
        return resolved;
    }
    return dup_cstr("lldb-dap");
}

/* Print command usage for `feng dap`. */
static void print_usage(const char *program, FILE *stream) {
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s dap [--stdio]\n", program);
    fprintf(stream, "\n");
    fprintf(stream, "Start Feng Debug Adapter Protocol proxy on stdio.\n");
}

/* Parse `feng dap` arguments and start the transparent stdio proxy. */
int feng_cli_dap_main(const char *program, int argc, char **argv) {
    int index;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program, stdout);
            return 0;
        }
        if (strcmp(arg, "--stdio") == 0) {
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", arg);
        print_usage(program, stderr);
        return 1;
    }

#if !defined(__APPLE__)
    fprintf(stderr, "error: `feng dap` currently only supports macOS with lldb-dap\n");
    return 1;
#else
    char *backend_program = resolve_lldb_dap_backend_program();
    int exit_code;

    if (backend_program == NULL) {
        fprintf(stderr, "error: failed to resolve lldb-dap backend path\n");
        return 1;
    }

    exit_code = feng_dap_proxy_run(backend_program, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
    free(backend_program);
    return exit_code;
#endif
}
