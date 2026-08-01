# Feng LSP Hover 提示优化方案

> 状态：已实施
>
> 日期：2026-07-17
>
> 关联文档：
> - [Feng LSP 已交付方案](feng-lsp-delivered.md)：定义 LSP 已交付能力与语义行为基线。
> - [Feng LSP 性能优化方案](feng-lsp-performance-optimize.md)：定义最后一次成功分析、交互请求只读缓存与 Hover 性能门槛。
> - [Feng 语言类型规范](../docs/feng-type.md)：定义普通 `type` 的语言规则。
> - [Feng 语言元组规范](../docs/feng-tuple.md)：定义具名元组声明与元素规则。
> - [Feng 语言 `spec` 规范](../docs/feng-spec.md)：定义 `spec` 的语言规则。

本文档是 Feng LSP 类型类别 Hover 展示的主方案。缓存、调度和性能指标统一引用
`feng-lsp-performance-optimize.md`，本文不重复定义。

---

## 1. 背景与目标

当前 Hover 能显示声明、绑定、参数、字段、函数和方法的基础签名，但类型声明统一显示为
`type Name`，`spec` 声明统一显示为 `spec Name`。以下语义类别因而无法直接区分：

- 普通 `type`；
- 具名元组；
- `@value type`；
- object-form、callback-form、union-form、intersection-form `spec`。

本方案的目标是：

1. Hover 类型或 `spec` 的声明与引用时，显示“Feng 声明核心形状 + 明确类别标签”。
2. Hover 绑定、参数和成员时，在原有签名之外显示其静态类型类别。
3. 当前文件与跨文件、跨模块结果必须一致，不允许通过源码文本或成员名称猜测类别。
4. Hover 只读取当前文档状态、最后一次成功分析和持久符号索引，不得触发同步分析或磁盘读取。
5. 不显示任何声明注解；类别来自语义数据，不通过选择性展示 `@value` 等注解表达。

本方案不改变 Feng 语言语义、诊断、符号文件格式或编译行为。

---

## 2. 展示模型

Hover 内容按以下固定顺序组织：

1. Feng 代码块：目标自身的签名或声明核心形状；
2. 类别行：目标静态类型对应的 `Kind` 标签；
3. 文档区：原有文档注释，没有文档时省略。

Markdown 客户端示例：

````markdown
```feng
let user: User
```

**Kind:** `Reference Type`

用户信息。
````

纯文本客户端使用等价布局：

```text
let user: User

Kind: Reference Type

用户信息。
```

类别行不得放入 Feng 代码块，避免把 LSP 元信息伪装成 Feng 语法。

### 2.1 注解展示规则

声明核心形状不显示任何注解，包括但不限于 `@value`、`@abi` 以及未来新增的注解。
禁止只为区分 Value Type 而选择性显示 `@value`。

例如 `@value type Point { ... }` 的 Hover 应显示：

```feng
type Point {...}
```

**Kind:** `Value Type`

普通 `type` 使用相同的声明语法形状，由类别标签表达差异：

```feng
type User {...}
```

**Kind:** `Reference Type`

### 2.2 块体省略规则

普通 `type`、Value Type 与 Object Spec 不展开块体成员：

- 声明确实没有成员时显示 `{}`；
- 声明存在成员但被省略时显示 `{...}`。

不得显示 `{}` 来代表被省略的非空块体，也不得把完整字段、方法或继承成员集合展开到类型 Hover 中。

### 2.3 紧凑声明完整展示规则

以下声明的主体本身是有界或紧凑的类型签名，应完整展示：

- Tuple Type 的有序元素类型列表；
- Callback Spec 的参数与返回类型；
- Union Spec 的直接 member 列表；
- Intersection Spec 的直接 member 列表。

不得递归展开元素类型、union member、intersection member 或其继承闭包。

### 2.4 类型名称展示规则

Hover 的 Feng 代码块只显示命名类型路径的最后一段，不显示模块限定的
full name。该规则递归适用于泛型参数、泛型约束、数组元素、指针目标、参数类型与返回类型。

例如，符号索引中保存的 `std.json.JsonSerializable<T>` 在 Hover 中显示为：

```feng
JsonSerializable<T>
```

