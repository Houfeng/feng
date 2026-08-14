# Feng 泛型剩余结构性死角测试补强开发文档

> 状态：待人工 Review，尚未开始实施
>
> 泛型、迭代和 callable 的语言规则分别以
> [feng-generics-draft.md](../specifications/feng-generics-draft.md)、
> [feng-iterator.md](../specifications/feng-iterator.md) 和
> [feng-function.md](../specifications/feng-function.md) 为准。
> 已完成的泛型组合覆盖及 callable 值具化修复见
> [feng-generic-cross-feature-fcts-hardening-dev.md](./feng-generic-cross-feature-fcts-hardening-dev.md) 和
> [feng-callable-value-reification-refactor-dev.md](./feng-callable-value-reification-refactor-dev.md)。
> 本文只规划尚未形成直接证据的结构性测试，不重复定义或改变语言语义。

## 1 目标

现有用例已经覆盖普通 `type`、`@value type`、tuple、array、enum、union、多种 spec、类型级与
方法级泛型、顶层/静态/实例 callable、lambda、方法值、构造与终结、`fit`、`@mixable`、复杂控制流、
生命周期以及 `fcts_lib -> fcts_bin` 的跨包二进制分发。

本轮只补以下仍缺少直接或充分覆盖的独立路径：

1. 泛型元素参与 `for-in` 协议及循环控制流；
2. 多参数泛型 callable 在混合运行时表示下的检查、传递和调用；
3. 多 provider 菱形依赖下，同一泛型描述图的闭合与身份收敛；
4. 最终按编译器实现分支核对测试映射，补齐遗漏的非等价路径。

完成标准是覆盖独立的语义、lowering、运行时表示、所有权和包边界，不对所有类型、容器、参数位置和
嵌套深度做笛卡尔积枚举。

## 2 测试职责与组织

### 2.1 测试职责

- FCTS 验证 Feng 语言的语法、语义和可观察行为；可以引用 `std` 作为测试夹具，但不以标准库 API
  自身的正确性为测试目标；
- `std/std_test` 验证标准库逻辑，不承担编译器泛型语义的主要覆盖；
- 必然失败的声明、约束和类型检查放入 `test/semantic` 等 compiler tests；
- 仅能通过生成结构稳定验证的 ABI、描述符槽位和零装箱要求放入 `test/codegen`；
- 需要临时构造多个包或进程级环境的场景放入 `test/cli`。

### 2.2 文件组织

- 成功行为优先在 `fcts/fcts_bin` 新增独立测试文件，并使用 `std.test` 的 `describe`、`test` 和
  `assert`；
- 跨包公开模型只在需要时新增到 `fcts/fcts_lib`，库中不输出额外内容；
- `fcts/fcts_bin/src/main.ff` 只登记新增测试入口，不改变已有入口和断言；
- 多 provider 菱形依赖无法由现有 `fcts_lib -> fcts_bin` 两包结构完整表达，使用 `test/cli` 创建独立
  的临时多包工程；
- 不修改已有用例的语义或断言。实施时若必须调整已有测试文件，需先取得人工批准。

## 3 第一组：泛型 `for-in`

### 3.1 覆盖模型

分别覆盖迭代规范中的独立路径：

1. 元素依赖 `T` 的只读数组和可写数组内建迭代；
2. 泛型 `@iterable` source 返回泛型 cursor，cursor 的 `@iterator` 返回依赖 `T` 的具名 tuple；
3. 泛型 source 自身实现 `@iterator` 的 self-cursor；
4. 若 `fit` 提供的 `@iterable` / `@iterator` 会经过不同语义或 codegen 路径，增加泛型 `fit`
   代表用例；若与现有路径完全等价，只在最终分支核对中记录映射，不重复添加。

元素表示至少选择以下代表类型：

- 标量或其他直接值；
- 普通托管引用；
- descriptor-sized `@value type`；
- object-form spec；
- callable-form spec；
- union-form spec。

数组、cursor 和全部元素表示不做笛卡尔积。每条 lowering 路径覆盖基本行为，再选择一个含托管叶子的
descriptor-sized 模型覆盖复杂控制流和生命周期。

### 3.2 行为与结构断言

