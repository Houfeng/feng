# Filesystem and I/O

Terminal I/O, path processing, and filesystem access are provided by `std.io`, `std.path`, and `std.fs`, respectively.

## Standard Input and Output

```feng
import std.io;

print("Name: ");
let name = readLine();
println("Hello, {0}", name);
```

`print` does not append a newline; `println` does. `stdio` provides lower-level byte I/O, standard-error output, and line-based reading:

```feng
stdio.writeErrorLine("warning");
```

## Paths

`std.path` processes strings without accessing the filesystem:

```feng
import std.path;

let path = join("data", "users", "alice.json");
let parent = dirname(path);
let name = basename(path);
let extension = extname(path);
let normalized = normalize("data/./users/../config");
```

The path separator is always `/`. `resolve(base, target)` uses the base supplied by the caller and does not read the current working directory.

## File Convenience Functions

```feng
import std.fs;

writeText("message.txt", "Hello, world!");
let content = readText("message.txt");

if exists("message.txt") && isFile("message.txt") {
  rename("message.txt", "greeting.txt");
}
```

Other functions include `readBytes`, `writeBytes`, `stat`, `isDir`, and `remove`.

## Explicit File Lifetime

```feng
let file = File.create("data.bin", FileMode.ReadWriteCreate);
defer {
  file.close();
}

file.writeText("Feng");
```

`File` provides byte and text I/O, status queries, and an explicit `close()`. A closed file cannot be read or written.

## Directories

```feng
mkdir("output");
let entries = readDir("output");

let directory = Dir.create(".");
defer {
  directory.close();
}

for let entry in directory {
  println(entry.name);
}
```

Directory iteration yields `EntryInfo` values whose kind is represented by `EntryKind`. Filesystem errors are reported as Feng exceptions.
