#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
let buffer = Buffer.alloc(0);
let sequence = 1;
let held;
let resumed = false;
const closureType = 'FengClosure__arbitrary__Action *';
const cases = {
    normal: { result: '0x1000', type: closureType, variablesReference: 12 },
    empty: { result: 'nullptr', type: closureType, variablesReference: 0 },
    failed: { result: 'FengClosure__arbitrary__Action', type: closureType, variablesReference: 13 },
    leaf: { result: 'FengClosure__arbitrary__Action', type: 'const struct FengClosure__arbitrary__Action *', variablesReference: 0 },
    indirect: { result: 'unavailable', type: 'FengClosure__arbitrary__Action **', variablesReference: 14 },
    view: { result: 'FengSpecValue__arbitrary__View', type: 'FengSpecValue__arbitrary__View', variablesReference: 15 },
    emptyView: { result: 'FengSpecValue__arbitrary__View', type: 'struct FengSpecValue__arbitrary__View', variablesReference: 0 },
    unionValue: { result: '{ tag = 1 }', type: 'FengSpecValue__arbitrary__Union', variablesReference: 16 }
};

// Write complete frames so the protocol assertions observe actual proxy output.
function send(message) {
    const payload = JSON.stringify({ seq: sequence++, ...message });
    process.stdout.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
}

// Preserve client and internal request identities across reordered replies.
function respond(request, body = {}, success = true) {
    send({ type: 'response', command: request.command, request_seq: request.seq, success, body });
}

// Model addresses independently of the closure object pointer and backend summary.
function handle(request) {
    const args = request.arguments || {};
    switch (request.command) {
    case 'initialize': return respond(request);
    case 'launch': return respond(request);
    case 'stackTrace':
        return respond(request, { stackFrames: [{ id: 7, name: 'backend_main',
            source: { path: 'callable_test://main.ff' }, line: 2, column: 1 }], totalFrames: 1 });
    case 'scopes':
        return respond(request, { scopes: [{ name: 'Locals', variablesReference: 101, expensive: false }] });
    case 'variables':
        assert.equal(args.variablesReference, 101);
        return respond(request, { variables: Object.entries(cases).map(([name, value]) => ({
            name, evaluateName: name, value: value.result, type: value.type,
            variablesReference: value.variablesReference, namedVariables: 3, indexedVariables: 2
        })) });
    case 'evaluate': {
        const expression = args.expression;
        if (expression === '(normal)') {
            held = request;
            return send({ type: 'event', event: 'output', body: { output: 'evaluate pending' } });
        }
        if (cases[expression]) return respond(request, { ...cases[expression], namedVariables: 3, indexedVariables: 2 });
        assert.equal(resumed, false, 'must not read an entry after resume');
        const shape = /^\(unsigned int\)\(sizeof\(\((\w+)\)\.subject\) \+ sizeof\(\(\1\)\.witness\)\)$/.exec(expression);
        if (shape) return respond(request, { result: '16', type: 'unsigned int', variablesReference: 0 }, shape[1] !== 'unionValue');
        const subject = /^\(uintptr_t\)\(\((\w+)\)\.subject\)$/.exec(expression);
        if (subject) return respond(request, { result: subject[1] === 'emptyView' ? '0' : '12288', type: 'uintptr_t', variablesReference: 0 });
        const match = /^\(uintptr_t\)\(\((\w+)\) \? \(\1\)->(_self|invoke) : 0\)$/.exec(expression);
        assert.ok(match, `unexpected expression: ${expression}`);
        if (match[1] === 'failed') return respond(request, {}, false);
        const address = match[1] === 'empty' ? '0' : match[2] === '_self' ? '12288' : '8192';
        return respond(request, { result: address, type: 'uintptr_t', variablesReference: 0 });
    }
    case 'continue':
        resumed = true;
        respond(request, { allThreadsContinued: true });
        assert.ok(held);
        return respond(held, { ...cases.normal, namedVariables: 3, indexedVariables: 2 });
    case 'disconnect':
        respond(request);
        return process.exit(0);
    default: throw new Error(`unexpected request: ${request.command}`);
    }
}

// Decode incrementally because pipe writes need not align with DAP frames.
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
