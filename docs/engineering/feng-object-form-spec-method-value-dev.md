# Feng object-form `spec` 方法值分派开发草案

> **状态**：待 Review，尚未实施。
>
> **性质**：独立 engineering 开发草案，不是语言权威规范。
>
> 本文只规划“通过 object-form `spec` 视角形成方法值”的新增能力，不修改
> [`spec seal` 成员草案](./feng-spec-seal-member-draft.md)，不修改 `@mixable`
> 语义。Review 通过后，正式语言行为必须收敛到
> [Feng 函数规范](../specifications/feng-function.md)、
> [Feng `spec` 规范](../specifications/feng-spec.md) 和
> [Feng 可见性规范](../specifications/feng-visibility.md)，本文只保留实现范围、
> 实施顺序和验收记录。

## 1 背景

Feng 已支持：

- 通过 object-form `spec` 值调用实例方法，并经 witness 分派到实际实现；
- 通过受 object-form `spec` 约束的类型参数调用静态方法；
- 具体 `type` 实例方法、`fit` 实例方法和显式闭合的泛型方法形成方法值；
- 顶层函数、方法值和 lambda 进入 callable-form `spec`；
- callable value 的保存、参数传递、返回、复制、调用和生命周期管理。

但 object-form `spec` 方法目前只能立即调用，不能通过 `spec` 视角形成方法值：

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Reader(offset: int): string;

func bind(value: Readable): Reader {
  return value.read; // 当前不支持
}
```

现有 Semantic 方法值解析只把具体 `type` owner 和可见 `fit` 方法作为候选；现有
Codegen 方法值路径也要求 receiver 是具体 object，并直接绑定已解析的 type/fit
实现。因此，不能只放开一条语义判断：必须增加“spec 方法值”这一稳定语义来源，
并为它生成绑定 subject 与 witness 方法槽的 callable closure。

此前两个已完成专项均明确把该能力排除在各自范围之外：

- [`feng-callable-value-reification-refactor-dev.md`](./feng-callable-value-reification-refactor-dev.md)
  只重构已经合法的 callable value 具化，不增加 object-form spec 方法值分派；
- [`feng-value-type-method-value-capture-dev.md`](./feng-value-type-method-value-capture-dev.md)
  只处理具体值接收者的方法值，不把值接收者改写为 spec box。

本文作为独立开发项补齐该能力。

## 2 Review 决策项

以下内容是本文建议方案，实施前需要 Review 确认：

1. **继续采用目标类型驱动**：`value.method` 只有在绑定、参数、返回或显式转换位置
   已存在 callable-form `spec` 目标时合法；不新增 `let method = value.method` 的自然
   callable 类型推导。
2. **权限在形成点检查**：公开 spec 方法可在普通位置形成方法值；`spec seal` 方法仅
   可在现有合法实现上下文形成。合法形成后的 callable 可以作为普通值传递，调用点
   不重复检查访问权限。
3. **绑定一次、分派目标固定**：receiver 在方法值形成时只求值一次；closure 保存该次
   求值得到的 subject 和 witness 中选定的方法槽。后续 receiver 变量重新赋值不改变
   已形成的方法值。
4. **一次 closure 分配**：实例 spec 方法值只形成一个 callable closure，不增加独立
   receiver box 或通用 lambda capture cell；closure 保存一个托管 subject 和一个非托管
   typed witness 方法指针。
5. **静态 spec 方法值纳入同一能力**：允许在受 object-form `spec` 约束的类型参数视角
   形成 `T.method`；静态方法值不捕获 subject，只绑定当前 `T` witness 中的方法槽。
   该项同时新增“静态成员引用可形成 callable value”的语义，不只是补齐实例 spec
   方法值；如果 Review 不接受，应从本开发项拆出，不得在实施中默认带入。
6. **不增加 runtime ABI 和 `.ft` 格式**：复用现有 callable closure、witness、泛型
   descriptor 和 reified callable dependency 抽象；如实施中证明必须改变 runtime
   私有 ABI、增加动态查找或增加额外分配，应停止并提交人工决策。

## 3 目标与非目标

### 3.1 目标

- 允许 object-form `spec` 实例方法通过 `value.method` 形成 callable value；
- 允许受 object-form `spec` 约束的类型参数通过 `T.method` 形成静态 callable value；
- 公开方法和 `seal` 方法分别遵守现有 spec 成员访问域；
- 重载方法由目标 callable-form `spec` 唯一选择；
- 泛型方法必须显式提供完整类型实参后再形成方法值；
- receiver、subject、witness、泛型描述符和 callable closure 生命周期正确；
- 本包、跨包 `.ft`、普通闭合代码和共享泛型体行为一致；
- 不改变直接 spec 方法调用和既有具体 type/fit 方法值的行为或开销。

### 3.2 非目标

- 不增加匿名 callable 类型或 `let value = object.method` 自然类型推导；
- 不改变 callable-form `spec` 的结构匹配、variance、转换或重载优先级；
- 不支持形成后仍保留未闭合方法级泛参的 first-class polymorphic callable；
- 不修改 `type seal`、`fit` 私有访问权或 `spec seal` 的既有访问域；
- 不修改 `@mixable` 来源传播、wrapper 或具体 type 互访规则；
- 不把静态 spec 方法改为实例值成员，也不允许 `value.staticMethod`；
- 不增加运行时可见性检查、按类型名搜索、tag 分派或动态 descriptor 工厂；
- 不以单态化共享泛型体作为正确性前提；
- 不借机重构无关的 lambda 捕获、普通方法值或 callable ABI。

## 4 语言语义

### 4.1 实例 spec 方法值

实例 spec 方法值使用现有成员引用语法：

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Reader(offset: int): string;

func bind(value: Readable): Reader {
  return value.read;
}

func apply(reader: Reader): string {
  return reader(1);
}

func run(value: Readable): string {
  let first: Reader = value.read;
  let a = first(0);
  let b = apply(value.read);
  return a + b;
}
```

