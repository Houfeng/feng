// Report protocol-lowered operation counts and actual native stack/code sizes.
// The lowered .bc snapshot precedes the host optimizer: it is not a claim about
// dynamic operation counts or the remaining operations after O2/O3 inlining.
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
if (process.argv.length < 4) throw new Error('usage: node structure.js LLVM_ROOT BUILD_DIR...');
const llvm = process.argv[2];
const operations = ['feng_retain', 'feng_release', 'feng_aggregate_retain',
    'feng_aggregate_release', 'feng_cleanup_push', 'feng_cleanup_pop'];

// Enumerate only real files so compiler/toolchain symlinks cannot escape a run.
function files(directory) {
    return fs.readdirSync(directory, {withFileTypes:true}).flatMap(entry => {
        const file = path.join(directory, entry.name);
        return entry.isDirectory() ? files(file) : entry.isFile() ? [file] : [];
    });
}

console.log(['build','optimization','layout','unit','function','native_stack_bytes', ...operations].join('\t'));
for (const directory of process.argv.slice(3)) {
    for (const level of [0, 2, 3]) for (const layout of ['single', 'split']) {
        const root = path.join(directory, `O${level}-${layout}`);
        const entries = files(root);
        const stacks = new Map();
        for (const file of entries.filter(f => f.endsWith('.su'))) {
            for (const row of fs.readFileSync(file, 'utf8').trim().split('\n')) {
                const [location, bytes] = row.split('\t');
                stacks.set(location.slice(location.lastIndexOf(':') + 1), bytes);
            }
        }
        for (const file of entries.filter(f => f.endsWith('.lowered.ll'))) {
            const unit = file.includes('/provider/') ? 'provider' : 'consumer';
            const ir = fs.readFileSync(file, 'utf8');
            for (const match of ir.matchAll(/^define [^\n]*@(feng__arc__(?:bench|measure)__[^\s(]+|main)[^\n]*\{\n([\s\S]*?)^\}/gm)) {
                const [, name, body] = match;
                const counts = operations.map(op => [...body.matchAll(new RegExp(`@${op}\\(`, 'g'))].length);
                console.log([path.basename(directory), `O${level}`, layout, unit, name,
                    stacks.get(name) ?? 'inlined_or_absent', ...counts].join('\t'));
            }
        }
        const binary = path.join(root, 'consumer/bin/arc_bench');
        const sizes = cp.execFileSync(path.join(llvm, 'bin/llvm-size'), ['--format=sysv', binary], {encoding:'utf8'});
        fs.writeFileSync(path.join(root, 'native-size.txt'), sizes);
    }
}
