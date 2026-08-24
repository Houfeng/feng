# Feng object-form `spec` 完整行为与用例覆盖补强

状态：待 Review，尚未实施。

本文的目标是补齐 object-form `spec` 的完整非等价用例，并确保每个用例对应的最终语言
行为正确，不是只把测试数量补齐。语言规则以
[Feng 语言 `spec` 规范](../specifications/feng-spec.md) 为准；方法参数绑定规则引用
[Feng 语言函数规范](../specifications/feng-function.md)。本文 §5 已确认而主规范尚未明文记录
的规则，实施时必须先更新主规范，再修改代码和测试。

## 1. 目标与覆盖口径

本专项完成后，`feng-spec.md` 中每条直接适用于 object-form `spec` 的 `[必须]` / `[禁止]`
规则，以及本文 §5 已确认的成员冲突规则，都必须同时满足：

1. 合法用法具有直接、可观察的正向测试，最终行为符合规范；
2. 非法用法被编译器拒绝，不得进入 Codegen 或运行时；
3. 每个非等价分支有直接测试，或明确记录现有哪条公共测试经过同一实现路径；
4. 新增测试发现最终行为错误时，问题已经记录、分析并修复。

测试职责保持分层：

| 测试层 | 只验证什么 |
| --- | --- |
| Parser test | 语法、AST 字段和 `SE` 诊断 |
| Semantic test | 满足关系、成员选择、转换资格和 `AE` 诊断 |
| Codegen test | witness/slot、默认初始化、求值次数和 cleanup 结构 |
| FCTS | 用户可观察的合法语言行为 |
| 跨包 FCTS | 公开 `.ft` 恢复后的非等价路径 |

对于非法用法，Parser 还是 Semantic 负责拒绝不是本专项的调整目标。实施时先记录当前实际
拒绝阶段和诊断码：只要最终拒绝行为正确，就保持现状，不迁移诊断阶段；若非法程序进入
Codegen 或运行时，才属于本专项必须修复的最终行为错误。

“完整覆盖”不要求枚举参数名、普通标量类型、泛参数量或父级深度的笛卡尔积；经过同一
实现分支的等价程序只保留一个最小代表。

## 2. 强制约束

- [ ] 不修改已有测试用例的源码、预期诊断或断言；只新增用例。允许在测试入口追加新用例
  的注册或调用，但不得改写既有用例。
- [ ] 不删除、注释或弱化既有断言，也不把预期改成当前错误行为。
- [ ] 新增用例若暴露最终语言行为错误，必须先按 §6 记录最小复现、当前结果、预期结果
  和影响范围，再分析并修复；规则明确且不触碰人工门槛时，不因它是新发现问题而降低
  交付范围。
- [ ] 非法用法已经由 Parser 或 Semantic 正确拒绝时，只记录当前阶段和诊断码；本专项
  不迁移拒绝阶段、不改诊断码归属。
- [ ] 不改变 runtime ABI、Feng/C ABI、`.ft` 格式或现有 witness/descriptor 布局。
- [ ] 不增加生成程序的运行时分支、查找、签名比较、分配、间接调用、adapter、
  retain/release 或 cleanup。
- [ ] 不增加只服务某个测试类型名、成员名或固定模型的特殊处理。
- [ ] 如果测试预期或修复方案不能由主规范及 §5 的已确认规则唯一确定，停止该分支，
  由人工决策；测试文档不得擅自定义其他语言语义。
- [ ] 如果修复必须修改已有测试、增加运行时开销、改变 ABI / `.ft` / witness 布局，
  必须停止，由人工决策。
- [ ] 每个非文档步骤完成后，先运行对应专项测试，再在 Codex 沙箱外运行全量
  `make test`；两者均通过后才能独立交付。

## 3. 当前覆盖基线与缺口总表

以下结论基于当前源码和测试入口核对。已有用例保持不变，本专项补齐右侧测试缺口，并修复
新增用例确认的最终行为错误。

