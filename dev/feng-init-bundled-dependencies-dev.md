# Feng init 自动声明随附包依赖开发方案

> 状态：等待人工 Review，尚未实施。

## 1. 目标

`feng init` 初始化项目时，从当前 Feng 发行安装根的 `pkg/` 目录读取随附包坐标，
并将全部随附包以精确版本依赖写入新项目的 `feng.fm`。

例如，发行包中存在：

```text
<feng-install-root>/pkg/std-0.1.0.fb
```

且该包内 `feng.fm` 声明：

```text
[package]
name:    "std"
version: "0.1.0"
```

则新项目的 `feng.fm` 包含：

```text
[dependencies]
std: "0.1.0"
```

该行为面向 `pkg/` 中的全部随附包，不针对 `std` 或其他具体包名增加特判。

正式的项目清单、依赖管理、随附包来源和发行目录语义仍分别以下列主规范为准，本文
只收敛本次开发方案，不重复定义已有规则：

- [Feng CLI](../docs/feng-cli.md)
- [Feng 包格式](../docs/feng-package.md)
- [Feng 依赖管理机制](../docs/feng-deps.md)
- [Feng 分发与安装方案](./feng-release-and-instanll.md)

## 2. 已确认规则

### 2.1 查找路径

`feng init` 必须根据当前正在运行的 Feng 可执行文件定位：

```text
<feng-executable-directory>/../pkg/
```

也就是：

```text
<feng-install-root>/pkg/
```

实现必须复用现有的 Feng 可执行文件绝对路径和安装根相对定位能力，不根据当前工作
目录查找，不从 `PATH` 中另选一份 Feng，也不新增 `FENG_HOME` 等环境变量。

该规则同时适用于正式发行布局和仓库构建布局：

```text
<release-root>/bin/feng  -> <release-root>/pkg/
<repo-root>/build/bin/feng -> <repo-root>/build/pkg/
```

### 2.2 目录不存在

若 `<feng-install-root>/pkg/` 不存在，`feng init` 保持当前行为：

- 正常创建项目；
- 不写入 `[dependencies]`；
- 不因缺少随附包目录报错。

若目录存在但顶层没有 `.fb` 文件，同样不写入空的 `[dependencies]`。

### 2.3 随附包集合

若 `pkg/` 存在，`feng init` 只枚举该目录顶层扩展名为 `.fb` 的文件，不递归扫描
子目录，也不处理其他扩展名的文件。

每个可读取坐标的顶层 `.fb` 都成为新项目的直接精确版本依赖。该规则对
`--target=bin`、`--target=lib` 以及默认 `bin` 一致。

输出依赖按包名排序；包名相同时按版本排序，使不同文件系统上的初始化结果保持
确定。

### 2.4 坐标来源

`feng init` 不从 `<name>-<version>.fb` 文件名反向拆分包名和版本，而是从 `.fb`
根目录内的 `feng.fm` 读取：

```text
[package].name
[package].version
```

选择包内 manifest 的原因是文件名不能在现有规则下无歧义地反向拆分。例如：

```text
foo-bar-1.0.0-rc.1.fb
```

包名和精确版本当前都没有一套足以支持可靠反向拆分的强制语法；从文件名猜测会隐式
引入新的包名或版本限制。读取包内权威坐标可以直接支持 `-rc`、`-beta` 及未来其他
精确版本形式。

## 3. init 与 build 的职责边界

### 3.1 init 只读取生成配置所需元数据

`feng init` 对每个 `.fb` 只执行生成 `[dependencies]` 所必需的操作：

1. 打开 ZIP；
2. 读取根目录 `feng.fm`；
3. 取得 `[package].name` 和 `[package].version`；
4. 将坐标写入新项目 manifest。

`init` 不执行以下完整包校验：

- 不比较文件名与包内坐标；
- 不校验包的平台集合或 ABI；
- 不校验 `.ft`、静态库、动态库、资源等包内容；
- 不展开或安装包内传递依赖；
- 不检查 registry 或全局 cache；
- 不把 `.fb` 复制到全局 cache。

