# Feng `@mixable seal` 方法开发草案

> **状态**：Draft，等待 Review，尚未实施。
>
> **主规范归属**：Review 通过后，`@mixable` 的正式语言语义统一更新到
> [`feng-function.md`](../specifications/feng-function.md)；
> [`feng-type.md`](../specifications/feng-type.md) 只补充 `seal` 普通可见性规则对
> `@mixable seal static` 授权的引用，不重复定义生成流程。
>
> **范围**：只增强 `@mixable` 对 `seal static` 方法的支持，使其与现有
> `@mixable open static` 使用同一套 mix、wrapper、冲突和调用流程，并增加直接
> mix 目标对来源 `@mixable seal static` 方法的受限访问权。

## 1. 背景

当前 `@mixable` 只让目标位置可见的公开静态方法参与成员展开。来源方法被选中后，
编译器为目标生成保留 `mixable` 事实的静态 wrapper，再由该静态 wrapper 派生删除
首参数的普通实例 wrapper。

这个机制适合公开复用行为，但无法表达以下需求：

- 行为只供直接 mix 该来源的目标 type 复用；
- 行为不应成为来源或目标的公开 API；
- 来源仍需保留其他普通 `seal` 成员的严格私有能力；
- 目标显式替换某个 mixable 行为时，仍能通过完整限定静态调用复用来源实现。

本草案增加 `@mixable seal static`。它提供类似 OOP `protected` 的使用效果，但授权
依据是显式成员展开关系，不是继承、共同实现某个 `spec` 或 type 间的整体友元关系。

## 2. 设计目标

1. `@mixable seal static` 与现有 `@mixable open static` 使用同一套行为生成流程。
2. 来源为 `seal` 时，目标生成的静态 wrapper 和实例 wrapper 都保持 `seal`。
3. 通过任一合法成员展开形式直接 mix `Source` 的目标 type，可以调用来源成员面中
   符合条件的 `@mixable seal static` 方法。
4. 普通 `seal` 字段、实例方法和未标注 `@mixable` 的静态方法继续保持现有私有边界。
5. `type` 自身声明的 `@mixable` 方法与可见 `fit Source` 声明的 `@mixable` 方法使用
   一致规则。
6. 跨包 mix 使用 `.ft` 中现有的 `seal`、`static`、`is_mixable` 与完整签名事实恢复
   同一授权和 wrapper 生成行为。
7. 不修改 `spec` witness、type/spec 满足关系或普通成员可见性模型。
8. 不引入新的运行时分派、方法表、来源实例或额外运行时开销。

## 3. 非目标

本草案不处理：

- 让实现同一个 `spec` 的多个 type 自动互相访问 `seal` 成员；
- type 之间的整体友元关系；
- 来源 `seal` 字段或普通 `seal` 实例方法的展开或访问；
- 未标注 `@mixable` 的来源 `seal static` 方法；
- `fit Target`、其他无直接 mix 关系的 type 或顶层函数对来源 `seal` 成员的普通访问；
- 修改成员默认可见性；成员默认公开的现有语义保持不变；
- 修改 package-public `.ft` 对普通 `seal` 方法的导出边界；
- 为 `.ft` 新增 `protected`、`mix-visible` 或其他可见性/授权字段；
- 新增运行时 `protected`、虚方法或继承机制。

## 4. 当前 `@mixable open` 行为

### 4.1 type 来源

```feng
type View: Widget {
  @mixable
  open static func draw(target: Widget, area: Area): void {
  }
}

type Button: Widget {
  ...: View;
}
```

`Button` 中概念生成：

```feng
type Button: Widget {
  @mixable
  open static func draw(target: Widget, area: Area): void {
    View.draw(target, area);
  }

  open func draw(area: Area): void {
    Button.draw(self, area);
  }
}
```

静态 wrapper 保留完整签名、可见性和 `mixable` 事实；实例 wrapper 删除第一个
object-form `spec` 参数，保留其余签名与可见性，但不保留 `mixable` 事实。

### 4.2 fit 来源

当前 `fit Source` 中的公开 `@mixable static` 方法同样受支持：

