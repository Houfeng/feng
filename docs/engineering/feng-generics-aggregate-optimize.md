# Generic Aggregate 泛型实参优化方案

> 状态：已实现
> 日期：2026-05-22
> 关联规范：[docs/feng-type.md](../docs/feng-type.md)、[docs/feng-spec.md](../docs/feng-spec.md)、[docs/feng-expression.md](../docs/feng-expression.md)
> 关联设计：[dev/feng-value-model-delivered.md](./feng-value-model-delivered.md)、[dev/feng-runtime-generics-delivered.md](./feng-runtime-generics-delivered.md)、[docs/feng-union-type.md](../docs/feng-union-type.md)

## 1. 背景

本方案用于把 codegen 对“aggregate 类型作为泛型实参”的支持收敛为通用机制。

现状可以拆成两层：

- runtime 与值模型层已经按通用 aggregate 抽象设计完成：值统一落入 `trivial`、`managed-pointer`、`aggregate-with-managed-slots` 三分类；aggregate 生命周期统一走 `FengAggregateValueDescriptor` + `FengManagedSlotDescriptor` + 五类 aggregate API。
- codegen 层原先对 aggregate 的事实提取存在 spec-only 硬编码：当前 aggregate 主要由 object-form `spec` 打通；未来新增 tuple、值语义 struct、union value 等 aggregate 类型时，若不先收敛 codegen 抽象，仍需要额外改动泛型实参路径。

这与值模型设计目标不一致。值模型文档已经明确：新增按值聚合类型时，应只新增类型级描述符，不修改 runtime walker 与公共 helper。

## 2. 当前实现事实

### 2.1 已经通用的部分

1. `FengValueKind` 已把值模型收敛为三分类：`FENG_VALUE_TRIVIAL`、`FENG_VALUE_MANAGED_POINTER`、`FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`。
2. `FengGenericParamDescriptor` 已具备 generic shared body 所需的最小通用信息：`size`、`kind`、`type_kind`、`aggregate`、`witness`。
3. aggregate 生命周期 API 已经固定为：`feng_aggregate_retain`、`feng_aggregate_release`、`feng_aggregate_assign`、`feng_aggregate_take`、`feng_aggregate_default_init`。
4. aggregate walker 已经只消费静态 slot 描述符，不按类型种类分派。
5. object-form `spec` 作为 aggregate 的现有接入，已经证明 generic descriptor、aggregate return、aggregate field、if/match aggregate result 这些路径可以工作。

基于上述结构，现有 descriptor 结构定义本身无需扩展；当前优化重点应放在 codegen aggregate introspection，而不是改动 runtime descriptor 形状。

### 2.2 本轮修复前写死的部分

1. `cgtype_value_kind(...)` 只把 `CG_TYPE_SPEC` 判定为 aggregate；新增 aggregate 类型后，这里必须扩展。
2. `cg_generic_descriptor_expr(...)` 在 aggregate 分支上依赖 spec-only descriptor helper；一旦拿不到 aggregate descriptor，就直接报 `missing flatten rule`。
3. `cg_aggregate_field_desc_name(...)`、`cg_aggregate_pointer_slot_count(...)`、`cg_emit_aggregate_pointer_slot_rows(...)` 只认 object-form `spec`。
4. codegen 中已有大量站点直接调用上述 helper；如果未来只修泛型实参路径，不统一这些 helper，仍会在字段、数组元素、结果槽、清理逻辑等相邻路径再次失败。

本轮实现已将这些入口收敛到 `CGAggregateFacts` / `cg_aggregate_facts(...)`：值分类、carrier C 类型、runtime `type_kind`、descriptor 名称、managed-slot flatten、default-init kind、cleanup 注册与 cleanup zero 均从同一份 codegen facts 读取。

### 2.3 现有报错的准确含义

“aggregate type as generic type argument not yet supported (missing flatten rule)” 的准确含义不是“所有 aggregate 都不能作为泛型实参”，而是：

- 泛型共享体在 aggregate 实参上需要静态 aggregate descriptor；
- 当前 codegen 只有部分 aggregate 形态能提供这份描述；
- 一旦某个 aggregate 类型无法提供统一的 flatten 规则与 descriptor，就无法进入泛型共享代码路径。

因此，当前问题的本质是 **codegen aggregate introspection 不通用**，而不是 runtime aggregate 模型不成立。

## 3. 目标

### 3.1 直接目标

把“aggregate 作为泛型实参”的支持收敛成通用机制，使未来新增 aggregate 类型时：

