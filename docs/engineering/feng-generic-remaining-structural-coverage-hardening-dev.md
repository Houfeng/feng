# Feng 泛型剩余结构性死角测试补强开发文档

> 状态：已完成
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

- [x] 人工 Review 本文范围、分组和测试归属
- [x] 实施前确认对 `fcts/fcts_bin/src/main.ff` 新增测试入口的批准
- [x] 第一组：补齐泛型 `for-in` 用例
- [x] 第一组专项测试及全量 `make test`
- [x] 第二组：补齐多参数泛型 callable 用例
- [x] 第二组专项测试及全量 `make test`
- [x] 实施前确认对现有 `test/cli` 测试文件新增用例的批准
- [x] 第三组：补齐多 provider 菱形依赖用例
- [x] 第三组专项测试及全量 `make test`
- [x] 完成最终实现分支与直接测试映射核对
- [x] 仅对核对发现的非等价空白补充最小用例
- [x] 最终专项测试及全量 `make test`
- [x] 所有项完成后将本文状态更新为“已完成”

## 11 实施问题跟踪

本节用于记录各组补测过程中实际发现的问题。每组补齐用例后必须先执行该组专项测试，
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

### 11.1 泛型循环的 `continue` 跳转跨越 descriptor-sized VLA

- 所属分组与状态：第一组；通用修复及全量回归均已完成；
- 明确结论：`for-in + IteratorResult<T>(bool, T)` 在本轮修复前后都是合法且可用的；
  本问题只由循环体中的 `continue` 跳转跨越 descriptor-sized 局部槽作用域触发，
  不是泛型 iterator result 本身不受支持；
- 已确认能力边界：`for-in` 配合 `IteratorResult<T>(bool, T)` 形式的泛型迭代结果在语义上合法；新增
  用例中的 `LibGenericForInResult<T>(bool, T)` 也已通过 Semantic 和 provider 发码。本问题不是该
  泛型结果类型不可用；其 descriptor-sized 闭合布局的独立 codegen 缺口记录在 §11.4；
- 最小复现和失败现象：imported 泛型共享体在 `for-in` 循环中先执行 `continue`，随后存在对 `T` 的
  赋值；生成 C 的 `goto _cont_N` 会跨过 descriptor-sized 临时值的 VLA 声明，Clang 报
  `cannot jump from this goto statement to its label`；
- 根因：三段式 `for`、数组 `for-in` 和 iterator 协议 `for-in` 的 continue label 与循环体声明位于
  同一个 C 词法块。`continue` 使用 `goto` 跳到更新位置时，可能从 VLA 声明之前进入其作用域；
- 通用修复规则：三个使用 continue label 的循环 lowering 都为每轮 binding、临时值及用户 body
  建立独立 C 词法块，并把 continue label 放到该块之外；跳转只离开声明作用域，不进入任何 VLA
  作用域；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI，不增加 Feng 运行时操作、分配或
  间接层；只调整生成 C 的词法块结构；
- 新增回归用例：codegen 用例同时覆盖三段式 `for`、泛型数组 `for-in` 和泛型 iterator `for-in`
  在 `continue` 后存在 descriptor-sized 泛型存储的场景，并要求生成 C 可由主机 C 编译器通过；
- 专项测试与全量 `make test` 结果：`./build/bin/test_codegen`、第一组 FCTS 专项及全量
  `make test` 均已通过。

### 11.2 consumer-only 闭合泛型实参未完成 codegen 实例登记

- 所属分组与状态：第一组；通用修复及全量回归均已完成；
- 最小复现和失败现象：consumer 将自身声明的 `GenericForInWide<T>` 闭合后，作为 imported 泛型
  iterator 类型或共享函数的类型实参，consumer codegen 报
  `CE0031: generic type/spec instance 'GenericForInWide<...>' was not registered`；
- 已确认边界：`LibGenericForInResult<T>(bool, T)` 本身合法，provider 已成功完成 Semantic 和发码；
  本问题不由泛型 iterator 结果 tuple 引起；
