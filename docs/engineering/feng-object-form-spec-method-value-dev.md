# Feng 成员方法值缺口与分项交付计划

> **状态**：待 Review，尚未实施。
>
> **性质**：engineering 任务文档，不是语言权威规范。
>
> 本文只记录当前缺口、期望行为、任务依赖、验证范围和交付状态。正式语言行为必须更新
> 到对应权威规范；本文不预先规定 Semantic sidecar、Codegen closure、descriptor、
> `.ft` 或 runtime 的具体实现形态。

## 1 范围与共同边界

本专项只为按既有成员访问规则本来合法的成员引用补齐 callable value 形成能力，不重新
定义实例成员与静态成员的访问边界，也不扩大 `spec seal`、type/fit 可见性、`@friend`
或 `@mixable` 授权。

所有分项共同遵守以下规则：

1. 方法值继续由明确的 callable-form `spec` 目标类型驱动；不增加匿名 callable 类型或
   `let method = value.method` 自然类型推导。
2. 实例 receiver 在方法值形成点求值并绑定一次；后续调用不得重新读取原 receiver
   表达式或重新选择 receiver。
3. `value: T` 形成实例方法值时继续保持 `T` 的既有值语义；不得为了复用 spec 值路径
   把 `T` 装箱成 object-form spec 值。
4. intersection-form 路径复用其既有 merged member/witness 语义，不增加只服务于方法值
   的 intersection 特判。
5. 访问权限在方法值形成点按既有成员访问规则检查；形成后的 callable 作为普通值传递。
6. 方法值解析必须复用对应直接调用的成员面、可见性过滤、泛型代入和重载基础设施；
   不得按 spec/type/fit 名称、方法名、包名或测试模型增加特判。
7. 本文中的 `SPEC_INSTANCE`、`CONCRETE_STATIC`、`SPEC_STATIC` 只是任务级逻辑分类，
   不要求编译器新增同名枚举或采用特定运行时表示。
8. 如果某个分项需要增加既有正确路径的运行时开销、修改 runtime 私有 ABI、扩展 `.ft`
   格式/兼容边界或引入额外动态查找，必须先停止并提交人工决策。

权威语义按分项更新到相关主规范，至少包括：

- [Feng 函数规范](../specifications/feng-function.md)；
- [Feng `spec` 规范](../specifications/feng-spec.md)；
- [Feng 泛型规范](../specifications/feng-generics-draft.md)；
- [Feng `type` 规范](../specifications/feng-type.md)；
- [Feng `fit` 规范](../specifications/feng-fit.md)；
- [Feng 可见性规范](../specifications/feng-visibility.md)；
- 涉及跨包依赖恢复时的 [Feng 符号表规范](../specifications/feng-symbol-table.md)；
- 对应 AE/CE 诊断规范。

## 2 当前基线

本表只核对按既有成员访问规范本来合法的来源。当前最小探针与现有回归确认如下：

| 来源 | 直接调用现状 | 方法值现状 | 本专项定位 |
| --- | --- | --- | --- |
| object-form `spec` 参数/局部值的实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE` |
| `T: ObjectSpec` 的泛型值实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，receiver 保持 `T` 的值语义 |
| 具体 `type` / 可见 `fit` 的静态方法 | 已支持 | `AE0522` | `CONCRETE_STATIC`；包含 friend 修复 P01 的 `Vault.readShared` |
| `T: ObjectSpec` 的类型参数静态方法 | 已支持 | `AE0522` | `SPEC_STATIC` |
| intersection-form `spec` 参数/局部值的实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，使用 merged witness |
| `T: IntersectionSpec` 的泛型值实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，receiver 保持 `T` 的值语义并使用 merged witness |
| `T: IntersectionSpec` 的类型参数静态方法 | `AE0512` | 尚未进入方法值解析 | 直接调用存在独立缺口；对应方法值依赖其先修复 |

前六行当前缺失的是“把已经可以直接调用的合法成员引用形成 callable value”。最后一行
首先是直接调用缺口，不能把它直接归因于方法值解析。

## 3 分项与依赖

每个分项完成自己的规范、实现、专项测试和全量回归后即可单独交付。依赖只表示开始该
分项前必须已经存在的通用能力，不要求把全部分项合并成一次大交付。

| ID | 分项 | 前置依赖 |
| --- | --- | --- |
| MV01 | object-form `spec` 参数/局部值的实例方法值 | 无 |
| MV02 | `T: ObjectSpec` 泛型值的实例方法值 | MV01 |
| MV03 | 具体 `type` / 可见 `fit` 的静态方法值 | 无 |
| MV04 | `T: ObjectSpec` 类型参数的静态方法值 | 无 |
| MV05 | intersection-form `spec` 参数/局部值的实例方法值 | MV01 |
| MV06 | `T: IntersectionSpec` 泛型值的实例方法值 | MV02、MV05 |
| IC01 | `T: IntersectionSpec` 静态方法直接调用 | 无；独立 bugfix |
| MV07 | `T: IntersectionSpec` 类型参数的静态方法值 | IC01、MV04 |

建议按 `MV01 → MV02 → MV05 → MV06` 完成实例分派链，按
`MV03`、`MV04 → IC01 → MV07` 完成静态链。每完成一个分项即执行该分项的交付门槛，
不得等到全部分项结束后才集中验证。

## 4 分项任务

### 4.1 MV01：object-form `spec` 参数/局部值的实例方法值

#### 问题与最小示例

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Reader(offset: int): string;

func direct(value: Readable): string {
  return value.read(0); // 当前已支持
}

func bind(value: Readable): Reader {
  return value.read; // 当前 AE0522
}
```

