# spec 语义层优化方案草案

> 本文档是实现草案，不是语言权威规范。
> 语言语义以 [docs/feng-spec.md](../docs/feng-spec.md)、[docs/feng-fit.md](../docs/feng-fit.md)、[docs/feng-function.md](../docs/feng-function.md) 为准。
> 本草案只讨论 `spec` 在 semantic 层应当稳定产出哪些结论，**不**讨论运行时值表示选型（box / fat value / 其他），也**不**讨论 codegen lowering 细节。

## 1 目标

把 `spec` 相关的"语言规则推导"完全收敛在 semantic 层，使其具备以下性质：

- **决策一次**：可见 `fit`、满足关系、对象形状/可调用形状的适配路径、默认 witness 决策，全部在 semantic 阶段完成并定型。
- **结论可消费**：codegen 不需要重新理解 `fit` 可见性、`spec` 父子闭包、签名匹配规则；它只读取 semantic 已经选定的结果。
- **与值表示无关**：无论后续 `spec` 的运行时值表示是 box、fat value 还是其他形式，本草案产出的语义结论都不变；仅 codegen 在 lowering 时如何消费这些结论会变。

## 2 非目标

本草案明确不处理：

- `spec` 的运行时值表示（独立草案：[feng-spec-codegen-draft.md](./feng-spec-codegen-draft.md) 与 [feng-value-model-draft.md](./feng-value-model-draft.md)）。
- codegen 如何发码（witness thunk、coercion helper、closure env 布局等）。
- 运行时 ARC / cycle collector / 默认零值的具体实现路径。
- 未来去虚化 / 内联缓存 / 单态化等优化。
- 修改 `docs/` 下的语言权威规范；本草案在权威规范现有规则之内补强 semantic 实现。

## 3 与其他草案的关系

| 草案 | 关心的问题 |
| --- | --- |
| 本草案 | semantic 层应稳定产出哪些 `spec` 相关结论 |
| [feng-spec-codegen-draft.md](./feng-spec-codegen-draft.md) | `spec` 值如何在 box 模型下落地（含 codegen / runtime） |
| [feng-value-model-draft.md](./feng-value-model-draft.md) | 三类值模型与"按值聚合 + 托管槽位"未来抽象 |

本草案与后两者解耦：本草案产出的结论是后两者的输入，但本草案不依赖后两者的任何选型。

## 4 当前 semantic 层的事实清单

以下是 [src/semantic/analyzer.c](../src/semantic/analyzer.c) 当前已实现的能力，本草案不重复不否定：

- 区分 object-form / callable-form `spec`（`FengSpecDeclForm`、`decl_is_function_type`）。
- `type_decl_satisfies_spec_decl`：基于声明头 + 可见 `fit` + 父 `spec` 闭包做名义满足判定。
- `spec_collect_closure`：收集 `spec` 的传递父闭包。
- `find_spec_object_member`：在闭包内按名查找 object-form 成员。
- `fit_decl_is_visible_from`：跨模块 `pu fit` + `use` 可见性判定。
- `detect_cross_spec_method_conflicts`：多 `spec` 同名同参不同返回的冲突检测。
- `expr_type_assignable_to_type_ref`：把"具体 type → object-form spec"的赋值/传参/返回路径接入名义满足判定。
- `lambda_expr_matches_function_type`：lambda → callable-form `spec` 的形状匹配。
- 重载重叠判定（`param_type_refs_potentially_overlap`）使用 `type_decl_satisfies_spec_decl` 处理 spec 参数。
- `fit` 块内私有成员访问拦截（`fit_body_blocks_private_access`）。

## 5 当前 semantic 层的差距清单

差距并非语义判定错误，而是"判定的结果没有被以稳定形式记录下来供后续阶段消费"。

### 5.1 spec coercion 缺少稳定的"路径选择"结论

`expr_type_assignable_to_type_ref` 仅返回 `bool`。对于 `具体 type T → object-form spec S` 这一次 coercion，semantic 当前没有记录：

- 这次 coercion 走的是哪条可见满足关系（`type` 声明头、传递父 `spec`、还是某个具体的 `fit`）。
- 当满足关系来自 `fit` 时，是哪一个 `fit` 声明（多个可见 `fit` 提供同一关系时需要明确选择规则）。
- 是 object-form 适配还是 callable-form 适配（语法上明显，但调用点目前没有显式标注）。

后果：codegen 如果未来需要决定"绑定哪一组方法实现 / 字段访问入口"，必须重新实现一遍同样的可见性遍历与冲突判定。

### 5.2 spec 成员访问缺少稳定的"成员实现绑定"结论

