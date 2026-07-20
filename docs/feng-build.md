# Feng 语言编译与构建规范

本文档说明 feng 编译器与构建工具的职责划分、处理逻辑及调用协议。CLI 语法以 [feng-cli.md](./feng-cli.md) 为准,平台标识值以 [feng-os-arch.md](./feng-os-arch.md) 为准,分发包内工具链与 sysroot 布局以 [feng-release-and-instanll.md](../dev/feng-release-and-instanll.md) 为准。

## 1 职责划分

Feng 将"编译"与"构建"明确分为两个独立层次:

- **编译器（`feng` 直接模式）**: 接受源文件列表、目标平台、已平铺的包路径列表（`--pkg <.fb路径>`）以及额外原生库参数（`--lib <库路径或系统库名>`）,负责类型检查、代码生成和链接参数收集。编译器不读取任何 `feng.fm`,也不解析依赖树,仅从传入的平铺 `.fb` 与显式 CLI 参数读取依赖输入。
- **构建工具（`feng build`）**: 读取项目 `feng.fm`,并在解析依赖图时读取依赖包内的 `feng.fm`,按精确版本检查并安装依赖,将依赖图展平为编译器可直接消费的 `--pkg <.fb路径>` 列表,再组装其他编译参数。

两者职责分离的核心原因:编译器只需要足够的信息完成一次编译,不应感知任何 `feng.fm`; 是否存在可用制品,最终都必须以 `.fb` 内实际文件为准。`feng.fm` 由构建工具用于项目组织、依赖解析和打包校验。构建工具可替换,编译器可被第三方构建系统直接驱动。

## 2 编译器职责

### 2.1 接受参数

编译器接受以下输入:

```bash
feng <源文件列表> --target=<目标> [--platform=<os>-<arch>] --out=<输出路径> [--release] [--pkg <.fb路径>]... [--lib <库路径>]...
```

- `<源文件列表>`: 需要编译的 `.ff` 源文件,可使用 glob 展开
- `--target=<目标>`: 产物类型,取值为 `bin`（可执行文件）或 `lib`（库,通常会进一步打包为 `.fb` 分发包）; 该参数必须显式指定,不表示目标操作系统或 CPU 架构
- `--platform=<os>-<arch>`: 目标平台,取值以 [feng-os-arch.md](./feng-os-arch.md) 为准; 未指定时使用归一化后的 host 平台
- `--out <输出路径>`: 指定输出文件路径; 该参数必须显式指定,编译器不假定默认输出位置
- `--release`: 使用发布模式编译; 未指定时使用调试友好的构建模式。调试友好构建会保留主机编译器的 `-O0` / `-g` 路径,并额外生成稳定 `#line` 与当前产物对应的 `.fd` 调试 sidecar; 项目级 `build`、`run` 与 `pack` 的发布模式行为最终透传到该参数
- `--pkg <.fb路径>`: 直接指定依赖包的 `.fb` 文件路径,可重复出现
- `--lib <库路径>`: 直接指定额外链接的原生库路径或系统库名,可重复出现

编译器不接受包名、版本号或搜索路径,不读取任何 `feng.fm`,也不解析依赖树。对 `--pkg` 指定的 `.fb`,编译器只直接读取其中的公开 `.ft` 与目标平台正式库文件（如 `.a`、`.lib`、`.so`、`.dylib`、`.dll`）; `.o` / `.obj` 不属于 `.fb` 的稳定分发接口。调试 sidecar `.fd` 只属于本地构建产物,不从 `.fb` 读取,也不进入 `.fb` 分发包。

### 2.2 目标平台与工具链转换

`--platform` 使用 Feng 自身的 `<os>-<arch>` 标识,不得把 Clang triple 直接暴露为 CLI 值。编译器在调用 Clang 前把目标平台转换为 Clang 的 `--target` 参数；平台标识合法但当前安装缺少对应工具链、runtime 或 sysroot 时,必须报告目标平台不可用。

