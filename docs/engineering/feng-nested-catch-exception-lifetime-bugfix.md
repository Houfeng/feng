# Feng 嵌套 catch 异常上下文与载荷生命周期修复方案

> **状态**：已复现并确认根因，待 Review，尚未实施修复。
>
> **核查日期与基线**：2026-09-20，`6ab6c4b4`，Darwin arm64。
>
> **范围**：修复现有嵌套 catch 中外层异常记录丢失、引用载荷提前释放的问题。
> 本问题在核查 [`@throwable` 方案](./feng-throwable-annotation-dev.md) 时发现，
> 但不由该方案或 `E: throw` 引入，独立处理，不依赖异常根特性。

## 1 问题与规范归属

外层 catch 执行期间，如果内部再次抛出异常并被内层 catch 处理，当前实现会提前
释放外层异常记录。内层 catch 结束后，外层异常上下文也没有恢复，产生两个已确认
的结果：外层匿名 `throw;` 失败，以及外层具名引用载荷在 catch 尚未结束时析构。

本修复恢复既有语义，不新增异常语法或匹配规则。权威规则见
[异常规范 §2](../specifications/feng-exception.md#2-throw-语句) 的匿名重抛归属，
以及 [§4.1](../specifications/feng-exception.md#41-异常路径上的资源清理) 的异常载荷
持有与释放规则。实现前在该主规范补明嵌套 catch 的上下文恢复及提前退出边界，
本文只记录问题、修复约束和验收任务。

## 2 已复现的行为

两个探针均经当前编译器编译成功。执行产物位于工程 `build/throwable-readiness-probe/`，
没有修改正式用例。以下源码可分别保存为对应的 `.ff` 文件重新复现。

### 2.1 内层异常处理后，无法匿名重抛外层异常

`nested_rethrow.ff`：

```feng
module throwable_readiness_rethrow;

/** 输出检查标记。 */
@cdecl("libc")
extern func puts(message: string*): int;

/** 抛出外层载荷。 */
func failOuter(): void { throw 17; }

/** 抛出内层载荷。 */
func failInner(): void { throw "inner"; }

/** 内层异常处理结束后，重抛外层匿名 catch 的原异常。 */
func nested(): void {
    try failOuter() catch {
        try failInner() catch error: string {
            puts(&"inner handled");
        }
        puts(&"rethrow outer");
        throw;
    }
}

/** 接收重新传播的外层载荷。 */
func main(args: string[]): void {
    try nested() catch error: int {
        if error == 17 { puts(&"outer recovered"); }
        else { puts(&"wrong outer payload"); }
    }
}
```

预期输出 `inner handled`、`rethrow outer`、`outer recovered`，正常结束。
实际输出前两个标记后报告：

```text
feng: panic: feng_rethrow: no current exception
```

进程退出码为 **134**。这里的 `throw;` 位于外层匿名 catch 中，内层具名 catch
已经结束，符合既有语义规则；不能通过禁止该写法来规避 runtime 问题。

### 2.2 外层引用载荷在 catch 尚未结束时析构

`nested_lifetime.ff`：

```feng
module throwable_readiness_lifetime;

/** 输出检查标记。 */
@cdecl("libc")
extern func puts(message: string*): int;

/** 通过静态计数观察异常对象的析构时间。 */
type TrackedError {
    static var finalized: int = 0;
    let code: int;
    /** 仅统计本探针创建的载荷。 */
    func ~TrackedError() {
        if self.code == 17 { TrackedError.finalized += 1; }
    }
    /** 查询计数，不读取异常对象本身。 */
    static func count(): int { return TrackedError.finalized; }
}

/** 将临时对象转交给异常记录持有。 */
func failOuter(): void { throw TrackedError { code: 17 }; }

/** 产生独立的内层异常。 */
func failInner(): void { throw "inner"; }

/** 只在载荷尚未析构时读取外层绑定，避免探针自身访问已释放内存。 */
func main(args: string[]): void {
    try failOuter() catch outer: TrackedError {
        if TrackedError.count() == 0 { puts(&"outer alive before inner"); }
        try failInner() catch inner: string { puts(&"inner handled"); }
        if TrackedError.count() == 0 {
            if outer.code == 17 { puts(&"outer alive after inner"); }
            else { puts(&"wrong outer payload"); }
        }
        else { puts(&"outer finalized inside catch"); }
    }
    if TrackedError.count() == 1 { puts(&"outer finalized once"); }
}
```

实际输出：

```text
outer alive before inner
inner handled
outer finalized inside catch
outer finalized once
```

预期第三行是 `outer alive after inner`。实际第三行证明外层载荷已提前析构。
本观察程序退出码为 **0**，不代表行为正确；它通过标记报告结果，且没有对已经
析构的对象解引用。若用户代码在内层异常处理后直接访问该绑定，则存在悬空访问风险。

### 2.3 复现命令

从工程根目录执行；源码和产物均放在上述 `build/` 子目录，不在系统临时目录执行：

```sh
make cli
build/bin/feng build/throwable-readiness-probe/nested_rethrow.ff --out=build/throwable-readiness-probe/rethrow --name=nested_rethrow --keep-ir
build/throwable-readiness-probe/rethrow/bin/nested_rethrow
build/bin/feng build/throwable-readiness-probe/nested_lifetime.ff --out=build/throwable-readiness-probe/lifetime --name=nested_lifetime --keep-ir
build/throwable-readiness-probe/lifetime/bin/nested_lifetime
```

核查时 `make cli` 返回无需重编。两个探针均完成编译及运行，结果如上；尚未执行
修复后的验证或全量回归。

## 3 根因与代码依据

| 代码位置 | 当前行为 |
| --- | --- |
| [feng_exception.c](../../src/runtime/feng_exception.c) 的 `g_current_unwind` | 用一个线程局部指针保存当前异常，没有保存外层 catch 上下文的结构 |
| 同文件 `feng_throw` | 建立新异常前，无条件调用 `feng_release_current_unwind_exception(true)`，释放旧记录及其载荷 |
| 同文件 `feng_release_unwind_exception` | 释放当前记录并清空指针，没有恢复外层上下文 |
| 同文件 `feng_rethrow` | 从该指针取得记录；内层 catch 清理后指针为空，报 `no current exception` |
| [codegen.c](../../src/codegen/codegen.c) 的 `cg_emit_try_expr_catch_binding` | 具体引用类型绑定直接借用 `feng_caught_value()` 返回的指针，没有独立持有 |
| 同文件 catch 结束和 return 清理路径 | 调用释放当前异常的入口；修复嵌套上下文时须一起核对退出层级 |

当前时序是：

```text
外层异常 A 被捕获，当前异常为 A
  → 内层 throw B 先释放 A
  → 当前异常改为 B，内层 catch 处理 B
  → 内层 catch 清理 B，当前异常置空
  → 返回外层 catch，但 A 的记录和载荷已经丢失
```

具体引用类型 catch 的借用绑定本身并非必须改为独立持有；只要对应异常记录在其
有效作用域内正确存活，借用可以成立。本问题的根因是作为 owner 的记录被提前释放。
只给具名绑定增加 retain 不能恢复匿名 catch 的原异常，也不能解决上下文丢失。

## 4 修复设计需要满足的条件

需要按 catch 的实际进入和退出管理异常上下文，区分“当前正在传播的异常”与
“尚未退出的 catch 所处理的异常”。不能仅根据再次调用 `feng_throw` 就判断旧异常
已经不再需要，也不能只删除旧记录释放而留下泄漏。

| 场景 | 修复必须保障的结果 |
| --- | --- |
| 外层 catch 内的内层 try 正常完成 | 外层异常及绑定继续有效 |
| 内层异常被内部 catch 处理并正常继续 | 内层记录正确结束，外层异常上下文恢复 |
| 内层异常继续逃逸出外层 catch | 按实际退出的层级清理旧上下文，新异常继续传播 |
| 外层匿名 catch 执行 `throw;` | 取回该 catch 的原异常，保留原记录、具体描述符及载荷身份 |
| 具名 catch 执行 `throw error;` | 按新抛出语义先取得所需所有权，再结束已退出的旧上下文 |
| catch 内 return 或其他既有合法控制流退出 | 清理实际离开的上下文；仍然活跃的外层上下文保持有效 |
| catch 调用其他函数，函数内部处理异常后返回 | 被调用函数的异常处理不能破坏调用方仍在执行的 catch |
| 载荷被复制或保存到 catch 外 | 既有普通值持有规则继续有效，异常记录清理不破坏外部所有者 |

修复应复用统一清理机制，覆盖普通落尾、return、合法循环控制退出、展开及重抛。
匿名重抛的词法归属、具体类型精确匹配、ValueBox 表示及普通值复制规则保持不变。

**尚待设计与人工 Review 的内容**：异常上下文的实际存储、与 cleanup/frame 的
关联、进入／退出协议，以及所需的 runtime 私有结构或接口调整。需要列出相对当前
实现增加的字段、存取、分配与清理操作后再决定，不能预先声称无 ABI 或性能影响。
本文没有选定新的 runtime API，也不授权实现时自行扩展这些边界。

与 `@throwable` 的关系只在于后续异常传播也依赖正确的上下文生命周期；本修复
不引入根 spec、witness 参数、异常句柄或新的载荷种类。

## 5 验证范围

| 维度 | 必须覆盖 |
| --- | --- |
| 基础复现 | 第 2 节两个探针改为结果断言，分别验证原异常恢复及析构时间 |
| 嵌套组合 | 匿名／具名 catch 的组合，两层与多层，内层正常返回、处理异常、继续抛出 |
| callable 边界 | 同函数内嵌套、调用其他函数／方法／Lambda 后返回，调用方仍持有外层异常 |
| 重抛身份 | 匿名重抛保留原记录及具体身份；具名再次抛出保留载荷值；不同具体类型不混淆 |
| 生命周期 | 临时引用对象、动态 string、含托管字段的值载荷及 ValueBox；无提前释放、泄漏或重复析构 |
| 退出与清理 | catch 正常落尾、return、既有合法 break／continue、defer、未匹配异常继续展开 |
| 外部持有 | 绑定被保存或捕获后离开 catch，外部引用继续有效，最后释放时正确析构 |
| 发码与 runtime | 上下文进入／退出与 cleanup 层级配套；借用绑定的 owner 存活期正确 |
| 普通路径 | 单层 catch、无嵌套 throw、原有重抛与异常清理行为不回退；核对批准的成本范围 |

编译器测试放在 `test/`，验证生成代码和 runtime 协议；语言行为放在 `fcts/`。
优先增加独立用例，未经人工批准不修改已有用例。实现后的定向验证通过后，在
沙箱外执行 `make test` 全量回归；新增问题先记录、再分析、再修复。

## 6 实施 Todo

- [x] 复现内层异常处理后外层匿名重抛失败。
- [x] 通过析构计数复现外层引用载荷提前释放，避免探针访问已释放内存。
- [x] 核对 runtime 的单一当前异常指针、旧记录释放及 catch 借用绑定发码。
- [ ] 在异常主规范补明嵌套上下文恢复与退出边界，保留既有异常语义。
- [ ] 完成上下文存储、cleanup 关联及进入／退出协议设计，提交 runtime／ABI／成本 Review。
- [ ] 按批准方案修复 runtime 与相应发码，统一处理正常退出、提前退出及传播路径。
- [ ] 增加编译器与 FCTS 用例，覆盖第 5 节；需要调整已有用例时先取得批准。
- [ ] 完成定向验证与沙箱外 `make test`，记录环境、命令、结果及问题处理过程。
- [ ] 回填交付结果，给出英文 commit message，由开发者自行提交。

## 7 当前交付状态

本次只建立独立 bugfix 文档，复现证据来自前序分析。编译器、runtime、正式规范
和正式测试均未修改；没有宣称修复完成，也没有把探针的执行结果当作回归通过记录。
