import Foundation

/// Read, validate and aggregate fixed-format ASCII log records.
func main() throws {
  guard CommandLine.arguments.count == 2 else { exit(2) }
  let data = Array(try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
  guard data.count % 8 == 0 else { exit(2) }
  var lines: UInt64 = 0
  var warnings: UInt64 = 0
  var errors: UInt64 = 0
  var sum: UInt64 = 0
  for i in stride(from: 0, to: data.count, by: 8) {
    let level = data[i]
    guard (level == 73 || level == 87 || level == 69) && data[i + 1] == 44 && data[i + 7] == 10
    else { exit(2) }
    var value: UInt64 = 0
    for j in (i + 2)..<(i + 7) {
      let digit = data[j]
      guard digit >= 48 && digit <= 57 else { exit(2) }
      value = value * 10 + UInt64(digit - 48)
    }
    lines += 1
    if level == 87 { warnings += 1 }
    if level == 69 { errors += 1 }
    sum += value
  }
  print(lines, warnings, errors, sum)
}
try main()
