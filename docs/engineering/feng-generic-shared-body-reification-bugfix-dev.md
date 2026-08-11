# Feng 泛型共享体具化修复开发文档

> 状态：已完成
>
> 实施顺序：本专项完成并通过全量回归后，再继续
> [feng-generic-advanced-composition-fcts-hardening-dev.md](./feng-generic-advanced-composition-fcts-hardening-dev.md)。
>
> 关联主设计：
> [feng-generic-optimize-dev.md](./feng-generic-optimize-dev.md)、
> [feng-generics-delivered.md](./feng-generics-delivered.md)。

## 1. 目标

修复泛型共享体不能稳定获得闭合派生类型和闭合 callee 函数描述符的问题，使以下三类共享体使用同一套
具化规则：

1. 顶层泛型函数；
2. 泛型静态方法，包括泛型 owner 的静态方法和方法自身声明泛型参数的静态方法；
3. 泛型实例方法，包括泛型 owner 的实例方法和方法自身声明泛型参数的实例方法。

每类共享体都必须完整支持：

- 参数类型直接或传递引用类型级、函数级或方法级泛型参数；
- 返回类型直接或传递引用上述泛型参数；
- 共享体内部显式或推断声明派生泛型局部值，例如 `List<T>`、`Box<U>`、
  `Action<List<T>>`；
- 同一共享体内同时存在多个独立泛型参数及多层嵌套派生类型；
- 调用另一个需要 `FengFunctionDescriptor` 的泛型共享 callable；
- provider 本包调用与 consumer 通过 `.ft` 使用预编译共享体得到相同结果。

本专项不改变 Feng 泛型语言语义，不改变重载、类型推导、约束、variance 或转换规则。

## 2. 已确认的问题

### 2.1 顶层函数已有描述符，但闭合依赖没有完整生成

顶层泛型函数已经接收：

```c
const FengFunctionDescriptor *_desc;
const FengGenericParamDescriptor *_T;
```

共享体也已经能够按固定索引读取 `_desc->reified_agg_deps[i]` 和
`_desc->reified_type_deps[i]`。缺口位于调用点的闭合物化管线。

最小场景：

```feng
func roundTrip<T>(value: T): T {
  let values = List<T>();
  values.add(value);
  return values.get(0);
}
```

当前推断局部声明会在共享体映射阶段报告：

```text
CE0007: codegen: no reified_type_dep found for generic managed dependency
```

改为显式声明 `let values: List<T> = List<T>();` 后，具体调用点仍可能报告：

```text
CE0031: codegen: generic type/spec instance 'List<...>' was not registered
```

`CE0031` 发生在编译期：codegen 已将 `List<T>` 替换为 `List<i64>` 等闭合类型引用，但在
`cg->user_types` 中找不到对应的闭合实例，因而无法取得 descriptor symbol 并填入
`FengFunctionDescriptor`。它不是运行时读取 `_desc` 失败。

### 2.2 普通静态方法和实例方法没有 callable 自身的函数描述符

当前泛型类型方法共享体主要从 `_type_desc` 获取类型级参数、字段偏移及派生依赖。方法级参数 `U` 仍以
独立 `FengGenericParamDescriptor *` 传入，但普通静态方法和实例方法没有统一接收描述本次方法调用的
`FengFunctionDescriptor *`。

现有语义收集还会把类型字段、字段初始化器及全部成员方法的 reifiable dependency 合并到 owner type
的 dependency set。对于 `Flow<T>.map<U>()`，这会在只有 `T` 已闭合时错误尝试将依赖 `U` 的
`Composite<U>` 写入 `Flow<T>` 的 `FengTypeDescriptor`。

### 2.3 泛型共享 caller 不能通用传递 callee 的闭合函数描述符

```feng
func inner<T>() {
  let values = List<T>();
}

func outer<T>() {
  inner<T>();
}
```

