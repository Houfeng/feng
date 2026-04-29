# spec 运行时与 codegen 可执行方案草案（fat 值方向，不含 @fixed / C ABI）

> 本文档是实现草案，不是语言权威规范。
> 语言语义以 [docs/feng-spec.md](../docs/feng-spec.md)、[docs/feng-fit.md](../docs/feng-fit.md)、[docs/feng-function.md](../docs/feng-function.md) 为准。
> 值模型基座见 [feng-value-model-delivered.md](./feng-value-model-delivered.md)；本文档不重复其规范，只声明 spec 如何在该基座上落点。
> 本草案只讨论非 `@fixed` 场景，不涉及 C ABI 与函数指针桥接。

## 1 目标与范围

### 1.1 直接目标

把 `spec` 的发码方案收敛为**胖值（fat value）方向**，并规定：

- object-form `spec` 的运行时值表示与 ABI；
- object-form `spec` 的强制转换、成员访问、参数传递、返回值、字段、数组元素的 emit 路径；
- object-form `spec` 默认零值的生成规则；
- object-form `spec` `==` / `!=` 的比较语义；
- object-form `spec` 与 [feng-value-model-delivered.md](./feng-value-model-delivered.md) 的对接点；
- semantic 必须为 codegen 提供的 sidecar；
- 4b-α / 4b-β / 4b-γ 三个子步骤的范围与交付物。

### 1.2 非目标（明确禁止）

- **双托管 box 方案**：先前草案使用 `FengSpecRef__S { FengManagedHeader; subject; witness; }` 作为独立托管对象，已被废弃；本草案禁止再以 box 方式承载 object-form `spec`。
- callable-form `spec`（含函数值 / 方法值 / lambda 适配 / 默认 stub）：留待 [feng-plan.md](./feng-plan.md) Phase 2，本文档只声明其值模型预期与现有结构的接入方式，不展开发码细节。
- `@fixed spec` 与 C ABI、跨 TU 符号导出。
- 运行时反射、全局 registry、自动鸭子类型适配、object-form spec 的去虚化与内联缓存。

### 1.3 与 value-model 的关系

本文档 **不** 引入新的 runtime ABI；spec 值的生命周期一律走 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §5 的五个聚合 API。本文档对 value-model 的依赖项见 §11，必须在 spec 发码扩面前完成。

### 1.4 子步骤划分

| 步骤 | 范围 | 状态 |
| --- | --- | --- |
| 4b-α | object-form spec 最小垂直切片：spec 形参 + 对象→spec 强制转换 + spec 方法调用 + spec 局部 + 1 个 smoke | **已交付**（commit `f69dfe0`） |
| 4b-β | spec 字段（对象成员为 spec）+ spec 默认零值 + spec `==` / `!=` + 3 个 smoke | 待实施 |
| 4b-γ | fit-method 来源 witness、spec 数组元素、spec 作为返回值的完整移动 + 对应 smoke | 待实施 |
| Phase 2 | callable-form spec | 不在 4b 范围 |

> 各章节内行文将以 **\[已交付 4b-α]** / **\[4b-β]** / **\[4b-γ]** 标注，第 13 节再给出完整清单。

## 2 设计约束

### 2.1 必须保持的语言语义

- `spec` 可作为参数类型、返回类型、成员类型与其他类型位置中的引用目标。
- object-form `spec` 不约束物理布局，只约束可见形状。
- object-form `spec` 的满足关系必须来自声明头或可见 `fit`，禁止鸭子类型。
- 无初始值的 `spec` 绑定必须得到默认 witness。
- 默认 witness 对开发者不可见，不可显式引用。
- 每次 `spec` 默认初始化都创建新实例，不复用共享单例。
- `spec` 值的 `==` / `!=` 默认比较 **subject 引用身份**，不受中间 coercion 次数影响。
- callable-form `spec` 是可调用形状，不是对象布局契约（Phase 2 处理）。

### 2.2 必须贴合的现有运行时基座

- 单指针原语 `feng_retain` / `feng_release` / `feng_assign` 与作用域 / 异常清理表。
- `FengManagedHeader` 与 cycle collector：**不修改**。
- value-model §5 的五个聚合 API 与 walker：**不在本文档新增**，仅消费。

