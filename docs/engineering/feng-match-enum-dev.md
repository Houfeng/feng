# match 支持 enum 值匹配

## 状态: delivered

> [docs/feng-enum.md](../docs/feng-enum.md) 是 enum 的专项规范；
> [docs/feng-flow.md](../docs/feng-flow.md) §3 是 `match` 模式匹配的专项规范；
> 本文只写开发步骤与 TODO，不重复规范定义。

## 1 问题背景

当前 `match` 仅支持两种匹配形式（见 [docs/feng-flow.md](../docs/feng-flow.md) §3）：

1. **常量相等性匹配**：目标类型限定为「所有整型、`string`、`bool`」，标签为字面量或 `let` 绑定到字面量。
2. **union-form member 匹配**：目标类型为 union-form `spec`，标签为归一化 member。

`enum` 作为「具名的 `int` 标量」（见 [docs/feng-enum.md](../docs/feng-enum.md) §3）当前无法进入 `match`：

- 语义层 `match_target_type_is_allowed`（`src/semantic/analyzer.c:6666`）把 enum 视为不允许，触发 `AE0050`「match target type '%s' is not allowed; allowed types are integers, 'string' and 'bool'」。
- parser 层 `parse_match_label_atom`（`src/parser/parser.c:2688`）只接受 `INTEGER / STRING / BOOL / IDENTIFIER`，无法承载 `EnumName.ItemName` 这种 member access 形式的标签。
- 语义层 `extract_match_label_literal`（`src/semantic/analyzer.c:6608`）只识别字面量与 `let` 绑定到字面量的标识符，不识别 enum item 引用。
- `MatchConstKind`（`src/semantic/analyzer.c:6554`）只有 `INT / BOOL / STRING` 三种，无 enum 类别。
- codegen 层 `cg_emit_match_label_cond`（`src/codegen/codegen.c:17604`）的 `FENG_MATCH_LABEL_VALUE` 分支已经走 `cg_emit_expr` 通用路径，对 enum item 引用本身能正确发出 `EnumName__ItemName` 常量；enum 的 `CGType` kind 为 `CG_TYPE_I32`（`cgtype_new_enum` 于 `codegen.c:210`），所以 `cgtype_is_integer(tk)` 已隐式放行 enum target。codegen 层除路径放行外不增加新分支。

实际缺口集中在「标签必须是 enum item 引用、且与 target 同 enum 类型」这一层语义约束；表示层与发码层成本极低。

## 2 设计目标

- 把 `enum` 类型纳入 `match` 常量相等性匹配的目标类型集合。
- 标签仅接受 `EnumName.ItemName` 形式的 enum item 引用；标签的 enum 类型必须与 target 的 enum 类型一致。
- 复用既有「单值」标签 AST 节点与 codegen 发码路径，不引入新的 AST kind、不新增 enum 专用 runtime 原语。
- 不放宽既有「禁止 `int -> enum` 转换、禁止 `enum` 与 `int` 隐式比较」的语义边界（见 [docs/feng-enum.md](../docs/feng-enum.md) §3、§4）。
- 不引入 enum 区间匹配（enum 区间在语义上无意义，与底层 int 表达不耦合）。
- 不引入 enum 字面量与 enum item 引用混用：同一 match 体中 enum 分支只允许 enum item 引用，不允许把 `int` 字面量或别的 enum 类型标签混入。
- 不破坏既有「字面量、值列表、整数区间」三类常量相等性匹配的语义与发码。
- 抽象驱动、面向未来可扩展：标签值路径以「值表达式」为抽象（`FENG_MATCH_LABEL_VALUE` 的 `value` 字段已为 `FengExpr*`），新增支持的 enum item 引用也是值表达式的一个合法实例，无需新增 label kind。

## 3 规范变更（先文档）

按 CLAUDE.md 「先规范、后代码、再测试」要求，先回写规范再动代码。

### 3.1 [docs/feng-flow.md](../docs/feng-flow.md)

修改点：

- §3 开头「该语法按目标值静态类型分为两类」列表中，**常量相等性匹配**的目标类型集合由「所有整型、`string`、`bool`」扩展为「所有整型、`string`、`bool`、`enum`」。
- §3.1 第一条「匹配目标类型仅支持 `所有整型`、`string`、`bool`」改为「匹配目标类型仅支持 `所有整型`、`string`、`bool`、`enum`」。
- §3.1 标签形式列表「**单值**」一项中追加：当目标类型为 `enum` 时，单值必须写为 `EnumName.ItemName` 形式的 enum item 引用；同一 match 体中的所有 enum 标签必须引用与目标类型相同的 `enum`，不得跨 `enum`，也不得与字面量、值列表、整数区间标签混用。
- §3.1 标签形式列表「**值列表**」一项追加：`enum` 目标类型下的值列表元素必须是同一 `enum` 的 enum item 引用，按逗号分隔，如 `HttpStatus.Ok, HttpStatus.NotFound`。
- §3.1 标签形式列表「**整数闭区间**」一项保留「仅支持整型」描述不动，自然不覆盖 enum；在该项末尾追加一句明确「`enum` 目标类型不支持区间标签」。
- §3.1 编译器交叉检测规则中追加：对 `enum` 目标类型，同一 enum item 在多个分支重复出现视为不可达死代码；不同 enum 的同底层 `int` 值不视为重叠。
- §3.1 给出一个 enum 匹配代码示例，仅作为语法与语义示意，不重复规范定义。

