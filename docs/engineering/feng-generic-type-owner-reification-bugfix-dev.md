# Feng 泛型类型 owner 依赖具化修复开发文档

> 状态：已完成（2026-08-12，全量回归通过）
>
> 本专项采用“方案一”：泛型类型的实例/静态字段初始化、全部构造函数及终结器统一使用类型描述符中的
> owner dependency slots；普通实例方法与静态方法继续使用各自的函数描述符。
>
> [feng-generic-cross-feature-fcts-hardening-dev.md](./feng-generic-cross-feature-fcts-hardening-dev.md)
> 已恢复；其中第三组已按本专项结果完成，后续从第四组继续。

## 1. 依据与目标

语言语义以
[Feng 泛型规范草案](../specifications/feng-generics-draft.md)、
[Feng 语言类型规范](../specifications/feng-type.md) 和
[Feng 语言对象生命周期规范](../specifications/feng-lifetime.md) 为准；函数级依赖图沿用
[Feng 泛型共享体具化修复开发文档](./feng-generic-shared-body-reification-bugfix-dev.md)
已经落地的 dependency key、固定 slot 和闭合描述符图规则。

本专项修复泛型类型 owner 代码无法完整取得闭合 aggregate、managed type 及 generic callable
描述符的问题，使下面的普通引用类型代码能够正确编译、跨包二进制分发并运行：

```feng
type UserType<T> {
  // 字段初始化：类型级 managed/callable 依赖。
  let a = Box1<T>();
  let b = test<T>();

  func UserType() {
    // 无参构造函数：类型级 managed/callable 依赖。
    let c = Box2<T>();
  }

  func UserType(x: int) {
    // 重载构造函数：类型级 managed/callable 依赖。
    let d = Box3<T>();
  }

  func ~UserType() {
    // 终结器：类型级 managed/callable 依赖。
    let e = Box4<T>();
  }
}
```

其中 `T` 只由闭合 owner 类型决定。成员字段、全部构造函数和终结器必须作为一个整体建立
`UserType<T>` 的 owner dependency set：先统一收集全部派生依赖，再统一去重、排序并一次性确定固定
slots。闭合为 `UserType<i64>`、`UserType<string>` 等实例时，由调用点生成完整闭合描述符数据，共享体
只按固定索引读取。任何单个字段、构造函数重载或终结器都不得独立分配自己的 owner slots。

## 2. 已确认的问题

### 2.1 owner callable dependency 缺少承载位置

当前 `FengTypeDescriptor` 和 `FengAggregateDescriptor` 已承载 owner 的：

- `reified_generic_params`；
- `reified_agg_deps`；
- `reified_type_deps`。

`FengFunctionDescriptor` 已承载 `reified_callable_deps`，但类型描述符没有对应字段。Semantic 已把字段
初始化、构造函数和终结器收集到 owner dependency set；Codegen 在 generic callable 调用处却只能生成
`_desc->reified_callable_deps[slot]`。构造共享体没有 `_desc` 参数，因此生成的 C 代码引用未声明标识符。

这不是构造函数缺少函数描述符，而是类型描述符的三类 owner dependency slots 尚未对齐。

### 2.2 当前发码框架已经区分依赖来源

构造函数和普通方法共用 `cg_emit_generic_type_method_shared` 主发码框架，但已有不同的依赖来源：

| 位置 | Semantic dependency set | aggregate/type 读取来源 | callable 读取来源 |
|---|---|---|---|
| 字段初始化 | owner set | `_td` | 当前缺失 |
| 构造函数 | owner set | `_td` | 当前错误地落到 `_desc` |
| 普通实例/静态方法 | member set | `_desc` | `_desc` |
| 终结器 | owner set | 尚未形成泛型共享路径 | 尚未形成泛型共享路径 |

因此本专项延续既有类型级路径，不给构造函数和终结器增加 `_func_desc` 隐藏参数。

### 2.3 泛型终结器的规范与实现不一致

[Feng 泛型规范草案](../specifications/feng-generics-draft.md) 已规定：泛型 `type` 可以定义终结器；终结器
自身不得声明类型参数，并继续遵守无参数、无非 `void` 返回值、不可重载等既有规则。

