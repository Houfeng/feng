# `spec` 实现成员可见性问题备忘

> **状态**：待决策，暂不实施。
> **日期**：2026-08-14。
> **性质**：engineering 问题备忘，不是语言权威规范。

## 1. 目的与范围

本文独立记录 object-form `spec` requirement 与具体实现成员可见性之间的语义缺口：当前公开的 `spec` 成员可以由同名、同结构的 `seal` 类型成员自动满足，并通过 `spec` 视角公开访问。

本文只记录现状、问题边界和候选方向，不选择最终方案，也不修改当前编译器行为。未来若作出决策，应先更新以下权威规范，再实施代码与测试：

- [`feng-spec.md`](../specifications/feng-spec.md)
- [`feng-visibility.md`](../specifications/feng-visibility.md)
- 必要时更新 [`feng-fit.md`](../specifications/feng-fit.md)

未来 `spec seal` 成员的设计仍以 [`feng-spec-seal-member-draft.md`](./feng-spec-seal-member-draft.md) 为准。本文不重复定义该能力，只记录两项设计之间需要统一的边界。

## 2. 当前行为

当前 `spec` 成员不能显式声明 `open` 或 `seal`，并按公开 requirement 处理：

```feng
open spec TextWidget: Widget {
  let lines: List<TextLine>;
}

open type Text: TextWidget {
  seal let lines: List<TextLine> = List<TextLine>();
}
```

上述代码目前能够通过满足检查。其可见性表现为：

```feng
let text = Text();
text.lines;                   // 非 Text 自身代码：不可访问

let widget: TextWidget = text;
widget.lines;                 // 可通过 TextWidget 视角访问
```

即同一个实现成员同时具有两种访问面：

- 通过具体 `Text` 视角访问时，遵守类型成员的 `seal` 限制；
- 通过 `TextWidget` witness 访问时，按公开 `spec` requirement 暴露。

这不是显式声明的“仅通过契约访问”实现，而是当前满足检查仅比较成员结构所产生的自动行为。

## 3. 当前实现依据

当前实现存在以下事实：

1. parser 拒绝在 `spec` 成员上声明 `open` 或 `seal`，诊断为 `SE0601`。
2. object-form `spec` 的字段满足检查比较成员名称、静态性、可变性和类型。
3. object-form `spec` 的方法满足检查比较成员名称、静态性和 callable 签名。
4. 字段和方法的满足检查均未要求实现成员为 `open`，因此 `seal` 成员也可被选为 witness 实现。
5. 成员一旦进入公开 `spec` 的 witness，调用侧便可通过该 `spec` 视角访问。

因此，当前行为是编译器现有检查规则的直接结果，不是 `TextWidget` 或 TUI 的局部行为；实例字段、静态字段、实例方法和静态方法都需要在最终方案中统一考虑。

## 4. 语义问题

当前行为存在需要明确决策的歧义：

- `seal` 通常表达“只有定义该成员的类型自身能够访问”，但公开 `spec` 视角会间接公开该成员。
- 类型作者只写了同名成员，没有显式声明该成员允许作为公开契约实现。
- 该行为类似“自动显式接口实现”，但 Feng 当前没有对应的显式语法或规范术语。
- `fit`、跨包 `.ft` 导出、API 文档、补全、查找引用和重命名都需要与最终可见性语义一致。
- 未来引入 `spec seal` requirement 后，必须明确“公开 requirement”和“受限 requirement”分别允许由哪些可见性的类型成员满足。

## 5. 其他语言的相关选择

其他语言没有统一做法，但大致分为两类：

| 语言 | 相关规则 |
| --- | --- |
| C# | 隐式接口实现必须是 `public`；显式接口实现不声明访问修饰符，并且只能通过接口视角访问。 |
| Java | 接口方法实现必须保持 `public`，不能用更窄的访问级别。 |
| Swift | protocol witness 的可见性必须满足 conformance 的可见性要求。 |
| Rust | trait 实现项不能单独声明可见性，其访问面由 trait 定义。 |
| C++ | 私有虚方法可以覆写公开基类虚方法，并通过公开基类视角动态调用。 |

