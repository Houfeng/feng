# Feng 联合类型规范

> **状态**: 已实现，作为 union-form 专项规范维护。
> `spec` 三种 form 的总入口见 [feng-spec.md](./feng-spec.md)；本文档只收敛 Feng 联合类型在当前阶段的专项语义、实现边界与运行时布局细则。
> `match 目标值 { ... }` 的通用流程控制语法外壳见 [feng-flow.md](./feng-flow.md)。

## 1 目标

- 在**不引入新关键字**的前提下，为 Feng 增加联合类型能力。
- Feng 的目标是**高性能静态语言**；union-form 的成员选择、收窄与相关 `spec` 转换资格应尽可能在编译期确定，避免引入额外的运行时搜索、回退或二次判别开销。
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

上例不符合本文规范，因为 union-form 不允许块体。

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

进一步说，union-form 也不是“具体 `type` 可以在声明头或 `fit` 中声明满足的契约目标”；`type A: UnionSpec` 与 `fit A: UnionSpec` 都应非法。具体值进入 union-form 只发生在赋值、初始化、传参、返回等值流站点，并按 union member 选择规则处理。
此外，**object-form `spec` 的父 spec 只能是 object-form `spec`**；union-form `spec` 不能出现在 object-form `spec` 的父 spec 列表中。union-form 没有可继承的字段契约，以它作为父 spec 在语义上无意义，应在语义层报错。

### 3.5 union-form 的成员访问必须先收窄

本次讨论已确认：

- union-form **不提供**未收窄的抽象成员访问。
- 当值仍处于 union-form 视角时，不允许直接访问成员。
- 只有在类型已经收窄到确定 member 后，才允许继续做字段访问、方法调用或其他依赖具体类型布局的操作。

这意味着 union-form 的访问模型与 object-form `spec` 不同：

- object-form `spec` 允许在抽象契约层直接访问成员；
- union-form 必须先缩小到确定类型，再按该具体类型继续访问。

### 3.6 union-form 的基础收窄语法复用现有 `if` 条件匹配，不引入独立 `is`

本次讨论已确认：union-form 的成员判别与类型收窄不引入独立 `is` 运算符，而是复用现有 `match 目标值 { ... }` 条件匹配形式，并在该语法下扩展 union member 匹配能力。

其中，`match 目标值 { ... }` 作为流程控制语法外壳及其表达式位置规则由 [feng-flow.md](./feng-flow.md) 统一定义；本文只补充 union-form 在该语法下的 member 标签、active member 判别与收窄语义。

union member 匹配分支支持两种形式：

- **无绑定分支** `Type { ... }`：仅做匹配判别，目标值在分支内保持原始 union 类型，不做自动收窄。
- **有绑定分支** `[let|var] name: Type { ... }`：匹配判别的同时，把收窄后的值绑定到新的局部变量 `name`；`name` 在分支内以获得的具体 member 类型操作。

约束：

- 绑定变量默认为 `let`（不可变），可显式使用 `let` 或 `var`。
- 无绑定分支不创建任何新变量，目标值保持原始 union 类型，分支内不可直接做成员访问或比较。
- `else` 分支不支持绑定。
- 允许同一匹配体中混合使用绑定分支和无绑定分支。

候选形式如下：

```feng
spec T: int | string | UserType | Named;

match v {
  x: int {
    // x 类型为 int
  }
  s: string {
    // s 类型为 string
  }
  UserType {
    // 无绑定，v 保持 union 类型
  }
  n: Named {
    // n 类型为 Named
  }
  else {
    // 无绑定，收窄为剩余 member 集合
  }
}
```

这里复用的是现有 `match 目标值 { ... }` 的分支外壳，而不是继续引入一个容易被误解为”任意运行时类型/接口判断”的独立 `is` 运算符。

当前已确认的语义要点：

