package main

import (
	"fmt"
	"os"
	"strconv"
)

// run enumerates permutations in prefix-rotation order and counts actual reversals.
func run(n int) (int64, int) {
	permutation, scratch, count := make([]int, n), make([]int, n), make([]int, n)
	for i := range permutation {
		permutation[i] = i
	}
	r, maximum := n, 0
	var checksum, ordinal int64
	for {
		for r > 1 {
			count[r-1] = r
			r--
		}
		copy(scratch, permutation)
		flips := 0
		for scratch[0] != 0 {
			for left, right := 0, scratch[0]; left < right; left, right = left+1, right-1 {
				scratch[left], scratch[right] = scratch[right], scratch[left]
			}
			flips++
		}
		if flips > maximum {
			maximum = flips
		}
		if ordinal%2 == 0 {
			checksum += int64(flips)
		} else {
			checksum -= int64(flips)
		}
		ordinal++
		for {
			if r == n {
				return checksum, maximum
			}
			first := permutation[0]
			for i := 0; i < r; i++ {
				permutation[i] = permutation[i+1]
			}
			permutation[r] = first
			count[r]--
			if count[r] > 0 {
				break
			}
			r++
		}
	}
}

// main validates the size and emits the checksum and maximum flip count.
func main() {
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	n, err := strconv.Atoi(os.Args[1])
	if err != nil || n < 1 || n > 11 {
		os.Exit(2)
	}
	checksum, maximum := run(n)
	fmt.Println(checksum, maximum)
}
