import fs from 'node:fs';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { join, resolve, relative, dirname } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';

const root = process.cwd();
const { measure, loadBuild, benchmarkEnvironment } = await import(pathToFileURL(join(root, 'benchmarks/scripts/bench.mjs')));
const { hashFile, readJson, writeJson, quantile, validateOutput, languageOrder } = await import(pathToFileURL(join(root, 'benchmarks/scripts/common.mjs')));
const { cases, definitions } = await import(pathToFileURL(join(root, 'benchmarks/scripts/definitions.mjs')));
const baselineDirectory = resolve(process.argv[2]);
const baseline = readJson(join(baselineDirectory, 'build.json'));
const originalRun = readJson(join(baselineDirectory, 'run.json'));
const id = new Date().toISOString().replace(/[:.]/g, '-') + '-allocator';
const directory = join(root, 'build/benchmarks/allocator-compare', id);
fs.mkdirSync(directory, { recursive: true });
const config = { id, started_at: new Date().toISOString(), baseline: relative(root, baselineDirectory),
  host: { platform: process.platform, arch: process.arch, cpu: os.cpus()[0].model, release: os.release(), load_average: os.loadavg() },
  profile: 'standard', warmup: 2, rounds: 10, allocators: ['system', 'builtin'], cases,
  inputs: originalRun.inputs, artifacts: {}, commands: [], identities: [...baseline.identities,
    { path: baseline.tools.feng.path, sha256: baseline.tools.feng.sha256 },
    { path: join(root, 'build/lib/macos-arm64/libmimalloc.a'), sha256: hashFile(join(root, 'build/lib/macos-arm64/libmimalloc.a')) }] };
if (process.platform !== 'darwin' || process.arch !== 'arm64' || !originalRun.completed_at) throw new Error('requires completed macOS ARM64 baseline');
if (JSON.stringify(loadBuild()) !== JSON.stringify(baseline)) throw new Error('baseline build changed');

/** Check all shared compiler, library, source, input and executable identities. */
function checkIdentities() {
  if (JSON.stringify(loadBuild()) !== JSON.stringify(baseline)) throw new Error('baseline changed');
  for (const item of config.identities) if (hashFile(item.path) !== item.sha256) throw new Error(`changed dependency: ${item.path}`);
  for (const input of Object.values(config.inputs)) if (input.sha256 && hashFile(input.args[0]) !== input.sha256) throw new Error('input changed');
  for (const variants of Object.values(config.artifacts)) for (const artifact of Object.values(variants)) {
    if (hashFile(artifact.command) !== artifact.sha256) throw new Error('executable changed');
  }
}

/** Require a valid completed child process before using its measurements. */
function success(sample) {
  if (sample.exit_code !== 0 || sample.signal || sample.interruption || sample.sampler_exit !== 0 ||
      !Number.isSafeInteger(sample.wall_ns) || sample.wall_ns <= 0 ||
      !Number.isSafeInteger(sample.peak_rss_bytes) || sample.peak_rss_bytes <= 0) {
    throw new Error(`sample failed: ${JSON.stringify(sample)}`);
  }
}