- 单个 member 的有绑定分支表示”当前 active member 就是该 member”；绑定变量以获得的具体 member 类型操作。
- 当 `match` 的目标表达式静态类型为 union-form 时，该 `match 目标值 { ... }` 的整个匹配体进入 union member 类型匹配模式；只允许 union member 类型标签与 `else`，不允许字面量值标签或区间标签。
- union-form 未收窄前不具备可直接比较的统一值语义，因此不能在同一次匹配中把 `int { ... }` 这类 member 标签与 `1 { ... }`、`1...5 { ... }` 这类值/区间标签混用。
- union 值在进入 union-form 的具体站点时，active member 就已经被确定；后续 `match 目标值 { ... }` 条件匹配只针对这个已确定的 active member 做判别，不会先把当前 member 向上转换到别的 `spec` 后再尝试匹配。
- 多个 member 可在同一分支中以逗号罗列；有绑定分支把绑定变量收窄到”被列出的 member 子集”的 union 类型，而不是某个单一确定类型；无绑定分支保持原始 union 类型。
- `else` 分支收窄为剩余 member 集合；若剩余集合大小为 1，则该分支收窄到唯一剩余 member，否则仍是更小的 union 子集。`else` 分支不支持绑定。
- 当有绑定分支的绑定变量收窄后仍包含多个 member 时，绑定变量依然处于 union 视角，只是 member 集合变小；此时仍不允许直接做成员访问、方法调用或 `==` / `!=` 比较。
- 当目标 member 本身是 object-form `spec` 且该分支只命中该单一 member 时，绑定变量直接取得该 `spec` 视角，并可按该 `spec` 的既有规则访问成员与发起调用。
- 收窄到 object-form `spec` member 后，绑定变量仍处于该 `spec` 的抽象视角，**不能进一步收窄到具体实现类型**。原因在于 object-form `spec` 是开放类型——任意在当前可见契约闭包中 fit 该 `spec` 的类型均可进入，编译期无法穷举全部候选；因此，该分支内只允许通过该 `spec` 的方法或成员发起调用，不存在继续向下收窄的路径。若程序需要对具体实现类型做进一步判别，应将各具体类型作为独立 union member 直接列出，而不是通过它们共同满足的 object-form `spec` 间接列入。
- 若某个分支先收窄到具体类型，且当前可见契约闭包可证明该具体类型满足某个 object-form `spec`，则允许通过显式转换把该值转换到目标 `spec` 视角；该能力与分支匹配收窄分离。
- 由于不引入独立 `is` 运算符，因此不存在 `&&` / `||` 中基于 `is` 的短路收窄传播规则。

例如，单 member 有绑定分支可直接操作：

```feng
match v {
  n: Named {
    return n.display();
  }
}
```

上例成立的前提是 `Named` 本身就是该 union-form 的直接成员；这里不是在运行时检查某个 concrete type 是否”也满足 `Named`”，而是在判断当前 active member 是否就是 `Named`。

同理，若某个值在进入 union-form 时是按 `UserType` member 进入，则后续：

```feng
match v {
  Named {
    // 无绑定分支，仅做匹配判别
    // 不会因为当前 active member `UserType` 恰好满足 `Named` 就自动命中
  }
}
```

上例中，`Named` 分支只有在 `Named` 本身就是当前 active member 时才会命中；不会因为 `UserType` 可显式向上转换到 `Named`，就在条件匹配阶段自动完成这次转换后再匹配。

若无绑定分支一次罗列多个 member，则该分支仅做匹配判别，目标值保持原始 union 类型。例如：

```feng
match v {
  UserType, Named {
    // 无绑定分支，v 保持 union 类型（子集为 UserType | Named）
    // 若要访问或比较，仍需继续收窄到单一 member
  }
}
```

若需要把已收窄到具体类型的值转成其所满足的 object-form `spec`，应显式写出转换，而不是由分支匹配隐式完成。例如：

```feng
match v {
  u: UserType {
    let named = (Named)u;
  }
}
```

上例中，`UserType` 分支通过绑定变量 `u` 把值收窄为 `UserType`；后续 `(Named)u` 是否成立，取决于当前可见契约闭包中 `UserType` 是否满足 `Named`。若不满足，则该显式转换应报错。

需要额外明确的是：当前阶段**不支持**把仍处于 union 视角的值直接投影到一个共同 `spec`，即使该 union 的全部 member 都满足该 `spec`。例如：

```feng
spec U: UserType1 | UserType2;

let u: U = ...;
let x = (Named)u;   // 当前阶段不支持
```

原因不是编译期无法证明 `UserType1` 与 `UserType2` 都满足 `Named`，而是 `union -> common spec` 不再是“确定源类型上的普通向上转换”，而是一次从 union 视角到共同 `spec` 视角的投影。

对 object-form `spec` 而言，目标值不仅包含 `subject`，还需要对应的目标 witness。若 `UserType1` 与 `UserType2` 各自进入 `Named` 时使用不同 witness，则一般需要先根据 union 当前 active member 选择正确的那一条投影路径。也就是说，这类投影在一般情形下**可能需要一次基于 `tag` 的运行时选择**，然后才能构造目标 `Named` 值。

