# Feng CLI 命令与选项

> 本文件仅描述 CLI 的命令、选项与参数，不涉及 CLI 内部处理逻辑。平台标识值以 [feng-os-arch.md](feng-os-arch.md) 为准，内部构建、工具链选择与平台转换流程以 [feng-build.md](feng-build.md) 为准，发行包内 runtime / LLVM / sysroot 布局以 [feng-release-and-instanll.md](../dev/feng-release-and-instanll.md) 为准。

## 1 设计目标

- 让新用户只靠 `feng --help` 就能看到最常用命令。
- 让项目构建、运行、检查、发布形成一致的工作流。
- 保留直接驱动编译器的能力,方便第三方构建系统、IDE、脚本和编译器开发调试。
- 尽量避免把调试型命令暴露为顶层主命令,防止顶层命名空间过早膨胀。

## 2 顶层命令结构

基础编译

```bash
feng <源文件列表> --target=<目标> --platform=<platform> [--sysroot=<路径>] --out=<输出目录> [--name=<产物名>] [--release] [--keep-ir] [--pkg=<.fb路径>|--pkg <.fb路径>]... [--lib <库路径>]...
```

```text
feng <command>  [<options>]
feng <源文件列表> [<options>]

命令:
  init       在当前目录初始化 Feng 项目
  build      构建当前项目
  run        构建并运行当前项目
  check      检查当前项目,不产出最终制品
  clean      清理所有构建产物
  pack       以 release 模式构建 lib 项目并打包为 .fb
  deps       管理项目依赖（add / remove / install 为二级子命令）
  lsp        启动 Feng Language Server（stdio）
  dap        启动 Feng Debug Adapter Protocol 代理（stdio）
  tool       编译器调试与高级诊断子命令集合

选项:
  -h, --help
  -v, --version
```

其中:

- `init`、`build`、`run`、`check`、`clean`、`pack`、`deps` 面向普通项目开发。
- `lsp` 面向 IDE / 编辑器集成,当前通过 stdio 提供 Language Server 入口。
- `dap` 面向 IDE / 编辑器集成,当前通过 stdio 提供 Debug Adapter Protocol 入口。
- `tool` 面向编译器开发过程中的调试,以及高级用户对编译细节的诊断。
- 顶层直编模式接收源文件与已平铺的 `--pkg <.fb路径>` / `--lib` 参数,不解析依赖树。
- 依赖树解析、安装与展平由 `feng build` / `feng deps` 负责。

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
- 当前服务端对 Feng 源文件提供以下语言能力: diagnostics、hover、completion、definition、references、rename。
- diagnostics / hover / completion / definition / references / rename 统一复用现有 parser / semantic / imported-module 能力; 当前项目不存在本地 workspace `.ft` 时,必须直接回退到源码分析,不得要求用户先手动生成缓存。
- 若当前项目目录下存在合法 `feng.fm`,LSP 按项目上下文解析整个项目源码并解析依赖包; 若不存在 `feng.fm`,则按单文件模式分析当前文档。
- 对当前项目内已保存且与磁盘一致的文档,若 `build/<归一化 host 平台>/obj/symbols/**/*.ft` 可读,`hover` / `definition` / `completion` 可优先消费 workspace cache; 若缓存缺失、命中失败或当前文档存在未保存修改,则回退到源码分析。
- `hover` 优先展示声明签名与文档注释; 绑定签名必须展示该绑定的静态类型,省略类型标注的绑定应使用 semantic 分析得到的初始化器推导类型,不得把缺失的语法类型误展示为 `void`; infix match 绑定必须在声明位置及其按 `docs/feng-flow.md` 可见的使用位置展示收窄后的静态类型,单 member 显示具体 member 类型,多 member 显示对应的子集 union 类型; 参数悬停签名必须保留参数可变性关键字(`let`/`var`)与参数类型; 函数或方法返回类型位置的类型标识符必须支持 `hover` 与 `definition`; 对声明悬停命中应以标识符为主,如 `fit` 扩展方法的 `func` 关键字位置不要求返回函数说明; 文档注释只识别已绑定到声明的 `/** */`,外部依赖包公开 `.ft` 中已规范化保存的文档注释也必须在 hover 中展示; 服务端应按客户端在 initialize 中声明的 Hover `contentFormat` 能力协商返回 `markdown` 或 `plaintext`,支持 Markdown 时应将声明签名与文档注释格式化为标准 Markdown,其中 `@foo bar ...` 风格的文档标签应按结构化参数项显示; 客户端未声明 Markdown 能力时必须回退为纯文本 Hover,保证跨编辑器兼容。
- LSP 用户可见的声明签名必须保留参数声明的表层语法; 对变长参数必须显示为 `T...`,不得把内部规范化后的 `T[]` 暴露在 hover 或 completion detail 中。
- `definition` 以源码声明位置为主; 当前项目内定义应返回对应源文件位置。
- `references` 返回当前工作区源码中的所有引用位置; 对外部依赖包符号,可返回本地工作区中的使用点,但不要求返回包内只读定义位置。
- `rename` 仅作用于当前工作区中的可写源码符号; 外部依赖包符号、只读缓存符号与非独立命名实体不参与重命名; 光标位于标识符内部或紧邻标识符末尾时,均应按该标识符处理。
- `completion` 以当前位置可见的参数、局部名、模块级声明、导入模块公开名和对象成员为范围,不要求依赖额外构建步骤; 服务端应声明 `.` 与标识符起始字符为自动补全触发字符; 作用域补全应按源码中的词法位置判断当前可见范围,包括函数或方法体内最后一条语句之后、闭合 `}` 之前的空白区域; 对编辑过程中常见的临时不完整语句和成员访问,例如在两条语句之间新输入标识符、输入普通标识符前缀、位于闭合 `}` 或文件末尾前的未完成表达式前缀、刚输入 `.` 或成员名前缀尚未解析通过时,应尽量返回可推断的作用域符号或对象成员补全,不得因为当前光标处的临时语法不完整而直接放弃已有可见符号; `import foo.` 这类模块路径补全对同一层级的重复段名只允许返回一次,并且必须同时覆盖当前项目源码模块与外部依赖包 `.fb/mod/**/*.ft` 中的公开模块; 对 `import foo.bar as baz;` 这类别名导入,输入 `baz.` 时也应返回被导入模块的公开声明,即使当前成员访问仍处于编辑中的不完整状态; 来自导入模块或别名模块的候选项必须保留真实声明种类与签名详情,例如函数显示为 `func ...` 签名、类型显示为 `type ...`,不得退化成泛化的 `module` 或 `imported` 文本; 当当前文档仍可解析、但项目级语义分析因其他文件或入口约束失败时,补全仍应尽力返回当前文件可见作用域以及 `import` 导入模块的公开名; 当当前文档仅因正在输入模块路径或类型名而暂时无法完整解析时,补全仍应利用项目依赖解析结果返回外部包模块路径片段与已导入模块的公开类型名; 当已 `import` 外部依赖模块后,普通标识符位置与类型构造表达式位置也必须返回该模块公开类型名,不得只在类型标注位置生效; 外部类型实例成员补全只能返回字段、方法、构造函数与析构函数等可访问成员,不得把类型泛型形参作为实例成员候选; 不得因为项目其余诊断或当前输入未完成而整体退化为空列表; 当对象来自局部变量时,显式类型标注、对象字面量初始化式与类型构造调用初始化式均可作为对象类型推断来源; 同名重载函数或方法应保留为可区分的候选项,不得因为标签相同而丢失全部候选。
- 除 `--stdio` 之外不接受其他位置参数或命令选项;出现多余参数时应报错退出。

## 2.2 `feng dap`

用途: 在 stdio 上传输 DAP/JSON-RPC 消息,启动 Feng Debug Adapter Protocol 代理。

用法:

```text
feng dap [--stdio]
```

选项:

- `--stdio`: 显式声明使用 stdio 传输。当前实现默认即为 stdio,保留该选项用于编辑器与脚本配置显式化。

说明:

- `dap` 与 `lsp` 明确分层; `feng dap` 只负责调试协议代理,不承载语言服务能力。
- `feng dap` 支持 macOS 与 Linux 上的 `lldb-dap` 后端,launch 入口只接受 `target=bin` 的本地非 `release` 构建产物。
- `feng dap` 先在本地处理 `initialize`,随后在 DAP `launch` 前完成 `.fd` 装载与 binary 指纹校验。只有校验通过后才按以下顺序定位并拉起后端：非空 `FENG_LLDB_DAP` 显式指定的单个可执行文件、`<feng 可执行文件目录>/../toolchain/llvm/bin/lldb-dap`（发行布局见 [feng-release-and-instanll.md](../dev/feng-release-and-instanll.md)）、`PATH` 中的 `lldb-dap`、macOS `xcrun -f lldb-dap`。`FENG_LLDB_DAP` 指定的工具不可用、bundled 路径存在但损坏或已选后端启动失败时必须保留真实原因并明确报错，不得静默尝试后续候选；只有前一层未配置或 bundled 路径不存在时才能继续。环境变量值不作为 shell 命令解析，也不接受内嵌参数。进入代理阶段后,`setBreakpoints` 会把编辑器本地文件路径改写为 `PKG_NAME://<package-relative path>`,`stackTrace` 会把该逻辑 URI 回写为编辑器本地文件路径,并把 backend frame 名称重写为 Feng callable 名称,同时隐藏标记为 runtime / generated helper 的 frame。
- `feng dap` 在 DAP `launch` 请求中定位目标 binary 同级的 `.fd`,校验 sidecar 中记录的 binary 内容指纹与当前 binary 是否匹配; 校验失败必须直接拒绝会话。
- `feng dap` 当前已负责在编辑器本地文件路径与 `PKG_NAME://<package-relative path>` 逻辑源码 URI 之间双向转换,并在 `stackTrace` 上完成 backend frame 名称重写与 `HIDDEN` frame 过滤; `variables` 当前以 `.fd` 中声明的用户变量映射为白名单,会过滤未映射的 backend 临时变量,并优先用用户变量自己的读表达式回读显示值: 对 capture / `self` 等特殊 carrier 使用 `.fd.read_expr`,对普通参数与局部绑定则默认以其 backend lvalue 作为只读回读表达式,避免首次停下时直接暴露不稳定的 backend 原值; 当前 frame 可直接访问的模块级 binding 也需要出现在该 frame 的 `.fd` 变量映射中,这样 backend `Globals` scope 才能显示 Feng 名称; `.fd` 中每个用户变量记录现在都必须携带 `display_type`,数组类型沿用 Feng 语法显示为 `T[]` 或 `T[!]`; 对仍表现为 runtime carrier pointer 的数组 / 字符串变量,顶层变量显示会进一步收敛,其中数组显示为基于 runtime `length` 字段回读得到的 `元素类型[length=N]` 摘要,例如 `string[length=1]`,而字符串应优先显示实际字符串值,而不是沿用数组式的 `string[length=N]` 格式。为避免循环体首行断点命中 `for` 初始化之前的 backend 位置,调试构建还需要把三段式 `for` 的 header / body / update scaffolding 保持在稳定且分离的逻辑源码行上,并把一个 Feng binding 或表达式语句展开出的 `ensure_init` / 临时局部 / variadic 打包数组 / cleanup 注册持续锚定在同一源码行,不能把下一条语句的首个断点地址提前占走。`evaluate` 已支持只读 watch 子集中的 identifier、成员访问、常量整数字面量索引以及简单算术 / 比较表达式的 Feng 名称解析与后端读取改写; `frame_policy` 的 `COLLAPSE` 语义仍在后续子项中。
- 当前首版不支持 attach、reverse debugging、具有副作用的 evaluate/watch,也不支持函数调用、赋值、非常量索引或其他未落入只读 watch 子集的 Feng 表达式求值。
- 除 `--stdio` 之外不接受其他位置参数或命令选项;出现多余参数时应报错退出。

## 2.3 --out 说明