### 2.3 必须与 value-model 对齐的硬约束

- spec 值是 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §2.1 中的 `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`。
- spec 值的描述符是该文档 §3.1 的 `FengAggregateValueDescriptor`。
- spec 值的默认初始化使用该文档 §4.2 的 `FENG_DEFAULT_INIT_FN`。
- spec 字段对对象 `managed_fields` 的接入走该文档 §7.2 的展平规则。
- spec 数组元素走该文档 §7.3 的元素三分类。

## 3 总体模型

### 3.1 object-form spec 是胖值

object-form `spec` 在运行时的值表示为：

```c
struct FengSpecValue__<mod>__<S> {
    void *subject;                                       /* 托管指针，参与 ARC */
    const struct FengSpecWitness__<mod>__<S> *witness;   /* 静态只读表，不参与 ARC */
};
```

要点：

- **按值聚合，无 header**，本身不是托管对象，不进入 ARC 自身的对象计数。
- **subject 在偏移 0**，与 value-model §3 的 `FENG_SLOT_POINTER` 槽位对齐；现有"单指针 cleanup chain"通过 `(void **)&local.subject` 直接复用，无需新清理原语。
- **witness 是静态全局表**，在程序生命周期内有效，绝不参与 retain / release。
- 大小固定 16 字节（64-bit 平台），按值传参时多数 ABI 走寄存器。

### 3.2 为什么不用 box

- **额外分配**：box 方案对每次 coercion 都要分配一个堆托管对象，再装一根指针；fat 方案只是在栈/寄存器里组合 16 字节。
- **生命周期复杂**：box 与 subject 形成两层托管引用，cycle collector 必须把 box 也纳入扫描，spec 之间互引用极易触发误判与额外开销。
- **相等性失真**：每次 coercion 新建 box，比较 box 地址会被中间 coercion 次数污染；fat 方案直接比 `subject`，语义清洁。
- **C ABI 易控**：fat 方案是普通 C struct，调用方 / 被调方使用同一 typedef，编译器自然处理寄存器/栈传递。

### 3.3 编译期 / 运行时职责边界

- 编译期决定：某个 `type` 是否满足某个 `spec`、当前作用域哪些 `fit` 可见、spec coercion 站点应使用哪条满足关系与哪张 witness。
- 运行时只负责：执行已选定的 witness thunk、按 value-model §5 处理 spec 值生命周期。

运行时**不得**：

- 动态搜索"哪些类型满足某个 spec"。
- 在运行期重新解释 fit 可见性。
- 基于对象形状做结构化自动适配。

## 4 object-form spec 的运行时结构

### 4.1 值结构 \[已交付 4b-α]

每个 object-form `spec S`（在模块 `mod` 内）生成独立的具名 typedef：

```c
struct FengSpecValue__demo__Named {
    void *subject;
    const struct FengSpecWitness__demo__Named *witness;
};
```

不使用统一 `void* + void*` 的两字段无名结构：会丢失 witness 静态类型，迫使每个 callsite 强转，易错。

### 4.2 witness 表结构 \[已交付 4b-α]

每个 object-form `spec S` 生成独立 witness 表 typedef：

```c
struct FengSpecWitness__demo__Named {
    /* 按 spec 成员声明顺序排列；目前仅方法。 */
    FengString *(*greet)(void *subject);
    /* 字段访问槽位见 §4.3，4b-β 引入。 */

    /* 若无任何成员，发射一个 char _padding 占位，避免空 struct。 */
};
```

要点：

- witness 表的成员顺序与 `spec` 源码声明顺序严格一致，便于 codegen / 调试器交叉对照。
- 表本身是 `static const`，单 TU 内通过 (T,S) 缓存唯一化。
- 跨 TU 共享留待 Phase 2 一并解决（`@fixed` 章节会决定符号导出策略）。

### 4.3 字段访问槽位 \[4b-β]

object-form `spec` 字段必须经 thunk 访问，不允许 offset 直读：

- spec 不约束布局，offset 直读会被 fit 与多类型适配场景立即破坏。
- 字段读 thunk：`FengString *(*get_name)(void *subject)`；写 thunk（`var` 字段）：`void (*set_name)(void *subject, FengString *value)`。
- `let` 字段不发 set thunk，witness 表中不留该位。
- thunk 命名：`FengSpecThunk__<modT>__<T>__as__<modS>__<S>__<member>`（4b-α 已用此命名，4b-β 复用）。

