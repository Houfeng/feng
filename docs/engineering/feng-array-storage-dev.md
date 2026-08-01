# Feng 数组容器存储 API 开发方案

> 状态：已 Review，实施中
> 主规范范围：本文定义 `FengArray` 的内部容量模型，以及仅供标准库容器使用的 array storage runtime contract API。  
> 实施顺序：先实现并验证 runtime API；API 独立通过回归后，再单独优化 `List<T>`。

## 1. 背景

当前 `FengArray` 采用 header 与 payload 同一块 allocation 的尾随内联布局，但只有 `length`，没有独立的 `capacity`：

```text
allocation size = array header + length * element_size
```

因此当前数组始终满足：

```text
length == 已初始化元素数量 == 已分配槽位数量
```

当前 `List<T>` 为了实现容量复用，只能把 `items.length()` 当作容量，另用 `count` 表示有效元素数量。这会带来以下开销：

1. `grow()` / `shrink()` 创建新数组并逐项赋值，托管元素在迁移过程中产生不必要的 retain/release。
2. `clear()` 直接替换为 `T[:0]`，无法保留原容量。
3. `[count, items.length())` 仍然必须存放合法的默认值；对于需要自定义默认初始化的元素，这些备用槽位也会产生初始化和生命周期开销。
4. `remove()` 逐项向左赋值，托管元素在移动过程中产生逐项 retain/release。

`docs/engineering/feng-array-optimize-delivered.md` 已把 List 迁移 API 明确留到后续阶段。本文承接该边界，不重新定义已交付的尾随内联方案。

## 2. 目标与非目标

### 2.1 目标

1. 为 `FengArray` 增加独立的 `capacity`，允许内部存在未初始化的备用槽位。
2. 保持普通 Feng 数组的固定长度语言语义不变。
3. 提供一组 array storage runtime contract API，让标准库容器可以读取固定的 `capacity`，并在该容量内管理作为占用数量的 `length`。
4. 需要不同容量时，通过迁移创建另一个 capacity 固定的新数组；保留元素的槽位内容及引用持有关系随之转移，不产生元素级 retain/release。
5. 中间删除时只释放被删除元素，保留元素通过重叠安全的内存迁移完成左移。
6. `clear()` 后保留容量，托管元素按正常生命周期规则释放。
7. 继续复用 `FengArray` 已有的元素类型信息、终结逻辑和循环回收遍历能力，不引入第二套容器存储对象。

### 2.2 非目标

1. 本文不改变普通数组的公开语法和 API。
2. 本文不向普通程序公开 `capacity`、`insert`、`remove` 或 `migrate`。
3. 第一实施阶段不修改 `List<T>`。
4. 第一实施阶段不优化 `ListIterator<T>`，也不确定迭代期间修改 List 的最终语义。
5. 本文不引入 COW、切片共享、独立 payload allocation 或新的内存池。
6. 本文不改变普通数组赋值、传参和返回时的引用语义。

## 3. 分层与命名

新增 API 使用 `feng_array_storage_*` 命名：

```text
feng_array_*          普通数组 runtime 能力，遵守语言层固定长度语义
feng_array_storage_*  容器底层存储能力，可以观察固定 capacity 或在其范围内改变 length
```

`storage` 表示这些 API 把 `FengArray` 当作容器 backing storage 使用，而不是普通的固定长度数组操作。

本文约定名称中的 `range` 表示由 `start/end` 描述的右开区间。删除 API 接收的是 `index/count`，因此命名为 `feng_array_storage_remove`，不使用 `remove_range`。

这些 API 属于 runtime contract，只供随编译器发布的标准库声明和使用，不承诺稳定性。它们不应出现在普通数组的公开扩展 API 中。

## 4. `FengArray` 存储模型

### 4.1 结构变化

`FengArray` 增加 `capacity`：

```c
struct FengArray {
    FengManagedHeader header;
    size_t length;
    size_t capacity;
    size_t element_size;
    const FengTypeDescriptor *element_desc;
    FengValueKind element_kind;
    const FengAggregateDescriptor *element_aggregate;
};
```

payload 继续尾随内联在同一块 allocation 中：

```text
allocation size = aligned array header + capacity * element_size
```

### 4.2 核心不变量

任意时刻必须满足：

```text
0 <= length <= capacity
```

`capacity` 是单个数组实例的物理存储属性：

