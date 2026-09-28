/** Read the input and validate and aggregate each fixed-format ASCII record. */
fn main() {
    let args: Vec<String> = std::env::args().collect();
    assert_eq!(args.len(), 2);
    let data = std::fs::read(&args[1]).expect("cannot read input");
    assert_eq!(data.len() % 8, 0);
    let (mut lines, mut warnings, mut errors, mut sum) = (0u64, 0u64, 0u64, 0u64);
    for record in data.chunks_exact(8) {
        let level = record[0];
        assert!(matches!(level, b'I' | b'W' | b'E'));
        assert_eq!(record[1], b',');
        assert_eq!(record[7], b'\n');
        let mut value = 0u64;
        for &digit in &record[2..7] {
            assert!(digit.is_ascii_digit());
            value = value * 10 + u64::from(digit - b'0');
        }
        lines += 1;
        warnings += u64::from(level == b'W');
        errors += u64::from(level == b'E');
        sum += value;
    }
    println!("{} {} {} {}", lines, warnings, errors, sum);
}