`find_spec_object_member` 只能定位 `spec` 上的成员**签名**。当某个表达式 `obj.method()` 其中 `obj` 静态类型为 spec `S`、运行时具体类型为 `T`（在赋值点已确定），semantic 没有记录：

- 此次 spec 成员访问对应的最终实现归属（来自 `T` 自身的方法、还是某个可见 `fit` 提供的方法）。
- 当成员实现来自 `fit` 时，是哪一个 `fit` 声明。
- 字段成员是 `let` 还是 `var`（影响可写性；目前可由签名读出，但调用点未沉淀）。

注意：因为 spec 类型在调用点本身就是多态的，"最终实现绑定"在通用情况下是**每个具体 (T, S) 组合**才能给出，而不是在调用点直接给出单一答案。语义层应稳定产出的更应是：

- 调用点：成员签名（来自 `S` 闭包的哪一项）。
- 每对 `(T, S)`：witness 解析结果（成员名 → 最终实现来源）。

### 5.3 缺少 (T, S) 级"witness resolution"结果表

针对每对在分析中实际成立的 `(具体 type T, object-form spec S)` 满足关系，semantic 没有产出"按 S 的成员闭包顺序，每个成员对应 T 上哪一段实现"的稳定结果。

后果：

- codegen 要发 witness 表时，需自行重做"成员闭包枚举 + 实现归属判定"。
- 多 `fit` 同时提供同名实现时，冲突应在 semantic 层一次性判定与报错；目前对单个 `fit` 闭包冲突已检测，但跨 "声明头 + fit" 的成员归属冲突是否充分覆盖需要专项审计。

### 5.4 spec 默认零值绑定点未标注

规范要求：无初始值的 `spec` 绑定 / 字段 / 返回点必须使用默认 witness（[docs/feng-spec.md](../docs/feng-spec.md) §7）。semantic 当前对这些点应该都允许通过（不报"missing initializer"），但是否**显式标注**了"此处需要默认 witness"这一事实，目前不明确。

后果：codegen 难以在不重做规则推导的前提下区分"显式 `null`/默认零值"与"spec 默认 witness"两条不同 lowering 路径。

### 5.5 callable-form spec 的"目标签名匹配结论"未沉淀

lambda / 函数值 / 方法值 → callable-form `spec` 的匹配在 `lambda_expr_matches_function_type` 中即时判定。该结果未与表达式节点稳定关联：

- 是哪一类源：顶层函数、方法值、lambda。
- 当源是方法值（`obj.method`）时，绑定的接收者表达式是哪一个。
- 当源是 lambda 时，签名是否做过隐式参数类型补全。

### 5.6 spec 等值比较的语义路径未明确

[docs/feng-spec.md](../docs/feng-spec.md) §7 规定 spec 值默认按引用身份比较。当 spec 值参与 `==` / `!=` 时，semantic 应当：

- 将其归类为引用身份比较（与 string / array 的值语义比较区分开）。
- 若未来运行时表示发生变化（box → fat value），semantic 这条结论本身不变，仅 codegen 改变 lowering。

当前是否在二元运算解析阶段对 spec 值显式归类，需要审计。

## 6 应稳定产出的语义结论（最终目标）

为达成"决策一次 / 结论可消费 / 与值表示无关"，semantic 应能向后续阶段提供以下结果。**结果挂载形式（AST 注记字段、旁路表、独立资源）由实现阶段再定**，本草案只规定结论本身。

### 6.1 SpecRelation：满足关系定型

对每条在分析中实际成立的"具体 type T 满足 object-form spec S"关系，记录：

- `type_decl`、`spec_decl`。
- 关系来源链：每条具体来源 `{ kind, source_decl }`，其中 `kind ∈ { 声明头, 父 spec 传递, 可见 fit }`。
- 当来源为 `fit` 时，记录该 `fit` 所在模块与可见性判定结果。

该结构是后续所有 spec 相关结论的基础。

### 6.2 SpecCoercionSite：每个 coercion 点的选型结论

对每个"具体表达式 → object-form spec 类型位置"的 coercion 点（赋值、传参、返回、字段初始化），记录：

- 源表达式的具体 type。
- 目标 spec。
- 选定的 `SpecRelation`（如果存在多条等价可见关系，需有稳定优先级规则；本草案要求实现阶段把规则写出来）。

对每个"可调用值 → callable-form spec 类型位置"的 coercion 点，记录：

- 源类别：顶层函数 / 方法值 / lambda。
- 目标 callable-form spec。
- 签名匹配后的最终参数类型与返回类型。

### 6.3 SpecDefaultBinding：默认 witness 绑定点

