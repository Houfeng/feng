# Task 6 测试失败根因分析

## 背景

Task 6（[相关文档](feng-scalar-alias-optimize.md)）将 `int` 改为平台相关别名（32 位 → `i32`，64 位 → `i64`）后，C 侧测试出现 5 个失败。其中 2 个被临时绕过，3 个尚未修复。本文档记录根因分析和修复方案。

---

## 一、被绕过的 2 个问题

### 问题 1：test_inferred_array_literal_binding_rejects_index_write_without_writable_layer

**测试源码**：
```feng
var items = [1, 2, 3];
items[0] = 4;
let first: int = items[0];
```

**预期行为**：
- 第 2 行 `items[0] = 4` 报错 "is not writable"（推断数组不可写）
- 第 3 行 `let first: int = items[0]` 应通过（类型匹配）

**实际行为**（64 位平台）：
- 第 2 行报错 "is not writable" ✓
- 第 3 行报错 "does not match expected type 'i64'" ✗

**根因**：

`type_ref_builtin_canonical_name` 函数（analyzer.c:4518）硬编码 `pointer_size=0`：

```c
static const char *type_ref_builtin_canonical_name(const FengTypeRef *type_ref) {
    // ...
    return canonical_builtin_type_name(type_ref->as.named.segments[0], 0U);
}
```

**详细调用链**：

1. 数组字面量 `[1, 2, 3]` 的类型推断：
   - `infer_expr_type` 对整数字面量返回 `inferred_expr_type_builtin("int")` (analyzer.c:15012)
   - `infer_array_literal_expr_type` 用此类型合成数组 type_ref (analyzer.c:14081)
   - 合成的 type_ref 的 segment 为 "int"（未规范化为 "i64"）

2. `let first: int = items[0]` 的类型检查：
   - 目标类型 `int` 经 AST 规范化后变为 "i64" ✓
   - 表达式 `items[0]` 的类型：
     - `resolve_indexed_array_element_type_ref` 返回数组的 inner type_ref (analyzer.c:14078)
     - inner type_ref 的 segment 仍为 "int"（推断时创建，未经 AST 规范化）

3. 类型比较 (`inferred_expr_type_matches_type_ref`, analyzer.c:5045)：
   ```c
   expr_builtin = canonical_builtin_type_name(expr_type.builtin_name, context->pointer_size);
   // → "i64" (pointer_size=8)
   
   target_builtin = type_ref_builtin_canonical_name(type_ref);
   // → 内部调用 canonical_builtin_type_name("int", 0U) → "i32"
   ```
   "i64" ≠ "i32"，类型不匹配。

**临时绕过**：
- 将源码改为 `var items: i32[] = [1, 2, 3];` 和 `let first: i32 = items[0];`
- **问题**：改变了测试本意（从"推断数组"变为"显式类型数组"）

---

### 问题 2：test_valid_unary_binary_and_if_expressions_pass

**测试源码**：
```feng
let flag: bool = !false && 1 < 2;
let value: int = if flag { 1 + 2; } else { 3 + 4; };
```

**预期行为**：
- 两个 let 绑定都应通过类型检查

**实际行为**（64 位平台）：
- 第 1 行通过 ✓
- 第 2 行报错 "does not match expected type 'i64'" ✗

**根因**：

与问题 1 相同的 bug。

**详细调用链**：

1. if 表达式的分支 `1 + 2` 和 `3 + 4` 是整数字面量
2. `infer_expr_type` 返回 `inferred_expr_type_builtin("int")`
3. if 表达式的类型推断为 "int"（未规范化）
4. 目标类型 `let value: int` 经 AST 规范化后为 "i64"
5. 类型比较时：
   - 表达式类型 "int" → `canonical_builtin_type_name("int", 8)` → "i64"
   - 目标类型 "int" → `type_ref_builtin_canonical_name` → `canonical_builtin_type_name("int", 0)` → "i32"
   - "i64" ≠ "i32"，类型不匹配

**临时绕过**：
- 将源码改为 `let value: i32 = if flag { 1 + 2; } else { 3 + 4; };`
- **问题**：避开了 `int`（i64）的类型检查，未测试平台相关别名的正确性

