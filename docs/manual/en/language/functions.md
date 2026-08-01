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
