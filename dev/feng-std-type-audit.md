# std 整数类型用法审计

> 状态：待决策  
> 日期：2026-06-27  
> 关联：[dev/feng-scalar-alias-optimize.md](feng-scalar-alias-optimize.md) Task 7

## 背景

Task 6 完成后，`int` 在 64 位平台上映射为 `i64`。本审计检查 std 中所有应使用平台位宽（`int`/`uint`）却使用了固定宽度类型（`i64`/`u64`）的地方，以及 extern 声明与实际 ABI 不匹配的地方。

- **ABI 声明不对齐**：Feng extern 声明的类型与实际 C/runtime 函数的 ABI 类型不一致，在 32 位平台上会导致调用失败
- **语义不合理**：ABI 层面当前可工作，但类型选择不能准确表达语义（如用固定宽度 `i64` 表示本质上为平台位宽的 size/count/offset/handle），不符合 `int` 平台相关的定位

---

## 一、feng_runtime_contract（运行时 ABI）

> runtime contract 定义于 `src/runtime/feng_runtime_contract.inc`，使用 `intptr_t` 表示平台位宽整数。std 中 `@runtime` extern 声明若使用 `i64`，在 32 位平台上 `int`→`i32` 而 `i64` 仍为 64 位，将导致 ABI 不匹配。

### 1.1 ABI 声明不对齐 [待优化]

| # | 文件 | 行 | 当前声明 | runtime contract 实际签名 | 不对齐的参数/返回值 |
|---|------|----|---------|------------------------|-------------------|
| 1 | `thread/Thread.ff` | 6 | `feng_alloc(size: i64): i64` | `intptr_t feng_alloc(intptr_t size)` | `size` 和返回值 |
| 2 | `thread/Thread.ff` | 9 | `feng_free(ptr: i64): void` | `void feng_free(intptr_t ptr)` | `ptr` |
| 3 | `time/TimeZone.ff` | 26 | `feng_pointer_move(ptr: i64*, offset: i64): i64*` | `void *feng_pointer_move(void *ptr, intptr_t offset)` | `ptr`、`offset`、返回值 |
| 4 | `time/TimeZone.ff` | 29 | `feng_pointer_get_scalar<T>(ptr: i64*): T` | `void feng_pointer_get_scalar(..., void *ptr, ...)` | `ptr` |
| 5 | `fs/EntryInfo.ff` | 15 | `feng_pointer_move(ptr: i64*, offset: i64): i64*` | 同 #3 | `ptr`、`offset`、返回值 |
| 6 | `fs/EntryInfo.ff` | 18 | `feng_pointer_get_scalar<T>(ptr: i64*): T` | 同 #4 | `ptr` |
| 7 | `text/RegExp.ff` | 57 | `feng_pointer_get_scalar<T>(ptr: i64*): T` | 同 #4 | `ptr` |
| 8 | `text/RegExp.ff` | 60 | `feng_pointer_move(ptr: i64*, offset: i64): i64*` | 同 #3 | `ptr`、`offset`、返回值 |
| 9 | `process/Process.ff` | 19 | `feng_pointer_move(ptr: byte*, offset: i64): byte*` | `intptr_t offset` | `offset` |
| 10 | `process/Process.ff` | 22 | `feng_pointer_move(ptr: string*, offset: i64): string*` | `intptr_t offset` | `offset` |
| 11 | `numeric/f64.ff` | 14 | `feng_pointer_get_pointer(ptr: i64*): i64*` | `void *feng_pointer_get_pointer(void *ptr)` | `ptr`、返回值 |
| 12 | `numeric/f64.ff` | 17 | `feng_pointer_get_scalar<T>(ptr: i64*): T` | 同 #4 | `ptr` |

> **注**：以下 @runtime 声明已正确使用 `int`（与 `intptr_t` 对齐），无需变更：`feng_array_get_length`、`feng_array_slice`、`feng_string_utf8_length`、`feng_string_from_utf8_bytes`、`feng_string_to_utf8_bytes`、`feng_string_slice`、`feng_string_slice_bytes`、`feng_string_range_equal`、`feng_pointer_diff`、`feng_pointer_is_null`、`u8_mbsnlen`。

### 1.2 语义不合理 [待优化]

> 以下变量/常量使用 `i64`，但语义上应与 runtime contract 的 `intptr_t` 对齐（平台位宽）。

