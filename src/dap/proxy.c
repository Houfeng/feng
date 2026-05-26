#include "dap/proxy.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define FENG_DAP_PROXY_BUFFER_CAPACITY 4096U

/* Track one half-duplex relay leg between two file descriptors. */
typedef struct FengDapProxyPipe {
    int read_fd;
    int write_fd;
    bool close_write_on_finish;
    bool read_closed;
    bool write_closed;
    size_t buffer_start;
    size_t buffer_end;
    unsigned char buffer[FENG_DAP_PROXY_BUFFER_CAPACITY];
} FengDapProxyPipe;

/* Emit a proxy error message to the selected stderr fd. */
static void proxy_report_error(int error_fd,
                               const char *context,
                               const char *detail) {
    if (error_fd < 0 || context == NULL || detail == NULL) {
        return;
    }
    dprintf(error_fd, "%s: %s\n", context, detail);
}


/* Return whether the relay leg still has bytes buffered for output. */
static bool proxy_pipe_has_pending_output(const FengDapProxyPipe *pipe_state) {
    return pipe_state != NULL && pipe_state->buffer_start < pipe_state->buffer_end;
}

/* Return whether the relay leg has fully drained and no longer needs polling. */
static bool proxy_pipe_is_done(const FengDapProxyPipe *pipe_state) {
    if (pipe_state == NULL) {
        return true;
    }
    if (!pipe_state->read_closed) {
        return false;
    }
    if (proxy_pipe_has_pending_output(pipe_state)) {
        return false;
    }
    return !pipe_state->close_write_on_finish || pipe_state->write_closed;
}

/* Close the downstream writer once upstream EOF has been fully drained. */
static void proxy_pipe_finish_write(FengDapProxyPipe *pipe_state) {
    if (pipe_state == NULL || pipe_state->write_closed || !pipe_state->close_write_on_finish) {
        return;
    }
    if (!pipe_state->read_closed || proxy_pipe_has_pending_output(pipe_state)) {
        return;
    }
    close(pipe_state->write_fd);
    pipe_state->write_closed = true;
}

/* Read the next chunk from the upstream side into the relay buffer. */
static bool proxy_pipe_read_into_buffer(FengDapProxyPipe *pipe_state, int error_fd) {
    ssize_t read_size;

    if (pipe_state == NULL || pipe_state->read_closed || pipe_state->buffer_end >= sizeof(pipe_state->buffer)) {
        return true;
    }

    read_size = read(pipe_state->read_fd,
                     pipe_state->buffer + pipe_state->buffer_end,
                     sizeof(pipe_state->buffer) - pipe_state->buffer_end);
    if (read_size == 0) {
        pipe_state->read_closed = true;
        proxy_pipe_finish_write(pipe_state);
        return true;
    }
    if (read_size < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return true;
        }
        proxy_report_error(error_fd, "failed to read dap proxy stream", strerror(errno));
        return false;
    }

    pipe_state->buffer_end += (size_t)read_size;
    return true;
}

/* Flush buffered bytes to the downstream side. */
static bool proxy_pipe_write_from_buffer(FengDapProxyPipe *pipe_state, int error_fd) {
    ssize_t written;

    if (pipe_state == NULL || pipe_state->write_closed || !proxy_pipe_has_pending_output(pipe_state)) {
        return true;
    }

    written = write(pipe_state->write_fd,
                    pipe_state->buffer + pipe_state->buffer_start,
                    pipe_state->buffer_end - pipe_state->buffer_start);
    if (written < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return true;
        }
        if (errno == EPIPE && pipe_state->close_write_on_finish) {
            pipe_state->write_closed = true;
            pipe_state->read_closed = true;
            pipe_state->buffer_start = 0U;
            pipe_state->buffer_end = 0U;
            return true;
        }
        proxy_report_error(error_fd, "failed to write dap proxy stream", strerror(errno));
        return false;
    }

    pipe_state->buffer_start += (size_t)written;
    if (pipe_state->buffer_start == pipe_state->buffer_end) {
        pipe_state->buffer_start = 0U;
        pipe_state->buffer_end = 0U;
        proxy_pipe_finish_write(pipe_state);
    }
    return true;
}

