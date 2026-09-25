#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Parse one bounded dynamic input; malformed runs fail rather than skew timing. */
static uint64_t input(const char *name, uint64_t fallback, uint64_t max) {
    const char *text = getenv(name);
    if (text == NULL || *text == '\0') return fallback;
    char *end;
    errno = 0;
    uint64_t value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > max) exit(2);
    return value;
}

/* Bounded counts keep the independent checksum within uint64_t range. */
uint64_t arc_bench_count(void) { return input("ARC_COUNT", 1000000U, 100000000U); }
/* Input retrieval does not belong to the timed loop. */
uint64_t arc_bench_seed(void) { return input("ARC_SEED", 42U, UINT32_MAX); }
/* Every mode performs the same number of business object allocations. */
uint64_t arc_bench_mode(void) { return input("ARC_MODE", 0U, 10U); }

/* Monotonic nanoseconds permit paired comparisons without wall-clock changes. */
uint64_t arc_bench_clock(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) exit(2);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

/* Independent oracle and lifetime assertions are checked on every sample. */
void arc_bench_finish(uint64_t begin, uint64_t end, uint64_t actual,
                      uint64_t live, uint64_t peak, uint64_t drops, uint64_t defaults) {
    uint64_t count = arc_bench_count(), value = arc_bench_seed(), expected = 0U;
    for (uint64_t i = 0U; i < count; ++i) {
        value = (value * UINT64_C(1664525) + UINT64_C(1013904223)) & UINT32_MAX;
        expected += value;
    }
    uint64_t expected_defaults = arc_bench_mode() == 5U ? count : 0U;
    if (actual != expected || live != 0U || drops != count || peak != (count != 0U) ||
        defaults != expected_defaults) {
        fprintf(stderr, "ARC verification failed: sum=%" PRIu64 "/%" PRIu64
            " live=%" PRIu64 " peak=%" PRIu64 " drops=%" PRIu64 "/%" PRIu64
            " defaults=%" PRIu64 "/%" PRIu64 "\n",
            actual, expected, live, peak, drops, count, defaults, expected_defaults);
        exit(1);
    }
    printf("%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
           "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n",
           arc_bench_mode(), count, end - begin, actual, peak, drops, defaults);
}
