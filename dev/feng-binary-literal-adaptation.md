# 标量二元运算字面量贴合

> 状态：草案  
> 日期：2026-06-24  
> 关联规范：[docs/feng-builtin-type.md](../docs/feng-builtin-type.md)、[docs/feng-expression.md](../docs/feng-expression.md)  
> 关联设计：[dev/feng-scalar-alias-optimize.md](./feng-scalar-alias-optimize.md)

## 1. 背景

Feng 的数值字面量贴合策略（docs/feng-builtin-type.md §2）当前覆盖以下场景：

| 场景 | 贴合 | 示例 |
|---|---|---|
| 赋值/绑定（有类型注解） | ✅ | `let x: i32 = 10;` |
| 函数/方法参数传递 | ✅ | `func f(n: i32); f(10)` |
| 返回值（有返回类型声明） | ✅ | `return 10;`（函数返回 `i32`） |
| 成员赋值 | ✅ | `obj.value = 1;`（`value: i32`） |
| 数组字面量元素（有目标类型） | ✅ | `let a: i32[] = [1, 2, 3];` |
| **二元运算** | **❌** | `n == 10`（`n: i32`） |

贴合的定义是"当数值字面量或纯字面量数值常量表达式**被绑定或传递到已确定的目标数值类型时**"。二元运算中没有"目标类型"概念，两侧各自独立推导类型，因此字面量贴合不覆盖。

### 1.1 Task 6 暴露的问题

`feng-scalar-alias-optimize.md` Task 6 将 `int` 从固定 `i32` 改为平台相关（64 位平台 → `i64`）。此前字面量默认推导为 `i32`，与 `i32` 变量比较碰巧类型一致；Task 6 后字面量默认推导为 `i64`，二元运算中 `n == 10`（`n: i32`）变为 `i32 == i64`，编译失败。

smoke 测试中 45/87 个用例因此失败，全部是二元运算中字面量与标量变量类型不匹配。

## 2. 主流语言对比

| 语言 | 二元运算字面量处理 | 机制 |
|---|---|---|
| **Go** | ✅ 贴合 | untyped constant，字面量无固定类型，在任何数值上下文无形贴合 |
| **Swift** | ✅ 贴合 | integer literal 通过 `ExpressibleByIntegerLiteral` 协议贴合到对侧类型 |
| **Rust** | ✅ 贴合 | integer literal 默认 `{integer}`（占位类型），在二元运算中贴合到对侧具体类型 |
| **Zig** | ✅ 贴合 | `comptime_int`，编译期整数无固定宽度，在运算时贴合 |
| **C/C++** | ❌ 不贴合 | `10` 固定为 `int`，与 `int64_t` 运算时靠隐式整数提升规则对齐 |

Go、Swift、Rust、Zig 四门现代语言全部支持二元运算中的字面量贴合，只是机制不同：

- Go/Swift/Zig：字面量本身无类型（untyped / literal protocol / comptime_int）
- Rust：字面量有占位类型 `{integer}`，在上下文确定后具体化
- C/C++：不靠贴合，靠隐式整数提升——Feng 明确不做隐式转换，此路径不适用

## 3. 方案

### 3.1 扩展贴合定义

在 `docs/feng-builtin-type.md` §2 的贴合定义中，增加二元运算场景：

> 当数值字面量或纯字面量数值常量表达式被绑定或传递到已确定的目标数值类型时，**或作为二元运算符的一侧操作数且对侧操作数为已确定的标量类型时**，按目标类型进行类型贴合。

### 3.2 贴合规则

二元运算中，当一侧为纯数值字面量（或纯字面量数值常量表达式），另一侧为已确定的标量类型（`i8`~`i64`、`u8`~`u64`、`f32`、`f64`）时：

1. 字面量贴合到对侧的标量类型
2. 贴合遵循现有的编译期范围检查（`integer_literal_fits_canonical_target`）
3. 贴合仅在编译期可确定字面量值时生效
4. 两侧均为字面量时各自默认推导，类型一致则通过，不触发贴合

### 3.3 不适用场景

