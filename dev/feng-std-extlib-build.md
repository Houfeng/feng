# Feng 标准库 extlib 预构建规范

本文档是标准库原生依赖预构建的主规范。完整平台标识及目录命名以
[feng-os-arch.md](../docs/feng-os-arch.md) 为准；各依赖的源码同步范围仍由各自
开发文档定义。

## 1 构建范围

§8.4 在三个发行 host 上分别生成以下原生静态库：

| 构建环境 | 输出目录 |
| --- | --- |
| `macos-arm64` | `std/extlib/macos-arm64/` |
| `linux-x64-gnu` | `std/extlib/linux-x64-gnu/` |
| `linux-arm64-gnu` | `std/extlib/linux-arm64-gnu/` |

每个目录必须包含：

- `libfeng_std_uv.a`
- `libfeng_std_unistring.a`
- `libfeng_std_pcre2.a`
- `libfeng_std_sodium.a`

本阶段只生成 native host 需要的 GNU/Linux 产物。`linux-*-musl` 目标产物随
§8.5 交叉编译能力实现，不得用 GNU 产物冒充。

## 2 工具链与 SDK / sysroot

- 所有依赖统一使用 `toolchain/llvm/<host-platform>/bin/clang` 与
  `llvm-ar`，不得隐式使用系统 C 编译器或归档器。
- macOS 使用 `xcrun --sdk macosx --show-sdk-path` 定位 SDK，并传入
  `--target=arm64-apple-macosx -isysroot <sdk>`。
- Linux 使用 `toolchain/sysroot/<host-platform>/`，并传入对应 target triple、
  `--sysroot` 与 `--gcc-toolchain`。预构建物不得引用构建容器自身高于该
  sysroot 的 glibc 符号。
- 构建脚本只接受当前执行环境由 `uname` 报告的 host 平台，不提供参数伪造
  另一 CPU 架构；使用指令集翻译环境验收时，必须明确记录其非物理原生性质。

## 3 构建隔离与发布

- 每次构建前必须清理 vendored Makefile 留下的对象，避免复用其他平台产物。
- 先写入 `temp/` 下的本次构建 staging 目录；构建和校验全部成功后，再发布到
  `std/extlib/<host-platform>/`。
- 每份归档必须使用 `file` 与 bundled `llvm-ar` 校验：归档非空，且全部对象的
  文件格式和 CPU 架构与 host 平台一致。
- `scripts/build_std_extlibs.sh` 依次构建四份依赖；单库脚本仍可独立执行。
- 失败时不得发布半成品，并应清理本次 staging 与 vendored 中间对象。

## 4 关联

- [feng-extlib-libuv.md](./feng-extlib-libuv.md)
- [feng-extlib-libsodium.md](./feng-extlib-libsodium.md)
- [feng-std-regexp-dev.md](./feng-std-regexp-dev.md)
- [feng-release-and-instanll.md](./feng-release-and-instanll.md)
