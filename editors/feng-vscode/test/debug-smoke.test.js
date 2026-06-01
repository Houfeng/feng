const assert = require('assert')
const Module = require('module')
const cp = require('child_process')
const fs = require('fs')
const os = require('os')
const path = require('path')

function createDisposable() {
    return {
        dispose() {
        }
    }
}

function createWorkspaceFolder(fsPath) {
    return {
        uri: {
            fsPath
        }
    }
}

function createActiveEditor(documentPath, languageId = 'feng') {
    return {
        document: {
            languageId,
            uri: {
                scheme: 'file',
                fsPath: documentPath
            }
        }
    }
}

function createMockVscode(options = {}) {
    const workspaceFolders = Array.isArray(options.workspaceFolders)
        ? options.workspaceFolders
        : (options.workspaceRoot == null ? [] : [createWorkspaceFolder(options.workspaceRoot)])
    const executablePath = options.executablePath

    return {
        DebugAdapterExecutable: class DebugAdapterExecutable {
            constructor(command, args, optionsValue) {
                this.command = command
                this.args = args
                this.options = optionsValue
            }
        },
        DebugConfigurationProviderTriggerKind: {
            Dynamic: 2
        },
        ProcessExecution: class ProcessExecution {
            constructor(process, args, optionsValue) {
                this.process = process
                this.args = args
                this.options = optionsValue
            }
        },
        Task: class Task {
            constructor(definition, scope, name, source, execution, problemMatchers) {
                this.definition = definition
                this.scope = scope
                this.name = name
                this.source = source
                this.execution = execution
                this.problemMatchers = problemMatchers
                this.group = undefined
                this.detail = undefined
            }

            get label() {
                return `${this.source}: ${this.name}`
            }
        },
        TaskGroup: {
            Build: { kind: 'build' }
        },
        TaskScope: {
            Workspace: 1
        },
        debug: {
            registerDebugAdapterDescriptorFactory() {
                return createDisposable()
            },
            registerDebugConfigurationProvider() {
                return createDisposable()
            }
        },
        tasks: {
            registerTaskProvider() {
                return createDisposable()
            }
        },
        window: {
            activeTextEditor: options.activeTextEditor || null,
            showErrorMessage() {
                return Promise.resolve(undefined)
            }
        },
        workspace: {
            workspaceFolders,
            getConfiguration() {
                return {
                    get(_key, defaultValue) {
                        return executablePath !== undefined ? executablePath : defaultValue
                    },
                    inspect() {
                        return {
                            workspaceFolderValue: executablePath,
                            workspaceValue: undefined,
                            globalValue: undefined
                        }
                    }
                }
            },
            getWorkspaceFolder(uri) {
                const targetPath = uri != null && typeof uri.fsPath === 'string'
                    ? path.resolve(uri.fsPath)
                    : null

                if (targetPath == null) {
                    return null
                }

                for (const workspaceFolder of workspaceFolders) {
                    const workspacePath = path.resolve(workspaceFolder.uri.fsPath)

                    if (targetPath === workspacePath || targetPath.startsWith(workspacePath + path.sep)) {
                        return workspaceFolder
                    }
                }

                return null
            }
        }
    }
}

function loadExtensionModule(mockVscode) {
    const originalLoad = Module._load
    const extensionPath = require.resolve('../extension')

    delete require.cache[extensionPath]
    Module._load = function patchedLoad(request, parent, isMain) {
        if (request === 'vscode') {
            return mockVscode
        }
        return originalLoad.call(this, request, parent, isMain)
    }

    try {
        return require(extensionPath)
    } finally {
        Module._load = originalLoad
    }
}

