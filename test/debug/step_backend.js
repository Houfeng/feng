#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
let buffer = Buffer.alloc(0);
let seq = 1;
let scenario;
let launch;
let heldQuery;
let queries = 0;
let resumed = [];
let continuedByClient = false;

// Serialize complete DAP messages, including events preceding their responses.
function send(message) {
    const payload = JSON.stringify({ seq: seq++, ...message });
    process.stdout.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
}

// Acknowledge the exact request so leaked internal replies remain detectable.
function respond(request, body = {}, success = true) {
    send({ type: 'response', command: request.command, request_seq: request.seq, success, body });
}

// Give each stop a stable identity independent of stack presentation.
function stop(reason = 'step', description = 'hidden', threadId = 1) {
    send({ type: 'event', event: 'stopped', body: { reason, description, threadId, allThreadsStopped: true } });
}

// Preserve genuine process status and both lifecycle events through the proxy.
function finish() {
    send({ type: 'event', event: 'exited', body: { exitCode: 7 } });
    send({ type: 'event', event: 'terminated', body: {} });
}

// Use arbitrary symbols: the adapter must decide from metadata, not dyld/main.
function frame(name, path = '/native/platform-entry', line = 17) {
    return { id: 10, name, source: { path }, line, column: 9 };
}

// Return a complete, truncated, invalid or failed native stack as requested.
function stack(request) {
    const hidden = frame('entry_thunk');
    const visible = frame('user_function', 'step_test://main.ff');
    let frames = [hidden, frame('arbitrary_platform_loader')];
    if (['caller', 'stepout_failure', 'hidden_chain', 'stepout_event_first'].includes(scenario)) {
        frames = [hidden, visible];
    } else if (scenario === 'mapped_caller') {
        frames = [hidden, frame('unrecorded_generic', '/cache/step_test:/main.ff')];
    } else if (scenario === 'visible_top') {
        frames = [visible];
    } else if (scenario === 'native_top') {
        frames = [frame('unrecorded_native')];
    } else if (scenario === 'empty') {
        frames = [];
    }
    if (resumed.includes('stepOut') &&
        (scenario !== 'hidden_chain' || resumed.length === 2)) frames = [visible];
    if (scenario === 'invalid_stack') return respond(request, { stackFrames: {} });
    if (scenario === 'invalid_total') return respond(request, { stackFrames: frames, totalFrames: 'unknown' });
    if (scenario === 'missing_total') return respond(request, { stackFrames: frames });
    respond(request, { stackFrames: frames, totalFrames: frames.length + (scenario === 'truncated' ? 1 : 0) },
        scenario !== 'stack_failure');
}

// Confirm both the action and query count, catching unexpected auto-resumes.
function verify() {
    const bypass = ['breakpoint', 'exception', 'pause', 'other_thread', 'instruction', 'client_step_failure'];
    const callers = ['caller', 'mapped_caller', 'stepout_event_first'];
    const exits = ['exit', 'event_before_response', 'interrupted_continue', 'single_thread_false', 'late_failure', 'missing_total', 'step_event_first'];
    let expectedQueries = bypass.includes(scenario) ? 0 : callers.includes(scenario) ? 2 : 1;
    let expectedActions = callers.includes(scenario) ? ['stepOut'] : exits.includes(scenario) ? ['continue'] : [];
    if (scenario === 'hidden_chain') {
        expectedQueries = 3;
        expectedActions = ['stepOut', 'stepOut'];
    }
    if (scenario === 'continue_failure') expectedActions = ['continue'];
    if (scenario === 'stepout_failure') expectedActions = ['stepOut'];
    assert.equal(queries, expectedQueries, scenario);
    assert.deepEqual(resumed, expectedActions, scenario);
    assert.equal(heldQuery, undefined);
}

