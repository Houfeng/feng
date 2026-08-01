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
- `pattern` 复用 `match 目标值 { ... }` 既有标签形式：单值、整数闭区间、union member type、enum item 引用；多 label 用 `|` 分隔（如 `x match 0 | 1 | 2`、`x match Type1 | Type2`），区别于块形式的 `,`，见 §3.3。
- 支持可选绑定：`expr match [let|var] name: Type` 仅对 union member type 模式有效，把收窄后的值绑定到 `name`。
- 默认绑定为 `let`，可显式 `let` 或 `var`。
- 可作为 `if` / `while` 条件，也可作为普通 bool 表达式参与 `&&` / `||` / `!` 与赋值。
- 不引入新的 runtime 原语；尽量复用既有 match 的语义与发码路径。
- 抽象驱动：pattern 复用既有 `FengMatchLabel`，绑定复用既有 `FengMatchBranch` 的 binding prefix，仅新增一个 AST 节点 `FENG_EXPR_MATCH_OP`。

## 3 语法规范（先文档）

按 CLAUDE.md「先规范、后代码、再测试」要求，先回写规范再动代码。

### 3.1 修改点（具体由人工审定后落到 [docs/feng-flow.md](../docs/feng-flow.md) §3）

- §3 新增「infix match 运算」子节，描述 `expr match pattern` 的语法、语义、pattern 形式、绑定作用域、优先级。
- §3.1 / §3.2 标签形式说明中追加：标签形式同时作为 infix 运算的 pattern 使用；infix 形式的多 label 用 `|` 分隔（区别于块形式的 `,`）。
- §3.2 union member 匹配规则中追加：infix 形式的 union member pattern 支持可选绑定 `[let|var] name: Type`，绑定作用域与 `if` / `while` 体绑定一致。
- 不在 `feng-flow.md` 中重复标签规则；规范收敛以 `feng-flow.md` 为单一信源。

### 3.2 语法形式

