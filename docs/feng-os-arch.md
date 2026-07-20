# Feng OS 与 Arch 标识规范

> 本文件是 Feng 体系内"平台标识"（os + arch）的主规范,定义枚举值、适用范围与归一化映射。
> 其他文档（[feng-package.md](./feng-package.md) 的 `arch` 字段、[feng-cli.md](./feng-cli.md) 的 `--platform` 参数、[feng-build.md](./feng-build.md) 的工具链转换、[feng-std-platform.md](./feng-std-platform.md) 的 API 行为、[../dev/feng-release-and-instanll.md](../dev/feng-release-and-instanll.md) 的分发物命名)仅引用本规范,不重复定义。

## 1 设计目标

- 为 Feng 体系内涉及"平台标识"的场景提供统一的 os / arch 枚举,避免各处自行命名导致不一致。
- 与 `std.platform` API 分离:本规范定义 Feng 自身的标识体系,`std.platform.SystemInfo` 如实返回平台原生 uname 值,两者职责不同,详见 §6。

## 2 适用范围

本规范适用于:

- **目录命名**:`extlib/<os>-<arch>/`、`.fb` 包内 `lib/<os>-<arch>/` 与 `extlib/<os>-<arch>/`、`std/extlib/<os>-<arch>/` 等。
- **分发物命名**:`feng-<version>-<os>-<arch>.zip`。
- **包标识字段**:`feng.fm` 的 `arch` 字段取值。
- **CLI 参数**:`--platform=<os>-<arch>` 的平台标识取值(见 §7)。
- **构建脚本归一化目标**:`scripts/` 下各构建脚本从 `uname` 检测后归一化到的目标值。

**不适用**:

- `std.platform.SystemInfo.arch()` / `os()`:如实返回平台原生 uname 值,不归一化,详见 §6。
- 消费 `SystemInfo` 的代码(如 `std/src/fs/Dir.ff`):需自行兼容 SystemInfo 返回的多种原生值,不由本规范强制归一化。

## 3 os 标识

`<os>` 取值如下,使用小写:

| 标识 | 含义 | 备注 |
|------|------|------|
| `linux` | Linux | |
| `macos` | macOS | 使用 Apple 官方 marketing 名,不使用底层 uname 的 `Darwin` / `darwin` |
| `windows` | Windows | |

## 4 arch 标识

`<arch>` 取值如下,使用小写,采用短名风格(不使用 `aarch64` / `x86_64` / `amd64` 长名):

| 标识 | 含义 | 对应 uname -m 原生值 |
|------|------|----------------------|
| `x64` | 64 位 x86 | `x86_64`、`amd64`、`AMD64` |
| `arm64` | 64 位 ARM | `arm64`(macOS)、`aarch64`(Linux)、`ARM64`(Windows) |
| `x86` | 32 位 x86 | `i386`、`i486`、`i586`、`i686` |
| `arm` | 32 位 ARM | `armv6l`、`armv7l` 等 |

## 5 平台矩阵

`<os>-<arch>` 组合如下。CLI 值是否合法由本表决定；对应工具链、runtime 与 sysroot 是否已经交付是独立的能力判断:

| 平台标识 | 首版产出 | 备注 |
|----------|---------|------|
| `macos-arm64` | 是 | Apple Silicon Mac |
| `macos-x64` | 否 | Intel Mac |
| `linux-x64` | 是 | glibc native；musl 交叉编译 sysroot |
| `linux-arm64` | 是 | glibc native；musl 交叉编译 sysroot |
| `linux-x86` | 否 | 32 位,低优先级 |
| `linux-arm` | 否 | 32 位,低优先级 |
| `windows-x64` | 否 | |
| `windows-arm64` | 否 | |
| `windows-x86` | 否 | 32 位 |
| `windows-arm` | 否 | 32 位,极少见 |

## 6 与 `std.platform` API 的关系

`std.platform.SystemInfo.arch()` / `os()` 是**信息查询 API**,职责是如实返回 libuv `uv_os_uname` 的原生值,不归一化到本规范:

- `SystemInfo.os()` 返回 `darwin`(macOS)、`linux`(Linux)、`windows` / `windows_nt`(Windows)等原生值。
- `SystemInfo.arch()` 返回 `arm64`(macOS)、`aarch64`(Linux arm64)、`x86_64`、`amd64` 等原生值。

消费 `SystemInfo` 的代码需自行兼容多种原生值,不由本规范强制归一化。例如 `std/src/fs/Dir.ff` 在判断 dirent 结构偏移时,需同时接受 `arm64`、`aarch64`、`x86_64` 等。

这是**分层设计**:
- 本规范(Feng 自身标识体系)服务于目录命名、包标识、分发物命名、CLI 参数等"构建/分发/工具链"场景。
- `SystemInfo` 服务于"运行时信息查询"场景,如实返回底层值。
- 两层"不一致"是 by design,与 Python `platform.machine()`(如实返回)与 wheel tag(归一化)的关系一致。

## 7 与 CLI 参数的关系

当前 `feng` CLI 的 `--target` 参数已用于**编译目标类型**(`bin` / `lib`),语义为"产物形态",不是"目标平台"。

目标平台使用独立的 `--platform=<os>-<arch>` 参数；CLI 语法与默认行为由 [feng-cli.md](./feng-cli.md) 定义，Feng 平台到 Clang target triple / sysroot 的转换由 [feng-build.md](./feng-build.md) 定义。

约束:
- `--platform` 的取值必须使用本规范 §3 / §4 的 os / arch 枚举,并且是 §5 定义的 `<os>-<arch>` 组合(如 `--platform=linux-arm64`)。
- 不引入 `aarch64` / `x86_64` / `amd64` / `darwin` 等本规范之外的别名。
- 未指定 `--platform` 时使用归一化后的 host 平台。
- 平台标识合法但当前安装缺少对应工具链、runtime 或 sysroot 时,应报告“目标平台不可用”,不得误报为参数格式错误。

## 8 归一化映射

当从 `uname -s` / `uname -m` / `SystemInfo.os()` / `SystemInfo.arch()` 检测到的原生值需要映射到本规范时,按下表归一化:

**os 映射**:

| 原生值 | 本规范 |
|--------|--------|
| `Darwin` / `darwin` | `macos` |
| `Linux` / `linux` | `linux` |
| `Windows` / `windows` / `Windows_NT` | `windows` |

**arch 映射**:

| 原生值 | 本规范 |
|--------|--------|
| `x86_64`、`amd64`、`AMD64` | `x64` |
| `arm64`、`aarch64`、`ARM64` | `arm64` |
| `i386`、`i486`、`i586`、`i686`、`x86` | `x86` |
| `armv6l`、`armv7l` 等 32 位 ARM 变体 | `arm` |

归一化逻辑由构建层(`Makefile`、`scripts/` 下各脚本)实现,`SystemInfo` 不参与归一化。
