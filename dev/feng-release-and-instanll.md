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

`scripts/fetch_gnu_sysroot.sh` 必须从 Debian 官方 archive / snapshot 的不可变地址下载固定 `.deb` 到 `local/sysroot-gnu/`，逐项校验文件名、版本与 SHA-256 后再解包；不得调用 host 包管理器解析当前最新版本。`scripts/trim_gnu_sysroot.sh` 从缓存原子生成 `toolchain/sysroot/linux-x64-gnu/` 与 `toolchain/sysroot/linux-arm64-gnu/`，仅保留目标公开 C / Linux userspace 头文件、glibc 动态与链接所需文件、动态加载器、CRT、linker scripts、`libgcc_s`、`libgcc.a`、`libgcc_eh.a`、GCC CRT 及 Clang GCC installation detector 所需目录和相对链接。GCC、binutils、`ld`、包维护脚本以及其他 host 可执行文件一律排除。

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

仅因外部原生机器或发行版镜像暂不可用而未完成的兼容性矩阵验收，在人工确认其不阻塞下一子阶段实现后，可与不依赖该验收结果的后续子阶段并行；对应 TODO 必须保持未完成，并在 §8.5 分发、CI 汇聚与安装实施前全部通过。该例外不适用于脚本、产物、代码、测试或当前可用环境中的验收缺口，也不代表前一子阶段已经完成。

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
- [x] `scripts/fetch_musl_sysroot.sh`：从 musl.cc 下载并解压 x64 与 arm64 配套预构建包。
- [x] `scripts/trim_musl_sysroot.sh`：已验证两种架构 musl、CRT、libgcc 与相对目录关系完整，并排除 GCC / binutils host 可执行工具。
- [x] 将 musl 维护脚本与现有产物迁移为 `toolchain/sysroot/linux-x64-musl/`、`toolchain/sysroot/linux-arm64-musl/`，保持既有来源、许可和链路验收。
- [x] 新增 `scripts/fetch_gnu_sysroot.sh`，按 §5.3 从 Debian 官方不可变 archive / snapshot 地址真实下载两个架构的固定 Bullseye cross packages 到 `local/sysroot-gnu/`，逐项校验文件名、版本与 SHA-256；不得调用 host 包管理器选择当前版本。
- [x] 新增 `scripts/trim_gnu_sysroot.sh`，原子产出 `toolchain/sysroot/linux-x64-gnu/`、`toolchain/sysroot/linux-arm64-gnu/`；保留目标头文件、glibc、动态加载器、CRT、linker scripts、compiler runtime 与 Clang 所需目录关系，不包含 GCC / binutils / `ld` 或其他 host 可执行工具。
- [x] 两份 GNU sysroot 固定 glibc 2.31 目标 ABI，并分别记录全部二进制包与源码包的地址、版本、SHA-256、裁剪清单、许可证与源码提供方式；构造过程不得复制维护机的 `/usr`。

独立交付与回归门：

- [x] `macos-arm64` LLVM 产物通过工具启动、二进制架构和动态依赖验收；`lldb` 与 `lldb-dap` 均通过真实断点、调用栈、局部变量和继续执行调试会话。
- [ ] `linux-x64-gnu` LLVM 产物已在 Apple Container 的 Ubuntu 26.04 x64 环境通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb` 与 `lldb-dap` 启动验收，并已校验 ELF 架构、通用 x86-64 CPU 基线、最高 `GLIBC_*` / `GLIBCXX_*` 版本、相对 RPATH / RUNPATH 与完整动态依赖闭包；但 Apple Container 的 x64 仿真环境设置 LLDB 软件断点时返回 `SIGILL`，硬件断点也不可用，因此仍须在原生 x64 Linux host 完成真实 `lldb` / `lldb-dap` 基础调试会话后才能标记完成。
- [x] `linux-arm64-gnu` LLVM 产物已在 Apple Container 的 Ubuntu 26.04 ARM64 干净环境通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb` 与 `lldb-dap` 启动验收，并通过真实 `lldb` / `lldb-dap` 断点、调用栈、局部变量、继续与正常退出基础调试会话；同时已校验 ELF 架构、通用 AArch64 CPU 基线、最高 `GLIBC_*` / `GLIBCXX_*` 版本、相对 RPATH / RUNPATH 与完整动态依赖闭包。
- [ ] LLVM host 的 glibc 下限已校验不高于 2.34；对应 x64 / ARM64 发行物已在 Apple Container 的 Ubuntu 26.04 干净环境验证无需安装额外包即可启动，其中 ARM64 真实 LLDB / DAP 调试会话已通过。仍须补齐 Ubuntu 22.04 / 24.04、Debian 12 / 13 与 AlmaLinux 9 系列的 x64 / ARM64 干净环境验收；纯 musl Alpine 不作为 LLVM host 验收环境。
- [x] 使用精简 Clang / LLD 与两份 sysroot 直接链接最小 C ELF，验证 x64 / arm64 的 CRT、libgcc 和 musl 链路完整，不依赖 Feng CLI。
- [x] 使用精简 Clang / LLD 与两份 GNU sysroot 分别在 native、Linux 跨架构和 macOS host 路径编译并链接最小 C ELF，验证 glibc 2.31 目标 ABI、动态加载器、CRT、linker scripts、compiler runtime 与 LLD 链路完整，不依赖 Feng CLI。
- [x] 在对应 x64 / ARM64 GNU/glibc 目标环境运行两份最小 ELF，并检查产物的 ELF 架构、解释器路径与最高 `GLIBC_*` 要求；GNU sysroot 目录同时通过无 host ELF 可执行工具、无断链符号链接和许可证 / 来源 manifest 完整性检查。
- [x] `macos-arm64` host 的全量 `make test` 在 Codex 沙箱外通过。

