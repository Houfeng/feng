# Feng 联合类型草案

> **状态**: 讨论草案。
> 本文档用于整理 2026-05-05 关于联合类型的讨论结论与待定问题，**不是当前语言权威规范**。
> 若后续采纳，应把已确认规则拆分并并入 [feng-spec.md](./feng-spec.md)、[feng-language.md](./feng-language.md)、[feng-type.md](./feng-type.md) 与相关规范；本文档本身不替代这些主规范。

## 1 目标

- 在**不引入新关键字**的前提下，为 Feng 增加联合类型能力。
- 保持 `spec` 关键字的一词多用设计，不把 `spec` 绑定到单一运行时结构。
- 保持运行时顶层值模型的抽象完备性：任何新类型最终都必须落入既有三类之一，而不是新增第四类顶层运行时结构。

## 2 现状事实

以下内容是当前仓库中的既有事实，用于约束联合类型设计。

### 2.1 `spec` 当前已经是一词多用

- object-form `spec`：声明对象形契约。
- callable-form `spec`：声明可调用形状。

这说明 `spec` 关键字本身并不对应唯一运行时结构，而是一个可承载多个语义 form 的入口关键字。

### 2.2 现有 object-form `spec` 与 callable-form `spec` 的运行时结构不同

- object-form `spec` 当前实现为双指针胖值，概念上是：
  - `subject`
  - `witness`
- callable-form `spec` 当前不使用这套双指针胖值。

因此，若未来引入联合类型，也可以继续作为 `spec` 的新增 form，而不是新增关键字。

### 2.3 运行时顶层值模型当前只有三类

当前讨论将运行时顶层值模型抽象为三类：

- `trivial`
- `managed-pointer`
- `aggregate-with-managed-slots`

本次讨论已确认：这三类在抽象层面已经完备。未来新增的任何类型，包括联合类型与 tuple，都必须无例外地映射到这三类之一；不存在第四类顶层运行时结构。

## 3 本次讨论已确认的结论

### 3.1 不新增关键字，继续复用 `spec`

- 联合类型不引入新的关键字。
- `spec` 继续作为统一入口关键字。
- 若后续采纳联合类型，则它应成为 `spec` 的第三种 form。

因此，`spec` 在语义层将至少包含：

- object-form
- callable-form
- union-form

### 3.2 union-form `spec` 的候选语法

当前讨论收敛出的候选语法为：

```feng
spec UnionType: int | string | UserType;
```

约束：

- union-form 以分号 `;` 结束。
- union-form **不允许**使用 `{}` 块。

对应的候选语法骨架可写为：

```text
spec Name : TypeRef ('|' TypeRef)+ ;
```

示例：

```feng
spec ValueOrName: int | string;
spec DisplayTarget: string | User;
```

非示例：

```feng
spec ValueOrName: int | string {}
```

上例不符合本草案，因为 union-form 不允许块体。

### 3.3 逗号与竖线的语义不同，不只是写法不同

现有 object-form `spec` 的 parent spec list：

```feng
spec Child: A, B { ... }
```

本质是 **AND** 关系：

- 目标类型必须同时满足 `A` 和 `B`。
- 若 `A` 或 `B` 还依赖其他 `spec`，则这些传递闭包中的要求也必须一并满足。

提议中的 union-form：

```feng
spec Choice: A | B | C;
```

本质是 **OR** 关系：

- 目标值或目标类型只需满足其中之一，即可进入该 union-form。

因此，union-form **绝不能**复用现有 parent spec list 的语义和数据结构。二者不仅语法不同，满足关系的逻辑也相反。

### 3.4 union-form 不应复用 object-form 的块体语义

object-form `spec` 的职责是声明契约形状，允许字段与方法签名。

union-form `spec` 的职责是声明“若干候选类型的其一”。

因此，union-form 与 object-form 应当是并列 form，而不是“object-form 的另一种 parent 列表写法”。

### 3.5 union-form 的成员访问必须先收窄

本次讨论已确认：