出于 Feng 当前“高性能静态语言”的目标，以及“普通向上转换资格与发码路径应尽可能在编译期定死”的约束，本文当前阶段不把这种 `union -> common spec` 投影纳入普通显式转换规则。若后续需要支持，应作为单独能力设计，而不是并入当前的向上转换语义。

### 3.7 union-form 的成员允许包含基础类型、用户定义类型与其他 `spec`

本次讨论已确认：union-form 的 member 合法集合至少包括以下三类：

- 基础类型。
- 用户定义类型。
- 其他 `spec`。
- `void` 不允许作为 union member。

这意味着 union-form 不仅可表达“若干具体类型的其一”，也可表达“具体类型与其他 `spec` 形状的其一”。

同时，`void` 当前应继续仅用于无返回值语义的位置，例如函数与方法的返回值声明；不应进入 union-form 的值类型成员集合。

### 3.8 嵌套 union-form 保持声明时层次，不展开

本次讨论已确认：若某个 union-form 的 member 解析后本身又是 union-form，**不递归展开**其子变体，而是保持声明时的层次结构。成员去重作用在直接成员层面，并保持声明顺序。

这条规则的含义是：

- union-form 的成员列表即声明时写出的直接成员，不递归展开嵌套的 union-form。
- 每个直接成员可以是具体类型（`type`、基础类型等），也可以是另一个 union-form。
- 成员去重在直接成员层面执行：若同一类型被多次列出，只保留首次出现。
- 运行时采用嵌套标记联合（nested tagged union）布局，每层 union-form 各自拥有独立的 tag。

示意上，可理解为：

```feng
spec A: int | string;
spec B: A | UserType;
```

`B` 的直接成员为 `[A, UserType]`（2 个），**不是** `[int, string, UserType]`（3 个）。

#### 3.8.1 赋值时的多级链路查找

赋值、初始化、传参、返回等进入站点确定 active member 时，编译器在**编译期语义分析阶段**通过递归链路查找，确定从源类型到目标 union-form 的完整路径。路径信息传递给代码生成阶段，运行时仅执行已确定的 tag 设置与数据拷贝，**不存在动态匹配或递归**。

查找规则：

1. **直接匹配**：源类型与某个直接成员精确一致 → 路径为该单一成员。
2. **成员转换匹配**：不存在精确成员时，若源类型可通过既有隐式转换进入某个成员（例如具体 `type` 向上转换到其实现的 object-form `spec`），则该成员可以成为路径叶子。
3. **间接匹配**：源类型可通过某个嵌套 union-form 成员间接到达 → 递归查找，路径为外层成员 + 内层路径。
4. **歧义检测**：若源类型可通过多条不同路径到达目标 union-form → 编译期报错，要求显式转换（`as`）。

源表达式能否进入候选叶子 member，必须复用该表达式向叶子 member 类型的既有目标类型贴合规则，不得仅因 member 是内建类型就视为可进入。数值字面量及纯字面量数值常量表达式的候选资格统一由 [Feng 内建类型规范](./feng-builtin-type.md) §6 决定；不存在可贴合的叶子 member 时，整个 union-form 目标贴合失败。

选定路径只确定 active member，不会省略该成员自身要求的转换。值写入叶子 payload 前，必须先按普通赋值语义转换为叶子成员类型，再逐层组装 union-form。因而 `ConcreteType -> Option<ObjectSpec>` 的语义是先构造完整的 `ObjectSpec` 值（包括 `subject` 与 `witness`），再把该值写入 `Option<ObjectSpec>` 的对应 payload；实现可以融合发码，但结果表示与分步写法必须一致。

示例：

```feng
// 直接匹配
let b: B = UserType(...);        // path = [UserType]
let b: B = a;                    // a 类型是 A → path = [A]

// 间接匹配
let i: int = 42;
let b: B = i;                    // int 是 A 的成员 → path = [A, int]

// 歧义报错
spec X: int | string;
spec Y: int | bool;
spec Z: X | Y;
let i: int = 42;
let z: Z = i;                    // 错误：int 可通过 X 和 Y 两条路径到达 Z
let z: Z = i as X;               // 修复：显式指定路径
```

#### 3.8.2 代码生成

代码生成器根据编译期确定的路径，发射逐级 tag 设置和数据拷贝指令：

