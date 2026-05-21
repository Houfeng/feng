# Callable-Form Spec 匹配策略优化

> 状态：已实施  
> 日期：2026-05-21  
> 关联规范：[docs/feng-spec.md](../docs/feng-spec.md)、[docs/feng-function.md](../docs/feng-function.md)

> 2026-05-21 更新：最终交付规则以 [docs/feng-spec.md](../docs/feng-spec.md) 为准。本文早期草案中的 callable-form `spec` 值间“结构可赋值”方案已收敛为：不同 callable-form `spec` 不允许隐式结构匹配；仅当实例化后签名完全一致时允许显式转换；未绑定的顶层函数、方法值、lambda 进入 callable-form `spec` 位置时仍保持结构匹配。
>
> 2026-05-21 补充：实例化后签名完全一致的 callable-form `spec` 显式转换必须是零转发的目标视角切换。`(BSpec)a` 不得生成 `FengCallableRewrap__...` 适配器，不得分配新的 wrapper/closure，且 `b(...)` 的每次调用开销必须小于等于 `a(...)`。

## 1. 背景

当前 callable-form spec 的满足判断采用**纯结构匹配**：任何参数和返回形状匹配的可调用值都被接受，无需声明意图。

```c
// src/semantic/analyzer.c 现有注释
/* Callable-form spec satisfaction is structural: any function whose
 * parameter and return shape matches the spec is accepted, no declaration
 * of intent required. */
```

这意味着两个签名完全相同但语义不同的 callable-form spec 之间可以互相赋值，编译器无法区分。

```feng
spec CelsiusToFahrenheit(c: float): float;
spec KilogramsToPounds(kg: float): float;

fn convert_impl(v: float): float { return v * 1.8 + 32.0; }

let c2f: CelsiusToFahrenheit = convert_impl;   // ✓ 结构匹配
let k2p: KilogramsToPounds = c2f;              // ✓ 当前：结构匹配通过
                                                // ✗ 期望：语义错误！温度转换 ≠ 重量转换
```

## 2. 目标

**混合名义/结构匹配**：

| 源表达式类型 | 目标类型 | 匹配方式 | 说明 |
|------------|---------|---------|------|
| 已绑定到 callable-form spec 的值 | callable-form spec | **名义匹配** | 必须同一 spec 声明 |
| 未绑定的顶层函数 | callable-form spec | **结构匹配** | 签名形状一致即可 |
| 未绑定的方法值 | callable-form spec | **结构匹配** | 签名形状一致即可 |
| Lambda 表达式 | callable-form spec | **结构匹配** | 签名形状一致即可 |

"绑定到 spec"指：该值的类型已被编译器确定为某个具名 callable-form spec（通过 `let v: SpecName = ...` 声明、参数类型标注等方式）。

**设计类比**：这与 object-form spec 的规则形成对称——object-form 中 `type→spec` 走名义（需 `fit` 声明），callable-form 中 `spec→spec` 也走名义（需同一 spec），而未绑定的函数/lambda 则像结构字面量一样走形状匹配。

## 3. 语义定义

### 3.1 名义匹配规则（spec ↔ spec）

当源表达式类型和目标类型**均为** callable-form spec 时：

- **[必须]** 两个 spec 必须为同一声明（同一 `FengDecl`），泛型实例化后也须为同一实例化结果
- **[必须]** 下列情况视为"同一 spec"：
  - 直接引用同一 `spec Name` 声明
  - 泛型实例化 `Spec<T>` 与 `Spec<T>` 且类型实参语义相等
- **[禁止]** 仅因签名结构相同而接受不同 spec 间的转换
- **[禁止]** spec 继承链中的父子 spec 互相赋值（callable-form spec 间不存在继承关系，但如有 `spec A: B` 则 A 不可赋值给 B）

### 3.2 结构匹配规则（函数/方法/lambda → spec）

当源表达式是**未绑定到 spec 的可调用值**（顶层函数、方法值、lambda）时：