| 编号 | 已有直接证据 | 仍需补齐 |
| --- | --- | --- |
| OFC01 | `test_spec_static_members_parse`、`test_spec_seal_members_parse`、`test_spec_static_member_parse_errors` | 实例成员声明负向分支、实例/静态方法缺少返回类型、参数 `let`/`var` |
| OFC02 | `test_type_declared_specs_missing_field_rejected`、一条实例字段 mutability 不匹配、静态字段缺失 | 实例/静态字段的类型不匹配和两个 mutability 方向 |
| OFC03 | 满足检查当前不比较实现参数 mutability | type/fit、实例/静态、参数省略/`let`/`var` 的直接正向证据 |
| OFC04 | 普通 type 父级、重复父级、父级环及父方法叠加已有测试 | 非 object-form 父级拒绝；父字段进入子成员面后的统一冲突检查 |
| OFC05 | 默认 spec 局部、字段和方法已有基础测试 | 每次默认初始化产生独立 subject、数组元素独立、各返回值类别的默认方法 |
| OFC06 | `test_spec_upcast.ff` 已覆盖初始化、传参、返回、字段/数组写入、单次求值和正常生命周期 | 普通 object-form fat value 的覆盖赋值与各 cleanup 边 |
| OFC07 | 方法名等于 spec 名已有 Parser/Semantic；`@abi` object-form 已拒绝 | 字段/方法同一冲突面拒绝与跨冲突面允许、spec 同名方法端到端调用、调用方式注解 `AE1305`、必要的跨包恢复路径 |
| OFC08 | 各专项已有分散证据 | 最终“规范条目 → 测试”追踪表、结构/性能检查和全量回归 |

以下能力已有独立专项，本文件只建立引用，不重复设计用例：

- object-form 实例/静态方法重载、父契约方法叠加和 witness：
  [object-form `spec` 方法重载修复](./feng-object-form-spec-overload-witness-bugfix.md)；
- 实例方法值和受约束类型参数静态方法值：`test_spec_method_value.ff`、
  `test_spec_static_method_value.ff`；
- 子到父 coercion/cast、多父、菱形、泛型父、subject 类别和跨包：
  `test_spec_upcast.ff`；
- `open` / `seal` / `@friend` requirement：`test_spec.ff`、
  `test_spec_seal_dependency.ff` 和 `test_friend.ff`；
- intersection-form 的 merged witness 与方法调用/方法值由 intersection 专项负责，
  本专项只验证它不能成为 object-form 父级。

## 4. 分步实施

每一步均可单独“补齐、验证、交付”。前一步完成不是后一步修改既有用例的理由。

每一步统一按以下顺序执行：先补主规范（仅限已有人工结论），再用最小程序确认当前行为；
若最终行为错误，先写入 §6，再完成根因分析与通用修复；最后补齐测试并回归验证。非法
用法当前由 Parser 还是 Semantic 拒绝只记录，不在该过程中顺便迁移。

### OFC01：非法声明的最终拒绝

#### 问题

正向 AST 已覆盖实例/静态字段、实例/静态方法以及默认、`open`、`seal`。当前实现核对
结果是：以下非法声明均由 Parser 使用 `SE0603`–`SE0606` 拒绝；该阶段归属只作为现状
记录，本专项不讨论或迁移到 Semantic。现有负向测试只直接覆盖静态字段初始值和静态
方法函数体，其余最终拒绝分支仍缺少直接证据。

每个片段应作为独立程序测试，确保只产生目标诊断：

```feng
spec InvalidField {
  let value: int = 1; // SE0603：spec 字段不能有初始值
}
```

```feng
spec InvalidBody {
  func read(): int { return 1; } // SE0605：requirement 不能有函数体
}
```

```feng
spec InvalidReturn {
  func run(); // SE0604：spec 方法必须显式声明返回类型
}
```

```feng
spec InvalidParameter {
  func update(var value: int): int;         // SE0606
  static func inspect(let value: int): int; // SE0606
}
```

#### 任务

