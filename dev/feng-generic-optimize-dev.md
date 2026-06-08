# 泛型复合类型实例创建修复方案

> 状态：待审批
> 日期：2026-06-08
> 关联规范：[dev/feng-generics-aggregate-optimize.md](./feng-generics-aggregate-optimize.md)、[dev/feng-value-model-delivered.md](./feng-value-model-delivered.md)

## 1. 问题

### 1.1 现象

```
feng: panic: feng_aggregate: unknown forwarded slot kind 6 in 'JsonPayload'
```

### 1.2 根因

共享泛型方法体中，含泛型参数字段的复合类型的 C struct 为擦除形态——每个泛型参数字段为 `void*`（8 字节）。当泛型参数具体化为 by-value aggregate（如 `JsonPayload` = 40 字节）时，`sizeof(未特化 struct)` 不等于实际实例大小，导致内存截断和数据损坏。

### 1.3 问题本质

共享体中缺乏含泛型字段的复合类型的**具体化描述符**，导致两个相互关联的问题：

**问题一：大小错误**。实例创建、栈分配、数组 stride 均使用未特化大小（`sizeof(未特化 struct)`），当泛型参数为 by-value aggregate 时偏小，导致内存截断。

**问题二：生命周期描述符缺失**。含泛型字段的复合类型（如 `MapEntity<K,V>`）作为数组元素或局部变量时，共享体只有未特化描述符（`managed_slots = NULL`，偏移基于未特化布局），无法正确驱动 retain/release，导致内存泄漏或 UAF。

两个问题同根同源：共享体无法获得与具体化类型参数对应的正确描述符。

### 1.4 为何共享体无法持有具体化描述符

共享体编译时 K/V 未知，调用点（Wrapper）编译时 K/V 具体化，但共享体已编译为二进制——Wrapper 无法将静态生成的具体化描述符"注入"进已编译的共享体。这是二进制分发 + 泛型共享体模型的根本约束，与 Swift 的 `swift_initStructMetadata` 面临的约束相同。

### 1.5 受影响的代码路径（`src/codegen/codegen.c`）

1. **数组创建**（~L15448）：`feng_array_new(NULL, sizeof(未特化struct), false, n)` — element_size 错误，element_aggregate 使用未特化描述符
2. **Tuple 局部变量**（~L9667）：`struct Erased _tuple; memset(...)` — 栈变量太小
3. **数组元素写入**（~L18376）：`((ErasedT*)data)[idx] = value` — stride 错误
4. **数组元素读取 / for-in**（~L20526）：stride 错误
5. **Wrapper `_erased_buf`**（~L28443）：接收缓冲区太小
6. **托管对象创建**：共享体中若创建含泛型字段的托管对象，分配大小错误

## 2. 设计方案

### 2.1 核心思路：编译期描述符树

**Wrapper（调用点）在编译时已知所有具体类型**，因此由 Wrapper 静态生成目标泛型类型及其方法体中所有含泛型字段的复合类型的具体化描述符，形成描述符树（全在 `.rodata`，零运行时开销）。

描述符之间通过 `deps[]` 字段链式引用，共享体通过接收上下文描述符并访问 `ctx->deps[i]` 获取所需的具体化描述符。

**链式特化**（`Foo<T1,T2>` → `Bar<int,T2>` → `Xyz<T2>`）：Wrapper 在特化 `Foo<T1,T2>` 时发现需要 `Bar<int,T2>`，继而发现需要 `Xyz<T2>`，递归完成所有依赖的特化。各层描述符通过 `deps[]` 相互引用，共享体逐级访问。

不新增描述符类型。生命周期三大类不变。Aggregate walker 不变。不新增 runtime 函数。

### 2.2 扩展已有描述符

在 `FengAggregateDescriptor` 和 `FengTypeDescriptor` 各新增两个字段：

