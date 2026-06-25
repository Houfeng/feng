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

#### 4.1.1 AST 节点增加 `type` 字段

**改动文件**：`src/parser/parser.h`

`FengExpr` 结构体增加字段：

```c
struct FengExpr {
    FengToken token;
    FengExprKind kind;
    const FengTypeRef *type;  /* 语义层填充，NULL 表示未推导 */
    union { ... } as;
};
```

**设计要点**：
- 直接使用已有的 `FengTypeRef` 作为表达式类型表示，不引入新类型
- `FengTypeRef` 已能表达所有类型：内建标量（`FENG_TYPE_REF_NAMED` 单段，如 `"i32"`）、用户类型（多段 + 泛型参数）、数组（`FENG_TYPE_REF_ARRAY`）、指针（`FENG_TYPE_REF_POINTER`）
- 语义层完成贴合/推导后，将已有的或合成的 `FengTypeRef *` 挂到节点上
- codegen 直接用现有的 `cg_resolve_type(cg, expr->type, ...)` 解析为 `CGType`，无需新增辅助函数
- 生命周期：来自声明的 `FengTypeRef *`（如绑定类型注解、函数参数类型）借用 AST 生命周期；合成的 `FengTypeRef *` 由 `analysis_track_synthetic_type_ref` 管理，与 `FengSemanticAnalysis` 同生命周期

**先例**：`FengResolvedCallable`（59 行）已是语义分析结果存储在 AST 节点上的先例；`lambda.captures` 注释为 "filled by the semantic analyzer"；`for_stmt.iter_result_type_ref` 注释为 "Synthesized by the analyzer; lifetime managed by FengSemanticAnalysis.synthesized_type_refs"。

#### 4.1.2 所有贴合场景写入 `type`

语义层在所有字面量贴合成功时，将目标类型的 `FengTypeRef *` 挂到字面量 AST 节点的 `type`。codegen 统一读取，不再自行推导。

**写入点 1**：`expr_matches_expected_type_ref()`（15479 行）—— 覆盖绑定、参数、返回值

所有现有贴合场景（赋值/绑定、函数参数、返回值、数组元素、成员赋值）都经过此函数。在 `numeric_literal_adapts_to_target` 返回 true 时写入：

```c
// 现有代码（15479 行）
if (expr_is_pure_numeric_literal_expr_for_target_adaptation(expr) &&
    evaluate_constant_expr(context, expr, &value) &&
    (value.kind == FENG_CONST_INT || value.kind == FENG_CONST_FLOAT)) {
    if (numeric_literal_adapts_to_target(context, expr, expected_type_ref)) {
        // [新增] 贴合成功，将目标类型直接挂到字面量节点
        // expected_type_ref 来自声明（AST 生命周期），借用即可
        ((FengExpr *)expr)->type = expected_type_ref;
        return true;
    }
    return false;
}
```

覆盖的场景：

| 场景 | 调用链 |
|---|---|
| `let x: u32 = 1;` | `resolve_binding` → `expr_matches_expected_type_ref(1, u32)` |
| `f(10)` (`f(n: i32)`) | `resolve_call` → `expr_matches_expected_type_ref(10, i32)` |
| `return 123;`（返回 `i64`） | `validate_return_stmt` → `expr_matches_expected_type_ref(123, i64)` |
| `let a: i32[] = [1, 2];` | `expr_matches_expected_type_ref_when_inference_unknown` → 逐元素 `expr_matches_expected_type_ref` |
| `obj.value = 1;`（`value: i32`） | 成员赋值验证 → `expr_matches_expected_type_ref(1, i32)` |

**写入点 2**：`infer_expr_type()` 的 `FENG_EXPR_BINARY` 分支 —— 覆盖二元运算

二元运算不经过 `expr_matches_expected_type_ref`（无目标类型概念），贴合逻辑在 `infer_expr_type` 中独立处理（见下方 §4.1.3 伪代码）。

**无类型标注绑定**：`let x = 123;`

语义层将 `int` 解析为平台相关的规范名（`i32` 或 `i64`），合成 `FengTypeRef` 挂到字面量节点：

