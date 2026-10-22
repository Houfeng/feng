# Feng async/await 与 `@future` 实施草案 4

> **状态**：待 Review，未实施，不覆盖正式规范。§1 为已确认原则；其余为提交 Review 的具体方案，批准后实施。void 类型方案单独 Review 和交付，依赖关系见 §3.1。
>
> **更新**：2026-10-22。本文集中定义异步增量；普通类型、初始化、异常和生命周期规则引用主规范。

## 1 原则与两个阶段

- 编译器将 async/await 转成无栈状态机；lowering 位于源码语义分析之后、发码之前。Codegen 接收普通 AST 及完整的类型、绑定和生命周期信息，不处理 await 控制流。
- 薄状态机保存执行位置、跨挂起数据及有效清理；厚 Future 保存完成状态、结果、异常和等待关系。
- Future 单向拥有状态机；状态机不保存所属 Future。结果经推进调用边界交付。
- `@future` 采用单槽注册，不绑定类型名或 std 模块名，规则见 §2。
- 无结果异步按 [void 类型方案](feng-void-type-dev.md)使用 `Future<void>`，与有结果实例共享声明槽；该前置方案最终采用后独立交付，不在本文重复定义。
- 调用时立即执行至首次实际挂起或完成；已完成的 await 同步继续。`return await f` 显式等待，`return f` 不自动展开。
- runtime 只提供库无法直接操作的不透明状态机 API。调度由库实现；std 提供同步上下文，支持线程池、事件循环等实现。

| 阶段 | 交付 | 验收依赖 |
| --- | --- | --- |
| 一：编译器与 runtime | §2—§8 的语义、lowering、私有 API、FT 元数据和语言服务；§10 的编译器及行为测试 | 测试专用 Future 和手动驱动器，无须先实现 std 调度器 |
| 二：库 | std Future/Promise、同步上下文、线程池及事件循环接入；见 §9 | 使用阶段一的同一协议，不再修改语言规则才能运行 |

## 2 单槽注册

每次分析在源码及实际依赖闭包中登记一个规范化类型声明。源码声明收集、依赖元数据读取时顺带登记，不增加独立的类型遍历；未直接引用的依赖公开模块也参与，不扫描未使用的安装包。

| 输入 | 处理 |
| --- | --- |
| 首个合法声明 | 保存声明身份、协议成员和诊断来源 |
| 同声明重复导入、别名或菱形依赖 | 幂等 |
| 第二个不同声明；同声明重复标注 | 报错，冲突诊断指向双方来源 |
| 无注册且未使用协议 | 允许 |
| 使用协议但无注册 | 使用处报错 |

登记必须先于协议使用检查，且与文件／导入顺序无关；LSP 每次分析重建。泛型实例化共享声明槽。显式返回类型不能绕过注册冲突，仍遵循普通可见性规则。

协议类型为公开模块中的公开、引用语义对象 `type`；不允许 `@value`、`@abi`、tuple 或 spec 作为协议实现主体。泛型参数布局不固定；各实例的成员签名按普通泛型规则代入。

## 3 源码规则

### 3.1 独立前置：void 类型

void 的目标语义、实施范围及验收统一见 [Feng void 类型开发方案](feng-void-type-dev.md)。本文以该方案最终采用并交付为前提，不为 Future 单独放宽类型规则。

两份文档分别 Review、实施和验收：void 可在没有 async/await 时独立交付；async/await 接入其结果，仍按 §1 的编译器／runtime、库两个阶段交付。若 void 方案未采用，本文的无结果协议须重新 Review，不能默认为切换双槽或 Unit。

### 3.2 语法与类型

```feng
// 示例名称均由库定义；Future<int> 的 value 方法返回 int。
async func twice(): Future<int> {
  let x = await load();
  return x * 2;
}

// 同一声明的 void 实例；body 可以空 return 或正常落尾。
async func save(): Future<void> {
  await write();
  return;
}
```

