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

### 1.4 本次不做

- C 指针进入 `fit`
- 多维数组目标形式（`T[][]`、`T[]![]` 等）
- 为 builtin / array 单独发明第二套运行时模型
- 任何基于 boxing 的过渡方案
- 将现有 `UserType.fitMethod()` direct-call 缺口留到"以后再修"

## 2. 设计原则（已讨论确认）

### 2.1 spec 运行时结构不动

以下组件对所有目标类型通用，本次不做任何改动：

| 组件 | C 结构 | 通用性 |
| --- | --- | --- |
| spec value struct | `struct { void *subject; const Witness *witness; }` | `subject` 是 `void*` 指针，通吃所有类型；数据始终只有一份 |
| witness vtable | `RetType (*fn)(void *_subject, ...)` | 函数指针签名统一为 `void*`，不感知具体类型 |
| coercion 包装 | `{ .subject = (void *)expr, .witness = &Witness }` | `expr` 先物化再强转 `void*`，类型无关 |
| witness 表 | `static const FengSpecWitness__M__S` | 编译期静态生成 |

### 2.2 仅需极小适配

| 适配点 | 改动内容 | 量级 |
| --- | --- | --- |
| **thunk 内部** | `void*` → 目标类型适配：用户 type → `(struct T*)`，标量 → `*(int32_t*)`，`string`/数组 → 受管引用指针 | 每种目标一个 cast |
| **witness sidecar key** | 从 `(type_decl, spec_decl)` 扩到统一 subject key（用户 type / builtin canonical name / 数组结构化签名） | 扩字段，不拆表，纯编译期 |
| **fit target 归一化** | 语义层新增归一化 helper，使 target 识别、self 绑定、孤儿判定都基于同一份归一化结果 | 新增一个内部结构 |

### 2.3 不做的方向

- 不拆 witness 表：继续单一 `lookup/reserve/append` API
- 不装箱：无 wrapper object、无额外 carrier struct、无 heap allocation
- 不引入第二种用户可见调用方式

## 3. 当前代码根因

现有主路径把 `fit` 目标、witness key 和 `self` 都绑定到了"用户 `type` 声明"这一前提上，导致 builtin / array 无法进入。

### 3.1 语义层：fit 目标硬限定为用户 type

- `validate_fit_declaration_contracts`（`src/semantic/analyzer.c`）要求 `resolve_type_ref_decl(target)` 必须是 `FENG_DECL_TYPE`。
- `resolve_declaration(FENG_DECL_FIT)` 只在目标是 `FENG_DECL_TYPE` 时设置 `context->current_type_decl`。

> 结果：builtin / array target 即使语法可表示，也进不了 fit 校验和 body 解析。

### 3.2 `self` 解析依赖 `current_type_decl`

- `resolve_self_member_expr`（`src/semantic/analyzer.c`）只在 `context->current_type_decl != NULL` 时走"当前目标类型成员"路径。

> 结果：builtin / array fit body 里的 `self`、`self.xxx()` 没有统一入口。

### 3.3 witness sidecar key 只接受 `(type_decl, spec_decl)`

- `FengSpecWitness`（`src/semantic/semantic.h`）以 `type_decl` 作为主体键。
- `lookup/reserve`（`src/semantic/spec_witnesses.c`）按 `(type_decl, spec_decl)` 线性查找。

> 结果：builtin / array 无法进入同一份 witness 缓存。**注意：sidecar key 是纯编译期概念，对运行时零影响。**

### 3.4 codegen 只认识对象 target

- `UserFit.target` 类型是 `const UserType *`（`src/codegen/codegen.c`）。
- `cg_ensure_witness_instance` 只接受 `const UserType *t`，`_subject` 强制转 `struct T *`。

> 结果：builtin / array target 没有 codegen 入口。

### 3.5 UserType.fitMethod() direct-call 缺口（已验证）

- fit 方法单独注册在 `UserFit.methods`，但 codegen 的 direct-call 发码路径按 `UserType.methods` 查找。
- `FENG_RESOLVED_CALLABLE_FIT_METHOD` 已在语义层标记，但 codegen 缺少对应的 static dispatch 发码分支。
- 已验证：`examples/src/hello_world.ff` 中 `user.say2(123)` 语义层能解析为 fit method，但 codegen 报 `type 'User' has no method 'say2'`。

