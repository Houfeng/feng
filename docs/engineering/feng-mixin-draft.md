# Feng `@mixin` / `@mixable` 设计草案

> **状态**：讨论草案，尚未进入语言规范，尚未实现。
>
> 本文用于方案评审与后续推敲。评审通过前，不修改
> `docs/specifications/` 中的正式规范，也不据此实现编译器或标准库。

## 1 背景

Feng 当前没有继承，也没有增加继承的计划。retained-mode GUI/TUI 组件通常需要
重复声明一组公共状态，例如样式、布局结果、父子关系和事件绑定，同时只在少量
行为上存在差异。

仅增加类似 Rust trait 的 `spec` 默认实现只能复用行为，不能复用实体字段。直接
转发具体类型的实例方法又会产生接收者语义问题：来源方法中的 `self` 是来源类型，
不能安全地动态重绑定为具有不同内存布局的目标类型。

本草案提出一组配对的内建注解：

- `@mixable` 标记来源类型中允许被编译器展开的成员；
- `@mixin(Source)` 或 `@mixin(Source(...))` 标记目标类型选择展开该来源；
- 编译器在前端早期把它们归一化为普通字段和普通 wrapper 方法；
- 归一化完成后，成员检查、初始化、重载、`spec` 满足、方法值、代码生成和运行时
  均复用现有机制。

该机制是受显式授权控制的语法糖，不引入继承、实例方法混入、动态 `self`、运行时
mixin 对象或新的方法分派机制。

## 2 设计目标

1. 复用一组公开类型已经定义的实例字段和静态字段。
2. 通过普通静态调用 wrapper 复用安全的公共行为。
3. 来源与目标双方均显式表达意图，避免公共类型的成员被意外展开。
4. 展开结果与用户手写普通字段和 wrapper 方法遵循相同语义。
5. 展开后尽早回到现有 AST、语义分析和代码生成主路径。
6. 支持 `.ft` + 静态库形式的跨包二进制分发，不要求 `.ft` 携带函数体或字段初始
   绑定表达式。
7. 实例字段在目标类型中具有独立存储，热路径成员访问不经过来源实例。
8. 机制不依赖 GUI/TUI，可用于任何需要显式成员生成的类型。

## 3 非目标

本草案不提供：

- 继承或子类关系；
- 来源类型到目标类型的实体子类型转换；
- 具体实例方法的方法体复制；
- 将来源实例方法的 `self` 重绑定为目标实例；
- Go 风格的实例方法自动提升；
- `spec` 默认实现；
- 运行时动态添加或删除成员；
- 来源字段与目标字段之间的长期绑定或同步；
- 来源静态字段与目标静态字段之间的别名关系；
- 任意用户自定义注解实现 `@mixin` / `@mixable` 的能力。

## 4 核心术语

- **来源类型**：`@mixin(...)` 选定的具体类型，例如 `View`。
- **目标类型**：声明 `@mixin(...)` 的类型，例如 `Button`。
- **可混入成员**：来源类型中显式标注 `@mixable` 的合法成员。
- **实例来源值**：每次构造目标实例时，为提取实例字段值而产生的一次来源类型值。
- **实例 wrapper**：目标类型中生成的普通实例方法，方法体调用来源静态方法，并把
  目标实例作为第一个实参。
- **静态 wrapper**：目标类型中生成的普通静态方法，方法体调用来源静态方法。
- **早期展开**：在来源类型可解析后、普通成员合法性检查和代码生成前，将注解转换
  为普通成员及普通初始化代码的前端归一化过程。

## 5 注解定位

`@mixin` 和 `@mixable` 使用 Feng 现有 annotation 语法，但属于编译器内建的语义
注解，不属于 `feng-custom-annotation-draft-2.md` 所述的 pre-Parse 自定义注解。

原因如下：

- `@mixin` 需要解析来源具体类型；
- `@mixin(Source(...))` 需要获得表达式的静态类型；
- 编译器需要读取来源成员、静态性、可见性、参数类型和 `@mixable` 标记；
- 跨包时需要从 `.ft` 恢复这些声明级事实；
- `@mixable("instance")` 需要检查静态方法第一个参数是否为 object-form `spec`。

