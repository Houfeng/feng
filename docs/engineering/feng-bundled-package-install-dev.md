# Feng 随附包备用安装来源开发方案

> 状态：实施完成，等待人工 Review。本文只收敛将编译器发行包顶层 `pkg/` 目录接入
> `feng deps install` 的开发方案；正式依赖管理语义仍以
> [feng-deps.md](../specifications/feng-deps.md) 为主规范。实施前先更新主规范，再修改代码和测试。

## 1 目标

Feng 编译器发行包可以在顶层 `pkg/` 目录随附 `.fb` 包。对于 `feng.fm`
中以精确版本声明的依赖，`feng deps install` 在 registry 未配置或明确不包含
对应包时，以 `pkg/` 作为备用安装来源，将随附包安装到全局 cache。

`pkg/` 是面向所有精确版本包的通用备用来源，不针对 `std` 或其他具体包名增加
特判。项目继续使用现有标准依赖形式：

```text
[dependencies]
std: "0.1.0"
```

## 2 已确认边界

- 精确版本依赖继续使用 `<name>: "<version>"`，不增加新的 manifest 语法。
- 不增加 `feng.lock`。
- 不增加包指纹、内容寻址 cache 或 catalog。
- 不改变全局 cache 的现有目录和文件名规则。
- 不重构 install 与 build 的现有公共递归依赖解析流程。
- 不改变本地路径依赖的安装和构建语义。
- 不改变 registry URL、远程包路径或本地 registry 目录结构。
- 不改变编译器消费 `.fb`、`--pkg` 展平和链接行为。
- 不为 `std` 增加安装、解析、构建或编译特判。

## 3 发行目录

发行包顶层增加 `pkg/`：

```text
feng-<version>-<platform>/
├── bin/
│   └── feng
├── include/
├── lib/
├── pkg/
│   └── <name>-<version>.fb
├── toolchain/
└── VERSION
```

随附包路径固定为：

```text
<feng-install-root>/pkg/<name>-<version>.fb
```

`feng` 必须复用现有的可执行文件绝对路径与安装根相对定位能力查找 `pkg/`，不得增加
`FENG_HOME` 等新的安装根环境变量，也不得分别实现另一套可执行文件定位逻辑。

`pkg/` 中可以放置任意数量的精确版本 `.fb`。目录中的包集合由发行流程决定，
依赖管理器只按依赖坐标组成固定文件名并查找，不扫描目录，也不感知具体包名。

## 4 现有依赖行为

### 4.1 精确版本依赖

`feng deps install` 递归展开完整依赖图。对于每个精确版本依赖，当前流程检查全局
cache，必要时从 registry 安装，并继续读取已安装 `.fb` 内的 `feng.fm` 处理传递
依赖。

本次只在这个“确保精确版本包已安装到 cache”的既有流程中增加 `pkg/` 备用来源。

### 4.2 本地路径依赖

`feng deps install` 不直接忽略本地路径依赖，而是继续执行现有校验和递归处理：

- 本地 `.fb`：校验包名并读取包内 `feng.fm`，递归安装其中的精确版本依赖。
- 本地项目目录或 `feng.fm`：校验路径、包名和 `target: "lib"`，递归安装其清单中的
  精确版本依赖。
- install 阶段不构建本地项目，也不把本地依赖复制到全局 cache。
- `feng build`、`check`、`run` 和 `pack` 继续在需要消费本地项目时递归构建并使用
  生成的 `.fb`。

本次不修改上述行为。若本地依赖的传递依赖是精确版本，该传递依赖仍按本文的
registry / `pkg/` 规则安装。

## 5 精确版本安装规则

以下规则适用于根项目、远程 `.fb`、本地 `.fb` 和本地项目清单中出现的每一个
精确版本依赖。

### 5.1 cache 已安装

当 `~/.feng/cache/<name>-<version>.fb` 已存在、校验合法且未指定 `--force` 时，
沿用现有行为，直接复用 cache，不访问 registry 或 `pkg/`。

### 5.2 未配置 registry

