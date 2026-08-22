# Feng intersection 约束类型参数静态方法直接调用修复

> **状态**：已完成，可独立交付。
>
> **关联任务**：
> [`feng-object-form-spec-method-value-dev.md`](./feng-object-form-spec-method-value-dev.md)
> 的 `IC01`。
>
> **文档定位**：本文记录 IC01 的问题、根因、修复边界、实施步骤、验证门槛和实施过程
> 问题，不是语言权威规范。正式语义由 `feng-spec.md` 唯一定义，泛型、可见性和诊断
> 规范只引用或约束各自负责的部分。

## 1. 问题

以下直接调用按 intersection-form `spec` 的合并成员语义应当合法：

```feng
open spec Factory {
  static func create(seed: int): string;
}

open spec TaggedFactory {
  static func tag(): string;
}

open spec CombinedFactory: Factory & TaggedFactory;

func direct<T: CombinedFactory>(seed: int): string {
  return T.create(seed);
}
```

`CombinedFactory` 已在编译期展平为 `Factory` 与 `TaggedFactory` 的合并 requirement
成员面，闭合 `T` 的 descriptor 也已经携带对应 merged witness；但当前 Semantic 对
`T.create(seed)` 报告 `AE0512`。

该缺口只涉及直接静态方法调用。`T: CombinedFactory` 的实例方法直接调用已经合法；
`T.create` 静态方法值属于后续 `MV07`，不在 IC01 提前开放。

## 2. 正确语义

### 2.1 候选成员面

对 `T: IntersectionSpec` 的 `T.method(args)`：

1. 从完整约束实例出发，递归经过 intersection member 边和 object parent 边；
2. 每经过一条泛型边，都按当前 spec 实例替换 owner 类型参数；
3. 只收集原声明 object-form `spec` 上同名的静态普通方法；
4. 在 requirement 原声明 spec 上检查 `seal` / `@friend` 访问权限，不可访问候选不参与
   重载；
5. 按替换后的参数、变长参数形态、显式方法类型实参和既有重载优先级选择唯一候选；
6. 语义等价的重复 requirement 合并为一个候选；同名但参数签名不同的合法 requirement
   继续作为重载候选；
7. 结果必须保留选中 `FengTypeMember` 的声明身份、原声明 object-form `spec` 及其精确
   实例，Codegen 不得按名称重新选择来源。

object-form 和 intersection-form 约束必须共用同一静态 spec requirement 解析规则；两者
只在成员面的组成方式和最终 witness 视角上不同。

### 2.2 诊断

- 多个非等价候选以相同优先级匹配：`AE0511`；
- 存在同名可访问静态方法，但没有可接受实参的重载：`AE0512`；
- 不存在同名静态 requirement：保持现有约束成员缺失诊断 `AE0512`；
- 只有本来匹配但当前上下文不可访问的 `seal` requirement：`AE0708`；
- 预打包变长参数等调用形态错误：继续使用对应既有诊断，不降级为 `AE0512`。

不可访问候选不能遮蔽同名 open 候选。诊断选择只使用编译期候选事实，不增加运行时
访问检查。

### 2.3 运行时行为

Semantic 选中 requirement 后，调用使用闭合 `T` descriptor 中已有的 witness：

- object-form 约束进入其普通 witness 槽；
- intersection-form 约束进入 merged witness 中与该 requirement 对应的既有槽；
- 静态 requirement 不传递 receiver 或 subject；
- 不执行运行时成员查找、满足关系查询、签名比较或候选回退。

因此 IC01 不增加运行时指令、分配、descriptor 字段或间接层级，也不修改 runtime ABI、
生成程序 ABI 或 `.ft` 格式。

## 3. 已确认根因

`validate_function_call_expr()` 已能把 `T` 解析为“泛型类型参数 + 完整 spec 约束实例”，
但其静态 requirement 分支调用 `find_accessible_spec_object_member()`。该查询明确只接受
object-form `spec`，所以 intersection-form 约束在参数匹配和重载选择之前就返回空，最终
报告 `AE0512`。

