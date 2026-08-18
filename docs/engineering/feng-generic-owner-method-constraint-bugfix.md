# Feng 泛型 owner 方法约束引用 owner 泛参修复开发文档

> 状态：已完成（2026-08-19）
>
> 本文只修复泛型 `type` / 泛型 `fit` 的方法级泛型约束引用 owner 类型参数时，
> Semantic 与 Codegen 没有使用同一联合泛参作用域的问题。

## 1. 目标

以下声明和调用均为合法代码，必须在同包和跨包场景中正确完成语义检查、witness
物化、发码、链接和运行：

```feng
open spec Surface<T> {
  static func echo(value: T): T;
}

open type Value<T>: Surface<T> {
  static func echo(value: T): T {
    return value;
  }
}

open type Host<T> {
  open static func invoke<U: Surface<T>>(value: T): T {
    return U.echo(value);
  }
}

let result = Host<int>.invoke<Value<int>>(42);
```

泛型 `fit` 中的等价方法必须复用同一处理：

```feng
open type FitHost<T> {}

open fit FitHost<T> {
  open static func invoke<U: Surface<T>>(value: T): T {
    return U.echo(value);
  }
}
```

## 2. 已确认的现状与原因

### 2.1 基线能力正常

实测确认以下两类现有路径均可编译：

- 泛型 owner 的无约束方法级泛参，例如 `Host<T>.invoke<U>(value: U)`；
- 泛型 owner 中使用闭合方法约束，例如 `U: Surface<int>`。

现有 FCTS 也已覆盖泛型 owner 与方法泛参并存时的参数、返回值和 reified
dependency。问题不在泛型 owner 方法的基本 ABI，也不在普通方法级约束。

### 2.2 Semantic 调用点没有关闭约束中的 owner 泛参

实测 `Host<int>.invoke<Value<int>>(42)` 和对应 generic fit 调用均报 `AE0512`。
现有 owner-aware 参数匹配已经按以下顺序关闭参数和返回值：

1. 以具体 owner 实参替换 owner 类型参数；
2. 以具体 fit target 替换 fit 局部参数；
3. 以方法类型实参替换方法泛参。

但 generic candidate 的约束检查只执行第 3 步。`Surface<T>` 中的 owner `T`
因此保持开放，合法的 `Value<int>` 被错误排除。候选选中后的 constraint witness
物化也只替换方法泛参，存在同一缺口。

修复必须让“候选约束检查”和“选中后的 witness 物化”共用 owner-aware 约束实例化
顺序；不得在 `Host`、`Surface`、static 方法或错误码处增加特判。

### 2.3 Codegen 构造约束时丢失联合泛参作用域

删除调用、仅保留声明后，generic type 与 generic fit 两种最小用例均在 Codegen
报 `CE0031`：`Surface<...>` 未注册。

现有 generic instance 收集阶段已经以联合顺序记录开放实例：

```text
[owner 类型参数] + [方法类型参数]
```

例如 `Host<T>.invoke<U: Surface<T>>` 收集的开放 `Surface<T>` 使用 `[T, U]`
作为上下文。问题发生在方法共享体构造 constraint 时：现有代码把 owner constraints
与 method constraints 分开解析；解析后者时活动上下文仅剩 `[U]`，因而无法查到按
`[T, U]` 注册的 `Surface<T>`。

Codegen 已有 `CGTypeParamScope` 表达 owner 与 callable 两段泛参，也已有联合 scope
constraint builder。本次应修正并复用该抽象，使 constraint 解析期间始终激活完整、
顺序稳定的联合上下文；不改变开放 generic spec instance 的身份规则。

### 2.4 实施中发现：generic fit 调用触发的开放实例上下文仍不一致

完成 3.1 节 Semantic 修复并让 generic type/generic fit 共享联合 scope constraint
builder 后，四个最小探针的结果为：

