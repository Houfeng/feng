# Feng `@mixable seal` 实例字段开发草案

> **状态**：已实施，专项测试和聚焦 FCTS 已通过；已执行全量回归，但被现有 TUI seal
> API 与测试调用不一致的问题阻断于 `std-tests`，未进入完整 FCTS。
>
> **主规范归属**：字段展开的正式语义统一定义在
> [`feng-type.md`](../specifications/feng-type.md)；
> [`feng-function.md`](../specifications/feng-function.md) 只把 `@mixable` 的合法标注位置
> 扩展到本草案定义的 `seal` 实例字段，并引用类型规范；
> [`feng-visibility.md`](../specifications/feng-visibility.md) 只记录对应的受限能力事实与
> 普通 `seal` 访问边界，不重复字段生成规则。
>
> **范围**：只让具体 `type` 中显式标注 `@mixable` 的 `seal` 实例字段参与既有 `...`
> 成员展开，并在目标中生成同名、同类型、同可变性且仍为 `seal` 的普通实例字段；同时
> 与现有 `@mixable seal static` 一致，授予直接 mix 目标对来源对应字段的受限访问权。
> `is_mixable` 仍不把字段变成 `open`，只有它与合法直接 mix 关系、目标 type 自身的方法
> 上下文同时成立时才形成访问能力。本草案不改变现有 `@mixable static` 方法、普通
> `seal` 成员、spec witness、fit 访问规则或运行时 ABI。

## 1. 背景

当前成员展开对字段只选择来源最终成员面中的公开实例字段：

```feng
open type View {
  open var frame: Rect;
}

open type Button {
  ...: View;
}
```

`Button` 会得到自己的 `open var frame: Rect`。该字段直接进入 `Button` 的对象布局，
不是对 `View` 实例的别名或转发。

这使需要跨多个组合层复用的 GUI/TUI 状态必须声明为 `open`。即使字段只应由来源 type
及显式展开来源的目标 type 使用，也会成为普通公开 API。当前已经支持的
`@mixable seal static` 解决了受限行为复用，但尚没有与之对应的受限状态复用能力。

本草案增加 `@mixable seal` 实例字段：来源显式声明该字段可以被成员展开复制；目标
得到自己的 `seal` 字段，并可以在自身实例方法或静态方法中访问来源对应字段。该能力
由字段上的 `@mixable` 与目标对来源的直接 `...` 关系共同授权，不是继承、共同实现某个
`spec`、整个 type 间的友元关系，也不把字段公开给普通调用点。

## 2. 设计目标

1. 让来源 type 可以显式选择允许成员展开的 `seal` 实例字段。
2. 目标生成字段保持 `seal`，并让直接 mix 目标在自身实例方法或静态方法中可以读写
   Source 对应实例字段；字段仍不成为普通公开 API。
3. `...: Source`、`...: Source = SourceConstruction` 和
   `... = SourceConstruction` 三种现有形式使用相同的字段候选与直接授权规则。
4. 保持现有字段展开的名称、类型、`let` / `var`、初始化、绑定、冲突、泛型和生命周期
   语义。
5. 生成字段继续携带 mixable 能力事实，使多层显式展开可以传播受限状态。
6. 同包 AST 与跨包 `.ft` 恢复声明使用同一候选和直接授权规则；`.ft` 正常记录
   `is_mixable`，但该事实单独存在时不构成访问权。
7. 生成字段的稳定运行阶段访问成本与手写目标字段一致，不增加来源对象、间接访问、
   动态分派或运行时检查。

## 3. 非目标

本草案不处理：

- 静态字段展开；
- 来源实例方法或未标注 `@mixable` 的 `seal` 成员展开；
- `spec` 成员、`fit` 成员字段或顶层绑定上的 `@mixable`；
- 让未直接 mix Source 的 type、顶层函数、fit 方法、构造器或字段初始化器访问 Source
  的 `@mixable seal` 字段；
- 让直接 mix 关系授权 Source 中其他普通 `seal` 字段、方法、构造器或整个 seal 成员面；
- 让共同实现同一个 `spec` 的 type 自动互相访问字段；
- 复制或传播来源字段的 `@friend` 集合及其他普通注解；
- 修改默认成员可见性；省略可见性仍按现状等价于 `open`；
- 修改 `@mixable static` 方法的首参数、wrapper、授权或发码规则；
- 新增继承、`protected` 可见性、隐藏来源实例或运行时字段转发；
- 修改 runtime ABI 或增加专用字段访问 helper。

静态字段继续不参与成员展开。复制静态字段会产生来源静态状态与目标静态状态两份独立
存储，而来源方法中的完整限定访问仍指向来源状态；该问题与实例字段复用不同，需要时
应另行设计。

