# 嵌套 Spec Union 赋值问题与优化方案

> **状态**：方案讨论阶段，尚未实施。
> **背景**：Parser 6.4 实施过程中发现 `LambdaBody: Expression | Block` 的嵌套 spec union 赋值失败（AE1003）。

---

## 1 问题描述

### 1.1 现象

```feng
open spec Expression: IdentifierExpr | BooleanLiteralExpr | ... | RangeExpr;  // 22 个变体
open spec LambdaBody: Expression | Block;

// 赋值 Block → LambdaBody ✓（Block 是具体类型，展开后直接匹配）
// 赋值 Expression → LambdaBody ✗（AE1003: expression does not match expected type）
```

### 1.2 根因

编译器在 `collect_normalized_union_member`（analyzer.c:5038）中对 spec union 做**完全展开**：

```c
// 遇到嵌套 union spec，递归展开其所有子变体
if (resolved is union spec) {
    for each nested member:
        collect_normalized_union_member(...);  // 递归
}
// 叶子类型才作为成员添加
return append_normalized_union_member(...);
```

展开后 `LambdaBody` 的成员列表为 23 个叶子类型：

```
LambdaBody = IdentifierExpr | BooleanLiteralExpr | ... | RangeExpr | Block
```

赋值时 `validate_expr_against_expected_type`（analyzer.c:17913）调用 `select_union_member_for_expr_type`，逐个成员做 `type_refs_semantically_equal` **直接类型对比**。`Expression` 不等于任何展开后的叶子类型，因此匹配失败。

### 1.3 影响范围

所有嵌套 spec union 场景均受影响：

- `LambdaBody: Expression | Block`
- `ForInit: SimpleBinding | AssignmentStmt`（如果 `SimpleBinding` 或 `AssignmentStmt` 本身是 spec union）
- 未来可能出现的更多嵌套 spec union 设计

---

## 2 三种优化方案

### 方案一：保持展开，优化赋值逻辑

**核心思路**：保持现有的完全展开行为不变，在赋值检查时增加对"源类型是 spec union"的特殊处理。

**赋值规则**：

- 如果新值的类型是 spec union S，且 S 的所有变体都是目标 spec union T 的成员（子集或全集），则允许赋值
- 运行时保留原始 tag，不需要重新打标

**match 行为**：

- 只能 match 展开后的具体类型（与当前行为一致）
- 不能 match 未展开的 `Expression`（因为它不在成员列表中）
- 单层收窄，无需多级 match

**修改点**：

- `validate_expr_against_expected_type`（analyzer.c:17913）：增加嵌套 spec union 的子集检查
- `select_union_member_for_expr_type`：当源类型是 spec union 时，检查其所有变体是否都在目标成员列表中

**示例**：

```feng
// 赋值
let expr: Expression = self.parseExpression();
let body: LambdaBody = expr;  // ✓ Expression 的所有变体都是 LambdaBody 的成员

// match（只能匹配展开后的类型）
match body {
    IdentifierExpr => ...
    BooleanLiteralExpr => ...
    Block => ...
    else => ...
}
```

**优点**：
- 改动最小，只修改赋值检查逻辑
- match 行为不变，保持简单
- 不影响现有代码

**缺点**：
- 赋值时需要遍历源 spec union 的所有变体，检查是否都是目标成员（性能开销）
- match 时无法用 `Expression` 一次性匹配所有表达式变体，需要逐个列出或用 `else`

---

### 方案二：改为不展开，多级 coerce

**核心思路**：spec union 不再自动展开，保持层次结构。赋值时做多级链路查找，match 时逐级收窄。

**赋值规则**：

- spec union 的成员列表保持声明时的结构（不递归展开）
- `LambdaBody` 的成员为：`Expression | Block`（2 个成员）
- 赋值时做多级检查：`IdentifierExpr` → 是 `Expression` 的成员？✓ → `Expression` 是 `LambdaBody` 的成员？✓ → 允许
- 如果多个父级成员都匹配（如 `spec S: A | B`，其中 `A` 和 `B` 都包含 `X` 作为子孙类型），报错要求显式转换

**match 行为**：

- 只能 match 槽位声明的直接类型
- `LambdaBody` 只能 match `Expression | Block`
- 在 `Expression` 分支内，需要再次 match 具体变体（多级收窄）

**修改点**：

- `collect_normalized_union_member`（analyzer.c:5038）：移除递归展开逻辑
- `select_union_member_for_expr_type`：增加多级链路查找
- match 穷尽性检查：改为只检查直接成员
- 代码生成：多级 tag 表示（每个嵌套层一个 tag）

**示例**：

```feng
// 赋值
let body: LambdaBody = someIdentifierExpr;  // ✓ IdentifierExpr → Expression → LambdaBody

// match（多级收窄）
match body {
    Expression => match body {
        IdentifierExpr => ...
        BooleanLiteralExpr => ...
        else => ...
    }
    Block => ...
}
```

