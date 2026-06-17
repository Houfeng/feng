# Feng 层级作用域链统一优化 TODO

目标：将现有三套独立的符号查找机制统一为一条层级作用域链（模块 → 文件 → 类型 → 函数 → 块），使符号查找、冲突检测、重载决议共用同一套机制，消除代码重复和跨命名空间漏检。

## 0. 现状分析（代码事实）

### 0.1 三套独立查找机制

| 层级 | 查找函数 | 数据结构 | 调用次数 |
|---|---|---|---|
| 块/函数（局部变量） | `resolver_find_local_name_entry`（L3136） | `ScopeFrame` + `LocalNameEntry`（L117） | 19 |
| 文件/模块（顶层声明） | `find_visible_type`（L2284）/ `find_visible_value`（L2300） | `VisibleTypeEntry`（L69）+ `VisibleValueEntry`（L76） | 6 + 12 |
| 类型成员 | `find_type_field_member`（L7458）/ `find_type_method_member`（L9939）/ `find_type_static_member`（L9979） | `FengTypeMember` | 7 + 3 + 3 |

另外 `find_function_overload_set`（L2322）独立管理函数重载集（`FunctionOverloadSetEntry`，L61），调用 11 次。

### 0.2 三套机制的隔离问题

**块级查找**（`resolver_find_local_name_entry`）从内向外遍历 `ScopeFrame` 栈（L3140: `while (scope_index > 0U)`），到函数边界停止。不继续向上查找文件/模块层。

**文件级查找**（`find_visible_type` / `find_visible_value`）查两张独立的平表。两张表之间不交叉检查：
- `visible_types`：存放 TYPE / ENUM / SPEC
- `visible_values`：存放 FUNCTION / GLOBAL_BINDING

**类型成员查找**（`find_type_field_member` 等）直接遍历 `type_decl->as.type_decl.members` 数组，与作用域链无关。

### 0.3 visible_types 和 visible_values 分离的危害

两张表结构几乎相同（4/4 字段一致），`VisibleValueEntry` 比 `VisibleTypeEntry` 多两个字段：
- `is_function`（L82）：赋值点 L17537 直接写 `decl->kind == FENG_DECL_FUNCTION`，是 `decl->kind` 的冗余缓存
- `mutability`（L81）：赋值点 L22195 直接写 `decl->as.binding.mutability`，是 `decl` 字段的冗余缓存

消费点验证（全部可替换为 `decl` 派生）：
- `is_function`：12 处读取（L2763, L6253, L8149, L9123, L12259, L12639, L13474, L14963, L22166, L22196, L22211, L22315），全部等价于 `decl->kind == FENG_DECL_FUNCTION`
- `mutability`：2 处通过 entry 读取（L12262），其他消费点（L2769, L6257, L13478）已直接读 `decl->as.binding.mutability`

分离造成的实际问题：
1. `import_public_names`（L17317）中 4 个分支（TYPE / ENUM / SPEC / GLOBAL_BINDING+FUNCTION）写了近乎相同的冲突检测逻辑
2. `check_symbol_conflicts`（L22063）中 5 个分支（TYPE / ENUM / SPEC / GLOBAL_BINDING / FUNCTION）重复相同逻辑
3. `find_visible_type_index` / `find_visible_value_index`（L2116 / L2128）逻辑完全相同
4. `copy_visible_type_entries` / `copy_visible_value_entries`（L2382 / L2407）逻辑完全相同
5. 跨种类同名漏检：`check_symbol_conflicts` 中 TYPE 只查 `visible_types`（L22094），FUNCTION 只查 `visible_values`（L22207），两张表之间不互查。规范 §7（`docs/feng-module.md` L154）明确说"以下规则对 type、enum、func、spec、let / var 统一适用"，但实现未覆盖跨种类场景

### 0.4 ResolveContext 中的符号表分散

`ResolveContext`（L242）携带 5 组独立的符号表：

```c
const VisibleTypeEntry *visible_types;         // L246
const VisibleValueEntry *visible_values;       // L248
const FunctionOverloadSetEntry *function_sets; // L250
const AliasEntry *aliases;                     // L252
ScopeFrame *scopes;                            // L259
```

加上 `current_type_decl`（L262）用于隐式的类型作用域。查找一个裸名需要在多个结构上分别查询，没有统一的查找入口。

### 0.5 块级作用域链已有的基础

`resolver_find_local_name_entry`（L3136）已实现从内向外的 `ScopeFrame` 遍历：

