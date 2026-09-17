# Error Handling

Feng uses `throw` to raise exceptions, `try/catch` to handle them, and `defer` to schedule scope cleanup.

## Throw an Exception

```feng
func require_positive(value: int) {
  if value <= 0 {
    throw "value must be positive";
  }
}
```

You can throw numeric scalars, `bool`, `string`, named enums, named tuples, and concrete closed user-defined types,
including their supported `@abi` and `@value` forms. Typed `catch` uses the same set of concrete types. Arrays,
values viewed through any `spec`, open generic types, pointers, `void`, and callable values or types cannot be
thrown or used by a typed `catch`.

## Catch an Exception

`try` is followed by an expression, not a statement block, and requires at least one `catch` clause:

```feng
try load_config() catch error: string {
  println("load failed: {0}", error);
} catch {
  println("unknown failure");
}
```

Multiple `catch` clauses are matched in source order. Put concrete type branches first. An anonymous `catch` is the catch-all branch and must be last.
A `catch` name is an immutable header binding, and the following braced body is a child block that may declare a local
binding with the same name. Each `catch` clause has an independent header scope, so different clauses may reuse the
same exception name.

An anonymous `catch` has no exception binding and can rethrow the original exception with `throw;`:

```feng
try run_task() catch {
  throw;
}
```

The nearest enclosing `catch` for `throw;` must be an anonymous `catch` in the current function, method, or lambda.
Ordinary nested blocks can still rethrow; a nested typed `catch` or a new function, method, or lambda cannot reuse
the enclosing anonymous `catch`'s rethrow context. `throw;` is not allowed inside a `defer` block.

## try/catch Expressions

`try/catch` can produce a value:

```feng
let port = try parse_port(text) catch error: string {
  8080;
};
```

The normal path of the `try` operand and every normally completing `catch` must produce a result. A path that returns
from the current function, method, or lambda, or escapes the current `try/catch` through `throw`, produces no result.
When the context provides a target type, every normal result must fit that type. Without a contextual target, Feng
determines a target type from these normal results and requires the remaining normal results to fit it. A `return`
inside a nested lambda returns only from that lambda. An exception not matched by any `catch` continues to the caller.

## defer

`defer` runs when execution leaves the current lexical scope and is useful for paired resource operations:

```feng
let file = File.create(path, FileMode.Read);
defer {
  file.close();
}

let content = file.readText();
```

Multiple `defer` blocks in one scope run in last-in, first-out order. Normal exit, `return`, `break`, `continue`, and exception propagation all trigger cleanup blocks that have already been registered.

## C Boundaries

An exception cannot cross a C ABI boundary. An ABI function must internally handle every exception that could propagate to the boundary. A C function should report errors through return values, error codes, or callback conventions.

## Exact Matching and Cleanup Boundaries

A named catch matches the exact concrete type. An enum is not its underlying integer, and distinct named types do not match merely because their layouts agree. This complete program also shows defer order during nested unwinding and handling an exception from a cleanup function:

```feng
module manual_cleanup;
import std.io;

enum CodeA { Failure = 1 }

enum CodeB { Failure = 1 }

/** Throws one exact named enum type. */
func fail_exact() { throw CodeA.Failure; }

/** Registers cleanups in nested scopes before throwing. */
func work() {
  defer { println("outer"); }
  if true {
    defer { println("inner"); }
    throw "work failed";
  }
}

/** Models a cleanup operation that can fail. */
func close_resource() { throw "close failed"; }

/** Handles a possible close failure before returning to defer. */
func close_safely() {
  try close_resource() catch error: string { println(error); }
}

/** Calls a cleanup helper that handles its own exceptions. */
func cleanup_safely() {
  defer { close_safely(); }
}

/** Observes exact matching and cleanup order. */
func main(args: string[]) {
  try fail_exact() catch error: i32 {
    println("integer");
  } catch error: CodeB {
    println("other enum");
  } catch error: CodeA {
    println("exact");
  }
  try work() catch error: string { println("caught"); }
  cleanup_safely();
}
```

The output is `exact`, `inner`, `outer`, `caught`, and `close failed`. Inner scopes leave before outer scopes. Registered defers run in reverse order within each scope, and managed locals follow normal exit cleanup. A defer that execution never reaches is never registered.

No part of a defer block may directly contain `return`, `throw` or another `defer`. It cannot break or continue an outer loop, although loops created inside it may use their own break or continue. A function called during cleanup can still throw. An exception not handled inside that function interrupts the remaining cleanup statements. The current implementation has an exception-catching limitation for try/catch written directly inside defer. Handle recoverable cleanup failures inside a helper such as `close_safely()` and call that helper from defer, as above.

A runtime panic terminates the process; it is not a Feng exception that catch can handle, and it is not a reliable way to run every defer. See [User-Defined Types](./user-defined-types.md) for resource ownership and finalizer limits.
