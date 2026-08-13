# Feng 泛型共享体 Callable Value 具化重构开发文档

> 状态：待 Review，尚未开始实施
>
> 本文只定义泛型共享体形成 callable value 的实现重构方案，不新增或修改 Feng
> 语言语义。方法值与 `self` 的语义仍以
> [Feng 函数规范](../specifications/feng-function.md) 和
> [Feng 类型规范](../specifications/feng-type.md) 为准；泛型二进制分发规则仍以
> [Feng 泛型规范](../specifications/feng-generics-draft.md) 为准。

## 1. 背景

当前泛型共享体使用 `reified_callable_deps` 保存直接泛型 callable 依赖。数组的每个
slot 按编译期确定的固定索引读取，元素类型为：

```c
const FengFunctionDescriptor *const *reified_callable_deps;
```

值类型方法值的共享体支持在此基础上引入了 codegen-private 的
`FengMethodValueDescriptor`：

```c
typedef struct FengMethodValueDescriptor {
    FengFunctionDescriptor base;
    const FengTypeDescriptor *closure_desc;
    const FengAggregateDescriptor *receiver_desc;
    size_t receiver_offset;
    void (*invoke)(void);
} FengMethodValueDescriptor;
```

consumer 实际生成 `FengMethodValueDescriptor`，但只把 `&descriptor.base` 放入
`reified_callable_deps`。provider 共享体读取 `FengFunctionDescriptor *` slot 后，
再向下强制转换回 `FengMethodValueDescriptor *`。

该表示依赖 C 的首成员地址规则，当前生成代码可以工作，但存在以下结构性问题：

1. `reified_callable_deps` 的声明类型与 slot 中实际对象的完整类型不一致；
2. `base` 当前主要充当兼容前缀，没有完整表达被绑定方法的真实具化依赖；
3. `FengMethodValueDescriptor` 已经构成生成代码与 runtime 之间的 ABI，但定义仍位于
   生成 C 的 codegen-private 区域；
4. 当前结构只描述值 receiver，不能统一描述引用 receiver、顶层函数值和 lambda；
5. 若继续为每种 callable 来源增加专用描述符，将形成不可扩展的特判集合。

本专项废弃 `FengMethodValueDescriptor`，扩展真实的 `FengFunctionDescriptor`，使同一个
闭合函数描述符同时承载共享体执行所需的具化依赖，以及该函数形成 callable value
时所需的闭合表示信息。

## 2. 目标

本次重构必须同时满足以下目标：

1. `reified_callable_deps` 的每个 slot 始终指向真实、完整、同类型的
   `FengFunctionDescriptor`；
2. `FengFunctionDescriptor` 中原有依赖字段继续描述目标 callable 的真实共享体依赖，
   不再只是其他结构的兼容前缀；
3. `FengCallableValueDescriptor` 作为正式 runtime 私有 ABI，并以内嵌字段形式进入
   `FengFunctionDescriptor`；
4. 顶层函数共享体和类型成员共享体均可形成并返回顶层函数引用、绑定成员方法或
   lambda；
5. 引用 receiver 与值 receiver 复用相同的描述符读取、closure 分配和 invoke 初始化
   流程，只在捕获 receiver 的值表示操作上不同；
6. provider 编译时允许相关类型仍然开放，consumer 在最终具化点静态生成闭合函数
   描述符、closure 描述符和 typed invoke adapter；
7. 共享体只按固定 slot 读取静态描述符，不执行运行时搜索、动态具化、tag 分派或
   描述符工厂间接调用；
8. 不改变 callable value 的现有值表示、调用 ABI、生命周期和装箱规则；
9. 不给现有非泛型路径、已闭合路径或直接泛型调用路径增加执行时开销。

## 3. 范围与边界

### 3.1 纳入范围

- 顶层泛型函数共享体形成并返回 callable value；
- 泛型 owner 的实例方法、静态方法以及带方法级泛参的方法共享体形成并返回
  callable value；
- 顶层函数引用，包括闭合普通函数和需要 consumer 闭合的泛型顶层函数；
- 引用类型 receiver 的方法值；
- `@value type` 与 tuple 等值语义 receiver 的方法值；
- 自身方法和 `fit` 方法形成的方法值；
- lambda 及其捕获环境；
- 类型级泛参、方法级泛参以及两者共同参与 callable 签名或捕获布局；
- 本包和跨包 `.ft` 二进制分发；
- callable value 的形成、返回、保存、调用和生命周期验证。