`value.read` 的成员访问本来合法，目标 `Reader` 也已明确；当前只缺少把形成点的 spec
receiver 与选中 requirement 绑定为 callable value 的能力。

#### 期望行为

- `bind` 编译通过并返回可调用的 `Reader`；
- receiver 只求值一次，方法值始终绑定形成点的 subject 与分派目标；
- 原 spec 局部绑定后续被重新赋值，不改变已经形成的方法值；
- 子 spec 视角取得父 requirement、默认 spec 值以及合法 `spec seal` 方法遵守同一规则；
- 不改变现有 `value.read(args...)` 直接调用行为和开销。

#### 修复任务

- [ ] 更新函数、spec、可见性和诊断主规范，定义 object-form spec 实例方法值。
- [ ] 让目标类型驱动的方法值解析复用 object-form spec 实例方法直接调用的成员闭包、
      原声明 requirement、访问过滤和重载匹配结果。
- [ ] 记录 Codegen 所需的稳定已解析事实，不在 Codegen 按名称重新选择成员。
- [ ] 实现 receiver 一次绑定、动态分派和正确生命周期；具体内部表示由实现分析决定。
- [ ] 若实现需要第二个 receiver box、通用 lambda capture cell、新 runtime ABI 或额外
      每次调用查找，停止并提交人工决策。

#### 验证与交付

- [ ] Semantic：绑定、参数、返回和显式 callable 转换四种目标位置均通过。
- [ ] Semantic：无 callable 目标、签名不匹配、歧义和非法 seal 访问分别稳定拒绝。
- [ ] FCTS：两个实际 type 通过同一 spec 形成方法值，分别进入各自实现。
- [ ] FCTS：receiver 只求值一次、局部重新赋值不重绑定、方法值逃逸后仍有效。
- [ ] FCTS：子到父 requirement、默认 spec 值、本地与 imported spec 均有代表用例。
- [ ] Codegen：没有二次装箱、每次调用不重新查找 witness/member。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 MV01 可独立交付。

### 4.2 MV02：`T: ObjectSpec` 泛型值的实例方法值

#### 问题与最小示例

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Reader(offset: int): string;

func direct<T: Readable>(value: T): string {
  return value.read(0); // 当前已支持
}

