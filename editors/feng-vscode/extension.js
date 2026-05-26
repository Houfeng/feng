const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const { formatFengSource, formatFengManifestSource } = require('./formatter');

const RESTART_LANGUAGE_SERVER_COMMAND = 'feng.restartLanguageServer';
const FENG_DEBUG_TYPE = 'feng';
const FENG_TASK_TYPE = 'feng';
const FENG_BUILD_TASK = 'build';

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

function getWorkspaceFolderPath(workspaceFolder) {
    if (workspaceFolder == null || workspaceFolder.uri == null || typeof workspaceFolder.uri.fsPath !== 'string') {
        return null;
    }
    return workspaceFolder.uri.fsPath;
}

function getWorkspaceFolderForPath(filePath, vscodeApi = vscode) {
    const workspace = vscodeApi.workspace;
    let bestMatch = null;
    let bestLength = -1;
    let resolvedPath;

    if (workspace == null || !Array.isArray(workspace.workspaceFolders) || typeof filePath !== 'string' || filePath.length === 0) {
        return null;
    }

    resolvedPath = path.resolve(filePath);
    if (typeof workspace.getWorkspaceFolder === 'function') {
        const directMatch = workspace.getWorkspaceFolder({ fsPath: resolvedPath });

        if (directMatch != null) {
            return directMatch;
        }
    }

    for (const workspaceFolder of workspace.workspaceFolders) {
        const workspacePath = getWorkspaceFolderPath(workspaceFolder);
        const resolvedWorkspacePath = typeof workspacePath === 'string'
            ? path.resolve(workspacePath)
            : null;

        if (resolvedWorkspacePath == null) {
            continue;
        }
        if (resolvedPath !== resolvedWorkspacePath && !resolvedPath.startsWith(resolvedWorkspacePath + path.sep)) {
            continue;
        }
        if (resolvedWorkspacePath.length <= bestLength) {
            continue;
        }
        bestMatch = workspaceFolder;
        bestLength = resolvedWorkspacePath.length;
    }

    return bestMatch;
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

function isExistingDirectory(filePath) {
    try {
        return fs.statSync(filePath).isDirectory();
    } catch (_) {
        return false;
    }
}

function findProjectManifestPath(filePath) {
    let currentDir;
    const resolvedPath = path.resolve(filePath);

    if (typeof filePath !== 'string' || filePath.length === 0) {
        return null;
    }

    currentDir = isExistingDirectory(resolvedPath)
        ? resolvedPath
        : path.dirname(resolvedPath);
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

function resolveProjectPath(projectRoot, maybeRelativePath) {
    if (typeof maybeRelativePath !== 'string' || maybeRelativePath.length === 0) {
        return projectRoot;
    }
    if (path.isAbsolute(maybeRelativePath) || typeof projectRoot !== 'string' || projectRoot.length === 0) {
        return maybeRelativePath;
    }
    return path.join(projectRoot, maybeRelativePath);
}

function parseProjectManifestDebugSettings(manifestPath) {
    let source;
    let section = null;
    const settings = {
        name: null,
        out: 'build',
        target: null
    };

    if (!isExistingFile(manifestPath)) {
        return null;
    }

    try {
        source = fs.readFileSync(manifestPath, 'utf8');
    } catch (_) {
        return null;
    }

    for (const rawLine of source.split(/\r?\n/u)) {
        const line = rawLine.trim();
        const sectionMatch = /^\[([A-Za-z0-9._-]+)\]$/u.exec(line);
        let fieldMatch;

        if (line.length === 0 || line.startsWith('#')) {
            continue;
        }
        if (sectionMatch != null) {
            section = sectionMatch[1];
            continue;
        }
        if (section !== 'package') {
            continue;
        }
        fieldMatch = /^([A-Za-z0-9._-]+)\s*:\s*"([^"]*)"\s*$/u.exec(line);
        if (fieldMatch == null) {
            continue;
        }
        if (fieldMatch[1] === 'name') {
            settings.name = fieldMatch[2];
        } else if (fieldMatch[1] === 'out') {
            settings.out = fieldMatch[2] || 'build';
        } else if (fieldMatch[1] === 'target') {
            settings.target = fieldMatch[2];
        }
    }

    return settings.name == null ? null : settings;
}

