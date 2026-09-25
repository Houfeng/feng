// Report medians from validated samples; timing is never a shared-CI assertion.
const fs = require('node:fs');
if (process.argv.length !== 3) throw new Error('usage: node report.js RESULTS_TSV');
const groups = new Map();
for (const line of fs.readFileSync(process.argv[2], 'utf8').trim().split('\n').slice(1)) {
    const [round, build, optimization, layout, mode, count, ns, sum, peak, drops, defaults] = line.split('\t');
    if (!round || !build || !sum || BigInt(drops) !== BigInt(count) || peak !== '1' ||
        BigInt(defaults) !== (mode === '5' ? BigInt(count) : 0n))
        throw new Error(`invalid sample: ${line}`);
    const key = [build, optimization, layout, mode].join('\t');
    const values = groups.get(key) || [];
    values.push(Number(ns) / Number(count)); groups.set(key, values);
}
console.log('build\toptimization\tlayout\tmode\tmedian_ns_per_iteration\tsamples');
for (const [key, values] of groups) {
    values.sort((a, b) => a - b);
    const middle = Math.floor(values.length / 2);
    const median = values.length % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
    console.log(`${key}\t${median.toFixed(3)}\t${values.length}`);
}