- **字符串 `+` 拼接**：不涉及数值字面量贴合
- **布尔 `&&`/`||`**：不涉及数值字面量
- **位移 `<<`/`>>`**：右侧已有独立的范围验证逻辑（`validate_integer_shift_rhs_range`），字面量贴合可复用现有路径
- **非字面量表达式**：贴合不适用于已具备静态类型的表达式（如变量、函数调用返回值）

### 3.4 示例

```feng
let n: i32 = 5;

// Task 6 后（64 位平台 int → i64）
if n == 10 { }        // ✅ 10 贴合到 i32
if n + 1 > 3 { }      // ✅ (n + 1): 1 贴合到 n 的 i32，结果 i32；再 > 3: 3 贴合到 i32
let x: u8 = 200;
if x == 255 { }       // ✅ 255 贴合到 u8
if x == 256 { }       // ❌ 256 超出 u8 范围，编译期报错

// 两侧均为字面量
if 10 == 20 { }       // ✅ 两侧都推导为 int（i64），类型一致

// 非字面量不贴合
let a: i32 = 1;
let b: i64 = 2;
if a == b { }         // ❌ i32 != i64，不贴合，需显式转换
```

## 4. 实现方案

### 4.1 语义分析层改动

**改动文件**：`src/semantic/analyzer.c`

**改动位置**：`infer_expr_type()` 的 `FENG_EXPR_BINARY` 分支（约 14449 行）

在现有的 `inferred_expr_types_equal` 类型相等性检查**之前**，增加字面量贴合步骤：

```
infer_expr_type → FENG_EXPR_BINARY:
  1. infer_expr_type(left) → left_type       // 递归推导
  2. infer_expr_type(right) → right_type      // 递归推导
  3. [新增] 如果 right 是纯数值字面量且 left_type 是已确定的标量类型：
     求值字面量，检查是否适配 left_type，适配则以 left_type 替换 right_type
  4. [新增] 否则如果 left 是纯数值字面量且 right_type 是已确定的标量类型：
     求值字面量，检查是否适配 right_type，适配则以 right_type 替换 left_type
  5. [现有] inferred_expr_types_equal(left_type, right_type) → 返回结果类型
```

**伪代码**（插入在步骤 1-2 之后、步骤 5 之前）：

```c
/* 二元运算字面量贴合：一侧为纯数值字面量，对侧为已确定标量类型时，
 * 字面量贴合到对侧类型。递归自然生效，支持任意嵌套深度。 */
if (expr_is_pure_numeric_literal_expr_for_target_adaptation(expr->as.binary.right) &&
    inferred_expr_type_is_numeric(left_type)) {
    if (numeric_literal_fits_inferred_target(context, expr->as.binary.right, left_type)) {
        right_type = left_type;
    }
} else if (expr_is_pure_numeric_literal_expr_for_target_adaptation(expr->as.binary.left) &&
           inferred_expr_type_is_numeric(right_type)) {
    if (numeric_literal_fits_inferred_target(context, expr->as.binary.left, right_type)) {
        left_type = right_type;
    }
}
```

**新增辅助函数** `numeric_literal_fits_inferred_target`：

```c
static bool numeric_literal_fits_inferred_target(ResolveContext *context,
                                                 const FengExpr *literal_expr,
                                                 InferredExprType target_type) {
    const char *canonical = inferred_expr_type_builtin_canonical_name(
        target_type, context->pointer_size);
    FengConstValue value;
    bool target_is_float;

    if (canonical == NULL) return false;
    target_is_float = strcmp(canonical, "f32") == 0 || strcmp(canonical, "f64") == 0;
    if (!evaluate_constant_expr(context, literal_expr, &value)) return false;
    if (value.kind == FENG_CONST_INT) {
        return target_is_float || integer_literal_fits_canonical_target(value.i, canonical);
    }
    if (value.kind == FENG_CONST_FLOAT) {
        return target_is_float;
    }
    return false;
}
```

**设计要点**：

