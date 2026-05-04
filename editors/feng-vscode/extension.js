const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const { formatFengSource, formatFengManifestSource } = require('./formatter');

let client;

function getExecutablePathConfig(vscodeApi = vscode) {
    const config = vscodeApi.workspace.getConfiguration('feng');
    const executablePath = config.get('executablePath', 'feng');
    const inspected = typeof config.inspect === 'function'
        ? config.inspect('executablePath')
        : null;
    const hasExplicitSetting = inspected != null && (
        inspected.workspaceFolderValue !== undefined ||
        inspected.workspaceValue !== undefined ||
        inspected.globalValue !== undefined
    );

    return {
        executablePath,
        hasExplicitSetting
    };
}

function getPrimaryWorkspaceRoot(vscodeApi = vscode) {
    const folders = vscodeApi.workspace.workspaceFolders;

    if (!Array.isArray(folders) || folders.length === 0) {
        return null;
    }
    if (folders[0] == null || folders[0].uri == null || typeof folders[0].uri.fsPath !== 'string') {
        return null;
    }
    return folders[0].uri.fsPath;
}

function resolveExecutablePath(executablePath, workspaceRoot, hasExplicitSetting = true) {
    if (typeof executablePath !== 'string' || executablePath.length === 0) {
        return 'feng';
    }
    if (!hasExplicitSetting && executablePath === 'feng') {
        return 'feng';
    }
    if (path.isAbsolute(executablePath) || typeof workspaceRoot !== 'string' || workspaceRoot.length === 0) {
        return executablePath;
    }
    return path.join(workspaceRoot, executablePath);
}

function getLanguageServiceDocumentSelector() {
    return [
        { language: 'feng', scheme: 'file' },
        { language: 'feng', scheme: 'untitled' }
    ];
}

function getFormattingDocumentSelector() {
    return [
        { language: 'feng', scheme: 'file' },
        { language: 'feng', scheme: 'untitled' },
        { language: 'feng-manifest', scheme: 'file' },
        { language: 'feng-manifest', scheme: 'untitled' }
    ];
}

function createServerOptions(executablePath, workspaceRoot, hasExplicitSetting = true) {
    const serverOptions = {
        command: resolveExecutablePath(executablePath, workspaceRoot, hasExplicitSetting),
        args: ['lsp']
    };

    if (typeof workspaceRoot === 'string' && workspaceRoot.length > 0) {
        serverOptions.options = {
            cwd: workspaceRoot
        };
    }
    return serverOptions;
}

function loadLanguageClientModule() {
    return require('vscode-languageclient/node');
}

function createLanguageClient({ executablePath, workspaceRoot, hasExplicitSetting, languageClientModule }) {
    const moduleRef = languageClientModule || loadLanguageClientModule();

    return new moduleRef.LanguageClient(
        'feng-language-server',
        'Feng Language Server',
        createServerOptions(executablePath, workspaceRoot, hasExplicitSetting),
        {
            documentSelector: getLanguageServiceDocumentSelector()
        }
    );
}

function hasAnyLspCapability(capabilities) {
    return capabilities != null && Object.keys(capabilities).length > 0;
}

function isExistingFile(filePath) {
    try {
        return fs.statSync(filePath).isFile();
    } catch (_) {
        return false;
    }
}

function findProjectManifestPath(filePath) {
    let currentDir;

    if (typeof filePath !== 'string' || filePath.length === 0) {
        return null;
    }

    currentDir = path.dirname(path.resolve(filePath));
    while (true) {
        const manifestPath = path.join(currentDir, 'feng.fm');
        if (isExistingFile(manifestPath)) {
            return manifestPath;
        }
        const parentDir = path.dirname(currentDir);
        if (parentDir === currentDir) {
            return null;
        }
        currentDir = parentDir;
    }
}

function buildCheckCommand(filePath) {
    if (findProjectManifestPath(filePath) !== null) {
        return ['check', '--format', 'json', filePath];
    }
    return ['tool', 'check', filePath];
}

function sameFilePath(lhs, rhs) {
    if (typeof lhs !== 'string' || typeof rhs !== 'string') {
        return false;
    }
    return path.resolve(lhs) === path.resolve(rhs);
}

function filterEntriesForPath(entries, filePath) {
    return entries.filter(entry => sameFilePath(entry.path, filePath));
}

