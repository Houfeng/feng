# Feng LSP 关键字与注解补全优化

> 关联文档：[dev/feng-lsp-delivered.md](feng-lsp-delivered.md)
> 本文档定义 Feng LSP 关键字上下文感知补全、注解补全、Snippet 支持的设计方案与实施任务。

---

## 1. 目标

当前 LSP 已支持局部变量、模块成员、成员访问（`.`）、import 路径等补全，但缺少以下能力：

1. **关键字补全**：输入时不提示 Feng 关键字（`func`、`let`、`if` 等）。
2. **注解补全**：输入 `@` 时不提示内置注解（`@abi`、`@runtime` 等）。
3. **Snippet 支持**：关键字补全不附带代码片段模板。

本方案分阶段交付上述能力，核心原则：

- **上下文感知**：根据光标所在位置（顶层 / 类型体 / 函数体）提供合适的关键字子集，不做全量堆砌。
- **复用现有基础设施**：关键字补全和注解补全的数据均在独立头文件中声明，与编译器内部表（`FENG_KEYWORD_LIST`、`feng_builtin_annotations()`）有意独立，避免 LSP 对编译器内部的耦合。
- **最小侵入**：在现有 completion 流程的非成员分支末尾追加关键字项，不改变现有补全逻辑。
- **数据与逻辑分离**：关键字集合、注解集合、snippet 模板声明在独立头文件（`lsp_keywords.h`、`lsp_annotations.h`）中，C 逻辑仅遍历表结构，新增或调整关键字/注解/snippet 时只改数据文件，不改逻辑代码。
- **禁止改动核心编译器**：所有变更仅限 `src/cli/lsp/` 目录，不得修改 `src/lexer/`、`src/parser/`、`src/semantic/`、`src/codegen/`、`src/symbol/` 等核心编译器模块，仅通过已有公开 API 消费编译器能力。

---

## 2. 关键字位置分类

### 2.1 语法位置枚举 `FengLspPosition`

```c
typedef enum {
    FENG_LSP_POS_OTHER,    /* 未分类位置（零初始化默认值） */
    FENG_LSP_POS_TOP_DECL, /* 模块顶层（声明位置） */
    FENG_LSP_POS_TOP_BIND, /* 顶层绑定（全局 let/var 初始化表达式） */
    FENG_LSP_POS_MEMBER,   /* type/spec/fit 声明体内部（成员声明位置） */
    FENG_LSP_POS_BODY      /* 函数/方法体内部（语句位置） */
} FengLspPosition;
```

> `FengLspPosition` 是 `FengLspCompletionContext` 的字段，描述光标在文档中的语法位置，
> 与现有字段（`is_member`、`prefix` 等描述正在输入的状态）正交，互不冲突。
> 关键字补全是第一个消费方，未来类型补全、修饰符补全等也可消费此字段。

`OTHER` 为零初始化默认值（C 结构体 `{0}` 时 `position == 0 == OTHER`），
覆盖成员访问、import 路径、enum 体等未归入已分类位置的场景。
消费方在 `position == OTHER` 时不追加关键字。

### 2.2 各位置关键字集合

> 下表中标注 `(变体)` 的项是同一关键字的不同 Snippet 模板形式，
> 不是独立关键字。在 §2.4 补全项表中作为独立补全项列出。
> 各位置计数为补全项总数（含变体）。

**TOP_DECL**（14 个）：

| 关键字          | 说明           |
| --------------- | -------------- |
| `module`        | 模块声明       |
| `import`        | 导入声明       |
| `func`          | 函数声明       |
| `type`          | 对象类型声明   |
| `type` *(变体)* | 元组类型声明   |
| `enum`          | 枚举声明       |
| `spec`          | 规约声明       |
| `spec` *(变体)* | callable 规约  |
| `spec` *(变体)* | union 规约     |
| `fit`           | 适配声明       |
| `extern`        | 外部声明       |
| `open`          | 可见性修饰     |
| `seal`          | 可见性修饰     |
| `as`            | 导入别名       |

**TOP_BIND**（17 个）：

| 关键字        | 说明             |
| ------------- | ---------------- |
| `let`         | 全局不可变绑定   |
| `var`         | 全局可变绑定     |
| `open`        | 可见性修饰       |
| `seal`        | 可见性修饰       |
| `extern`      | 外部声明         |
| `if`          | 条件表达式       |
| `if` *(变体)* | 条件+else 表达式 |
| `else`        | 条件分支         |
| `match`       | 模式匹配         |
| `while`       | 循环             |
| `for`         | 循环             |
| `for` *(变体)*| for/in 迭代      |
| `in`          | for/in 迭代      |
| `try`         | 异常捕获         |
| `catch`       | 异常处理         |
| `unknown`     | 未知值           |
| `void`        | 空类型           |

