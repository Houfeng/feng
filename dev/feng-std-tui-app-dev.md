# Feng TUI App 控制层方案

> 状态：设计中（design）
>
> 本文档是 `dev/feng-std-tui-dev.md` 阶段四（应用控制层 - 纯渲染）的实现方案细化。
> 仅在此记录 C ABI 外部函数声明、类型选择依据与渲染主循环设计，不重复 dev 主文档的路线规划。

## 1 总体策略

**混合方案 + poll 多路复用**：libuv TTY API 处理 Raw Mode / 终端尺寸获取，libc `signal()` + self-pipe 处理 SIGWINCH，主循环用 `poll()` 多路复用驱动。无事件时阻塞休眠，零 CPU；任一 fd 可读即唤醒，渲染一帧。

### 1.1 选择理由

- **libuv TTY 绕开 termios 内联数组**：`struct termios` 含 `cc_t c_cc[NCCS]` 内联数组（macOS 32 字节，Linux 更大），无法用 `@abi type` 正规建模。`uv_tty_t` 内部持有 `struct termios orig_termios`，由 libuv 自行分配和管理，Feng 侧完全不碰 termios 内存布局。
- **libc signal() + self-pipe 绕开 sigaction 结构体**：`struct sigaction` 含 `sigset_t sa_mask` 内联数组（macOS 128 字节，Linux 1024+ 字节）。`signal()` 不涉及任何结构体，签名简洁。信号到达时写管道，将信号转为 fd，纳入 `poll()` 监听。
- **poll() 多路复用驱动主循环**：`poll(stdin + sigpipe + 用户fds, -1)` 无事件时阻塞休眠，零 CPU；任一 fd 可读即唤醒。比 busy-wait `while` 轮询高效，比 libuv `uv_run` 简单（无 handle 生命周期管理、无 `@abi func` 模块级回调限制）。
- **PollFd 值类型**：`@value @abi type PollFd` 直接建模 C `struct pollfd`，含 `i16` 字段，`&` 取地址传给 `poll()`，无需位操作或 C 封装函数。
- **阶段五衔接**：阶段五 stdin 已在 poll 监听集合中，只需加读取逻辑。异步 I/O 库通过 `addFd` 注册其通知 fd（如 epoll/kqueue/self-pipe 的管道读端），事件到达时通知主循环渲染。

### 1.2 各能力来源一览

| 能力 | 方案 | 避开了什么 |
|------|------|-----------|
| Raw Mode 进入 | `uv_tty_set_mode(tty, UV_TTY_MODE_RAW)` | `termios` 结构体（含 `c_cc[32]` 内联数组） |
| 终端恢复 | `uv_tty_reset_mode()` + atexit | 全局重置 |
| 终端尺寸 | `uv_tty_get_winsize(tty, &w, &h)` | `struct winsize`（`i32*` 直出） |
| SIGWINCH | `libc signal()` + self-pipe → `poll()` 监听 | `struct sigaction`（含 `sigset_t` 内联数组） |
| 主循环驱动 | `poll(stdin + sigpipe + 用户fds, -1)` | busy-wait 轮询 / uv_run handle 管理 |
| 网络/文件 I/O | 用户 `addFd(fd)` 注册通知 fd | uv_tcp_t 全套 |
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
| `pipe` | `fds` | `int[2]` | `i32*` | 输出参数，pipefd[0]=读端, pipefd[1]=写端 |
| | 返回值 | `int` | `i32` | |
| `poll` | `fds` | `struct pollfd*` | `PollFd*` | `@value @abi type` 取地址 |
| | `nfds` | `nfds_t` | `i32` | 元素个数 |
| | `timeout` | `int` | `i32` | -1=无限等待 |
| | 返回值 | `int` | `i32` | 有事件的 fd 数 |
| `close` | `fd` | `int` | `i32` | |
| | 返回值 | `int` | `i32` | |
| `write` | `fd` | `int` | `i32` | |
| | `buf` | `void*` | `byte*` | |
| | `count` | `size_t` | `uint` | 无符号平台位宽 |
| | 返回值 | `ssize_t` | `int` | 有符号平台位宽 |
| `read` | `fd` | `int` | `i32` | |
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

### 3.3 libc signal / atexit / pipe

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

