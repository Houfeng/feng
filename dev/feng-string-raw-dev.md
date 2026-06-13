# Feng 反引号字符串字面量开发任务

## 规范阶段

- [x] 编写反引号字符串字面量规范文档 `docs/feng-string-raw.md`
- [x] 明确语法规则（界定符、无 `\` 转义、`` `` `` 嵌入反引号、换行原样保留、空字符串合法）
- [x] 明确 Codegen 处理方案（通过 `FENG_TOKEN_RAW_STRING` token kind 区分）

## 实现阶段

- [ ] Token：在 `src/lexer/token.h` 中新增 `FENG_TOKEN_RAW_STRING` token kind
- [ ] 全量回归测试（Token kind 变更后）
- [ ] Lexer：在 `src/lexer/lexer.c` 中增加反引号字符串扫描逻辑（识别 `` ` `` 界定符、处理 `` `` `` → `` ` ``、去掉首尾界定符、输出 `FENG_TOKEN_RAW_STRING`）
- [ ] 全量回归测试（Lexer 变更后）
- [ ] Parser：在 `src/parser/parser.c` 中将 `FENG_TOKEN_RAW_STRING` 映射为 `FENG_EXPR_STRING` AST 节点
- [ ] 全量回归测试（Parser 变更后）
- [ ] Codegen：在 `src/codegen/codegen.c` 中区分 `FENG_TOKEN_RAW_STRING`，不走转义解码循环，直接传字节数组
- [ ] 全量回归测试（Codegen 变更后）

## 测试阶段

- [ ] 编写正向测试：合法反引号字符串（正则、多行文本、`` `` `` 嵌入、空字符串）
- [ ] 编写负向测试：未终止反引号字符串的错误诊断
- [ ] 运行全量回归测试，确保未破坏现有功能

## 收尾阶段

- [ ] 检查 lint 错误，确保修改文件无新增问题
- [ ] 给出建议的 commit message
