# Feng reified 泛型值局部与泛型 callable ABI 修复方案

> 状态：已完成
>
> 关联主设计：
> [feng-generic-optimize-dev.md](./feng-generic-optimize-dev.md)、
> [feng-generics-delivered.md](./feng-generics-delivered.md)
>
> 关联问题：
> [feng-generic-callable-member-call-bugfix.md](./feng-generic-callable-member-call-bugfix.md)

## 1. 结论

当前暴露的是两个独立问题：

1. 共享泛型体内，布局依赖泛型参数的按值聚合在局部绑定或表达式物化时，
   被错误地重新装入未具化 C 占位结构。
2. 泛型 callable 的 open/closed 实例生成了不同的 C `invoke` 函数签名，
   共享体通过不兼容函数指针调用具体 callback，属于 C 未定义行为。

`Action<KeyEvent<T>>` 同时触发两个问题：`KeyEvent<T>` 是布局依赖 `T` 的
`@value` 聚合，同时又作为泛型 callable 的参数。`Action<MouseEvent<T>>`
只触发第二个问题，因为 `MouseEvent<T>` 是引用类型，值载体始终是对象指针。

两个问题必须分别修复，并使用组合用例共同验收，不能把其中一个当成另一个的
特例或绕过条件。

## 2. 既有规范与本方案边界

### 2.1 reified 值规则已经存在