### 3.2 不纳入范围

- callable-form spec 的新结构匹配、转换或重载规则；
- 泛型重载优先级、variance 或新的类型推导规则；
- object-form spec 的新方法值分派能力；
- 修改方法值 receiver 复制语义或 lambda 捕获语义；
- 以单态化泛型共享体替代现有二进制分发方案；
- 与本缺口无关的 callable ABI 重设计。

本专项只补齐已经通过语义检查的 callable value 表达式在共享泛型体中的通用具化和
发码能力。

## 4. 扩展后的描述符结构

### 4.1 `FengCallableValueDescriptor`

`FengCallableValueDescriptor` 描述“一个已经闭合的 callable 实现如何形成 callable
value”。它是生成代码直接读取的 runtime 私有 ABI，必须定义在 runtime 头文件中，
不能继续作为 codegen-private C 结构生成。

```c
typedef struct FengCallableValueDescriptor {
    /* 无动态捕获时已经形成的静态 callable value；否则为 NULL。 */
    const void *static_value;

    /* 需要动态形成 closure 时使用的闭合 closure 类型描述符。 */
    const FengTypeDescriptor *closure_desc;

    /* 与目标 callable-form spec ABI 完全一致的 typed invoke adapter。 */
    void (*invoke)(void);

    /* 需要按值复制的 descriptor-sized aggregate 捕获；不适用时为 NULL。 */
    const FengAggregateDescriptor *aggregate_capture_desc;

    /* 上述 aggregate 捕获在 closure 对象中的字节偏移；不适用时为 0。 */
    size_t aggregate_capture_offset;
} FengCallableValueDescriptor;
```

字段用途如下：

| 字段 | 用途 |
| --- | --- |
| `static_value` | 保存无动态捕获的静态 callable value，例如 consumer 为闭合顶层泛型函数生成的 immortal closure；共享体直接返回该值，不分配对象 |
| `closure_desc` | 描述动态 closure 的闭合大小、托管字段和释放规则，供 `feng_object_new` 分配正确的 closure |
| `invoke` | 指向 consumer 已闭合的 typed adapter；adapter 负责从 closure 取出 receiver/捕获，并调用目标函数或共享方法体 |
| `aggregate_capture_desc` | 描述需要按值复制到 closure 的 aggregate 捕获，包括大小和托管 slot；当前主要用于值 receiver，也可复用到具有同种表示需求的 aggregate 捕获 |
| `aggregate_capture_offset` | 指明该 aggregate 捕获在 closure 中的实际位置，避免 provider 依赖 consumer 才能确定的闭合 C 布局 |

该结构不保存实际 receiver 或 lambda 捕获值。实际运行时值仍来自共享体当前执行上下文。
该结构也不增加 `kind` 字段：callable 来源和捕获表示已经由语义分析与 codegen 在编译期
确定，生成代码直接使用对应字段，不生成运行时 `switch`。

### 4.2 `FengFunctionDescriptor`

`FengFunctionDescriptor` 从“一个闭合共享函数调用的依赖描述”扩展为“一个闭合 callable
实例的完整静态描述”。它仍然描述真实函数或编译器生成的 callable helper，并新增一个
内嵌的 callable-value 形成描述：

```c
typedef struct FengFunctionDescriptor {
    /* 闭合 callable 的调试名称。 */
    const char *name;

    /* 当前 callable 共享体直接使用的闭合 aggregate 依赖。 */
    size_t reified_agg_deps_count;
    const FengAggregateDescriptor *const *reified_agg_deps;

    /* 当前 callable 共享体直接使用的闭合 managed type 依赖。 */
    size_t reified_type_deps_count;
    const FengTypeDescriptor *const *reified_type_deps;

    /* 当前 callable 共享体直接调用或形成值的其他闭合 callable 描述符。 */
    size_t reified_callable_deps_count;
    const struct FengFunctionDescriptor *const *reified_callable_deps;

    /* 当前闭合 callable 被当作值使用时的静态形成信息。 */
    FengCallableValueDescriptor callable_value;
} FengFunctionDescriptor;
```

各字段的职责必须严格区分：

- `reified_agg_deps`、`reified_type_deps` 和 `reified_callable_deps` 描述目标
  callable 共享体执行时直接使用的真实闭合依赖；
- `callable_value` 只描述同一个闭合 callable 在进入目标 callable-form spec 视角时
  如何形成 callable value；
