# Feng CLI 命令与选项草案

本文档是 Feng CLI 的命令与选项设计草案,用于评审和讨论,不是最终定稿。

起草目标参考了主流语言工具链的共性做法,例如 Rust / Cargo、Go、dotnet、Swift Package Manager 等:

- 普通用户优先面对“项目级命令”,而不是编译器内部细节。
- 高频操作应有短而直接的命令,例如 `build`、`run`、`check`、`test`。
- 低层编译与调试能力仍然保留,但应与日常项目命令分层。
- 选项风格应尽量统一,减少“同一类含义却多套命名”的情况。

## 1 设计目标

- 让新用户只靠 `feng --help` 就能看到最常用命令。
- 让项目构建、运行、检查、测试、格式化、发布形成一致的工作流。
- 保留直接驱动编译器的能力,方便第三方构建系统、IDE、脚本和编译器开发调试。
- 尽量避免把调试型命令暴露为顶层主命令,防止顶层命名空间过早膨胀。

## 2 顶层命令结构草案

建议 Feng CLI 采用如下分层:

```text
feng <command> [options]

常用项目命令:
  new        创建新项目
  init       在当前目录初始化项目
  build      构建当前项目
  run        构建并运行当前项目
  check      检查当前项目,不产出最终制品
  test       运行测试
  fmt        格式化源代码
  doc        生成项目文档
  clean      清理构建产物

依赖与发布命令:
  add        添加依赖
  remove     移除依赖
  update     更新依赖锁定
  package    打包为 .fb
  publish    发布包

低层编译器命令:
  compile    直接调用编译器
  tool       调试/分析子命令集合

全局选项:
  -h, --help
  -V, --version
  -v, --verbose
  -q, --quiet
  --color <auto|always|never>
  -C, --cwd <dir>
```

其中:

- `build`、`run`、`check`、`test`、`fmt`、`doc` 面向普通项目开发。
- `compile` 面向构建工具、脚本、IDE 或高级用户。
- `tool` 面向编译器开发与调试,例如词法、语法、语义检查。

## 3 全局选项草案

所有顶层命令建议统一支持以下全局选项:

- `-h`, `--help`: 显示帮助,可用于 `feng` 或任意子命令。
- `-V`, `--version`: 显示 CLI 版本和编译器版本。
- `-v`, `--verbose`: 增加日志详细度,可重复,例如 `-vv`。
- `-q`, `--quiet`: 减少输出,常用于 CI。
- `--color <auto|always|never>`: 控制彩色输出,建议默认 `auto`。
- `-C`, `--cwd <dir>`: 切换工作目录后再执行,参考主流工具链。

建议约定:

- 长选项统一使用 kebab-case,例如 `--manifest-path`、`--out-dir`。
- 布尔选项优先使用显式长选项,例如 `--release`、`--offline`。
- 首版不建议提供过多命令别名,避免文档和补全复杂化。

## 4 常用项目命令草案

### 4.1 `feng new`

用途: 创建一个新的 Feng 项目目录。

建议用法:

```text
feng new <name> [--bin|--lib] [--vcs <git|none>]
```

建议选项:

- `--bin`: 创建可执行项目。
- `--lib`: 创建库项目。
- `--vcs <git|none>`: 是否初始化版本控制。

说明:

- 默认建议创建 `bin` 项目。
- 自动生成 `feng.fm`、`src/`、示例入口文件和基础 `.gitignore`。

### 4.2 `feng init`

用途: 在当前目录初始化 Feng 项目。

建议用法:

```text
feng init [path] [--bin|--lib]
```

说明:

- 适合把已有目录转成 Feng 项目。
- 与 `new` 不同,`init` 不强制创建新目录。

### 4.3 `feng build`

用途: 读取 `feng.fm`,解析依赖,构建项目。

建议用法:

```text
feng build [--release] [--target <bin|lib>] [--manifest-path <path>] [--out-dir <dir>] [--offline] [--locked]
```

建议选项:

