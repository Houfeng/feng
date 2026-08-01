# Feng 标准库 Future 规范

本文档定义标准库 `std.async` 中的 `Future<T>`、`Promise<T>` 与 `Scheduler` 类型，用于调度机制无关的异步结果传递。

## 1 职责

- 提供调度机制无关的异步结果传递：生产者通过 `Promise<T>` 发布结果，消费者通过 `Future<T>` 获取结果。
- `Promise<T>` 是生产者端，拥有 `resolve`、`reject` 和 `future` 方法，只能 resolve 或 reject 一次。
- `Future<T>` 是消费者端，提供等待获取结果和非阻塞状态查询能力，不包含任何生产方法。
- Future/Promise 是纯状态机 + waker 通知协议，不包含任何等待原语。等待机制由 `Scheduler` 提供。
- `Scheduler` 是调度器抽象接口，提供 `awaitFuture` 方法，不同调度器实现不同的等待策略。

### 关于状态保护同步的说明

`Promise.resolve()`/`reject()` 可从任意线程调用，需要与 `Future.addWaker()`（由调度器调用）之间做并发保护。此同步当前由 SharedState 内部的 Mutex 提供。未来可通过可注入的 Sync 协议将同步机制也交由调度器决定。

## 2 公开 API

这些符号定义在 `std.async` 模块中。使用方通过 `import std.async;` 后使用。

### 2.1 `Promise<T>`

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Promise` | `open type Promise<T>` | 异步结果的生产者端，泛型参数 `T` 为结果值类型 |
| `Promise.Promise` | `open func Promise()` | 创建一个 pending 状态的 Promise，同时创建关联的 Future |
| `Promise.resolve` | `open func resolve(value: T): void` | 将 Promise 标记为已解决并发布结果值 |
| `Promise.reject` | `open func reject(error: string): void` | 将 Promise 标记为已拒绝并发布错误信息 |
| `Promise.future` | `open let future: Future<T>` | 关联的 Future 实例，在 Promise 构造时一并创建，共享底层状态 |

### 2.2 `Future<T>`

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Future` | `open type Future<T>` | 异步结果的消费者端，泛型参数 `T` 为结果值类型 |
| `Future.get` | `open func get(): T` | 委托给当前调度器的 `awaitFuture` 等待结果；已解决时返回值，已拒绝时抛出错误 |
| `Future.isReady` | `open func isReady(): bool` | 非阻塞查询是否已完成（已解决或已拒绝） |

### 2.3 调度器集成（内部协议）

以下 API 供 `Scheduler` 实现使用，普通用户代码不应直接调用。

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `WakerFn` | `open spec WakerFn(): void` | 唤醒回调的可调用形状，调度器创建并注册到 Future |
| `Future.addWaker` | `func addWaker(waker: WakerFn): void` | 注册唤醒回调；若 Future 已 settled，立即调用 waker |
| `Future.result` | `func result(): T` | 已 settled 时返回值或抛出错误；pending 时行为未定义 |