- `callable_value` 不复制前述依赖集合，也不保存另一个函数描述符指针；
- typed adapter 调用目标泛型共享体时，直接传入其所属的同一个
  `FengFunctionDescriptor`；
- 只用于直接调用、从未作为值形成的描述符，其 `callable_value` 全部字段为 `NULL/0`，
  共享体直接调用路径不会读取该字段。

因此，`reified_callable_deps` 中不再混装不同结构：

```c
const FengFunctionDescriptor *dependency =
    _func_desc->reified_callable_deps[SLOT];
```

直接调用使用 `dependency` 本身；形成 callable value 使用：

```c
const FengCallableValueDescriptor *value_desc =
    &dependency->callable_value;
```

这里没有向下强制转换，也没有第二个 slot 数组或额外的描述符指针间接读取。

### 4.3 同一声明的多个 callable value surface

同一个函数或方法可以进入不同的 callable-form spec 视角。不同目标 spec 可能具有不同
closure C 类型或 invoke ABI，因此“来源 callable + 闭合泛参”不足以唯一确定
`callable_value`。

当目标 callable surface 不同时，consumer 可以为同一个底层声明生成不同的闭合
`FengFunctionDescriptor` 实例：

- 这些实例可以复用相同的 `reified_agg_deps`、`reified_type_deps` 和下级 callable
  dependency 数组；
- 每个实例拥有与其目标 callable surface 对应的 `callable_value`；
- slot key 必须包含目标 callable-form spec 的完整闭合身份；
- typed adapter 始终与该 descriptor 的目标 surface 一致。

这不是函数体单态化。函数或方法主体仍然只有 provider 编译的共享体，consumer 只生成
静态描述符和必要的小型 typed adapter/wrapper。

## 5. 通用共享体模型

### 5.1 编译期语义计划

每个未绑定 callable 源进入 callable-form spec 的位置，语义分析必须确定：

- callable 来源：顶层函数、绑定方法或 lambda；
- 已解析的函数、成员、`fit` 或 lambda helper 身份；
- 目标 callable-form spec 的完整实例类型；
- 方法值的 receiver 表达式及完整 receiver 实例类型；
- receiver 是托管引用表示还是值语义 aggregate 表示；
- 来源和目标签名已经结构匹配后的确定结果。

codegen 不重新选择重载、猜测 callable 来源或重新判断签名。是否需要 reified callable
dependency 由上述已解析结果是否仍引用当前活动泛参决定，而不是由包名、类型名、函数名
或“是否为值 receiver”决定。

### 5.2 依赖收集与 slot

共享体需要 consumer 补充闭合 callable 信息时，收集一条通用 callable dependency。
依赖记录至少包含：

- 使用方式：直接调用、形成 callable value，或者两者都需要；
- 来源类别；
- 顶层函数、类型成员、`fit` 或 lambda helper 的稳定符号身份；
- owner 实例类型；
- 来源 callable 的显式或推断泛型实参；
- 形成值时的目标 callable-form spec 实例类型；
- 依赖归属的 owner/function domain。

`METHOD_VALUE` 不再是特殊描述符用途。方法值、顶层函数值和 lambda 值都属于
“形成 callable value”，最终 slot 元素统一为 `FengFunctionDescriptor *`。

slot 使用规范化 key 排序和去重。用于形成 callable value 时，key 至少区分：

- 来源 callable 的稳定声明身份；
- owner 实例类型；
- callable 泛型实参；
- `fit` 实现身份；
- 目标 callable-form spec 实例类型；
- lambda helper 的稳定生成身份。

provider 写入 `.ft` 的开放 key 与 consumer 替换泛参后的闭合 key 必须遵守同一规则。
不得使用 AST 地址、遍历偶然顺序或生成 C 符号文本作为跨包身份。

### 5.3 Consumer 闭合

consumer 在具体 wrapper 或闭合 owner descriptor 生成阶段：

1. 替换 owner 与方法活动泛参；
2. 注册目标 callable-form spec、receiver、捕获环境和共享体依赖需要的闭合类型；
3. 递归生成目标 callable 的真实 `reified_agg_deps`、`reified_type_deps` 和
   `reified_callable_deps`；
4. 生成或复用与目标 callable surface 匹配的 typed invoke adapter；
5. 生成或复用闭合 closure descriptor；
6. 对无动态捕获的顶层函数生成或复用静态 callable value；
7. 填充内嵌的 `FengCallableValueDescriptor`；
8. 构造完整的 `static const FengFunctionDescriptor`；
9. 把该函数描述符地址写入当前 owner/function descriptor 的固定 callable slot。

