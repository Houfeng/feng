// Summarize every group; optionally write paired deltas against a named build.
const fs = require('node:fs');
const path = require('node:path');
if (process.argv.length < 3 || process.argv.length > 5 || process.argv.length === 4) {
    throw new Error('usage: node report.js RESULTS_TSV [REFERENCE_BUILD PAIRED_TSV]');
}
const records = fs.readFileSync(process.argv[2], 'utf8').trim().split('\n').slice(1).map(line => {
    const [round, build, level, layout, language, mode, count, ns, checksum] = line.split('\t');
    return {round: +round, build: path.basename(build), level: +level, layout, language,
        mode: +mode, count: +count, ns: +ns, perOperation: +ns / +count, checksum};
});
const groups = new Map();
for (const row of records) {
    const key = [row.build, row.level, row.layout, row.language, row.mode].join('\t');
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(row.perOperation);
}
/* A midpoint median avoids selecting a favorable sample from an even-sized set. */
function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    const half = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[half] : (sorted[half - 1] + sorted[half]) / 2;
}
console.log('build\tlevel\tlayout\tlanguage\tmode\tsamples\tmedian_ns\tmin_ns\tmax_ns');
for (const [key, values] of groups) {
    console.log([key, values.length, median(values).toFixed(3),
        Math.min(...values).toFixed(3), Math.max(...values).toFixed(3)].join('\t'));
}
if (process.argv.length === 5) {
    const reference = process.argv[3];
    const paired = new Map();
    /* Round and workload must match; never subtract unrelated fastest samples. */
    const identity = row => [row.round, row.level, row.layout, row.language, row.mode].join('\t');
    for (const row of records) {
        if (row.build === reference) paired.set(identity(row), row);
    }
    if (!paired.size) throw new Error(`missing reference build: ${reference}`);
    const differences = new Map();
    const samples = ['reference\tcandidate\tlevel\tlayout\tlanguage\tmode\tround\tdelta_ns'];
    for (const row of records) {
        if (row.build === reference) continue;
        const before = paired.get(identity(row));
        if (!before || before.count !== row.count || before.checksum !== row.checksum) {
            throw new Error(`unmatched paired sample: ${identity(row)} ${row.build}`);
        }
        const key = [reference, row.build, row.level, row.layout, row.language, row.mode].join('\t');
        if (!differences.has(key)) differences.set(key, []);
        const delta = row.perOperation - before.perOperation;
        differences.get(key).push(delta);
        samples.push([key, row.round, delta.toFixed(3)].join('\t'));
    }
    fs.writeFileSync(process.argv[4], samples.join('\n') + '\n');
    const summary = ['reference\tcandidate\tlevel\tlayout\tlanguage\tmode\tsamples\tmedian_delta_ns\tmin_delta_ns\tmax_delta_ns'];
    for (const [key, values] of differences) {
        summary.push([key, values.length, median(values).toFixed(3),
            Math.min(...values).toFixed(3), Math.max(...values).toFixed(3)].join('\t'));
    }
    fs.writeFileSync(process.argv[4] + '.summary.tsv', summary.join('\n') + '\n');
}