| # | 文件 | 位置 | 描述 |
|---|------|------|------|
| 1 | `thread/Thread.ff:32` | `seal var spawnLock: i64` | 存储 `feng_alloc` 返回值（`intptr_t`） |
| 2 | `thread/Thread.ff:33` | `seal var spawnCond: i64` | 同上 |
| 3 | `time/TimeZone.ff:33` | `TM_BUF_LONGS: i64` | struct tm buffer 的元素数量，用于 `feng_pointer_move` 偏移计算 |
| 4 | `time/TimeZone.ff:34` | `TM_GMTOFF_OFFSET: i64` | struct tm 中 tm_gmtoff 的字节偏移，传给 `feng_pointer_move` |
| 5 | `fs/EntryInfo.ff:21-25` | `STAT_*_OFFSET: i64`（5 项） | struct stat 字段字节偏移，传给 `feng_pointer_move` |
| 6 | `fs/Dir.ff:44-46` | `DIRENT_*_OFFSET: i64`（3 项） | struct dirent 字段字节偏移，传给 `feng_pointer_move` |
| 7 | `text/RegExp.ff:132-141` | `ovectorStart()`/`ovectorEnd()` | `feng_pointer_move` 偏移计算用 `i64` |

---

## 二、普通 C ABI（libc/libuv/pcre2/libunistring）

### 2.1 ABI 声明不对齐

#### 2.1.1 size_t / ssize_t / PCRE2_SIZE 类 [待优化]

> C 的 `size_t` 为无符号平台位宽（Feng `uint`），`ssize_t` 为有符号平台位宽（Feng `int`），`PCRE2_SIZE` 等同于 `size_t`。当前均声明为 `i64`（有符号固定 64 位），在符号性和平台位宽上均不对齐。

| # | 文件 | 行 | 当前声明 | C 实际签名 | 不对齐项 |
|---|------|----|---------|-----------|---------|
| 1 | `io/stdio.ff` | 8 | `read(fd: int, buf: byte*, count: i64): i64` | `ssize_t read(int, void*, size_t)` | `count`→`uint`，返回值→`int` |
| 2 | `io/stdio.ff` | 11 | `write(fd: int, buf: byte*, count: i64): i64` | `ssize_t write(int, const void*, size_t)` | `count`→`uint`，返回值→`int` |
| 3 | `fs/File.ff` | 14 | `c_read(fd: int, buf: byte*, count: i64): i64` | 同 #1 | `count`→`uint`，返回值→`int` |
| 4 | `fs/File.ff` | 17 | `c_write(fd: int, buf: byte*, count: i64): i64` | 同 #2 | `count`→`uint`，返回值→`int` |
| 5 | `time/TimeZone.ff` | 22 | `readlink(path: string*, buf: byte*, bufsiz: i64): i64` | `ssize_t readlink(const char*, char*, size_t)` | `bufsiz`→`uint`，返回值→`int` |
| 6 | `process/Process.ff` | 43 | `strlen(s: string*): i64` | `size_t strlen(const char*)` | 返回值→`uint` |
| 7 | `process/Process.ff` | 52 | `fread(ptr: byte*, size: i64, nmemb: i64, stream: CStream*): i64` | `size_t fread(void*, size_t, size_t, FILE*)` | `size`/`nmemb`/返回值→`uint` |
| 8 | `numeric/f64.ff` | 11 | `snprintf_f64(buf: byte*, size: i64, ...)` | `int snprintf(char*, size_t, ...)` | `size`→`uint` |
| 9 | `numeric/f32.ff` | 11 | `snprintf_f32(buf: byte*, size: i64, ...)` | 同 #8 | `size`→`uint` |
| 10 | `platform/SystemInfo.ff` | 7 | `uv_os_gethostname(name: byte*, size: i64*): int` | `int uv_os_gethostname(char*, size_t*)` | `size`→`uint*` |
| 11 | `text/RegExp.ff` | 15 | `pcre2_compile(..., length: i64, ...)` | `PCRE2_SIZE length` | `length`→`uint` |
| 12 | `text/RegExp.ff` | 33 | `pcre2_match(..., length: i64, startOffset: i64, ...)` | `PCRE2_SIZE` × 2 | `length`/`startOffset`→`uint` |
| 13 | `text/RegExp.ff` | 44 | `pcre2_get_error_message(..., buffer_size: i64): i32` | `int ...(PCRE2_SIZE)` | `buffer_size`→`uint` |
| 14 | `text/RegExp.ff` | 48 | `pcre2_substitute(..., length: i64, startoffset: i64, ..., rlength: i64, ..., outlengthptr: i64*)` | 多个 `PCRE2_SIZE` | 所有 size 参数→`uint` |