consumer 只生成闭合 wrapper、adapter、closure 类型和静态描述符；函数或方法主体继续
使用 provider 已编译的共享体，不把正确性建立在共享体单态化之上。

### 5.4 Provider 共享体统一发码

provider 共享体形成 callable value 时，首先执行完全相同的描述符读取：

```c
const FengFunctionDescriptor *callable_desc =
    _func_desc->reified_callable_deps[SLOT];
const FengCallableValueDescriptor *value_desc =
    &callable_desc->callable_value;
```

若依赖属于字段初始化器、构造函数或析构函数，则第一行按照现有 owner-domain 规则从
`_type_desc` 读取；slot 元素类型和后续逻辑完全相同。

随后按编译期已经确定的 callable 表示发码。

#### 顶层函数值

无动态捕获的顶层函数直接取得静态 callable value：

```c
result = value_desc->static_value;
```

静态 value 的 typed adapter 已静态关联 `callable_desc`，调用泛型共享函数时传入同一个
真实函数描述符。

#### 引用 receiver 的方法值

```c
closure = feng_object_new(value_desc->closure_desc);
closure->_self = NULL;
closure->invoke = value_desc->invoke;
feng_assign(&closure->_self, receiver);
```

receiver 本身是固定大小的托管指针。闭合 owner 类型信息由 typed adapter 静态关联；
形成方法值时不复制对象内容，也不需要 aggregate descriptor。

#### 值 receiver 的方法值

```c
closure = feng_object_new(value_desc->closure_desc);
closure->_self = NULL;
closure->invoke = value_desc->invoke;
feng_aggregate_assign(
    (char *)closure + value_desc->aggregate_capture_offset,
    receiver,
    value_desc->aggregate_capture_desc
);
```

该路径与引用 receiver 共用 slot 读取、closure 分配和 invoke 初始化。唯一不同的是语言
语义要求值 receiver 在方法值形成时复制完整值，因此使用 aggregate descriptor 将值
复制到 closure 的内联存储，不能保存源值地址或额外装箱。

typed adapter 从 closure 的内联捕获区取得 `self`，再把同一个 `callable_desc` 传给目标
共享方法体。方法值后续调用不再次复制 receiver。

#### Lambda

lambda 使用编译器生成的 invoke helper 作为 callable 实现。其函数描述符同样可携带
真实共享体依赖和 `callable_value`：

```c
closure = feng_object_new(value_desc->closure_desc);
closure->invoke = value_desc->invoke;
/* 按现有 lambda 捕获计划写入普通捕获、owner/function descriptor 和泛参描述符。 */
```

lambda 的多个捕获字段继续由现有捕获分析逐项处理；不能把
`aggregate_capture_desc/offset` 特判为“描述全部 lambda 捕获”。只有 provider 无法独立
确定的闭合 closure 级事实从 `callable_value` 读取。

### 5.5 Typed adapter 的统一职责

无论 callable value 来源是顶层函数、引用方法、值方法还是 lambda，typed adapter 均
负责：

1. 按目标 callable-form spec ABI 接收参数和返回值；
2. 在需要时从 closure 取得 receiver 或捕获环境；
3. 调用对应的普通闭合入口或 provider 共享体；
4. 调用共享体时传入该 callable value 所属的同一个
   `FengFunctionDescriptor`；
5. 转发 owner descriptor、方法级泛参描述符和参数/返回值，不执行运行时具化。

因此，形成 callable value 和调用该 value 使用同一份闭合函数事实，不会出现
`FengMethodValueDescriptor.base` 与真实函数描述符内容不一致的问题。

## 6. 必须支持的组合

以下方向必须统一支持，不能只支持其中一个方向：

### 6.1 顶层共享体返回成员方法

```feng
open func bindReader<T>(value: Reader<T>): Producer<T> {
  return value.read;
}
```

receiver 可以是引用类型、`@value type` 或 tuple 的 `fit` 视角，分别遵守既有引用捕获
或值复制语义。

### 6.2 成员共享体返回成员方法

```feng
open type Reader<T> {
  open func reader(): Producer<T> {
    return self.read;
  }
}
```

### 6.3 成员共享体返回顶层函数

