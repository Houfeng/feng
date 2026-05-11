# Feng `fit` 调用优化开发指南

> 本文档是本次 `fit` 调用优化的开发指导文档，定义施工范围、设计原则、实现步骤与验收口径。
> 语言规则权威来源见 [docs/feng-fit-builtin-type-draft.md](../docs/feng-fit-builtin-type-draft.md)、[docs/feng-fit.md](../docs/feng-fit.md)、[docs/feng-builtin-type.md](../docs/feng-builtin-type.md)。
> 符号表导出规范见 [docs/feng-symbol-table.md](../docs/feng-symbol-table.md)。
> 本文只写实现方案与任务拆解，不重复规范定义。

## 1. 目标

### 1.1 核心目标

本次优化的目标是：**让 `fit` 调用在支持用户类型的同时，也支持内建类型（值类型和引用类型），并保证直接调用零开销、spec witness 调用编译期静态确定。**

### 1.2 支持的目标类型

| 类别 | 类型 | 值/引用 |
| --- | --- | --- |
| 用户类型 | `type User { ... }` | 引用（指针） |
| 内建标量 | `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` `f32` `f64` `bool` | 值 |
| 内建引用 | `string` | 引用 |
| 数组 | `T[]` `T[]!` | 引用 |

### 1.3 性能硬约束

- **直接调用**：`it.some()` 编译期完成全部分派决策，运行时开销不得大于等价自由函数调用 `some(it)`。不得在调用路径中插入任何运行时类型查询、方法选择或 witness 解析。
- **spec witness 调用**：witness 表必须在编译期静态确定，不得生成运行时散列查找、动态分派或 JIT 路径。
- **标量 direct-call 硬约束**：标量目标的 `it.some()` 必须与非标量 direct-call 走同级静态调用路径；不得出现运行时装箱、运行时查表或额外间接分派。
- **标量 spec 视角硬约束**：标量进入 spec 视角时允许在 coercion site 产生 subject 封装；之后的 spec 调用路径必须与非标量 spec 调用保持同一开销模型（`subject + witness + 单层 thunk`），不得引入额外运行时层级。
- **泛型单态化硬约束**：`Set<int>` 等标量泛型实例必须在编译期完成 wrapper/实例单态化；泛型直接调用路径不得产生运行时装箱，其开销必须与非标量泛型实例一致。

### 1.4 当前切片

- 当前新增的稳定 subject 承载问题只收敛到**内建标量**。
- `string` 与数组本身就是受管引用，继续沿用现有 subject 指针路径，不新增箱对象。
- 只有标量形成**可逃逸的一等 object-form spec 值**时，才需要额外的稳定 subject owner。

### 1.5 本次不做

- C 指针进入 `fit`
- 多维数组目标形式（`T[][]`、`T[]![]` 等）
- 为 builtin / array 单独发明第二套运行时模型
- 用户可见的 boxing 语义，或 direct-call 路径上的任何装箱

### 1.6 分步交付原则

- 每一步必须能独立进入实现与验证，不依赖后续步骤才能形成可复核结果。
- 语义目标归一化、`self` 作用域、witness sidecar、运行时 subject 承载、codegen、符号导出分批推进，不在同一批次里混合多个抽象层的大改。
- 每一步的验收只覆盖当前批次新增能力，不把后续批次的能力当作前置成功条件。
- `UserType.fitMethod()` direct-call 当前能力必须保留独立回归守护，不得在后续批次退化。

## 2. 设计原则（已讨论确认）

### 2.1 fat spec 的外层 ABI 保持不动

以下组件的**外层 ABI 与主路径职责**对所有目标类型保持通用；本次不改变 fat spec 的两字段外层布局，也不改变 witness 静态生成模型：