#### 2.1.2 time_t 类 [待优化]

> C 的 `time_t` 在现代 64 位系统上为 64 位有符号整数，与平台位宽一致。

| # | 文件 | 行 | 当前声明 | C 实际签名 | 不对齐项 |
|---|------|----|---------|-----------|---------|
| 1 | `time/TimeZone.ff` | 7 | `time(tloc: i64*): i64` | `time_t time(time_t*)` | `tloc`→`int*`，返回值→`int` |
| 2 | `time/TimeZone.ff` | 10 | `localtime_r(timep: i64*, result: i64*): i64*` | `struct tm* localtime_r(const time_t*, struct tm*)` | `timep`→`int*`，`result`/返回值为 opaque struct 指针 |

#### 2.1.3 不透明指针/句柄类 [待优化]

> C API 返回或接收不透明指针（`DIR*`、`uv_mutex_t*`、`pcre2_code*`、`pthread_t` 等），本质为指针地址，应为平台位宽。Feng 中有两种表示方式：
> 1. **空结构体指针**（如 `Pcre2MatchData*`、`Dirent*`、`CStream*`）— 已正确，无需改动
> 2. **用 `i64` 存储指针地址** — 应改为 `int`（平台位宽）

| # | 文件 | 行 | 当前声明 | C 实际类型 | 优化方案 |
|---|------|----|---------|-----------|---------|
| 1 | `thread/Thread.ff` | 12 | `uv_thread_create(tid: i64*, entry: ThreadAbiEntry*, arg: i64): int` | `int uv_thread_create(uv_thread_t*, uv_thread_cb, void*)` | `tid: int*`，`arg: int` |
| 2 | `thread/Thread.ff` | 15 | `uv_thread_join(tid: i64*): int` | `int uv_thread_join(uv_thread_t*)` | `tid: int*` |
| 3 | `thread/Thread.ff` | 18 | `uv_thread_self(): i64` | 返回 `uv_thread_t` | 返回值→`int` |
| 4 | `thread/Thread.ff` | 30 | `spec ThreadAbiEntry(arg: i64): void` | `uv_thread_cb` 签名为 `void (*)(void *arg)` | `arg: int` |
| 5 | `fs/Dir.ff` | 13 | `c_opendir(path: byte*): i64` | 返回 `DIR*` | 返回值→`int` |
| 6 | `fs/Dir.ff` | 16 | `c_readdir(dir: i64): Dirent*` | `DIR*` 参数 | `dir: int` |
| 7 | `fs/Dir.ff` | 19 | `c_closedir(dir: i64): int` | `DIR*` 参数 | `dir: int` |
| 8 | `thread/Mutex.ff` | 4 | `uv_mutex_init(handle: i64): int` | `uv_mutex_t*` | `handle: int` |
| 9 | `thread/Mutex.ff` | 7 | `uv_mutex_destroy(handle: i64): void` | `uv_mutex_t*` | `handle: int` |
| 10 | `thread/Mutex.ff` | 10 | `uv_mutex_lock(handle: i64): void` | `uv_mutex_t*` | `handle: int` |
| 11 | `thread/Mutex.ff` | 13 | `uv_mutex_trylock(handle: i64): int` | `uv_mutex_t*` | `handle: int` |
| 12 | `thread/Mutex.ff` | 16 | `uv_mutex_unlock(handle: i64): void` | `uv_mutex_t*` | `handle: int` |
| 13 | `thread/CondVar.ff` | 4 | `uv_cond_init(handle: i64): int` | `uv_cond_t*` | `handle: int` |
| 14 | `thread/CondVar.ff` | 7 | `uv_cond_destroy(handle: i64): void` | `uv_cond_t*` | `handle: int` |
| 15 | `thread/CondVar.ff` | 10 | `uv_cond_signal(handle: i64): void` | `uv_cond_t*` | `handle: int` |
| 16 | `thread/CondVar.ff` | 13 | `uv_cond_broadcast(handle: i64): void` | `uv_cond_t*` | `handle: int` |
| 17 | `thread/CondVar.ff` | 16 | `uv_cond_wait(cond: i64, mutex: i64): void` | `uv_cond_t*`, `uv_mutex_t*` | `cond: int`，`mutex: int` |
| 18 | `thread/CondVar.ff` | 19 | `uv_cond_timedwait(cond: i64, mutex: i64, timeout: u64): int` | `uv_cond_t*`, `uv_mutex_t*`, `uint64_t` | `cond: int`，`mutex: int`（`timeout: u64` 正确） |
| 19 | `text/RegExp.ff` | 18 | `pcre2_compile(..., cContext: i64): i64` | `pcre2_compile_context*`→`pcre2_code*` | `cContext: int`，返回值→`int` |
| 20 | `text/RegExp.ff` | 21 | `pcre2_code_free(code: i64): void` | `pcre2_code*` | `code: int` |
| 21 | `text/RegExp.ff` | 25 | `pcre2_match_data_create_from_pattern(code: i64, gContext: i64)` | `pcre2_code*`, `pcre2_general_context*` | `code: int`，`gContext: int` |
| 22 | `text/RegExp.ff` | 38 | `pcre2_get_ovector_pointer(...): i64*` | 返回 `PCRE2_SIZE*` | 返回值→`uint*` |