- **叶子类型赋值**（path = [A, int]）：先按普通赋值语义把源值转换为叶子成员类型，再设置 B 的外层 tag 为 A 槽位、设置 A 的内层 tag 为 int 槽位，最后拷贝转换后的 int 数据。
- **整体赋值**（path = [A]）：设置 B 的外层 tag 为 A 槽位，整体拷贝 A 值（含 A 自身的 tag）。

#### 3.8.3 内存布局

采用嵌套标记联合（nested tagged union）。每个 union-form 本身就是 tagged union，嵌套是自然组合——外层 union-form 的嵌套 union-form 槽位的 payload 就是一个完整的内层 union-form 值（自带 tag + data），无需额外编码处理。

Tag 编码规则：

- 每个 union-form 层的 tag 值从 0 开始，按直接成员声明顺序递增。
- 嵌套层数在实践中通常 ≤ 2，内存开销可控。
- 成员去重逻辑保持不变（去重作用在直接成员层面）。

#### 3.8.4 边界场景

**三层及以上嵌套**：路径递归查找，深度无硬限制。运行时对应多级 tag 嵌套。

**open spec 扩展**：`spec Base: A | B; spec Extended: Base | C;` 后通过 `open` 扩展 `Base` 增加新成员 `D`，`Extended` 的直接成员仍为 `[Base, C]`，`D` 通过 `Base` 路径可达 `Extended`。链路查找自动适应 `open spec` 的扩展。

**泛型 union-form**：`spec Option<T>: None | T; spec Result<T>: Option<T> | Error;` 泛型实例化后，链路查找在实例化类型上进行。

### 3.9 union-form 默认零值先取直接成员列表中的第一个 member

本次讨论先定为：union-form 的默认零值应取直接成员列表中第一个 member 的默认零值。

这条规则的直接含义是：

- union-form 默认初始化时，active variant 取直接成员列表中的第一个 member。
- payload 按该 member 自身的默认零值规则初始化。
- 若直接成员列表中的第一个 member 本身不是合法的默认零值目标，则该 union-form 也不是合法的默认零值目标。

示意上，可理解为：

```feng
spec Value: int | string | UserType;
```

则 `Value` 的默认零值先按 `int` 的默认零值来理解，也就是 active variant 为 `int`，payload 为 `0`。

```feng
spec Display: Named | string;
```

则 `Display` 的默认零值先按直接成员列表中的第一个 member `Named` 的默认零值来理解，也就是 active variant 为 `Named`，payload 按 `Named` 自身的默认零值规则初始化（如果 `Named` 有合法默认零值）。

### 3.10 union-form 的 `==` / `!=` 也必须先收窄

本次讨论已确认：union-form 上的 `==` / `!=` 运算也必须先收窄到确定 member，未收窄时不允许直接比较。

这条规则的直接含义是：

- 当值仍处于 union-form 视角时，不允许直接对其执行 `==` 或 `!=`。
- 只有在控制流中已经把值收窄到确定 member 后，才允许继续比较。
- 收窄后的比较语义直接复用该 member 自身既有的相等性规则，而不是为 union-form 另行定义一套“跨 member 比较”规则。

因此，union-form 不存在“未收窄时先比较 active variant，再比较 payload”的统一比较入口；比较行为总是在收窄后按具体类型规则发生。

### 3.11 运行时顶层分类的抽象完备性

本次讨论已确认：

- `trivial`：纯值，不涉及托管所有权。
- `managed-pointer`：值本身就是一根托管引用。
- `aggregate-with-managed-slots`：值语义承载体，内部含一个或多个托管槽位，或按值组合多个成分。

任何看起来像“第四类”的方案，最终都会归约到这三类之一：

- 若新类型最终装箱到堆上，本质是 `managed-pointer`。
- 若新类型按值布局且内部含托管成员，本质是 `aggregate`。
- 若新类型最终只是纯字节 payload，本质是 `trivial`。

### 3.12 union-form 可作为泛型类型参数约束

union-form `spec` 可用于声明泛型类型参数的约束，例如：

```feng
spec Value: int | string;

type Cell<V: Value> {
  let value: V;
}

func use<V: Value>(v: V): void { ... }
```

约束细则：

