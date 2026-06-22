# match 运算（infix 形式）

## 状态: dev

> [docs/feng-flow.md](../docs/feng-flow.md) §3 是 `match 目标值 { ... }` 模式匹配的专项规范；
> 本文只写 `expr match pattern` infix 运算的开发步骤与 TODO，不重复规范定义。

## 1 问题背景

当前 `match` 仅支持 `match 目标值 { ... }` 块形式，分支标签写在 `{ ... }` 内。对于「只匹配单一模式并执行一段逻辑」的常见场景，块形式显得冗长：

- 单分支命中后执行一段逻辑：需写完整的 `match v { Type { ... } else { } }` 才能表达「v 是 Type 时执行 ...」。
- 单分支命中并绑定到变量：需写 `match v { x: Type { ... } else { } }`。
- 作为 `if` 条件：当前需先 match 到一个 bool 变量再 `if`，无法直接 `if v match Type { ... }`。

实际诉求是把单分支 match 表达为可嵌入条件、可参与逻辑组合的 bool 表达式，并支持在命中时绑定收窄后的值。

## 2 设计目标

- 引入 infix 运算 `expr match pattern`：返回 `bool`，表示 `expr` 是否匹配 `pattern`。
- `pattern` 复用 `match 目标值 { ... }` 既有标签形式：单值、值列表、整数闭区间、union member type、enum item 引用。
- 支持可选绑定：`expr match [let|var] name: Type` 仅对 union member type 模式有效，把收窄后的值绑定到 `name`。
- 默认绑定为 `let`，可显式 `let` 或 `var`。
- 可作为 `if` / `while` 条件，也可作为普通 bool 表达式参与 `&&` / `||` / `!` 与赋值。
- 不引入新的 runtime 原语；尽量复用既有 match 的语义与发码路径。
- 抽象驱动：pattern 复用既有 `FengMatchLabel`，绑定复用既有 `FengMatchBranch` 的 binding prefix，仅新增一个 AST 节点 `FENG_EXPR_MATCH_OP`。

## 3 语法规范（先文档）

按 CLAUDE.md「先规范、后代码、再测试」要求，先回写规范再动代码。

### 3.1 修改点（具体由人工审定后落到 [docs/feng-flow.md](../docs/feng-flow.md) §3）

- §3 新增「infix match 运算」子节，描述 `expr match pattern` 的语法、语义、pattern 形式、绑定作用域、优先级。
- §3.1 / §3.2 标签形式说明中追加：标签形式同时作为 infix 运算的 pattern 使用；infix 形式只接受单 label，不接受逗号分隔的多 label 列表。
- §3.2 union member 匹配规则中追加：infix 形式的 union member pattern 支持可选绑定 `[let|var] name: Type`，绑定作用域与 `if` / `while` 体绑定一致。
- 不在 `feng-flow.md` 中重复标签规则；规范收敛以 `feng-flow.md` 为单一信源。

### 3.2 语法形式

```feng
// 无绑定：返回 bool
let r = x match 0                // 值匹配（整型 / string / bool / enum item 引用）
let r = x match 1...10           // 整数闭区间匹配
let r = x match UserType         // union member 匹配

// 有绑定（仅 union member 模式）：返回 bool，同时把收窄后的值绑定到 name
let r = x match v: UserType       // 默认 let 绑定
let r = x match let v: UserType   // 显式 let 绑定
let r = x match var v: UserType   // 显式 var 绑定

// 用于 if 条件
if x match 0 { ... }
if x match 1...10 { ... }
if x match v: UserType { ... }       // v 在 if 体内可见
if x match var v: UserType { ... }   // v 在 if 体内可见且可变

// 用于 while 条件（每轮迭代重新求值并重新绑定）
while x match v: UserType { ... }

// 与逻辑运算符组合
if x match UserType && y match OtherType { ... }
```

### 3.3 pattern 形式

