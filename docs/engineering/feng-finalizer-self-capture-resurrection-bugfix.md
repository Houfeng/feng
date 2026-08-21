# Feng 终结器 `self` 捕获与对象复活修复方案

> **状态**：已完成；定向验证、FCTS 与沙箱外 `make test` 全部通过。
>
> **文档定位**：本文是独立 bugfix 工程方案，不是正式语言规范。Review 通过后，必须
> 先修订相关正式规范，再修改 Runtime、Codegen 和测试。Review 通过前不得开始实现。
>
> **来源**：本问题最初记录为
> [`feng-friend-type-implementation-context-bugfix.md`](./feng-friend-type-implementation-context-bugfix.md)
> 的 P02。它与 `@friend` 授权无关，现已拆分为独立专项。

## 1. 核心结论

终结器是具有隐式 `self` 的实例实现上下文。终结器可以直接使用 `self`，也可以让
Lambda 捕获 `self`。对引用语义对象，捕获或赋值跨越普通值边界时形成强引用：

- 终结器返回后仍保留在外部的强引用使对象复活；
- 仅在终结器执行期间存在、返回前已经释放的临时强引用不构成复活；
- 临时强引用释放时不得在原终结器返回前重入同一对象的终结器；
- 被复活对象以后再次不可达时，终结器按既有规则再次执行。

现有 Runtime 已支持“终结器把 `self` 直接保存到外部”这一持久复活路径，但尚未完整
支持捕获产生的临时 retain/release。P02 因此不能只补一段非泛型 Codegen lowering；
否则会把当前 `CE0103` 编译错误变成终结器递归重入和运行时崩溃。

## 2. 规范现状

### 2.1 已有生命周期规则

