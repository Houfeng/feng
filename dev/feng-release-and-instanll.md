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

- Windows 可执行文件使用 `.exe` 后缀，静态库使用 `.lib` 后缀。
- `lib/` 按完整目标平台分目录。三份分发包均包含五份 runtime；macOS runtime 在合法 macOS 环境构建，Linux runtime 使用对应 sysroot 构建。发布时校验对象格式、CPU 架构和平台，禁止跨平台或 libc ABI 复用。
- `include/` 仅存放一份平台无关的 runtime 公共 ABI 头文件。标准和系统头文件由目标 SDK / sysroot 提供，不得复制 Apple SDK 头文件。
- `toolchain/llvm/` 保持 LLVM 官方包布局，所有工具来自同一版本、同一 host 平台包。每份 Linux 分发包只包含当前 host 架构的一份 LLVM，同时支持 GNU 和 musl 目标。
- `toolchain/sysroot/` 按完整 Linux 目标平台分目录，保留编译和链接所需文件及目录关系，移除 GCC、binutils 和 musl.cc 工具。native 与交叉编译共用 sysroot，调用参数见 [feng-build.md](../docs/feng-build.md)。
- sysroot 的来源、版本、裁剪和许可信息见 §5.3。禁止复制 host `/usr`。
- 分发包不包含 Feng 源码、中间产物或构建缓存。
- `feng` 按自身位置查找 `../lib/`、`../include/` 和 `../toolchain/llvm/bin/`，不使用 `FENG_HOME` 或 `FENG_TOOLCHAIN`。查找顺序见 [feng-build.md](../docs/feng-build.md) 和 [feng-cli.md](../docs/feng-cli.md)。

## 5 toolchain 形态

分发包内 `toolchain/` 包含：

- LLVM
  - 来源：LLVM 官方 host 平台预编译包。
  - 版本：`22.1.8`。
  - 工具：`clang`。
  - 工具：`lld`、`ld.lld`。
  - 工具：`llvm-ar`、`llvm-ranlib`。
  - 工具：`lldb`、`lldb-dap`、`lldb-argdumper`、`debugserver` / `lldb-server`。
  - 脚本：`scripts/fetch_llvm.sh`、`scripts/trim_llvm.sh`。
  - Linux host 私有运行库（§5.2）
    - AlmaLinux 8.10：`libxml2 2.9.7`、`xz 5.2.4`、`zlib 1.2.11`、`libgcc 8.5.0`、`Python 3.11.9`。
    - Ubuntu 22.04：`ncurses 6.3`、`libstdc++ 12.3.0`。
    - host 下限：glibc `2.34`。
- sysroot
  - macOS（§5.1）：来自 Xcode 或 Xcode Command Line Tools，不随 Feng 分发，不固定版本。
  - GNU/Linux（§5.3）
    - 平台：`linux-x64-gnu`、`linux-arm64-gnu`。
    - 来源：Debian 11 Bullseye。
    - 版本：glibc `2.31`、Linux headers `5.10.13`、GCC runtime `10.2.1`。
    - 脚本：`scripts/fetch_gnu_sysroot.sh`、`scripts/trim_gnu_sysroot.sh`。
  - musl/Linux
    - 平台：`linux-x64-musl`、`linux-arm64-musl`。
    - 来源：musl.cc。
    - 版本：musl `1.2.2-git-50-gb76f37fd`、GCC runtime `11.2.1`。
    - 脚本：`scripts/fetch_musl_sysroot.sh`、`scripts/trim_musl_sysroot.sh`。

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

## 8 实施 TODO

本节交付三个 native host 的 Feng 编译器、五个平台的 runtime、调试工具、四份 Linux sysroot、安装包和交叉编译。标准库多平台制品及 `.fb` 多平台组包见 §9。各 native CI 任务构建当前 host 的 `feng`，汇聚任务将五份 runtime 加入每份发行包。

§8 与 §9 的每个子阶段依次执行：

1. 完成所列任务、专项验收和规定的测试。修改 Feng 编译器、runtime 或 host 行为时，须在 `macos-arm64`、`linux-x64-gnu`、`linux-arm64-gnu` 上通过 `make test`。§8.1 仅在 `macos-arm64` 上运行 `make test`，Linux host 只执行专项验收。
2. 提交变更和验收结果供人工 Review。
3. 人工 Review 通过并明确批准后，方可进入下一子阶段；§8 全部通过前不得开始 §9。

外部原生机器或发行版镜像不可用时，经人工确认，可并行实施不依赖相关兼容性验收的后续任务。未完成项须保持为 TODO，并在 §8.6 前通过。此规则不适用于脚本、产物、代码、测试或当前环境中的验收缺口。

