# Feng 值类型方法值接收者捕获与泛型 FCTS 加固开发文档

> 状态：已通过人工 Review，实施中
>
> 方法值与 `self` 的权威语义以
> [Feng 函数规范](../specifications/feng-function.md) 和
> [Feng 类型规范](../specifications/feng-type.md) 为准；泛型规则以
> [Feng 泛型规范](../specifications/feng-generics-draft.md) 为准。
> [feng-value-type-dev.md §9.17](./feng-value-type-dev.md#917-值类型方法值的接收者捕获独立后续-todo)
> 已记录当前实现缺口。本文只组织实现、测试范围、实施顺序和验收门槛，不新增语言语义。

## 1 目标

完整实现值类型方法值的接收者捕获，并补齐其与泛型、共享体、跨包和生命周期组合的行为证据：

- 方法值形成时，值类型接收者按值复制到方法值持有的独立存储；
- 后续调用中的 `self` 始终引用该存储，调用本身不再次复制接收者；
- 值类型进入泛型参数、泛型字段、泛型参数/返回值或共享泛型 ABI 时始终保持值表示，不发生装箱；
- `@value type` 的自身方法、`@value type` 的 `fit` 方法和 tuple 的 `fit` 方法行为一致；
- 泛型接收者中的标量、托管引用、descriptor-sized 聚合、object-form spec 和 callable-form spec
  均按既有值模型正确复制、保留和释放；
- 本地与 imported `.ft` 声明、普通闭合调用点与共享泛型体使用一致的 callable ABI；
- 引用类型方法值的现有语义、ABI 和运行时开销保持不变。

新增成功行为优先放入 FCTS，并验证实际返回值、接收者状态和确定的生命周期结果，不能只验证源码可编译。

## 2 当前缺口

现有 callable method-value lowering 按普通托管对象接收者组织：bind 入口接收引用类型对象指针，closure
的 `_self` 通过普通托管引用赋值保存，invoke adapter 再把该指针作为实例 `self` 调用目标方法。该路径
没有表示“值接收者在形成方法值时复制，并由方法值长期持有该值存储”的能力。

现有 FCTS 已覆盖普通引用 `type` 的方法值，以及泛型值作为方法参数和返回值经过 callable ABI；但没有
覆盖以下非等价路径：

1. `@value type` 值直接形成自身方法值；
2. `@value type` 值直接形成 `fit` 方法值；
3. tuple 值直接形成 `fit` 方法值；
4. 泛型共享体形成并返回上述方法值；
5. 值接收者包含托管或 descriptor-sized 泛型成员时的方法值逃逸和清理。

## 3 范围与边界

### 3.1 纳入范围

- 非泛型 trivial/aggregate `@value type`，作为通用实现的基础回归；
- 泛型 `@value type<T>` 的自身方法与 `fit` 方法；
- 泛型具名 tuple 的 `fit` 方法；
- 只读方法、修改 `self.var` 的方法，以及含普通参数和返回值的方法；
- 参数或返回值包含 `T`、tuple 或 `Composite<T>` 的 callable ABI；
- 方法值的绑定、参数、返回、字段保存、复制、覆盖和调用；
- 顶层泛型函数、泛型 owner 实例方法、泛型 owner 静态方法和非泛型 owner 泛型方法；
- 本地声明、跨包 `.ft` 恢复、imported `fit` 和共享泛型体；
- 正常离开作用域、逃逸、覆盖和异常展开时的生命周期；
- 与本专项直接相关的 Semantic 诊断和 Codegen 结构回归。

### 3.2 不纳入范围

- `Promise<T>` / `Future<T>` 等标准库泛型专项；
- 闭合泛型函数或泛型方法 target 直接作为一等 callable 值，例如 `obj.method<T>`；
- 泛型 union 参数的 `match + narrowing`；
- 数组 `fit` 局部类型参数的约束声明；
- 泛型重载优化、variance 和泛型与 C ABI 的新交叉语义；
- 通过 object-form spec 视图取得方法值这一独立分派能力；本专项只允许 object-form spec 作为泛型
  接收者内部保存的 `T`，不借机扩展 spec 方法值来源；
- 对所有接收者形态和所有类型实参做笛卡尔积枚举。

### 3.3 实现约束

- 实现必须由接收者的值语义和描述符事实驱动，不得针对类型名、包名、方法名、测试模型或参数位置特判；
- 固定布局与 reified 布局必须复用统一的值复制、托管槽保留/释放和 cleanup 抽象；
- 不得把值接收者地址借给可能逃逸的方法值，不得保存栈指针；
- 泛型不是装箱边界。无论 `T` 在调用点闭合为标量、tuple 或 `@value type`，都不得创建 object-form
  spec box，不得把值改写为 `{ subject, witness }`，也不得借助装箱绕过动态布局；
- 值接收者本身只有作为 subject 进入需要 subject/witness 的 spec 视角时，才按既有 spec coercion 规则
  装箱，例如赋值给 object-form spec 绑定、作为 object-form spec 参数传递、作为 object-form spec
  返回值或保存到对应字段；
- 方法值进入 callable-form spec 只形成既有 callable closure。值接收者必须作为值 payload 保存在该
  closure 的捕获环境中，不得先把接收者装入 spec box，也不得为接收者另建托管 box；
- 引用类型方法值继续只复制对象引用，不得改变其 closure 表示、调用层数、分配次数或运行时操作；
- 值类型方法值必须拥有独立且可逃逸的接收者存储；该值存储应内联于方法值 closure 的捕获环境，并
  复用既有 value capture、aggregate descriptor 和 cleanup 能力；方法值 closure 本身的既有分配不属于
  接收者装箱，但不得因此增加第二个 receiver box 分配；
- 若通用实现需要新增一次堆分配、额外运行时间接调用、动态查找、runtime API/私有 ABI 变更，必须停止
  实施并提交人工决策；
- 新增的值复制、托管槽 retain/release 仅用于实现既定按值捕获语义，不能以当前不支持该路径作为有效
  性能基线，但仍应避免重复复制和重复读取描述符；
- 泛型二进制分发继续使用既有 type/function descriptor 和 reified dependency slots，不得引入单态化
  作为正确性前提。

## 4 测试组织

- 新增跨包 provider 文件到 `fcts/fcts_lib/src/test/`；
- 新增 consumer 与全部成功行为断言到 `fcts/fcts_bin/src/`，统一使用 `std.test`；
- `fcts/fcts_bin/src/main.ff` 只增加一次新测试入口调用；
- 测试库不使用 `puts` 或其他额外输出；
- 不修改既有测试的语义和断言；compiler regression 只追加独立测试函数和注册入口；
- 编译期失败、Semantic 目标选择和诊断码放入 `test/semantic`；
- 只用于锁定 closure/capture、描述符或生成 C ABI 结构的断言放入 `test/codegen`；
- 用户代码可观察的调用、复制和生命周期行为全部优先放入 FCTS；只有 FCTS 无法稳定观察的进程级
  生命周期才使用 `test/cli`。

测试按运行时表示选取代表实例，不做笛卡尔积：

| 代表实参 | 必须验证的事实 |
| --- | --- |
| `i64` | 标量字段完整复制，形成方法值后与源值独立 |
| `string` 或普通引用 `type` | managed pointer 保留、覆盖和释放正确 |
| descriptor-sized `@value type` | 捕获存储大小、字段偏移和 aggregate cleanup 来自闭合描述符 |
| 泛型 tuple | 嵌套聚合值完整复制，tuple `fit` self 指向捕获存储 |
| object-form spec | subject 与 witness 完整保留并随接收者生命周期释放 |
| callable-form spec | 嵌套闭包引用保持有效并正确释放 |

表中的 object-form spec 指 `T` 在进入泛型接收者以前已经是 spec 视角。复制外层值接收者时只复制该
spec 值已有的 subject/witness，不得把外层 `@value type` 或 tuple 再装箱。

## 5 分组实施

### 5.1 第一组：通用值接收者捕获与基础语义

先实现不依赖开放泛参的通用值接收者方法值路径，覆盖：

- trivial `@value type` 的自身只读方法值；
- aggregate `@value type` 的自身只读方法值；
- `@value type` 的自身修改型方法值；
- `@value type` 的直接 `fit` 方法值；
- 非泛型 tuple 的直接 `fit` 方法值；
- 显式 callable-form spec 局部绑定、直接作为 callable 参数和从函数返回；
- 临时值形成方法值，创建作用域结束后继续调用。

必须断言：

1. 形成方法值时立即复制接收者；
2. 此后修改或重新赋值源变量不影响捕获值；
3. 修改型方法值只修改捕获存储，不修改源变量；
4. 连续调用同一方法值时不重新复制 `self`，上一次修改对下一次调用可见；
5. 从同一源值分别形成的两个方法值拥有独立捕获存储；
6. 复制已经形成的方法值时，继续遵守既有 callable 引用语义，共享同一个方法值 closure。

专项验证：运行 `./build/bin/feng run fcts/fcts_bin`。专项通过后，在沙箱外执行一次完整 `make test`；
通过前不得进入第二组。

### 5.2 第二组：闭合泛型值接收者与方法值 ABI

本组只验证方法值形成表达式位于普通发码点、且接收者泛型实例已经完整闭合的情况；不验证该表达式本身
位于另一个共享泛型 callable 体内，也不涉及跨包二进制分发。目标方法属于泛型 `type` 时仍可执行既有
共享方法体，本组区分的是“方法值在何处形成”，不是把目标方法改为单态化。例如：

```feng
open spec ValueReplacer<T>(next: T): T;

@value
open type MethodValueCell<T> {
  var value: T;

  open func MethodValueCell(value: T) {
    self.value = value;
  }

  open func replace(next: T): T {
    let previous = self.value;
    self.value = next;
    return previous;
  }
}

let cell = MethodValueCell<WideValue>(initial);
let replace: ValueReplacer<WideValue> = cell.replace;
```

`cell.replace` 形成时，调用点已经知道 `T = WideValue` 及 `MethodValueCell<WideValue>` 的闭合 aggregate
descriptor。本组验证编译器直接按该描述符把接收者值复制进 method-value closure，并生成与
`ValueReplacer<WideValue>` 一致的 invoke adapter；不允许装箱，也不依赖共享体在运行时补充类型信息。

在第一组通用实现上增加泛型 `@value type<T>` 自身方法、泛型 `@value type<T>` 的 `fit` 方法及泛型
具名 tuple 的 `fit` 方法。按 §4 的六类代表实参组合覆盖：

- 方法只读返回 `T`；
- 方法修改接收者中的 `T` 字段；
- 方法接受 `T` 并返回修改前或修改后的 `T`；
- 方法参数和返回值使用 tuple 或 `Composite<T>`；
- 方法值保存到字段、经绑定复制、覆盖后再次调用；
- 捕获 object-form/callable-form spec 时完整保持其内部 subject、witness 或 closure。

该组必须确认方法值 adapter 同时服从：

- 接收者由闭合 aggregate descriptor 决定大小、字段偏移和 cleanup；
- 泛型值接收者直接复制进 closure 捕获 payload，不生成或调用任何 spec box；
- callable 参数/返回值按原始 callable 槽位决定 ABI，不按某个替换后 C 类型重新分类；
- invoke 时 `self` 指向方法值保存的当前接收者存储，不产生第二份接收者副本。

专项 FCTS 通过后，在沙箱外执行一次完整 `make test`；通过前不得进入第三组。

### 5.3 第三组：共享泛型体内形成值接收者方法值

现有 FCTS 已经分别覆盖泛型共享体、跨包 `.ft`、Lambda 捕获泛型值、泛型值经过 callable 参数/返回值，
以及普通引用 `type` 的实例方法值。本组不重复这些基础设施矩阵，只补“编译一次的共享泛型体形成
值接收者方法值”这一条尚未覆盖的 lowering 路径。

典型场景为 provider 编译时不知道 `MethodValueCell<T>` 的最终大小，而具体闭合实例只出现在 consumer：

```feng
// fcts_lib
open func bindReplace<T>(
  value: MethodValueCell<T>
): ValueReplacer<T> {
  return value.replace;
}

// fcts_bin
let replace = bindReplace<WideValue>(value);
```

共享体必须使用调用点已经闭合并传入的描述符，把整个值接收者复制到 method-value closure 的内联捕获
payload 中；不得保存调用方栈地址、不得装箱，也不得在运行时搜索或动态具化类型。

另一个非等价入口是泛型 owner 通过 `self.method` 形成方法值：

```feng
open spec ValueProducer<T>(): T;

@value
open type MethodValueReader<T> {
  open var value: T;

  open func read(): T {
    return self.value;
  }

  open func reader(): ValueProducer<T> {
    return self.read;
  }
}
```

这里的接收者布局依赖 owner `T`，必须从 owner descriptor 取得；若泛型 owner 的泛型方法同时使用方法
参数 `U` 构造另一泛型值接收者，则 `T` 与 `U` 必须分别从 owner descriptor 与 function descriptor 取得，
不能混用 slot。

provider 导出测试模型与形成方法值的泛型 API，consumer 只通过 `.ft` 使用。只保留以下非等价入口：

1. imported 顶层泛型函数体内形成 `value.method`，验证 function descriptor domain；
2. imported 泛型 owner 实例方法体内形成 `self.method`，验证 owner descriptor domain；
3. imported 泛型 owner 的泛型方法同时使用 owner `T` 与方法 `U` 形成值接收者方法值，验证两级
   descriptor domain 与稳定 slot；
4. imported 泛型 tuple 或 `@value type` 形成 imported `fit` 方法值，验证 `.ft` 恢复的 fit 目标和方法；
5. consumer 使用 provider 中未预先出现的具体闭合实例，验证二进制分发不依赖 provider 预注册闭合类型。

现有泛型 callable 的静态方法、多层转发及普通跨包组合不在本组重复展开；方法值只需经过一层既有泛型
callable 参数或返回值，确认新 closure 表示可以复用现有转发路径。

必须检查 owner 类型参数依赖继续从 owner descriptor 获取，callable 方法参数依赖继续从 function
descriptor 获取；共享体只读取既有闭合描述符和稳定 slots，不执行运行时搜索、动态具化或泛型装箱。

专项 FCTS 通过后，在沙箱外执行一次完整 `make test`；通过前不得进入第四组。

### 5.4 第四组：生命周期、异常与负向诊断

生命周期成功行为覆盖：

- 捕获含 `string`、普通托管对象、object-form spec 和 callable-form spec 的接收者；
- 创建方法值的函数返回后，捕获值继续有效；
- 方法值正常离开作用域时，全部托管叶子恰好释放一次；
- 方法值被覆盖时，旧捕获值恰好释放一次；
- 多个 callable 引用共享一个方法值 closure 时，最后一个引用释放后才销毁捕获值；
- 异常展开销毁方法值时不提前释放、不泄漏且不重复释放。

Semantic/compiler regression 覆盖：

- 值类型方法签名与目标 callable-form spec 参数不匹配；
- 返回类型不匹配；
- 重载值类型方法缺少 callable 目标类型时保持既有二义性诊断；
- 绑定、参数和返回位置提供明确 callable 目标后选择同一个唯一方法；
- 不可见的 imported 方法不能形成方法值；
- Codegen 回归确认普通闭合调用点和共享泛型体均不引用值类型 spec-box 构造入口，且只分配既有的
  method-value closure；
- Semantic 失败不得落入 Codegen；合法程序不得新增普通 Codegen 错误作为功能分支。

专项 FCTS 与 compiler tests 通过后，在沙箱外执行最终完整 `make test`。

## 6 每组缺陷处理流程

每组严格执行以下步骤：

1. 只增加当前组所需实现与新用例，不提前加入下一组场景；
2. 先运行当前组最小 compiler regression 或 `./build/bin/feng run fcts/fcts_bin`；
3. 发现缺陷后立即停止补下一组，在本文“实施中发现的问题”章节记录最小复现、根因、通用修复规则及
   ABI/性能影响；
4. 修复必须复用普通 type、值模型、callable closure、generic descriptor 和 cleanup 的通用抽象；
5. 禁止针对当前测试类型、包、方法、实参类别或生成符号名增加特判；
6. 如果修复需要增加已有正确路径的运行时开销、额外分配或修改 runtime 私有 ABI，停止并取得人工批准；
7. 缺陷修复、当前组专项用例和沙箱外 `make test` 全部通过后，才开始下一组。

## 7 完成标准

- [x] 人工 Review 并确认本文范围、分组和性能门槛
- [x] 第一组：通用值接收者捕获与基础语义
- [x] 第一组完成后的完整 `make test`
- [x] 第二组：闭合泛型值接收者与方法值 ABI
- [x] 第二组完成后的完整 `make test`
- [ ] 第三组：共享泛型体内形成值接收者方法值
- [ ] 第三组完成后的完整 `make test`
- [ ] 第四组：生命周期、异常与负向诊断
- [ ] 第四组完成后的完整 `make test`
- [ ] 核对 §3.2 的非目标没有被实现或测试变更隐式扩展
- [ ] 核对新增用例覆盖 §4 的全部运行时表示与 §5 的全部非等价路径
- [ ] 核对值类型进入泛型和形成方法值时均未装箱，只有显式进入 object-form spec 视角的既有路径装箱
- [ ] 核对没有类型名、包名、方法名、参数位置或测试模型特判
- [ ] 最终 `make test` 无失败和 sanitizer 报告

## 8 实施中发现的问题

### 8.1 第一组：fit 方法无法形成目标类型明确的方法值

- 最小复现：`let read: Producer = value.fitRead;` 在 `fitRead` 仅由可见 `fit` 提供时报告 `AE0522`；
- 根因：方法调用的重载解析已经同时枚举 type 自身方法和可见 fit 方法，但目标类型驱动的方法值解析只枚举
  type 自身方法；
- 通用修复：方法值解析通过统一的可见 fit 遍历器收集候选，并在匹配前按 owner 实例与 fit 目标替换参数、
  返回类型；候选唯一性仍使用既有 callable 目标类型规则；
- 测试归属：第一组 FCTS 覆盖 `@value type` 与 tuple 的直接 fit 方法值；
- ABI/性能影响：仅增加编译期候选解析；运行时无新增路径或开销；
- 状态：已修复，第一组专项 FCTS（719/719）与完整 `make test` 均通过。

### 8.2 第二组：对象字面量推导丢失闭合 owner 实参

- 最小复现：`let value = Cell<int> { payload: 1 }; let read: Producer<int> = value.read;` 中，对象字面量
  被推导为泛型声明 `Cell<T>`，字段校验与方法值签名匹配均无法把 `T` 替换为 `int`；
- 根因：对象字面量类型推导只返回 target 的类型声明，没有保留 generic target 表达式提供的完整
  `FengTypeRef`；
- 通用修复：对象字面量推导和字段校验均优先使用 target 表达式的完整实例类型，只有非泛型 target 才回退到
  类型声明视角；
- ABI/性能影响：仅修正编译期类型事实；运行时无新增路径或开销；
- 状态：已修复。

### 8.3 第二组：泛型 tuple 的共享 fit 路径错误使用 managed descriptor

- 最小复现：闭合泛型 tuple 形成其泛型 `fit` 方法值时，生成 C 引用了不存在的
  `FengTypeDescriptor`，而该 tuple 实际只拥有 `FengAggregateDescriptor`；
- 根因：部分泛型共享方法、wrapper、fit descriptor 与 lambda reification 路径只检查 `@value` 标记，未复用
  “tuple 或 `@value type` 均为值语义 aggregate”的既有统一判断；
- 通用修复：新增声明级值语义谓词，并让上述泛型 descriptor 路径和 `UserType` 级谓词保持一致；fit function
  descriptor 的符号前缀同样取实际 owner descriptor；
- ABI/性能影响：只修正生成 C 的静态 descriptor 类型与符号；运行时无新增间接层、分配或查找；
- 状态：已修复，第二组专项 FCTS（727/727）与完整 `make test` 均通过。
