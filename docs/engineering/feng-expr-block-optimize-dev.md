# 表达式块末尾语句自动提升为表达式

> 状态：待实施  
> 日期：2026-06-07

## 1. 背景

在 Feng 语言中，`if` 和 `try` 出现在语句起始位置时，parser 始终将其解析为语句节点（`FENG_STMT_IF` / `FENG_STMT_TRY`）。但 if/match/try 表达式的分支块要求最后一条语句必须是 `FENG_STMT_EXPR`（表达式语句），才能作为分支的求值结果（`block_yield_expression` 检查）。

这导致在表达式分支中嵌套 `if-else` 或 `try-catch` 时，必须加括号才能编译：

```feng
// 报错：match expression branch block must end with an expression statement
return if self.payload {
    bool { if self.payload { "true" } else { "false" } }
    else { "null" }
};

// 当前必须写成
return if self.payload {
    bool { (if self.payload { "true" } else { "false" }) }
    else { "null" }
};
```

**根因**：`parse_statement` 遇到 `if` 时直接调用 `parse_if_statement`（生成 `FENG_STMT_IF`），不会走到 `parse_simple_statement` → `parse_expression` → `parse_if_expression`（生成 `FENG_EXPR_IF`）。只有在表达式上下文中（如 `return if ...`、`let x = if ...`、括号内 `(if ...)`），`if` 才被解析为表达式节点。

## 2. 目标

在 semantic 阶段，对处于表达式上下文的 block，自动将末尾的 `FENG_STMT_IF` / `FENG_STMT_MATCH` / `FENG_STMT_TRY` 提升为 `FENG_STMT_EXPR`（包装对应的表达式节点），使嵌套写法无需括号即可编译。

**不提升的情况**（保持原有报错）：

- `if` 无 `else`（表达式必须有 else 分支）
- `if-else-if` 链（`clause_count > 1`，`FENG_EXPR_IF` 不支持 else-if 语法）
- 块末尾是 `return`、`break`、`continue` 等语句
- 非表达式上下文的块（函数体、循环体等不调用提升函数，行为不变）

## 3. AST 结构对应关系

### 3.1 FENG_STMT_IF → FENG_EXPR_IF

```
Statement:  { clauses: FengIfClause[], clause_count, else_block }
Expression: { condition, then_block, else_block }
```

可提升条件：`clause_count == 1` 且 `else_block != NULL`

转换方式：
- `expr.condition` ← `clauses[0].condition`
- `expr.then_block` ← `clauses[0].block`
- `expr.else_block` ← `else_block`
- `free(clauses)` 释放数组壳，内部指针已移走并置 NULL

### 3.2 FENG_STMT_MATCH → FENG_EXPR_MATCH

```
Statement:  { target, branches, branch_count, else_block }
Expression: { target, branches, branch_count, else_block }
```

两者结构完全一致，直接迁移所有字段。

### 3.3 FENG_STMT_TRY → FENG_STMT_EXPR

`FENG_STMT_TRY` 和 `FENG_STMT_EXPR` 共用 `stmt->as.expr` 字段（均指向 `FENG_EXPR_TRY`），`free_stmt` 对两者均调用 `free_expr(stmt->as.expr)`。直接改 `last->kind = FENG_STMT_EXPR` 即可，无需创建新节点或迁移数据。

提升后 `resolve_expr` → `resolve_try_expr(result_required=true)` 会自动验证 catch 块产出结果表达式。

## 4. 实现方案

### 4.1 新增 `block_promote_tail_to_expr` 函数

位置：`src/semantic/analyzer.c`，`validate_block_yields_expression` 函数之后。

```c
static void block_promote_tail_to_expr(FengBlock *block) {
    FengStmt *last;

    if (block == NULL || block->statement_count == 0U) {
        return;
    }
    last = block->statements[block->statement_count - 1U];
    if (last == NULL || last->kind == FENG_STMT_EXPR) {
        return;
    }

    /* FENG_STMT_IF → FENG_EXPR_IF：仅 simple if-else（clause_count == 1 且有 else） */
    if (last->kind == FENG_STMT_IF &&
        last->as.if_stmt.clause_count == 1U &&
        last->as.if_stmt.else_block != NULL) {
        FengExpr *expr = (FengExpr *)calloc(1U, sizeof(*expr));
        if (expr == NULL) { return; }
        expr->kind = FENG_EXPR_IF;
        expr->token = last->as.if_stmt.clauses[0].token;
        expr->as.if_expr.condition = last->as.if_stmt.clauses[0].condition;
        expr->as.if_expr.then_block = last->as.if_stmt.clauses[0].block;
        expr->as.if_expr.else_block = last->as.if_stmt.else_block;
        last->as.if_stmt.clauses[0].condition = NULL;
        last->as.if_stmt.clauses[0].block = NULL;
        last->as.if_stmt.else_block = NULL;
        free(last->as.if_stmt.clauses);
        last->kind = FENG_STMT_EXPR;
        last->as.expr = expr;
        return;
    }

    /* FENG_STMT_MATCH → FENG_EXPR_MATCH */
    if (last->kind == FENG_STMT_MATCH) {
        FengExpr *expr = (FengExpr *)calloc(1U, sizeof(*expr));
        if (expr == NULL) { return; }
        expr->kind = FENG_EXPR_MATCH;
        expr->token = last->token;
        expr->as.match_expr.target = last->as.match_stmt.target;
        expr->as.match_expr.branches = last->as.match_stmt.branches;
        expr->as.match_expr.branch_count = last->as.match_stmt.branch_count;
        expr->as.match_expr.else_block = last->as.match_stmt.else_block;
        last->as.match_stmt.target = NULL;
        last->as.match_stmt.branches = NULL;
        last->as.match_stmt.else_block = NULL;
        last->kind = FENG_STMT_EXPR;
        last->as.expr = expr;
        return;
    }

    /* FENG_STMT_TRY → FENG_STMT_EXPR：共用 stmt->as.expr，直接改 kind */
    if (last->kind == FENG_STMT_TRY) {
        last->kind = FENG_STMT_EXPR;
        return;
    }
}
```

