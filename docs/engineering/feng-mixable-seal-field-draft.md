# Feng `@mixable seal` 实例字段开发草案

> **状态**：待 Review，尚未实施。
>
> **主规范归属**：Review 通过后，字段展开的正式语义统一更新到
> [`feng-type.md`](../specifications/feng-type.md)；
> [`feng-function.md`](../specifications/feng-function.md) 只把 `@mixable` 的合法标注位置
> 扩展到本草案定义的 `seal` 实例字段，并引用类型规范；
> [`feng-visibility.md`](../specifications/feng-visibility.md) 只记录对应的受限能力事实与
> 普通 `seal` 访问边界，不重复字段生成规则。
>
> **范围**：只让具体 `type` 中显式标注 `@mixable` 的 `seal` 实例字段参与既有 `...`
> 成员展开，并在目标中生成同名、同类型、同可变性且仍为 `seal` 的普通实例字段。
> `is_mixable` 只供编译器的成员展开、生成初始化投影和多层传播使用，不放宽任何用户
> 源码中的普通字段访问。本草案不改变现有 `@mixable static` 方法、普通 type 成员
> 可见性、spec witness、fit 访问规则或运行时 ABI。

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
得到自己的 `seal` 字段。`@mixable` 在字段上的作用只是声明一项编译期展开能力，不是
继承、共同实现某个 `spec`、type 间友元关系或新的成员可见性。

## 2. 设计目标

1. 让来源 type 可以显式选择允许成员展开的 `seal` 实例字段。
2. 目标生成字段保持 `seal`，只按目标自身既有 `seal` 规则访问；Target 的手写代码不
   获得访问 Source 原始 seal 字段的权限。
3. `...: Source`、`...: Source = SourceConstruction` 和
   `... = SourceConstruction` 三种现有形式使用相同的字段候选规则。
4. 保持现有字段展开的名称、类型、`let` / `var`、初始化、绑定、冲突、泛型和生命周期
   语义。
5. 生成字段继续携带 mixable 能力事实，使多层显式展开可以传播受限状态。
6. 同包 AST 与跨包 `.ft` 恢复声明使用同一候选规则；`.ft` 正常记录 `is_mixable`，但
   普通成员访问仍只按现有 `seal` 规则判断。
7. 生成字段的稳定运行阶段访问成本与手写目标字段一致，不增加来源对象、间接访问、
   动态分派或运行时检查。

## 3. 非目标

本草案不处理：

- 静态字段展开；
- 来源实例方法或未标注 `@mixable` 的 `seal` 成员展开；
- `spec` 成员、`fit` 成员字段或顶层绑定上的 `@mixable`；
- 让 Target 或其他 type、顶层函数、fit 方法通过普通成员表达式访问 Source 的
  `@mixable seal` 字段；
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

本草案建议禁止在公开实例字段上写 `@mixable`。公开实例字段已经无条件参与展开；允许
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

第二个分支只在当前成员展开操作中生效。它不把来源字段变成普通公开成员，也不进入
普通字段访问的可见性判断。

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
的 seal 成员”规则。fit、`@friend`、module、spec 或其他上下文均不因字段来自成员
展开而获得新权限。

### 5.4 编译器专用展开投影许可

字段上的 `is_mixable` 只授予编译器成员展开流程以下能力：

1. 在来源最终成员面中，把 `seal + instance + is_mixable` 字段与现有 open/default
   实例字段合并为本次 `...` 的字段候选集合；
2. 为 Target 生成保持名称、类型、可变性、seal 和 `is_mixable` 的普通目标字段；
3. 对带来源构造表达式的形式，从同一次求值得到的临时 Source 值读取已选字段并初始化
   目标生成字段；
4. 把生成字段的 `is_mixable` 写入后续 symbol / `.ft`，支持下一层显式成员展开。

这项许可不进入普通成员访问规则。即使 `Target` 直接展开 `Source`，Target 的实例方法
和静态方法仍不能手写访问某个 Source 实例上的原始字段：

