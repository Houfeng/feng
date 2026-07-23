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
│   │   │   ├── llvm-ar        # 跨目标静态归档工具
│   │   │   ├── llvm-ranlib    # 跨目标静态归档索引工具
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
- `lib/` 按目标平台分子目录（`lib/<os>-<arch>/`，取值见 [feng-os-arch.md](../docs/feng-os-arch.md)）。首版三份分发包都包含 `macos-arm64`、`linux-x64`、`linux-arm64` 三份 runtime；每份 runtime 必须在对应 native host 构建，再由发布汇聚任务统一放入各分发包，不得把 host runtime 改名为其他平台产物。
- `include/` 为 runtime 公共 ABI 头文件，平台无关（C 源码），不分平台子目录，单一一份供所有平台使用，扁平置于 `include/` 根下。`feng_runtime.h` 内部以相对路径 `#include "feng_runtime_contract.inc"`，二者位于同一目录。最终编译 / 链接所需的标准 C 头文件（`<stdint.h>` 等）与系统头文件（`<unwind.h>`）由目标平台 SDK / sysroot 提供，不复制到 runtime include 目录；§9.4 的 SDK-free `target=lib` 编译头闭包也不得通过复制 Apple SDK 头文件实现。
- `toolchain/llvm/` 保持 LLVM 官方包的统一根目录布局，`clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb` 与 `lldb-dap` 必须来自同一 LLVM 版本和同一 host 平台包。`bin/clang` 与 `lib/clang/<version>/` 的相对位置关系由 clang 自动推导（`-print-resource-dir`），driver 无需额外指定 `-resource-dir` 或 `-isystem`；`bin/ld.lld` 必须保留为指向 `bin/lld` 的入口；`bin/lldb` / `bin/lldb-dap` 与 `lib/liblldb.*` 也保持官方包内的相对位置。`lib/clang/<version>/include/`（编译器内置头）平台无关；`lib/clang/<version>/lib/<os>/`（编译器运行时库）按目标 OS 分目录。
- `toolchain/sysroot/` 按目标平台分子目录，保持 `--sysroot` 约定的 `usr/include/` + `usr/lib/` 主视图。Linux musl sysroot 从 musl.cc 配套包精简，除 musl 头文件、库与 `crt1` / `crti` / `crtn` 外，还必须保留目标 `crtbegin*` / `crtend*`、`libgcc.a`、`libgcc_eh.a`、`libgcc_s.so*` 及 Clang GCC toolchain 检测所需的相对目录关系；不保留 musl.cc 中只能在特定宿主上运行的交叉链接器可执行文件。具体调用参数见 [feng-build.md](../docs/feng-build.md)。macOS SDK 不进入 Feng 分发包，也不由 Feng 下载或复制到 Linux；`target=lib` 默认使用 §9 定义的 SDK-free 静态库构建路径，确需目标 SDK 时只接受用户显式传入的通用 `--sysroot=<path>`。
- 分发物不包含任何 Feng 源码、`.o` / `.obj` 中间产物、构建缓存。
- `feng` 编译器基于自身位置查找 runtime 静态库、头文件与 toolchain：runtime 位于 `<feng 可执行文件目录>/../lib/` 与 `../include/`，Clang 和 `lldb-dap` 位于 `<feng 可执行文件目录>/../toolchain/llvm/bin/`。不引入 `FENG_HOME` / `FENG_TOOLCHAIN` 等环境变量；完整查找顺序见 [feng-build.md](../docs/feng-build.md) 与 [feng-cli.md](../docs/feng-cli.md) 的 DAP 规范。

## 5 toolchain 形态

分发包内 `toolchain/` 为精简版 LLVM 工具链与交叉编译 sysroot，与 `bin/`、`lib/`、`include/` 并列置于分发包根下。