- generic type 仅声明：通过；
- generic type 实际调用：通过；
- generic fit 仅声明：通过；
- generic fit 实际调用：Semantic 通过，但 Codegen 仍报 `CE0031`。

诊断确认开放 `Surface<T>` 的 `[T, U]` 实例已经正确注册，失败的不是开放实例身份。
实际缺口位于共用的 method-call constraint builder：关闭 owner 后，它把
`Surface<T>` 构造成 `Surface<int>` 并直接解析，却没有先让这个新构造的闭合实例进入
现有 generic instance 收集链路。generic type 的其他成员解析此前恰好提前收集了该
闭合实例；generic fit 没有这个旁路，因此暴露错误。

修复应让共用 method-call constraint builder 对替换后的 constraint 执行现有的
“先收集、后解析”流程，并在 owner 仍开放时使用 owner + method 联合作用域。该修复
适用于 type 与 fit，不增加 fit 特判，也不放宽实例查找。

### 2.5 实施中发现：generic fit 方法 wrapper 与调用点的 descriptor 参数不一致

完成 2.4 节修复后，generic fit 实际调用已通过 Semantic 和 Feng Codegen，随后由
host C 编译器报告参数数量不一致：生成的 closed fit wrapper 接收“方法泛参
descriptor + 显式参数 + out”，调用点则在最前面额外传入一个
`FengFunctionDescriptor`。

该问题在 `CE0031` 消除前不可到达。代码核对确认，generic fit 的既有 wrapper
生成链路有意绑定 fit 自己的 function descriptor，以携带 fit 的 reified
dependencies；因此 wrapper 不应再接收调用点 descriptor。遗漏发生在普通方法和
static 方法的直接调用发码：它们无条件构造并传入 descriptor，没有消费“closed
generic fit wrapper 已绑定 descriptor”这一既有事实。

这是现有 wrapper/call 两端的局部不一致，不需要新 ABI。修复应让 closed generic fit
wrapper 调用省略 descriptor 的构造与传参；直接调用 shared body 的路径继续传入，
且 descriptor dependency 来源必须是 fit declaration 的现有 reified dependency
集合。这样既不增加运行时参数，也不改变 generic type 方法 ABI。

### 2.6 实施中发现：嵌套 owner 实参仍未命中已收集的开放 constraint 实例

在直接约束 `U: Surface<T>` 修复通过后，新增的嵌套覆盖
`U: Surface<Box<T>>` 仍在 generic type 共享体发码时报 `CE0031`；同一综合探针尚未
进入后续 generic fit 调用发码。Semantic 已接受声明与调用，因此该问题位于 Codegen
的开放 generic instance 收集/解析一致性，不是新的满足规则。

临时诊断确认了具体顺序问题：通用收集器按“内层 type argument 在前、外层实例在后”
处理 `Surface<Box<T>>`，因此先尝试注册开放 `Box<T>`。现有 generic type shell
注册却在把 `Box<T>` 身份放入 registry 之前，先解析完整 `[T, U]` 上下文的约束；
解析 `U: Surface<Box<T>>` 时，外层 `Surface<Box<T>>` 尚未来得及注册，于是形成
“收集内层 -> 解析外层”的假循环并报 `CE0031`。

这不是 generic instance identity 或 ABI 缺失，而是 constraint 根实例没有遵循
“先有 shell、后解析其嵌套依赖”的顺序。修复应在现有 type-parameter constraint
收集入口中，先按既有身份规则登记每个 constraint 的根 generic shell，再复用现有递归
收集器处理 type arguments；这样收集 `Box<T>` 时，`Surface<Box<T>>` 已可由同一
registry 查询命中。不得改为忽略 constraint、按名称回退、改变全局 generic type
遍历顺序或增加类型专用分支。该调整只修正编译期 constraint 收集的两阶段顺序，不
改变实例身份、descriptor 布局、生成参数或运行时开销。

### 2.7 验证中确认的独立基线：具体 type 的 static 方法不能形成方法值

