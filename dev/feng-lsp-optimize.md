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
- **复用现有基础设施**：利用 `feng_keywords()`、`feng_builtin_annotations()` 等已有 API，不重新发明关键字表。
- **最小侵入**：在现有 completion 流程的非成员分支末尾追加关键字项，不改变现有补全逻辑。
- **禁止改动核心编译器**：所有变更仅限 `src/cli/lsp/` 目录，不得修改 `src/lexer/`、`src/parser/`、`src/semantic/`、`src/codegen/`、`src/symbol/` 等核心编译器模块，仅通过已有公开 API 消费编译器能力。

---

## 2. 关键字上下文分类

### 2.1 上下文枚举

```c
typedef enum {
    FENG_LSP_KW_CTX_NONE,       /* 无需关键字（成员访问、import 路径等） */
    FENG_LSP_KW_CTX_TOP_LEVEL,  /* 模块顶层（declaration 之外） */
    FENG_LSP_KW_CTX_MEMBER,     /* type / spec / fit 声明体内部（成员声明位置） */
    FENG_LSP_KW_CTX_BODY        /* 函数 / 方法体内部（语句位置） */
} FengLspKeywordContext;
```

### 2.2 各上下文关键字集合

**TOP_LEVEL**（14 个）：

| 关键字   | 说明           |
| -------- | -------------- |
| `module` | 模块声明       |
| `import` | 导入声明       |
| `func`   | 函数声明       |
| `type`   | 类型声明       |
| `enum`   | 枚举声明       |
| `spec`   | 规约声明       |
| `fit`    | 适配声明       |
| `extern` | 外部声明       |
| `open`   | 可见性修饰     |
| `seal`   | 可见性修饰     |
| `static` | 静态修饰       |
| `let`    | 全局不可变绑定 |
| `var`    | 全局可变绑定   |
| `as`     | 导入别名       |

**MEMBER**（6 个）：

| 关键字   | 说明       |
| -------- | ---------- |
| `func`   | 方法声明   |
| `let`    | 不可变字段 |
| `var`    | 可变字段   |
| `static` | 静态成员修饰 |
| `open`   | 可见性修饰 |
| `seal`   | 可见性修饰 |

**BODY**（17 个）：

| 关键字     | 说明             |
| ---------- | ---------------- |
| `let`      | 局部不可变绑定   |
| `var`      | 局部可变绑定     |
| `if`       | 条件语句/表达式  |
| `else`     | 条件分支         |
| `match`    | 模式匹配         |
| `while`    | 循环             |
| `for`      | 循环             |
| `in`       | for/in 迭代      |
| `break`    | 跳出循环         |
| `continue` | 继续循环         |
| `return`   | 返回             |
| `throw`    | 抛出异常         |
| `try`      | 异常捕获         |
| `catch`    | 异常处理         |
| `defer`    | 延迟执行         |
| `unknown`  | 未知值           |
| `void`     | 空类型           |

### 2.3 排除项

- **保留字**（`class`、`struct`、`const`、`export`、`prop`）不纳入补全，当前不使用。
- **`self`** 已由 `collect_visible_locals_for_completion` 作为局部变量提供，在 BODY 上下文中不再重复添加。
- **`true` / `false` / `none`** 不属于关键字（分别是 `FENG_TOKEN_BOOL` 和标准库函数），不通过关键字补全提供。

---

## 3. 上下文判定

### 3.1 AST 路径（优先）

当 `build_completion_json` 或 `build_cached_completion_json` 能获取到有效 AST 时，使用以下规则：