func bind<T: Readable>(value: T): Reader {
  return value.read; // 当前 AE0522
}
```

这里 `value` 的静态类型是 `T`，不是一等 object-form spec 值。直接调用已经能够通过
`T` 的约束能力分派；方法值形成不得把 `T` 改写为 `{ subject, witness }`。

#### 期望行为

- `bind` 对满足 `Readable` 的合法 `T` 编译并运行；
- receiver 按闭合 `T` 的既有值模型复制或保留，形成后与原绑定重新赋值无关；
- 引用类型保持同一实例，值语义类型保持形成点的独立值存储；
- 不产生 object-form spec box，也不以单态化共享泛型体作为正确性前提。

#### 修复任务

- [ ] 更新函数、spec、泛型和诊断主规范，明确受约束泛型 receiver 的方法值语义。
- [ ] 复用 MV01 的 requirement 选择和访问过滤，但保留 receiver 的完整 `T` 类型事实。
- [ ] 复用既有 generic value capture、descriptor、cleanup 和共享体具体化基础设施。
- [ ] 证明普通闭合代码、共享泛型体和跨包 consumer 使用同一语义计划。
- [ ] 若必须增加 spec box、第二次 receiver 分配或既有泛型路径开销，停止并提交人工决策。

#### 验证与交付

- [ ] Semantic：object-form 约束泛型值在绑定、参数、返回和显式转换位置均通过。
- [ ] FCTS：`T` 分别闭合为托管引用、trivial 值和 descriptor-sized 值语义类型。
- [ ] FCTS：引用 receiver 绑定同一实例；值 receiver 捕获独立值且连续调用保持其状态。
- [ ] Codegen：三类 `T` 均不生成 spec box，不保存会逃逸的栈地址。
- [ ] 跨包：provider 共享泛型体由 consumer-only 具体类型闭合并正确调用。
- [ ] 生命周期：托管叶子在复制、覆盖、正常退出和异常展开时正确清理。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 MV02 可独立交付。

### 4.3 MV03：具体 `type` / 可见 `fit` 的静态方法值

#### 问题与最小示例

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

func direct(value: int): int {
  return Math.double(value) + ExtendedMath.triple(value); // 当前已支持
}

let double: IntMapper = Math.double;          // 当前 AE0522
let triple: IntMapper = ExtendedMath.triple;  // 当前 AE0522
```

type 自有静态方法和当前位置可见的 fit 静态方法已经属于同一合法静态成员调用面；当前
缺少的是把已选定静态方法形成 callable value。

#### 期望行为

- type 自有和可见 fit 静态方法均可在明确 callable 目标下形成值；
- 形成点固定已经解析的 owner、fit 和方法，不捕获 receiver 或 subject；
- fit 的 module/import 可见面、普通 seal、`@friend` 和 `@mixable` 授权保持既有规则；
- 泛型 owner 与泛型静态方法按既有规则完整闭合，方法自身泛参不得由目标反向推导；
- 无动态捕获的完全闭合静态来源不得无故增加每次形成时的堆分配。

#### 修复任务

- [ ] 更新函数、type、fit、可见性、泛型和诊断主规范，定义具体静态方法值。
- [ ] 复用具体静态直接调用的 type/fit 候选、可见性过滤、owner 代入和重载结果。
- [ ] 让目标 callable 匹配接受已解析的具体静态方法来源，不伪造 receiver 或 spec witness。
- [ ] 分析并复用现有无捕获 callable value 能力；如实现认为必须动态分配，先提交性能决策。
- [ ] 保持本地、imported、普通闭合和共享泛型体使用同一来源身份与调用语义。

#### 验证与交付

- [ ] Semantic：type 自有静态方法和可见 fit 静态方法均可形成值。
- [ ] Semantic：不可见 fit、不可访问 seal、签名不匹配、歧义和缺失显式方法实参稳定拒绝。
- [ ] FCTS：type 与 fit 静态方法值分别调用正确实现，并可保存、传递和返回。
- [ ] 泛型：普通/泛型 owner、普通/泛型静态方法、本地/imported 来源均有代表用例。
- [ ] 性能：完全闭合且无捕获的静态来源不产生不必要的动态 closure 分配。
- [ ] 恢复 friend 专项 P01 中字段初始化、构造函数和终结器的 `Vault.readShared` 方法值覆盖，
      不增加 friend 专用实现路径。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 MV03 可独立交付。

### 4.4 MV04：`T: ObjectSpec` 类型参数的静态方法值

#### 问题与最小示例

```feng
open spec Factory {
  static func create(seed: int): string;
}

open spec Creator(seed: int): string;

func direct<T: Factory>(seed: int): string {
  return T.create(seed); // 当前已支持
}

func bind<T: Factory>(): Creator {
  return T.create; // 当前 AE0522
}
```

`T.create()` 已能通过 object-form spec 约束的静态 requirement 分派。静态方法值没有
实例 receiver；需要形成的是与当前闭合 `T` 对应的 callable。

#### 期望行为

