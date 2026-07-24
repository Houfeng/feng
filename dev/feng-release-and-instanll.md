# Feng 分发与安装方案

> 本方案收敛 Feng 工具链（编译器 + 运行时静态库 + 精简 toolchain）的分发包结构、构建发布工作流、安装方式。
> `.fb` 包格式（feng 项目间源码级闭源分发）由 [feng-package.md](../docs/feng-package.md) 单独定义，不在本文件重复。
> CLI 命令与 `--platform` 选项由 [feng-cli.md](../docs/feng-cli.md) 单独定义，`<os>-<arch>[-<abi>]` 完整平台标识由 [feng-os-arch.md](../docs/feng-os-arch.md) 统一定义，工具链选择与 Clang target / sysroot 转换由 [feng-build.md](../docs/feng-build.md) 定义；本方案只定义分发与安装布局及实施阶段。

## 1 目标与范围

本方案解决一件事：让用户在目标机器上拿到一个可用的 Feng 工具链，并能在多种平台上以一致方式安装。

首版明确覆盖：

- 分发物：单一压缩包 `feng-<version>-<platform>.zip`
- host 平台：`macos-arm64`、`linux-x64-gnu`、`linux-arm64-gnu`
- 用户程序目标平台：`macos-arm64`、`linux-x64-gnu`、`linux-x64-musl`、`linux-arm64-gnu`、`linux-arm64-musl`
- 安装方式：手动解压 + 在线脚本两种
- toolchain 形态：bundle 精简版（从同一 host 平台的 LLVM 官方预编译包剥离，核心保留 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap`，不自建 LLVM）
- 运行时分发形态：仅静态库 `.a` / `.lib`
- 分发渠道：GitHub Releases

首版明确不做：

- Windows 实际打包（脚本与工作流预留扩展点，不产出二进制）
- Linux host 交叉链接 macOS 可执行程序。Apple SDK 不随 Feng 分发，且标准 [Xcode and Apple SDKs Agreement](https://www.apple.com/legal/sla/docs/xcode.pdf) 不允许单独使用 Apple SDK 或在非 Apple 品牌硬件上运行 Apple Software；但 `target=lib` 只生成 Mach-O 对象和静态归档，不执行最终链接，其 SDK-free 跨 host 能力由 §9 单独交付。
- 动态运行时库 `.so` / `.dylib` / `.dll`
- 版本管理器（fengvm 类工具）
- 包管理器集成（Homebrew tap / apt repo / winget）
- 代码签名与公证（macOS notarization、Windows code signing）
- 自更新命令 `feng upgrade`

## 2 设计原则

- **抽象驱动，面向未来可扩展**：平台矩阵与分发渠道均以参数化形式表达，新增平台不改动脚本主干。
- **单一分发物**：一个 zip 覆盖一个目标平台，不按子组件拆分多包，降低用户组合安装成本。
- **安装极简**：安装只需将解压目录的 `bin/` 加入 `PATH`，不引入 `FENG_HOME` / `FENG_TOOLCHAIN` 等环境变量；runtime lib/include 与 toolchain 均由 `feng` 自身查找定位。
- **可逆安装**：安装产物集中在一个目录树下，清理只需删除该目录与 shell 片段。
- **与现有体系一致**：复用 `Makefile` 产物路径、Feng 编译期 `extlib/<platform>/` 平台隔离约定与 `scripts/` 下既有构建脚本，不另立构建体系。

## 3 命名规范与平台矩阵

### 3.1 分发物命名

```text
feng-<version>-<platform>.zip
```

- `feng` 后紧跟 `<version>`，明确表示该版本为 Feng 自身版本；`<platform>` 作为 host 平台后缀。
- `<platform>` 取值见 [feng-os-arch.md](../docs/feng-os-arch.md)，不在本文件重复定义。首版 Linux 工具运行于 GNU/glibc host，因此发行包名称必须包含 `-gnu`。
- `<version>` 与 git tag 一致，形如 `0.1.0`、`0.2.0-rc.1`，不带前缀 `v`

示例：`feng-0.1.0-macos-arm64.zip`、`feng-0.1.0-linux-x64-gnu.zip`

### 3.2 平台矩阵

| 平台标识     | 首版产出 | 扩展预留 | 备注                                  |
|--------------|---------|---------|---------------------------------------|
| `macos-arm64` | 是      | -       | Feng 编译期 extlib 依赖已就绪          |
| `macos-x64`  | 否      | 是      | Intel Mac，后续按需补齐               |
| `linux-x64-gnu`  | 是  | -       | GNU-hosted LLVM；内建 x64 / arm64 的 GNU 与 musl 目标 sysroot |
| `linux-arm64-gnu`| 是  | -       | GNU-hosted LLVM；内建 x64 / arm64 的 GNU 与 musl 目标 sysroot |
| `windows-x64`| 否      | 是      | 静态库后缀切换为 `.lib`，可执行为 `.exe` |
| `windows-arm64`| 否    | 是      | -                                     |

矩阵在 CI 工作流与安装脚本中以表驱动形式表达，新增平台只改表项，不改主干逻辑。

## 4 压缩包目录结构

解压后顶层目录名与压缩包主名一致：`feng-<version>-<platform>/`。

```text
feng-<version>-<platform>/
├── bin/                          # 必须：Feng 可执行
│   └── feng                      # 编译器 + CLI 主入口（含 lsp / dap 子命令）
├── include/                      # 必须：runtime 公共 ABI 头文件（平台无关）
│   ├── feng_runtime.h
│   └── feng_runtime_contract.inc
├── lib/                          # 必须：运行时静态库（按完整目标平台分子目录）
│   ├── macos-arm64/
│   │   └── libfeng_runtime.a
│   ├── linux-x64-gnu/
│   │   └── libfeng_runtime.a
│   ├── linux-x64-musl/
│   │   └── libfeng_runtime.a
│   ├── linux-arm64-gnu/
│   │   └── libfeng_runtime.a
│   └── linux-arm64-musl/
│       └── libfeng_runtime.a
│  
├── toolchain/                    # 必须：精简 LLVM 工具链 + 交叉编译 sysroot
│   ├── llvm/                     # 同一 LLVM 官方包精简后的统一根目录
│   │   ├── bin/
│   │   │   ├── clang          # C/LLVM 后端驱动
│   │   │   ├── lld            # LLVM linker，所有 Linux GNU / musl 目标链接使用
│   │   │   ├── ld.lld -> lld  # Clang / driver 调用入口
│   │   │   ├── llvm-ar        # 跨目标静态归档工具
│   │   │   ├── llvm-ranlib    # 跨目标静态归档索引工具
│   │   │   ├── lldb           # 命令行调试器
│   │   │   └── lldb-dap       # DAP 适配器，供 feng dap / VS Code 使用
│   │   └── lib/
│   │       ├── clang/22/      # clang 官方 resource-dir，保持与 bin/clang 的相对位置
│   │       │   ├── include/   # 编译器内置头文件（平台无关）
│   │       │   └── lib/<os>/  # 编译器运行时库（目标 OS）
│   │       └── liblldb.*       # lldb / lldb-dap 运行所必需的库
│   └── sysroot/                  # Linux 目标 sysroot（native / 交叉共用）
│       ├── linux-x64-gnu/
│       ├── linux-x64-musl/
│       ├── linux-arm64-gnu/
│       └── linux-arm64-musl/
│           ├── usr/include/      # 目标平台系统头文件
│           ├── usr/lib/          # libc、动态加载器、CRT 与 linker scripts
│           │   └── gcc/<target-triple>/<runtime-version>/
│           │       └── crtbegin* / crtend* / compiler runtime
│           ├── lib -> usr/lib    # Clang / toolchain 目录兼容视图
│           └── <target-triple>/  # Clang GCC toolchain 检测所需的目标视图
│               ├── include -> ../usr/include
│               └── lib -> ../usr/lib
└── VERSION                       # 必须：纯文本版本号，单行
```

- Windows 平台下，`bin/` 中可执行文件追加 `.exe` 后缀，`lib/` 中静态库后缀切换为 `.lib`。
- `lib/` 按完整目标平台分子目录。首版三份分发包都包含五份 runtime；macOS runtime 只能在合法 macOS 构建环境生成，Linux GNU / musl runtime 分别使用对应 bundled sysroot 构建。每份 runtime 必须校验对象格式、CPU 架构与完整平台，发布汇聚任务不得把其他平台或其他 libc ABI 的 runtime 改名复用。
- `include/` 为 runtime 公共 ABI 头文件，平台无关（C 源码），不分平台子目录，单一一份供所有平台使用，扁平置于 `include/` 根下。`feng_runtime.h` 内部以相对路径 `#include "feng_runtime_contract.inc"`，二者位于同一目录。最终编译 / 链接所需的标准 C 头文件（`<stdint.h>` 等）与系统头文件（`<unwind.h>`）由目标平台 SDK / sysroot 提供，不复制到 runtime include 目录；§9.4 的 SDK-free `target=lib` 编译头闭包也不得通过复制 Apple SDK 头文件实现。
- `toolchain/llvm/` 保持 LLVM 官方包的统一根目录布局，`clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb` 与 `lldb-dap` 必须来自同一 LLVM 版本和同一 host 平台包。官方 LLVM Linux 可执行文件本身是 GNU/glibc 动态程序，不提供独立 musl host 包；一份 GNU-hosted Clang 可生成 GNU 与 musl 两类目标，因此每份 Linux 发行包只携带当前 host 架构的一份 LLVM，不按用户程序目标 ABI复制。
- `toolchain/sysroot/` 按完整 Linux 目标平台分子目录，同时内建 GNU/glibc 与 musl。每份 sysroot 都必须保持 `--sysroot` 约定的头文件、目标 libc、动态加载器、CRT、linker scripts、compiler runtime 及 Clang 检测所需相对目录关系；不保留只能在特定宿主上运行的 GCC、binutils 或 musl.cc 工具可执行文件。GNU 与 musl sysroot 均属于目标输入，native 与交叉编译使用同一份。具体调用参数见 [feng-build.md](../docs/feng-build.md)。
- GNU/glibc sysroot 的来源与裁剪规则以 §5.3 为准，必须记录确切来源、版本、目标架构、裁剪清单及 LGPL 等上游许可材料，并满足二进制再分发所需的许可与源码提供义务；不得把 host `/usr` 临时复制进发行包。musl sysroot 同样记录 musl 与配套 compiler runtime 的独立来源及许可证。
- 分发物不包含任何 Feng 源码、`.o` / `.obj` 中间产物、构建缓存。
- `feng` 编译器基于自身位置查找 runtime 静态库、头文件与 toolchain：runtime 位于 `<feng 可执行文件目录>/../lib/` 与 `../include/`，Clang 和 `lldb-dap` 位于 `<feng 可执行文件目录>/../toolchain/llvm/bin/`。不引入 `FENG_HOME` / `FENG_TOOLCHAIN` 等环境变量；完整查找顺序见 [feng-build.md](../docs/feng-build.md) 与 [feng-cli.md](../docs/feng-cli.md) 的 DAP 规范。

