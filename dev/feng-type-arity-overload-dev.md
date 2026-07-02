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

所有带泛型参数的 type / spec 子形式均按 `(name, arity)` 做 identity 判定。共 7 类 category（§0.3 表），由 `kind + is_tuple + form` 派生；其中 6 类支持 arity 重载，不支持泛型的声明（enum / global_binding 等）统一归 `NO_OVERLOADING`：

| category | 声明形式 | 语法示例 | 派生条件 | 支持 arity 重载 |
| -------- | -------- | -------- | -------- | --------------- |
| 1. FUNCTION | 函数声明 | `func foo<T>()` | `kind == FENG_DECL_FUNCTION` | ✅（多维：arity + 参数个数 + 参数类型；**已实现，本次不动**） |
| 2. TYPE | 普通 type + @value type | `type User<T> {}` / `@value type User<T> {}` | `kind == FENG_DECL_TYPE && is_tuple == false` | ✅（仅 arity） |
| 3. TUPLE | 元组类型 | `type User<T1, T2>(T1, T2)` | `kind == FENG_DECL_TYPE && is_tuple == true` | ✅（仅 arity） |
| 4. SPEC_OBJECT | 对象契约 | `spec User<T> {}` | `kind == FENG_DECL_SPEC && form == FENG_SPEC_FORM_OBJECT` | ✅（仅 arity） |
| 5. SPEC_CALLABLE | 函数契约 | `spec User<T>()` | `kind == FENG_DECL_SPEC && form == FENG_SPEC_FORM_CALLABLE` | ✅（仅 arity） |
| 6. SPEC_UNION | 联合契约 | `spec User<T1, T2>: T1 \| T2` | `kind == FENG_DECL_SPEC && form == FENG_SPEC_FORM_UNION` | ✅（仅 arity） |
| 7. NO_OVERLOADING | enum / global_binding 等 | `enum Color { ... }` | 其他（不支持泛型） | ❌（同名即冲突） |

- 表中"支持 arity 重载"列反映该 category 的**固有重载能力**，不代表本次改造范围。FUNCTION 的多维重载已实现（`FunctionOverloadSetEntry` / `compute_overload_match_priority`，`analyzer.c:10587`），本次**不动**；本次新增的 (name, arity) 单维重载仅适用于 5 类 type/spec category（TYPE / TUPLE / SPEC_OBJECT / SPEC_CALLABLE / SPEC_UNION）。

- **FUNCTION 与 type/spec 重载维度不同**：函数的重载是多维的——泛型 arity + 参数个数 + 参数类型，由现有 `FunctionOverloadSetEntry` / `compute_overload_match_priority`（`src/semantic/analyzer.c:10587`）处理，已实现。本次优化**不改动函数重载逻辑**，FUNCTION 列入 category 体系仅为：(a) 参与 §3.0 规则 2 的跨 category name-only 冲突检测（函数名与 type/spec/enum 同名即冲突）；(b) 表明 FUNCTION 同 category 内不适用本次新增的 (name, arity) 单维重载规则。规则 3 的 (name, arity) 仅适用于 5 类 type/spec category。

- **category 划分理由**：当前 `FengDeclKind`（`src/parser/parser.h:436`）粒度太粗——普通 type 和元组都归 `FENG_DECL_TYPE`，三种 spec form 都归 `FENG_DECL_SPEC`。但语义上它们是不同重载面，跨子形式同名即应冲突（详见 §3.0 规则 2）。仅普通 type 与 @value type 语义同构（`is_value` 不参与 category 区分），归为同类。enum / global_binding 等不支持泛型的声明统一归 `NO_OVERLOADING`——命名 `FengOverloadCategory` 本身体现"按重载能力分类"，不支持重载的归一类更自洽。

**子形式冲突规则（已决策）**：按 category 区分冲突面。

- **FUNCTION**：同 category 内的重载是**多维**的（泛型 arity + 参数个数 + 参数类型），由现有 `FunctionOverloadSetEntry` / `compute_overload_match_priority` 处理，**本次不改动**；跨 category name-only 冲突（函数名与 type/spec/enum 同名即冲突）。
- **5 类 type/spec category**（TYPE / TUPLE / SPEC_OBJECT / SPEC_CALLABLE / SPEC_UNION）：**同 category 内**按 `(name, arity)` 区分（同名同 arity 冲突、同名不同 arity 允许共存）；**跨 category** name-only 冲突（同名即冲突，不看 arity）。这是本次新增的重载规则。
- **NO_OVERLOADING**（enum / global_binding 等）：**无论同 category 还是跨 category，一律 name-only 判定冲突**（同名即冲突，不看 arity）。这类不支持泛型，无 arity 维度。

示例：

- `type Box<T> {}`（普通 type）与 `type Box<T, U>(T, U)`（元组）→ 跨 category → **冲突**（不看 arity）
- `spec Foo<T> {}`（object spec）与 `spec Foo<T>()`（callable spec）→ 跨 category → **冲突**（不看 arity）
- `type Box<T> {}` 与 `spec Box<T> {}` → 跨 category → **冲突**
- `type Box<T> {}` 与 `enum Box` → 跨 category → **冲突**
- `type Box<T> {}` 与 `func Box<T>()` → 跨 category → **冲突**（函数名与 type 同名）
- `type Box<T> {}` 与 `type Box<T, U> {}`（同为普通 type）→ 同 category，不同 arity → **允许共存**
- `type Box<T> {}` 与 `@value type Box<T> {}`（同类，`is_value` 不参与）→ 同 category，同 arity → **冲突**
- `func foo<T>()` 与 `func foo<T, U>(x: int)` → 同 category（FUNCTION），走现有函数重载机制（多维决议），**不在本次 (name, arity) 规则范围内**
- `enum Color { ... }` 与 `enum Color { ... }` → 同 category（NO_OVERLOADING），name-only → **冲突**

**当前实现缺陷**：

