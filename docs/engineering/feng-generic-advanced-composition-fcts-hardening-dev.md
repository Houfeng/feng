# Feng 泛型高阶组合场景 FCTS 补强开发文档

> 状态：已完成
>
> 泛型语言规则以
> [feng-generics-draft.md](../specifications/feng-generics-draft.md) 为准；基础组合覆盖及上一轮修复记录见
> [feng-generic-composition-fcts-hardening-dev.md](./feng-generic-composition-fcts-hardening-dev.md)。
> 本文只定义现有语言能力的第二轮组合测试范围、实施顺序和验收门槛，不新增或改变语言语义。
>
> 第一组暴露出的共享体具化缺口已转入独立前置专项：
> [feng-generic-shared-body-reification-bugfix-dev.md](./feng-generic-shared-body-reification-bugfix-dev.md)。
> 该专项已经完成并通过最终 `make test`，本文从第二组继续实施。

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
- compiler tests 通常只用于锁定诊断、AST、Semantic 或生成 C 结构，语言行为由 FCTS 验证；
  第四组循环回收因验证时点限制，按 §4.4 的专项边界改由 `test/cli` 端到端子进程验收。

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

本组不写入 FCTS，改由 `test/cli` 创建并运行独立的跨包 provider/consumer 工程。测试子进程在启动时
通过生命周期规范公开定义的 `FENG_GC_THRESHOLD=1` 固定收集触发条件，使最后一个外部引用释放后
能够在 Feng 用户代码中立即断言终结器计数。该设置只属于测试子进程环境，不改变 FCTS 默认命令、
产品默认阈值、runtime API 或 runtime ABI。

CLI 用例必须同时验证 provider 导出、consumer `.ft` 恢复、泛型闭合类型描述符、managed-field
遍历和实际循环回收，不能只检查生成 C 文本。现有 `test/runtime` 已覆盖回收算法及直接、数组、
aggregate 槽遍历，本组不重复增加等价的手工描述符测试。

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
- [x] 前置专项：泛型共享体具化修复及其最终 `make test`
- [x] 第一组：双层泛型作用域与多个独立类型参数
- [x] 第一组完成后的 `make test`
- [x] 第二组：复杂 reified 值的控制流与生命周期
- [x] 第二组完成后的 `make test`
- [x] 第三组：复杂泛型值作为容器元素
- [x] 第三组完成后的 `make test`
- [x] 第四组：泛型双向对象图的循环回收或经人工确认的替代验收
- [x] 第四组完成后的 `make test`
- [x] 第五组：union-form 与复合泛型数据流
- [x] 第五组完成后的 `make test`
- [x] 最终核对新增用例与既有 FCTS 无等价重复
- [x] 最终 `make test` 无 sanitizer 报告

完成前不得把本文状态改为“已完成”。

## 7 实施中发现的缺陷

### 7.1 imported 泛型方法成员签名缺少双层泛型作用域实例注册（已修复并完成回归）

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
该前置专项已经完成并通过最终 `make test`；第一组也已完成专项验证和全量回归，后续从第二组继续。

### 7.3 泛型共享体未统一接入 lambda 捕获降级（已修复并完成回归）

第二组在 imported 泛型方法共享体内创建捕获局部状态的 lambda 时报错：

```text
CE0102: codegen: lambda capture 'capturedLabel' was not lowered to a capture cell
```

根因是可调用体发码入口不一致：普通顶层函数、普通成员方法和 fit 方法都会先分析
lambda 捕获，再将被捕获的参数、`self` 和局部绑定提升为引用捕获单元；顶层泛型函数和
泛型类型共享方法漏掉了同一阶段，导致创建闭包时无法找到捕获单元。

通用修复规则：

1. 顶层函数、静态方法和实例方法的普通体与泛型共享体必须共用同一套捕获分析、
   绑定提升和清理规则；
2. 捕获保持权威规范中的引用捕获语义；`var` 与外层共享同一存储，`let` 保持不可重新赋值，
   闭包延长捕获绑定及其托管成员的生命期；