- 顶层直编模式的 `--out` 指定**本次单平台编译的精确输出根目录**,直编不会自动追加目标平台目录，也不感知多平台目录编排；其 `ir/`、`gen/`、`bin/`、`lib/`、`mod/` 与 `obj/` 均直接位于该 `--out` 目录下。
- 项目级 `feng build` 从 `feng.fm.out` 取得项目输出根（默认 `./build`），再为每个完整目标平台构造 `<项目输出根>/<platform>`，并将该目录作为对应直编调用的 `--out`。因此项目开发态产物表现为 `build/linux-x64-gnu/ir/`、`build/linux-x64-musl/gen/` 等。
- `assets/` 与 `extlib/` 由读取 `feng.fm` 的项目构建层 staging 到同一 `<项目输出根>/<platform>/`；直编模式不读取 `[assets]`，也不负责建立这两个目录。
- `feng pack` 从各目标平台开发目录提取并校验所需内容，将最终单一 `.fb` 写入 `<项目输出根>/pkg/`；直编模式不生成 `.fb`。

## 2.4 顶层直编补充选项

- `--target=<bin|lib>`: 指定产物类型,`bin` 为可执行文件,`lib` 为库；该参数不表示目标操作系统或 CPU 架构。
- `--platform=<platform>`: 必须显式指定本次核心直编的唯一完整目标平台，取值必须是 [feng-os-arch.md](feng-os-arch.md) 平台矩阵中的规范标识。顶层直编不根据 host、sysroot 或产物类型推断平台，不自动展开多平台，也不允许重复该选项。Linux 必须写成 `linux-x64-gnu`、`linux-x64-musl`、`linux-arm64-gnu` 或 `linux-arm64-musl`，不接受不完整的 `linux-x64` / `linux-arm64`。项目级 `feng build` / `feng pack` 的多平台编排规则见 §4.2 / §4.6。
- `--sysroot=<路径>`: 为本次唯一目标平台显式指定目标 sysroot。该选项不下载、复制或授权任何 SDK；用户负责所提供路径及其内容的许可合规。macOS 目标传给 Clang 时转换为 `-isysroot`，Linux 目标转换为 `--sysroot`。完整默认值与平台规则见 [feng-build.md](feng-build.md)。
- `--name=<产物名>`: 指定本次直编的产物基名。`bin` 目标落到 `<out>/bin/<name>`，`lib` 目标落到 `<out>/lib/<平台静态库名>`；该选项不负责 `.fb` 命名。
- `--keep-ir`: 固定保留本次直编的中间 IR 产物。当前实现会把生成的 C 文件保留在 `<out>/ir/c/` 下面，便于编译器开发与问题排查；未指定时，构建开始前会先清理该直编输出根中的旧 `ir/c` 产物，前端 / 语义 / codegen 失败不会留下陈旧 C 文件，只有目标 C 编译阶段失败时才保留本次生成的 C 代码用于排查；成功构建后仍会把已变空的 `ir/c` 与 `ir` 一并清理掉。
- `--pkg=<.fb路径>` / `--pkg <.fb路径>`: 注册一个外部 `.fb` 依赖包,可重复出现。直编模式只接受具体 `.fb` 路径,不接受包名、版本号或搜索路径,也不解析依赖树。
- `--release`: 作为统一顶层选项保留；是否真正生效由对应构建路径决定。

## 3 全局选项

所有顶层命令支持以下全局选项:

- `-h`, `--help`: 显示帮助,可用于 `feng` 或任意子命令; 用户显式请求帮助时,帮助文本输出到 stdout 并返回 0。
- `-v`, `--version`: 显示版本信息。

补充约定:

- 当命令行参数、子命令或位置参数不合法时,CLI 应先输出错误原因,再把对应 usage 输出到 stderr,并以非 0 状态退出。

## 4 常用项目命令

### 项目平台选择统一规则

| 场景 | `feng.fm.platform` 已声明 | 未声明 |
|---|---|---|
| `feng build` 不传 `--platform` | 按声明顺序构建全部平台 | 只构建 host |
| `feng build --platform=<platform>` | 只构建指定平台，但该平台必须在声明中 | 允许任意合法且工具链可用的平台 |
| `feng run` | 固定运行 host；host 不在声明中则报错 | 固定构建并运行 host |
| `feng pack` 不传 `--platform` | 先以 release 模式构建全部声明平台，再打包全部平台 | 先构建 host，再打包 host |
| `feng pack --platform=...` | 只构建并打包指定平台，所有值必须在声明中 | 可选择任意合法且工具链可用的平台 |

补充规则：

- `feng.fm.platform` 声明的是完整平台集合，例如：

```text
platform: "macos-arm64,linux-x64-gnu,linux-x64-musl"
```