当前阶段不根据可见性或同名歧义恢复模块限定；Hover 一律使用短名称。该规则仅改变
Hover 表现，不改变源码、名称解析、符号索引、Completion、Signature Help、诊断或编译行为。

---

## 3. 类别标签

Hover 使用以下面向用户的完整英文标签，不使用 `Val`、`Ref` 等缩写：

| 语义类别 | Hover 标签 |
| --- | --- |
| 普通 `type` | `Reference Type` |
| `@value type` | `Value Type` |
| 具名元组 | `Tuple Type` |
| `enum` 及 enum item | `Enum` |
| object-form spec | `Object Spec` |
| callback-form spec | `Callback Spec` |
| union-form spec | `Union Spec` |
| intersection-form spec | `Intersection Spec` |
| 数组 | `Array` |
| 内建类型 | `Builtin` |
| C 指针 | `Pointer` |

四种 `spec` 的 Hover 标签省略 `-form`，正式语言规范仍可使用完整 form 术语。
当前编译器内部的 `FENG_SPEC_FORM_CALLABLE` 映射为面向用户的 `Callback Spec`；内部枚举名称不直接暴露到 Hover。

Tuple Type 虽具有值语义，仍显示独立的 `Tuple Type`，不得合并显示为 `Value Type`。

### 3.1 最外层类别

绑定、参数、字段和返回值的类别按静态类型最外层判断：

| 静态类型 | 类别 |
| --- | --- |
| `User`，且 `User` 为普通 `type` | `Reference Type` |
| `Point`，且 `Point` 为 `@value type` | `Value Type` |
| `Pair`，且 `Pair` 为具名元组 | `Tuple Type` |
| `Color`，且 `Color` 为 `enum` | `Enum` |
| `User[]` | `Array` |
| `User*` | `Pointer` |
| `Box<User>`，且 `Box` 为普通 `type` | `Reference Type` |

Hover `User[]` 中的 `User` 类型引用时，目标是 `User` 声明本身，因此显示 `Reference Type`。
无法从成功语义结果或符号索引证明的类别不得猜测；无法确定时省略类别行。

---

## 4. 类型与 `spec` 声明 Hover

### 4.1 Reference Type

保留名称、泛型参数和声明头上的 spec 关系，不展开块体成员：

```feng
type User<T>: Named, Serializable {...}
```

**Kind:** `Reference Type`

### 4.2 Value Type

不显示 `@value` 或其他注解；保留与普通 `type` 相同的声明核心形状：

```feng
type Point: Equatable {...}
```

**Kind:** `Value Type`

### 4.3 Tuple Type

完整显示声明中的有序元素类型。元组最多 8 个元素，因此展示与格式化开销有明确上限：

```feng
type Point(float, float);
```

**Kind:** `Tuple Type`

泛型、空元组和声明头 spec 关系按真实核心形状显示：

```feng
type Unit();
type Pair<T>(T, T): Equatable;
```

### 4.4 Enum

Enum 声明显示声明头，不展开 enum item：

```feng
enum Color
```

**Kind:** `Enum`

Hover enum item 时显示 item 自身名称及其确定的整数值：

```feng
Red = 1
```

**Kind:** `Enum`

### 4.5 Object Spec

保留名称、泛型参数和父 spec 列表，不展开字段、方法或继承成员：

```feng
spec Readable<T>: Named {...}
```

**Kind:** `Object Spec`

### 4.6 Callback Spec

完整显示参数与返回类型：

```feng
spec Mapper<T>(value: T): string;
```

**Kind:** `Callback Spec`

### 4.7 Union Spec

完整显示直接 member 列表，不展开嵌套 union：

```feng
spec Result: Success | Failure;
```

**Kind:** `Union Spec`

### 4.8 Intersection Spec

完整显示直接 member 列表，不展开 member 的父 spec 或成员集合：

```feng
spec ReadWrite: Readable & Writable;
```

**Kind:** `Intersection Spec`

---

## 5. 绑定、参数与成员 Hover

目标自身的原有签名保持不变，类别行统一使用 `Kind`，避免在
`spec` 声明下方使用 `Type kind` 造成语义歧义。参数在 Feng 签名前增加
`param` 紧凑角色前缀，与 `ctor` 等 Hover 签名前缀一样与 `let` / `var` 同行显示。
`param` 只是 Hover 表现层元信息，不新增 Feng 语法或关键字。