| pattern 形式 | 适用 target 类型 | 是否支持绑定 |
| ----------- | --------------- | ----------- |
| 单值字面量 `0` / `"abc"` / `true` / `EnumName.Item` | 整型 / string / bool / enum | 否 |
| 整数闭区间 `m...n` | 整型 | 否 |
| union member type `Type` | union-form spec | 是 |

- 值列表（逗号分隔多 label）不支持作为 infix pattern：逗号在表达式上下文中已用于函数调用参数、元组字面量等，混用会破坏既有语法；多值匹配请使用块形式 `match x { 0, 1, 2 { ... } }`。
- 链式 `x match a match b` 按左结合解析为 `(x match a) match b`，语义合法性由类型检查器捕获；详见 §3.6。

### 3.4 绑定作用域

绑定可见性由**短路语义**统一决定，与表达式所在位置（`if` / `while` 条件、`let r = ...` 右值、函数参数等）无关。规则：

- **表达式内可见性**：`name` 在「短路语义保证 match 已命中」的后续子表达式内可见。
  - `x match v: T && expr_rest`：`v` 在 `expr_rest` 与后续 `&&` 链内可见。
  - `x match v: T || expr_rest`：`v` 在 `expr_rest` 内**不可见**（`||` 短路：`expr_rest` 求值时 match 未命中）。
  - `!(x match v: T)`：`v` 在 `!` 内无后续子表达式，不涉及可见性。
- **外层体可见性**（仅 `if` / `while` 条件位置）：`name` 在外层 `if` / `while` 体内可见，当且仅当 match 命中是条件为真的必要条件。
  - `if x match v: T && expr_rest { ... }`：`v` 在 if 体内可见（`&&` 要求两侧为真）。
  - `if x match v: T || expr_rest { ... }`：`v` 在 if 体内**不可见**（可能由 `expr_rest` 为真进入，`v` 不保证已绑定）。
  - `if !(x match v: T) { ... }`：`v` 在 if 体内**不可见**（条件为真时 match 未命中）。
- **语句后不可见**：绑定作用域止于所在表达式；`if` / `while` / `let` 语句结束后 `name` 不再可见（与既有 match branch binding 仅在分支体内可见的行为一致）。
- 多重绑定：`x match v: T1 && y match w: T2 && v.foo + w.bar > 0 { ... }`，`v` 与 `w` 均在后续 `&&` 链与外层 `if` / `while` 体内可见。
- 合法形式示例：
  - `if x match v: UserType && v.age > 10 { ... }`         — `v` 在 `v.age > 10` 与 if 体内可见。
  - `let r = x match v: UserType && v.age > 10;`           — `v` 在 `v.age > 10` 内可见；语句后不可见。
  - `while x match var v: UserType && v.active { ... }`    — `v` 在 `v.active` 与 while 体内可见，每轮迭代重新绑定。
- 非法形式示例（语义层报「未绑定」错误）：
  - `let r = x match v: UserType || v.age > 10;`           — `v` 在 `v.age > 10` 内不可见。
  - `if x match v: UserType || v.age > 10 { ... }`        — `v` 在 `v.age > 10` 与 if 体内不可见。
  - `if !(x match v: UserType) { v.age }`                  — `v` 在 if 体内不可见。
- 实现侧需要语义层做「确定赋值」流分析：从 `FENG_EXPR_MATCH_OP` 节点的 binding 出发，沿 `&&` 链向下传播「已绑定」状态，遇到 `||` / `!` / 函数边界时停止传播；在可见范围内登记绑定变量，超范围使用报「未绑定」错误。

> **明确不支持**：`let r = x match v: Type;` 后，`v` 仅在所在表达式内可见，语句后不可用；即使在后续 `if r { ... }` 内也不可访问 `v`。参考 C# `is` 模式匹配：`bool r = x is UserType v;` 后 `if (r) { v.Method(); }` 同样编译报错「use of unassigned local variable 'v'」——C# 不跟踪 `r` 真值与 `v` 绑定状态的蕴含关系，本设计与此一致。

### 3.5 运算结果

