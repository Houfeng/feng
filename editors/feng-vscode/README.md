# Feng VS Code Extension

Feng Language provides an out-of-the-box editing experience for Feng in VS Code. After installing the extension, you get syntax highlighting, document formatting, diagnostics powered by the Feng CLI, dedicated Feng manifest support for `.fm`, and Feng file icon support.

## Features

- Syntax highlighting: Covers common Feng keywords, strings, comments, assignment and compound operators, and basic language structures, and highlights Feng manifest sections and `#` comments in `.fm` files.
- Document formatting: Normalizes indentation, whitespace, and common syntax spacing for day-to-day editing, including compound assignment and bitwise shift operators, and aligns manifest values inside `.fm` sections.
- Diagnostics: If `feng check` is available on your machine, diagnostics are shown automatically when a file is opened or saved, and are cleared as soon as you start editing.
- Icon support: The extension uses the Feng Logo, and falls back to the built-in Feng file icon when your current file icon theme does not provide a Feng-specific icon.

## Supported File Extensions

- `.feng`、`.ff`、`.fm`、`.fi`

## Quick Start

1. Install Feng Language from the VS Code Marketplace.
2. Open any Feng source file and syntax highlighting will be enabled automatically.
3. When you want to clean up code, run VS Code's Format Document command.
4. If you already have the Feng CLI installed, diagnostics will appear automatically for Feng source files when they are opened, disappear while you edit, and reappear after you save.

## Optional Configuration

If the `feng` executable is not available in your system `PATH`, you can configure it explicitly in VS Code settings:

```json
{
  "feng.executablePath": "./build/bin/feng"
}
```

This path can be either an absolute path or a path relative to the workspace root.

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

## Diagnostics

- Diagnostics depend on the Feng CLI `check` subcommand.
- The default executable name is `feng`.
- If your CLI is not in the default lookup path, set `feng.executablePath` to the correct executable location.
- Diagnostics are triggered when a Feng source file is opened and when it is saved.
- As soon as the document changes, the extension clears existing diagnostics for that file to avoid showing stale red underlines.