| Hover 目标 | 签名前缀 | 类别行 |
| --- | --- | --- |
| 全局或局部绑定 | 无 | `Kind: Reference Type` |
| 参数 | `param` | `Kind: Value Type` |
| 字段，包括元组 `item1`～`item8` | 无 | `Kind: Builtin` |
| enum item | 无 | `Kind: Enum` |
| 函数或方法 | 无 | 不显示 |
| 构造方法或类型构造调用 | `ctor` 或类型声明形状 | 被构造类型的 `Kind` |

### 5.1 绑定

```feng
let user: User
```

**Kind:** `Reference Type`

显式类型使用声明类型；省略类型的绑定使用最后一次成功分析中已经证明的推导静态类型。

### 5.2 参数

```feng
param let point: Point
```

**Kind:** `Value Type`

参数原有 `let` / `var` 展示规则保持不变，仅在最前面增加 `param` 表现前缀。

### 5.3 字段与元组元素

```feng
let owner: User
```

**Kind:** `Reference Type`

元组元素沿用字段 Hover：

```feng
let item1: float
```

**Kind:** `Builtin`

### 5.4 函数与方法

```feng
func create(): User
```

函数和方法签名已经明确显示返回类型，不再额外显示返回类型 `Kind`。

### 5.5 构造方法与类型构造调用

构造方法声明以及 `UserType()` 等类型构造调用显示被构造类型的类别：

```feng
ctor UserType(): void
```

**Kind:** `Reference Type`

`UserType {}` 与 `UserType()` 可以采用各自已有的签名形状，但必须显示相同的类型类别。
该规则不适用于返回 `UserType` 的普通函数；普通函数仍按 5.4 节不显示返回类型类别。

---

## 6. 数据来源与跨模块一致性

### 6.1 当前文件与最后一次成功分析

当前文件成功 AST 已包含：

- `type_decl.is_tuple`；
- `type_decl.is_value`；
- `spec_decl.form`；
- 元组元素字段；
- Callback Spec 参数与返回类型；
- Union Spec 与 Intersection Spec 的直接 member 列表。

当前文档存在未完成输入时，Hover 继续遵守最后一次成功分析规则。失败、取消或过期分析不得清除成功结果，也不得用失败结果覆盖类别信息。

### 6.2 跨模块符号索引

符号文件已经保存 tuple、value 和 spec form 标志，且已保存各紧凑声明所需的成员与类型信息。本方案不修改符号文件格式。

现有 symbol provider 已公开 spec form、Callback Spec 参数与返回类型、Union Spec member、Intersection Spec member 和普通成员查询，但尚未公开 tuple/value 标志。为保证跨模块结果与当前文件一致，实施时应新增只读接口：

```c
/* Return whether a symbol declaration is a named tuple type. */
bool feng_symbol_decl_is_tuple(const FengSymbolDeclView *decl);

/* Return whether a symbol declaration is an @value type. */
bool feng_symbol_decl_is_value_type(const FengSymbolDeclView *decl);
```

该变更只暴露符号缓存中已经存在的信息，不改变 parser、semantic、codegen 或符号文件格式。
禁止根据 `item1` 等成员名称推断 tuple，也禁止通过重新读取依赖源码判断 `@value type`。

### 6.3 修改范围

预计实施范围：

- `docs/feng-spec.md`：在编码前收敛四种 spec form 的主规范描述；
- `src/symbol/provider.h`、`src/symbol/provider.c`：增加 tuple/value 只读查询；
- `src/cli/lsp/`：类别解析、声明核心形状格式化和 Hover 渲染；
- 新增 LSP Hover 专项测试；
- 本文档：记录实施状态与验收结果。

因此，完整且跨模块一致的实现不能只修改 `src/cli/lsp/`。`src/symbol/` 的变更仅限公开已有只读信息，不扩大到符号格式或编译语义。

---

## 7. LSP 内部设计

### 7.1 统一类别模型

LSP 应定义统一的内部类别枚举，由 AST 目标和 symbol provider 目标共同映射，避免当前文件与缓存路径分别拼接标签：

