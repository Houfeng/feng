# Feng 发行包随附 std 的 CI 实施方案

> 状态：实施完成，等待人工代码 Review。

## 1. 目标

在现有 GitHub Actions 发行流程中增加一份独立构建的多平台 `std` `.fb`，并通过
现有发行组装入口将其原样放入三个 host 发行包的顶层 `pkg/`。

本方案只决定本发行阶段的随附包集合包含 `std`。`.fb` 格式、项目平台选择、发行包
目录结构及 `pkg/` 的安装来源语义分别以以下主规范为准，本文不重复定义：

- [Feng 包格式](../specifications/feng-package.md)
- [Feng CLI](../specifications/feng-cli.md)
- [Feng 构建流程](../specifications/feng-build.md)
- [Feng 依赖管理机制](../specifications/feng-deps.md)
- [Feng 分发与安装方案](./feng-release-and-install.md)
- [发行包随附包安装方案](./feng-bundled-package-install-dev.md)

## 2. 范围

### 2.1 本次范围

- 新增一个独立的随附包构建脚本，负责准备构建用 Feng、构建 `std` 并发布确定的
  `.fb` 输出目录。
- GitHub Actions 新增一个独立的随附包 Job。
- 发行组装 Job 下载随附包 artifact，并通过现有
  `scripts/release_assemble.sh --packages=<目录>` 接入组装。
- 三个 host 发行包使用同一份 `std` `.fb` 输入。
- 增加随附包构建脚本与 CI 接线所需的专项测试。

### 2.2 不在本次范围

- 不修改 `feng install`、`feng deps install` 或构建阶段的包查找与消费逻辑。
- 不修改 `.fb` 格式、cache、registry、精确版本或本地依赖语义。
- 不在编译器、依赖管理器或发行组装器中增加 `std` 特判。
- 不改变现有三个原生编译器 Job 的构建、测试、component 内容和 artifact。
- 不引入 `feng.lock`、指纹、内容寻址、包目录索引或平台包合并协议。
- 不在 CI 中重新构建或维护 `std/std/extlib/`；继续使用仓库中已准备好的平台制品。
- 不在本次决定未来除 `std` 以外还要随附哪些包。

## 3. 设计原则

### 3.1 核心逻辑不写入 workflow

新增脚本：

```text
scripts/release_bundled_packages.sh
```

该脚本是随附包生产流程的唯一核心入口。它负责构建工具准备、隔离构建、输出校验和
原子发布。GitHub Actions 不展开 `std` 的打包步骤，不解析 `feng.fm`，也不自行复制
或改写 `.fb`。

workflow 中的构建步骤只调用：

```bash
scripts/release_bundled_packages.sh \
  --output=build/release-bundled-packages
```

CI 仍负责 runner、系统依赖、checkout、版本配置以及 artifact 上传下载；这些属于
GitHub Actions 编排，不进入构建脚本。

### 3.2 随附包生产与原生 component 解耦

现有三个原生 Job 继续只生产：

```text
release-component-macos-arm64
release-component-linux-x64-gnu
release-component-linux-arm64-gnu
```

随附包由单独 Job 生产：

```text
release-bundled-packages
```

不把 `std` 塞入任一原生 component，也不让三个原生 Job 分别生成同名 `.fb`。这样
可以避免单平台包冲突，并保证三个最终发行包接收字节完全相同的随附包输入。

### 3.3 一次构建一个多平台 std 包

随附包脚本在 Linux x64 环境使用 `build/bin/feng pack std` 的项目级多平台能力。
调用不传 `--platform`，目标平台集合由 `std/std/feng.fm` 按主规范选择。

当前 `std/std/feng.fm` 声明全部发行目标平台；Linux host 构建 `macos-arm64`
`target=lib` 时沿用现有 SDK-free Mach-O 对象与静态归档路径。因此无需分别生成
平台包，也不需要在汇聚阶段合并 `.fb`。

`std` 包坐标来自 `std/std/feng.fm`。脚本和 workflow 都不硬编码
`std-0.1.0.fb`，也不在本次增加“std 版本必须等于编译器发行版本”的新规则。

## 4. Job 拓扑

目标拓扑如下：

```text
prepare
├── component_macos
├── component_linux
│   ├── linux-x64-gnu
│   └── linux-arm64-gnu
└── bundled_packages
    └── release-bundled-packages

component_macos ─────────────┐
component_linux ─────────────┼── assemble ── verify ── publish
bundled_packages ────────────┘
```

### 4.1 `bundled_packages` Job

该 Job：