```text
输入：enclosing_decl, enclosing_member

前置条件：调用方已在进入此函数前排除了成员访问（is_member）和 import 路径
（is_use_path）上下文——前者走成员分支，后者已由 extract_use_path_context
提前返回，均不会到达关键字分类逻辑。

if enclosing_decl == NULL → TOP_LEVEL
if enclosing_member != NULL:
    if enclosing_member 是 callable（method/constructor/finalizer）:
        → BODY
    else:
        → MEMBER（field 内部或声明位置）
else:
    if enclosing_decl->kind == FENG_DECL_FUNCTION
        || enclosing_decl->kind == FENG_DECL_GLOBAL_BINDING → BODY
    else → MEMBER（type/enum/spec/fit 体内，但不在具体成员内）
```

> 说明：`GLOBAL_BINDING`（`let` / `var`）在顶层和函数体内均合法，其初始化表达式
> 位于语句级上下文，因此与 `FUNCTION` 一样归类为 `BODY`。

对应调用点（`build_completion_json` 的 `else` 分支 ~12044 行，非成员 `else` 分支）：

```c
/* 已有的 locals + module + imports 补全之后，追加关键字 */
{
    FengLspKeywordContext kw_ctx = classify_keyword_context(
        enclosing_decl, enclosing_member);
    if (kw_ctx != FENG_LSP_KW_CTX_NONE) {
        if (!append_context_keyword_items(json, &first, kw_ctx)) {
            local_list_dispose(&locals);
            return false;
        }
    }
}
```

`build_cached_completion_json`（`else` 分支 ~12338 行，非成员 `else` 分支）使用相同逻辑。

### 3.2 文本回退路径（脏代码场景）

当 AST 分析失败（脏代码无法解析）时，`handle_completion_request` 中的各回退路径可能产生空结果或仅含少量项。此时需要基于文本的上下文判定。

新增函数 `classify_keyword_context_from_text`：

```c
static FengLspKeywordContext classify_keyword_context_from_text(
    const char *text, size_t offset);
```

判定逻辑：

1. 先检测是否为成员访问（复用 `completion_context_is_member_access`）→ `NONE`。
2. 从 offset 向前扫描，查找最近的未闭合 `{`。
3. 若找不到 `{` → `TOP_LEVEL`。
4. 若找到 `{`，继续向前跳过空白，读取 `{` 前的标识符：
   - 若标识符是 `type` / `spec` / `fit` → `MEMBER`。
   - 若标识符是 `func` 或 `}` 前紧跟 `)` → `BODY`。
   - 否则默认 `BODY`（在 `{}` 内部大概率是函数体）。

此函数在 `handle_completion_request` 的以下回退路径中使用：

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
- **路径 8**（返回空）：若文本回退判定上下文不是 `NONE`，将关键字项作为最终兜底返回。

为确保所有非成员路径都能提供关键字，新增一个统一的兜底逻辑：在 `handle_completion_request` 返回 `"[]"` 之前，若 `classify_keyword_context_from_text` 返回非 `NONE`，则返回关键字项。

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

使用 `feng_builtin_annotations()` 和 `feng_builtin_annotation_count()` 获取内置注解列表：

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

在 `handle_completion_request` 中，解析参数后、调用 `build_completion_json` 之前，先检测注解上下文：

```c
FengSlice annotation_prefix = {0};
if (completion_context_is_annotation(document->text, offset, &annotation_prefix)) {
    /* 直接构建注解补全 JSON 并返回 */
    ok = build_annotation_completion_json(annotation_prefix, &json);
    /* ... send response and return ... */
}
```

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

各上下文中，适合提供 Snippet 模板的关键字应附带 `insertText`，不适合的关键字仅提供纯文本补全。

**TOP_LEVEL Snippet**：

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
| `let`           | `let ${1:name}: ${2:type} = ${0:value}`               | 全局绑定          |
| `var`           | `var ${1:name}: ${2:type} = ${0:value}`               | 全局绑定          |

> 说明：
> - `extern` 不提供独立 Snippet，因为 `extern` 总是修饰后续声明关键字
>   （如 `extern func`），用户选择 `func` 的 Snippet 后手动添加 `extern` 即可。
> - `type` 与 `type-tuple`、`spec` / `spec-callable` / `spec-union` 均为独立项，
>   label 不同，用户输入关键字前缀时由 VSCode 同时展示，按需选择。

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