```feng
open type Button {
  ...: View;

  static func readSource(view: View): Rect {
    return view.frame; // 错误：frame 仍是 View 的 seal 字段
  }
}
```

`is_mixable` 不是 visibility、`@friend` 或 protected。普通成员解析、重载候选过滤、
LSP 普通来源成员视图均不得因为字段具有 `is_mixable` 而放行。Source 自身及现有
`@friend` 是否可以访问该字段，继续完全使用既有规则。

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
定义的编译器专用展开投影许可。该许可不等价于用户可书写的成员访问；`Button` 自身
成员中的以下代码仍然非法：

```feng
func readOther(view: View): Rect {
  return view.frame; // 错误：frame 仍是 View 的 seal 字段
}
```

跨包时，provider 必须先从 `.ft` 恢复字段的 seal 与 mixable 能力事实；成员展开阶段只
把合法的 `seal + is_mixable` 字段生成到 Target，codegen 才能对这组已生成字段复用
现有布局借用路径完成复制。不得让 codegen 自行遍历或选择普通 seal 字段，也不得把
`.ft` 中存在布局事实解释为用户可访问。

### 6.3 运行时模型

生成字段直接进入目标对象布局。稳定运行阶段：

- `self.frame` 与手写目标字段使用相同寻址和读写路径；
- 不保存来源对象；
- 不通过 getter、resolver、witness 或动态 offset 间接访问；
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

每一层都得到独立的目标字段槽位。传播的是字段声明形状和 mix 能力，不是同一存储地址
或对来源字段的普通访问权限。`Container` 只能按自身 seal 规则访问自己生成的
`Container.frame`，`VStack` 也只能访问自己生成的 `VStack.frame`；二者都不会因为
成员展开而获得手写访问某个 `View.frame` 或 `Container.frame` 来源实例字段的权限。

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

当前 symbol 声明模型已有独立的 visibility、static 与 `is_mixable` 事实。本草案应复用
这些正交事实；如果现有 `.ft` 属性或内部命名带有 `METHOD` 字样，只能在保持 wire 数值
和兼容性的前提下泛化其含义，不能为字段再建立平行的 mixable 表示。

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

`.ft` 记录能力事实不等于公开字段：

```text
普通成员查找
  → visibility = seal
  → is_mixable 不参与判断
  → 继续只按当前 type 自身、@friend 等既有规则决定

成员展开来源选择
  → instance field
  → open/default 或 seal && is_mixable
  → 合并为当前 Source 的生成字段集合

带 SourceConstruction 的生成初始化投影
  → 只遍历上一步已生成并映射到来源的字段
  → 允许读取临时 Source 中对应值并初始化目标字段
  → 不形成用户成员访问权限
```

同包 AST 与 imported `.ft` 必须进入同一个字段候选查询。不得因为 workspace cache 暴露
了更多私有声明，或因为 package-public `.ft` 为对象布局记录了 seal 字段，就让未标注
`@mixable` 的普通 seal 字段参与展开。

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
在生成前验证；某次直接 `...` 是否选择该字段在字段展开阶段验证；codegen 只消费已经
生成的普通目标字段。

## 10. 编译器实现建议

### 10.1 Parser / AST

- 沿用现有字段注解解析，不增加语法分支；
- 把 `FengTypeMember.is_mixable` 从“只表示方法”泛化为成员级归一化能力事实；
- 对字段也从内建注解生成该事实，后续阶段不得反复解析注解文本；
- 生成字段保留 `is_mixable`，但不复制原始 annotation 数组；
- AST dump、来源位置与释放逻辑覆盖字段事实。

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

`expand_type_mixed_fields` 只消费已经验证的声明事实；`create_mixed_field` 继续复用同一
字段克隆、泛型替换和来源映射路径，并额外保留 seal 字段的 `is_mixable`。

