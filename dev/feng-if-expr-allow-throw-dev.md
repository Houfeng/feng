# if/match/try 表达式分支块允许以 throw 结尾

## 状态: done

## 背景

当前 `if` 表达式、`match` 表达式的分支块要求最后一条语句必须是表达式（`FENG_STMT_EXPR`）或可转换为表达式的 if/match/try 语句。`try` 表达式的 catch 子句已经支持以 `throw` 结尾，但 `if` 和 `match` 表达式尚未支持。

`throw` 会终止当前执行路径，语义上不需要提供结果值，因此应允许分支块以 `throw` 结尾。

## 示例

```feng
let x = if cond {
  throw "error";
} else {
  42;
};

let label = match value {
  0 { "zero"; }
  1 { throw "unexpected one"; }
  else { "other"; }
};
```

## 规则

- `if` 表达式的 then/else 分支块、`match` 表达式的各分支块（含 else），最后一条语句允许为 `throw`。
- 以 `throw` 结尾的分支不参与结果类型推导和一致性比较。
- 如果所有分支都以 `throw` 结尾，则表达式结果类型为 unknown（不可用于需要明确类型的上下文）。
- 如果一个分支 throw、另一个有 yield 表达式，则结果类型取有 yield 的分支类型。
- `try` 表达式的 catch 子句已经支持 `throw`，无需修改。

## 实施方案

### 1. 语义分析（src/semantic/analyzer.c）

#### 1.1 修改 `validate_block_yields_expression`

在 `block_yield_expression(block) != NULL` 检查后，增加对 `block_terminates_with_throw(block)` 的检查。

```
位置: analyzer.c:6361
变更: 增加 throw 终止检查
```

#### 1.2 修改 `validate_if_expr` 类型一致性检查

当前逻辑: 两个分支都必须有 yield expression，类型必须一致。
改为:
- 两个分支都以 throw 结尾 → 通过验证
- 一个分支 throw，另一个有 yield → 通过验证，不比较类型
- 两个都有 yield → 类型必须一致（不变）

```
位置: analyzer.c:6374-6424
```

#### 1.3 修改 match 表达式类型一致性检查

两处逻辑（union match 和普通 match）:
- 获取 `expected` 类型时，跳过以 throw 结尾的分支
- 比较分支类型时，跳过以 throw 结尾的分支

```
位置:
- Union match: analyzer.c:7200-7236
- General match: analyzer.c:7378-7412
```

#### 1.4 修改类型推导 `infer_expr_type`

`FENG_EXPR_IF` 和 `FENG_EXPR_MATCH` 的类型推导:
- 以 throw 结尾的分支类型视为 unknown
- 结果类型取第一个已知的非 throw 分支类型
- 所有分支都 throw → 结果类型为 unknown

```
位置: analyzer.c:13568-13599
```

### 2. 代码生成（src/codegen/codegen.c）

#### 2.1 修改 `cg_emit_if_expr`

- 不再要求两个分支都有 yield expression
- 结果类型从非 throw 分支推导
- 对于 throw 结尾的分支，直接发射所有语句（包括 throw），不写入 result slot
- 对于有 yield 的分支，继续使用 `cg_emit_branch_into_slot`
- 如果两个分支都 throw，不需要分配 result slot，直接发射两个分支

```
位置: codegen.c:17263-17368
```

#### 2.2 修改 `cg_emit_match_expr`

类似处理:
- 允许分支以 throw 结尾
- 结果类型从非 throw 分支推导
- throw 结尾的分支直接发射所有语句

```
位置: codegen.c:17417-17785
```

### 3. 文档更新

#### 3.1 docs/feng-flow.md

更新 §4 `if` 表达式规则:
- 在现有规则后补充: 各分支块的最后一条语句也允许是 `throw`，此时该分支不产生结果值。

#### 3.2 docs/feng-exception.md

补充说明 `if`/`match` 表达式分支中也允许以 `throw` 结尾。

## 不需要修改的部分

- **Parser（parser.c）**: `convert_trailing_yield_stmt_to_expr` 只转换 if/match/try 语句为表达式形式。`throw` 本身就是 `FENG_STMT_THROW` 语句，无需转换，保留原样。
- **try 表达式的 catch 子句**: 已经支持 throw（`block_terminates_with_throw`），无需修改。

## Todo

- [x] **T1. 语义: validate_block_yields_expression** — 增加 `block_terminates_with_throw` 检查，throw 结尾视为合法终止（if/match 共用，改一处全部生效）
- [x] **T2. 语义: validate_if_expr** — 类型一致性比较时，跳过以 throw 结尾的分支
- [x] **T3. 语义: union match 类型比较** — 获取 expected 和逐分支比较时，跳过 throw 分支
- [x] **T4. 语义: general match 类型比较** — 同上
- [x] **T5. 语义: infer_expr_type** — FENG_EXPR_IF / FENG_EXPR_MATCH 推导时，throw 分支类型视为 unknown，结果取第一个已知的非 throw 分支类型
- [x] **T6. 代码生成: cg_emit_if_expr** — throw 分支走 cg_emit_block 整体发射，非 throw 分支走 cg_emit_branch_into_slot；两个都 throw 时不分配 result slot
- [x] **T7. 代码生成: cg_emit_match_expr** — 同上模式，throw 分支走 cg_emit_block
- [x] **T8. 文档: docs/feng-flow.md** — 更新 §4 if 表达式规则，补充 throw 允许说明
- [x] **T9. 文档: docs/feng-exception.md** — 补充 if/match 表达式分支允许 throw
- [x] **T10. 全量回归测试** — 编译通过并运行现有测试套件

## 验证

1. 测试用例:
   - `if` 表达式: then throw / else 有值
   - `if` 表达式: then 有值 / else throw
   - `if` 表达式: 两个分支都 throw
   - `match` 表达式: 部分分支 throw，其余有值
   - `match` 表达式: 所有分支都 throw
2. 全量回归测试
