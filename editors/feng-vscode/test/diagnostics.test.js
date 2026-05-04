const assert = require('assert');
const Module = require('module');
const fs = require('fs');
const os = require('os');
const path = require('path');

function createDisposable() {
    return {
        dispose() {
        }
    };
}

function createMockVscode(options = {}) {
    const recorder = {
        diagnosticCollections: [],
        formattingProviders: [],
        warningMessages: []
    };
    const workspaceRoot = options.workspaceRoot || null;
    const executablePath = options.executablePath;
    const textDocuments = options.textDocuments || [];
    const mockVscode = {
        Range: class Range {
            constructor(start, end) {
                this.start = start;
                this.end = end;
            }
        },
        Position: class Position {
            constructor(line, character) {
                this.line = line;
                this.character = character;
            }
        },
        TextEdit: {
            replace(range, newText) {
                return { range, newText };
            }
        },
        Diagnostic: class Diagnostic {
            constructor(range, message, severity) {
                this.range = range;
                this.message = message;
                this.severity = severity;
            }
        },
        DiagnosticSeverity: {
            Error: 0,
            Information: 1
        },
        languages: {
            createDiagnosticCollection(name) {
                const collection = createCollectionRecorder();

                collection.name = name;
                recorder.diagnosticCollections.push(collection);
                return collection;
            },
            registerDocumentFormattingEditProvider(selector, provider) {
                recorder.formattingProviders.push({ selector, provider });
                return createDisposable();
            }
        },
        workspace: {
            workspaceFolders: workspaceRoot == null
                ? []
                : [{ uri: { fsPath: workspaceRoot } }],
            textDocuments,
            getConfiguration() {
                return {
                    get(_key, defaultValue) {
                        return executablePath !== undefined ? executablePath : defaultValue;
                    }
                };
            },
            onDidOpenTextDocument() {
                return createDisposable();
            },
            onDidChangeTextDocument() {
                return createDisposable();
            },
            onDidSaveTextDocument() {
                return createDisposable();
            },
            onDidCloseTextDocument() {
                return createDisposable();
            }
        },
        window: {
            showWarningMessage(message) {
                recorder.warningMessages.push(message);
                return Promise.resolve(undefined);
            }
        }
    };

    return { mockVscode, recorder };
}

function createMockLanguageClientModule(options = {}) {
    const recorder = {
        constructorArgs: null,
        startCalls: 0,
        stopCalls: 0
    };

    class MockLanguageClient {
        constructor(id, name, serverOptions, clientOptions) {
            recorder.constructorArgs = { id, name, serverOptions, clientOptions };
            this.initializeResult = options.initializeResult;
        }

        async start() {
            recorder.startCalls += 1;
            if (options.startError) {
                throw options.startError;
            }
        }

        stop() {
            recorder.stopCalls += 1;
            return Promise.resolve();
        }
    }

    return {
        module: {
            LanguageClient: MockLanguageClient
        },
        recorder
    };
}

function loadExtensionModule(options = {}) {
    const originalLoad = Module._load;
    const mockVscode = options.mockVscode || createMockVscode().mockVscode;
    const mockLanguageClientModule = options.mockLanguageClientModule;
    let loaded;

    const extensionPath = require.resolve('../extension');
    delete require.cache[extensionPath];

    Module._load = function patchedLoad(request, parent, isMain) {
        if (request === 'vscode') {
            return mockVscode;
        }
        if (request === 'vscode-languageclient/node' && mockLanguageClientModule) {
            return mockLanguageClientModule;
        }
        return originalLoad.call(this, request, parent, isMain);
    };

    try {
        const extension = require(extensionPath);

        if (options.keepPatchedLoad) {
            loaded = {
                extension,
                restore() {
                    Module._load = originalLoad;
                }
            };
            return loaded;
        }
        loaded = extension;
        return loaded;
    } finally {
        if (!options.keepPatchedLoad) {
            Module._load = originalLoad;
        }
    }
}