- 不再修改 generic codegen 主路径；
- 不再修改 aggregate runtime walker；
- 只需为新类型补齐其静态布局与 descriptor 事实；
- 即可自动进入泛型实参支持。

### 3.2 硬约束

本方案必须满足严格的 **0 增量运行时开销** 约束。这里的“0 增量”不是只指对象布局或 runtime API 不新增，而是指所有当前已经支持的泛型类型与调用场景，在所有情况下生成后的运行时代价只能持平或减少，不能增加。

1. 允许增加编译期代码与静态 descriptor。
2. 不允许新增堆分配。
3. 不允许为每个值实例增加额外字节或额外 descriptor 指针。
4. 不允许引入运行时反射。
5. 不允许为了 aggregate 泛型支持增加 generic boxing。
6. 不允许修改 aggregate walker 的热点分支结构。
7. 新 aggregate 类型接入后，其运行时成本必须等价于“直接使用该类型本来就需要承担的成本”，而不是额外叠加“泛型支持成本”。
8. 对当前已支持的泛型实参类型与调用场景，generated C 不得新增额外函数调用、条件分支、retain/release、assign/take/default-init、临时值生命周期管理、descriptor 转发、wrapper/thunk 调用或 runtime helper 调用。
9. 对当前已支持的 `trivial` 类型，仍必须走原有直接值路径；不得因为统一抽象而绕入 aggregate descriptor 查询或 aggregate 生命周期 API。
10. 对当前已支持的 `managed-pointer` 类型，仍必须走原有单指针路径；不得绕入 aggregate API，也不得增加额外 retain/release。
11. 对当前已支持的 object-form `spec` aggregate，生成的 aggregate 生命周期调用序列必须与现状等价或更少；不得新增 wrapper、boxing、额外 indirection 或额外 cleanup 槽。
12. 对当前已有 generic descriptor forwarding 场景，不得新增运行时判断、descriptor 复制、descriptor 适配器分配或额外转发层。

### 3.3 非目标

1. 本文不交付 tuple / union value / 值语义 struct 的语法与语义本身。
2. 本文不重写 runtime aggregate ABI。
3. 本文不新增第四类顶层运行时值模型。
4. 本文不为单个未来类型增加临时特判。

## 4. 设计原则

### 4.1 aggregate 泛型支持是编译期问题，不是 runtime 重构问题

runtime 当前已经具备处理任意 aggregate-with-managed-slots 的抽象。后续优化重点应放在 codegen：

- 统一提取 aggregate 的 carrier 形状、descriptor 符号、managed-slot flatten 规则、默认初始化事实；
- 让 generic descriptor 构造、字段 flatten、数组元素处理、结果槽清理等路径共享同一份 aggregate 事实来源。

这层收敛只能改变编译器内部如何取得类型事实，不得改变当前已支持场景的 generated C 运行时操作数量。若某个实现方案需要在生成代码中增加新的判断、调用或临时对象，即使 runtime descriptor 结构没有变化，也视为违反本方案。

### 4.2 新 aggregate 类型的接入责任必须一次性收敛

任何新 aggregate 类型在进入 codegen 之前，都必须能回答以下问题：

1. 它的值模型分类是什么。
2. 它的 carrier C 形状是什么。
3. 它的 `FengAggregateValueDescriptor` 符号是什么。
4. 它的 managed-slot flatten 规则是什么。
5. 它的默认初始化策略是什么。
6. 它对应的 runtime semantic type classification 是什么。

只有当这些事实一次性齐备时，generic path 才能自动成立。

### 4.3 不能把类型种类分派扩散到 runtime

future tuple / union value 的接入，不应表现为：

- 在 `feng_aggregate.c` 里新增 `if tuple ... else if union ...`；
- 在每个值实例中埋 descriptor 指针；
- 在 runtime helper 中做动态结构探测。

正确方向只能是：codegen 生成新的静态 descriptor，runtime 继续只消费 descriptor。

## 5. 推荐方案

### 5.1 在 codegen 内建立统一 aggregate introspection contract

新增一层只存在于 codegen 内部的 aggregate 查询抽象，用来统一替代当前分散的 spec-only helper。

这层抽象至少要回答：

1. 当前类型的 codegen value-kind 是 `trivial`、`managed-pointer` 还是 `aggregate-with-managed-slots`。
2. aggregate carrier 对应的 C 类型是什么。
3. aggregate 的 `FengAggregateValueDescriptor` 符号名是什么。
4. aggregate flatten 后有多少个 managed pointer slot。
5. 如何枚举这些 slot，并产出对象字段 flatten 所需的偏移表达。
6. 如何为 aggregate local 注册 cleanup slots，并在释放后清零对应 slots。
7. 默认初始化是 zero-bytes 还是 init-fn。
8. 对应的 runtime `type_kind` 是什么。

