# 泛型复合类型实例创建修复方案

> 状态：待审批
> 日期：2026-06-07
> 关联规范：[dev/feng-generics-aggregate-optimize.md](./feng-generics-aggregate-optimize.md)、[dev/feng-value-model-delivered.md](./feng-value-model-delivered.md)

## 1. 问题

### 1.1 现象

```
feng: panic: feng_aggregate: unknown forwarded slot kind 6 in 'JsonPayload'
```

### 1.2 根因

共享泛型方法体中，含泛型参数字段的复合类型的 C struct 为擦除形态——每个泛型参数字段为 `void*`（8 字节）。当泛型参数具体化为 by-value aggregate（如 `JsonPayload` = 40 字节）时，`sizeof(擦除 struct)` 不等于实际实例大小，导致内存截断和数据损坏。

### 1.3 问题本质

**实例创建时使用了错误的大小**。不涉及生命周期管理变更——单个泛型字段的 retain/release/assign 已由 `desc->kind` switch 分派正确处理。

### 1.4 泛型类型分类

1. **type**（用户定义类型）→ 必然是托管对象，共享体中可能创建实例
2. **数组** → 必然是托管对象
3. **元组** → 可能是 Trivial 也可能是 Aggregate；含泛型字段时统一生成 `FengAggregateDescriptor`（0 个 managed slot 兼容 trivial）
4. 方案必须面向所有含泛型字段的类型通用

### 1.5 受影响的代码路径（`src/codegen/codegen.c`）

1. **数组创建**（~L15448）：`feng_array_new(NULL, sizeof(擦除struct), false, n)` — element_size 错误
2. **Tuple 局部变量**（~L9667）：`struct Erased _tuple; memset(...)` — 栈变量太小
3. **数组元素写入**（~L18376）：`((ErasedT*)data)[idx] = value` — stride 错误
4. **数组元素读取 / for-in**（~L20526）：stride 错误
5. **Wrapper _erased_buf**（~L28443）：接收缓冲区太小
6. **托管对象创建**：共享体中若创建含泛型字段的托管对象，分配大小错误

## 2. 设计方案

### 2.1 核心思路

在已有的类型描述符（`FengTypeDescriptor`、`FengAggregateDescriptor`）上增加 `generic_slots` 字段，记录哪些字段是泛型参数。Runtime 函数接收「描述符 + 泛型参数描述符数组」，计算正确的实例大小和具体化的生命周期描述符。

不新增描述符类型。生命周期三大类不变。Aggregate walker 不变。

### 2.2 扩展已有描述符

#### 新增辅助类型

```c
/* 描述类型中一个泛型参数字段的位置。
 * 由 codegen 生成为 static const。*/
typedef struct FengGenericSlot {
    size_t offset;               /* 字段在擦除 struct 中的 offsetof */
    size_t generic_param_index;  /* 对应第几个泛型参数 */
} FengGenericSlot;
```

#### 扩展 `FengTypeDescriptor`（[feng_runtime.h:71](src/runtime/feng_runtime.h)）

```c
typedef struct FengTypeDescriptor {
    /* ... 已有字段不变 ... */
    size_t generic_slot_count;              /* 0 = 无泛型字段 */
    const FengGenericSlot *generic_slots;   /* NULL when count == 0 */
} FengTypeDescriptor;
```

#### 扩展 `FengAggregateDescriptor`（[feng_runtime.h:316](src/runtime/feng_runtime.h)）

```c
typedef struct FengAggregateDescriptor {
    /* ... 已有字段不变 ... */
    size_t generic_slot_count;              /* 0 = 无泛型字段 */
    const FengGenericSlot *generic_slots;   /* NULL when count == 0 */
} FengAggregateDescriptor;
```

#### `FengTrivialDescriptor` 不变

含泛型字段的元组统一生成 `FengAggregateDescriptor`（Aggregate 兼容 Trivial），因此 `FengTrivialDescriptor` 无需扩展。

### 2.3 Codegen 变更

#### 2.3.1 含泛型字段的元组统一生成 `FengAggregateDescriptor`

当前 `cg_emit_tuple_type_definition()`（~L27179）中，如果 `slot_count == 0` 则生成 `FengTrivialDescriptor`。修改为：当类型含泛型参数字段时，无论 slot_count 是否为 0，都生成 `FengAggregateDescriptor`。

#### 2.3.2 生成 `generic_slots`

在描述符生成时，遍历类型字段，为每个 `CG_TYPE_GENERIC_PARAM` 字段生成一条 `FengGenericSlot`：

```c
/* 示例：MapEntity<K, V> */
static const FengGenericSlot MapEntity__G__K__V__gslots[] = {
    { offsetof(struct MapEntity__G__K__V, item1), 0 },
    { offsetof(struct MapEntity__G__K__V, item2), 1 },
};
static const FengAggregateDescriptor MapEntity__G__K__V__desc = {
    .name = "MapEntity",
    .size = sizeof(struct MapEntity__G__K__V),  /* 擦除大小 */
    .default_init = ...,
    .managed_slot_count = 0,
    .managed_slots = NULL,
    .equal_fn = ...,
    .generic_slot_count = 2,
    .generic_slots = MapEntity__G__K__V__gslots,
};
```