- **从 LLVM 官方预编译包剥离，不自建 LLVM/Clang**；核心只保留 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap` 及其运行所必需的最小依赖集，不含其他 `llvm-*`、`clang-format`、`clang-tidy` 等通用 LLVM 工具。Linux musl 交叉链接统一使用该 host 平台 LLVM 包中的 `lld`，不分发 musl.cc 中的 linker 可执行文件。
- 精简由 `scripts/fetch_llvm.sh` + `scripts/trim_llvm.sh` 完成（维护性脚本，不在发布流程）：`fetch_llvm.sh` 下载并解压 LLVM 官方预编译包到 `local/llvm/`（持久 cache，gitignored，不受 `make clean` 或测试清理 `temp/` 影响），`trim_llvm.sh` 从单个已解压 LLVM root 中同时精简 clang、lld、lldb 与 lldb-dap，原子产出到仓库 `toolchain/llvm/<os>-<arch>/`。合并为一个精简脚本，避免两个脚本共享输出根目录时相互删除产物，并保证四个工具的版本、来源和 host 平台一致。精简脚本只保留运行与目标编译所必需的文件，实际解压体积在维护验收时记录，不以删除必要运行依赖换取固定体积。
- `toolchain/sysroot/` 为交叉编译 sysroot，按目标平台分子目录。Linux 交叉编译目标基于 musl libc，由 `scripts/fetch_musl.sh` 下载 musl.cc 配套预构建包到 `temp/musl/`，再由 `scripts/trim_musl.sh` 将 musl sysroot 与目标 CRT / GCC 支持运行库一起精简到仓库 `toolchain/sysroot/<os>-<arch>/`（git lfs 管理）；不保留预构建包中的 GCC、`ld` 等 host 可执行工具。精简产物的 README 必须分别记录 musl 与 GCC runtime 的来源、版本和上游许可，不得把整个 sysroot 笼统标记为单一 musl 许可。musl 主要用于交叉编译场景，Linux 平台原生编译采用 glibc（由 host 系统提供）。macOS 目标受 Apple SDK 版权限制不可自由分发，需用户自行合法获取。
- 精简 toolchain 的版本、来源、剥离清单由独立子任务文档承载，不在本文件展开，避免方案膨胀。
- `feng` 编译器基于自身位置查找 `toolchain/`。源码开发继续使用现有 `build/` 根；Makefile 在 `build/toolchain/` 下创建 `llvm -> ../../toolchain/llvm/<host-platform>` 与 `sysroot -> ../../toolchain/sysroot` 两个软链接，使 `build/bin/feng` 观察到的 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 都与发行包布局一致，不要求为 Feng 自身引入 `build/<目标平台>/` 多目标构建体系。`make clean` 删除整个 `build/`，软链接不作为持久产物或分发内容。

### 5.1 macOS 系统前置条件

macOS SDK 不随 Feng 分发。使用 Feng 内置 Clang 编译 macOS 目标时，系统必须已安装 Xcode 或 Xcode Command Line Tools，以提供 macOS SDK、系统链接器与 `xcrun`。用户可执行以下命令安装 Command Line Tools:

```bash
xcode-select --install
```

Feng 在 macOS host 最终链接 macOS 可执行程序时，通过 `xcrun --sdk macosx --show-sdk-path` 获取 SDK 路径并向内置 Clang 传入 `-isysroot`。命令不可用、未选中有效 developer directory 或 SDK 不存在时,编译必须给出明确诊断；安装脚本不得静默安装 Xcode 或 Command Line Tools。该前置条件不适用于 §9 定义的 SDK-free `target=lib` 对象生成与静态归档，也不改变 `feng --version` 等无需编译的命令。

### 5.2 Linux LLDB 系统前置条件

LLVM 22.1.8 官方 Linux x64 与 arm64 包中的 `liblldb.so.22.1.8` 都直接依赖 `libpython3.11.so.1.0`,而官方包本身不携带该共享库。Feng 分发包不额外捆绑 libpython；Linux 用户使用 `lldb` 或 `feng dap` 前,必须通过所在发行版安装能够提供该 soname 的 Python 3.11 共享库包。Feng 不使用 Python 脚本,但动态加载器仍要求满足 `liblldb` 的直接依赖。

该前置条件只影响 `lldb` / `lldb-dap` 启动；`feng` 编译、`lsp` 等不加载 `liblldb` 的路径不应因此依赖 Python。若共享库缺失,`feng dap` 必须保留后端加载器的真实错误并给出可操作诊断。

## 6 构建与发布工作流

### 6.1 触发

GitHub Actions 工作流 `release.yml` 由推送形如 `v*.*.*` 的 tag 触发，并支持 `workflow_dispatch` 手动触发用于试发。

### 6.2 原生构件 matrix

```yaml
strategy:
  matrix:
    include:
      - { os: macos, arch: arm64, runner: macos-14 }
      - { os: linux, arch: x64,  runner: ubuntu-latest }
      - { os: linux, arch: arm64, runner: ubuntu-24.04-arm }
      # 扩展时追加：macos-x64、windows-x64 等