为覆盖显式 callable value 约束检查而尝试
`let mapper: Mapper = Host<int>.echo<Value<int>>` 时，Semantic 报 `AE0522`。进一步
用无约束、非泛型基线确认 `Plain.map`、`Plain.genericMap<string>` 和
`Generic<int>.map` 均不能形成 callable-form spec 值；因此这是当前所有具体 type
static 方法值都不支持的既有能力边界，不是 owner-dependent constraint 引入的回归。

本专项不新增 static 方法值能力。显式 callable value 路径用已经受支持的实例泛型
方法值验证；static 方法继续覆盖声明、显式/推导调用与 witness 发码。未来若要支持
具体 type static 方法值，应单独定义语义与 Codegen，不得混入本修复。

### 2.8 实施中发现：closed generic fit shell 未收集实例化后的方法 surface

隔离 package consumer 验证显示，导入的 generic `type` owner 实例/静态调用均已通过；
导入的 generic `fit` owner 则在注册 closed fit target 时因 `Box<int>` 未注册而报
`CE0031`。去掉方法值、分别缩减为单个调用后结果不变：`Host<int>` 的 type 路径通过，
`FitHost<int>` 的 fit 路径失败。失败由同一 fit 中
`U: Surface<Box<T>>` 的嵌套方法 constraint 触发，即使 consumer 实际调用的是该 fit
的另一个非嵌套方法。

代码核对确认，generic type instance shell 已复用
`cg_collect_instantiated_callable_member_instances`，会在 owner 实参关闭后收集每个成员的
constraint、参数和返回值；`cg_register_user_fit_shell_for_target` 目前只收集实例化后的
fit spec clauses，没有对 closed generic fit target 执行同一成员 surface 收集。随后
`cg_register_user_fit_members` 直接解析所有方法签名，因而首先遇到尚未登记的
`Box<int>`。

修复应在现有 closed generic fit shell 注册阶段复用同一个 instantiated-callable
collector，并继续由后续统一 member registration 解析类型；不得在 `Box`、导入包或
被调用的方法处特判。开放 generic fit 模板仍由声明预收集链路处理，fit 可见性、FT
格式、符号选择和 wrapper ABI 均不变。

### 2.9 实施中发现：导入 generic fit 的 static 调用仍在发码阶段静默失败

复用 instantiated-callable collector 后重新缩减验证：导入 generic fit 的实例方法
已经通过编译；同一 closed fit target 的 static 泛型方法仍在 consumer 的声明发码阶段
失败，并由外层补成 `CE0356`，说明具体失败路径没有设置诊断。导入 generic type 的
等价 static 调用继续通过，因此问题不在通用 static generic call 或 owner constraint
满足检查。

代码核对确认失败发生在 static 调用参数 ABI 查询，而不是 shared symbol、descriptor
或 wrapper 签名：`cg_generic_static_method_param_uses_address_abi` 对导入方法会按
`owner_type->static_methods` ordinal 恢复 type 原声明；fit 方法实际保存在
`UserFit.methods`，不属于目标 type 的 static member 数组，因此查询返回 `false` 且未
设置诊断。local closed fit 此前走“直接使用 concrete param type”的快速路径，正好
没有暴露这一缺口。

修复应让同一 ABI 查询入口显式消费已有 `UserFit` 来源：使用 fit 原方法 member、目标
generic type 的 owner 参数域，以及 fit 声明所在 program，复用现有 declared-param
解析与 address/direct 分类；普通 type 的 ordinal 恢复保持不变。查询失败还应产生明确
的内部诊断，不再由外层退化成无原因 `CE0356`。该修复不改变 shared symbol、wrapper
签名、descriptor 参数、FT 或跨包 ABI。

### 2.10 FCTS 验证中发现：同包 closed generic fit static wrapper 的参数分类错误

