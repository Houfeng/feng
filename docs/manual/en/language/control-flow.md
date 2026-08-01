# Control Flow

Feng provides conditional branches, three loop forms, and `break`, `continue`, and `return`. Pattern matching is covered separately in [Pattern Matching](./pattern-matching.md).

## Conditional Branches

```feng
if score >= 90 {
  println("excellent");
} else if score >= 60 {
  println("pass");
} else {
  println("retry");
}
```

A condition must be `bool`. Feng does not implicitly treat a number, string, or object as a Boolean value.

`if` can also be used as an expression:

```feng
let status = if ready {
  "ready";
} else {
  "waiting";
};
```

The expression form requires an `else`, and every normally completing branch must produce a value of the same type.

## while

```feng
var index = 0;
while index < 3 {
  println("{0}", index);
  index += 1;
}
```

The condition is reevaluated before each iteration.

## Three-Clause for

```feng
for var index = 0; index < 10; index += 1 {
  if index == 3 {
    continue;
  }
  if index == 8 {
    break;
  }
  println("{0}", index);
}
```

The initializer, condition, and update clauses can each be omitted. `for ;; { ... }` is an infinite loop.

## for/in

```feng
let values = [10, 20, 30];
var total = 0;

for let value in values {
  total += value;
}
```

`for/in` can iterate over arrays and over standard-library or user-defined iterators that implement the `@iterable` / `@iterator` protocols. The loop variable is a new binding on every iteration.

## Control Transfer

- `break` exits the nearest loop.
- `continue` skips the current iteration of the nearest loop.
- `return` ends the current function and can carry a value when required by the function's return type.

Feng does not support loop labels, so one `break` or `continue` cannot cross multiple nested loops.
