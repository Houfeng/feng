# Feng object-form `spec` 完整行为与用例覆盖补强

状态：已完成（2026-08-24）。

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

- [x] 不修改已有测试用例的源码、预期诊断或断言；只新增用例。允许在测试入口追加新用例
  的注册或调用，但不得改写既有用例。
- [x] 不删除、注释或弱化既有断言，也不把预期改成当前错误行为。
- [x] 新增用例若暴露最终语言行为错误，必须先按 §6 记录最小复现、当前结果、预期结果
  和影响范围，再分析并修复；规则明确且不触碰人工门槛时，不因它是新发现问题而降低
  交付范围。
- [x] 非法用法已经由 Parser 或 Semantic 正确拒绝时，只记录当前阶段和诊断码；本专项
  不迁移拒绝阶段、不改诊断码归属。
- [x] 不改变 runtime ABI、Feng/C ABI、`.ft` 格式或 descriptor 布局；ISSUE-003 所述错误
  合槽的 witness 仅在取得人工批准后改为两个规范要求的独立槽。
- [x] 不增加生成程序的运行时分支、查找、签名比较、分配、间接调用、adapter、
  retain/release 或 cleanup。
- [x] 不增加只服务某个测试类型名、成员名或固定模型的特殊处理。
- [x] 如果测试预期或修复方案不能由主规范及 §5 的已确认规则唯一确定，停止该分支，
  由人工决策；测试文档不得擅自定义其他语言语义。
- [x] 如果修复必须修改已有测试、增加运行时开销、改变 ABI / `.ft` / witness 布局，
  必须停止，由人工决策。
- [x] 每个非文档步骤完成后，先运行对应专项测试，再在 Codex 沙箱外运行全量
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

- [x] 先记录每种非法声明当前由 Parser 还是 Semantic 拒绝，以及当前诊断码；
- [x] Parser：新增实例字段初始值 `SE0603`；
- [x] Parser：新增实例方法函数体 `SE0605`；
- [x] Parser：分别新增实例/静态方法缺少返回类型 `SE0604`；
- [x] Parser：分别新增实例/静态方法参数显式 `let`、显式 `var` 的 `SE0606`；
- [x] 保留已有静态字段初始值、静态方法函数体和方法级泛参用例不变；
- [x] 运行 Parser 专项测试和沙箱外 `make test`；
- [x] 在本文记录 OFC01 的新增用例、命令和结果。

该步骤按当前事实预期只增加 Parser test。如果某个非法程序未被 Parser 或 Semantic 拒绝，
先记录、再分析并修复最终拒绝行为；不借此调整其他已正确拒绝用法的阶段归属。

#### OFC01 实施结果（2026-08-24）

- 新增 Parser 测试 `test_spec_instance_and_signature_parse_errors`，以 8 个独立程序覆盖实例
  字段初始值、实例方法函数体、实例/静态方法缺少返回类型，以及实例/静态方法参数的
  `let` / `var` 两个方向；没有修改已有测试；
- 当前拒绝阶段和诊断码确认如下：字段初始值由 Parser 报 `SE0603`，方法缺少返回类型由
  Parser 报 `SE0604`，方法函数体由 Parser 报 `SE0605`，方法参数显式 `let` / `var` 由
  Parser 报 `SE0606`；
- 专项命令 `make build/bin/test_parser`、`build/bin/test_parser` 通过；
- Codex 沙箱外完整 `make test` 通过：smoke 91/91、std 601/601、FCTS 874/874，
  `perf-constraints` 及其余完整回归项均通过；
- 未发现最终行为错误，未修改生产代码、runtime、ABI、`.ft` 或 witness 布局。

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

- [x] Semantic：实例 `let requirement` / `var implementation` 保留既有用例；
- [x] Semantic：新增实例 `var requirement` / `let implementation`，断言 `AE0702`；
- [x] Semantic：新增实例字段同 mutability、不同类型，断言 `AE0703`；
- [x] Semantic：新增静态字段两个 mutability 方向，均断言 `AE0702`；
- [x] Semantic：新增静态字段同 mutability、不同类型，断言 `AE0703`；
- [x] 运行 Semantic 专项测试和沙箱外 `make test`；
- [x] 在本文记录 OFC02 的新增用例、命令和结果。

该步骤只验证编译期满足检查，不需要 FCTS 或运行时改动。

#### OFC02 实施结果（2026-08-24）

- 新增 Semantic 测试 `test_object_spec_field_exact_satisfaction_matrix`，以 5 个独立程序
  覆盖实例 `var` requirement / `let` implementation、实例字段类型不匹配、静态字段
  两个 mutability 方向以及静态字段类型不匹配；没有修改已有测试；
- mutability 不匹配均确认由 Semantic 报 `AE0702`，类型不匹配均确认由 Semantic 报
  `AE0703`；每个程序只产生一个目标诊断；
- 专项命令 `make build/bin/test_semantic`、`build/bin/test_semantic` 通过；
- Codex 沙箱外完整 `make test` 通过：smoke 91/91、std 601/601、FCTS 874/874，
  `perf-constraints` 及其余完整回归项均通过；
- 未发现最终行为错误，未修改生产代码、runtime、ABI、`.ft` 或 witness 布局。

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

- [x] Semantic：type 实例方法的参数省略/`let`/`var` 均满足同一 requirement；
- [x] Semantic：type 静态方法的参数省略/`let`/`var` 均满足同一 requirement；
- [x] Semantic：fit 实例方法的参数省略/`let`/`var` 均满足同一 requirement；
- [x] Semantic：fit 静态方法的参数省略/`let`/`var` 均满足同一 requirement；
- [x] FCTS：至少让实例和静态 `var` 参数真实修改自己的局部绑定并断言结果；
- [x] Codegen：确认参数 mutability 不进入 witness 槽签名，不生成 adapter；
- [x] 运行 Semantic、Codegen、新增 FCTS 和沙箱外 `make test`；
- [x] 在本文记录 OFC03 的新增用例、命令和结果。

如果实现参数 mutability 进入 `.ft` 契约身份、ABI 或需要运行时 adapter，必须停止并由人工
决策。

#### OFC03 实施结果（2026-08-24）