```c
typedef enum FengLspTypeCategory {
    FENG_LSP_TYPE_CATEGORY_UNKNOWN = 0,
    FENG_LSP_TYPE_CATEGORY_REFERENCE,
    FENG_LSP_TYPE_CATEGORY_VALUE,
    FENG_LSP_TYPE_CATEGORY_TUPLE,
    FENG_LSP_TYPE_CATEGORY_OBJECT_SPEC,
    FENG_LSP_TYPE_CATEGORY_CALLBACK_SPEC,
    FENG_LSP_TYPE_CATEGORY_UNION_SPEC,
    FENG_LSP_TYPE_CATEGORY_INTERSECTION_SPEC,
    FENG_LSP_TYPE_CATEGORY_ARRAY,
    FENG_LSP_TYPE_CATEGORY_BUILTIN,
    FENG_LSP_TYPE_CATEGORY_POINTER
} FengLspTypeCategory;
```

枚举到用户标签的映射必须集中定义，不得在不同 Hover 分支重复硬编码字符串。

### 7.2 结构化 Hover 表现

签名、类别和文档应先形成结构化的 LSP 内部结果，再分别渲染 Markdown 与 plaintext。不得继续依赖在一个字符串中查找空行来猜测哪一段是签名、类别或文档。

建议内部模型至少包含：

```c
typedef struct FengLspHoverPresentation {
    FengLspString signature;
    const char *category_caption;
    const char *category_label;
    char *documentation;
} FengLspHoverPresentation;
```

所有新增结构体、枚举和函数必须按仓库规范添加职责注释。具体文件拆分由实施阶段依据现有 LSP 组织确定，但 AST 路径与持久符号路径必须复用同一类别标签与渲染层。

### 7.3 声明格式化

声明格式化应由声明类别驱动：

1. 先确定 `FengLspTypeCategory`；
2. 再选择块体省略、tuple、callback、union 或 intersection 格式化策略；
3. 最后附加类别和文档。

禁止为具体类型名、模块名、成员名或项目路径添加特判。

---

## 8. 性能与缓存要求

本功能必须继续满足 `feng-lsp-performance-optimize.md` 的全部不变量和指标：

1. Hover 请求不得同步启动 parser、semantic 或整项目分析。
2. Hover 请求不得同步读取源码、manifest、依赖或符号文件。
3. 类别解析只读取当前已发布 AST、最后一次成功分析或已加载 symbol provider。
4. 普通 type、Value Type 和 Object Spec 不展开成员集合。
5. Tuple Type 最多格式化 8 个直接元素。
6. Callback、Union 和 Intersection 只格式化声明中的直接项，不递归展开。
7. 不为类别标签建立多版本缓存；已有 Hover 缓存可直接缓存最终表现结果。
8. 无法证明类别时省略标签，不得进入慢路径猜测。

本功能不得引入用户可感知的 Hover 延迟，也不得改变最后一次成功分析的保留与替换规则。

---

## 9. 测试方案

未经人工批准不得修改已有测试用例。实施时优先新增专项测试，并执行完整回归。

### 9.1 声明 Hover

至少覆盖：

- 有成员和无成员的 Reference Type；
- 有成员和无成员的 Value Type，且签名中不出现 `@value`；
- 同一声明同时含 `@value`、`@abi` 或其他注解时，签名中不出现任何注解；
- 0、2、8 元素 Tuple Type；
- 泛型 Tuple Type 与 tuple 声明头 spec 关系；
- 有成员、无成员和有父 spec 的 Object Spec；
- 泛型 Callback Spec 的参数与返回类型；
- Union Spec 的直接 member 顺序；
- Intersection Spec 的直接 member 顺序；
- Markdown 与 plaintext 两种客户端能力。

### 9.2 使用位置 Hover

至少覆盖：

- 显式类型和推导类型的全局、局部绑定；
- 参数；
- 普通字段；
- 元组 `item1`～`item8` 字段；
- 函数和方法不额外显示返回类型 `Kind`；
- Array、Builtin、Pointer 与泛型具名类型的最外层类别；
- Hover 类型引用时显示被引用声明自身的类别。

### 9.3 跨模块与缓存

至少覆盖：

- 依赖模块中的 Reference、Value、Tuple 和四种 Spec 与当前文件显示一致；
- 只依赖已加载 symbol provider，不读取依赖源码；
- 当前文件出现不完整输入后仍可使用最后一次成功分析显示正确类别；
- 后台分析失败、取消或过期后，类别信息不被清空；
- 无法证明类别时省略类别，不显示错误标签。

