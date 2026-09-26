# Codegen LeakSanitizer 泄漏修复

## 现场

2026-09-26，统一开启泄漏检测后的既有 `test_codegen` 行为断言通过，进程退出
报告 177,299 字节／4,999 次分配泄漏。日志保存在
`third_party/llvm-c-eh/temp/parser-leaks/codegen-after.log`。

用户已批准修复检测出的真实缺陷。先从分配栈定位资源所有者，在现有生成上下文
及临时对象的清理路径补齐释放；不改生成程序的语义或 runtime／ABI。
修复前后使用同一批既有用例与 sanitizer 验证，并继续全量 `make test`。
涉及既有测试修改或不确定的语义取舍时由人工决定。

## 已确认的所有权遗漏与修复方案

- 泛型 type/spec 实例拥有上下文泛参名称及约束索引，但 `cg_dispose` 未释放；
  enum 发码登记数组也未释放。补齐登记对象的统一销毁。
- defer 辅助函数临时建立的 void 返回类型在恢复上层上下文前未释放；
  `scope_add_defer` 复制了闭包名称，调用方却将复制误当成转移。补齐临时资源清理。
- materialize 内部函数已经把局部名写入 `ExprResult.c_expr`，又返回一份要求调用方
  释放的名称；大量仅检查成功与否的调用点遗漏该返回值。改为返回成功状态，
  名称统一由 `ExprResult` 持有。确实需要跨结果销毁保存名称的调用点显式复制，
  保留其原有释放路径。此为 Codegen 内部所有权调整，不改跨层 API 或生成 C 行为。
- reified binding 分支提前返回，遗漏普通 C 类型名临时字符串；将该字符串的构造
  放到实际使用它的普通布局分支。
- 数组／游标循环把拥有值放入 scope 后，没有销毁已不再使用的 `ExprResult`；
  scope 保存的是类型副本，不能省略原结果的销毁。统一在完成序列／游标准备后释放。
- `test_codegen.c` 的 9 个 literal adaptation 用例未释放生成 C 字符串与输入 AST。
  其中 AST 遗漏还有“短进程可泄漏”的旧注释。让辅助函数将 AST 返回给调用方，
  在每个用例结束时统一按 C 输出、analysis、AST 顺序清理；所有源码和断言保持。
  用户随后批准全部修复，这项既有测试调整已获授权。

## 验证

2026-09-26，原 Codegen 行为断言通过；修复上述编译器清理后，剩余报告为
119,928 字节／291 次分配，直接泄漏全部指向 9 个 literal adaptation 用例
未释放的输出及 AST，其余为这些 AST 所拥有的子节点。当时既有测试尚未修改；
用户随后批准全部修复，现已补齐这 9 个测试的清理，等待完整回归结果。
日志：`third_party/llvm-c-eh/temp/parser-leaks/codegen-after-all-compiler.log`。
Debug 全套和 CLI 路径测试的 ASan、UBSan、泄漏检查均已通过。
Linux ARM64 的 Codegen 结果同为 119,928 字节／291 次分配；Symbol、Debug
完整原测试通过。日志分别保存为该目录下 `linux-arm64-{codegen,symbol,debug}.log`。

- [x] Codegen 既有用例在 macOS ARM64 与 Linux ARM64 的 ASan、UBSan、`detect_leaks=1` 下全部通过。
- [x] Codegen 编译器分配栈中的剩余问题已逐一修复；生成程序另发现的中间值泄漏见[独立记录](./feng-call-result-leak-sanitizer-bugfix.md)。
- [x] 全量 `make test` 与 Linux 验证完成，记录实际结果。

最终两平台完整回归通过，结果与日志见[最终验收](./feng-llvm-c-eh-integration-dev.md#最终验收2026-09-26)。
