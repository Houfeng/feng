const assert = require('assert');
const Module = require('module');
const fs = require('fs');
const os = require('os');
const path = require('path');

const packageJson = require('../package.json');

function createDisposable() {
    return {
        dispose() {
        }
    };
}

function createWorkspaceFolder(fsPath) {
    return {
        uri: {
            fsPath
        }
    };
}

function createMockVscode(options = {}) {
    const recorder = {
        adapterFactories: [],
        configurationProviders: [],
        errorMessages: [],
        findFilesCalls: [],
        taskProviders: []
    };
    const workspaceFolders = Array.isArray(options.workspaceFolders)
        ? options.workspaceFolders
        : (options.workspaceRoot == null ? [] : [createWorkspaceFolder(options.workspaceRoot)]);
    const executablePath = options.executablePath;
    const hasExplicitSetting = options.hasExplicitSetting !== false && executablePath !== undefined;

    return {
        mockVscode: {
            DebugAdapterExecutable: class DebugAdapterExecutable {
                constructor(command, args, optionsValue) {
                    this.command = command;
                    this.args = args;
                    this.options = optionsValue;
                }
            },
            DebugConfigurationProviderTriggerKind: {
                Initial: 1,
                Dynamic: 2
            },
            ProcessExecution: class ProcessExecution {
                constructor(process, args, optionsValue) {
                    this.process = process;
                    this.args = args;
                    this.options = optionsValue;
                }
            },
            Task: class Task {
                constructor(definition, scope, name, source, execution, problemMatchers) {
                    this.definition = definition;
                    this.scope = scope;
                    this.name = name;
                    this.source = source;
                    this.execution = execution;
                    this.problemMatchers = problemMatchers;
                    this.group = undefined;
                    this.detail = undefined;
                }

                get label() {
                    return `${this.source}: ${this.name}`;
                }
            },
            TaskGroup: {
                Build: { kind: 'build' }
            },
            TaskScope: {
                Workspace: 1
            },
            debug: {
                registerDebugAdapterDescriptorFactory(type, factory) {
                    recorder.adapterFactories.push({ type, factory });
                    return createDisposable();
                },
                registerDebugConfigurationProvider(type, provider, triggerKind) {
                    recorder.configurationProviders.push({ type, provider, triggerKind });
                    return createDisposable();
                }
            },
            tasks: {
                registerTaskProvider(type, provider) {
                    recorder.taskProviders.push({ type, provider });
                    return createDisposable();
                }
            },
            window: {
                activeTextEditor: options.activeTextEditor || null,
                showErrorMessage(message) {
                    recorder.errorMessages.push(message);
                    return Promise.resolve(undefined);
                }
            },
            workspace: {
                workspaceFolders,
                findFiles(include, exclude) {
                    recorder.findFilesCalls.push({ include, exclude });
                    return Promise.resolve(options.findFilesResults || []);
                },
                getConfiguration() {
                    return {
                        get(_key, defaultValue) {
                            return executablePath !== undefined ? executablePath : defaultValue;
                        },
                        inspect() {
                            if (!hasExplicitSetting) {
                                return {
                                    workspaceFolderValue: undefined,
                                    workspaceValue: undefined,
                                    globalValue: undefined
                                };
                            }
                            return {
                                workspaceFolderValue: executablePath,
                                workspaceValue: undefined,
                                globalValue: undefined
                            };
                        }
                    };
                },
                getWorkspaceFolder(uri) {
                    const targetPath = uri != null && typeof uri.fsPath === 'string'
                        ? path.resolve(uri.fsPath)
                        : null;

                    if (targetPath == null) {
                        return null;
                    }

                    for (const workspaceFolder of workspaceFolders) {
                        const workspacePath = path.resolve(workspaceFolder.uri.fsPath);

                        if (targetPath === workspacePath || targetPath.startsWith(workspacePath + path.sep)) {
                            return workspaceFolder;
                        }
                    }

                    return null;
                }
            }
        },
        recorder
    };
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
    };
}