### 3.2 [docs/feng-enum.md](../docs/feng-enum.md)

修改点：

- §3 语义条目追加一条：`enum` 可作为 `match` 常量相等性匹配的目标类型；标签必须为该 `enum` 的 item 引用，具体规则与限制见 [Feng 语言流程控制规范](./feng-flow.md) §3.1。
- §7 关联列表中追加一条指向 [Feng 语言流程控制规范](./feng-flow.md) 的链接。
- 不在 `feng-enum.md` 中重复 `match` 标签规则；规范收敛以 `feng-flow.md` 为单一信源。

### 3.3 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md) / [docs/feng-error-codes-ce.md](../docs/feng-error-codes-ce.md) / [docs/feng-error-codes.md](../docs/feng-error-codes.md)

- 更新 `AE0050` 描述：允许列表从「integers, 'string' and 'bool'」改为「integers, 'string', 'bool' and enum」（保留原错误码，仅放宽允许集合）。
- 复用 `AE1106`「match label overlaps with an earlier label and is unreachable」覆盖 enum item 重复场景；不新增重叠错误码。
- 复用 `AE0404`「enum '%.*s' has no item '%.*s'」覆盖 enum item 引用中 item 名不存在场景；不新增该场景错误码。
- 新增以下错误码（命名与编号按既有规则由人工审定后再落地）：
  - **AE11xx**「match enum target requires enum item reference label」：enum 目标类型的分支标签必须是 `EnumName.ItemName` 形式的 enum item 引用，触发于语义层 `extract_match_label_literal` 无法把标签解析为 enum item 时。
  - **AE11xx**「match label references enum '%.*s' but target type is enum '%.*s'」：标签引用的 enum 类型与 target enum 类型不一致。
  - **AE11xx**「match enum target does not support range labels」：enum 目标类型分支出现区间标签（区间标签仅支持整型）。
  - **AE11xx**「match enum target does not allow mixing enum item labels with literal/value-list/range labels」：enum 目标类型分支混用 enum item 引用与其他形式标签。
- 既有 `CE0211 / CE0270`「match target must be integer, bool, or string」描述更新为「match target must be integer, bool, string, or enum」；触发条件不变（仍由 codegen 层 `cgtype_is_integer(tk) || tk == CG_TYPE_BOOL || tk == CG_TYPE_STRING` 隐式放行 enum，不实际触发，但错误文本需保持一致）。
- 错误码具体编号、是否合并、文案最终口径，先在 `feng-error-codes-*.md` 中确定后再写代码；不在编码阶段临时决定编号。

### 3.4 正确用法示例

以下示例仅作为语法与语义示意，规范定义以 [docs/feng-flow.md](../docs/feng-flow.md) §3.1 与 [docs/feng-enum.md](../docs/feng-enum.md) §3 为准。

#### 3.4.1 单值 enum 匹配（语句形式）

```feng
enum Color {
  Red,
  Green,
  Blue
}

let c: Color = Color.Red;
match c {
  Color.Red { print("red"); }
  Color.Green { print("green"); }
  Color.Blue { print("blue"); }
}
```

#### 3.4.2 值列表 enum 匹配

```feng
enum HttpStatus {
  Ok = 200,
  NotFound = 404,
  InternalError = 500
}

match status {
  HttpStatus.Ok { println("ok"); }
  HttpStatus.NotFound, HttpStatus.InternalError { println("error"); }
  else { println("unknown"); }
}
```

#### 3.4.3 enum 匹配表达式

```feng
let label: string = match status {
  HttpStatus.Ok { "ok"; }
  HttpStatus.NotFound { "not found"; }
  else { "error"; }
};
```

#### 3.4.4 enum 匹配作为块尾表达式参与返回值

```feng
func describe(c: Color): string {
  match c {
    Color.Red { "red"; }
    Color.Green { "green"; }
    else { "blue"; }
  }
}
```

#### 3.4.5 跨模块 enum 引用

跨模块 enum 引用通过 `import` 引入 enum 类型后，在 match 标签位置以短名两段形式 `EnumName.ItemName` 引用，与既有 enum 表达式引用规则一致（见 [docs/feng-module.md](../docs/feng-module.md) §6）。

```feng
import net.HttpStatus;

match code {
  HttpStatus.Ok { /* ... */ }
  HttpStatus.NotFound { /* ... */ }
  else { /* ... */ }
}
```

### 3.5 错误用法示例

以下示例均为编译期报错场景，错误码以 §3.3 最终确定为准。

#### 3.5.1 enum target 出现整型字面量标签

```feng
match c {
  0 { /* ... */ }              // 错：enum 模式标签必须是 Color 的 item 引用
  Color.Green { /* ... */ }
  else { /* ... */ }
}
```

#### 3.5.2 enum target 出现 string / bool 字面量标签

```feng
match c {
  "red" { /* ... */ }          // 错：enum 模式不接受 string 字面量
  Color.Red { /* ... */ }
  else { /* ... */ }
}
```

```feng
match c {
  true { /* ... */ }           // 错：enum 模式不接受 bool 字面量
  Color.Red { /* ... */ }
  else { /* ... */ }
}
```

#### 3.5.3 enum target 出现区间标签

```feng
match c {
  0...2 { /* ... */ }            // 错：enum 不支持区间标签（AE1111）
  Color.Blue { /* ... */ }
  else { /* ... */ }
}
```

