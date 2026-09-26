# 调用结果的字符串所有权泄漏

## 现场

2026-09-26，Linux ARM64 的 ASan、UBSan、`detect_leaks=1` smoke 回归中，
`test/smoke/phase1a/tuple.ff` 报告 76 字节／2 次分配泄漏。两次分配均来自
`Label.show` 的字符串拼接，经 object-form spec 的方法 thunk 返回给调用方；
其他 90 个 smoke 用例通过。先保留原源码和断言，分析所有权移交与释放路径。

日志位于 Linux 隔离容器 `/repo-generated-lsan-tests.log`。
此问题与 DAP/ptrace 的工具限制不同，是生成程序实际分配未释放的报告。
用户已要求检测出的真实缺陷全部修复；涉及新的 runtime API、ABI 或不确定
的性能取舍时仍由人工决定。

## 已确认原因与修复方案

泄漏实际来自 `self.item1 + ":" + self.item2` 的中间结果。生成 C 为
`feng_string_concat(feng_string_concat(left, colon), right)`；内层调用返回 +1，
外层调用借用它，但无人保存和释放该 +1。方法返回值及 spec thunk 调用方的
清理本身存在，问题位于 `cg_emit_binary` 的操作数生命周期。

在非短路二元操作的公共操作数发码路径，按所有权统一接管拥有引用的托管／
聚合临时值，复用 `cg_materialize_to_local` 与现有作用域清理。在求值左操作数后、
求值右操作数前登记左侧清理；右侧同理。正常退出释放临时值，右侧抛异常时
由已有异常清理回收左侧。覆盖拼接、相等性及聚合值；不为 tuple、spec 或
具体源码表达式添加特判，不新增 runtime API 或描述符。

这会对原先遗漏清理的临时值增加必要的 cleanup 登记及释放，不能声称完全没有
运行成本。复用既有所有权转移避免不必要 retain；已有 spec 比较的 materialize
需要统一处理，避免重复复制和清理。2026-09-26 用户明确要求修复，批准补齐
必要的清理登记与释放；先用本轮修改前的提交复现，确认缺陷来源。
新增覆盖包括嵌套拼接、双侧临时值、比较结果、短路右侧、循环及右侧异常；保留
原 tuple smoke 源码和断言。

## 实施与验证

- [x] 使用本轮修改前的 `cca35c64` 编译器复现并保留生成 C 和泄漏报告。
- [x] 复用公共所有权处理，补齐二元操作数的正常及异常清理。
- [x] 增加嵌套拼接、相等性、左右临时值、短路、循环和异常覆盖。
- [x] 保留原用例，在 macOS 与 Linux 上执行 ASan、UBSan、LSan 验证及完整 `make test`。

基线验证：从 `cca35c64` 独立构建未修改的编译器及 runtime，以相同 LLVM
22.1.8、补丁 LLD 和 ASan／UBSan 编译原 `tuple.ff`，开启 LSan 执行。
两条 `Label.show` 调用路径分别泄漏 38 字节，合计 76 字节，与本轮报告一致；
另有 77 字节来自旧版 argv 的 immortal 构造，应单独归因。因此二元表达式
泄漏是固有缺陷，不是本轮资源释放改动引入。日志与生成 C 保存在
`third_party/llvm-c-eh/temp/parser-leaks/baseline-tuple-*`。

专项验证：修复后的原 `tuple.ff` 已通过 ASan／UBSan／LSan，原输出保持一致，
未再报告中间拼接或 argv 泄漏。新增矩阵初稿误用了保留字 `spec` 作为局部变量名，
按既有语法改为 `viewed`，不修改解析规则。

新增析构计数初稿把聚合返回槽中的默认零值对象也计入释放次数，导致构造计数
26、析构计数 30。生成 C 及调试验证确认这四次来自既有默认初始化。测试改为
只统计显式构造的非零编号对象，仍要求这些对象全部且仅析构一次；不改初始化语义。

新增 Codegen 断言检查正常释放、异常清理入口、左侧登记先于右侧调用、借用
比较不增加 retain，以及拥有值转移不增加 retain。CLI 矩阵通过真实 driver
编译默认／release 两种产物，覆盖字符串、对象、数组、闭包、spec、value、tuple、
泛型返回、条件／match、正常／异常、短路、while／for 和 continue／break。
macOS 专项执行已通过；整体结果以完整回归记录为准。

最终两平台完整回归通过，结果与日志见[最终验收](./feng-llvm-c-eh-integration-dev.md#最终验收2026-09-26)。
