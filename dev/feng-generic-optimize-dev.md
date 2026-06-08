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

**Wrapper（调用点）在编译时已知所有具体类型**，因此由 Wrapper 静态生成具体化描述符（全在 `.rodata`，零运行时开销）。共享体通过统一的"自身具体化描述符"在运行时访问所有依赖。

**路径一：类型实例方法（有 `self`）**

`self->_hdr.desc` 即是当前实例的具体化 `FengTypeDescriptor`，永远可靠。**共享体不新增任何额外参数**；所有大小、描述符均在运行时从 `self->_hdr.desc->agg_deps[i]->size` / `->type_deps[i]->size` 读取。

**路径二：类型静态方法（无 `self`）**

Wrapper 将具体化 `FengTypeDescriptor*` 作为额外参数 `_type_desc` 传入共享体，共享体通过 `_type_desc->agg_deps[i]` / `->type_deps[i]` 访问依赖。

`FengTypeDescriptor.agg_deps[]` 和 `type_deps[]` 的索引在整个类型范围内**全局稳定**：codegen 收集该类型所有方法（实例方法 + 静态方法）中全部依赖（成员字段 + 各方法体局部依赖），按依赖排序 key（见 §2.2）字典序升序分配全局唯一索引，所有方法共享同一 `FengTypeDescriptor`，各方法按编译期确定的固定索引访问各自所需的描述符。

**路径三：独立泛型函数（无 `self`）**

Wrapper 为本次具体化静态生成一个 **`FengFunctionDescriptor`**（`static const`），传入共享体时命名为 `_desc`。共享体通过 `_desc->agg_deps[i]` / `->type_deps[i]` 在运行时访问各依赖。

三条路径形式完全统一：`自身具体化描述符->agg_deps[i]` / `->type_deps[i]`，区别仅在"自身具体化描述符"的来源。

新增 `FengFunctionDescriptor` 描述独立泛型函数的具体化依赖（详见 §2.2）。生命周期三大类不变。Aggregate walker 不变。不新增 runtime 函数。

### 2.2 描述符结构

#### `FengFunctionDescriptor`（新增）

独立泛型函数专用的具体化描述符，Wrapper 静态生成，传入共享体时命名 `_desc`：

```c
/* 新增：独立泛型函数具体化描述符 */
typedef struct FengFunctionDescriptor {
    const char *name;
    /* 本次调用中直接使用的各具体化 aggregate（tuple）类型的描述符，
     * 按排序 key（见下方"依赖排序 key 规则"）字典序升序排列。
     * [约束] 严禁用于生命周期管理（retain/release/destroy）；唯一用途是在运行时
     *        找到具体化描述符，完成大小计算、栈分配、数组 stride、创建实例等操作。 */
    size_t agg_deps_count;
    const FengAggregateDescriptor *const *agg_deps;
    /* 本次调用中需要创建实例的各具体化 managed（type）类型的描述符，
     * 按排序 key 字典序升序排列。
     * [约束] 严禁用于生命周期管理（retain/release/destroy）；唯一用途是在运行时
     *        找到具体化描述符以创建实例。生命周期管理由 ARC 指针操作负责，与此无关。 */
    size_t type_deps_count;
    const FengTypeDescriptor *const *type_deps;
} FengFunctionDescriptor;
```

#### `FengAggregateDescriptor`（扩展，字段暂未启用）

```c
typedef struct FengAggregateDescriptor {
    /* ... 已有字段不变 ... */

    /* 保留字段，当前版本始终为 0 / NULL。
     * 预留给未来非托管结构体（non-managed struct）场景：
     * 若非托管 struct 内含泛型 aggregate 字段且无法通过 managed_slots.nested 表达，
     * 届时启用这两组字段，语义与 FengTypeDescriptor 上的同名字段一致。
     * 当前所有 aggregate 类型的内部嵌套依赖均由 managed_slots[i].nested 链式引用。
     * [约束] 届时同样严禁用于生命周期管理（retain/release/destroy），
     *        唯一用途是在运行时找到具体化描述符，完成大小计算等操作。 */
    size_t agg_deps_count;                                       /* 当前恒为 0 */
    const struct FengAggregateDescriptor *const *agg_deps;       /* 当前恒为 NULL */
    size_t type_deps_count;                                      /* 当前恒为 0 */
    const struct FengTypeDescriptor *const *type_deps;           /* 当前恒为 NULL */
} FengAggregateDescriptor;
```

