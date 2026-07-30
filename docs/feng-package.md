# Feng 语言包分发规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的包分发与二进制复用概要说明,聚焦 `.fb` 统一包格式的结构定义、编译流程和使用规则。

## 1 包系统概览

Feng 采用单一标准化包格式 `.fb`（Feng Bundle）,通过包内目录结构的有无来表达所携带的能力层:

- `feng.fm` + `mod/`: 必须存在,记录包元信息与全部公开符号表
- `lib/`: 可选,携带自有 ABI 静态库,供 feng 项目间复用
- `extlib/`: 可选,携带需要随包分发的依赖型原生库制品（静态库或动态库）
- `复制资源目录`: 可选,携带项目声明的运行时资源文件（目录由 `[assets]` 配置指定）

`.fb` 包当前只定义 feng 层正式库分发（`lib/`）; `extern func` 的原生库需求通过公开 `.ft` 元信息表达,必要时可由 `extlib/` 随包携带对应制品。所有模式均支持闭源分发、压缩归档与编译加速,不破坏 feng → C 编译架构。
feng 编译器在消费 `.fb` 时,只直接读取 `mod/` 下的公开 `.ft` 与对应平台下的正式库文件; 不读取包内 `feng.fm`,也不依赖 `feng.fm` 判断制品是否存在。

## 2 包格式定义

- 扩展名: `.fb`（Feng Bundle）
- 底层: ZIP 兼容压缩归档格式,支持标准解压工具
- 不含任何 feng 实现源码,支持闭源分发与编译加速
- 正式分发面只包含公开 `.ft` 与库文件,不包含 `.o` / `.obj` 这类中间目标文件

## 3 包结构

```text
库名-版本.fb
├── feng.fm                  // 必须：包元信息清单
├── mod/                     // 必须：公开符号表目录
│   └── .../*.ft
│
├── lib/                     // 可选①：自有 ABI 静态库（abi 含 feng 时存在）
│   ├── linux-x64-gnu/
│   │   └── libxxx.a
│   ├── linux-x64-musl/
│   │   └── libxxx.a
│   ├── linux-arm64-gnu/
│   │   └── libxxx.a
│   ├── linux-arm64-musl/
│   │   └── libxxx.a
│   └── macos-arm64/
│       └── libxxx.a
│
├── extlib/                  // 可选②：依赖型原生库（按需随包分发；静态/动态均可）
│   ├── linux-x64-gnu/
│   │   ├── libdep.a
│   │   └── libdep.so
│   ├── linux-x64-musl/
│   │   ├── libdep.a
│   │   └── libdep.so
│   └── macos-arm64/
│       ├── libdep.a
│       └── libdep.dylib
│
├── <configured-dir>/        // 可选③：资源目录（由开发态 [assets] 配置指定）
│   └── ...
```

## 4 清单文件（feng.fm）

包 manifest 文件扩展名为 `.fm`,文件名固定为 `feng.fm`,记录包名、版本、支持平台、携带能力层和直接依赖。`feng.fm` 在开发阶段和分发包内均使用同一格式,但部分字段仅在开发阶段有效。

规则说明:

- `feng.fm` 是包级元信息,不是模块声明文件; 一个包可以包含多个公开模块。
- 开发项目使用的 `feng.fm` 与分发包内的 `feng.fm` 共用文件名和同一套节式文本语法,但职责不同: 前者服务于项目构建,后者服务于 `.fb` 分发元信息表达; `feng build` / `deps` / `pack` 等上层工具可以读取它们,而 feng 编译器不读取任何 `feng.fm`。
- 包名用于分发、安装与包管理,不要求与任何具体模块名相同,也不要求一个包只对应一个模块。
- `feng.fm` 采用固定节名的简洁文本格式: 以 `[section]` 声明节,节内以 `key: "value"` 声明字段值。当前标准节为 `[package]`、`[dependencies]`、可选的 `[assets]` 与可选的 `[registry]`。`[package]` 保存包与项目字段,`[dependencies]` 保存直接依赖列表,`[assets]` 保存开发态资源复制配置,`[registry]` 保存开发态项目的本地 registry 覆盖配置。
- 字段值当前统一使用双引号字符串; 节名与键名均为不带引号的标识符。空行允许存在。以 `#` 开头的独立注释行会被忽略。
- `target` 字段声明构建目标,取值为 `bin`（可执行文件）或 `lib`（分发包）; 该字段为**开发阶段必填**,由构建工具读取后转换为编译器 `--target` 参数; 分发包内的 `feng.fm` 不含此字段。
- `src` 字段指定源文件根目录; 省略时默认为 `src/`; 仅开发阶段有效,不出现在分发包内。
- `out` 字段指定输出根目录；省略时默认为 `build/`；`target=bin` 时最终文件位于 `<out>/<platform>/bin/<name>`，`target=lib` 时各完整平台开发产物位于 `<out>/<platform>/`，执行 `feng pack` 后最终包位于 `<out>/pkg/<name>-<version>.fb`；仅开发阶段有效，不出现在分发包内。
- `[assets]` 节声明开发态资源复制配置; 省略时为空,仅开发阶段有效,不出现在分发包内。节内每个键是目标目录（相对可执行文件目录或 `.fb` 根目录）,每个值是源目录路径（相对 `feng.fm` 所在目录）且不得为空字符串。所有开发态资源均按目标平台隔离:`target=bin` 时将指定资源目录复制到 `<out>/<platform>/bin/` 下的可执行文件同级目标目录；`target=lib` 时普通目标目录复制到 `<out>/<platform>/assets/` staging,但当目标目录精确为 `extlib` 时,只将当前平台内容复制到 `<out>/<platform>/extlib/`,不额外插入 `assets/` 目录层。`pack` 从各平台 staging 校验并提取资源写入 `.fb`；构建工具写回 `feng.fm` 时保留 `[assets]` 的声明顺序。
- `platform` 字段采用逗号分隔的完整平台标识列表（例如 `macos-arm64,linux-x64-gnu,linux-x64-musl,linux-arm64-gnu,linux-arm64-musl`），取值见 [feng-os-arch.md](./feng-os-arch.md)，Linux 项必须包含 GNU / musl 后缀。开发项目的平台选择、白名单校验、字段缺失行为以及分发包内该字段的要求，统一以 [feng-cli.md](./feng-cli.md)“项目平台选择统一规则”为准。
- `abi` 字段声明本包携带哪些 Feng 能力层，当前仅支持 `feng`；`feng` 表示存在 `lib/` 目录。该字段与完整平台标识中的 Linux C library ABI 无关，不得写入 `gnu` / `musl` 或用于选择 libc。
- `[dependencies]` 节表示当前包对其他 feng 包的直接依赖; 同包内模块之间的引用不属于依赖声明。
- `[dependencies]` 节中的每个键表示一个直接依赖包名,同一依赖包名不得重复出现。开发态项目中,值允许是精确版本字符串或本地路径字符串: 以 `./`、`../` 或 `/` 开头的值视为本地路径依赖,其他值视为精确版本依赖。分发包内的 `feng.fm` 不允许保留本地路径写法,所有直接依赖都必须写回为精确版本字符串。
- 本地路径依赖可指向三类目标: `.fb` 文件、包含 `feng.fm` 的目录,或显式 `feng.fm` 文件路径。若值是本地路径,则依赖键必须与目标包的 `package.name` 一致; 不一致时构建工具报错。
- `[registry]` 节当前只允许 `url` 字段,其值表示当前开发项目的 registry 基地址或本地 registry 根目录; 该节仅开发态有效,不出现在分发包内。远程依赖的实际拉取协议、全局配置回退与缓存布局以 [feng-deps.md](./feng-deps.md) 为准。
- 当前规范中的依赖声明只描述 feng 包依赖,不描述系统库、动态库、C 头文件或其他原生平台依赖。
- 未知节、未知键、缺失引号值或重复声明均属于非法 `feng.fm`。之所以采用这套自定义小语法,是为了保留“轻量、易解析、字段集合稳定”的优势,同时为后续扩展更多节预留空间; 它不是 YAML、TOML 或 INI 的子集承诺。

示例（开发项目，构建可执行文件）:

```text
# package
[package]
name: "myapp"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"

[dependencies]
base: "1.0.0"
```

示例（开发项目，包含本地路径依赖与局部 registry）:

```text
[package]
name: "app"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"

[dependencies]
base: "1.0.0"
util.local: "../util-local"

[registry]
url: "https://packages.example.com/feng"
```

示例（分发包）:

```text
[package]
name: "mylib"
version: "1.0.0"
platform: "macos-arm64,linux-x64-gnu,linux-x64-musl,linux-arm64-gnu,linux-arm64-musl"
abi: "feng"

[dependencies]
base: "1.0.0"
```

## 5 公开符号表目录与 `.ft` 文件

包内 `mod/` 目录承载公开包表 `.ft`。`.ft` 的语义范围、二进制布局、profile 常量以及 type 实例成员绑定推断事实 / ABI 元信息如何编码,统一以 [feng-symbol-table.md](./feng-symbol-table.md) 为准; 本节只定义它在 `.fb` 中的包级定位与使用规则。

### 5.1 生成与定位规则