- source 表达式只求值一次；
- 循环变量取得完整值，泛型 tuple 的 `bool` 与元素槽位顺序正确；
- 正常完成、`break`、`continue`、嵌套循环和函数内提前 `return`；
- 异常离开循环时，source、cursor、当前元素及托管叶子无遗漏释放或重复释放；
- imported provider 共享体与 consumer-only 闭合类型组合能正确工作；
- 值类型进入泛型迭代路径不装箱，只有进入 spec 视角时才按既有规则装箱；
- codegen 结构验证 iterator 协议不引入规范外的元素副本、额外堆分配或运行时间接层。

这里引用 `List<T>` 或其他标准库类型时，只验证泛型 `for-in` 的语言行为，不验证容器 API 本身。

### 3.3 验收

1. 运行 `./build/bin/feng run fcts/fcts_bin`；
2. 运行新增的 compiler 专项测试；
3. 在沙箱外执行一次 `make test`；
4. 全部通过后才进入第二组。

## 4 第二组：多参数泛型 callable

### 4.1 覆盖模型

使用独立 callable-form spec 表达多个参数，例如：

```feng
spec Transform<T, U, V, R>(first: T, second: U, third: V): R;
```

同一个 callable 签名同时包含：

- 直接值、托管引用和 descriptor-sized 聚合；
- 互换顺序或重复出现的 `T`、`U`、`V`；
- 同时依赖多个泛参的 tuple 或 `@value type` 返回值；
- owner 类型参数 `T` 与方法参数 `U` 同时进入不同参数位置和返回值；
- callable、spec 或 union 作为另一个参数或类型实参的代表性递归组合。

callable 来源覆盖现有语言已经支持的非等价路径：

1. 顶层函数；
2. lambda；
3. 绑定普通引用 receiver 的实例方法；
4. 绑定 `@value type` receiver 的实例方法；
5. 泛型 `fit` 方法。

同时覆盖本地声明、跨包 provider 共享体以及 consumer-only 闭合类型。不得为某一种 callable 来源、
某个参数序号或当前测试类型增加实现特判。

### 4.2 行为、负向与结构断言

成功行为覆盖：

- 目标 callable-form spec 赋值、显式转换、参数传入、函数返回和字段保存；
- 形成函数值后调用，所有参数与返回值保持完整；
- callable 值的复制、覆盖、异常清理和 receiver 生命周期；
- 参数与返回值中的值类型保持值语义且不装箱；
- 显式泛型函数值闭合继续遵守现有 callable 规范，不新增其他闭合或推导语法。

负向用例覆盖：

- 参数数量不匹配；
- 第一个、中间和最后一个参数分别不匹配；
- 返回类型不匹配；
- 泛型实参数量错误或实参不满足约束。

codegen 结构验证：

- 参数顺序和泛型描述符 slot 映射正确；
- direct、managed、descriptor-sized 参数使用既有 ABI 分类；
- 值类型不装箱；
- 不增加额外间接调用、分配或运行时描述符查找。

### 4.3 验收

1. 运行 `./build/bin/feng run fcts/fcts_bin`；
2. 运行新增的 semantic/codegen 专项测试；
3. 在沙箱外执行一次 `make test`；
4. 全部通过后才进入第三组。

## 5 第三组：多 provider 菱形依赖

### 5.1 包结构

在 `test/cli` 的独立临时工程中构造：

```text
common-generic
├── provider-a ──┐
└── provider-b ──┴── consumer
```

`provider-a` 与 `provider-b` 都引用 `common-generic` 导出的开放泛型 type/spec/callable 图；consumer
同时导入两个 provider，并使用仅在 consumer 中声明的具体类型完成闭合。

### 5.2 验证内容

- 两条导入路径恢复出的同一开放泛型声明具有一致身份；
- type、aggregate、callable、object-form spec 及 witness 的闭合描述和 slot 映射收敛；
- 两个 provider 能互相传递、返回或组合相同的闭合泛型值；
- consumer-only 闭合实例无重复符号、错误描述符绑定或链接冲突；
- 若构建系统允许稳定控制依赖顺序，以相反 provider 顺序构建两个 consumer 变体，结果保持一致；
- 运行行为验证完整值和身份相关语义，不只检查生成文本。

本组验证 Feng 的包边界和泛型二进制分发，不验证所引用标准库 API 的实现逻辑。

