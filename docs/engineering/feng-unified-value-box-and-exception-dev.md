# Feng 统一 ValueBox 与 throw/catch 类型对齐开发方案

> **状态**：待 Review，尚未实施。
>
> **性质**：工程实现提案，不定义新的语言规则。异常载荷类型的最终权威规则仍应在实施阶段先更新
> [`feng-exception.md`](../specifications/feng-exception.md)；值类型、enum 与 spec 的既有语言语义分别以
> [`feng-type.md`](../specifications/feng-type.md)、[`feng-tuple.md`](../specifications/feng-tuple.md)、
> [`feng-enum.md`](../specifications/feng-enum.md) 和 [`feng-spec.md`](../specifications/feng-spec.md) 为准。

## 1. 目标与实施顺序

本方案分为两个严格串行的阶段：

1. **统一 ValueBox**：先将标量、enum、tuple 与 `@value type` 的逃逸装箱统一为按具体类型静态生成的
   `ValueBox<T>` 模型，保持现有语言行为和异常匹配行为不变；
2. **对齐 throw/catch**：在统一箱模型稳定后，再统一 `throw` 与具体类型 `catch` 的类型许可规则，删除
   异常专用描述符，并通过箱描述符或托管对象描述符完成精确匹配。

两个阶段不得合并提交。阶段一完成全部定向测试和全量回归后，才能开始阶段二。这样可以独立验证：

- 阶段一只改变值的托管承载方式，不改变异常语言规则；
- 阶段二只消费已经统一的托管载荷模型，不再同时重构装箱基础设施。

## 2. 当前实现与问题

### 2.1 当前值描述符

Feng 当前根据值的物理表示使用三类整体描述符：

| 描述符 | 描述对象 | 示例 |
|---|---|---|
| `FengTrivialDescriptor` | 未装箱的平凡值 | `i32`、`bool`、enum、trivial tuple、trivial `@value type` |
| `FengAggregateDescriptor` | 未装箱且含托管槽位的聚合值 | aggregate tuple、aggregate `@value type` |
| `FengTypeDescriptor` | 含 `FengManagedHeader` 的托管堆对象 | 普通 `type`、string、array、closure、各种 box |

每种内建标量已经有自己的 `FengTrivialDescriptor`，例如 `feng_i32_descriptor` 和
`feng_f64_descriptor`；每个具名 enum 也已经有声明级唯一的 `FengTrivialDescriptor`。这些描述符描述
未装箱值，不可作为托管箱对象的 `FengManagedHeader.desc`。

### 2.2 当前两套箱模型

内建标量共用一个 runtime 预编译箱：

```c
typedef struct FengScalarBox {
    FengManagedHeader header;
    FengBuiltinScalarKind kind;
    union {
        bool b;
        int8_t i8;
        int16_t i16;
        int32_t i32;
        int64_t i64;
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        float f32;
        double f64;
    } payload;
} FengScalarBox;
```

所有 `FengScalarBox` 的 `header.desc` 都指向同一个 `feng_scalar_box_descriptor`；`kind` 只能区分内建
物理标量种类，不能区分底层同为 `i32` 的普通 `i32` 与不同具名 enum。

tuple 与 `@value type` 已使用每类型独立的箱：

```c
struct FengValueBox__Point {
    FengManagedHeader header;
    struct Feng__Point value;
};

const FengTypeDescriptor FengValueBoxDesc__Point = { /* ... */ };
```

即使两个具名 Value 的字段布局完全相同，其箱描述符地址仍不同，因此已经具备名义类型身份。

### 2.3 当前 spec 适配

object-form spec 的 fat value 仍为 `subject + witness`。当标量 subject 必须逃逸时，Codegen 创建
`FengScalarBox`；witness thunk 将 `subject` 转回 `FengScalarBox *` 并读取 `payload.<kind>`。tuple 与
`@value type` 则创建各自的 value box，witness thunk 读取 `box->value`。

因此 spec 成员协议本身没有两套，但其 subject 装箱与取值适配存在两套实现。

### 2.4 当前异常匹配

`feng_throw` 接受一个规范化后的托管指针和一个 `FengTypeDescriptor *`。普通 `type`、string 等托管引用
直接传递其对象描述符；标量装入 `FengScalarBox` 后却另外传递
`feng_scalar_<kind>_exception_descriptor`。这导致：

```c
((FengManagedHeader *)value)->desc != exception->desc;
```

标量箱的对象描述符与异常匹配描述符不是同一个描述符。enum 如果复用底层 `i32` 异常描述符，则无法
保持名义类型身份；如果为 enum 新增异常专用描述符，则会进一步扩大异常特判。

此外，Semantic 对 `throw` 与具体类型 `catch` 使用了不同的类型许可规则。数组只有共享对象描述符，
不同闭合元素类型不能仅靠当前描述符指针区分；部分 spec 形态能通过 `throw` 语义检查，却无法被具体
`catch` 正确匹配，甚至会进入错误 Codegen 路径。

## 3. 总体设计约束

### 3.1 统一不变量

最终实现必须满足：

```c
exception->desc == ((const FengManagedHeader *)exception->value)->desc;
```