| 组件 | C 结构 | 通用性 |
| --- | --- | --- |
| spec value struct | `struct { void *subject; const Witness *witness; }` | 保持两字段胖值 ABI；`subject` 始终指向当前 spec 值的稳定 subject 承载 |
| witness vtable | `RetType (*fn)(void *_subject, ...)` | 函数指针签名统一为 `void*`，不感知具体类型 |
| coercion 包装 | `{ .subject = (void *)expr, .witness = &Witness }` | `subject` + `witness` 的包装模型保持不变 |
| witness 表 | `static const FengSpecWitness__M__S` | 编译期静态生成 |

### 2.2 仅需局部适配

| 适配点 | 改动内容 | 量级 |
| --- | --- | --- |
| **thunk 内部** | `void*` → 目标类型适配：用户 type → `(struct T*)`，标量 → 从借用地址或 `FengScalarBox` 取原生值，`string`/数组 → 受管引用指针 | 每种目标一个专门化解包 |
| **witness sidecar key** | 从 `(type_decl, spec_decl)` 扩到统一 subject key（用户 type / builtin canonical name / 数组结构化签名） | 扩字段，不拆表，纯编译期 |
| **fit target 归一化** | 语义层新增归一化 helper，使 target 识别、self 绑定、孤儿判定都基于同一份归一化结果 | 新增一个内部结构 |
| **标量 spec subject 物化** | 非逃逸临时 spec 调用可借用局部物化地址；可逃逸 object-form spec 值统一进入 runtime-internal `FengScalarBox` | 一套内部承载机制 |

### 2.3 当前推荐的标量 subject 策略

- 对 `1.xx()` 这类 direct-call，仍然直接传原生值；不得经过 `FengScalarBox`。
- 对标量的临时 spec 调用，编译器可先物化局部变量，再把其地址作为 `_subject` 传给 witness thunk。
- 只有当标量真正形成**可逃逸的一等 object-form spec 值**时，才创建 runtime-internal `FengScalarBox`。
- `FengScalarBox` 只负责提供稳定 subject 存储；fit 方法实现与 `self` 语义永远只看原始标量值，不看箱对象。

### 2.4 不做的方向

- 不拆 witness 表：继续单一 `lookup/reserve/append` API
- 不引入用户可见的 wrapper object、第二套语言语义或 direct-call 装箱
- 不引入第二种用户可见调用方式

## 3. 当前代码根因

现有主路径把 `fit` 目标、witness key 和 `self` 都绑定到了"用户 `type` 声明"这一前提上，导致 builtin / array 无法进入；同时 fat spec 当前仍把 `subject` 当成托管指针槽位，标量 spec 的 stable subject ownership 尚未收口。

### 3.1 语义层：fit 目标硬限定为用户 type

- `validate_fit_declaration_contracts`（`src/semantic/analyzer.c`）要求 `resolve_type_ref_decl(target)` 必须是 `FENG_DECL_TYPE`。
- `resolve_declaration(FENG_DECL_FIT)` 只在目标是 `FENG_DECL_TYPE` 时设置 `context->current_type_decl`。

> 结果：builtin / array target 即使语法可表示，也进不了 fit 校验和 body 解析。

### 3.2 `self` 解析依赖 `current_type_decl`

- `resolve_self_member_expr`（`src/semantic/analyzer.c`）只在 `context->current_type_decl != NULL` 时走"当前目标类型成员"路径。

> 结果：builtin / array fit body 里的 `self`、`self.xxx()` 没有统一入口。

### 3.3 调用侧成员解析也仍绑定在 `type_decl`

- 当前成员访问与方法调用解析在 owner 不是 `FENG_DECL_TYPE` 时会直接拒绝继续走实例成员 / fit 成员路径。
- 现有 `find_fit_method_member_for_type(...)` 也以 `type_decl` 为入口。

> 结果：builtin / array 的 `it.some()` 不是只差 codegen 发码，语义层 call-site member resolution 也需要放开。

### 3.4 witness sidecar key 只接受 `(type_decl, spec_decl)`

