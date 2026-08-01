# 平台与进程

`std.platform` 查询当前系统资源，`std.process` 处理环境变量和子进程。

## 系统信息

```feng
import std.platform;

let host = SystemInfo.hostName();
let os = SystemInfo.os();
let arch = SystemInfo.arch();
let parallelism = CpuInfo.availableParallelism();
let total_memory = MemoryInfo.totalBytes();
```

`SystemInfo.os()` 和 `arch()` 返回底层系统的原生小写值，例如 `darwin`、`linux`、`arm64`、`aarch64` 或 `x86_64`。它们不是 Feng 构建参数使用的规范平台名；需要判断多个平台时应兼容可能的原生别名。

`MemoryInfo` 还提供空闲、可用、约束和当前进程常驻内存查询。完整保证见[Platform 规范](../../../specifications/feng-std-platform.md)。

## 环境与当前进程

```feng
import std.process;

if Process.hasEnv("HOME") {
  let home = Process.getEnv("HOME");
}

let pid = Process.currentId();
let parent = Process.parentId();
```

不存在的环境变量由 `getEnv` 返回空字符串；若要区分“不存在”和“值为空”，先调用 `hasEnv`。

## 启动子进程

```feng
let child = Process.spawn("printf", "%s", "hello");
let output = child.output();
let code = child.exitCode();
```

`spawn` 接收程序路径和变长字符串参数。当前实现捕获标准输出并等待进程结束。可以通过 `outputBytes()` 读取原始字节，通过 `isFinished()` 确认调用已经完成。

不要把不可信输入拼接成 shell 命令；将程序和每个参数分别传给 `spawn`。