新增同包 FCTS 后，`LocalFitHost<int>.echo<FitValue<int>>(13)` 的 Feng Semantic 与
Codegen 均完成，但 host C 编译器报告：closed fit wrapper 的 `value` 形参是具体
`int64_t`，调用点却传入 `void *`。生成代码确认 witness 和目标 wrapper 均已正确选择，
问题仅发生在显式参数的 direct/address ABI 分类。

2.9 节为修复导入 fit static 调用，让 ABI 查询能够从 `UserFit` 恢复 fit 原方法；该
查询得到的是 shared body 中开放 `T` 的 address ABI。导入调用实际进入 shared body，
因此该结果正确；同包 closed generic fit 调用则进入已将 owner `T` 具体化的 wrapper，
wrapper 的参数 ABI 应按 wrapper 的具体方法 surface 分类。当前查询没有消费调用点
已经确定的“进入 closed wrapper / 进入 shared body”事实，因而把后一种规则错误用于
前一种调用。

修复应保留并优先复用既有“本地具体方法 surface”参数分类；只有进入 shared body 的
fit 方法才从 fit 原声明查询开放参数 ABI。该既有分支与 wrapper/shared body 选择使用
同一 owner 实例状态，不需要新 ABI 或新链路。不得改变 wrapper/shared body 签名，不得
增加运行时转换或分支，也不得对 `int`、FCTS 或方法名特判。若该既有状态不足以区分
调用目标，需暂停并由人工决定是否调整更上层抽象。

## 3. 修复方案

### 3.1 Semantic

为 callable type parameter constraint 建立统一的 owner-aware 实例化入口，顺序与
现有 owner-aware 参数匹配一致：

```text
原始 constraint
  -> owner type 实参替换
  -> fit target 实参替换
  -> callable 方法类型实参替换
```

以下现有路径统一消费该结果：

- overload candidate 的泛型约束满足检查；
- callable value 的显式方法类型实参约束检查；
- 选中 callable 后的 object-form constraint witness 物化。

顶层函数和非泛型 owner 通过同一入口，owner 信息为空时保持原行为。约束不满足的
候选仍按现有规则被排除，不能因本修复放宽约束。

### 3.2 Codegen

- 修正 `CGTypeParamScope` 的 constraint builder：两段参数先形成同一有序作用域，
  再在该作用域内解析每个 constraint；输出仍按 owner 参数在前、方法参数在后的
  现有 descriptor 顺序排列；
- generic type 方法共享体复用该 builder，不再分别解析两段 constraints；
- generic fit 方法共享体复用同一 builder，不保留平行的手工拼接算法；
- method-call constraint builder 对 owner 替换后的 constraint 复用现有“先收集、
  后解析”流程；owner 仍开放时使用同一 owner + method 联合作用域，不依赖其他成员
  解析是否碰巧提前注册闭合实例；
- type-parameter constraint 收集先按现有 generic instance 身份登记根实例 shell，再递归
  收集嵌套 type arguments，消除 `Surface<Box<T>>` 的假循环；不改变全局遍历顺序；
- closed generic fit shell 与 closed generic type shell 复用同一 callable surface 收集器，
  在统一 member registration 前收集 owner 替换后的 constraint、参数和返回值；
- generic fit 的 closed wrapper 调用消费既有“descriptor 已绑定”事实，不重复构造
  或传递 descriptor；直接 shared 调用继续使用 fit declaration 的既有 reified
  dependency 集合；
- generic fit static 方法的参数 ABI 查询从已有 `UserFit` 恢复原方法和声明 program，
  并复用现有 shared-method 参数类型解析及 address/direct 分类；本地 concrete 方法
  继续优先使用既有具体参数 surface，只有 shared fit 方法才查询开放参数分类；普通
  type 方法继续按原声明 ordinal 恢复；
- instance 收集、开放实例身份、descriptor/witness 布局、共享体 ABI 和调用 ABI
  均保持不变。

### 3.3 同包与跨包