- `FengSpecWitness`（`src/semantic/semantic.h`）以 `type_decl` 作为主体键。
- `lookup/reserve`（`src/semantic/spec_witnesses.c`）按 `(type_decl, spec_decl)` 线性查找。

> 结果：builtin / array 无法进入同一份 witness 缓存。**注意：sidecar key 是纯编译期概念，对运行时零影响。**

### 3.5 codegen 只认识对象 target

- `UserFit.target` 类型是 `const UserType *`（`src/codegen/codegen.c`）。
- `cg_ensure_witness_instance` 只接受 `const UserType *t`，`_subject` 强制转 `struct T *`。

> 结果：builtin / array target 没有 codegen 入口。

### 3.6 UserType.fitMethod() direct-call 当前能力（需保障不退化）

- 当前机制：fit 方法注册在 `UserFit.methods`，direct-call 发码通过 `FENG_RESOLVED_CALLABLE_FIT_METHOD` 走静态分派路径。
- 当前能力：`UserType.fitMethod()` 可正确 direct-call。
- 当前要求：该能力纳入独立回归守护，在本次 builtin / array 优化过程中持续保持。

> 结果：该路径不作为本轮前置阻断项，但属于必须持续通过的能力约束。

### 3.7 当前 fat spec 仍把 subject 当成托管指针槽位

- object-form spec 的 aggregate 描述符当前只登记一个 `FENG_SLOT_POINTER` 槽位，即 `subject`。
- spec coercion 的 object-form codegen 当前只接受对象值作为源。
- aggregate retain / release / cleanup 逻辑当前都把 `subject` 当作可直接 `feng_retain` / `feng_release` 的托管指针处理。

> 结果：若标量要形成可逃逸的一等 spec 值，不能只借用栈地址；必须先把其 stable subject ownership 收敛清楚。

### 3.8 符号表不是阻断点

- 现有实现已有 BUILTIN 与 ARRAY type node。

> 结论：符号导出只需复用既有 type node，主要工作仍在语义层与 codegen。

## 4. 架构决策：保持 fat spec 外层 ABI 不变，仅为标量 subject 增加稳定承载

经代码审查确认，当前 fat spec 的外层形状、witness 表模型与 direct-call 目标都应保持统一；需要新增的只是标量 subject 的稳定承载路径。

### 4.1 spec value struct

```c
// src/codegen/codegen.c:2150
struct FengSpecValue__M__S {
    void *subject;                          // 指针，非值嵌入
    const FengSpecWitness__M__S *witness;   // 静态 vtable 指针
};
```

`subject` 是 `void*` 指针，但其具体承载策略按目标类型细分：

- 用户 `type`：`subject` 指向现有对象。
- `string` / 数组：`subject` 指向现有受管引用对象。
- 标量 builtin：
  - 非逃逸临时 spec 调用：`subject` 可借用局部物化地址。
  - 可逃逸 object-form spec 值：`subject` 指向 runtime-internal `FengScalarBox`。

外层 fat spec 的两字段 ABI 不变。

### 4.2 witness vtable 接口

```c
// src/codegen/codegen.c:2167
RetType (*method)(void *_subject, ...);     // 统一 void* 接口
```

函数指针签名统一，不感知具体类型。不需要为内建类型新增 vtable 布局。

### 4.3 coercion 包装模型

```c
// 概念形状保持不变
((struct FengSpecValue){ .subject = subject_ptr, .witness = &Witness })
```

保持不变的是包装模型，不是 `subject_ptr` 的来源。对标量 builtin：

- 非逃逸临时 spec 调用可把 `subject_ptr` 设为局部物化地址。
- escaping spec 值则必须先创建 `FengScalarBox`，再让 `subject_ptr` 指向该 box。

### 4.4 direct-call 与 spec-call 共用同一个 fit 实现

fit 方法体看到的 `self` 永远是原始值或原始引用，不是箱对象。箱对象只存在于 subject 承载层。