---

## 二、未修复的 3 个问题

### 问题 3：test_semantic:8017 - test_external_imported_field_type_participates_in_typecheck

**测试源码**：
```feng
// external_source
open type User { open let name: string; }

// main_source
func project(user: model.User): int {
    return user.name;
}
```

**预期行为**：
- 报错 "does not match expected type '...'"（string 不匹配 int）

**实际行为**（64 位平台）：
- 报错 "does not match expected type 'i64'" ✓
- 但测试断言期望 "'i32'"，导致失败

**现状**：
- 已修复：测试已改为平台相关断言（8017-8022 行，检查 `int_canonical`）
- 64 位平台断言 `"i64"`，32 位平台断言 `"i32"`

**根因**：
- 与问题 1、2 相同：`type_ref_builtin_canonical_name` 使用 `pointer_size=0`
- 返回值类型 `int` 在错误消息中显示为 "i64"（正确），但断言未更新

---

### 问题 4：test_symbol:671 - test_provider_loads_bundle_public_module

**测试源码**：
```feng
open func answer(): int { return 42; }
```

**预期行为**：
- symbol 导出后，返回类型的 builtin_name 应为平台相关的规范名

**实际行为**（64 位平台）：
- 断言 `slice_equals_cstr(feng_symbol_type_builtin_name(...), "i32")` 失败
- 实际返回 "i64"

**现状**：
- 未修复
- 需要改为平台相关断言

**根因**：
- symbol 导出时，`int` 正确规范化为 "i64"
- 但测试断言硬编码 "i32"

---

### 问题 5：test_codegen:1701 - test_generic_runtime_extern_call_infers_type_args

**测试源码**：
```feng
extern func feng_array_get_length<T>(value: T[]): i64;
func run(values: int[]): i64 {
    return feng_array_get_length(values);
}
```

**预期行为**：
- 生成的 C 代码中，descriptor 应为平台相关的名称

**实际行为**（64 位平台）：
- 断言期望 `feng_i32_descriptor`
- 实际生成 `feng_i64_descriptor`

**现状**：
- 未修复
- 需要改为平台相关断言

**根因**：
- 参数类型 `int[]` 正确规范化为 "i64[]"
- 生成的 descriptor 名称为 `feng_i64_descriptor`（正确）
- 但测试断言硬编码 `feng_i32_descriptor`

---

## 三、修复方案

### 核心 Bug 修复（问题 1、2、3）

**决策**：所有使用 `pointer_size` 的地方，都必须使用从 CLI 层传入的参数，禁止硬编码 `0U`。语义层禁止自行使用 `sizeof(void *)`，否则会破坏未来交叉编译的能力。简单 API `feng_semantic_analyze` 从语义层移除，仅在测试层保留包装函数。**例外**：仅用于类型类别判断（is_numeric、is_bool 等）的函数，`pointer_size` 不参与分类逻辑，传入 0 各平台均逻辑正确且性能无损，无需修改，仅需注释说明。

**`pointer_size` 传递链路**：

```
CLI 层 (src/cli/frontend.c:298)
  └─ semantic_options.pointer_size = feng_get_host_pointer_size()
      │
      ▼
语义分析入口 (src/semantic/analyzer.c:26451)
  └─ pointer_size = options->pointer_size  // 必须使用 CLI 传入的值
  └─ analysis->pointer_size = pointer_size
      │
      ▼
ResolveContext (src/semantic/analyzer.c:24867)
  └─ context.pointer_size = analysis->pointer_size
      │
      ▼
各类型推断/比较函数
  └─ context->pointer_size  ← 应统一使用此值
```

**语义层违规使用 `sizeof(void *)` 的位置**：

语义层中 `sizeof(void *)` 仅允许出现在 `feng_get_host_pointer_size()`（analyzer.c:2251），其他位置均为违规：

- `analyzer.c:26453`：`options->pointer_size ?: sizeof(void *)` — fallback 逻辑，应改为报错或要求调用方必须传入
- `analyzer.c:26702`：简单 API `feng_semantic_analyze` 中 `options.pointer_size = sizeof(void *)` — 随简单 API 一起移除