### 4.4 调用约定 \[已交付 4b-α]

- **形参**：spec 值按 C struct 按值传递；callee **借用**，调用方负责保持源活到调用结束（与现有 managed-pointer 形参约定一致，`Local.is_param: caller owns`）。
- **方法分派**：`recv.witness->slot(recv.subject, args...)`。
- **绑定**：`let n: Named = u;` — 当源是 owns_ref 临时值，直接搬移；当源是借用，发 `feng_retain(&n_local.subject)`（subject 在 0 偏移，等价 `feng_retain(n_local.subject)` 但通过 value-model API 表达）。
- **作用域退出**：`feng_cleanup_pop(); feng_release(local.subject); local.subject = NULL;`（4b-α 直写，4b-β 起改用 §5 中将归并的 helper）。
- **返回**：与 managed return 对称——若值是借用，先 retain subject 再 transfer；若是 owns_ref，直接搬移。

> **\[4b-β 任务]** 把 4b-α 的"直写 `feng_release(local.subject)`"统一替换为 `feng_aggregate_release(&local, &FengSpecAgg__M__S)`，避免 spec 值生命周期路径长期与 value-model API 不一致。完成后所有 spec 局部的清理都通过描述符驱动。

## 5 witness 生成规则

### 5.1 (T, S) 唯一化与缓冲位置 \[已交付 4b-α]

- 每个具体 `(type T, spec S)` 组合生成一张 witness 表与一组 thunk；按 (T, S) 缓存，重复 coercion 不重复生成。
- thunk **forward decl** 写入 codegen 的 `cg->fn_protos`；thunk **body** 与 witness 表实例写入新增的 `cg->witness_defs` 缓冲，最后由 `cg_finalize` 拼接到 `fn_defs` 之后。
- 分桶原因：4b-α 早期曾误把 witness 实体写入 `cg->fn_defs`，导致它被插入"正在发射的函数体"内部，编译失败。`witness_defs` 是该问题的结构性修复，禁止退化。

### 5.2 witness 来源类型 \[4b-α / β / γ]

| 来源 | 4b-α | 4b-β | 4b-γ |
| --- | --- | --- | --- |
| `TYPE_OWN_METHOD` | ✅ 支持 | ✅ | ✅ |
| `TYPE_OWN_FIELD`（spec 含字段时） | 拒绝（明确报错） | ✅ 支持 | ✅ |
| `FIT_METHOD`（来自可见 fit） | 拒绝（明确报错） | 拒绝 | ✅ 支持 |

semantic 已经通过 `FengSpecWitness.source_kind` 给出来源；codegen 只按上表分派。任何在当前阶段未支持的来源都必须发出明确的"deferred to 4b-β/γ"错误，禁止静默回退。

### 5.3 witness thunk 责任 \[4b-α / β / γ]

thunk 是编译器生成的静态适配层，按成员形态分别承担：

- 方法 thunk：把 `void *subject` 强转回具体对象指针，调用 `T` 的方法或 fit 实现，转发返回值；返回值若是 spec / closure 由调用方 emit 二次包装，本 thunk 不感知。
- 字段读 thunk：返回 `_self->field`（非 owning）；调用方负责 retain / 包装。
- 字段写 thunk：`feng_assign((void **)&_self->field, value);`（owning 写入）。

thunk 的所有权语义统一为"借用进，借用出"——所有权调整在调用方完成，与现有 method emit 保持一致。

### 5.4 witness 表初始化 \[已交付 4b-α]

```c
static const struct FengSpecWitness__demo__Named FengWitness__demo__User__as__demo__Named = {
    .greet = FengSpecThunk__demo__User__as__demo__Named__greet,
};
```

为了让任何在 `cg->fn_defs` 中的函数体能够引用 witness 实例，codegen 在 `cg->fn_protos` 同时发射其前向声明。

## 6 默认零值

> object-form `spec` 默认零值不能为 `NULL`：fat 值的 `subject` 必须是真实托管对象，`witness` 必须是有效静态表。`memset(0)` 得到非法值。

