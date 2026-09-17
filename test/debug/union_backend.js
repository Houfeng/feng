#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
let buffer = Buffer.alloc(0);
let sequence = 1;
let held;
let resumed = false;
const cases = {
    choice: { result: 'FengSpecValue__backend__Choice', type: 'FengSpecValue__backend__Choice', variablesReference: 16 },
    generic: { result: '{ tag = 0 }', type: 'const struct FengSpecValue__backend__Result__G__i64', variablesReference: 17 },
    leaf: { result: '{}', type: 'FengSpecValue__backend__Choice', variablesReference: 0 },
    foreign: { result: '{}', type: 'ForeignStruct', variablesReference: 18 }
};

// Emit real framed DAP messages so filters are checked at the protocol boundary.
function send(message) {
    const payload = JSON.stringify({ seq: sequence++, ...message });
    process.stdout.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
}

// Keep the identity of each request, including delayed native expansion replies.
function respond(request, body = {}, success = true) {
    send({ type: 'response', command: request.command, request_seq: request.seq, success, body });
}

// Supply native children with the hidden field between the two visible fields.
function children(reference) {
    if (reference === 41) return [
        { name: 'm0', value: '42', type: 'int64_t', variablesReference: 0 },
        { name: 'm1', value: '0x1000', type: 'FengString *', variablesReference: 42 }
    ];
    return [
        { name: 'tag', value: '0', type: 'uint32_t', variablesReference: 0 },
        { name: '_fwd', value: 'FengManagedSlotDescriptor', type: 'FengManagedSlotDescriptor', variablesReference: 40 },
        { name: 'payload', value: '{ m0 = 42 }', type: 'union payload', variablesReference: 41, namedVariables: 2 }
    ];
}

// Model native expansion, source values, paging and a resume with an outstanding request.
function handle(request) {
    const args = request.arguments || {};
    switch (request.command) {
    case 'initialize':
    case 'launch': return respond(request);
    case 'stackTrace':
        return respond(request, { stackFrames: [{ id: 7, name: 'backend_main',
            source: { path: 'union_test://main.ff' }, line: 2, column: 1 }], totalFrames: 1 });
    case 'scopes':
        return respond(request, { scopes: [{ name: 'Locals', variablesReference: 101, expensive: false }] });
    case 'variables': {
        if (args.variablesReference === 101) return respond(request, {
            variables: Object.entries(cases).map(([name, value]) => ({ name, value: value.result,
                type: value.type, variablesReference: value.variablesReference, namedVariables: 3, indexedVariables: 0 }))
        });
        if (resumed) {
            // Reusing a backend id for an unrelated value must not retain the old filter.
            assert.equal(args.variablesReference, 16);
            return respond(request, { variables: children(16) });
        }
        assert.ok([16, 17, 18, 41].includes(args.variablesReference));
        if (args.variablesReference === 16 || args.variablesReference === 17) {
            assert.ok(!args.start && !args.count, 'the backend must see the full carrier before paging');
        }
        if (args.format?.hex) {
            held = request;
            return send({ type: 'event', event: 'output', body: { output: 'expansion pending' } });
        }
        const values = args.filter === 'indexed' ? [] : children(args.variablesReference);
        const start = args.start || 0;
        return respond(request, { variables: values.slice(start, args.count ? start + args.count : undefined) });
    }
    case 'evaluate':
        assert.equal(resumed, false);
        if (cases[args.expression]) return respond(request, { ...cases[args.expression], namedVariables: 3, indexedVariables: 0 });
        assert.match(args.expression, /^\(unsigned int\)\(sizeof\(\((choice|generic|leaf)\)\.subject\) \+ sizeof\(\(\1\)\.witness\)\)$/);
        return respond(request, {}, false);
    case 'continue':
        resumed = true;
        respond(request, { allThreadsContinued: true });
        assert.ok(held);
        return respond(held, { variables: children(16) });
    case 'disconnect':
        respond(request);
        return process.exit(0);
    default: throw new Error(`unexpected request: ${request.command}`);
    }
}

// Decode incrementally because pipe chunks can split or combine protocol frames.
process.stdin.on('data', chunk => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
        const separator = buffer.indexOf('\r\n\r\n');
        if (separator < 0) return;
        const match = /Content-Length: (\d+)/i.exec(buffer.subarray(0, separator).toString());
        assert.ok(match);
        const end = separator + 4 + Number(match[1]);
        if (buffer.length < end) return;
        const request = JSON.parse(buffer.subarray(separator + 4, end).toString());
        buffer = buffer.subarray(end);
        handle(request);
    }
});
