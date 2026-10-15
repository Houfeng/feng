#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

/** Enumerate permutations in prefix-rotation order and count actual reversals.
 */
std::pair<std::int64_t, int> run(int n) {
  std::vector<int> permutation(n), scratch(n), count(n);
  for (int i = 0; i < n; ++i)
    permutation[i] = i;
  int r = n, maximum = 0;
  std::int64_t checksum = 0, ordinal = 0;
  for (;;) {
    while (r > 1) {
      count[r - 1] = r;
      --r;
    }
    for (int i = 0; i < n; ++i)
      scratch[i] = permutation[i];
    int flips = 0;
    while (scratch[0] != 0) {
      for (int left = 0, right = scratch[0]; left < right; ++left, --right)
        std::swap(scratch[left], scratch[right]);
      ++flips;
    }
    if (flips > maximum)
      maximum = flips;
    checksum += ordinal++ % 2 == 0 ? flips : -flips;
    for (;;) {
      if (r == n)
        return {checksum, maximum};
      const int first = permutation[0];
      for (int i = 0; i < r; ++i)
        permutation[i] = permutation[i + 1];
      permutation[r] = first;
      --count[r];
      if (count[r] > 0)
        break;
      ++r;
    }
  }
}

/** Validate the size and emit the checksum and maximum flip count. */
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string_view input(argv[1]);
  int n = 0;
  const auto parsed =
      std::from_chars(input.data(), input.data() + input.size(), n);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
      n < 1 || n > 11)
    return 2;
  const auto result = run(n);
  std::cout << result.first << ' ' << result.second << '\n';
}