## 5 toolchain 形态

分发包内 `toolchain/` 为精简版 LLVM 工具链与交叉编译 sysroot，与 `bin/`、`lib/`、`include/` 并列置于分发包根下。

- **从 LLVM 官方预编译包剥离，不自建 LLVM/Clang**；核心只保留 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap` 及其运行所必需的最小依赖集，不含其他 `llvm-*`、`clang-format`、`clang-tidy` 等通用 LLVM 工具。所有 Linux GNU / musl 目标统一使用当前 host LLVM 包中的 `lld`，不分发 sysroot 来源包中的 linker 可执行文件。
- 精简由 `scripts/fetch_llvm.sh` + `scripts/trim_llvm.sh` 完成（维护性脚本，不在发布流程）：`fetch_llvm.sh` 下载并解压 LLVM 官方预编译包到 `local/llvm/`，并为 Linux host 下载 §5.2 固定的私有运行库来源包；`trim_llvm.sh` 从单个已解压 LLVM root 中同时精简 clang、lld、lldb 与 lldb-dap，为 Linux 产物提取并校验完整私有动态依赖闭包，原子产出到仓库 `toolchain/llvm/<host-platform>/`。`local/llvm/` 是 gitignored 的持久 cache，不受 `make clean` 或测试清理 `temp/` 影响。全部 LLVM 工具必须来自同一 LLVM 版本和同一 host 平台包，多个工具不得分别精简到共享输出根。
- `toolchain/sysroot/` 为 Linux native / 交叉编译共用的目标 sysroot，最终产物固定为 `linux-x64-gnu`、`linux-x64-musl`、`linux-arm64-gnu`、`linux-arm64-musl` 四份并由 git lfs 管理。musl 继续由维护脚本从 musl.cc 配套包精简；GNU/glibc 按 §5.3 由 `scripts/fetch_gnu_sysroot.sh` + `scripts/trim_gnu_sysroot.sh` 从固定 Debian cross packages 构造。维护脚本只读写 `local/` cache 与仓库 toolchain 产物，不在用户构建或 CI 发布时从 host 系统抓取内容。CI checkout 后直接复制，不临时下载或重建 sysroot。
- 每份精简 toolchain 产物以自身 README / manifest 记录实际版本、来源、校验值、许可证与剥离清单；本文件只定义统一来源政策和验收边界，不重复维护逐文件 manifest。
- `feng` 编译器基于自身位置查找 `toolchain/`。源码开发继续使用现有 `build/` 根；Makefile 在 `build/toolchain/` 下创建 `llvm -> ../../toolchain/llvm/<host-platform>` 与 `sysroot -> ../../toolchain/sysroot` 两个软链接，使 `build/bin/feng` 观察到的 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 都与发行包布局一致，不要求为 Feng 自身引入 `build/<platform>/` 多目标构建体系。`make clean` 删除整个 `build/`，软链接不作为持久产物或分发内容。

### 5.1 macOS 系统前置条件

macOS SDK 不随 Feng 分发。使用 Feng 内置 Clang 编译 macOS 目标时，系统必须已安装 Xcode 或 Xcode Command Line Tools，以提供 macOS SDK、系统链接器与 `xcrun`。用户可执行以下命令安装 Command Line Tools:

```bash
xcode-select --install
```

Feng 在 macOS host 最终链接 macOS 可执行程序时，通过 `xcrun --sdk macosx --show-sdk-path` 获取 SDK 路径并向内置 Clang 传入 `-isysroot`。命令不可用、未选中有效 developer directory 或 SDK 不存在时,编译必须给出明确诊断；安装脚本不得静默安装 Xcode 或 Command Line Tools。该前置条件不适用于 §9 定义的 SDK-free `target=lib` 对象生成与静态归档，也不改变 `feng --version` 等无需编译的命令。

### 5.2 Linux LLVM host 动态依赖与支持边界

LLVM 22.1.8 官方 Linux x64 与 arm64 包是 GNU/glibc host 程序，不是 musl 程序。已验证的直接依赖中，`lld` 需要 `libxml2.so.2`，`liblldb.so.22.1.8` 需要 `libpython3.11.so.1.0`、`libxml2.so.2`、`libncurses.so.6`、`libpanel.so.6`、`libform.so.6`、`libtinfo.so.6`，并同时依赖其完整的 `DT_NEEDED` 传递闭包。官方 LLVM 包本身不携带这些全部共享库。

Linux Feng 发行包必须开箱即用，不得要求用户为 bundled LLVM 手工安装 `libpython3.11`、`libxml2`、ncurses、特定版本的 `libstdc++` 或其他非 glibc 运行库。系统边界只保留 Linux 内核、动态加载器及 glibc 所属基础库；LLVM 需要的其余直接和传递动态依赖必须作为私有运行库置于 `toolchain/llvm/lib/`。所有 bundled LLVM 可执行文件和私有库必须通过相对 RPATH / RUNPATH 定位该目录，不依赖 `LD_LIBRARY_PATH`，也不得从目标 sysroot 加载 host 运行库。

Linux 私有运行库使用经过 ABI 实测的固定来源：`libxml2`、xz、zlib、`libpython3.11.so.1.0` 与 `libgcc_s.so.1` 使用同架构 AlmaLinux 8.10 BaseOS / AppStream RPM；`libncurses.so.6`、`libpanel.so.6`、`libform.so.6` 与 `libtinfo.so.6` 统一使用同架构 Ubuntu 22.04 Jammy security 的 ncurses `6.3-2ubuntu0.2`；`libstdc++.so.6.0.30` 使用同架构 Ubuntu 22.04 updates 的 `libstdc++6 12.3.0-1ubuntu1~22.04.3`。Ubuntu ncurses 提供官方 `liblldb` 要求的 `NCURSES6_*` / `NCURSES6_TINFO_*` 版本化符号且最高只要求 `GLIBC_2.34`；AlmaLinux 8 ncurses 只提供未版本化符号，禁止作为官方 LLVM 22.1.8 `liblldb` 的私有库来源。Ubuntu `libstdc++6` 同时提供 LLVM 需要的 `GLIBCXX_3.4.30` 且两架构最高只要求 `GLIBC_2.34`；AlmaLinux 8 的 GCC Toolset 12 只提供指向系统 `libstdc++.so.6` / `libgcc_s.so.1` 的 linker script 和 `libstdc++_nonshared.a`，不能满足已链接完成的官方 LLVM，禁止作为这两个私有共享库的来源。CPython 3.11.9 官方 `LICENSE` 与 Ubuntu `gcc-12-base` 的同版本包作为缺失许可证正文的固定来源，但后者的命令和其他运行文件不得进入产物。`scripts/fetch_llvm.sh` 必须固定每个 RPM / DEB / 许可证文件的来源位置、文件名、版本与 SHA-256；`scripts/trim_llvm.sh` 只提取实际依赖的共享库及 soname 链，并记录来源、许可证和裁剪清单，不携带包中的解释器、命令、头文件、GDB Python 脚本、包管理元数据或其他无关文件。已知 soname 只是闭包计算的起点，脚本必须对最终产物递归校验 `DT_NEEDED`，不得把固定库名当成完整清单。

Feng 永不使用或支持 Python 脚本。`libpython3.11.so.1.0` 用于满足官方 `liblldb` 的 ELF 直接依赖；官方 Linux `liblldb` 创建调试器时仍会初始化 Python 的文件系统编码，因此发行包额外只保留与该库同版本的 Python 3.11 `encodings` 包，不包含 Python 可执行文件、其余标准库、LLDB Python bindings、第三方模块或通用脚本能力。`lldb` 与 `lldb-dap` 的工具链内部启动器根据自身位置设置私有相对 `PYTHONHOME` 后执行原始 ELF，用户无需配置环境变量，私有 Python 路径也不得作为 Feng CLI 的工具链定位接口。§8.1 必须在这样的裁剪结果上完成真实 `lldb` / `lldb-dap` 基础调试会话，而不能只验证进程能够输出版本号；任何基础调试流程若仍要求 `encodings` 之外的 Python 运行时内容，该产物不得通过验收，也不得未经人工决策继续扩大 Python 运行时范围。

首版 Linux host ABI 下限固定为 glibc 2.34。`linux-x64-gnu` 不得引入 x86-64-v2 或更高的隐式 CPU 基线，`linux-arm64-gnu` 使用通用 AArch64 基线。每次重新提取必须同时校验 LLVM 可执行文件、`liblldb` 和全部私有库的最高 `GLIBC_*` / `GLIBCXX_*` 要求与 ELF CPU 属性；任一文件超过基线时必须停止生成产物。

首版 Linux host 支持范围是满足上述 ABI / CPU 基线并通过 §8.1 干净环境验收的主流 GNU/glibc 发行版，至少包括 Ubuntu 22.04 / 24.04 / 26.04、Debian 12 / 13 与 AlmaLinux 9 系列的 x64、ARM64 对应环境。纯 musl Alpine 不作为 Feng 编译器和 bundled LLVM 的 host；该限制只影响工具自身启动，`linux-*-musl` 目标程序仍可生成，并在静态链接后于 Alpine 运行。目标 GNU / musl sysroot 是用户程序的编译输入，不能解决 LLVM host 可执行文件自身的动态依赖。

### 5.3 Linux GNU sysroot 来源与裁剪

`linux-x64-gnu` 与 `linux-arm64-gnu` sysroot 统一以 Debian 11 Bullseye 官方 cross packages 为来源，固定 glibc 2.31 目标 ABI 基线。选择较低的目标 glibc 基线是为了提高 Feng 生成程序的运行兼容性，不改变 §5.2 中 LLVM host 自身的 glibc 2.34 下限。两者职责严格分离：LLVM 私有运行库只供 host 工具启动，GNU sysroot 只作为目标程序的头文件、对象与链接输入。

每个目标架构的来源集合固定为对应的 `libc6-<deb-arch>-cross`、`libc6-dev-<deb-arch>-cross`、`linux-libc-dev-<deb-arch>-cross`、`libgcc-s1-<deb-arch>-cross` 与 `libgcc-10-dev-<deb-arch>-cross`，其中 `linux-x64-gnu` 对应 Debian `amd64`，`linux-arm64-gnu` 对应 Debian `arm64`。首版固定 Debian Bullseye cross-toolchain-base 的 glibc `2.31-9cross4`、Linux userspace headers `5.10.13-1cross4` 与 GCC runtime `10.2.1-6cross1`；后续升级必须作为显式工具链基线变更单独 Review，不得由仓库 `latest` 状态自动漂移。

`scripts/fetch_gnu_sysroot.sh` 必须从 Debian 官方 archive / snapshot 的不可变地址下载固定 `.deb` 到 `local/sysroot/gnu/`，逐项校验文件名、版本与 SHA-256 后再解包；不得调用 host 包管理器解析当前最新版本。`scripts/trim_gnu_sysroot.sh` 从缓存原子生成 `toolchain/sysroot/linux-x64-gnu/` 与 `toolchain/sysroot/linux-arm64-gnu/`，仅保留目标公开 C / Linux userspace 头文件、glibc 动态与链接所需文件、动态加载器、CRT、linker scripts、`libgcc_s`、`libgcc.a`、`libgcc_eh.a`、GCC CRT 及 Clang GCC installation detector 所需目录和相对链接。GCC、binutils、`ld`、包维护脚本以及其他 host 可执行文件一律排除。

每份 GNU sysroot 必须内建 README / manifest，记录所有二进制包与对应源码包的精确地址、版本、SHA-256、许可证、裁剪清单和源码提供方式。剪裁验收必须检查目标 ELF 架构、动态加载器、CRT、linker scripts、compiler runtime、全部符号链接，以及目录中不存在 host ELF 可执行工具；随后按 §8.1 分别完成 native、Linux 跨架构和 macOS host 的最小 C 编译、链接与目标运行验证。

## 6 构建与发布工作流

### 6.1 触发

GitHub Actions 工作流 `release.yml` 由推送形如 `v*.*.*` 的 tag 触发，并支持 `workflow_dispatch` 手动触发用于试发。

### 6.2 原生构件 matrix

```yaml
strategy:
  matrix:
    include:
      - { platform: macos-arm64,     runner: macos-14 }
      - { platform: linux-x64-gnu,   runner: ubuntu-latest }
      - { platform: linux-arm64-gnu, runner: ubuntu-24.04-arm }
      # 扩展时追加：macos-x64、windows-x64 等