`value.read` 的可观察语义等价于：

```feng
(offset: int) -> value.read(offset)
```

但编译器不得通过普通 lambda capture cell 实现该表达式。方法值必须使用专用的一次
分配 closure，并满足以下绑定规则：

1. `value` 在形成点求值且只求值一次；
2. closure 保存形成点得到的 subject，并保持其生命周期；
3. closure 保存形成点所用 witness 中与 `Readable.read` 对应的实现方法槽；
4. 后续调用不重新读取原 receiver 表达式，不重新选择 receiver；
5. 后续对保存 `value` 的局部变量重新赋值，不改变已形成的方法值；
6. 不同实际实现通过各自 witness 形成的方法值，调用时进入各自实际实现。

object-form spec 值已经具有 `{ subject, witness }` 表示。方法值捕获的是该 spec
视角已经持有的 subject 身份，不重新复制具体对象内容。若具体值语义 subject 在进入
object-form spec 时已经按现有规则装箱，方法值保留同一个 box，不进行第二次装箱。

### 4.2 目标类型与重载

spec 方法值继续使用现有 target-typed callable value 规则。目标 callable-form `spec`
可以来自：

- 显式绑定类型；
- 被调函数的形参类型；
- 当前函数显式声明的返回类型；
- 显式 callable-form `spec` 转换目标。

```feng
let local: Reader = value.read;
consume(value.read);
return value.read;
let converted = (Reader)value.read;
```

以下写法继续非法：

```feng
let inferred = value.read; // 没有 callable-form spec 目标
```

如果 spec 中存在同名重载，Semantic 使用目标 callable-form `spec` 已实例化后的参数、
返回类型、变长参数和泛型形状唯一选择候选。返回类型继续只用于目标 callable 匹配，
不改变普通方法调用的重载规则。

### 4.3 泛型方法

声明方法级泛参的 spec 方法形成方法值时，必须显式提供完整类型实参：

```feng
open spec Transformer {
  func transform<T>(value: T): T;
}

open spec IntTransform(value: int): int;

func bind(value: Transformer): IntTransform {
  return value.transform<int>;
}
```

目标 callable-form `spec` 不反向推导来源方法泛参。Semantic 先检查显式实参数量和
约束，闭合来源签名，再执行普通 target-typed callable 匹配。形成后的值不再携带可由
调用者选择的方法级泛参，调用时只写 `callable(args...)`。

