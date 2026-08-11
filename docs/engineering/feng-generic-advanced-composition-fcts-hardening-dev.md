# Feng 泛型高阶组合场景 FCTS 补强开发文档

> 状态：实施暂停（先完成泛型共享体具化修复）
>
> 泛型语言规则以
> [feng-generics-draft.md](../specifications/feng-generics-draft.md) 为准；基础组合覆盖及上一轮修复记录见
> [feng-generic-composition-fcts-hardening-dev.md](./feng-generic-composition-fcts-hardening-dev.md)。
> 本文只定义现有语言能力的第二轮组合测试范围、实施顺序和验收门槛，不新增或改变语言语义。
>
> 第一组暴露出的共享体具化缺口已转入独立前置专项：
> [feng-generic-shared-body-reification-bugfix-dev.md](./feng-generic-shared-body-reification-bugfix-dev.md)。
> 该专项完成并通过最终 `make test` 前，本文不继续后续分组。

## 1 目标

上一轮已经闭环普通 `type`、`@value type`、tuple、object/callable/intersection spec、
三种 callable 来源及双向泛型关系的跨包组合。本轮只补以下仍未形成行为闭环的高风险组合：

1. 类型级泛型参数与方法级泛型参数同时参与复杂参数、返回值和 callable ABI；
2. 多个独立类型参数在同一个共享体中分别采用不同运行时表示；
3. descriptor-sized 复合值经过分支、循环和提前退出时的所有权与清理；
4. descriptor-sized 复合值作为数组、`List` 和 `Option` 元素时的复制、覆盖和释放；
5. 双向泛型对象图不主动断环时的循环回收；
6. union-form spec 与跨包复合泛型值、参数、返回值和收窄的组合。

本轮仍以 FCTS 行为验证为主。新增用例必须验证完整值内容或可观察生命周期结果，不能只验证源码可编译。

## 2 边界

### 2.1 纳入范围

- provider 定义于 `fcts_lib`、consumer 定义于 `fcts_bin` 的跨包路径；
- consumer 自定义的具体 `type`、`@value type` 和 `spec` 作为 imported 泛型 API 的类型实参；
- 标量、托管引用、descriptor-sized 聚合三类表示在同一共享体中的组合；
- `.ft` 恢复、reified dependency、共享泛型 ABI、callable/witness ABI 和生命周期行为；
- 当前语言已经支持的 union-form spec 及其 `match` 收窄行为。

### 2.2 不纳入范围

- 泛型重载优化；保持当前重载声明与调用决议行为不变；
- 泛型协变、逆变及泛型实例间自动转换；
- 泛型与 `extern`、`@abi` 或其他 C ABI 机制的新交叉语义；
- 新增垃圾回收公开 API、修改 runtime 私有 ABI，或依赖测试专用 runtime 入口；
- 语言尚未定义的多约束、variance 或其他新语义；
- 对全部类型与全部语法位置做笛卡尔积枚举。

若某组只有通过新增语言规则、runtime API、runtime ABI 或增加 Feng 程序运行时开销才能实现，必须停止该组并提交人工决策。

## 3 测试组织

- 新的跨包公开模型定义在 `fcts/fcts_lib/src/test/` 的独立文件中；
- 新的可执行断言定义在 `fcts/fcts_bin/src/` 的独立文件中，并使用 `std.test`；
- `fcts/fcts_bin/src/main.ff` 只增加一次新测试入口调用，不修改任何已有测试的语义或断言；
- 测试库不使用 `puts` 或其他额外输出；
- 不修改已有测试文件。若缺陷修复确实要求调整已有测试，必须先取得人工批准；
- compiler tests 只用于锁定诊断、AST、Semantic 或生成 C 结构，语言行为继续由 FCTS 验证。

## 4 分组实施

### 4.1 第一组：双层泛型作用域与多个独立类型参数

定义跨包泛型类型 `Flow<T>`，并在其方法上声明独立的 `U`。同一共享方法体至少同时处理：

- 由 `T` 决定布局的复合参数；
- 由 `U` 决定布局的复合返回值；
- `Func<Composite<T>, Composite<U>>` 形式的 callable 参数；
- 至少一个同时含 `T`、`U` 的 tuple 或 `@value type` 中间值。

至少使用以下两组闭合组合，并验证完整字段内容：

1. `T` 为标量、`U` 为托管类型；
2. `T` 为托管类型、`U` 为宽 `@value type`。

两组闭合组合需要复用同一个 imported 共享方法体，以验证：

- 类型级与方法级描述符的作用域和顺序；
- 多个独立 reified dependency 的精确查找；
- callable 参数 ABI 与 descriptor-sized 返回 out-slot；
- provider 声明、consumer 调用及 `.ft` 恢复使用同一 ABI 分类。