```

三个 matrix 项互不交叉构建 Feng 可执行文件，只在对应 native host 产出当前 host 平台的 `feng`。macOS 任务生成 `macos-arm64` runtime；两个 Linux 任务分别使用同架构 GNU / musl sysroot 生成该架构的两份 runtime。各 host 平台的精简 LLVM 产物位于仓库 `toolchain/llvm/<host-platform>/`，四份 Linux 目标 sysroot 位于仓库 `toolchain/sysroot/<platform>/`，均由 git lfs 管理。

三个原生构件任务全部成功后，独立汇聚任务下载三份构件并组装三份 `feng-<version>-<platform>.zip`。因此发布流程不要求 Linux 生成 macOS runtime，也不要求 macOS 或任一 Linux host 单独产生完整 release zip。

### 6.3 原生构建与汇聚步骤

原生构件任务：

1. checkout 仓库（含 git lfs 管理的精简 LLVM 与 Linux sysroot）
2. 安装当前 host 的构建依赖
3. 执行 `scripts/build_libunwind.sh`：macOS 生成一份，Linux 分别使用同架构 GNU / musl sysroot 生成两份 ABI 匹配的维护性中间产物
4. 执行现有 native `make cli` 生成当前 host 的 `build/bin/feng`；每个任务只生成 §6.2 分配给它的一份或两份 runtime / unwind，三个任务合计产出五份最终 `libfeng_runtime.a` 与内容一致的一套公共 `build/include/`，但不向 Makefile 增加用于交叉构建 Feng 可执行文件自身的 `TARGET_PLATFORM`
5. 校验 `feng`、runtime 与 unwind 的格式和 CPU 架构，执行当前 host 全量 `make test`
6. 上传 `release-component-<host-platform>`，其中 `feng` 与 runtime 均带明确平台元信息，公共头文件同时记录内容摘要

汇聚任务：

1. 等待并下载三份原生构件，不执行任何跨 host 编译
2. 校验五份 runtime 的完整平台互异且完整，公共头文件内容一致
3. 针对每个 host 平台选择对应 `feng` 与 `toolchain/llvm/<host-platform>/`
4. 将五份 runtime、四份 Linux sysroot、公共头文件及 `VERSION` 放入每一份分发目录
5. 执行 `scripts/release.sh` 的纯组装与校验入口，生成三份规范命名的 zip
6. 上传到 GitHub Release 对应 tag

toolchain 精简产物在**本地维护**：开发者使用 LLVM、musl 与 GNU sysroot 的 fetch / trim / build 维护脚本产出各平台精简结果，提交到仓库（git lfs）。CI 只需 checkout 后从仓库目录复制，不做任何精简。这样 CI 构建步骤简、快，且不依赖上游下载站点在发布时可达。

### 6.4 失败与回滚

- 任一原生构件任务失败时，汇聚任务不得生成任何 release zip；已成功的平台构件只作为本次失败工作流的诊断输入，不得单独发布。
- 汇聚任务失败时不得上传部分平台 release zip。
- 已发布 tag 不允许覆盖；发现问题时发新 tag，不在旧 tag 上重打。

## 7 安装方式

### 7.1 手动解压

适用于：离线环境、自定义安装路径、对安装脚本不信任的用户。

步骤：

1. 从 GitHub Releases 下载 `feng-<version>-<platform>.zip`
2. 解压到任意目录，例如 `~/.feng/`
3. 将解压目录的 `bin/` 加入 `PATH`（在 shell 启动脚本中追加 `export PATH="<解压目录>/bin:$PATH"`）
4. macOS 用户如需编译且尚未安装 Xcode / Command Line Tools,执行 `xcode-select --install`
5. 重开 shell 或 `source` 当前会话，`feng --version` 可用即安装成功

### 7.2 在线脚本

适用于：新机器快速上手、CI 环境、首版默认推荐路径。

入口（首版托管在 GitHub raw）：

```bash
curl -fsSL https://raw.githubusercontent.com/<org>/<repo>/main/scripts/install.sh | bash
```

`install.sh` 行为约束：

- 不接受参数，固定行为：
  1. 自动检测目标平台（按 `uname -s` / `uname -m`），解析 GitHub Releases 最新 tag 为版本
  2. 拼接下载 URL，下载 zip 到系统临时目录（`$TMPDIR`，回退 `/tmp`）
  3. 解压到 `$HOME/.feng/`
  4. 自动将 `$HOME/.feng/bin` 加入 `PATH`（自动写入 `$SHELL` 对应启动脚本；仅在需要管理员权限时提示用户授权）
- 失败时必须清理半成品目录，不留残文件。

## 8 实施 TODO：Feng 编译器与工具发行包

本大阶段只交付三个 native host 上的 Feng 编译器、五目标 runtime、调试工具、四份 Linux GNU / musl sysroot 与安装包，不实现 Feng 用户程序的跨平台编译、标准库多平台制品或 `.fb` 多平台组包；这些能力统一由 §9 承载。Feng 可执行文件自身不要求交叉构建：三个 native CI 任务分别生成当前 host 平台的 `feng`，runtime 按 §4 的五个完整目标平台生成后由汇聚任务放入每一份发行包。

§8 与 §9 的所有子阶段都必须严格遵守统一门禁：

1. 完成本子阶段全部任务、专项验收和该子阶段明确要求的全量回归。除子阶段另有明确规定外，涉及 Feng 编译器、runtime 或 host 行为的子阶段必须在 `macos-arm64`、`linux-x64-gnu`、`linux-arm64-gnu` 三个 host 上通过全量 `make test`；`make test` 是仓库全量回归入口，已包含单元测试、UBSan、smoke、CLI、std、fcts 与性能约束检查。§8.1 只改变维护脚本和预生成工具链产物，不改变 Feng 编译器、runtime、Makefile 或测试，因此只要求当前 `macos-arm64` 开发 host 的全量 `make test`；Linux host 只执行该节列出的工具链与 sysroot 专项验收。
2. 提交本子阶段全部变更和验收结果供人工 Review；全量回归通过不代表可以自动进入下一子阶段。
3. 只有人工 Review 通过且收到“开始下一阶段”的明确人工指令后，才能实施下一子阶段。§8 全部通过人工 Review 前不得开始 §9。

### 8.1 可独立验证的精简工具链

本阶段只交付精简脚本和工具链产物，不改变 Feng CLI 的工具选择行为。

任务：

- [x] `scripts/fetch_llvm.sh`：已能下载并解压 macOS ARM64、Linux x64、Linux ARM64 的 LLVM 官方预编译包到 `local/llvm/`；Linux 私有运行库来源包的下载、版本固定与校验仍由下列未完成项处理。
- [x] `scripts/trim_llvm.sh`：已从单个 host LLVM root 原子精简 `clang`、`lld` / `ld.lld`、`lldb`、`lldb-dap` 及官方包内可提取的运行依赖；官方包外的 Linux host 私有动态依赖闭包仍由下列未完成项处理。
- [x] 扩展 `scripts/trim_llvm.sh`，从同一官方包保留 `llvm-ar` 与 `llvm-ranlib`，供 §9 的跨目标静态归档使用；不得引入 host `ar` 处理其他目标对象的隐式依赖。
- [x] 按完整 host 平台标识调整 LLVM 维护脚本与现有产物目录，最终产出 `toolchain/llvm/macos-arm64/`、`toolchain/llvm/linux-x64-gnu/`、`toolchain/llvm/linux-arm64-gnu/`；不得仅重命名未校验的二进制。
- [x] 扩展 `scripts/fetch_llvm.sh`，按 §5.2 为 Linux x64 / ARM64 下载固定 AlmaLinux 8.10 BaseOS / AppStream RPM、Ubuntu 22.04 Jammy security ncurses DEB 与 Ubuntu 22.04 updates `libstdc++6` DEB，逐项固定仓库位置、文件名、版本和 SHA-256，并缓存到 `local/llvm/`；不得使用会随仓库更新漂移的未固定 URL。
- [x] 扩展 `scripts/trim_llvm.sh`，从 Linux 固定来源包中只提取 LLVM 实际需要的私有共享库与 soname 链，设置相对 RPATH / RUNPATH，递归验证最终 `DT_NEEDED` 闭包和 ncurses 版本化符号，并原子写入对应 `toolchain/llvm/<host-platform>/lib/`；系统不得再承担非 glibc LLVM 运行库。
- [x] Linux LLVM 产物保留 `libpython3.11.so.1.0` 与仅供 LLDB 初始化使用的同版本 `encodings`，通过工具链内部启动器设置私有相对 `PYTHONHOME`；不得包含 Python 可执行文件、其余标准库、LLDB Python bindings、第三方模块或通用脚本能力，Feng 永不使用或支持 Python 脚本。
- [x] `scripts/fetch_musl.sh`：从 musl.cc 下载并解压 x64 与 arm64 配套预构建包。
- [x] `scripts/trim_musl.sh`：已验证两种架构 musl、CRT、libgcc 与相对目录关系完整，并排除 GCC / binutils host 可执行工具。
- [x] 将 musl 维护脚本与现有产物迁移为 `toolchain/sysroot/linux-x64-musl/`、`toolchain/sysroot/linux-arm64-musl/`，保持既有来源、许可和链路验收。
- [x] 新增 `scripts/fetch_gnu_sysroot.sh`，按 §5.3 从 Debian 官方不可变 archive / snapshot 地址真实下载两个架构的固定 Bullseye cross packages 到 `local/sysroot/gnu/`，逐项校验文件名、版本与 SHA-256；不得调用 host 包管理器选择当前版本。
- [x] 新增 `scripts/trim_gnu_sysroot.sh`，原子产出 `toolchain/sysroot/linux-x64-gnu/`、`toolchain/sysroot/linux-arm64-gnu/`；保留目标头文件、glibc、动态加载器、CRT、linker scripts、compiler runtime 与 Clang 所需目录关系，不包含 GCC / binutils / `ld` 或其他 host 可执行工具。
- [x] 两份 GNU sysroot 固定 glibc 2.31 目标 ABI，并分别记录全部二进制包与源码包的地址、版本、SHA-256、裁剪清单、许可证与源码提供方式；构造过程不得复制维护机的 `/usr`。

独立交付与回归门：

- [x] `macos-arm64` LLVM 产物在对应 host 上通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap` 启动验收，并校验二进制架构与动态依赖。
- [ ] `linux-x64-gnu` LLVM 产物在对应 host 上通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib` 启动验收，并通过真实 `lldb` / `lldb-dap` 基础调试会话；同时校验 ELF 架构、通用 x86-64 CPU 基线、最高 `GLIBC_*` / `GLIBCXX_*` 版本、相对 RPATH / RUNPATH 与完整动态依赖闭包。
- [x] `linux-arm64-gnu` LLVM 产物在对应 host 上通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib` 启动验收，并通过真实 `lldb` / `lldb-dap` 基础调试会话；同时校验 ELF 架构、通用 AArch64 CPU 基线、最高 `GLIBC_*` / `GLIBCXX_*` 版本、相对 RPATH / RUNPATH 与完整动态依赖闭包。
- [ ] LLVM host 的 glibc 下限不高于 2.34；在 Ubuntu 22.04 / 24.04 / 26.04、Debian 12 / 13 与 AlmaLinux 9 系列干净环境验证对应 x64 / ARM64 发行物无需安装额外包即可运行。纯 musl Alpine 不作为 LLVM host 验收环境。
- [x] 使用精简 Clang / LLD 与两份 sysroot 直接链接最小 C ELF，验证 x64 / arm64 的 CRT、libgcc 和 musl 链路完整，不依赖 Feng CLI。
- [x] 使用精简 Clang / LLD 与两份 GNU sysroot 分别在 native、Linux 跨架构和 macOS host 路径编译并链接最小 C ELF，验证 glibc 2.31 目标 ABI、动态加载器、CRT、linker scripts、compiler runtime 与 LLD 链路完整，不依赖 Feng CLI。
- [x] 在对应 x64 / ARM64 GNU/glibc 目标环境运行两份最小 ELF，并检查产物的 ELF 架构、解释器路径与最高 `GLIBC_*` 要求；GNU sysroot 目录同时通过无 host ELF 可执行工具、无断链符号链接和许可证 / 来源 manifest 完整性检查。
- [x] `macos-arm64` host 的全量 `make test` 在 Codex 沙箱外通过。