### 4.4 `seal` 方法与 capability 传递

对 `spec seal` 方法，访问权限在方法值形成点执行现有 spec seal 访问域判断：

```feng
open spec Internal {
  seal func update(): void;
}

open spec Action(): void;

open type Worker: Internal {
  func update(): void {}

  func bind(other: Internal): Action {
    return other.update; // 允许：Worker 满足 Internal
  }
}

func bindOutside(value: Internal): Action {
  return value.update; // 拒绝：普通函数不在 Internal 实现域
}
```

合法形成后的 callable 是普通值，可以被返回、保存或传给当前访问域以外的代码。调用
callable 时不再检查其来源方法的可见性。这与现有访问控制的 capability 语义一致：
合法实现上下文本来就可以用 lambda 或公开 wrapper 封装同一次调用。若要求 callable
携带访问标签并在每个调用点继续限制，将需要新的类型传播或运行时检查，不属于本文
建议方案。

### 4.5 静态 spec 方法值

静态 spec 方法值只能从能够确定实现类型 witness 的静态 spec 约束视角形成：

```feng
open spec Factory {
  static func create(): Result;
}

open spec Creator(): Result;

func bind<T: Factory>(): Creator {
  return T.create;
}
```

规则如下：

- `T.create` 在形成点从 `T` 的 descriptor/witness 选择静态方法槽；
- 静态方法值不捕获 subject；
- `seal static` 方法使用与直接静态 spec 方法访问相同的实现上下文判断；
- object-form spec 声明名 `Factory.create` 本身不能选定满足类型或 witness，因此不能据此
  形成 spec 静态方法值；
- `value.create` 通过实例值访问静态成员，继续非法；
- 具体类型名上的 `Concrete.create` 仍属于 type 视角，按现有 type 静态成员规则处理，
  不因本能力改写为 spec 视角。

## 5 Semantic 设计

### 5.1 复用现有成员闭包与访问过滤

Semantic 应增加 object-form spec 方法值候选解析，不复制一套 spec 成员规则：

1. 解析 receiver 的 object-form spec 实例视角；
2. 从现有 spec 完整成员闭包枚举同名、同静态性的 method requirement；
3. 使用现有 `spec seal` 访问判断过滤当前不可访问候选；
4. 使用目标 callable-form `spec` 的已实例化签名匹配候选；
5. 对显式泛型 target 检查类型实参并闭合候选签名；
6. 产生唯一、无匹配或歧义结论；
7. 只在唯一成功后记录稳定的 spec 方法值语义事实。

普通方法调用和方法值必须复用同一 spec 成员闭包、继承替换、可见性过滤与 callable
签名比较基础设施，不得分别实现名称查找、父 spec 展开或 `seal` 特判。

### 5.2 Callable source 分类

现有 `FengSpecCoercionCallableSource` 只区分顶层函数、具体方法值、lambda 和已有
callable 值。建议新增独立来源：

```c
FENG_SPEC_COERCION_CALLABLE_SOURCE_SPEC_METHOD_VALUE
```

不得把 spec 方法值伪装成现有 `METHOD_VALUE`：后者要求具体 owner type、具体 type/fit
成员和 concrete receiver，Codegen 也据此直接定位实现函数。

成功站点至少要稳定记录：

- source object-form spec 的声明和完整实例类型；
- 选中的 spec method requirement；
- requirement 的原声明 spec，用于父 spec 和 `seal` 语义；
- receiver 表达式；
- 目标 callable-form spec 的声明和完整实例类型；
- 已显式闭合的方法级类型实参；
- 实例或静态方法值类别。

这些事实应进入现有 callable coercion sidecar 的通用来源模型，或进入由其直接引用的
专用稳定记录；不得由 Codegen 根据名称重新查找或重新执行重载选择。

### 5.3 Spec member access 分类

现有 `FengSpecMemberAccessKind` 把 spec 方法访问统一记录为 `METHOD_CALL`，注释虽提及
方法值，但实际没有独立 codegen 路径。建议显式区分：