对每处需要 spec 默认 witness 的位置（无初始值绑定、未指定默认值的字段、`return` 缺省等），记录：

- 目标 spec。
- 是 object-form 还是 callable-form。
- 处于哪一种语法位置（绑定 / 字段默认 / 缺省返回 / 数组元素默认）。

该结论使 codegen 可以在不重做"哪些位置需要默认零值"判断的情况下定位需要发"默认 witness"的发码点。

### 6.4 SpecMemberAccess：spec 成员访问签名结论

对每个 `obj.member` 表达式（其中 `obj` 静态类型为 object-form spec `S`），记录：

- 目标 spec `S`。
- 命中的成员（`FengTypeMember`，来自 `S` 闭包的哪一项）。
- 是字段读 / 字段写 / 方法调用。
- 字段成员：是 `let` 还是 `var`（影响左值合法性）。

> 该结论在调用点是**签名级**结论；具体实现绑定属于 §6.5 的 (T, S) 级结论。

### 6.5 SpecWitness：(T, S) 级 witness 解析结果

对每个在分析中实际成立的 `(T, S)` 满足关系，按 `S` 的成员闭包顺序，逐成员产出：

- 成员名与签名（来自 `S` 闭包）。
- 该成员对应 `T` 一侧的实现归属：
  - `kind ∈ { type 自身字段, type 自身方法, fit 块方法 }`。
  - 来源 decl / member 引用。
  - 当来源为 `fit` 时，记录 `fit` 声明。
- 解析过程中检测到的冲突（多个 `fit` 提供同名实现 / 声明头与 `fit` 同时提供同名实现且签名不一致）应在此处一次性报错。

该结论让"哪张 witness 表怎么填"成为查表问题。

### 6.6 SpecEquality：等值比较归类

对每个 `==` / `!=` 表达式，若任一操作数静态类型为 spec，记录其比较语义为"引用身份比较"。该结论与 string / array 等值语义解耦。

## 7 规则收敛点（必须在 semantic 一次性判定）

以下规则不允许遗留到 codegen / runtime：

- `spec` 闭包内成员同名签名冲突。
- 多 `spec` 同名同参不同返回冲突（已实现；归入新结构后保持）。
- 同一 `(T, S)` 组合下，声明头 / 传递父 `spec` / 可见 `fit` 三方提供同名成员且签名不一致的冲突。
- 同一 `(T, S)` 下多个可见 `fit` 都提供同名同签名实现的歧义判定（见 §8）。
- `fit` 跨模块可见性（`pu fit` + `use`）。
- callable-form `spec` 不可标 `@union`、对象形状 `spec` 不可标 `@fixed` / `@union` / 调用方式注解。

## 8 已决策的规则点

以下决策已与用户确认，作为后续实现的硬约束。

### 8.1 可见面冲突判定规则（原 Q1）

判定原则：**冲突按当前编译位置的可见面合并判定，不可见的声明不参与**。

具体规则：

- 对任何 `(T, S)` 对，当前位置可见的全部成员实现来源（`type` 内联声明 + 当前可见的全部 `fit`）合并为一个集合，按"同名同参数"规则做冲突判定：
  - 同名同参数但返回类型不一致 → 冲突报错。
  - 同名同参数同返回但实现归属不同（`type` 自身一份 + `fit` 一份，或两个 `fit` 各一份）→ 冲突报错。
- 不在当前编译位置可见的 `fit`（未通过 `pu fit` 导出，或未被 `use` 引入）**不进入可见面**，因此即便其与其他实现潜在冲突，**当前位置不报错**。
- `type` 内联声明本身的内部冲突（同名同参数的方法重载冲突）依然由现有 `type` 内重载冲突路径处理。

实现意义：所有 (T, S) 级冲突在 §6.5 `SpecWitness` 的解析过程中按可见面集合统一报错，不再分散到 `fit` 单独检查路径。

### 8.2 SpecWitness 产出粒度（原 Q2）

采用**按需产出**：仅对程序中实际发生过 coercion 的 `(T, S)` 对生成 `SpecWitness`。

补充约束：

- 同一 `(T, S)` 在程序中多处 coercion 时，复用同一份 `SpecWitness` 结果（首次需要时生成并缓存），保证多处结论一致。
- 歧义/冲突判定规则（§8.1）与产出粒度无关；按需产出只影响**报错时机**，不影响**报错规则**。即一个未被 coercion 用到的潜在冲突 `(T, S)` 不会触发报错，但一旦被 coercion，按 §8.1 立即报错。
- 如未来需要"库作者提前暴露契约冲突"，可作为独立的完整性检查开关补充，不影响本阶段主流程。