对于 `FengTypeDescriptor`（托管对象），同理在生成时填入 `generic_slots`。

#### 2.3.3 修复数组创建（~L15448）

```c
const FengGenericParamDescriptor *_gp[] = { _K, _V };
FengArray *_arr = feng_array_new_generic(
    &MapEntity__G__K__V__desc, _gp, 2, _n);
```

#### 2.3.4 修复 Tuple 局部变量（~L9667）

```c
const FengGenericParamDescriptor *_gp[] = { _K, _V };
size_t _tsz = feng_generic_instance_size(
    &MapEntity__G__K__V__desc, _gp, 2);
_Alignas(8) char _tmem[_tsz];
memset(_tmem, 0, _tsz);
struct MapEntity__G__K__V *_tuple = (struct MapEntity__G__K__V *)_tmem;
```

#### 2.3.5 修复数组元素写入（~L18376）

```c
size_t _esz = feng_generic_instance_size(&desc, _gp, 2);
void *_slot = (char *)feng_array_data(_arr) + _idx * _esz;
memcpy(_slot, _tuple, _esz);
```

#### 2.3.6 修复数组元素读取 / for-in（~L20526）

```c
size_t _esz = feng_generic_instance_size(&desc, _gp, 2);
void *_elem = (char *)feng_array_data(_arr) + _fidx * _esz;
memcpy(_iter_mem, _elem, _esz);
```

#### 2.3.7 修复 Wrapper `_erased_buf`（~L28443）

字段偏移在擦除和具体化 struct 间一致（padding 机制保证）。直接传 `&ret_tmp`（具体化类型，大小正确），去掉 `_erased_buf`。

### 2.4 新增 Runtime 函数

```c
/* 计算含泛型字段的类型的实际实例大小。
 * 遍历 generic_slots，取 max(slot.offset + feng_generic_value_size(params[slot.index]))，
 * 与描述符的 size（擦除大小）取 max。
 * generic_slot_count == 0 时直接返回 desc->size。 */
size_t feng_generic_instance_size(
    const FengAggregateDescriptor *desc,
    const FengGenericParamDescriptor *const *params,
    size_t param_count);

/* 创建元素为含泛型字段类型的数组。
 * 1. 计算实际 element_size
 * 2. 遍历 generic_slots，根据 params[i]->kind 确定每个泛型字段的 managed slot kind
 * 3. 合并描述符已有的 managed_slots + 泛型字段的实际 slots，
 *    构建完全解析后的标准 FengAggregateDescriptor（堆分配，数组持有）
 * 4. 调用 feng_array_new_kinded(AGGREGATE, resolved_desc, ...) */
FengArray *feng_array_new_generic(
    const FengAggregateDescriptor *desc,
    const FengGenericParamDescriptor *const *params,
    size_t param_count,
    size_t length);
```

### 2.5 数组描述符生命周期

`feng_array_new_generic` 在创建数组时，根据 `generic_slots` + `params` 构建一个**完全解析后的标准 `FengAggregateDescriptor`**（所有 slot kind 为 NONE/POINTER/NESTED_AGGREGATE，size 为实际大小）。堆分配，数组持有。

`struct FengArray` 增加 `bool owns_element_aggregate`。`feng_array_finalize_internal` 中，若 `owns_element_aggregate` 为 true，释放描述符。

已有的 aggregate walker 看到的是标准描述符，无任何变化。

## 3. 不变量

1. **生命周期三大类不变**
2. **Aggregate walker 不变**
3. **已有代码路径不变**：`generic_slot_count == 0` 的描述符行为完全等同于原先
4. **描述符结构向后兼容**：新增字段在末尾，已有初始化代码中 `generic_slot_count` 默认为 0

## 4. 涉及文件

| 文件 | 变更 |
|------|------|
| `src/runtime/feng_runtime.h` | `FengGenericSlot` 类型；`FengTypeDescriptor`、`FengAggregateDescriptor` 增加 `generic_slot_count`、`generic_slots`；新增函数声明 |
| `src/runtime/feng_runtime_internal.h` | `struct FengArray` 增加 `bool owns_element_aggregate` |
| `src/runtime/feng_generic_instance.c`（新） | 实现 `feng_generic_instance_size`、`feng_array_new_generic` |
| `src/runtime/feng_array.c` | `feng_array_finalize_internal` 增加 `owns_element_aggregate` 释放 |
| `src/codegen/codegen.c` | 含泛型字段元组统一生成 Aggregate；生成 `generic_slots`；修复 6 个创建路径 |

## 5. 验证

```bash
feng run examples/hello_world --keep-ir
# 预期: START / OBJ_CREATED / OBJ_SET / JV_CREATED / JV: {"a":1}

feng test
# 全量回归
```