- 根因：imported 泛型成员或 reified dependency 把 provider 类型参数替换为 consumer 类型引用后，
  原实现将替换后的整棵类型树统一放回 provider 声明程序解析，丢失“外层泛型声明来自 provider、
  被替换实参来自 consumer”的逐层来源信息，因而没有登记 consumer-only 嵌套闭合实例；selected
  callable surface 的最终解析也存在相同的整树来源折叠；
- 通用修复规则：实例预收集按“声明侧 source type ref + 替换后 effective type ref”成对递归；非泛参
  节点以声明来源解析泛型根，直接替换的泛参子树保持调用侧来源，并由调用点预收集。该规则统一用于
  字段、callable 参数与返回值、reifiable dependency、spec parent/union/intersection 以及 selected
  callable surface，不按包名或测试类型特判；
- runtime ABI 与 Feng 程序运行时性能影响：仅改变编译期实例登记与类型解析，不改变 runtime ABI、
  生成值表示、Feng 程序运行时操作或间接层；
- 新增回归用例：FCTS 由 `fcts_lib` 导出泛型 iterator/shared body，`fcts_bin` 以 consumer-only 的
  `GenericForInWide<T>` 形成嵌套闭合实例并验证完整字段和托管值；
- 专项测试与全量 `make test` 结果：`./build/bin/feng run fcts/fcts_bin` 及全量 `make test` 均已通过。

### 11.3 泛型 `for-in` 新增组运行时异常退出

- 所属分组与状态：第一组；通用修复及全量回归均已完成；
- 最小复现和失败现象：§11.2 的 codegen 登记错误修复后，FCTS 能进入新增的
  `generic for-in coverage` 分组，但 imported 共享体遍历 `T[]` 且 `T = i64` 时，将数组元素值 `1`
  当作地址 `0x1` 读取，因 `SIGSEGV` 退出；
- 根因：数组 `for-in` 把 `CG_TYPE_GENERIC_PARAM` 沿固定 trivial 元素路径发码为
  `((void **)feng_array_data(...))[index]`；共享体中的 `T` 实际是 descriptor-sized 地址表示，不能按
  `void *` 元素值读取；
- 通用修复规则：数组元素为直接泛参时，从活动泛参描述符取得闭合元素大小，并把
  `array_data + index * element_size` 作为借用元素地址；后续复制、保留和释放继续统一交给现有泛型
  值分派；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI；固定类型和已闭合数组的发码完全
  不变。开放 `T[]` 每次遍历只读取一次 `descriptor->size`，每轮仅进行必要的地址偏移，不增加装箱、
  堆分配或间接调用；
- 新增回归用例：codegen 用例断言元素大小在循环外缓存且循环内按字节偏移取址；FCTS 同时以标量
  和含托管字段的宽 `@value` 实参验证只读/可写数组完整值；
- 专项测试与全量 `make test` 结果：`./build/bin/test_codegen`、第一组 FCTS 专项及全量
  `make test` 均已通过。

### 11.4 泛型 `for-in` 控制流破坏 cleanup frame 顺序

- 所属分组与状态：第一组；通用修复及全量回归均已完成；
- 最小复现和失败现象：修复 §11.3 的泛型数组元素取址后，新增分组的首项“只读/可写泛型数组”
  已通过；执行其后的控制流用例时，runtime 报
  `feng_frame_pop: top cleanup node is not a frame marker` 并异常退出；
- 已确认边界：泛型数组的 descriptor-sized 元素已能按地址正确读取，本问题发生在后续清理链恢复
  阶段，与 `IteratorResult<T>(bool, T)` 的合法性无关；
