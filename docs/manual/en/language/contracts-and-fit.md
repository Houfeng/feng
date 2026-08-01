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
