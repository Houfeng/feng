# Editor Support

The official Feng extension for VS Code provides syntax highlighting, formatting, language services, and debugging integration.

## Install the Extension

Install **Feng Language** from the [Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=houfeng.feng-language).

The extension recognizes:

- `.feng` and `.ff`: Feng source files.
- `.fm`: Feng project or package manifests.
- `.fb`: Feng Bundles.
- `.ft`: Feng symbol tables.

## Documentation Comments

When you type `/**` in a Feng source file, the extension automatically inserts a documentation-comment closing delimiter with a leading space. Pressing Enter then expands and aligns the standard multiline documentation-comment structure:

```feng
/**
 *
 */
```

## CLI Path

By default, the extension launches `feng` from `PATH`. If the command is not in `PATH`, specify it in the VS Code settings:

```json
{
  "feng.executablePath": "/absolute/path/to/feng"
}
```

You can also use a path relative to the first workspace root:

```json
{
  "feng.executablePath": "./build/bin/feng"
}
```

This setting is used for both `feng lsp` and `feng dap`.

## Language Services

The extension starts `feng lsp` when you open a Feng file. The language server currently supports:

- Diagnostics.
- Hover information.
- Code completion.
- Go to definition.
- Find references.
- Rename.

After replacing or rebuilding the CLI, run **Feng: Restart Language Server** from the command palette, or select the Feng LSP status item in the status bar.

## Debugging

Create a Feng configuration in the **Run and Debug** view. The extension creates a build task for an executable project and starts the debug session with `feng dap --stdio`.

When generating a configuration, the extension first infers `program`, the working directory, and the build task from the `feng.fm` nearest to the current file. For manual configuration, the essential fields are:

```json
{
  "type": "feng",
  "request": "launch",
  "name": "Debug Feng",
  "program": "${workspaceFolder}/build/macos-arm64/bin/myapp",
  "cwd": "${workspaceFolder}",
  "args": []
}
```

The platform directory in `program` must match the platform actually built on the local machine. Debugging currently uses local, non-release builds.
