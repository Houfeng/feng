# 泛型 spec 递归注册的存储寿命修复

## 范围与依据

2026-09-26，用户批准修复追加 ASan 后发现的真实问题。现有
`test_generic_sibling_constraint_codegen` 在 `cg_register_generic_spec_instance_shell`
报告 `heap-use-after-free`；原始证据见
[ASan 接入记录](./feng-llvm-c-eh-integration-dev.md#9-追加-asan2026-09-26)。

该函数保存 `cg->user_specs` 数组元素的局部指针，随后递归注册父 spec。
递归中的 `realloc` 释放原数组；全局引用刷新不能修改调用栈上的局部指针。
递归返回后读取 `s->form` 因此访问已释放存储。这是编译器内存错误，
不是 ASan 与异常展开不兼容，也不是需要绕过的测试诊断。

## 修复原则

在初始化新实例之后，递归收集所需的类别来自稳定的 AST 声明
`decl->as.spec_decl.form`，不再通过可能移动的数组元素读取。
统一覆盖父 spec、object 成员、callable 参数／返回类型、union 和 intersection
成员之间的递归，不按名称、实例数量、sanitizer 或用例设置例外。

保留数组增长、实例去重、全局引用刷新与注册顺序。只修复编译器内部对象的访问寿命，
不改变语言语义、生成程序的运行成本、runtime、ABI、FT 或插件协议。
不增加预留容量或关闭 ASan 来掩盖错误。

## 验证与交付

- [x] 新增独立 Codegen 源码矩阵，覆盖各类递归依赖及多轮数组扩容；生成 C 必须可编译。
- [x] 保留原触发用例和全部断言，在 ASan＋UBSan 下复验。
- [x] 对新增矩阵比较修复前后，确认能够检出此次缺陷。
- [x] 在沙箱外重新执行完整 `make test`，记录失败及最终结果。
- [x] 给出英文 commit message，等待人工 Review，不自动提交。

仅增加新测试入口注册，不修改既有用例和断言。若回归发现其他根因，先记录、分析，
不借此扩大产品变更范围。

## 实施记录

- 首次定向回归：全部既有 Codegen 用例在 ASan＋UBSan 下通过，原触发用例通过。
  新增矩阵的模块名误用了关键字 `spec`，Parser 按原规则报 SE0902；仅将新夹具
  模块名改为 `registration.growth`，不修改解析规则或既有用例。
- 新增 42 组矩阵的前端／Codegen 对照：保存的修复前对象在同一注册函数报告
  `heap-use-after-free`（退出 134），修复后全部通过（退出 0）。该隔离对照不执行
  C 编译回调；生成 C 可编译性由正式 Codegen 入口和后续 `make test` 验证。
  日志为 `third_party/llvm-c-eh/temp/asan-enablement/spec-growth-{before,fixed}.log`。
- 修复后重新执行沙箱外 `make test`：ASan＋UBSan 的全部 Codegen（含新增 42 组
  生成 C 编译）通过，随后 CLI/DAP 在 `test_cli.c:9491` 的循环断点 ID 断言失败。
  期待进入 `generic_owner`，实际再次命中 `generic_method` 的断点 8。
  本轮日志没有新的 ASan 内存错误报告；保留现场继续分析，不改断点用例或断言。
  全量日志为同目录的 `make-test-spec-fixed.log`，退出码 2，普通阶段尚未运行。
- 断点现场核对：`runGeneric` 中 `0x1000069b0` 与 `0x100006bfc` 都映射到
  `main.ff:90:5`；后者位于 `println` 和 ARC 清理之后的 ASan 栈 shadow 标记写入。
  前者行表带 `is_stmt`，后者不带，但 DAP 实际仍呈现额外停顿。原二进制、dSYM、
  协议日志及行表均保留在上述目录。这是独立的 sanitizer 调试位置问题，用户随后
  批准先修复，见[调试位置修复](./feng-sanitizer-debug-location-bugfix.md)。
- 两处实现修复后的新增 42 组注册用例在 sanitizer 和普通阶段均通过；完整
  `make test-normal` 退出 0。完整 `make test` 的后续 release 名称断言适配
  已获人工批准并完成。最终沙箱外完整 `make test` 退出 0，ASan＋UBSan 与
  普通阶段均通过；全部状态与证据收敛记录在上述调试位置修复文档中。
