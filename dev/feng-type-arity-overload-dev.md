# Feng Type/Spec 按泛型参数个数重载开发文档

> 规范来源：[docs/feng-generics-draft.md](../docs/feng-generics-draft.md) §4-§6
> 本文记录实现方案、数据结构改造、影响面分析与待办任务。

---

## 0 现状与问题

### 0.1 函数泛型重载（已实现）

`compute_overload_match_priority`（`src/semantic/analyzer.c:10587`）已将 `type_param_count` 纳入重载决议：

- 有显式类型实参时：`type_param_count == explicit_type_arg_count` 为最优匹配（优先级 0）
- 无显式类型实参时：非泛型候选优先于泛型候选

`FunctionOverloadSetEntry` 按名称聚合多个 decl，允许同名不同泛型参数个数的函数共存。

### 0.2 Type/Spec 泛型重载（未实现）

**规范已明确要求**（`docs/feng-generics-draft.md`）：

- 正确语法九：`type UserType<T>` 和 `type UserType<T, U>` 应允许共存
- [必须] "同一作用域内,同 kind 的具名泛型 type 与具名泛型 spec 的声明 identity 按'名称 + 泛型参数数量'确定"
- [必须] "所有具名泛型 type / spec 的使用位置都必须按'名称 + 泛型参数数量'精确解析到已存在声明"

### 0.3 支持 arity 重载的声明形式（完整清单）

所有带泛型参数的 type / spec 子形式均按 `(name, arity)` 做 identity 判定：

| 声明形式 | 语法示例 | AST 字段 |
| -------- | -------- | -------- |
| 对象类型 | `type User<T> {}` / `type User<T1, T2> {}` | `FENG_DECL_TYPE`, `is_tuple == false` |
| 元组类型 | `type User<T1, T2>(T1, T2)` / `type User<T1, T2, T3>(T1, T2, T3)` | `FENG_DECL_TYPE`, `is_tuple == true` |
| 对象契约 | `spec User<T> {}` / `spec User<T1, T2> {}` | `FENG_DECL_SPEC`, `form == FENG_SPEC_FORM_OBJECT` |
| 函数契约 | `spec User<T>()` / `spec User<T1, T2>()` | `FENG_DECL_SPEC`, `form == FENG_SPEC_FORM_CALLABLE` |
| 联合契约 | `spec User<T1, T2>: T1 \| T2` / `spec User<T1, T2, T3>: T1 \| T2 \| T3` | `FENG_DECL_SPEC`, `form == FENG_SPEC_FORM_UNION` |

> **注**：跨子形式（如对象类型 `User<T>` 与元组类型 `User<T, U>`）是否构成冲突，仍按"同 kind"规则判定——它们同属 `FENG_DECL_TYPE`，因此按 `(name, arity)` 区分，不同 arity 允许共存，相同 arity 冲突。`FENG_DECL_SPEC` 的三个子形式同理。

**当前实现缺陷**：

- `VisibleTypeEntry`（`src/semantic/analyzer.c:70`）每个名称只存单个 decl，无法容纳同名不同 arity 的多个声明
- `check_symbol_conflicts` 在处理 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` 时，`find_visible_type_index` 仅按名称查找，发现同名即报 **AE0213** 冲突
- 类型引用解析（`find_named_type_decl` → `find_visible_type_decl`）只用名称查找，不涉及 `type_param_count`

---

## 1 方案选择

### 1.1 方案 A：复合 key（选定）

保留 `VisibleTypeEntry` 单 decl 结构，但将查找 key 从 `name` 改为 `(name, type_param_count)` 复合 key，允许同名不同 arity 的多个 entry 并存。

**选择理由**：

- 改动最小：只需在查找/插入时同时比对 arity，不改数据结构
- 类型引用解析天然确定：使用点的 arity 来自 `type_ref->as.named.type_arg_count`，无需复杂决议
- 回归面可控：核心逻辑集中在 `find_visible_type_index` 和 `check_symbol_conflicts`
- 符合开闭原则：扩展查找维度，不破坏现有单 arity 场景

### 1.2 未选方案

- **方案 B：decl 集合**（`VisibleTypeEntry` 改为 `const FengDecl **decls`）：类似 `FunctionOverloadSetEntry`，但类型不需要函数那样的优先级决议，引入集合结构过度设计。且所有使用 `entry.decl` 的代码都要改为遍历，改动面大，收益不明显。

---

## 2 数据结构改造

### 2.1 VisibleTypeEntry 保持不变

```c
typedef struct VisibleTypeEntry {
    FengSlice name;
    const FengSemanticModule *provider_module;
    const FengProgram *provider_program;
    const FengDecl *decl;          // 单个 decl，但允许同名不同 arity 的多个 entry
} VisibleTypeEntry;
```

### 2.2 查找函数改造

`find_visible_type_index`（`src/semantic/analyzer.c:2181`）改为接受 `type_param_count` 参数：

```c
static size_t find_visible_type_index(const VisibleTypeEntry *entries,
                                      size_t count,
                                      FengSlice name,
                                      size_t type_param_count);