因此，自定义注解的 Token 变换阶段不能实现本功能。`@mixin` / `@mixable` 必须像
`@value`、`@abi` 一样由核心编译器识别和处理。

## 6 建议语法

### 6.1 来源成员标记

```feng
open type View {
  @mixable
  open let style: WidgetStyle;

  @mixable
  open var parent: Option<Widget>;

  @mixable
  open static var defaultWidth: u32 = 10;

  @mixable
  open static func createStyle(): WidgetStyle {
    return WidgetStyle {};
  }

  @mixable("instance")
  open static func addChild(widget: Widget, child: Widget): void {
    widget.children.add(child);
  }
}
```

### 6.2 使用来源类型的零值

```feng
@mixin(View)
type Button: Widget {
  let text: string;

  func draw(manager: ViewManager): void {
    // ...
  }
}
```

`@mixin(View)` 的参数必须解析为具体类型。每次构造目标实例时，编译器取得一个
`View` 默认零值，不执行 `View` 构造函数，然后从该值读取所有可混入实例字段。

该规则与 Feng 当前“默认零值不执行构造函数”的类型语义保持一致。

### 6.3 使用显式构造表达式

```feng
@mixin(View(80, 24))
type Button: Widget {
  let text: string;

  func draw(manager: ViewManager): void {
    // ...
  }
}
```

`@mixin(View(80, 24))` 的参数是实例来源表达式。该表达式每构造一个目标实例
求值一次，其静态结果类型作为来源类型。编译器从表达式结果中读取所有可混入实例
字段。

由于 annotation 位于类型声明处，该表达式只使用类型声明位置可见的名称，不能引用
某个目标构造函数的参数或局部绑定，也不能直接引用目标实例的 `self`。

本草案当前示例使用对象构造表达式。是否允许任意能够产生具体来源类型值的表达式，
见 §16 的待评审项。

## 7 `@mixable` 合法位置与生成结果

| 来源声明 | 合法标注 | 目标中生成的成员 |
| --- | --- | --- |
| 实例 `let` / `var` | `@mixable` | 同名普通实例字段 |
| 静态 `let` / `var` | `@mixable` | 同名普通静态字段 |
| 静态方法 | `@mixable` | 同名普通静态 wrapper |
| 静态方法 | `@mixable("instance")` | 去掉首参数后的同名普通实例 wrapper |
| 实例方法 | 不合法 | 不生成 |
| 构造函数 | 不合法 | 不生成 |
| 终结器 | 不合法 | 不生成 |

`@mixable` 不接受其他参数。`@mixable("instance")` 中的参数必须是精确的字符串
字面量 `"instance"`，且只能标记静态方法。

实例方法不能标注 `@mixable`。本机制不复制实例方法体，也不把来源实例方法的
`self` 重绑定为目标实例。

## 8 实例字段展开

### 8.1 基本语义

假设来源声明为：

```feng
open type View {
  @mixable
  open let style: WidgetStyle;

  @mixable
  open var parent: Option<Widget>;
}
```

目标声明为：

```feng
@mixin(View(...))
type Button {
  let text: string;
}
```

编译器生成的普通成员在语义上等价于：

```feng
type Button {
  open let style: WidgetStyle;
  open var parent: Option<Widget>;
  let text: string;
}
```

每次构造 `Button` 时，初始化过程等价于：

```text
let mixinSource = View(...);
self.style = mixinSource.style;
self.parent = mixinSource.parent;
```

`View(...)` 只求值一次，不能为每个字段分别求值。字段绑定完成后，临时来源引用按
现有生命周期规则释放。

### 8.2 字段属性

生成字段复制来源字段的以下声明级事实：

- 名称；
- 类型；
- `let` / `var` 可变性；
- 可见性；
- 文档注释是否复制，见 §16 待评审项。

来源字段的初始绑定表达式不复制到目标字段。目标字段的值来自构造完成的实例来源
值或来源类型零值。

生成的 `let` 字段在 mixin 初始化代码执行时完成最终显式绑定。若目标构造函数或
对象字面量再次绑定同一 `let` 字段，后续现有绑定检查自然报错。

### 8.3 值语义