### 6.1 选用规则 \[4b-β]

按 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §4.3：

- spec 默认零值一律选用 `FENG_DEFAULT_INIT_FN`。
- codegen 为每个 object-form `spec S` 生成 init 函数 `FengSpecDefaultInit__<modS>__<S>`，并挂到该 spec 的 `FengAggregateValueDescriptor.default_init` 上。

### 6.2 隐藏 subject 类型 \[4b-β]

为每个 object-form `spec S` 生成一个隐藏具体类型 `FengSpecDefault__<modS>__<S>__Subject`：

```c
struct FengSpecDefault__demo__Named__Subject {
    FengManagedHeader header;
    /* 每个 spec 字段对应一个槽位，按字段类型默认零值初始化。 */
};
```

要点：

- 该类型对开发者不可见，名字以 `Default__` 前缀；不允许出现在源码可见命名表里。
- 是普通托管对象，走现有 `feng_object_new` 分配，参与 ARC 与 cycle collector。
- spec 字段在该隐藏类型上以"自有字段"形式存在，由 codegen 填充默认零值（按 [feng-builtin-type.md](../docs/feng-builtin-type.md) 默认零值规范）。

### 6.3 默认 witness \[4b-β]

为每个 object-form `spec S` 生成一张默认 witness 表 `FengSpecDefaultWitness__<modS>__<S>`：

- 字段读 thunk：返回该字段类型的默认零值（基础类型走 `0` / `0.0` / `false`；string / array / object / spec 走对应默认零值路径）。
- 字段写 thunk（仅 `var`）：写入隐藏 subject 对应槽位，正常 `feng_assign`。
- 方法 thunk：按返回类型生成默认零值返回；`void` 方法生成空实现。

> 默认 witness 与具体 (T, S) witness 在结构上**完全一致**（同一 `FengSpecWitness__M__S` typedef），仅 thunk 实现不同。

### 6.4 init 函数 \[4b-β]

```c
static void FengSpecDefaultInit__demo__Named(void *out) {
    struct FengSpecValue__demo__Named *v = out;
    v->subject = (void *)FengSpecDefault__demo__Named__new_subject();  /* +1 已 retain */
    v->witness = &FengSpecDefaultWitness__demo__Named;
}
```

要点：

- `new_subject` 内部使用 `feng_object_new`，返回值已是 +1 状态。
- init 函数不调 `feng_retain`，因为 `new_subject` 已经返回 owning。
- 由 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §5 的 `feng_aggregate_default_init` 调用。

### 6.5 调用站点 \[4b-β]

凡是 spec 局部 / 字段 / 数组元素无显式初始化时，codegen emit：

```c
struct FengSpecValue__demo__Named s;
feng_aggregate_default_init(&s, &FengSpecAgg__demo__Named);
/* 按聚合作用域规则注册 cleanup */
```

## 7 相等性 \[4b-β]

object-form `spec` 的 `==` / `!=` **比较 subject 引用身份**：

```c
/* a, b 是 struct FengSpecValue__M__S */
bool eq = (a.subject == b.subject);
```

理由：

- fat 值无 box，比较结构体本身字段不能区分"两次不同 coercion 得到的同一 subject"——witness 是只读静态表指针，跨 (T, S) 必然不等。
- 仅比较 subject 与"身份比较 = 判断是否同一被引用对象"语义吻合。

semantic 已通过 `FengSpecEquality` sidecar 把"两侧表达式应按 spec 身份比较"的结论给到 codegen；codegen 不再二次推导。该规则只对 object-form `spec` 生效；非 spec 值仍走原有路径。

## 8 callable-form spec（不在 4b 范围）

callable-form `spec` 不是对象布局契约，而是函数形状。Phase 2 起才正式处理。本文档对其值表示的预期方向是：

- 值表示为按值聚合 `{ void *env; const FengCallableInvoke *invoke; }` 的 fat 形态，与 object-form spec 在 ABI 形态上同构；
- env 走 closure 基座，invoke 是按签名生成的静态调用适配器；
- 默认零值生成方式与 object-form 类似，但 subject 类型替换为零捕获 closure。

