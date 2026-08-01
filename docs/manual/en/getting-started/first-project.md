# Your First Project

This chapter expands the minimal program into an executable project with multiple files and modules.

## Initialize the Project

```bash
mkdir greeting
cd greeting
feng init greeting
```

The initialized project has this core structure:

```text
greeting/
├── feng.fm
└── src/
    └── main.ff
```

## Add a Module

Create `src/greeting.ff`:

```feng
open module greeting.message;

open func make_greeting(name: string): string {
  return "Hello, " + name;
}
```

Update `src/main.ff`:

```feng
module greeting;

import greeting.message;
import std.collections;
import std.io;

func main(args: string[]) {
  let name = if args.length() > 1 {
    args[1];
  } else {
    "Feng";
  };
  println(make_greeting(name));
}
```

Module names and file paths are independent. A module can span multiple `.ff` files, but each file can declare only one module. Both the module and a top-level declaration must be public before another module can import that declaration.

## Run with Arguments

```bash
feng run
feng run -- Alice
```

`args[0]` is the program path, so the first user argument is stored in `args[1]`.

## Project Manifest

The default `feng.fm` for an executable project looks like this:

```text
[package]
name: "greeting"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
```

If the distribution includes packages such as the standard library, `feng init` also writes them to `[dependencies]` with exact versions. Do not edit generated build artifacts. Project inputs consist of `feng.fm` and the source files under `src/`.

## Common Development Loop

```bash
feng check
feng run -- Alice
feng clean
```

Use `check` for quick feedback while editing. `run` builds the project before running it, and `clean` removes every build artifact for the current project. See [Build and Run](../projects/build-and-run.md) for details.