当前已定义的转换如下:

| Feng 目标平台 | 场景 | Clang `--target` | sysroot |
|---------------|------|------------------|---------|
| `macos-arm64` | macOS 目标 | `arm64-apple-macosx` | 合法安装的 macOS SDK |
| `macos-x64` | macOS 目标 | `x86_64-apple-macosx` | 合法安装的 macOS SDK |
| `linux-x64` | Linux x64 native | `x86_64-unknown-linux-gnu` | host glibc,不显式传 sysroot |
| `linux-arm64` | Linux arm64 native | `aarch64-unknown-linux-gnu` | host glibc,不显式传 sysroot |
| `linux-x64` | 非 Linux x64 host 交叉编译 | `x86_64-unknown-linux-musl` | `toolchain/sysroot/linux-x64/` |
| `linux-arm64` | 非 Linux arm64 host 交叉编译 | `aarch64-unknown-linux-musl` | `toolchain/sysroot/linux-arm64/` |

Windows 与 32 位平台虽然是 [feng-os-arch.md](./feng-os-arch.md) 中的合法平台标识,但在对应 ABI、runtime 与 sysroot 交付前应报告目标平台不可用,不得临时猜测 Clang triple。

sysroot 规则:

- 一次编译只能选择一个有效 sysroot,不得叠加多个 `-isysroot` / `--sysroot` 并依赖参数覆盖顺序。
- macOS 目标使用 `-isysroot <macOS SDK>`。使用 Feng 内置 Clang 编译 macOS 目标时,通过 `xcrun --sdk macosx --show-sdk-path` 获取 SDK 路径；macOS SDK 不随 Feng 分发,用户必须安装 Xcode 或 Xcode Command Line Tools。已有 `CC` 显式覆盖与系统 `cc` 回退保持原有行为,不由 Feng 额外注入 SDK。
- Linux native 使用 host glibc,不传 musl sysroot。
- Linux 交叉编译使用与目标平台对应的 musl `--sysroot`,同时传入上表中的 musl triple,不调用 `xcrun`,也不再传 macOS SDK 的 `-isysroot`。
- `--target` 与 `--sysroot` 只解决目标代码生成、系统头文件和系统库定位,不能替代目标平台 linker 与 compiler runtime。完整链接 Linux musl 产物还必须提供目标 ELF linker,以及 `crtbegin` / `crtend` 和编译器运行时库（compiler-rt 或等价 libgcc 支持）；任一项缺失时只能报告目标平台不可用,不得宣称交叉链接成功。具体分发方案由 [feng-release-and-instanll.md](../dev/feng-release-and-instanll.md) 记录。

Clang 查找顺序:

1. 已有且非空的 `CC`,作为开发与测试的显式覆盖；指定非 host 平台时,该编译器必须兼容 Clang 的 `--target` / `--sysroot` 参数。
2. `<feng 可执行文件目录>/../toolchain/llvm/bin/clang`。
3. `PATH` 中的系统 `cc`,作为源码开发环境或不完整安装的兜底。

不增加 `FENG_HOME`、`FENG_TOOLCHAIN` 等环境变量。可执行文件绝对路径解析与 runtime、Clang、`lldb-dap` 的相对定位共用同一套 CLI 公共路径函数,不得分别实现重复的可执行文件定位逻辑。

目标平台同时决定以下输入,不得只转换 Clang 参数:

- 前端、语义分析与 codegen 中所有平台相关行为,包括指针宽度与平台相关基础类型；不得继续用编译 Feng 自身时的 host `sizeof` 代替目标平台信息。
- Feng runtime 静态库:`<feng 可执行文件目录>/../lib/<目标平台>/`；源码构建保留 `build/lib/` 开发布局回退。
- `.fb` 正式库:`lib/<目标平台>/`。
- `.fb` 原生扩展库:`extlib/<目标平台>/`。
- 交叉编译 sysroot:`toolchain/sysroot/<目标平台>/`。