- [ ] 先记录每种非法声明当前由 Parser 还是 Semantic 拒绝，以及当前诊断码；
- [ ] Parser：新增实例字段初始值 `SE0603`；
- [ ] Parser：新增实例方法函数体 `SE0605`；
- [ ] Parser：分别新增实例/静态方法缺少返回类型 `SE0604`；
- [ ] Parser：分别新增实例/静态方法参数显式 `let`、显式 `var` 的 `SE0606`；
- [ ] 保留已有静态字段初始值、静态方法函数体和方法级泛参用例不变；
- [ ] 运行 Parser 专项测试和沙箱外 `make test`；
- [ ] 在本文记录 OFC01 的新增用例、命令和结果。

该步骤按当前事实预期只增加 Parser test。如果某个非法程序未被 Parser 或 Semantic 拒绝，
先记录、再分析并修复最终拒绝行为；不借此调整其他已正确拒绝用法的阶段归属。

### OFC02：字段 requirement 精确满足矩阵

#### 问题

字段 requirement 按“名称 + `let`/`var` + 类型完全一致”匹配。现有用例覆盖字段缺失和
实例 `let requirement` / `var implementation`，还缺以下非等价分支：

```feng
spec Writable {
  var value: int;
}

type ReadonlyValue: Writable {
  let value: int; // AE0702：var requirement 不能由 let 字段满足
}
```

```feng
spec Numbered {
  let value: int;
}

type TextValue: Numbered {
  let value: string; // AE0703：字段类型不一致
}
```

静态字段必须执行同一规则：

```feng
spec StaticWritable {
  static var value: int;
}

type StaticReadonly: StaticWritable {
  static let value: int = 0; // AE0702
}
```

#### 任务

- [ ] Semantic：实例 `let requirement` / `var implementation` 保留既有用例；
- [ ] Semantic：新增实例 `var requirement` / `let implementation`，断言 `AE0702`；
- [ ] Semantic：新增实例字段同 mutability、不同类型，断言 `AE0703`；
- [ ] Semantic：新增静态字段两个 mutability 方向，均断言 `AE0702`；
- [ ] Semantic：新增静态字段同 mutability、不同类型，断言 `AE0703`；
- [ ] 运行 Semantic 专项测试和沙箱外 `make test`；
- [ ] 在本文记录 OFC02 的新增用例、命令和结果。

该步骤只验证编译期满足检查，不需要 FCTS 或运行时改动。

### OFC03：实现方法参数 `let` / `var`

#### 问题

`spec` 参数不能声明 `let` / `var`；实现方法的参数绑定可以省略修饰符、显式写 `let`，
或显式写 `var`。实现侧 mutability 不属于 requirement 签名，不改变 witness 槽函数签名。

以下 type 与 fit 都应合法：

```feng
spec Transformer {
  func transform(value: int): int;
  static func normalize(value: int): int;
}

type DirectTransformer: Transformer {
  func transform(var value: int): int {
    value += 1;
    return value;
  }

  static func normalize(let value: int): int {
    return value;
  }
}

type AdaptedTransformer {}

fit AdaptedTransformer: Transformer {
  func transform(let value: int): int {
    return value;
  }

  static func normalize(var value: int): int {
    value += 1;
    return value;
  }
}
```

#### 任务

- [ ] Semantic：type 实例方法的参数省略/`let`/`var` 均满足同一 requirement；
- [ ] Semantic：type 静态方法的参数省略/`let`/`var` 均满足同一 requirement；
- [ ] Semantic：fit 实例方法的参数省略/`let`/`var` 均满足同一 requirement；
- [ ] Semantic：fit 静态方法的参数省略/`let`/`var` 均满足同一 requirement；
- [ ] FCTS：至少让实例和静态 `var` 参数真实修改自己的局部绑定并断言结果；
- [ ] Codegen：确认参数 mutability 不进入 witness 槽签名，不生成 adapter；
- [ ] 运行 Semantic、Codegen、新增 FCTS 和沙箱外 `make test`；
- [ ] 在本文记录 OFC03 的新增用例、命令和结果。

