# 描述符体系重构优化方案

## 背景

当前 `FengGenericParamDescriptor` 存在以下问题：

```c
typedef struct FengGenericParamDescriptor {
    size_t          size;
    FengValueKind   kind;
    FengRuntimeTypeKind type_kind;
    const struct FengAggregateValueDescriptor *aggregate;
    const void     *witness;
} FengGenericParamDescriptor;
```

- `type_kind` 是一个独立的语义分类枚举，共 18 个值，与 `kind`（生命周期分类）职责重叠
- `aggregate` 只在 `kind == AGGREGATE_WITH_MANAGED_SLOTS` 时有意义，其余情况为 NULL，字段含义依赖条件
- `size` 和类型自身描述符中的 `size` 冗余
- 这三个字段的组合使得"如何解释一个泛型参数"散落在多处，缺乏统一入口

## 核心设计原则

Feng 的每个值类型，无论内建类型还是用户类型，都可以归类为 `FengValueKind` 的三大结构之一。这三大结构是最小且完整的分类：

| `FengValueKind` | 结构语义 |
|---|---|
| `FENG_VALUE_TRIVIAL` | 纯字节值，无托管槽，无 retain/release |
| `FENG_VALUE_MANAGED_POINTER` | 单个托管指针，通过现有 ARC 原语管理 |
| `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | by-value composite，内部含至少一个托管槽 |

每种结构类别对应一种描述符类型。每个具体类型根据自身所属结构种类，持有对应的描述符。内建类型的描述符由 runtime 预定义；用户类型的描述符由 codegen 生成。

## 目标结构

### 三类描述符

#### 1. `FengTrivialDescriptor`（新增）

对应 `FENG_VALUE_TRIVIAL`，描述纯值类型的语义能力：

```c
typedef bool (*FengValueEqualFn)(const void *left, const void *right);

typedef struct FengTrivialDescriptor {
    const char     *name;
    size_t          size;
    FengValueEqualFn equal_fn;  /* NULL 时退回 memcmp(left, right, size) == 0 */
} FengTrivialDescriptor;
```

典型类型：`bool`、整数类型、浮点类型、用户 enum、C pointer、全 trivial tuple（如 `type Point(int, int)`）。

`equal_fn` 仅在字节比较不满足语义时才需要填写。例如浮点类型需要覆盖，因为 IEEE 754 规定 `NaN != NaN`，而两个 NaN 的位模式相同，`memcmp` 会误判为相等；整数、bool、enum、C pointer 均可用 NULL（字节比较即为正确语义）。

内建类型由 runtime 预定义：

```c
extern const FengTrivialDescriptor feng_i32_descriptor;
extern const FengTrivialDescriptor feng_i64_descriptor;
extern const FengTrivialDescriptor feng_bool_descriptor;
/* ... 其余内建标量 ... */
```

#### 2. `FengTypeDescriptor`（现有，小幅增补）

对应 `FENG_VALUE_MANAGED_POINTER`，描述堆对象的生命周期与语义能力。

现有字段保持不变（`name`、`size`、`finalizer`、`release_children`、GC metadata）。

可按需追加 equality 语义：

```c
FengValueEqualFn equal_fn;   /* NULL 时退回指针身份比较 */
```

典型类型：`string`、`array`、普通用户 `type User {}`、closure/callable。

注意：`MANAGED_POINTER` 类型在泛型上下文中的值大小恒为 `sizeof(void *)`，不从 `FengTypeDescriptor.size`（堆对象实例大小）读取。

#### 3. `FengAggregateDescriptor`（重命名自 `FengAggregateValueDescriptor`）

对应 `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`，现有结构保持，重命名使其与三分法命名一致：

```c
typedef struct FengAggregateDescriptor {
    const char *name;
    size_t size;
    const FengDefaultZeroInitDescriptor *default_zero_init;
    size_t managed_slot_count;
    const FengManagedSlotDescriptor *managed_slots;
    FengValueEqualFn equal_fn;   /* 新增，支持 tuple equality */
} FengAggregateDescriptor;
```

典型类型：含 `string`/array/object 字段的 tuple（如 `type Pair(int, string)`）、object-form spec fat value。

### `FengGenericParamDescriptor` 优化后

```c
typedef struct FengGenericParamDescriptor {
    FengValueKind   kind;
    const void     *descriptor;
    const void     *witness;
} FengGenericParamDescriptor;
```

`descriptor` 的实际类型由 `kind` 决定：

| `kind` | `descriptor` cast 目标 |
|---|---|
| `FENG_VALUE_TRIVIAL` | `const FengTrivialDescriptor *` |
| `FENG_VALUE_MANAGED_POINTER` | `const FengTypeDescriptor *` |
| `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | `const FengAggregateDescriptor *` |

