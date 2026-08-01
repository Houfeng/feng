# 错误码统一管理（Phase 2）

> 状态：待 Review  
> 日期：2026-06-13  
> 前置依赖：[Phase 1 - 错误输出格式优化](feng-error-optimize-phase1-draft.md)

## 1. 背景

Phase 1 完成了错误输出格式的微调，但错误码仍为各调用点硬编码的临时常量。随着错误数量增长，需要建立统一的错误码管理机制，以实现：

- **单一事实来源**：所有错误码及其消息模板集中定义，避免分散和不一致
- **国际化就绪**：消息模板与错误码解耦，未来替换为 i18n lookup 仅需改一处
- **编译期安全**：通过枚举/宏定义错误码，拼写错误可在编译期捕获
- **工具链友好**：LSP/IDE 插件可直接从码表生成 diagnostic code 和文档链接

## 2. 目标

1. 建立统一的错误码注册表，固定在代码中管理
2. 提供 `logError(错误码, 参数列表)` 辅助函数，替代手动拼接字符串
3. 全量迁移所有错误报告点到新的辅助函数
4. 保持 Phase 1 确定的输出格式不变

## 3. 错误码编码规则

### 3.1 前缀定义

| 前缀 | 含义 | 对应阶段 | 数值范围（建议） |
|------|------|----------|------------------|
| LE | Lexical Error | 词法分析 | 0001–0999 |
| SE | Syntax Error | 语法分析 | 1001–1999 |
| AE | Analysis Error | 语义分析 | 2001–2999 |
| CE | Codegen Error | 代码生成 | 3001–3999 |

> 注：数值范围为建议值，便于按阶段快速定位。实际分配以码表为准，不强制连续。

### 3.2 编码格式

`{前缀}{4位数字}`，例如 `SE0001`、`AE2042`

- 错误码在代码中以枚举或宏常量表示
- 输出时自动追加 `:` 和消息文本

## 4. 码表设计

### 4.1 码表结构

每个错误码条目包含以下字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| code | 枚举值 | 唯一标识，如 `FENG_ERR_SE0001` |
| prefix | 字符串 | 前缀，如 `"SE"` |
| number | int | 4 位数字，如 `1` |
| message_template | 字符串 | 带占位符的消息模板，如 `"expected '{0}', got '{1}'"` |

### 4.2 码表定义位置

Feng 编译器遵循**严格的单向依赖**原则：语义阶段可依赖语法阶段输出，但不能反向直接依赖（除非是精心设计的抽象接口注入）。因此码表必须放在独立于所有编译器阶段和 cli 的位置。

新建 `src/diagnostic/` 目录，作为诊断信息基础设施：

- `src/diagnostic/diagnostic.h`：声明错误码枚举、码表查询接口和辅助函数原型
- `src/diagnostic/diagnostic.c`：定义码表数组和辅助函数实现

**依赖方向**（严格单向）：
```
cli ──→ diagnostic ←── lexer / parser / semantic / codegen
```

- `diagnostic` **不依赖任何其他模块**，仅使用 C 标准库和基本类型
- 所有编译器阶段（lexer、parser、semantic、codegen）和 cli 均可单向依赖 `diagnostic`
- `diagnostic` 专注于诊断信息的格式化与输出，不包含任何编译器业务逻辑
- 具体的终端适配（颜色检测、stream 判断等）由 cli 层通过回调或参数注入，`diagnostic` 本身不做终端假设

### 4.3 消息模板占位符

使用 `{0}`、`{1}`、`{2}` ... 作为占位符，与 Feng 标准库 `string.format` 风格保持一致。

示例：
```c
// 码表条目
{ FENG_ERR_SE0001, "SE", 1, "expression statements and local bindings must end with ';'" }
{ FENG_ERR_AE2001, "AE", 2001, "undefined identifier '{0}'" }
{ FENG_ERR_AE2002, "AE", 2002, "type mismatch: expected '{0}', got '{1}'" }
```

## 5. 辅助函数设计

### 5.1 函数签名

`diagnostic` 模块不依赖任何其他编译器模块（包括 `lexer/token.h`），因此位置信息通过基本类型传递：

```c
void feng_diagnostic_error(FengErrorCode code,
                           const char *path,
                           unsigned int line,
                           unsigned int column,
                           const char *source,
                           size_t source_length,
                           ...);
```

- `code`：错误码枚举值
- `path`/`line`/`column`：位置信息（从 `FengToken` 中提取后传入，`diagnostic` 本身不引用 `FengToken`）
- `source`/`source_length`：源码上下文，用于渲染上下文行和 `^` 指针
- `...`：可变参数，对应消息模板中的占位符

调用方（如 parser、semantic）负责从 `FengToken` 中提取 `line`/`column` 后传入，`diagnostic` 模块对 `FengToken` 无任何感知。

### 5.2 内部行为

1. 根据 `code` 查找码表，获取前缀、数字和消息模板
2. 用可变参数填充消息模板中的占位符
3. 按 Phase 1 格式输出到 stderr：`{前缀}{数字}: {填充后的消息}\n-> {path}:{line}:{col}:\n{上下文}`
4. **`diagnostic` 模块不做任何终端适配**（不检测颜色支持、不调用 ANSI 转义序列）。如需颜色高亮，由 cli 层在调用 `feng_diagnostic_error` 前后自行处理，或通过回调注入

### 5.3 与现有函数的关系

- `feng_cli_print_diagnostic` 可保留作为 cli 层的封装，内部提取 token 位置后转发到 `feng_diagnostic_error`，并在外层处理颜色
- 或直接废弃 `feng_cli_print_diagnostic`，各调用点直接使用 `feng_diagnostic_error`（需评估影响范围）

## 6. 迁移策略

### 6.1 迁移步骤

1. 定义错误码枚举和码表
2. 实现 `feng_diagnostic_error` 辅助函数
3. 逐个模块迁移错误报告点（lexer → parser → semantic → codegen）
4. 每迁移一个模块，运行全量回归测试确认无破坏
5. 全部迁移完成后，移除旧的 `feng_cli_print_diagnostic`（如选择废弃）

### 6.2 测试用例更新

- 现有测试用例中硬编码的错误消息断言需同步更新为新格式
- 修改测试用例需获得明确的人工批准
- 建议按模块分批更新，每批更新后立即验证

## 7. 国际化预留

### 7.1 当前阶段

消息模板以英文硬编码在码表中，不做 i18n。

### 7.2 未来扩展

当需要国际化时：
- 将码表中的 `message_template` 替换为 message key
- 增加语言资源文件（如 `messages_en.json`、`messages_zh.json`）
- `feng_diagnostic_error` 内部根据当前 locale 查找对应语言的消息模板
- 调用方代码无需任何改动

## 8. 不在本阶段范围

- warning / info 级别（LW/SW/AW/CW）
- help 提示行
- 实际的国际化实现
- 错误码文档网站自动生成

## 9. 风险与注意事项

- **性能**：码表查找应为 O(1)（数组索引或完美哈希），避免运行时开销增大
- **线程安全**：码表为只读静态数据，天然线程安全；消息格式化使用栈上缓冲区
- **向后兼容**：LSP 插件需适配新的 diagnostic code 提取逻辑
- **测试覆盖**：每个错误码应有对应的 conformance test 或 negative test 覆盖
