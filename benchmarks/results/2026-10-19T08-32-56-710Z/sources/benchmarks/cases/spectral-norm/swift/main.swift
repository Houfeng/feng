import Foundation

/// Evaluate one entry of the implicit matrix using zero-based indices.
func entry(_ i: Int, _ j: Int) -> Double {
    let sum = i + j
    return 1.0 / Double(sum * (sum + 1) / 2 + i + 1)
}

/// Multiply a vector by A, evaluating each entry inside the loop.
func multiplyA(_ n: Int, _ input: [Double], _ output: inout [Double]) {
    for i in 0..<n {
        var sum = 0.0
        for j in 0..<n { sum += entry(i, j) * input[j] }
        output[i] = sum
    }
}

/// Multiply a vector by the transpose of A.
func multiplyAt(_ n: Int, _ input: [Double], _ output: inout [Double]) {
    for i in 0..<n {
        var sum = 0.0
        for j in 0..<n { sum += entry(j, i) * input[j] }
        output[i] = sum
    }
}

/// Apply both products using the caller's reusable scratch array.
func multiplyAtA(_ n: Int, _ input: [Double], _ output: inout [Double], _ scratch: inout [Double]) {
    multiplyA(n, input, &scratch)
    multiplyAt(n, scratch, &output)
}

/// Validate input, perform ten power iterations and emit the norm.
func main() {
    guard CommandLine.arguments.count == 2, let n = Int(CommandLine.arguments[1]),
        n >= 1, n <= 10000 else { exit(2) }
    var u = Array(repeating: 1.0, count: n)
    var v = Array(repeating: 0.0, count: n)
    var scratch = Array(repeating: 0.0, count: n)
    for _ in 0..<10 {
        multiplyAtA(n, u, &v, &scratch)
        multiplyAtA(n, v, &u, &scratch)
    }
    var uv = 0.0
    var vv = 0.0
    for i in 0..<n { uv += u[i] * v[i]; vv += v[i] * v[i] }
    print((uv / vv).squareRoot())
}
main()
