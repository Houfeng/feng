#include <cstdint>

#if defined(EH_SINGLE_TU)
#include "callee.cpp"
#else
std::uint64_t plain(std::uint64_t);
std::uint64_t step(std::uint64_t, bool);
std::uint64_t relay(std::uint64_t, bool, std::uint64_t);
#endif
extern "C" {
std::uint64_t eh_bench_count(void);
std::uint64_t eh_bench_seed(void);
std::uint64_t eh_bench_mode(void);
std::uint64_t eh_bench_clock(void);
void eh_bench_finish(std::uint64_t, std::uint64_t, std::uint64_t);
}

/* Keep the algorithm, catch filters and timed interval identical to kernel.ff. */
int main() {
    const auto count = eh_bench_count(), mode = eh_bench_mode();
    auto value = eh_bench_seed();
    std::uint64_t sum = 0;
    const auto begin = eh_bench_clock();
    if (mode == 0) {
        for (std::uint64_t i = 0; i < count; ++i) {
            value = (value * UINT64_C(1664525) + UINT64_C(1013904223)) & UINT32_MAX;
            sum += value;
        }
    } else if (mode == 1) {
        for (std::uint64_t i = 0; i < count; ++i) { value = plain(value); sum += value; }
    } else if (mode == 2) {
        for (std::uint64_t i = 0; i < count; ++i) { value = step(value, false); sum += value; }
    } else if (mode == 3) {
        for (std::uint64_t i = 0; i < count; ++i) {
            try { value = step(value, false); } catch (std::uint64_t error) { value = error; }
            sum += value;
        }
    } else if (mode == 4) {
        for (std::uint64_t i = 0; i < count; ++i) {
            try { value = step(value, false); } catch (...) { value = 0; }
            sum += value;
        }
    } else if (mode == 5) {
        for (std::uint64_t i = 0; i < count; ++i) {
            try {
                try { value = step(value, false); } catch (std::int64_t) { value = 0; }
            } catch (std::uint64_t error) { value = error; }
            sum += value;
        }
    } else if (mode == 7) {
        for (std::uint64_t i = 0; i < count; ++i) {
            try { value = relay(value, true, 8); }
            catch (std::uint64_t error) { value = error; }
            sum += value;
        }
    } else {
        for (std::uint64_t i = 0; i < count; ++i) {
            try { value = step(value, mode != 8 || i % 1000 == 0); }
            catch (std::uint64_t error) { value = error; }
            sum += value;
        }
    }
    const auto end = eh_bench_clock();
    eh_bench_finish(begin, end, sum);
}