同时，旧分支通过 `constraint_member_declaring_spec_instance()` 只投影 object parent；该
函数不能表达 intersection member 边。因此，即使仅放宽 form 判断，也无法为嵌套或泛型
intersection 保留 requirement 原声明的精确实例。

仓库已经具备以下通用基础：

- spec 方法值解析按 intersection member 和 object parent 两类边递归，并在每条边上完成
  owner 类型实参替换；
- intersection Codegen 已把语义等价 requirement 归并为 merged witness 的同一槽，并把
  合法重载保留为独立槽；
- 泛型参数方法调用 Codegen 已优先使用 Semantic 记录的 `FengTypeMember` 身份查找普通或
  merged witness 槽。

缺失的是供直接调用使用的、基于同一精确 spec surface 遍历的静态重载解析，而不是新的
witness 或运行时表示。

## 4. 修复方案

### 4.1 Semantic

1. 提取无分配的通用 spec 方法 surface 遍历器，统一递归 intersection member 与 object
   parent，并向消费者提供 requirement、原声明 spec 和精确声明者实例。
2. 让现有 target-typed spec 方法值解析复用该遍历器，保持既有选择和诊断行为。
3. 增加通用 spec 静态调用重载 resolver；object-form 与 intersection-form 约束共同使用，
   不在 `validate_function_call_expr()` 中按 intersection 名称或 AST 形状临时搜索。
4. resolver 在访问过滤后执行既有参数匹配、精确匹配优先级、变长参数拒绝和歧义处理；
   对等价 requirement 做编译期去重。
5. `validate_function_call_expr()` 只消费 resolver 的稳定结果并记录既有
   `FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD` sidecar。

### 4.2 Codegen

优先验证现有路径能直接消费新的 Semantic 结果：泛型参数仍由其完整 intersection 约束
取得 merged `UserSpec`，并通过 `FengTypeMember` 身份选择槽。只有验证发现通用身份消费
缺口时，才补齐同一编译期抽象；不得增加 intersection 专用运行时分支或按名称回退。

### 4.3 强制边界

- 不引入任何增量运行时开销；
- 不修改或删除任何既有测试用例，只新增覆盖及注册入口；
- 不变更 runtime 私有 ABI、生成程序 ABI 或 `.ft` schema / 版本；
- 不提前实现 `MV07`；
- 不按 spec 名、方法名、包名或测试模型增加特判；
- 如正确实现必须突破任一边界，立即在第 7 节记录并停下交由人工决策。

## 5. 用例与交付拆解

### 5.1 Semantic

- [x] 直接 intersection 成员静态调用；
- [x] nested intersection 与 object parent 静态调用；
- [x] 泛型 intersection owner 参数的固定、传递和重排映射；
- [x] 分布在不同 member spec 的合法静态重载；
- [x] 语义等价重复 requirement 不产生伪歧义；
- [x] 同优先级真实歧义报告 `AE0511`；
- [x] 参数不匹配与不存在成员报告 `AE0512`；
- [x] 非授权 `seal` 报告 `AE0708`，授权实现上下文调用通过；
- [x] object-form 约束静态调用回归保持通过。

### 5.2 Codegen 与 FCTS

- [x] 本地不同闭合 `T` 通过各自 merged witness 静态实现返回不同结果；
- [x] 合法重载分别进入对应 merged witness 槽；
- [x] nested / parent requirement 使用精确原声明槽；
- [x] 共享泛型函数体调用通过；
- [x] 跨包 provider 声明约束与泛型函数、consumer 提供闭合类型并调用；
- [x] 生成代码不出现 receiver/subject、运行时成员查找或新增分配；
- [x] object-form 约束静态调用的生成路径与成本不变。

### 5.3 回归与交付