## 4. 语法与声明约束

### 4.1 合法声明

`@mixable` 新增一个合法位置：具体 `type` 中显式声明为 `seal` 的非静态字段。

```feng
open type View {
  @mixable
  seal let style: Style;

  @mixable
  seal var frame: Rect;
}
```

字段可以是 `let` 或 `var`，可以具有现有规则允许的类型、初值和其他正交注解。
`@mixable` 仍不接受参数。

### 4.2 合法位置矩阵

| 声明 | `@mixable` 是否合法 | 成员展开行为 |
| --- | --- | --- |
| `open` 或默认可见性实例字段 | 否；注解冗余 | 沿用现状，自动参与字段展开 |
| `seal` 实例字段 | 是 | 仅标注 `@mixable` 时参与展开 |
| 任意可见性的静态字段 | 否 | 不参与展开 |
| `open` / `seal static` 方法 | 是 | 完全沿用现有 wrapper 语义 |
| 实例方法、构造器、终结器 | 否 | 不因本草案改变 |
| object-form `spec` 成员 | 否 | 不因本草案改变 |
| `fit` 成员 | 只有现有合法静态方法位置 | `fit` 仍不能声明字段 |

禁止在公开实例字段上写 `@mixable`。公开实例字段已经无条件参与展开；允许
冗余注解既不增加能力，也会让读者误以为公开字段需要显式选择。若以后需要统一改为
“所有字段都必须显式 opt-in”，应作为改变现有公开字段行为的独立设计处理。

### 4.3 声明检查时机

Parser 只解析注解并保存归一化的 `is_mixable` 声明事实，不负责解析字段类型、判断
可见性或决定某次成员展开的结果。

语义层必须在任意目标字段生成前检查 `@mixable` 字段声明本身是否合法，包括：

- 注解无参数；
- owner 是具体 `type`；
- 成员是实例字段；
- 字段显式声明为 `seal`；
- 字段完整类型满足 §8.2 的有效可见范围要求。

声明合法性不依赖当前是否存在任何目标展开该来源。非法声明必须在注解字段处报错，
不能因为尚未被 mix 而被静默忽略，也不能等到 codegen 才报错。

声明检查通过后，某个字段是否参与某次 `...`，再由成员展开阶段根据来源成员面、目标
显式冲突和直接展开关系决定。这两个职责不得混为一个“生成成功即合法”的判断。

## 5. 来源字段候选与可见性映射

### 5.1 统一候选规则

来源具体 type 最终成员面中的一个非静态实例字段参与当前展开，当且仅当：

```text
字段在目标位置按现有规则可见且为 open
或
字段为 seal && is_mixable，且当前目标直接展开该来源 type
```

第二个分支既用于当前成员展开，也用于 §5.4 的直接 mix 授权资格判断。它不把来源字段
变成普通公开成员；不满足完整直接授权条件的普通字段访问仍按 seal 拒绝。

### 5.2 生成结果

| 来源实例字段 | 是否生成 | 目标生成字段 |
| --- | --- | --- |
| `open let/var` | 沿用现状：是 | `open let/var` |
| `@mixable seal let/var` | 是 | `@mixable seal let/var` 的归一化等价字段 |
| 普通 `seal let/var` | 否 | 无 |
| 任意静态字段 | 否 | 无 |

每个生成字段保留：

- 名称；
- 完成来源 owner 泛型实参替换后的字段类型；
- `let` / `var` 可变性；
- 来源可见性；
- 对 seal 字段而言，用于下一层传播的 `is_mixable` 能力事实；
- 目标 mix 声明与原始来源字段的位置映射。

来源字段的初始化器 AST、原始声明身份、doc comment、`@friend` 集合和其他普通注解不
复制到目标。`is_mixable` 是生成规则要求保留的归一化能力事实，不等同于复制任意注解
数组。

### 5.3 目标字段的归属

生成字段是目标自身的普通字段：

```feng
open type View {
  @mixable
  seal var frame: Rect;
}

open type Button {
  ...: View;
}
```

概念结果为：

```feng
open type Button {
  @mixable
  seal var frame: Rect;
}
```

`Button` 的实例方法和静态方法访问生成字段时，只使用现有“目标 type 自身访问自己
的 seal 成员”规则。生成字段属于 `Button`，不是 `View.frame` 的别名；对来源原始字段
的访问则只由下一节定义的直接 mix 授权决定。fit、`@friend`、module、spec 或其他
上下文均不因字段被复制到目标而自动获得新权限。

### 5.4 `@mixable seal` 字段的直接 mix 授权