当前 Semantic 仍以 `AE0316` 拒绝泛型类型声明终结器，错误码目录也保留了这一旧规则。这与权威泛型
规范不一致。本专项必须移除“owner 为泛型类型”这一 blanket rejection，并同步收敛错误码目录及对应
compiler regression；不得放宽 `@value type`、`@abi type` 或终结器自身声明泛参等既有禁止规则。

构造函数和终结器都不得声明自己的方法级类型参数；它们只能使用 owner 的类型参数。Parser 可以复用
通用 callable 语法并保留对应 AST，但 Semantic 必须在依赖收集和 Codegen 之前明确拒绝这些声明：

```feng
type UserType<T> {
  func UserType<U>() {}   // Semantic 错误：构造函数不得声明方法级泛参。
  func ~UserType<U>() {}  // Semantic 错误：终结器不得声明方法级泛参。
}
```

该规则须先在权威泛型规范中明确。非法声明中的 `U` 不得进入 owner dependency set，也不得继续生成
任何共享体、描述符或 wrapper。

### 2.4 开放泛型引用实例的字段偏移未统一使用闭合描述符

共享体内不仅 `value: T` 本身的存储依赖闭合类型；位于该字段之后的普通字段，其实际偏移也会随 `T`
的闭合布局变化。当前发码仅对字段类型直接为泛参的访问使用 `reified_field_offsets`，对同一开放泛型
实例中的其他字段仍使用开放占位 C 结构体直接访问。当 `T` 闭合为 descriptor-sized 聚合值时，这会让
后续字段读写落到错误地址；标量和托管指针实例只是因占位宽度相同而偶然可用。

因此，共享体访问开放泛型引用实例的任意字段时，都必须从该实例的闭合 `FengTypeDescriptor` 取得固定
字段偏移。该规则同时覆盖读取、普通赋值、复合赋值和托管/聚合字段的生命周期操作，不按字段名称、
字段类型、构造函数、终结器或静态初始化场景增加特判。普通闭合类型与非泛型类型继续使用编译期 C
结构体偏移。

## 3. 方案一及职责边界

### 3.1 描述符职责

`FengTypeDescriptor` 与 `FengAggregateDescriptor` 增加：

```c
size_t reified_callable_deps_count;
const struct FengFunctionDescriptor *const *reified_callable_deps;
```

类型级描述符完整承载类型 owner 代码的三类派生依赖：

| owner 代码位置 | dependency slots |
|---|---|
| 实例字段声明类型与初始化表达式（含成员 mixin 的源构造表达式） | type/aggregate/callable descriptor |
| 静态字段声明类型与初始化表达式 | type/aggregate/callable descriptor |
| 隐式构造阶段 | type/aggregate/callable descriptor |
| 每个显式构造函数的参数类型与函数体 | type/aggregate/callable descriptor |
| 普通引用类型的终结器函数体 | type/aggregate/callable descriptor |

普通实例方法与静态方法仍拥有独立 member dependency set，并通过 `FengFunctionDescriptor` 读取三类
依赖。顶层泛型函数、fit 共享方法及 Lambda 的既有函数描述符规则不变。

### 3.2 owner dependency set

同一开放泛型 owner 只建立一套规范化 dependency set。它按稳定声明身份和闭合类型实参形成 key，合并
字段、全部构造函数及终结器的依赖并去重，再分别为 aggregate、managed type 和 callable 分配稳定
slot。这里的“整体确定”是指：

1. 先遍历该 owner 的全部实例/静态字段、全部显式或隐式构造路径以及终结器；
2. 收集每个位置直接或传递引用的全部待具化依赖；
3. 按依赖种类分别形成 aggregate、managed type、callable 三个 owner-wide 集合；
4. 每个集合内部统一规范化、去重、稳定排序并分配从零开始的 slot；
5. 最后才为各字段、构造函数和终结器建立“依赖 key → owner slot”的编译期映射。