- `expr match pattern` 的运算结果类型为 `bool`：命中 pattern 为 `true`，未命中为 `false`。
- 结果可作为 `if` / `while` 条件、参与逻辑运算（`&&` / `||` / `!`）、赋值给 `bool` 变量、或作为函数参数与返回值。

### 3.6 运算优先级与结合性

- `match` 优先级**低于算术运算**（乘 `/` 除 `%`、加 `-` 等），**与关系运算一致**（即 `<` / `<=` / `>` / `>=` / `==` / `!=` 同级）。
- 同级运算**左结合**，不强制加括号；同级相邻混用按从左到右顺序解析，语义合法性由类型检查器捕获：
  - `x match T == y`   → `(x match T) == y`   → 语义上 `bool == y`，若 `y` 非 `bool` 则类型检查报错。
  - `x match T < y`    → `(x match T) < y`    → 语义上 `bool < y`，类型检查报错。
  - `a < b match T`    → `(a < b) match T`    → 语义上对 `bool` 做 pattern 匹配，类型检查报错（除非 `T` 是 `true` / `false` 值 pattern）。
  - 如需改变顺序，使用括号：`(x match T) == y` 与 `x match (T == y)`（后者 `T == y` 不是合法 pattern，会报语法错误）。
- `match` 优先级**高于**逻辑 `&&` / `||`：`a match T && b` 解析为 `(a match T) && b`，`a match T || b match U` 解析为 `(a match T) || (b match U)`。
- `match` 优先级**高于**赋值 `=`：`let r = x match T` 解析为 `let r = (x match T)`。
- 链式 `x match a match b` 按左结合解析为 `(x match a) match b`：`x match a` 返回 `bool`，`bool` 是常量相等性匹配的合法目标类型（见 [docs/feng-flow.md](../docs/feng-flow.md) §3.1），因此 `(x match a) match true` / `match false` 语义合法（虽然通常冗余，等价于 `x match a` 或 `!(x match a)`）；若 `b` 是类型 pattern 或区间 pattern，则由类型检查器报错（target 类型不匹配）。parser 不对链式做特殊检查，统一走左结合路径。

## 4 实现方案（后代码）

按「parser → semantic → codegen」顺序落地；新增 AST 节点 `FENG_EXPR_MATCH_OP`，复用既有 `FengMatchLabel` 表示 pattern、`FengMatchBranch` 的 binding prefix 表示绑定。

### 4.1 AST

新增表达式节点 `FENG_EXPR_MATCH_OP`：

```c
typedef struct FengExprMatchOp {
    FengExpr *target;          /* LHS：被匹配的表达式 */
    FengMatchLabel *label;     /* RHS：单 label pattern（复用既有 AST） */
    bool has_binding;          /* 是否带绑定前缀（仅 union member 模式有效） */
    FengSlice binding_name;    /* 绑定变量名 */
    FengMutability binding_mutability;  /* let / var，默认 let */
} FengExprMatchOp;
```

- 复用 `FengMatchLabel` 表示 pattern，避免新增 label AST kind。
- 复用 `FengMatchBranch` 的 binding prefix 字段语义（`has_binding` / `binding_name` / `binding_mutability`）。
- 不引入新的 `FengMatchLabelKind`、不修改既有 `FENG_EXPR_MATCH`（块形式）AST。

### 4.2 Lexer

- `FENG_TOKEN_KW_MATCH` 既是 match 语句/表达式的起始关键字，也是 infix 运算符。
- 不新增 token；在 parser 层根据位置区分语义。

### 4.3 Parser

- 在 `parse_comparison` 与 `parse_shift` 之间新增一层 `parse_match_op`（即 `match` 优先级与关系运算同级、低于算术与移位），识别 `FENG_TOKEN_KW_MATCH` 作为 infix 运算符。
- 解析 pattern 时复用 `parse_match_label_atom`（单 label），不允许逗号分隔的多 label 列表。
- 解析可选 binding 时复用 `parse_match_branch_binding_prefix`（`[let|var] name :`），限制为单 label + 单 binding。
- 解析后构造 `FENG_EXPR_MATCH_OP` AST 节点。
- 链式 `x match a match b` 不做特殊检查，按左结合自然解析为 `(x match a) match b`，语义合法性由类型检查器捕获（与同级关系运算符相邻混用处理方式一致）。
- `parse_primary` 中 `FENG_TOKEN_KW_MATCH` 仍优先识别为 match 语句/表达式起始；infix 识别发生在二元运算层，位置不重叠。
- 不修改 `parse_match_label`、`parse_match_body`、`parse_match_branch` 等既有路径。

