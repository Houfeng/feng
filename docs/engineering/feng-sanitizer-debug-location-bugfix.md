# Sanitizer 辅助指令的源码断点修复

## 范围与现场

2026-09-26，用户批准先修复本次追加 ASan 后发现的 DAP 断点错误。
`make test` 的 ASan＋UBSan 阶段在既有循环断点矩阵失败：`runGeneric`
的一次循环迭代中，同一 `println` 行被命中两次，第二次位于调用及 ARC 清理
之后的 ASan 栈 shadow 标记指令。原测试要求 12 个场景、24 次停靠。

原始二进制、dSYM、协议日志、行表和全量日志保存在
`third_party/llvm-c-eh/temp/asan-enablement/`。额外地址具有同一源码行号，
行表未设置 `is_stmt`，但 LLDB 仍将其解析为独立断点位置。初步现场不足以
仅凭该标志推断根因；随后核对生成 C 的存储／作用域与 LLVM 调试元信息，
确认下述调试上下文遗漏。

本问题与此前 CI 的 `setpgid` 启动断言失败不同；后者尚缺少失败时的
系统错误码，不在本修复中一并改动。

## 修复与验证约束

源码断点语义沿用 [CLI 调试规范](../specifications/feng-cli.md#22-feng-dap)。
从共用调试信息生成路径修复；禁止按函数名、泛型用例、源码行号或
sanitizer 辅助函数名添加特判，禁止在代理层按行号吞掉真实停靠。
保留 ASan＋UBSan、用户变量调试信息、真实控制流与全部既有断点断言。
不调整优化级别或关闭栈存储检查，不改变 runtime、ABI 或 FT。

- [x] 确认生成 C、LLVM IR、DWARF 和真实 DAP 停靠之间的根因链。
- [x] 记录确定的通用方案后修改实现，核对生成程序行为与开销。
- [x] 增补针对性覆盖，并运行原断点、单步及变量读取测试。
- [x] 在沙箱外运行完整 `make test`，记录所有失败及最终结果。
- [x] 给出英文 commit message，等待人工 Review，不自动提交。

遇到独立问题先记录和分析；额外范围或无法确定的方案由人工决定。

## 根因与实现方案

共享方法入口没有建立当前调试 frame，`cg_debug_local_attribute` 因此直接
返回空属性，已有的内部存储可见性规则未生效。生成 C 中 `_call_arg2`、
`_varr3`、返回值临时量等被当作普通可调试变量。LLVM IR 据此保留该语句的
独立 `DILexicalBlock`；ASan 在其末尾生成生命周期标记指令后，LLDB 将相同
行号、不同词法范围的位置分别解析为断点。普通方法和泛型自由函数已有
frame 上下文，没有触发这处遗漏。

修复补齐源码 callable 的调试 frame 上下文，复用现有变量映射与延后解析的
局部属性机制：保留用户绑定，纯内部临时存储沿用 `FENG_CODEGEN_NODEBUG`。
类型级／方法级泛参的共享方法使用同一个入口；builtin fit 入口同样遗漏了
frame 建立，也接入相同机制。保留这些入口既有的 backend frame 显示名，
不附带修改展示规则。退出入口时恢复上下文，避免污染后续 callable。不得通过隐藏
整个共享体或移除用户局部变量解决问题。

变更只涉及编译期调试元信息；原 C 存储、调用、清理和 ASan 检查保留。
需要以生成 IR 及真实断点验证，而非仅比较 C 文本或放宽停靠次数。

## 实施记录

- 首次 CLI 定向回归尚未执行到 DAP：既有 native EH 对照的默认构建产物被
  `SIGKILL` 终止，随后 `test_cli.c:924` 的退出码断言失败。日志为
  `third_party/llvm-c-eh/temp/asan-enablement/dap-cli-first-fixed.log`。
  暂无 ASan／UBSan 报告，不能直接归因为安全软件或本次修复；保留产物，
  检查签名、重试相同产物，并独立运行原 DAP 用例定位。
- 补齐共享体上下文后，原循环断点矩阵恢复 12 场景、24 次停靠，表达式与变量
  读取矩阵通过。初稿附带将 frame 名改成 `Factory.run`，被原 lambda 单步
  测试的名称断言发现；已撤回该展示变化，保留原 backend 名称和全部断言。
  该次失败不作为完整 DAP 通过结论。
- 保持原显示名后，独立调用全部原循环断点、表达式值、lambda 单步与程序退出
  单步测试：12 场景／24 次停靠、37 次有序停靠、9 个 lambda 场景和 9 个退出
  会话均通过，退出 0，日志为 `dap-focused-second.log`。这不替代完整回归。
- 新增 10 类 callable 入口矩阵，覆盖普通方法、方法泛型、类型泛型、组合泛型、
  静态方法、值类型及 scalar／array／generic／nominal fit。检查用户绑定可见、
  编译器实参临时量隐藏，以及生成 lambda／defer 后恢复外层绑定；每例生成的 C
  均须编译。新夹具最初错误使用 `()->{...}`，Parser 按现有规则报 SE0514；
  仅修正新夹具为 `(){...}`，不改语法规则。
- 新矩阵在保存的修复前 Codegen 对象上失败，原因是方法泛型没有源码绑定；
  修复后全部通过。日志为 `debug-matrix-{before,fixed}.log`。
- 再次执行完整 `make test`：全部 Codegen、新矩阵、源码断点及 DAP/LSP 检查
  已通过，随后 `test_cli.c:21709` 的 release 二进制名称清理断言失败，退出 2。
  该断言扫描二进制的全部字节，而非仅符号表；需先确认名称来自真实符号还是
  sanitizer 元信息，再确定处理方式。原断言保持不变，普通阶段尚未执行。
  日志为 `make-test-final.log`，现场为
  `temp/feng_cli_release_binary_cleanup_cmeX8e/`。
- 已确认 release 符号表中没有上述类型描述符或被检查的函数符号；残留的两个
  名称位于 `__TEXT,__cstring`，分别由 `__DATA,__asan_globals` 的 name 字段引用，
  是 ASan 对 `__rgp`／`__rfo` 全局数组的诊断名称。记录地址 `0x10002b198`、
  `0x10002b1d8` 经 Mach-O chained fixup 指向 `0x10001b446`、`0x10001b480`。
  证据为 `release-cleanup-symbols.txt` 与 `release-cleanup-asan-metadata.txt`。
  运行输出及跨包异常已在触发断言前通过；这不是新发现的内存错误。
- 用户在确认原因后批准优化该测试：用例中检查函数／描述符符号保留或移除的 6 处断言
  统一读取 `nm` 符号表；普通与 sanitizer 阶段共用，不增加 ASan 特判。保留原
  Feng 源码、构建模式、跨包调用、运行输出，以及普通字面量的原始二进制扫描。
  复用现有测试中的 `llvm-nm`／`nm` 查找方式，不修改产品发码或删除 ASan 元信息。
  普通与 sanitizer 构建均执行全部原有正负断言，不跳过 release 检查。
- 等待测试适配决定期间，独立执行剩余 sanitizer 目标，退出 0：CLI paths／symbol、
  91 个 smoke、CLI 直接模式／项目模式／初始化、std 607/607、FCTS 1619/1619、
  性能门槛和 LLVM C EH 集成均通过。日志为 `sanitize-remaining.log`。
  这不替代被上述断言中断的完整 `make test`。
- 独立执行完整 `make test-normal`，退出 0，日志为 `test-normal-final.log`。
  包含全部原有 CLI/DAP 断言、release 符号清理、新增 52 组 Codegen 用例、
  std、FCTS、性能及发行脚本验证。此前被 SIGKILL 的 native EH 用例在本次
  全新构建后已通过；未修改签名、安全设置、测试源码或断言，首次 SIGKILL
  的具体原因仍未确认。
- 批准前的交付状态：本次调试位置及 spec 注册寿命实现已修复，普通阶段完整通过；
  完整 `make test` 仍为失败。sanitizer 下 `test_cli` 在该断言之后的用例尚未
  全部执行，不能将分段结果合称全量通过。批准后完成上述适配，再重新执行完整
  `make test`，以其实际退出状态作为最终验收依据。
- 最终验收：在沙箱外执行完整 `make test`，退出 0，日志为
  `third_party/llvm-c-eh/temp/asan-enablement/make-test-symbol-table.log`。
  ASan＋UBSan 与普通阶段均通过全部原有及新增检查，包括 release 符号清理、
  Codegen 52 组新增用例、CLI/DAP/LSP、std 607/607、FCTS 1619/1619、
  性能门槛与 LLVM C EH 集成；普通阶段还通过增量构建及发行脚本验证。
  未修改 runtime、ABI、FT、插件或原断点断言，未关闭 sanitizer 检查。
  实现及本地全量回归完成，等待人工 Review，不自动提交。