- 在泛型声明体内，被约束参数 `V` 的值处于 union 视角；**未经 `match 目标值 { ... }` 绑定收窄时，不允许对其做成员访问、方法调用或 `==` / `!=` 比较**。
- 利用 `match 目标值 { ... }` 对参数值进行 member 匹配时，分支支持有绑定和无绑定两种形式（见 3.6 节）；只有有绑定分支才能通过绑定变量按已收窄的 member 类型做成员访问、方法调用或比较。
- 无绑定分支不做自动收窄；泛型体内对参数值的任何成员访问或比较，都必须通过有绑定分支的绑定变量完成。
- union-form 约束不为参数类型物化 witness；调用点只传入值本身，而不是 object-form `spec` 约束所需的「值 + witness」对。
- 当前阶段每个类型参数至多一个 union-form 约束；不支持 union-form 约束与 object-form `spec` 约束同时修饰同一个类型参数。
- 在约束链传递中（如子 `spec` 继承父 `spec` 的约束），union-form 约束遵循与 object-form `spec` 约束相同的参数传递规则：向上传递的约束不得比目标约束更宽松。
- union-form 约束不是进入 union 的值流站点；传入泛型函数的实际实参类型必须已是该 union-form 的某个 member，而不是先进入 union 再传入。

### 3.13 多级 match 语法（后续增强）

在基础 match 稳定后，可增加 `->` 链式模式语法，消除嵌套 match 的冗余。此语法为纯前端增强，独立排期，不影响核心类型系统和运行时表示。

**多级模式层级规则**：每一级必须是前一级类型的成员。

- 第一级：必须是 match 目标所属 union-form 的直接成员。
- 第二级：必须是第一级类型的直接成员（第一级必须是 union-form）。
- 第三级：必须是第二级类型的直接成员（第二级必须是 union-form）。
- 依此类推。

语法：

```feng
match body {
    Expression -> IdentifierExpr { ... }
    Expression -> BooleanLiteralExpr { ... }
    Block { ... }
    else { ... }
}
```

错误示例：

```feng
match body {
    IdentifierExpr { ... }       // 错误：IdentifierExpr 不是 body 所属 union-form 的直接成员
    Expression -> Block { ... }  // 错误：Block 不是 Expression 的成员
}
```

**链式写法与嵌套写法等价**：

```feng
// 链式写法
match body {
    Expression -> IdentifierExpr { ... }
    Expression -> BooleanLiteralExpr { ... }
    Block { ... }
    else { ... }
}

// 等价的嵌套写法
match body {
    Expression {
        match body {
            IdentifierExpr { ... }
            BooleanLiteralExpr { ... }
            else { ... }
        }
    }
    Block { ... }
}
```

**编译器处理**：在 pattern 编译阶段将 `->` 链递归展开为嵌套 match，不影响类型系统和运行时表示。穷尽性检查按层级验证。

**first-match-wins 语义**：match 不报 unreachable pattern 警告，分支按书写顺序从上到下匹配，第一个命中的分支执行。

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

本次讨论已确认：union-form 的值级实现统一采用 `aggregate-with-managed-slots` 基线表示。

首版固定包含三个逻辑组成部分：

- 一个 `tag`，用于表达当前 active variant。
- 一个运行时转发槽 `_fwd`，用于把 aggregate walker 转发到当前 active payload 的托管槽位描述。
- 一个 inline payload 区域，用于承载当前 active member 的按值表示。

其中，member 本身既可以是基础类型、用户定义类型，也可以是其他 `spec`；这些类别的混合并不改变 union-form 统一落到既有 `aggregate-with-managed-slots` 这一顶层值模型的原则。

这意味着首版不再为 `all-trivial union` 或 `all-managed-pointer union` 额外定义另一套顶层值分类特例；它们同样按这套统一基线表示落地。

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

当前讨论已确认：

- 把 `aggregate-with-managed-slots` 作为 union-form 的**统一基线表示**；
- 其首版固定布局为“一个 `tag`、一个 `FengManagedSlotDescriptor _fwd`、一个 inline payload 区域”；
- `tag` 负责表达当前 active member 在直接成员列表中的序号，供 `match` member 匹配与收窄发码使用；
- `_fwd` 负责表达当前 active payload 的生命周期槽位，供 aggregate walker 在 retain / release / assign / take / 托管扫描路径中转发使用；
- `_fwd` 不承载语言层 member identity；多个 member 可以拥有相同的 `_fwd.kind` 与 payload offset，但仍必须用不同 `tag` 区分；
- inline payload 区域必须能内联容纳直接成员中尺寸与对齐需求最大的 member 表示；不允许为 aggregate member 采用装箱作为首版通用路径；
- 当前 active member 不含托管槽位时，`_fwd.kind` 写为 `FENG_SLOT_NONE`；active member 是托管指针时，`_fwd.kind` 写为 `FENG_SLOT_POINTER`；active member 是内联 aggregate 时，`_fwd.kind` 写为 `FENG_SLOT_NESTED_AGGREGATE` 且 `nested` 指向对应 aggregate descriptor；
- 复制、销毁与托管扫描只按 `_fwd` 指向的当前 active payload 生命周期规则处理；这套生命周期继续复用现有 aggregate 通用能力，由相应描述符驱动，不要求为 union-form 新增专用 runtime 分支或新的通用 API；
- 即使未来对某些受限子集做优化，也不影响 union-form 在抽象层面归类为既有三类之一。