#### 2.1.4 char** (endptr) 类 [待优化]

> `strtod`/`strtof` 的 endptr 参数为 `char**`（指针的指针）。Feng 不支持指针的指针，endptr 存储的是指针地址（平台位宽），将 `i64*` 优化为 `int*` 即可与平台位宽对齐。

| # | 文件 | 行 | 当前声明 | C 实际签名 | 优化方案 |
|---|------|----|---------|-----------|---------|
| 1 | `numeric/f64.ff` | 8 | `strtod(text: string*, end: i64*): f64` | `double strtod(const char*, char**)` | `end: i64*` → `end: int*` |
| 2 | `numeric/f32.ff` | 8 | `strtof(text: string*, end: i64*): f32` | `float strtof(const char*, char**)` | `end: i64*` → `end: int*` |

#### 2.1.5 Feng `int` ≠ C `int` 问题 [待优化]

> C 的 `int` 始终为 32 位，但 Feng 的 `int` 在 64 位平台上为 `i64`（64 位），存在 ABI 宽度不匹配。C ABI 明确为 32 位宽的 `int`，应使用 `i32`。

| # | 文件 | 函数 | C 参数/返回值为 `int` (32 位) 的位置 | 优化方案 |
|---|------|------|-----------------------------------|---------|
| 1 | `io/stdio.ff` | `read`/`write` | `fd: int` | `fd: i32` |
| 2 | `fs/File.ff` | `c_open` | `flags: int, mode: int` 和返回值 `int` | → `i32` |
| 3 | `fs/File.ff` | `c_close` | `fd: int` 和返回值 `int` | → `i32` |
| 4 | `fs/File.ff` | `c_access` | `mode: int` 和返回值 `int` | → `i32` |
| 5 | `fs/File.ff` | `c_unlink`/`c_rename` | 返回值 `int` | → `i32` |
| 6 | `fs/File.ff` | `fs_posix_fstat` | 返回值 `int` | → `i32` |
| 7 | `fs/EntryInfo.ff` | `posix_stat` | 返回值 `int` | → `i32` |
| 8 | `fs/Dir.ff` | `c_closedir`/`c_mkdir`/`c_rmdir` | 返回值 `int` | → `i32` |
| 9 | `process/Process.ff` | `posix_exit` | `status: int` | `status: i32` |
| 10 | `process/Process.ff` | `getpid`/`getppid` | 返回值 `int` | → `i32` |
| 11 | `process/Process.ff` | `pclose` | 返回值 `int` | → `i32` |
| 12 | `process/Process.ff` | `posix_kill` | `pid: int, sig: int` 和返回值 `int` | → `i32` |
| 13 | `time/TimeZone.ff` | `setenv` | `overwrite: int` 和返回值 `int` | → `i32` |
| 14 | `time/TimeZone.ff` | `unsetenv` | 返回值 `int` | → `i32` |
| 15 | `thread/Thread.ff` | `uv_thread_create`/`uv_thread_join`/`uv_mutex_init` 等 | 返回值 `int`（libuv 返回码） | → `i32` |
| 16 | `thread/Thread.ff` | `uv_thread_getcpu` | 返回值 `int` | → `i32` |
| 17 | `platform/SystemInfo.ff` | `uv_os_gethostname`/`uv_os_uname` | 返回值 `int` | → `i32` |
| 18 | `platform/MemoryInfo.ff` | `uv_resident_set_memory` | 返回值 `int` | → `i32` |
| 19 | `platform/CpuInfo.ff` | `uv_available_parallelism` | 返回值 `int` | → `i32` |

