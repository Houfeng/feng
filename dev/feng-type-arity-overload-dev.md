# Feng Type/Spec 按泛型参数个数重载开发文档

> 规范来源：[docs/feng-generics-draft.md](../docs/feng-generics-draft.md) §4-§6
> 本文记录实现方案、数据结构改造、影响面分析与待办任务。

---

## 0 现状与问题

### 0.1 函数泛型重载（已实现，本次不改动）

`compute_overload_match_priority`（`src/semantic/analyzer.c:10587`）已将 `type_param_count` 纳入重载决议：

- 有显式类型实参时：`type_param_count == explicit_type_arg_count` 为最优匹配（优先级 0）
- 无显式类型实参时：非泛型候选优先于泛型候选

`FunctionOverloadSetEntry` 按名称聚合多个 decl，允许同名不同泛型参数个数的函数共存。函数重载是多维的（泛型 arity + 参数个数 + 参数类型），与 type/spec 仅按 arity 重载不同。**本次优化不改动函数重载逻辑**——FUNCTION 列入 category 体系（§0.3）仅为参与跨 category name-only 冲突检测。

### 0.2 Type/Spec 泛型重载（未实现）

**规范已明确要求**（`docs/feng-generics-draft.md`）：

- 正确语法九：`type UserType<T>` 和 `type UserType<T, U>` 应允许共存
- [必须] "同一作用域内,同 kind 的具名泛型 type 与具名泛型 spec 的声明 identity 按'名称 + 泛型参数数量'确定"
- [必须] "所有具名泛型 type / spec 的使用位置都必须按'名称 + 泛型参数数量'精确解析到已存在声明"

> **规范措辞待同步**：原文用"同 kind"，本次实现已将冲突维度从 `FengDeclKind` 收紧为 7 类 category（§0.3）。需同步更新 `docs/feng-generics-draft.md` 的措辞为"同 category"。

### 0.3 支持 arity 重载的声明形式（完整清单）

所有带泛型参数的 type / spec 子形式均按 `(name, arity)` 做 identity 判定。共 7 类 category（§0.3 表），由 `kind + is_tuple + form` 派生；其中 6 类支持 arity 重载，不支持泛型的声明（enum / fit / global_binding 等）统一归 `NO_OVERLOADING`：

| category | 声明形式 | 语法示例 | 派生条件 | 支持 arity 重载 |
| -------- | -------- | -------- | -------- | --------------- |
| 1. FUNCTION | 函数声明 | `func foo<T>()` | `kind == FENG_DECL_FUNCTION` | ✅（多维：arity + 参数个数 + 参数类型） |
| 2. TYPE | 普通 type + @value type | `type User<T> {}` / `@value type User<T> {}` | `kind == FENG_DECL_TYPE && is_tuple == false` | ✅（仅 arity） |
| 3. TUPLE | 元组类型 | `type User<T1, T2>(T1, T2)` | `kind == FENG_DECL_TYPE && is_tuple == true` | ✅（仅 arity） |
| 4. SPEC_OBJECT | 对象契约 | `spec User<T> {}` | `kind == FENG_DECL_SPEC && form == FENG_SPEC_FORM_OBJECT` | ✅（仅 arity） |
| 5. SPEC_CALLABLE | 函数契约 | `spec User<T>()` | `kind == FENG_DECL_SPEC && form == FENG_SPEC_FORM_CALLABLE` | ✅（仅 arity） |
| 6. SPEC_UNION | 联合契约 | `spec User<T1, T2>: T1 \| T2` | `kind == FENG_DECL_SPEC && form == FENG_SPEC_FORM_UNION` | ✅（仅 arity） |
| 7. NO_OVERLOADING | enum / fit / global_binding 等 | `enum Color { ... }` / `fit Foo` | 其他（不支持泛型） | ❌（同名即冲突） |