三类依赖拥有各自独立的 slot 空间，但同一类依赖在所有 owner 成员之间共用一张表。不同字段、不同
构造函数重载和终结器不得建立局部 slot 编号，也不得按成员顺序简单拼接局部数组。相同闭合依赖无论
出现于多少个成员位置，都必须映射到同一个 owner slot。

某个闭合类型实例的 descriptor 可以包含未被本次构造路径使用的其他构造函数依赖。这些依赖是编译期
生成的只读指针表，不产生运行时查找、遍历、分配或初始化分派；换取的是构造、终结和字段初始化统一
使用 owner descriptor，并避免每个构造函数重复生成函数描述符。

owner slot schema 属于泛型类型生成 ABI 的编译期契约。provider 输出 `.ft` 时必须保存 consumer 重建
完整 owner dependency set、稳定 key 和 slot 顺序所需的信息，包括共享主体实际使用但不构成源码公开
成员面的依赖；这类编译器元数据进入 `.ft` 不会改变对应成员的 Feng 可见性。consumer 不得只根据自己
可见的字段或构造函数重新计算 slots，否则其闭合类型描述符会与 provider 已编译共享体中的固定索引
不一致。

### 3.3 统一依赖来源抽象

Codegen 不复制两套表达式或 callable 发码逻辑。现有 `has_func_desc` 所承载的隐含含义应收敛为明确的
依赖来源，例如：

```c
typedef enum CGReifiedDependencySource {
    CG_REIFIED_DEP_SOURCE_TYPE,
    CG_REIFIED_DEP_SOURCE_FUNCTION
} CGReifiedDependencySource;
```

同一套 aggregate/type/callable slot mapping 根据来源生成：

```text
TYPE:     _td->reified_agg_deps[slot]
          _td->reified_type_deps[slot]
          _td->reified_callable_deps[slot]

FUNCTION: _desc->reified_agg_deps[slot]
          _desc->reified_type_deps[slot]
          _desc->reified_callable_deps[slot]
```

依赖来源在编译期确定，不得在生成的 Feng 程序中增加运行时分支。

### 3.4 字段初始化与构造函数

实例创建仍遵循“字段声明及绑定初值 → 构造函数 → 对象字面量初始化”的既有顺序。字段初始化和显式
构造函数共享同一个 `_type_desc`，但各自按编译期 slot 使用实际依赖；不能把字段依赖复制到每个构造
函数的独立函数描述符。

需要完整覆盖：

- 只有隐式构造函数的泛型类型；
- 一个显式无参构造函数；
- 多个重载构造函数；
- provider 编译字段初始化共享入口、consumer 仅通过 `.ft` 和二进制包构造闭合实例；
- 普通引用类型与 `@value type`；
- 字段推断类型和显式字段类型；
- 实例字段和静态字段初始化。

共享字段初始化中的“未显式初始化字段”仍必须遵守既有 default-zero 语义。`FengTypeDescriptor` 增加统一
的 `default_zero_init(value_out, descriptor)` 入口；编译器在每个闭合 managed 类型描述符中静态填入该
类型的默认零值实现。共享体初始化未知 managed 泛参 `T` 时，通过 `T` 的具化描述符调用该入口；不得把
managed 泛参统一写为 `NULL`。

各类型仍由编译器按既有语义处理自身这一层：`string` 产生空字符串，array 依据闭合元素描述产生空数组，
callable 产生编译器生成的 noop closure，普通及泛型 `type` 递归初始化每个字段的类型默认零值。普通和
泛型 `type` 的 default-zero 都不得执行字段声明初始化表达式、隐式/显式构造函数或对象字面量阶段。
复合类型描述符只引用其直接子级描述符，不在 default-zero 入口中进行类型名称判断、动态具化或递归
描述符查找。

静态已知类型继续使用既有直接发码，不增加间接调用；非泛型数组、已闭合数组、非泛型 `type` 和已闭合
`type` 的 default-zero 运行时路径及开销必须与改造前完全一致。闭合数组描述符必须作为编译期生成的
静态只读数据或既有固定 reified slot 提供，不得在运行时临时组装。仅共享体初始化真正未知的 managed
泛参时，才通过具化描述符进行一次函数指针调用。该路径不得增加运行时名称查找、slot 搜索、缓存、锁、
装箱，或 default-zero 语义之外的对象分配。