```feng
open fit View {
  @mixable
  open static func draw(target: Widget, area: Area): void {
  }
}
```

编译器先在该 `fit` 中为 `View` 派生普通实例 wrapper。其他 type 展开 `View` 时，
当前 mix 位置可见并可通过普通 `View.draw(...)` 查找的这个 fit 静态方法也进入来源
成员面，在目标中继续生成静态 wrapper 和实例 wrapper。

本草案不得为 `seal` 另建一套生成流程，也不得排除现有 open 路径已经支持的
`fit Source` 来源。

## 5. 核心语义

### 5.1 可见性映射

来源方法与目标生成成员按以下规则映射：

| 来源静态方法 | 目标静态 wrapper | 目标实例 wrapper |
|---|---|---|
| `@mixable open static` | `@mixable open static` | `open` 实例方法 |
| `@mixable seal static` | `@mixable seal static` | `seal` 实例方法 |

`seal` 只改变可见性与相应访问授权，不改变其他 mixable 行为。

### 5.2 seal 来源示例

```feng
type View: Widget {
  @mixable
  seal static func draw(target: Widget, area: Area): void {
  }
}

type Button: Widget {
  ...: View;
}
```

`Button` 中概念生成：

```feng
type Button: Widget {
  @mixable
  seal static func draw(target: Widget, area: Area): void {
    View.draw(target, area);
  }

  seal func draw(area: Area): void {
    Button.draw(self, area);
  }
}
```

目标静态 wrapper 对 `View.draw(...)` 的调用由本草案定义的直接 mix 授权允许。
目标实例 wrapper 调用目标自身的 `Button.draw(...)`，继续使用普通 type 自身
`seal` 访问规则。

### 5.3 直接 mix 授权

对一个已经通过现有来源解析、`@mixable` 契约和目标首参数 `spec` 名义关系检查的直接
成员展开声明：

```feng
type Target {
  ...: Source;
  // 或 ...: Source = Source();
  // 或 ... = Source();
}
```

三种成员展开形式建立相同的直接 mix 授权。来源构造表达式只决定现有字段初始化路径，
不改变 mixable 方法候选、wrapper 生成或 seal 访问授权。跨包来源仍须满足来源 type、
构造函数及构造表达式的现有普通可见性和合法性要求。

该声明授予 `Target` 对 `Source` 来源成员面中 `@mixable seal static` 方法的受限
访问权。授权满足以下条件：

1. 当前访问发生在 `Target` 自身的成员方法或静态方法中，或者发生在编译器为
   `Target` 生成的 mixable wrapper 中；
2. 被访问成员是 `Source` 来源成员面中符合现有 `@mixable` 契约的静态方法；
3. 被访问成员显式标注 `seal` 和 `@mixable`；
4. `Target` 通过任一合法成员展开形式直接 mix 对应 `Source`，不能只依赖间接来源
   关系；
5. 同包来源使用 AST 声明事实，跨包来源使用 package-public `.ft` 恢复的同一组
   `seal`、`static`、`is_mixable` 和签名事实；来源位置不改变授权规则。

该授权不适用于 `fit Target`、其他 type、顶层函数或普通外部调用点。

### 5.4 目标显式优先与来源调用

目标可以继续使用现有显式优先规则替换来源候选，并在自己的成员方法或静态方法中调用
来源 `@mixable seal static` 实现：

```feng
type Button: Widget {
  ...: View;

  @mixable
  seal static func draw(target: Widget, area: Area): void {
    View.draw(target, area);
    // Button 的附加逻辑
  }
}
```

授权取决于合法的直接 mix 关系和来源方法资格，不取决于来源静态 wrapper 是否
因目标显式优先规则而最终生成。否则目标在显式替换来源实现后将无法组合来源逻辑。

### 5.5 type 与 fit 来源一致

`@mixable seal static` 可以来自：

1. 来源具体 type 最终成员面；
2. 当前 mix 位置按现有 fit 声明可见性规则可见、目标为 `Source` 的 `fit Source`。