对一个已经通过现有来源解析和字段 `@mixable` 契约检查的直接成员展开声明：

```feng
type Target {
  ...: Source;
  // 或 ...: Source = Source();
  // 或 ... = Source();
}
```

三种形式建立相同的直接 mix 关系。该关系同时用于字段候选选择、生成初始化投影，以及
目标源码中的受限字段访问。构造表达式只决定现有字段初始化路径，不扩大授权成员集合。

直接 mix 授权仅在以下条件同时成立时放行来源字段访问：

1. 当前访问发生在 `Target` 自身的实例方法或静态方法中，或者发生在编译器为
   `Target` 生成的字段初始化投影中；
2. 被访问字段属于 `Source` 的来源成员面，是非静态实例字段，并同时具有 `seal` 和
   `is_mixable` 声明事实；
3. `Target` 通过上述任一合法形式直接 mix 对应 `Source`；间接传播关系不构成对原始
   Source 字段的访问权；
4. 同包来源使用 AST 声明事实，跨包来源使用 package-public `.ft` 恢复的同一组
   owner、字段 kind、`seal`、instance、`is_mixable`、类型与可变性事实。

授权适用于静态类型解析为对应 `Source` 实例的字段表达式。`let` 字段只允许按现有规则
读取，`var` 字段允许按现有规则读取和赋值：

```feng
open type Button {
  ...: View;

  func copyFrame(view: View): void {
    self.frame = view.frame; // 允许：读取 View 的 @mixable seal 字段
  }

  static func clearFrame(view: View): void {
    view.frame = Rect(); // 允许：写入 View 的 @mixable seal var 字段
  }
}
```

授权取决于直接 mix 声明，而不取决于同名字段是否因目标显式成员优先而最终生成。这样
目标显式声明自己的同名字段后，仍可像现有 `@mixable seal static` 方法一样组合来源
实现或状态。

字段上的 `is_mixable` 因此表达以下两类相互一致的能力：

1. 在来源最终成员面中，把 `seal + instance + is_mixable` 字段与现有 open/default
   实例字段合并为本次 `...` 的字段候选集合；
2. 为 Target 生成保持名称、类型、可变性、seal 和 `is_mixable` 的普通目标字段；
3. 对带来源构造表达式的形式，从同一次求值得到的临时 Source 值读取已选字段并初始化
   目标生成字段；
4. 在上述直接目标方法上下文中访问任意对应 Source 实例的该字段；
5. 把生成字段的 `is_mixable` 写入后续 symbol / `.ft`，支持下一层显式成员展开和授权。

该授权不适用于其他 type、顶层函数、fit 方法、构造器、字段初始化器、间接 mix 目标或
仅共同实现相同 spec 的 type。`is_mixable` 不是新的 visibility 或 `@friend`；字段仍为
seal，普通上下文继续拒绝访问，只有“字段资格 + 直接 mix 关系 + 目标方法上下文”三者
同时成立时才走受限授权分支。Source 自身及现有 `@friend` 是否可以访问该字段，继续
完全使用既有规则。

## 6. 初始化与绑定语义

### 6.1 默认零值形式

对于：

```feng
open type Button {
  ...: View;
}
```

`@mixable seal` 字段与现有公开生成字段使用完全相同的默认零值语义：

- 不构造完整 `View` 实例；
- 不执行来源字段初始化器、构造器、对象字面量阶段或终结逻辑；
- 生成 `var` 取得字段类型的普通默认零值；
- 生成 `let` 保持现有未显式绑定状态；
- 不增加分配、释放或引用计数。

### 6.2 显式来源构造形式

对于：

```feng
open type Button {
  ...: View = View();
}
```

继续使用现有流程：来源构造表达式只求值一次，全部生成字段从同一个完整来源值复制，
之后临时来源值按普通生命周期规则释放。来源 `let` 的声明绑定、构造器绑定和对象字面量
绑定事实，以及来源 `var` 的最终值，均沿用当前成员展开规则。

编译器为该 `...` 生成的字段投影读取被选中的 `@mixable seal` 来源字段时，使用 §5.4
定义的直接 mix 授权。该投影只访问已经由语义层选中并建立来源映射的字段。Target 自身
的实例方法或静态方法也可按 §5.4 手写访问对应 Source 实例上的字段；其他源码上下文
仍不能把编译器投影路径当作普通 seal 访问许可。

跨包时，provider 必须先从 `.ft` 恢复字段的 owner、seal、instance 与 mixable 能力
事实；成员展开阶段只把合法的 `seal + is_mixable` 字段生成到 Target，codegen 才能对
这组已生成字段复用现有布局借用路径完成复制。不得让 codegen 自行遍历或选择普通 seal
字段，也不得把 `.ft` 中存在布局事实单独解释为用户可访问；源码访问仍须满足 §5.4 的
完整直接授权条件。

