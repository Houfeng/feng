# `spec` 支持定义 `seal` 成员实现草案

> 本文档是实现草案，不是语言权威规范。
> 本文确认的规则已经写入
> [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 等权威规范；本文
> 继续用于跟踪实现范围和实施 TODO，最终语言语义以权威规范为准。
>
> **当前状态**：`spec seal` 核心语义及非泛型方法的跨包编译器 ABI
> 依赖修复已实现；泛型 owner、方法级泛型、泛型 `fit` 以及
> `seal static` 字段的跨包验证用例已保留并暂停，待独立分析对应的
> 既有泛型解析、泛型导入符号面和静态字段链接问题。

## 1 背景

object-form `spec` 当前只能声明公开成员。成员进入契约后，普通代码可以
通过 `spec` 视角访问，无法表达“实现者必须提供、但只有实现该契约的
`type` 才能通过契约视角访问”的成员：

```feng
open spec Widget {
  func show(): void;
  seal func draw(): void;
}
```

其中：

- `show` 是普通公开 API；
- `draw` 是所有 `Widget` 实现者必须提供的契约成员；
- 普通代码不能通过 `Widget` 视角访问 `draw`；
- 实现 `Widget` 的 `type` 可以在自身成员方法、静态方法及其 `fit`
  扩展方法中，通过 `Widget` 视角访问 `draw`；
- 调用继续通过既有 witness 分派到实际实现成员。

本能力只为 `spec` 增加成员级访问控制，不改变具体 `type` 成员的任何
既有可见性规则。

## 2 范围

### 2.1 目标

- 允许 object-form `spec` 的实例字段、实例方法、静态字段和静态方法
  声明 `seal`。
- 无修饰的 `spec` 成员继续保持当前公开语义。
- `seal` 成员与公开成员一样参与现有契约满足检查和 witness 构造。
- 实现该成员原声明 `spec` 的 `type`，可以在自身成员方法、静态方法和
  该 `type` 的 `fit` 扩展方法中，通过 `spec` 视角访问该 `seal` 成员。
- 公开 `spec` requirement 只能由公开 `type` 成员满足；`spec seal`
  requirement 可以由公开或 `seal` 的 `type` 成员满足。
- 普通函数以及未实现该 `spec` 的 `type` 不能获得该访问权限。
- 子 `spec` 按现有继承规则继承父 `spec` 的公开成员和 `seal` 成员。
- 源码和 `.ft` 恢复的 `spec` 成员必须保持相同的可见性语义。

### 2.2 非目标

- 不修改 `type` 的 `seal` 成员访问规则。
- 不授予 `fit` 扩展方法任何新的目标 `type` 私有访问权；`fit` 获得的
  新权限仅限于通过 `spec` 视角访问 `spec seal` 成员。
- 不改变跨包 `fit`、目标 `type` 可见面或私有成员发现规则。
- 不引入新的 witness 种类、显式 witness 映射语法或运行时访问检查。
- 不修改现有名义 relation 模型，不在 `.ft` 中增加逐槽
  witness plan。
- 不把成员级 `seal` 扩展到 callable-form、union-form 或
  intersection-form `spec`。
- 不引入 `protected`、逐项 `friend` 或其他新的访问控制模型。

## 3 核心语义

### 3.1 `spec` 成员可见性

object-form `spec` 成员具有以下有效可见性：

```text
effective_visibility(无修饰 spec 成员) = open
effective_visibility(open spec 成员)   = open
effective_visibility(seal spec 成员)   = seal
```

显式 `open` 与省略修饰符具有相同的公开 requirement 语义。Parser / AST
继续分别记录 `PUBLIC` 与 `DEFAULT` 声明事实，Semantic 负责将两者解释为
相同的公开成员可见性；`seal` 记录为 `PRIVATE` 并收窄 spec 访问面。

object-form `spec` 因此具有两个用途不同的成员集合：

| 集合 | 内容 | 用途 |
| --- | --- | --- |
| 公开访问面 | 无修饰或显式 `open` 成员 | 普通 `spec` 使用者访问 |
| 完整契约 | 无修饰、显式 `open` 与 `seal` 成员 | 满足检查、witness 构造和实现域访问 |

成员不在公开访问面中，不表示该成员不属于契约。所有实现者仍必须提供
完整契约所要求的成员。

### 3.2 契约满足与 witness

`spec seal` 成员沿用现有 object-form `spec` requirement 的结构匹配和
witness 规则，并在匹配时增加统一的可见性兼容条件：

- 字段继续按名称、静态性、绑定方式和类型匹配；
- 方法继续按名称、静态性和 callable 签名匹配；
- 父 `spec` requirement 继续按现有闭包规则参与满足检查；
- 匹配成功后，继续建立现有 requirement 到 `type` 实现成员的 witness；
- 调用继续沿现有 witness 分派，不增加新的运行时结构或分支。

可见性兼容规则如下。无修饰的 `spec` 成员和 `type` 成员均按现有
公开语义处理：

| `spec` requirement | 公开 `type` 成员 | `type seal` 成员 |
| --- | --- | --- |
| 无修饰（公开） | 允许满足 | 拒绝满足 |
| `seal` | 允许满足 | 允许满足 |

该规则统一适用于字段和方法、实例成员和静态成员，以及 `type` 直接声明
和 `fit` 提供的实现。满足检查、快速满足查询和 witness 选择必须复用
同一个兼容条件，不能出现“满足检查通过但 witness 选择到不兼容成员”
或相反的结果。

公开 requirement 已找到结构匹配但仅存在 `type seal` 成员时，应报告
实现成员可见性不足，而不是把该成员选为 witness。这同时修复当前公开
`spec` 成员可由 `type seal` 成员满足的问题。

`spec seal` 只限制 requirement 的 `spec` 访问面，不重新解释或修改
承担该 requirement 的具体实现成员。公开实现仍按具体 `type` 规则公开，
`seal` 实现仍按具体 `type` 规则受限。

### 3.3 `seal` 成员访问域

对声明 `seal` 成员 `M` 的 object-form `spec S`，访问 `M` 必须同时满足：

```text
1. 访问点位于 type T 自身的成员方法、静态方法，或 T 的 fit 扩展方法
   实现上下文；
2. T 在访问点满足 S；
3. receiver 使用能够暴露 M 的 spec 静态视角；
4. M 是该 spec 视角中由 S 声明或继承的 seal 成员。
```

这里的类型实现上下文识别沿用语义分析现有上下文。对 `fit` 方法，只在
上述 `spec seal` 访问判断中把其目标 `type` 记为 `T`；这不会把 `fit`
方法体变成目标 `type` 自身，也不会使其通过具体 `type` 视角访问目标
`type` 的 `seal` 成员。权限不依赖是否存在 `self`，所以实例方法、静态
方法和 `fit` 方法使用同一套 `spec seal` 判断。

```feng
open spec Widget {
  seal func draw(): void;
}

open type Button: Widget {
  seal func draw(): void {
    // Button 的具体实现。
  }

  func refresh(other: Widget): void {
    self.draw();   // 按既有 type seal 规则访问 Button.draw
    other.draw();  // 按本草案规则访问 Widget.draw，并经 witness 分派
  }

  static func refreshAll(other: Widget): void {
    other.draw();  // 允许：静态方法仍属于满足 Widget 的 Button
  }
}

func renderNow(widget: Widget): void {
  widget.draw();   // 错误：顶层函数不属于任何实现 Widget 的 type
}

fit Button {
  func refreshFromFit(other: Widget): void {
    other.draw();  // 允许：fit 的目标 Button 满足 Widget，通过 spec 视角访问
    self.draw();   // 错误：fit 不能直接访问 Button 的 seal 成员
  }
}
```

同一个 `spec` 的其他实现类型也只能通过 `spec` 视角获得新增权限：

```feng
open type Icon: Widget {
  seal func draw(): void {
    // Icon 的具体实现。
  }

  func refresh(widget: Widget, button: Button): void {
    widget.draw(); // 允许：Widget 视角，经 witness 分派
    button.draw(); // 错误：不能直接访问 Button 的 seal 成员
  }
}
```

因此，本能力建立的是：

```text
Widget.draw -> witness -> Button.draw / Icon.draw
```

而不是：

```text
实现 Widget -> 扩大 Button.draw / Icon.draw 的具体 type 可见性
```

### 3.4 实例成员与静态成员

实例字段、实例方法、静态字段和静态方法使用相同的 `spec seal` 访问域
判定。

- 实例成员必须通过 `spec` 实例值访问。
- 静态成员继续使用现有静态 spec 约束视角访问。
- 具体类型名仍只表达具体 `type` 视角，不因该类型实现某个 `spec` 而
  自动选择或转换为静态 spec 视角。

本草案不增加新的静态成员访问语法，也不枚举具体类型实现的全部
`spec` 来选择访问权限。

### 3.5 父 `spec` 成员

子 object-form `spec` 完整继承父 `spec` 的无修饰成员和 `seal` 成员。
继承成员保留原声明可见性，访问域以成员原声明 `spec` 为准。

实现子 `spec` 的类型按现有名义关系同时满足父 `spec`，因此可以在自身
成员方法、静态方法及其 `fit` 扩展方法中，通过相应 `spec` 视角访问父
`spec` 的 `seal` 成员。仅实现父 `spec` 不会获得子 `spec` 成员的访问
权限。

### 3.6 成员查找与重载

`spec` 成员查找必须先按当前访问点过滤不可访问的 `seal` 候选，再使用
现有字段查找、方法重载和歧义诊断规则：

- 存在可访问同名成员时，继续按现有规则解析；
- 同名成员存在但全部不可访问时，报告访问错误；
- 完全不存在同名成员时，保持现有“成员不存在”诊断；
- 不改变现有重载签名、重复声明和冲突规则。

这只把 `spec seal` 接入现有访问过滤，不重新设计重载系统。

## 4 `fit` 边界

`fit` 继续使用现有规则建立满足关系或提供扩展方法。本草案不修改
`fit` 的类型身份和目标 `type` 私有访问权限，只把 `fit` 的目标 `type`
纳入 `spec seal` 访问域判断：

- `fit` 方法体不是目标 `type` 自身；
- `fit` 方法体不能访问目标 `type` 的 `seal` 成员；
- 当目标 `type T` 在访问点满足 `S` 时，`fit` 方法体可以通过 `S` 的
  spec 视角访问 `S` 的 `seal` 成员；
- 该权限只作用于 spec 成员访问和 witness 分派，不能作为访问目标
  `type seal` 成员的依据；
- `fit` 对 requirement 的满足检查使用第 3.2 节的同一可见性兼容规则。

`T` 是否满足 `S` 继续由访问点可见的现有满足关系查询决定，包括直接
声明和 `fit` 建立的关系。该查询既用于 `T` 自身成员方法和静态方法，
也用于目标为 `T` 的 `fit` 方法，但不会把 `fit` 方法体变成 `T` 的实现
上下文。

## 5 跨包与 `.ft`

### 5.1 跨包访问原则

通过 `spec` 视角访问 `seal` requirement 时，编译期只检查：

- receiver 的静态 `spec` 视角；
- 访问点所属 `type`，或当前 `fit` 的目标 `type`；
- 该 `type` 在当前位置是否满足成员原声明 `spec`。

运行时接收者的具体类型及其所在包不参与访问判定。合法调用继续通过
`spec` 值携带的既有 witness 分派到实际实现。该分派不要求、也不允许
调用点通过具体类型成员查找看到实现成员。

具体 `type` 的跨包成员可见面和跨包 `fit` 候选继续保持现状。
为使已导出的名义满足关系能够在 consumer 中构造现有 witness，
声明期已选中的非公开实现方法可以作为编译器 ABI 依赖进入
package-public `.ft`，但必须保持 `seal`，不得因此升级为
Feng 公开 API。

### 5.2 声明期实现依赖选择

现有 package-public `.ft` 已经保存：

- object-form `spec` 的完整成员序列仍全部进入现有 spec 声明骨架；
- 已导出 `type`、`fit` 与 `spec` 的现有名义 relation；
- 已收录 `type` 的全部字段骨架，包括实例、静态、公开和
  `seal` 字段。

本次只补齐方法实现依赖。对每个进入 package-public `.ft` 的
`type/fit -> spec` 名义关系，声明期满足检查选中的实现方法
按以下统一规则处理：

- 公开实现方法继续按现有规则进入 `.ft`；
- `seal` 实现方法作为该名义关系的编译器 ABI 依赖进入
  `.ft`；
- 实例方法、静态方法、`type` 自有方法、`fit` 方法和编译器
  生成 wrapper 使用同一规则；
- 父 `spec` 闭包中 requirement 选中的实现方法使用同一规则；
- 未被任何已导出名义关系选中的无关具体 `type`/`fit seal`
  实现方法不因本次
  修复进入 package-public `.ft`。

"选中"必须是声明期契约满足检查的结果，不得在 `.ft` writer
中按方法名、`requestReflow`、`@mixable` 或其他具体场景重新推断。
满足检查、witness 选择、`.ft` 选择与 provider Codegen 必须消费同一份
统一语义事实。该事实只存在于 provider 当次编译的 Semantic sidecar，
不写入 `.ft`。

consumer 仍以现有名义 relation 判定关系是否可用，并只在为该
已证明关系构造 witness 时使用上述非公开实现方法。方法存在于
`.ft` 不是新的结构满足授权，外包不得因此使用自定义 spec
或 `fit` 选择导入 type 的 `seal` 成员。

本次不新增 `.ft` section、flag、relation 或版本，也不修改已有编解码
布局。具体 package-public 成员选择规则统一见
[`feng-symbol-table.md`](../specifications/feng-symbol-table.md)。

### 5.3 编译器 ABI 链接

被选中的 `seal` 实现成员不建立新的链接机制，而是直接复用对应
`open` 成员的现有发码、符号命名、声明和链接路径。两者唯一保留的
差异是源码 AST 和 `.ft` 中记录的语言可见性：前者仍为 `seal`，
语义分析、名称查找、补全和文档仍按 `seal` 拒绝普通访问。

具体规则如下：

- provider Codegen 将声明期选中的非泛型 `seal` 实例方法和静态方法
  纳入现有 package callable surface，后续处理与 `open` 方法相同；
- 非泛型 consumer 继续使用现有稳定方法符号和 witness 调用路径，
  不根据使用点是否已物化 witness 再次推断；
- 不为 `spec seal` 新增独立符号前缀、序号域、thunk 种类或 ABI；
  非泛型 `type`、非泛型 `fit` 和生成 wrapper 均保持现有 `open`
  成员路径的行为；
- 实例字段继续由现有布局骨架和 witness 字段访问路径处理，
  不新增实现符号。

`seal static` 字段的声明骨架虽已进入 `.ft`，但其 provider storage 与
`ensure_init` 当前仍只按公开字段获得跨包链接能力。该问题在本次方法
依赖修复前已经存在，不由“选中的 seal 方法进入 `.ft`”引入；对应验证
用例保留并暂停，后续作为独立静态字段链接问题处理。

workspace-cache `.ft` 继续只服务现有工具链用途，不参与本次 package
Codegen 判定。本次不修改运行时 witness 布局、槽位、调用层级或分派
路径，因此不增加运行时开销。

泛型 owner 的 package-public `.ft` 还可能因 reified callable dependency
收录普通 `seal` helper。因此，consumer 不能仅根据“某个 `seal` 方法出现
在 package-public `.ft`”就把它划入公开泛型符号序号域。泛型 owner、
泛型实现方法和泛型 `fit` 的跨包符号分类不在本次最小修复中增加特判或
新 FT 标记；对应验证用例保留并暂停，后续独立分析。

object-form `spec` 的方法级类型参数还有一个更早的既有解析问题：spec
成员签名当前没有像 type/fit 方法一样进入统一 callable 类型参数作用域，
所以无修饰（公开）与 `seal` 的方法级泛型都会在声明解析阶段对参数或
返回值中的类型参数报告 `AE1013`。该问题发生在契约满足和本次实现依赖
选择之前，不属于本次跨包方法依赖修复。

## 6 编译器实现影响

### 6.1 语法与 AST

- object-form `spec` 的实例字段、实例方法、静态字段和静态方法接受
  `seal` 修饰符。
- object-form `spec` 成员允许显式 `open` 或 `seal`；`SE0601` 不再用于
  拒绝显式 `open`。
- AST 原样保留无修饰、显式 `open` 与显式 `seal` 的可见性事实，不在
  Parser 中归一化。
- 复用 `FengTypeMember` 现有可见性字段，不增加新的 AST 节点或属性。

### 6.2 语义分析

- 完整 requirement 集合继续包含 object-form `spec` 的全部成员。
- 满足检查、快速满足查询和 witness 构造统一应用第 3.2 节的实现成员
  可见性兼容条件。
- 契约满足选择成功时，记录“名义关系、requirement、被选中
  实现成员及实现来源”的统一 Semantic sidecar 事实。该事实
  同时服务于 witness、package-public 依赖选择和 provider Codegen，
  不得为 `.ft`、Codegen 或特定成员另写一套匹配逻辑。
- 该 sidecar 按关系来源保留选择结果，不能只使用“某成员曾参与
  任意 spec 满足”的无主体布尔标记；只有进入 package-public
  `.ft` 的名义关系才能导出其非公开实现依赖。
- 不得使用“某个使用点已构造 witness”作为是否导出的条件；
  即使 provider 内部没有发生 spec coercion，已导出关系的必需实现
  依赖也必须完整。
- 新增统一的 `spec` 成员访问判断，接收成员原声明 `spec`、receiver 的
  spec 视角，以及当前 type 实现上下文或当前 fit 的目标 type。
- 实例/静态字段访问、字段写入和方法调用使用同一访问判断。
- 方法重载在现有解析前过滤当前不可访问的 `spec seal` 候选。
- 不调用具体 `type` 成员可见性判断来授权 `spec seal`，也不反向修改
  具体 `type` 成员可见性。
- 现有具体 `type seal` 成员访问判断保持不变，不能把当前 fit 的目标
  type 当作当前 type 来通过该判断。

### 6.3 导入导出

- 复用 `.ft` 现有成员可见性编码保存 `spec seal`。
- 导入恢复后的 `FengTypeMember.visibility` 必须与源码 AST 一致。
- 不增加逐成员 witness relation。
- package-public writer 使用第 6.2 节的统一 sidecar，收录已导出
  名义关系选中的 `seal` 实例/静态方法、`type`/`fit` 方法和
  生成 wrapper。
- 字段继续使用已有的完整骨架选择，不修改 writer 的字段规则。
- 未被已导出名义关系选中的无关具体 `type`/`fit seal`
  实现方法继续不进入
  package-public `.ft`。
- 不增加 `.ft` 字段、属性、section、relation 或格式版本。

### 6.4 代码生成与运行时

- 合法的 `spec seal` 成员访问继续使用现有 spec member access 记录和
  witness 分派。
- 可见性检查在语义阶段完成。
- 已导出名义关系选中的非泛型 `seal` 实例/静态方法加入现有 package
  callable surface，完全复用 `open` 方法的发码和链接路径。
- provider 通过声明期 sidecar 识别选中成员；非泛型 consumer 复用现有
  稳定符号和 witness 调用路径。不得增加 `spec seal` 专用符号域或按
  materialized witness 重新分类。
- 泛型 owner、泛型方法和泛型 `fit` 的跨包符号分类留待独立分析，
  本次不得以“导入的 seal 方法均为 spec 实现依赖”的过宽规则处理。
- 不增加运行时访问检查、wrapper、box、分支或 ABI 数据结构。
- 不改变 Feng 层的具体 type 成员可见性，不增加运行时开销。

### 6.5 工具链

LSP、补全和符号展示只需对 `spec` 成员应用相同访问判断：

- 普通位置不把 `spec seal` 成员展示为可访问成员；
- 满足该 `spec` 的 `type` 成员方法、静态方法及其 fit 扩展方法中，可以
  在对应 spec 视角补全该成员；
- 不改变具体 `type` 的成员补全和可见性规则；
- API 文档不得把 `spec seal` 成员标记为普通公开 API，但可以作为契约
  requirement 展示。

## 7 诊断要求

至少覆盖以下失败：

- 公开 requirement 仅找到结构匹配的 `type seal` 实现成员；
- 普通函数通过 spec 值访问 `seal` 字段或方法；
- 未满足目标 spec 的 type 方法访问其 `seal` 成员；
- 目标 type 未满足目标 spec 的 fit 方法访问其 `seal` 成员；
- 满足同一 spec 的 type 通过其他具体 type 视角访问对应实现的
  `seal` 成员；
- `fit` 方法体通过具体 type 视角访问目标 type 的 `seal` 成员；
- 静态访问没有使用现有静态 spec 约束视角；
- `.ft` 恢复后把 `spec seal` 错误恢复为公开成员。

诊断应区分：

1. 成员存在，但当前访问点不属于该 `spec seal` 成员的访问域；
2. 当前 spec 成员面中不存在该成员或没有匹配的重载。

公开 requirement 已有结构匹配成员但其实现可见性为 `seal` 时，诊断应
明确表达实现成员可见性不足，并与完全缺少实现成员区分。

## 8 测试矩阵

| 场景 | 预期 |
| --- | --- |
| object-form spec 声明实例/静态 `seal` 字段与方法 | 通过 |
| 无修饰 spec 成员保持公开 | 通过 |
| 显式 `open` spec 实例/静态字段与方法 | 通过，与省略修饰符语义等价 |
| `seal` requirement 参与 type 满足检查和 witness 构造 | 通过 |
| open/default type 成员承担 `seal` requirement | 通过 |
| type seal 成员承担 `seal` requirement | 通过 |
| 公开 requirement 由 open/default type 成员满足 | 通过 |
| 公开 requirement 由 type seal 成员满足 | 拒绝，并报告实现可见性不足 |
| 普通函数通过 spec 值访问 seal requirement | 拒绝 |
| 满足 spec 的 type 实例方法通过 spec 视角访问 seal requirement | 通过 |
| 满足 spec 的 type 静态方法通过 spec 视角访问 seal requirement | 通过 |
| 满足 spec 的 type 通过另一个 spec 值访问 seal requirement | 通过 |
| 未满足 spec 的 type 方法访问 seal requirement | 拒绝 |
| 实现同一 spec 的 type 直接访问其他具体 type 的 seal 实现 | 按既有 type 规则拒绝 |
| 目标 type 满足 spec 的 `fit` 方法通过 spec 视角访问 seal requirement | 通过 |
| 目标 type 未满足 spec 的 `fit` 方法访问 seal requirement | 拒绝 |
| `fit` 方法通过具体 type 视角访问目标 type 的 seal 成员 | 按既有 type 规则拒绝 |
| 子 spec 继承父 spec 的公开与 seal 成员 | 完整继承 |
| 实现子 spec 的 type 访问父 spec seal 成员 | 通过 |
| 仅实现父 spec 的 type 访问子 spec seal 成员 | 拒绝 |
| 公开与 seal 同名重载同时存在 | 先过滤访问权限，再按现有规则解析 |
| `.ft` 写入并恢复 spec seal 成员 | 保持 seal |
| 已导出 `type: spec` 关系选中 seal 实例/静态方法 | 方法以 seal 进入 `.ft`，witness 跨包可执行 |
| 已导出 `open fit Type: Spec` 选中 fit 的 seal 实例/静态方法 | 方法以 seal 进入 `.ft`，导入 fit 后 witness 可执行 |
| 编译器生成的 seal 实例 wrapper 被选中 | wrapper 以 seal 进入 `.ft`，witness 跨包可执行 |
| 父 spec 选中 seal 方法 | 复用 open 发码路径并正常执行 |
| 泛型 owner、泛型方法或泛型 fit 选中 seal 方法 | 保留验证用例，待独立分析现有泛型导入与符号面问题 |
| 已导出 type 中与 spec 实现无关的普通 seal 方法 | 不因本次修复进入 package-public `.ft` |
| seal 实例字段 | 继续使用已有 `.ft` 字段骨架，跨包 witness 正常执行 |
| seal static 字段 | `.ft` 骨架已存在；跨包 storage/ensure 链接用例保留，待独立修复 |
| 外包自定义 spec/fit 尝试选择导入 type 的 seal 成员 | 继续拒绝 |

编译器测试应关注 AST 可见性、诊断码、spec member access 和 witness；
FCTS 验证能够执行的正向语言行为。涉及 `.ft` 的测试还必须区分
“被已导出 spec 关系选中的编译器 ABI 依赖”与“无关 seal
成员”，并验证前者可用于 witness、后者仍不进入 package-public
方法骨架。

## 9 权威规范更新

正式实现前，应按本文已经确认的范围更新：

- `docs/specifications/feng-spec.md`：允许 object-form `spec seal` 成员，
  定义公开访问面、完整契约、实现成员可见性兼容规则、实现 type 与 fit
  访问域和现有 witness 复用；
- `docs/specifications/feng-visibility.md`：定义 `spec seal` 只约束 spec
  视角，不改变具体 type 成员可见性；
- `docs/specifications/feng-symbol-table.md`：确认复用现有成员可见性编码，
  已导出 spec 关系选中的 seal 实现方法作为编译器 ABI
  依赖进入 package-public `.ft`，且不修改 `.ft` 格式和 relation；
- `docs/specifications/feng-error-codes-se.md`：将不再有触发场景的 `SE0601`
  标记为失效；
- `docs/specifications/feng-error-codes-ae.md`：定义实现成员可见性不兼容和
  spec seal 成员越权访问诊断。

`docs/specifications/feng-fit.md`：明确 fit 不是目标 type 自身，不能直接
访问目标 type 的 seal 成员；同时，fit 以其目标 type 参与 `spec seal`
访问域判断，可以通过 spec 视角访问目标 type 所满足 spec 的 seal 成员。

权威规范只定义上述新增规则，并随本能力修复公开 requirement 可由
`type seal` 成员满足的问题；不顺带修改任何具体 type 成员可见性、
fit 的目标 type 私有访问权、跨包可见面或运行时行为。

## 10 实施 TODO

### 10.1 已完成的 `spec seal` 核心能力

- [x] 更新 `feng-spec.md`、`feng-visibility.md`、`feng-fit.md`、
  `feng-symbol-table.md` 及 SE/AE 诊断规范。
- [x] Parser 允许 object-form spec 成员使用 `open` 或 `seal`，并原样保留
  `DEFAULT`、`PUBLIC` 与 `PRIVATE` AST 事实；Semantic 将无修饰与显式
  `open` 统一解释为公开 requirement。
- [x] `.ft` 符号视图保存并恢复 spec 成员已有的可见性，不修改格式、版本
  或 relation。
- [x] 为 requirement 与实现成员增加统一可见性兼容判断，并在满足验证、
  快速满足查询和 witness 选择中复用。
- [x] 修复公开 spec requirement 可由同名同结构 `type seal` 成员自动满足
  的现有问题；字段、方法、实例成员、静态成员和 fit 实现使用同一规则。
- [x] 在实例/静态字段访问、字段写入、方法调用和重载解析入口执行
  spec seal 访问域检查。
- [x] 允许满足目标 spec 的 type 成员方法、静态方法及其 fit 扩展方法通过
  spec 视角访问 seal 成员；拒绝普通函数和未满足目标 spec 的上下文。
- [x] 保持具体 type 成员查找、type seal 可见性、跨包 fit 可见面、witness
  运行时结构和代码生成分派形式不变。
- [x] 补齐 Parser、AST、semantic、witness、`.ft` 往返、跨包、LSP 和 FCTS
      测试，并执行 `make test` 全量回归。

### 10.2 跨包编译器 ABI 依赖修复

下列任务中，“实际变更”会修改编译器行为；“验证”只补齐或执行
用例，不得借机改变其他语义。

- [x] **实际变更（文档）**：将方案收敛为“已导出名义关系选中的
  seal 实现方法进入 `.ft` 并具备编译器 ABI 链接能力”，明确
  不增加 witness plan、`.ft` 格式或运行时机制。
- [x] **实际变更（Semantic）**：让声明期契约满足选择生成关系级
  实现成员 sidecar，记录 requirement、确切实现成员和 `type`/`fit`
  实现来源；复用现有统一可见性与签名匹配，不得增加
  `requestReflow`、`@mixable` 或成员名特判。
- [x] **实际变更（Symbol）**：package-public 选择复用同一 sidecar，只让
  已导出名义关系选中的 `seal` 实例/静态方法、`type`/`fit`
  方法和生成 wrapper 进入 `.ft`，并原样保留 `seal`。
- [x] **实际变更（Codegen，非泛型）**：把上述非泛型方法加入现有
  package callable surface，直接复用 `open` 方法的发码、符号命名和
  链接路径；未新增 `spec seal` 专用符号前缀、序号域、thunk 或按
  materialized witness 推断的分类。
- [x] **验证（Codegen，实例字段）**：不改变字段 `.ft` 选择；实例字段
  继续使用已有布局骨架和 witness 字段访问路径。
- [ ] **待独立分析（保留禁用用例）**：`seal static` 字段虽然已进入
  `.ft`，provider storage/ensure 尚未获得跨包链接能力；该问题不由
  本次方法依赖修复引入，不得在本次方法修复中顺带修改。
- [x] **验证（现有行为）**：确认 object-form `spec` 全部成员、`type`
  全部实例/静态字段以及现有名义 relation 已进入 `.ft`，不修改
  其编解码或选择逻辑。
- [x] **验证（需新增编译器用例）**：覆盖 `type: spec` 与已导入的
  `open fit Type: Spec`
  选中的 seal 实例方法、静态方法和实例字段，并执行跨包 witness
  调用/读写；静态字段按上一项单独保留。
- [x] **验证（需新增用例）**：覆盖父 spec 与 `@mixable seal static`
  生成实例 wrapper；这些任务只验证现有 `open` 通用路径已被新选择
  事实覆盖，不修改该路径的既有行为。
- [ ] **待独立分析（保留禁用用例）**：泛型 owner、泛型方法、泛型
  `fit` 及 reified dependencies 的跨包符号分类；不得把 package-public
  `.ft` 中因其他编译器依赖出现的普通 `seal` helper 误判为 spec
  实现依赖。
- [x] **验证（需新增用例）**：确认无关普通 seal 方法仍不进入
  package-public `.ft`，普通访问仍被 `seal` 拒绝，外包自定义
  spec/fit 仍不能选择导入 type 的 seal 成员。
- [x] **验证（需新增 FCTS 用例）**：通过 `fcts_lib -> fcts_bin` 包边界
  覆盖非泛型 `type: spec`、`open fit Type: Spec`、父 spec、seal 实例/
  静态实现方法及 `@mixable seal static` 生成 wrapper 的 witness 调用。
- [x] **验证（回归）**：先执行定向 symbol、semantic、codegen、CLI 与 FCTS
  用例，确认编译 consumer 使用 package-public 而非 workspace-cache
  `.ft`，再在沙箱外执行 `make test` 全量回归。