- 新增 Semantic 测试 `test_object_spec_method_parameter_binding_satisfaction_matrix`，直接覆盖
  type/fit × 实例/静态 × 参数省略/`let`/`var` 共 12 条满足路径；
- 新增 FCTS `test_object_spec_contract_surface.ff` 和 4 条可观察测试；type 与 fit 的实例、
  静态实现均真实执行，`var` 参数在实现体内修改自己的局部绑定；
- 新增 Codegen 测试
  `test_object_spec_method_parameter_bindings_keep_witness_abi`，确认 requirement witness 只有
  既有 6 个槽，所有 `int` 参数与返回值保持 `int64_t` ABI，mutability 不进入槽签名，且
  不生成 `FengCallableInvoke__` / `FengCallableStaticInvoke__` adapter；普通 witness thunk
  保持既有路径；
- 专项命令 `make build/bin/test_semantic`、`build/bin/test_semantic`、`make fcts-tests`、
  `make build/bin/test_codegen`、`build/bin/test_codegen` 均通过，FCTS 为 878/878；
- Codex 沙箱外完整 `make test` 通过：smoke 91/91、std 601/601、FCTS 878/878，
  `perf-constraints` 及其余完整回归项均通过；
- ISSUE-001 已记录并解决；未修改生产代码、runtime、ABI、`.ft` 或 witness 布局。

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

- [x] 先在 `feng-spec.md` 明确父字段进入子完整成员面后按直接声明字段检查，同名即冲突；
- [x] Semantic：callable-form 父级拒绝，断言 `AE0613`；
- [x] Semantic：union-form 父级拒绝，断言 `AE0613`；
- [x] Semantic：intersection-form 父级拒绝，断言 `AE0613`；
- [x] Semantic：同一 object-form `spec` 体内直接重复实例字段、静态字段均拒绝；
- [x] Semantic：子级自有字段与直接/传递父字段同名时拒绝；
- [x] Semantic：多个父级提供完全相同字段时拒绝；
- [x] Semantic：多个父级字段类型不同或 mutability 不同时拒绝；
- [x] Semantic：覆盖实例字段、静态字段和 owner 泛参替换后产生同名字段的非等价路径；
- [x] 保留普通 type 父级、重复父级和直接/间接环用例不变；
- [x] 保留父方法叠加与重载用例不变；
- [x] 确认所有冲突均由 Parser 或 Semantic 拒绝，不进入 Codegen；
- [x] 运行 Semantic 专项测试和沙箱外 `make test`；
- [x] 在本文记录 OFC04 的新增用例、命令和结果。

#### 实施结果

- 主规范补齐普通 `type` 与 object-form `spec` 的字段成员面规则，并新增 `AE0514`：
  同一实例面或静态面中的重复字段在 Semantic 阶段拒绝；实例面与静态面相互独立；
- Semantic 新增四组覆盖：普通 `type` / object-form `spec` 直接重复字段、三种非
  object-form 父级、直接/传递/多父/闭合泛型父字段冲突，以及实例/静态同名字段合法路径；
- Codegen 新增实例/静态同名字段独立槽验证，确认实例 getter/setter 与静态
  getter/setter 分别生成且访问准确；默认 subject 只存储实例字段，不为静态字段增加
  subject 存储；
- FCTS 新增 `object-form spec field owner surfaces` 两项语言行为，覆盖同一 spec 直接声明
  和父子叠加后的实例/静态同名字段独立读写；
- 未修改已有用例、runtime 或 `.ft` 格式；已批准的 witness 修正只让受影响合法程序保留
  本应独立的两个字段成员面，不增加运行时查找、分支或分配；
- Semantic、Codegen 和 FCTS 专项均通过，FCTS 为 880/880；
- Codex 沙箱外完整 `make test` 通过：两套构建及 sanitizer 均通过，smoke 91/91、
  std 601/601、FCTS 880/880，`perf-constraints`、增量构建、发布脚本及其余完整回归项均通过；
- ISSUE-002、ISSUE-003 已记录、分析并解决。

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

- [x] FCTS：两个默认 spec 局部的字段状态与 identity 独立；
- [x] FCTS：默认 spec 数组各元素的字段状态与 identity 独立；
- [x] FCTS：默认方法返回 scalar、managed、aggregate 和 object-form `spec` 默认值；
- [x] Codegen：每个默认初始化站点调用既有 subject factory，不读取共享 subject 单例；
- [x] Codegen：数组元素初始化不复用同一个 subject；
- [x] 复用 `test_spec_upcast.ff` 已有“默认子 spec 投影到父 spec 后共享同一 subject”用例，
  不重复增加；
- [x] 运行 Codegen、新增 FCTS 和沙箱外 `make test`；
- [x] 在本文记录 OFC05 的新增用例、命令和结果。

#### 实施结果

- FCTS 新增 `object-form spec default subjects` 三项语言行为：分别覆盖两个局部默认值、
  两个默认数组元素，以及 scalar、managed、aggregate、object-form `spec` 四类默认方法返回值；
- 嵌套 object-form 返回值连续调用两次后具有独立字段状态和 identity；
- Codegen 新增槽级验证：两个局部默认值分别调用 object-form aggregate 默认初始化；默认
  初始化函数每次调用既有 `new_subject` factory，factory 每次执行 `feng_object_new`；数组构造
  携带同一 aggregate descriptor，由既有逐元素 `init_fn` 路径创建独立 subject；
- 复用了 `test_spec_upcast.ff` 的默认子到父投影测试，没有重复增加该场景；
- 未修改生产代码、已有用例、runtime、ABI、`.ft` 或 witness 布局；没有新增运行时开销；
- Codegen 专项和 FCTS 均通过，FCTS 为 883/883；
- Codex 沙箱外完整 `make test` 通过：两套构建及 sanitizer 均通过，smoke 91/91、
  std 601/601、FCTS 883/883，`perf-constraints`、增量构建、发布脚本及其余完整回归项均通过；
- 本组未发现最终行为错误，未新增实施过程问题。

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

