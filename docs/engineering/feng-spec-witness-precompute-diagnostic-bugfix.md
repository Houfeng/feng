# witness 预计算冲突诊断崩溃修复

状态：修复、专项验证与全量回归完成（2026-10-29）。

## 1. 问题与范围

加载外部包后，`precompute_imported_builtin_spec_witnesses` 为内建类型和数组
预计算 witness。该阶段使用的 `ResolveContext` 没有词法文件和诊断收集字段；
遇到多个匹配实现时，`compute_spec_witness_if_absent` 调用
`resolver_append_error`，在读取 `context->program->path` 时发生空指针崩溃。

已用 `feng 0.1.0-rc43` 在 macOS arm64 复现：为同一 spec 提供相同签名方法的
`fit int` 与 `fit i64` 在加载 std 包时触发崩溃，普通 fit 和 open fit 均可触发；
`fit i32` 与 `fit i64` 的对照编译成功。两个目标都写成 `i64` 也可触发，
因此问题不局限于类型别名。

本次只修复已有冲突诊断路径，不改变语法、满足关系、可见性、候选匹配、
witness 缓存或检查触发时机，不修改 Codegen、FT 格式、runtime 或 ABI。
语言规则继续以 [fit 规范](../specifications/feng-fit.md)、
[spec 规范](../specifications/feng-spec.md) 和
[AE 诊断码规范](../specifications/feng-error-codes-ae.md) 为准。

## 2. 修复方案

- 预计算上下文接入当前语义分析的错误数组、数量和容量；产生错误后沿用分析入口
  的失败返回和清理流程，不把有冲突的分析结果交给后续阶段。
- witness 计算显式接收诊断源文件路径，与现有诊断 token 一起传递。按需计算使用
  原调用文件，预计算使用触发关系的 fit 所属文件，递归父 spec 沿用该位置。
- 冲突分支通过已有诊断追加函数记录 `AE0804`，保留原消息及候选判断。
  诊断来源不写入用于类型解析的 `context->program`，避免改变预计算的解析环境。
- 不通过跳过诊断、忽略冲突或填入无关文件来规避崩溃。

## 3. 测试与验收

在 `test/semantic/` 独立新增测试并注册到现有入口，不修改已有用例的输入或断言。
本问题属于编译器诊断路径，不新增语言行为规范或 FCTS 行为。

覆盖内容：

- 加载导入模块时，普通/open fit 的重复实现报告 `AE0804`，返回失败且不返回分析结果。
- 32 位与 64 位目标下的 `int` 别名、显式相同内建目标及数组目标冲突。
- 多文件与文件顺序变化，断言诊断码、数量、文件路径、行列和 token。
- 导入模块中的 fit 及父 spec requirement 冲突，验证诊断来源和递归位置传递。
- 不同内建目标、不同数组目标与唯一实现正常通过；无导入包的按需诊断仍保持原行为。
- 专项测试验证修复前失败、修复后通过；最后在沙箱外执行 `make test`，包含
  ASan/UBSan 与普通阶段。

## 4. 验证结果

验证环境：macOS arm64，Homebrew Clang 23.1.3。

- 新增 61 个 Semantic 用例全部通过；已有测试输入和断言未修改。
- 将新用例链接到修复前的 `analyzer.c`，UBSan 在原第 3954 行报告对空
  `FengProgram` 的成员访问，随后 ASan 报零地址读取，进程退出码为 134。
- 用重建的 ASan/UBSan 编译器加载真实 `std-0.1.0.fb`：open 别名冲突、普通
  fit 别名冲突、显式相同 `i64` 目标冲突均输出 `AE0804`，位置指向对应源文件
  的 fit 目标；退出码均为 1，无信号终止或 sanitizer 报错。
- 沙箱外隔离副本执行 `LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 make test`，
  退出码为 0；ASan/UBSan 与普通阶段均完整通过。

| 检查 | ASan/UBSan 阶段 | 普通阶段 |
| --- | --- | --- |
| 新增 witness 诊断用例 | 61/61 | 61/61 |
| smoke | 91/91 | 91/91 |
| 标准库 | 611/611，失败与跳过为 0 | 611/611，失败与跳过为 0 |
| FCTS | 1732/1732，失败与跳过为 0 | 1732/1732，失败与跳过为 0 |
| 编译器、Runtime、CLI、LSP 生命周期、Symbol、性能约束 | 通过 | 通过 |

普通阶段的增量构建、发布/安装、macOS 签名流程模拟、内置包、预编译工具链
恢复、异常插件和发布链接检查也均通过。`git diff --check` 通过。

本轮日志与隔离副本位于 `temp/witness-fix-verify/`：完整日志为
`make-test.log`，修复前后专项日志为 `baseline-focused.log` 和
`fixed-focused.log`，真实包复现结果为 `cli-reproduction.json`。副本中的三份
代码/测试文件在回归前后均与主工作区逐一比对一致；隔离执行避免现有回归用例
清理 `temp/` 时删除本轮证据。