> **FUNCTION 与 type/spec 重载维度不同**：函数的重载是多维的——泛型 arity + 参数个数 + 参数类型，由现有 `FunctionOverloadSetEntry` / `compute_overload_match_priority`（`src/semantic/analyzer.c:10587`）处理，已实现。本次优化**不改动函数重载逻辑**，FUNCTION 列入 category 体系仅为：(a) 参与 §3.0 规则 2 的跨 category name-only 冲突检测（函数名与 type/spec/enum 同名即冲突）；(b) 表明 FUNCTION 同 category 内不适用本次新增的 (name, arity) 单维重载规则。规则 3 的 (name, arity) 仅适用于 5 类 type/spec category。

> **category 划分理由**：当前 `FengDeclKind`（`src/parser/parser.h:436`）粒度太粗——普通 type 和元组都归 `FENG_DECL_TYPE`，三种 spec form 都归 `FENG_DECL_SPEC`。但语义上它们是不同重载面，跨子形式同名即应冲突（详见 §3.0 规则 2）。仅普通 type 与 @value type 语义同构（`is_value` 不参与 category 区分），归为同类。enum / fit / global_binding 等不支持泛型的声明统一归 `NO_OVERLOADING`——命名 `FengOverloadCategory` 本身体现"按重载能力分类"，不支持重载的归一类更自洽。

> **子形式冲突规则（已决策）**：按 category 区分冲突面。
> - **FUNCTION**：同 category 内的重载是**多维**的（泛型 arity + 参数个数 + 参数类型），由现有 `FunctionOverloadSetEntry` / `compute_overload_match_priority` 处理，**本次不改动**；跨 category name-only 冲突（函数名与 type/spec/enum 同名即冲突）。
> - **5 类 type/spec category**（TYPE / TUPLE / SPEC_OBJECT / SPEC_CALLABLE / SPEC_UNION）：**同 category 内**按 `(name, arity)` 区分（同名同 arity 冲突、同名不同 arity 允许共存）；**跨 category** name-only 冲突（同名即冲突，不看 arity）。这是本次新增的重载规则。
> - **NO_OVERLOADING**（enum / fit / global_binding 等）：**无论同 category 还是跨 category，一律 name-only 判定冲突**（同名即冲突，不看 arity）。这类不支持泛型，无 arity 维度。
>
> 示例：
> - `type Box<T> {}`（普通 type）与 `type Box<T, U>(T, U)`（元组）→ 跨 category → **冲突**（不看 arity）
> - `spec Foo<T> {}`（object spec）与 `spec Foo<T>()`（callable spec）→ 跨 category → **冲突**（不看 arity）
> - `type Box<T> {}` 与 `spec Box<T> {}` → 跨 category → **冲突**
> - `type Box<T> {}` 与 `enum Box` → 跨 category → **冲突**
> - `type Box<T> {}` 与 `func Box<T>()` → 跨 category → **冲突**（函数名与 type 同名）
> - `type Box<T> {}` 与 `type Box<T, U> {}`（同为普通 type）→ 同 category，不同 arity → **允许共存**
> - `type Box<T> {}` 与 `@value type Box<T> {}`（同类，`is_value` 不参与）→ 同 category，同 arity → **冲突**
> - `func foo<T>()` 与 `func foo<T, U>(x: int)` → 同 category（FUNCTION），走现有函数重载机制（多维决议），**不在本次 (name, arity) 规则范围内**
> - `enum Color { ... }` 与 `enum Color { ... }` → 同 category（NO_OVERLOADING），name-only → **冲突**
> - `enum Foo` 与 `fit Foo` → 同 category（NO_OVERLOADING），name-only → **冲突**
> - `fit Foo` 与 `fit Foo` → 同 category（NO_OVERLOADING），name-only → **冲突**

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
| L14691 | 约束 spec 查找 | `cref->as.named.type_arg_count` |

**存在性检查场景**（只需判断是否存在任意 arity 的同名类型）：

| 行号 | 上下文 | 说明 |
|------|--------|------|
| L4500 | `find_unshadowed_alias` — alias 是否被类型遮蔽 | 只需知道是否存在同名类型 |
| L14658 | `resolve_type_target_expr` — 裸标识符解析类型目标 | 裸标识符不携带 arity，只需判断是否为已知类型名 |
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

