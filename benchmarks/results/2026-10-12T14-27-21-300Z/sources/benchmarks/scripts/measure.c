#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t child_pid = 0;
static volatile sig_atomic_t interruption = 0;

/** Stop the measured process group on timeout or caller interruption. */
static void stop_child(int signal_number) {
  interruption = signal_number;
  if (child_pid > 0) {
    if (kill(-(pid_t)child_pid, SIGKILL) != 0) {
      (void)kill((pid_t)child_pid, SIGKILL);
    }
  }
}

/** Return a monotonic timestamp; a clock failure invalidates the measurement.
 */
static int64_t monotonic_ns(void) {
  struct timespec stamp;
  if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
    perror("clock_gettime");
    exit(125);
  }
  return (int64_t)stamp.tv_sec * INT64_C(1000000000) + stamp.tv_nsec;
}

/** Measure one unmodified executable, preserving argument boundaries. */
int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr,
            "usage: measure SECONDS METRICS STDOUT STDERR PROGRAM [ARG...]\n");
    return 125;
  }
  char *end = NULL;
  errno = 0;
  long timeout = strtol(argv[1], &end, 10);
  if (errno || *end || timeout < 1 || timeout > 86400)
    return 125;
  int output = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int errors = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int input = open("/dev/null", O_RDONLY);
  if (output < 0 || errors < 0 || input < 0) {
    perror("measurement streams");
    return 125;
  }
  struct sigaction action = {0};
  action.sa_handler = stop_child;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGALRM, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
      sigaction(SIGTERM, &action, NULL)) {
    perror("sigaction");
    return 125;
  }
  const int64_t begin = monotonic_ns();
  const pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 125;
  }
  if (pid == 0) {
    const struct rlimit no_core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    (void)setpgid(0, 0);
    (void)signal(SIGALRM, SIG_DFL);
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    if (dup2(input, STDIN_FILENO) < 0 || dup2(output, STDOUT_FILENO) < 0 ||
        dup2(errors, STDERR_FILENO) < 0)
      _exit(125);
    close(input);
    close(output);
    close(errors);
    execv(argv[5], &argv[5]);
    perror("execv");
    _exit(127);
  }
  child_pid = pid;
  (void)setpgid(pid, pid);
  if (interruption)
    stop_child(interruption);
  close(input);
  close(output);
  close(errors);
  alarm((unsigned)timeout);
  int status = 0;
  struct rusage usage = {0};
  pid_t waited;
  do {
    waited = wait4(pid, &status, 0, &usage);
  } while (waited < 0 && errno == EINTR);
  const int64_t elapsed = monotonic_ns() - begin;
  alarm(0);
  child_pid = 0;
  if (waited != pid) {
    perror("wait4");
    return 125;
  }
  uint64_t rss = (uint64_t)usage.ru_maxrss;
#if defined(__linux__)
  rss *= UINT64_C(1024);
#elif !defined(__APPLE__)
#error "Define wait4 peak RSS units before enabling this platform"
#endif
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  const int signal_number = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  FILE *metrics = fopen(argv[2], "w");
  if (!metrics) {
    perror("metrics");
    return 125;
  }
  const int written =
      fprintf(metrics,
              "{\"wall_ns\":%" PRId64 ",\"peak_rss_bytes\":%" PRIu64
              ",\"exit_code\":%d,\"signal\":%d,\"interruption\":%d}\n",
              elapsed, rss, exit_code, signal_number, (int)interruption);
  if (fclose(metrics) != 0 || written < 0)
    return 125;
  if (interruption)
    return interruption == SIGALRM ? 124 : 128 + interruption;
  return exit_code >= 0 ? exit_code : 128 + signal_number;
}