- Feng 源码中,一个模块可以分散在多个 `.ff` 文件中,源文件路径与模块名无关。发布方在语义分析成功后按模块聚合公开语义事实与必要的链接事实,每个公开模块恰好输出一个 package-public profile `.ft` 文件。
- `.ft` 文件路径由模块名唯一决定,与源文件分布无关: 模块 `mylib.user` 固定输出到 `mod/mylib/user.ft`。发布方按此规则输出,使用方按此规则定位,无需额外索引。
- `import` 仍然以模块名为导入目标; 编译器与语言服务按模块名推导 `mod/<segments>.ft`,再交由 `.ft` 读取器解析为统一查询视图。
- 公开签名先按 [feng-visibility.md](./feng-visibility.md) 完成可见性一致性检查；检查失败时不生成公开 `.ft`。公开 `.ft` 的声明集合、私有表示依赖闭包和排除项统一以 [feng-symbol-table.md](./feng-symbol-table.md)“符号表内容范围”为准。
- 若模块公开了泛型 `type`、`spec`、顶层 `func` 或成员方法,则写入 `.fb/mod/**/*.ft` 的必须是其声明级泛型事实: 类型参数、约束、未实例化签名骨架、父 `spec` / `fit` 的泛型使用关系；不得要求使用方重新读取 provider 源码或依赖包内某个已单态化实例才能消费这些声明。
- `pack` 校验各 `<out>/<platform>/mod/**/*.ft` 的模块集合与公开语义事实一致后,从其中提取一套写入 `.fb` 的 `mod/` 目录；不得在打包阶段重新抽取接口或重新序列化公开符号表。

### 5.2 包级约束

- `mod/` 下的每个 `.ft` 都必须是公开包表; 是否可消费由文件 header 中的 profile、格式版本和包内 ABI 制品是否齐全共同决定。
- type 实例成员绑定推断事实、`@abi`、`extern func` 调用方式与原生库来源等编译期语义,都必须以 `.ft` 中的声明事实与元信息表达,而不是依赖文本接口源码或包内 `feng.fm`。
- `.ft` 的具体 section、flag、attr key、relation kind 以及兼容性演进规则,不在本文档重复定义; 如需新增或调整,必须修改 [feng-symbol-table.md](./feng-symbol-table.md)。

## 6 编译与使用流程

### 6.1 发布方流程

1. 发布方执行 `feng pack`；目标平台的选择与校验统一以 [feng-cli.md](./feng-cli.md)“项目平台选择统一规则”为准，显式 `--sysroot` 和平台可用性诊断以 [feng-build.md](./feng-build.md) 为准
2. `pack` 先复用项目构建流程，对选定平台固定执行 release 构建；每个平台分别扫描全部 `.ff` 源文件并完成生成、语义分析、代码生成与归档，其 `gen/`、`mod/`、`assets/`、对象、中间产物、正式静态库与原生依赖全部写入独立的 `<out>/<platform>/` 开发构建根（若 `abi` 含 `feng`,正式静态库位于 `<out>/<platform>/lib/`）
3. 全部选定平台构建成功后，`pack` 校验各 `<out>/<platform>/mod/` 的模块集合与公开语义事实等价,并校验各平台准备写入相同包路径的普通 `assets/` 内容一致；分别提取一套作为包内 `mod/` 与普通资源
4. `pack` 分平台汇总 `<out>/<platform>/lib/` 正式库以及 `<out>/<platform>/extlib/` 中由 `extern func` 链接事实要求的原生库,分别写入包内 `lib/<platform>/` 与 `extlib/<platform>/`
5. 生成 `feng.fm`,将实际平台集合填写到 `platform`,并将上述提取结果打包为 `<out>/pkg/<name>-<version>.fb`
6. 任一请求平台构建失败、构件缺失、公开符号表一致性校验或制品完整性校验失败时,发布整体失败,不得生成部分平台 `.fb`

### 6.2 feng 使用方流程

1. 构建工具已将依赖展平成 `.fb` 路径列表传给编译器
2. `import` 公开模块名 → 编译器按模块名推导路径,定位 `mod/` 下对应 `.ft`
3. 由 `.ft` 读取器把公开包表解析为统一查询视图,将公开 `type`、公开 `enum`、公开顶层 `func`、公开模块级 `let` / `var`、公开成员、`spec` / `fit` 与 type 实例成员绑定推断事实引入当前编译期查询环境
    对公开泛型声明,该查询视图还必须暴露类型参数顺序、约束目标、未实例化签名骨架以及泛型父 `spec` / 泛型 `fit` 使用事实,使使用方只依赖 `.ft` 即可完成跨包泛型语义分析
