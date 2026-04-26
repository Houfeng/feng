const vscode = require('vscode');
const cp = require('child_process');

const { formatFengSource } = require('./formatter');

function getExecutablePath() {
    return vscode.workspace.getConfiguration('feng').get('executablePath', 'feng');
}

function runCheck(filePath) {
    return new Promise((resolve) => {
        const execPath = getExecutablePath();
        const proc = cp.spawn(execPath, ['check', filePath], {
            stdio: ['ignore', 'pipe', 'pipe']
        });

        let stdout = '';
        proc.stdout.on('data', chunk => { stdout += chunk; });
        proc.on('close', () => {
            try {
                const entries = JSON.parse(stdout);
                resolve(Array.isArray(entries) ? entries : []);
            } catch (_) {
                resolve([]);
            }
        });
        proc.on('error', () => resolve([]));
    });
}

function entriesToDiagnostics(entries) {
    return entries.map(e => {
        const startLine = Math.max(0, (e.line || 1) - 1);
        const startCol  = Math.max(0, (e.col || 1) - 1);
        const endCol    = Math.max(startCol + 1, (e.end_col || 1) - 1);

        const range    = new vscode.Range(
            new vscode.Position(startLine, startCol),
            new vscode.Position(startLine, endCol)
        );
        const severity = e.severity === 'error'
            ? vscode.DiagnosticSeverity.Error
            : vscode.DiagnosticSeverity.Information;

        const diag = new vscode.Diagnostic(range, e.message || '', severity);
        diag.source = `feng(${e.source || 'check'})`;
        return diag;
    });
}

function isCheckableFengDocument(document) {
    return document.languageId === 'feng' && document.uri.scheme === 'file';
}

function createDiagnosticController({ collection, runCheckEntries }) {
    const generationByUri = new Map();

    function bumpGeneration(uri) {
        const key = uri.toString();
        const generation = (generationByUri.get(key) || 0) + 1;
        generationByUri.set(key, generation);
        return { key, generation };
    }

    async function checkDocument(document) {
        if (!isCheckableFengDocument(document)) {
            return;
        }

        const { key, generation } = bumpGeneration(document.uri);
        const entries = await runCheckEntries(document.uri.fsPath);

        // Ignore stale results once the document changes or a newer check starts.
        if (generationByUri.get(key) !== generation) {
            return;
        }

        collection.set(document.uri, entriesToDiagnostics(entries));
    }

    function clearDocument(document) {
        if (!isCheckableFengDocument(document)) {
            return;
        }

        bumpGeneration(document.uri);
        collection.delete(document.uri);
    }

    function closeDocument(document) {
        if (!isCheckableFengDocument(document)) {
            return;
        }

        generationByUri.delete(document.uri.toString());
        collection.delete(document.uri);
    }

    return {
        checkDocument,
        clearDocument,
        closeDocument
    };
}

function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection('feng');
    context.subscriptions.push(collection);
    const diagnostics = createDiagnosticController({
        collection,
        runCheckEntries: runCheck
    });

    const selector = [
        { language: 'feng', scheme: 'file' },
        { language: 'feng', scheme: 'untitled' }
    ];

    const formatter = {
        provideDocumentFormattingEdits(document, options) {
            const source = document.getText();
            const formatted = formatFengSource(source, options);

            if (formatted === source) {
                return [];
            }

            return [
                vscode.TextEdit.replace(
                    new vscode.Range(document.positionAt(0), document.positionAt(source.length)),
                    formatted
                )
            ];
        }
    };

    context.subscriptions.push(
        vscode.languages.registerDocumentFormattingEditProvider(selector, formatter),
        vscode.workspace.onDidOpenTextDocument(diagnostics.checkDocument),
        vscode.workspace.onDidChangeTextDocument(event => {
            diagnostics.clearDocument(event.document);
        }),
        vscode.workspace.onDidSaveTextDocument(diagnostics.checkDocument),
        vscode.workspace.onDidCloseTextDocument(diagnostics.closeDocument)
    );

    // 对已打开的文档立即做一次检查
    vscode.workspace.textDocuments.forEach(diagnostics.checkDocument);
}

function deactivate() {
}

module.exports = {
    activate,
    deactivate,
    __test__: {
        createDiagnosticController,
        entriesToDiagnostics,
        isCheckableFengDocument
    }
};