import Foundation

/// Enumerate permutations in prefix-rotation order and count actual reversals.
func run(_ n: Int) -> (Int64, Int) {
    var permutation = Array(0..<n)
    var scratch = Array(repeating: 0, count: n)
    var count = Array(repeating: 0, count: n)
    var r = n
    var maximum = 0
    var checksum: Int64 = 0
    var ordinal: Int64 = 0
    while true {
        while r > 1 { count[r - 1] = r; r -= 1 }
        for i in 0..<n { scratch[i] = permutation[i] }
        var flips = 0
        while scratch[0] != 0 {
            var left = 0
            var right = scratch[0]
            while left < right {
                scratch.swapAt(left, right)
                left += 1; right -= 1
            }
            flips += 1
        }
        maximum = max(maximum, flips)
        checksum += ordinal % 2 == 0 ? Int64(flips) : -Int64(flips)
        ordinal += 1
        while true {
            if r == n { return (checksum, maximum) }
            let first = permutation[0]
            for i in 0..<r { permutation[i] = permutation[i + 1] }
            permutation[r] = first; count[r] -= 1
            if count[r] > 0 { break }
            r += 1
        }
    }
}

/// Validate the size and emit the checksum and maximum flip count.
func main() {
    guard CommandLine.arguments.count == 2, let n = Int(CommandLine.arguments[1]),
        n >= 1, n <= 11 else { exit(2) }
    let (checksum, maximum) = run(n)
    print(checksum, maximum)
}
main()