对于 `fit Source` 来源，fit 声明本身必须按现有规则在当前 mix 位置可见；seal 授权只
替代其成员的普通公开访问要求，不扩大 fit 声明本身的作用域，也不改变 fit 的冲突、
导出或恢复规则。

来源为 type 或 fit 不改变目标 wrapper 的生成结果。来源方法是 `seal` 时，目标静态
wrapper 和实例 wrapper 都是 `seal`。

### 5.6 多层传播

生成的 seal 静态 wrapper 继续保留 `mixable` 事实。因此无论每一层位于同包还是不同
包，只要来源 type 与构造路径按普通规则可见并且每一层都显式建立直接 mix 关系：

```text
A ...: B
B ...: C
```

- `B` 可以获得并调用 `C` 的 `@mixable seal static` 行为；
- `A` 可以获得并调用 `B` 已生成的 `@mixable seal static` wrapper；
- `A` 不因间接关系获得直接调用 `C` 原始 seal 方法的权限；
- 如果 `A` 需要直接调用 `C`，必须显式声明对应的直接 mix 关系。

因此行为可以按现有 wrapper 链传播，但原始来源访问权不会沿整个依赖图隐式扩散。

## 6. 与普通 seal、protected 和 spec 的关系

### 6.1 普通 seal 保持 private

未标注 `@mixable` 的 `seal` 成员继续只能在现有 type 自身可见域内使用。直接 mix 关系
不授予以下访问：

- `seal` 实例字段；
- 普通 `seal` 实例方法；
- 未标注 `@mixable` 的 `seal static` 方法；
- `seal` 构造函数或其他私有声明。

所以 `@mixable seal` 不会使来源 type 丢失普通 private 能力。

### 6.2 protected-like，而不是继承

`@mixable seal` 的效果类似 `protected`：来源行为可以被明确关联的目标复用，但不会成为
公开 API。二者本质不同：

- OOP `protected` 通常由继承关系授权；
- Feng 由来源方法上的 `@mixable` 与目标中的直接 `...: Source` 共同授权；
- 授权粒度是具体静态方法，不是整个 type；
- 不建立子类型关系、动态分派或 receiver 重绑定。

### 6.3 不由共同 spec 实现授权

本草案不允许“实现相同 `spec` 的多个 type 自动互相访问 seal 成员”。这种互友元方案会
扩大授权集合，使普通 seal 的 private 边界依赖未来新增的 spec 实现者，同时把契约、
witness 与成员访问耦合在一起。

当前 `@mixable` 仍要求第一个参数是 object-form `spec`，并要求来源类型和目标类型满足
现有名义关系。该 `spec` 只负责：

- 约束 mixable 行为适用的目标；
- 为实例 wrapper 注入 `self` 提供静态参数视角；
- 参与生成成员进入普通成员表后的既有满足检查。

`spec`、witness 或“共同实现同一 spec”的事实不授予本草案的 seal 访问权。访问权只
来自 `@mixable seal static` 与直接 mix 关系。

生成的 seal wrapper 进入普通成员表后，继续遵守现有 spec requirement/实现成员可见性
兼容规则；本草案不为其增加特殊 witness 规则。

## 7. 保持不变的现有行为

支持 seal 后，以下规则必须与 open 路径保持一致：

- 第一个参数必须是非变长 object-form `spec`；
- 来源类型和目标类型的名义 spec 关系检查；
- 方法泛型参数、约束、类型实参替换与 reified dependencies；
- 其余参数、返回类型和变长参数转发；
- 目标显式成员优先与实例投影检查；
- 不同来源之间的冲突和普通重载规则；
- 静态 wrapper 保留 `mixable`，实例 wrapper 不保留；
- 来源位置映射、诊断位置、LSP definition/hover 所需来源关系；
- wrapper 进入普通成员表后的方法值、spec 满足、导出和代码生成检查；
- wrapper 只进行编译期确定的普通静态调用。

默认成员可见性仍为 open，不作修改。

## 8. 跨包与 `.ft`

### 8.1 现有事实表达

