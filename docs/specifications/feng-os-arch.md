# Feng 平台标识规范

> 本文件是 Feng 体系内“平台标识”的主规范，定义 `<os>-<arch>[-<abi>]` 格式、合法值、适用范围与归一化映射。
> 其他文档（[feng-package.md](./feng-package.md) 的包目录、[feng-cli.md](./feng-cli.md) 的 `--platform` 参数、[feng-build.md](./feng-build.md) 的工具链转换、[feng-std-platform.md](./feng-std-platform.md) 的 API 行为、[../engineering/feng-release-and-install.md](../engineering/feng-release-and-install.md) 的分发物命名）仅引用本规范，不重复定义值域。

## 1 设计目标

- 为 Feng 体系内涉及平台与目标 ABI 的场景提供一个完整、统一的标识，避免各处自行命名导致不一致。
- Linux 平台必须在 OS 与 CPU 架构之后显式携带 C library ABI，不允许由 host 或 sysroot 路径隐式推断。
- 与 `std.platform` API 分离：本规范定义 Feng 自身的构建标识体系，`std.platform.SystemInfo` 如实返回平台原生 uname 值，两者职责不同，详见 §6。

## 2 适用范围

本规范适用于：

- **目标构建目录**：`<platform>/`。
- **包内原生制品目录**：`.fb` 包内的 `lib/<platform>/` 与 `extlib/<platform>/`。
- **工具发行目录**：runtime 与 sysroot 的目标制品目录。
- **分发物命名**：`feng-<version>-<platform>.zip`。
- **包标识字段**：`feng.fm` 的 `platform` 字段记录完整平台集合。
- **CLI 参数**：`--platform=<platform>` 的值域。
- **构建脚本归一化目标**：`scripts/` 下各构建脚本从 `uname` 检测后归一化到的 host 平台值。

**不适用**：

- `std.platform.SystemInfo.arch()` / `os()`：如实返回平台原生 uname 值，不归一化，详见 §6。
- LLVM、Clang 等外部工具使用的 target triple；Feng 构建层负责从平台与 ABI 转换，见 [feng-build.md](./feng-build.md)。

## 3 OS 标识

`<os>` 取值如下，使用小写：

| 标识 | 含义 | 备注 |
|------|------|------|
| `linux` | Linux | |
| `macos` | macOS | 使用 Apple 官方 marketing 名，不使用底层 uname 的 `Darwin` / `darwin` |
| `windows` | Windows | |

## 4 arch 标识

`<arch>` 取值如下，使用小写，采用短名风格（不使用 `aarch64` / `x86_64` / `amd64` 长名）：

| 标识 | 含义 | 对应 uname -m 原生值 |
|------|------|----------------------|
| `x64` | 64 位 x86 | `x86_64`、`amd64`、`AMD64` |
| `arm64` | 64 位 ARM | `arm64`（macOS）、`aarch64`（Linux）、`ARM64`（Windows） |
| `x86` | 32 位 x86 | `i386`、`i486`、`i586`、`i686` |
| `arm` | 32 位 ARM | `armv6l`、`armv7l` 等 |

## 5 平台格式与矩阵

平台标识格式为：

```text
<platform> ::= <os>-<arch>[-<abi>]
```

ABI 后缀是否存在由 OS 决定，不允许任意省略或追加：

- Linux 必须带 ABI，首版值为 `gnu` 或 `musl`。
- macOS 当前不带 ABI 后缀，因为 `macos` 已唯一确定其目标 ABI。
- `gnu` 表示 GNU/Linux glibc ABI；`musl` 表示 Linux musl ABI。

CLI 值是否合法由下表完整枚举决定，不能仅根据分段数量判断；对应工具链、runtime 与 sysroot 是否已经交付是独立的能力判断：

| 平台标识 | 首版产出 | 备注 |
|----------|---------|------|
| `macos-arm64` | 是 | Apple Silicon Mac |
| `macos-x64` | 否 | Intel Mac |
| `linux-x64-gnu` | 是 | Linux x86-64，glibc ABI |
| `linux-x64-musl` | 是 | Linux x86-64，musl ABI |
| `linux-arm64-gnu` | 是 | Linux AArch64，glibc ABI |
| `linux-arm64-musl` | 是 | Linux AArch64，musl ABI |
| `linux-x86-gnu` / `linux-x86-musl` | 否 | 32 位，低优先级 |
| `linux-arm-gnu` / `linux-arm-musl` | 否 | 32 位，低优先级 |
| `windows-x64` | 否 | |
| `windows-arm64` | 否 | |
| `windows-x86` | 否 | 32 位 |
| `windows-arm` | 否 | 32 位，极少见 |

