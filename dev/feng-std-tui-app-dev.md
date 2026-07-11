# Feng TUI App 控制层方案

> 状态：设计中（design）
>
> 本文档是 `dev/feng-std-tui-dev.md` 阶段四（应用控制层 - 纯渲染）的实现方案细化。
> 仅在此记录 C ABI 外部函数声明、类型选择依据与渲染主循环设计，不重复 dev 主文档的路线规划。

## 1 总体策略

**混合方案**：libuv TTY API 处理 Raw Mode / 终端尺寸获取，libc `signal()` 处理 SIGWINCH，主循环自行实现，不依赖 `uv_run` 事件循环。

### 1.1 选择理由

- **libuv TTY 绕开 termios 内联数组**：`struct termios` 含 `cc_t c_cc[NCCS]` 内联数组（macOS 32 字节，Linux 更大），无法用 `@abi type` 正规建模。`uv_tty_t` 内部持有 `struct termios orig_termios`，由 libuv 自行分配和管理，Feng 侧完全不碰 termios 内存布局。
- **libc signal() 绕开 sigaction 结构体**：`struct sigaction` 含 `sigset_t sa_mask` 内联数组（macOS 128 字节，Linux 1024+ 字节）。`signal()` 不涉及任何结构体，签名简洁。
- **uv_run 不驱动 SIGWINCH**：`uv_signal_start` 需要事件循环驱动才能投递回调。阶段四是手动渲染循环，引入 `uv_run` 是不必要的复杂度。`signal()` 是异步中断，注册后立即生效。
- **阶段五衔接**：阶段五需要读 stdin 时，TTY 句柄已初始化（`readable=1`），只需加 `uv_read_start` + `uv_run(UV_RUN_NOWAIT)` 引入事件循环，天然衔接。

### 1.2 各能力来源一览

| 能力 | 方案 | 避开了什么 |
|------|------|-----------|
| Raw Mode 进入 | `uv_tty_set_mode(tty, UV_TTY_MODE_RAW)` | `termios` 结构体（含 `c_cc[32]` 内联数组） |
| Raw Mode 恢复 | `uv_tty_set_mode(tty, UV_TTY_MODE_NORMAL)` | 同上 |
| 终端恢复兜底 | `atexit(uv_tty_reset_mode)` | 全局重置，无需 tty 句柄 |
| 终端尺寸 | `uv_tty_get_winsize(tty, &w, &h)` | `struct winsize`（`i32*` 直出） |
| SIGWINCH | `libc signal(SIGWINCH, handler)` | `struct sigaction`（含 `sigset_t` 内联数组） |
| 写 stdout | `libc write(1, buf, len)` | 已有 `stdio.ff` 声明模式 |

## 2 类型选择原则

**核心规则**：指针值（`void*` / `uv_tty_t*` 等）用 Feng `int`（平台位宽，64 位系统上为 64 位），C 的 `int` 参数用 `i32`（32 位）。

### 2.1 区分依据

- Feng 的 `int` 是平台位宽：64 位系统上为 64 位，与 C 指针宽度一致。
- C 的 `int` 始终是 32 位。
- 若将 C 的 `int*` 参数声明为 `int*`（Feng 64 位），libuv 写入 32 位到 64 位槽位，小端序下碰巧可读但不安全。
- 正确做法：C 的 `int*` 声明为 `i32*`，用 `i32[!]` 缓冲。

### 2.2 逐参数类型表

| C 函数 | 参数 | C 类型 | Feng 类型 | 理由 |
|--------|------|--------|-----------|------|
| `uv_default_loop` | 返回值 | `uv_loop_t*` | `int` | 指针值 |
| `uv_tty_init` | `loop` | `uv_loop_t*` | `int` | 指针值 |
| | `tty` | `uv_tty_t*` | `int` | `feng_alloc` 返回的指针值 |
| | `fd` | `uv_file` (`int`) | `i32` | C 的 int = 32 位 |
| | `readable` | `int` | `i32` | C 的 int = 32 位 |
| | 返回值 | `int` | `i32` | C 的 int = 32 位 |
| `uv_tty_set_mode` | `tty` | `uv_tty_t*` | `int` | 指针值 |
| | `mode` | `uv_tty_mode_t` (enum) | `i32` | C enum = int = 32 位 |
| | 返回值 | `int` | `i32` | |
| `uv_tty_reset_mode` | 返回值 | `int` | `i32` | |
| `uv_tty_get_winsize` | `tty` | `uv_tty_t*` | `int` | 指针值 |
| | `width` | `int*` | `i32*` | C 的 int* 指向 32 位整数 |
| | `height` | `int*` | `i32*` | 同上 |
| | 返回值 | `int` | `i32` | |
| `signal` | `signum` | `int` | `i32` | |
| | `handler` | `sighandler_t` | `SignalHandler*` | 函数指针 |
| | 返回值 | `sighandler_t` | `SignalHandler*` | 旧 handler |
| `atexit` | `handler` | `void(*)(void)` | `AtexitHandler*` | 函数指针 |
| | 返回值 | `int` | `i32` | |
| `write` | `fd` | `int` | `i32` | |
| | `buf` | `void*` | `byte*` | |
| | `count` | `size_t` | `uint` | 无符号平台位宽 |
| | 返回值 | `ssize_t` | `int` | 有符号平台位宽 |
| `feng_alloc` | `size` | — | `int` | 已有声明 |
| | 返回值 | `void*` | `int` | 指针值 |
| `feng_free` | `ptr` | `void*` | `int` | 指针值 |

