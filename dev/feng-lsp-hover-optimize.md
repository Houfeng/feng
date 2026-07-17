# Feng LSP Hover 提示优化方案

> 状态：方案已确定，待实施
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
2. 类别行：目标静态类型对应的类别标签；
3. 文档区：原有文档注释，没有文档时省略。

Markdown 客户端示例：

````markdown
```feng
let user: User
```

**Type kind:** `Reference Type`

用户信息。
````

纯文本客户端使用等价布局：

```text
let user: User

Type kind: Reference Type

用户信息。
```

类别行不得放入 Feng 代码块，避免把 LSP 元信息伪装成 Feng 语法。

### 2.1 注解展示规则

声明核心形状不显示任何注解，包括但不限于 `@value`、`@abi` 以及未来新增的注解。
禁止只为区分 Value Type 而选择性显示 `@value`。

例如 `@value type Point { ... }` 的 Hover 应显示：

```feng
type Point { ... }
```

**Type kind:** `Value Type`

普通 `type` 使用相同的声明语法形状，由类别标签表达差异：

```feng
type User { ... }
```

**Type kind:** `Reference Type`

### 2.2 块体省略规则

普通 `type`、Value Type 与 Object Spec 不展开块体成员：

- 声明确实没有成员时显示 `{}`；
- 声明存在成员但被省略时显示 `{ ... }`。

不得显示 `{}` 来代表被省略的非空块体，也不得把完整字段、方法或继承成员集合展开到类型 Hover 中。

### 2.3 紧凑声明完整展示规则

以下声明的主体本身是有界或紧凑的类型签名，应完整展示：

- Tuple Type 的有序元素类型列表；
- Callback Spec 的参数与返回类型；
- Union Spec 的直接 member 列表；
- Intersection Spec 的直接 member 列表。

不得递归展开元素类型、union member、intersection member 或其继承闭包。

---

## 3. 类别标签

Hover 使用以下面向用户的完整英文标签，不使用 `Val`、`Ref` 等缩写：

| 语义类别 | Hover 标签 |
| --- | --- |
| 普通 `type` | `Reference Type` |
| `@value type` | `Value Type` |
| 具名元组 | `Tuple Type` |
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
type User<T>: Named, Serializable { ... }
```

**Type kind:** `Reference Type`

### 4.2 Value Type

不显示 `@value` 或其他注解；保留与普通 `type` 相同的声明核心形状：

```feng
type Point: Equatable { ... }
```

**Type kind:** `Value Type`

### 4.3 Tuple Type

完整显示声明中的有序元素类型。元组最多 8 个元素，因此展示与格式化开销有明确上限：

```feng
type Point(float, float);
```

**Type kind:** `Tuple Type`

泛型、空元组和声明头 spec 关系按真实核心形状显示：

```feng
type Unit();
type Pair<T>(T, T): Equatable;
```

### 4.4 Object Spec

保留名称、泛型参数和父 spec 列表，不展开字段、方法或继承成员：

```feng
spec Readable<T>: Named { ... }
```

**Type kind:** `Object Spec`

### 4.5 Callback Spec

完整显示参数与返回类型：

```feng
spec Mapper<T>(value: T): string;
```

**Type kind:** `Callback Spec`

### 4.6 Union Spec

完整显示直接 member 列表，不展开嵌套 union：

```feng
spec Result: Success | Failure;
```

**Type kind:** `Union Spec`

### 4.7 Intersection Spec

完整显示直接 member 列表，不展开 member 的父 spec 或成员集合：

```feng
spec ReadWrite: Readable & Writable;
```

**Type kind:** `Intersection Spec`

---

## 5. 绑定、参数与成员 Hover

目标自身的原有签名保持不变，类别行说明该目标静态类型的类别。类别标题必须明确所描述的对象，禁止统一使用含义模糊的 `Kind`。

| Hover 目标 | 类别标题 | 示例 |
| --- | --- | --- |
| 全局或局部绑定 | `Type kind` | `Type kind: Reference Type` |
| 参数 | `Parameter type kind` | `Parameter type kind: Value Type` |
| 字段，包括元组 `item1`～`item8` | `Field type kind` | `Field type kind: Builtin` |
| 函数或方法 | `Return type kind` | `Return type kind: Reference Type` |

### 5.1 绑定

```feng
let user: User
```

**Type kind:** `Reference Type`

显式类型使用声明类型；省略类型的绑定使用最后一次成功分析中已经证明的推导静态类型。

### 5.2 参数

```feng
let point: Point
```

**Parameter type kind:** `Value Type`

参数原有 `let` / `var` 展示规则保持不变。

### 5.3 字段与元组元素

```feng
let owner: User
```

**Field type kind:** `Reference Type`

元组元素沿用字段 Hover：

```feng
let item1: float
```

**Field type kind:** `Builtin`

### 5.4 函数与方法

```feng
func create(): User
```

**Return type kind:** `Reference Type`

返回类型类别不得误写成函数或方法自身的类别。没有可证明返回类型的目标不显示返回类型类别。

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
- 函数和方法返回类型；
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
6. 为绑定、参数、字段和函数/方法返回类型附加明确类别标题。
7. 新增专项正确性、跨模块、缓存与性能测试。
8. 执行完整回归并将结果记录到本文档。

### 10.2 验收标准

全部满足以下条件后方可视为完成：

- 类型和 `spec` 声明/引用 Hover 均显示声明核心形状与正确类别；
- 绑定、参数、字段和函数/方法返回值显示对应的明确类别标题；
- 所有注解在声明核心形状中一致省略；
- 非空块体显示 `{ ... }`，真实空块体显示 `{}`；
- Tuple、Callback、Union、Intersection 按本方案显示完整直接签名；
- 当前文件、跨文件和跨模块结果一致；
- 不通过源码文本或成员名称猜测类别；
- 不触发同步分析或磁盘 I/O；
- Markdown/plaintext 与文档注释布局正确；
- 专项测试、全量回归与性能验收完成，既有基线失败如实记录。
