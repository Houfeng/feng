# `spec` 支持定义 `seal` 成员实现草案

> 本文档是实现草案，不是语言权威规范。
> 本文确认的规则已经写入
> [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 等权威规范；本文
> 继续用于跟踪实现范围和实施 TODO，最终语言语义以权威规范为准。

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
- 不修改 `.ft` 格式、版本或 relation 模型。
- 不把成员级 `seal` 扩展到 callable-form、union-form 或
  intersection-form `spec`。
- 不引入 `protected`、逐项 `friend` 或其他新的访问控制模型。

## 3 核心语义

### 3.1 `spec` 成员可见性

object-form `spec` 成员具有以下有效可见性：

```text
effective_visibility(无修饰 spec 成员) = open
effective_visibility(seal spec 成员)   = seal
```

本次只新增 `seal` 写法。显式 `open` 是否允许不属于本需求，继续维持当前
语法规则：`spec` 成员不能显式声明 `open`。

object-form `spec` 因此具有两个用途不同的成员集合：

| 集合 | 内容 | 用途 |
| --- | --- | --- |
| 公开访问面 | 无修饰成员 | 普通 `spec` 使用者访问 |
| 完整契约 | 无修饰成员与 `seal` 成员 | 满足检查、witness 构造和实现域访问 |

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

具体 `type` 的跨包成员可见面、`type seal` 成员导出和跨包 `fit` 候选
继续保持现状。本草案不要求为 `spec seal` 额外导出任何具体 `type`
成员，也不允许把具体实现成员升级为公开 API。

### 5.2 `.ft` 复用现有格式

`.ft` 已有成员可见性表示，object-form `spec` 的成员也属于现有声明
骨架。本能力只要求使用现有字段忠实保存和恢复 `spec` 成员可见性：

- 无修饰成员按现有公开成员写入和恢复；
- `seal` 成员使用现有非公开可见性表示写入和恢复；
- object-form `spec` 的完整成员序列仍全部进入现有 spec 声明骨架；
- 继续使用现有 type—spec relation 和 witness 构造机制；
- 不增加 `.ft` 字段、flag、relation、格式版本或兼容分支；
- 不改变具体 `type` 成员的 `.ft` 选择和可见性规则。

`.ft` 的既有编解码逻辑无需修改。构建待导出的 spec 符号视图时，不得
把成员可见性统一归一化为公开；应将 AST 中已有的成员可见性原样交给
现有编码。

该调整只发生在 spec 符号视图构建阶段，属于保存新增 `spec seal` 语义，
不是 `.ft` 格式或编解码逻辑变更。

## 6 编译器实现影响

### 6.1 语法与 AST

- object-form `spec` 的实例字段、实例方法、静态字段和静态方法接受
  `seal` 修饰符。
- `SE0601` 收窄为继续拒绝显式 `open` 和非 object-form 场景中的成员
  可见性，不再拒绝合法的 object-form `seal` 成员。
- 无修饰成员继续使用当前默认可见性。
- 复用 `FengTypeMember` 现有可见性字段，不增加新的 AST 节点或属性。

### 6.2 语义分析

- 完整 requirement 集合继续包含 object-form `spec` 的全部成员。
- 满足检查、快速满足查询和 witness 构造统一应用第 3.2 节的实现成员
  可见性兼容条件。
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
- 不改变具体 `type` 私有成员的导出选择。

### 6.4 代码生成与运行时

- 合法的 `spec seal` 成员访问继续使用现有 spec member access 记录和
  witness 分派。
- 可见性检查在语义阶段完成。
- 不增加运行时访问检查、wrapper、box、分支或 ABI 数据结构。
- 不改变具体 type 方法、字段或静态成员的链接可见性。

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
| 显式 `open` spec 成员 | 按现有规则拒绝 |
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
| `.ft` 因 spec seal 改变具体 type 私有成员选择 | 不得发生 |

编译器测试应关注 AST 可见性、诊断码、spec member access 和 witness；
FCTS 只验证能够执行的正向语言行为。涉及 `.ft` 的测试只验证 spec 成员
可见性往返，不扩展到具体 `type seal` 成员导出。

## 9 权威规范更新

正式实现前，应按本文已经确认的范围更新：

- `docs/specifications/feng-spec.md`：允许 object-form `spec seal` 成员，
  定义公开访问面、完整契约、实现成员可见性兼容规则、实现 type 与 fit
  访问域和现有 witness 复用；
- `docs/specifications/feng-visibility.md`：定义 `spec seal` 只约束 spec
  视角，不改变具体 type 成员可见性；
- `docs/specifications/feng-symbol-table.md`：确认复用现有成员可见性编码，
  不修改 `.ft` 格式和 relation；
- `docs/specifications/feng-error-codes-se.md`：收窄 `SE0601`，只拒绝 spec
  成员显式 `open`；
- `docs/specifications/feng-error-codes-ae.md`：定义实现成员可见性不兼容和
  spec seal 成员越权访问诊断。

`docs/specifications/feng-fit.md`：明确 fit 不是目标 type 自身，不能直接
访问目标 type 的 seal 成员；同时，fit 以其目标 type 参与 `spec seal`
访问域判断，可以通过 spec 视角访问目标 type 所满足 spec 的 seal 成员。

权威规范只定义上述新增规则，并随本能力修复公开 requirement 可由
`type seal` 成员满足的问题；不顺带修改任何具体 type 成员可见性、
fit 的目标 type 私有访问权、跨包可见面或运行时行为。

## 10 实施 TODO

- [x] 更新 `feng-spec.md`、`feng-visibility.md`、`feng-fit.md`、
  `feng-symbol-table.md` 及 SE/AE 诊断规范。
- [x] Parser 仅允许 object-form spec 成员使用 `seal`，保持无修饰成员为
  公开语义，并继续拒绝显式 `open`。
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
