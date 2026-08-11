# Feng 标准库 `Event<T>` 规范

本文档定义标准库 `std.Event<T>` 的构造、监听器注册、注销、触发与清空语义。callable-form `spec` 的相等规则统一见 [feng-spec.md](./feng-spec.md)，本文只引用该规则，不重新定义。

## 1 公开 API

```feng
@value
open type Event<T> {
  open func Event(maxListeners: int): void;
  open func Event(): void;
  open func on(listener: Action<T>): void;
  open func off(listener: Action<T>): void;
  open func emit(arg: T): void;
  open func clear(): void;
}
```

## 2 语义

- `Event()` 创建监听器上限为 64 的事件。
- `Event(maxListeners)` 在 `maxListeners > 0` 时采用指定上限；传入 0 或负数时采用默认上限 64。
- `Event<T>` 是持有监听器列表引用的值类型句柄。每次独立构造创建不同的监听器列表；对已有 Event 赋值、传参或捕获时复制句柄，并继续共享同一个监听器列表。
- `on(listener)` 在当前监听器数量小于上限时，将 listener 追加到监听器序列末尾；重复注册同一 callable 引用会保留多个独立条目并分别计入上限。当前数量已经达到上限时，抛出 `"event/listener_limit_exceeded"`，事件保持不变。
- `off(listener)` 按 callable/closure 引用身份查找并删除第一个匹配条目；不存在匹配项时不修改事件，也不抛出异常。
- `emit(arg)` 按当前监听器序列的注册顺序依次调用监听器，并向每个监听器传入同一个 `arg`。
- `clear()` 删除全部监听器；后续 `emit` 在没有新增监听器时不执行任何回调。`off` 或 `clear` 使监听器数量低于上限后，可以继续通过 `on` 注册监听器。

`off` 必须复用 `List<Action<T>>.remove(listener)`，不得在 `Event<T>` 内重复实现元素查找或增加 `Action<T>` 特判。监听器在一次 `emit` 尚未结束时调用 `on`、`off` 或 `clear` 的行为不由当前版本保证；调用方不得依赖该场景中的访问次序或调用次数。

## 3 规则

- [必须] 默认监听器上限为 64；带参构造器仅接受正数作为自定义上限，非正数必须回退到默认上限。
- [必须] `on` 必须在追加前检查当前监听器数量；数量已经达到上限时必须抛出 `"event/listener_limit_exceeded"`，且不得修改监听器序列。
- [必须] Event 值的复制必须保留同一个监听器列表引用；通过任一副本执行 `on`、`off` 或 `clear` 的结果必须对其他副本可见。
- [必须] listener 匹配遵循 callable-form `spec` 的引用身份相等语义，不比较捕获内容、绑定对象或调用签名。
- [必须] `off` 最多删除一个监听器条目，并优先删除索引最小的匹配项。
- [必须] 在监听器序列未被回调修改时，`emit` 保持注册顺序且每个条目恰好调用一次。
- [必须] `clear` 释放事件对全部 listener 的持有关系。

## 4 关联

- [feng-spec.md](./feng-spec.md): callable-form `spec` 引用身份相等语义。
- [feng-std-list.md](./feng-std-list.md): `List<T>.remove(item)` 的首项删除与返回值语义。
- [feng-lifetime.md](./feng-lifetime.md): callable 引用的持有与释放规则。
