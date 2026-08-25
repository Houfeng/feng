# Feng 编译器状态机与 `@awaitable` 异步机制草案 2

> **状态**：待 Review，未最终确定，未实施
>
> **日期**：2026-08-25
>
> **方案定位**：本草案是编译器参与异步 lowering 的候选方案，与现有
> [标准库 Future 规范](../specifications/feng-std-future.md) 所代表的“异步机制全部由 std 实现”方案并列；
> 本草案不修改、不替代现有方案，最终采用哪一个方案由 Review 决定。

---

## 1 背景

Feng 编译器当前不感知 `std`，也不应内建识别 `Future<T>`、`Promise<T>`、
`Option<T>`、`Scheduler` 或 `AsyncContext` 等标准库类型名称。

本草案讨论另一种分层方式：

- 编译器负责识别 `async` / `await`，并将异步函数编译为无栈状态机。
- 编译器通过内建注解 `@awaitable` 识别可等待对象，不感知该对象的类型名称。
- 编译器生成每次异步函数调用对应的状态对象和状态推进代码。
- std 负责实现 `Future` / `Promise`、线程池、EventLoop、Scheduler、
  `AsyncContext`、完成通知和并发同步。
- runtime 只提供 std 驱动编译器状态机所必需的最小内部入口。

本方案的目标不是把 Scheduler 放进编译器，而是建立编译器状态机与 std 调度器之间的最小协议。

## 2 已收敛的设计方向

本次讨论已经收敛以下方向：

1. 采用**无栈异步状态机**，不保存或切换原生线程栈。
2. `async func` 的调用返回可等待 owner；调用者只在 `await` 处可能挂起。
3. 异步函数的返回类型由 Feng 源码显式书写，函数体内的 `return` 继续按普通 Feng
   返回类型规则检查；编译器不改写返回类型，也不把结果值包装或适配为 `Future<T>`。
4. 编译器不识别任何 std 类型名称，只识别内建 `@awaitable` 注解及其结构协议。
5. 可等待 owner 不要求是 `@value type`；是否使用 `@value` 由 std 或应用层决定。
6. `@awaitable` 方法返回两槽联合类型；编译器只按活动 slot/tag 判断 pending 或 ready，
   不识别 `Option<T>` 名称。
7. `@awaitable` 方法接收当前等待方的编译器状态对象指针，使 std 可以保存该指针并在将来调度它。
8. 编译器为每个异步函数 `T` 生成私有的 `T_async_state` 状态结构；该状态对象本身就是调度句柄，
   不再引入公开的 `AsyncTask`、`Coroutine` 或额外 handle 对象。
9. runtime 提供 `feng_async_next(state)`，由 std Scheduler 调用以推进状态机。
10. 不采用 `onCompleted` 回调协议；等待注册与完成通知由 std 在 `pull` 实现中自行组织。
11. 线程池、EventLoop、Scheduler 和 `AsyncContext` 都在 std 中实现，编译器与 runtime 不选择调度策略。
12. 本方案不引入 `async_builder`；异步函数只返回 Feng 代码显式返回的值。

## 3 术语

### 3.1 awaitable owner

带有合法 `@awaitable` 方法的值称为 **awaitable owner**。

owner 是调用方持有、以后可再次尝试取得结果的句柄。`Future<T>` 可以是 owner，用户自定义类型也可以是 owner。
owner 是否为对象类型或 `@value type` 不属于 `@awaitable` 协议要求。

### 3.2 `T_async_state`

`T_async_state` 表示编译器为异步函数 `T` 生成的、每次调用独立持有的状态对象类型。
该名称只用于说明，不是 Feng 源码可引用的公开类型名称。

状态对象至少需要保存：

- 当前状态编号，即下一次进入状态机的位置。
- 跨越 `await` 仍然存活的参数、局部变量和临时值。
- 当前正在等待的 awaitable owner，确保恢复时仍对同一个 owner 执行 `pull`。
- `feng_async_next` 定位该状态对象对应推进函数所需的编译器私有元数据。