/* Relay bytes between the editor stdio and the native backend pipes. */
static bool proxy_relay_streams(int input_fd,
                                int output_fd,
                                int child_stdin_fd,
                                int child_stdout_fd,
                                int error_fd) {
    FengDapProxyPipe inbound = {
        .read_fd = input_fd,
        .write_fd = child_stdin_fd,
        .close_write_on_finish = true,
        .read_closed = false,
        .write_closed = false,
        .buffer_start = 0U,
        .buffer_end = 0U,
    };
    FengDapProxyPipe outbound = {
        .read_fd = child_stdout_fd,
        .write_fd = output_fd,
        .close_write_on_finish = false,
        .read_closed = false,
        .write_closed = false,
        .buffer_start = 0U,
        .buffer_end = 0U,
    };

    while (!proxy_pipe_is_done(&inbound) || !proxy_pipe_is_done(&outbound)) {
        struct pollfd poll_fds[4];
        int inbound_read_slot = -1;
        int inbound_write_slot = -1;
        int outbound_read_slot = -1;
        int outbound_write_slot = -1;
        nfds_t poll_count = 0U;
        int poll_rc;

        if (!inbound.read_closed && inbound.buffer_end < sizeof(inbound.buffer)) {
            inbound_read_slot = (int)poll_count;
            poll_fds[poll_count].fd = inbound.read_fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }
        if (!inbound.write_closed && proxy_pipe_has_pending_output(&inbound)) {
            inbound_write_slot = (int)poll_count;
            poll_fds[poll_count].fd = inbound.write_fd;
            poll_fds[poll_count].events = POLLOUT | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }
        if (!outbound.read_closed && outbound.buffer_end < sizeof(outbound.buffer)) {
            outbound_read_slot = (int)poll_count;
            poll_fds[poll_count].fd = outbound.read_fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }
        if (proxy_pipe_has_pending_output(&outbound)) {
            outbound_write_slot = (int)poll_count;
            poll_fds[poll_count].fd = outbound.write_fd;
            poll_fds[poll_count].events = POLLOUT | POLLHUP;
            poll_fds[poll_count].revents = 0;
            poll_count += 1U;
        }

        if (poll_count == 0U) {
            break;
        }

        poll_rc = poll(poll_fds, poll_count, -1);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            proxy_report_error(error_fd, "failed to poll dap proxy streams", strerror(errno));
            return false;
        }

        if (inbound_read_slot >= 0 && (poll_fds[inbound_read_slot].revents & (POLLIN | POLLHUP)) != 0) {
            if (!proxy_pipe_read_into_buffer(&inbound, error_fd)) {
                return false;
            }
        }
        if (outbound_read_slot >= 0 && (poll_fds[outbound_read_slot].revents & (POLLIN | POLLHUP)) != 0) {
            if (!proxy_pipe_read_into_buffer(&outbound, error_fd)) {
                return false;
            }
        }
        if (inbound_write_slot >= 0 && (poll_fds[inbound_write_slot].revents & (POLLOUT | POLLHUP)) != 0) {
            if (!proxy_pipe_write_from_buffer(&inbound, error_fd)) {
                return false;
            }
        }
        if (outbound_write_slot >= 0 && (poll_fds[outbound_write_slot].revents & (POLLOUT | POLLHUP)) != 0) {
            if (!proxy_pipe_write_from_buffer(&outbound, error_fd)) {
                return false;
            }
        }
    }

    return true;
}

/* Wait for the native backend process and normalize its exit status. */
static int proxy_wait_for_child(pid_t child, int error_fd, const char *backend_program) {
    int status = 0;

    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        proxy_report_error(error_fd, "failed to wait for dap backend", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        dprintf(error_fd,
                "%s terminated by signal %d\n",
                backend_program != NULL ? backend_program : "lldb-dap",
                WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

/* Launch the native backend as a child process with stdio connected to pipes. */
static pid_t proxy_spawn_backend(const char *backend_program,
                                 int child_stdin[2],
                                 int child_stdout[2],
                                 int error_fd) {
    pid_t child;
    char *const argv[] = {(char *)(backend_program != NULL ? backend_program : "lldb-dap"), NULL};

    child = fork();
    if (child < 0) {
        proxy_report_error(error_fd, "failed to fork dap backend", strerror(errno));
        return -1;
    }
    if (child == 0) {
        close(child_stdin[1]);
        close(child_stdout[0]);
        if (dup2(child_stdin[0], STDIN_FILENO) < 0 ||
            dup2(child_stdout[1], STDOUT_FILENO) < 0 ||
            (error_fd >= 0 && error_fd != STDERR_FILENO && dup2(error_fd, STDERR_FILENO) < 0)) {
            dprintf(error_fd >= 0 ? error_fd : STDERR_FILENO,
                    "failed to wire dap backend stdio: %s\n",
                    strerror(errno));
            _exit(127);
        }
        close(child_stdin[0]);
        close(child_stdout[1]);
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "failed to exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    return child;
}

/* Run the transparent DAP proxy against the selected backend program. */
int feng_dap_proxy_run(const char *backend_program,
                       int input_fd,
                       int output_fd,
                       int error_fd) {
    int child_stdin[2] = {-1, -1};
    int child_stdout[2] = {-1, -1};
    struct sigaction old_sigpipe;
    struct sigaction ignore_sigpipe;
    pid_t child;
    int exit_code;

    if (backend_program == NULL || input_fd < 0 || output_fd < 0) {
        proxy_report_error(error_fd, "invalid dap proxy configuration", "backend program and stdio fds are required");
        return 1;
    }
    if (pipe(child_stdin) != 0 || pipe(child_stdout) != 0) {
        proxy_report_error(error_fd, "failed to create dap proxy pipes", strerror(errno));
        if (child_stdin[0] >= 0) {
            close(child_stdin[0]);
            close(child_stdin[1]);
        }
        if (child_stdout[0] >= 0) {
            close(child_stdout[0]);
            close(child_stdout[1]);
        }
        return 1;
    }

    ignore_sigpipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sigpipe.sa_mask);
    ignore_sigpipe.sa_flags = 0;
    sigaction(SIGPIPE, &ignore_sigpipe, &old_sigpipe);

    child = proxy_spawn_backend(backend_program, child_stdin, child_stdout, error_fd);
    if (child < 0) {
        close(child_stdin[0]);
        close(child_stdin[1]);
        close(child_stdout[0]);
        close(child_stdout[1]);
        sigaction(SIGPIPE, &old_sigpipe, NULL);
        return 1;
    }

    close(child_stdin[0]);
    close(child_stdout[1]);
    if (!proxy_relay_streams(input_fd, output_fd, child_stdin[1], child_stdout[0], error_fd)) {
        close(child_stdin[1]);
        close(child_stdout[0]);
        exit_code = proxy_wait_for_child(child, error_fd, backend_program);
        sigaction(SIGPIPE, &old_sigpipe, NULL);
        return exit_code != 0 ? exit_code : 1;
    }
    close(child_stdin[1]);
    close(child_stdout[0]);

    exit_code = proxy_wait_for_child(child, error_fd, backend_program);
    sigaction(SIGPIPE, &old_sigpipe, NULL);
    return exit_code;
}

/* End of transparent DAP proxy implementation. */
