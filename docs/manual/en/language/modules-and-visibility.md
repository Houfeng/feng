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

All three forms access the same public declaration. Full module paths apply to public `type`, `enum`, `spec`, top-level functions, and module-level `let` / `var`; both the module and target declaration must be public. See the [Feng Module Specification](../../specifications/feng-module.md#4-模块导入规则) for the normative rules.

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

Names from multiple imports become ambiguous only when the same bare name is actually used. Use an import alias or a full module path to resolve a conflict:

```feng
import app.first as first;
import app.second as second;

let a = first.User {};
let b = app.second.User {};
```
