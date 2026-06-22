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

### 4.0 改动规模概述

AST 层面 `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` 已完全独立，semantic / codegen / dump / export / lsp / reifiable_deps 各阶段的 case 分支早已分开，无需改动。改动集中在 parser 入口分发层：把原 `parse_if_statement` / `parse_if_expression` 中的 match 分支抽成独立的 `parse_match_statement` / `parse_match_expression`，并删除用于 if/match 歧义消除的 `peek_match_body` 前瞻逻辑及其依赖函数 `peek_scan_type_ref_label`。

核心编译器（C 代码）净变化约 +50 / -190 = -140 行，且新增的 50 行基本是从原函数搬出的代码，逻辑不变。工作量主体在 Feng 侧（测试重命名 + 标准库/示例的关键字替换）和文档全局替换 `if 目标值` → `match 目标值`、`if-match` → `match` 上。

C 代码各文件改动规模：

| 文件 | 改动类型 | 规模 |
| ---- | -------- | ---- |
| `src/lexer/token.h` | 新增 `MATCH` 关键字 | +1 行 |
| `src/parser/parser.c` | 新增 match 入口、删除 if 中的 match 分支、删除 `peek_match_body` / `peek_scan_type_ref_label`、更新错误信息与注释 | +50 / -190 行 |
| `src/codegen/codegen.c` | 错误信息字符串更新 | 3 处 |
| `src/semantic/analyzer.c` | 注释更新 | 1 处 |
| `src/parser/dump.c` / `src/symbol/export.c` / `src/cli/lsp/runtime.c` / `src/semantic/reifiable_deps.c` | 不动 | 0 |

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

#### 4.2.6 辅助函数处置

引入独立 `match` 关键字后，`match` 入口无需前瞻即可直接进入 `parse_match_body`，`if` 入口也不再处理 match 语义。原用于 if/match 歧义消除的前瞻逻辑完全失去调用方，应删除：

- `peek_match_body` — 删除。原仅被 `parse_if_statement`(parser.c:4008) 和 `parse_if_expression`(parser.c:3306) 调用，这两处移除 match 分支后无任何调用方。
- `peek_scan_type_ref_label` — 删除。原仅被 `peek_match_body` 内部调用，随 `peek_match_body` 一并删除。

以下函数继续保留，由 `parse_match_statement` / `parse_match_expression` 路径调用，无需修改：

- `parse_match_body`
- `parse_match_branch`
- `parse_match_branch_binding_prefix`
- `parse_match_label`
- `parse_match_label_atom`
- `is_match_label_atom_token`
- `is_type_label_start_token`

### 4.3 Semantic Analyzer（src/semantic/）

- `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` 已有独立的 case 分支，与 `FENG_STMT_IF` / `FENG_EXPR_IF` 完全分离，无需修改。
- 涉及文件：`analyzer.c`（8 处 case：`FENG_EXPR_MATCH` 5 处、`FENG_STMT_MATCH` 3 处）、`reifiable_deps.c`（2 处 case）。
- 注释更新：`analyzer.c:6370` 注释 "if/if-match expression branches" 中 "if-match" 改为 "match"。

### 4.4 Codegen（src/codegen/codegen.c）

