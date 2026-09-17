# Functions

Functions organize behavior and can also be passed as values through callable `spec` types.

## Declaration and Invocation

```feng
func add(a: int, b: int): int {
  return a + b;
}

let sum = add(20, 22);
```

Every parameter must declare a type. A function that returns no value can omit `: void`. A regular function without a declared return type can also infer its return type from consistent `return` paths. Public APIs should declare return types explicitly.

A function whose return type is explicitly declared or inferred as non-`void` must not have a path that reaches the end of its body normally without returning a value:

```feng
func choose(flag: bool): i32 {
  if flag {
    return 1;
  }
  // Error: flag == false reaches the end of the function normally.
}
```

The compiler rejects this function at compile time. A complete `if / else` is valid when every branch returns a value. A `throw` that escapes the function terminates its current path and does not require a following return; if a local `catch` handles it, analysis continues with the outcome of that `catch`. A loop that may execute zero times or exit through `break` does not guarantee a return. When a loop condition is the literal `true` and there is no reachable `break` targeting that loop, code after the loop is unreachable. A non-`void` function may therefore contain a non-terminating `while true {}` because that loop never reaches the end of the function normally.

Functions can be overloaded by name and parameter list. The return type does not distinguish overloads:

```feng
func describe(value: int): string {
  return "integer";
}

func describe(value: string): string {
  return value;
}
```

## Program Entry Point

An executable project must have exactly one top-level entry point:

```feng
func main(args: string[]) {
  // args[0] is the program path
}
```

The entry point's return type is always `void`. In a library project, `main` is an ordinary function and does not become an entry point.

## Variadic Parameters

A variadic parameter is written as `T...` and must be last in the parameter list:

```feng
import std.text;

func join_words(separator: string, words: string...): string {
  return string.join(separator, words);
}

let text = join_words(", ", "Feng", "is", "clear");
```

Inside the function body, the variadic parameter is used as `T[]`.

## Lambdas

A lambda requires a callable `spec` as its target type:

```feng
spec Mapper(value: int): int;

let double: Mapper = (value: int) -> value * 2;
let transform: Mapper = (value: int) {
  let next = value + 1;
  return next * 2;
};
```

Use `->` for a single-expression lambda and a block body for a multiline lambda. A lambda can capture outer bindings. A captured `var` shares the same storage as the outer binding.

```feng
func make_adder(base: int): Mapper {
  return (value: int) -> base + value;
}
```

## Method Values

An object method can be bound as a callable value:

```feng
spec Action(): void;

type Button {
  func click() {
    println("clicked");
  }
}

let button = Button {};
let action: Action = button.click;
action();
```

A method value retains the original object as `self`. If the method is overloaded, the explicit target `spec` must identify a single overload.

## Forwarding Variadic Arguments

Use `...items` to pass an existing array as the entire variadic argument array. This example covers ordinary packing, prepacked forwarding and a variadic callable contract:

```feng
module manual_variadic;
import std.io;
import std.numeric;

spec Sum(values: int...): int;

/** Adds an ordinary variadic argument array. */
func sum(values: int...): int {
  var total = 0;
  for let value in values { total += value; }
  return total;
}

/** Forwards the existing array without packing another one. */
func forward(values: int...): int { return sum(...values); }

/** Uses direct calls and a variadic callable value. */
func main(args: string[]) {
  let items: int[] = [1, 2, 3];
  let writable: int[!] = [7, 8];
  let readonly = (int[])writable;
  let operation: Sum = sum;
  println<int>("{0} {1} {2}", sum(...items), forward(4, 5), operation(...readonly));
}
```

The output is `6 9 15`. `...items` must be last and occupy the whole variadic portion. Only fixed arguments may precede it, so `sum(1, ...items)` is invalid. It is not a general array-spread operator.

The target must declare a variadic parameter, and the forwarded array must match its read-only `T[]` shape. A `T[!]` cannot be forwarded directly; the example explicitly removes write permission first. Forwarding preserves the same array without copying or repacking elements. A callable parameter declared `T...` is not the same signature as an ordinary `T[]`, including for binding and explicit conversion.

## Callable Sources, Conversions and Defaults

Callable values can come from top-level functions, concrete static or instance methods, and methods on object-contract views. Forming a value from a generic function or method requires complete explicit type arguments first:

```feng
module manual_callable;
import std.io;
import std.numeric;

spec Calculate(value: int): int;

spec OtherCalculate(value: int): int;

spec Read(): int;

spec Readable { func read(): int; }

/** Supplies instance and static callable sources. */
type Reader: Readable {
  let value: int;

  /** Reads the retained receiver. */
  func read(): int { return self.value; }

  /** Doubles a value without capturing an instance. */
  static func twice(value: int): int { return value * 2; }
}

/** Supplies a top-level callable source. */
func increment(value: int): int { return value + 1; }

/** Must be explicitly closed before it becomes a callable value. */
func identity<T>(value: T): T { return value; }

/** Exercises each source and a default callable. */
func main(args: string[]) {
  let top: Calculate = increment;
  let generic: Calculate = identity<int>;
  let method: Calculate = Reader.twice;
  let receiver: Readable = Reader { value: 7 };
  let read: Read = receiver.read;
  let converted = (OtherCalculate)method;
  let empty: Calculate;
  println<int>("{0} {1} {2} {3} {4}", top(1), generic(3), read(), converted(4), empty(9));
}
```

The output is `2 3 7 8 0`. Distinct callable specs cannot be directly assigned to each other even when their signatures match. An explicit conversion is allowed when parameter order, types, variadic shape and result type all match. Default callables are safe to invoke: a void callable does nothing, and a non-void callable returns its result type’s default.

Write generic sources as `identity<int>`, `object.method<int>` or `Type.method<int>`; the target callable does not infer source type parameters. Close a generic owner too, as in `Box<int>.method<string>`. When overloads exist, the target callable signature must select one source uniquely.

An instance method value retains the receiver selected when it is formed. Value receivers are copied, while reference receivers retain the same object. See [User-Defined Types](./user-defined-types.md) for examples and state changes across repeated calls.
