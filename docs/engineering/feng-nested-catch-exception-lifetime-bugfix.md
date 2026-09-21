# Feng 嵌套 catch 异常上下文与载荷生命周期修复方案

> **状态**：2026-09-21 已完成修复及沙箱外 `make test` 全量回归，待人工 Review，未提交。
> 实现采用人工批准的 §4.1 协议、成本及配对入口名称。
> N08 按人工决定独立跟踪，触发用例完整注释保留，本次未修复 N08。
>
> **核查日期与基线**：2026-09-20，`6ab6c4b4`，Darwin arm64。
>
> **范围**：修复现有嵌套 catch 中外层异常记录丢失、引用载荷提前释放的问题。
> 本问题在核查 [`@throwable` 方案](./feng-throwable-annotation-dev.md) 时发现，
> 但不由该方案或 `E: throw` 引入，独立处理，不依赖异常根特性。
> 展开期间 defer 再次抛出并逃逸的固有问题 N08，转入
> [callable 异常集合分析与 N08 后续优化方案](./feng-callable-exception-effects-dev.md)。

## 1 问题与规范归属

修复前，外层 catch 执行期间，如果内部再次抛出异常并被内层 catch 处理，实现会提前
释放外层异常记录。内层 catch 结束后，外层异常上下文也没有恢复，产生两个已确认
的结果：外层匿名 `throw;` 失败，以及外层具名引用载荷在 catch 尚未结束时析构。

