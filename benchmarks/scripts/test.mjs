import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { fannkuchReference, languages, languageOrder, nbodyReference, quantile, spectralReference, textInput, treeReference, validateOutput, work, writeJson } from './common.mjs';
import { artifactFor, benchmarkEnvironment, measure, report } from './bench.mjs';

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

// Every configuration occupies every ordinal position twice in a full cycle.
test('sampling order balances all languages and preserves one run per round', () => {
  const positions = Object.fromEntries(languages.map(language => [language, Array(languages.length).fill(0)]));
  for (let round = 0; round < languages.length * 2; round++) {
    const order = languageOrder(round);
    assert.equal(new Set(order).size, languages.length);
    order.forEach((language, position) => positions[language][position]++);
  }
  for (const counts of Object.values(positions)) assert.deepEqual(counts, Array(languages.length).fill(2));
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
  assert.equal(rows.length, languages.length);
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

// Managed launch arguments precede input arguments without introducing a shell.
test('managed and native artifacts resolve directly to their measured processes', () => {
  const launch = { command: '/runtime with spaces/java', args: ['-cp', '/classes with spaces', 'Main'] };
  const manifest = { artifacts: [
    { case: 'nbody', language: 'java', path: 'build/Main.class', launch },
    { case: 'nbody', language: 'feng', path: 'build/benchmarks/bin/nbody-feng' },
  ] };
  assert.deepEqual(artifactFor(manifest, 'nbody', 'java'), launch);
  assert.deepEqual(artifactFor(manifest, 'nbody', 'feng'), { command: join(work, 'bin/nbody-feng'), args: [] });
  assert.throws(() => artifactFor(manifest, 'nbody', 'missing'), /missing implementation/);
});

// Ambient tuning must not silently change JIT, GC or injected Java arguments.
test('managed runtime configuration uses defaults and suite-owned caches', () => {
  const overrides = { DOTNET_TieredPGO: '0', COMPlus_gcServer: '1', JAVA_TOOL_OPTIONS: '-Xmx64m',
    JDK_JAVA_OPTIONS: '-Xint', JDK_JAVAC_OPTIONS: '--release 8', CLASSPATH: 'ambient' };
  const previous = Object.fromEntries(Object.keys(overrides).map(key => [key, process.env[key]]));
  try {
    Object.assign(process.env, overrides);
    const env = benchmarkEnvironment();
    for (const key of Object.keys(overrides)) assert.equal(env[key], undefined);
    assert.equal(env.NUGET_PACKAGES, join(work, 'cache/nuget'));
    assert.equal(env.DOTNET_CLI_HOME, join(work, 'cache/dotnet'));
  } finally {
    for (const [key, value] of Object.entries(previous)) {
      if (value === undefined) delete process.env[key]; else process.env[key] = value;
    }
  }
});

// Archived runs retain their five-configuration matrix after the suite expands.
test('report keeps historical artifact configurations and their sampling order', () => {
  const fixture = reportFixture('report-legacy');
  const legacy = ['feng', 'cpp', 'rust', 'go', 'swift'];
  const samples = [];
  for (let round = 0; round < 3; ++round) languageOrder(round, legacy).forEach((language, position) => samples.push({
    case: 'nbody', language, round, position, phase: round ? 'measure' : 'warmup',
    wall_ns: (round ? round : 1000) * 1000000, peak_rss_bytes: 1024 ** 2, stdout: '1 2',
  }));
  writeJson(join(fixture.path, 'build.json'), {
    tools: Object.fromEntries(legacy.map(language => [language, { version: 'legacy' }])),
    artifacts: legacy.map(language => ({ case: 'nbody', language })),
  });
  writeFileSync(join(fixture.path, 'samples.jsonl'), samples.map(sample => JSON.stringify(sample)).join('\n') + '\n');
  const rows = report(fixture.path);
  assert.deepEqual(rows.map(row => row.language), legacy);
  assert.equal(rows[0].median_ms, 1.5);
});

// Hand-worked small permutations and the public n=7 result constrain the reference.
test('fannkuch reference covers permutation order, signed checksums and factorial work', () => {
  assert.deepEqual(fannkuchReference(1).expected, [0, 0]);
  assert.deepEqual(fannkuchReference(2).expected, [-1, 1]);
  assert.deepEqual(fannkuchReference(3).expected, [2, 2]);
  assert.deepEqual(fannkuchReference(4).expected, [4, 4]);
  assert.deepEqual(fannkuchReference(7).expected, [228, 16]);
  assert.equal(fannkuchReference(7).units, 5040);
  for (const n of [0, 12, 1.5]) assert.throws(() => fannkuchReference(n), /invalid/);
});

// A closed-form 2x2 eigenvalue and the public n=100 result constrain matrix indexing.
test('spectral reference agrees with independent small matrices and public output', () => {
  assert.deepEqual(spectralReference(1).expected, [1]);
  const a = 1 + 1 / 9, b = 1 / 2 + 1 / 15, d = 1 / 4 + 1 / 25;
  const norm = Math.sqrt((a + d + Math.sqrt((a - d) ** 2 + 4 * b * b)) / 2);
  assert.ok(Math.abs(spectralReference(2).expected[0] - norm) < 1e-12);
  assert.equal(spectralReference(2).units, 160);
  validateOutput('1.274219991', spectralReference(100).expected, 1e-9);
  for (const n of [0, 10001, 1.5]) assert.throws(() => spectralReference(n), /invalid/);
});

// Exact signed arithmetic is needed for the two-element fannkuch checksum.
test('signed integer checksums remain exact and reject fractional or changed output', () => {
  validateOutput('-1 1', [-1, 1]);
  validateOutput('-9007199254740993', [-9007199254740993n]);
  for (const output of ['1 1', '-1.0 1', '-1e0 1', '-1 1 0']) {
    assert.throws(() => validateOutput(output, [-1, 1]));
  }
  assert.throws(() => validateOutput('-9007199254740992', [-9007199254740993n]));
});