> 说明：`let x = ...` 初始化表达式支持所有合法表达式，
> 因此 TOP_BIND 包含表达式关键字（`if`、`match`、`while` 等）。
> 与 BODY 的区别：不包含语句级关键字（`return`、`break`、`continue`、`throw`、`defer`），
> 这些仅在函数体内合法。

**MEMBER**（6 个）：

> 适用于 `type`、`spec`（object form）、`fit` 声明体内部。
> `enum` 体内部不使用关键字（enum item 为纯标识符），
> 分类为 `OTHER`（见 §3.1）。

| 关键字   | 说明       |
| -------- | ---------- |
| `func`   | 方法声明   |
| `let`    | 不可变字段 |
| `var`    | 可变字段   |
| `static` | 静态成员修饰 |
| `open`   | 可见性修饰 |
| `seal`   | 可见性修饰 |

**BODY**（19 个）：

| 关键字        | 说明             |
| ------------- | ---------------- |
| `let`         | 局部不可变绑定   |
| `var`         | 局部可变绑定     |
| `if`          | 条件语句/表达式  |
| `if` *(变体)* | 条件+else        |
| `else`        | 条件分支         |
| `match`       | 模式匹配         |
| `while`       | 循环             |
| `for`         | 循环             |
| `for` *(变体)*| for/in 迭代      |
| `in`          | for/in 迭代      |
| `break`       | 跳出循环         |
| `continue`    | 继续循环         |
| `return`      | 返回             |
| `throw`       | 抛出异常         |
| `try`         | 异常捕获         |
| `catch`       | 异常处理         |
| `defer`       | 延迟执行         |
| `unknown`     | 未知值           |
| `void`        | 空类型           |

### 2.3 排除项

- **保留字**（`class`、`struct`、`const`、`export`、`prop`）不纳入补全，当前不使用。
- **`self`** 已由 `collect_visible_locals_for_completion` 作为局部变量提供，在 BODY 位置中不再重复添加。
- **`true` / `false` / `none`** 不属于关键字（分别是 `FENG_TOKEN_BOOL` 和标准库函数），不通过关键字补全提供。
- **`enum` 体内部**不提供关键字补全（enum item 为纯标识符，如 `enum Color { Red, Green, Blue }`），位置分类为 `OTHER`。

### 2.4 声明式头文件 `lsp_keywords.h`

> 关键字集合与 snippet 模板统一定义在独立头文件中，C 逻辑仅遍历表结构。
> 新增或调整关键字/snippet 时只改此文件，不改逻辑代码。

**文件路径**：`src/cli/lsp/lsp_keywords.h`

**数据结构**：

```c
/* 单个关键字/ snippet 项 */
typedef struct {
    const char *label;      /* 关键字文本，如 "func" */
    const char *detail;     /* 说明文本，如 "function declaration" */
    const char *snippet;    /* Snippet 模板，NULL 表示纯文本项 */
} LspKwItem;

/* 位置关键字表 */
typedef struct {
    const LspKwItem *items;
    size_t count;
} LspKwTable;
```

**各位置表声明**（内容对应 §2.2 各表）：

