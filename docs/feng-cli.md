# Feng CLI 命令与选项

> 本文件仅描述 CLI 的命令、选项与参数，不涉及 CLI 内部处理逻辑。内部构建与编译流程请参考 [feng-build.md](feng-build.md)。

## 1 设计目标

- 让新用户只靠 `feng --help` 就能看到最常用命令。
- 让项目构建、运行、检查、发布形成一致的工作流。
- 保留直接驱动编译器的能力,方便第三方构建系统、IDE、脚本和编译器开发调试。
- 尽量避免把调试型命令暴露为顶层主命令,防止顶层命名空间过早膨胀。

## 2 顶层命令结构

基础编译

```bash
feng <源文件列表> --target <目标> --out <输出路径> --release [--pkg <.fb路径>]... [--lib <库路径>]...
```

```text
feng <command> [options]

命令:
  init       在当前目录初始化 Feng 项目
  build      构建当前项目
  run        构建并运行当前项目
  check      检查当前项目,不产出最终制品
  clean      清理构建产物
  pack       打包为 .fb
  deps       管理项目依赖（add / remove / update 为二级子命令）
  tool       编译器调试与高级诊断子命令集合

选项:
  -h, --help
  -v, --version
```

其中:

- `init`、`build`、`run`、`check`、`clean`、`pack`、`deps` 面向普通项目开发。
- `tool` 面向编译器开发过程中的调试,以及高级用户对编译细节的诊断。

## 3 全局选项

所有顶层命令支持以下全局选项:

- `-h`, `--help`: 显示帮助,可用于 `feng` 或任意子命令。
- `-v`, `--version`: 显示 CLI 版本和编译器版本。

## 4 常用项目命令

### 4.1 `feng init`

用途: 在当前目录初始化一个 Feng 项目。

用法:

```text
feng init [<pkg-name>] [--target <bin|lib>]
```

选项:

- `<pkg-name>`: 指定包名,记录到 `feng.fm`;若省略,使用当前目录名。
- `--target <bin|lib>`: 指定项目类型,`bin` 为可执行项目,`lib` 为库项目,默认 `bin`。

说明:

- 在当前目录初始化项目,生成 `feng.fm`、`src/` 及示例入口文件。

### 4.2 `feng build`

用途: 读取 `feng.fm`,调用编译器构建项目。

用法:

```text
feng build [<path>] [--release]
```

选项:

- `<path>`: 显式指定 `feng.fm` 路径;若省略,自动查找当前目录下的 `feng.fm`;若找不到,报错退出。
- `--release`: 以发布模式构建,透传给编译器。

说明:

- `build` 从 `feng.fm` 中读取源文件列表、编译目标、输出路径等配置,不接受编译器级别的细粒度选项。

### 4.3 `feng run`

用途: 构建并运行当前项目的可执行目标。

用法:

```text
feng run [<path>] [--release] [-- <program-args>...]
```

选项:

- `<path>`: 显式指定 `feng.fm` 路径;若省略,自动查找当前目录下的 `feng.fm`;若找不到,报错退出。
- `--release`: 以发布模式构建,透传给编译器。

说明:

- `--` 之后的参数直接透传给目标程序。
- 若当前项目是 `lib`,应给出明确诊断。

### 4.4 `feng check`

用途: 做快速语义检查,不产出最终二进制或包。

用法:

```text
feng check [<path>] [--target <bin|lib>] [--format <text|json>]
```

选项:

- `<path>`: 显式指定 `feng.fm` 路径;若省略,自动查找当前目录下的 `feng.fm`;若找不到,报错退出。
- `--target <bin|lib>`: 指定检查目标类型;若省略,从 `feng.fm` 读取。
- `--format <text|json>`: 指定诊断输出格式,`text` 为人类可读,`json` 适合编辑器或 CI 消费,默认 `text`。

说明:

- 面向日常编辑-检查循环。
- 语义上等价于“走完整依赖解析与检查流程,但跳过最终制品生成”。

### 4.5 `feng clean`

用途: 清理构建产物。

用法:

```text
feng clean [<path>]
```

选项:

- `<path>`: 显式指定 `feng.fm` 路径;若省略,自动查找当前目录下的 `feng.fm`;若找不到,报错退出。

### 4.6 `feng pack`

用途: 生成 `.fb` 分发包。

用法:

```text
feng pack [<path>]
```

选项:

- `<path>`: 显式指定 `feng.fm` 路径;若省略,自动查找当前目录下的 `feng.fm`;若找不到,报错退出。

说明:

- `pack` 内部应隐式触发一次干净的 `build`,确保打包产物与源码同步。

## 5 依赖管理命令

`deps` 是管理 `feng.fm` 依赖的统一入口,`add`、`remove`、`update` 均作为其二级子命令。

### 5.1 `feng deps add`

用途: 向 `feng.fm` 增加依赖。

用法:

```text
feng deps add <pkg-name[@version]>
```

选项:

- `<pkg-name[@version]>`: 包名,可附加 `@version` 指定版本要求,例如 `feng deps add mylib@1.2`。

### 5.2 `feng deps remove`

用途: 从 `feng.fm` 移除依赖。

用法:

```text
feng deps remove <package>
```

### 5.3 `feng deps update`

用途: 更新依赖锁定结果。

用法:

```text
feng deps update [package...]
```

说明:

- 无参数时更新全部可更新依赖。
- 带包名时只更新指定依赖。

## 6 调试与分析命令

`feng tool` 面向两类使用场景:编译器开发过程中的调试,以及高级用户对词法、语法、语义细节的诊断。

命令结构:

```text
feng tool lex <file>
feng tool parse <file>
feng tool semantic [--target <bin|lib>] <file> [more files...]
feng tool check [--target <bin|lib>] <file> [more files...]
```

各子命令职责:

- `feng tool lex`: 输出词法 token 流。
- `feng tool parse`: 输出 AST 或 parse 结果。
- `feng tool semantic`: 输出人类可读的语义诊断。
- `feng tool check`: 输出更适合编辑器或 CI 消费的结构化诊断。

说明:

- 当前原型已存在 `lex`、`parse`、`semantic`、`check` 顶层命令。

## 8 帮助输出示例

示例:

```text
Feng CLI

Usage:
  feng <command> [options]

Project Commands:
  init      Initialize a project in the current directory
  build     Build the current project
  run       Build and run the current project
  check     Type-check and analyze without producing final artifacts
  clean     Remove build artifacts
  pack      Create a .fb package

Dependency Commands:
  deps      Manage dependencies (add / remove / update)

Developer Tools:
  tool      Compiler debugging and advanced diagnostic tools

Global Options:
  -h, --help
  -v, --version
```