最终是否还需要在状态对象中保存异常和生命周期字段，取决于第 11 节待决策事项。

### 3.3 状态对象指针 `S*`

std 不感知各个具体的 `T_async_state` 类型。`@awaitable` 协议不规定状态指针的名义 pointee；
协议方法可以使用任意合法的 Feng 指针类型 `S*`。例如，std 可以自行声明一个无字段
`@abi seal type` 作为其状态指针类型：

```feng
@abi
seal type AwaitState {}
```

`AwaitState` 只是示例名称，不是编译器内建名称。`AwaitState*` 不表示额外分配的对象，
也不是独立 handle；它承载的就是实际 `T_async_state` 状态对象的指针。
具体指针桥接与恢复由编译器/runtime 内部完成。

## 4 `@awaitable` 协议

### 4.1 基本形态

候选协议形态如下：

```feng
type MyFuture<T> {
  @awaitable
  func pull(state: AwaitState*): PendingOrResult<T> {
    // 由 std 或应用层实现
  }
}
```

`@awaitable` 是编译器内建注解。编译器感知注解，不感知方法名 `pull`；方法可以使用其他名称。

### 4.2 方法约束

`@awaitable` 方法必须满足以下约束：

- 必须是实例方法。
- 除隐式 `self` 外，必须恰好有一个显式参数，该参数只要求是合法的 Feng 指针类型 `S*`；
  编译器不限制 pointee 的具体类型或名称。
- 返回类型必须是恰好包含两个直接成员的 union-form spec。
- 第一个直接成员表示 pending；编译器忽略该成员的 payload。
- 第二个直接成员表示 ready；该成员类型就是 `await` 表达式的结果类型。
- 编译器不检查两个成员的名称，也不要求第一个成员为 `None`。
- 编译器只检查当前返回联合本身具有两个 slot，不检查任一 slot 的成员类型内部是否还是联合类型；
  联合嵌套规则与 `@awaitable` 协议无关。
- 编译器沿用迭代器协议的可见面规则，在 owner 的 type 本体及当前可见的全部 `fit` 中
  查找 `@awaitable` 方法。
- `@awaitable` 不增加或改变重载规则；方法查找和重载决议完全沿用 Feng 当前规则。
- 不限制方法的 Feng 可见性；`open`、默认可见性和 `seal` 方法都可标注并参与协议。
- `await` 协议查找按 `@awaitable` 注解识别方法，不因方法为 `seal` 而忽略该协议方法。
- owner 不要求使用 `@value`。

### 4.3 可见性与符号表

`@awaitable` 与方法的 Feng 可见性是两个独立维度：

- `@awaitable` 表示该方法参与编译器 awaitable 协议。
- `open`、默认可见性或 `seal` 继续决定普通 Feng 源码能否直接访问该方法。
- 标注 `@awaitable` 的方法必须进入符号表，并保留原始可见性、完整签名和
  `@awaitable` 协议事实。
- consumer 编译器从 `.ft` 恢复后，必须能够按注解识别该协议方法；方法为 `seal` 时，
  普通成员访问仍按现有可见性规则处理。
- `@awaitable` 只使方法随其 owner 进入符号表，不反向提升 owner 自身的可见性，
  也不把原本不能进入相应符号表 profile 的 owner 变成导出根。

当前 Feng 符号表已经能够在收录声明时保持 `seal` 可见性，本规则复用现有能力，
不引入新的 Feng 可见性层级。

### 4.4 两槽联合协议

假设返回联合类型直接成员为：

```feng
spec PendingOrResult<T>: Pending | T;
```

编译器只读取 union tag：

- 第一个 slot，tag 为 `0`：本次未取得结果，当前状态机暂停推进。
- 第二个 slot，tag 为 `1`：本次取得结果，提取第二个 slot 的值并继续执行。