### 8.1 可独立验证的精简工具链

本阶段只交付精简脚本和工具链产物，不改变 Feng CLI 的工具选择行为。

- [x] `scripts/fetch_llvm.sh` 下载并解压三个 host 的 LLVM 官方包到 `local/llvm/`。Linux 私有运行库由后续任务处理。
- [x] `scripts/trim_llvm.sh` 从同一 LLVM 包保留 `clang`、`lld` / `ld.lld`、`lldb`、`lldb-dap` 及包内依赖。Linux 额外依赖由后续任务处理。
- [x] `scripts/trim_llvm.sh` 保留同一 LLVM 包中的 `llvm-ar` 和 `llvm-ranlib`，供 §9 跨目标归档使用，不依赖 host `ar`。
- [x] 按 host 平台生成 `toolchain/llvm/macos-arm64/`、`toolchain/llvm/linux-x64-gnu/` 和 `toolchain/llvm/linux-arm64-gnu/`，并校验二进制平台。
- [x] `scripts/fetch_llvm.sh` 按 §5.2 下载并校验固定版本的 AlmaLinux 8.10 RPM、Ubuntu 22.04 ncurses 和 `libstdc++6` DEB，缓存到 `local/llvm/`。仓库地址、文件名、版本和 SHA-256 必须固定。
- [x] `scripts/trim_llvm.sh` 只提取 Linux LLVM 需要的共享库和 soname 链，设置相对 RPATH / RUNPATH，并检查全部 `DT_NEEDED` 依赖和 ncurses 版本化符号。失败时不留下半成品；除 glibc 外，不依赖系统运行库。
- [x] Linux LLVM 仅保留 `libpython3.11.so.1.0` 和 LLDB 初始化所需的 `encodings`，由内部启动器设置相对 `PYTHONHOME`。不包含 Python 可执行文件、其他标准库、LLDB Python bindings、第三方模块或脚本功能。
- [x] `scripts/fetch_musl_sysroot.sh` 从 musl.cc 下载并解压 x64 和 arm64 预构建包。
- [x] `scripts/trim_musl_sysroot.sh` 保留两种架构的 musl、CRT、libgcc 和所需目录，移除 GCC 和 binutils 可执行文件。
- [x] 将 musl sysroot 迁移到 `toolchain/sysroot/linux-x64-musl/` 和 `toolchain/sysroot/linux-arm64-musl/`，保留原有来源、许可及编译链接验证。
- [x] `scripts/fetch_gnu_sysroot.sh` 按 §5.3 从 Debian 固定 archive / snapshot 地址下载两个架构的 Bullseye cross packages 到 `local/sysroot-gnu/`，并校验文件名、版本和 SHA-256。不使用 host 包管理器。
- [x] `scripts/trim_gnu_sysroot.sh` 生成 `toolchain/sysroot/linux-x64-gnu/` 和 `toolchain/sysroot/linux-arm64-gnu/`。保留头文件、glibc、动态加载器、CRT、linker scripts、compiler runtime 及 Clang 所需目录，移除 host 可执行工具，失败时不留下半成品。
- [x] 两份 GNU sysroot 固定使用 glibc 2.31，并记录二进制包和源码包的地址、版本、SHA-256、裁剪清单、许可证及源码提供方式。不复制 host `/usr`。
- [x] 验证 `macos-arm64` LLVM 的工具启动、架构和动态依赖，并通过 `lldb`、`lldb-dap` 的真实基础调试。
- [ ] `linux-x64-gnu` LLVM 已在 Apple Container Ubuntu 26.04 x64 中通过工具启动、ELF 架构、CPU 要求、`GLIBC_*` / `GLIBCXX_*` 版本、RPATH / RUNPATH 和动态依赖检查。该仿真环境无法使用 LLDB 断点，仍须在原生 x64 Linux 上验证 `lldb` 和 `lldb-dap` 基础调试。
- [x] `linux-arm64-gnu` LLVM 已在 Apple Container Ubuntu 26.04 ARM64 中通过工具启动、架构、CPU 要求、依赖和真实 `lldb` / `lldb-dap` 基础调试。
- [ ] LLVM host 仅要求 glibc 2.34 或更低。x64 和 ARM64 产物已在 Apple Container Ubuntu 26.04 中直接启动，ARM64 调试已通过；仍须在 Ubuntu 22.04 / 24.04、Debian 12 / 13 和 AlmaLinux 9 的 x64 / ARM64 环境验证，不验证纯 musl Alpine host。
- [x] 使用精简 Clang / LLD 和两份 musl sysroot 链接最小 C ELF，验证 x64 / arm64 的 CRT、libgcc 和 musl，不依赖 Feng CLI。
- [x] 使用精简 Clang / LLD 和两份 GNU sysroot，在 native、Linux 跨架构和 macOS host 上编译并链接最小 C ELF，验证 glibc 2.31、动态加载器、CRT、linker scripts 和 compiler runtime，不依赖 Feng CLI。
- [x] 在 x64 / ARM64 GNU/glibc 环境运行最小 ELF，检查架构、解释器路径和最高 `GLIBC_*` 要求；同时检查 GNU sysroot 不含 host ELF 工具和失效链接，且许可与来源清单完整。
- [x] `macos-arm64` 已在 Codex 沙箱外通过 `make test`。