相关官方资料：

- [C# interfaces](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/interfaces)
- [C# explicit interface implementation](https://learn.microsoft.com/en-us/dotnet/csharp/programming-guide/interfaces/explicit-interface-implementation)
- [Java interfaces](https://docs.oracle.com/javase/tutorial/java/concepts/interface.html)
- [Swift access control](https://docs.swift.org/swift-book/documentation/the-swift-programming-language/accesscontrol/)
- [Rust E0449](https://doc.rust-lang.org/error_codes/E0449.html)
- [C++ virtual functions and access control](https://eel.is/c++draft/class.access.virt)

## 6. 与 `spec seal` 成员的关系

[`feng-spec-seal-member-draft.md`](./feng-spec-seal-member-draft.md) 计划引入具有受限访问域的 `spec seal` requirement。当前问题早于该能力，并且不能由该草案自动解决：

- 当前问题是：公开 `spec` requirement 是否允许由类型的 `seal` 成员自动满足。
- `spec seal` 草案的问题是：受限 requirement 如何由实现类型满足，以及实现者如何通过 `spec` 视角访问。

两者最终必须形成一致规则，但本文不改变 `spec seal` 草案已经记录的设计方向。

## 7. 候选方向

以下方向仅用于后续决策，不代表当前结论。

### 7.1 可见性不得收窄

- 公开 `spec` requirement 必须由 `open` 类型成员满足。
- `seal` 类型成员不能自动满足公开 requirement。
- 未来的 `spec seal` requirement 可按其受限访问域决定是否允许 `seal` 或 `open` 实现。

该方向接近 C# 的隐式接口实现、Java、Swift 和 Rust 的公开契约规则。

### 7.2 保留当前自动契约视角

- `seal` 类型成员仍可自动满足公开 `spec` requirement。
- 具体类型视角保持 `seal`，`spec` 视角按 requirement 的访问面公开。
- 需要在规范中明确这是有意支持的双访问面语义，而不是检查遗漏。

该方向更接近 C++ 私有覆写公开虚方法的结果，但 Feng 的结构满足与 witness 机制并不等同于 C++ 继承。

### 7.3 增加显式契约实现形式

- 默认情况下，公开 requirement 要求 `open` 实现。
- 语言另行提供显式写法，允许类型作者主动声明某个 `seal` 成员只通过指定 `spec` 视角公开。
- 现有同名 `seal` 成员不再自动产生该映射。

该方向接近 C# 的显式接口实现，但是否增加新语法、如何与 `fit` 配合均需另行设计。

## 8. 后续决策点

实施前至少需要明确：

1. 公开 `spec` requirement 能否由 `seal` 类型成员满足。
2. 字段、方法、实例成员和静态成员是否使用完全一致的规则。
3. 类型直接声明满足关系与 `fit` 提供实现时是否使用一致规则。
4. 若保留双访问面，是否必须通过显式语法授权。
5. 跨包 `.ft` 应导出哪些实现映射，以及是否会间接暴露类型的 `seal` 成员。
6. LSP 的补全、定义、引用和重命名应如何表示同一成员的两个访问面。
7. 最终规则与 `spec seal` requirement 的满足及访问域如何衔接。

## 9. 待办

- [ ] 确认最终语义方向。
- [ ] 更新唯一权威的语言规范，关联工程文档仅保留引用。
- [ ] 更新 semantic 满足检查及诊断。
- [ ] 如有需要，更新 witness 与 `.ft` 导出规则。
- [ ] 补齐字段、方法、实例、静态、`fit` 和跨包测试。
- [ ] 补齐 LSP 补全、定义、引用和重命名测试。
- [ ] 根据最终结论调整 TUI `TextWidget` 的内部缓存表达方式。

在以上决策完成前，保留当前编译器行为，不在 TUI `Text` 实现中局部绕过或定义新的语言规则。
