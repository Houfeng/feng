# User-Defined Types

`type` organizes data and behavior into a named type. Object types are managed reference types; named tuples are value types.

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

When no constructor is declared explicitly, the default parameterless constructor and object literals are available. An object literal can also override members after construction while those members remain bindable or writable.

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

## Named Tuples and Value Types

The parenthesized form declares a named tuple. `@value` can declare an object type with value semantics. Their copying and lifetime rules differ from those of regular object types.