对象复活的权威生命周期规则位于
[`feng-lifetime.md` §13.2](../specifications/feng-lifetime.md#132-终结器执行流程)：

- ARC 在用户终结器返回后重新检查引用计数；
- 引用计数大于零时对象复活，本轮不得释放字段或对象内存；
- 循环回收器在 Phase 1 后通过 Phase 1.5 判断组外新增引用并传播存活；
- 规范明确把“在终结器内将 `self` 赋给全局变量或长期存活对象”列为允许但不推荐的
  复活用法，而不是非法语法。

因此，禁止终结器捕获或向外保存 `self` 不符合既有生命周期语义。

### 2.2 正式规范存在遗漏

以下文字只列出“成员方法或构造函数”，遗漏终结器：

- [`feng-function.md` §4](../specifications/feng-function.md) 中 `self` 的引入与 Lambda
  捕获来源；
- [`feng-function.md` §6](../specifications/feng-function.md) 中编译器对合法外层 `self`
  的检查要求；
- [`feng-type.md` §4](../specifications/feng-type.md) 中隐式 `self` 及其跨值边界语义。

这些遗漏与 `feng-lifetime.md` 的复活规则、终结器直接访问 `self` 的既有能力，以及
当前 Semantic 实际行为不一致。修复时应在各自权威位置补齐“终结器”，不得在本文或
其他关联文档重复定义正式语义。

## 3. 问题复现

### 3.1 非泛型终结器无法降低 `self` 捕获

以下合法代码通过 Semantic，但在 Codegen 报告 `CE0103`：

```feng
spec Producer(): int;

type Resource {
  let value: int;

  func Resource(value: int) {
    self.value = value;
  }

  func ~Resource() {
    let producer: Producer = () -> self.value;
    producer();
  }
}
```

当前诊断为：

```text
CE0103: codegen: lambda self capture was not lowered to a capture cell
```

同一专用终结器 emitter 也没有准备普通局部绑定的 capture cell。因此根因不是
`self` 成员访问本身，而是非泛型终结器遗漏了普通 callable 已有的整体捕获准备阶段；
捕获普通局部绑定时可能以 `CE0102` 暴露同一缺口。

### 3.2 泛型终结器已经发码，但会递归崩溃

泛型终结器走共享 generic member emitter，已经执行捕获需求收集并为 `self` 建立
capture cell。等价泛型用例能够完成编译，但不逃逸的局部 Lambda 在对象释放时会使
进程以 `139` 退出。

当前 ARC 时序为：

```text
最后一个外部引用释放，self.rc: 1 -> 0
    ↓
进入用户终结器
    ↓
建立 self capture cell，retain(self)，self.rc: 0 -> 1
    ↓
局部 Lambda 与 capture cell 清理，release(self)，self.rc: 1 -> 0
    ↓
原终结器尚未返回，再次进入同一终结器
```

capture cell 在 callable 入口按捕获需求统一建立；即使捕获 Lambda 位于本次未执行的
分支，`self` 的临时强引用仍会在入口建立并在退出清理。这是普通 callable 当前已有的
统一捕获模型，不能通过 finalizer 分支特判或逃逸猜测规避。

### 3.3 持久外部复活路径已经可用

以下行为已确认能够成功编译和运行：

```feng
var resurrected: Resource[] = [];

type Resource {
  func ~Resource() {
    resurrected = [self];
  }
}
```

生成代码在数组元素写入时 retain `self`；用户终结器返回后，ARC 观察到引用计数大于
零并保留对象。现有 Runtime 单元测试同时覆盖：

- ARC 路径把 `self` 发布到全局、以后再次释放；
- 连续多次复活及最终释放；
- 循环回收 Phase 1.5 中复活 `self` 并沿强引用边传播存活。

缺口因此不是“Runtime 完全不支持复活”，而是“终结器执行期间没有隔离临时强引用的
归零与终结器重入”。

## 4. 根因

### 4.1 Semantic 已接受正确语义

类型成员解析对所有非静态 callable 传入 `allow_self = true`。终结器不是静态成员，
其 Lambda 引用 `self` 时会正常记录 `captures_self`。因此 P02 不应通过新增 Semantic
限制解决，也不需要新增错误码。

### 4.2 非泛型与泛型 Codegen 路径发生漂移

普通方法、构造函数及 generic member 共享体会：

1. 收集整个 callable body 的捕获需求；
2. 为将被捕获的参数、局部绑定与 `self` 建立 capture cell；
3. 再生成函数体并按作用域清理 capture cell。

非泛型 `cg_emit_user_finalizer` 是独立 emitter，只向作用域加入普通 `self` binding，
没有执行前两步。泛型终结器则复用了 generic member shared body，因而没有 `CE0103`，
同时暴露了 Runtime 重入问题。

### 4.3 ARC 在引用计数为零时直接执行用户代码

`feng_release` 把最后一个引用从 1 减到 0 后直接调用用户终结器。终结器执行期间没有
Runtime 私有的执行持有，也没有同一对象的 finalizing 状态。任何平衡的临时
retain/release 都可能在用户回调结束前再次把计数降到 0，从而递归进入销毁流程。

### 4.4 循环回收路径必须保持 collector 所有权

循环回收 Phase 1 运行终结器时，`self` 捕获同样会产生平衡的 retain/release。对
potentially-cyclic 对象，普通 `feng_release` 还可能把对象重新加入候选缓冲区并在阈值
达到时触发嵌套收集。

本次修复必须保证：

- 当前收集轮次的 white 对象在 Phase 1/1.5/2 完成前仍由该轮 collector 管理；
- 捕获清理不得递归启动处理同一 white 集合的收集；
- Phase 1 中新加入候选缓冲区、但最终属于 free_set 的对象必须在释放前移除候选引用；
- Runtime 私有执行持有必须在 Phase 1.5 统计真实引用前移除，不能被误判为外部复活。

这里处理的是 `self` 捕获直接产生的临时引用。若实施中确认现有 collector 对“终结器
任意修改并释放其他 white 对象字段”的处理也不满足 §13.2，应先记入本文的实施问题，
停止扩大修改范围并由人工决定是否拆分为另一个专项。

## 5. 修复方案

### 5.1 先修订正式规范

正式规范应表达以下一致事实：

1. 终结器与实例方法、构造函数一样具有隐式 `self`；
2. Lambda 可捕获合法终结器外层作用域中的 `self`；
3. 引用语义 `self` 跨赋值、参数或捕获边界时复制强引用；
4. 只有终结器返回后仍存在的程序强引用才构成复活；
5. Runtime 为安全执行终结器设置的内部持有不属于程序可观察强引用，也不参与复活
   判定；
6. 临时强引用返回前归零不得导致终结器重入。

生命周期流程的唯一权威定义继续放在 `feng-lifetime.md`；函数捕获规则只在
`feng-function.md` 定义；类型 `self` 值边界只在 `feng-type.md` 定义。

### 5.2 ARC 增加终结器执行持有

建议在仅有用户终结器的销毁慢路径引入 Runtime 私有的 **finalizer execution hold**：

```text
程序强引用降为 0
    ↓
Runtime 建立一个不可观察的执行持有
    ↓
调用用户终结器
    ↓
Runtime 直接移除执行持有，不通过普通 feng_release
    ↓
程序强引用 > 0：复活
程序强引用 = 0：释放子引用并回收对象
```

约束如下：

- 执行持有只存在于用户终结器调用期间；
- 建立和移除必须使用与现有原子引用计数一致的内存序；
- 移除持有不能再次触发用户终结器；
- 无终结器对象及普通 retain/release 快路径不增加分支、字段或原子操作；
- 终结器路径允许增加一对必要的原子计数操作；
- 最终不复活并释放 potentially-cyclic 对象前，必须再次清理终结器期间可能产生的候选
  记录；
- 复活后的 potentially-cyclic 对象必须保持可被后续循环检测器发现的候选状态。

该方案不要求修改 `FengManagedHeader` 布局、公开 Runtime ABI 或类型描述符。若实现中
发现必须新增 header 字段、改变所有对象的 retain/release 快路径，或增加其他持续运行时
开销，必须先记录问题并等待人工决策。

### 5.3 循环回收禁止嵌套处理当前 white 集合

循环回收器应为一次收集建立明确的内部 collecting scope：

- Phase 1 中产生的新候选可以延后到当前轮次结束后处理；
- 达到阈值时不得在当前 Phase 1 调用栈内递归启动另一轮收集；
- 当前轮 free_set 中若存在新候选记录，Phase 2 释放前必须移除；
- survivor 候选可在当前轮完成后按普通候选规则处理；
- execution hold 在每次用户终结器返回后立即移除，Phase 1.5 只观察真实程序引用和
  组内引用。

collecting scope 只影响循环检测器慢路径，不得给 acyclic ARC 快路径增加常驻检查。
具体状态若不能复用 Runtime 内部收集上下文，应在实施问题中记录候选结构及性能影响，
由人工确认后再继续。

### 5.4 统一 callable 捕获准备

非泛型终结器必须复用普通 callable 的捕获需求与绑定 lowering：

- 统一收集局部绑定和 `self` 捕获需求；
- 统一调用现有 capture-cell 创建与作用域清理能力；
- 不在 Lambda 表达式、`self`、终结器名称或是否逃逸处增加特判；
- 泛型与非泛型终结器必须得到相同的捕获与生命周期行为；
- 不改变不含捕获 Lambda 的终结器生成结果与运行时开销。

优先抽取可由普通实例 method、constructor 和 finalizer 共用的 callable receiver/capture
准备 helper，避免继续复制 emitter 逻辑。若现有 emitter 状态差异使共用 helper 不成立，
应先记录差异并由人工确认，不得退回新增 finalizer 专用捕获规则。

### 5.5 明确拒绝的方案

- 禁止终结器使用或捕获 `self`：违反现有生命周期规范；
- 仅给非泛型终结器补 capture cell：会保留 Runtime 重入崩溃；
- finalizer 专用“不 retain self”捕获：逃逸闭包会持有悬空对象；
- 依据当前分支、逃逸猜测或变量名称决定是否 retain：引入不完整逃逸分析和特判；
- 让内部 execution hold 参与复活判定：所有终结器都会被错误判定为复活；
- 通过禁止泛型终结器捕获掩盖现有崩溃：缩减已被 Semantic 接受的语言能力。

## 6. 测试与验证范围

### 6.1 Runtime 单元测试

- ARC 终结器内临时 `retain(self)` 后 `release(self)`：不重入，只执行一次终结器；
- 临时引用清理后无外部引用：对象正常释放；
- 外部引用保留到终结器返回后：对象复活；
- 释放复活引用且不再复活：终结器再次执行并最终释放；
- potentially-cyclic 对象的临时 self 引用不会递归收集或留下悬空候选；
- 循环组内外部复活继续由 Phase 1.5 正确识别，既有传播行为不回归。

### 6.2 Semantic 与 Codegen 测试

- 非泛型终结器 Lambda 捕获 `self`，不再产生 `CE0103`；
- 非泛型终结器 Lambda 捕获普通局部绑定，不再产生 `CE0102`；
- 嵌套 Lambda 捕获 `self`；
- 泛型终结器使用 owner 泛参并捕获 `self`；
- 生成代码具有 capture cell、平衡清理及正确的 finalizer thunk/shared body；
- 非终结器 callable 捕获生成结果保持不变。

### 6.3 FCTS 行为测试

- 非泛型、不逃逸 Lambda 捕获 `self`：调用结果正确且终结器不重入；
- 泛型、不逃逸 Lambda 捕获 `self`：行为与非泛型一致；
- 捕获 `self` 的闭包保存到外部：闭包可在终结器返回后调用，对象已复活；
- 清除外部闭包并关闭再次复活条件：对象第二次终结并最终释放；
- 直接把 `self` 保存到外部的既有复活行为保持正确；
- 用可观察计数锁定“每次不可达周期只执行一次终结器”。

### 6.4 回归要求

- 定向构建并运行 Runtime、Semantic、Codegen 测试；
- 执行 FCTS 全量行为测试；
- 最后在沙箱外执行 `make test` 全量回归；
- `git diff --check` 必须通过。

## 7. 性能与兼容性约束

- 无用户终结器对象的 ARC 释放路径必须保持不变；
- 不含捕获 Lambda 的终结器 Codegen 不新增 capture cell 或闭包分配；
- execution hold 的原子操作只发生在用户终结器实际执行时；
- 循环回收的 collecting scope 只影响 collector 慢路径；
- 不修改 `FengManagedHeader` 布局、`FengTypeDescriptor` 布局、公开 C ABI、`.ft` 格式或
  Feng 对象布局；
- 不改变闭包强捕获、capture cell 共享或对象复活的既有语义。

任何偏离以上性能或 ABI 边界的必要修改，都必须暂停并由人工决策。

## 8. 实施清单

### 8.1 Review 与正式规范

- [x] 人工 Review 并批准本文方案
- [x] 修订 `docs/specifications/feng-lifetime.md`
- [x] 修订 `docs/specifications/feng-function.md`
- [x] 修订 `docs/specifications/feng-type.md`
- [x] 核对错误码规范无需新增或改义

### 8.2 Runtime

- [x] 为 ARC 用户终结器路径实现 execution hold
- [x] 保证 execution hold 不参与复活判定
- [x] 处理终结器期间产生的 potentially-cyclic 候选状态
- [x] 防止 Phase 1 内嵌套收集当前 white 集合
- [x] 在 Phase 1.5 前移除所有 Runtime 私有执行持有
- [x] 补齐 Runtime 定向用例

### 8.3 Codegen

- [x] 抽取或复用统一 callable capture 准备能力
- [x] 为非泛型 finalizer 建立普通局部 capture cell
- [x] 为非泛型 finalizer 建立 `self` capture cell
- [x] 统一恢复 Codegen 捕获上下文和错误清理路径
- [x] 补齐非泛型、泛型及嵌套捕获 Codegen 用例

### 8.4 行为与回归

- [x] 补齐 FCTS 非逃逸捕获用例
- [x] 补齐 FCTS 逃逸闭包复活与最终释放用例
- [x] 运行 Runtime、Semantic、Codegen 定向测试
- [x] 运行 FCTS 全量测试
- [x] 在沙箱外运行 `make test`
- [x] 运行 `git diff --check`
- [x] 补齐本文实施结果与问题记录

## 9. 实施过程问题记录

实施过程中发现任何偏离本文方案、正式规范或预期测试结果的问题时，必须先在本节记录
事实、影响和复现，再分析并处理；不得先加入特判或扩大范围。

### P01：终结器内带值 `return` 的块体 Lambda 被误判为终结器返回

- 状态：已解决（人工决定纳入本专项）
- 发现阶段：新增“嵌套 Lambda 捕获 `self`”Codegen 组合用例并运行定向测试时
- 复现形状：终结器内声明目标为 callable-form `spec` 的块体 Lambda，Lambda 自身使用
  `return value;`
- 实际结果：Semantic 报告 `AE0501: finalizer body must use 'return;' without a value`，
  Codegen 尚未开始
- 期望结果：该 `return` 属于 Lambda 自身，应按 Lambda 的返回类型检查；外层终结器的
  “只能 `return;`”规则不适用
- 根因一：`current_callable_member` 同时承担词法成员实现授权与当前 callable body 身份；
  Lambda 必须继承前者，却不能继承后者，单一字段无法正确表达这两个上下文
- 根因二：异常逃逸派生分析会再次解析 Lambda body，但原实现只重置异常标记，仍沿用
  外层 callable signature、构造函数/终结器成员身份以及 loop/defer 等 body 状态；即使主解析
  边界修正，二次解析仍会重复触发 `AE0501`
- 与本专项关系：这是既有 Semantic callable-context Bug，不是本次 Runtime execution hold
  或 finalizer capture lowering 引入；但它会阻断本文要求的嵌套块体 Lambda 覆盖
- 人工决策：本次一并修复。正式规范先明确特殊成员与嵌套 Lambda 的 `return` 归属；
  Semantic 将“词法成员实现上下文”与“当前 callable body 所属成员”分离，保证 Lambda
  进入自身 callable body 后不继承构造函数/终结器返回限制，同时继续继承外层类型实现
  上下文、`self` 捕获能力和既有 friend/spec-seal 授权
- 处理结果：新增 `current_callable_body_member`，保留 `current_callable_member` 作为词法授权
  身份；主解析与异常逃逸分析共用无分配的 Lambda callable-context 进入/恢复 helper。
  异常逃逸作为派生分析只处理主语义解析尚无错误的 body，避免对非法 Lambda 重复产生
  诊断
- 验证：Semantic 覆盖构造函数/终结器嵌套块体 Lambda 的带值 `return`，以及构造函数
  Lambda 不得继承 `self.<let>` 最终绑定权限；完整 Semantic 单元测试通过

### P02：FCTS 生命周期 helper 误读 `seal` 字段

- 状态：已解决
- 发现阶段：首次运行新增 FCTS 用例时
- 实际结果：四个仅用于结束局部对象生命周期的 helper 在绑定对象后读取 `resource.value`；
  其中三个字段为 `seal`，Semantic 正确报告 `AE0308`
- 根因：该读取原本用于显式“使用”局部绑定，但 Feng 不要求以字段读取维持局部生命周期；
  局部绑定本身已持有对象并在函数退出时释放
- 处理结果：删除四个无语义作用的字段读取，不改变字段可见性，也不放宽语言规则
- 验证：FCTS 全量 821 项通过，新增 4 项全部通过

## 10. 实施结果

- Runtime 为每次用户终结器调用建立并直接移除 execution hold，返回移除后的程序强引用
  计数；普通 retain/release 快路径、托管对象 header 与公开 ABI 均未改变
- 循环回收在当前收集轮次内抑制嵌套收集，保留 survivor 的新增候选状态，并在释放
  free_set 前清除终结器期间产生的候选记录
- concrete method、constructor 与 finalizer 共用接收者 capture-cell 绑定 helper；非泛型
  finalizer 现已执行完整的 callable capture 需求收集、作用域清理与上下文恢复
- Semantic 分离词法成员实现身份与当前 callable body 身份，并让主解析、异常逃逸派生分析
  共用 Lambda callable-context 边界，完成 P01 修复且未改变 friend/spec-seal 授权
- Runtime、Semantic、Codegen 定向测试通过；FCTS 全量 `821/821` 通过，其中新增四项覆盖
  concrete/generic 非逃逸捕获、逃逸闭包复活、最终释放与直接 `self` 复活
- 沙箱外 `make test` 通过：macOS UBSan 与 normal 两阶段的单元测试、smoke、CLI、std、
  FCTS、性能约束、增量构建、发布及工具链脚本均通过；`git diff --check` 通过

## 11. 完成标准

只有同时满足以下条件，本专项才能标记为已解决：

1. 正式规范明确包含终结器 `self` 及 Lambda 捕获语义；
2. 非泛型和泛型终结器的普通局部与 `self` 捕获均可编译；
3. 不逃逸捕获不会递归终结、崩溃或错误复活；
4. 逃逸闭包能够安全复活对象，并在外部引用释放后按规则再次终结；
5. ARC 与循环回收路径均无悬空候选、重复释放或 Phase 1.5 误判；
6. 定向测试、FCTS 与沙箱外 `make test` 全部通过；
7. 实施问题记录和清单完整更新。