#### 3.5.4 enum target 跨 enum 引用

```feng
enum Color { Red, Green, Blue }
enum Shape { Circle, Square }

let c: Color = Color.Red;
match c {
  Shape.Circle { /* ... */ }  // 错：标签 enum 类型与 target enum 类型不一致
  Color.Red { /* ... */ }
  else { /* ... */ }
}
```

#### 3.5.5 enum target 出现 union member 匹配风格的 type 标签

```feng
match c {
  Color { /* ... */ }          // 错：enum 模式不允许 type 标签
  Color.Red { /* ... */ }
  else { /* ... */ }
}
```

#### 3.5.6 引用不存在的 enum item

```feng
match c {
  Color.Purple { /* ... */ }   // 错：Color 没有 Purple 项（AE0404）
  else { /* ... */ }
}
```

#### 3.5.7 enum item 重复（不可达死代码）

```feng
match c {
  Color.Red { /* ... */ }
  Color.Red { /* ... */ }      // 错：与前面分支重叠，不可达（AE1106）
  else { /* ... */ }
}
```

#### 3.5.8 enum target 出现 binding 前缀

```feng
match c {
  x: Color { /* ... */ }       // 错：enum 模式不需要 binding 前缀
  Color.Red { /* ... */ }
  else { /* ... */ }
}
```

#### 3.5.9 enum item 引用不是严格两段形式

```feng
match c {
  net.HttpStatus.Ok { /* ... */ }   // 错：enum item 引用必须严格两段 `EnumName.ItemName`；
                                     //      跨模块引用应通过 `import` 引入后使用短名形式
  else { /* ... */ }
}
```

#### 3.5.10 enum target 与 int / string / bool target 在同一体中混用

```feng
match c {
  Color.Red { /* ... */ }
  "green" { /* ... */ }        // 错：enum 模式不接受非 enum item 引用标签
  else { /* ... */ }
}
```

## 4 实现方案（后代码）

按「parser → semantic → codegen」顺序落地；每层都不增加新的 AST 节点、不新增 runtime 原语、不放宽既有 enum 类型边界。

### 4.1 Parser

#### 4.1.1 复用既有 type label 路径承载 enum item 引用

enum item 引用 `EnumName.ItemName` 与 union member 匹配的多段 qualified type label（如 `pkg.Named`）在 AST 形式上一致：都是 `FENG_MATCH_LABEL_TYPE`，`type_ref` 是多段 `FENG_TYPE_REF_NAMED`。parser 层无法区分二者，也不需要区分——由 semantic 层根据 target 类型分流（target 为 enum 走 enum 匹配，target 为 union-form spec 走 union member 匹配）。

`parse_match_label`（`src/parser/parser.c:2747`）与 `parse_match_label_atom`（`src/parser/parser.c:2688`）**无需修改**，既有 type label 路径已经能正确解析 `EnumName.ItemName` 为多段 named type label。

enum target 的区间标签 `1...10`（整数区间）能被 parser 解析为 `FENG_MATCH_LABEL_RANGE`，semantic 层在 enum 模式下报 AE1111 拒绝；多段 type 形式 `Color.Red...Color.Green` 在 parser 层无法解析（多段 type label 不接受 `...` 后接 type），视为不合法语法，由 parser 报错，不到 semantic 层。

#### 4.1.2 不动既有 union member 匹配路径

`parse_match_label` 中 `is_type_label_start_token` 分支用于 union member 匹配的 `Type { ... }` 形式标签，同时承载 enum item 引用 `EnumName.ItemName`。两条路径在 parser 层合并，在 semantic 层分流。

#### 4.1.3 不动 binding prefix 路径

`parse_match_branch_binding_prefix`（`src/parser/parser.c:2836`）已经识别 `IDENT :`（单段名）并回退多段名；enum item 引用不会被误判为 binding。无需修改。

### 4.2 Semantic Analyzer

#### 4.2.1 放行 enum 作为 match target

`match_target_type_is_allowed`（`src/semantic/analyzer.c:6666`）追加 enum 检查：

- 新增辅助 `inferred_expr_type_is_enum(context, target_type)`（函数已存在于 `src/semantic/analyzer.c:5660`，当前仅在 `enum -> int` 显式转换场景被引用），作为允许条件之一。
- 函数签名需要带 `context`（已有 `ResolveContext`），需要把 `match_target_type_is_allowed` 的所有调用点同步传入 context；当前仅一处调用点（`resolve_and_validate_match_common` 中 `src/semantic/analyzer.c:7336`），改动可控。

#### 4.2.2 扩展 `MatchConstKind` 与 `MatchConstValue`

`MatchConstKind`（`src/semantic/analyzer.c:6554`）新增 `MATCH_CONST_ENUM`：

```c
typedef enum MatchConstKind {
    MATCH_CONST_INT = 0,
    MATCH_CONST_BOOL,
    MATCH_CONST_STRING,
    MATCH_CONST_ENUM
} MatchConstKind;
```

`MatchConstValue`（`src/semantic/analyzer.c:6560`）新增 enum 字段：

```c
typedef struct MatchConstValue {
    MatchConstKind kind;
    int64_t i;
    bool b;
    FengSlice s;
    const FengDecl *enum_decl;   /* enum 类型声明，仅 ENUM kind 有效 */
    FengSlice enum_item_name;   /* enum item 名，仅 ENUM kind 有效 */
    int64_t enum_item_value;    /* enum item 底层值，仅 ENUM kind 有效 */
    FengToken token;
} MatchConstValue;
```

