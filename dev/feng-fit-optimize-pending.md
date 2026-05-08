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

### 1.4 当前切片

- 当前新增的稳定 subject 承载问题只收敛到**内建标量**。
- `string` 与数组本身就是受管引用，继续沿用现有 subject 指针路径，不新增箱对象。
- 只有标量形成**可逃逸的一等 object-form spec 值**时，才需要额外的稳定 subject owner。

### 1.5 本次不做

- C 指针进入 `fit`
- 多维数组目标形式（`T[][]`、`T[]![]` 等）
- 为 builtin / array 单独发明第二套运行时模型
- 用户可见的 boxing 语义，或 direct-call 路径上的任何装箱
- 将现有 `UserType.fitMethod()` direct-call 缺口留到"以后再修"

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

### 3.6 UserType.fitMethod() direct-call 缺口（已验证）

- fit 方法单独注册在 `UserFit.methods`，但 codegen 的 direct-call 发码路径按 `UserType.methods` 查找。
- `FENG_RESOLVED_CALLABLE_FIT_METHOD` 已在语义层标记，但 codegen 缺少对应的 static dispatch 发码分支。
- 已验证：`examples/src/hello_world.ff` 中 `user.say2(123)` 语义层能解析为 fit method，但 codegen 报 `type 'User' has no method 'say2'`。

> 结果：现有 `UserType.fitMethod()` direct call 已确认存在缺口，不是理论风险。

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

## 5. 实现步骤

### 步骤1：语义层 fit target 归一化 + 调用侧解锁 + UserType direct-call 修复

> **目标**：让 analyzer 用一个统一结构描述“当前 fit 目标是什么”，并同时打通调用侧 member resolution 与现有 direct-call 缺口。

- [ ] 新增归一化 helper（建议 `resolve_fit_target(...)`），统一识别三类目标：用户 `type`、builtin、array。
- [ ] builtin 统一转成 canonical name；array 拆出元素类型引用、rank、可写标记。
- [ ] `validate_fit_declaration_contracts` 改为依赖归一化结果。
- [ ] 调整成员访问 / 方法调用解析，让 builtin / array 也能通过统一 fit target 归一化查到 fit 方法。
- [ ] **同时**：在 codegen 补齐 `FENG_RESOLVED_CALLABLE_FIT_METHOD` 的 direct-call 静态分派发码分支。

**验收**：

- `user.say2(...)` 直接生成对 fit 方法静态符号的调用，不再报 `type 'User' has no method 'say2'`。
- builtin / array 的 `it.some()` 在语义层能进入统一 fit 方法解析路径。
- analyzer 只在一个位置决定 fit target 种类。

### 步骤2：fit target 作用域与 self 绑定

> **目标**：让 `self` 在三类 target 上都能正确推断类型。

- [ ] 为 fit target 解析补轻量“fit target scope”。
- [ ] 对用户 `type`：继续沿用 `current_type_decl`。
- [ ] 对 builtin / array：补 `current_fit_target` 或等价上下文。
- [ ] 对 `fit T[]` / `fit T[]!`：将 `T` 压入 fit 局部类型参数作用域，作用域不泄漏。
- [ ] 调整 `resolve_self_member_expr`，让 builtin / array 走统一 self 入口。

**验收**：

- `self` 在用户 type、builtin 标量、string、array 四类 target 上都能稳定推断出正确类型。
- 数组 `T` 在整个 fit 声明内可见且不泄漏。

### 步骤3：spec 满足性与孤儿导出

> **目标**：让 builtin / array target 能正确判定 spec 满足性与导出规则。

- [ ] 拆开“目标有无自有实例成员”与“目标是不是用户 type”。
- [ ] 用户 `type`：继续复用现有自有成员 + visible fit members 满足性检查。
- [ ] builtin / array：自有成员集合视为空，只由 fit body 方法与可见 fit 关系组成可见实现面。
- [ ] 孤儿适配导出判定通过归一化 target 计算 locality；所有内建类型目标按外部类型处理。

**验收**：

- `pu fit i32: LocalSpec` 可导出；`pu fit i32: ExternalSpec` 会被移除导出并输出肯定式提示。
- 用户 `type` 的原有孤儿规则不回归。

### 步骤4：witness sidecar key 统一化

> **目标**：让 builtin / array 使用同一套 witness 缓存，不另开表。

- [ ] 将 `FengSpecWitness` 的主体键从 `type_decl` 扩到统一 subject key：用户 type → `decl`，builtin → canonical name，array → 结构化签名。
- [ ] 保留单套 `lookup/reserve/append` API。
- [ ] 数组 target 用结构化 key（元素类型 + rank + 逐层可写性），不用拍平文本或 AST 指针比较。
- [ ] coercion site 计算 subject key 时与 fit target 归一化逻辑一致。

**验收**：

- builtin / array / user type 三类 target 的 witness materialization 时机一致。
- 语义层只有一份 witness 记录。

### 步骤5：标量 spec subject 物化与 `FengScalarBox`

> **目标**：在不改变 fat spec 外层 ABI 的前提下，为 escaping scalar spec 值提供稳定 subject 存储。

- [ ] 在 runtime 内新增 `FengScalarBox`，使用单一托管对象类型承载全部内建标量。
- [ ] `FengScalarBox` 内部采用自然对齐的 `union` payload，而不是字节数组 payload。
- [ ] 非逃逸临时 spec 调用继续允许借用局部物化地址，不分配 `FengScalarBox`。
- [ ] 可逃逸 object-form spec 值（赋给 spec 局部、返回、存进字段/数组/闭包等）统一创建 `FengScalarBox`。
- [ ] `subject` 仍保持单一 `FENG_SLOT_POINTER` 槽位，不新增第三字段，不改 fat spec 两字段 ABI。

