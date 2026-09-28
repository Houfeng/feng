import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { languages, languageOrder, nbodyReference, quantile, textInput, treeReference, validateOutput, work, writeJson } from './common.mjs';
import { measure, report } from './bench.mjs';

const directory = join(work, 'self-test');
mkdirSync(directory, { recursive: true });

// The public n-body checksum independently constrains the local reference implementation.
test('nbody reference agrees with the public 1000-step checksum', () => {
  validateOutput('-0.169075164 -0.169087605', nbodyReference(1000).expected, 1e-8);
  const zero = nbodyReference(0).expected;
  assert.equal(zero[0], zero[1]);
});

// Tree checks cover odd depth and an independently worked out small tree.
test('tree reference accounts for stretch, temporary and long-lived work', () => {
  assert.deepEqual(treeReference(6).expected, [255, 4, 64, 1984, 6, 16, 2032, 127]);
  assert.equal(treeReference(6).units, 4398);
  assert.deepEqual(treeReference(7).expected, [511, 4, 128, 3968, 6, 32, 4064, 255]);
});

// The generator's exact byte contract must agree with checksums, including empty input.
test('text input has deterministic bytes and independent sums', () => {
  const path = join(directory, 'input with spaces.txt');
  const generated = textInput(path, 3);
  assert.equal(readFileSync(path, 'utf8'), 'I,00023\nW,00040\nE,00057\n');
  assert.deepEqual(generated.expected, [3, 1, 1, 120]);
  assert.equal(generated.units, 24);
  assert.deepEqual(textInput(path, 0).expected, [0, 0, 0, 0]);
});

// Corrupt output must never become a successful benchmark sample.
test('checksum validation rejects missing, extra, nonfinite and incorrect fields', () => {
  for (const output of ['', '1 2 3', 'NaN 2', 'Infinity 2', '1 3', '1abc 2']) {
    assert.throws(() => validateOutput(output, [1, 2]));
  }
  assert.throws(() => validateOutput('1.0001 2', [1, 2], 1e-8));
  assert.throws(() => validateOutput('9007199254740993', [9007199254740992]));
  validateOutput('1.000000001 2\n', [1, 2], 1e-8);
});

// Statistics use known small-sample answers and reject empty data.
test('summary quantiles interpolate without mutating samples', () => {
  const values = [4, 1, 3, 2];
  assert.equal(quantile(values, 0.5), 2.5);
  assert.equal(quantile(values, 0.25), 1.75);
  assert.equal(quantile(values, 0.75), 3.25);
  assert.deepEqual(values, [4, 1, 3, 2]);
  assert.throws(() => quantile([], 0.5));
  assert.throws(() => quantile([NaN], 0.5));
});

// Every language occupies every ordinal position twice in ten rounds.
test('sampling order balances all languages and preserves one run per round', () => {
  const positions = Object.fromEntries(languages.map(language => [language, Array(5).fill(0)]));
  for (let round = 0; round < 10; round++) {
    const order = languageOrder(round);
    assert.equal(new Set(order).size, 5);
    order.forEach((language, position) => positions[language][position]++);
  }
  for (const counts of Object.values(positions)) assert.deepEqual(counts, [2, 2, 2, 2, 2]);
});

// The sampler must execute argv literally and isolate stdout from stderr.
test('external measurement preserves arguments and stream boundaries', async () => {
  const argument = 'a b; $(literal) "quoted"';
  const sample = await measure(process.execPath, ['-e', 'process.stdout.write(process.argv[1]); process.stderr.write("diagnostic");', argument], join(directory, 'spaces in sample'));
  assert.equal(sample.exit_code, 0);
  assert.equal(sample.stdout, argument);
  assert.equal(sample.stderr, 'diagnostic');
  assert.ok(sample.wall_ns > 0);
  assert.ok(sample.peak_rss_bytes > 0);
});