function buildDapMessage(message) {
    const payload = JSON.stringify(message)

    return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\r\n\r\n${payload}`
}

function createDapClient(child) {
    let buffer = Buffer.alloc(0)
    let stderrText = ''
    let closeInfo = null
    const messages = []
    const pending = []

    function summarizeMessages() {
        return messages.slice(-8).map(message => JSON.stringify(message)).join('\n')
    }

    function failPending(reason) {
        while (pending.length > 0) {
            const waiter = pending.shift()

            clearTimeout(waiter.timeoutId)
            waiter.reject(reason)
        }
    }

    function dispatch(message) {
        messages.push(message)
        for (let index = 0; index < pending.length; index += 1) {
            if (!pending[index].predicate(message)) {
                continue
            }

            const waiter = pending.splice(index, 1)[0]

            clearTimeout(waiter.timeoutId)
            waiter.resolve(message)
            return
        }
    }

    child.stdout.on('data', chunk => {
        buffer = Buffer.concat([buffer, chunk])
        for (;;) {
            const separator = buffer.indexOf('\r\n\r\n')

            if (separator < 0) {
                break
            }

            const header = buffer.slice(0, separator).toString('utf8')
            const match = /Content-Length: (\d+)/i.exec(header)

            assert(match, `missing Content-Length header in ${header}`)
            const payloadLength = Number(match[1])
            const frameLength = separator + 4 + payloadLength

            if (buffer.length < frameLength) {
                break
            }

            const payload = JSON.parse(buffer.slice(separator + 4, frameLength).toString('utf8'))

            buffer = buffer.slice(frameLength)
            dispatch(payload)
        }
    })

    child.stderr.on('data', chunk => {
        stderrText += chunk.toString('utf8')
    })

    child.on('close', (code, signal) => {
        closeInfo = { code, signal }
        failPending(new Error(`debug adapter closed early (code=${code}, signal=${signal})\nstderr:\n${stderrText}\nmessages:\n${summarizeMessages()}`))
    })

    return {
        send(message) {
            assert(child.stdin.write(buildDapMessage(message)))
        },

        waitFor(predicate, timeoutMs, description) {
            for (const message of messages) {
                if (predicate(message)) {
                    return Promise.resolve(message)
                }
            }

            if (closeInfo != null) {
                return Promise.reject(
                    new Error(`debug adapter already closed while waiting for ${description}\nstderr:\n${stderrText}\nmessages:\n${summarizeMessages()}`)
                )
            }

            return new Promise((resolve, reject) => {
                const waiter = {
                    predicate,
                    resolve,
                    reject,
                    timeoutId: setTimeout(() => {
                        const index = pending.indexOf(waiter)

                        if (index >= 0) {
                            pending.splice(index, 1)
                        }
                        reject(new Error(`timed out waiting for ${description}\nstderr:\n${stderrText}\nmessages:\n${summarizeMessages()}`))
                    }, timeoutMs)
                }

                pending.push(waiter)
            })
        },

        getStderr() {
            return stderrText
        },

        async close() {
            if (closeInfo != null) {
                return
            }

            child.kill('SIGTERM')
            await new Promise(resolve => {
                child.once('close', () => resolve())
                setTimeout(resolve, 5000)
            })
        }
    }
}

function resolveLldbDapPath() {
    if (process.platform !== 'darwin') {
        return null
    }

    try {
        const resolved = cp.execFileSync('xcrun', ['-f', 'lldb-dap'], { encoding: 'utf8' }).trim()

        return resolved.length > 0 ? resolved : null
    } catch (_) {
        return null
    }
}

function realpathIfExists(filePath) {
    try {
        return fs.realpathSync(filePath)
    } catch (_) {
        return path.resolve(filePath)
    }
}

async function run() {
    const lldbDapPath = resolveLldbDapPath()
    const repoRoot = path.resolve(__dirname, '..', '..', '..')
    const fengBinary = path.join(repoRoot, 'build', 'bin', 'feng')

    if (lldbDapPath == null) {
        console.log('debug smoke test skipped: lldb-dap is unavailable')
        return
    }

    cp.execFileSync('make', ['cli', 'runtime'], {
        cwd: repoRoot,
        stdio: 'inherit'
    })

    const tempRoot = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'feng-vscode-debug-smoke-')))
    const projectRoot = path.join(tempRoot, 'hello_world')
    const sourceDir = path.join(projectRoot, 'src')
    const sourcePath = path.join(sourceDir, 'main.ff')
    const manifestPath = path.join(projectRoot, 'feng.fm')
    const stdPath = path.join(repoRoot, 'std')
    const stdRelativePath = path.relative(projectRoot, stdPath).split(path.sep).join('/')
    const wrapperDir = path.join(tempRoot, 'tool-bin')
    const wrapperPath = path.join(wrapperDir, 'lldb-dap')
    const workspaceFolder = createWorkspaceFolder(tempRoot)
    const mockVscode = createMockVscode({
        workspaceRoot: tempRoot,
        activeTextEditor: createActiveEditor(sourcePath),
        executablePath: fengBinary
    })
    const extension = loadExtensionModule(mockVscode)
    const provider = extension.__test__.createFengDebugConfigurationProvider(mockVscode)
    let child = null
    let client = null

    fs.mkdirSync(sourceDir, { recursive: true })
    fs.writeFileSync(manifestPath,
                     '[package]\n' +
                     'name: "hello_world"\n' +
                     'version: "0.1.0"\n' +
                     'target: "bin"\n' +
                     'src: "src/"\n' +
                     'out: "build/"\n\n' +
                     '[dependencies]\n' +
                     `std: "${stdRelativePath}"\n`)
    fs.writeFileSync(sourcePath,
                     'module hello_world;\n\n' +
                     'import std.io;\n\n' +
                     'func main(args: string[]) {\n' +
                     '  for var i = 1; i <= 1000; i += 1 {\n' +
                     '    println("Hello, world!");\n' +
                     '  }\n' +
                     '}\n')

    cp.execFileSync(fengBinary, ['build', projectRoot], {
        cwd: repoRoot,
        stdio: 'inherit'
    })

    const resolvedConfiguration = await provider.resolveDebugConfiguration(workspaceFolder, {
        type: 'feng',
        request: 'launch'
    })
    const adapter = extension.__test__.createDebugAdapterExecutable(workspaceFolder, mockVscode)

    assert.deepStrictEqual(resolvedConfiguration, {
        type: 'feng',
        request: 'launch',
        name: 'Debug hello_world',
        program: path.join(projectRoot, 'build', 'bin', 'hello_world'),
        cwd: projectRoot,
        preLaunchTask: 'feng: build hello_world'
    })
    assert.strictEqual(adapter.command, fengBinary)
    assert.deepStrictEqual(adapter.args, ['dap', '--stdio'])
    assert.deepStrictEqual(adapter.options, { cwd: tempRoot })
    assert(fs.existsSync(resolvedConfiguration.program), 'expected built program to exist')
    assert(fs.existsSync(resolvedConfiguration.program + '.fd'), 'expected debug sidecar to exist')

    fs.mkdirSync(wrapperDir, { recursive: true })
    fs.writeFileSync(wrapperPath, `#!/bin/sh\nexec ${JSON.stringify(lldbDapPath)} "$@"\n`)
    fs.chmodSync(wrapperPath, 0o775)

    try {
        let nextSeq = 1

        child = cp.spawn(adapter.command,
                         adapter.args,
                         {
                             cwd: adapter.options != null ? adapter.options.cwd : undefined,
                             env: {
                                 ...process.env,
                                 PATH: `${wrapperDir}${path.delimiter}${process.env.PATH || ''}`
                             },
                             stdio: ['pipe', 'pipe', 'pipe']
                         })
        client = createDapClient(child)

        client.send({
            seq: nextSeq++,
            type: 'request',
            command: 'initialize',
            arguments: {
                adapterID: 'feng',
                linesStartAt1: true,
                columnsStartAt1: true,
                pathFormat: 'path'
            }
        })
        await client.waitFor(message => message.type === 'response' &&
            message.command === 'initialize' &&
            message.success === true,
        30000,
        'initialize response')

        client.send({
            seq: nextSeq++,
            type: 'request',
            command: 'launch',
            arguments: {
                program: resolvedConfiguration.program,
                cwd: resolvedConfiguration.cwd
            }
        })
        await client.waitFor(message => message.type === 'event' && message.event === 'initialized',
                             30000,
                             'initialized event')

        client.send({
            seq: nextSeq++,
            type: 'request',
            command: 'setBreakpoints',
            arguments: {
                source: {
                    name: path.basename(sourcePath),
                    path: sourcePath
                },
                breakpoints: [
                    {
                        line: 7
                    }
                ]
            }
        })
        const setBreakpointsResponse = await client.waitFor(message => message.type === 'response' &&
            message.command === 'setBreakpoints' &&
            message.success === true,
        30000,
        'setBreakpoints response')

        assert(Array.isArray(setBreakpointsResponse.body.breakpoints))
        assert.strictEqual(setBreakpointsResponse.body.breakpoints.length, 1)
        assert.strictEqual(setBreakpointsResponse.body.breakpoints[0].verified, true)

        client.send({
            seq: nextSeq++,
            type: 'request',
            command: 'configurationDone',
            arguments: {}
        })

        const stoppedEvent = await client.waitFor(message => message.type === 'event' &&
            message.event === 'stopped' &&
            message.body != null &&
            message.body.reason === 'breakpoint',
        60000,
        'breakpoint stop event')
        let threadId = stoppedEvent.body.threadId

        await client.waitFor(message => message.type === 'response' &&
            message.command === 'launch' &&
            message.success === true,
        30000,
        'launch response')

        if (typeof threadId !== 'number') {
            client.send({
                seq: nextSeq++,
                type: 'request',
                command: 'threads',
                arguments: {}
            })
            const threadsResponse = await client.waitFor(message => message.type === 'response' &&
                message.command === 'threads' &&
                message.success === true,
            30000,
            'threads response')

            assert(Array.isArray(threadsResponse.body.threads))
            assert(threadsResponse.body.threads.length > 0)
            threadId = threadsResponse.body.threads[0].id
        }

        client.send({
            seq: nextSeq++,
            type: 'request',
            command: 'stackTrace',
            arguments: {
                threadId
            }
        })
        const stackTraceResponse = await client.waitFor(message => message.type === 'response' &&
            message.command === 'stackTrace' &&
            message.success === true,
        30000,
        'stackTrace response')
        const expectedSourcePath = realpathIfExists(sourcePath)
        const stackFrameSummary = JSON.stringify(stackTraceResponse.body.stackFrames)
        const frame = Array.isArray(stackTraceResponse.body.stackFrames)
            ? stackTraceResponse.body.stackFrames.find(candidate => candidate.source != null &&
                typeof candidate.source.path === 'string' &&
                realpathIfExists(candidate.source.path) === expectedSourcePath)
            : null

        assert(frame, `expected a stack frame for ${expectedSourcePath}: ${stackFrameSummary}`)
        assert.strictEqual(frame.name, 'main')
        assert.strictEqual(frame.source.name, 'main.ff')
        assert(frame.line >= 6 && frame.line <= 7)
        assert.strictEqual(client.getStderr(), '')

        client.send({
            seq: nextSeq++,
            type: 'request',
            command: 'disconnect',
            arguments: {
                terminateDebuggee: true
            }
        })
        await client.waitFor(message => message.type === 'response' &&
            message.command === 'disconnect' &&
            message.success === true,
        30000,
        'disconnect response')
    } finally {
        if (client != null) {
            await client.close()
        }
        fs.rmSync(tempRoot, { recursive: true, force: true })
    }

    console.log('debug smoke tests passed')
}

run().catch(error => {
    console.error(error)
    process.exit(1)
})