具体细节、closure 与函数值 / 方法值 / lambda 的接入路径、默认 stub 生成规则，留待 Phase 2 单独定稿，本文档不展开。

## 9 codegen 降级规则

### 9.1 对象 → object-form spec 强制转换 \[已交付 4b-α / 扩展 4b-γ]

凡是以下场景：

- `let x: Named = user;`
- `use_named(user);` 当形参 `Named`
- `return user;` 当返回类型 `Named`
- `obj.field = user;` 当字段类型 `Named`
- 数组元素位置 / spec 字段位置写入

统一发码为：

```c
((struct FengSpecValue__M__S){ .subject = (void *)<src_expr>, .witness = &FengWitness__T__as__S })
```

调用方决定 retain：

- 源是 owns_ref 临时：搬移，不额外 retain（由后续聚合操作的 owns_ref 语义承担生命周期）。
- 源是借用：写入持久槽位（绑定 / 字段 / 数组元素 / 返回）时，按 value-model §5 走 `feng_aggregate_retain` 或与之等价的逐槽 retain；纯参数借用不额外 retain。

> **\[4b-γ 任务]** 4b-α 在传参路径上是"源借用 → 形参借用 → 不 retain"；当 spec 出现在数组元素 / 对象字段 / 返回值的持久位置时，必须改走 `feng_aggregate_retain` 而不是 hand-rolled `feng_retain(.subject)`，与 value-model API 对齐。

### 9.2 object-form spec 成员访问 \[已交付 4b-α 方法 / 4b-β 字段]

- 方法调用：`recv.witness->method(recv.subject, args...)`。
- 字段读：`recv.witness->get_field(recv.subject)`。
- 字段写：`recv.witness->set_field(recv.subject, value)`。

接收方在调用前必须先 materialize 到一个借用本地（避免对临时值多次取址），与 4b-α 已落地的 `cg_materialize_to_local` 路径一致。

### 9.3 spec 默认零值 \[4b-β]

参见 §6.5：

```c
feng_aggregate_default_init(&s, &FengSpecAgg__M__S);
```

不允许走 `memset(&s, 0, sizeof s)` 或 `s = (struct ... ){0}`。

### 9.4 spec 等值 \[4b-β]

参见 §7。codegen 直接发 `(a.subject == b.subject)` / `(a.subject != b.subject)`，无需调用 helper。

### 9.5 spec 字段在对象上 \[4b-β]

对象拥有 spec 字段时：

- 字段在对象内按 fat 值大小占位（`sizeof(struct FengSpecValue__M__S)`）。
- 字段读取：`obj->field` 是 lvalue，可直接传给 spec 形参（借用）。
- 字段写入：`feng_aggregate_assign(&obj->field, &src, &FengSpecAgg__M__S);`。
- 对象释放：`release_children` 中对 spec 字段调用 `feng_aggregate_release(&obj->field, &desc);`。
- 对象 `managed_fields` 按 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §7.2 展平：spec 字段的 subject 槽位以 `字段偏移 + 0` 作为一条 `FengManagedFieldDescriptor`（`static_desc = NULL`，多态）。

### 9.6 spec 数组 \[4b-γ]

- 数组创建按 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §7.3 传入元素分类 `aggregate` 与 `&FengSpecAgg__M__S`。
- 元素读：返回 `struct FengSpecValue__M__S` 按值（借用），或在需要拥有时由调用方 retain。
- 元素写：`feng_aggregate_assign(&arr->elements[i], &src, &desc);`
- 数组销毁：逐元素 `feng_aggregate_release`。

## 10 semantic → codegen 数据契约

### 10.1 已交付 sidecar

| sidecar | 路径 | spec codegen 用途 |
| --- | --- | --- |
| `SpecRelation` | [src/semantic/spec_relations.c](../src/semantic/spec_relations.c) | 哪条 (T, S) 满足关系生效 |
| `SpecCoercionSite` | [src/semantic/spec_coercion_sites.c](../src/semantic/spec_coercion_sites.c) | 站点 form (OBJECT / CALLABLE) 与 src/target |
| `SpecMemberAccess` | [src/semantic/spec_member_accesses.c](../src/semantic/spec_member_accesses.c) | 表达式访问哪个 spec 成员（field / method） |
| `SpecDefaultBinding` | [src/semantic/spec_default_bindings.c](../src/semantic/spec_default_bindings.c) | 绑定需要默认 witness |
| `SpecWitness` | [src/semantic/spec_witnesses.c](../src/semantic/spec_witnesses.c) | (T, S) witness 的成员来源 (TYPE_OWN_FIELD / TYPE_OWN_METHOD / FIT_METHOD) |
| `SpecEquality` | [src/semantic/spec_equalities.c](../src/semantic/spec_equalities.c) | `==` / `!=` 是否按 spec 身份比较 |

