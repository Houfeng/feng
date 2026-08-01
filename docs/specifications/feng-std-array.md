# Feng 标准库数组扩展规范

本文档只定义标准库 `std.collections` 作用在内建数组 `T[]` / `T[!]` 上的扩展 API，以及 `Span<T>` 只读视图能力。数组类型语法、逐层可写性、类型兼容、ABI 约束与托管生命周期统一见 [feng-builtin-type.md](./feng-builtin-type.md) 与 [feng-lifetime.md](./feng-lifetime.md)。

## 1 职责

- 定义 `std.collections` 在数组目标上提供的 `length()`、`indexOf(value)`、`clone()` 与 `clone(start, end)` 扩展。
- 定义 `std.collections` 提供的 `slice()`、`Span<T>`、`Span<T>.slice()`、`Span<T>.get()` 与 `Span<T>.toArray()` 能力。
- 定义这些标准库能力的返回值语义、边界校验与共享/复制约束。

## 2 语义

内建数组 `T[]` / `T[!]` 的类型语法、逐层可写性、显式转换、运行时对象与 ABI 约束统一见 [Feng 内建类型规范](./feng-builtin-type.md)。本文只定义标准库在这些数组目标上的扩展方法与 `Span<T>` 视图能力。

补充定义:

- `std.collections` 在数组目标 `T[]` 与 `T[!]` 上提供 `length()`，返回类型为 `long`（`i64`），表示当前数组层元素个数。
- `std.collections` 在数组目标 `T[]` 与 `T[!]` 上提供 `indexOf(value)`，返回指定值第一次出现的零基索引；若不存在匹配元素，则返回 `-1`。
- `indexOf(value)` 的匹配判定由运行时表达式相等 helper 执行，直接比较当前数组元素与目标值；该 helper 不改变数组长度、元素所有权或数组 payload。
- `std.collections` 提供 `clone()` 与 `clone(start, end)`。`clone()` 返回复制当前数组全部元素得到的新数组；`clone(start, end)` 返回复制区间 `[start, end)` 得到的新数组，不与源数组共享底层元素存储。只读数组 `T[]` 的复制结果保持为 `T[]`，可写数组 `T[!]` 的复制结果保持为 `T[!]`。
- 标准库模块 `std.collections` 在数组目标上提供 `slice(start, end)`，返回只读切片视图 `Span<T>`。`Span<T>` 是 `std.collections` 中定义的共享底层数组的普通对象，不是新的内建数组类型，也不改变数组实例长度固定的语义；当前阶段应通过数组 `slice` 获得该视图。
- `Span<T>` 采用右开区间 `[start, end)` 表示视图范围；`length()` 返回 `end - start`，`get(index)` 以视图左端为基准访问元素。
- `Span<T>` 在自身上继续提供 `slice(start, end)`，表示以当前视图左端为基准再取得一个只读子视图；该操作继续共享同一底层数组，不隐式复制元素。
- `Span<T>` 提供 `toArray()`，返回复制当前视图全部元素得到的新数组；该数组不与源数组或源视图共享底层元素存储。
- 当前版本不支持 `.length` / `.len` 属性语法；必须使用方法调用形式 `value.length()`。

```feng
let nums = [1, 2, 3];
let count = nums.length();
let cloned = nums.clone();
let writable: int[!] = [1, 2, 3];
let writable_cloned: int[!] = writable.clone();
let cloned_range = nums.clone(1, 3);
let middle = nums.slice(1, 3);
let dense = middle.toArray();
let nested = middle.slice(0, 1);
let first = nested.get(0);
```

## 3 规则

分为「必须」。

- [必须] 数组长度访问仅通过 `std.collections` 提供的 `length()` 方法提供，返回类型为 `long`（`i64`）；当前版本不得提供 `.length` / `.len` 属性语法。
- [必须] 标准库数组 `length()` 的实现必须直接读取运行时数组长度元数据，时间复杂度必须为 `O(1)`；不得通过遍历数组计数获得长度。
- [必须] `std.collections` 提供的数组 `indexOf(value)` 必须从索引 `0` 到 `array.length() - 1` 按顺序查找，返回第一个匹配元素的索引；未找到时必须返回 `-1`。
- [必须] `std.collections` 提供的数组 `indexOf(value)` 必须通过运行时表达式相等 helper 完成元素匹配，不得在标准库泛型 `fit` 方法体内直接对元素类型参数 `T` 使用 `==`。
- [必须] `std.collections` 提供的数组 `clone()` 必须返回复制当前数组全部元素得到的新数组，不得返回共享底层存储的别名视图。
- [必须] 数组 `clone()`、`clone(start, end)` 与 `clone(start)` 的返回类型必须保持当前数组层的可写性：`T[]` 返回 `T[]`，`T[!]` 返回 `T[!]`；不得为复制结果增加源数组当前层没有的写权限。
- [必须] `std.collections` 提供的数组 `clone(start, end)` 必须校验 `start >= 0`、`end >= start` 且 `end <= array.length()`；不满足时必须以 Feng 异常失败，而不是静默截断。
- [必须] `std.collections` 提供的数组 `clone(start, end)` 返回的新数组必须与源数组解耦；后续对源数组元素的写入不得改变已返回的新数组内容。
- [必须] `std.collections` 提供的数组 `slice(start, end)` 返回共享底层数组的只读视图 `Span<T>`；该操作不得隐式复制底层数组元素。
- [必须] `slice(start, end)` 必须校验 `start >= 0`、`end >= start` 且 `end <= array.length()`；不满足时必须以 Feng 异常失败，而不是静默截断。
- [必须] `std.collections.Span<T>.length()` 必须返回当前视图覆盖的元素个数。
- [必须] `std.collections.Span<T>.slice(start, end)` 必须以当前视图左端为基准校验 `start >= 0`、`end >= start` 且 `end <= span.length()`；不满足时必须以 Feng 异常失败。
- [必须] `std.collections.Span<T>.get(index)` 必须按视图边界校验 `index`，不得把越界访问透传为对底层数组其他区间的合法访问。
- [必须] `std.collections.Span<T>.toArray()` 必须返回复制当前视图元素得到的新数组，不得返回共享底层存储的别名视图。

## 4 运行结果

- `length()` 必须直接读取底层数组长度元数据，不得通过遍历数组计数。
- `indexOf(value)` 必须返回第一个匹配元素索引；未找到时返回 `-1`。
- `clone()` 与 `Span<T>.toArray()` 必须产生新的数组对象；数组 `clone()` 的结果保持源数组当前层的可写性。
- `slice()` 与 `Span<T>.slice()` 只返回共享底层数组的只读视图，不得隐式复制数组元素。

## 5 关联

- [feng-builtin-type.md](./feng-builtin-type.md): 数组类型语法、可写性、兼容性、ABI 约束与运行时属性。
- [feng-expression.md](./feng-expression.md): 数组创建表达式、索引访问与方法调用语法。
- [feng-fit-builtin-type.md](./feng-fit-builtin-type.md): 数组作为 `fit` 左侧目标时的补充规则。
- [feng-lifetime.md](./feng-lifetime.md): 数组底层对象布局与托管生命周期。