```c
while (scope_index > 0U) {
    const ScopeFrame *frame = &context->scopes[scope_index - 1U];
    // 遍历 frame 内的所有 local
    --scope_index;
}
```

`resolver_push_scope` / `resolver_pop_scope` 管理作用域栈的进出（调用点 22 处）。这套机制已验证了"从当前块向上查找"的可行性，但目前只覆盖局部变量，到函数边界就停止。

## 1. 目标设计

### 1.1 统一作用域链

```
模块作用域 → 文件作用域 → [类型作用域] → 函数作用域 → 块作用域 → ... → 块作用域
```

查找从最内层向外走，第一个匹配的作用域即停止。同一作用域内同名按该层的冲突规则处理。

各层职责：

| 作用域层 | 包含的符号 | 冲突规则 |
|---|---|---|
| 模块 | 本模块所有文件的公开 type / enum / spec / func / let / var | 同名 func 允许重载；非 func 同名报错；func 与非 func 同名报错 |
| 文件 | 模块符号 + import 引入的符号 | 惰性歧义：多来源同名在使用点报错 |
| 类型 | field / let / method / static / ctor / dtor | 同名报错 |
| 函数 | 参数 | 同名报错 |
| 块 | 局部 let / var | 同名报错；遮蔽外层 |

### 1.2 统一作用域条目

```c
typedef struct ScopeEntry {
    FengSlice name;
    const FengDecl *decl;
    const FengSemanticModule *provider_module;   // 顶层声明的来源模块；局部符号为 NULL
    const FengProgram *provider_program;         // 顶层声明的来源文件；局部符号为 NULL
} ScopeEntry;
```

- `decl->kind` 决定符号种类，无需 `is_function` 字段
- `decl->as.binding.mutability` 直接获取可变性，无需 `mutability` 字段
- `provider_module` / `provider_program` 对局部符号为 NULL，区分顶层与局部

同一作用域层同一名称可有多个条目（func 重载）。

### 1.3 统一查找函数

一个入口覆盖所有层级：

```c
/* 从当前作用域向上查找裸名，返回第一个匹配作用域中的所有条目。
 * out_count 返回同名条目数（>1 表示重载或歧义）。 */
static const ScopeEntry *scope_chain_find(const ResolveContext *context,
                                          FengSlice name,
                                          size_t *out_count);
```

类型成员通过 `self.member` / `Type.member` 语法限定访问，不走裸名查找链。但类型作用域仍然是链的一部分，`self` 解析后在类型作用域层查找成员。

### 1.4 惰性歧义检测

文件作用域层的惰性歧义检测不再需要 `count_local_name_providers` / `count_imported_name_providers` 辅助函数。文件作用域本身记录了所有条目（本模块 + 各 import 模块），查找时直接检查：

- 所有条目来自同一个 provider_module → 不歧义
- 条目来自不同 provider_module → 歧义，在使用点报错

这统一覆盖了 import vs import、import vs 本模块、import vs 本文件三种场景。

### 1.5 函数重载

同一作用域层同一名称的多个 func 条目构成重载集。`FunctionOverloadSetEntry`（L61）不再需要独立维护——重载集由作用域层的多条目自然表达。

重载决议（参数匹配）逻辑不变，只是数据来源从独立的 `function_sets` 改为作用域层的多条目。

## 2. 与 import 惰性歧义优化（feng-module-optimize-dev.md）的关系

`feng-module-optimize-dev.md` 的步骤 4 需要在使用点增加惰性歧义检查。如果在当前架构上实现，需要新增 `count_local_name_providers` / `count_imported_name_providers` 辅助函数，在 `resolve_expr` / `resolve_named_type_ref` 等多个位置添加检查。

本优化（层级作用域链）从根本上消除了对辅助函数的需求——作用域链自身携带多来源信息，查找时自然检测歧义。

建议的执行顺序：

- **先执行本优化**（层级作用域链），再执行 `feng-module-optimize-dev.md` 的步骤 4
- 或者将 `feng-module-optimize-dev.md` 步骤 4 合并到本优化中

## 3. 实现步骤

### 3.1 统一作用域条目与查找函数