- [x] 新增独立生命周期 FCTS，逐项覆盖上表；
- [x] 每项断言精确 created/finalized 计数，不只断言程序未崩溃；
- [x] Codegen：检查覆盖赋值和各控制流边使用既有 cleanup，不出现额外 box；
- [x] Codegen：检查没有因 spec 视角增加多余 retain/release；
- [x] 运行 Codegen、新增 FCTS 和沙箱外 `make test`；
- [x] 在本文记录 OFC06 的新增用例、命令和结果。

实施结果：

- 新增 `fcts/fcts_bin/src/test_object_spec_lifecycle.ff`，覆盖局部复制、局部/字段/
  数组元素覆盖、参数返回转发、提前返回、`break` / `continue`、异常与多视角
  生命周期；每组均精确断言 created/finalized 计数。
- 新增 Codegen 回归，直接检查局部、字段和数组元素的拥有源使用
  `feng_aggregate_take`，借用源保持 `feng_aggregate_assign`，且不为构造主体生成
  额外 managed cleanup 或 scalar box。
- 运行 `build/bin/test_codegen`、`make fcts-tests` 和沙箱外 `make test`。结果：
  Codegen 通过，smoke `91/91`、std `601/601`、FCTS `892/892`，UBSan、
  `perf-constraints`、增量构建、发布脚本、bundled packages 和 toolchain prebuilt 流程全部
  通过。

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

- [x] 先在 `feng-spec.md` 明确字段/方法同名冲突只发生在同一实例或静态冲突面；
- [x] Semantic：实例字段 + 实例方法同名时拒绝，复用普通 `type` 的同一冲突检查；
- [x] Semantic：静态字段 + 静态方法同名时拒绝，复用普通 `type` 的同一冲突检查；
- [x] Semantic/FCTS：实例字段 + 静态方法同名保持合法并可分别访问；
- [x] Semantic/FCTS：静态字段 + 实例方法同名保持合法并可分别访问；
- [x] Codegen：合法跨冲突面同名成员进入各自既有字段/方法路径，不增加运行时选择；
- [x] FCTS/Codegen：实例方法名等于 spec 名时走普通实例 requirement 槽；
- [x] FCTS/Codegen：静态方法名等于 spec 名时走普通静态 requirement 槽；
- [x] Semantic：object-form `spec` 使用 `@cdecl(...)` 时断言 `AE1305`；
- [x] 跨包 FCTS：公开 spec 经 `.ft` 恢复后，验证一次“默认值独立”和一次“方法名等于
  spec 名”的非等价路径；
- [x] 确认只消费既有 `.ft` 声明事实，不改变 `.ft` 格式；
- [x] 运行 Semantic、Codegen、新增 FCTS 和沙箱外 `make test`；
- [x] 在本文记录 OFC07 的新增用例、命令和结果。

#### OFC07 实施结果（2026-08-24）

- 主规范明确实例与静态 owner 分属不同冲突面；`spec Surface { let value: int;
  static let value: int; }` 合法，并保留两个独立 requirement；
- Semantic 新增 `test_object_spec_field_method_conflict_surface_matrix`，覆盖实例/静态、
  声明顺序、直接/父级/闭合泛型父级的同面字段-方法冲突，统一在 Semantic 阶段报
  `AE0513`；普通 `type` 的方法先于字段分支也复用同一检查；
- Semantic 新增 `test_object_spec_cross_surface_field_method_names_are_allowed`，证明实例字段
  + 静态方法、静态字段 + 实例方法、实例字段 + 静态字段三种跨 owner 组合均合法；
- Codegen 新增 `test_object_spec_special_names_keep_distinct_witness_slots`，确认跨 owner 同名
  字段/方法及“方法名等于 spec 名”均生成独立、编译期确定的既有 witness 槽，不生成
  callable adapter 或运行时成员选择；
- FCTS 新增 `test_object_spec_special_surface.ff` 四项本包行为；新增
  `test_object_spec_package_coverage.ff` 与库侧
  `lib_object_spec_complete_coverage.ff`，证明 `.ft` 恢复后的默认 subject 独立及同名方法
  调用；`.ft` 格式未修改；
- Semantic 新增调用方式注解负向用例，object-form `spec` 的 `@cdecl(...)` 报 `AE1305`；
- ISSUE-005、ISSUE-006 已记录、分析并解决；未修改已有用例、runtime、ABI 或 `.ft`
  格式，也未增加运行时分支、查找、分配或间接调用；
- `build/bin/test_semantic`、`build/bin/test_codegen` 与 `make fcts-tests` 通过，FCTS 为
  `898/898`；沙箱外完整 `make test` 通过：smoke `91/91`、std `601/601`、FCTS
  `898/898`，UBSan、`perf-constraints`、增量构建、发布脚本、bundled packages 和
  toolchain prebuilt 流程全部通过。

### OFC08：最终追踪与回归验收

#### 任务

- [x] 为 `feng-spec.md` 中每条直接适用于 object-form `spec` 的 `[必须]` / `[禁止]`
  建立“规范条目 → 测试文件/测试函数”映射；
- [x] 没有新增直接测试的条目，记录所复用的公共路径及不重复的原因；
- [x] 非法用法清单均记录当前由 Parser 或 Semantic 拒绝及诊断码，未为阶段归属迁移实现；
- [x] 所有实施过程发现的最终行为错误均已记录、分析并修复；
- [x] 不存在仍未交付的“计划问题”或“实施过程发现问题”；
- [x] 检查 object-form fat value 仍为既有 subject+witness 表示；
- [x] 检查新增测试没有要求 runtime、`.ft` 格式或 ABI 变更；
- [x] 检查生成 C 没有新增运行时成员搜索、签名比较、adapter 或额外分配；
- [x] `git diff --check` 通过；
- [x] 所有新增专项测试通过；
- [x] FCTS 全量通过；
- [x] 在 Codex 沙箱外运行全量 `make test` 并通过；
- [x] 在本文补齐每一步交付结果、测试命令和最终计数；
- [x] 状态更新为“已完成”。

#### 规范条目追踪表

以下按 `feng-spec.md` §5 的出现顺序列出全部直接适用于 object-form `spec` 的 37 条
`[必须]` / `[禁止]`。纯 callable-form、union-form 或 intersection-form 规则不列入本表；
它们各自由对应专项负责。表中 `P`、`S`、`C`、`Y`、`F` 分别表示 Parser、Semantic、
Codegen、Symbol 测试和 FCTS。