```c
// 在 resolve_binding 或 infer_expr_type 的绑定路径中
// 无类型标注时，字面量默认推导为 int，解析为平台规范名
if (binding->type == NULL && expr->kind == FENG_EXPR_INTEGER) {
    InferredExprType inferred = inferred_expr_type_builtin("int");
    FengTypeRef *synth = create_type_ref_from_inferred_type(&inferred, expr->token);
    if (synth != NULL) {
        resolver_track_synthetic_type_ref(context, synth);
        ((FengExpr *)expr)->type = synth;
    }
}
// 浮点字面量同理，默认推导为 double → f64
if (binding->type == NULL && expr->kind == FENG_EXPR_FLOAT) {
    InferredExprType inferred = inferred_expr_type_builtin("double");
    FengTypeRef *synth = create_type_ref_from_inferred_type(&inferred, expr->token);
    if (synth != NULL) {
        resolver_track_synthetic_type_ref(context, synth);
        ((FengExpr *)expr)->type = synth;
    }
}
```

#### 4.1.3 二元运算贴合逻辑

**改动文件**：`src/semantic/analyzer.c`

**改动位置**：`infer_expr_type()` 的 `FENG_EXPR_BINARY` 分支（约 14449 行）

在现有的 `inferred_expr_types_equal` 类型相等性检查**之前**，增加字面量贴合步骤：

```
infer_expr_type → FENG_EXPR_BINARY:
  1. infer_expr_type(left) → left_type       // 递归推导
  2. infer_expr_type(right) → right_type      // 递归推导
  3. [新增] 如果 right 是纯数值字面量且 left_type 是已确定的标量类型：
     求值字面量，检查是否适配 left_type，适配则：
     a. 以 left_type 替换 right_type
     b. 将 left_type 对应的 FengTypeRef 挂到 right 节点的 type 字段
  4. [新增] 否则如果 left 是纯数值字面量且 right_type 是已确定的标量类型：
     求值字面量，检查是否适配 right_type，适配则：
     a. 以 right_type 替换 left_type
     b. 将 right_type 对应的 FengTypeRef 挂到 left 节点的 type 字段
  5. [现有] inferred_expr_types_equal(left_type, right_type) → 返回结果类型
```

**伪代码**（插入在步骤 1-2 之后、步骤 5 之前）：

```c
/* 二元运算字面量贴合：一侧为纯数值字面量，对侧为已确定标量类型时，
 * 字面量贴合到对侧类型。贴合结果挂到字面量节点的 type 字段，
 * 供 codegen 直接读取，codegen 无需任何分析逻辑。 */
if (expr_is_pure_numeric_literal_expr_for_target_adaptation(expr->as.binary.right) &&
    inferred_expr_type_is_numeric(left_type)) {
    if (numeric_literal_fits_inferred_target(context, expr->as.binary.right, left_type)) {
        right_type = left_type;
        FengTypeRef *synth = create_type_ref_from_inferred_type(&left_type, expr->token);
        if (synth != NULL) {
            resolver_track_synthetic_type_ref(context, synth);
            ((FengExpr *)expr->as.binary.right)->type = synth;
        }
    }
} else if (expr_is_pure_numeric_literal_expr_for_target_adaptation(expr->as.binary.left) &&
           inferred_expr_type_is_numeric(right_type)) {
    if (numeric_literal_fits_inferred_target(context, expr->as.binary.left, right_type)) {
        left_type = right_type;
        FengTypeRef *synth = create_type_ref_from_inferred_type(&right_type, expr->token);
        if (synth != NULL) {
            resolver_track_synthetic_type_ref(context, synth);
            ((FengExpr *)expr->as.binary.left)->type = synth;
        }
    }
}
```

**写入 AST 的先例**：`(FengExpr *)expr` 去 const 写入在语义层已有先例（18194 行、19649 行）。

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

**与现有 `numeric_literal_adapts_to_target` 的关系**：

- 已有的 `numeric_literal_adapts_to_target()`（15405 行）接受 `const FengTypeRef *target`，用于绑定/参数传递等场景（目标类型来自声明的 `FengTypeRef`）
- 新增的 `numeric_literal_fits_inferred_target()` 接受 `InferredExprType target_type`，用于二元运算场景（目标类型来自对侧操作数的推导结果 `InferredExprType`）
- 两者核心逻辑相同（求值 → 范围检查），但目标类型来源不同，无法直接复用；如需消除重复，可将核心逻辑提取为接受 `const char *canonical` 的内部函数，由两者分别调用

