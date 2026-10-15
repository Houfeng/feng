use std::env;
use std::process;

/// Enumerate permutations in prefix-rotation order and count actual reversals.
fn run(n: usize) -> (i64, usize) {
    let mut permutation: Vec<usize> = (0..n).collect();
    let mut scratch = vec![0; n];
    let mut count = vec![0; n];
    let mut r = n;
    let mut maximum = 0;
    let mut checksum: i64 = 0;
    let mut ordinal: i64 = 0;
    loop {
        while r > 1 {
            count[r - 1] = r;
            r -= 1;
        }
        scratch.copy_from_slice(&permutation);
        let mut flips = 0;
        while scratch[0] != 0 {
            let mut left = 0;
            let mut right = scratch[0];
            while left < right {
                scratch.swap(left, right);
                left += 1;
                right -= 1;
            }
            flips += 1;
        }
        maximum = maximum.max(flips);
        checksum += if ordinal % 2 == 0 {
            flips as i64
        } else {
            -(flips as i64)
        };
        ordinal += 1;
        loop {
            if r == n {
                return (checksum, maximum);
            }
            let first = permutation[0];
            for i in 0..r {
                permutation[i] = permutation[i + 1];
            }
            permutation[r] = first;
            count[r] -= 1;
            if count[r] > 0 {
                break;
            }
            r += 1;
        }
    }
}

/// Validate the size and emit the checksum and maximum flip count.
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        process::exit(2);
    }
    let n = match args[1].parse::<usize>() {
        Ok(value) if (1..=11).contains(&value) => value,
        _ => process::exit(2),
    };
    let (checksum, maximum) = run(n);
    println!("{} {}", checksum, maximum);
}