- 根因：问题并非缺少 `cleanup_pop`。iterator lowering 在 imported 泛型共享体中仍以固定 C `struct`
  声明 `source.iter()` 返回的开放泛型 `@value` cursor，以及 `cursor.next()` 返回的开放泛型 result；
  当闭合 `T` 是 descriptor-sized 宽 `@value` 时，被调用共享体按闭合 aggregate descriptor 的真实
  大小写回，越界覆盖相邻 cleanup node 和 function frame marker，最终在早返回路径的
  `feng_frame_pop` 处暴露；
- 已确认能力边界：`for-in + IteratorResult<T>(bool, T)` 的协议和类型声明本身合法，固定表示或较小
  实参场景在修复前也可工作；缺口是共享体中 cursor 或 iterator result 的闭合布局依赖 `T` 时，
  合成返回槽没有遵循现有 descriptor-sized aggregate 规则；
- 通用修复规则：按现有 reified aggregate 约定，为协议 lowering 合成的 cursor 和 result 返回槽统一
  区分固定布局与 descriptor-sized 布局；后者使用闭合描述符的 `size`、`reified_field_offsets` 和
  aggregate cleanup。元素为嵌套 descriptor-sized aggregate 时也按同一规则复制到闭合大小的局部
  槽，不针对某一种 tuple 或测试类型硬编码；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI，不增加装箱、堆分配或间接调用；
  固定布局路径发码不变。此前不能正确运行的动态布局路径，对 cursor 与 result 各读取一次闭合
  descriptor/size，并在循环中使用已缓存的 result metadata；这是正确分配和访问闭合值所需的最小
  成本；
- 新增回归用例：codegen 用例断言 cursor/result 均生成 descriptor-sized 槽并通过 runtime field
  offsets 访问；FCTS 以 consumer-only 宽 `@value` 元素覆盖正常完成、`continue`、`break`、函数内
  `return`、嵌套遍历及异常清理；
- 专项测试与全量 `make test` 结果：`./build/bin/test_codegen`、第一组 8 项 FCTS 及全量
  `make test` 均已通过。

### 11.5 iterator result 的存储布局与清理描述符判定被错误合并

- 所属分组与状态：第一组；§11.4 修复过程中引入的回归及全量回归均已完成；
- 最小复现和失败现象：`std.Event<T>.emit()` 遍历 `List<Action<T>>` 时，result 的 C 物理大小固定，
  但其托管字段仍依赖闭合 aggregate descriptor；修复草稿错误地沿静态 descriptor 分支引用不存在的
  开放符号 `IteratorResult<Action<T>>__aggregate_desc`，最终链接失败；
- 根因：把“物理存储大小是否依赖开放泛参”和“aggregate 生命周期是否需要闭合 descriptor”合并为
  同一个布尔判定。前者只决定是否使用 descriptor-sized storage，后者还覆盖固定大小但闭合托管槽
  信息依赖泛参的 aggregate；
- 通用修复规则：恢复两层正交判定：`uses_reified_storage` 只控制槽大小及 runtime field offsets；
  `needs_reified_layout` 独立控制 aggregate cleanup descriptor。固定大小路径不得引用开放实例的静态
  descriptor；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI；恢复固定大小路径原有的闭合
  descriptor cleanup，不增加 Feng 程序运行时操作、查表、分配或间接层；
- 新增回归用例：`test_generic_iterator_fixed_storage_reified_cleanup_codegen` 以最小化的
  `Result<T>(bool, Action<T>)` 直接断言 result 保持固定 C 存储，同时从当前闭合描述符的
  `reified_agg_deps` 槽取得 aggregate cleanup descriptor，并且不引用开放泛型实例的静态
  descriptor；现有 `std.Event<T>.emit()` 继续提供完整标准库路径的行为与链接回归；
- 专项测试与全量 `make test` 结果：`./build/bin/test_codegen`、包含 `std.Event<T>` 的
  `std-tests` 及全量 `make test` 均已通过。

### 11.6 泛型 callable 闭合实例缺少共享体名义指针桥接

