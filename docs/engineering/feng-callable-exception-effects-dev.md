# Feng callable 异常集合分析与 N08 后续优化方案

> **状态**：2026-09-21，记录已确认问题和后续方向，具体方案待 Review，尚未实施。
>
> **人工决定**：先完成[嵌套 catch 生命周期修复](./feng-nested-catch-exception-lifetime-bugfix.md)；
> N08 独立跟踪，触发用例完整保留并注释。后续记录函数、方法等 callable 可能向外
> 传播的异常，为 N08 的处理及将来提醒用户显式处理异常提供依据。

## 1 范围与规范归属

本任务包含两个相关目标：明确并解决 N08 的 defer 异常逃逸问题；建立可跨调用、
跨包使用的异常集合分析。本文记录事实、设计方向和待决策项，不直接修改语言规则。

异常传播及具体类型匹配以[异常规范](../specifications/feng-exception.md)为准；
清理块限制以 [defer 规范](../specifications/feng-defer.md)为准。
后续规则通过 Review 后更新对应主规范，本文只引用其结论。

## 2 N08：展开期间 defer 再次抛出且逃逸

### 2.1 复现形态

```feng
module n08_example;

/** 用 code 区分原异常与清理异常。 */
type Error { let code: int; }

/** 向调用方抛出独立异常。 */
func fail(code: int): void { throw Error { code: code }; }

/** 原异常退出函数时，defer 调用再次抛出且没有内部处理。 */
func work(): void {
    defer { fail(2); }
    fail(1);
}

/** 当前实现会在进入该 catch 之前因清理链损坏而终止。 */
func main(args: string[]): void {
    try work() catch error: Error {
        // 这里最终应如何处理，取决于后续审定的 defer 异常逃逸规则。
    }
}
```

异常 A（`code == 1`）触发 `work()` 的栈展开；清理过程中执行 defer，异常 B
（`code == 2`）又启动一次向外展开。当前 runtime 在 personality 回调中执行函数
清理，因此第二次 throw 时，第一次 throw 及其 unwinder 仍在物理调用栈上。

LLDB 确认的关键调用顺序为：

```text
feng_throw(B)
  → fail(2)
  → defer 生成函数
  → __feng_personality_v0
  → unwind_phase2 / _Unwind_RaiseException(A)
  → feng_throw(A) / fail(1)
  → work()
  → 调用方
```

现有 `feng_cleanup_release_to_frame_marker` 按清理链顺序移除一个 function marker，
没有记录哪些 native frame 已在第一次展开中清理。第二次展开再次经过这些帧时，
会错误移除外层 try 边界，最终报告边界不在清理链上。仅忽略缺失边界不能修复资源
归属，也没有解决被中断的原异常记录应由谁持有和释放的问题。

### 2.2 已确认是固有缺陷

2026-09-21，Darwin arm64，使用 `85806657` 的原 `codegen.c`、`feng_exception.c`
及 runtime 头文件重建基线，复用其他未修改目标文件。由原 codegen 直接生成 C，
没有手工替换 landing pad；旧产物使用 `FengFrameMarker`、
`feng_release_unwind_exception` 和 `g_current_unwind`。

| 版本 | 同一源程序的实际结果 |
| --- | --- |
| 嵌套 catch 修复前 | `feng_frame_release_to: marker is not on cleanup chain` |
| 嵌套 catch 修复后 | `feng cleanup: boundary is not on cleanup chain` |

两者均未进入预期 catch，确认 N08 是补充用例时发现的固有缺陷。
基线源码和产物核查时位于 `build/n08-baseline/`；全量测试会清理该目录，正式复现
内容保留在 §2.1 及下述 FCTS 中。

早期仅替换旧 runtime 并手工恢复 landing pad 的探索探针曾得到退出码 137、无正常
输出；该观察不用于判断具体根因，以以上直接发码对照和 LLDB 调用栈为准。
调试日志保存在工程根目录 `nested-exception-escape-debug.log`。

### 2.3 用例保留与本次交付边界

在 [`test_nested_exception_lifetime.ff`](../../fcts/fcts_bin/src/test_nested_exception_lifetime.ff)
完整注释保留以下内容，不删除源码或断言：

- `nestedExceptionEscapingFunctionCleanup`：触发 N08 的专属辅助函数。
- `escaping function-unwind defer releases its abandoned exception`：相应行为用例，
  包含原异常析构、替换异常身份及最终析构次数检查。

上述断言是发现问题时用于验证“新异常替换旧异常”的候选预期，尚未成为已批准规则。
后续需根据审定结果恢复为运行时行为用例，或转为相应的编译期诊断用例。

以下已通过场景继续作为嵌套 catch 修复的活动用例：defer 调用内部捕获错误并返回；
landing pad 清理期间处理另一异常；landing pad 中的清理异常向外传播；catch 的
defer 处理内层异常后继续读取原绑定。它们不触发本节所述整函数二次展开缺陷。

