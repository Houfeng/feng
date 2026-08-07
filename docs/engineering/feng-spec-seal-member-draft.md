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
requirement visibility    谁能看到并实现成员
invocation accessibility  谁能调用成员
```

`seal` spec 成员因此是一种“公开可实现、受限可调用”的契约成员，而不是 spec 内部的私有辅助函数。

## 2 目标

- 允许 object-form `spec` 声明 `seal` 成员 requirement。
- `seal` requirement 仍参与契约满足检查和 witness 构造。
- 普通调用侧不能通过具体类型或 `spec` 视角调用该成员。
- 满足该 `spec` 的类型进入同一个受限访问域，并可在该域内调用成员。
- `open spec` 明确允许包外类型进入该访问域；封闭的 `spec` 不允许包外加入。
- 跨包 `fit` 只能基于当前可见类型面建立关系，不能借此发现或暴露目标类型未导出的 `seal` 成员。
- `.ft` 只传播契约所需信息，不把实现类型的私有成员升级为普通公开成员。

## 3 非目标

- 不引入基于继承关系的 `protected`。
- 不引入 C++ 式逐项 `friend` 声明。
- 不允许 `fit` 获得目标 `type` 定义体的特殊私有访问权。
- 不允许运行时绕过编译期可见性检查。
- 本文不决定 `seal` 是否适用于 spec 字段、静态成员、callable-form、union-form 或 intersection-form；对话中已确认的用例仅覆盖 object-form 方法 requirement。

## 4 核心语义

### 4.1 两层成员面

object-form `spec` 同时具有两层成员面：

| 成员面 | 内容 | 用途 |
| --- | --- | --- |
| 公开调用面 | 默认公开的成员 | 普通 `spec` 使用者可调用 |
| 完整实现契约 | 公开成员与 `seal` requirement | 满足检查、witness 构造和受限域调用 |

以 `Widget` 为例：

```text
public callable surface(Widget) = { show }
implementation requirements(Widget) = { show, draw }
```

因此，“调用侧看不到 `draw`”不等于“实现侧不知道 `draw`”。

### 4.2 访问域

对 object-form `spec S`，定义：

```text
domain(S) = { T | T 在当前位置满足 S }
```

`S` 的 `seal` 成员只允许在 `domain(S)` 中的类型实现上下文调用。普通顶层函数、普通外部类型和仅持有 `S` 值的调用侧均不获得权限。

```feng
open spec Widget {
  seal func draw(): void;
}

open type Button: Widget {
  seal func draw(): void {
    // ...
  }

  func refresh(other: Widget): void {
    self.draw();   // 允许：Button 属于 domain(Widget)
    other.draw();  // 允许：调用点位于 Widget 的实现域
  }
}

func renderNow(widget: Widget): void {
  widget.draw();   // 错误：普通调用点不属于 domain(Widget)
}
```

访问权限由调用点的声明上下文决定，而不是由运行时接收者的具体类型决定。权限检查应在编译期完成，不增加运行时访问检查。

### 4.3 `open spec` 是显式授权

当 `seal` requirement 定义在 `open spec` 中时，spec 作者明确允许包外类型：

1. 看到该 requirement；
2. 实现该 requirement；
3. 通过满足关系加入该 spec 的受限访问域；
4. 在该访问域内调用该 requirement。

因此，第三方实现一个 `Widget` 后获得 `Widget` seal domain 的调用资格，是 `open spec Widget` 的直接语义结果，不属于可见性漏洞。

如果 spec 作者不允许包外代码加入该域，应封闭 spec 声明；如果具体类型本身也不应被包外适配或扩展，应同时收窄 type 的声明可见性。声明级 `seal spec` / `seal type` 与成员级 `seal func` 是不同维度：前者控制谁能看到并加入声明，后者控制谁能调用成员。

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

该规则只说明满足检查能够识别声明，不授予 `fit` 块方法访问 `MyType` 私有成员的能力。`fit` 方法体中的 `self` 仍受正常成员可见面约束；本文不修改现行 `fit` 私有访问限制。

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

失败原因不是“目标 requirement 为 `seal`”，而是当前编译单元没有可用于满足检查或成员查找的信息。

## 6 `.ft` 与符号信息传播

需要区分三类信息：

```text
declared members     类型完整定义中的成员
exported members     普通跨包成员查找可见的成员
contract witnesses   已导出契约关系所需的实现映射
```

### 6.1 纯 type 私有成员

未承担对外契约的 `type` seal 成员：

- 不进入普通跨包符号表；
- 不成为包外成员匹配候选；
- 不可被包外 `fit` 重新激活；
- 不可被工具误报为可调用 API。

### 6.2 实现已导出 seal requirement 的成员

当类型实现已导出的 spec seal requirement 时，跨包只保留完成该契约所需的 witness 身份：

```text
Button : Widget
  witness(Widget.draw) -> Button 的对应实现