```c
/* TOP_DECL (§2.2) */
static const LspKwItem TOP_DECL_KWS[] = {
    { "module", "module declaration",     "module ${1:name}" },
    { "import", "import declaration",     "import ${1:path}" },
    { "func",   "function declaration",   "func ${1:name}(${2:params}): ${3:void} {\n\t$0\n}" },
    { "type",   "object type declaration","type ${1:Name} {\n\t$0\n}" },
    { "type-tuple", "tuple type declaration", "type ${1:Name}(${2:types});" },
    { "enum",   "enum declaration",       "enum ${1:Name} {\n\t$0\n}" },
    { "spec",   "spec declaration",       "spec ${1:Name} {\n\t$0\n}" },
    { "spec-callable", "callable spec",   "spec ${1:Name}(${2:params}): ${3:void};" },
    { "spec-union", "union spec",         "spec ${1:Name}: ${2:T1} | ${0:T2};" },
    { "fit",    "fit declaration",        "fit ${1:Name} {\n\t$0\n}" },
    { "extern", "external declaration",   NULL },
    { "open",   "visibility modifier",    NULL },
    { "seal",   "visibility modifier",    NULL },
    { "as",     "import alias",           NULL },
};

/* TOP_BIND (§2.2) */
static const LspKwItem TOP_BIND_KWS[] = {
    { "let",     "immutable binding",  "let ${1:name}: ${2:type} = ${0:value}" },
    { "var",     "mutable binding",    "var ${1:name}: ${2:type} = ${0:value}" },
    { "open",    "visibility modifier", NULL },
    { "seal",    "visibility modifier", NULL },
    { "extern",  "external declaration",NULL },
    { "if",      "conditional",         "if ${1:condition} {\n\t$0\n}" },
    { "if-else", "conditional+else",    "if ${1:condition} {\n\t$2\n} else {\n\t$0\n}" },
    { "else",    "else branch",         NULL },
    { "match",   "pattern matching",    "match ${1:target} {\n\t$0\n}" },
    { "while",   "while loop",          "while ${1:condition} {\n\t$0\n}" },
    { "for",     "for loop",            "for ${1:var i = 0}; ${2:i < n}; ${3:i = i + 1} {\n\t$0\n}" },
    { "for-in",  "for/in iteration",    "for ${1:let it} in ${2:iterable} {\n\t$0\n}" },
    { "in",      "for/in keyword",      NULL },
    { "try",     "exception handling",  "try {\n\t$1\n} catch ${2:err} {\n\t$0\n}" },
    { "catch",   "exception handler",   NULL },
    { "unknown", "unknown value",       NULL },
    { "void",    "void type",           NULL },
};

/* MEMBER (§2.2) */
static const LspKwItem MEMBER_KWS[] = {
    { "func",   "method declaration",  "func ${1:name}(${2:params}): ${3:void} {\n\t$0\n}" },
    { "let",    "immutable field",     "let ${1:name}: ${2:type}" },
    { "var",    "mutable field",       "var ${1:name}: ${2:type}" },
    { "static", "static modifier",     NULL },
    { "open",   "visibility modifier", NULL },
    { "seal",   "visibility modifier", NULL },
};

/* BODY (§2.2) */
static const LspKwItem BODY_KWS[] = {
    { "let",      "local immutable binding", "let ${1:name}: ${2:type} = ${0:value}" },
    { "var",      "local mutable binding",   "var ${1:name}: ${2:type} = ${0:value}" },
    { "if",       "conditional",             "if ${1:condition} {\n\t$0\n}" },
    { "if-else",  "conditional+else",        "if ${1:condition} {\n\t$2\n} else {\n\t$0\n}" },
    { "else",     "else branch",             NULL },
    { "match",    "pattern matching",        "match ${1:target} {\n\t$0\n}" },
    { "while",    "while loop",              "while ${1:condition} {\n\t$0\n}" },
    { "for",      "for loop",                "for ${1:var i = 0}; ${2:i < n}; ${3:i = i + 1} {\n\t$0\n}" },
    { "for-in",   "for/in iteration",        "for ${1:let it} in ${2:iterable} {\n\t$0\n}" },
    { "in",       "for/in keyword",          NULL },
    { "break",    "break loop",              NULL },
    { "continue", "continue loop",           NULL },
    { "return",   "return from function",    NULL },
    { "throw",    "throw exception",         NULL },
    { "try",      "exception handling",      "try {\n\t$1\n} catch ${2:err} {\n\t$0\n}" },
    { "catch",    "exception handler",       NULL },
    { "defer",    "deferred execution",      "defer {\n\t$0\n}" },
    { "unknown",  "unknown value",           NULL },
    { "void",     "void type",               NULL },
};
```

**位置索引表**：

```c
static const LspKwTable KW_TABLE[] = {
    [FENG_LSP_POS_OTHER]    = { NULL, 0 },
    [FENG_LSP_POS_TOP_DECL] = { TOP_DECL_KWS,  sizeof(TOP_DECL_KWS)  / sizeof(TOP_DECL_KWS[0])  },
    [FENG_LSP_POS_TOP_BIND] = { TOP_BIND_KWS,  sizeof(TOP_BIND_KWS)  / sizeof(TOP_BIND_KWS[0])  },
    [FENG_LSP_POS_MEMBER]   = { MEMBER_KWS,    sizeof(MEMBER_KWS)    / sizeof(MEMBER_KWS[0])    },
    [FENG_LSP_POS_BODY]     = { BODY_KWS,      sizeof(BODY_KWS)      / sizeof(BODY_KWS[0])      },
};
```

> 阶段三（Snippet）启用后，`append_context_keyword_items` 统一遍历此表：
> `snippet != NULL` 的项使用 `append_completion_item_snippet`，
> `snippet == NULL` 的项使用 `append_completion_item`（纯文本）。
> 阶段一、二（纯关键字、无 Snippet）暂不使用 `snippet` 字段，所有项以纯文本方式追加。
> `position == FENG_LSP_POS_OTHER` 时 `KW_TABLE[OTHER]` 为 `{NULL, 0}`，不追加任何项。

**与 `token.h` 的关系**：

`lsp_keywords.h` 中的 `label` 字符串与 `token.h` 的 `FENG_KEYWORD_LIST` 文本一致，
但属于有意重复——两者职责不同：