```

查找逻辑：同时匹配 `name` 和从 `decl` 提取的 `type_param_count`。

### 2.3 find_visible_type 改造

`find_visible_type`（`src/semantic/analyzer.c:2372`）当前仅按名称查找，有 **6 个调用点**。改造后需区分两类场景：

**精确匹配场景**（知道 arity，需要匹配特定声明）：

| 行号 | 上下文 | arity 来源 |
|------|--------|------------|
| L2383 | `find_visible_type_decl` 内部 | 从参数透传 |
| L14658 | 泛型目标类型推断 | 需从调用上下文获取 `type_arg_count` |
| L14691 | 约束 spec 查找 | `cref->as.named.type_arg_count` |

**存在性检查场景**（只需判断是否存在任意 arity 的同名类型）：

| 行号 | 上下文 | 说明 |
|------|--------|------|
| L4500 | `find_unshadowed_alias` — alias 是否被类型遮蔽 | 只需知道是否存在同名类型 |
| L20305 | `use` 声明别名冲突检查 | 只需知道是否存在同名类型（冲突检查不关心 arity） |
| L20809 | 标识符解析 — 判断标识符是否为类型名 | 只需知道是否存在同名类型 |

**方案**：新增 `find_visible_type_any_arity` 辅助函数，用于存在性检查：

```c
static const VisibleTypeEntry *find_visible_type_any_arity(const VisibleTypeEntry *entries,
                                                           size_t count,
                                                           FengSlice name) {
    for (size_t i = 0U; i < count; ++i) {
        if (slice_equals(entries[i].name, name)) {
            return &entries[i];
        }
    }
    return NULL;
}
```

`find_visible_type` 本身改为接受 `type_param_count`，仅用于精确匹配场景。

### 2.4 辅助函数

从 `FengDecl` 提取 `type_param_count`：

```c
static size_t decl_type_param_count(const FengDecl *decl) {
    if (decl->kind == FENG_DECL_TYPE) {
        return decl->as.type_decl.type_param_count;
    }
    if (decl->kind == FENG_DECL_SPEC) {
        return decl->as.spec_decl.type_param_count;
    }
    return 0U;  // FENG_DECL_ENUM 等无泛型参数
}
```

---

## 3 冲突检查改造

### 3.1 check_symbol_conflicts 分支改造

**FENG_DECL_TYPE 分支**（`src/semantic/analyzer.c:25078`）：

```c
case FENG_DECL_TYPE: {
    size_t arity = decl->as.type_decl.type_param_count;

    // 先检查跨 kind 冲突
    if (has_cross_kind_conflict(visible_types, visible_type_count,
                                decl->as.type_decl.name, FENG_DECL_TYPE)) {
        char *message = format_message("type declaration '%.*s' conflicts with an existing visible type name",
                                       (int)decl->as.type_decl.name.length,
                                       decl->as.type_decl.name.data);
        ok = append_error(errors, error_count, error_capacity,
                          program->path, *decl_token(decl), "AE0213", message);
        break;
    }

    // 再检查同 kind 冲突（按 name + arity）
    index = find_visible_type_index(visible_types, visible_type_count,
                                    decl->as.type_decl.name, arity);
    if (index < visible_type_count) {
        char *message = format_message("duplicate type declaration '%.*s'",
                                       (int)decl->as.type_decl.name.length,
                                       decl->as.type_decl.name.data);
        ok = append_error(errors, error_count, error_capacity,
                          program->path, *decl_token(decl), "AE0213", message);
        break;
    }
    // 同名不同 arity → 允许注册为新 entry
    entry.name = decl->as.type_decl.name;
    entry.provider_module = module;
    entry.provider_program = program;
    entry.decl = decl;
    ok = append_raw((void **)&visible_types, &visible_type_count,
                    &visible_type_capacity, sizeof(entry), &entry);
    break;
}
```

**FENG_DECL_SPEC 分支**（`src/semantic/analyzer.c:25320`）：同理改造。

**FENG_DECL_ENUM 分支**（`src/semantic/analyzer.c:25110`）：enum 无泛型参数，arity 固定为 0，逻辑不变。

### 3.2 跨 kind 冲突规则

- 同 kind（type vs type, spec vs spec）：按 `(name, arity)` 判定，同名不同 arity 允许
- 不同 kind（type vs spec, type vs enum, spec vs enum）：仍按 `name` 判定，同名即冲突（规范说"同 kind"才按 arity 区分）

**实现**：提取辅助函数 `has_cross_kind_conflict`：

```c
static bool has_cross_kind_conflict(const VisibleTypeEntry *entries,
                                    size_t count,
                                    FengSlice name,
                                    FengDeclKind new_kind) {
    for (size_t i = 0U; i < count; ++i) {
        if (slice_equals(entries[i].name, name) &&
            entries[i].decl->kind != new_kind) {
            return true;
        }
    }
    return false;
}
```

注册前调用检查，若返回 true 则报跨 kind 冲突。

---

## 4 类型引用解析改造

### 4.1 使用点 arity 确定

类型引用时，arity 来自 `type_ref->as.named.type_arg_count`：

- `UserType<int>` → arity = 1
- `UserType<int, string>` → arity = 2
- `UserType`（无 `<...>`）→ arity = 0

### 4.2 find_visible_type_decl 改造

`find_visible_type_decl`（`src/semantic/analyzer.c:2380`）改为接受 `type_param_count`：

```c
static const FengDecl *find_visible_type_decl(const VisibleTypeEntry *entries,
                                              size_t count,
                                              FengSlice name,
                                              size_t type_param_count);
