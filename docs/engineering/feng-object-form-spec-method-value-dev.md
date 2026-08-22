# Feng 成员方法值缺口与分项交付计划

> **状态**：MV01、MV02、MV03、MV04、MV05、MV06 已完成，可分别独立交付；
> 其余分项尚未实施。
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
8. **强制规则**：任何分项均不得引入增量运行时开销，不得修改或删除任何既有测试用例，
   不得变更任何现有 ABI（包括 runtime 私有 ABI），也不得变更 `.ft` 格式或兼容边界。
   如果经根因分析确认，正确且通用的实现必须触及上述任一项，必须立即停止实施，先在
   第 6 节记录原因、影响和备选方案并提交人工决策；只有取得明确批准后才能继续。

权威语义按分项更新到相关主规范，至少包括：

- [Feng 函数规范](../specifications/feng-function.md)；
- [Feng `spec` 规范](../specifications/feng-spec.md)；
- [Feng 泛型规范](../specifications/feng-generics-draft.md)；
- [Feng `type` 规范](../specifications/feng-type.md)；
- [Feng `fit` 规范](../specifications/feng-fit.md)；
- [Feng 可见性规范](../specifications/feng-visibility.md)；
- 涉及跨包依赖恢复时的 [Feng 符号表规范](../specifications/feng-symbol-table.md)；
- 对应 AE/CE 诊断规范。

## 2 专项实施前基线

本表只记录专项启动时，按既有成员访问规范本来合法的来源。启动时最小探针与回归确认
如下；各分项完成后的结果以第 4、6 节为准：