`outer` 可以继续将 `_T` 作为独立隐藏参数转发给 `inner`，但 `_T` 不能代替 `inner<T>` 自己的
`FengFunctionDescriptor`。后者还需要保存闭合的 `List<T>` 等直接依赖，并使用由 `inner` 共享体
确定的 slot 布局。

当前 `FengFunctionDescriptor` 只能保存 aggregate 和 managed type 依赖，不能保存调用点已经静态
物化的 callee 函数描述符。

### 2.4 现有覆盖为什么没有暴露基础缺口

现有 std 和 FCTS 已覆盖泛型 type 字段中的 `List<T>`，例如：

```feng
type Holder<T> {
  let items = List<T>();
}
```

该路径在闭合 `Holder<i64>` 时将 `List<i64>` 放入 owner `_type_desc->reified_type_deps`。现有用例随后
通过成员方法使用已经初始化的字段，但没有覆盖顶层函数或成员方法局部创建开放 `List<T>` 的准确场景，
因此不能证明 callable-local dependency 管线正确。

## 3. 设计不变量

1. 泛型参数描述符保持独立隐藏参数：顶层函数的 `T`、方法级的 `U` 不移入
   `FengFunctionDescriptor`。
2. `FengTypeDescriptor` / `FengAggregateDescriptor` 只拥有类型本身的结构性具化信息，包括类型级
   泛型参数、字段布局、字段及字段初始化所需的直接派生依赖和闭合静态状态。
3. `FengFunctionDescriptor` 拥有一个具体 callable 调用实例的直接 aggregate、managed type 和
   callable 依赖。
4. 每层描述符只保存直接依赖。外层 descriptor 不拍平 callee 内部全部依赖；callee descriptor
   继续负责自己的直接依赖。
5. 具体 wrapper 在编译期递归闭合并静态生成完整 descriptor graph。共享体运行时不构造 descriptor，
   不按名称查找，不遍历泛型类型树，也不进行堆分配。
6. provider 本地声明和从 `.ft` 恢复的 imported 声明必须使用相同的依赖身份、参数顺序、slot 顺序
   和共享体 ABI。
7. 所有收集、替换、注册和 slot 分配规则必须由规范化语义类型及已解析 callable 身份驱动，禁止针对
   `List`、`Box`、`Func`、某个包、某种参数位置或当前测试模型增加特判。

本专项对 [feng-generic-optimize-dev.md](./feng-generic-optimize-dev.md) 中“泛型 type 聚合其全部方法
依赖”的旧实现规则作出修正：方法签名和方法体的 callable-local dependency 改由该方法自己的
`FengFunctionDescriptor` 承载；类型结构性依赖继续留在 type descriptor。

## 4. 描述符与共享体 ABI

### 4.1 扩展 `FengFunctionDescriptor`

```c
typedef struct FengFunctionDescriptor {
    const char *name;

    size_t reified_agg_deps_count;
    const FengAggregateDescriptor *const *reified_agg_deps;

    size_t reified_type_deps_count;
    const FengTypeDescriptor *const *reified_type_deps;

    /* 当前闭合 callable 直接调用的其他闭合泛型 callable 描述符。
     * slot 由编译期规范化依赖 key 决定；运行时不做名称查找。 */
    size_t reified_callable_deps_count;
    const struct FengFunctionDescriptor *const *reified_callable_deps;
} FengFunctionDescriptor;
```

无 callable dependency 时，新字段为 `0/NULL`。这只扩展 runtime 私有 descriptor ABI，不新增
runtime 函数或公开语言 API。

### 4.2 共享体隐藏参数

顶层泛型函数保持现状：

```c
void top_shared(const FengFunctionDescriptor *_func_desc,
                const FengGenericParamDescriptor *_T,
                ...);
```

泛型静态方法增加 `_func_desc`：

```c
void static_method_shared(const FengTypeDescriptor *_type_desc,
                          const FengFunctionDescriptor *_func_desc,
                          const FengGenericParamDescriptor *_U,
                          ...);
```

