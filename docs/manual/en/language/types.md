# Types

Feng is statically typed. Types are determined at compile time, and conversions between distinct, already determined
types must be written explicitly. Numeric literals can fit a target numeric type. Using an object contract through
declared conformance, or automatically projecting to a parent contract through a declared parent relationship,
establishes a contract view and is not considered an implicit type conversion. See [Contracts and fit](./contracts-and-fit.md)
for explanations and examples.

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

After importing `std.text`, methods such as `length()`, searching, splitting, and case conversion are available. String length is measured in UTF-8 bytes. Use the standard library's rune or grapheme APIs when you need a Unicode code-point or grapheme-cluster view.

## Arrays

`T[]` is a fixed-length array whose current level is read-only. `T[!]` is a fixed-length array whose current level is writable. Arrays are managed reference types: assignment copies the reference, not the elements.

```feng
let values: int[] = [1, 2, 3];
let buffer: byte[!] = byte[:1024];
let matrix: int[!][!] = [[1, 2], [3, 4]];

buffer[0] = (byte)65;
matrix[0][1] = 9;
```

`Type[:length]` creates an array of the specified length and initializes every element to its zero value. Without a
target type, the result is `T[]`, whose current level is read-only; that level is writable only when the explicit target
is `T[!]`. `length` is an integer evaluated exactly once and must be between zero and the target platform's maximum
`int` value. A statically known range error is a compile error; a dynamic range error panics before allocation and is
never truncated or wrapped. An unrepresentable allocation size or insufficient memory also panics. The element type
must statically have a finite default zero value, including for constant-zero and dynamic lengths. An array's own empty
default value and an array literal made from explicit element values do not require an element default zero value.

An array's length does not change after creation, and Feng imposes no fixed array-nesting limit. An index must be in
the range from zero through `length - 1`; a negative or out-of-range index panics before the read or write. Use
`std.collections.List<T>` when you need a growable collection.

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

Named tuples are value types, and their elements are always immutable. A `var` binding can replace the entire tuple
but cannot modify an individual element in place. A new tuple literal must fit a named target type. An existing tuple
value can be copied by value, and a tuple binding without an initializer uses the default zero value of each element.
A tuple is not an object type and has no ordinary constructor; `Pair()`, `Pair { ... }`, and `Pair() { ... }` are invalid.

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

## Numeric Literals and Explicit Conversions

Integers support decimal, hexadecimal `0x`, binary `0b` and octal `0o` notation, with `_` separators between digits. Floating-point literals support an `e` exponent. Numeric suffixes such as `L`, `U` and `F` are unavailable; use a type annotation to select a type.

```feng
module manual_numeric;
import std.io;
import std.numeric;

/** Shows literal notation, conversions and grouping. */
func main(args: string[]) {
  let binary = 0b1111_1111;
  let octal = 0o377;
  let hex = 0xFF;
  let million = 1_000_000;
  let scientific = 1.5e3;
  let fraction = 2.0e-1;
  println<int>("{0} {1} {2} {3}", binary, octal, hex, million);
  println<int>("{0}", (int)scientific);
  let wide: u16 = 300;
  let low = (u8)wide;
  let truncated = (i32)-3.9;
  println<int>("{0} {1}", (int)low, (int)truncated);
  let precise: i32 = 16_777_217;
  let rounded = (f32)precise;
  println<i32>("{0}", (i32)rounded);
  println<int>("{0} {1}", 2 + 3 * 4, (2 + 3) * 4);
  let small: i32 = 7;
  let large: i64 = 7;
  if (i64)small == large { println("same"); }
}
```

The output is `255 255 255 1000000`, `1500`, `44 -3`, `16777216`, `14 20`, then `same`. Narrowing an integer discards high bits; converting a floating-point value to an integer truncates its fractional part. Integer-to-float conversions and floating-point narrowing may lose precision. Writing a cast does not make it lossless. A separator cannot start or end a literal or immediately follow a base prefix.

`bool` is not numeric. Use a Boolean expression such as `value != 0` instead of converting between numbers and bool. To compare values of different established numeric types, explicitly convert to a chosen common type as above. Other named types cannot be compared across types merely because their shapes match; use a supported explicit conversion before comparing values of the same type.