```c
typedef struct FengAggregateDescriptor {
    /* ... 已有字段不变 ... */
    size_t dep_count;                                         /* 0 = 无依赖 */
    const struct FengAggregateDescriptor *const *deps;        /* NULL when count == 0 */
} FengAggregateDescriptor;

typedef struct FengTypeDescriptor {
    /* ... 已有字段不变 ... */
    size_t dep_count;                                         /* 0 = 无依赖 */
    const FengAggregateDescriptor *const *deps;               /* NULL when count == 0 */
} FengTypeDescriptor;
```

`deps[i]` 是具体化后的标准 `FengAggregateDescriptor`（`size`、`managed_slots` 全部为具体化正确值，可直接传入 `feng_array_new_kinded` 等接口）。`deps` 的索引顺序在 codegen 生成共享体时确定，所有 Wrapper 严格遵循同一顺序——这是编译期约束，运行时无需校验。

### 2.3 Wrapper 静态生成具体化描述符树

Wrapper 在编译时按以下步骤生成：

1. 遍历共享体内使用的所有含泛型字段的复合类型，为每个类型生成具体化 `FengAggregateDescriptor`（含正确的 `size` 和 `managed_slots`）
2. 若某个复合类型本身依赖其他泛型复合类型，递归生成并通过 `deps[]` 链式引用（从叶到根）
3. 为该共享体调用生成一个**函数上下文描述符**（`FengAggregateDescriptor`，`size=0`，`managed_slots=NULL`），`deps[]` 按顺序引用所有具体化描述符

**示例**（`func process<K, V>()` 内使用 `MapEntity<K, V>`，`K=Foo`（托管），`V=Bar`（aggregate））：

```c
/* MapEntity<Foo, Bar> 的具体化 managed slots（具体化偏移和 kind） */
static const FengManagedSlotDescriptor MapEntity__K_Foo__V_Bar__slots[] = {
    { offsetof(struct MapEntity__K_Foo__V_Bar, item1), FENG_SLOT_POINTER, NULL },
    { offsetof(struct MapEntity__K_Foo__V_Bar, item2), FENG_SLOT_NESTED_AGGREGATE, &Bar__desc },
};
/* MapEntity<Foo, Bar> 具体化描述符 */
static const FengAggregateDescriptor MapEntity__K_Foo__V_Bar__desc = {
    .name = "MapEntity",
    .size = sizeof(struct MapEntity__K_Foo__V_Bar),  /* 具体化正确大小 */
    .managed_slot_count = 2,
    .managed_slots = MapEntity__K_Foo__V_Bar__slots,
    .dep_count = 0,
    .deps = NULL,
};
/* process<Foo, Bar> 的函数上下文描述符，deps[0] = MapEntity<Foo,Bar> */
static const FengAggregateDescriptor *process__K_Foo__V_Bar__deps[] = {
    &MapEntity__K_Foo__V_Bar__desc,
};
static const FengAggregateDescriptor process__K_Foo__V_Bar__ctx = {
    .name = "process",
    .size = 0,
    .managed_slot_count = 0,
    .managed_slots = NULL,
    .dep_count = 1,
    .deps = process__K_Foo__V_Bar__deps,
};
```

**链式依赖示例**（`Foo<T1,T2>` → `Bar<int,T2>` → `Xyz<T2>`，从叶到根生成）：

```c
static const FengAggregateDescriptor Xyz__T2c__desc        = { /* 具体化 */ };
static const FengAggregateDescriptor *Bar__deps[]          = { &Xyz__T2c__desc };
static const FengAggregateDescriptor Bar__int__T2c__desc   = { ..., .deps = Bar__deps };
static const FengAggregateDescriptor *Foo__ctx__deps[]     = { &Bar__int__T2c__desc };
static const FengAggregateDescriptor Foo__ctx              = { ..., .deps = Foo__ctx__deps };
```

对于**类型方法**（type 的成员 func），该 type 的具体化 `FengTypeDescriptor` 本身即携带 `deps[]`，直接作为上下文，无需额外生成函数上下文描述符。

### 2.4 共享体 ABI 变更

共享体新增上下文描述符参数 `_ctx`：

```c
/* 旧签名 */
void process__G__K__V(FengGenericParamDescriptor *_K, FengGenericParamDescriptor *_V, ...)

/* 新签名 */
void process__G__K__V(FengGenericParamDescriptor *_K, FengGenericParamDescriptor *_V,
                      const FengAggregateDescriptor *_ctx, ...)
```

