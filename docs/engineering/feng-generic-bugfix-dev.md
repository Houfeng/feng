# 共享体内 reified 聚合元组完整生命周期管理 + for-in 迭代器协议支持

## Context

`ForInGenericTester<T>.iterateAll()` 的共享体中执行 `for let v in self.items` 时，codegen 在 L20752 报错。根本原因不限于 for-in——**任何共享体内绑定含泛型参数字段的元组（reified tuple），其生命周期管理都缺失**。本方案在类型分类层和 cleanup 机制层做通用修复，for-in 仅需删除 bail + 补传 `_type_desc`。

## 根因分析

### 问题 1：类型分类——reified tuple 被错误归类为 trivial

`cg_tuple_aggregate_flattened_pointer_slot_count`（L27984-28007）遍历元组字段，通过 `cgtype_value_kind` 统计 managed slot 数量。`CG_TYPE_GENERIC_PARAM` 被分类为 `CG_VK_TRIVIAL`（L244-248），不计入 slot count → 返回 0 → `cg_aggregate_facts` 将 `value_kind` 设为 `CG_VK_TRIVIAL` → `cgtype_is_aggregate` 返回 false。

**结果**：所有使用 `cgtype_is_aggregate` 做分支的路径（`cg_emit_binding`、`cg_materialize_to_local`、`cg_release_scope`、`cg_emit_cleanup_push_for_aggregate_local`、for-in cleanup 注册）全部跳过 reified tuple，不做 cleanup。运行时 T = string 时，+1 引用泄漏。

**这不是 for-in 特有问题**——共享体内 `let x = someObj.method()` 返回 reified tuple 时，`cg_emit_binding` 同样走 trivial 分支，不注册 cleanup。

### 问题 2：for-in 方法调用缺少 `_type_desc`

for-in 代码手动发出 `iter()` 和 `next()` 的 C 调用（L20627、L20768），绕过了通用 `cg_emit_call` 路径。通用路径（L14622-14639）通过 `cg_append_user_type_context_descriptor_args` 正确传递 `_type_desc`，but for-in 没有。

## 设计方案

### 原则

1. **根因修复在类型分类层**：让 `cg_tuple_aggregate_flattened_pointer_slot_count` 计入 `CG_TYPE_GENERIC_PARAM` → 所有下游路径自动启用 aggregate 处理
2. **cleanup 机制层感知 reified tuple**：在已有的 aggregate cleanup 入口点检测 `cg_tuple_needs_reified_layout`，使用 RAD 替代静态描述符
3. **不修改 Local 结构体**：在 cleanup 时通过 `cg_lookup_reified_agg_dep_index` 实时查找 RAD（编译期小数组扫描，零运行时开销）
4. **不引入新运行时函数**：复用已有的 `feng_cleanup_push_aggregate`/`feng_aggregate_release`/`feng_aggregate_retain`

### 为什么不需要修改 Local 结构体

前版方案提议增加 `reified_agg_desc_expr` 字段。但 `cg_release_scope` 已经有 `CG *cg` 参数，可以直接调用 `cg_tuple_needs_reified_layout(cg, l->type)` 和 `cg_lookup_reified_agg_dep_index` 在 cleanup 时实时查找 RAD。这避免了结构体变更、内存管理、和 `scope_pop_free` 的修改。

### 变更 1：类型分类——计入泛型参数潜在 managed slot

**文件**: `src/codegen/codegen.c`
**函数**: `cg_tuple_aggregate_flattened_pointer_slot_count`（L27984-28007）

在 `switch (cgtype_value_kind(field_type))` 中增加 `CG_TYPE_GENERIC_PARAM` 的显式处理。注意：不能改 `cgtype_value_kind` 本身（那会影响所有类型分类），而是在 slot count 函数中直接检查字段类型：