- `async func` 用于有实现体的普通函数／方法；Lambda 写作 `async (参数) -> 表达式` 或 `async (参数) { ... }`，仍须具备现有 callable-form spec 目标类型。泛型 callable 同样适用。
- `async` 在上述前缀位置、`await` 在 async callable 的一元表达式位置作为上下文关键字；`std.async`、`obj.await()` 等成员／模块名称仍合法。async body 内的局部绑定及参数不得命名为 `await`，嵌套 callable 独立判断。
- `await` 与其他前缀一元表达式同级，操作数包含完整的后缀调用／成员链；`await f() + g()` 等于 `(await f()) + g()`。普通函数和顶层使用 await 报错。
- 构造函数、终结器、`main`、`extern func`、顶层 `@abi func` 不接受 async。同步入口通过库驱动异步；普通外部函数可以按其既有 ABI 返回合法值，但不参与状态机生成。
- spec 中只声明返回 F 的普通方法／callable 签名，不标 async；实现可以同步返回 F，也可以使用 async。
- `F` 必须来自 §2 的声明；`T` 为其 `@future("value")` 方法代入后的返回类型。T 按普通类型规则检查，包括 §3.1 引用方案中的 void。
- async body 按 T 检查返回值及正常落尾；调用表达式类型为 F。普通函数不自动把 T 包装成 F。T 本身为 Future 时保持嵌套，不自动展开。
- defer 自身不允许 await；嵌套 async Lambda 的 body 独立检查。try/catch、分支、循环、普通表达式均可挂起。

### 3.3 返回类型推导

有显式 F 或 Lambda 目标 F 时直接得到 T。否则先按[函数规范](../specifications/feng-function.md)推导 body 的 T，再将协议 value 方法的返回类型模式与 T 做结构统一：规范化声明、泛参槽位、数组及 tuple 等结构均须一致，重复槽位约束须一致；全部协议类型参数唯一确定且满足约束才得到 F。

不反推隐式转换，不补猜测实参；无法唯一确定时要求显式 F。无 return 或仅空 return 的 body 推导 T 为 void，再按同一规则确定 F；只有无目标类型的 `()` 等表达式、无法确定 T 时要求显式返回类型。推导不扫描其他协议声明。

## 4 Future 协议

以下是**签名模式**，不是完整可编译的类。F 为当前协议实例，T 为结果类型，E 为已注册 `@throwable` 根 spec，W 为库声明的无参数、返回 void 的 callable-form spec。所有实例使用同一 W 声明。

| 标记 | 成员形状 | 契约 |
| --- | --- | --- |
| `@future("state")` | `seal var state: u8*` | 编译器拥有的状态机槽，默认零值表示无状态机；见 §6 |
| `@future("ready")` | 实例方法 `(): bool` | 非阻塞查询是否成功或失败；true 后结果不再改变 |
| `@future("poll")` | 实例方法 `(wake: W): bool` | 已完成返回 true，不登记、不通知；否则原子地登记 wake 并返回 false |
| `@future("value")` | 实例方法 `(): T` | 已成功则按普通值规则返回结果；已失败则抛出保存的 E；未完成调用是协议错误 |
| `@future("resolve")` | 实例方法 `(value: T): void` | 发布一次成功结果，再通知等待者 |
| `@future("reject")` | 实例方法 `(error: E): void` | 发布一次失败结果，再通知等待者 |
| `@future("resume")` | 实例方法 `(): void` | 串行驱动本对象的状态机；算法见 §5 |

T 为 void 时，value 成功返回 void，resolve 接收唯一的 void 值；仍须保存完成／失败状态并执行通知。无载荷不等于未完成，不另设无结果协议或注册槽。

每个角色恰好一个，不允许成员级泛参或变长参数；按声明身份登记角色，重复报错。角色方法可以是 seal；只有 lowering 产生的协议访问和经校验的 runtime 适配器获得对应访问权，不扩大普通源码的可见性。普通源码不能读写、取地址或复制 state 槽，只能通过 §6 的 API 操作。

ready、poll、resolve、reject、resume 必须通过现有异常摘要证明无异常逃逸；value 允许抛出。ready 仅提供快速路径，返回 false 后必须由 poll 重新检查并原子登记；不能直接登记而遗漏中间的完成。poll 返回 false 后，若对象完成，恰好通知一次，允许在 poll 返回前通知。通知不传递结果，恢复后读取同一个 Future。重复完成、非法状态操作为协议违约，不得变成重复结果交付。

Future 可多次 await，也可有多个等待者；读取不消耗结果。失败保存为普通 E 值，不保存 `_Unwind_Exception` 或 TLS catch 指针。W 仅用于恢复通知，不能向调用方抛异常；库对无法静态证明安全的间接通知调用须在内部捕获，并将逃逸视为协议违约终止，不能悄悄吞掉。编译器不要求结果、异常或等待列表采用特定字段布局。

### 4.1 创建与首次执行

生成入口执行：按参数求值顺序保存调用输入 → 分配 F 并执行普通字段初始化器 → 创建状态机并绑定 state → 调用 resume → 返回 F。不调用协议指定构造函数，也不跳过字段初始化器；普通库手工构造仍遵循普通构造规则。