checkIdentities();
writeJson(join(directory, 'baseline-build.json'), baseline);
console.log(`run: ${directory}`);
for (const name of cases) {
  const original = baseline.commands.find(c => c.command === baseline.tools.feng.path && c.args[0]?.includes(`/cases/${name}/feng/`));
  if (!original || original.args.some(a => a.startsWith('--alloc='))) throw new Error(`unexpected baseline command: ${name}`);
  const args = [...original.args, '--alloc=builtin'];
  const result = spawnSync(original.command, args, { cwd: root, env: benchmarkEnvironment(), encoding: 'utf8', timeout: 120000, maxBuffer: 16 * 1024 * 1024 });
  fs.writeFileSync(join(directory, `${name}-build.log`), `${result.stdout ?? ''}${result.stderr ?? ''}`);
  config.commands.push({ command: original.command, args, exit_code: result.status, signal: result.signal });
  writeJson(join(directory, 'config.json'), config);
  if (result.error || result.status !== 0) throw new Error(`${name} build failed: ${result.error ?? result.stderr}`);
  const output = original.args.find(a => a.startsWith('--out=')).slice(6);
  const binary = join(directory, 'bin', `${name}-builtin`);
  fs.mkdirSync(dirname(binary), { recursive: true });
  fs.copyFileSync(join(output, 'bin', name), binary);
  fs.chmodSync(binary, 0o755);
  const system = baseline.artifacts.find(a => a.case === name && a.language === 'feng');
  config.artifacts[name] = {
    system: { command: join(root, system.path), sha256: system.sha256 },
    builtin: { command: binary, sha256: hashFile(binary) },
  };
  console.log(`built ${name} builtin`);
}
writeJson(join(directory, 'config.json'), config);
checkIdentities();
const verification = [];
const verificationDirectory = join(directory, 'verification');
fs.mkdirSync(verificationDirectory, { recursive: true });
for (const name of cases) for (const check of definitions[name].checks(verificationDirectory)) for (const alloc of config.allocators) {
  const sample = await measure(config.artifacts[name][alloc].command, check.args, join(verificationDirectory, `${name}-${check.id}-${alloc}`));
  if (check.invalid) {
    if (sample.interruption || sample.signal || sample.exit_code <= 0 || sample.exit_code === 127 || sample.sampler_exit === 125 || sample.sampler_exit !== sample.exit_code) throw new Error('invalid input was not rejected correctly');
  } else { success(sample); validateOutput(sample.stdout, check.expected, check.tolerance); }
  verification.push({ case: name, allocator: alloc, input: check.id, invalid: Boolean(check.invalid), expected: check.expected, ...sample });
}
writeJson(join(directory, 'verified.json'), { count: verification.length, results: verification });
console.log(`verified ${verification.length} executions`);
const records = [];
for (const name of cases) for (let round = 0; round < config.warmup + config.rounds; round++) {
  const phase = round < config.warmup ? 'warmup' : 'measure';
  for (const [position, alloc] of languageOrder(round, config.allocators).entries()) {
    const input = config.inputs[name];
    const sample = await measure(config.artifacts[name][alloc].command, input.args, join(directory, 'samples', `${name}-${round}-${alloc}`));
    success(sample);
    validateOutput(sample.stdout, input.expected, input.tolerance);
    const record = { case: name, allocator: alloc, phase, round, position, ...sample };
    records.push(record);
    fs.appendFileSync(join(directory, 'samples.jsonl'), JSON.stringify(record) + '\n');
    console.log(`${name} ${phase} ${round + 1}/${config.warmup + config.rounds} ${alloc}: ${(sample.wall_ns / 1e6).toFixed(3)} ms ${(sample.peak_rss_bytes / 2 ** 20).toFixed(2)} MiB`);
  }
}
checkIdentities();
if (records.length !== cases.length * config.allocators.length * (config.warmup + config.rounds)) throw new Error('incomplete samples');
config.completed_at = new Date().toISOString();
config.final_load_average = os.loadavg();
writeJson(join(directory, 'config.json'), config);
const summary = [];
for (const name of cases) {
  const row = { case: name };
  for (const alloc of config.allocators) {
    const selected = records.filter(r => r.case === name && r.allocator === alloc && r.phase === 'measure');
    const times = selected.map(r => r.wall_ns / 1e6), rss = selected.map(r => r.peak_rss_bytes / 2 ** 20);
    row[alloc] = { milliseconds: quantile(times, .5), p25: quantile(times, .25), p75: quantile(times, .75), min: Math.min(...times), max: Math.max(...times), rss_mib: quantile(rss, .5), rss_max_mib: Math.max(...rss) };
  }
  row.time_change_percent = 100 * (row.builtin.milliseconds / row.system.milliseconds - 1);
  row.rss_change_percent = 100 * (row.builtin.rss_mib / row.system.rss_mib - 1);
  summary.push(row);
}
writeJson(join(directory, 'summary.json'), summary);
const report = ['# Feng system / builtin 分配器对照', '',
  `开始：${config.started_at}；结束：${config.completed_at}。Apple M4 Pro / macOS ARM64。`, '',
  '复用同一编译器、runtime、std、源码及 standard 输入；system 为上一轮原制品，builtin 复用原构建命令及输出路径，仅追加 --alloc=builtin。',
  '每项每版预热 2 轮、正式 10 轮，交错并反转运行顺序；每次为新进程，统计启动到退出耗时和峰值 RSS，每次均校验输出。',
  `正确性校验 ${verification.length} 次；正式样本 ${records.filter(r => r.phase === 'measure').length} 个。仅运行性能评测，未运行全量回归。`, '',
  '| 用例 | system ms | builtin ms | 耗时变化 | system RSS MiB | builtin RSS MiB | RSS 变化 |',
  '|---|---:|---:|---:|---:|---:|---:|',
  ...summary.map(r => `| ${r.case} | ${r.system.milliseconds.toFixed(3)} | ${r.builtin.milliseconds.toFixed(3)} | ${r.time_change_percent.toFixed(1)}% | ${r.system.rss_mib.toFixed(2)} | ${r.builtin.rss_mib.toFixed(2)} | ${r.rss_change_percent.toFixed(1)}% |`), '',
  '表内为中位数，负数表示降低。P25/P75、最小/最大值及最大 RSS 见 summary.json，所有样本及输出见 samples.jsonl。短任务及小幅差异可能受启动和系统调度噪声影响。',
  '本轮为 Feng 两种分配器的专项对照，不代表八语言全套重新评测。上一轮八配置报告见 ../2026-10-19T05-47-30-350Z/report.md。',
  '复现：在相同仓库根目录执行 node <本目录>/run.mjs build/benchmarks/runs/2026-10-19T05-47-30-350Z；要求原 system 制品及依赖身份匹配。', ''];
fs.writeFileSync(join(directory, 'report.md'), report.join('\n'));
const archive = join(root, 'benchmarks/results', id);
fs.mkdirSync(archive, { recursive: false });
for (const name of ['config.json', 'baseline-build.json', 'verified.json', 'samples.jsonl', 'summary.json', 'report.md', ...cases.map(name => `${name}-build.log`)]) fs.copyFileSync(join(directory, name), join(archive, name));
fs.copyFileSync(fileURLToPath(import.meta.url), join(archive, 'run.mjs'));
console.log(`archived: ${archive}`);
console.log(report.join('\n'));