```c
// 标量 fit 实现
static int32_t FengFit__i32__double(int32_t self) {
    return self * 2;
}

// direct-call: 1.double()
static int32_t direct_call(void) {
    return FengFit__i32__double(1);
}

// spec-call: ((S)1).double()
static int32_t witness_thunk(void *_subject) {
    const struct FengScalarBox *box = (const struct FengScalarBox *)_subject;
    return FengFit__i32__double(box->value.as_i32);
}
```

约束如下：

- direct-call 与 spec-call 共享同一个 fit 实现符号，不允许分裂成两套方法体。
- fit 方法实现层永远不接收箱对象作为 `self`。
- 箱对象只用于 escaping scalar spec 值的 stable subject ownership。

### 4.5 当前推荐的内部承载：`FengScalarBox`

当前阶段只需要覆盖内建标量；因此推荐的内部承载不是用户可见的 `ValueBox<T>`，而是一个 runtime-internal 的单一 `FengScalarBox`：

```c
struct FengScalarBox {
    FengManagedHeader header;
    FengBuiltinScalarKind kind; /* 调试/断言，可选 */
    union {
        bool as_bool;
        int8_t as_i8;
        int16_t as_i16;
        int32_t as_i32;
        int64_t as_i64;
        uint8_t as_u8;
        uint16_t as_u16;
        uint32_t as_u32;
        uint64_t as_u64;
        float as_f32;
        double as_f64;
    } value;
};
```

设计约束：

- `FengScalarBox` 是 runtime-internal 托管对象，不是用户可见 `type`。
- 所有 escaping scalar spec 值共用这一种箱对象机制，不为每个标量再造一套 runtime type。
- `kind` 仅用于断言、调试和测试，不参与正常 method dispatch。
- `union` payload 使用自然对齐成员，而不是字节数组 payload，避免当前阶段不必要的对齐复杂度。
- `FengScalarBox` 标记为非循环、无 managed fields，仅承担 stable subject ownership。
- spec 等值继续按 `subject` 身份比较：同一个 spec 值的拷贝共享同一 `subject` 指针；两个独立标量 coercion 各自产生独立 `FengScalarBox`。

### 4.6 fit 编译主路径保持统一，只在局部表示处分支

推荐的编译组织方式不是“用户类型一套、标量一套、托管引用一套”，而是：

- 一套统一的 fit target 归一化
- 一套统一的 member resolution / resolved callable 记录
- 一套统一的 direct-call emitter
- 一套统一的 witness emitter
- 仅在两个局部位置分支：
  - `self` 的底层表示（对象指针 / 托管引用 / 标量值）
  - spec subject 的物化策略（现有对象 / 借用临时标量地址 / `FengScalarBox`）

目标是：编译器只有一条 fit 主路径，不做两套互相平行的 fit 子系统。

### 4.7 其他语言在类似场景下通常也会装箱或使用等价稳定承载

- C# 中 `struct` 转到 `interface` 或 `object` 时，默认会发生 boxing。
- Go 的 interface 在值逃逸到抽象接口值时，运行时通常也需要一个稳定承载；具体是否分配由编译器和逃逸分析决定。
- Swift 的 existential container 对小值会优先内联，但超出内联容量或需要稳定拥有时仍会退化到堆分配承载。
- Rust 的静态泛型调用不装箱；但一旦进入拥有型 trait object 语义，通常也需要 `Box<dyn Trait>` 或等价 owner。

结论：当值类型要进入可逃逸的运行时抽象值（interface / existential / trait object）时，装箱或等价稳定容器是常见做法；Feng 当前阶段把这条成本限制在“escaping scalar spec 值”路径上是合理的。

### 4.8 witness sidecar key 是纯编译期概念

`FengSpecWitness.type_decl` 是编译期语义分析用的查找键，不进入任何生成代码。扩展 key 的表达能力对运行时零影响。

### 4.9 统一 subject key 的当前落点

批次 C 的主体键统一，先收敛为一份公共 compile-time `subject key` 载体，供 witness / relation / coercion sidecar 逐步复用：