### 2.3 构建模块索引

编译器启动后,对每个 `--pkg` 指定的 `.fb`:

1. 打开 `.fb` 归档,扫描 `mod/` 目录下所有以 `.ft` 结尾的 entry 路径
2. 由 entry 路径反推出模块名（例如 `mod/net/http.ft` → `net.http`）
3. 建立内部映射表: 模块名 → `.fb` 文件路径 + entry 路径

若两个不同 `--pkg` 注册了相同模块名,编译器在索引阶段立即报错:

```bash
error: 模块 "net.http" 在包 utils-1.0.0.fb 和 net-2.0.0.fb 中均有定义
```

模块名冲突不推迟到 `import` 使用点,编译器不提供别名机制;消解冲突是构建工具在依赖解析阶段的责任。

### 2.4 处理 import 声明

遇到 `import net.http;` 时:

1. 查内部模块索引,定位对应 `.fb`
2. 按模块名层级推导 `.ft` 路径（`net.http` → `mod/net/http.ft`）
3. 通过 `.ft` 读取器把该公开包表解析为统一查询视图,将公开 `type`、公开 `enum`、公开顶层 `func`、公开模块级 `let` / `var`、公开成员、`spec` / `fit` 与 type 实例成员绑定推断事实引入当前作用域

整个过程 O(1),无需遍历搜索。

### 2.5 收集链接信息

编译器从源码 / 依赖包自动收集链接参数,并接收用户显式追加的 `--lib` 参数。

runtime 链接边界:

- `target=bin`: 编译器固定补入目标平台对应的 runtime 静态库。
- `target=lib`: 编译器只生成对象并归档,不链接 runtime,也不在此阶段闭合最终原生依赖。

编译器私有的 runtime contract helper 若存在,统一由这套 runtime 库提供,不再单独产出 intrinsic 静态库。在 Feng 层（如标准库）显式调用这类 helper 时,使用 `@runtime extern func`,不通过公开 C ABI 注解声明内部 helper。

**来源①: `.ff` 源文件中的 `@cdecl` / `@stdcall` / `@fastcall` 导入注解**

```feng
@cdecl("m")
extern func sin(x: float): float;

@cdecl("ssl")
extern func ssl_connect(fd: int): int;
```

编译器读取调用方式注解的第一个参数,解析为库名并生成对应链接参数。若注解带第二个参数,该参数只表示 C 函数名,不参与链接库收集。

**来源②: `--pkg` 指定的 `.fb` 包内正式库文件**

编译器根据公开 `.ft` 中的声明事实与 `extern` 元信息,并结合 `.fb` 内实际存在的目录与文件,自动确定目标平台可用的链接目标:

- 普通 `open type` / `open func` / `open let` / `open var` 声明 → 链接 `lib/` 下目标平台正式静态库; 目标平台文件名规则固定为 Linux / macOS 使用 `lib<name>.a`,Windows 使用 `<name>.lib`
- `extern func` 导入声明 → 从当前源码与导入包公开 `.ft` 携带的 `extern` 链接事实中收集原生库名; 导入包 `.ft` 可以携带非公开 `extern` 的链接事实,这类事实只参与链接信息收集,不作为用户可见 API 导入。若某个 `--pkg` 包在 `extlib/<目标平台>/` 下携带了与该库名匹配的目标平台静态库文件,编译器先提取该静态库并以显式文件路径参与链接; 其余未命中 `extlib/` 的原生库继续转换为底层 C 链接器参数

上述自动收集结果与显式 `--lib` 参数最终汇总后统一传递给底层 C 链接器。`--lib` 在 `target=bin` 的最终链接步骤生效; `target=lib` 只生成对象并归档,不会在该阶段闭合原生依赖。

**`--lib` 的用途**