| ID | 规范条目（简述） | 直接证据或复用的公共路径 |
| --- | --- | --- |
| S01 | object-form 父列表由一个或多个 `spec` 组成并以逗号分隔 | `S::test_spec_parent_specs_must_be_spec`、`S::test_spec_parent_specs_rejects_duplicate`；合法单父/多父由 `F::test_spec.ff` 与 `F::test_object_spec_overload.ff` 直接执行 |
| S02 | object-form 父级只能是 object-form | `S::test_object_spec_rejects_non_object_parent_forms`，分别覆盖 callable/union/intersection 并断言 `AE0613` |
| S03 | `type` / `fit` 满足列表只能含 object-form | `S::test_type_declared_specs_must_be_spec`、`S::test_fit_specs_must_be_spec`、`S::test_union_form_spec_rejects_type_declared_spec_clause`、`S::test_union_form_spec_rejects_fit_spec_clause`；各 form 共用同一列表形态验证路径 |
| S04 | 字段按名称、`let`/`var`、类型精确满足 | `S::test_type_declared_specs_missing_field_rejected`、`S::test_object_spec_field_exact_satisfaction_matrix` |
| S05 | object-form 方法不得声明方法级泛参 | `P::test_object_spec_method_type_params_parse` 证明 AST 保留；`S::test_object_spec_method_type_params_rejected` 断言 `AE0331`；`S::test_generic_method_does_not_satisfy_non_generic_spec` 覆盖实现侧差异 |
| S06 | 方法按完整签名、owner 替换后精确满足 | `S::test_type_declared_specs_method_signature_mismatch_rejected`、`S::test_variadic_spec_satisfaction_mismatch_rejected`、`S::test_object_spec_method_parameter_binding_satisfaction_matrix`；`C::test_object_spec_method_parameter_bindings_keep_witness_abi` |
| S07 | requirement 与实现成员可见性兼容 | `S::test_spec_requirement_implementation_visibility_matrix`，实例/静态、字段/方法、type/fit 均覆盖并断言 `AE0707` |
| S08 | 跨包 fit 不得借用目标 type 的 `seal` 成员，同包允许 | `Y::test_imported_type_seal_members_do_not_satisfy_consumer_fit` 直接覆盖跨包拒绝；`S::test_same_package_fit_uses_target_seal_members` 覆盖同包分支；二者复用同一 target-owner 检查 |
| S09 | 满足契约的 type/fit 实现上下文可经 spec 视角访问 `seal` requirement | `S::test_spec_seal_member_access_from_implementation_contexts`、`F::test_spec.ff`、`F::test_spec_seal_dependency.ff` |
| S10 | `spec seal` 不扩大具体 type 成员可见性 | `S::test_spec_seal_member_access_rejected_outside_implementation`、`S::test_spec_seal_inheritance_and_overload_filtering`，非法访问断言 `AE0708` |
| S11 | 父成员是契约叠加，完整实例/静态方法面使用普通重载规则 | `S::test_object_spec_declared_method_overload_diagnostics`、`S::test_object_spec_accumulated_method_overload_diagnostics`、`F::test_object_spec_overload.ff` |
| S12 | 父 owner 泛参先替换，合法重载保留，返回冲突拒绝 | `S::test_object_spec_accumulated_method_overload_diagnostics`、`S::test_object_spec_overload_witness_keeps_exact_requirements`、`C::test_object_spec_closed_parent_witness_uses_exact_implementation` |
| S13 | 子/父完整等价 requirement 合并为一个，子声明代表；同体重复拒绝 | `S::test_object_spec_overload_witness_keeps_exact_requirements` 断言单槽及子声明 identity；`S::test_object_spec_declared_method_overload_diagnostics` 断言 `AE0508` |
| S14 | 父字段叠加后同面同名冲突；实例/静态字段同名合法且独立 | `S::test_object_spec_accumulated_field_conflicts_rejected`、`S::test_type_and_object_spec_cross_surface_fields_allowed`；`C::test_object_spec_cross_surface_fields_keep_distinct_slots`；`F::test_object_spec_field_surfaces.ff` |
| S15 | 字段/方法同一 owner 面同名冲突，跨 owner 合法 | `S::test_object_spec_field_method_conflict_surface_matrix`、`S::test_object_spec_cross_surface_field_method_names_are_allowed`；`C::test_object_spec_special_names_keep_distinct_witness_slots`；`F::test_object_spec_special_surface.ff` |
| S16 | 同一 type 满足多个 spec 时的返回类型冲突拒绝 | `S::test_type_declared_specs_cross_spec_method_conflict`，复用完整 requirement 冲突检查并报 `AE0706` |
| S17 | 满足子 spec 必须满足其传递父契约 | `S::test_type_declared_specs_transitive_satisfaction_required`、`S::test_spec_relation_declared_parent_transitive`；`F::test_spec.ff` |
| S18 | object-form 父关系不得成环 | `S::test_spec_parent_specs_rejects_cycle`，断言 `AE0614` |
| S19 | 同一声明头不得重复列出 spec | `S::test_spec_parent_specs_rejects_duplicate`、`S::test_type_declared_specs_rejects_duplicate`、`S::test_fit_specs_rejects_duplicate` |
| S20 | type/fit 不得声明满足 callable/union/intersection form | 与 S03 共用列表形态验证；union 由两条 `test_union_form_spec_rejects_*_clause` 直接覆盖，其余 form 进入相同 kind 分支，不重复等价用例 |
| S21 | object-form 方法参数不得写 `let` / `var` | `P::test_spec_instance_and_signature_parse_errors`，实例/静态及两个修饰方向均断言 `SE0606` |
| S22 | object-form 不得声明终结器 | `S::test_object_form_spec_rejects_finalizer_member`，断言 `AE0620` |
| S23 | object-form 静态 requirement 无初始化器/函数体、以分号结束且方法有返回类型 | `P::test_spec_static_member_parse_errors`、`P::test_spec_instance_and_signature_parse_errors`，断言 `SE0603`–`SE0605` |
| S24 | 实例/静态字段与方法允许 `open` / `seal`，AST 保留且 Semantic 解释 | `P::test_spec_seal_members_parse`、`P::test_spec_static_members_parse`；`S::test_spec_requirement_implementation_visibility_matrix` |
| S25 | `seal` object-form requirement 可使用 `@friend`，授权经 witness | `S::test_friend_spec_member_access_uses_spec_view`、`S::test_friend_declaration_and_access_diagnostics`；`F::test_friend.ff` |
| S26 | object-form 参数/局部值的实例方法值绑定形成点 subject+witness+requirement | `S::test_object_spec_method_values_record_exact_target_context`、`S::test_object_spec_method_values_reject_invalid_sources`；`C::test_object_spec_method_value_codegen_uses_bound_witness`；`F::test_spec_method_value.ff` |
| S27 | object-form 约束泛型值的方法值保持完整 `T` receiver | `S::test_constrained_generic_spec_method_values_preserve_receiver`；`C::test_constrained_generic_spec_method_value_codegen`；`F::test_spec_method_value.ff` |
| S28 | object-form 约束类型参数的静态方法值复用静态 requirement/witness 且无 receiver | `S::test_constrained_generic_spec_static_method_values` 及其负向用例；`C::test_constrained_generic_spec_static_method_value_codegen`；`F::test_spec_static_method_value.ff` |
| S29 | 静态字段只由 type 满足；静态方法可由 type 或 fit 满足 | `S::test_type_satisfies_spec_static_members`、`S::test_fit_satisfies_spec_static_method`、`S::test_spec_static_field_missing_rejected`、`S::test_spec_static_member_missing_rejected` |
| S30 | object-form 禁止 `@abi` 和调用方式注解 | `S::test_object_form_spec_rejects_abi_annotation` 断言 `AE1304`；`S::test_object_form_spec_rejects_calling_convention_annotation` 断言 `AE1305` |
| S31 | 只允许具体 type→已满足 spec、子 spec→父 spec 两类向上视角 | `S::test_object_spec_upcast_records_selected_parent_paths`、`S::test_spec_relation_declared_head_recorded`、`S::test_spec_relation_declared_parent_transitive` |
| S32 | 向上视角适用于规定站点，不扩展为一般隐式转换 | `F::test_spec_upcast.ff` 直接覆盖初始化、赋值、传参、返回、字段和数组写入；`S::test_object_spec_upcast_rejects_out_of_scope_conversions` 覆盖无名义关系拒绝 |
| S33 | object-form 向上视角参与声明期重载重叠检查 | `S::test_top_level_overload_overlap_via_two_specs_rejected`、`S::test_member_method_overload_overlap_via_fit_rejected`、`S::test_object_spec_declared_method_overload_diagnostics`，断言 `AE0706` |
| S34 | 显式 cast 只建立相同两类父视角 | `S::test_object_spec_upcast_records_selected_parent_paths`、`S::test_object_spec_upcast_rejects_out_of_scope_conversions`；`F::test_spec_upcast.ff` |
| S35 | coercion/cast 资格仅由编译期可见关系决定 | `S::test_spec_relation_visibility_filter`、`S::test_object_spec_upcast_records_selected_parent_paths`、`S::test_object_spec_upcast_rejects_out_of_scope_conversions` |
| S36 | 合法视角直接构造目标 subject+witness，不做运行时搜索 | `C::test_object_spec_upcast_witness_and_lowering_codegen` 断言静态 witness 路径；`F::test_spec_upcast.ff` 断言单次求值和 identity |
| S37 | 父→子、无关 spec、依赖运行时具体类型的转换均禁止 | `S::test_object_spec_upcast_rejects_out_of_scope_conversions`，隐式拒绝为 `AE1003`、显式拒绝为 `AE0051` |

