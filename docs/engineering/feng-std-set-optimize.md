# Feng 标准库 Set 稠密存储优化方案

> 状态：已实施  
> 主规范范围：本文定义 `std.collections.Set<K>` 的内部存储模型、行为边界、复杂度目标与验收要求。  
> 关联能力：[Feng 数组容器存储 API 开发方案](./feng-array-storage-dev.md)

## 1. 背景

优化前的 `Set<K>` 使用开放寻址、线性探测和 tombstone：

```text
states[capacity]
hashes[capacity]
keys[capacity]
```

其中 `size` 记录有效 key 数，`used` 同时包含有效槽位与 tombstone。删除会留下 tombstone；后续插入、删除或扩缩容需要根据 `size` 与 `used` 的比例重建整张表。`entries()` 也必须扫描全部 `capacity` 个槽位才能复制有效 key。

`Map<K, V>` 已采用“桶索引 + 稠密条目数组 + 桶内链”的存储模型。Set 与 Map 具有相同的 key 唯一性、hash 和 `same` 判断要求，因此 Set 采用同一索引模型的 key-only 形式，不保存无意义的 value payload。

## 2. 目标与非目标

### 2.1 目标

1. Set 的 `hashes`、`nexts` 和 `keys` 只在 `[0, size)` 保存有效条目。
2. 使用独立桶数组定位 hash bucket，使用条目索引链处理冲突。
3. 删除条目后使用最后一个有效条目填补空位，始终保持条目稠密。
4. 消除 tombstone、`states`、`used` 和 tombstone 原地重建路径。
5. 复用 `feng_array_storage_*` 完成条目存储的增长、收缩、插入和删除，保持托管 key 的生命周期正确。
6. 保持 `add`、`remove`、`has`、`count` 和 `entries` 的公开签名与集合语义不变。
7. 在 hash 正常分布时，`add`、`remove`、`has` 保持平均 O(1)；`entries` 只遍历有效条目。

### 2.2 非目标

1. 不把 Set 实现为 `Map<K, bool>` 或其他带虚拟 value 的 Map 包装。
2. 不修改 `Map<K, V>` 的实现或公开 API。
3. 不新增 runtime API，不修改私有 runtime ABI。
4. 不承诺 Set 的遍历顺序、插入顺序或 hash 顺序。
5. 不把桶索引或条目索引私自缩窄为固定宽度整数；索引继续使用平台相关 `int`。
6. 不在缺少基准数据时调整初始容量、增长阈值或收缩阈值。

## 3. 存储模型

Set 使用以下字段：

```text
buckets:  int[!]  // bucket 头部，长度固定为 capacity
hashes:   u64[!]  // 稠密条目 hash，length == size
nexts:    int[!]  // 稠密条目后继，length == size
keys:     K[!]    // 稠密条目 key，length == size
size:     int     // 有效条目数
capacity: int     // bucket 数及条目数组的 storage capacity
```

`buckets` 和 `nexts` 使用“条目索引加一”的编码：

```text
0       表示空 bucket 或链结束
index+1 表示 hashes[index]、nexts[index]、keys[index]
```

任意公开方法返回时必须满足以下不变量：

```text
0 <= size <= capacity
hashes.length() == size
nexts.length() == size
keys.length() == size

每个 [0, size) 条目恰好出现在一个 bucket 链中
条目所在 bucket == hashes[index] % capacity
每个 bucket 链都以 0 结束，且不存在环
```

空 Set 的 `capacity == 0`，各数组保持默认空数组。第一次插入时分配八个 bucket 和八个条目容量。

## 4. 核心操作

### 4.1 查询

查询先计算 `key.hash()`，再由 `hash % capacity` 选择 bucket，只遍历该 bucket 的索引链。只有条目 hash 相等时才调用 `key.same(candidate)`。

### 4.2 插入

`add(key)` 必须按以下顺序执行：

1. 计算 hash 并查询 key 是否已经存在；存在时直接返回。
2. 仅在确定插入新 key 后检查容量，避免重复添加触发无意义扩容。
3. 当 `(size + 1) / capacity >= 0.7` 时将容量扩大为两倍。
4. 分别向 `hashes`、`nexts`、`keys` 的尾部插入新条目，并把新条目设为 bucket 头部。

### 4.3 删除

`remove(key)` 在目标 bucket 链中找到条目后：

1. 从 bucket 链中解除目标索引。
2. 若目标不是最后一个稠密条目，将最后一个条目的 hash、next 和 key 搬到目标位置。
3. 在被搬条目所属的 bucket 链中，把旧索引引用替换为新索引。
4. 从三个条目数组末尾删除最后一个槽位，并将 `size` 减一。
5. 当 `capacity > 8` 且 `size / capacity <= 0.2` 时将容量缩小为一半，但不得低于八。

条目搬移只改变 Set 的内部位置，不产生任何公开顺序保证。

### 4.4 entries

`entries()` 返回包含调用时全部有效 key 的不可写数组副本：

1. 返回数组长度恰好等于 `size`。
2. 每个有效 key 恰好出现一次。
3. 返回结果与 Set 的后续修改互不影响。
4. 元素顺序不属于公开契约；调用方不得依赖插入顺序、bucket 顺序或删除前后的相对顺序。

## 5. 复杂度与资源边界

设有效条目数为 `n`，当前容量为 `C`：

| 操作 | 正常 hash 分布 | 最坏冲突 |
| --- | --- | --- |
| `has` | 平均 O(1) | O(n) |
| `add` | 摊销 O(1) | O(n) |
| `remove` | 平均 O(1) | O(n) |
| `entries` | O(n) | O(n) |
| 扩缩容 | O(C + n) | O(C + n) |

与原开放寻址实现相比，本方案增加一个 bucket 索引数组和一个条目后继数组，同时删除 `states` 数组。64 位目标上的索引元数据可能高于原实现；本优化的验收不能只比较单一操作耗时，还必须同时记录查找、增删、稀疏 `entries()`、连续删除后的性能和内存占用。

## 6. 测试与验收

实现必须新增以下标准库行为测试，既有测试不得删除或放宽：

1. 所有 key 使用相同 hash 时，添加、查询和重复添加仍然正确。
2. 从碰撞链中删除多个非末尾条目后，其余 key 仍可查询，`count()` 正确。
3. 多次扩容后连续删除触发收缩，其余 key 和 `entries()` 仍然正确。
4. 托管 key 经历扩容、删除和收缩后保持有效，删除的 key 不再由 Set 持有。
5. `entries()` 在删除导致末尾条目搬移后包含全部且仅包含存活 key。
6. `Set<i32>` 的泛型标量 direct-call 既有测试继续通过，不引入装箱或运行时查表。
7. 完成实现后在 Codex 沙箱外执行全量 `make test`。

性能结论必须以后续同平台、同构建配置的基准结果为依据。本次结构重构不预先声称 `has` 或 `add` 一定快于开放寻址实现。

## 7. 实施范围

本次只修改：

1. 本主规范。
2. `std/std/src/collections/Set.ff`。
3. `std/std_test/src/test_set.ff` 中新增的 Set 行为测试。

不修改编译器、runtime、Map、Hashable 或既有测试断言。