### 4.4 Semantic Analyzer

新增 `FENG_EXPR_MATCH_OP` 的 resolve 分支：

- 复用 `match_target_type_is_allowed` 校验 target 类型（整型 / string / bool / enum / union-form spec）。
- 复用 `extract_match_label_literal` 与 union member 解析路径校验 label 合法性。
- 对 union member pattern + binding，复用 `resolve_and_validate_union_match_common` 的单分支子集（限制为单 member、单 binding）。
- 对值 / 区间 / enum item 引用 pattern，禁止 binding（binding 仅对 union member type 有效）。
- 结果类型为 `bool`。
- **「确定赋值」流分析**：从 `FENG_EXPR_MATCH_OP` 节点的 binding 出发，沿 `&&` 链向下传播「已绑定」状态，遇到 `||` / `!` / 函数边界时停止；在可见范围内登记绑定变量，超范围使用报「未绑定」错误。可见范围包括：
  - `A && B` 中 `A` 含 `match v: T` 时：`B` 子树与外层 `if` / `while` 体。
  - `A || B` 中 `A` 含 `match v: T` 时：均不可见。
  - `!A` 中 `A` 含 `match v: T` 时：外层体不可见。
- 不引入新的 `MatchConstKind`、不修改既有 overlap 检测路径（单 pattern 无重叠概念）。

### 4.5 Codegen

新增 `FENG_EXPR_MATCH_OP` 的发码分支：

- 求值 target 到临时变量 `_mt`（与块形式 match 共用临时变量命名）。
- 复用 `cg_emit_match_label_cond` 的单 label 发码逻辑，发出判定布尔结果到结果 slot。
- 对 union member pattern + binding，在判定为 true 的分支中绑定 `name` 到收窄后的值（复用既有 union match 的绑定发码路径）。
- **`&&` 条件位置发码**：当 `FENG_EXPR_MATCH_OP` 作为 `&&` 左操作数时，在命中分支中初始化绑定变量，再求值右操作数；未命中则短路跳过右操作数与外层 `if` / `while` 体，绑定变量不在右操作数与体外可见。
- **`||` / `!` 条件位置发码**：绑定变量不在后续子表达式或外层体中可见（由 §4.4 流分析保证），无需初始化；按普通 bool 求值即可。
- 不引入新的 runtime 原语。

### 4.6 if / while 条件位置处理

- `if` / `while` 条件位置识别 `FENG_EXPR_MATCH_OP`：
  - 生成「计算 match → if true 则进入体」的代码结构。
  - 在体作用域中登记绑定变量。
- 复用既有 `if` 条件为表达式时的处理路径（已存在）；仅在条件为 `FENG_EXPR_MATCH_OP` 时追加 binding 作用域登记。
- `while` 每轮迭代重新求值条件并重新绑定。

### 4.7 dump / export / lsp / reifiable_deps

- `src/parser/dump.c`、`src/symbol/export.c`、`src/cli/lsp/runtime.c`、`src/semantic/reifiable_deps.c` 新增 `FENG_EXPR_MATCH_OP` case，按既有模式输出节点信息。
- 不引入新的 dump 格式、不修改 `.ft` 导出格式。

## 5 错误码与诊断