验收：专项运行 `./build/bin/feng run fcts/fcts_bin` 通过后，在沙箱外执行一次 `make test`。

### 4.2 第二组：复杂 reified 值的控制流与生命周期

复用第一组的复合值，在 imported 泛型共享体中覆盖：

- `if` 表达式的两条 aggregate 结果分支；
- `match` 表达式的不同结果分支；
- 循环内反复赋值，并分别通过 `break`、`continue` 离开迭代作用域；
- 从嵌套作用域提前 `return`；
- owned 构造表达式直接作为嵌套共享泛型方法实参；
- 捕获外部状态并返回 descriptor-sized 结果的 lambda。

断言既要验证最终完整值，也要通过带终结器的托管叶子验证没有提前释放、重复释放或遗漏释放。
终结器计数只验证语言可观察的确定性 ARC 路径，不在本组引入循环引用。

验收：专项运行通过后，在沙箱外执行一次 `make test`。

### 4.3 第三组：复杂泛型值作为容器元素

以含多个 managed slot 和标量尾字段的泛型 `@value type` 作为元素，分别覆盖：

- `Composite<T>[]` 的构造、读取、元素覆盖和数组返回；
- `List<Composite<T>>` 的 `add`、`get`、覆盖或删除、扩容及 `clear`；
- `Option<Composite<T>>` 的 `none -> some -> 替换 -> none` 状态变化；
- 容器本身作为 imported 泛型方法的参数和返回值；
- 标量与托管两种 `T` 闭合实例。

如果现有 `List` 公开 API 不提供某项操作，只使用其已有公开能力形成等价生命周期路径，不为测试扩展标准库 API。

断言需要验证元素全部字段、值复制独立性及托管叶子的最终释放次数。

验收：专项运行通过后，在沙箱外执行一次 `make test`。

### 4.4 第四组：泛型双向对象图的循环回收

定义跨包双向泛型引用图，节点字段至少同时包含：

- 指向另一种泛型节点的直接引用或数组引用；
- 依赖 `T` 的托管字段；
- 可观察终结器计数。

测试必须创建失去外部引用的真实循环，不能调用 `clear`、手工置空或以其他方式主动断环。
循环检测应完全复用现有类型描述符、managed-field 元数据及自动循环回收机制；禁止为测试暴露 runtime 私有入口。

为避免把 runtime 内部阈值写成语言契约，用例只允许验证现有公开执行生命周期内能够稳定观察到的回收结果。
如果当前公开行为无法在程序退出前稳定观察回收结果，应停止本组，先提交“是否增加公开回收触发能力或改由 runtime/CLI 测试验收”的人工决策，不能编写依赖私有阈值的脆弱 FCTS。

验收：专项运行通过后，在沙箱外执行一次 `make test`。

### 4.5 第五组：union-form 与复合泛型数据流

定义跨包泛型 union-form spec，使其成员至少包含：

- 一个普通引用类型；
- 一个依赖 `T` 的 descriptor-sized `@value type`；
- 一个标量成员。

覆盖：

- 具体叶子直接满足 union-form 泛型约束；
- union 值作为泛型字段、参数和返回值；
- provider 构造、consumer `match` 收窄并读取完整复合值；
- union 值嵌入 tuple 或 `@value type` 后经过 imported 共享体；
- 标量分支、引用分支和 descriptor-sized 分支的实际运行结果。

现有被注释的直接叶子约束用例不做修改；在新文件中增加独立覆盖。如果该调用不符合当前权威规范，先记录事实并提交人工决策，不通过改变推导或转换规则绕过。

验收：专项运行通过后，在沙箱外执行一次 `make test`。

## 5 缺陷处理流程

每组严格执行：

1. 只增加当前组需要的模型和断言；
2. 先运行 FCTS 专项命令，区分测试写法问题、Semantic 问题、Codegen 问题和 runtime 问题；
3. 如发现编译器缺陷，停止下一组，在本文记录最小复现、根因、通用修复规则及 ABI/性能影响；
4. 修复必须复用现有泛型描述符、aggregate descriptor、witness、callable ABI 和生命周期抽象；
5. 严禁针对测试类型、`List`、`Option`、某个参数位置或某个包增加特判；
6. 修复若增加 Feng 程序运行时操作、改变 runtime 私有 ABI 或引入新分配，必须先取得人工批准；
7. 缺陷修复、专项用例和该组 `make test` 全部通过后，才进入下一组。

## 6 完成标准