```feng
// 无绑定：返回 bool
let r = x match 0                // 值匹配（整型 / string / bool / enum item 引用）
let r = x match 1...10           // 整数闭区间匹配
let r = x match UserType         // union member 匹配
let r = x match 0 | 1 | 2        // 值列表匹配（| 分隔，区别于块形式的 ,）
let r = x match 0 | 1...10 | 100 // 值与区间混合
let r = x match Type1 | Type2    // 多 member type 匹配

// 有绑定（仅 union member 模式）：返回 bool，同时把收窄后的值绑定到 name
let r = x match v: UserType       // 默认 let 绑定
let r = x match let v: UserType   // 显式 let 绑定
let r = x match var v: UserType   // 显式 var 绑定
let r = x match v: Type1 | Type2 // 多 member 绑定，v 收窄为 Type1 | Type2 子集

// 用于 if 条件
if x match 0 { ... }
if x match 1...10 { ... }
if x match v: UserType { ... }       // v 在 if 体内可见
if x match var v: UserType { ... }   // v 在 if 体内可见且可变
if x match 0 | 1 | 2 { ... }         // 值列表作为 if 条件

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
| 多 label `label1 \| label2 \| ...`（值、区间、type 可混合） | 各 label 须与 target 类型兼容 | 仅当全部为 type 时支持绑定 |

- 多 label 用 `|` 分隔（区别于块形式的 `,`）：`x match 0 | 1 | 2`、`x match 0 | 1...10 | 100`、`x match Type1 | Type2`。
- `|` 在 pattern 位置作为 label 分隔符，**不是按位或运算符**；parser 在 pattern 解析循环内消费 `|`，退出循环后剩余 `|` 才按表达式按位或处理。`bool | int` 本身非法（AE0030），故 `(x match 0) | 1` 这种「match 先算再按位或」的解读天然无意义，`|` 在 pattern 位置吃掉所有连续 label 是唯一有意义的解析。
- 不用 `,` 作为 infix 多 label 分隔符：`,` 在函数参数、元组、数组等容器上下文已是分隔符，混用会歧义（见 §7.1 测试用例）；块形式在 `{ ... }` 内无此歧义，继续用 `,`。
- 链式 `x match a match b` 按左结合解析为 `(x match a) match b`，语义合法性由类型检查器捕获；详见 §3.6。

### 3.4 绑定作用域

绑定变量 `name` 的可见性由一条根本原则决定：**从条件根递归向下收集，`&&` 收集两侧，`||` / `!` 子树内全部丢弃，分组透明穿透**。match 表达式为 `true` ⟺ 命中并完成绑定；为 `false` ⟺ 未命中、未绑定。短路语义（`&&` / `||` / `!`）是推导 match 表达式在各位置真值的工具，而非独立的可见性规则。

**规则表述**（用户视角）：

> 从条件根开始递归：`&&` 收集两侧可见的 match 变量；`||` 或 `!` 子树内所有 match 变量不可见；分组 `(A)` 透明穿透到 A；`match v: T` 收集 v；其他运算符（`==`/`!=`/`^`/`?:`/位运算/函数调用/类型转换等）不传播，返回空。body 内可见的 match 变量 = 整个条件的收集结果。

可见性分析是 **AST 级静态推导**：编译器从条件根递归向下遍历，按节点类型收集可见的 match 变量；不需要运行时真值跟踪、不需要跨语句流分析、不需要跨函数边界。这也是「语句后不可见」的根本依据：一旦离开 match 表达式所在语句，编译器无法静态保证 match 表达式真值与任何变量（如 `let r = ...` 中的 `r`）的关联，因此 `name` 不可见。

**判定规则**（递归收集）：

- **`A && B`**：递归收集 A 和 B 的可见 match 变量，合并。B 求值时可见 A 的 match 变量（`&&` 短路保证 A 为 true）。
- **`A || B`**：返回空集合。`||` 短路：B 求值时 A 为 false，A 中的 match 变量未绑定；`||` 为 true 也不保证 A 或 B 任一为 true，body 内 match 变量也不可见。
- **`!A`**：返回空集合。`!A` 为 true ⟹ A 为 false ⟹ A 中的 match 变量未绑定。
- **`(A)`**：递归收集 A，分组透明穿透。
- **`match v: T`**：收集 v（若 has_binding）。
- **其他叶子节点**（比较、字面量、函数调用等）：返回空。

**关键洞察**：`||` 或 `!` 只在其子树内丢弃 match 变量。如果 `||` 或 `!` 是 match 的**侄子**（位于兄弟子树内），不影响 match 的可见性——因为它在 `&&` 短路保证 match 为 true 之后才求值。例如 `match v: T && (a || b)` 中，`(a || b)` 是 match 的侄子，`||` 只丢弃 `(a || b)` 内自身的 match 变量，不丢弃 v。

**具体场景**：

- **`match v: T && B`**：v 在 B 与后续 `&&` 链内可见。B 内若含 `||` 或 `!`，只丢弃 B 内自身的 match 变量，不影响 v。
- **`match v: T || B`**：v 在 B 内不可见，在 body 内不可见（`||` 子树全部丢弃）。B 内自身的 match 变量按 B 内部结构判定（但整个 `||` 已返回空，body 内也不可见）。
- **`!(match v: T)`**：v 在 `!` 外（包括 if/while body）不可见（`!` 子树全部丢弃）。
- **`match v: T && (a || b)`**：v 在 `(a || b)` 整体及 `a`、`b` 内均可见（`(a || b)` 是 v 的侄子；`&&` 短路保证 v 已绑定）。
- **`match v: T && !(cond)`**：v 在 `!(cond)` 整体内可见（`!` 是 v 的侄子，只丢弃其子树内的 match 变量，不丢弃 v）。
- **`(match v: T && a) || b`**：v 在 `a` 内可见（分组内 `&&` 收集到 v），v 在 `b` 与 if/while body 内不可见（顶层 `||` 整体返回空）。
- **`(match v: T || a) && b`**：v 在 `a` 与 `b`、if/while body 内均不可见（分组内 `||` 丢弃 v）。
- **`if (A) { } else { else_body }`**：else_body 内所有 match 绑定变量一律不可见（不做 when-false 反向推导，简化规则）。
- **`else if (cond) { body }`**：等价于 `else { if (cond) { body } }`，新 if 按新条件 cond 自行判定可见性。
- **语句后不可见**：`if` / `while` / `let` 语句结束后 `name` 不再可见。`let r = x match v: T;` 后即使写 `if r { use(v); }` 也不允许：`r` 是独立 bool 变量，编译器不跟踪 `r` 真值与 `v` 绑定状态的关联。
- **函数边界外不可见**：match 表达式作为函数参数（`f(x match v: T)`）时，`v` 仅在 match 表达式内可见，不跨函数体传播。
- **while 每轮重新绑定**：`while (x match v: T) { body }` 每轮迭代重新求值条件并重新绑定；`v` 在 body 内可见，但上一轮的 `v` 与本轮无关。

**只识别 `&&` / `||` / `!` 三种逻辑运算符**：可见性分析只对 `&&`、`||`、`!` 递归判定，其他运算符（`==`、`!=`、`^`、`?:`、位运算、函数调用、类型转换等）一律保守处理，不传播 match 表达式的真值约束。`!` 不做递归等价变换：`!!(match v: T)` 中 v 不可见（`!` 子树返回空），用户应把 `!!cond` 改写为正向形式 `cond`。

**嵌套场景示例**：`if !(match v: T && (match w: U || other)) { body }`

```
!
└── &&
    ├── match v: T
    └── ( )
        └── ||
            ├── match w: U
            └── other
