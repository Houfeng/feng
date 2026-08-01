# 错误输出格式优化（Phase 1）

> 状态：已 Review 通过
> 日期：2026-06-13

## 1. 背景

当前 Feng 编译器的错误输出格式为：

```
path:line:col: kind: message
  source context
  ^ pointer
```

其中 `kind` 为自由文本（`parse error` / `semantic error` / `codegen error`），消息由各模块自行拼接，缺乏结构化编码。这导致：

- 无法程序化识别具体错误类型
- LSP/IDE 插件难以提供精准的 quick fix 和文档链接
- 错误分类粗糙，仅有三大类

本阶段仅做**输出格式微调**，不涉及错误码表统一管理（Phase 2）。

## 2. 目标

将错误输出格式调整为：

```
{path}:{line}:{col}
{错误码}: {消息}
  {源码上下文}
  ^ pointer
```

**核心变化**：
- 位置信息独占第一行，与诊断描述分行显示，终端视觉更清晰
- 错误码 + 消息在第二行，移除 `kind` 文本（"parse error" 等），由错误码前缀表达类型
- 第一行位置信息末尾不加 `:`

## 3. 错误码编码规则

### 3.1 前缀定义

| 前缀 | 含义 | 对应阶段 |
|------|------|----------|
| LE | Lexical Error | 词法分析 |
| SE | Syntax Error | 语法分析 |
| AE | Analysis Error | 语义分析 |
| CE | Codegen Error | 代码生成 |
| IE | Infrastructure Error | 基础设施（跨阶段） |

### 3.2 编码格式

`{前缀}{4位数字}:`，例如 `SE0001:`、`AE0042:`

- 4 位数字从 `0001` 开始递增，**各阶段独立编号**（IE 除外，IE 采用全局统一编号）
- 错误码后必须跟 `:` 再跟消息文本
- 本阶段错误码可临时硬编码，Phase 2 再统一码表管理
- **错误码稳定性**：Feng 每个版本中错误码是稳定且唯一的。当前阶段仅要求递增分配，未来错误码体系稳定后才承诺废弃码不回收

### 3.3 错误码分配原则

- **按阶段归属**：错误码前缀由**产生错误的阶段**决定，而非传递错误的结构体。例如词法错误虽通过 `FengParseError` 传出，但其产生阶段是词法分析，应使用 LE 前缀。
- **同一错误同一码**：同一阶段内，**相同的错误**必须使用相同的错误码，不因出现位置不同而分配不同编号。本质是：同样的错误需要同错误码，而非简单按消息文本字符串匹配。
- **基础设施错误统一使用 IE 前缀**：如 `out of memory`、文件 I/O 失败等由底层设施直接抛出的错误，**不按发生阶段归类**，统一使用 `IE` 前缀。`IE` 错误码采用**全局统一编号**（如 `IE0001`、`IE0002`...），不分阶段独立编号，确保全局唯一性。具体阶段信息通过错误消息文本体现（如 `IE0001: out of memory in semantic analysis`）。

### 3.4 词法错误（LE）的处理方式

当前架构中，lexer 不直接报出错误，而是产生 `FENG_TOKEN_ERROR` token 并将错误消息挂在 `token.error_message` 上返回。parser 在 `parser_tokenize()`（`src/parser/parser.c`）中检测到该 token 后，将其转为 `FengParseError` 传出。

**LE 前缀的判定规则**：在 `parser_tokenize()` 中，当检测到 `token.kind == FENG_TOKEN_ERROR` 时，该错误由词法阶段产生，`parser->error.code` 应使用 LE 前缀。parser 自身在其他位置（如 `parser_error_at()` / `parser_error_current()` / `parser_expect()` 等函数中）产生的错误，使用 SE 前缀。

