# C Interoperability

Feng calls native libraries through explicit C ABI declarations. Interoperability requires the caller to describe external signatures, ownership, and lifetimes accurately.

## Import C Functions

`extern func` declares an external function. A calling-convention annotation specifies the library name and, optionally, the C symbol name:

```feng
@cdecl("m")
extern func sin(value: f64): f64;

@cdecl("m", "fabs")
extern func absolute(value: f64): f64;
```

`@stdcall` and `@fastcall` are also available. Every C ABI `extern func` must have exactly one parameterized calling-convention annotation.

The system linker rules add library prefixes and suffixes. When the Feng name differs from the actual C symbol, specify the symbol as the second argument.

## ABI Types

Scalars and enums can be passed directly by value. Use `@abi type` for a struct-like payload:

```feng
@abi
type Point {
  var x: i32;
  var y: i32;
}

@cdecl("geometry")
extern func point_distance(left: Point, right: Point): f64;
```

Direct fields of an `@abi type` are limited to supported scalars, enums, and pointer forms. Regular Feng objects, strings, arrays, and generic instances cannot be embedded directly as ABI fields.

## Pointers

A pointer type is written as `T*` and is used only for ABI storage and transfer:

```feng
@cdecl("libc", "strlen")
extern func c_strlen(value: string*): uint;

let text = "Feng";
let length = c_strlen(&text);
```

Feng pointers cannot be directly dereferenced, used in arithmetic, converted across types, or called. Only `==` and `!=` comparisons between pointers of the same type are allowed.

A data pointer is valid only for the duration of the call by default. If C stores a pointer, uses it asynchronously, or otherwise allows it to escape, Feng code must keep the original owner alive and follow the external API's ownership contract.

## Arrays and Lengths

Taking the address of an ABI-compatible array passes only the address of its first element; its length is not passed implicitly:

```feng
import std.collections;

@cdecl("checksum")
extern func checksum(data: byte*, length: uint): u32;

let bytes: byte[] = [1, 2, 3];
let value = checksum(&bytes, (uint)bytes.length());
```

Pass the length explicitly in a separate parameter or ABI field. The `T[]` / `T[!]` level of the array determines writability.

## Callbacks

Declare a function-pointer signature with `@abi spec` and provide a Feng callback with a top-level `@abi func`:

```feng
@abi
spec Compare(left: i32, right: i32): i32;

@abi
func compare_int(left: i32, right: i32): i32 {
  return left - right;
}

let callback: Compare* = &compare_int;
```

Only a top-level `@abi func` can be addressed as a function pointer. Regular functions, methods, lambdas, and closures cannot.

## Exceptions and Resources

A Feng exception cannot cross a C ABI boundary. An ABI function must catch internal exceptions and convert them into return values or error codes that C can understand.

`@abi` describes only layout and calling compatibility; it does not express resource ownership. Who allocates, who releases, and how long a pointer remains valid must all be handled explicitly in the Feng wrapper according to the C API's contract.

## Complete Example: Packaging and Using a C Static Library

This project packages a C static library with its Feng bindings and consumes the package from an application. It requires the host C toolchain and Feng, with no standard-library dependency. Save the files at these paths:

```text
geometry-demo/
├── geometry/
│   ├── feng.fm
│   ├── native/geometry.c
│   └── src/bindings.ff
└── app/
    ├── feng.fm
    └── src/main.ff
```

`geometry/native/geometry.c`:

```text
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/** Payload layout shared with the Feng declaration. */
typedef struct Point { int32_t x; int32_t y; } Point;

/** Private C resource layout, opaque to Feng. */
typedef struct Counter { int32_t value; } Counter;

/** Changes a copy and returns its payload by value. */
Point point_shift_value(Point point) {
    point.x += 10;
    return point;
}

/** Changes the borrowed payload in place. */
void point_shift_pointer(Point *point) { point->x += 20; }

/** Allocates a resource; the caller must destroy a non-null result. */
Counter *counter_create(int32_t initial) {
    Counter *counter = malloc(sizeof(*counter));
    if (counter != NULL) counter->value = initial;
    return counter;
}

/** Reads a live resource. */
int32_t counter_read(Counter *counter) { return counter->value; }

/** Releases a resource exactly once. */
void counter_destroy(Counter *counter) { free(counter); }

/** Prints one value without a Feng library dependency. */
void show_number(int32_t value) { printf("%d\n", (int)value); }

/** Has the same C signature for every Feng type argument. */
int32_t geometry_version(void) { return 1; }
```

`geometry/src/bindings.ff`:

```feng
open module geometry;

/** Matches the C Point payload in field order and width. */
@abi
open type Point { var x: i32; var y: i32; }

/** Names the opaque C resource; only Counter* crosses the boundary. */
@abi
open type Counter {}

/** Calls the by-value C operation. */
@cdecl("geometry")
extern func point_shift_value(point: Point): Point;

/** Calls the borrowed-pointer C operation. */
@cdecl("geometry")
extern func point_shift_pointer(point: Point*): void;

/** Creates a resource owned by the caller. */
@cdecl("geometry")
open extern func counter_create(initial: i32): Counter*;

/** Reads a live resource. */
@cdecl("geometry")
open extern func counter_read(counter: Counter*): i32;

/** Releases a previously created resource. */
@cdecl("geometry")
open extern func counter_destroy(counter: Counter*): void;

/** Prints through the C library. */
@cdecl("geometry")
open extern func show_number(value: i32): void;

/** Demonstrates a generic declaration with a fixed C signature. */
@cdecl("geometry", "geometry_version")
open extern func abi_version<T>(): i32;

/** Keeps the by-value C boundary inside the binding package. */
open func shift_value(point: Point): Point { return point_shift_value(point); }

/** Borrows the payload while this Feng call keeps its owner alive. */
open func shift_in_place(point: Point) { point_shift_pointer(&point); }
```

`geometry/feng.fm`:

```text
[package]
name: "geometry"
version: "0.1.0"
target: "lib"
src: "src/"
out: "build/"

[assets]
extlib: "extlib/"
```

`app/src/main.ff`:

```feng
module geometry_demo;
import geometry;

/** Contrasts Feng references, C value copies and borrowed pointers. */
func main(args: string[]) {
  let point = Point { x: 1, y: 2 };
  let alias = point;
  alias.x = 3;
  show_number(point.x);
  let shifted = shift_value(point);
  show_number(point.x);
  show_number(shifted.x);
  shift_in_place(point);
  show_number(point.x);
  show_number(alias.x);

  let handle = counter_create(7);
  let null_handle: Counter*;
  if handle == null_handle { show_number(-1); return; }
  defer { counter_destroy(handle); }
  show_number(counter_read(handle));
  show_number(abi_version<Point>());
  show_number(abi_version<Counter>());
}
```

`app/feng.fm`:

```text
[package]
name: "geometry_demo"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"

[dependencies]
geometry: "../geometry/build/pkg/geometry-0.1.0.fb"
```

Run these commands from `geometry-demo/`. This example targets macOS ARM64, and `cc` and `ar` must target the same platform. On Linux, replace the directory platform names and `--platform` together with the matching Feng identifier, such as `linux-x64-gnu`, and use the corresponding C toolchain.

```bash
mkdir -p geometry/build/native geometry/extlib/macos-arm64
cc -std=c11 -c geometry/native/geometry.c -o geometry/build/native/geometry.o
ar rcs geometry/extlib/macos-arm64/libgeometry.a geometry/build/native/geometry.o
feng pack geometry --platform=macos-arm64
feng run app
```

Normal output is `3`, `3`, `13`, `23`, `23`, `7`, `1`, then `1`, one number per line.

`@abi` preserves ordinary object reference semantics inside Feng: `alias` and `point` refer to the same object. A C function taking `Point` receives a copy of its field payload, and a returned payload becomes a new Feng object. A function taking `Point*` borrows the original payload address, so its changes are visible through `alias`. Here C does not retain the borrowed address, and Feng keeps `point` alive during the call.

The current version has a code-generation limitation when directly calling an imported extern that passes or borrows a field-bearing `@abi` type. The example keeps these C calls inside the binding package that declares `Point` and exposes ordinary Feng functions, `shift_value` and `shift_in_place`. The application uses them with normal Feng object semantics.

The fieldless `@abi type Counter` only names an opaque pointer type. It cannot cross the ABI by value, and taking the address of this empty object cannot create a C resource. C creates the resource; Feng stores a `Counter*` and operates through the C API. The example checks for null before registering exactly one release. The pointer and its copies must not be used after release. See [user-defined types](../language/user-defined-types.md) for lifetimes.

## Native Library Configuration

`[assets].extlib` points to a native-library root containing platform subdirectories. Packaging places `libgeometry.a` in `.fb/extlib/<platform>/`. The imported `@cdecl("geometry")` declarations require that library, so the compiler extracts and links it from the dependency package. Merely placing a library file there does not add it to the link without a corresponding extern library declaration.

Bare library names not supplied by a package use system linker lookup. `--lib` adds otherwise unrepresented native dependencies in direct compilation; native dependencies normally belong in extern declarations and `.fb` packages. It does not turn an unpackaged source project directory into a native-library search path. See [building and running](../projects/build-and-run.md) and [dependency management](../projects/dependencies.md).

## Restricted Generic Externs

The example's `abi_version<T>()` demonstrates a restricted generic extern. Whether `T` is `Point` or `Counter`, the external function is always `geometry_version`, taking no arguments and returning `i32`. The type parameter does not change the C signature or turn the C function into a template. No value argument can infer `T` here, so the call must provide it explicitly.

This does not permit C declarations such as `extern func identity<T>(value: T): T` or declarations containing an open `T*`. Every parameter and result must have a unique, fixed external representation that passes ABI checks at the declaration, not only after selecting a call instance. Put type-dependent Feng work in an ordinary generic wrapper that calls an extern with a fixed signature.