### 10.2 codegen 消费规则

- 不在 codegen 中重新做 fit 可见性判断。
- 不在 codegen 中重新做 (T, S) 满足关系搜索。
- 不在 codegen 中再次区分 object-form / callable-form：以 `SpecCoercionSite.form` 为准。
- witness 来源未支持时（见 §5.2 表），按"明确报错 + 标注 deferred"处理，禁止静默回退。

### 10.3 4b-β 起对 semantic 无新增需求

4b-β、4b-γ 全部在 codegen 与 runtime 之间完成，semantic 不再扩面。如果发现某条 spec emit 路径需要新事实，必须先评审 sidecar 设计，禁止 codegen 现场推导。

## 11 对 value-model 的依赖与本文档不新增 runtime API

### 11.1 直接依赖

[feng-value-model-delivered.md](./feng-value-model-delivered.md) **layer 已交付**（§9.4 封顶声明）；本文档对其依赖项全部就绪：

| value-model 章节 | spec codegen 依赖项 | 4b 阶段 | layer 状态 |
| --- | --- | --- | --- |
| §3 描述符 | `FengAggregateValueDescriptor` / `FengManagedSlotDescriptor` | β（spec 字段、默认零值） | ✅ 已交付（含 codegen 自动 emit `FengSpecAgg__M__S`） |
| §4 默认初始化 | `FENG_DEFAULT_INIT_FN` 路径 | β | ✅ 已交付（init 实体由 4b-β 替换 panic stub） |
| §5 五个聚合 API | retain / release / assign / take / default_init | β / γ | ✅ 已交付（含 5 项 runtime 单测） |
| §7.2 cycle collector 展平 | 含 spec 字段的对象 `managed_fields` 自动展平 | β | ✅ 已交付（codegen helper 已就位待引用站点接入） |
| §7.3 数组元素三分类 | spec 数组 | γ | ✅ 已交付（`feng_array_new_kinded` + 6 项单测） |
| §7.4 对象字段为 aggregate | spec 字段读写 / `release_children` | β | ✅ 已交付（codegen helper 已就位） |

### 11.2 本文档不新增 runtime ABI

- spec 值的 retain / release / assign / take / default_init 全部走 §5 的五个聚合 API。
- 不新增 `feng_spec_*` runtime helper。
- 不新增 spec 专用 type tag / cleanup chain。
- 不修改 `FengManagedHeader`。

### 11.3 4b-α 中尚未对齐 value-model 的临时路径

4b-α 因 value-model §7.2/§7.4 未完成，使用了"subject 在偏移 0 → 直接复用单指针 cleanup chain"的快捷路径：

- `cg_emit_cleanup_push_for_aggregate_local` 直接以 `(void **)&local.subject` 入清理栈。
- spec 局部作用域退出直接调 `feng_release(local.subject)`。

这一路径**仅在 spec 值仅含一个托管槽位 subject** 时与 `feng_aggregate_release` 等价。本文档明确将其登记为**临时 shortcut**，并要求：

- 4b-β 完成 value-model §3-§5 描述符与 API 后，立即将 spec 局部清理切换到 `feng_aggregate_release(&local, &FengSpecAgg__M__S)`；
- 4b-γ 完成 value-model §7.2/§7.3/§7.4 后，spec 字段 / 数组元素 / 返回路径直接走聚合 API，不再扩展 shortcut；
- 完成 β、γ 后，整源码路径中不应再有"对 spec 值手写 `feng_release(.subject)`"的代码。

## 12 测试面

### 12.1 4b-α 已交付

- `test/smoke/phase1a/spec_object_param.ff`：对象 → spec 形参 + spec 方法调用。

### 12.2 4b-β 必须新增