| 维度 | `token.h`（`FENG_KEYWORD_LIST`） | `lsp_keywords.h`（`KW_TABLE`） |
|---|---|---|
| 职责 | 词法分析器识别关键字 | LSP 补全提供位置感知的关键字列表 |
| 内容 | 关键字文本 + token kind | 关键字文本 + 位置分组 + detail + snippet |
| 变体项 | 无（`if-else`、`for-in` 不是真正关键字） | 有（作为独立补全项） |

`lsp_keywords.h` 不依赖 `token.h` 的 API，保持独立声明，
避免因补全需求变更而引入对词法分析器内部的依赖。

---

## 3. 位置判定

### 3.0 `FengLspCompletionContext` 与 `position` 字段

`FengLspCompletionContext` 现有字段描述**正在输入的状态**，新增 `position` 字段描述**光标所在的语法位置**，两者正交，互不冲突：

| 字段类别 | 字段 | 描述 |
|---|---|---|
| 正在输入的状态 | `is_member`、`is_static_access`、`object`、`prefix`、`literal_builtin_name` | 用户正在输入什么 |
| 所在位置 | `position`（新增） | 光标在文档中的语法位置 |

更新后的结构：

```c
typedef struct FengLspCompletionContext {
    bool is_member;
    bool is_static_access;
    FengSlice object;
    FengSlice prefix;
    FengSlice literal_builtin_name;
    FengLspPosition position;   /* 新增：语法位置 */
} FengLspCompletionContext;
```

`position` 由两阶段填充：

1. `completion_context_from_text`（文本路径）：设置初步位置
2. `build_completion_json` 内部（AST 路径）：用 `enclosing_decl`/`enclosing_member` 精确覆盖

关键字补全消费 `position`：非成员分支末尾根据 `completion_context.position` 从 `KW_TABLE` 取出对应关键字子集追加。`position == OTHER` 时不追加。

```text
build_completion_json 入口
  ├── completion_context.is_member == true  → 成员补全分支（不消费 position）
  └── completion_context.is_member == false → 非成员分支
        ├── locals + module members + imports（已有）
        └── append_context_keyword_items(completion_context.position)（新增）
```

### 3.1 AST 路径（优先）

当 `build_completion_json` 或 `build_cached_completion_json` 能获取到有效 AST 时，使用以下规则：

```text
输入：enclosing_decl, enclosing_member

前置条件：调用方已在进入此函数前排除了成员访问（is_member）和 import 路径
（is_use_path）上下文——前者走成员分支，后者已由 extract_use_path_context
提前返回，均不会到达关键字分类逻辑。

if enclosing_decl == NULL → TOP_DECL
if enclosing_member != NULL:
    if enclosing_member 是 callable（method/constructor/finalizer）:
        → BODY
    else:
        → MEMBER（field 内部或声明位置）
else:
    if enclosing_decl->kind == FENG_DECL_FUNCTION → BODY
    if enclosing_decl->kind == FENG_DECL_GLOBAL_BINDING → TOP_BIND
    if enclosing_decl->kind == FENG_DECL_ENUM → OTHER
        （enum item 为纯标识符，不提供关键字补全）
    else → MEMBER（type/spec/fit 体内，但不在具体成员内）
```

> 说明：
> - `GLOBAL_BINDING`（`let` / `var`）归类为 `TOP_BIND`，
>   提供绑定关键字与修饰符，不包含语句级关键字（`if`、`while`、`return` 等）。
> - `FENG_DECL_ENUM` 归类为 `OTHER`，因为 enum item 是纯标识符
>   （如 `enum Color { Red, Green, Blue }`），不提供关键字补全。

对应调用点（`build_completion_json` 的 `else` 分支 ~12044 行，非成员 `else` 分支）：

```c
/* 已有的 locals + module + imports 补全之后，追加关键字 */
if (completion_context.position != FENG_LSP_POS_OTHER) {
    if (!append_context_keyword_items(json, &first, completion_context.position)) {
        local_list_dispose(&locals);
        return false;
    }
}
```

`build_cached_completion_json`（`else` 分支 ~12338 行，非成员 `else` 分支）使用相同逻辑。

### 3.2 文本回退路径（脏代码场景）

当 AST 分析失败（脏代码无法解析）时，`handle_completion_request` 中的各回退路径可能产生空结果或仅含少量项。此时需要基于文本的位置判定。

在 `completion_context_from_text` 中增加 `position` 字段的赋值逻辑：

判定逻辑：

1. 先检测是否为成员访问（复用 `completion_context_is_member_access`）→ `OTHER`。
2. 从 offset 向前扫描，查找最近的未闭合 `{`。
3. 若找不到 `{` → `TOP_DECL`。
4. 若找到 `{`，继续向前跳过空白，读取 `{` 前的标识符：
   - 若标识符是 `type` / `spec` / `fit` → `MEMBER`。
   - 若标识符是 `enum` → `OTHER`（enum item 为纯标识符）。
   - 若标识符是 `func` 或 `}` 前紧跟 `)` → `BODY`。
   - 若标识符是 `let` / `var` → `TOP_BIND`。
   - 否则默认 `BODY`（在 `{}` 内部大概率是函数体）。