```c
for (size_t i = 0; i < tuple_type->field_count; ++i) {
    const CGType *field_type = tuple_type->fields[i].type;
    /* CG_TYPE_GENERIC_PARAM 在 cgtype_value_kind 中是 CG_VK_TRIVIAL（ARC
     * 由描述符在调用点处理），但在元组中它是潜在的 managed slot——运行时
     * 可能是 string/array/object。必须计入以触发 aggregate cleanup 路径。 */
    if (field_type != NULL && field_type->kind == CG_TYPE_GENERIC_PARAM) {
        count++;
        continue;
    }
    switch (cgtype_value_kind(field_type)) {
        // ... 现有代码不变 ...
    }
}
```

**影响范围**：`CG_TYPE_GENERIC_PARAM` 仅存在于共享体（erased 上下文），具体实例中 T 已解析为具体类型。因此此变更仅影响共享体内的元组分类。

### 变更 2：cleanup push——reified tuple 使用 RAD 整体推入

**函数**: `cg_emit_cleanup_push_for_aggregate_local`（L17732-17740）

在现有逻辑前增加 reified tuple 分支。reified tuple 使用 `feng_cleanup_push_aggregate`（1 个 cleanup 节点覆盖整个聚合），而非 per-field 的 `cg_tuple_emit_cleanup_push_slots`（逐字段推入，但 generic_param 字段会被跳过）：

```c
static void cg_emit_cleanup_push_for_aggregate_local(CG *cg,
                                                     const char *cname,
                                                     const CGType *type) {
    if (cg_tuple_needs_reified_layout(cg, type)) {
        const char *agg_desc = cg_aggregate_desc_name(type);
        size_t rad_idx;
        if (agg_desc &&
            cg_lookup_reified_agg_dep_index(cg, agg_desc, &rad_idx)) {
            const char *src = cg->generic_type_method_rad_via_desc
                                  ? "_desc" : "_td";
            cg_emit_current_stmt_line_directive_force(cg);
            buf_append_fmt(cg->cur_body,
                "    FengCleanupNode _cu_%s; "
                "feng_cleanup_push_aggregate(&_cu_%s, &%s, "
                "(const FengAggregateDescriptor *)%s->reified_agg_deps[%zu]);\n",
                cname, cname, cname, src, rad_idx);
        }
        return;
    }
    /* 现有路径不变 */
    CGAggregateFacts facts;
    if (cg_aggregate_facts(type, &facts) && facts.emit_cleanup_push != NULL) {
        cg_emit_current_stmt_line_directive_force(cg);
        facts.emit_cleanup_push(cg->cur_body, cname, type);
    }
}
```

### 变更 3：scope release——reified tuple 使用 RAD 释放

**函数**: `cg_release_scope`（L17630-17665）

在 `cgtype_is_aggregate` 分支中，前置 reified tuple 检查。reified tuple 只有 1 个 cleanup 节点（变更 2），所以 pop 1 次；用 RAD 释放；用 `memset` 清零整个结构体：

```c
} else if (cgtype_is_aggregate(l->type)) {
    if (cg_tuple_needs_reified_layout(cg, l->type)) {
        const char *agg_desc = cg_aggregate_desc_name(l->type);
        size_t rad_idx;
        if (agg_desc &&
            cg_lookup_reified_agg_dep_index(cg, agg_desc, &rad_idx)) {
            const char *src = cg->generic_type_method_rad_via_desc
                                  ? "_desc" : "_td";
            cg_emit_current_stmt_line_directive_force(cg);
            buf_append_fmt(cg->cur_body,
                "    feng_cleanup_pop(); "
                "feng_aggregate_release(&%s, "
                "(const FengAggregateDescriptor *)%s->reified_agg_deps[%zu]); "
                "memset(&%s, 0, sizeof %s);\n",
                l->c_name, src, rad_idx, l->c_name, l->c_name);
        }
    } else {
        /* 现有路径不变 */
        ...
    }
}
```

### 变更 4：binding retain——reified tuple 使用 RAD 保留引用

**函数**: `cg_emit_binding`（L18508-18528）和 `cg_materialize_to_local`（L10285-10291）

在 `!init.owns_ref` 的 aggregate retain 路径中，前置 reified tuple 检查：