1. `needs: prepare`。
2. 使用 Linux x64 runner 和与现有 Linux 发行构建兼容的 Ubuntu 容器。
3. 安装构建脚本所需的最小系统命令。
4. checkout 仓库并启用 Git LFS，取得 LLVM、sysroot 和 `std/std/extlib/`。
5. 使用现有 `scripts/release_version.sh set` 配置本次构建版本。
6. 调用一次 `scripts/release_bundled_packages.sh`。
7. 将脚本输出目录作为 `release-bundled-packages` artifact 上传，关闭二次压缩。

该 Job 与三个原生 Job 并行，不下载或消费原生 component。脚本内部只构建本次打包
需要的 CLI，不重复执行 `make test`。

Job 使用与 `assemble` 相同的发行条件：仅在版本发行或手动试发时运行。普通
push / pull request 继续沿用现有三个原生 Job 的全量回归路径，不增加新的必跑 Job，
从而保持当前非发行 CI 的行为和耗时边界。

### 4.2 `assemble` Job

`assemble` 在现有依赖之外增加 `bundled_packages`，并新增一次 artifact 下载：

```text
release-bundled-packages
  -> build/release-bundled-packages/
```

现有组装命令只增加一个参数：

```bash
--packages=build/release-bundled-packages
```

其他参数、三份原生 component 下载、校验、发行 zip 命名和上传逻辑保持不变。

### 4.3 后续 Job

`verify_macos`、`verify_linux` 和 `publish` 的依赖关系、artifact 名称及调用方式不
改变。它们继续消费 `assemble` 生成的同一个 `release-packages` artifact。

## 5. 随附包构建脚本

### 5.1 接口

首版接口：

```text
scripts/release_bundled_packages.sh \
  --output=<bundled-package-directory>
```

约束：

- `--output` 必填且只能指定一次。
- 输出目录用于直接传给 `release_assemble.sh --packages`。
- 输出目录只允许包含顶层 `.fb` 文件。
- 若最终输出目录已存在，脚本拒绝覆盖，避免混入旧包或破坏调用方数据。
- 脚本失败时不得留下最终输出目录或部分 `.fb`。

脚本可以使用仓库相对路径定位 `Makefile`、`std/` 和 `build/bin/feng`，但不能依赖
调用者的当前工作目录。

### 5.2 构建工具准备

脚本在仓库根目录调用：

```bash
make cli
```

该步骤只准备本次 `feng pack` 所需的 Feng CLI、host LLVM 布局、公共头文件和当前
Linux host runtime，不承担全量回归职责。全量回归仍由现有三个原生 Job 中的
`make test` 完成。

脚本必须使用刚生成的仓库内 `build/bin/feng`，不能从 `PATH` 选择另一版本 Feng。

### 5.3 隔离构建

脚本在仓库 `build/` 下创建本次调用专用的临时工作目录，将构建 `std` 所需的项目
清单、源码和 `extlib` 输入复制到隔离项目根，再对该项目执行 `feng pack`。

这样做的目的：

- 不读取或发布开发者现有的 `std/std/build/pkg/`。
- 不把被 `.gitignore` 忽略的旧 `.fb` 混入发行 artifact。
- 不删除、覆盖或依赖调用前已有的 `std/std/build/`。
- 打包失败时可以整体清理本次临时目录。

临时目录必须位于工程 `build/` 内，不在 `/tmp` 或 `/private/tmp` 中生成并执行编译
产物。

### 5.4 包集合

首版随附项目集合在脚本内部集中声明，当前只有：

```text
std/
```

workflow 不传 `--project=std`，避免把发行包集合策略分散到 CI 配置。未来若经人工
决策增加其他随附包，只扩展脚本管理的项目集合和对应规范，不改变 workflow Job
拓扑或组装接口。

这只是发行内容选择，不改变安装器、构建器和组装器面向任意精确版本包的通用行为。

### 5.5 输出校验与发布

脚本只接受本次隔离构建实际产生的包，不扫描仓库其他输出目录。发布前至少校验：

1. 每个声明的随附项目恰好产生一个 `.fb`。
2. `.fb` 可以作为 ZIP archive 完整读取。
3. 包内恰好存在一个顶层 `feng.fm`。
4. 包内 `name`、`version` 与输出文件名坐标一致。
5. 最终输出目录中没有非 `.fb` 文件、子目录或额外包。

平台构建、公开 `.ft` 一致性、各平台 `lib/` / `extlib/` 完整性继续由
`feng pack` 按主规范负责，不在脚本中复制一套并行实现。

