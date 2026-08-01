# Project Structure

A Feng project consists of a `feng.fm` manifest, a source directory, and a build output directory.

## Typical Layout

```text
myapp/
├── feng.fm
├── src/
│   ├── main.ff
│   ├── model.ff
│   └── service.ff
└── build/
```

`src/` and `build/` are the defaults and can be changed in the manifest. A source file's path does not determine its module name; the `module` declaration at the beginning of the file does.

## feng.fm

A minimal executable project:

```text
[package]
name: "myapp"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
```

For a minimal library project, change `target` to `"lib"`. A development manifest can contain four sections:

- `[package]`: name, version, target, source directory, output directory, and target platforms.
- `[dependencies]`: exact-version or local-path dependencies.
- `[assets]`: resource directories copied during a build.
- `[registry]`: the package source used by the current project.

The manifest uses its own section-based text syntax. Do not parse it as TOML, YAML, or INI.

## Modules and Files

```feng
open module myapp.user;

open type User {
  let name: string;
}
```

A file can belong to only one module, while a module can span multiple files. Each file independently declares the imports it needs.

Organize files and modules by responsibility, but do not rely on the directory hierarchy for visibility. The actual package boundary is determined by `open module`, top-level `open` declarations, and member visibility.

## Output Directory

The build creates a separate directory for each target platform:

```text
build/<platform>/
├── bin/    # Executable targets
├── lib/    # Library targets
├── mod/    # Symbol tables
├── obj/    # Intermediate objects
└── ...
```

A packaged library is written to `build/pkg/<name>-<version>.fb`. The build directory contains generated files and should not be committed or maintained manually.
