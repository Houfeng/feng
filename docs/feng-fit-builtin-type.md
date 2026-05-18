# Feng `fit` 支持内建类型草案

> **状态**: 草案。
> 本文档只定义“内建类型作为 `fit` 左侧目标”的补充规则。
> `fit` 的通用语法、通用语义、通用冲突判定、通用可见性、导出与孤儿适配规则统一见 [feng-fit.md](./feng-fit.md)。
> 内建数值、布尔、字符串与别名规则统一见 [feng-builtin-type.md](./feng-builtin-type.md)。
> 数组本体语义统一见 [feng-builtin-type.md](./feng-builtin-type.md)；数组标准库扩展能力见 [feng-std-array.md](./feng-std-array.md)。
> 符号表导出格式统一见 [feng-symbol-table.md](./feng-symbol-table.md)。
> 开发指导见 [dev/feng-fit-optimize-delivered.md](../dev/feng-fit-optimize-delivered.md)。

## 1 职责

- 定义哪些内建类型可以作为 `fit` 左侧目标。
- 定义内建类型进入 `fit` 后, `self` 的补充语义。
- 定义内建类型进入 `fit` 后的编译期与运行时补充约束。
- 明确哪些类型仍然不属于 `fit` 内建目标范围。

## 2 适用范围

本草案支持以下内建类型作为 `fit` 左侧目标:

- 标量内建类型: `i8`、`i16`、`i32`、`i64`、`u8`、`u16`、`u32`、`u64`、`f32`、`f64`、`bool`
- `string`
- 数组目标形式 `T[]` 与 `T[!]`

内建类型别名按 [feng-builtin-type.md](./feng-builtin-type.md) 的现有规则处理。例如 `int` 视为 `i32`、`float` 视为 `f32`。`fit int` 与 `fit i32` 指向同一个语言类型目标, 编译器必须按规范化后的内建类型名处理。

数组目标形式 `T[]` 与 `T[!]` 的数组层级、逐层 `!` 语义、元素类型规则与数组本体语义统一遵循 [feng-builtin-type.md](./feng-builtin-type.md)。本文只补充“数组可作为 `fit` 左侧目标”这一规则, 不重复定义数组语义本体。

其中 `T[]` 与 `T[!]` 中的 `T` 表示数组元素类型引用。`T` 可以是具体类型；当写作单个类型参数名时, 该类型参数由数组目标形式引入, 其作用域覆盖整个 `fit` 声明。

数组目标解析约定:

- `fit int[]` / `fit int[!]` 表示固定目标, 元素类型固定为 `int`。
- `fit T[]` / `fit T[!]` 仅在 `T` 未命中当前可见范围中的同名 `type`、同名类型参数且 `T` 不是内建类型名时, 才表示由 `fit` 左侧目标形式引入的局部元素类型参数（即“任意元素类型数组”）。
- 若当前可见范围已存在同名 `T`, 则 `fit T[]` / `fit T[!]` 绑定该已存在符号, 不触发局部元素类型参数引入。
- `[]` 与 `[!]` 是不同的数组目标；如需同时覆盖两者, 必须分别声明 `fit`。
- 数组目标形式引入的元素类型参数可继续出现在 `fit` 方法返回类型中,包括 `Span<T>` 这类具名泛型返回类型。具体数组实例调用该方法时,返回类型必须按接收者元素类型实例化,例如 `int[]` 上调用返回 `Span<int>`。

以下类型不在本草案范围内:

- C 指针类型。

## 3 语义

在 [feng-fit.md](./feng-fit.md) 已定义的 `fit` 通用语义基础上, 本草案增加以下补充语义:

- `fit` 左侧目标除了用户定义的具体 `type`, 还可以是本草案 §2 列出的内建类型。
- 内建类型上的 `fit` 与用户 `type` 上的 `fit` 在用户可见行为上保持一致。用户仍然通过同一套 `fit` 语法与普通方法调用语法使用这些能力。
- 内建类型上的 `self` 仍由编译器隐式提供, 仍表示“当前实例”。其中, 标量内建类型中的 `self` 是当前值本身, `string` 中的 `self` 是当前字符串值, 数组目标中的 `self` 是当前数组值。
- 在孤儿适配判定中, 所有内建类型目标都按外部类型处理, 包括标量、`string` 与数组目标形式 `T[]`、`T[!]`。因此, 当目标 `spec` 定义在当前包内时, `pu fit` 可以公开导出该关系声明；当目标 `spec` 也定义在当前包外时, `pu fit` 必须按 [feng-fit.md](./feng-fit.md) 的孤儿规则移除导出并输出肯定式提示。