这条结论的含义是：

- 联合类型**不需要新增第四类顶层运行时结构**；
- 首版主要新增的是 codegen 发出的固定布局与描述符元信息，而不是修改现有 runtime 的通用 aggregate 生命周期实现。

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
- 值进入 union-form 时的 member 选择在语义层尽可能编译期确定；运行时不得为此重新搜索“某对象还满足哪些 `spec`”。

## 6 对 parser / AST / 语义层的直接要求

若按本文推进实现，需要把联合类型视为 `spec` 的新增 form，而不是沿用现有字段名硬塞进 object-form 语义。

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
  - union-form 不得作为 `type A: B` 声明头或契约适配 `fit A: B` 的目标；具体值进入 union-form 只能发生在赋值、初始化、传参、返回等值流站点；
  - 若 member 解析后本身是 union-form，保持声明时层次结构不递归展开；成员去重在直接成员层面执行并保持声明顺序；
  - 默认零值取直接成员列表中第一个 member 的默认零值；
  - 若直接成员列表中的第一个 member 不是合法默认零值目标，则该 union-form 也不是合法默认零值目标；
  - union-form 的成员收窄仅复用现有 `match 目标值 { ... }` 条件匹配形式，不引入独立 `is` 运算符；收窄通过有绑定分支的绑定变量实现，无绑定分支不做自动收窄（绑定语法与约束见 3.6 节）；
  - union 值一旦在进入站点选定 active member，后续条件匹配只按该已选定 member 判别；不会在匹配阶段再依据其可向上转换到的 `spec` 重新解释当前值；
  - 值进入 union-form 时，编译器在编译期通过多级链路查找确定路径：精确直接 member 优先，嵌套 union-form 间接匹配次之；多条可达路径构成歧义时必须报错，不得按声明顺序兜底；
  - 只有在不存在精确 member 命中，且多个 `spec` member 可通过编译期可证的向上转换同时接纳源值时，才构成进入站点冲突；
  - 上述冲突站点禁止隐式选择 active member，也不按声明顺序兜底；开发者必须先显式转换到目标 `spec` member，再把结果写入 union-form；
  - 当开发者已通过显式向上转换选定某个 `spec` member 时，其语义等价于“先构造该目标 `spec` 值，再按该 `spec` member 进入 union-form”；实现可融合发码，但不得高于这条显式两步路径的额外运行时开销；
  - 重叠 member 的最终歧义判定不在 union-form 声明点完成，而应在当前可见契约闭包固定后，于值进入 union-form 的具体站点执行；
  - `==` / `!=` 也必须先收窄到确定 member；
  - 未收窄时禁止成员访问；
  - 单个 member 的有绑定分支把绑定变量收窄到确定 member 类型；无绑定分支保持原始 union 类型；多个 member 罗列的有绑定分支把绑定变量收窄到对应子集的 union 类型；
  - 当 `spec` 本身是 union member 时，有绑定分支收窄到该 `spec` 后，绑定变量直接按该 `spec` 视角操作；
  - 已收窄到具体类型后，若当前可见契约闭包可证明其满足某个 object-form `spec`，则允许显式转换到该 `spec`；
  - 即使某个 union 或 union 子集的全部可能 member 都满足同一个 object-form `spec`，当前阶段也不允许直接把该 union 视角值显式转换到该共同 `spec`；若放开此能力，一般可能需要一次基于 `tag` 的运行时投影，因此应单独设计；
  - 可编译期收窄时优先编译期收窄；
  - 运行时收窄时，分支内后续访问按确定 member 直接发码。

## 7 已收口的边界问题

