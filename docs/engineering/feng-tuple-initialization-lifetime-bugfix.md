# Tuple 初始化生命周期修复

## 复现与原因

统一开启 LSan 后，CLI 跨包环用例的断言失败路径额外泄漏一个 TestState。
来源是 `ListIterator<T>.next` 返回 `(false, self.missing)` 时，在共享体中
构造 `IteratorResult<T>`。已用不依赖 std 的最小程序独立复现：

```feng
type Ref { let n: int; }
type Pair<T>(bool, T);
func pair<T>(value: T): Pair<T> { return (true, value); }
```

`pair<Ref>(Ref{n:3})` 在 Linux ARM64 报告 32 字节／1 次分配泄漏。
描述符大小的 tuple 缓冲已清零，随后又执行整体 default-zero 初始化，构造
默认 Ref；字段存储使用首次初始化语义直接覆盖指针，没有释放该默认值。
固定布局 tuple 使用清零后逐字段初始化，不存在这次额外整体默认构造。
修复为统一使用清零存储，再初始化全部显式字段；保留字段自身既有的转换
与所有权逻辑，不增加 runtime API、ABI、FT 或新的分配。

另一个独立最小复现确认：`(Ref{n:5}, fail())` 中后续字段抛异常，前面的
Ref 泄漏 32 字节。构造中的 tuple 尚未登记清理，只有成功返回结果后才由
消费者接管；固定布局和描述符布局需要一致的构造中生命周期保护。
应复用现有作用域清理，保证清零的未初始化字段可安全释放；不可仅针对 std、
IteratorResult 或 Ref 添加特判。此项需要增加原来遗漏的必要清理成本，
与跨包环回收一起提交人工确认后实施。

用户在了解上述必要成本后要求“继续将所有问题修复（生产级）”，现按该授权
实施构造期保护并补齐覆盖；不再重复请求同一项确认。

具体方案：在 tuple 字段求值期间使用内部构造作用域，清零后即登记整个聚合
存储；后续字段异常由现有 unwind 清理释放已填入字段。成功时按现有 LIFO
顺序释放字段求值临时值，tuple 自身仅撤销登记、保留原 +1 给消费者接管。
复用 `Scope`、`cg_materialize_to_local`、`cleanup_pop_only` 和现有清理发码，
固定及描述符布局走同一生命周期流程。正常路径增加构造窗口中的必要清理
push/pop；不因保护本身增加 retain/release 配对或堆分配，也不增加 runtime API。

证据保存在 `third_party/llvm-c-eh/temp/parser-leaks/linux-tuple-focus.log`
和 `linux-tuple-partial.log`。macOS 同一最小程序本次被 SIGKILL，未获得
sanitizer 报告；按用户要求保留记录、仅重试，不因此修改代码或安全配置。

## 验证任务

- [x] 移除描述符布局 tuple 的重复整体默认构造。
- [x] 按确认方案补齐构造期间的正常／异常所有权保护。
- [x] 覆盖固定／描述符布局、借用／拥有字段、指针／聚合／标量、嵌套与后续字段异常。
- [x] 保留原用例，执行 macOS、Linux 的 ASan／UBSan／LSan 及完整 `make test`。

移除重复默认构造后的正常路径矩阵已在 Linux ARM64 默认／release 两种模式
通过 ASan、UBSan、LSan，覆盖函数级／类型级共享体、指针、value、spec、闭包、
数组、字符串、标量和嵌套 tuple；计数要求所有显式构造的对象全部且仅释放一次。
矩阵已固化为 `test/cli/tuple_lifetime.inc`，构造中途异常项随后按授权补齐。
macOS 重试仍被 SIGKILL，未把它记为测试通过。

macOS ARM64 与 Linux ARM64 全部既有 Codegen 用例随后通过 ASan／UBSan／LSan，
未修改其结构断言；日志为 `tuple-codegen-run.log` 和 `linux-tuple-codegen-run.log`。
这仍不能替代待完成的完整回归。

构造期保护已实现：固定／描述符布局共用初始化循环和作用域清理，结果存储
在保护块外声明，保护节点在块内声明，成功撤销后可由消费者按原规则接管。
Linux ARM64 新增异常矩阵在默认／release 均通过 ASan／UBSan／LSan，覆盖
后续字段异常、借用字段、嵌套 tuple、value/spec/数组字段及构造中提前返回；
原跨包泛型环 CLI 用例也一并通过。日志为 `linux-remaining-focus.log`。

最终两平台完整回归通过，包含默认／release 的完整初始化和异常矩阵。
结果与日志见[最终验收](./feng-llvm-c-eh-integration-dev.md#最终验收2026-09-26)。
