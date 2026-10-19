'use strict';

/** An ordinary reference node with two optional children. */
class Node {
  /** Attach the children to this node. */
  constructor(left, right) { this.left = left; this.right = right; }
}

/** Recursively construct all nodes, including leaves. */
function makeTree(depth) {
  return depth === 0 ? new Node(null, null) : new Node(makeTree(depth - 1), makeTree(depth - 1));
}

/** Count nodes by following child references. */
function check(node) {
  return node === null ? 0 : 1 + check(node.left) + check(node.right);
}

/** Construct and inspect one temporary tree. */
function temporary(depth) { return check(makeTree(depth)); }

if (process.argv.length !== 3 || !/^\d+$/.test(process.argv[2])) process.exit(2);
const depth = Number(process.argv[2]);
if (!Number.isInteger(depth) || depth < 6 || depth > 20) process.exit(2);
console.log(temporary(depth + 1));
const longLived = makeTree(depth);
for (let d = 4; d <= depth; d += 2) {
  const iterations = 2 ** (depth - d + 4);
  let sum = 0;
  for (let i = 0; i < iterations; ++i) sum += temporary(d);
  console.log(d, iterations, sum);
}
console.log(check(longLived));