`enum_item_value` 取自 `ensure_enum_decl_info` 写入的 `FengSemanticEnumInfo`（见 `src/semantic/analyzer.c:7705`，已经为每个 item 记录底层值）；通过 `feng_semantic_find_enum_item_info` 查询（`src/semantic/analyzer.c:12120` 已有调用范例）。

#### 4.2.3 不修改 `extract_match_label_literal`

按 §4.1 的设计调整（复用既有 `FENG_MATCH_LABEL_TYPE` 路径），enum item 引用标签的 AST 形式是 `FENG_MATCH_LABEL_TYPE` 而非 `FENG_MATCH_LABEL_VALUE`，因此不经过 `extract_match_label_literal`。enum item 引用的解析（enum decl 查找、item 查找、底层值查询）由 §4.2.6 中新增的 `resolve_and_validate_enum_match_common` 直接在分支遍历时完成，不污染既有 `extract_match_label_literal` 与 `collect_match_branch_label_records` 路径。

#### 4.2.4 扩展 `match_const_kind_matches_target`

`match_const_kind_matches_target`（`src/semantic/analyzer.c:6684`）追加 `MATCH_CONST_ENUM` case：

- 对 `MATCH_CONST_ENUM`，检查 target_type 是否为 enum 类型且 enum decl 与 record 中 enum_decl 一致。
- 该函数当前签名不带 context，对 enum 判定需要 context 才能解析 type_ref 形式的 enum 类型；需要把 context 作为参数传入，调用点同步修改。

#### 4.2.5 扩展 `match_label_records_overlap`

`match_label_records_overlap`（`src/semantic/analyzer.c:6717`）追加 `MATCH_CONST_ENUM` case：

- 两 record 必须同 enum decl（指针相等即可，类型一致是 §4.2.4 已校验的前提）且同 item name（slice 相等）才算重叠。
- 不按底层 int 值比较：不同 enum 的同值 item 不视为重叠（§4.2.6 由 target 类型一致约束保证不会出现同 match 体中混入不同 enum 的情况）。

#### 4.2.6 在 `resolve_and_validate_match_common` 增加 enum 模式约束

`resolve_and_validate_match_common`（`src/semantic/analyzer.c:7260`）在 union member 匹配分支之后、`match_target_type_is_allowed` 检查之前，增加 enum target 模式分支：

- 若 `inferred_expr_type_is_enum(context, target_type)`：
  - 进入 enum 匹配模式；
  - 对每个 branch 的每个 label：
    - `FENG_MATCH_LABEL_RANGE`：报 AE11xx「match enum target does not support range labels」。
    - `FENG_MATCH_LABEL_VALUE`：调用扩展后的 `extract_match_label_literal`，失败时报 AE11xx「match enum target requires enum item reference label」。
    - `FENG_MATCH_LABEL_TYPE`：enum 模式不允许 type label，报 AE11xx（同上文案或单独文案）。
  - 对每个成功解析的 enum record，校验 `record.enum_decl` 与 target 的 enum decl 一致；不一致报 AE11xx「match label references enum '%.*s' but target type is enum '%.*s'」。
  - 校验通过后，记录 MatchLabelRecord（kind = `MATCH_CONST_ENUM`），与既有 `validate_match_label_records` 重叠检测路径对接。
- 不引入 union-form 风格的 binding 前缀：enum 标签分支不支持 `[let|var] name: Type` 绑定形式（语义无意义）；若 parser 已解析到 binding prefix，在 enum 模式下报 AE11xx。

#### 4.2.7 不动既有 union member 匹配路径

union member 匹配走 `resolve_and_validate_union_match_common`（`src/semantic/analyzer.c:7323` 调用点），与 enum 模式互斥（target 类型不同）。两条路径在 `resolve_and_validate_match_common` 顶部按 target 类型分流，互不影响。

### 4.3 Codegen

#### 4.3.1 路径放行

`cg_emit_match_expr`（`src/codegen/codegen.c:17848`）与 `cg_emit_match_stmt`（`src/codegen/codegen.c:21522`）对 target 类型检查位于：

- `cg_emit_match_expr` 中 `src/codegen/codegen.c:17795`：`if (tk != CG_TYPE_BOOL && tk != CG_TYPE_STRING && !cgtype_is_integer(tk))` 触发 `CE0211`。
- `cg_emit_match_stmt` 中 `src/codegen/codegen.c:21678`：同上触发 `CE0270`。

enum 的 `CGType` kind 为 `CG_TYPE_I32`（`cgtype_new_enum`，`src/codegen/codegen.c:210`），`cgtype_is_integer(CG_TYPE_I32)` 返回 true，**已经隐式放行**，无需修改检查条件。

#### 4.3.2 标签发码路径

`cg_emit_match_label_cond`（`src/codegen/codegen.c:17604`）原仅处理 `FENG_MATCH_LABEL_VALUE` 与 `FENG_MATCH_LABEL_RANGE`；enum item 引用标签在 parser 层解析为 `FENG_MATCH_LABEL_TYPE`（两段 named type ref，`EnumName.ItemName`），原 codegen 不覆盖此 AST kind，会触发 `CE0204`「unknown match label kind」。

为此在 `cg_emit_match_label_cond` 新增 `FENG_MATCH_LABEL_TYPE` 分支：

