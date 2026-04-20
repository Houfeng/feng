const vscode = require('vscode');

const { formatFengSource } = require('./formatter');

function activate(context) {
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
        vscode.languages.registerDocumentFormattingEditProvider(selector, formatter)
    );
}

function deactivate() {
}

module.exports = {
    activate,
    deactivate
};