这层抽象必须是编译期静态查询，不得产生 runtime callback，也不得在 generated C 中引入新的运行时查询步骤。对当前已经支持的类型，切换到该 contract 后生成代码的关键调用序列必须保持等价或更少。

### 5.2 用统一 contract 替换当前 scattered helper

优先替换以下几类入口：

1. generic descriptor 构造路径。
2. 用户类型字段的 managed descriptor 生成路径。
3. aggregate field release / assign / default-init 路径。
4. aggregate result slot 与 cleanup 注册路径。
5. aggregate array element lowering 路径。
6. `cgtype_value_kind(...)`、`cg_emit_c_type(...)`、`cg_runtime_type_kind_name(...)` 这类泛型 descriptor 前置分类路径。

替换完成后，generic path 与字段/数组/结果槽路径必须共享同一份 aggregate 事实来源，避免一处支持 tuple、另一处仍只认 spec。

### 5.3 保持 nominal type 与 aggregate facts 分离

不建议把大量 aggregate 元数据直接塞进每个 `CGType` 实例。

推荐做法是：

- `CGType` 继续描述 nominal kind 与 owner；
- 由 owner 侧 metadata 负责产出 aggregate facts；
- codegen 通过统一 aggregate introspection contract 读取这些 facts。

这样做有三个好处：

1. 不会让 `cgtype_clone/free` 复杂化。
2. 不会把 spec-only 字段结构继续复制到 tuple / union。
3. 更符合“新增类型只补自己的静态事实”的开闭原则。

### 5.4 把生命周期支持与 runtime-generic 语义支持拆开

`FengGenericParamDescriptor` 中：

- `size`、`kind`、`aggregate` 解决的是 generic copy / retain / release / return。
- `type_kind` 解决的是 runtime-generic helper 的语义分派，例如相等比较。

因此，aggregate 泛型支持的第一目标是把生命周期与布局路径通用化；这并不要求现在就为未来 tuple / union 添加完整 runtime-generic 语义。

推荐策略：

- 先保证新 aggregate 类型一旦存在，就能作为泛型实参稳定通过 copy / retain / release / return / cleanup 路径；
- 只有当该类型真实进入 `feng_expression_equal(...)` 这类 runtime-generic helper 时，再扩展对应的 `FengRuntimeTypeKind` 与 helper 语义；
- 不允许为了“也许以后要用”而提前引入半支持状态的 runtime 分类。

## 6. 未来 aggregate 类型的接入 contract

未来任一 aggregate 类型，例如 tuple、值语义 struct、union value，在进入 generic path 前必须一次性交付以下内容：

1. carrier C 布局。
2. 静态 `FengManagedSlotDescriptor[]`。
3. 静态 `FengAggregateValueDescriptor`。
4. 默认初始化策略描述。
5. codegen aggregate introspection 所需的 owner metadata。
6. codegen value-kind 与 runtime `type_kind` 事实。
7. 对象字段 flatten 与 aggregate local cleanup 所需的 slot 枚举 / cleanup emitter。
8. 如有 runtime-generic helper 需求，再补对应 helper 语义。

满足 1-7 后，generic 实参支持应自动成立；禁止再修改 generic descriptor builder，给该类型额外开分支。若该类型需要进入 runtime-generic helper，再按第 8 点独立补齐语义，不应反向污染 generic descriptor 构造主路径。

## 7. 风险与错误方向

### 7.1 需要避免的错误方向

1. 为 tuple 单独新增“generic aggregate tuple path”。
2. 为 union value 单独新增第二套 aggregate walker。
3. 在 runtime 通过动态探测字段布局决定 retain / release。
4. 在每个 aggregate 实例中额外挂 descriptor 指针。
5. 用 boxing 把所有 future aggregate 包成 managed-pointer，再借此绕过 aggregate flatten。

这些方向都会违反“无增量运行时开销”和“新增类型只补静态描述符”的目标。

### 7.2 当前最容易遗漏的风险点

1. 只修 `cg_generic_descriptor_expr(...)`，不统一字段 flatten helper，导致 future aggregate 只能当泛型实参，不能当字段或结果槽。
2. 只修 aggregate descriptor，不同步审查 cleanup 注册、if/match result slot、数组元素 lowering 等邻接路径。
3. 过早扩展 `FengRuntimeTypeKind`，却没有补齐 runtime helper 的真实语义，造成“有 type kind、无行为定义”的半支持状态。