- `--release`: 以发布模式构建。
- `--target <bin|lib>`: 覆盖项目默认目标。
- `--manifest-path <path>`: 指定 `feng.fm` 路径。
- `--out-dir <dir>`: 覆盖输出目录。
- `--offline`: 禁止联网下载依赖。
- `--locked`: 必须使用已锁定依赖,不允许更新锁文件。

说明:

- `build` 是项目级入口,优先给普通用户使用。
- 其职责应与 [docs/feng-build.md](./feng-build.md) 中“构建工具”一致。

### 4.4 `feng run`

用途: 构建并运行当前项目的可执行目标。

建议用法:

```text
feng run [--release] [--manifest-path <path>] [-- <program-args>...]
```

说明:

- `--` 之后的参数直接透传给目标程序。
- 若当前项目是 `lib`,应给出明确诊断。

### 4.5 `feng check`

用途: 做快速语义检查,不产出最终二进制或包。

建议用法:

```text
feng check [--target <bin|lib>] [--manifest-path <path>] [--offline] [--locked]
```

说明:

- 面向日常编辑-检查循环。
- 语义上等价于“走完整依赖解析与检查流程,但跳过最终制品生成”。

### 4.6 `feng test`

用途: 发现并运行测试。

建议用法:

```text
feng test [<filter>] [--release] [--manifest-path <path>] [-- --test-args...]
```

建议选项:

- `<filter>`: 只运行名称匹配的测试。
- `--release`: 用发布模式执行测试。
- `-- <test-args...>`: 透传给测试运行器。

### 4.7 `feng fmt`

用途: 格式化 Feng 源码。

建议用法:

```text
feng fmt [paths...] [--check]
```

建议选项:

- `paths...`: 限定格式化路径。
- `--check`: 仅检查是否需要格式化,不改写文件。

说明:

- `fmt` 应尽量可预测,避免引入多套互斥风格。
- 若将来有专门格式化规范,`fmt` 应以前者为准。

### 4.8 `feng doc`

用途: 生成项目 API 文档或规范化文档输出。

建议用法:

```text
feng doc [--open] [--manifest-path <path>] [--out-dir <dir>]
```

建议选项:

- `--open`: 生成后自动打开文档。
- `--out-dir <dir>`: 指定输出目录。

### 4.9 `feng clean`

用途: 清理构建产物。

建议用法:

```text
feng clean [--manifest-path <path>]
```

## 5 依赖与发布命令草案

### 5.1 `feng add`

用途: 向 `feng.fm` 增加依赖。

建议用法:

```text
feng add <package> [--version <req>]
```

### 5.2 `feng remove`

用途: 从 `feng.fm` 移除依赖。

建议用法:

```text
feng remove <package>
```

### 5.3 `feng update`

用途: 更新依赖锁定结果。

建议用法:

```text
feng update [package...]
```

说明:

- 无参数时更新全部可更新依赖。
- 带包名时只更新指定依赖。

### 5.4 `feng package`

用途: 生成 `.fb` 分发包。

建议用法:

```text
feng package [--manifest-path <path>] [--out-dir <dir>] [--allow-dirty]
```

建议选项:

- `--allow-dirty`: 允许在工作区有未提交变更时继续打包。

### 5.5 `feng publish`

用途: 发布 `.fb` 包到仓库或注册中心。

建议用法:

```text
feng publish [--dry-run] [--registry <name>] [--token <token>]
```

建议选项:

- `--dry-run`: 只验证发布流程,不真正上传。
- `--registry <name>`: 指定目标注册中心。
- `--token <token>`: 显式传入发布令牌。

## 6 低层编译器命令草案

### 6.1 `feng compile`

用途: 直接驱动编译器,不读取项目文件,面向构建工具、脚本和高级用户。

建议用法:

```text
feng compile <sources...> --target <bin|lib> --out <path> [--pkg <file>]... [--lib <path>]...
```

建议首版选项:

- `<sources...>`: 输入的 `.ff` 源文件列表。
- `--target <bin|lib>`: 编译目标。
- `--out <path>`: 输出路径。
- `--pkg <file>`: 依赖 `.fb` 文件,可重复。
- `--lib <path>`: 额外原生库路径或库名,可重复。

