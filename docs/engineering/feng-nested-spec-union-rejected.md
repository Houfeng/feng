# 嵌套 Spec Union — 落选方案

> **状态**：已落选，仅供存档参考。
> **决策**：选择方案二（不展开 + 多级链路），详见 [feng-nested-spec-union-draft.md](feng-nested-spec-union-draft.md)。

---

## 方案一：保持展开，优化赋值逻辑

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
    IdentifierExpr { ... }
    BooleanLiteralExpr { ... }
    Block { ... }
    else { ... }
}
```

**优点**：
- 改动最小，只修改赋值检查逻辑
- match 行为不变，保持简单
- 不影响现有代码

**缺点**：
- 赋值时需要遍历源 spec union 的所有变体，检查是否都是目标成员（性能开销）
- match 时无法用 `Expression` 一次性匹配所有表达式变体，需要逐个列出或用 `else`
- 子集检查是补丁式修复，不符合类型层次的设计原则

**落选原因**：
- match 不能匹配父类型，随着嵌套增多会越来越痛
- 子集检查逻辑在方案二中会被完全替换，属于临时 workaround

---

## 方案三：保持展开 + 保留未展开类型占用槽位

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
    Expression { ... }   // 匹配以 Expression 槽位进入的值
    Block { ... }
}

match body {
    IdentifierExpr { ... }   // 匹配以 IdentifierExpr 槽位进入的值
    BooleanLiteralExpr { ... }
    Block { ... }
    else { ... }
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

**落选原因**：
- "进入槽位"语义是陷阱：同一个 `IdentifierExpr` 值，通过 `Expression` 赋值和直接赋值，在 match 中走不同分支，行为取决于赋值路径而非值本身
- 穷尽性检查的覆盖关系增加认知负担