**设计要点**：

- **改动位置选择 `infer_expr_type` 而非 `validate_binary_expr`**：`infer_expr_type` 是递归函数，贴合逻辑在类型推导阶段生效，任意嵌套深度的二元表达式（如 `n + 1 > 3`）逐层自然贴合；`validate_binary_expr` 内部依赖 `infer_expr_type` 获取子表达式类型，若贴合仅在验证阶段，内层推导返回 unknown 将导致外层无法贴合
- **贴合是单向的**：字面量贴合到对侧，不是两侧互相贴合
- **两侧均为字面量时不触发贴合**：各自默认推导，类型一致即通过
- **贴合失败（超出范围）时**：保持默认推导类型，后续 `binary_expr_types_are_valid` 按类型不匹配报错，不新增错误码
- **位移运算符（`<<`/`>>`）**：类型推导阶段贴合后，右操作数获得与左操作数相同的标量类型；验证阶段 `validate_integer_shift_rhs_range` 正常工作，无需额外协调
- **`inferred_expr_type_is_numeric` 的 `pointer_size` 问题**：该函数当前硬编码 `pointer_size=0`（5719 行），`int` 始终解析为 `i32`。对二元运算贴合无功能影响——`i32` 和 `i64` 均为数值类型，`is_numeric` 判断结果一致；`numeric_literal_fits_inferred_target` 内部使用 `context->pointer_size` 获取正确的规范名，范围检查正确

**复用现有基础设施**：

- `expr_is_pure_numeric_literal_expr_for_target_adaptation()`：判断表达式是否为纯数值字面量（定义在 15372 行，需添加前向声明）
- `inferred_expr_type_is_numeric()`：判断对侧是否为数值类型
- `inferred_expr_type_builtin_canonical_name()`：从 `InferredExprType` 中提取标准名
- `evaluate_constant_expr()`：求值字面量的编译期值（已有前向声明，4202 行）
- `integer_literal_fits_canonical_target()`：纯函数，检查整数值是否在目标类型范围内（定义在 14892 行，需添加前向声明）

### 4.2 codegen 层改动

**改动文件**：`src/codegen/codegen.c`

**背景**：当前标量字面量在所有场景均固定发射为 `int64_t`/`f64`（11073 行 `cg_emit_literal`），不感知语义层确定的类型。绑定时靠 C 编译器隐式截断（`int8_t x = (int64_t)5;`），函数参数靠 C 隐式转换，二元运算靠 `cg_unify_numeric`（11288 行）提升。能运行但发码不干净。

**原则**：codegen 只负责发码，不做任何分析。语义层已将贴合结论以 `FengTypeRef *` 挂到 AST 节点的 `type` 字段，codegen 统一读取。不引入新的辅助函数——codegen 已有的 `cg_resolve_type()`（6636 行）直接将 `FengTypeRef *` 解析为 `CGType`，完全复用。

**改动**：`cg_emit_literal()`（11066 行）读取 `type`

```c
static bool cg_emit_literal(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);

    if (e->kind == FENG_EXPR_INTEGER) {
        if (e->type != NULL) {
            CGType *target = NULL;
            if (cg_resolve_type(cg, e->type, &e->token, &target) &&
                target != NULL && cgtype_is_integer(target->kind)) {
                // 按 type 的宽度和符号发射
                // 例如 type=i32 → (int32_t)INT32_C(5)
                // 例如 type=u32 → (uint32_t)UINT32_C(1)
                ...
                return true;
            }
        }
        // type 为 NULL（未推导）时，保持现有默认行为
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "(int64_t)INT64_C(%" PRId64 ")", e->as.integer);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_I64);
        return out->c_expr && out->type;
    }
    if (e->kind == FENG_EXPR_FLOAT) {
        if (e->type != NULL) {
            CGType *target = NULL;
            if (cg_resolve_type(cg, e->type, &e->token, &target) &&
                target != NULL && cgtype_is_float(target->kind)) {
                // 按 type 发射
                // 例如 type=f32 → (float)3.14f
                // 例如 type=f64 → 保持现有 (%a)
                ...
                return true;
            }
        }
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "(%a)", e->as.floating);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_F64);
        return out->c_expr && out->type;
    }
    // ... bool, string 等不变
```

