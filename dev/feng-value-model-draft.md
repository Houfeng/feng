# Feng 值模型与托管槽位处理草案

> 本文档是实现草案，不是语言权威规范。
> 本草案讨论的是运行时值分类、托管槽位处理与 codegen/runtime 的职责边界。
> 本文档不直接修改现有 `spec` 语义，也不替代 [feng-spec-codegen-draft.md](./feng-spec-codegen-draft.md)。

## 1 目标

本文档用于收敛一个更面向未来的值模型抽象，使后续能力可以落到同一套框架中，而不是围绕某一个特性单独做特判。

当前明确会或可能受益的能力包括：

- object-form `spec` 的未来 fat value 方案
- callable 值与闭包环境
- 未来的 tuple
- 未来的普通值语义 struct
- 数组元素为按值聚合类型的场景
- 对象字段为按值聚合类型的场景

本文档的核心目标有两个：

- 保持现有“单托管指针”基础设施尽量不动
- 在其上增加一层可扩展的“按值聚合 + 托管槽位处理”抽象

## 2 设计结论

### 2.1 不再只用“指针 vs 非指针”二分

如果值模型只有“指针 vs 非指针”两类，那么一旦引入 tuple 或普通值语义 struct，且这些聚合值内部允许引用 `string`、数组、对象、闭包或其他托管值，就会出现两难：

- 要么让所有“非指针”值都进入成员遍历路径，导致纯平凡值也被拖入额外处理流程
- 要么让所有“非指针”值都不进入遍历路径，导致含托管引用的聚合值无法正确 retain/release

因此，值模型应至少区分以下三类：

1. trivial value
2. managed pointer value
3. aggregate value with managed slots

### 2.2 三类值的定义

#### trivial value

满足以下条件的值属于 `trivial value`：

- 按值存储
- 按值传递
- 复制不需要 retain
- 销毁不需要 release
- 不需要扫描内部成员

典型例子：

- `bool`
- 各类整数与浮点
- 只由上述平凡值组成的 tuple / struct

#### managed pointer value

满足以下条件的值属于 `managed pointer value`：

- 值本身就是一根托管指针
- retain/release/assign 可以直接对该值本身操作
- 当前运行时的绝大多数基础设施已经围绕它建立

典型例子：

- `string`
- 数组对象引用
- 普通对象引用
- 当前闭包对象引用
- 现有 box 方案下的 object-form `spec`

#### aggregate value with managed slots

满足以下条件的值属于 `aggregate value with managed slots`：

- 值本身按值存储，不是单一托管指针
- 内部包含一个或多个托管子槽位
- 复制、赋值、销毁时需要对子槽位做 retain/release
- 只有托管子槽位需要参与处理，其他平凡字段不进入托管流程

典型例子：

- `(string, int)`
- `(string, User, int[])`
- 未来带托管字段的值语义 struct
- 未来的 fat `spec` 值

## 3 设计原则

### 3.1 单指针原语尽量不动

现有底层原语已经能稳定处理“一根托管指针”：

- retain
- release
- assign
- take
- cleanup 链中的单槽位释放

本草案不建议把这些原语升级成“直接处理任意复杂值”的大接口。

更稳的路线是：

- 底层原语继续只处理单个托管槽位
- 上层如果遇到多个托管槽位，就逐个调用这些现有原语

这样做有三个直接收益：

- 保持现有运行时基座稳定
- 降低新模型对现有代码路径的冲击
- 让多槽位值的处理逻辑集中在一层较薄的抽象里

### 3.2 不把处理器挂到值实例上

未来 tuple 或值语义 struct 若需要托管槽位处理，不建议把“处理器指针”挂到每个值实例上。

不建议的原因：

- 每个实例都会额外增大
- 平凡值也会被迫承担不必要的空间成本
- 会污染按值布局、数组元素布局、返回值 ABI 与默认零值模型
- 同一种类型的处理逻辑本质上是静态的，没有必要重复存在于每个实例中

更合适的方式是：

- 把处理逻辑挂在类型上，而不是值实例上
- 用静态描述符或静态处理器描述“这个类型有哪些托管槽位、如何遍历它们”