首版目标制品目录直接使用完整平台标识：

```text
macos-arm64/
linux-x64-gnu/
linux-x64-musl/
linux-arm64-gnu/
linux-arm64-musl/
```

补充约束：

- `linux-x64`、`linux-arm64` 不是完整平台标识，不能作为编译目标。
- LLVM 工具自身也具有 host ABI。首版官方 Linux LLVM 可执行文件运行于 glibc host，因此 Linux 工具发行包是 `linux-x64-gnu`、`linux-arm64-gnu`；同一个 GNU-hosted Clang 仍可通过不同 target triple 与 sysroot 生成 GNU 或 musl 目标程序。
- `feng.fm` 的 `abi: "feng"` 表示包携带 Feng 正式库能力，与平台标识中的 Linux C library ABI 不是同一概念，严禁复用或重载。

## 6 与 `std.platform` API 的关系

`std.platform.SystemInfo.arch()` / `os()` 是**信息查询 API**，职责是如实返回 libuv `uv_os_uname` 的原生值，不归一化到本规范：

- `SystemInfo.os()` 返回 `darwin`（macOS）、`linux`（Linux）、`windows` / `windows_nt`（Windows）等原生值。
- `SystemInfo.arch()` 返回 `arm64`（macOS）、`aarch64`（Linux arm64）、`x86_64`、`amd64` 等原生值。

消费 `SystemInfo` 的代码需自行兼容多种原生值，不由本规范强制归一化。例如 `std/std/src/fs/Dir.ff` 在判断 dirent 结构偏移时，需同时接受 `arm64`、`aarch64`、`x86_64` 等。

这是分层设计：

- 本规范服务于目录命名、包标识、分发物命名、CLI 参数等构建、分发与工具链场景。
- `SystemInfo` 服务于运行时信息查询场景，如实返回底层值。

## 7 与 CLI 参数的关系

当前 `feng` CLI 的 `--target` 参数已用于**编译目标类型**（`bin` / `lib`），语义为产物形态，不是目标平台。

目标平台使用 `--platform=<platform>`。CLI 语法与默认行为由 [feng-cli.md](./feng-cli.md) 定义，Feng 完整平台到 Clang target triple / sysroot 的转换由 [feng-build.md](./feng-build.md) 定义。

约束：

- 顶层直编和项目命令的平台选择、默认行为及清单校验统一以 [feng-cli.md](./feng-cli.md)“项目平台选择统一规则”为准。
- 不接受 `aarch64`、`x86_64`、`amd64`、`darwin`、`glibc` 等别名；glibc ABI 的规范标识是 `gnu`。
- 标识合法但当前安装缺少对应工具链、runtime、sysroot 或 SDK 时，应报告“目标平台不可用”，不得误报为参数格式错误，也不得回退到 host 平台。

## 8 host 归一化映射

当从 `uname -s` / `uname -m` / `SystemInfo.os()` / `SystemInfo.arch()` 检测到的原生值需要映射到本规范时，按下表归一化：

**OS 映射**：

| 原生值 | 本规范 |
|--------|--------|
| `Darwin` / `darwin` | `macos` |
| `Linux` / `linux` | `linux` |
| `Windows` / `windows` / `Windows_NT` | `windows` |

**arch 映射**：

| 原生值 | 本规范 |
|--------|--------|
| `x86_64`、`amd64`、`AMD64` | `x64` |
| `arm64`、`aarch64`、`ARM64` | `arm64` |
| `i386`、`i486`、`i586`、`i686`、`x86` | `x86` |
| `armv6l`、`armv7l` 等 32 位 ARM 变体 | `arm` |

归一化需要同时确定 Linux host ABI。首版只发行 GNU-hosted Linux 工具，因此受支持 Linux host 的归一化结果固定追加 `-gnu`：

```text
Linux + x86_64/aarch64 → linux-x64-gnu / linux-arm64-gnu
Darwin + arm64         → macos-arm64
```

纯 musl host（例如未安装 glibc 兼容层的 Alpine）不映射成首版受支持的 Feng host 平台。该限制只描述 Feng 与 bundled LLVM 自身能否启动，不限制 `linux-*-musl` 目标程序的生成与运行。
