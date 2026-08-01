# Feng 字符串 `\xNN` 转义开发任务

## 规范阶段

- [x] 编写字符串转义序列规范文档 `docs/specifications/feng-string-escape.md`
- [x] 明确 `\xNN` 语法规则（恰好 2 位十六进制、不足报错、支持连续书写）
- [x] 明确 Codegen 解码实现方案并写入规范文档

## 实现阶段

- [x] Lexer：在 `src/lexer/lexer.c` 转义 switch 中增加 `case 'x'`，验证后跟恰好 2 位十六进制数字
- [x] 全量回归测试（Lexer 变更后）
- [x] Codegen：在 `src/codegen/codegen.c` 字符串解码循环中增加 `case 'x'` 分支及辅助函数 `is_hex_digit` / `hex_value`
- [x] 全量回归测试（Codegen 变更后）
- [x] 更新 `examples/hello_world/src/main.ff` 中的示例代码，使用 `\x1b` 替代 `\u001b`
- [x] 全量回归测试（示例更新后）

## 测试阶段

- [x] 编写正向测试：合法 `\xNN`（ASCII、UTF-8 多字节、连续书写）
- [x] 编写负向测试：非法格式（不足 2 位、非十六进制字符）的错误诊断
- [x] 运行全量回归测试，确保未破坏现有字符串解析行为

## 收尾阶段

- [x] 检查 lint 错误，确保修改文件无新增问题
- [x] 给出建议的 commit message
