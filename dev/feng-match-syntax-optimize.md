# match 语法优化：引入独立 match 关键字

## 状态: dev

## 1 问题背景

当前 Feng 复用 `if` 关键字承载两种语义：

1. **条件分支**：`if bool_expr { ... } else { ... }`
2. **模式匹配**：`if target { label { ... } ... else { ... } }`（包括值匹配、区间匹配、union member 匹配）

解析器通过 `peek_match_body()` 函数（约 100 行前瞻逻辑）在 `{` 内部判断分支结构来区分两者。

存在的问题：

- **可读性差**：`if value { ... }` 和 `if condition { ... }` 在第一眼无法区分，读者必须查看 `{` 内部才能判断语义。
- **解析复杂度高**：`peek_match_body()` 需要复杂的前瞻扫描来消除歧义，且随着语言演进（新增分支标签形式），维护成本可能上升。
- **行业惯例差异**：Rust、Swift、Kotlin、Scala、OCaml 等语言均使用独立的 `match` 关键字。
- **语义不精确**：`if` 表达"条件判断"，`match` 表达"模式匹配"，两者本质不同。当前错误信息也只能写 "if-match" 这一非关键字术语。

## 2 设计目标

- 引入 `match` 作为独立关键字，用于模式匹配语句和表达式。
- `if` 仅用于条件分支，不再承担模式匹配语义。
- 匹配体语法（分支标签、绑定前缀、`else` 分支）保持不变。
- AST 层面 `FENG_STMT_MATCH` / `FENG_EXPR_MATCH` 已独立，无需变更。

## 3 语法变更

### 3.1 语句形式

```feng
// 变更前
if value {
    0 { "zero"; }
    1...9 { "small"; }
    else { "other"; }
}

// 变更后
match value {
    0 { "zero"; }
    1...9 { "small"; }
    else { "other"; }
}
```

### 3.2 表达式形式

```feng
// 变更前
let label = if age {
    0 { "infant"; }
    1...17 { "minor"; }
    else { "adult"; }
};

// 变更后
let label = match age {
    0 { "infant"; }
    1...17 { "minor"; }
    else { "adult"; }
};
```

### 3.3 union member 匹配（含绑定）

```feng
// 变更前
if value {
    v: Success { println("ok: {0}", v.value); }
    e: Failure { println("fail: {0}", e.message); }
    else { println("other"); }
}

// 变更后
match value {
    v: Success { println("ok: {0}", v.value); }
    e: Failure { println("fail: {0}", e.message); }
    else { println("other"); }
}
```

### 3.4 条件 if 不受影响

```feng
// 条件分支 — 语法不变
if a > b {
    // ...
} else if a == b {
    // ...
} else {
    // ...
}

// if 表达式 — 语法不变
let label = if score >= 60 {
    "pass";
} else {
    "fail";
};
```

### 3.5 match 不支持 else if 链

`match` 的分支结构是标签匹配，不存在 `else match` 链的需求。`match` 体内部只包含标签分支和可选的 `else` 分支，与当前 `if-match` 的规则一致。

### 3.6 match 表达式作为块尾语句

与当前 `if-match` 行为一致，`match` 语句出现在块尾且块需要返回值时，编译器自动将其转换为 `match` 表达式。

## 4 影响范围分析

### 4.1 Lexer（src/lexer/token.h）

- 在 `FENG_KEYWORD_LIST` 中新增 `X(MATCH, "match")`，产生 `FENG_TOKEN_KW_MATCH` 令牌。

### 4.2 Parser（src/parser/parser.c）

#### 4.2.1 新增 match 语句解析入口

- 在 `parse_statement` 中新增 `FENG_TOKEN_KW_MATCH` 分支，调用 `parse_match_statement`。
- 新建 `parse_match_statement` 函数：消费 `match` 关键字 → 解析目标表达式 → 期望 `{` → 调用 `parse_match_body`。

#### 4.2.2 新增 match 表达式解析入口

- 在 `parse_primary` 中新增 `FENG_TOKEN_KW_MATCH` 分支，调用 `parse_match_expression`。
- 新建 `parse_match_expression` 函数：消费 `match` 关键字 → 解析目标表达式 → 期望 `{` → 调用 `parse_match_body` → 校验 `else` 分支存在性。

#### 4.2.3 简化 if 解析