S03/S20 以及少量“合法/非法两面”的条目复用公共实现路径，是因为 form kind、列表形态、
可见性和转换资格均在 Semantic 的统一入口检查；重复增加仅替换声明名的等价程序不会进入
不同分支。其他表项均给出直接的非等价测试。

#### 非法用法的当前拒绝阶段

本专项只记录实际阶段，不迁移已有诊断归属：

| 非法用法 | 当前阶段 | 诊断码 |
| --- | --- | --- |
| spec 字段初始化器、方法缺少返回类型、方法体、参数 `let` / `var` | Parser | `SE0603`、`SE0604`、`SE0605`、`SE0606` |
| 非 object-form 父级；父级重复或成环 | Semantic | `AE0613`、`AE0614` |
| type 满足列表含非 spec / 非 object-form 或重复 | Semantic | `AE0615`、`AE0616` |
| fit 满足列表含非 spec / 非 object-form 或重复 | Semantic | `AE0809`、`AE0810` |
| object-form 方法级泛参；终结器 | Semantic | `AE0331`、`AE0620` |
| 同面重复字段；同面字段/方法同名 | Semantic | `AE0514`、`AE0513` |
| 字段缺失、mutability 不同、类型不同 | Semantic | `AE0701`、`AE0702`、`AE0703` |
| 方法签名不同或缺失 | Semantic | `AE0704`、`AE0705` |
| 重复方法、仅返回类型不同、变长参数冲突、可见契约重叠 | Semantic | `AE0508`、`AE0509`、`AE0510`、`AE0706` |
| requirement/实现可见性不兼容；越权访问 `spec seal` | Semantic | `AE0707`、`AE0708` |
| object-form 使用 `@abi` 或调用方式注解 | Semantic | `AE1304`、`AE1305` |
| 非法隐式向下/无关转换；非法显式转换；调用点重载二义性 | Semantic | `AE1003`、`AE0051`、`AE0511` |

#### OFC08 最终验收结果（2026-08-24）

- 上表已经逐条映射全部 37 条直接适用于 object-form 的主规范规则；纯 callable、union、
  intersection 规则继续由各自专项负责，没有扩大本文件范围；
- ISSUE-001–ISSUE-006 均已记录根因、方案和验证结果并关闭；没有 ISSUE-007，也没有仍处于
  “计划修复”或“发现后待处理”的问题；
- `test_value_kind_object_form_spec_is_aggregate` 与 Codegen 结构检查确认 object-form fat value
  仍为既有 `{ void *subject; const Witness *witness; }` 两字段 aggregate；