// Peak RSS is checked against touched physical pages rather than virtual allocation alone.
test('external measurement reports child peak RSS in bytes', async () => {
  const sample = await measure(process.execPath, ['-e', 'const b=Buffer.alloc(32*1024*1024,1); process.stdout.write(String(b[0]));'], join(directory, 'rss'));
  assert.equal(sample.exit_code, 0);
  assert.equal(sample.stdout, '1');
  assert.ok(sample.peak_rss_bytes >= 32 * 1024 ** 2);
  assert.ok(sample.peak_rss_bytes < 4 * 1024 ** 3);
});

// A timeout must terminate the child, return failure and retain diagnostics.
test('timeout stops the child and cannot look like a successful run', async () => {
  const sample = await measure(process.execPath, ['-e', 'process.stdout.write("started"); setInterval(()=>{},1000);'], join(directory, 'timeout'), 1);
  assert.equal(sample.stdout, 'started');
  assert.equal(sample.sampler_exit, 124);
  assert.notEqual(sample.signal, 0);
  assert.notEqual(sample.interruption, 0);
  assert.ok(sample.wall_ns >= 900000000 && sample.wall_ns < 5000000000);
});

// Failed exec and failed workloads are distinguished from a zero checksum.
test('failed execution and nonzero child status remain failures', async () => {
  const missing = await measure(join(directory, 'absent executable'), [], join(directory, 'missing'));
  assert.equal(missing.exit_code, 127);
  const failed = await measure(process.execPath, ['-e', 'process.exit(7)'], join(directory, 'failed'));
  assert.equal(failed.exit_code, 7);
  assert.equal(failed.sampler_exit, 7);
});

/** Create a small complete result matrix to exercise report integrity checks. */
function reportFixture(label) {
  const path = join(directory, label);
  mkdirSync(path, { recursive: true });
  const run = { started_at: 'fixture', completed_at: 'fixture', host: {}, config: { profile: 'quick', warmup: 1, rounds: 2, cases: ['nbody'] },
    inputs: { nbody: { expected: [1, 2], tolerance: 1e-8, units: 10, unit: 'steps/s' } } };
  const manifest = { tools: Object.fromEntries(languages.map(language => [language, { version: 'fixture' }])) };
  const samples = [];
  for (let round = 0; round < 3; round++) languageOrder(round).forEach((language, position) => samples.push({
    case: 'nbody', language, round, position, phase: round ? 'measure' : 'warmup',
    wall_ns: (round ? round : 1000) * 1000000, peak_rss_bytes: 1024 ** 2, stdout: '1 2',
  }));
  writeJson(join(path, 'run.json'), run); writeJson(join(path, 'build.json'), manifest);
  writeFileSync(join(path, 'samples.jsonl'), samples.map(sample => JSON.stringify(sample)).join('\n') + '\n');
  return { path, run, samples };
}

// Warmup data must not affect medians, and a missing or duplicate sample is fatal.
test('report excludes warmups and refuses incomplete or tampered results', () => {
  const fixture = reportFixture('report');
  const rows = report(fixture.path);
  assert.equal(rows.length, 5);
  assert.equal(rows[0].median_ms, 1.5);
  fixture.samples.pop();
  writeFileSync(join(fixture.path, 'samples.jsonl'), fixture.samples.map(sample => JSON.stringify(sample)).join('\n'));
  assert.throws(() => report(fixture.path), /incomplete/);
  const second = reportFixture('report-corrupt');
  second.samples[1].stdout = '1 3';
  writeFileSync(join(second.path, 'samples.jsonl'), second.samples.map(sample => JSON.stringify(sample)).join('\n'));
  assert.throws(() => report(second.path), /mismatch/);
  delete second.run.completed_at; writeJson(join(second.path, 'run.json'), second.run);
  assert.throws(() => report(second.path), /incomplete/);
});

// Invalid options must fail before launching any workload.
test('CLI rejects invalid counts and unknown cases', () => {
  for (const args of [['--rounds', '0'], ['--timeout', '-1'], ['--case', 'unknown']]) {
    const result = spawnSync(process.execPath, [join(import.meta.dirname, 'bench.mjs'), 'run', ...args], { encoding: 'utf8' });
    assert.equal(result.status, 1);
    assert.match(result.stderr, /invalid|unknown/);
  }
});