## 3 extern 函数声明

### 3.1 runtime 内存分配（与 Thread.ff 模式一致）

```feng
@runtime
extern func feng_alloc(size: int): int;

@runtime
extern func feng_free(ptr: int): void;
```

### 3.2 libuv TTY API

```feng
/** uv_loop_t* — 默认事件循环句柄（指针值，用 int 承载） */
@cdecl("feng_std_uv", "uv_default_loop")
extern func uv_default_loop(): int;

/** 初始化 TTY 句柄。fd=0 为 stdin（readable=1），fd=1 为 stdout（readable=0） */
@cdecl("feng_std_uv", "uv_tty_init")
extern func uv_tty_init(loop: int, tty: int, fd: i32, readable: i32): i32;

/** 设置终端模式：UV_TTY_MODE_NORMAL=0, UV_TTY_MODE_RAW=1, UV_TTY_MODE_IO=2 */
@cdecl("feng_std_uv", "uv_tty_set_mode")
extern func uv_tty_set_mode(tty: int, mode: i32): i32;

/** 全局重置终端模式（用于 atexit 兜底恢复） */
@cdecl("feng_std_uv", "uv_tty_reset_mode")
extern func uv_tty_reset_mode(): i32;

/** 获取终端窗口尺寸（列数, 行数） */
@cdecl("feng_std_uv", "uv_tty_get_winsize")
extern func uv_tty_get_winsize(tty: int, width: i32*, height: i32*): i32;
```

### 3.3 libc signal / atexit

```feng
/** 信号回调签名：handler(int signum) */
@abi
spec SignalHandler(signum: i32): void;

/** atexit 回调签名：handler() */
@abi
spec AtexitHandler(): void;

@cdecl("libc", "signal")
extern func c_signal(signum: i32, handler: SignalHandler*): SignalHandler*;

@cdecl("libc", "atexit")
extern func c_atexit(handler: AtexitHandler*): i32;
```

### 3.4 libc write（输出 ANSI 序列到 stdout）

```feng
@cdecl("libc", "write")
extern func c_write(fd: i32, buf: byte*, count: uint): int;
```

> **无需 `feng_string_to_utf8_bytes`**：`Screen.buildPatchBytes()` 直接返回 `byte[]`，TuiApp 直接写入 stdout，不经过 string 中间转换。

## 4 常量

| 常量 | 值 | 说明 |
|------|----|------|
| `UV_TTY_MODE_NORMAL` | `0` | 正常终端模式 |
| `UV_TTY_MODE_RAW` | `1` | Raw 输入模式 |
| `UV_TTY_MODE_IO` | `2` | IPC 二进制安全模式（Unix-only） |
| `SIGWINCH` | `28` | 窗口大小变化信号（macOS/Linux 一致） |
| `STDIN_FD` | `0` | stdin 文件描述符 |
| `STDOUT_FD` | `1` | stdout 文件描述符 |
| `UV_TTY_T_SIZE` | `344` | `uv_tty_t` 结构体大小（macOS arm64，用于 feng_alloc） |

> **平台注意**：`UV_TTY_T_SIZE` 可能因平台不同而变化。macOS arm64 实测 344 字节。Linux 需另行确认。分配时按平台取值，或取一个足够大的安全值（如 512）。

## 5 TuiApp 类型设计

### 5.1 字段

```feng
open type TuiApp {
  /** TTY 句柄（feng_alloc 分配的 uv_tty_t 内存指针） */
  seal var tty: int;
  /** 默认事件循环句柄（uv_default_loop() 返回值） */
  seal var loop: int;
  /** Screen 渲染底座 */
  seal var screen: Screen;
  /** SIGWINCH 标志：信号 handler 中置位，主循环检查并清零 */
  seal var resizeRequested: bool;
  /** 是否已初始化（防止重复 init / 重复 cleanup） */
  seal var initialized: bool;
}
```

### 5.2 构造函数