### 8.2 统一相对布局与 CLI 路径基础设施

本阶段只收敛发行包与源码开发的路径基础设施，不切换现有 driver 的编译器选择策略，也不改变 Feng 自身现有 `build/` 产物层级。

任务：

- [ ] Makefile 在现有 `build/toolchain/` 中创建 `llvm -> ../../toolchain/llvm/<host-platform>` 与 `sysroot -> ../../toolchain/sysroot`；`make clean` 随 `build/` 清理软链接。
- [ ] 将 Feng 可执行文件绝对路径解析与相对路径组装收敛到 `src/cli/common.*`，runtime、Clang 与 `lldb-dap` 共用，不增加工具链环境变量或重复实现。
- [ ] 为可执行文件定位、相对目录组装、软链接布局与缺失路径诊断补充独立回归。

独立交付与回归门：

- [ ] `build/bin/feng` 可通过 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 观察到与发行包一致的布局，同时现有编译、runtime 与 DAP 行为不变。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.3 三 host 编译器与五目标 runtime 发行构件

本阶段只在三个对应 native host 构建 Feng 可执行文件；runtime / unwind 按五个完整目标平台构建。不得把“Feng 可执行文件 native 构建”与“随发行包提供多目标 runtime”混为同一个 Makefile 交叉构建体系。