- 取 `lab->type` 的两段 named type ref：segments[0] 为 enum 名、segments[1] 为 item 名。
- 通过 `cg_find_visible_enum_decl(cg, segments[0].data, segments[0].length)` 解析 enum decl（语义层已校验存在性与可见性，此处不会失败）。
- 调用 `cg_ensure_enum_emitted(cg, enum_decl)` 确保 enum 的 C typedef 与 item 常量已发出。
- 在 enum decl 中查找 segments[1] 对应的 item（语义层已校验存在性，此处不会失败）。
- 调用 `cg_enum_item_c_name`（`src/codegen/codegen.c:3495`）生成 `EnumTypedef__ItemName` 常量名。
- 发出 `(bool)(_mt == EnumTypedef__ItemName)`，与既有 int 标签发码路径一致。

不新增 AST kind、不新增 MatchLabelKind、不新增 runtime 原语；仅复用既有 enum 发码原语（`cg_enum_item_c_name`、`cg_ensure_enum_emitted`）。

#### 4.3.3 区间标签保护

`cg_emit_match_label_cond` 中 `FENG_MATCH_LABEL_RANGE` 分支已经检查 `target_kind == CG_TYPE_STRING || target_kind == CG_TYPE_BOOL` 时报 `CE0203`「range labels apply to integer match targets only」。enum 的 target_kind 为 `CG_TYPE_I32`，不会触发该检查；但 §4.2.6 已在语义层禁止 enum 区间标签，codegen 阶段不会收到 enum range label。保持现状即可。

#### 4.3.4 错误信息更新

- `CE0211`（`src/codegen/codegen.c:17797` 与 `:18175`）与 `CE0270`（`src/codegen/codegen.c:21683`）的文案「match target must be integer, bool, or string」更新为「match target must be integer, bool, string, or enum」，与放宽后的允许集合保持一致。
- 文案更新仅做描述对齐，不改变触发条件（仍由 `cgtype_is_integer(tk) || tk == CG_TYPE_BOOL || tk == CG_TYPE_STRING` 隐式放行 enum，对其他不允许类型仍触发）。

### 4.4 Symbol Table / 包导出 / LSP

- enum 类型与 enum item 的导出、跨模块引用、`EnumName.ItemName` 解析能力已由 enum 首版交付（见 [dev/feng-enum-delivered.md](./feng-enum-delivered.md) §2.5）完整支持；match enum 模式只是消费侧，不需要扩展 `.ft` 格式或查询视图。
- LSP completion / hover / definition 在 `EnumName.ItemName` 标签位置的行为已由 enum 首版的 LSP 支持覆盖；不需要新增 LSP 能力。
- 复跑既有 `test_symbol` / `test_cli` 验证无回归。

## 5 错误码与诊断

| 错误码 | 触发场景 | 现状 | 变更 |
| ------ | -------- | ---- | ---- |
| AE0050 | match 目标类型不在允许集合 | 文案「allowed types are integers, 'string' and 'bool'」 | 文案更新为「integers, 'string', 'bool' and enum」；enum 不再触发 |
| AE0404 | `EnumName.ItemName` 中 ItemName 不存在 | 已支持 | 复用，不新增 |
| AE1103 | 区间标签端点非整型 | 已支持 | enum target 不允许区间，由 §4.2.6 提前拦截 |
| AE1105 | 标签非字面量或 `let` 绑定到字面量 | 已支持 | 对 enum target，标签解析走 §4.2.3 新路径，失败时改报 AE11xx（enum 专用文案） |
| AE1106 | 标签重叠不可达 | 已支持 | 复用，覆盖 enum item 重复 |
| AE11xx (新) | enum target 标签非 enum item 引用 | 不存在 | 新增 |
| AE11xx (新) | enum target 标签跨 enum | 不存在 | 新增 |
| AE11xx (新) | enum target 出现区间标签 | 不存在 | 新增 |
| AE11xx (新) | enum target 出现 type 标签 / binding 前缀 | 不存在 | 新增 |
| CE0211 / CE0270 | codegen target 类型检查 | 文案「integer, bool, or string」 | 文案更新为「integer, bool, string, or enum」；触发条件不变 |

具体错误码编号、文案口径、是否合并，先在 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md) 与 [docs/feng-error-codes-ce.md](../docs/feng-error-codes-ce.md) 中确定，再落到代码。

## 6 影响范围分析

### 6.1 改动规模概述

- parser：无修改；既有 `FENG_MATCH_LABEL_TYPE` 路径已能解析 `EnumName.ItemName` 为两段 named type label。
- semantic：`match_target_type_is_allowed`、`MatchConstKind` / `MatchLabelRecord`、`match_const_kind_matches_target`、`match_label_records_overlap` 共约 +80 行；`resolve_and_validate_enum_match_common` 新增约 +190 行；移除无 context 版本 `inferred_expr_type_is_enum_kind`。
- codegen：错误信息文案更新 3 处；`cg_emit_match_label_cond` 新增 `FENG_MATCH_LABEL_TYPE` 分支约 +50 行。
- 文档：`feng-flow.md`、`feng-enum.md`、`feng-error-codes-*.md` 文案更新与新错误码条目。
- 测试：parser / semantic / codegen 新增 enum match 用例；既有 `if_match_*` / `match_*` 测试不受影响。

### 6.2 各文件改动规模