```

从根递归收集：
- 根是 `!`：返回空集合。**v 和 w 在 body 内都不可见**。
- 但在 `!` 内部 `&&` 求值时，`&&` 短路保证 `match v: T` 为 true，所以 v 在 `(match w: U || other)` 整体内可见（表达式内可见性，非 body 可见性）。
- `(match w: U || other)` 内部是 `||`：返回空。w 在 `other` 内不可见。

对 v（match v: T 的绑定变量）：
- v 在 `(match w: U || other)` 整体内可见（`&&` 短路保证 v 已绑定）
- v 在 `match w: U` 与 `other` 内可见（同上）
- v 在 body 内不可见（`!` 根返回空）

对 w（match w: U 的绑定变量）：
- w 在 `other` 内不可见（`||` 返回空）
- w 在 body 内不可见（`!` 根返回空）

**场景判定表**：

| 条件形式 | v 在兄弟子树内 | v 在 body 内 | 理由 |
| --- | --- | --- | --- |
| `match v: T` | — | ✓ | 根是 match，收集 v |
| `match v: T && cond` | ✓ | ✓ | `&&` 合并 {v} + {} |
| `match v: T && (a \|\| b)` | ✓ | ✓ | `&&` 合并 {v} + `(a\|\|b)`的空 = {v} |
| `match v: T && !(cond)` | ✓ | ✓ | `&&` 合并 {v} + `!(cond)`的空 = {v} |
| `match v: T \|\| cond` | ✗ | ✗ | `\|\|` 返回空 |
| `!(match v: T)` | — | ✗ | `!` 返回空 |
| `!!(match v: T)` | — | ✗ | 外层 `!` 返回空（不做 `!!` 等价变换） |
| `(match v: T && a) \|\| b` | ✓（在 `a` 内） | ✗ | 顶层 `\|\|` 返回空；但 `a` 在分组内 `&&` 求值时可见 v |
| `(match v: T \|\| a) && b` | ✗ | ✗ | 分组内 `\|\|` 返回空，`&&` 合并空 + 空 |
| `(match v: T) && cond` | ✓ | ✓ | 分组透明，`&&` 合并 {v} + {} |
| `match v: T && (match w: U \|\| other)` | v 在 `(match w: U \|\| other)` 内 ✓ | ✓ | `&&` 合并 {v} + 空 = {v}；w 被 `\|\|` 丢弃 |

**多重绑定**：`if x match v: T1 && y match w: T2 && v.foo + w.bar > 0 { ... }`，`v` 与 `w` 均在后续 `&&` 链与 if 体内可见。

**合法形式示例**：
- `if x match v: UserType && v.age > 10 { ... }`         — `v` 在 `v.age > 10` 与 if 体内可见。
- `let r = x match v: UserType && v.age > 10;`           — `v` 在 `v.age > 10` 内可见；语句后不可见。
- `while x match var v: UserType && v.active { ... }`    — `v` 在 `v.active` 与 while 体内可见，每轮迭代重新绑定。
- `if x match v: UserType && (cond || other) { ... }`   — `v` 在 `(cond || other)` 与 if 体内可见（`||` 是侄子，只丢弃其内部 match 变量）。

**非法形式示例**（语义层报「未绑定」错误）：
- `let r = x match v: UserType || v.age > 10;`           — `v` 在 `v.age > 10` 内不可见（`||` 返回空）。
- `if x match v: UserType || v.age > 10 { ... }`        — `v` 在 `v.age > 10` 与 if 体内不可见（`||` 返回空）。
- `if !(x match v: UserType) { v.age }`                  — `v` 在 if 体内不可见（`!` 返回空）。
- `if !!(x match v: UserType) { v.age }`                 — `v` 在 if 体内不可见（外层 `!` 返回空，不做 `!!` 等价变换；应改写为 `if x match v: UserType { v.age }`）。
- `if x match v: UserType { } else { v.age }`            — `v` 在 else 体内不可见（else 一律不可见，不做反向推导）。
- `let r = x match v: UserType; if r { v.age }`          — `v` 在 `if r { ... }` 内不可见（语句后不可见，编译器不跟踪 `r` 真值与 `v` 绑定状态关联）。
- `if (x match v: UserType == true) { v.age }`           — `v` 不可见（`==` 不传播真值约束，保守处理）。

> **与 C# `is` 模式匹配的差异**：本设计是 C# 的保守简化版。差异点：(1) `!!(match v: T)` 中 v 不可见（C# 通过 `!` 递归交换两态让 v 可见）；(2) else body 内 v 一律不可见（C# 在 `if (!cond) { } else { use(v); }` 中让 v 可见）；(3) 不识别 `==`/`!=`/`^`/`?:` 等运算符的语义等价（与 C# 一致）。这些差异都遵循「宁可误阻止也不误允许」原则，用户应把 match 表达式直接写在条件正向位置（`if x match v: T { ... }`），这也是 infix match 运算的设计意图。

### 3.5 运算结果

- `expr match pattern` 的运算结果类型为 `bool`：命中 pattern 为 `true`，未命中为 `false`。
- 结果可作为 `if` / `while` 条件、参与逻辑运算（`&&` / `||` / `!`）、赋值给 `bool` 变量、或作为函数参数与返回值。

### 3.6 运算优先级与结合性

- `match` 优先级**低于算术运算**（乘 `/` 除 `%`、加 `-` 等），**与关系运算一致**（即与 `<` / `<=` / `>` / `>=` 同级），**高于相等运算**（`==` / `!=`）。参考 C# `is` 运算符归类为 relational and type-testing，与本设计一致。
- 同级运算（match 与 `<` / `<=` / `>` / `>=`）**左结合**，不强制加括号；同级相邻混用按从左到右顺序解析，语义合法性由类型检查器捕获：
  - `x match T < y`    → `(x match T) < y`    → 语义上 `bool < y`，类型检查报错。
  - `a < b match T`    → `(a < b) match T`    → 语义上对 `bool` 做 pattern 匹配，类型检查报错（除非 `T` 是 `true` / `false` 值 pattern）。
- match **高于** equality（`==` / `!=`）：
  - `x match T == y`   → `(x match T) == y`   → 语义上 `bool == y`，若 `y` 非 `bool` 则类型检查报错。
- 如需改变顺序，使用括号：`(x match T) == y` 与 `x match (T == y)`（后者 `T == y` 不是合法 pattern，会报语法错误）。
- `match` 优先级**高于**逻辑 `&&` / `||`：`a match T && b` 解析为 `(a match T) && b`，`a match T || b match U` 解析为 `(a match T) || (b match U)`。
- `match` 优先级**高于**赋值 `=`：`let r = x match T` 解析为 `let r = (x match T)`。
- 链式 `x match a match b` 按左结合解析为 `(x match a) match b`：`x match a` 返回 `bool`，`bool` 是常量相等性匹配的合法目标类型（见 [docs/feng-flow.md](../docs/feng-flow.md) §3.1），因此 `(x match a) match true` / `match false` 语义合法（虽然通常冗余，等价于 `x match a` 或 `!(x match a)`）；若 `b` 是类型 pattern 或区间 pattern，则由类型检查器报错（target 类型不匹配）。parser 不对链式做特殊检查，统一走左结合路径。
- **`\|` 在 pattern 位置**：`x match 0 | 1 | 2` 中 `|` 是 label 分隔符（见 §3.3），不参与表达式优先级比较；parser 在 pattern 解析循环内消费所有连续 `|`-separated label，构造 label 数组。退出 pattern 解析后，剩余 `|` 按按位或运算符处理（`parse_bit_or` 层，低于 equality）。由于 `bool | int` 非法（AE0030），`(x match 0) | 1` 这种写法无意义，`|` 在 pattern 位置的「吃尽」行为不会产生歧义。

## 4 实现方案（后代码）

按「parser → semantic → codegen」顺序落地；新增 AST 节点 `FENG_EXPR_MATCH_OP`，复用既有 `FengMatchLabel` 表示 pattern、`FengMatchBranch` 的 binding prefix 表示绑定。

### 4.1 AST

新增表达式节点 `FENG_EXPR_MATCH_OP`：

```c
typedef struct FengExprMatchOp {
    FengExpr *target;          /* LHS：被匹配的表达式 */
    FengMatchLabel *labels;    /* RHS：label 数组（单 label 或 | 分隔的多 label，复用既有 AST） */
    size_t label_count;        /* label 数量，>= 1 */
    bool has_binding;          /* 是否带绑定前缀（仅 union member 模式有效） */
    FengSlice binding_name;    /* 绑定变量名 */
    FengMutability binding_mutability;  /* let / var，默认 let */
} FengExprMatchOp;
```

- 复用 `FengMatchLabel` 表示 pattern，避免新增 label AST kind。
- `labels` 为数组，与块形式 `FengMatchBranch.labels`（parser.h:150）结构一致；单 label 时 `label_count == 1`，多 label 时 `label_count > 1`。
- 复用 `FengMatchBranch` 的 binding prefix 字段语义（`has_binding` / `binding_name` / `binding_mutability`）。
- 不引入新的 `FengMatchLabelKind`、不修改既有 `FENG_EXPR_MATCH`（块形式）AST。

### 4.2 Lexer

- `FENG_TOKEN_KW_MATCH` 既是 match 语句/表达式的起始关键字，也是 infix 运算符。
- 不新增 token；在 parser 层根据位置区分语义。

### 4.3 Parser

- 在 `parse_comparison` 内识别 `FENG_TOKEN_KW_MATCH` 作为同级 infix 运算符（与 `<` / `<=` / `>` / `>=` 共用同一左结合循环，不新增独立 parser 层）；match 优先级与关系运算同级、高于 equality、低于算术与移位。
- 解析顺序：先调用 `parse_match_branch_binding_prefix`（`[let|var] name :` 或裸 `name :`）解析可选 binding 前缀，再调用 `parse_match_label_atom` 解析首个 label；若下一个 token 是 `FENG_TOKEN_PIPE`，循环消费 `|` 并解析后续 label，构造 `FengMatchLabel` 数组（与块形式 `FengMatchBranch.labels` 结构一致）。
- `|` 在 pattern 解析循环内作为 label 分隔符消费，**不作为按位或运算符**；退出循环后剩余 `|` 才交给上层 `parse_bit_or` 按按位或处理。由于 `bool | int` 非法，此「吃尽」行为无歧义。
- binding 与 label 的限制：binding 至多一个；label 数量不限（`label_count >= 1`）；binding 仅在全部 label 为 type pattern 时有效，否则语义层报错（见 §4.4）。
- 链式 `x match a match b` 不做特殊检查，按左结合自然解析为 `(x match a) match b`，语义合法性由类型检查器捕获（与同级关系运算符相邻混用处理方式一致）。
- `parse_primary` 中 `FENG_TOKEN_KW_MATCH` 仍优先识别为 match 语句/表达式起始；infix 识别发生在二元运算层，位置不重叠。
- 不修改 `parse_match_label`、`parse_match_body`、`parse_match_branch` 等既有路径。

### 4.4 Semantic Analyzer

新增 `FENG_EXPR_MATCH_OP` 的 resolve 分支：

- 复用 `match_target_type_is_allowed` 校验 target 类型（整型 / string / bool / enum / union-form spec）。
- 复用 `extract_match_label_literal` 与 union member 解析路径校验每个 label 合法性；遍历 `labels` 数组逐个校验，逻辑与块形式 `FengMatchBranch.labels` 一致。
- 复用块形式的 overlap 检测路径（块形式已对 branch 内多 label 做交叉检测，infix 的 label 数组结构相同，可直接套用）。
- 对 union member pattern + binding，复用 `resolve_and_validate_union_match_common` 的单分支逻辑（支持单 member 与多 member 子集）；binding 仅在全部 label 为 type pattern 时有效，否则报 AE1009。
- 对值 / 区间 / enum item 引用 pattern，禁止 binding（binding 仅对 union member type 有效）。
- 结果类型为 `bool`。
- **可见性分析（从根向下递归收集）**：实现为单一递归函数 `collect_visible_match_bindings`，从条件根递归向下遍历：

```c
BindingSet collect_visible_match_bindings(FengExpr *expr) {
    switch (expr->kind) {
    case FENG_EXPR_AND:  /* && 合并两侧 */
        return merge(collect_visible_match_bindings(expr->binary.left),
                     collect_visible_match_bindings(expr->binary.right));
    case FENG_EXPR_OR:   /* || 子树全部丢弃 */
    case FENG_EXPR_NOT:  /* ! 子树全部丢弃 */
        return EMPTY_SET;
    case FENG_EXPR_GROUP:  /* 分组透明穿透 */
        return collect_visible_match_bindings(expr->group.inner);
    case FENG_EXPR_MATCH_OP:  /* match v: T 收集 v */
        return expr->match_op.has_binding
            ? single_set(expr->match_op.binding_name)
            : EMPTY_SET;
    default:  /* 其他运算符（==/!=/^/?:/位运算/函数调用/类型转换等）不传播 */
        return EMPTY_SET;
    }
}
```

  - **body 可见性**：`collect_visible_match_bindings(条件根)` 的结果集合即 body 内可见的 match 绑定变量，登记到 `if` / `while` 体作用域。
  - **`&&` 链右侧可见性**：在 `A && B` 中，B 求值时可见 A 的 match 变量。实现上分两步：先递归收集整个条件的可见 match 变量（用于 body），再递归传播 `&&` 左侧的收集结果到右侧作用域（用于表达式内可见性）。
  - **else body**：不做反向推导，所有 match 绑定变量一律不可见。
  - **语句后不可见**：`let r = x match v: T;` 后 `v` 不可见（不跟踪 `r` 真值与 `v` 绑定状态关联）。
  - **函数边界外不可见**：`f(x match v: T)` 中 `v` 仅在 match 表达式内可见。
- 只识别 `&&` / `||` / `!` 三种逻辑运算符；其他运算符（`==` / `!=` / `^` / `?:` / 位运算 / 函数调用 / 类型转换等）不传播真值约束，binding 变量不可见。
- `!` 不做递归等价变换：`!!(match v: T)` 中 `v` 不可见。
- 超范围使用 binding 变量时，标识符查找失败，复用 AE0001（undefined identifier），不引入新的可见性专用错误码；与 C# pattern 变量在语句外不可见、使用时报 CS0103 的策略一致。
- 不引入新的 `MatchConstKind`、不维护 when-true / when-false 两态集合、不做跨语句流分析。

### 4.5 Codegen

新增 `FENG_EXPR_MATCH_OP` 的发码分支：

- 求值 target 到临时变量 `_mt`（与块形式 match 共用临时变量命名）。
- 遍历 `labels` 数组，复用 `cg_emit_match_label_cond` 的单 label 发码逻辑逐个发出判定；任一 label 命中即结果为 `true`，全部未命中则为 `false`（与块形式 branch 内多 label 的「或」语义一致）。
- 对 union member pattern + binding，在判定为 true 的分支中绑定 `name` 到收窄后的值（复用既有 union match 的绑定发码路径）；多 member 子集绑定的发码与块形式一致。
- **可见范围内的发码**：当 `FENG_EXPR_MATCH_OP` 的 binding 变量在 `&&` 右操作数或 `if` / `while` body 内可见时（由 §4.4 `collect_visible_match_bindings` 判定），在命中分支中初始化绑定变量，再求值右操作数或进入体；未命中则短路跳过。
- **不可见位置的发码**：`||` / `!` 子树内（收集返回空）的位置，binding 变量不初始化（语义层已保证不可见，无需发码绑定）；按普通 bool 求值即可。
- **else body 发码**：else body 内 binding 变量不可见，无需初始化。
- 不引入新的 runtime 原语。

### 4.6 if / while 条件位置处理

- `if` / `while` 条件位置识别 `FENG_EXPR_MATCH_OP`：
  - 生成「计算条件 → if true 则进入体」的代码结构。
  - 调用 `collect_visible_match_bindings(条件根)` 收集可见的 match 绑定变量，登记到体作用域。
- 复用既有 `if` / `while` 条件发码路径（条件求值为 bool 后跳转至体）；条件含 `FENG_EXPR_MATCH_OP` 时，在命中分支内对可见的 binding 变量追加初始化与作用域登记。
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
| AE0030 | 按位运算符操作数类型不匹配 | 已支持 | 复用（覆盖 infix 中 `\|` 被误作按位或的场景，如 `(x match 0) \| 1`） |
| AE1009 (新) | infix match 出现 binding 但 pattern 非 union member type | 不存在 | 新增 |

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
| `docs/feng-error-codes-ae.md` | 新增 AE1009 条目 | +3 行 |
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

- `x match 0` / `x match 1...10` / `x match UserType` 解析为 `FENG_EXPR_MATCH_OP`，`label_count == 1`。
- `x match 0 | 1 | 2` / `x match 0 | 1...10 | 100` / `x match Type1 | Type2` 解析为多 label `FENG_EXPR_MATCH_OP`，`label_count == 3` / `3` / `2`。
- `x match v: UserType` / `x match let v: UserType` / `x match var v: UserType` 解析为带绑定的 `FENG_EXPR_MATCH_OP`。
- `x match v: Type1 | Type2` 解析为多 member 绑定的 `FENG_EXPR_MATCH_OP`，`label_count == 2`。
- `if x match v: UserType { ... }` 解析为 if 语句，条件为 `FENG_EXPR_MATCH_OP`。
- `while x match v: UserType { ... }` 解析为 while 语句，条件为 `FENG_EXPR_MATCH_OP`。
- `x match UserType && y match OtherType` 解析为逻辑与，两操作数均为 `FENG_EXPR_MATCH_OP`。
- `|` 在 pattern 位置作为分隔符：`x match 0 | 1` 解析为 `FENG_EXPR_MATCH_OP`（值列表），**不**解析为 `(x match 0) | 1`（按位或）。
- 上下文相关：`f(x match 0, 1)` 中逗号被解释为参数分隔符，按两参数 `f((x match 0), 1)` 解析；若函数形参数不匹配则由调用端语义检查报错；如需将 `x match 0` 作为单参数，去掉 `, 1` 写为 `f(x match 0)` 即可。
- 非法形式：`let r = x match 0, 1;` parse 报错：`,` 不是 Feng 表达式运算符，`let` RHS 期望 `;` 而遇到 `,`，无处可去（infix 多 label 用 `|`，不用 `,`）。
- 加括号成元组：`let r = (x match 0, 1);` 合法（若 `r` 为二元组类型），`(x match 0, 1)` 是元组字面量；但这是 tuple 语义，不是 match 值列表。
- 合法形式：`x match T == y` 解析为 `(x match T) == y`（左结合，语义合法性由类型检查器捕获）。
- 合法形式：`x match T < y` 解析为 `(x match T) < y`（左结合）。
- 合法形式：`a < b match T` 解析为 `(a < b) match T`（左结合）。
- 合法形式：`x match a match b` 解析为 `(x match a) match b`（左结合，`b` 为 `true` / `false` 时语义合法，否则由类型检查器报错）。

### 7.2 semantic 单元测试

在 `test/semantic/test_semantic.c` 中新增用例：

- 合法 infix match：值、区间、union member type、union member type + binding。
- 合法多 label infix match：`x match 0 | 1 | 2`、`x match 0 | 1...10 | 100`、`x match Type1 | Type2`、`x match v: Type1 | Type2`（`v` 收窄为 `Type1 | Type2` 子集）。
- target 类型不允许（如 `float`）报 AE0050。
- binding 用于非 union member type pattern 报 AE1009（如 `x match v: 0`、`x match v: 1...10`）。
- 多 label pattern 中 binding 与非 type label 混用报 AE1009（如 `x match v: 0 | 1`、`x match v: T | 0`）。
- 绑定变量在不可见范围使用报 AE0001（`||` / `!` / 函数边界外、语句后，标识符查找失败复用既有未定义错误码）。
- 链式 `x match a match b` 解析为 `(x match a) match b`；`b` 为 `true` / `false` 时合法（冗余），`b` 为类型 / 区间 pattern 时由类型检查器报 target 类型不匹配。
- 绑定变量作用域正确（if 体内可见，体外不可见）。
- 绑定变量在 if 体中类型为收窄后的 member 类型。
- 绑定变量在 while 体中每轮迭代重新绑定。
- **`let r = ...` 形式**：
  - `let r = x match v: T && v.foo > 0;` 合法，`v` 在 `v.foo > 0` 内可见，语句后不可见。
  - `let r = x match v: T || v.foo > 0;` 中 `v` 在 `v.foo > 0` 内**不可见**（`||` 子树返回空）。
  - `let r = x match v: T;` 后访问 `v` 报未绑定错误（语句后不可见）。
  - `let r = x match v: T; if r { v.foo }` 报未绑定错误（编译器不跟踪 `r` 真值与 `v` 绑定状态关联）。
- **条件内可见性（递归收集规则）**：
  - `if x match v: T && v.foo > 0 { ... }` 中 `v` 在 `v.foo > 0` 与 if 体内可见。
  - `if x match v: T && v.foo > 0 && v.bar < 10 { ... }` 中 `v` 在后续所有 `&&` 子表达式与 if 体内可见。
  - `if x match v: T1 && y match w: T2 && v.foo + w.bar > 0 { ... }` 中 `v` 与 `w` 均在后续条件与 if 体内可见。
  - `if x match v: T || v.foo > 0 { ... }` 中 `v` 在 `v.foo > 0` 与 if 体内**不可见**（`||` 返回空）。
  - `if !(x match v: T) { ... }` 中 `v` 在 if 体内**不可见**（`!` 返回空）。
  - `if !!(x match v: T) { ... }` 中 `v` 在 if 体内**不可见**（外层 `!` 返回空，不做 `!!` 等价变换）。
  - `if x match v: T || y { ... } else { v.foo }` 中 `v` 在 else 体内**不可见**（else 一律不可见）。
- **`||` / `!` 作为侄子不影响 v 可见性**：
  - `if x match v: T && (cond || other) { ... }` 中 `v` 在 `(cond || other)` 与 if 体内可见（`||` 在侄子位置，只丢弃其内部 match 变量）。
  - `if x match v: T && (match w: U || other) { ... }` 中 `v` 在 `(match w: U || other)` 整体及 `match w: U` / `other` 内可见（`||` 是 v 侄子，但 w 被 `||` 丢弃）。
  - `if x match v: T && !(cond) { ... }` 中 `v` 在 `!(cond)` 与 if 体内可见（`!` 在侄子位置）。
- **嵌套场景**：
  - `if !(x match v: T && (match w: U || other)) { ... }` 中 `v` 在 body 内**不可见**（根 `!` 返回空）；`w` 在 body 内**不可见**（同上）；但 `v` 在 `(match w: U || other)` 整体内可见（表达式内，`&&` 短路保证 v 已绑定）。
  - `if (x match v: T && a) || b { ... }` 中 `v` 在 `a` 内可见（分组内 `&&` 求值时），在 `b` 与 if body 内**不可见**（顶层 `||` 返回空）。
  - `if (x match v: T || a) && b { ... }` 中 `v` 在 `a` / `b` / if body 内均**不可见**（分组内 `||` 返回空，`&&` 合并空 + 空）。
- **不识别的运算符**：
  - `if (x match v: T) == true { v.foo }` 中 `v` 在 body 内**不可见**（`==` 不传播真值约束）。
  - `if (x match v: T) != false { v.foo }` 中 `v` 在 body 内**不可见**（`!=` 不传播）。

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

- [x] 更新 [docs/feng-flow.md](../docs/feng-flow.md) §3：新增「infix match 运算」子节，描述语法、pattern 形式、绑定作用域、优先级；既有标签形式说明中追加 infix 用法引用
- [x] 更新 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md)：新增 AE1009 条目（binding 用于非 union member pattern、多 label pattern 中 binding 与非 type label 混用）；不可见位置使用 binding 变量复用 AE0001，不引入新错误码；错误码编号与文案最终口径由人工审定
- [x] 全量回归点：`make test` 通过（仅文档变更，无代码行为变化）

### 8.2 步骤 2：AST 节点与 Parser 支持 infix match 运算符

- 前置依赖：步骤 1
- 范围：parser 新增 infix match 识别与 pattern 解析；AST 新增 `FENG_EXPR_MATCH_OP` 节点；dump 同步输出

- [x] 在 `src/parser/parser.h` 新增 `FengExprMatchOp` 结构与 `FENG_EXPR_MATCH_OP` 枚举
- [x] 在 `src/parser/parser.c` 的 `parse_comparison` 内识别 `FENG_TOKEN_KW_MATCH` 作为同级 infix 运算符（与 `<` / `<=` / `>` / `>=` 共用同一左结合循环，不新增独立 parser 层）；链式与同级混用统一走左结合路径，不做出口特殊检查
- [x] 复用 `parse_match_branch_binding_prefix` 解析可选 binding（先于 label 解析）
- [x] 复用 `parse_match_label_atom` 解析首个 label；若下一个 token 是 `FENG_TOKEN_PIPE`，循环消费 `|` 并解析后续 label，构造 `FengMatchLabel` 数组（与块形式 `FengMatchBranch.labels` 结构一致）
- [x] `|` 在 pattern 解析循环内作为 label 分隔符消费，退出循环后剩余 `|` 才按按位或处理；由于 `bool | int` 非法（AE0030），此「吃尽」行为无歧义
- [x] 构造 `FENG_EXPR_MATCH_OP` AST 节点（`labels` 数组 + `label_count`）；同步 `free_expr` / `copy_expr` 路径
- [x] 在 `src/parser/dump.c` 新增 `FENG_EXPR_MATCH_OP` case
- [x] 在 `test/parser/test_parser.c` 新增用例
- [x] 全量回归点：`make build/bin/test_parser && build/bin/test_parser` 通过；`make test` 全量回归通过

### 8.3 步骤 3：Semantic 扩展支持 infix match

- 前置依赖：步骤 2
- 范围：semantic 新增 `FENG_EXPR_MATCH_OP` resolve 分支；绑定变量作用域登记
- 行为变化：原 `x match int` 形式无既有语义（`match` 关键字此前不在 infix 位置出现），无既有测试回归

- [x] 复用 `match_target_type_is_allowed` 校验 target
- [x] 复用既有 label 解析路径校验 pattern
- [x] 对 union member pattern + binding，复用 union match 单分支子集
- [x] 对值 / 区间 / enum item 引用 pattern，禁止 binding
- [x] 结果类型为 bool
- [x] 绑定变量在 `if` / `while` 体作用域内登记
- [x] 在 `src/semantic/reifiable_deps.c` 新增 `FENG_EXPR_MATCH_OP` case
- [x] 在 `test/semantic/test_semantic.c` 新增用例
- [x] 全量回归点：`make build/bin/test_semantic && build/bin/test_semantic` 通过；`make test` 全量回归通过

### 8.4 步骤 4：Codegen 发码扩展

- 前置依赖：步骤 3
- 范围：codegen 新增 `FENG_EXPR_MATCH_OP` 发码分支；`if` / `while` 条件位置 binding 作用域处理

- [x] 求值 target 到临时变量
- [x] 复用 `cg_emit_match_label_cond` 单 label 发码
- [x] union member pattern + binding 在命中分支中绑定
- [x] `if` / `while` 条件位置识别并处理 binding 作用域
- [x] `while` 每轮迭代重新求值与绑定
- [x] 在 `test/codegen/test_codegen.c` 新增用例
- [x] 全量回归点：`make build/bin/test_codegen && build/bin/test_codegen` 通过；`make test` 全量回归通过

### 8.5 步骤 5：Symbol Export / LSP 支持

- 前置依赖：步骤 4
- 范围：`symbol/export.c` 与 `cli/lsp/runtime.c` 新增 `FENG_EXPR_MATCH_OP` case

- [ ] 在 `src/symbol/export.c` 新增 `FENG_EXPR_MATCH_OP` case（经核查 export.c 无 expr-kind 通用分发 switch，仅在特定路径按需检查 expr kind；infix match 无 symbol 导出需求，待人工确认是否标记 N/A）
- [x] 在 `src/cli/lsp/runtime.c` 新增 `FENG_EXPR_MATCH_OP` case（completion / hover / definition 行为复用既有 match 表达式路径）
- [x] 全量回归点：`make test` 全量回归通过

### 8.6 步骤 6：cts / fcts 端到端用例

- 前置依赖：步骤 5
- 范围：端到端测试套件追加 infix match 用例

- [x] 在 `fcts/fcts_bin/src/test_flow.ff` 中追加 infix match 端到端用例：覆盖值、区间、union member type、union member type + binding、`if` / `while` 条件形式
- [x] 全量回归点：fcts 套件通过；`make test` 全量回归通过

### 8.7 步骤 7：最终全量回归与收尾

- 前置依赖：步骤 1–6 全部完成
- 范围：最终验收与一致性复核

- [x] 执行 `make test` 全量回归通过
- [x] 复核所有新增测试用例与既有测试用例无冲突
- [x] 复核 `docs/feng-flow.md` / `docs/feng-error-codes-*.md` 与代码实现一致
- [x] 复核 `dev/feng-match-operator-dev.md` 中实现方案与最终代码一致（行号、函数名、错误码编号如有调整需回写文档）
- [x] 准备建议 commit message，由开发者自行提交

## 9 风险评估

| 风险 | 等级 | 缓解措施 |
| ---- | ---- | -------- |
| `match` 关键字作为 infix 运算符与既有 match 语句/表达式起始关键字冲突 | 中 | parser 在 `parse_primary` 中优先识别 match 语句/表达式起始；infix 识别发生在二元运算层，位置不重叠 |
| `\|` 作为 infix 多 label 分隔符与既有用法（按位或、union-form spec 分隔符）冲突 | 中 | `\|` 在 pattern 解析循环内被消费为 label 分隔符，退出循环后剩余 `\|` 才按按位或处理；`bool \| int` 本身非法（AE0030），`(x match 0) \| 1` 这种写法无意义；union-form spec 的 `\|` 仅出现在 `spec U: T1 \| T2` 声明位置，与 infix pattern 位置不重叠。块形式继续用 `,` 不变 |
| 绑定可见性分析实现复杂度 | 低 | 单一递归函数 `collect_visible_match_bindings` 从条件根向下收集：`&&` 合并两侧、`||` / `!` 子树返回空、分组透明穿透、`match v: T` 收集 v。只识别 `&&` / `\|\|` / `!`，不维护两态集合，不做 `!!` 等价变换，不做跨语句跟踪。规则保守可预测，实现简洁。复用既有 match branch binding 的作用域登记路径 |
| `match` 运算符优先级与既有运算符交互 | 中 | 与关系运算 `<` / `<=` / `>` / `>=` 同级、高于 equality、低于算术；同级相邻混用与链式均走既有左结合路径，语义合法性由类型检查器捕获，parser 不做出口特殊检查 |
| `if` / `while` 条件位置的 binding 作用域处理与既有 match 表达式分支 binding 不一致 | 低 | 复用既有 union match binding 的作用域处理路径 |
| 既有 `match_*` 测试受影响 | 低 | 仅新增 infix 形式，既有块形式不变 |
| `match` 关键字与用户代码标识符冲突 | 低 | `match` 已是关键字，行为与其他关键字一致；既有代码已不能以 `match` 为标识符 |

## 10 当前明确不做

- 不引入新的 runtime 原语或值模型分类。
- 不修改既有 `match 目标值 { ... }` 块形式的语义与发码，包括块形式继续用 `,` 作为多 label 分隔符（infix 形式用 `|`，块形式不变；后续是否统一或同时支持 `,` 和 `|`，由人工决策后另行评估）。
- 不引入新的 `FengMatchLabelKind`。
- 不修改既有标签形式（单值、值列表、区间、union member type、enum item 引用）的语义。
- **不做 `!!` 等价变换**：`!!(match v: T)` 中 v 不可见（`!` 子树返回空）；用户应改写为正向形式 `match v: T`。
- **不做 else body 反向推导**：else body 内所有 match 绑定变量一律不可见，即使语义上 match 必定为 true（如 `if (!cond) { } else { use(v); }`）；用户应改写为 `if (cond) { use(v); }`。
- **不跨语句跟踪绑定状态**：`let r = x match v: T; if r { use(v); }` 中 v 不可见；用户应把 match 表达式直接写在条件位置。
- **不识别非 `&&` / `||` / `!` 运算符的真值传播**：`==` / `!=` / `^` / `?:` / 位运算 / 函数调用 / 类型转换等不传播 match 表达式的真值约束。

## 11 交付约束

- 所有实现必须以 [docs/feng-flow.md](../docs/feng-flow.md) §3 与本文档为准，不得在编码阶段临时放宽或收紧。
- 错误码编号与文案必须先在 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md) 中确定，再落到代码。
- 每个阶段都以「先文档、后代码、再测试」的顺序落地，不修改既有测试语义，只新增覆盖。
- 若实现过程中发现规范仍有缺口，先回写对应权威文档，再继续编码。
- 实现过程中如发现新的不确定语义，必须由人工决策后才能继续。
- 不得引入新的 runtime 对象表示、runtime API 或额外值模型分类。
- 不得边写代码边临时决定错误码编号或文案。

## 建议 commit message

```text
feat(match): add infix match operator for single-pattern testing

