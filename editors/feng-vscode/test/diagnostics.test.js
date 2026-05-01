const assert = require('assert');
const Module = require('module');
const fs = require('fs');
const os = require('os');
const path = require('path');

function loadExtensionModule() {
    const originalLoad = Module._load;
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
        workspace: {
            getConfiguration() {
                return {
                    get(_key, defaultValue) {
                        return defaultValue;
                    }
                };
            }
        }
    };

    const extensionPath = require.resolve('../extension');
    delete require.cache[extensionPath];

    Module._load = function patchedLoad(request, parent, isMain) {
        if (request === 'vscode') {
            return mockVscode;
        }
        return originalLoad.call(this, request, parent, isMain);
    };

    try {
        return require(extensionPath);
    } finally {
        Module._load = originalLoad;
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
        buildCheckCommand,
        createDiagnosticController,
        filterEntriesForPath,
        findProjectManifestPath,
        isCheckableFengDocument,
        formatDocumentSource
    } = extension.__test__;

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

    console.log('diagnostics tests passed');
}

run().catch(error => {
    console.error(error);
    process.exitCode = 1;
});