| 来源 | 直接调用现状 | 方法值现状 | 本专项定位 |
| --- | --- | --- | --- |
| object-form `spec` 参数/局部值的实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE` |
| `T: ObjectSpec` 的泛型值实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，receiver 保持 `T` 的值语义 |
| 具体 `type` / 可见 `fit` 的静态方法 | 已支持 | `AE0522` | `CONCRETE_STATIC`；包含 friend 修复 P01 的 `Vault.readShared` |
| `T: ObjectSpec` 的类型参数静态方法 | 已支持 | `AE0522` | `SPEC_STATIC` |
| intersection-form `spec` 参数/局部值的实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，使用 merged witness |
| `T: IntersectionSpec` 的泛型值实例方法 | 已支持 | `AE0522` | `SPEC_INSTANCE`，receiver 保持 `T` 的值语义并使用 merged witness |
| `T: IntersectionSpec` 的类型参数静态方法 | `AE0512` | 尚未进入方法值解析 | 直接调用存在独立缺口；对应方法值依赖其先修复 |

专项启动时，前六行缺失的是“把已经可以直接调用的合法成员引用形成 callable value”。
最后一行首先是直接调用缺口，不能把它直接归因于方法值解析。

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

- [x] 更新函数、spec、可见性和诊断主规范，定义 object-form spec 实例方法值。
- [x] 让目标类型驱动的方法值解析复用 object-form spec 实例方法直接调用的成员闭包、
      原声明 requirement、访问过滤和重载匹配结果。
- [x] 记录 Codegen 所需的稳定已解析事实，不在 Codegen 按名称重新选择成员。
- [x] 实现 receiver 一次绑定、动态分派和正确生命周期；具体内部表示由实现分析决定。
- [x] 若实现需要第二个 receiver box、通用 lambda capture cell、新 runtime ABI 或额外
      每次调用查找，停止并提交人工决策。

#### 验证与交付

- [x] Semantic：绑定、参数、返回和显式 callable 转换四种目标位置均通过。
- [x] Semantic：无 callable 目标、签名不匹配和非法 seal 访问分别稳定拒绝；相同签名的
      继承 requirement 继续按现有直接调用规则去重，不产生虚假歧义。
- [x] FCTS：两个实际 type 通过同一 spec 形成方法值，分别进入各自实现。
- [x] FCTS：receiver 只求值一次、局部重新赋值不重绑定、方法值逃逸后仍有效。
- [x] FCTS：子到父 requirement、默认 spec 值、本地与 imported spec 均有代表用例。
- [x] Codegen：没有二次装箱、每次调用不重新查找 witness/member。
- [x] 专项测试通过，并在沙箱外执行完整 `make test`。
- [x] 在第 6 节记录实施问题与最终结果，标记 MV01 可独立交付。

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

- [x] 更新函数、spec、泛型和符号表主规范，明确受约束泛型 receiver 的方法值及跨包恢复
      语义；核对现有诊断规范，本分项不新增或变更诊断。
- [x] 复用 MV01 的 requirement 选择和访问过滤，但保留 receiver 的完整 `T` 类型事实。
- [x] 复用既有 generic value capture、descriptor、cleanup 和共享体具体化基础设施。
- [x] 证明普通闭合代码、共享泛型体和跨包 consumer 使用同一语义计划。
- [x] 确认没有增加 spec box、第二次 receiver 分配或任何此前合法路径的运行时开销。

#### 验证与交付

- [x] Semantic：object-form 约束泛型值在绑定、参数、返回和显式转换位置均通过。
- [x] FCTS：`T` 分别闭合为托管引用、trivial 值和 descriptor-sized 值语义类型。
- [x] FCTS：引用 receiver 绑定同一实例；值 receiver 捕获独立值且连续调用保持其状态。
- [x] Codegen：三类 `T` 均不生成 spec box，不保存会逃逸的栈地址。
- [x] 跨包：provider 共享泛型体由 consumer-only 具体类型闭合并正确调用。
- [x] 生命周期：托管叶子在复制、覆盖、正常退出和异常展开时正确清理。
- [x] 专项测试通过，并在沙箱外执行完整 `make test`。
- [x] 在第 6 节记录实施问题与最终结果，标记 MV02 可独立交付。

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

- [x] 更新函数、type、fit、可见性、泛型和诊断主规范，定义具体静态方法值。
- [x] 复用具体静态直接调用的 type/fit 候选、可见性过滤、owner 代入和重载结果。
- [x] 让目标 callable 匹配接受已解析的具体静态方法来源，不伪造 receiver 或 spec witness。
- [x] 分析并复用现有无捕获 callable value 能力；如实现认为必须动态分配，先提交性能决策。
- [x] 保持本地、imported、普通闭合和共享泛型体使用同一来源身份与调用语义。

#### 验证与交付

- [x] Semantic：type 自有静态方法和可见 fit 静态方法均可形成值。
- [x] Semantic：不可见 fit、不可访问 seal、签名不匹配、歧义和缺失显式方法实参稳定拒绝。
- [x] FCTS：type 与 fit 静态方法值分别调用正确实现，并可保存、传递和返回。
- [x] 泛型：普通/泛型 owner、普通/泛型静态方法、本地/imported 来源均有代表用例。
- [x] 性能：完全闭合且无捕获的静态来源不产生不必要的动态 closure 分配。
- [x] 恢复 friend 专项 P01 中字段初始化、构造函数和终结器的 `Vault.readShared` 方法值覆盖，
      不增加 friend 专用实现路径。
- [x] 专项测试通过，并在沙箱外执行完整 `make test`。
- [x] 在第 6 节记录实施问题与最终结果，标记 MV03 可独立交付。

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

- [x] 更新函数、spec、泛型、可见性和诊断主规范，定义受约束类型参数静态方法值。
- [x] 复用 `T.create()` 直接调用已经解析的 requirement、原声明 spec、访问权限和闭合签名。
- [x] 在实现前比较无捕获静态 callable 与动态绑定表示，选择满足语义且成本最低的通用方案。
- [x] 如果正确实现需要每次形成时新增动态分配或修改 runtime ABI，停止并提交人工决策。
- [x] 验证共享泛型体和跨包恢复是否需要扩展 `.ft` 值域或兼容边界；不得预设结论。

#### 验证与交付

- [x] Semantic：公开与合法 seal 静态 requirement 方法值均通过。
- [x] Semantic：非法访问、无目标、签名不匹配和歧义稳定拒绝。
- [x] FCTS：不同闭合 `T` 形成的方法值分别进入各自 type/fit 静态实现。
- [x] 共享泛型与跨包：provider/consumer 使用同一闭合 requirement 身份。
- [x] 性能：记录形成、复制、调用和释放的实际成本，并确认没有未审批的增量开销。
- [x] `.ft`：若扩展合法值域或兼容规则，先更新符号表规范并取得 Review。
- [x] 专项测试通过，并在沙箱外执行完整 `make test`。
- [x] 在第 6 节记录表示决策、实施问题和最终结果，标记 MV04 可独立交付。

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

- [x] 更新函数、spec、可见性和诊断主规范，定义显式 intersection spec 实例方法值。
- [x] 在 MV01 基础上复用 intersection 直接调用已经展平、去重的成员面和稳定槽映射。
- [x] 保留 requirement 原声明 spec，以正确处理父成员、seal 和重载。
- [x] 复用 intersection 值既有 subject 生命周期，不做第二次视角构造或装箱。
- [x] 若直接调用没有提供方法值所需的稳定解析事实，完善通用 intersection 成员解析，
      不在方法值路径增加补偿特判。

#### 验证与交付

- [x] Semantic：来自不同成员 spec、父 spec 和嵌套 intersection 的方法值均通过。
- [x] Semantic：重复签名去重、合法重载、返回类型冲突和 seal 访问保持既有规则。
- [x] FCTS：同一 subject 通过 merged witness 形成方法值并进入正确实现。
- [x] FCTS：局部重新赋值不重绑定，方法值逃逸后 subject 生命周期正确。
- [x] 跨包：imported intersection 及其成员 spec 方法值正确恢复。
- [x] Codegen：不拆分为多个 spec 值，不增加运行时 member 搜索。
- [x] 专项测试通过，并在沙箱外执行完整 `make test`。
- [x] 在第 6 节记录实施问题与最终结果，标记 MV05 可独立交付。

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

- [x] 更新函数、spec、泛型、符号表和诊断主规范，明确 intersection 约束泛型 receiver
      方法值及跨包恢复边界。
- [x] 组合复用 MV02 的泛型 receiver 捕获计划与 MV05 的 merged requirement 解析结果。
- [x] 保留完整 `T` 类型事实、requirement 原声明和 intersection 约束实例。
- [x] 不新增只针对 intersection 泛型方法值的 closure、descriptor 或 `.ft` 特判。
- [x] 若组合时发现两项基础抽象不能复用，先在第 6 节记录根因并提交通用方案 Review。

#### 验证与交付

- [x] Semantic：object 成员、父成员、重载和合法 seal 方法值均通过。
- [x] FCTS：`T` 分别闭合为托管引用、trivial 值和 descriptor-sized 值语义类型。
- [x] FCTS：不同实际 type 通过各自 merged witness 调用正确实现。
- [x] Codegen：不生成 spec box，不重复读取成员映射，不保存栈地址。
- [x] 共享泛型与跨包：consumer-only 类型闭合路径通过。
- [x] 生命周期：内联值中的托管叶子正确复制和清理。
- [x] 专项测试通过，并在沙箱外执行完整 `make test`。
- [x] 在第 6 节记录实施问题与最终结果，标记 MV06 可独立交付。

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
3. 测试变更严格遵守第 1 节第 8 项强制规则。MV03 恢复此前明确移除的 P01 方法值覆盖
   属于新增覆盖，不构成修改其他既有测试用例的授权。
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

### ISSUE-001：MV01 无法构造独立的合法方法值歧义用例

- **关联分项**：MV01
- **状态**：已解决
- **最小复现**：object-form `spec` 中同名但参数或返回类型不同的 requirement，在明确
  callable-form `spec` 目标下最多只有一个签名能够完全匹配；继承路径中实例化后签名
  完全相同的 requirement 则按现有直接调用规则表示同一逻辑 requirement。
- **实际结果**：MV01 的目标类型匹配是“参数个数、参数类型、参数顺序和返回类型完全
  一致”。两个不同的合法签名不能同时匹配同一个 callable 目标；完全相同的继承或覆盖
  签名由直接调用解析去重。因此不存在既保持现有直接调用语义、又能稳定产生 `AE0521`
  的 MV01 专属合法来源。
- **期望结果**：第 4.1 节当前要求“无 callable 目标、签名不匹配、歧义和非法 seal
  访问分别稳定拒绝”，其中“歧义”需要一个可构造且不改变既有语义的代表用例。
- **根因**：该验收项沿用了通用重载方法值诊断矩阵，但 MV01 的来源只包含 object-form
  `spec` requirement；object-form spec 方法不允许方法级泛参，且 callable 目标执行精确
  结构匹配，所以没有能够同时匹配同一目标的两个非等价候选。
- **通用修复方案**：建议将 MV01 的该项改为“无 callable 目标、签名不匹配和非法
  seal 访问分别稳定拒绝；等价继承 requirement 不产生虚假歧义”，并继续由现有可实际
  构成多候选的方法值来源覆盖 `AE0521`。不建议把等价 requirement 强制判为歧义，因为
  这会偏离现有直接调用的逻辑槽去重语义。
- **运行时性能影响**：无；只涉及验收边界和 Semantic 测试定义。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：已完成。人工确认相同签名的继承 requirement 应当合法；未绑定
  callable 按结构匹配贴合目标 callable-form spec，已经绑定的 callable-form spec 只可在
  结构匹配时显式转换，结构不匹配必须报错且显式转换也不得绕过检查。
- **专项验证结果**：Semantic 与 FCTS 均验证等价继承 requirement 不产生虚假歧义，
  并通过同一逻辑 witness 槽调用实现。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### ISSUE-002：单一 object-form spec 方法引用缺少 callable 目标时被接受

- **关联分项**：MV01
- **状态**：已解决
- **最小复现**：

  ```feng
  spec Readable {
    func read(offset: int): string;
  }

  func run(value: Readable) {
    let reader = value.read;
  }
  ```

- **实际结果**：Semantic 分析成功，没有诊断。
- **期望结果**：方法值必须由明确的 callable-form `spec` 目标驱动；该表达式应以
  `AE0523` 拒绝。
- **根因**：通用无类型绑定校验只在来源存在多个重载时要求 callable 目标。单一顶层函数、
  具体 type 方法和 object-form spec 方法都会被 Semantic 接受，但它们没有匿名 callable
  类型，随后分别在 Codegen 进入错误的普通值/字段路径并失败；具体 type 与 spec 最小探针
  分别产生 `CE0176` 和 `CE0173`。
- **通用修复方案**：让无类型绑定校验对任何尚未绑定的顶层函数或实例方法引用统一要求
  callable-form `spec` 目标；局部变量、参数和 callable 类型字段等已经绑定的 callable 值
  保持原行为。同步把 `AE0523` 文案从“消解重载”收敛为“形成 callable value”。
- **运行时性能影响**：无。修复只增加 Semantic 编译期分类，不改变任何可生成程序的
  运行时代码。
- **runtime ABI / `.ft` / 兼容性影响**：无。当前被接受的最小程序均在 Codegen 失败，
  不存在可执行产物或既有 ABI；修复只把失败提前到稳定的 Semantic 诊断。
- **是否需要人工决策**：已完成。通用修复会让既有编译器用例
  `test_unary_address_of_rejects_bound_method_pointer_target` 在原有两个下游地址诊断之前新增
  `AE0523`，使其精确错误数从 2 变为 3。推荐把该用例的前置绑定修正为
  `let method: BoundCmp = box.cmp`，使其先合法形成真正的 bound method，再继续验证
  `&method` 不能形成 `Cmp*`；缺少目标的行为由新增独立用例验证。该方案需要修改一个
  既有用例，但能保持其原测试目的。若只修改错误数预期，用例会同时混入两个独立错误；
  若只对 object-form spec 增加特判，则会让 type/spec 方法值规则分裂并违反通用实现原则，
  两者均不建议。
- **人工决策结果**：批准采用通用修复，并批准把该既有用例的前置绑定改为显式
  `BoundCmp`，使其继续只验证已绑定方法值不能形成 ABI 函数指针。
- **专项验证结果**：新增单一顶层函数、具体 type 方法和 object-form spec 方法无目标
  用例，均稳定产生 `AE0523`；修正后的 ABI 地址用例继续稳定产生 `AE0231`；
  `build/bin/test_semantic` 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### ISSUE-003：FCTS 使用了当前尚不支持的 callable 返回值立即调用形式

- **关联分项**：MV01
- **状态**：已解决
- **最小复现**：`makeEscapedSpecMethod(30)(3)`。
- **实际结果**：Codegen 报 `CE0166: only direct or method calls supported in this iteration`。
- **期望结果**：MV01 用例需要验证返回的方法值逃逸后仍可调用，不要求新增调用结果链式语法。
- **根因**：新增用例把“返回 callable”和“调用 callable”写在同一表达式中，进入当前尚未
  支持的 callable-result 直接调用路径；方法值本身已经成功形成和返回。
- **通用修复方案**：先把返回值绑定到显式 `SpecMethodReader` 局部，再通过局部调用。
  该写法与逃逸生命周期验证等价，不扩大 MV01 范围。
- **运行时性能影响**：无；只修改新增测试表达式，编译器实现不变。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；不修复或绕过编译器能力，只纠正新增测试夹具。
- **专项验证结果**：修正后的逃逸用例通过，`make fcts-tests` 最终结果为
  `827 passed, 0 failed, 0 skipped`。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### MV01 独立交付记录

- **变更范围**：补齐 object-form `spec` 参数/局部值的实例方法值解析与代码生成；
  同步收敛无匿名 callable 类型时的单一未绑定函数/方法引用诊断。
- **实现结果**：Semantic 复用 spec 父闭包投影、访问过滤、实例化签名和逻辑 requirement
  去重，并把精确 requirement 身份记录到既有 callable coercion sidecar；Codegen 只消费
  该稳定事实，在形成点保留 subject、借用静态 witness 指针，并由 adapter 直接调用已选
  witness 槽。
- **运行时成本**：既有可编译路径没有新增运行时分支、分配或查找。新支持的方法值形成
  只产生一个保存 receiver 的 callable closure，不产生第二个 spec box；后续调用不执行
  witness/member 搜索或重载选择。
- **ABI 与格式**：没有修改 runtime、runtime 私有 ABI、公开 ABI、`.ft` 格式或符号恢复
  边界。
- **专项测试**：`build/bin/test_semantic`、`build/bin/test_codegen` 均通过；生成 C 编译
  通过，并验证一个 closure 分配、零二次装箱和直接 witness 槽调用。
- **FCTS**：本地与跨包动态分派、绑定/参数/返回/显式转换、一次求值、稳定绑定、逃逸、
  父 requirement、默认值、合法 seal、等价继承 requirement 和闭合泛型 spec 实例均通过；
  最终结果为 `827 passed, 0 failed, 0 skipped`。
- **全量回归**：沙箱外完整 `make test` 通过，包含 UBSan、普通 `-O2 -Werror`、smoke、
  CLI、stdlib、FCTS、性能约束、增量构建与发布脚本测试。
- **未解决问题**：MV01 无未解决问题；后续分项状态以第 3、4 节及各自交付记录为准。
- **建议 commit message**：`feat: support object-form spec instance method values`

### ISSUE-004：跨包读取拒绝 constrained-generic spec 方法值依赖

- **关联分项**：MV02
- **状态**：已解决
- **最小复现**：provider 导出
  `bind<T: ObjectSpec>(value: T): CallableSpec { return value.method; }`，consumer-only
  具体类型满足该约束并闭合调用；provider 包可以生成，但 consumer 加载 provider 的 `.fb`
  时读取对应 `.ft` 失败。
- **实际结果**：`make fcts-tests` 在加载
  `mod/fcts_lib/test/lib_spec_method_value.ft` 时失败，报告
  `invalid callable dependency record 0`。
- **期望结果**：既有 callable dependency 表示能够恢复 MV02 编译期依赖，consumer 使用自身具体
  类型生成闭合 descriptor、witness 与 adapter；不得为此变更 `.ft` 格式或 ABI。
- **根因**：`.ft` 写端已经把既有枚举
  `FENG_RESOLVED_CALLABLE_SPEC_METHOD` 写入 callable dependency 记录的现有 `kind: u16`
  字段；读端合法性校验却仍把上界固定在更早的
  `FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD`，因此先拒绝记录。导入恢复逻辑也只覆盖 function、
  type method 和 fit method，未把已经能够映射到 synthesized object-spec member 的目标恢复为
  `SPEC_METHOD` dependency。
- **通用修复方案**：在 `.ft` 读端以明确的合法 kind 集合接受既有
  `SPEC_METHOD` 枚举；在统一 callable dependency 恢复分支中校验目标确为 object-form spec
  实例方法，并恢复 owner/member。新增独立 symbol `.ft` 往返用例，验证 kind、目标 requirement
  identity、receiver `T` 与 callable 目标均原样保留。
- **运行时性能影响**：无；只改变编译期 `.ft` 校验与 sidecar 恢复。
- **runtime ABI / `.ft` / 兼容性影响**：无。没有新增或修改记录字段、尺寸、枚举值、版本号或
  runtime 定义；只让读端接受写端已经按现有格式产生的既有枚举值。
- **是否需要人工决策**：否；修复不触发本专项的 ABI、`.ft` 格式或运行时开销停止条件。
- **专项验证结果**：新增独立 symbol `.ft` 往返用例并通过；修复后的跨包 FCTS 由 provider
  共享泛型体接受 consumer-only 具体类型，正确生成并调用 consumer witness。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### MV02 独立交付记录

- **变更范围**：补齐 `T: ObjectSpec` 泛型值实例方法值的函数/spec/泛型/符号表规范、
  Semantic、共享泛型依赖、闭合代码生成与跨包恢复；没有扩大实例/静态成员访问边界，也
  没有变更现有诊断行为。
- **实现结果**：Semantic 复用 MV01 的 requirement 选择、访问过滤和结构匹配，同时把 receiver
  保持为开放 `T`；共享体通过既有 callable dependency 槽获得已闭合 descriptor。consumer
  为具体 receiver 生成静态 witness adapter：托管引用保留同一实例，trivial 与 aggregate
  receiver 直接存入最终 callable closure，其中 aggregate 沿用既有 descriptor cleanup。
- **运行时成本**：此前所有合法路径没有新增运行时指令、分支、分配或查找。新支持路径在
  方法值形成时只分配一个最终 callable closure，并复用共享泛型值捕获既有的 value-kind
  分派；不产生 spec box、第二次 receiver 分配、运行时成员搜索或每次调用动态查找。
- **ABI 与格式**：没有修改 runtime/private/public ABI、结构布局、字段、枚举值、`.ft` 记录
  尺寸或版本。`FengCallableValueDescriptor` 只修正注释以覆盖同一既有 offset 字段承载的
  inline trivial/aggregate receiver；ISSUE-004 只让读端恢复写端已经写入现有字段的既有
  `SPEC_METHOD` 枚举。
- **专项测试**：`build/bin/test_semantic`、`build/bin/test_codegen`、`build/bin/test_symbol`
  均通过；生成 C 编译通过，并验证三类 receiver 只有一个形成点 closure 分配、零 spec box、
  静态 witness 槽调用和 aggregate cleanup metadata。
- **FCTS**：引用、trivial、含托管叶子的 descriptor-sized receiver，独立连续状态、原绑定
  重赋值、跨包 consumer-only 类型，以及复制、覆盖、正常退出和异常展开均通过；最终结果为
  `830 passed, 0 failed, 0 skipped`。
- **全量回归**：沙箱外完整 `make test` 通过，包含 UBSan、普通 `-O2 -Werror`、smoke、CLI、
  stdlib、FCTS、性能约束、增量构建与发布脚本测试。
- **未解决问题**：MV02 无未解决问题；MV03 及后续未完成分项继续按第 3、4 节实施。
- **建议 commit message**：`feat: support constrained generic spec method values`

### ISSUE-005：闭合泛型 owner 静态方法值 adapter 误用共享体参数 ABI

- **关联分项**：MV03
- **状态**：已解决
- **最小复现**：

  ```feng
  spec IntMapper(value: int): int;

  type GenericMath<T> {
    static func echo(value: T): T {
      return value;
    }
  }

  let echo: IntMapper = GenericMath<int>.echo;
  ```

- **实际结果**：Semantic 与 Feng Codegen 均完成，但 host C 编译拒绝生成代码；静态方法值
  adapter 把 `_arg0` 的地址传给已经闭合为 `int` 直接参数的 thin wrapper，产生
  `int*` 到 `int` 的不兼容调用。
- **期望结果**：`GenericMath<int>.echo` 形成 `IntMapper`，adapter 使用实际目标 thin wrapper
  的直接参数 ABI 调用，且形成时不分配 closure。
- **根因**：静态方法值 adapter 最终调用的是已完成 owner 代入的 concrete thin wrapper，
  但首次实现复用了该方法原始共享体的参数 ABI 判定。原始 `T` 在共享体中按地址传递；
  `GenericMath<int>` thin wrapper 的对应参数已经闭合为直接 `int`，两层 ABI 不同。
- **通用修复方案**：adapter 始终按其实际调用层的注册参数面选择 ABI：user type/fit 使用
  已闭合 `UserMethod.param_types` 与 thin wrapper 的既有 address 判定；builtin fit 使用其
  现有方法入口的泛参擦除判定。adapter 继续负责在 callable 目标 ABI 与该 wrapper ABI
  之间做静态桥接，不绕过 thin wrapper，也不按具体类型或方法名特判。
- **运行时性能影响**：无增量开销；只修正生成 C 中参数表达式是传值还是取地址，不增加
  分支、分配、查找或调用层。
- **runtime ABI / `.ft` / 兼容性影响**：无；未修改任何 runtime 定义、ABI、`.ft` 字段、
  枚举或版本。
- **是否需要人工决策**：否；这是 adapter 与其实际 callee ABI 不一致的实现错误，修复
  完全复用既有 thin wrapper 规则，不触发专项停止条件。
- **专项验证结果**：普通 type/fit、闭合泛型 owner、显式泛型静态方法以及共享泛型函数
  返回的静态方法值均已通过 Semantic、Codegen、Symbol、生成 C 严格编译和 FCTS 真实执行。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### ISSUE-006：FCTS 泛型 fit 用例对无约束 `T` 使用数值加法

- **关联分项**：MV03
- **状态**：已解决
- **最小复现**：

  ```feng
  type Extended<T> {}

  fit Extended<T> {
    static func triple(value: T): T {
      return value + value + value;
    }
  }
  ```

- **实际结果**：FCTS 编译在两个 `+` 位置报告 `AE0030`；无约束泛型 `T` 不能使用数值或
  字符串加法。
- **期望结果**：MV03 的泛型 fit 静态方法值用例仅使用对任意 `T` 合法的函数体，使测试
  失败能够反映静态方法值形成或调用问题，而不是无关的泛型运算约束。
- **根因**：用例为了让 fit 结果易于观察，误把只在闭合为 `int` 时合法的 `triple`
  实现写进对所有 `T` 都要先完成语义检查的共享泛型声明；其函数体与 MV03 的静态方法值
  目标无关。
- **通用修复方案**：将泛型 fit 方法改为对任意 `T` 都合法的 `echo(value: T): T`，并通过
  type 自有 `double`、builtin fit `mv03Increment` 以及来源身份断言继续区分实际选中的实现；
  不添加约束特判，也不削减静态方法值覆盖面。
- **运行时性能影响**：无；只修正 FCTS 用例函数体及对应期望值，不修改产品代码。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；诊断符合既有泛型与运算符规范，修复仅移除测试中的无关非法
  表达式，不改变需求或实现方案。
- **专项验证结果**：FCTS 重新编译并真实执行通过，MV03 与完整 FCTS 共 835 项全部通过。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### ISSUE-007：跨包泛型静态方法值 adapter 缺少 imported thin-wrapper 声明

- **关联分项**：MV03
- **状态**：已解决
- **最小复现**：

  ```feng
  // provider
  open type Math {
    open static func identity<T>(value: T): T {
      return value;
    }
  }

  // consumer
  spec IntMapper(value: int): int;
  let mapper: IntMapper = Math.identity<int>;
  ```

- **实际结果**：Semantic 与 Feng Codegen 成功；consumer 的静态 callable adapter 调用 provider
  generic static method thin wrapper，但生成 C 未声明该 wrapper，host C 以隐式函数声明错误拒绝。
- **期望结果**：跨包 generic static method value 与既有直接调用使用同一 imported wrapper 声明面；
  adapter 能严格编译并链接到 provider 实现。
- **根因**：静态 method-value adapter 首次实现统一调用 `UserMethod.c_name`。本地闭合来源的
  该符号是可用 thin wrapper；但 imported generic method 的 thin wrapper 仅具 provider 文件内
  链接，跨包原本就只公开 `FengGenericMethod__...` / `FengFitMethod__...` shared dispatch。既有
  直接调用已做这一区分，method-value adapter 尚未复用。
- **通用修复方案**：抽取并复用直接静态调用的 call-surface 判定。对本地闭合来源继续调用
  thin wrapper；对 imported generic method 或 imported generic owner 实例，adapter 直接静态调用
  既有 exported shared dispatch，并按其既有 ABI 传入 owner descriptor、function descriptor、
  显式方法类型 descriptor、声明参数表示和返回 out storage。
- **运行时性能影响**：无增量开销；选择在编译期完成，不增加分配、查找或运行时分支；跨包
  adapter 直接进入既有 shared dispatch，也不增加代理调用层。
- **runtime ABI / `.ft` / 兼容性影响**：无；仅复用已有 exported shared symbol 与签名，未修改
  runtime 定义、公开/私有 ABI、`.ft` 字段、枚举或版本。
- **是否需要人工决策**：否；修复使 method value 与既有直接调用恢复同一跨包调用面，不引入
  特判或新的表示决策。
- **专项验证结果**：Codegen 专项通过；FCTS 的 imported 普通/泛型 owner、普通/泛型 type/fit
  静态方法值及 provider shared-body dependency 均真实执行通过，完整 FCTS 为 835/835。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### ISSUE-008：P01 恢复直接改写了既有 friend 用例

- **关联分项**：MV03
- **状态**：已解决
- **最小复现**：为恢复 friend P01，直接向既有
  `test_friend_type_implementation_contexts_are_authorized` 与 FCTS
  `type implementation contexts execute friend access` 夹具增加静态方法值，并调整既有
  FCTS 合计期望。
- **实际结果**：行为覆盖与全量回归均通过，但最终合规审查确认该做法改写了既有测试
  夹具和断言，不满足第 1 节第 8 项“不得修改或删除任何既有测试用例”的最严格解释。
- **期望结果**：保留原 friend 用例逐字不变，通过独立新增的 MV03 Semantic/FCTS 用例
  覆盖字段初始化、构造函数和终结器中的 friend 静态方法值。
- **根因**：首次恢复沿用了 P01 原问题所在夹具，以最短路径把静态方法值断言插回原测试；
  没有把“恢复行为覆盖”与“不可改写既有用例”同时落实为独立新增用例。
- **通用修复方案**：撤销对两个既有 friend 用例的修改；在 MV03 新增测试域中定义独立
  friend owner/target/state，分别验证三个类型实现上下文，并保留原测试及其期望不变。
- **运行时性能影响**：无；只重组新增测试覆盖，不修改产品实现。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；该修复直接落实已经明确的强制规则，不改变语言语义、实现
  范围或验收目标。
- **专项验证结果**：原 `test_friend` 与原 Semantic friend 用例恢复为零差异；独立新增的
  Semantic 用例通过，FCTS 在三个类型实现上下文中真实形成并调用 friend 静态方法值，
  最终为 `836 passed, 0 failed, 0 skipped`。
- **全量回归结果**：迁移后的最终工作树在沙箱外完整 `make test` 通过。

### MV03 独立交付记录

- **变更范围**：补齐具体 `type` 自有与当前位置可见 `fit` 静态方法值的函数、type、fit、
  泛型、可见性、诊断和符号表规范，以及 Semantic、共享泛型依赖与 Codegen；没有扩大
  实例/静态成员访问边界，也没有改变既有直接调用规则。
- **实现结果**：Semantic 复用静态直接调用的 type/fit 候选面、访问过滤、owner 代入和
  结构匹配，并稳定记录 owner、可选 fit、方法、owner 实例类型、显式方法类型实参及目标
  callable。Codegen 只消费这些已解析事实；本地闭合来源进入既有 thin wrapper，跨包泛型
  来源进入与直接调用相同的 exported shared dispatch，均不在生成期或运行时按名称重选。
- **运行时成本**：此前所有合法路径没有新增运行时指令、分支、分配或查找。新支持的完全
  闭合静态来源使用编译期生成的 immortal callable singleton；形成时只取得其静态地址，
  不分配 closure、不捕获 receiver/subject/witness，也不执行运行时成员或 fit 查找。
- **ABI 与格式**：没有修改 runtime、runtime 私有 ABI、公开 ABI、结构布局、字段、枚举值、
  `.ft` 记录尺寸或版本；静态方法值依赖复用既有 callable dependency 记录。
- **专项测试**：`build/bin/test_semantic`、`build/bin/test_codegen`、`build/bin/test_symbol`
  均通过；覆盖 type/fit/builtin-fit、泛型 owner、显式泛型方法、共享泛型体、目标缺失、
  签名不匹配、歧义、seal、fit 可见性及 `.ft` 往返，生成 C 通过严格编译。
- **FCTS**：覆盖保存、传参、返回、显式转换，本地与 imported type/fit，普通与泛型 owner/
  方法、provider shared-body dependency 及独立 friend 类型实现上下文；最终结果为
  `836 passed, 0 failed, 0 skipped`。
- **friend P01**：已恢复字段初始化、构造函数和终结器中的 `Vault.readShared` 静态方法值，
  复用同一 friend 访问检查，没有 friend 专用方法值路径。
- **全量回归**：沙箱外完整 `make test` 通过，包含 UBSan、普通 `-O2 -Werror`、两轮
  91/91 smoke、CLI、stdlib、两轮 FCTS 836/836、性能约束、增量构建与发布脚本测试。
- **实施问题**：ISSUE-005、ISSUE-006、ISSUE-007、ISSUE-008 均已按先记录、后分析、
  再修复的流程解决并完成专项及全量验证；MV03 无未解决问题。
- **建议 commit message**：`feat: support concrete static method values`

### ISSUE-009：MV04 跨包恢复需要启用既有 `SPEC_STATIC_METHOD` 依赖 kind

- **关联分项**：MV04
- **状态**：已解决
- **最小复现**：

  ```feng
  // provider
  open spec Factory {
    static func create(seed: int): string;
  }

  open spec Creator(seed: int): string;

  open func bind<T: Factory>(): Creator {
    return T.create;
  }

  // consumer
  type LocalFactory: Factory {
    static func create(seed: int): string {
      return "local";
    }
  }

  let creator: Creator = bind<LocalFactory>();
  ```

- **实际结果**：MV04 尚未写入产品实现。现有 AST/semantic 枚举已经包含
  `FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD`，既有 `FT_SEC_CALLABLE_DEPS` 记录也已有
  可承载来源 kind、requirement 符号、caller 视角 owner `T` 和目标 callable 类型的全部
  字段；但 `.ft` reader 的合法 kind 集合明确拒绝 `SPEC_STATIC_METHOD`，import 恢复也只
  重建 `SPEC_METHOD` 实例 requirement。因此一旦 Semantic 为公开共享泛型 `bind` 产出
  MV04 依赖，provider 可以按现有记录布局写出该 kind，consumer 会在读取 `.ft` 时拒绝。
- **期望结果**：provider/consumer 保留同一个静态 requirement 身份；consumer 为每个闭合
  `T` 生成一个绑定该具体 descriptor/witness 槽的 immortal callable singleton，provider
  共享体形成值时只读取既有 reified callable dependency 槽，不分配 closure、不做运行时
  成员搜索或重载选择。
- **根因**：`SPEC_STATIC_METHOD` 已作为编译器内部枚举存在，但从未进入 `.ft` callable
  dependency 的完整读入和 import 重建路径；MV04 首次需要跨包传递这一类别。它不需要新增
  二进制记录能力，却会把 reader 当前拒绝的 kind 变为合法输入，因此属于 `.ft` 合法值域/
  兼容边界扩展，而不仅是同一编译中的 Codegen 补齐。
- **通用修复方案（建议）**：保持 `FT_SEC_CALLABLE_DEPS` 布局、字段、枚举数值、section、
  格式版本全部不变；让 reader 接受既有 `SPEC_STATIC_METHOD` 数值，并在统一 import 恢复分支
  校验目标为 object-form spec 的静态方法 requirement，原样恢复 owner `T`、目标 callable
  与 requirement 身份。Semantic、依赖收集和 Codegen 均按独立的 spec-static 类别消费，
  不伪装为 type/fit static 或 spec instance method。
- **备选方案及结论**：
  1. 不启用该 kind：只能放弃公开共享泛型跨包场景，无法满足 MV04 的独立交付标准。
  2. 在共享体运行时捕获当前 type descriptor：需要动态 closure 分配；违反无增量运行时
     开销规则。
  3. 把静态 callable 缓存加入 type/runtime descriptor：需要 ABI/runtime 变更；违反强制
     规则。
  因此不存在同时保持当前 `.ft` 合法值域、零新增运行时分配和完整跨包交付的通用方案。
- **运行时性能影响**：建议方案不改变任何既有合法路径。MV04 新形成路径不分配、不查找、
  不分支；共享体只读取一个既有 callable dependency 槽并取得其中的静态 singleton，调用
  直接进入编译期固定的 witness 槽。
- **runtime ABI / `.ft` / 兼容性影响**：runtime ABI、公开 ABI、`.ft` 记录布局、字段、枚举
  数值和格式版本均不变；但 `.ft` callable dependency 的合法 kind 集合会新增接受既有
  `SPEC_STATIC_METHOD`。新编译器继续读取旧产物；旧编译器不能读取实际含该 MV04 依赖的
  新产物，未使用 MV04 的产物不受影响。
- **人工决策**：已批准启用既有 `SPEC_STATIC_METHOD` callable dependency kind。批准范围
  仅包括上述合法值域/兼容边界扩展；不得修改 `.ft` 布局、字段、枚举数值、格式版本、
  runtime ABI 或公开 ABI，也不得为 MV04 增加运行时分配、查找或分支。
- **专项验证结果**：Symbol `.ft` 往返与 provider/consumer FCTS 均通过；恢复后的依赖保留
  `SPEC_STATIC_METHOD`、原 requirement、owner `T` 和目标 callable 身份。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV04 独立交付记录。

### ISSUE-010：MV04 闭合时不能把子约束实参直接解释为 requirement 声明实参

- **关联分项**：MV04
- **状态**：已解决
- **最小复现**：

  ```feng
  spec Factory<T> {
    static func create(value: T): T;
  }

  spec MappedFactory<Unused, Value>: Factory<Value> {}
  spec IntCreator(value: int): int;

  func bind<T: MappedFactory<string, int>>(): IntCreator {
    return T.create;
  }
  ```

- **实际结果**：Semantic 能按既有父成员闭包选择 `Factory<int>.create`，但 MV04 初版
  Codegen 闭合辅助函数以 requirement 原声明 `Factory<T>` 为目标，直接读取完整约束
  `MappedFactory<string, int>` 的顶层实参；两者泛型形状不同，因而无法取得已注册 witness
  surface。
- **期望结果**：闭合阶段先解析 `T` 声明的完整约束实例
  `MappedFactory<string, int>`，再从该约束已经
  注册的继承成员面按 Semantic 提供的 requirement 原声明身份取得静态槽；不得重新按名称
  选择成员。
- **根因**：闭合辅助函数混淆了“泛型参数持有的完整约束 surface”和“选中 requirement
  的原声明 spec”。前者决定 descriptor 中 witness 的实际结构，后者只决定稳定成员身份。
- **通用修复方案**：对代入后的完整约束类型引用复用通用类型解析，取得其 `UserSpec`；
  继续使用 `cg_user_spec_member_by_decl` 在该 surface 中定位已注册的原 requirement。实例与
  静态受约束方法值共用这一辅助函数，不新增父 spec 或方法名特判。
- **运行时性能影响**：仅调整编译期闭合和生成选择；生成代码不增加分配、查找、分支或
  调用层。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：不需要；修复复用既有完整约束解析和稳定成员身份，未触及强制
  停止边界。
- **专项验证结果**：Semantic、Codegen 与本地/跨包 FCTS 的映射父约束场景均通过；闭合后
  使用完整 `MappedFactory<string, int>` witness surface 和原 `Factory<int>.create` 身份。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV04 独立交付记录。

### ISSUE-011：静态 spec requirement 的 builtin fit 实现在闭合 descriptor 时缺失

- **关联分项**：MV04；可能同时影响既有静态 requirement 直接调用
- **状态**：已解决
- **最小复现**：

  ```feng
  spec Factory {
    static func create(seed: int): int;
  }

  fit int: Factory {
    static func create(seed: int): int {
      return seed + 1;
    }
  }

  spec Creator(seed: int): int;

  func bind<T: Factory>(): Creator {
    return T.create;
  }

  let creator = bind<int>();
  ```

- **实际结果**：Semantic 接受该 fit 与方法值；FCTS consumer 闭合 `bind<int>()` 时 Codegen
  报 `CE0319: missing implementation for spec member 'create'`。独立探针把方法值改成
  `direct<T: Factory>(seed) { return T.create(seed); }` 后，`direct<int>(1)` 得到同一
  `CE0319`，确认问题早于 MV04，属于既有直接调用 descriptor/witness 基线缺口。用户 type
  的可见 fit 静态实现走另一条既有 witness 路径，不受该缺口影响。
- **期望结果**：builtin fit 既然已被 Semantic 接受为静态 spec requirement 的实现，直接
  调用和方法值应复用同一个已闭合 witness；static requirement 的 witness ABI 不含 subject。
- **根因**：`compute_spec_witness_if_absent` 为 builtin/array 等非 type subject 收集 fit 方法
  时，把 `require_static` 固定传为 `false`，导致静态实现没有写入 semantic witness；即使补齐
  该选择，现有 non-type witness Codegen 仍按实例方法固定生成 `_subject` 并向 fit wrapper
  传 self，尚未实现静态 requirement 已有的无 subject ABI。
- **通用修复方案**：在独立 bugfix 中让 non-type witness 收集按 `sm->is_static` 选择候选，
  并把 subject-independent 静态 witness thunk 抽取为 type/user-fit/builtin-fit 可复用路径；
  静态槽直接调用已经注册的 fit 静态 wrapper，不在 MV04 callable 形成处搜索 fit 或伪造
  witness。实例槽维持现状。
- **运行时性能影响**：不会增加任何现有合法路径的运行时开销；新合法静态路径与 type
  静态 requirement 一样使用编译期固定 witness 槽和直接 wrapper 调用，不分配、不查找。
- **runtime ABI / `.ft` / 兼容性影响**：无；复用既有 static requirement witness ABI、fit
  wrapper ABI 和 descriptor 布局。
- **人工决策**：已批准本次一并修复该既有直接调用 Bug。修复必须位于通用 semantic
  witness / Codegen static thunk 路径；不得以 callable 专用特判绕过，不得修改 ABI 或增加
  运行时分配、查找和分支。
- **专项验证结果**：独立 Semantic 与 Codegen 用例验证 builtin fit 的直接调用和 MV04
  方法值共用 receiver-free 静态 witness；完整 FCTS 为 840/840。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV04 独立交付记录。

### ISSUE-012：受约束泛型闭合遗漏数组类型实参的 structured subject key

- **关联分项**：MV04 的数组 subject 补充验证；属于既有受约束泛型闭合缺口。
- **状态**：已解决。
- **最小复现**：

  ```feng
  spec Factory {
    static func create(seed: int): int;
  }

  spec Creator(seed: int): int;

  fit int[]: Factory {
    static func create(seed: int): int { return seed + 1; }
  }

  func bind<T: Factory>(): Creator {
    return T.create;
  }

  let creator = bind<int[]>();
  ```

- **实际结果**：Semantic 已接受 `int[]` 满足 `Factory`，但 Codegen 初始在闭合受约束泛型
  实参时报告 `CE0294: constrained generic type argument currently requires ... concrete builtin
  type`。补齐 Codegen structured key 后，路径继续前进并报告 `CE0329: missing semantic witness
  for constrained array type argument`，确认 Semantic 的闭合 witness 物化也遗漏了数组 subject。
- **期望结果**：数组是普通合法类型实参；受 object-form spec 约束时，应以既有
  `FENG_SEMANTIC_SUBJECT_KEY_ARRAY` 查找并生成 witness，然后继续复用现有数组 type descriptor。
- **根因**：第一层，`cg_generic_descriptor_expr` 的约束 witness 选择已分别覆盖 user type、
  enum、spec value 和 canonical builtin，却把其余类型统一交给
  `cg_builtin_canonical_name_for_kind`。数组本来具有独立 structured subject key，不能转换为
  canonical builtin 名称，因而在后续已经支持数组 descriptor 的分支之前被 `CE0294` 提前拒绝。
  第二层，闭合泛型调用的 Semantic witness demand 尚未把数组类型实参转换为同一 structured
  subject key，所以 Analysis 中不存在可供 Codegen 消费的 `(int[], Factory)` witness。
- **通用修复方案**：在约束 witness 选择层把 `CG_TYPE_ARRAY` 转回完整数组 type ref，通过
  `feng_semantic_subject_key_init_array_from_type_ref` 构造既有 structured key，再调用统一的
  `cg_ensure_witness_instance`；不得按元素类型、fit 名称或本用例特判。数组 descriptor、witness
  ABI 和 non-type thunk 均继续复用现有实现。
- **运行时性能影响**：不改变任何既有合法路径；新合法数组闭合路径只生成并传递既有静态
  descriptor/witness 地址，不增加运行时分配、查找、分支或调用层。
- **runtime ABI / `.ft` / 兼容性影响**：无；不修改结构、字段、枚举、格式或版本。
- **人工决策**：已批准本次一并修复；修复必须位于受约束泛型 descriptor 的统一 array
  subject-key 路径，不得为 MV04 callable、具体元素类型或测试声明增加特判。
- **专项验证结果**：独立 Semantic 与 Codegen 用例确认数组 structured subject key、witness
  物化、直接调用和 MV04 方法值均通过；完整 FCTS 为 840/840。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV04 独立交付记录。

### ISSUE-013：MV04 固定聚合返回值的新增 Codegen 断言错误

- **关联分项**：MV04 聚合返回值专项验证。
- **状态**：已解决。
- **最小复现**：运行新增的 `test_constrained_generic_spec_static_method_value_codegen`。
- **实际结果**：Semantic 与生成阶段成功；测试未在生成 C 中找到预期的
  `_witness->pair(_out)` 调用文本。
- **期望结果**：静态 callable adapter 应遵循既有声明槽 ABI 调用 requirement witness，不引入
  subject、分配或额外运行时分派。
- **根因**：新增断言把“聚合返回”误等同于 address ABI。既有
  `cg_callable_decl_slot_abi_kind` 只让开放的泛型依赖声明槽使用 address ABI；`Pair` 是不含类型
  参数的固定 nominal type，正确形式是按值 `return _witness->pair();`。
- **通用修复方案**：保持产品实现不变，把本次新增测试的错误文本断言改为固定类型的既有直接
  返回形式；生成 C 编译验证继续负责 ABI 一致性。
- **运行时性能影响**：无产品变更，无增量运行时开销。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **人工决策**：不需要；仅修正本次新增且尚未交付的错误测试断言，不修改任何既有用例。
- **专项验证结果**：修正后的 Codegen 专项与生成 C 严格编译通过。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### ISSUE-014：MV04 泛型父 requirement 的新增 Codegen 断言错误

- **关联分项**：MV04 映射父约束专项验证。
- **状态**：已解决。
- **最小复现**：运行新增的 `test_constrained_generic_spec_static_method_value_codegen`。
- **实际结果**：生成阶段成功；测试未在生成 C 中找到预期的 `_witness->map(_arg0)` 文本。
- **期望结果**：从 `MappedFactory<string, i32>` 继承的 `GenericFactory<i32>.map` 应通过已闭合的
  requirement witness 路径调用，无 subject、运行时查找或额外分派。
- **根因**：新增断言误把闭合后的 callable 参数 ABI 当成 requirement 声明槽 ABI。
  `MappedFactory<Unused, Value>` 的 `map` 槽依赖泛型参数，既有 witness ABI 必须保持 address
  形式；闭合为 `i32` 后，adapter 正确生成
  `_witness->map((const void *)&_arg0, &_result)` 并返回 `_result`。
- **通用修复方案**：保持产品实现和 witness 布局不变，把本次新增测试改为验证既有 direct-to-address
  ABI bridge 及其结果返回。
- **运行时性能影响**：无产品变更，无增量运行时开销。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **人工决策**：不需要；仅修正本次新增且尚未交付的错误测试断言，不修改任何既有用例。
- **专项验证结果**：修正后的 Codegen direct-to-address bridge 探针与生成 C 严格编译通过。
- **全量回归结果**：沙箱外完整 `make test` 通过。

### MV04 独立交付记录

- **变更范围**：补齐 `T: ObjectSpec` 类型参数静态方法值的函数、spec、泛型、可见性、诊断
  和符号表规范，以及 Semantic、共享泛型依赖、跨包恢复与 Codegen；没有扩大实例/静态成员
  访问边界，也没有改变实例访问静态成员或类型访问实例成员的既有禁止规则。
- **实现结果**：Semantic 复用静态直接调用的约束闭包、父 spec 投影、访问过滤、闭合签名和
  requirement 身份，并把 caller 视角 owner 保持为 `T`。闭合点从具体 type descriptor 取得
  既有 witness 槽，生成 receiver-free immortal callable singleton；共享泛型体只读取既有
  reified callable dependency 槽，不按名称重选成员。
- **一并修复**：经人工批准，ISSUE-011 让 builtin/array 等 non-type subject 的静态 fit
  requirement 复用统一 receiver-free witness thunk；ISSUE-012 让数组类型实参通过既有
  structured subject key 完成 Semantic witness 物化和 Codegen descriptor 闭合。
- **运行时成本**：此前所有合法路径没有新增运行时指令、分支、分配或查找。新支持路径的
  callable 在编译期静态生成，形成时只取得 singleton 地址；不捕获 receiver/subject、
  不分配或释放动态 closure，调用直接进入编译期固定的 witness 槽。
- **ABI 与格式**：没有修改 runtime/private/public ABI、结构布局、字段、枚举数值、`.ft`
  记录尺寸或格式版本。ISSUE-009 按人工批准仅让 reader/import 接受并恢复既有枚举值
  `SPEC_STATIC_METHOD`；旧 reader 仍可读取未使用该依赖的新产物，但可拒绝实际包含该依赖的
  新产物。
- **专项测试**：`build/bin/test_semantic`、`build/bin/test_codegen`、`build/bin/test_symbol`
  均通过；覆盖公开/seal requirement、非法访问、目标缺失、签名不匹配、父约束映射、固定与
  泛型依赖 ABI、type/fit/builtin-fit/array subject、`.ft` 往返及生成 C 严格编译。
- **FCTS**：覆盖保存、传参、返回、显式转换、直接调用、本地 type/builtin fit/array fit、
  friend seal、含托管叶子的聚合返回，以及 provider shared body 闭合 consumer type 与映射
  父 requirement；最终结果为 `840 passed, 0 failed, 0 skipped`。
- **全量回归**：沙箱外完整 `make test` 通过；UBSan 与普通 `-O2 -Werror` 两阶段均完成，
  两轮 91/91 smoke、两轮 FCTS 840/840，以及 CLI、stdlib、性能约束、增量构建、发布脚本、
  bundled packages 和 toolchain 测试全部通过。
- **实施问题**：ISSUE-009 至 ISSUE-014 均已按先记录、后分析、再修复的流程解决；MV04 无
  未解决问题。
- **建议 commit message**：`feat: support constrained spec static method values`

### ISSUE-015：intersection 方法引用缺少 callable 目标时未进入通用诊断

- **关联分项**：MV05
- **状态**：已解决
- **最小复现**：

  ```feng
  spec Readable { func read(offset: int): string; }
  spec Traceable { func trace(): string; }
  spec Both: Readable & Traceable;

  func run(value: Both) {
    let reader = value.read;
  }
  ```

- **实际结果**：Semantic 接受该绑定，没有产生诊断。
- **期望结果**：Feng 不推导匿名 callable 类型；`value.read` 缺少明确 callable-form
  `spec` 目标，必须稳定产生 `AE0523`。
- **根因**：通用未绑定 callable 引用分类最终通过
  `count_accessible_method_overloads` 判断成员表达式是否指向方法；该查询只遍历 object-form
  spec 的父闭包，对 intersection-form 直接返回零。于是后续无目标校验把表达式误判为普通
  成员值。
- **通用修复方案**：让同一成员引用分类查询复用 intersection 已有的 flattened member
  metadata，遍历各 object-form member 的父闭包并识别可访问实例方法；不在 `AE0523`
  报告点增加方法名或 intersection 名称特判。
- **运行时性能影响**：无；只补齐 Semantic 编译期来源分类，生成程序和运行时路径不变。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；该修复直接落实已批准的“无匿名 callable 类型”和 MV05
  诊断边界，不触及强制停止条件。
- **专项验证结果**：新增 Semantic 反例稳定产生 `AE0523`；完整 Semantic 专项测试通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV05 独立交付记录。

### ISSUE-016：新增 Codegen 用例对 merged witness 调用文本的断言不匹配

- **关联分项**：MV05
- **状态**：已解决
- **最小复现**：新增泛型 intersection receiver `Both<int>` 分别形成 `value.read` 与
  `value.trace` 方法值，并检查生成 C 中的直接 witness 槽调用。
- **实际结果**：Semantic 与 Codegen 均成功，生成 C 也已产出；新增测试对
  `_bound->_witness->read(_bound->_self, _arg0)` 的完整文本断言失败。
- **期望结果**：确认生成代码仍直接调用已选 merged witness 槽，并让新增测试只断言该
  稳定语义事实，不错误绑定到允许变化的 ABI bridge 表达式文本。
- **根因**：`Readable<T>.read(T): T` 的 witness 槽必须保持开放泛型 requirement 已有的
  address ABI；即使 receiver 闭合为 `Both<int>`，callable 目标的直接 `int` ABI 仍由
  adapter 静态桥接为 `(const void *)&_arg0` 和 `&_result`。新增断言误把它写成了非泛型
  requirement 的直接参数/返回文本。
- **通用修复方案**：只修正本次新增断言，使其验证 merged witness 的 `read` 槽被直接
  调用且沿用既有静态 ABI bridge；产品实现不需要变更。`trace` 的非泛型槽继续验证直接
  参数调用。
- **运行时性能影响**：无；没有修改生成代码，静态 ABI bridge 也是该泛型 requirement
  直接调用既有的参数/返回适配。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；只纠正新增测试对既有 ABI 的错误假设，没有修改既有用例。
- **专项验证结果**：修正新增断言后，完整 Codegen 专项测试通过，生成 C 严格编译通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV05 独立交付记录。

### ISSUE-017：intersection 合法重载的第二个 requirement 未保留到 Codegen 成员表

- **关联分项**：MV05
- **状态**：已解决
- **最小复现**：

  ```feng
  spec Left { func select(offset: int): int; }
  spec Right { func select(label: string): string; }
  spec Both: Left & Right;
  spec StringMapper(label: string): string;

  func bind(value: Both): StringMapper {
    return value.select;
  }
  ```

- **实际结果**：Semantic 按 callable 目标正确选择 `Right.select(string)`；Codegen 随后
  报 `CE0114: resolved spec method-value requirement was not registered`。
- **期望结果**：intersection 的 merged member/witness 表同时保留两个参数签名不同的合法
  重载槽；方法值按 Semantic 保存的 requirement 声明身份直接取得 `Right.select` 槽。
- **根因**：Semantic 的 intersection 成员面以 requirement 声明和闭合签名为身份，允许
  同名但参数签名不同的合法重载；Codegen 注册 merged witness 时却调用
  `cg_user_spec_has_member_name` 按 Feng 成员名去重，`UserSpecMember.c_field_name` 也只由成员名
  生成。于是 `Left.select(int)` 先占据唯一的 `select` 槽，`Right.select(string)` 在成员表和
  witness 结构中均被丢弃。该缺口早于方法值存在：intersection 直接调用虽由 Semantic 保存
  精确 requirement，当前普通 spec 调用 Codegen 仍按名称取第一个槽；MV05 的按声明身份查询
  只是首次把缺失稳定暴露为 `CE0114`。
- **通用修复方案**：统一修正 intersection merged member/witness 建模，而不在方法值路径增加
  补偿逻辑：按 Semantic 已确定的 requirement 等价类去重，仅合并同一闭合参数/返回签名的
  等价 requirement；参数签名不同的合法重载各自保留一个成员槽，并生成确定、无冲突的 C
  槽名。直接调用与方法值均使用 Semantic 保存的 requirement 声明身份取得同一稳定槽。merged
  witness 仍在编译期静态组装，并直接复制对应 member-spec witness 的函数指针，不增加运行时
  查找、动态分派、分支、分配或额外 spec 视角。
- **运行时性能影响**：建议方案不增加任何执行路径上的指令、分支、查找或分配；合法重载的
  merged witness 常量会静态多保存必要的函数指针槽。试图在不增加槽的前提下修复只能改为
  运行时按来源 witness 查找、额外捕获或分派，均违反本专项的零增量运行时开销规则。
- **runtime ABI / `.ft` / 兼容性影响**：`.ft` 语义事实、记录布局和格式版本无需变化；runtime
  及 runtime 私有 ABI 也无需变化。但正确修复会改变已被 Semantic 接受的 intersection 合法
  重载类型的编译器生成 witness 结构布局：原来错误地只有首个同名槽，修复后会追加其余合法
  重载槽并使用唯一字段名。即使保留首槽的名称和偏移，这仍会改变 witness 结构尺寸以及新旧
  编译产物之间的 Feng 私有链接契约，属于第 1 节强制规则禁止自行实施的现有 ABI 变更。
- **是否需要人工决策**：已由人工批准限定修复。无法同时满足“合法重载完整可调用”和“现有 merged witness
  ABI 完全不变”；也没有既不变更 ABI、又不增加运行时开销的通用实现。建议批准仅针对
  intersection 合法重载缺失槽的 ABI 完整性修复，并要求保持既有首槽偏移、`.ft` 格式和
  runtime ABI 不变。本次批准范围即为上述 merged witness ABI 完整性修复，并包含让直接调用
  与方法值共用 Semantic 保存的精确 requirement；不得扩大到其他 ABI 或运行时机制。
- **专项验证结果**：新增 Codegen 用例确认首槽及既有后续槽顺序不变、合法重载槽追加、
  merged witness 直接引用 leaf witness，且方法值和直接调用均使用精确槽；完整 Codegen、
  Semantic、Symbol 专项测试及 FCTS `844/844` 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV05 独立交付记录。

### ISSUE-018：intersection 成员的 owned type ref 未在 Codegen 销毁时释放

- **关联分项**：MV05 / ISSUE-017 编译期成员登记。
- **状态**：已解决。
- **最小复现**：编译任一泛型 intersection 实例，使
  `cg_user_spec_clone_intersection_member` 为成员保存 `source_member_type_ref`，随后销毁 Codegen
  上下文。
- **实际结果**：`UserSpecMember.source_member_type_ref` 的字段注释和克隆函数均明确由成员取得
  所有权；`cg_free` 释放成员时却没有调用 `cg_type_ref_free`。
- **期望结果**：Codegen 上下文销毁时释放每个成员拥有的 substituted type ref；ISSUE-017
  新增的合法重载槽不能扩大该编译期泄漏。
- **根因**：intersection 泛型成员信息加入 `UserSpecMember` 后，成员销毁路径没有同步补齐该
  owned 字段；运行期对象和生成程序不涉及此内存。
- **通用修复方案**：在统一的 `UserSpecMember` 销毁循环中释放
  `source_member_type_ref`；ISSUE-017 新增的 requirement 声明别名数组也在同一位置释放。二者
  都只保存编译期元数据，不改变生成代码。
- **运行时性能影响**：无；只减少编译器上下文销毁时遗留的堆内存，不改变生成程序。
- **runtime ABI / `.ft` / 兼容性影响**：无；不修改任何运行时、生成结构、符号表记录或格式。
- **是否需要人工决策**：否；这是 owned 编译期字段与既有销毁契约不一致的资源释放修复，
  不触发本专项停止条件。
- **专项验证结果**：统一成员销毁函数已覆盖 owned substituted type-ref 与声明别名数组；
  编译器以 `-Werror -pedantic` 构建，Semantic、Codegen、Symbol 专项测试通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV05 独立交付记录。

### ISSUE-019：新增重载 Codegen 探针的来源槽断言不匹配

- **关联分项**：MV05 / ISSUE-017 专项验证。
- **状态**：已解决。
- **最小复现**：运行新增的
  `test_intersection_spec_overload_codegen_preserves_exact_slots`，查找生成 C 中首个
  `.select__feng_overload_2 =` 初始化行并要求其包含 `__Textual.select`。
- **实际结果**：Semantic、Codegen 和此前 FCTS 执行成功；witness 结构的首槽、既有后续槽和
  追加重载槽顺序断言均通过，但上述来源文本断言失败。
- **期望结果**：确定该行实际属于哪个生成结构，并确认 merged witness 的追加槽直接引用
  `Textual.select`；若只是全局首个文本定位不精确，只修正本次新增探针。
- **根因**：生成 C 中同名 designated initializer 不只出现在具体 subject 的 merged witness
  常量中；交叉 spec 的默认 witness 初始化更早生成同名行。新增探针从整个文件取第一次文本
  命中，实际抓到的是
  `FengSpecDefaultWitness__...__Both__select__feng_overload_2`，不是待验证的 leaf witness 引用。
- **通用修复方案**：保持产品实现不变，让本次新增探针逐行查找同名 initializer，直到同一行
  同时包含 `__Textual.select`；witness 结构顺序、方法值槽和直接调用槽仍分别独立断言。
- **运行时性能影响**：无产品变更，无增量运行时开销。
- **runtime ABI / `.ft` / 兼容性影响**：无；只修正本次新增测试的定位条件。
- **是否需要人工决策**：否；不修改既有用例，不改变产品代码、ABI 或格式。
- **专项验证结果**：修正新增探针后，完整 Codegen 专项测试通过，生成 C 严格编译通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV05 独立交付记录。

### ISSUE-020：泛型 intersection 直接成员 type-ref 克隆失败未被识别

- **关联分项**：MV05 / ISSUE-017 泛型 merged member 登记。
- **状态**：已解决。
- **最小复现**：编译泛型 intersection 实例，在登记直接 member-spec 的 merged member 时，
  `cg_type_ref_clone(sub)` 因内存不足返回 `NULL`。
- **实际结果**：现有失败判断只覆盖嵌套 intersection 已带
  `source_member_type_ref` 的分支；直接 member-spec 分支会把克隆失败得到的 `NULL` 继续传入，
  后续错误地退化为仅按声明查找来源 spec。
- **期望结果**：由于被克隆的 `sub` 必定非空，任一分支返回 `NULL` 都应立即稳定报告
  `IE0001`，不能以缺失的泛型来源事实继续生成代码。
- **根因**：原判断把“是否来自嵌套 intersection”误当成“克隆结果是否必须非空”的条件，
  漏掉了直接 member-spec 对同一 owned type-ref 的克隆失败。
- **通用修复方案**：统一要求 `type_ref_for_clone != NULL`；失败时走既有 Codegen OOM 清理与
  `IE0001` 路径，不按成员来源增加分支特判。
- **运行时性能影响**：无；仅修正编译器内存不足路径，生成程序不变。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；该修复不改变正常语义、运行时路径或 ABI，也不修改既有用例。
- **专项验证结果**：登记循环已统一检查两类来源的 type-ref 克隆结果；编译器构建及完整
  Codegen 专项测试通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV05 独立交付记录。

### MV05 独立交付记录

- **变更范围**：补齐显式 intersection-form `spec` 参数/局部值的实例方法值；Semantic、
  Codegen、诊断、可见性和权威语言规范保持同一边界。`T: IntersectionSpec` 的泛型值方法值
  仍属于 MV06，本次没有提前开放。
- **实现结果**：方法值复用 intersection 直接调用的 flattened requirement 面和 merged
  witness，按 Semantic 保存的精确 requirement 声明选择槽位；直接成员、object 父 spec、
  嵌套 intersection、等价 requirement 去重及合法重载均走同一机制。receiver 在形成点只求值
  一次并绑定既有 subject 与 witness，不构造额外 object-spec 视角或运行时成员索引。
- **一并修复**：ISSUE-015 补齐无 callable 目标的 `AE0523`；ISSUE-017 按人工批准补全合法
  重载 merged witness 槽，并让直接调用与方法值共用精确槽；ISSUE-018 补齐编译期 owned
  metadata 销毁；ISSUE-020 修正泛型来源 type-ref 克隆失败路径。ISSUE-016、ISSUE-019 仅
  修正本次新增测试对生成文本的错误定位。
- **运行时成本**：相对于 object-form spec 方法值没有新增机制；仍只进行形成 callable 所必需
  的一次 closure 分配，捕获既有 subject 并借用既有 witness，调用直接进入编译期固定槽。
  没有新增运行时查找、分支、装箱、第二份 spec 视角或额外分配。合法重载仅静态追加人工批准
  的必要 witness 函数指针槽，不增加执行路径指令。
- **ABI 与格式**：`.ft` 记录、格式版本、runtime 及 runtime 私有 ABI 均未改变。唯一 ABI
  变化是人工批准的 intersection 合法重载 merged witness 完整性修复：保留所有既有槽名、
  顺序和偏移，只在末尾追加此前被错误丢弃的合法重载槽，并为其生成确定的唯一 C 字段名。
- **专项测试**：`build/bin/test_semantic`、`build/bin/test_codegen`、`build/bin/test_symbol` 均
  通过；新增用例覆盖直接/父级/嵌套来源、等价签名去重、合法重载的精确方法值与直接调用、
  缺少目标、签名不匹配、seal 访问、merged witness 布局及生成 C 严格编译。没有修改既有
  测试用例，只增加专项用例与注册入口。
- **FCTS**：覆盖形成、传参、显式转换、receiver 单次求值、重新赋值后稳定绑定、逃逸生命
  周期、等价 requirement、两类合法重载，以及 imported provider/consumer 的方法值和直接
  调用；结果为 `844 passed, 0 failed, 0 skipped`。
- **全量回归**：沙箱外完整 `make test` 通过；UBSan 与普通 `-O2 -Werror -pedantic` 两阶段
  均完成，两轮 smoke `91/91`、两轮 std `601/601`、两轮 FCTS `844/844`，以及 CLI、性能
  约束、增量构建、发布/安装脚本、bundled packages 和 toolchain 测试全部通过。
- **实施问题**：ISSUE-015 至 ISSUE-020 均已按先记录、后分析、再修复的流程解决；MV05 无
  未解决问题。
- **建议 commit message**：`feat: support intersection spec method values`

### ISSUE-021：无目标的约束泛型实例方法引用漏过 Semantic

- **关联分项**：MV06；同时覆盖 MV02 的同类诊断路径。
- **状态**：已解决。
- **最小复现**：

  ```feng
  spec Left {
    func select(value: int): int;
  }

  spec Right {
    func trace(): int;
  }

  spec Both: Left & Right;

  func bind<T: Both>(value: T): void {
    let selected = value.select;
  }
  ```

- **实际结果**：Semantic 未报告诊断，表达式进入 Codegen 后才以
  `CE0173: spec 'Both' has no field 'select'` 失败。
- **期望结果**：`value.select` 是尚未绑定的实例方法引用；Feng 不推导匿名 callable
  类型，因此必须在 Semantic 稳定报告 `AE0523`，不得进入 Codegen。
- **根因**：无目标 callable 引用分类在处理普通 receiver 后只按 `owner type decl` 统计
  方法重载；泛型参数 receiver 没有具体 owner decl，该路径没有复用既有
  `find_generic_param_spec_member_surface` 约束成员查询。目标明确的方法值解析则已经单独
  读取 object-form 约束，二者因此出现不一致。
- **通用修复方案**：在无目标实例成员引用分类中，当 receiver 是受 spec 约束的泛型参数
  时，复用直接调用和字段访问已使用的 `find_generic_param_spec_member_surface`；只要约束
  合并成员面中的可访问成员是实例方法，就把表达式分类为 callable value reference。
  该方案同时覆盖 object-form、intersection-form、父成员和嵌套 intersection，不按名称
  或 spec form 增加特判。
- **运行时性能影响**：无；只补齐 Semantic 编译期诊断查询，不改变合法程序生成代码。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；修复使无目标诊断复用既有约束成员面，不触及强制规则。
- **专项验证结果**：新增 Semantic 用例确认 object-form 与 intersection-form 约束泛型的无目标
  实例方法引用均稳定报告 `AE0523`；`build/bin/test_semantic` 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-022：MV06 非法访问夹具误用保留字作为 module 段

- **关联分项**：MV06 专项 Semantic 诊断验证。
- **状态**：已解决。
- **最小复现**：新增用例声明
  `module demo.generic_intersection_method_value.seal;`。
- **实际结果**：Parser 在 `seal` 处按规范报告“qualified name 的 `.` 后需要标识符”，
  专项测试在进入预期的 `AE0708` Semantic 检查前终止。
- **期望结果**：夹具本身应使用合法 module 名，使测试只验证 intersection 约束泛型方法值
  的非法 seal requirement 访问。
- **根因**：新增测试把 Feng 保留字 `seal` 误作 module 名段；与 MV06 实现无关。
- **通用修复方案**：仅把该新增夹具的 module 末段改为合法标识符 `seal_access`，不修改
  测试语义、断言或任何既有用例。
- **运行时性能影响**：无。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；只修正新增夹具的非法语法。
- **专项验证结果**：新增夹具已使用合法 module 名并进入预期 Semantic 访问检查；
  `build/bin/test_semantic` 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-023：约束泛型方法值的非法 seal 访问被降格为签名不匹配

- **关联分项**：MV06；约束泛型实例方法值的访问诊断一致性。
- **状态**：已解决。
- **最小复现**：

  ```feng
  spec Hidden {
    seal func secret(value: int): int;
  }

  spec Visible {
    func visible(): int;
  }

  spec Both: Hidden & Visible;
  spec Mapper(value: int): int;

  func bad<T: Both>(value: T): Mapper {
    return value.secret;
  }
  ```

- **实际结果**：Semantic 报告 `AE0522`，表示来源不匹配目标 callable。
- **期望结果**：成员名与签名均匹配，唯一失败原因是访问点无权使用 `Hidden.secret`；应与
  显式 intersection receiver 和直接成员访问一致，报告既有 `AE0708`。
- **根因**：目标明确的方法值候选解析会先过滤不可访问 requirement。显式 spec receiver
  随后还会由普通成员表达式验证找到同名的不可访问成员并报告 `AE0708`；泛型参数 receiver
  在普通成员验证中只被视为“由后续约束分派处理”并直接通过，因此方法值解析只看到“无
  匹配候选”，最终错误降格为 `AE0522`。
- **通用修复方案**：让现有 spec 方法值 surface resolver 支持“应用访问过滤”和“保留其余
  签名规则但暂不应用访问过滤”两种编译期查询。正常候选选择继续使用前者；只有正常结果
  为 NONE 时，约束泛型 receiver 才用后者确认是否存在签名完全匹配但不可访问的唯一
  requirement，并调用既有 `report_inaccessible_spec_member`。该回落复用同一成员展平、
  owner 代入、目标签名和 requirement 原声明，不硬编码错误码或方法名，也不让不可访问
  候选参与正常重载选择。
- **运行时性能影响**：无；只在编译期失败路径增加一次受限查询，合法程序生成代码不变。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；修复恢复既有访问诊断边界，不触及强制规则。
- **专项验证结果**：新增 Semantic 用例确认签名匹配但不可访问的 seal requirement 报告
  `AE0708`，而真正的签名不匹配仍报告 `AE0522`；`build/bin/test_semantic` 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-024：MV06 三类 receiver Codegen 专项在生成阶段失败

- **关联分项**：MV06 Codegen 与 receiver 表示验证。
- **状态**：已解决。
- **最小复现**：新增 Codegen 专项以 `T: Stateful & Named` 的具名 intersection 约束形成
  `value.step` 与合法重载 `value.select` 方法值，并分别闭合为托管引用、`i32` 和含
  `string` 的 `@value` receiver。
- **实际结果**：Semantic 无诊断通过；`feng_codegen_emit_program` 报告 `CE0329`：
  `missing semantic witness for object-form spec coercion`。托管引用 receiver 尚未进入方法值
  闭包生成，首个基础类型 receiver 在为具名 intersection 约束取得 descriptor 时失败。
- **期望结果**：复用 MV02 的三类 receiver closure 与 MV05 merged witness 槽，生成 C
  成功并通过严格 C 编译。
- **根因**：Semantic 按 intersection satisfaction 规则为具体 subject 保存各叶级 object-form
  witness，不创建外层 intersection 的伪 witness。Codegen 已能为具体用户类型把这些叶级
  witness 的槽编译期合并为静态 merged witness；但基础类型和数组仍直接查找
  `(subject, intersection)` 的单一 Semantic witness，因此必然查找失败并报告 `CE0329`。
- **通用修复方案**：把现有用户类型专用的 intersection merged-witness 组装抽象为适用于
  任意 Semantic subject key 的编译期路径。按 merged member 保存的原始叶级 spec 与原始槽名，
  对同一个 subject 取得或生成叶级 witness，再生成并缓存相同布局的静态 merged witness。
  用户类型、基础类型和数组共同复用该逻辑；不新增 subject/spec/方法名特判，不新增运行时
  查找、box、descriptor 字段或闭包层。
- **运行时性能影响**：无；仅把编译期已经存在的静态 witness 槽引用组装能力覆盖到基础类型
  和数组，方法调用仍是现有 witness 槽的一次间接调用。
- **runtime ABI / `.ft` / 兼容性影响**：无；复用现有 witness struct 与 callable dependency
  表示，不修改 runtime、`.ft` 布局或公开 ABI。
- **是否需要人工决策**：否；修复补齐同一 intersection 规则在不同 subject 表示上的实现，
  未触及强制停止条件。
- **专项验证结果**：新增 Codegen 用例覆盖托管引用、trivial enum、descriptor-sized
  `@value` 与数组 receiver 的 merged witness 形成及严格 C 编译；`build/bin/test_codegen`
  通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-025：MV06 Codegen 专项的无 spec-box 断言失败

- **关联分项**：MV06 Codegen 与零增量运行时表示验证。
- **状态**：已解决。
- **最小复现**：ISSUE-024 修复后重新运行新增的三类 receiver Codegen 专项。
- **实际结果**：Semantic 与 Codegen 生成均成功。命中内容是 `@value ValueCounter` 注册时
  已有的 `ValueCounter__spec_box` 类型与 descriptor 前置声明，不是方法值形成产生的
  `feng_object_new` 分配。
- **期望结果**：方法值形成沿用 receiver 原有捕获表示，不因 intersection 约束新增
  object-spec box。
- **根因**：新增测试把“生成 C 中出现 spec-box 支持声明”误等同于“方法值形成分配了
  spec box”。value type 的既有 metadata 会独立生成该名称，因此全文件子串断言范围过宽。
- **通用修复方案**：与既有同类 Codegen 用例保持一致，只遍历实际
  `feng_object_new(&...)` 分配语句并断言其中不引用 `__spec_box`。同时保留 callable 分配
  次数、三类 receiver 分支和直接 merged-witness 槽调用断言，验证强度不降低。
- **运行时性能影响**：无；仅修正新增测试的观察边界，生成代码不变。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；不变更语言或实现语义，也不触及强制停止条件。
- **专项验证结果**：Codegen 专项只检查实际 `feng_object_new` 分配，确认各 receiver 的
  方法值形成没有分配 spec box，并确认 callable 分配次数与 merged-witness 槽调用；
  `build/bin/test_codegen` 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-026：MV06 FCTS 的基础类型 fit 与既有 FCTS 发生包级实现冲突

- **关联分项**：MV06 FCTS 与 trivial receiver 覆盖。
- **状态**：已解决。
- **最小复现**：在 `fcts_bin` 新增为 `i32` 实现 MV06 两个叶级 spec 的 fit 后运行完整
  `make fcts-tests`。
- **实际结果**：新增 `bindMV06Step<i32>` 与既有
  `bindGenericSpecMethod<i32>` 均报告 `AE0804`，指出各自要求的 `step` 存在多个可见实现。
- **期望结果**：新增 MV06 用例独立覆盖 trivial receiver，不改变既有 FCTS 的实现候选集合与
  结果。
- **根因**：`fcts_bin/src` 的测试文件共同属于 `fcts_bin` 模块；新增 `fit i32` 与既有
  `fit i32: GenericSpecMethodStateful` 都提供结构相同的 `step(i32): i32`，扩大了整个模块
  对同一基础 subject 的可见实现集合。这是新增 FCTS 数据互相污染，不是 MV06 语言规则缺口。
- **通用修复方案**：新增用例改用本文件独立声明的 nominal enum 作为 trivial receiver，并只
  为该 enum 定义 MV06 叶级 fit。enum 仍走 `FENG_VALUE_TRIVIAL` receiver 表示，同时隔离
  subject identity；不修改既有用例，也不调整编译器候选规则。
- **运行时性能影响**：无；仅隔离新增测试数据。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；不改变实现或规范，仅修正新增用例的模块级隔离。
- **专项验证结果**：新增 FCTS 使用独立 nominal enum 覆盖 trivial receiver，完整 FCTS
  `850 passed, 0 failed, 0 skipped`。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-027：MV06 enum 闭合仍丢失 nominal witness subject

- **关联分项**：MV06 Codegen 与 trivial enum receiver。
- **状态**：已解决。
- **最小复现**：将新增 FCTS 的 trivial receiver 隔离为 nominal enum
  `MV06TrivialCounter`，为该 enum 实现全部 MV06 叶级 spec，再调用
  `bindMV06Step<MV06TrivialCounter>`。
- **实际结果**：Semantic 通过，Codegen 在闭合调用点报告 `CE0329`：
  `missing semantic witness for object-form spec coercion`。
- **期望结果**：闭合路径保留 enum declaration subject identity，生成 enum 各叶级 witness 与静态
  merged witness，并按 trivial receiver 值捕获。
- **根因**：闭合点首次从显式类型参数生成 generic descriptor 时，`CGType` 正确保留
  `enum_decl`；随后为 reifiable callable dependency 把同一个 `CGType` 重建为 `FengTypeRef`
  时，`cg_type_ref_from_cgtype` 先按其底层 `CG_TYPE_I32` 命中 builtin 分支，生成 `i32`
  引用并丢失 nominal enum identity。第二次 descriptor 闭合因而错误查找
  `(i32, MV06Stateful)` witness。
- **通用修复方案**：在通用 `CGType -> FengTypeRef` 重建中，若 `enum_decl` 存在则优先生成
  该 enum declaration 的 nominal named ref，再处理普通 builtin kind。所有需要重建 enum
  类型参数的 generic/reifiable 路径共同受益；不特判 enum 名称、spec 或调用点。
- **运行时性能影响**：无；仅修正编译期类型引用重建，生成的 receiver 捕获与调用路径不变。
- **runtime ABI / `.ft` / 兼容性影响**：无；不修改数据布局、runtime 接口或符号文件记录。
- **是否需要人工决策**：否；修复恢复已有 nominal enum 类型语义，未触及强制停止条件。
- **专项验证结果**：新增 Codegen 与 FCTS 用例确认 enum nominal identity 在泛型 callable
  dependency 闭合中保留，trivial receiver 使用自身叶级及 merged witness；
  `build/bin/test_codegen` 与完整 FCTS 通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-028：constrained array 在 merged-witness 入口前强制查找外层伪 witness

- **关联分项**：MV06 Codegen 与 array fit receiver。
- **状态**：已解决。
- **最小复现**：代码审计 `cg_generic_descriptor_expr` 的 constrained array 分支；为数组
  subject 闭合 `T: IntersectionSpec` 时，该分支先查找
  `(array, IntersectionSpec)` Semantic witness。
- **实际结果**：Semantic 按规则只保存各叶级 object-form witness，因此该预查找必然为空并在
  通用 merged-witness helper 运行前报告 `CE0329`。此外，数组 lookup key 的 element type ref
  为临时重建值，不能直接浅拷贝进长期 Codegen witness cache。加入 array 专项后确认：首版
  直接用该重建 key 查找首个叶级 witness 仍为空，因此还需以 Semantic 已解析的闭合调用信息
  确认实际数组 subject key，不能假定两者完全等价。
- **期望结果**：数组与 enum、builtin、用户类型一样，由同一 subject 的叶级 witness 编译期
  组装静态 merged witness；缓存使用 Semantic witness 持有的稳定 subject key。
- **根因**：数组分支沿用 object-form 单一 witness 的入口前置条件，尚未按 intersection
  satisfaction 改为叶级 witness；新通用 helper 也需要在缓存数组 subject 前取得稳定 key。
- **通用修复方案**：object-form 数组继续复用现有直接 witness；intersection-form 数组跳过
  外层伪 witness 要求，进入通用 helper。Semantic 先按 ISSUE-029 保证叶级 witness 持有
  稳定 subject key；helper 解析首个叶级 source spec 后取得该 witness 的 key，并让所有叶级
  生成与 merged cache 共用该 key。不新增数组/spec/成员名称特判。
- **运行时性能影响**：无；仅增加编译期 key 选择，生成的静态 witness 与方法调用路径不变。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；补齐既定抽象覆盖，不触及强制停止条件。
- **专项验证结果**：新增 Semantic、Codegen 与 FCTS 数组用例确认 intersection-form 数组
  跳过外层伪 witness，并由同一稳定 subject key 的叶级 witness 组装 merged witness；三项
  专项均通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-029：array SpecWitness subject key 借用解析期临时 type ref

- **关联分项**：MV06 Semantic witness 生命周期与 ISSUE-028。
- **状态**：已解决。
- **最小复现**：generic call 以数组闭合 intersection 约束并按叶级 spec 物化 witness；Semantic
  结束 resolver scope 后，Codegen 再按等价数组 key 查找该 witness。
- **实际结果**：叶级 witness 已生成，但其 `subject_key.as.array.element_type_ref` 可能指向已由
  generic call 清理的临时 type ref；后续线性查找会漏配，读取该悬空 key 可触发非法内存访问。
- **期望结果**：`FengSpecWitness` 声明的 analysis 生命周期内，其完整 subject key 始终有效且
  可由结构等价 key 稳定查找。
- **根因**：`compute_spec_witness_if_absent` 从调用期 `actual_type_ref` 构造 array key，
  `feng_semantic_reserve_spec_witness` 再浅拷贝该 key；当前实现没有把 element type ref 的生命
  周期提升到 analysis。
- **通用修复方案**：object-form satisfaction 已有同一 `(subject, spec)` 的权威
  `FengSpecRelation`，其 array key 源自 fit 声明 AST，生命周期覆盖 analysis。reserve array
  witness 前，用现有结构等价查询取得该 relation，并复制 relation 持有的稳定 subject key；
  不新增 ownership 字段、不深拷贝、不改变公开结构布局。
- **运行时性能影响**：无；仅在原有编译期 witness 物化中复用已经执行的 relation 查询结果，
  不改变生成代码。
- **runtime ABI / `.ft` / 兼容性影响**：无；不修改结构布局、符号格式或 runtime。
- **是否需要人工决策**：否；方案消除悬空借用且不触及强制停止条件。
- **专项验证结果**：新增 Semantic 用例验证数组叶级 witness 的 subject key 在 analysis
  生命周期内稳定且可按结构等价 key 查找；`build/bin/test_semantic` 通过，Codegen 数组专项
  同时通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-030：Codegen 全套在 imported-codegen 生成 C 编译阶段失败

- **关联分项**：MV06 专项 Codegen 回归。
- **状态**：已解决。
- **最小复现**：与新增 Semantic、Symbol 专项并行执行 `build/bin/test_codegen`。
- **实际结果**：`temp/feng_codegen_imported_YTkT0w/generated.c` 编译失败，测试在
  `test/codegen/test_codegen.c:296` 中止；测试辅助函数当前隐藏了底层 C 编译器诊断。
- **期望结果**：Codegen 全套通过，或保留并展示足以定位失败根因的底层编译诊断。
- **根因**：`test_symbol` 与 `test_codegen` 的 `main` 都会在启动时执行 `rm -rf temp`；二者并行
  运行时，前者删除了后者已经写入、尚未交给 C 编译器的文件。该失败是测试进程之间共享
  临时根目录造成的执行方式冲突，与 MV06 生成结果无关。
- **通用修复方案**：遵循现有测试入口的串行执行约束，不并行运行会重建共享 `temp` 根目录的
  测试二进制；本专项不修改已有测试基础设施。
- **运行时性能影响**：无；没有产品代码变更。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；不触及强制停止条件。
- **专项验证结果**：单独执行 `build/bin/test_codegen` 通过。
- **全量回归结果**：沙箱外完整 `make test` 串行执行并通过，详见 MV06 独立交付记录。

### ISSUE-031：MV06 FCTS 对只读数组执行元素赋值

- **关联分项**：MV06 array receiver 行为验证。
- **状态**：已解决。
- **最小复现**：运行新增 MV06 FCTS，其中对声明为只读数组形态的局部值执行
  `array[0] = MV06TrivialCounter.ThreeHundred`。
- **实际结果**：Semantic 报 `AE0104: assignment target '<expression>' is not writable`。
- **期望结果**：测试使用语言已支持的可写数组声明验证方法值保留原数组 identity 与可观察变更。
- **根因**：新增 fit 与局部值都声明为 `MV06TrivialCounter[]`；该类型按数组规范只提供读取
  能力，而测试随后对其元素执行赋值。错误完全位于本次新增夹具。
- **通用修复方案**：把本次新增 array fit、局部值和泛型闭合实参统一声明为
  `MV06TrivialCounter[!]`，继续用元素变更验证已形成的方法值持有同一托管数组 identity。
- **运行时性能影响**：无；仅修正新增测试输入。
- **runtime ABI / `.ft` / 兼容性影响**：无。
- **是否需要人工决策**：否；不涉及产品行为或强制停止条件。
- **专项验证结果**：新增 FCTS 已改用可写数组并验证方法值形成后元素变更可由同一 receiver
  观察；完整 FCTS `850 passed, 0 failed, 0 skipped`。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-032：具体 nominal 元素数组 fit 被 Codegen 当作泛型数组 fit

- **关联分项**：MV06 array receiver FCTS；可能属于既有 array-fit Codegen 缺口。
- **状态**：已解决。
- **最小复现**：不需要 intersection 或方法值即可触发：

  ```feng
  enum Element { First }

  spec Readable {
    func read(): i32;
  }

  fit Element[!]: Readable {
    func read(): i32 {
      return if self[0] == Element.First { 1 } else { 0 };
    }
  }

  func run(value: Element[!]): i32 {
    return value.read();
  }
  ```

  该独立夹具通过 Semantic 后在 Codegen 报
  `CE0083: cannot apply numeric op to non-numeric operands`；MV06 FCTS 继续生成 C 时还会暴露
  descriptor 参数不一致。
- **实际结果**：生成的 fit 方法错误增加
  `const FengGenericParamDescriptor *_MV06TrivialCounter` 参数；数组下标按未知泛型元素生成为
  `void *`，并且生成的 witness wrapper 未传该额外 descriptor，最终原生 C 编译报参数不足及
  `void *` 到 `i32` 不兼容。
- **期望结果**：`MV06TrivialCounter` 绑定当前模块已有的具体 enum；数组 fit 方法不携带元素类型
  descriptor，数组下标按 enum 的 trivial 表示生成，witness wrapper 与实现签名一致。
- **根因**：Semantic 的 `fit_target_collect_array_local_type_param` 会先解析可见类型，因此正确把
  `Element` 识别为具体 enum；Codegen 的
  `cg_builtin_fit_target_local_type_param` 只排除了 builtin、user type、spec 与 generic 声明，
  没有排除可见 enum，于是重新把同一名称合成为 fit 局部类型参数。与此同时，
  `reifiable_deps.c` 的 `extract_fit_target_implicit_type_param` 仅按单段名称形状判断，也没有复用
  Semantic 已确定的“具体类型 / 隐式参数”结论。这是既有 array-fit 分类在编译阶段不一致，
  不是 MV06 merged witness 的错误。
- **通用修复方案**：Semantic 在现有 type-fact sidecar 中仅为真正由数组 fit 引入的局部元素
  类型参数记录声明事实；reifiable dependency 收集与 Codegen 优先读取该结论，导入包声明继续
  使用结构化 `.ft` 类型节点回退。Codegen 的回退解析同时完整排除可见 enum。新增独立
  direct-call Semantic/Codegen 回归后，再验证 MV06 merged witness；不得用更换测试 subject
  的方式掩盖缺陷。
- **运行时性能影响**：无；修复仅改变编译期分类，正确代码不增加运行时指令、查找或分配。
- **runtime ABI / `.ft` / 兼容性影响**：无格式或 runtime ABI 变更；原先无法正确生成的程序
  使用具体 enum 数组 fit 的既定 ABI，`.ft` roundtrip 已验证。
- **是否需要人工决策**：是；开发者已批准一并修复。
- **专项验证结果**：新增独立 Semantic 与 Codegen direct-call 用例确认具体 enum 数组 fit
  不携带伪元素 descriptor、数组下标保持 enum trivial 表示；新增 Symbol 用例确认 `.ft`
  concrete enum 与真正局部类型参数的节点分类；三项专项及完整 FCTS 均通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### ISSUE-033：具体 enum 数组 fit 的 `.ft` 元素节点不是 nominal named

- **关联分项**：ISSUE-032 的 `.ft` 不变性验证。
- **状态**：已解决。
- **最小复现**：导出并重新读取公开声明
  `open fit Element[!]: Readable`，其中 `Element` 是同模块公开 enum；检查 fit target 的数组
  element 类型节点。
- **实际结果**：array target 与可写位均正确，但 element 节点 kind 不是预期的
  `FENG_SYMBOL_TYPE_KIND_NAMED`，新增 Symbol 专项断言失败。
- **期望结果**：具体 enum 保留 nominal `Element` identity；只有真正的 `fit T[]` 局部元素
  类型参数才导出为 `TYPE_PARAM_REF`。
- **根因**：Symbol export 的 `find_local_type_like_decl` 只把 type 与 spec 视为本模块已声明
  类型，漏掉 enum；`infer_fit_array_target_type_param_name` 因而把单段 `Element` 误判为 fit
  局部类型参数并写成 `TYPE_PARAM_REF`。这与 ISSUE-032 是同一类编译期分类遗漏。
- **通用修复方案**：让 Symbol 的本地 type-like 声明查询同时覆盖 enum；数组 fit 的既有推断
  流程由此把 `Element` 保留为普通 nominal named 节点，真正的 `fit T[]` 仍使用
  `TYPE_PARAM_REF`。
- **运行时性能影响**：无；仅修正编译期符号导出分类。
- **runtime ABI / `.ft` / 兼容性影响**：不新增节点 kind、字段或格式版本，也不修改 runtime
  ABI；只是把此前错误写成 `TYPE_PARAM_REF` 的具体 enum 恢复为格式早已支持的 `NAMED` 节点。
- **是否需要人工决策**：否；属于已批准 ISSUE-032 的同类遗漏，不触及格式或 ABI 变更条件。
- **专项验证结果**：新增具体 enum 数组 fit `.ft` roundtrip 通过；既有 `fit T[!]` 的
  `TYPE_PARAM_REF` roundtrip 同时通过。
- **全量回归结果**：沙箱外完整 `make test` 通过，详见 MV06 独立交付记录。

### MV06 独立交付记录

- **变更范围**：补齐 `T: IntersectionSpec` 泛型值的实例方法值，使其组合复用 MV02 的泛型
  receiver 值语义与 MV05 的 flattened requirement / merged witness 分派；同时按人工批准一并
  修复具体 nominal enum 元素数组 fit 的编译期分类缺口。IC01 与 MV07 的 intersection 约束
  静态成员能力没有在本次提前开放。
- **实现结果**：Semantic 对 object-form 与 intersection-form 约束使用同一方法值成员面和重载
  解析，保留完整 `T`、精确 requirement 原声明及访问事实；Codegen 沿用现有共享泛型 callable
  捕获计划，并把静态 merged-witness 物化扩展到用户类型、enum、builtin 与数组 subject。
  intersection 数组从同一稳定 subject key 的叶级 witness 组装 merged witness，不要求不存在的
  外层伪 witness。无目标引用稳定报告 `AE0523`，不可访问 seal requirement 稳定报告 `AE0708`。
- **一并修复**：ISSUE-021 至 ISSUE-033 均已解决。其中 ISSUE-032 经人工批准，Semantic 通过
  既有 type-fact sidecar 记录真正的数组 fit 局部元素类型参数，reifiable dependency 与 Codegen
  复用该编译期事实；Symbol export 将具体 enum 保留为既有 `NAMED` 节点，真正的 `fit T[!]`
  仍导出为 `TYPE_PARAM_REF`。其余问题均是 MV06 通用抽象覆盖、编译期生命周期、诊断一致性或
  本次新增夹具问题。
- **运行时成本**：没有增量运行时开销。方法值仍只进行形成 callable 本身所必需的一次既有
  closure 分配；receiver 使用原有引用、trivial 或 descriptor-sized 捕获表示，调用仍通过
  编译期固定的 witness 槽。没有新增 spec box、第二份 receiver、运行时成员查找、额外分配、
  descriptor 字段或动态分支；merged witness 只在编译期生成静态数据。
- **ABI 与格式**：runtime、runtime 私有 ABI、生成程序既有数据布局以及 `.ft` schema / 版本均
  未改变；没有新增或修改公开结构字段及依赖 kind。ISSUE-033 仅把此前错误分类的具体 enum
  数组元素写为格式既有的 `NAMED` 节点，真正的局部类型参数表示保持不变。
- **专项测试**：`build/bin/test_semantic`、`build/bin/test_codegen`、`build/bin/test_symbol` 均
  通过；新增用例覆盖 object/父级/嵌套成员、合法重载、seal 与无目标诊断、托管引用、trivial
  enum、descriptor-sized `@value`、可写数组、稳定 witness key、无 spec box、严格 C 编译、
  callable dependency 与 `.ft` roundtrip。没有修改既有测试用例，只新增专项用例及注册入口。
- **FCTS**：覆盖直接/父级/嵌套 requirement、重载、三类值表示、可写 nominal-enum 数组、
  receiver 稳定绑定、生命周期、合法 seal，以及 imported provider / consumer-only 闭合；结果为
  `850 passed, 0 failed, 0 skipped`。
- **全量回归**：沙箱外完整 `make test` 通过；UBSan 与普通
  `-O2 -Werror -pedantic` 两阶段均完成，两轮 smoke `91/91`、两轮 std `601/601`、两轮 FCTS
  `850/850`，以及 archive、lexer、parser、Semantic、runtime、Codegen、debug、CLI、symbol、
  性能约束、增量构建、发布/安装脚本、bundled packages 和 toolchain 测试全部通过。
- **实施问题**：ISSUE-021 至 ISSUE-033 均已按先记录、后分析、再修复的流程收口；MV06 无
  未解决问题。
- **建议 commit message**：`feat: support intersection-constrained generic method values`

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
- [ ] 第 1 节第 8 项强制规则已经满足，且不存在尚未完成人工决策的例外。
- [ ] 最后一个分项完成后的沙箱外完整 `make test` 通过。