```c
} else if (cgtype_is_aggregate(decl_type)) {
    if (init.owns_ref) {
        buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, cname, init.c_expr);
    } else if (cg_tuple_needs_reified_layout(cg, decl_type)) {
        const char *agg_desc = cg_aggregate_desc_name(decl_type);
        size_t rad_idx;
        if (agg_desc &&
            cg_lookup_reified_agg_dep_index(cg, agg_desc, &rad_idx)) {
            const char *src = cg->generic_type_method_rad_via_desc
                                  ? "_desc" : "_td";
            buf_append_fmt(cg->cur_body,
                "    %s %s = %s; feng_aggregate_retain(&%s, "
                "(const FengAggregateDescriptor *)%s->reified_agg_deps[%zu]);\n",
                cty, cname, init.c_expr, cname, src, rad_idx);
        }
    } else {
        /* 现有路径不变 */
        ...
    }
}
```

同理修改 `cg_materialize_to_local` 中的 L10285-10291。

### 变更 5：for-in 删除 error bail

**函数**: `cg_emit_for_in_iterator`

删除 L20752-20766 的 `if (cg_tuple_needs_reified_layout(...)) { ... bail ... }` 整段代码。变更 1-3 使得后续的 aggregate 路径自动处理 reified tuple。

### 变更 6：for-in `iter()` 调用传递 `_type_desc`

**位置**: L20627

使用 `cg_append_user_type_context_descriptor_args(cg, buf, src_ut, blame)` 为 `iter()` 调用追加 `_type_desc` 参数。仅当 `src_ut->generic_context_type_param_count > 0` 时生效（函数内部检查）：

```c
Buf iter_call; buf_init(&iter_call);
buf_append_fmt(&iter_call, "%s(%s", iterable_um->c_name, src.c_expr);
if (!cg_append_user_type_context_descriptor_args(cg, &iter_call, src_ut, stmt->token)) {
    buf_free(&iter_call); /* ... error handling ... */
}
buf_append_cstr(&iter_call, ")");
buf_append_fmt(cg->cur_body, "    %s %s = %s;\n",
               cursor_cty, cursor_var, iter_call.data);
buf_free(&iter_call);
```

### 变更 7：for-in `next()` 调用传递 `_type_desc`

**位置**: L20768

同变更 6，为 `next()` 调用追加 `_type_desc`，使用 `cursor_ut` 作为 owner type：

```c
Buf next_call; buf_init(&next_call);
buf_append_fmt(&next_call, "%s(%s", iter_um->c_name, cursor_var);
if (!cg_append_user_type_context_descriptor_args(cg, &next_call, cursor_ut, stmt->token)) {
    buf_free(&next_call); /* ... error handling ... */
}
buf_append_cstr(&next_call, ")");
buf_append_fmt(cg->cur_body, "        %s %s = %s;\n",
               result_cty, result_var, next_call.data);
buf_free(&next_call);
```

### 变更 8：for-in 终止 break 路径使用 RAD

**位置**: L20779-20790

终止路径手动释放 result 并 pop cleanup。需要区分 reified 和非 reified：

```c
buf_append_fmt(cg->cur_body, "        if (!%s.%s) { ", result_var, bool_field->c_name);
if (cg_tuple_needs_reified_layout(cg, result_type)) {
    const char *agg_desc = cg_aggregate_desc_name(result_type);
    size_t rad_idx;
    if (agg_desc &&
        cg_lookup_reified_agg_dep_index(cg, agg_desc, &rad_idx)) {
        const char *src = cg->generic_type_method_rad_via_desc ? "_desc" : "_td";
        buf_append_fmt(cg->cur_body,
            "feng_cleanup_pop(); feng_aggregate_release(&%s, "
            "(const FengAggregateDescriptor *)%s->reified_agg_deps[%zu]); ",
            result_var, src, rad_idx);
    }
} else if (cgtype_is_aggregate(result_type)) {
    /* 现有路径不变 */
    ...
}
buf_append_cstr(cg->cur_body, "break; }\n");
```

### 循环变量绑定——无需修改