- `FENG_STMT_MATCH` 和 `FENG_EXPR_MATCH` 已有独立的 case 分支，无需修改。
- 错误信息中 "if-match" 字样更新为 "match"（CE0269 ×2 处、CE0270 ×1 处）。
- CE0196 当前文本为 `"codegen: missing aggregate descriptor for if/match result slot"`，其中 "if/match" 是 if 表达式与 match 表达式**共用**的分支发射路径 `cg_emit_branch_into_slot` 触发的（该函数同时被 `FENG_EXPR_IF`、`FENG_STMT_IF` 和 `FENG_EXPR_MATCH`、`FENG_STMT_MATCH` 调用），"if/match" 合并写法准确反映实际触发场景，**保留不动**。

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
| SE1106 (parser.c:3146) | expected '}' to close match body | （已正确，无需修改） |
| SE1106 (parser.c:3366) | expected '}' to close the true branch of if expression | （已正确，无需修改） |
| SE1106 (parser.c:4003) | expected '{' after if condition or match target | expected '{' after if condition |
| SE1106 (parser.c:4067) | expected '}' to close if block | （已正确，无需修改） |
| CE0196 | codegen: missing aggregate descriptor for if/match result slot | （if/match 共用路径触发，保留不动） |
| CE0269 | codegen: if-match branch has no labels | codegen: match branch has no labels |
| CE0270 | codegen: if-match target must be integer, bool, or string | codegen: match target must be integer, bool, or string |

注：SE1106 在 parser.c 中有 4 处不同消息文本，其中 parser.c:4003 的消息包含 "match target" 字样，引入独立 `match` 关键字后该入口只处理条件 if，应移除 "or match target" 部分；其余 3 处消息已分别针对 match body / if expression / if block，无需修改。

## 6 文档更新

### 6.1 规范文档（docs/）

| 文件 | 变更内容 |
| ---- | -------- |
| `feng-flow.md` | 第 3 节"条件匹配形式"改为"模式匹配"，所有 `if 目标值 { ... }` 改为 `match 目标值 { ... }`（第 33、40、79 行）；第 3.1 节代码示例 `if age {` 改为 `match age {`（第 58 行）；第 3.2 节代码示例 `if v {` 改为 `match v {`（第 94 行）；第 4 节中 match 表达式示例 `let label = if age {` 改为 `let label = match age {`（第 132 行），第 124 行的条件 if 表达式示例不动；第 120 行 "if/else、if-match 或 try/catch" 中 "if-match" 改为 "match" |
| `feng-spec.md` | `if 目标值 { ... }` 引用改为 `match 目标值 { ... }`（第 167、251 行）；union member 匹配描述中 `if 目标值` 改为 `match 目标值` |
| `feng-union-type.md` | 所有 `if 目标值 { ... }` 引用改为 `match 目标值 { ... }`（第 5、143、145、183、188、190、380、381、508、556、645 行）；代码示例 `if v {` 改为 `match v {`（第 164、202、214、227、238 行） |
| `feng-exception.md` | 3.6 节标题 `### 3.6 \`if\`/\`if-match\` 表达式分支中的 \`throw\`` 改为 `### 3.6 \`if\`/\`match\` 表达式分支中的 \`throw\``（第 144 行）；正文 "\`if\` 表达式和 \`if-match\`（match）表达式" 改为 "\`if\` 表达式和 \`match\` 表达式"（第 146 行） |
| `feng-error-codes-se.md` | SE1103 描述更新（"if-match 表达式需要 else 分支" → "match 表达式需要 else 分支"）；SE1106 描述中 "expected '{' after if condition or match target" 同步更新为 "expected '{' after if condition" |
| `feng-error-codes-ce.md` | CE1045、CE1046 描述中 "if-match" 改为 "match"（第 438、439 行） |
| `feng-error-codes.md` | CE0269、CE0270 描述中 "if-match" 改为 "match"（第 484、485 行）；CE0196 描述保留 "if/match" 不动（第 411 行） |
| `feng-error-codes-ae.md` | 第 11 节标题 "if/match 分支完备性与标签约束" 改为 "match 分支完备性与标签约束"（第 26 行） |
| `feng-generics-draft.md` | `if 目标值 { ... }` 引用改为 `match 目标值 { ... }`（第 282、283 行） |

### 6.2 开发文档（dev/）

历史文档中的旧 `if-match` 术语同步更新为 `match`，但以下文档作为例外保留不动：