#### `FengTypeDescriptor`（扩展）

```c
typedef struct FengTypeDescriptor {
    /* ... 已有字段不变 ... */

    /* 具体化泛型类型的全局依赖（所有方法共享，索引全局稳定）：
     * agg_deps: 该类型所有方法中使用的具体化 aggregate（tuple）类型的描述符，
     *           包含成员字段的 aggregate 依赖和各方法体内局部的 aggregate 依赖
     * type_deps: 该类型所有方法中需要创建新实例的具体化 managed（type）类型的描述符；
     *            托管成员字段的 ARC（retain/release）操作无需此描述符，仅"创建新实例"场景使用
     * 索引规则：按排序 key（见下方"依赖排序 key 规则"）字典序升序在 codegen 阶段全局分配，
     *           同类型不同方法访问各自所需索引，索引语义跨方法一致不变。
     * [约束] agg_deps/type_deps 严禁用于生命周期管理（retain/release/destroy）；
     *        唯一用途是声明静态描述符间依赖关系，使方法和成员在运行时正确找到具体化描述符，
     *        完成大小计算、栈分配、数组 stride、创建实例等操作。
     *        生命周期管理由 managed_slots（FengAggregateDescriptor）和 ARC 指针操作负责，与此无关。 */
    size_t agg_deps_count;
    const struct FengAggregateDescriptor *const *agg_deps;
    size_t type_deps_count;
    const struct FengTypeDescriptor *const *type_deps;
} FengTypeDescriptor;
```

访问方式对比：

| 场景 | 自身具体化描述符 | aggregate 依赖 | managed 依赖 |
|------|------|------|------|
| 类型方法（有 `self`） | `self->_hdr.desc`（`FengTypeDescriptor*`） | `->agg_deps[i]` | `->type_deps[i]` |
| 类型静态方法（无 `self`） | `_type_desc`（`FengTypeDescriptor*`，Wrapper 传入） | `->agg_deps[i]` | `->type_deps[i]` |
| 独立泛型函数（无 `self`） | `_desc`（`FengFunctionDescriptor*`） | `->agg_deps[i]` | `->type_deps[i]` |

> **重要约束**：`agg_deps`/`type_deps` 两组字段**严禁直接用于生命周期管理**（retain/release/destroy）。它们的唯一用途是声明静态描述符之间的依赖关系，使类型的方法和成员能在运行时正确找到并使用相应具体化描述符，完成大小计算、栈分配、数组 stride、创建实例等操作。生命周期管理仍由 `managed_slots`（`FengAggregateDescriptor`）和 ARC 指针操作（`FengTypeDescriptor`）负责，与这两个字段无关。

#### 依赖排序 key 规则

`agg_deps[]`/`type_deps[]` 的索引按各依赖类型的**排序 key** 字典序升序分配。

排序 key 的生成规则（递归定义）：

- **根模式**（作为排序 key 的顶层）：`TypeName__p1__p2__...`，参数间以 `__`（双下划线）分隔
- **参数模式**（作为另一个类型参数出现时）：`TypeName_p1_p2_...`，参数间以 `_`（单下划线）分隔
- 每个参数 `pi` 递归以**参数模式**展开
- **`UserType` 自身的泛型参数**（即当前类型/函数声明中的类型参数）以其在声明列表中的**0-based 索引**表示为 `T0`、`T1`、`T2`……，不使用参数名（防止仅因重命名泛型参数导致排序 key 变化而破坏兼容性）

示例（`UserType<K,V>` 内，`K` 为 `T0`、`V` 为 `T1`，依赖 `Foo<int,K>`、`Bar<V>`、`Xyz<Foo<int,K>,Bar<V>>`）：

| 类型 | 泛型参数替换 | 排序 key（根模式） |
|------|------|------|
| `Foo<int,K>` | `K` → `T0` | `Foo__int__T0` |
| `Bar<V>` | `V` → `T1` | `Bar__T1` |
| `Xyz<Foo<int,K>,Bar<V>>` | `K` → `T0`，`V` → `T1` | `Xyz__Foo_int_T0__Bar_T1` |

`Foo<int,K>` 作为 `Xyz` 的参数时展开为参数模式 `Foo_int_T0`；`Bar<V>` 展开为 `Bar_T1`。