- **改动位置选择 `infer_expr_type` 而非 `validate_binary_expr`**：`infer_expr_type` 是递归函数，贴合逻辑在类型推导阶段生效，任意嵌套深度的二元表达式（如 `n + 1 > 3`）逐层自然贴合；`validate_binary_expr` 内部依赖 `infer_expr_type` 获取子表达式类型，若贴合仅在验证阶段，内层推导返回 unknown 将导致外层无法贴合
- **贴合是单向的**：字面量贴合到对侧，不是两侧互相贴合
- **两侧均为字面量时不触发贴合**：各自默认推导，类型一致即通过
- **贴合失败（超出范围）时**：保持默认推导类型，后续 `binary_expr_types_are_valid` 按类型不匹配报错，不新增错误码
- **位移运算符（`<<`/`>>`）**：类型推导阶段贴合后，右操作数获得与左操作数相同的标量类型；验证阶段 `validate_integer_shift_rhs_range` 正常工作，无需额外协调

**复用现有基础设施**：

- `expr_is_pure_numeric_literal_expr_for_target_adaptation()`：判断表达式是否为纯数值字面量（定义在 15372 行，需添加前向声明）
- `inferred_expr_type_is_numeric()`：判断对侧是否为数值类型
- `inferred_expr_type_builtin_canonical_name()`：从 `InferredExprType` 中提取标准名
- `evaluate_constant_expr()`：求值字面量的编译期值（已有前向声明，4202 行）
- `integer_literal_fits_canonical_target()`：纯函数，检查整数值是否在目标类型范围内（定义在 14892 行，需添加前向声明）

### 4.2 codegen 层改动

**改动文件**：`src/codegen/codegen.c`

**背景**：当前标量字面量在所有场景均固定发射为 `int64_t`/`f64`（11073 行），不感知语义层确定的类型。绑定时靠 C 编译器隐式截断（`int8_t x = (int64_t)5;`），函数参数靠 C 隐式转换，二元运算靠 `cg_unify_numeric` 提升。能运行但发码不干净，且二元运算贴合后两侧 CG 类型不一致会导致不必要的类型提升。

**改动 1**：`cg_emit_expr_for_expected_type()`（19634 行）增加数值字面量分支

当 `expected_type` 为标量 CG 类型且 `expr` 为整型/浮点字面量时，按 `expected_type` 发射对应宽度的 C 类型：

```c
// 新增分支（在现有的 array/tuple 分支之后，fallthrough 之前）
if (expected_type != NULL && expr != NULL && expr->kind == FENG_EXPR_INTEGER &&
    cgtype_is_integer(expected_type->kind)) {
    // 按 expected_type 的宽度和符号发射，例如 (int32_t)INT32_C(5)
    ...
}
if (expected_type != NULL && expr != NULL && expr->kind == FENG_EXPR_FLOAT &&
    cgtype_is_float(expected_type->kind)) {
    // 按 expected_type 发射，例如 (float)3.14f
    ...
}
```

此改动是通用的，所有已有的 `cg_emit_expr_for_expected_type` 调用点（绑定初始化、函数参数、数组元素等）统一受益，不再依赖 C 编译器隐式转换。

**改动 2**：`cg_emit_binary()`（11306 行）使用语义类型编译子表达式

```c
// 现有：
cg_emit_expr(cg, e->as.binary.left, &lr);
cg_emit_expr(cg, e->as.binary.right, &rr);

// 改为：通过 infer_expr_type 获取语义类型，按类型发射
CGType *common = cg_semantic_type_to_cg(cg, infer_expr_type(cg->context, e));
cg_emit_expr_for_expected_type(cg, e->as.binary.left, common, &lr);
cg_emit_expr_for_expected_type(cg, e->as.binary.right, common, &rr);
```

语义层贴合后，两侧类型已一致（如 `n + 1` 均为 `i32`），`cg_emit_expr_for_expected_type` 将字面量按 `i32` 发射，`cg_unify_numeric` 看到的两侧 CG 类型一致，无需提升。

**效果对比**：

```c
// 改动前：n: i32, n + 1
((int64_t)n + (int64_t)INT64_C(1))    // 盲目 i64，靠 C 编译器优化

// 改动后：
((int32_t)n + (int32_t)INT32_C(1))    // 按语义类型发码，干净正确
```

### 4.3 规范文档改动