element_type 为 `CG_TYPE_GENERIC_PARAM` 时：
- L20811 `cgtype_is_managed` → false
- L20815 `cgtype_is_aggregate` → false（单个 generic_param 不是元组）
- L20825-20828 走 else 分支：`void * v = result._2;` — trivial copy，正确

cleanup 注册（L20851-20855）：同理走 else，不注册 cleanup。正确——element 的引用由 result tuple 的 RAD cleanup 管理，循环体内的泛型赋值（如 `self.lastSeen = v`）会自动 retain。

**引用计数验证**（以 T = string 为例）：
1. `result = cursor.next()` → result._2 持有 +1
2. `v = result._2` → trivial copy，不 retain
3. 循环体 `self.lastSeen = v` → 泛型赋值 retain +1，release 旧值 -1
4. 迭代结束 `cg_release_scope` → RAD release result，release result._2 → -1
5. 净效果：lastSeen +1，result._2 +1-1=0，正确

### 变更 9：语义层——收集 for-in 迭代器协议的隐式 reifiable deps

for-in 迭代器协议的 `iter()` 和 `next()` 调用是 codegen 合成的，不存在于 AST 的 `FENG_EXPR_CALL` 节点中。`rd_try_collect_call_return_type_dep`（`reifiable_deps.c` L628-728）仅对显式 `FENG_EXPR_CALL` 生效，因此 for-in 的隐式类型依赖不会被自动收集。

**缺失的依赖：**

1. **cursor 类型（RTD）**：`iter()` 返回的迭代器类型（如 `ListIterator<T>`）。变更 7 中 `cg_append_user_type_context_descriptor_args(cg, &next_call, cursor_ut, ...)` 需要 `cursor_ut->c_desc_name` 在当前类型的 `generic_type_method_rtd_descs` 中，否则查找失败报错 `"codegen: no reified_type_dep found for generic type method call"`。

2. **result 元组类型（RAD）**：`next()` 返回的结果元组（如 `(Bool, T)`）。变更 2/3/8 中 `cg_lookup_reified_agg_dep_index(cg, agg_desc, &rad_idx)` 需要 result 元组的聚合描述符在当前类型的 `generic_type_method_rad_descs` 中，否则 cleanup push/release/break 路径无法获取 RAD。

**为什么现有收集不够：**

`collect_from_stmt` for `FENG_STMT_FOR`（`reifiable_deps.c` L908-915）只收集 `iter_expr`（迭代源表达式）和 `iter_binding`（循环变量）：
- `collect_from_expr` 对 `self.items`（member access）只递归收集 object，不收集 member 的类型
- `List<T>` 通过字段类型声明 `var items: List<T>` 已收集为 RTD → `iter()` 的 `_type_desc` 有效
- 但 `ListIterator<T>` (cursor) 和 `(Bool, T)` (result) 没有在任何 AST 节点中被引用 → 未收集

**实现方案：**

**方案 A（推荐）**：在语义分析器中存储 substituted result_type_ref + 在 reifiable deps 收集时使用

1. **`src/parser/parser.h`** — for_stmt 增加字段：
```c
const FengTypeRef *iter_result_type_ref;  /* substituted result tuple type ref */
```

2. **`src/semantic/analyzer.c`** — 在 `result_type_ref` substitution 完成后（L18398-18401 附近），将其存入 for_stmt：
```c
mutable_stmt->as.for_stmt.iter_result_type_ref = result_type_ref;
```

3. **`src/semantic/reifiable_deps.c`** — `collect_from_stmt` 的 `FENG_STMT_FOR` case 增加：
```c
case FENG_STMT_FOR:
    collect_from_stmt(ctx, stmt->as.for_stmt.init);
    collect_from_expr(ctx, stmt->as.for_stmt.condition);
    collect_from_stmt(ctx, stmt->as.for_stmt.update);
    collect_from_binding(ctx, &stmt->as.for_stmt.iter_binding);
    collect_from_expr(ctx, stmt->as.for_stmt.iter_expr);
    /* 收集 for-in 迭代器协议的隐式类型依赖（cursor RTD + result RAD）。
     * cursor_type_ref 和 result_type_ref 已由语义分析器 substitute
     * 为当前上下文的 type params，可直接用 try_collect_type_ref。 */
    try_collect_type_ref(ctx, stmt->as.for_stmt.iter_cursor_type_ref);
    try_collect_type_ref(ctx, stmt->as.for_stmt.iter_result_type_ref);
    collect_from_block(ctx, stmt->as.for_stmt.body);
    return;
```