三者按字典序：`Bar__T1` < `Foo__int__T0` < `Xyz__Foo_int_T0__Bar_T1`，分别占 `agg_deps[0]`、`agg_deps[1]`、`agg_deps[2]`。

### 2.3 Wrapper 静态生成具体化描述符

Wrapper 在编译时按以下步骤生成：

1. 遍历共享体内所有含泛型字段的 aggregate/managed 类型（成员字段 + 方法体局部使用），代入具体类型参数，为每个类型生成具体化描述符
2. 生成某 aggregate 描述符的 `managed_slots` 时，若某字段的类型仍含未特化泛型参数，则触发链式物化：递归对该类型执行步骤 1，直到所有字段均为非泛型叶子类型（类型图无环，DFS 必然终止）；内层描述符直接作为外层 `managed_slots` 中对应 slot 的 `nested` 指针（不经过 `agg_deps`/`type_deps`）
3. 按依赖排序 key（见 §2.2）字典序升序为所有依赖分配**全局稳定索引**，填入 `agg_deps[]` / `type_deps[]`

上述步骤均为**编译期逻辑**：静态描述符本身及描述符间的依赖关系，在同包内依赖 AST 生成，跨包依赖符号表（ft，见 §2.7）生成；生成结果以 `static const` 形式写入目标文件，运行时只读取，不动态构造。

**链式物化无需特殊检测**：触发条件就是"生成某个 `FENG_SLOT_NESTED_AGGREGATE` 的 `nested` 指针时，该字段的类型仍含泛型参数"，codegen 在生成每个字段 slot 时递归走该逻辑即可。

#### 场景一：独立泛型函数

**示例**（`func process<K, V>()` 内直接使用 `MapEntity<K,V>`，`K=Foo`（托管），`V=Bar`（aggregate））：

```c
/* MapEntity<Foo, Bar> 的具体化 managed slots */
static const FengManagedSlotDescriptor MapEntity__K_Foo__V_Bar__slots[] = {
    { offsetof(struct MapEntity__K_Foo__V_Bar, item1), FENG_SLOT_POINTER, NULL },
    { offsetof(struct MapEntity__K_Foo__V_Bar, item2), FENG_SLOT_NESTED_AGGREGATE, &Bar__desc },
};
/* MapEntity<Foo, Bar> 具体化描述符（agg/type_deps 保留字段当前恒为 0/NULL） */
static const FengAggregateDescriptor MapEntity__K_Foo__V_Bar__desc = {
    .name = "MapEntity", .size = sizeof(struct MapEntity__K_Foo__V_Bar),
    .managed_slot_count = 2, .managed_slots = MapEntity__K_Foo__V_Bar__slots,
};
/* process<Foo, Bar> 函数具体化描述符，agg_deps[0] = MapEntity<Foo,Bar> */
static const FengAggregateDescriptor *process__K_Foo__V_Bar__agg_deps[] = {
    &MapEntity__K_Foo__V_Bar__desc,
};
static const FengFunctionDescriptor process__K_Foo__V_Bar__desc = {
    .name = "process",
    .agg_deps_count = 1, .agg_deps = process__K_Foo__V_Bar__agg_deps,
    .type_deps_count = 0, .type_deps = NULL,
};
/* 调用共享体：传入 _K、_V、_desc */
process__G__K__V(_K_desc, _V_desc, &process__K_Foo__V_Bar__desc, ...);
```

共享体内访问：`_desc->agg_deps[0]->size` 在运行时给出 `MapEntity<Foo,Bar>` 的正确大小。

#### 场景二：泛型类型的方法

**示例**（`type Container<K,V>` 有成员字段 `entry: Foo<int,V>`（tuple），方法体内局部使用 `Boo<K>`（tuple））：