- `VisibleTypeEntry`（`src/semantic/analyzer.c:70`）每个名称只存单个 decl，无法容纳同名不同 arity 的多个声明
- `check_symbol_conflicts` 在处理 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` 时，`find_visible_type_index` 仅按名称查找，发现同名即报 **AE0213** 冲突
- 类型引用解析（`find_named_type_decl` → `find_visible_type_decl`）只用名称查找，不涉及 `type_param_count`

---

## 1 方案选择

保留 `VisibleTypeEntry` 单 decl 结构，但将查找 key 从 `name` 改为 `(name, type_param_count)` 复合 key，允许同名不同 arity 的多个 entry 并存。

**选择理由**：

- 改动最小：只需在查找/插入时同时比对 arity，不改数据结构
- 类型引用解析天然确定：使用点的 arity 来自 `type_ref->as.named.type_arg_count`，无需复杂决议
- 回归面可控：核心逻辑集中在 `find_visible_type_index` 和 `check_symbol_conflicts`
- 符合开闭原则：扩展查找维度，不破坏现有单 arity 场景

**设计取舍（C# vs Rust 之间选中间道路）**：考虑过两种极端——① C# 方向：类型标识为 (name, arity)，kind 不参与，跨类别允许 arity 重载（CLR 用 `Foo`1` / `Foo`2` 反引号记法区分）。灵活但易混淆：可能写出 `Foo<T>()` 是函数、`Foo<T1,T2>` 是类型/元组，使用者难从语法直接判断实体类别。② Rust 方向：纯 name-only（E0428 同名即冲突），清晰但失去灵活性，像 `Func<R>` / `Func<R,T1>` / `Func<R,T1,T2>` 这样的同形函数族定义不出来。Feng 选中间道路：同 category 内按 (name, arity) 重载，跨 category name-only 冲突——既保留同形实体族（不同 arity 的元组/spec）的扩展性，又避免跨形歧义。

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
| ------ | -------- | ------------ |
| L2383 | `find_visible_type_decl` 内部 | 从参数透传 |
| L14691 | 约束 spec 查找 | `cref->as.named.type_arg_count` |

**存在性检查场景**（只需判断是否存在任意 arity 的同名类型）：

| 行号 | 上下文 | 说明 |
| ------ | -------- | ------ |
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
    FENG_OVERLOAD_CATEGORY_NO_OVERLOADING     // enum / global_binding 等（不支持泛型，始终 name-only）
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
            return FENG_OVERLOAD_CATEGORY_NO_OVERLOADING;  // enum / global_binding
    }
}
```