```feng
open func topRead<T>(): T {
  // 示例主体省略
}

open type Reader<T> {
  open func reader(): Producer<T> {
    return topRead<T>;
  }
}
```

### 6.4 顶层共享体返回顶层函数

```feng
open func topReader<T>(): Producer<T> {
  return topRead<T>;
}
```

### 6.5 顶层或成员共享体返回 lambda

```feng
open func makeReader<T>(value: T): Producer<T> {
  return () => value;
}
```

成员方法中的等价 lambda 也必须工作。lambda 继续遵守既有捕获语义；本专项只保证其
在共享体和跨包闭合下取得完整 callable ABI 与具化信息。

### 6.6 转发已经形成的 callable value

```feng
open func forward<T>(reader: Producer<T>): Producer<T> {
  return reader;
}
```

该路径只转发既有托管 callable value，不应生成新的函数描述符、closure 或 adapter。

## 7. 描述符归属与 `.ft`

### 7.1 描述符归属

callable dependency 放入实际执行共享体所读取的 descriptor domain：

- 顶层泛型函数、普通成员方法和静态方法：放入该 callable 的
  `FengFunctionDescriptor.reified_callable_deps`；
- 字段初始化器、构造函数和析构函数：继续遵守现有 owner 归属规则，放入
  `FengTypeDescriptor` 或 `FengAggregateDescriptor` 的
  `reified_callable_deps`；
- 类型级 `T` 与方法级 `U` 仍分别从 owner descriptor 与方法泛参隐藏参数取得，本专项
  不改变泛参描述符传递协议。

所有 domain 的 `reified_callable_deps` 元素类型均保持为准确的
`FengFunctionDescriptor *`。

### 7.2 `.ft` 规则

`.ft` 序列化 callable dependency 的编译期身份、使用方式和开放类型信息，不序列化
runtime 地址或 consumer 生成的 C 符号。

导出和恢复必须保证：

1. 顶层函数、成员、`fit` 和 lambda helper 的身份稳定；
2. owner 实例、目标 callable spec 和 callable 泛参保留完整开放类型表达式；
3. provider 与 consumer 使用相同的 slot key、排序和去重规则；
4. consumer 可使用 provider 从未预先出现的具体类型实参完成闭合；
5. imported 与本地声明进入同一 semantic、reification 和 codegen 管线，不建立 imported
   专用分支；
6. FT 版本或记录结构变化必须显式升级，不能让旧 reader 静默误读新记录。

## 8. ABI 与性能约束

### 8.1 Runtime ABI

- `FengCallableValueDescriptor` 进入 runtime 相关头文件；
- `FengFunctionDescriptor` 内嵌一个 `FengCallableValueDescriptor` 字段；
- `FengTypeDescriptor` 和 `FengAggregateDescriptor` 不增加第二套 callable-value
  dependency 数组，继续指向扩展后的 `FengFunctionDescriptor`；
- 删除生成 C 中的 `FengMethodValueDescriptor`；
- 删除 `&descriptor.base`、向下强制转换和首成员兼容协议；
- 不新增 runtime 查找表、全局注册、延迟具化或 descriptor factory API。

内嵌字段会增加每个 `FengFunctionDescriptor` 的固定静态大小，即使该函数从未作为值
形成，其 `callable_value` 也占用静态只读数据。这是本方案明确接受的静态数据成本，
换取以下性质：

- slot 始终具有单一、准确的类型；
- callable value 形成不增加第二次描述符指针读取；
- 直接调用和作为值使用共享同一份真实函数依赖；
- 不需要第二套 slot 数组及其 count/pointer。

若实现阶段需要改成 sidecar、可选指针、tagged union 或其他表示，必须暂停并重新提交
人工 Review。

### 8.2 执行时性能

必须满足：

- 只用于直接调用的共享体不读取 `callable_value`，生成的调用路径保持不变；
- 直接泛型 callable 调用继续原样读取 `reified_callable_deps`；
- 当前共享值 receiver 方法值路径原有的一次 slot 读取保持一次，不增加描述符间接层；
- 无捕获顶层函数值使用静态 callable value，不进行堆分配；
- 引用或值 receiver 方法值只保留 callable closure 既定的一次分配，不增加 receiver box；
- 值 receiver 只执行语义要求的一次 aggregate 复制；
- lambda 保持现有闭包分配和捕获次数；
- callable 调用继续直接进入 typed invoke adapter，不新增 wrapper 层或运行时间接工厂；
- 不装箱泛型值。值只在进入 object-form spec 视角时按既有规则装箱。

