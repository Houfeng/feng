# Feng 语言包分发规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的包分发与二进制复用概要说明,聚焦 `.fb` 统一包格式的结构定义、编译流程和使用规则。

## 1 包系统概览

Feng 采用单一标准化包格式 `.fb`（FBundle）,通过包内目录结构的有无来表达所携带的能力层:

- `feng.fm` + `mod/`: 必须存在,记录包元信息与全部公开接口声明
- `lib/`: 可选,携带自有 ABI 静态库,供 feng 项目间复用
- `clib/` + `include/`: 可选,携带 C ABI 兼容库与头文件,供跨语言复用

一个 `.fb` 包可以同时携带两个可选层,也可以只携带其中一层,由 `feng.fm` 中的 `abi` 字段显式声明。所有模式均支持闭源分发、压缩归档与编译加速,不破坏 feng → C 编译架构。

## 2 包格式定义

- 扩展名: `.fb`（FBundle）
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

包 manifest 文件扩展名为 `.fm`,文件名固定为 `feng.fm`,记录包名、版本、支持平台、携带能力层和直接依赖,编译器自动识别。

规则说明:

- `feng.fm` 是包级元信息,不是模块声明文件; 一个包可以包含多个公开模块。
- 包名用于分发、安装与包管理,不要求与任何具体模块名相同,也不要求一个包只对应一个模块。
- `abi` 字段声明本包携带哪些能力层,取值为 `feng`、`c` 或 `feng,c`; `feng` 表示存在 `lib/` 目录,`c` 表示存在 `clib/` 与 `include/` 目录; 该字段必须与包内实际目录结构一致,声明与实际不符则该包非法。
- `dependency` 表示当前包对其他 feng 包的直接依赖; 同包内模块之间的引用不属于 `dependency`。
- `dependency` 可重复出现零次到多次,每一行表示一个直接依赖包; 同一依赖包名不得重复出现。
- 当前规范中的 `dependency` 只描述 feng 包依赖,不描述系统库、动态库、C 头文件或其他原生平台依赖。
- `feng.fm` 采用固定字段的简洁键值文本格式,不使用 YAML; 原因是字段集合稳定、语法更简单、解析成本更低,也避免引入缩进、锚点等与包加载无关的复杂性。

示例（同时携带两层）:

```text
name:mylib
version:1.0.0
arch:linux-x64,windows-x64,macos-arm64
abi:feng,c
dependency:base@^1.0.0
dependency:json@^2.1.0
```

示例（仅携带自有 ABI 层）:

```text
name:mylib
version:1.0.0
arch:linux-x64,windows-x64,macos-arm64
abi:feng
dependency:base@^1.0.0
```

## 5 接口声明目录与 `.fi` 文件

### 5.1 生成规则

公开接口描述文件使用 `.fi` 扩展名,由 feng 编译器自动生成,统一放在包内固定的 `mod/` 目录下,包含公有签名以及编译期语义检查所需的初始化元信息,但不包含实现逻辑。

规则说明:

- Feng 源码中,一个模块可以分散在多个 `.f` 文件中,源文件路径与模块名无关。编译器在生成 `.fi` 时负责将同一模块的所有公开声明合并,每个公开模块恰好输出一个 `.fi` 文件。
- `.fi` 文件路径由模块名唯一决定,与源文件分布无关: 模块 `mylib.user` 固定输出到 `mod/mylib/user.fi`。编译器按此规则输出,使用方按此规则定位,无需额外索引。
- `use` 仍然以模块名为导入目标; 编译器按模块名推导路径、定位对应 `.fi`,解析其公有声明后根据声明关键字决定链接目标,最后将公开 `type` 与公开顶层 `fn` 引入当前模块作用域。
- 一个 `.fi` 文件中可以同时包含 `type`/`fn` 声明和 `extern type`/`extern fn` 声明,编译器按关键字区分链接目标,两者语义不冲突。

### 5.2 `.fi` 文件示例

`mod/mylib/api.fi`（同时包含两类声明）:

```feng
pu mod mylib.api;

pu type User {
    pu var name: string;
    pu var age: int;
    pu fn get_info(): string;
}

pu fn add_user(u: User): bool;

pu extern type Buffer {
    pu var data: *byte;
    pu var size: int;
}

@cdecl("mylib")
pu extern fn buf_alloc(size: int): Buffer;
```

### 5.3 初始化元信息要求