- `spec_object_field.ff`：对象字段类型为 spec，赋值后通过字段访问其成员；验证 §9.5 与 value-model §7.2 / §7.4。
- `spec_object_default.ff`：`let s: Named;` 走默认 witness，字段读返回默认零值，方法返回默认零值；验证 §6 与 value-model §4。
- `spec_equality.ff`：同一 subject 经两次 coercion 到同一 object-form spec 后仍相等；不同 subject 经相同 coercion 不相等；验证 §7。

### 12.3 4b-γ 必须新增

- `spec_array.ff`：spec 元素数组的创建、元素读写、销毁清理；验证 §9.6 与 value-model §7.3。
- `spec_return.ff`：函数返回 spec 值，调用方接收并使用；验证 §9.1 + value-model §5 take 路径。
- `spec_fit_witness.ff`：通过可见 fit 满足 spec，方法分派经 fit 路径；验证 §5.2 `FIT_METHOD` 来源。

### 12.4 单测

- 不强制新增 runtime 单测：spec 走 value-model 的聚合 API，runtime 单测应在 value-model 范围内补齐（见 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §11）。
- semantic 单测由各 sidecar 提交时已覆盖；本阶段不再新增。

### 12.5 全量回归

每个子步骤完成时执行：

- `make clean && make -j8 all && make test`：所有单元套件 + 全部 smoke 通过。
- 旧 smoke 零回归。

## 13 实施阶段（4b 完整清单）

### 13.1 4b-α \[已交付 commit `f69dfe0`]

实现项：

- [x] 引入 `CG_TYPE_SPEC`、`UserSpec` / `UserSpecMember` 注册表
- [x] fat 值 typedef + witness typedef 发射（`cg->headers` / `cg->type_defs`）
- [x] (T, S) witness 缓存 + thunk + 表 → `cg->witness_defs`，前向声明 → `cg->fn_protos`
- [x] 对象 → spec 强制转换（compound literal 包装）
- [x] spec 方法分派
- [x] spec 形参（按值借用）
- [x] spec 局部声明 + 作用域退出（subject-shortcut 清理）
- [x] spec 返回值（subject-shortcut transfer）
- [x] 仅支持单模块、`TYPE_OWN_METHOD` witness 来源；其他来源明确报错延后
- [x] smoke：`spec_object_param.ff`

### 13.2 4b-β \[本步要做]

按以下顺序执行，便于每步独立通过回归：

1. ~~value-model §3 / §4 / §5 落地（与 4b-β 强相关章节）~~。**已交付**（[feng-value-model-delivered.md](./feng-value-model-delivered.md) layer 封顶；codegen 已为每个 object-form spec 自动 emit `FengSpecAgg__M__S` + slot table + panic stub init fn；§7.2 / §7.4 helpers 就位）。
2. ~~把 4b-α 的 subject-shortcut 清理切换到 `feng_aggregate_release` + `FengSpecAgg__M__S` 描述符。~~ **已交付**（local cleanup / init borrowed retain / return borrowed retain 三处替换；smoke `spec_object_local.ff`）。
3. ~~spec 字段（§4.3 thunk + §9.5 lvalue / 写 / `release_children`），含 value-model §7.2 / §7.4。~~ **已交付**（`UserSpecMember.is_var` + 字段 getter/setter slot；`cg_ensure_witness_instance` 发 `get_<f>` / `set_<f>` thunk；`cg_emit_member` 与 `cg_emit_assign` member 分支接入 spec 接收者 → `recv.witness->get_<f>(recv.subject)` / `set_<f>`；smoke `spec_object_field.ff`。aggregate-typed spec 字段在注册期显式拒绝并标注 4b-γ）。
4. ~~spec 默认零值（§6 全套：隐藏 subject 类型、默认 witness、init 函数实体替换 panic stub、绑定到 `FengSpecAgg__M__S.default_init`）。~~ **已交付**（每个 object-form spec emit 隐藏 subject struct + `FengTypeDescriptor` + `release_children` + managed_fields 元数据 + factory `<spec>__new_subject()`；默认 witness thunk —— 字段 getter 直读 subject 字段、var 字段 setter 走 `feng_assign`、方法 thunk 忽略参数返回 `cg_default_value_expr` 的默认值；init fn 替换 panic stub 为真实分配并绑定默认 witness；`let s: Spec;` 路径切换到 `feng_aggregate_default_init(&s, &FengSpecAgg__M__S)`；smoke `spec_object_default.ff`）。
5. ~~spec 等值（§7 + 消费 `SpecEquality` sidecar）。~~ **已交付**（`cg_emit_binary` 头部新增 `feng_semantic_lookup_spec_equality` 早出分支：先把两侧 owns_ref 临时 materialise 到本地以让 aggregate cleanup 正常退役，再 emit `(bool)(L.subject ==/!= R.subject)`；非 spec 比较走原有路径无回归；smoke `spec_equality.ff`）。
6. smoke：~~`spec_object_field.ff`~~ **已交付** / ~~`spec_object_default.ff`~~ **已交付** / ~~`spec_equality.ff`~~ **已交付**。
7. 全量回归。