如果实现参数 mutability 进入 `.ft` 契约身份、ABI 或需要运行时 adapter，必须停止并由人工
决策。

### OFC04：父级 form 限制与父字段叠加冲突

#### 问题一：父级必须是 object-form

现有用例只直接证明普通 `type` 不能成为 object-form 父级。主规范还明确禁止
callable-form、union-form 和 intersection-form `spec` 成为父级：

```feng
spec Left {}
spec Right {}
spec Callback(): void;
spec Choice: int | string;
spec Both: Left & Right;

spec BadCallableParent: Callback {}
spec BadUnionParent: Choice {}
spec BadIntersectionParent: Both {}
```

上述三个 `Bad...` 声明必须放在三个独立用例中，以免前一个错误遮蔽后一个错误。

#### 问题二：父字段进入子成员面后按直接声明处理

已确认规则：父 `spec` 的字段叠加到子 `spec` 后，与直接写在子 `spec` 中的字段本质相同；
字段不能重载，同一实例或静态成员面中只要出现同名字段就构成冲突，不因类型和 mutability
完全一致而合并。实例面与静态面仍按普通直接成员规则分别处理。

子级与父级同名：

```feng
spec Parent {
  let value: int;
}

spec Child: Parent {
  let value: int; // 冲突
}
```

多个父级同名，即使签名完全一致也冲突：

```feng
spec Left { let value: int; }
spec Same { let value: int; }
spec DifferentType { let value: string; }
spec DifferentMutability { var value: int; }

spec Repeated: Left, Same {}                       // 冲突
spec TypeConflict: Left, DifferentType {}          // 冲突
spec MutabilityConflict: Left, DifferentMutability {} // 冲突
```

该决定只补齐字段叠加规则。父方法进入子方法面后仍按已经确认的普通方法重载规则处理，
不改变合法重载、重复签名和仅返回类型差异的现有结论。

#### 任务

- [ ] 先在 `feng-spec.md` 明确父字段进入子完整成员面后按直接声明字段检查，同名即冲突；
- [ ] Semantic：callable-form 父级拒绝，断言 `AE0613`；
- [ ] Semantic：union-form 父级拒绝，断言 `AE0613`；
- [ ] Semantic：intersection-form 父级拒绝，断言 `AE0613`；
- [ ] Semantic：同一 object-form `spec` 体内直接重复实例字段、静态字段均拒绝；
- [ ] Semantic：子级自有字段与直接/传递父字段同名时拒绝；
- [ ] Semantic：多个父级提供完全相同字段时拒绝；
- [ ] Semantic：多个父级字段类型不同或 mutability 不同时拒绝；
- [ ] Semantic：覆盖实例字段、静态字段和 owner 泛参替换后产生同名字段的非等价路径；
- [ ] 保留普通 type 父级、重复父级和直接/间接环用例不变；
- [ ] 保留父方法叠加与重载用例不变；
- [ ] 确认所有冲突均由 Parser 或 Semantic 拒绝，不进入 Codegen；
- [ ] 运行 Semantic 专项测试和沙箱外 `make test`；
- [ ] 在本文记录 OFC04 的新增用例、命令和结果。

### OFC05：默认 witness 与默认 subject

#### 问题

当前用例证明默认 object-form 值可访问字段和调用方法，但没有直接证明规范要求的“每次
默认初始化创建新实例”，也没有覆盖默认方法返回值的主要值模型。

独立 subject：

```feng
spec Counter {
  var value: int;
}

let first: Counter;
let second: Counter;
first.value = 1;

assert(first.value == 1);
assert(second.value == 0);
assert(first != second);

let values: Counter[!] = Counter[:2];
values[0].value = 1;
assert(values[1].value == 0);
assert(values[0] != values[1]);
```

默认方法返回值类别：

```feng
@value
type DefaultPair {
  var left: int;
  var right: int;
}

spec NestedDefault {
  var value: int;
}

spec Defaults {
  func number(): int;
  func text(): string;
  func pair(): DefaultPair;
  func nested(): NestedDefault;
}
```