字段能力查询必须与现有 mixable seal 方法授权查询分离：前者只服务于字段展开候选、
字段生成和初始化来源映射，不得接入 `type_member_is_accessible_from`、普通成员解析或
方法重载可访问性过滤。实现应使用统一的字段候选谓词，不能在 codegen、LSP 或跨包
provider 中分别复制 `seal && is_mixable` 特判。

### 10.3 Symbol / `.ft`

- source export 把字段 AST 的归一化 `is_mixable` 写入统一 symbol 声明；
- package-public 选择必须保留合法 `seal + instance field + is_mixable` 能力事实；
- writer / reader 原样保存和恢复字段的 visibility、static、mixable、类型、可变性和绑定
  状态；
- 普通 provider 成员访问继续过滤 seal，字段展开使用独立能力候选查询；
- 保持 `.ft` wire 兼容；如需泛化内部属性名称，不改变既有数值含义。

### 10.4 Codegen / LSP

Codegen 原则上只看到已经生成的普通目标字段。应验证现有默认零值、显式来源构造、字段
借用、泛型 layout、RC 和销毁路径可以直接覆盖 seal 来源，不增加专用发码。

LSP 必须保持两种声明归属视图：

- `Source` 的 seal 字段在 Source 既有可见域之外继续按普通规则隐藏；即使当前上下文是
  直接 mix Source 的 Target，也不得因 `is_mixable` 显示为可访问来源字段；
- 目标自身上下文按普通目标 seal 字段规则展示生成字段，并让 definition 指向原始来源
  字段或已有来源映射。

不得为了让 LSP 看见 mix 候选而把来源字段加入普通可访问成员集合。

## 11. 测试计划

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
- Target 自身实例方法和静态方法仍不能手写访问 Source 实例的 `@mixable seal` 字段；
- 顶层函数、fit 方法、其他 type 和多层展开中的各层同样不能因 `is_mixable` 访问原始
  来源字段；
- 多层展开只传播生成字段和 mixable 事实，不传播普通成员访问权；
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
- 跨包显式来源构造可以复制已选字段，Target 的手写代码仍不能访问 Source 原始字段；
- 目标生成字段再次导出后可以被第三个包继续显式展开；
- workspace cache 与 package-public provider 得到相同语言结果。

### 11.5 回归

- 新增 `test/` 用例验证诊断码、AST、成员表、来源映射、symbol / `.ft` 和 IR/codegen；
- 新增 `fcts/` 用例验证语言行为；
- 不修改既有测试用例；
- 完成全部非文档变更后，在非 Codex 沙箱环境执行 `make test` 全量回归。

## 12. 实施 TODO

实施严格遵循“先正式规范、再代码、后测试”。

### TODO 1：正式规范与诊断

- [ ] 将本草案 Review 结论写入 `feng-type.md`，唯一正式定义字段候选集合
  `open/default || (seal && is_mixable)`、生成映射、初始化投影和多层传播。
- [ ] 更新 `feng-function.md` 的 `@mixable` 合法位置矩阵，只引用类型规范中的字段语义，
  不重复生成规则。
- [ ] 更新 `feng-visibility.md`，明确字段 `is_mixable` 不参与普通成员可见性，只供编译器
  的成员展开与生成初始化投影使用。
- [ ] 在诊断码主规范中分配或调整注解位置、冗余 open 字段、静态字段及签名可见性诊断。

### TODO 2：声明事实与声明期校验

- [ ] 泛化成员级 `is_mixable` 注释与内部表示，对字段解析并保存归一化事实。
- [ ] 在字段生成前完成 mixable 成员契约校验：只允许具体 type 的显式 seal 实例字段，
  拒绝 open/default 字段、静态字段、spec 成员及其他位置。
- [ ] 保证非法声明即使从未被任何 type 展开也会在声明处报错。
- [ ] 保持现有 `@mixable static` 的首参数和 owner/target spec 检查结果不变。

### TODO 3：字段候选与生成

- [ ] 建立统一字段候选谓词：非静态实例字段且为 open/default，或者同时为 seal 与
  `is_mixable`；不增加具体 type、字段名或 TUI 特判。