| 错误码 | 触发场景 | 现状 | 变更 |
| ------ | ------- | ---- | ---- |
| AE0050 | match 目标类型不在允许集合 | 已支持 | 复用，覆盖 infix 形式 |
| AE0404 | enum item 引用不存在 | 已支持 | 复用 |
| AE1105 | 标签非字面量或 const 绑定 | 已支持 | 复用 |
| AE11xx (新) | infix match pattern 出现值列表（多 label） | 不存在 | 新增 |
| AE11xx (新) | infix match 出现 binding 但 pattern 非 union member type | 不存在 | 新增 |
| AE11xx (新) | 绑定变量在不可见范围使用（逻辑或、逻辑非、函数边界外、语句后） | 不存在 | 新增 |

具体错误码编号、文案，先在 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md) 中确定后再落到代码。

## 6 影响范围分析

### 6.1 改动规模概述

- lexer：无修改。
- parser：新增 infix `match` 运算符识别与 pattern 解析，约 +120 行；新增 AST 节点构造与释放。
- semantic：新增 `FENG_EXPR_MATCH_OP` resolve 分支，约 +100 行。
- codegen：新增 `FENG_EXPR_MATCH_OP` 发码分支，约 +80 行；`if` / `while` 条件位置 binding 作用域处理约 +50 行。
- dump / export / lsp / reifiable_deps：新增 `FENG_EXPR_MATCH_OP` case，4 处共约 +30 行。
- 文档：`feng-flow.md` §3 新增 infix 运算子节；`feng-error-codes-ae.md` 新增错误码条目。
- 测试：parser / semantic / codegen / cts 新增覆盖。

### 6.2 各文件改动规模

| 文件 | 改动类型 | 规模 |
| ---- | ------- | ---- |
| `docs/feng-flow.md` | §3 新增 infix 运算子节 | +30 行 |
| `docs/feng-error-codes-ae.md` | 新增 AE11xx 条目 | +6 行 |
| `src/parser/parser.h` | 新增 `FengExprMatchOp` 与 `FENG_EXPR_MATCH_OP` | +15 行 |
| `src/parser/parser.c` | infix match 运算符识别 + pattern 解析 | +120 行 |
| `src/semantic/analyzer.c` | `FENG_EXPR_MATCH_OP` resolve 分支 | +100 行 |
| `src/codegen/codegen.c` | `FENG_EXPR_MATCH_OP` 发码分支 + if/while binding | +130 行 |
| `src/parser/dump.c` / `src/symbol/export.c` / `src/cli/lsp/runtime.c` / `src/semantic/reifiable_deps.c` | 新增 `FENG_EXPR_MATCH_OP` case | 4 处 +30 行 |

### 6.3 不动既有路径

- `match 目标值 { ... }` 语句与表达式路径不变。
- 既有标签形式（单值、值列表、区间、union member type、enum item 引用）语义不变。
- union member 匹配的收窄与绑定规则不变。
- enum match 路径不变。
- 既有 `match_*` 测试语义不变，只新增覆盖。

## 7 测试计划

### 7.1 parser 单元测试

在 `test/parser/test_parser.c` 中新增用例：

- `x match 0` / `x match 1...10` / `x match UserType` 解析为 `FENG_EXPR_MATCH_OP`。
- `x match v: UserType` / `x match let v: UserType` / `x match var v: UserType` 解析为带绑定的 `FENG_EXPR_MATCH_OP`。
- `if x match v: UserType { ... }` 解析为 if 语句，条件为 `FENG_EXPR_MATCH_OP`。
- `while x match v: UserType { ... }` 解析为 while 语句，条件为 `FENG_EXPR_MATCH_OP`。
- `x match UserType && y match OtherType` 解析为逻辑与，两操作数均为 `FENG_EXPR_MATCH_OP`。
- 非法形式：`x match 0, 1`（值列表）解析报错。
- 非法形式：`x match v: 0`（值模式 + 绑定）解析报错。
- 合法形式：`x match T == y` 解析为 `(x match T) == y`（左结合，语义合法性由类型检查器捕获）。
- 合法形式：`x match T < y` 解析为 `(x match T) < y`（左结合）。
- 合法形式：`a < b match T` 解析为 `(a < b) match T`（左结合）。
- 合法形式：`x match a match b` 解析为 `(x match a) match b`（左结合，`b` 为 `true` / `false` 时语义合法，否则由类型检查器报错）。