`number()`、`text()`、`pair()` 和 `nested()` 应分别返回 scalar、managed、aggregate 和
object-form `spec` 的默认值；`nested()` 产生的 object-form 默认值也必须遵守独立 subject
规则。

#### 任务

- [ ] FCTS：两个默认 spec 局部的字段状态与 identity 独立；
- [ ] FCTS：默认 spec 数组各元素的字段状态与 identity 独立；
- [ ] FCTS：默认方法返回 scalar、managed、aggregate 和 object-form `spec` 默认值；
- [ ] Codegen：每个默认初始化站点调用既有 subject factory，不读取共享 subject 单例；
- [ ] Codegen：数组元素初始化不复用同一个 subject；
- [ ] 复用 `test_spec_upcast.ff` 已有“默认子 spec 投影到父 spec 后共享同一 subject”用例，
  不重复增加；
- [ ] 运行 Codegen、新增 FCTS 和沙箱外 `make test`；
- [ ] 在本文记录 OFC05 的新增用例、命令和结果。

### OFC06：普通 object-form fat value 生命周期

#### 问题

`test_spec_upcast.ff` 已证明一次普通子到父投影不会产生额外 subject，并在正常退出时只终结
一次；尚未形成普通 object-form fat value 在覆盖赋值和不同 cleanup 边上的直接证据。

最小覆盖赋值模型：

```feng
spec ResourceView {
  func id(): int;
}

type Resource: ResourceView {
  open static var finalized: int = 0;
  let value: int;

  func id(): int { return self.value; }

  func ~Resource() {
    Resource.finalized += 1;
  }
}

func overwriteResource(): void {
  var view: ResourceView = Resource { value: 1 };
  view = Resource { value: 2 };
  assert(Resource.finalized == 1); // 覆盖时释放旧 subject
}
```

完整验证矩阵：

| 场景 | 必须断言的事实 |
| --- | --- |
| spec 局部复制 | 原绑定离开作用域后副本仍可用；最终只终结一次 |
| spec `var` 覆盖 | 覆盖时释放旧 subject，作用域退出释放新 subject |
| spec 字段覆盖 | holder 替换旧值和 holder 退出均无泄漏/重复释放 |
| spec 数组元素覆盖 | 元素替换与数组退出均无泄漏/重复释放 |
| 参数、返回和多层转发 | subject 保持有效且最终只终结一次 |
| 提前 `return` | cleanup 执行一次 |
| `break` / `continue` | 每条循环退出边执行正确 cleanup |
| `throw` / `catch` | 异常清理后不泄漏、不重复释放 |
| 同一 subject 的多个 spec/父 spec 视角 | 所有视角释放后 subject 只终结一次 |

#### 任务

- [ ] 新增独立生命周期 FCTS，逐项覆盖上表；
- [ ] 每项断言精确 created/finalized 计数，不只断言程序未崩溃；
- [ ] Codegen：检查覆盖赋值和各控制流边使用既有 cleanup，不出现额外 box；
- [ ] Codegen：检查没有因 spec 视角增加多余 retain/release；
- [ ] 运行 Codegen、新增 FCTS 和沙箱外 `make test`；
- [ ] 在本文记录 OFC06 的新增用例、命令和结果。

### OFC07：特殊命名、注解与跨包恢复

#### 问题一：字段与方法只在同一实例/静态冲突面内禁止同名

已确认规则与普通 `type` 一致：实例字段只与实例方法检查同名冲突，静态字段只与静态
方法检查同名冲突。字段与另一冲突面的方法没有歧义，允许同名。

```feng
spec InvalidInstance {
  let value: int;
  func value(): int; // 冲突：实例字段 + 实例方法
}

spec InvalidStatic {
  static let value: int;
  static func value(): int; // 冲突：静态字段 + 静态方法
}

spec ValidInstanceField {
  let value: int;
  static func value(): int; // 合法：实例字段 + 静态方法
}

spec ValidStaticField {
  static let value: int;
  func value(): int; // 合法：静态字段 + 实例方法
}
```

