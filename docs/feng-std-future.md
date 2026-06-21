# Feng 标准库 Future 规范

本文档定义标准库 `std` 中的 `Future<T>` 与 `Promise<T>` 类型，用于异步结果传递。

## 1 职责

- 提供调度机制无关的异步结果传递：生产者通过 `Promise<T>` 发布结果，消费者通过 `Future<T>` 获取结果。
- `Promise<T>` 是生产者端，拥有 `resolve`、`reject` 和 `future` 方法，只能 resolve 或 reject 一次。
- `Future<T>` 是消费者端，提供等待获取结果和非阻塞状态查询能力，不包含任何生产方法。
- Future/Promise 是纯状态机 + waker 通知协议，不包含任何具体的同步或等待原语。同步机制与等待机制均由调度器提供。

## 2 公开 API

这些符号都定义在根模块 `std` 中。使用方通过 `import std;` 后即可直接使用。

### 2.1 `Promise<T>`

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Promise` | `open type Promise<T>` | 异步结果的生产者端，泛型参数 `T` 为结果值类型 |
| `Promise.Promise` | `open func Promise()` | 创建一个 pending 状态的 Promise |
| `Promise.resolve` | `open func resolve(value: T): void` | 将 Promise 标记为已解决并发布结果值 |
| `Promise.reject` | `open func reject(error: Error): void` | 将 Promise 标记为已拒绝并发布错误 |
| `Promise.future` | `open let future: Future<T>` | 关联的 Future 实例，在 Promise 构造时一并创建，共享底层状态 |

### 2.2 `Future<T>`

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Future` | `open type Future<T>` | 异步结果的消费者端，泛型参数 `T` 为结果值类型 |
| `Future.get` | `open func get(): T` | 等待结果；已解决时返回值，已拒绝时抛出错误 |
| `Future.isReady` | `open func isReady(): bool` | 非阻塞查询是否已完成（已解决或已拒绝） |

### 2.3 调度器集成（基础设施 API）

以下 API 供调度器实现使用，普通用户代码不应直接调用。

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Waker` | `open type Waker` | 唤醒回调协议，调度器将 Waker 注册到 Future，Promise settle 时调用 |
| `Future.registerWaker` | `open func registerWaker(waker: Waker): void` | 注册唤醒回调；若 Future 已 settled，立即调用 waker |

## 3 语义

### 3.1 状态模型

Promise 与 Future 共享同一底层状态，状态有三种取值：

- **pending**：初始状态，尚未 resolve 或 reject。
- **resolved**：已解决，携带结果值 `T`。
- **rejected**：已拒绝，携带错误 `Error`。

resolved 和 rejected 统称为 **settled**（已终结）。状态一旦进入 settled 即不可变更。

### 3.2 `Promise.resolve`

- 将 pending 状态的 Promise 转为 resolved，存储 `value`，并调用所有已注册的 waker。
- 若 Promise 已处于 settled 状态（已 resolved 或已 rejected），抛出 `Error { name: "promise/already-settled", message: "", origin: none }`。

### 3.3 `Promise.reject`

- 将 pending 状态的 Promise 转为 rejected，存储 `error`，并调用所有已注册的 waker。
- 若 Promise 已处于 settled 状态（已 resolved 或已 rejected），抛出 `Error { name: "promise/already-settled", message: "", origin: none }`。

### 3.4 `Promise.future`

- 在 Promise 构造时一并创建的关联 Future 实例。
- 与 Promise 共享同一底层状态。
- 需要多个 Future 引用时，直接传递同一 Future 实例即可。

### 3.5 `Future.get`

- 若 Future 已 resolved，返回结果值。
- 若 Future 已 rejected，抛出关联的 `Error`（`throw error`）。
- 若 Future 仍为 pending，通过调度器挂起当前执行单元直到 settled，然后按上述规则返回或抛出。
- 若不在调度器上下文中调用，抛出 `Error { name: "future/no-scheduler", message: "", origin: none }`。

### 3.6 `Future.isReady`

- 返回 `true` 当且仅当关联的 Promise 已 resolved 或已 rejected。
- 不阻塞。

### 3.7 `Future.registerWaker`

- 将 waker 注册到 Future 的等待列表中。
- 若注册时 Future 已 settled，立即调用该 waker（确保 waker 不会遗漏通知）。
- Promise settle 时，遍历并调用所有已注册的 waker，然后清空等待列表。

### 3.8 调度器集成

Future/Promise 通过 waker 通知协议与调度器交互，调度器负责提供两个能力：

1. **同步**（如何保护 Future/Promise 内部状态的并发访问）
2. **等待**（如何在 `get()` 时挂起并在 waker 被调用时恢复）

不同调度器提供不同实现：

- 线程调度器：同步用 Mutex，等待用 CondVar。
- 单线程 eventloop：同步为空操作（无并发），等待为任务挂起。
- 多线程 eventloop：同步用 Mutex 或原子操作，等待为任务挂起。

## 4 规则

- [必须] `Promise<T>` 与 `Future<T>` 是分离的泛型类型，`Promise` 不包含任何消费方法，`Future` 不包含任何生产方法。
- [必须] Future/Promise 的同步机制完全由调度器提供，Future/Promise 本身不包含任何具体的同步或等待原语。
- [必须] Future/Promise 通过 waker 通知协议与调度器交互，settle 时调用所有已注册的 waker。
- [必须] `registerWaker` 在 Future 已 settled 时必须立即调用 waker，确保通知不丢失。
- [必须] `resolve` 和 `reject` 都只能调用一次，重复调用必须抛出 `Error { name: "promise/already-settled", ... }`。
- [必须] `Future.get()` 在 rejected 状态下必须抛出 reject 时提供的 `Error`。
- [必须] `Future.get()` 在 pending 状态下必须通过调度器等待，直到 settled。
- [必须] `Future.get()` 不在调度器上下文中调用时，必须抛出 `Error { name: "future/no-scheduler", ... }`。
- [必须] `Promise.future` 是在 Promise 构造时创建的字段，与 Promise 共享同一底层状态。
- [必须] `Future<T>` 不暴露 `resolve`、`reject` 或任何可改变状态的方法（`registerWaker` 仅供调度器使用）。
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
  promise.reject(Error { name: "io/read-failed", message: "connection reset", origin: none });
});

try future.get() catch err: Error {
  println("error: {0} - {1}", err.name, err.message);
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

## 6 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-exception.md](./feng-exception.md): `throw`、`try/catch` 异常模型。
- [feng-generics-draft.md](./feng-generics-draft.md): 泛型类型规则。
- 标准库 `std.thread`：`Thread`、`Mutex`、`CondVar` 等线程原语。