泛型实例方法增加 `_func_desc`：

```c
void method_shared(void *_self,
                   const FengTypeDescriptor *_type_desc,
                   const FengFunctionDescriptor *_func_desc,
                   const FengGenericParamDescriptor *_U,
                   ...);
```

泛型 `@value type` 的 owner descriptor 继续使用 `FengAggregateDescriptor *`，其他参数顺序相同。
类型级泛型参数继续从 `_type_desc->reified_generic_params` 获取；方法级参数继续作为独立隐藏参数传入。
非共享的普通静态方法和实例方法不增加 `_func_desc`。

本专项覆盖顶层函数、静态方法和实例方法。现有 fit 共享方法已经具有 `_desc` 路径，必须适配新增字段并
保持回归通过；构造器不在本专项新增 ABI 的范围内，既有字段初始化和构造路径不得回归。

### 4.3 descriptor graph 示例

对 `outer<i64>() -> inner<i64>() -> List<i64>`，wrapper 静态生成：

```c
static const FengTypeDescriptor *inner_i64_type_deps[] = {
    &List_i64_desc,
};

static const FengFunctionDescriptor inner_i64_desc = {
    .name = "inner<i64>",
    .reified_type_deps_count = 1,
    .reified_type_deps = inner_i64_type_deps,
};

static const FengFunctionDescriptor *outer_i64_callable_deps[] = {
    &inner_i64_desc,
};

static const FengFunctionDescriptor outer_i64_desc = {
    .name = "outer<i64>",
    .reified_callable_deps_count = 1,
    .reified_callable_deps = outer_i64_callable_deps,
};
```

`outer` 共享体只读取直接 callee：

```c
inner_shared(
    _func_desc->reified_callable_deps[0],
    _T
);
```

`inner` 共享体再读取自己的直接类型依赖：

```c
const FengTypeDescriptor *list_desc =
    _func_desc->reified_type_deps[0];
```

运行时没有递归遍历；每层只进行一次由编译期固定 slot 决定的直接读取。

## 5. 语义依赖模型

### 5.1 dependency owner

reifiable dependency 必须按 domain 分离：

| 来源 | dependency owner | 运行时入口 |
|------|------------------|------------|
| 泛型 type 字段、字段推断类型、字段初始化器及类型布局 | type 声明 | `_type_desc` |
| 顶层泛型函数签名和函数体 | 顶层函数声明 | `_func_desc` |
| 静态方法签名和方法体 | owner type + 静态方法声明 | `_func_desc` |
| 实例方法签名和方法体 | owner type + 实例方法声明 | `_func_desc` |

方法 dependency key 不能只使用 owner type。语义侧必须用“owner 声明 + 具体成员声明”标识方法，以免
不同方法的依赖集合互相污染。方法活动泛型参数顺序固定为：owner 类型参数在前，方法参数在后。

### 5.2 direct type dependency

语义阶段从规范化类型事实收集当前 callable 直接使用、且含活动泛型参数的完整类型表达式，包括：

- 参数和返回类型；
- 显式局部绑定类型；
- 推断局部绑定的语义类型；
- 泛型构造目标、对象字面量目标、数组元素及 tuple / `@value type` 中间值；
- callable 参数、返回值和捕获中出现的嵌套泛型类型；
- 分支、循环、提前返回和 lambda lowering 实际物化的派生泛型类型。

显式与推断写法必须得到相同依赖：

```feng
let a: List<T> = List<T>();
let b = List<T>();
```

codegen 不再独立通过 AST 形态重新猜测 reifiable dependency。语义 dependency set 是调用点闭合和共享体
slot 分配的唯一事实来源。

### 5.3 direct callable dependency

共享体调用另一个需要函数描述符的泛型共享 callable 时，记录一条 direct callable dependency。记录至少
包含：