/** 创建管道：fds[0]=读端, fds[1]=写端 */
@cdecl("libc", "pipe")
extern func c_pipe(fds: i32*): i32;
```

### 3.4 libc poll / write / read / close

```feng
/** pollfd 结构体 — 与 C struct pollfd 内存布局一致 */
@value @abi
type PollFd {
  var fd: i32;
  var events: i16;
  var revents: i16;

  /** 无参构造：所有字段置 0 */
  func PollFd() {
    self.fd = 0;
    self.events = 0;
    self.revents = 0;
  }

  /** 指定 fd 和监听事件，revents 内部置 0（输出字段，由内核在 poll 返回后填充） */
  func PollFd(fd: i32, events: i16) {
    self.fd = fd;
    self.events = events;
    self.revents = 0;
  }
}

/** 多路复用 I/O 等待，无事件时阻塞休眠 */
@cdecl("libc", "poll")
extern func c_poll(fds: PollFd*, nfds: i32, timeout: i32): i32;

/** 输出 ANSI 序列到 stdout */
@cdecl("libc", "write")
extern func c_write(fd: i32, buf: byte*, count: uint): int;

/** 从 fd 读取数据 */
@cdecl("libc", "read")
extern func c_read(fd: i32, buf: byte*, count: uint): int;

/** 关闭文件描述符 */
@cdecl("libc", "close")
extern func c_close(fd: i32): i32;
```

> **PollFd 是 `@value @abi type`**：无托管头，`offsetof(first_field) == 0`，`&` 取地址直接得到 `struct pollfd*`，与 C 内存布局完全一致。含 `i16` 字段，无需位操作。
>
> **无需 `feng_string_to_utf8_bytes`**：`Screen.buildPatchBytes()` 直接返回 `byte[]`，TuiApp 直接写入 stdout，不经过 string 中间转换。

## 4 常量

| 常量 | 值 | 说明 |
|------|----|------|
| `UV_TTY_MODE_NORMAL` | `0` | 正常终端模式 |
| `UV_TTY_MODE_RAW` | `1` | Raw 输入模式 |
| `UV_TTY_MODE_IO` | `2` | IPC 二进制安全模式（Unix-only） |
| `POLLIN` | `1` | fd 可读事件 |
| `POLL_TIMEOUT_BLOCK` | `-1` | 无限等待 |
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
  /** 窗口尺寸已变化标志：sigpipe 可读时置位，render() 中检查并清零 */
  seal var resizeRequested: bool;
  /** 是否已初始化（防止重复 init / 重复 exit） */
  seal var initialized: bool;
  /** 主循环是否运行中（run() 置 true，exit() 置 false） */
  seal var running: bool;
  /** 信号管道读端（主循环 poll 监听） */
  seal var sigpipeR: i32;
  /** 信号管道写端（信号 handler 写） */
  seal var sigpipeW: i32;
  /** 外部通知 fd 数组（不含 stdin 和 sigpipeR，构造时传入）。TuiApp 只监听可读事件唤醒渲染，不写入、不关闭。 */
  seal var fds: i32[];
}
```

### 5.2 核心生命周期 API

阶段四 TuiApp 的公开生命周期仅三个方法：

| 方法 | 职责 |
|------|------|
| `init()` | 分配 TTY 句柄、进入 Raw Mode、创建信号管道、注册 SIGWINCH 和 atexit |
| `run()` | 启动主循环，`poll()` 多路复用驱动，事件到达时渲染一帧 |
| `exit()` | 退出 TUI 模式、恢复终端、关闭管道、释放 TTY 句柄 |

> 其他方法（如 `setRootView`、输入处理等）后续阶段扩展。`render()` 是单帧渲染方法，供 `run()` 内部调用，测试中也可单独调用。
>
> **`exit()` 不退出进程**：`exit()` 的语义是“退出 TUI 模式并清理资源”，不是终止进程。进程生命周期由调用方控制。进程真正退出时，`atexit` handler 调 `uv_tty_reset_mode()` 兜底恢复。
>
> **事件驱动模型**：`run()` 用 `poll()` 多路复用，无事件时阻塞休眠（零 CPU），任一 fd 可读即唤醒渲染。stdin、信号管道、用户注册的 fd 统一监听。

### 5.3 构造函数

```feng
/**
 * 构造函数（无外部 fd）：仅保存 Screen 引用，不做 TTY 初始化。
 * 阶段四纯渲染场景使用。
 * @param screen - 由调用方创建的 Screen 实例
 */
func TuiApp(screen: Screen) {
  self.screen = screen;
  self.tty = 0;
  self.loop = 0;
  self.resizeRequested = false;
  self.initialized = false;
  self.running = false;
  self.sigpipeR = 0;
  self.sigpipeW = 0;
  self.fds = i32[:0];
}

/**
 * 构造函数（有外部 fd）：保存 Screen 引用和外部通知 fd 数组，不做 TTY 初始化。
 * 异步 I/O 库场景使用：fds 为异步 I/O 库的通知 fd（如 epoll/kqueue/self-pipe 管道读端）。
 * TuiApp 只监听 fd 可读事件唤醒渲染，不向 fd 写入、不处理关闭——fd 的生命周期由调用方管理。
 * @param screen - 由调用方创建的 Screen 实例
 * @param fds - 外部通知 fd 数组，poll 监听可读事件
 */
func TuiApp(screen: Screen, fds: i32[]) {
  self.screen = screen;
  self.tty = 0;
  self.loop = 0;
  self.resizeRequested = false;
  self.initialized = false;
  self.running = false;
  self.sigpipeR = 0;
  self.sigpipeW = 0;
  self.fds = fds;
}
```