> 说明：此 heuristic 不处理字符串字面量和注释中的 `{`，
> 可能在极端场景下误判位置。这是已知 tradeoff，
> 脏代码场景下优先保证常见路径正确。

此逻辑在 `handle_completion_request` 的以下回退路径中使用：

- `build_use_path_fallback_completion_json` 之后
- `build_repaired_completion_json` 之后
- 最终 `send_json_response(output, id, "[]")` 之前

### 3.3 回退路径关键字注入策略

`handle_completion_request` 有多个返回路径，当前结构如下：

```text
1. cached path → 有项则返回
2. member repair fast path → 有项则返回
3. analysis path → 有项则返回
4. repaired completion path → 有项则返回
5. single parse fallback → 有项则返回
6. use path fallback → 有项则返回
7. literal builtin fallback → 有项则返回
8. 返回 "[]"
```

策略：

- **路径 1、3、5**：在 `build_completion_json` / `build_cached_completion_json` 内部的非成员分支追加关键字（AST 路径）。
- **路径 2、4**：repaired 路径仍调用 `build_completion_json`，关键字已在内部追加。
- **路径 6**（use path fallback）：import 路径上下文，不追加关键字。
- **路径 7**（literal builtin）：成员访问上下文，不追加关键字。
- **路径 8**（返回空）：若文本回退判定 `position != OTHER`，将关键字项作为最终兜底返回。

为确保所有非成员路径都能提供关键字，新增一个统一的兜底逻辑：在 `handle_completion_request` 返回 `"[]"` 之前，若 `completion_context.position != FENG_LSP_POS_OTHER`，则返回关键字项。

---

## 4. 注解补全

### 4.1 触发条件

当前 LSP `completionProvider.triggerCharacters` 不包含 `@`。需要：

1. 在 `initialize` 响应的 `triggerCharacters` 中添加 `"@"`。
2. 新增 `completion_context_is_annotation` 函数，检测光标前是否为 `@` 或 `@` + 部分标识符。

```c
static bool completion_context_is_annotation(const char *text,
                                              size_t offset,
                                              FengSlice *out_prefix);
```

判定逻辑：从 offset 向前扫描标识符字符，检查标识符起始位置的前一个字符是否为 `@`。若是，返回 `true` 并设置 `out_prefix` 为已输入的标识符部分。

### 4.2 注解项

内建注解与关键字遵循相同的"数据与逻辑分离"原则，定义在独立头文件 `lsp_annotations.h` 中，
C 逻辑仅遍历表结构，新增或调整注解时只改数据文件，不改逻辑代码。

**文件路径**：`src/cli/lsp/lsp_annotations.h`

**数据结构**：

```c
/* 单个注解项 */
typedef struct {
    const char *label;      /* 注解文本，如 "abi" */
    const char *detail;     /* 说明文本，如 "ABI annotation" */
} LspAnnotationItem;
```

**注解表**（内容对应 §4.2 注解列表）：

```c
static const LspAnnotationItem BUILTIN_ANNOTATIONS[] = {
    { "abi",      "ABI annotation" },
    { "cdecl",    "C calling convention" },
    { "stdcall",  "StdCall calling convention" },
    { "fastcall", "FastCall calling convention" },
    { "runtime",  "runtime annotation" },
    { "iterable", "iterable annotation" },
    { "iterator", "iterator annotation" },
};

static const size_t BUILTIN_ANNOTATION_COUNT =
    sizeof(BUILTIN_ANNOTATIONS) / sizeof(BUILTIN_ANNOTATIONS[0]);
```

> 说明：`lsp_annotations.h` 与编译器内部注解表（`feng_builtin_annotations()`）内容一致，
> 但属于有意独立声明——两者职责不同：编译器注解表用于语义分析，LSP 注解表用于补全。
> 独立声明避免 LSP 对编译器内部 API 的耦合，后续 LSP 可提供额外的补全辅助信息（如 detail、snippet 等）。

| 注解         | 说明              | CompletionItemKind |
| ------------ | ----------------- | ------------------ |
| `abi`        | ABI 标注          | 14 (Keyword)       |
| `cdecl`      | C 调用约定        | 14                 |
| `stdcall`    | StdCall 调用约定  | 14                 |
| `fastcall`   | FastCall 调用约定 | 14                 |
| `runtime`    | 运行时标注        | 14                 |
| `iterable`   | 可迭代标注        | 14                 |
| `iterator`   | 迭代器标注        | 14                 |

### 4.3 注入位置