| 文件 | 变更内容 |
| ---- | -------- |
| `feng-if-and-match-expr-allow-throw-dev.md` | 标题 `# if/if-match/try 表达式分支块允许以 throw 结尾` 改为 `# if/match/try 表达式分支块允许以 throw 结尾`；正文 `if-match（match）表达式` 改为 `match 表达式`（第 7 行）；代码示例 `let label = if value {` 改为 `let label = match value {`（第 20 行）；规则与 TODO 项中 `\`if-match\` 表达式` 改为 `\`match\` 表达式`（第 29、117、134、143、144 行）；文件名同步由 `feng-if-expr-allow-throw-dev.md` 重命名为 `feng-if-and-match-expr-allow-throw-dev.md`（反映标题已覆盖 if 与 match 两类表达式） |
| `feng-exception-dev.md` | 第 5 行交付范围说明中 `\`if-match\` 类型收窄` 改为 `\`match\` 类型收窄` |
| `feng-expr-block-optimize-dev.md` | 第 234 行 smoke test 列表中 \`if_match_*\` 改为 \`match_*\`（与重命名后的 smoke 目录一致） |
| `feng-generics-delivered.md` | 第 71 行 A2 项中 `if-match aggregate result` 改为 `match aggregate result` |
| `feng-semantic-logic-c2s-draft.md` | CE0269、CE0270、CE1045、CE1046 描述中 `if-match` 改为 `match`；AE 挂载点 `if_match` 改为 `match`（第 222、223、738、739 行） |
| `feng-union-type-dev.md` | 全文 `if-match` 改为 `match`、`if 目标值 { ... }` 改为 `match 目标值 { ... }`、`\`if\` 收窄` 改为 `\`match\` 收窄`、`\`if\` 目标表达式` 改为 `\`match\` 目标表达式`、`union \`if\` 类型标签匹配` 改为 `union \`match\` 类型标签匹配`（第 9、13、15、17、27、76、78、80、81、109、186、227、233、246、247 行） |
| `feng-if-match-optimize-dev.md` | 不修改：该文档本身即 "if-match 优化" 的历史设计记录，标题与主题均为 if-match，保留作为历史记录 |

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
| `test/parser/test_parser.c` | 内嵌 Feng 源码字符串中 5 处 match 语句/表达式（第 125、1257、1296、1359、1395、1452 行） |
| `test/codegen/test_codegen.c` | 内嵌 Feng 源码字符串中 6 处 match 语句/表达式（第 4565、4613、4619、5706、5722、5730、5799、5860、5917 行） |
| `test/semantic/test_semantic.c` | 内嵌 Feng 源码字符串中多处 match 语句/表达式（含 union member 匹配、值匹配、区间匹配测试用例） |
| `test/lexer/test_lexer.c` | `feng_keyword_count() == 28U` 断言更新为 `29U`（第 46 行） |

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
| `std/src/async/Future.ff` | `return if self.owner { ... }` 改为 `return match self.owner { ... }`（第 32 行） |
| `std/src/json/Json.ff` | 12 处 `return if value { ... }` 改为 `return match value { ... }`（第 29、41、53、67、79、91、103、115、129、141、153、215 行） |
| `std/src/test/TestContext.ff` | 3 处 `if parentOutput/selfOutput { ... }` 改为 `match ... { ... }`（第 134、152、155 行） |

### 8.2 示例

| 文件 | 变更内容 |
| ---- | -------- |
| `examples/std_demo/src/debug.ff` | `let y = if message { ... }` 改为 `let y = match message { ... }` |

### 8.3 fcts 测试套件

| 文件 | 变更内容 |
| ---- | -------- |
| `fcts/fcts_bin/src/test_flow.ff` | 多处 match 语义 `if` 改为 `match`：值匹配（single value、value list、integer range、mixed range、string）的 `if age/code/score/val/word/word2 {` → `match ... {`；`flow_match_pick` 中 `return if tag {` → `return match tag {` |
| `fcts/fcts_bin/src/test_union.ff` | 多处 union member 匹配 `if` 改为 `match`：`print_display_union` 的 `return if value {`、`test_union` 各子用例中 `if v1/v2 {`、`if v3/v4/v5/v6 {`、`if default_choice {`、`if ok_val/err_val {` 等 |

