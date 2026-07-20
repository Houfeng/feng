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
- toolchain 形态：bundle 精简版（从 LLVM 官方预编译包剥离，核心保留 `clang`、`lldb`、`lldb-dap`，Linux 交叉链接器按 §9 决策，不自建 LLVM）
- 运行时分发形态：仅静态库 `.a` / `.lib`
- 分发渠道：GitHub Releases

首版明确不做：

- Windows 实际打包（脚本与工作流预留扩展点，不产出二进制）
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
│           └── usr/lib/          # 目标平台系统库
└── VERSION                       # 必须：纯文本版本号，单行
```

- Windows 平台下，`bin/` 中可执行文件追加 `.exe` 后缀，`lib/` 中静态库后缀切换为 `.lib`。
- `lib/` 按目标平台分子目录（`lib/<os>-<arch>/`，取值见 [feng-os-arch.md](../docs/feng-os-arch.md)）。分发包至少包含其 host 平台 runtime；支持交叉编译时，可同时包含其他目标平台 runtime 子目录。
- `include/` 为 runtime 公共 ABI 头文件，平台无关（C 源码），不分平台子目录，单一一份供所有平台使用，扁平置于 `include/` 根下。`feng_runtime.h` 内部以相对路径 `#include "feng_runtime_contract.inc"`，二者位于同一目录；标准 C 头文件（`<stdint.h>` 等）与系统头文件（`<unwind.h>`）由目标平台 SDK / sysroot 提供，不重复放入 runtime include 目录。
- `toolchain/llvm/` 保持 LLVM 官方包的统一根目录布局，`clang`、`lldb` 与 `lldb-dap` 必须来自同一 LLVM 版本和同一 host 平台包。`bin/clang` 与 `lib/clang/<version>/` 的相对位置关系由 clang 自动推导（`-print-resource-dir`），driver 无需额外指定 `-resource-dir` 或 `-isystem`；`bin/lldb` / `bin/lldb-dap` 与 `lib/liblldb.*` 也保持官方包内的相对位置。`lib/clang/<version>/include/`（编译器内置头）平台无关；`lib/clang/<version>/lib/<os>/`（编译器运行时库）按目标 OS 分目录。
- `toolchain/sysroot/` 按目标平台分子目录，保持 `--sysroot` 官方约定结构（`usr/include/` + `usr/lib/`），具体选择规则见 [feng-build.md](../docs/feng-build.md)。Linux 交叉编译 sysroot 可随包分发 musl；macOS SDK 受 Apple 许可限制不进入 Feng 分发包，macOS native 编译使用用户合法安装的系统 SDK，macOS 作为交叉目标时也必须由用户自行合法获取 SDK。
- 分发物不包含任何 Feng 源码、`.o` / `.obj` 中间产物、构建缓存。
- `feng` 编译器基于自身位置查找 runtime 静态库、头文件与 toolchain：runtime 位于 `<feng 可执行文件目录>/../lib/` 与 `../include/`，Clang 和 `lldb-dap` 位于 `<feng 可执行文件目录>/../toolchain/llvm/bin/`。不引入 `FENG_HOME` / `FENG_TOOLCHAIN` 等环境变量；完整查找顺序见 [feng-build.md](../docs/feng-build.md) 与 [feng-cli.md](../docs/feng-cli.md) 的 DAP 规范。

## 5 toolchain 形态

分发包内 `toolchain/` 为精简版 LLVM 工具链与交叉编译 sysroot，与 `bin/`、`lib/`、`include/` 并列置于分发包根下。

