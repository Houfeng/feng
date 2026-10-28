# Feng async/await 与 `@future` 实施草案 4

> **状态**：待 Review，未实施，不覆盖正式规范。§1 的分层、§2 的双固定槽注册及结果映射、§3.1 的 C# 式返回语义、隐藏托管槽及编译器首次执行已确认；采用 §4—§6 的最小方案：await 接入库等待，通知后恢复，通过 runtime 取结果，手工 Promise 使用 runtime 可读取的完成状态对象。方法泛参 P 表达父 Future，私有 ABI 接口提案及同步交接等未闭合项集中在 §11。本文不依赖 void 类型方案。
>
> **更新**：2026-10-28。本文集中定义异步增量；普通类型、初始化、异常和生命周期规则引用主规范。

## 1 原则与两个阶段

- 编译器将 async/await 转成无栈状态机；lowering 位于源码语义分析之后、发码之前。Codegen 接收普通 AST 及完整的类型、绑定和生命周期信息，不处理 await 控制流。
- 状态机保存执行位置、跨挂起数据、有效清理及最终成功值／异常；手工 Promise 使用仅保存完成状态及结果／异常的托管对象。两者均可经 runtime 读取，库负责等待、通知、wake 和调度。
- Future 通过隐藏槽单向拥有状态机或完成状态对象，后者不反向持有所属 Future。结果不要求复制到库字段中另存一份。
- `@future` 按目标类型的泛参数量登记到两个固定槽，每槽唯一，不绑定类型名或 std 模块名，规则见 §2。
- 编译器只识别 `@future` 和 `@future("await")`。编译器创建并初始化 Future、绑定状态机并直接发起首次执行；在 await 位置调用被等待对象的库入口，该入口允许 seal。不依赖特定构造函数。
- 无结果异步使用 `Future`，有结果异步使用 `Future<T>`；源码返回及 await 规则见 §3.1。
- 调用时立即执行至首次实际挂起或完成；已完成的 await 同步继续。返回语义统一见 §3.1。
- runtime API 接收 Future，经描述符定位隐藏字段，提供状态机操作及统一结果访问。调度由库实现；std 提供同步上下文，支持线程池、事件循环等实现。

| 层次 | 职责 | 不承担 |
| --- | --- | --- |
| 编译器 | 生成隐藏槽及状态机；创建、初始化、绑定并首次推进；生成 await 入口调用、结果／异常存储及恢复后的结果读取 | 为 Future 另写 ARC 流程、识别 await 以外的库方法、决定调度策略 |
| runtime | 接收 Future，提供挂起交接、恢复、状态查询、统一结果读取及关闭清理；支持手工完成状态对象的创建／关联／结果发布；释放复用普通对象流程 | 自行分派库成员、按 Future 类型特判释放、管理等待列表、发送通知或调度 |
| 库 | await 入口、完成检查及等待登记、wake、后续串行驱动、通知、同步上下文及调度；经 runtime 完成手工 Promise | 声明或直接访问隐藏槽、解释状态对象布局、手动维护状态机引用计数 |

| 阶段 | 交付 | 验收依赖 |
| --- | --- | --- |
| 一：编译器与 runtime | §2—§8 的语义、lowering、私有 API、FT 元数据和语言服务；§10 的编译器及行为测试 | 测试专用 Future 和手动驱动器，无须先实现 std 调度器 |
| 二：库 | std Future/Promise、同步上下文、线程池及事件循环接入；见 §9 | 使用阶段一的同一协议，不再修改语言规则才能运行 |

## 2 双固定槽注册

类型注册统一使用无参数的 `@future`，按目标类型声明的泛参数量选择固定槽：

| 泛参数量 | 注册槽 | 结果映射 |
| --- | --- | --- |
| 0 | 无结果槽 | 不产生返回值 |
| 1 | 有结果槽 | 唯一泛参 `T` 就是结果类型 |
| 2 个及以上 | 不允许注册 | 编译期报错 |

每次分析在源码及实际依赖闭包中为每槽登记至多一个规范化类型声明。源码声明收集、依赖元数据读取时顺带登记，不增加独立的类型遍历；未直接引用的依赖公开模块也参与，不扫描未使用的安装包。

| 输入 | 处理 |
| --- | --- |
| 某槽的首个合法声明 | 保存声明身份、协议成员和诊断来源 |
| 同声明重复导入、别名或菱形依赖 | 幂等 |
| 同槽第二个不同声明；同声明重复标注 | 报错，冲突诊断指向双方来源 |
| 无结果槽与有结果槽各登记一个声明 | 允许，两槽互不竞争 |
| 某槽无注册且未使用对应形态协议 | 允许 |
| 使用某形态协议但对应槽无注册 | 使用处报错 |

登记必须先于协议使用检查，且与文件／导入顺序无关；LSP 每次分析重建。有结果声明的各泛型实例共享该槽，不按 `T` 分别注册。显式返回类型不能绕过同槽注册冲突，仍遵循普通可见性规则。

协议类型为公开模块中的公开、引用语义对象 `type`；不允许 `@value`、`@abi`、tuple 或 spec 作为协议实现主体。唯一泛参的约束及各实例的成员签名按普通泛型规则检查、代入；不从库方法签名推导结果类型，不增加 `@future(void)` 等类型注册参数。

## 3 源码规则

### 3.1 两种结果形态与返回语义

本文用 `Future`、`Future<T>` 分别代称 §2 两槽中的类型；实际名称由库定义。

| async 返回类型 | body 返回及正常落尾 | await 的结果 |
| --- | --- | --- |
| `Future` | 沿用普通无返回值函数规则：允许空 `return;` 或正常落尾 | 无返回值，不能绑定或作为值传递 |
| `Future<T>` | 按 `T` 检查返回值及正常落尾；`T` 须符合现有普通类型和泛型约束 | 得到 `T` |

**已确认采用 C# 的 async 返回语义，保留 Future 嵌套：**

- `return f` 将 f 作为本次 async 的结果值；f 为 Future 时，不隐式等待其完成，也不自动展开。
- `return await f` 先按正常 await 规则等待，再返回取得的结果值；每个 await 只取出一层结果，结果本身仍为 Future 时继续保留嵌套。

上述规则同样适用于泛参代入 Future 的情况。省略返回类型时仍使用 Feng 的推导规则，见 §3.3。