任务：

- [ ] `scripts/build_libunwind.sh` 按 `macos-arm64` 与四个 Linux GNU / musl 完整平台生成维护性中间产物；每份必须使用对应 SDK / sysroot，并校验对象格式、CPU 架构与 libc ABI。
- [ ] runtime 维护入口分别生成 `lib/<platform>/libfeng_runtime.a`，只合并同一完整平台的 unwind；不得把 libc 链入 runtime 归档，最终 libc 仍由 `target=bin` 的目标 sysroot 在链接阶段提供。
- [ ] 三个 CI 原生构件任务分别上传当前 host 的 `feng`，并上传其负责生成的目标 runtime：macOS 一份、Linux x64 GNU / musl 两份、Linux ARM64 GNU / musl 两份；不得由改名伪造其他目标。
- [ ] 为 native 构件的平台标识、格式、CPU 架构、公共头文件摘要和缓存污染拒绝补充回归。

独立交付与回归门：

- [ ] 三个 host 上分别验证 native `feng --version`，并验证五份 runtime / unwind 的对象格式、CPU 架构与完整平台，确认构件可被后续汇聚任务唯一识别。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.4 macOS / Linux DAP

本阶段只交付发行工具中的调试后端定位、启动与诊断，不混入用户程序交叉编译或发布脚本变更。