> **fds 是通知 fd，不是请求 fd**：典型场景是异步 I/O 库内部维护自己的 fd 集合（epoll/kqueue/self-pipe），对 TuiApp 只暴露一个或几个通知 fd。事件到达时 TuiApp 只管 render，异步 I/O 库自己处理事件分发。不是每个网络请求注册一个 fd。

### 5.4 init() — 进入 Raw Mode + 注册信号

```feng
/**
 * 初始化：分配 TTY 句柄、进入 Raw Mode、创建信号管道、注册 SIGWINCH 和 atexit 回调。
 * 幂等：重复调用安全。
 * @throws "tui/app/alloc-failed" — feng_alloc 失败
 * @throws "tui/app/tty-init-failed" — uv_tty_init 失败
 * @throws "tui/app/raw-mode-failed" — uv_tty_set_mode 失败
 * @throws "tui/app/pipe-failed" — pipe() 失败
 */
open func init(): void {
  if self.initialized { return; }
  // 分配 uv_tty_t 内存
  self.tty = feng_alloc(UV_TTY_T_SIZE);
  if self.tty == 0 { throw "tui/app/alloc-failed"; }
  self.loop = uv_default_loop();
  // 初始化 TTY 句柄：fd=stdout, readable=0（阶段四只输出，不读输入）
  let rc = uv_tty_init(self.loop, self.tty, STDOUT_FD, 0);
  if rc != 0 {
    feng_free(self.tty);
    self.tty = 0;
    throw "tui/app/tty-init-failed";
  }
  // 进入 Raw Mode
  let modeRc = uv_tty_set_mode(self.tty, UV_TTY_MODE_RAW);
  if modeRc != 0 {
    feng_free(self.tty);
    self.tty = 0;
    throw "tui/app/raw-mode-failed";
  }
  // 创建信号管道（SIGWINCH → fd 转换）
  let pipefds: i32[!] = i32[:2];
  let pipeRc = c_pipe(&pipefds);
  if pipeRc != 0 {
    feng_free(self.tty);
    self.tty = 0;
    throw "tui/app/pipe-failed";
  }
  self.sigpipeR = pipefds[0];
  self.sigpipeW = pipefds[1];
  // 注册 atexit 兜底恢复
  c_atexit(&ttyCleanup);
  // 注册 SIGWINCH handler（写管道唤醒主循环）
  c_signal(SIGWINCH, &handleSigwinch);
  self.initialized = true;
}
```

### 5.5 render() — 渲染一帧

```feng
/**
 * 渲染一帧：检查 resize、调用 Screen.buildPatchBytes()、写入 stdout。
 * 单帧渲染，由 run() 事件循环调用，也可在测试中单独调用。
 * 直接使用 byte[]，不经过 string 中间转换。
 * @throws "tui/app/write-failed" — write 返回负值
 */
open func render(): void {
  // 检查窗口尺寸变化标志（由 sigpipe 可读时置位）
  if self.resizeRequested {
    let w: i32[!] = i32[:1];
    let h: i32[!] = i32[:1];
    let rc = uv_tty_get_winsize(self.tty, &w, &h);
    if rc == 0 {
      self.screen.resize((u32)w[0], (u32)h[0]);
    }
    self.resizeRequested = false;
  }
  // 调用 Screen.buildPatchBytes() 生成 ANSI 序列字节
  let ansi = self.screen.buildPatchBytes();
  // 直接写入 stdout，无需 string → byte[] 转换
  let len = ansi.length();
  if len > 0 {
    let written = c_write(STDOUT_FD, &ansi, (uint)len);
    if written < 0 {
      throw "tui/app/write-failed";
    }
  }
}
```

> `render()` 与 `run()` 职责分离：`render()` 只管单帧渲染，`run()` 管循环生命周期。

### 5.6 run() — 启动主循环