```

三个 matrix 项互不交叉构建 Feng 自身，只在对应 native host 产出当前平台的 `feng`、`libfeng_runtime.a` 与公共头文件，并上传命名明确的原生构件。各 host 平台的精简 LLVM 产物位于仓库 `toolchain/llvm/<os>-<arch>/`，各目标平台的 sysroot 位于仓库 `toolchain/sysroot/<os>-<arch>/`，均由 git lfs 管理。CI checkout 即有，无需构建期精简，只需从仓库目录复制。

三个原生构件任务全部成功后，独立汇聚任务下载三份构件并组装三份 `feng-<version>-<os>-<arch>.zip`。因此发布流程不要求 Linux 生成 macOS runtime，也不要求 macOS 或任一 Linux host 单独产生完整 release zip。

### 6.3 原生构建与汇聚步骤

原生构件任务：

1. checkout 仓库（含 git lfs 管理的精简 LLVM 与 Linux sysroot）
2. 安装当前 host 的构建依赖
3. 执行 `scripts/build_libunwind.sh`，只构建当前 host 的 `extlib/<host-platform>/libfeng_unwind.a`
4. 执行现有 native `make cli runtime`，产出 `build/bin/feng`、`build/lib/libfeng_runtime.a` 与 `build/include/`；不向 Makefile 增加用于交叉构建 Feng 自身的 `TARGET_PLATFORM`
5. 校验 `feng`、runtime 与 unwind 的格式和 CPU 架构，执行当前 host 全量 `make test`
6. 上传 `release-component-<host-platform>`，其中 `feng` 与 runtime 均带明确平台元信息，公共头文件同时记录内容摘要

汇聚任务：

1. 等待并下载三份原生构件，不执行任何跨 host 编译
2. 校验三份 runtime 平台互异且完整，公共头文件内容一致
3. 针对每个 host 平台选择对应 `feng` 与 `toolchain/llvm/<host-platform>/`
4. 将三份 runtime、两份 Linux sysroot、公共头文件及 `VERSION` 放入每一份分发目录
5. 执行 `scripts/release.sh` 的纯组装与校验入口，生成三份规范命名的 zip
6. 上传到 GitHub Release 对应 tag

toolchain 精简产物在**本地维护**：开发者用 `fetch_llvm.sh` + `trim_llvm.sh` + `fetch_musl.sh` + `trim_musl.sh` 在本地产出各平台精简结果，提交到仓库（git lfs）。CI 只需 checkout 后从仓库目录复制，不做任何精简。这样 CI 构建步骤简、快，且不依赖 musl.cc / LLVM Releases 网络可达性。

### 6.4 失败与回滚

- 任一原生构件任务失败时，汇聚任务不得生成任何 release zip；已成功的平台构件只作为本次失败工作流的诊断输入，不得单独发布。
- 汇聚任务失败时不得上传部分平台 release zip。
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

## 8 实施 TODO：Feng 编译器与工具发行包

本大阶段只交付三个 native host 上的 Feng 编译器、runtime、调试工具与安装包，不实现 Feng 用户程序的跨平台编译、标准库多平台制品或 `.fb` 多平台组包；这些能力统一由 §9 承载。Feng 自身不要求交叉构建：三个 native CI 任务分别生成当前平台的 `feng` 与 runtime，汇聚任务再把三份 runtime 放入每一份发行包。

§8 与 §9 的所有子阶段都必须严格遵守统一门禁：

1. 完成本子阶段全部任务和专项验收，并在 `macos-arm64`、`linux-x64`、`linux-arm64` 三个 host 上通过全量 `make test`。`make test` 是仓库全量回归入口，已包含单元测试、UBSan、smoke、CLI、std、fcts 与性能约束检查。
2. 提交本子阶段全部变更和验收结果供人工 Review；全量回归通过不代表可以自动进入下一子阶段。
3. 只有人工 Review 通过且收到“开始下一阶段”的明确人工指令后，才能实施下一子阶段。§8 全部通过人工 Review 前不得开始 §9。

### 8.1 可独立验证的精简工具链

本阶段只交付精简脚本和工具链产物，不改变 Feng CLI 的工具选择行为。

任务：

- [x] `scripts/fetch_llvm.sh`：下载并解压 `macos-arm64`、`linux-x64`、`linux-arm64` 的 LLVM 官方预编译包到 `local/llvm/`。
- [x] `scripts/trim_llvm.sh`：从单个 host LLVM root 原子精简 `clang`、`lld` / `ld.lld`、`lldb`、`lldb-dap` 及必要运行依赖，分别产出 `toolchain/llvm/macos-arm64/`、`toolchain/llvm/linux-x64/`、`toolchain/llvm/linux-arm64/`。
- [x] 扩展 `scripts/trim_llvm.sh`，从同一官方包保留 `llvm-ar` 与 `llvm-ranlib`，供 §9 的跨目标静态归档使用；不得引入 host `ar` 处理其他目标对象的隐式依赖。
- [x] `scripts/fetch_musl.sh`：从 musl.cc 下载并解压 `linux-x64` 与 `linux-arm64` 配套预构建包到 `temp/musl/`。
- [x] `scripts/trim_musl.sh`：产出 `toolchain/sysroot/linux-x64/` 与 `toolchain/sysroot/linux-arm64/`，保留 §4 / §10 规定的 musl、CRT、libgcc 与相对目录关系，排除 GCC / binutils host 可执行工具，并记录来源、版本和上游许可。

独立交付与回归门：

- [x] `macos-arm64` LLVM 产物在对应 host 上通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap` 启动验收，并校验二进制架构与动态依赖。
- [ ] `linux-x64` LLVM 产物在对应 host 上通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap` 启动验收，并校验二进制架构与动态依赖。
- [ ] `linux-arm64` LLVM 产物在对应 host 上通过 `clang`、`lld`、`llvm-ar`、`llvm-ranlib`、`lldb`、`lldb-dap` 启动验收，并校验二进制架构与动态依赖。
- [x] 使用精简 Clang / LLD 与两份 sysroot 直接链接最小 C ELF，验证 x64 / arm64 的 CRT、libgcc 和 musl 链路完整，不依赖 Feng CLI。
- [x] `macos-arm64` host 的全量 `make test` 在 Codex 沙箱外通过。
- [ ] `linux-x64` host 的全量 `make test` 通过。
- [ ] `linux-arm64` host 的全量 `make test` 通过。

### 8.2 统一相对布局与 CLI 路径基础设施

本阶段只收敛发行包与源码开发的路径基础设施，不切换现有 driver 的编译器选择策略，也不改变 Feng 自身现有 `build/` 产物层级。

任务：

- [ ] Makefile 在现有 `build/toolchain/` 中创建 `llvm -> ../../toolchain/llvm/<host-platform>` 与 `sysroot -> ../../toolchain/sysroot`；`make clean` 随 `build/` 清理软链接。
- [ ] 将 Feng 可执行文件绝对路径解析与相对路径组装收敛到 `src/cli/common.*`，runtime、Clang 与 `lldb-dap` 共用，不增加工具链环境变量或重复实现。
- [ ] 为可执行文件定位、相对目录组装、软链接布局与缺失路径诊断补充独立回归。

独立交付与回归门：

- [ ] `build/bin/feng` 可通过 `../toolchain/llvm/` 与 `../toolchain/sysroot/` 观察到与发行包一致的布局，同时现有编译、runtime 与 DAP 行为不变。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.3 三平台 native 发行构件

本阶段只在三个对应 native host 构建 Feng 自身及其 runtime，不向 Makefile 或维护脚本引入交叉构建 Feng 自身的 `TARGET_PLATFORM` 体系。

任务：

- [ ] `scripts/build_libunwind.sh` 保持 native host 构建入口，分别由三个 host 生成 `extlib/<host-platform>/libfeng_unwind.a`，每份产物必须校验对象格式与 CPU 架构。
- [ ] 现有 `make cli runtime` 在每个 native host 选择同平台 `libfeng_unwind.a`，产出 `build/bin/feng`、`build/lib/libfeng_runtime.a` 与 `build/include/`；runtime 只合并同平台 unwind。
- [ ] 三个 CI 原生构件任务分别上传当前平台 `feng` 与 `libfeng_runtime.a`，不得由任一 host 伪造、改名或交叉生成其他平台 runtime。
- [ ] 为 native 构件的平台标识、格式、CPU 架构、公共头文件摘要和缓存污染拒绝补充回归。

独立交付与回归门：

- [ ] 三个 host 上分别验证 native `feng --version`、runtime / unwind 对象格式与 CPU 架构，并确认三个原生构件可被后续汇聚任务唯一识别。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.4 macOS / Linux DAP

本阶段只交付发行工具中的调试后端定位、启动与诊断，不混入用户程序交叉编译或发布脚本变更。

任务：

- [ ] `feng dap` 在 macOS 与 Linux 按 [feng-cli.md](../docs/feng-cli.md) 规定共用 §8.2 的路径能力，依次定位并启动 bundled、`PATH` 与 macOS `xcrun` 提供的 `lldb-dap`。
- [ ] Linux 缺失 `libpython3.11.so.1.0` 或其他官方 LLDB 直接动态依赖时，保留动态加载器真实错误并给出与已验证 Linux 支持基线一致的可操作诊断；Feng 本身不使用 Python 脚本。
- [ ] 补充 macOS / Linux 后端定位、启动、缺失依赖和 DAP 基础会话回归。

独立交付与回归门：

- [ ] 三个 host 分别完成 `feng dap` 真实后端启动与基础调试会话；Linux 验收环境满足 §5.2 规定的系统动态依赖。
- [ ] 三个 host 的全量 `make test` 通过。

### 8.5 分发、CI 汇聚与安装

本阶段在 §8.1—§8.4 的可验收产物之上组装分发包，不在发布流程中临时下载、精简或修补工具链，也不交叉构建 Feng 自身。

任务：

- [ ] `.github/workflows/release.yml` 由 tag / `workflow_dispatch` 触发三个 native 构件任务；每个任务执行 §8.3 的 native 构建、构件校验和全量回归后上传 `release-component-<host-platform>`。
- [ ] 独立汇聚任务等待三份原生构件，校验三份 runtime 平台完整且公共头文件一致，再调用 `scripts/release.sh` 的纯组装入口。
- [ ] `scripts/release.sh` 不调用 Feng 自身的跨平台构建入口；它针对每份 zip 选择对应 host 的 `feng` 与 LLVM，并放入三份 native runtime、两份 Linux sysroot、公共头文件和 `VERSION`。
- [ ] 三份 `feng-<version>-<os>-<arch>.zip` 都包含 `macos-arm64`、`linux-x64`、`linux-arm64` runtime 与两份 Linux sysroot，不得通过改名复用其他平台 runtime。
- [ ] `scripts/install.sh` 按 [feng-os-arch.md](../docs/feng-os-arch.md) 归一化当前 host，下载对应 zip，原子解压并配置 `PATH`，失败时不留半成品。

独立交付与回归门：

- [ ] 在三个干净 host 上分别从 zip 安装，验证 `feng --version` / `VERSION`、native `build` / `run` / `lsp` / `dap` 以及 bundled Clang / LLD / LLVM ar / LLDB。
- [ ] 校验三份 zip 的 runtime / sysroot 集合、host toolchain 架构与相对定位，验证 toolchain 缺失时的诊断和安装失败无残留。
- [ ] 三个 host 的全量 `make test` 通过；完成人工 Review 并收到明确指令后，才能开始 §9。

## 9 实施 TODO：Feng 程序交叉编译与标准库

本大阶段以 §8 已发布的三个 native Feng 工具包为基础，交付 Feng 用户程序的 native / Linux 交叉编译、SDK-free macOS 静态库分片、标准库分平台原生依赖以及单一跨平台 `.fb`。平台标识以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准，CLI、构建编排和包格式分别以 [feng-cli.md](../docs/feng-cli.md)、[feng-build.md](../docs/feng-build.md) 和 [feng-package.md](../docs/feng-package.md) 为主规范。

最终能力矩阵：

- `macos-arm64` host：支持 native 编译 / 运行 macOS 程序，交叉编译 `linux-x64` / `linux-arm64` 程序，并为三个平台生成 `target=lib` 静态库。
- `linux-x64` host：支持 native 编译 / 运行 `linux-x64` 程序，交叉编译 `linux-arm64` 程序，并为三个平台生成 `target=lib` 静态库。
- `linux-arm64` host：支持 native 编译 / 运行 `linux-arm64` 程序，交叉编译 `linux-x64` 程序，并为三个平台生成 `target=lib` 静态库。
- Linux host 不使用 Feng 提供的 Apple SDK，也不承诺交叉链接 macOS 可执行程序；macOS `target=lib` 默认只生成 Mach-O 对象与静态归档，确需 SDK 头文件时由用户自行通过通用 `--sysroot=<path>` 显式提供。

### 9.1 标准库第三方 C 依赖的三平台原生预构建

本阶段只交付 PCRE2、libsodium、libunistring 与 libuv 的三平台预构件。这四个 `libfeng_std_*` 是 std 的第三方 C 依赖，与 `libfeng_runtime`、`libfeng_unwind` 及 Feng 自身构建链无关。

任务：

- [ ] `scripts/build_pcre2.sh`、`scripts/build_libsodium.sh`、`scripts/build_libunistring.sh`、`scripts/build_libuv.sh` 继续作为独立维护脚本，在对应 native host 生成 `std/extlib/<host-platform>/` 下的静态库。
- [ ] 三个平台分别原生生成 `libfeng_std_pcre2.a`、`libfeng_std_sodium.a`、`libfeng_std_unistring.a`、`libfeng_std_uv.a`；macOS 构件只在合法 macOS 构建环境使用 Apple SDK，Linux 构件分别在对应 Linux host 生成。
- [ ] 维护流程汇聚三平台 std extlib；不得将 host 静态库改名为其他平台产物，每份静态库必须校验成员对象格式与 CPU 架构。
- [ ] 补充原生脚本的 host 识别、输出隔离、缓存污染拒绝和平台不匹配诊断回归，不改变 runtime 或 Feng 自身构建行为。

独立交付与回归门：

- [ ] 在三个 native host 上分别使用对应四个静态库完成标准库专项回归，并记录第三方来源、版本和许可证。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.2 三平台 native driver

本阶段交付三个 host 上 Feng CLI 编译用户程序的 native 能力；非 native Linux 目标在 §9.3 前必须明确报告目标不可用。

任务：

- [ ] 实现 Feng CLI `--platform`、host 默认值、平台参数校验、Clang 查找顺序和 native target triple 转换。
- [ ] macOS native 最终链接使用 `xcrun` 获取 SDK 并传入 `-isysroot`；Linux native 使用 host glibc，不传 musl sysroot。
- [ ] native `target=bin` 只定位并链接 `lib/<目标平台>/libfeng_runtime.a`，不得使用其他平台 runtime。
- [ ] 补充 `--platform` 诊断、native Clang / SDK / runtime 定位和三平台 native 编译回归。

独立交付与回归门：

- [ ] 三个 host 分别使用 §8 发行包完成 native `feng build` / `run` / `lsp` 验收，并验证 bundled toolchain、显式 `CC` 覆盖和回退路径。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.3 Linux 全矩阵交叉编译

本阶段只交付 Feng 用户程序从 macOS 到两个 Linux 目标及两个 Linux host 之间的交叉编译，不交叉构建 Feng 自身、runtime、unwind 或 std 第三方 C 库。

任务：

- [ ] 在任一 host 交叉编译 Linux 时传入对应 `--target`、`--sysroot`、`--gcc-toolchain` 与 `-fuse-ld=lld`，不得误用 macOS SDK 或 host linker。
- [ ] 交叉目标只消费 §8 发行包中已经由对应 native CI 生成的 `lib/<目标平台>/libfeng_runtime.a`，以及 §9.1 已生成的目标 std extlib；不得在用户程序构建期间重建或改名这些基础制品。
- [ ] 不同目标的 `gen/`、`mod/`、`assets/`、对象、IR、库和可执行产物完全隔离，目标选择不得依赖编译 Feng 自身时的 host 宏或 `sizeof`。
- [ ] 补充 sysroot、compiler runtime、目标 Feng runtime、目标 std extlib 与 LLD 缺失诊断，以及 `target=bin` / `target=lib` 交叉构建回归。

独立交付与回归门：

- [ ] 分别验证 `macos-arm64 → linux-x64`、`macos-arm64 → linux-arm64`、`linux-x64 → linux-arm64`、`linux-arm64 → linux-x64`；检查目标格式、CPU 架构、ELF interpreter 与链接依赖，并在对应架构 Linux musl 环境实际运行目标程序。
- [ ] 每条路径都必须消费对应平台 runtime 与 std extlib 完成标准库集成回归。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.4 SDK-free macOS `target=lib`

本阶段交付任意支持 host 生成 `macos-arm64` Mach-O 对象和静态归档的能力，不在 Linux host 上执行 macOS 最终链接，也不由 Feng 提供 Apple SDK。

任务：

- [ ] 将生成 C 与 `feng_runtime.h` 在 `target=lib` 编译阶段需要的 C 类型、函数声明和 unwind 声明收敛为 Feng / LLVM 合法分发的自包含编译头闭包；默认路径不得读取目标 macOS SDK 的 `math.h`、`stdlib.h`、`string.h` 等系统头。
- [ ] `target=lib --platform=macos-arm64` 使用 bundled Clang 的明确 Darwin target triple，只执行 `-c`，再使用 bundled `llvm-ar` / `llvm-ranlib` 生成 Mach-O 静态库；不得调用 host `ar` 或进入最终链接。
- [ ] 核心直编与项目级单平台 `feng build` 支持通用 `--sysroot=<path>`。该路径只作用于本次唯一目标平台；项目级同时指定多个 `--platform` 时不得同时使用单一 `--sysroot`，需要不同 sysroot 的平台必须分多次 `feng build`，再由 `feng pack` 汇聚。
- [ ] macOS 目标显式 `--sysroot` 转换为 Clang `-isysroot`；Feng 只校验路径与构建所需内容，不下载、复制或判断第三方 SDK 的授权来源，用户负责其输入的许可合规。
- [ ] `target=lib` 不链接 `libfeng_runtime.a` 或 std extlib；macOS runtime 与四个 std extlib 继续使用 §8 / §9.1 在 macOS CI 原生预构建的制品。

独立交付与回归门：

- [ ] 在 `linux-x64` 与 `linux-arm64` host 不提供 Apple SDK，分别生成合法的 `macos-arm64` Mach-O 静态库，并校验 archive index、成员格式、CPU 架构和符号命名。
- [ ] 将两种 Linux host 生成的静态库带到 macOS，与 native runtime / std extlib 及官方 macOS SDK完成最终链接和运行回归。
- [ ] 使用用户显式 `--sysroot` 的正向、缺失路径、目标不匹配与多平台歧义诊断回归通过。
- [ ] 三个 host 的全量 `make test` 通过。

### 9.5 多平台 `.fb` 与 std 组包

本阶段在 §9.1—§9.4 之上交付项目级分平台构建、可搬运构件汇聚和单一跨平台 `.fb`；std 作为包含四个分平台 extlib 的完整验收包。

任务：

- [ ] 核心编译器直编继续一次只接受一个 `--platform` 和调用方给定的精确 `--out`；项目级 `feng build` 可以按目标平台分别调用核心编译器，也允许不同 host / CI 任务分别生成可搬运的 `<out>/<平台>/` 构件。
- [ ] `feng build` 将 `gen/`、`mod/`、`assets/`、`bin/`、`lib/`、`obj/`、`ir/` 与 `extlib/` 全部隔离到 `<项目输出根>/<目标平台>/`；相同目标递归传递给本地 `target=lib` 依赖。
- [ ] `feng pack` 只读取已经存在的各平台构件，不重新编译，也不接受 `--sysroot`；它校验各平台 `mod/` 的模块集合与公开语义事实等价、普通 `assets/` 内容一致后分别提取一套。
- [ ] `feng pack` 从每个 `<项目输出根>/<目标平台>/lib/` 与 `extlib/` 提取分平台制品，写入包内 `lib/<目标平台>/` 与 `extlib/<目标平台>/`，将实际平台集合写入 `feng.fm.arch`；任一请求平台缺失或校验失败时不得生成部分 `.fb`。
- [ ] std 构建对三个目标分别生成 `libstd.a`，并从 §9.1 的预构件中选择对应平台的四个 `libfeng_std_*` 写入 `extlib/<目标平台>/`；Linux 构建 macOS std 分片时只编译 Feng 生成 C，不重建或链接 macOS extlib。
- [ ] 补充单平台默认值、重复 `--platform`、分次构建后汇聚、分平台产物隔离、递归本地依赖、远程包目标制品缺失、公开符号表不一致、`arch` 精确匹配和原子组包回归。

独立交付与回归门：

- [ ] 在单个 Linux host 不使用 Apple SDK，分别构建 `macos-arm64`、`linux-x64`、`linux-arm64` 三个平台的 std 静态分片并组装一个三平台 `std.fb`。
- [ ] 在多 host CI 中分别构建平台目录，再由任一 host 的 `feng pack` 汇聚为相同结构的三平台 `.fb`。
- [ ] 三个平台分别将该 `.fb` 作为依赖完成编译、最终链接与运行；macOS 最终链接和运行只在合法 macOS 环境执行。
- [ ] 三个 host 的全量 `make test` 通过。

## 10 Linux musl 交叉链接已定方案

本节记录 Linux musl 交叉链接的分发决策；Feng 平台到 Clang 参数的转换以 [feng-build.md](../docs/feng-build.md) 为主规范。

1. **linker**：各 host 分发包使用自身 `toolchain/llvm/bin/lld`，并保留 `bin/ld.lld -> lld`。该可执行文件来自与 `clang` 相同的 LLVM 官方包，因此可在当前 host 上运行并链接 Linux ELF。driver 传入 `-fuse-ld=lld`，由 Clang 基于自身安装目录定位 `bin/ld.lld`，不传 `--ld-path`，也不使用 musl.cc 预构建包中的 `ld`。
2. **目标 C 环境与 compiler runtime**：`toolchain/sysroot/<linux-target>/` 使用与目标架构匹配的 musl.cc 配套包，同时保留 musl 头文件、库、动态加载器、musl CRT，以及目标 `crtbegin*` / `crtend*`、`libgcc.a`、`libgcc_eh.a`、`libgcc_s.so*`。仅排除不会在目标程序中被链接且不作为当前 host 工具使用的 GCC / binutils 可执行文件。
3. **Feng runtime**：交叉目标必须使用 `lib/<目标平台>/libfeng_runtime.a`，不得误用 host runtime。该路径属于本文 §4 已定义的分平台 runtime 布局，不在 sysroot 中重复分发。
4. **可用性边界**：`lld`、目标 sysroot / compiler runtime 与目标 `libfeng_runtime.a` 必须同时存在并通过链接验收，才能宣称该 Linux 交叉目标可用。
