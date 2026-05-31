# libuv 最小闭包同步方案

当前 `libuv` 采用“三平台统一最小闭包”同步策略，不引入完整上游仓库。

## 目标

- 只同步 Feng 当前需要的基础运行时能力闭包：
  - 事件循环 / 异步 I/O
  - 操作系统信息
  - 内存信息
  - CPU 信息
  - 进程与线程
  - 网络设备信息（网卡、IP、MAC）
  - DNS
  - TCP / UDP
- 同步物理目录固定为 `third_party/libuv`。
- 构建产物固定为 `std/extlib/<os-arch>/libfeng_std_uv.a`。

## 同步范围

`scripts/fetch_libuv.sh` 默认一次性同步 Linux/macOS/Windows 三平台共同所需闭包：

- 公共头：`include/uv.h` 与 `include/uv/*.h` 最小公开头集。
- 公共源：`src/` 下 `uv-common`、`threadpool`、`timer`、`inet`、`idna`、`random` 等核心单元。
- Unix 基础源：`src/unix/` 下 loop、poll、stream、tcp、udp、pipe、fs、process、thread、signal、DNS 查询等单元。
- Linux 专属源：`linux.c`、`procfs-exepath.c`、`random-getrandom.c`、`random-sysctl-linux.c` 等。
- macOS 专属源：`darwin.c`、`darwin-proctitle.c`、`fsevents.c`、`kqueue.c`、`bsd-ifaddrs.c` 等。
- Windows 专属源：`src/win/` 下 async/core/fs/process/thread/tcp/udp/getaddrinfo/getnameinfo/winsock 等单元。
- 递归 `#include "..."` 依赖闭包：脚本自动补齐被选中源码引用到的内部头文件，避免手工维护散列表。

## 显式排除

- 不同步测试、benchmark、工具链配置、CI、文档与示例目录。
- 不同步 CMake/Autotools 工程文件作为构建输入（Feng 使用 vendored `Makefile`）。

## 构建约束

- `scripts/build_libuv.sh` 仅构建静态库，不生成动态库。
- `third_party/libuv/Makefile` 由同步脚本生成，按宿主平台选择源码列表：Linux/macOS/Windows。
- Darwin 额外启用：`_DARWIN_UNLIMITED_SELECT=1`、`_DARWIN_USE_64_BIT_INODE=1`。
- Linux 额外启用：`_GNU_SOURCE`、`_POSIX_C_SOURCE=200112`。
- Unix 通用宏：`_FILE_OFFSET_BITS=64`、`_LARGEFILE_SOURCE`。

## 使用方式

1. `./scripts/fetch_libuv.sh`
2. `./scripts/build_libuv.sh`

可选参数：

- `LIBUV_VERSION`：指定版本，默认 `1.49.2`。
- `LIBUV_SRC_URL`：覆盖下载地址。