`--lib` 是兜底参数,仅用于以下场景：没有对应 feng `extern func` 声明、也不来自任何 `.fb` 包的纯原生库（如系统 `pthread`）。大多数情况下不需要手动指定。裸系统库名按 `-l<name>` 语义参与最终链接；显式库文件路径（如 `.a`、`.lib`、`.so`、`.dylib`、`.dll`）按文件路径原样参与最终链接。

补充边界：除编译器自身 runtime 产物外,编译器不会主动扫描磁盘动态查找 `.a` / `.lib` / `.so` / `.dylib` / `.dll`。`extlib/` 静态库只会在已有 `extern func` 元信息显式要求该库名时参与链接,不会因为目录存在而被自动注入。项目与依赖库输入应通过源码声明和显式 CLI 参数（`--pkg` / `--lib`）提供。

### 2.6 动态库运行时查找策略

动态库的运行时加载遵循以下顺序:

1. 可执行文件所在目录。
2. 各操作系统动态加载器的默认查找路径与策略。

说明:

- 编译器不定义额外的运行时动态库发现机制。
- 当编译目标是 `target=bin` 时,核心编译器应先根据当前源码与导入包公开 `.ft` 中携带的 `extern` 链接事实收集库名,仅从传入的平铺 `.fb` 中筛出目标平台且被实际引用的动态库,再释放到可执行文件目录（与可执行文件同目录）。
- 运行期释放只处理 `.fb/extlib/<目标平台>/` 下与已收集库名精确命中的动态库后缀（Linux `lib<name>.so`、macOS `lib<name>.dylib`、Windows `<name>.dll`）；未命中的动态库、`extlib/` 中的静态库（`.a` / `.lib`）与 `.fb/lib/<平台>/` 中的正式静态库都不参与运行期释放。
- 若多个依赖包在目标平台提供同名动态库,构建应报错,避免在可执行文件目录中发生静默覆盖。

### 2.7 调试 sidecar 与源码映射

调试产物遵循以下规则:

- 非 `release` 的 `target=bin` 与 `target=lib` 构建都生成与最终产物同级的 `.fd` sidecar; `target=bin` 产物形态为 `build/bin/<artifact>` 与 `build/bin/<artifact>.fd`, `target=lib` 产物则在库文件同级生成 `.fd`。
- `release` 构建默认不生成 `.fd`,也不额外保留调试态插桩。
- `#line` 的文件名参数固定使用逻辑源码 URI `PKG_NAME://<package-relative path>`,其中 `PKG_NAME` 取源码所属包的 `feng.fm.name`; 编译器不得把宿主磁盘绝对路径写进 `#line`。
- `.fd` 只记录 LLDB 无法直接知道的最小调试事实,例如 package URI 到本地包根的映射、frame 显示名重写、变量显示名映射与少量特殊 carrier 的 `read_expr`; 地址到源码行的映射仍以主机编译器基于 `#line` 与 `-g` 生成的原生调试信息为准。
- `.fd` 属于 artifact-scoped 本地调试产物,`feng clean` 应与其他构建产物一并删除,`feng pack` 不得打包 `.fd`。

## 3 构建工具职责

### 3.1 读取项目清单

构建工具读取项目根目录的 `feng.fm`。CLI 传入的 `<path>` 若省略,使用当前目录下的 `feng.fm`;若为目录,使用该目录下的 `feng.fm`;若为文件,支持直接传入 `feng.fm` 路径。`feng deps install` 使用同样的 `<path>` 规则;`feng deps add` 与 `feng deps remove` 也支持 `<path>`。项目开发阶段与发布包共用同一格式,区别在于:

- 开发项目的 `feng.fm` 可以省略 `abi` 字段（或留空），表示"不作为包发布"
- 发布为 `.fb` 时，`abi` 字段必须存在且与包内目录结构一致

构建工具从 `feng.fm` 读取以下构建配置字段（仅开发阶段有效，不出现在分发包内）:

| 字段 | 必填 | 默认值 | 说明 |
| ------ | ------ | ------ | ------ |
| `target` | 是 | — | 构建目标，取值 `bin`（可执行文件）或 `lib`（库；进一步打包为 `.fb` 分发包） |
| `src` | 否 | `src/` | 源文件根目录，构建工具对此目录 glob 展开 `.ff` 文件 |
| `out` | 否 | `build/` | 输出根目录；`target bin` 时输出 `<out>/<name>`，`target lib` 时输出 `<out>/<name>-<version>.fb` |
| `[assets]` | 否 | 空 | 资源复制配置（字段语义见 [feng-package.md](./feng-package.md)） |

构建工具将 `target` 转换为编译器的 `--target` 参数，将 `out` 与项目名/版本拼装为编译器的 `--out` 参数。

### 3.2 依赖解析

1. 读取 `feng.fm` 的 `[dependencies]` 节,收集直接依赖列表
2. 对每个依赖按 [feng-package.md](./feng-package.md) 的规则判定是精确版本还是本地路径
3. 对远程精确版本依赖检查本地缓存;若未安装或指定了 `--force`,立即从 registry 拉取对应 `.fb` 包到本地缓存
4. 对本地路径依赖做目标解析: `.fb` 直接纳入图; 目录或显式 `feng.fm` 解析为本地 `target: "lib"` 项目,递归构建后纳入图
5. 递归读取每个依赖项目或 `.fb` 包内的 `feng.fm`,继续展开其传递依赖
6. 若两个不同分支依赖同一包的不同精确版本,构建工具报冲突错误,要求用户显式消解版本分歧
7. 若本地项目依赖图出现循环,构建工具立即报错
8. 依赖图锁定后,构建工具将其展平成一组确定的 `.fb` 路径列表,作为后续 `--pkg` 参数输入给编译器

对调试构建补充以下约束:

- 当顶层项目是 `target=bin` 且未指定 `--release` 时,递归本地 `target=lib` 依赖除了产出自身普通库制品外,还必须产出中间 `.fd`。
- 顶层 `target=bin` 构建完成后,构建工具负责把主项目与递归本地依赖的中间 `.fd` 汇总为最终 binary 对应的单个 `.fd`; 运行时的 `feng dap` 只读取这个最终 sidecar,不在会话中递归追踪多个依赖 sidecar。
- 若两个输入 `.fd` 对同一 `PKG_NAME` 提供不一致本地包根,或对同一 `frame_backend_symbol + backend_name` 提供不一致映射,构建必须立即报错,不得把歧义推迟到调试会话。

`feng deps add` 在写入 `feng.fm` 后,若新增的是远程精确版本依赖,立即触发对应安装流程; 若新增的是本地路径依赖,则立即做路径合法性校验。`feng deps install` 会按 `feng.fm` 中声明的依赖递归检查远程缓存,并校验本地路径依赖; 默认只重新拉取缺失的远程包,传入 `--force` 时强制重新拉取全部远程依赖。`feng deps remove` 只更新目标 `feng.fm`,不触发安装流程。`feng build` 与 `feng check` 在执行前总是先执行 `feng deps install`; 然后继续做本地路径依赖的递归构建与完整依赖图展平。`feng build --release` 与 `feng run --release` 需要把 release 模式继续传递给递归构建的本地 `target: "lib"` 依赖; `feng pack` 固定使用 release 模式完成同样的递归构建与打包。

### 3.3 模块名冲突预检

构建工具在锁定依赖图后,可选择性地预检模块名冲突（扫描各包 `mod/**/*.ft` 的 entry 路径并按路径推导模块名），在调用编译器之前提前给出友好报错和消解建议。

### 3.4 组装编译器调用

将解析完成并展平的依赖图转换为编译器参数:

```bash
feng src/*.ff --platform=<host平台> --pkg ~/.feng/cache/utils-1.0.0.fb --pkg ~/.feng/cache/base-2.1.0.fb
```

构建工具传入的是**已确定并展平的 `.fb` 路径列表**,不传包名、版本或搜索路径。编译器只认路径,并直接从这些 `.fb` 中读取所需公开 `.ft` 与正式库文件。

构建工具还应按 `feng.fm` 的 `[assets]` 配置处理资源目录:

- 每个 `[assets]` value 一律按相对 `feng.fm` 所在目录解析,且必须指向目录。
- 复制时递归保留源目录中的相对层级。
- 若目标目录已存在,按构建产物覆盖语义刷新为最新内容,与现有可执行文件/库产物覆盖行为保持一致。
- `target=bin`: 将 `[assets]` 中声明的资源复制到可执行文件同级目录下的目标目录。
- `target=lib`: 普通 `[assets]` 目标目录复制到输出目录下的 `build/assets/` staging; 若目标目录精确为 `extlib`,则直接复制到 `build/extlib/`,供后续 `pack` 复用 `.fb` 顶层 `extlib/` 能力目录。

### 3.5 发布流程（打包为 .fb）

构建工具驱动完整发布流程:

- 若项目的 `target` 不是 `lib`,构建工具在进入打包流程前立即报错。

1. 在语义分析成功后导出 `build/mod/**/*.ft` 公开符号表
2. 调用编译器将普通 `type` / `func` / 模块级 `let` / `var` 实现编译为静态库，放入 `lib/` 对应平台目录（若 `abi` 含 `feng`）
3. 汇总需要参与链接的 `extern func` 声明并保留其原生库来源与调用方式元信息
4. 补全 `feng.fm`（填写 `abi`、`arch` 等字段）
5. 直接复用 `build/mod/**/*.ft`、正式库文件、可选 `build/extlib/` 目录树（包含 `[assets].extlib` 直接 staging 的内容）与 `build/assets/` 中其余 `[assets]` staging 目录打包为 `.fb` ZIP 归档

补充约束:

- `pack` 只复用公开 `.ft`、正式库与资源 staging; `.fd` 调试 sidecar 不属于分发接口,不得进入 `.fb`。

## 4 交互协议总览

```bash
feng.fm (项目)
    │
    ▼
构建工具
    ├─ 解析依赖图，检查并安装精确版本依赖
    ├─ 下载 .fb 到本地缓存
    └─ 组装参数
           │
           ▼
feng src/*.ff --platform=<host平台> --pkg a.fb --pkg b.fb
    │
    ├─ 扫描 .fb，建模块索引
    ├─ 处理 import，定位 .ft，类型检查
    ├─ 收集 @cdecl 注解，生成链接参数
    ├─ 从 .fb 读取目标平台实际存在的 lib，生成链接参数
    └─ 调用 C 链接器，产出最终二进制
```

## 5 与相关规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范，包含模块、类型、函数、C 互操作概要。
- [feng-package.md](./feng-package.md): `.fb` 包格式、`feng.fm` 清单以及编译器可从 `.fb` 读取哪些包级元信息。
- [feng-cli.md](./feng-cli.md): CLI 命令、`--target` / `--platform` 语法与用户可见诊断。
- [feng-os-arch.md](./feng-os-arch.md): Feng 平台标识的唯一值域与归一化规则。
- [feng-release-and-instanll.md](../dev/feng-release-and-instanll.md): 分发包内 runtime、LLVM 工具链与 sysroot 的安装布局。
- [feng-symbol-table.md](./feng-symbol-table.md): `.ft` 符号表格式、profile 常量与二进制布局。
- [feng-interop.md](./feng-interop.md): `@cdecl`/`@stdcall`/`@fastcall` 注解语法与 C 互操作规则。
- 本文档: 编译器与构建工具的职责划分、参数协议、模块索引机制与链接信息收集规则。