### 9.4 性能与回归

至少执行：

- Hover 专项协议测试；
- LSP 性能矩阵与调度测试；
- `make test-normal`；
- `make test-sanitize`；
- VS Code 插件测试。

专项性能测试必须确认新增类别格式化不触发同步分析或磁盘 I/O，并继续满足 LSP 性能主规范中的 Hover P95 与交互请求 P99 门槛。

---

## 10. 实施顺序与验收标准

### 10.1 实施顺序

1. 收敛 `docs/feng-spec.md` 中四种 spec form 及其术语。
2. 在 symbol provider 中公开 tuple/value 只读标志。
3. 在 LSP 中建立统一类别枚举和标签映射。
4. 统一 AST 与持久符号两条声明核心形状格式化路径。
5. 结构化构建签名、类别和文档，并渲染 Markdown/plaintext。
6. 为绑定、参数和字段附加 `Kind`，并在参数签名前同行显示 `param`。
7. 新增专项正确性、跨模块、缓存与性能测试。
8. 执行完整回归并将结果记录到本文档。

### 10.2 验收标准

全部满足以下条件后方可视为完成：

- 类型和 `spec` 声明/引用 Hover 均显示声明核心形状与正确类别；
- enum 声明、引用及 enum item 均显示 `Kind: Enum`；
- 绑定、参数和字段显示 `Kind`，参数签名以 `param let/var` 开头；
- 类型构造调用与构造方法显示被构造类型的 `Kind`；
- 函数和方法不额外显示返回类型 `Kind`；
- 所有注解在声明核心形状中一致省略；
- 非空块体显示 `{...}`，真实空块体显示 `{}`；
- Tuple、Callback、Union、Intersection 按本方案显示完整直接签名；
- 当前文件、跨文件和跨模块结果一致；
- 不通过源码文本或成员名称猜测类别；
- 不触发同步分析或磁盘 I/O；
- Markdown/plaintext 与文档注释布局正确；
- 专项测试、全量回归与性能验收完成，既有基线失败如实记录。

---

## 11. 实施与验收结果

2026-07-17 完成实施：

- 已统一 AST 与持久符号索引两条 Hover 路径的声明形状、类别映射与渲染；
- 已实现 Reference、Value、Tuple、Enum、四种 Spec、Array、Builtin 与 Pointer 类别；
- enum item 及 `UserType()` / `UserType {}` 类型构造形式显示一致的类型类别；
- 参数签名以同行的 `param let/var` 开头，类别行统一为 `Kind`；
- 函数和方法不额外显示返回类型 `Kind`；
- Hover 中的命名类型统一显示短名称，并递归应用于泛型、参数和返回类型；
- 非空声明块体统一显示为 `{...}`，真实空块体仍显示 `{}`；
- 不完整输入继续使用最后一次成功分析，失败分析不覆盖已成功结果。

验收结果：

- Hover 专项协议测试：普通构建与 UBSan 均通过；
- 跨模块符号 Hover、失败编辑缓存保留与既有参数缓存失效测试：通过；
- `test_symbol`、LSP 调度、LSP 缓存保留和性能约束：通过；
- LSP 性能复测：Hover P95 为 `0.049 ms`，Completion P95 为 `0.041 ms`；
  100 万行热 Hover P95 为 `0.033 ms`，矩阵交互请求 P99 为 `0.197 ms`；
- `make test-normal`：88 项 smoke、542 项 FCTS、标准库、perf constraints 与 CLI 之前的
  编译器单元套件均通过；在既有 CLI/LLDB 子进程测试处因
  `process exited with status -1 (no such process)` 中止；
- `make test-sanitize`：UBSan 下的 archive、lexer、parser、semantic、runtime、codegen、debug
  及 Hover 专项测试均通过；同样在上述 CLI/LLDB 子进程测试处中止；
- VS Code 插件 formatter、diagnostics、debug integration 和 syntax 通过；
  既有 debug smoke 在断点验证处失败，icon 测试存在
  `icons/feng-logo.png` 与 `./icons/feng-logo.png` 的既有预期不一致。