从 `FengDecl` 派生 `category`（§0.3 的 7 类冲突面）：

```c
typedef enum {
    FENG_OVERLOAD_CATEGORY_FUNCTION = 0,
    FENG_OVERLOAD_CATEGORY_TYPE,              // 普通 type + @value type
    FENG_OVERLOAD_CATEGORY_TUPLE,             // 元组
    FENG_OVERLOAD_CATEGORY_SPEC_OBJECT,
    FENG_OVERLOAD_CATEGORY_SPEC_CALLABLE,
    FENG_OVERLOAD_CATEGORY_SPEC_UNION,
    FENG_OVERLOAD_CATEGORY_NO_OVERLOADING     // enum / fit / global_binding 等（不支持泛型，始终 name-only）
} FengOverloadCategory;

static FengOverloadCategory decl_overload_category(const FengDecl *decl) {
    switch (decl->kind) {
        case FENG_DECL_FUNCTION:
            return FENG_OVERLOAD_CATEGORY_FUNCTION;
        case FENG_DECL_TYPE:
            return decl->as.type_decl.is_tuple
                ? FENG_OVERLOAD_CATEGORY_TUPLE
                : FENG_OVERLOAD_CATEGORY_TYPE;  // is_value 不参与区分
        case FENG_DECL_SPEC:
            switch (decl->as.spec_decl.form) {
                case FENG_SPEC_FORM_OBJECT:   return FENG_OVERLOAD_CATEGORY_SPEC_OBJECT;
                case FENG_SPEC_FORM_CALLABLE: return FENG_OVERLOAD_CATEGORY_SPEC_CALLABLE;
                case FENG_SPEC_FORM_UNION:    return FENG_OVERLOAD_CATEGORY_SPEC_UNION;
            }
            return FENG_OVERLOAD_CATEGORY_NO_OVERLOADING;
        default:
            return FENG_OVERLOAD_CATEGORY_NO_OVERLOADING;  // enum / fit / global_binding
    }
}
```

