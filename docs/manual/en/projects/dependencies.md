# Dependency Management

Feng projects declare dependencies in the `[dependencies]` section of `feng.fm`.

## Exact-Version Dependencies

```text
[dependencies]
std: "0.1.0"
logging: "1.2.3"
```

Feng currently uses exact versions and does not support version ranges or wildcards. Add a dependency with:

```bash
feng deps add logging 1.2.3
```

Remove a dependency with:

```bash
feng deps remove logging
```

## Local Path Dependencies

```text
[dependencies]
shared: "../shared"
```

A path must begin with `./`, `../`, or `/` and can refer to:

- An `.fb` file.
- A directory containing `feng.fm`.
- A specific `feng.fm` file.

The dependency key must match the target package's `[package].name`. A local project dependency must use `target: "lib"`.

You can also add it from the command line:

```bash
feng deps add shared ../shared
```

## Install Dependencies

```bash
feng deps install
feng deps install --force
```

Installed packages at exact versions are reused from `~/.feng/cache` by default. `--force` reinstalls every dependency declared in the manifest.

`build`, `check`, `run`, and `pack` all perform the same dependency preparation first, so you normally do not need to run `deps install` manually.

## Registry

A project can override the registry:

```text
[registry]
url: "https://packages.example.com/feng"
```

You can also configure a global registry in `~/.feng/config.fm`. The stable path for a remote package is:

```text
<registry>/packages/<name>-<version>.fb
```

If no registry is configured, or if the registry explicitly reports that a package does not exist, the tool checks the bundled `pkg/` directory in the Feng installation.

## Transitive Dependencies and Conflicts

Feng recursively reads dependency manifests from `.fb` files and flattens the dependency graph. The build stops with an error if the same package name resolves to different exact versions, local project dependencies form a cycle, or a dependency key does not match the package found at its path.

When packaging a local library, the `.fb` file does not retain local paths. `pack` replaces each path with the corresponding package's exact version.