| 文件 | 改动类型 | 规模 |
| ---- | -------- | ---- |
| `docs/feng-flow.md` | §3 / §3.1 文案与示例 | +10 / -3 行 |
| `docs/feng-enum.md` | §3 / §7 文案 | +3 行 |
| `docs/feng-error-codes-ae.md` | AE0050 文案、新增 AE11xx 条目 | +6 行 |
| `docs/feng-error-codes-ce.md` | CE0211 / CE0270 文案 | 2 处 |
| `docs/feng-error-codes.md` | 索引同步 | 2 处 |
| `src/parser/parser.c` | 不动 | 0 |
| `src/semantic/analyzer.c` | enum 模式分支、MatchConstKind 扩展、辅助函数签名 | +270 / -20 行 |
| `src/codegen/codegen.c` | CE0211 / CE0270 文案 + `cg_emit_match_label_cond` 新增 TYPE 分支 | +55 行 |
| `src/codegen/codegen.c` | CE0211 / CE0270 文案 | 3 处 |
| `src/parser/dump.c` / `src/symbol/export.c` / `src/cli/lsp/runtime.c` / `src/semantic/reifiable_deps.c` | 不动 | 0 |

### 6.3 不动既有路径

- AST 节点 `FengMatchLabel`、`FengMatchBranch`、`FENG_STMT_MATCH` / `FENG_EXPR_MATCH` 不变。
- 既有「字面量、值列表、整数区间」常量相等性匹配路径不变。
- union member 匹配路径不变。
- enum 的运行时表示、`.ft` 导出、ABI 处理不变。
- enum 与 `int` 之间的转换边界不变。

## 7 测试计划

### 7.1 parser 单元测试

在 `test/parser/test_parser.c` 中新增用例：

- `EnumName.ItemName { ... }` 形式单值标签解析。
- 值列表 `EnumName.Item1, EnumName.Item2 { ... }` 解析。
- `EnumName.Item1, EnumName.Item2, EnumName.Item3` 多标签解析。
- 非法形式：`EnumName.Item1.EnumName2.Item2`（多段 member access）解析报错。

### 7.2 semantic 单元测试

在 `test/semantic/test_semantic.c` 中新增用例：

- 合法 enum match：单值、值列表、`else` 分支。
- enum match 表达式（带返回值）。
- enum target 标签跨 enum：报 AE11xx。
- enum target 标签非 enum item 引用（如 int 字面量、string 字面量）：报 AE11xx。
- enum target 出现区间标签：报 AE11xx。
- enum target 标签引用不存在的 item：复用 AE0404。
- enum item 重复：复用 AE1106。
- enum match 与 int / bool / string match 不混用：在同一体中混用报 AE11xx。
- 跨模块 enum 引用：通过 `import` 后 `EnumName.ItemName` 标签可解析。

### 7.3 codegen 单元测试

在 `test/codegen/test_codegen.c` 中新增用例：

- enum match 语句生成 `(bool)(_mt == EnumName__ItemName)` 形式。
- enum match 表达式发码正确返回分支结果。
- 跨模块 enum item 引用在 codegen 阶段发出正确的限定 C 常量名。

### 7.4 cts / fcts 端到端

- 在 `cts/` 或 `fcts/fcts_bin/src/test_flow.ff` 中追加 enum match 端到端用例，覆盖单值、值列表、表达式形式、跨模块引用。
- 既有 `match_*` 测试不受影响。

### 7.5 全量回归

- 阶段性执行 `make build/bin/test_parser && build/bin/test_parser`、`make build/bin/test_semantic && build/bin/test_semantic`、`make build/bin/test_codegen && build/bin/test_codegen`。
- 最终执行 `make test`。

## 8 分步任务清单

按依赖顺序执行；每步完成后必须执行全量回归 `make test` 全部通过，方可勾选完成并进入下一步。每步都设计为「独立可交付」切片：落地后既有测试无回归，且本步新增能力可被对应窄测试覆盖。

> 单步内部子项不要求独立通过回归，但单步整体必须通过。每步都标注「前置依赖」与「全量回归点」。

### 8.1 步骤 1：规范与错误码文档变更

- 前置依赖：无
- 范围：仅文档变更，无代码改动

- [ ] 更新 [docs/feng-flow.md](../docs/feng-flow.md) §3 与 §3.1：常量相等性匹配目标类型集合追加 `enum`；标签形式列表中「单值」「值列表」追加 enum item 引用规则；「整数闭区间」保留整型限定并明确 enum 不支持区间；编译器交叉检测规则追加 enum item 重复判定；追加 enum 匹配代码示例
- [ ] 更新 [docs/feng-enum.md](../docs/feng-enum.md) §3 与 §7：语义条目追加「可作为 match 常量相等性匹配目标」，关联列表追加指向 `feng-flow.md` 的链接（不在 `feng-enum.md` 中重复 match 标签规则）
- [ ] 更新 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md)：`AE0050` 文案允许集合追加 `enum`；新增 AE11xx 条目（enum target 标签非 enum item 引用、跨 enum、区间标签、混用、type 标签、binding 前缀）；错误码编号与文案最终口径由人工审定
- [ ] 更新 [docs/feng-error-codes-ce.md](../docs/feng-error-codes-ce.md)：`CE0211` / `CE0270` 文案「integer, bool, or string」更新为「integer, bool, string, or enum」
- [ ] 更新 [docs/feng-error-codes.md](../docs/feng-error-codes.md)：索引同步
- [ ] 全量回归点：`make test` 通过（仅文档变更，无代码行为变化）

