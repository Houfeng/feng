# Feng 成员方法值补齐开发草案

> **状态**：待 Review，尚未实施。
>
> **性质**：独立 engineering 开发草案，不是语言权威规范。
>
> 本文统一规划尚未支持的成员方法值：通过 object-form / intersection-form `spec`
> 参数值或受约束泛型值形成实例方法值，通过受约束类型参数形成静态方法值，以及
> 通过具体 `type` / 可见 `fit` 视角形成静态方法值。这些入口属于同一个“对既有合法
> 成员解析结果形成 callable value”的能力，不再拆分为平行专项。
> 本文不修改 [`spec seal` 成员草案](./feng-spec-seal-member-draft.md)，不修改
> `@mixable` 语义。Review 通过后，正式语言行为必须收敛到
> [Feng 函数规范](../specifications/feng-function.md)、
> [Feng `spec` 规范](../specifications/feng-spec.md) 和
> [Feng `type` 规范](../specifications/feng-type.md)、
> [Feng `fit` 规范](../specifications/feng-fit.md) 及
> [Feng 可见性规范](../specifications/feng-visibility.md)，本文只保留实现范围、
> 实施顺序和验收记录。

## 1 背景

Feng 已支持：

- 通过 object-form `spec` 值调用实例方法，并经 witness 分派到实际实现；
- 通过 intersection-form `spec` 值及受其约束的泛型值调用实例方法，并经 merged
  witness 分派到实际实现；
- 通过受 object-form `spec` 约束的类型参数调用静态方法；
- 具体 `type` 实例方法、可见 `fit` 实例方法和显式闭合的泛型实例方法形成方法值；
- 顶层函数、方法值和 lambda 进入 callable-form `spec`；
- callable value 的保存、参数传递、返回、复制、调用和生命周期管理。

### 1.1 当前调用与方法值基线

本表只核对按既有成员访问规范本来合法的来源；实例成员与静态成员之间的访问边界不在
本专项重新定义。最小探针和现有回归确认当前基线如下：

| 来源 | 直接调用现状 | 方法值现状 | 本专项定位 |
| --- | --- | --- | --- |
| object-form `spec` 参数/局部值的实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE` |
| `T: ObjectSpec` 的泛型值实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，receiver 保持 `T` 的值语义 |
| 具体 `type` / 可见 `fit` 的静态方法 | 已支持 | `AE0522` | `CONCRETE_STATIC`；包含 friend 修复 P01 的 `Vault.readShared` |
| `T: ObjectSpec` 的类型参数静态方法 | 已支持 | `AE0522` | `SPEC_STATIC` |
| intersection-form `spec` 参数/局部值的实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，使用 merged witness |
| `T: IntersectionSpec` 的泛型值实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，receiver 保持 `T` 的值语义并使用 merged witness |
| `T: IntersectionSpec` 的类型参数静态方法 | `AE0512` | 尚未进入方法值解析 | 直接调用已有独立基线缺口；对应方法值依赖其先修复 |

前六行的成员直接调用均已闭环，当前缺失只发生在把同一合法成员引用形成 callable
value 时。最后一行不是方法值根因：当前 `T.staticMethod()` 已在直接调用解析阶段报告
`AE0512`，因此不能把对应 `T.staticMethod` 的失败记作方法值缺口。是否在本专项中把该
直接调用问题作为前置修复，必须由第 2 节 Review 决定。

方法值层面的缺口可以按运行时绑定方式归为两类。第一类是 spec-backed 方法只能立即
调用，不能通过 spec 参数值、受约束泛型值或受约束类型参数形成方法值。例如
object-form `spec` 参数值：

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Reader(offset: int): string;

func bind(value: Readable): Reader {
  return value.read; // 当前不支持
}
```

第二类是具体 `type` / 可见 `fit` 的静态方法可以立即调用，但不能作为 callable value
传递：

```feng
open spec IntMapper(value: int): int;

type Math {
  static func double(value: int): int {
    return value * 2;
  }
}

let mapper: IntMapper = Math.double; // 当前不支持
```

验证已经确认，无约束非泛型 `Plain.map`、显式闭合泛型
`Plain.genericMap<string>` 和泛型 owner `Generic<int>.map` 均会在方法值形成阶段被拒绝；
这是具体静态方法形成 callable value 的统一能力缺口，不是某个泛型修复的回归。
同一最小矩阵也确认 object-form / intersection-form 参数值、受约束泛型值以及
object-form `spec` 约束类型参数的合法方法引用均在形成方法值时报告 `AE0522`。

现有 Semantic 已能解析具体 type/fit 的静态方法直接调用，也能为具体实例方法值完成
目标 callable 匹配；现有 Codegen 则分别具备静态方法直接调用和实例方法值绑定能力。
本专项应统一“成员引用形成 callable value”的语义计划，在同一个模型下表达：

- 已有的具体实例方法值；
- 新增的具体 type/fit 静态方法值；
- 新增的 object-form / intersection-form spec 参数值实例方法值；
- 新增的受 object-form / intersection-form spec 约束泛型值实例方法值；
- 新增的受 object-form spec 约束类型参数静态方法值；
- 在直接调用基线修复后，新增的受 intersection-form spec 约束类型参数静态方法值。

