# Modules and Visibility

Modules organize source code. `open` and `seal` control the visibility of declarations.

## Declare a Module

`module` must be the first nonempty, non-comment declaration in a file:

```feng
module app.internal;
```

A module is not visible outside its package by default. Declare a public module with:

```feng
open module app.api;
```

One module can span multiple files, and a file path does not determine its module name.

## Import a Module

`import` declarations appear after the module declaration and before other declarations:

```feng
module app;

import std.io;
import app.user;
import app.service as service;
```

Public top-level declarations have three name-access forms:

```feng
import app.user;
import app.service as service;

let first = load();                    // short name after import
let second = service.load();           // alias after import ... as
let third = app.archive.load();        // full module path without import
```

All three forms access the same public declaration. Full module paths apply to public `type`, `enum`, `spec`,
top-level functions, and module-level `let` / `var`; both the module and target declaration must be public.

In expression contexts, a local value takes precedence over the same first segment of a full module path. Use a non-conflicting import alias to access a module hidden by a local value.

Imports are independent in each file. An `import` in another file of the same module does not automatically apply to the current file.

## Three Levels of Visibility

External access to a type member passes through three levels:

1. Module: `seal` by default; write `open module` to make it public.
2. Module member: top-level `type`, `spec`, `enum`, functions, and bindings are `seal` by default; write `open` to make them public.
3. Type member: fields, methods, and constructors are `open` by default; write `seal` to hide them.

```feng
open module app.api.user;

open type User {
  var name: string;
  seal var token: string;

  func display(): string {
    return self.name;
  }
}
```

The signature of a public API cannot expose a type with narrower visibility.

## Seal Members in Object Contracts

Fields and methods in an object-form `spec` are public by default. An explicit `seal` declares a restricted contract member for cooperation between implementing types. It remains part of the complete contract and must be implemented. Both instance and static members support this access control.

These two types satisfy the same contract. `Reader` uses the restricted members of `Counter` through a contract view, while ordinary callers use public entry points:

```feng
module manual_spec_visibility;
import std.io;
import std.numeric;

/** Keeps internal operations in the complete contract. */
spec InternalCounter {
  seal var value: int;

  seal func current(): int;
}

/** Implements the contract with private concrete members. */
type Counter: InternalCounter {
  seal var value: int;

  /** Sets the private counter value. */
  func Counter(value: int) { self.value = value; }

  /** Supplies the restricted contract operation. */
  seal func current(): int { return self.value; }
}

/** Implements the same contract with public concrete members. */
type Reader: InternalCounter {
  var value: int;

  /** Remains public when called through the concrete Reader type. */
  func current(): int { return self.value; }

  /** Uses the contract view of another implementing type. */
  func inspect(other: InternalCounter): int {
    return other.value + other.current();
  }
}

fit Reader {
  /** Uses the target type's contract implementation context. */
  func inspect_from_fit(other: InternalCounter): int {
    return other.current();
  }
}

/** Calls public entry points without accessing sealed contract members. */
func main(args: string[]) {
  let counter = Counter(21);
  let view: InternalCounter = counter;
  let reader = Reader {};
  println<int>("{0} {1} {2}", reader.inspect(view), reader.inspect_from_fit(view),
    reader.current());
}
```

The output is `42 21 0`. Because `Reader` satisfies `InternalCounter`, its instance methods, static methods and fit methods targeting `Reader` may access these seal members through the corresponding contract view. Permission depends on the implementing type at the access site and the spec that originally declared the member. Sharing a module or package alone grants no permission. Inherited seal members retain their original declaring contract; a type satisfying a child contract also satisfies its parents.

In the ordinary top-level function `main`, both `view.value` and `view.current()` are rejected, even when `view` holds a valid implementation. `counter.current()` is also inaccessible because the concrete type `Counter` declares that method seal. `Reader.inspect` cannot bypass that restriction by switching to a concrete `Counter` view either.

By contrast, `reader.current()` is valid because the implementation method on `Reader` is public. After putting that value into an `InternalCounter` view, an ordinary caller still cannot invoke `current()` through that view. The contract view and the concrete type retain their own member visibility. See [Contracts and fit](./contracts-and-fit.md) for how implementation members satisfy public or seal requirements.

To grant access to a concrete type that does not implement the spec, annotate the seal member in the spec with `@friend` and follow the targeted-access rules later in this chapter. That permission applies to the contract view and does not also expose seal members of the concrete implementation.