说明:

- 该接口应与 [docs/feng-build.md](./feng-build.md) 中的编译器职责保持一致。
- 作为低层接口,建议参数尽量稳定、明确、可脚本化。
- `--target` 建议要求显式给出,不要依赖隐式默认值。

可预留但不必首版就实现的扩展项:

- `--emit <kind>`: 控制输出中间产物,如对象文件、接口文件、IR。
- `-O`, `--opt-level <0|1|2|3|s|z>`: 优化等级。
- `-g`, `--debug-info`: 生成调试信息。

## 7 调试与分析命令草案

建议不要把词法、语法、语义调试命令长期保留为顶层主命令,而是收口到 `feng tool` 下。

建议结构:

```text
feng tool lex <file>
feng tool parse <file>
feng tool semantic [--target <bin|lib>] <file> [more files...]
feng tool check [--target <bin|lib>] <file> [more files...]
```

各子命令职责建议如下:

- `feng tool lex`: 输出词法 token 流。
- `feng tool parse`: 输出 AST 或 parse 结果。
- `feng tool semantic`: 输出人类可读的语义诊断。
- `feng tool check`: 输出更适合编辑器或 CI 消费的结构化诊断。

说明:

- 当前原型已存在 `lex`、`parse`、`semantic`、`check` 顶层命令。
- 本草案建议未来把它们迁移到 `feng tool` 命名空间,这样顶层帮助更聚焦普通开发流。

## 8 建议的帮助输出风格

建议 `feng --help` 的帮助文本按“高频优先、分类清晰”的方式组织,而不是简单按字母序平铺。

建议示例:

```text
Feng CLI

Usage:
  feng <command> [options]

Project Commands:
  new       Create a new project
  init      Initialize a project in an existing directory
  build     Build the current project
  run       Build and run the current project
  check     Type-check and analyze without producing final artifacts
  test      Run tests
  fmt       Format source files
  doc       Generate documentation
  clean     Remove build artifacts

Package Commands:
  add       Add a dependency
  remove    Remove a dependency
  update    Update dependencies
  package   Create a .fb package
  publish   Publish a package

Compiler Commands:
  compile   Invoke the compiler directly
  tool      Developer-oriented lexer/parser/semantic tools

Global Options:
  -h, --help
  -V, --version
  -v, --verbose
  -q, --quiet
  --color <auto|always|never>
  -C, --cwd <dir>
```

## 9 命名与选项风格建议

- 顶层命令优先使用单个英文动词: `build`、`run`、`test`、`publish`。
- 选项名优先使用长选项,并保持术语统一。例如统一使用 `--manifest-path`,不要同时出现 `--manifest`、`--project-file`、`--fm`。
- 对同一概念尽量只保留一种主写法。例如输出目录建议统一 `--out-dir`,最终输出文件建议统一 `--out`。
- 项目级命令与低层编译命令尽量不要复用完全不同语义的同名参数。

## 10 建议首批落地命令

若需要分阶段实现,建议优先级如下:

1. `build`
2. `run`
3. `check`
4. `test`
5. `fmt`
6. `compile`
7. `tool lex` / `tool parse` / `tool semantic`
8. `new` / `init`
9. `add` / `remove` / `update`
10. `package` / `publish` / `doc` / `clean`

这样可以先把“开发闭环”补齐,再逐步完善项目初始化、包管理与发布能力。

## 11 评审点

本草案当前最值得 review 的点包括:

1. 顶层是否应保留 `compile`,还是进一步弱化为仅供构建工具使用。
2. 当前原型的 `lex` / `parse` / `semantic` / `check` 是否应迁移到 `feng tool` 命名空间。
3. 项目级 `check` 是否足够,还是仍然需要单独保留文件级检查命令。
4. `package` 与 `publish` 是否分开,还是合并成更少的发布命令。
5. `build` 是否允许 `--target` 覆盖 `feng.fm`,还是要求项目清单为唯一来源。