- 用户 `type`：键值为目标 `decl`。
- builtin：键值为 canonical builtin name。
- array：键值为数组结构载体，至少包含 `element_type_ref`、`rank` 与逐层可写位。

当前阶段对 array key 的要求是：

- 先把数组键从单一 `type_decl` 假设中拆出来，进入统一 `subject key` 载体。
- witness sidecar 不再把 array target 退回到“只能是用户 `type`”的键模型。
- array key 的后续强化仍由 C4 收口；若当前实现还保留 `element_type_ref` 的借用表示，比较逻辑也必须基于结构字段而不是拍平文本。

该键仍然只服务编译期 sidecar 查找，不进入 fat spec ABI，不改变 witness 生成模型，也不引入运行时成本。

## 5. 交付批次

### 5.1 批次划分

| 批次 | 目标 | 主要交付物 | 依赖 |
| --- | --- | --- | --- |
| A | 先修稳现有 `fit` 主路径 | 当前 direct-call 能力的回归守护，语义层 `fit` 目标归一化结构（不改变行为） | 现有代码 |
| B | 放开 builtin / array 的语义入口 | builtin / array 目标识别、`self` 入口、调用侧成员解析、契约校验接线 | 批次 A |
| C | 收口 witness / subject 模型 | witness sidecar key 统一、主体键扩展、标量 subject 物化、`FengScalarBox` | 批次 B |
| D | 完成 codegen / 符号导出 / 回归 | direct-call 与 witness thunk 适配、符号导出、测试回归 | 批次 C |

### 5.2 详细步骤

### 批次 A：先修稳现有主路径

> **目标**：先把现有 `fit` 主路径修稳，再在同一份目标归一化结果上逐步放开 builtin / array。

- [x] A1：固化 `UserType.fitMethod()` direct-call 当前能力回归守护（smoke + codegen 形态检查），防止后续批次退化。
- [x] A2：新增归一化 helper（建议 `resolve_fit_target(...)`）与统一 target 数据结构，先只接入用户 `type` 主路径，不放开 builtin / array 行为。
- [x] A3：将 analyzer 内部“fit target 分类决策点”收敛到单一入口，确保后续批次只在该入口扩展 builtin / array。

**验收**：

- `user.say2(...)`/等价用例稳定走 direct-call，回归用例持续通过且无退化。
- analyzer 只在一个位置决定 fit target 种类。

### 批次 B：放开 builtin / array 的语义入口

> **目标**：让 `self` 在三类 target 上都能正确推断类型，并完成 spec 满足性与孤儿导出判定。

- [x] B1：为 fit target 解析补轻量“fit target scope”。
- [x] B0：在 A 批次统一 target 结构上放开 builtin canonical name 与 array 结构化 target（元素类型引用、rank、可写标记）识别。
- [x] B2：对用户 `type`：继续沿用 `current_type_decl`。
- [x] B3：对 builtin / array：补 `current_fit_target` 或等价上下文。
- [x] B4：对 `fit T[]` / `fit T[]!`：将 `T` 压入 fit 局部类型参数作用域，作用域不泄漏。
- [x] B5：调整 `resolve_self_member_expr`，让 builtin / array 走统一 self 入口。
- [x] B6：拆开“目标有无自有实例成员”与“目标是不是用户 type”。
- [x] B7：用户 `type` 继续复用现有自有成员 + visible fit members 满足性检查。
- [x] B8：builtin / array 自有成员集合视为空，只由 fit body 方法与可见 fit 关系组成可见实现面。
- [x] B9：孤儿适配导出判定通过归一化 target 计算 locality；所有内建类型目标按外部类型处理。
- [x] B10：`validate_fit_declaration_contracts` 改为依赖归一化 target 结果，并与调用侧 member resolution 使用同一份 target 语义。

**验收**：

