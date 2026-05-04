# Feng VS Code Extension

Feng Language provides an out-of-the-box editing experience for Feng in VS Code. After installing the extension, you get syntax highlighting, document formatting, Feng Language Server client integration for source files, dedicated Feng manifest support for `.fm`, and distinct Feng file icons for source, manifest, bundle, and symbol-table files.

## Features

- Syntax highlighting: Covers common Feng keywords, strings, comments, assignment and compound operators, and basic language structures, and highlights Feng manifest sections and `#` comments in `.fm` files.
- Document formatting: Normalizes indentation, whitespace, and common syntax spacing for day-to-day editing, including compound assignment and bitwise shift operators, and aligns manifest values inside `.fm` sections.
- Language Server client: For Feng source files, the extension launches `feng lsp` through the configured Feng executable and connects it using VS Code's standard Language Client. Hover, completion, definition, references, rename, diagnostics, and later language features are now sourced from the Feng LSP capability set exposed by your installed CLI.
- Diagnostics compatibility: If the current Feng CLI does not yet advertise any LSP capability, the extension keeps the existing check-based diagnostics path as a temporary compatibility fallback so open/save validation does not regress.
- Icon support: The extension uses the Feng Logo, and falls back to built-in Feng icons for `.feng`/`.ff` source files, `.fm` manifests, `.fb` bundles, and `.ft` symbol tables when your current file icon theme does not provide a Feng-specific icon.

## Supported File Extensions

- `.feng` and `.ff` for Feng source files
- `.fm` for Feng manifests
- `.fb` for Feng bundles
- `.ft` for Feng symbol tables

## Quick Start

1. Install Feng Language from the VS Code Marketplace.
2. Open any Feng source file and syntax highlighting will be enabled automatically.
3. When you want to clean up code, run VS Code's Format Document command.
4. If you already have the Feng CLI installed, the extension will start `feng lsp` automatically for Feng source files. Language features are then provided by the LSP capabilities exposed by that CLI build.
5. If your current CLI build still exposes an empty LSP capability set, the extension will temporarily keep open/save diagnostics through the legacy `check` path until the server side is filled in.

## Optional Configuration

If the `feng` executable is not available in your system `PATH`, you can configure it explicitly in VS Code settings:

```json
{
  "feng.executablePath": "./build/bin/feng"
}
```

This path can be either an absolute path or a path relative to the first workspace root.

When `feng.executablePath` keeps its default value, the extension runs `feng` directly from your system `PATH`.

## Formatting Behavior

The formatter is designed to cover the most common cleanup tasks in everyday development:

- Adjust indentation around `{}`, `()`, and `[]`
- Remove trailing whitespace at the end of each line
- Normalize line endings to `\n`
- Normalize binary and compound operator spacing, for example `a+b` → `a + b`, `total+=1` → `total += 1`, `mask>>=1` → `mask >>= 1`
- Normalize parameter and argument lists, for example `fn add(a:int,b:int)` → `fn add(a: int, b: int)`
- Normalize spacing around `:`, `,`, and `{}` in object literals and type annotations

For `.fm` manifest files, the formatter additionally:

- Normalizes `[section]` headers
- Normalizes `#` comment spacing
- Aligns `key: "value"` entries within each section so values start in the same column

Its goal is to provide a stable and predictable formatting experience for daily use. It does not currently reflow complex expressions across lines or perform multi-line alignment.

## Language Service

- Feng source files are connected through the standard VS Code Language Client by launching `feng lsp`.
- The default executable name is `feng`, resolved through your system `PATH`.
- If your CLI is not in the default lookup path, set `feng.executablePath` to the correct executable location.
- The extension keeps the built-in formatter and TextMate grammars; only language-service features move behind LSP.
- If the installed CLI currently reports an empty LSP capability set, the extension temporarily preserves the previous open/save diagnostics path by running `feng check --format json <file>` for project files and `feng tool check <file>` for standalone files.