Introduce `expr match pattern` as an infix operator that returns a
bool indicating whether `expr` matches `pattern`. The pattern reuses
the existing match label forms: single value, integer range, and
union member type, with multi-label patterns separated by `|` (e.g.
`x match 0 | 1 | 2`, `x match Type1 | Type2`). Block-form match keeps
`,` as the label separator unchanged. Union member patterns support
optional binding `[let|var] name: Type`, with the bound variable
visible inside the `if`/`while` body when used as a condition.

Binding visibility follows a conservative AST-ancestor rule: a bound
variable is visible at a position iff the path from the `match v: T`
node to that position contains only `&&` and grouping nodes (no `||`
or `!` ancestor). `||` or `!` only blocks visibility when it is an
ancestor of the match node; as a nephew (in a sibling subtree) it
does not block. Only `&&` / `||` / `!` are recognized for visibility
propagation; `==` / `!=` / `^` / `?:` / function calls / casts do
not propagate. No `!!` equivalence folding, no else-body reverse
derivation, no cross-statement tracking. This is a conservative
simplification of C# `is` pattern matching.

The change adds a new AST node `FENG_EXPR_MATCH_OP` with a label array
(`labels` + `label_count`) mirroring `FengMatchBranch.labels`, reusing
the existing label parsing, semantic resolution, and codegen paths for
match labels. No new runtime primitive is introduced.
```
