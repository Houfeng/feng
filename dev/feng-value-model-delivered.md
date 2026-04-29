# Feng 值模型与托管槽位处理（已交付，layer）

> 本文档原为实现草案，layer 已交付（runtime + codegen + semantic 三方接口齐备）。
> 已完成范围见本文末 §13；尚未到达"完整 fat spec 端到端验证"层级的扩面工作（spec 字段 / 默认零值 / 等值 / 数组等）由 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) 的 4b-β / 4b-γ 跟踪。
> 本文档不修改任何语言权威规范（`docs/`），不修改 `FengManagedHeader` 与现有单指针 ARC 原语。
> 本文档是 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) 的前置依赖：spec 发码改用 fat 方案前，本文档必须落地。

## 1 目标与范围

### 1.1 直接目标

- 收敛"按值聚合值（含托管槽位）"的统一模型，使其可作为 spec 发码的运行时基座。
- 把模型设计成**对扩展开放、对修改封闭**：新增 tuple / 值语义 struct / 其他按值聚合类型时，仅需新增**类型级描述符**，不修改 runtime 与 codegen 的核心代码路径。
- 完全保留并复用现有单托管指针基础设施（`feng_retain` / `feng_release` / `feng_assign` / 作用域清理 / 异常清理 / cycle collector），不修改其 ABI 与行为。

### 1.2 首发消费者

**fat object-form `spec`**。

理由：

- 现有 box 方案是双托管对象，运行时分配/回收开销过大。
- 已决策 object-form `spec` 改为 fat 值（subject + witness 两字段按值聚合）。
- spec 发码是 [feng-plan.md](./feng-plan.md) 第二阶段的前置依赖，必须在 Phase 2 启动前交付。
- fat spec 的运行时形态恰好是"含一个托管槽位的按值聚合值"，是验证本模型最自然的最小切片。

### 1.3 设计原则（硬约束）

1. **单指针基座不动。** 所有针对聚合值的操作必须通过**多次调用现有单指针原语**实现，而非新增"能直接处理任意复杂值"的底层原语。
2. **开闭原则。** runtime 的聚合值处理代码路径必须是**类型无关**的通用 walker；新增按值聚合类型时只生成新描述符，不改 walker 与不改 codegen 公共 helper。
3. **类型级描述，不污染实例。** 处理逻辑挂在类型描述符上，不挂在每个值实例上；trivial 值不承担任何额外空间与运行时成本。
4. **静态描述优先于回调。** 第一阶段只允许"偏移表 + kind 枚举"的静态描述；不暴露任意函数指针回调。
5. **零运行时反射。** runtime 不在运行期推断"某个值内部到底有哪些托管成员"。

### 1.4 非目标

- 修改 `FengManagedHeader` 布局或新增 `FengTypeTag`。
- 把所有值统一装箱。
- 为每个值实例存储描述符指针。
- 引入面向用户的自定义生命周期回调系统。
- 为 `@fixed spec` / C ABI 兼容做特殊设计（独立草案处理）。
- 修改 `docs/` 下任何语言权威规范。

## 2 三类值分类

### 2.1 分类

值统一分为三类，**仅此三类**：

| 分类 | 说明 |
| --- | --- |
| `FENG_VALUE_TRIVIAL` | 按值存储，复制不需要 retain，销毁不需要 release，无需扫描内部成员。 |
| `FENG_VALUE_MANAGED_POINTER` | 值本身是一根托管指针；所有现有单指针原语直接作用其上。 |
| `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | 值按值聚合存储，内部包含至少一个托管子槽位；生命周期通过逐槽调用单指针原语完成。 |

下文统称 trivial / managed-pointer / aggregate。

### 2.2 各类的判定与示例

**trivial**：`bool`、整数、浮点、纯平凡字段组成的未来 tuple / 值语义 struct。
**managed-pointer**：`string`、数组、普通对象引用、闭包对象引用。
**aggregate**：fat object-form `spec`（首发消费者）；未来含托管字段的 tuple / 值语义 struct。

判定职责见 §6。

### 2.3 不再使用"指针 vs 非指针"二分

仅二分会让"含托管子槽位的按值聚合值"无处安放：要么把所有非指针值都拖进遍历路径（trivial 受拖累），要么都不进（aggregate 漏处理）。三分类是基础前提，不可退化。

## 3 类型级描述符

### 3.1 描述符核心定义

```c
typedef enum FengManagedSlotKind {
    /* 槽位本身就是一根托管指针，使用现有单指针原语。 */
    FENG_SLOT_POINTER = 1,
    /* 槽位内嵌套另一个按值聚合值，需要递归走 walker。 */
    FENG_SLOT_NESTED_AGGREGATE = 2,
} FengManagedSlotKind;