function getProjectDebugSettings(manifestPath) {
    const parsed = parseProjectManifestDebugSettings(manifestPath);
    let outRoot;

    if (parsed == null) {
        return null;
    }

    outRoot = resolveProjectPath(path.dirname(manifestPath), parsed.out || 'build');
    return {
        manifestPath,
        packageName: parsed.name,
        outRoot,
        programPath: path.join(outRoot, 'bin', parsed.name),
        projectRoot: path.dirname(manifestPath),
        target: parsed.target
    };
}

function containsVariableReference(value) {
    return typeof value === 'string' && value.includes('${');
}

function resolveLaunchConfigPath(value, workspaceFolder, vscodeApi = vscode) {
    const baseRoot = getWorkspaceFolderPath(workspaceFolder) || getPrimaryWorkspaceRoot(vscodeApi);

    if (typeof value !== 'string' || value.length === 0 || containsVariableReference(value)) {
        return null;
    }
    if (path.isAbsolute(value) || baseRoot == null) {
        return value;
    }
    return path.resolve(baseRoot, value);
}

function getActiveFilePath(vscodeApi = vscode) {
    const editor = vscodeApi.window != null ? vscodeApi.window.activeTextEditor : null;
    const document = editor != null ? editor.document : null;

    if (document == null || document.uri == null || document.uri.scheme !== 'file') {
        return null;
    }
    return typeof document.uri.fsPath === 'string'
        ? document.uri.fsPath
        : null;
}

function findDebugManifestPath(config, workspaceFolder, vscodeApi = vscode) {
    const programPath = resolveLaunchConfigPath(config != null ? config.program : null,
                                                workspaceFolder,
                                                vscodeApi);
    const cwdPath = resolveLaunchConfigPath(config != null ? config.cwd : null,
                                            workspaceFolder,
                                            vscodeApi);
    const activeFilePath = getActiveFilePath(vscodeApi);
    const workspaceRoot = getWorkspaceFolderPath(workspaceFolder) || getPrimaryWorkspaceRoot(vscodeApi);

    for (const candidatePath of [programPath, cwdPath, activeFilePath, workspaceRoot]) {
        const manifestPath = typeof candidatePath === 'string'
            ? findProjectManifestPath(candidatePath)
            : null;

        if (manifestPath != null) {
            return manifestPath;
        }
    }

    return null;
}

function createBuildTaskName(projectRoot, workspaceFolder) {
    const workspaceRoot = getWorkspaceFolderPath(workspaceFolder);
    let relativePath;

    if (typeof workspaceRoot !== 'string' || workspaceRoot.length === 0) {
        return 'build';
    }

    relativePath = path.relative(workspaceRoot, projectRoot);
    if (relativePath.length === 0 || relativePath === '.') {
        return 'build';
    }
    if (relativePath.startsWith('..')) {
        return `build ${path.basename(projectRoot)}`;
    }
    return `build ${relativePath.split(path.sep).join('/')}`;
}

function createBuildTaskLabel(projectRoot, workspaceFolder) {
    return `feng: ${createBuildTaskName(projectRoot, workspaceFolder)}`;
}

function createFengBuildTask(projectRoot, workspaceFolder, vscodeApi = vscode) {
    const executablePathConfig = getExecutablePathConfig(vscodeApi);
    const execution = new vscodeApi.ProcessExecution(
        resolveExecutablePath(executablePathConfig.executablePath,
                              getPrimaryWorkspaceRoot(vscodeApi),
                              executablePathConfig.hasExplicitSetting),
        ['build'],
        { cwd: projectRoot }
    );
    const task = new vscodeApi.Task(
        {
            type: FENG_TASK_TYPE,
            task: FENG_BUILD_TASK,
            cwd: projectRoot
        },
        workspaceFolder != null ? workspaceFolder : vscodeApi.TaskScope.Workspace,
        createBuildTaskName(projectRoot, workspaceFolder),
        'feng',
        execution,
        []
    );

    if (vscodeApi.TaskGroup != null) {
        task.group = vscodeApi.TaskGroup.Build;
    }
    task.detail = `Build Feng project at ${projectRoot}`;
    return task;
}

