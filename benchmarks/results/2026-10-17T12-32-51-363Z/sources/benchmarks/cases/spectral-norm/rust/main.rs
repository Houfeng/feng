use std::env;
use std::process;

/// Evaluate one entry of the implicit matrix using zero-based indices.
fn entry(i: usize, j: usize) -> f64 {
    let sum = i + j;
    1.0 / (sum * (sum + 1) / 2 + i + 1) as f64
}

/// Multiply a vector by A, evaluating each entry inside the loop.
fn multiply_a(n: usize, input: &[f64], output: &mut [f64]) {
    for i in 0..n {
        let mut sum = 0.0;
        for j in 0..n {
            sum += entry(i, j) * input[j];
        }
        output[i] = sum;
    }
}

/// Multiply a vector by the transpose of A.
fn multiply_at(n: usize, input: &[f64], output: &mut [f64]) {
    for i in 0..n {
        let mut sum = 0.0;
        for j in 0..n {
            sum += entry(j, i) * input[j];
        }
        output[i] = sum;
    }
}

/// Apply both products using the caller's reusable scratch array.
fn multiply_ata(n: usize, input: &[f64], output: &mut [f64], scratch: &mut [f64]) {
    multiply_a(n, input, scratch);
    multiply_at(n, scratch, output);
}

/// Validate input, perform ten power iterations and emit the norm.
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        process::exit(2);
    }
    let n = match args[1].parse::<usize>() {
        Ok(value) if (1..=10000).contains(&value) => value,
        _ => process::exit(2),
    };
    let mut u = vec![1.0; n];
    let mut v = vec![0.0; n];
    let mut scratch = vec![0.0; n];
    for _ in 0..10 {
        multiply_ata(n, &u, &mut v, &mut scratch);
        multiply_ata(n, &v, &mut u, &mut scratch);
    }
    let (mut uv, mut vv) = (0.0, 0.0);
    for i in 0..n {
        uv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    println!("{:.17}", (uv / vv).sqrt());
}