## 8. 验证口径

### 8.1 结构验证

实现完成后，应满足：

1. `feng_aggregate.c` 不因 future aggregate 类型新增任何 kind-specific 热点分支。
2. `FengGenericParamDescriptor` 与 aggregate 生命周期 ABI 不因本方案而新增实例级成本。
3. codegen 中不再通过 scattered spec-only helper 决定 aggregate flatten 与 generic descriptor 生成。
4. `src/runtime/feng_runtime.h` 的 descriptor 结构定义保持不变；除注释外，不应出现 runtime ABI 结构性修改。
5. `src/runtime/feng_expression.c` 不因本轮 aggregate introspection 收敛新增 helper 分支；runtime-generic helper 语义扩展必须作为 future aggregate 语义接入的独立事项处理。

### 8.2 运行时零增量验证

对所有当前已经支持的泛型类型与调用场景，必须验证 generated C 的关键运行时路径持平或减少：

1. `trivial` 泛型实参：不新增 aggregate descriptor、aggregate API 调用、额外临时值 cleanup 或 runtime helper 调用。
2. `managed-pointer` 泛型实参：不新增额外 retain/release、wrapper/thunk、条件分支或 aggregate API 调用。
3. object-form `spec` aggregate 泛型实参：`feng_aggregate_retain/release/assign/take/default_init` 的调用序列保持等价或减少，不新增 boxing、wrapper 或额外 cleanup 槽。
4. generic descriptor forwarding：不新增运行时 descriptor 复制、适配器分配或额外间接层。
5. 数组元素、字段、if/match result slot 等邻接路径：切换到统一 contract 后，不得比当前 spec-only 路径多出运行时分派或生命周期操作。

### 8.3 回归验证

现有 spec aggregate 相关能力必须保持不回退，至少覆盖：

1. generic spec arg。
2. aggregate return。
3. aggregate field。
4. if/match aggregate result。

### 8.4 新能力验证

当 tuple / value aggregate 能力落地时，至少补两类测试：

1. trivial aggregate 作为泛型实参，证明不会额外依赖 aggregate descriptor，也不会引入额外 runtime 成本。
2. 含 managed slots 的 aggregate 作为泛型实参，证明无需修改 generic codegen 主路径，只通过静态 descriptor 接入即可工作。

## 9. 影响面与规模预估

以下预估按**只收敛 aggregate introspection，不同时交付 tuple / union value / 值语义 struct 本体**为前提。

### 9.1 预计涉及文件

1. `src/codegen/codegen.c`
    这是主改动面。当前 aggregate 相关锚点分布在值分类、runtime type 分类、generic descriptor 构造、字段 flatten、数组元素 lowering、结果槽与 cleanup 等多个位置；单文件内已有二十多个调用点直接依赖现有 spec-only helper。预计需要在这里完成统一 aggregate introspection contract 的建立、接管和旧 helper 收敛。
2. `test/codegen/test_codegen.c`
    这是主回归面。现有 generic spec arg、aggregate return、aggregate field、if/match aggregate result 都应继续作为回归锚点；若本次改动只收敛抽象而不引入新 aggregate 语法，测试主要是保证 spec aggregate 路径不回退。
3. `dev/feng-generics-aggregate-optimize.md`
    方案文档需要同步记录影响面、实施顺序和验收口径。
4. `src/runtime/feng_runtime.h`
    预期不需要改动 descriptor 结构定义；本轮仅允许注释层面的同步。若出现任何结构字段、枚举语义或 ABI 声明改动，应暂停并重新确认是否偏离 0 增量运行时开销目标。
5. `src/runtime/feng_expression.c`
    预期不属于本轮改动面，按当前收敛口径应为 0 行。只有 future aggregate 真实进入 runtime-generic helper 语义时，才允许另行设计并修改该文件。
6. `src/runtime/feng_aggregate.c`
    预期不改。该文件应继续作为“不得增加 kind-specific 分支”的验证基线。

### 9.2 大致行数

按当前实现分布，预计改动规模如下：

1. `src/codegen/codegen.c`：约 220 到 360 行。
    其中包括：新增统一 aggregate introspection helper、替换现有 spec-only helper 的主要调用点、清理旧分派逻辑。
2. `test/codegen/test_codegen.c`：约 40 到 90 行。
    主要用于补或改 focused regression，确保现有 spec aggregate 路径不回退。