`is_value` 不参与 category 派生——普通 type 与 @value type 同属 `FENG_OVERLOAD_CATEGORY_TYPE`（语义同构，详见 §0.3）。enum / global_binding 等不支持泛型的声明统一归 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`：无 arity 维度，**无论同 category 还是跨 category 都走 name-only 冲突**（同名即冲突）。命名 `FengOverloadCategory` 本身体现"按重载能力分类"，不支持重载的归一类更自洽。

---

## 3 冲突检查改造

### 3.0 冲突判定规则总览

**触发时机约束（本次不改动）**：冲突检测的触发时机遵循 [`dev/feng-module-optimize-dev.md`](./feng-module-optimize-dev.md) §0 六类规则的定义——涉及 import 的总是惰性（规则 1/2/3，使用点报 AE0005），纯本地的非 func 总是急切（规则 4，定义处报 AE0213-AE0216），func 同模块允许重载、仅重载冲突急切报错（规则 5，AE0217-AE0220）。**本次优化只改冲突"判定规则"（引入 category 维度 + arity 维度），不改"触发时机"**——急切的仍急切，惰性的仍惰性。

**术语澄清——本文规则编号 vs module-optimize-dev.md §0 规则编号**：两者同名不同义，不要混淆。

- module-optimize-dev.md §0 的"规则 1/2/3"指**涉及 import 的三类惰性冲突**（import vs import / import vs 本模块其他文件 / import vs 本文件），"规则 4"指同模块非 func 急切冲突，"规则 5"指 func 重载冲突急切，"规则 6"指别名急切。
- 本文下表的"规则 1/2/3"是**按 category 维度重新划分的判定规则**：规则 1 对应 module 规则 1/2/3 的合并（跨模块 name-only），规则 2/3 是 module 规则 4 按 category 维度的拆分（规则 2 = 跨 category name-only，规则 3 = 同 category 按 (name, arity)）。
- 两套规则的对应关系：本文规则 1 = module 规则 1/2/3（触发时机不变，都是惰性）；本文规则 2 + 规则 3 = module 规则 4 + 规则 5（触发时机不变，都是急切）。

本次判定规则改造涉及三条：

| 规则 | 场景 | 触发时机（不变） | 判定维度（本次改） | 改造内容 |
| ------ | ------ | ------------------ | --------------------- | ---------- |
| 规则 1 | 跨模块（涉及 import） | 惰性（使用点 AE0005） | name-only（不变） | 不同模块间 name-only 同名即冲突（与顶层函数一致）；`collect_symbol_candidates` 不按 arity 筛选，`candidates_form_ambiguity` 按 `provider_module` 判定歧义；同模块重载面内 arity 精确匹配由下游 `find_named_type_decl` 负责（§4） |
| 规则 2 | 同模块，不同 category | 急切（定义处） | name（不变） | 补充 function/binding 与 type/spec/enum 跨数组 name-only 检查（当前缺失，§3.3） |
| 规则 3 | 同模块，相同 category | 急切（定义处） | name → **(name, arity)**（仅 5 类 type/spec） | 5 类 type/spec 同名不同 arity 允许共存；FUNCTION 仍走多维重载（不变）；NO_OVERLOADING 仍 name（不变） |

- **规则 1 影响**（惰性，不改触发时机）：
  - `import_public_names` 中 `seen_type_names` 去重 key 从 `name` 改为 `(name, arity)`（L20052/L20084/L20193），支持同名不同 arity 同时导入
  - `import_public_names` 中 `find_visible_type_index` "已存在则 break" 判断从 name 改为 (name, arity)（L20060/L20092/L20201）
  - **跨模块歧义检测保持 name-only**（`collect_symbol_candidates` + `candidates_form_ambiguity`，L2480/L2547）：候选收集不按 arity 筛选，模块A `Box<T>` + 模块B `Box<T,U>` 在使用方同时 import 时，`candidates_form_ambiguity` 因 `provider_module` 不同直接报 AE0005（与"跨模块 name-only 同名即冲突"一致）
  - **同模块重载面内按 arity 精确匹配**：跨模块歧义检测通过后（`report_name_ambiguity_if_any` 返回 true），类型引用解析走 `find_named_type_decl(name, arity)` 按 (name, arity) 精确匹配同模块重载面内的目标声明（§4）。`collect_symbol_candidates` **不增加 arity 参数**——arity 精确匹配由下游 `find_named_type_decl` 负责，不在候选收集阶段做
  - `report_name_ambiguity_if_any`（L2716）**不透传 arity**：歧义检测是 name-only 的，与 arity 无关
- **跨模块规则（本次不改动触发时机，仅澄清语义）**：不同模块间一律 name-only 同名即冲突（与顶层函数重载一致——同模块才构成同一重载面，跨模块 name-only 判定）。模块A `type Box<T>` 与模块B `type Box<T, U>` 在使用方同时 import 时，**不论 arity 都报 AE0005**（lazy，使用点触发）。`collect_symbol_candidates` 不按 arity 筛选，`candidates_form_ambiguity` 按 `provider_module` 判定歧义
- **规则 2 影响**（急切，不改触发时机）：`check_symbol_conflicts` 中跨 category 冲突检查按 name 判定。当前实现中，type/spec/enum 三者共享 `visible_types` 数组（已通过 `find_visible_type_index` name 查找间接实现 name-only 冲突，L25080/L25113/L25323），function/binding 共享 `visible_values` 数组（同理已实现，L25144/L25191）；但 **function/binding 与 type/spec/enum 之间无跨数组检查**（既有缺陷，非泛型也存在，如 `type Box{}` 与 `func Box()` 同名当前不报错），本次新增 `has_value_name_only_conflict` / `has_type_name_only_conflict` 分别在注册前查**对岸数组**按 name + category 判定（§3.3）。**本次修复不依赖 scope 优化**——`dev/feng-scope-optimize-dev.md` 是 AST 多级作用域链统一（模块→文件→类型→函数→块）的优化，与顶层符号冲突检测是不同层面，且该优化尚未实施；本次在双表架构上独立修复跨数组漏检
- **规则 3 影响**（急切，不改触发时机）：`check_symbol_conflicts` 中同 category 冲突检查：5 类 type/spec category 改为按 `(name, arity)` 判定（**核心改动点**）；FUNCTION 仍走现有 `FunctionOverloadSetEntry` / `compute_overload_match_priority` 多维重载机制（**不改动**）；NO_OVERLOADING 仍按 name 判定（现有行为，不变）

- **规则 2 与规则 3 的关系**：规则 2 在 category 维度判定"跨面冲突"（一律 name-only）；规则 3 在 category 维度判定"同面重载"——5 类 type/spec 按 (name, arity)，FUNCTION 按现有函数多维重载，NO_OVERLOADING 按 name（即同面也是 name-only，等价于规则 2 的行为）。两者均以 category 为边界，取代原文档的 kind 维度判定。
- **FUNCTION 的特殊性**：函数重载维度多于 type/spec（泛型 arity + 参数个数 + 参数类型），已由现有机制实现，本次不改动。FUNCTION 列入 category 体系仅为参与规则 2 的跨 category name-only 冲突检测。
- **NO_OVERLOADING 的特殊性**：这类（enum / global_binding 等）不支持泛型，无 arity 维度，因此无论同面还是跨面都走 name-only。可理解为"规则 3 对它退化为 name-only"。

### 3.1 check_symbol_conflicts 分支改造

**FENG_DECL_TYPE 分支**（`src/semantic/analyzer.c:25078`）：

```c
case FENG_DECL_TYPE: {
    size_t arity = decl->as.type_decl.type_param_count;
    FengOverloadCategory category = decl_overload_category(decl);

    // 先检查跨数组 name-only 冲突（规则 2，查对岸 visible_values）
    if (has_value_name_only_conflict(visible_values, visible_value_count,
                                     decl->as.type_decl.name, category)) {
        char *message = format_message("type declaration '%.*s' conflicts with an existing visible name in a different category",
                                       (int)decl->as.type_decl.name.length,
                                       decl->as.type_decl.name.data);
        ok = append_error(errors, error_count, error_capacity,
                          program->path, *decl_token(decl), "AE0004", message);
        break;
    }

    // 再检查同数组跨 category 冲突（规则 2，查本岸 visible_types）
    if (has_cross_category_conflict(visible_types, visible_type_count,
                                    decl->as.type_decl.name, category)) {
        char *message = format_message("type declaration '%.*s' conflicts with an existing visible name in a different category",
                                       (int)decl->as.type_decl.name.length,
                                       decl->as.type_decl.name.data);
        ok = append_error(errors, error_count, error_capacity,
                          program->path, *decl_token(decl), "AE0004", message);
        break;
    }

    // 最后检查同 category 冲突（规则 3，按 name + arity）
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

**FENG_DECL_SPEC 分支**（`src/semantic/analyzer.c:25320`）：同理改造，category 由 `decl_overload_category` 按 `form` 派生（OBJECT / CALLABLE / UNION 分属不同 category）。注册前依次调用 `has_value_name_only_conflict`（对岸 visible_values）→ `has_cross_category_conflict`（本岸 visible_types 跨 category）→ `find_visible_type_index`（本岸同 category 按 (name, arity)）。

**FENG_DECL_ENUM 分支**（`src/semantic/analyzer.c:25110`）：enum 派生为 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`，arity 固定为 0。无 arity 维度——跨 category 冲突按 name 判定（与 type/spec 同名即冲突，报 AE0004）；同 category 内也按 name 判定（同名即冲突，报 AE0214）。**当前 enum 分支只检查 `visible_types` 内冲突，不检查 `visible_values`（与 function/binding 同名当前不报错，既有缺陷），需补充 `has_value_name_only_conflict` 查对岸 `visible_values`**（规则 2，急切，不改触发时机，报 AE0004）。

**FENG_DECL_FUNCTION 分支**（`src/semantic/analyzer.c:25191`）：当前只检查 `visible_values` 内的 binding/function 同名冲突（AE0215/7/8/9/20），**不检查 `visible_types`**（既有缺陷，`func Box()` 与 `type Box{}` 同名当前不报错）。改造后注册前先调用 `has_type_name_only_conflict` 查对岸 `visible_types` 按 name + category 判定（§3.3），有冲突报 AE0004 并 break；否则继续走现有 `visible_values` 内检查（AE0215/7/8/9/20）与 `FunctionOverloadSetEntry` / `compute_overload_match_priority` 多维重载机制（不改动）。

**FENG_DECL_GLOBAL_BINDING 分支**（`src/semantic/analyzer.c:25144`）：同理，当前只检查 `visible_values` 内冲突（AE0215/6），**不检查 `visible_types`**（既有缺陷）。改造后注册前先调用 `has_type_name_only_conflict` 查对岸 `visible_types`（§3.3），有冲突报 AE0004 并 break；否则继续走现有 `visible_values` 内检查（AE0215/6）。

**FENG_DECL_FIT**：fit 声明不注册到 `visible_types` / `visible_values`（仅注册 adapter 关系，`analyzer.c:25350` 空分支），不参与 name-only 冲突检测，无需改造。

### 3.2 跨 category 冲突检查（规则 2）

不同 category（§0.3 的 7 类之间）按 `name` 判定，同名即冲突，不看 arity。相比原 kind 维度，category 更细：同 `FENG_DECL_TYPE` 的普通 type 与元组、同 `FENG_DECL_SPEC` 的三种 form、enum 与其他 category 现在分属不同 category，同名即冲突。

**实现**：新增辅助函数 `has_cross_category_conflict`，在 type/spec/enum 分支注册前调用。判定遵循各 category 的重载能力语义——NO_OVERLOADING 不支持重载（同名即冲突），5 类 type/spec 同 category 内按 (name, arity) 区分，FUNCTION 同 category 内走多维重载；因此只要 new 或 existing 任一是 NO_OVERLOADING、或两者 category 不同，即冲突：

```c
static bool has_cross_category_conflict(const VisibleTypeEntry *entries,
                                        size_t count,
                                        FengSlice name,
                                        FengOverloadCategory new_category) {
    for (size_t i = 0U; i < count; ++i) {
        if (!slice_equals(entries[i].name, name)) {
            continue;
        }
        FengOverloadCategory existing_category = decl_overload_category(entries[i].decl);
        /* 重载能力语义驱动:
         * - NO_OVERLOADING: 不支持重载,同名即冲突
         * - 5 类 type/spec: 同 category 内按 (name, arity) 区分;跨 category 同名即冲突
         * - FUNCTION: 同 category 内多维重载;跨 category 同名即冲突
         * 任一是 NO_OVERLOADING、或两者 category 不同,即冲突。 */
        if (new_category == FENG_OVERLOAD_CATEGORY_NO_OVERLOADING ||
            existing_category == FENG_OVERLOAD_CATEGORY_NO_OVERLOADING ||
            existing_category != new_category) {
            return true;
        }
    }
    return false;
}
```

注册前调用检查，若返回 true 则报跨 category 冲突（错误码 AE0004，§3.3）。

> **与当前实现的差异**：

- 当前实现没有显式的跨 category 冲突函数——type/spec/enum 三者共享 `visible_types` 数组（`analyzer.c:25077/25110/25320` 各分支注册），function/binding 共享 `visible_values` 数组（`analyzer.c:25144/25191` 各分支注册），靠 `find_visible_type_index` / `find_visible_value_index` 的 name 查找间接实现"同数组内 name-only 冲突"。这导致两个问题：
- (a) 改造后同一 `visible_types` 数组需容纳同名不同 arity 的多 entry，单纯 name 查找会误报冲突，必须引入显式 category 判定；
- (b) function/binding 与 type/spec/enum 分属不同数组，当前**完全没有**跨数组 name 检查——`type Box<T>{}` 与 `func Box<T>()` 同名当前不报错，与 §0.3 示例矛盾（见 §3.3）。
- 新增 `has_cross_category_conflict` 显式按 `decl_overload_category(decl) != new_category` 判定，在 type/spec/enum 分支注册前调用（同数组内跨 category 检查）；新增 `has_value_name_only_conflict` / `has_type_name_only_conflict` 分别查对岸 `visible_values` / `visible_types` 按 name + category 判定，在所有分支注册前调用（跨数组 name-only 检查，§3.3）。

### 3.3 跨数组 name-only 冲突检查（规则 2 补充）

当前实现中，function/binding 注册到 `visible_values`（`analyzer.c:25144/25191`），type/spec/enum 注册到 `visible_types`（`analyzer.c:25077/25110/25320`），两个数组不交叉。因此 `type Box{}` 与 `func Box()` 同名当前**不报错**——这是既有缺陷（非泛型也存在），与 §0.3 示例矛盾。

**实现**：新增两个辅助函数，**各自只查对岸数组**（避免与本岸检查职责重叠/双重报错）：

```c
/* type/spec/enum 注册前调用：查对岸 visible_values 按 name + category 判定 */
static bool has_value_name_only_conflict(
    const VisibleValueEntry *values, size_t value_count,
    FengSlice name, FengOverloadCategory new_category) {
    for (size_t i = 0U; i < value_count; ++i) {
        if (!slice_equals(values[i].name, name)) {
            continue;
        }
        FengOverloadCategory existing_category = decl_overload_category(values[i].decl);
        if (new_category == FENG_OVERLOAD_CATEGORY_NO_OVERLOADING ||
            existing_category == FENG_OVERLOAD_CATEGORY_NO_OVERLOADING ||
            existing_category != new_category) {
            return true;
        }
    }
    return false;
}

/* function/binding 注册前调用：查对岸 visible_types 按 name + category 判定 */
static bool has_type_name_only_conflict(
    const VisibleTypeEntry *types, size_t type_count,
    FengSlice name, FengOverloadCategory new_category) {
    for (size_t i = 0U; i < type_count; ++i) {
        if (!slice_equals(types[i].name, name)) {
            continue;
        }
        FengOverloadCategory existing_category = decl_overload_category(types[i].decl);
        if (new_category == FENG_OVERLOAD_CATEGORY_NO_OVERLOADING ||
            existing_category == FENG_OVERLOAD_CATEGORY_NO_OVERLOADING ||
            existing_category != new_category) {
            return true;
        }
    }
    return false;
}
```

> 注：NO_OVERLOADING vs NO_OVERLOADING（不同 kind，如 enum 与 global_binding 同名）由 `new_category == NO_OVERLOADING` 分支捕获——NO_OVERLOADING 不支持重载，固有语义即同名即冲突，并非特判。

**调用点**：

- type/spec/enum 分支注册前：调用 `has_value_name_only_conflict` 查对岸 `visible_values`
- function/binding 分支注册前：调用 `has_type_name_only_conflict` 查对岸 `visible_types`

**为何拆为两个函数而非单个函数遍历两个数组**：单个函数同时遍历两个数组会与本岸检查职责重叠——type/spec/enum 分支本岸由 `has_cross_category_conflict`（§3.2）检查 visible_types 内跨 category，function/binding 分支本岸由现有 AE0215-AE0220 检查 visible_values 内冲突；若单函数同时遍历两个数组，会重复检查本岸，导致双重报错（如 type vs type 同 category 同名会同时触发 AE0004 + AE0213）。拆为两个函数各自只查对岸数组，职责清晰，与本岸检查不重叠——检查到对岸冲突就报 AE0004 并 break，不再走本岸检查。

**错误码决策（已定）**：跨数组 name-only 冲突（即跨 category 冲突）统一使用通用段 **AE0004**（"跨结构或跨类型冲突"）。理由：

- 00 通用段定义即"跨结构的基础语义约束"（`docs/feng-error-codes-ae.md` §00），与"function/binding 与 type/spec/enum 跨数组冲突"语义对齐
- AE0002 旧语义为 @runtime 相关（已迁移至 AE1301），码位虽释放但语义易混，避开
- AE0004/AE0006 旧语义为 @runtime/@abi 相关（已迁至 AE1320/AE1303），码位释放且**代码中完全未被引用**（`grep -rn "AE0004\|AE0006" src/` 无结果），本次复用为通用段新错误码
- 同 category 内重复仍用 AE0213（type/spec）/ AE0214（enum）/ AE0215-AE0220（function/binding），按 decl kind 选码；跨 category 冲突用 AE0004，message 区分"跨 category 冲突"
- 实施时需同步在 `docs/feng-error-codes-ae.md` §00 通用段新增 AE0004 条目

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

**调用点**（3 处，不含 `find_visible_type` 内部调用）：

| 行号 | 上下文 | arity 说明 |
| ------ | -------- | ------------ |
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
| ------ | -------- | ------------ | ------ |
| L4756 | `resolve_type_ref_decl` 内部 | `type_ref->as.named.type_arg_count` | 主要类型引用解析入口 |
| L20403 | `resolve_named_type_ref` 中 `type_arg_count > 0` 分支 | 局部变量 `type_arg_count` | 带类型参数的引用 |
| L20492 | `resolve_named_type_ref` 中 `segment_count == 1` 无类型参数分支 | 0 | 裸名引用，无 `<...>` |
| L20506 | `resolve_named_type_ref` 中多段路径分支 | 0 | 模块限定路径，无 `<...>` |

> 注：上述 L20403/L20492/L20506 位于 `resolve_named_type_ref`（`analyzer.c:20361` 定义）内，不是 `resolve_type_ref`（`analyzer.c:20523` 定义，仅做 switch 分发到 `resolve_named_type_ref` 等子函数）。实施时按 `resolve_named_type_ref` 定位分支。

#### 4.4.2 resolve_type_ref_decl 调用点（多处）

`resolve_type_ref_decl`（`src/semantic/analyzer.c:4746`）改造后内部通过 `type_ref->as.named.type_arg_count` 确定 arity 并传递给 `find_named_type_decl`。其调用者不需要修改，arity 由 type_ref 自身确定。

### 4.5 错误处理与 arity 验证重构

**当前流程**（`resolve_named_type_ref` L20391–20457，`type_arg_count > 0` 分支）：

1. find_named_type_decl(name)          → 按名称找到 decl
2. 计算 expected_arity                  → 从 decl 提取
3. 验证 expected_arity != 0             → 否则报 AE1014 "not a generic type"
4. 验证 type_arg_count == expected_arity → 否则报 AE1015 "expects N but got M"
5. materialize constraint witnesses

**改造后流程**：`find_named_type_decl` 按 `(name, type_arg_count)` 精确匹配，步骤 2–4 的验证变为冗余。需重构为：

1. find_named_type_decl(name, type_arg_count) → 按 (name, arity) 精确查找
2. 若返回 NULL：
   a. 先按名称查找（不传 arity）判断是否存在同名类型
   b. 存在同名但 arity 不匹配 → 报 AE1015 "expects N type argument(s), but M were provided"
   c. 不存在 → 报 AE1013 "unknown type"
3. 若返回非 NULL → materialize constraint witnesses

**错误码决策**：保留 AE1013/AE1014/AE1015，仅重构触发逻辑，错误码语义不变：

- 完全不存在同名类型 → AE1013 "unknown type"
- 同名类型存在但 arity=0（非泛型），使用点带类型参数 → AE1014 "not a generic type"
- 同名泛型类型存在但 arity 不匹配 → AE1015 "expects N type argument(s), but M were provided"

**实现要点**：步骤 2a 可复用 `find_visible_type_any_arity`（§2.3），按返回结果区分上述三种场景。

### 4.6 裸名分支错误信息改进

**当前流程**（`resolve_named_type_ref` L20460–20508，裸名无类型参数分支）：

1. find_named_type_decl(name) → 按名称找到 decl（当前实现）
2. 若返回非 NULL → return true
3. 若返回 NULL → 继续后续查找（alias 等），最终报 "unknown type"

**改造后流程**：`find_named_type_decl` 改为按 `(name, 0)` 精确匹配。当返回 NULL 时，同样需要区分：

1. find_named_type_decl(name, 0) → 按 (name, arity=0) 精确查找
2. 若返回非 NULL → return true
3. 若返回 NULL：
   a. 用 find_visible_type_any_arity 判断是否存在同名类型
   b. 存在同名但 arity ≠ 0 → 报 AE0006 "'%.*s' is a generic type and requires type arguments"
   c. 不存在 → 继续后续查找（alias 等）

**错误码决策（已定）**：使用点裸名引用泛型（目标 arity≠0 但使用点不带 `<...>`）报 **AE0006**（00 通用段）。理由：该错误对 type 和 spec 都适用（不局限于单一类别），本质是"类型引用不完整"的通用问题，与 AE0001 "基础符号与类型存在性" 同属"不跨结构但通用"的先例；按类别分 03+06 两个码位会造成碎片化，统一用 AE0006 更合理。与 §4.5 的 AE1013/AE1015（10 表达式段，"带类型参数的泛型目标"）形成对比——§4.5 使用点是带 `<...>` 的泛型目标（表达式段），§4.6 使用点是裸名普通类型引用（通用段，因不局限于单一类别）。

**实现要点**：与 §4.5 共享 `find_visible_type_any_arity` 辅助函数和错误信息模式。

---

## 5 影响面分析

### 5.1 核心影响

| 模块 | 文件 | 改动点 |
| ------ | ------ | -------- |
| 查找函数 | `src/semantic/analyzer.c` | `find_visible_type_index` / `find_visible_type` / `find_visible_type_decl` / `find_named_type_decl` + 新增 `find_visible_type_any_arity` / `decl_type_param_count` / `decl_overload_category` |
| 冲突检查 | `src/semantic/analyzer.c` | `check_symbol_conflicts` 中 `FENG_DECL_TYPE` / `FENG_DECL_SPEC` / `FENG_DECL_ENUM` / `FENG_DECL_FUNCTION` / `FENG_DECL_GLOBAL_BINDING` 五分支 + 新增 `has_cross_category_conflict` / `has_value_name_only_conflict` / `has_type_name_only_conflict` |
| 类型解析 | `src/semantic/analyzer.c` | `resolve_type_ref`（L20391–20457 arity 验证重构）/ `resolve_type_ref_decl` 及所有调用点 |
| 存在性检查 | `src/semantic/analyzer.c` | `find_unshadowed_alias`（L4500）/ `resolve_type_target_expr`（L14658）/ `use` 声明冲突检查（L20305）/ 标识符解析（L20809）改用 `find_visible_type_any_arity` |
| 模块导入 | `src/semantic/analyzer.c` | `import_public_names` 中 `seen_type_names` 改为 (name, arity) 去重（规则 1，惰性不改触发时机）；`collect_symbol_candidates`（L2480）+ `report_name_ambiguity_if_any`（L2716）**保持 name-only**，不感知 arity（跨模块歧义按 name 判定，同模块 arity 精确匹配由下游 `find_named_type_decl` 负责） |
| 跨模块查找 | `src/semantic/analyzer.c` | `find_module_public_type_decl`（L2948）改为按 (name, arity) 查找 + 新增 `find_module_public_type_decl_any_arity`（存在性检查）；7 处调用点改造（§6.4） |
| 符号提供 | `src/symbol/provider.c` | 导入时按 `(name, arity)` 注册 |

### 5.2 关联影响

| 模块 | 文件 | 说明 |
| ------ | ------ | ------ |
| 可见类型复制 | `src/semantic/analyzer.c` | `copy_visible_type_entries`（L2835）是纯 memcpy，结构不变则无需改动 |
| ~~代码生成~~ | ~~`src/codegen/codegen.c`~~ | **无需改动**：现有泛型实例 mangling 已包含完整类型参数信息（`__G__<arg1>__<arg2>`），同名不同 arity 的类型生成符号天然不冲突（详见 §5.3） |
| ~~符号表序列化~~ | ~~`src/symbol/ft_write.c` / `ft_read.c`~~ | **无需改动**：`.ft` 文件按 decl 独立写入（decl-by-decl），同名不同 arity 的 type/spec 是两个独立 decl，不会冲突；泛型函数重载已能正确写入 `.ft`，同理适用于 type/spec（详见 §5.3） |
| LSP | `src/cli/lsp/runtime.c` | 补全、跳转、hover 需感知多 arity，按使用点 arity 精确匹配（详见 §6.7） |
| 约束见证 | `src/semantic/analyzer.c` | `materialize_named_type_param_constraint_witnesses` 通过 `resolve_type_ref_decl` 间接获得正确 arity 的 decl，**自然受益，无需主动改造** |
| 测试 | `test/` / `fcts/` | 新增诊断测试和行为兼容测试 |

### 5.3 不受影响

- **函数重载**：已实现（`FunctionOverloadSetEntry` / `compute_overload_match_priority`，多维：泛型 arity + 参数个数 + 参数类型），本次**不改动**。FUNCTION 列入 category 体系仅为参与跨 category name-only 冲突检测，同 category 内仍走现有函数重载机制
- 方法泛型重载：已实现，逻辑独立
- 非泛型类型：arity = 0，走现有逻辑
- 运行时：泛型在编译期完成实例化，运行时不涉及 arity 判定
- **代码生成（codegen）无需改动**：现有泛型实例的 mangling 已包含完整类型参数信息。例如 `Box<int>` 生成符号 `Feng__module__Box__G__int`，`Box<int, string>` 生成 `Feng__module__Box__G__int__string`。同名不同 arity 的类型是不同的 `FengDecl`，各自的实例通过 `generic_origin_decl` 指向不同 origin，符号天然不冲突（`generic_origin_decl` 定义于 `src/codegen/codegen.c` L600/L764，使用点 L4186/L5722/L5775/L5797）
- **符号表序列化（ft_write/ft_read）无需改动**：`.ft` 文件按 decl 独立写入（decl-by-decl），同名不同 arity 的 type/spec 是两个独立 decl，不会冲突。泛型函数的重载已能正确写入 `.ft`（`writer_collect_decl` 定义于 `src/symbol/ft_write.c` L1011，调用点 L1168），同理适用于 type/spec 的 arity 重载
- **符号导出（export.c）无需改动**：`export.c` 处理的是泛型实例的符号导出，而泛型实例的符号名已包含完整类型参数信息，同名不同 arity 的类型导出时天然区分

---

## 6 实施步骤

按 CLAUDE.md "先规范，再实现，后测试"原则，重新组织为以下子步骤，每个子步骤可独立编译通过、回归测试通过、独立交付：

**拆分原则**：

- 签名改造必须与所有调用点改造同步交付（否则编译失败）
- 辅助函数实现与存在性检查调用点改造可独立交付（不改签名，行为不变）
- `find_named_type_decl` 签名改造与 arity 验证重构必须同步交付（否则 arity 不匹配场景行为变更）
- 步骤顺序：6.1 → 6.2 → 6.3 → 6.4 → 6.5 → 6.6（无需改动）→ 6.7 → 6.8

### 6.1 规范确认

- [ ] 同步更新 `docs/feng-generics-draft.md`：将"同 kind"措辞改为"同 category"（§0.3 的 7 类），定义 7 类 category 划分及派生规则
- [ ] 确认 `docs/feng-generics-draft.md` 中相关规范是否完整
- [ ] 在 `docs/feng-error-codes-ae.md` §00 通用段新增 AE0004 条目（"跨 category name-only 冲突"），message 形如 "'%.*s' conflicts with an existing visible name in a different category"
- [ ] 在 `docs/feng-error-codes-ae.md` §00 通用段新增 AE0006 条目（"使用点裸名引用泛型"），message 形如 "'%.*s' is a generic type and requires type arguments"

**备注（已决策/已查证/已澄清，无需进一步改动）**：

- **错误码决策**：跨 category name-only 冲突用 AE0004（§3.3）；使用点裸名引用泛型用 AE0006（§4.6，对 type/spec 都适用，按"不跨结构但通用"先例归通用段）；同 category 重复仍用 AE0213（type/spec）/ AE0214（enum）/ AE0215-AE0220（function/binding）；arity 不匹配保留 AE1013/AE1014/AE1015，仅重构触发逻辑（§4.5）
- **AE0004/AE0006 码位可用性已查证**：代码中完全未被引用（`grep -rn "AE0004\|AE0006" src/` 无结果），错误码文档中作为"原错误码"出现（旧语义已迁至 AE1320/AE1303），码位实际处于释放可用状态
- **跨 kind 冲突规则已决策**：规则 2，同模块不同 category 按 name 判定冲突（补充 function/binding 与 type/spec/enum 跨数组检查，§3.3）
- **触发时机约束已决策**：本次只改判定规则，不改触发时机（急切的仍急切，惰性的仍惰性，遵循 `dev/feng-module-optimize-dev.md` §0 六类规则）
- **与 scope 优化的关系已澄清**：`dev/feng-scope-optimize-dev.md` 是 AST 多级作用域链统一（模块→文件→类型→函数→块）的优化，与顶层符号冲突检测是不同层面，且该优化尚未实施；本次在双表架构上独立修复跨数组漏检，不依赖 scope 优化，不与 scope 优化冲突
- **独立性**：仅文档变更，可独立交付。无代码变更，回归测试行为不变。

### 6.2 辅助函数实现 + 存在性检查调用点改造

本步骤仅新增辅助函数 + 改造存在性检查调用点，**不改造任何现有函数签名**，不改造任何精确匹配调用点。

- [ ] 实现 `decl_type_param_count`（§2.4，从 FengDecl 提取 type_param_count）
- [ ] 实现 `decl_overload_category`（§2.4，派生 7 类 category）
- [ ] 新增 `find_visible_type_any_arity`（§2.3，存在性检查辅助函数，**不替换** `find_visible_type`）
- [ ] 新增 `has_cross_category_conflict`（§3.2，本岸 visible_types 内跨 category name-only 检查）
- [ ] 新增 `has_value_name_only_conflict`（§3.3，type/spec/enum 注册前查对岸 visible_values）
- [ ] 新增 `has_type_name_only_conflict`（§3.3，function/binding 注册前查对岸 visible_types）
- [ ] 改造存在性检查调用点（改用 `find_visible_type_any_arity`，行为与原 `find_visible_type` name-only 查找一致）：
  - L4500 `find_unshadowed_alias` — alias 是否被类型遮蔽
  - L14658 `resolve_type_target_expr` — 裸标识符解析类型目标
  - L20305 `use` 声明别名冲突检查
  - L20809 标识符解析 — 判断标识符是否为类型名

**独立性**：仅新增辅助函数 + 改造存在性检查调用点（不改任何现有函数签名）。新增辅助函数暂未被调用（除存在性检查场景改用 `find_visible_type_any_arity`，行为与原 name-only 查找一致）。编译通过，回归测试行为不变，可独立交付。

**为何将存在性检查调用点改造纳入本步骤**：存在性检查场景改用 `find_visible_type_any_arity` 是为了后续 6.4 改造 `find_visible_type` 签名时这些场景不受影响。本步骤完成后，6.4 改造 `find_visible_type` 签名时只需关注精确匹配调用点，存在性检查调用点已用 `find_visible_type_any_arity` 不受影响。

### 6.3 `find_visible_type_index` 签名改造 + 所有调用点

本步骤改造 `find_visible_type_index` 签名 + 所有调用点（check_symbol_conflicts 五分支 + import_public_names）。

- [ ] 改造 `find_visible_type_index` 接受 `category` + `type_param_count` 参数（§2.2，同面精确匹配）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_TYPE` 分支（§3.1）：注册前查对岸 visible_values（`has_value_name_only_conflict`）+ 本岸跨 category（`has_cross_category_conflict`）+ 同 category 按 (name, arity)（`find_visible_type_index` 新签名）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_SPEC` 分支（§3.1，同理，category 由 `decl_overload_category` 按 `form` 派生）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_ENUM` 分支（§3.1，派生 `FENG_OVERLOAD_CATEGORY_NO_OVERLOADING`，注册前查对岸 visible_values + 本岸 name-only 不变）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_FUNCTION` 分支（§3.1，注册前查对岸 visible_types，本岸走现有重载机制不变）
- [ ] 改造 `check_symbol_conflicts` 中 `FENG_DECL_GLOBAL_BINDING` 分支（§3.1，注册前查对岸 visible_types，本岸 name-only 不变）
- [ ] 改造 `import_public_names` 中 `find_visible_type_index` 调用（L20060/L20092/L20201）：传入 (name, category, arity)
- [ ] 改造 `import_public_names` 中 `seen_type_names` 去重 key（L20052/L20084/L20193）：从 name 改为 (name, arity)
- [ ] 改造 `import_public_names` 中 `FENG_DECL_SPEC` 分支（L20189，同理）

**独立性**：

- 改造 `find_visible_type_index` 签名 + 所有调用点（check_symbol_conflicts 五分支 + import_public_names）。完成后，编译通过，回归测试通过（非泛型场景行为不变）。新增 arity 重载能力（type/spec 同名不同 arity 允许共存、可同时导入），但使用点解析尚未改造（6.4 完成），同名不同 arity 类型的引用暂不可靠（取决于注册顺序）。不破坏现有行为，可独立交付。

**不改动触发时机**：

- import 阶段仍不做冲突检查（惰性，§3.0 规则 1），仅做去重 + 候选收集；check_symbol_conflicts 中急切/惰性的触发时机遵循 `dev/feng-module-optimize-dev.md` §0 六类规则，本次只改判定规则不改触发时机。

**备注（无需改动）**：

- `collect_symbol_candidates`（L2480）：跨模块歧义检测已按 name-only 实现，不感知 arity，arity 精确匹配由下游 `find_named_type_decl` 负责（§4）
- `report_name_ambiguity_if_any`（L2716）及各调用点（L7005/L15875/L19657/L20485/L20800）：歧义检测已按 name-only 实现，不透传 arity

### 6.4 `find_visible_type` / `find_visible_type_decl` / `find_named_type_decl` 签名改造 + 精确匹配调用点 + arity 验证重构

本步骤改造 3 个 static 函数签名 + 所有精确匹配调用点 + arity 验证重构 + 错误处理改进。**必须同步交付**（find_named_type_decl 签名改造后，arity 验证逻辑必须同步重构，否则 arity 不匹配场景行为变更）。

- [ ] 改造 `find_visible_type` 接受 `type_param_count` 参数（§2.3，精确匹配场景）
- [ ] 改造 `find_visible_type_decl` 接受 `type_param_count` 参数（§4.2）
- [ ] 改造 `find_named_type_decl` 接受并传递 `type_param_count` 参数（§4.3）
- [ ] 改造 `resolve_type_ref_decl`（L4746）：从 `type_ref->as.named.type_arg_count` 提取 arity 传递给 `find_named_type_decl`
- [ ] 改造精确匹配调用点：
  - L2383 `find_visible_type_decl` 内部：从参数透传
  - L14691 约束 spec 查找：传递 `cref->as.named.type_arg_count`
  - L4522 `find_named_type_decl` 内部：从参数透传
  - L7930 match enum target：arity = 0（enum 无泛型）
  - L8660 match label enum 引用：arity = 0（enum 无泛型）
  - L4756 `resolve_type_ref_decl` 内部：`type_ref->as.named.type_arg_count`
  - L20403 `resolve_named_type_ref` 中 `type_arg_count > 0` 分支：局部变量 `type_arg_count`
  - L20492 `resolve_named_type_ref` 中 `segment_count == 1` 无类型参数分支：arity = 0
  - L20506 `resolve_named_type_ref` 中多段路径分支：arity = 0
- [ ] 重构 `resolve_named_type_ref` 有类型参数分支（L20403-L20457，§4.5）：
  - `find_named_type_decl(name, type_arg_count)` 按 (name, arity) 精确查找
  - 返回 NULL 时用 `find_visible_type_any_arity` 区分：同名但 arity=0（非泛型）→ AE1014 "not a generic type"；同名但 arity≠0 且不匹配 → AE1015 "expects N type argument(s), but M were provided"；完全不存在 → AE1013 "unknown type"
  - 移除冗余的 expected_arity 计算与验证（原 L20415-L20437 的 AE1014/AE1015 验证）
- [ ] 重构 `resolve_named_type_ref` 裸名无类型参数分支（L20460-L20508，§4.6）：
  - `find_named_type_decl(name, 0)` 按 (name, arity=0) 精确查找
  - 返回 NULL 时用 `find_visible_type_any_arity` 区分：同名但 arity≠0 → AE0006 "'%.*s' is a generic type and requires type arguments"；完全不存在 → 继续后续查找（alias 等）

> **独立性**：改造 3 个 static 函数签名 + 所有精确匹配调用点 + arity 验证重构。完成后，编译通过，回归测试通过（行为改进：同名不同 arity 类型可精确解析，arity 不匹配错误码触发逻辑更精确，新增 AE0006）。可独立交付。
> **为何签名改造与 arity 验证重构必须同步交付**：`find_named_type_decl` 改签名后按 (name, arity) 精确查找，找不到返回 NULL。原 L20415-L20437 的 expected_arity 验证逻辑（从 decl 提取 arity）会因 decl 为 NULL 而 crash 或报错不正确。必须同步重构为"查找失败后用 `find_visible_type_any_arity` 区分场景"的新逻辑。
> **存在性检查调用点已在 6.2 改造完成**：L4500/L14658/L20305/L20809 已改用 `find_visible_type_any_arity`，本步骤只需关注精确匹配调用点。
> **备注（无需改动）**：`copy_visible_type_entries`（L2835）是纯 memcpy 操作，结构不变则无需改动。

### 6.5 `find_module_public_type_decl` 签名改造 + `_any_arity` 版本 + 7 处调用点

本步骤改造跨模块类型查找。**依赖 6.4 完成**（L4529/L4538 透传 `type_param_count` 依赖 `find_named_type_decl` 已改签名）。

- [ ] 改造 `find_module_public_type_decl`（L2948）：接受 `type_param_count` 参数，按 (name, arity) 查找
- [ ] 新增 `find_module_public_type_decl_any_arity`：存在性检查
- [ ] 改造 7 处调用点：

  | 行号 | 上下文 | arity 来源 | 改造方式 |
  | ------ | -------- | ---------- | --------- |
  | L3393 | `module_exports_public_type` 存在性检查 | 不需要 | 改用 `find_module_public_type_decl_any_arity` |
  | L4529 | `find_named_type_decl` segment_count==2 alias 路径 | `type_param_count` 透传 | 透传（依赖 6.4） |
  | L4538 | `find_named_type_decl` 多段路径 | `type_param_count` 透传 | 透传（依赖 6.4） |
  | L14716 | `resolve_type_target_expr` alias 路径 | 裸标识符，不带 arity | 改用 `_any_arity` 版本 |
  | L14733 | `resolve_type_target_expr` 多段路径 | 裸标识符，不带 arity | 改用 `_any_arity` 版本 |
  | L25807 | `analysis_resolve_named_type_ref` 多段路径分支 | `ref->as.named.type_arg_count` | 透传 |
  | L25814 | `analysis_resolve_named_type_ref` 单段查找（遍历所有模块） | `ref->as.named.type_arg_count` | 透传 |

> **独立性**：改造跨模块类型查找。完成后，编译通过，回归测试通过（跨模块同名不同 arity 类型可精确解析，存在性检查场景行为不变）。可独立交付。
> **依赖 6.4**：L4529/L4538 是 `find_named_type_decl` 内部的 alias/多段路径分支，透传 `type_param_count` 依赖 `find_named_type_decl` 已改签名（6.4 完成）。其他调用点不依赖 6.4，但为保持步骤内聚，统一在 6.5 完成。

**备注（无需改动）**：

- `src/symbol/export.c`：泛型实例 mangling 已含完整类型参数（详见 §5.3）
- `src/symbol/ft_write.c` / `ft_read.c`：`.ft` 按 decl 独立写入，同名不同 arity 的 type/spec 是两个独立 decl，不会冲突（详见 §5.3）

### 6.6 代码生成

> **备注（无需改动）**：`src/codegen/codegen.c` 现有泛型实例 mangling 已包含完整类型参数信息（如 `Feng__module__Box__G__int` vs `Feng__module__Box__G__int__string`），同名不同 arity 的类型是不同的 `FengDecl`，各自实例通过 `generic_origin_decl` 指向不同 origin，符号生成路径完全分离，天然不冲突（详见 §5.3）

### 6.7 LSP

LSP 不直接调用 `find_visible_type` 等 static 函数，而是通过两类入口访问符号信息：

1. `feng_semantic_*` 公共 API（`feng_semantic_lookup_type_fact` 等，`semantic.h`）
2. `feng_symbol_*` 符号表 API（`feng_symbol_module_find_public_type` / `feng_symbol_module_public_decl_at` / `feng_symbol_decl_type_param_count` 等，`src/symbol/provider.h`）

当前 `feng_symbol_module_find_public_type(module, name)` 按 name 查找，只返回首个匹配 decl；同名不同 arity 共存时只能拿到第一个。需改造的 LSP 行为：

- **补全**：用户输入 `Box` 时，应展示所有 arity 的 Box 候选（多个 completion item），而非只展示第一个 arity。改造方式：用 `feng_symbol_module_public_decl_count` + `feng_symbol_module_public_decl_at` 遍历模块 public decl，按 `feng_symbol_decl_name` 筛选同名、按 `feng_symbol_decl_type_param_count` 区分 arity，逐个发出 completion item（label 形如 `Box<T>` / `Box<T, U>`）。
- **跳转/hover**：用户在 `Box<int>` 上跳转/hover 时，应跳到 arity=1 的 Box。改造方式：从使用点的 type_ref 提取 `type_arg_count`（arity），遍历同名候选按 `feng_symbol_decl_type_param_count == arity` 精确匹配；找不到时回退到 name-only 查找（保持当前行为，避免回归）。

待办：

- [ ] 改造 `src/cli/lsp/runtime.c` 补全路径：替换 `find_symbol_module_decl_by_name`（L3548）等按 name 查找单个 decl 的调用，改为遍历 + 按 (name, arity) 区分多 arity 候选
- [ ] 改造 `src/cli/lsp/runtime.c` 跳转/hover 路径：从使用点 type_ref 提取 `type_arg_count`，按 (name, arity) 精确匹配 decl
- [ ] 验证 `feng_symbol_module_find_public_type` 等 name-only API 是否需要新增按 arity 查找的对应版本；若 LSP 内部遍历已够用，则公共 API 可不动（待实施时确认）
- [ ] 全量回归 LSP 现有补全/跳转/hover 测试，确保非泛型场景行为不变

> **独立性**：LSP 通过公共 API 访问符号信息，不直接调用 static 函数。依赖前面步骤完成（符号表已支持同名不同 arity 共存）。可独立交付，但需先确认公共 API 是否需要扩展。

### 6.8 测试

- [ ] `test/` 新增诊断测试：同名同 arity 冲突（AE0213）、同名不同 arity 允许、跨 category 冲突（AE0004）
- [ ] `test/` 新增解析测试：使用点 arity 精确匹配、arity 不匹配报错（AE1013/AE1014/AE1015）、裸名引用泛型（AE0006）
- [ ] `fcts/` 新增行为测试：同名不同 arity 的类型独立使用、泛型实例化正确
- [ ] 全量回归测试（`make test`），确保非泛型场景行为不变

> **独立性**：必须在所有代码改造完成后进行。不独立交付，作为整体交付的验证步骤。

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

**跨 category 冲突**（不同 category 同名即冲突，不看 arity，统一报 AE0004）：

```feng
type Box<T> { value: T }
spec Box<T> { func get(): T }  // AE0004: type vs spec 跨 category 同名冲突

type Pair<T, U> { first: T, second: U }
type Pair<T, U>(T, U)  // AE0004: 普通 type vs 元组 跨 category 同名冲突（不看 arity）

type Pair<T> { first: T }
type Pair<T, U>(T, U)  // AE0004: 普通 type vs 元组 跨 category 同名冲突（即使 arity 不同也冲突）

spec Callback<T> { func invoke(): T }
spec Callback<T>(value: T): T  // AE0004: object spec vs callable spec 跨 category 同名冲突

type Box<T> { value: T }
enum Box { A, B }  // AE0004: type vs enum 跨 category 同名冲突

type Box<T> { value: T }
func Box<T>() {}  // AE0004: type vs function 跨 category 同名冲突

enum Color { A, B }
let Color = 1  // AE0004: enum vs global_binding 同 category（NO_OVERLOADING）name-only 冲突（不同 kind 但同 category，NO_OVERLOADING 固有语义即同名冲突）
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

## 9 关联文档

- [docs/feng-generics-draft.md](../docs/feng-generics-draft.md)：泛型规范与语法定义
- [docs/feng-type.md](../docs/feng-type.md)：类型系统规范
- [docs/feng-spec.md](../docs/feng-spec.md)：spec 规范
- [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md)：错误码定义
- [CLAUDE.md](../CLAUDE.md)：开发原则与流程
