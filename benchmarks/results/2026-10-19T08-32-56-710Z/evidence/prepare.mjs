import assert from 'node:assert/strict';
import { copyFileSync, mkdirSync, readFileSync, writeFileSync, chmodSync } from 'node:fs';
import { join, relative } from 'node:path';
import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

// Run from the existing benchmark repository; preserve the baseline before replacing artifacts.
const root = process.cwd();
const { loadBuild, benchmarkEnvironment } = await import(pathToFileURL(join(root, 'benchmarks/scripts/bench.mjs')));
const { hashFile, writeJson, work } = await import(pathToFileURL(join(root, 'benchmarks/scripts/common.mjs')));
const baseline = loadBuild();
const manifest = structuredClone(baseline);
const evidence = join(root, 'temp/node-builtin-benchmark-20261019');
mkdirSync(join(evidence, 'system'), { recursive: true });
writeJson(join(evidence, 'system-build.json'), baseline);
const rebuilt = [];
for (const artifact of manifest.artifacts.filter(item => item.language === 'feng')) {
  const name = artifact.case;
  const previous = baseline.commands.find(item => item.command === baseline.tools.feng.path && item.args.includes(`--name=${name}`));
  assert.ok(previous);
  assert.ok(!previous.args.some(arg => arg.startsWith('--alloc=')));
  copyFileSync(join(root, artifact.path), join(evidence, 'system', name));
  const output = join(work, 'feng-builtin', name);
  const args = [...previous.args.map(arg => arg.startsWith('--out=') ? `--out=${output}` : arg), '--alloc=builtin'];
  const start = performance.now();
  const result = spawnSync(previous.command, args, { cwd: root, env: benchmarkEnvironment(), encoding: 'utf8', timeout: 120000 });
  const log = join(evidence, `${name}-build.log`);
  writeFileSync(log, `${result.stdout ?? ''}${result.stderr ?? ''}`);
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  copyFileSync(join(output, 'bin', name), join(root, artifact.path));
  chmodSync(join(root, artifact.path), 0o755);
  const symbols = spawnSync('nm', ['-D', '--defined-only', join(root, artifact.path)], { encoding: 'utf8' });
  assert.equal(symbols.status, 0, symbols.stderr);
  for (const symbol of ['malloc', 'free', 'calloc', 'realloc']) assert.match(symbols.stdout, new RegExp(`\\bT ${symbol}$`, 'm'));
  writeFileSync(join(evidence, `${name}-symbols.txt`), symbols.stdout);
  artifact.sha256 = hashFile(join(root, artifact.path));
  artifact.bytes = readFileSync(join(root, artifact.path)).length;
  rebuilt.push({ command: previous.command, args, log: relative(root, log), seconds: (performance.now() - start) / 1000, exit_code: result.status });
  console.log(`rebuilt and verified ${name}: --alloc=builtin`);
}
// All other languages retain exactly the binaries and launch configuration measured previously.
assert.deepEqual(manifest.artifacts.filter(item => item.language !== 'feng'), baseline.artifacts.filter(item => item.language !== 'feng'));
const allocator = join(root, 'build/lib/linux-arm64-gnu/libmimalloc.a');
manifest.identities.push({ path: allocator, sha256: hashFile(allocator) });
manifest.commands.push(...rebuilt);
manifest.created_at = new Date().toISOString();
manifest.experiment = { feng_allocator: 'builtin', baseline_archive: '2026-10-19T08-05-53-748Z',
  notes: 'Only the five Feng executables were rebuilt with --alloc=builtin; the other eight configurations are unchanged.',
  preparation_script_sha256: hashFile(join(evidence, 'prepare.mjs')) };
writeJson(join(work, 'build.json'), manifest);
loadBuild();