- 变更清单不含 runtime 或 `.ft` 序列化/格式代码；runtime ABI、Feng/C ABI 和 `.ft` 格式
  未改变。ISSUE-003 仅按人工批准修正原本错误合槽的合法程序 witness；
- 生成 C 继续直接选择编译期已知 witness 槽；专项断言不存在新增 callable adapter、运行时
  成员搜索或签名比较。默认值与生命周期测试确认没有额外 subject 分配、retain/release 或
  cleanup；ISSUE-004 改用既有 `feng_aggregate_take` 后还移除了原错误路径的一组
  retain/release；
- 没有修改已有测试用例的源码或预期，只新增测试函数/文件并在入口追加注册或调用；
- 最终专项命令 `build/bin/test_parser`、`build/bin/test_semantic`、
  `build/bin/test_codegen`、`make fcts-tests` 全部通过，FCTS 为 `898/898`；
- 最终 `git diff --check` 通过；
- 最后一次代码与测试状态的沙箱外完整 `make test` 通过：smoke `91/91`、std `601/601`、
  FCTS `898/898`，UBSan、`perf-constraints`、增量构建、发布脚本、bundled packages 与
  toolchain prebuilt 流程全部通过。此后只更新本文验收记录，无代码或测试变更。

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

实例字段与静态字段的 owner 不同，不在同一成员面；二者允许同名，并且必须保留为两个
独立 requirement。

D02 只补齐字段规则。父方法继续按既有普通方法重载规则处理，不改变合法重载、重复签名、
仅返回类型差异及完整签名等价 requirement 的既有规则。

## 6. 实施过程问题记录

新增用例暴露最终行为问题时，必须先填写以下模板，再分析和修改生产代码。规则与方案明确
且不触碰人工门槛时，记录完成后继续修复；只有不确定或触碰强制约束时才停止等待人工
决策。

### ISSUE-001：OFC03 Codegen 槽签名断言未匹配实际生成文本

- 发现阶段：OFC03 Codegen 专项测试。
- 最小复现：`Transformer` requirement 参数为 `int`，type/fit 分别以省略、`let`、`var`
  参数实现；生成 witness 后断言实例槽为
  `int32_t (*plain)(void *_subject, int32_t);`。
- 当前结果：Semantic 与 FCTS 均通过；实际六个槽全部使用既有 Feng `int` 的
  `int64_t` C ABI，实例槽保留 `_subject`，静态槽不含 `_subject`，槽中没有
  mutability 信息。
- 预期结果：实现参数 mutability 不进入 requirement witness 槽签名，不为其生成额外
  callable adapter。
- 影响范围：当前仅确认新增 Codegen 测试断言失败；尚无证据表明语言行为或 ABI 错误。
- 是否涉及运行时开销：否。
- 是否涉及 ABI / `.ft` / witness 布局：当前不变更；若分析表明必须变更，需人工决策。
- 根因分析：新增测试误把 Feng `int` 的既有 C ABI 写成 `int32_t`；实现没有错误。
- 建议方案：仅把新增测试的六条槽签名断言改为 `int64_t`，不修改生产代码或 ABI。
- 人工决策：当前无需；若实际行为不符合既有 ABI，再停止决策。
- 状态：已解决；修正新增断言后 Codegen 专项通过。

### ISSUE-002：OFC04 缺少字段冲突的声明期检查

- 发现阶段：OFC04 现状探针。
- 最小复现：

  ```feng
  spec Parent {
    let value: int;
  }

  spec Child: Parent {
    let value: int;
  }
  ```

- 当前结果：`Child` 通过 Semantic 和 Codegen；
  `object_spec_requirement_set_add` 按名称保留子级字段并静默丢弃父级字段。直接在同一
  object-form `spec` 中声明两个同面同名字段也会通过。
- 预期结果：按 §5 D02，同一实例或静态成员面中的同名字段必须在 Semantic 阶段报冲突，
  不得进入 Codegen；完全相同的字段 requirement 也不能合并。
- 影响范围：object-form `spec` 的直接字段、直接/传递父字段、多父字段和闭合泛型父字段；
  方法叠加规则不变。
- 是否涉及运行时开销：否；检查只在 Semantic 声明验证阶段执行。
- 是否涉及 ABI / `.ft` / witness 布局：否；只拒绝按 D02 本来非法的声明，不改变合法
  程序的 witness 布局。
- 根因分析：当前只有方法的声明内与父级叠加冲突检查，没有字段对应检查；公共 requirement
  收集器沿用旧的一名一槽逻辑，因而掩盖了非法字段冲突。
- 建议方案：新增编译期 object-form 字段成员面检查，先按父引用完成 owner 替换并遍历完整
  父闭包，再按实例/静态面比较字段名；新增专用 AE 诊断。验证通过后才允许既有 requirement
  收集与 Codegen 继续执行，不添加运行时状态或分支。
- 补充事实：用于核对“与普通 `type` 是否已有同一诊断”的独立探针表明，普通 `type` 的
  直接重复字段当前也没有在 Parser/Semantic 拒绝，而是在生成 C 后由宿主编译器报告
  `duplicate member`。
- 人工决策：已批准同时补普通 `type` 的直接重复字段声明检查，并与 object-form `spec`
  复用声明期字段冲突基础逻辑。
- 实施结果：新增普通 `type` 与 object-form `spec` 共用的直接声明字段冲突检查，并在
  object-form 父闭包完成 owner 替换后检查叠加字段；所有冲突均以 `AE0514` 在 Semantic
  阶段拒绝，不进入 Codegen。
- 状态：已解决；专项测试和完整回归均通过。

### ISSUE-003：合法的实例/静态同名字段被合并为一个 witness 槽

- 发现阶段：OFC04 修复方案的 witness 布局核对。
- 最小复现：

  ```feng
  spec Surface {
    let value: int;
    static let value: int;
  }

  type Impl: Surface {
    let value: int = 1;
    static let value: int = 2;
  }
  ```

- 当前结果：Semantic 接受该合法声明，但 requirement 收集与 Codegen 都只按裸名称合槽，
  最终 witness 只生成 `int64_t (*get_value)(void)` 静态槽；通过 spec 实例读取
  `value.value` 时却向它传入 subject，宿主 C 编译器报告函数参数过多。