### 6.3 运行时模型

生成字段直接进入目标对象布局。稳定运行阶段：

- `self.frame` 与手写目标字段使用相同寻址和读写路径；
- 已授权的 `view.frame` 与同一 Source 类型的普通可见字段使用相同寻址和读写路径；
- 不为成员展开额外保存来源对象；
- 不新增 getter、resolver、witness 或专用动态 offset；泛型布局继续复用普通字段的现有
  reified offset 路径；
- 不增加 `@mixable seal` 专用运行时分支。

显式来源构造的初始化成本继续等价于当前公开字段展开成本，本草案不增加新的来源构造
或字段复制次数。

## 7. 多层展开、冲突与 spec

### 7.1 多层传播

生成的 seal 字段继续保留 `is_mixable`，所以每一层都显式建立直接展开关系时可以传播：

```text
View
  @mixable seal frame
    ↓ Container ...: View
Container
  @mixable seal frame
    ↓ VStack ...: Container
VStack
  @mixable seal frame
```

每一层都得到独立的目标字段槽位。传播的是字段声明形状和 mix 能力，不是同一存储
地址。每一条直接 mix 边只授权其直接目标：`Container` 可以访问任意 `View` 实例的
对应 mixable seal 字段，`VStack` 可以访问任意 `Container` 实例的对应生成字段；
`VStack` 不因间接关系获得访问原始 `View.frame` 的权限。如果 `VStack` 也需要访问
`View.frame`，必须显式建立对 `View` 的直接 mix 关系。

如果中间 type 显式声明同名字段并按现有显式成员优先规则跳过来源字段，则下一层只根据
该中间 type 最终保留字段的公开性或 `is_mixable` 事实决定，不能绕过中间层直接恢复被
跳过的原始字段。

### 7.2 冲突

目标显式成员优先、字段与方法同名冲突、多个来源之间无隐式优先级、泛型替换后的冲突
以及诊断来源映射，完全复用现有公开字段展开规则。

实现不得为 seal 字段新增按来源顺序覆盖、自动去重或 TUI 专用字段名单。若等价手写的
目标字段集合会冲突，生成后同样必须报错。

### 7.3 与 spec 的关系

`@mixable seal` 字段进入目标普通成员表后，按现有 type 对 object-form `spec` 的满足
规则参与 witness 建立：

- spec 的 `seal` 字段需求是否可由目标生成 seal 字段满足，沿用既有规则；
- spec 的 `open` 字段需求不会因本草案放宽为可由 seal 字段满足；
- spec seal 成员的访问授权仍由 spec 规则决定；
- `@mixable`、直接展开关系和字段来源映射都不进入 witness ABI。

本草案不为 spec 增加字段复制、友元或共同实现者授权。

## 8. 跨包与 `.ft`

### 8.1 所需声明事实

当来源 type 可以跨包作为成员展开来源时，package-public `.ft` 必须为
`@mixable seal` 实例字段保留至少以下事实：

- owner 与字段声明身份；
- 字段 kind、名称、类型和 `let` / `var`；
- `visibility = seal`；
- `is_static = false`；
- `is_mixable = true`；
- 现有 `let` 声明绑定事实；
- 泛型 owner 实参替换和对象布局所需的既有类型事实。

当前 symbol 声明模型已有独立的 visibility、static 与 `is_mixable` 事实；package-public
`.ft` 也已经为对象布局选择所有字段，通用属性 writer / reader 与 imported member 恢复
路径均可承载 `is_mixable`。因此实现只需让来源字段和生成字段形成正确的归一化事实，
并**验证**该事实经现有通路进入和恢复 `.ft`，不需要新增字段选择规则或平行的 mixable
格式。现有属性枚举即使保留带有 `METHOD` 的历史内部名称，也不得改变 wire 数值和含义。

目标导出前已经完成字段展开。生成的 `seal + is_mixable` 字段也必须保留到目标 `.ft`，
使下游包显式展开该目标时可以继续多层传播。

### 8.2 字段类型的有效可见范围

`@mixable seal` 字段虽然不是普通公开成员，但它可以跨 type、跨包生成新的目标字段，
因此其完整字段类型必须按所属 type 中同位置 open 实例字段的有效可见范围检查，包括
泛型实参、数组、指针及其他递归组成类型。

例如，公开来源 type 中的 `@mixable seal` 字段不能使用包外无法形成合法目标字段类型
的私有组成类型。普通未标注 `@mixable` 的 seal 字段仍使用现有 type-private 声明规则，
不受本条影响。

