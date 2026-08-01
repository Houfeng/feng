# Generics

Generics let types and functions be reused with multiple static types.

## Generic Types

```feng
type Box<T> {
  var value: T;

  func get(): T {
    return self.value;
  }
}

let number = Box<int> { value: 42 };
let text = Box<string> { value: "Feng" };
```

Type construction requires explicit type arguments. Feng does not infer a `type` declaration's type parameters from constructor arguments or context.

## Generic Functions

```feng
func identity<T>(value: T): T {
  return value;
}

let first = identity(42);
let second = identity<string>("Feng");
```

At a call site, type arguments can be written explicitly or inferred when the arguments, receiver, or target type determine them uniquely.

## Multiple Type Parameters

```feng
type Pair<T, U> {
  let first: T;
  let second: U;
}

func make_pair<T, U>(first: T, second: U): Pair<T, U> {
  return Pair<T, U> { first: first, second: second };
}
```

## Generic Constraints

A constraint must refer to a `spec`:

```feng
spec Named {
  let name: string;
}

func name_of<T: Named>(value: T): string {
  return value.name;
}
```

An object-contract constraint makes the contract's members directly available in the generic implementation. A callable-contract constraint allows the parameter to be called directly. A union-contract constraint still requires narrowing with `match` first.

## Generic Methods

```feng
type Box<T> {
  let value: T;

  func pair_with<U>(other: U): Pair<T, U> {
    return Pair<T, U> { first: self.value, second: other };
  }
}
```

A method's own type parameters cannot reuse the names of the enclosing type's parameters.

## Invariance

Generic instances are invariant. Even if `Dog` satisfies `Animal`, `Box<Dog>` does not automatically convert to `Box<Animal>`. When such a conversion is needed, iterate explicitly and create a new target container or adapter object.

An unconstrained type parameter provides no members, comparison operations, or logical operations. A generic implementation can use only the basic operations available to every type and the capabilities supplied by its declared constraints.
