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
feng <源文件列表> --target=<目标> --out=<输出路径> [--name=<产物名>] [--release] [--keep-ir] [--pkg=<.fb路径>|--pkg <.fb路径>]... [--lib <库路径>]...
```

```text
feng <command> [options]
feng <源文件列表> --target=<目标> --out=<输出路径> [--name=<产物名>] [--release] [--keep-ir] [--pkg=<.fb路径>|--pkg <.fb路径>]... [--lib <库路径>]...

命令:
  init       在当前目录初始化 Feng 项目
  build      构建当前项目
  run        构建并运行当前项目
  check      检查当前项目,不产出最终制品
  clean      清理所有构建产物
  pack       为 lib 项目构建并打包为 .fb
  deps       管理项目依赖（add / remove / install 为二级子命令）
  lsp        启动 Feng Language Server（stdio）
  tool       编译器调试与高级诊断子命令集合

选项:
  -h, --help
  -v, --version
```

其中:

- `init`、`build`、`run`、`check`、`clean`、`pack`、`deps` 面向普通项目开发。
- `lsp` 面向 IDE / 编辑器集成,当前通过 stdio 提供 Language Server 入口。
- `tool` 面向编译器开发过程中的调试,以及高级用户对编译细节的诊断。

## 2.1 `feng lsp`

用途: 在 stdio 上传输 LSP/JSON-RPC 消息,启动 Feng Language Server。

用法:

```text
feng lsp [--stdio]
```

选项:

- `--stdio`: 显式声明使用 stdio 传输。当前实现默认即为 stdio,保留该选项用于编辑器与脚本配置显式化。

说明:

- `lsp` 不参与项目构建、打包与运行职责。
- 当前首版仅提供 stdio 传输,不支持 socket / TCP 等其他传输方式。
- 当前服务端通过全量文本同步维护已打开文档状态,并在源码分析路径上支持 `textDocument/didOpen`、`didChange`、`didSave`、`didClose`。
- 当前服务端对 Feng 源文件提供以下语言能力: diagnostics、hover、completion、definition。
- diagnostics / hover / completion / definition 统一复用现有 parser / semantic / imported-module 能力; 当前项目不存在本地 workspace `.ft` 时,必须直接回退到源码分析,不得要求用户先手动生成缓存。
- 若当前项目目录下存在合法 `feng.fm`,LSP 按项目上下文解析整个项目源码并解析依赖包; 若不存在 `feng.fm`,则按单文件模式分析当前文档。
- 对当前项目内已保存且与磁盘一致的文档,若 `build/obj/symbols/**/*.ft` 可读,`hover` / `definition` / `completion` 可优先消费 workspace cache; 若缓存缺失、命中失败或当前文档存在未保存修改,则回退到源码分析。
- `hover` 优先展示声明签名与文档注释; 文档注释只识别已绑定到声明的 `/** */`。
- `definition` 以源码声明位置为主; 当前项目内定义应返回对应源文件位置。
- `completion` 以当前位置可见的局部名、模块级声明、导入模块公开名和对象成员为范围,不要求依赖额外构建步骤。
- 除 `--stdio` 之外不接受其他位置参数或命令选项;出现多余参数时应报错退出。

## 2.2 --out 说明

- `--out` 指定输出路径，需要是一个目录，默认 `./build`
- `build/ir` 中间产物（目前就编译后的 C 源文件），也可放入 `build/ir/c` 中。
- `build/gen` 将来的自定义注解（编译期生成的文件目录）
- `build/bin` 存放可执行文件
- `build/lib` 存放库文件
- `build/pkg` 存放包文件

## 2.3 顶层直编补充选项

- `--name=<产物名>`: 指定本次编译的产物基名。当前 `bin` 目标会落到 `<out>/bin/<name>`；未来 `lib`/`pkg` 目标也复用同一命名语义，而不是再引入只针对可执行文件的选项。
- `--keep-ir`: 固定保留中间 IR 产物。当前实现会把生成的 C 文件保留在 `<out>/ir/c/` 下面，便于编译器开发与问题排查；未指定时，构建开始前会先清理旧的 `ir/c` 产物，前端 / 语义 / codegen 失败不会留下陈旧 C 文件，只有 host C 编译阶段失败时才保留本次生成的 C 代码用于排查；成功构建后仍会把已变空的 `<out>/ir/c` 与 `<out>/ir` 一并清理掉。
- `--pkg=<.fb路径>` / `--pkg <.fb路径>`: 注册一个外部 `.fb` 依赖包,可重复出现。直编模式只接受具体 `.fb` 路径,不接受包名、版本号或搜索路径。
- `--release`: 作为统一顶层选项保留；是否真正生效由对应构建路径决定。

## 3 全局选项

所有顶层命令支持以下全局选项:

- `-h`, `--help`: 显示帮助,可用于 `feng` 或任意子命令。
- `-v`, `--version`: 显示 CLI 版本和编译器版本。

## 4 常用项目命令

### 4.1 `feng init`

用途: 在当前目录初始化一个 Feng 项目。

用法:

```text
feng init [<name>] [--target=<bin|lib>]
```

选项:

- `<name>`: 指定包名,记录到 `feng.fm`;若省略,使用当前目录名。
- `--target=<bin|lib>`: 指定项目类型,`bin` 为可执行项目,`lib` 为库项目,默认 `bin`。

说明:

- `init` 只在当前目录为空时允许执行;若当前目录存在除 `.` 与 `..` 之外的任意目录项,应报错退出,且不得覆盖或追加任何现有文件。
- 初始化成功时写入当前目录下的 `feng.fm`,其中至少包含 `name`、`version`、`target`、`src` 与 `out` 字段; `version` 固定初始化为 `0.1.0`, `src` 与 `out` 分别初始化为 `src/` 与 `build/`。
- `target = bin` 时生成 `src/main.ff` 作为可执行项目入口模板; `target = lib` 时生成 `src/lib.ff` 作为库项目模板。
- `init` 会先将 `<name>` 或当前目录名归一化为安全名称后再写入 `feng.fm` 与 starter 文件: 保留 ASCII 字母、数字、下划线与 `.` 分段,其他字符替换为 `_`; 每个分段若以数字开头或命中关键字 / 保留字,自动前缀 `_`; 若归一化后为空,回退为 `app`。
- starter 源文件中的默认 `mod` 声明使用当前包名,便于初始化后直接形成与项目名一致的默认示例; 若用户需要其他模块名,可自行修改源文件。

### 4.2 `feng build`

用途: 读取 `feng.fm`,调用编译器构建项目。

用法:

```text
feng build [<path>] [--release]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--release`: 以发布模式构建,透传给当前项目编译器,并同样用于递归构建本地 `target: "lib"` 依赖。

说明:

- `build` 从 `feng.fm` 中读取源文件列表、编译目标、输出路径等配置,不接受编译器级别的细粒度选项。
- `build` 总是先对同一 `feng.fm` 执行 `feng deps install`;默认情况下,已安装的依赖不会重新安装。
- 未指定 `--release` 时使用调试友好的构建模式; 指定 `--release` 时改用发布优化模式。

### 4.3 `feng run`

用途: 构建并运行当前项目的可执行目标。

用法:

```text
feng run [<path>] [--release] [-- <program-args>...]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--release`: 以发布模式构建,透传给当前项目编译器,并同样用于递归构建本地 `target: "lib"` 依赖。

说明:

- `run` 总是先复用与 `feng build` 相同的项目构建主链; 构建成功后再运行产物,`<path>` 和 `--release` 均透传给该构建阶段。
- `--` 之后的参数直接透传给目标程序。
- 若当前项目是 `lib`,应给出明确诊断。

### 4.4 `feng check`

用途: 做快速语义检查,不产出最终二进制或包。

用法:

```text
feng check [<path>] [--format <text|json>]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为 `feng.fm` 文件,直接使用该清单;若为项目内任意文件路径,则从该文件所在目录开始逐级向上查找最近的 `feng.fm`;若最终找不到 `feng.fm`,报错退出。
- `--format <text|json>`: 指定诊断输出格式,`text` 为人类可读,`json` 适合编辑器或 CI 消费,默认 `text`。