- `T.create` 在明确 `Creator` 目标下形成 callable value；
- callable 固定当前闭合 `T` 对应的静态实现，不捕获或伪造 subject；
- `seal static` requirement 与直接调用使用相同访问判断；
- 每次调用不重新按名称搜索成员或重新执行重载选择。

#### 修复任务

- [ ] 更新函数、spec、泛型、可见性和诊断主规范，定义受约束类型参数静态方法值。
- [ ] 复用 `T.create()` 直接调用已经解析的 requirement、原声明 spec、访问权限和闭合签名。
- [ ] 在实现前比较无捕获静态 callable 与动态绑定表示，选择满足语义且成本最低的通用方案。
- [ ] 如果正确实现需要每次形成时新增动态分配或修改 runtime ABI，停止并提交人工决策。
- [ ] 验证共享泛型体和跨包恢复是否需要扩展 `.ft` 值域或兼容边界；不得预设结论。

#### 验证与交付

- [ ] Semantic：公开与合法 seal 静态 requirement 方法值均通过。
- [ ] Semantic：非法访问、无目标、签名不匹配和歧义稳定拒绝。
- [ ] FCTS：不同闭合 `T` 形成的方法值分别进入各自 type/fit 静态实现。
- [ ] 共享泛型与跨包：provider/consumer 使用同一闭合 requirement 身份。
- [ ] 性能：记录形成、复制、调用和释放的实际成本，并确认没有未审批的增量开销。
- [ ] `.ft`：若扩展合法值域或兼容规则，先更新符号表规范并取得 Review。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录表示决策、实施问题和最终结果，标记 MV04 可独立交付。

### 4.5 MV05：intersection-form `spec` 参数/局部值的实例方法值

#### 问题与最小示例

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Traceable {
  func trace(): string;
}

open spec ReadableTraceable: Readable & Traceable;
open spec Reader(offset: int): string;

func direct(value: ReadableTraceable): string {
  return value.read(0); // 当前已支持
}

func bind(value: ReadableTraceable): Reader {
  return value.read; // 当前 AE0522
}
```

intersection-form 参数/局部值已经可以通过 merged member/witness 视角调用成员；当前只
缺少从同一视角形成实例方法值。

#### 期望行为

- `value.read` 使用与直接调用相同的 merged member/witness 选择结果；
- receiver 在形成点绑定一次，不拆成多个 object-form spec 值；
- 方法来自任一直接成员、嵌套 intersection 或 object-form 父 spec 时行为一致；
- 不增加 intersection 专用 callable 类型、运行时 tag 或成员搜索。

#### 修复任务

- [ ] 更新函数、spec、可见性和诊断主规范，定义显式 intersection spec 实例方法值。
- [ ] 在 MV01 基础上复用 intersection 直接调用已经展平、去重的成员面和稳定槽映射。
- [ ] 保留 requirement 原声明 spec，以正确处理父成员、seal 和重载。
- [ ] 复用 intersection 值既有 subject 生命周期，不做第二次视角构造或装箱。
- [ ] 若直接调用没有提供方法值所需的稳定解析事实，完善通用 intersection 成员解析，
      不在方法值路径增加补偿特判。

#### 验证与交付

- [ ] Semantic：来自不同成员 spec、父 spec 和嵌套 intersection 的方法值均通过。
- [ ] Semantic：重复签名去重、合法重载、返回类型冲突和 seal 访问保持既有规则。
- [ ] FCTS：同一 subject 通过 merged witness 形成方法值并进入正确实现。
- [ ] FCTS：局部重新赋值不重绑定，方法值逃逸后 subject 生命周期正确。
- [ ] 跨包：imported intersection 及其成员 spec 方法值正确恢复。
- [ ] Codegen：不拆分为多个 spec 值，不增加运行时 member 搜索。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 MV05 可独立交付。

### 4.6 MV06：`T: IntersectionSpec` 泛型值的实例方法值

#### 问题与最小示例

```feng
open spec Readable {
  func read(offset: int): string;
}

open spec Traceable {
  func trace(): string;
}

open spec ReadableTraceable: Readable & Traceable;
open spec Reader(offset: int): string;

func direct<T: ReadableTraceable>(value: T): string {
  return value.read(0); // 当前已支持
}