**实现方式**：给 `FengToken` 增加 `const char *error_code` 字段，并将现有 `message` 重命名为 `error_message`（两者对称：正常 token 均为 NULL，错误 token 均非 NULL）。lexer 在 `make_error()` 中直接硬编码对应的 LE 错误码字符串字面量赋值给 `token.error_code`，parser 在 `parser_tokenize()` 中透传 `token.error_code` → `parser->error.code`，无需字符串匹配。所有引用 `token.message` 的代码同步改为 `token.error_message`。具体的 LE 错误码在实现时根据 lexer 中 `make_error()` 的所有调用点逐一确定，相同消息文本使用相同错误码。Phase 2 再统一为码表管理。

## 4. 输出格式规范

### 4.1 单条错误

```
/path/to/file.ff:17:3
SE0001: expression statements and local bindings must end with ';'
  got: KW_LET "let"
  17 |   let obj = JsonObject();
     |   ^
```

**结构分解**：
- **第 1 行**：`{path}:{line}:{col}` — 位置信息，无尾部冒号
- **第 2 行**：`{错误码}: {消息}` — 核心诊断信息
- **后续行**：源码上下文 + `^` 指针（保持现有逻辑不变）

### 4.2 多条错误

多条错误之间的分隔方式**保持现有行为不变**，不做调整。

### 4.3 颜色方案

**保持现有颜色方案不变**：
- location 部分（`path:line:col`）红色高亮
- 其余部分保持默认色

## 5. 变更范围

### 5.1 需要修改的文件

#### 5.1.1 核心编译器：错误相关结构体增加 `code` 字段

- `src/lexer/token.h` — `FengToken` 中现有 `message` 字段重命名为 `error_message`，新增 `const char *error_code`（与 `error_message` 对称，正常 token 两者均为 NULL，错误 token 由 lexer 在 `make_error()` 中直接赋值 LE 错误码字面量）。所有引用 `token.message` 的代码同步改为 `token.error_message`
- `src/parser/parser.h` — `FengParseError` 增加 `const char *code`
- `src/semantic/semantic.h` — `FengSemanticError` 增加 `const char *code`
- `src/codegen/codegen.h` — `FengCodegenError` 增加 `const char *code`

各模块在构造错误时填入对应的错误码字符串字面量（如 `"SE0001"`），指向 `.rodata` 段静态字符串，无需动态分配和释放。

**词法错误的处理**：lexer 在 `make_error()` 中直接设置 `token.error_code` 为 LE 前缀的错误码字面量。parser 在 `parser_tokenize()` 中透传 `token.error_code` → `parser->error.code`。parser 自身产生的语法错误在 `parser_error_at()` / `parser_error_current()` / `parser_expect()` 等函数中设置 SE 前缀的 code。

#### 5.1.2 CLI：渲染函数参数及输出格式调整

- `src/cli/common.c` — `feng_cli_print_diagnostic` 函数，调整输出格式
- `src/cli/common.h` — 函数签名中 `kind` 参数改为 `code`

**签名变化**：

现有签名：
```c
void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *kind,
                               const char *message,
                               const FengToken *token,
                               const char *source,
                               size_t source_length);
```

Phase 1 新签名（`kind` 改为 `code`）：
```c
void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *code,
                               const char *message,
                               const FengToken *token,
                               const char *source,
                               size_t source_length);
```

- `kind` 参数（原值为 `"parse error"` / `"semantic error"` / `"codegen error"`）改为 `code`（如 `"SE0001"`）
- 所有调用点从结构体的 `code` 字段取值传入，不再硬编码分类标签
- 函数内部按新格式输出：第一行 `{path}:{line}:{col}`（无尾部冒号），第二行 `{code}: {message}`，后续为源码上下文

### 5.2 不需要修改的部分

- 各模块的错误消息文本内容（本阶段不改消息措辞）
- 源码上下文渲染逻辑（`print_error_context`）
- 颜色控制逻辑
- 测试用例（需人工批准后同步更新）

## 6. 不在本阶段范围

以下事项留待 Phase 2 或更后续阶段：

- 统一错误码表（码表固定在代码中，支持国际化）
- `logError(错误码, 参数列表)` 辅助函数
- warning / info 级别（LW/SW/AW/CW）
- help 提示行
- 错误码注册表文档