以下写法应合法:

```feng
fit i32 {
  fn double(): i32 {
    return self * 2;
  }
}

fit T[] {
  fn same(): T[] {
    return self;
  }

  fn clone(): T[] {
    return self;
  }

  fn clone(start: long, length: long): T[] {
    ...
  }

  fn slice(start: long, length: long): Span<T> {
    ...
  }
}

fit int[] {
  fn first(): int {
    return self[0];
  }
}

fit T2[] {
  fn head(): T2 {
    return self[0];
  }
}

fit T[!] {
  fn readonly(): T[] {
    return (T[])self;
  }

  fn clone(): T[] {
    return (T[])self;
  }

  fn clone(start: long, length: long): T[] {
    ...
  }

  fn slice(start: long, length: long): Span<T> {
    ...
  }
}
```

以下写法继续非法:

```feng
fit *byte {
  fn hash(): u64 {
    return 0;
  }
}
```

## 4 规则

分为「必须、禁止、说明」。本节只列出内建类型进入 `fit` 后新增的补充规则。未在此列出的通用规则全部继续遵循 [feng-fit.md](./feng-fit.md)。

- [必须] `fit` 左侧使用内建类型时, 目标类型必须属于本草案 §2 列出的范围。
- [必须] `fit` 左侧使用内建类型别名时, 编译器必须按 [feng-builtin-type.md](./feng-builtin-type.md) 的既有别名规则将其归并到同一个规范化内建类型目标。
- [必须] `fit` 左侧使用数组目标形式时, 只允许 `T[]` 与 `T[!]` 两类形态；其数组层级与逐层 `!` 语义统一遵循 [feng-builtin-type.md](./feng-builtin-type.md)。
- [必须] `fit T[]` 或 `fit T[!]` 中, `T` 必须表示数组元素类型引用；当 `T` 由数组目标形式引入时, 其作用域必须覆盖整个 `fit` 声明。
- [必须] 对 `fit X[]` / `fit X[!]` 的 `X` 解析必须遵循“可见符号优先”原则: 若当前可见范围已有同名 `type` 或同名类型参数, 必须绑定已有符号；仅在未命中且 `X` 非内建类型名时, 才可把 `X` 视为由数组目标形式引入的局部元素类型参数。
- [必须] `[]` 与 `[!]` 必须视为不同 `fit` 目标；`fit X[]` 的声明不得自动覆盖 `fit X[!]`，`fit X[!]` 的声明也不得自动覆盖 `fit X[]`。
- [必须] 数组目标方法的参数类型与返回类型必须在调用点按接收者数组元素类型实例化；若返回类型包含由数组目标引入的元素类型参数,编译器不得把该返回类型保留为 open 泛型类型。
- [必须] 在孤儿适配判定中, 内建类型目标一律按外部类型处理；当目标 `spec` 在当前包内时, `pu fit` 可公开导出关系声明；当目标 `spec` 也在当前包外时, `pu fit` 不得导出。
- [必须] 内建类型上的 `fit` 在用户可见层面不得引入区别于用户 `type` 的独立调用规则或独立语义模型。
- [必须] 内建类型上的 `fit` 方法继续通过普通方法调用语法使用, 不得引入第二种用户可见调用方式。
- [必须] 数组目标上的 `clone()` 与 `clone(start, length)` 可返回新的实体数组 `T[]`；它们与返回视图类型 `Span<T>` 的 `slice(start, length)` 必须视为不同能力，不得混淆语义。
- [必须] 数组目标上的 `slice(start, length)` 可返回只读视图类型 `Span<T>`；第一阶段该方法不得隐式产生可写视图语义。
- [禁止] 将 C 指针类型作为 `fit` 左侧目标。
- [说明] 对方法重载、方法冲突、块体成员限制、私有成员访问、可见性与导出等通用规则, 统一直接适用 [feng-fit.md](./feng-fit.md), 本文不重复定义。

## 5 编译期