## 3 当前异常分析基础及缺口

| 位置 | 已核实实现 | 后续需要处理 |
| --- | --- | --- |
| `analyzer.c` 的 `CallableExceptionEscapeCacheEntry` | 保存 callable 与 `bool escapes`，通过迭代分析传播变化，供 `@abi` 边界检查使用 | 将布尔结果发展为可表达可能异常类型的摘要 |
| `callable_may_escape_exception` | 无函数体或无缓存时返回 false | 无信息不能直接当作已证明不抛错；跨包摘要和未知情况需要明确定义 |
| `resolve_try_expr` | 有 catch 子句即提升 `exception_capture_depth` | 集合分析需按实际覆盖范围扣除已处理异常，并计入 catch 本身逃逸的异常 |
| callable 值分析 | 可以追踪部分已知函数来源和 Lambda；无法唯一解析目标时返回 false | 动态 callable、spec 方法和泛型约束调用需要契约或保守摘要 |
| symbol / FT | callable 导出参数、返回类型及既有声明属性，未携带异常集合 | 导出、读取、重建及缓存需要保留异常信息 |

代码依据：[semantic analyzer](../../src/semantic/analyzer.c)、
[symbol 数据结构](../../src/symbol/internal.h)、[FT 编写](../../src/symbol/ft_write.c)
及 [FT 格式](../../src/symbol/ft_internal.h)。这些现有机制可作为基础，不能直接当作
完整的无异常逃逸证明。

## 4 已确认的优化方向

### 4.1 记录 callable 可能向外传播的异常

分析对象是未被 callable 内部完整处理的异常。函数内部出现 throw，但该异常已被
内部 catch 处理，不应仅因该 throw 就要求调用方处理同一个异常。

后续摘要应能表达已知的可能异常类型、依赖泛参的异常信息及无法静态确定的部分。
已证明没有逃逸异常与尚无足够信息需要明确区分；具体数据结构由后续方案确定。

分析需要覆盖直接 throw、匿名重抛、调用链、递归、catch 内再次抛出、defer 调用，
以及函数、方法、构造函数、Lambda 等 callable 边界。异常身份和 catch 覆盖范围
复用主规范，避免另建一套类型匹配规则。

### 4.2 跨包 FT 与间接调用

可跨包调用的 callable 需要导出异常摘要；私有辅助 callable 对外部的影响汇总到
导出的调用者，不要求为了该分析额外公开私有实现。

FT 导入方在看不到源码时仍需取得摘要。缺失或无法确定的信息应在分析模型中保留，
不能解释为“没有异常”。具体字段、兼容校验、缓存失效及重编要求留待 FT 方案 Review；
本次嵌套 catch 修复不改 FT。

callable-form spec、object-form spec 方法及泛型约束调用，需要明确声明契约、
实现方法和动态实际目标之间的异常信息关系。仅给每个具体函数增加一个标志不能
覆盖这些调用入口。

### 4.3 用于 N08 处理及未来显式处理提醒

异常集合可用于判断 defer 中调用产生的错误是否可能逃出清理边界。是否据此禁止
逃逸、禁止哪些路径，以及无法静态证明时如何处置，需要后续审定；当前未改变
defer 的既有规则，也未批准“替换原异常”或“终止进程”的新语义。

同一摘要未来可用于向调用方展示可能的异常、提醒显式处理，及判断已有 catch 是否
覆盖可能异常。提示形式、诊断级别、是否强制处理、是否需要声明式语法均是后续
决策项，本记录不将这些可能用途提前变为语言要求。

优先在编译期完成分析及信息传递。若 N08 的最终方案仍需要 runtime／ABI 修改或
增加运行时开销，需先列明具体协议与成本，单独通过人工 Review。

## 5 后续 Todo

- [x] 确认 N08 在修复前的 codegen/runtime 中同样存在。
- [x] 独立记录复现、根因和基线证据，与嵌套 catch 修复分开交付。
- [x] 完成嵌套 catch 修复的全量回归，N08 用例按人工决定完整注释保留。
- [ ] 完成嵌套 catch 修复的人工 Review。
- [ ] 定义异常集合、泛参依赖、未知信息和空集合的分析模型。
- [ ] 明确直接调用、递归、匿名重抛与 catch 覆盖的传播规则。
- [ ] 明确 callable/spec/泛型调用的异常契约及实现兼容规则。
- [ ] 设计 FT 摘要、导入恢复、兼容校验及缓存更新方案。
- [ ] 人工审定 defer 异常逃逸规则和 N08 的具体修复协议。
- [ ] 独立实施 N08 修复与异常集合分析，按审定规则恢复保留用例。
- [ ] 覆盖跨包、递归、动态调用、精确 catch、兜底 catch、重抛及清理组合，执行全量回归。
- [ ] 后续评估基于异常集合的显式处理提醒。