**验收**：

- fat spec 的外层 C ABI 仍为两字段按值 struct。
- direct-call 路径不创建箱对象。
- escaping scalar spec 值拥有稳定 subject 生命周期。

### 步骤6：codegen direct-call 归一化

> **目标**：三类 direct-call 均收敛到同一编译期静态分派模型，无运行时查表。

- [ ] 抽开 `UserFit.target` 的“只能是 `UserType *`”假设。
- [ ] 新增统一 fit method 发码入口：对象走现有 `cg_emit_user_method`，builtin / array 走新分支。
- [ ] `string` / 数组的 `self` 沿用现有受管引用表示；builtin 标量的 `self` 沿用原生 C 类型表示。
- [ ] direct-call 与 spec-call 共享同一个 fit 实现符号，不允许为标量再分裂出一套“箱版方法实现”。
- [ ] 不引入 synthetic object、wrapper struct 或新 runtime helper API。

**验收**：

- 所有 direct call 生成代码为直接静态函数调用，不存在运行时散列查找、动态分派、间接跳转或 witness 表查询。
- 生成代码中 `it.some()` 调用成本不高于 `some(it)`。

### 步骤7：witness thunk 类型适配

> **目标**：让 spec 视角的 witness 调用支持所有目标类型。

- [ ] 将 `cg_ensure_witness_instance` 从“只接受 `UserType`”推广到接受统一 subject target。
- [ ] 对对象 target：继续 `(struct T *)_subject`。
- [ ] 对 builtin 标量：从借用地址或 `FengScalarBox` 中只做一次原生类型取值。
- [ ] 对 `string` / 数组：只做一次现有引用表示取指针。
- [ ] thunk 直接调用 fit 方法实现，不经额外运行时查表或第二层 wrapper。

**验收**：

- 抽象 spec 调用只有一层 witness 间接调用。
- witness 表编译期静态生成，代码中无运行时散列查找或动态分派。

### 步骤8：符号导出

> **目标**：`.ft` 正确导出 builtin / array fit target。

- [ ] builtin 标量 / `string` 目标：复用 BUILTIN type node。
- [ ] `T[]` / `T[]!` 目标：复用 ARRAY type node，保留元素类型引用与可写位图。
- [ ] 数组元素类型引用 `T`：通过 TYPE_PARAM_REF / ARRAY 组合导出，不拍平文本。

**验收**：

- `.ft` 不新增 top-level type kind。
- consumer 能区分 builtin fit target 与 array fit target。

### 步骤9：测试与全量回归

> **目标**：覆盖所有新增路径，保证原有功能不回归。

- [ ] semantic 用例：`fit i32`、`fit string`、`fit T[]`、`fit T[]!`、孤儿导出正反例。
- [ ] codegen 用例：`UserType.fitMethod()` direct call、builtin 标量 direct call、`string` direct call、array direct call、对应 witness thunk、escaping scalar spec coercion。
- [ ] smoke 用例：
  - `type User { ... }` + `fit User { fn say2(...) ... }` + `user.say2(...)`（验证现有 direct-call 修复）
  - `fit i32 { ... }` + 标量 direct call
  - `fit string: Spec { ... }` + spec witness 调用
  - `fit T[] { fn slice(...) ... }` + `fit T[]! { fn readonly() ... }`
  - 标量转 spec 后赋给局部、返回、参与 `==` / `!=`、存进数组或对象字段
- [ ] 每步完成后执行全量回归（`make test`）。

**验收**：

- `make test` 全量通过。
- 原有用户 `type` fit、witness、generic method 不回归。
- direct-call 路径无 boxing、wrapper object、额外 carrier、运行时类型查询、方法选择或 witness 表散列查找。
- escaping scalar spec 值仅通过 runtime-internal `FengScalarBox` 提供稳定 subject 存储，且不改变语言层 `self` 语义。
- 所有 direct call 调用开销 ≤ 等价自由函数调用。
- witness 表编译期静态生成。

## 6. 执行顺序

```text
步骤1: fit target 归一化 + 调用侧解锁 + UserType direct-call 修复
  │
步骤2: fit target 作用域与 self 绑定
  │
步骤3: spec 满足性与孤儿导出
  │
步骤4: witness sidecar key 统一化
  │
步骤5: 标量 spec subject 物化与 FengScalarBox
  │
步骤6: codegen direct-call 归一化
  │
步骤7: witness thunk 类型适配
  │
步骤8: 符号导出
  │
步骤9: 测试与全量回归
```

## 7. 交付约束

- 每一步先更新对应 `docs/` 或 `dev/` 文档，再改代码，最后补测试。
- 实现必须优先消除“只能处理 `type_decl`”这一根因，而非沿现有路径堆叠 builtin / array 特判。
- 现有 `UserType.fitMethod()` direct-call 缺口修复必须与 builtin / array fit 支持同批完成。
- direct-call 任何时候都不能通过 boxing 作为过渡方案。
- fat spec 的外层运行时结构（value struct、vtable、coercion 包装模型）保持不动；escaping scalar spec 值允许引入 runtime-internal `FengScalarBox` 作为 stable subject owner。
- 若后续要放开更高 rank 数组 target 或其他内建类型 target，必须先更新规范再实现。