任务：

- [ ] `feng dap` 在 macOS 与 Linux 按 [feng-cli.md](../docs/feng-cli.md) 规定共用 §8.2 的路径能力，依次定位并启动 bundled、`PATH` 与 macOS `xcrun` 提供的 `lldb-dap`。
- [ ] Linux bundled `libpython3.11.so.1.0`、私有 `encodings` 或其他 §5.2 私有 LLDB 依赖缺失时，按发行包损坏处理，保留真实错误并给出可操作诊断，不得提示用户安装系统 Python 或其他系统包；Feng 永不使用或支持 Python 脚本。
- [ ] 补充 macOS / Linux 后端定位、启动、缺失依赖和 DAP 基础会话回归。

独立交付与回归门：

- [ ] 三个 host 分别完成 `feng dap` 真实后端启动与基础调试会话；Linux 验收环境只提供 §5.2 规定的 host 基线，不预装 Feng 私有 LLVM 运行库。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.5 分发、CI 汇聚与安装

本阶段在 §8.1—§8.4 的可验收产物之上组装分发包，不在发布流程中临时下载、精简或修补工具链，也不交叉构建 Feng 自身。

任务：

- [ ] `.github/workflows/release.yml` 由 tag / `workflow_dispatch` 触发三个 native 构件任务；每个任务执行 §8.3 的 native 构建、构件校验和全量回归后上传 `release-component-<host-platform>`。
- [ ] 独立汇聚任务等待三份原生构件，校验五份 runtime 平台完整且公共头文件一致，再调用 `scripts/release.sh` 的纯组装入口。
- [ ] `scripts/release.sh` 不调用 Feng 可执行文件的跨平台构建入口；它针对每份 zip 选择对应 host 的 `feng` 与 LLVM，并放入五份 runtime、四份 Linux GNU / musl sysroot、公共头文件和 `VERSION`。
- [ ] 三份 `feng-<version>-<platform>.zip` 都包含 `macos-arm64`、`linux-x64-gnu`、`linux-x64-musl`、`linux-arm64-gnu`、`linux-arm64-musl` runtime 与四份 Linux sysroot，不得通过改名复用其他平台 runtime。
- [ ] `scripts/install.sh` 按 [feng-os-arch.md](../docs/feng-os-arch.md) 归一化当前 host，下载对应 zip，原子解压并配置 `PATH`，失败时不留半成品。

独立交付与回归门：

