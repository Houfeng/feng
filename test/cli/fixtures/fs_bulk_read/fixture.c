/* Deterministic POSIX input faults, confined to the inode created by this test.
 * The production reader and runtime are linked without test-only changes. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int active, observed_fd = -1, hint_mode;
static dev_t device;
static ino_t inode;
static intptr_t short_limit, fail_after, delivered, content_size;
static pid_t writer;
static int (*real_stat)(int, struct stat *);
static ssize_t (*real_read)(int, void *, size_t);
static off_t (*real_seek)(int, off_t, int);
static pthread_once_t libc_once = PTHREAD_ONCE_INIT;

/* Resolve libc without type-punning function pointers or recursing through hooks. */
static void initialize_libc(void) {
    void *symbol;
    symbol = dlsym(RTLD_NEXT, "fstat"); assert(symbol != NULL);
    memcpy(&real_stat, &symbol, sizeof(real_stat));
    symbol = dlsym(RTLD_NEXT, "read"); assert(symbol != NULL);
    memcpy(&real_read, &symbol, sizeof(real_read));
    symbol = dlsym(RTLD_NEXT, "lseek"); assert(symbol != NULL);
    memcpy(&real_seek, &symbol, sizeof(real_seek));
}

/* Library code may also use libc while fault injection is inactive. */
static void resolve_libc(void) {
    assert(pthread_once(&libc_once, initialize_libc) == 0);
}

/* Produce binary data, including embedded zero bytes, in bounded writes. */
static void write_pattern(int fd, intptr_t length) {
    unsigned char bytes[4096];
    intptr_t offset = 0;
    while (offset < length) {
        size_t count = (size_t)(length - offset);
        if (count > sizeof(bytes)) count = sizeof(bytes);
        for (size_t i = 0; i < count; ++i)
            bytes[i] = (unsigned char)(((uintptr_t)offset + i) * 37U + 11U);
        size_t used = 0;
        while (used < count) {
            ssize_t n = write(fd, bytes + used, count - used);
            assert(n > 0); used += (size_t)n;
        }
        offset += (intptr_t)count;
    }
}

/* Modes: ordinary, stat failure, seek failure, zero hint, growth, truncation,
 * real FIFO, a negative size hint, or interrupted reads. Growth/truncation happen after fstat's
 * snapshot and before the first read, with no scheduling-dependent sleeps. */
void fs_fixture_prepare(intptr_t size, intptr_t mode, intptr_t limit, intptr_t fail) {
    struct stat info;
    resolve_libc();
    active = 0; observed_fd = -1; writer = 0;
    hint_mode = (int)mode; short_limit = limit; fail_after = fail;
    delivered = 0; content_size = size;
    (void)unlink("input.bin");
    if (mode == 6) {
        assert(mkfifo("input.bin", 0600) == 0);
        assert(stat("input.bin", &info) == 0);
        writer = fork(); assert(writer >= 0);
        if (writer == 0) {
            int fd = open("input.bin", O_WRONLY); assert(fd >= 0);
            write_pattern(fd, size); assert(close(fd) == 0); _exit(0);
        }
    } else {
        int fd = open("input.bin", O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0);
        write_pattern(fd, mode == 4 ? 1 : mode == 5 ? size * 2 + 1 : size);
        assert(real_stat(fd, &info) == 0); assert(close(fd) == 0);
    }
    device = info.st_dev; inode = info.st_ino; active = 1;
}

/* Observe the actual reader descriptor, then change only this fixture's hint. */
int fstat(int fd, struct stat *info) {
    resolve_libc();
    int result = real_stat(fd, info);
    if (result != 0 || !active || info->st_dev != device || info->st_ino != inode)
        return result;
    observed_fd = fd;
    if (hint_mode == 1) { errno = EIO; return -1; }
    if (hint_mode == 3) info->st_size = 0;
    if (hint_mode == 7) info->st_size = -1;
    if (hint_mode == 4 || hint_mode == 5) {
        int output = open("input.bin", O_WRONLY | O_TRUNC); assert(output >= 0);
        write_pattern(output, content_size); assert(close(output) == 0);
        hint_mode = 0;
    }
    return result;
}

/* Failed offset queries must fall back without moving the file position. */
off_t lseek(int fd, off_t offset, int whence) {
    resolve_libc();
    if (active && fd == observed_fd && hint_mode == 2) { errno = ESPIPE; return -1; }
    return real_seek(fd, offset, whence);
}

/* Cap positive reads and inject errors only after the selected byte count. */
ssize_t read(int fd, void *buffer, size_t count) {
    resolve_libc();
    if (active && fd == observed_fd) {
        assert(count <= 1048576U);
        if (fail_after >= 0 && delivered >= fail_after) {
            errno = hint_mode == 8 ? EINTR : EIO; return -1;
        }
        if (short_limit > 0 && count > (size_t)short_limit) count = (size_t)short_limit;
        if (fail_after >= 0 && count > (size_t)(fail_after - delivered))
            count = (size_t)(fail_after - delivered);
        ssize_t result = real_read(fd, buffer, count);
        if (result > 0) delivered += result;
        return result;
    }
    return real_read(fd, buffer, count);
}

/* Validate every returned byte independently of Feng's scanning implementation. */
intptr_t fs_fixture_verify(const unsigned char *bytes, intptr_t length, intptr_t offset) {
    if (length != content_size - offset) return 0;
    for (intptr_t i = 0; i < length; ++i)
        if (bytes[i] != (unsigned char)((uintptr_t)(i + offset) * 37U + 11U)) return 0;
    return 1;
}

/* Check descriptor lifetime before disabling hooks and removing the fixture. */
intptr_t fs_fixture_is_open(void) {
    return observed_fd >= 0 && fcntl(observed_fd, F_GETFD) >= 0;
}

/* Reap the FIFO producer; it must have delivered all bytes and exited normally. */
void fs_fixture_finish(void) {
    active = 0;
    if (writer > 0) {
        int status = 0; assert(waitpid(writer, &status, 0) == writer);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    assert(unlink("input.bin") == 0);
}