参数求值、字段初始化或建帧失败沿同步调用传播，并清理已建立的资源；body 开始后的未捕获异常进入 reject，包括首次挂起前的异常。所有初始化完成后才允许通知引用 F。

async 调用按普通值边界保存参数和接收者：引用类型保活原对象；值类型接收者复制到该次计算的状态中，之后的 self 修改该副本。闭包捕获可变绑定继续共享原绑定单元，不转为注册时快照。此接收者规则是本次新增提案。

## 5 await 与恢复时序

lowering 的逻辑伪码如下；方括号表示编译器操作，不是新语法。

```text
进入 await child_expr：
  child = [求值一次并保活 child_expr]
  pc = 当前 await 的读取位置
  if child.ready():
    转到读取位置
  if child.poll([本次推进借入的 owner.resume 方法值]):
    转到读取位置
  return Pending

读取位置：
  value = child.value()          // 在原 await 位置参与 try/catch
  [释放本次等待专用的 child 持有]
  [继续原控制流]
```

被等待 Future 保存的 wake 强持有父 Future；队列任务也强持有其目标，直到执行或移除。父状态机只持有 child 等源码数据，不保存 owner 或 owner.resume。对象图可成环，必须按 §6 纳入现有循环回收。丢弃一个外部引用不等于取消；仍可达的等待通知可以保活父计算。这是本次选择的持有策略，不引入 weak/unowned 原语。

resume 的库实现使用短临界区维护 `running/requested/closed`；推进、通知及用户代码均在锁外执行：

```text
resume：
  [锁内] closed 则返回；running 则设置 requested 并返回；否则置 running
  循环：
    [锁内] 清除 requested
    result = feng_async_next(self)
    [锁内] result != Pending：清除 running 并返回
           requested：继续循环
           否则清除 running 并返回
```

最后一次检查 requested 与清除 running 必须在同一临界区内。这样 poll 内的同步通知只登记再次推进请求，不递归进入状态机。返回 false 的登记必须最终提供一次完成通知；禁止以无结果的虚假通知提前进入读取位置。恢复投递及上下文捕获见 §9。

## 6 状态机与 runtime 私有 API

### 6.1 表示、持有与清理

state 的 u8* 是协议专用不透明句柄，不是普通可解引用指针；零值仅为私有 ABI 的空槽表示，不引入 Feng null。

状态帧使用现有托管对象头及描述符，包含 pc、执行状态、跨挂起值和有效清理信息。尚未初始化／已释放的值槽采用编译器生成的普通 `Empty | T` 具名 union，默认 Empty；T 在源码求值位置才构造，避免建帧时提前执行 T 的默认初始化。该 tag 同时决定槽是否参与 trace／release，不保存未定类型值。

lowering 将 state 槽登记为一条托管强边，并生成帧内引用的普通 trace／release 信息；不能因源码槽写作 u8* 就从循环图中遗漏它。普通原始指针规则不变。

该所有权事实进入统一生命周期元数据，Codegen 只消费通用的字段及描述符信息；F 的普通释放路径释放 state 边，帧的编译器生成终结路径执行尚未执行的清理。循环路径同样须调用帧清理，不能只依赖不会在该路径执行的 release-children。每个清理先失效再调用，严格保持原有 defer／局部释放的交错逆序。

帧关闭幂等；正常完成已经清理过的作用域不再执行。关闭开始后不能恢复业务代码；即使清理导致对象复活，帧也保持 Closed，库按关闭态处理，不能重新运行。尚未初始化的槽不释放。持有类型参数的帧保存普通泛型描述符，不改变泛型 ABI。

### 6.2 Feng 侧入口

以下为新增私有 runtime contract 的签名提案；由编译器验证 F 来自注册声明，拒绝其他类型。名称固定的是 runtime 入口，F 及成员名称仍由库定义。

```feng
@runtime
extern func feng_async_next<F>(owner: F): i32;
@runtime
extern func feng_async_status<F>(owner: F): i32;
@runtime
extern func feng_async_destroy<F>(owner: F): void;
```

| API | 结果与要求 |
| --- | --- |
| next | 返回 0=Pending、1=Completed、2=Closed、3=Empty；从 Ready/Waiting 推进，遇到下一次挂起或结束返回；Done/Closed/空槽直接返回对应状态 |
| status | 返回 0=Ready、1=Running、2=Waiting、3=Done、4=Closed、5=Empty；只查询，不推进 |
| destroy | 与 next 互斥；取走并清空 state，将原强持有转入本次调用；置帧为 Closed，清理后释放持有；空槽无操作；不发布结果 |