- 预期结果：实例字段与静态字段属于不同成员面，必须分别保留并可独立访问；只有同一成员
  面中的同名字段才按 D02 拒绝。
- 影响范围：object-form `spec` 自有及父级叠加成员中，实例字段与静态字段同名的合法程序。
- 是否涉及运行时开销：不影响其他程序；该合法组合必须携带两个本来就不同的 getter/setter
  requirement，因而其 witness 需要对应的独立槽，不增加运行时查找、分支或分配。
- 是否涉及 ABI / `.ft` / witness 布局：需要改变该受影响程序的生成 witness 布局，由一个
  错误合并槽改为两个独立槽；`.ft` 格式本身无需改变。根据 §2 强制约束，必须人工批准后
  才能实施。
- 根因分析：`object_spec_requirement_set_add` 和
  `cg_user_spec_record_object_member` 的旧逻辑均把成员裸名称当作唯一槽键，没有把
  `is_static` 纳入字段槽身份。
- 建议方案：字段槽身份至少使用“字段名 + 实例/静态面”；同一面同名字段在 Semantic
  声明验证中前置拒绝，不进入收集与 Codegen；不同面字段保留为两个确定槽，并使用稳定、
  可区分的 C 字段名。该处理是通用成员面规则，不按测试名称或具体类型特判。
- 人工决策：已确认实例字段与静态字段 owner 不同，允许同名且不存在冲突或歧义；批准
  为该合法组合保留独立 requirement，并实施对应的受限 witness 布局修正。
- 实施结果：requirement 收集和 Codegen 槽注册均按字段所属实例/静态成员面区分；Semantic
  为字段访问记录精确成员，Codegen 据此选择对应槽；默认 subject 不存储静态字段。
- 状态：已解决；Codegen 槽级测试、FCTS 语言行为和完整回归均通过。

### ISSUE-004：object-form 覆盖赋值后旧 subject 未按预期立即终结

- 发现阶段：OFC06 FCTS。
- 最小复现：

  ```feng
  spec ResourceView {
    func id(): int;
  }

  type Resource: ResourceView {
    open static var finalized: int = 0;
    let value: int;

    func id(): int { return self.value; }
    func ~Resource() { Resource.finalized += 1; }
  }

  var view: ResourceView = Resource { value: 1 };
  view = Resource { value: 2 };
  assert(Resource.finalized == 1);
  ```

- 当前结果：局部 `var`、普通 type 的 object-form 字段、object-form 数组三种覆盖路径均未
  通过“赋值语句完成后 `finalized == 1`”断言；对应作用域完全退出后的
  `created == 2 && finalized == 2` 断言均通过。
- 预期结果：覆盖赋值完成后旧 subject 已无引用时立即终结；新 subject 在最终持有者退出时
  终结。每个创建的 subject 只终结一次。
- 影响范围：普通 object-form fat value 的局部、字段与数组元素覆盖赋值；
  直接由拥有式托管值构造 spec fat value 的其他持久化站点也受同一所有权
  降格影响。
- 是否涉及运行时开销：否。修复不增加运行时状态、分支、分配或 helper
  调用；拥有源由“`assign` + 源 cleanup”改为既有 `take` 路径，反而去掉一组
  retain/release。
- 是否涉及 ABI / `.ft` / witness 布局：否。只修正 Codegen 内部 `ExprResult`
  所有权传递和已有 aggregate helper 的选择。
- 根因分析：对象构造表达式原本产生 `owns_ref=true` 的 `+1` 引用；对象→
  object-form spec coercion 调用 `cg_materialize_to_local` 后，把该引用登记到
  当前词法作用域 cleanup，并把生成的 fat value 错误标为借用。持久槽因此
  使用 `feng_aggregate_assign` 再 retain 一次，而隐藏源引用要到整个词法
  作用域退出才释放。这与 `feng-lifetime.md` 规定的“当前求值过程临时值在
  表达式结束时释放”不一致，也违反已有 object-form Codegen 方案中“拥有源
  直接搬移，不额外 retain”的规则。
- 建议方案：用 Codegen 内部的“单次求值但保留所有权”别名物化取代
  coercion 中的作用域托管临时值；生成的 fat value 继承源 `owns_ref`。持久
  aggregate 槽使用同一通用准备路径：拥有源调用 `feng_aggregate_take`，
  借用源调用 `feng_aggregate_assign`。不增加 object-form 专用 runtime 分支。
- 人工决策：当前无需；若修复必须改变 runtime、ABI、已有用例或增加运行时开销则停止。
- 实施结果：新增所有权保留别名物化和通用 aggregate 持久槽源准备；对象、
  `string` 与数组主体的 object-form coercion 保留源 `owns_ref`，局部、字段
  与数组元素对拥有源使用 `take`、对借用源保持 `assign`。三条覆盖路径
  不再为构造主体登记隐藏 managed cleanup。
- 验证结果：新增 Codegen 所有权选路测试通过；OFC06 FCTS 九项全部通过，
  完整回归中 smoke `91/91`、std `601/601`、FCTS `892/892`，其余阶段全部通过。
- 状态：已解决；专项测试和完整回归均通过。

### ISSUE-005：object-form 字段与方法同面同名未在 Semantic 拒绝

- 发现阶段：OFC07 生产实现核对。
- 最小复现：

  ```feng
  spec InvalidInstance {
    let value: int;
    func value(): int;
  }

  spec Parent {
    static let seed: int;
  }

  spec InvalidChild: Parent {
    static func seed(): int;
  }
  ```

- 当前结果：普通 `type` 已调用同一实例/静态冲突面的字段与方法同名检查，但现有实现只
  比较“字段声明在前、方法声明在后”的顺序，反向声明仍会漏诊；object-form `spec` 完全
  未调用该检查，直接声明可通过 Semantic。父级叠加路径的 requirement 收集器还会按子级
  优先静默保留同面同名成员之一，因此跨父级的冲突也不会形成诊断。
- 预期结果：按 `feng-spec.md` 和 §5 D01，两个示例均在 Semantic 阶段报 `AE0513`；实例
  字段 + 静态方法、静态字段 + 实例方法以及实例字段 + 静态字段继续合法。
