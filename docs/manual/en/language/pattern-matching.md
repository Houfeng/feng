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
  else { "unknown" }
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

When `match` is an expression, every normally completing path must reach the final result expression of its branch.
A path that uses `return` to leave the current function, method, or lambda, or escapes through `throw`, produces no result and
does not participate in result-type checking. The expression form always requires an `else` branch, even when all union
members are listed. A standalone `match` statement may omit `else`. When the context
provides a target type, every normal result must fit that type. Without a contextual target, Feng determines a target
type from the normal results and requires the remaining normal results to fit it.

## Nested Unions, Chained Patterns and Subset Bindings

Unions preserve their declared nesting. `A -> B` first matches direct member `A`, then its direct member `B`. A bound chained pattern obtains the final member value:

```feng
module manual_nested_union;
import std.io;
import std.numeric;

spec TextOrCode: int | string;

spec Reply: TextOrCode | bool;

spec Flat: int | string | bool;

/** Reads a nested member using its declared path. */
func describe(value: Reply): string {
  return match value {
    var code: TextOrCode -> int { code += 1; code.toString(); }
    let text: TextOrCode -> string { text; }
    bool { "flag"; }
    else { "other"; }
  };
}

/** Narrows a subset once more before using its concrete member. */
func describe_subset(value: Flat): string {
  return match value {
    let selected: int, string {
      let text = match selected {
        let code: int { code.toString(); }
        let word: string { word; }
        else { "other"; }
      };
      text;
    }
    bool { "flag"; }
    else { "other"; }
  };
}

/** Uses both an entered value and the first-member default. */
func main(args: string[]) {
  let reply: Reply = 7;
  let empty: Reply;
  println("{0} {1} {2}", describe(reply), describe(empty), describe_subset("hello"));
}
```

The output is `8 1 hello`. `var code` can be reassigned without changing the original union’s active member to a different type. Omitting the modifier or writing `let` creates an immutable binding. A comma-grouped branch still yields a union subset, which needs further narrowing before concrete member operations. Every chain edge must be a direct member edge; you cannot skip `TextOrCode` and match `int` directly.

A union without an initializer uses the default of its first direct member, recursively when that member is another union. Thus `empty` ultimately holds the integer zero. Expression-form match still requires `else`.

Union entry searches the entire member graph for an exact type first, then for a member that accepts conformance or target fitting. Each pass is breadth-first and selects the first match in declaration order at the same depth. A deep exact match therefore beats shallow contract conformance, and multiple valid paths alone are not ambiguous. To choose a nested member, explicitly form that member value before entering the outer union. Copying the same complete union type preserves its active member.

## Object and Callable Contracts as Union Members

Form a callable value explicitly before entering a union to combine data and behavior in one result type. After narrowing, use the selected member’s capabilities:

```feng
module manual_union_contracts;
import std.io;
import std.numeric;

spec Named { func display(): string; }

spec Action(): string;

spec Result: Named | Action | int;

/** Supplies a nominal object-contract member. */
type Message: Named {
  /** Returns the object member's text. */
  func display(): string { return "object"; }
}

/** Uses the stored member rather than testing the runtime object type. */
func render(result: Result): string {
  return match result {
    let named: Named { named.display(); }
    let action: Action { action(); }
    let code: int { code.toString(); }
    else { "other"; }
  };
}

/** Creates both contract member forms. */
func main(args: string[]) {
  let object: Result = Message {};
  let callback: Action = () -> "callable";
  let function: Result = callback;
  println("{0} {1}", render(object), render(function));
}
```

The output is `object callable`. The `Named` branch matches only when the active member is `Named`. If the union also lists concrete `Message` and a value enters through that concrete member, its conformance does not make it match the `Named` branch. An object-contract view cannot then be narrowed downward to its runtime implementation type.
