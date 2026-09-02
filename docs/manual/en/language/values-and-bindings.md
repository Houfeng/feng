# Values and Bindings

Feng uses `let` for bindings that cannot be reassigned and `var` for bindings that can.

## Declare a Binding

When an initial value is present, the type can be omitted:

```feng
let language = "Feng";
var count = 0;
count += 1;
```

Without an initial value, the type is required. Feng initializes the binding with that type's zero value:

```feng
let retries: int;    // 0
let enabled: bool;   // false
let message: string; // ""
let values: int[];   // []
```

Prefer `let`; use `var` only when reassignment is genuinely required.

## Binding and Object Mutability

Whether a binding can be reassigned and whether an object's members can be modified are separate rules:

```feng
type User {
  var name: string;
}

let user = User { name: "Alice" };
user.name = "Bob";        // Valid: name is a var member
// user = User {};         // Invalid: user is a let binding
```

Arrays have an independent element-writability marker. `T[]` is read-only at its current level, while `T[!]` is writable at its current level:

```feng
let readonly = [1, 2, 3];
let writable: int[!] = [1, 2, 3];
writable[0] = 10;
// readonly[0] = 10;       // Invalid
```

## Parameter Bindings

Every parameter must declare a type. A parameter is not reassignable by default when neither `let` nor `var` is written:

```feng
func add(a: int, b: int): int {
  return a + b;
}

func advance(step: int, var position: int): int {
  position += step;
  return position;
}
```

Parameters enter a function by value. `var` only allows the parameter binding to be modified inside the function body; reassignment is not written back to the caller's binding.
Names in one parameter list must be unique, and the outermost function body cannot redeclare a parameter name. A
deeper nested block may shadow a parameter.

## Block Scope

A binding is visible from its declaration through its containing block and nested blocks. The same block cannot
declare one binding name twice. A nested block can shadow an outer binding with a binding of the same name:

```feng
let label = "outer";

if true {
  let label = "inner";
  println(label);
}

println(label);
```

After the nested block ends, the outer `label` becomes visible again.

`_` is an ordinary identifier, not a discard marker. `let _ = expression;` declares an ordinary binding named `_`,
so declaring `_` again in the same scope is a duplicate-binding error. To evaluate an expression and ignore its result,
write it as an expression statement:

```feng
advance();
```

## Destructuring Bindings

Named tuples and tuple literals can be destructured by position:

```feng
type Pair(int, string);

let pair: Pair = (7, "seven");
let (number, text) = pair;
let (first, second) = (1, 2);
let (, only_text) = pair;
```

An empty position discards the corresponding value and creates no binding. Nonempty positions in one destructuring
pattern must use distinct names. Feng currently supports only one level of destructuring.
