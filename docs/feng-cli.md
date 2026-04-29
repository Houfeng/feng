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
feng <源文件列表> --target <目标> --out <输出路径> [--release] [--pkg <.fb路径>]... [--lib <库路径>]...
```

```text
feng <command> [options]
feng <源文件列表> --target <目标> --out <输出路径> [--release] [--pkg <.fb路径>]... [--lib <库路径>]...

命令:
  init       在当前目录初始化 Feng 项目
  build      构建当前项目
  run        构建并运行当前项目
  check      检查当前项目,不产出最终制品
  clean      清理所有构建产物
  pack       为 lib 项目构建并打包为 .fb
  deps       管理项目依赖（add / remove / install 为二级子命令）
  tool       编译器调试与高级诊断子命令集合

选项:
  -h, --help
  -v, --version
```

其中:

- `init`、`build`、`run`、`check`、`clean`、`pack`、`deps` 面向普通项目开发。
- `tool` 面向编译器开发过程中的调试,以及高级用户对编译细节的诊断。

## 2.1 --out 说明

- `--out` 指定输出路径，需要是一个目录，默认 `./build`
- `build/ir` 中间产物（目前就编译后的 C 源文件），也可放入 `build/ir/c` 中。
- `build/gen` 将来的自定义注解（编译期生成的文件目录）
- `build/bin` 存放可执行文件
- `build/lib` 存放库文件
- `build/pkg` 存放包文件

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

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--release`: 以发布模式构建,透传给编译器。

说明:

- `build` 从 `feng.fm` 中读取源文件列表、编译目标、输出路径等配置,不接受编译器级别的细粒度选项。
- `build` 总是先对同一 `feng.fm` 执行 `feng deps install`;默认情况下,已安装的依赖不会重新安装。

### 4.3 `feng run`

用途: 构建并运行当前项目的可执行目标。

用法:

```text
feng run [<path>] [--release] [-- <program-args>...]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--release`: 以发布模式构建,透传给编译器。

说明:

- `run` 总是先执行 `feng build`,构建成功后再运行产物,`<path>` 和 `--release` 均透传给 `feng build`。
- `--` 之后的参数直接透传给目标程序。
- 若当前项目是 `lib`,应给出明确诊断。

### 4.4 `feng check`

用途: 做快速语义检查,不产出最终二进制或包。

用法:

```text
feng check [<path>] [--format <text|json>]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--format <text|json>`: 指定诊断输出格式,`text` 为人类可读,`json` 适合编辑器或 CI 消费,默认 `text`。

说明:

- 面向日常编辑-检查循环。
- `check` 总是先对同一 `feng.fm` 执行 `feng deps install`;默认情况下,已安装的依赖不会重新安装。
- 完成依赖安装后执行语义检查,但跳过最终制品生成。

### 4.5 `feng clean`

用途: 清理所有构建产物。

用法:

```text
feng clean [<path>]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。

说明:

- `clean` 删除该项目的所有构建产物,包括最终产物与中间文件。

### 4.6 `feng pack`

用途: 为 `target = lib` 的项目生成 `.fb` 分发包。

用法:

```text
feng pack [<path>]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。

说明:

- `pack` 总是先执行 `feng build --release`,再对产物打包,不接受 `--release` 选项。
- `<path>` 透传给 `feng build`。
- 若项目的 `target` 不是 `lib`,报错退出。

## 5 依赖管理命令

`deps` 是管理 `feng.fm` 依赖的统一入口,`add`、`remove`、`install` 均作为其二级子命令。

### 5.1 `feng deps add`

用途: 向 `feng.fm` 增加依赖。

用法:

```text
feng deps add <pkg-name[@version]> [<path>]
```

选项:

- `<pkg-name[@version]>`: 包名,可附加 `@version` 指定版本,例如 `feng deps add mylib@1.2`。若包已存在,覆盖其版本记录并重新拉取该依赖，省略版本时将安装最新版。
- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。

说明:

- `deps add` 的 `<path>` 为第二个位置参数。
- `deps add` 在写入 `feng.fm` 后立即拉取依赖,确保本地安装状态与清单一致。

### 5.2 `feng deps remove`

用途: 从 `feng.fm` 移除依赖。

用法:

```text
feng deps remove <pkg-name> [<path>]
```

选项:

- `<pkg-name>`: 依赖包名。
- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。

说明:

- `deps remove` 的 `<path>` 为第二个位置参数。
- 一个项目只允许依赖同一包的一个版本,因此移除时只需指定包名。

### 5.3 `feng deps install`

用途: 按 `feng.fm` 中的声明安装项目依赖。

用法:

```text
feng deps install [<path>] [--force]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--force`: 强制重新安装 `feng.fm` 中声明的全部依赖,即使这些依赖已经安装。

说明:

- `deps install` 按 `feng.fm` 中声明的精确版本安装所有依赖。
- 默认情况下,已安装的依赖不会重新安装。

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

## 7 帮助输出示例

示例:

```text
Feng CLI

Usage:
  feng <command> [options]
  feng <源文件列表> --target <目标> --out <输出路径> [--release] [--pkg <.fb路径>]... [--lib <库路径>]...

Project Commands:
  init      Initialize a project in the current directory
  build     Build the current project
  run       Build and run the current project
  check     Type-check and analyze without producing final artifacts
  clean     Remove all build artifacts
  pack      Create a .fb package from the current lib project

Dependency Commands:
  deps      Manage dependencies (add / remove / install)

Developer Tools:
  tool      Compiler debugging and advanced diagnostic tools

Global Options:
  -h, --help
  -v, --version
```

## 8 有意不提供的命令

### `feng test`

测试程序本质上是普通的 Feng 程序。用户选择适合项目的测试框架,通过 `feng run` 执行测试入口即可。CLI 不感知"测试"概念,避免对测试框架的选择产生不必要的约束。

### `feng fmt`

代码格式化由编辑器插件负责,例如 VS Code 的 Feng 插件。CLI 不提供格式化命令,避免在工具链中重复维护相同能力。

### `feng publish`

包发布涉及注册表认证、包命名策略、版本冲突处理等配套基础设施。当前阶段尚无官方包注册表,待生态具备条件后再引入发布命令。

### `feng deps update`

Feng 依赖采用严格版本管理：`feng.fm` 中记录的版本即为精确版本，不使用版本范围或"最新兼容版本"语义。

原因：若采用非严格版本，开发者本地拉取代码安装依赖时可能自动升级，CI 构建和生产部署时同样可能拿到不同版本，导致"在我机器上能跑"的经典问题。解决这个问题通常需要引入 lock 文件机制，带来额外复杂度。Feng 的选择是从源头避免：版本始终精确，`feng.fm` 本身即为唯一的版本来源，无需 lock 文件，也无需 update 命令。升级依赖时，显式执行 `feng deps add <pkg-name@new-ver>` 覆盖即可。
