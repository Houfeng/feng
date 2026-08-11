# Feng 标准库 `List<T>` 规范

本文档定义标准库 `std.collections.List<T>` 的公开 API、元素移除语义与复杂度边界。内建数组及数组扩展能力统一见 [feng-builtin-type.md](./feng-builtin-type.md) 与 [feng-std-array.md](./feng-std-array.md)，本文不重复定义。

## 1 职责

- 定义可增长有序集合 `List<T>` 的公开构造、访问、修改与遍历 API。
- 定义按索引移除与按元素移除的独立命名、返回值和匹配规则。
- 定义 `List<T>` 对输入数组和输出数组的复制边界。

## 2 公开 API

```feng
open type List<T> {
  open func List();
  open func List(items: T[!]);
  open func List(items: T[]);
  open func List(capacity: int);

  open func add(item: T): void;
  open func insert(index: int, item: T): void;
  open func get(index: int): T;
  open func set(index: int, item: T): void;
  open func removeAt(index: int): void;
  open func remove(item: T): bool;
  open func clear(): void;
  open func size(): int;
  open func entries(): T[];
  open func iter(): ListIterator<T>;
}
```

`List<T>` 保持元素插入顺序。`List(items)` 必须复制输入数组的当前有效元素，后续列表修改不得改变输入数组。`entries()` 必须返回列表当前全部元素的只读数组副本，后续列表修改不得改变已经返回的数组。

## 3 移除语义

### 3.1 `removeAt(index: int)`

`removeAt` 删除零基位置 `index` 上的元素，并将后续元素依次左移。`index < 0` 或 `index >= size()` 时必须失败，不得静默忽略或截断。

### 3.2 `remove(item: T): bool`

`remove` 从索引 `0` 开始查找与 `item` 相等的第一个元素：

- 找到时，删除该元素、保持其余元素的相对顺序并返回 `true`。
- 未找到时，不修改列表并返回 `false`。
- 存在多个相等元素时，每次调用只删除索引最小的一个。

元素匹配必须复用运行时表达式相等 helper，并与 Feng 表达式相等语义一致；不得在 `List<T>` 的泛型方法体中直接对类型参数 `T` 使用 `==`。因此基础值、字符串、对象引用、callable-form `spec` 与聚合值分别遵循各自已经定义的语言相等语义。

按索引移除与按元素移除必须使用不同方法名：按索引操作固定命名为 `removeAt(index)`，`remove(item)` 仅表示按元素移除。不得恢复 `remove(index: int)`，否则 `List<int>` 上会与 `remove(item: T)` 形成重复签名和调用歧义。

## 4 复杂度

- `get`、`set` 与 `size` 的时间复杂度必须为 `O(1)`。
- `add` 的均摊时间复杂度必须为 `O(1)`。
- `insert` 与 `removeAt` 因元素移动，最坏时间复杂度为 `O(n)`。
- `remove(item)` 包含线性查找与至多一次元素移动，最坏时间复杂度为 `O(n)`。
- `entries()` 的时间和空间复杂度均为 `O(n)`。

## 5 规则

- [必须] `removeAt(index)` 只按位置删除，不执行元素相等比较。
- [必须] `remove(item)` 只删除第一个匹配元素，并以 `bool` 报告列表是否发生变化。
- [必须] `remove(item)` 通过运行时表达式相等 helper 判断匹配，不得为特定元素类型增加特殊分支。
- [必须] `removeAt`、`remove` 与 `clear` 必须正确释放被删除元素对应的列表持有关系；保留元素因左移不得产生额外的引用计数增减。
- [必须] `List(items)` 与 `entries()` 均使用独立数组存储，不得泄漏列表的私有 backing array。

## 6 关联

- [feng-expression.md](./feng-expression.md): 表达式相等语义。
- [feng-spec.md](./feng-spec.md): callable-form `spec` 与其他 `spec` 值的相等语义。
- [feng-std-array.md](./feng-std-array.md): 数组复制、`indexOf` 与运行时表达式相等 helper。
- [feng-lifetime.md](./feng-lifetime.md): 托管值的持有、移动与释放规则。