不同来源仍使用各自合适的绑定方式：具体静态方法绑定已解析函数身份；显式 spec 值
绑定形成点的 subject/witness 方法槽；受约束泛型值必须按 `T` 的既有值语义捕获
receiver，并绑定 descriptor 中的 witness 方法槽。不得因为统一语言能力而强行统一
运行时表示，也不得把受约束泛型值装箱成 object-form spec 值。

此前两个已完成专项均明确把该能力排除在各自范围之外：

- [`feng-callable-value-reification-refactor-dev.md`](./feng-callable-value-reification-refactor-dev.md)
  只重构已经合法的 callable value 具化，不增加 object-form spec 方法值分派；
- [`feng-value-type-method-value-capture-dev.md`](./feng-value-type-method-value-capture-dev.md)
  只处理具体值接收者的方法值，不把值接收者改写为 spec box。

object-form spec requirement 自身声明方法级泛参当前由
[方法级泛参暂不支持备注](./feng-object-form-spec-method-generic-restriction-note.md)
明确禁止。本文当前只设计非方法泛型 spec requirement 的方法值，以及既有合法的
type/fit 泛型方法在显式闭合后形成方法值；未来若恢复 spec 方法级泛参，相关方法值
能力再以
[未来支持分析](./feng-object-form-spec-generic-method-bugfix.md) 为前置条件纳入。

本文作为一个独立开发项补齐上述成员方法值能力。文件名为避免已有引用失效暂时保留，
其范围以本文标题和正文为准。

## 2 Review 决策项

以下内容是本文建议方案，实施前需要 Review 确认：

1. **继续采用目标类型驱动**：`value.method` 或 `Type.method` 只有在绑定、参数、返回
   或显式转换位置已存在 callable-form `spec` 目标时合法；不新增
   `let method = value.method` 的自然 callable 类型推导。
2. **权限在形成点检查**：spec 方法复用 `spec seal` 访问判断；具体 type/fit 静态方法
   复用现有成员可见性、fit 可见面及 `@friend` / `@mixable` 等既有授权查询。合法形成
   后的 callable 可以作为普通值传递，调用点不重复检查来源成员的访问权限。
3. **绑定一次、调用目标固定**：实例 receiver 在形成点只求值一次；显式 spec 值
   closure 保存该次得到的 subject 和 witness 方法槽，受约束泛型值 closure 按 `T` 的
   descriptor 保存 receiver payload 和 witness 方法槽。静态方法值在形成点固定已解析
   具体方法或当前类型参数 witness 方法槽。后续环境变化不重新执行成员选择。
4. **按来源使用最小表示**：实例 spec 方法值只形成一个 callable closure，不增加独立
   receiver box 或通用 lambda capture cell；受约束泛型 receiver 使用 closure 内联
   payload，不装箱为 spec 值；受约束类型参数的静态 spec 方法值不捕获 subject，只保存
   所需 witness 方法槽；完全闭合的具体 type/fit 静态方法没有动态捕获，应复用现有无
   捕获 callable value/descriptor 路径，不增加每次形成时的堆分配。
5. **静态方法值并入同一专项**：具体 type 静态方法、可见 fit 静态方法和受 object-form
   spec 约束类型参数的静态方法值，与 object-form / intersection-form spec 实例方法值
   统一设计、实施和验收，不再拆为后续独立能力。
6. **不增加 runtime ABI 和 `.ft` 格式**：复用现有 callable closure、witness、泛型
   descriptor 和 reified callable dependency 抽象；如实施中证明必须改变 runtime
   私有 ABI、增加动态查找或增加额外分配，应停止并提交人工决策。
7. **intersection 静态调用前置项**：`T: IntersectionSpec` 的 `T.staticMethod()` 当前
   报告 `AE0512`。Review 必须决定是先在本交付中修复该直接调用基线，再补齐对应方法值，
   还是暂缓 intersection 静态方法值；不得在方法值 lowering 中绕过直接调用解析器增加
   特判。

## 3 目标与非目标

### 3.1 目标

- 允许 object-form / intersection-form `spec` 参数值的实例方法通过 `value.method`
  形成 callable value；
- 允许受 object-form / intersection-form `spec` 约束的泛型值通过 `value.method`
  形成 callable value，并保持 receiver 的既有值语义；
- 允许受 object-form `spec` 约束的类型参数通过 `T.method` 形成静态 callable value；
- 在第 2 节决定并完成直接调用前置修复后，允许受 intersection-form `spec` 约束的类型
  参数通过 `T.method` 形成静态 callable value；
- 允许具体 `type` 和可见 `fit` 的静态方法通过 `Type.method` 形成 callable value；
- 公开方法和合法可访问的 `seal` 方法分别遵守现有 spec/type/fit 成员访问域；
- 重载方法由目标 callable-form `spec` 唯一选择；
- 对既有合法的 type/fit 泛型方法，必须显式提供完整类型实参后再形成方法值；
- receiver、subject、witness、泛型描述符和 callable closure 生命周期正确；
- 本包、跨包 `.ft`、普通闭合代码和共享泛型体行为一致；
- 除第 2 节可能决定纳入的 intersection 静态调用前置修复外，不改变直接 spec/具体
  静态方法调用和既有具体 type/fit 实例方法值的行为或开销。