typedef struct FengManagedSlotDescriptor {
    /* 槽位相对于聚合值起始地址的字节偏移。 */
    size_t offset;
    FengManagedSlotKind kind;
    /* 仅当 kind == FENG_SLOT_NESTED_AGGREGATE 时使用，指向嵌套聚合值的描述符。 */
    const struct FengAggregateValueDescriptor *nested;
} FengManagedSlotDescriptor;

typedef struct FengAggregateValueDescriptor {
    /* 调试用，全限定名。 */
    const char *name;
    /* 聚合值整体大小（字节）。 */
    size_t size;
    /* 默认初始化策略，见 §4。 */
    const struct FengAggregateDefaultInitDescriptor *default_init;
    /* 按声明顺序排列的托管槽位表（仅托管槽位，不含平凡字段）。 */
    size_t managed_slot_count;
    const FengManagedSlotDescriptor *managed_slots;
} FengAggregateValueDescriptor;
```

### 3.2 设计要点

- 描述符**只列托管槽位**，不枚举平凡字段。trivial 字段不进入任何 walker 路径。
- `kind` 仅有两种取值；不引入回调指针。
- `nested` 字段为递归留出空间，但第一阶段（fat spec）不会用到（fat spec 只有一个 `FENG_SLOT_POINTER` 槽位 = subject）。
- `name` 仅用于调试与诊断，不参与任何决策。
- 描述符是 `static const`，由 codegen 生成。

### 3.3 trivial 值不生成任何描述符

Trivial 值（`bool` / 整数 / 浮点 / 全部字段都是 trivial 的未来 tuple 与值语义 struct）**不生成本草案的任何描述符**：

- 没有托管槽位需要被 walker 访问。
- 大小、对齐、复制语义由 C 编译器直接承担。
- runtime 与 codegen 的所有按值聚合 helper（§5）不应在 trivial 值上调用；codegen 在站点分派时直接走 C 原生路径（赋值 / `memcpy`）。

这条规则保证 §1.3 第 3 条原则成立：trivial 值不承担任何额外空间与运行时成本。

### 3.4 与现有 `FengTypeDescriptor` 体系的关系

runtime 现有体系里已经存在两个相关结构：`FengTypeDescriptor`（描述堆托管对象整体）与 `FengManagedFieldEntry`（描述堆托管对象内部的某一根托管指针字段，详见 [src/runtime/feng_runtime.h](../src/runtime/feng_runtime.h)）。

本草案新增的两个结构与之**职责相同、应用对象不同**。为避免命名漂移，做以下两件事：

- 重命名旧类型 `FengManagedFieldEntry` → `FengManagedFieldDescriptor`，使后缀统一为 `Descriptor`（见 §9.1 任务）。
- 不合并新旧两套描述符。

#### 3.4.1 描述符总览

| 描述符 | 描述对象 | 整体 vs 单槽 | 主要消费者 |
| --- | --- | --- | --- |
| `FengTypeDescriptor` | 堆托管对象（含 header） | 整体 | ARC / cycle collector / 终结器 |
| `FengManagedFieldDescriptor`（原 `FengManagedFieldEntry`） | 堆托管对象内部的一根托管指针字段 | 单槽 | cycle collector |
| `FengAggregateValueDescriptor` | 按值聚合值（无 header） | 整体 | §5 五个聚合 API |
| `FengManagedSlotDescriptor` | 按值聚合值内部的一个托管槽位 | 单槽 | §5 walker |

#### 3.4.2 单槽描述符差异对比

`FengManagedFieldDescriptor` 与 `FengManagedSlotDescriptor` 都是"单槽描述符"，但服务于不同载体：

| 维度 | `FengManagedFieldDescriptor` | `FengManagedSlotDescriptor` |
| --- | --- | --- |
| 描述谁内部 | 堆托管对象（含 header） | 按值聚合值（无 header） |
| `offset` 起点 | 对象 base | 聚合值 base |
| 是否带 `kind` | 不带（隐含 = 托管指针） | 带（指针 / 嵌套聚合） |
| 是否支持嵌套 | 不需要（堆对象不嵌套堆对象） | 需要（聚合可嵌套聚合） |
| 是否带目标类型字段 | 带 `static_desc`（CC 提示） | 不带 |
| 主要消费者 | cycle collector | §5 walker |

#### 3.4.3 不合并的理由

上表三处差异（offset 起点、是否需要 kind、是否需要嵌套）都是本质差异。强行合并会让两边都背上自己用不到的字段；同时，堆对象描述符的多数字段（finalizer / refcount 语义 / tag）对按值聚合值无意义，强行复用 `FengTypeDescriptor` 也会让其字段语义跨语境，长期容易出错。

二者唯一的桥接点见 §7.2。

## 4 默认初始化

### 4.1 必要性

按值聚合值的默认零值**不一定是全 0 字节**。例如 fat spec 的默认值是 `{ subject = 默认子对象指针, witness = 默认 witness 表指针 }`，subject 必须是已 retain 的真实对象，witness 必须是有效的静态表。`memset(0)` 会得到非法值。

### 4.2 描述

```c
typedef enum FengAggregateDefaultKind {
    /* 全零字节就是合法默认值。codegen 直接 memset(0)。 */
    FENG_DEFAULT_ZERO_BYTES = 1,
    /* 需要调用类型自带的初始化函数。 */
    FENG_DEFAULT_INIT_FN = 2,
} FengAggregateDefaultKind;