### 13.3 4b-γ \[随后]

1. ~~value-model §7.3 数组元素三分类落地~~。**已交付**（`feng_array_new_kinded` + cycle collector 三分支 + 6 项单测）。
2. ~~spec 数组发码（§9.6）~~：**已交付** — array literal / 默认零值 / index 写入三处分支到 `feng_array_new_kinded(..., FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &FengSpecAgg__M__S, sizeof(struct FengSpecValue__M__S), n)` + `feng_aggregate_assign`；语义层支持 array-literal 元素的 `User → Named` 自动 coercion（`expr_matches_expected_type_ref` / `record_object_spec_coercion_site_if_applicable` 数组递归）。
3. spec 返回值移动路径完整化，走 `feng_aggregate_take`。
4. fit-method 来源 witness（§5.2 第三行）。
5. smoke：~~`spec_array.ff`~~ ✅ / `spec_return.ff` / `spec_fit_witness.ff`。
6. 全量回归。

### 13.4 Phase 2

callable-form spec、`@fixed` spec、跨 TU 符号导出、devirtualization。

## 14 验收标准

实现完成时（4b-α + β + γ 全部落地）必须满足：

- `spec` 类型可出现在参数、返回值、成员字段、局部绑定、数组元素中。
- object-form `spec` 的 coercion、字段访问、方法调用、默认 witness、相等比较均正确发码。
- object-form `spec` 默认零值不为 `NULL`，每次默认初始化都创建新实例。
- `spec` 值比较不因 coercion 次数改变语义。
- `FengManagedHeader` / cycle collector 主体代码 / 单指针原语 **零修改**。
- 无任何 `feng_spec_*` runtime helper，无 spec 专用 type tag，无 spec 专用 cleanup chain。
- 所有 spec 值生命周期路径走 [feng-value-model-delivered.md](./feng-value-model-delivered.md) §5 的五个聚合 API；不存在对 spec 值手写 `feng_release(.subject)` 的残留。
- 所有现有 smoke 与单测零回归。

## 15 非目标与禁止项

明确禁止：

- **再以 box 形态承载 object-form `spec`**（含 `FengManagedHeader`、`FengTypeTag`、独立托管对象等任何变体）。
- 运行时通过全局 registry 动态判断"某对象是否满足某个 spec"。
- 通过字段 offset 直读 spec 字段。
- 以共享单例实现 `spec` 默认 witness。
- 因 fit 可见性复杂而把契约选择推迟到运行时。
- 在 spec emit 路径上引入"绕过 value-model API"的运行时 shortcut（4b-α 的 subject-shortcut 是过渡，必须按 §11.3 在 4b-β/γ 内退役）。

## 16 4b-β 首切片建议

为保证可独立通过回归，4b-β 第一刀建议只覆盖：

1. value-model §3 描述符 + §5 五 API 的 codegen 接入（不含 §7.2 / §7.3 / §7.4）。
2. spec 局部清理切换到 `feng_aggregate_release`。
3. 一个 smoke：将 `spec_object_param.ff` 复制为 `spec_object_param_2.ff` 加一行 `let m: Named = u;` 验证赋值路径无回归。

完成第一刀后再依次推 §6 默认零值、§7 等值、§9.5 字段。这样每一步都能独立 commit + 跑全量回归，避免一次性大改难定位回归源。
