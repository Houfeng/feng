# Checking, Testing, and Debugging

Feng currently separates semantic checking, user tests, and debugging into independent workflows.

## Check a Project

```bash
feng check
feng check --format json
```

`check` always checks the entire resolved project rather than only a single `.ff` file passed on the command line. The path can be a project directory, a `feng.fm` file, or any file within a project; the tool searches upward for the nearest manifest.

Text output is intended for terminals. JSON output is suitable for editors and CI.

## Write a Test Project

The CLI does not currently provide a `feng test` command. Tests can be written as a regular `target: "bin"` project using the `std.test` standard library package:

```feng
module app_test;

import std.test;

func main(args: string[]) {
  describe("math", () {
    test("addition", () {
      assert(1 + 1 == 2, "addition should produce two");
    });
  });
}
```

Run the tests as a regular program:

```bash
feng run
```

`std.test` also provides skip markers and `assertEquals`. A test project can declare the library under test as a local path dependency.

## Debug with VS Code

After installing the Feng VS Code extension, create a Feng launch configuration in **Run and Debug**. The extension invokes `feng dap --stdio` and attempts to infer the program path and build task from the nearest `target: "bin"` manifest.

Local, non-release executable projects on macOS and Linux are currently supported. You can inspect breakpoints, stack frames, and variables. Watch and evaluate expressions support identifiers, member access, constant integer indexing, and simple arithmetic and comparison expressions, but not calls or assignments with side effects.

## Diagnose Problems

When a build fails, investigate in this order:

1. Run `feng check` to obtain syntax and semantic diagnostics.
2. Run `feng deps install --force` to rule out a dependency cache problem.
3. Run `feng clean`, then build again.
4. Use `feng --help` or the relevant command's `--help` to verify its options.

The lexical, syntax, and semantic subcommands under `feng tool` are intended for advanced diagnostics and are not required in a normal project workflow.