### 5.3 验收

1. 运行新增的 CLI 专项用例；
2. 在沙箱外执行一次 `make test`；
3. 全部通过后才进入最终核对。

## 6 最终实现分支核对

前三组完成后，对 Semantic、FT 恢复、Codegen 和泛型描述符构建中的非等价实现分支做一次静态核对，
建立“实现分支 -> 直接测试”的映射。核对至少包含：

- 类型级、方法级及二者组合的泛参来源；
- direct、managed、descriptor-sized、spec、callable 和 union 表示；
- 参数、返回值、局部值、字段和 callable capture；
- 本地、单 provider 跨包和多 provider 菱形依赖；
- 正常退出、提前退出、异常退出和生命周期清理。

只有发现未被现有或本轮用例直接覆盖、且行为并非等价复用的分支时，才增加最小用例。不能以构造
更多类型排列或加深嵌套层数代替结构性核对。

## 7 `extern` 边界

- 泛型不进入 C ABI，`@abi extern` 函数不能使用泛型；本轮不新增其成功行为用例，也不扩展该规则；
- `@runtime extern` 可按现有能力由编译器传入描述符，并由 runtime C 实现处理部分泛型场景；
- 本轮保持现有 `@runtime extern` 覆盖，不刻意增加组合矩阵；最终分支核对若发现已有合法路径完全没有
  回归证据，只记录事实并提交人工决策，不自行扩大 runtime 私有 ABI 或运行时实现。

## 8 缺陷处理约束

每组严格执行：

1. 先确认新增用例与现有覆盖不等价；
2. 一次只增加当前组需要的模型和断言；
3. 发现问题后停止下一组，先记录最小复现、根因、通用修复规则以及 ABI/性能影响；
4. 修复必须复用现有 descriptor、reified slots、witness、callable ABI、aggregate 和 cleanup 抽象；
5. 严禁针对测试类型、标准库容器、参数位置、包名或某种错误文本增加特判；
6. 若修复需要新增或改变 runtime 私有 ABI、增加 Feng 程序运行时操作、间接层或分配，必须先取得
   人工批准；
7. 当前组的 FCTS/compiler/CLI 专项及 `make test` 全部通过后，才进入下一组。

## 9 非目标

- 标准库 API 的专项正确性；
- 为每个 `Tuple`、`Func`、`Union` arity 重复同一语言路径；
- 泛型重载优化、variance 或数组 fit 约束等尚未实施的能力；
- 泛型进入 C ABI；
- 扩大 `@runtime extern` 的泛型能力；
- 任意嵌套深度及全部类型组合的穷举测试。

## 10 实施步骤

- [ ] 人工 Review 本文范围、分组和测试归属
- [ ] 实施前确认对 `fcts/fcts_bin/src/main.ff` 新增测试入口的批准
- [ ] 第一组：补齐泛型 `for-in` 用例
- [ ] 第一组专项测试及全量 `make test`
- [ ] 第二组：补齐多参数泛型 callable 用例
- [ ] 第二组专项测试及全量 `make test`
- [ ] 实施前确认对现有 `test/cli` 测试文件新增用例的批准
- [ ] 第三组：补齐多 provider 菱形依赖用例
- [ ] 第三组专项测试及全量 `make test`
- [ ] 完成最终实现分支与直接测试映射核对
- [ ] 仅对核对发现的非等价空白补充最小用例
- [ ] 最终专项测试及全量 `make test`
- [ ] 所有项完成后将本文状态更新为“已完成”

## 11 实施问题跟踪

本节用于记录各组补测过程中实际发现的问题，当前暂无记录。每组补齐用例后必须先执行该组专项测试，
再在沙箱外执行全量 `make test`。如果任一测试发现问题，必须立即暂停当前组的后续工作以及下一组实施，
并在本节记录问题、完成通用修复和相应回归；该组专项测试与全量 `make test` 全部通过后才能继续。

每个问题按以下结构记录：

### 11.x 问题标题

- 所属分组与状态；
- 最小复现和失败现象；
- 根因；
- 通用修复规则；
- runtime ABI 与 Feng 程序运行时性能影响；
- 新增回归用例；
- 专项测试与全量 `make test` 结果。