即 `feng_throw` 接收的描述符始终描述实际传入的托管对象。异常运行时只处理“托管对象指针 + 该对象的
`FengTypeDescriptor`”，不感知标量、enum、tuple、`@value type` 或装箱过程。

### 3.2 一个箱抽象，不是一个动态物理结构

所有需要逃逸装箱的值类型统一建模为：

```c
struct ValueBox<T> {
    FengManagedHeader header;
    T value;
};
```

这是统一的静态生成模式，不是包含动态字节区、原始描述符字段或运行时大小分派的单一通用 C 结构。
每个具体 `T` 仍有自己的 C 箱类型与 `FengTypeDescriptor`，从而让 C 编译器静态确定大小、对齐和字段
访问。

禁止采用以下设计：

```c
struct DynamicValueBox {
    FengManagedHeader header;
    const void *value_descriptor;
    unsigned char payload[];
};
```

该设计会增加每对象空间、动态大小计算和运行时分派，不符合本方案的性能约束。

### 3.3 每个具体值类型有两种表示描述符

| 表示 | 描述符 | 职责 |
|---|---|---|
| 未装箱值 `T` | `FengTrivialDescriptor` 或 `FengAggregateDescriptor` | 值大小、相等、默认初始化、聚合生命周期 |
| 托管箱 `ValueBox<T>` | `FengTypeDescriptor` | 箱大小、ARC、子引用释放、箱对象身份 |

箱对象不增加“原始值描述符”字段，箱描述符也不要求增加到原始值描述符的通用反向链接。Codegen 在静态
类型已知的装箱、解箱和生命周期站点直接引用两者。若未来确有运行时反查需求，应另行设计，不能借本次
变更增加字段或查找成本。

### 3.4 不增加 `type_identity`

不增加 `type_identity` 字段、数值类型 ID、字符串匹配或描述符哈希。箱描述符或普通托管对象描述符的
指针地址本身就是运行时对象类型身份。`FengUnwindException.desc` 与 `FengCatchClause.type` 继续使用
`const FengTypeDescriptor *`。

### 3.5 性能约束

**硬性要求：本方案不得引入任何增量运行时开销。所有受影响路径的执行时间、动态分配次数、分配空间、
运行时分支、间接访问、descriptor 查询或比较以及 ARC/CC 操作数量，只能小于或等于变更前基线。任一项
确认高于当前开销即视为方案不合格，不能以功能正确、测试通过或回退幅度较小为由接受。**

两个阶段均不得：

- 为原本不装箱的表达式、普通参数传递、算术或泛型直接调用新增装箱；
- 增加运行时类型查找、字符串比较、哈希、descriptor-to-descriptor 映射或 `kind` 分派；
- 增加 box 对象字段或异常对象字段；
- 增加 throw/catch 匹配的比较次数或改变其描述符指针比较机制；
- 将 trivial 值引入 aggregate walker；
- 改变普通路径 ARC 或 cycle collector 的行为。

类型化标量箱应删除 `kind + 最大 union`，其分配大小不得大于现有 `FengScalarBox`。直接路径必须通过既有
性能约束脚本和生成 C 断言证明未新增装箱。

### 3.6 强制回归与人工决策约束

以下要求是两个阶段均不可豁免的交付红线：

1. `fcts/` 下的全部用例必须全量通过，不允许删除、跳过、标记预期失败或降低断言；
2. `std/std_test/`（标准库 `std_test` 工程）下的全部用例必须全量通过，不允许删除、跳过、标记预期失败
   或降低断言；
3. `test/` 下除“直接强断言旧 `FengScalarBox` 结构、符号、构造 API 或生成 C 文本”的既有用例外，
   其余用例必须保持原样并全量通过；
4. 旧标量箱强断言用例不能直接删除或跳过，必须迁移为对统一 `ValueBox<T>` 新结构、descriptor、生成 C
   和行为的等价或更强断言；
5. 开始修改测试前，必须列出拟修改的旧标量箱强断言用例精确清单（文件、测试函数、旧断言及修改原因），
   由人工确认；未列入人工确认清单的任何既有 `test/` 用例均不得修改；
6. 不得通过放宽断言、扩大容差、减少覆盖、吞掉诊断或新增 skip/xfail 绕过回归；
7. 任一非人工批准修改范围内的既有测试失败，都直接阻断当前阶段完成；
8. 不得引入任何增量运行时开销；所有受影响路径的运行时开销必须小于或等于变更前基线，并同时满足
   §3.5 的结构性约束与验证要求；
9. 对语言许可集合、descriptor 身份、跨 package 链接、ABI、生命周期、性能影响、测试归类或实现取舍存在
   任何不确定时，必须停止对应工作，陈列事实与可选方案并交由人工决策，禁止自行选择或增加特判。

本文中的建议项只有在 §9 Review 后才成为实施决策；实施期间发现本文未覆盖的新问题，不得以“沿用总体
方向”为由自行扩展范围。

## 4. 阶段一：统一 ValueBox

### 4.1 阶段目标

将 `bool`、数值标量、enum、tuple 与 `@value type` 的逃逸装箱统一为 `ValueBox<T>`。本阶段不改变
`throw`/`catch` 的语言许可集合、匹配规则或诊断；现有异常专用描述符可作为过渡兼容保留到阶段二。

### 4.2 箱类型与符号