async function findWorkspaceManifestPaths(vscodeApi = vscode) {
    const manifestPaths = new Set();
    const workspaceFolders = vscodeApi.workspace != null && Array.isArray(vscodeApi.workspace.workspaceFolders)
        ? vscodeApi.workspace.workspaceFolders
        : [];

    for (const workspaceFolder of workspaceFolders) {
        const workspaceRoot = getWorkspaceFolderPath(workspaceFolder);
        const manifestPath = typeof workspaceRoot === 'string'
            ? path.join(workspaceRoot, 'feng.fm')
            : null;

        if (manifestPath != null && isExistingFile(manifestPath)) {
            manifestPaths.add(path.resolve(manifestPath));
        }
    }

    if (vscodeApi.workspace != null && typeof vscodeApi.workspace.findFiles === 'function') {
        const discovered = await vscodeApi.workspace.findFiles('**/feng.fm',
                                                               '**/{build,.git,node_modules,third_party,temp}/**');

        for (const entry of Array.isArray(discovered) ? discovered : []) {
            if (entry != null && typeof entry.fsPath === 'string') {
                manifestPaths.add(path.resolve(entry.fsPath));
            }
        }
    }

    return Array.from(manifestPaths).sort();
}

function createFengTaskProvider(vscodeApi = vscode) {
    return {
        async provideTasks() {
            const manifestPaths = await findWorkspaceManifestPaths(vscodeApi);

            return manifestPaths
                .map(manifestPath => getProjectDebugSettings(manifestPath))
                .filter(settings => settings != null)
                .map(settings => createFengBuildTask(settings.projectRoot,
                                                     getWorkspaceFolderForPath(settings.projectRoot, vscodeApi),
                                                     vscodeApi));
        },

        resolveTask(task) {
            const definition = task != null ? task.definition : null;
            const projectRoot = definition != null && typeof definition.cwd === 'string'
                ? definition.cwd
                : null;

            if (definition == null || definition.type !== FENG_TASK_TYPE || definition.task !== FENG_BUILD_TASK) {
                return undefined;
            }
            if (typeof projectRoot !== 'string' || projectRoot.length === 0) {
                return undefined;
            }
            return createFengBuildTask(path.resolve(projectRoot),
                                       getWorkspaceFolderForPath(projectRoot, vscodeApi),
                                       vscodeApi);
        }
    };
}

function buildMissingDebugProgramMessage() {
    return 'Unable to resolve the Feng debug program. Open a Feng source file inside a target: "bin" project or set launch.program explicitly.';
}

function buildUnsupportedDebugTargetMessage() {
    return 'Feng debugging currently requires a target: "bin" project when launch.program is omitted.';
}

function showErrorMessage(vscodeApi, message) {
    if (vscodeApi.window != null && typeof vscodeApi.window.showErrorMessage === 'function') {
        void vscodeApi.window.showErrorMessage(message);
    }
}

function createDefaultDebugConfiguration(manifestPath, workspaceFolder, vscodeApi = vscode) {
    const settings = getProjectDebugSettings(manifestPath);
    const taskWorkspaceFolder = settings != null
        ? (workspaceFolder || getWorkspaceFolderForPath(settings.projectRoot, vscodeApi))
        : null;

    if (settings == null || settings.target !== 'bin') {
        return null;
    }

    return {
        type: FENG_DEBUG_TYPE,
        request: 'launch',
        name: `Debug ${settings.packageName}`,
        program: settings.programPath,
        cwd: settings.projectRoot,
        preLaunchTask: taskWorkspaceFolder != null
            ? createBuildTaskLabel(settings.projectRoot, taskWorkspaceFolder)
            : undefined
    };
}

function createFengDebugConfigurationProvider(vscodeApi = vscode) {
    return {
        async provideDebugConfigurations(workspaceFolder) {
            const manifestPath = findDebugManifestPath({}, workspaceFolder, vscodeApi);
            const configuration = manifestPath != null
                ? createDefaultDebugConfiguration(manifestPath, workspaceFolder, vscodeApi)
                : null;

            return configuration == null ? [] : [configuration];
        },

        async resolveDebugConfiguration(workspaceFolder, config) {
            const resolved = config != null ? { ...config } : {};
            const manifestPath = findDebugManifestPath(resolved, workspaceFolder, vscodeApi);
            const settings = manifestPath != null ? getProjectDebugSettings(manifestPath) : null;
            const taskWorkspaceFolder = settings != null
                ? (workspaceFolder || getWorkspaceFolderForPath(settings.projectRoot, vscodeApi))
                : null;

            if (!resolved.type) {
                resolved.type = FENG_DEBUG_TYPE;
            }
            if (!resolved.request) {
                resolved.request = 'launch';
            }
            if (resolved.request !== 'launch') {
                return resolved;
            }
            if (!resolved.name) {
                resolved.name = settings != null ? `Debug ${settings.packageName}` : 'Debug Feng Program';
            }
            if (!resolved.cwd && settings != null) {
                resolved.cwd = settings.projectRoot;
            }
            if (!resolved.program && settings != null) {
                if (settings.target !== 'bin') {
                    showErrorMessage(vscodeApi, buildUnsupportedDebugTargetMessage());
                    return undefined;
                }
                resolved.program = settings.programPath;
            }
            if (!resolved.preLaunchTask && settings != null && taskWorkspaceFolder != null) {
                resolved.preLaunchTask = createBuildTaskLabel(settings.projectRoot, taskWorkspaceFolder);
            }
            if (!resolved.program) {
                showErrorMessage(vscodeApi, buildMissingDebugProgramMessage());
                return undefined;
            }
            return resolved;
        }
    };
}