在 `handle_completion_request` 中，解析参数并计算 `offset` 后、
计算 `is_member_completion` 之前，先检测注解上下文：

```c
/* offset 计算完成后，立即检测注解 */
FengSlice annotation_prefix = {0};
if (completion_context_is_annotation(document->text, offset, &annotation_prefix)) {
    /* 直接构建注解补全 JSON 并返回 */
    ok = build_annotation_completion_json(annotation_prefix, &json);
    /* ... send response and return ... */
}
/* 之后才计算 is_member_completion 等 */
```

> 说明：注解检测必须在 `is_member_completion` 计算之前执行并提前返回，
> 确保注解上下文与成员访问等其他上下文完全互斥，避免干扰。

注解补全是互斥的：如果光标在 `@` 后，只返回注解项，不返回其他补全。

### 4.4 `@` 触发字符声明变更

`initialize` 响应中的 `completionProvider.triggerCharacters` 需从：

```json
[".", "_", "a"..."z", "A"..."Z"]
```

扩展为：

```json
[".", "_", "@", "a"..."z", "A"..."Z"]
```

---

## 5. Snippet 支持（最终阶段）

### 5.1 LSP 协议支持

LSP 协议通过 `CompletionItem` 的以下字段支持 Snippet：

- `insertText: string` — Snippet 文本，使用 LSP Snippet 语法（`${1:placeholder}`、`$0` 等）。
- `insertTextFormat: 2` — 值 `2` 表示 Snippet 格式（`1` 为纯文本）。

VSCode 原生支持 LSP Snippet 语法，无需额外适配。

### 5.2 Snippet 模板定义

各位置中，适合提供 Snippet 模板的关键字应附带 `insertText`，不适合的关键字仅提供纯文本补全。

**TOP_DECL Snippet**：

| label           | insertText                                           | 说明              |
| --------------- | ---------------------------------------------------- | ----------------- |
| `module`        | `module ${1:name}`                                   | 模块声明          |
| `import`        | `import ${1:path}`                                   | 导入声明          |
| `func`          | `func ${1:name}(${2:params}): ${3:void} {\n\t$0\n}`  | 函数声明          |
| `type`          | `type ${1:Name} {\n\t$0\n}`                          | 对象类型声明      |
| `type-tuple`    | `type ${1:Name}(${2:types});`                        | 元组类型声明      |
| `enum`          | `enum ${1:Name} {\n\t$0\n}`                          | 枚举声明          |
| `spec`          | `spec ${1:Name} {\n\t$0\n}`                          | object-form 规约  |
| `spec-callable` | `spec ${1:Name}(${2:params}): ${3:void};`            | callable-form 规约 |
| `spec-union`    | `spec ${1:Name}: ${2:T1} \| ${0:T2};`               | union-form 规约   |
| `fit`           | `fit ${1:Name} {\n\t$0\n}`                           | 适配声明          |

> 说明：
> - `extern` 不提供独立 Snippet，因为 `extern` 总是修饰后续声明关键字
>   （如 `extern func`），用户选择 `func` 的 Snippet 后手动添加 `extern` 即可。
> - `type` 与 `type-tuple`、`spec` / `spec-callable` / `spec-union` 均为独立项，
>   label 不同，用户输入关键字前缀时由 VSCode 同时展示，按需选择。

**TOP_BIND Snippet**：

| 关键字 | insertText                              | 说明     |
| ------ | --------------------------------------- | -------- |
| `let`  | `let ${1:name}: ${2:type} = ${0:value}` | 全局绑定 |
| `var`  | `var ${1:name}: ${2:type} = ${0:value}` | 全局绑定 |

**MEMBER Snippet**：

| 关键字 | insertText                                           | 说明     |
| ------ | ---------------------------------------------------- | -------- |
| `func` | `func ${1:name}(${2:params}): ${3:void} {\n\t$0\n}`  | 方法声明 |
| `let`  | `let ${1:name}: ${2:type}`                            | 字段声明 |
| `var`  | `var ${1:name}: ${2:type}`                            | 字段声明 |

**BODY Snippet**：

| label     | insertText                                               | 说明                                 |
| --------- | -------------------------------------------------------- | ------------------------------------ |
| `if`      | `if ${1:condition} {\n\t$0\n}`                           | 条件语句                             |
| `if-else` | `if ${1:condition} {\n\t$2\n} else {\n\t$0\n}`           | 条件+else                            |
| `match`   | `match ${1:target} {\n\t$0\n}`                           | 模式匹配                             |
| `while`   | `while ${1:condition} {\n\t$0\n}`                        | 循环                                 |
| `for`     | `for ${1:var i = 0}; ${2:i < n}; ${3:i = i + 1} {\n\t$0\n}` | 三段式循环                       |
| `for-in`  | `for ${1:let it} in ${2:iterable} {\n\t$0\n}`            | for/in 迭代循环                      |
| `try`     | `try {\n\t$1\n} catch ${2:err} {\n\t$0\n}`               | 异常捕获                             |
| `defer`   | `defer {\n\t$0\n}`                                       | 延迟执行                             |