### 3.3 只处理托管槽位，不遍历所有成员

好的实现目标不是“遍历聚合值的每个字段再判断是否托管”，而是：

- 在编译期或类型描述中直接得到该类型的托管槽位列表
- 运行时或 codegen 只处理这些托管槽位

这样可以确保：

- `trivial value` 根本不进入托管遍历流程
- 只有 `aggregate value with managed slots` 才进入托管槽位 walk
- 即使一个 tuple/struct 有很多字段，也只处理真正需要 retain/release 的那几个

## 4 通用抽象

### 4.1 面向上层的值分类

面对 codegen、数组、cleanup、对象字段与返回值路径，建议统一暴露以下分类信息：

```c
typedef enum FengValueKind {
    FENG_VALUE_TRIVIAL,
    FENG_VALUE_MANAGED_POINTER,
    FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
} FengValueKind;
```

这个分类解决的是：

- 当前值是否需要任何托管生命周期处理
- 如果需要，是单指针快路径还是聚合槽位路径

### 4.2 面向内部实现的进一步拆分

如果后续需要更精细的优化，内部实现可以再拆成两个维度，而不改变上面的对外分类：

- 表示形态：标量 / 指针 / 聚合
- 生命周期类别：平凡 / 单托管槽位 / 多托管槽位

这样做的目的是为将来的优化保留空间，但第一阶段不必把所有细分类都暴露成公共概念。

## 5 类型级静态处理描述

### 5.1 核心思想

对 `aggregate value with managed slots`，类型应提供一份静态描述，告诉 codegen/runtime：

- 这个值有几个托管槽位
- 每个槽位在值内部的偏移是多少
- 该槽位是否仅是单托管指针
- 该槽位是否对应另一个按值聚合子值，需要继续递归处理

这个描述应归属于“类型”，而不是某个具体实例。

### 5.2 一个可执行的最小形状

第一阶段可以把描述压到尽量简单，例如：

```c
typedef enum FengManagedSlotKind {
    FENG_SLOT_POINTER,
    FENG_SLOT_NESTED_AGGREGATE,
} FengManagedSlotKind;

typedef struct FengManagedSlotDesc {
    size_t offset;
    FengManagedSlotKind kind;
    const struct FengAggregateValueDesc *nested;
} FengManagedSlotDesc;

typedef struct FengAggregateValueDesc {
    const char *debug_name;
    size_t size;
    size_t managed_slot_count;
    const FengManagedSlotDesc *managed_slots;
} FengAggregateValueDesc;
```

这个最小形状先解决三件事：

- 静态定位托管槽位
- 支持嵌套聚合值递归进入
- 不要求每个类型先写自定义函数

### 5.3 为什么第一阶段优先描述符，而不是任意回调处理器

任意回调处理器的灵活性更高，但第一阶段不应直接上这条路。

原因：

- tuple、普通规则布局 struct、fat `spec` 这类类型，大多数情况下都可以用“偏移表 + 通用 walker”表达
- 先上回调会让 codegen/runtime 边界变松，过早把控制流复杂度引入底层
- 纯静态描述更利于检查、优化和验证

因此第一阶段建议：

- 默认使用静态描述符
- 仅当未来出现规则布局无法表达的类型时，再讨论是否允许补充自定义回调

## 6 通用 walker

### 6.1 设计目标

既然底层原语继续只处理单个托管槽位，那么聚合值的处理层需要一个统一 walker，用于：

- retain 聚合值内部的所有托管槽位
- release 聚合值内部的所有托管槽位
- assign 一个聚合值到另一个聚合槽位
- cleanup 作用域退出时释放局部按值聚合变量中的托管槽位

### 6.2 建议接口

第一阶段可以采用 visitor 形状，而不是“返回托管指针数组”：

```c
typedef void (*FengManagedSlotVisitor)(void *slot_base,
                                       const FengManagedSlotDesc *slot,
                                       void *ctx);

void feng_visit_aggregate_managed_slots(void *value,
                                        const FengAggregateValueDesc *desc,
                                        FengManagedSlotVisitor visitor,
                                        void *ctx);
```

选择 visitor 的原因：