- 所属分组与状态：第二组；通用修复及全量回归均已完成；
- 最小复现和失败现象：泛型共享函数接收 `Multi<T, U, V>` callable 值，调用点传入闭合的
  `Multi<i64, string, Wide<string>>`。两者均为单个闭包指针，但生成 C 分别使用开放实例与闭合实例的
  不同 `struct *` 类型；主机 C 编译器在调用共享体时报告 `incompatible pointer types`，启用
  `-Werror` 后编译失败；
- 根因：共享体调用边界已有的零成本名义托管指针桥接只识别同一泛型来源的普通引用 type，没有覆盖
  同样具有固定单指针表示的 callable-form spec；
- 通用修复规则：将该桥接统一为“同一泛型声明来源的名义托管指针”规则，同时覆盖普通引用 type 与
  callable-form spec；仅在开放实例与闭合实例的 C `struct *` 名义类型不同时，转换为共享声明要求的
  指针类型。不得改变 callable 参数 ABI、值所有权或调用分发；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI，不增加 ARC、装箱、分配、描述符读取
  或间接调用；新增内容仅为生成 C 中的表示保持型指针转换，不产生增量 Feng 运行时开销；
- 新增回归用例：codegen 用例以 `U, T, V, U` 参数顺序同时覆盖 managed、direct、
  descriptor-sized 表示，要求闭合 callable 传入共享体后生成 C 能在 `-Werror` 下编译，并断言参数
  descriptor 映射、单次 callable 分发、结果 slot 及零装箱；FCTS 覆盖相同参数图的跨包运行行为；
- 专项测试与全量 `make test` 结果：新增 semantic 负向用例、codegen 结构用例及 6 项 FCTS 均已通过；
  全量 `make test` 的 UBSan 与 normal 阶段、91 项 smoke、768 项 FCTS、标准库、性能约束、增量构建及
  发布/安装脚本均已通过。

### 11.7 imported 开放泛型 type 的共享体缺少完整 C 布局

- 所属分组与状态：第三组；通用修复、专项验证及全量回归均已完成；
- 最小复现和失败现象：`common` 导出含字段的 `Node<T>`，`provider-b` 的共享函数接收 imported
  `Node<T>` 并访问其字段。`provider-b` 自身发码时，生成 C 只有
  `struct Node__G__T__CTX__T;` 前置声明，却在共享体中解引用 `node.mapper`、`node.box` 等字段，主机
  C 编译器因此报告 `incomplete definition of type`；失败发生在 provider 构建阶段，尚未进入
  consumer-only 闭合或菱形依赖合并；
- 已确认能力边界：初始失败发生在单个 provider 自身的共享体发码阶段，早于菱形身份合并；修复后，
  同一开放泛型声明经两条 provider 路径恢复、consumer-only 闭合及相反依赖顺序均已通过专项验证；
- 根因：普通成员读取已经在开放泛型共享体中通过闭合 type descriptor 的
  `reified_field_offsets` 访问字段；但“callable 字段被立即调用”的分支重复手写了
  `receiver->field` / `receiver.field`，绕过同一成员读取规则。当前测试中的 callable 字段位于
  descriptor-sized 字段之后，因此该表达式不仅要求不存在的完整开放 C struct，也无法表达闭合实例
  的真实偏移；
- 通用修复规则：抽取“从已求值 user-type receiver 借用字段”的统一 lowering，普通成员读取和
  callable 字段立即调用共同复用；引用 type 和 `@value type` 均保留固定布局直接访问，并在已有
  reified storage/layout 条件成立时统一通过闭合 descriptor 的字段偏移访问。receiver 只求值一次，
  不按字段类型、位置、包来源或调用形式分叉布局规则；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI、descriptor 结构、值表示或所有权；
  固定布局生成路径仍是原有直接字段访问，不增加 Feng 运行时操作。此前错误的 reified 布局路径改为
  与普通成员读取相同的 descriptor 偏移，这是访问闭合字段所必需的既有成本；