function loadExtensionModule(mockVscode) {
    const originalLoad = Module._load;
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

async function run() {
    const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'feng-vscode-debug-'));
    const projectRoot = path.join(tempRoot, 'examples', 'hello_world');
    const projectSrcDir = path.join(projectRoot, 'src');
    const projectManifestPath = path.join(projectRoot, 'feng.fm');
    const projectSourcePath = path.join(projectSrcDir, 'main.ff');
    const libProjectRoot = path.join(tempRoot, 'libs', 'demo_lib');
    const libManifestPath = path.join(libProjectRoot, 'feng.fm');
    const libSourcePath = path.join(libProjectRoot, 'src', 'lib.ff');

    fs.mkdirSync(projectSrcDir, { recursive: true });
    fs.writeFileSync(projectManifestPath,
                     '[package]\nname: "hello_world"\nversion: "0.1.0"\ntarget: "bin"\nout: "dist"\n');
    fs.writeFileSync(projectSourcePath, 'fn main(args: string[]): void {}\n');

    fs.mkdirSync(path.dirname(libSourcePath), { recursive: true });
    fs.writeFileSync(libManifestPath,
                     '[package]\nname: "demo_lib"\nversion: "0.1.0"\ntarget: "lib"\nout: "artifacts"\n');
    fs.writeFileSync(libSourcePath, 'fn helper(): void {}\n');

    try {
        {
            const { mockVscode, recorder } = createMockVscode({
                workspaceRoot: tempRoot,
                activeTextEditor: createActiveEditor(projectSourcePath),
                findFilesResults: [
                    { fsPath: projectManifestPath }
                ]
            });
            const extension = loadExtensionModule(mockVscode);
            const {
                createBuildTaskLabel,
                createBuildTaskName,
                createDefaultDebugConfiguration,
                createDebugAdapterExecutable,
                createFengBuildTask,
                createFengDebugConfigurationProvider,
                createFengTaskProvider,
                findProjectManifestPath,
                getProjectDebugSettings,
                registerDebuggingSupport,
                FENG_BUILD_TASK,
                FENG_DEBUG_TYPE,
                FENG_TASK_TYPE
            } = extension.__test__;
            const workspaceFolder = createWorkspaceFolder(tempRoot);
            const taskProvider = createFengTaskProvider(mockVscode);
            let providedTasks;
            let resolvedConfiguration;
            let context;

            assert.strictEqual(FENG_DEBUG_TYPE, 'feng');
            assert.strictEqual(FENG_TASK_TYPE, 'feng');
            assert.strictEqual(FENG_BUILD_TASK, 'build');
            assert.strictEqual(findProjectManifestPath(projectSourcePath), projectManifestPath);
            assert.strictEqual(findProjectManifestPath(projectRoot), projectManifestPath);

            assert.deepStrictEqual(getProjectDebugSettings(projectManifestPath), {
                manifestPath: projectManifestPath,
                packageName: 'hello_world',
                outRoot: path.join(projectRoot, 'dist'),
                programPath: path.join(projectRoot, 'dist', 'bin', 'hello_world'),
                projectRoot,
                target: 'bin'
            });

            assert.strictEqual(createBuildTaskName(projectRoot, workspaceFolder), 'build examples/hello_world');
            assert.strictEqual(createBuildTaskLabel(projectRoot, workspaceFolder), 'feng: build examples/hello_world');

            {
                const buildTask = createFengBuildTask(projectRoot, workspaceFolder, mockVscode);

                assert.deepStrictEqual(buildTask.definition, {
                    type: 'feng',
                    task: 'build',
                    cwd: projectRoot
                });
                assert.strictEqual(buildTask.name, 'build examples/hello_world');
                assert.strictEqual(buildTask.label, 'feng: build examples/hello_world');
                assert.strictEqual(buildTask.execution.process, 'feng');
                assert.deepStrictEqual(buildTask.execution.args, ['build']);
                assert.deepStrictEqual(buildTask.execution.options, { cwd: projectRoot });
                assert.strictEqual(buildTask.group, mockVscode.TaskGroup.Build);
            }

            {
                const adapter = createDebugAdapterExecutable(workspaceFolder, mockVscode);

                assert.strictEqual(adapter.command, 'feng');
                assert.deepStrictEqual(adapter.args, ['dap', '--stdio']);
                assert.deepStrictEqual(adapter.options, { cwd: tempRoot });
            }

            assert.deepStrictEqual(createDefaultDebugConfiguration(projectManifestPath,
                                                                   workspaceFolder,
                                                                   mockVscode), {
                type: 'feng',
                request: 'launch',
                name: 'Debug hello_world',
                program: '${workspaceFolder}/examples/hello_world/dist/bin/hello_world',
                cwd: '${workspaceFolder}/examples/hello_world',
                preLaunchTask: 'feng: build examples/hello_world'
            });

            {
                const resolvedTask = taskProvider.resolveTask({
                    definition: {
                        type: 'feng',
                        task: 'build',
                        cwd: '${workspaceFolder}/examples/hello_world'
                    },
                    scope: workspaceFolder
                });

                assert(resolvedTask, 'expected persisted Feng task to resolve');
                assert.strictEqual(resolvedTask.label, 'feng: build examples/hello_world');
                assert.deepStrictEqual(resolvedTask.execution.options, { cwd: projectRoot });
            }

            resolvedConfiguration = await createFengDebugConfigurationProvider(mockVscode)
                .resolveDebugConfiguration(workspaceFolder, {
                    type: 'feng',
                    request: 'launch'
                });
            assert.deepStrictEqual(resolvedConfiguration, {
                type: 'feng',
                request: 'launch',
                name: 'Debug hello_world',
                program: path.join(projectRoot, 'dist', 'bin', 'hello_world'),
                cwd: projectRoot,
                preLaunchTask: 'feng: build examples/hello_world'
            });

            providedTasks = await taskProvider.provideTasks();
            assert.strictEqual(providedTasks.length, 1);
            assert.strictEqual(providedTasks[0].label, 'feng: build examples/hello_world');

            context = { subscriptions: [] };
            registerDebuggingSupport(context, mockVscode);
            assert.strictEqual(context.subscriptions.length, 4);
            assert.deepStrictEqual(recorder.configurationProviders.map(entry => entry.triggerKind), [1, 2]);
        }

        {
            const { mockVscode } = createMockVscode({
                workspaceRoot: tempRoot,
                activeTextEditor: null,
                findFilesResults: [
                    { fsPath: projectManifestPath },
                    { fsPath: libManifestPath }
                ]
            });
            const extension = loadExtensionModule(mockVscode);
            const provider = extension.__test__.createFengDebugConfigurationProvider(mockVscode);
            const workspaceFolder = createWorkspaceFolder(tempRoot);
            const providedConfigurations = await provider.provideDebugConfigurations(workspaceFolder);
            const tasksPath = path.join(tempRoot, '.vscode', 'tasks.json');
            const resolved = await provider.resolveDebugConfiguration(workspaceFolder, {
                type: 'feng',
                request: 'launch'
            });

            assert.deepStrictEqual(providedConfigurations, [{
                type: 'feng',
                request: 'launch',
                name: 'Debug hello_world',
                program: '${workspaceFolder}/examples/hello_world/dist/bin/hello_world',
                cwd: '${workspaceFolder}/examples/hello_world',
                preLaunchTask: 'feng: build examples/hello_world'
            }]);
            assert.deepStrictEqual(resolved, {
                type: 'feng',
                request: 'launch',
                name: 'Debug hello_world',
                program: path.join(projectRoot, 'dist', 'bin', 'hello_world'),
                cwd: projectRoot,
                preLaunchTask: 'feng: build examples/hello_world'
            });
            assert.deepStrictEqual(JSON.parse(fs.readFileSync(tasksPath, 'utf8')), {
                version: '2.0.0',
                tasks: [{
                    label: 'feng: build examples/hello_world',
                    type: 'feng',
                    task: 'build',
                    cwd: '${workspaceFolder}/examples/hello_world',
                    group: 'build',
                    problemMatcher: []
                }]
            });
        }

        {
            const { mockVscode, recorder } = createMockVscode({
                workspaceRoot: tempRoot,
                activeTextEditor: createActiveEditor(libSourcePath)
            });
            const extension = loadExtensionModule(mockVscode);
            const provider = extension.__test__.createFengDebugConfigurationProvider(mockVscode);
            const workspaceFolder = createWorkspaceFolder(tempRoot);
            const resolved = await provider.resolveDebugConfiguration(workspaceFolder, {
                type: 'feng',
                request: 'launch'
            });

            assert.strictEqual(resolved, undefined);
            assert.deepStrictEqual(recorder.errorMessages, [
                'Feng debugging currently requires a target: "bin" project when launch.program is omitted.'
            ]);
        }

        {
            const debuggerContribution = Array.isArray(packageJson.contributes.debuggers)
                ? packageJson.contributes.debuggers.find(entry => entry.type === 'feng')
                : null;
            const taskDefinition = Array.isArray(packageJson.contributes.taskDefinitions)
                ? packageJson.contributes.taskDefinitions.find(entry => entry.type === 'feng')
                : null;

            assert(debuggerContribution, 'expected Feng debugger contribution');
            assert.deepStrictEqual(debuggerContribution.languages, ['feng']);
            assert.deepStrictEqual(debuggerContribution.breakpoints, [{ language: 'feng' }]);
            assert(taskDefinition, 'expected Feng task definition');
            assert.deepStrictEqual(taskDefinition.properties.task.enum, ['build']);
        }
    } finally {
        fs.rmSync(tempRoot, { recursive: true, force: true });
    }

    console.log('debug integration tests passed');
}

run().catch(error => {
    console.error(error);
    process.exit(1);
});