# Semantic LeakSanitizer 泄漏修复

## 现场

2026-09-26，统一开启泄漏检查并修复 Parser 后，既有 `test_semantic`
全部行为断言通过，但 LeakSanitizer 报告 411,002 字节／1,680 次分配泄漏。
日志：`third_party/llvm-c-eh/temp/parser-leaks/semantic-after.log`。
用户已批准将 sanitizer 检测出的真实缺陷一并修复。

当前分配栈集中在 enum 元信息、语义分析的临时数组和合成类型等路径。
先逐项确认资源所有者及释放时机，再复用对应所有者的销毁入口补齐释放。
不能因为测试最终退出而忽略分析器长期运行时会积累的泄漏。

## 已定位的所有权遗漏

1. `FengSemanticAnalysis` 拥有 enum 元信息及各 enum 的条目数组，但销毁函数
   没有释放它们；在该所有者的销毁入口补齐。
2. `ResolveContext` 拥有 lambda 捕获帧数组，但清理作用域时未释放数组。
   已发布到 AST 的 captures 由 AST 管理；仅清理尚在活动帧中的 captures，
   再释放帧数组，不触碰已经转移的所有权。
3. `reifiable_deps.c` 把完整深拷贝类型存入只按浅包装释放的池，遗漏嵌套泛型
   实参数组。复用现有两个所有权池：借用 AST 实参的包装仍存浅池，完整类型
   存入已有的完整树池；不新增描述符字段或跨层 API。
4. imported module 的类型树销毁缺少泛型实参递归清理；enum 合成的条目名称
   也未在正常销毁时释放。补齐各自所有者的清理函数，并复用到构建失败路径。
5. 另有两个既有 Semantic 负例未释放返回的诊断数组，已定位到普通／泛型
   `array_new_legacy_bracket_syntax_rejected`。这属于测试资源清理遗漏；修改前
   已获人工批准，保持源码及断言不变。
6. FT 读取器的字符串表从下标 1 开始保存，销毁却释放下标 0 到 `count-1`，
   每次漏掉最后一条字符串。按原有 1 基索引释放完整字符串表；不改变 FT
   内容、版本、加载语义或对外接口。

修复前四类遗漏后，同一测试集剩余 40,281 字节／223 次分配；其中 76 字节
属于第 6 项，其余来自既有测试自身的清理遗漏。已获批并补齐清理的测试如下：

| 函数 | 补齐的清理 |
| --- | --- |
| `test_defer_inside_function_is_accepted` | `feng_semantic_analysis_free(analysis)` |
| `test_defer_with_break_inside_nested_loop_is_accepted` | 同上 |
| `test_lazy_ambiguity_unused_no_error` | 同上 |
| `test_lazy_ambiguity_resolved_by_qualified_path` | 同上 |
| `test_lazy_ambiguity_resolved_by_alias` | 同上 |
| `test_type_field_static_method_same_name_allowed` | 同上 |
| `test_type_static_field_instance_method_same_name_allowed` | 同上 |
| `test_non_generic_array_new_legacy_bracket_syntax_rejected` | `feng_semantic_errors_free(errors, error_count)` |
| `test_generic_array_new_legacy_bracket_syntax_rejected` | 同上 |

它们均位于 `test/semantic/test_semantic.c`，不修改源程序或任何行为断言。
用户随后批准全部修复，以上清理调整已获授权。

补齐 FT 字符串清理后，macOS 原测试剩余 40,205 字节／221 次分配，全部来自
上表测试未释放的对象。Linux ARM64 原始 `make test` 也得到相同剩余报告。
Symbol 全套测试已开启 ASan、UBSan 和泄漏检测通过；112 个原有 LSP 用例
通过临时入口直接调用，源码与断言未变，也通过检测。这些针对性结果不能
替代最终完整回归；上述 9 个测试的清理现已按批准范围完成。

## 处理与验证

- [x] 取得带源码行号的调用栈并逐类确认所有权。
- [x] 记录并修复已定位的编译器所有权遗漏，不改语义、runtime、ABI 或 FT 格式。
- [x] 保留原有触发用例，获批后补齐清理，不更改源码和断言。
- [x] 原 Semantic 测试集在 macOS ARM64 与 Linux ARM64 的 ASan、UBSan 和泄漏检查下通过。
- [x] 继续全量 `make test` 并处理后续真实失败。

跨平台检查入口见[集成文档第 9.1 节](./feng-llvm-c-eh-integration-dev.md#91-统一泄漏检测2026-09-26)；
前置 Parser 修复见[独立记录](./feng-parser-leak-sanitizer-bugfix.md)。

最终两平台完整回归通过，结果与日志见[最终验收](./feng-llvm-c-eh-integration-dev.md#最终验收2026-09-26)。