typedef void (*FengAggregateDefaultInitFn)(void *value_out);

typedef struct FengAggregateDefaultInitDescriptor {
    FengAggregateDefaultKind kind;
    /* 仅当 kind == FENG_DEFAULT_INIT_FN 时使用。函数应将 value_out 初始化为
     * 一个完全合法的、所有托管槽位都已正确 retain 的实例。 */
    FengAggregateDefaultInitFn init_fn;
} FengAggregateDefaultInitDescriptor;
```

### 4.3 选用规则

- 全部托管槽位的默认值都允许为 `NULL` 时（未来 tuple `(string?, int)` 这类），可使用 `FENG_DEFAULT_ZERO_BYTES`。
- 任一托管槽位需要非空合法默认值时（fat spec），必须使用 `FENG_DEFAULT_INIT_FN`，由 codegen 为该类型生成 init 函数。
- 第一阶段 fat spec 一律走 `FENG_DEFAULT_INIT_FN`。

## 5 五类聚合操作（runtime 公共 API）

### 5.1 API 列表

下列 API 是 runtime 公共 ABI，定义在 `feng_runtime.h` 的新章节中，由 codegen 直接 emit 调用。

```c
/* 对聚合值的每个 FENG_SLOT_POINTER 槽位调用 feng_retain；对每个
 * FENG_SLOT_NESTED_AGGREGATE 槽位递归调用 feng_aggregate_retain。
 * 对 NULL 指针槽位是安全的。 */
void feng_aggregate_retain(void *value, const FengAggregateValueDescriptor *desc);

/* 对称的 release。语义保证：调用后 value 中的托管槽位仍是当前字节，但
 * 其托管引用已被释放，调用方负责确保此后不再读取这些槽位（通常紧接
 * memcpy 覆盖或栈帧弹出）。 */
void feng_aggregate_release(void *value, const FengAggregateValueDescriptor *desc);

/* 语义等价于 *dst = src，且对托管槽位执行正确的 retain/release。
 * 实现保证：
 *   - 自赋值（dst == src）安全且无副作用。
 *   - 中途异常不会导致泄露或重复释放。
 * 内部实现通过逐槽调用 feng_assign 完成；非托管字节通过 memcpy 复制。 */
