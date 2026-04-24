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

function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection('feng');
    context.subscriptions.push(collection);

    async function checkDocument(document) {
        if (document.languageId !== 'feng' || document.uri.scheme !== 'file') {
            return;
        }
        const entries = await runCheck(document.uri.fsPath);
        collection.set(document.uri, entriesToDiagnostics(entries));
    }

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
        vscode.workspace.onDidOpenTextDocument(checkDocument),
        vscode.workspace.onDidSaveTextDocument(checkDocument),
        vscode.workspace.onDidCloseTextDocument(document => {
            collection.delete(document.uri);
        })
    );

    // 对已打开的文档立即做一次检查
    vscode.workspace.textDocuments.forEach(checkDocument);
}

function deactivate() {
}

module.exports = {
    activate,
    deactivate
};