- 字段存在时，它同时是默认目标集合和平台白名单。
- 字段不存在时，不限制显式 `--platform`；仅在没有命令行参数时默认使用 host。
- `feng init --target=bin` 默认不写 `platform`。
- `feng init --target=lib` 默认写入首版全部可产出平台。
- `feng run` 不支持 `--platform`。
- `feng build` / `feng pack` 传入 `--sysroot` 时，最终只能选择一个平台；未传 `--platform` 且 `feng.fm.platform` 声明多个平台时，必须报错。
- `feng pack` 会复用构建流程，固定执行 release 构建，成功后再生成 `.fb`。
- 分发包内的 `feng.fm.platform` 必须存在，并精确记录包内实际携带的平台集合。
- 顶层直编不读取 `feng.fm`，必须显式指定唯一的 `--platform`。

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
- 初始化成功时写入当前目录下的 `feng.fm`,其中至少包含 `name`、`version`、`target`、`src` 与 `out` 字段; `version` 固定初始化为 `0.1.0`, `src` 与 `out` 分别初始化为 `src/` 与 `build/`。`platform` 字段的初始化规则以本节“项目平台选择统一规则”为准。
- `target = bin` 时生成 `src/main.ff` 作为可执行项目入口模板; `target = lib` 时生成 `src/lib.ff` 作为库项目模板。
- `init` 会先将 `<name>` 或当前目录名归一化为安全名称后再写入 `feng.fm` 与 starter 文件: 保留 ASCII 字母、数字、下划线与 `.` 分段,其他字符替换为 `_`; 每个分段若以数字开头或命中关键字 / 保留字,自动前缀 `_`; 若归一化后为空,回退为 `app`。
- starter 源文件中的默认 `module` 声明使用当前包名,便于初始化后直接形成与项目名一致的默认示例; 若用户需要其他模块名,可自行修改源文件。

### 4.2 `feng build`

用途: 读取 `feng.fm`,调用编译器构建项目。

用法:

```text
feng build [<path>] [--release] [--platform=<platform>] [--sysroot=<路径>]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--release`: 以发布模式构建,透传给当前项目编译器,并同样用于递归构建本地 `target: "lib"` 依赖。
- `--platform=<platform>`: 指定本次项目构建的唯一完整目标平台，不能重复；选择与校验规则以本节“项目平台选择统一规则”为准。每个值都必须是 [feng-os-arch.md](feng-os-arch.md) 中的完整规范标识；不得从 `--sysroot` 推断 GNU / musl。
- `--sysroot=<路径>`: 显式指定目标 sysroot，只允许本次命令最终构建一个目标平台时使用。需要为多个平台使用不同 sysroot 时，应分别执行多次带单一 `--platform` 的 `feng build`。

说明:

- `build` 从 `feng.fm` 中读取源文件列表、编译目标、输出路径等配置；除项目级 `--release`、`--platform` 与单目标 `--sysroot` 外，不接受编译器级别的细粒度选项。
- `build` 总是先对同一 `feng.fm` 执行 `feng deps install`;默认情况下,已安装的依赖不会重新安装。
- `build` 负责解析依赖树并展平为 `--pkg <.fb路径>` 列表,再调用核心编译器。
- `build` 按本节“项目平台选择统一规则”取得目标平台集合。每次直编只传一个 `--platform`，并将 `<项目输出根>/<platform>` 作为该次直编的 `--out`。项目构建层再把原生依赖与普通资源 staging 到同一平台目录；同一目标平台集合继续传递给递归本地 `target: "lib"` 依赖。
- 多平台 `target=lib` 构建在各平台目录分别产生 `mod/**/*.ft`；`feng pack` 校验其公开接口事实一致后只向 `.fb` 写入一套。完整编排与产物规则见 [feng-build.md](feng-build.md)，`.fb` 结构见 [feng-package.md](feng-package.md)。
- `build` 读取 `feng.fm` 的 `[assets]` 配置：`target=bin` 时复制到 `<项目输出根>/<platform>/bin/` 下的可执行文件同级目标目录；`target=lib` 时普通目标目录先复制到 `<项目输出根>/<platform>/assets/` staging，后续 `pack` 从各目标平台目录校验并提取一份写入 `.fb` 内对应目标目录；若目标目录精确为 `extlib`，则直接复制当前目标平台对应的内容到 `<项目输出根>/<platform>/extlib/`，不额外插入 `assets` 目录层。
- 当构建目标是 `target=bin` 时,核心编译器会先根据当前源码与导入包公开 `.ft` 中的 `extern func` 元信息解析原生库名；若某个依赖包在 `.fb/extlib/<platform>/` 下携带了匹配该库名的主机静态库（Linux / macOS `lib<name>.a`,Windows `<name>.lib`）,则先提取并参与链接；若携带了匹配该库名的动态库（Linux `lib<name>.so`,macOS `lib<name>.dylib`,Windows `<name>.dll`）,则仅这些已命中的动态库会被释放到可执行文件同目录,其余未命中的 `extlib` 制品不参与。
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