- [x] 更新权威规范；
- [x] 完成 Semantic / Codegen 实现；
- [x] Semantic、Codegen、Symbol（如涉及）专项测试通过；
- [x] FCTS 全量通过；
- [x] 在沙箱外执行完整 `make test`；
- [x] 审计无既有用例变更、无 ABI / `.ft` 变更、无增量运行时开销；
- [x] 更新关联任务文档并标记 IC01 可独立交付。

## 6. 实施步骤

- [x] 复现并定位 object-form 专用查询导致的 `AE0512` 根因；
- [x] 更新 `spec`、泛型、可见性和 AE 诊断主规范；
- [x] 提取通用 spec 方法 surface 遍历；
- [x] 实现并接入静态 constraint method call resolver；
- [x] 验证并在必要时补齐 Codegen 的 merged witness 身份消费；
- [x] 新增 Compiler tests；
- [x] 新增 FCTS 本地、共享泛型与跨包覆盖；
- [x] 执行专项测试和全量回归；
- [x] 完成性能、ABI、格式和既有用例审计；
- [x] 补充独立交付记录。

## 7. 实施过程问题记录

实施中发现的问题必须先记录最小复现、实际结果、期望结果和影响，再分析根因并决定
是否可在已批准的通用方案内修复。不确定或触及第 4.3 节强制边界时停止实施，由人工
决策。

### ISSUE-001：object-form 约束静态调用只检查首个同名重载

- **状态**：已解决。
- **最小复现**：

  ```feng
  spec Factory {
    static func select(value: int): int;
    static func select(value: string): string;
  }

  func selectText<T: Factory>(value: string): string {
    return T.select(value);
  }
  ```

- **实际结果**：Semantic 只取得首个 `select(int)`，随后报告
  `AE0512: static method 'T.select' has no overload accepting 1 argument(s)`；合法的
  `select(string)` 没有进入候选集。
- **期望结果**：两个同名静态 requirement 都进入既有重载选择，调用唯一选择
  `select(string)`。
- **根因**：IC01 根因中的 `find_accessible_spec_object_member()` 不仅限定 object-form，
  还只返回首个同名可访问成员；旧直接调用分支随后只对该单一成员执行参数匹配。这与
  `feng-spec.md` 已定义的 spec 静态方法重载语义不一致。
- **通用修复**：object-form 与 intersection-form 共同切换到第 4.1 节的静态 spec
  requirement 重载 resolver；不在 object-form 旧分支补第二套循环。该 resolver 访问完整
  精确 surface 并复用既有重载优先级、变长参数和歧义规则。
- **范围判断**：这是 IC01 要求建立通用静态 constraint member resolver 时必须消除的同根
  单候选限制，不是独立语言扩展，也不需要特判。
- **运行时 / ABI / `.ft` 影响**：无；只修正编译期候选收集与选择。
- **是否需要人工决策**：否；修复方案已经包含在 IC01 批准的 object/intersection 共用
  resolver 中，且不触及强制停止边界。
- **验证结果**：object-form 第二个静态重载与 intersection-form 分布式重载均唯一选择
  正确 requirement；Semantic、Codegen 和 FCTS 通过。

### ISSUE-002：spec-seal 既有回归的 `AE0708` 数量发生变化

- **状态**：已解决。
- **触发方式**：接入通用静态 requirement resolver 后运行既有
  `test_spec_seal_member_access_rejected_outside_implementation`。
- **实际结果**：独立静态方法探针在同一个 `T.value()` 位置报告两个完全相同的
  `AE0708`。
- **期望结果**：实例字段、实例方法和类型参数静态方法的每个非法访问点都只报告一个
  `AE0708`，不产生重复或降级诊断。
- **初步影响**：仅观察到编译期诊断数量变化；尚未发现合法程序行为、Codegen、运行时、
  ABI 或 `.ft` 影响。