- **从 LLVM 官方预编译包剥离，不自建 LLVM/Clang**；核心只保留 `clang`、`lldb`、`lldb-dap` 及其运行所必需的最小依赖集，不含 `llvm-*`、`clang-format`、`clang-tidy` 等其他通用 LLVM 工具。`lld` 是否作为 Linux 交叉链接器进入最小集合由 §9 单独决策,不得在决策前由精简脚本私自加入。
- 精简由 `scripts/fetch_llvm.sh` + `scripts/trim_llvm.sh` 完成（维护性脚本，不在发布流程）：`fetch_llvm.sh` 下载并解压 LLVM 官方预编译包到 `local/llvm/`（持久 cache，gitignored，不受 `make clean` 或测试清理 `temp/` 影响），`trim_llvm.sh` 从单个已解压 LLVM root 中同时精简 clang、lldb 与 lldb-dap，原子产出到仓库 `toolchain/llvm/<os>-<arch>/`。合并为一个精简脚本，避免两个脚本共享输出根目录时相互删除产物，并保证 clang 与 LLDB 的版本、来源和目标平台一致。本方案只约束产出形式与体积目标：精简后 `toolchain/` 解压体积目标控制在 300 MB 量级（实际值待实施时验证，写入 release notes）。
- `toolchain/sysroot/` 为交叉编译 sysroot，按目标平台分子目录。Linux 交叉编译目标基于 musl libc（MIT 许可，自由可分发），由 `scripts/fetch_musl.sh` 下载预构建包到 `temp/musl/`，再由 `scripts/trim_musl.sh` 精简到仓库 `toolchain/sysroot/<os>-<arch>/`（git lfs 管理）；musl 主要用于交叉编译场景，未来 linux 平台原生编译时采用 glibc（由 host 系统提供）。macOS 目标受 Apple SDK 版权限制不可自由分发，需用户自行合法获取。
- 精简 toolchain 的版本、来源、剥离清单由独立子任务文档承载，不在本文件展开，避免方案膨胀。
- `feng` 编译器基于自身位置查找 `toolchain/`。源码开发构建通过 Makefile 创建 `build/toolchain/llvm -> ../../toolchain/llvm/<host-platform>` 软链接，使 `build/bin/feng` 观察到的 `../toolchain/llvm/bin/` 与发行包完全一致；`make clean` 删除整个 `build/`，软链接不作为持久产物或分发内容。

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
4. 执行 `make cli runtime`（产出 `build/bin/feng`、`build/lib/libfeng_runtime.a`、`build/include/feng_runtime.h` 与 `build/include/feng_runtime_contract.inc`）
5. 组装分发目录树：`build/bin/feng` 放入 `bin/`，`build/include/` 下的两个头文件放入 `include/`，`build/lib/libfeng_runtime.a` 放入 `lib/<os>-<arch>/`，仓库 `toolchain/llvm/<os>-<arch>/` 对应 host 平台产物放入分发包 `toolchain/llvm/`，需要分发的 `toolchain/sysroot/<target-os>-<target-arch>/` 保持目标平台子目录放入分发包 `toolchain/sysroot/`，并生成 `VERSION` 文件（写入 git tag 版本号）
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

首版 `macos-arm64`、`linux-x64` 与 `linux-arm64` 发布前需完成的实施项：

- [x] `scripts/fetch_llvm.sh`：下载并解压 LLVM 官方预编译包到 `local/llvm/`（持久 cache，gitignored，维护性脚本）
- [ ] `scripts/trim_llvm.sh`：从同一 LLVM root 原子精简 clang + lldb + lldb-dap，产出到 `toolchain/llvm/<os>-<arch>/`（git lfs 管理），支持 `macos-arm64`、`linux-x64` 与 `linux-arm64`
- [x] `scripts/fetch_musl.sh`：从 musl.cc 下载并解压 Linux musl 预构建包到 `temp/musl/`（持久 cache，gitignored，维护性脚本）
- [x] `scripts/trim_musl.sh`：从 `temp/musl/` 精简 musl sysroot（仅保留 `include/` + `lib/`，排除 GCC 工具链）到 `toolchain/sysroot/<os>-<arch>/`（git lfs 管理）
- [ ] `scripts/release.sh`：构建入口，编排 `build_libunwind.sh` + `make cli runtime` + 组装分发目录树，产出安装包到 `release/`
- [ ] `.github/workflows/release.yml`：CI 工作流，tag 触发，各 matrix 项调用 `scripts/release.sh`
- [ ] `scripts/install.sh`：在线安装脚本（自动检测平台 + 下载到系统临时目录 + 解压 + 自动配 PATH）
- [ ] Linux musl 交叉链接工具链：按 §9 的人工决策补齐目标 ELF linker 与 compiler runtime，并纳入精简脚本和发布验收
- [ ] 发布前验收：`feng --version` 与 VERSION 一致；`feng build` / `run` / `lsp` / `dap` 可用；bundled toolchain 存在/缺失两种情形定位正确；干净环境安装无残留

## 9 待人工决策项

以下事项影响后续迭代，但不阻塞首版 macos-arm64 发布，列出以备决策：

1. **Linux musl 版本与来源**：`fetch_musl.sh` 下载的 musl 预构建包版本与提供方（如 musl.cc）由人工决策锁定，确保 ABI 稳定与许可合规。musl 仅用于交叉编译 sysroot；未来 linux 平台原生编译时采用 glibc，其最低版本基线另由人工决策。
2. **Linux musl 交叉链接依赖**：`--target` + musl `--sysroot` 不包含目标 linker 与 compiler runtime。当前 macOS LLVM Clang 会回退调用不支持 ELF 的 `/usr/bin/ld`；当前精简 musl sysroot 也不含 `crtbegin` / `crtend` 与 libgcc,macOS LLVM resource-dir 只含 Darwin compiler-rt。必须由人工决定是把 LLVM 官方包中的 `lld` 与目标 compiler-rt 纳入 `toolchain/llvm/`,还是从 musl 交叉工具链保留等价 linker / libgcc 支持；该决策完成前不得宣称 Linux 交叉链接可用。