### 3.2 非目标

- 不增加匿名 callable 类型或 `let value = object.method` 自然类型推导；
- 不改变 callable-form `spec` 的结构匹配、variance、转换或重载优先级；
- 不支持形成后仍保留未闭合方法级泛参的 first-class polymorphic callable；
- 不修改 `type seal`、`fit` 私有访问权或 `spec seal` 的既有访问域；
- 不修改 `@mixable` 来源传播、wrapper 或具体 type 互访规则；
- 不增加运行时可见性检查、按类型名搜索、tag 分派或动态 descriptor 工厂；
- 不以单态化共享泛型体作为正确性前提；
- 不处理 object-form spec 普通成员访问的重复表达式提升；该性能方向继续由
  [成员访问优化备忘](./feng-object-form-spec-access-codegen-optimize-dev.md) 独立跟踪；
- 不借机重构无关的 lambda 捕获、既有具体实例方法值或 callable ABI。

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

受 spec 约束的泛型值也允许形成同一实例 requirement 的方法值：

```feng
func bindGeneric<T: Readable>(value: T): Reader {
  return value.read;
}
```

这里 `value` 仍是 `T` 的值，不是一等 object-form spec 值。方法值必须复用既有泛型值
捕获能力，按闭合 descriptor 的值模型复制或保留 receiver，并从 `T` 的约束 witness
保存选中方法槽；不得为了复用 `{ subject, witness }` 路径把 `T` 装箱。`SPEC_INSTANCE`
描述的是 witness 分派类别，不代表所有 receiver 都使用同一种捕获布局。

intersection-form 的实例方法值使用相同规则。显式 intersection-form 参数值保存当前
subject 与 merged witness；受 intersection-form 约束的泛型值保持 `T` 的值语义，并从
merged witness 保存选中成员槽。二者都归入 `SPEC_INSTANCE`，不新增 intersection 专用
绑定类别：

```feng
open spec Traceable {
  func trace(): string;
}

open spec ReadableTraceable: Readable & Traceable;

func bindIntersection(value: ReadableTraceable): Reader {
  return value.read;
}

func bindGenericIntersection<T: ReadableTraceable>(value: T): Reader {
  return value.read;
}
```

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

### 4.3 泛型来源方法

既有合法的 type/fit 泛型方法形成方法值时，必须显式提供完整类型实参。object-form
spec 方法自己声明方法级泛参当前不合法，因此下例只保留为未来恢复该语言能力后的
目标，不属于本文当前可实施范围：

```feng
open spec Transformer {
  func transform<T>(value: T): T;
}

open spec IntTransform(value: int): int;

func bind(value: Transformer): IntTransform {
  return value.transform<int>;
}
```

对当前合法的 type/fit 来源，目标 callable-form `spec` 不反向推导来源方法泛参。
Semantic 先检查显式实参数量和约束，闭合来源签名，再执行普通 target-typed
callable 匹配。形成后的值不再携带可由调用者选择的方法级泛参，调用时只写
`callable(args...)`。未来的 object-form spec 泛型方法值也必须遵守同一闭合边界，
但需先完成其动态 witness descriptor 路由能力。

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

### 4.5 静态成员方法值

object-form spec 约束类型参数的静态方法值使用已有合法的类型参数成员引用：

```feng
open spec Factory {
  static func create(): Result;
}

open spec Creator(): Result;

func bind<T: Factory>(): Creator {
  return T.create;
}
```

spec 约束视角规则如下：

- `T.create` 在形成点从 `T` 的 descriptor/witness 选择静态方法槽；
- 静态方法值不捕获 subject；
- `seal static` 方法使用与直接静态 spec 方法访问相同的实现上下文判断；
- 具体类型名上的 `Concrete.create` 仍属于 type/fit 视角，不改写为 spec 视角。

intersection-form 约束类型参数的静态路径当前不能直接套用上述规则。实测
`T: IntersectionSpec` 下的 `T.staticMethod()` 在直接调用阶段报告 `AE0512`，说明现有
静态 requirement 查找尚未进入 intersection merged surface；对应 `T.staticMethod`
方法值也因此尚不可到达。若 Review 决定补齐该前置能力，直接调用与方法值必须复用同一
intersection 静态成员解析结果，方法值继续归入 `SPEC_STATIC`，不得增加 intersection
专用 callable 绑定类别。

具体 type/fit 静态方法值使用同一目标类型驱动规则：

```feng
open spec IntMapper(value: int): int;

type Math {
  static func double(value: int): int {
    return value * 2;
  }
}

type ExtendedMath {}

fit ExtendedMath {
  static func triple(value: int): int {
    return value * 3;
  }
}

let double: IntMapper = Math.double;
let triple: IntMapper = ExtendedMath.triple;
```

规则如下：

- `Type.method` 先按现有具体静态成员规则，在 type 自有静态方法和当前位置可见的 fit
  静态方法中完成候选枚举、访问过滤和唯一选择；
- 方法值形成不会改变 fit 的 module/import 可见面，也不会让不可见 fit 参与候选；
- 普通 `seal`、`@friend`、`@mixable seal` 等成员访问继续复用现有授权判断，只把当前
  位置原本能够直接调用的方法作为方法值候选；