- **[必须]** 保持现有结构匹配行为不变
- **[必须]** 源可调用值的参数个数、参数类型、参数顺序、返回类型与目标 spec 完全一致
- **[必须]** 每个参数类型和返回类型通过 `type_refs_semantically_equal` 判定
- **[建议]** 结构匹配通过后记录 `SpecCoercionSite`，codegen 按需生成适配器

### 3.3 泛型 callable-form spec

```feng
spec Transform<T>(x: T): T;

fn double(x: int): int { return x * 2; }
fn identity<T>(x: T): T { return x; }

let t1: Transform<int> = double;     // 结构匹配：函数 → spec ✓
let t2: Transform<int> = identity;  // 结构匹配：泛型函数实例化 → spec ✓
let t3: Transform<int> = t1;        // 名义匹配：Transform<int> → Transform<int> ✓
let t4: Transform<float> = t1;      // 名义匹配：Transform<float> ≠ Transform<int> ✗
```

**关键细节**：`identity` 是泛型顶层函数，赋值给 `Transform<int>` 时先实例化再结构匹配，走"函数→spec"路径。而 `t1` 已是 `Transform<int>` 类型，赋值给 `Transform<float>` 时走"spec→spec"路径，名义匹配拒绝。

### 3.4 通过局部变量间接引用

```feng
spec A(x: int): int;
spec B(x: int): int;

fn foo(x: int): int { return x; }

let a: A = foo;   // 结构匹配：函数 → spec ✓
let b: B = a;     // 名义匹配：a 已绑定到 A，A ≠ B ✗
let c: A = a;     // 名义匹配：同一 spec ✓
```

即使 `a` 的底层来源是函数 `foo`，一旦 `a` 的类型被确定为 `A`，后续所有对该值的类型判断都以 `A` 为准，不再回溯其原始来源。

### 3.5 参数传递

```feng
fn use_mapper(m: Mapper): void { ... }

spec Mapper(x: int): int;

fn double(x: int): int { return x * 2; }
let m: Mapper = double;

use_mapper(double);  // 结构匹配：函数 → spec ✓
use_mapper(m);       // 名义匹配：spec → spec，同一 spec ✓
```

参数传递规则与赋值一致。

## 4. 实现策略

### 4.1 关键函数与变更位置

| 文件 | 函数 | 变更 |
|------|------|------|
| `src/semantic/analyzer.c` | `inferred_expr_type_matches_type_ref` | 在 callable spec 分支增加名义判断 |
| `src/semantic/analyzer.c` | `function_type_refs_have_equal_signature` | 不变（继续服务函数→spec 结构匹配） |
| `src/semantic/analyzer.c` | `resolve_expr_callable_value` | 不变（IDENTIFIER 分支已有类型区分） |

### 4.2 `inferred_expr_type_matches_type_ref` 修改方案

当前逻辑（简化）：

```
if type_refs_semantically_equal(src, dst) → true   // 完全同一
if function_type_refs_have_equal_signature(...) → true  // 结构匹配
if type_ref_satisfies_spec_type_ref(...) → true     // object spec 满足
```

修改后逻辑：

```
if type_refs_semantically_equal(src, dst) → true   // 完全同一

// ★ 新增：callable spec 之间的名义判断
if both_are_callable_form_specs(src, dst):
    → false  // 既不同一，则拒绝（名义匹配）

if function_type_refs_have_equal_signature(...) → true  // 函数→spec 结构匹配
if type_ref_satisfies_spec_type_ref(...) → true         // object spec 满足
```

**判断"双方均为 callable-form spec"**：

```c
const FengDecl *src_decl = resolve_type_ref_decl(context, src_ref);
const FengDecl *dst_decl = resolve_type_ref_decl(context, dst_ref);
if (decl_is_function_type(src_decl) && decl_is_function_type(dst_decl)) {
    // 源类型已是具名 callable spec，目标也是 callable spec
    // 前面 type_refs_semantically_equal 已判定不同一
    // → 名义匹配：拒绝
    return false;
}
```