### 7.2 semantic 单元测试

在 `test/semantic/test_semantic.c` 中新增用例：

- 合法 infix match：值、区间、union member type、union member type + binding。
- target 类型不允许（如 `float`）报 AE0050。
- binding 用于非 union member type pattern 报 AE11xx。
- 绑定变量在不可见范围使用报 AE11xx（`||` / `!` / 函数边界外、语句后）。
- 链式 `x match a match b` 解析为 `(x match a) match b`；`b` 为 `true` / `false` 时合法（冗余），`b` 为类型 / 区间 pattern 时由类型检查器报 target 类型不匹配。
- 绑定变量作用域正确（if 体内可见，体外不可见）。
- 绑定变量在 if 体中类型为收窄后的 member 类型。
- 绑定变量在 while 体中每轮迭代重新绑定。
- **`let r = ...` 形式**：
  - `let r = x match v: T && v.foo > 0;` 合法，`v` 在 `v.foo > 0` 内可见，语句后不可见。
  - `let r = x match v: T || v.foo > 0;` 中 `v` 在 `v.foo > 0` 内**不可见**（报未绑定错误）。
  - `let r = x match v: T;` 后访问 `v` 报未绑定错误（语句后不可见）。
- **条件内可见性**：
  - `if x match v: T && v.foo > 0 { ... }` 中 `v` 在 `v.foo > 0` 与 if 体内可见。
  - `if x match v: T && v.foo > 0 && v.bar < 10 { ... }` 中 `v` 在后续所有 `&&` 子表达式与 if 体内可见。
  - `if x match v: T1 && y match w: T2 && v.foo + w.bar > 0 { ... }` 中 `v` 与 `w` 均在后续条件与 if 体内可见。
  - `if x match v: T || v.foo > 0 { ... }` 中 `v` 在 `v.foo > 0` 与 if 体内**不可见**（报未绑定错误）。
  - `if !(x match v: T) { ... }` 中 `v` 在 if 体内**不可见**（报未绑定错误）。
  - `if x match v: T || y { ... } else { v.foo }` 中 `v` 在 else 体内**不可见**。

### 7.3 codegen 单元测试

在 `test/codegen/test_codegen.c` 中新增用例：

- infix match 发码为「求值 target → 比较 → 返回 bool」。
- union member pattern + binding 发码为「求值 target → 比较 active member → 若命中则绑定」。
- `if` 条件位置 infix match 发码为「计算 → 跳转」结构。
- `while` 条件位置 infix match 每轮迭代重新求值与绑定。

### 7.4 cts / fcts 端到端

- 在 `fcts/fcts_bin/src/test_flow.ff` 中追加 infix match 端到端用例，覆盖值、区间、union member type、union member type + binding、`if` / `while` 条件形式。
- 既有 `match_*` 测试不受影响。

### 7.5 全量回归

- 阶段性执行 `make build/bin/test_parser && build/bin/test_parser`、`make build/bin/test_semantic && build/bin/test_semantic`、`make build/bin/test_codegen && build/bin/test_codegen`。
- 最终执行 `make test`。

## 8 分步任务清单

按依赖顺序执行；每步完成后必须执行全量回归 `make test` 全部通过，方可勾选完成并进入下一步。每步都设计为「独立可交付」切片。

### 8.1 步骤 1：规范与错误码文档变更

- 前置依赖：无
- 范围：仅文档变更，无代码改动

- [ ] 更新 [docs/feng-flow.md](../docs/feng-flow.md) §3：新增「infix match 运算」子节，描述语法、pattern 形式、绑定作用域、优先级；既有标签形式说明中追加 infix 用法引用
- [ ] 更新 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md)：新增 AE11xx 条目（值列表 pattern、binding 用于非 union member pattern、绑定变量在不可见范围使用）；错误码编号与文案最终口径由人工审定
- [ ] 全量回归点：`make test` 通过（仅文档变更，无代码行为变化）

### 8.2 步骤 2：AST 节点与 Parser 支持 infix match 运算符