- 具体静态方法不捕获 receiver、subject 或 witness；完全闭合后固定已解析的 type/fit
  方法身份；
- 泛型 owner 与泛型静态方法分别使用现有 owner 实参和显式方法实参完成闭合；方法级
  泛参继续遵守第 4.3 节“形成方法值时必须显式闭合”的统一规则；
- 形成后的 callable 调用不重新执行 type/fit 成员查找或访问检查。

[`feng-friend-type-implementation-context-bugfix.md`](./feng-friend-type-implementation-context-bugfix.md)
P01 中的 `Vault.readShared` 正是本节的具体 type 静态方法值，并额外组合了 `@friend`
访问域以及字段初始化、构造函数、终结器形成点。该语言能力由本文统一实现；完成后必须
恢复 P01 当时移除的方法值覆盖，不为 friend 场景增加专用路径。

## 5 Semantic 设计

### 5.1 复用现有成员闭包与访问过滤

Semantic 应建立统一的成员方法值解析入口。该入口先根据来源表达式区分显式 spec
receiver、受 spec 约束的泛型 receiver、受 spec 约束的类型参数静态 target 或具体
type target，再复用对应的现有成员面：

1. object-form / intersection-form spec receiver 从现有完整成员闭包枚举同名实例
   requirement；intersection 使用已经展平、去重的 merged surface；
2. 受 object-form / intersection-form spec 约束的泛型 receiver 从同一约束 surface
   枚举同名实例 requirement，同时保留 receiver 的完整 `T` 类型和 descriptor 事实；
3. 受 object-form spec 约束的类型参数从同一闭包枚举同名 static requirement；
   intersection 静态路径只有在第 2 节前置项完成后才能进入本步骤；
4. 具体 type target 复用现有 type 自有静态方法与可见 fit 静态方法候选枚举；
5. 分别使用现有 spec seal 或 type/fit 成员访问查询过滤当前不可访问候选；
6. 使用目标 callable-form `spec` 的已实例化签名匹配候选；
7. 对显式方法类型实参检查数量与约束，并在 owner 实例之后关闭方法签名；
8. 产生唯一、无匹配或歧义结论；
9. 只在唯一成功后记录稳定的成员方法值语义事实。

直接方法调用和方法值必须复用各自已有的成员闭包、owner/fit 实例替换、可见性过滤与
callable 签名比较基础设施。不得为方法值复制 spec 父成员展开、type/fit 静态成员枚举、
fit 可见面、intersection surface 合并或 `seal` 授权算法。intersection 静态直接调用的
`AE0512` 前置问题也必须在通用静态成员解析中解决，不得只让方法值路径额外识别该成员。

### 5.2 Callable source 分类

现有 `FengSpecCoercionCallableSource` 已有统一 `METHOD_VALUE` 来源，但其稳定记录和
Codegen 当前只表达带具体 receiver 的实例方法。合并后的设计不应为每种来源复制一套
callable coercion；应在 `METHOD_VALUE` 下增加明确的成员绑定类别：

```c
typedef enum FengCallableMethodValueBindingKind {
    FENG_CALLABLE_METHOD_VALUE_CONCRETE_INSTANCE,
    FENG_CALLABLE_METHOD_VALUE_CONCRETE_STATIC,
    FENG_CALLABLE_METHOD_VALUE_SPEC_INSTANCE,
    FENG_CALLABLE_METHOD_VALUE_SPEC_STATIC
} FengCallableMethodValueBindingKind;
```

现有具体实例方法值归入 `CONCRETE_INSTANCE`，行为和表示保持不变。其余三类作为同一
成员方法值抽象的新绑定形态接入。Codegen 必须消费该稳定类别，不得再以“是否存在
receiver”或表达式语法形状猜测来源。

成功站点至少要稳定记录：

- 成员绑定类别；
- 具体 owner type、选中的 type/fit 方法及 fit 声明身份，或 source object-form /
  intersection-form spec 的声明和完整实例类型；
- spec 来源选中的 requirement 及其原声明 spec，用于父 spec 和 `seal` 语义；
- 实例来源的 receiver 表达式及其实际静态类型；受约束泛型 receiver 还必须记录其
  descriptor/value-model 来源，静态来源不伪造 receiver；
- 目标 callable-form spec 的声明和完整实例类型；
- 已显式闭合的方法级类型实参；
- owner/fit 实例替换后的来源签名。

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

当来源 owner/spec、receiver 类型、目标 callable-form spec、方法级实参或静态约束类型
仍包含活动泛参时，provider 必须把成员方法值形成需求写入现有 reified callable
dependency 固定 slot。
consumer 在闭合点生成：

- 闭合 callable adapter；
- 闭合 closure descriptor；
- 参数与返回值 ABI bridge 所需的类型描述符；
- 受约束泛型 receiver 的复制、保留、释放和捕获 payload 布局所需的闭合类型描述符；
- 具体 type/fit 静态方法值所需的闭合方法身份；
- spec 静态方法值所需的实际 witness 方法槽来源。

共享体只按编译期分配的固定 slot 读取描述符，不执行运行时名称搜索、哈希查找、tag
分派或 descriptor 构造。应扩展现有 resolved callable/source 抽象表达上述成员绑定
类别，不得按具体 spec、type、fit、方法名或参数位置增加特判。

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