- 对公开 `type` 的成员,接口元信息必须保留其 `let` / `var` 属性。
- `.fi` 中使用内建注解 `@bounded` 表达公开 `let` 成员的显式绑定元信息; 该注解只允许出现在 `.fi` 文件中,不属于普通 `.f` 源码可手写语法。
- 无参数形式 `@bounded` 只能标注公开 `let` 成员,表示该成员已在类型声明初值阶段完成显式绑定。
- 带参数形式 `@bounded(foo, bar)` 只能标注公开构造函数,表示该构造函数重载会在构造阶段显式绑定同一 `type` 中列出的公开 `let` 成员。
- `@bounded(...)` 的参数名必须是同一 `type` 内存在的公开 `let` 成员名,参数列表不能为空,成员名不得重复。
- 若某公开 `let` 成员已使用无参数 `@bounded` 标注,则任何构造函数上的 `@bounded(...)` 都不得再次列出该成员; 若接口元信息出现这种重复绑定冲突,则该 `.fi` 非法。
- `@bounded` 不得用于 `var` 成员、普通成员函数、普通顶层函数或非构造函数声明。
- 这些元信息用于支持包使用方在编译期检查 `let` 成员是否发生重复绑定,而无需暴露具体实现表达式。

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

- `id` 已在成员声明初值阶段完成显式绑定。
- `User(ts: int)` 会在构造阶段显式绑定 `created_at`。
- 若某个构造函数再写成 `@bounded(id, created_at)`,则因为 `id` 已在声明阶段绑定,该接口元信息应判为非法。

## 6 编译与使用流程

### 6.1 发布方流程

1. 编译器扫描所有 `.f` 源文件,按 `mod` 声明分组,将同一模块的全部公开声明合并
2. 按模块名层级将每个模块的公有接口输出为对应路径的 `.fi` 文件,放入 `mod/` 目录
3. 将 feng 非 `extern` 实现编译为 C → 生成静态库,放入 `lib/` 对应平台子目录（若 `abi` 含 `feng`）
4. 将 `extern` 接口编译为 C ABI 兼容代码 → 生成动态库/静态库和 C 头文件,放入 `clib/` 与 `include/`（若 `abi` 含 `c`）
5. 生成 `feng.fm`,打包为 `.fb`

### 6.2 feng 使用方流程

1. `use` 公开模块名 → 编译器按模块名推导路径,定位 `mod/` 下对应 `.fi`
2. 解析 `.fi` 中公有声明,将公开 `type` 与公开顶层 `fn` 引入当前模块作用域
3. 根据声明关键字自动选择链接目标: `type`/`fn` 链 `lib/`,`extern type`/`extern fn` 链 `clib/`
4. 使用方无需手写带参 `@cdecl` / `@stdcall` / `@fastcall`,包内 `.fi` 已携带对应原生库来源与调用方式元信息

### 6.3 外部语言使用方流程

1. 链接 `clib/` 下对应平台库文件
2. 引入 `include/` 下自动生成的 C 头文件
3. 按 C 标准方式调用

## 7 约束规则

- `mod/` 下的 `.fi` 文件和 `include/` 下的头文件均属编译器自动生成的接口描述产物,不得手动修改; 一旦其内容与包内真实实现不一致,后续编译、链接或运行行为均不再受语言规范保证,可能表现为编译失败、链接失败、ABI 不匹配或不可预期的运行时异常
- `abi` 字段声明必须与包内实际目录结构一致: 声明了 `feng` 则 `lib/` 必须存在,声明了 `c` 则 `clib/` 和 `include/` 必须存在,否则该包非法
- C ABI 层（`clib/`）仅支持 C 兼容类型,禁止使用 GC 托管类型（如 `string` 和动态数组）; `extern` 函数禁止捕获变量、使用闭包
- 公开导出到 `clib/` 的接口以 `pu extern` 声明为准,头文件内容由其自动生成; 若公开 `extern fn` 属于导入型外部函数声明,生成的 `.fi` 必须保留其带参 `@cdecl` / `@stdcall` / `@fastcall` 元信息,以便使用方无需手写这些注解即可完成编译与链接
- 不同 feng 编译器版本编译的包不兼容
- 若包中公开 `type` 包含 `let` 成员,则编译产物必须按 `@bounded` 规则携带其显式绑定状态相关元信息,以保证使用方仍可在编译期完成对象字面量与构造路径的重复绑定检查
- 私有成员（`pr`）不写入接口文件,完全隐藏
- `clib/` 支持动态库热更新、静态库全量嵌入

## 8 包导入语法

`.fb` 包的导入语法与包内携带哪些能力层无关,统一使用模块名:

```feng
use mylib;
use mylib.user;
```

编译器根据 `.fi` 中的声明关键字自动决定链接策略,使用方无需感知包内具体层结构。

## 9 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、包系统概要、模块、类型、函数、C 互操作、流程控制、GC 与完整示例。
- 本文档: `.fb` 包格式、三层结构、`feng.fm` 清单、编译流程、`.fi` 生成规则、使用规则和导入语法的独立补充文档。