1. 已解析 callee 的稳定声明身份，能够区分 owner、静态/实例形式、重载签名和泛型 arity；
2. callee owner 类型参数按声明顺序对应的 caller 视角类型表达式；
3. callee 函数或方法级类型参数按声明顺序对应的 caller 视角类型表达式；
4. callee 是否需要 aggregate、managed type 或下一级 callable dependency。

同一 caller 中“相同 callee 声明 + 相同完整类型实参”只分配一个 slot；不同泛型实参分别分配。不得仅按
callee 名称合并。

callee 无任何直接或传递 reifiable dependency 时，不需要 callable dependency slot；泛型参数描述符仍按
现有隐藏参数 ABI 直接转发。若该 callee 的统一共享 ABI 仍要求 `_func_desc`，caller 传入编译期静态空
descriptor，不为它增加 caller-specific dependency slot。

### 5.4 递归与相互递归

descriptor graph 可能包含自环或互相引用。编译期必须以“稳定 callable 身份 + 完整闭合类型实参”作为
节点 key，先注册 descriptor shell，再递归连接 direct dependency，禁止按源码调用树无限展开。

静态 descriptor 可以通过前置声明互相引用。运行时不进行递归初始化或延迟缓存。

## 6. 编译期闭合与发码

### 6.1 统一闭合入口

增加一个通用、幂等的闭合物化入口，职责是：

1. 将 dependency 中的 owner / callable 泛型参数替换为本次调用的闭合类型实参；
2. 递归注册完整类型树中的 generic type 和 generic spec instance shell；
3. 完成各实例成员、布局、生命周期及 descriptor 的静态生成；
4. 在实例已经存在时复用既有节点；
5. 返回闭合的 aggregate/type/function descriptor symbol。

必须先 ensure/register，再解析 descriptor name。禁止在尚未注册闭合实例时直接调用查找函数并以
`CE0031` 结束。

### 6.2 调用点生成 descriptor graph

具体 wrapper 已知本次闭合类型实参，按以下顺序生成：

1. 读取 caller 的 open reifiable dependency set；
2. 按 owner 参数、函数参数或方法参数完成替换；
3. ensure 所有 direct aggregate 和 managed type dependency；
4. ensure 所有 direct callable dependency 的函数描述符节点；
5. 先注册全部函数 descriptor shell，再连接依赖数组，以支持递归；
6. 生成 `static const` 描述符及数组；
7. wrapper 将 root `_func_desc`、独立泛型参数描述符及普通参数传入共享体。

### 6.3 共享体 slot 映射

open dependency 和 closed dependency 必须由同一个规范化 key 算法分配 slot：

- 泛型参数使用稳定位置 `T0`、`T1`，不使用源码参数名；
- generic type key 包含完整嵌套结构；
- callable key 包含稳定 callee 身份、owner 类型实参和 callable 类型实参；
- aggregate、managed type、callable 分别建立独立 slot 空间；
- provider 和 consumer 对相同 imported 声明必须生成相同顺序。

共享体不得再依赖“开放泛型实例的 C descriptor symbol name”确定 slot。开放 `List<T>` 没有可供运行时
直接使用的闭合 symbol；symbol 只在调用点完成闭合注册后用于填充对应 slot。

### 6.4 参数、返回值和共享体局部

本专项不重新定义现有共享泛型值 ABI。既有规则继续成立：

- 直接泛型参数按其现有固定/地址 ABI 传递；
- descriptor-sized 参数使用地址表示；
- descriptor-sized 返回值使用调用方提供的 `_out`；
- 直接泛型参数返回值同样由调用方按 `FengGenericParamDescriptor` 的闭合大小声明对齐存储，不能使用
  指针大小的 `void *` 局部变量代替返回槽；
- 派生 aggregate 的大小、字段偏移、复制和清理由闭合 `FengAggregateDescriptor` 决定；
- 派生 managed type 的分配和泛型方法调用使用闭合 `FengTypeDescriptor`；
- 局部显式绑定、推断绑定、临时值和返回接收槽必须使用同一闭合描述符，不能落回 open C 占位布局。