- union-form **不提供**未收窄的抽象成员访问。
- 当值仍处于 union-form 视角时，不允许直接访问成员。
- 只有在类型已经收窄到确定 member 后，才允许继续做字段访问、方法调用或其他依赖具体类型布局的操作。

这意味着 union-form 的访问模型与 object-form `spec` 不同：

- object-form `spec` 允许在抽象契约层直接访问成员；
- union-form 必须先缩小到确定类型，再按该具体类型继续访问。

### 3.6 union-form 的基础收窄语法采用 `is`

本次讨论已确认：为了对 union-form 做成员判别与类型收窄，需要引入 `is` 语法。

候选形式如下：

```feng
spec T: int | string | UserType

if v is int {
  // 此处分支内，v 收窄为 int
} else if v is string {
  // 此处分支内，v 收窄为 string
} else {
  // 若前两支都被排除，且仅剩 UserType，则此处分支内，v 收窄为 UserType
}
```

当前已确认的语义要点：

- `v is Type` 用于测试 union-form 当前 active member 是否为 `Type`。
- 当条件成立时，对应分支内的 `v` 必须收窄为该 `Type`。
- 当先前分支已经排除了若干 member，且剩余 member 集合可唯一确定时，后续 `else` 分支也应自动收窄到该唯一剩余类型。
- `is` 收窄既可服务于编译期已知情形，也可服务于运行时基于 `tag` 的判别情形。

### 3.7 union-form 的成员允许包含基础类型、用户定义类型与其他 `spec`

本次讨论已确认：union-form 的 member 合法集合至少包括以下三类：

- 基础类型。
- 用户定义类型。
- 其他 `spec`。
- `void` 不允许作为 union member。

这意味着 union-form 不仅可表达“若干具体类型的其一”，也可表达“具体类型与其他 `spec` 形状的其一”。

同时，`void` 当前应继续仅用于无返回值语义的位置，例如函数与方法的返回值声明；不应进入 union-form 的值类型成员集合。

### 3.8 若 member 本身是 union-form，可在编译期拍平

本次讨论已确认：若某个 union-form 的 member 解析后本身又是 union-form，则应在编译期将其拍平到当前 union-form 的 member 集合中。

这条规则的含义是：

- 拍平是编译期语义归一化行为，而不是新的运行时结构。
- 运行时不需要保留“union 里面再包一层 union”的层级。
- 后续的成员匹配、收窄、相等性与 codegen，都应基于拍平后的成员集合来定义。

示意上，可理解为：

```feng
spec A: int | string;
spec B: A | UserType;
```

在语义层可按如下方式归一化理解：

```feng
spec B: int | string | UserType;
```

### 3.9 union-form 默认零值先取归一化后的第一个 member

本次讨论先定为：union-form 的默认零值应取归一化后第一个 member 的默认零值。

这条规则的直接含义是：

- union-form 默认初始化时，active variant 取归一化后的第一个 member。
- payload 按该 member 自身的默认零值规则初始化。
- 若归一化后的第一个 member 本身不是合法的默认零值目标，则该 union-form 也不是合法的默认零值目标。

示意上，可理解为：

```feng
spec Value: int | string | UserType;
```

则 `Value` 的默认零值先按 `int` 的默认零值来理解，也就是 active variant 为 `int`，payload 为 `0`。

```feng
spec Display: Named | string;
```

则 `Display` 的默认零值先按归一化后的第一个 member `Named` 的默认零值来理解，也就是 active variant 为 `Named`，payload 为该 `spec` 的默认 witness。

### 3.10 运行时顶层分类的抽象完备性

本次讨论已确认：

- `trivial`：纯值，不涉及托管所有权。
- `managed-pointer`：值本身就是一根托管引用。
- `aggregate-with-managed-slots`：值语义承载体，内部含一个或多个托管槽位，或按值组合多个成分。

任何看起来像“第四类”的方案，最终都会归约到这三类之一：