`_K`、`_V` 保留，用于直接泛型参数字段的 retain/release 和大小（基础类型 kind/size）。`_ctx` 用于访问方法体中含泛型字段的复合类型的具体化描述符。

### 2.5 共享体内使用 `deps`

#### 2.5.1 修复数组创建（~L15448）

```c
/* _ctx->deps[0] 是 MapEntity<K,V> 的具体化描述符，size 和 managed_slots 均正确 */
const FengAggregateDescriptor *_ed = _ctx->deps[0];
FengArray *_arr = feng_array_new_kinded(
    FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, _ed, NULL, _ed->size, _n);
```

#### 2.5.2 修复 Tuple 局部变量（~L9667）

```c
const FengAggregateDescriptor *_ed = _ctx->deps[0];
_Alignas(max_align_t) char _tmem[_ed->size];  /* VLA，大小来自具体化描述符 */
feng_aggregate_default_init(_tmem, _ed);
struct MapEntity__G__K__V *_tuple = (struct MapEntity__G__K__V *)_tmem;
```

#### 2.5.3 修复数组元素写入（~L18376）

```c
const FengAggregateDescriptor *_ed = _ctx->deps[0];
void *_slot = (char *)feng_array_data(_arr) + _idx * _ed->size;
feng_aggregate_assign(_slot, _tuple, _ed);
```

#### 2.5.4 修复数组元素读取 / for-in（~L20526）

```c
const FengAggregateDescriptor *_ed = _ctx->deps[0];
void *_elem = (char *)feng_array_data(_arr) + _fidx * _ed->size;
feng_aggregate_assign(_iter_mem, _elem, _ed);
```

#### 2.5.5 修复 Wrapper `_erased_buf`（~L28443）

字段偏移在未特化和具体化 struct 间一致（padding 机制保证）。直接传 `&ret_tmp`（具体化类型，大小正确），去掉 `_erased_buf`。

#### 2.5.6 修复托管对象创建

对于类型方法，`_ctx` 即为该 type 的具体化描述符，`_ctx->size` 已是正确大小，直接使用：

```c
/* type 方法场景：_ctx 即为 BoxedNode<T> 的具体化 FengTypeDescriptor */
FengObject *_obj = feng_obj_alloc(_ctx->size, ...);
```

对于非类型方法中创建含泛型字段的托管对象，通过 `_ctx->deps[i]` 获取该对象类型的具体化描述符后使用其 `size`。

### 2.6 不新增 Runtime 函数

本方案不引入任何新的 runtime 函数和新的 runtime 源文件：

- `feng_generic_aggregate_instance_size`：不需要（大小直接来自 `desc->size`）
- `feng_generic_type_instance_size`：不需要
- `feng_generic_desc_resolve` / 全局缓存：不需要（全部静态，`.rodata`）
- `feng_generic_array_new`：不需要（直接调用 `feng_array_new_kinded`）

## 3. 不变量

1. **生命周期三大类不变**
2. **Aggregate walker 不变**
3. **已有非泛型代码路径不变**：`dep_count == 0` 的描述符行为完全等同于原先
4. **描述符结构向后兼容**：新增字段在末尾，已有初始化代码中 `dep_count` 默认为 0
5. **零运行时开销**：所有具体化描述符均为 `static const`，全在 `.rodata`

## 4. 涉及文件

| 文件 | 变更 |
|------|------|
| `src/runtime/feng_runtime.h` | `FengAggregateDescriptor`、`FengTypeDescriptor` 增加 `dep_count`、`deps` 字段 |
| `src/codegen/codegen.c` | Wrapper 生成具体化描述符树；共享体 ABI 增加 `_ctx` 参数；修复 6 个创建路径（2.5.1–2.5.6） |

## 5. 验证

```bash
feng run examples/hello_world --keep-ir
# 预期: START / OBJ_CREATED / OBJ_SET / JV_CREATED / JV: {"a":1}

feng test
# 全量回归
```