### 4.2 在表达式块解析点插入提升调用

所有提升必须在 `resolve_block` / `resolve_block_contents` **之前**调用，确保提升后的节点走正确的表达式语义验证路径。

#### 4.2.1 Union match 表达式分支

`resolve_union_match_block_with_narrowing`（line ~6534），`resolve_block_contents` 之前：

```c
if (ok && out_yield_type != NULL) {
    block_promote_tail_to_expr((FengBlock *)block);
}
if (ok) {
    ok = resolve_block_contents(context, block, allow_self);
}
```

`out_yield_type != NULL` 即表示当前为表达式形式。

#### 4.2.2 Value match 表达式分支

`resolve_and_validate_match_common`（line ~6863），`resolve_match_branch_body` 之前：

```c
if (is_expression_form) {
    block_promote_tail_to_expr((FengBlock *)branches[branch_index].body);
}
if (!resolve_match_branch_body(context, &branches[branch_index], allow_self)) {
```

else block（line ~6888）同理：

```c
if (else_block != NULL) {
    if (is_expression_form) {
        block_promote_tail_to_expr((FengBlock *)else_block);
    }
    if (!resolve_block(context, else_block, allow_self)) {
```

#### 4.2.3 If 表达式 then/else 块

`FENG_EXPR_IF` 解析处（line ~17733），`resolve_block` 之前：

```c
block_promote_tail_to_expr((FengBlock *)expr->as.if_expr.then_block);
block_promote_tail_to_expr((FengBlock *)expr->as.if_expr.else_block);
ok = resolve_block(context, expr->as.if_expr.then_block, allow_self) &&
     resolve_block(context, expr->as.if_expr.else_block, allow_self);
```

此处始终为表达式上下文，无需条件判断。

#### 4.2.4 Try 表达式 catch 块

`resolve_try_expr`（line ~18052/18063），两处 `resolve_block` 之前：

```c
/* anonymous catch 分支 */
if (result_required) {
    block_promote_tail_to_expr((FengBlock *)clause->body);
}
ok = resolve_block(context, clause->body, allow_self) && ...

/* named catch 分支 */
if (result_required) {
    block_promote_tail_to_expr((FengBlock *)clause->body);
}
ok = ... resolve_block(context, clause->body, allow_self) && ...
```

## 5. 内存安全分析

| 提升类型 | 分配 | 释放路径 | 安全性 |
|---------|------|---------|-------|
| STMT_IF → EXPR_IF | `calloc` 新 `FengExpr`，`free(clauses)` 释放数组壳 | `free_stmt(STMT_EXPR)` → `free_expr(EXPR_IF)` → 释放 condition + then_block + else_block | 原 stmt 中移走的指针已置 NULL，不会 double-free |
| STMT_MATCH → EXPR_MATCH | `calloc` 新 `FengExpr` | `free_stmt(STMT_EXPR)` → `free_expr(EXPR_MATCH)` → 释放 target + branches + else_block | 原 stmt 中移走的指针已置 NULL，不会 double-free |
| STMT_TRY → STMT_EXPR | 无分配 | `free_stmt(STMT_EXPR)` → `free_expr(stmt->as.expr)` | 与 `free_stmt(STMT_TRY)` 路径完全一致 |

## 6. 测试

### 6.1 新增 smoke test

`test/smoke/phase1a/expr_tail_promote.ff` + `.expected`，覆盖：

1. union match 表达式分支末尾嵌套 if-else
2. if 表达式 then/else 块末尾嵌套 if-else
3. value match 表达式分支末尾嵌套 match
4. try 表达式 catch 块末尾嵌套 if-else
5. try 表达式 catch 块末尾嵌套 try-catch

### 6.2 全量回归

```bash
make test    # unit tests + smoke tests + cli tests + std tests
```

关注：
- 新增 smoke test 通过
- 已有 `if_expr_*`、`match_*`、`exception_try_expr` smoke test 不受影响
- `test_semantic` 单元测试全部通过
- `std-tests` 通过