```

**调用点**（3 处）：

| 行号 | 上下文 | arity 说明 |
|------|--------|------------|
| L4522 | `find_named_type_decl` 内部 | 从参数透传 |
| L7930 | match enum target 解析 | 查找 enum 名，enum 无泛型，arity = 0 |
| L8660 | match label 中 enum 引用 | 查找 enum 名，enum 无泛型，arity = 0 |

### 4.3 find_named_type_decl 改造

`find_named_type_decl`（`src/semantic/analyzer.c:4507`）改为接受并传递 `type_param_count`：

```c
static const FengDecl *find_named_type_decl(const ResolveContext *context,
                                            const FengSlice *segments,
                                            size_t segment_count,
                                            size_t type_param_count);
```

### 4.4 调用点改造

#### 4.4.1 find_named_type_decl 调用点（4 处）

| 行号 | 上下文 | arity 来源 | 说明 |
|------|--------|------------|------|
| L4756 | `resolve_type_ref_decl` 内部 | `type_ref->as.named.type_arg_count` | 主要类型引用解析入口 |
| L20403 | `resolve_type_ref` 中 `type_arg_count > 0` 分支 | 局部变量 `type_arg_count` | 带类型参数的引用 |
| L20492 | `resolve_type_ref` 中 `segment_count == 1` 无类型参数分支 | 0 | 裸名引用，无 `<...>` |
| L20506 | `resolve_type_ref` 中多段路径分支 | 0 | 模块限定路径，无 `<...>` |

#### 4.4.2 resolve_type_ref_decl 调用点（多处）

`resolve_type_ref_decl`（`src/semantic/analyzer.c:4746`）改造后内部通过 `type_ref->as.named.type_arg_count` 确定 arity 并传递给 `find_named_type_decl`。其调用者不需要修改，arity 由 type_ref 自身确定。

### 4.5 错误处理与 arity 验证重构

**当前流程**（`resolve_type_ref` L20391–20457，`type_arg_count > 0` 分支）：

```
1. find_named_type_decl(name)          → 按名称找到 decl
2. 计算 expected_arity                  → 从 decl 提取
3. 验证 expected_arity != 0             → 否则报 AE1014 "not a generic type"
4. 验证 type_arg_count == expected_arity → 否则报 AE1015 "expects N but got M"
5. materialize constraint witnesses
```

**改造后流程**：`find_named_type_decl` 按 `(name, type_arg_count)` 精确匹配，步骤 2–4 的验证变为冗余。需重构为：

```
1. find_named_type_decl(name, type_arg_count) → 按 (name, arity) 精确查找
2. 若返回 NULL：
   a. 先按名称查找（不传 arity）判断是否存在同名类型
   b. 存在同名但 arity 不匹配 → 报专用错误（如 "type 'Box' with N type argument(s) not found"）
   c. 不存在 → 报 AE1013 "unknown type"
