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

The expression form requires an `else`, and every normally completing branch must produce a value. When the context
provides a target type, every branch result must fit that type. Without a contextual target, Feng determines a target
type from the branch results and requires the remaining results to fit it.

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

A nonempty initializer runs exactly once when control enters the loop. A binding declared by the initializer remains
the same binding throughout the loop, and a binding declared outside the loop remains the original binding. The
condition, update, and every execution of the body refer to those same bindings.

Each entry into the body is a new execution of that block. Whenever a body-local declaration is executed, it creates
a new binding. Closures created in different iterations therefore share a captured outer or initializer binding, but
capture separate body-local bindings for their respective iterations.

## for/in

```feng
let values = [10, 20, 30];
var total = 0;

for let value in values {
  total += value;
}
```

`for/in` can iterate over arrays and over standard-library or user-defined iterators that implement the `@iterable` /
`@iterator` protocols. Before each entry into the body, Feng creates a new loop-variable binding initialized with the
current element. This is equivalent to executing the corresponding local declaration once per iteration immediately
before the body begins.

A `let` loop variable cannot be reassigned. A `var` loop variable can be modified during its iteration, but doing so
does not change the iterated sequence or the next iteration's initial value. A closure captures only the current
iteration's binding: closures created in the same iteration share that binding, while closures from different
iterations do not. If a closure is created before the same iteration's `var` is modified, it observes the modified
value.

## Control Transfer

- `break` exits the nearest loop.
- `continue` skips the current iteration of the nearest loop.
- `return` ends the current function and can carry a value when required by the function's return type.

Feng does not support loop labels, so one `break` or `continue` cannot cross multiple nested loops.