1. allocation 创建时确定。
2. 该实例存活期间始终不变。
3. 普通数组 API 和 storage API 都不得原地修改它。
4. 若需要不同容量，只能创建另一个数组实例并迁移有效元素。

`length` 的语义按使用层次区分：

1. 普通数组创建完成时 `length == capacity`，之后保持不变。
2. 作为容器 backing storage 使用时，`length` 表示当前已占用、已初始化的元素数量，可以由 storage API 在 `[0, capacity]` 内改变。

元素区分为两个区间：

```text
[0, length)       已初始化、可索引、参与 ARC/终结/循环遍历
[length, capacity) 未初始化、不可索引、不参与 ARC/终结/循环遍历
```

未初始化槽位中的字节没有 Feng 值语义，可以是零字节，也可以保留旧字节。任何路径都不得读取、复制为普通 Feng 值或释放这些槽位。

### 4.3 普通数组保持固定长度

普通数组构造仍然执行：

```text
length = requestedLength
capacity = requestedLength
```

并默认初始化全部 `[0, length)` 元素。普通数组语义下没有任何 API 可以改变该实例的 `length` 或 `capacity`；因此新增 `capacity` 不改变语言层数组行为。

普通数组的下标检查继续只使用 `length`：

```text
index < length
```

### 4.4 payload 访问

内部 payload 地址是否存在由 `capacity` 决定，而不是由 `length` 决定。`length == 0 && capacity > 0` 时，内部仍然存在可供 storage API 使用的 payload。

为保持普通数组现有可观察行为：

1. 复用现有内部 `feng_array_payload_inline()` / `feng_array_payload_inline_const()` 作为 payload 起始地址的唯一计算入口，但把空 payload 判断从 `length == 0` 改为 `capacity == 0`。
2. `feng_array_data()` 在调用内部 payload helper 前单独检查 `length == 0`，继续向普通数组访问路径返回 `NULL`，保持现有可观察行为。
3. storage API 直接通过现有内部 payload helper 取得起始地址；第 `index` 个槽位地址为 `payload + index * element_size`，不增加第二套 payload 地址计算 helper。

### 4.5 生命周期与循环回收

以下路径全部只处理 `[0, length)`：

1. `feng_array_finalize_internal()`。
2. cycle collector 的所有数组子引用遍历路径。
3. 数组 slice/copy 等普通数组操作。

`[length, capacity)` 不表示有效 Feng 值；其中旧字节即使仍呈现为指针，也不计作有效引用。

## 5. Runtime contract API

Feng 侧拟采用以下声明形态：

```feng
@runtime
extern func feng_array_storage_get_capacity<T>(array: T[!]): int;

@runtime
extern func feng_array_storage_insert<T>(array: T[!], index: int, value: T): void;

@runtime
extern func feng_array_storage_remove<T>(array: T[!], index: int, count: int): void;

@runtime
extern func feng_array_storage_migrate<T>(array: T[!], newCapacity: int): T[!];
```

编译器继续按现有 runtime 泛型规则注入 `FengGenericParamDescriptor`。C contract 形态拟为：

```c
intptr_t feng_array_storage_get_capacity(
    const FengGenericParamDescriptor *type,
    const FengArray *array);

void feng_array_storage_insert(
    const FengGenericParamDescriptor *type,
    FengArray *array,
    intptr_t index,
    const void *value);

void feng_array_storage_remove(
    const FengGenericParamDescriptor *type,
    FengArray *array,
    intptr_t index,
    intptr_t count);

FengArray *feng_array_storage_migrate(
    const FengGenericParamDescriptor *type,
    FengArray *array,
    intptr_t new_capacity);
```

数组实例中记录的 `element_kind`、`element_size`、`element_desc` 和 `element_aggregate` 是元素生命周期操作的权威信息；泛型 descriptor 用于 runtime contract ABI 以及必要的一致性检查。

### 5.1 `feng_array_storage_get_capacity`

用途：读取数组 allocation 中实际可容纳的元素数量。

语义：

```text
result = array.capacity
```

要求：

1. `array` 必须是合法数组。
2. `capacity` 必须能转换为 Feng `int`；否则 runtime panic。
3. 不修改数组，不执行任何元素生命周期操作。

### 5.2 `feng_array_storage_insert<T>`

用途：在已有容量内向指定位置插入一个元素，将原位置及后续元素整体右移，并扩大逻辑长度。

前置条件：

```text
0 <= index <= array.length
array.length < array.capacity
```

