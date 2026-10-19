#include <charconv>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

/** Evaluate one entry of the implicit matrix using zero-based indices. */
double entry(int i, int j) {
  const int sum = i + j;
  return 1.0 / (sum * (sum + 1) / 2 + i + 1);
}

/** Multiply a vector by A, evaluating each entry inside the loop. */
void multiply_a(int n, const std::vector<double> &input,
                std::vector<double> &output) {
  for (int i = 0; i < n; ++i) {
    double sum = 0;
    for (int j = 0; j < n; ++j)
      sum += entry(i, j) * input[j];
    output[i] = sum;
  }
}

/** Multiply a vector by the transpose of A. */
void multiply_at(int n, const std::vector<double> &input,
                 std::vector<double> &output) {
  for (int i = 0; i < n; ++i) {
    double sum = 0;
    for (int j = 0; j < n; ++j)
      sum += entry(j, i) * input[j];
    output[i] = sum;
  }
}

/** Apply both products using the caller's reusable scratch array. */
void multiply_ata(int n, const std::vector<double> &input,
                  std::vector<double> &output, std::vector<double> &scratch) {
  multiply_a(n, input, scratch);
  multiply_at(n, scratch, output);
}

/** Validate input, perform ten power iterations and emit the norm. */
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string_view input(argv[1]);
  int n = 0;
  const auto parsed =
      std::from_chars(input.data(), input.data() + input.size(), n);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
      n < 1 || n > 10000)
    return 2;
  std::vector<double> u(n, 1.0), v(n), scratch(n);
  for (int iteration = 0; iteration < 10; ++iteration) {
    multiply_ata(n, u, v, scratch);
    multiply_ata(n, v, u, scratch);
  }
  double uv = 0, vv = 0;
  for (int i = 0; i < n; ++i) {
    uv += u[i] * v[i];
    vv += v[i] * v[i];
  }
  std::cout << std::setprecision(17) << std::sqrt(uv / vv) << '\n';
}
