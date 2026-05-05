const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const { formatFengSource, formatFengManifestSource } = require('./formatter');

const RESTART_LANGUAGE_SERVER_COMMAND = 'feng.restartLanguageServer';

let languageServerController;

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

function disposeDisposable(disposable) {
    if (disposable != null && typeof disposable.dispose === 'function') {
        disposable.dispose();
    }
}

function createCompositeDisposable(disposables) {
    return {
        dispose() {
            while (disposables.length > 0) {
                disposeDisposable(disposables.pop());
            }
        }
    };
}

function updateLanguageServerStatusBar(statusBarItem, state) {
    if (statusBarItem == null) {
        return;
    }

    statusBarItem.command = RESTART_LANGUAGE_SERVER_COMMAND;
    if (state === 'starting') {
        statusBarItem.text = '$(sync~spin) Feng LSP';
        statusBarItem.tooltip = 'Starting Feng language server';
    } else if (state === 'restarting') {
        statusBarItem.text = '$(sync~spin) Feng LSP';
        statusBarItem.tooltip = 'Restarting Feng language server';
    } else if (state === 'running') {
        statusBarItem.text = '$(check) Feng LSP';
        statusBarItem.tooltip = 'Feng language server is running. Click to restart.';
    } else if (state === 'fallback') {
        statusBarItem.text = '$(warning) Feng LSP';
        statusBarItem.tooltip = 'Feng language server has no capabilities. Click to restart.';
    } else if (state === 'failed') {
        statusBarItem.text = '$(warning) Feng LSP';
        statusBarItem.tooltip = 'Feng language server failed to start. Click to retry.';
    } else {
        statusBarItem.text = '$(debug-restart) Feng LSP';
        statusBarItem.tooltip = 'Feng language server is stopped. Click to start.';
    }
}