3. `dev/feng-generics-aggregate-optimize.md`：约 40 到 100 行。
    主要用于补实施影响、TODO 和验收说明。
4. `src/runtime/feng_runtime.h`：0 行代码改动，最多允许注释同步。
    目标是保持 descriptor 结构定义与 runtime ABI 不变；若出现代码级改动，应重新审查是否偏离方案边界。
5. `src/runtime/feng_expression.c`：0 行。
    本轮不扩展 runtime-generic helper 语义；若需要修改该文件，说明任务范围已经超出 aggregate introspection 收敛。
6. `src/runtime/feng_aggregate.c`：0 行。

按上述口径，**本轮总改动量预计约 300 到 560 行，绝大部分集中在 `src/codegen/codegen.c`**。

## 10. 实施状态与后续 TODO

### 10.1 本轮实施结果

1. 已固化不变边界。
    明确 `FengGenericParamDescriptor`、`FengManagedSlotDescriptor`、`FengAggregateValueDescriptor` 的结构定义不扩展；`feng_aggregate.c` 不新增 kind-specific 分支；本轮不把 future aggregate 的 runtime-generic helper 语义一并纳入。
2. 已建立 codegen 内部统一 aggregate introspection contract。
    在 `src/codegen/codegen.c` 中新增统一查询入口，收敛 aggregate 的值分类、carrier C 形状、descriptor 符号、managed-slot flatten、default-init kind、cleanup 注册、cleanup zero、runtime `type_kind` 等事实。
3. 已接管 generic descriptor 构造路径。
    `cg_generic_descriptor_expr(...)` 的 aggregate 分支已读取 `CGAggregateFacts`；trivial 分支也改为使用统一 C carrier 输出，避免 future trivial aggregate 需要改泛型 descriptor builder。
4. 已接管字段与结果槽相关路径。
    字段 flatten、field release、aggregate result slot、cleanup 注册等路径已切到统一 contract。
5. 已接管数组与元素路径。
    数组元素 lowering、aggregate 元素 descriptor 选择等站点已切到新的 contract，避免 generic path 支持而数组 path 仍然只认 spec。
6. 已清理旧 helper 与重复分派。
    旧的 spec-only descriptor helper 已收敛为 `cg_aggregate_desc_name(...)` / `cg_aggregate_facts(...)`，不再形成两套事实来源。
7. 已补 focused regression。
    `test/codegen/test_codegen.c` 已新增 generic aggregate facts shape 用例，覆盖 trivial、managed-pointer、object-form spec aggregate 三类 descriptor 形状。
8. 已做回归与结构检查。
    检查 `src/runtime/feng_runtime.h` 的 descriptor 结构是否保持稳定、`src/runtime/feng_aggregate.c` 是否完全未被污染，并确认 codegen 不再依赖 scattered spec-only helper。
9. 已做 generated C 成本检查。
    对当前已支持的 trivial、managed-pointer、object-form spec aggregate、descriptor forwarding 代表用例，比较关键 generated C 调用序列，确认没有新增函数调用、分支、retain/release、aggregate API 调用、wrapper 或额外 cleanup 槽。

### 10.2 后续联动 TODO

1. 当 tuple / value aggregate 真正落地时，先只实现该类型自己的静态布局与 descriptor 事实，不修改 generic codegen 主路径。
2. 为 future aggregate 增加两类验证：一个 trivial aggregate 泛型实参用例，一个含 managed slots 的 aggregate 泛型实参用例。
3. 只有当 future aggregate 真实进入 `feng_expression_equal(...)` 等 runtime-generic helper 时，才扩展 `FengRuntimeTypeKind` 与对应 helper 语义。

## 11. 结论

当前问题不是 runtime aggregate 模型不通用，而是 codegen 对 aggregate 事实的提取尚未完全通用。

现有 descriptor 结构定义无需扩展；优化重点在 codegen aggregate introspection。

正确的优化方向是：

- 保持 runtime 三分类、aggregate ABI、aggregate walker 不变；
- 把 aggregate 的布局与 descriptor 事实收口为 codegen 内部统一 contract；
- 让 future tuple / union / value aggregate 只通过补齐静态 descriptor 与布局事实接入；
- 使“能否作为泛型实参”成为 aggregate 类型接入后的自然结果，而不是额外功能分支。

这条路径才能同时满足：

1. 对扩展开放。
2. 对修改封闭。
3. 不增加运行时实例成本。
4. 不增加 runtime 热路径复杂度。
5. 对所有当前已支持的泛型类型与场景，generated C 运行时操作数量持平或减少。