> 结果：现有 `UserType.fitMethod()` direct call 已确认存在缺口，不是理论风险。

### 3.6 符号表不是阻断点

- 现有实现已有 BUILTIN 与 ARRAY type node。

> 结论：符号导出只需复用既有 type node，主要工作仍在语义层与 codegen。

## 4. 架构决策：spec 运行时结构已通用，无需改动

经代码审查确认，当前 spec 运行时结构对所有类型（用户 type、builtin 标量、`string`、数组）已经通用。

### 4.1 spec value struct

```c
// src/codegen/codegen.c:2150
struct FengSpecValue__M__S {
    void *subject;                          // 指针，非值嵌入
    const FengSpecWitness__M__S *witness;   // 静态 vtable 指针
};
```

`subject` 是 `void*` 指针，指向数据的唯一存储位置。所有类型都只有一份数据副本。对值类型的临时表达式，编译器物化到栈后再取其地址。

### 4.2 witness vtable 接口

```c
// src/codegen/codegen.c:2167
RetType (*method)(void *_subject, ...);     // 统一 void* 接口
```

函数指针签名统一，不感知具体类型。不需要为内建类型新增 vtable 布局。

### 4.3 coercion 包装

```c
// src/codegen/codegen.c:4251
((struct FengSpecValue){ .subject = (void *)expr, .witness = &Witness })
```

`expr` 先通过 `cg_materialize_to_local` 物化到局部变量，再强转 `void*`。逻辑与类型无关。

### 4.4 唯一变动点：thunk 内部类型适配

thunk 是唯一需要感知目标类型的位置：

```c
// 用户类型（现有）
RetType thunk(void *_subject, ...) {
    struct T *_self = (struct T *)_subject;
    return T__fit__method(_self, ...);
}

// 内建标量（新增）
RetType thunk(void *_subject, ...) {
    return i32__fit__method(*(int32_t*)_subject, ...);
}

// string / 数组（新增）
RetType thunk(void *_subject, ...) {
    return feng_string__fit__method((feng_string_ref*)_subject, ...);
}
```

每种目标类型仅一行 cast 差异，其余结构完全一致。

### 4.5 witness sidecar key 是纯编译期概念

`FengSpecWitness.type_decl` 是编译期语义分析用的查找键，不进入任何生成代码。扩展 key 的表达能力对运行时零影响。

## 5. 实现步骤

### 步骤1：语义层 fit target 归一化 + UserType direct-call 修复

> **目标**：让 analyzer 用一个统一结构描述"当前 fit 目标是什么"，并立即修复现有 direct-call 缺口。

- [ ] 新增归一化 helper（建议 `resolve_fit_target(...)`），统一识别三类目标：用户 `type`、builtin、array。
- [ ] builtin 统一转成 canonical name；array 拆出元素类型引用、rank、可写标记。
- [ ] `validate_fit_declaration_contracts` 改为依赖归一化结果。
- [ ] **同时**：在 codegen 补齐 `FENG_RESOLVED_CALLABLE_FIT_METHOD` 的 direct-call 静态分派发码分支。

**验收**：

- `user.say2(...)` 直接生成对 fit 方法静态符号的调用，不再报 `type 'User' has no method 'say2'`。
- analyzer 只在一个位置决定 fit target 种类。

### 步骤2：fit target 作用域与 self 绑定

> **目标**：让 `self` 在三类 target 上都能正确推断类型。

- [ ] 为 fit target 解析补轻量"fit target scope"。
- [ ] 对用户 `type`：继续沿用 `current_type_decl`。
- [ ] 对 builtin / array：补 `current_fit_target` 或等价上下文。
- [ ] 对 `fit T[]` / `fit T[]!`：将 `T` 压入 fit 局部类型参数作用域，作用域不泄漏。
- [ ] 调整 `resolve_self_member_expr`，让 builtin / array 走统一 self 入口。

**验收**：

- `self` 在用户 type、builtin 标量、string、array 四类 target 上都能稳定推断出正确类型。
- 数组 `T` 在整个 fit 声明内可见且不泄漏。

### 步骤3：spec 满足性与孤儿导出

> **目标**：让 builtin / array target 能正确判定 spec 满足性与导出规则。

- [ ] 拆开"目标有无自有实例成员"与"目标是不是用户 type"。
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