`let f = save();` 可持有无结果 Future；`await f;` 等待其完成。无结果等待后结束函数写作 `await f; return;`；有结果时可用 `return await f;` 返回 `T`。两种 await 都在原词法位置传播失败。

本文不扩展 void 的普通值或泛型实参能力，`Future<void>` 不属于合法形态。[void 类型方案](feng-void-type-dev.md)已暂停，不作为异步实施或验收的前置依赖，std 的 None／none 替代迁移不在本轮范围内。

### 3.2 语法与类型

```feng
// 示例名称由库定义；Future<int> 对应的异步结果类型为 int。
async func twice(): Future<int> {
  let x = await load();
  return x * 2;
}

// 无结果槽中的类型；body 可以空 return 或正常落尾。
async func save(): Future {
  await write();
  return;
}
```

- `async func` 用于有实现体的普通函数／方法；Lambda 写作 `async (参数) -> 表达式` 或 `async (参数) { ... }`，仍须具备现有 callable-form spec 目标类型。泛型 callable 同样适用。
- `async` 在上述前缀位置、`await` 在 async callable 的一元表达式位置作为上下文关键字；`std.async`、`obj.await()` 等成员／模块名称仍合法。async body 内的局部绑定及参数不得命名为 `await`，嵌套 callable 独立判断。
- `await` 与其他前缀一元表达式同级，操作数包含完整的后缀调用／成员链；`await f() + g()` 等于 `(await f()) + g()`。普通函数和顶层使用 await 报错。
- 构造函数、终结器、`main`、`extern func`、顶层 `@abi func` 不接受 async。同步入口通过库驱动异步；普通外部函数可以按其既有 ABI 返回合法值，但不参与状态机生成。
- spec 中只声明返回 F 的普通方法／callable 签名，不标 async；实现可以同步返回 F，也可以使用 async。
- `F` 必须是 §2 无结果槽的类型或有结果槽声明的实例，body 按 §3.1 检查；调用表达式类型为 F。普通函数不自动把结果包装成 F。
- defer 自身不允许 await；嵌套 async Lambda 的 body 独立检查。try/catch、分支、循环、普通表达式均可挂起。

### 3.3 返回类型推导

async 函数／方法允许省略返回类型声明。省略时先按[函数规范](../specifications/feng-function.md)推导 body，再按 §2 确定函数返回类型：

- 有返回值，所有 return 路径的结果类型一致为 T：返回类型为有结果槽的 `Future<T>`，并检查泛参约束。
- 无 return 或仅空 return：返回类型为无结果槽的 `Future`。
- 多条 return 路径的类型不一致，包括带值 return 与空 return 混用：沿用函数规范在编译期报错，不自动推导联合类型。

一致性比较发生在包装 Future 之前：按普通类型规则比较各 return 表达式的静态类型，得到统一的 T 后再推导为 `Future<T>`。返回和等待遵循 §3.1。

以下均省略函数返回类型，`f`、`g` 为 `Future<int>`，`h` 为 `Future<bool>`；合法结果仍须满足泛参约束。

| 各分支的 return | 一致性 | 函数返回类型／诊断 |
| --- | --- | --- |
| `return 1;` / `return 2;` | 一致，均为 int | `Future<int>` |
| `return 1;` / `return await f;` | 一致，均为 int | `Future<int>` |
| `return 1;` / `return f;` | 不一致，int 与 Future<int> | 编译期报错 |
| `return f;` / `return g;` | 一致，均为 Future<int> | `Future<Future<int>>` |
| `return f;` / `return h;` | 不一致，Future<int> 与 Future<bool> | 编译期报错 |
| `return 1;` / `return true;` | 不一致，int 与 bool | 编译期报错 |
| `return;` / `return;` | 一致，均无返回值 | `Future` |
| `return;` / 任意带值 return（含 Future 值） | 不一致，无返回值与有返回值 | 编译期报错 |