intersection-form spec 值使用等价的 subject + merged witness 分发模型。受 spec 约束
的直接泛参 `T` 则不是上述 spec 值：值本体继续按 `FengGenericParamDescriptor` 的
`size/kind/aggregate` 表示，约束能力才从 descriptor 的 witness 取得。该既有差异必须
延续到方法值 receiver 捕获。

现有具体方法值路径生成 callable closure，保存具体 receiver，并由 typed invoke
adapter 直接调用已解析的 type/fit 实现。该路径明确要求 receiver 是 concrete object，
不能承载运行时才由 spec witness 决定的实现方法。

具体 type/fit 静态方法的直接调用已经具有稳定的 resolved callable、owner/fit 身份、
泛型实参和直接调用发码；当前缺少的是把同一已解析静态方法具化为 callable value 的
入口。该路径不需要 receiver 或 witness，不应绕到 spec closure 实现。

### 6.2 按绑定类别选择表示

spec 实例方法和受约束类型参数的 spec 静态方法需要在形成点保存动态方法槽。建议为
每个闭合的“source spec method + target callable-form spec ABI”组合生成
codegen-private closure。显式 object-form / intersection-form spec 值 receiver 的概念
布局如下：

```c
struct BoundSpecMethod {
    FengManagedHeader _hdr;
    void *_self;                 // 实例方法：subject；静态方法：NULL
    TargetCallableInvoke invoke;
    SourceSpecMethodFn target;   // 形成点从 witness 读取的 typed 方法槽
};
```

从显式 spec 值形成实例方法值时：

1. 物化 receiver，保证 receiver 表达式只求值一次；
2. 分配一个 closure；
3. 用现有托管赋值保存 `receiver.subject`；
4. 从正确的 receiver witness 视角读取选中 requirement 的 typed 方法槽；
5. 保存生成的 target callable invoke adapter；
6. 返回普通 callable-form spec 值。

从受 object-form / intersection-form spec 约束的泛型值 `T` 形成实例方法值时，不能
套用固定 `_self` 指针布局：

1. 物化 receiver，保证表达式只求值一次；
2. 使用闭合 `T` descriptor 在同一个 method-value closure 的内联 payload 中复制或
   保留 receiver；
3. 从 `T` descriptor 的 object-form witness 或 intersection merged witness 读取选中
   requirement 的 typed 方法槽；
4. invoke adapter 按闭合 `T` 的既有 value/address ABI 把内联 payload 作为 receiver
   传给该方法槽；
5. closure cleanup 复用既有 generic value capture/aggregate cleanup，恰好释放一次
   receiver 的托管叶子；
6. 不创建 spec box、第二个 receiver box 或通用 lambda capture cell。

两种实例来源均归入 `SPEC_INSTANCE`；具体 closure payload 由 receiver 值模型决定，
不能用绑定类别代替布局分类。intersection 只改变 witness surface，不改变 closure
类别、receiver 值语义或 callable ABI。

形成 spec 静态方法值时：

1. 从约束类型参数 descriptor/witness 读取选中的静态方法槽；
2. `_self` 保持 `NULL`；
3. 保存 typed 方法槽和 invoke adapter；
4. 返回普通 callable-form spec 值。

显式 spec 值 closure descriptor 只把 `_self` 标记为托管槽；泛型 receiver closure
descriptor 按闭合 receiver descriptor 描述内联 payload 的复制和清理；`target` 始终是
非托管函数指针。不得保存栈地址，不得把 witness 表或函数指针作为托管对象处理。

形成具体 type/fit 静态方法值时：

1. 直接消费 Semantic 已选定的具体方法、owner 实例、fit 身份及显式方法类型实参；
2. 复用现有顶层函数值/具名 callable dependency 的闭合 typed adapter、closure
   descriptor 和静态 callable value 机制；
3. 完全闭合的具体静态方法没有动态捕获，不在每次形成时分配 closure；
4. 共享泛型体只从固定 reified callable dependency slot 取得 consumer 已生成的闭合
   callable descriptor/static value；
5. 不在 Codegen 重新枚举 type/fit 成员，也不把具体静态方法包装为伪 receiver 或
   spec witness。

现有具体实例方法值继续使用既有 receiver closure，不因新增绑定类别改变布局、分配或
调用路径。

### 6.3 Invoke adapter

实例方法 adapter 的概念调用为：

```c
return closure->target(closure->_self, args...);
```

受约束泛型 receiver 的 adapter 不固定读取 `_self`，而是按闭合 descriptor 的 ABI 将
closure 内联 payload 的值或地址传给 `target`。不得先把 payload 转换成 spec box。

spec 静态方法 adapter 的概念调用为：

```c
return closure->target(args...);
```

具体 type/fit 静态方法 adapter 直接调用 Semantic 已解析的闭合方法入口；泛型 shared
body 和 closed wrapper 的选择、function descriptor 参数及 value/address ABI 分类必须
复用现有直接静态调用与 reified callable dependency 规则，不建立方法值专用 ABI。

adapter 必须复用现有 callable ABI bridge：

- value ABI 与 address ABI 参数；
- value ABI 与 address ABI 返回值；
- descriptor-sized 泛型值；
- 变长参数的既有预打包数组传递；
- callable-form spec 目标的闭合参数和返回类型。