### 8.2 步骤 2：Parser 复用既有 type label 路径承载 enum item 引用

- 前置依赖：步骤 1
- 范围：parser 无需修改；既有 `FENG_MATCH_LABEL_TYPE` 路径已能解析 `EnumName.ItemName` 为多段 named type label；新增 parser AST 测试验证解析结果
- 设计调整：原方案「扩展 `parse_match_label_atom` 支持 member access」会破坏既有 union member 匹配对多段 qualified type name（如 `pkg.Named`）的支持；改为复用既有 type label 路径，semantic 层根据 target 类型分流

- [x] 确认 `parse_match_label`（`src/parser/parser.c:2747`）与 `parse_match_label_atom`（`src/parser/parser.c:2688`）无需修改
- [x] 在 `test/parser/test_parser.c` 新增 AST 结构测试用例：验证 `Color.Red` 与 `Color.Green, Color.Blue` 被解析为 `FENG_MATCH_LABEL_TYPE`，`type_ref` 为两段 `FENG_TYPE_REF_NAMED`（segments[0]="Color"、segments[1]="Red"）
- [x] 全量回归点：`make build/bin/test_parser && build/bin/test_parser` 通过；`make test` 全量回归通过

### 8.3 步骤 3：Semantic 扩展支持 enum 匹配模式

- 前置依赖：步骤 2
- 范围：semantic 全面扩展支持 enum 模式
- 行为变化：enum item 引用标签从「semantic 报 AE1105」变为「合法通过 semantic」；既有测试不使用 enum match，无回归

- [x] 扩展 `MatchConstKind`（`src/semantic/analyzer.c:6554`）新增 `MATCH_CONST_ENUM`
- [x] 扩展 `MatchConstValue`（`src/semantic/analyzer.c:6560`）新增 `enum_decl` / `enum_item_name` / `enum_item_value` 字段
- [x] 扩展 `match_target_type_is_allowed`（`src/semantic/analyzer.c:6666`）：追加 `inferred_expr_type_is_enum(context, target_type)` 判定；签名带 `context`，调用点（`src/semantic/analyzer.c:7336`）同步修改；移除无 context 版本 `inferred_expr_type_is_enum_kind`，因 enum target 既可能是 `FENG_INFERRED_EXPR_TYPE_DECL` 也可能是 `FENG_INFERRED_EXPR_TYPE_TYPE_REF`，必须带 context 解析
- [x] 扩展 `match_const_kind_matches_target`（`src/semantic/analyzer.c:6684`）：新增 `MATCH_CONST_ENUM` case，校验 target 为 enum；签名带 `context`
- [x] 扩展 `match_label_records_overlap`（`src/semantic/analyzer.c:6717`）：新增 `MATCH_CONST_ENUM` case，按 enum decl 指针相等 + item name slice 相等判定
- [x] 在 `resolve_and_validate_match_common`（`src/semantic/analyzer.c:7260`）增加 enum 模式分支：禁止 `FENG_MATCH_LABEL_RANGE`、要求标签为 `FENG_MATCH_LABEL_TYPE` 形式的两段 named type ref、禁止 binding prefix、校验 enum decl 一致、禁止与字面量/值列表/区间标签混用
- [x] 在 `test/semantic/test_semantic.c` 新增用例：合法 enum match（单值、值列表、表达式形式、块尾表达式）、跨 enum 报错、range 标签报错、不存在的 item 报错（复用 AE0404）、enum item 重复报错（复用 AE1106）、与 int/string 字面量混用报错、binding 前缀报错
- [x] 全量回归点：`make build/bin/test_semantic && build/bin/test_semantic` 通过；`make test` 全量回归通过

### 8.4 步骤 4：Codegen 文案更新与发码验证

- 前置依赖：步骤 3
- 范围：codegen 错误信息文案更新 + codegen 发码扩展（enum item 引用标签走 `FENG_MATCH_LABEL_TYPE` 路径，原 `FENG_MATCH_LABEL_VALUE` 不覆盖该 AST kind，需要新增 codegen 分支）+ codegen 发码测试

- [x] 更新 `src/codegen/codegen.c` 中 `CE0211`（2 处：`:17797` 与 `:18175`）文案为「match target must be integer, bool, string, or enum」
- [x] 更新 `src/codegen/codegen.c` 中 `CE0270`（1 处：`:21683`）文案为「match target must be integer, bool, string, or enum」
- [x] 在 `cg_emit_match_label_cond`（`src/codegen/codegen.c:17604`）新增 `FENG_MATCH_LABEL_TYPE` 分支：解析两段 named type ref 的 segments[0] 为 enum decl（复用 `cg_find_visible_enum_decl`），查找 segments[1] 为该 enum 的 item，复用 `cg_enum_item_c_name` 生成 `EnumTypedef__ItemName` 常量，发出 `(bool)(_mt == EnumTypedef__ItemName)`
- [x] 在 `test/codegen/test_codegen.c` 新增用例：enum match 语句发码、enum match 表达式发码
- [x] 全量回归点：`make build/bin/test_codegen && build/bin/test_codegen` 通过；`make test` 全量回归通过

### 8.5 步骤 5：cts / fcts 端到端用例

- 前置依赖：步骤 4
- 范围：端到端测试套件追加 enum match 用例