`is_value` 不参与 category 派生——普通 type 与 @value type 同属 `FENG_OVERLOAD_CATEGORY_TYPE`（语义同构，详见 §0.3）。enum / fit / global_binding 等不支持泛型的声明统一归 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`：无 arity 维度，**无论同 category 还是跨 category 都走 name-only 冲突**（同名即冲突）。命名 `FengOverloadCategory` 本身体现"按重载能力分类"，不支持重载的归一类更自洽。

---

## 3 冲突检查改造

### 3.0 冲突判定规则总览

三条规则，明确哪些是现有行为、哪些是本次新增：

| 规则 | 场景 | 判定维度 | 状态 |
|------|------|----------|------|
| 规则 1 | 不同模块，import 到同一模块 | 仅按 **name** | **现有规则，不变** |
| 规则 2 | 相同模块，不同 category（§0.3 的 7 类之间） | 仅按 **name** | **本次调整**：判定维度从 kind 收紧为 category |
| 规则 3 | 相同模块，相同 category | 5 类 type/spec category 按 **(name, arity)**；FUNCTION 按现有函数重载机制（多维，不变）；NO_OVERLOADING 按 **name** | **本次新增**：5 类 type/spec 同名不同 arity 允许共存 |

- 规则 1 影响：`import_public_names` 中 `seen_type_names` 去重保持 name-only，跨模块冲突检查改用 `find_visible_type_any_arity`（§2.3），**行为不变**
- 规则 2 影响：`check_symbol_conflicts` 中跨 category 冲突检查按 name 判定；相比原 kind 维度，category 更细——同 `FENG_DECL_TYPE` 的普通 type 与元组、同 `FENG_DECL_SPEC` 的三种 form、enum/fit 与其他 category 现在分属不同 category，同名即冲突
- 规则 3 影响：`check_symbol_conflicts` 中同 category 冲突检查：5 类 type/spec category 改为按 `(name, arity)` 判定（**核心改动点**）；FUNCTION 仍走现有 `FunctionOverloadSetEntry` / `compute_overload_match_priority` 多维重载机制（**不改动**）；NO_OVERLOADING 仍按 name 判定（现有行为，不变）

> **规则 2 与规则 3 的关系**：规则 2 在 category 维度判定"跨面冲突"（一律 name-only）；规则 3 在 category 维度判定"同面重载"——5 类 type/spec 按 (name, arity)，FUNCTION 按现有函数多维重载，NO_OVERLOADING 按 name（即同面也是 name-only，等价于规则 2 的行为）。两者均以 category 为边界，取代原文档的 kind 维度判定。
> **FUNCTION 的特殊性**：函数重载维度多于 type/spec（泛型 arity + 参数个数 + 参数类型），已由现有机制实现，本次不改动。FUNCTION 列入 category 体系仅为参与规则 2 的跨 category name-only 冲突检测。
> **NO_OVERLOADING 的特殊性**：这类（enum / fit / global_binding 等）不支持泛型，无 arity 维度，因此无论同面还是跨面都走 name-only。可理解为"规则 3 对它退化为 name-only"。
> **设计取舍（C# vs Rust 之间选中间道路）**：考虑过两种极端——① C# 方向：类型标识为 (name, arity)，kind 不参与，跨类别允许 arity 重载（CLR 用 `Foo`1` / `Foo`2` 反引号记法区分）。灵活但易混淆：可能写出 `Foo<T>()` 是函数、`Foo<T1,T2>` 是类型/元组，使用者难从语法直接判断实体类别。② Rust 方向：纯 name-only（E0428 同名即冲突），清晰但失去灵活性，像 `Func<R>` / `Func<R,T1>` / `Func<R,T1,T2>` 这样的同形函数族定义不出来。Feng 选中间道路：同 category 内按 (name, arity) 重载（规则 3），跨 category name-only 冲突（规则 2）——既保留同形实体族（不同 arity 的元组/spec）的扩展性，又避免跨形歧义。

### 3.1 check_symbol_conflicts 分支改造

**FENG_DECL_TYPE 分支**（`src/semantic/analyzer.c:25078`）：

```c
case FENG_DECL_TYPE: {
    size_t arity = decl->as.type_decl.type_param_count;
    FengOverloadCategory category = decl_overload_category(decl);

    // 先检查跨 category 冲突（规则 2，name-only）
    if (has_cross_category_conflict(visible_types, visible_type_count,
                                    decl->as.type_decl.name, category)) {
        char *message = format_message("type declaration '%.*s' conflicts with an existing visible name in a different category",
                                       (int)decl->as.type_decl.name.length,
                                       decl->as.type_decl.name.data);
        ok = append_error(errors, error_count, error_capacity,
                          program->path, *decl_token(decl), "AE0213", message);
        break;
    }

    // 再检查同 category 冲突（规则 3，按 name + arity）
    index = find_visible_type_index(visible_types, visible_type_count,
                                    decl->as.type_decl.name, category, arity);
    if (index < visible_type_count) {
        char *message = format_message("duplicate type declaration '%.*s'",
                                       (int)decl->as.type_decl.name.length,
                                       decl->as.type_decl.name.data);
        ok = append_error(errors, error_count, error_capacity,
                          program->path, *decl_token(decl), "AE0213", message);
        break;
    }
    // 同 category 同名不同 arity → 允许注册为新 entry
    entry.name = decl->as.type_decl.name;
    entry.provider_module = module;
    entry.provider_program = program;
    entry.decl = decl;
    ok = append_raw((void **)&visible_types, &visible_type_count,
                    &visible_type_capacity, sizeof(entry), &entry);
    break;
}
```

**FENG_DECL_SPEC 分支**（`src/semantic/analyzer.c:25320`）：同理改造，category 由 `decl_overload_category` 按 `form` 派生（OBJECT / CALLABLE / UNION 分属不同 category）。

**FENG_DECL_ENUM 分支**（`src/semantic/analyzer.c:25110`）：enum 派生为 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`，arity 固定为 0。无 arity 维度——跨 category 冲突按 name 判定（与 type/spec 同名即冲突）；同 category 内也按 name 判定（同名即冲突）。两者均为现有行为，逻辑不变。

