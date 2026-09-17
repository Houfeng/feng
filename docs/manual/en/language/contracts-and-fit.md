# Contracts and `fit`

`spec` declares a capability boundary, `type` provides an implementation, and `fit` establishes conformance or adds methods without modifying the original type definition.

## Object Contracts

```feng
spec Named {
  let name: string;

  func display(): string;
}

type User: Named {
  let name: string;

  func display(): string {
    return self.name;
  }
}

func print_name(value: Named) {
  println(value.display());
}
```

A field must match in name, type, and `let`/`var` form. A method must match in name, parameters, and return type. A relationship in the declaration header states that the type author explicitly promises conformance to the contract.

## Contract Views

Once conformance is declared in a `type` header or a currently visible `fit`, a concrete value can be used directly
where that object contract is expected. For an object-form `spec`, the parent relationship is explicitly declared in
its definition, so a child contract can automatically project to its direct or transitive parent view. Establishing
contract views through these declared relationships is not considered an implicit type conversion. Bindings,
assignments, arguments, returns, fields, and array element writes can use the target type to establish the appropriate view:

```feng
spec Profile: Named {}

/** Satisfies both Profile and its parent contract Named. */
type Member: Profile {
  let name: string;

  /** Returns the member's name. */
  func display(): string {
    return self.name;
  }
}

/** Returns the parent contract view of the same value. */
func as_named(value: Profile): Named {
  return value;
}

let member = Member { name: "Alice" };
let profile: Profile = member;
let named: Named = profile;
let parent = as_named(profile);
print_name(member);
print_name(profile);
```

`spec Profile: Named {}` explicitly declares the parent relationship, so `profile` can automatically project to the
`Named` view without writing `(Named)profile`. Writing that cast explicitly establishes the same parent view.
Matching fields or methods alone do not establish conformance, and a runtime object's ability to satisfy another
contract does not automatically make that target type acceptable.

## Callable Contracts

```feng
spec Mapper(value: int): int;

let double: Mapper = (value: int) -> value * 2;
println("{0}", double(21));
```

The callable form describes only a function signature and is suitable for lambdas, top-level functions, and method values. It cannot appear in `type Foo: Mapper` or `fit Foo: Mapper`.

## Union Contracts

```feng
spec Identifier: int | string;

let id: Identifier = "user-42";
let label = match id {
  number: int { "numeric" }
  text: string { text }
  else { "unknown" }
};
```

The union form means “one of these members.” A value must first be narrowed with `match` before it can be used as a concrete member.

## Intersection Contracts

```feng
spec Readable {
  func read(): string;
}

spec Writable {
  func write(value: string): void;
}

spec ReadWrite: Readable & Writable;
```

The intersection form combines multiple object contracts and can be used in type positions or generic constraints. A concrete type should declare conformance to each constituent object contract rather than directly declaring conformance to the intersection contract.

Projection from an intersection contract to a constituent contract still requires an explicit cast. For a `ReadWrite`
value named `value`, write `(Readable)value` to obtain the `Readable` view; assigning `value` directly to a `Readable`
binding is not allowed.

## Satisfy a Contract with fit

```feng
spec DisplayName {
  func display_name(): string;
}

type Account {
  let name: string;
}

fit Account: DisplayName {
  func display_name(): string {
    return self.name;
  }
}
```

`fit` is useful when the original type cannot be modified or when an adaptation should live in a separate module.

## Add Extension Methods with fit

When no target `spec` is listed, `fit` can add methods only:

```feng
fit Account {
  func greeting(): string {
    return "Hello, " + self.name;
  }
}
```

Whether an extension can be used across modules is determined by the visibility of both the module and the `fit`. To export it, use `open fit` in a public module. The relationship and extension methods take effect in the current file only after that module is imported.

## Recommendations

- Put contracts intrinsic to a type in the `type` declaration header.
- Use `fit` for third-party adaptations or capabilities enabled by importing a module.
- Do not treat `spec` as implementation inheritance; it describes only a visible contract.
- Do not rely on implicit structural matching; conformance must be declared explicitly.

## Multiple Parents, Static Capabilities and Seal Requirements

An object contract lists multiple parents with commas; an intersection combines existing contracts with `&`. Access static capabilities through a type name or a constrained type parameter. A `seal` requirement restricts access through the contract view:

```feng
module manual_contract_advanced;
import std.io;
import std.numeric;

spec NamedDevice { func name(): string; }

spec Capacity {
  static let maximum: int;

  static func hint(): int;
}

spec InternalDevice { seal func secret(): int; }

spec DeviceContract: NamedDevice, Capacity, InternalDevice {}

spec DeviceView: NamedDevice & Capacity;

spec Query(): int;

/** Implements all parents and exposes a controlled internal operation. */
type Device: DeviceContract {
  static let maximum: int = 32;

  /** Supplies a static contract requirement. */
  static func hint(): int { return 16; }

  /** Supplies the public name. */
  func name(): string { return "device"; }

  /** Implements a restricted requirement. */
  seal func secret(): int { return 9; }

  /** The implementing type can use its restricted contract view. */
  func inspect(): int {
    let view: InternalDevice = self;
    return view.secret();
  }
}

/** Uses the static capability of a constrained type argument. */
func capacity<T: Capacity>(): int { return T.maximum + T.hint(); }

/** Forms a static method value from the same constrained surface. */
func query<T: Capacity>(): Query { return T.hint; }

/** Projects an intersection member explicitly. */
func main(args: string[]) {
  let device = Device {};
  let view: DeviceView = device;
  let named = (NamedDevice)view;
  let get_hint = query<Device>();
  println(named.name());
  println<int>("{0} {1} {2}", capacity<Device>(), device.inspect(), get_hint());
}
```