```c
FENG_SPEC_MEMBER_ACCESS_KIND_METHOD_CALL
FENG_SPEC_MEMBER_ACCESS_KIND_METHOD_VALUE
```

该区分用于：

- 保证访问权限在正确形成点检查；
- 为 LSP、诊断和 Codegen 提供稳定用途；
- 防止把字段中保存的 callable value 误判为方法值；
- 避免通过父表达式形状反推该成员是否立即调用。

### 5.4 共享泛型体依赖

当 source spec、目标 callable-form spec、方法级实参或静态约束类型仍包含活动泛参时，
provider 必须把 spec 方法值形成需求写入现有 reified callable dependency 固定 slot。
consumer 在闭合点生成：

- 闭合 callable adapter；
- 闭合 closure descriptor；
- 参数与返回值 ABI bridge 所需的类型描述符；
- 静态方法值所需的实际 witness 方法槽来源。

共享体只按编译期分配的固定 slot 读取描述符，不执行运行时名称搜索、哈希查找、tag
分派或 descriptor 构造。应扩展现有 resolved callable/source 抽象表达 spec method，
不得按具体 spec 名、方法名或参数位置增加特判。

## 6 Codegen 设计

### 6.1 现有基础

object-form spec 值的现有表示为：

```c
struct SpecValue {
    void *subject;
    const SpecWitness *witness;
};
```

`subject` 是托管引用；`witness` 指向只读静态表，不参与 retain/release。直接 spec 方法
调用已经通过 `witness->method(subject, args...)` 分派。

现有具体方法值路径生成 callable closure，保存具体 receiver，并由 typed invoke
adapter 直接调用已解析的 type/fit 实现。该路径明确要求 receiver 是 concrete object，
不能承载运行时才由 spec witness 决定的实现方法。

### 6.2 一次分配 closure

建议为每个闭合的“source spec method + target callable-form spec ABI”组合生成一种
codegen-private closure 支持。概念布局如下：

```c
struct BoundSpecMethod {
    FengManagedHeader _hdr;
    void *_self;                 // 实例方法：subject；静态方法：NULL
    TargetCallableInvoke invoke;
    SourceSpecMethodFn target;   // 形成点从 witness 读取的 typed 方法槽
};
```

形成实例方法值时：

1. 物化 receiver，保证 receiver 表达式只求值一次；
2. 分配一个 closure；
3. 用现有托管赋值保存 `receiver.subject`；
4. 从正确的 receiver witness 视角读取选中 requirement 的 typed 方法槽；
5. 保存生成的 target callable invoke adapter；
6. 返回普通 callable-form spec 值。

形成静态方法值时：

1. 从约束类型参数 descriptor/witness 读取选中的静态方法槽；
2. `_self` 保持 `NULL`；
3. 保存 typed 方法槽和 invoke adapter；
4. 返回普通 callable-form spec 值。

closure descriptor 只把 `_self` 标记为托管槽；`target` 是非托管函数指针。不得保存栈
地址，不得把 witness 表或函数指针作为托管对象处理。

### 6.3 Invoke adapter

实例方法 adapter 的概念调用为：

```c
return closure->target(closure->_self, args...);
```

静态方法 adapter 的概念调用为：

```c
return closure->target(args...);
```

adapter 必须复用现有 callable ABI bridge：

- value ABI 与 address ABI 参数；
- value ABI 与 address ABI 返回值；
- descriptor-sized 泛型值；
- 变长参数的既有预打包数组传递；
- callable-form spec 目标的闭合参数和返回类型。

形成点保存精确 witness 方法槽后，调用点不得再次遍历 witness、按名称查找或判断实际
subject 类型。每次调用不分配对象。

### 6.4 父 spec 与向上转换

如果方法由父 spec 声明，Semantic 必须记录 requirement 原声明和形成点使用的完整
spec 视角。Codegen 复用现有 object-form spec 向上转换和父 witness 投影，取得与直接
方法调用相同的 witness view 后再读取方法槽。

不得假定父 requirement 在子 witness 中具有相同字段偏移，也不得仅按方法名从最外层
witness 猜测目标槽。