从来源值读取成员并绑定到目标字段时，完全采用普通成员读取、初始化和赋值规则：

- 值类型成员按现有值语义复制；
- 引用类型成员按现有引用语义绑定；
- callable 成员按现有 callable 值语义绑定；
- 不建立来源字段槽位与目标字段槽位之间的后续关联。

如果某个成员值内部保留了来源实例，例如 Lambda 捕获了来源实例的 `self`，该值
继续按普通引用语义保留来源实例。本机制不把其中的来源实例替换为目标实例。

### 8.4 构造阶段归一化

为保证实例来源表达式只执行一次，同时尽量复用现有成员与构造机制，建议早期展开：

1. 为每个可混入实例字段生成无初始绑定的普通目标字段；
2. 为目标类型的每个构造函数生成同一段构造前置代码；
3. 前置代码先求值一次实例来源表达式，再依次绑定生成字段；
4. 前置代码完成后执行用户编写的构造函数体；
5. 目标没有显式构造函数时，沿用现有默认公开无参构造，并为其加入前置代码。

这样，生成字段先取得默认零值但尚未完成显式绑定，再由构造前置代码完成绑定，符合
现有 `let` 字段可在构造函数阶段完成一次最终绑定的规则。

展开后的前置代码和字段都是普通 AST/语义对象；代码生成阶段不需要理解 `@mixin`。

## 9 静态字段展开

来源声明：

```feng
open type View {
  @mixable
  open static let defaultWidth: u32 = 10;

  @mixable
  open static var createdCount: u32 = 0;
}
```

目标声明：

```feng
@mixin(View)
type Button {}
```

概念展开结果：

```feng
type Button {
  open static let defaultWidth: u32 = View.defaultWidth;
  open static var createdCount: u32 = View.createdCount;
}
```

目标静态字段具有独立存储，不是来源静态字段的别名：

- 首次读取目标静态字段时，继续使用现有静态绑定延迟初始化机制；
- 其初始化表达式读取来源静态字段，来源静态字段按现有规则先确保初始化；
- 初始化完成后，来源与目标的静态 `var` 可以独立变化；
- 对 `@mixin(View(...))`，实例来源表达式不参与静态字段初始化，静态字段只由其来源
  类型 `View` 决定。

以上行为与用户手写对应静态绑定完全一致。

## 10 静态 wrapper

来源声明：

```feng
open type View {
  @mixable
  open static func createStyle(): WidgetStyle {
    return WidgetStyle {};
  }
}
```

目标概念展开结果：

```feng
type Button {
  open static func createStyle(): WidgetStyle {
    return View.createStyle();
  }
}
```

生成的静态 wrapper：

- 复制来源静态方法的名称、泛型参数、普通参数、返回类型与可见性；
- 方法体只进行一次对来源静态方法的完整限定调用；
- `void` 返回方法生成普通调用语句，非 `void` 返回方法返回调用结果；
- 不复制来源方法体；
- 不重写来源方法内部对来源类型静态成员的访问。

因此，如果来源静态方法内部修改 `View.createdCount`，目标 wrapper 调用后修改的仍然
是 `View.createdCount`，不是独立的 `Button.createdCount`。这与手写
`Button.method() { View.method(); }` 的语义相同。

## 11 实例 wrapper

### 11.1 来源约束

只有静态方法可以标注 `@mixable("instance")`。该静态方法必须至少包含一个参数，
且第一个参数的声明类型必须是 object-form `spec`。

例如：

```feng
open type View {
  @mixable("instance")
  open static func addChild(widget: Widget, child: Widget): void {
    widget.children.add(child);
  }
}
```

### 11.2 生成规则

目标：

```feng
@mixin(View(...))
type Button: Widget {
  // ...
}
```

概念展开结果：

```feng
type Button: Widget {
  open func addChild(child: Widget): void {
    View.addChild(self, child);
  }
}
```

生成 wrapper：

- 名称与来源静态方法相同；
- 删除来源静态方法的第一个参数；
- 其余参数、泛型参数和返回类型保持一致；
- 方法体以目标实例 `self` 作为来源静态方法的第一个实参；
- 其余实参按原顺序转发；
- 不复制来源静态方法体；
- 不引入来源类型实例；来源静态方法本身没有 `self`，目标 wrapper 的 `self` 只作为
  首个普通实参传入。