The output is `device` and `48 9 16`. Multiple parents combine requirements; same-name signatures must remain compatible, and a parent list cannot hide incompatible members. Intersection projection preserves object identity. Nested intersection members and their object-contract parents can also be reached by explicit projection along declared relationships. Another intersection with merely the same member set is not automatically a conversion target.

A public requirement needs a public implementation. A seal requirement may use a public or seal implementation when the rules permit it. A concrete type's seal members may be selected as implementations only when conformance is declared in the type header or by a fit in the same package as that type. A fit in another package can use the target's public members or its own methods, but cannot select the target's seal members.

Seal members remain required parts of the contract, but ordinary callers cannot access them through that view. The concrete implementation members retain their own visibility. See [Seal Members in Object Contracts](./modules-and-visibility.md#seal-members-in-object-contracts) for cooperation between implementing types through contract views. Static fields must come from the type itself; fit can supply static methods. See [Generics](./generics.md) for combinations and [Modules and Visibility](./modules-and-visibility.md) for targeted seal access.

## Generic Fits and Other Target Types

Fit can adapt generic objects and extend scalars, strings, arrays, named tuples and enums. It adds methods and conformance, not instance fields or static bindings:

```feng
module manual_fit;
import std.io;
import std.numeric;

spec Reader<T> { func read(): T; }

/** A generic payload container. */
type Box<T> { let value: T; }

fit Box<T>: Reader<T> {
  /** Reads the closed payload type. */
  func read(): T { return self.value; }

  /** Adds a static factory without adding storage. */
  static func wrap(value: T): Box<T> { return Box<T> { value: value }; }
}

fit i32 {
  /** Adds one to the scalar receiver. */
  func plus_one(): i32 { return self + 1; }
}

fit string {
  /** Surrounds text with brackets. */
  func tagged(): string { return "[" + self + "]"; }
}

fit int[] {
  /** Adds the array elements. */
  func total(): int {
    var result = 0;
    for let value in self { result += value; }
    return result;
  }
}

/** A named tuple with extension behavior. */
type Coordinates(int, int);

fit Coordinates {
  /** Adds the two tuple elements. */
  func sum(): int { return self.item1 + self.item2; }
}

enum Stage { Idle, Ready }

fit Stage {
  /** Tests one named enum case. */
  func is_ready(): bool { return self == Stage.Ready; }
}

spec HasName { let name: string; }

/** Already provides the required field. */
type NamedRecord { let name: string; }

fit NamedRecord: HasName;

/** Uses each extension through its normal receiver. */
func main(args: string[]) {
  let box = Box<int>.wrap(3);
  let number: i32 = 4;
  let values: int[] = [1, 2, 3];
  let point: Coordinates = (3, 4);
  let named: HasName = NamedRecord { name: "record" };
  println<int>("{0} {1} {2}", box.read(), values.total(), point.sum());
  println<i32>("{0}", number.plus_one());
  println("fit".tagged());
  println(named.name);
  if Stage.Ready.is_ready() { println("ready"); }
}
```

The output is `3 6 7`, `5`, `[fit]`, `record`, and `ready`. The `T` in `fit Box<T>` refers to the target’s existing parameter. It cannot be renamed, reordered, added, removed or replaced with `fit Box<int>` as a specialization. Each closed instance uses its own arguments. The bodyless `fit NamedRecord: HasName;` only declares conformance using existing members.

Visibility depends on the declaring module and `open fit`; consumers import the extension module. Export to other packages also follows the orphan rule below. Being in the same package does not by itself let fit access the target's seal members. See [Modules and Visibility](./modules-and-visibility.md) for targeted permissions and access through spec views.

## The Orphan Rule and Package Exports

For `fit A: B`, the relationship is an orphan adaptation when the implementation sources of both the target type `A` and the contract `B` are outside the current package. The boundary is the package, not the file or module. It does not matter whether `A` and `B` come from the same external package or two different external packages.

An orphan adaptation works inside the current package, subject to ordinary module and import visibility. It cannot export the satisfaction relationship to other packages. Even with `open fit`, the compiler removes its export and emits an informational note, not an error or warning.

| Where `A` and `B` in `fit A: B` are defined | Restricted by the orphan rule? |
| --- | --- |
| `A` is in the current package; `B` is local or external | No; ordinary visibility rules govern export |
| `A` is external; `B` is in the current package | No; ordinary visibility rules govern export |
| Both `A` and `B` are external | Yes; the relationship applies only inside the current package |

This application adapts the standard library's `Rect` to its `Display` contract. Both come from the `std` package, so the relationship remains inside the application package despite the public module and `open fit`:

```feng
open module manual_orphan;
import std;
import std.io;
import std.numeric;
import std.tui.common;

open fit Rect: Display {
  /** Supplies an application-local display policy for a library type. */
  func toString(): string { return "rectangle"; }
}

open fit Rect {
  /** Adds a pure extension without a contract relationship. */
  func area(): int { return self.width * self.height; }
}

/** Uses both extensions inside the declaring package. */
func main(args: string[]) {
  let rect = Rect { width: 3, height: 4 };
  println<Rect>("{0}", rect);
  println<int>("{0}", rect.area());
}
```

The program prints `rectangle` and `12`. During the build, an informational note explains that the orphan adaptation applies only inside the current package. Here `println<Rect>` can use the local `Display` relationship, but another package importing this module does not acquire `Rect: Display`.

A `fit A { ... }` without a contract on the right is a pure method extension and is not subject to the orphan rule. Therefore, the example's `open fit Rect` can export `area()` under ordinary rules. To provide a relationship that other packages can use, define the target type or contract in the current package. See [Modules and Visibility](./modules-and-visibility.md#public-fit-declarations) for public modules and extension visibility.
