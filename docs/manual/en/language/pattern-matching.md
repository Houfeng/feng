# Pattern Matching

`match` supports constant matching and union-type narrowing. It can also be used as an infix operator that returns `bool`.

## Constant Matching

Integers, `string`, `bool`, and `enum` values can be matched by value:

```feng
let label = match status_code {
  200 { "ok" }
  201, 204 { "success" }
  400...499 { "client error" }
  else { "other" }
};
```

Labels in the same match body cannot overlap. An integer range `a...b` is closed and includes both endpoints.

An enum label must use the fully qualified enum case:

```feng
enum State {
  Idle,
  Running,
  Done
}

match state {
  State.Idle { println("idle"); }
  State.Running { println("running"); }
  State.Done { println("done"); }
}
```

## Union Types

A union-form `spec` declares a set of possible member types:

```feng
spec Result: int | string;

let result: Result = "ready";
```

Before narrowing, a union value cannot access members directly or be compared for equality. Use a branch with a binding to obtain the concrete member:

```feng
let message = match result {
  value: int { "code" }
  text: string { text }
};
```

A branch-head binding belongs to that branch's own header scope, and the following braced body is a child block. The
body may declare a local binding with the same name and shadows the header binding from that declaration onward.
Different branches may independently reuse one binding name.

A branch without a binding only tests the member type and does not change the original variable's static type:

```feng
match result {
  int { println("integer"); }
  string { println("text"); }
}
```

## Infix match

`value match pattern` returns `bool`. Separate multiple labels with `|`:

```feng
if code match 200 | 201 | 204 {
  println("success");
}

if score match 0...59 {
  println("retry");
}
```

A union member pattern can bind the narrowed value at the same time:

```feng
if result match text: string && !text.isEmpty() {
  println(text);
}
```

A binding is visible only where a successful match can be guaranteed statically. `&&` carries a binding from its left
side into its right side and branch body; `||`, `!`, and `else` do not propagate bindings. A binding propagated into an
`if` or `while` belongs to the condition-header scope, and the braced body is a child block that may shadow it. Multiple
bindings propagated by one `&&` condition must use distinct names.

## Expression Results

When `match` is an expression, the final expression in every normally completing branch is its result. A branch that
ends with `throw` produces no result and does not participate in result-type checking. The expression form always
requires an `else` branch. When the context provides a target type, every normal branch result must fit that type.
Without a contextual target, Feng determines a target type from the branch results and requires the remaining normal
results to fit it.
