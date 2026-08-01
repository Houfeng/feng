# Build and Run

This chapter introduces the build commands used in regular project development.

## Build a Project

```bash
feng build
feng build --release
```

You can pass either a project directory or a `feng.fm` path:

```bash
feng build ./examples/hello_world
feng build ./examples/hello_world/feng.fm
```

Debug builds retain information useful for debugging. `--release` enables release build mode.

## Run a Project

```bash
feng run
feng run --release
feng run -- argument-one argument-two
```

`run` runs only a `target: "bin"` project for the current host platform. Library projects cannot be run directly.

## Target Platforms

If `feng.fm` does not declare `platform`, running `build` without platform options builds for the host platform. You can select platforms explicitly:

```bash
feng build --platform=macos-arm64
feng build --platform=linux-x64-gnu
feng build --platform=linux-arm64-musl
```

You may pass `--platform` more than once. If the manifest declares a platform list, that list serves as both the default target set and the set of allowed targets.

A Linux platform name must include `gnu` or `musl`. A valid target identifier does not guarantee that the current installation contains the required toolchain or SDK.

## Custom Sysroot

A single-platform build can specify a sysroot explicitly:

```bash
feng build --platform=linux-x64-musl --sysroot=/opt/sysroots/musl-x64
```

When `--sysroot` is specified, only one platform can be built at a time. Feng does not download, copy, or license third-party SDKs for you.

## Package a Library

For a `target: "lib"` project, use:

```bash
feng pack
feng pack --platform=macos-arm64 --platform=linux-x64-gnu
```

`pack` always performs a release build and writes an `.fb` file to `<out>/pkg/` after a successful build. Every requested platform must build successfully before the final package is created.

## Clean

```bash
feng clean
```

This command removes all build artifacts associated with the current project manifest.

## Compile Source Files Directly

Third-party build systems and advanced workflows can pass source files directly:

```bash
feng src/main.ff --target=bin --name=myapp --out=build
```

Direct compilation mode does not read `feng.fm` or resolve a dependency graph. Prefer `build`, `run`, and `check` for normal project development.
