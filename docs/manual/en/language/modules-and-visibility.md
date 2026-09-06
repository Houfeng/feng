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

A consumer must import `app.extensions` before using the extension.

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