**无需新增辅助函数**：`cg_resolve_type`（6636 行）已完整实现 `FengTypeRef *` → `CGType *` 的解析，覆盖所有内建标量类型（通过 `k_builtin_types` 查表）。

**`cg_emit_binary()` 无需修改**：`cg_emit_binary`（11306 行）调用 `cg_emit_expr` 发射两侧操作数，`cg_emit_expr` → `cg_emit_literal` 读取 `type`。二元运算自然受益。

**效果对比**：

```c
// 改动前：所有场景
let x: u32 = 1;     // uint32_t x = (int64_t)INT64_C(1);     ← 靠 C 隐式截断
f(10);              // f((int64_t)INT64_C(10));              ← 靠 C 隐式截断
return 123;         // return (int64_t)INT64_C(123);         ← 靠 C 隐式截断
n + 1;              // ((int64_t)n + (int64_t)INT64_C(1))    ← 靠 C 优化

// 改动后：语义层写入 type，codegen 通过 cg_resolve_type 读取
let x: u32 = 1;     // uint32_t x = (uint32_t)UINT32_C(1);   ← 正确
f(10);              // f((int32_t)INT32_C(10));              ← 正确
return 123;         // return (int64_t)INT64_C(123);         ← 正确（返回 i64）
n + 1;              // ((int32_t)n + (int32_t)INT32_C(1))    ← 正确
let x = 123;        // int32_t x = (int32_t)INT32_C(123);    ← 正确（32 位平台）
let x = 12.5;       // double x = (double)(12.5);            ← 正确
```

### 4.3 规范文档改动

**改动文件**：`docs/feng-builtin-type.md`

- §2 贴合定义：扩展覆盖二元运算场景
- §6 规则：新增一条 `[必须]` 规则，明确二元运算中字面量贴合的条件与范围检查
- §7 编译期：新增一条编译器要求，明确二元运算字面量贴合的实现义务

## 5. 影响范围

- 规范文档：`docs/feng-builtin-type.md`（贴合定义扩展）
- AST 节点：`src/parser/parser.h`（`FengExpr` 增加 `const FengTypeRef *type` 字段）
- 编译器语义层：`src/semantic/analyzer.c`（`expr_matches_expected_type_ref` 贴合成功时写入 `type`；`infer_expr_type` 二元分支增加贴合及 `type` 写入；无类型标注绑定写入合成 `FengTypeRef`；新增 `numeric_literal_fits_inferred_target` 辅助函数）
- 编译器 codegen 层：`src/codegen/codegen.c`（`cg_emit_literal` 读取 `type`，通过现有 `cg_resolve_type` 解析为 `CGType`，按目标类型发射；无新增辅助函数）
- 测试：现有 45 个 smoke 测试失败预期全部恢复；可能需要新增专门测试用例

## 6. 已决问题

- **表达式类型用 `const FengTypeRef *` 而非自定义结构体**：`FengTypeRef` 已能表达所有类型（内建标量、用户类型、数组、指针），无需引入 `FengExprType` 等新类型。复用已有类型系统和 `cg_resolve_type` 函数，codegen 无需新增辅助函数
- **AST 是 parser 和 semantic 共用的单一数据结构**：parser 创建，semantic 补齐语义字段（`type`、`resolved_callable`、`captures` 等）。这是已有模式，本次遵循
- **贴合结果内联写入 AST 节点**：与 `resolved_callable`、`captures` 一致，一一对应且 codegen 必读的信息放 AST 节点上，不放 sidecar 表

## 7. 待决问题

- 二元运算字面量贴合是否应同步覆盖 if/match/try 表达式内部分支的字面量贴合？（本次不处理，后续评估）
- `FengExpr` 增加 `const FengTypeRef *type` 字段（8 字节/节点，64 位平台）的内存开销是否可接受？

## 8. 开发 TODO

> 分为 4 个独立步骤，每步完成后全量回归通过。步骤 1-3 仅影响语义层（codegen 不读 `type`，行为不变）；步骤 4 改 codegen + 新增测试。

### 8.1 规范文档（docs/feng-builtin-type.md）

