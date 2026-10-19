'use strict';

/** Enumerate permutations in prefix-rotation order and count actual reversals. */
function run(n) {
  const permutation = new Int32Array(n), scratch = new Int32Array(n), count = new Int32Array(n);
  for (let i = 0; i < n; ++i) permutation[i] = i;
  let r = n, maximum = 0, checksum = 0, ordinal = 0;
  for (;;) {
    while (r > 1) { count[r - 1] = r; --r; }
    scratch.set(permutation);
    let flips = 0;
    while (scratch[0] !== 0) {
      for (let left = 0, right = scratch[0]; left < right; ++left, --right) {
        const value = scratch[left]; scratch[left] = scratch[right]; scratch[right] = value;
      }
      ++flips;
    }
    if (flips > maximum) maximum = flips;
    checksum += ordinal++ % 2 === 0 ? flips : -flips;
    for (;;) {
      if (r === n) return [checksum, maximum];
      const first = permutation[0];
      for (let i = 0; i < r; ++i) permutation[i] = permutation[i + 1];
      permutation[r] = first; --count[r];
      if (count[r] > 0) break;
      ++r;
    }
  }
}

if (process.argv.length !== 3 || !/^\d+$/.test(process.argv[2])) process.exit(2);
const n = Number(process.argv[2]);
if (!Number.isInteger(n) || n < 1 || n > 11) process.exit(2);
console.log(...run(n));