`.ft` 已能记录方法的 visibility、static、`is_mixable`、完整签名、泛型与 reified
dependencies。本草案不增加 `protected`、`mix-visible` 或其他新事实，也不改变
`seal` 的普通访问含义。

package-public `.ft` 的方法选择规则扩展为：

```text
open 方法
或
seal && static && is_mixable 方法
```

第二类声明作为受限 mix 能力记录，恢复后仍然同时保持：

```text
visibility = seal
is_static = true
is_mixable = true
```

普通 `seal` 方法、`seal` 实例方法以及 `seal static` 但未标注 `@mixable` 的方法仍不
进入 package-public `.ft`。

### 8.2 访问解释

`.ft` 只恢复声明事实，不直接授予访问权：

```text
普通成员访问
  → 按 seal 规则过滤并拒绝

直接 mix 目标中的访问
  → seal && static && is_mixable && 存在对应直接 mix 关系
  → 放行
```

因此，记录 `@mixable seal static` 不会使其成为 open 成员。同包 AST 和跨包 `.ft`
必须进入同一个候选与授权查询，不能由 provider 来源决定不同语言行为。

### 8.3 多层导出

目标生成的静态 wrapper 仍是 `seal + static + is_mixable`，所以必须按相同规则记录到
目标 package-public `.ft`，使下游包再次显式 mix 该目标时可以继续传播行为。

目标生成的实例 wrapper 是 `seal + !is_mixable`，不作为 mix 能力记录到
package-public `.ft`。下一层传播只依赖目标静态 wrapper，不依赖上一层实例 wrapper。

可见 `fit Source` 中的 `@mixable seal static` 方法使用相同规则；fit 声明本身仍须满足
现有导出、可见性和孤儿适配规则。

### 8.4 跨包链接

`@mixable seal static` 在 Feng 语言层保持 seal，但 provider 必须为它提供跨包可链接的
稳定调用符号，否则 consumer 生成的静态 wrapper 无法完整限定调用来源方法。生成的
seal 静态 wrapper 也适用同一规则。

链接可用性不改变 Feng 成员可见性。符号身份必须只依赖 package-public `.ft` 可恢复的
稳定事实；泛型 type 方法和 fit 方法不得依赖 consumer 无法看到的其他普通 seal 方法
计算符号序号。

本草案不新增 seal 方法体、运行时方法表或专用 wrapper ABI。

## 9. 编译器设计建议

### 9.1 统一候选模型

来源候选不应继续等价于“公开 `@mixable` 方法”，而应统一定义为：

```text
满足现有 @mixable 契约
并且
(
  方法 open 且在目标位置普通可见
  或
  方法 seal 且当前目标具有直接 mix 授权
)
```

type 成员和 fit 成员都应进入同一个候选判断，不应分别复制 seal 特判。

### 9.2 统一授权查询

语义层应提供可复用的 mixable seal 授权查询，输入至少包含：

- 当前实现 type；
- 直接成员展开声明；
- 来源具体 type；
- 来源方法及其 type/fit 声明归属；
- provider 与 package-public 声明事实。

该查询同时服务于：

- 来源静态 wrapper 候选选择；
- 编译器生成 wrapper 方法体的来源静态调用；
- 目标显式成员中的 `Source.method(...)` 解析；
- 普通静态重载过滤和诊断；
- LSP completion、definition 和 hover 的可访问成员视图。

不得只在某一个生成点跳过可见性检查，否则显式来源调用、重载或 LSP 可能与生成结果
不一致。

### 9.3 wrapper 生成

现有 wrapper 已按来源成员复制可见性。实现应复用现有创建流程：

- 静态 wrapper 的 visibility 取来源静态方法 visibility；
- 实例 wrapper 的 visibility 取当前保留静态方法 visibility；
- seal 与 open 使用相同签名克隆、转发、冲突和来源映射逻辑；
- 不复制来源方法体，不引入 seal 专用代码生成路径。

### 9.4 访问检查

普通 type seal 访问规则保持默认入口。只有待访问成员同时是符合条件的
`@mixable seal static`，并且当前 type 与该成员之间存在合法直接 mix 授权时，才增加
访问成功分支。

