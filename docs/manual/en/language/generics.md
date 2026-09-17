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

## Generic Contracts and Constraint Combinations

All four spec forms can declare type parameters and serve as constraints. This program combines object, callable, union and intersection contracts, and declares a generic parent for `SizedReader<T>`:

```feng
module manual_generic_contracts;
import std.io;
import std.numeric;

/** Reads a value of the chosen type. */
spec Reader<T> { func read(): T; }

/** Extends the explicitly instantiated parent contract. */
spec SizedReader<T>: Reader<T> { func size(): int; }

/** Supplies a label. */
spec Labelled { func label(): string; }

/** Combines two object contracts. */
spec LabelledReader<T>: Reader<T> & Labelled;

/** Produces a value. */
spec Producer<T>(): T;

/** Holds either the chosen type or text. */
spec ValueOrText<T>: T | string;

/** Implements the generic object contracts. */
type Box<T>: SizedReader<T>, Labelled {
  let value: T;

  /** Reads the stored value. */
  func read(): T { return self.value; }

  /** Reports this example's fixed size. */
  func size(): int { return 1; }

  /** Labels the box. */
  func label(): string { return "box"; }
}

/** Calls a value through a callable constraint. */
func produce<F: Producer<int>>(factory: F): int { return factory(); }

/** Uses both parts of an intersection constraint. */
func read_labelled<R: LabelledReader<int>>(reader: R): int {
  println(reader.label());
  return reader.read();
}

/** Narrows a value admitted by a union constraint. */
func number_or_zero<V: ValueOrText<int>>(value: V): int {
  return match value {
    let number: int { number }
    else { 0 }
  };
}

/** Exercises the four forms together. */
func main(args: string[]) {
  let box = Box<int> { value: 7 };
  let child: SizedReader<int> = box;
  let parent: Reader<int> = child;
  let factory: Producer<int> = () -> 5;
  let choice: ValueOrText<int> = "text";
  let read = read_labelled(box);
  println<int>("{0} {1} {2} {3}", read, parent.read(), produce(factory),
    number_or_zero<ValueOrText<int>>(choice));
  println<int>("{0}", number_or_zero<int>(9));
}
```

The output is `box`, `7 7 5 0`, then `9`. Each type parameter has at most one constraint; declare an intersection contract to combine object capabilities. `SizedReader<T>: Reader<T>` uses an instance of the parent contract. The parent list cannot redeclare parameter constraints, and the existing constraints must prove the capabilities required by the parent.

A union constraint admits type arguments that can enter that union, including the complete union itself. Here `V` is respectively the complete `ValueOrText<int>` and `int`; the constraint does not replace them with one common type. The generic body operates on members after binding a narrowed match result. An intersection used as a type argument likewise retains its complete contract view. See [contracts and fit](./contracts-and-fit.md) for static requirements and constrained method values, and [pattern matching](./pattern-matching.md) for union narrowing.

## Self Constraints, Target Inference and Declaration Overloads

A self constraint expresses an operation involving another value of the same type. Unconstrained parameters support storing, copying, passing and returning values. These operations preserve each concrete type's value or reference semantics without requiring extra members.

```feng
module manual_generic_inference;
import std.io;
import std.numeric;

/** Compares values of a chosen type through a named operation. */
spec Equal<T> { func same(other: T): bool; }

/** Satisfies a contract instantiated with its own type. */
type Key: Equal<Key> {
  let id: int;

  /** Compares identifiers. */
  func same(other: Key): bool { return self.id == other.id; }
}

/** Requires comparison with the same actual type. */
func same_key<T: Equal<T>>(left: T, right: T): bool {
  return left.same(right);
}

/** Stores one unconstrained type. */
type Cell<T> { let value: T; }

/** A distinct declaration selected by its two type parameters. */
type Cell<T, U> { let first: T; let second: U; }

/** Produces one type. */
spec Mapper<T>(): T;

/** A separate callable declaration with two type parameters. */
spec Mapper<T, U>(value: T): U;

/** Copies and returns a value using its existing semantics. */
func identity<T>(value: T): T { let copy = value; return copy; }

/** Gets its type parameter from the caller's result type. */
func empty<T>(): T[] { return []; }

/** Demonstrates inference and exact generic arity selection. */
func main(args: string[]) {
  let values: int[] = empty();
  let cell = Cell<int> { value: identity(3) };
  let pair = Cell<int, string> { first: 4, second: "four" };
  let source: Mapper<int> = () -> cell.value;
  let convert: Mapper<int, string> = (value: int) -> pair.second;
  println<int>("{0}", source());
  println(convert(pair.first));
  if same_key(Key { id: 7 }, Key { id: 7 }) { println("same"); }
}
```

The output is `3`, `four`, then `same`. The target `int[]` of `empty()` determines `T = int`. Type construction still requires `Cell<int>`; a target type cannot supply the owning type's omitted arguments.

Within one scope, types of the same category or specs of the same form may share a name when their generic parameter counts differ. Changing only parameter names or constraints at the same count does not create a separate declaration. Multiple inferences for one parameter must agree; inference does not find a common parent contract or insert conversions to reconcile conflicting types. An unconstrained `T` does not support direct member access, equality comparisons or logical operations. Express capabilities such as `same` through constraints; a constraint does not introduce operators either.

Inference for function or method calls differs from forming a function value, where the source type arguments must be explicit; see [function values](./functions.md). See [contracts and fit](./contracts-and-fit.md) for generic fit parameter matching and static extensions.