```feng
/**
 * 启动主循环：poll() 多路复用驱动。
 * 无事件时阻塞休眠（零 CPU），任一 fd 可读即唤醒，渲染一帧。
 * 监听集合：stdin + sigpipeR + 构造时传入的 fds（一次性构造）。
 * 与 render() 职责分离：run() 管循环生命周期，render() 管单帧渲染。
 *
 * 启动时执行初始化渲染：清空物理终端屏幕（不擦除已绘制的 back 缓冲区内容）、
 * 隐藏光标、立即渲染首帧。这样应用在 run() 前绘制的内容首帧即可见，
 * 无需等待首个 poll 事件唤醒。
 * 退出由 exit() 置 running = false，下一轮 poll 返回后循环结束。
 */
open func run(): void {
  self.running = true;
  // 启动初始化渲染：清屏 + 隐藏光标 + 首帧
  self.screen.clearScreen();
  self.screen.hideCursor();
  self.render();
  // 一次性构造 pollfd 数组：stdin + sigpipeR + 用户 fds
  let total: int = 2 + self.fds.length();
  let pfds: PollFd[!] = PollFd[:total];
  // stdin + sigpipeR
  pfds[0] = PollFd(STDIN_FD, POLLIN);
  pfds[1] = PollFd(self.sigpipeR, POLLIN);
  // 用户 fds
  for var i: int = 0; i < self.fds.length(); i += 1 {
    pfds[i + 2] = PollFd(self.fds[i], POLLIN);
  }
  while self.running {
    // 重置 revents（poll 输出字段，每轮需清零）
    for var i: int = 0; i < total; i += 1 {
      pfds[i].revents = 0;
    }
    // 阻塞等待任一 fd 可读
    let rc = c_poll(&pfds, (i32)total, POLL_TIMEOUT_BLOCK);
    if rc > 0 {
      // 处理 sigpipeR（信号到达）
      if pfds[1].revents & POLLIN != 0 {
        let dummy: byte[!] = byte[:1];
        c_read(self.sigpipeR, &dummy, 1);
        self.resizeRequested = true;
      }
      // stdin 和用户 fd 的数据处理由调用方自行处理
      // 阶段五在此处加 stdin 读取逻辑
    }
    self.render();
  }
}
```

> `run()` 是主循环入口，无参数。pollfd 数组在循环前一次性构造，循环内只重置 `revents`。
> `poll(-1)` 无事件时阻塞休眠，零 CPU。任一 fd 可读即唤醒，TuiApp 只处理内部 sigpipeR，然后 render。
> 退出由 `exit()` 置 `running = false`，下一轮 poll 返回后循环结束。
>
> **TuiApp 不处理用户 fd 的数据**：TuiApp 只监听 fd 可读事件唤醒渲染，不向 fd 写入、不读取 fd 数据、不关闭 fd。fd 的生命周期由调用方管理。

### 5.7 exit() — 退出 TUI 模式与清理

```feng
/**
 * 退出 TUI 模式与清理：停止主循环、显示光标、全局重置终端模式、关闭管道、释放 TTY 句柄。
 * 不退出进程——进程生命周期由调用方控制。
 * 幂等：重复调用安全。
 */
open func exit(): void {
  if !self.initialized { return; }
  self.running = false;
  self.screen.showCursor();   // 恢复光标显示，与 run() 启动时隐藏对称
  uv_tty_reset_mode();       // 全局重置，与 atexit handler 一致
  c_close(self.sigpipeR);
  c_close(self.sigpipeW);
  self.sigpipeR = 0;
  self.sigpipeW = 0;
  feng_free(self.tty);
  self.tty = 0;
  self.initialized = false;
}
```

> `exit()` 与 atexit handler 统一使用 `uv_tty_reset_mode()`。
> `exit()` 后调用方仍可执行业务资源释放等操作，进程退出由调用方决定。

## 6 回调函数

### 6.1 SIGWINCH handler

```feng
/**
 * SIGWINCH 信号回调：写信号管道唤醒主循环。
 * 信号中断上下文中只做 write（信号安全操作），不做复杂逻辑。
 * 主循环 poll 返回后读走管道数据，置 resizeRequested 标志。
 */
@abi
func handleSigwinch(signum: i32): void {
  // sigpipeW 是模块级变量，init() 中赋值
  let dummy: byte[!] = byte[:1];
  c_write(sigpipeW, &dummy, 1);
}
```

> **self-pipe 模式**：`signal()` 回调无法唤醒 `poll()`，通过写管道将信号转为 fd 事件。主循环 `poll` 监听管道读端，信号到达时写管道 → poll 唤醒 → 读走数据 → 置 resizeRequested → render。
> `sigpipeW` 是模块级 `seal var`，`init()` 中赋值。`signal()` 回调无法访问 TuiApp 实例，但写管道是信号安全操作。

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