- [ ] 让字段候选与现有 mixable seal 方法授权查询保持分离，不修改
  `type_member_is_accessible_from` 或普通字段成员解析。
- [ ] 生成字段保留名称、替换后类型、可变性、seal、mixable 和来源映射。
- [ ] 让显式来源构造只从已经生成并映射的字段集合执行初始化投影；不得从 codegen
  重新发现或选择普通 seal 字段。
- [ ] 复用现有目标显式优先、多个来源冲突、循环检测与多层递归展开。
- [ ] 确认中间层显式覆盖会自然截断被跳过来源字段的传播。

### TODO 4：Symbol / `.ft` 与有效可见范围

- [ ] 让 source export、symbol 声明、`.ft` writer / reader 和 imported AST 原样传递字段
  `is_mixable`。
- [ ] 保持 package-public `.ft` wire 兼容；复用现有成员级 mixable 属性，不增加平行字段
  能力格式。
- [ ] 确保普通 seal 字段即使因对象布局存在于 `.ft`，也不会被成员展开误选或被普通成员
  访问放行。
- [ ] 让生成的 mixable seal 字段继续进入目标 `.ft`，覆盖跨包多层传播。
- [ ] 复用同位置 open 字段的完整类型有效可见范围检查。

### TODO 5：验证现有初始化、Codegen 与 LSP 路径

- [ ] **验证**默认零值形式可直接处理生成 seal 字段；预计无需专用 codegen。
- [ ] **验证**显式来源构造的现有字段借用路径可直接读取被选中的 seal capability 字段，
  包括值/引用语义、泛型 reified layout、RC 和跨包来源；预计不增加 runtime helper。
- [ ] **验证**固定布局跨包来源可由 `.ft` 字段顺序重建布局，reified 泛型来源可继续使用
  现有 field index / offset，不新增字段 offset ABI。
- [ ] **验证**现有 `let` 绑定事实计算、目标布局、销毁与来源位置映射可直接复用；发现缺口
  时只修复通用字段展开路径。
- [ ] 调整 LSP：Source 原始 seal 字段继续按普通可见性隐藏，Target 自身上下文只展示其
  已生成字段，并保留 definition 来源映射。

### TODO 6：新增测试与回归

- [ ] 新增 Parser/AST 与非法位置诊断用例。
- [ ] 新增 Semantic 成员面、普通来源字段访问拒绝、冲突、spec 满足、泛型和多层传播
  用例。
- [ ] 新增默认零值、显式来源一次求值、seal `let` / `var`、IR/codegen 与 FCTS 行为用例。
- [ ] 新增同包、跨 module、跨包 `.ft` / `.fb` 及三包传播用例。
- [ ] 执行专项测试后，在非 Codex 沙箱环境执行 `make test` 全量回归。

## 13. Review 要点

本草案需要确认以下边界：

1. 字段上的 `@mixable` 只允许显式 seal 实例字段；open/default 字段继续隐式参与且禁止
   冗余标注。
2. mix 时把 open/default 实例字段与 `seal + is_mixable` 实例字段合并为统一候选集合；
   普通 seal 字段不参与。
3. 目标得到自己的 `seal + is_mixable` 字段；只有编译器生成初始化投影可以读取临时
   Source 中的对应值，Target 手写代码不能访问 Source 原始字段。
4. 生成 seal 字段保留 mixable 能力事实，以支持逐层显式成员展开。
5. `@friend` 及其他注解不传播；字段类型、可变性、seal 与 mixable 事实传播。
6. `.ft` 正常记录并恢复 `is_mixable`，但普通成员访问仍只按 seal 及现有规则判断。
7. 静态字段、spec 字段、fit 字段、普通 seal 成员以及现有 `@mixable static` 语义均不
   扩展。
8. 不允许以 runtime helper、动态字段表、额外间接访问或新字段 offset ABI 实现该能力。

Review 通过后再更新正式规范和实施代码。
