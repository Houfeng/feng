# Feng 语言包分发规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的包分发与二进制复用概要说明,聚焦 `.fb` 统一包格式的结构定义、编译流程和使用规则。

## 1 包系统概览

Feng 采用单一标准化包格式 `.fb`（Feng Bundle）,通过包内目录结构的有无来表达所携带的能力层:

- `feng.fm` + `mod/`: 必须存在,记录包元信息与全部公开接口声明
- `lib/`: 可选,携带自有 ABI 静态库,供 feng 项目间复用
- `clib/` + `include/`: 可选,携带 C ABI 兼容库与头文件,供跨语言复用

一个 `.fb` 包可以同时携带两个可选层,也可以只携带其中一层,由 `feng.fm` 中的 `abi` 字段显式声明。所有模式均支持闭源分发、压缩归档与编译加速,不破坏 feng → C 编译架构。

## 2 包格式定义

- 扩展名: `.fb`（Feng Bundle）
- 底层: ZIP 兼容压缩归档格式,支持标准解压工具
- 不含任何 feng 实现源码,支持闭源分发与编译加速

## 3 包结构

```text
库名-版本.fb
├── feng.fm                  // 必须：包元信息清单
├── mod/                     // 必须：公开接口声明目录
│   └── .../*.fi
│
├── lib/                     // 可选①：自有 ABI 静态库（abi 含 feng 时存在）
│   ├── linux-x64/
│   │   └── libxxx.a
│   ├── windows-x64/
│   │   └── xxx.lib
│   └── macos-arm64/
│       └── libxxx.a
│
├── clib/                    // 可选②：C ABI 兼容库（abi 含 c 时存在）
│   ├── linux-x64/
│   │   ├── libxxx.a
│   │   └── libxxx.so
│   ├── windows-x64/
│   │   ├── xxx.lib
│   │   └── xxx.dll
│   └── macos-arm64/
│       ├── libxxx.a
│       └── libxxx.dylib
│
└── include/                 // 可选②附：C 头文件（随 clib/ 一起出现）
    └── xxx.h
```

## 4 清单文件（feng.fm）

包 manifest 文件扩展名为 `.fm`,文件名固定为 `feng.fm`,记录包名、版本、支持平台、携带能力层和直接依赖。`feng.fm` 在开发阶段和分发包内均使用同一格式,但部分字段仅在开发阶段有效。

规则说明:

- `feng.fm` 是包级元信息,不是模块声明文件; 一个包可以包含多个公开模块。
- 包名用于分发、安装与包管理,不要求与任何具体模块名相同,也不要求一个包只对应一个模块。
- `target` 字段声明构建目标,取值为 `bin`（可执行文件）或 `lib`（分发包）; 该字段为**开发阶段必填**,由构建工具读取后转换为编译器 `--target` 参数; 分发包内的 `feng.fm` 不含此字段。
- `src` 字段指定源文件根目录; 省略时默认为 `src/`; 仅开发阶段有效,不出现在分发包内。
- `out` 字段指定输出根目录; 省略时默认为 `build/`; `target bin` 时最终文件为 `<out>/<name>`,`target lib` 时为 `<out>/<name>-<version>.fb`; 仅开发阶段有效,不出现在分发包内。
- `abi` 字段声明本包携带哪些能力层,取值为 `feng`、`c` 或 `feng,c`; `feng` 表示存在 `lib/` 目录,`c` 表示存在 `clib/` 与 `include/` 目录; 该字段必须与包内实际目录结构一致,声明与实际不符则该包非法; 开发项目若不作为包发布可省略此字段。
- `dependency` 表示当前包对其他 feng 包的直接依赖; 同包内模块之间的引用不属于 `dependency`。
- `dependency` 可重复出现零次到多次,每一行表示一个直接依赖包; 同一依赖包名不得重复出现。
- 当前规范中的 `dependency` 只描述 feng 包依赖,不描述系统库、动态库、C 头文件或其他原生平台依赖。
- `feng.fm` 采用固定字段的简洁键值文本格式,不使用 YAML; 原因是字段集合稳定、语法更简单、解析成本更低,也避免引入缩进、锚点等与包加载无关的复杂性。

示例（开发项目，构建可执行文件）:

```text
name:myapp
version:0.1.0
target:bin
src:src/
out:build/
dependency:base@1.0.0
```

示例（分发包，同时携带两层）:

```text
name:mylib
version:1.0.0
arch:linux-x64,windows-x64,macos-arm64
abi:feng,c
dependency:base@1.0.0
dependency:json@2.1.0
```

示例（分发包，仅携带自有 ABI 层）:

```text
name:mylib
version:1.0.0
arch:linux-x64,windows-x64,macos-arm64
abi:feng
dependency:base@1.0.0
```

## 5 接口声明目录与 `.fi` 文件

### 5.1 生成规则

公开接口描述文件使用 `.fi` 扩展名,由 feng 编译器自动生成,统一放在包内固定的 `mod/` 目录下,包含公有签名以及编译期语义检查所需的类型与绑定元信息,但不包含实现逻辑、值或数据本身。

规则说明:

- Feng 源码中,一个模块可以分散在多个 `.ff` 文件中,源文件路径与模块名无关。编译器在生成 `.fi` 时负责将同一模块的所有公开声明合并,每个公开模块恰好输出一个 `.fi` 文件。
- `.fi` 文件路径由模块名唯一决定,与源文件分布无关: 模块 `mylib.user` 固定输出到 `mod/mylib/user.fi`。编译器按此规则输出,使用方按此规则定位,无需额外索引。
- `use` 仍然以模块名为导入目标; 编译器按模块名推导路径、定位对应 `.fi`,解析其公有声明后根据声明关键字决定链接目标,最后将公开 `type`、公开顶层 `fn` 和公开模块级 `let` / `var` 引入当前模块作用域。
- 公开模块级 `let` / `var` 也必须写入 `.fi`; 接口文件只保留其可见性、`let` / `var` 属性和显式类型。若源码中省略了类型标注,编译器在 `.fi` 中必须写出推导后的显式类型; `.fi` 不保留初始化表达式、值或数据本身。
- `type` 内的公开 `let` / `var` 成员在 `.fi` 中同样只保留可见性、`let` / `var` 属性和显式类型,不保留成员声明及绑定初值、值或数据本身。
- 顶层公开 `fn`、公开成员方法和公开构造函数在 `.fi` 中都只保留签名与必要元信息,不保留函数体、语句、表达式或其他实现内容。
- `.fi` 中若需要表达公开 `let` 的显式绑定状态,必须通过 `@bounded` 等内建元信息声明,不能通过保留值、常量数据或初始化结果来隐式表达。
- 一个 `.fi` 文件中可以同时包含普通 `type`/`fn`/模块级绑定声明、带 `@fixed` 的 ABI 声明以及 `extern fn` 导入声明。编译器按声明关键字与注解元信息共同确定链接目标和 ABI 语义。

### 5.2 `.fi` 文件示例

`mod/mylib/api.fi`（同时包含多类声明）:

```feng
pu mod mylib.api;

pu type User {
    pu var name: string;
    pu var age: int;
    pu fn get_info(): string;
}

pu let default_age: int;

pu fn add_user(u: User): bool;

@fixed
pu type Buffer {
    pu var size: int;
}

@fixed
pu fn buf_alloc(size: int): Buffer;

@cdecl("mylib")
pu extern fn buf_free(buf: Buffer);
```

### 5.3 初始化元信息要求

- `.fi` 只保留类型、可见性、`let` / `var` 属性和显式绑定元信息,不保留值、常量数据或初始化表达式本身。
- `.fi` 中的顶层公开 `fn`、公开成员方法和公开构造函数都必须写成无函数体的签名形式; `.fi` 不保留任何 `fn` 的实现代码。
- 对公开 `type` 的成员,接口元信息必须保留其 `let` / `var` 属性。
- `.fi` 中使用内建注解 `@bounded` 表达公开 `let` 成员的显式绑定元信息; 该注解只允许出现在 `.fi` 文件中,不属于普通 `.ff` 源码可手写语法。
- 无参数形式 `@bounded` 只能标注公开 `let` 成员,表示该成员已在成员声明及绑定初值阶段完成显式绑定。
- 带参数形式 `@bounded(foo, bar)` 只能标注公开构造函数,表示该构造函数重载会在构造阶段显式绑定同一 `type` 中列出的公开 `let` 成员。
- `@bounded(...)` 的参数名必须是同一 `type` 内存在的公开 `let` 成员名,参数列表不能为空,成员名不得重复。
- 若某公开 `let` 成员已使用无参数 `@bounded` 标注,则任何构造函数上的 `@bounded(...)` 都不得再次列出该成员; 若接口元信息出现这种重复绑定冲突,则该 `.fi` 非法。
- `@bounded` 不得用于 `var` 成员、普通成员函数、普通顶层函数或非构造函数声明。
- 这些元信息用于支持包使用方在编译期检查 `let` 成员是否发生重复绑定,而无需暴露具体实现表达式、值、数据本身或函数实现。

`.fi` 中的元信息示例:

```feng
pu type User {
    pu var name: string;

    @bounded
    pu let id: int;

    pu let created_at: int;

    @bounded(created_at)
    pu fn User(ts: int);

    pu fn get_info(): string;
}
```

上例表示:

- `id` 已在成员声明及绑定初值阶段完成显式绑定。
- `User(ts: int)` 会在构造阶段显式绑定 `created_at`。
- 若某个构造函数再写成 `@bounded(id, created_at)`,则因为 `id` 已在声明阶段绑定,该接口元信息应判为非法。

## 6 编译与使用流程

### 6.1 发布方流程

1. 编译器扫描所有 `.ff` 源文件,按 `mod` 声明分组,将同一模块的全部公开声明合并
2. 按模块名层级将每个模块的公有接口输出为对应路径的 `.fi` 文件,放入 `mod/` 目录
3. 将 feng 普通 `type` / `fn` / 模块级 `let` / `var` 实现编译为 C → 生成静态库,放入 `lib/` 对应平台子目录（若 `abi` 含 `feng`）
4. 将公开的 `@fixed` 接口编译为 C ABI 兼容代码 → 生成动态库/静态库和 C 头文件,放入 `clib/` 与 `include/`（若 `abi` 含 `c`）; 公开 `extern fn` 导入声明则保留其原生库来源与调用方式元信息
5. 生成 `feng.fm`,打包为 `.fb`

### 6.2 feng 使用方流程

1. `use` 公开模块名 → 编译器按模块名推导路径,定位 `mod/` 下对应 `.fi`
2. 解析 `.fi` 中公有声明及其注解元信息,将公开 `type`、公开顶层 `fn` 与公开模块级 `let` / `var` 引入当前模块作用域
3. 根据声明关键字与 `@fixed` / `extern` 元信息自动选择链接目标: 普通 `type`/`fn`/模块级 `let` / `var` 链 `lib/`,公开 `@fixed` 接口链 `clib/`,公开 `extern fn` 导入声明则补充所需原生库链接信息
4. 使用方无需手写带参 `@cdecl` / `@stdcall` / `@fastcall`,包内 `.fi` 已携带对应原生库来源与调用方式元信息

### 6.3 外部语言使用方流程

1. 链接 `clib/` 下对应平台库文件
2. 引入 `include/` 下自动生成的 C 头文件
3. 按 C 标准方式调用

## 7 约束规则

- `mod/` 下的 `.fi` 文件和 `include/` 下的头文件均属编译器自动生成的接口描述产物,不得手动修改; 一旦其内容与包内真实实现不一致,后续编译、链接或运行行为均不再受语言规范保证,可能表现为编译失败、链接失败、ABI 不匹配或不可预期的运行时异常
- `abi` 字段声明必须与包内实际目录结构一致: 声明了 `feng` 则 `lib/` 必须存在,声明了 `c` 则 `clib/` 和 `include/` 必须存在,否则该包非法
- C ABI 层的直接布局仍仅支持 ABI 稳定类型; 公开的可调用 ABI 接口对 `string` 与数组的使用统一遵循 [Feng 内建类型规范](./feng-builtin-type.md) 中的桥接规则; 进入 ABI 边界的 `@fixed fn` / `@fixed` 方法禁止捕获变量、使用闭包
- 公开导出到 `clib/` 的接口以 `pu @fixed` 声明为准,头文件内容由其自动生成; 若公开 `extern fn` 属于导入型外部函数声明,生成的 `.fi` 必须保留其带参 `@cdecl` / `@stdcall` / `@fastcall` 元信息,以便使用方无需手写这些注解即可完成编译与链接
- 不同 feng 编译器版本编译的包不兼容
- 若包中公开 `type` 包含 `let` 成员,则编译产物必须按 `@bounded` 规则携带其显式绑定状态相关元信息,以保证使用方仍可在编译期完成对象字面量与构造路径的重复绑定检查
- 私有成员（`pr`）与私有模块级绑定不写入接口文件,完全隐藏
- `clib/` 支持动态库热更新、静态库全量嵌入

## 8 包导入语法

`.fb` 包的导入语法与包内携带哪些能力层无关,统一使用模块名:

```feng
use mylib;
use mylib.user;
```

编译器根据 `.fi` 中的声明关键字自动决定链接策略,使用方无需感知包内具体层结构。

## 9 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、包系统概要、模块、类型、函数、C 互操作、流程控制、自动内存管理与完整示例。
- 本文档: `.fb` 包格式、三层结构、`feng.fm` 清单、编译流程、`.fi` 生成规则、使用规则和导入语法的独立补充文档。
