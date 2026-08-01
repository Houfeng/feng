# Feng 泛型实现开发文档

> **状态**: 实现进行中。
> 语言规范权威来源：[docs/specifications/feng-generics-draft.md](../specifications/feng-generics-draft.md)
> 符号表规范权威来源：[docs/specifications/feng-symbol-table.md](../specifications/feng-symbol-table.md)
> 前置任务（Phase 5.5 符号表重构）见 [docs/engineering/feng-plan.md](./feng-plan.md)。
> **当前阶段优先级调整（2026-05-08）**：暂停 [docs/engineering/feng-fit-optimize-delivered.md](./feng-fit-optimize-delivered.md) 中的“fit 值类型”推进，先把泛型本身补齐到可支撑标准库 `Map`、约束调用、值类型与各类契约场景的完整状态；fit 值类型工作在泛型完整后继续。

---

## Todo List

> 当前复查结论：G0-G3/G5 已基本落地；G4/G6/G7 的第一阶段骨架已经形成，且 object-form 约束 witness lowering、generic instantiation witness materialization、constrained spec generic arg slot witness、generic aggregate return、泛型类型上的泛型方法、generic type shared body 内部 self-call、简化 Map/Hashable 场景、类型级 `K: Eq<K>`、函数级 `U: Eq<U>`、open generic 返回值 witness adapter、generic spec parent codegen、约束面向上转发、open generic spec parent 替换，以及泛型 fit 均已闭环。由于 union-form `spec` 仍处于语法草案阶段，本轮继续限定为**不含 union-form**的既有语言形态；`std Map` 应在本轮回归确认后再进入实现。