- 编译器必须允许 [feng-fit.md](./feng-fit.md) 中的 `fit` 通用语法以本草案 §2 列出的内建类型作为左侧目标。
- 编译器必须允许数组目标形式 `T[]` 与 `T[!]` 出现在 `fit` 左侧, 并按 [feng-builtin-type.md](./feng-builtin-type.md) 的数组类型规则解析其元素类型、数组层级与逐层 `!` 语义。
- 编译器必须在 `fit T[]` 或 `fit T[!]` 中把 `T` 解析为数组元素类型引用；当 `T` 由数组目标形式引入时, 编译器必须建立覆盖整个 `fit` 声明的类型参数作用域。
- 编译器必须把数组目标形式引入的元素类型参数作为 `fit` 方法成员体可见的泛型上下文处理,并在需要时向生成的 `fit` 方法实现转发元素类型描述符。
- 编译器必须在数组 `fit` 方法调用点对包含元素类型参数的返回类型进行实例化,例如把 `Span<T>` 具体化为 `Span<i32>`；后续成员调用、赋值、返回和 C 代码生成均必须使用实例化后的类型。
- 编译器必须在语义分析阶段拒绝 C 指针类型出现在 `fit` 左侧目标位置。
- 编译器必须在解析 `fit` 左侧目标时, 先按现有规则尝试解析用户 `type`, 未命中时再按 [feng-builtin-type.md](./feng-builtin-type.md) 的标量 / `string` / 数组规则识别内建目标。
- 编译器必须把内建类型别名规范化为同一个 canonical builtin target, 不得把 `fit int` 与 `fit i32` 视为两个不同目标。
- 编译器必须在内建类型的 `fit` 块中把 `self` 解析为对应的内建类型表达式。不得把 `self` 伪装成对象类型, 也不得要求额外用户可见载体。
- 编译器必须允许内建类型通过 `fit` 建立 object-form `spec` 契约关系, 并继续遵循现有 `spec` 满足性、冲突检测与 witness materialization 主路径。
- 编译器必须在 object-form spec coercion sidecar 中记录 subject 承载策略（临时借用或稳定 owner）, 并把该策略从语义阶段传递到 codegen 阶段；codegen 不得自行重判该站点的承载语义。
- 编译器必须在孤儿适配导出判定中把内建类型目标视为外部类型；因此当目标 `spec` 在当前包外时, `pu fit` 必须移除导出, 当目标 `spec` 在当前包内时则允许公开导出关系声明。
- 编译器必须按 [feng-symbol-table.md](./feng-symbol-table.md) 的既有类型节点规则导出内建类型 fit 目标: 标量与 `string` 使用 BUILTIN type node, 数组目标使用 ARRAY type node。不得把数组 fit target 错导为 builtin type node, 也不得把任何内建 fit target 错导为普通 named type。
- 编译器在数组 fit target 的 subject key 构建与 sidecar 查找中, 必须使用结构化数组键（元素类型结构、数组层级 rank、逐层可写位图）进行比较；不得退化为拍平文本比较或 AST 指针比较。
- 编译器导出 `fit T[]` / `fit T[!]` 的符号类型节点时, 必须保留 ARRAY 节点的元素类型引用与逐层可写位图；读取 `.ft` 后该信息必须可无损还原。
- 当数组 fit 目标元素是类型参数引用（如 `fit T[]`、`fit T[!]`）时, 元素类型必须通过 `TYPE_PARAM_REF` 节点嵌入到 ARRAY 节点中导出；不得拍平为文本类型名。

## 6 运行时

### 6.1 直接调用

- 对所有具体直接 `fit` 方法调用（无论用户类型还是内建类型），编译器必须在编译期完成静态分派，生成以目标既有表示直接传递 `self` 的静态函数调用。不得为此引入 boxing、wrapper object、额外 carrier struct 或额外 heap allocation。
- 具体直接调用的运行时开销不得大于等价的自由函数调用。即 `it.some()` 的调用成本不得高于 `some(it)`。
- 对标量内建类型, `fit` 方法的 `self` 必须按该标量类型的原生值表示传递。
- 对已具体化的泛型标量实例（例如 `Set<int>`）的 direct-call, 编译器必须走具体实例 wrapper 的单态化发码路径并以原生标量值传递参数/返回值；不得在 direct-call 路径中触发 `FengScalarBox` 分配或任何等价运行时装箱。
- 对 `string` 与数组, `fit` 方法的 `self` 必须按其现有受管引用表示传递。对用户仍表现为普通 `string` 值或普通数组值。
- 编译器不得在具体直接调用的调用路径中插入任何运行时类型查询、方法选择或 witness 解析逻辑。

