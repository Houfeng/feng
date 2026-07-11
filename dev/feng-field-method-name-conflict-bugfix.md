# 字段-方法同名冲突：声明期编译报错

> 状态：开发中（dev）

## 背景

`std/src/tui/Cell.ff` 中 `seal var value: u64` 字段与 `open func value(): u64` 方法同名，同模块内编译通过，但跨模块访问 `cell.value()` 时报：

```
AE0308: member 'value' of type 'Cell' is not accessible from the current module
```

根因是 Feng 编译器在声明期不检查字段-方法同名，在访问期 `find_instance_member` 按声明顺序线性扫描、字段命中即返回不回退方法，`seal var` 遮蔽同名 `open func`，导致跨模块不可用。

## 根因分析

### 问题 1：声明期不检查字段-方法同名

`validate_type_member_overloads`（`src/semantic/analyzer.c` 第 19160 行）负责成员重名检查，但两个 `continue` 守卫（第 19179、19188 行）跳过了所有非 METHOD 成员：

- 字段-字段同名 → 不检查
- 字段-方法同名 → 不检查
- 仅方法-方法重复签名 → 报 `AE0508`

### 问题 2：访问期字段遮蔽同名方法

`find_instance_member`（`src/semantic/analyzer.c` 第 9813 行）在同一个循环里按声明顺序线性扫描字段和方法，**先声明者胜出并立即 return**，不回退尝试同名方法。`Cell.ff` 中 `seal var value` 在 `open func value()` 之前声明，因此 `find_instance_member` 命中字段，方法永远不被触达。

跨模块访问走到 `validate_instance_member_expr`（第 14296 行）第 14585 行调用 `find_instance_member` 拿到 `seal var value` 字段，第 14593 行 `type_member_is_accessible_from` 返回 false（字段是 `seal`），直接报 `AE0308`，**根本不会回退去尝试同名方法**。

### 问题 3：成员注册链路无唯一性校验

`src/symbol/export.c` 的 `append_member_decl`（第 1715 行）→ `append_decl_pointer`（第 201 行）只做 realloc + 追加，完全不做唯一性校验。语义层 module 级符号唯一性检查 `check_symbol_conflicts`（第 26424 行）只处理模块顶层声明，不进入 type 内部成员层级。

## 设计方案

### 原则

1. **声明期报错优于访问期报错**：在语义分析阶段就拦截字段-方法同名，而非推迟到访问期才发现不可见。
2. **冲突面按访问路径划分**：Feng 中静态成员通过 `TypeName.member` 访问，实例成员通过 `self.member` 访问，两者不在同一查找路径。静态成员与实例成员之间不构成冲突面。
3. **不区分泛型与非泛型**：字段名与方法名同名一律报错，不区分方法是否有泛型参数。简单一致，无歧义。

### 冲突面划分

- **实例字段 vs 实例方法**（同一冲突面，需检查）
- **静态字段 vs 静态方法**（同一冲突面，需检查）
- 静态 vs 实例之间不冲突（访问路径不同，不检查）

### 变更 1：新增校验函数

**文件**: `src/semantic/analyzer.c`
**位置**: `validate_type_member_overloads`（第 19160 行）之后

新增 `validate_type_member_field_method_name_conflict`：

- 遍历所有 METHOD 成员
- 对每个方法，向前扫描所有 FIELD 成员
- 当 `mi->is_static == mj->is_static` 且 `slice_equals(si->name, mj->as.field.name)` 时，报 `AE0513`
- 不区分泛型与非泛型方法

### 变更 2：注册校验调用

**文件**: `src/semantic/analyzer.c`
**位置**: `resolve_declaration` 的 `FENG_DECL_TYPE` 分支（第 25974 行之后）

在 `validate_type_member_overload_overlap` 之后、`validate_type_finalizer_constraints` 之前，新增：

```c
if (ok && !validate_type_member_field_method_name_conflict(context, decl)) {
    ok = false;
}
```

### 变更 3：新增错误码

**文件**: `docs/feng-error-codes-ae.md`
**位置**: AE05 段末尾

```
| AE0513 | 字段-方法同名冲突约束 | AE0513 | field '%.*s' and method '%.*s' in type '%.*s' cannot share the same name within the same conflict surface (static or instance) |
```

### 变更 4：文档规则

**文件**: `docs/feng-type.md`

- §5 规则段新增：`[必须] 同一冲突面内字段与方法不得同名; 实例字段与实例方法属于同一冲突面,静态字段与静态方法属于同一冲突面; 静态成员与实例成员因访问路径不同不构成冲突面; 违反时编译期报错。`
- 编译器规则段新增：`编译器必须在语义阶段检查同一 type 内同一冲突面中字段与方法是否同名,并在编译期报错; 静态成员与实例成员因访问路径不同不构成冲突面,不在此检查范围内。`

### 变更 5：语义测试

**文件**: `test/semantic/test_semantic.c`

新增测试用例：

1. `test_type_field_method_same_name_is_rejected`：实例字段 + 同名实例方法 → 期望 `AE0513`
2. `test_type_static_field_static_method_same_name_is_rejected`：静态字段 + 同名静态方法 → 期望 `AE0513`
3. `test_type_field_static_method_same_name_allowed`：实例字段 + 同名静态方法 → 期望通过（不同冲突面）
4. `test_type_static_field_instance_method_same_name_allowed`：静态字段 + 同名实例方法 → 期望通过（不同冲突面）

### 变更 6：修复 Cell.ff

**文件**: `std/src/tui/Cell.ff`

`value`/`style` 两对字段-方法同名需要重命名（方法或字段之一），由人工决策改名方向。

## 影响范围

- 不影响已有测试用例（没有任何测试锁定「字段-方法同名应通过」的行为）
- 不影响 fit 成员（fit 不允许声明字段，只有方法）
- 不影响构造函数（构造函数名 = 类型名，属于不同成员种类）