// Model source stepping without a real inferior so failure and cancellation
// ordering are deterministic and do not require platform-specific symbols.
function handle(request) {
    const internal = request.seq >= 1000000;
    if (request.command === 'initialize') return respond(request, { supportsConfigurationDoneRequest: true });
    if (request.command === 'launch') {
        scenario = request.arguments.stepScenario;
        launch = request;
        return send({ type: 'event', event: 'initialized', body: {} });
    }
    if (request.command === 'configurationDone') {
        respond(request);
        respond(launch);
        return stop('breakpoint', 'initial');
    }
    if (request.command === 'stackTrace') {
        assert.equal(internal, true);
        assert.equal(request.arguments.threadId, 1);
        assert.equal(request.arguments.startFrame, 0);
        assert.equal(request.arguments.levels, 0);
        ++queries;
        if (scenario === 'late_client_failure') respond(launch.stepRequest, {}, false);
        if (['pause_cancels_query', 'continue_cancels_query'].includes(scenario)) {
            heldQuery = request;
            return send({ type: 'event', event: 'output', body: { output: 'query pending' } });
        }
        if (scenario === 'breakpoint_cancels_query') stop('breakpoint', 'interrupt');
        if (scenario === 'query_continued') {
            send({ type: 'event', event: 'continued', body: { threadId: 1, allThreadsContinued: true } });
        }
        stack(request);
        if (scenario === 'query_continued') finish();
        return;
    }
    if (internal) {
        assert.ok(['stepOut', 'continue'].includes(request.command));
        assert.equal(request.arguments.threadId, 1);
        assert.equal(request.arguments.singleThread, scenario !== 'single_thread_false');
        resumed.push(request.command);
        if (['continue_failure', 'stepout_failure'].includes(scenario)) return respond(request, {}, false);
        send({ type: 'event', event: 'continued', body: { threadId: 1, allThreadsContinued: true } });
        if (scenario === 'late_failure') {
            stop('breakpoint', 'interrupt');
            return respond(request, {}, false);
        }
        const eventFirst = ['event_before_response', 'stepout_event_first'].includes(scenario);
        if (!eventFirst) respond(request);
        if (request.command === 'stepOut') stop('step', 'returned');
        else finish();
        if (eventFirst) respond(request);
        return;
    }
    if (heldQuery && ['pause', 'continue'].includes(request.command)) {
        respond(request);
        if (request.command === 'pause') stop('pause', 'cancelled');
        else finish();
        stack(heldQuery);
        heldQuery = undefined;
        return;
    }
    if (scenario === 'interrupted_continue' && request.command === 'continue') {
        continuedByClient = true;
        respond(request);
        return stop();
    }
    if (['next', 'stepIn', 'stepOut'].includes(request.command)) {
        if (scenario === 'late_client_failure') {
            launch.stepRequest = request;
            return stop();
        }
        if (scenario === 'step_event_first') {
            stop();
            return respond(request);
        }
        respond(request, {}, scenario !== 'client_step_failure');
        if (['breakpoint', 'exception', 'pause'].includes(scenario)) return stop(scenario);
        if (scenario === 'interrupted_continue') {
            assert.equal(continuedByClient, false);
            return stop('breakpoint', 'interrupt');
        }
        return stop('step', 'hidden', scenario === 'other_thread' ? 2 : 1);
    }
    if (request.command === 'threads') return respond(request, { threads: [{ id: 1, name: 'test' }] });
    if (request.command === 'disconnect') {
        verify();
        respond(request);
        process.stdin.destroy();
        return;
    }
    assert.fail(`unexpected request: ${request.command}`);
}

// Decode framed input incrementally; one read may contain several requests.
process.stdin.on('data', chunk => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
        const split = buffer.indexOf('\r\n\r\n');
        if (split < 0) return;
        const length = Number(/Content-Length: (\d+)/i.exec(buffer.subarray(0, split).toString())[1]);
        const end = split + 4 + length;
        if (buffer.length < end) return;
        const message = JSON.parse(buffer.subarray(split + 4, end).toString());
        buffer = buffer.subarray(end);
        handle(message);
    }
});
