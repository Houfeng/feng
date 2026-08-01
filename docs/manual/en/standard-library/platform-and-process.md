# Platform and Processes

`std.platform` queries system resources, while `std.process` handles environment variables and child processes.

## System Information

```feng
import std.platform;

let host = SystemInfo.hostName();
let os = SystemInfo.os();
let arch = SystemInfo.arch();
let parallelism = CpuInfo.availableParallelism();
let total_memory = MemoryInfo.totalBytes();
```

`SystemInfo.os()` and `arch()` return the underlying system's native lowercase values, such as `darwin`, `linux`, `arm64`, `aarch64`, or `x86_64`. These are not the canonical platform names used by Feng build options. Code that checks multiple platforms should account for possible native aliases.

`MemoryInfo` also provides queries for free, available, and constrained memory, as well as the current process's resident memory.

## Environment and Current Process

```feng
import std.process;

if Process.hasEnv("HOME") {
  let home = Process.getEnv("HOME");
}

let pid = Process.currentId();
let parent = Process.parentId();
```

`getEnv` returns an empty string for a missing environment variable. To distinguish a missing variable from one whose value is empty, call `hasEnv` first.

## Start a Child Process

```feng
let child = Process.spawn("printf", "%s", "hello");
let output = child.output();
let code = child.exitCode();
```

`spawn` accepts a program path followed by variadic string arguments. The current implementation captures standard output and waits for the process to finish. Use `outputBytes()` to read raw bytes and `isFinished()` to confirm that the call has completed.

Do not concatenate untrusted input into a shell command. Pass the program and each argument separately to `spawn`.
