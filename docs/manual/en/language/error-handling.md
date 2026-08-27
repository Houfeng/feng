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
thrown or used by a typed `catch`. See the [exception specification](../../../specifications/feng-exception.md#33-catch)
for the authoritative rules.

## Catch an Exception

`try` is followed by an expression, not a statement block:

```feng
try load_config() catch error: string {
  println("load failed: {0}", error);
} catch {
  println("unknown failure");
};
```

Multiple `catch` clauses are matched in source order. Put concrete type branches first. `catch error: unknown` and an anonymous `catch` are catch-all branches.

An `unknown` binding can only be rethrown; its fields and methods are not accessible:

```feng
try run_task() catch error: unknown {
  throw error;
};
```

## try/catch Expressions

`try/catch` can produce a value:

```feng
let port = try parse_port(text) catch error: string {
  8080;
};
```

The normal path and every normally completing `catch` must produce a result. A path that ends with `throw` produces
no result. When the context provides a target type, every normal result must fit that type. Without a contextual
target, Feng determines a target type from these results and requires the remaining normal results to fit it. Omitting
`catch` creates only an exception propagation point; the exception continues to the caller.

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