- [ ] 在三个干净 host 上分别从 zip 安装，验证 `feng --version` / `VERSION`、native `build` / `run` / `lsp` / `dap` 以及 bundled Clang / LLD / LLVM ar / LLDB。
- [ ] 校验三份 zip 的五目标 runtime / 四目标 sysroot 集合、host LLVM 架构与 ABI、相对定位，验证 toolchain 缺失时的诊断和安装失败无残留。
- [ ] 三个 host 的全量 `make test` 通过；完成人工 Review 并收到明确指令后，才能开始 §9。

## 9 实施 TODO：Feng 程序交叉编译与标准库

本大阶段以 §8 已发布的三个 native Feng 工具包为基础，交付 Feng 用户程序的 native / Linux 交叉编译、GNU / musl 完整目标矩阵、SDK-free macOS 静态库分片、标准库分平台原生依赖以及单一跨平台 `.fb`。平台标识以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准，CLI、构建编排和包格式分别以 [feng-cli.md](../docs/feng-cli.md)、[feng-build.md](../docs/feng-build.md) 和 [feng-package.md](../docs/feng-package.md) 为主规范。

最终能力矩阵：

- `macos-arm64` host：支持 native 编译 / 运行 macOS 程序，交叉编译四个 Linux GNU / musl 完整平台程序，并为五个平台生成 `target=lib` 静态库。
- `linux-x64-gnu` host：支持 native 编译 / 运行 `linux-x64-gnu` 程序，生成同架构 musl 目标并交叉编译两个 ARM64 GNU / musl 目标；为五个平台生成 `target=lib` 静态库。
- `linux-arm64-gnu` host：支持 native 编译 / 运行 `linux-arm64-gnu` 程序，生成同架构 musl 目标并交叉编译两个 x64 GNU / musl 目标；为五个平台生成 `target=lib` 静态库。
- Linux host 不使用 Feng 提供的 Apple SDK，也不承诺交叉链接 macOS 可执行程序；macOS `target=lib` 默认只生成 Mach-O 对象与静态归档，确需 SDK 头文件时由用户自行通过通用 `--sysroot=<path>` 显式提供。

### 9.1 标准库第三方 C 依赖的五目标预构建

本阶段只交付 PCRE2、libsodium、libunistring 与 libuv 的五目标预构件。这四个 `libfeng_std_*` 是 std 的第三方 C 依赖，与 `libfeng_runtime`、`libfeng_unwind` 及 Feng 自身构建链无关。

任务：

- [ ] `scripts/build_pcre2.sh`、`scripts/build_libsodium.sh`、`scripts/build_libunistring.sh`、`scripts/build_libuv.sh` 继续作为独立维护脚本，输出到 `std/extlib/<platform>/`。
- [ ] 五个平台分别生成 `libfeng_std_pcre2.a`、`libfeng_std_sodium.a`、`libfeng_std_unistring.a`、`libfeng_std_uv.a`；macOS 构件只在合法 macOS 构建环境使用 Apple SDK，Linux GNU / musl 构件必须使用 §8 对应 bundled sysroot。
- [ ] 维护流程汇聚五目标 std extlib；不得将其他平台、其他架构或其他 libc ABI 静态库改名复用，每份静态库必须校验成员对象格式、CPU 架构与完整目标平台。
- [ ] 补充原生脚本的 host 识别、输出隔离、缓存污染拒绝和平台不匹配诊断回归，不改变 runtime 或 Feng 自身构建行为。

独立交付与回归门：

- [ ] 在三个 native host 上对其可运行平台完成标准库专项回归，并对五目标静态库完成格式与链接验收；记录第三方来源、版本和许可证。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.2 三 host native driver

本阶段交付三个 host 上 Feng CLI 编译用户程序的 native 能力；非 native Linux 目标在 §9.3 前必须明确报告目标不可用。

任务：

- [ ] 将 `feng.fm` 的完整平台集合字段由 `arch` 改为 `platform`，并完整实现 [feng-cli.md](../docs/feng-cli.md)“项目平台选择统一规则”、Clang 查找顺序和 native target triple 转换。
- [ ] macOS native 最终链接使用 `xcrun` 获取 SDK 并传入 `-isysroot`；Linux native GNU 与 Linux GNU 交叉编译统一使用对应 bundled glibc sysroot，不读取 host glibc 开发文件。
- [ ] native `target=bin` 只定位并链接 `lib/<platform>/libfeng_runtime.a`，不得使用其他平台或其他 libc ABI runtime。
- [ ] 补充完整平台诊断、native Clang / SDK / sysroot / runtime 定位和三 host native 编译回归。

独立交付与回归门：

- [ ] 三个 host 分别使用 §8 发行包完成 native `feng build` / `run` / `lsp` 验收，并验证 bundled toolchain、显式 `CC` 覆盖和回退路径。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.3 Linux GNU / musl 全矩阵编译

本阶段只交付 Feng 用户程序从三个支持 host 到四个 Linux GNU / musl 目标的 native / 交叉编译，不交叉构建 Feng 可执行文件自身，也不在用户程序构建期间重建 runtime、unwind 或 std 第三方 C 库。

任务：

- [ ] 任一 Linux 完整平台在 native 与交叉编译时都传入对应 `--target`、`--sysroot`、`--gcc-toolchain` 与 `-fuse-ld=lld`；GNU / musl 只由完整平台决定，显式 `--sysroot` 只覆盖路径，不得改变 ABI。
- [ ] 交叉目标只消费 §8 发行包中已经由对应 native CI 生成的 `lib/<platform>/libfeng_runtime.a`，以及 §9.1 已生成的目标 std extlib；不得在用户程序构建期间重建或改名这些基础制品。
- [ ] 不同目标的 `gen/`、`mod/`、`assets/`、对象、IR、库和可执行产物完全隔离，目标选择不得依赖编译 Feng 自身时的 host 宏或 `sizeof`。
- [ ] 补充 sysroot、compiler runtime、目标 Feng runtime、目标 std extlib 与 LLD 缺失诊断，以及 `target=bin` / `target=lib` 交叉构建回归。

独立交付与回归门：

- [ ] 分别验证 `macos-arm64` 到四个 Linux 平台、`linux-x64-gnu` 到两个 ARM64 平台、`linux-arm64-gnu` 到两个 x64 平台，以及两个 Linux host 的同架构 GNU / musl 目标；检查目标格式、CPU 架构、ELF interpreter、静态 / 动态链接属性与依赖。
- [ ] 每条路径都必须消费对应完整平台 runtime 与 std extlib；GNU 目标在匹配 glibc 基线环境运行，静态 musl 目标在对应架构 Alpine 实际运行。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.4 SDK-free macOS `target=lib`

本阶段交付任意支持 host 生成 `macos-arm64` Mach-O 对象和静态归档的能力，不在 Linux host 上执行 macOS 最终链接，也不由 Feng 提供 Apple SDK。