形成点保存精确 witness 方法槽或具体静态方法身份后，调用点不得再次遍历 witness、按名
称查找、枚举 fit 或判断实际 subject 类型。每次调用不分配对象。

### 6.4 父 spec 与向上转换

如果方法由父 spec 声明，Semantic 必须记录 requirement 原声明和形成点使用的完整
spec 视角。Codegen 复用现有 object-form spec 向上转换和父 witness 投影，取得与直接
方法调用相同的 witness view 后再读取方法槽。

intersection-form 来源必须先复用直接实例调用的展平、去重和 merged witness 槽映射，
再按 requirement 原声明读取方法槽。受 intersection-form 约束的泛型 receiver 使用同一
merged witness，但 receiver 仍按 `T` 的闭合 descriptor 捕获。未来修复 intersection
静态直接调用时，也必须先产出同一稳定槽映射，方法值只消费该结果。

不得假定父 requirement 在子 witness 中具有相同字段偏移，也不得仅按方法名从最外层
witness 猜测目标槽。

## 7 生命周期与值语义

- 引用类型 subject：closure retain 同一对象引用；调用作用于同一实例。
- 已装箱的值类型 subject：closure retain 现有 spec box；不重新复制或二次装箱。
- 默认 object-form spec 值：closure retain 默认 subject，并保存对应默认 witness 槽。
- intersection-form spec 值：closure retain 同一 subject，并保存形成点 merged witness
  中的选中方法槽，不拆成多个 object-form spec 值。
- 受 spec 约束的泛型 receiver：closure 按闭合 `T` descriptor 复制或保留完整值，最后
  通过同一 descriptor/aggregate cleanup 清理；不得装箱成 spec subject。
- 显式 spec receiver 临时值：形成方法值前必须物化为具有稳定生命周期的 spec
  subject；受约束泛型 receiver 临时值必须立即复制到 closure 内联 payload；两者均不得
  借用会在形成表达式后失效的栈地址。
- callable 复制：继续使用现有 callable 托管引用语义，多个绑定共享同一个 closure。
- callable 覆盖、正常离开作用域和异常展开：最后一个引用释放时恰好释放一次 subject。
- spec 静态方法值：没有 subject retain/release，只管理用于保存 witness 方法槽的 callable
  closure 自身。
- 具体 type/fit 静态方法值：没有 subject/witness 生命周期；完全闭合来源复用静态
  callable value，共享泛型体复用 descriptor 固定 slot。

## 8 跨包与 `.ft`

本能力预计不增加 `.ft` 格式字段：

- object-form spec method requirement、可见性、泛型签名和父 spec 关系，以及
  intersection-form 的展平成员关系已经进入符号表；
- package-public type/fit 静态方法的 owner、成员签名、fit 来源和泛型 callable 依赖已经
  由现有直接调用/具名 callable 机制恢复；
- callable-form spec 目标签名已经可跨包恢复；
- consumer 可以根据 imported spec method 语义记录和 witness 布局生成本地 adapter；
- spec 具体实现成员仍由 witness 封装，consumer 不需要发现或直接访问实际 type/fit
  实现；具体 type/fit 静态方法值则复用已导出方法的现有 package callable 身份。

实施时必须用跨包 provider/consumer 验证上述事实。如果现有 `.ft` 无法稳定恢复
requirement 原声明、完整泛型实例或 witness slot 身份，应先完善通用符号事实；不得按
包名、类型名或方法名增加恢复特判，也不得未经 Review 修改 `.ft` 格式。普通 seal 或
未进入既有 package callable surface 的方法不能因“形成方法值”而获得新的跨包导出资格。

## 9 诊断与工具链

至少需要区分以下失败：

- 成员方法值没有 callable-form spec 目标；
- 存在同名方法，但没有签名匹配目标 callable；
- 多个重载同时匹配目标 callable；
- `spec seal` 或具体 type/fit seal 方法在非法上下文形成方法值；
- 当前合法的 type/fit 泛型方法未显式提供完整类型实参、实参数量错误或约束不满足；
- 具体 type target 上只有不可见 fit 或不可访问静态候选；
- callable 字段读取与同名方法值候选发生现有规则无法消解的冲突。

`T: IntersectionSpec` 下静态直接调用当前产生的 `AE0512` 属于第 1.1 节调用基线缺口，
在该前置项修复前不得用新的“方法值不匹配”诊断掩盖它。

诊断码和最终消息在权威规范阶段确定，不在本文预先占用编号。

LSP、hover 和 completion 继续显示 spec/type/fit 方法声明本身。具有目标类型的
`value.method` / `Type.method` 表达式应返回目标 callable-form spec 类型；非法 seal
方法和不可见 fit 不应作为当前位置可形成的方法值候选。不得改变普通方法调用补全和
具体 type 成员可见性。

## 10 性能约束

显式 object-form / intersection-form spec 值实例方法值的目标成本：

| 阶段 | 成本 |
| --- | --- |
| 形成 | 一次 receiver 求值、一次 closure 分配、一次 subject retain、一次 witness slot 读取 |
| 复制 | 现有 callable 引用复制 |
| 调用 | 一次 callable adapter 调用、一次已保存方法函数指针间接调用 |
| 释放 | closure 最后释放时一次 subject release |