不对成员名称、具体类型或构造 API 增加特殊规则。

std 可以直接使用现有 `Option<T>`：

```feng
open type None();
open let none: None = ();
open spec Option<T>: None | T;
```

对应的 `@awaitable` 方法按 Feng 联合类型的普通值流返回结果：

```feng
type Future<T> {
  @awaitable
  func pull(state: AwaitState*): Option<T> {
    if self.isCompleted() {
      return self.readResult();
    }

    // std 必须在这里以无丢失通知的方式登记 state 及所需上下文。
    self.registerWaiter(state);
    return none;
  }
}
```

上例只说明协议值流，不确定 `Future<T>` 的最终字段与方法 API。ready 时直接返回 `T`，pending 时返回
`none`；不存在 `Option.none()` 或 `Option.some(value)` 特判。

## 5 `async func` 与 `await` 的表面语义

### 5.1 显式返回类型

异步函数必须显式声明其 Feng 返回类型：

```feng
async func forward(future: Future<Data>): Future<Data> {
  return future;
}
```

编译器不会把 `Data` 自动包装或改写成 `Future<Data>`，也不会通过名称识别 `Future`。
声明的返回类型必须满足本草案的 awaitable owner 协议。函数声明返回 `Future<Data>` 时，
每个携带表达式的 `return` 都必须返回与 `Future<Data>` 兼容的值；`return data` 不合法。

`@awaitable` 返回联合的第二个 slot 只决定 `await future` 表达式得到的结果类型，
不参与 `async func` 自身 `return` 语句的类型适配。

### 5.2 调用与等待

目标使用方式如下：

```feng
let future: Future<Data> = load(); // 返回 owner；调用者不在这里异步挂起
let data: Data = await future;     // 只有这里可能挂起当前异步状态机
```

“调用者不挂起”表示 `load()` 不会把调用者登记为等待方并暂停调用者状态机。
异步函数在调用时是立即开始执行到首次暂停点，还是只创建状态后交由 Scheduler 首次推进，尚需 Review 决定；
因此本草案暂不把“立即返回”进一步定义为固定的执行步数或时间上界。

### 5.3 `await` 使用限制

`await` 只能出现在 `async func` 的可挂起执行区域内。编译器对：

```feng
let value = await expression;
```

执行以下静态检查：

1. 求 `expression` 的静态类型 `R`。
2. 沿用迭代器协议规则，在 `R` 的可见面中查找 `@awaitable` 方法；可见面包括 type 本体
   及当前可见的全部 `fit`，方法自身为 `seal` 时仍可作为协议方法。既有方法查找和重载规则不变。
3. 检查方法参数和两槽联合返回形态。
4. 将第二个 slot 的类型作为整个 `await expression` 的静态类型。

`expression` 必须只求值一次；所得 owner 保存到当前 `T_async_state`，跨暂停继续存活。

## 6 编译器状态机 lowering

### 6.1 生成内容

对每个异步函数 `T`，编译器至少生成：

1. 对外调用入口。
2. 每次调用对应的 `T_async_state` 状态对象。
3. 根据状态编号继续执行的状态推进函数。
4. 将具体 `T_async_state*` 按所选 `@awaitable` 方法声明的指针类型 `S*` 进入
   awaitable/runtime 协议的内部桥接。

这些生成物都是编译器私有实现，不暴露给 std。std 只通过 `@awaitable` 方法观察到
当前等待方的状态对象指针。

### 6.2 `await` 展开

在状态机中执行 `await owner` 时，逻辑过程为：

1. 只求值一次 `owner`，并将 owner 保存到当前 `T_async_state`。
2. 保存所有跨暂停存活值和恢复状态编号。
3. 调用 owner 的 `@awaitable` 方法，并按该方法声明的指针类型传入当前状态对象指针。
4. 检查返回联合的活动 tag。
5. 若 tag 为第二个 slot，提取结果，清理当前 awaited owner 状态并继续执行。
6. 若 tag 为第一个 slot，退出本次状态推进；控制权返回调用 `feng_async_next` 的 Scheduler，
   或返回首次启动路径。