同一冲突面内允许同名会使字段读取与方法值写法无法唯一解释：实例侧均使用
`value.member`，静态约束侧均使用 `T.member`。跨冲突面不存在该歧义。

#### 问题二：方法名等于 spec 名缺少端到端证据

Parser 和 Semantic 已确认下列方法是普通方法，但没有直接 FCTS/Codegen 证明其进入普通
实例槽和静态槽：

```feng
spec ResourceSurface {
  func ResourceSurface(): int;
  static func ResourceSurface(): string;
}

type ResourceImpl: ResourceSurface {
  func ResourceSurface(): int { return 42; }
  static func ResourceSurface(): string { return "resource"; }
}

func readInstance(value: ResourceSurface): int {
  return value.ResourceSurface();
}

func readStatic<T: ResourceSurface>(): string {
  return T.ResourceSurface();
}
```

#### 问题三：调用方式注解没有直接负向用例

`@abi` object-form 已有 `AE1304` 用例；还需证明实际调用方式注解走 `AE1305`：

```feng
@cdecl("native")
spec InvalidConvention {
  func run(): void;
}
```

#### 任务

- [ ] 先在 `feng-spec.md` 明确字段/方法同名冲突只发生在同一实例或静态冲突面；
- [ ] Semantic：实例字段 + 实例方法同名时拒绝，复用普通 `type` 的同一冲突检查；
- [ ] Semantic：静态字段 + 静态方法同名时拒绝，复用普通 `type` 的同一冲突检查；
- [ ] Semantic/FCTS：实例字段 + 静态方法同名保持合法并可分别访问；
- [ ] Semantic/FCTS：静态字段 + 实例方法同名保持合法并可分别访问；
- [ ] Codegen：合法跨冲突面同名成员进入各自既有字段/方法路径，不增加运行时选择；
- [ ] FCTS/Codegen：实例方法名等于 spec 名时走普通实例 requirement 槽；
- [ ] FCTS/Codegen：静态方法名等于 spec 名时走普通静态 requirement 槽；
- [ ] Semantic：object-form `spec` 使用 `@cdecl(...)` 时断言 `AE1305`；
- [ ] 跨包 FCTS：公开 spec 经 `.ft` 恢复后，验证一次“默认值独立”和一次“方法名等于
  spec 名”的非等价路径；
- [ ] 确认只消费既有 `.ft` 声明事实，不改变 `.ft` 格式；
- [ ] 运行 Semantic、Codegen、新增 FCTS 和沙箱外 `make test`；
- [ ] 在本文记录 OFC07 的新增用例、命令和结果。

### OFC08：最终追踪与回归验收

#### 任务

- [ ] 为 `feng-spec.md` 中每条直接适用于 object-form `spec` 的 `[必须]` / `[禁止]`
  建立“规范条目 → 测试文件/测试函数”映射；
- [ ] 没有新增直接测试的条目，记录所复用的公共路径及不重复的原因；
- [ ] 非法用法清单均记录当前由 Parser 或 Semantic 拒绝及诊断码，未为阶段归属迁移实现；
- [ ] 所有实施过程发现的最终行为错误均已记录、分析并修复；
- [ ] 不存在仍未交付的“计划问题”或“实施过程发现问题”；
- [ ] 检查 object-form fat value 仍为既有 subject+witness 表示；
- [ ] 检查新增测试没有要求 runtime、`.ft` 格式或 ABI 变更；
- [ ] 检查生成 C 没有新增运行时成员搜索、签名比较、adapter 或额外分配；
- [ ] `git diff --check` 通过；
- [ ] 所有新增专项测试通过；
- [ ] FCTS 全量通过；
- [ ] 在 Codex 沙箱外运行全量 `make test` 并通过；
- [ ] 在本文补齐每一步交付结果、测试命令和最终计数；
- [ ] 状态更新为“已完成”。

