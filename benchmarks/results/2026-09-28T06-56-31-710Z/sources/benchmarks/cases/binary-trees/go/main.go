package main

import (
	"fmt"
	"os"
	"strconv"
)

// Node holds the two optional children of a complete tree.
type Node struct{ left, right *Node }

// makeTree recursively allocates the actual nodes of a complete tree.
func makeTree(depth int) *Node {
	if depth == 0 {
		return &Node{}
	}
	return &Node{makeTree(depth - 1), makeTree(depth - 1)}
}

// check counts real nodes by following the tree's children.
func check(node *Node) uint64 {
	if node == nil {
		return 0
	}
	return 1 + check(node.left) + check(node.right)
}

// temporary constructs and inspects a tree, then drops its root reference.
func temporary(depth int) uint64 { tree := makeTree(depth); return check(tree) }

// main executes the stretch, temporary and long-lived tree workloads.
func main() {
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	depth, err := strconv.Atoi(os.Args[1])
	if err != nil || depth < 6 || depth > 20 {
		os.Exit(2)
	}
	fmt.Println(temporary(depth + 1))
	longLived := makeTree(depth)
	for d := 4; d <= depth; d += 2 {
		iterations := uint64(1) << uint(depth-d+4)
		sum := uint64(0)
		for i := uint64(0); i < iterations; i++ {
			sum += temporary(d)
		}
		fmt.Println(d, iterations, sum)
	}
	fmt.Println(check(longLived))
}
