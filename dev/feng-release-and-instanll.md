# Feng 分发与安装方案

> 本方案收敛 Feng 工具链（编译器 + 运行时静态库 + 精简 toolchain）的分发包结构、构建发布工作流、安装方式。
> `.fb` 包格式（feng 项目间源码级闭源分发）由 [feng-package.md](../docs/feng-package.md) 单独定义，不在本文件重复。
> CLI 命令与 `--platform` 选项由 [feng-cli.md](../docs/feng-cli.md) 单独定义，平台标识值由 [feng-os-arch.md](../docs/feng-os-arch.md) 统一定义，工具链选择与 Clang target / sysroot 转换由 [feng-build.md](../docs/feng-build.md) 定义；本方案只定义分发与安装布局。

## 1 目标与范围

本方案解决一件事：让用户在目标机器上拿到一个可用的 Feng 工具链，并能在多种平台上以一致方式安装。

首版明确覆盖：

- 分发物：单一压缩包 `feng-<version>-<os>-<arch>.zip`
- 平台：`macos-arm64`、`linux-x64`、`linux-arm64`
- 安装方式：手动解压 + 在线脚本两种
- toolchain 形态：bundle 精简版（从同一 host 平台的 LLVM 官方预编译包剥离，核心保留 `clang`、`lld`、`lldb`、`lldb-dap`，不自建 LLVM）
- 运行时分发形态：仅静态库 `.a` / `.lib`
- 分发渠道：GitHub Releases

首版明确不做：