### 2.4 `Scheduler`

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Scheduler` | `open spec Scheduler { ... }` | 调度器抽象接口，定义等待策略 |
| `Scheduler.awaitFuture` | `func awaitFuture<T>(future: Future<T>): T` | 等待 Future 完成并返回结果，具体等待方式由实现决定 |

## 3 语义

### 3.1 状态模型

Promise 与 Future 共享同一底层状态，状态有三种取值：

- **pending**：初始状态，尚未 resolve 或 reject。
- **resolved**：已解决，携带结果值 `T`。
- **rejected**：已拒绝，携带错误信息 `string`。

resolved 和 rejected 统称为 **settled**（已终结）。状态一旦进入 settled 即不可变更。

### 3.2 `Promise.resolve`

- 将 pending 状态的 Promise 转为 resolved，存储 `value`，并调用所有已注册的 waker。
- 若 Promise 已处于 settled 状态，抛出 `"promise/already-settled"`。

### 3.3 `Promise.reject`

- 将 pending 状态的 Promise 转为 rejected，存储 `error`，并调用所有已注册的 waker。
- 若 Promise 已处于 settled 状态，抛出 `"promise/already-settled"`。

### 3.4 `Promise.future`

- 在 Promise 构造时一并创建的关联 Future 实例。
- 与 Promise 共享同一底层状态。
- 需要多个 Future 引用时，直接传递同一 Future 实例即可。

### 3.5 `Future.get`

- 委托给当前调度器的 `awaitFuture` 方法。
- 原型阶段使用 `ThreadScheduler` 作为默认调度器。
- 若 Future 已 resolved，返回结果值。
- 若 Future 已 rejected，抛出关联的错误信息。

### 3.6 `Future.isReady`

- 返回 `true` 当且仅当关联的 Promise 已 resolved 或已 rejected。
- 不阻塞。

### 3.7 `Future.addWaker`（内部协议）

- 将 waker 注册到 Future 的等待列表中。
- 若注册时 Future 已 settled，立即调用该 waker（确保 waker 不会遗漏通知）。
- Promise settle 时，遍历并调用所有已注册的 waker，然后清空等待列表。
- `addWaker` 与 `resolve`/`reject` 之间通过 SharedState 内部 Mutex 保证线程安全。

### 3.8 `Scheduler` 语义

- `Scheduler` 是调度器的抽象接口，不同调度器实现不同的 `awaitFuture` 策略。
- `awaitFuture` 负责：注册 waker、等待 Future settled、返回结果或抛出错误。
- `ThreadScheduler`：使用 Mutex + CondVar 实现阻塞等待，适用于线程环境。
- 未来的 `EventLoopScheduler`：使用任务挂起/恢复实现非阻塞等待，适用于协程环境。

## 4 规则

- [必须] `Promise<T>` 与 `Future<T>` 是分离的泛型类型，`Promise` 不包含任何消费方法，`Future` 不包含任何生产方法。
- [必须] Future/Promise 不包含任何等待原语（如 CondVar），等待机制由 `Scheduler` 提供。
- [必须] Future/Promise 通过 waker 通知协议与调度器交互，settle 时调用所有已注册的 waker。
- [必须] `addWaker` 在 Future 已 settled 时必须立即调用 waker，确保通知不丢失。
- [必须] `resolve` 和 `reject` 都只能调用一次，重复调用必须抛出 `"promise/already-settled"`。
- [必须] `Future.get()` 在 rejected 状态下必须抛出 reject 时提供的错误。
- [必须] `Future.get()` 在 pending 状态下必须通过调度器等待，直到 settled。
- [必须] `Promise.future` 是在 Promise 构造时创建的字段，与 Promise 共享同一底层状态。
- [必须] `Future<T>` 不暴露 `resolve`、`reject` 或任何可改变状态的方法（`addWaker` 仅供调度器使用）。
- [必须] `Scheduler` 是 object-form spec，至少包含 `awaitFuture` 方法。
- [禁止] `Future` 提供任何非阻塞获取结果值的方法（避免轮询模式）。
- [禁止] 在 `Promise` 或 `Future` 中引入回调或链式组合（如 `then`、`map`），保持最小职责。

## 5 典型用法

### 5.1 基本 resolve

```feng
let promise = Promise<int>();
let future = promise.future;

Thread.spawn(() => {
  promise.resolve(42);
});

let result = future.get(); // result == 42
```

### 5.2 reject 与异常处理

```feng
let promise = Promise<string>();
let future = promise.future;

Thread.spawn(() => {
  promise.reject("io/read-failed");
});

try future.get() catch err: string {
  println("error: {0}", err);
}
```

### 5.3 非阻塞状态查询

```feng
let promise = Promise<int>();
let future = promise.future;

println("{0}", future.isReady()); // false

promise.resolve(1);
println("{0}", future.isReady()); // true
```

### 5.4 显式使用调度器

```feng
let scheduler = ThreadScheduler();
let promise = Promise<int>();

Thread.spawn(() => {
  promise.resolve(42);
});

let result = scheduler.awaitFuture(promise.future);
```

## 6 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-exception.md](./feng-exception.md): `throw`、`try/catch` 异常模型。
- [feng-generics-draft.md](./feng-generics-draft.md): 泛型类型规则。
- 标准库 `std.thread`：`Thread`、`Mutex`、`CondVar` 等线程原语。