[feng-generic-optimize-dev.md §2.6](./feng-generic-optimize-dev.md#26-共享体内使用具体化描述符)
已经规定：共享体内布局依赖泛型参数的按值聚合，必须使用具化 descriptor
决定大小、字段偏移和生命周期；局部绑定、临时值与构造结果均不得落入未具化
C struct。

本方案不重新定义 reified 值模型，只修复 codegen 未完整执行既有规则的问题。

### 2.2 泛型共享体 ABI 已采用地址参数与 `_out` 返回

[feng-generics-delivered.md](./feng-generics-delivered.md) 已规定共享泛型主体使用
`const void *` 传递动态布局参数，并使用 `void *_out` 返回动态布局结果。泛型
callable constraint 的 witness `invoke` 也已经使用地址参数和 `_out`。

本方案不增加新的值模型、boxing 或 runtime 反射。泛型 callable 修复需要解决的
是：普通 callable value 的 closure `invoke` 仍按 open/closed C 类型分别生成，
没有与共享泛型 ABI 保持一致。

### 2.3 既有 member-call 分派修复保持独立

[feng-generic-callable-member-call-bugfix.md](./feng-generic-callable-member-call-bugfix.md)
修复的是编译期成员调用分派顺序：泛型 receiver 的 callable 字段应进入普通
callable value 调用路径。该修复本身不处理 callable ABI，也不是本次运行时
未定义行为的根因。

## 3. 问题一：reified-layout 按值聚合局部

### 3.1 触发条件

受影响类型必须同时满足：

1. 类型是按值聚合，例如泛型 `@value type` 或 tuple；
2. 其布局直接或传递依赖共享泛型参数；
3. 共享体将构造结果、返回结果或其他表达式物化为局部绑定。

示例：

```feng
@value
type MyValue<T> {
  let value: T;
}
```

不是所有泛型 `@value type` 都受影响。若字段布局与 `T` 无关，则布局固定：

```feng
@value
type StableValue<T> {
  let value: i64;
}
```

普通引用类型也不属于该问题：

```feng
type MyType<T> {
  let value: T;
}
```

`MyType<T>` 变量本身是固定大小的对象指针；对象按具化后的
`FengTypeDescriptor.size` 分配，较大的值类型 `T` 可以继续内联存储在对象中。

### 3.2 当前错误 lowering

当前构造阶段已经使用正确大小：

```c
const FengAggregateDescriptor *desc = reified_agg_dep;
const size_t size = desc->size;
_Alignas(max_align_t) char temporary[size];
memset(temporary, 0, size);
construct(temporary, desc);
```

但局部绑定或 callable 参数物化随后重新生成未具化占位结构：

```c
struct MyValue__G__T local =
    *(struct MyValue__G__T *)(void *)temporary;
```

若具化后的 `T` 大于 open 形态使用的 `void *` 占位，则会产生：

- 值截断；
- 错误字段偏移；
- cleanup 按具化 descriptor 访问较小局部变量而越界；
- managed slots 泄漏、重复释放或 UAF。

### 3.3 修复规则

共享体内 reified-layout 按值聚合必须始终保持地址存储：

```c
const FengAggregateDescriptor *desc = reified_agg_dep;
const size_t size = desc->size;
_Alignas(max_align_t) char local[size];
memset(local, 0, size);
construct(local, desc);
```

后续操作必须直接使用 `local`：

- 字段地址：`local + desc->reified_field_offsets[i]`；
- 借用复制：`feng_aggregate_assign`；
- owned 转移：`feng_aggregate_take` 或等价的现有转移路径；
- scope cleanup：对同一 `local + desc` 执行 `feng_aggregate_release`；
- 参数转发：传递 `local` 地址；
- 返回：写入调用方提供的 `_out`。

严禁再声明 `struct <open generic>` 局部变量，也不得通过扩大占位结构、固定上限、
boxing 或堆分配规避动态布局。

### 3.4 codegen 收敛点

应由同一项“是否需要 reified layout”的类型事实驱动以下路径：

1. 构造表达式结果；
2. 显式 `let` / `var` 绑定；
3. owned 表达式物化；
4. 参数准备与转发；
5. 返回值接收槽；
6. cleanup 注册与 scope release；
7. 字段访问、赋值和方法 receiver。

内部数据结构必须分别记录两项正交事实：

1. 表达式是否已经表示值的存储地址；
2. 该存储是否是由 aggregate descriptor 决定大小和生命周期的 reified 存储。

共享体的直接泛型参数 `T`、`T[]` 的元素槽和泛型对象中的 `T` 字段属于第一类，
但不属于第二类；布局依赖泛型参数的按值聚合同时属于两类。普通 C 左值两者均不
属于。参数转发等只需要值地址的路径必须读取第一项，aggregate cleanup 等需要
descriptor 权威的路径必须同时读取第二项，不能用“可取地址”或 C 类型名称反推。
这些事实只存在于编译期 codegen 元数据中，不改变 Feng 值布局，也不产生运行时
字段、分支或 descriptor 读取。

布局依赖必须收敛为类型级事实，并由每一层类型负责：

- 类型只分析自己的直接字段；
- 标量、托管引用、数组、callable 和 object-form spec 报告固定布局；
- 泛型参数按值字段报告动态布局；
- 嵌套按值字段只读取其字段类型已经计算的布局事实；
- 每个类型的结果只计算一次并缓存，使用点不得反复展开完整字段树；
- 跨包实例必须复用相同的类型事实，不能根据类型名或使用场景猜测。

因此，包含 `T[]` 字段的泛型按值类型仍是固定物理布局；包含按值 `T` 字段或
传递包含该字段的嵌套按值类型才使用 descriptor-sized 地址存储。该规则是统一的
类型分类，不允许为 `Span<T>`、TUI event 或任何具体标准库类型增加特判。

## 4. 问题二：泛型 callable 的 open/closed C ABI

### 4.1 当前错误

共享体看到的 callable：

```c
void (*invoke)(void *closure, struct Event__G__T value);
```

具化 callback 的实际函数：

```c
void (*invoke)(void *closure, struct Event__G__Widget value);
```

即使两种类型在某次具化中恰好大小相同，函数指针类型仍不兼容。通过强制转换
调用属于 C 未定义行为，UBSan 已在 TUI 的 `KeyEvent<T>` 和 `MouseEvent<T>`
路径中报告该问题。

引用类型也不能仅凭“都是一个指针”忽略此问题：

```c
struct MouseEvent__G__T *
struct MouseEvent__G__Widget *
```

二者物理表示相同，但仍是不同的 C 参数类型。

### 4.2 必须满足的 ABI 不变量

1. 同一 Feng callable 类型的 open/closed 使用必须具有兼容的 C `invoke` ABI；
2. ABI 必须由 callable 类型规则决定，不能由单个调用点临时决定；
3. 不允许通过不兼容函数指针 cast 作为实现；
4. 动态布局参数必须复用共享泛型的地址表示；
5. 动态布局返回值必须复用 `_out`；
6. 具体 callback thunk 负责在已知闭合类型的上下文中读取参数；
7. 不新增 runtime 类型分派、反射或按类型名称查找。

### 4.3 二进制分发下的统一 ABI

泛型共享体在下游具化前已经编译完成。descriptor 可以在运行时提供值的大小、
对齐和生命周期操作，但不能改变已生成 C 函数的参数原型、寄存器分类和返回约定。
固定大小的 C 包装结构若只保存数据指针，本质仍是地址 ABI；若内联数据，其自身的
按值大小仍无法覆盖任意 Feng 类型。

因此，callable ABI 必须根据 callable 的原始声明槽位确定，不能根据替换后的闭合
类型重新分类：

- 与泛型无关的固定类型保持精确 C ABI；
- 原始槽位直接是泛型参数，或其按值 C 类型依赖泛型参数时，参数使用
  `const void *`；后者即使物理布局固定，open/closed 实例的 C 名义类型仍不同，
  也不能生成不同的按值函数原型；
- 原始槽位已知是普通引用类型时，即使该引用类型包含泛型参数，也使用统一对象
  指针表示；
- C 类型依赖泛型参数的按值返回使用隐藏的 `void *_out`；
- 非泛型 callable 完全保持现有直接参数和直接返回 ABI。

例如 `Action<T>` 的所有闭合实例都复用同一个参数 ABI：

```c
void (*invoke)(void *closure, const void *value);
```

`Func<T, R>` 在参数和返回布局均依赖泛型时使用：

```c
void (*invoke)(void *closure, const void *value, void *result_out);
```

具体 callback 的现有 wrapper 负责在已知闭合类型的上下文中读取参数，或者继续把
地址传给共享泛型函数体。参数地址只在同步调用期间有效；任何赋值、字段保存、捕获
或返回仍必须执行 Feng 已有的值复制或移动语义，不能让裸地址逃逸。

callable 泛型声明的 codegen 查找必须继续遵守
[feng-type-arity-overload-dev.md](./feng-type-arity-overload-dev.md) 已定义的
`(name, arity)` 标识规则。`Func<R>`、`Func<T, R>` 等同名不同元数声明必须按使用点
类型实参数量精确解析；本方案不重新定义该规则，只要求 callable ABI 生成不能退化为
仅按名称选中声明。

### 4.4 泛型 callable 默认值

callable 的默认值包含一个空实现；非 `void` callable 的空实现必须返回目标类型的
默认值。open 泛型空实现不能仅凭运行时 descriptor 改变 C 函数原型，也不能在
shared body 中预先知道闭合返回类型对应的默认值工厂。

采用以下通用方案：

1. 每个闭合 callable descriptor 在 codegen 私有的 descriptor 扩展中记录本实例的
   已定型空实现函数；扩展以 `FengTypeDescriptor` 为首成员，不修改 runtime ABI；
2. shared body 创建泛型 callable 默认值时，从既有 `reified_type_deps` 取得闭合
   callable descriptor；
3. 默认值工厂仍只分配原有一个 closure，并把 descriptor 中的已定型空实现写入
   closure 现有的 `invoke` 字段；
4. 实际调用继续直接经过 closure 的单个 `invoke` 指针，不增加分派层；
5. 闭合空实现按闭合返回类型执行现有默认值或 aggregate default-init 规则。

该方案不增加 closure 字段、对象大小、heap allocation、boxing、sidecar、普通调用的
descriptor 读取或间接调用。只在原本无法正确工作的“shared body 创建泛型 callable
默认值”路径中读取一次既有 reified descriptor 和其中的空实现指针。

### 4.5 排除方案与最小开销边界

以下方案不采用：

- closure 增加第二个 erased `invoke` 字段：增加每个 closure 的内存和初始化成本；
- descriptor/sidecar 保存 erased adapter：增加间接读取或调用层；
- shared caller 运行时按 descriptor.kind 分支选择 ABI：不能覆盖任意聚合 C ABI，
  且增加热点分支；
- 普通调用时临时包装或 boxing：增加存储、分配或调用层；
- `libffi`、JIT 或整体单态化：不符合当前实现边界或二进制分发要求；
- 依赖 C 函数指针 cast：仍是未定义行为；
- 对 TUI 的 `KeyEvent` / `MouseEvent` 或其他具体类型增加特判：不通用。

闭合 `Action<i32>` 也必须遵守其原始泛型槽位的地址 ABI，因此相对理想的专用
`i32` C 原型会多出必要的传址和读取。这不是在正确旧路径上增加的可选开销，而是
单一共享函数体和任意 `T` 共同要求的统一调用约定成本。实现不得再增加 descriptor
读取、运行时分支、boxing、额外 thunk 层、额外间接调用或 per-closure 存储。

两个不同 callable spec 之间的显式转换需按生成后的 ABI 处理：参数及返回 ABI
完全相同时，只改变静态 closure 视角，不分配对象、不增加调用层；ABI 不同时，
转换点必须创建既有的 rewrap closure，由该对象持有源 callable 并适配一次调用。
该适配不能靠 C 函数指针 cast 实现，否则仍属于未定义行为。分配和额外调用层仅由
用户显式请求的 ABI 变换承担，不进入普通 callable 创建、传递或调用路径。

## 5. 两个问题的交互

`Action<KeyEvent<T>>` 的错误顺序是：

```text
descriptor-sized 存储中正确构造 KeyEvent<T>
    -> 错误复制到 KeyEvent<T-placeholder> 局部值      （问题一）
    -> 通过不兼容 open invoke 调用 closed callback    （问题二）
```

`Action<MouseEvent<T>>` 的顺序是：

```text
构造固定大小的 MouseEvent<T> 对象指针
    -> 无 reified 局部尺寸问题
    -> 通过不兼容 open invoke 调用 closed callback    （仅问题二）
```

修复问题一不能消除问题二；修复问题二也不能修复已经被占位局部截断的值。

## 6. 运行时性能硬约束

“无增量运行时开销”按以下口径验收：

1. 非泛型 callable 等当前语义和 ABI 正确的路径，运行时操作数量只能不变或减少；
2. 当前未定义行为路径不作为有效性能基线，但修复后不得增加非必要操作；
3. reified 局部修复应删除占位结构复制，不增加新的复制；
4. 每个动态局部值的 descriptor 与 `size` 只读取一次并复用；
5. 普通创建、传递和调用路径不新增 heap allocation；仅 ABI 不同的显式 callable
   转换使用既有 rewrap closure，ABI 相同的转换不得分配；
6. 不新增 boxing、sidecar、runtime helper、运行时分支；
7. 不新增 retain/release/assign/take；
8. 除泛型槽位统一地址 ABI 必需的传址和读取，以及 ABI 不同的显式 callable 转换外，
   不增加 callable 间接调用和
   thunk 层级；
9. 不增大 Feng 值、对象或 closure 的实例布局；
10. runtime 私有 ABI 和 runtime 源文件保持不变。

泛型槽位统一地址 ABI 的必要成本已经完成人工决策；其他运行时成本若无法满足上述
约束，仍必须返回设计阶段，不能在实现中静默引入。

## 7. 测试方案

### 7.1 reified-layout 局部专项

新增 fcts 行为用例，且不依赖 callable：

- `T = i64`；
- `T = string`；
- `T = object-form spec`，保证值至少包含 `subject + witness`；
- `T =` 含 managed slots 且大于一个机器字的 `@value` 类型；
- 嵌套 `Outer<Inner<T>>`；
- 默认初始化、构造表达式、显式绑定、复制、返回和 scope cleanup；
- 同包与跨包 FT。

### 7.2 callable ABI 专项

在 callable ABI 方案获批后新增：

- 普通引用参数；
- 泛型引用参数；
- 固定布局按值参数；
- reified-layout 按值参数；
- void 与非 void 返回；
- lambda、顶层函数和当前已支持的实例方法绑定；
- callable 字段读取后调用；
- 同包和跨包 FT；
- 闭合快速路径生成代码约束测试。

### 7.3 组合验收

- `Action<MyValue<ObjectSpec>>`；
- `Action<KeyEvent<Widget>>`；
- `Action<MouseEvent<Widget>>`；
- callback 内验证完整字段、target/witness 和 managed 生命周期；
- TUI demo 的输入、锁定、拖动路径。

### 7.4 回归要求

- fcts 用例必须注册到 `fcts_bin` 并使用 `std.test`；
- 生成程序的 UBSan 必须设置 `halt_on_error=1`；
- Linux CI 使用 ASan + UBSan 覆盖动态局部越界；
- 构建 `std/std` 与 `examples/tui_demo`；
- 最后执行 `make test`；
- `make test` 不能只依据退出码，必须确认无 sanitizer 报告。

## 8. 实施顺序

1. 修复 reified-layout 局部绑定和表达式物化；
2. 增加不依赖 callable 的完整 fcts 用例并执行专项回归；
3. 按原始声明槽位确定 callable ABI，闭合实例不得重新分类；
4. 实施 callable ABI 修复；
5. 增加 callable 专项及组合用例；
6. 构建 std、TUI demo，并执行完整 sanitizer 与 `make test` 回归；
7. 回归全部通过后，继续 TUI event target/lock 的验收。

## 9. 实施清单

- [x] 修复 reified-layout 按值聚合的局部绑定
- [x] 修复 reified-layout 按值聚合的表达式物化
- [x] 验证参数、返回、字段和 cleanup 统一复用 descriptor-sized 存储
- [x] 增加 reified-layout 专项 fcts
- [x] 完成二进制分发下的 callable ABI 设计并 Review
- [x] 实施 callable open/closed ABI 修复
- [x] 增加 callable ABI 专项与组合 fcts
- [x] 构建 `std/std`
- [x] 构建并运行 `examples/tui_demo`
- [x] 执行无 sanitizer 报告的完整 `make test`
