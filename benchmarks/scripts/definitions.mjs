import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fannkuchReference, nbodyReference, spectralReference, textInput, treeReference, validateOutput } from './common.mjs';

/** Declarative case adapters supply inputs and expected outputs to the generic runner. */
export const definitions = {
  nbody: {
    profiles: { quick: 10000, standard: 20000000 },
    /** Prepare a numerical workload without invoking any measured executable. */
    prepare(steps) { return { args: [String(steps)], ...nbodyReference(steps) }; },
    /** Check the public golden result and cover the zero/one-step boundaries. */
    checks() {
      validateOutput('-0.169075164 -0.169087605', nbodyReference(1000).expected, 1e-8);
      return [0, 1, 1000].map(steps => ({ id: String(steps), ...this.prepare(steps) }));
    },
  },
  'binary-trees': {
    profiles: { quick: 8, standard: 16 },
    /** Derive a tree checksum algebraically, without building any reference tree. */
    prepare(depth) { return { args: [String(depth)], ...treeReference(depth) }; },
    /** Cover the minimum workload, an odd maximum depth and a larger small tree. */
    checks() { return [6, 7, 8].map(depth => ({ id: String(depth), ...this.prepare(depth) })); },
  },
  'text-scan': {
    profiles: { quick: 10000, standard: 32000000 },
    /** Generate an input file shared by all language implementations. */
    prepare(rows, directory) {
      const path = join(directory, `text-${rows}.txt`);
      return { args: [path], ...textInput(path, rows) };
    },
    /** Check generated data, an independent fixture and five malformed record classes. */
    checks(directory) {
      const checks = [0, 1, 1000].map(rows => ({ id: String(rows), ...this.prepare(rows, directory) }));
      const fixture = join(directory, 'independent.txt');
      writeFileSync(fixture, 'E,99999\nI,00000\nW,00127\nE,54321\n');
      checks.push({ id: 'independent', args: [fixture], expected: [4, 1, 2, 154447], tolerance: 0 });
      for (const [id, text] of Object.entries({ partial: 'I,00000', severity: 'X,00000\n', digit: 'I,00x00\n', delimiter: 'I;00000\n', newline: 'I,00000x' })) {
        const path = join(directory, `${id}.txt`);
        writeFileSync(path, text);
        checks.push({ id, args: [path], invalid: true });
      }
      return checks;
    },
  },
  'fannkuch-redux': {
    profiles: { quick: 7, standard: 11 },
    /** Derive the checksum and work count using the recursive reference. */
    prepare(n) { return { args: [String(n)], ...fannkuchReference(n) }; },
    /** Cover minimal, signed-checksum, ordinary and public-golden inputs. */
    checks() {
      validateOutput('228 16', fannkuchReference(7).expected);
      return [1, 2, 3, 7].map(n => ({ id: String(n), ...this.prepare(n) }));
    },
  },
  'spectral-norm': {
    profiles: { quick: 100, standard: 2000 },
    /** Calculate the norm with a stored-matrix reference outside measurement. */
    prepare(n) { return { args: [String(n)], ...spectralReference(n) }; },
    /** Cover minimal matrices and the public rounded checksum. */
    checks() {
      validateOutput('1.274219991', spectralReference(100).expected, 1e-9);
      return [1, 2, 10, 100].map(n => ({ id: String(n), ...this.prepare(n) }));
    },
  },
};

export const cases = Object.keys(definitions);
export const profiles = Object.fromEntries(['quick', 'standard'].map(profile =>
  [profile, Object.fromEntries(cases.map(name => [name, definitions[name].profiles[profile]]))]));