- [x] 在 `fcts/fcts_bin/src/test_enum.ff` 中追加 enum match 端到端用例：覆盖单值、值列表、语句形式、显式取值 enum 匹配
- [x] 全量回归点：fcts 套件通过（237 全部通过）；`make test` 全量回归通过

### 8.6 步骤 6：最终全量回归与收尾

- 前置依赖：步骤 1–5 全部完成
- 范围：最终验收与一致性复核

- [x] 执行 `make test` 全量回归通过（233 → 237 全部通过）
- [x] 复核所有新增测试用例与既有测试用例无冲突
- [x] 复核 `docs/feng-flow.md` / `docs/feng-enum.md` / `docs/feng-error-codes-*.md` 与代码实现一致
- [x] 复核 `dev/feng-match-enum-dev.md` 中实现方案与最终代码一致（行号、函数名、错误码编号如有调整需回写文档）
- [x] 准备建议 commit message，由开发者自行提交

## 9 风险评估

| 风险 | 等级 | 缓解措施 |
| ---- | ---- | -------- |
| `parse_match_label_atom` 扩展 member access 后与既有 union member 匹配的 type label 路径冲突 | 低 | type label 路径走 `is_type_label_start_token` + `parse_type_ref`，对 `IDENTIFIER . IDENTIFIER` 形式会在 `parse_type_ref` 后判断 `parser_check(COMMA)` / `LBRACE` 不成立时回退到 atom 路径；atom 内部识别 member access 后由语义层判定 enum 还是 union，互斥分流 |
| `MatchConstKind` 扩展后既有 int / bool / string overlap 检测受影响 | 低 | 新增 ENUM case 独立处理，既有 case 行为不变；overlap 比较仍按 kind 严格匹配，跨 kind 不重叠 |
| enum target 与 int 字面量在同一体中混用导致底层 int 值与 enum item 引用语义冲突 | 低 | §4.2.6 在语义层禁止混用，codegen 不会收到混合标签体 |
| 跨模块 enum item 引用在 match 标签位置解析失败 | 低 | enum 跨模块引用已由 enum 首版交付（见 [dev/feng-enum-delivered.md](./feng-enum-delivered.md) §2.5），match 仅消费既有查询视图 |
| `inferred_expr_type_is_enum` 在 type_ref 形式 enum 上需要 context 解析 | 低 | 函数已存在并支持 type_ref / type_decl 两种形式（见 `src/semantic/analyzer.c:5660`），调用点已有 context 可传 |
| 新增 AE11xx 错误码与既有错误码编号冲突 | 低 | 具体编号在 `feng-error-codes-ae.md` 中先确定再写代码，遵循既有编号区间规则 |
| 既有 `match_*` 测试受影响 | 低 | 仅扩展允许集合，既有 int / bool / string match 测试语义不变 |
| enum target 与 `cgtype_is_integer` 隐式放行导致的边界 case | 低 | enum 的 CGType kind 为 `CG_TYPE_I32`，codegen 路径与 int 路径一致；既有 int match 测试覆盖该路径 |

## 10 当前明确不做

- 不支持 enum 区间匹配（区间仅整型）。
- 不支持 enum 与 int 字面量在同一体中混用。
- 不支持不同 enum 类型在同一体中混用。
- 不支持 enum item 引用与 binding 前缀（`[let|var] name: EnumType { ... }`）的组合（enum 模式不需要收窄）。
- 不引入 enum payload、字段、方法（超出 enum 当前规范边界，见 [docs/feng-enum.md](../docs/feng-enum.md) §3）。
- 不引入新的 AST 节点、新的 MatchLabelKind、新的 runtime 原语。
- 不放宽 enum 与 int 之间的转换边界。

## 11 交付约束

- 所有实现必须以 [docs/feng-flow.md](../docs/feng-flow.md) §3.1 与 [docs/feng-enum.md](../docs/feng-enum.md) §3 为准，不得在编码阶段临时放宽或收紧。
- enum match 必须保持「enum item 引用为唯一合法标签形式」的定位，不得偷渡成「底层 int 值匹配」或「int 字面量与 enum item 混用」。
- 不得引入新的 runtime 对象表示、runtime API 或额外值模型分类。
- 进入代码实现前，必须先在 [docs/feng-flow.md](../docs/feng-flow.md) 与 [docs/feng-enum.md](../docs/feng-enum.md) 中写清 enum match 的语义边界与标签形式，不得边写代码边临时决定。
- 错误码编号与文案必须先在 [docs/feng-error-codes-ae.md](../docs/feng-error-codes-ae.md) / [feng-error-codes-ce.md](../docs/feng-error-codes-ce.md) 中确定，再落到代码。
- 每个阶段都以「先文档、后代码、再测试」的顺序落地，不修改既有测试语义，只新增覆盖。
- 若实现过程中发现规范仍有缺口，先回写对应权威文档，再继续编码。

## 建议 commit message

```text
feat(match): support enum value matching

Extend match constant equality matching to accept enum types as the
target. Branch labels must be enum item references of the form
`EnumName.ItemName`, and the referenced enum must match the target's
enum type. Enum targets do not support range labels or mixing with
integer/string/bool literal labels.

The change reuses the existing `FENG_MATCH_LABEL_VALUE` AST node and
the existing `cg_emit_expr` codegen path for member access, so no new
AST kind, MatchLabelKind, or runtime primitive is introduced.
```
