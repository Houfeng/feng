import Foundation

/// A reference node whose children are managed by ordinary Swift ARC.
final class Node {
  let left: Node?, right: Node?
  /// Allocate both child trees when the requested depth is nonzero.
  init(_ depth: Int) {
    if depth == 0 {
      left = nil
      right = nil
    } else {
      left = Node(depth - 1)
      right = Node(depth - 1)
    }
  }
}

/// Count real nodes by following the tree's children.
func check(_ node: Node?) -> UInt64 {
  guard let node else { return 0 }
  return 1 + check(node.left) + check(node.right)
}

/// Construct, inspect and release one temporary tree.
func temporary(_ depth: Int) -> UInt64 {
  let tree = Node(depth)
  return check(tree)
}

/// Execute the stretch, temporary and long-lived tree workloads.
func main() {
  guard CommandLine.arguments.count == 2, let depth = Int(CommandLine.arguments[1]),
    depth >= 6, depth <= 20
  else { exit(2) }
  print(temporary(depth + 1))
  let longLived = Node(depth)
  for d in stride(from: 4, through: depth, by: 2) {
    let iterations = UInt64(1) << (depth - d + 4)
    var sum: UInt64 = 0
    for _ in 0..<iterations { sum += temporary(d) }
    print(d, iterations, sum)
  }
  print(check(longLived))
}
main()