建议统一使用与用途无关的命名：

```text
FengValueBox__<canonical-type-mangle>
FengValueBoxDesc__<canonical-type-mangle>
FengValueBoxReleaseChildren__<canonical-type-mangle>
```

现有 tuple/`@value type` 的 `__spec_box` 命名应迁移到通用 `value_box` 命名，因为箱不再只服务 spec。
生成符号属于 Feng 内部 ABI，迁移要求全量重编译，不改变 `@abi` C surface。

内建标量的箱类型与箱描述符由 runtime 提供唯一全局符号，避免不同模块分别生成而破坏指针身份。
`int` 等平台相关别名复用其规范化底层标量箱。具名 enum、tuple、`@value type` 及闭合泛型 Value 的箱
描述符按完整名义类型生成稳定符号；公开或跨包类型沿用现有外部可见/弱定义规则，必须保证链接后的描述符
地址唯一。

### 4.3 标量与 enum

标量和 enum 不再生成或使用第三类箱结构，均是通用 `ValueBox<T>` 的具体化：

```c
struct FengValueBox__i32 {
    FengManagedHeader header;
    int32_t value;
};

struct FengValueBox__demo__Color {
    FengManagedHeader header;
    FengEnum__demo__Color value;
};
```

本阶段删除：

- `FengBuiltinScalarKind`；
- `FengScalarBox` 的共享 `kind + union` 结构；
- `feng_scalar_box_descriptor`；
- `feng_scalar_box_new_<kind>` 构造函数族；
- Codegen 中的 scalar payload 字段映射与 scalar box 构造函数映射。

本阶段用类型化 box 的 `.value` 直接写入和读取。enum 的箱是 `ValueBox<Enum>` 的普通具体化，不引入
enum 专用箱抽象、运行时 enum 分支或 enum `kind`。

### 4.4 tuple 与 `@value type`

tuple 与 `@value type` 继续复用现有每类型独立箱结构，但收敛命名和通用生成入口：

- trivial Value 直接赋值到 `box->value`；
- aggregate Value 通过其 `FengAggregateDescriptor` 调用 `feng_aggregate_assign`；
- 含托管槽位时，箱描述符的 `release_children` 继续调用 `feng_aggregate_release`；
- 箱 descriptor 的 `managed_fields` 继续按现有规则从 `value` 内部槽位生成；
- 不改变原始值描述符和五类 aggregate API。

两个布局相同但声明不同的具名 tuple/`@value type` 必须拥有不同的箱描述符。

### 4.5 Codegen 抽象

现有 box 元数据主要挂在 `UserType.c_value_box_*`，无法直接服务内建标量和 enum。Codegen 应引入按完整
具体类型键控的统一 box 注册表/查询入口，至少提供：

```text
ensure ValueBox<T> symbols/definition
get ValueBox<T> C struct name
get ValueBox<T> FengTypeDescriptor symbol
emit box allocation and value initialization
emit statically typed unbox access
```

调用方只提交 `CGType` 与责任 token，不得分别维护 scalar、enum、tuple、`@value type` 四套装箱逻辑。
物理生命周期仍由类型分类决定：trivial 走直接赋值，aggregate 走 descriptor 驱动的 assign/release。

### 4.6 object-form spec 适配

spec fat value、witness ABI、witness 缓存键和 `subject: void *` 表示保持不变。只调整装箱适配层：

```text
当前逃逸标量：FengScalarBox.payload.<kind>
统一后：      ValueBox<T>.value
```

具体要求：

- `BORROW_LOCAL` 路径继续直接借用栈上原始值地址，不装箱；
- `BOX_OWNER` 更名或更新注释为通用 ValueBox owner，不再绑定 `FengScalarBox`；
- 逃逸的 builtin scalar 与 enum subject 使用各自 `ValueBox<T>`；
- witness getter、setter、type method 与 fit method thunk 从静态已知的 box 类型读取 `.value`；
- tuple 与 `@value type` 的现有 value-box witness 路径接入同一查询入口；
- spec 成员分派仍是一次 witness 间接调用，不能增加运行时类型判断；
- spec subject 的 retain/release 和逃逸所有权规则保持不变。

箱对象相等行为也必须保持阶段一变更前的结果，包括浮点边界值。类型化 box 可以使用按具体 `T` 静态
生成或 runtime 预定义的 `equal_fn`，但不得借重构改变 object-form spec subject 的既有相等语义；任何
相等语义调整都应作为独立语言变更处理。

### 4.7 阶段一中的异常兼容

由于当前 `throw` 也依赖 `FengScalarBox`，阶段一必须让标量和 enum 的异常载荷改用新的 `ValueBox<T>`，
但暂不改变 Semantic 许可集合和 catch 匹配语义。现有异常匹配描述符在阶段一可以暂时保留，作为明确的
过渡层：

```text
ValueBox<T> 负责载荷承载与生命周期
现有 exception descriptor 继续负责旧匹配行为
```

阶段一不得借机修正 enum、array 或 spec 的异常匹配结果；这些变化必须留到阶段二单独验证。阶段一完成后
文档和代码中必须明确标记该过渡层，并在阶段二完整删除，不能作为永久方案。

### 4.8 阶段一验证

Runtime/Codegen 测试至少覆盖：