### 步骤5：codegen direct-call 归一化

> **目标**：三类 direct-call 均收敛到同一编译期静态分派模型，无运行时查表。

- [ ] 抽开 `UserFit.target` 的"只能是 `UserType *`"假设。
- [ ] 新增统一 fit method 发码入口：对象走现有 `cg_emit_user_method`，builtin / array 走新分支。
- [ ] `string` / 数组的 `self` 沿用现有受管引用表示；builtin 标量的 `self` 沿用原生 C 类型表示。
- [ ] 不引入 synthetic object、wrapper struct 或新 runtime helper API。

**验收**：

- 所有 direct call 生成代码为直接静态函数调用，不存在运行时散列查找、动态分派、间接跳转或 witness 表查询。
- 生成代码中 `it.some()` 调用成本不高于 `some(it)`。

### 步骤6：witness thunk 类型适配

> **目标**：让 spec 视角的 witness 调用支持所有目标类型。

- [ ] 将 `cg_ensure_witness_instance` 从"只接受 `UserType`"推广到接受统一 subject target。
- [ ] 对对象 target：继续 `(struct T *)_subject`。
- [ ] 对 builtin 标量：只做一次原生类型解引用。
- [ ] 对 `string` / 数组：只做一次现有引用表示取指针。
- [ ] thunk 直接调用 fit 方法实现，不经 boxing 或中间 wrapper。

**验收**：

- 抽象 spec 调用只有一层 witness 间接调用。
- witness 表编译期静态生成，代码中无运行时散列查找或动态分派。

### 步骤7：符号导出

> **目标**：`.ft` 正确导出 builtin / array fit target。

- [ ] builtin 标量 / `string` 目标：复用 BUILTIN type node。
- [ ] `T[]` / `T[]!` 目标：复用 ARRAY type node，保留元素类型引用与可写位图。
- [ ] 数组元素类型引用 `T`：通过 TYPE_PARAM_REF / ARRAY 组合导出，不拍平文本。

**验收**：

- `.ft` 不新增 top-level type kind。
- consumer 能区分 builtin fit target 与 array fit target。

### 步骤8：测试与全量回归

> **目标**：覆盖所有新增路径，保证原有功能不回归。

- [ ] semantic 用例：`fit i32`、`fit string`、`fit T[]`、`fit T[]!`、孤儿导出正反例。
- [ ] codegen 用例：`UserType.fitMethod()` direct call、builtin 标量 direct call、`string` direct call、array direct call、对应 witness thunk。
- [ ] smoke 用例：
  - `type User { ... }` + `fit User { fn say2(...) ... }` + `user.say2(...)`（验证现有 direct-call 修复）
  - `fit i32 { ... }` + 标量 direct call
  - `fit string: Spec { ... }` + spec witness 调用
  - `fit T[] { fn slice(...) ... }` + `fit T[]! { fn readonly() ... }`
- [ ] 每步完成后执行全量回归（`make test`）。

**验收**：

- `make test` 全量通过。
- 原有用户 `type` fit、witness、generic method 不回归。
- 生成代码中无 boxing、wrapper object、额外 carrier、运行时类型查询、方法选择或 witness 表散列查找。
- 所有 direct call 调用开销 ≤ 等价自由函数调用。
- witness 表编译期静态生成。

## 6. 执行顺序

```text
步骤1: fit target 归一化 + UserType direct-call 修复
  │
步骤2: fit target 作用域与 self 绑定
  │
步骤3: spec 满足性与孤儿导出
  │
步骤4: witness sidecar key 统一化
  │
步骤5: codegen direct-call 归一化
  │
步骤6: witness thunk 类型适配
  │
步骤7: 符号导出
  │
步骤8: 测试与全量回归
```

## 7. 交付约束

- 每一步先更新对应 `docs/` 或 `dev/` 文档，再改代码，最后补测试。
- 实现必须优先消除"只能处理 `type_decl`"这一根因，而非沿现有路径堆叠 builtin / array 特判。
- 现有 `UserType.fitMethod()` direct-call 缺口修复必须与 builtin / array fit 支持同批完成。
- 任何时候都不能用 boxing 作为过渡方案。
- spec 运行时结构（value struct、vtable、coercion 包装）保持不动。
- 若后续要放开更高 rank 数组 target 或其他内建类型 target，必须先更新规范再实现。