- `parse_if_statement` 中移除 `peek_match_body()` 调用及 `FENG_STMT_MATCH` 分支创建逻辑，if 仅处理条件分支。
- `parse_if_expression` 中移除 `peek_match_body()` 调用及 `FENG_EXPR_MATCH` 分支创建逻辑，if 仅处理条件表达式。

#### 4.2.4 token_starts_expression

- 新增 `FENG_TOKEN_KW_MATCH` 到 `token_starts_expression` 函数。

#### 4.2.5 块尾转换

- `convert_trailing_yield_stmt_to_expr` 中 `FENG_STMT_MATCH` 分支保持不变，继续调用 `convert_match_stmt_to_match_expr`。
- `convert_match_stmt_to_match_expr` 函数本身无需修改。

#### 4.2.6 辅助函数保持不变

以下函数无需修改，仅由 `match` 解析路径调用：

- `peek_match_body` — 不再用于 if/match 歧义消除，可视实现需要决定是否保留或移除。
- `parse_match_body`
- `parse_match_branch`
- `parse_match_branch_binding_prefix`
- `parse_match_label`
- `parse_match_label_atom`
- `is_match_label_atom_token`
- `is_type_label_start_token`
- `peek_scan_type_ref_label`

### 4.3 Semantic Analyzer（src/semantic/）

- `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` 已有独立的 case 分支，与 `FENG_STMT_IF` / `FENG_EXPR_IF` 完全分离，无需修改。
- 涉及文件：`analyzer.c`（约 10 处 case）、`reifiable_deps.c`（2 处 case）。

### 4.4 Codegen（src/codegen/codegen.c）

- `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` 已有独立的 case 分支，无需修改。
- 错误信息中 "if-match" 字样更新为 "match"（CE0269、CE0270）。

### 4.5 Symbol Export（src/symbol/export.c）

- `FENG_STMT_MATCH` case 分支已独立，无需修改。

### 4.6 LSP / CLI（src/cli/lsp/runtime.c）

- `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` case 分支已独立，无需修改。

### 4.7 Parser Dump（src/parser/dump.c）

- `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` case 分支已独立，无需修改。

### 4.8 Debug / DAP

- 无 match 相关引用，无需修改。

## 5 错误信息更新

| 错误码 | 变更前 | 变更后 |
| ------ | ------ | ------ |
| SE1103 | if-match expressions require an 'else' branch | match expressions require an 'else' branch |
| SE1104 | match expression cannot declare more than one 'else' branch | （已正确，无需修改） |
| SE1106 | expected '}' to close match body | （已正确，无需修改） |
| CE0269 | codegen: if-match branch has no labels | codegen: match branch has no labels |
| CE0270 | codegen: if-match target must be integer, bool, or string | codegen: match target must be integer, bool, or string |

## 6 文档更新

### 6.1 规范文档（docs/）

| 文件 | 变更内容 |
| ---- | -------- |
| `feng-flow.md` | 第 3 节"条件匹配形式"改为"模式匹配"，所有 `if 目标值 { ... }` 改为 `match 目标值 { ... }`；第 4 节示例中 match 相关代码改为 `match`；"if-match" 术语统一改为 "match" |
| `feng-spec.md` | `if 目标值 { ... }` 引用改为 `match 目标值 { ... }`；union member 匹配描述中 `if 目标值` 改为 `match 目标值` |
| `feng-union-type.md` | `if 目标值 { ... }` 引用改为 `match 目标值 { ... }` |
| `feng-exception.md` | 3.6 节标题和正文中 "if-match" 改为 "match" |
| `feng-error-codes-se.md` | SE1103 描述更新 |
| `feng-error-codes-ce.md` | CE1045、CE1046 描述中 "if-match" 改为 "match" |
| `feng-error-codes.md` | CE0196、CE0269、CE0270 描述中 "if-match" 改为 "match" |
| `feng-error-codes-ae.md` | 分段标题无实质变更 |

### 6.2 开发文档（dev/）

以下为已交付的历史文档，仅记录设计过程，不做回溯修改：

- `feng-if-match-optimize-dev.md`
- `feng-if-expr-allow-throw-dev.md`
- `feng-exception-dev.md`
- `feng-expr-block-optimize-dev.md`
- `feng-generics-delivered-dev.md`
- `feng-semantic-logic-c2s-draft.md`
- `feng-union-type-dev.md`

### 6.3 编辑器（editors/feng-vscode/syntaxes/）