- 前置依赖：步骤 1
- 范围：parser 新增 infix match 识别与 pattern 解析；AST 新增 `FENG_EXPR_MATCH_OP` 节点；dump 同步输出

- [ ] 在 `src/parser/parser.h` 新增 `FengExprMatchOp` 结构与 `FENG_EXPR_MATCH_OP` 枚举
- [ ] 在 `src/parser/parser.c` 新增 `parse_match_op` 层（位于 `parse_comparison` 与 `parse_shift` 之间，与关系运算同级、低于算术与移位），识别 `FENG_TOKEN_KW_MATCH` 作为 infix 运算符；链式与同级混用统一走左结合路径，不做出口特殊检查
- [ ] 复用 `parse_match_label_atom` 解析 pattern（限制为单 label）
- [ ] 复用 `parse_match_branch_binding_prefix` 解析可选 binding
- [ ] 构造 `FENG_EXPR_MATCH_OP` AST 节点；同步 `free_expr` / `copy_expr` 路径
- [ ] 在 `src/parser/dump.c` 新增 `FENG_EXPR_MATCH_OP` case
- [ ] 在 `test/parser/test_parser.c` 新增用例
- [ ] 全量回归点：`make build/bin/test_parser && build/bin/test_parser` 通过；`make test` 全量回归通过

### 8.3 步骤 3：Semantic 扩展支持 infix match

- 前置依赖：步骤 2
- 范围：semantic 新增 `FENG_EXPR_MATCH_OP` resolve 分支；绑定变量作用域登记
- 行为变化：原 `x match int` 形式无既有语义（`match` 关键字此前不在 infix 位置出现），无既有测试回归

- [ ] 复用 `match_target_type_is_allowed` 校验 target
- [ ] 复用既有 label 解析路径校验 pattern
- [ ] 对 union member pattern + binding，复用 union match 单分支子集
- [ ] 对值 / 区间 / enum item 引用 pattern，禁止 binding
- [ ] 结果类型为 bool
- [ ] 绑定变量在 `if` / `while` 体作用域内登记
- [ ] 在 `src/semantic/reifiable_deps.c` 新增 `FENG_EXPR_MATCH_OP` case
- [ ] 在 `test/semantic/test_semantic.c` 新增用例
- [ ] 全量回归点：`make build/bin/test_semantic && build/bin/test_semantic` 通过；`make test` 全量回归通过

### 8.4 步骤 4：Codegen 发码扩展

- 前置依赖：步骤 3
- 范围：codegen 新增 `FENG_EXPR_MATCH_OP` 发码分支；`if` / `while` 条件位置 binding 作用域处理

- [ ] 求值 target 到临时变量
- [ ] 复用 `cg_emit_match_label_cond` 单 label 发码
- [ ] union member pattern + binding 在命中分支中绑定
- [ ] `if` / `while` 条件位置识别并处理 binding 作用域
- [ ] `while` 每轮迭代重新求值与绑定
- [ ] 在 `test/codegen/test_codegen.c` 新增用例
- [ ] 全量回归点：`make build/bin/test_codegen && build/bin/test_codegen` 通过；`make test` 全量回归通过

### 8.5 步骤 5：Symbol Export / LSP 支持

- 前置依赖：步骤 4
- 范围：`symbol/export.c` 与 `cli/lsp/runtime.c` 新增 `FENG_EXPR_MATCH_OP` case

- [ ] 在 `src/symbol/export.c` 新增 `FENG_EXPR_MATCH_OP` case
- [ ] 在 `src/cli/lsp/runtime.c` 新增 `FENG_EXPR_MATCH_OP` case（completion / hover / definition 行为复用既有 match 表达式路径）
- [ ] 全量回归点：`make test` 全量回归通过

### 8.6 步骤 6：cts / fcts 端到端用例

- 前置依赖：步骤 5
- 范围：端到端测试套件追加 infix match 用例