- **根因**：`resolve_expr()` 先把调用 callee `T.value` 作为成员表达式校验；旧 object-form
  成员查询已经报告一次 `AE0708`。新的静态调用 resolver 随后又根据同一不可访问
  requirement 报告第二次。intersection-form 的成员表达式旧查询反而看不到该 surface，
  因此不能简单删除调用 resolver 的访问诊断。
- **通用修复**：让成员表达式阶段也通过同一精确 spec 方法 surface 对 object/intersection
  执行静态方法访问探测。只有不可访问候选时由该阶段统一报告一次；若同时存在可访问
  同名候选但其签名不匹配、另一个不可访问候选本来匹配，则调用 resolver 负责报告
  `AE0708`。这样保持“先访问过滤、后重载”的规则，并消除重复诊断。
- **运行时 / ABI / `.ft` 影响**：无；纯编译期访问诊断收敛。
- **是否需要人工决策**：否；属于通用 resolver 接入后的诊断去重，不修改语义或强制边界。
- **验证结果**：非授权静态 requirement 每个访问点只报告一次 `AE0708`；授权实现上下文
  继续通过，既有 seal 回归通过。

### ISSUE-003：已解析的 object-form 第二重载返回类型仍按首个成员推断

- **状态**：已解决。
- **最小复现**：ISSUE-001 的 `select(int)` / `select(string)` 示例经通用 resolver 已唯一
  选择 `select(string)`。
- **实际结果**：调用选择不再报告 `AE0512`，但返回表达式随后报告
  `AE1003: expression 'T.select' does not match expected type 'string'`。同一探针中的
  intersection 直接、嵌套、父 requirement、重载和等价 requirement 调用均已通过。
- **期望结果**：调用表达式的返回类型来自已经记录的唯一 `FengResolvedCallable`，即
  `select(string): string`，不得重新按名称取得首个 `select(int)`。
- **初步影响**：纯 Semantic 返回类型事实不一致；尚未进入 Codegen，无运行时、ABI 或
  `.ft` 影响。
- **根因**：`infer_call_expr_type()` 的泛型约束静态方法分支没有读取
  `expr->as.call.resolved_callable`，而是再次调用只返回首个同名成员的
  `find_spec_object_member_with_owner()`。因此调用验证与返回类型推断使用了两个不同来源。
- **通用修复**：该分支在已经存在 `FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD` 时直接消费
  其 callable、原声明 spec 和精确 owner instance；在尚未完成调用解析的推断探测阶段，
  调用同一个 spec 静态 requirement 重载 resolver，不保留按名称首成员回退。
- **运行时 / ABI / `.ft` 影响**：无；只统一编译期返回类型来源。
- **是否需要人工决策**：否；属于 IC01 “Codegen/后续阶段消费 Semantic 已解析事实”的
  同一约束，不增加特殊处理或强制边界影响。
- **验证结果**：返回推断与调用验证共同消费同一已解析 callable，object-form 与
  intersection-form 的非首个重载均通过。

### ISSUE-004：新增 seal 诊断夹具误用关键字作为 module 段

- **状态**：已解决。
- **实际结果**：新增 Semantic 夹具使用
  `module demo.intersection_static_call.seal;`，Parser 在 `seal` 处报告“qualified name 的
  `.` 后需要标识符”，测试尚未进入 IC01 Semantic 路径。
- **根因**：`seal` 是 Feng 关键字，不能作为未转义 module 名称段。
- **修复**：仅把该新增夹具的 module 段改为 `denied_seal`；测试语义与覆盖目标不变。
- **运行时 / ABI / `.ft` 影响**：无。
- **是否需要人工决策**：否；属于本次新增夹具的语法修正，不修改任何既有用例。

### ISSUE-005：新增 Codegen 夹具未通过 Semantic 前置分析

- **状态**：已解决。
- **实际结果**：首条诊断为
  `type 'First' method 'inherited' signature does not match spec 'RootFactory'`。夹具关系是
  `NumberFactory: RootFactory<i32>`，`First: NumberFactory` 实现
  `static inherited(i32): i32`。