function createLanguageServerStatusBar(vscodeApi = vscode) {
    let statusBarItem;

    if (vscodeApi.window == null || typeof vscodeApi.window.createStatusBarItem !== 'function') {
        return undefined;
    }

    statusBarItem = vscodeApi.window.createStatusBarItem(
        vscodeApi.StatusBarAlignment != null ? vscodeApi.StatusBarAlignment.Right : undefined,
        100
    );
    statusBarItem.name = 'Feng Language Server';
    updateLanguageServerStatusBar(statusBarItem, 'starting');
    if (typeof statusBarItem.show === 'function') {
        statusBarItem.show();
    }
    return statusBarItem;
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
    const disposables = [];
    const collection = vscodeApi.languages.createDiagnosticCollection('feng');
    disposables.push(collection);
    const diagnostics = createDiagnosticController({
        collection,
        runCheckEntries
    });

    disposables.push(
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

    const disposable = createCompositeDisposable(disposables);
    context.subscriptions.push(disposable);
    return disposable;
}

function buildLspStartupWarning(error) {
    let message;

    if (error == null) {
        message = 'unknown error';
    } else if (typeof error === 'string') {
        message = error || 'unknown error';
    } else if (typeof error.message === 'string') {
        message = error.message || 'unknown error';
    } else {
        message = String(error) || 'unknown error';
    }

    return `Feng LSP startup failed, falling back to legacy diagnostics: ${message}`;
}

function buildLspCapabilityWarning() {
    return 'Feng LSP reported no language capabilities, falling back to legacy diagnostics. Check that the extension is launching a current Feng executable.';
}

function createLanguageServerController({
    context,
    vscodeApi = vscode,
    loadLanguageClientModuleFn = loadLanguageClientModule,
    runCheckEntries = runCheck,
    statusBarItem
}) {
    let activeClient;
    let legacyDiagnostics;
    let lifecycle = Promise.resolve();

    function registerFallbackDiagnostics() {
        if (legacyDiagnostics === undefined) {
            legacyDiagnostics = registerLegacyDiagnostics(context, vscodeApi, runCheckEntries);
        }
    }

    function disposeFallbackDiagnostics() {
        disposeDisposable(legacyDiagnostics);
        legacyDiagnostics = undefined;
    }

    async function stopActiveClient() {
        const clientToStop = activeClient;

        activeClient = undefined;
        if (clientToStop != null && typeof clientToStop.stop === 'function') {
            await clientToStop.stop();
        }
    }

    async function startClient() {
        const workspaceRoot = getPrimaryWorkspaceRoot(vscodeApi);
        const executablePathConfig = getExecutablePathConfig(vscodeApi);
        let nextClient;

        updateLanguageServerStatusBar(statusBarItem, 'starting');
        try {
            nextClient = createLanguageClient({
                executablePath: executablePathConfig.executablePath,
                workspaceRoot,
                hasExplicitSetting: executablePathConfig.hasExplicitSetting,
                languageClientModule: loadLanguageClientModuleFn()
            });
            await nextClient.start();
        } catch (error) {
            activeClient = undefined;
            registerFallbackDiagnostics();
            updateLanguageServerStatusBar(statusBarItem, 'failed');
            if (vscodeApi.window != null && typeof vscodeApi.window.showWarningMessage === 'function') {
                void vscodeApi.window.showWarningMessage(buildLspStartupWarning(error));
            }
            return false;
        }

        activeClient = nextClient;
        if (!hasAnyLspCapability(nextClient.initializeResult != null ? nextClient.initializeResult.capabilities : null)) {
            registerFallbackDiagnostics();
            updateLanguageServerStatusBar(statusBarItem, 'fallback');
            if (vscodeApi.window != null && typeof vscodeApi.window.showWarningMessage === 'function') {
                void vscodeApi.window.showWarningMessage(buildLspCapabilityWarning());
            }
            return false;
        }

        disposeFallbackDiagnostics();
        updateLanguageServerStatusBar(statusBarItem, 'running');
        return true;
    }

    function serialize(operation) {
        lifecycle = lifecycle.then(operation, operation);
        return lifecycle;
    }

    async function start() {
        return startClient();
    }

    async function restart() {
        return serialize(async () => {
            const restarted = await (async () => {
                updateLanguageServerStatusBar(statusBarItem, 'restarting');
                await stopActiveClient();
                disposeFallbackDiagnostics();
                return startClient();
            })();

            if (restarted && vscodeApi.window != null && typeof vscodeApi.window.showInformationMessage === 'function') {
                void vscodeApi.window.showInformationMessage('Feng language server restarted.');
            }
            return restarted;
        });
    }

    async function stop() {
        return serialize(async () => {
            updateLanguageServerStatusBar(statusBarItem, 'stopped');
            disposeFallbackDiagnostics();
            await stopActiveClient();
        });
    }

    return {
        start,
        restart,
        stop,
        get client() {
            return activeClient;
        }
    };
}

function registerLanguageServerRestartCommand(context, controller, vscodeApi = vscode) {
    if (vscodeApi.commands == null || typeof vscodeApi.commands.registerCommand !== 'function') {
        return;
    }
    context.subscriptions.push(
        vscodeApi.commands.registerCommand(RESTART_LANGUAGE_SERVER_COMMAND, () => controller.restart())
    );
}

async function activate(context) {
    const statusBarItem = createLanguageServerStatusBar(vscode);

    registerFormatter(context, vscode);
    if (statusBarItem !== undefined) {
        context.subscriptions.push(statusBarItem);
    }
    languageServerController = createLanguageServerController({
        context,
        vscodeApi: vscode,
        loadLanguageClientModuleFn: loadLanguageClientModule,
        runCheckEntries: runCheck,
        statusBarItem
    });
    registerLanguageServerRestartCommand(context, languageServerController, vscode);
    await languageServerController.start();
}

function deactivate() {
    if (languageServerController === undefined) {
        return undefined;
    }

    const controller = languageServerController;
    languageServerController = undefined;
    return controller.stop();
}

module.exports = {
    activate,
    deactivate,
    __test__: {
        buildLspCapabilityWarning,
        buildLspStartupWarning,
        buildCheckCommand,
        createCompositeDisposable,
        createLanguageClient,
        createDiagnosticController,
        createLanguageServerController,
        createLanguageServerStatusBar,
        createServerOptions,
        entriesToDiagnostics,
        filterEntriesForPath,
        findProjectManifestPath,
        getFormattingDocumentSelector,
        getLanguageServiceDocumentSelector,
        getPrimaryWorkspaceRoot,
        hasAnyLspCapability,
        isCheckableFengDocument,
        registerLanguageServerRestartCommand,
        registerLegacyDiagnostics,
        updateLanguageServerStatusBar,
        RESTART_LANGUAGE_SERVER_COMMAND,
        resolveExecutablePath,
        sameFilePath,
        formatDocumentSource
    }
};