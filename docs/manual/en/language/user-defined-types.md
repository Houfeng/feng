# User-Defined Types

`type` organizes data and behavior into a named type. Regular object types are managed reference types; named tuples and object types marked with `@value` are value types.

## Fields and Object Literals

```feng
type User {
  let id: int;
  var name: string;
}

let user = User { id: 1, name: "Alice" };
user.name = "Bob";
```

A `let` field cannot be changed after initialization is complete, while a `var` field can. Object assignment copies a reference:

```feng
let alias = user;
alias.name = "Carol";
println(user.name); // Carol
```

## Constructors

A constructor has the same name as its type and initializes the current object through `self`:

```feng
type User {
  let id: int;
  var name: string;

  func User(id: int, name: string) {
    self.id = id;
    self.name = name;
  }
}

let user = User(1, "Alice");
let renamed = User(2, "Bob") { name: "Carol" };
```

For a braced object type with no explicitly declared constructor, the default parameterless constructor and object
literals are available. An object literal can also override members after construction while those members remain
bindable or writable. This rule does not apply to parenthesized named tuples, which have no ordinary constructor.

## Methods

```feng
type Counter {
  var value: int;

  func increment() {
    self.value += 1;
  }

  func current(): int {
    return self.value;
  }
}
```

An instance method accesses the current instance through `self`. Methods can be overloaded, but the return type alone cannot distinguish overloads.

## Static Members

```feng
type Counter {
  open static var created: int = 0;

  open static func create(): Counter {
    Counter.created += 1;
    return Counter {};
  }

  var value: int;
}

let counter = Counter.create();
```

Access a static member through the type name, not through an instance.

## Finalizers

Define a finalizer when an object must release an external resource:

```feng
type Resource {
  var handle: int;

  func Resource(handle: int) {
    self.handle = handle;
  }

  func ~Resource() {
    // Release the external resource represented by handle
  }
}
```

Feng manages memory automatically. Finalizers are suitable for unmanaged resources such as file handles. Prefer `defer` for predictable lexical cleanup; see [Error Handling](./error-handling.md).

## Value Types and Method Capture

Mark a braced type with `@value` to copy its value across assignment, argument and return boundaries. This complete program shows independent value fields, shared reference fields, and the receiver copy saved by a method value:

```feng
module manual_value;
import std.io;
import std.numeric;

/** Shared reference stored inside a value. */
type SharedCount { var count: int; }

/** Mutable value with a shared reference field. */
@value
type Position {
  var x: int;
  let shared: SharedCount;

  /** Changes the current receiver storage. */
  func add(delta: int): int {
    self.x += delta;
    return self.x;
  }
}

spec Step(delta: int): int;

/** Changes a parameter copy and returns its value. */
func shifted(value: Position): Position {
  value.add(5);
  return value;
}

/** Prints the observable copy boundaries. */
func main(args: string[]) {
  let original = Position { x: 1, shared: SharedCount {} };
  let copy = original;
  copy.add(2);
  copy.shared.count = 9;
  let moved = shifted(original);
  let step: Step = original.add;
  let first = step(2);
  let second = step(1);
  println<int>("{0} {1} {2}", original.x, copy.x, moved.x);
  println<int>("{0}", original.shared.count);
  println<int>("{0} {1} {2}", first, second, original.x);
}
```

The output is `1 3 6`, then `9`, then `3 4 1`. A direct instance call makes `self` refer to the current receiver storage without another copy. Forming `original.add` saves a value copy, and later calls keep using that copy. A method value formed from a reference object saves the object reference. See [Functions](./functions.md) for more callable examples.

| Form | Copy behavior inside Feng | Main consideration |
| --- | --- | --- |
| Ordinary `type` | Copies a reference | Multiple bindings access the same object |
| `@value type` | Copies field values | Reference fields still share their objects; this is not a deep copy |
| Named tuple | Copies element values | Elements cannot be mutated in place; replace the whole tuple |
| `@abi type` | Still copies an object reference | Constrains C ABI layout, not Feng value semantics |

See [Types](./types.md) for tuple construction and [C Interoperability](../interop/c-interop.md) for ABI arguments.

## Member Expansion and @mixable

Member expansion adds source fields and reusable behavior to a target type. It does not establish inheritance or a conversion between the source and target. The target declares its own contract conformance. The three forms differ in whether they construct a source value first:

```feng
module manual_mixin;
import std.io;
import std.numeric;

spec Counted { var value: int; }

/** Supplies reusable fields and behavior. */
type CounterPart: Counted {
  var value: int = 4;
  @mixable
  seal var hidden: int = 5;

  /** Initializes the source instance. */
  func CounterPart(seed: int) { self.value = seed; }

  /** Becomes a callable instance operation on each participating type. */
  @mixable
  static func bump(target: Counted): int {
    target.value += 1;
    return target.value;
  }

  /** Supplies a restricted reusable operation. */
  @mixable
  seal static func secret(target: Counted): int { return target.value + 100; }
}

/** Expands fields with their type defaults. */
type ZeroCounter: Counted { ...: CounterPart; }

/** Constructs one source value before copying its selected fields. */
type SeededCounter: Counted {
  ...: CounterPart = CounterPart(10);

  /** Reads the generated seal field from its owning type. */
  func hidden_value(): int { return self.hidden; }

  /** Uses the generated seal method inside its owning type. */
  func secret_value(): int { return self.secret(); }
}

/** Infers the source type from its construction expression. */
type InferredCounter: Counted { ... = CounterPart(20); }

/** Compares all three initialization forms. */
func main(args: string[]) {
  let zero = ZeroCounter {};
  let seeded = SeededCounter {};
  let inferred = InferredCounter {};
  println<int>("{0} {1} {2}", zero.value, seeded.value, inferred.value);
  println<int>("{0} {1} {2}", zero.bump(), seeded.hidden_value(), seeded.secret_value());
}
```

The output is `0 10 20` and `1 5 110`. `...: Source;` initializes each generated field with its type default and does not run source field initializers or constructors. Both forms with `= Source(...)` construct one source per target construction and use its final field values. The right side must be an object construction expression, not an arbitrary variable or factory call. Reference fields copy references; generated slots do not stay synchronized with source slots.

Public instance fields participate automatically; ordinary instance methods, static fields and unmarked methods do not. A reusable `@mixable` static method must take an object contract as its first parameter, with conformance declared by both source and target. It also supplies an instance call form that omits that parameter. Explicit target members take precedence. Separate expansion sources have no priority: ordinary member conflicts and expansion cycles are errors.

Expanded `@mixable seal` fields and methods remain `seal`. The direct target’s own instance and static methods may use the corresponding restricted source capabilities. This permission does not extend to top-level functions, other types or indirect targets. Source-field permission does not apply to target `fit` methods, constructors or ordinary field initializers; generated expansion initialization is handled by the compiler. Method permission covers only marked seal static methods, not other seal members.

## Defaults and Initialization Order

A binding without an initializer uses the type default; it does not call a parameterless constructor. Object construction runs field initializers, then the constructor, then the object literal:

```feng
module manual_initialization;
import std.io;
import std.numeric;

/** Exposes constructor and literal initialization separately. */
type Record {
  let id: int;
  var stage: int = 1;

  /** Binds id once and advances the writable field. */
  func Record(id: int) { self.id = id; self.stage = 2; }
}

/** Compares default values with ordinary construction. */
func main(args: string[]) {
  let zero: Record;
  let ready = Record(7) { stage: 3 };
  println<int>("{0} {1}", zero.id, zero.stage);
  println<int>("{0} {1}", ready.id, ready.stage);
}
```

The output is `0 0` and `7 3`. A `let` field can receive only one final explicit binding across the three construction stages, so the object literal above cannot also specify `id: 8`. A field without a declaration initializer first receives its type default; this does not consume its first explicit binding. After construction, even a `let` left at its default cannot be reassigned.

Object defaults recursively initialize fields and must have a finite expansion. Self-referencing or mutually referencing objects cannot directly be default-value targets. Later constructor or object-literal assignments do not waive an invalid first-stage default; a field declaration initializer can supply a value at that stage. Arrays default to empty arrays, tuples default element by element, enums use the first case, and unions use the default of their first direct member. See [Types](./types.md) and [Pattern Matching](./pattern-matching.md).

## Memory and Resource Lifetimes

Ordinary objects, strings, arrays and closures use automatic reference counting. Strong references held by bindings, fields and closures keep their objects alive. Returning a closure can keep a captured local binding alive after its creating function returns:

```feng
module manual_lifetime;
import std.io;
import std.numeric;

/** Reference object kept alive by a closure. */
type Label { var text: string; }

spec ReadLabel(): string;

/** Returns a closure that retains access to a local binding. */
func make_reader(): ReadLabel {
  let label = Label { text: "still alive" };
  return () -> label.text;
}

/** Uses the captured object after make_reader returns. */
func main(args: string[]) {
  let reader = make_reader();
  println(reader());
}
```

The output is `still alive`. Closures capturing a `var` share that binding with outer code; receiver capture by method values follows the value-type example above. Ordinary objects enter release when their last strong reference disappears. A cycle collector reclaims unreachable reference cycles, so do not depend on the exact release time or finalizer order within a cycle.

Managed memory is separate from external resources such as file handles, sockets and C allocations. Provide an explicit `close` or release operation and register `defer` immediately after acquiring a resource. A finalizer can cover missed cleanup, and cleanup should handle an already-closed resource. Finalizers cannot be called directly and must catch their own exceptions. Do not depend on a fixed execution thread, other objects in a cycle remaining unfinalized, or retaining `self` to repeatedly postpone release.

A `T*` borrowed by C does not itself retain a managed object. Keep its owner alive for the whole period of pointer use instead of retaining only the pointer. See [C Interoperability](../interop/c-interop.md) for code and [Error Handling](./error-handling.md) for scope cleanup and throwing boundaries.
