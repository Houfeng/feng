# 错误输出格式优化（Phase 1）

> 状态：待 Review  
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

### 3.2 编码格式

`{前缀}{4位数字}:`，例如 `SE0001:`、`AE0042:`

- 4 位数字从 `0001` 开始递增
- 错误码后必须跟 `:` 再跟消息文本
- 本阶段错误码可临时硬编码，Phase 2 再统一码表管理

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

- `src/cli/common.c` — `feng_cli_print_diagnostic` 函数，调整输出格式
- `src/cli/common.h` — 函数签名需增加错误码参数

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

Phase 1 新签名（`kind` 替换为 `error_code`）：
```c
void feng_cli_print_diagnostic(FILE *stream,
                               const char *path,
                               const char *error_code,
                               const char *message,
                               const FengToken *token,
                               const char *source,
                               size_t source_length);
```

- `kind` 参数（原值为 `"parse error"` / `"semantic error"` / `"codegen error"`）替换为 `error_code`（如 `"SE0001"`）
- 调用方负责传入完整的错误码字符串（含前缀和数字）
- 函数内部按新格式输出：第一行 `{path}:{line}:{col}`（无尾部冒号），第二行 `{error_code}: {message}`，后续为源码上下文

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