func bind<T: ReadableTraceable>(value: T): Reader {
  return value.read; // 当前 AE0522
}
```

该分项同时组合 MV02 的“receiver 保持 `T` 值语义”和 MV05 的“成员来自 merged
witness”两个已经分别验证的维度。

#### 期望行为

- receiver 按闭合 `T` 的既有值模型绑定，不转成 intersection/object-form spec box；
- requirement 从 `T` 的 intersection 约束成员面选择，并使用对应 merged witness 分派；
- 引用和值语义 receiver 分别保持 MV02 已确认的绑定行为；
- 普通闭合、共享泛型体和跨包路径一致。

#### 修复任务

- [ ] 更新函数、spec、泛型和诊断主规范，明确 intersection 约束泛型 receiver 方法值。
- [ ] 组合复用 MV02 的泛型 receiver 捕获计划与 MV05 的 merged requirement 解析结果。
- [ ] 保留完整 `T` 类型事实、requirement 原声明和 intersection 约束实例。
- [ ] 不新增只针对 intersection 泛型方法值的 closure、descriptor 或 `.ft` 特判。
- [ ] 若组合时发现两项基础抽象不能复用，先在第 6 节记录根因并提交通用方案 Review。

#### 验证与交付

- [ ] Semantic：object 成员、父成员、重载和合法 seal 方法值均通过。
- [ ] FCTS：`T` 分别闭合为托管引用、trivial 值和 descriptor-sized 值语义类型。
- [ ] FCTS：不同实际 type 通过各自 merged witness 调用正确实现。
- [ ] Codegen：不生成 spec box，不重复读取成员映射，不保存栈地址。
- [ ] 共享泛型与跨包：consumer-only 类型闭合路径通过。
- [ ] 生命周期：内联值中的托管叶子正确复制和清理。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 MV06 可独立交付。

### 4.7 IC01：`T: IntersectionSpec` 静态方法直接调用

#### 问题与最小示例

```feng
open spec Factory {
  static func create(seed: int): string;
}

open spec TaggedFactory {
  static func tag(): string;
}

open spec CombinedFactory: Factory & TaggedFactory;

func direct<T: CombinedFactory>(seed: int): string {
  return T.create(seed); // 当前 AE0512
}
```

这是独立的直接调用基线缺口，不是方法值失败。当前 object-form 约束下的
`T.create(seed)` 已支持；intersection 约束下尚未取得同一静态 requirement。

#### 期望行为

- `T.create(seed)` 能从 intersection 展平后的合法静态成员面解析到 `Factory.create`；
- 访问过滤、重载、原声明 spec、泛型代入和静态 witness 分派与 object-form 约束一致；
- 不影响 intersection 实例方法调用和 object-form 静态方法调用；
- 不为后续方法值增加专用绕行入口。

#### 修复任务

- [ ] 建立独立 IC01 bugfix 文档，更新 spec、泛型、可见性和诊断主规范。
- [ ] 找到 `AE0512` 的直接调用解析根因，并让通用静态 constraint member resolver 支持
      intersection 展平成员面。
- [ ] 复用既有 merged surface 的去重、冲突和原声明身份，不在调用点按名称临时搜索。
- [ ] Codegen 消费 Semantic 已解析的静态 requirement，不重新推断来源成员。
- [ ] 若现有 intersection witness/符号事实不足，先记录并 Review 通用扩展方案。

#### 验证与交付

- [ ] Semantic：直接成员、嵌套 intersection、父 spec 静态方法和合法重载均通过。
- [ ] Semantic：返回类型冲突、非法 seal 访问和不存在成员保持稳定诊断。
- [ ] FCTS：不同闭合 `T` 通过各自 merged witness 静态实现返回正确结果。
- [ ] 本地、共享泛型体和跨包 provider/consumer 均有覆盖。
- [ ] 回归：object-form 约束静态调用与 intersection 实例调用行为和成本不变。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 IC01 可独立交付。

### 4.8 MV07：`T: IntersectionSpec` 类型参数的静态方法值

#### 问题与最小示例

```feng
open spec Factory {
  static func create(seed: int): string;
}

open spec TaggedFactory {
  static func tag(): string;
}

open spec CombinedFactory: Factory & TaggedFactory;
open spec Creator(seed: int): string;