> **FENG_DECL_FIT / FENG_DECL_GLOBAL_BINDING 等**：同样派生为 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`，行为与 enum 相同——无论同面还是跨面都走 name-only 冲突。若后续 fit 需要支持泛型，可单独提升为独立 category。

### 3.2 跨 category 冲突检查（规则 2）

不同 category（§0.3 的 7 类之间）按 `name` 判定，同名即冲突，不看 arity。相比原 kind 维度，category 更细：同 `FENG_DECL_TYPE` 的普通 type 与元组、同 `FENG_DECL_SPEC` 的三种 form、enum 与其他 category 现在分属不同 category，同名即冲突。

**实现**：新增辅助函数 `has_cross_category_conflict`，在 type/spec 分支注册前调用：

```c
static bool has_cross_category_conflict(const VisibleTypeEntry *entries,
                                        size_t count,
                                        FengSlice name,
                                        FengOverloadCategory new_category) {
    for (size_t i = 0U; i < count; ++i) {
        if (slice_equals(entries[i].name, name) &&
            decl_overload_category(entries[i].decl) != new_category) {
            return true;
        }
    }
    return false;
}
```

注册前调用检查，若返回 true 则报跨 category 冲突。

> **与原 `has_cross_kind_conflict` 的差异**：原函数比对 `decl->kind != new_kind`，粒度为 `FengDeclKind`（type/spec/enum/function）；新函数比对 `decl_overload_category(decl) != new_category`，粒度为 7 类 category。普通 type 与元组、三种 spec form 在原 kind 维度下不冲突，在 category 维度下冲突。

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

### 4.6 裸名分支错误信息改进

**当前流程**（`resolve_type_ref` L20460–20508，裸名无类型参数分支）：

```
1. find_named_type_decl(name) → 按名称找到 decl（当前实现）
2. 若返回非 NULL → return true
3. 若返回 NULL → 继续后续查找（alias 等），最终报 "unknown type"
```

**改造后流程**：`find_named_type_decl` 改为按 `(name, 0)` 精确匹配。当返回 NULL 时，同样需要区分：

```
1. find_named_type_decl(name, 0) → 按 (name, arity=0) 精确查找
2. 若返回非 NULL → return true
3. 若返回 NULL：
   a. 用 find_visible_type_any_arity 判断是否存在同名类型
   b. 存在同名但 arity ≠ 0 → 报专用错误（如 "'Box' requires type arguments"）
   c. 不存在 → 继续后续查找（alias 等）
```

**实现要点**：与 §4.5 共享 `find_visible_type_any_arity` 辅助函数和错误信息模式。

---

## 5 影响面分析

### 5.1 核心影响

| 模块 | 文件 | 改动点 |
| ------ | ------ | -------- |
| 查找函数 | `src/semantic/analyzer.c` | `find_visible_type_index` / `find_visible_type` / `find_visible_type_decl` / `find_named_type_decl` + 新增 `find_visible_type_any_arity` / `decl_type_param_count` / `decl_overload_category` |
| 冲突检查 | `src/semantic/analyzer.c` | `check_symbol_conflicts` 中 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` / `FENG_DECL_ENUM` 分支 + 新增 `has_cross_category_conflict` |
| 类型解析 | `src/semantic/analyzer.c` | `resolve_type_ref`（L20391–20457 arity 验证重构）/ `resolve_type_ref_decl` 及所有调用点 |
| 存在性检查 | `src/semantic/analyzer.c` | `find_unshadowed_alias`（L4500）/ `resolve_type_target_expr`（L14658）/ `use` 声明冲突检查（L20305）/ 标识符解析（L20809）改用 `find_visible_type_any_arity` |
| 模块导入 | `src/semantic/analyzer.c` | `import_public_names` 保持 name-only（规则 1，不变），改用 `find_visible_type_any_arity` |
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

