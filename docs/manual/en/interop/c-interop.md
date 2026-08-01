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