### 删除 `FengRuntimeTypeKind`

`FengRuntimeTypeKind` 的 18 个语义分类可以被三类 descriptor 完全替代：

- 标量语义由各 `FengTrivialDescriptor` 实例区分
- enum 由 codegen 生成的 `FengTrivialDescriptor` 区分
- string/array/object/callable 由各 `FengTypeDescriptor` 实例区分
- spec/tuple 由 `FengAggregateDescriptor` 实例区分

`FengRuntimeTypeKind` 和 `aggregate` 字段均可从 `FengGenericParamDescriptor` 删除。

## 关于 `size` 字段

### 当前语义

`FengGenericParamDescriptor.size` 是"泛型值槽的字节大小"，即 `sizeof(T)` 在 ABI 中的体现。codegen 在生成三种 descriptor 时分别填入：

| `kind` | codegen 填入的 `.size` | 实际含义 |
|---|---|---|
| `FENG_VALUE_TRIVIAL` | `sizeof(C标量类型)` | T 的值字节数，如 `sizeof(int32_t)` = 4 |
| `FENG_VALUE_MANAGED_POINTER` | `sizeof(void *)` | 固定 8，指针宽度，不是堆对象大小 |
| `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | `sizeof(struct xxx)` | aggregate by-value struct 大小 |

注意：`MANAGED_POINTER` 的 `.size` 是 `sizeof(void *)`，而不是 `FengTypeDescriptor.size`（堆对象含 header 的完整大小），两者语义不同，不可混用。

当前 runtime 中 `.size` 只在 `feng_runtime_contract.c` 的 `runtime_contract_copy_value_to_out` 中使用，用于 `memcpy(out, value, type->size)`（仅 TRIVIAL 和 AGGREGATE 两个分支，MANAGED_POINTER 分支不访问 `.size`）。

### 重构后的处理

`FengGenericParamDescriptor.size` 与各 descriptor 中的 `size` 冗余。可以删除，替换规则：

- `FENG_VALUE_TRIVIAL`：改从 `((const FengTrivialDescriptor *)param->descriptor)->size` 读取
- `FENG_VALUE_MANAGED_POINTER`：固定写 `sizeof(void *)`，不查 descriptor
- `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`：改从 `((const FengAggregateDescriptor *)param->descriptor)->size` 读取

与现有 codegen 填值逻辑完全对应，语义不变。

可提供 inline helper 统一封装：

- runtime 提供按 `kind + descriptor` 取 size 的 helper：

```c
static inline size_t feng_generic_value_size(const FengGenericParamDescriptor *param) {
    switch (param->kind) {
        case FENG_VALUE_TRIVIAL:
            return ((const FengTrivialDescriptor *)param->descriptor)->size;
        case FENG_VALUE_MANAGED_POINTER:
            return sizeof(void *);
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            return ((const FengAggregateDescriptor *)param->descriptor)->size;
    }
}
```

可以在重构时一并删除，也可以保留作过渡，待全量回归通过后再清理。

## 运行时开销保证

本次重构不得增加任何运行时开销，以下逐路径说明。

### 泛型 dispatch（witness 调用）

`witness` 字段位置和语义完全不变。`FengGenericParamDescriptor` 结构体从 40 bytes 缩小到 24 bytes，`kind` 和 `witness` 落在同一 cache line，dispatch 路径汇编指令不变，开销只减不增。

`kind` 保留在外层 `FengGenericParamDescriptor`，不放入 descriptor 头部，原因：若放入头部，每次按 `kind` 分派都需要先读 `descriptor` 指针再读 `kind`，多一次间接寻址，有额外 cache miss 风险。外层直读是零额外开销的唯一选择。

### 泛型 retain/release（`feng_generic_retain` / `feng_generic_release`）

`kind` 直接可读，分派逻辑不变。

- `FENG_VALUE_TRIVIAL`：无操作，不变
- `FENG_VALUE_MANAGED_POINTER`：直接 `feng_retain(ptr)` / `feng_release(ptr)`，不变
- `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS`：读 `descriptor` 后访问 `managed_slots`，指针解引用链与原来 `param->aggregate->managed_slots` 完全等价，热循环体汇编不变

### 非泛型普通类型的 retain/release

非泛型代码中，codegen 在编译期已知类型，直接静态发出对应操作：

- 普通对象 / string / array：codegen 直接发出 `feng_retain(ptr)` / `feng_release(ptr)`，一条函数调用
- aggregate with managed slots：codegen 在编译期展开逐槽 retain/release，不查任何描述符

`FengGenericParamDescriptor` 和 `kind` 判断完全不出现在非泛型热路径中，与本次重构无关。

## 实施任务清单

### 阶段一：描述符重构

- [x] **runtime** 新增 `FengTrivialDescriptor` 结构定义（`feng_runtime.h`）
- [x] **runtime** 为内建标量类型预定义 `FengTrivialDescriptor` 实例（bool、i8-i64、u8-u64、f32/f64）
- [x] **runtime** `FengAggregateValueDescriptor` 重命名为 `FengAggregateDescriptor`，更新所有引用
- [x] **runtime** `FengTypeDescriptor` 追加 `equal_fn` 字段
- [x] **runtime** `FengGenericParamDescriptor` 精简为 3 字段（`kind` / `descriptor` / `witness`），删除 `size`、`type_kind`、`aggregate`
- [x] **runtime** 更新 `runtime_contract_copy_value_to_out`：将 `type->size` 改为通过 descriptor 读取
- [x] **codegen** 标量 / enum / C pointer / 全 trivial tuple：生成或引用对应 `FengTrivialDescriptor`
- [x] **codegen** 含 managed slot 的 tuple / spec fat value：生成 `FengAggregateDescriptor`
- [x] **codegen** `FengGenericParamDescriptor` 初始化：去除 `size`、`type_kind`、`aggregate`，改为 `descriptor`
- [x] **runtime** 确认 `FengRuntimeTypeKind` 在所有 runtime / codegen 文件中无残留读取点，随后删除
- [x] 全量回归测试通过

### 阶段二：Tuple 相等性

- [x] **runtime** `FengAggregateDescriptor` 追加 `equal_fn` 字段
- [x] **codegen** 为含 managed slot 的 tuple 生成 `equal_fn` 函数实现
- [x] **runtime** `feng_expression_equal` 的 `AGGREGATE` 分支调用 `descriptor->equal_fn`
- [x] 全量回归测试通过

## 实施范围（概述）

1. **runtime**：新增 `FengTrivialDescriptor`；为内建标量类型预定义 descriptor；`FengAggregateValueDescriptor` 重命名为 `FengAggregateDescriptor`，追加 `equal_fn`；`FengTypeDescriptor` 追加 `equal_fn`；`FengRuntimeTypeKind` 标记废弃或删除
2. **codegen**：
   - 标量/enum/C pointer/全 trivial tuple：生成或引用 `FengTrivialDescriptor`
   - 含 managed slot 的 tuple / spec fat value：生成 `FengAggregateDescriptor`（加 `equal_fn`）
   - 普通对象 type：沿用 `FengTypeDescriptor`
   - `FengGenericParamDescriptor` 初始化：去除 `size`、`type_kind`、`aggregate`，改为 `descriptor`
3. **全量回归测试**：所有现有测试必须通过

## 不变约束

- `FengTypeDescriptor` 作为堆对象描述符的核心结构不拆分
- `FengAggregateDescriptor`（现 `FengAggregateValueDescriptor`）的 managed slot 机制不动
- 三类 descriptor 之间不互相持有
- 不引入运行时类型注册表；所有 descriptor 均为静态 const 对象