- **函数重载**：已实现（`FunctionOverloadSetEntry` / `compute_overload_match_priority`，多维：泛型 arity + 参数个数 + 参数类型），本次**不改动**。FUNCTION 列入 category 体系仅为参与跨 category name-only 冲突检测，同 category 内仍走现有函数重载机制
- 方法泛型重载：已实现，逻辑独立
- 非泛型类型：arity = 0，走现有逻辑
- 运行时：泛型在编译期完成实例化，运行时不涉及 arity 判定

---

## 6 实施步骤

按 CLAUDE.md "先规范，再实现，后测试"原则：

### 6.1 规范确认

- [ ] 同步更新 `docs/feng-generics-draft.md`：将"同 kind"措辞改为"同 category"（§0.3 的 7 类）
- [ ] 确认 `docs/feng-generics-draft.md` 中相关规范是否完整
- [ ] 确认是否需要补充错误码（如 arity 不匹配时的专用错误码）
- [x] ~~确认跨 kind 冲突规则~~ — 已决策：规则 2，同模块不同 category 按 name 判定冲突（从 kind 收紧为 category）

### 6.2 数据结构与查找函数

- [ ] 实现 `decl_type_param_count` 辅助函数
- [ ] 实现 `decl_overload_category` 辅助函数（§2.4，派生 7 类 category）
- [ ] 改造 `find_visible_type_index` 接受 `category` + `type_param_count` 参数（同面精确匹配）
- [ ] 新增 `find_visible_type_any_arity` 辅助函数（存在性检查，name-only）
- [ ] 改造 `find_visible_type` 接受 `type_param_count` 参数（精确匹配）
- [ ] 改造 `find_visible_type_decl` 接受 `type_param_count` 参数
- [ ] 改造 `find_named_type_decl` 接受并传递 `type_param_count` 参数
- [ ] 验证 `copy_visible_type_entries`（L2835）无需改动（纯 memcpy，结构不变）

### 6.3 冲突检查

- [ ] 新增 `has_cross_category_conflict` 辅助函数（规则 2，跨 category name-only 检查）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_TYPE` 分支（规则 2 + 规则 3，按 category 判定）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_SPEC` 分支（规则 2 + 规则 3，按 category 判定）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_ENUM` 分支（派生 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`，行为不变）

### 6.4 类型引用解析

- [ ] 改造 `resolve_type_ref_decl`（L4746）：从 `type_ref->as.named.type_arg_count` 提取 arity 传递给 `find_named_type_decl`
- [ ] 改造 `resolve_type_ref`（L20391–20457）arity 验证逻辑：
  - 有类型参数分支（L20403）：`find_named_type_decl` 传入 `type_arg_count`
  - 无类型参数裸名分支（L20492）：`find_named_type_decl` 传入 arity = 0
  - 多段路径分支（L20506）：`find_named_type_decl` 传入 arity = 0
- [ ] 重构 arity 验证：原 L20415–20437 的 AE1014/AE1015 验证改为「查找失败后区分同名不同 arity vs 完全不存在」
- [ ] 改造裸名分支错误信息（L20492/L20506）：`find_named_type_decl(name, 0)` 返回 NULL 时，用 `find_visible_type_any_arity` 区分「同名但需泛型参数」vs「完全不存在」
- [ ] 改造存在性检查调用点：
  - `find_unshadowed_alias`（L4500）：改用 `find_visible_type_any_arity`
  - `resolve_type_target_expr`（L14658）：裸标识符不携带 arity，改用 `find_visible_type_any_arity`
  - `use` 声明别名冲突检查（L20305）：改用 `find_visible_type_any_arity`
  - 标识符解析（L20809）：改用 `find_visible_type_any_arity`
- [ ] 改造精确匹配调用点：
  - 约束 spec 查找（L14691）：传递 `cref->as.named.type_arg_count`