脚本先在输出目录同级建立 staging，全部包通过校验后再发布为最终输出目录。任何
准备、构建、校验或发布步骤失败，都清理 staging 并返回非零状态。

`release_assemble.sh` 仍按现有通用规则再次校验收到的 `.fb` 并原样复制。这属于
生产者和汇聚点各自的边界校验，不引入 `std` 特判。

## 6. 对现有 CI 正确性的保护

### 6.1 保持不变的部分

- 不修改 `component_macos` 和 `component_linux` 的现有步骤。
- 不修改原生 component 的文件布局与 `SHA256SUMS`。
- 不让随附包 Job 参与或替代三个平台的 `make test`。
- 不修改 `release-packages` artifact 名称。
- 不修改 verify 与 publish Job 的输入和发布协议。
- 不改变普通 push / pull request 的现有 Job 集合。
- 不修改省略 `--packages` 时生成空 `pkg/` 的组装能力。

### 6.2 新增失败边界

- 随附包 Job 失败时，`assemble` 不运行。
- 任一原生 Job 失败时，`assemble` 仍按现有规则不运行。
- 随附包 artifact 缺失、包含额外文件或包非法时，组装失败且不产生部分发行 zip。
- 三个最终发行包只能由同一份随附包 artifact 组装，不允许按 host 替换输入。

## 7. 测试方案

遵循“新增测试、不修改已有测试用例”的约束。

### 7.1 随附包构建脚本专项测试

新增独立测试脚本，覆盖：

- 从干净的独立 staging 构建 `std` 成功。
- 最终目录只包含一个与 `std/std/feng.fm` 坐标一致的 `.fb`。
- 包内平台集合及平台制品由真实 `feng pack` 成功路径生成。
- 调用前已有 `std/std/build/pkg/` 时，输出不读取其中旧包。
- 最终输出目录已存在时拒绝覆盖。
- 构建或校验失败时不留下最终输出目录和部分包。

测试使用工程 `build/` 下的工作目录，不在系统临时目录执行产物。

### 7.2 发行组装专项测试

沿用现有通用随附包组装测试验证：

- `--packages` 输入被原样复制到三个 host 发行包。
- 三个 zip 中对应 `.fb` 字节一致。
- 非法坐标、非法 archive 或额外目录被拒绝。
- 省略 `--packages` 时仍产生空 `pkg/`，保证旧调用方式继续有效。

如现有覆盖已经满足上述项目，不重复新增同义测试。

### 7.3 CI 配置检查

- 检查 workflow 语法。
- 检查 `bundled_packages` 与三个原生 Job 并行，仅依赖 `prepare`。
- 检查 `assemble` 同时依赖三类原生 component 和随附包 Job。
- 检查普通 push / pull request 路径不新增随附包 Job。
- 检查版本发行和手动试发路径能够上传、下载并传入随附包目录。

### 7.4 回归

实施完成后在沙箱外执行全量回归：

```bash
make test
```

## 8. 实施顺序

- [x] TODO 0：人工 Review 并确认本方案。
- [x] TODO 1：更新发行主方案，只引用本阶段确定的随附包集合与脚本入口，不重复包
  格式和安装规则。
- [x] TODO 2：新增 `scripts/release_bundled_packages.sh`，完成隔离构建、校验和原子
  发布。
- [x] TODO 3：新增独立脚本专项测试并接入现有测试入口。
- [x] TODO 4：在 GitHub Actions 中增加 `bundled_packages` Job。
- [x] TODO 5：让 `assemble` 下载随附包 artifact，并向现有组装脚本传入
  `--packages`。
- [x] TODO 6：执行脚本专项测试、workflow 检查和沙箱外全量 `make test`。
- [x] TODO 7：更新本文状态和 TODO，等待人工代码 Review。

## 9. 验收标准

- GitHub Actions 中存在一个独立、单次执行的随附包 Job。
- workflow 的 std 构建步骤只调用独立脚本，不展开核心构建与校验逻辑。
- 随附包脚本在隔离目录中使用当前源码构建的 Feng 生成一个多平台 std `.fb`。
- `std` 包坐标来自 `std/std/feng.fm`，workflow 与脚本不硬编码具体版本文件名。
- 三个 host 发行 zip 的 `pkg/` 中包含字节一致的同一份 std `.fb`。
- 现有三个原生 Job、component 结构、普通 CI 路径、verify 和 publish 协议保持正确。
- 任一原生 component 或随附包失败时均不会发布部分发行包。
- 专项测试与沙箱外全量 `make test` 全部通过。