### 8.3 访问与候选解释

`.ft` 记录能力事实不等于公开字段。consumer 对字段访问和成员展开分别解释同一组事实：

```text
源码字段访问
  → open：沿用普通访问规则
  → seal：先按 owner 自身、@friend 等既有规则判断
  → seal && instance && is_mixable：还可检查 §5.4 的直接 mix 授权
  → is_mixable 单独存在、间接 mix 或非目标方法上下文：拒绝

成员展开来源选择
  → instance field
  → open/default 或 seal && is_mixable
  → 合并为当前 Source 的生成字段集合

带 SourceConstruction 的生成初始化投影
  → 只遍历上一步已生成并映射到来源的字段
  → 允许读取临时 Source 中对应值并初始化目标字段
  → 与目标源码访问共用同一直接 mix 关系，但不绕过字段资格检查
```

同包 AST 与 imported `.ft` 必须进入同一个字段资格、候选和直接授权查询。不得因为
workspace cache 暴露了更多私有声明，或因为 package-public `.ft` 为对象布局记录了 seal
字段，就让未标注 `@mixable` 的普通 seal 字段参与展开或获得直接授权。

### 8.4 二进制与性能约束

显式来源构造后的字段复制必须继续复用现有对象布局和字段借用路径，不新增 runtime
helper、动态字段表或 getter ABI。若实现调查发现跨包 `@mixable seal` 字段无法在不增加
运行时开销或修改 ABI 的情况下复用现有路径，必须暂停实施并提交人工决策，不能私自
增加补偿机制。

## 9. 与现有 `@mixable static` 的阶段关系

本草案不修改静态方法规则。当前实现中，“首参数是 type 满足的 object-form spec”实际
分为两个不同责任：

1. Parser 在方法声明处只把 `@mixable` 归一化为 `is_mixable`，不做 spec 解析或满足
   检查。
2. 静态 wrapper 候选阶段会解析首参数、确认其为非变长 object-form spec，并检查当前
   mix 目标 type 是否满足该 spec；不满足时报目标展开诊断。
3. 完整声明语义解析中的 `validate_mixable_method_contract` 会检查标注位置、参数形状，
   并检查方法 owner type（type 成员的所属 type，或 fit 的目标 type）是否满足首参数
   spec；不满足时报来源声明诊断。

由于当前流水线先传播静态 wrapper、后执行完整声明解析，所以 owner type 的完整声明
诊断在现有实现时序上晚于 wrapper 生成；wrapper 候选阶段自身只负责目标关系及必要的
候选形状过滤。不能把现状概括为“所有检查都在添加注解时”或“所有检查都在生成 wrapper
时”。

`@mixable seal` 字段没有首参数、spec 注入或 wrapper。其职责应保持简单：字段声明契约
在生成前验证；某次直接 `...` 是否选择该字段在字段展开阶段验证；目标源码访问在成员
解析阶段检查同一直接 mix 关系；codegen 只消费已经生成的普通目标字段或已经通过语义
授权的普通字段表达式。

## 10. 编译器实现建议

### 10.1 Parser / AST

- 沿用现有字段注解解析，不增加语法分支；
- 把 `FengTypeMember.is_mixable` 从“只表示方法”泛化为成员级归一化能力事实；
- 对字段也从内建注解生成该事实，后续阶段不得反复解析注解文本；
- 生成字段保留 `is_mixable`，但不复制原始 annotation 数组；
- **验证** AST dump、来源位置与释放逻辑继续覆盖字段事实。

### 10.2 Semantic

提供按成员 kind 分派的统一 mixable 契约检查：

```text
mixable method
  → 完全沿用现有 static + first object-spec contract

mixable field
  → concrete type + instance + explicit seal contract

other member
  → 非法标注位置
```

字段来源候选查询统一表达为：

```text
ordinary open instance field
或
seal instance field with is_mixable
```

`expand_type_mixed_fields` 只消费已经验证的声明事实，并把现有“公开实例字段”判断扩展为
上述统一候选谓词；`create_mixed_field` 额外保留 seal 字段的 `is_mixable`。字段克隆、
泛型替换和来源映射继续走既有路径，并作为**验证**项确认行为不变。

现有 mixable seal 方法授权与新增字段授权应共享“目标是否直接 mix 来源”的关系查询，
再按 member kind 检查资格：方法继续要求 `seal + static + is_mixable`，字段要求
`seal + instance + is_mixable`。字段资格查询同时服务于字段展开候选、生成初始化映射和
受限源码成员访问；不得把 `is_mixable` 改写为 open，也不得让直接 mix 关系跳过成员
资格检查。