### 3.5 泛型类型终结器

普通泛型 `type` 的 provider 生成一个可由 consumer 链接的共享终结器主体；它是包内生成 ABI 的导出
符号，不是 Feng 源码中的公开成员：

```c
void Feng_UserType_finalizer_shared(
    void *self,
    const FengTypeDescriptor *_type_desc);
```

每个闭合 `UserType<Arg>` 生成匹配现有 `FengFinalizerFn(void *self)` ABI 的薄 thunk。该 thunk 只把闭合
类型描述符静态绑定给共享主体；类型描述符的 `.finalizer` 指向该 thunk。consumer 具化 imported 泛型
类型时，必须能够引用 provider 导出的共享主体并生成本地闭合 thunk。

该设计保持 runtime `FengFinalizerFn` ABI 不变，也不增加对象字段、动态名称查找、描述符遍历或堆分配。
终结器只有在对象实际终结时多一层闭合 thunk 调用；这是共享二进制主体绑定闭合 owner descriptor 所需
的固定适配层。非泛型类型的现有终结器发码不变。

以下既有规则保持不变：

- `@value type` 禁止终结器；
- `@abi type` 禁止终结器；
- 终结器不得声明类型参数、参数或非 `void` 返回值；
- 每个类型至多一个终结器，且不得直接调用；
- 终结器内异常不得传播到终结器边界之外。

### 3.6 闭合描述符图

闭合 `UserType<Arg>` 时，Codegen 必须递归 ensure owner dependency set 中的：

1. aggregate descriptor；
2. managed type descriptor；
3. direct generic callable 的 `FengFunctionDescriptor`；
4. 上述函数描述符继续引用的闭合依赖。

所有 descriptor shell 和符号前向声明先注册，再连接只读依赖数组，以支持相互引用的静态描述符图。
共享体只读取调用点已生成的闭合图，不得在运行时递归具化、按名称查找或缓存补全。

### 3.7 开放泛型引用实例的字段访问

共享体中的开放泛型引用实例先通过现有 type dependency 固定 slot 取得闭合 `FengTypeDescriptor`，随后按
编译期已确定的字段索引读取 `reified_field_offsets[field]`。字段类型是否直接引用泛参不影响该选择；
因为决定字段地址的是整个闭合 owner 的布局，而不是该字段自身的类型。

同一字段访问只读取一次对应字段偏移。字段地址确定后，标量、托管引用、descriptor-sized 聚合值及直接
泛参继续复用既有读写与所有权规则，不增加运行时分支、名称查找、slot 搜索或动态具化。

## 4. ABI 与性能边界

### 4.1 已确认的私有 ABI 调整

本专项按人工确认的方案一，为 `FengTypeDescriptor` 和 `FengAggregateDescriptor` 增加 callable dependency
计数与数组指针。它们属于 Feng 编译器和 runtime 之间的私有描述符 ABI，所有静态初始化器和读取方必须
同步更新。

同时为 `FengTypeDescriptor` 增加 managed 值的 `default_zero_init` 入口。具化后的普通 type、泛型 type、
string、array 和 callable descriptor 均必须填入准确实现；`FengGenericParamDescriptor` 不重复保存该入口。
trivial 与 aggregate 分别继续使用 `FengTrivialDescriptor.size` 和 `FengAggregateDescriptor.default_init`。

不修改：

- `FengFunctionDescriptor` 已有字段及含义；
- runtime `FengFinalizerFn(void *self)`；
- 普通 Feng 方法、构造函数的用户可见签名；
- 普通非泛型类型的调用 ABI；
- 方法级泛参继续使用的独立隐藏描述符参数。

### 4.2 运行时性能要求

- 构造函数和终结器不增加 `_func_desc` 隐藏参数；
- 每个实际依赖使用点只进行一次既有形式的固定 slot 间接读取；同一使用点需要多次使用时先读取到局部
  变量，不得重复读取 descriptor slot；