## 9 实施步骤

按依赖顺序执行：

1. **文档变更**：更新 `feng-flow.md`、`feng-spec.md`、`feng-union-type.md`、`feng-exception.md`、错误码文档（含 `feng-error-codes-ae.md` 第 11 节标题）。
2. **Lexer**：在 `token.h` 中新增 `MATCH` 关键字。
3. **Parser**：
   - 新增 `parse_match_statement` / `parse_match_expression`（抽自原 `parse_if_statement` / `parse_if_expression` 的 match 分支）；
   - 简化 `parse_if_statement` / `parse_if_expression`，移除 `peek_match_body` 分支调用与 `FENG_STMT_MATCH` / `FENG_EXPR_MATCH` 创建逻辑；
   - 删除 `peek_match_body` 与 `peek_scan_type_ref_label` 两个失去调用方的前瞻函数；
   - 在 `parse_statement` / `parse_primary` / `token_starts_expression` 中新增 `FENG_TOKEN_KW_MATCH` 分发；
   - 更新 SE1103、SE1106（parser.c:4003）错误信息与 parser.c:3992 注释。
4. **Codegen**：更新 CE0269（2 处）、CE0270（1 处）错误信息中的 "if-match" 字样；CE0196 保留不动。
5. **Semantic**：更新 analyzer.c:6370 注释。
6. **编辑器**：更新 `feng.tmLanguage.json` 关键字列表。
7. **测试文件**：重命名 `if_match_*` → `match_*`，修改所有 match 语义测试文件中的 `if` → `match`。
8. **标准库与示例**：更新 `Option.ff` 文档注释和 `debug.ff` 示例。
9. **全量回归测试**：确保所有测试通过。

## 10 风险评估

| 风险 | 等级 | 缓解措施 |
| ---- | ---- | -------- |
| `peek_match_body` / `peek_scan_type_ref_label` 删除后是否有遗漏调用方 | 低 | 已通过 grep 确认两个函数当前仅被 `peek_match_body` 内部链路调用，删除后无任何调用方 |
| if 解析移除 match 分支后正确性 | 低 | AST 层面 `FENG_STMT_IF` / `FENG_EXPR_IF` 与 `FENG_STMT_MATCH` / `FENG_EXPR_MATCH` 早已独立，semantic/codegen/dump/export/lsp 的 case 分支完全分开，parser 入口分发调整后下游零影响 |
| `match` 关键字与用户代码标识符冲突 | 低 | Feng 已有关键字保护机制，`match` 作为新关键字的行为与其他关键字一致 |
| 测试文件重命名导致 CI 脚本路径失效 | 低 | 测试运行器按目录扫描，不依赖硬编码文件名 |
| 历史 dev 文档中 "if-match" 术语与现行语法不一致 | 低 | 除 `dev/feng-if-match-optimize-dev.md`（标题与主题即 if-match 优化，作为历史设计记录保留不动）外，其余 dev 文档已统一更新为 `match` 术语 |
| CE0196 错误信息保留 "if/match" 是否引起混淆 | 低 | 该错误由 if 与 match 共用的 `cg_emit_branch_into_slot` 路径触发，"if/match" 合并写法准确反映实际触发场景，保留比强行改为 "match" 更准确 |

## 建议 commit message

```text
feat: introduce dedicated match keyword for pattern matching

Replace the overloaded `if` keyword for pattern matching with a
dedicated `match` keyword. The `if` keyword now exclusively handles
boolean conditional branching, while `match` handles value matching,
range matching, and union member matching.

This change eliminates the need for the parser's peek_match_body()
lookahead disambiguation and improves code readability by making the
intent of each construct immediately clear.
```