- 若新类型最终装箱到堆上，本质是 `managed-pointer`。
- 若新类型按值布局且内部含托管成员，本质是 `aggregate`。
- 若新类型最终只是纯字节 payload，本质是 `trivial`。

## 4 基于三分类的映射原则

### 4.1 tuple 的映射原则

本次讨论已确认以下例子：

- `(int, int)` 应映射到 `trivial`。
- `(int, UserType)` 应映射到 `aggregate`。

原因：

- `int` 是 `trivial`。
- `UserType` 在当前值模型中应视为托管指针。
- `(int, UserType)` 整体既不是纯值，也不是单一托管指针，因此应是按值聚合、内部带托管槽位的 `aggregate`。

### 4.2 联合类型的映射原则

本次讨论已确认：联合类型可以走 `aggregate`，而且对多数有意义的联合，这应当是主路线。

在当前讨论下，较稳妥的理解是：

- `all-trivial union`：可映射到 `trivial`。
- `mixed union`：应映射到 `aggregate`。
- `包含 aggregate 成员的 union`：应映射到 `aggregate`。

其中，member 本身既可以是基础类型、用户定义类型，也可以是其他 `spec`；这些类别的混合并不改变 union-form 需要最终映射到既有三类值模型之一这一原则。

对 `all-managed-pointer union`，本次讨论未形成最终定稿：

- 可以继续评估是否允许它落到 `managed-pointer`；
- 若不能在不引入额外顶层类别的前提下稳定恢复 active variant，则仍建议统一落到 `aggregate`。

### 4.3 union-form 的访问路径开销模型

本次讨论已确认：union-form 的访问开销应拆成“收窄成本 + 收窄后的具体类型访问成本”，而不是按“每次成员访问都走抽象分派”来理解。

具体约束如下：

- 若可在编译期确定 active member，则应直接在编译期完成收窄。
- 若只能在运行时确定 active member，则访问路径至多先承担一次 `tag` 判别。
- 一旦某个控制流分支内已经收窄到确定 member，后续访问应按该具体类型直接发码，而不是为每次访问重复承担 union-form 的判别开销。

因此，union-form 与 object-form `spec` 的访问成本模型不同：

- object-form `spec` 的访问通常表现为抽象契约分派；
- union-form 的访问通常表现为“一次收窄 + 分支内直接访问具体类型”。

### 4.4 union-form 的运行时基线表示

当前讨论倾向于：

- 把 `aggregate` 作为 union-form 的**统一基线表示**；
- 即使未来对某些受限子集做优化，也不影响 union-form 在抽象层面归类为既有三类之一。

这条结论的含义是：

- 联合类型**不需要新增第四类顶层运行时结构**；
- 但它仍可能需要新的布局规则、判别规则或描述符扩展。

## 5 关于开销的评估结论

本次讨论中，“没有更多开销”被拆成两层：

- 表示开销：值大小、默认值形态、是否新增对象分配。
- 路径开销：成员访问、类型提取、赋值/复制/释放时是否新增判别分支。

基于此，当前形成的评估结论是：

- 不能把“联合类型没有更多开销”作为一般性保证。
- 对某些受限子集，可能做到：
  - 不新增额外堆分配；
  - 或值大小不增加；
- 但通常很难同时做到：
  - 无额外状态；
  - 无额外分支；
  - 且对所有联合都不高于当前 object-form `spec` 的成本。

因此，若 union-form 作为真实值进入 codegen 与 runtime，其一般方案仍应假设存在：

- active variant 的判别信息；
- 基于判别信息的路径分派。

## 6 对 parser / AST / 语义层的直接要求

若后续采纳本草案，需要把联合类型视为 `spec` 的新增 form，而不是沿用现有字段名硬塞进 object-form 语义。

最低要求如下：

### 6.1 parser / AST

- `FengSpecForm` 需要扩展为：
  - `OBJECT`
  - `CALLABLE`
  - `UNION`
- union-form 需要自己的成员集合，例如：
  - `union_members: FengTypeRef[]`