### 11.3 `spec` 满足

编译器不增加“目标必须在不依赖生成 wrapper 的情况下先满足首参数 spec”的特殊
约束。

`@mixin` 展开完成后，生成字段和生成方法与显式手写成员一起进入普通成员集合，随后
按现有规则检查目标声明的 `spec` 关系。若目标可以作为首参数传给来源静态方法，
wrapper 合法；否则 wrapper 中的普通调用表达式自然产生现有类型诊断。

生成 wrapper 可以像手写方法一样参与目标的 `spec` 满足，包括参与首参数 spec
自身的满足。编译器不得因为成员来源于 `@mixin` 而增加手写代码不存在的限制。

### 11.4 方法值与动态调用

生成 wrapper 是目标类型的普通实例方法：

- `target.method` 按现有规则形成绑定到目标实例的方法值；
- 目标通过该 wrapper 满足某个 `spec` 方法时，按现有 witness 生成规则处理；
- 通过 `spec` 调用时动态分派到目标 wrapper；
- wrapper 再静态调用来源方法。

调用链为：

```text
spec witness 调用
  → Target.wrapper(self, ...)
  → Source.staticMethod(self, ...)
```

不存在将来源具体类型方法的 `self` 解释为目标内存布局的过程。

### 11.5 对“`a` 调用 `b`”的影响

静态方法形式可以安全支持一部分传统模板方法场景，关键取决于 `a` 如何调用 `b`。

来源声明：

```feng
open spec WidgetBehavior {
  func b(): void;
}

open type View {
  @mixable("instance")
  open static func a(widget: WidgetBehavior): void {
    widget.b();
  }
}
```

目标声明：

```feng
@mixin(View)
type Button: WidgetBehavior {
  func b(): void {
    // Button 的实现
  }
}
```

调用链为：

```text
button.a()
  → View.a(button)
  → widget.b()
  → 通过 WidgetBehavior witness 调用 Button.b()
```

这种写法能够解决 Go 内嵌中常见的 `a` 固定调用内层 `b` 的问题，因为来源静态方法
显式通过首个 spec 参数调用 `b`，动态分派目标没有歧义。

如果来源静态方法写成对另一个静态方法的完整限定调用：

```text
View.b(widget)
```

则它就是普通静态调用，不会动态选择 `Button.b()`。两种行为从调用表达式本身即可
区分。

当前草案仍不能同时提供“自动生成默认 `b`”和“允许目标无冲突地替换 `b`”。如果
来源还把静态 `b` 标记为 `@mixable("instance")`，目标会先生成一个普通 `b`
wrapper；Button 再显式声明同签名 `b` 时，按现有规则形成重复方法并报错。这与用户
手写两个同签名方法一致。

因此，当前纯语法糖模型支持的是：

- mixin 提供 `a`，`a` 通过 spec 调用由目标实现的 `b`；
- 或 mixin 同时提供 `a` 和默认 `b` wrapper，但目标不能直接覆盖该 `b`。

若要同时支持默认 `b` 和目标覆盖，需要再引入排除、替换或优先级规则；这些规则不再
是普通手写展开的自然结果，不属于当前草案。

## 12 冲突、重载与错误处理

早期展开不实现特殊的优先级、覆盖或消歧规则。生成成员进入普通成员集合后，完全使用
现有规则：

- 生成字段与显式字段重名，按现有字段冲突规则报错；
- 多个 `@mixin` 生成同名字段，按现有字段冲突规则报错；
- 生成方法与显式方法签名重复，按现有重复签名规则报错；
- 生成方法形成合法重载时，按现有重载规则保留；
- 静态成员与实例成员继续属于不同冲突面；
- 字段与方法同名时继续使用现有冲突面规则；
- 来源成员不可见、调用参数不匹配或目标不满足 spec 时，使用现有可见性、调用和
  `spec` 诊断。

编译器不因为某个错误来自生成成员而放宽规则。诊断应同时指出目标上的 `@mixin`
位置和对应来源成员，帮助用户理解生成来源。

## 13 可见性

`@mixin` 访问来源类型和来源成员时遵循现有可见性规则：