- [x] §2 语义：在「数值字面量的目标类型贴合」条目中，扩展贴合定义，补充二元运算场景
- [x] §6 规则：新增一条 [必须] 规则，明确二元运算中字面量贴合的条件（一侧为纯数值字面量、对侧为已确定标量类型）与范围检查要求
- [x] §7 编译期：新增一条编译器要求，明确二元运算字面量贴合的实现义务

### 8.2 步骤 1：AST 增加 `type` 字段

仅增加字段，不填充、不读取，行为无变化。

- [x] `src/parser/parser.h`：`FengExpr` 结构体增加 `const FengTypeRef *type` 字段（顶层，NULL 表示未推导）
- [x] `src/parser/parser.c`：确认 `calloc` 创建的 `FengExpr` 节点 `type` 默认值为 NULL（`calloc` 置零，指针为 NULL，无需显式初始化）
- [x] 全量回归测试，确认无新增失败

### 8.3 步骤 2：现有贴合场景填充 `type`

语义层在已有的贴合路径上写入 `type`。codegen 尚不读取，行为不变。

- [x] `expr_matches_expected_type_ref()`（15479 行）：`numeric_literal_adapts_to_target` 返回 true 时，将 `expected_type_ref`（已有 `FengTypeRef *`，借用 AST 生命周期）直接挂到字面量节点的 `type`（覆盖绑定、参数、返回值、数组元素、成员赋值等所有现有贴合场景）
- [x] 无类型标注绑定（`let x = 123;`）：语义层通过 `inferred_expr_type_builtin("int")` + `create_type_ref_from_inferred_type` 合成 `FengTypeRef`，clone 后通过 `analysis_track_synthetic_type_ref` 管理生命周期（确保 ResolveContext 释放后仍存活），挂到字面量节点的 `type`
- [x] 无类型标注浮点（`let x = 12.5;`）：同理，通过 `inferred_expr_type_builtin("double")` 合成 `FengTypeRef`，clone + analysis_track 挂到字面量节点的 `type`
- [x] 全量回归测试，确认无新增失败

### 8.4 步骤 3：二元运算贴合

新增二元运算中的字面量贴合逻辑。codegen 尚不读取，行为不变。

- [x] 新增辅助函数 `numeric_literal_fits_inferred_target()`：求值字面量常量，检查是否适配目标 `InferredExprType`（参考已有的 `numeric_literal_adapts_to_target()` 15405 行，核心逻辑相同但目标类型来源不同）
- [x] 为 `expr_is_pure_numeric_literal_expr_for_target_adaptation()` 和 `integer_literal_fits_canonical_target()` 添加前向声明（两者定义在 `infer_expr_type` 之后）
- [x] `infer_expr_type()` 的 `FENG_EXPR_BINARY` 分支：在 `inferred_expr_types_equal` 之前插入字面量贴合步骤
  - [x] 右操作数为纯数值字面量且左操作数类型为已确定标量时，贴合右操作数类型到左操作数类型，并通过 `create_type_ref_from_inferred_type` + clone + `analysis_track_synthetic_type_ref` 合成 `FengTypeRef` 挂到右操作数节点的 `type`
  - [x] 左操作数为纯数值字面量且右操作数类型为已确定标量时，贴合左操作数类型到右操作数类型，同理合成 `FengTypeRef` 挂到左操作数节点的 `type`
  - [x] 两侧均为字面量时不触发贴合，各自默认推导
  - [x] 贴合时通过 `evaluate_constant_expr` 求值、`integer_literal_fits_canonical_target` 检查范围，不适配时保持默认推导类型
- [x] 位移运算符（`<<`/`>>`）：确认贴合后 `validate_integer_shift_rhs_range` 正常工作（预期无冲突）
- [x] 全量回归测试，确认无新增失败

### 8.5 步骤 4：按类型正确发码 + 测试

codegen 读取 `type` 按实际类型发射，新增专项测试验证。