说明:

- 面向日常编辑-检查循环。
- `check` 是项目级命令,检查范围始终由解析出的 `feng.fm` 决定,而不是只检查传入的单个 `.ff` 文件。
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

- `pack` 总是先复用与 `feng build --release` 相同的项目构建主链,完成当前项目及其递归本地 `target: "lib"` 依赖的 release 构建后,再对产物打包,不接受 `--release` 选项。
- `<path>` 透传给 `feng build`。
- 若项目的 `target` 不是 `lib`,报错退出。

## 5 依赖管理命令

`deps` 是管理 `feng.fm` 依赖的统一入口,`add`、`remove`、`install` 均作为其二级子命令。

### 5.1 `feng deps add`

用途: 向 `feng.fm` 增加依赖。

用法:

```text
feng deps add <pkg-name> <version-or-path> [<path>]
```

选项:

- `<pkg-name>`: 依赖包名。
- `<version-or-path>`: 精确版本字符串,或以 `./`、`../`、`/` 开头的本地路径。
- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。

说明:

- `deps add` 的 `<path>` 为第三个位置参数。
- 若 `<version-or-path>` 是远程精确版本,`deps add` 在写入 `feng.fm` 后立即安装或校验缓存。
- 若 `<version-or-path>` 是本地路径,`deps add` 在写入前先校验目标是否合法,但不触发构建。

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
feng tool compile [--target=<bin|lib>] [--emit-c=<path>] <file>
feng tool lex <file>
feng tool parse <file>
feng tool semantic [--target=<bin|lib>] <file> [more files...]
feng tool check [--target=<bin|lib>] <file> [more files...]
```

各子命令职责:

- `feng tool compile`: 面向编译器开发过程中的单文件 codegen 调试，可直接输出 C 源到 stdout 或 `--emit-c=<path>`。
- `feng tool lex`: 输出词法 token 流。
- `feng tool parse`: 输出 AST 或 parse 结果。
- `feng tool semantic`: 输出人类可读的语义诊断。
- `feng tool check`: 输出更适合编辑器或 CI 消费的结构化诊断。

说明:

- `compile` 归属于 `tool`，不作为长期保留的顶层主命令，避免把编译器调试入口暴露到普通项目工作流的主命名空间。

## 7 帮助输出示例

示例:

```text
Feng CLI

Usage:
  feng <command> [options]
  feng <源文件列表> --target=<目标> --out=<输出路径> [--name=<产物名>] [--release] [--keep-ir] [--pkg=<.fb路径>|--pkg <.fb路径>]... [--lib <库路径>]...

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
