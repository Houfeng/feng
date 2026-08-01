# Standard Library Guide

The Feng standard library ships with the official distribution. Its modules provide commonly used data structures, text processing, I/O, filesystem access, numeric operations, time, platform information, and process management.

## Use the Standard Library

Import modules to use standard-library features:

```feng
import std.collections;
import std.io;
import std.text;
```

Some modules add methods to built-in types through `fit`. These extensions enter the current file's scope only after the corresponding module is imported. For example, an array's `length()` method comes from `std.collections`, and string text methods come from `std.text`.

## Chapters

- [Collections](./collections.md): array extensions, `Span<T>`, `List<T>`, `Map<K, V>`, and `Set<K>`.
- [Text](./text.md): strings, formatting, Unicode views, and regular expressions.
- [Filesystem and I/O](./filesystem-and-io.md): terminal input and output, paths, files, and directories.
- [Time and Math](./time-and-math.md): `DateTime` and `Math`.
- [Platform and Processes](./platform-and-process.md): system information, memory, CPUs, and child processes.

This directory introduces standard-library capabilities through common tasks. Each chapter should directly explain the boundaries, errors, and complexity guarantees users need without relying on documentation outside the manual.