function createDocument(pathname, languageId = 'feng') {
    return {
        languageId,
        uri: {
            scheme: 'file',
            fsPath: pathname,
            toString() {
                return `file://${pathname}`;
            }
        }
    };
}

function createCollectionRecorder() {
    const operations = [];

    return {
        operations,
        set(uri, diagnostics) {
            operations.push({ type: 'set', uri, diagnostics });
        },
        delete(uri) {
            operations.push({ type: 'delete', uri });
        }
    };
}

async function run() {
    const extension = loadExtensionModule();
    const {
        buildLspCapabilityWarning,
        buildLspStartupWarning,
        buildCheckCommand,
        createServerOptions,
        createDiagnosticController,
        filterEntriesForPath,
        findProjectManifestPath,
        hasAnyLspCapability,
        isCheckableFengDocument,
        formatDocumentSource,
        resolveExecutablePath
    } = extension.__test__;

    assert.strictEqual(resolveExecutablePath('./build/bin/feng', '/workspace/demo'), path.join('/workspace/demo', './build/bin/feng'));
    assert.strictEqual(resolveExecutablePath('/usr/local/bin/feng', '/workspace/demo'), '/usr/local/bin/feng');
    assert.strictEqual(resolveExecutablePath('feng', '/workspace/demo', false), 'feng');
    assert.deepStrictEqual(createServerOptions('./build/bin/feng', '/workspace/demo'), {
        command: path.join('/workspace/demo', './build/bin/feng'),
        args: ['lsp'],
        options: {
            cwd: '/workspace/demo'
        }
    });
    assert.deepStrictEqual(createServerOptions('feng', '/workspace/demo', false), {
        command: 'feng',
        args: ['lsp'],
        options: {
            cwd: '/workspace/demo'
        }
    });
    assert.strictEqual(hasAnyLspCapability({}), false);
    assert.strictEqual(hasAnyLspCapability({ hoverProvider: true }), true);
    assert.strictEqual(buildLspCapabilityWarning().includes('no language capabilities'), true);
    assert.strictEqual(buildLspStartupWarning(new Error('boom')).includes('boom'), true);
    assert.strictEqual(buildLspStartupWarning('spawn feng ENOENT').includes('spawn feng ENOENT'), true);
    assert.strictEqual(buildLspStartupWarning(null).includes('unknown error'), true);
    assert.strictEqual(buildLspStartupWarning(undefined).includes('unknown error'), true);

    assert.strictEqual(isCheckableFengDocument(createDocument('/tmp/manifest.fm', 'feng-manifest')), false);

    {
        const sourceDocument = {
            languageId: 'feng',
            getText() {
                return 'fn main(args:string[]):void {}\n';
            }
        };
        const manifestDocument = {
            languageId: 'feng-manifest',
            getText() {
                return '[package]\nname:"demo"\nversion:"0.1.0"\n';
            }
        };

        assert.strictEqual(
            formatDocumentSource(sourceDocument, { insertSpaces: true, tabSize: 4 }),
            'fn main(args: string[]): void {}\n'
        );
        assert.strictEqual(
            formatDocumentSource(manifestDocument, { insertSpaces: true, tabSize: 4 }),
            '[package]\nname:    "demo"\nversion: "0.1.0"\n'
        );
    }

    {
        const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'feng-vscode-project-'));
        const projectDir = path.join(tempRoot, 'demo');
        const srcDir = path.join(projectDir, 'src');
        const sourcePath = path.join(srcDir, 'main.ff');
        const standalonePath = path.join(tempRoot, 'single.ff');

        fs.mkdirSync(srcDir, { recursive: true });
        fs.writeFileSync(path.join(projectDir, 'feng.fm'), '[package]\nname: "demo"\nversion: "0.1.0"\n');
        fs.writeFileSync(sourcePath, 'fn main(args: string[]) {}\n');
        fs.writeFileSync(standalonePath, 'fn main(args: string[]) {}\n');

        assert.strictEqual(findProjectManifestPath(sourcePath), path.join(projectDir, 'feng.fm'));
        assert.strictEqual(findProjectManifestPath(standalonePath), null);
        assert.deepStrictEqual(buildCheckCommand(sourcePath), ['check', '--format', 'json', sourcePath]);
        assert.deepStrictEqual(buildCheckCommand(standalonePath), ['tool', 'check', standalonePath]);

        fs.rmSync(tempRoot, { recursive: true, force: true });
    }

    {
        const currentPath = '/tmp/open.ff';
        const filtered = filterEntriesForPath([
            {
                path: currentPath,
                line: 1,
                col: 1,
                end_col: 2,
                severity: 'error',
                message: 'current file error',
                source: 'check'
            },
            {
                path: '/tmp/other.ff',
                line: 1,
                col: 1,
                end_col: 2,
                severity: 'error',
                message: 'other file error',
                source: 'check'
            }
        ], currentPath);

        assert.strictEqual(filtered.length, 1);
        assert.strictEqual(filtered[0].message, 'current file error');
    }

    {
        const document = createDocument('/tmp/open.ff');
        const collection = createCollectionRecorder();
        const controller = createDiagnosticController({
            collection,
            async runCheckEntries(filePath) {
                assert.strictEqual(filePath, '/tmp/open.ff');
                return [{
                    path: '/tmp/open.ff',
                    line: 1,
                    col: 2,
                    end_col: 4,
                    severity: 'error',
                    message: 'unexpected token',
                    source: 'check'
                }, {
                    path: '/tmp/other.ff',
                    line: 2,
                    col: 1,
                    end_col: 3,
                    severity: 'error',
                    message: 'should be filtered out',
                    source: 'check'
                }];
            }
        });

        await controller.checkDocument(document);

        assert.strictEqual(collection.operations.length, 1, 'open should publish diagnostics');
        assert.strictEqual(collection.operations[0].type, 'set');
        assert.strictEqual(collection.operations[0].diagnostics.length, 1);
        assert.strictEqual(collection.operations[0].diagnostics[0].message, 'unexpected token');
    }

    {
        const document = createDocument('/tmp/change.ff');
        const collection = createCollectionRecorder();
        let resolveFirstCheck;
        let invocationCount = 0;
        const controller = createDiagnosticController({
            collection,
            runCheckEntries() {
                invocationCount += 1;

                if (invocationCount === 1) {
                    return new Promise(resolve => {
                        resolveFirstCheck = resolve;
                    });
                }

                return Promise.resolve([{
                    path: '/tmp/change.ff',
                    line: 2,
                    col: 1,
                    end_col: 3,
                    severity: 'error',
                    message: 'save error',
                    source: 'check'
                }]);
            }
        });

        const staleCheck = controller.checkDocument(document);
        controller.clearDocument(document);
        resolveFirstCheck([{
            path: '/tmp/change.ff',
            line: 1,
            col: 1,
            end_col: 2,
            severity: 'error',
            message: 'stale error',
            source: 'check'
        }]);
        await staleCheck;

        assert.deepStrictEqual(
            collection.operations.map(operation => operation.type),
            ['delete'],
            'editing should clear diagnostics and suppress stale results'
        );

        await controller.checkDocument(document);

        assert.deepStrictEqual(
            collection.operations.map(operation => operation.type),
            ['delete', 'set'],
            'saving should publish fresh diagnostics again'
        );
        assert.strictEqual(collection.operations[1].diagnostics[0].message, 'save error');
    }

    {
        const { mockVscode, recorder } = createMockVscode({
            workspaceRoot: '/workspace/demo',
            executablePath: './build/bin/feng'
        });
        const mockClient = createMockLanguageClientModule({
            initializeResult: {
                capabilities: {
                    hoverProvider: true
                }
            }
        });
        const loaded = loadExtensionModule({
            mockVscode,
            mockLanguageClientModule: mockClient.module,
            keepPatchedLoad: true
        });
        const extensionWithLsp = loaded.extension;
        const context = { subscriptions: [] };

        try {
            await extensionWithLsp.activate(context);
            assert.strictEqual(mockClient.recorder.startCalls, 1);
            assert.strictEqual(mockClient.recorder.constructorArgs.id, 'feng-language-server');
            assert.strictEqual(mockClient.recorder.constructorArgs.name, 'Feng Language Server');
            assert.deepStrictEqual(mockClient.recorder.constructorArgs.clientOptions.documentSelector, [
                { language: 'feng', scheme: 'file' },
                { language: 'feng', scheme: 'untitled' }
            ]);
            assert.deepStrictEqual(mockClient.recorder.constructorArgs.serverOptions, {
                command: path.join('/workspace/demo', './build/bin/feng'),
                args: ['lsp'],
                options: {
                    cwd: '/workspace/demo'
                }
            });
            assert.strictEqual(recorder.formattingProviders.length, 1);
            assert.strictEqual(recorder.diagnosticCollections.length, 0);

            await extensionWithLsp.deactivate();
            assert.strictEqual(mockClient.recorder.stopCalls, 1);
        } finally {
            loaded.restore();
        }
    }

    {
        const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'feng-vscode-exec-'));
        const buildBinDir = path.join(tempRoot, 'build', 'bin');
        const workspaceExecutable = path.join(buildBinDir, 'feng');
        const { mockVscode } = createMockVscode({
            workspaceRoot: tempRoot
        });
        const mockClient = createMockLanguageClientModule({
            initializeResult: {
                capabilities: {
                    hoverProvider: true
                }
            }
        });
        const loaded = loadExtensionModule({
            mockVscode,
            mockLanguageClientModule: mockClient.module,
            keepPatchedLoad: true
        });
        const extensionWithWorkspaceExecutable = loaded.extension;

        fs.mkdirSync(buildBinDir, { recursive: true });
        fs.writeFileSync(workspaceExecutable, '#!/bin/sh\nexit 0\n');

        try {
            await extensionWithWorkspaceExecutable.activate({ subscriptions: [] });
            assert.deepStrictEqual(mockClient.recorder.constructorArgs.serverOptions, {
                command: 'feng',
                args: ['lsp'],
                options: {
                    cwd: tempRoot
                }
            });
        } finally {
            await extensionWithWorkspaceExecutable.deactivate();
            loaded.restore();
            fs.rmSync(tempRoot, { recursive: true, force: true });
        }
    }

    {
        const { mockVscode, recorder } = createMockVscode({
            workspaceRoot: '/workspace/demo',
            executablePath: 'feng'
        });
        const mockClient = createMockLanguageClientModule({
            initializeResult: {
                capabilities: {}
            }
        });
        const loaded = loadExtensionModule({
            mockVscode,
            mockLanguageClientModule: mockClient.module,
            keepPatchedLoad: true
        });
        const extensionWithFallback = loaded.extension;

        try {
            await extensionWithFallback.activate({ subscriptions: [] });
            assert.strictEqual(recorder.diagnosticCollections.length, 1);
            assert.strictEqual(recorder.warningMessages.length, 1);
            assert.strictEqual(recorder.warningMessages[0].includes('no language capabilities'), true);
        } finally {
            loaded.restore();
        }
    }

    {
        const { mockVscode, recorder } = createMockVscode({
            workspaceRoot: '/workspace/demo',
            executablePath: 'feng'
        });
        const mockClient = createMockLanguageClientModule({
            startError: new Error('spawn failed')
        });
        const loaded = loadExtensionModule({
            mockVscode,
            mockLanguageClientModule: mockClient.module,
            keepPatchedLoad: true
        });
        const extensionWithStartupError = loaded.extension;

        try {
            await extensionWithStartupError.activate({ subscriptions: [] });
            assert.strictEqual(recorder.diagnosticCollections.length, 1);
            assert.strictEqual(recorder.warningMessages.length, 1);
            assert.strictEqual(recorder.warningMessages[0].includes('spawn failed'), true);
        } finally {
            loaded.restore();
        }
    }

    console.log('diagnostics tests passed');
}

run().catch(error => {
    console.error(error);
    process.exitCode = 1;
});