> 说明：
> - `if` 和 `if-else` 作为两个独立项，label 分别为 `if` 和 `if-else`，
>   两者的 `insertText` 均以 `if` 开头，输入 `if` 时 VSCode 同时展示两项。
> - `for` 和 `for-in` 同理，`insertText` 均以 `for` 开头，输入 `for` 时
>   同时展示三段式与 for/in 两种模板。

### 5.3 新增 append 函数

需要新增 `append_completion_item_snippet`，在现有 `append_completion_item` 基础上增加 `insertText` 和 `insertTextFormat` 字段：

```c
static bool append_completion_item_snippet(FengLspString *json,
                                            bool *first,
                                            FengSlice label,
                                            const char *detail,
                                            int kind,
                                            const char *insert_text);
```

生成的 JSON 项示例：

```json
{
  "label": "if",
  "kind": 14,
  "detail": "keyword",
  "insertText": "if ${1:condition} {\n\t$0\n}",
  "insertTextFormat": 2
}
```

> **JSON 转义**：`insertText` 值包含换行符（`\n`）和制表符（`\t`）等特殊字符，
> 写入 JSON 时必须通过 `string_append_json_string` 进行 JSON 转义
> （与 `label` 和 `detail` 字段的处理方式相同）。
> 直接拼接原始字符串会导致 JSON 格式错误。

### 5.4 Snippet 与纯关键字策略

遵循主流语言服务器做法（rust-analyzer、gopls、TypeScript），
每个关键字**仅提供一个补全项**：

- 有 Snippet 模板的关键字：仅提供 Snippet 项（`insertTextFormat: 2`）。
  用户选择后展开模板，通过 Tab 键在占位符间跳转。
- 无 Snippet 模板的关键字：仅提供纯文本项（`insertTextFormat: 1` 或省略）。
  用户选择后直接插入关键字文本。

> 说明：不提供重复项（同一关键字同时出现纯文本和 Snippet 两个项），
> 避免补全列表中出现相同 label 的项导致用户困惑。
> 这是 rust-analyzer、gopls 等生产级语言服务器的统一做法。

---

## 6. 变更点汇总

### 6.1 `src/cli/lsp/lsp_keywords.h`（新增）

| 内容         | 说明                                     | 阶段 |
| ------------ | ---------------------------------------- | ---- |
| `LspKwItem`  | 关键字项结构（label、detail、snippet）   | 1    |
| `LspKwTable` | 位置表结构（items + count）              | 1    |
| 四张位置表 | `TOP_DECL_KWS`、`TOP_BIND_KWS`、`MEMBER_KWS`、`BODY_KWS` | 1 |
| `KW_TABLE`   | 按 `FengLspPosition` 索引的总表          | 1    |
| 各表项填充 `snippet` 字段 | 阶段三启用 Snippet 模板       | 3    |

### 6.2 `src/cli/lsp/lsp_annotations.h`（新增）

| 内容                       | 说明                                       | 阶段 |
| -------------------------- | ------------------------------------------ | ---- |
| `LspAnnotationItem`        | 注解项结构（label、detail）                | 2    |
| `BUILTIN_ANNOTATIONS`      | 内建注解表（abi、cdecl、stdcall 等）       | 2    |
| `BUILTIN_ANNOTATION_COUNT` | 注解数量常量                               | 2    |

### 6.3 `src/cli/lsp/runtime.c`

| 位置                                                      | 变更                                           | 阶段 |
| --------------------------------------------------------- | ---------------------------------------------- | ---- |
| 新增 `FengLspPosition` 枚举                               | 定义语法位置类型                               | 1    |
| `FengLspCompletionContext` 新增 `position` 字段           | 语法位置字段（`FengLspPosition`）              | 1    |
| `completion_context_from_text`                            | 文本路径填充 `position` 字段                   | 1    |
| `build_completion_json` / `build_cached_completion_json`  | AST 路径精确覆盖 `position` 字段               | 1    |
| 新增 `append_context_keyword_items`                       | 按 `position` 追加关键字补全项                 | 1    |
| `build_completion_json` 的 `else` 分支 ~12044 行末尾      | 调用 `append_context_keyword_items`            | 1    |
| `build_cached_completion_json` 的 `else` 分支 ~12338 行末尾 | 调用 `append_context_keyword_items`            | 1    |
| `handle_completion_request` 返回 `"[]"` 之前              | 文本回退兜底关键字                             | 1    |
| 新增 `completion_context_is_annotation`                    | 注解上下文检测                                 | 2    |
| 新增 `build_annotation_completion_json`                    | 构建注解补全响应（遍历 `BUILTIN_ANNOTATIONS`） | 2    |
| `handle_completion_request` 参数解析后                     | 注解上下文优先检测                             | 2    |
| `initialize` 响应 ~14382 行                               | `triggerCharacters` 添加 `"@"`                 | 2    |
| 新增 `append_completion_item_snippet`                      | Snippet 补全项构建                             | 3    |
| `append_context_keyword_items`                            | 各位置适合 Snippet 的关键字附带 insertText     | 3    |

