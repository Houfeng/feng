# G25：泛型诊断用例补齐实施文档

> 状态：待 Review，实施范围已细化，尚未实施
>
> 所属实施文档：[Feng 语言正确性用例补齐实施文档](./feng-language-conformance-coverage-hardening-pending.md)
>
> 交付组：G25
>
> 静态盘点基线：2026-09-10，`4a3ce618`

本文独立维护 G25 的测试重点、稳定诊断映射、用例 TODO、实施边界、验收与交付记录。通用授权边界、
测试层级、分组原则与 TODO 状态规则沿用
[总实施文档第 1～4 节](./feng-language-conformance-coverage-hardening-pending.md#1-文档定位)，不因文档拆分改变
本组范围或批准状态。

## 1 依据、目标与边界

### 1.1 权威依据

- [泛型主规范](../specifications/feng-generics-draft.md)：泛型声明、identity、约束、显式实例化、
  推导和 callable value 规则；
- [AE 语义错误码规范](../specifications/feng-error-codes-ae.md)：错误码归属及“同类错误共码”规则；
- [函数规范](../specifications/feng-function.md)、[类型规范](../specifications/feng-type.md)和
  [`spec` 规范](../specifications/feng-spec.md)：重载、构造、成员及契约关系；
- [类型 arity 与重载实施记录](./feng-type-arity-overload-dev.md)：已经交付的 `(name, arity)`
  identity、精确解析及裸构造边界。

本文只把主规范中的规则转化为可执行测试，不另行定义泛型语义。主规范不能唯一确定预期时，必须先在
[G25 问题记录](./feng-language-conformance-coverage-hardening-issues/g25.md)登记并取得人工决定，不能在实现或
测试中自行选择行为。

### 1.2 测试目标

G25 验证以下编译期边界产生稳定、同根因复用的 Semantic 诊断，并为相邻合法行为提供真实运行证据：

- 类型参数名称和泛型声明 identity；
- 泛型类型、构造目标、函数和方法的显式类型实参数量；
- 约束引用形态、直接 owner 实例化、开放泛参转传及 callable 候选筛选；
- 从实参、receiver 静态类型和目标类型进行的类型参数推导；
- 显式泛型 target 的名称、目标能力和消费位置。

### 1.3 与其他交付组的边界

- `<...>`、旧 `:<...>`、缺少分隔符及 postfix 消歧等纯语法错误映射已交付的 G12，不在 G25
  人工构造非法 AST；
- 普通绑定重名映射 G13；非泛型调用参数、普通重载和 callable 目标结构映射 G15；
- tuple 作为约束的既有证据来自 G20，但其错误码按本组统一到通用约束形态码；
- 泛型类型引用的未知名称、裸名、非泛型目标及 arity 基础证据映射 G21／G22；
- object-form `spec` 方法级泛参限制和 fit／spec 名义满足映射 G23；
- union／intersection 特有的约束准入、收窄、组成关系、开放约束投影及完整值承载映射 G24；
  G25 只覆盖所有约束形式共享的机制，不重复同构复合类型矩阵。

本组不定义泛型与 `extern`、`@abi`、C ABI 的交叉规则，不修改 runtime 私有 ABI、公开 ABI、`.ft`
格式或运行时表示，也不引入运行时约束检查、装箱、动态搜索或额外分派。若实施需要任一上述变更或
新增运行时成本，必须记录问题并暂停，取得人工决定后才能继续。

## 2 错误码复用与收敛决定

### 2.1 分配规则

每个候选诊断先按 AE 主规范核对“冲突对象、根因、修复动作”。三者均相同时必须复用同一错误码，
不得仅因声明种类、语法入口、显式／推导或错误文案不同而拆码；任一不同且没有现有码可表达时，才可
提出新增错误码。新增提议必须先更新 AE 主规范并经人工 Review，不能先在代码中占号。

按该规则完成静态盘点后，G25 **不需要新增错误码**。本组只扩展现有码的适用入口，并收敛三个历史
发码出口：`AE0304 -> AE0709`，以及 `AE0232`／`AE0233 -> AE1014` 或 `AE1015`。

### 2.2 稳定码映射

| 错误对象与根因 | 稳定错误码 | G25 决定 |
|---|---|---|
| 同一类型参数列表重名；方法类型参数遮蔽 owner 类型参数 | `AE1017` | 都是类型参数名称冲突，修复均为重命名或删除重复参数；扩展现有码，不新增。 |
| 同 category 的具名 `type`／`spec` 声明 identity 重复 | `AE0213` | 泛型与非泛型复用既有声明重复码；identity 仍按名称和 arity 判定，G25 不拆泛型专用码。 |
| `type` 与 `spec` 等跨 category name-only 冲突 | `AE0004` | 映射 G13／既有 arity 证据，不因声明带泛参改变根因。 |
| 顶层函数或 type 方法重复签名 | `AE0508` | 泛型参数名和约束不参与签名；复用普通重复签名码。 |
| fit 方法重复签名 | `AE0801` | 冲突对象是 fit 方法面，复用既有 fit 重复签名码。 |
| 构造函数／终结器声明方法级泛参 | `AE0316` | 复用既有特殊成员声明限制。 |
| object-form `spec` 方法声明方法级泛参 | `AE0331` | 直接映射 G23 已交付证据。 |
| 约束解析到非 `spec`，包括普通 type、tuple 或其他非 spec 类型 | `AE0709` | 对象都是约束引用，根因都是“约束不是 spec”，修复都是改用 spec；`AE0304` 停止新发码。 |
| 泛型 type／spec owner 的具体实参或开放泛参不满足声明约束 | `AE0710` | 扩展现有 owner 约束码；包括父 `spec` 转传，定位不满足约束的类型实参。 |
| 泛型 callable 候选的推导结果或显式实参不满足约束 | `AE0512` | 该候选先被排除；没有候选存活时，调用根因是无匹配重载，不改报 `AE0710`。 |
| 已唯一选定的泛型 callable 仍有类型参数无推导来源 | `AE0525` | 只用于“已选 callable 的推导不完整”，定位第一个未解析类型参数。 |
| 对类型参数本身继续写类型实参 | `AE1012` | 复用既有类型参数误实例化码。 |
| 泛型类型引用、约束或类型实参中的类型名称不存在 | `AE1013` | 复用类型解析码；普通值／callable 名称不存在仍映射 G22／G15，不因带 `<...>` 改成类型错误。 |
| 对非泛型 type、函数或方法写显式类型实参 | `AE1014` | 都是目标没有类型参数；扩展现有码到 callable target，不再发 `AE0232`／`AE0233`。 |
| 泛型 type、构造目标、函数或方法的显式类型实参数量不匹配 | `AE1015` | 都是已解析泛型目标的 arity 不匹配；callable 有多个可用 arity 时列出集合，不任选一个候选；不再发 `AE0232`／`AE0233`。 |
| 显式泛型 target 未被合法调用、构造、数组创建或明确 callable 目标消费 | `AE1016` | 复用既有未消费 target 诊断。 |
| 类型位置裸名引用泛型 type／spec | `AE0006` | 复用已交付的通用裸泛型类型引用码。 |
| 泛型 callable value 未显式闭合，或闭合后与目标 callable 不匹配 | `AE0522` | 目标 callable 不反向推导来源泛参；显式 cast 的不兼容仍使用 `AE1023`。 |
| 裸泛型类型构造目标无法按 `(name, arity=0)` 解析 | `AE0315` | 构造目标不从构造参数或目标类型推导 owner 泛参，保持已交付决定。 |
| 裸泛型静态 owner 无法按 arity 0 形成可调用目标 | `AE0512` | 复用调用无匹配码，不新增 owner 推导码。 |

同一源码若含多个独立根因，必须断言完整诊断集合；G25 新增的最小非法程序原则上每例只保留一个根因，
避免用错误顺序掩盖映射。诊断文案可以按目标种类提供不同模板，但模板差异本身不产生新错误码。

### 2.3 已知实现差异与迁移白名单

静态盘点确认三项实施前差异，详细记录于
[ISSUE-G25-001～003](./feng-language-conformance-coverage-hardening-issues/g25.md)：

1. 当前类型参数作用域入口直接追加同一列表的参数，未检查列表内重名；需补通用 Semantic 检查并发
   `AE1017`；
2. 显式构造／callable arity 的后置检查仍发未进入现行 AE 分段规范的 `AE0232`／`AE0233`；需按目标
   是否泛型分别统一为 `AE1014`／`AE1015`；
3. tuple 约束单独发 `AE0304`，与普通非 spec 约束的对象、根因和修复动作相同；需统一为 `AE0709`。

实施时既有测试默认保持原样。仅以下三条既有错误码断言需要随稳定码收敛迁移，源码、位置、数量和
其他断言不得改变：

| 文件与测试函数 | 允许修改的既有断言 | 原因 |
|---|---|---|
| `test/semantic/test_semantic.c`，`test_g20_tuple_nominal_and_conversion_diagnostics` | 两个 `AE0304` 期望改为 `AE0709` | tuple 与其他非 spec 约束统一根因。 |
| `test/codegen/test_generic_argument_static.c`，`arguments_reject_invalid_actuals` | 第三个 case 的 `AE0233` 期望改为 `AE1015` | `id` 是泛型 callable，错误是显式类型实参 arity 不匹配。 |

上述表是 G25 的**拟批准白名单**，不是当前已取得的既有测试修改授权。只有人工 Review 明确批准 G25
实施及该表后才能修改；发现其他既有测试必须变化时，先在问题记录逐条列出并再次取得批准。

## 3 现有证据基线

| 现有证据 | 已证明内容 | G25 缺口或处理 |
|---|---|---|
| `test_generic_type_param_constraint_must_be_spec` | 非 spec type 约束会失败 | 只检查数量下限和消息片段；新增精确 `AE0709` 证据。 |
| `test_tuple_type_constraint_is_rejected` 及 G20 精确矩阵 | tuple 约束会失败，G20 已锁定位置 | 按白名单迁移为 `AE0709`，G25 增加与普通 type 配对的同码证据。 |
| `test_generic_type_ref_arity_too_many`、`test_generic_arity_mismatch_with_overloads` | 类型引用 arity 会失败 | 部分只检查消息；补齐短名／限定名、过少／过多和完整诊断属性。 |
| `test_generic_explicit_type_args_arity_mismatch`、`test_generic_type_constructor_explicit_type_args_arity_mismatch` | callable／构造显式 arity 会失败 | 只检查消息；补齐 `AE1015` 及函数、方法、构造入口。 |
| `test_generic_non_generic_type_with_type_args_rejected` | 非泛型 type 拒绝类型实参 | 补齐 `AE1014`，并增加非泛型 callable 的同根因证据。 |
| `test_generic_method_type_param_collides_with_type_param` | 方法泛参遮蔽 owner 会失败 | 只检查消息；补齐 `AE1017`、位置、数量，并增加同列表重名。 |
| `test_generic_type_same_name_same_arity_rejected` | 泛型 type 同名同 arity 使用 `AE0213` | 已锁定错误码，但缺少唯一数量和位置；新增精确配对，并映射不同 arity 正例。 |
| `test_generic_duplicate_fn_by_type_param_name_only_rejected` | 泛型参数名不同不能区分函数 | 只检查失败；新增声明处 `AE0508` 精确证据，不以调用点二义性代替重复声明。 |
| `test_generic_call_without_inference_source_rejected` | 无推导来源使用 `AE0525` | 已锁定单码和消息，仍缺 token／行列；G25 补齐完整属性及部分推导。 |
| `test_child_spec_generic_parent_constraint_rejects_unrelated_instance`、`test_generic_overload_constraint_excludes_candidate` | callable 约束失败排除候选，最终使用 `AE0512` | 增加显式／推导及有合法 fallback 的配对，不改成 owner 码。 |
| G22、G23、G24 独立精确测试 | 名称解析、spec／fit 和复合约束专属边界 | 逐项引用，已有直接证据不重复新增。 |

上述函数名只是静态基线。只有满足总计划 §3.2 的错误码、文件、token、行列、数量和 Semantic 阶段
断言，才可直接计入 G25；仅断言失败、消息片段或 `error_count >= 1` 的旧测试不能独立关闭 TODO。

## 4 用例 TODO

### 4.1 GENERIC01：类型参数名称与声明 identity

- [ ] GENERIC01-A：分别对泛型 `type`、顶层 `func`、`spec` 和 type／fit 方法构造同一参数列表重名，
  在第二个参数名产生唯一 `AE1017`；替换为不同名称后通过。共享一个表驱动 Semantic 矩阵，不为声明
  种类分配不同错误码。
- [ ] GENERIC01-B：type 方法泛参和 owner 泛参同名时，在方法参数名产生唯一 `AE1017`；不同名时两层
  参数均可在签名和方法体解析。不得把 owner 参数从作用域移除来规避遮蔽检查。
- [ ] GENERIC01-C：构造函数和终结器声明方法级泛参时分别产生 `AE0316`；只使用 owner 泛参的泛型
  构造／终结器作为合法邻界。终结器的参数、返回和唯一性继续映射类型规范既有诊断。
- [ ] GENERIC01-D：object-form `spec` 方法级泛参直接映射 G23 的 `AE0331` 精确证据；泛型 spec owner
  合法，不得误判为方法级泛参。
- [ ] GENERIC01-E：同 category 的泛型 type 与 spec 分别验证同名同 arity 即使参数名或约束不同仍在
  后一声明产生唯一 `AE0213`；同名不同 arity 合法并能在使用点精确解析。
- [ ] GENERIC01-F：跨 category 同名继续映射 `AE0004`，不得因泛型 arity 不同绕过 name-only 冲突。
- [ ] GENERIC01-G：顶层函数、type 方法和 fit 方法分别验证“仅类型参数名／约束不同”不能形成新签名，
  对应复用 `AE0508`／`AE0801`；不同泛型 arity 合法。仅返回类型或变参冲突继续映射既有
  `AE0509`／`AE0510`／`AE0802`／`AE0803`。
- [ ] GENERIC01-H：重复参数、声明 identity 和重载冲突均在声明期急切诊断；不得以随后调用的
  `AE0511` 二义性代替声明错误。纯语法失败只映射 G12。

### 4.2 GENERIC02：类型实参数量与精确目标解析

- [ ] GENERIC02-A：泛型 type／spec 引用在字段、参数、返回、父 spec、fit target、嵌套类型实参和数组
  元素类型中的过少／过多 arity 均产生唯一 `AE1015`；选取必要代表覆盖短名与真实限定名，不机械排列。
- [ ] GENERIC02-B：显式泛型构造目标的过少／过多 arity 产生 `AE1015`，定位构造目标；不得继续到
  构造参数匹配后误报 `AE0313`／`AE0315`，也不得发历史 `AE0232`。
- [ ] GENERIC02-C：顶层／模块函数、实例／静态方法和 fit 方法的显式类型实参过少／过多时产生
  `AE1015`；用最小代表覆盖各 resolver 入口，不得发历史 `AE0233`。
- [ ] GENERIC02-D：同名不同 arity 的 type／spec 和 callable 候选按精确 arity 选择，不依赖声明顺序；
  无精确 arity 时 `AE1015`，callable 消息列出可用 arity，不任选某个候选作为期望值，也不得回退到
  其他同名声明后产生次生诊断。
- [ ] GENERIC02-E：约束引用自身是泛型 spec 时，其 arity 错误仍使用 `AE1015`；只有解析到正确 arity
  后才进入 `AE0709`／`AE0710` 检查。
- [ ] GENERIC02-F：每个反例断言唯一诊断、源码文件、目标 token、行列及 Semantic 阶段；合法邻界在
  FCTS 中真实选择预期闭合声明并断言返回值。

### 4.3 GENERIC03：约束声明、满足与候选筛选

- [ ] GENERIC03-A：不存在的约束类型名使用 `AE1013`，映射 G22 名称解析规则；不附加
  `AE0709`，避免一个根因产生两个诊断。
- [ ] GENERIC03-B：约束解析到普通 object type、tuple 及另一必要非 spec 代表时均使用
  `AE0709`；tuple 可保留更具体的消息，但不得继续发 `AE0304`。
- [ ] GENERIC03-C：泛型 type／spec owner 的具体实参不满足 object-form 或 callable-form 约束时使用
  `AE0710`，定位具体实参；满足约束的直接 fit、传递父 spec 和精确闭合实例作为合法邻界。
- [ ] GENERIC03-D：开放泛参转传覆盖相同约束、更强约束、无约束、较弱约束和无关约束；前两者合法，
  后三者在内层 owner／父 spec 使用点产生 `AE0710`，不得延迟到后续闭合调用。
- [ ] GENERIC03-E：泛型函数／方法候选的显式或推导实参不满足约束时先排除该候选；存在合法非泛型或
  其他泛型候选时正常选择，不存在候选时在调用目标产生唯一 `AE0512`，不得改发 `AE0710`。
- [ ] GENERIC03-F：object-form 约束下的字段／实例方法、静态 requirement，以及 callable-form 约束
  直接调用建立最小正向行为；访问约束未提供的成员继续使用普通成员／调用诊断，不新增泛型专码。
- [ ] GENERIC03-G：union／intersection 的准入、开放约束转传和共享体能力逐项映射 G24
  COMPOSITE23～28、34～35；只有通用机制缺少独立证据时才新增，不复制复合类型矩阵。
- [ ] GENERIC03-H：泛型实例之间保持不变；因类型实参不同产生的赋值或 cast 失败映射通用类型不匹配／
  `AE1023`，不得误报为“实参不满足泛型约束”。

### 4.4 GENERIC04：类型参数推导

- [ ] GENERIC04-A：从一个及多个普通实参推导全部类型参数，覆盖同一参数多次出现和嵌套泛型／数组
  类型结构；运行时断言闭合返回值和所选重载。
- [ ] GENERIC04-B：方法级泛参从实参和适用的 receiver 静态类型推导；owner 泛参仍来自闭合 receiver，
  不得与方法泛参串槽。
- [ ] GENERIC04-C：返回位置可由明确目标类型推导时成功；同一调用缺少目标时产生唯一 `AE0525`，
  证明目标只参与调用结果推导，不改变来源 callable value 规则。
- [ ] GENERIC04-D：多类型参数只推导出一部分时，在调用目标产生唯一 `AE0525`，消息中的索引指向首个
  未解析参数；显式补齐全部实参后通过。
- [ ] GENERIC04-E：同一类型参数从多个来源得到冲突结果时，该泛型候选被排除；无候选存活使用
  `AE0512`，有合法 fallback 时选择 fallback，不把冲突误报为“没有推导来源”的 `AE0525`。
- [ ] GENERIC04-F：两个或以上候选均完成推导且同优先级可用时复用 `AE0511`；精确非泛型候选优先于
  泛型候选的行为映射 G15／既有泛型重载证据。
- [ ] GENERIC04-G：callable-form 目标不得反向推导泛型函数／方法来源泛参；未显式闭合时赋值使用
  `AE0522`、cast 使用 `AE1023`，显式闭合且结构匹配后可形成 callable value 并真实调用。
- [ ] GENERIC04-H：类型构造参数和目标类型不推导 owner 泛参；裸目标仅按 `(name, arity=0)` 解析，
  没有 arity 0 声明时使用 `AE0315`。静态成员 owner 也必须显式闭合，无匹配调用使用 `AE0512`。

### 4.5 GENERIC05：显式泛型 target 合法性

- [ ] GENERIC05-A：对类型参数自身写 `<...>` 时产生唯一 `AE1012`；错误定位类型参数名，不继续按普通
  具名类型查找。
- [ ] GENERIC05-B：显式类型实参内部的未知类型使用 `AE1013`；外层 target 正确时不得附加 arity 或
  调用诊断。普通 callable 名称不存在继续映射 G22／G15 的名称或调用诊断。
- [ ] GENERIC05-C：非泛型 type、构造目标、顶层函数、实例／静态方法和 fit 方法携带类型实参时统一
  使用 `AE1014`；选择必要代表覆盖 resolver 分支，不为 callable 新增错误码。
- [ ] GENERIC05-D：已解析泛型 target 作为独立表达式、没有明确 callable 目标的绑定，或落入未定义的
  postfix 消费位置时产生 `AE1016`；不得延迟为 Codegen `CE`／`IE`。
- [ ] GENERIC05-E：类型位置裸名引用泛型 type／spec 使用 `AE0006`；它与“已写 `<...>` 但目标不合法”
  的 `AE1012`～`AE1016` 分开，映射 G21 的精确证据。
- [ ] GENERIC05-F：顶层／模块函数、实例／静态方法、构造、对象字面量、array-new 及明确 callable
  目标等规范允许的消费形式分别映射正向证据；语法缺口只映射 G12。

### 4.6 GENERIC06：最小合法邻界行为

- [ ] GENERIC06-A：generic type／spec／function／type method／fit method 的不同参数名和不同合法 arity
  均可声明、闭合并使用；行为断言能区分实际选择的 identity 或重载。
- [ ] GENERIC06-B：显式与推导调用分别覆盖顶层、实例和静态入口，断言返回值、receiver 和 owner／
  方法泛参替换，不以仅 Semantic 通过代替行为。
- [ ] GENERIC06-C：object-form、callable-form 约束及父 spec 约束转传各有一个合法闭合程序，真实调用
  requirement 并断言结果；union／intersection 正向行为直接映射 G24。
- [ ] GENERIC06-D：显式闭合的泛型 callable value 在赋值和 cast 两种明确目标下真实调用，证明
  `AE0522`／`AE1023` 反例只排除未闭合或结构不匹配来源。
- [ ] GENERIC06-E：同名不同 arity 的 type／spec／callable 连续使用，改变声明顺序后仍选择相同目标；
  只在既有 FCTS 没有直接结果断言时新增用例。
- [ ] GENERIC06-F：优先复用现有 FCTS；新增文件中的每个 `test(...)` 必须登记并真实执行，包含明确
  成功结果和失败分支，避免“未进入断言分支也通过”。

## 5 实施文件与顺序

### 5.1 文档先行（本次变更）

- 更新本实施文档、G25 问题记录和 AE 主规范；
- 将泛型主规范中已经存在于实现要求、但未进入规范规则清单的“同一类型参数列表名称唯一”要求收敛到
  规范性条款；
- 修正总计划的 G24／G25 状态、G24 范围和历史交付记录；
- 将滞后的错误码总表收敛为分段主规范索引，不再重复维护旧 AE／CE 表格。

### 5.2 Review 通过后的产品实现

预计只修改 `src/semantic/analyzer.c`：

1. 在所有泛型声明共用的类型参数列表入口检查列表内重名，并与 owner／方法遮蔽复用同一
   `AE1017` 诊断辅助逻辑；
2. 让 tuple 与其他非 spec 约束走同一 `AE0709` 分支；
3. 把显式 type／callable target 的验证统一为“未知名称、非泛型、泛型 arity 不匹配、约束不满足、
   未消费”顺序，分别落到 `AE1013`、`AE1014`、`AE1015`、`AE0710`／`AE0512`、`AE1016`；
4. 移除用户源码可达的 `AE0232`／`AE0233` 和 `AE0304` 发码出口，不保留按目标种类分叉的特判。

实现不得更改类型推导优先级、泛型 identity、约束满足关系或运行时表示。补测暴露与上述范围不同的
产品缺陷时，先登记问题并取得人工批准，不顺手修复。

### 5.3 Review 通过后的测试

- 新增 `test/semantic/test_g25.c`，使用统一辅助函数对每个非法程序核对完整诊断集合的错误码、文件、
  token、行列、数量和 Semantic 阶段；在 `test/semantic/test_semantic.c` 只登记入口；
- 仅按 §2.3 白名单修改三个既有错误码期望，不修改既有测试源码、位置、数量、消息和行为断言；
- 逐项盘点现有 FCTS，只有 GENERIC06 缺少可观察结果时才新增
  `fcts/fcts_bin/src/test_g25_generic_boundaries.ff` 并在既有主入口登记；
- 若需 Codegen／Symbol 测试，只能用于确认诊断在 Semantic 停止或跨包声明事实可恢复，不能以内部
  结构测试代替合法语言行为。

## 6 独立验收与交付 TODO

- [ ] 完成 GENERIC01～06 的“主规范条款—稳定码—既有／新增测试—断言”反向矩阵；已有证据不足时
  标记新增，不能以相邻测试或全量通过代替。
- [ ] 独立运行 G25 Semantic 专项，逐例核对完整诊断集合、文件、token、行列、数量和阶段；检查合法
  邻界无错误。
- [ ] 独立运行 G25 FCTS 行为；若全部复用，逐项记录现有测试名及可观察断言，不以成功编译代替运行。
- [ ] 搜索 `src/`，确认用户源码可达路径不再产生 `AE0232`、`AE0233` 或 `AE0304`；历史文档可保留
  旧码，但必须明确标为迁移前记录。
- [ ] 核对本组没有 runtime、公开 ABI、私有 ABI、`.ft` 格式或运行时成本变更；若实际需要，暂停并
  取得人工决定。
- [ ] 在 Codex 沙箱外为 G25 独立执行完整 `make test`，记录 UBSan 与常规两阶段各套件准确结果；
  其他组的回归不得代替。
- [ ] 执行 `git diff --check`，关闭或取得不阻塞交付的明确 G25 问题决定，并填写实际文件、专项、
  全量结果和建议 commit message；不自动提交。

## 7 Review 启动门槛

开始产品和测试实施前，需要人工一次性明确批准以下范围：

1. §5.2 所列 `analyzer.c` 编译期改动；
2. §2.3 中三个既有错误码断言的精确迁移白名单；
3. G25 不新增错误码，按 §2.2 复用并扩展现有码适用入口；
4. 实施前和完成后均在 Codex 沙箱外执行独立 `make test`。

未取得上述批准时，本文件保持“待 Review”，只能继续做只读核对或文档修正，不能修改产品代码和既有
测试。批准后若工作区不干净，须先确认重叠变更，再按“规范—产品—测试—专项—全量”的顺序实施。

## 8 独立交付记录

- 状态：待实施；
- 稳定码映射：已在 §2 收敛，计划不新增错误码；
- 实际产品文件：—；
- 实际新增／复用测试：—；
- 本组专项结果：—；
- 本组沙箱外 `make test`：—；
- 问题：[G25 问题记录](./feng-language-conformance-coverage-hardening-issues/g25.md)，当前
  ISSUE-G25-001～003 均待实施批准；
- 建议实施 commit message：`test: complete G25 generic diagnostic coverage`。