- `self` 在用户 type、builtin 标量、string、array 四类 target 上都能稳定推断出正确类型。
- 数组 `T` 在整个 fit 声明内可见且不泄漏。
- `pu fit i32: LocalSpec` 可导出；`pu fit i32: ExternalSpec` 会被移除导出并输出肯定式提示。
- 用户 `type` 的原有孤儿规则不回归。

### 批次 C：收口 witness / subject 模型

> **目标**：让 builtin / array 使用同一套 witness 缓存，并把主体键扩展同步到所有语义侧数据结构。

- [x] C1：将 `FengSpecWitness` 的主体键从 `type_decl` 扩到统一 subject key：用户 type → `decl`，builtin → canonical name，array → 结构化签名。
- [x] C2：同步扩展 `FengSpecRelation` 与 `FengSpecCoercionSite` 的主体键，避免语义侧仍停留在 `type_decl`。
- [x] C3：保留单套 `lookup/reserve/append` API。
- [x] C4：数组 target 用结构化 key（元素类型 + rank + 逐层可写性），不用拍平文本或 AST 指针比较。
- [x] C5：coercion site 计算 subject key 时与 fit target 归一化逻辑一致（含 builtin/array 的 object-form spec coercion 入口）。
- [x] C6：在 runtime 内新增 `FengScalarBox`，使用单一托管对象类型承载全部内建标量。
- [x] C7：`FengScalarBox` 内部采用自然对齐的 `union` payload，而不是字节数组 payload。
- [x] C8：非逃逸临时 spec 调用继续允许借用局部物化地址，不分配 `FengScalarBox`。
- [x] C9：可逃逸 object-form spec 值（赋给 spec 局部、返回、存进字段/数组/闭包等）统一创建 `FengScalarBox`。
- [x] C10：`subject` 仍保持单一 `FENG_SLOT_POINTER` 槽位，不新增第三字段，不改 fat spec 两字段 ABI。
- [x] C11：补充 coercion site 分类规则（临时借用/可逃逸装箱）并在语义到 codegen 间打通标记传递。
- [x] C12：新增标量 spec 视角回归用例，覆盖“重复调用不重复封装”的行为断言。
- [x] C13：新增 codegen 形态断言：spec 调用路径固定为 `subject + witness + 单层 thunk`。

**验收**：

- builtin / array / user type 三类 target 的 witness materialization 时机一致。
- 语义层只有一份 witness 记录。
- fat spec 的外层 C ABI 仍为两字段按值 struct。
- direct-call 路径不创建箱对象。
- escaping scalar spec 值拥有稳定 subject 生命周期。
- 标量从具体值进入 spec 视角只在 coercion site 产生 subject 封装，不在每次 spec 方法调用时重复封装。
- 标量 spec 调用路径与非标量 spec 调用路径保持同构：`subject + witness` 与单层 thunk，不新增额外运行时查表或分派层。

### 批次 D：完成 codegen / 符号导出 / 回归

> **目标**：三类 direct-call 与 witness thunk 均完成适配，并用 `.ft` 与回归测试收口。

