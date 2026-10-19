import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { fannkuchReference, suite, validateOutput, work } from './common.mjs';
import { definitions } from './definitions.mjs';
import { artifactFor, benchmarkEnvironment } from './bench.mjs';

const directory = join(work, 'node-self-test');
mkdirSync(directory, { recursive: true });

/** Execute the standalone JavaScript implementation with literal arguments. */
function execute(name, args) {
  const result = spawnSync(process.execPath, [join(suite, 'cases', name, 'javascript/main.js'), ...args],
    { encoding: 'utf8', env: benchmarkEnvironment(), timeout: 120000 });
  assert.ifError(result.error);
  assert.equal(result.signal, null);
  return result;
}

// The same independent checks used by every language also constrain JavaScript.
test('JavaScript implementations satisfy all common correctness checks', () => {
  for (const [name, definition] of Object.entries(definitions)) {
    for (const check of definition.checks(directory)) {
      const result = execute(name, check.args);
      if (check.invalid) assert.equal(result.status, 2, `${name}/${check.id}`);
      else {
        assert.equal(result.status, 0, result.stderr);
        validateOutput(result.stdout, check.expected, check.tolerance);
      }
    }
  }
});

// Exhaustive enumeration establishes an exact bound for every supported n.
test('fannkuch Number checksum and all intermediate sums are exact throughout the supported range', () => {
  for (let n = 1; n <= 11; ++n) {
    const reference = fannkuchReference(n);
    const bound = reference.units * reference.expected[1];
    assert.ok(Number.isSafeInteger(bound), `unsafe absolute checksum bound for n=${n}`);
    const result = execute('fannkuch-redux', [String(n)]);
    assert.equal(result.status, 0, result.stderr);
    validateOutput(result.stdout, reference.expected);
  }
});

// Bad CLI inputs must fail cleanly rather than trigger an unbounded workload.
test('JavaScript numerical arguments reject malformed and out-of-range inputs', () => {
  const bounds = { nbody: ['-1', '100000001'], 'binary-trees': ['5', '21'], 'fannkuch-redux': ['0', '12'], 'spectral-norm': ['0', '10001'] };
  for (const [name, invalid] of Object.entries(bounds)) {
    for (const args of [[], ['1', '2'], ...['', 'abc', '1.5', 'NaN', 'Infinity', '9007199254740993', ...invalid].map(value => [value])]) {
      assert.equal(execute(name, args).status, 2, `${name}: ${JSON.stringify(args)}`);
    }
  }
});

// Literal paths, read failures and maximum record values are part of the byte contract.
test('JavaScript text scan handles paths, failures and exact integer accumulation', () => {
  const path = join(directory, 'input with spaces.txt');
  writeFileSync(path, 'E,99999\nW,99999\nI,99999\n');
  const result = execute('text-scan', [path]);
  assert.equal(result.status, 0, result.stderr);
  validateOutput(result.stdout, [3, 1, 1, 299997]);
  for (const args of [[], [path, path], [join(directory, 'missing.txt')]]) assert.equal(execute('text-scan', args).status, 2);
  assert.ok(Number.isSafeInteger(32000000 * 99999));
});

// The measured process must be Node itself, with no shell or build wrapper.
test('JavaScript launch preserves the selected runtime and script paths', () => {
  const launch = { command: '/runtime with spaces/node', args: ['/case with spaces/main.js'] };
  assert.deepEqual(artifactFor({ artifacts: [{ case: 'nbody', language: 'javascript', launch }] }, 'nbody', 'javascript'), launch);
});

// Ambient Node settings must not silently tune the measured runtime.
test('benchmark environment removes Node tuning and cache settings', () => {
  const names = ['NODE_OPTIONS', 'NODE_PATH', 'NODE_V8_COVERAGE', 'NODE_COMPILE_CACHE', 'NODE_DISABLE_COMPILE_CACHE'];
  const saved = names.map(name => process.env[name]);
  try {
    for (const name of names) process.env[name] = 'test-only';
    const environment = benchmarkEnvironment();
    for (const name of names) assert.equal(environment[name], undefined);
  } finally {
    names.forEach((name, index) => { if (saved[index] === undefined) delete process.env[name]; else process.env[name] = saved[index]; });
  }
});