该分支不得授权 `fit Target`，也不得把“当前包相同”“实现相同 spec”或“来源 type
可见”单独视为授权条件。

同包 AST 与跨包 `.ft` 恢复成员必须执行同一判断；`seal + is_mixable` 本身也不构成
授权，必须同时存在当前目标到对应来源的直接 mix 关系。

## 10. 测试计划

### 10.1 Parser / AST

- `@mixable seal static func` 解析成功并同时保留 visibility 与 mixable 声明事实；
- `@mixable open static func` 现有解析结果不变；
- 现有非法 `@mixable` 位置和参数诊断不变。

### 10.2 Semantic

- type 来源的 seal mixable 方法生成 seal 静态 wrapper；
- seal 静态 wrapper 派生 seal 实例 wrapper；
- 静态 wrapper 保留 mixable，实例 wrapper 不保留；
- 目标成员方法可以调用来源 seal mixable 静态方法；
- 目标静态方法可以调用来源 seal mixable 静态方法；
- 编译器生成 wrapper 可以调用来源 seal mixable 静态方法；
- 目标显式优先跳过来源 wrapper 后，仍可显式调用来源实现；
- 可见 `fit Source` 中的 seal mixable 方法按 type 来源同样参与生成；
- 普通来源 seal 字段、实例方法和非 mixable 静态方法仍不可访问；
- 同包其他无直接 mix 关系的 type 不可访问；
- `fit Target` 不获得访问权；
- 间接 mix 目标不能直接调用原始来源 seal 方法；
- 多层 seal wrapper 可以沿每一层直接 mix 关系继续传播；
- open mixable 的现有生成、冲突和调用行为不变；
- seal 生成成员继续执行普通 spec requirement/实现成员可见性兼容规则。

### 10.3 Symbol / `.ft`

- package-public `.ft` 记录来源 `seal + static + is_mixable` 方法，并原样恢复三个事实；
- package-public `.ft` 记录目标生成的 `seal + static + is_mixable` wrapper；
- package-public `.ft` 不记录普通 seal 方法和目标生成的 `seal + !is_mixable` 实例
  wrapper；
- open mixable 及生成 open wrapper 继续导出 mixable、签名和可见性事实；
- 普通 imported 成员访问仍按 seal 拒绝；
- 跨包直接 mix 目标可以选择并调用恢复的 seal mixable 静态方法；
- workspace cache 中无关私有事实不能扩大直接 mix 授权集合。

### 10.4 Codegen / FCTS

- seal 静态 wrapper 正确完整限定调用来源静态方法；
- seal 实例 wrapper 正确把 `self` 作为第一个 spec 参数调用目标静态 wrapper；
- type 来源和 fit 来源都覆盖实际运行结果；
- 跨包来源和跨包多层目标 wrapper 能正确链接并运行；
- 泛型、变长参数和多层 seal mix 链正确转发；
- 不增加运行时动态分派、额外分配或参数重新打包。

### 10.5 回归

- 目标专项测试全部通过；
- 完成非文档变更后执行 `make test` 全量回归。

## 11. TODO

### TODO 1：Review 并收敛正式规范

- [ ] Review 本草案的直接 mix 授权、type/fit 一致性和跨包边界。
- [ ] 更新 `docs/specifications/feng-function.md`，作为 `@mixable seal` 生成与授权的唯一
  主规范。
- [ ] 更新 `docs/specifications/feng-type.md`，在普通 seal 私有规则中引用
  `@mixable seal static` 的明确例外，不重复生成语义。
- [ ] 更新 `docs/specifications/feng-symbol-table.md`，定义 package-public `.ft` 对
  `seal + static + is_mixable` 方法的选择与原样事实恢复。
- [ ] 更新 `docs/specifications/feng-language.md` 的能力总览引用。
- [ ] 检查现有 mixin 工程文档，只保留历史设计或对主规范的引用，避免重复规范。

### TODO 2：统一语义候选与授权查询