## Operator Precedence

The table runs from highest to lowest precedence. Unary operators associate right to left; the other listed operations associate left to right. Use parentheses to make grouping explicit.

| Precedence | Forms |
| --- | --- |
| 1 | Calls `f(...)`, members `value.member`, indexing `value[index]` |
| 2 | `~x`, `-x`, `!x` |
| 3 | `*`, `/`, `%` |
| 4 | `+`, `-` |
| 5 | `<<`, `>>` |
| 6 | `&` |
| 7 | `^` |
| 8 | `\|` |
| 9 | `<`, `<=`, `>`, `>=` |
| 10 | `==`, `!=` |
| 11 | `&&` |
| 12 | `\|\|` |

Assignment is a statement and does not participate in this expression table. Feng has no `++`, `--` or comma expression. `&&` and `||` short-circuit: the right side runs only when needed.

## Write Permissions at Each Array Layer

Read array suffixes from the inside out. The rightmost suffix controls whether the outer array can replace a row; the preceding suffix controls whether a row can replace an element. `let` only restricts the binding itself.

```feng
module manual_array_layers;
import std.io;
import std.numeric;

/** Modifies independent layers through their declared permissions. */
func main(args: string[]) {
  let rows: int[!][] = [[1, 2], [3, 4]];
  rows[0][0] = 9;
  let slots: int[][!] = [[1, 2], [3, 4]];
  slots[0] = [8, 9];
  let both: int[!][!] = [[1, 2]];
  let readonly_outer = (int[!][])both;
  readonly_outer[0][0] = 7;
  println<int>("{0} {1} {2}", rows[0][0], slots[0][0], both[0][0]);
}
```

The output is `9 8 7`. The cast does not copy the array; both views still share its data.

| Type | Replace a row `a[0] = ...` | Replace an element `a[0][0] = ...` |
| --- | --- | --- |
| `int[][]` | No | No |
| `int[!][]` | No | Yes |
| `int[][!]` | Yes | No |
| `int[!][!]` | Yes | Yes |

Explicit casts can only remove existing write permissions, not grant stronger ones. If an element is an ordinary object, a read-only array element does not freeze that object; its `var` fields remain usable according to their access permissions.

## Empty Tuples, Named Conversions and Enum Behavior

Declare an empty tuple with `type Unit();` and create it with `()` in a target type context. Distinct named tuples allow an explicit cast only when their element counts and positional types match exactly. Both tuples and enums can gain methods through fit:

```feng
module manual_tuple_enum;
import std.io;
import std.numeric;

/** An empty named tuple. */
type Unit();

/** Two distinct named tuples with identical element types. */
type Position(int, int);

type Dimensions(int, int);

/** Adds behavior without adding tuple elements. */
fit Position {
  /** Adds the two coordinates. */
  func total(): int { return self.item1 + self.item2; }
}

/** Uses an explicit value for every member. */
enum Status { Pending = 7, Done = 12 }

/** Adds behavior to the named enum. */
fit Status {
  /** Describes the current enum value. */
  func label(): string {
    if self == Status.Done { return "done"; }
    return "pending";
  }
}

/** Demonstrates construction, conversion and the first-member default. */
func main(args: string[]) {
  let unit: Unit = ();
  let default_unit: Unit;
  let position: Position = (2, 3);
  let dimensions = (Dimensions)position;
  let pending: Status;
  println<int>("{0} {1} {2}", position.total(), dimensions.item1, (int)pending);
  println(Status.Done.label());
}
```

The output is `5 2 7`, then `done`. The default of `Status` is its first member, `Pending`, even though that member's underlying value is nonzero. Explicit enum values must be integer literals in range and cannot be mixed with automatically incremented members. Matching element shapes do not merge the identities of `Position` and `Dimensions` or share their extension methods.

Tuples support zero or two through eight elements, not one; `(value)` is a grouped expression. An empty tuple still has no `Unit()` constructor. See [contracts and fit](./contracts-and-fit.md) for more extension examples.