- 11 种内建标量的类型化 box 布局、descriptor、构造、读取和释放；
- enum 的类型化 box，两个不同 enum 即使底层值相同也拥有不同箱描述符；
- trivial 与 aggregate tuple/`@value type` 的装箱、复制、解箱和子引用释放；
- 闭合泛型 Value box 的 descriptor 唯一性和跨模块符号一致性；
- spec `BORROW_LOCAL` 不装箱；
- spec `BOX_OWNER` 对 bool、整数、浮点、enum、tuple、`@value type` 正确装箱；
- 从 spec 视角访问成员、调用 type method/fit method，返回值与 mutation 语义不变；
- spec 值复制、返回、字段保存和释放不泄漏、不重复释放；
- 普通标量运算、参数传递、泛型直接调用和非逃逸 spec 临时调用不新增装箱；
- 现有 throw/catch 行为在阶段一保持不变。

阶段一完成后运行所有定向测试、性能约束测试，并在沙箱外执行完整 `make test`。

## 5. 阶段二：throw/catch 类型对齐

### 5.1 先更新语言主规范

阶段二开始前必须先更新 [`feng-exception.md`](../specifications/feng-exception.md)，明确 `throw` 与具体类型
`catch` 使用完全相同的有限类型集合。建议允许：

- `bool` 与数值标量；
- `string`；
- 具名 enum；
- 普通实体 `type`；
- 具名 tuple；
- `@value type`；
- 上述用户类型的闭合泛型实例；
- 普通 `@abi type` 与 `@value @abi type`。

建议拒绝：

- array；
- 所有形式的 `spec`；
- 直接泛型类型参数和仍含开放类型参数的类型；
- pointer、`void`；
- 函数值、函数类型和成员方法。

`@abi` 只增加 C ABI 投影，不抹除 Feng 类型身份；允许 `@abi type` 作为异常载荷不改变“异常不得跨
`@abi func` 边界传播”的既有规则。

上述集合必须经 Review 后写入主规范，不能只存在于本工程文档。

### 5.2 统一 Semantic 分类

Semantic 只保留一个异常载荷分类入口，由以下站点共同调用：

- `throw <expr>`；
- `catch ex: Type`；
- 必要的异常逃逸/边界检查。

分类结果必须包含稳定诊断原因，确保相同类型在 throw 与 catch 两侧不会出现一侧接受、另一侧拒绝。
`catch ex: unknown` 与匿名 `catch` 是兜底匹配，不进入具体载荷类型分类；`catch ex: unknown` 中的
`throw ex` 是原样重抛，也不创建 `unknown` 类型的新载荷。

### 5.3 统一 Codegen 与运行时不变量

阶段二删除全部异常专用描述符，包括现有：

```text
feng_scalar_bool_exception_descriptor
feng_scalar_i8_exception_descriptor
...
feng_scalar_f64_exception_descriptor
```

不得增加 `FengEnumExceptionDesc__...` 或其他只被 throw/catch 使用的描述符。

规范化规则统一为：

| Feng 值 | 传入 `feng_throw` 的托管载荷 | `exception->desc` |
|---|---|---|
| 标量、`bool` | `ValueBox<T> *` | `ValueBox<T>` 的箱描述符 |
| enum | `ValueBox<Enum> *` | 该具名 enum 的箱描述符 |
| tuple、`@value type` | `ValueBox<T> *` | 该具体 Value 的箱描述符 |
| 普通实体 `type` | 原对象指针 | 原对象的 `FengTypeDescriptor` |
| string | 原字符串指针 | `feng_string_descriptor` |

所有路径均满足：

```c
((const FengManagedHeader *)value)->desc == desc;
```

`feng_throw(void *value, const FengTypeDescriptor *desc)`、`FengUnwindException.desc`、
`FengCatchClause.type` 与 personality 的描述符指针比较机制保持不变，不增加 `type_identity`、不改为
`const void *`、不读取 descriptor 内容。

### 5.4 精确匹配

具体类型 catch 继续通过描述符地址完全相等匹配：

```c
clause->type == exception->desc
```

因此必须保证：

- `i32` 与 `i64` 的箱描述符不同；
- `i32` 与底层为 `i32` 的 enum 箱描述符不同；
- 两个不同具名 enum 的箱描述符不同；
- 两个布局相同的具名 tuple/`@value type` 箱描述符不同；
- 闭合泛型实例按完整类型参数区分；
- 普通 `@abi type` 与同布局的其他类型按其普通 `FengTypeDescriptor` 区分。

catch 命中后，普通托管引用直接绑定；值类型根据 catch 的静态类型从 `ValueBox<T>.value` 解箱。匹配
阶段不读取 box 内容，不检查 `kind`，不执行运行时类型转换。

### 5.5 为什么阶段二拒绝 array 与 spec

所有 `FengArray` 当前共享 `feng_array_descriptor`。元素种类和元素生命周期元数据位于数组实例中，但
`FengManagedHeader.desc` 不能仅靠地址区分 `i32[]`、`string[]` 等闭合数组类型。若在 catch 搜索中读取
实例元素元数据，将引入额外分派并改变当前 O(1) 描述符身份模型，因此本阶段拒绝 array。未来若数组获得
闭合类型唯一的普通对象描述符，可在独立设计中重新评估。