任务：

- [ ] 将生成 C 与 `feng_runtime.h` 在 `target=lib` 编译阶段需要的 C 类型、函数声明和 unwind 声明收敛为 Feng / LLVM 合法分发的自包含编译头闭包；默认路径不得读取目标 macOS SDK 的 `math.h`、`stdlib.h`、`string.h` 等系统头。
- [ ] `target=lib --platform=macos-arm64` 使用 bundled Clang 的明确 Darwin target triple，只执行 `-c`，再使用 bundled `llvm-ar` / `llvm-ranlib` 生成 Mach-O 静态库；不得调用 host `ar` 或进入最终链接。
- [ ] 核心直编与项目级单平台 `feng build` 支持通用 `--sysroot=<path>`。该路径只作用于本次唯一目标平台；项目命令的目标集合按 [feng-cli.md](../docs/feng-cli.md)“项目平台选择统一规则”确定，集合包含多个平台时不得同时使用单一 `--sysroot`，需要不同 sysroot 的平台必须分多次带单一 `--platform` 的 `feng build`。
- [ ] macOS 目标显式 `--sysroot` 转换为 Clang `-isysroot`；Feng 只校验路径与构建所需内容，不下载、复制或判断第三方 SDK 的授权来源，用户负责其输入的许可合规。
- [ ] `target=lib` 不链接 `libfeng_runtime.a` 或 std extlib；macOS runtime 与四个 std extlib 继续使用 §8 / §9.1 在 macOS CI 原生预构建的制品。

独立交付与回归门：

- [ ] 在 `linux-x64-gnu` 与 `linux-arm64-gnu` host 不提供 Apple SDK，分别生成合法的 `macos-arm64` Mach-O 静态库，并校验 archive index、成员格式、CPU 架构和符号命名。
- [ ] 将两种 Linux host 生成的静态库带到 macOS，与 native runtime / std extlib 及官方 macOS SDK完成最终链接和运行回归。
- [ ] 使用用户显式 `--sysroot` 的正向、缺失路径、目标不匹配与多平台歧义诊断回归通过。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.5 多平台 `.fb` 与 std 组包

本阶段在 §9.1—§9.4 之上交付项目级分平台构建和单一跨平台 `.fb`；std 作为每个完整平台均包含四个第三方 extlib 的完整验收包。

任务：

- [ ] 核心编译器直编与项目级 `feng init` / `build` / `run` / `pack` 完整实现 [feng-cli.md](../docs/feng-cli.md)“项目平台选择统一规则”；直编继续使用调用方给定的精确 `--out`。
- [ ] `feng build` 将 `gen/`、`mod/`、`assets/`、`bin/`、`lib/`、`obj/`、`ir/` 与 `extlib/` 全部隔离到 `<项目输出根>/<platform>/`；相同目标递归传递给本地 `target=lib` 依赖。
- [ ] `feng pack` 按 [feng-cli.md](../docs/feng-cli.md)“项目平台选择统一规则”复用项目构建流程，固定执行 release 构建且不接受 `--release` / `--sysroot`；全部选定平台构建成功后，校验各平台 `mod/` 的模块集合与公开语义事实等价、普通 `assets/` 内容一致并分别提取一套。
- [ ] `feng pack` 从每个 `<项目输出根>/<platform>/lib/` 与 `extlib/` 提取分平台制品，写入包内 `lib/<platform>/` 与 `extlib/<platform>/`，将实际平台集合写入分发包 `feng.fm.platform`；任一请求平台缺失或校验失败时不得生成部分 `.fb`。
- [ ] std 构建对五个目标分别生成 `libstd.a`，并从 §9.1 的预构件中选择对应完整平台的四个 `libfeng_std_*` 写入 `extlib/<platform>/`；Linux 构建 macOS std 分片时只编译 Feng 生成 C，不重建或链接 macOS extlib。
- [ ] 补充 [feng-cli.md](../docs/feng-cli.md)“项目平台选择统一规则”表格全部分支，以及分平台产物隔离、递归本地依赖、远程包目标制品缺失、公开符号表不一致、`platform` 精确匹配和原子组包回归。

独立交付与回归门：

- [ ] 在单个 Linux host 不使用 Apple SDK，由 std 项目执行 `feng pack`，通过其 release 构建与组包流程生成一个五平台 `std.fb`；目标选择按 [feng-cli.md](../docs/feng-cli.md)“项目平台选择统一规则”执行。
- [ ] 在三个支持 host 上分别执行 `feng pack`，验证均生成结构一致的五平台 `.fb`。
- [ ] 五个平台分别将该 `.fb` 作为依赖完成编译与最终链接，并在可运行环境完成运行验收；macOS 最终链接和运行只在合法 macOS 环境执行。
- [ ] 三个 host 的全量 `make test` 通过。

## 10 Linux GNU / musl 目标链接已定方案

本节只记录 Linux 目标链接所需的分发组成；完整平台到 Clang 参数的转换以 [feng-build.md](../docs/feng-build.md) 为主规范。

1. **linker**：各 host 分发包使用自身 `toolchain/llvm/bin/lld`，并保留 `bin/ld.lld -> lld`。该可执行文件来自与 `clang` 相同的 LLVM 官方 host 包。driver 对全部 Linux GNU / musl 平台传入 `-fuse-ld=lld`，由 Clang 基于自身安装目录定位 `bin/ld.lld`，不传 `--ld-path`，也不使用 sysroot 来源包中的 linker。
2. **GNU 目标 C 环境**：`toolchain/sysroot/linux-*-gnu/` 按 §5.3 从固定 Debian Bullseye cross packages 裁剪，携带与目标架构及 glibc 2.31 目标 ABI 匹配的头文件、glibc、动态加载器、CRT、linker scripts 与 compiler runtime。Linux native 与交叉编译都使用该 bundled sysroot，不消费 host glibc 开发文件。
3. **musl 目标 C 环境**：`toolchain/sysroot/linux-*-musl/` 使用与目标架构匹配的 musl.cc 配套内容，保留 musl 头文件、库、动态加载器、CRT 及目标 compiler runtime。`target=bin` 默认执行静态链接；`target=lib` 只编译对象并归档。
4. **Feng runtime**：`target=bin` 必须使用 `lib/<platform>/libfeng_runtime.a`，GNU / musl 不得互换。libc 不合并进 runtime 归档，由最终目标链接从对应 sysroot 解析。
5. **Feng 包与 std 原生库**：正式 `.a` 与 std 第三方 C 依赖同样按完整平台区分，分别位于 `.fb/lib/<platform>/` 与 `.fb/extlib/<platform>/`。
6. **可用性边界**：host `clang` / `lld`、目标 sysroot / compiler runtime、目标 Feng runtime 及实际引用的目标 extlib 必须同时存在并通过链接验收，才能宣称该完整 Linux 平台可用。
