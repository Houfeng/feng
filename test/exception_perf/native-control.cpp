#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

extern "C" int native_producer(int value);
extern "C" int native_call(int value);

#if defined(KERNEL)
/* The compiler constructs this EH graph directly, without loading the plugin. */
extern "C" int native_call(int value) {
#if PROTECTED
    try {
#endif
        return native_producer(value) + 1;
#if PROTECTED
    } catch (...) {
        return 2;
    }
#endif
}
#else
/* A separate object preserves the same actual call boundary without noinline. */
extern "C" int native_producer(int value) {
    if (value < 0) throw value;
    return value + 1;
}

/* Startup, argument decoding and verification stay outside the timed interval. */
static std::uint64_t now() {
    timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) std::abort();
    return static_cast<std::uint64_t>(value.tv_sec) * UINT64_C(1000000000) + value.tv_nsec;
}

/* Both controls link this exact object and validate every normally completed run. */
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const bool failure = std::atoi(argv[1]) != 0;
    const auto count = std::strtoull(argv[2], nullptr, 10);
    const auto begin = now();
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < count; ++i) sum += native_call(failure ? -1 : 1);
    const auto elapsed = now() - begin;
    if (sum != count * (failure ? 2 : 3)) return 1;
    std::printf("%llu\t%llu\n", static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(sum));
}
#endif