- [ ] 3.1.1 定义 `ScopeEntry` 结构体，替代 `VisibleTypeEntry` / `VisibleValueEntry` / `LocalNameEntry`
- [ ] 3.1.2 实现 `scope_chain_find`：从当前块向上遍历，返回第一个匹配层的所有条目
- [ ] 3.1.3 实现 `scope_chain_add`：向当前作用域层添加条目，按该层规则检查冲突
- [ ] 3.1.4 `ScopeFrame` 扩展为通用作用域帧，携带 `ScopeEntry` 数组和作用域层标记

### 3.2 合并 visible_types + visible_values

- [ ] 3.2.1 将 `visible_types` 和 `visible_values` 合并为一张 `ScopeEntry` 表
- [ ] 3.2.2 合并 `find_visible_type_index` / `find_visible_value_index` → `find_scope_entry_index`
- [ ] 3.2.3 合并 `copy_visible_type_entries` / `copy_visible_value_entries` → `copy_scope_entries`
- [ ] 3.2.4 `check_symbol_conflicts`（L22063）简化：5 个分支合并为通用冲突检测
- [ ] 3.2.5 `import_public_names`（L17317）简化：4 个分支合并为通用 import 逻辑
- [ ] 3.2.6 移除 `VisibleTypeEntry` / `VisibleValueEntry` 结构体定义

### 3.3 合并函数重载集

- [ ] 3.3.1 将 `FunctionOverloadSetEntry` 的多 decl 表达迁移到作用域层的多条目
- [ ] 3.3.2 更新重载决议逻辑（`resolve_top_level_function_overload` 等）从作用域条目读取
- [ ] 3.3.3 移除 `FunctionOverloadSetEntry` 结构体和相关函数

### 3.4 类型成员接入作用域链

- [ ] 3.4.1 进入函数体时，将当前类型的成员注入类型作用域层
- [ ] 3.4.2 `self.member` / `Type.member` 查找改为在类型作用域层查找
- [ ] 3.4.3 评估 `find_type_field_member` / `find_type_method_member` / `find_type_static_member` 是否可统一替换
- [ ] 3.4.4 fit 扩展方法的处理：fit 方法是目标类型的扩展，需在类型作用域层动态注入

### 3.5 ResolveContext 简化

- [ ] 3.5.1 移除 `visible_types` / `visible_type_count` / `visible_values` / `visible_value_count` / `function_sets` / `function_set_count`
- [ ] 3.5.2 ResolveContext 只保留作用域链 + aliases + imported_modules
- [ ] 3.5.3 更新所有消费 `ResolveContext` 符号表的调用点

### 3.6 惰性歧义检测

- [ ] 3.6.1 `scope_chain_find` 在文件作用域层检测多 provider_module → 报歧义
- [ ] 3.6.2 歧义错误码统一使用 AE0906
- [ ] 3.6.3 替代 `feng-module-optimize-dev.md` 步骤 4 的辅助函数需求

### 3.7 更新测试

- [ ] 3.7.1 更新 `feng-module-optimize-dev.md` 中受影响的现有测试（4 个）
- [ ] 3.7.2 新增 `feng-module-optimize-dev.md` 中的惰性行为测试（6 个）
- [ ] 3.7.3 新增跨种类同名冲突测试（type vs func、type vs binding 等）
- [ ] 3.7.4 通过 `make test` 全量回归测试

## 4. 依赖关系

```
步骤 3.1（统一条目与查找）── 无依赖
步骤 3.2（合并 visible 表）── 依赖 3.1
步骤 3.3（合并重载集）── 依赖 3.2
步骤 3.4（类型成员接入）── 依赖 3.1，可与 3.2/3.3 并行
步骤 3.5（Context 简化）── 依赖 3.2、3.3、3.4
步骤 3.6（惰性歧义）── 依赖 3.5
步骤 3.7（测试）── 依赖 3.6
```

## 5. 风险与注意事项

- **运行时性能**：作用域链查找从最内层向外遍历，最坏情况遍历所有层。当前块级查找已验证此模式的性能可接受。文件/模块层条目数通常较少（几十到几百），线性扫描开销可忽略。若后续有性能瓶颈，可在作用域层引入哈希索引，不改变语义。
- **fit 扩展方法**：fit 方法是动态注入到目标类型的扩展，不是类型声明时就确定的。类型作用域层需要支持动态注入 fit 方法，或在查找时额外查询 fit 注册表。需进一步设计。
- **self / 类型参数**：`self` 和类型参数是特殊的隐式符号，需要确定它们在作用域链中的位置（函数作用域层？独立层？）。
- **向后兼容**：此优化是纯内部重构，不影响语言语义和外部行为。所有现有测试应继续通过。