- 新增回归用例：四包 CLI 工程由 `common` 导出泛型引用 type、descriptor-sized `@value`、callable
  spec 与 object spec；两个 provider 分别构造、调用、转发并通过 witness 读取相同闭合图，consumer
  仅以自身宽值类型闭合。另构建依赖顺序相反的第二个 consumer，两个可执行文件都必须输出
  `diamond-ok`；
- 专项测试与全量 `make test` 结果：`./build/bin/test_codegen` 与沙箱外 `./build/bin/test_cli` 均已
  通过，两个 consumer 变体均已成功构建和运行；全量 `make test` 的 UBSan 与 normal 阶段、91 项
  smoke、510 项标准库测试、768 项 FCTS、性能约束、增量构建及发布/安装脚本均已通过。

### 11.8 object-form spec 的 callable 字段被错当作 spec 方法调用

- 所属分组与状态：最终实现分支核对；通用修复、专项验证及全量回归均已完成；
- 最小复现和失败现象：

  ```feng
  spec Mapper<T>(value: T): T;

  spec HasMapper<T> {
    let mapper: Mapper<T>;
  }

  func apply<T>(holder: HasMapper<T>, value: T): T {
    return holder.mapper(value);
  }
  ```

  Semantic 已正确接受该声明；Codegen 却未生成 getter 和 callable invoke，只保留共享体序言，
  同时误将内部发码失败当作成功返回，最终由主机 C 编译器报函数体缺少结束 `}`；
- 根因：`cg_emit_call` 对 object-form spec 值和受 object-form spec 约束的直接泛参只接受
  `USM_KIND_METHOD`。Semantic 对 callable 字段调用不会伪造方法 target，因此该路径应先通过
  witness getter 取得字段值，再按 callable-form spec ABI 调用，但原实现没有进入字段读取分支；
- 错误传播根因：`cg_emit_generic_function` 只在完整函数体发码后才声明并初始化
  `ok = true`，而失败分支会从更早位置直接跳到共用清理 label，最终返回未初始化的 `ok`；
  因此这是泛型顶层函数所有 codegen 失败共享的通用传播缺陷，不是 callable 字段的特例；
- 嵌套实例登记根因：普通 `HasMapper<T>` spec 参数的修复路径已能正确发码；进一步的
  `func f<T, H: HasMapper<T>>(value: H)` 仍会报 `Mapper<T>` 实例未登记。泛型 type 实例已会在
  owner 泛参替换后递归收集字段及方法签名；泛型 spec 实例却只处理父 spec、union 和
  intersection 成员，没有以实例点的完整活动作用域递归登记 object/callable spec 签名；
- 通用修复规则：抽取“从已求值 object-form spec receiver 借用字段”的统一 lowering，
  普通 spec 字段读取和 callable 字段立即调用共同复用；同时覆盖 spec 值和受 spec
  约束的直接泛参，receiver 只求值一次。普通 spec 方法继续走原 witness method slot，不改变分发语义；
- 错误传播修复规则：泛型顶层函数的共用清理路径以失败为默认值，仅在函数体、
  fallthrough cleanup 和结尾全部成功发码后显式置为成功；任一 `cg_fail` 或子 lowering 失败都必须
  稳定向上返回 `false`，不得输出部分 C 后继续主机编译；
- 嵌套实例登记修复规则：泛型 spec 实例与泛型 type 实例保持同一收集模型；object-form
  的字段、方法约束/参数/返回值，以及 callable-form 的参数/返回值，都要按“声明侧原类型 +
  owner 泛参替换后类型 + 实例点完整活动作用域”递归登记；这里复用现有通用收集函数，
  不按 spec form 中的具体类型、泛参名或约束位置特判；
- runtime ABI 与 Feng 程序运行时性能影响：不改变 runtime ABI、witness 结构、callable ABI 或值表示；
  新路径只执行语义必需的一次现有 witness getter 和一次 callable invoke，不增加装箱、分配、
  额外描述符查找或新间接层；方法调用及已正常工作的字段读取发码不变。错误传播修复也只改变
  编译器内部布尔状态，不会进入生成的 Feng 程序；