- Windows 实际打包（脚本与工作流预留扩展点，不产出二进制）
- Linux host 交叉编译 macOS 程序（Apple SDK 不可随 Feng 分发，且标准 [Xcode and Apple SDKs Agreement](https://www.apple.com/legal/sla/docs/xcode.pdf) 不允许单独使用 Apple SDK 或在非 Apple 品牌硬件上运行 Apple Software）
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
- **与现有体系一致**：复用 `Makefile` 产物路径、Feng 编译期 `extlib/<os>-<arch>/` 平台隔离约定与 `scripts/` 下既有构建脚本，不另立构建体系。

## 3 命名规范与平台矩阵

### 3.1 分发物命名

```text
feng-<version>-<os>-<arch>.zip
```

- `feng` 后紧跟 `<version>`，明确表示该版本为 Feng 自身版本；`<os>-<arch>` 作为目标平台后缀。
- `<os>` 与 `<arch>` 取值见 [feng-os-arch.md](../docs/feng-os-arch.md)，不在本文件重复定义。
- `<version>` 与 git tag 一致，形如 `0.1.0`、`0.2.0-rc.1`，不带前缀 `v`

示例：`feng-0.1.0-macos-arm64.zip`

### 3.2 平台矩阵

| 平台标识     | 首版产出 | 扩展预留 | 备注                                  |
|--------------|---------|---------|---------------------------------------|
| `macos-arm64` | 是      | -       | Feng 编译期 extlib 依赖已就绪          |
| `macos-x64`  | 否      | 是      | Intel Mac，后续按需补齐               |
| `linux-x64`  | 是      | -       | musl sysroot 内建；LLVM 官方预编译包提供 |
| `linux-arm64`| 是      | -       | musl sysroot 内建；LLVM 官方预编译包提供 |
| `windows-x64`| 否      | 是      | 静态库后缀切换为 `.lib`，可执行为 `.exe` |
| `windows-arm64`| 否    | 是      | -                                     |

矩阵在 CI 工作流与安装脚本中以表驱动形式表达，新增平台只改表项，不改主干逻辑。

## 4 压缩包目录结构

解压后顶层目录名与压缩包主名一致：`feng-<version>-<os>-<arch>/`。

```text
feng-<version>-<os>-<arch>/
├── bin/                          # 必须：Feng 可执行
│   └── feng                      # 编译器 + CLI 主入口（含 lsp / dap 子命令）
├── include/                      # 必须：runtime 公共 ABI 头文件（平台无关）
│   ├── feng_runtime.h
│   └── feng_runtime_contract.inc
├── lib/                          # 必须：运行时静态库（按目标平台分子目录）
│   └── <os>-<arch>/              # 目标平台标识，取值见 feng-os-arch.md
│       └── libfeng_runtime.a     # linux/macos；Windows 下为 feng_runtime.lib
│  
├── toolchain/                    # 必须：精简 LLVM 工具链 + 交叉编译 sysroot
│   ├── llvm/                     # 同一 LLVM 官方包精简后的统一根目录
│   │   ├── bin/
│   │   │   ├── clang          # C/LLVM 后端驱动
│   │   │   ├── lld            # LLVM linker，Linux musl 交叉链接使用
│   │   │   ├── ld.lld -> lld  # Clang / driver 调用入口
│   │   │   ├── lldb           # 命令行调试器
│   │   │   └── lldb-dap       # DAP 适配器，供 feng dap / VS Code 使用
│   │   └── lib/
│   │       ├── clang/22/      # clang 官方 resource-dir，保持与 bin/clang 的相对位置
│   │       │   ├── include/   # 编译器内置头文件（平台无关）
│   │       │   └── lib/<os>/  # 编译器运行时库（目标 OS）
│   │       └── liblldb.*       # lldb / lldb-dap 运行所必需的库
│   └── sysroot/                  # 交叉编译 sysroot（按目标平台分子目录）
│       └── <os>-<arch>/          # 目标平台标识，取值见 feng-os-arch.md
│           ├── usr/include/      # 目标平台系统头文件
│           ├── usr/lib/          # musl 库、动态加载器与 musl CRT
│           │   └── gcc/<musl-triple>/<gcc-version>/
│           │       └── crtbegin* / crtend* / libgcc*
│           ├── lib -> usr/lib    # musl.cc / Clang 目录兼容视图
│           └── <musl-triple>/    # Clang GCC toolchain 检测所需的目标视图
│               ├── include -> ../usr/include
│               └── lib -> ../usr/lib
└── VERSION                       # 必须：纯文本版本号，单行
```

- Windows 平台下，`bin/` 中可执行文件追加 `.exe` 后缀，`lib/` 中静态库后缀切换为 `.lib`。
- `lib/` 按目标平台分子目录（`lib/<os>-<arch>/`，取值见 [feng-os-arch.md](../docs/feng-os-arch.md)）。分发包至少包含其 host 平台 runtime；支持交叉编译时，可同时包含其他目标平台 runtime 子目录。
- `include/` 为 runtime 公共 ABI 头文件，平台无关（C 源码），不分平台子目录，单一一份供所有平台使用，扁平置于 `include/` 根下。`feng_runtime.h` 内部以相对路径 `#include "feng_runtime_contract.inc"`，二者位于同一目录；标准 C 头文件（`<stdint.h>` 等）与系统头文件（`<unwind.h>`）由目标平台 SDK / sysroot 提供，不重复放入 runtime include 目录。
- `toolchain/llvm/` 保持 LLVM 官方包的统一根目录布局，`clang`、`lld`、`lldb` 与 `lldb-dap` 必须来自同一 LLVM 版本和同一 host 平台包。`bin/clang` 与 `lib/clang/<version>/` 的相对位置关系由 clang 自动推导（`-print-resource-dir`），driver 无需额外指定 `-resource-dir` 或 `-isystem`；`bin/ld.lld` 必须保留为指向 `bin/lld` 的入口；`bin/lldb` / `bin/lldb-dap` 与 `lib/liblldb.*` 也保持官方包内的相对位置。`lib/clang/<version>/include/`（编译器内置头）平台无关；`lib/clang/<version>/lib/<os>/`（编译器运行时库）按目标 OS 分目录。
- `toolchain/sysroot/` 按目标平台分子目录，保持 `--sysroot` 约定的 `usr/include/` + `usr/lib/` 主视图。Linux musl sysroot 从 musl.cc 配套包精简，除 musl 头文件、库与 `crt1` / `crti` / `crtn` 外，还必须保留目标 `crtbegin*` / `crtend*`、`libgcc.a`、`libgcc_eh.a`、`libgcc_s.so*` 及 Clang GCC toolchain 检测所需的相对目录关系；不保留 musl.cc 中只能在特定宿主上运行的交叉链接器可执行文件。具体调用参数见 [feng-build.md](../docs/feng-build.md)。macOS SDK 受 Apple 许可限制不进入 Feng 分发包，macOS native 编译使用用户合法安装的系统 SDK；首版不在 Linux host 上支持 macOS 交叉编译。
- 分发物不包含任何 Feng 源码、`.o` / `.obj` 中间产物、构建缓存。
- `feng` 编译器基于自身位置查找 runtime 静态库、头文件与 toolchain：runtime 位于 `<feng 可执行文件目录>/../lib/` 与 `../include/`，Clang 和 `lldb-dap` 位于 `<feng 可执行文件目录>/../toolchain/llvm/bin/`。不引入 `FENG_HOME` / `FENG_TOOLCHAIN` 等环境变量；完整查找顺序见 [feng-build.md](../docs/feng-build.md) 与 [feng-cli.md](../docs/feng-cli.md) 的 DAP 规范。

## 5 toolchain 形态

分发包内 `toolchain/` 为精简版 LLVM 工具链与交叉编译 sysroot，与 `bin/`、`lib/`、`include/` 并列置于分发包根下。

- **从 LLVM 官方预编译包剥离，不自建 LLVM/Clang**；核心只保留 `clang`、`lld`、`lldb`、`lldb-dap` 及其运行所必需的最小依赖集，不含 `llvm-*`、`clang-format`、`clang-tidy` 等其他通用 LLVM 工具。Linux musl 交叉链接统一使用该 host 平台 LLVM 包中的 `lld`，不分发 musl.cc 中的 linker 可执行文件。
- 精简由 `scripts/fetch_llvm.sh` + `scripts/trim_llvm.sh` 完成（维护性脚本，不在发布流程）：`fetch_llvm.sh` 下载并解压 LLVM 官方预编译包到 `local/llvm/`（持久 cache，gitignored，不受 `make clean` 或测试清理 `temp/` 影响），`trim_llvm.sh` 从单个已解压 LLVM root 中同时精简 clang、lld、lldb 与 lldb-dap，原子产出到仓库 `toolchain/llvm/<os>-<arch>/`。合并为一个精简脚本，避免两个脚本共享输出根目录时相互删除产物，并保证四个工具的版本、来源和 host 平台一致。精简脚本只保留运行与目标编译所必需的文件，实际解压体积在维护验收时记录，不以删除必要运行依赖换取固定体积。
- `toolchain/sysroot/` 为交叉编译 sysroot，按目标平台分子目录。Linux 交叉编译目标基于 musl libc，由 `scripts/fetch_musl.sh` 下载 musl.cc 配套预构建包到 `temp/musl/`，再由 `scripts/trim_musl.sh` 将 musl sysroot 与目标 CRT / GCC 支持运行库一起精简到仓库 `toolchain/sysroot/<os>-<arch>/`（git lfs 管理）；不保留预构建包中的 GCC、`ld` 等 host 可执行工具。精简产物的 README 必须分别记录 musl 与 GCC runtime 的来源、版本和上游许可，不得把整个 sysroot 笼统标记为单一 musl 许可。musl 主要用于交叉编译场景，Linux 平台原生编译采用 glibc（由 host 系统提供）。macOS 目标受 Apple SDK 版权限制不可自由分发，需用户自行合法获取。
- 精简 toolchain 的版本、来源、剥离清单由独立子任务文档承载，不在本文件展开，避免方案膨胀。
- `feng` 编译器基于自身位置查找 `toolchain/`。源码开发构建以 `build/<目标平台>/` 作为完整平台构建根；Makefile 在其中创建 `toolchain/llvm -> ../../../toolchain/llvm/<目标平台>` 与 `toolchain/sysroot -> ../../../toolchain/sysroot` 两个软链接，使 `build/<目标平台>/bin/feng` 观察到的 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 都与发行包布局一致。目标 Feng 可执行文件运行时使用与自身平台匹配的 LLVM，sysroot 视图保留全部已交付目标平台子目录；`make clean` 删除整个 `build/`，软链接不作为持久产物或分发内容。

### 5.1 macOS 系统前置条件

macOS SDK 不随 Feng 分发。使用 Feng 内置 Clang 编译 macOS 目标时，系统必须已安装 Xcode 或 Xcode Command Line Tools，以提供 macOS SDK、系统链接器与 `xcrun`。用户可执行以下命令安装 Command Line Tools:

```bash
xcode-select --install
```

Feng 通过 `xcrun --sdk macosx --show-sdk-path` 获取 SDK 路径并向内置 Clang 传入 `-isysroot`。命令不可用、未选中有效 developer directory 或 SDK 不存在时,编译必须给出明确诊断；安装脚本不得静默安装 Xcode 或 Command Line Tools。该前置条件只用于 macOS 编译,不改变 `feng --version` 等无需编译的命令。

### 5.2 Linux LLDB 系统前置条件

LLVM 22.1.8 官方 Linux x64 与 arm64 包中的 `liblldb.so.22.1.8` 都直接依赖 `libpython3.11.so.1.0`,而官方包本身不携带该共享库。Feng 分发包不额外捆绑 libpython；Linux 用户使用 `lldb` 或 `feng dap` 前,必须通过所在发行版安装能够提供该 soname 的 Python 3.11 共享库包。Feng 不使用 Python 脚本,但动态加载器仍要求满足 `liblldb` 的直接依赖。

该前置条件只影响 `lldb` / `lldb-dap` 启动；`feng` 编译、`lsp` 等不加载 `liblldb` 的路径不应因此依赖 Python。若共享库缺失,`feng dap` 必须保留后端加载器的真实错误并给出可操作诊断。

## 6 构建与发布工作流

### 6.1 触发

GitHub Actions 工作流 `release.yml` 由推送形如 `v*.*.*` 的 tag 触发，并支持 `workflow_dispatch` 手动触发用于试发。

### 6.2 matrix

```yaml
strategy:
  matrix:
    include:
      - { os: macos, arch: arm64, runner: macos-14 }
      - { os: linux, arch: x64,  runner: ubuntu-latest }
      - { os: linux, arch: arm64, runner: ubuntu-24.04-arm }
      # 扩展时追加：macos-x64、windows-x64 等
```

每个 matrix 项产出一份 `feng-<version>-<os>-<arch>.zip`，互不依赖，可并行。各 host 平台的精简 LLVM 产物位于仓库 `toolchain/llvm/<os>-<arch>/`，各目标平台的 sysroot 位于仓库 `toolchain/sysroot/<os>-<arch>/`，均由 git lfs 管理。CI checkout 即有，无需构建期精简，只需从仓库目录复制。

### 6.3 单平台构建步骤

GitHub Actions 工作流在各 matrix 项中调用 `scripts/release.sh`，产出安装包到 `release/` 目录。步骤如下：

1. checkout 仓库（含 git lfs 管理的 `toolchain/llvm/<os>-<arch>/` 与 `toolchain/sysroot/<os>-<arch>/` 精简产物）
2. 安装构建依赖（macOS runner 必须已有 Xcode 或 Xcode Command Line Tools；Linux 需 `build-essential` 等，由 `release.sh` 检测并安装）
3. 执行 `scripts/build_libunwind.sh`（产出 Feng **编译期**依赖 `extlib/<os>-<arch>/libfeng_unwind.a`，不进分发物；其对象在 `make runtime` 时被合并进 `libfeng_runtime.a`）
4. 执行 `make TARGET_PLATFORM=<os>-<arch> cli runtime`（产出 `build/<os>-<arch>/bin/feng`、`build/<os>-<arch>/lib/libfeng_runtime.a`、`build/<os>-<arch>/include/feng_runtime.h` 与 `build/<os>-<arch>/include/feng_runtime_contract.inc`）
5. 组装分发目录树：`build/<os>-<arch>/bin/feng` 放入 `bin/`，同一平台构建根 `include/` 下的两个头文件放入 `include/`，`lib/libfeng_runtime.a` 放入分发包 `lib/<os>-<arch>/`，仓库 `toolchain/llvm/<os>-<arch>/` 对应 host 平台产物放入分发包 `toolchain/llvm/`，需要分发的 `toolchain/sysroot/<target-os>-<target-arch>/` 保持目标平台子目录放入分发包 `toolchain/sysroot/`，并生成 `VERSION` 文件（写入 git tag 版本号）
6. 打包 zip
7. 上传到 GitHub Release 对应 tag

toolchain 精简产物在**本地维护**：开发者用 `fetch_llvm.sh` + `trim_llvm.sh` + `fetch_musl.sh` + `trim_musl.sh` 在本地产出各平台精简结果，提交到仓库（git lfs）。CI 只需 checkout 后从仓库目录复制，不做任何精简。这样 CI 构建步骤简、快，且不依赖 musl.cc / LLVM Releases 网络可达性。

### 6.4 失败与回滚

- 任一 matrix 项失败不影响其他平台，失败平台不产出资产。
- 已发布 tag 不允许覆盖；发现问题时发新 tag，不在旧 tag 上重打。

## 7 安装方式

### 7.1 手动解压

适用于：离线环境、自定义安装路径、对安装脚本不信任的用户。

步骤：

1. 从 GitHub Releases 下载 `feng-<version>-<os>-<arch>.zip`
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

## 8 实施 TODO

首版交付的能力矩阵如下：

- `macos-arm64` host：支持 native 编译 / 运行 `macos-arm64` 程序，并支持交叉编译 `linux-x64` 与 `linux-arm64` 程序。
- `linux-x64` host：支持 native 编译 / 运行 `linux-x64` 程序，并支持交叉编译 `linux-arm64` 程序。
- `linux-arm64` host：支持 native 编译 / 运行 `linux-arm64` 程序，并支持交叉编译 `linux-x64` 程序。
- 本 TODO 不要求产出 `macos-x64` 程序，也不支持在 Linux host 上交叉编译 macOS 程序。平台标识以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准，CLI 与工具链转换分别以 [feng-cli.md](../docs/feng-cli.md) 和 [feng-build.md](../docs/feng-build.md) 为准。

实施必须严格按以下阶段顺序进行，每个阶段都是可独立交付的实施单元，并遵守统一阶段门禁：

1. 完成本阶段全部任务和专项验收，并在三个 host 上通过全量 `make test`。`make test` 是仓库全量回归入口，已包含单元测试、UBSan、smoke、CLI、std、fcts 与性能约束检查。
2. 提交本阶段的全部变更和验收结果供人工 Review；全量回归通过不代表可以自动进入下一阶段。
3. 只有人工 Review 通过且收到“开始下一阶段”的明确人工指令后，才能实施下一阶段。在此之前必须停止在当前阶段，不得提前修改后续阶段的文档、代码或测试。

### 8.1 阶段 1：可独立验证的精简工具链

本阶段只交付精简脚本和工具链产物，不改变 Feng CLI 的编译器选择行为。

任务：

- [x] `scripts/fetch_llvm.sh`：下载并解压 `macos-arm64`、`linux-x64`、`linux-arm64` 的 LLVM 官方预编译包到 `local/llvm/`。
- [ ] `scripts/trim_llvm.sh`：从单个 host LLVM root 原子精简 `clang`、`lld` / `ld.lld`、`lldb`、`lldb-dap` 及必要运行依赖，分别产出 `toolchain/llvm/macos-arm64/`、`toolchain/llvm/linux-x64/`、`toolchain/llvm/linux-arm64/`。
- [x] `scripts/fetch_musl.sh`：从 musl.cc 下载并解压 `linux-x64` 与 `linux-arm64` 配套预构建包到 `temp/musl/`。
- [ ] `scripts/trim_musl.sh`：产出 `toolchain/sysroot/linux-x64/` 与 `toolchain/sysroot/linux-arm64/`，保留 §4 / §9 规定的 musl、CRT、libgcc 与相对目录关系，排除 GCC / binutils host 可执行工具，并记录来源、版本和上游许可。

独立交付与回归门：

- [ ] 三份 LLVM 产物在对应 host 上通过 `clang`、`lld`、`lldb`、`lldb-dap` 启动验收，并校验二进制架构与动态依赖。
- [ ] 使用精简 Clang / LLD 与两份 sysroot 直接链接最小 C ELF，验证 x64 / arm64 的 CRT、libgcc 和 musl 链路完整，不依赖 Feng CLI。
- [ ] `macos-arm64`、`linux-x64`、`linux-arm64` 三个 host 的全量 `make test` 通过。

### 8.2 阶段 2：统一相对布局与 CLI 路径基础设施

本阶段只收敛路径和开发布局，不切换现有 driver 的编译器选择策略。

任务：

- [ ] Makefile 以 `build/<host-platform>/` 作为当前 native 开发构建根，并在其中创建 `toolchain/llvm -> ../../../toolchain/llvm/<host-platform>` 与 `toolchain/sysroot -> ../../../toolchain/sysroot`；`make clean` 随 `build/` 清理软链接。
- [ ] 将 Feng 可执行文件绝对路径解析与相对路径组装收敛到 `src/cli/common.*`，runtime、Clang 与 `lldb-dap` 共用，不增加工具链环境变量或重复实现。
- [ ] 为可执行文件定位、相对目录组装、软链接布局与缺失路径诊断补充独立回归。

独立交付与回归门：

- [ ] `build/<host-platform>/bin/feng` 可通过 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 观察到与发行包一致的布局，同时现有编译、runtime 与 DAP 行为不变。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.3 阶段 3：Feng 自身的三平台原生构建链

本阶段只交付在三个对应 native host 上按显式目标平台构建 `libfeng_unwind.a`、`libfeng_runtime.a` 和 Feng CLI 自身的能力，不改变 Feng CLI 编译用户程序时的 driver 行为，也不在本阶段交付 Feng CLI 自身的交叉编译；后者由阶段 6 独立交付。

任务：

- [ ] Feng 自身的 Makefile 增加 `TARGET_PLATFORM` 显式目标平台输入，取值以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准，未指定时默认为 host。该输入属于 Makefile / 维护脚本的 Feng 自身构建参数，与 Feng CLI 编译用户程序时的 `--platform` 职责分离，但两者共用同一平台值域。目标平台必须统一驱动 Feng CLI、runtime 和底层依赖的编译器、链接器、归档器、编译参数与 `build/<目标平台>/` 完整产物目录；不同目标平台不得复用对象、依赖文件、临时目录或静态库。
- [ ] `scripts/build_libunwind.sh` 作为维护脚本支持显式目标平台，分别构造 `extlib/macos-arm64/libfeng_unwind.a`、`extlib/linux-x64/libfeng_unwind.a`、`extlib/linux-arm64/libfeng_unwind.a`；未指定时默认构造 host 版本，每份产物必须校验对象格式与 CPU 架构。
- [ ] 构建指定目标平台的 Feng 发行产物时，Makefile 必须选择 `extlib/<目标平台>/libfeng_unwind.a`，将其对象合并进同一目标平台的 `libfeng_runtime.a`；任何对象格式、CPU 架构或目标平台不匹配都必须在构建阶段失败。
- [ ] `libfeng_runtime` 支持按显式目标平台独立构建，分别产出并定位 `macos-arm64`、`linux-x64`、`linux-arm64` 的 `libfeng_runtime.a`，每份 runtime 只合并同平台 `libfeng_unwind.a`，不得以 host runtime 改名替代其他平台 runtime。
- [ ] Feng CLI 自身支持按显式目标平台构建，本阶段分别在对应 native host 产出 `macos-arm64`、`linux-x64`、`linux-arm64` 版本的 `feng`；产物的可执行格式与 CPU 架构必须与指定目标一致。
- [ ] 补充显式目标平台、host 默认值、产物隔离、缓存污染拒绝和平台不匹配诊断回归，覆盖 `libfeng_unwind`、runtime 与 Feng CLI。

独立交付与回归门：

- [ ] 在三个 host 上分别验证显式目标平台构建出的 `feng`、`libfeng_runtime.a` 与 `libfeng_unwind.a` 的格式、CPU 架构与目标标识一致，并在相应 native host 启动目标 `feng` 验证 `--version`。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.4 阶段 4：标准库第三方 C 依赖的多平台预构建

本阶段只交付标准库依赖的 PCRE2、libsodium、libunistring 与 libuv 静态库的分平台预构建能力。这四个 `libfeng_std_*` 是 `std` 的第三方 C 库依赖，与 `libfeng_runtime`、`libfeng_unwind` 及 Feng 自身构建链无关；其产物只写入 `std/extlib/<目标平台>/`，不得合并进 runtime，也不得写入 Feng 自身使用的仓库根目录 `extlib/<目标平台>/`。

任务：

- [ ] `scripts/build_pcre2.sh`、`scripts/build_libsodium.sh`、`scripts/build_libunistring.sh`、`scripts/build_libuv.sh` 作为独立维护脚本支持显式目标平台输入，取值以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准，未指定时默认为 host。
- [ ] 四个脚本分别预生成 `std/extlib/macos-arm64/`、`std/extlib/linux-x64/`、`std/extlib/linux-arm64/` 下对应的 `libfeng_std_pcre2.a`、`libfeng_std_sodium.a`、`libfeng_std_unistring.a`、`libfeng_std_uv.a`；macOS 目标只要求在 `macos-arm64` host 原生构建，两个 Linux 目标同时支持在对应 Linux host 原生构建以及从三个支持的 host 交叉构建。
- [ ] 每个脚本必须按目标平台统一选择编译器、归档器、编译参数、Linux 目标 sysroot 和上游构建缓存 / 中间产物目录；Linux 交叉目标使用阶段 1 交付的 Clang、LLD 与对应 musl sysroot，不得使用 host 头文件、host 库或 host linker。
- [ ] 不得将 host 静态库改名为其他平台产物；每份静态库必须校验成员对象格式与 CPU 架构，两个 Linux 目标还必须通过对应 musl 环境的链接与运行验证。
- [ ] 补充四个脚本的显式目标平台、host 默认值、产物隔离、缓存污染拒绝和平台不匹配诊断回归，不改变 `libfeng_runtime`、`libfeng_unwind` 或 Feng CLI 的构建行为。

独立交付与回归门：

- [ ] 在三个 native host 上分别验证四个 `libfeng_std_*` 静态库的格式、CPU 架构与目标标识一致，并使用对应平台的四个静态库完成标准库专项回归。
- [ ] 分别验证 `macos-arm64 → linux-x64`、`macos-arm64 → linux-arm64`、`linux-x64 → linux-arm64`、`linux-arm64 → linux-x64` 的四库预构建路径；使用目标静态库和对应 musl sysroot 直接链接最小 C 验收程序，并在对应架构的 Linux musl 环境实际运行，不依赖尚未交付的 Feng CLI 交叉编译能力。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.5 阶段 5：三平台 native driver

本阶段交付三个 host 上 Feng CLI 编译用户程序的 native 能力；非 native Linux 目标在阶段 6 交付前必须明确报告目标不可用。

任务：

- [ ] 实现 Feng CLI `--platform`、host 默认值、平台参数校验、Clang 查找顺序和 native target triple 转换。
- [ ] macOS native 使用 `xcrun` 获取 SDK 并传入 `-isysroot`；Linux native 使用 host glibc，不传 musl sysroot。
- [ ] native 编译只定位并链接 `lib/<host 平台>/libfeng_runtime.a`，不得使用其他平台 runtime。
- [ ] 补充 `--platform` 诊断、native Clang / SDK / runtime 定位和三平台 native 编译回归。

独立交付与回归门：

- [ ] 在三个 host 上分别使用阶段 3 产出的目标 `feng` 完成 native `feng build` / `run` / `lsp` 验收，并验证 bundled toolchain 存在、显式 `CC` 覆盖和回退路径的行为。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.6 阶段 6：Linux 全矩阵交叉编译

本阶段交付 macOS 到两个 Linux 目标及两个 Linux host 之间的交叉编译，不包含 Linux 到 macOS。

任务：

- [ ] 在任一 host 交叉编译 Linux 时传入对应 `--target`、`--sysroot`、`--gcc-toolchain` 与 `-fuse-ld=lld`，不得误用 macOS SDK 或 host linker。
- [ ] 将阶段 3 的显式目标平台构建扩展到 Linux 交叉场景：`macos-arm64` host 可构建 `linux-x64` 与 `linux-arm64` 目标的 `feng`、`libfeng_runtime.a` 与 `libfeng_unwind.a`，`linux-x64` / `linux-arm64` host 可构建另一个 Linux 架构的对应产物；不同目标的构建缓存与产物必须隔离。
- [ ] 建立两个 Linux 目标 `libfeng_unwind.a` 与 `libfeng_runtime.a` 的可复现产出与交付流程，runtime 只合并同目标 unwind，Feng 交叉编译程序时只链接 `lib/<目标平台>/libfeng_runtime.a`。
- [ ] Linux host 上的 macOS 目标报告目标不可用，不查找、下载或接受 Apple SDK。
- [ ] 补充 sysroot / compiler runtime / target unwind / target runtime / LLD 缺失诊断，以及 Feng 自身与 Feng 用户程序的交叉构建回归。

独立交付与回归门：

- [ ] 分别验证 `macos-arm64 → linux-x64`、`macos-arm64 → linux-arm64`、`linux-x64 → linux-arm64`、`linux-arm64 → linux-x64`；每条路径都必须同时验证目标 `feng`、`libfeng_unwind.a`、`libfeng_runtime.a` 和 Feng 用户程序，检查可执行 / 对象格式、CPU 架构、ELF interpreter 与链接依赖，并消费阶段 4 已交付的对应平台 `libfeng_std_*` 完成标准库集成回归，在对应架构的 Linux musl 环境实际运行目标 `feng` 和用户程序。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.7 阶段 7：多平台 lib 构建与 `.fb` 组包

本阶段在阶段 5 / 6 的单目标 driver 和阶段 4 的标准库第三方 C 依赖产物之上，交付项目级多平台库构建与单一跨平台 `.fb`。CLI 语法以 [feng-cli.md](../docs/feng-cli.md) 为准，构建编排与包格式分别以 [feng-build.md](../docs/feng-build.md) 和 [feng-package.md](../docs/feng-package.md) 为准。

任务：

- [ ] 核心编译器直接模式继续保持一次只接受一个 `--platform` 和调用方给定的精确 `--out`；直编不自动追加平台目录、不感知多平台循环，也不负责 `.fb` 聚合。项目级 `feng build` / `feng pack` 对 `target=lib` 支持重复 `--platform=<os>-<arch>`，未指定时只使用 host 平台；`target=bin` 最多接受一个目标平台。
- [ ] `feng build` 按目标平台分别调用核心编译器，每次传入单个 `--platform=<目标平台>` 与 `--out=<项目输出根>/<目标平台>`，将 `gen/`、`mod/`、`assets/`、`bin/`、`lib/`、`obj/`、`ir/` 与 `extlib/` 全部隔离到该平台完整开发构建根；相同平台集合必须递归传递给本地 `target=lib` 依赖。
- [ ] `feng pack` 校验各平台 `mod/` 的模块集合与公开语义事实等价、普通 `assets/` 内容一致后分别提取一套；平台相关公开 API / ABI 或同包路径普通资源不一致时拒绝合并。
- [ ] `feng pack` 从每个 `<项目输出根>/<目标平台>/lib/` 与 `extlib/` 提取分平台制品，写入包内 `lib/<目标平台>/` 与 `extlib/<目标平台>/`，将实际平台集合写入 `feng.fm.arch`，最终包写入 `<项目输出根>/pkg/`；任一平台构建或校验失败时整体失败，不得生成部分平台 `.fb`。
- [ ] Linux host 指定 macOS 目标时明确报告目标不可用，不查找或接受 Apple SDK；平台标识合法不得误报为参数格式错误。
- [ ] 补充单平台默认值、重复 `--platform`、分平台产物隔离、递归本地依赖、远程包目标制品缺失、公开符号表不一致、`arch` 精确匹配和原子组包回归。

独立交付与回归门：

- [ ] 在 `macos-arm64` host 执行 `feng build --platform=macos-arm64 --platform=linux-x64 --platform=linux-arm64` 与对应 `feng pack`，生成一个包含三平台 `lib/` / `extlib/` 制品的 `.fb`，并在三个目标环境分别作为依赖完成编译、链接与运行。
- [ ] 在两个 Linux host 分别构建包含 `linux-x64` 与 `linux-arm64` 的 `.fb`，并验证请求 `macos-arm64` 明确失败且不留下部分包。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.8 阶段 8：macOS / Linux DAP

本阶段只交付调试后端的跨平台定位、启动与诊断，不与发布脚本变更混合。

任务：

- [ ] `feng dap` 在 macOS 与 Linux 按 [feng-cli.md](../docs/feng-cli.md) 规定共用阶段 2 的路径能力，依次定位并启动 bundled、`PATH` 与 macOS `xcrun` 提供的 `lldb-dap`。
- [ ] Linux 缺失 `libpython3.11.so.1.0` 时保留动态加载器真实错误并给出可操作诊断；Feng 本身不使用 Python 脚本。
- [ ] 补充 macOS / Linux 后端定位、启动、缺失依赖和 DAP 基础会话回归。

独立交付与回归门：

- [ ] 三个 host 分别完成 `feng dap` 真实后端启动与基础调试会话；Linux 验收环境已安装 §5.2 规定的 libpython。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.9 阶段 9：分发、CI 与安装

本阶段在前八个阶段的可验收产物之上组装分发包，不在发布流程中临时下载、精简或修补工具链。

任务：

- [ ] `scripts/release.sh` 按能力矩阵组装 `feng-<version>-<os>-<arch>.zip`，将 host LLVM、所需目标 runtime 与两份 Linux sysroot 放入约定相对位置。
- [ ] `scripts/release.sh` 调用阶段 3 / 6 的显式目标平台构建入口，分别产出对应平台的 `feng`、`libfeng_unwind.a` 与 `libfeng_runtime.a`；组装前再次校验可执行 / 对象格式和 CPU 架构，不依赖 runner `uname` 冒充显式目标。
- [ ] `feng-<version>-macos-arm64.zip` 包含三份目标 runtime；两份 Linux zip 都包含两份 Linux runtime；三份 zip 都包含 `linux-x64` / `linux-arm64` sysroot，不得通过改名复用其他平台 runtime。
- [ ] `.github/workflows/release.yml` 由 tag / `workflow_dispatch` 触发三个 host 构建，能够正确聚合各目标 runtime，并产出三份规范命名的 zip。
- [ ] `scripts/install.sh` 按 [feng-os-arch.md](../docs/feng-os-arch.md) 归一化当前 host，下载对应 zip，原子解压并配置 `PATH`，失败时不留半成品。

独立交付与回归门：

- [ ] 在三个干净 host 上分别从 zip 安装，验证 `feng --version` / `VERSION`、native `build` / `run` / `lsp` / `dap`、bundled Clang / LLD / LLDB，并复验阶段 6 的四条交叉编译路径与阶段 7 的多平台 `.fb`。
- [ ] 验证 bundled toolchain 存在 / 缺失时的定位与诊断，以及安装失败无残留。
- [ ] 三个 host 的全量 `make test` 通过。

## 9 Linux musl 交叉链接已定方案

本节记录 Linux musl 交叉链接的分发决策；Feng 平台到 Clang 参数的转换以 [feng-build.md](../docs/feng-build.md) 为主规范。

1. **linker**：各 host 分发包使用自身 `toolchain/llvm/bin/lld`，并保留 `bin/ld.lld -> lld`。该可执行文件来自与 `clang` 相同的 LLVM 官方包，因此可在当前 host 上运行并链接 Linux ELF。driver 传入 `-fuse-ld=lld`，由 Clang 基于自身安装目录定位 `bin/ld.lld`，不传 `--ld-path`，也不使用 musl.cc 预构建包中的 `ld`。
2. **目标 C 环境与 compiler runtime**：`toolchain/sysroot/<linux-target>/` 使用与目标架构匹配的 musl.cc 配套包，同时保留 musl 头文件、库、动态加载器、musl CRT，以及目标 `crtbegin*` / `crtend*`、`libgcc.a`、`libgcc_eh.a`、`libgcc_s.so*`。仅排除不会在目标程序中被链接且不作为当前 host 工具使用的 GCC / binutils 可执行文件。
3. **Feng runtime**：交叉目标必须使用 `lib/<目标平台>/libfeng_runtime.a`，不得误用 host runtime。该路径属于本文 §4 已定义的分平台 runtime 布局，不在 sysroot 中重复分发。
4. **可用性边界**：`lld`、目标 sysroot / compiler runtime 与目标 `libfeng_runtime.a` 必须同时存在并通过链接验收，才能宣称该 Linux 交叉目标可用。