- [ ] 改造 enum 专用调用点（arity = 0）：
  - match enum target（L7930）
  - match label enum 引用（L8660）

### 6.5 模块导入导出

- [ ] 改造 `import_public_names`（L20015）：
  - 跨模块冲突检查改用 `find_visible_type_any_arity`（规则 1，name-only，行为不变）
  - `seen_type_names` 去重保持 name-only（规则 1，不变）
  - `FENG_DECL_SPEC` 分支（L20189）同理
- [ ] 改造 `src/symbol/export.c`，导出时携带 arity
- [ ] 改造 `src/symbol/ft_write.c` / `ft_read.c`，序列化支持 arity

### 6.6 代码生成

- [ ] 改造 `src/codegen/codegen.c`，mangling 包含 arity 信息
- [ ] 验证同名不同 arity 的类型生成符号不冲突

### 6.7 LSP

- [ ] 改造 `src/cli/lsp/runtime.c`，补全/跳转/hover 感知多 arity
- [ ] 按使用点 arity 精确匹配，不展示所有 arity 候选

### 6.8 测试

- [ ] `test/` 新增诊断测试：同名同 arity 冲突（AE0213）、同名不同 arity 允许、跨 category 冲突
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

**跨 category 冲突**（不同 category 同名即冲突，不看 arity）：

```feng
type Box<T> { value: T }
spec Box<T> { func get(): T }  // AE0213: type vs spec 跨 category 同名冲突

type Pair<T, U> { first: T, second: U }
type Pair<T, U>(T, U)  // AE0213: 普通 type vs 元组 跨 category 同名冲突（不看 arity）

type Pair<T> { first: T }
type Pair<T, U>(T, U)  // AE0213: 普通 type vs 元组 跨 category 同名冲突（即使 arity 不同也冲突）

spec Callback<T> { func invoke(): T }
spec Callback<T>(value: T): T  // AE0213: object spec vs callable spec 跨 category 同名冲突

type Box<T> { value: T }
enum Box { A, B }  // AE0213: type vs enum 跨 category 同名冲突
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

- 不改动函数重载机制（`FunctionOverloadSetEntry` / `compute_overload_match_priority`，多维重载，已实现且独立）
- 不支持泛型参数默认值（规范未定义）
- 不支持可变参数泛型（规范未定义）
- 不支持按泛型约束不同区分重载（规范明确禁止）
- 不支持按泛型参数名不同区分重载（规范明确禁止）

---

## 9 待办任务

- [ ] 同步更新 `docs/feng-generics-draft.md`：将"同 kind"措辞改为"同 category"
- [ ] 确认规范完整性，必要时补充 `docs/feng-generics-draft.md`
- [ ] 实现 `decl_type_param_count` 辅助函数
- [ ] 实现 `decl_overload_category` 辅助函数（§2.4，派生 7 类 category）
- [ ] 新增 `find_visible_type_any_arity` 辅助函数
- [ ] 新增 `has_cross_category_conflict` 辅助函数（规则 2，跨 category name-only 检查）
- [ ] 改造 `find_visible_type_index`（接受 `category` + `type_param_count`）/ `find_visible_type` / `find_visible_type_decl` / `find_named_type_decl`
- [ ] 验证 `copy_visible_type_entries` 无需改动
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` / `FENG_DECL_ENUM` 分支 + 跨 category 冲突检查
- [ ] 改造 `resolve_type_ref_decl` 及 `resolve_type_ref` 中的 arity 验证逻辑（含 AE1014/AE1015 重构 + 裸名分支错误改进）
- [ ] 改造存在性检查调用点（`find_unshadowed_alias` L4500 / `resolve_type_target_expr` L14658 / `use` 声明冲突 L20305 / 标识符解析 L20809）
- [ ] 改造精确匹配调用点（L14691 约束 spec 查找）
- [ ] 改造 enum 专用调用点（L7930 / L8660，arity = 0）
- [ ] 改造 `import_public_names`：跨模块冲突检查改用 `find_visible_type_any_arity`（规则 1，name-only，行为不变）
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