现有普通成员访问入口已经调用 mixable seal 直接授权查询，因此不新增平行的字段访问
系统；只需把现有方法专用的成员资格判断泛化为按 member kind 判断，并继续复用同一个
Target / Source 直接 mix 关系。应**验证**既有上下文筛选仍排除构造器、字段初始化器、
fit、顶层、其他 type 和间接 mix 上下文。codegen、LSP 或跨包 provider 不得分别复制
`seal && is_mixable` 特判。

### 10.3 Symbol / `.ft` 现有通路验证

本草案不新增 `.ft` 字段或选择规则。字段 AST 与生成字段形成 `is_mixable` 后，应明确
**验证**以下现有通路：

- source export 把成员级 `is_mixable` 写入统一 symbol 声明；
- package-public 既有字段选择会保留合法 `seal + instance field + is_mixable` 事实；
- writer / reader 原样保存和恢复字段的 visibility、static、mixable、类型、可变性和绑定
  状态；
- imported provider 原样恢复 seal 字段；普通上下文继续过滤，字段展开和受限访问由
  semantic 的统一字段资格与直接授权查询决定；
- `.ft` wire 保持兼容，不增加平行属性或改变既有属性数值。

### 10.4 Codegen / LSP

Codegen 对成员展开原则上只看到已经生成的普通目标字段；对 Target 源码中的来源字段
表达式，只接收已经通过语义授权的普通字段读写。应验证现有默认零值、显式来源构造、
字段借用、普通字段读写、泛型 layout、RC 和销毁路径可以直接覆盖 seal 来源，不增加
专用发码。

LSP 必须保持两种声明归属视图：

- `Source` 的 seal 字段在普通上下文继续隐藏；在直接 mix Source 的 Target 实例方法或
  静态方法中，仅对应的 `seal + instance + is_mixable` 字段按 §5.4 显示为可访问；
- 目标自身上下文按普通目标 seal 字段规则展示生成字段，并让 definition 指向原始来源
  字段或已有来源映射。

LSP 必须复用语义层的直接 mix 授权查询，不得为了让某个上下文看见 mix 候选而把来源
字段加入全局普通可访问成员集合。

## 11. 测试计划

本节全部属于对新增字段资格、直接授权以及既有通用路径的**验证**，不代表额外的语言
能力或独立实现机制。

### 11.1 Parser / AST

- `@mixable seal let` 与 `@mixable seal var` 保存 visibility、mutability 和 mixable 事实；
- `@mixable` 带参数报错；
- `@mixable` 标注 open/default 字段、静态字段、实例方法、构造器、终结器、spec 成员及
  其他非法位置报错；
- 现有 open/seal `@mixable static` 方法 AST 结果不变。

### 11.2 Semantic

- 三种 `...` 形式都选择 `@mixable seal` 实例字段；
- 未标注的普通 seal 字段不参与；
- open 字段保持现有无注解展开行为；
- 目标生成字段保持 seal、类型、`let` / `var` 和 mixable 事实；
- 目标自身成员可按既有规则访问生成字段，外部普通访问被拒绝；
- Target 自身实例方法和静态方法可读取 Source 实例的 `@mixable seal let/var` 字段，
  并可写入 `var`；写入 `let` 仍按现有规则报错；
- 三种直接 `...` 形式建立相同授权；目标显式同名字段使生成字段被跳过时，直接授权仍
  保留；
- 顶层函数、fit 方法、构造器、字段初始化器、其他 type 和仅有间接 mix 关系的 type
  不能因 `is_mixable` 访问原始来源字段；
- 多层展开传播生成字段和 mixable 能力；每层只能访问其直接来源，不能穿透到原始来源；
- 目标显式字段优先、多来源冲突和字段/方法冲突沿用现有结果；
- 泛型 owner、递归组成字段类型和类型实参替换正确；
- `@friend` 与其他普通注解不复制，mixable 能力事实继续保留；
- 生成 seal 字段参与现有 spec seal 成员满足，且不放宽 spec open 成员规则；
- mixable seal 字段的完整类型按同位置 open 字段有效范围检查。

### 11.3 初始化 / Codegen / FCTS

- 默认零值形式不构造来源实例；
- 显式来源构造只求值一次，并复制被选中的 seal `var` 最终值；
- seal `let` 的已绑定与未绑定状态沿用现有规则；
- 值类型、引用类型、callable、泛型字段与 RC/终结路径结果正确；
- 稳定访问与手写目标字段使用相同对象布局，不出现 helper 或额外间接层；
- TUI/GUI 风格的 `View -> Container -> VStack` 多层状态展开行为正确。

