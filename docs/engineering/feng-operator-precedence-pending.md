# 运算符优先级：`==` 高于位运算的历史遗留

> 状态：已交付（delivered）  
> 日期：2026-07-11  
> 关联规范：`docs/specifications/feng-expression.md` §5 运算符优先级

## 1. 问题概述

Feng 当前的运算符优先级表中，相等性比较运算符 `==` / `!=`（优先级 7）**高于** 按位与 `&`（优先级 8）和按位或 `|`（优先级 10）。

这意味着：

```feng
a & b == 7   // 被解析为 a & (b == 7)，而非 (a & b) == 7
```

此设计与 C/C++/Java/C# 一致，但与直觉相悖——位运算在概念上类似算术（`*`、`+`），应当像算术一样**高于**比较运算。业界普遍认为 C 系的 `==` 高于位运算的设计是一个缺陷（参见 Wikipedia *Operators in C and C++* §"Criticism of bitwise and equality operators precedence"），源于 BCPL/早期 C 中 `&` `|` 兼做逻辑运算符的历史遗留。

## 2. 主流语言对比

以下数据均来自各语言官方文档，优先级从高到低排列（仅截取相关层级）。

### 2.1 C 系（`==` 高于位运算）

| 语言 | `==` vs `&` | `==` vs `\|` | 来源 |
|---|---|---|---|
| **Feng** | `==` > `&` | `==` > `\|` | `docs/specifications/feng-expression.md` §5 |
| C / C++ | `==` > `&` | `==` > `\|` | Wikipedia: Operators in C and C++ |
| Java | `==` > `&` | `==` > `\|` | JLS §15.22–15.23 |
| C# | `==` > `&` | `==` > `\|` | C# 语言规范 |

C 系优先级（从高到低，节选）：

```
* / %          →  + -          →  << >>         →  < <= > >=
→  == !=       →  &             →  ^             →  |
→  &&          →  ||
```

### 2.2 现代系（位运算高于 `==`）

| 语言 | `==` vs `&` | `==` vs `\|` | 来源 |
|---|---|---|---|
| Python | `&` > `==` | `\|` > `==` | Python 文档 §6.17 |
| Rust | `&` > `==` | `\|` > `==` | Rust Reference §Expression precedence |
| Go | `&` > `==` | `\|` > `==` | Go 语言规范 §Operator precedence |
| Swift | `&` > `==` | `\|` > `==` | Apple Operator Declarations |
| Zig | `&` > `==` | `\|` > `==` | Zig 文档 §Operators §Precedence |

现代系优先级（从高到低，节选，以 Zig/Swift 为代表）：

```
* / %          →  + -          →  << >>         →  &             →  ^  →  |
→  == != < <= > >=  →  &&      →  ||
```

Swift 官方文档明确指出：

> "Swift's operator precedences and associativity rules are simpler and more predictable than those found in C and Objective-C."

Wikipedia 对 C 的批评原文：

> "The precedence of the bitwise logical operators has been criticized. Conceptually, & and | are arithmetic operators like \* and +. The expression `a & b == 7` is syntactically parsed as `a & (b == 7)` whereas the expression `a + b == 7` is parsed as `(a + b) == 7`. This requires parentheses to be used more often than they otherwise would."

### 2.3 对比汇总

| 语言 | `==` vs `&` | `==` vs `\|` | 阵营 |
|---|---|---|---|
| **Feng** | `==` > `&` | `==` > `\|` | **C 系** |
| C / C++ | `==` > `&` | `==` > `\|` | C 系 |
| Java | `==` > `&` | `==` > `\|` | C 系 |
| C# | `==` > `&` | `==` > `\|` | C 系 |
| Python | `&` > `==` | `\|` > `==` | 现代 |
| Rust | `&` > `==` | `\|` > `==` | 现代 |
| Go | `&` > `==` | `\|` > `==` | 现代 |
| Swift | `&` > `==` | `\|` > `==` | 现代 |
| Zig | `&` > `==` | `\|` > `==` | 现代 |

## 3. Feng 当前设计

Feng `docs/specifications/feng-expression.md` §5 优先级表（数值越小优先级越高）：