## 7 事件驱动主循环流程

```
TuiApp.init()
  ├── feng_alloc(UV_TTY_T_SIZE) → tty 句柄
  ├── uv_default_loop() → loop 句柄
  ├── uv_tty_init(loop, tty, STDOUT_FD, readable=0)
  ├── uv_tty_set_mode(tty, UV_TTY_MODE_RAW)
  ├── c_pipe(&pipefds) → sigpipeR(读), sigpipeW(写)
  ├── c_atexit(&ttyCleanup)              ← 进程退出兜底
  └── c_signal(SIGWINCH, &handleSigwinch) ← 信号→管道

TuiApp.run()
  ├── self.running = true
  └── while self.running:
        ├── 构造 pollfd = [stdin, sigpipeR, ...用户fds]
        ├── poll(pollfd, -1)              ← 阻塞，无事件时零 CPU
        │
        ├── if sigpipeR 可读:
        │     c_read(sigpipeR) → resizeRequested = true
        │
        ├── （阶段五）if stdin 可读: 读取输入
        └── render()
              ├── if resizeRequested:
              │     uv_tty_get_winsize → screen.resize(w, h)
              │     resizeRequested = false
              ├── screen.buildPatchBytes() → byte[]     ← 零转换
              └── c_write(STDOUT_FD, &ansi, len)    ← 零转换

SIGWINCH 中断 → handleSigwinch() → c_write(sigpipeW)
  → poll 唤醒 → c_read(sigpipeR) → resizeRequested = true → render()

TuiApp.exit()
  ├── self.running = false
  ├── uv_tty_reset_mode()       ← 全局重置，与 atexit 一致
  ├── c_close(sigpipeR) / c_close(sigpipeW)
  └── feng_free(tty)
```

## 8 测试策略

### 8.1 可测试项

| 测试项 | 方法 |
|--------|------|
| TuiApp 构造与字段初始化 | 构造后检查 tty/loop/screen/initialized/running/sigpipeR/sigpipeW/fds 字段 |
| init() 幂等性 | 连续调用两次 init()，第二次不重复分配 |
| render() 空帧输出 | Screen 空白时 render() 后 stdout 无额外输出 |
| render() 有内容输出 | buffer.draw 后 render()，stdout 包含正确 ANSI 序列 |
| resize 标志处理 | 手动设 resizeRequested=true，render() 后检查 screen 尺寸已更新 |
| exit() 幂等性 | 连续调用两次 exit()，第二次不重复恢复 |

### 8.2 不可测试项（需真实终端）

| 测试项 | 原因 |
|--------|------|
| Raw Mode 实际生效 | 需真实终端环境，CI 不可用 |
| SIGWINCH 实际触发 | 需真实终端 resize 操作 |
| uv_tty_init 实际返回值 | 在非 TTY 环境（如 CI 管道）中可能失败 |

> **测试环境注意**：`uv_tty_init` 在非 TTY 环境（如 CI 管道、重定向输出）中可能返回错误。std_test 运行在非 TTY 环境中，因此 TuiApp 的 TTY 初始化相关测试需做条件跳过（检测 isatty 后决定是否运行），或只测试不依赖 TTY 的逻辑（如 resize 标志处理、render 输出内容）。

## 9 阶段五衔接预留

阶段四完成后，阶段五（输入支持）的衔接点：

1. **stdin 已在 poll 监听集合中**：阶段四 `run()` 的 pollfd 数组已包含 stdin，阶段五只需在 poll 返回后加 stdin 读取逻辑（键盘/鼠标解析）
2. **异步 I/O**：异步 I/O 库内部维护自己的 fd 集合（epoll/kqueue/self-pipe），通过 `addFd` 注册一个通知 fd。事件到达时通知主循环渲染，异步 I/O 库自己处理事件分发
3. **SIGWINCH 已通过 self-pipe 处理**：阶段四已将 SIGWINCH 转为 fd 纳入 poll，无需迁移

## 10 平台注意

- **`uv_tty_t` 大小**：macOS arm64 实测 344 字节。Linux arm64 需另行确认。`feng_alloc` 分配时按平台取值，或取安全值 512。
- **`SIGWINCH` 值**：macOS 和 Linux 均为 28，一致。
- **Windows**：`SIGWINCH` 不存在，Windows 终端尺寸变化通过其他机制（如 `WINDOW_BUFFER_SIZE_EVENT`）。当前阶段只支持 Unix，Windows 支持后续设计。