执行逻辑：

1. 将 `[index, array.length)` 通过重叠安全的 `memmove` 整体右移一个槽位。
2. 被右移元素只发生存储位置迁移，不执行 retain/release。
3. 移动后 `data[index]` 中的旧字节不再表示有效 Feng 值，可用于初始化新元素。
4. 在 `data[index]` 按元素类别执行 copy-initialization：
   - trivial：复制 `element_size` 字节；
   - managed pointer：保存指针并 retain 一次，使被插入元素的 RC 正常增加 1；
   - aggregate：复制完整值，并按 aggregate descriptor 将每个托管槽位 retain 一次。
5. 新元素完全初始化后，设置 `length += 1`。
6. 不改变 `capacity`。

`insert` 只为新插入元素建立一个新的数组槽位引用持有关系。原有元素即使因插入而右移，也只发生存储位置迁移，其 RC 必须保持不变。

当 `index == array.length` 时，右移区间为空，`insert` 直接执行尾部初始化，等价于 append 快路径。

若数组已满，runtime panic；`insert` 不负责自行决定增长策略，也不隐式调用 `migrate`。

首版只提供单元素插入，不提供 `insert_range`：

1. 单元素插入可以直接承接 `List.add(item)` 和未来的 `List.insert(index, item)`。
2. `remove(index, count)` 不需要额外的输入容器，并且可以统一实现单项删除与 clear，因此保留批量形式。
3. 批量插入若接收数组，调用方为了组织待插入元素可能需要额外分配数组；若来源数组已经存在，又需要继续定义来源区间、来源与目标相同数组时的重叠、自插入和别名语义。
4. 当前没有已确认的标准库调用需求或基准数据，不提前扩展批量插入 contract；未来可在存在实际需求时独立增加。

### 5.3 `feng_array_storage_remove<T>`

用途：删除右开区间 `[index, index + count)`，将后续元素整体左移，并缩短逻辑长度。

前置条件：

```text
index >= 0
count >= 0
index <= length
count <= length - index
```

执行逻辑：

```text
tailStart  = index + count
tailCount  = length - tailStart
newLength  = length - count
```

1. 被删除区间中的元素按元素类别正常释放：managed pointer 元素的 RC 正常减少 1；aggregate 元素的每个托管槽位正常 release 一次。
2. `[tailStart, length)` 通过重叠安全的 `memmove` 左移到 `index`。
3. 保留元素视为存储位置迁移，不执行 retain/release。
4. 设置 `length = newLength`。
5. 不改变 `capacity`。
6. 移动后落在 `[newLength, oldLength)` 的旧字节不再表示有效 Feng 值，不得再次释放。

`remove` 只解除被删除元素对应的数组槽位引用持有关系。被保留元素即使因删除而左移，其 RC 必须保持不变。

`count == 0` 时为空操作。

释放托管元素时，必须保证 cycle collector 不会遍历到已经 release、但仍残留在有效区间内的陈旧指针。实现应在 release 前使相应托管槽位失效，或采用等价的 collector-safe 顺序；不能直接对仍可被数组遍历的陈旧槽位调用 release。

`List.clear()` 后续通过以下方式实现，不再需要独立的 `feng_array_storage_clear`：

```feng
feng_array_storage_remove(self.items, 0, self.items.length());
```

### 5.4 `feng_array_storage_migrate<T>`

用途：创建一块容量为 `newCapacity` 的新尾随内联 array allocation，并把旧数组的有效元素迁移过去。所谓扩容、缩容和等容量纯迁移，均表示“用另一个数组实例替换 backing array”，不表示修改旧数组实例的 `capacity`。

计算：

```text
oldLength = array.length
moveLength = min(oldLength, newCapacity)
```

迁移与释放区间固定为：

```text
[0, moveLength)         迁移到新数组，元素 RC 不变
[moveLength, oldLength) 丢弃并正常释放，元素 RC 减少
```

只有 `newCapacity < oldLength` 时，第二个区间才非空，此时迁移同时完成缩容截断。若 `newCapacity >= oldLength`，全部有效元素都会迁移，任何元素都不会因迁移发生 RC 增减。

对旧数组、迁移元素和丢弃元素的处理分别为：