**改动文件**：`docs/feng-builtin-type.md`

- §2 贴合定义：扩展覆盖二元运算场景
- §6 规则：新增一条 `[必须]` 规则，明确二元运算中字面量贴合的条件与范围检查
- §7 编译期：新增一条编译器要求，明确二元运算字面量贴合的实现义务

## 5. 影响范围

- 规范文档：`docs/feng-builtin-type.md`（贴合定义扩展）
- 编译器语义层：`src/semantic/analyzer.c`（`infer_expr_type` 二元分支增加贴合，新增 `numeric_literal_fits_inferred_target` 辅助函数）
- 编译器 codegen 层：`src/codegen/codegen.c`（`cg_emit_expr_for_expected_type` 增加数值字面量分支，`cg_emit_binary` 按语义类型编译子表达式）
- 测试：现有 45 个 smoke 测试失败预期全部恢复；可能需要新增专门测试二元运算字面量贴合的用例

## 6. 已决问题

（待决策后补充）

## 7. 待决问题

- 二元运算字面量贴合是否应同步覆盖 if/match/try 表达式内部分支的字面量贴合？（本次不处理，后续评估）

## 8. 开发 TODO

### 8.1 规范文档（docs/feng-builtin-type.md）

- [ ] §2 语义：在「数值字面量的目标类型贴合」条目中，扩展贴合定义，补充二元运算场景
- [ ] §6 规则：新增一条 [必须] 规则，明确二元运算中字面量贴合的条件（一侧为纯数值字面量、对侧为已确定标量类型）与范围检查要求
- [ ] §7 编译期：新增一条编译器要求，明确二元运算字面量贴合的实现义务

### 8.2 语义分析（src/semantic/analyzer.c）

- [ ] 新增 `numeric_literal_fits_inferred_target()` 辅助函数：求值字面量常量，检查是否适配目标 `InferredExprType`
- [ ] 为 `expr_is_pure_numeric_literal_expr_for_target_adaptation()` 和 `integer_literal_fits_canonical_target()` 添加前向声明（两者定义在 `infer_expr_type` 之后）
- [ ] `infer_expr_type()` 的 `FENG_EXPR_BINARY` 分支：在 `inferred_expr_types_equal` 之前插入字面量贴合步骤
  - [ ] 右操作数为纯数值字面量且左操作数类型为已确定标量时，贴合右操作数类型到左操作数类型
  - [ ] 左操作数为纯数值字面量且右操作数类型为已确定标量时，贴合左操作数类型到右操作数类型
  - [ ] 两侧均为字面量时不触发贴合，各自默认推导
  - [ ] 贴合时通过 `evaluate_constant_expr` 求值、`integer_literal_fits_canonical_target` 检查范围，不适配时保持默认推导类型
- [ ] 位移运算符（`<<`/`>>`）：确认贴合后 `validate_integer_shift_rhs_range` 正常工作（预期无冲突）

### 8.3 codegen（src/codegen/codegen.c）

- [ ] `cg_emit_expr_for_expected_type()`：增加整型字面量分支——当 `expected_type` 为整数 CG 类型时，按 `expected_type` 的宽度和符号发射对应 C 类型（如 `int32_t`/`INT32_C`）
- [ ] `cg_emit_expr_for_expected_type()`：增加浮点字面量分支——当 `expected_type` 为 `f32` 时发射 `(float)`，`f64` 时保持现有行为
- [ ] `cg_emit_binary()`：通过 `infer_expr_type` 获取二元表达式的语义类型，使用 `cg_emit_expr_for_expected_type` 编译两侧子表达式，替代当前的 `cg_emit_expr`

### 8.4 测试

- [ ] 新增二元运算字面量贴合专项测试用例（覆盖：算术运算、比较运算、位运算、范围越界报错、两侧均为字面量不贴合）
- [ ] 运行全量回归测试，确认无新增失败

> **备注**：本次完成后，重新实施 `feng-scalar-alias-optimize.md` Task 6（`int` → 平台相关）时，原 §1.1 提到的 45 个 smoke 失败用例预期全部恢复通过。该验证属于 Task 6 的回归范围，不作为本文档的交付项。