- `run` 的平台选择与清单校验以本节“项目平台选择统一规则”为准。校验通过后，它复用项目构建主链，只对归一化后的 host 平台执行一次构建；构建成功后运行 `<项目输出根>/<host-platform>/bin/` 下的 host 可执行文件，`<path>` 和 `--release` 均透传给该构建阶段。
- `--` 之后的参数直接透传给目标程序。
- 若当前项目是 `lib`,应给出明确诊断。
- host 平台不可用、项目依赖缺少 host 平台制品或最终产物不能在当前 host 运行时，必须给出明确诊断，不得选择其他目标平台尝试运行。

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
feng pack [<path>] [--platform=<platform>]... [--sysroot=<路径>]
```

选项:

- `<path>`: 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径;若最终找不到 `feng.fm`,报错退出。
- `--platform=<platform>`: 指定需要写入同一 `.fb` 的完整目标平台，可重复出现；选择与校验规则以本节“项目平台选择统一规则”为准。
- `--sysroot=<路径>`: 显式指定目标 sysroot，只允许本次打包一个平台时使用，并传给项目构建流程。

说明:

- `pack` 按本节“项目平台选择统一规则”取得目标平台集合，复用项目构建流程并固定执行 release 构建；全部目标平台构建成功后才生成 `.fb`。`pack` 不接受 `--release`。
- `<path>` 用于读取项目 `feng.fm`、输出根、包名和版本；`--platform` 选择本次必须构建并写入 `.fb` 的平台。
- 若项目的 `target` 不是 `lib`,报错退出。
- `pack` 从每个 `<项目输出根>/<platform>/` 提取 `mod/`、`lib/`、`extlib/` 与 `assets/`：各平台 `mod/` 的公开语义事实和普通 `assets/` 内容必须一致,校验后各提取一套写入 `.fb/mod/` 与配置的资源目录；`lib/` 和 `extlib/` 则按目标平台分别写入 `.fb/lib/<platform>/` 与 `.fb/extlib/<platform>/`。分发包 `feng.fm.platform` 必须与实际写入的平台集合完全一致。任一请求平台构件缺失或校验失败时不得生成部分平台 `.fb`。

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
Usage:
  feng <command>  [<options>]
  feng <files...> [<options>]

Project:
  feng init       [<name>] [--target=<bin|lib>]
  feng build      [<path>] [--release] [--platform=<platform>] [--sysroot=<path>]
  feng check      [<path>] [--format=<text|json>]
  feng run        [<path>] [--release] [-- <program-args>...]
  feng clean      [<path>]
  feng pack       [<path>] [--platform=<platform>]... [--sysroot=<path>]
  feng deps       <add|remove|install> ...

Compile:
  feng <files...> [--target=<bin|lib>]
                  --platform=<platform>
                  [--sysroot=<path>]
                  [--out=<dir>]
                  [--name=<artifact>]
                  [--release]
                  [--keep-ir]
                  [--pkg=<fb-path>...]
                  [--lib=<lib-path>...]

Global:
  -h, --help      Display this message.
  -v, --version   Display version information.

Protocol:
  feng lsp        [--stdio]
  feng dap        [--stdio]
```

- 用户显式执行 `--help` 时,上述帮助文本输出到 stdout。
- 当 CLI 因参数错误、未知命令或缺少必要参数而附带输出同类 usage 时,该文本输出到 stderr。

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