- 不增加运行时名称查找、slot 搜索、依赖遍历、动态具化、缓存、锁或堆分配；
- 没有 callable dependency 的描述符使用 `0/NULL`，对应代码不产生额外读取；
- 普通非泛型路径保持原发码和运行时成本；
- 非泛型及已闭合数组/type 的 default-zero 保持既有直接发码，不进入 descriptor 间接分发；
- 已闭合数组的描述符只使用静态只读数据或既有固定 slot，不在运行时构造；
- descriptor 增加的只读数组只包含编译期去重后的依赖指针。

如实施中发现必须突破以上任一性能或 ABI 边界，立即停止并提交人工 Review。

## 5. 实施步骤

### 5.1 规范与诊断收敛

1. 在泛型主规范明确构造函数和终结器都不得声明方法级泛参，只能使用 owner 泛参；
2. 保持“泛型 owner 可以声明非泛型终结器”的现有权威规则；
3. 移除错误码目录中“泛型类型禁止终结器”的旧条目，并保留实际有效的终结器诊断；
4. 识别需要调整的既有负向测试；修改任何既有测试前再次取得人工批准，函数名与新语义保持一致。

### 5.2 描述符与依赖收集

1. 扩展 `FengTypeDescriptor` 与 `FengAggregateDescriptor`；
2. 将字段、全部构造函数和终结器作为整体收集到唯一 owner dependency set；
3. 按 aggregate、managed type、callable 三个独立 slot 空间统一去重、稳定排序并一次性确定偏移；
4. 为 owner direct callable dependency 建立稳定 identity、key、排序和去重；
5. 确保 `.ft` 保存并恢复完整 owner slot schema，consumer 不按可见成员子集重新计算；
6. 确保 `.ft` 中恢复的字段、构造函数、终结器及调用目标拥有生成闭合依赖所需的编译期信息；
7. 在依赖收集前由 Semantic 明确拒绝构造函数和终结器的方法级泛参，不把非法泛参并入 owner set。

### 5.3 闭合描述符生成与共享发码

1. 为闭合 `FengTypeDescriptor`/`FengAggregateDescriptor` 生成 callable dependency 数组；
2. 让通用 descriptor graph 注册管线递归生成 owner callable dependencies；
3. 将共享发码的依赖来源显式化，三类 slot 使用一致的 TYPE/FUNCTION 选择；
4. 字段、隐式构造和所有显式构造函数统一从 `_td` 读取；
5. 开放泛型引用实例的全部字段读写统一使用闭合类型描述符中的 `reified_field_offsets`；
6. 为闭合 managed 类型描述符生成正确的 default-zero 入口，并让未知 managed 泛参通过 descriptor 调用；
7. 普通方法、静态方法、fit 和顶层函数继续从 `_desc` 读取并保持回归通过。

### 5.4 泛型终结器

1. 移除 Semantic 对泛型 owner 终结器的旧 blanket rejection；
2. 生成 provider 共享终结器主体；
3. 为每个闭合类型生成 runtime ABI thunk 并绑定闭合类型描述符；
4. 支持 imported 泛型类型在 consumer 侧生成闭合 thunk；
5. 验证正常 ARC、异常清理和循环回收路径仍只按既有生命周期协议调用一次终结器。

### 5.5 测试与恢复原计划

compiler regression、FCTS 和沙箱外 `make test` 均已完成。全量回归通过后已：

1. 将本文状态改为“已完成”；
2. 恢复 [feng-generic-cross-feature-fcts-hardening-dev.md](./feng-generic-cross-feature-fcts-hardening-dev.md)；
3. 将其中第三组按本专项结果标记完成；后续从第四组继续，不重复维护本专项的 ABI 规则。

## 6. 测试计划

### 6.1 FCTS 成功行为

成功行为尽可能放入 FCTS。provider 定义于 `fcts_lib`，consumer 与断言定义于 `fcts_bin`，并使用
`std.test`，不输出额外文本。至少覆盖：