func bind<T: CombinedFactory>(): Creator {
  return T.create; // 当前先受 IC01 阻塞，尚未进入方法值解析
}
```

该分项只有在 IC01 使 `T.create(seed)` 成为合法直接调用、MV04 已定义受约束类型参数静态
方法值之后才能开始。它组合的是既有静态方法值规则与 intersection 静态成员面，不应
产生第三套静态方法值机制。

#### 期望行为

- `T.create` 形成绑定当前闭合 `T` 静态实现的 `Creator`；
- requirement 选择、原声明 spec、访问权限和 merged 槽映射与 IC01 完全一致；
- 不捕获或伪造 subject；
- 形成、调用和生命周期成本遵守 MV04 已 Review 的静态方法值表示决策。

#### 修复任务

- [ ] 在 IC01 与 MV04 均完成后，更新函数、spec、泛型、可见性和诊断主规范。
- [ ] 让方法值解析直接消费 IC01 的静态 requirement 解析结果。
- [ ] 复用 MV04 的静态 callable 形成与共享泛型具体化能力。
- [ ] 不增加 intersection 专用 binding kind、closure 或 `.ft` 记录形态。
- [ ] 若 IC01 或 MV04 的稳定事实不足，回到对应基础分项完善通用抽象，不在 MV07 补特判。

#### 验证与交付

- [ ] Semantic：直接成员、父成员、嵌套 intersection、公开与合法 seal 静态方法值通过。
- [ ] Semantic：无目标、签名不匹配、歧义和非法访问稳定拒绝。
- [ ] FCTS：不同闭合 `T` 的方法值分别进入各自静态实现。
- [ ] 本地、共享泛型体和跨包 provider/consumer 均有覆盖。
- [ ] 性能：与 MV04 相同的静态来源不增加额外分配、查找或调用层。
- [ ] 专项测试通过，并在沙箱外执行完整 `make test`。
- [ ] 在第 6 节记录实施问题与最终结果，标记 MV07 可独立交付。

## 5 通用验证与交付规则

每个分项都必须独立满足以下交付规则：

1. 先更新该分项涉及的权威规范，再修改 Semantic/Codegen，最后增加测试。
2. 编译器测试放在 `test/`，重点验证诊断码、稳定语义事实和必要的生成结构；用户可观察
   的形成、调用、分派、状态与生命周期行为优先放在 `fcts/`。
3. 新增测试，不修改既有测试；确需修改既有测试时必须先取得人工批准。MV03 恢复此前
   明确移除的 P01 方法值覆盖属于该分项显式任务。
4. 不做所有类型、入口和包形态的笛卡尔积；每个非等价语义或运行时表示至少保留一个
   能证明行为的代表用例。
5. 任一实现问题必须先写入第 6 节，再分析根因、通用方案、性能/ABI 影响和是否需要人工
   决策；禁止先加特判使测试通过。
6. 每个非文档分项完成后都必须在 Codex 沙箱外执行完整 `make test`，不得只依赖最终一次
   总回归。
7. 每个分项交付记录至少包含：变更范围、专项测试、FCTS 结果、全量回归结果、未解决问题
   和建议的英文 commit message。

## 6 实施过程问题记录

实施过程中发现问题时，先按下列模板增加记录，再继续分析或修改代码：

### ISSUE-待编号：待填写

- **关联分项**：MV/IC 待填写
- **状态**：待分析
- **最小复现**：待填写
- **实际结果**：待填写
- **期望结果**：待填写
- **根因**：待填写
- **通用修复方案**：待填写
- **运行时性能影响**：待填写
- **runtime ABI / `.ft` / 兼容性影响**：待填写
- **是否需要人工决策**：待填写
- **专项验证结果**：待填写
- **全量回归结果**：待填写

## 7 总体完成标准

只有满足以下条件，整个专项才可标记完成：

- [ ] MV01、MV02、MV03、MV04、MV05、MV06、IC01、MV07 均已经单独完成并具有交付记录。
- [ ] 每个分项的正式语义均已收敛到对应权威规范，本文没有成为第二份语言规范。
- [ ] 所有原本合法的直接调用保持原行为，实例/静态访问边界没有扩大。
- [ ] object-form、intersection-form、具体 type/fit、普通泛型和共享泛型路径均有有效证据。
- [ ] 没有类型名、spec 名、方法名、包名、测试模型或参数位置特判。
- [ ] 没有未经人工批准的运行时成本、runtime ABI 或 `.ft` 兼容性变化。
- [ ] 最后一个分项完成后的沙箱外完整 `make test` 通过。
