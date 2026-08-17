# `spec` 实现成员可见性问题备忘

> **状态**：已决策，随 `spec seal` 成员能力实施。
> **记录日期**：2026-08-14。
> **决策日期**：2026-08-17。
> **性质**：engineering 问题备忘，不是语言权威规范。

## 1. 目的与范围

本文记录 object-form `spec` requirement 与具体实现成员可见性之间的
现有语义缺口及其决策：当前公开的 `spec` 成员可以由同名、同结构的
`seal` 类型成员自动满足，并通过 `spec` 视角公开访问。

该问题已纳入 [`feng-spec-seal-member-draft.md`](./feng-spec-seal-member-draft.md)
统一处理。具体的满足兼容矩阵、访问域和实现要求只在该草案中定义；本文
保留问题背景和决策记录，不重复定义规则。正式实施前，应先更新以下
权威规范，再实施代码与测试：

- [`feng-spec.md`](../specifications/feng-spec.md)
- [`feng-visibility.md`](../specifications/feng-visibility.md)
- 必要时更新 [`feng-fit.md`](../specifications/feng-fit.md)

`spec seal` 成员及本问题的工程设计均以
[`feng-spec-seal-member-draft.md`](./feng-spec-seal-member-draft.md) 为准。

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

## 4. 已确认的问题边界

本次决策确认：

- 公开 `spec` requirement 由 `type seal` 成员满足是应修复的现有问题；
- 该修复与 `spec seal` requirement 的新增满足规则一起实施，不另行引入
  显式契约实现语法；
- 字段、方法、实例成员、静态成员、直接声明和 `fit` 提供实现必须使用
  草案定义的同一规则；
- 该修复只改变 requirement 满足与 witness 选择，不改变具体 `type`
  成员的固有可见性。

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

[`feng-spec-seal-member-draft.md`](./feng-spec-seal-member-draft.md) 计划引入
具有受限访问域的 `spec seal` requirement。当前问题早于该能力，但两者
共享同一项“requirement 可见性与实现成员可见性是否兼容”的判断，因此
合并实施：

- 新增 `spec seal` requirement 的解析、满足和访问语义；
- 同时修复公开 requirement 选择 `type seal` 成员作为 witness 的问题；
- 不改变承担 requirement 的具体 `type` 成员本身的可见性。

具体规则统一引用草案的“契约满足与 witness”及“`seal` 成员访问域”章节。

## 7. 已确认方向

采用“requirement 可见性不得由实现成员收窄”的方向，并由 `spec seal`
为受限契约提供明确表达。完整兼容矩阵见
[`feng-spec-seal-member-draft.md` 第 3.2 节](./feng-spec-seal-member-draft.md#32-契约满足与-witness)。

该方向意味着：

- 当前依赖“公开 requirement 自动映射 `type seal` 成员”的代码需要迁移为
  明确的 `spec seal` requirement，或提供公开实现成员；
- witness 只能从与 requirement 可见性兼容的候选中选择；
- 通过 spec 视角访问实现成员时，权限由 requirement 决定；通过具体 type
  视角访问时，仍由具体成员的既有可见性决定。

## 8. 实施约束

实施时必须与主草案保持一致，尤其包括：

- 满足验证、快速满足查询和 witness 选择复用同一个可见性兼容判断；
- `type` 直接声明与 `fit` 提供实现不分叉；
- 不修改 `.ft` relation 模型，不扩大具体 `type seal` 成员导出；
- LSP 的补全、定义、引用和重命名遵守相同的 spec 访问面；
- TUI 当前依赖该现有行为的 `TextWidget.lines` 应按最终权威规范迁移。

## 9. 待办

- [x] 确认最终语义方向。
- [ ] 更新唯一权威的语言规范，关联工程文档仅保留引用。
- [ ] 更新 semantic 满足检查及诊断。
- [ ] 如有需要，更新 witness 与 `.ft` 导出规则。
- [ ] 补齐字段、方法、实例、静态、`fit` 和跨包测试。
- [ ] 补齐 LSP 补全、定义、引用和重命名测试。
- [ ] 根据最终结论调整 TUI `TextWidget` 的内部缓存表达方式。

在代码实施完成前，编译器仍保持当前行为。TUI 不局部定义新的语言规则，
待权威规范更新后与编译器实现一并迁移。