1. 泛型普通 `type` 的两个字段分别执行 `Box1<T>()` 和 `test<T>()`；
2. 无参构造函数执行 `Box2<T>()`；
3. 带参重载构造函数执行 `Box3<T>()`；
4. 终结器执行 `Box4<T>()`，并以确定的计数或可观察状态证明只执行一次；
5. 隐式构造函数只有字段初始化时使用相同 owner slots；
6. 泛型 `@value type` 的字段初始化及多个构造函数使用 aggregate descriptor owner slots；
7. 泛型静态字段初始化调用泛型 callable；
8. `T` 分别闭合为标量、托管引用和 descriptor-sized `@value type`；
9. provider 本包使用与 consumer 跨包使用结果一致；
10. 字段值、构造函数结果、终结器副作用和托管叶子生命周期均有真实断言，不能只验证可编译。

FCTS 不承担循环回收场景；泛型终结器参与循环回收的验证放入 `test/` 现有生命周期测试层。

### 6.2 Compiler regression

- Semantic：泛型 owner 终结器合法；构造函数和终结器声明方法级泛参均明确报错；两者使用 owner 泛参
  合法；`@value`、`@abi` 终结器仍非法；
- Codegen：类型和 aggregate 描述符生成稳定 callable slots；构造共享体不引用 `_desc`；普通方法仍从
  `_desc` 读取；
- Codegen：字段、两个构造函数和终结器整体确定 owner slots；同一依赖跨成员复用相同 slot，且不会生成
  构造函数 `FengFunctionDescriptor`；
- Codegen：泛型终结器共享主体接收 type descriptor，闭合 thunk 保持 `void (*)(void *)`；
- Codegen：宽聚合泛参之前和之后的字段均按闭合引用类型描述符读写，不能使用开放占位结构偏移；
- 跨包：provider 完整 owner slot schema、`.ft` 恢复和 consumer 闭合 descriptor graph 一致，私有成员
  不因元数据导出而改变可见性；
- Lifecycle：普通释放、异常离开构造路径和循环回收中，终结器及其泛型局部值无泄漏、重复释放或悬垂。

### 6.3 每阶段验证

测试实施时，每完成一个独立行为组，先运行相关 compiler test 和：

```sh
./build/bin/feng run fcts/fcts_bin
```

随后在沙箱外执行：

```sh
make test
```

发现缺陷即暂停下一组，先记录根因并完成通用修复；禁止为某个测试类型、构造函数重载、终结器、容器或
包路径增加特判。

## 7. 完成标准

- [x] 泛型主规范与错误码目录明确构造函数/终结器方法级泛参的 Semantic 禁止规则
- [x] Semantic 在依赖收集前拒绝构造函数和终结器的方法级泛参
- [x] `FengTypeDescriptor` 增加 owner callable dependency slots
- [x] `FengAggregateDescriptor` 增加 owner callable dependency slots
- [x] owner 字段、隐式/显式构造函数、终结器作为整体完成统一收集、排序、去重和 slot 分配
- [x] `.ft` 完整保存 owner slot schema，consumer 与 provider 使用完全相同的固定偏移
- [x] TYPE/FUNCTION 依赖来源覆盖 aggregate/type/callable 三类 slot
- [x] 泛型字段初始化和多个构造函数完整支持示例中的 `Box1<T>`、`test<T>`、`Box2<T>`、`Box3<T>`
- [x] 泛型普通 `type` 终结器完整支持示例中的 `Box4<T>`
- [x] 开放泛型引用实例的全部字段读写使用闭合描述符偏移
- [x] `FengTypeDescriptor.default_zero_init` 覆盖 string、array、callable、普通及泛型 type
- [x] 未知 managed 泛参使用具化 descriptor 的 default-zero 入口而不是写入 `NULL`
- [x] imported provider 与 consumer 二进制分发路径通过
- [x] 普通 `type`、`@value type`、静态字段及三类代表性 `T` 的 FCTS 通过
- [x] Semantic、Codegen、跨包和生命周期 compiler regression 通过
- [x] 未增加构造/终结器 `_func_desc`、运行时搜索、遍历、分配或动态具化
- [x] 沙箱外 `make test` 全量通过
- [x] 恢复跨特性 FCTS 加固文档并将第三组标记完成；后续从第四组继续