### 5.4 Snippet 与纯关键字共存

有 Snippet 模板的关键字**同时提供纯文本项和 Snippet 项**，两者 label 相同，
VSCode 并列展示，用户可选择直接插入关键字或展开模板。无 Snippet 模板的关键字
仅提供纯文本项。

---

## 6. 变更点汇总

### 6.1 `src/cli/lsp/runtime.c`

| 位置                                                      | 变更                                           | 阶段 |
| --------------------------------------------------------- | ---------------------------------------------- | ---- |
| 新增 `FengLspKeywordContext` 枚举                         | 定义上下文类型                                 | 1    |
| 新增 `classify_keyword_context`                           | AST 路径上下文判定                             | 1    |
| 新增 `classify_keyword_context_from_text`                 | 文本回退路径上下文判定                         | 1    |
| 新增 `append_context_keyword_items`                       | 按上下文追加关键字补全项                       | 1    |
| `build_completion_json` 的 `else` 分支 ~12044 行末尾      | 调用 `append_context_keyword_items`            | 1    |
| `build_cached_completion_json` 的 `else` 分支 ~12338 行末尾 | 调用 `append_context_keyword_items`            | 1    |
| `handle_completion_request` 返回 `"[]"` 之前              | 文本回退兜底关键字                             | 1    |
| 新增 `completion_context_is_annotation`                    | 注解上下文检测                                 | 2    |
| 新增 `build_annotation_completion_json`                    | 构建注解补全响应                               | 2    |
| `handle_completion_request` 参数解析后                     | 注解上下文优先检测                             | 2    |
| `initialize` 响应 ~14382 行                               | `triggerCharacters` 添加 `"@"`                 | 2    |
| 新增 `append_completion_item_snippet`                      | Snippet 补全项构建                             | 3    |
| `append_context_keyword_items`                            | 各上下文适合 Snippet 的关键字附带 insertText   | 3    |

### 6.2 `editors/feng-vscode/`

无需变更。`package.json` 已配置 `editor.quickSuggestions` 和 `suggestOnTriggerCharacters`，LSP 的 `completionProvider` 声明变更后 VSCode 自动适配。

---

## 7. 任务拆分

### 任务 1：上下文感知关键字补全

**交付内容**：

- 实现 `FengLspKeywordContext` 枚举与 `classify_keyword_context`。
- 实现 `classify_keyword_context_from_text` 文本回退。
- 实现 `append_context_keyword_items`。
- 在 `build_completion_json`、`build_cached_completion_json` 的非成员分支末尾调用。
- 在 `handle_completion_request` 兜底路径中调用。
- 补充 LSP 协议级回归测试：顶层输入 `f` 应包含 `func`；函数体内输入 `l` 应包含 `let`；成员访问时不应出现关键字。

### 任务 2：注解补全

**前置**：任务 1 完成。

**交付内容**：

- 实现 `completion_context_is_annotation`。
- 实现 `build_annotation_completion_json`。
- 在 `handle_completion_request` 中添加注解上下文优先检测。
- `initialize` 响应的 `triggerCharacters` 添加 `"@"`。
- 补充回归测试：输入 `@` 应返回 `abi`、`runtime` 等；输入 `@a` 应过滤出 `abi`。

### 任务 3：Snippet 支持

**前置**：任务 2 完成。

**交付内容**：

- 实现 `append_completion_item_snippet`。
- 修改 `append_context_keyword_items`，为各上下文中具有模板的关键字附加 `insertText` + `insertTextFormat: 2`。
- 补充回归测试：BODY 上下文中的 `if` 补全项应包含 `insertText` 字段且 `insertTextFormat` 为 2；TOP_LEVEL 中的 `func` 补全项应包含 Snippet 模板。
