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
