'use strict';

const { readFileSync } = require('node:fs');

/** Read the complete input and scan validated fixed-width byte records. */
function main() {
  if (process.argv.length !== 3) return 2;
  let data;
  try { data = readFileSync(process.argv[2]); } catch { return 2; }
  if (data.length % 8 !== 0 || data.length / 8 > Math.floor(Number.MAX_SAFE_INTEGER / 99999)) return 2;
  let lines = 0, warnings = 0, errors = 0, sum = 0;
  for (let i = 0; i < data.length; i += 8) {
    const level = data[i];
    if ((level !== 73 && level !== 87 && level !== 69) || data[i + 1] !== 44 || data[i + 7] !== 10) return 2;
    let value = 0;
    for (let j = i + 2; j < i + 7; ++j) {
      const digit = data[j] - 48;
      if (digit < 0 || digit > 9) return 2;
      value = value * 10 + digit;
    }
    ++lines;
    if (level === 87) ++warnings;
    if (level === 69) ++errors;
    sum += value;
  }
  console.log(lines, warnings, errors, sum);
  return 0;
}

process.exitCode = main();