这些入口不调度、不阻塞等待、不创建线程。库必须用 §5 的 gate 排除并发访问；runtime 不再加一套锁，也不承诺检测非法并发。帧有独立调用标志，next/destroy 从进入到结果交付结束均占用，在有效持有下发现同步重入即终止。调用期间 owner 按普通调用持有规则存活；status 只在 gate 确认无在途修改时查询，不能用于无同步的“先检查后操作”。

编译器为 F 生成 `FengAsyncOwnerOps`：state 槽偏移、W 方法值构造适配器、resolve/reject 适配器及闭合类型信息。resolve/reject 适配器接收 owner 和 T/E 的借用载荷；有载荷时以存储指针传递，void 不读取载荷，按普通 void 参数规则调用 resolve，不额外装箱。C 入口固定为：

```c
int32_t feng_async_next(const FengGenericParamDescriptor *f, void *owner);
int32_t feng_async_status(const FengGenericParamDescriptor *f, void *owner);
void feng_async_destroy(const FengGenericParamDescriptor *f, void *owner);
```

描述符 f 按既有泛型 runtime contract 传递，并通过新增的可选 async-owner 元数据引用上述静态适配信息。无此元数据的类型拒绝调用；不开辟全局句柄表，不按名字查找成员。描述符增量是静态元数据，不增加普通对象的实例字段；该私有 ABI 变更要求重建匹配的 provider/consumer 产物。

帧操作表提供 `advance(frame, call)` 和 `close(frame)`。call 仅在本次推进有效，借入 owner 及其静态适配信息；不可存入帧。Pending 保留逻辑作用域，只拆除本次物理异常框架；成功／失败先完成逻辑作用域清理，再退出物理框架。之后经 call 交付本次结果。

next 将 Ready/Waiting 置为 Running；advance 返回 Pending 后置为 Waiting，完成则置为 Done，再调用对应适配器保存并通知。Waiting 只在其登记的完成通知到达后推进。结果由本次调用持有，适配器按普通参数规则接收后释放临时持有。next 在整个调用中保护帧存活；destroy 的槽位先清空，使清理中的重入销毁无操作。

不额外构造长期存活的结果盒；结果大小／复制／释放沿用现有类型描述符。句柄访问、状态检查和不透明操作表分派属于 runtime；普通协议方法及调度属于 Feng 库代码。

## 7 lowering、异常与工具链

### 7.1 转换顺序

```text
Parser → 源码 Semantic → async lowering → 生成节点语义／生命周期事实 → Codegen
```

1. 先将含 await 的表达式拆为有序临时绑定和控制流，保留短路、分支、循环及求值次数。
2. 对正常、异常、清理边求活跃信息；跨挂起值进入帧，闭包共享单元及已取地址存储保持身份。原始指针借用仍按互操作规范由调用方保活 owner，不新增隐式安全承诺。
3. 生成普通内部类型、启动／advance／close 函数及 pc 分派；生成声明不进入用户可见符号空间。挂起只退出物理推进，不退出源码逻辑作用域。
4. 重建受影响的符号、成员绑定、泛参替换、异常和所有权旁表；不复用指向已替换节点的语义引用。源码 AST 保留给 LSP 和诊断，生成节点关联原源码位置。

已完成的 await 不设置调度任务；Pending 路径保存 pc 和必要数据后返回。close 只执行有效清理及释放，不运行 await 后续业务代码。共享绑定和需要稳定地址的槽不参与字段复用；其他字段复用属于正确性验收后的优化。

### 7.2 异常衔接