**注意**：`iter_cursor_type_ref` 和 `iter_result_type_ref` 都已在语义分析器中完成 type param substitution（L18367-18376 和 L18392-18401），引用当前类型的 type params。`try_collect_type_ref` 会通过 `type_ref_contains_type_param` 检查匹配，并通过 `determine_dep_kind` 正确区分 MANAGED（cursor 类型→RTD）和 AGGREGATE（result 元组→RAD）。

自游标（self-cursor）路径（L18437-18491）也需要同样处理：存储 `iter_result_type_ref` 并确保在 `collect_from_stmt` 中收集。

## 涉及位置

| 位置 | 变更 | 性质 |
|------|------|------|
| `parser.h` for_stmt 结构体 | 增加 `iter_result_type_ref` 字段 | 语义 |
| `analyzer.c` L18398 / L18491 附近 | 存储 substituted result_type_ref | 语义 |
| `reifiable_deps.c` L908 `FENG_STMT_FOR` | 收集 cursor_type_ref + result_type_ref | 语义 |
| `cg_tuple_aggregate_flattened_pointer_slot_count` (L27990) | 计入 CG_TYPE_GENERIC_PARAM | 通用 |
| `cg_emit_cleanup_push_for_aggregate_local` (L17732) | reified tuple 用 RAD push | 通用 |
| `cg_release_scope` (L17643) | reified tuple 用 RAD release | 通用 |
| `cg_emit_binding` (L18508) | reified tuple 用 RAD retain | 通用 |
| `cg_materialize_to_local` (L10285) | reified tuple 用 RAD retain | 通用 |
| `cg_emit_for_in_iterator` bail (L20752) | 删除 | for-in |
| `cg_emit_for_in_iterator` iter() (L20627) | 补传 _type_desc | for-in |
| `cg_emit_for_in_iterator` next() (L20768) | 补传 _type_desc | for-in |
| `cg_emit_for_in_iterator` break path (L20779) | 用 RAD release + memset 清零 + 正确 pop 数量 | for-in |

涉及三个文件：`src/parser/parser.h`、`src/semantic/analyzer.c`（+ `reifiable_deps.c`）、`src/codegen/codegen.c`。

## 泛型参数约束（spec）的影响分析

当 T 有 spec 约束时（如 `type Container<T: Printable>`），共享体内元组 `(Bool, T)` 的 RAD 处理与无约束情况完全一致，**无额外问题，无额外开销**。

### 正确性

RAD 的 ARC 行为由 `FengGenericParamDescriptor.kind` 在运行时决定（L22195-22225），与约束无关：
- T = string → `FENG_VALUE_MANAGED_POINTER` → retain/release 指针
- T = i32 → `FENG_VALUE_TRIVIAL` → 跳过 retain/release
- T = 某 spec 的 by-value aggregate → `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` → 递归处理嵌套聚合

约束 spec 只影响方法分派（通过 `FengGenericParamDescriptor.witness`），不影响 ARC 语义。RAD 的 `feng_aggregate_retain`/`feng_aggregate_release` 根据 `kind` 字段处理每个字段，与约束 spec 无关。

### 开销分析

变更 1 将含 generic_param 字段的元组从 trivial 提升为 aggregate。当 T 在运行时为 trivial（如 i32）时，与修复前相比有少量额外操作：

| 操作 | 开销 | 说明 |
|------|------|------|
| `feng_cleanup_push_aggregate` | 栈上写入几个字段 | 同任何 aggregate，极低 |
| `feng_cleanup_pop` | 指针操作 | 同任何 managed local |
| `feng_aggregate_release` | 遍历 slot 数组，trivial 字段跳过 | slot 数量通常 ≤2，O(1) |