- 不需要临时分配“托管指针列表”
- 可以直接处理槽位地址，而不是先构造中间结果
- 更适合递归进入嵌套聚合值

### 6.3 与现有单指针原语的协作

walker 本身不直接替代 retain/release/assign。

它只负责：

- 找到托管槽位
- 逐个把槽位交给现有原语或上层操作

例如：

- retain 聚合值时，对每个 `FENG_SLOT_POINTER` 槽位调用 retain
- release 聚合值时，对每个 `FENG_SLOT_POINTER` 槽位调用 release
- 对 `FENG_SLOT_NESTED_AGGREGATE` 槽位，递归进入其 `nested` 描述

## 7 与现有能力的关系

### 7.1 对当前单指针基础设施的影响

本草案的目标是：

- 不推翻现有单指针基础设施
- 不要求 `feng_retain` / `feng_release` / `feng_assign` 直接理解复杂聚合值
- 仅在上层新增一层“聚合值托管槽位处理”能力

换句话说，单指针路径仍然是快路径，不应被聚合值模型拖慢。

### 7.2 对 tuple 的意义

如果未来支持 tuple：

- `(int, bool)` 应落为 `trivial value`
- `(string, int)` 应落为 `aggregate value with managed slots`
- `((string, int), bool)` 应通过嵌套描述递归处理

因此 tuple 是本草案成立的直接驱动之一。

### 7.3 对值语义 struct 的意义

如果未来支持值语义 struct：

- 纯平凡字段 struct 仍可保持 `trivial value`
- 含托管字段 struct 不必退化成托管对象指针
- 可以继续是按值布局，只是在生命周期路径上走托管槽位处理

### 7.4 对 future fat spec 的意义

如果 object-form `spec` 未来改为 fat value，本草案提供了一条更稳的落点：

- fat `spec` 不是新的特殊大类
- 它只是一个 `aggregate value with managed slots`
- 只要它的托管槽位可以静态描述，就能复用同一套 walker 和单指针原语

这意味着 `spec` 不再需要单独发明一套生命周期模型。

## 8 codegen / runtime / semantic 的职责边界

### 8.1 semantic

semantic 不负责具体 retain/release 细节。

但 semantic 应能向 codegen 明确提供：

- 某个类型是平凡值、托管指针值还是含托管槽位的聚合值
- 对于 `spec` 这类带契约语义的类型，具体使用哪种适配关系

### 8.2 codegen

codegen 负责：

- 在本地变量、参数、返回值、字段赋值、数组元素等路径上选择正确的值处理路径
- 为 tuple / 值语义 struct / fat `spec` 生成静态描述符
- 在需要时调用聚合值 walker
- 继续复用现有单指针原语处理实际托管槽位

### 8.3 runtime

runtime 负责：

- 提供稳定的单指针原语
- 提供尽量薄的聚合值 walker 支撑
- 不在运行时动态推断语言层类型语义

## 9 分阶段建议

### Phase A

- 只在设计层引入三类值模型
- 不修改现有单指针 runtime ABI
- 为未来 tuple / 值语义 struct / fat `spec` 预留统一抽象

### Phase B

- 先让一个受限场景接入该模型
- 推荐优先顺序：fat `spec` 或 tuple 二选一
- 第一刀可只支持“规则布局 + 静态托管槽位描述”

### Phase C

- 扩展到嵌套聚合值
- 接入数组元素与对象字段路径
- 视需要再讨论是否引入自定义处理回调

## 10 非目标

本草案当前明确不处理：

- 修改已有托管对象头布局
- 把所有值统一装箱
- 为每个值实例保存处理器指针
- 一开始就引入完全通用的自定义回调系统
- 在运行时动态反射“一个值内部到底有哪些托管成员”

## 11 当前建议

当前建议可以压缩成以下几条：

- 值模型不再只用“指针 vs 非指针”二分
- 保留现有单指针原语作为稳定基座
- 新增 `aggregate value with managed slots` 作为第三类值
- 聚合值的托管处理逻辑挂在类型上，不挂在值实例上
- 第一阶段优先采用静态描述符 + 通用 walker
- 未来 tuple、值语义 struct、fat `spec` 统一落到这套框架中
