package main

import (
	"fmt"
	"math"
	"os"
	"strconv"
)

// entry evaluates one entry of the implicit matrix using zero-based indices.
func entry(i, j int) float64 {
	sum := i + j
	return 1.0 / float64(sum*(sum+1)/2+i+1)
}

// multiplyA multiplies a vector by A, evaluating each entry inside the loop.
func multiplyA(n int, input, output []float64) {
	for i := range output[:n] {
		sum := 0.0
		for j, value := range input[:n] {
			sum += entry(i, j) * value
		}
		output[i] = sum
	}
}

// multiplyAt multiplies a vector by the transpose of A.
func multiplyAt(n int, input, output []float64) {
	for i := range output[:n] {
		sum := 0.0
		for j, value := range input[:n] {
			sum += entry(j, i) * value
		}
		output[i] = sum
	}
}

// multiplyAtA applies both products using the caller's reusable scratch array.
func multiplyAtA(n int, input, output, scratch []float64) {
	multiplyA(n, input, scratch)
	multiplyAt(n, scratch, output)
}

// main validates input, performs ten power iterations and emits the norm.
func main() {
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	n, err := strconv.Atoi(os.Args[1])
	if err != nil || n < 1 || n > 10000 {
		os.Exit(2)
	}
	u, v, scratch := make([]float64, n), make([]float64, n), make([]float64, n)
	for i := range u {
		u[i] = 1
	}
	for iteration := 0; iteration < 10; iteration++ {
		multiplyAtA(n, u, v, scratch)
		multiplyAtA(n, v, u, scratch)
	}
	uv, vv := 0.0, 0.0
	for i := 0; i < n; i++ {
		uv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	fmt.Printf("%.17g\n", math.Sqrt(uv/vv))
}
