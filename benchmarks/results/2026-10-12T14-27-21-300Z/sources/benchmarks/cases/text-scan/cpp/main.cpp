#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

/** Read the input and validate and aggregate each fixed-format ASCII record. */
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
  if (!file)
    return 2;
  const auto size = file.tellg();
  if (size < 0 || size % 8 != 0)
    return 2;
  std::vector<char> data(static_cast<size_t>(size));
  file.seekg(0);
  if (!file.read(data.data(), size))
    return 2;
  std::uint64_t lines = 0, warnings = 0, errors = 0, sum = 0;
  for (size_t i = 0; i < data.size(); i += 8) {
    const char level = data[i];
    if ((level != 'I' && level != 'W' && level != 'E') || data[i + 1] != ',' ||
        data[i + 7] != '\n')
      return 2;
    std::uint64_t value = 0;
    for (size_t j = i + 2; j < i + 7; ++j) {
      const char digit = data[j];
      if (digit < '0' || digit > '9')
        return 2;
      value = value * 10 + static_cast<unsigned>(digit - '0');
    }
    ++lines;
    warnings += level == 'W';
    errors += level == 'E';
    sum += value;
  }
  std::cout << lines << ' ' << warnings << ' ' << errors << ' ' << sum << '\n';
}