```c
/* 具体化 aggregate 描述符 */
static const FengAggregateDescriptor Boo__K_Foo__desc = {
    .name = "Boo", .size = sizeof(struct Boo__K_Foo),
    .managed_slot_count = ..., .managed_slots = ...,
};
static const FengAggregateDescriptor Foo__int__Bar__desc = {
    .name = "Foo", .size = sizeof(struct Foo__int__Bar),
    .managed_slot_count = ..., .managed_slots = ...,
};
/* Container<Foo,Bar> 的 FengTypeDescriptor：
 * agg_deps 按全局稳定排序 key 顺序覆盖该类型所有方法（实例方法+静态方法）的 aggregate 依赖 */
static const FengAggregateDescriptor *Container__K_Foo__V_Bar__agg_deps[] = {
    &Boo__K_Foo__desc,      /* agg_deps[0]：Boo<K>（按类型名排序在 Foo 之前） */
    &Foo__int__Bar__desc,   /* agg_deps[1]：成员字段 entry: Foo<int,V> */
};
static const FengTypeDescriptor Container__K_Foo__V_Bar__type_desc = {
    .name = "Container", .size = sizeof(struct Container__K_Foo__V_Bar),
    /* ... 其他字段 ... */
    .agg_deps_count = 2, .agg_deps = Container__K_Foo__V_Bar__agg_deps,
    .type_deps_count = 0, .type_deps = NULL,
};
/* 调用方法共享体：无额外描述符参数 */
Container_method__G__K__V(self, _K_desc, _V_desc, ...);
```

方法共享体内访问（索引由 codegen 在编译期按全局稳定顺序确定）：
```c
/* 方法体内局部 Boo<K>（tuple）：agg_deps[0]，大小在运行时读取 */
const FengAggregateDescriptor *_boo_desc = self->_hdr.desc->agg_deps[0];
_Alignas(max_align_t) char _boo_mem[_boo_desc->size];
feng_aggregate_default_init(_boo_mem, _boo_desc);

/* 成员字段 entry（Foo<int,V> 是 tuple）：agg_deps[1] */
const FengAggregateDescriptor *_entry_desc = self->_hdr.desc->agg_deps[1];
feng_aggregate_assign(&self->entry, &new_entry, _entry_desc);
```

**成员字段为托管对象（type）时**：ARC 操作（retain/release）无需描述符；若需在方法体内**创建该托管类型新实例**，则通过 `self->_hdr.desc->type_deps[i]` 获取其具体化 `FengTypeDescriptor`。

**每种具体化（如 `Container<Foo,Bar>`）生成一份 `FengTypeDescriptor`**，所有方法的共享体共享该描述符，各自按编译期已知的固定索引访问。

### 2.4 共享体 ABI 变更

**独立泛型函数**新增 `_desc` 参数（`FengFunctionDescriptor*`）：

```c
/* 旧签名 */
void process__G__K__V(FengGenericParamDescriptor *_K, FengGenericParamDescriptor *_V, ...)

/* 新签名 */
void process__G__K__V(FengGenericParamDescriptor *_K, FengGenericParamDescriptor *_V,
                      const FengFunctionDescriptor *_desc, ...)
```

**泛型类型的实例方法共享体**不新增任何描述符参数，所有依赖通过 `self->_hdr.desc->agg_deps[i]` / `->type_deps[i]` 访问，`self->_hdr.desc` 已是该次调用的具体化 `FengTypeDescriptor`。

**泛型类型的静态方法共享体**没有 `self`，无法通过实例访问具体化类型描述符，新增 `_type_desc` 参数（`FengTypeDescriptor*`）：

```c
/* 旧签名 */
void Container_static_method__G__K__V(FengGenericParamDescriptor *_K,
                                       FengGenericParamDescriptor *_V, ...)
/* 新签名 */
void Container_static_method__G__K__V(FengGenericParamDescriptor *_K,
                                       FengGenericParamDescriptor *_V,
                                       const FengTypeDescriptor *_type_desc, ...)
```

静态方法共享体通过 `_type_desc->agg_deps[i]` / `->type_deps[i]` 访问依赖，`_type_desc` 由 Wrapper 在编译期从具体化 `FengTypeDescriptor` 取地址传入。

`_K`、`_V` 在三种场景中均保留，用于直接泛型参数字段的 retain/release 和大小（基础类型 kind/size）。

### 2.5 共享体内使用 `agg_deps` / `type_deps`

以下各路径中，索引 `i` 均由 codegen 在**编译期**按排序 key（见 §2.2）全局分配，不是运行时计算的值。同一共享体内若有多种泛型 aggregate 依赖，各自占不同索引位置。

#### 2.5.1 修复数组创建（~L15448）