受 spec 约束泛型值的实例方法值仍只有一次 method-value closure 分配。形成时按闭合
`T` descriptor 执行既定的 receiver 值复制/retain，并读取一次 witness 方法槽；调用时
使用一次 callable adapter 和已保存的方法函数指针；释放时按同一 descriptor 清理内联
payload。不得增加 spec box 或第二次 receiver 分配。intersection merged witness 不应
增加形成次数、分配次数或每次调用的间接层数。

受约束类型参数的静态 spec 方法值不执行 subject retain/release；形成时至多保存一次
witness 方法槽，调用时不再读取 witness。

完全闭合的具体 type/fit 静态方法值目标成本不得高于既有顶层函数值：形成时复用静态
callable value，不分配动态 closure；调用执行既有 callable adapter 后直接进入已解析
静态方法。共享泛型体只增加既有固定 descriptor slot 读取，不增加运行时成员查找。

实现不得：

- 为实例 receiver 增加第二个 box 或 capture cell；
- 把受约束泛型 receiver 装箱为 object-form spec 值；
- 每次 callable 调用重新读取 witness slot；
- 每次 callable 调用分配临时对象；
- 增加运行时可见性检查；
- 为既有具体实例方法值、直接 spec 调用或直接具体静态调用增加新分支或额外间接层；
- 在 intersection 静态调用前置修复中增加运行时成员搜索、额外 witness 遍历或调用层；
- 为完全闭合的具体 type/fit 静态方法值增加不必要的动态 closure 分配。

如果正确实现必须增加上述成本、修改 runtime 私有 ABI 或引入新的动态查找，应停止
实施并提交人工决策。

## 11 测试矩阵

### 11.1 Parser 与 AST

本能力预计不修改语法和 AST。Parser 回归只需确认现有以下表达式继续稳定：

- `value.method`；
- `value.method<T>`；
- `T.staticMethod`；
- `Concrete.staticMethod` 与 `Concrete.staticMethod<T>`；
- `(CallableSpec)value.method`。

如果无需 Parser 变更，不增加重复语法测试。

### 11.2 Semantic

| 场景 | 预期 |
| --- | --- |
| object-form spec 参数/局部值的公开实例方法进入显式 callable 绑定 | 通过 |
| object-form spec 参数/局部值的公开实例方法直接作为参数或返回值 | 通过 |
| `T: ObjectSpec` 的泛型值实例方法形成方法值 | 通过，保留 `T` 的 receiver 值语义 |
| intersection-form spec 参数/局部值的实例方法形成方法值 | 通过，使用 merged witness |
| `T: IntersectionSpec` 的泛型值实例方法形成方法值 | 通过，保留 `T` 的值语义并使用 merged witness |
| 显式 callable-form spec 转换承接 spec 方法值 | 通过 |
| 无 callable 目标的 `let value = spec.method` | 拒绝 |
| 重载由目标 callable 唯一选择 | 通过 |
| 无匹配或多个匹配 | 分别报告不匹配或歧义 |
| object-form spec 方法自己声明方法级泛参 | 当前由 spec 声明检查前置拒绝，不进入方法值解析 |
| 既有合法的 type/fit 泛型方法值未闭合或约束不满足 | 拒绝 |
| 合法实现上下文形成 `spec seal` 方法值 | 通过 |
| 普通函数或无关 type 形成 `spec seal` 方法值 | 拒绝 |
| callable 字段读取 | 保持现有字段语义，不误判为方法值 |
| 子 spec 视角形成父 spec 方法值 | 通过并记录正确原声明 |
| 受 object-form spec 约束类型参数形成公开/合法 seal 静态方法值 | 通过 |
| 受 intersection-form spec 约束类型参数直接调用静态方法 | 当前基线为 `AE0512`；按第 2 节 Review 结果先修复或延期 |
| 受 intersection-form spec 约束类型参数形成静态方法值 | 只有直接调用前置项完成后才应通过 |
| 具体 type 自有公开静态方法形成方法值 | 通过 |
| 当前位置可见 fit 静态方法形成方法值 | 通过并记录 fit 身份 |
| 不可见 fit、不可访问 seal 静态方法形成方法值 | 拒绝 |
| 具体泛型 owner 静态方法与显式闭合泛型静态方法形成方法值 | 通过 |
| P01 的 `Vault.readShared` 在 friend 字段初始化、构造函数和终结器形成方法值 | 通过，复用具体静态方法值与既有 friend 授权 |

### 11.3 Codegen 与 FCTS

- 两个不同 type 实现同一 spec，分别形成方法值并调用各自实现；
- receiver 表达式只求值一次；
- receiver 局部变量重新赋值后，已形成方法值仍绑定原 subject；
- 方法值从创建函数返回后继续有效；
- 方法值保存到字段、复制、覆盖和释放；
- 修改型方法通过方法值修改同一个引用 subject 或现有 spec box；
- 受约束泛型 receiver 分别闭合为托管引用、trivial 值和 descriptor-sized 值时，方法值
  按闭合 `T` 值语义捕获，不生成 spec box；
