# Feng

**English** · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português](README.pt-BR.md) · [日本語](README.ja.md) · [Русский](README.ru.md) · [Deutsch](README.de.md) · [Français](README.fr.md)

Feng is a statically typed, compiled programming language with concise syntax, explicit contracts, and automatic memory management. Its name comes from the Chinese character “锋”, meaning “sharp”.

[Website](https://feng-lang.com) · [User manual](docs/manual/en/README.md) · [Releases](https://github.com/Houfeng/feng/releases)

## Features

- **Strong, static typing**: Type inference, generics, closures, and pattern matching.
- **Explicit contracts**: Declare contracts with `spec`; use `fit` to adapt types to contracts or add extension members.
- **Automatic memory management**: Automatic reference counting (ARC) and cycle collection manage the lifetime of managed objects.
- **C interoperability**: Call C libraries through `extern func` and ABI annotations.
- **Developer tools**: Project builds, dependency management, language services, debugging, and extensions for VS Code and Zed.

## Install

Official packages support Apple Silicon Macs and GNU/Linux hosts on x86-64 and ARM64.

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

After installation, open a new terminal and run `feng --version` to verify the installation. See the [installation guide](docs/manual/en/getting-started/installation.md) for version selection and manual installation.

## Quick start

Create a project in an empty directory:

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

Replace the generated `src/main.ff` with:

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

Run the program:

```bash
feng run
```

The output is `Hello, Feng!`. Use `feng check` to check the project and `feng build --release` to build a release version. Continue with [Your first project](docs/manual/en/getting-started/first-project.md).

## Documentation

- [Language and tooling specifications](docs/specifications/README.md) (Chinese)
- [Standard library guide](docs/manual/en/standard-library/README.md)
- Editor support: [VS Code](editors/feng-vscode/README.md), [Zed](editors/feng-zed/README.md) (Chinese)
- [Engineering documentation](docs/engineering/README.md) (Chinese)

## Build from source

Install Clang, Make, and Git LFS. macOS also requires Xcode Command Line Tools.

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

The compiler is available at `build/bin/feng`. Run `make test` for the full regression suite: `test/` covers the compiler and runtime, while `fcts/` covers language behavior.

## License

[MIT](LICENSE)