**简单 API `feng_semantic_analyze` 处理方案**：

从语义层**移除**简单 API `feng_semantic_analyze`，仅保留完整 API `feng_semantic_analyze_with_options`。在测试代码中定义本地包装函数，使现有 678 个调用点几乎不需要改动。

理由：

- 语义层中 `feng_semantic_analyze` 的实现包含 `sizeof(void *)`（analyzer.c:26702），违反 "语义层禁止使用 `sizeof(void *)`" 原则
- 简单 API 的唯一非测试调用者是 `legacy.c`（已隐藏的旧编译路径，后续废弃）
- 移至测试层后，`pointer_size` 由测试代码自行控制，支持跨平台测试场景

**语义层变更**：

- 删除 `analyzer.c:26692` 的 `feng_semantic_analyze` 实现
- 删除 `semantic.h` 中的 `feng_semantic_analyze` 声明
- 删除 `analyzer.c:26453` 的 `sizeof(void *)` fallback 逻辑（`options->pointer_size` 为 0 时应报错而非 fallback）

**测试层包装函数**（在各测试文件顶部或公共头文件中定义）：

```c
static bool test_semantic_analyze(const FengProgram *const *programs,
                                  size_t program_count,
                                  FengCompileTarget target,
                                  FengSemanticAnalysis **out_analysis,
                                  FengSemanticError **out_errors,
                                  size_t *out_error_count) {
    FengSemanticAnalyzeOptions options;
    memset(&options, 0, sizeof(options));
    options.target = target;
    options.pointer_size = feng_get_host_pointer_size();
    return feng_semantic_analyze_with_options(programs, program_count,
                                              &options, out_analysis,
                                              out_errors, out_error_count);
}
```

调用点只需将 `feng_semantic_analyze` 替换为 `test_semantic_analyze`（或通过宏 `#define feng_semantic_analyze test_semantic_analyze` 实现零改动）。
需要跨平台测试的场景可直接使用 `feng_semantic_analyze_with_options` 并自定义 `pointer_size`。

**`legacy.c` 处理**：

`src/cli/compile/legacy.c:41` 是唯一使用简单 API 的非测试代码。该文件是已隐藏的 `feng tool compile` 旧编译路径（`main.c:51` 已注释），后续废弃。当前将其改为调用完整 API：

```c
// src/cli/compile/legacy.c:41
FengSemanticAnalyzeOptions sem_opts;
memset(&sem_opts, 0, sizeof(sem_opts));
sem_opts.target = opts->target;
sem_opts.pointer_size = feng_get_host_pointer_size();
if (!feng_semantic_analyze_with_options(&prog_ptr, 1U, &sem_opts,
                                        &analysis, &errors, &error_count)) {
```

**调用点影响统计**：

| 文件 | 简单 API 调用数 | 修复方式 |
| --- | --- | --- |
| test/semantic/test_semantic.c | 573 | 使用测试包装函数 |
| test/codegen/test_codegen.c | 102 | 使用测试包装函数 |
| test/symbol/test_symbol.c | 2 | 使用测试包装函数 |
| test/debug/test_debug.c | 1 | 使用测试包装函数 |
| src/cli/compile/legacy.c | 1 | 改用完整 API（后续废弃） |
| **合计** | **679** | |

**`pointer_size=0` 硬编码全表**：

| 行号 | 函数 | 用途 | 影响分类 | 是否需要修改 |
| --- | --- | --- | --- | --- |
| 4518 | `type_ref_builtin_canonical_name` | 类型比较 | **核心 bug**（导致类型不匹配） | **需要** |
| 5761 | `inferred_expr_type_is_numeric` | 判断是否数值类型 | 无需修改（位宽无关） | 不需要，更新注释 |
| 5788 | `inferred_expr_type_is_integer` | 判断是否整数类型 | 无需修改（位宽无关） | 不需要，更新注释 |
| 5800 | `inferred_expr_type_is_bool` | 判断是否布尔 | 无需修改（位宽无关） | 不需要，更新注释 |
| 5807 | `inferred_expr_type_is_string` | 判断是否字符串 | 无需修改（位宽无关） | 不需要，更新注释 |
| 2308 | `builtin_type_name_is_numeric` | 判断是否数值（下游） | 无需修改（位宽无关） | 不需要，更新注释 |
| 2318 | `builtin_type_name_is_integer` | 判断是否整数（下游） | 无需修改（位宽无关） | 不需要，更新注释 |
| 17760 | `format_inferred_expr_type_name` | 错误消息格式化 | 影响诊断信息正确性 | **需要** |