spec 是抽象或联合值，不是单一具体托管实体类型：

- object/intersection-form spec 的 witness 表达“具体 subject 满足 spec”，不应替代具体 subject 类型；
- union-form spec 可能携带多个成员表示，不能统一访问 `.subject`；
- callable-form spec 是函数值，不是异常数据实体。

因此所有 spec 在 Semantic 阶段拒绝。若调用方需要抛出 object-form spec 的具体 subject，应在静态类型仍
可见时抛出具体类型；若需要抛出 union-form spec 成员，应先收窄并取出具体成员。

### 5.6 阶段二验证

Semantic 测试必须对允许与禁止集合中的每个类别分别覆盖 `throw` 和具体类型 `catch`。Codegen/FCTS
至少覆盖：

- 所有内建标量的 throw、精确 catch 与错误标量不匹配；
- enum 只命中同一具名 enum，不命中 `i32` 或其他 enum；
- 跨 package 的 public enum throw/catch 描述符身份一致；
- 两个字段布局相同的具名 tuple 不互相匹配；
- trivial/aggregate `@value type` 精确匹配并正确释放托管字段；
- 普通实体 type、string 与闭合泛型 type 的既有行为；
- 普通 `@abi type` 与 `@value @abi type` 精确匹配；
- array、所有 spec、开放泛型、pointer、`void`、函数值和成员方法在 Semantic 阶段拒绝；
- `unknown`/匿名 catch 兜底顺序与原样重抛；
- catch 中值类型解箱后的字段访问、方法调用、返回与再次抛出；
- 未捕获异常和 catch 正常完成时，载荷恰好释放一次；
- `examples/hello_world` 中类似 `throw self._error` 的 spec/开放抽象载荷得到稳定的 Semantic 诊断，
  不进入无效 C Codegen。

阶段二完成后运行所有定向测试、FCTS、smoke、性能约束测试，并在沙箱外执行完整 `make test`。

## 6. 变更范围

### 6.1 阶段一预计涉及

- `src/runtime/feng_runtime.h`：移除 scalar box 结构/API，声明内建类型化 ValueBox；
- `src/runtime/feng_scalar_box.c`：迁移为内建 ValueBox 实现，或由职责准确的新模块替代；
- `src/codegen/codegen.c`：统一 box registry、装箱/解箱、spec subject 与 witness thunk；
- `src/semantic/semantic.h`：将 `BOX_OWNER` 注释从 `FengScalarBox` 收敛为通用 ValueBox；
- runtime/Codegen/FCTS/performance tests；
- 已交付工程文档中关于“ScalarBox 与 per-type box 不合并”的历史结论。

### 6.2 阶段二预计涉及

- `docs/specifications/feng-exception.md` 与关联手册；
- `src/semantic/analyzer.c`：统一异常载荷分类；
- `src/codegen/codegen.c`：统一异常载荷 descriptor、value box catch 解箱；
- `src/runtime/feng_runtime.h`、`src/runtime/feng_scalar_box.c`：删除异常专用 descriptor 声明和定义；
- Semantic/Codegen/runtime/FCTS/smoke tests；
- `bug-union-spec-throw.md` 与 `feng-exception-dev.md` 状态和实现说明。

## 7. 非目标

本方案不包含：

- 允许异常跨越 `@abi func` 边界；
- 允许按 spec 做 catch 多态匹配；
- 为 array 增加闭合类型对象描述符；
- 为 pointer、函数或开放泛型增加装箱与异常身份；
- 修改 `FengManagedHeader`、`FengTypeDescriptor`、`FengAggregateDescriptor` 的结构布局；
- 修改 spec witness ABI、fat value 布局或动态分派协议；
- 修改普通实体 `type` 的存储模型；
- 借本次变更处理无关的默认值、泛型字段或 defer 问题。

## 8. 分步实施 TODO

以下任务按依赖顺序排列。只有一个任务的实现、测试与检查均完成后，才能将其标记为 `[x]`。阶段完成
门禁不得在任一前置项未完成时提前勾选。

### 8.1 Review 与变更前基线

- [ ] 完成本方案 §9 的全部人工 Review 决策并记录结论；
- [ ] 确认工作区只包含本方案已经 Review 的文档变更；
- [ ] 在沙箱外运行变更前完整 `make test`，记录各测试套件数量与结果；
- [ ] 保存当前 scalar spec `BORROW_LOCAL` 与 `BOX_OWNER` 的代表性生成 C 片段；
- [ ] 保存当前标量、enum、tuple、`@value type` throw/catch 的代表性生成 C 片段；
- [ ] 记录 `FengScalarBox`、11 个标量原始值描述符、共享箱描述符和异常专用描述符的基线符号；
- [ ] 运行既有性能约束并记录直接标量、泛型标量与非逃逸 spec 路径的基线；
- [ ] 记录所有受影响路径的动态分配次数与大小、运行时分支和间接访问、descriptor 比较以及 ARC/CC 操作
  基线，作为两个阶段不可放宽的比较上限；
- [ ] 列出 `test/` 下直接强断言旧标量箱的用例精确清单；
- [ ] 将拟修改的每个旧标量箱测试函数、旧断言和迁移目标提交人工确认；
- [ ] 记录人工批准修改的 `test/` 用例白名单，阶段一不得修改白名单外的既有用例。