- 新增回归用例：codegen 成功用例在同一共享体中分别验证泛型 object-form spec 值
  与受该 spec 约束的直接泛参，两者均能通过 callable 字段传递 descriptor-sized 实参和返回值；
  另以泛型函数体中的既有不支持 pointer pointee 为稳定失败源，直接断言内部 codegen 错误返回
  `false` 且保留原诊断，不允许部分 C 被当作成功结果；
- 专项测试与全量 `make test` 结果：`test_generic_object_spec_callable_field_call_codegen`、
  `test_generic_function_codegen_failure_propagates` 及完整 `./build/bin/test_codegen` 已通过；
  `./build/bin/test_cli` 与 768 项 FCTS 也已通过；最终全量 `make test` 的 UBSan 与 normal
  `-Werror` 阶段、91 项 smoke、510 项标准库测试、性能约束、增量构建及发布/安装脚本均已通过。

## 12 最终实现分支与直接测试映射

本节只记录实现分支与现有直接证据，不重复定义泛型、callable、spec 或迭代语义。

### 12.1 Semantic 依赖收集与类型检查

| 非等价实现分支 | 直接测试证据 |
| --- | --- |
| 类型级泛参、方法级泛参及二者组合 | `test_generic_type_decl_ok`、`test_generic_function_two_type_params_ok`、`test_generic_method_rejects_owner_and_method_type_argument_mismatch`、`test_generic_shared_method_descriptor_order_codegen` |
| 泛参约束、实参个数及 callable 签名检查 | `test_generic_type_param_constraint_must_be_spec`、`test_generic_explicit_type_args_arity_mismatch`、`test_multi_parameter_generic_callable_rejects_each_mismatch`、`test_explicit_generic_callable_values_reject_invalid_sources` |
| object-form、callable-form 和 union-form spec | `test_spec_witness_via_generic_instantiation`、`test_generic_callable_spec_instance_adapts_untyped_callable_values`、`test_generic_union_form_accepts_concrete_member_matching`、`test_generic_union_form_rejects_mismatched_member` |
| 构造、终结、`fit` 与 `@mixable` 中的 owner dependency domain | `test_generic_type_with_finalizer_allowed`、`test_finalizer_method_type_params_rejected`、`test_member_mix_generic_field_type_is_substituted`，以及 `test_generic_type_owner_reification.ff`、`test_generic_fit_subject.ff`、`test_generic_mixable_reification.ff` |

### 12.2 FT 导出与 imported 恢复

| 非等价实现分支 | 直接测试证据 |
| --- | --- |
| 顶层泛型 callable 的泛参、签名与 reifiable dependencies | `test_generic_function_ft_roundtrip`、`test_callable_value_dependency_ft_roundtrip` |
| 泛型 type 的字段、推断字段、owner dependency 与成员 callable | `test_generic_type_ft_roundtrip`、`test_inferred_generic_field_ft_roundtrip`，以及跨包 `test_generic_type_owner_reification.ff` |
| 泛型 `fit`、object/callable spec 及其嵌套返回类型 | `test_generic_fit_ft_roundtrip`、`test_generic_fit_named_generic_return_ft_roundtrip`、`test_callable_value_dependency_ft_roundtrip` |
| 单 provider 与多 provider 路径中的开放声明身份和 consumer-only 闭合 | FCTS 的 `fcts_lib -> fcts_bin` 全部新增泛型组，以及 `test_project_build_closes_multi_provider_generic_diamond` 的相反依赖顺序变体 |

### 12.3 Codegen 值表示、描述符与位置