3. 若返回非 NULL → materialize constraint witnesses
```

**错误信息质量**：不能简单报 "unknown type"，需区分：
- 同名不同 arity 的类型存在 → 给出更精确的提示
- 完全不存在 → 报 AE1013

**实现要点**：步骤 2a 可复用 `find_visible_type_any_arity`（§2.3）。

---

## 5 影响面分析

### 5.1 核心影响

| 模块 | 文件 | 改动点 |
| ------ | ------ | -------- |
| 查找函数 | `src/semantic/analyzer.c` | `find_visible_type_index` / `find_visible_type` / `find_visible_type_decl` / `find_named_type_decl` + 新增 `find_visible_type_any_arity` / `decl_type_param_count` |
| 冲突检查 | `src/semantic/analyzer.c` | `check_symbol_conflicts` 中 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` 分支 + 新增 `has_cross_kind_conflict` |
| 类型解析 | `src/semantic/analyzer.c` | `resolve_type_ref`（L20391–20457 arity 验证重构）/ `resolve_type_ref_decl` 及所有调用点 |
| 存在性检查 | `src/semantic/analyzer.c` | `find_unshadowed_alias`（L4500）/ `use` 声明冲突检查（L20305）/ 标识符解析（L20809）改用 `find_visible_type_any_arity` |
| 模块导入 | `src/semantic/analyzer.c` | `import_public_names` 中类型注册逻辑 + `seen_type_names` 去重机制 |
| 模块导出 | `src/symbol/export.c` | 导出时需携带 arity 信息 |
| 符号提供 | `src/symbol/provider.c` | 导入时按 `(name, arity)` 注册 |

### 5.2 关联影响

| 模块 | 文件 | 说明 |
| ------ | ------ | ------ |
| 可见类型复制 | `src/semantic/analyzer.c` | `copy_visible_type_entries`（L2835）是纯 memcpy，结构不变则无需改动 |
| 代码生成 | `src/codegen/codegen.c` | mangling 需包含 arity 信息，避免同名不同 arity 的类型符号冲突 |
| 符号表序列化 | `src/symbol/ft_write.c` / `ft_read.c` | `.ft` 文件格式需支持同名不同 arity 的类型条目 |
| LSP | `src/cli/lsp/runtime.c` | 补全、跳转、hover 需感知多 arity，按使用点 arity 精确匹配 |
| 约束见证 | `src/semantic/analyzer.c` | `materialize_named_type_param_constraint_witnesses` 每种 arity 独立实例化 |
| 测试 | `test/` / `fcts/` | 新增诊断测试和行为兼容测试 |

### 5.3 不受影响

- 函数泛型重载：已实现，逻辑独立
- 方法泛型重载：已实现，逻辑独立
- 非泛型类型：arity = 0，走现有逻辑
- 运行时：泛型在编译期完成实例化，运行时不涉及 arity 判定

---

## 6 实施步骤

按 CLAUDE.md "先规范，再实现，后测试"原则：

### 6.1 规范确认

- [ ] 确认 `docs/feng-generics-draft.md` 中相关规范是否完整
- [ ] 确认是否需要补充错误码（如 arity 不匹配时的专用错误码）
- [ ] 确认跨 kind 冲突规则（type vs spec 同名是否允许）

