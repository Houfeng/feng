# Types

Feng is statically typed. Types are determined at compile time, and conversions between types must be written explicitly.

## Built-in Scalar Types

| Category | Types | Common aliases |
| --- | --- | --- |
| Signed integers | `i8`, `i16`, `i32`, `i64` | `int` |
| Unsigned integers | `u8`, `u16`, `u32`, `u64` | `byte`, `uint` |
| Floating point | `f32`, `f64` | `float`, `double` |
| Boolean | `bool` | — |
| String | `string` | — |

The widths of `int` and `uint` depend on the platform: they map to 32-bit types on 32-bit platforms and 64-bit types on 64-bit platforms. Integer literals infer as `int` by default, and floating-point literals infer as `double`.

```feng
let count: i32 = 42;
let size: uint = (uint)count;
let ratio: f64 = 0.5;
let ready = true;
```

At runtime, integer overflow wraps at the type's bit width. A literal known at compile time to be out of range is rejected.
Integer division or remainder with a right operand of `0` is undefined behavior (UB). For every signed integer type,
`MIN / -1` and `MIN % -1` are also UB, where `MIN` is that type's minimum value. Feng adds no runtime protection for
these cases.

## Strings

`string` is an immutable UTF-8 string with a default value of `""`. Double-quoted strings support `\\`, `\"`, `\n`, `\r`, `\t`, `\0`, and hexadecimal byte escapes. Backtick strings preserve their raw contents.

```feng
let line = "first\nsecond";
let path = `C:\data\feng`;
let message = "Hello, " + "Feng";
```

After importing `std.text`, methods for length, searching, splitting, and case conversion are available. String length is measured in UTF-8 bytes. Use the standard library's rune or grapheme APIs when you need a Unicode code-point or grapheme-cluster view.

## Arrays

`T[]` is a fixed-length array whose current level is read-only. `T[!]` is a fixed-length array whose current level is writable. Arrays are managed reference types: assignment copies the reference, not the elements.

```feng
let values: int[] = [1, 2, 3];
let buffer: byte[!] = byte[:1024];
let matrix: int[!][!] = [[1, 2], [3, 4]];

buffer[0] = (byte)65;
matrix[0][1] = 9;
```

`Type[:length]` creates a writable array of the specified length. An array's length does not change after creation. Use `std.collections.List<T>` when you need a growable collection.

Write permission can be removed only with an explicit conversion:

```feng
let writable: int[!] = [1, 2, 3];
let readonly = (int[])writable;
```

## Named Tuples

Feng has no anonymous tuple types. Declare a named tuple with the parenthesized form:

```feng
type Point(f64, f64);
type Pair<T, U>(T, U);

let origin: Point = (0.0, 0.0);
let item: Pair<int, string> = (1, "one");
println("{0}", origin.item1);
```

Named tuples are value types, and their elements are always immutable. A `var` binding can replace the entire tuple but cannot modify an individual element in place.

## Enumerations

An enumeration defines a distinct named type:

```feng
enum Status {
  Pending,
  Running,
  Done
}

let status = Status.Running;
let raw = (i32)status;
```

An enum's underlying representation is always `i32`, independent of the platform width of `int`. All enum cases must either use implicit incrementing values or specify integer literals in the range `-2147483648...2147483647`; the two forms cannot be mixed. An explicit conversion from `enum` to any integer type is allowed, but an explicit conversion from an integer type to `enum` is not. Implicit conversions in either direction are not allowed.

## User-Defined and Contract Types

Object types, `spec` contracts, union types, and generics are covered in [User-Defined Types](./user-defined-types.md), [Contracts and `fit`](./contracts-and-fit.md), [Pattern Matching](./pattern-matching.md), and [Generics](./generics.md).