## 7 生命周期与值语义

- 引用类型 subject：closure retain 同一对象引用；调用作用于同一实例。
- 已装箱的值类型 subject：closure retain 现有 spec box；不重新复制或二次装箱。
- 默认 object-form spec 值：closure retain 默认 subject，并保存对应默认 witness 槽。
- receiver 临时值：形成方法值前必须物化为具有稳定生命周期的 spec subject；不得借用
  会在形成表达式后失效的栈地址。
- callable 复制：继续使用现有 callable 托管引用语义，多个绑定共享同一个 closure。
- callable 覆盖、正常离开作用域和异常展开：最后一个引用释放时恰好释放一次 subject。
- 静态方法值：没有 subject retain/release，只管理 callable closure 自身。

## 8 跨包与 `.ft`

本能力预计不增加 `.ft` 格式字段：

- object-form spec method requirement、可见性、泛型签名和父 spec 关系已经进入符号表；
- callable-form spec 目标签名已经可跨包恢复；
- consumer 可以根据 imported spec method 语义记录和 witness 布局生成本地 adapter；
- 具体实现成员仍由 witness 封装，consumer 不需要发现或直接访问实际 type/fit 方法。

实施时必须用跨包 provider/consumer 验证上述事实。如果现有 `.ft` 无法稳定恢复
requirement 原声明、完整泛型实例或 witness slot 身份，应先完善通用符号事实；不得按
包名、类型名或方法名增加恢复特判，也不得未经 Review 修改 `.ft` 格式。

## 9 诊断与工具链

至少需要区分以下失败：

- spec 方法值没有 callable-form spec 目标；
- 存在同名方法，但没有签名匹配目标 callable；
- 多个重载同时匹配目标 callable；
- `spec seal` 方法在非法上下文形成方法值；
- 泛型方法未显式提供完整类型实参、实参数量错误或约束不满足；
- 通过实例值形成静态方法值，或通过不能确定实现 witness 的 spec 名形成静态方法值；
- callable 字段读取与同名方法值候选发生现有规则无法消解的冲突。

诊断码和最终消息在权威规范阶段确定，不在本文预先占用编号。

LSP、hover 和 completion 继续显示 spec 方法声明本身。需要目标类型的 `value.method`
表达式应返回目标 callable-form spec 类型；非法 `seal` 方法不应作为当前位置可形成的
方法值候选。不得改变普通方法调用补全和具体 type 成员可见性。

## 10 性能约束

实例 spec 方法值的目标成本：

| 阶段 | 成本 |
| --- | --- |
| 形成 | 一次 receiver 求值、一次 closure 分配、一次 subject retain、一次 witness slot 读取 |
| 复制 | 现有 callable 引用复制 |
| 调用 | 一次 callable adapter 调用、一次已保存方法函数指针间接调用 |
| 释放 | closure 最后释放时一次 subject release |

静态 spec 方法值不执行 subject retain/release。

实现不得：

- 为实例 receiver 增加第二个 box 或 capture cell；
- 每次 callable 调用重新读取 witness slot；
- 每次 callable 调用分配临时对象；
- 增加运行时可见性检查；
- 为非 spec 方法值或直接 spec 方法调用增加新分支或额外间接层。

如果正确实现必须增加上述成本、修改 runtime 私有 ABI 或引入新的动态查找，应停止
实施并提交人工决策。

## 11 测试矩阵

### 11.1 Parser 与 AST

本能力预计不修改语法和 AST。Parser 回归只需确认现有以下表达式继续稳定：

- `value.method`；
- `value.method<T>`；
- `T.staticMethod`；
- `(CallableSpec)value.method`。

如果无需 Parser 变更，不增加重复语法测试。

### 11.2 Semantic