### 6.4 `editors/feng-vscode/`

无需变更。`package.json` 已配置 `editor.quickSuggestions` 和 `suggestOnTriggerCharacters`，LSP 的 `completionProvider` 声明变更后 VSCode 自动适配。

---

## 7. 分步 TODO

> 每个步骤完成后可独立运行全量回归测试并单独交付。
> 各步骤引用 §2–§5 中的详细设计。

### 阶段一：上下文感知关键字补全

- [x] **1.1** 新建 `src/cli/lsp/lsp_keywords.h`，声明 `FengLspPosition` 枚举、`LspKwItem`、`LspKwTable`、`KW_TABLE` 及四张位置表（§2.1、§2.4），此阶段 `snippet` 字段全部为 `NULL`
- [x] **1.2** 在 `FengLspCompletionContext` 中新增 `position` 字段（`FengLspPosition` 类型）（§3.0）
- [x] **1.3** 在 `completion_context_from_text` 中增加文本路径的 `position` 赋值逻辑（§3.2）
- [x] **1.4** 在 `build_completion_json` / `build_cached_completion_json` 中增加 AST 路径的 `position` 精确覆盖逻辑（§3.1）
- [x] **1.5** 实现 `append_context_keyword_items`：遍历 `KW_TABLE[position]`，调用已有 `append_completion_item`（§2.4）
- [x] **1.6** 在 `build_completion_json` 和 `build_cached_completion_json` 非成员分支末尾，`position != OTHER` 时调用 `append_context_keyword_items`（§3.1）
- [x] **1.7** 在 `handle_completion_request` 兜底路径（返回 `"[]"` 前）注入文本回退关键字（§3.3 路径 8）
- [x] **1.8** 补充回归测试（§2.2 各表 + §3 判定规则），运行全量回归验证

### 阶段二：注解补全

- [x] **2.1** 新建 `src/cli/lsp/lsp_annotations.h`，声明 `LspAnnotationItem`、`BUILTIN_ANNOTATIONS` 表及 `BUILTIN_ANNOTATION_COUNT`（§4.2）
- [x] **2.2** 实现 `completion_context_is_annotation`：检测 `@` 前缀（§4.1）
- [x] **2.3** 实现 `build_annotation_completion_json`：遍历 `BUILTIN_ANNOTATIONS` 构建注解项（§4.2）
- [x] **2.4** 在 `handle_completion_request` 中 offset 计算后、`is_member` 计算前，插入注解优先检测并提前返回（§4.3）
- [x] **2.5** `initialize` 响应 `triggerCharacters` 添加 `"@"`（§4.4）
- [x] **2.6** 补充注解补全回归测试（输入 `@` 返回全部注解；输入 `@a` 过滤出 `abi`），运行全量回归验证

### 阶段三：Snippet 支持

- [ ] **3.1** 实现 `append_completion_item_snippet`：支持 `insertText` + `insertTextFormat: 2`（§5.3）
- [ ] **3.2** 在 `lsp_keywords.h` 中填充各表项的 `snippet` 字段（§2.4、§5.2）
- [ ] **3.3** 修改 `append_context_keyword_items`：`snippet != NULL` 的项调用 `append_completion_item_snippet`，`snippet == NULL` 的项保持纯文本（§5.4）
- [ ] **3.4** 补充 Snippet 回归测试（`if` 项含 `insertText` + `insertTextFormat: 2`；同一关键字无重复项），运行全量回归验证

---

## 8. 参考实现

TypeScript / rust-analyzer / gopls 均采用统一 Context 结构，将语法位置作为字段，AST 优先、文本回退兜底。

| 服务器 | 位置字段 | 位置分类 |
|---|---|---|
| TypeScript | `CompletionContext.syntacticPosition` | Expression / Statement / Type / Modifier / Declaration |
| rust-analyzer | `CompletionContext.position` | Expr / Item / Type / Name |
| gopls | `CompletionContext.Position` | topLevel / funcBody / structField / interfaceMethod |

Feng 的 `FengLspPosition` 作为 `FengLspCompletionContext.position` 字段，与上述方向一致。当前覆盖位置分类和 AST 优先两个核心模式，类型位置和修饰符位置暂不区分。