### 6.2 数据结构与查找函数

- [ ] 实现 `decl_type_param_count` 辅助函数
- [ ] 改造 `find_visible_type_index` 接受 `type_param_count` 参数
- [ ] 新增 `find_visible_type_any_arity` 辅助函数（存在性检查）
- [ ] 改造 `find_visible_type` 接受 `type_param_count` 参数（精确匹配）
- [ ] 改造 `find_visible_type_decl` 接受 `type_param_count` 参数
- [ ] 改造 `find_named_type_decl` 接受并传递 `type_param_count` 参数
- [ ] 验证 `copy_visible_type_entries`（L2835）无需改动（纯 memcpy，结构不变）

### 6.3 冲突检查

- [ ] 新增 `has_cross_kind_conflict` 辅助函数
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_TYPE` 分支
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_SPEC` 分支
- [ ] 添加跨 kind 冲突检查逻辑（调用 `has_cross_kind_conflict`）

### 6.4 类型引用解析

- [ ] 改造 `resolve_type_ref_decl`（L4746）：从 `type_ref->as.named.type_arg_count` 提取 arity 传递给 `find_named_type_decl`
- [ ] 改造 `resolve_type_ref`（L20391–20457）arity 验证逻辑：
  - 有类型参数分支（L20403）：`find_named_type_decl` 传入 `type_arg_count`
  - 无类型参数裸名分支（L20492）：`find_named_type_decl` 传入 arity = 0
  - 多段路径分支（L20506）：`find_named_type_decl` 传入 arity = 0
- [ ] 重构 arity 验证：原 L20415–20437 的 AE1014/AE1015 验证改为「查找失败后区分同名不同 arity vs 完全不存在」
- [ ] 改造存在性检查调用点：
  - `find_unshadowed_alias`（L4500）：改用 `find_visible_type_any_arity`
  - `use` 声明别名冲突检查（L20305）：改用 `find_visible_type_any_arity`
  - 标识符解析（L20809）：改用 `find_visible_type_any_arity`
- [ ] 改造精确匹配调用点：
  - 泛型目标类型推断（L14658）：传递上下文 arity
  - 约束 spec 查找（L14691）：传递 `cref->as.named.type_arg_count`
- [ ] 改造 enum 专用调用点（arity = 0）：
  - match enum target（L7930）
  - match label enum 引用（L8660）

### 6.5 模块导入导出

- [ ] 改造 `import_public_names`（L20015）：
  - `find_visible_type_index` 调用传入 arity（L20060/L20092/L20201）
  - `seen_type_names` 去重机制（L20052/L20084/L20193）改为 `(name, arity)` 复合去重（将 `FengSlice *` 改为结构体数组或直接用 `find_visible_type_index` 替代去重）
  - `FENG_DECL_SPEC` 分支（L20189）同理改造
- [ ] 改造 `src/symbol/export.c`，导出时携带 arity
- [ ] 改造 `src/symbol/ft_write.c` / `ft_read.c`，序列化支持 arity

### 6.6 代码生成

- [ ] 改造 `src/codegen/codegen.c`，mangling 包含 arity 信息
- [ ] 验证同名不同 arity 的类型生成符号不冲突

### 6.7 LSP

- [ ] 改造 `src/cli/lsp/runtime.c`，补全/跳转/hover 感知多 arity
- [ ] 按使用点 arity 精确匹配，不展示所有 arity 候选

### 6.8 测试

- [ ] `test/` 新增诊断测试：同名同 arity 冲突（AE0213）、同名不同 arity 允许、跨 kind 冲突
- [ ] `test/` 新增解析测试：使用点 arity 精确匹配、arity 不匹配报错
- [ ] `fcts/` 新增行为测试：同名不同 arity 的类型独立使用、泛型实例化正确

---

## 7 测试用例设计

### 7.1 诊断测试（`test/`）

**允许场景**：

```feng
type Box<T> { value: T }
type Box<T, U> { first: T, second: U }

spec Reader<T> { func read(): T }
spec Reader<T, U> { func read(): T, func write(value: U) }
```

**冲突场景**：

```feng
type Box<T> { value: T }
type Box<U> { data: U }  // AE0213: duplicate type declaration 'Box'

spec Reader<T> { func read(): T }
spec Reader<U> { func read(): U }  // AE0213: duplicate type declaration 'Reader'
```