- **期望结果**：夹具先通过 Semantic，再验证 merged witness 静态调用发码。
- **期望结果**：满足检查沿父 spec 边把 `RootFactory<T>` 闭合为 `RootFactory<i32>`，实现
  签名精确匹配。
- **初步影响**：这是 IC01 父 requirement 端到端路径的 Semantic 前置缺口；若不修复，
  调用 resolver 虽能选择精确父 requirement，具体闭合类型却无法合法提供对应 witness。
- **根因**：type 与 fit 的 spec closure 满足验证只在“直接声明 spec 引用自身带类型实参”
  时调用 `instantiate_parent_spec_ref_for_instance()`。对非泛型
  `NumberFactory: RootFactory<i32>`，直接引用 `NumberFactory` 的实参数量为零，旧循环提前
  `continue`，因此父 `RootFactory` 以空实例验证，签名中的 `T` 没有替换为 `i32`。
- **通用修复**：type/fit 两条 closure 验证路径都允许任意 object-form 直接 spec 实例进入
  既有父投影函数；该函数本来就能对非泛型 child 保留固定父引用、对泛型 child 执行实参
  替换。直接 generic spec 的既有一次验证和去重逻辑保持不变。
- **测试要求**：新增 type 与 fit 各一个非泛型 child 固定 generic parent 的满足回归，并
  保留 IC01 nested merged witness 端到端用例。
- **运行时 / ABI / `.ft` 影响**：无；只补齐编译期父 spec 实例选择，使用既有 witness 与
  符号格式。
- **是否需要人工决策**：否；这是 IC01 父 requirement 端到端验证必需的通用父实例投影，
  复用既有抽象且不触及强制停止边界。
- **验证结果**：新增 type 与 fit 两条固定泛型父 spec 满足回归均通过；IC01 父 requirement
  的 Semantic、Codegen 与 FCTS 路径通过。

### ISSUE-006：诊断补丁误匹配既有 Codegen 用例

- **状态**：已解决。
- **实际结果**：为 ISSUE-005 增加诊断输出时，补丁上下文过宽，误把更早的既有
  `test_address_of_module_binding_uses_storage_slot_codegen` 断言改成 IC01 诊断包装。
- **根因**：补丁只使用了重复出现的 `feng_semantic_analyze()` 断言作为定位上下文，没有
  锚定 IC01 新函数名或其唯一局部变量。
- **修复**：立即恢复既有用例原文；重新以 IC01 的 `required_calls` 数组作为唯一上下文，
  只修改本次新增用例。最终差异审计必须确认既有用例内容没有变化。
- **运行时 / ABI / `.ft` 影响**：无；未进入产品代码。
- **是否需要人工决策**：否；属于实施过程中的补丁定位错误，已按强制规则恢复。

### ISSUE-007：merged witness 发码未取得叶 spec 的静态实现

- **状态**：已解决。
- **实际结果**：新增 IC01 Codegen 夹具通过 Semantic 后，Codegen 报告
  `type 'First' is missing an implementation for spec 'NumberFactory' member 'create'`。
  `First` 直接声明满足 `NumberFactory`、`TextFactory` 与 `Tagged`，而调用约束是嵌套
  intersection-form `Nested: Combined & Tagged`、`Combined: NumberFactory & TextFactory`。
- **期望结果**：merged witness 沿既有 relation/witness 事实取得每个叶 spec 的静态实现，
  `T.create()` 使用 `NumberFactory.create` 对应的已有静态 witness slot。
- **事实确认**：Semantic 已物化 `(First, NumberFactory)` 叶 witness，且 requirement 顺序与
  Codegen `UserSpec` 顺序一致；`inherited` 槽有实现，而 `create`、`select` 两个静态槽的
  `impl_member` 为 `NULL`。因此不是 merged-witness 元数据、关系声明或夹具不足。