当 cache 不可复用且当前项目和全局配置都未提供 registry 时：

1. 查找 `<feng-install-root>/pkg/<name>-<version>.fb`。
2. 若文件存在，按现有 `.fb` 合法性规则校验包结构、包名和精确版本。
3. 校验成功后，通过临时文件和原子替换将其复制到
   `~/.feng/cache/<name>-<version>.fb`。
4. 从 cache 中继续读取包清单并递归安装传递依赖。
5. 若 `pkg/` 中不存在对应文件，报 `<name>@<version>` 找不到，不再以“未配置
   registry”作为唯一失败原因。

### 5.3 已配置 registry

当 cache 不可复用且存在可用 registry 时：

1. 先按现有 registry 路径规则查找并安装 `<name>@<version>`。
2. registry 明确不存在对应包时，再查找
   `<feng-install-root>/pkg/<name>-<version>.fb`。
3. `pkg/` 中存在合法包时，将其原子复制到全局 cache。
4. registry 和 `pkg/` 都不存在对应包时，报 `<name>@<version>` 找不到。

registry 中存在对应文件但文件内容非法、包名不匹配或版本不匹配时，继续报告
registry 包非法，不使用 `pkg/` 覆盖该错误。registry 的其他既有读取、下载和诊断
行为本次不调整。

### 5.4 `--force`

`feng deps install --force` 继续忽略可复用的现有 cache，并重新执行来源安装：

- 已配置 registry：先尝试 registry；registry 明确不存在时回退 `pkg/`。
- 未配置 registry：直接尝试 `pkg/`。

成功后原子替换 cache 中的同坐标文件。registry 与 `pkg/` 均无对应包时安装失败。

## 6 build 关系

`feng deps install` 继续可以独立执行。`feng build`、`check`、`run` 和 `pack`
继续复用现有依赖准备流程：

1. 确保依赖图中的精确版本包已经安装到全局 cache。
2. 递归处理并按需构建本地项目依赖。
3. 从 cache 和本地依赖产物收集确定的 `.fb` 路径。
4. 展平后传给编译器的 `--pkg`。

`pkg/` 只参与第 1 步的安装来源选择。编译器前端、链接驱动以及第 3、4 步不得把
`pkg/` 增加为新的直接包扫描或消费来源。

当前 install 与 build 使用同一套依赖管理实现，并通过是否物化本地项目区分行为；
这是既有逻辑复用。本次不拆分该实现，只在确保精确版本包进入 cache 的既有入口中
增加通用 `pkg/` 回退。

## 7 合法性、写入与诊断

- registry 包、`pkg/` 随附包和 cache 包沿用同一套 `.fb` 基础合法性校验：文件可
  作为 `.fb` 打开、内置 `feng.fm` 可解析、包名和版本与请求坐标完全一致。
- `pkg/` 来源必须先校验，再发布到 cache；失败时不得留下新的半成品 cache。
- 写入 cache 继续采用同目录临时文件和原子替换，避免 install 失败后留下部分文件。
- `pkg/` 文件存在但非法时，诊断必须包含请求坐标、随附包路径和具体非法原因。
- registry 未配置且 `pkg/` 未命中时，诊断必须包含找不到的 `<name>@<version>`。
- registry 已配置但 registry 与 `pkg/` 均未命中时，诊断必须包含请求坐标以及已经
  检查的 registry。
- `pkg/` 定位失败应说明期望的发行安装路径；不得把它报告成 registry 下载失败。

## 8 实现影响

### 8.1 主规范

实施代码前更新 [feng-deps.md](../specifications/feng-deps.md)：

- 在 registry 与全局 cache 规则中加入发行包 `pkg/` 备用安装来源。
- 更新缺少 registry 时的行为：先查找 `pkg/`，未命中后报告包找不到。
- 更新已配置 registry 时的回退顺序。
- 明确本地路径依赖不受影响。
- 明确 `pkg/` 只用于安装到 cache，不作为编译器直接包来源。

发行目录布局在 [feng-release-and-install.md](./feng-release-and-install.md)
中只定义一次；其他文档引用该布局，不重复维护目录结构。

