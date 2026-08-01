# Quick Start

This chapter introduces the basic workflow for creating, running, and checking a Feng program with a minimal project.

## Create a Project

`feng init` must be run in an empty directory:

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

The command creates the project manifest `feng.fm` and the entry file `src/main.ff`.

## Write the Program

Replace `src/main.ff` with:

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, world!");
}
```

Every source file begins by declaring its module. `import std.io;` brings the standard output function into the current file, and `main(args: string[])` is the entry point of an executable project.

## Run the Program

Run this command in the directory containing `feng.fm`:

```bash
feng run
```

Output:

```text
Hello, world!
```

To pass program arguments, place `--` after the Feng options:

```bash
feng run -- first second
```

The arguments are available in the `args` array passed to `main`.

## Check and Build

Check the entire project without producing a final artifact:

```bash
feng check
```

Create a debug build:

```bash
feng build
```

Create a release build:

```bash
feng build --release
```

Development artifacts are stored by platform under `build/<platform>/`. Executables are placed in `build/<platform>/bin/`.

## Next Step

Read [Your First Project](./first-project.md) to learn how to organize multiple files, then continue with [Values and Bindings](../language/values-and-bindings.md) and the remaining language chapters.