7. std 将来重新调度该状态对象后，状态机再次对保存的同一 owner 执行 `pull`；若仍为 pending，
   可以再次暂停。

编译器不得因 owner 名为 `Future` 或返回联合名为 `Option` 而生成不同路径。

### 6.3 状态对象持有 owner

状态对象必须持有活动 awaitable owner，而不是只保存某次 `pull` 的临时结果。原因是：

- owner 是后续继续取得结果的能力载体。
- 恢复可能是虚假唤醒，恢复后仍需再次 `pull`。
- owner 可能包含 std 自己的共享状态、调度器信息或 I/O 句柄。

具体持有、复制、retain/release 和 aggregate 生命周期规则必须服从 owner 自身的 Feng 值模型。

## 7 runtime 最小入口

std 需要一个能够推进编译器异步状态对象的 runtime 入口。该入口参数使用 awaitable 实现
选定的普通 Feng 指针类型；以下继续使用前文示例的 `AwaitState*`：

```feng
@runtime
extern func feng_async_next(state: AwaitState*): void;
```

`AwaitState*` 不是协议规定的固定类型。其他实现可以使用自己的合法指针类型；
相关声明的查找与重载继续沿用 Feng 当前规则。

`feng_async_next` 的目标职责只有：

1. 根据编译器私有元数据定位 `state` 对应的状态推进函数。
2. 推进该状态机，直到完成、再次 pending 或抛出未被状态机处理的异常。

它不负责：

- 维护 Scheduler queue。
- 创建线程池或 EventLoop。
- 选择在哪个线程执行。
- 感知 `Future` / `Promise`。
- 感知或切换 `AsyncContext`。
- 主动轮询所有 pending owner。

`feng_async_next` 是私有 runtime contract，不是面向普通 Feng 应用的公共 API。正式实施前必须按
[Runtime Contract API](./feng-runtime-contract-api.md) 的规则单独 Review 并进入白名单。

## 8 std Scheduler 与完成通知

### 8.1 通知链路

当 `pull(state)` 返回 pending 时，std awaitable 实现负责以自己的方式记录 `state`。owner 完成后：

1. std 从 owner 的等待记录中取得相应状态对象指针。
2. std 根据自身 Scheduler 或捕获的 `AsyncContext` 将状态指针放入 ready queue。
3. 线程池 worker 或 EventLoop 从 queue 取出状态指针。
4. 调用 `feng_async_next(state)` 推进等待方状态机。

queue 完全由 Scheduler 自己维护；编译器不向应用层暴露 `next` 方法，也不要求 std 理解
`T_async_state` 的字段。

### 8.2 ThreadPool Scheduler

线程池实现可以：

- 在 `pull` 返回 pending 前登记等待状态指针。
- owner 完成时把等待状态指针放入线程安全 ready queue。
- worker 从 queue 取出状态对象指针并调用 `feng_async_next`。

线程数量、任务窃取、优先级和公平性均是 std 策略，不进入语言协议。

### 8.3 EventLoop Scheduler

EventLoop 实现可以：

- 将 I/O 完成事件与一个或多个等待状态指针关联。
- I/O ready 后把状态指针放入 loop 的 ready queue。
- EventLoop 在适当阶段调用 `feng_async_next`。

编译器不需要感知文件描述符、IOCP、epoll、kqueue、libuv 或其他事件后端。

### 8.4 `AsyncContext`

`AsyncContext` 可以完全由 std 建立在本协议之上：

- `pull` 登记等待方时捕获当前 context。
- owner 完成后，把状态对象指针 post 到捕获的 context。
- context 在其选择的线程或事件循环中调用 `feng_async_next`。