### 8.2 统一相对布局、CLI 路径基础设施与 host LLVM 定位

本阶段收敛发行包与源码开发的路径基础设施，并切换 driver 默认使用当前 host 的 bundled LLVM；同时完成 macOS native bundled Clang 必需的系统 SDK 定位。不引入显式目标平台、target triple、用户 `--sysroot` 或其他交叉编译逻辑，也不改变 Feng 自身现有 `build/` 产物层级。LLVM 可执行文件始终属于 host 工具，native 与后续交叉编译共用同一套定位结果，目标平台只影响后续传给 LLVM 的编译与链接参数。

Feng 编译器自身固定使用 `clang` 构建，不读取或接受其他 `CC` 值，也不支持 GCC。Linux host 在严格 C11 模式下统一启用 glibc 的 GNU feature namespace，以公开源码和测试实际使用的 GNU、XSI 与 POSIX.1-2008 接口。Feng 语义分析器直接调用 `fmod` 完成编译期浮点常量计算，因此仅为包含该语义分析器对象的 Linux host 工具与测试可执行文件链接 `libm`；这属于 Feng 编译器自身的构建依赖，不得用于替代 [feng-build.md](../docs/feng-build.md#25-收集链接信息) 规定的 Feng 用户程序 external 链接信息收集机制。

任务：

- [x] Makefile 将 `build/toolchain/llvm -> ../../toolchain/llvm/<host-platform>` 与 `build/toolchain/sysroot -> ../../toolchain/sysroot` 作为实际构建目标，仅在缺失时创建；不持续校验已存在链接的指向，链接被手工破坏时通过 `make clean` 后重新执行 `make all` 恢复。
- [x] 将 Feng 可执行文件绝对路径解析、安装根解析、相对路径组装、`PATH` 可执行文件查找与缺失路径诊断收敛到 `src/cli/common.*`；runtime 已迁移到该公共能力，host LLVM 构建工具在本阶段继续复用，`lldb-dap` 在 §8.4 切换定位策略时复用，不增加改变安装根或整套工具链根目录的环境变量，也不重复实现。
- [x] 为可执行文件定位、相对目录组装、软链接布局与缺失路径诊断补充独立回归。
- [x] driver 按 [feng-build.md](../docs/feng-build.md) 规定的统一顺序选择 host LLVM 构建工具：`FENG_CC` / `FENG_AR` / `FENG_RANLIB`、bundled `clang` / `llvm-ar` / `llvm-ranlib`、传统 `CC` / `AR` / `RANLIB`、系统 `cc` / `ar` / `ranlib`；native 编译立即使用该结果，后续交叉编译也复用同一 host 工具定位，不得按目标平台选择另一份 LLVM。
- [x] 补充 driver 工具选择回归，分别验证 Feng 专用变量显式覆盖、三种 bundled 工具默认命中、bundled 缺失后的传统环境变量候选、环境变量未设置时的系统 `PATH` 兜底，以及 bundled 损坏、环境变量无效、候选均不可用时不静默降级并给出明确诊断；不得在本阶段加入 `--target`、`--sysroot` 或目标 runtime 选择测试。
- [x] macOS native 编译通过 `xcrun --sdk macosx --show-sdk-path` 定位系统 SDK，并向本阶段选中的 C 编译器传入唯一的 `-isysroot`；`xcrun`、developer directory 或 SDK 不可用时给出明确诊断，不得回退到未指定 SDK 的编译。该行为只启用当前 native 编译，不增加显式 `--sysroot` 或跨目标选择。
- [x] Makefile 固定使用 `clang` 构建 Feng 编译器自身，不增加运行时编译器类型检查；Linux host 全局启用 GNU / XSI / POSIX.1-2008 声明，并仅为包含语义分析器对象的 host 工具与测试链接其直接使用的 `libm`，不得依赖 GCC 或 macOS `libSystem` 的隐式行为。

独立交付与回归门：

- [x] `build/bin/feng` 可通过 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 观察到与发行包一致的布局；native `.ff` 编译实际启动 bundled `clang`，macOS 同时使用 `xcrun` 返回的系统 SDK，静态归档实际启动 bundled `llvm-ar`，归档流程需要独立索引时实际启动 bundled `llvm-ranlib`，runtime 与 DAP 的现有行为保持不变。
- [x] `macos-arm64` 已在 Codex 沙箱外通过全量 `make test`。
- [ ] `linux-x64-gnu` 与 `linux-arm64-gnu` 已分别在 Apple Container 的 Ubuntu 26.04 中通过新增路径能力、软链接布局与 bundled LLVM / sysroot 专项回归，仍须在两个 Linux host 上通过全量 `make test`。

### 8.3 libunwind、Feng 编译器与 runtime 构建

本阶段只更新 `scripts/build_libunwind.sh` 和 Makefile，不修改 Feng CLI、CI 或发行脚本。

平台标识以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准，构件路径固定如下：

| 构建环境 | `libfeng_unwind.a` 维护产物 | `libfeng_runtime.a` 构建产物 |
|----------|-----------------------------|-------------------------------|
| `macos-arm64` | `extlib/macos-arm64/libfeng_unwind.a` | `build/lib/macos-arm64/libfeng_runtime.a` |
| `linux-x64-gnu` | `extlib/linux-x64-gnu/libfeng_unwind.a`<br>`extlib/linux-x64-musl/libfeng_unwind.a` | `build/lib/linux-x64-gnu/libfeng_runtime.a`<br>`build/lib/linux-x64-musl/libfeng_runtime.a` |
| `linux-arm64-gnu` | `extlib/linux-arm64-gnu/libfeng_unwind.a`<br>`extlib/linux-arm64-musl/libfeng_unwind.a` | `build/lib/linux-arm64-gnu/libfeng_runtime.a`<br>`build/lib/linux-arm64-musl/libfeng_runtime.a` |

任务：

- [ ] 更新 `scripts/build_libunwind.sh`。该脚本只供维护者手动执行，生成上表规定的五份 unwind；每份构件使用对应 SDK 或 sysroot。
- [ ] 更新 Makefile。三个构建环境均将当前平台的 Feng 编译器输出到 `build/bin/feng`，并生成上表规定的 runtime。
- [ ] 每份 runtime 只合并 `extlib/<platform>/libfeng_unwind.a` 中 `<platform>` 完全相同的 unwind，不包含 libc；SDK / sysroot、文件格式或 CPU 架构与 `<platform>` 不匹配时立即失败。

独立交付与回归门：

- [ ] 五份 unwind 均可由维护脚本重新生成并通过校验。
- [ ] `macos-arm64`、`linux-x64-gnu`、`linux-arm64-gnu` 的 `build/bin/feng --version` 均可正常执行，上表五份 runtime 均通过平台、文件格式与 CPU 架构校验。
- [ ] `macos-arm64`、`linux-x64-gnu`、`linux-arm64-gnu` 的全量 `make test` 均通过。

### 8.4 macOS 与 Linux 调试

本阶段只更新 `feng dap`。精简 `lldb` 与 `lldb-dap` 由 §8.1 提供，不在本阶段重新剪裁。

任务：

- [ ] `feng dap` 在 macOS 与 Linux 按 [feng-cli.md](../docs/feng-cli.md) 规定定位并启动 `lldb-dap`，复用 §8.2 的公共路径能力。
- [ ] 补充后端定位、启动失败和真实 DAP 调试会话回归。

独立交付与回归门：

- [ ] 三个发行平台均通过 `feng dap` 真实调试会话。
- [ ] 三个发行平台的全量 `make test` 均通过。

### 8.5 CI、发行与安装

本阶段只编写 CI、构件汇聚、发行包生成和安装脚本。

任务：

- [ ] `.github/workflows/release.yml` 在三个发行平台分别构建 Feng 编译器及该平台负责的 runtime，完成校验和全量回归后上传构件。
- [ ] 汇聚任务下载三组构件，校验五份 runtime 和公共头文件，再调用 `scripts/release.sh`。
- [ ] `scripts/release.sh` 生成三个发行包；每个发行包包含对应平台的 Feng 编译器与 LLVM，以及全部五份 runtime、四份 Linux sysroot、公共头文件和 `VERSION`。
- [ ] `scripts/install.sh` 按 [feng-os-arch.md](../docs/feng-os-arch.md) 归一化当前 host，下载对应 zip，原子解压并配置 `PATH`，失败时不留半成品。

独立交付与回归门：

- [ ] 在三个干净发行平台安装对应发行包，验证 `feng --version`、`build`、`run`、`lsp`、`dap` 和 bundled LLVM。
- [ ] 三个发行包的目录、平台构件和相对定位均通过校验。
- [ ] 三个发行平台的全量 `make test` 均通过；完成人工 Review 并收到明确指令后，才能开始 §9。