function createDebugAdapterExecutable(workspaceFolder, vscodeApi = vscode) {
    const executablePathConfig = getExecutablePathConfig(vscodeApi);
    const adapterOptions = {};
    const workspaceRoot = getWorkspaceFolderPath(workspaceFolder) || getPrimaryWorkspaceRoot(vscodeApi);

    if (typeof workspaceRoot === 'string' && workspaceRoot.length > 0) {
        adapterOptions.cwd = workspaceRoot;
    }

    return new vscodeApi.DebugAdapterExecutable(
        resolveExecutablePath(executablePathConfig.executablePath,
                              getPrimaryWorkspaceRoot(vscodeApi),
                              executablePathConfig.hasExplicitSetting),
        ['dap', '--stdio'],
        Object.keys(adapterOptions).length > 0 ? adapterOptions : undefined
    );
}

function registerDebuggingSupport(context, vscodeApi = vscode) {
    const disposables = [];

    if (vscodeApi.debug != null && typeof vscodeApi.debug.registerDebugAdapterDescriptorFactory === 'function') {
        disposables.push(
            vscodeApi.debug.registerDebugAdapterDescriptorFactory(FENG_DEBUG_TYPE, {
                createDebugAdapterDescriptor(session) {
                    return createDebugAdapterExecutable(session != null ? session.workspaceFolder : undefined,
                                                        vscodeApi);
                }
            })
        );
    }
    if (vscodeApi.debug != null && typeof vscodeApi.debug.registerDebugConfigurationProvider === 'function') {
        const triggerKind = vscodeApi.DebugConfigurationProviderTriggerKind != null
            ? vscodeApi.DebugConfigurationProviderTriggerKind.Dynamic
            : undefined;

        disposables.push(
            vscodeApi.debug.registerDebugConfigurationProvider(FENG_DEBUG_TYPE,
                                                               createFengDebugConfigurationProvider(vscodeApi),
                                                               triggerKind)
        );
    }
    if (vscodeApi.tasks != null && typeof vscodeApi.tasks.registerTaskProvider === 'function') {
        disposables.push(
            vscodeApi.tasks.registerTaskProvider(FENG_TASK_TYPE, createFengTaskProvider(vscodeApi))
        );
    }

    context.subscriptions.push(...disposables);
    return createCompositeDisposable(disposables);
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
    registerDebuggingSupport(context, vscode);
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
        findDebugManifestPath,
        findWorkspaceManifestPaths,
        getFormattingDocumentSelector,
        getLanguageServiceDocumentSelector,
        getProjectDebugSettings,
        getPrimaryWorkspaceRoot,
        getWorkspaceFolderForPath,
        getWorkspaceFolderPath,
        hasAnyLspCapability,
        isCheckableFengDocument,
        createBuildTaskLabel,
        createBuildTaskName,
        createDebugAdapterExecutable,
        createDefaultDebugConfiguration,
        createFengBuildTask,
        createFengDebugConfigurationProvider,
        createFengTaskProvider,
        registerLanguageServerRestartCommand,
        registerLegacyDiagnostics,
        registerDebuggingSupport,
        updateLanguageServerStatusBar,
        FENG_BUILD_TASK,
        FENG_DEBUG_TYPE,
        FENG_TASK_TYPE,
        RESTART_LANGUAGE_SERVER_COMMAND,
        resolveExecutablePath,
        resolveLaunchConfigPath,
        resolveProjectPath,
        sameFilePath,
        parseProjectManifestDebugSettings,
        buildMissingDebugProgramMessage,
        buildUnsupportedDebugTargetMessage,
        formatDocumentSource
    }
};