- 跨包来源类型必须在目标位置可见；
- 跨包可混入成员必须对目标位置可见；
- `.ft` 中存在但标记为私有的声明不能因 `@mixable` 变成 consumer 可访问成员；
- 生成成员默认复制来源成员的可见性；
- wrapper 对来源静态方法的调用仍执行普通可见性检查。

若目标需要与来源不同的生成成员可见性，当前草案不提供修改语法，见 §16 待评审项。

## 14 跨包与 `.ft`

### 14.1 跨包成立条件

本方案不要求 `.ft` 携带来源字段初始绑定或来源方法体：

- `@mixin(View)` 使用来源类型的默认零值；
- `@mixin(View(...))` 通过外部包公开构造入口创建完整来源实例；
- 实例字段值通过普通公开成员访问取得；
- 静态字段初值通过普通外部静态字段访问取得；
- wrapper 只调用静态库中已有的来源静态方法符号。

因此，consumer 只需要声明级事实和链接符号，符合当前 `.ft` + 静态库的二进制分发
模型。

### 14.2 `.ft` 新增事实

来源包的 `.ft` 至少需要为可混入成员保留：

- `@mixable` 是否存在；
- mixable 模式：普通模式或 `instance` 模式；
- 所属类型；
- 成员种类与静态性；
- 名称、可见性和完整类型/函数签名；
- 静态方法的链接符号事实。

建议通过 `ATRS` 扩展属性节增加强制语义属性，而不是修改 `SYMS` 固定记录布局。
由于旧 consumer 忽略该属性后无法正确展开 `@mixin`，该属性不能作为可安全忽略的
可选语义发布；具体版本兼容策略在正式设计时确定。

目标包在导出前已经完成 `@mixin` 展开，因此目标 `.ft` 应导出生成后的普通字段和
普通方法声明。consumer 使用目标类型时不需要重新执行目标的 `@mixin`。是否同时
保留目标声明的 `@mixin` 来源元信息，仅影响 IDE 展示和后续传递性设计，不是目标
类型正确消费的必要条件。

### 14.3 包兼容性

为公开来源成员增加、删除或修改 `@mixable`，会改变重新编译后的目标类型成员集合和
对象布局，应视为公开源码/API 兼容性变化。已经编译完成的目标静态库不会在未重新
编译时自动改变。

## 15 编译器归一化阶段

建议的前端顺序为：

```text
Lex / Parse
  → 加载并恢复外部 .ft 声明
  → 解析 @mixin 来源类型或来源表达式静态类型
  → 校验 @mixable 标注形状
  → 生成普通字段、wrapper 与构造前置代码
  → 建立普通成员表
  → 执行现有语义分析
  → 执行现有代码生成
```

这里的“早期”不是 pre-Parse：显式构造表达式需要最小类型推导，外部来源需要先恢复
`.ft`。核心要求是展开必须发生在普通成员冲突、`spec` 满足、重载和方法值分析之前，
使后续阶段只看到普通成员。

来源静态方法的返回类型可以继续使用现有推导规则。生成 wrapper 若复制了省略的
返回类型，也由现有 wrapper 调用表达式自然推导，不需要为 mixin 新增返回类型系统。

## 16 待评审问题

以下问题尚未由本轮讨论确定，本文不替开发者决策：

1. 是否允许同一目标重复声明多个 `@mixin`；若允许，是否严格按 annotation 书写
   顺序生成构造前置代码。
2. `@mixin(Source(...))` 是否允许推广为任意具体类型表达式，而不限于对象构造
   表达式。
3. 是否允许泛型来源，例如 `@mixin(View<T>)` 或 `@mixin(View<T>(...))`，以及类型
   实参替换的精确规则。
4. 生成成员是否复制来源成员的文档注释和除 `@mixable` 之外的其他 annotation。
5. 生成成员是否保留 `@mixable`，从而允许传递性 mixin；还是默认移除该标记，禁止
   隐式传递。
6. 是否需要允许目标调整生成成员的可见性，或者始终复制来源可见性。
7. `@mixin(View(...))` 的表达式若只用于确定来源类型但来源没有可混入实例字段，
   是否仍需要每次构造目标时执行该表达式。