- [x] **Phase 5.5**：符号表结构重构（G5 前置，见 [feng-plan.md](./feng-plan.md) §Phase 5.5）
- [x] **G0**：所有设计决策收口（Q1–Q5 全部已决策，见 [§G0](#g0-规则收口前置于一切编码)）
- [x] **G1**：词法分析确认与测试（见 [§G1](#g1-词法分析)）
- [x] **G2**：AST 结构扩展（见 [§G2](#g2-ast-扩展)）
- [x] **G3**：Parser 泛型语法解析（见 [§G3](#g3-语法分析parser扩展)）
- [x] **G4**：语义分析扩展收口（声明/推导基础已落地；generic 实例化点的 witness materialization、generic callable direct call、generic spec concrete instance、direct generic spec instance coercion、callable-form method coercion、callable-form lambda coercion，以及泛型类型上的泛型方法基础语义已完成；union 约束面按 union 语法落地后另行收口，见 [§当前问题总表](#当前问题总表)）
- [x] **G5**：符号表导出 `.ft` 泛型支持（需 Phase 5.5 完成，见 [§G5](#g5-符号表导出ft-泛型支持)）
- [x] **G6**：代码生成收口（共享主体 ABI 已落地，object-form 约束 lowering、constrained spec generic arg slot witness、generic aggregate return、generic callable constraint invoke lowering、generic spec concrete instance、direct generic spec coercion、callable-form method coercion、callable-form lambda coercion、generic-type generic-method 基础路径、约束面向上转发、open generic spec parent 替换，以及泛型 fit 均已完成；union-form 和未来 tuple/value-struct 聚合值不纳入本轮既有形态验收，见 [§G6](#g6-代码生成)）
- [x] **G7**：端到端测试与全量回归收口（现有 smoke 已覆盖基础路径、object-form 约束调用、generic aggregate return、泛型类型上的泛型方法、generic type shared body 内部 self-call、简化 Map/Hashable 场景、类型级 `K: Eq<K>`、函数级 `U: Eq<U>`、open generic 返回值 witness adapter、generic spec parent codegen、约束面向上转发、open generic spec parent 替换，以及泛型 fit；union-form 随 union 阶段另行验收，见 [§G7](#g7-测试与验证)）

---

## 当前问题总表

本节只记录**已经通过当前代码与 smoke/最小样例验证过**的问题，用于指导下一阶段工作。这里的“问题”是“泛型尚未完整”的事实，不代表 Q1 路线错误；相反，当前问题大多是 Q1 路线尚未收口完毕。

### 本轮修复记录（2026-05-08）

本轮按以下顺序收口当前语言形态内的缺口，并分别补充 smoke：

1. 函数级自引用约束：`fn sameAs<U: Eq<U>>(...)` 与类型级 `type MiniMap<K: Eq<K>, V>` 使用同一套 open generic spec instance 注册与 descriptor 解析机制。
2. open generic 返回值 witness adapter：`spec Cloneable<T> { func cloneValue(): T; }` 这类约束方法返回 open generic parameter 时，witness thunk 通过 `_out`/slot ABI 把 concrete 返回值写回 erased slot。
3. generic spec parent codegen：`spec IntSequence: Sequence<int>` 这类语义层已接受的 generic spec parent，会在 codegen 注册与 witness 结构中按子优先、同名跳过的规则展开父 spec 成员。

### 2026-05-09 复查新增缺口与本轮修复

本次复查不只看文档，按现有 smoke、代码生成护栏与最小 probe 反查，确认以下缺口属于当前既有语言形态内的问题；本轮已按顺序修复并补充 smoke：

1. 约束面向上转发：`func useLabelled<U: Labelled>(value: U) { useNamed:<U>(value); }` 在 `Labelled: Named` 已成立时，可通过 parent-prefix 兼容的 descriptor adapter 转发到 `Named` 约束面。
2. open generic spec parent 替换：`spec Collection<T>: Sequence<T>` 与 `type IntBag: Collection<int>` 组合下，语义层与 codegen 均以 `Sequence<int>` 检查和注册 parent spec instance。
3. 泛型 fit：`fit Box<T>: Reader<T>` 中左侧 `<T>` 进入目标泛型 type 参数作用域，满足性检查、符号导出与 codegen 均按 concrete target instance 替换 `T`。

### 2026-05-09 跨包泛型消费缺口

本次复查进一步确认，`std Map` 的真实准入路径是“作为 `.fb` 包公开泛型 API，再由 consumer 通过 `import` 消费”。该路径必须满足 [feng-package.md](../specifications/feng-package.md) 对公开 `.ft` 的要求：公开泛型 `type`、`spec`、顶层 `func` 与成员方法必须导出类型参数、约束、未实例化签名骨架、泛型父 `spec` / `fit` 使用事实，consumer 不能依赖 provider 源码或某个已单态化实例。

本轮按以下四项收口：

1. `type_param` 的约束必须随 `.ft/.fb` 导出、读取，并在 imported AST 合成时恢复为 `T: Constraint`。
2. consumer 调用 imported 受约束泛型顶层函数时，descriptor 必须携带本地实际类型对 imported spec 的 witness，不能退化为 `witness = NULL`。
3. consumer 以本地类型实参实例化 imported 受约束泛型类型并调用方法时，wrapper/codegen 必须在使用点解析约束并传递正确 descriptor。
4. CLI package 回归必须覆盖 imported 受约束泛型函数调用和 imported 受约束泛型类型实例化两条路径。

已用最小 probe 复现的缺口如下，本轮修复必须同时补回归：

1. 外部包公开泛型顶层函数后，consumer 调用时不能在 codegen 中丢失函数级类型参数引用。
2. 外部包公开泛型类型后，consumer 构造具体实例并调用成员方法时，必须正确生成实例类型、默认零值构造符声明/实现引用以及对应成员方法符号。
3. 外部包公开泛型 `spec` 后，consumer 必须能通过 `import` 引入该 spec，实现 `type Key: Eq<Key>`，并把 imported generic spec 作为本地泛型约束使用。

### 2026-05-11 全量修复拆分（先全局 codegen，再泛型特有路径）

以下拆分用于本轮“完全修复”实施，目标是先补齐全局 codegen 值模型与 coercion 能力，再把泛型入口接入同一套稳定抽象，避免在泛型层堆叠一次性特判。

#### Phase A. 文档与验收口径收口

- [x] A1. 在本文件锁定四项缺口的最终完成定义：aggregate spec field、if/match aggregate result、callable OTHER coercion、aggregate generic arg。
- [x] A2. 如 aggregate spec field / match aggregate result 的值语义与现有权威文档不一致，先更新主规范，再继续编码。
- [x] A3. 明确本轮只覆盖当前既有语言形态；future tuple / value-struct / union carrier 继续留在后续阶段。

#### Phase B. 全局 codegen：aggregate spec field

- [x] B1. 放开 object-form `spec` 成员注册时对 aggregate field 的拒绝，改为进入统一 witness/member 生成路径。
- [x] B2. 扩展 witness struct 中 aggregate field getter/setter 的 ABI，使其可以表达 borrowed read、var store 与必要的 retain/take/assign 语义。
- [x] B3. 扩展 default witness / concrete witness / slot witness adapter，使 aggregate field 在默认值、真实实现、spec-to-spec 转发三条路径上一致闭环。
- [x] B4. 扩展 default subject release / managed field descriptors，确保 aggregate field 的 managed slot 会被 cycle collector 与 cleanup 正确看到。
- [x] B5. 新增 focused codegen regression，覆盖 object-form `spec` 的 aggregate field getter/setter 与 slot witness forwarding。

本轮 Phase B 已完成的实现要点：

1. object-form `spec` 的 aggregate field 已不再被 codegen 护栏拒绝；default witness、concrete witness、slot witness forwarding 均已支持 aggregate getter/setter。
2. object literal / default subject factory 对 aggregate field 不能再使用直接结构体赋值或普通 default value expression，必须统一走 `feng_aggregate_assign` / `feng_aggregate_default_init`，否则会在运行期出现引用计数失衡。
3. 已补 focused codegen regression 与 smoke：既覆盖 aggregate field 自身，也覆盖 aggregate field 经 generic constrained spec value / slot witness adapter 转发的真实执行路径。

#### Phase C. 全局 codegen：if/match aggregate result

- [x] C1. 扩展 if-expression result slot，使其支持 aggregate 结果的 default-init、分支写入、cleanup 与 owns/borrow 语义。
- [x] C2. 扩展 match-expression result slot，复用与 if-expression 相同的 aggregate 写入/释放协议。
- [x] C3. 新增 focused codegen regression 与 smoke，覆盖 object-form `spec` aggregate 结果在 if/match 中的真实执行路径。

本轮 Phase C 已完成的实现要点：

1. if-expression / match-expression 的结果槽位在 aggregate 场景不再报 not yet supported，而是统一走 `feng_aggregate_default_init` + 分支 `feng_aggregate_assign`/`feng_aggregate_take` + scope cleanup。
2. 分支表达式对 aggregate 结果的 owns/borrow 语义与既有 value-model 保持一致：borrow 走 assign，owns 走 materialize + take。
3. 已补 focused codegen regression 与 smoke，覆盖 if/match 两条 aggregate result 路径，并通过全量回归验证。

#### Phase D. 全局 codegen：callable OTHER coercion

- [x] D1. 抽取 callable-form `spec` 的统一 closure/adaptor builder，保留 top-level func / method / lambda 快路径。
- [x] D2. 为 `FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER` 增加“已是 callable value 但 target surface 不同”时的重包装 lowering。
- [x] D3. 复核 coercion site 元数据后确认当前字段已足够区分 source 形态，本轮无需新增 semantic sidecar。
- [x] D4. 新增 focused codegen regression 与 smoke，覆盖 local binding / parameter 来源的 OTHER coercion。

本轮 Phase D 已完成的实现要点：

1. `FENG_SPEC_COERCION_CALLABLE_SOURCE_OTHER` 分支不再直接报 not yet supported；当 source 已是 callable value 且与 target callable spec 仅 surface 不同，会生成统一的 rewrap closure。
2. rewrap closure 复用 callable-form 统一对象布局（`_hdr`/`_self`/`invoke`），通过 adaptor `invoke` 桥接到 source callable 的 `invoke`，并通过 `feng_assign` 管理 `_self` 生命周期。
3. top-level func / method / lambda 既有快路径保持不变；OTHER 仅补齐缺口路径，不回退已有行为。
4. 已新增 focused codegen regression 与 smoke，覆盖 parameter -> local binding -> target callable spec 的连续 coercion 路径。

#### Phase E. 泛型特有路径：aggregate generic arg

- [x] E1. 基于已稳定的 aggregate descriptor / witness 规则，扩展 generic descriptor 生成，让 aggregate type arg 产出完整 `FengGenericParamDescriptor`。
- [x] E2. 保证 aggregate generic arg 在 generic function、generic method、generic type instantiation 三个入口上走同一条 descriptor 路径。
- [x] E3. focused regression 先锁定当前语言里已存在的 aggregate surface（object-form `spec` fat value），不要把 future tuple/value-struct 一并卷入。

本轮 Phase E 实施切片（当前进行中）：

1. 仅覆盖当前既有 aggregate surface：object-form `spec` fat value；future tuple/value-struct/union carrier 不纳入本轮。
2. 以 `cg_generic_descriptor_expr` 为唯一 descriptor 生成入口，验证 generic function / generic method / generic type instantiation 三个路径对 aggregate type arg 的一致性。
3. 先补 focused regression，再补 smoke 端到端，最后执行全量回归。

本轮 Phase E 已完成的实现要点：

1. aggregate type arg（当前既有形态：object-form `spec` fat value）在 descriptor 生成上统一通过 `cg_generic_descriptor_expr`，产出的 `FengGenericParamDescriptor` 同时携带 aggregate descriptor 与约束 witness。
2. 三个入口（generic function / generic method / generic type instantiation）均走同一 descriptor 生成路径；没有新增并行特判路径。
3. 已新增 focused codegen regression 与 smoke：锁定 object-form `spec` fat value 的 aggregate generic arg 三入口组合路径，并通过全量回归验证。

#### Phase F. 泛型组合回归与全量验证

- [x] F1. 新增 generic callable/coercion 组合回归，覆盖 callable 参数、局部绑定、字段读取后的 target spec coercion。
- [x] F2. 新增 aggregate generic arg 端到端 smoke，覆盖实参、字段、返回值与约束 witness 传递。
- [x] F3. 阶段性 focused validation 全绿后，执行 `make smoke`、`make test`，必要时补 CLI/package 回归。

本轮 Phase F 已完成的实现要点：

1. 新增 callable OTHER coercion 组合回归，覆盖 parameter -> local binding -> field read -> target callable spec coercion 的完整路径。
2. 修复 callable coercion source 分类边界：member 表达式仅在语义解析出 method 绑定时才归类为 METHOD_VALUE；callable-typed field read 归类为 OTHER，避免误走 method coercion。
3. aggregate generic arg 端到端 smoke 已覆盖实参、字段、返回值与约束 witness 传递路径，并与 focused 回归共同通过 `make smoke`、`make test` 全量验证。

### 一、当前已经验证可工作的范围

- 泛型声明、类型引用、显式类型实参、基本推导、名称与 arity 校验，已经具备可用基础。
- 泛型类型上的**非泛型方法**，当参数/返回仅使用外层类型参数，且具体类型实参属于当前已支持类别时，可以正确编译运行。
- 泛型类型上的**泛型方法**基础路径已经可用：共享方法体按外层类型参数描述符后接方法类型参数描述符展开；实例 wrapper 补外层描述符，调用点补方法级描述符；当前 smoke 已覆盖 `Box<T>.echo<U>` 的推导调用与 `Box<T>.replace<U>` 的显式类型实参调用。
- 当前 smoke 已验证 `Box<T>` 的 `setValue(next: T)` / `readValue(): T` 路径可工作，覆盖了 `i32` 与 `string` 两类实例。
- 当前 codegen 与 smoke 已验证 object-form `spec` 约束下的字段读写与方法调用可以通过 `_T->witness` 闭环工作。
- 当前 codegen 与 smoke 已验证 generic function / shared body 中 object-form `spec` 这类 aggregate return 已闭环，且同时覆盖 borrowed 与 owns_ref 两条返回路径。
- 共享泛型主体的基本 ABI 已经落地：`void *self` / `const void *param` / `T` 的隐藏描述参数 / `void *_out` 这条路线是当前继续完善的基础，不再推翻；完整态将该隐藏参数统一收口为 `FengGenericParamDescriptor *T`。

### 二、当前已经验证未完成的问题

#### P1. `spec` / 聚合值 generic arg 的当前口径

- analyzer 已经在 generic function call、generic method call、generic type ref 与 generic constructor 的实例化点统一 demand object-form witness，semantic sidecar 不再只依赖 coercion site 才产出 `(ConcreteType, ConstraintSpec)` witness。
- constrained object-form `spec` value 作为 generic type argument 的 slot witness adapter 已经补齐，`spec` 值现在可以在受约束 generic path 中通过常量 adapter witness 转发到其动态 witness。
- 这里的“aggregate generic arg”指 `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` 这类**按值聚合**作为泛型实参进入 `T`，例如当前 object-form `spec` fat value，以及未来 tuple / value-struct / union carrier 这类按值聚合。
- 当前已实现语言形态中的 object-form `spec` 聚合值已经可作为无约束与受约束 generic arg；`aggregate type as generic type argument not yet supported (missing flatten rule) (G6)` 只保护尚未定义 flatten 规则的未来聚合类型。
- 因此，在“不含 union，且 tuple/value-struct 尚未成为可用语言形态”的本轮验收口径下，aggregate generic arg 不再作为当前阻塞项；未来新增聚合值类型时，必须先为其补齐 `FengAggregateValueDescriptor` / flatten 规则，再进入同一 generic descriptor ABI。

- 当前**无约束** object-form `spec` value 作为 generic arg 的基础路径已经通过 codegen 回归与 smoke：`let x: MySpec` 这类 fat spec value 可以作为 `t: T` 的实参传入泛型函数/方法，也可以作为 `Holder<MySpec>` / `Box<MySpec>` 这类泛型类型实参进入字段、参数与返回路径。
- 当前**受约束** object-form `spec` value 作为 generic type argument 已通过 slot witness adapter 闭环。
- 一般 aggregate type 作为 generic type argument 的失败分支仍应保留，直到对应语言形态具备稳定布局与 flatten 规则；这不是当前既有泛型能力的未完成项，而是未来值聚合类型接入泛型 ABI 的护栏。
- 影响范围包括：未来 tuple / value-struct / union carrier，以及任何新增的 `FengAggregateValueDescriptor` 按值泛型场景。

#### P2. union-form `spec` 的泛型覆盖暂缓

- 当前源码中的 `spec` form 枚举仍只有 object-form / callable-form 两类；源码尚未把 union-form 纳入同一条泛型实现路径。
- union-form `spec` 目前仍处于语法草案阶段，本轮泛型完整度验收不包含 union-form。
- callable-form `spec` 现在已经有统一 callable value 表示；top-level function coercion、direct invoke、generic callable constraint invoke lowering，以及 generic callable `spec` concrete instance 的 direct call 均已进入同一条 lowering。
- codegen 现在已经能注册并解析 generic `spec` concrete instance；object-form / callable-form 两类 `Spec<int>` 具名使用都能进入 shell/member/codegen 路径。
- analyzer / codegen 现在已经按 concrete target type ref 打通 direct generic `spec` coercion：object-form `type -> Spec<int>` 与 top-level function -> `CallableSpec<int>` 都已进入同一条 semantic sidecar / codegen 路径，并补齐了对应回归。
- callable-form `spec` 的 method value coercion 已经进入统一 callable value lowering，并通过绑定 receiver 的 closure 形态闭环。
- callable-form `spec` 的 lambda coercion 已经复用同一 callable closure ABI 闭环：lambda closure 对象显式保存 capture/self 槽位，静态 invoke thunk 执行 lambda body，并通过 capture cell 保持文档规定的引用捕获语义；没有为 lambda 引入第二套 callable 运行时形态。
- 因而，generic `spec` 与 callable-form `spec` 的真实收口前提是：codegen 能注册并解析 generic spec instance；callable-form `spec` 能复用同一套 callable value 表示与 direct invoke lowering；这套表示未来还能向 union-form `spec` 扩展，而不是再次推翻 ABI。
- 因此，当前实现距离“包含 union-form 的所有形态 Spec 都可作为泛型实参/约束”仍有明确差距；该差距转入 union-form 语法和语义落地后的后续阶段，不阻塞本轮“不含 union”的泛型完整验收。

#### P3. 泛型类型上的泛型方法基础路径已完成

- 当前 `type Box<T> { func map<U>(...) { ... } }` 这类“泛型类型 + 方法级泛型参数”的基础组合已经打通。
- 语义层已经能在成员方法调用返回类型上同时替换外层类型参数与方法级类型参数，支持方法级类型实参显式指定与直接参数位置推导。
- 代码生成层已经移除 `generic methods on generic types are not yet supported` 失败分支；G6-6 的 ABI 落地为：泛型类型共享方法体按“外层类型参数描述符 -> 方法类型参数描述符”接收统一 descriptor 序列；具体类型实例 wrapper 负责补齐外层 descriptor，调用点负责为方法级类型实参构造 descriptor，并继续通过 `_out` 处理方法级泛型返回值。
- 当前基础证据来自 `test/codegen/test_codegen.c` 的生成 C 编译用例，以及 `test/smoke/phase1a/generic_type_generic_method.ff` 端到端 smoke。

#### P4. `Map<K: Hashable, V>` 级场景与类型级自引用约束已形成基础证据

- 当前 object-form 约束调用不再只依赖最小 smoke；`test/smoke/phase1a/generic_map_hashable.ff` 已把 `MiniMap<K: Hashable, V>` 作为 Map/Hashable 级准入用例锁定。
- 该 smoke 覆盖受约束类型参数方法调用、泛型类型字段读写、generic type shared body 内部同类型方法调用，以及 `K`/`V` 多类型参数组合，作为“不含 union”的真实目标场景基础证据。
- `test/smoke/phase1a/generic_self_constraint.ff` 已补齐真实 `Map` 所需的 `K: Eq<K>` 自引用泛型 spec constraint：open generic spec instance 会带上所属泛型上下文，constraint witness descriptor 会为 concrete `Key as Eq<K>` 生成 adapter，并在 `key.same(self.key)` / `self.hasKey(key)` 路径中把 erased generic slot 解包为 concrete `Key` 参数。
- `test/smoke/phase1a/generic_function_self_constraint.ff` 已覆盖函数级 `fn sameAs<U: Eq<U>>(...)`：调用点在解析 callee 类型参数约束时启用 callee 自身的泛型上下文，descriptor 构造可复用 open generic spec instance。
- `test/smoke/phase1a/generic_self_constraint_return.ff` 已覆盖 spec 方法返回 open generic parameter：witness slot 使用 `_out`/erased slot ABI，concrete method return 由 adapter 写回调用方按描述符分配的 slot。

#### P4b. generic spec parent codegen 已收口

- semantic 已有 `spec IntSequence: Sequence<int>` 的成功用例，说明 generic spec parent forwarding 在语义层被视为合法形态。
- codegen 已移除 `spec parent_specs not yet supported in Step 4b-α` 护栏，并在 `UserSpec` 成员注册时按语义侧闭包规则展开父 spec 成员：子 spec 自身成员优先，同名父成员跳过。
- `test/smoke/phase1a/generic_spec_parent_codegen.ff` 已覆盖 `spec IntSequence: Sequence<int>`，并验证子 spec 未重复声明父成员时，`IntSequence` 视角仍可通过 witness 调用继承自 `Sequence<int>` 的 `size()`。
- `test/smoke/phase1a/generic_spec_open_parent.ff` 已覆盖 `spec Collection<T>: Sequence<T>`，验证 parent spec 在 semantic 与 codegen 中都会按当前实例替换为 `Sequence<int>`。

#### P5. 测试覆盖已形成基础证据，并补齐真实 Map 约束准入

- 现有 smoke / 单测已证明：标量值类型、托管指针类型、object-form `spec` 聚合值 generic arg（包含 `let x: MySpec` 作为 `t: T` 实参与 `MySpec` 作为泛型类型实参）、object-form 约束调用、generic aggregate return、generic callable constraint invoke lowering、callable-form `spec` 的 lambda coercion、generic spec concrete instance 的 object/callable 基础 codegen 路径、泛型类型上的泛型方法、generic type shared body 内部 self-call、`Map<K: Hashable, V>` 级基础场景、类型级 `K: Eq<K>`、函数级 `U: Eq<U>`、open generic 返回值 witness adapter、generic spec parent codegen、约束面向上转发、open generic spec parent 替换，以及泛型 fit 场景。
- 当前 smoke 已覆盖 `generic_self_constraint`；在完成 `std Map` 规范、实现与回归之前，不把标准库 Map 本身计入已交付。
- 源码中保留的 `aggregate type as generic type argument not yet supported (missing flatten rule)` 是未来聚合值类型接入前的护栏；多约束 / 约束合取语义仍等待后续规范收口，不作为当前既有语言形态的阻塞项。

### 三、当前类型覆盖矩阵

| 类型类别 | 当前状态 | 说明 |
| --- | --- | --- |
| 内建标量值类型（`bool` / 整数 / 浮点） | **已支持基础路径** | 走 `FENG_VALUE_TRIVIAL` |
| 托管指针类型（`UserType` / `string` / `T[]`） | **已支持基础路径** | 走 `FENG_VALUE_MANAGED_POINTER` |
| object-form spec value | **已支持基础路径** | 无约束 generic arg、约束 witness 调用、aggregate return 已闭环 |
| 受约束 object-form spec value generic arg | **已支持基础路径** | slot witness adapter 已闭环 |
| callable-form spec value | **已支持基础路径** | callable value 表示、top-level func coercion、method coercion、lambda coercion、generic callable spec instance coercion、direct call、generic callable constraint 已闭环 |
| union-form spec value | **暂缓** | union-form 仍处于语法草案阶段，不纳入本轮验收 |
| 未来 tuple / value-struct / 其他按值聚合类型 | **暂缓** | 尚未成为当前可用语言形态；未来接入时必须提供 aggregate descriptor / flatten rule |
| 受约束类型参数上的 object-form 行为调用 | **已支持基础路径** | `_T->witness` lowering 已闭环 |
| generic spec concrete instance (`Spec<int>`) | **已支持基础路径** | object-form / callable-form concrete instance 已能进入 shell/member/codegen 路径、直调/成员调用与 direct coercion |
| analyzer 在 generic 实例化点统一 witness materialization | **已支持基础路径** | generic call / type ref / constructor 已统一 demand witness |

### 四、当前契约覆盖矩阵

| 场景 | 当前状态 | 说明 |
| --- | --- | --- |
| 无约束类型参数声明/传参/返回 | **已支持基础路径** | 限制操作面仍成立 |
| 无约束类型参数禁止成员访问 | **语义规则已存在** | 需继续以测试锁死 |
| 泛型约束声明与解析 | **已支持基础路径** | 允许 `T: Spec`、允许泛型 spec 实例约束 |
| object-form `spec` 约束下的成员/方法行为调用 | **已支持基础路径** | 当前已通过 `_T->witness` + smoke/codegen 回归 |
| analyzer 在 generic 实例化点 materialize object-form witness | **已支持基础路径** | semantic 侧已统一 demand witness |
| callable-form `spec` 约束下的直接调用 | **已支持基础路径** | `_T->witness->invoke(...)` lowering 已闭环，并与 callable value direct call 共享表示 |
| union-form `spec` 约束下的收窄/成员可见性 | **未完成** | 需要复用 union 既有规则，不可另开特判 |
| 一等 spec 值与泛型约束共享一套 lowering | **不应继续混用** | 需要在实现上显式区分 |
| callable value 的 codegen 表示 | **已支持基础路径** | callable-form `spec`、generic callable constraint 与 generic callable spec instance 已共享同一套表示 |

---

## 目标收口（2026-05-08 版）

本节是对“当前实现骨架”与“最终完整态目标”的重新收口。Q1 节保留的是当前已落地的基础路线；本节给出**完整态**要求，后续 G4/G6/G7 以本节为准补齐。

> 当前交付口径说明：本节仍描述长期完整态，因此保留 union-form `spec` 的最终接入要求；但 union-form 语法尚处草案阶段，本轮“泛型补齐到 100%”临时解释为“不含 union-form 的既有语言形态完整”。

### 一、六项目标

1. 支持 `UserType`、所有内建类型（包括值类型、引用类型、和值语义的引用类型），以及所有形态 `spec` 作为泛型实参。
2. 支持所有形态 `spec` 作为泛型约束；`spec` 的主要设计目标就是契约，而泛型约束本质上就是一个重要的契约场景。
3. 支持“泛型类型 + 泛型方法”、泛型函数、泛型 `spec` 声明契约。
4. 支持任意多个泛型参数；实现不能把 ABI 写死为单参数或双参数特例。
5. 运行时开销极小：
    - 非契约路径不增加运行时查表；
    - 契约路径至多一次间接调用，或在可专门化场景下零间接调用；
    - 禁止任何基于 `(type, spec)` 的运行时查表。
6. 值类型不能产生托管堆分配；泛型路径不得以装箱作为通路前提。

### 二、完整态总原则

1. 保留 Q1 的“布局单态化 + 方法共享”主路线，不推翻当前共享主体 ABI。
2. 泛型架构的抽象单位不是 G6-1/G6-2 这类功能点，而是稳定抽象输入。完整态至少要把四类输入定死：泛型参数描述、宿主布局输入、约束面 witness 生成规则、统一泛型环境展开规则。
3. 泛型完整态必须把“如何操作 `T` 的值”和“如何使用 `T` 满足约束后的能力”统一收进同一个参数 ABI：`FengGenericParamDescriptor` 负责值模型字段，`witness` 指向该约束面的静态见证实例。
4. `FengGenericParamDescriptor` 不是值载体，也不是一等 `spec` 载体；具体值始终位于单态化结构字段、局部临时或普通实参槽中。
5. 共享主体不得把具体布局写死进实现；字段访问只能依赖宿主布局输入，而不能依赖“当前这个类型恰好只有一个字段”之类的偶然事实。
6. 契约分发必须通过 `FengGenericParamDescriptor.witness` 进入共享主体；禁止为同一类型参数再平行追加运行时描述参数，也禁止按 `type/spec` 做字典、哈希表、注册表或字符串查找。
7. 对值类型，泛型路径一律使用“字段内联 + 局部临时 + `_out` 返回 + 按值 thunk 参数”方案；不得为满足泛型或契约调用而分配托管对象。
8. 所有形态 `spec` 都必须能进入泛型约束体系；不能把 callable-form、union-form 等 form 视为“非主路径”，因为 `spec` 的核心职责就是契约，而泛型约束正是契约的关键使用面。
9. 泛型核心必须遵循开闭原则：共享主体只依赖稳定抽象输入，而不依赖“当前已知有哪些具体类型/契约 form”。未来新增类型或新增契约 form 时，应通过编译器补齐对应 `FengGenericParamDescriptor` 实例与对应 witness 实例接入；泛型共享主体本身不应因此改动控制流或分发逻辑。

### 三、完整态 ABI 设计

#### 1. 泛型参数 ABI

- 每个类型参数 `T` 都有且仅有一个 `const FengGenericParamDescriptor *_T`。
- 共享主体按**泛型环境**完整展开隐藏描述符参数：每个类型参数一份描述符；若同一主体同时涉及 `T`、`U`、`V`，则签名中必须显式出现 `_T`、`_U`、`_V` 三个隐藏参数，且顺序与声明顺序一致。
- 该描述符直接回答四个问题：
  - `sizeof(T)` 是多少；
  - `T` 属于 trivial / managed pointer / aggregate 哪一类；
  - 若是 aggregate，使用哪一个 `FengAggregateValueDescriptor`。
    - 若 `T` 声明了 `spec` 约束，复用哪一个编译期已经生成好的 witness 实例；否则 `witness == NULL`。
- 对 `T` 的**值语义操作**（复制、assign、retain/release、按值传参、按值返回、字段内联大小判断）只读取 `size` / `kind` / `aggregate`；只有 `T` 上的**契约能力操作**（object-form 成员访问、object-form 方法调用、callable-form 调用、union-form 收窄/投影）才读取 `witness`。
- 这条 ABI 已有实现基础，后续工作是把类型覆盖补齐，而不是重写。
- 当前实现里的 `FengGenericValueDescriptor` 是这条 ABI 的值模型前身；完整态直接将其重命名为 `FengGenericParamDescriptor` 并增加 `witness`，而不是再引入一层平行 wrapper。
- `FengGenericParamDescriptor` 的设计目标必须是“泛型参数使用契约”，而不是“把值本体或一等 `spec` 载体塞进 descriptor”。未来新增类型时，只要编译器能为该类型生成正确的 `FengGenericParamDescriptor`（必要时连同已有 aggregate 描述符体系与 witness 实例一起接入），泛型共享主体就不应改动。
- 共享主体只通过 `_T` 操作值、通过 `_T->witness` 使用约束；具体值本体始终在单态化字段、局部临时或普通实参里。

#### 2. 宿主布局输入

- 泛型共享主体不得把具体字段偏移硬编码进共享实现。
- 共享主体只按字段序消费由编译器提供的布局输入；最小形态可以是 `const size_t *field_offsets`，未来若收口为具名 layout descriptor，也不应改变共享主体的控制流。
- 未来新增类型、字段布局策略或对象承载形态时，只要编译器能提供正确的布局输入，泛型共享主体就不应改动。

#### 3. witness ABI（约束 ABI）

- `FengGenericParamDescriptor.witness` 不是运行时 `spec` 载体；它只是共享主体里复用编译期已经生成好的静态 witness instance。
- 这里“复用 witness”指复用同一套编译期 witness 生成机制与同一约束面的静态 slot 形状；不要求所有调用路径无条件共用同一个最终 witness instance。
- 对每个类型参数，按声明顺序只展开一个隐藏 `const FengGenericParamDescriptor *_T`；共享主体需要约束能力时，从 `_T->witness` 取对应 witness。
- `witness` 的静态解释在生成共享主体时就固定，不允许运行时枚举或查表。
- `FengNamedWitness` / `FengHandlerWitness` / `FengIntOrStringWitness` 只是三种示例，不是“全语言只固定这三种 witness 结构”。
- **固定的是生成规则**：object-form 生成字段/方法槽位；callable-form 生成 `invoke` 槽位；future union-form 生成 `test__Case` / `project__Case` 槽位。
- **变化的是 witness type**：按“约束面”生成不同的 witness 结构。
- **再变化的是 witness instance**：按“具体类型如何满足该约束面”生成不同实例。
- 因此，泛型核心不是依赖“只有三种 witness 类型”，而是依赖“任意约束面都能按固定规则生成 witness type 与 witness instance”。
- 在泛型共享主体里，传给 witness thunk 的 `subject` / `callee` / `value` 指针直接指向单态化结构字段、局部临时或普通实参槽；witness 只描述“怎么用这个值满足约束”，不承载值本身。
- 在一等 `spec` 值路径里，当前 object-form `spec` 仍沿用 fat value `{ void *subject; const Witness *witness; }`，调用形态是 `recv.witness->slot(recv.subject, ...)`。

**最小可实现定义**：

```c
/* 示例：object-form 约束面的 witness type。
 * 实际代码必须按具体约束面生成命名；这里的 Named 只用于说明形状。
 *
 * object-form spec Named {
 *   let name: string;
 *   func rename(next: string): void;
 * }
 */
typedef struct FengNamedWitness {
    void (*get__name)(const void *subject, void *_out);
    void (*set__name)(void *subject, const void *value);   /* 只对可写字段生成 */
    void (*call__rename)(void *subject, const void *arg_next, void *_out);
} FengNamedWitness;

/* 示例：callable-form 约束面的 witness type。
 * 实际代码必须按具体约束面生成命名；这里的 Handler 只用于说明形状。
 *
 * callable-form spec Handler(x: int): bool;
 */
typedef struct FengHandlerWitness {
    void (*invoke)(const void *callee, const void *arg_x, void *_out);
} FengHandlerWitness;

/* 示例：union-form 约束面的 witness type。
 * 实际代码必须按具体约束面生成命名；这里的 IntOrString 只用于说明形状。
 *
 * future union-form spec IntOrString: int | string;
 */
typedef struct FengIntOrStringWitness {
    bool (*test__int)(const void *subject);
    void (*project__int)(const void *subject, void *_out);
    bool (*test__string)(const void *subject);
    void (*project__string)(const void *subject, void *_out);
} FengIntOrStringWitness;
```

- 上面不是“运行时通用反射表”，而是**编译器按约束面直接生成的静态 witness 结构**。
- runtime 不需要理解这些 witness 结构的语义；runtime 继续只承载值模型与生命周期。约束 ABI 由 semantic/codegen 决定、声明和传递。
- object-form `spec` 的 slot 命名规则固定为：
  - 字段读：`get__FieldName`
  - 字段写：`set__FieldName`
  - 方法调用：`call__MethodName`
- callable-form `spec` 固定为单槽 `invoke`。
- future union-form 固定为成对槽位：`test__CaseName` + `project__CaseName`。

**witness type 与 witness instance 的边界**：

```c
/* Named 约束面的 witness type 见前文 FengNamedWitness 定义 */

/* 不同满足者：各自生成不同 instance */
extern const FengNamedWitness feng_witness__User__Named;
extern const FengNamedWitness feng_witness__Admin__Named;
```

- `Named`、`Hashable`、`Reader<int>`、`Reader<string>` 这类**不同约束面**，都应生成各自独立的 witness type。
- `User: Named`、`Admin: Named`、`fit Box<string>: Named` 这类**不同满足路径**，共用 `FengNamedWitness` 这个 type，但各自拥有不同 instance。
- 对泛型 `spec`，基线规则应按“完成类型实参替换后的约束面”生成 witness type；不要把“不同约束面偶然长得像”当成语义前提。若未来要做 ABI identical 的合并，也只能是优化，不得成为正确性前提。
- 绝不能退化成“一个通用 witness descriptor + slot 数组 + 名字查找”；那会把当前静态槽位 ABI 重新拉回运行时反射表，与本方案目标冲突。

**同一份 witness 的复用边界**：

- witness 能否被两个调用路径**字面上复用为同一个 instance**，取决于它们是否向 thunk 传入同一种 receiver 表示。
- 对 `UserType`、`string`、closure 等托管指针类型，泛型路径与一等 `spec` 路径都可以把具体对象指针作为 `subject` 传入，因此可以直接复用同一个 witness instance。
- 对值类型，泛型路径天然作用于值槽地址；若未来一等 `spec` 值路径为了生命周期管理仍以 subject carrier / 箱对象指针承载，则两条路径的 receiver 表示不同，**不能直接共用同一个 witness instance**。
- 这不是契约语义问题，而是 receiver ABI 问题。
- 完整态应把“无装箱值槽表示”作为泛型 witness 的规范 receiver ABI；一等 `spec` 值若需要 subject carrier，则应为 `spec` 路径生成一个静态 adapter witness，其 thunk 先解 carrier，再转发到同一约束面的值槽实现。
- 因此，泛型调用与 `spec` 调用共享的是同一约束面的 witness family、slot 契约与编译期静态生成机制；对值类型不强求共享同一个最终 instance。

**值类型 witness 的规范解法**：

- 对每个“值类型 `V` 满足约束面 `C`”的组合，编译器必须先生成一份**值槽 witness**，其所有 thunk 都以 `V` 的内联值槽地址作为 receiver ABI。
- `FengGenericParamDescriptor.witness` 在值类型场景下**只能**指向这份值槽 witness；共享主体不得改指向 `spec` 路径的 carrier witness。
- 若 object-form `spec` 的一等值路径仍要求 `subject` 指向 carrier / 箱对象，则编译器必须再生成一份**spec 路径适配 witness**；其 thunk 先把 `subject` 解释为 carrier，再把 carrier 中的 `V` 值槽地址转发给值槽 witness。
- 泛型路径与 `spec` 路径共享的是同一约束面的 slot 形状和底层实现逻辑，不共享同一个最终 witness instance。

```c
typedef struct FengPoint__Named__SpecCarrier {
    /* 实际 carrier 若需要 header / ARC 字段，按 carrier 自身规则补齐 */
    FengPoint value;
} FengPoint__Named__SpecCarrier;

static void FengPoint__Named__get__name__slot(const void *subject, void *_out) {
    const FengPoint *self = (const FengPoint *)subject;
    *(FengString **)_out = (FengString *)feng_retain(self->name);
}

static void FengPoint__Named__call__rename__slot(void *subject,
                                                 const void *arg_next,
                                                 void *_out) {
    FengPoint *self = (FengPoint *)subject;
    FengPoint__rename(self, *(FengString *const *)arg_next);
    (void)_out;
}

static const FengNamedWitness feng_witness__Point__Named__slot = {
    .get__name = FengPoint__Named__get__name__slot,
    .set__name = NULL,
    .call__rename = FengPoint__Named__call__rename__slot,
};

static void FengPoint__Named__get__name__spec(const void *subject, void *_out) {
    const FengPoint__Named__SpecCarrier *carrier =
        (const FengPoint__Named__SpecCarrier *)subject;
    feng_witness__Point__Named__slot.get__name(&carrier->value, _out);
}

static void FengPoint__Named__call__rename__spec(void *subject,
                                                 const void *arg_next,
                                                 void *_out) {
    FengPoint__Named__SpecCarrier *carrier =
        (FengPoint__Named__SpecCarrier *)subject;
    feng_witness__Point__Named__slot.call__rename(&carrier->value, arg_next, _out);
}

static const FengNamedWitness feng_witness__Point__Named__spec = {
    .get__name = FengPoint__Named__get__name__spec,
    .set__name = NULL,
    .call__rename = FengPoint__Named__call__rename__spec,
};

static const FengGenericParamDescriptor feng_generic_Point__Named = {
    .size      = sizeof(FengPoint),
    .kind      = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
    .aggregate = &FengPointAgg,
    .witness   = &feng_witness__Point__Named__slot,
};
```

- 上例中，泛型共享主体永远读取 `feng_generic_Point__Named.witness`，因此永远拿到值槽 ABI 的 witness。
- 一等 `spec` 值形成时，若 `subject` 必须是 carrier 指针，则 fat value 必须绑定 `feng_witness__Point__Named__spec`，而不是偷用 `feng_witness__Point__Named__slot`。

**实例化点生成规则**：

```c
static void FengUser__Named__get__name(const void *subject, void *_out) {
    const FengUser *self = (const FengUser *)subject;
    *(FengString **)_out = (FengString *)feng_retain(self->name);
}

static void FengUser__Named__set__name(void *subject, const void *value) {
    FengUser *self = (FengUser *)subject;
    feng_assign((void **)&self->name, *(void *const *)value);
}

static void FengUser__Named__call__rename(void *subject,
                                          const void *arg_next,
                                          void *_out) {
    FengUser *self = (FengUser *)subject;
    FengUser__rename(self, *(FengString *const *)arg_next);
    (void)_out;
}

static const FengNamedWitness feng_witness__User__Named = {
    .get__name = FengUser__Named__get__name,
    .set__name = FengUser__Named__set__name,
    .call__rename = FengUser__Named__call__rename,
};
```

- 编译器在“具体类型 `User` 满足 `Named`”的实例化点生成或选取对应 witness 实例。
- 泛型共享主体签名中隐藏参数只保留 `const FengGenericParamDescriptor *_T` 这一层抽象；共享主体需要约束能力时，再把 `_T->witness` 解释为 `const FengNamedWitness *`。共享主体既不关心 thunk 名字，也不关心它们来自 `fit` 还是直接满足。

**共享主体中的最终形态**：

```c
typedef struct FengHashableWitness {
     void (*call__hash)(const void *subject, void *_out);
     void (*call__equals)(const void *subject, const void *arg_other, void *_out);
} FengHashableWitness;

void FengMap__has__shared(
     void *_self,
     const size_t *_field_offsets,
     const FengGenericParamDescriptor *_K,
     const FengGenericParamDescriptor *_V,
     const void *_p_key,
     void *_out);
```

- 共享主体对 `K.hash()` 的 lowering 直接变成：

```c
const FengHashableWitness *_K_Hashable =
    (const FengHashableWitness *)_K->witness;
uint64_t _tmp_hash;
_K_Hashable->call__hash(_p_key, &_tmp_hash);
```

- 共享主体对 `K.equals(other)` 的 lowering 直接变成：

```c
const FengHashableWitness *_K_Hashable =
    (const FengHashableWitness *)_K->witness;
bool _tmp_eq;
_K_Hashable->call__equals(entry_key_ptr, _p_key, &_tmp_eq);
```

- object-form `T.name` 的 lowering 直接变成 `((const FengNamedWitness *)_T->witness)->get__name(t_ptr, out)`。
- object-form `T.name = x` 的 lowering 直接变成 `((const FengNamedWitness *)_T->witness)->set__name(t_ptr, x_ptr)`。
- callable-form `F(x)` 的 lowering 直接变成 `((const FengHandlerWitness *)_F->witness)->invoke(f_ptr, x_ptr, out)`。
- future union-form 收窄直接变成：

```c
const FengIntOrStringWitness *_V_IntOrString =
    (const FengIntOrStringWitness *)_V->witness;

if (_V_IntOrString->test__int(v_ptr)) {
    int64_t _tmp;
    _V_IntOrString->project__int(v_ptr, &_tmp);
    /* ... */
}
```

- 该路径没有运行时查表，只有编译期静态确定的 `witness` 槽位读取与最多一次间接调用。

#### 4. 外层泛型参数与方法级泛型参数统一展开

- 泛型类型上的泛型方法，不应再被视为特殊情况。
- ABI 统一规则如下。
- 先展开外层类型参数描述符。
- 再展开方法级类型参数描述符。
- 再展开显式普通参数。
- 最后是可选 `_out`。
- 这样天然支持任意多个泛型参数，也支持“泛型类型 + 泛型方法”。
- 对泛型 `type` 的共享方法，完整签名顺序固定为：`self` -> 宿主布局输入 -> 外层类型参数描述符（按声明顺序） -> 方法类型参数描述符（按声明顺序） -> 显式普通参数 -> 可选 `_out`。
- 对顶层泛型函数，完整签名顺序固定为：类型参数描述符（按声明顺序） -> 显式普通参数 -> 可选 `_out`。
- 这一顺序属于共享主体 ABI 的一部分；后续优化可以内联 wrapper、裁剪未使用局部，但不能改变主体签名展开顺序。

```c
/* type PairMap<K: Hashable, V> { func merge<U>(key: K, value: V, extra: U): void } */
void FengPairMap__merge__shared(
    void *_self,
    const size_t *_field_offsets,
    const FengGenericParamDescriptor *_K,
    const FengGenericParamDescriptor *_V,
    const FengGenericParamDescriptor *_U,
    const void *_p_key,
    const void *_p_value,
    const void *_p_extra,
    void *_out);

/* func clone<T>(value: T): T */
void FengClone__shared(
    const FengGenericParamDescriptor *_T,
    const void *_p_value,
    void *_out);
```

### 四、所有类型的统一落点

| 类型类别 | 完整态 generic arg 落点 | 约束 |
| --- | --- | --- |
| `bool` / 整数 / 浮点 | `FENG_VALUE_TRIVIAL` | 不分配托管堆对象 |
| `string` | `FENG_VALUE_MANAGED_POINTER` | 复用现有字符串运行时 |
| `T[]` / `T[]!` | `FENG_VALUE_MANAGED_POINTER` | 复用现有数组运行时 |
| `UserType` | `FENG_VALUE_MANAGED_POINTER` | 复用现有对象运行时 |
| object-form `spec` value | `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | 复用 fat value + aggregate descriptor |
| callable-form `spec` value | `FENG_VALUE_MANAGED_POINTER` | 复用 closure pointer 路径 |
| union-form `spec` value | 复用 union 自身 carrier 的值模型 | generic 层不单独发明第四类 |
| tuple / 其他按值聚合类型 | `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | 统一走 aggregate 描述符 |

说明：`void` 仍按既有语义处理，不作为普通字段/实参/返回值的可存储泛型值类型。

### 五、所有契约的统一落点

约束位置只接受 `spec` 引用，不接受具体 `type` 等值约束。Feng 当前没有继承/子类型层级；把类型参数约束写成“必须精确等于某具体类型”不会提供新的契约能力，也不构成有意义的泛型抽象，因此完整态不保留这一路径。

#### 1. object-form `spec` 约束

- 语义层提供固定成员面。
- codegen 生成 witness 结构，其中包含：
  - 字段 getter/setter thunk；
  - 方法 thunk；
  - 必要时父 `spec` 继承闭包展平后的成员槽位。
- `t.field` / `t.method()` 在共享主体中都应通过 `_T->witness` 上的固定槽位 lowering。

#### 2. callable-form `spec` 约束

- 语义层把 `T(...)` 解释为对 callable-form 契约的直接调用。
- codegen 为该约束生成单独 invoke witness，例如 `call(const void *callee, ..., void *_out)`。
- 共享主体调用时至多一次间接调用；不得先物化成 object-form `spec` 或做运行时匹配。

#### 3. union-form `spec` 约束

- 语义层继续复用 union 既有规则：先收窄，再访问允许的 member。
- codegen 必须通过固定 witness / carrier 操作进入 union lowering；generic 层不得重造一套独立的 union 运行时。
- 即使当前源码尚未把 union-form 纳入同一条 code path，完整态设计也必须按这条原则保留 ABI。

### 六、性能与零分配要求

1. 非契约泛型路径只允许 descriptor 分支与必要的 retain/release；不得引入额外间接调用。
2. 契约路径最多一次间接调用；允许的间接仅来自静态 witness thunk。
3. 禁止运行时查表，包括但不限于：
    - `(type, spec)` -> witness 字典
    - 字符串或符号 ID 查找
    - 运行时枚举 witness 数组再匹配目标
4. 值类型不得因为泛型或契约调用而产生托管堆分配。
5. object-form `spec` 的一等值语义与泛型约束调用必须分离：
    - 一等 spec 值走既有 spec value 语义；
    - 泛型约束调用走 `FengGenericParamDescriptor.witness` ABI；
    - 泛型路径不得依赖未来的 fit 值类型装箱方案。

---

## 一、开始实现前需要明确的规则

以下问题规范中尚未收口，必须由开发者决策后才能推进对应实现。  
**每项决策结果应直接更新到对应规范文档，再开始编码。**

---

### Q1 代码生成策略

**决策**：**布局单态化 + 方法共享**，现有非泛型发码路径零改动。

**核心思路**：值在结构体字段里，方法/顶层泛型函数都通过描述符了解类型参数的类型性质。完整态按三层拆分：

- **struct 布局**：按具体 T 在使用点单态化生成，字段大小由 T 决定
- **wrapper**：按具体实例单态化生成；对成员方法负责组装 `self`、布局输入、`FengGenericParamDescriptor` 与其他常量，对顶层泛型函数负责组装类型参数描述符与其他常量，然后转发到共享主体
- **共享主体**：只编译一份；成员方法通过 `void *self` + 布局输入 + 多个 `FengGenericParamDescriptor *` + 显式参数 + 可选 `_out` 操作，顶层泛型函数则省去 `self`/布局输入段

Q1 的抽象目标不是“先把当前已知类型跑通”，而是让共享主体只依赖稳定抽象输入。对未来新类型，编译器应优先通过补齐该类型的 `FengGenericParamDescriptor`（以及必要时复用既有 aggregate 描述符与 witness 实例）完成接入；泛型主体本身不应因为类型扩展而改写控制流。这是泛型方案必须满足的开闭原则。

Q1 的架构单位不是功能点，而是四类稳定抽象输入：

1. **泛型参数描述**：`FengGenericParamDescriptor` 负责告诉共享主体如何复制、保活、返回值，并在 `witness` 非空时暴露约束见证。
2. **宿主布局输入**：共享主体只按字段序读布局输入，不直接依赖具体实例布局如何算出。
3. **约束面 witness 生成规则**：所有受约束操作都走 `_T->witness` 的固定槽位，不借道一等 `spec` 值。
4. **统一泛型环境展开规则**：外层类型参数、方法类型参数、对应 descriptor 的顺序一旦定义，就不再随场景漂移。

G6-1 ~ G6-6 只是把具体类型、具体约束、具体场景映射进这四类稳定输入；它们不是架构本身。只要映射关系成立，泛型共享主体就不应为新增类型或未来 form 扩展而改写核心控制流。

**struct 布局单态化**（编译器在使用点自动生成，用户无感知）：

```c
// type Box<T> { let value: T; }

// Box<int>
typedef struct { FengManagedHeader _hdr; int64_t value; } FengBox__int;

// Box<Widget>（Widget 是 spec，16 字节 fat value）
typedef struct { FengManagedHeader _hdr; struct FengSpecValue__Widget value; } FengBox__Widget;

// Box<(int, float)>（值类型 tuple，未来支持）
typedef struct { FengManagedHeader _hdr; int64_t value_0; double value_1; } FengBox__tuple_i64_f64;
```

**共享主体只编译一份**（泛型专有发码，依赖宿主布局输入 + 按顺序展开的 `FengGenericParamDescriptor *` + 显式参数 + out 参数）：

```c
// func get(): T
void FengBox__get__shared(void *self, const size_t *field_offsets,
                         const FengGenericParamDescriptor *T, void *out) {
    void *fp = (char *)self + field_offsets[0];
    switch (T->kind) {
        case FENG_VALUE_TRIVIAL:                           break;
        case FENG_VALUE_MANAGED_POINTER:                   feng_retain(*(void **)fp); break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:      feng_aggregate_retain(fp, T->aggregate); break;
    }
    memcpy(out, fp, T->size);
}
```

**单态 wrapper**（编译器在实例化点生成，负责绑定常量并转发）：

```c
static void FengBox__get__int(FengBox__int *self, void *out) {
    static const size_t _layout[] = { offsetof(FengBox__int, value) };
    static const FengGenericParamDescriptor _T =
        { .size=8, .kind=FENG_VALUE_TRIVIAL, .aggregate=NULL, .witness=NULL };
    FengBox__get__shared(self, _layout, &_T, out);
}

static void FengBox__get__Widget(FengBox__Widget *self, void *out) {
    static const size_t _layout[] = { offsetof(FengBox__Widget, value) };
    static const FengGenericParamDescriptor _T = {
        .size      = sizeof(struct FengSpecValue__Widget),
        .kind      = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
        .aggregate = &FengSpecAgg__Widget,
        .witness   = NULL,
    };
    FengBox__get__shared(self, _layout, &_T, out);
}
```

**调用点**（源码层调用 wrapper；wrapper 再转发到共享主体）：

```c
// Box<int>.get()
int64_t x;
FengBox__get__int(box, &x);

// Box<Widget>.get()
struct FengSpecValue__Widget w;
FengBox__get__Widget(box, &w);

// 子表达式场景：let y = b.get() + 1
int64_t _tmp;
FengBox__get__int(box, &_tmp);
int64_t y = _tmp + 1;
```

**顶层泛型函数也生成单态 wrapper**（与成员方法同一路线，只是不含 `self` / 布局输入）：

```c
/* func clone<T>(value: T): T */
void FengClone__shared(const FengGenericParamDescriptor *T,
                      const void *p_value,
                      void *out) {
    switch (T->kind) {
        case FENG_VALUE_TRIVIAL:                           break;
        case FENG_VALUE_MANAGED_POINTER:                   feng_retain(*(void *const *)p_value); break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:      feng_aggregate_retain(p_value, T->aggregate); break;
    }
    memcpy(out, p_value, T->size);
}

static void FengClone__int(const int64_t *p_value, void *out) {
    static const FengGenericParamDescriptor _T =
        { .size=8, .kind=FENG_VALUE_TRIVIAL, .aggregate=NULL, .witness=NULL };
    FengClone__shared(&_T, p_value, out);
}

static void FengClone__string(FengString *const *p_value, void *out) {
    static const FengGenericParamDescriptor _T =
        { .size=8, .kind=FENG_VALUE_MANAGED_POINTER, .aggregate=NULL, .witness=NULL };
    FengClone__shared(&_T, p_value, out);
}
```

- 因此，顶层泛型函数与泛型方法在非单态主路径下都必须有编译期单态 wrapper；区别只在于前者没有 `self` / 布局输入。
- wrapper 的命名是编译器内部稳定名字问题，`FengClone__int`、`foo__int`、`foo_int` 都可以作为实现细节；不变的是“共享主体 1 份 + 每个具体实例 1 个薄 wrapper”这个结构。

**`FengGenericParamDescriptor`（完整态运行时结构）**：

```c
typedef struct FengGenericParamDescriptor {
    size_t          size;        /* T 占用的字节数（8/16/N×8） */
    FengValueKind   kind;        /* TRIVIAL / MANAGED_POINTER / AGGREGATE */
    const FengAggregateValueDescriptor *aggregate;  /* 仅 AGGREGATE 时非 NULL */
    const void     *witness;     /* 无约束时为 NULL；spec 约束时指向静态 witness */
} FengGenericParamDescriptor;
```

各类 T 的静态实例（codegen 在使用点生成）：

```c
// 所有 size = 8 且无约束的 trivial 值类型可共用一个实例
const FengGenericParamDescriptor feng_generic_trivial8_param =
    { .size=8, .kind=FENG_VALUE_TRIVIAL, .aggregate=NULL, .witness=NULL };

// UserType — managed pointer，编译器为每个 T 生成
const FengGenericParamDescriptor feng_generic_Foo_param =
    { .size=8, .kind=FENG_VALUE_MANAGED_POINTER, .aggregate=NULL, .witness=NULL };

// spec value — aggregate，复用已有 FengAggregateValueDescriptor，零重复
const FengGenericParamDescriptor feng_generic_Widget_param = {
    .size      = sizeof(struct FengSpecValue__Widget),
    .kind      = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
    .aggregate = &FengSpecAgg__Widget,   // 直接引用现有结构，无重复数据
    .witness   = NULL
};
```

若 `K: Hashable`，则对应实例只需把 `.witness` 指向编译期现成的 `feng_witness__K__Hashable`；共享主体仍然只接收一个 `_K`。若同一主体还同时涉及 `V`、`U` 等其他类型参数，则它们各自拥有 `_V`、`_U` 描述符，并按 ABI 规则一并传入。

若 `K` 是值类型且一等 `spec` 值路径未来仍使用 subject carrier，则 `FengGenericParamDescriptor.witness` 应指向“值槽 ABI”的 witness；`spec` coercion 路径再生成自己的静态 adapter witness。泛型路径不得为了复用 `spec` 路径 witness 而引入装箱。

**与现有运行时结构的关系**：

| 结构 | 描述对象 | 是否改动 |
| --- | --- | --- |
| `FengTypeDescriptor` | 堆上托管对象（有 header），用于 `feng_object_new`、ARC、Phase 1B GC | **零改动** |
| `FengAggregateValueDescriptor` | 栈上聚合值（无 header），用于 spec value ARC | **零改动，直接复用** |
| `FengGenericParamDescriptor` | 泛型参数位置上的统一使用契约；同时给出值模型字段与可选 witness | **由当前 `FengGenericValueDescriptor` 重命名并扩展** |

**运行时开销**（vs 非泛型、vs spec dispatch）：

- 方法调用：调用点先 **direct call wrapper**，wrapper 再 **direct call 共享主体**；同包场景下 wrapper 可被内联
- 字段操作：`memcpy(T->size)`，对 8 字节值被 C 编译器折叠为单条 MOV
- ARC：switch 三分支，trivial 分支立即跳出；分支预测友好
- 整体：跨包共享主体路径比非泛型多一个很薄的 wrapper 转发与 out 参数；约束调用仍只多一次 witness 间接调用，比运行时查表或动态派发更可控

**对现有代码的改动**：

- 现有 `cg_register_user_type`、`cg_emit_user_method`、`cg_emit_function` 等：**零改动**
- 新增 `cg_emit_generic_type_instance`（使用点触发，生成 struct 定义）
- 新增 `cg_emit_generic_method_wrapper`、`cg_emit_generic_fn_wrapper`
- 新增 `cg_emit_generic_shared_method`、`cg_emit_generic_shared_fn`、`cg_emit_generic_call`
- 运行时把当前 `FengGenericValueDescriptor` 重命名并扩展为 `FengGenericParamDescriptor`，并提供 `feng_generic_trivial8_param` 等实例
- `.fb` 分发：共享方法体预编译进 `lib/`，struct 定义在消费方生成，支持闭源分发

**两阶段发码架构（进阶优化预留）**：

`cg_emit_generic_xxx` 是泛型发码的统一 API 层，内部策略可以独立演进：

| 阶段 | 策略 | 适用场景 | 方法编译份数 |
| --- | --- | --- | --- |
| 当前实现（通用） | **布局单态化 + wrapper 单态化 + 方法共享** | 所有场景，含跨包闭源 `.fb` | 1 份共享主体 + N 个薄 wrapper |
| 进阶优化（可选） | **源码全单态化** | 同包泛型，源码可见 | 每个具体 T 一份 |

进阶优化时，`cg_emit_generic_xxx` 的**调用接口不变**，只在内部按可见性分流：

```c
// 示例：未来分流点，接口对调用方完全透明
static void cg_emit_generic_method(CG *cg, ...) {
    if (cg_generic_source_visible(cg, generic_type)) {
        cg_emit_generic_method_monomorphized(cg, ...);  // 全单态化：生成具体类型函数
    } else {
        cg_emit_generic_method_shared(cg, ...);         // wrapper + 共享主体：void* + T + out
    }
}
```

**结论**：当前方案本身就位于“全单态化”和“动态派发”之间：布局与 wrapper 是单态的，主体语义是共享的，契约调用走静态 witness 槽位而不是运行时反射。未来若对本包源码可见实例追加更激进的全单态化，只需要在 wrapper / 发码策略层分流，不需要推翻共享主体 ABI。

**规范更新**：`docs/specifications/feng-generics-draft.md` §9 需要按此方案重写（详见 G0-1）。

---

### Q2 泛型约束是否可以写泛型 spec 实例

**决策**：**允许**。泛型约束可以是任何可解析的 spec 引用，包括泛型 spec 的具体实例。

```feng
spec Reader<T> {
  func get(): T;
}

type Box<T: Reader<int>> {   // 合法：泛型约束是泛型 spec 的具体实例
  ...
}
```

**影响实现**：

- 语义分析中泛型约束解析逻辑：泛型约束按普通类型引用解析，支持 `NAMED_GENERIC` 节点（即带类型实参的 spec 引用）
- 父 spec 约束检查中，`spec Child<T>: Parent<int>` 的 `Parent<int>` 同样是泛型 spec 实例，按同样规则处理
- `.ft` 中 type_param 的约束 TYPS 节点可以是 `FT_TYPE_KIND_NAMED_GENERIC` 节点

---

### Q3 泛型 type 的默认零值规则

**决策**：**按字段递推**。泛型 type 的零值规则与非泛型 type 一致：将类型参数替换为实际类型实参后，若所有字段都有默认零值，则该泛型实例有默认零值（字段逐一取零值）。

```feng
type Box<T> {
  let value: T;
}

let b: Box<int>;   // 合法：value 取 int 零值（0），b 是 Box<int> 的零值实例
```

**影响实现**：

- 语义分析中的零值推导逻辑：将类型参数替换为实际类型实参后，按现有字段递推规则判断
- 若某字段的实际类型本身无零值（如某些带约束的 spec 类型），仍须显式初始化，诊断规则不变

---

### Q4 `>>` token 与嵌套泛型的歧义处理

**决策**：采用策略 1（Parser 内部 `pending_gt` 计数器），词法器零改动。

**问题背景**：词法器遇 `>` 后紧接 `>` 时，会整体产出 `FENG_TOKEN_SHR`（`>>`）（见 `src/lexer/lexer.c` `case '>'` 分支）。在 `Map<string, List<int>>` 这类嵌套泛型中，末尾 `>>` 整体被产出为 `SHR`，Parser 无法正确闭合两层 `<...>`。

**方案细化**：Parser 在类型实参解析上下文中维护一个 `int pending_gt` 计数器：

- 需要消费一个 `>` 时，先检查 `pending_gt > 0`：若是则 `pending_gt--`，不取新 token。
- `pending_gt == 0` 且当前 token 是 `SHR`（`>>`）：消费该 token，`pending_gt = 1`，本次得到一个 `>`。
- `pending_gt == 0` 且当前 token 是 `GT`（`>`）：正常消费。

`Map<string, List<int>>` 末尾 `>>` 先闭合内层 `List<int>`，再从 `pending_gt` 取第二个 `>` 闭合外层 `Map<...>`；表达式中的移位运算 `a >> b` 不在类型实参上下文中，`pending_gt` 不参与，不受影响。

---

### Q5 泛型推导的冲突报错规则

**决策**：从实参列表从左到右推导，第一个能确定类型参数的实参位置为准；后续实参若产生不匹配，在**该不匹配实参的位置**报编译错误（信息：参数类型不匹配，期望 `T = <已推导类型>`，实际 `<当前类型>`）。

```feng
func pair<T>(left: T, right: T): T { ... }

pair(1, "x");
//       ^^^ 编译错误：right 期望类型 int（由 left 推导 T = int），实际类型 string
```

**规则细化**：

- 推导优先级：接收者静态类型（若有）> 实参列表从左到右 > 上下文目标类型（如赋值期望类型）
- 若所有信息都不足以确定某个类型参数，则在整个调用表达式处报"无法推导类型参数 T"
- 若某位置推导出的类型与已确定结论冲突，则在该不匹配的实参位置报错

---

## 二、任务拆解

以下任务列表按推荐实现顺序排列。每项任务完成后应补测试并执行全量回归。

> **前置**：Phase 5.5 符号表结构重构必须先完成。详见 `docs/engineering/feng-plan.md`。

---

### G0 规则收口（前置于一切编码）

| 编号 | 任务 | 产出 | 备注 |
| --- | --- | --- | --- |
| G0-1 | 决策 Q1 代码生成策略，写入 `docs/specifications/feng-generics-draft.md` §9 | 规范更新 | ✓ 已决策，参见 Q1 节；阻塞 G6 已解除 |
| G0-2 | 决策 Q2 泛型约束是否可以写泛型 spec 实例，更新规范 §4 和 §5 | 规范更新 | ✓ 已决策，参见 Q2 节 |
| G0-3 | 决策 Q3 泛型 type 默认零值规则，更新 `docs/specifications/feng-type.md` | 规范更新 | ✓ 已决策，参见 Q3 节（原阻塞 G4-19） |
| G0-4 | 决策 Q4 `>>` token 处理策略 | 规范更新 | ✓ 已决策，参见 Q4 节；G1-2 可删除 |

---

### G1 词法分析

词法器本身不需要新增关键字。主要工作取决于 Q4 的决策。

| 编号 | 任务 | 涉及文件 | 依赖 |
| --- | --- | --- | --- |
| G1-1 | 确认 `:<` 序列不被词法器合并为新 token，保持 `COLON` + `LT` 独立产出 | `src/lexer/lexer.c` | — |
| G1-2 | ~~（Q4 已决策为策略 1，本任务取消）~~ 词法器零改动，无需新增接口 | — | — |
| G1-3 | 补词法层单元测试：`:<`、`Map<int>`、`Map<List<int>>`、`a >> b`（位移）不被误拆 | `test/lexer/` | G1-1、G1-2 |

---

### G2 AST 扩展

Parser 的 AST 数据结构当前完全没有泛型信息，需要系统性扩展。

#### G2-1 新增类型参数定义节点

在 `src/parser/parser.h` 中新增：

```c
/* 单个类型参数定义，如 <T> 或 <T: Named> */
typedef struct FengTypeParam {
    FengToken token;
    FengSlice name;          /* 参数名 */
    FengTypeRef *constraint; /* 泛型约束（NULL = 无约束） */
} FengTypeParam;
```

| 编号 | 任务 | 涉及文件 |
| --- | --- | --- |
| G2-1 | 新增 `FengTypeParam` 结构 | `src/parser/parser.h` |

#### G2-2 扩展声明节点

| 编号 | 任务 | 修改内容 |
| --- | --- | --- |
| G2-2a | `type_decl` 新增 `FengTypeParam *type_params; size_t type_param_count;` | `parser.h` |
| G2-2b | `spec_decl` 新增 `FengTypeParam *type_params; size_t type_param_count;` | `parser.h` |
| G2-2c | `FengCallableSignature` 新增 `FengTypeParam *type_params; size_t type_param_count;`（用于顶层泛型 fn 和泛型方法） | `parser.h` |
| G2-2d | `fit_decl` 已有 `target`（FengTypeRef），但需确认泛型 fit 左侧 `<T>` 的表达方式——由于规范明确 fit 左侧 `<T>` 不是新参数定义而是引用，直接复用 `FengTypeRef.named` 中的 type_args 即可，不需要新增 type_params | `parser.h`（审查，可能无需改动） |

#### G2-3 扩展类型引用节点

```c
/* FengTypeRef.named 扩展：增加类型实参列表 */
struct {
    FengSlice *segments;
    size_t segment_count;
    FengTypeRef **type_args;   /* 新增 */
    size_t type_arg_count;     /* 新增 */
} named;
```

| 编号 | 任务 |
| --- | --- |
| G2-3 | 扩展 `FengTypeRef.named` 新增 `type_args` / `type_arg_count` |

#### G2-4 扩展调用表达式节点

```c
struct {
    FengExpr *callee;
    FengExpr **args;
    size_t arg_count;
    FengResolvedCallable resolved_callable;
    /* 新增：显式泛型调用 callee:<T1, T2>(...) 的类型实参 */
    FengTypeRef **explicit_type_args;
    size_t explicit_type_arg_count;
    bool has_explicit_type_args;
} call;
```

| 编号 | 任务 |
| --- | --- |
| G2-4 | 扩展 `FengExpr.call` 新增 `explicit_type_args` / `explicit_type_arg_count` / `has_explicit_type_args` |

---

### G3 语法分析（Parser）扩展

基于扩展后的 AST，扩展 Parser 支持泛型语法。

| 编号 | 任务 | 说明 |
| --- | --- | --- |
| G3-1 | 扩展 `type` 声明解析：检测 `Name<...>` 中的类型参数列表并填入 `type_params` | `src/parser/parser.c` |
| G3-2 | 扩展 `spec` 声明解析（object-form、callable-form）：检测类型参数列表 | `src/parser/parser.c` |
| G3-3 | 扩展类型引用解析 `parse_type_ref`：检测 `Name<T1, T2>` 并填入 `type_args`；同时解决 `>>` 歧义（依赖 G0-4 决策） | `src/parser/parser.c` |
| G3-4 | 扩展成员方法和顶层 fn 的声明解析：检测 `fn Name<T>` 中的类型参数 | `src/parser/parser.c` |
| G3-5 | 扩展调用表达式解析：检测 `callee:<T1, T2>(...)` 的显式泛型调用语法 | `src/parser/parser.c` |
| G3-6 | 确保 `parse_type_ref` 中 `<...>` 仅作为类型实参，不在非调用位置误解析 `:<...>` | `src/parser/parser.c` |
| G3-7 | 扩展 `spec` 父列表解析（`spec Child: Parent<T>`）：父列表使用扩展后的类型引用解析即可，无需单独处理 | `src/parser/parser.c` |
| G3-8 | 扩展 dump/print：新增 `type_params`、`type_args`、`explicit_type_args` 的输出 | `src/parser/dump.c` |
| G3-9 | Parser 单元测试：泛型声明、类型实例化引用、显式泛型调用、嵌套泛型、错误语法拒绝 | `test/parser/` |

**Parser 关键边界**（不能越界）：

- Parser 不得依赖语义判断某个 `<...>` 是否是泛型调用；只有 `:<...>` 才标记为显式泛型调用
- `foo<T>(...)`、`pkg.foo<T>(...)` 不得被 Parser 解析为显式泛型调用

---

### G4 语义分析扩展

这是实现量最大、最复杂的一环。

> **当前复查结论**：G4 中“声明、作用域、推导、identity、约束声明合法性”已经落地；但 G4-8 的“约束体内成员访问”和 G4-12 之后真正进入 codegen 的调用闭环没有完成，因此本阶段不能视为结束。

| 编号 | 任务 | 说明 | 依赖 |
| --- | --- | --- | --- |
| G4-1 | **类型参数作用域**：为每个泛型声明建立类型参数作用域，把类型参数名注册为可解析名字 | `src/semantic/analyzer.c` | — |
| G4-2 | **类型参数引用解析**：在类型位置解析时，区分"具名类型引用"和"类型参数引用" | `src/semantic/analyzer.c` | G4-1 |
| G4-3 | **泛型约束解析与记录**：解析每个类型参数的泛型约束（无约束 / spec 约束） | `src/semantic/analyzer.c` | G4-1 |
| G4-4 | **泛型约束是泛型 spec 实例时的处理**（G0-2 已决策：允许，泛型约束按 `NAMED_GENERIC` 节点解析） | `src/semantic/analyzer.c` | — |
| G4-5 | **泛型声明 identity 注册**：按"名称 + 泛型参数数量"注册具名泛型 type/spec；冲突检查 | `src/semantic/analyzer.c` | — |
| G4-6 | **泛型实例类型匹配**：按不变规则（invariance）检查泛型实例兼容性；类型参数不同的实例不兼容 | `src/semantic/analyzer.c` | — |
| G4-7 | **泛型具名 type/spec 的使用解析**：按"名称 + 泛型参数数量"精确解析；实参数量不匹配时报错 | `src/semantic/analyzer.c` | G4-5 |
| G4-8 | **约束体内成员访问**：object-form `spec` 路径已通过 `_T->witness` 闭环；剩余 callable-form 按签名调用、union-form 复用收窄规则 | `src/semantic/spec_member_accesses.c` | G4-3 |
| G4-9 | **无约束类型参数的成员访问禁止**：无约束类型参数不得访问成员、做关系/逻辑运算 | `src/semantic/analyzer.c` | G4-1 |
| G4-10 | **泛型重载扩展**：在现有重载规则基础上，把"泛型参数数量"并入重载签名；泛型约束不参与 | `src/semantic/analyzer.c` | G4-5 |
| G4-11 | **非泛型优先**：当精确具体类型候选和泛型候选同时可匹配时，优先非泛型 | `src/semantic/analyzer.c` | G4-10 |
| G4-12 | **泛型推导**：省略显式类型实参时，从实参类型、接收者静态类型、上下文目标类型推导类型参数；不唯一时报错 | `src/semantic/analyzer.c` | G4-7 |
| G4-13 | **显式泛型调用验证**：`:<...>` 只允许在泛型可调用目标上；对非泛型函数写 `:<...>` 报错；实参数量必须与类型参数个数一致 | `src/semantic/analyzer.c` | G4-7 |
| G4-14 | **方法泛型参数重名检查**：泛型 type 内的方法泛型参数名不得与外层类型泛型参数名重名 | `src/semantic/analyzer.c` | G4-1 |
| G4-15 | **泛型父 spec 约束传递检查**：`spec Child<T>: Parent<T>` 中 T 传递到 Parent 时，检查 T 是否满足 Parent 对应位置的约束 | `src/semantic/spec_relations.c` | G4-3 |
| G4-16 | **泛型 fit 左侧解析**：`fit Box<T>: Reader<T>` 中 `<T>` 是对 `Box<T>` 已声明参数的引用，按"名称 + 泛型参数数量"匹配目标 type | `src/semantic/analyzer.c` | G4-5 |
| G4-17 | **泛型 fit 满足性检查**：检查 `fit Box<T>: Reader<T>` 中方法签名是否满足 `Reader<T>`；此处 T 统一指向 `Box<T>` 的类型参数 | `src/semantic/spec_relations.c` | G4-16 |
| G4-18 | **终结器泛型参数拒绝**：泛型 type 内的终结器不允许携带类型参数 | `src/semantic/analyzer.c` | — |
| G4-19 | **默认零值泛型扩展**（G0-3 已决策：按字段递推，类型参数替换后与非泛型规则一致） | `src/semantic/analyzer.c` | — |
| G4-20 | **语义分析单元测试**：所有正确语法通过；所有错误语法（错误语法 1-12）报错 | `test/semantic/` | — |
| G4-21 | **generic 实例化点 witness materialization**：已完成；generic function / generic method / generic type instantiation 的实际 demand 点现在统一调用 analyzer witness compute，semantic sidecar 已能为 `(ConcreteType, ConstraintSpec)` 稳定产出 witness | `src/semantic/analyzer.c`、`src/semantic/spec_witnesses.c` | G4-3、G4-8 |

**G4 当前补齐重点**：

1. 受约束类型参数在语义层必须形成稳定的“能力视图”，而不是只停留在“约束存在”。
2. `T: SomeSpec` 下的 `t.some()`，object-form 路径已经进入 `_T->witness`，但 analyzer 仍需在 generic 实例化点统一 materialize witness，避免 codegen 继续承担 semantic sidecar 回退职责。
3. `T: SomeSpec` 下的 `t.some()`，必须在语义信息中明确区分：
   - 这不是普通 object method call；
   - 这也不等同于强制先物化成一等 spec 值。
4. callable-form `spec` 约束下的直接调用必须有独立的 resolved callable 形态。
5. union-form `spec` 约束必须继续复用 union 收窄规则，不能在泛型里另起一套访问语义；该项随 union-form 语法落地后再进入实现收口。
6. `Map<K: Hashable, V>` 要求的 `hash` / `equals` 场景，必须先在语义层具备稳定的 resolved callable / witness 槽位形态，然后才能进入 G6。

---

### G5 符号表导出（.ft 泛型支持）

基于 Phase 5.5 完成的 TSEQ 架构，扩展 ft_write.c 支持泛型声明导出。

| 编号 | 任务 | 说明 |
| --- | --- | --- |
| G5-1 | **type_param 符号导出**：为泛型 type/spec 的每个类型参数导出 `FT_SYM_KIND_TYPE_PARAM` 符号（owner = 泛型声明符号，顺序按声明顺序） | `src/symbol/ft_write.c` |
| G5-2 | **TYPE_PARAM_REF 类型节点**：在 TYPS 中生成 `FT_TYPE_KIND_TYPE_PARAM_REF` 节点（string_ref = 参数名，sym_ref = type_param 符号 ID） | `src/symbol/ft_write.c` |
| G5-3 | **NAMED_GENERIC 类型节点**：为泛型具名使用（如 `Box<T>`、`List<int>`）生成 `FT_TYPE_KIND_NAMED_GENERIC` 节点 + TSEQ 类型实参 | `src/symbol/ft_write.c` |
| G5-4 | **CALLABLE 类型节点**：泛型函数/方法的 type_ref 指向 CALLABLE 节点，其 TSEQ 中参数类型可含 TYPE_PARAM_REF | `src/symbol/ft_write.c` |
| G5-5 | **泛型 spec 的 TYPS 编码**：按 SPEC_OBJECT / SPEC_CALLABLE TYPS.kind 区分 form，sym_ref = spec 符号 ID | `src/symbol/ft_write.c` |
| G5-6 | **泛型 fit 的 extra_ref 与 attr**：fit 符号的 extra_ref 指向 NAMED_GENERIC 类型节点；FT_ATTR_DECLARED_SPECS 范围存 `Reader<T>` 等结构化使用 | `src/symbol/ft_write.c` |
| G5-7 | **泛型声明的跨模块读取**：在 `src/symbol/imported_module.c` 中扩展 .ft 读取，重建泛型声明的类型参数和 NAMED_GENERIC 使用节点 | `src/symbol/imported_module.c` |
| G5-8 | **符号表单元测试**：泛型 type/spec/fn/fit 的导出内容验证；跨模块读取后符号查询正确 | `test/symbol/` |

---

### G6 代码生成

**依赖 G0-1 决策**。Q1 路线已经确定，不再重开。当前 G6 的核心不是重选路线，而是把“共享主体 + 描述符”的第一阶段实现补齐到完整状态。

| 编号 | 任务 | 说明 |
| --- | --- | --- |
| G6-1 | **完成当前可用 generic arg 类型覆盖**：内建值类型、`string`、数组、`UserType`、受约束 object-form `spec`、callable-form `spec` 进入统一 generic ABI；future union-form / tuple / value-struct 随对应语言形态落地后补接入 | `src/codegen/codegen.c` |
| G6-2 | **完成当前 aggregate 返回支持**：共享泛型函数/方法返回 object-form `spec` 时 `_out` 路径必须闭环；future tuple / 其他 aggregate 随稳定布局补齐 | `src/codegen/codegen.c` |
| G6-3 | **完成 object-form `spec` 约束的 witness lowering**：字段 getter/setter 与方法 thunk 全部进入静态 witness 结构 | `src/codegen/codegen.c` |
| G6-4 | **完成 callable-form `spec` 约束的 invoke lowering**：共享主体可直接调用 callable-form 契约 | `src/codegen/codegen.c` |
| G6-5 | **为 union-form `spec` 保留统一 generic ABI 设计**：复用 union runtime/value model，不在 generic 层新增第四类分发机制；实现随 union-form 语法阶段推进 | `src/codegen/codegen.c` |
| G6-6 | **已完成泛型类型上的泛型方法基础路径**：外层类型参数与方法类型参数统一进入共享 ABI，覆盖推导与显式方法类型实参调用 | `src/codegen/codegen.c` / `src/semantic/analyzer.c` / `test/codegen/` / `test/smoke/` |
| G6-7 | **补齐代码生成单元测试与 smoke** | `test/codegen/` |

**G6 当前必须坚持的实现原则**：

1. **不推翻 Q1**：继续采用“布局单态化 + 方法共享”，不回退到“所有场景全单态化”。
2. **先 correctness，后优化**：先把各类值与契约路径跑通，再做更激进的专门化。
3. **值类型要按类别收口，不做点状特判**：
   - trivial 值类型：走 `FENG_VALUE_TRIVIAL`
   - managed pointer：走 `FENG_VALUE_MANAGED_POINTER`
   - aggregate value：走 `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`
4. **泛型约束调用与一等 spec 值分离**：
   - 一等 spec 值是用户可见值语义；
   - 泛型约束调用是共享主体内部 ABI 语义；
   - 两者不能继续混成同一条 lowering。
5. **semantic / codegen 职责边界明确**：analyzer 已负责在 generic 实例化点 materialize semantic witness sidecar；codegen 最终仍应以消费稳定的 descriptor / witness 输入为主，不以回退合成作为完成态前提。
6. **禁止运行时查表**：所有 witness 读取都必须经由 `FengGenericParamDescriptor.witness` 的静态解释进入共享主体。
7. **值类型零堆分配**：泛型路径不得引入任何为了契约或多态而存在的托管堆分配。
8. **callable-form 走统一 callable value 表示**：callable-form `spec` 与 generic callable constraint 必须收口到同一套 callable value / invoke lowering；不能为 generic path 另起一条只在约束内部可用的特殊 ABI。

**G6 后续优化思路**：

#### O1. 用 `FengGenericParamDescriptor.witness` 收口受约束类型参数

- 共享泛型主体对每个类型参数只接收一个 `FengGenericParamDescriptor`，约束能力统一从 `witness` 读取。
- `Map<K: Hashable, V>` 中的 `K.hash()` / `K.equals()` 应 lower 为 witness 调用，而不是先物化 object-form spec 值。
- 这条路线的收益：
  - 避免在热路径上构造一等 spec 值；
  - 避免为标量值类型/普通值类型引入额外装箱；
  - 更符合 `.fb` 分发下共享主体 ABI 的长期稳定性。

#### O1-a. object-form / callable-form / union-form 约束必须各自有稳定 witness 族

- object-form `spec`：成员访问与方法调用走固定 witness 槽位。
- callable-form `spec`：直接调用走 invoke witness。
- union-form `spec`：收窄与 member 进入走 union witness / carrier 操作；当前暂缓到 union-form 语法阶段。
- 三类约束不能再混成“都先变成一等 spec 值”。

#### O2. aggregate 泛型路径直接复用已有 aggregate 描述符体系

- object-form spec value、未来 tuple、其他聚合值，不应各走各的特殊通道。
- 统一通过 `FengAggregateValueDescriptor` 进入泛型复制、retain、release、assign、return 路径。
- 当前 object-form spec value 已经使用这条路径；future tuple / value-struct 在自身布局规范稳定前不纳入本轮完成口径。

#### O2-a. callable-form `spec` 不进入 aggregate 通道

- callable-form `spec` 的值模型是 managed pointer；完整态必须直接复用 closure pointer 路径。
- 不能把“所有 spec 都是 fat aggregate”写死到 generic 实现里。

#### O3. 保留“源码可见时可追加全单态化”的后续优化口子

- 当前先把共享主体路径补齐。
- 等 correctness 完成后，再决定是否在“源码可见 / 同包 / 热点场景”增加全单态化发码。
- 该优化只能是附加优化，不能破坏共享主体 ABI。

#### O4. 用真实标准库场景驱动 smoke，而不是只靠最小盒子样例

- `Box<T>` 只适合验证存取语义。
- 当前已新增 `MiniMap<K: Hashable, V>` smoke 作为 Map/Hashable 级真实场景准入；后续新增 `List<T>`、受约束算法函数等能力时，也应按真实场景继续补 smoke。

---

### G7 测试与验证

| 编号 | 任务 |
| --- | --- |
| G7-1 | Parser 测试：全量覆盖 `docs/specifications/feng-generics-draft.md` 中正确语法 1-9 和错误语法 1-12 |
| G7-2 | 语义分析测试：泛型声明、重载、推导、object-form / callable-form 约束访问、invariance 各场景；union-form 随 union 阶段补齐 |
| G7-3 | 符号表测试：泛型声明导出 / 跨包读取后语义等价验证 |
| G7-4 | 代码生成 smoke：基础 `Box<T>`、`MyType<T, V>`、任意多个类型参数、泛型类型上的泛型方法、generic type shared body 内部 self-call |
| G7-5 | 类型覆盖 smoke：内建标量、`string`、数组、`UserType`、object-form `spec`、callable-form `spec`、aggregate 返回 |
| G7-6 | 契约 smoke：`Map<K: Hashable, V>`、受约束算法函数、object-form / callable-form 约束场景；Map/Hashable 基础 smoke 已补，union-form 随 union 阶段补齐 |
| G7-7 | 全量回归测试：确保既有非泛型功能无回归 |

**G7 当前补齐原则**：

1. 不再把“`Box<T>` 的基础 smoke 通过”当作“泛型完整”的证明。
2. 每补齐一种类型类别，就必须补对应 smoke：
   - 标量值类型
   - 托管指针类型
   - 聚合值类型
3. 每补齐一种契约能力，就必须补对应 smoke：
   - 无约束
   - `T: Spec`
   - 泛型 spec 实例约束
4. `Map<K: Hashable, V>` 必须成为泛型完成判定的准入用例，而不是完成之后才顺手补的示例。
5. 本轮“不含 union 的泛型完整”必须覆盖 object-form / callable-form `spec`、所有当前可用内建类型、任意多个类型参数、值类型零堆分配与 Map/Hashable 级真实场景；union-form 另随 union 语法阶段验收。

---

## 三、任务依赖关系

```text
G0-1(代码生成策略) ─────────────────────────────────────────────── G6
G0-2(泛型约束是否可写泛型实例) ─────────────────────────────────── G4-4
G0-3(默认零值规则) ──────────────────────────────────────────────── G4-19
G0-4(>>歧义处理) ────────────────────────────────────────────────── G1-2 → G3-3

Phase 5.5(符号表重构) → G5-1..G5-8

G1-1(:<确认) → G3-5(显式泛型调用解析)

G2-1(TypeParam结构) → G2-2 → G2-3 → G2-4 → G3 全部 → G4 全部

G4-1(类型参数作用域) → G4-2 → G4-3 → G4-8, G4-9, G4-15
G4-5(声明 identity) → G4-7 → G4-10, G4-11, G4-12, G4-13
G4-16(泛型 fit 左侧) → G4-17
```

---

## 四、补充说明

### 关于实现顺序建议

推荐按以下顺序推进，每步都要回归测试：

1. Phase 5.5（符号表重构，已在 feng-plan.md）
2. G0 全部决策（无编码，只收口规范）
3. G2 AST 扩展（不跑测试，只修改结构定义）
4. G1 + G3（词法和 Parser 扩展）+ G3-9（Parser 测试）
5. G4-1 ~ G4-14（语义分析主体）+ G4-20（语义测试）
6. G4-15 ~ G4-18（父 spec 约束、fit、终结器）
7. G5（符号表导出）+ G5-8（符号表测试）
8. **当前补齐阶段 A**：完成 callable value codegen 表示与 callable-form `spec` / generic `spec` 的统一契约 lowering（G4 收口 + G6-3 / G6-4 / G6-5）
9. **当前补齐阶段 B（当前既有形态已完成）**：object-form `spec` 聚合值 generic arg 已闭环；future tuple / value-struct / union carrier 随对应语言形态落地后补接入（G6-1）
10. **当前补齐阶段 C（已完成基础路径）**：完成泛型类型上的泛型方法（G6-6）
11. **当前补齐阶段 D（不含 union 口径已形成完成证据）**：补齐 `Map<K: Hashable, V>` 等真实 smoke 与回归（G7-4 / G7-5 / G7-6 / G7-7）
12. 泛型完整后，再恢复 fit 值类型工作

### 关于当前代码库基础

泛型的 Parser / AST / 符号表基础已经不是空白状态；当前主要缺口已经从“语法/结构缺失”转移到“语义闭环 + codegen 类型覆盖 + 约束 lowering”。

当前已知基础事实：

- 当前代码实现里已经有以 `FengGenericValueDescriptor` 为核心的共享主体 ABI；完整态将其收口为 `FengGenericParamDescriptor`（重命名并增加 `witness`）。
- `Box<T>` 的基础 smoke 已通过，证明“外层类型参数参与参数/返回/字段读写”这条最小路径可工作。
- 语义 value-kind 已经给出一条可用总分类：标量 builtin 是 trivial，`UserType` 与 callable-form `spec` 是 managed pointer，object-form `spec` 是 aggregate。
- 当前 Map/Hashable 级标准库目标场景已经形成基础回归证据；union-form `spec` 与 future tuple / value-struct 聚合值接入暂缓到对应语法/值模型阶段。

因此，当前阶段不再从 G2/G3 重新起步，而是直接针对 G4/G6/G7 的剩余缺口收口。