以下小节记录 union-form 在本轮讨论中需要单独收口、现已确认的边界问题。当前阶段无剩余未决项；若后续能力边界发生变化，应同步修订本文与相关主规范。

### 7.1 union-form 的成员合法集合

当前讨论已确认：

- 允许基础类型作为 union member。
- 允许用户定义类型作为 union member。
- 允许其他 `spec` 作为 union member。
- 不允许 `void` 进入 union-form。
- 若某个 member 解析后本身是 union-form，保持声明时层次结构不递归展开；成员去重在直接成员层面执行并保持声明顺序（详见 3.8 节）。

当前阶段无剩余未决项。

与 C ABI 相关的表示、兼容性与约束问题，不在本文当前阶段处理；该问题留待未来统一方案单独收口，不作为 union-form 当前设计的前置约束。

### 7.2 union-form 的相等性

当前讨论已确认：

- union-form 的 `==` / `!=` 必须先收窄到确定 member。
- 未收窄时不允许直接比较。
- 收窄后的比较直接复用该 member 自身既有的相等性规则。

当前阶段无剩余未决项。

### 7.3 union-form 的 narrowing / 模式匹配

当前讨论已确认基础收窄语义：

- 基础收窄仅复用现有 `match 目标值 { ... }` 条件匹配形式，不新增独立 `is` 运算符。
- 当 `match` 的目标静态类型为 union-form 时，匹配体只允许 union 直接成员类型标签与 `else`；字面量值标签与区间标签仅适用于非 union 目标。
- match 匹配直接成员；若某个直接成员本身是 union-form，匹配该成员后目标值收窄为该嵌套 union-form 类型，可在分支内继续 match 其成员。穷尽性检查只验证直接成员是否被覆盖。
- union 值在进入站点选定 active member 后，后续条件匹配只按该 active member 本身判别；不会在匹配阶段自动向上转换后再命中别的 `spec` branch。
- union-form 的成员访问必须先收窄到确定 member；收窄通过有绑定分支的绑定变量实现，无绑定分支不做自动收窄（绑定语法与约束见 3.6 节）。
- 可编译期完成的收窄应优先在编译期完成。
- 运行时收窄的基本成本应收敛为一次 `tag` 判别，而不是每次成员访问重复判别。
- 单个 member 的有绑定分支把绑定变量收窄到该确定 member 类型；无绑定分支保持目标值的原始 union 类型。
- 多个 member 可以在同一分支中以逗号罗列；有绑定分支把绑定变量收窄到对应 member 子集的 union 类型，无绑定分支保持原始 union 类型。
- 当某个有绑定分支收窄后的 member 子集大小仍大于 1 时，绑定变量仍是 union 视角；若要访问、调用或比较，必须继续收窄到单一 member。
- `else` 分支收窄为剩余 member 集合；若剩余集合可唯一确定，则该分支收窄到唯一剩余 member，否则仍是更小的 union 子集。`else` 分支不支持绑定。
- 当目标 member 本身是 object-form `spec` 且该分支只命中这一个 member 时，绑定变量直接按该 `spec` 视角操作。收窄到 object-form `spec` member 后，绑定变量仍处于该 `spec` 的抽象视角，不能进一步收窄到具体实现类型；object-form `spec` 是开放类型，编译期无法穷举其全部实现，该分支内只允许调用该 `spec` 本身定义的方法与成员。若需对具体实现类型做判别，应将各具体类型作为独立 union member 直接列出。
- 已收窄到具体类型后，若当前可见契约闭包能证明其满足某个 object-form `spec`，则应通过显式转换进入该 `spec` 视角。
- 即使某个 union 视角值的全部可能 member 都满足同一个 object-form `spec`，当前阶段也不支持直接把该 union 视角值显式转换到该共同 `spec`；这类能力一般可能需要一次基于 `tag` 的运行时投影，因此暂不纳入本轮设计。
- 当前阶段无剩余未决项。

### 7.4 aggregate union 的条件性槽位

当前讨论已确认：union-form 的值级实现固定包含：

- 一个 `tag`。
- 一个运行时转发槽 `_fwd`。
- 一个 inline payload 区域。

对应语义为：

- `tag` 负责表达当前 active member 在直接成员列表中的序号，供 member 匹配与收窄发码使用。
- `_fwd` 负责表达当前 active payload 的生命周期槽位，供 aggregate walker 转发到 `NONE` / `POINTER` / `NESTED_AGGREGATE` 路径。
- inline payload 区域按值承载当前 active member，必须能容纳直接成员中尺寸与对齐需求最大的表示。
- 复制、销毁与托管扫描仅按 `_fwd` 指向的当前 active payload 规则处理。
- 该设计仍属于既有 `aggregate-with-managed-slots` 顶层值模型，不构成第四类运行时结构。