```c
/* 独立函数：i 由 codegen 编译期确定 */
const FengAggregateDescriptor *_ed = _desc->agg_deps[i];
FengArray *_arr = feng_array_new_kinded(
    FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, _ed, NULL, _ed->size, _n);

/* 类型实例方法：i 由 codegen 编译期确定 */
const FengAggregateDescriptor *_ed = self->_hdr.desc->agg_deps[i];
FengArray *_arr = feng_array_new_kinded(
    FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, _ed, NULL, _ed->size, _n);

/* 类型静态方法：i 由 codegen 编译期确定 */
const FengAggregateDescriptor *_ed = _type_desc->agg_deps[i];
FengArray *_arr = feng_array_new_kinded(
    FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, _ed, NULL, _ed->size, _n);
```

#### 2.5.2 修复局部变量（~L9667）

```c
/* 独立函数 */
const FengAggregateDescriptor *_ed = _desc->agg_deps[i];
/* 类型实例方法 */
const FengAggregateDescriptor *_ed = self->_hdr.desc->agg_deps[i];
/* 类型静态方法 */
const FengAggregateDescriptor *_ed = _type_desc->agg_deps[i];

_Alignas(max_align_t) char _mem[_ed->size];  /* VLA，大小在运行时从具体化描述符读取 */
feng_aggregate_default_init(_mem, _ed);
```

#### 2.5.3 修复数组元素写入（~L18376）

```c
/* 独立函数 / 类型实例方法 / 类型静态方法，取 _ed 方式同 2.5.1 */
void *_slot = (char *)feng_array_data(_arr) + _idx * _ed->size;
feng_aggregate_assign(_slot, _src, _ed);
```

#### 2.5.4 修复数组元素读取 / for-in（~L20526）

```c
/* 独立函数 / 类型实例方法 / 类型静态方法，取 _ed 方式同 2.5.1 */
void *_elem = (char *)feng_array_data(_arr) + _fidx * _ed->size;
feng_aggregate_assign(_iter_mem, _elem, _ed);
```

三大路径（独立函数、实例方法、静态方法）在取到 `_ed` 后，后续操作完全一致，无特判。**tuple 与其他 by-value aggregate 均走同一路径，无需额外处理**；tuple 是否需要生命周期管理由 `_ed->managed_slots` 决定，walker 自动分派。

#### 2.5.5 修复 Wrapper `_erased_buf`（~L28443）

字段偏移在未特化和具体化 struct 间一致（padding 机制保证）。直接传 `&ret_tmp`（具体化类型，大小正确），去掉 `_erased_buf`。

#### 2.5.6 修复托管对象创建

需创建某个泛型托管类型新实例时，通过 `type_deps[i]` 获取其具体化 `FengTypeDescriptor`：

```c
/* 独立泛型函数：通过 _desc->type_deps[i] */
void BoxedNode__init__G__T(FengGenericParamDescriptor *_T,
                            const FengFunctionDescriptor *_desc, ...)
{
    const FengTypeDescriptor *_node_desc = _desc->type_deps[0];
    FengObject *_obj = feng_obj_alloc(_node_desc->size, _node_desc);
    ...
}

/* 类型实例方法：通过 self->_hdr.desc->type_deps[i] */
void Container_create__G__K__V(struct Container__G__K__V *self,
                                FengGenericParamDescriptor *_K,
                                FengGenericParamDescriptor *_V, ...)
{
    const FengTypeDescriptor *_node_desc = self->_hdr.desc->type_deps[0];
    FengObject *_obj = feng_obj_alloc(_node_desc->size, _node_desc);
    ...
}

/* 类型静态方法：通过 _type_desc->type_deps[i] */
void Container_make__G__K__V(FengGenericParamDescriptor *_K,
                              FengGenericParamDescriptor *_V,
                              const FengTypeDescriptor *_type_desc, ...)
{
    const FengTypeDescriptor *_node_desc = _type_desc->type_deps[0];
    FengObject *_obj = feng_obj_alloc(_node_desc->size, _node_desc);
    ...
}
```

三种场景取 `type_deps[i]` 后，后续创建流程完全一致。

### 2.7 跨包泛型依赖信息（ft 扩展）

同包内 Wrapper 可直接从 AST 语义信息收集目标 type/func 的依赖列表。跨包时只有 ft，需在 ft 中记录每个泛型 type/func 的依赖元数据，供调用方 codegen 读取并按排序 key 分配索引。

#### ft 现有能力