### 8.2 依赖管理

主要修改点为 `src/cli/deps/manager.c` 中现有的精确版本 cache 安装入口：

- 使用 CLI 公共安装路径函数组成
  `pkg/<name>-<version>.fb` 的绝对路径。
- 在未配置 registry 时增加 `pkg/` 安装。
- 在已配置 registry 且对应包不存在时增加 `pkg/` 回退。
- 复用现有 bundle manifest 读取、坐标校验、cache 临时文件和原子发布能力。
- 保持根依赖和传递依赖使用同一入口。
- 不增加按 `std`、包名前缀或模块名分支的处理。

如需让 registry 查找结果表达“找到”“不存在”“失败”，该结果只服务于既定的
registry → `pkg/` 回退语义，不改变 registry 协议、重试策略或其他错误行为。

### 8.3 发行组装

- 发行组装流程创建顶层 `pkg/`。
- 将发行任务准备好的随附 `.fb` 原样放入 `pkg/`。
- 三个 host 发行包使用同一套随附包输入，不在组装过程中重新构建或修改 `.fb`。
- 发行校验确认每个预期随附包存在、可打开，且包内名称和版本与文件坐标一致。
- 本文不规定发行包必须随附哪些具体包；具体集合由对应发行阶段决定。

## 9 测试方案

只新增本需求测试，不修改已有测试用例。

### 9.1 install

- cache 缺失、无 registry、`pkg/` 命中：包被复制到 cache，install 成功。
- cache 缺失、无 registry、`pkg/` 未命中：报告 `<name>@<version>` 找不到。
- cache 缺失、有 registry、registry 命中：使用 registry 包，不使用同坐标
  `pkg/` 包。
- cache 缺失、有 registry、registry 未命中、`pkg/` 命中：回退并安装随附包。
- cache 缺失、有 registry、registry 与 `pkg/` 均未命中：安装失败并保留明确诊断。
- cache 已存在且合法、未指定 `--force`：复用 cache。
- 指定 `--force`：按 §5.4 重新选择来源并替换 cache。
- `pkg/` 文件存在但包名、版本或 bundle 结构非法：安装失败且不发布非法 cache。

测试包使用通用名称，不以 `std` 作为触发条件，证明实现不存在具体包特判。

### 9.2 递归依赖

- `pkg/` 安装的包包含精确版本传递依赖，传递依赖继续按相同规则递归安装。
- 本地 `.fb` 包含精确版本传递依赖，该传递依赖可以从 `pkg/` 安装。
- 本地项目 manifest 包含精确版本传递依赖，该传递依赖可以从 `pkg/` 安装，同时
  install 不构建本地项目。

### 9.3 build

- 先独立执行 install，从 `pkg/` 安装到 cache；随后 build 使用 cache 成功。
- 直接执行 build 时，现有依赖准备流程可以完成同样的安装和构建。
- 精确版本依赖最终传给编译器的路径位于全局 cache，不是发行包 `pkg/`。
- 本地路径依赖继续按现有规则消费，不被复制到全局 cache。

### 9.4 回归

- 执行全量 `make test`。
- 按工程规则在 Codex 沙箱外执行全量回归。
- registry 安装、cache 复用、本地 `.fb`、本地项目依赖、传递依赖和 `--force`
  既有行为不得回归。

## 10 实施 TODO

- [x] 0. 等待本文人工 Review；Review 通过后再开始实现。
- [x] 1. 更新依赖管理主规范和发行目录规范。
- [x] 2. 在发行组装与校验中加入通用 `pkg/` 目录。
- [x] 3. 在精确版本安装入口中加入无 registry 时的 `pkg/` 安装。
- [x] 4. 加入 registry 包不存在时的 `pkg/` 回退。
- [x] 5. 复用既有校验和原子写入能力，将随附包安装到全局 cache。
- [x] 6. 新增 install、递归依赖、build 和发行组装测试。
- [x] 7. 在 Codex 沙箱外执行全量 `make test`。
- [x] 8. 整理实现与测试结果，等待交付 Review。
