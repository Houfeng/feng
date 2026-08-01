# Feng CLI

`feng` provides project workflows, language-service entry points, and advanced compiler diagnostics.

## Global Help

```bash
feng --help
feng --version
feng build --help
```

When an argument is invalid, the CLI explains the problem, prints the relevant usage information, and exits with a nonzero status.

## Everyday Project Commands

| Command | Purpose |
| --- | --- |
| `feng init [name] [--target=bin\|lib]` | Initialize a project in an empty directory |
| `feng check [path] [--format text\|json]` | Check the entire project without producing a final artifact |
| `feng build [path] [--release]` | Build a project |
| `feng run [path] [--release] [-- args...]` | Build and run an executable project |
| `feng clean [path]` | Remove project build artifacts |
| `feng pack [path]` | Create a release build and package a library project |

For most project commands, `path` can be either a project directory or a `feng.fm` file. If omitted, the command uses the manifest in the current directory.

## Dependency Commands

```bash
feng deps add <name> <version-or-path>
feng deps remove <name>
feng deps install
feng deps install --force
```

Feng uses exact versions and does not provide `deps update`. To upgrade a dependency, run `deps add` again with the new version.

## Platform Options

`build` and `pack` accept repeated `--platform=<platform>` options. For a single platform, you can also pass `--sysroot=<path>`. `run` always selects the current host platform and does not accept `--platform`.

## Language and Debug Services

```bash
feng lsp --stdio
feng dap --stdio
```

These entry points are launched by editor clients and do not need to be invoked directly in a regular interactive terminal.

## Advanced Diagnostics

```bash
feng tool lex <file>
feng tool parse <file>
feng tool semantic <file> [more-files...]
feng tool check <file> [more-files...]
feng tool compile <file>
```

`tool` is intended for investigating compiler behavior and integrating third-party tools. For normal projects, prefer `feng check` and `feng build`.

## Commands Not Currently Available

- No `feng test`: tests are regular executable projects run with `feng run`.
- No `feng fmt`: formatting is currently provided by the editor extension.
- No `feng publish`: there is currently no official registry publishing workflow.
- No `feng deps update`: dependency versions are always exact.