直接退出当前 callable 的 throw 路径不提供返回类型。部分分支返回 T、其他分支可正常落尾时，按[函数规范的正常落尾规则](../specifications/feng-function.md#411-非-void-callable-的正常落尾)报错，不因省略声明而放宽检查。

```feng
async func count() { return 1; }  // 推导为 Future<int>
async func save() {}             // 推导为 Future
async func finish() { return; }  // 推导为 Future
```

调用表达式使用推导后的 Future 类型。有显式 F 或 Lambda 目标 F 时，按 §2 确定无结果或结果类型 T，再检查 body；Lambda 仍须满足 §3.2 的目标类型要求。

不反推隐式转换，不补猜测结果类型；只有无目标类型的 `()` 等表达式、无法确定 T 时要求显式返回类型。两槽按 body 结果形态直接选定，缺失注册按 §2 报错，不扫描其他协议声明。

## 4 类型与 await 协议

F 统称两槽中的 Future 类型或实例；仅有结果形态具有结果类型 T。E 为已注册 `@throwable` 根 spec。库不需要声明状态机类型或指针。

| 标记 | 标注目标 | 契约 |
| --- | --- | --- |
| `@future` | 无泛参或单泛参的 Future 类型声明 | 按 §2 登记到两个固定槽，每槽唯一 |
| `@future("await")` | 被等待 Future 的同步实例方法 | 已完成返回 true；未完成则原子复查、登记通知并完成挂起交接，返回 false；不返回结果／异常载荷 |

每个注册声明内的 await 角色恰好一个，随成员收集登记，按成员身份绑定，重复报错，不增加独立遍历。方法名不固定，不要求名为 await。立即完成和挂起恢复均通过 runtime 读取 child 的结果；不引入 AwaitStep／AwaitReady 返回包装，也不向父状态机投递结果。

两个 Future 均用方法泛参 P 接收父 Future，库只定义一次泛型方法，不枚举父 async 函数。编译器在每个 await 调用点，将语义分析确定的父函数返回类型代入 P。源码签名形状如下，省略方法体；普通约束与私有 ABI 的实现细节仍按 §11 Review，接口尚未实施：

```feng
@future("await")
seal func onAwait<P>(waiter: P, ticket: u64): bool;
```

| 项目 | 含义 |
| --- | --- |
| self | 隐式接收者，被等待的 child |
| waiter: P | 父状态机所属 Future；P 是整个父 Future 类型，不是 child 的结果类型 T |
| ticket | 本次等待标识，用于拒绝过期或重复恢复；生成与失效规则待 ABI Review |
| true | child 已完成，包括成功和失败；进入统一取结果位置 |
| false | 已登记通知并完成挂起交接；退出本次推进 |

例如 child 为 `Future<int>`、父函数返回 `Future<string>`，生成调用为 `child.onAwait<Future<string>>(parent, ticket)`；此时类型级 T 为 int，方法级 P 为 `Future<string>`。无结果父函数则代入 `P = Future`；泛型父函数可代入 `P = Future<U>`。这沿用普通泛型表达能力，不需要 `Future<?>`。该调用为编译器内部示例，用户源码仍受 seal 限制。

| 父函数返回类型 | 代入 P | 与具体状态机的关系 |
| --- | --- | --- |
| a、b 均返回 `Future<int>` | 均为 `Future<int>` | a、b 的帧类型可以不同，runtime 经各实例的隐藏槽定位 |
| `Future<string>` | `Future<string>` | 不影响 child 的 T |
| `Future` | `Future` | 父函数无结果，仍可等待有结果 child |
| `Future<U>` | `Future<U>` | U 尚未闭合时沿用普通泛型描述信息 |

P 按普通泛型持有规则保存，并按 §6.2.1 进入 runtime；库无需知道父函数名称或帧类型。库可将捕获 waiter／ticket 的恢复动作存为同一无参 callable 类型；捕获按普通 ARC 规则保活。只保存 P 并传给 runtime 不需要访问 P 的成员；若库要调用 waiter 的通知等普通方法，则给 P 添加相应的普通 spec 约束，不增加编译器协议角色。源码不透明指针本身不能保活 Future，不用它替代 P 的托管所有权。

该方法允许 seal；编译器生成的调用按已绑定成员身份忽略普通可见性限制，该权限不扩展到用户源码。跨包和纯 FT 保留其身份、签名和可见性。隐藏字段不参与源码成员注册、查找或补全。

编译器只识别上述两个标记。start、state、ready、poll、value、resolve、reject、resume 及 new 不是有效注解参数；其他库方法按普通语言规则实现。

### 4.1 创建与首次执行

```text
生成入口：
  按普通顺序求值并保存参数／接收者
  创建 Future 实例 f，执行普通字段初始化
  创建状态机，保存本次调用的输入
  通过普通托管字段赋值，将状态机绑定到 f 的隐藏字段
  直接发起首次推进，执行至实际挂起或完成
  返回 f；临时持有按普通返回及异常路径清理
```

首次推进由编译器生成代码发起，不经过库启动方法，不投递调度队列。`let f = test();` 已经开始执行 test；外层 `await f` 只负责等待，不负责启动。遇到内部 await 时按 §5 调用被等待对象的库入口；已完成依赖同步继续，未完成依赖才实际挂起。没有 await 或全部依赖已完成时，函数可在调用返回前完成并保存结果／异常。

编译器负责实例创建，不依赖特定构造函数，也不跳过普通字段初始化器；库通过普通初始化规则准备等待列表及同步状态。隐藏字段赋值、调用及返回均使用普通托管引用规则，绑定前后的初始化失败按普通异常路径清理。库不手动绑定或释放隐藏字段。

参数求值、Future 初始化和建帧失败沿同步调用传播；body 的未捕获异常由生成边界捕获并存入状态机，包括首次挂起前的异常，不从 async 调用直接抛出。初始化异常与 body 失败必须区分。

async 调用按普通值边界保存参数和接收者：引用类型保活原对象；值类型接收者复制到该次计算的状态中，之后的 self 修改该副本。闭包捕获可变绑定继续共享原绑定单元，不转为注册时快照。此接收者规则是本次新增提案。

## 5 await、wake 与库驱动

等待登记保留等待者 Future、本次等待标识及库调度信息；恢复通知不携带成功值或异常。两种 Future 可相互等待，取结果的形态由 child 决定，不由等待者自身的返回类型决定。

```text
状态机执行到 await child_expr：
  求值一次并保活 child，保存恢复位置及必要的跨挂起数据
  调用 child 上注册的 await 入口
    true → 进入取结果位置
    false → 已完成挂起交接，退出本次推进

取结果位置（后续恢复直接进入这里）：
  通过 runtime 读取 child 的完成结果
    成功 → 有结果时取得 T，无结果时直接继续
    失败 → 抛出保存的异常，进入原 await 的词法 try/catch

库的 await 入口：
  经 runtime 查询 child 的完成状态，统一处理 async 与手工 Promise
  已完成 → 返回 true，不登记通知、不调度
  未完成 → 原子复查、登记等待者和通知，完成挂起交接后返回 false

child 完成后的库流程：
  取出等待者，按捕获的上下文／调度策略安排恢复
  仅将等待者 Future 和等待标识交给 runtime 恢复入口
  runtime 校验等待标识，从上述取结果位置继续执行
  若本次推进完成该等待者，库通知其等待者；再次挂起则等待下一次 wake
```

检查完成、登记通知及提交挂起必须衔接，不丢失完成事件；每次等待至多恢复一次。交接前保存恢复点并保活 child，交接后旧推进不得再读写业务帧。登记过程中同步到达的 wake 由库协调，不递归推进仍为 Running 的帧；只有完成交接才允许恢复。该交接同时覆盖编译器首次推进和库后续推进，精确同步机制按 §11 定稿。推进、通知及用户代码在库同步临界区外执行。

状态机完成时保存 §6.3 的终态，runtime 查询／读取不消耗结果，支持多个等待者和重复 await。后续推进由库发起，库在获知完成后通知等待者；首次同步完成无需库启动或完成回调，之后的 await 入口直接读到终态。若首次推进期间已有等待登记，挂起／完成交接也必须保证其通知不丢失。

库负责 wake、队列及调度；恢复后不重新求值 child_expr，也不再次调用 await 入口。已完成路径与恢复路径共用 §6.3 的 runtime 结果读取，失败在原 await 位置抛出；不能用调度回调中的抛异常代替，也不跨挂起保留原生展开记录。

手工 Promise 的 Future 关联 runtime 可读取的完成状态对象，遵循同一等待和取结果流程；不需要执行状态机。不能以隐藏槽是否有值判断 Future 是否完成。编译器只调用已注册的 await 入口及 runtime API，不读取库业务字段或按约定名称调用其他方法。

库的等待通知及队列任务强持有等待者 Future，直到执行或移除；父状态机保活 child 等源码数据，不保存所属 Future 或其驱动回调。当前等待者 Future 可由推进调用上下文借入并交给库登记，不形成帧到所属 Future 的隐式反向持有。该通知持有策略是待 Review 提案，对象图可能成环，须按 §6 回收。丢弃一个外部引用不等于取消。

## 6 状态机与 runtime 私有 API

### 6.1 表示、持有与清理

编译器在普通字段布局和生命周期分析之前，为两槽中各注册 Future 生成一个源码不可见的、**类型擦除的托管引用槽**，并生成遵循普通托管引用规则的字段赋值。该槽不固定具体对象类型，也不是源码裸指针字段；库不声明或访问它，不为此引入内部 State spec。

每个 async 函数／Lambda 定义生成自己的具体状态机类型，保存所需的跨挂起绑定；同一定义的每次调用创建独立实例，泛型按现有泛型机制处理。协议允许隐藏槽关联两类对象：编译器生成的执行状态机，或经 runtime 创建／关联的手工完成状态对象。后者只保存完成状态及结果／异常，不含执行位置或推进入口。两类对象均保留普通对象头、具体描述符及清理信息；同一 Future<int> 的布局不随关联对象变化，runtime 不把任意托管对象或手工完成状态对象当作执行帧推进。

编译器须明确表达该槽的托管所有权，并接入普通字段处理：赋值维护持有，描述符枚举引用，字段释放函数释放持有，强引用图包含 `Future → 状态帧／完成状态对象`。不能因具体目标类型被擦除而遗漏强边或判为无引用；无法证明无环时遵循既有保守分析规则。接入后复用现有 ARC 和循环回收，不为 Future 另写计数、trace 或释放算法；不能假定加入一个无具体类型的槽就已自动完成接入。库终结器不重复释放隐藏字段，两类状态对象均不反向持有所属 Future。

runtime 的对象释放路径不判断对象是否为 Future。ARC 归零、终结及复活检查、字段释放、循环回收统一遵循[生命周期规范](../specifications/feng-lifetime.md)。仅 runtime 状态操作 API 根据描述符中的定位信息访问隐藏字段；其布局定位契约见 §6.2。

所有注册 Future 实例均具有同一规则生成的隐藏字段，包括普通库代码创建的实例；布局不能取决于当前模块是否含 async body。手工 Promise 经 runtime 关联完成状态对象后进入等待协议；不创建空执行帧，也不只把结果保存在 runtime 无法读取的库字段中。未关联对象的状态须与手工 Promise 的未完成状态区分，非法调用的诊断按 §11 定稿。

状态帧使用现有托管对象头及描述符，包含 pc、执行状态、跨挂起值、有效清理信息和最终结果／异常存储。尚未初始化／已释放的值槽采用编译器生成的普通 `Empty | V` 具名 union，默认 Empty；V 为该槽的值类型，在源码求值位置才构造，避免建帧时提前执行默认初始化。该 tag 同时决定槽是否参与 trace／release，不保存未定类型值。最终结果的有无载荷及成功／失败标记见 §6.3。

帧内引用在保存时持有或转移所有权，挂起不释放仍然活跃的槽；覆盖、作用域退出和关闭时按有效状态清理。帧的生成终结路径执行尚未执行的清理，循环路径同样适用，不能只依赖不会在该路径执行的 release-children。每个清理先失效再调用，保持 defer／局部释放的交错逆序。普通生命周期规则引用[生命周期规范](../specifications/feng-lifetime.md)，不新增另一套计数或循环回收器。

帧关闭幂等；正常完成已经清理过的作用域不再执行，已保存的终态及结果保持可重复读取，直到其持有结束。未完成帧关闭后不能恢复业务代码；即使清理导致对象复活，帧也保持 Closed，库按关闭态处理，不能重新运行。尚未初始化的槽不释放。持有类型参数的帧保存普通泛型描述符，不改变泛型 ABI。

### 6.2 runtime 操作边界

runtime API 接收任一注册形态的 Future，通过其描述符中的定位信息取得隐藏字段，再按关联对象的能力操作；不按字段名查找，不假定固定偏移。runtime 不自行分派库方法；执行状态机时到达 await 的库调用由编译器生成并绑定。定位元数据不替代普通托管字段元数据。下表定义已确认方案所需能力；§6.2.1 给出等待／恢复／读取的接口提案，手工对象创建／发布及关闭等其余签名与错误行为按 §11 定稿。

| 能力 | 契约 |
| --- | --- |
| 首次推进 | 由编译器生成入口发起；从 Ready 执行至实际挂起或完成，返回推进状态；完成结果存入帧 |
| 挂起交接 | 与库等待登记衔接，提交恢复点并交出推进权；不保存等待列表、发送通知或调度 |
| 恢复并推进 | 由库传入等待者 Future 和等待标识；校验当前等待及执行状态，进入原 await 的取结果位置，继续至实际挂起或完成；不接收成功值或异常载荷 |
| 查询 | 识别关联对象并查询完成／关闭状态，未关联对象单独表示；运行／等待状态只适用于执行帧，未完成的手工 Promise 另以 Pending 表示；不执行用户代码 |
| 结果读取 | 对两类对象统一按 §6.3 读取成功完成或结果 T；失败时抛出保存的 E；不消耗结果，不阻塞等待，未完成不可读取，读取与发布须正确同步 |
| 手工完成 | 创建并关联手工完成状态对象；按 Future 的结果形态发布无结果成功、T 或 E，终态只发布一次；发布后由库通知等待者 |
| 关闭清理 | 与推进互斥；未完成帧先置 Closed 再执行有效清理，重复关闭无操作；已完成帧保留结果，不能重新执行业务代码 |

执行帧推进过程中为 Running，完成挂起交接后为 Waiting，保存成功／失败结果后为 Done；Done／Closed 不允许重新执行，已发布终态不得覆盖。手工完成状态对象不参与推进。runtime 调用期间须保证传入 Future 及所关联对象有效，按既有托管调用边界保活；当前 Future 仅作为本次推进上下文传给生成代码，不保存为帧的反向引用。关闭不等于释放所有引用，不要求库调用 async 专用 retain/release API；关闭失败的结果发布顺序见 §11。

库负责后续推进、恢复及关闭的串行化，并通过 §5 的交接协调首次推进；查询／结果读取与完成发布必须具有一致的同步契约。runtime 不调度、不阻塞等待、不创建线程，不擅自增加 Future 锁。首次执行、库同步与状态发布之间的准确握手、重入检测及错误处理在 ABI Review 中确定。

生成的状态机入口与 runtime 共享推进／清理／结果访问约定；手工完成状态对象复用结果访问契约。隐藏字段定位信息由编译器提供，await 入口生成按成员身份绑定的实例调用。runtime 不持有库方法分派表，不管理 Future 等待列表、完成通知或调度。

#### 6.2.1 泛型 P 的 Feng／C 接口提案

复用现有 `@runtime` 泛型 ABI：类型描述符按泛参声明顺序置于 C 参数前部；裸泛参值按存储地址传入；裸泛参返回值写入最后的 out 参数。既有依据见 [runtime contract](../../src/runtime/feng_runtime_contract.inc) 的 `feng_expression_equal`／`__test_value_identity` 及[发码](../../src/codegen/codegen.c)。下列 async 符号、状态编码及 Future 定位元数据是提案，尚未实现。

```feng
enum AsyncState {
  Unbound, Ready, Running, Waiting, Pending, Succeeded, Failed, Closed
}
enum AsyncAdvance { Ignored, Suspended, Succeeded, Failed }

@runtime
extern func feng_async_state<P>(future: P): AsyncState;
@runtime
extern func feng_async_suspend<P>(future: P, ticket: u64): void;
@runtime
extern func feng_async_resume<P>(future: P, ticket: u64): AsyncAdvance;
@runtime
extern func feng_async_result<P, R>(future: P): R;
@runtime
extern func feng_async_result_void<P>(future: P): void;
```

AsyncState 中 Unbound 为未关联对象，Ready 为尚未首次推进的帧，Pending 为尚未完成的手工对象；其余按 §6.2。AsyncAdvance 的 Ignored 表示等待已失效且未推进；Suspended 表示本次推进已交接，不是帧当前状态的实时快照；Succeeded／Failed 表示本次推进发布了对应终态，由库通知等待者。两个 enum 按声明顺序从 0 编码，C 返回类型均为 `int32_t`；编码最终须 Review。

对应的单一 C 入口如下，不为每个 P 生成一个 runtime 实现：

```c
int32_t feng_async_state(
    const FengGenericParamDescriptor *p, const void *future_value);
void feng_async_suspend(
    const FengGenericParamDescriptor *p, const void *future_value,
    uint64_t ticket);
int32_t feng_async_resume(
    const FengGenericParamDescriptor *p, const void *future_value,
    uint64_t ticket);
void feng_async_result(
    const FengGenericParamDescriptor *p,
    const FengGenericParamDescriptor *r,
    const void *future_value, void *out);
void feng_async_result_void(
    const FengGenericParamDescriptor *p, const void *future_value);
```

| C 参数 | 含义 |
| --- | --- |
| p | 整个 P 的泛型描述符；本协议的 P 是注册 Future，类别为 `FENG_VALUE_MANAGED_POINTER`，descriptor 指向相应 Future 类型描述符 |
| future_value | P 值的存储地址；存储中装着 Future 对象指针，不是 Future 对象地址或状态机地址本身 |
| ticket | 原等待标识；Feng u64 对应 C `uint64_t` |
| r／out | R 的描述符及调用方提供的未初始化结果存储；大小、对齐和普通复制／持有规则均按 R |

result 只用于有结果 Future，R 必须与该 Future 的结果类型一致；result_void 只用于无结果 Future。两者均不等待，只读取已发布的成功／失败终态。suspend 只用于当前推进的挂起交接，resume 只消费匹配的等待标识；非法对象、状态或结果类型的错误行为仍须 Review。

“任意 P”指任一注册 Future 形态及其泛型实例，不是任意普通 Feng 类型。runtime 须核对 P 的类别和 Future 定位元数据；不能仅凭 managed-pointer 类别接受 string 等其他对象。通过 p 定位 Future 隐藏槽，再按槽中对象自身的描述信息选择执行帧或完成对象操作，不从裸地址猜测类型。

以下是两个独立调用片段：恢复用于库收到通知之后；取结果用于 child 已完成的位置。示例使用 i32，避免平台 int 宽度差异：

```feng
// parent: Future<string>；child: Future<i32>。
let advance = feng_async_resume<Future<string>>(parent, ticket);
let count = feng_async_result<Future<i32>, i32>(child);
```

对应 C 调用示意；desc_* 代表编译器提供的 `FengGenericParamDescriptor` 常量，变量／对象符号名仅作说明：

```c
void *parent_value = parent_object;
int32_t advance = feng_async_resume(
    &desc_future_string, &parent_value, ticket);

void *child_value = child_object;
int32_t count;
feng_async_result(&desc_future_i32, &desc_i32, &child_value, &count);
```

在库的泛型共享体中，P 已经采用存储地址表示，直接转发即可，不再对该地址取地址：

```feng
func drive<P>(future: P, ticket: u64): AsyncAdvance {
  return feng_async_resume<P>(future, ticket);
}
```

```c
/* 仅展示共享体内部调用；_P 为已有 P 描述符，future_value 为 P 存储地址。 */
int32_t advance = feng_async_resume(_P, future_value, ticket);
```

描述符校验后，runtime 可从值存储读取对象指针，例如 `memcpy(&future_object, future_value, sizeof future_object)`，其中 future_object 为 `void *`；随后才访问对象头及隐藏槽。不能把 future_value 直接强转成 Future 对象，也不能把对象地址冒充 P 存储地址传入。

这些输入均为调用期借用；runtime 不保存 future_value 这个可能指向调用栈的地址。等待记录／闭包按 §5 保活实际 Future，不新增泛型参数盒。result 成功时在 out 中建立普通拥有的 R 值，失败时不建立结果值并抛出保存的 E；原终态不被消耗。R 在共享体中尚未闭合时，发码须按描述符提供尺寸及对齐正确的结果槽，不能用一个指针变量充当任意 R 的缓冲区；该路径须专项验收。无结果读取没有 out 参数，不实例化 `R = void`。

### 6.3 完成结果与异常读取

等待登记须保活依赖及等待者，并保留本次等待标识；通知只请求恢复，不携带结果或异常，也不设置 await 结果输入槽。已完成路径和恢复路径共用 runtime 结果读取：无结果 Future 成功时正常返回，不构造 void 值；有结果 Future 成功时返回 T；失败均抛出保存的 E。两类状态对象的最终结果均有独立的成功／失败标记，不以 `T | E` 去重后的分支或有无载荷代替完成状态。

运行时不得从裸载荷指针猜类型。编译器提供 async 结果存储／读取及类型适配信息；手工完成状态对象按对应 Future 的结果形态接入同一契约。结果／异常按普通复制、持有与释放规则保存和读取；E 是普通异常根值，不是 `_Unwind_Exception` 或 TLS catch 指针。保存的结果及其嵌套引用参与普通 trace／release。载荷表示、初始化、跨泛型实例访问及读取期间的保活须在 §11 定稿。

挂起保留逻辑作用域，只拆除本次物理异常框架；读取失败在原 await 的词法位置抛出。async 结束时先保活待返回值／异常，完成源码作用域清理，再发布状态帧的成功／失败终态；清理局部值不能提前释放最终结果。结果保留供重复读取，状态对象最终回收时按普通字段规则释放。库负责等待者通知；async 结果仍保存在帧中，不额外创建完成状态对象或库结果副本。

## 7 lowering、异常与工具链

### 7.1 转换顺序

```text
Parser → 源码 Semantic → async lowering → 生成节点语义／生命周期事实 → Codegen
```

1. 先将含 await 的表达式拆为有序求值和控制流，仅为有值结果建立临时绑定；保留短路、分支、循环及求值次数。
2. 对正常、异常、清理边求活跃信息；跨挂起值进入帧，闭包共享单元及已取地址存储保持身份。原始指针借用仍按互操作规范由调用方保活 owner，不新增隐式安全承诺。
3. 生成普通内部类型、创建／绑定入口、advance／close 函数及 pc 分派；生成声明不进入用户可见符号空间。Future 的隐藏字段按 §6.1 在布局和生命周期分析前补齐；挂起只退出物理推进，不退出源码逻辑作用域。
4. 重建受影响的符号、成员绑定、泛参替换、异常和所有权旁表；不复用指向已替换节点的语义引用。源码 AST 保留给 LSP 和诊断，生成节点关联原源码位置。

生成代码在原 await 位置调用库入口；返回 true 时直接读取 runtime 结果，不递归推进当前帧，也不设置调度任务。登记等待前保存 child、指向取结果位置的 pc 和必要数据；完成挂起交接后退出，恢复直接取结果，不重新调用库等待入口。close 只执行有效清理，不运行 await 后续业务代码，不清除已发布的结果。共享绑定和需要稳定地址的槽不参与字段复用；其他字段复用属于正确性验收后的优化。

### 7.2 异常衔接

- body 失败保存为状态机中的普通 E；await 的立即完成和恢复路径均由 runtime 结果读取抛出保存的异常，在原 await 的词法位置捕获。调度回调不能成为业务异常接收者，也不能用回调中的裸 rethrow 替代原位置抛出。
- catch 内允许 await。挂起时将仍活跃的异常载荷及其类型身份保存为受持有的值，退出本次 TLS catch／cleanup 框架；恢复后重建逻辑 catch 关系。`throw;` 保留原载荷身份，原生展开记录不跨挂起保存；内层 catch 不覆盖外层保存值。
- `f()` 的摘要包含参数求值、Future 字段初始化及建帧的同步逃逸；body 的失败由生成边界捕获。await 合并已注册库入口的普通异常摘要及 runtime 结果读取的异常效果，无法证明具体异常类型时保留 unknown；登记后、提交挂起前发生异常须撤销该次登记，不能再恢复已走异常分支的等待者；交接后旧调用只能收尾返回，不再抛出登记异常。阶段一不增加跨任意 Future 值的精确失败来源追踪系统。
- [defer 主规范](../specifications/feng-defer.md#6-与异常模型的关系)已经禁止异常逃逸，原规则不变。当前 `feng_throw` 在 finalizer 中无条件 panic，尚不满足[生命周期规范 §13.2](../specifications/feng-lifetime.md#132-终结器执行流程)允许内部捕获的要求。
- 阶段一必须统一修正终结器异常边界：建立边界保护框架，允许边界内 catch；无匹配内部处理器时在越过边界前终止，不能被外部 catch 接管。throw 与 rethrow 使用同一边界规则，不按“是否 async”分支。此项是已有语义的 runtime 配套修正，须单独验证。

### 7.3 元数据与诊断

FT 保存 async 源声明标记、各注册 Future 的声明身份和泛参数量、各自 await 实例方法的身份／签名／可见性，以及隐藏槽的类型擦除／托管所有权事实、两类状态对象的统一结果访问和 runtime 定位契约所需的元数据。导入时按 §2 恢复双槽及结果映射；源码、纯 FT 和缓存对同一实例生成一致布局，不重复添加隐藏字段。seal await 入口可供编译器内部绑定，隐藏字段不进入源码可见面。按既有版本／缓存失效规则更新格式。LSP Hover/SignatureHelp 展示调用返回 F，await 按形态展示无返回值或 T；生成函数及隐藏字段不成为用户补全结果。

新增诊断覆盖：类型／await 标记的目标／参数／签名错误、非法泛参数量、同槽重复注册、缺失对应槽、非法 async/await 位置、返回结果形态不匹配、推导不唯一、无结果 await 被用作值、defer 挂起和源码越权访问 seal 成员。编号按现有诊断主表分配；避免在类型解析失败后追加依赖性协议错误。创建、绑定和首次执行由编译器保证；库的原子等待登记、wake 及通知正确性属于执行契约，不能声称仅凭签名检查即可证明。

## 8 第一阶段实施顺序与成本

本阶段以现有类型与泛型能力为基础，按 §2—§3 接入两种 Future 形态。

| 顺序 | 工作及完成条件 |
| --- | --- |
| A | 闭合 §11 的 await 签名、挂起交接及结果 ABI，Review 批准实施细节；将源码行为收敛到相关主规范，旧 async 方案标为历史方案 |
| B | Lexer/Parser、双槽与类型／await 标记登记、结果映射及返回检查、隐藏字段及 FT／缓存布局一致性；非法程序在源码语义阶段诊断 |
| C | 独立 lowering、Future 创建／初始化／绑定及首次推进、普通字段生命周期接入、帧内结果保存及统一读取、手工完成状态对象的创建／关联／发布、await 调用及恢复后取结果、终结器边界；直接完成、挂起恢复、挂起关闭均可运行 |
| D | 接入两种测试 Future 的 seal await 入口、手工 Promise 及手动 wake／驱动，完成语言服务和 §10 验收；仅随后进入 std 阶段 |

成本目标：每次 async 调用一个 Future 和一个状态帧；最终结果保存在帧中，不要求额外结果盒或库字段副本，也不先创建手工完成状态对象再替换为帧。手工 Promise 的完成状态对象及通知记录成本单独测量。有捕获的通知闭包按现有规则分配，不声称每次 await 零分配。库对已完成依赖不登记通知、不入队；不跨挂起的局部不提升；defer 使用静态槽／标志，不分配注册节点。类型与 await 入口绑定均在编译期；首次推进没有库启动调用。非 async 函数不增加状态机或驱动控制，但其创建的注册 Future 同样包含隐藏字段。库承担等待／后续驱动同步成本；可能成环的状态对象及 Future 须保持正确的循环回收行为。

await 入口调用、挂起交接、结果读取及恢复的边界调用、载荷复制和类型适配成本必须测量。完成后帧因结果仍被持有的内存成本也须记录；已结束的局部值应及时释放。ABI 定稿须评估已完成 await 的路径，未经 Review 不引入额外装箱或分配。

阶段一记录同步完成、一次／多次挂起的分配数、retain/release、耗时及峰值内存，与等价手工协议实现对照。不得为性能省略清理或所有权；额外装箱、额外运行时全局表或通用同步开销需另行 Review。

## 9 第二阶段：std 与同步上下文

std 实现 §2 两槽的 Future 声明及各自的 await 入口，以普通字段初始化器建立等待列表及同步状态。async Future 关联执行帧，手工 Promise 经 runtime 关联完成状态对象；两者的终态、成功载荷和异常统一经 runtime 读取。库不声明状态机字段，也不维护供 await 单独读取的第二份结果。

库按 §5 检查完成并登记等待；wake 可以投递线程池、事件循环队列或同步上下文。恢复任务仅携带等待者 Future 和等待标识调用 runtime；本次推进完成后，库取走该 Future 的等待列表并在锁外通知。手工 Promise 经 runtime 发布终态后，由库执行同一通知流程；首次同步完成由后来进入的 await 直接观察。内部方法名及组织方式由库决定，不增加编译器标记。库必须协调完成观察、等待登记及挂起交接，完成通知不能向调度器逃逸业务异常。

库的内部关闭请求先阻止新推进；若正在推进，只登记请求，由驱动者在本次推进返回后处理，不等待自己。无在途推进后将 Future 传给 runtime，关闭其关联状态机；已完成结果不变。原来未完成的 Future 拟仍由库构造 `Error("async/closed")`，但必须保存为 runtime 可读取的失败结果；其发布与关闭标记、清理的先后及互斥关系待 §11 Review，不能只保存库字段供 await 绕路读取。该关闭步骤用于生命周期协调，不新增公开取消 API。

若清理后对象复活，Closed 帧不能重新挂起或执行业务代码；关闭失败的可读性按上述 Review 项收敛。手工 Promise 关联完成状态对象，虽无执行帧，仍须与未关联对象及 Closed 区分。不可达对象回收只释放通知，不投递新任务。runtime 关闭操作本身不构造这种库错误。

库接口提案按结果形态区分：

| 形态 | Promise 的 Future | 成功完成入口 | Future 的完成结果读取 |
| --- | --- | --- | --- |
| 无结果 | `Promise.future: Future` | `resolve(): void` | `result(): void` |
| 有结果 | `Promise<T>.future: Future<T>` | `resolve(value: T): void` | `result(): T` |

两种 Promise 均提供 `reject(error: E): void` 且不能重复完成；resolve／reject 经 runtime 发布完成状态后由库通知。两种 Future 均提供 `isReady(): bool`；`result` 封装统一 runtime 读取，只处理已完成状态，失败时抛出保存的异常，无结果形态正常返回时不产生值。这些名称不构成编译器协议。普通函数需要等待时使用上下文的库驱动入口，不把阻塞等待放入 Future。

同步上下文的最小契约如下，类型名不是编译器协议的一部分：

```feng
// 接口示意；W 是库定义的无参数、返回 void 的 callable-form spec。
open spec SynchronizationContext {
  func post(work: W): void;
}
```

库提供当前线程上下文的读取及作用域安装。实际登记等待时捕获当前上下文；完成后通过该上下文 post 恢复任务，没有安装上下文时使用线程池上下文。已完成 await 不 post。执行任务时安装该上下文，退出恢复原上下文；上下文存取和恢复全部在库中。

post 不内联执行 work，必须保证接受的任务最终执行；停止上下文前先排空任务，不静默丢弃。线程池实现不保证同一线程；事件循环实现固定在线程内执行。任务强持有目标 Future，恢复前检查关闭状态；同一 Future 即使收到同步通知也由库串行推进。跨线程共享数据的同步责任不因 async 改变。

不把取消、超时、异步流或异步析构追加为本轮语言特性；后续库扩展不得改变上述状态机契约。

## 10 验收矩阵

| 层次 | 必须验证 |
| --- | --- |
| Lexer/Parser/Semantic | 上下文关键字与现有成员名；合法 callable 形式与非法位置；§3.1 返回语义与 §3.3 推导表，覆盖显式返回目标、泛参代入 Future、分支一致性、无 return、throw／正常落尾及调用表达式类型；有结果泛参约束；0／1 泛参分别注册、同槽冲突、缺失对应槽、重复导入幂等、重复标注及多泛参报错；各声明恰好一个 await 入口，按 §11 定稿签名验证；无结果 await 用作值报错，void 既有禁止项不变；非法注解参数；同包、跨包、菱形依赖与重新分析 |
| lowering／Codegen | 操作数只求值一次、短路、多 await 的嵌套表达式；立即完成和恢复共用 runtime 取结果位置，恢复不重新调用 await 入口，不生成结果投递槽；循环 break/continue；泛型、值类型接收者、闭包共享绑定、稳定地址；隐藏槽在普通布局／生命周期分析前生成，类型擦除不丢失所有权或强引用边，普通赋值、描述符及字段释放处理覆盖该槽；生成节点类型及旁表完整 |
| FCTS | 两槽形态相互等待及不同 T 之间等待；无结果 body 空 return／正常落尾、有结果 body 返回 T；按 §3.1 验证嵌套返回、显式等待及单层取值，覆盖内层未完成／失败；初始化、建帧、绑定及编译器首次推进的顺序；不 await 调用结果也执行；无 await／全已完成依赖时同步保存终态；不同帧共用同一 Future 类型的布局；库入口只用于等待且 bool 不区分成功／失败；手工 Promise 通过完成状态对象统一取结果；多个等待者及重复读取；首次／后续推进中的登记、wake、挂起交错 |
| 异常／资源 | Future 初始化／建帧异常与首次 body 失败分开处理；绑定前后异常清理；立即失败和异步失败均在原 await 位置捕获；登记后异常不遗留恢复；catch 内挂起后重抛；祖先块 defer、部分初始化；最终结果不随局部清理提前释放；正常／异常／关闭各清理一次；finalizer 内捕获及越界终止 |
| runtime／内存 | API 经两种 Future 正确定位执行帧或完成状态对象；闭合 P 与共享体 P 的地址层级、描述符转发及非法 P／R；闭合与未闭合 R 的输出槽尺寸、对齐及持有；无结果成功没有载荷，有结果成功按 T 保存，失败均保存 E；手工对象创建／关联、只完成一次及重复读取；未关联对象、手工完成对象与 Closed 区分；非法推进、重复关闭、读取结果形态不匹配及过期／重复通知；隐藏槽、两类状态对象及最终结果参与普通 ARC 和循环回收；结果保留期间的内存；复活后不恢复业务；队列保活无悬空引用；无所属 Future 反向持有；挂起交接排除并发和重入 |
| 包／工具 | 源码、纯 FT、workspace cache 恢复双槽及结果映射；两种 Future 的隐藏字段／布局／定位／结果访问元数据一致且只生成一次；无 async body 的模块同样保留布局；跨包 seal await 入口可供生成代码调用，用户源码不可越权；隐藏字段不可访问或补全；泛型 provider/consumer；原始源码诊断与 Hover |
| std | 无结果／有结果 Promise 经 runtime 发布终态、由库通知；线程池／事件循环上下文；上下文恢复；停止时排空；完成／关闭并发；实际异步操作接入 |

阶段一新增隔离依赖的测试协议及异常根，不同时加载 std 的协议声明，也不修改既有“未实现”断言；std 接入后这些测试仍能独立运行。阶段二替换占位实现时单独批准旧测试迁移。`test/` 检查诊断、AST、生成代码；`fcts/` 检查语言行为。两个阶段均按仓库要求在沙箱外执行 `make test`，二进制在 build/ 或 temp/ 中运行；按目标矩阵记录实际验证平台，已有后端缺口与未运行项单列。

## 11 Review 要点与参考

已确认：§2 的双固定槽及结果映射、§3.1 的 C# 式返回语义；仅保留类型与 await 标记，各 await 入口允许 seal。编译器创建、初始化、绑定并首次执行；await 仅接入库等待，通知后直接恢复到 runtime 取结果位置，不传入结果、不重复调用 await 入口。手工 Promise 同样使用 runtime 可读取的完成状态对象。隐藏槽继续采用类型擦除托管引用，复用普通 ARC；等待、通知和调度全部在库层。具体职责见 §4—§6。

以下实现细节仍须在实施前 Review：

| 优先级 | 待闭合项 |
| --- | --- |
| P0 | §4 的方法泛参 P 所允许的普通 spec 约束及签名校验；覆盖无结果、有结果及泛型父函数，明确等待标识的生成／失效与错误处理。bool 只区分已完成与已挂起，不携带结果／异常 |
| P0 | 首次执行与库后续驱动的挂起／完成握手：登记期间的同步 wake、提交挂起前的完成、已有等待者的终态通知、异常撤销及同步可见性；不得漏通知或重入运行中的帧 |
| P0 | 执行帧及手工完成状态对象的统一结果 ABI：无结果成功、有结果成功及失败的布局、初始化、结果形态验证、只完成一次、重复读取、持有及最终释放；立即完成与恢复共用结果读取，不保留原生异常记录 |
| P0 | 类型擦除托管槽及两类状态对象的能力／定位元数据、普通生命周期接入和跨 FT 布局契约；§6.2.1 的接口／状态编码提案及其余 runtime 签名，包括手工完成对象创建／关联／发布、未关联对象及非法调用处理。共享体 R 的结果暂存、具体布局、边界调用及快速完成路径成本须 Review |
| P0 | 关闭失败如何成为 runtime 可读取的终态：库构造错误与关闭标记、清理及结果发布的先后／互斥关系；已完成结果不覆盖，复活后不可重新执行 |
| P1 | 值接收者复制、通知强持有父 Future、defer 禁止挂起及通用终结器异常边界修正、按等待点捕获同步上下文等保留提案 |

本文采用类型／await 两项标记，首次执行、结果存储及库接入方向已确定；上述 P0 是具体接口与并发交接的未闭合项，未定稿前不宣称具备完整可实施 ABI。不得用旧成员角色、隐式库方法调用或 runtime 全局驱动注册填补缺口。

参考只用于核对行为，不要求复制其他语言的类型布局：

- [C# async 函数及 task builder](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/classes#1514-async-functions)：调用时启动、结果／失败完成及编译器与库的协作。
- [C# return 语句](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/statements/jump-statements#the-return-statement)：用于核对 §3.1 的结果值检查与嵌套保留；返回类型推导按 §3.3。
- [C# await 求值](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/expressions#1299-await-expressions)：用于核对完成检查、通知与结果读取的行为；本文通过一个 await 库入口衔接，async 结果保存在状态机中。
- [.NET SynchronizationContext / ConfigureAwait](https://devblogs.microsoft.com/dotnet/configureawait-faq/)：恢复位置由库安排；本文选择统一异步 post，未照搬所有内联策略。
- [Feng 异常规范](../specifications/feng-exception.md)、[defer 规范](../specifications/feng-defer.md)、[生命周期规范](../specifications/feng-lifetime.md)、[tuple 规范](../specifications/feng-tuple.md)：沿用其普通规则。

旧 [awaitable 草案 2](feng-async-awaitable-draft-2.md)、[自定义注解草案](feng-custom-annotation-draft-1.md)和[std Future 规范](../specifications/feng-std-future.md)本轮不修改；Review 批准后的状态调整按 §8 执行，不同时维护两套异步定义。