provider 独立构建时必须能够发出 generic type/fit 方法共享体；consumer 只读取
package-public `.ft` / `.fb` 时，owner 实参替换、方法约束检查和现有 package callable
符号选择必须继续工作。本修复不增加 Symbol/Exporter 格式或查询，不新增跨包专用
链路。

## 4. 范围边界

本次包含：

- 泛型 `type` 与泛型 `fit`；
- 实例方法与静态方法；
- 显式方法类型实参与现有类型实参推导；
- constraint 中 owner 泛参的直接、嵌套及重排引用；
- 同包调用和隔离 package consumer 调用；
- 正向行为及不满足约束的反向验证。

本次不包含：

- object-form `spec` requirement 自身声明方法级泛参；该问题继续由
  [object-form `spec` 方法级泛型修复](feng-object-form-spec-generic-method-bugfix.md)
  跟踪；
- 新的满足规则、variance、结构满足或 fit 可见性规则；
- generic instance identity、witness ABI、descriptor ABI、`.ft` wire 格式或
  runtime ABI 变更；
- 与正确性无关的单态化或性能优化。
- 具体 `type` / `fit` static 方法形成 callable-form spec 值的新能力。

若实施需要修改上述 ABI/格式、增加运行时判断或不能复用联合泛参作用域，必须暂停
并由人工决策。

## 5. 性能与兼容性

- 所有新增工作仅发生在编译期；生成程序不增加分支、查找、分配或间接调用；
- 已工作的顶层泛型函数、非泛型 owner 方法约束、无约束 generic owner 方法以及
  不满足约束的诊断行为保持不变；
- owner 与方法泛参的 descriptor 顺序及索引保持不变；
- 同包与跨包使用同一 Semantic 替换顺序和 Codegen 联合作用域。

## 6. TODO

- [x] **分析与复现**：分别确认 generic type/generic fit 的声明期 `CE0031`、调用点
  `AE0512`，并验证无约束方法和闭合 constraint 基线正常。
- [x] **实际变更（Semantic）**：抽取 owner-aware callable constraint 实例化，
  统一用于候选满足检查、callable value 显式类型实参检查和 constraint witness
  物化。
- [x] **实际变更（Codegen）**：修正联合 `CGTypeParamScope` constraint 构造，并让
  generic type 与 generic fit 方法共享体复用该入口；查明并修复嵌套 owner 实参下
  开放 constraint 实例的收集/解析身份不一致；让 closed generic fit shell 复用
  既有实例化 callable surface 收集器；统一 closed fit wrapper 的 descriptor 调用
  规则，并让 fit static 方法复用既有 shared-method 参数 ABI 查询。
- [x] **验证（Semantic）**：覆盖 type/fit、实例/static、显式/推导、受支持的实例
  泛型方法值、嵌套 owner 实参及不满足约束的反向用例。
- [x] **验证（Codegen）**：覆盖联合上下文中的开放 generic spec constraint、witness
  static/instance 调用及生成 C 编译，不新增 ABI 或运行时开销。
- [x] **验证（CLI 跨包）**：provider 独立 pack，consumer 只读 `.fb`，验证 generic
  type/fit 方法的编译、链接和运行。
- [x] **验证（FCTS）**：增加同包与跨包可观察行为，覆盖 generic type 与 generic fit
  的 owner-dependent 方法约束。
- [x] **验证（全量回归）**：在沙箱外执行 `make test` 并记录完整结果。

## 7. 验证结果

2026-08-19 在沙箱外完成：

- Semantic 定向测试：通过；
- Codegen 定向测试及生成 C 编译：通过；
- CLI 隔离 package provider/consumer 编译、链接和运行：通过；
- FCTS：`801 passed, 0 failed, 0 skipped`，新增同包与跨包用例均通过；
- 全量 `make test`：通过，包含 UBSan 与普通 `-O2 -Werror` 两阶段回归。