| 文件 | 变更内容 |
| ---- | -------- |
| `feng.tmLanguage.json` | `keywords` 的 control 关键字正则中追加 `match` |

## 7 测试文件更新

### 7.1 需要重命名的测试文件（match 语义）

以下文件使用 `if target { ... }` 模式匹配语法，需要将文件及对应 `.expected` 文件重命名，并将源码中 `if` 改为 `match`：

| 原文件名 | 新文件名 | 说明 |
| -------- | -------- | ---- |
| `if_match_int.ff` / `.expected` | `match_int.ff` / `.expected` | 整型值匹配 + 区间匹配 |
| `if_match_string.ff` / `.expected` | `match_string.ff` / `.expected` | 字符串值匹配 |
| `if_match_aggregate.ff` / `.expected` | `match_aggregate.ff` / `.expected` | match 表达式返回 aggregate |
| `if_match_binding.ff` / `.expected` | `match_binding.ff` / `.expected` | union member 匹配 + 绑定 |

### 7.2 需要修改但不改名的测试文件

以下文件包含 `if value { ... }` 模式匹配代码段，需要将相关 `if` 改为 `match`：

| 文件 | 说明 |
| ---- | ---- |
| `union_form.ff` | 3 处 `if value { ... }` union member 匹配 |
| `union_form_generic.ff` | 1 处 `if value { ... }` union member 匹配 |
| `union_tuple_cleanup.ff` | 1 处 `if value { ... }` union member 匹配 |

### 7.3 不需要修改的测试文件

以下文件使用条件分支语法，不受影响：

- `if_expr_int.ff` / `.expected` — 条件 if 表达式
- `if_expr_string.ff` / `.expected` — 条件 if 表达式
- `if_expr_aggregate.ff` / `.expected` — 条件 if 表达式
- `if_expr_return.ff` / `.expected` — 条件 if 表达式
- `defaults.ff` — 条件 if 语句
- `exceptions.ff` — 条件 if 语句

## 8 标准库与示例更新

### 8.1 标准库

| 文件 | 变更内容 |
| ---- | -------- |
| `std/src/basic/Option.ff` | 文档注释中 `if value { ... }` 改为 `match value { ... }`（2 处） |

### 8.2 示例

| 文件 | 变更内容 |
| ---- | -------- |
| `examples/std_demo/src/debug.ff` | `let y = if message { ... }` 改为 `let y = match message { ... }` |

## 9 实施步骤

按依赖顺序执行：

1. **文档变更**：更新 `feng-flow.md`、`feng-spec.md`、`feng-union-type.md`、`feng-exception.md`、错误码文档。
2. **Lexer**：在 `token.h` 中新增 `MATCH` 关键字。
3. **Parser**：新增 `parse_match_statement` / `parse_match_expression`；简化 `parse_if_statement` / `parse_if_expression` 移除 `peek_match_body` 分支；更新错误信息。
4. **Codegen**：更新 CE0269、CE0270 错误信息中的 "if-match" 字样。
5. **编辑器**：更新 `feng.tmLanguage.json` 关键字列表。
6. **测试文件**：重命名 `if_match_*` → `match_*`，修改所有 match 语义测试文件中的 `if` → `match`。
7. **标准库与示例**：更新 `Option.ff` 文档注释和 `debug.ff` 示例。
8. **全量回归测试**：确保所有测试通过。

## 10 风险评估

| 风险 | 等级 | 缓解措施 |
| ---- | ---- | -------- |
| `peek_match_body` 移除后 if 解析正确性 | 低 | if 解析移除后不再需要区分，仅处理条件分支，逻辑更简单 |
| `match` 关键字与用户代码标识符冲突 | 低 | Feng 已有关键字保护机制，`match` 作为新关键字的行为与其他关键字一致 |
| 测试文件重命名导致 CI 脚本路径失效 | 低 | 测试运行器按目录扫描，不依赖硬编码文件名 |
| 历史 dev 文档中 "if-match" 术语与现行语法不一致 | 低 | 历史文档记录设计过程，不做回溯修改，不影响用户 |

## 建议 commit message

```
feat: introduce dedicated match keyword for pattern matching

Replace the overloaded `if` keyword for pattern matching with a
dedicated `match` keyword. The `if` keyword now exclusively handles
boolean conditional branching, while `match` handles value matching,
range matching, and union member matching.

This change eliminates the need for the parser's peek_match_body()
lookahead disambiguation and improves code readability by making the
intent of each construct immediately clear.
```
