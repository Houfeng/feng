#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

/** A node owns its two optional children. */
struct Node {
  std::unique_ptr<Node> left, right;
};

/** Construct a complete tree with separately owned nodes. */
std::unique_ptr<Node> make_tree(unsigned depth) {
  auto node = std::make_unique<Node>();
  if (depth > 0) {
    node->left = make_tree(depth - 1);
    node->right = make_tree(depth - 1);
  }
  return node;
}

/** Count real nodes by following the tree's children. */
std::uint64_t check(const Node *node) {
  return node ? 1 + check(node->left.get()) + check(node->right.get()) : 0;
}

/** Construct, inspect and release one temporary tree. */
std::uint64_t temporary(unsigned depth) {
  auto tree = make_tree(depth);
  return check(tree.get());
}

/** Execute the stretch, temporary and long-lived tree workloads. */
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string_view input(argv[1]);
  unsigned depth = 0;
  const auto parsed =
      std::from_chars(input.data(), input.data() + input.size(), depth);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
      depth < 6 || depth > 20)
    return 2;
  std::cout << temporary(depth + 1) << '\n';
  auto long_lived = make_tree(depth);
  for (unsigned d = 4; d <= depth; d += 2) {
    const std::uint64_t iterations = std::uint64_t{1} << (depth - d + 4);
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iterations; ++i)
      sum += temporary(d);
    std::cout << d << ' ' << iterations << ' ' << sum << '\n';
  }
  std::cout << check(long_lived.get()) << '\n';
}
