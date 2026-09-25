#include <cstdint>

/* Match the Feng unsigned recurrence without a one-sided noexcept promise. */
std::uint64_t plain(std::uint64_t value) {
    return (value * UINT64_C(1664525) + UINT64_C(1013904223)) & UINT32_MAX;
}

/* The separate-TU build prevents input-specialized inlining on both sides. */
std::uint64_t step(std::uint64_t value, bool failure) {
    if (failure) throw value;
    return (value * UINT64_C(1664525) + UINT64_C(1013904223)) & UINT32_MAX;
}

/* Use ordinary recursion and let each compiler apply its normal optimizations. */
std::uint64_t relay(std::uint64_t value, bool failure, std::uint64_t depth) {
    if (depth == 0) return step(value, failure);
    return relay(value, failure, depth - 1);
}