```

不应把它恢复为普通 `Button.draw` 成员。于是：

```feng
button.draw(); // 普通成员查找仍然失败
```

而处于 `domain(Widget)` 的合法调用可沿契约 witness 分派：

```text
Widget.draw -> witness -> Button implementation
```

核心原则是：

> 导出的是 conformance witness，不是 private member 本身。

## 7 编译器实现影响

### 7.1 语法与 AST

- object-form spec 方法签名需要接受 `seal` 修饰符。
- 当前 `SE0601`“spec 成员不能声明可见性”的规则需要收窄，不能继续无条件拒绝 `seal func`。
- 默认无修饰成员继续进入公开调用面。
- 是否允许冗余的显式 `open`，本次对话未确认，正式规范必须单独决定。
- AST 和 `.ft` 恢复声明必须保留 spec requirement 的可见性。

### 7.2 语义分析

语义层需要分别产出：

- spec 的公开调用成员集合；
- spec 的完整 requirement 集合；
- 当前调用点是否处于目标 spec 的访问域；
- `(T, S)` 的满足关系和逐成员 witness；
- witness 来源是否只作为契约实现存在，还是也是普通公开成员。

满足检查必须遍历完整 requirement 集合；普通成员访问只查询公开调用面；seal-domain 调用在权限检查通过后查询相应契约成员和 witness。

### 7.3 导入导出

- `open spec` 的 `seal` requirement 签名必须写入 `.ft`，否则外部无法实现契约。
- 实现类型未参与公开契约的 seal 成员不得进入 `.ft`。
- 已导出的满足关系需要携带足够的 witness 映射，但不能将 witness 源恢复为普通公开成员。
- consumer 恢复声明后，必须保持“可实现、受限可调用”的语义，不能退化为公开方法或完全不可见方法。

### 7.4 代码生成与运行时

- `seal` requirement 继续使用现有或规划中的 spec witness 分派机制。
- 可见性在语义阶段完成判定；运行时不需要增加访问控制结构或分支。
- witness slot 的 ABI/布局是否因成员可见性变化需要版本化，应在 `.ft` 格式设计时确认。

### 7.5 工具链

LSP、文档生成器和符号查询应区分：

- Public API；
- Implemented contracts；
- Contract hooks；
- Private implementation。

工具可以向实现者展示 `Widget.draw` requirement，但不得把具体实现的 `draw` 作为普通可调用成员补全给无权限调用点。

## 8 诊断要求

至少需要覆盖以下诊断：

- 普通代码调用 spec seal 成员；
- 普通代码直接调用具体类型的对应 seal 实现；
- 非满足类型的实现上下文调用 seal requirement；
- 跨包 `fit` 尝试用未导出的 type seal 成员满足 spec；
- `fit` 方法体通过 `self` 访问当前可见面之外的 seal 成员；
- `.ft` 恢复后错误地把 contract witness 当作普通成员；
- 外部尝试满足不可见或封闭的 spec。

诊断信息应明确区分两类失败：

1. 成员存在，但当前调用点不属于访问域；
2. 当前可见类型面中不存在可用于满足检查的成员或 witness。

## 9 测试矩阵

| 场景 | 预期 |
| --- | --- |
| `open spec` 声明 `seal func` | 通过 |
| 外部 type 实现 open spec 的 seal requirement | 通过 |
| 普通函数通过 spec 值调用 seal requirement | 拒绝 |
| 满足 spec 的 type 方法内调用 seal requirement | 通过 |
| 满足 spec 的 type 方法内通过另一个 spec 值调用 seal requirement | 通过 |
| 外部 type 尝试实现不可见的 spec | 拒绝 |
| 包内 `fit T: S` 使用已知 type seal 成员完成满足检查 | 通过 |
| 跨包 `fit T: S` 尝试匹配未进入 `.ft` 的 type seal 成员 | 拒绝 |
| 跨包 `fit T` 方法中的 `self` 调用目标隐藏成员 | 拒绝 |
| 导出的 witness 在 consumer 中完成合法 seal-domain 分派 | 通过 |
| consumer 对 witness 源做普通成员调用 | 拒绝 |
| 未参与公开契约的 type seal 成员出现在 `.ft` 普通成员表 | 不得出现 |

测试应同时覆盖源码消费与 `.fb` / `.ft` 跨包消费，确保两条路径的成员面、满足关系和诊断一致。

## 10 与现行规范的变更关系

该能力与现行规范存在直接冲突，至少需要先修改：

- `docs/specifications/feng-spec.md`：放宽 object-form spec 方法的 `seal` 限制，定义两层成员面与访问域；
- `docs/specifications/feng-fit.md`：定义包内/跨包满足检查使用的可见类型面，并明确 `fit` 不扩大可见性；
- `docs/specifications/feng-visibility.md`：补充声明级 `open` / `seal` 与 spec 成员级 `seal` 的正交关系；
- `docs/specifications/feng-symbol-table.md`：定义 `.ft` 中 requirement、公开成员与 contract witness 的传播边界；
- `docs/specifications/feng-error-codes-se.md`：调整 `SE0601` 的适用范围，并为非法调用或跨包满足失败补充准确诊断。

在权威规范更新前，不应直接修改 parser、semantic、codegen 或 `.ft` 格式。

## 11 待人工确认

以下内容未在本次讨论中形成结论，不能由实现阶段自行推断：

- `seal` 是否扩展到 spec 字段、静态成员及其 getter/setter 语义；
- 是否允许 spec 成员显式写 `open`，还是仅允许省略修饰符或写 `seal`；
- 子 spec 与父 spec 之间如何继承、收窄或合并 seal domain；
- intersection-form 聚合 object-form spec 后如何计算 seal domain；
- 泛型 spec 实例之间的访问域身份是否按声明还是按实例化区分；
- 重载集合中公开与 seal 同名成员的冲突和查找规则；
- `.ft` 的具体编码、版本升级和旧包兼容策略；
- 反射或未来符号查询 API 对 contract hook 暴露到何种粒度。

这些决策完成并写入权威规范后，才能形成可执行的实现任务拆分。