**跨 kind 冲突**：

```feng
type Box<T> { value: T }
spec Box<T> { func get(): T }  // AE0213 或新错误码：type/spec 名称冲突
```

### 7.2 解析测试（`test/`）

**精确匹配**：

```feng
let a: Box<int> = Box<int>(value: 1);           // 匹配 Box<T>
let b: Box<int, string> = Box<int, string>(...); // 匹配 Box<T, U>
```

**arity 不匹配**：

```feng
let c: Box<int, string, bool> = ...;  // AE1013 或新错误码：unknown type 'Box' with 3 type argument(s)
```

### 7.3 行为测试（`fcts/`）

**独立使用**：

```feng
type Container<T> {
  value: T
  func get(): T { return self.value }
}

type Container<T, U> {
  first: T
  second: U
  func swap(): Container<U, T> {
    return Container<U, T>(first: self.second, second: self.first)
  }
}

func main() {
  let c1 = Container<int>(value: 42)
  let c2 = Container<int, string>(first: 1, second: "hello")
  let c3 = c2.swap()
  assert(c3.first == "hello")
  assert(c3.second == 1)
}
```

---

## 8 风险与边界

### 8.1 风险点

- **改动面广**：涉及语义分析、符号表、代码生成、LSP 等多个模块，需全量回归测试
- **错误码调整**：可能需要新增 arity 不匹配的专用错误码，需同步更新文档
- **性能影响**：查找时额外比对 arity，但影响极小（线性扫描 + 整数比较）

### 8.2 边界情况

- **arity = 0**：非泛型类型，走现有逻辑，不受影响
- **泛型参数默认值**：当前规范不支持，无需考虑
- **可变参数泛型**：当前规范不支持，无需考虑
- **跨模块同名**：不同模块的同名类型通过模块限定符区分，不受影响

### 8.3 不做的事

- 不支持泛型参数默认值（规范未定义）
- 不支持可变参数泛型（规范未定义）
- 不支持按泛型约束不同区分重载（规范明确禁止）
- 不支持按泛型参数名不同区分重载（规范明确禁止）

---

## 9 待办任务

- [ ] 确认规范完整性，必要时补充 `docs/feng-generics-draft.md`
- [ ] 实现 `decl_type_param_count` 辅助函数
- [ ] 新增 `find_visible_type_any_arity` 辅助函数
- [ ] 新增 `has_cross_kind_conflict` 辅助函数
- [ ] 改造 `find_visible_type_index` / `find_visible_type` / `find_visible_type_decl` / `find_named_type_decl`
- [ ] 验证 `copy_visible_type_entries` 无需改动
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` 分支 + 跨 kind 冲突检查
- [ ] 改造 `resolve_type_ref_decl` 及 `resolve_type_ref` 中的 arity 验证逻辑（含 AE1014/AE1015 重构）
- [ ] 改造存在性检查调用点（`find_unshadowed_alias` L4500 / `use` 声明冲突 L20305 / 标识符解析 L20809）
- [ ] 改造精确匹配调用点（L14658 泛型目标推断 / L14691 约束 spec 查找）
- [ ] 改造 enum 专用调用点（L7930 / L8660，arity = 0）
- [ ] 改造 `import_public_names` 类型注册逻辑 + `seen_type_names` 去重机制
- [ ] 改造 `src/symbol/export.c` / `ft_write.c` / `ft_read.c`
- [ ] 改造 `src/codegen/codegen.c` mangling
- [ ] 改造 `src/cli/lsp/runtime.c`
- [ ] 新增 `test/` 诊断测试
- [ ] 新增 `test/` 解析测试
- [ ] 新增 `fcts/` 行为测试
- [ ] 全量回归测试

---

## 10 关联文档

- [docs/feng-generics-draft.md](../docs/feng-generics-draft.md)：泛型规范与语法定义
- [docs/feng-type.md](../docs/feng-type.md)：类型系统规范
- [docs/feng-spec.md](../docs/feng-spec.md)：spec 规范
- [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md)：错误码定义
- [CLAUDE.md](../CLAUDE.md)：开发原则与流程