### 8.2 阶段一文档

- [ ] 更新值模型工程文档，定义所有可装箱具体值类型统一使用 `ValueBox<T>`；
- [ ] 更新 `@value type` 工程文档中“`FengScalarBox` 与 per-type box 不合并”的历史结论；
- [ ] 更新 builtin fit/spec 工程文档中的 scalar box subject 表示；
- [ ] 明确记录阶段一不改变 throw/catch 语言许可集合和匹配行为；
- [ ] 明确记录 `ValueBox<T>` 是静态具体化模式，不是动态 payload 容器；
- [ ] 明确记录每个具体值类型分别拥有原始值描述符和箱描述符；
- [ ] 明确记录箱中不增加原始值描述符字段，descriptor 中不增加反向链接。

### 8.3 阶段一 Runtime：内建 ValueBox

- [ ] 为 `bool` 与 10 种数值标量定义类型化内建 `ValueBox<T>` C 结构；
- [ ] 为 `bool` 与 10 种数值标量定义全局唯一的内建箱 `FengTypeDescriptor`；
- [ ] 保证 `int` 按目标平台复用规范化 `i32` 或 `i64` 箱类型及描述符；
- [ ] 为每种内建箱提供无额外分派的构造/初始化路径；
- [ ] 保持当前 scalar object-form spec subject 的相等行为，包括 `f32`/`f64` 边界值；
- [ ] 删除 `FengBuiltinScalarKind`；
- [ ] 删除共享 `FengScalarBox` 的 `kind + union` 结构；
- [ ] 删除 `feng_scalar_box_descriptor`；
- [ ] 删除 `feng_scalar_box_new_<kind>` 构造函数族；
- [ ] 检查 runtime 公开头中不再残留 `FengScalarBox` 或 scalar-box API。

### 8.4 阶段一 Codegen：统一 Box 抽象

- [ ] 定义按完整具体 `CGType` 键控的统一 box registry/key；
- [ ] 实现查询/确保 `ValueBox<T>` C struct 符号的统一入口；
- [ ] 实现查询/确保 `ValueBox<T>` descriptor 符号的统一入口；
- [ ] 实现 trivial `T` 的静态 box 分配与直接 `.value` 初始化；
- [ ] 实现 aggregate `T` 的 box 分配与 `feng_aggregate_assign` 初始化；
- [ ] 实现 trivial `T` 的静态 `.value` 解箱；
- [ ] 实现 aggregate `T` 的 `feng_aggregate_assign` 解箱；
- [ ] 将 tuple/`@value type` 的 `UserType.c_value_box_*` 路径接入统一 box 查询入口；
- [ ] 将现有 `__spec_box` 内部符号迁移为用途无关的 `value_box` 命名；
- [ ] 为每个具名 enum 生成声明级唯一的 `ValueBox<Enum>` descriptor；
- [ ] 为 public/imported enum 保证跨 package 的箱 descriptor 符号地址唯一；
- [ ] 为闭合泛型 Value 保证按完整类型参数生成唯一箱 descriptor；
- [ ] 删除 scalar payload 字段映射 helper；
- [ ] 删除 scalar box 构造函数名称映射 helper；
- [ ] 删除 Codegen 中只服务 `FengScalarBox` 的状态和 guard。

### 8.5 阶段一 Codegen：spec 适配

- [ ] 将 `FENG_SPEC_OBJECT_SUBJECT_STORAGE_BOX_OWNER` 文档化为通用 ValueBox owner；
- [ ] 保持 scalar/enum spec `BORROW_LOCAL` 路径直接借用原始值地址；
- [ ] 将逃逸 builtin scalar spec subject 改为对应的 `ValueBox<T>`；
- [ ] 将逃逸 enum spec subject 改为对应的 `ValueBox<Enum>`；
- [ ] 将 builtin scalar witness thunk 从 `payload.<kind>` 改为静态 box 类型的 `.value`；
- [ ] 将 enum witness thunk 从 `payload.i32` 改为 `ValueBox<Enum>.value`；
- [ ] 将 tuple/`@value type` witness thunk 接入相同的 box 信息查询；
- [ ] 验证 spec getter、setter、type method 与 fit method thunk 不增加类型判断；
- [ ] 验证 spec fat value、witness ABI、witness cache key 和 subject 所有权协议没有变化。

### 8.6 阶段一 Codegen：异常兼容迁移

- [ ] 将标量 throw 载荷从 `FengScalarBox` 迁移为对应的 `ValueBox<T>`；
- [ ] 将 enum throw 载荷迁移为对应的 `ValueBox<Enum>`；
- [ ] 将标量 catch 解箱从 `payload.<kind>` 迁移为静态 box 类型的 `.value`；
- [ ] 保留阶段一所需的现有异常匹配 descriptor，保证匹配行为不变；
- [ ] 为过渡匹配 descriptor 添加明确的阶段二删除标记，禁止扩展为新永久抽象；
- [ ] 验证阶段一没有新增 enum 异常专用 descriptor；
- [ ] 验证阶段一没有改变 array/spec 的现有 Semantic 接受或拒绝行为。

### 8.7 阶段一测试与完成门禁

