# `spec` 支持定义 `seal` 成员实现草案

> 本文档是实现草案，不是语言权威规范。
> 当前 [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 明确规定 `spec` 成员不得声明 `open` 或 `seal`；本文描述的是待确认并写入语言规范的新能力。正式实现前，必须先更新 `feng-spec.md`、[Feng 语言 `fit` 规范](../specifications/feng-fit.md) 与相关可见性规范。

## 1 背景

object-form `spec` 目前只能声明公开成员。成员一旦成为契约 requirement，普通代码便可通过 `spec` 视角调用。这无法直接表达以下常见框架协议：

```feng
open spec Widget {
  func show(): void;
  seal func draw(): void;
}
```

其中：

- `show` 是面向普通使用者的公开 API；
- `draw` 是所有 `Widget` 实现者必须提供的框架入口；
- 外部必须能看到并实现 `draw` 的签名；
- 普通代码不应主动调用 `draw`，以免绕过框架生命周期或状态机；
- `Widget` 实现域中的代码可以调用 `draw`。

该需求的关键不是“隐藏 requirement”，而是分离两个当前绑定在一起的维度：

```text
requirement visibility  谁能看到并实现成员
member accessibility    谁能访问成员
```

`seal` spec 成员因此是一种“公开可实现、受限可访问”的契约成员，而不是 spec 内部的私有辅助成员。
其核心规则可收敛为一句话：

> `spec` 的 `seal` 成员，所有实现该 `spec` 的 `type` 都能通过该
> `spec` 视角访问。

## 2 目标

- 允许 object-form `spec` 的实例成员和静态成员声明 `seal`
  requirement，覆盖字段与方法。
- `seal` requirement 仍参与契约满足检查和 witness 构造。
- 普通访问侧不能通过具体类型或 `spec` 视角访问该成员。
- 满足该 `spec` 的类型进入同一个受限访问域，并可通过该 `spec`
  视角访问其 `seal` 成员。
- 子 `spec` 按现有继承规则完整继承父 `spec` 的公开成员和 `seal`
  成员。
- `open spec` 明确允许包外类型进入该访问域；封闭的 `spec` 不允许包外加入。
- 跨包 `fit` 只能基于当前可见类型面建立关系，不能借此发现或暴露目标类型未导出的 `seal` 成员。
- `.ft` 同时传播 `spec` 的 `seal` requirement、承担该 requirement 的
  `type` `seal` 成员及其 witness 映射；进入 `.ft` 不改变成员的
  `seal` 可见性，也不使其进入具体 `type` 的跨包成员访问面。

## 3 非目标

- 不引入基于继承关系的 `protected`。
- 不引入 C++ 式逐项 `friend` 声明。
- 不允许 `fit` 获得目标 `type` 定义体的特殊私有访问权。
- 不允许运行时绕过编译期可见性检查。
- 不把 `seal` 扩展到 callable-form、union-form 或 intersection-form；本文只定义
  object-form `spec` 的成员可见性。

## 4 核心语义

### 4.1 两层成员面

object-form `spec` 同时具有两层成员面：

| 成员面 | 内容 | 用途 |
| --- | --- | --- |
| 公开访问面 | 默认公开的成员 | 普通 `spec` 使用者可访问 |
| 完整实现契约 | 公开成员与 `seal` requirement | 满足检查、witness 构造和受限域访问 |

以 `Widget` 为例：

```text
public access surface(Widget) = { show }
implementation requirements(Widget) = { show, draw }
```

因此，“调用侧看不到 `draw`”不等于“实现侧不知道 `draw`”。

### 4.2 访问域

对 object-form `spec S`，定义：

```text
domain(S) = { T | T 在当前位置满足 S }
```

`S` 的 `seal` 成员只允许在 `domain(S)` 中的类型实现上下文访问。普通顶层函数、普通外部类型和仅持有 `S` 值的调用侧均不获得权限。

domain 是 `spec` `seal` 成员的访问控制属性，与泛型无关。泛型只按现有
规则进行类型替换和契约满足检查；不按泛型实参或实例化结果
拆分 domain。

权限只作用于 `spec` 成员访问，不扩大具体 `type` 实现成员的可见性。
对实例成员，访问必须同时满足：

```text
receiver 的静态视角是 spec S
目标成员是 S 的 seal 成员
当前声明所在 type 满足 S
```

即使 `type T` 的 `seal` 成员 `I` 实现了 `S` 的 `seal` requirement，
`I` 作为具体 `T` 成员时仍仅保持原有 type 私有语义；其他实现
`S` 的类型不得通过 `T` 视角直接访问 `I`。同一契约行为必须先以
`S` 视角表达，再通过 `S` 的 witness 分派到 `I`。

```feng
open spec Widget {
  seal func draw(): void;
}

open type Button: Widget {
  seal func draw(): void {
    // ...
  }

  func refresh(other: Widget): void {
    self.draw();   // 允许：Button 访问自身 type 私有成员
    other.draw();  // 允许：调用点位于 Widget 的实现域
  }
}

func renderNow(widget: Widget): void {
  widget.draw();   // 错误：普通调用点不属于 domain(Widget)
}
```

访问权限由访问点的声明上下文和 receiver 的静态 `spec` 视角共同决定，
而不是由运行时接收者的具体类型决定。权限检查应在编译期完成，
不增加运行时访问检查。具体类型值若需使用契约访问，必须先按现有
规则转换或绑定为目标 `spec` 视角。

### 4.3 实例成员与静态成员

`seal` 适用于 object-form `spec` 当前支持的实例和静态成员：

- 实例 `let` / `var` 字段与实例 `func`；
- `static let` / `static var` 字段与 `static func`。

实例与静态 `seal` 成员共用相同的 domain 判定、契约满足与跨包
传播原则。静态成员不允许通过 `spec` 实例值访问；现有的
`T.member`（`T: S`）是已有的静态 spec 约束视角，可在当前声明所在
`type` 满足 `S` 时访问 `S` 的 `seal` 静态成员。这里的泛型类型参数
只是现有静态成员访问形式，不引入泛型 domain。

具体类型名当前只表达具体 `type` 视角，不自动转为某个 `spec` 的静态
视角。如果要让调用方对已知具体类型显式选择静态 spec 视角，还需要
确定相应的表达形式；实现阶段不得隐式枚举具体类型满足的所有
`spec` 来选择授权。

### 4.4 父 `spec` 成员继承

子 object-form `spec` 与继承公开成员一样，完整继承父 `spec` 的实例、
静态、公开和 `seal` 成员。继承成员保留原声明的可见性与 domain；
子 `spec` 使用不同可见性重新声明同签名成员的规则仍属于待确认边界。

实现子 `spec` 的类型按现有名义契约闭包同时满足父 `spec`，因而自然
进入父 `spec` 中所声明 `seal` 成员的 domain。该继承关系不允许反向
从父 `spec` 获得子 `spec` 成员的访问权限。

### 4.5 `open spec` 是显式授权

当 `seal` requirement 定义在 `open spec` 中时，spec 作者明确允许包外类型：

1. 看到该 requirement；
2. 实现该 requirement；
3. 通过满足关系加入该 spec 的受限访问域；
4. 在该访问域内调用该 requirement。

因此，第三方实现一个 `Widget` 后获得 `Widget` seal domain 的调用资格，是 `open spec Widget` 的直接语义结果，不属于可见性漏洞。

如果 spec 作者不允许包外代码加入该域，应封闭 spec 声明；如果具体类型本身也不应被包外适配或扩展，应同时收窄 type 的声明可见性。声明级 `seal spec` / `seal type` 与成员级 `seal` 是不同维度：前者控制谁能看到并加入声明，后者控制谁能访问成员。

## 5 `fit` 与可见类型面

### 5.1 基本原则

`fit` 是基于当前位置可见类型面建立契约关系或补充方法的机制，不是“进入目标 type 定义体”的能力。

对编译上下文 `C`，记目标类型的可见面为 `visible_shape(T, C)`。外置适配：

```text
fit T : S
```

只能在以下条件成立时通过：

```text
visible_shape(T, C) 能提供 requirements(S) 所需的全部实现或 witness
```

并且 `fit` 不得改变 `visible_shape(T, C)`。

换言之：

> `fit` 可以解释已有信息，不能揭示隐藏信息。

### 5.2 包内适配

当 `T` 的完整声明在当前包内可用时，契约满足检查可以使用编译器已经掌握的声明信息，确认现有 `seal` 成员是否承担目标 spec requirement：

```feng
type MyType {
  seal func draw(): void {
    // ...
  }
}

spec Drawable {
  seal func draw(): void;
}

fit MyType: Drawable;
```

该规则只说明满足检查能够识别声明，不授予 `fit` 块方法访问
`MyType` 其他私有成员的能力。合法的 `fit T: S` 实现上下文进入
`domain(S)`，因此可通过 `S` 视角访问 `S` 的 `seal` 成员；`fit`
方法体中的 `self` 仍不能因该关系通过具体 `MyType` 视角访问其他私有
成员。

### 5.3 跨包适配

如果 `MyType.seal draw` 没有实现任何对外契约，它不进入 `.ft`，包外看到的类型面中不存在该成员：

```feng
// package A
open type MyType {
  seal func draw(): void {
    // ...
  }
}
```

```feng
// package B
open spec Drawable {
  seal func draw(): void;
}

fit MyType: Drawable; // 错误：当前可见类型面无法证明 MyType 满足 Drawable
```

包 B 不能通过新建一个同签名 spec，使 package A 未导出的成员重新出现，也不能借 `self` 调用未进入可见面的成员：

```feng
fit MyType {
  func extra(): void {
    self.draw(); // 错误
  }
}
```

失败原因不是“目标 requirement 为 `seal`”，而是该 `type` 私有成员不属于
包外可用于满足检查或成员查找的具体类型面。包外 `fit` 不得通过
新建同签名 `spec` 重新发现该成员。

### 5.4 跨包 seal domain 访问

如果包 A 中的 `type` `seal` 成员已实现某个已导出 `spec` 的 `seal`
requirement，则该成员和 witness 映射进入 `.ft`，但该成员仍不进入具体
`type` 的跨包成员访问面。包 B 中的新 `type` 合法实现同一 `open spec`
后，进入该 spec domain，可通过该 `spec` 视角访问成员并沿 witness 分派到
包 A 中的实现。

```feng
// package A
open spec Drawable {
  seal func draw(): void;
}

open type Button: Drawable {
  seal func draw(): void {
    // ...
  }
}
```

```feng
// package B
open type Icon: Drawable {
  seal func draw(): void {
    // Icon 已进入 domain(Drawable)
  }

  func refresh(drawable: Drawable, button: Button): void {
    drawable.draw(); // 允许：receiver 是 Drawable 视角
    button.draw();   // 错误：receiver 是具体 Button 视角
  }
}

func refreshNow(button: Button): void {
  button.draw(); // 错误：普通调用点不属于 domain(Drawable)
}
```

调用方也可先按现有规则将 `Button` 值转换或绑定为 `Drawable` 视角，再访问
`Drawable.draw`。这是 spec domain 的跨包延续，不是将 `Button.draw` 变为
`open`，也不允许其他实现类型直接访问 `Button.draw`。

## 6 `.ft` 与符号信息传播

需要区分四类信息：

```text
declared members       类型完整定义中的成员
public members         普通跨包成员查找可见的成员
contract seal members  为 witness 恢复而导出且保留 seal 可见性的实现成员
contract bindings      requirement、实现成员与 witness 的映射
```

### 6.1 纯 type 私有成员

未承担对外契约的 `type` seal 成员：

- 不因本能力额外进入公开 `.ft`；若因布局等既有底层需求被收录，
  仍保持不可见；
- 不成为包外成员匹配候选；
- 不可被包外 `fit` 重新激活；
- 不可被工具误报为可访问 API。

### 6.2 实现已导出 seal requirement 的成员

当类型实现已导出的 spec seal requirement 时，公开 `.ft` 必须同时保留：

- spec `seal` requirement 的声明；
- 承担该 requirement 的 `type` 成员，并保留其 `seal` 可见性；
- requirement 与实现成员之间完成契约分派所需的 witness 映射。

例如：

```text
Button : Widget
  seal member Button.draw
  witness(Widget.draw) -> Button.draw
```

进入 `.ft` 不意味着成员变为 `open`。无权调用点的普通成员查找仍然
不得返回该成员：

```feng
button.draw(); // 在 domain(Widget) 之外仍然失败
```

处于 `domain(Widget)` 的合法访问必须先以 `Widget` 视角查找成员，再沿
契约 witness 分派；不得通过具体 `Button` 视角绕过该边界。

```text
Widget.draw -> witness -> Button implementation
```

核心原则是：

> 导出契约 witness 所需的 `seal` 实现成员，但不将该成员升级为
> 普通公开成员，也不为它追加 spec domain 成员访问权。

## 7 编译器实现影响

### 7.1 语法与 AST

- object-form spec 的实例字段、实例方法、静态字段和静态方法声明需要
  接受 `seal` 修饰符。
- 当前 `SE0601`“spec 成员不能声明可见性”的规则需要收窄，不能继续
  无条件拒绝 `seal` 成员。
- 默认无修饰成员继续进入公开访问面。
- 是否允许冗余的显式 `open`，本次对话未确认，正式规范必须单独决定。
- AST 和 `.ft` 恢复声明必须保留 spec requirement 的可见性。

### 7.2 语义分析

语义层需要分别产出：

- spec 的公开访问成员集合；
- spec 的完整 requirement 集合；
- 父 `spec` 闭包完整继承的公开与 `seal` 成员集合；
- receiver 是否以目标 `spec` 静态视角进行成员查找；
- 当前声明所在 `type` 是否满足目标 `spec`；
- `(T, S)` 的满足关系和逐成员 witness；
- 成员是普通公开成员、契约 `seal` 成员，还是未关联契约的 type 私有成员。

满足检查必须遍历完整 requirement 集合；普通成员访问只查询公开访问面；
seal-domain 访问必须在 receiver 已是目标 `spec` 视角且当前声明所在
`type` 满足该 `spec` 时才能通过，通过后沿既有 witness 查找实现。具体
`type` 成员查找不得枚举当前类型满足的 `spec` 来扩大私有成员可见性。
domain 计算不得引入泛型实例身份或运行时判定。

### 7.3 导入导出

- `open spec` 的 `seal` requirement 签名必须写入 `.ft`，否则外部无法实现契约。
- 承担已导出 spec `seal` requirement 的 `type` `seal` 成员必须写入
  `.ft`，并保留 `seal` 可见性。
- 已导出的满足关系需要携带足够的 requirement—实现成员 witness 映射。
- 实现类型未参与任何 spec `seal` requirement 的 type `seal` 成员不得因
  本能力变成公开 `.ft` 成员。
- `.ft` 已能分离符号收录与公开可见性；实现时还必须确认现有契约
  关系是否足以完整恢复 witness 映射，不足时再按符号表规范扩展。
- consumer 恢复声明后，必须保持“可实现、仅通过 spec 视角受限访问”
  的语义，不能退化为具体 `type` 公开成员或完全不可见成员。

### 7.4 代码生成与运行时

- 实例与静态 `seal` requirement 继续使用现有 spec 实例与静态成员的
  witness 分派机制。
- 可见性在语义阶段完成判定；运行时不需要增加访问控制结构或分支。
- 具体类型值进入 `spec` 视角时复用现有视角转换，不分配 wrapper 或 box；
  随后的成员访问按 `spec` 语义走 witness 分派，相比具体 `type` 直接调用
  可能多一次间接分派；本方案不为此增加专用优化或运行时特判。
- `seal` 不改变 requirement 的成员签名与既有分派形式；如果 `.ft` 需要
  追加实现成员 witness 映射，应按现有格式版本与兼容规则处理。

### 7.5 工具链

LSP、文档生成器和符号查询应区分：

- Public API；
- Implemented contracts；
- Contract hooks；
- Private implementation。

工具可以在 `Widget` 视角且当前声明所在 `type` 满足 `Widget` 时补全
`Widget.draw` requirement，但不得把具体实现的 `Button.draw` 作为普通可访问成员
补全给其他实现类型。

## 8 诊断要求

至少需要覆盖以下诊断：

- 普通代码调用 spec seal 成员；
- 普通代码直接调用具体类型的对应 seal 实现；
- 实现同一 spec 的其他 type 通过具体 type 视角访问其 seal 实现；
- 普通代码访问 spec 或具体类型的 seal 字段、静态字段或静态方法；
- 非满足类型的实现上下文调用 seal requirement；
- 跨包 `fit` 尝试用未导出的 type seal 成员满足 spec；
- `fit` 方法体通过 `self` 访问当前可见面之外的 seal 成员；
- `.ft` 恢复后丢失 witness 映射，或错误地把 contract seal 实现成员当作
  具体 `type` 的普通公开成员；
- 外部尝试满足不可见或封闭的 spec。

诊断信息应明确区分两类失败：

1. 成员存在，但当前调用点不属于访问域；
2. 当前可见类型面中不存在可用于满足检查的成员或 witness。

## 9 测试矩阵

| 场景 | 预期 |
| --- | --- |
| `open spec` 声明实例/静态 `seal` 字段与方法 | 通过 |
| 外部 type 实现 open spec 的实例/静态 seal requirement | 通过 |
| 普通函数通过 spec 值调用 seal requirement | 拒绝 |
| 满足 spec 的 type 方法内调用 seal requirement | 通过 |
| 满足 spec 的 type 方法内通过另一个 spec 值调用 seal requirement | 通过 |
| 满足 spec 的 type 内通过 spec 视角访问 seal 成员 | 通过 |
| 满足同一 spec 的 type 直接访问其他具体 type 的 seal 实现 | 拒绝 |
| 子 spec 继承父 spec 的公开与 seal 实例/静态成员 | 完整继承 |
| 实现子 spec 的 type 访问父 spec seal 成员 | 通过 |
| 仅实现父 spec 的 type 访问子 spec seal 成员 | 拒绝 |
| 外部 type 尝试实现不可见的 spec | 拒绝 |
| 包内 `fit T: S` 使用已知 type seal 成员完成满足检查 | 通过 |
| 跨包 `fit T: S` 尝试匹配未进入 `.ft` 的 type seal 成员 | 拒绝 |
| 跨包 `fit T` 方法中的 `self` 调用目标隐藏成员 | 拒绝 |
| 导出的 witness 在 consumer 中完成合法 seal-domain 分派 | 通过 |
| 跨包新 type 实现 open spec 后通过 spec 视角访问已导出实现 | 通过 |
| consumer 在 domain 外对 contract seal 实现做普通成员访问 | 拒绝 |
| 未实现 spec seal requirement 的 type seal 成员被包外 `fit` 重新激活 | 拒绝 |
| contract seal 实现进入 `.ft` 后变为普通公开成员 | 不得发生 |

测试应同时覆盖源码消费与 `.fb` / `.ft` 跨包消费，确保两条路径的成员面、满足关系和诊断一致。

## 10 与现行规范的变更关系

该能力与现行规范存在直接冲突，至少需要先修改：

- `docs/specifications/feng-spec.md`：放宽 object-form spec 实例/静态字段与方法的
  `seal` 限制，定义两层成员面、父级完整继承与访问域；
- `docs/specifications/feng-fit.md`：定义包内/跨包满足检查使用的可见类型面，并明确 `fit` 不扩大可见性；
- `docs/specifications/feng-visibility.md`：补充声明级 `open` / `seal` 与 spec 成员级 `seal` 的正交关系；
- `docs/specifications/feng-symbol-table.md`：定义 `.ft` 中 requirement、contract seal 实现成员、
  可见性与 witness 映射的传播边界；
- `docs/specifications/feng-error-codes-se.md`：调整 `SE0601` 的适用范围，并为非法调用或跨包满足失败补充准确诊断。

在权威规范更新前，不应直接修改 parser、semantic、codegen 或 `.ft` 格式。

## 11 待人工确认

以下内容未在本次讨论中形成结论，不能由实现阶段自行推断：

- 是否允许 spec 成员显式写 `open`，还是仅允许省略修饰符或写 `seal`；
- intersection-form 聚合 object-form spec 后如何计算 seal domain；
- 重载集合中公开与 seal 同名成员的冲突和查找规则；
- 已知具体 `type` 的静态 spec 视角如何显式表达；
- `.ft` 现有契约关系是否足以恢复实现成员 witness 映射；如不足，需要追加的
  具体编码与兼容处理；
- 反射或未来符号查询 API 对 contract hook 暴露到何种粒度。

这些决策完成并写入权威规范后，才能形成可执行的实现任务拆分。