- intersection-form 参数值和受约束泛型值分别通过 merged witness 形成实例方法值；
- object-form 子 spec 向父 spec 投影后形成父 requirement 方法值；
- 默认 spec 值形成公开方法值；
- 本地与 imported spec、直接关系与可见 fit relation；
- 普通闭合泛型实例、共享泛型函数、泛型 owner 方法和静态方法；
- 参数或返回值包含标量、托管引用、tuple、`@value type`、object-form spec 和
  callable-form spec 的 ABI 代表组合；
- 静态 spec 方法值在不同闭合类型参数下进入各自 witness 实现；
- 若纳入 intersection 静态调用前置修复，直接调用与方法值必须读取同一 merged witness
  静态槽，且对应本地、共享泛型体和跨包路径均有覆盖；
- 具体 type 与 fit 静态方法值分别调用已选实现，重载、同名 type/fit 候选和 fit 可见面
  与直接调用结果一致；
- 具体静态方法值覆盖普通/泛型 owner、普通/泛型方法、同包和 imported package；
- `seal` 方法值合法形成后跨函数传递并调用；
- 生成 C 中每个动态方法值形成点只有一次 closure 分配，实例 receiver 不出现第二个
  box/capture cell，调用体不重复读取 witness slot；
- 完全闭合的具体静态方法值复用静态 callable value，不产生每次形成的动态分配；
- 全量 `make test` 回归。

测试不做所有类型与所有入口的笛卡尔积。Semantic 测试关注目标选择、sidecar 和诊断；
Codegen 测试只锁定 closure、witness slot、描述符和 ABI 结构；用户可观察的动态分派、
传递、状态与生命周期行为优先放入 FCTS。

## 12 实施 TODO

- [ ] Review 并确认第 2 节全部设计决策。
- [ ] Review 并决定 intersection-form 约束类型参数静态直接调用 `AE0512` 是否作为本专项
  前置修复；若延期，同步明确延期对应静态方法值，不以特判绕过。
- [ ] 更新 `feng-function.md`、`feng-spec.md`、`feng-fit.md`、`feng-visibility.md` 及相应
  诊断规范，将正式语言语义收敛到权威规范。
- [ ] 将现有 `METHOD_VALUE` 收敛为带明确绑定类别的统一成员方法值来源，保留既有具体
  实例方法值行为，并增加稳定 sidecar 记录。
- [ ] 让具体 type/可见 fit 静态方法值复用现有静态调用候选、访问过滤、owner/fit 实例化
  与 target-typed callable 匹配。
- [ ] 增加 object-form / intersection-form spec 参数值及受约束泛型值实例方法值的统一
  Semantic 候选解析；intersection 复用 merged surface，不新增候选算法。
- [ ] 增加受 object-form spec 约束类型参数静态方法值的 Semantic 解析；若纳入
  intersection 静态前置修复，先让直接调用复用 merged static surface，再接入同一结果。
- [ ] 区分 spec `METHOD_CALL` 与 `METHOD_VALUE` 成员访问用途。
- [ ] 接入 spec/type/fit 公开及合法 seal 方法的形成点访问检查、重载匹配和显式泛型闭合。
- [ ] 让完全闭合的具体 type/fit 静态方法值复用无捕获静态 callable value，并让共享泛型
  体复用既有 reified callable dependency；不新增专用 ABI。
- [ ] 实现一次分配的 spec method callable closure、typed witness slot 捕获和 ABI adapter；
  受约束泛型 receiver 使用 descriptor 驱动的内联 payload，不增加 spec box。
- [ ] 接入父 spec witness 投影、共享泛型 reified dependency 和 spec 静态方法值。
- [ ] 验证 imported `.ft` 恢复，不修改格式、版本和 relation 模型；如无法复用则停止并
  提交 Review。
- [ ] 补齐 Semantic、Codegen、跨包、生命周期、LSP 和 FCTS 测试，并恢复 friend 修复 P01
  中字段初始化、构造函数和终结器的 `Vault.readShared` 方法值覆盖。
- [ ] 执行 `make test` 全量回归并记录结果。

## 13 完成标准

只有同时满足以下条件，本开发项才可标记完成：

1. 权威规范已经定义 object-form / intersection-form spec 实例方法值、受约束类型参数
   静态方法值、具体 type/fit 静态方法值、target typing、绑定和 seal capability 语义；
2. 本地与跨包、公开与合法 seal、type 与 fit、普通与泛型、object-form 与
   intersection-form、实例与静态路径均有有效证据；若 intersection 静态路径经 Review
   延期，文档和完成状态必须明确排除，不能宣称已经完整支持；
3. spec 方法值动态分派到形成点 witness 对应实现，具体静态方法值调用形成点已选定的
   type/fit 方法，receiver 生命周期和修改语义正确；
4. 每个动态 spec 方法值只产生既定的一次 closure 分配，不增加 receiver box/capture
   cell；完全闭合的具体静态方法值不产生每次形成的动态分配；
5. 直接 spec 调用、直接具体静态调用、既有具体实例方法值、lambda 和无关泛型路径没有
   新增运行时成本；若修复 intersection 静态直接调用，不增加运行时搜索或额外间接层；
6. `.ft`、runtime 私有 ABI 和 `@mixable` 保持本文边界；
7. `make test` 全量通过。