如果实现需要新增一次运行时间接调用、动态 tag 判断、描述符遍历、哈希查找、额外堆
分配或既有路径的额外 retain/release，必须暂停并提交人工 Review。

## 9. 测试要求

### 9.1 Compiler tests

必须验证：

- callable 来源、使用方式与目标 callable spec 的语义结果稳定；
- `reified_callable_deps` 的所有元素均为完整 `FengFunctionDescriptor *`；
- 本地与 imported FT round-trip 后的依赖身份、开放类型和 slot 顺序一致；
- 生成的函数描述符包含目标共享体的真实递归依赖；
- callable value 的 typed adapter 把同一函数描述符传给目标共享体；
- 不再生成 `FengMethodValueDescriptor`、首成员地址或描述符向下强制转换；
- 共享体只使用固定索引读取，不生成运行时搜索、tag switch 或 factory 间接调用；
- callable 签名不匹配仍在语义阶段使用既有诊断拒绝。

未经人工批准，不修改已有测试的语义；优先新增独立测试。

### 9.2 FCTS

可表达的语言行为优先放入 FCTS。跨包 provider 写入 `fcts_lib`，consumer、调用和断言
写入 `fcts_bin`，统一使用 `std.test`，库中不输出额外文本。

至少覆盖以下矩阵：

| 共享体归属 | 返回来源 | 必须验证 |
| --- | --- | --- |
| 顶层泛型函数 | 顶层泛型函数 | consumer 闭合、静态 callable、调用结果 |
| 顶层泛型函数 | 引用 receiver 方法 | receiver 生命周期与调用结果 |
| 顶层泛型函数 | 值 receiver 方法 | 按值捕获、源值独立、调用结果 |
| 顶层泛型函数 | lambda | 泛型捕获逃逸和调用结果 |
| 泛型 owner 成员方法 | 顶层泛型函数 | owner `T` 的闭合与静态 callable |
| 泛型 owner 成员方法 | 自身或其他对象的方法 | receiver 捕获和调用结果 |
| 泛型 owner 成员方法 | lambda | owner/function descriptor 捕获正确 |
| owner `T` + 方法 `U` | 上述代表来源 | 两级泛参 domain 和 slot 不混用 |

代表实参至少包含标量、普通引用 type、descriptor-sized `@value type`、tuple、
object-form spec 和 callable-form spec。测试不做无意义的全笛卡尔积，但每种不同运行时
表示至少有一个可观察断言。

### 9.3 性能结构断言

codegen tests 还必须锁定：

- 静态顶层函数值没有 `feng_object_new`；
- 方法值只有一次 closure 分配；
- 值 receiver 没有 spec box 或第二次 receiver 分配；
- 直接调用路径不读取 `callable_value`；
- 新路径没有 descriptor factory 调用；
- 既有非泛型、闭合和 direct-call 代表用例的关键生成结构不变。

## 10. 验收标准

只有同时满足以下条件，本专项才能标记完成：

1. runtime 中存在正式的 `FengCallableValueDescriptor`；
2. `FengFunctionDescriptor` 包含文档定义的完整真实依赖和内嵌 callable-value 信息；
3. `reified_callable_deps` 的所有 slot 均指向准确、完整的
   `FengFunctionDescriptor`；
4. codegen-private `FengMethodValueDescriptor` 及首成员兼容协议已删除；
5. 顶层函数共享体与成员共享体均支持返回顶层函数、成员方法和 lambda；
6. 引用 receiver 与值 receiver 共用统一形成管线，只保留语言值表示要求的捕获差异；
7. 本地与跨包、类型级与方法级泛参、引用与值 receiver 均有有效行为证据；
8. callable value 的复制、捕获、调用和释放符合既有语言语义；
9. 没有类型名、包名、声明方向或测试模型特判；
10. 没有新增运行时搜索、tag 分派、工厂间接调用、额外装箱或额外分配；
11. 所有专项测试及完整 `make test` 通过；
12. 相关既有开发文档的状态和实现描述在实施完成后另行同步。

## 11. 实施步骤

### 11.1 Runtime 描述符与编译期数据模型

