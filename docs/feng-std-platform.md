# Feng 标准库 Platform 规范

本文档定义 `std.platform` 模块当前公开的系统与资源信息 API。

## 1 设计约束

- `std.platform` 对 `extlib` 的调用必须走 C ABI 路径。
- `std.platform` 不通过 `@runtime` 访问 `extlib`。
- 当前实现基于 `libuv`，库名采用 `feng_std_uv`。

## 2 API 概览

模块：`std.platform`

公开类型：

- `SystemInfo`
  - `hostName(): string`
  - `os(): string`
  - `arch(): string`
  - `processId(): int`
  - `parentProcessId(): int`
- `MemoryInfo`
  - `totalBytes(): u64`
  - `freeBytes(): u64`
  - `availableBytes(): u64`
  - `constrainedBytes(): u64`
  - `residentBytes(): u64`
- `CpuInfo`
  - `availableParallelism(): u32`

公开入口按单项信息拆分，避免为了读取单个值触发额外 ABI 调用。

## 3 行为约定

- `SystemInfo.hostName()` 仅查询主机名。
- `SystemInfo.os()` 仅查询操作系统名称（小写，如 `linux`、`darwin`），如实返回平台原生 uname 值，不归一化到 [feng-os-arch.md](./feng-os-arch.md) 规范，消费方需自行兼容多种原生值。
- `SystemInfo.arch()` 仅查询系统架构（小写，如 `x86_64`、`arm64`），如实返回平台原生 uname 值，不归一化到 [feng-os-arch.md](./feng-os-arch.md) 规范，消费方需自行兼容多种原生值。
- `SystemInfo.processId()` 仅查询当前进程 PID。
- `SystemInfo.parentProcessId()` 仅查询父进程 PID。
- `MemoryInfo.totalBytes()` 仅查询总内存。
- `MemoryInfo.freeBytes()` 仅查询空闲内存。
- `MemoryInfo.availableBytes()` 仅查询可用内存。
- `MemoryInfo.constrainedBytes()` 仅查询受限内存。
- `MemoryInfo.residentBytes()` 仅查询进程常驻集内存（RSS）。
- `CpuInfo.availableParallelism()` 仅查询可并行执行度（available parallelism）。
- 当底层 `libuv` 调用返回错误码时，API 抛出字符串异常：
  - `platform/system-info/hostname`
  - `platform/system-info/uname`
  - `platform/memory-info/resident`

## 4 互操作约束

`std.platform` 仅使用如下 C ABI 导入（示意）：

- `@cdecl("feng_std_uv", "uv_os_gethostname")`
- `@cdecl("feng_std_uv", "uv_os_uname")`
- `@cdecl("feng_std_uv", "uv_os_getpid")`
- `@cdecl("feng_std_uv", "uv_os_getppid")`
- `@cdecl("feng_std_uv", "uv_get_total_memory")`
- `@cdecl("feng_std_uv", "uv_get_free_memory")`
- `@cdecl("feng_std_uv", "uv_get_available_memory")`
- `@cdecl("feng_std_uv", "uv_get_constrained_memory")`
- `@cdecl("feng_std_uv", "uv_resident_set_memory")`
- `@cdecl("feng_std_uv", "uv_available_parallelism")`
