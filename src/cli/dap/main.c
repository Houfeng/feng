#include "cli/cli.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli/common.h"
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

#if defined(__APPLE__)
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
    char *xcrun_program = feng_cli_find_executable_on_path("xcrun");
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
    if (!feng_cli_path_is_executable(output_buffer)) {
        return NULL;
    }
    return dup_cstr(output_buffer);
}
#endif

/* Resolve lldb-dap according to the documented host-tool precedence. */
static char *resolve_lldb_dap_backend(void *context, char **out_error_message) {
    const char *program = (const char *)context;
    const FengCliHostToolStrategy strategy = {
        "lldb-dap backend",
        "FENG_LLDB_DAP",
        "toolchain/llvm/bin/lldb-dap",
        NULL,
        "lldb-dap"
    };
    FengCliHostToolLookupStatus status;
    char *resolved = NULL;

    status = feng_cli_lookup_host_tool(program,
                                       &strategy,
                                       &resolved,
                                       out_error_message);
    if (status == FENG_CLI_HOST_TOOL_FOUND ||
        status == FENG_CLI_HOST_TOOL_ERROR) {
        return resolved;
    }
#if defined(__APPLE__)
    resolved = try_resolve_lldb_dap_via_xcrun();
    if (resolved != NULL) {
        return resolved;
    }
    if (out_error_message != NULL) {
        *out_error_message = dup_cstr(
            "cannot locate lldb-dap backend: FENG_LLDB_DAP is unset, "
            "the bundled path and PATH entry are absent, and xcrun could not resolve lldb-dap");
    }
#else
    if (out_error_message != NULL) {
        *out_error_message = dup_cstr(
            "cannot locate lldb-dap backend: FENG_LLDB_DAP is unset and "
            "the bundled path and PATH entry are absent");
    }
#endif
    return NULL;
}

/* Print command usage for `feng dap`. */
static void print_usage(const char *program, FILE *stream) {
    if (stream == stderr) fprintf(stream, "\n");
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

    return feng_dap_proxy_run(resolve_lldb_dap_backend,
                              (void *)program,
                              STDIN_FILENO,
                              STDOUT_FILENO,
                              STDERR_FILENO);
}