- **TYPS** 节已有 `NAMED_GENERIC`（kind=6）：`string_ref`=全限定基础名，`elem_start`=TSEQ 起始，`elem_count`=类型参数数量——已能完整表达 `Foo<int,K>`
- **ATTRS** 节：`symbol_id` + `kind`（16 位）+ `value0/1/2`（各 32 位），可挂在任意符号上

#### 新增两个 attr kind（`src/symbol/internal.h`）

```c
typedef enum FengSymbolAttrKind {
    /* ... 已有 1–8 不变 ... */
    FENG_SYMBOL_ATTR_GENERIC_AGG_DEP  = 9,   /* 泛型 aggregate 依赖 */
    FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP = 10,  /* 泛型 managed 依赖 */
} FengSymbolAttrKind;
```

两个 attr 的字段布局（`FengSymbolFtAttrRecord`）：

| 字段 | 含义 |
|------|------|
| `symbol_id` | 泛型 type 的 sym id（`SYM_KIND_TYPE`）或 func 的 sym id（`SYM_KIND_TOP_FN` / `SYM_KIND_METHOD`） |
| `kind` | `FENG_SYMBOL_ATTR_GENERIC_AGG_DEP`（9）或 `FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP`（10） |
| `value0` | `TYPS.id`（`NAMED_GENERIC` 记录，包含完整类型名和参数信息） |
| `value1` | 0（保留） |
| `value2` | 0（保留） |

排序 key 由读取方从 `TYPS` 记录按 §2.2 规则推导，不存 ft，保持 ft 紧凑。

每个泛型依赖对应一条 attr 记录，多个依赖对应多条记录，Wrapper 读取后按 sort key 字典序排列，分配 `agg_deps[]`/`type_deps[]` 索引。

### 2.6 不新增 Runtime 函数

本方案不引入任何新的 runtime 函数和新的 runtime 源文件：

- `feng_generic_aggregate_instance_size`：不需要（大小直接来自 `desc->size`）
- `feng_generic_type_instance_size`：不需要
- `feng_generic_desc_resolve` / 全局缓存：不需要（全部静态，`.rodata`）
- `feng_generic_array_new`：不需要（直接调用 `feng_array_new_kinded`）

## 3. 不变量

1. **生命周期三大类不变**
2. **Aggregate walker 不变**
3. **已有非泛型代码路径不变**：`agg_deps_count == 0` 且 `type_deps_count == 0` 的描述符行为完全等同于原先
4. **描述符结构向后兼容**：新增字段在末尾，已有初始化代码中四个 count 字段默认为 0
5. **零运行时开销**：所有具体化描述符均为 `static const`，全在 `.rodata`

## 4. 涉及文件

| 文件 | 变更 |
|------|------|
| `src/runtime/feng_runtime.h` | 新增 `FengFunctionDescriptor`（`name`/`agg_deps`/`type_deps`）；`FengAggregateDescriptor` 新增 `agg_deps_count`/`agg_deps`/`type_deps_count`/`type_deps` 四个保留字段（当前恒为 0/NULL）；`FengTypeDescriptor` 新增 `agg_deps_count`/`agg_deps`/`type_deps_count`/`type_deps` 四个字段 |
| `src/codegen/codegen.c` | Wrapper 生成具体化描述符树（`FengTypeDescriptor.agg_deps`/`type_deps` 按排序 key 全局稳定）；独立函数共享体 ABI 增加 `const FengFunctionDescriptor *_desc`；静态方法共享体 ABI 增加 `const FengTypeDescriptor *_type_desc`；实例方法共享体不增加参数；修复 6 个创建路径（§2.5.1–§2.5.6）；读取 ft ATTRS 中 `GENERIC_AGG_DEP`/`GENERIC_TYPE_DEP` 支持跨包特化 |
| `src/symbol/internal.h` | `FengSymbolAttrKind` 新增 `FENG_SYMBOL_ATTR_GENERIC_AGG_DEP = 9`、`FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP = 10` |
| `src/symbol/ft_write.c` | 写出泛型 type/func 的 aggregate 和 managed 依赖 attr 记录 |
| `src/symbol/ft_read.c` | 解析新 attr kind，填入对应符号的依赖列表 |

## 5. 验证

```bash
feng run examples/hello_world --keep-ir
# 预期: START / OBJ_CREATED / OBJ_SET / JV_CREATED / JV: {"a":1}

feng test
# 全量回归
```