8. `@mixin` 与 `@value`、`@abi` 等现有内建语义注解组合时，需要增加哪些限制。
9. 生成代码的调试信息中，是否展示为目标普通成员，还是同时展示来源 mixin 信息。

## 17 预期实现范围

若方案评审通过，后续实现仍应遵循“先正式规范、再代码、后测试”的工程顺序。

### 17.1 正式规范

至少需要更新：

- `docs/specifications/feng-type.md`：`@mixin`、生成成员、初始化与冲突语义；
- `docs/specifications/feng-function.md`：`@mixable("instance")` wrapper 与方法值；
- `docs/specifications/feng-symbol-table.md`：跨包 mixable 属性；
- `docs/specifications/feng-package.md`：`.ft` 与静态库消费要求；
- annotation 相关正式规范或内建 annotation 清单。

每项规范只在对应主规范中定义，其他文档仅引用，避免重复。

### 17.2 编译器

预计涉及：

- lexer 内建 annotation 注册；
- parser annotation 合法目标支持；
- AST/声明模型中的 mixin 来源临时表示；
- 前端 mixin 归一化阶段；
- 构造函数前置代码生成；
- `.ft` writer、reader、imported-module cache 与 provider 查询；
- LSP 的生成成员补全、hover、definition 与诊断来源映射；
- codegen 对生成普通成员继续复用现有路径。

具体文件和函数在正式实现前通过代码调查确认，本文不预设未经验证的修改点。

### 17.3 测试

实现阶段至少需要覆盖：

- annotation 解析与非法标注目标；
- 零值来源与显式构造来源；
- 实例来源表达式只执行一次；
- 实例字段、静态字段、静态 wrapper 和实例 wrapper；
- `let` / `var`、可见性、字段冲突和方法重载；
- wrapper 参与 `spec` 满足和方法值；
- 泛型场景（若评审纳入）；
- 同包、跨模块和 `.fb` 跨包消费；
- `.ft` 写入、读取与旧格式兼容诊断；
- AST、IR/codegen 与运行行为；
- LSP 可见成员与来源定位；
- 最终执行 `make test` 全量回归。

未经人工批准，不修改既有测试用例；新增测试应覆盖新语义。

## 18 性能模型

实例字段展开把额外成本放在构造阶段：

- `@mixin(Source(...))` 每个目标实例求值一次来源表达式；
- 来源为普通引用类型时，通常会临时创建一个来源对象；
- 编译器按普通值语义读取并绑定可混入字段；
- 绑定完成后释放临时来源引用；若展开值保留了来源实例，该实例按普通引用语义继续
  存活；
- 静态字段继续使用现有延迟初始化，不产生每实例成本。

稳定运行阶段不存在来源对象转发：

- 生成字段直接进入目标对象布局；
- 字段访问与用户手写字段相同，不增加间接访问；
- 目标对象不为 mixin 额外保存隐藏来源引用；
- wrapper 只增加一次普通静态方法调用，后续优化不得改变其手写等价语义。

该模型适合“构造次数远少于布局、绘制和事件访问次数”的 GUI/TUI 组件，但正式实现
前仍需通过基准确认构造成本、临时分配、引用计数操作和热路径收益。任何扩大运行时
开销的实现选择均需单独评审，不能仅以语法便利为由默认接受。

## 19 当前结论

当前候选方案可以概括为：

```text
来源成员 @mixable
  +
目标类型 @mixin(Source) / @mixin(Source(...))
  ↓ 前端早期展开
普通实例字段 + 普通静态字段 + 普通静态调用 wrapper
  ↓
现有语义分析、spec、方法值、代码生成和运行时机制
```

其核心边界是：

- 字段生成独立目标存储，不转发；
- 静态字段生成独立目标静态存储，不建立别名；
- 方法只由静态方法生成 wrapper；
- 具体实例方法永不混入；
- 实例 wrapper 把目标 `self` 作为首个 spec 实参执行普通静态调用；来源静态方法
  通过该实参调用方法时，按现有 spec witness 规则动态分派；
- 所有合法性在展开后尽可能交给现有规则检查。

该方案是否进入正式语言规范，等待后续评审决定。