- [x] `cg_emit_literal()` 整型字面量分支：`expr->type != NULL` 时，通过 `cg_builtin_scalar_kind_from_type_ref` 直接查找内建标量类型（避免 `cg_resolve_type` 的副作用），按目标宽度和符号发射（如 `(int32_t)INT32_C(5)`、`(uint32_t)UINT32_C(1)`）；`type == NULL` 时保持现有 `(int64_t)INT64_C(...)` 默认行为
- [x] `cg_emit_literal()` 浮点字面量分支：`expr->type != NULL` 时，通过 `cg_builtin_scalar_kind_from_type_ref` 直接查找，按目标类型发射（`f32` → `(float)`，`f64` → 保持现有 `(%a)`）；`type == NULL` 时保持现有默认行为
- [x] 新增贴合发码专项测试（覆盖所有场景的正确发码）：
  - [x] 绑定（有类型注解）：`let x: u32 = 1;` → `(uint32_t)UINT32_C(1)`
  - [x] 绑定（有类型注解）：`let x: i8 = 5;` → `(int8_t)INT8_C(5)`
  - [x] 绑定（无类型标注）：`let x = 123;` → 按平台 `int` 宽度发射
  - [x] 绑定（无类型标注）：`let x = 12.5;` → `f64` 发射
  - [x] 函数参数：`f(10)` (`f(n: i32)`) → `(int32_t)INT32_C(10)`
  - [x] 返回值：`return 123;`（返回 `i64`）→ `(int64_t)INT64_C(123)`
  - [x] 成员赋值：`obj.value = 1;`（`value: i32`）→ `(int32_t)INT32_C(1)`
  - [x] 数组元素：`let a: i32[] = [1, 2];` → 各元素按 `i32` 发射
  - [x] 二元运算：`n + 1` (`n: i32`) → `(int32_t)INT32_C(1)`
  - [x] 二元运算嵌套：`n + 1 > 3` → 逐层按 `i32` 发射
  - [x] 范围越界：`let x: u8 = 256;` → 编译期报错
  - [x] 两侧均为字面量：`10 == 20` → 不贴合，各自默认推导
- [x] 全量回归测试，确认无新增失败

> **备注**：本次完成后，重新实施 `feng-scalar-alias-optimize.md` Task 6（`int` → 平台相关）时，原 §1.1 提到的 45 个 smoke 失败用例预期全部恢复通过。该验证属于 Task 6 的回归范围，不作为本文档的交付项。

## 9. 贴合类型推导优化

> 状态：草案  
> 日期：2026-06-25  
> 目标：修正 §4.1.3 实现中"验证在推导之前"的顺序缺陷，使推导与验证真正各司其职

### 9.1 问题

§4.1.3 把贴合逻辑放在 `infer_expr_type` 的 `FENG_EXPR_BINARY` 分支，但 `validate_binary_expr`（analyzer.c:6294）验证时调的是 `infer_expr_type(left/right)`——对**操作数**推导，不是对**二元表达式整体**推导，因此贴合永远不会在验证前触发。

以 `if n == 10 { }`（`n: i32`，64 位平台 `int = i64`）为例：

| 步骤 | 调用 | 结果 |
|---|---|---|
| 1·验证 | `validate_binary_expr` → `infer_expr_type(n)` / `infer_expr_type(10)` | `i32` / `i64`（默认，未贴合） |
| 2·验证 | `binary_expr_types_are_valid(i32, i64)` | false，报"类型不匹配" |
| 3·推导 | `validate_stmt_condition_expr` → `infer_expr_type(n == 10)` | 触发贴合，但错误已报 |

**验证在推导之前，验证会失败**。当前测试在 `int = i32` 下通过纯属巧合（字面量默认 `i32` 与 `n: i32` 恰好一致），Task 6 重新应用后 45 个 smoke 用例会继续失败。

### 9.2 设计原则

- **贴合是推导**：字面量适配对侧类型属于类型推导，不是验证
- **推导和验证各司其职**：两个独立函数，见名知意，不让一个函数隐含干另一个的事
- **先推导后验证**：验证必须基于"推导完成后的类型"，顺序不可颠倒
- **复用已有 `expr->type`**：`FengExpr` 已有 `type` 字段（注释"filled by the semantic analyzer; NULL means not inferred"），设计意图本来就是承载推导结果，不需新增字段

### 9.3 职责划分

| 函数 | 职责 | 不做 |
|---|---|---|
| `infer_expr_type` | 推导（含贴合），填 `expr->type` | 不报错 |
| `validate_binary_expr` | 验证，读 `expr->type` | 不调 `infer_expr_type` |

`resolve_expr` 二元分支显式两步：先推导后验证。若多处需要"先推后验"模式，可再包装 `infer_and_validate_binary`，但不让验证函数内部隐含调推导。