4. 根据声明关键字与 `extern` 链接事实,编译器直接在 `.fb` 中定位当前平台下实际存在的 `lib/` 正式库文件; 若所需文件不存在则报错; 对 `.ft` 保留的原生库名,编译器尝试匹配同包或其他依赖包 `extlib/<platform>/` 下的主机静态库文件（Linux / macOS `lib<name>.a`,Windows `<name>.lib`）,命中时直接提取该静态库参与链接,未命中时再回退为常规原生库链接参数
5. 使用方无需手写带参 `@cdecl` / `@stdcall` / `@fastcall`,包内公开 `.ft` 已携带必要的原生库来源与调用方式元信息

### 6.3 依赖型原生库分发流程

1. 若包需随分发携带依赖型原生库,按平台放入 `extlib/`; 其中静态库文件名遵循主机静态库规则（Linux / macOS `lib<name>.a`,Windows `<name>.lib`）
2. `target=bin` 构建阶段先按 `.ft` 中的 `extern` 链接事实把 `extlib/` 下命中的静态库接入链接,并仅对同样由这些链接事实命中的动态库执行释放到可执行文件同目录; 未命中的 `extlib/` 制品保持不参与

## 7 约束规则

- `mod/` 下的公开 `.ft` 文件属编译器自动生成的接口描述产物,不得手动修改; 一旦其内容与包内真实实现不一致,后续编译、链接或运行行为均不再受语言规范保证,可能表现为编译失败、链接失败或不可预期的运行时异常
- `platform` 与 `abi` 字段属于分发元信息; 若字段存在,则必须与包内实际平台目录和能力层目录一致: 声明了 `feng` 则 `lib/` 必须存在,`platform` 中列出的平台也必须在包内具有对应制品,否则该包非法
- 多平台 `.fb` 仍然只允许一套 `mod/**/*.ft`; 各平台实现对应的公开模块集合与公开语义事实必须等价。若公开 API 或 ABI 事实随平台不同,当前包格式不能把这些实现合并为一个 `.fb`,构建工具必须拒绝打包
- 多平台 `.fb` 的普通复制资源不分平台；各开发平台 staging 中映射到同一包路径的普通资源必须逐文件一致,否则构建工具必须拒绝打包。原生依赖使用 `extlib/<platform>/`,允许各平台内容不同
- feng 编译器只消费公开 `.ft` 与实际存在的正式库文件,不读取 `feng.fm`; `feng.fm` 用于构建、依赖管理与打包校验
- `extern func` 属于导入型外部函数声明,生成的公开 `.ft` 必须保留必要的带参 `@cdecl` / `@stdcall` / `@fastcall` 链接事实,以便使用方无需手写这些注解即可完成编译与链接
- 包兼容性由 `.fb` 目录结构、公开 `.ft` 的格式主版本以及对应平台 ABI 兼容契约共同决定; 编译器不得仅因包由不同版本的 Feng 编译器生成而拒绝使用。若 consumer 不支持该包所需的 `.ft` 主版本或 ABI 契约,必须给出明确诊断。
- 包若公开泛型接口,其可消费性还取决于公开 `.ft` 是否完整携带这些泛型声明的声明级事实。若 `.ft` 丢失类型参数、约束、泛型父 `spec` / `fit` 使用关系或未实例化签名骨架,则该包非法。
- 若包中公开 `type` 包含 `let` 成员,则编译产物必须携带由源码推断出的显式绑定状态相关元信息,以保证使用方仍可在编译期完成对象字面量与构造路径的重复绑定检查
- 私有声明是否作为表示依赖写入 `.ft` 及其内部消费规则以 [feng-symbol-table.md](./feng-symbol-table.md) 为准；写入不改变其可见性
- 公开签名的可见性一致性规则以 [feng-visibility.md](./feng-visibility.md) 为准
- `extlib/` 仅用于依赖型原生库分发; 它不参与模块解析、类型检查与 `lib/` 正式库链接推导,且只有在 `.ft` 链接事实显式引用对应库名时,其中命中的静态库才可参与链接、命中的动态库才可参与运行期释放
- 复制资源目录仅用于运行时资源分发,不参与模块解析、类型检查与链接参数推导
- `.fb` 的稳定分发接口不包含 `.o` / `.obj`; 这类目标文件仅可作为构建或打包阶段的中间产物存在

## 8 包导入语法

`.fb` 包的导入语法与包内携带哪些能力层无关,统一使用模块名:

```feng
import mylib;
import mylib.user;
```

编译器根据公开 `.ft` 中的声明事实与 ABI 元信息自动决定链接策略,使用方无需感知包内具体层结构。

## 9 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、包系统概要、模块、类型、函数、C 互操作、流程控制、自动内存管理与完整示例。
- [feng-symbol-table.md](./feng-symbol-table.md): `.ft` 符号表格式、workspace cache profile 与二进制布局。
- 本文档: `.fb` 包格式、三层结构、`feng.fm` 清单、公开 `.ft` 的包级定位规则、编译流程、使用规则和导入语法的独立补充文档。
