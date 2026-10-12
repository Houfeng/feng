import { spawn, spawnSync } from 'node:child_process';
import { appendFileSync, chmodSync, copyFileSync, existsSync, mkdirSync, readFileSync, realpathSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import { basename, dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { filesUnder, hashFile, languageOrder, languages, quantile, readJson,
  root, sourceIdentities, suite, validateOutput, work, writeJson } from './common.mjs';
import { cases, definitions, profiles } from './definitions.mjs';

const extensions = { feng: 'ff', cpp: 'cpp', rust: 'rs', go: 'go', swift: 'swift' };
const controlledVariables = ['CFLAGS', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS', 'FENG_CC_FLAGS', 'FENG_CC',
  'FENG_AR', 'FENG_RANLIB', 'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'GOFLAGS', 'GOEXPERIMENT', 'GOARCH',
  'GOOS', 'GOAMD64', 'GOARM64', 'GOGC', 'GOMEMLIMIT', 'GODEBUG', 'SWIFTFLAGS', 'SDKROOT',
  'ASAN_OPTIONS', 'UBSAN_OPTIONS', 'LSAN_OPTIONS', 'LD_PRELOAD', 'DYLD_INSERT_LIBRARIES', 'COMPILER_PATH'];

/** Pin measurement-relevant environment while preserving normal tool lookup. */
export function benchmarkEnvironment() {
  const env = { ...process.env };
  for (const key of controlledVariables) delete env[key];
  Object.assign(env, { LC_ALL: 'C', LANG: 'C', TZ: 'UTC', GOMAXPROCS: '1', GOTOOLCHAIN: 'local',
    GOPROXY: 'off', GOWORK: 'off', GOCACHE: join(work, 'cache/go'), GOMODCACHE: join(work, 'cache/gomod'),
    CLANG_MODULE_CACHE_PATH: join(work, 'cache/clang'), TMPDIR: join(work, 'tmp') });
  return env;
}

/** Create only suite-owned build, log and cache directories. */
function prepareWork() {
  for (const dir of ['bin', 'logs', 'runs', 'cache/go', 'cache/gomod', 'cache/clang', 'cache/swift', 'tmp']) {
    mkdirSync(join(work, dir), { recursive: true });
  }
}

/** Resolve a compiler path without evaluating a shell command. */
function executable(name) {
  const candidates = name.includes('/') ? [resolve(root, name)] : (process.env.PATH ?? '').split(':').map(dir => join(dir, name));
  for (const candidate of candidates) {
    // Driver aliases such as clang++ and rustc rely on their invocation name.
    if (existsSync(candidate)) return resolve(candidate);
  }
  throw new Error(`required tool not found: ${name}`);
}

/** Capture read-only metadata, failing if a required tool cannot start. */
function capture(command, args, env) {
  const result = spawnSync(command, args, { cwd: root, env, encoding: 'utf8', timeout: 60000, maxBuffer: 8 * 1024 * 1024 });
  if (result.error || result.status !== 0) throw new Error(`${command} ${args.join(' ')}: ${result.error?.message ?? result.stderr}`);
  return result.stdout.trim();
}

/** Record machine identity without collecting arbitrary environment variables. */
function hostMetadata() {
  return { platform: process.platform, arch: process.arch, release: os.release(), version: os.version(),
    cpu: os.cpus()[0]?.model, logical_cpus: os.cpus().length, total_memory_bytes: os.totalmem(),
    node: process.version, load_average: os.loadavg() };
}

/** Build one artifact and retain its complete command and diagnostic log. */
function compile(command, args, label, env, commands) {
  const log = join(work, 'logs', `${label}.log`);
  const start = performance.now();
  const result = spawnSync(command, args, { cwd: root, env, encoding: 'utf8', timeout: 900000, maxBuffer: 32 * 1024 * 1024 });
  writeFileSync(log, `${result.stdout ?? ''}${result.stderr ?? ''}`);
  commands.push({ command, args, log: relative(root, log), seconds: (performance.now() - start) / 1000, exit_code: result.status });
  writeJson(join(work, 'commands.json'), commands);
  if (result.error || result.status !== 0) throw new Error(`${label} failed: ${result.error?.message ?? ''}\n${(result.stderr ?? result.stdout ?? '').slice(-8000)}\nlog: ${log}`);
  console.log(`built ${label}`);
}

/** Build all five implementations and freeze their reproducibility metadata. */
function build() {
  prepareWork();
  rmSync(join(work, 'build.json'), { force: true });
  const env = benchmarkEnvironment();
  const selectors = { feng: process.env.FENG ?? join(root, 'build/bin/feng'), cc: process.env.CC ?? 'clang',
    cpp: process.env.CXX ?? 'clang++', rust: process.env.RUSTC ?? 'rustc', go: process.env.GO ?? 'go', swift: process.env.SWIFTC ?? 'swiftc' };
  const tools = {};
  for (const [language, selector] of Object.entries(selectors)) {
    const path = executable(selector);
    tools[language] = { path, real_path: realpathSync(path), version: capture(path, language === 'go' ? ['version'] : ['--version'], env), sha256: hashFile(path) };
  }
  if (!['darwin', 'linux'].includes(process.platform)) throw new Error('measurement currently supports macOS and Linux');
  const platform = process.platform === 'darwin' && process.arch === 'arm64' ? 'macos-arm64'
    : process.platform === 'linux' && ['arm64', 'x64'].includes(process.arch) ? `linux-${process.arch}-gnu` : null;
  if (!platform) throw new Error('unsupported Feng host platform');
  const commands = [];
  compile(tools.cc.path, ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', join(suite, 'scripts/measure.c'), '-o', join(work, 'bin/measure')], 'measure', env, commands);
  // Packaging rebuilds the actual current std in release mode and includes its native assets.
  compile(tools.feng.path, ['pack', join(root, 'std/std'), `--platform=${platform}`], 'feng-std', env, commands);
  const packages = filesUnder(join(root, 'std/std/build/pkg')).filter(path => path.endsWith('.fb'));
  if (packages.length !== 1) throw new Error('expected exactly one rebuilt std .fb package');
  const stdPackage = packages[0];
  const artifacts = [];
  for (const name of cases) for (const language of languages) {
    const source = join(suite, 'cases', name, language, `main.${extensions[language]}`);
    const binary = join(work, 'bin', `${name}-${language}`);
    const command = tools[language].path;
    let args;
    if (language === 'feng') {
      const output = join(work, 'feng', name);
      args = [source, '--target=bin', '--release', `--platform=${platform}`, `--pkg=${stdPackage}`, `--out=${output}`, `--name=${name}`];
      compile(command, args, `${name}-${language}`, env, commands);
      copyFileSync(join(output, 'bin', name), binary); chmodSync(binary, 0o755);
    } else {
      const options = {
        cpp: ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', source, '-o', binary],
        rust: ['--edition=2021', '-C', 'opt-level=3', source, '-o', binary],
        go: ['build', '-o', binary, source],
        swift: ['-O', '-module-cache-path', join(work, 'cache/swift'), source, '-o', binary],
      };
      args = options[language];
      compile(command, args, `${name}-${language}`, env, commands);
    }
    artifacts.push({ case: name, language, path: relative(root, binary), sha256: hashFile(binary), bytes: readFileSync(binary).length });
  }
  const install = dirname(dirname(realpathSync(tools.feng.path)));
  const runtime = join(install, 'lib', platform, 'libfeng_runtime.a');
  const plugin = join(install, 'toolchain/llvm-c-eh/lib', process.platform === 'darwin' ? 'llvm_c_eh.dylib' : 'llvm_c_eh.so');
  const identities = [runtime, join(install, 'include/feng_runtime.h'), plugin, stdPackage].map(path => ({ path, sha256: hashFile(path) }));
  const git = { revision: capture('git', ['rev-parse', 'HEAD'], env), status: capture('git', ['status', '--short'], env) };
  const manifest = { schema: 1, created_at: new Date().toISOString(), host: hostMetadata(), platform, tools, commands,
    git, identities, cleared_environment: controlledVariables,
    environment: { LC_ALL: env.LC_ALL, GOMAXPROCS: env.GOMAXPROCS, GOTOOLCHAIN: env.GOTOOLCHAIN, GOPROXY: env.GOPROXY },
    sources: sourceIdentities(), artifacts, sampler: { path: 'build/benchmarks/bin/measure', sha256: hashFile(join(work, 'bin/measure')) } };
  writeJson(join(work, 'build.json'), manifest);
  console.log(`all ${artifacts.length} programs built; manifest: ${join(work, 'build.json')}`);
}

/** Reject stale artifacts instead of comparing mixed source and binary versions. */
export function loadBuild() {
  const manifest = readJson(join(work, 'build.json'));
  if (JSON.stringify(manifest.sources) !== JSON.stringify(sourceIdentities())) throw new Error('benchmark sources changed; run build again');
  for (const artifact of [...manifest.artifacts, manifest.sampler]) {
    if (hashFile(join(root, artifact.path)) !== artifact.sha256) throw new Error(`artifact changed: ${artifact.path}; rebuild required`);
  }
  if (manifest.artifacts.length !== cases.length * languages.length) throw new Error('incomplete build manifest');
  return manifest;
}

/** Run the external sampler while forwarding cancellation to its process group handler. */
export async function measure(binary, args, prefix, timeout = 120) {
  mkdirSync(dirname(prefix), { recursive: true });
  const paths = { metrics: `${prefix}.json`, stdout: `${prefix}.stdout`, stderr: `${prefix}.stderr` };
  const command = join(work, 'bin/measure');
  const arguments_ = [String(timeout), paths.metrics, paths.stdout, paths.stderr, binary, ...args];
  const result = await new Promise((resolvePromise, reject) => {
    const child = spawn(command, arguments_, { cwd: root, env: benchmarkEnvironment(), stdio: ['ignore', 'ignore', 'pipe'] });
    let diagnostic = '';
    /** Ask the sampler to stop and reap its measured process before returning. */
    const interrupt = () => child.kill('SIGTERM');
    process.on('SIGINT', interrupt); process.on('SIGTERM', interrupt);
    /** Remove temporary handlers on either startup failure or normal completion. */
    const cleanup = () => { process.off('SIGINT', interrupt); process.off('SIGTERM', interrupt); };
    child.stderr.on('data', data => { diagnostic = (diagnostic + data).slice(-8000); });
    child.on('error', error => { cleanup(); reject(error); });
    child.on('close', (code, signal) => { cleanup(); resolvePromise({ code, signal, diagnostic }); });
  });
  if (!existsSync(paths.metrics)) throw new Error(`sampler failed: ${result.diagnostic || result.signal || result.code}`);
  const metrics = readJson(paths.metrics);
  return { ...metrics, sampler_exit: result.code, stdout: readFileSync(paths.stdout, 'utf8'), stderr: readFileSync(paths.stderr, 'utf8'), files: paths };
}

/** Require a completed, successful measurement before interpreting its checksum. */
function requireSuccess(sample, context) {
  if (sample.exit_code !== 0 || sample.signal || sample.interruption || sample.sampler_exit !== 0 ||
      !Number.isSafeInteger(sample.wall_ns) || sample.wall_ns <= 0 || !Number.isSafeInteger(sample.peak_rss_bytes) || sample.peak_rss_bytes <= 0) {
    throw new Error(`${context} failed: ${JSON.stringify(sample).slice(0, 2500)}`);
  }
}

/** Resolve one language's executable from the frozen build record. */
function artifactFor(manifest, name, language) {
  const artifact = manifest.artifacts.find(item => item.case === name && item.language === language);
  if (!artifact) throw new Error(`missing implementation: ${name}/${language}`);
  return join(root, artifact.path);
}

/** Check boundary cases, independent expected results and malformed text input. */
async function verify(manifest, output, selected = cases, timeout = 120) {
  mkdirSync(output, { recursive: true });
  let count = 0;
  const checks = selected.flatMap(name => definitions[name].checks(output).map(check => ({ ...check, name })));
  const results = [];
  for (const check of checks) for (const language of languages) {
    const sample = await measure(artifactFor(manifest, check.name, language), check.args, join(output, `${check.name}-${check.id}-${language}`), timeout);
    if (check.invalid) {
      if (sample.interruption || sample.signal || sample.exit_code <= 0 || sample.exit_code === 127 || sample.sampler_exit === 125) throw new Error(`${check.name}/${language} did not reject malformed input correctly`);
    } else {
      requireSuccess(sample, `${check.name}/${language}`);
      validateOutput(sample.stdout, check.expected, check.tolerance);
    }
    results.push({ case: check.name, input: check.id, language, expected: check.expected,
      invalid: Boolean(check.invalid), exit_code: sample.exit_code, stdout: sample.stdout, stderr: sample.stderr });
    count++;
  }
  writeJson(join(output, 'verified.json'), { checks: count, cases: selected, languages, results, completed_at: new Date().toISOString() });
  console.log(`verified ${count} executions across ${selected.join(', ')}`);
}

/** Require bounded integer command-line options. */
function positiveOption(text, name, minimum, maximum) {
  if (!/^\d+$/.test(text ?? '')) throw new Error(`invalid ${name}`);
  const value = Number(text);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) throw new Error(`invalid ${name}`);
  return value;
}

/** Parse a small explicit option surface, rejecting typos and duplicates. */
function options(args) {
  const out = { profile: 'standard', rounds: 7, warmup: 2, timeout: 120, cases: [...cases] };
  const seen = new Set();
  for (let i = 0; i < args.length; i += 2) {
    const key = args[i], value = args[i + 1];
    if (seen.has(key) || value === undefined) throw new Error(`invalid or repeated option: ${key}`);
    seen.add(key);
    if (key === '--profile' && Object.hasOwn(profiles, value)) out.profile = value;
    else if (key === '--case' && cases.includes(value)) out.cases = [value];
    else if (key === '--rounds') out.rounds = positiveOption(value, key, 1, 1000);
    else if (key === '--warmup') out.warmup = positiveOption(value, key, 0, 100);
    else if (key === '--timeout') out.timeout = positiveOption(value, key, 1, 86400);
    else throw new Error(`unknown option or value: ${key} ${value}`);
  }
  return out;
}

/** Run the complete matrix serially and preserve raw samples before summarizing. */
async function run(config) {
  const manifest = loadBuild();
  const id = new Date().toISOString().replace(/[:.]/g, '-');
  const directory = join(work, 'runs', id);
  mkdirSync(directory, { recursive: false });
  writeJson(join(directory, 'build.json'), manifest);
  const runRecord = { schema: 1, id, started_at: new Date().toISOString(), config, host: hostMetadata(), inputs: {} };
  writeJson(join(directory, 'run.json'), runRecord);
  console.log(`run: ${directory}`);
  await verify(manifest, join(directory, 'verification'), config.cases, config.timeout);
  for (const name of config.cases) {
    const size = profiles[config.profile][name];
    const input = definitions[name].prepare(size, directory);
    const args = input.args;
    runRecord.inputs[name] = { ...input, size, args };
    writeJson(join(directory, 'run.json'), runRecord);
    for (let round = 0; round < config.warmup + config.rounds; round++) {
      const phase = round < config.warmup ? 'warmup' : 'measure';
      for (const [position, language] of languageOrder(round).entries()) {
        const sample = await measure(artifactFor(manifest, name, language), args, join(directory, 'samples', `${name}-${round}-${language}`), config.timeout);
        requireSuccess(sample, `${name}/${language}`);
        validateOutput(sample.stdout, input.expected, input.tolerance);
        const record = { case: name, language, phase, round, position, wall_ns: sample.wall_ns, peak_rss_bytes: sample.peak_rss_bytes, stdout: sample.stdout, stderr: sample.stderr,
          files: Object.fromEntries(Object.entries(sample.files).map(([key, path]) => [key, relative(directory, path)])) };
        appendFileSync(join(directory, 'samples.jsonl'), `${JSON.stringify(record)}\n`);
        console.log(`${name} ${phase} ${round + 1}/${config.warmup + config.rounds} ${language}: ${(sample.wall_ns / 1e9).toFixed(3)} s`);
      }
    }
  }
  // Recheck identities after all measurements so mid-run source changes invalidate the run.
  if (JSON.stringify(loadBuild()) !== JSON.stringify(manifest)) throw new Error('build changed during measurement; discard this run');
  for (const input of Object.values(runRecord.inputs)) {
    if (input.sha256 && hashFile(input.args[0]) !== input.sha256) throw new Error('input changed during measurement; discard this run');
  }
  runRecord.completed_at = new Date().toISOString();
  runRecord.final_load_average = os.loadavg();
  writeJson(join(directory, 'run.json'), runRecord);
  report(directory);
}

/** Validate and summarize a completed run without requiring installed compilers. */
export function report(directory) {
  directory = resolve(directory);
  const runRecord = readJson(join(directory, 'run.json'));
  const manifest = readJson(join(directory, 'build.json'));
  if (!runRecord.completed_at) throw new Error('run is incomplete; no final comparison report is available');
  const samples = readFileSync(join(directory, 'samples.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
  if (samples.length !== runRecord.config.cases.length * languages.length * (runRecord.config.warmup + runRecord.config.rounds)) {
    throw new Error('incomplete or unexpected sample matrix');
  }
  const rows = [];
  const headings = ['| Case | 语言 | 中位数 ms | P25–P75 ms | min–max ms | 吞吐量/s | RSS 中位数 MiB | RSS 最大 MiB | 相对 C++ 用时 |',
    '|---|---|---:|---:|---:|---:|---:|---:|---:|'];
  for (const name of runRecord.config.cases) {
    const expectedCount = runRecord.config.warmup + runRecord.config.rounds;
    const caseSamples = samples.filter(sample => sample.case === name);
    if (caseSamples.length !== languages.length * expectedCount) throw new Error(`incomplete sample matrix for ${name}`);
    const medians = {};
    for (const language of languages) {
      const all = caseSamples.filter(sample => sample.language === language);
      if (all.length !== expectedCount || new Set(all.map(sample => sample.round)).size !== expectedCount) throw new Error(`incomplete/duplicate samples: ${name}/${language}`);
      for (const sample of all) {
        if (!Number.isInteger(sample.round) || sample.round < 0 || sample.round >= expectedCount ||
            sample.phase !== (sample.round < runRecord.config.warmup ? 'warmup' : 'measure') ||
            languageOrder(sample.round)[sample.position] !== sample.language || !Number.isSafeInteger(sample.wall_ns) || sample.wall_ns <= 0 ||
            !Number.isSafeInteger(sample.peak_rss_bytes) || sample.peak_rss_bytes <= 0) throw new Error('invalid sample metadata');
        validateOutput(sample.stdout, runRecord.inputs[name].expected, runRecord.inputs[name].tolerance);
      }
      const selected = all.filter(sample => sample.phase === 'measure');
      medians[language] = quantile(selected.map(sample => sample.wall_ns / 1e6), 0.5);
    }
    for (const language of languages) {
      const selected = caseSamples.filter(sample => sample.language === language && sample.phase === 'measure');
      const times = selected.map(sample => sample.wall_ns / 1e6), memories = selected.map(sample => sample.peak_rss_bytes / 1024 ** 2);
      const row = { case: name, language, median_ms: medians[language], p25_ms: quantile(times, 0.25), p75_ms: quantile(times, 0.75),
        min_ms: Math.min(...times), max_ms: Math.max(...times), throughput: runRecord.inputs[name].units * 1000 / medians[language],
        unit: runRecord.inputs[name].unit, median_rss_mib: quantile(memories, 0.5), max_rss_mib: Math.max(...memories),
        relative_cpp: medians[language] / medians.cpp, short_run: medians[language] < 100 };
      rows.push(row);
      headings.push(`| ${name} | ${language} | ${row.median_ms.toFixed(3)} | ${row.p25_ms.toFixed(3)}–${row.p75_ms.toFixed(3)} | ${row.min_ms.toFixed(3)}–${row.max_ms.toFixed(3)} | ${row.throughput.toFixed(0)} ${row.unit} | ${row.median_rss_mib.toFixed(2)} | ${row.max_rss_mib.toFixed(2)} | ${row.relative_cpp.toFixed(3)}× |`);
    }
  }
  const text = ['# 跨语言黑盒性能评测结果', '', `时间：${runRecord.started_at}；配置：${runRecord.config.profile}；预热 ${runRecord.config.warmup} 轮，正式 ${runRecord.config.rounds} 轮。`,
    `主机：${runRecord.host.cpu}，${runRecord.host.platform}/${runRecord.host.arch}，${runRecord.host.version}。`, '',
    '整程序启动到退出计时；输入读取和输出包含在内；单业务线程、正常运行时、预热文件缓存。相对值大于 1 表示比 C++ 用时更多。', '',
    ...headings, '', '工具链：', '', ...languages.map(language => `- ${language}: ${manifest.tools[language].version.split('\n')[0]}`), '',
    '小于 100 ms 的项目更容易受启动开销和调度噪声影响：' + (rows.filter(row => row.short_run).map(row => `${row.case}/${row.language}`).join('、') || '无') + '。',
    'RSS 是每个新进程的峰值，包含运行时与加载依赖。结果仅代表本次环境和工作负载，不单独归因于 ARC、GC 或某个编译器内部机制。',
    '原始数据见 samples.jsonl，参数和输入校验值见 run.json，构建参数与哈希见 build.json。', ''].join('\n');
  writeJson(join(directory, 'summary.json'), rows);
  writeFileSync(join(directory, 'report.md'), text);
  console.log(`report: ${join(directory, 'report.md')}`);
  return rows;
}

/** Archive completed results and matching source snapshots without copying binaries or caches. */
function archive(directory) {
  directory = resolve(directory);
  report(directory);
  const manifest = readJson(join(directory, 'build.json'));
  for (const source of manifest.sources) if (hashFile(join(root, source.path)) !== source.sha256) throw new Error('sources no longer match this run; archive before editing');
  const destination = join(suite, 'results', basename(directory));
  if (existsSync(destination)) throw new Error(`archive already exists: ${destination}`);
  mkdirSync(destination, { recursive: false });
  for (const file of ['build.json', 'run.json', 'samples.jsonl', 'summary.json', 'report.md', 'verification/verified.json']) {
    const target = join(destination, file);
    mkdirSync(dirname(target), { recursive: true });
    copyFileSync(join(directory, file), target);
  }
  for (const source of manifest.sources) {
    const target = join(destination, 'sources', source.path); mkdirSync(dirname(target), { recursive: true }); copyFileSync(join(root, source.path), target);
  }
  console.log(`archived: ${destination}`);
}

/** Dispatch explicit independent benchmark commands. */
async function main() {
  const [command, ...args] = process.argv.slice(2);
  if (command === 'build' && !args.length) build();
  else if (command === 'verify' && !args.length) { prepareWork(); await verify(loadBuild(), join(work, 'verification')); }
  else if (command === 'run') await run(options(args));
  else if (command === 'report' && args.length === 1) report(args[0]);
  else if (command === 'archive' && args.length === 1) archive(args[0]);
  else throw new Error('usage: node benchmarks/scripts/bench.mjs build|verify|run [--profile quick|standard] [--rounds N] [--warmup N] [--case NAME] [--timeout SECONDS]|report DIR|archive DIR');
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => { console.error(error.stack ?? error.message); process.exitCode = 1; });
}