## 5. 已确认规则

以下两项已经人工确认，不再是实施过程中的待决语义。OFC04、OFC07 必须先把它们补入
`feng-spec.md`，再据此验证和修复实现。

### D01：字段与方法同名冲突面

object-form `spec` 与普通 `type` 使用相同规则：

| 组合 | 结果 |
| --- | --- |
| 实例字段 + 实例方法同名 | 拒绝 |
| 静态字段 + 静态方法同名 | 拒绝 |
| 实例字段 + 静态方法同名 | 允许 |
| 静态字段 + 实例方法同名 | 允许 |

同一实例或静态冲突面内，字段访问与方法值引用使用相同成员写法，不能唯一解释；跨冲突面
不存在该问题，不应误报冲突。

### D02：父字段按子级直接成员处理

父 `spec` 的字段叠加到子 `spec` 后，与直接写在子 `spec` 中的字段本质相同。在同一实例
或静态成员面内，子级自有字段与父字段同名、多个父级字段同名，都必须报冲突；即使字段
类型与 mutability 完全一致也不合并。

D02 只补齐字段规则。父方法继续按既有普通方法重载规则处理，不改变合法重载、重复签名、
仅返回类型差异及完整签名等价 requirement 的既有规则。

## 6. 实施过程问题记录

新增用例暴露最终行为问题时，必须先填写以下模板，再分析和修改生产代码。规则与方案明确
且不触碰人工门槛时，记录完成后继续修复；只有不确定或触碰强制约束时才停止等待人工
决策。

### ISSUE-待编号：待填写

- 发现阶段：OFCxx。
- 最小复现：待填写。
- 当前结果：待填写。
- 预期结果：引用主规范条目或已批准的人工结论。
- 影响范围：待填写。
- 是否涉及运行时开销：待填写。
- 是否涉及 ABI / `.ft` / witness 布局：待填写。
- 根因分析：待填写。
- 建议方案：待填写；不得包含测试模型特判。
- 人工决策：无需 / 待填写。
- 状态：待分析。

如果问题的预期不能由主规范或 §5 唯一确定，或者修复需要修改已有用例、增加运行时开销、
改变 ABI / `.ft` / witness 布局，立即停止该分支，由人工决策。

## 7. 测试文件组织

为保证每一步可独立交付，建议新增文件而不是把不同职责继续堆入已有大型 FCTS：

- OFC03：`fcts/fcts_bin/src/test_object_spec_contract_surface.ff`；
- OFC05：`fcts/fcts_bin/src/test_object_spec_default_value.ff`；
- OFC06：`fcts/fcts_bin/src/test_object_spec_value_lifecycle.ff`；
- OFC07 本包：`fcts/fcts_bin/src/test_object_spec_special_surface.ff`；
- OFC07 跨包：
  `fcts/fcts_lib/src/test/lib_object_spec_complete_coverage.ff` 与
  `fcts/fcts_bin/src/test_object_spec_package_coverage.ff`；
- Parser/Semantic/Codegen 在对应 C 测试文件末尾新增独立测试函数，只在入口追加调用；
- `fcts/fcts_bin/src/main.ff` 只追加新测试入口调用，不改写已有入口。

## 8. Review 清单

- [ ] 认可 §1 的“完整覆盖”口径；
- [ ] 认可 OFC01–OFC08 的范围和实施顺序；
- [ ] OFC02 准确记录字段 `let` / `var` 必须双向精确匹配；
- [ ] OFC03 准确记录实现参数省略/`let`/`var` 均可满足 requirement，且不进入 ABI；
- [ ] §5 准确记录 D01 的同一冲突面拒绝、跨冲突面允许规则；
- [ ] §5 准确记录 D02 的父字段按子级直接成员处理规则；
- [ ] 非法用法的 Parser/Semantic 拒绝阶段只记录，本专项不迁移；
- [ ] 最终行为错误先记录、再分析并修复；不确定或触碰人工门槛时停止。