### 9.4 实现方案

#### 9.4.1 `infer_expr_type` 二元分支：推导时填 `expr->type`

analyzer.c:14475 `FENG_EXPR_BINARY` 分支，在确定结果类型后，把推导结果写入 AST 节点：

```c
case FENG_EXPR_BINARY: {
    InferredExprType left_type = infer_expr_type(context, expr->as.binary.left);
    InferredExprType right_type = infer_expr_type(context, expr->as.binary.right);
    /* ... 现有贴合逻辑（保留）... */

    /* [新增] 推导结果写入 AST，供验证阶段直接读 */
    fill_expr_type_from_inferred(context, expr->as.binary.left, left_type);
    fill_expr_type_from_inferred(context, expr->as.binary.right, right_type);
    /* 结果类型也写入，供外层二元读内层结果 */
    InferredExprType result_type = <switch 确定的结果>;
    fill_expr_type_from_inferred(context, expr, result_type);
    return result_type;
}
```

**新增辅助函数 `fill_expr_type_from_inferred`**：把 `InferredExprType` 转成 `const FengTypeRef *` 写入 `expr->type`。

| `InferredExprType` kind | 转换方式 |
|---|---|
| `BUILTIN` | 合成命名 `FengTypeRef`（单段），`analysis_track_synthetic_type_ref` 管理生命周期 |
| `TYPE_REF` | 借用已有 `type_ref`（AST 生命周期） |
| `DECL` / `LAMBDA` / `UNKNOWN` | 留 `expr->type = NULL`（二元运算中本就非法，验证自然失败） |

可复用现有的 `create_type_ref_from_inferred_type` + `clone_type_ref_for_inference` + `analysis_track_synthetic_type_ref` 路径（与 §4.1.2 无类型标注绑定的合成路径一致）。

#### 9.4.2 `validate_binary_expr`：纯验证，直接读 `expr->type`

analyzer.c:6294-6296，不再调 `infer_expr_type`：

```c
static bool validate_binary_expr(ResolveContext *context, const FengExpr *expr) {
    /* [改] 直接读推导阶段填的结果，不调 infer_expr_type */
    InferredExprType left_type  = expr->as.binary.left->type != NULL
        ? inferred_expr_type_from_type_ref(expr->as.binary.left->type)
        : inferred_expr_type_unknown();
    InferredExprType right_type = expr->as.binary.right->type != NULL
        ? inferred_expr_type_from_type_ref(expr->as.binary.right->type)
        : inferred_expr_type_unknown();
    /* ... 后续 binary_expr_types_are_valid 等验证逻辑不变 ... */
}
```

`validate_binary_expr` 零 `infer_expr_type` 调用，纯验证。`inferred_expr_type_from_type_ref` 把 `FengTypeRef *` 转回 `InferredExprType`，复用现有 `binary_expr_types_are_valid` 等比较函数。

#### 9.4.3 `resolve_expr` 二元分支：先推后验，显式两步

analyzer.c:20137-20200，在 `validate_binary_expr` 之前补一次 `infer_expr_type`：

```c
/* 两条路径（&& 和其他）都在验证前加推导 */
infer_expr_type(context, expr);              /* 步骤1：推导（含贴合，填 expr->type） */
if (!validate_binary_expr(context, expr)) {  /* 步骤2：验证（读 expr->type） */
    ...
}
```

`&&` 路径在 binding scope 内调用，与现有 `validate_binary_expr` 同作用域，不影响绑定可见性。

#### 9.4.4 `InferredExprType` 增加重构方向注释

analyzer.c:99-113，在 `InferredExprType` 定义上方加注释：

```c
/* InferredExprType represents the type of an expression during semantic
 * analysis. It exists as a separate structure from FengTypeRef for historical
 * implementation reasons: it carries direct pointers to FengDecl/FengExpr
 * (avoiding name resolution) and uses string slices for builtin types
 * (avoiding FengTypeRef synthesis).
 *
 * Future refactoring direction: unify with FengTypeRef. A type is a type,
 * regardless of how it was determined (annotated, declared, or inferred).
 * "Inferred" should describe the provenance, not be a separate type structure.
 * When unified, infer_expr_type will return const FengTypeRef *, expr->type
 * will be the single source of truth, and the BUILTIN/DECL/LAMBDA kinds will
 * be representable as FENG_TYPE_REF_NAMED (with an intern table for builtins
 * and function type declarations for lambdas). */
typedef struct InferredExprType {
    ...
} InferredExprType;
```