- **根因**：`compute_spec_witness_if_absent()` 在没有显式 spec instance 的非泛型叶路径调用
  `type_find_matching_method()`；该通用候选函数在 type 与 extra-method 两个循环中都硬编码
  `!m->is_static`，没有按 requirement 的 `is_static` 属性选择。因此实例 requirement 正常，
  静态 requirement 必然漏选。
- **通用修复**：两个循环都改为要求
  `m->is_static == spec_member->is_static`，让同一候选函数同时正确处理实例与静态
  requirement；签名、可见性与 requirement 兼容性检查保持不变。
- **测试要求**：新增 intersection 约束闭合后静态叶 witness 的 Semantic 断言，并保留
  IC01 merged witness Codegen 端到端验证。
- **运行时 / ABI / `.ft` 影响**：无；只修正编译期 witness 实现选择，不改变 witness
  结构、槽位或运行时路径。
- **是否需要人工决策**：否；这是现有通用函数与其名称/用途不一致的明确实现错误，修复
  没有触发强制停止边界。
- **验证结果**：新增 Semantic 叶 witness 身份断言通过；merged witness Codegen 严格 C
  编译与本地、跨包 FCTS 真实执行均通过。

## 8. 独立交付记录

- **变更范围**：为 object-form 与 intersection-form 约束建立统一的静态 requirement
  surface 遍历和调用重载解析；保留原声明 member、原声明 object-form spec 与精确 owner
  instance。返回类型推断直接消费同一已解析 callable，访问探测复用同一 surface。
- **关联修复**：ISSUE-001 至 ISSUE-007 均已解决。其中固定泛型父 spec 满足检查同时覆盖
  type/fit 两条通用路径；叶 witness 候选选择按 requirement 的 static/instance 属性统一
  匹配。没有添加名称、包、AST 形状或测试模型特判。
- **Codegen 结果**：现有 `FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD`、merged witness 与
  requirement identity 路径可直接消费 Semantic 结果；只补齐编译期叶 witness 实现选择，
  未新增 Codegen 运行时表示。生成 C 验证静态槽调用不携带 receiver/subject，不出现
  `feng_object_new` 或运行时成员查找。
- **专项测试**：`build/bin/test_semantic` 与 `build/bin/test_codegen` 通过；新增覆盖直接、
  嵌套、父级、泛型映射、合法重载、等价 requirement、真实歧义、参数/成员缺失、seal、
  object-form 回归、type/fit 固定泛型父 spec、叶 witness 身份及严格生成 C 编译。
- **FCTS**：本地两个闭合 `T`、共享泛型体、父级/嵌套 requirement、分布式重载，以及跨包
  provider/consumer-only 类型均真实执行通过；结果为
  `853 passed, 0 failed, 0 skipped`。
- **全量回归**：沙箱外完整 `make test` 通过；UBSan 与普通
  `-O2 -Werror -pedantic` 两阶段均完成，两轮 smoke `91/91`、两轮 std `601/601`、两轮
  FCTS `853/853`，以及 compiler、runtime、CLI、Symbol、性能约束、增量构建和发布脚本测试
  全部通过。
- **运行时成本**：没有增量运行时开销。新增逻辑全部位于编译期；新支持调用仍直接进入
  闭合 descriptor 中已有的静态 witness 槽，不增加分支、分配、查找、descriptor 字段或
  间接层级。既有 object-form 静态调用和 intersection 实例调用路径保持原成本。
- **ABI 与格式**：未修改 runtime、runtime 私有 ABI、生成程序 ABI、公开结构、`.ft`
  schema、版本或兼容边界；跨包 FCTS 使用既有符号事实通过。
- **测试审计**：没有修改或删除任何既有测试用例；只新增 Compiler/FCTS 用例及其注册入口。
- **未解决问题**：IC01 无未解决问题；intersection 约束类型参数静态方法值仍属于独立
  后续分项 MV07。
- **建议 commit message**：`fix: support intersection-constrained static method calls`
