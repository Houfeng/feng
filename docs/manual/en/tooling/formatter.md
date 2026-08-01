# Code Formatting

The Feng CLI does not currently provide `feng fmt`. Production-ready formatting is available through the Feng extension for VS Code.

## Usage

After installing the Feng Language extension, run VS Code's **Format Document** command in a Feng source file or `feng.fm`. You can also enable format on save:

```json
{
  "[feng]": {
    "editor.formatOnSave": true
  },
  "[feng-manifest]": {
    "editor.formatOnSave": true
  }
}
```

## Current Formatting Coverage

The Feng source formatter handles:

- Indentation around braces, parentheses, and brackets.
- Trailing whitespace and line endings.
- Spacing around binary, compound-assignment, and shift operators.
- Common spacing for parameters, arguments, type annotations, commas, and object literals.

The manifest formatter handles:

- `[section]` headings.
- Spacing after `#` in comments.
- Alignment of values in `key: "value"` entries within a section.

The formatter does not automatically rearrange complex expressions and does not replace semantic checking. After formatting, still run:

```bash
feng check
```

Teams should use the same extension version and follow the [Feng Style Guide](../feng-style.md) for naming and structural choices not covered by the formatter.