直接泛型参数值在共享体内统一采用“存储地址”表示。每个 owning 存储声明时只读取一次泛型参数
descriptor、只计算一次闭合大小；参数转发不得再次对存储地址取址。其作用域清理由 descriptor 的
`kind` 选择既有 pointer cleanup 或 aggregate cleanup，trivial 值不注册 cleanup。该规则统一覆盖顶层函数、
静态方法、实例方法、callable/spec 调用和递归调用，不按调用形式增加分支特判。

## 7. `.ft` 与跨包恢复

### 7.1 导出范围

`.ft` 必须为顶层函数、静态方法和实例方法分别导出：

- direct reifiable aggregate dependencies；
- direct reifiable managed type dependencies；
- direct reifiable callable dependencies；
- callable dependency 的稳定 callee 身份及按“owner 在前、callable 在后”排列的开放类型实参表达式。

现有 `FengSymbolDeclView` 的 aggregate/type dependency 字段需要扩展到普通方法符号；另增加结构化的
callable dependency view。callable dependency 不能降级为函数名字符串，必须在导出前绑定到已解析的
声明身份，并在导入后恢复为可唯一解析的符号引用。

### 7.2 FT 编码约束

FT 编码可以复用现有 SYMS 对 callable 声明的身份、TYPS 对类型表达式的编码和 TSEQ 对有序类型序列的
编码，但必须增加明确的 callable dependency 记录，至少保存：caller symbol、callee symbol 引用以及
完整有序类型实参序列。

若 callee 位于其他 package，记录必须保留可跨 module graph 唯一恢复的声明身份；不能假设不同 `.ft`
文件中的本地 `symbol_id` 具有全局意义。具体记录布局在实现时必须作为 FT 私有格式的一次版本化扩展，
不得把重载签名或类型实参拼接为运行时字符串。

导入后恢复出的 dependency graph 必须与 provider 本地语义图等价，consumer 不读取 provider 源码，仍能
静态生成完整闭合 descriptor graph。

## 8. 运行时成本与安全边界

### 8.1 保持不变

- 不增加 runtime descriptor factory；
- 不增加动态布局解释器、descriptor cache 或同步；
- 不增加堆分配、boxing、反射或运行时名称查找；
- 不改变泛型参数值表示、ARC、aggregate walker 或 witness 语义；
- 非共享普通函数和方法不受影响。

### 8.2 固定增量

- 泛型静态方法和实例方法共享 ABI 增加一个 `_func_desc` 指针参数；
- 共享体读取自身 aggregate/type dependency 时进行一次现有形式的固定 slot 间接读取；
- 调用需要函数描述符的泛型 callee 时增加一次
  `_func_desc->reified_callable_deps[slot]` 固定索引读取；
- 所有 descriptor graph 节点和数组均为编译期生成的 `static const` 数据。
- 共享体实际拥有直接泛型参数临时值或局部值时，按既有泛型值 ABI 对 `kind` 做生命周期分派；
  descriptor 与 size 在该存储声明处各缓存一次。该分派是 erased owning 值正确 ARC/aggregate 清理所必需，
  不影响闭合非泛型路径，也不增加运行时查找、分配或类型树遍历。

不存在按泛型嵌套深度进行的运行时遍历。上述固定成本是本方案为保持共享二进制和零动态具化所引入的
完整运行时边界，实施中不得再扩大。

## 9. 测试计划

不得修改已有测试文件；新增 compiler tests 和 FCTS 文件完成覆盖。若确需修改已有用例，必须另行取得
人工批准。FCTS 行为用例只写入 `fcts_bin`；需要跨包模型时在 `fcts_lib` 增加独立文件。用例使用
`std.test`，测试库不通过 `puts` 或其他方式输出额外内容；`fcts_bin/src/main.ff` 仅在取得人工批准后增加
一次新测试入口调用。