### 2.2 语义不合理 [待优化]

> 以下为非 extern 的内部代码，使用 `i64` 作为 size/length/count/index/offset/handle，语义上应为平台位宽。在 64 位平台上无行为差异，但不符合 `int` 平台相关的定位。

| # | 文件 | 位置 | 描述 |
|---|------|------|------|
| 1 | `thread/Thread.ff:60` | `seal var handle: i64` (Thread) | 存储 `uv_thread_t`（opaque handle），应为 `int` |
| 2 | `thread/Thread.ff:63` | `seal func Thread(handle: i64)` | 构造参数，同上，应为 `int` |
| 3 | `thread/Thread.ff:76,110` | `tidBuf: i64[!]` | 存储 `uv_thread_t`，应为 `int[!]` |
| 4 | `thread/Mutex.ff:23` | `seal let handle: i64` (Mutex) | 存储 `uv_mutex_t*` handle，应为 `int` |
| 5 | `thread/CondVar.ff:27` | `seal let handle: i64` (CondVar) | 存储 `uv_cond_t*` handle，应为 `int` |
| 6 | `time/TimeZone.ff:41` | `gmtoff: i64` | 从 struct tm 读取的 `tm_gmtoff`（POSIX `long`，平台位宽） |
| 7 | `time/TimeZone.ff:49` | `t: i64[!]` | `time_t` buffer（`time_t` 为平台位宽有符号整数） |
| 8 | `text/RegExp.ff:65` | `PCRE2_UNSET: i64` | `~(size_t)0`，应为 `uint`（`PCRE2_SIZE` = `size_t`） |
| 9 | `numeric/f64.ff:52` | `endRaw: i64` | `strtod` 的 endptr 存储（指针地址），应为 `int` |
| 10 | `numeric/f32.ff:46` | `endRaw: i64` | `strtof` 的 endptr 存储（指针地址），应为 `int` |
| 11 | `fs/Dir.ff:89` | `Dir.dir: i64` | 存储 `DIR*` 句柄，应为 `int` |
| 12 | `fs/Dir.ff:111` | `read(batchSize: i64)` | 批量读取数量参数 |
| 13 | `fs/Dir.ff:117` | `count: i64` | 已读条目计数 |
| 14 | `fs/Dir.ff:191,199,211` | `nameLen: i64`、`i: i64` | dirent 名称长度和循环变量 |
| 15 | `fs/Dir.ff:280-284` | `readDir()` 内部 | `i: i64` 循环、`batch.length()` 比较用 `(i64)0` |
| 16 | `fs/File.ff:64,82` | `capacity == (i64)0`、`length == (i64)0` | `buffer.length()` 返回 `int`，却与 `(i64)0` 比较 |
| 17 | `fs/File.ff:118-148` | `readLines()` 内部 | `i: i64` 循环、`(i64)lineBytes.size()` 强转、`(i64)lines.size()` 强转 |
| 18 | `fs/File.ff:191-197` | `readAllBytes()` 内部 | `chunk: byte[:(i64)4096]`、`i: i64` 循环 |
| 19 | `fs/EntryInfo.ff:254-262` | `makeNullTerminatedPath()` | `pathLen` 用 `i64`（`bytes.length()` 返回 `int`）、`i: i64` 循环 |
| 20 | `time/TimeZone.ff:58-88` | `readSystemZoneId()` 内部 | `buf: byte[:(i64)256]`、`len`/`found`/`i`/`k`/`markerLen`/`idLen` 均为 `i64` |
| 21 | `time/TimeZone.ff:112` | `tmBuf: i64[!] = i64[:TM_BUF_LONGS]` | tm buffer 声明 |
| 22 | `time/DateTime.ff:178` | `tv: i64[!] = i64[:(i64)2]` | gettimeofday buffer |
| 23 | `time/DateTime.ff:160,186` | `components: i32[:(i64)6]` | 字面量 `(i64)6` 应为 `(int)6` |
| 24 | `time/DateTime.ff:828` | `components: i32[:(i64)6]` | 同上 |
| 25 | `process/Process.ff:61` | `byte[:(i64)0]` | 字面量类型 |
| 26 | `process/Process.ff:67,76` | `i: i64` 循环 | 遍历 `source.length()` / `arg.length()`（返回 `int`） |
| 27 | `process/Process.ff:114` | `i: i64` 循环 | 遍历 `strlen` 返回值 |
| 28 | `process/Process.ff:180` | `i: i64` 循环 | 遍历 `args.length()`（返回 `int`） |
| 29 | `process/Process.ff:191,201` | `byte[:(i64)2]`、`byte[:(i64)4096]` | 字面量类型 |
| 30 | `process/Process.ff:203-207` | `fread` 参数和循环 | `(i64)1`、`(i64)4096`、`j: i64` |
| 31 | `io/stdio.ff:247` | `chunk: byte[:(int)4096]` → 后续 `count` 为 `i64` | `read()` 返回 `i64` 导致级联 |
| 32 | `io/stdio.ff:259-270` | `readLine()` 内部 | `index: i64`、`(i64)1`、`(int)(index + (i64)1)` 强转 |
| 33 | `text/RegExp.ff:149` | `groupArr: string[:(i64)ovCount]` | `ovCount` 为 `u32`，强转 `i64` |
| 34 | `text/RegExp.ff:176` | `buf: byte[:(i64)256]` | 字面量类型 |
| 35 | `text/RegExp.ff:288-327` | `findAll()` 内部 | `capacity: i64`、`count: i64`、`offset: i64`、`j: i64`、`k: i64` |
| 36 | `text/RegExp.ff:337-369` | `split()` 内部 | `pos: i64` |
| 37 | `text/RegExp.ff:399-433` | `substituteInternal()` 内部 | `outLen: i64`、`actualLen: i64` |
| 38 | `text/RegExp.ff:117,122` | `Match.start: i64`、`Match.end: i64` | 匹配位置索引，语义为平台位宽 |
| 39 | `platform/SystemInfo.ff:24-25` | `nameSize: i64[!] = i64[:(int)1]` | `size_t*` 参数，应为 `uint[!]` |
| 40 | `platform/SystemInfo.ff:31` | `(int)nameSize[0]` | 需从 `i64` 强转为 `int` |

