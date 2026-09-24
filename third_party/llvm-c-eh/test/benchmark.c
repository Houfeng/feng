#include "runtime.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Separate translation unit keeps the benchmark call boundary without noinline. */
extern int ceh_benchmark_call(int value);

/* Read a monotonic clock without changing exception protocol or runtime state. */
static uint64_t now(void) {
    struct timespec value;
    ceh_test_check(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "monotonic clock");
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

/* Report observations, not an unstable timing threshold or a claim about all programs. */
int main(int argc, char **argv) {
    int exceptional = argc == 2 && !strcmp(argv[1], "throw");
    int iterations = exceptional ? 10000 : 5000000;
    int value = exceptional ? -1 : 1;
    uint64_t sum = 0, start = now();
    for (int i = 0; i < iterations; ++i) sum += (unsigned)ceh_benchmark_call(value);
    uint64_t elapsed = now() - start;
    ceh_test_check(sum == (uint64_t)iterations * (exceptional ? 2 : 3), "benchmark results");
    ceh_test_check(atomic_load(&ceh_test_alive) == 0, "benchmark lifetime");
    printf("%s ns/call=%.3f iterations=%d total_ns=%" PRIu64 "\n",
           exceptional ? "throw" : "normal", (double)elapsed / iterations, iterations, elapsed);
    return 0;
}