| 优先级 | 形式 | 说明 |
|---|---|---|
| 1 | `expr(...)` `expr.member` `expr[index]` | 调用、成员访问、下标访问 |
| 2 | `~x` `-x` `!x` | 按位非（整数）、数值取负、逻辑非（`bool`） |
| 3 | `*` `/` `%` | 乘除模 |
| 4 | `+` `-` | 加减 |
| 5 | `<<` `>>` | 左移、右移 |
| 6 | `<` `<=` `>` `>=` | 比较运算 |
| **7** | **`==` `!=`** | **相等性比较** |
| **8** | **`&`** | **按位与** |
| 9 | `^` | 按位异或 |
| **10** | **`\|`** | **按位或** |
| 11 | `&&` | 逻辑与 |
| 12 | `\|\|` | 逻辑或 |

## 4. 影响分析

### 4.1 当前行为

```feng
// a & b == c 被解析为 a & (b == c)
// 若 a 为 u64、b 为 u64、c 为 u64 字面量，则 (b == c) 产生 bool，
// a & bool 类型不匹配 → AE0030
let result = (a & b) == 7;   // 需要括号才能表达"先按位与，再比较"
```

### 4.2 变更后行为（拟）

```feng
// a & b == c 被解析为 (a & b) == c，与直觉一致
let result = a & b == 7;     // 等价于 (a & b) == 7
```

### 4.3 破坏性影响

- 所有形如 `expr & expr == expr`（无括号）的现有代码，语义将发生变化
- 当前 `fcts/` 和 `test/` 中已有受影响的测试用例（如 `test_expression.ff` 中的 `operator precedence bitwise vs logical` 测试），需要同步调整
- `docs/specifications/feng-expression.md` §5 优先级表需要重新排序

## 5. 拟定变更方案

采用与 Rust / Zig 一致的方案：仅将位运算 `&` `^` `|`（原优先级 8/9/10）整体上移到比较运算之前（新优先级 6/7/8），比较运算与相等性比较顺延为 9/10，其余层级不变。

Rust 参考文档的优先级表（从高到低，节选）：

```
* / %  →  + -  →  << >>  →  &  →  ^  →  |  →  == != < > <= >=  →  &&  →  ||
```

Zig 文档的优先级表（从高到低，节选）：

```
* / %  →  + -  →  << >>  →  &  →  ^  →  |  →  == != < > <= >=  →  and  →  or
```

两者结构完全相同：移位 → 按位与 → 按位异或 → 按位或 → 比较/相等性 → 逻辑与 → 逻辑或。

### 变更内容

将优先级表从：

```
... << >> → < <= > >= → == != → & → ^ → | → && → ||
```

调整为：

```
... << >> → & → ^ → | → < <= > >= → == != → && → ||
```

即：仅将原 8/9/10（`&` `^` `|`）上移为 6/7/8，原 6/7（`< <= > >=` / `== !=`）顺延为 9/10，其余层级不变。

### 新优先级表

| 优先级 | 形式 | 说明 | 结合方向 |
| --- | --- | --- | --- |
| 1 | `expr(...)` `expr.member` `expr[index]` | 调用、成员访问、下标访问 | 从左到右 |
| 2 | `~x` `-x` `!x` | 按位非（整数）、数值取负、逻辑非（`bool`） | 从右到左 |
| 3 | `*` `/` `%` | 乘除模 | 从左到右 |
| 4 | `+` `-` | 加减 | 从左到右 |
| 5 | `<<` `>>` | 左移、右移 | 从左到右 |
| 6 | `&` | 按位与 | 从左到右 |
| 7 | `^` | 按位异或 | 从左到右 |
| 8 | `\|` | 按位或 | 从左到右 |
| 9 | `<` `<=` `>` `>=` | 比较运算 | 从左到右 |
| 10 | `==` `!=` | 相等性比较 | 从左到右 |
| 11 | `&&` | 逻辑与 | 从左到右 |
| 12 | `\|\|` | 逻辑或 | 从左到右 |

### 涉及修改

- `docs/specifications/feng-expression.md` §5 优先级表
- `src/parser/parser.c` 运算符优先级解析顺序
- `fcts/fcts_bin/src/test_expression.ff` 中受影响的优先级测试用例
- `test/` 中受影响的 C 测试用例（如有）

## 6. 事实依据

本文档中的对比数据来自以下权威来源：

