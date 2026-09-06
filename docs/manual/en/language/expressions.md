# Expressions

Expressions produce values. Statements bind or assign values and control execution.

## Literals and Construction

```feng
let integer = 42;
let decimal = 3.14;
let enabled = true;
let text = "Feng";
let items = [1, 2, 3];
let user = User { name: "Alice" };
```

Create an array with `Type[:length]` and access an element with `value[index]`:

```feng
let bytes: byte[!] = byte[:256];
bytes[0] = (byte)65;
```

`length` is an integer evaluated exactly once and must be between zero and the target platform's maximum `int` value.
A statically known invalid value is a compile error; a dynamic invalid value panics before allocation. An array index
is also evaluated exactly once, and a negative or out-of-range value panics before the element is read or written.
Without an explicit array target type, `Type[:length]` produces `T[]`, whose current level is read-only. The element
type must have a finite default zero value at compile time, even when `length` is the constant `0` or a runtime integer.

## Arithmetic and Comparison

Feng provides the usual arithmetic, relational, equality, logical, bitwise, and shift operators:

```feng
let total = price * count + shipping;
let in_range = score >= 60 && score <= 100;
let same = left == right;
let masked = flags & 0xff;
```

The two operands must usually have the same static type. A numeric literal can adapt to an established numeric target type, but values already bound with different numeric types require an explicit conversion.

Logical `&&` and `||` use short-circuit evaluation. Function arguments and binary operands are evaluated from left to right.

## Explicit Conversions

A conversion is written as `(TargetType)expression`:

```feng
let small: i32 = 42;
let wide = (i64)small;
let ratio = (f64)small / 100.0;
```

Explicit syntax does not imply that every pair of types is convertible. The source and target type rules determine whether a conversion is valid.

## Assignment and Compound Assignment

```feng
var count = 0;
count = count + 1;
count += 2;
count *= 3;

var mask: i32 = 1;
mask <<= 3;
```

An assignment target must be writable: a `var` binding, a `var` member, or an element at a writable array level. A compound assignment evaluates its left-hand target only once.

## Branch Expressions

`if`, `match`, and `try/catch` can all produce values:

```feng
let label = if score >= 60 {
  "pass";
} else {
  "fail";
};

let category = match code {
  200 { "ok" }
  400...499 { "client error" }
  else { "other" }
};
```

Every normally completing path must produce a result, and the final reachable expression is the value of its block.
A path that returns from the current function, method, or lambda, or escapes through `throw`, produces no result.
When the context provides a target type, every normal result must fit that type. Without a contextual target, Feng
determines a target type from the normal results and requires the remaining normal results to fit it. An expression
after an unconditional `return` or `throw` is unreachable and is not a block result. See
[Control Flow](./control-flow.md), [Pattern Matching](./pattern-matching.md), and
[Error Handling](./error-handling.md) for the syntax of each expression form.