因此，本次不是把 `feng deps install` 的包校验和安装流程前移到 `init`。

### 3.2 无法取得坐标

读取坐标不是可省略的完整包校验，而是生成依赖配置的必要输入。出现以下情况时，
`init` 无法把对应文件声明为依赖：

- `.fb` 无法打开；
- 根目录不存在 `feng.fm`；
- 无法读取 `[package].name` 或 `[package].version`。

此时 `feng init` 必须报错并沿用现有初始化失败清理语义，不得静默跳过该文件。
静默跳过会使该包不进入依赖图，后续 `feng build` 也就没有机会报告问题。

若多个 `.fb` 读取出同一包名，`feng.fm` 的单键依赖模型无法同时表达它们。此时
同样报错，不因版本相同而去重，也不在版本不同时静默选择版本。正常发行的随附包
集合应保证同一包名只出现一次。

### 3.3 build/install 执行完整校验

初始化后，现有命令链保持不变：

```text
feng build
  -> feng deps install
  -> 按 name + version 查找 <name>-<version>.fb
  -> registry / pkg 选择
  -> 校验包内 manifest 与请求坐标
  -> 安装到全局 cache
  -> 构建时消费 cache 中确定的 .fb 路径
```

因此，文件名与包内坐标不一致、包平台或 ABI 非法、依赖冲突以及包内容非法，继续由
现有 build/install 流程报告。核心编译器仍只接收上层确定的 `--pkg <path>`，
不扫描 `pkg/`，也不按包名或版本自行查找文件。

## 4. 性能

读取包内 manifest 不需要解压完整 `.fb`。现有 ZIP reader 可以：

1. 打开归档并读取 ZIP 元数据；
2. 按固定路径定位根目录 `feng.fm`；
3. 只解压这一份很小的文本条目。

时间复杂度与 `pkg/` 顶层 `.fb` 数量线性相关，不随包内静态库、模块和资源的未压缩
总大小线性解压。`feng init` 是一次性操作，正式发行随附包集合数量有限，因此不为
该读取增加索引、catalog、持久缓存或新的发行元数据文件。

## 5. 实现方案

### 5.1 主规范

实施代码前，只在 [Feng CLI](../docs/feng-cli.md) 的 `feng init` 章节定义本行为：

- 安装根 `pkg/` 的定位方式；
- 目录不存在时不生成依赖；
- 从随附 `.fb` 的 `feng.fm` 读取坐标；
- 将全部随附包写为直接精确版本依赖；
- 完整校验和安装仍由 build/install 执行。

[Feng 包格式](../docs/feng-package.md) 和
[Feng 依赖管理机制](../docs/feng-deps.md) 已分别定义 manifest 与 `pkg/` 安装
语义，本次只按需引用 `feng init` 规则，不重复维护同一规范。

### 5.2 复用现有读取能力

当前已有两层可直接复用的公共能力：

- `FengZipReader` 可以打开 `.fb`，并按固定路径只读取根目录 `feng.fm`；
- `feng_fm_parse` 可以解析 manifest 的节和字段。

依赖管理器内部的 `read_bundle_manifest` 也是基于上述能力实现，但它是私有函数，
并会继续调用完整 bundle manifest 解析器，校验平台、ABI 和其他分发字段。该完整
校验不属于 `init` 职责，因此本次不把该私有函数直接公开给 `init`，也不为读取
manifest 源再增加一层公共封装。

`feng init` 直接组合现有 `FengZipReader` 与 `feng_fm_parse`，只从 `[package]`
取得 `name` 和 `version`。实现必须完整清理 ZIP reader、读取缓冲区、FM document
和错误信息；不得通过启动 `unzip` 等外部命令读取 manifest。

### 5.3 init 依赖收集

在 `feng init` 项目文件写入前增加通用随附依赖收集步骤：

1. 调用现有安装根定位函数得到 `pkg/`；
2. 区分目录不存在、目录存在和读取错误；
3. 枚举顶层 `.fb`；
4. 读取每个包的 manifest 源和包坐标；
5. 排序并检查同名随附包不可重复表达的问题；
6. 生成可选的 `[dependencies]` 文本；
7. 与现有 `[package]` 文本一起创建 `feng.fm`。

