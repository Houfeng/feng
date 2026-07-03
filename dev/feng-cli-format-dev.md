# feng format 实现方案

## 背景

当前 Feng 语言的格式化功能仅在 VSCode 插件中通过 `editors/feng-vscode/formatter.js`（约 1200 行 JS）实现。计划将格式化能力内置到编译器 CLI 中，新增 `feng format [path]` 命令，并接入 LSP。

## 目标

1. 在 C 编译器中实现格式化器，忠实移植 JS 版本的算法
2. 提供 `feng format [path]` CLI 命令
3. 在 LSP 中接入 `textDocument/formatting`
4. 仅处理 `.ff` 源文件，暂不处理 manifest

## 分步计划

### 第一步：核心格式化模块

**新建文件**：

- `src/cli/formatter/formatter.h` — 公共 API
- `src/cli/formatter/formatter.c` — 核心格式化实现

**API 设计**：

```c
typedef struct FengFormatOptions {
    unsigned int tab_size;     /* 默认 2 */
    bool insert_spaces;        /* 默认 true */
} FengFormatOptions;

/* 格式化结果，调用方负责调用 feng_format_result_dispose 释放 */
typedef struct FengFormatResult {
    char *output;
    size_t output_length;
    bool changed;              /* 格式化后是否有变更 */
} FengFormatResult;

FengFormatResult feng_format_source(const char *source,
                                    size_t length,
                                    const FengFormatOptions *options);

void feng_format_result_dispose(FengFormatResult *result);
```

**实现要点**：

- 独立 tokenizer（不复用 `src/lexer/`，因现有 lexer 会跳过注释和空白）
- tokenizer 产出的 token 类型与 JS 版一致：keyword, identifier, number, string, boolean, operator, delimiter, punctuation, annotation, comment, newline, text
- 算法流程：tokenize → splitIntoLines → 逐行（indent 计算 + formatLineTokens）→ join
- 忠实移植 JS 版所有辅助函数：`needsSpaceBetween`, `isPrefixOperator`, `isPostfixPointerStar`, `looksLikeExplicitGenericOpen`, `updateDelimiterStack` 等

### 第二步：CLI 接入

**新建文件**：

- `src/cli/project/format.c` — `feng format` 子命令入口

**修改文件**：

- `src/cli/main.c` — 注册 `format` 命令路由
- `src/cli/cli.h` — 声明 `feng_cli_project_format_main`
- `Makefile` — 将 `src/cli/formatter/*.c` 和 `src/cli/project/format.c` 加入编译

**命令行为**：

```
feng format [<path>]
```

- 无 path：当前目录查找 `feng.fm`，格式化项目所有 `.ff` 文件
- path = 目录：递归格式化目录下所有 `.ff` 文件
- path = 文件：格式化单个文件
- 直接原地写入（in-place）
- 输出每个被格式化的文件路径

**实现模式**（参考 `check.c`）：

解析参数 → `feng_cli_project_find_manifest_in_ancestors` → `feng_cli_project_open` → 遍历 source_paths → 读取 → 格式化 → 比较 → 写入

### 第三步：LSP 接入

**修改文件**：

- `src/cli/lsp/runtime.c`：
  - `handle_initialize` 的 capabilities 中添加 `"documentFormattingProvider":true`
  - 在方法分发链中添加 `textDocument/formatting` 分支
  - 实现 `handle_formatting_request`：从 runtime 的 document store 获取源文本，调用 `feng_format_source`，返回 `TextEdit[]`

**LSP 响应格式**：

```json
[{"range":{"start":{"line":0,"character":0},"end":{"line":N,"character":M}},"newText":"..."}]
```

用单个 TextEdit 替换整个文档内容。

### 第四步：测试

**新建文件**：

- `test/cli/test_format.c` — 格式化单元测试

**测试策略**：

- 将 `editors/feng-vscode/test/formatter.test.js` 中的所有测试用例翻译为 C 测试
- 测试 `feng_format_source` 函数，验证输入/输出一致性

## 关键文件清单

| 操作 | 文件 |
|------|------|
| 新建 | `src/cli/formatter/formatter.h` |
| 新建 | `src/cli/formatter/formatter.c` |
| 新建 | `src/cli/project/format.c` |
| 新建 | `test/cli/test_format.c` |
| 修改 | `src/cli/main.c` |
| 修改 | `src/cli/cli.h` |
| 修改 | `src/cli/lsp/runtime.c` |
| 修改 | `Makefile` |

## 可复用的现有设施

- `feng_cli_read_entire_file`（`src/cli/common.c`）— 读取文件
- `feng_cli_project_find_manifest_in_ancestors`（`src/cli/project/common.c`）— 查找项目
- `feng_cli_project_open`（`src/cli/project/common.c`）— 打开项目、收集 .ff 文件
- `send_json_response`（`src/cli/lsp/runtime.c`）— LSP 响应
- `find_document`（`src/cli/lsp/runtime.c`）— 获取 LSP 文档

## 验证方式

1. 编译：`make clean && make`
2. 单元测试：`make test-cli && ./build/bin/test-cli`
3. CLI 手动验证：`feng format examples/` 格式化示例项目
4. LSP 验证：在 VSCode 中使用 Format Document 功能