- [x] D1：抽开 `UserFit.target` 的“只能是 `UserType *`”假设。
- [x] D2：新增统一 fit method 发码入口：对象走现有 `cg_emit_user_method`，builtin / array 走新分支。
- [x] D3：`string` / 数组的 `self` 沿用现有受管引用表示；builtin 标量的 `self` 沿用原生 C 类型表示。
- [x] D4：direct-call 与 spec-call 共享同一个 fit 实现符号，不允许为标量再分裂出一套“箱版方法实现”。
- [x] D5：将 `cg_ensure_witness_instance` 从“只接受 `UserType`”推广到接受统一 subject target。
- [x] D6：对对象 target：继续 `(struct T *)_subject`。
- [ ] D7：对 builtin 标量：从借用地址或 `FengScalarBox` 中只做一次原生类型取值。
- [ ] D8：对 `string` / 数组：只做一次现有引用表示取指针。
- [ ] D9：thunk 直接调用 fit 方法实现，不经额外运行时查表或第二层 wrapper。
- [ ] D10：builtin 标量 / `string` 目标复用 BUILTIN type node；`T[]` / `T[]!` 目标复用 ARRAY type node，保留元素类型引用与可写位图。
- [ ] D11：数组元素类型引用 `T` 通过 TYPE_PARAM_REF / ARRAY 组合导出，不拍平文本。
- [x] D12：补齐 semantic、codegen、smoke 三层用例并执行 `make test`。
- [ ] D13：补齐泛型标量实例（如 `Set<int>`）的单态化发码路径，确保 direct-call 不触发运行时装箱。
- [ ] D14：新增泛型标量 direct-call 回归用例，对齐非标量泛型的调用形态。
- [ ] D15：新增 IR/代码形态检查：`Set<int>` direct-call 路径不得出现运行时装箱与运行时查表。
- [ ] D16：统一整理性能约束检查清单（direct-call/spec-call/泛型）并纳入 CI 回归脚本。

**验收**：

- 所有 direct call 生成代码为直接静态函数调用，不存在运行时散列查找、动态分派、间接跳转或 witness 表查询。
- 生成代码中 `it.some()` 调用成本不高于 `some(it)`。
- 抽象 spec 调用只有一层 witness 间接调用。
- witness 表编译期静态生成，代码中无运行时散列查找或动态分派。
- 标量 direct-call 不出现运行时装箱，调用成本与等价非标量 direct-call 保持同级。
- `Set<int>` 等标量泛型实例的 wrapper 在编译期单态化完成，直接调用路径不开启运行时装箱，开销与非标量泛型实例一致。
- `.ft` 不新增 top-level type kind。
- consumer 能区分 builtin fit target 与 array fit target。
- `make test` 全量通过。
- 原有用户 `type` fit、witness、generic method 不回归。
- direct-call 路径无 boxing、wrapper object、额外 carrier、运行时类型查询、方法选择或 witness 表散列查找。
- escaping scalar spec 值仅通过 runtime-internal `FengScalarBox` 提供稳定 subject 存储，且不改变语言层 `self` 语义。
- 所有 direct call 调用开销 ≤ 等价自由函数调用。

## 6. 执行顺序

```text
批次 A -> 批次 B -> 批次 C -> 批次 D
```

## 7. 交付约束

- 每一批次先更新对应 `docs/` 或 `dev/` 文档，再改代码，最后补测试，并执行全量回归。
- 实现必须优先消除“只能处理 `type_decl`”这一根因，而非沿现有路径堆叠 builtin / array 特判。
- `UserType.fitMethod()` direct-call 当前能力必须保持回归守护通过，严禁退化。
- direct-call 任何时候都不能通过 boxing 作为过渡方案。
- 标量 direct-call 任何时候都不能通过运行时装箱、运行时查表或额外动态分派作为过渡方案。
- 标量进入 spec 视角时的 subject 封装仅允许发生在 coercion site；spec 调用阶段的开销模型必须与非标量 spec 调用一致。
- 泛型标量实例（如 `Set<int>`）必须编译期单态化；直接泛型调用路径不得引入运行时装箱。
- fat spec 的外层运行时结构（value struct、vtable、coercion 包装模型）保持不动；escaping scalar spec 值允许引入 runtime-internal `FengScalarBox` 作为 stable subject owner。
- `FengSpecRelation` 与 `FengSpecCoercionSite` 的主体键扩展必须与 witness sidecar key 扩展同步完成，不能让语义侧继续停留在 `type_decl`。
- 若后续要放开更高 rank 数组 target 或其他内建类型 target，必须先更新规范再实现。
- `FengScalarBox` 作为新增运行时内部结构，必须先在 [docs/feng-fit-builtin-type-draft.md](../docs/feng-fit-builtin-type-draft.md) 中补充运行时约束定义，再进入批次 C 实现。