**这些是保证正确性所必需的最小开销**——修复前不做 cleanup 的"零开销"是以引用泄漏为代价的。对于 T = managed pointer 的常见场景，这些操作本就是必需的，不存在额外开销。RAD 路径与静态描述符路径的运行时开销相同——唯一差异是描述符来源（动态 vs 静态），但实际的 retain/release 操作完全一致。

### 变更 10：for-in break 路径——释放后清零

**位置**：`cg_emit_for_in_iterator` L20782-20789

**问题**：break 路径在 `feng_aggregate_release` 后不做 `memset` 清零，而 `cg_release_scope`（L17662）对相同类型的局部变量释放后会调用 `cg_emit_cleanup_zero_for_aggregate_local` 清零。在异常展开场景中，未清零的已释放 slot 可能导致 double-free。本次一并修复，与 `cg_release_scope` 保持一致。

**修复**：在 `feng_aggregate_release` 后补充 `memset` 清零，reified 和非 reified 路径均需处理：

```c
buf_append_fmt(cg->cur_body, "        if (!%s.%s) { ", result_var, bool_field->c_name);
if (cg_tuple_needs_reified_layout(cg, result_type)) {
    /* reified 路径（变更 8）已含 cleanup_pop + release，补充 memset */
    const char *agg_desc = cg_aggregate_desc_name(result_type);
    size_t rad_idx;
    if (agg_desc &&
        cg_lookup_reified_agg_dep_index(cg, agg_desc, &rad_idx)) {
        const char *src = cg->generic_type_method_rad_via_desc ? "_desc" : "_td";
        buf_append_fmt(cg->cur_body,
            "feng_cleanup_pop(); feng_aggregate_release(&%s, "
            "(const FengAggregateDescriptor *)%s->reified_agg_deps[%zu]); "
            "memset(&%s, 0, sizeof %s); ",
            result_var, src, rad_idx, result_var, result_var);
    }
} else if (cgtype_is_aggregate(result_type)) {
    const char *desc = cg_aggregate_desc_name(result_type);
    if (desc) {
        buf_append_fmt(cg->cur_body,
            "feng_aggregate_release(&%s, &%s); ", result_var, desc);
    }
    cg_emit_cleanup_pops_for_aggregate_local(cg->cur_body, result_type);
    cg_emit_cleanup_zero_for_aggregate_local(cg->cur_body, result_var, result_type);
}
buf_append_cstr(cg->cur_body, "break; }\n");
```

### 变更 11：for-in break 路径——非 reified 聚合 cleanup pop 数量修正

**位置**：`cg_emit_for_in_iterator` L20788

**问题**：break 路径固定执行 1 次 `feng_cleanup_pop()`，但 `cg_tuple_emit_cleanup_push_slots`（L28201-28245）可能对包含多个 managed 字段的元组推入多个 cleanup 节点。当 result 元组含 >1 个 managed pointer 字段时（如理论上的 `(String, String, Bool)`），push 了 2 个 cleanup 节点但只 pop 1 个 → cleanup 链损坏。

**说明**：reified 路径在变更 2 中使用 `feng_cleanup_push_aggregate`（1 个节点），变更 8 中 pop 1 次，匹配正确。此问题仅影响非 reified 路径。

**修复**：已合并到变更 10 中——非 reified 分支使用 `cg_emit_cleanup_pops_for_aggregate_local(cg->cur_body, result_type)` 代替固定的 `feng_cleanup_pop()`，根据 `facts.pointer_slot_count` pop 正确次数。

### 变更 9：for-in 共享体跨类型方法调用——使用 shared body 名称和调用约定

**位置**：`cg_emit_for_in_iterator` iter() 调用（L20690）和 next() 调用（L20831）

**问题**：for-in 代码使用 `iterable_um->c_name` 和 `iter_um->c_name` 作为函数名，这在共享体上下文中产生 open wrapper 名称（如 `Feng__std__collections__List__G__T__iter__from__void`），但 open wrapper 函数**不存在**——只有具体实例的 wrapper 和共享体函数存在。