```feng
/**
 * 构造函数：初始化 TTY 句柄并进入 Raw Mode。
 * @param screen - 由调用方创建的 Screen 实例
 */
func TuiApp(screen: Screen) {
  self.screen = screen;
  self.tty = (int)0;
  self.loop = (int)0;
  self.resizeRequested = false;
  self.initialized = false;
}
```

### 5.3 init() — 进入 Raw Mode + 注册信号

```feng
/**
 * 初始化：分配 TTY 句柄、进入 Raw Mode、注册 SIGWINCH 和 atexit 回调。
 * 幂等：重复调用安全。
 * @throws "tui/app/alloc-failed" — feng_alloc 失败
 * @throws "tui/app/tty-init-failed" — uv_tty_init 失败
 * @throws "tui/app/raw-mode-failed" — uv_tty_set_mode 失败
 */
open func init(): void {
  if self.initialized { return; }
  // 分配 uv_tty_t 内存
  self.tty = feng_alloc(UV_TTY_T_SIZE);
  if self.tty == (int)0 { throw "tui/app/alloc-failed"; }
  self.loop = uv_default_loop();
  // 初始化 TTY 句柄：fd=stdout, readable=0（阶段四只输出，不读输入）
  let rc = uv_tty_init(self.loop, self.tty, STDOUT_FD, (i32)0);
  if rc != (i32)0 {
    feng_free(self.tty);
    self.tty = (int)0;
    throw "tui/app/tty-init-failed";
  }
  // 进入 Raw Mode
  let modeRc = uv_tty_set_mode(self.tty, UV_TTY_MODE_RAW);
  if modeRc != (i32)0 {
    feng_free(self.tty);
    self.tty = (int)0;
    throw "tui/app/raw-mode-failed";
  }
  // 注册 atexit 兜底恢复
  c_atexit(&ttyCleanup);
  // 注册 SIGWINCH handler
  c_signal(SIGWINCH, &handleSigwinch);
  self.initialized = true;
}
```

### 5.4 render() — 渲染一帧

```feng
/**
 * 渲染一帧：检查 resize、调用 Screen.buildPatchBytes()、写入 stdout。
 * 直接使用 byte[]，不经过 string 中间转换。
 * @throws "tui/app/write-failed" — write 返回负值
 */
open func render(): void {
  // 检查 SIGWINCH 标志
  if self.resizeRequested {
    let w: i32[!] = i32[:(int)1];
    let h: i32[!] = i32[:(int)1];
    let rc = uv_tty_get_winsize(self.tty, &w, &h);
    if rc == (i32)0 {
      self.screen.resize((u32)w[0], (u32)h[0]);
    }
    self.resizeRequested = false;
  }
  // 调用 Screen.buildPatchBytes() 生成 ANSI 序列字节
  let ansi = self.screen.buildPatchBytes();
  // 直接写入 stdout，无需 string → byte[] 转换
  let len = ansi.length();
  if len > (int)0 {
    let written = c_write(STDOUT_FD, &ansi, (uint)len);
    if written < (int)0 {
      throw "tui/app/write-failed";
    }
  }
}
```

### 5.5 run() — 主循环

```feng
/**
 * 主循环：反复 render()，由调用方控制退出。
 * 注意：阶段五在此处加入输入读取逻辑（uv_read_start + uv_run）。
 * 退出时通过 defer 保证终端恢复。
 */
open func run(renderFn: RenderFn): void {
  defer {
    if self.initialized {
      uv_tty_set_mode(self.tty, UV_TTY_MODE_NORMAL);
      feng_free(self.tty);
      self.tty = (int)0;
      self.initialized = false;
    }
  }
  while renderFn(self) {
    self.render();
  }
}
```

> `renderFn` 是一个回调，每次循环调用，返回 `true` 继续、`false` 退出。
> 回调中由调用方完成业务绘制（往 `self.screen.buffer()` 写内容）。
> 阶段四先用简单的循环结构，阶段五加入输入后改为事件驱动。

### 5.6 cleanup() — 手动清理

```feng
/**
 * 手动清理：恢复终端模式并释放 TTY 句柄。
 * 正常退出路径由 defer 保证调用；此方法供异常路径补充调用。
 */
open func cleanup(): void {
  if !self.initialized { return; }
  uv_tty_set_mode(self.tty, UV_TTY_MODE_NORMAL);
  feng_free(self.tty);
  self.tty = (int)0;
  self.initialized = false;
}
```

## 6 回调函数

### 6.1 SIGWINCH handler

```feng
/**
 * SIGWINCH 信号回调：只设标志位，不做复杂操作（信号安全）。
 * 主循环在 render() 中检查 resizeRequested 标志并处理 resize。
 */
@abi
func handleSigwinch(signum: i32): void {
  // 模块级标志位（需在 TuiApp 实例中可访问）
  // 注意：signal handler 无法直接访问 TuiApp 实例，
  // 使用模块级 seal var 作为信号标志
  resizeFlag = true;
}
```