function runCheck(filePath) {
    return new Promise((resolve) => {
        const executablePathConfig = getExecutablePathConfig(vscode);
        const execPath = resolveExecutablePath(
            executablePathConfig.executablePath,
            getPrimaryWorkspaceRoot(vscode),
            executablePathConfig.hasExplicitSetting
        );
        const proc = cp.spawn(execPath, buildCheckCommand(filePath), {
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

function formatDocumentSource(document, options) {
    const source = document.getText();

    if (document.languageId === 'feng-manifest') {
        return formatFengManifestSource(source, options);
    }

    return formatFengSource(source, options);
}

function registerFormatter(context, vscodeApi = vscode) {
    const formatter = {
        provideDocumentFormattingEdits(document, options) {
            const source = document.getText();
            const formatted = formatDocumentSource(document, options);

            if (formatted === source) {
                return [];
            }

            return [
                vscodeApi.TextEdit.replace(
                    new vscodeApi.Range(document.positionAt(0), document.positionAt(source.length)),
                    formatted
                )
            ];
        }
    };

    context.subscriptions.push(
        vscodeApi.languages.registerDocumentFormattingEditProvider(getFormattingDocumentSelector(), formatter)
    );
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

        collection.set(document.uri,
                       entriesToDiagnostics(filterEntriesForPath(entries,
                                                                document.uri.fsPath)));
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

function registerLegacyDiagnostics(context,
                                   vscodeApi = vscode,
                                   runCheckEntries = runCheck) {
    const collection = vscodeApi.languages.createDiagnosticCollection('feng');
    context.subscriptions.push(collection);
    const diagnostics = createDiagnosticController({
        collection,
        runCheckEntries
    });

    context.subscriptions.push(
        vscodeApi.workspace.onDidOpenTextDocument(diagnostics.checkDocument),
        vscodeApi.workspace.onDidChangeTextDocument(event => {
            diagnostics.clearDocument(event.document);
        }),
        vscodeApi.workspace.onDidSaveTextDocument(diagnostics.checkDocument),
        vscodeApi.workspace.onDidCloseTextDocument(diagnostics.closeDocument)
    );

    vscodeApi.workspace.textDocuments.forEach(document => {
        void diagnostics.checkDocument(document);
    });
}

function buildLspStartupWarning(error) {
    const message = error != null && typeof error.message === 'string'
        ? error.message
        : 'unknown error';

    return `Feng LSP startup failed, falling back to legacy diagnostics: ${message}`;
}

function buildLspCapabilityWarning() {
    return 'Feng LSP reported no language capabilities, falling back to legacy diagnostics. Check that the extension is launching a current Feng executable.';
}

async function activate(context) {
    const workspaceRoot = getPrimaryWorkspaceRoot(vscode);
    const executablePathConfig = getExecutablePathConfig(vscode);

    registerFormatter(context, vscode);
    try {
        client = createLanguageClient({
            executablePath: executablePathConfig.executablePath,
            workspaceRoot,
            hasExplicitSetting: executablePathConfig.hasExplicitSetting,
            languageClientModule: loadLanguageClientModule()
        });
        await client.start();
    } catch (error) {
        client = undefined;
        if (vscode.window != null && typeof vscode.window.showWarningMessage === 'function') {
            void vscode.window.showWarningMessage(buildLspStartupWarning(error));
        }
        registerLegacyDiagnostics(context, vscode, runCheck);
        return;
    }

    if (!hasAnyLspCapability(client.initializeResult != null ? client.initializeResult.capabilities : null)) {
        if (vscode.window != null && typeof vscode.window.showWarningMessage === 'function') {
            void vscode.window.showWarningMessage(buildLspCapabilityWarning());
        }
        registerLegacyDiagnostics(context, vscode, runCheck);
    }
}

function deactivate() {
    if (client === undefined) {
        return undefined;
    }

    const activeClient = client;
    client = undefined;
    return activeClient.stop();
}

module.exports = {
    activate,
    deactivate,
    __test__: {
        buildLspCapabilityWarning,
        buildLspStartupWarning,
        buildCheckCommand,
        createLanguageClient,
        createDiagnosticController,
        createServerOptions,
        entriesToDiagnostics,
        filterEntriesForPath,
        findProjectManifestPath,
        getFormattingDocumentSelector,
        getLanguageServiceDocumentSelector,
        getPrimaryWorkspaceRoot,
        hasAnyLspCapability,
        isCheckableFengDocument,
        registerLegacyDiagnostics,
        resolveExecutablePath,
        sameFilePath,
        formatDocumentSource
    }
};