**优点**：
- 类型层次清晰，与声明结构一致
- 赋值自然（类似 Rust 的 enum 嵌套）
- match 可以用 `Expression` 一次性匹配所有表达式变体

**缺点**：
- 改动较大，涉及展开逻辑、赋值检查、match 收窄、代码生成
- match 需要多级收窄，代码更冗长
- 多级 tag 的运行时表示增加复杂度
- 可能影响现有依赖展开行为的代码

---

### 方案三：保持展开 + 保留未展开类型占用槽位

**核心思路**：展开所有子变体作为成员，同时保留未展开的 spec union 本身也作为一个成员槽位。

**赋值规则**：

- `LambdaBody` 的成员为：`Expression | IdentifierExpr | BooleanLiteralExpr | ... | RangeExpr | Block`（24 个成员）
- 赋值时做直接类型对比（`type_refs_semantically_equal`）
- `IdentifierExpr` 值 → 匹配 `IdentifierExpr` 槽位（不匹配 `Expression` 槽位，因为 `IdentifierExpr ≠ Expression`）
- `Expression` 值 → 匹配 `Expression` 槽位
- **无歧义**：编译器做的是精确类型对比，不是递归成员检查

**match 行为**：

- 可以 match 展开后的具体类型，也可以 match `Expression` 槽位
- 进入时的槽位决定 match 的行为：
  - 如果值以 `IdentifierExpr` 槽位进入，match `IdentifierExpr` 分支命中
  - 如果值以 `Expression` 槽位进入，match `Expression` 分支命中
- 同一值不会同时匹配 `Expression` 和 `IdentifierExpr` 槽位（取决于进入时的 tag）

**修改点**：

- `collect_normalized_union_member`（analyzer.c:5038）：遇到嵌套 spec union 时，既保留它作为成员，又展开其子变体

```c
// 修改后
if (resolved is union spec) {
    append_normalized_union_member(member_ref);  // 保留 Expression 本身
    for each nested member:
        collect_normalized_union_member(...);     // 同时展开子变体
}
```

- match 穷尽性检查：需要理解 `Expression` 槽位覆盖其所有展开子变体
- 代码生成：tag 值需要区分 `Expression` 槽位和展开子变体槽位

**示例**：

```feng
// 赋值
let expr: Expression = self.parseExpression();
let body: LambdaBody = expr;  // ✓ 匹配 Expression 槽位

let ident: IdentifierExpr = ...;
let body2: LambdaBody = ident;  // ✓ 匹配 IdentifierExpr 槽位

// match（可以匹配 Expression 或具体类型）
match body {
    Expression => ...   // 匹配以 Expression 槽位进入的值
    Block => ...
}

match body {
    IdentifierExpr => ...   // 匹配以 IdentifierExpr 槽位进入的值
    BooleanLiteralExpr => ...
    Block => ...
    else => ...
}
```

**优点**：
- 赋值自然，无需特殊逻辑（直接类型对比即可）
- match 灵活，可以用 `Expression` 或具体类型
- 无歧义（精确类型对比）
- 对现有代码影响小（展开行为保持不变，只是多了一个 `Expression` 槽位）

**缺点**：
- match 穷尽性检查更复杂（需要理解 `Expression` 槽位与展开子变体的覆盖关系）
- 成员列表变长（多了一个冗余槽位）
- 需要明确"进入时的槽位"语义（值以哪个槽位进入取决于赋值源的类型）

---

## 3 方案对比

| 维度 | 方案一：优化赋值 | 方案二：不展开 | 方案三：展开 + 保留 |
|------|-----------------|---------------|-------------------|
| 改动范围 | 小（仅赋值检查） | 大（展开 + 赋值 + match + 代码生成） | 中（展开逻辑 + match 穷尽性） |
| 赋值行为 | 子集检查 | 多级链路查找 | 直接对比（无歧义） |
| match 层数 | 单层 | 多层 | 单层 |
| match 灵活性 | 只能匹配具体类型 | 可匹配父级类型 | 可匹配父级或具体类型 |
| 运行时表示 | 不变（单 tag） | 多级 tag | 单 tag（多一个槽位值） |
| 对现有代码影响 | 无 | 可能较大 | 小 |
| 穷尽性检查 | 不变 | 改为直接成员 | 需理解覆盖关系 |

---

## 4 待决策事项

1. 选择哪个方案
2. 如果选方案三，match 穷尽性检查的具体规则（`Expression` 槽位是否覆盖所有子变体）
3. 如果选方案二，多级 tag 的运行时表示方案

---

## 5 参考

- 问题发现：Parser 6.4 实施（`LambdaBody: Expression | Block`）
- 相关代码：`src/semantic/analyzer.c`（`collect_normalized_union_member`、`validate_expr_against_expected_type`、`select_union_member_for_expr_type`）
- AST 定义：`std/src/compiler/parser/FengAstNodes.ff`