| 非等价实现分支 | 直接测试证据 |
| --- | --- |
| direct、managed pointer 与 descriptor-sized 值的入参、返回和局部槽 | `test_generic_direct_result_uses_descriptor_sized_storage_codegen`、`test_generic_managed_return_let_binding_codegen`、`test_multi_parameter_generic_callable_abi_codegen` |
| 固定布局与 reified 布局的字段、构造和默认零值 | `test_non_generic_default_zero_stays_direct_codegen`、`test_closed_generic_default_zero_stays_direct_codegen`、`test_generic_value_construction_uses_reified_storage_codegen`、`test_generic_type_owner_reification_codegen` |
| object-form witness、callable 字段/closure 与 union payload | `test_generic_spec_arg_codegen`、`test_generic_object_spec_coercion_codegen`、`test_generic_object_spec_callable_field_call_codegen`、`test_generic_callable_value_reification_codegen`、`test_generic_union_reified_storage_and_array_source_codegen` |
| 顶层函数、引用 receiver、值 receiver、lambda 和 `fit` callable | `test_generic_callable_value_reification_codegen`、`test_unbound_callable_explicit_cast_codegen`、`test_generic_lambda_dynamic_capture_codegen`，以及 `test_generic_callable_value_reification.ff` |
| 参数、返回值、局部值、字段及 capture 五种承载位置 | `test_multi_parameter_generic_callable_abi_codegen`、`test_generic_aggregate_return_codegen`、`test_generic_type_inferred_field_codegen`、`test_generic_lambda_dynamic_capture_codegen`，以及 `test_generic_multi_parameter_callable_coverage.ff` |

### 12.4 包边界、控制流与生命周期

| 非等价实现分支 | 直接测试证据 |
| --- | --- |
| 本地、单 provider 和多 provider 菱形闭合 | compiler 本地用例、FCTS 的 `fcts_lib -> fcts_bin` 用例、`test_project_build_closes_multi_provider_generic_diamond` |
| 普通完成、`break`、`continue`、嵌套循环和提前 `return` | `test_generic_for_in_coverage.ff`、`test_generic_loop_reified_storage_codegen` |
| `try/catch`、异常退栈、callable 复制/覆盖和 receiver 清理 | `test_generic_try_body_reified_storage_codegen`、`test_generic_multi_parameter_callable_coverage.ff`、`test_generic_advanced_composition_coverage.ff` |
| 泛型循环回收与终结器闭环 | `test_project_run_collects_cross_package_generic_cycle`、`test_project_run_collects_generic_finalizer_cycle` |

### 12.5 核对发现并已补齐的直接证据空白

核对共发现两个非等价空白，均已用最小直接用例补齐。

第一项：第三组已覆盖 imported 开放泛型引用 type 的 descriptor-offset 字段读取与 callable 字段
立即调用，但尚未直接覆盖同一通用 lowering 的 descriptor-sized `@value type` receiver 分支。
两者的存储权限、receiver 取址和字段偏移取值路径不等价，因此在多 provider CLI 工程中增加：

```feng
@value
type DiamondValueNode<T> {
  let box: DiamondBox<T>;
  let mapper: DiamondMapper<T>;
}

func applyValue<T>(node: DiamondValueNode<T>): T {
  return node.mapper(node.box.value);
}
```

该用例由 provider 发码开放共享体，由 consumer-only descriptor-sized 值类型完成闭合；两个
依赖顺序相反的 consumer 均已构建并运行通过。

第二项：object-form spec 的 callable 字段调用不等价于普通 spec 方法调用，且 generic spec
实例登记原先没有递归收集 object-form 成员和 callable-form 签名。新增
`test_generic_object_spec_callable_field_call_codegen`，同时覆盖普通 `HasMapper<T>` spec 值、
`H: HasMapper<T>` 直接约束泛参，以及 callable 参数/返回值中的 descriptor-sized `Payload<T>`；
另由 `test_generic_function_codegen_failure_propagates` 直接覆盖核对时发现的泛型函数发码错误传播。

以上实施只复用既有 field offset、witness、callable ABI 与实例收集规则，不新增语言语义、runtime
ABI、Feng 运行时操作或 codegen 特判。其余核对项均已有直接证据，无需增加排列组合式用例。