- [x] 盘点第二轮高风险组合并排除与上一轮等价的用例
- [ ] 前置专项：泛型共享体具化修复及其最终 `make test`
- [ ] 第一组：双层泛型作用域与多个独立类型参数
- [ ] 第一组完成后的 `make test`
- [ ] 第二组：复杂 reified 值的控制流与生命周期
- [ ] 第二组完成后的 `make test`
- [ ] 第三组：复杂泛型值作为容器元素
- [ ] 第三组完成后的 `make test`
- [ ] 第四组：泛型双向对象图的循环回收或经人工确认的替代验收
- [ ] 第四组完成后的 `make test`
- [ ] 第五组：union-form 与复合泛型数据流
- [ ] 第五组完成后的 `make test`
- [ ] 最终核对新增用例与既有 FCTS 无等价重复
- [ ] 最终 `make test` 无 sanitizer 报告

完成前不得把本文状态改为“已完成”。

## 7 实施中发现的缺陷

### 7.1 imported 泛型方法成员签名缺少双层泛型作用域实例注册（已修复，待回归）

第一组首次专项运行在 consumer codegen 阶段报告：

```text
CE0031: codegen: generic type/spec instance 'Func<...>' was not registered
```

触发签名为 imported `Flow<T>` 上的方法级泛型成员：

```feng
func map<U>(
  mapper: Func<Composite<T>, Composite<U>>,
  input: Composite<T>
): Transfer<T, U>
```

consumer 闭合 owner 为 `Flow<i64>` 后，需要解析的精确成员签名包含
`Func<Composite<i64>, Composite<U>>`。全局预扫描已经注册原始 `T/U` 开放形态，但不能代替
这个“owner 已闭合、方法参数仍开放”的实例。成员签名解析路径只把方法级 `U` 放入活动泛型
上下文，也没有在解析前注册 owner 代入后的完整类型树，最终无法找到对应 callable-form
泛型 spec 实例。

通用修复规则：

1. 泛型类型上的泛型方法必须统一以“owner 类型参数在前、方法类型参数在后”的组合顺序建立
   codegen 类型参数作用域；闭合 owner 的参数虽然已被具体类型替换，仍保留其描述符槽位顺序，
   使方法参数索引与共享方法 ABI 一致；
2. owner 类型实参代入成员参数或返回类型后，必须在该组合作用域和声明所属 program 中递归注册
   完整类型树里的泛型 type 与泛型 spec 实例，再执行类型解析；
3. 注册必须按完整类型实参和活动上下文区分原始开放、部分闭合和完全闭合实例，不能退化为按泛型
   声明原点选择已有实例；
4. provider 本地声明与 consumer 恢复的 imported 声明必须复用同一规则，不针对 `Func`、当前测试
   模型或参数位置增加特判。

该修复只补齐编译期实例注册和泛型参数索引，不改变生成 ABI、runtime 私有 ABI 或 Feng 程序
运行时操作。

实现中还确认，泛型实例壳在递归收集其成员签名时，原逻辑只继承类型级作用域，没有叠加
成员方法自己的类型参数；具体 owner 的方法参数 `U` 因而会被错误登记为闭合类型。当前实现已统一为：

- 字段继承实例化点尚未闭合的泛型作用域；
- 方法与构造器在继承作用域之上叠加自己的泛型参数；
- 超过两层的嵌套作用域先按声明顺序扁平化已有前缀，再追加最内层参数；
- 同一规则同时收集方法约束、参数和返回值中的泛型实例。

### 7.2 泛型共享体具化信息未完整生成（已确认，转入前置专项）

修复 7.1 后确认，问题不只影响 `Flow<T>.map<U>()`。顶层泛型函数和成员方法在共享体内局部创建
`List<T>` 等派生泛型实例时，也会分别暴露闭合实例未注册和 descriptor slot 未建立的问题。现有类型
字段中的 `List<T>` 覆盖不能替代 callable-local dependency 覆盖。

已确定的修复方向为：

1. 泛型参数描述符继续作为独立隐藏参数传入；
2. 泛型静态方法和实例方法共享体增加 `FengFunctionDescriptor *`；
3. `FengFunctionDescriptor` 增加 `reified_callable_deps`，由具体 wrapper 静态生成 direct callee
   descriptor graph；
4. 顶层函数、静态方法和实例方法统一修复参数、返回值及共享体内部派生泛型依赖的收集、闭合注册、
   descriptor 生成和 slot 映射；
5. 不增加运行时 descriptor 构造、堆分配、名称查找或按嵌套深度遍历。

完整 ABI、依赖 domain、FT 恢复、递归 descriptor graph、测试矩阵及实施步骤统一定义在
[feng-generic-shared-body-reification-bugfix-dev.md](./feng-generic-shared-body-reification-bugfix-dev.md)，本文不再重复。
该前置专项完成并通过最终 `make test` 后，才恢复第一组及后续分组。
