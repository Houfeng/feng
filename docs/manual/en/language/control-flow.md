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

A binding can be declared only in the initializer clause. The update clause may modify an already visible binding or
call a function, but it cannot declare a new binding with `let` or `var`.

A nonempty initializer runs exactly once when control enters the loop. A binding declared by the initializer remains
the same binding throughout the loop, and a binding declared outside the loop remains the original binding. The
condition, update, and every execution of the body refer to those same bindings.
An initializer binding belongs to the loop-header scope, and the braced body is its child block. A body-local binding
may use the same name and shadows the initializer binding from its declaration onward.

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
before the body begins. The loop variable belongs to the current iteration's header scope, and the braced body is its
child block. A body-local binding may use the same name and shadows the loop variable from its declaration onward.

When each element is a named tuple, the loop can destructure it directly:

```feng
type Entry(string, int);
let apples: Entry = ("apples", 2);
let pears: Entry = ("pears", 3);
let entries: Entry[] = [apples, pears];

for let (name, count) in entries {
  println("{0}: {1}", name, count);
}

for var (name, count) in entries {
  count += 1;
}
```

The `let` or `var` keyword is required and applies to every named position. A pattern must have the same number of
positions as the tuple. An empty position skips that element, for example `for let (name, ) in entries`. Patterns are
one level only: nested tuple patterns, single-position patterns, and type annotations inside a pattern are not
supported. Each nonempty position is a separate per-iteration binding.
Names in one pattern must be distinct.

A `let` loop variable cannot be reassigned. A `var` loop variable can be modified during its iteration, but doing so
does not change the iterated sequence or the next iteration's initial value. A closure captures only the current
iteration's binding: closures created in the same iteration share that binding, while closures from different
iterations do not. If a closure is created before the same iteration's `var` is modified, it observes the modified
value. The same rules apply independently to every nonempty tuple-pattern position.

## Control Transfer

- `break` exits the nearest loop immediately. It does not run the pending iterator clause of a three-clause `for` or
  acquire another element from a `for/in` loop.
- `continue` skips the remaining statements in the current iteration of the nearest loop. A `while` reevaluates its
  condition; a three-clause `for` runs its iterator clause exactly once and then reevaluates its condition; a `for/in`
  loop acquires the next element. Multiple `continue` branches still reach the continuation step only once on any
  executed path.
- `return` ends the current function and can carry a value when required by the function's return type.

Neither `break` nor `continue` accepts a label or nesting count. Loops can be nested, but each statement targets only
the nearest loop and cannot cross multiple nesting levels.

When `if`, `match`, or `try/catch` is used as an expression, each result branch is a control-transfer boundary: a
`break` or `continue` in that branch cannot target a loop outside the expression. A `while`, three-clause `for`, or
`for/in` loop declared inside the branch may still use `break` and `continue`, which target only that inner loop. The
same constructs do not impose this additional boundary when used as statements.
A result branch also cannot use `return` to exit its enclosing function. Ordinary nested control-flow does not change
this rule, while a nested lambda may return from that lambda normally.
