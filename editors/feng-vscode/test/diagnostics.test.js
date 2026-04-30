const assert = require('assert');
const Module = require('module');

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
    const { createDiagnosticController, isCheckableFengDocument, formatDocumentSource } = extension.__test__;

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
        const document = createDocument('/tmp/open.ff');
        const collection = createCollectionRecorder();
        const controller = createDiagnosticController({
            collection,
            async runCheckEntries(filePath) {
                assert.strictEqual(filePath, '/tmp/open.ff');
                return [{
                    line: 1,
                    col: 2,
                    end_col: 4,
                    severity: 'error',
                    message: 'unexpected token',
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