void feng_aggregate_assign(void *dst, const void *src,
                           const FengAggregateValueDescriptor *desc);

/* 移动语义：把 src 的所有权搬移到 dst，src 对应字节被覆盖为"已被搬走"
 * 的合法状态（托管槽位置 NULL，平凡字段保持原值或清零，由 walker 行为
 * 决定）。dst 原内容被释放（按 release 语义）。
 * 用于返回值优化、临时值落入命名变量等场景。 */
void feng_aggregate_take(void *dst, void *src,
                         const FengAggregateValueDescriptor *desc);

/* 把 value_out 初始化为该类型的默认值。
 * 内部根据 desc->default_init 选择 memset 或调用 init_fn。 */
void feng_aggregate_default_init(void *value_out,
                                 const FengAggregateValueDescriptor *desc);
```

### 5.2 实现策略：完全复用单指针原语

**这是本草案的核心实现约定。** 五个 API 的实现一律通过通用 walker 遍历描述符，对每个槽位调用现有单指针原语：

- `retain`：每个 `FENG_SLOT_POINTER` 槽位 → `feng_retain`；每个 `FENG_SLOT_NESTED_AGGREGATE` → 递归。
- `release`：每个 `FENG_SLOT_POINTER` 槽位 → `feng_release`；每个 `FENG_SLOT_NESTED_AGGREGATE` → 递归。
- `assign`：先按声明顺序对每个 `FENG_SLOT_POINTER` 槽位 `feng_assign(dst_slot, src_slot)`（`feng_assign` 内部已做 retain-before-release，自赋值安全）；非托管字节由 `memcpy` 完成。
- `take`：对每个 `FENG_SLOT_POINTER` 槽位 `feng_release(*dst_slot); *dst_slot = *src_slot; *src_slot = NULL;`；非托管字节同样 `memcpy`。
- `default_init`：见 §4。

walker 函数本身是 runtime 内部实现，不暴露公共回调签名。

### 5.3 通用 walker（runtime 内部）

```c
/* runtime 内部使用，不进入公共头。 */
typedef void (*FengManagedSlotVisitor)(void *slot_base,
                                       const FengManagedSlotDescriptor *slot,
                                       void *ctx);

static void feng_visit_aggregate_managed_slots(
    void *value,
    const FengAggregateValueDescriptor *desc,
    FengManagedSlotVisitor visitor,
    void *ctx);