- 影响范围：普通 `type` 中方法先于字段的声明顺序；object-form `spec` 的直接成员，以及
  直接、传递、多父和闭合泛型父级叠加后的完整实例/静态成员面。
- 是否涉及运行时开销：否；检查只在 Semantic 声明验证阶段执行。
- 是否涉及 ABI / `.ft` / witness 布局：否；只拒绝本来非法且会丢失一个 requirement 的
  声明，不改变合法程序的声明恢复或 witness 布局。
- 根因分析：现有字段/方法同名验证函数只接受普通 `type`，且只从方法向前查找字段；
  object-form 的完整父级成员面也没有对应的声明期冲突遍历。requirement 收集器中的旧
  兼容兜底只能避免非法声明继续产生重复槽，不能代替 Semantic 诊断。
- 建议方案：把直接声明检查改为普通 `type` 与 object-form `spec` 共用；另以编译期临时
  集合遍历 object-form 的闭合父级成员面，只比较 `is_static` 相同且类别分别为字段/方法的
  同名成员。跨实例/静态面不比较，不增加运行时状态、分支或分配。
- 人工决策：无需；规则已由 §5 D01 和“父级成员按子级直接成员统一检查”的既有结论唯一
  确定。若实施中必须改变合法程序的 ABI、`.ft`、witness 或运行时成本则停止。
- 实施结果：普通 `type` 与 object-form `spec` 的直接声明共用双向、与声明顺序无关的
  字段/方法冲突检查；object-form 的闭合父级成员面在完成 owner 替换后执行同一
  instance/static owner 规则。非法组合均在 Semantic 阶段报 `AE0513`，跨 owner 组合保持
  合法。
- 验证结果：`test_object_spec_field_method_conflict_surface_matrix`、
  `test_object_spec_cross_surface_field_method_names_are_allowed`、Codegen 特殊槽测试、FCTS
  本包/跨包用例及完整 `make test` 全部通过。
- 状态：已解决；专项测试和完整回归均通过。

### ISSUE-006：OFC07 Codegen 新增槽名断言未匹配实际生成文本

- 发现阶段：OFC07 Codegen 专项测试。
- 最小复现：合法的实例字段 `instanceOwned` 与静态方法 `instanceOwned()` 同名，新增测试
  预期 witness 中静态方法槽文本为 `(*instanceOwned)(void)`。
- 当前结果：Semantic 通过，Codegen 生成成功；测试在该精确槽名断言处失败，尚未进入生成
  C 编译步骤。
- 预期结果：两个成员应进入各自既有、可区分的槽，并由调用点静态选择；不要求绕过既有
  C 标识符重载命名规则。
- 影响范围：当前仅确认新增 Codegen 断言；是否存在最终行为错误待检查实际生成 C。
- 是否涉及运行时开销：否；实际代码无误，仅新增测试的文本断言错误。
- 是否涉及 ABI / `.ft` / witness 布局：否；禁止为了匹配测试改变槽命名或布局，实施时也
  未作此类修改。
- 根因分析：既有槽命名器把同一 Feng 名称下的字段和方法共同编号；字段仍生成
  `get_instanceOwned` / `set_instanceOwned`，同名静态方法因此稳定命名为
  `instanceOwned__feng_overload_2`。另一组成员遵循相同规则。新增断言错误地假设字段不参与
  该编译期 C 标识符编号。
- 建议方案：按实际稳定命名修正新增测试，继续断言 getter/setter、实例方法、静态方法的
  六个独立槽及各自调用路径；不修改生产 Codegen。
- 人工决策：当前无需；若发现必须改变 ABI、`.ft`、witness 或运行时成本则停止。
- 实施结果：仅修正新增测试的槽名和调用文本断言；实际生成 C 已证明六个槽独立存在，
  调用点均直接选择对应槽，没有运行时成员选择。
- 验证结果：修正后的 Codegen 专项、FCTS 及完整 `make test` 全部通过。
- 状态：已解决；专项测试和完整回归均通过。

### 后续问题记录模板

本专项最终验收时没有 ISSUE-007。后续若扩展本专项，仍须先记录发现阶段、最小复现、当前
与预期结果、影响范围、运行时/ABI/`.ft`/witness 影响、根因、通用方案和人工决策状态，
再修改生产代码；本模板不表示存在未解决问题。

如果问题的预期不能由主规范或 §5 唯一确定，或者修复需要修改已有用例、增加运行时开销、
改变 ABI / `.ft` / witness 布局，立即停止该分支，由人工决策。

## 7. 测试文件组织

为保证每一步可独立交付，建议新增文件而不是把不同职责继续堆入已有大型 FCTS：

- OFC03：`fcts/fcts_bin/src/test_object_spec_contract_surface.ff`；
- OFC05：`fcts/fcts_bin/src/test_object_spec_default_subject.ff`；
- OFC06：`fcts/fcts_bin/src/test_object_spec_lifecycle.ff`；
- OFC07 本包：`fcts/fcts_bin/src/test_object_spec_special_surface.ff`；
- OFC07 跨包：
  `fcts/fcts_lib/src/test/lib_object_spec_complete_coverage.ff` 与
  `fcts/fcts_bin/src/test_object_spec_package_coverage.ff`；
- Parser/Semantic/Codegen 在对应 C 测试文件末尾新增独立测试函数，只在入口追加调用；
- `fcts/fcts_bin/src/main.ff` 只追加新测试入口调用，不改写已有入口。

## 8. Review 清单

- [x] 认可 §1 的“完整覆盖”口径；
- [x] 认可 OFC01–OFC08 的范围和实施顺序；
- [x] OFC02 准确记录字段 `let` / `var` 必须双向精确匹配；
- [x] OFC03 准确记录实现参数省略/`let`/`var` 均可满足 requirement，且不进入 ABI；
- [x] §5 准确记录 D01 的同一冲突面拒绝、跨冲突面允许规则；
- [x] §5 准确记录 D02 的父字段按子级直接成员处理规则；
- [x] 非法用法的 Parser/Semantic 拒绝阶段只记录，本专项不迁移；
- [x] 最终行为错误先记录、再分析并修复；不确定或触碰人工门槛时停止。
