# libuv 最小闭包同步方案

当前 `libuv` 采用“宿主平台最小闭包”同步策略，不引入完整上游仓库。

## 目标

- 只同步 Feng 当前需要的事件循环/异步 I/O/网络基础能力闭包。
- 同步物理目录固定为 `third_party/libuv`。
- 构建产物固定为 `std/extlib/<os-arch>/libfeng_std_uv.a`。

## 同步范围

`scripts/fetch_libuv.sh` 默认根据宿主平台（Darwin/Linux）同步：

- 公共头：`include/uv.h` 与 `include/uv/*.h` 中 Unix 所需最小头。
- 公共源：`src/` 下 `uv-common`、`threadpool`、`timer`、`inet`、`idna`、`random` 等核心单元。
- Unix 源：`src/unix/` 下 loop、poll、stream、tcp、udp、pipe、fs、process、thread、signal 等基础单元。
- 平台源：
  - Darwin: `darwin.c`、`darwin-proctitle.c`、`fsevents.c`、`kqueue.c` 等。
  - Linux: `linux.c`、`procfs-exepath.c`、`random-getrandom.c`、`random-sysctl-linux.c` 等。
- 递归 `#include "..."` 依赖闭包：脚本会自动补齐被源码引用到的内部头文件，避免手工维护散列表。

## 显式排除

- 不同步 Windows 源码与头文件。
- 不同步测试、benchmark、工具链配置、CI、文档与示例目录。
- 不同步 CMake/Autotools 工程文件作为构建输入（Feng 使用 vendored `Makefile`）。

## 构建约束

- `scripts/build_libuv.sh` 仅构建静态库，不生成动态库。
- `third_party/libuv/Makefile` 由同步脚本生成，包含平台宏与最小源码列表。
- Darwin 额外启用：`_DARWIN_UNLIMITED_SELECT=1`、`_DARWIN_USE_64_BIT_INODE=1`。
- 通用 Unix 宏：`_FILE_OFFSET_BITS=64`、`_LARGEFILE_SOURCE`。

## 使用方式

1. `./scripts/fetch_libuv.sh`
2. `./scripts/build_libuv.sh`

可选参数：

- `LIBUV_VERSION`：指定版本，默认 `1.49.2`。
- `LIBUV_SRC_URL`：覆盖下载地址。
- `LIBUV_PLATFORM`：强制平台（`darwin`/`linux`），默认自动检测。