| 场景 | 预期 |
| --- | --- |
| 公开实例 spec 方法进入显式 callable 绑定 | 通过 |
| 公开实例 spec 方法直接作为参数或返回值 | 通过 |
| 显式 callable-form spec 转换承接 spec 方法值 | 通过 |
| 无 callable 目标的 `let value = spec.method` | 拒绝 |
| 重载由目标 callable 唯一选择 | 通过 |
| 无匹配或多个匹配 | 分别报告不匹配或歧义 |
| 显式闭合泛型 spec 方法值 | 通过 |
| 未闭合或约束不满足的泛型方法值 | 拒绝 |
| 合法实现上下文形成 `spec seal` 方法值 | 通过 |
| 普通函数或无关 type 形成 `spec seal` 方法值 | 拒绝 |
| callable 字段读取 | 保持现有字段语义，不误判为方法值 |
| 子 spec 视角形成父 spec 方法值 | 通过并记录正确原声明 |
| 受约束类型参数形成公开/合法 seal 静态方法值 | 通过 |
| 通过实例值或 spec 名形成静态方法值 | 拒绝 |

### 11.3 Codegen 与 FCTS

- 两个不同 type 实现同一 spec，分别形成方法值并调用各自实现；
- receiver 表达式只求值一次；
- receiver 局部变量重新赋值后，已形成方法值仍绑定原 subject；
- 方法值从创建函数返回后继续有效；
- 方法值保存到字段、复制、覆盖和释放；
- 修改型方法通过方法值修改同一个引用 subject 或现有 spec box；
- object-form 子 spec 向父 spec 投影后形成父 requirement 方法值；
- 默认 spec 值形成公开方法值；
- 本地与 imported spec、直接关系与可见 fit relation；
- 普通闭合泛型实例、共享泛型函数、泛型 owner 方法和静态方法；
- 参数或返回值包含标量、托管引用、tuple、`@value type`、object-form spec 和
  callable-form spec 的 ABI 代表组合；
- 静态 spec 方法值在不同闭合类型参数下进入各自 witness 实现；
- `seal` 方法值合法形成后跨函数传递并调用；
- 生成 C 中每个动态方法值形成点只有一次 closure 分配，实例 receiver 不出现第二个
  box/capture cell，调用体不重复读取 witness slot；
- 全量 `make test` 回归。

测试不做所有类型与所有入口的笛卡尔积。Semantic 测试关注目标选择、sidecar 和诊断；
Codegen 测试只锁定 closure、witness slot、描述符和 ABI 结构；用户可观察的动态分派、
传递、状态与生命周期行为优先放入 FCTS。

## 12 实施 TODO

- [ ] Review 并确认第 2 节全部设计决策。
- [ ] 更新 `feng-function.md`、`feng-spec.md`、`feng-visibility.md` 及相应诊断规范，
  将正式语言语义收敛到权威规范。
- [ ] 增加 object-form spec 实例/静态方法值的统一 Semantic 候选解析。
- [ ] 增加独立 spec method value callable source 和稳定 sidecar 记录。
- [ ] 区分 spec `METHOD_CALL` 与 `METHOD_VALUE` 成员访问用途。
- [ ] 接入公开/`seal` 方法形成点访问检查、重载匹配和显式泛型闭合。
- [ ] 实现一次分配的 spec method callable closure、typed witness slot 捕获和 ABI adapter。
- [ ] 接入父 spec witness 投影、共享泛型 reified dependency 和静态方法值。
- [ ] 验证 imported `.ft` 恢复，不修改格式、版本和 relation 模型；如无法复用则停止并
  提交 Review。
- [ ] 补齐 Semantic、Codegen、跨包、生命周期、LSP 和 FCTS 测试。
- [ ] 执行 `make test` 全量回归并记录结果。

## 13 完成标准

只有同时满足以下条件，本开发项才可标记完成：

1. 权威规范已经定义实例/静态 spec 方法值、target typing、receiver 绑定和 `seal`
   capability 语义；
2. 本地与跨包、公开与合法 `seal`、普通与泛型、实例与静态路径均有有效证据；
3. 方法值动态分派到形成点 witness 对应实现，receiver 生命周期和修改语义正确；
4. 每个动态方法值只产生既定的一次 closure 分配，不增加 receiver box/capture cell；
5. 直接 spec 调用、具体 type/fit 方法值、lambda 和无关泛型路径没有新增运行时成本；
6. `.ft`、runtime 私有 ABI 和 `@mixable` 保持本文边界；
7. `make test` 全量通过。