---

## 三、当前正确、无需变更的用法 [无需变更]

以下用法经分析确认为正确，不在变更范围内：

| 类别 | 文件 | 说明 |
|------|------|------|
| DateTime 日历分量 | `DateTime.ff` | `year/month/day/hour/minute/second/millisecond/microsecond/nanosecond: i32` — 范围小，固定 32 位合理 |
| 时间戳 | `DateTime.ff` | `milliseconds: i64`、`toMilliseconds(): i64`、`toSeconds(): i64` — 需要 64 位固定宽度 |
| 算术中间值 | `DateTime.ff` | `days: i64`、`era: i64`、`secPerDay: i64` 等 — 日历算法中间值，固定 64 位防溢出 |
| 内存字节数 | `MemoryInfo.ff` | `u64` — libuv API 定义为 `uint64_t`，且可超过 2^32 |
| CPU 并行度 | `CpuInfo.ff` | `u32` — 合理上限 |
| Unicode code point | `String.ff`、`Rune.ff` | `u32` — Unicode 码点为 21 位 |
| PCRE2 选项位 | `RegExp.ff` | `u32` — PCRE2 选项为 32 位位掩码 |
| 纳秒超时 | `CondVar.ff` | `timeoutNs: u64` — 需要大值范围 |
| 条件变量等待 | `CondVar.ff` | `uv_cond_timedwait` 的 `timeout: u64` — `uint64_t` 纳秒 |
| struct stat buffer | `EntryInfo.ff`、`File.ff` | `statBuf: i64[!]` — 读取 struct stat 的 64 位字段，buffer 元素类型为 `i64` 正确 |
| POSIX 文件类型掩码 | `EntryInfo.ff` | `S_IFMT`/`S_IFREG`/`S_IFDIR` 等 `i64` 常量 — 与 stat buffer 中读取的 `i64` 值比较，正确 |
| stat 字段值 | `EntryInfo.ff` | `readStatInfo()` 中 `modeRaw`/`size`/`atime`/`mtime`/`ctime` 等 `i64` — struct stat 字段为 64 位，正确 |