- [ ] 在 `fcts/fcts_bin/src/test_flow.ff` 中追加 infix match 端到端用例：覆盖值、区间、union member type、union member type + binding、`if` / `while` 条件形式
- [ ] 全量回归点：fcts 套件通过；`make test` 全量回归通过

### 8.7 步骤 7：最终全量回归与收尾

- 前置依赖：步骤 1–6 全部完成
- 范围：最终验收与一致性复核

- [ ] 执行 `make test` 全量回归通过
- [ ] 复核所有新增测试用例与既有测试用例无冲突
- [ ] 复核 `docs/feng-flow.md` / `docs/feng-error-codes-*.md` 与代码实现一致
- [ ] 复核 `dev/feng-match-operator-dev.md` 中实现方案与最终代码一致（行号、函数名、错误码编号如有调整需回写文档）
- [ ] 准备建议 commit message，由开发者自行提交

## 9 风险评估

| 风险 | 等级 | 缓解措施 |
| ---- | ---- | -------- |
| `match` 关键字作为 infix 运算符与既有 match 语句/表达式起始关键字冲突 | 中 | parser 在 `parse_primary` 中优先识别 match 语句/表达式起始；infix 识别发生在二元运算层，位置不重叠 |
| 值列表 pattern 与逗号表达式歧义 | 中 | 默认禁止值列表作为 infix pattern，仅允许单值/单区间/单 type；多值匹配请使用块形式 |
| 绑定可见性流分析实现复杂度 | 中 | 由短路语义统一决定：沿逻辑与链传播「已绑定」状态，遇逻辑或、逻辑非、函数边界停止；`if` / `while` 体可见性要求 match 命中是条件为真的必要条件；语句后不可见。复用既有 match branch binding 的作用域登记路径 |
| `match` 运算符优先级与既有运算符交互 | 中 | 与关系运算同级、低于算术；同级相邻混用与链式均走既有左结合路径，语义合法性由类型检查器捕获，parser 不做出口特殊检查 |
| `if` / `while` 条件位置的 binding 作用域处理与既有 match 表达式分支 binding 不一致 | 低 | 复用既有 union match binding 的作用域处理路径 |
| 既有 `match_*` 测试受影响 | 低 | 仅新增 infix 形式，既有块形式不变 |
| `match` 关键字与用户代码标识符冲突 | 低 | `match` 已是关键字，行为与其他关键字一致；既有代码已不能以 `match` 为标识符 |

## 10 当前明确不做

- 不支持值列表（逗号分隔多 label）作为 infix pattern。
- 不引入新的 runtime 原语或值模型分类。
- 不修改既有 `match 目标值 { ... }` 块形式的语义与发码。
- 不引入新的 `FengMatchLabelKind`。
- 不修改既有标签形式（单值、值列表、区间、union member type、enum item 引用）的语义。

## 11 交付约束

- 所有实现必须以 [docs/feng-flow.md](../docs/feng-flow.md) §3 与本文档为准，不得在编码阶段临时放宽或收紧。
- 错误码编号与文案必须先在 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md) 中确定，再落到代码。
- 每个阶段都以「先文档、后代码、再测试」的顺序落地，不修改既有测试语义，只新增覆盖。
- 若实现过程中发现规范仍有缺口，先回写对应权威文档，再继续编码。
- 所有不确定的语义（值列表 pattern、运算符优先级）均由人工决策后才能继续。
- 不得引入新的 runtime 对象表示、runtime API 或额外值模型分类。
- 不得边写代码边临时决定错误码编号或文案。

## 建议 commit message

```text
feat(match): add infix match operator for single-pattern testing

Introduce `expr match pattern` as an infix operator that returns a
bool indicating whether `expr` matches `pattern`. The pattern reuses
the existing match label forms: single value, integer range, and
union member type. Union member patterns support optional binding
`[let|var] name: Type`, with the bound variable visible inside the
`if`/`while` body when used as a condition.

The change adds a new AST node `FENG_EXPR_MATCH_OP` and reuses the
existing label parsing, semantic resolution, and codegen paths for
match labels. No new runtime primitive is introduced.
```
