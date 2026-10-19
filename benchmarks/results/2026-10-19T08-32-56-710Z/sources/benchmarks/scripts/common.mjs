import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
export const suite = join(root, 'benchmarks');
export const work = join(root, 'build/benchmarks');
export const languages = ['feng', 'cpp', 'rust', 'go', 'swift', 'csharp', 'csharp-aot', 'java', 'javascript'];

/** Compute an identity for a source, executable or input file. */
export function hashFile(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

/** Read JSON without accepting absent or partially written data. */
export function readJson(path) { return JSON.parse(readFileSync(path, 'utf8')); }

/** Save reviewable structured records with a trailing newline. */
export function writeJson(path, value) { writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`); }

/** Recursively enumerate regular files in stable order. */
export function filesUnder(path) {
  if (!existsSync(path)) return [];
  return readdirSync(path).sort().flatMap(name => {
    const child = join(path, name);
    return statSync(child).isDirectory() ? filesUnder(child) : [child];
  });
}

/** Identify benchmark sources without including generated or archived results. */
export function sourceIdentities() {
  return [join(suite, 'README.md'), ...filesUnder(join(suite, 'cases')),
    ...filesUnder(join(suite, 'scripts'))].map(path => ({ path: path.slice(root.length + 1), sha256: hashFile(path) }));
}

/** Interpolate a quantile using the R-7 convention on a nonempty sample. */
export function quantile(values, probability) {
  if (!values.length || values.some(v => !Number.isFinite(v)) || !Number.isFinite(probability) || probability < 0 || probability > 1) {
    throw new Error('invalid quantile input');
  }
  const sorted = [...values].sort((a, b) => a - b);
  const index = (sorted.length - 1) * probability;
  const lower = Math.floor(index);
  return sorted[lower] + (sorted[Math.ceil(index)] - sorted[lower]) * (index - lower);
}

/** Rotate starting positions and reverse alternate rounds to limit order bias. */
export function languageOrder(round, configurations = languages) {
  const offset = Math.floor(round / 2) % configurations.length;
  const order = [...configurations.slice(offset), ...configurations.slice(0, offset)];
  return round % 2 ? order.reverse() : order;
}

/** Check numeric checksums, rejecting malformed, nonfinite and extra output. */
export function validateOutput(output, expected, tolerance = 0) {
  const tokens = output.trim().split(/\s+/);
  if (tokens.length !== expected.length) throw new Error(`expected ${expected.length} output fields, received ${tokens.length}`);
  tokens.forEach((token, index) => {
    if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(token)) throw new Error(`invalid numeric output: ${token}`);
    if (!tolerance) {
      if (!/^[+-]?\d+$/.test(token) || BigInt(token) !== BigInt(expected[index])) throw new Error(`checksum mismatch at field ${index}: ${token} != ${expected[index]}`);
    } else {
      const value = Number(token);
      if (!Number.isFinite(value) || Math.abs(value - expected[index]) > tolerance) throw new Error(`numeric mismatch at field ${index}: ${token} != ${expected[index]}`);
    }
  });
}

/** Calculate required tree work and outputs without constructing any tree. */
export function treeReference(depth) {
  const nodes = d => 2 ** (d + 1) - 1;
  const expected = [nodes(depth + 1)];
  let units = nodes(depth + 1) + nodes(depth);
  for (let d = 4; d <= depth; d += 2) {
    const iterations = 2 ** (depth - d + 4);
    expected.push(d, iterations, iterations * nodes(d));
    units += iterations * nodes(d);
  }
  expected.push(nodes(depth));
  return { expected, units, unit: 'nodes/s', tolerance: 0 };
}

/** Simulate with a scalar-array reference, separate from measured executables. */
export function nbodyReference(steps) {
  const bodies = readJson(join(suite, 'cases/nbody/initial.json'));
  const solar = 4 * Math.PI * Math.PI;
  const momentum = [0, 0, 0];
  for (const body of bodies) {
    body[6] *= solar;
    for (let axis = 0; axis < 3; axis++) { body[axis + 3] *= 365.24; momentum[axis] += body[axis + 3] * body[6]; }
  }
  for (let axis = 0; axis < 3; axis++) bodies[0][axis + 3] = -momentum[axis] / solar;
  /** Observe the state's kinetic energy and pairwise gravitational energy. */
  const energy = () => {
    let total = 0;
    for (let i = 0; i < bodies.length; i++) {
      const a = bodies[i];
      total += 0.5 * a[6] * (a[3] * a[3] + a[4] * a[4] + a[5] * a[5]);
      for (let j = i + 1; j < bodies.length; j++) {
        const b = bodies[j], x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        total -= a[6] * b[6] / Math.sqrt(x * x + y * y + z * z);
      }
    }
    return total;
  };
  const before = energy();
  for (let step = 0; step < steps; step++) {
    for (let i = 0; i < bodies.length; i++) for (let j = i + 1; j < bodies.length; j++) {
      const a = bodies[i], b = bodies[j];
      const x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
      const d2 = x * x + y * y + z * z, magnitude = 0.01 / (d2 * Math.sqrt(d2));
      for (const [axis, delta] of [x, y, z].entries()) {
        a[axis + 3] -= delta * b[6] * magnitude;
        b[axis + 3] += delta * a[6] * magnitude;
      }
    }
    for (const body of bodies) for (let axis = 0; axis < 3; axis++) body[axis] += 0.01 * body[axis + 3];
  }
  return { expected: [before, energy()], units: steps, unit: 'steps/s', tolerance: 1e-8 };
}

/** Generate identical ASCII input and derive the checksum from record values. */
export function textInput(path, rows) {
  const data = Buffer.alloc(rows * 8);
  let warnings = 0, errors = 0, sum = 0;
  for (let i = 0; i < rows; i++) {
    const level = 'IWE'[i % 3], value = (i * 17 + 23) % 10000;
    data.write(`${level},${String(value).padStart(5, '0')}\n`, i * 8, 8, 'ascii');
    warnings += level === 'W'; errors += level === 'E'; sum += value;
  }
  writeFileSync(path, data);
  return { expected: [rows, warnings, errors, sum], units: data.length, unit: 'bytes/s', tolerance: 0, sha256: hashFile(path) };
}

/** Independently enumerate permutations recursively, without the measured counter algorithm. */
export function fannkuchReference(n) {
  if (!Number.isInteger(n) || n < 1 || n > 11) throw new Error('invalid permutation size');
  const permutation = Array.from({ length: n }, (_, i) => i), scratch = Array(n);
  let checksum = 0, maximum = 0, ordinal = 0;
  /** Visit smaller prefixes and rotate this prefix through each possible last element. */
  function enumerate(size) {
    if (size === 1) {
      for (let i = 0; i < n; ++i) scratch[i] = permutation[i];
      let flips = 0;
      while (scratch[0] !== 0) {
        for (let left = 0, right = scratch[0]; left < right; ++left, --right) {
          const value = scratch[left]; scratch[left] = scratch[right]; scratch[right] = value;
        }
        ++flips;
      }
      checksum += ordinal++ % 2 === 0 ? flips : -flips;
      maximum = Math.max(maximum, flips);
      return;
    }
    for (let rotation = 0; rotation < size; ++rotation) {
      enumerate(size - 1);
      const first = permutation[0];
      for (let i = 0; i < size - 1; ++i) permutation[i] = permutation[i + 1];
      permutation[size - 1] = first;
    }
  }
  enumerate(n);
  return { expected: [checksum, maximum], units: ordinal, unit: 'permutations/s', tolerance: 0 };
}

/** Materialize a matrix for the independent reference; measured programs evaluate it on demand. */
export function spectralReference(n) {
  if (!Number.isInteger(n) || n < 1 || n > 10000) throw new Error('invalid matrix size');
  const matrix = new Float64Array(n * n), u = new Float64Array(n).fill(1);
  const v = new Float64Array(n), scratch = new Float64Array(n);
  for (let i = 0; i < n; ++i) for (let j = 0; j < n; ++j) {
    matrix[i * n + j] = 1 / ((i + j) * (i + j + 1) / 2 + i + 1);
  }
  /** Multiply by stored coefficients, transposing only the matrix indexing. */
  function multiply(input, output, transpose) {
    for (let i = 0; i < n; ++i) {
      let sum = 0;
      for (let j = 0; j < n; ++j) sum += matrix[transpose ? j * n + i : i * n + j] * input[j];
      output[i] = sum;
    }
  }
  for (let iteration = 0; iteration < 10; ++iteration) {
    multiply(u, scratch, false); multiply(scratch, v, true);
    multiply(v, scratch, false); multiply(scratch, u, true);
  }
  let uv = 0, vv = 0;
  for (let i = 0; i < n; ++i) { uv += u[i] * v[i]; vv += v[i] * v[i]; }
  return { expected: [Math.sqrt(uv / vv)], units: 40 * n * n, unit: 'elements/s', tolerance: 1e-9 };
}