- body 失败经 E 保存；value 抛出时按原 await 的词法位置捕获。调度回调不能成为业务异常接收者。
- catch 内允许 await。挂起时将仍活跃的异常载荷及其类型身份保存为受持有的值，退出本次 TLS catch／cleanup 框架；恢复后重建逻辑 catch 关系。`throw;` 保留原载荷身份，原生展开记录不跨挂起保存；内层 catch 不覆盖外层保存值。
- `f()` 的摘要包含参数求值、初始化等同步逃逸；body 的失败由生成边界捕获。`await f` 另合并 poll/value 的普通摘要；结果读取抛根值而无法证明具体类型时保留 unknown，不宣称无异常。阶段一不增加跨任意 Future 值的精确失败来源追踪系统。
- [defer 主规范](../specifications/feng-defer.md#6-与异常模型的关系)已经禁止异常逃逸，原规则不变。当前 `feng_throw` 在 finalizer 中无条件 panic，尚不满足[生命周期规范 §13.2](../specifications/feng-lifetime.md#132-终结器执行流程)允许内部捕获的要求。
- 阶段一必须统一修正终结器异常边界：建立边界保护框架，允许边界内 catch；无匹配内部处理器时在越过边界前终止，不能被外部 catch 接管。throw 与 rethrow 使用同一边界规则，不按“是否 async”分支。此项是已有语义的 runtime 配套修正，须单独验证。

### 7.3 元数据与诊断

FT 保存 async 源声明标记、协议声明身份、角色成员身份与签名、state 所有权事实、初始化及 runtime 适配入口；seal 角色只供编译器查询，不能进入普通成员可见面。按既有版本／缓存失效规则更新格式，源码与纯 FT 消费一致。LSP Hover/SignatureHelp 展示调用返回 F、await 得到 T；生成函数不成为用户补全结果。

新增诊断覆盖：标注目标／角色／签名错误、重复注册、缺失协议、非法 async/await 位置、F/T 不匹配、推导不唯一、defer 挂起和源码非法访问 state。编号按现有诊断主表分配；避免在类型解析失败后追加依赖性协议错误。

## 8 第一阶段实施顺序与成本

进入本阶段实施前，[void 类型方案](feng-void-type-dev.md)须已独立交付；其基础语义和工具链验收不并入本文。

| 顺序 | 工作及完成条件 |
| --- | --- |
| A | Review 批准本方案；将源码行为收敛到语言／函数／注解相关主规范，生命周期规则只引用原主规范；旧 async 方案标为历史方案 |
| B | Lexer/Parser、单槽与角色登记、F/T 检查、FT 读写及缓存；非法程序在源码语义阶段诊断 |
| C | 独立 lowering、通用生命周期元数据、runtime 操作适配及终结器边界；三条路径均可运行：直接完成、挂起恢复、挂起销毁 |
| D | 接入测试 Future／手动驱动、语言服务，完成 §10 验收；仅随后进入 std 阶段 |

成本预算：每次调用一个 Future 和一个状态帧；有捕获的通知闭包按现有规则分配，不声称每次 await 零分配。ready 为 true 时不构造 wake、不入队；不跨挂起的局部不提升；defer 使用静态槽／标志，不分配注册节点。协议查找均在编译期；非 async 函数不增加状态机或 gate。库承担 gate 同步成本，runtime 只检查调用及生命周期状态；可能成环的帧及 Future 保留正常循环回收成本。

阶段一记录同步完成、一次／多次挂起的分配数、retain/release、耗时及峰值内存，与等价手工协议实现对照。不得为性能省略清理或所有权；额外装箱、额外运行时全局表或通用同步开销需另行 Review。

## 9 第二阶段：std 与同步上下文

std 实现 §4 的唯一 Future 声明及 Promise；手工 Promise 的 state 为空，仍使用相同 poll/value。Future 初始化器建立结果及 gate 的初始状态；resume 按 §5 推进。resolve/reject 在锁内发布终态并取走等待列表，锁外通知；value 在同步保护下读取结果。

库的内部关闭请求先阻止新推进；若 running，只登记关闭请求，由驱动者在本次 next 返回后处理，不等待自己。无在途推进后调用 destroy；原来未完成的 Future 发布 `Error("async/closed")`，已完成结果不变。该关闭步骤用于生命周期协调，不新增公开取消 API。

若清理后对象复活，ready/poll/value/resume 在 gate 内确认未 running 后检查帧状态，非空帧为 Closed 时同样转换为关闭失败，不重新挂起；Empty 是手工 Promise 的合法状态，不等于 Closed。不可达对象回收只释放通知，不投递新任务。runtime destroy 本身不构造这种库错误。

公开生产接口为 `Promise<T>.future: Future<T>`、`resolve(value: T): void`、`reject(error: E): void`；Promise 不能重复完成。Future 的协议生产角色保持 seal，对外提供 `isReady(): bool` 和 `result(): T`，后者只读取已完成结果。普通函数需要等待时使用上下文的库驱动入口，不把阻塞等待放入 Future。无业务结果使用 `Promise<void>`／`Future<void>`，成功完成调用 `resolve(void)`。

同步上下文的最小契约如下，类型名不是编译器协议的一部分：

```feng
// 接口示意；W 为 §4 的同一普通 callable-form spec。
open spec SynchronizationContext {
  func post(work: W): void;
}
```

库提供当前线程上下文的读取及作用域安装。poll 真正登记等待时捕获当前上下文；完成后通过该上下文 post 恢复任务，没有安装上下文时使用线程池上下文。已完成 await 不 post。执行任务时安装该上下文，退出恢复原上下文；上下文存取和恢复全部在库中。

post 不内联执行 work，必须保证接受的任务最终执行；停止上下文前先排空任务，不静默丢弃。线程池实现不保证同一线程；事件循环实现固定在线程内执行。任务强持有目标 Future，恢复前检查关闭状态；同一 Future 即使收到同步通知也由 gate 串行推进。跨线程共享数据的同步责任不因 async 改变。

不把取消、超时、异步流或异步析构追加为本轮语言特性；后续库扩展不得改变上述状态机契约。

## 10 验收矩阵

| 层次 | 必须验证 |
| --- | --- |
| Lexer/Parser/Semantic | 上下文关键字与现有成员名；所有合法 callable 形式与非法位置；返回推导；角色缺失／重复／签名；同包、跨包、菱形依赖与重新分析 |
| lowering／Codegen | 操作数只求值一次、短路、多 await 的嵌套表达式；循环 break/continue；泛型、值类型接收者、闭包共享绑定、稳定地址；生成节点类型及旁表完整 |
| FCTS | 有结果及 void 实例共享注册、返回推导与完成协议；立即启动；已完成成功／失败且不构造 wake；手工 Promise；多个等待者与重复 await；同步登记通知；ready/poll/完成交错；状态机串行执行 |
| 异常／资源 | 首次推进失败、await 失败、catch 内挂起后重抛；祖先块 defer、清理修改原绑定、部分初始化；正常／异常／销毁各清理一次；finalizer 内捕获及越界终止 |
| runtime／内存 | 空 state、完成／关闭后再次推进、重复 destroy；帧 trace、ARC 与循环释放、复活后不恢复业务；外部队列保活与销毁无悬空引用；同步重入检测及库 gate 排除并发 |
| 包／工具 | 源码、纯 FT、workspace cache 一致；协议 seal 元数据；泛型 provider/consumer；原始源码诊断与 Hover |
| std | 线程池／事件循环上下文；上下文恢复；停止时排空；完成／关闭并发；实际异步操作接入 |

阶段一新增隔离依赖的测试协议及异常根，不同时加载 std 的协议声明，也不修改既有“未实现”断言；std 接入后这些测试仍能独立运行。阶段二替换占位实现时单独批准旧测试迁移。`test/` 检查诊断、AST、生成代码；`fcts/` 检查语言行为。两个阶段均按仓库要求在沙箱外执行 `make test`，二进制在 build/ 或 temp/ 中运行；按目标矩阵记录实际验证平台，已有后端缺口与未运行项单列。

## 11 Review 要点与参考

本次建议整体批准后才能称为实施基线。单槽注册已确认；void 决策集中在其独立开发方案中，本文仅检查接入。仍需 Review：结果读取角色、构造器无关的初始化及值接收者复制、通知强持有父 Future、state 托管边与三项私有 API、defer 禁止挂起及通用终结器边界修正、按等待点捕获同步上下文；这些具体方案不回溯标记为此前已批准。

参考只用于核对行为，不要求复制其他语言的类型布局：

- [C# async 函数及 task builder](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/classes#1514-async-functions)：调用时启动、结果／失败完成及编译器与库的协作。
- [C# await 求值](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#1299-await-expressions)：完成检查、通知与结果读取分工。本文在 ready 快速路径之后由 poll 原子复查并登记，采用 Feng 的注解角色和单槽规则。
- [.NET SynchronizationContext / ConfigureAwait](https://devblogs.microsoft.com/dotnet/configureawait-faq/)：恢复位置由库安排；本文选择统一异步 post，未照搬所有内联策略。
- [Feng 异常规范](../specifications/feng-exception.md)、[defer 规范](../specifications/feng-defer.md)、[生命周期规范](../specifications/feng-lifetime.md)、[tuple 规范](../specifications/feng-tuple.md)：沿用其普通规则。

旧 [awaitable 草案 2](feng-async-awaitable-draft-2.md)、[自定义注解草案](feng-custom-annotation-draft-1.md)和[std Future 规范](../specifications/feng-std-future.md)本轮不修改；Review 批准后的状态调整按 §8 执行，不同时维护两套异步定义。