> **设计注意**：`signal()` 回调无法携带 `user_data`，无法直接访问 TuiApp 实例。采用模块级 `seal var resizeFlag: bool` 作为信号标志，TuiApp.render() 中读取并清零。如果未来有多个 TuiApp 实例（通常不会），需要改为引用计数或其他机制。

### 6.2 atexit handler

```feng
/**
 * atexit 回调：进程退出时全局重置终端模式。
 * 不依赖任何 TuiApp 实例，直接调用 uv_tty_reset_mode()。
 */
@abi
func ttyCleanup(): void {
  uv_tty_reset_mode();
}
```

## 7 渲染主循环流程

```
TuiApp.init()
  ├── feng_alloc(UV_TTY_T_SIZE) → tty 句柄
  ├── uv_default_loop() → loop 句柄
  ├── uv_tty_init(loop, tty, STDOUT_FD, readable=0)
  ├── uv_tty_set_mode(tty, UV_TTY_MODE_RAW)
  ├── c_atexit(&ttyCleanup)         ← 进程退出兜底
  └── c_signal(SIGWINCH, &handleSigwinch)  ← 信号注册

TuiApp.run(renderFn)
  ├── defer { uv_tty_set_mode(NORMAL); feng_free(tty) }  ← 正常退出保证
  └── while renderFn(self):
        TuiApp.render()
          ├── if resizeRequested:
          │     uv_tty_get_winsize → screen.resize(w, h)
          │     resizeRequested = false
          ├── screen.buildPatchBytes() → byte[]          ← 零转换
          └── c_write(STDOUT_FD, &ansi, len)        ← 零转换

SIGWINCH 中断 → handleSigwinch() → resizeFlag = true
  （下一帧 render() 时检查）
```

## 8 测试策略

### 8.1 可测试项

| 测试项 | 方法 |
|--------|------|
| TuiApp 构造与字段初始化 | 构造后检查 tty/loop/screen/initialized 字段 |
| init() 幂等性 | 连续调用两次 init()，第二次不重复分配 |
| render() 空帧输出 | Screen 空白时 render() 后 stdout 无额外输出 |
| render() 有内容输出 | buffer.draw 后 render()，stdout 包含正确 ANSI 序列 |
| resize 标志处理 | 手动设 resizeRequested=true，render() 后检查 screen 尺寸已更新 |
| cleanup() 幂等性 | 连续调用两次 cleanup()，第二次不重复恢复 |

### 8.2 不可测试项（需真实终端）

| 测试项 | 原因 |
|--------|------|
| Raw Mode 实际生效 | 需真实终端环境，CI 不可用 |
| SIGWINCH 实际触发 | 需真实终端 resize 操作 |
| uv_tty_init 实际返回值 | 在非 TTY 环境（如 CI 管道）中可能失败 |

> **测试环境注意**：`uv_tty_init` 在非 TTY 环境（如 CI 管道、重定向输出）中可能返回错误。std_test 运行在非 TTY 环境中，因此 TuiApp 的 TTY 初始化相关测试需做条件跳过（检测 isatty 后决定是否运行），或只测试不依赖 TTY 的逻辑（如 resize 标志处理、render 输出内容）。

## 9 阶段五衔接预留

阶段四完成后，阶段五（输入支持）的衔接点：

1. **TTY 句柄复用**：阶段四 `uv_tty_init` 的 `readable` 参数改为 `1`（同时支持读写），阶段五直接加 `uv_read_start`
2. **事件循环引入**：阶段五在主循环中加入 `uv_run(loop, UV_RUN_NOWAIT)` 驱动异步 I/O
3. **SIGWINCH 迁移**（可选）：阶段五可将 `signal()` 迁移为 `uv_signal_start`，统一到事件循环

```feng
// 阶段五预览（不在阶段四实现）
@cdecl("feng_std_uv", "uv_read_start")
extern func uv_read_start(tty: int, allocCb: UvAllocCb*, readCb: UvReadCb*): i32;

@cdecl("feng_std_uv", "uv_run")
extern func uv_run(loop: int, mode: i32): i32;
```

## 10 平台注意

- **`uv_tty_t` 大小**：macOS arm64 实测 344 字节。Linux arm64 需另行确认。`feng_alloc` 分配时按平台取值，或取安全值 512。
- **`SIGWINCH` 值**：macOS 和 Linux 均为 28，一致。
- **Windows**：`SIGWINCH` 不存在，Windows 终端尺寸变化通过其他机制（如 `WINDOW_BUFFER_SIZE_EVENT`）。当前阶段只支持 Unix，Windows 支持后续设计。