### 8.3 结论挂载形式（原 Q3）

采用**混合方案**：

- **站点级结论**（与某个具体 AST 节点强相关）挂在 AST 节点上：
  - `SpecCoercionSite`（§6.2）→ 挂在产生 coercion 的表达式节点。
  - `SpecDefaultBinding`（§6.3）→ 挂在需要默认 witness 的绑定 / 字段 / 返回点。
  - `SpecMemberAccess`（§6.4）→ 挂在 `obj.member` 表达式节点。
  - `SpecEquality`（§6.6）→ 挂在 `==` / `!=` 表达式节点。
- **关系级结论**（与具体 AST 节点无强绑定，按 (T, S) 索引）放入 `FengSemanticAnalysis` 旁路表：
  - `SpecRelation`（§6.1）。
  - `SpecWitness`（§6.5）。

站点级结论引用关系级结论的指针/句柄，避免重复存储。

### 8.4 callable-form spec 不进入关系表（原 Q4）

callable-form spec 的满足判定是结构化的（签名匹配，无可见性来源），**不进入** §6.1 `SpecRelation` 表。

仅在 §6.2 `SpecCoercionSite` 上沉淀本次签名匹配结论（源类别 + 匹配后的参数 / 返回类型）。

## 9 测试面（语义级）

所有测试均为 `.ff` 输入 + 期望错误集 / 通过状态，**不依赖 codegen / runtime**。

### 9.1 关系定型

- T 在声明头满足 S，可见关系命中声明头。
- T 在 `fit` 中满足 S，可见关系命中该 `fit`。
- T 通过传递 `spec S2: S` + `T: S2` 满足 S，可见关系命中传递路径。
- T 同时通过声明头与 `fit` 满足 S，按 §8 Q1 决策给出预期。
- 跨模块 `pu fit` + `use` 命中；缺 `use` 时不命中。

### 9.2 coercion 站点

- 赋值 / 传参 / 返回 / 字段初始化 / 数组元素初始化的 spec 站点全部命中并落到正确关系。
- callable-form spec 接收顶层函数 / 方法值 / lambda，签名匹配结论稳定。

### 9.3 默认 witness 标注

- `let s: S;` 标注为默认 witness 站点，不报"missing initializer"。
- 字段类型为 spec 而未指定默认值时，标注默认 witness 站点。
- callable-form spec 同上。

### 9.4 成员访问

- spec-typed 表达式访问字段（`let` 与 `var` 区分）。
- spec-typed 表达式调用方法。
- callable-form spec 上访问成员应报"无成员"错误。

### 9.5 (T, S) witness 解析

- T 仅靠声明头满足 S：每个成员命中 T 自身实现。
- T 通过 `fit` 满足 S：每个成员命中 `fit` 实现；其他成员（若 `fit` 不全）由 T 自身补齐或报错。
- 多 `fit` 冲突按 §8 Q1 决策。

### 9.6 等值归类

- spec 类型变量的 `==` / `!=` 表达式归类为引用身份比较，不触发字符串/数组的值比较路径。

### 9.7 已有用例的回归

将 [test/semantic/](../test/semantic/) 中现有 spec/fit 用例并入新结构后跑全量回归。

## 10 分阶段建议

### Phase S1 — 关系层基础
- 落地 §6.1 `SpecRelation`，把 `type_decl_satisfies_spec_decl` 的内部状态显式化。
- 落地 §6.2 在 coercion 站点选定关系（object-form 优先；callable-form 仅落签名匹配）。
- 补充 §9.1 / §9.2 测试。

### Phase S2 — 默认零值与成员访问
- 落地 §6.3 默认 witness 站点标注。
- 落地 §6.4 spec 成员访问签名结论。
- 补充 §9.3 / §9.4 测试。

### Phase S3 — witness 解析与冲突收敛
- 落地 §6.5 (T, S) witness 解析结果。
- 把 §7 中 (T, S) 级冲突全部归入此阶段统一报错。
- 补充 §9.5 测试。

### Phase S4 — 等值归类与全量回归
- 落地 §6.6。
- 补充 §9.6 测试。
- 跑 §9.7 全量回归。

每个阶段完成后必须能在不依赖 codegen 改动的前提下独立通过。

## 11 验收标准

实现完成时，应满足：

- 上述六类语义结论均可被读取（具体 API/结构由实现阶段决定）。
- §7 列出的冲突全部在 semantic 层一次性判定与报错，不下沉到后续阶段。
- 现有 `test/semantic/` 中 spec/fit 用例零回归。
- 新增结论的产出与 `spec` 运行时值表示选型完全无关，切换值表示不需要回头改 semantic。