### 11.4 跨包与 `.ft`

- 来源字段以 `seal + instance + is_mixable` 写入并从 package-public `.ft` 原样恢复；
- 普通 seal 字段即使因布局存在于 `.ft`，也不能参与成员展开；
- 跨包三种 `...` 形式均能生成目标 seal 字段；
- 跨包显式来源构造可以复制已选字段，Target 的实例方法和静态方法可按直接授权访问
  Source 原始 mixable seal 字段；普通 seal 字段及其他上下文仍被拒绝；
- 目标生成字段再次导出后可以被第三个包继续显式展开；
- workspace cache 与 package-public provider 得到相同语言结果。

### 11.5 回归

- 新增 `test/` 用例验证诊断码、AST、成员表、来源映射、symbol / `.ft` 和 IR/codegen；
- 新增 `fcts/` 用例验证语言行为；
- 不修改既有测试用例；
- 完成全部非文档变更后，在非 Codex 沙箱环境执行 `make test` 全量回归。

## 12. 实施 TODO

实施严格遵循“先正式规范、再代码、后测试”。

核心代码变更收敛为两条：

1. 让字段的归一化 `is_mixable` 事实进入并贯穿 AST、生成字段、symbol 与现有 `.ft`
   成员属性通路；字段展开据此把 `seal + instance + is_mixable` 合并到既有字段候选集合。
2. 复用现有直接 mix 关系，在成员访问时仅对对应的
   `seal + instance + is_mixable` 字段放行。

下列标为 **验证** 的任务只确认现有通用路径和既有拒绝边界在新增字段资格后继续成立，
不代表需要新增一套语义、发码或 ABI。

### TODO 1：正式规范与诊断

- [x] 将本草案 Review 结论写入 `feng-type.md`，唯一正式定义字段候选集合
  `open/default || (seal && is_mixable)`、生成映射、直接 mix 授权、初始化投影和多层
  传播。
- [x] 更新 `feng-function.md` 的 `@mixable` 合法位置矩阵，只引用类型规范中的字段语义，
  不重复生成规则。
- [x] 更新 `feng-visibility.md`，明确字段仍为 seal；只有 `seal + instance + is_mixable`、
  直接 mix 关系与目标实例/静态方法上下文同时成立时才形成受限访问例外。
- [x] 在诊断码主规范中分配或调整注解位置、冗余 open 字段、静态字段及签名可见性诊断。

### TODO 2：声明事实与声明期校验

- [x] 泛化成员级 `is_mixable` 注释与内部表示，对字段解析并保存归一化事实。
- [x] 在字段生成前完成 mixable 成员契约校验：只允许具体 type 的显式 seal 实例字段，
  拒绝 open/default 字段、静态字段、spec 成员及其他位置。
- [x] 保证非法声明即使从未被任何 type 展开也会在声明处报错。
- [x] **验证**现有 `@mixable static` 的首参数和 owner/target spec 检查结果保持不变。

### TODO 3：字段候选与生成

- [x] 建立统一字段候选谓词：非静态实例字段且为 open/default，或者同时为 seal 与
  `is_mixable`；不增加具体 type、字段名或 TUI 特判。
- [x] 复用现有直接 mix 关系查询，并按 member kind 区分方法资格与字段资格；不得把字段
  `is_mixable` 解释为 open。
- [x] 将现有 mixable seal 成员资格从方法专用判断泛化为按 member kind 判断，使普通成员
  访问入口可按直接 mix 关系放行 `seal + instance + is_mixable` 字段；不新增平行访问
  系统，并沿用 `let` / `var` 读写规则。
- [x] **验证**新增字段受限授权没有扩大现有 seal 访问边界：顶层、fit、构造器、字段
  初始化器、其他 type、间接 mix 和共同 spec 实现者仍被拒绝；未标注 `@mixable` 的
  普通 seal 字段仍不得因直接 mix 放行。
- [x] 让生成字段保留 `seal + is_mixable`，供下一层展开与直接授权继续使用。
- [x] **验证**生成字段继续沿用现有克隆路径，保留名称、替换后类型、可变性和来源映射。
- [x] **验证**显式来源构造继续只从已经生成并映射的字段集合执行初始化投影，codegen
  不会重新发现或选择普通 seal 字段。
- [x] **验证**目标显式优先、多个来源冲突、循环检测与多层递归展开继续沿用现有规则。
- [x] **验证**直接字段授权只依赖直接 mix 关系，不依赖同名生成字段是否因目标显式优先
  而被跳过，与现有 mixable seal 静态方法授权一致。