| 语言 | 来源 |
|---|---|
| C / C++ | Wikipedia: *Operators in C and C++*（含 "Criticism of bitwise and equality operators precedence" 一节） |
| Python | Python 3 官方文档 §6.17 *Operator precedence* |
| Rust | Rust Reference §Expression precedence |
| Go | Go 语言规范 §Operator precedence |
| Swift | Apple Developer Documentation: *Operator Declarations*（`BitwiseShiftPrecedence` / `MultiplicationPrecedence` / `AdditionPrecedence` / `ComparisonPrecedence`） |
| Zig | Zig 官方文档 §Operators §Precedence |

## 7. 状态

此为 pending 状态，待人工决策是否实施优先级调整。该变更属于破坏性变更，需在主要版本边界进行。

实施前提：
- 人工明确批准
- 全量回归测试通过
- 文档、测试同步更新

## 8. 实施 TODO

以下为获批后的分步实施计划，每一步均需独立验证后再进入下一步。

进度概览：`[ 7 / 7 ]`

- [x] **TODO 1：更新文档 §5 优先级表**
  - 文件：`docs/specifications/feng-expression.md` §5
  - 内容：将 `&` `^` `|`（原优先级 8/9/10）上移为 6/7/8，`< <= > >=`（原 6）顺延为 9，`== !=`（原 7）顺延为 10
  - 验证：文档内容与新优先级表一致，无遗漏层级

- [x] **TODO 2：调整 parser 优先级链顺序**
  - 文件：`src/parser/parser.c`
  - 内容：将调用链从 `parse_shift → parse_comparison → parse_equality → parse_bit_and → parse_bit_xor → parse_bit_or` 调整为 `parse_shift → parse_bit_and → parse_bit_xor → parse_bit_or → parse_comparison → parse_equality`
  - 注意：`parse_comparison` 内包含 infix `match` 运算符（与关系运算同级），调整后 match 的优先级也随之变为高于位运算，需确认 `docs/engineering/feng-match-operator-dev.md` 中记录的 match 语义不受影响
  - 验证：编译通过，无警告

- [x] **TODO 3：修正 `test_bitwise_expr_parsing` 断言**
  - 文件：`test/parser/test_parser.c` `test_bitwise_expr_parsing`
  - 内容：原测试验证 `a | b ^ c & d == e << f` 的 AST 为 `shift > equality > & > ^ > |`，变更后 AST 变为 `shift > & > ^ > | > comparison > equality`，需重写断言以匹配新的解析结构
  - 验证：`test_parser` 单独运行通过

- [x] **TODO 4：检查并修正 infix match 相关 parser 测试**
  - 文件：`test/parser/test_parser.c`
  - 涉及测试：`test_infix_match_op_mixed_with_relational_and_equality` 等
  - 内容：match 原与关系运算同级，变更后 match 仍与关系运算同级但整体高于位运算；需验证现有断言是否仍成立，不成立则修正
  - 验证：`test_parser` 全部通过

- [x] **TODO 5：调整 fcts 优先级测试用例**
  - 文件：`fcts/fcts_bin/src/test_expression.ff`
  - 涉及测试：
    - `operator precedence bitwise layers`（`&` 高于 `|` 不变，但 `&` 与 `==` 的关系变化）
    - `operator precedence shift vs comparison`（shift 仍高于 comparison，不受影响）
    - `operator precedence comparison vs equality`（comparison 仍高于 equality，不受影响）
    - `operator precedence bitwise vs logical`（位运算高于逻辑，不受影响）
    - `parentheses override precedence`（有括号，不受影响）
  - 内容：逐个检查每个测试用例，若断言依赖 `==` 高于 `&`/`|` 则需调整；同时新增测试用例验证新优先级：`a & b == c` 解析为 `(a & b) == c`、`a | b == c` 解析为 `(a | b) == c`、`a ^ b < c` 解析为 `(a ^ b) < c`
  - 验证：`feng run ./fcts/fcts_bin` 中表达式测试通过

- [x] **TODO 6：排查 std/ 代码中无括号的 `位运算 == 比较` 写法**
  - 范围：`std/std/src/**/*.ff`
  - 内容：搜索所有形如 `expr & expr == expr`、`expr | expr == expr`、`expr ^ expr == expr` 等无括号写法，逐一判断变更后语义是否变化；若语义变化则补加括号保持原有行为
  - 验证：`feng build std` 通过，std-tests 通过

- [x] **TODO 7：全量回归测试**
  - 命令：`make test-normal`
  - 内容：运行 C 单元测试 + fcts + smoke + cli-tests + std-tests + perf-constraints
  - 验证：全部通过，无回归