3. lambda invoke 体若依赖类型级或方法级泛参，闭包环境必须保存创建点已有的具化描述符
   引用，invoke 体仍按现有静态 slot 读取约定取得闭合依赖；
4. 修复必须按绑定的通用表示分类处理固定布局、直接泛参和 descriptor-sized 聚合，不得针对
   某个测试类型、容器、参数位置或包增加特判；
5. 普通非泛型闭包和已有固定布局捕获保持现有发码与开销。具化描述符是现有静态数据，
   只在原本无法正确发码的泛型 lambda 环境中保存必要指针，不新增 runtime ABI、运行时
   descriptor 构造、名称查找、额外堆分配或非泛型路径开销。

修复必须由 compiler test 锁定两类共享体的捕获单元与描述符传递结构，并由本组
FCTS 验证跨包泛型方法中的完整值、引用捕获和生命周期结果。

当前实现对固定布局捕获继续使用原有 typed capture cell；直接泛参与 descriptor-sized
聚合则以一元素 kinded array 作为捕获单元本身，由既有 array 元素描述统一负责具体大小、
复制、ARC 与循环遍历。该路径仍只有一次捕获单元分配，不修改 runtime ABI，也不增加
固定布局或非泛型闭包的运行时操作。闭包只保存共享体 invoke 所必需的静态 descriptor 指针。

第二组的生命周期用例还确认：按值聚合结果槽在分支写入前会执行类型规定的默认初始化；
若聚合中含普通引用类型字段，其默认对象也会正常进入终结流程，但不会调用显式构造器。
因此生命周期计数必须用非默认标记区分测试显式创建的叶子，不能把“显式构造器调用次数”
直接当作该类型全部实例的创建次数。该行为属于现有默认值与终结语义，不是重复释放。

### 7.4 分支表达式结果槽丢失 descriptor-sized 存储信息（已修复并完成回归）

修复 7.3 后，`if` 与 `match` 表达式返回 `Composite<U>` 时，生成 C 曾引用开放泛型
aggregate descriptor，consumer 链接阶段因而报告未定义符号。根因是表达式分支汇合只保存
逻辑结果类型，没有保留结果槽的具体 descriptor、size 及“C 表达式本身就是存储地址”的
表示信息；普通固定布局聚合可以沿用静态 C 类型，descriptor-sized 聚合则不能。

通用修复规则：

1. `if`、`match` 与 `try/catch` 表达式共用同一种结果槽抽象；
2. descriptor-sized 聚合结果槽在声明处一次读取具体 descriptor 与 size，分支写入、结果传播
   和作用域清理全部复用同一份信息；
3. 分支写入统一按 borrowed/owned 分别执行 aggregate assign/take，结果不得退化为开放泛型
   placeholder 的 C 值复制；
4. 固定布局标量、托管引用和聚合保持原有表示及运行时操作，不新增 runtime ABI 或堆分配。

### 7.5 共享体中的泛型 union 值仍按开放占位布局构造（已修复并完成回归）

第三组首次专项运行在 imported 泛型方法共享体中构造
`Option<Composite<U>>` 时，生成 C 对开放占位结构体执行聚合初始化，并引用开放
`Composite<U>` 的静态 aggregate descriptor。前者不能代表闭合 union 的物理大小，
后者也不是可链接的具化 descriptor，因而产生 C 初始化告警和未定义符号。

该问题不属于 `Option` 或某个成员类型，而是 union-form spec 布局依赖泛参时未接入
descriptor-sized 聚合的通用表示。通用修复规则为：

1. union 的任一直接成员布局依赖泛参时，该 union 本身必须归类为
   descriptor-sized 聚合；嵌套 union 和嵌套 `@value type` 递归复用同一布局判定；
2. 共享体必须从已有 callable descriptor 依赖槽取得闭合 union 及其聚合成员的
   descriptor，按具体 size 声明存储，不得读取开放占位结构体的 `sizeof`；
3. union 构造统一写入 tag、活动成员的 forward slot 和 payload；payload 复制或移动
   必须按成员的具体值种类与 descriptor 处理，嵌套 union 递归复用同一构造路径；