影响分类说明：

- **核心 bug**（4518）：直接导致类型比较失败，是问题 1、2 的根因
- **无需修改（位宽无关）**（5761、5788、5800、5807、2308、2318）：这些函数仅用于类型类别判断（is_numeric、is_bool 等），`pointer_size` 不参与分类逻辑，传入 0 各平台均逻辑正确且性能无损，只需更新注释说明原因
- **影响诊断信息**（17760）：错误消息中会显示错误的规范名（64 位平台上 "int" 显示为 "i32"），不影响类型检查正确性但影响用户体验

处理原则：

- **需要修改的**（4518、17760）：函数职责是**解析别名**或**生成用户可见文本**，结果随平台变化，必须传入正确的 `pointer_size`
- **不需要修改的**（5761、5788、5800、5807、2308、2318）：函数职责是**类型类别判断**（is_numeric、is_bool 等），检查的是类型种类而非别名映射，`pointer_size` 不影响分类结果。保持 `0U`，更新注释为：`位宽无关，传入 0 各平台均逻辑正常，性能无损`

**修复步骤**：

1. 修改 `type_ref_builtin_canonical_name` 接受 `pointer_size` 参数（核心 bug 修复）：

```c
static const char *type_ref_builtin_canonical_name(const FengTypeRef *type_ref, size_t pointer_size) {
    if (type_ref == NULL || type_ref->kind != FENG_TYPE_REF_NAMED ||
        type_ref->as.named.segment_count != 1U) {
        return NULL;
    }
    return canonical_builtin_type_name(type_ref->as.named.segments[0], pointer_size);
}
```

2. 更新 `type_ref_builtin_canonical_name` 的所有调用点（~20 处），传入 `context->pointer_size`

3. 修改 `format_inferred_expr_type_name` 接受 `pointer_size` 参数（诊断信息修复）：

   - `format_inferred_expr_type_name(type, pointer_size)` — 传入 `context->pointer_size`
   - 递归更新其调用点

4. 更新 is_* 函数注释（类别判断函数，保持 `0U` 不变）：

   - `inferred_expr_type_is_numeric`、`inferred_expr_type_is_integer`、`inferred_expr_type_is_bool`、`inferred_expr_type_is_string` 保持 `0U`
   - 将现有注释（如 "pointer_size=0: Task 3 keeps int→i32 regardless"）替换为：`位宽无关，传入 0 各平台均逻辑正常，性能无损`

5. 删除过时注释（如 "Task 6 will thread pointer_size"、"pointer_size=0 is safe" 等），替换为步骤 4 中的统一注释

6. 从语义层移除简单 API `feng_semantic_analyze`：

   - 删除 `analyzer.c` 中的 `feng_semantic_analyze` 实现
   - 删除 `semantic.h` 中的 `feng_semantic_analyze` 声明
   - 删除 `analyzer.c:26453` 的 `sizeof(void *)` fallback 逻辑
   - 在测试公共头文件中定义 `test_semantic_analyze` 包装函数
   - 测试文件中通过宏或批量替换将 `feng_semantic_analyze` 改为 `test_semantic_analyze`

7. 修改 `legacy.c` 改用完整 API `feng_semantic_analyze_with_options`（后续废弃）

8. 清理测试中残留的调试代码（`test_semantic.c` 约 5195-5198 行的 `fprintf`）

### 测试断言修复（问题 3、4、5）

- 问题 3：验证平台相关断言是否正确应用
- 问题 4：改为 `const char *int_canonical = sizeof(void *) >= 8U ? "i64" : "i32";`
- 问题 5：改为 `const char *descriptor = sizeof(void *) >= 8U ? "feng_i64_descriptor" : "feng_i32_descriptor";`