- [ ] Runtime 测试覆盖 11 种内建 ValueBox 的 header descriptor、值读写、相等与释放；
- [ ] Runtime/Codegen 测试证明类型化标量箱不大于原 `FengScalarBox`；
- [ ] Codegen 测试覆盖 builtin scalar、enum、trivial Value、aggregate Value 的 box 生成；
- [ ] Codegen 测试覆盖 public/imported enum 与闭合泛型 Value 的 descriptor 唯一性；
- [ ] FCTS 覆盖 scalar/enum spec 的 `BORROW_LOCAL` 与 `BOX_OWNER`；
- [ ] FCTS 覆盖 spec getter、setter、type method、fit method、返回、字段保存和复制；
- [ ] FCTS 覆盖 tuple/`@value type` 装箱行为无回归；
- [ ] 异常现有用例证明阶段一 throw/catch 行为无变化；
- [ ] ARC/CC 测试证明箱及其托管子字段恰好释放一次；
- [ ] 运行性能约束，证明直接标量、泛型直接调用和非逃逸 spec 路径不新增装箱；
- [ ] 对照变更前基线，证明阶段一所有受影响路径的运行时开销逐项小于或等于当前开销；
- [ ] 检查生成 C 中不再出现 `FengScalarBox`、`payload.<kind>` 或 scalar-box constructor；
- [ ] 检查 runtime/generated symbols 中不再出现共享 scalar-box 符号；
- [ ] 确认仅修改人工批准白名单内的旧标量箱强断言用例；
- [ ] 将白名单内旧标量箱断言迁移为统一 ValueBox 的等价或更强断言，不删除、不跳过；
- [ ] `test/` 下除白名单内表示迁移用例外的全部既有用例保持原样并通过；
- [ ] `test/` 下迁移后的标量箱相关用例全部通过；
- [ ] `fcts/` 下全部用例全量通过；
- [ ] `std/std_test/` 下全部用例全量通过；
- [ ] 运行所有阶段一定向测试；
- [ ] 在沙箱外运行完整 `make test`；
- [ ] **阶段一完成门禁**：以上阶段一任务全部完成，`test/`、`fcts/`、`std/std_test/` 满足 §3.6 强制
  要求，完整回归结果已记录，允许开始阶段二。

### 8.8 阶段二语言规范与工程文档

- [ ] 更新 `feng-exception.md`，定义 throw 与具体类型 catch 共用的允许集合；
- [ ] 更新 `feng-exception.md`，定义 throw 与具体类型 catch 共用的拒绝集合；
- [ ] 在主规范中明确具名 enum、具名 tuple 与闭合泛型按精确类型身份匹配；
- [ ] 在主规范中明确普通 `@abi type` 与 `@value @abi type` 可作为异常载荷；
- [ ] 在主规范中保持异常不得跨 `@abi func` 边界；
- [ ] 在主规范中明确 array 因缺少闭合类型对象 descriptor 而暂不允许；
- [ ] 在主规范中明确所有 spec、开放泛型、pointer、`void` 与 callable 不允许；
- [ ] 更新关联语言手册，只引用主规范而不重复定义独立规则；
- [ ] 更新异常工程文档中的规范化载荷表和 descriptor 不变量；
- [ ] 将 `bug-union-spec-throw.md` 的方案收敛到统一 Semantic 拒绝路径。

### 8.9 阶段二 Semantic

- [ ] 实现 `throw` 与具体类型 `catch` 共用的异常载荷分类入口；
- [ ] 接受 bool、数值标量、string、具名 enum；
- [ ] 接受普通实体 type、具名 tuple 与 `@value type`；
- [ ] 接受上述用户类型的闭合泛型实例；
- [ ] 接受普通 `@abi type` 与 `@value @abi type`；
- [ ] 拒绝 array；
- [ ] 拒绝所有形式的 spec；
- [ ] 拒绝直接类型参数和仍含开放类型参数的类型；
- [ ] 拒绝 pointer、`void`、函数值、函数类型和成员方法；
- [ ] 保持 `catch ex: unknown` 与匿名 catch 不进入具体类型分类；
- [ ] 保持 `throw unknownBinding` 为原样重抛；
- [ ] 为每个拒绝类别提供稳定且一致的 Semantic 诊断原因；
- [ ] 验证相同类型不会在 throw 与 catch 两侧得到不同分类结果。

### 8.10 阶段二 Runtime 与 Codegen

- [ ] 删除所有 `feng_scalar_<kind>_exception_descriptor` 声明；
- [ ] 删除所有 `feng_scalar_<kind>_exception_descriptor` 定义；
- [ ] 删除 Codegen 中的 scalar exception descriptor 映射；
- [ ] 确认没有生成 `FengEnumExceptionDesc__...` 或其他异常专用 descriptor；
- [ ] 标量 throw 使用 `ValueBox<T>.header.desc` 对应的箱 descriptor；
- [ ] enum throw 使用 `ValueBox<Enum>.header.desc` 对应的箱 descriptor；
- [ ] tuple/`@value type` throw 使用各自 ValueBox descriptor；
- [ ] 普通实体 type 与 string throw 使用原对象 descriptor；
- [ ] 所有新 throw 路径满足 `value->header.desc == exception->desc`；
- [ ] 具体类型 catch 的 LSDA 项使用与 throw 相同的对象/箱 descriptor；
- [ ] personality 保持单次 descriptor 指针相等比较，不读取 descriptor 或 box 内容；
- [ ] 标量、enum、tuple、`@value type` catch 使用静态 box 类型 `.value` 解箱；
- [ ] 普通实体 type 与 string catch 保持直接对象指针绑定；
- [ ] Codegen 为 Semantic 已拒绝的 array/spec/open generic 保留防御性诊断；
- [ ] 未捕获、正常 catch 完成和重抛路径保持异常载荷恰好释放一次。