是否默认捕获 context、是否允许跳过捕获以及 context 的嵌套规则，均属于 std API 设计；
编译器只提供可被重新推进的状态指针。

## 9 并发与原子性要求

`pull` 的关键语义不是单纯读取 ready 状态，而是“检查结果并在 pending 时登记等待方”的不可丢通知操作。

std 必须保证 `pull(state)` 与 owner 完成之间满足：

- 如果完成先发生，`pull` 返回 ready。
- 如果登记先发生，完成方能够取得已登记的 `state` 并调度它。
- 不允许在“检查为 pending”与“登记 state”之间丢失完成通知。
- 同一状态对象不得被并发执行多个 `feng_async_next`。
- 重复完成、重复入队和虚假唤醒的处理策略由 std 定义，但不得导致状态机并发重入或 use-after-free。

实现可以使用 Mutex、原子状态机或 EventLoop 单线程约束；协议不限定具体同步原语。

`Option<T>` 的两槽联合只解决“本次 pull 是否取得结果”的原子观察表示，不替代 std 对共享状态和等待登记的并发保护。

## 10 明确不属于编译器协议的内容

以下内容不进入编译器内建知识：

- `Future<T>` 与 `Promise<T>` 的名称、字段和构造方式。
- `Option<T>`、`None` 和 `none` 的名称。
- resolve、reject、错误包装和取消 API。
- Scheduler 的 queue 数据结构和执行策略。
- ThreadPool 与 EventLoop 的实现。
- `AsyncContext` 的捕获和恢复策略。
- owner 是否为 `@value`。
- 是否允许多个等待方等待同一 owner。

本草案也不增加 `onCompleted`、公开 `AsyncTask`、公开 `Coroutine` 或独立
`FengAsyncHandle`。当前候选协议也不增加 `async_builder`。`@awaitable` 方法接收的 `S*`
只是具体编译器状态对象指针的协议视图。

`@awaitable` 不要求协议方法必须为 `open` 或 `seal`。方法是否能被普通 Feng 源码调用，
完全沿用其声明可见性；`await` 是否识别该方法只由 `@awaitable` 协议事实决定。

## 11 待 Review 的阻断事项

以下问题尚未在本次讨论中闭环，不能在实现阶段自行决定。

### 11.1 状态对象生命周期

std 会在 `pull` 返回后保存状态对象指针，因此 `T_async_state` 必须在暂停期间持续存活。
需要明确：

- 状态对象由谁持有以及何时释放。
- `pull` 保存的是借用指针还是带有所有权的调度引用。
- `feng_async_next` 是否消费一次调度引用。
- owner 永不完成、Scheduler 丢弃队列项或未来加入取消时如何释放状态对象。
- 多个 await 和重复入队如何避免泄漏与 use-after-free。

这是 runtime 生命周期与性能决策，必须单独 Review，不能由实现自行选择。

### 11.2 首次启动策略

需要在以下语义中确定一种：

- 调用线程立即执行状态机，直到首次 pending 或本次推进结束。
- 首次推进也交给 std Scheduler。

两种方案的执行时机、异常行为、延迟和调度开销不同。

### 11.3 异常、reject 与取消

需要明确：

- `@awaitable` 方法在 ready 时能否通过 `throw` 表示 reject，使 `await` 表达式抛出异常。
- `feng_async_next` 推进状态机时遇到未捕获异常的边界行为。
- 取消是否属于第一版范围。
- 取消后仍在 std queue 中的状态对象指针如何处理。

两槽联合只表达 pending/ready，不为错误或取消强制增加第三个 slot；错误也可由 std 通过结果类型或异常表达，
但最终规则必须由 Review 确定。

## 12 与现有 std-only 方案的关系

