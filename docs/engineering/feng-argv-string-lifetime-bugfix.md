# main 参数字符串生命周期修复

## 复现与原因

2026-09-26，开启 ASan、UBSan、`detect_leaks=1` 后，CLI 的原生异常最小用例
运行结束报告 61 字节／1 次分配泄漏，栈为 `main → feng_string_literal →
feng_string_allocate`。日志：
`third_party/llvm-c-eh/temp/parser-leaks/native-reproducer-build-path.log`。

`cg_emit_main_wrapper` 使用 `feng_string_literal` 将原生 `argv` 转为 Feng 字符串。
该 API 按契约分配 immortal 字符串，`feng_release` 不释放它。main 结束时释放
`_args` 数组，元素随之失去引用，但并未回收；这些动态参数也没有字面量缓存。
这是产品生成代码的资源所有权问题，不是 sanitizer 误报。

原测试产物在 `temp/` 路径连续被信号 9 终止且无 sanitizer 输出；签名校验通过。
同一文件复制到 `build/` 后可运行并报告上述泄漏。系统日志含安全软件对原进程
的记录，但目前不足以断言该软件是终止源；不通过修改安全设置解决。

## 已批准的通用方案

用户在确认测试与产品泄漏的区别后，批准“全部修复，并进行全量回归测试”，
包含此前提交的 runtime 入口、必要生命周期成本及测试清理／注册方案。

新增 runtime 字符串构造入口 `feng_string_from_utf8(const char *, size_t)`：
复制输入字节，返回普通 +1 字符串。复用 `feng_string_allocate`，分配与拷贝次数
和原 `feng_string_literal` 一致；不改变已有字面量入口或字符串布局。
main 参数改用该构造入口，由 `_args` 元素及普通 ARC 路径管理生命周期。

产品调用点是 `src/codegen/codegen.c` 的 `cg_emit_main_wrapper`：生成的原生
`main` 逐项把 `argv[i]` 转成 Feng 字符串。`test/runtime/test_string_owned.c`
另直接调用该入口验证所有权。既有 `feng_string_from_utf8_bytes` 接收的是
`FengArray *`，继续服务于 std 的字节数组转换；原生 `argv` 是 `char *`，
使用它会先要求额外构造数组。新入口复用底层分配器，直接复制原生字节。

这会新增一个私有 runtime 函数，需要重编 runtime 与 Feng，并使动态参数的
retain/release 执行真实计数和最终释放。无新增分配或额外字节复制，但相对
immortal 的无操作计数存在必要的生命周期成本。按仓库规则，实施前由人工批准。

## 验证要求

- [x] 批准新增 runtime 入口及上述生命周期成本。
- [x] 覆盖空串、UTF-8、含 NUL 字节、独立副本与普通 ARC 回收；既有 literal
  immortal 契约保持。
- [x] 生成程序覆盖 main 省略返回类型／显式 void、参数读取／复制／重绑定，
  ASan、UBSan 和泄漏检测通过。
- [x] 完整 `make test` 通过，并在 Linux 验证。

## 验证记录

首轮正式新增 CLI 用例误用了 `main: i32`，被现有 AE0910 正确拒绝。
虽然 Codegen 仍有 i32 分支，当前语言入口规则只允许 void；修正新增用例为
省略返回类型与显式 void 两种合法形式，不变更语言规则或旧用例。

两平台 Runtime 单测通过。Linux ARM64 的生成程序四种组合通过，
ASan／UBSan／LSan 无报告；macOS 的后续 CLI 重跑在首个产物处遭信号 9，
同一文件复制到 `build/argv-lifetime-lsan` 后以相同参数及泄漏检查运行通过。
尚未将这项 macOS 环境失败记为整组通过。

最终完整重试中，两平台的上述四种生成程序组合均通过，`make test` 均退出 0。
macOS 沿用原测试路径，未再被信号 9 中断；结果与日志见
[最终验收](./feng-llvm-c-eh-integration-dev.md#最终验收2026-09-26)。