- union-form 不应复用 object-form 的：
  - `parent_specs`
  - `member_count`
  - `{}` 块体语义

### 6.2 语义层

- object-form 继续走“全满足、闭包展开、冲突检查”路径。
- union-form 需要独立定义：
  - 成员匹配规则；
  - 赋值可接受性；
  - 重载匹配规则；
  - 相等性规则；
  - narrowing / pattern matching 的收窄规则。
- union-form 还需要保证：
  - 若 member 解析后本身是 union-form，则在编译期拍平为归一化成员集合；
  - 默认零值取归一化后的第一个 member 的默认零值；
  - 若归一化后的第一个 member 不是合法默认零值目标，则该 union-form 也不是合法默认零值目标；
  - 未收窄时禁止成员访问；
  - 可编译期收窄时优先编译期收窄；
  - 运行时收窄时，分支内后续访问按确定 member 直接发码。

## 7 当前尚未拍板的问题

以下问题在本次讨论中尚未最终定稿，不应在主规范中直接写成既定规则。

### 7.1 union-form 的成员合法集合

当前讨论已确认：

- 允许基础类型作为 union member。
- 允许用户定义类型作为 union member。
- 允许其他 `spec` 作为 union member。
- 不允许 `void` 进入 union-form。
- 若某个 member 解析后本身是 union-form，则应在编译期拍平。

当前阶段无剩余未决项。

与 C ABI 相关的表示、兼容性与约束问题，不在本草案当前阶段处理；该问题留待未来统一方案单独收口，不作为 union-form 当前设计的前置约束。

### 7.2 union-form 的相等性

仍待明确：

- 相等性是否先比较 active variant，再比较对应值。
- 若两个值属于不同 member，是否必定不等。
- 若 member 自身是引用语义类型，是比较身份还是比较值，需与其原类型规则对齐。

### 7.3 union-form 的 narrowing / 模式匹配

当前讨论已确认基础收窄语义，但尚未形成完整扩展语法：

- 基础收窄语法采用 `is`。
- union-form 的成员访问必须先收窄到确定 member。
- 可编译期完成的收窄应优先在编译期完成。
- 运行时收窄的基本成本应收敛为一次 `tag` 判别，而不是每次成员访问重复判别。
- `else` 分支在剩余 member 可唯一确定时，应自动收窄为该唯一剩余类型。
- 仍待明确：
  - 是否引入 union member 解构。
  - 除 `if ... is ...` 之外，是否还需要与 `match` 集成的专门语法。

### 7.4 aggregate union 的条件性槽位

若 union-form 的值级实现采用 `aggregate`，仍需回答：

- 如何表达 active variant。
- 如何表达“某些托管槽位只在某个 variant 下有效”。

本次讨论仅确认：

- 这属于 union-form 的核心实现问题；
- 但它不构成引入第四类顶层运行时结构的理由。

## 8 若后续采纳，需要更新的主规范

若本草案后续进入正式规范，至少需要评估以下文档的改动面：

- [feng-spec.md](./feng-spec.md)
- [feng-language.md](./feng-language.md)
- [feng-type.md](./feng-type.md)
- [feng-flow.md](./feng-flow.md)

必要时也可能涉及：

- [feng-builtin-type.md](./feng-builtin-type.md)
- [feng-expression.md](./feng-expression.md)

其中：

- `feng-spec.md` 应成为 union-form `spec` 的主规范归属。
- `feng-language.md` 只负责总览性说明，不应重复细则。
- `feng-type.md` 与 `feng-flow.md` 只负责引用与协作规则，不应重复 union-form 的主定义。

## 9 当前建议的推进顺序

若继续推进，建议按以下顺序收敛：

1. 先拍板 union-form 的语法与 form 边界。
2. 再拍板 union-form 的成员合法集合。
3. 再拍板 union-form 的相等性。
4. 最后才进入运行时布局与 codegen 细化。

原因：

- 语法与语义边界先定，才能稳定 AST 结构。
- 成员合法集合、相等性决定后，运行时结构才不会反复返工。