依赖收集逻辑面向任意随附包，不感知 `std`。现有包名归一化、项目版本、目标类型、
平台默认值和 starter 源文件内容保持不变。

### 5.4 失败清理

`feng init` 现有的失败清理保证必须保留：

- 失败时不留下部分写入的 `feng.fm`；
- 失败时删除本次创建的 starter 源文件；
- 失败时只删除本次创建且仍为空的 `src/`；
- 不修改、移动或删除安装根 `pkg/` 中的任何文件。

## 6. 不在本次范围

- 不为 `std` 增加任何特判。
- 不修改 `.fb` 文件格式。
- 不修改 registry、全局 cache 或 `pkg/` 的来源优先级。
- 不修改 `feng deps install` 的递归安装行为。
- 不修改 build、check、run、pack 对依赖图和 cache 的现有消费方式。
- 不增加 `feng.lock`、指纹、包索引或 catalog。
- 不为包名或版本新增语法约束。
- 不根据文件名猜测 SemVer、预发布版本或包名边界。
- 不自动修改已经存在的项目。

## 7. 测试方案

只新增本需求测试，不修改已有测试用例。

测试使用工程 `temp/` 或 `build/` 下的隔离发行布局，复制测试用 Feng 到独立
`bin/`，并在同一安装根准备独立 `pkg/`，避免依赖开发者当前 `build/pkg/` 内容。

### 7.1 无随附包

- `pkg/` 不存在：初始化成功，`feng.fm` 不含 `[dependencies]`。
- `pkg/` 存在但为空：初始化成功，`feng.fm` 不含 `[dependencies]`。
- `pkg/` 只有非 `.fb` 文件：初始化成功且忽略这些文件。

### 7.2 生成依赖

- 一个 `.fb`：生成一个精确版本依赖。
- 多个 `.fb`：全部生成，顺序按坐标确定。
- `bin` 与 `lib` 项目生成相同随附依赖集合。
- 包版本包含 `-rc` 或 `-beta`：完整保留包内版本，不从文件名错误截断。
- 文件名与包内坐标不同：`init` 使用包内坐标，不提前执行一致性校验。
- 包内 manifest 只具备读取坐标所需元数据、但不满足完整 bundle 约束：
  `init` 仍可生成依赖，证明完整校验没有前移。

### 7.3 失败

- `.fb` 无法打开、缺少根 `feng.fm` 或缺少坐标：初始化失败且无残留项目文件。
- 同一包名由多个 `.fb` 重复提供：初始化失败，不自行去重或选择版本。
- `pkg/` 存在但无法读取：初始化失败并报告实际路径及底层原因。

### 7.4 回归

- 执行新增的 init 专项测试。
- 执行全量 `make test`。
- 按工程规则在 Codex 沙箱外执行全量回归。

## 8. 实施顺序

- [ ] TODO 0：等待本文人工 Review；Review 通过后再实施。
- [ ] TODO 1：更新 `docs/feng-cli.md` 中的 `feng init` 主规范。
- [ ] TODO 2：复用现有 `FengZipReader` 与 `feng_fm_parse` 读取随附包坐标。
- [ ] TODO 3：实现安装根 `pkg/` 枚举和包坐标收集。
- [ ] TODO 4：将确定排序的随附包依赖写入新项目 `feng.fm`。
- [ ] TODO 5：保留并验证初始化失败清理语义。
- [ ] TODO 6：新增隔离安装布局下的 init 专项测试。
- [ ] TODO 7：在 Codex 沙箱外执行全量 `make test`。
- [ ] TODO 8：整理实现与测试结果，等待人工代码 Review。

## 9. Review 重点

- `init` 只读取坐标、完整包校验继续留在 build/install 的职责划分是否符合预期。
- 无法取得坐标时失败，而不是静默跳过，是否符合预期。
- 同一包名由多个随附包重复提供时失败，而不是自行去重或选版本，是否符合预期。
- `pkg/` 不存在或没有 `.fb` 时不生成 `[dependencies]`，是否符合预期。