- [ ] 在 runtime 中定义 `FengCallableValueDescriptor`。
- [ ] 为 `FengCallableValueDescriptor` 及其所有字段补充准确的 runtime ABI 注释。
- [ ] 在 `FengFunctionDescriptor` 中内嵌 `FengCallableValueDescriptor callable_value`。
- [ ] 更新 `FengFunctionDescriptor` 的职责注释，明确其描述一个闭合 callable 实例。
- [ ] 保持 `reified_callable_deps` 的数组元素类型为
  `FengFunctionDescriptor *`，不增加第二套 callable-value slot 域。
- [ ] 将 method-value 专用依赖抽象为可表达直接调用、形成 callable value 或二者兼有的
  通用 callable dependency。
- [ ] 确认没有引入类型名、包名、声明方向或测试模型特判。

### 11.2 `.ft` 闭合协议

- [ ] 扩展 callable dependency 的 FT 记录，使其保存使用方式、来源身份、owner 实例、
  callable 泛参和目标 callable-form spec。
- [ ] 覆盖顶层函数、类型成员、`fit` 和需要稳定身份的 lambda helper。
- [ ] 实现本地、导出和导入一致的规范化 key、排序、去重及类型替换。
- [ ] 显式升级受影响的 FT 版本或记录结构，禁止旧 reader 静默误读。
- [ ] 增加 FT round-trip 和 consumer-only 闭合实例的 compiler tests。
- [ ] 完成本阶段专项测试并执行完整 `make test`；若发现问题，先修复通用根因再进入下一阶段。

### 11.3 Consumer 描述符生成

- [ ] 递归生成每个依赖 callable 的真实 `reified_agg_deps`、
  `reified_type_deps` 和 `reified_callable_deps`。
- [ ] 为顶层函数、引用 receiver 方法、值 receiver 方法和 lambda 生成或复用闭合
  closure descriptor 与 typed invoke adapter。
- [ ] 为无动态捕获的顶层函数生成或复用静态 callable value。
- [ ] 为引用 receiver 填写 `closure_desc` 和 `invoke`。
- [ ] 为值 receiver 填写 `closure_desc`、`invoke`、
  `aggregate_capture_desc` 和 `aggregate_capture_offset`。
- [ ] 为同一声明的不同 callable surface 生成正确且可复用的闭合函数描述符实例。
- [ ] 输出元素类型严格一致的 `FengFunctionDescriptor *` 依赖数组。
- [ ] 删除 codegen-private `FengMethodValueDescriptor`、`&descriptor.base` 和向下强制转换。
- [ ] 完成本阶段专项测试并执行完整 `make test`；若发现问题，先修复通用根因再进入下一阶段。

### 11.4 通用共享体形成发码

- [ ] 让顶层函数共享体和成员共享体复用统一的 callable descriptor slot 查找入口。
- [ ] 实现无动态捕获顶层函数的静态 callable value 形成路径。
- [ ] 实现引用 receiver 方法值的一次 closure 分配与 `feng_assign` 捕获路径。
- [ ] 实现值 receiver 方法值的一次 closure 分配与 aggregate 内联复制路径。
- [ ] 让 lambda 复用统一的闭合 callable 描述信息，同时保持现有逐项捕获语义。
- [ ] 让 typed adapter 使用所属的同一个 `FengFunctionDescriptor` 调用目标共享体。
- [ ] 支持 §6 中顶层函数共享体与成员共享体返回顶层函数、成员方法、lambda，以及转发
  已形成 callable value 的全部组合。
- [ ] 保持既有非泛型、已闭合和直接调用路径的发码及执行时开销不变。
- [ ] 完成本阶段专项测试并执行完整 `make test`；若发现问题，先修复通用根因再进入下一阶段。

### 11.5 完整用例与最终验收

- [ ] 补齐 §9.1 的 semantic、FT 和 codegen compiler tests。
- [ ] 在 FCTS 中补齐 §9.2 的本地及跨包行为矩阵，并使用 `std.test` 完成可观察断言。
- [ ] 补齐 §9.3 的分配次数、装箱、slot 读取和直接调用路径性能结构断言。
- [ ] 验证引用和值 receiver 的捕获、调用、复制、释放及异常展开符合既有语义。
- [ ] 验证类型级泛参、方法级泛参以及二者组合的 descriptor domain 和 slot 均正确。
- [ ] 验证 consumer 使用 provider 未预先出现的闭合类型实参时仍可正确二进制分发。
- [ ] 执行完整 `make test` 并确认全部通过。
- [ ] 按实际实现更新本文状态和 TODO。
- [ ] 在实施完成后另行同步相关既有开发文档的状态与实现描述。