本修复恢复既有语义，不新增异常语法或匹配规则。权威规则见
[异常规范 §2](../specifications/feng-exception.md#2-throw-语句) 的匿名重抛归属，
以及 [§4.1](../specifications/feng-exception.md#41-异常路径上的资源清理) 的异常载荷
持有与释放规则。实现前在该主规范补明嵌套 catch 的上下文恢复及提前退出边界，
本文只记录问题、修复约束和验收任务。

## 2 已复现的行为

两个探针均经修复前基线编译器编译成功。核查时的执行产物位于工程 `build/throwable-readiness-probe/`，
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

核查时 `make cli` 返回无需重编。两个探针均完成编译及运行，修复前结果如上；
修复后的验证记录见 §7。

## 3 修复前的根因与代码依据

| 代码位置 | 修复前行为 |
| --- | --- |
| [feng_exception.c](../../src/runtime/feng_exception.c) 的 `g_current_unwind` | 用一个线程局部指针保存当前异常，没有保存外层 catch 上下文的结构 |
| 同文件 `feng_throw` | 建立新异常前，无条件调用 `feng_release_current_unwind_exception(true)`，释放旧记录及其载荷 |
| 同文件 `feng_release_unwind_exception` | 释放当前记录并清空指针，没有恢复外层上下文 |
| 同文件 `feng_rethrow` | 从该指针取得记录；内层 catch 清理后指针为空，报 `no current exception` |
| [codegen.c](../../src/codegen/codegen.c) 的 `cg_emit_try_expr_catch_binding` | 具体引用类型绑定直接借用 `feng_caught_value()` 返回的指针，没有独立持有 |
| 同文件 catch 结束和 return 清理路径 | 调用释放当前异常的入口；修复嵌套上下文时须一起核对退出层级 |

修复前的时序是：

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

与 `@throwable` 的关系只在于后续异常传播也依赖正确的上下文生命周期；本修复
不引入根 spec、witness 参数、异常句柄或新的载荷种类。

### 4.1 已批准的具体实现方案

复用现有 cleanup chain 管理 catch 的持有，将异常处理上下文作为作用域资源：

1. 增加栈上 `FengCatchContext`，包含原有 `FengFrameMarker`、原异常记录指针、
   前一层活跃 catch 指针。每个 try/catch 用它替换原有 try marker 存储；进入 catch
   时原位将 marker 节点改为 `FENG_NODE_DEFER` 并设置内部清理函数，不另加节点或
   节点类别，不改普通 local、defer 或 function frame marker 的布局。
2. 原有单一 TLS 指针拆分职责：原指针只交接 personality 选中的待进入异常，另加一个
   TLS 指针保存活跃 catch 栈顶。`feng_throw` 不再释放仍处于 catch 中的旧异常。
3. 增加一个私有入口 `feng_exception_catch_begin(FengCatchContext *)`，
   替代 landing pad 中的 `feng_frame_release_to` 调用。先取走待进入异常，将原 try
   marker 转为其持有节点，再清理该节点之上的 try 资源，最后激活新 catch。
   这样清理期间调用的函数即使内部再次处理异常，也不会覆盖待进入的异常；若清理
   本身向外传播异常，已登记的持有节点仍可随展开释放。
4. `feng_caught_value` / `feng_caught_clause` 从活跃 catch 读取。
   将现有 `feng_release_unwind_exception(void)` 替换为 `feng_exception_catch_end(void)`，与
   `feng_exception_catch_begin` 成对表达 catch 上下文的进入和退出，不保留旧名的重复入口。
   `feng_exception_catch_end` 按 LIFO 移除本层节点、恢复外层上下文、释放本层仍持有的记录及载荷。
   正常落尾及 return、break/continue 的退出路径调用该入口；异常展开则通过同一节点
   的内部清理函数完成结束动作，不重复移除节点或释放异常。
5. `feng_rethrow` 从最近活跃 catch 取出原记录，并清空该 catch 的记录持有，再使用
   既有 `_Unwind_Resume_or_Rethrow` 传播。旧上下文节点留在 cleanup chain，随实际退出
   清理；它不再释放已经转交的记录。保持原描述符、载荷和 unwind 记录身份。
6. Codegen 将 try marker 和 catch 结束动作记录在各自的编译期 scope 上，由
   `cg_release_scope` 按局部值之后、父 scope 之前的顺序发码。正常落尾、return 和
   break/continue 共用该规则，移除按总 catch 数量释放一次的处理及混用 try/catch
   深度的计算；异常路径继续由 runtime 遍历同一 cleanup chain。

成本与 ABI 边界：

| 项目 | 相对修复前实现的变化 |
| --- | --- |
| 堆分配 | catch 上下文不增加堆分配；每次新 throw 仍只有既有 unwind record 与原载荷所需分配。绑定被闭包捕获时沿用普通 capture cell 的分配与持有规则，见 N03 |
| 异常记录和普通 cleanup/frame 布局 | 保持 `FengUnwindException`、`FengCleanupNode`、`FengFrameMarker` 原布局 |
| 栈空间 | 每个 try/catch 用 80 字节的 `FengCatchContext` 替换 64 字节的 try marker，当前 64 位布局净增 16 字节；实际函数栈增量受后端栈槽复用影响 |
| TLS | 每线程增加一个活跃 catch 指针，当前 64 位平台为 8 字节 |
| 正常 try 路径 | 仍只初始化原有 marker 字段，不初始化两个 catch 字段；仍有上述潜在栈空间成本 |
| catch 路径 | 增加固定数量的指针存取，将原节点转换为 catch 持有节点并在退出时移除；沿用既有 cleanup 遍历，无单独全链扫描 |
| ARC | 未被捕获的借用引用绑定不增加 retain/release；异常记录仍持有原有的一份引用。捕获取得普通 capture cell 所需的独立持有 |
| 私有入口 | 新增 `feng_exception_catch_begin`；将 `feng_release_unwind_exception` 替换为配对的 `feng_exception_catch_end`，结束入口仍无参数、无返回值；`feng_throw`、`feng_rethrow` 和 caught 读取入口签名不变 |

2026-09-21 人工批准上述协议和成本，并采用 `feng_exception_catch_begin/end` 命名。
实现与测试若发现该协议还不足以满足本修复范围，先记录事实，再提交具体差异决策。
2026-09-20 已用当前 runtime 头文件及宿主 Clang 测量布局：原 marker 为 64 字节，
拟议 context 为 80 字节，指针为 8 字节；测量程序位于工程 `build/`，不属于修复验证。

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
配对入口替换所需的既有 codegen 字符串断言同步迁移到新名称，保留原有断言目的与强度；
新增测试的 runner 注册不改变已有用例。
2026-09-21 人工批准将 N08 的失败用例及专属辅助函数完整注释保留，作为独立后续任务。
其余测试继续执行；本修复的全量通过不表示 N08 已修复。

## 6 实施 Todo

- [x] 复现内层异常处理后外层匿名重抛失败。
- [x] 通过析构计数复现外层引用载荷提前释放，避免探针访问已释放内存。
- [x] 核对 runtime 的单一当前异常指针、旧记录释放及 catch 借用绑定发码。
- [x] 在异常主规范补明嵌套上下文恢复与退出边界，保留既有异常语义。
- [x] 完成上下文存储、cleanup 关联及进入／退出协议设计，通过 runtime／ABI／成本 Review。
- [x] 按批准方案修复 runtime 与相应发码，统一处理本修复范围的正常退出、提前退出及传播路径。
- [x] 增加编译器与 FCTS 用例，覆盖第 5 节；N08 按人工决定独立跟踪并注释保留。
- [x] 完成定向验证与沙箱外 `make test`，记录环境、命令、结果及问题处理过程。
- [x] 回填交付结果，给出英文 commit message，由开发者自行提交。

## 7 当前交付状态

§4.1 的 runtime 与 scope 发码已经实现，另修复 N03 的 catch 绑定捕获缺口。
新增测试位于以下独立文件；既有用例只迁移两处配对入口名称断言并注册新 suite：

- [`test/runtime/test_nested_exception.c`](../../test/runtime/test_nested_exception.c)：
  原始 unwind record、载荷指针、描述符及引用计数；未捕获 throw/rethrow 的终止清理；
  4 线程分别执行 64 次嵌套处理，验证 TLS 隔离。
- [`test/codegen/test_nested_exception.c`](../../test/codegen/test_nested_exception.c)：
  try/catch 清理与 return、break/continue 的 scope 顺序；未被闭包捕获时的绑定借用成本；
  引用、值、tuple、标量、enum、string 的捕获发码及宿主 C 编译。
- [`fcts/fcts_bin/src/test_nested_exception_lifetime.ff`](../../fcts/fcts_bin/src/test_nested_exception_lifetime.ff)：
  29 项活动行为测试，覆盖 §5 的嵌套、callable、退出、捕获、载荷及清理维度；另有
  1 项 N08 用例及专属辅助函数完整注释保留，原因与后续任务见独立优化文档。

2026-09-21，Darwin arm64，最终验证结果：

| 验证 | 结果 |
| --- | --- |
| 第 2 节两个独立探针 | 均恢复预期输出，退出码 0 |
| `build/bin/test_runtime` | 通过，包含新增原始记录、所有权、终止清理和 TLS 验证 |
| `build/bin/test_codegen` | 全部通过，包含新增 scope/capture 验证及既有测试 |
| `make fcts-tests` | 退出码 0，1463/1463 通过，失败 0、跳过 0；包含新增 29 项活动行为测试 |
| 沙箱外 `make test` | 退出码 0，UBSan 与普通构建两个阶段全部完成 |
| 全量回归的编译器与工具测试 | 两阶段的 `test_archive`、`test_lexer`、`test_parser`、`test_semantic`、`test_runtime`、`test_codegen`、`test_debug`、`test_cli`、`test_cli_paths`、`test_symbol` 全部通过 |
| 全量回归的语言与标准库测试 | 两阶段各自 smoke 91/91、标准库 607/607、FCTS 1463/1463 全部通过，FCTS 与标准库失败及跳过均为 0 |
| 其余全量目标 | CLI 脚本、项目与初始化、性能约束、增量构建、发布／安装、macOS finalization、bundled packages 和 toolchain fetch 检查均通过 |

最终全量命令为 `make test > nested-exception-final-regression.log 2>&1`，在沙箱外执行。
完整日志保存在工程根目录 `nested-exception-final-regression.log`；独立 FCTS 日志为
`nested-exception-final-fcts.log`。未发现 UBSan 报告，最终保留普通构建产物。

首次回归曾由 N08 中断，记录后完成修复前基线对照，确认属于固有缺陷，再按人工决定
将其用例完整注释保留并重新执行上述全量回归。早期日志保留在
`nested-exception-full-regression.log`、`nested-exception-normal-regression.log` 和
`nested-exception-normal-unit.log`，作为问题处理过程的记录。

N08 的注释用例未注册、未计入上述通过数或跳过数；全量通过不表示该问题已修复。
嵌套 catch 修复已完成，代码未提交，等待人工 Review；N08 留待独立修复和优化。

## 8 实施问题记录

| 编号 | 事实与分析 | 处理 |
| --- | --- | --- |
| N01 | `cg_emit_control_cleanup_to_try_depth` 只在存在 catch 时释放一次异常；循环保存的 `try_depth` 同时计入 try 与 catch，却与仅计入 try 的 frame 数相减。它不能表示实际离开的上下文及局部资源之间的清理顺序。 | 纳入 §4.1 的 scope 清理设计；增加嵌套 return、退出 catch 的 break/continue 及留在 catch 内的循环控制验证。 |
| N02 | landing pad 在读取 caught 信息前调用 `feng_frame_release_to`；该清理可执行 defer，defer 调用的函数可内部处理另一异常。仅将 TLS 改成 catch 栈仍不足以保护尚未进入 catch 的原异常。 | §4.1 将待进入异常先登记为 cleanup 资源；增补 landing pad 清理期间嵌套处理异常的用例。 |
| N03 | 新增外部持有用例在 `return () { return error.code; }` 处编译失败，诊断 `CE0102: lambda capture 'error' was not lowered to a capture cell`。语义接受 catch 绑定捕获，但发码未建立其 capture cell。 | 已将 match 借用绑定的 capture 提升函数泛化，catch 复用同一机制；仅发生捕获时按普通闭包规则取得独立持有，未捕获路径不增加 ARC 或分配。 |
| N04 | 新增同帧重抛用例把 `void` try 结果赋给未使用的局部，宿主 C 报 `variable has incomplete type 'void'`。 | 用例目标不需要结果值，改为直接的 try 语句，保留嵌套 catch、重抛和析构断言。 |
| N05 | 新矩阵的 value 和闭合泛型载荷场景，实测析构总数均比初始断言多 1，但带标记的载荷字段及其最终析构顺序正确。 | 核对类型规范 §5：字段先创建默认零值，再由对象字面量覆盖，因此这里还会释放一个默认子对象。修正新增断言以精确计入该次合法析构，仍核对原载荷在 catch 内有效且最终仅析构一次。 |
| N06 | 新增测试准备中，`assertEquals<int>` 需要显式导入 `std.numeric` 的 Display 满足关系；codegen 的首次 `break` 搜索误选到循环条件自身的退出分支。 | 已核对 `std/std/src/numeric/i32.ff` 并补充测试 import；将控制流断言限定到 catch 激活之后，并定位重抛之前的实际循环跳转。 |
| N07 | 既有 `test_generic_try_body_reified_storage_codegen` 检查 try 结束 label 之后移除 marker，首次作用域改造把正常 pop 放到了 label 之前。 | 将 try marker 归属建模为主表达式局部 scope 的父边界 scope；正常路径在原 label 后结束边界，提前退出仍按同一 scope 链清理，保留既有断言。 |
| N08 | 补例发现整函数展开期间 defer 再次抛出且逃逸，会错误清理外层边界；修复前 codegen/runtime 的独立对照也复现，确认为固有缺陷。 | 2026-09-21 人工决定独立修复和优化，原用例及专属辅助函数完整注释保留。复现、根因、基线证据和后续方向收敛到[独立优化文档](./feng-callable-exception-effects-dev.md)。 |