### 9.5 验证流程（修复后）

`if n == 10 { }`（`n: i32`，64 位 `int = i64`）：

| 步骤 | 函数 | 动作 | 结果 |
|---|---|---|---|
| 推导 | `infer_expr_type(n == 10)` | 贴合 `10→i32`，填 `n->type=i32`、`10->type=i32`、`(n==10)->type=bool` | 返回 `bool` |
| 验证 | `validate_binary_expr` | 读 `n->type`/`10->type` → `i32`/`i32` | 通过 |

嵌套场景 `n + 1 > 3`：内层 `infer_expr_type(n + 1)` 填 `(n+1)->type=i32`，外层 `validate_binary_expr(n+1 > 3)` 读 `(n+1)->type` 拿到贴合后的 `i32`，逐层自然成立。

越界场景 `n == 9999999999`（超出 `i32`）：贴合失败（范围检查不通过），`10->type` 未填，`validate_binary_expr` 读到 `i32`/`i64`（默认），报类型不匹配——与"贴合失败则按不贴合推导"的设计一致。

### 9.6 不改的部分

- `infer_expr_type` 二元分支的贴合逻辑（analyzer.c:14485-14525）保留，在新的推导调用中生效
- `binary_expr_types_are_valid` 接口不变（仍用 `InferredExprType`），待 `InferredExprType` 统一后一并改
- `expr_matches_expected_type_ref` 等绑定/参数/返回值路径不受影响（这些路径已通过 `expected_type_ref` 直接贴合）

### 9.7 未来重构方向：`InferredExprType` 统一

`InferredExprType`（5 种 kind）与 `FengTypeRef`（3 种 kind）存在类型模型二分，是设计气味：

| `InferredExprType` kind | `FengTypeRef` 对应 |
|---|---|
| `UNKNOWN` | `NULL` |
| `BUILTIN` | `NAMED`（单段，需 intern 表） |
| `TYPE_REF` | 直接 |
| `DECL` | `NAMED`（decl 名字，需解析回 decl） |
| `LAMBDA` | `NAMED`（函数类型 decl） |

统一后：
- `infer_expr_type` 返回 `const FengTypeRef *`，`expr->type` 成为唯一真源
- 消除 `InferredExprType` ↔ `FengTypeRef *` 转换摩擦
- `validate_binary_expr` 直接用 `FengTypeRef` 比较，不需 `inferred_expr_type_from_type_ref` 转回

本次只在 `InferredExprType` 上加注释说明方向，不执行统一。统一作为独立重构，需评估 `analyzer.c` 中几百处 `InferredExprType` 使用的迁移。

### 9.8 开发 TODO

#### 9.8.1 文档修订

- [x] §4.1.3 补充说明：`resolve_expr` 中先推后验的调用顺序（本文 §9.4.3 已记述）

#### 9.8.2 步骤 1：`infer_expr_type` 填 `expr->type`

- [ ] 新增 `fill_expr_type_from_inferred` 辅助函数（复用 `create_type_ref_from_inferred_type` + `analysis_track_synthetic_type_ref`）
- [ ] `FENG_EXPR_BINARY` 分支：在确定结果类型后，填 `left->type`、`right->type`、`expr->type`
- [ ] 全量回归测试

#### 9.8.3 步骤 2：`validate_binary_expr` 改为纯验证

- [ ] `validate_binary_expr`：移除 `infer_expr_type(left/right)` 调用，改为直接读 `expr->as.binary.left->type` / `right->type`，通过 `inferred_expr_type_from_type_ref` 转换
- [ ] 全量回归测试

#### 9.8.4 步骤 3：`resolve_expr` 先推后验

- [ ] `FENG_EXPR_BINARY` 分支两条路径（`&&` 和其他）：在 `validate_binary_expr` 前加 `infer_expr_type(context, expr)`
- [ ] 全量回归测试

#### 9.8.5 步骤 4：`InferredExprType` 注释

- [ ] analyzer.c:99-113 `InferredExprType` 定义上方加重构方向注释
- [ ] 全量回归测试
