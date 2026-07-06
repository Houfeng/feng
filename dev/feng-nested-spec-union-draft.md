# 嵌套 Spec Union 赋值问题与优化方案

> **状态**：方案二已确定，待实施。
> **背景**：Parser 6.4 实施过程中发现 `LambdaBody: Expression | Block` 的嵌套 spec union 赋值失败（AE1003）。
> **落选方案**：方案一、方案三见 [feng-nested-spec-union-rejected.md](feng-nested-spec-union-rejected.md)。

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

当前嵌套 spec union 共 3 处，全部在 `FengAstNodes.ff`：

| 声明 | 嵌套来源 |
|------|----------|
| `LambdaBody: Expression \| Block` | Expression 是 spec union |
| `MatchTarget: ... \| TypeReference \| ...` | TypeReference 是 spec union |
| `ForUpdate: Expression \| AssignmentStmt` | Expression 是 spec union |

语义分析和代码生成尚未消费这些类型，当前是实施改动影响最小的时机。

---

## 2 方案设计：不展开 + 多级链路

### 2.1 核心原则

Spec union 保持声明时的层次结构，不递归展开。赋值时做多级链路查找，match 时逐级收窄。类型关系与声明结构一致，心智模型清晰。

### 2.2 成员收集

`collect_normalized_union_member` 不再递归展开嵌套 spec union。`LambdaBody` 的成员为 `[Expression, Block]`（2 个），而非 23 个叶子类型。

```c
// 修改后：直接添加成员，不递归展开
if (resolved is union spec) {
    return append_normalized_union_member(member_ref);  // 保持原样
}
return append_normalized_union_member(member_ref);
```

每个成员节点记录：
- `member_type_ref`：成员的类型引用（可能是具体类型或 spec union）
- `is_nested_union`：该成员是否为 spec union（用于后续链路查找）

### 2.3 赋值校验（多级链路查找）

`select_union_member_for_expr_type` 改为多级查找：

```
function select_union_member(source_type, target_union):
    // 第一级：直接匹配
    for each member in target_union.members:
        if types_equal(source_type, member.type_ref):
            return MatchResult(member, path=[member])

    // 第二级：通过嵌套 spec union 间接匹配
    for each member in target_union.members:
        if member.is_nested_union:
            nested_result = select_union_member(source_type, member.resolved_union)
            if nested_result is MatchResult:
                return MatchResult(member, path=[member, ...nested_result.path])

    // 歧义检测：收集所有可达路径
    paths = collect_all_reachable_paths(source_type, target_union)
    if paths.length > 1:
        return AmbiguousError(paths)

    return NoMatch
```

**赋值示例**：

```feng
// 直接匹配
let body: LambdaBody = Block(...);
// source=Block, target=LambdaBody → 直接匹配 Block 成员

// 间接匹配（单级）
let ident: IdentifierExpr = ...;
let body: LambdaBody = ident;
// source=IdentifierExpr, target=LambdaBody
//   → 直接匹配？No（IdentifierExpr ≠ Expression, IdentifierExpr ≠ Block）
//   → 通过 Expression 间接匹配？Yes（IdentifierExpr 是 Expression 的成员）
//   → path = [Expression, IdentifierExpr]

// 整体赋值
let expr: Expression = ...;
let body: LambdaBody = expr;
// source=Expression, target=LambdaBody → 直接匹配 Expression 成员
// 运行时：外层 tag=Expression，内层 tag 保持 Expression 自身的 tag
```

**歧义报错**：

```feng
open spec A: X | Y;
open spec B: X | Z;
open spec C: A | B;

let x: X = ...;
let c: C = x;
// 错误：X 可通过 A 和 B 两条路径到达 C，需要显式转换
// 修复：let c: C = x as A;  // 或 x as B
```

### 2.4 运行时表示

采用嵌套标记联合（nested tagged union），每个 spec union 层一个 tag。

**内存布局**：

```
LambdaBody {
    outer_tag: u8          // 0=Expression, 1=Block
    union payload {
        // outer_tag=0 时有效
        expression: Expression {
            inner_tag: u8   // Expression 自身的 variant tag
            data: [...]     // 具体变体的数据
        }
        // outer_tag=1 时有效
        block: Block { ... }
    }
}
```

**Tag 编码规则**：
- 每个 spec union 层的 tag 值从 0 开始，按成员声明顺序递增
- 嵌套层数在实践中通常 ≤ 2，内存开销可控
- `Expression` 整体赋给 `LambdaBody` 时，外层 tag=0，内层 tag 原样保留

### 2.5 Match 行为

**基础 match**：匹配直接成员，逐级收窄。