1. **旧数组**：迁移成功后旧数组实例的 `length` 固定设为 `0`；其 `capacity`、allocation 地址和数组引用计数均不由 `migrate` 修改。`migrate` 不释放调用方传入的旧数组引用，调用方仍须通过普通赋值路径替换并 release 该引用。旧 payload 中残留的字节不再表示 Feng 值，不得读取、释放或再次迁移。
2. **迁移元素**：`[0, moveLength)` 的完整槽位字节复制到新数组的同一下标区间；元素的引用持有关系从旧槽位转移给新槽位，不执行默认初始化、copy-initialization、retain 或 release。物理字节在短暂时间内可能同时存在于新旧 payload，但语义上始终只有一份元素所有权，元素 RC 保持不变。
3. **丢弃元素**：`[moveLength, oldLength)` 中的每个元素按 trivial、managed pointer 或 aggregate 的现有生命周期规则恰好释放一次。managed pointer 和 aggregate 槽位必须在可能触发同步 cycle collector 遍历的 release 前清除托管引用；释放后的旧槽位不再是有效 Feng 值，且不得在旧数组终结时再次释放。

若其他引用仍指向旧数组实例，该引用不会重定向新数组，并会在迁移后观察到 `length == 0`。因此 `migrate` 只能用于不再被其他调用方按普通数组语义观察的私有 backing array；完整引用约束见第 7 节。

执行逻辑：

1. 校验 `newCapacity >= 0` 以及 allocation 大小无溢出。
2. 分配一块新的 array allocation，设置：

   ```text
   newArray.length = 0
   newArray.capacity = newCapacity
   ```

   `newArray.capacity` 在 allocation 创建时确定，后续不再改变；旧数组的 `capacity` 在整个迁移过程中也保持不变。

3. `[0, moveLength)` 按槽位迁移处理：复制元素字节，引用持有关系从旧数组槽位转到新数组槽位，但不执行元素级 retain/release，元素 RC 不变。
4. `[moveLength, oldLength)` 为 `newCapacity < oldLength` 时产生的缩容截断区间，元素被丢弃并按元素类别正常释放，元素 RC 相应减少。
5. 在不执行任何元素生命周期回调的连续状态更新中，设置 `oldArray.length = 0` 和 `newArray.length = moveLength`。此后旧数组的终结、cycle collector 遍历和普通索引均不会再触及已迁移或已丢弃的槽位。
6. 返回新的 `+1` 数组引用。
7. 调用方必须立即用返回值替换旧 backing array：

   ```feng
   self.items = feng_array_storage_migrate(self.items, newCapacity);
   ```

8. 旧数组随后由普通赋值路径 release；其终结路径不得再次释放已经迁移的元素。

无论目标容量大于、等于还是小于旧容量，保留前缀的元素 RC 都不变；只有超出 `newCapacity` 的旧有效元素发生 RC 减少。

首次实现不使用 `realloc`。`FengArray` 的 header 与 payload 位于同一块尾随内联 allocation，`realloc` 一旦移动 allocation，整个数组对象的地址都会改变。调用形态：

```feng
self.items = feng_array_storage_migrate(self.items, newCapacity);
```

在语义上仍需要先以旧数组引用调用 `migrate`，再用返回的新数组替换 `self.items`，并由普通赋值路径 release 字段中的旧引用。若 `realloc` 已把对象移到新地址，字段中保留的旧地址已是野指针，随后的 release 会访问失效的对象头，造成 use-after-free。

`array.header.refcount == 1` 也不能解决该问题：它只能证明没有其他数组引用，不能阻止调用方字段在赋值完成前仍持有并随后 release 旧地址。若要安全使用 `realloc`，必须另外引入“调用前从调用方槽位取出并消费旧引用”的 contract 或编译器特殊发码，不属于本方案。

首版固定采用“新 allocation + 槽位迁移 + 旧数组有效长度归零”的方式，确保普通赋值路径仍可安全 release 旧数组。

若 allocation 失败，必须在修改旧数组前 panic，保证旧数组仍保持原状态。

## 6. 元素类别分派

四个 API 复用现有三类值模型：