编译生成的 C 代码报错：`call to undeclared function 'Feng__std__collections__List__G__T__iter__from__void'`。

**根因**：`cg_register_user_type_members`（L7720）为 open generic 实例设置 `um->c_name = struct_name + "__" + method_san`，产生如 `List__G__T__iter__from__void` 的名称。这是 wrapper 命名规则，但 open generic 类型不生成 wrapper——只有共享体（`FengGenericMethod__...`）存在。

**修复**：在 `cg->in_generic_type_method && src_ut->generic_context_type_param_count > 0` 时：

1. 使用 `cg_generic_type_method_shared_cname(cg, src_ut->generic_origin_decl, iterable_method)` 获取共享体函数名
2. 使用 `cg_rtd_expr_for_type(cg, src_ut, ...)` 获取目标类型的 RTD 表达式（如 `_td->reified_type_deps[1]`）
3. 使用共享体调用约定：`shared_fn((void *)receiver, type_desc_expr, &return_var)`

新增辅助函数 `cg_rtd_expr_for_type`：查找 RTD 索引并返回表达式字符串（如 `_td->reified_type_deps[idx]`），与 `cg_append_user_type_context_descriptor_args` 共享相同的查找逻辑但返回独立表达式而非追加到已有调用中。

对 iter() 和 next() 调用分别处理：
```c
// iter() — 返回 managed pointer (cursor)
CursorType *cursor_var;
shared_iter_name((void *)src, _td->reified_type_deps[src_idx], &cursor_var);

// next() — 返回 aggregate (result tuple)
ResultType result_var;
shared_next_name((void *)cursor, _td->reified_type_deps[cursor_idx], &result_var);
```

### 变更 12：for-in 循环变量绑定——泛型参数元素取地址

**位置**：`cg_emit_for_in_iterator` 循环变量绑定（L20988）

**问题**：循环变量绑定 `for let v in ...` 中，当元素类型为 `CG_TYPE_GENERIC_PARAM` 时，生成的代码是：
```c
void * _l_v_0 = _ir3.item2;
```
这将 `void *` 槽的**值**赋给循环变量。对于 trivial 类型（如 i32），`item2` 中存储的是值本身（如 30），被当作 `void *` 读出。后续代码将 `_l_v_0` 作为**指向数据的指针**使用（共享体中泛型参数变量的约定是 `const void *` 指针），导致 `memcpy(_gdst, _l_v_0, size)` 从无效地址（如 0x1e）读取，segfault。

**修复**：当 `element_type->kind == CG_TYPE_GENERIC_PARAM` 时，取字段地址而非字段值：
```c
void * _l_v_0 = &_ir3.item2;
```
此指针指向元组 `void *` 槽的起始位置。后续代码通过 `_T->kind` 判断：
- `FENG_VALUE_TRIVIAL`：`memcpy(dst, &item2, size)` — 从槽中复制值，正确
- `FENG_VALUE_MANAGED_POINTER`：`*(void **)&item2` — 解引用读出指针，正确

## 不变量

1. **非共享体路径完全不变**：`CG_TYPE_GENERIC_PARAM` 仅存在于共享体；`cg_tuple_needs_reified_layout` 在非共享体返回 false
2. **运行时开销最小化**：不引入新运行时函数；复用 `feng_cleanup_push_aggregate`/`feng_aggregate_release`/`feng_aggregate_retain`。对 trivial T 的额外开销仅为 push/pop cleanup 节点 + 空遍历 release，均为 O(1) 常数级
3. **通用方案**：类型分类层修复使 binding/materialize/scope-release 等所有路径自动获得 reified tuple 支持
4. **不修改 Local 结构体**：通过 `cg_tuple_needs_reified_layout` + `cg_lookup_reified_agg_dep_index` 实时查找，无结构体膨胀
5. **约束 spec 兼容**：RAD 的 ARC 行为由运行时 `FengGenericParamDescriptor.kind` 决定，约束 spec 仅影响 witness 方法分派

## 验证

```bash
make test                    # 全量回归
build/bin/feng run std_test  # 确认 for-in generic test cases 通过
```