- [ ] 将 mixable 来源候选从“必须公开”扩展为“普通 open 可见或具有 seal mix 授权”。
- [ ] 为 type 来源和可见 fit 来源提供统一的候选及方法归属信息。
- [ ] 实现基于当前 type、直接 mix 声明、来源 type、来源成员和 provider 的统一授权查询。
- [ ] 让 package-public writer 选择 `seal + static + is_mixable` 方法，并让 reader/import
  继续按 seal/static/is_mixable 原样恢复，不引入新的 `.ft` 授权字段。
- [ ] 让来源及生成的 seal mixable 静态 wrapper 使用同一选择规则；seal 实例 wrapper
  继续不进入 package-public 方法面。
- [ ] 确保目标显式优先跳过来源 wrapper 后，来源方法授权仍然存在。
- [ ] 确保同包、spec 实现关系或 fit 目标身份本身不会扩大授权。

### TODO 3：复用现有 wrapper 流程

- [ ] 验证来源 seal visibility 原样传播到目标静态 wrapper。
- [ ] 验证静态 seal visibility 原样传播到目标实例 wrapper。
- [ ] 保持静态 wrapper 的 mixable 事实和实例 wrapper 的非 mixable 事实。
- [ ] 复用现有泛型、变长参数、来源映射、冲突和代码生成逻辑。
- [ ] 让来源及生成的 `seal + static + is_mixable` 方法具有跨包稳定、可链接的调用
  符号，同时保持 Feng visibility 为 seal。
- [ ] 确保泛型 type/fit 方法的符号身份不依赖未记录到 package-public `.ft` 的普通
  seal 方法。
- [ ] 禁止新增 seal 专用运行时或 ABI 路径。

### TODO 4：接入所有访问与工具链入口

- [ ] 让生成 wrapper 的来源静态调用使用统一授权查询。
- [ ] 让目标显式成员中的来源静态调用使用统一授权查询。
- [ ] 让静态重载选择先过滤无授权 seal 候选。
- [ ] 保持现有不可访问诊断，不让无授权 seal 候选遮蔽可访问 open 候选。
- [ ] 检查 LSP completion、definition、hover 对直接授权成员的结果与编译器一致。

### TODO 5：增加完整测试

- [ ] 增加 type 来源 seal mixable 的 AST、semantic 和 codegen 测试。
- [ ] 增加 fit 来源 seal mixable 的 AST、semantic 和 codegen 测试。
- [ ] 增加目标成员方法、静态方法和生成 wrapper 的授权正向测试。
- [ ] 增加普通 seal、无直接 mix、fit Target 和间接来源访问的反向测试。
- [ ] 增加 open 行为不变、显式优先、重载、泛型、变长参数和多层传播回归测试。
- [ ] 增加 package-public `.ft` 只记录 `seal + static + is_mixable`、不记录其他 seal
  方法和 seal 实例 wrapper 的回归测试。
- [ ] 增加跨包 type/fit 来源、三种成员展开形式、显式来源调用和多层传播测试。
- [ ] 增加跨包普通访问仍按 seal 拒绝的回归测试。
- [ ] 增加 FCTS 语言行为覆盖。
- [ ] 执行 `make test` 全量回归。

## 12. 验收标准

本草案完成实施时必须同时满足：

1. open 与 seal 使用同一套 `@mixable` 生成、传播和冲突流程；
2. seal 来源在目标中只生成 seal 静态 wrapper 和 seal 实例 wrapper；
3. 直接 mix 目标可以调用对应来源 seal mixable 静态方法；
4. type 来源与当前可见 fit 来源行为一致；
5. 普通 seal 成员继续保持现有 private 能力；
6. spec/witness 不参与 seal mix 授权；
7. 跨包直接 mix 与同包直接 mix 使用相同授权和 wrapper 生成规则；
8. `.ft` 只使用现有 seal/static/is_mixable/签名事实表达受限 mix 能力；
9. 跨包普通访问仍然按 seal 拒绝；
10. 来源及生成的 seal mixable 静态方法具有稳定跨包链接入口；
11. 不增加运行时开销或专用 ABI；
12. 专项测试与 `make test` 全量回归全部通过。