### 6.2 spec 视角调用与 witness

- 当内建类型按 `spec` 视角被使用时, 运行时继续沿用现有 witness 间接调用模型。这一层间接调用只用于抽象 `spec` 调用, 不得外溢到具体直接调用。
- witness 表必须在编译期静态确定。编译器不得为 witness 表解析生成任何运行时散列查找、动态分发或 JIT 路径。
- witness materialization 入口必须统一基于 compile-time `subject key`（用户 `type` / builtin / array）进行解析，不得再拆分为仅面向用户 `type` 的独立主入口。
- 内建类型进入 witness 路径时, subject 必须携带实际值、实际字符串引用或实际数组引用。不得出现“只有 witness、subject 为空”的运行时模型。
- builtin witness thunk 只允许执行一次按目标既有表示的取值或取指针, 然后直接调用对应的内建 `fit` 方法实现。不得通过 boxing 构造新的运行时对象。
- 对 `string` 与数组 target, witness thunk 必须先从 `_subject` 做一次现有引用表示取指针, 再直接调用 fit 方法实现；不得在 thunk 中增加第二层 wrapper 或额外运行时查表。
- 对同一 `fit` 方法, direct-call 与 spec-call 必须复用同一个已发射的 `fit` 实现符号; 禁止为 spec 路径再生成独立的“箱版方法实现符号”。
- witness thunk 的函数体必须直接调用 fit 方法实现符号, 不得在 thunk 体内再次经过 `witness->...` 分派或再次转发到其他 `FengSpecThunk__...` 包装层。
- 对对象 target 的 spec thunk, `_subject` 必须保持一次 `(struct T *)_subject` 还原后直接转发到实现方法, 不得改写为其他承载或额外包装层。

### 6.3 标量 subject 稳定承载

- 当标量内建类型的 object-form `spec` 值需要逃逸到局部作用域之外时, 运行时必须提供稳定的 subject 承载。
- 该稳定承载由 runtime-internal `FengScalarBox` 提供, 仅用于内部 subject 物化, 不暴露给用户语言层。
- `FengScalarBox` 不改变 fat spec 的两字段外层 ABI, 也不改变 direct-call 的零 boxing 约束。
- 非逃逸的标量 `spec` 调用仍可借用局部物化地址, 不要求分配 `FengScalarBox`。
- 当前阶段对“非逃逸”的可证明口径收敛为: 标量到 object-form `spec` 的调用实参临时 coercion 点只在当前调用栈帧内消费 subject, 不进入局部绑定、返回值或聚合字段/元素存储。
- `FengScalarBox` 只服务于标量内建类型, 不作为 `string`、数组或用户 `type` 的通用承载模型。
- `FengScalarBox` 必须是 runtime 内单一托管对象类型, 用同一套对象头与同一套 retain/release 生命周期承载全部标量内建类型。
- `FengScalarBox` 的 payload 必须使用自然对齐的 `union` 成员布局（`bool` / 有符号整数 / 无符号整数 / 浮点）, 不得退化为字节数组 payload。
- `FengScalarBox` 必须标记为非循环对象且不包含 managed fields；其职责仅是稳定 subject ownership, 不参与额外运行时分派。
- 标量 spec thunk 必须只做一次原生类型取值后直接调用 fit 方法实现；当 coercion site 标记为临时借用时从借用地址取值, 当标记为稳定 owner 时从 `FengScalarBox` payload 取值。不得引入第二层 wrapper 或运行时查表。

## 7 关联

- [feng-fit.md](./feng-fit.md): `fit` 的通用语法、通用语义、通用规则、导出与孤儿适配。
- [feng-builtin-type.md](./feng-builtin-type.md): 内建数值、布尔、字符串、数组与别名规则。
- [feng-std-array.md](./feng-std-array.md): 数组标准库扩展与 `Span<T>` 规则。
- [feng-spec.md](./feng-spec.md): object-form `spec` 的匹配、冲突与运行时约束。
- [feng-symbol-table.md](./feng-symbol-table.md): `.ft` 中 BUILTIN / ARRAY type node 与 fit 目标导出格式。