```feng
let body: LambdaBody = ...;

match body {
    Expression {
        // body 在此分支内收窄为 Expression 类型
        // 可进一步 match 具体变体
        match body {
            IdentifierExpr { ... }
            BooleanLiteralExpr { ... }
            else { ... }
        }
    }
    Block { ... }
}
```

**穷尽性检查**：只检查直接成员覆盖，不检查展开后的叶子类型。

- `LambdaBody` 需覆盖 `Expression` 和 `Block`
- 在 `Expression` 分支内的嵌套 match，需覆盖 `Expression` 的所有直接成员

### 2.6 多级 Match 语法（后续增强）

在基础 match 稳定后，可增加 `->` 链式模式语法，消除嵌套 match 的冗余：

```feng
// 链式写法
match body {
    Expression -> IdentifierExpr { ... }
    Expression -> BooleanLiteralExpr { ... }
    Block { ... }
    else { ... }
}

// 等价于嵌套写法
match body {
    Expression {
        match body {
            IdentifierExpr { ... }
            BooleanLiteralExpr { ... }
            else { ... }
        }
    }
    Block { ... }
}
```

支持任意深度链：

```feng
match value {
    A -> B -> C { ... }
    A -> D { ... }
    E { ... }
}
```

**编译器处理**：在 pattern 编译阶段将 `->` 链递归展开为嵌套 match，不影响类型系统和运行时表示。穷尽性检查按层级验证。

> 此语法为纯前端增强，独立排期，不影响核心方案。

---

## 3 代码变更清单

### 3.1 类型系统

| 文件 | 函数 | 变更 |
|------|------|------|
| analyzer.c | `collect_normalized_union_member` | 移除递归展开逻辑 |
| analyzer.c | `select_union_member_for_expr_type` | 改为多级链路查找，增加歧义检测 |
| analyzer.c | `validate_expr_against_expected_type` | 适配新的 MatchResult（含 path 信息） |

### 3.2 模式匹配

| 文件 | 函数 | 变更 |
|------|------|------|
| analyzer.c | match 穷尽性检查 | 改为检查直接成员覆盖 |

### 3.3 代码生成

| 文件 | 函数 | 变更 |
|------|------|------|
| codegen | spec union tag 编码 | 支持嵌套 tag 布局 |
| codegen | spec union 赋值 | 按 path 设置多级 tag |
| codegen | spec union 值访问 | 按 tag 层次遍历 |

### 3.4 规范文档

| 文件 | 变更 |
|------|------|
| spec 文档 | 更新 spec union 语义：非展开、多级链路、嵌套 tag |

---

## 4 边界场景

### 4.1 三层及以上嵌套

```feng
open spec A: X | Y;
open spec B: A | Z;
open spec C: B | W;

let x: X = ...;
let c: C = x;  // path = [B, A, X]，三级链路
```

运行时：三级 tag 嵌套。查找递归进行，深度无硬限制。

### 4.2 同一类型多条路径（歧义）

```feng
open spec A: X;
open spec B: X;
open spec C: A | B;

let x: X = ...;
let c: C = x;  // 歧义：X → A → C 或 X → B → C
```

编译器报错，要求显式转换：`let c: C = x as A;`

### 4.3 open spec 扩展

```feng
open spec Base: A | B;
open spec Extended: Base | C;

// 后续通过 open 扩展 Base
open spec Base: ... | D;
```

`Extended` 的成员为 `[Base, C]`。`D` 作为 `Base` 的新成员，通过 `Base` 路径可达 `Extended`。链路查找自动适应 `open spec` 的扩展。

### 4.4 泛型 spec union

```feng
open spec Option<T>: None | T;
open spec Result<T>: Option<T> | Error;

let none_val: None = None;
let r: Result<int> = none_val;
// path = [Option<int>, None]
```

泛型实例化后，链路查找在实例化类型上进行。

---

## 5 实施分期

### 一期：核心（解除 Parser 6.4 阻塞）

1. 成员收集：移除递归展开
2. 赋值校验：多级链路查找 + 歧义检测
3. 运行时：嵌套 tag 布局
4. 基础 match：匹配直接成员 + 嵌套 match 收窄
5. 穷尽性检查：直接成员覆盖
6. 全量回归测试

### 二期：语法增强（独立排期）

1. `->` 链式模式语法解析
2. Pattern 编译展开
3. 链式穷尽性检查
4. 测试覆盖

---

## 6 参考

- 问题发现：Parser 6.4 实施（`LambdaBody: Expression | Block`）
- 相关代码：`src/semantic/analyzer.c`（`collect_normalized_union_member`、`validate_expr_against_expected_type`、`select_union_member_for_expr_type`）
- AST 定义：`std/src/compiler/parser/FengAstNodes.ff`
- 嵌套 spec union 实例：`LambdaBody`（:389）、`MatchTarget`（:422）、`ForUpdate`（:650）
