/* Shared measurement and independent checksum oracle for the Feng/C++ controls. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Read bounded run-time inputs without involving either language's timed kernel. */
static uint64_t input(const char *name, uint64_t fallback, uint64_t maximum) {
    const char *text = getenv(name);
    if (text == NULL) return fallback;
    char *end;
    uint64_t value = strtoull(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value > maximum) abort();
    return value;
}

/* Export scalar-only inputs through the C ABI. */
uint64_t eh_bench_count(void) { return input("EH_COUNT", 20000000, 1000000000); }
uint64_t eh_bench_seed(void) { return input("EH_SEED", 7, UINT32_MAX); }
uint64_t eh_bench_mode(void) { return input("EH_MODE", 0, 8); }

/* Use the same monotonic clock on both sides; startup and reporting are excluded. */
uint64_t eh_bench_clock(void) {
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) abort();
    return (uint64_t)stamp.tv_sec * UINT64_C(1000000000) + (uint64_t)stamp.tv_nsec;
}

/* Check every result outside the measured interval, including actual throw inputs. */
void eh_bench_finish(uint64_t begin, uint64_t end, uint64_t actual) {
    uint64_t value = eh_bench_seed(), count = eh_bench_count(), mode = eh_bench_mode();
    uint64_t expected = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (mode < 6 || (mode == 8 && i % 1000 != 0))
            value = (value * UINT64_C(1664525) + UINT64_C(1013904223)) & UINT32_MAX;
        expected += value;
    }
    if (actual != expected || end < begin) {
        fprintf(stderr, "exception benchmark checksum: got %" PRIu64 ", expected %" PRIu64 "\n",
                actual, expected);
        abort();
    }
    printf("%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n",
           mode, count, end - begin, actual);
}