当前阶段无剩余未决项。

### 7.5 重叠 member 的歧义判定与 active variant 选择

当前讨论已确认：

- 由于契约关系可能在后续声明中补全，且包内孤儿契约也会影响当前可见语义，重叠 member 的最终歧义判定不适合在 union-form 声明点完成。
- 相关判定应延后到当前可见契约闭包固定后，并在值进入 union-form 的具体站点执行。
- 若源静态类型与某个直接 member 精确一致，则必须优先按该 member 进入；即使该源类型也满足某个 `spec` member，也不构成歧义。
- 因此，在 `UserType | Named` 这类组合中，`UserType` 值进入 union-form 时应按 `UserType` member 进入；`UserType` 满足 `Named` 本身不构成冲突。
- 真正的进入冲突只发生在不存在精确 member 命中，而两个或多个 `spec` member 同时可通过编译期可证的向上转换接纳同一源值时，例如两个重叠 `spec`，或父/子 `spec` 同时出现且源值可同时进入二者。
- 这类冲突站点禁止隐式选择 active variant，也不按声明顺序兜底；开发者必须先显式转换到目标 `spec` member，再把结果写入 union-form。
- 显式转换资格本身仍按 `spec` 的向上转换规则在编译期确定，不引入运行时满足关系搜索。
- 一旦目标 `spec` member 由显式转换确定，进入 union-form 的语义等价于“先得到该 `spec` 值，再写入 union-form”；实现可直接融合这两步，但不得额外引入运行时成员选择、满足关系搜索、候选比较或回退。
- 这里比较的是“转换资格判定与 union member 选择”的额外成本；union 自身固有的 `tag` / payload 写入，以及赋值、传参、返回等站点本来就需要承担的值搬运规则，不属于该条额外成本。
- 与此相对，若允许 `union -> common spec` 投影，例如 `spec U: UserType1 | UserType2; let x = (Named)u;`，则一般可能需要先依据 `u` 的 `tag` 选择 `UserType1 -> Named` 或 `UserType2 -> Named` 的那条固定投影路径。该能力当前阶段暂不支持，也不并入“普通向上转换”规则。

示意上，可理解为：

```feng
spec Display: Named | Identified;

let a: Display = (Named)user;
let b: Display = (Identified)user;
```

上例中，若 `user` 同时满足 `Named` 与 `Identified`，则 `let x: Display = user;` 应报错，而不是隐式选择其中之一。

其成本基线可理解为：

```feng
let s: Named = user;
let t: Display = s;
```

直接写成 `let t: Display = (Named)user;` 时，发码不得高于这条显式两步路径的额外成本；实现可融合中间临时值，但不得增加动态判定或回退开销。

当前阶段无剩余未决项。

## 8 若后续并入主规范，需要更新的文档

若后续把本文并入总规范，至少需要评估以下文档的改动面：

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
- `feng-type.md` 只负责类型系统总览与引用，不应重复 union-form 的主定义。
- `feng-flow.md` 只负责 `match 目标值 { ... }` 的流程控制语法外壳与其对 union member 匹配的入口说明；union member 的多级链路查找、收窄、active member 判别与转换边界仍由本文定义。

## 9 当前建议的实现顺序

若继续推进实现，建议按以下顺序落地：

1. 先落 parser / AST，补齐 union-form 语法、form 边界与成员集合承载。
2. 再落语义层，完成成员收集（保持声明时层次）、进入站点多级链路查找、显式转换边界与 `match` 收窄规则。
3. 再落 codegen / 描述符接入，基于现有 aggregate runtime 能力按”一个 `tag`、一个 `_fwd`、一个 inline payload 区域”的固定布局完成构造、判别、复制、销毁与扫描。
4. 最后补齐 diagnostics、测试与主规范并入。

原因：

- 语法承载与语义收集先稳定，后续 runtime / codegen 才有确定输入。
- 固定布局与分支收窄规则需要依赖已完成的语义信息，不能倒序施工。
- 现有 runtime 的 aggregate 通用能力已足以承载 union 生命周期；实现工作重心在描述符设计与发码接入，而不在 runtime 通用层改造。