### 4.3 不影响现有结构匹配路径

`resolve_expr_callable_value` 中的函数重载解析、方法值解析、lambda 匹配均**不受影响**。这些路径处理的是"未绑定到 spec 的可调用值"，继续走结构匹配。

关键：`resolve_expr_callable_value` 中处理 IDENTIFIER 时，先查局部变量（`local_entry`），如果找到且其类型是 callable spec，会调用 `inferred_expr_type_matches_type_ref`，此时走新增的名义判断。如果没有找到局部变量，才会查函数重载集，走结构匹配。这与需求完全一致。

### 4.4 诊断信息

当名义匹配失败时（两个不同 callable spec 但签名相同），建议给出明确诊断：

```
error: cannot assign value of spec 'CelsiusToFahrenheit' to 'KilogramsToPounds'
  note: callable-form specs use nominal matching; only the same spec declaration
        is accepted, even if parameter and return types are structurally identical
  note: consider wrapping the underlying function directly:
        let k2p: KilogramsToPounds = convert_impl;
```

## 5. 影响分析

### 5.1 破坏性变更

- **现有代码可能受影响**：如果代码依赖两个签名相同但名称不同的 callable spec 之间可以互相赋值，将编译失败
- **缓解措施**：修改为直接引用底层函数（重新走结构匹配），而非通过已绑定到另一个 spec 的变量

### 5.2 兼容性

- 函数→spec、方法→spec、lambda→spec 的结构匹配**完全不变**
- object-form spec 的名词匹配**完全不变**
- ABI `@abi spec` 互操作**不受影响**
- 泛型 callable spec 的实例化与匹配**逻辑一致**

### 5.3 性能影响

- 无负面影响。名义匹配比结构匹配更简单（仅比较 decl 指针），实际减少了一次参数逐位比较
- 在双方均为 callable spec 时提前返回 false，避免走入 `function_type_refs_have_equal_signature` 的完整签名比较

## 6. 测试要点

### 6.1 应通过的用例

```feng
// 1. 结构匹配：函数 → spec（保持）
spec Mapper(x: int): int;
fn double(x: int): int { return x * 2; }
let m: Mapper = double;  // ✓

// 2. 结构匹配：lambda → spec（保持）
let m2: Mapper = (x: int) -> x + 1;  // ✓

// 3. 名义匹配：同一 spec（保持）
let m3: Mapper = m;  // ✓ 同一 spec

// 4. 参数传递：函数 → spec（保持）
fn use_mapper(m: Mapper): void {}
use_mapper(double);  // ✓

// 5. 泛型：结构匹配
spec Transform<T>(x: T): T;
let t: Transform<int> = double;  // ✓
let t2: Transform<int> = t;      // ✓ 同一实例化
```

### 6.2 应拒绝的用例

```feng
// 1. 名义匹配：不同 spec，相同签名
spec A(x: int): int;
spec B(x: int): int;
let a: A = double;
let b: B = a;  // ✗ A ≠ B

// 2. 名义匹配：通过参数传递不同 spec
fn use_b(b: B): void {}
use_b(a);  // ✗ A ≠ B

// 3. 名义匹配：泛型不同实例化
let t3: Transform<float> = t;  // ✗ Transform<float> ≠ Transform<int>
```

### 6.3 回归范围

- `make test` 全量回归
- 重点：所有现有 callable spec 相关测试（test/semantic/、test/codegen/、test/smoke/）
- 需关注：现有测试中是否有依赖 spec→spec 结构匹配的用例

## 7. 决策记录

- **2026-05-21**：用户决策——callable-form spec 之间改为名义匹配，未绑定到 spec 的函数/方法/lambda 保持结构匹配
- 设计类比：与 object-form spec 的名义匹配形成对称
- 不影响函数→spec、方法→spec、lambda→spec 的结构匹配路径
