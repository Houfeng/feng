'use strict';

/** Evaluate a single implicit matrix entry without caching a matrix. */
function entry(i, j) { return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1); }

/** Multiply A by a vector, preserving sequential summation order. */
function multiplyA(input, output) {
  for (let i = 0; i < input.length; ++i) {
    let sum = 0;
    for (let j = 0; j < input.length; ++j) sum += entry(i, j) * input[j];
    output[i] = sum;
  }
}

/** Multiply the transpose of A by a vector. */
function multiplyAt(input, output) {
  for (let i = 0; i < input.length; ++i) {
    let sum = 0;
    for (let j = 0; j < input.length; ++j) sum += entry(j, i) * input[j];
    output[i] = sum;
  }
}

/** Apply A followed by its transpose, reusing the scratch vector. */
function multiplyAtA(input, output, scratch) {
  multiplyA(input, scratch);
  multiplyAt(scratch, output);
}

if (process.argv.length !== 3 || !/^\d+$/.test(process.argv[2])) process.exit(2);
const n = Number(process.argv[2]);
if (!Number.isInteger(n) || n < 1 || n > 10000) process.exit(2);
const u = new Float64Array(n).fill(1), v = new Float64Array(n), scratch = new Float64Array(n);
for (let iteration = 0; iteration < 10; ++iteration) {
  multiplyAtA(u, v, scratch);
  multiplyAtA(v, u, scratch);
}
let uv = 0, vv = 0;
for (let i = 0; i < n; ++i) { uv += u[i] * v[i]; vv += v[i] * v[i]; }
console.log(Math.sqrt(uv / vv));