| 维度 | 现有 std-only 方案 | 本草案 |
| --- | --- | --- |
| 状态机生成 | std/注解机制负责 | 编译器负责 |
| 编译器感知 std | 不感知 | 仍不感知 |
| 编译器内建协议 | 无异步专用协议 | `async`、`await`、`@awaitable` |
| Future/Promise | std 定义 | std 定义 |
| Scheduler/AsyncContext | std 定义 | std 定义 |
| 状态恢复入口 | 由 std-only 方案自行设计 | `feng_async_next(S*)`，其中 `S*` 为实现选定的指针类型 |
| 调度句柄 | std 自己的任务抽象 | 编译器生成状态对象的指针 |

两个方案在最终决策前并列保留。若采用本草案，需要另行更新正式语言规范、runtime contract 规范和 std async 规范；
不能直接把本工程草案当作正式语义来源。

## 13 性能目标

本草案的目标包括：

- 不切换原生线程栈。
- `await` 表达式只求值一次。
- ready 快路径只执行一次 `pull`、一次 tag 判断和必要的结果移动。
- pending 路径不通过回调闭包额外包装状态对象。
- std queue 直接保存状态对象指针，不额外创建公开 task/handle 对象。
- owner 使用 `@value` 时，协议本身不强制为 owner 额外分配对象。

但是，`@value Future<T>` 只能说明 Future owner 自身可采用值表示，不能据此推导整个异步调用零分配。
跨暂停存活的 `T_async_state`、共享完成状态和 std 数据结构是否分配，取决于最终生命周期和实现方案。

所有新增 runtime 开销、状态对象分配策略和引用管理策略都必须在实施前进行人工性能决策。

## 14 建议的实施顺序

只有本草案 Review 通过并解决第 11 节阻断项后，才进入实现：

1. 把确定语义收敛到正式语言规范，现有 std-only 文档按最终方案调整为引用正式规范。
2. 定义 `async` / `await` AST、语义检查和诊断码。
3. 定义内建 `@awaitable` 注解及跨包符号持久化。
4. 定义 `T_async_state` lowering、控制流拆分和 owner 生命周期。
5. Review 并加入 `feng_async_next` runtime contract。
6. 在 std 中实现至少一个 `Future` / `Promise` 和一个 Scheduler。
7. 分别实现 ThreadPool、EventLoop 和 `AsyncContext` 集成。
8. 增加编译器测试与 FCTS，最后执行全量 `make test`。

测试至少覆盖：ready 快路径、一次和多次暂停、分支/循环中的 `await`、多个连续 owner、
owner 为普通 type 与 `@value type`、完成与登记竞争、线程池恢复、EventLoop 恢复、
`AsyncContext` 恢复、异常路径、跨包 `@awaitable` 以及状态对象生命周期。

## 15 Review 清单

- [x] 采用无栈编译器状态机。
- [x] `@awaitable(state: S*) -> 两槽联合`；`S*` 可为任意合法 Feng 指针类型。
- [x] 第一个直接 slot 为 pending、第二个直接 slot 为 ready result；编译器只检查当前联合的 tag，
  不检查 slot 内部是否嵌套联合。
- [x] owner 不要求 `@value`，编译器不识别任何 std 类型名。
- [x] `@awaitable` 不限制方法可见性；协议方法进入符号表并保持原始可见性。
- [x] awaitable owner 发现沿用迭代器的可见面规则，包含 type 本体及当前可见的 `fit`。
- [x] 状态对象持有当前 awaitable owner。
- [x] std Scheduler 通过 `feng_async_next(state: S*)` 推进状态机。
- [x] `async func` 的 `return` 必须返回声明的 owner 类型，不按 ready slot 做类型适配。
- [x] 异步函数只返回 Feng 代码显式返回的值，不引入 `async_builder` 或生产侧绑定协议。
- [x] `await` 只能出现在 `async func` 中。
- [ ] 决定 `T_async_state` 生命周期与调度引用所有权。
- [ ] 决定首次启动策略。
- [ ] 决定异常与取消范围。
- [ ] 最终决定采用现有 std-only 方案还是本草案。