4. 修复只补齐原本无法正确发码的 descriptor-sized union 路径；固定布局 union
   保持原有 C 值表示。不新增 runtime ABI、运行时 descriptor 构造、名称查找或堆分配。

修复需由 compiler test 锁定泛型 union 的 descriptor-sized 存储、闭合成员 descriptor
和地址形式 ABI，并由第三组 FCTS 验证 `none/some` 转换、成员替换、返回值与精确释放次数。

### 7.6 数组字面量对地址形式聚合重复取地址（已修复并完成回归）

修复 7.5 后，第三组的第一个数组用例在运行期崩溃。生成 C 显示，数组字面量
已正确将 descriptor-sized 参数识别为“C 表达式本身就是值存储地址”，但写入数组
槽时仍模板化地生成 `&source`，实际把“参数指针变量的地址”传给
`feng_aggregate_assign`，导致按聚合 descriptor 越界读取。

通用修复规则为：数组字面量的聚合元素写入，统一通过 `ExprResult` 的地址抽象
获取源存储；普通可寻址 C 值生成 `&value`，直接泛参或
descriptor-sized 值则直接复用其存储地址。不以类型名、参数位置或是否 imported 分支。
该修复只改正生成 C 的地址形成，不新增 runtime ABI、运行时操作或分配。

### 7.7 第四组缺少可确定观察循环回收结果的 FCTS 时点（已决策并完成回归）

第四组实施前的现状审计确认：生成的 C 入口会在 Feng `main` 返回、模块级托管值释放之后调用
`feng_runtime_shutdown()`，因而退出时仍在候选缓冲区中的真实循环会被最终回收；但该调用发生在
Feng 用户代码全部结束之后，FCTS 无法在其后通过 `std.test` 断言终结器计数。

程序运行期间只在候选缓冲区达到阈值时触发收集。权威生命周期规范允许实现选择默认阈值，并允许
进程启动时通过 `FENG_GC_THRESHOLD` 覆盖；循环对象的具体释放时机不具备确定性。因此，在普通
`./build/bin/feng run fcts/fcts_bin` 命令中创建固定数量的循环、依赖当前默认阈值或依赖此前用例
遗留的候选数量，都不能形成稳定的语言行为断言。

已确定改由 `test/cli` 运行独立的跨包工程，并在测试子进程启动时设置规范公开的
`FENG_GC_THRESHOLD=1`。该方案使回收在 Feng 用户代码仍可观察时同步发生，同时不依赖实现默认阈值，
不修改 FCTS，不暴露测试专用 runtime 入口，也不新增公开回收 API。具体测试约束见 §4.4。

### 7.8 imported 泛型 union-form 约束的标量叶子被错误要求 witness（已修复并完成回归）

第五组首次专项运行在调用 imported 泛型函数时报告：

```text
CE0329: codegen: missing semantic witness for object-form spec coercion
```

触发声明与调用形态为：

```feng
func accept<T, U: GenericUnion<T>>(value: U): U { return value; }
accept<WideValue, i64>(value);
```

其中 `GenericUnion<T>` 的直接成员包含 `i64`。按 union-form 权威规范，union-form 约束只在编译期
验证实际类型是否为直接 member，不物化 object-form witness；`i64` 实参也不应进入 spec coercion。
当前失败说明 imported 约束恢复或泛型调用隐藏参数发码仍把该约束错误归入 object-form witness 路径。

修复必须按约束 spec 的实际 form 统一分类：只有 object-form 约束生成并传递 witness；union-form
约束只执行既有编译期成员资格检查。provider 本地与 imported 声明、基础类型、引用类型和聚合类型
实参必须复用同一规则，不得针对 `i64`、当前函数或当前包增加特判。修复不得改变 runtime ABI、
增加运行时参数或引入 Feng 程序运行时开销。

实现已在泛型描述符隐藏参数发码的统一入口按约束 form 分类：union-form 约束固定使用空 witness，
其余约束继续保持原有 witness ABI。该修复不增加隐藏参数、运行时操作或分配，也不改变 runtime ABI。