### 9.1 第一组：顶层泛型函数直接依赖

覆盖显式和推断局部 `List<T>` / 等价自定义容器，至少以以下类型闭合：

- 标量；
- 普通 managed type；
- 含 managed slot 的宽 `@value type`；
- object-form 和 callable-form spec。

同时覆盖参数、返回值、局部值及两层以上嵌套派生泛型。先运行新增 compiler tests 和
`./build/bin/feng run fcts/fcts_bin`，随后在沙箱外执行 `make test`。

### 9.2 第二组：实例方法和静态方法直接依赖

分别覆盖：

- 泛型 owner 方法体使用 owner 的 `T`；
- 非泛型 owner 的泛型方法体使用方法级 `U`；
- 泛型 owner 的泛型方法同时使用 `T`、`U`；
- 同一泛型 owner 同时包含推断字段 `List<T>`，并在泛型方法中分别构造
  `List<T>`、`List<U>`，将两类局部值返回给调用方断言；
- 静态方法与实例方法；
- 参数、返回值、显式/推断局部值；
- provider 定义、consumer 具化的跨包路径。

本组完成后再次执行专项测试和 `make test`。

### 9.3 第三组：共享 caller 到共享 callee

覆盖：

- 顶层函数调用顶层泛型函数；
- 实例方法和静态方法调用顶层函数及其他方法；
- 类型实参直传、重排、裁剪和重复，例如 `<T, U> -> <U, T>`、`<T, U> -> <T>`、
  `<T> -> <T, T>`；
- callee 同时含 aggregate、managed type 和下一级 callable dependency；
- 同一闭合 callee 去重；
- 直接递归与相互递归；
- 跨包 descriptor graph 恢复。

本组完成后再次执行专项测试和 `make test`。

### 9.4 结构测试

compiler tests 需要锁定：

- 泛型 owner 的方法级 `T`、`U` 保持独立，`List<T>` 变量不能接收
  `List<U>`，并在定义处报告 `AE1003`；
- 顶层、静态、实例共享体隐藏参数顺序；
- `FengFunctionDescriptor.reified_callable_deps` 静态初始化结构；
- open dependency 与 closed dependency 的 slot 顺序一致；
- wrapper 在 descriptor symbol 解析前递归注册闭合实例；
- `.ft` callable dependency round-trip 后身份和类型实参顺序不变；
- descriptor graph 循环通过 shell 复用，不产生无限代码生成。

## 10. 实施步骤

- [x] 更新主泛型实现说明中 dependency owner 与方法 `_func_desc` ABI
- [x] 扩展语义 reifiable dependency 结构，按 type / callable domain 分离
- [x] 从规范化语义类型事实统一收集显式与推断依赖
- [x] 为 direct callable dependency 增加稳定身份、类型实参映射和去重 key
- [x] 扩展 `FengSymbolDeclView` 及 FT round-trip
- [x] 扩展 `FengFunctionDescriptor.reified_callable_deps`
- [x] 实现通用闭合 dependency ensure/register 管线，修复 `CE0031`
- [x] 统一规范化 key 与 slot 映射，修复 `CE0007`
- [x] 顶层函数 wrapper 正确生成 aggregate/type/callable descriptor graph
- [x] 静态方法共享体和 wrapper 增加 `_func_desc`
- [x] 实例方法共享体和 wrapper 增加 `_func_desc`
- [x] fit 既有 `_desc` 路径适配新增 descriptor 字段并回归
- [x] 第一组新增测试及 `make test`
- [x] 第二组新增测试及 `make test`
- [x] 第三组新增测试及 `make test`
- [x] 核对没有类型名、包名、参数位置或测试模型特判
- [x] 最终 `make test` 无失败和 sanitizer 报告

完成全部步骤并通过最终回归前，不得恢复
[feng-generic-advanced-composition-fcts-hardening-dev.md](./feng-generic-advanced-composition-fcts-hardening-dev.md)
的后续分组实施。