### 8.11 阶段二测试与完成门禁

- [ ] Semantic 正向矩阵覆盖全部允许类别的 throw；
- [ ] Semantic 正向矩阵覆盖全部允许类别的具体类型 catch；
- [ ] Semantic 负向矩阵覆盖全部拒绝类别的 throw；
- [ ] Semantic 负向矩阵覆盖全部拒绝类别的具体类型 catch；
- [ ] Codegen 测试证明生成结果不再包含任何异常专用 descriptor；
- [ ] FCTS 覆盖所有内建标量精确匹配和错误标量不匹配；
- [ ] FCTS 覆盖 enum 不匹配底层 `i32`；
- [ ] FCTS 覆盖两个不同具名 enum 不互相匹配；
- [ ] FCTS 覆盖 public enum 跨 package 精确匹配；
- [ ] FCTS 覆盖两个同布局具名 tuple 不互相匹配；
- [ ] FCTS 覆盖 trivial/aggregate `@value type` 精确匹配；
- [ ] FCTS 覆盖闭合泛型普通 type 与 Value 的精确匹配；
- [ ] FCTS 覆盖普通 `@abi type` 与 `@value @abi type` 精确匹配；
- [ ] FCTS 覆盖 `unknown`、匿名 catch、catch 顺序和原样重抛；
- [ ] FCTS/Runtime 覆盖 catch 后字段访问、方法调用、返回、再次抛出和释放；
- [ ] 验证 `examples/hello_world` 的不合法 spec/开放抽象 throw 得到稳定 Semantic 诊断；
- [ ] 运行性能约束，证明 catch 搜索仍只有 descriptor 指针比较；
- [ ] 对照变更前基线，证明阶段二所有受影响路径的运行时开销逐项小于或等于当前开销；
- [ ] 检查生成 C 中 `value->header.desc` 与传给 `feng_throw` 的 descriptor 一致；
- [ ] 确认阶段二没有修改任何未获人工批准的既有 `test/` 用例；
- [ ] `test/` 下全部用例通过；
- [ ] `fcts/` 下全部用例全量通过；
- [ ] `std/std_test/` 下全部用例全量通过；
- [ ] 运行所有阶段二定向测试、FCTS 与 smoke；
- [ ] 在沙箱外运行完整 `make test`；
- [ ] **阶段二完成门禁**：以上阶段二任务全部完成，`test/`、`fcts/`、`std/std_test/` 满足 §3.6 强制
  要求，且无遗留过渡 descriptor。

### 8.12 最终收尾

- [ ] 全仓搜索并确认没有 `FengScalarBox`、`FengBuiltinScalarKind`、共享 scalar-box API；
- [ ] 全仓搜索并确认没有标量或 enum 异常专用 descriptor；
- [ ] 检查所有已交付工程文档，不保留与最终实现冲突的历史现状描述；
- [ ] 检查主规范、手册与工程文档之间没有重复且不一致的规则定义；
- [ ] 检查 public/imported/closed-generic box descriptor 的链接符号和地址唯一性；
- [ ] 检查 `git diff --check`、工作区变更范围和新增测试清单；
- [ ] 对实施期间所有不确定项附上人工决策记录，确认没有未授权的推断或特判；
- [ ] 记录两个阶段最终测试数量、逐项运行时开销对比结果与完整 `make test` 结果；
- [ ] 将本文状态从“待 Review”更新为与实际交付进度一致的状态。

本提案经 Review 前不得开始代码实施。

## 9. Review 决策清单

开始实施前需要逐项确认：

- [ ] 接受删除共享 `FengScalarBox`、`FengBuiltinScalarKind` 与固定 union，统一为静态
  `ValueBox<T>`；
- [ ] 接受每个具体值类型分别拥有原始值描述符与托管箱描述符，且不增加运行时反向链接；
- [ ] 接受不增加 `type_identity`，不修改 `FengManagedHeader` 与三类整体描述符的结构布局；
- [ ] 接受阶段一暂时保留现有异常匹配描述符以隔离行为变更，阶段二必须完整删除；
- [ ] 接受 object-form spec 仅修改 subject 装箱/解箱适配，不修改 fat value 与 witness ABI；
- [ ] 接受阶段二建议的 throw/catch 允许集合与拒绝集合；
- [ ] 接受本阶段拒绝 array 和所有 spec，不增加 catch 搜索时的实例元数据检查；
- [ ] 接受普通 `@abi type` 与 `@value @abi type` 可作为异常载荷，同时保持异常不得跨
  `@abi func` 边界；
- [ ] 接受每个阶段分别执行性能约束与沙箱外完整 `make test`。