```

五个公共 API 通过它实现；新增聚合类型不改它。

### 5.4 OCP 体现

- 新增按值聚合类型 → 仅生成新的 `FengAggregateValueDescriptor` 实例。
- runtime 的五个 API、walker、单指针原语 → 全部不动。
- codegen 公共 helper（"对一个 aggregate 槽位 emit retain/release/assign/take/default-init"）→ 全部不动。

## 6 三方职责边界

### 6.1 semantic

semantic 必须为每个类型稳定产出：

1. 该类型的 `FengValueKind` 分类（trivial / managed-pointer / aggregate）。
2. 若为 aggregate，该类型对应的**抽象**槽位描述（slot 列表、每个 slot 的 offset 与 kind、嵌套指向）。

semantic **不**直接生成 C 描述符字面量，仅产出抽象描述供 codegen 消费。

semantic **不**参与 retain/release 决策；仅做分类。

semantic 对 `spec` 的契约规则推导继续按 [feng-spec-semantic-delivered.md](./feng-spec-semantic-delivered.md) 已决策的方式执行；本草案不重叠该职责。

### 6.2 codegen

codegen 负责：

- 把 semantic 的抽象描述 emit 成 C `static const FengAggregateValueDescriptor`。
- 在每个值处理站点（局部变量声明、参数传递、返回值、字段赋值、数组元素读写、作用域退出清理、异常清理路径）按值类别选择对应的 emit 路径：
  - trivial → 直接 C 赋值 / `memcpy`。
  - managed-pointer → 调用现有单指针原语（保持当前行为）。
  - aggregate → 调用 §5 的五类聚合 API。
- 为含 `FENG_DEFAULT_INIT_FN` 的类型生成对应的 init 函数。

codegen 公共 helper 必须按值类别**分派**，但**不**为每种 aggregate 类型生成专用代码。新增 aggregate 类型 → 仅新增描述符 + 必要时新增 init 函数；codegen helper 与 emit 路径不动。

### 6.3 runtime

runtime 负责：

- 维持现有单指针原语稳定不变。
- 提供 §5 列出的五个聚合 API 与内部 walker。
- 不为任何具体 aggregate 类型写专用代码。
- 不在运行期推断语言层类型语义。

## 7 与现有运行时的接入点

### 7.1 单指针原语：零变更

`feng_retain` / `feng_release` / `feng_assign` / 作用域清理表 / 异常清理表 / `release_children` 调用约定 / `FengManagedHeader` 布局 / `FengTypeTag` 集合：**全部不变**。

### 7.2 Cycle collector：通过描述符展平接入

**接入方式：在 codegen 生成 `FengTypeDescriptor.managed_fields` 时，将含 aggregate 字段的对象按 aggregate 描述符展平成多条原始 `FengManagedFieldDescriptor`。**

具体规则：

- 若对象字段是 managed-pointer：`managed_fields` 加一条，`offset = 字段偏移`，与现状一致。
- 若对象字段是 aggregate：对该 aggregate 描述符的每个 `FENG_SLOT_POINTER` 槽位生成一条 `FengManagedFieldDescriptor`，`offset = 字段偏移 + 槽位偏移`。
- 嵌套 aggregate 槽位递归展开（`offset = 字段偏移 + 外层槽位偏移 + ... + 最内层指针槽位偏移`）。
- aggregate 中的非托管字节不进入 `managed_fields`。

#### 7.2.1 桥接示意

以一个含 fat spec 字段的对象为例：

```c
struct Feng__demo__Holder {
    FengManagedHeader header;            /* 偏移 0 */
    int32_t tag;                          /* 偏移 16，trivial */
    FengSpecValue__demo__Named named;     /* 偏移 24（subject 在 +0，witness 在 +8） */
    FengString *label;                    /* 偏移 40，普通托管指针 */
};

/* fat spec 内部槽位描述（来自 §3）： */
static const FengManagedSlotDescriptor FengSpec__demo__Named__slots[] = {
    { offsetof(FengSpecValue__demo__Named, subject), FENG_SLOT_POINTER, NULL },
};

/* codegen 展平后生成的对象托管字段表： */
static const FengManagedFieldDescriptor Feng__demo__Holder__managed_fields[] = {
    /* 来自 named.subject：字段偏移 24 + 槽位偏移 0 = 24 */
    { .offset = 24, .static_desc = NULL },
    /* 来自 label：字段偏移 40 */
    { .offset = 40, .static_desc = &feng_string_descriptor },
};
```

这是新旧两套描述符之间**唯一的**桥接点：codegen 在生成对象 `FengTypeDescriptor` 时读取该对象每个字段的 `FengAggregateValueDescriptor`，把其中每个 `FENG_SLOT_POINTER` 槽位翻译成一条 `FengManagedFieldDescriptor`。CC 看到的仍是平铺的托管指针表，不感知"这条来自一个聚合字段"。

#### 7.2.2 收益

- cycle collector 代码完全不动。
- `FengManagedFieldDescriptor` 结构不动（仅完成 §9.1 的命名重构，字段语义保持）。
- 完全符合 OCP：新增 aggregate 类型 → codegen 在生成对象描述符时多调一次展平逻辑，CC 不感知。

### 7.3 数组：元素分类升级

数组元素需要升级为三分类。第一阶段把改动纳入本草案的实现范围（不延后），否则 spec 数组用例无法运行。

具体规则：

- 数组创建处需要传入元素分类信息（trivial / managed-pointer / aggregate）。
- 对 aggregate 元素，数组内部存储其按值布局；元素 size 取自 `FengAggregateValueDescriptor.size`。
- 数组元素的 retain / release / assign / 数组销毁逐元素清理 → 对 aggregate 元素调用 §5 API。
- 数组的 `managed_fields` 等价物（运行时遍历入口）按元素分类做对应分派。

数组接口的具体改动属于 runtime 实现细节，本草案规定语义边界，详细 API 在实现阶段与现有 `feng_array_*` 一并定型并写入实现交付文档。

### 7.4 对象字段

对象字段为 aggregate 时：

- 字段在对象中按 aggregate 大小占位。
- 字段读写的 retain/release 由 codegen emit 时使用 §5 API。
- 对象的 `managed_fields` 按 §7.2 展平规则生成。
- 对象的 `release_children` 按字段分类，对 aggregate 字段调用 `feng_aggregate_release`。

## 8 fat object-form `spec` 在本模型下的映射

> 本节给出首发消费者的具体落点示意。完整的 spec 发码方案以 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) 的更新版本为准；该文档将在本草案落地后同步更新。

### 8.1 类型表示

```c
typedef struct FengSpecValue__demo__Named {
    void *subject;                                   /* 托管指针 */
    const struct FengSpecWitness__demo__Named *witness; /* 静态只读表，非托管 */
} FengSpecValue__demo__Named;
```

按值聚合，无 header，不参与 ARC 自身。

### 8.2 描述符

```c
static const FengManagedSlotDescriptor FengSpec__demo__Named__slots[] = {
    { offsetof(FengSpecValue__demo__Named, subject),
      FENG_SLOT_POINTER, NULL },
};