### 8.2 统一相对布局、CLI 路径基础设施与 host LLVM 定位

本阶段收敛发行包与源码开发的路径基础设施，并切换 driver 默认使用当前 host 的 bundled LLVM；同时完成 macOS native bundled Clang 必需的系统 SDK 定位。不引入显式目标平台、target triple、用户 `--sysroot` 或其他交叉编译逻辑，也不改变 Feng 自身现有 `build/` 产物层级。LLVM 可执行文件始终属于 host 工具，native 与后续交叉编译共用同一套定位结果，目标平台只影响后续传给 LLVM 的编译与链接参数。

Feng 编译器自身固定使用 `clang` 构建，不读取或接受其他 `CC` 值，也不支持 GCC。Linux host 在严格 C11 模式下统一启用 glibc 的 GNU feature namespace，以公开源码和测试实际使用的 GNU、XSI 与 POSIX.1-2008 接口。Feng 语义分析器直接调用 `fmod` 完成编译期浮点常量计算，因此仅为包含该语义分析器对象的 Linux host 工具与测试可执行文件链接 `libm`；这属于 Feng 编译器自身的构建依赖，不得用于替代 [feng-build.md](../docs/feng-build.md#25-收集链接信息) 规定的 Feng 用户程序 external 链接信息收集机制。

- [x] Makefile 在缺失时创建 `build/toolchain/llvm -> ../../toolchain/llvm/<host-platform>` 和 `build/toolchain/sysroot -> ../../toolchain/sysroot`。不检查已有链接；链接错误时执行 `make clean` 和 `make all` 重建。
- [x] 在 `src/cli/common.*` 统一实现 Feng 可执行文件、安装根、相对路径和 `PATH` 工具的查找与错误提示。runtime 和 host LLVM 共用该实现，`lldb-dap` 在 §8.4 接入。不增加工具链根目录环境变量。
- [x] 测试可执行文件查找、相对路径、软链接布局和缺失路径错误。
- [x] driver 按 [feng-build.md](../docs/feng-build.md) 的顺序选择 host 工具：`FENG_CC` / `FENG_AR` / `FENG_RANLIB`、bundled `clang` / `llvm-ar` / `llvm-ranlib`、`CC` / `AR` / `RANLIB`、系统 `cc` / `ar` / `ranlib`。native 和交叉编译共用该结果。
- [x] 测试上述工具选择顺序。bundled 工具缺失时继续查找；bundled 工具损坏、环境变量无效或全部工具不可用时明确报错。本阶段不加入 `--target`、`--sysroot` 或目标 runtime 测试。
- [x] macOS native 编译使用 `xcrun --sdk macosx --show-sdk-path` 定位 SDK，并传入一个 `-isysroot`。`xcrun` 或 SDK 不可用时直接报错，不增加显式 `--sysroot` 或跨目标选择。
- [x] Makefile 固定使用 `clang` 构建 Feng，不检查编译器类型，也不依赖 GCC。Linux host 启用源码和测试所需的 GNU、XSI 与 POSIX.1-2008 声明；仅为使用语义分析器的 host 程序链接 `libm`，不依赖 macOS `libSystem` 的默认行为。
- [x] 验证 `build/bin/feng` 可使用发行包相同的工具链布局：native 编译启动 bundled `clang`，macOS 使用系统 SDK，静态归档启动 bundled `llvm-ar`，需要索引时启动 bundled `llvm-ranlib`。runtime 和 DAP 行为不变。
- [x] `macos-arm64` 已在 Codex 沙箱外通过 `make test`。
- [ ] `linux-x64-gnu` 和 `linux-arm64-gnu` 已在 Apple Container Ubuntu 26.04 中通过路径、软链接及 bundled LLVM / sysroot 测试，仍须在两个原生 Linux host 上通过 `make test`。

### 8.3 libunwind 面向多平台预构建

本阶段只更新 `scripts/build_libunwind.sh`。

平台标识以 [feng-os-arch.md](../docs/feng-os-arch.md) 为准：

| 构建环境 | 预构建产物 |
|----------|------------|
| `macos-arm64` | `extlib/macos-arm64/libfeng_unwind.a` |
| `linux-x64-gnu` | `extlib/linux-x64-gnu/libfeng_unwind.a` |
| `linux-x64-musl` | `extlib/linux-x64-musl/libfeng_unwind.a` |
| `linux-arm64-gnu` | `extlib/linux-arm64-gnu/libfeng_unwind.a` |
| `linux-arm64-musl` | `extlib/linux-arm64-musl/libfeng_unwind.a` |

- [x] 更新 `scripts/build_libunwind.sh`。Linux 必须传入 `--libc=gnu|musl`；非 Linux 传入该参数时报错。
- [x] 在上表对应环境执行脚本，生成五份 libunwind，并校验平台、文件格式和 CPU 架构。

### 8.4 Feng 编译器及运行时，支持多平台分别构建

本阶段更新构建脚本，并确保 `feng dap` 在 macOS 与 Linux 正常运行。不修改 CI 或发行脚本。

| 构建环境 | runtime 构建产物 |
|----------|------------------|
| `macos-arm64` | `build/lib/macos-arm64/libfeng_runtime.a` |
| `linux-x64-gnu` | `build/lib/linux-x64-gnu/libfeng_runtime.a`<br>`build/lib/linux-x64-musl/libfeng_runtime.a` |
| `linux-arm64-gnu` | `build/lib/linux-arm64-gnu/libfeng_runtime.a`<br>`build/lib/linux-arm64-musl/libfeng_runtime.a` |

- [x] Feng 编译器：在三个构建环境构建并运行；失败时更新构建脚本。各平台均输出 `build/bin/feng`，并能正常执行 `feng --version`。
- [x] Feng runtime：在三个构建环境使用对应 SDK / sysroot 生成上表五份 runtime。每份只合并 `extlib/<platform>/libfeng_unwind.a` 中的同平台预构建库，不包含 libc，并通过平台、文件格式和 CPU 架构校验。
- [x] 各平台的 Feng 编译器编译 `.ff` 文件时，均能找到对应平台的 runtime 和 SDK / sysroot。
- [x] 三个发行平台的 `feng dap` 均按 [feng-cli.md](../docs/feng-cli.md) 定位并启动 `lldb-dap`，路径处理复用 §8.2，并通过定位失败、启动失败和真实调试测试。
- [x] 按 [feng-std-extlib-build.md](./feng-std-extlib-build.md) 更新标准库依赖的 extlib 预构建脚本，分别生成并校验三个发行 host 的预构建物；验收中发现的小型平台兼容问题在本阶段修复。
- [x] `macos-arm64`、`linux-x64-gnu`、`linux-arm64-gnu` 的全量 `make test` 均通过。

### 8.5 支持交叉编译

项目命令的参数行为以 [feng-cli.md](../docs/feng-cli.md#项目平台选择统一规则) 为准。

- [ ] 支持 `--platform=<platform>` 和 `--sysroot=<path>`，按目标平台选择对应的 runtime 和 SDK / sysroot，并完成交叉编译。
- [ ] `feng build` 与 `feng pack` 支持 `--sysroot=<path>`；传入时使用该 sysroot，不使用目标平台的默认 sysroot。
- [ ] 验证 `feng build`、`feng run`、`feng pack` 传入或省略 `--platform`、`--sysroot` 时均符合上述规则。
- [ ] 人工 Review。

### 8.6 CI 构建脚本

本阶段编写以下文件：

- [ ] `.github/workflows/release.yml`：在三个发行平台构建 Feng 编译器和对应 runtime，运行校验与 `make test`，上传构件；全部成功后调用 `scripts/release.sh` 并发布。
- [ ] `scripts/release.sh`：汇总三组构件，校验五份 runtime 和公共头文件，生成三个发行包。每个包包含对应平台的 Feng 编译器与 LLVM，以及五份 runtime、四份 Linux sysroot、公共头文件和 `VERSION`。
- [ ] `scripts/install.sh`：按 [feng-os-arch.md](../docs/feng-os-arch.md) 识别当前 host，下载对应发行包，完成安装和 `PATH` 配置；失败时不留半成品。
- [ ] 在三个干净发行平台安装并验证 `feng --version`、`build`、`run`、`lsp`、`dap`、bundled LLVM、目录结构、平台构件和相对定位。
- [ ] 人工 Review。