## 7. 迁移注意事项

- 现有测试用例中有大量硬编码的错误消息断言，格式变更后需同步更新
- 修改测试用例需获得明确的人工批准
- LSP 插件（VSCode/Zed）可能需要适配新的输出格式以正确解析 diagnostic code

## 8. 开发任务拆解

### 实现阶段

- [x] `FengToken` 中 `message` 重命名为 `error_message`，新增 `const char *error_code`（`src/lexer/token.h`），`make_token()` 中初始化为 NULL，所有引用 `token.message` 的代码同步改为 `token.error_message`
- [x] lexer 的 `make_error()` 增加 `error_code` 参数，所有调用点传入对应的 LE 错误码字面量（`src/lexer/lexer.c`）
- [x] 三个错误结构体增加 `const char *code` 字段（`src/parser/parser.h`、`src/semantic/semantic.h`、`src/codegen/codegen.h`）
- [x] 全量回归测试（结构体变更后）
- [x] `parser_tokenize()` 中透传 `token.error_code` → `parser->error.code`；parser 自身错误在 `parser_error_at()` 等函数中设置 SE 错误码（`src/parser/parser.c`）
- [x] 核心编译器 semantic、codegen 模块在构造错误时填入错误码字符串字面量（semantic → `"AE..."`、codegen → `"CE..."`）
- [x] 梳理所有基础设施错误（OOM、I/O 失败等）调用点，确定全局唯一的 IE 错误码（如 `IE0001`、`IE0002`...），并在对应模块构造错误时填入
- [x] 全量回归测试（错误码填入后）
- [x] `feng_cli_print_diagnostic` 签名中 `kind` 参数改为 `code`，输出格式调整为两行（`src/cli/common.h`、`src/cli/common.c`）
- [x] 全量回归测试（渲染函数变更后）
- [x] CLI 层所有 `feng_cli_print_diagnostic` 调用点将硬编码分类标签替换为从结构体取 `code` 字段传入，涉及以下文件（均在 `src/cli/` 下，非核心编译器）：
  - `src/cli/compile/direct.c` — 新版编译路径，通过回调处理错误，3 处调用（parse error / semantic error / semantic info）
  - `src/cli/compile/legacy.c` — 旧版单文件编译路径，直接调用 parser/semantic/codegen，3 处调用（parse error / semantic error / codegen error）
  - `src/cli/tool/parse.c` — `feng parse` 子命令，1 处调用（parse error）
  - `src/cli/tool/semantic.c` — `feng semantic` 子命令，3 处调用（parse error / semantic error / info）
  - `src/cli/project/check.c` — `feng check` 子命令，3 处调用（parse error / semantic error / semantic info）
  - 示例：`"parse error"` → `error->code`，`"semantic error"` → `error->code`，`"codegen error"` → `cgerr.code`
- [x] 全量回归测试（调用点变更后）

### 测试阶段

- [x] 更新测试用例中的错误输出断言（分析确认：test_*.c 均通过内部 API 调用编译器，其 parse error/semantic error 字符串均为测试 helper 自用的 fprintf 输出，非对 CLI 格式的断言，无需修改）
- [x] 运行全量回归测试，确保未破坏现有错误输出行为（make test 全部通过，exit code 0）
- [x] CLI 输出格式验证：grep -rn 'feng_cli_print_diagnostic' src/cli/ 验证完整调用点清单与文档一致，无遗漏；手动触发各类错误确认格式符合规范（第一行 path:line:col，第二行 code: 消息）
- [x] LSP 插件兼容性验证：VSCode 扩展通过 vscode-languageclient 使用 LSP JSON-RPC 协议获取诊断（textDocument/publishDiagnostics），Zed 扩展同样通过 LSP 适配器层获取诊断——两者均不解析 CLI 文本输出格式，不受本次格式变更影响

### 收尾阶段

- [x] 检查 lint 错误，确保修改文件无新增问题（编译使用 -Wall -Wextra -Werror，make test 构建通过即代表无 lint 错误）
- [x] 给出建议的 commit message