static void FengSpec__demo__Named__default_init(void *out);

static const FengAggregateDefaultInitDescriptor FengSpec__demo__Named__default = {
    .kind = FENG_DEFAULT_INIT_FN,
    .init_fn = FengSpec__demo__Named__default_init,
};

static const FengAggregateValueDescriptor FengSpecAgg__demo__Named = {
    .name = "demo.Named",
    .size = sizeof(FengSpecValue__demo__Named),
    .default_init = &FengSpec__demo__Named__default,
    .managed_slot_count = 1,
    .managed_slots = FengSpec__demo__Named__slots,
};
```

### 8.3 C ABI

fat spec 作为参数 / 返回值时，使用**具名 C struct 按值传递**。每个 object-form `spec` 生成独立 `FengSpecValue__<module>__<name>` 类型；调用方与被调方使用同一类型签名，C 编译器负责寄存器/栈分配（多数 64-bit 平台两指针 struct 走寄存器）。

不采用统一二字段 `void* + void*` 的原因：会丢失 witness 静态类型，迫使 callsite 强转，易错。

### 8.4 生命周期路径

| 场景 | emit 调用 |
| --- | --- |
| 局部声明 `let s: Named` 无初值 | `feng_aggregate_default_init(&s, &FengSpecAgg__demo__Named)` |
| 拷贝 `let t = s` | `memcpy(&t, &s, ...); feng_aggregate_retain(&t, &desc)` |
| 赋值 `t = s` | `feng_aggregate_assign(&t, &s, &desc)` |
| 作用域退出 | `feng_aggregate_release(&s, &desc)` |
| 返回值 | 按 §7.3 take 路径或拷贝路径，由 codegen 根据移动分析决定 |
| 对象字段为 spec | §7.4 展平进对象 `managed_fields` |
| 数组元素为 spec | §7.3 元素分类为 aggregate |

## 9 实施阶段

### 9.1 Phase 1：模型落地 + fat spec 接入（spec 发码前置，必做）

不再分 Phase A / Phase B，因为 fat spec 的工期不允许"先空跑骨架"。

任务（建议按以下顺序执行，便于每一步独立通过回归）：

1. **重命名预备**：把现有 `FengManagedFieldEntry` 重命名为 `FengManagedFieldDescriptor`。涉及 [src/runtime/feng_runtime.h](../src/runtime/feng_runtime.h)、[src/codegen/codegen.c](../src/codegen/codegen.c)、[test/runtime/test_runtime.c](../test/runtime/test_runtime.c)。**已交付**。
2. runtime：新增 `FengManagedSlotKind` / `FengManagedSlotDescriptor` / `FengAggregateValueDescriptor` / `FengAggregateDefaultInitDescriptor` 类型与五个公共 API；内部实现 walker。**已交付**（src/runtime/feng_aggregate.c + VM-1 单测）。
3. runtime：单指针原语零修改；`FengManagedHeader` / `FengTypeTag` 零修改；cycle collector 数组分派按 §7.3 升级（属于本模型范围内的必要扩面，对外仍是描述符驱动）。**已交付**（VM-3）。
4. codegen：增加值类别分派 helper；为 aggregate 类型生成描述符与 init 函数；按 §7.2 展平规则生成对象 `managed_fields`；按 §7.4 生成对象字段 release 调用。**已交付**（VM-4 / VM-5；object-form spec 自动 emit `FengSpecAgg__M__S` + slot table + 默认 init stub；helpers 已就位待引用站点接入）。
5. codegen + runtime：数组元素分类升级（§7.3）。**已交付**（`feng_array_new_kinded` + 三分类 finalize + cycle collector 三分支）。
6. semantic：补充类型分类与抽象槽位描述的输出（§6.1）。**已交付**（VM-2：`FengSemanticValueKind` + 9 个单测）。
7. **fat spec 全链路接入**：替换 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) 的 box 路径。
   - 已交付：4b-α 已完全去除 box，object-form spec 走 fat 值；spec 形参 / 方法分派 / 局部 / 返回（subject-shortcut）已通。
   - **未交付（layer 范围之外）**：spec 局部清理切换到 `feng_aggregate_release`、spec 字段读写、默认零值、等值、spec 数组、return take 移动路径、fit-method witness。这些扩面**已转交 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) §13.2 / §13.3 跟踪**，不在本 layer 文档收口范围内（理由见 §9.4）。
8. 测试：**layer 范围已覆盖**——VM-1 五 API 单测、VM-2 semantic 分类单测、VM-3 数组三分类 6 单测、4b-α `spec_object_param.ff` 验证 fat spec 局部 / 参数 / 返回 / 方法分派；端到端 spec 字段 / 数组 / 默认 / 等值 / cycle collector 含 spec 字段对象的 smoke 与 §11 中尚未具备触发路径的项，转 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) §12 跟踪。

### 9.4 layer 封顶范围声明

本文档定义的"value-model layer"包含：runtime 描述符与五 API、cycle collector 与数组的描述符驱动分派、codegen 值类别分派 helper 与对象字段展平 / release emit、semantic 值类别分类 API。该 layer 已具备**支撑任意 aggregate 类型接入所需的全部抽象**——证据是 object-form spec 已经能自动 emit 描述符、对象字段位置已能按 §7.2 / §7.4 正确展平 / 释放，且整个 layer 通过 11 smokes + 4 unit suites 全量回归。

**不在 layer 范围内的工作**：spec 本身作为首发消费者的端到端站点接入（含字段写入侧 retain、默认 init 实体、等值 sidecar 消费、数组创建调用站点、return take 路径、fit-method witness），均属于 spec-codegen 的工程实施面，由 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) §13.2 / §13.3 单独跟踪。layer 不对这些站点接入的完成时间负责，但承诺不再扩面 / 不再变更已发布的 API。

### 9.2 Phase 2：tuple 接入（未来特性）

仅当 `docs/` 引入 tuple 规范后启动。预期工作量：

- semantic：tuple 类型的 `FengValueKind` 与槽位描述生成。
- codegen：tuple 字面量、解构、字段访问的 emit。
- runtime：**零修改**。
- 走 OCP 验证：本阶段若需要修改 §5 API、walker、CC 接入逻辑或数组接入逻辑，则证明 Phase 1 抽象不达标，必须返工。

### 9.3 Phase 3：值语义 struct（未来特性）

同 Phase 2，仅当语言规范引入后启动；runtime 同样应零修改。

## 10 验收标准（layer 已达成）

实现完成时必须满足：

- 单指针原语零修改 ✅。
- `FengManagedHeader` / `FengTypeTag` 零修改 ✅。
- cycle collector **数组元素遍历**升级为按元素三分类（trivial / managed-pointer / aggregate）分派；其余 CC 路径（phase15 BFS 主框架、phase 2 free 路径、对象 `managed_fields` 通用展平）零修改 ✅。理由：数组元素的物理布局是数组本身固有信息，无法通过 §7.2 对象 `managed_fields` 方式承载，因此元素层的描述符分派必须写在 CC 数组分支里；该扩面是描述符驱动而非按 spec 写死的特殊路径，对未来新增 aggregate 元素类型零修改。
- runtime 中**没有任何**专门为 fat spec 写的代码路径；全部通过描述符驱动 ✅。
- 新增按值聚合类型（tuple / 值语义 struct）经纸面推演，仅需新增 `FengAggregateValueDescriptor` + 必要时新增 `FengAggregateDefaultInitFn`，无需修改 §5 API、walker、CC 数组分派、对象字段展平 ✅。
- fat spec 默认零值机制（§6.5）：layer 已提供 `feng_aggregate_default_init` API；spec 端默认 init 实体由 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) §6 跟踪生成（4b-β 范围）。当前 codegen 为每个 object-form spec emit panic stub init fn，作为安全栅栏直至 4b-β 提供真实实现。
- fat spec 数组、含 spec 字段的对象、嵌套 spec 调用链路：layer 已提供 `feng_array_new_kinded` + `feng_aggregate_release` + 对象字段展平 helper；端到端站点接入由 spec-codegen-pending §13.2 / §13.3 跟踪。
- 所有现有 smoke 与单测零回归 ✅（11/11 smokes + 4 单元套件）。

## 11 测试面（layer 已覆盖）

layer 已覆盖：

- ✅ aggregate 五 API（retain / release / assign / take / default_init）的 5 项 runtime 单测（VM-1）。
- ✅ aggregate 自赋值安全（`feng_aggregate_assign(p, p, desc)`）通过 VM-1 中 assign 单测。
- ✅ aggregate take 路径源值置零通过 VM-1 中 take 单测。
- ✅ 数组元素三分类（trivial / managed-pointer / aggregate；含 INIT_FN policy 与 ZERO_BYTES policy）的 6 项 runtime 单测（VM-3）。
- ✅ semantic 值类别分类的 9 项单测（VM-2）。
- ✅ 现有 11 个 smoke 全量回归。

转 [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md) §12 跟踪：

- aggregate 局部 / 参数 / 返回的 retain / release 配平：spec 是首发消费者；4b-α 已经验证局部 / 参数 / 方法分派；字段 / 数组 / 默认 / 等值的端到端 smoke 由 4b-β / 4b-γ 提供。
- 含 aggregate 字段的对象进入 cycle collector 的端到端 smoke：等待 4b-β `spec_object_field.ff` 触发。
- 异常展开过程中 aggregate 局部变量被正确释放：等待 4b-β 切换 spec 局部清理为 `feng_aggregate_release` 后由现有 `exception_cleanup.ff` 隐式覆盖。
- fat spec 等值按 subject 身份比较：4b-β `spec_equality.ff`。

## 12 OCP 检查清单（每次新增聚合类型时回看）

新增任何按值聚合类型时，下列项**必须全部为"无需修改"**：

- [x] runtime 单指针原语
- [x] `FengManagedHeader` / `FengTypeTag`
- [x] cycle collector 数组分派的整体结构（按描述符分派，不按 spec 类型写死）
- [x] cycle collector 对象字段展平路径（通用展平，不感知具体 aggregate 类型）
- [x] §5 五个聚合 API 与 walker
- [x] codegen 值类别分派 helper 的整体结构
- [x] 数组元素分类分派的整体结构
- [x] 对象字段展平规则的整体结构

允许且仅允许的新增工作：

- [x] 新生成一个 `FengAggregateValueDescriptor`
- [x] 必要时新生成一个 `FengAggregateDefaultInitFn`
- [x] 在该类型的引用站点 emit 已有 helper 的调用

任一项突破清单，则视为抽象不达标，必须先回到本草案讨论修订。