- [x] **验证**中间层显式覆盖会按现有规则自然截断被跳过来源字段的传播。

### TODO 4：Symbol / `.ft` 与有效可见范围

- [x] **验证**现有 source export、symbol 声明、`.ft` writer / reader 和 imported AST
  通用路径会原样传递字段 `is_mixable`；发现缺口时只修复通用成员属性路径。
- [x] **验证**package-public `.ft` wire 保持兼容；字段能力复用现有成员级 mixable 属性，
  不增加平行格式。
- [x] **验证**普通 seal 字段即使因对象布局存在于 `.ft`，仍不会被成员展开误选或被普通
  成员访问放行。
- [x] **验证**生成的 mixable seal 字段会经现有字段导出路径进入目标 `.ft`，支持跨包
  多层传播。
- [x] **验证**同包 AST 与 imported `.ft` 恢复字段进入同一直接授权查询，不需要
  provider 特判。
- [x] **验证**`.ft` 中存在 mixable 事实本身不构成访问权，仍须同时满足直接 mix 关系和
  合法目标方法上下文。
- [x] 复用同位置 open 字段的完整类型有效可见范围检查。

### TODO 5：验证初始化与 Codegen 通路、调整 LSP

- [x] **验证**默认零值形式可直接处理生成 seal 字段，无需专用 codegen。
- [x] **验证**显式来源构造的现有字段借用路径可直接读取被选中的 seal capability 字段，
  包括值/引用语义、泛型 reified layout、RC 和跨包来源；不增加 runtime helper。
- [x] **验证**固定布局跨包来源可由 `.ft` 字段顺序重建布局，reified 泛型来源可继续使用
  现有 field index / offset，不新增字段 offset ABI。
- [x] **验证**现有 `let` 绑定事实计算、目标布局、销毁与来源位置映射可直接复用；发现缺口
  时只修复通用字段展开路径。
- [x] **验证**Target 对 Source 字段的已授权读写可直接复用普通字段 codegen，不新增
  runtime helper、动态检查或间接访问。
- [x] 调整 LSP：Source 原始字段只在获得直接授权的 Target 实例/静态方法上下文中展示，
  其他上下文继续隐藏；目标生成字段保留 definition 来源映射。

### TODO 6：新增验证用例与回归

- [x] **验证**新增 Parser/AST 与非法位置诊断用例。
- [x] **验证**新增 Semantic 成员面、Target 实例/静态方法直接读写、越权拒绝、冲突、
  spec 满足、泛型和多层直接/间接授权用例。
- [x] **验证**新增默认零值、显式来源一次求值、seal `let` / `var`、IR/codegen 与 FCTS
  行为用例；聚焦 FCTS 的 5 项行为用例全部通过。
- [x] **验证**新增同包、跨 module、跨包 `.ft` / `.fb` 及三包传播用例。
- [x] **验证**执行专项测试后，在非 Codex 沙箱环境执行 `make test` 全量回归；UBSan
  单元测试、CLI/LSP、symbol、smoke 和 CLI 脚本均通过，现有 TUI 错误阻断 `std-tests`。

## 13. Review 要点

本草案需要确认以下边界：

1. 字段上的 `@mixable` 只允许显式 seal 实例字段；open/default 字段继续隐式参与且禁止
   冗余标注。
2. mix 时把 open/default 实例字段与 `seal + is_mixable` 实例字段合并为统一候选集合；
   普通 seal 字段不参与。
3. 目标得到自己的 `seal + is_mixable` 字段；直接 Target 的实例方法或静态方法也可以
   读写任意对应 Source 实例上的原始字段，其中 `let` / `var` 规则保持不变。
4. 直接授权与字段最终是否生成相互独立；目标显式同名成员跳过生成时仍保留授权。
5. 授权不适用于顶层、fit、构造器、字段初始化器、其他 type、间接 mix 关系或共同
   spec 实现者；普通 seal 成员不受影响。
6. 生成 seal 字段保留 mixable 能力事实，以支持逐层显式成员展开和逐边直接授权。
7. `@friend` 及其他注解不传播；字段类型、可变性、seal 与 mixable 事实传播。
8. `.ft` 正常记录并恢复 `is_mixable`；该事实只有与直接 mix 关系和合法目标方法上下文
   结合时才形成访问能力。
9. 静态字段、spec 字段、fit 字段、普通 seal 成员以及现有 `@mixable static` 语义均不
   扩展。
10. 不允许以 runtime helper、动态字段表、额外间接访问或新字段 offset ABI 实现该能力。

正式规范与实现已经落地；现有 TUI 回归阻断解除并完成 FCTS 后关闭本草案。