| 元素类别 | insert 新元素 | remove | migrate 保留区间 | migrate 截断区间 |
| --- | --- | --- | --- | --- |
| `FENG_VALUE_TRIVIAL` | `memcpy` | 不做 release | `memcpy` | 不做 release |
| `FENG_VALUE_MANAGED_POINTER` | 保存并 retain | release 被删除指针 | 转移指针槽位及其引用持有关系，RC 不变 | release 被截断指针 |
| `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | 复制并 retain 托管槽 | aggregate release | 转移完整 aggregate 槽位及其引用持有关系，RC 不变 | aggregate release |

storage API 不得对 `[length, capacity)` 执行 aggregate default initialization。新增容量只产生未初始化槽位。

### 6.1 RC 计数不变量

本节所说的 RC 变化是元素持有的托管引用计数变化，不包括新旧 `FengArray` 对象自身的引用计数。

storage API 必须满足：

| 操作 | 新增元素 | 删除或丢弃元素 | 保留但移动的元素 |
| --- | --- | --- | --- |
| `insert` | 每个新增托管引用正常 `+1` | 无 | RC 不变 |
| `remove` | 无 | 每个被删除托管引用正常 `-1` | RC 不变 |
| `migrate` | 无 | 每个被新容量截断的托管引用正常 `-1` | RC 不变 |

对于 `FENG_VALUE_MANAGED_POINTER`，表中的 `+1/-1` 对应元素指针本身的一次 retain/release。对于 `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`，规则分别应用到 aggregate descriptor 描述的每个托管槽位。若同一个对象在多个元素或多个槽位中重复出现，每个存储位置分别表示一次独立的引用持有关系，必须分别计数。

## 7. backing array 引用约束

本稿建议 storage mutation API 只操作不会被普通数组语义继续观察的标准库容器 backing array：

1. `insert` 和 `remove` 会原地改变 `length` 与 payload。
2. `migrate` 会把旧数组有效元素的槽位内容及引用持有关系迁移到新数组，并将旧数组的有效长度归零。
3. 若调用方仍以普通数组语义观察同一个 `FengArray`，上述操作会破坏普通固定长度数组的可观察语义。

后续优化 `List<T>` 时，`List(items: T[!])` 不得继续直接保存调用方数组引用。该构造函数必须利用 `std.collections` 已有的数组 `clone()` 能力，以复制输入数组全部有效元素的语义创建新的私有 backing array。新 backing array 不得与输入数组共享 payload；后续 List 的 `insert`、`remove` 或 `migrate` 不得改变调用方数组的 `length` 或元素槽位。

`ListIterator<T>` 本轮保持现状，继续保存 backing array 引用：

1. 第一阶段不修改 `List<T>` 或 `ListIterator<T>`。
2. 第二阶段优化 `List<T>` 时，不同步改造 Iterator 的字段、游标或失效检查机制。
3. `for/in` 每次进入循环时创建独立 Iterator，循环结束时按当前作用域生命周期清理。
4. 是否改为引用 List、是否增加版本号或其他迭代失效检查，后续根据实际需求和问题单独评估。

首版 API 不在 runtime 中检查 `array.header.refcount == 1`。该检查不能证明没有调用方继续按普通数组语义观察 backing array，且会给尾部 `insert` 热路径增加一次引用计数读取；独占性由标准库实现保证。

首版不建议增加共享数组复制 fallback。共享 fallback 会让 `migrate` 的“保留元素 RC 不变”只在部分路径成立，并掩盖 backing array 别名错误。

## 8. 第一阶段：实现并验证 storage API

第一阶段只实现底层能力，不修改 `std/std/src/collections/List.ff`。

### 8.1 文档

- [x] 本文 Review 通过后作为本次开发主文档。
- [x] 实现时同步更新 `docs/engineering/feng-runtime-contract-api.md`，只登记新增 contract API 并引用本文，不重复维护完整算法。
- [x] 在 `docs/engineering/feng-array-optimize-delivered.md` 的后续边界处引用本文，不改写已交付阶段的历史范围。
- [x] 人工 Review。

### 8.2 Runtime

- [x] 为 `FengArray` 增加 `capacity`。
- [x] 普通数组创建路径设置 `capacity = length`，保持现有公开语义。
- [x] 新增可接收独立 `length/capacity` 的内部 storage allocation 路径，其 allocation 大小按 `capacity * element_size` 计算；现有普通数组创建路径继续传入 `length == capacity`，行为不变。
- [x] 将现有内部 `feng_array_payload_inline()` / `feng_array_payload_inline_const()` 的空 payload 判断改为基于 `capacity`；`feng_array_data()` 自身保留 `length == 0` 返回 `NULL` 的普通数组行为。storage API 直接复用现有内部 helper，不新增第二套 payload helper。
- [x] 审计终结器、cycle collector 及其他数组路径，并增加回归测试，确认所有路径都只遍历 `[0, length)`，没有路径错误地遍历到 `capacity`。
- [x] 补齐用例进行验证，并进行全量回归测试。
- [x] 人工 Review。

### 8.3 Runtime contract

- [x] 在 `src/runtime/feng_runtime_contract.inc` 增加四个 contract 符号。
- [x] 在数组 runtime 模块中实现四个 API；`feng_runtime_contract.c` 只保留必要的 contract bridge，避免重复数组生命周期逻辑。
- [x] 补齐用例进行验证，并进行全量回归测试。
- [x] 人工 Review。

### 8.4 测试

新增测试至少覆盖：

- [x] 普通数组仍满足 `length == capacity`，现有数组行为不变。
- [x] `length == 0 && capacity > 0` 时，普通索引仍全部越界，storage insert 可以复用容量。
- [x] trivial、managed pointer、aggregate 三类元素的头部、中间和尾部 insert；新增托管引用恰好 retain 一次，原有移动元素 RC 不变。
- [x] 删除头部、中间、尾部、全部区间以及 `count == 0`。
- [x] 删除后保留元素顺序正确；每个被删除托管引用恰好 release 一次，保留移动元素 RC 不变。
- [x] migrate 到更大、更小、相等和零 `capacity` 的新数组实例，并覆盖截断有效元素的场景。
- [x] migrate 保留元素不产生元素级 RC 增减，截断元素恰好 release 一次。
- [x] 数组终结和 cycle collector 只遍历当前 `length`。
- [x] 非法负数、越界范围、满容量 insert 和 allocation 溢出按 contract panic。
- [x] `@runtime` 泛型声明、参数 carrier、数组返回值及赋值接管 `+1` 返回引用的 codegen 回归。
- [x] 补齐用例进行验证，并进行全量回归测试。

  验证顺序：

  ```sh
  make runtime
  ./build/bin/test_runtime
  make test
  make test-sanitize
  ```

- [x] 人工 Review。

不得把测试产物放到 `/tmp` 或 `/private/tmp` 后执行。

## 9. 第二阶段：优化 `List<T>`

### 9.1 实施

- [x] 第一阶段独立通过全量回归后，开始第二阶段。
- [x] 实施前再次 Review `List<T>` 的具体改造方案。

拟使用关系：

- [x] `List.add` 使用 `get_capacity + migrate（必要时）+ insert(length, item)`。
- [x] `List.insert(index, item)` 使用 `get_capacity + migrate（必要时）+ insert(index, item)`，只插入单个元素。
- [x] `List.remove` 使用 `remove(index, 1) + migrate（满足缩容策略时）`。
- [x] `List.clear` 使用 `remove(0, length)`。
- [x] `List.size` 使用 `items.length()`。

第二阶段已确定的实施项与备注：

- [x] `List(items: T[!])` 直接保存 `items.clone()` 返回的可写副本作为私有 backing array，不直接保存调用方数组引用；数组 `clone` 的返回可写性由 `docs/specifications/feng-std-array.md` 统一定义。
- [x] 删除 `List.count` 字段，统一使用 `items.length()` 表示元素数量。
- [x] 自动 shrink 沿用现有逻辑：容量大于 8 且元素数量不超过容量的四分之一时，将容量减半且不低于 8。
- [x] runtime storage contract 保持现有四个 API，不增加批量 insert 或其他 range API。

备注：`ListIterator<T>` 保持当前实现，本阶段不同步优化；后续根据实际需求单独评估。

第二阶段验证与验收：

- [x] 补齐用例进行验证，并进行全量回归测试。
- [x] 人工 Review。

## 10. Review 清单

本轮 Review 需要确认以下决策后才能开始实现：

1. `FengArray` 增加 `capacity`；普通数组始终以 `length == capacity` 创建，且任意数组实例的 `capacity` 创建后不可变。
2. `[length, capacity)` 明确定义为未初始化、不可观察、无生命周期的存储。
3. 四个 API 的名称与签名是否固定为 `feng_array_storage_*`。
4. `remove(index, count)` 是否作为 clear、单项删除与批量删除的统一原语。
5. `migrate` 是否要求旧 backing array 无仍按普通数组语义观察它的外部引用、迁移后立即用返回的新数组替换它，并且不提供共享复制 fallback。
6. runtime 不对 storage mutation 执行唯一引用检查；独占性由标准库实现保证。
7. 第一阶段只实现 API，第二阶段再修改 List。