Direct expansion can also grant a target type's methods access to restricted source members. That permission depends on a direct expansion relationship and the source members' `@mixable` annotations, separately from spec-view access. See [Member Expansion (mixin)](./mixins.md#seal-access-granted-by-direct-expansion) for examples and boundaries.

## Public fit Declarations

`fit` is not a named declaration. Only an `open fit` in a public module can be exported outside its package; any other `fit` applies only within its declaring module.

```feng
open module app.extensions;

open fit User {
  func greeting(): string {
    return "Hello, " + self.name;
  }
}
```

A consumer must import `app.extensions` before using the extension. Adapting an external type to an external contract also follows [the orphan rule](./contracts-and-fit.md#the-orphan-rule-and-package-exports): the relationship works within the package, but `open fit` cannot export it to other packages.

## Avoid Name Conflicts

Invalid duplicate top-level declarations in the same module are reported during declaration checking, even when unused. Valid top-level function overloads are allowed.

An import alias introduces a name in its file, not a top-level module declaration. An alias that duplicates a top-level declaration or another alias in the same file is an error even when unused. This does not include local bindings in nested blocks.

The following alias conflicts are reported only where the name is used; leaving the name unused is allowed:

- An alias duplicates a declaration in another file of the same module.
- An alias duplicates a public name introduced by an import without an alias.

The diagnostic identifies the conflicting sources. This check also applies to the alias in `alias.member`, in both type references and ordinary expressions. Resolution does not select a source based on whether the following member exists, and aliases do not take precedence.

A name introduced by an import without an alias also conflicts lazily with a top-level declaration in the same file or another file of the same module. Top-level declarations do not automatically hide imported names. The local-value precedence over module paths described above still applies.

Names from multiple imports become ambiguous only when the same bare name is actually used. Use an import alias or a full module path to resolve a conflict:

```feng
import app.first as first;
import app.second as second;

let a = first.User {};
let b = app.second.User {};
```

## Granting Access with @friend

`@friend(Type, ...)` grants listed concrete types access to a particular `seal` member. It is useful for restricted factories and collaborating types. This example covers a field, an instance method, a static factory and a same-package `fit`:

```feng
module manual_friend;
import std.io;
import std.numeric;

/** Restricts creation and sensitive state to an auditor. */
type Vault {
  @friend(Auditor)
  seal var code: int;

  /** Initializes the private state. */
  seal func Vault(code: int) { self.code = code; }

  /** Exposes one private operation to the friend. */
  @friend(Auditor)
  seal func read(): int { return self.code; }

  /** Supplies the authorized construction entry. */
  @friend(Auditor)
  seal static func create(code: int): Vault { return Vault(code); }
}

/** Receives member-specific permission. */
type Auditor {
  /** Uses the restricted factory. */
  static func create_vault(): Vault { return Vault.create(41); }

  /** Uses the authorized field and method. */
  func inspect(vault: Vault): int {
    vault.code += 1;
    return vault.read();
  }
}

fit Auditor {
  /** Uses the same friend identity within this package. */
  func snapshot(vault: Vault): int { return vault.code; }
}

/** Uses only the public auditor interface. */
func main(args: string[]) {
  let auditor = Auditor {};
  let vault = Auditor.create_vault();
  println<int>("{0} {1}", auditor.inspect(vault), auditor.snapshot(vault));
}
```

The output is `42 42`. Top-level `main` cannot directly read `vault.code` or call `Vault.create`. The annotation also applies to seal static fields and permitted seal members in `spec` or `fit`. Constructors and finalizers cannot carry it, which is why restricted creation uses the static factory above.

Permission belongs to the marked member and does not transitively extend to a friend’s friends. A same-package `fit` targeting the exact friend type can use it; a fit from another package cannot. Modules and owner types must remain visible, and binding mutability, generic argument identity and other access rules still apply. Friend permission cannot bypass those boundaries.

## Module Bindings and Static Initialization

Initialization is lazy per binding. Importing a module changes name visibility without running all its initializers. Each module binding initializes once when first read or written:

```feng
module manual_lazy_init;
import std.io;
import std.numeric;

var calls: int;

/** Counts evaluation of a module initializer. */
func load(): int { calls += 1; return 10; }

let cached: int = load();

/** Owns separate static storage for each closed type. */
type Cache<T> { static var count: int = 0; }

/** Observes lazy initialization and closed generic static state. */
func main(args: string[]) {
  println<int>("{0}", calls);
  let first = cached;
  let second = cached;
  Cache<int>.count = 2;
  println<int>("{0} {1} {2}", first, second, calls);
  println<int>("{0} {1}", Cache<int>.count, Cache<string>.count);
}
```

The output is `0`, `10 10 1`, and `2 0`. Reading another binding from an initializer triggers that binding along the access path. File order, declaration order and import order do not define initialization order.

Avoid executed dependency cycles. If initializing A reads B and initializing B reads the still-initializing A, current behavior is equivalent to unbounded recursion and eventually exhausts the stack; it does not automatically detect, break or recover from the cycle. Type static bindings follow the same lazy rules. One closed generic type shares one static storage instance, while distinct complete type arguments have independent storage and initialization state.
