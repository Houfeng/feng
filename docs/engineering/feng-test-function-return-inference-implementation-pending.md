# Feng 函数返回类型推导测试补齐实施文档

> 状态：已完成
>
> 所属总计划：[Feng 测试覆盖补齐计划](./feng-test-coverage-hardening-pending.md)
>
> 交付编号：D1B
>
> 盘点基线：2026-08-24

## 1 文档定位

本文是总计划 D1B 的唯一实施清单，负责记录函数返回类型推导测试的现状证据、交叉矩阵、实施 Todo、
验收结果和实施过程问题。

语言规则只引用主规范，本文不定义或改变函数、方法、Lambda、模块、制品或 ABI 语义。总计划只维护
D1B 的范围、顺序与整体状态；FUNC01～FUNC08 的具体实施状态以本文为准。

## 2 目标、归属与规范依据

### 2.1 目标

针对每一种具有函数体的 callable 形态，分别覆盖以下四种省略返回类型场景：

1. 函数体完全没有 `return`；
2. 函数体只有无值 `return;`；
3. 函数体包含多个有值 `return`，且返回类型一致；
4. 函数体包含多个有值 `return`，但返回类型不一致。

交叉的 callable 形态包括：

- 顶层函数；
- 普通实例方法；
- 普通静态方法；
- `fit` 实例方法；
- `fit` 静态方法；
- 块 Lambda。

此外，以一个跨包公开顶层函数验证“多个返回类型一致”的推导结果能够经过
`fcts_lib -> fcts_bin` 制品边界恢复并运行。

块 Lambda 返回与外层省略返回类型函数的上下文隔离作为独立 FUNC08 验收，不与矩阵中的
“多个返回一致”合并，避免一个失败遮蔽另一个失败面。

### 2.2 测试归属

- “无 `return`”“只有 `return;`”“多个有值 `return` 类型一致”均为合法行为，放入
  `fcts/` 并断言真实运行结果或副作用；
- “多个有值 `return` 类型不一致”为非法行为，放入 `test/semantic/` 并断言编译拒绝；
- 顶层函数冲突已有直接 Semantic 证据，D1B 只复用、不新增等价 case，并按 ISSUE-D1B-001 的人工
  决策仅补充一条 `AE0058` 诊断码断言；
- 块 Lambda 的“无 `return`”已有直接 FCTS 证据，D1B 只复用，不新增等价 case，也不修改既有用例；
- 跨包是推导结果的制品传播维度，不与全部返回形态再次做笛卡尔积。冲突声明无法生成合法 provider
  制品，因此不构成跨包消费行为。

合法行为只成功通过 Semantic 分析或编译不视为完成；必须有 FCTS 运行证据。非法程序不得进入 FCTS。

### 2.3 权威规范

- [函数规范 §4.1](../specifications/feng-function.md)：省略返回类型时的推导规则、块 Lambda 的独立
  callable body，以及返回 Lambda 时必须存在显式 callable-form `spec` 目标；
- [函数规范 §4.2](../specifications/feng-function.md)：`main` 的固定返回类型；
- [模块规范](../specifications/feng-module.md)：公开模块成员的导入与访问；
- [符号表规范](../specifications/feng-symbol-table.md)：跨包消费所依赖的符号制品规则。

若实施结果与上述规范不一致，应先按第 8 节记录最小复现，再停止相关 Todo，不得在本文中解释、改写
规范或弱化合法用例。

### 2.4 非目标

本交付不覆盖：

- 单个有值 `return` 的完整 callable 形态交叉；多个同类型返回已经覆盖有值推导与类型统一，现有
  顶层函数、实例方法和 Lambda 证据继续作为基础证据；
- 显式声明返回类型后的匹配、显式非 `void` 函数使用空 `return;`、省略返回类型时返回 Lambda；
- callable-form 或 object-form `spec` requirement 自身的返回声明；无函数体的签名必须显式声明；
- 构造函数、终结器和 `main` 的专门返回约束；
- 泛型函数、泛型方法、重载、异常、生命周期、内建 subject、value subject 或 descriptor-sized 值
  的组合排列；
- 通过 object-form `spec` witness 调用推导方法，或为普通方法与 `fit` 方法增加跨包排列；
- 除本交付明确列出的返回冲突用例外，其他 Parser AST、Semantic 诊断、Codegen C 结构、runtime
  内部行为或性能结构测试；
- 除 ISSUE-D1B-001 已获人工批准的单条既有测试诊断码断言外，任何其他既有测试语义或断言变更；
  runtime 私有 ABI、公开 ABI、`.ft` 格式或标准库 API 变更。

若实施盘点发现上述非目标存在与本矩阵不等价的独立规范缺口，应先记录到第 8 节，由人工决定是否扩展
D1B 或另立交付，不得直接增加用例。

## 3 现有证据与交叉缺口

### 3.1 现有直接证据

| 规范行为 | 现有直接证据 | 证据结论 |
| --- | --- | --- |
| 顶层函数单个有值返回 | `test_top_level_function_auto_infers_return_type_for_forward_call` | Semantic 接受推导结果并供前向调用使用 |
| 顶层函数多个返回冲突 | `test_top_level_function_rejects_conflicting_inferred_return_types` | Semantic 拒绝类型不一致的两个有值 `return` |
| 实例方法单个有值返回 | `test_method_auto_infers_return_type_for_forward_call` | Semantic 接受实例方法推导结果 |
| 同分析集跨模块公开函数 | `test_imported_function_auto_infers_return_type_across_modules` | Semantic 可跨源码模块使用推导结果，尚未经过包制品 |
| 顶层函数无 `return` | `test_function.ff` 的 `do_nothing()` | 与显式 `void` 函数共同调用，最终只断言 `true`，观察强度不足 |
| 单表达式 Lambda | `test_lambda.ff` 的 `single-expression lambda` | 在显式 `IntMapper` 目标下返回正确值 |
| 块 Lambda 单个有值返回 | `test_lambda.ff` 的 `multi-line lambda` | 在显式 `IntMapper` 目标下返回正确值 |
| 块 Lambda 无 `return` | `test_lambda.ff` 的 `closure captures var binding (reference)` | 在显式 `Action` 目标下运行三次并观察副作用 |
| 特殊成员内 Lambda 返回隔离 | `test_special_member_block_lambda_value_return_uses_lambda_context` | Semantic 证明 Lambda 不继承构造/终结器返回限制 |

### 3.2 当前交叉缺口

下表中的“弱”表示存在间接或组合证据，但不足以作为本矩阵的专项断言；“已有”表示可以直接复用。

| Callable 形态 | A：无 `return` | B：只有 `return;` | C：多个一致 | D：多个冲突 |
| --- | --- | --- | --- | --- |
| 顶层函数 | 弱 | 缺失 | 缺失 | 已有 Semantic |
| 普通实例方法 | 缺失 | 缺失 | 缺失 | 缺失 |
| 普通静态方法 | 缺失 | 缺失 | 缺失 | 缺失 |
| `fit` 实例方法 | 缺失 | 缺失 | 缺失 | 缺失 |
| `fit` 静态方法 | 缺失 | 缺失 | 缺失 | 缺失 |
| 块 Lambda | 已有 FCTS | 缺失 | 缺失 | 缺失 |

现状只有顶层函数零散覆盖多种返回形态；各种方法没有形成返回形态交叉，不能再以单个有值
`return` 代表完整矩阵。

## 4 测试模型

### 4.1 用例编号

FUNC01～FUNC06 分别对应一种 callable 形态，后缀固定表示返回形态：

| 后缀 | 返回形态 | 归属 | 通用断言 |
| --- | --- | --- | --- |
| A | 完全没有 `return` | FCTS | 正常调用，副作用恰好发生一次 |
| B | 只有无值 `return;` | FCTS | 正常调用，`return;` 前副作用恰好发生一次 |
| C | 两个有值 `return` 类型一致 | FCTS | 两条路径均真实执行并返回各自预期值 |
| D | 两个有值 `return` 类型不一致 | Semantic | 编译拒绝；断言错误数量、`AE0058`、位置和冲突信息 |

Callable 与编号映射如下：

| 编号 | Callable 形态 |
| --- | --- |
| FUNC01-A～D | 顶层函数 |
| FUNC02-A～D | 普通实例方法 |
| FUNC03-A～D | 普通静态方法 |
| FUNC04-A～D | `fit` 实例方法 |
| FUNC05-A～D | `fit` 静态方法 |
| FUNC06-A～D | 块 Lambda |
| FUNC07 | 跨包公开顶层函数，使用 C 形态验证制品传播 |
| FUNC08 | 块 Lambda 返回上下文与外层函数推导隔离 |

共形成 26 个逻辑 case：

- 新增 19 个 FCTS；
- 新增 5 个 Semantic case；
- 复用 1 个既有 FCTS：FUNC06-A；
- 复用 1 个既有 Semantic case：FUNC01-D。

### 4.2 文件与入口

- 新增 `fcts/fcts_bin/src/test_function_inference.ff`，实现新增 FCTS 及唯一公开入口
  `test_function_inference()`；
- 新增 `fcts/fcts_lib/src/test/lib_function_inference.ff`，只提供 FUNC07 所需的公开推导函数；
- `fcts_bin` 通过现有 `import fcts_lib.test;` 路径消费 provider，不新增包或依赖；
- 在 `test/semantic/test_semantic.c` 新增 FUNC02-D～FUNC06-D 五个独立 case 及 runner 登记；
- FUNC01-D、FUNC06-A 只复用既有用例，不修改其内容与断言；
- 经人工 Review 批准后，仅在 `fcts/fcts_bin/src/main.ff` 增加一次
  `test_function_inference();`；
- 不修改 `test_function.ff`、`test_lambda.ff` 或其他既有测试的内容与断言。

当前最近一次已记录的 FCTS 基线为 D1A 完成后的
`904 passed, 0 failed, 0 skipped`。实施时必须先确认实际基线；若期间没有其他用例变更，新增
19 个 `test(...)` 后预期为 `923 passed, 0 failed, 0 skipped`。最终验收以实施时基线加 19
为准，不以本文绝对总数替代实际核对。

### 4.3 最小夹具

消费侧只定义以下职责单一的夹具：

- 一个只保存调用次数的引用类型探针，供所有 A、B case 观察副作用；
- 一个同时承载 FUNC02 实例方法与 FUNC03 静态方法的最小普通引用类型；
- 一个最小 `fit` target 及其 `fit` 块，承载 FUNC04、FUNC05；
- 显式 callable-form `Action(): void` 与值返回 selector `spec`，为 FUNC06 提供目标类型；
- 每个 C case 使用布尔参数选择两个相同静态类型、不同值的返回路径；
- FUNC07 provider 使用与 C 相同的两路径结构，但名称和值保持专项唯一。

除 FUNC04、FUNC05 明确需要的最小 `fit` 外，不添加模块级可变状态、泛型、继承、spec 适配、
重载或与目标无关的辅助层。所有新增类型和函数按工程规则编写注释。

### 4.4 断言原则

1. 每个新增 FCTS 只对应矩阵中的一个格子；
2. A、B case 使用各自独立探针，精确断言副作用次数为 1；
3. C case 必须分别调用 true/false 两条路径，并精确断言不同结果；
4. D case 必须使用两个明确不一致的返回类型，不依赖其他前置错误触发拒绝；
5. FUNC06-B、FUNC06-C 必须具有显式 callable-form `spec` 目标；
6. FUNC06-C 只验证块 Lambda 的两个同类型有值返回，分别调用两条路径；
7. FUNC07 必须调用 `fcts_lib` provider，不能在 `fcts_bin` 复制等价本地函数代替；
8. FUNC08 中 Lambda 返回 `string`，外层省略返回类型函数返回 `int`，两个 callable body 的结果均
   必须在运行时被观察。

## 5 用例设计

### 5.1 FUNC01：顶层函数矩阵

- FUNC01-A：顶层函数修改探针后自然结束，函数体完全没有 `return`；
- FUNC01-B：顶层函数修改探针后执行 `return;`；
- FUNC01-C：顶层函数根据布尔参数执行两个同为 `int`、值不同的 `return`；
- FUNC01-D：复用现有顶层函数 `int` / `bool` 返回冲突 Semantic 用例。

FUNC01-A 以可观察副作用补强现有 `do_nothing()` 的纯调用证据。

### 5.2 FUNC02：普通实例方法矩阵

- FUNC02-A：省略返回类型的实例方法修改传入探针后自然结束；
- FUNC02-B：省略返回类型的实例方法修改探针后执行 `return;`；
- FUNC02-C：省略返回类型的实例方法包含两个同类型有值返回，接收者分别执行两条路径；
- FUNC02-D：新增实例方法返回类型冲突 Semantic case。

四个 case 均通过普通接收者访问，不经 `spec` view。

### 5.3 FUNC03：普通静态方法矩阵

- FUNC03-A：省略返回类型的静态方法修改传入探针后自然结束；
- FUNC03-B：省略返回类型的静态方法修改探针后执行 `return;`；
- FUNC03-C：省略返回类型的静态方法包含两个同类型有值返回，通过 `Type.method()` 执行两条路径；
- FUNC03-D：新增静态方法返回类型冲突 Semantic case。

### 5.4 FUNC04：`fit` 实例方法矩阵

- FUNC04-A：省略返回类型的 `fit` 实例方法修改传入探针后自然结束；
- FUNC04-B：省略返回类型的 `fit` 实例方法修改探针后执行 `return;`；
- FUNC04-C：省略返回类型的 `fit` 实例方法包含两个同类型有值返回，通过接收者执行两条路径；
- FUNC04-D：新增 `fit` 实例方法返回类型冲突 Semantic case。

使用无 spec 声明的最小自扩展 `fit`，避免 witness 适配混入返回推导证据。

### 5.5 FUNC05：`fit` 静态方法矩阵

- FUNC05-A：省略返回类型的 `fit` 静态方法修改传入探针后自然结束；
- FUNC05-B：省略返回类型的 `fit` 静态方法修改探针后执行 `return;`；
- FUNC05-C：省略返回类型的 `fit` 静态方法包含两个同类型有值返回，通过 target type 执行两条路径；
- FUNC05-D：新增 `fit` 静态方法返回类型冲突 Semantic case。

### 5.6 FUNC06：块 Lambda 矩阵

- FUNC06-A：复用 `test_lambda.ff` 的
  `closure captures var binding (reference)`，其 `Action` Lambda 无 `return` 并具有直接副作用；
- FUNC06-B：新增显式 `Action` 目标的块 Lambda，修改探针后执行 `return;`；
- FUNC06-C：新增值返回块 Lambda，内部两个 `return` 均返回 `string`，运行时分别调用并断言两条路径；
- FUNC06-D：新增显式 callable-form `spec` 目标下的块 Lambda 返回冲突 Semantic case。

单表达式 Lambda 和单个有值返回的块 Lambda 已有直接 FCTS，不再新增等价 case。

### 5.7 FUNC07：跨包推导签名传播

在 `fcts_lib.test` 中定义省略返回类型的公开顶层函数，使用两个同为 `int`、值不同的返回路径。
`fcts_bin` 分别执行两条路径并断言结果，证明推导签名能够写入并从依赖包制品恢复。

FUNC07 不再交叉 A、B、D：A/B 的 `void` 传播不是当前最强制品缺口；D 在 provider 编译阶段已非法，
无法形成可供消费的 `.ft`。

### 5.8 FUNC08：块 Lambda 与外层返回上下文隔离

定义一个省略返回类型的普通顶层函数，在其函数体内创建具有显式 callable-form `spec` 目标的块
Lambda：Lambda 返回 `string`，外层函数返回 `int`。测试必须实际调用 Lambda，并断言 Lambda 结果和
外层函数结果。

FUNC08 只证明两个 callable body 的返回上下文隔离；FUNC06-C 独立负责块 Lambda 多个返回一致。

## 6 计划变更边界

| 文件 | 计划变更 | 允许范围 |
| --- | --- | --- |
| `fcts/fcts_bin/src/test_function_inference.ff` | 新增 | 探针、最小 callable 夹具、19 个新增 FCTS、一个公开入口 |
| `fcts/fcts_lib/src/test/lib_function_inference.ff` | 新增 | FUNC07 所需的一个公开省略返回类型函数 |
| `fcts/fcts_bin/src/main.ff` | 既有文件追加登记 | Review 批准后只增加 `test_function_inference();` |
| `test/semantic/test_semantic.c` | 既有文件新增 case | 新增 FUNC02-D～FUNC06-D 及五次 runner 登记；按 ISSUE-D1B-001 人工决策仅为 FUNC01-D 补充 `AE0058` 断言 |
| 本文 | 持续更新 | Todo、问题记录、专项与全量结果、最终状态 |
| 总计划 | 状态同步 | 只更新 D1B 范围、状态和本文链接，不复制实施记录 |

若合法 FCTS 或新增 Semantic case 暴露产品错误，不得修改既有测试、产品代码或 runtime 规避失败。
必须先按第 8 节记录事实和分析；凡涉及既有用例、语言语义、特判、runtime 私有 ABI、公开 ABI、
`.ft` 格式或运行时性能，均标记为“待人工决策”并停止相关实施。不确认的处置同样由人工决策。

## 7 可标记实施 Todo

Todo 使用标准 Markdown 复选框：`- [ ]` 表示未完成，`- [x]` 表示已完成。进行中或受阻事项保持
未勾选，并在条目后追加 `（进行中）` 或 `（受阻：ISSUE-D1B-XXX）`；完成后再勾选。

### 7.1 Review 与基线

- [x] 核对函数主规范和当前实现中的普通 callable 与 Lambda 返回推导路径。
- [x] 盘点现有函数、方法、Lambda FCTS 与 Semantic 直接证据。
- [x] 将六种 callable 形态与四种返回形态收敛为 24 格核心矩阵。
- [x] 将跨包传播收敛为一个非 `void`、多个返回一致的独立制品 case。
- [x] 将块 Lambda 与外层函数的返回上下文隔离收敛为独立 FUNC08。
- [x] 人工 Review 并批准矩阵、复用项、跨包边界和停止条件。
- [x] 人工批准向 `test_semantic.c` 新增五个独立 case 与 runner 登记。
- [x] 人工批准仅向 `fcts/fcts_bin/src/main.ff` 新增 D1B 入口登记。
- [x] 实施前确认工作区已有变更，避免覆盖或混入其他交付。
- [x] 实施前运行 `make fcts-tests`，记录实际基线及通过、失败、跳过数量：
  `904 passed, 0 failed, 0 skipped`。

### 7.2 FCTS 实施

- [x] 新增 `test_function_inference.ff`，实现带注释的最小探针、夹具和测试入口。
- [x] 实施 FUNC01-A：顶层函数无 `return`。
- [x] 实施 FUNC01-B：顶层函数只有 `return;`。
- [x] 实施 FUNC01-C：顶层函数多个有值返回类型一致。
- [x] 实施 FUNC02-A：普通实例方法无 `return`。
- [x] 实施 FUNC02-B：普通实例方法只有 `return;`。
- [x] 实施 FUNC02-C：普通实例方法多个有值返回类型一致。
- [x] 实施 FUNC03-A：普通静态方法无 `return`。
- [x] 实施 FUNC03-B：普通静态方法只有 `return;`。
- [x] 实施 FUNC03-C：普通静态方法多个有值返回类型一致。
- [x] 实施 FUNC04-A：`fit` 实例方法无 `return`。
- [x] 实施 FUNC04-B：`fit` 实例方法只有 `return;`。
- [x] 实施 FUNC04-C：`fit` 实例方法多个有值返回类型一致。
- [x] 实施 FUNC05-A：`fit` 静态方法无 `return`。
- [x] 实施 FUNC05-B：`fit` 静态方法只有 `return;`。
- [x] 实施 FUNC05-C：`fit` 静态方法多个有值返回类型一致。
- [x] 复核并复用 FUNC06-A 的既有块 Lambda 无 `return` FCTS，不修改原用例。
- [x] 实施 FUNC06-B：块 Lambda 只有 `return;`。
- [x] 实施 FUNC06-C：块 Lambda 多个有值返回类型一致。
- [x] 新增 `fcts_lib` provider 并实施 FUNC07 跨包推导签名传播。
- [x] 实施 FUNC08：块 Lambda 返回上下文与外层函数推导隔离。
- [x] 在获批范围内向 `main.ff` 登记一次 `test_function_inference()`。
- [x] 静态复核新增文件没有共享可变状态、无关组合或额外输出；既有用例变更仅限人工批准的
  FUNC01-D 单条 `AE0058` 断言。

### 7.3 Semantic 实施

- [x] 复核并复用 FUNC01-D 的既有顶层函数返回冲突 case，并按 ISSUE-D1B-001 的人工决策仅补充
  一条 `AE0058` 诊断码断言。
- [x] 实施 FUNC02-D：普通实例方法多个有值返回类型不一致。
- [x] 实施 FUNC03-D：普通静态方法多个有值返回类型不一致。
- [x] 实施 FUNC04-D：`fit` 实例方法多个有值返回类型不一致。
- [x] 实施 FUNC05-D：`fit` 静态方法多个有值返回类型不一致。
- [x] 实施 FUNC06-D：块 Lambda 多个有值返回类型不一致。
- [x] 为五个新增 Semantic case 各登记一次 runner；除人工批准的 FUNC01-D 单条 `AE0058` 断言外，
  不修改其他既有 case。
- [x] 静态复核每个 D case 只由返回类型冲突触发 `AE0058`，没有遮蔽目标的前置错误。

### 7.4 验证

- [x] 在工程目录执行 `make fcts-tests`，记录结果：
  `923 passed, 0 failed, 0 skipped`。
- [x] 确认新增 19 个 `test(...)` 全部真实执行，不仅成功编译。
- [x] 确认 FUNC06-A 的既有 FCTS 仍在入口中真实执行。
- [x] 确认新增 5 个与复用 1 个 Semantic 冲突 case 均在 runner 中真实执行并通过。
- [x] 执行 `make build/bin/test_semantic` 构建 Semantic 专项测试。
- [x] 执行 `build/bin/test_semantic`，结果为 `semantic tests passed`。
- [x] 确认 FUNC07 的公开函数确实来自 `fcts_lib` 制品而非消费侧同名实现。
- [x] 检查新增失败均已先记录到第 8 节，ISSUE-D1B-001～ISSUE-D1B-004 均已有最终状态或人工结论。
- [x] 在 Codex 沙箱外执行完整 `make test`，退出码为 0。
- [x] 执行 `git diff --check`。

### 7.5 交付收口

- [x] 补齐第 8 节所有实施过程问题的最终状态或人工处置结论。
- [x] 补齐第 9 节实际变更、测试结果、影响与未解决问题。
- [x] 在总计划中只同步 D1B 状态，不复制本文实施细节。
- [x] 将本文状态更新为“已完成”。

## 8 实施过程问题记录

### ISSUE-D1B-001：FUNC01-D 既有用例未断言诊断码

- **关联 Todo / 用例**：FUNC01-D、7.3 Semantic 实施、10.3 完成标准。
- **状态**：已解决。
- **发现阶段**：实施前既有用例复核。
- **最小复现**：检查
  `test_top_level_function_rejects_conflicting_inferred_return_types()` 的现有断言。
- **实际结果**：既有用例断言错误数量、路径、行号和“conflicting inferred return types”消息，未断言
  `errors[0].code`。
- **规范期望**：本文 4.1 要求每个 D case 断言诊断码 `AE0058`；本文同时要求 FUNC01-D 原样复用，
  不修改既有用例。
- **现有证据**：产品分析器为该冲突生成 `AE0058`；新增 D case 可以按本文要求直接断言该诊断码。
- **根因**：实施计划将既有 FUNC01-D 视为完整复用证据时，没有识别其断言集合缺少诊断码。
- **通用处理方案**：方案 A 是经人工批准，仅为既有 FUNC01-D 增加 `AE0058` 断言；方案 B 是保持
  既有用例不变，并由人工批准将 FUNC01-D 的现有断言视为复用例外，同时在本文明确该差异。
- **是否需要修改既有测试**：方案 A 需要；方案 B 不需要。
- **运行时性能影响**：无。
- **runtime ABI / 公开 ABI / `.ft` 影响**：无。
- **是否需要人工决策**：是；取得结论前不修改 FUNC01-D。
- **人工决策结论**：2026-08-24 选择方案 A，授权仅向既有 FUNC01-D 增加一条 `AE0058` 诊断码
  断言；修改后必须重新执行 Semantic 专项和沙箱外完整 `make test`。
- **专项验证结果**：按人工决策为 FUNC01-D 增加 `ASSERT(strcmp(errors[0].code, "AE0058") == 0);`；
  既有 FUNC01-D 与新增 FUNC02-D～FUNC06-D 均随 `build/bin/test_semantic` 执行并通过，输出
  `semantic tests passed`。
- **全量回归结果**：清理嵌套工程构建产物后的沙箱外 `make test` 退出码为 0；UBSan 与普通构建
  两轮均通过。

### ISSUE-D1B-002：跨包公开函数的推导返回类型未进入 Codegen

- **关联 Todo / 用例**：FUNC07、7.2 FCTS 实施、7.4 验证。
- **状态**：已解决。
- **发现阶段**：新增 FCTS 第一次专项回归。
- **最小复现**：在 `open module fcts_lib.test` 中定义省略返回类型的 `open func`，两个路径分别
  `return 701;` 与 `return 702;`，由 `fcts_bin` 导入并调用。
- **实际结果**：`make fcts-tests` 在 provider Codegen 阶段失败；首个有值 `return` 报
  `CE0264: codegen: void function cannot return a value`。
- **规范期望**：函数规范 §4.1 要求省略返回类型时根据函数体推导；两个返回值类型均为 `int`，provider
  应生成非 `void` 函数，并允许推导签名经过 `.ft` 被消费。
- **现有证据**：同包顶层函数、普通方法及 Semantic 跨源码模块推导已有通过证据；本次失败发生在
  `fcts_lib` 构建、消费包执行之前。
- **根因**：Semantic 已将省略声明后的有效返回类型作为 callable signature 的 type fact 保存，符号导出
  也通过同一事实构建 `.ft` 返回类型；Codegen 注册函数和方法时只解析 AST 的显式 `return_type`，
  对 `NULL` 直接按 `void` 降低，未读取已有 type fact。
- **通用处理方案**：在 Codegen 增加统一的 callable 有效返回类型解析：优先显式声明，否则读取
  callable type fact，并由函数、普通方法、`fit` 方法及其泛型发射路径共同使用；只有既无显式声明又
  无事实时才回退到 `void`。不得为 FUNC07 添加显式返回类型或消费侧替代实现规避失败。
- **是否需要修改既有测试**：否；只新增本文计划用例。
- **运行时性能影响**：无；变更只发生在编译期类型解析。
- **runtime ABI / 公开 ABI / `.ft` 影响**：不修改 runtime ABI 和 `.ft` 格式；符号导出已经使用推导
  事实写入现有返回类型字段。此前有值返回的省略声明 callable 无法通过 Codegen，不存在可继续兼容的
  成功制品；修复后生成规范要求的非 `void` 调用表面。
- **是否需要人工决策**：否；不涉及既有用例、runtime、特判、格式变更或已存在成功制品的 ABI 变更。
- **人工决策结论**：不适用。
- **专项验证结果**：首次 `make fcts-tests` 失败，错误为 `CE0264`；完成通用修复后重新执行，结果为
  `923 passed, 0 failed, 0 skipped`，FUNC07 两条 provider 返回路径均通过消费侧运行断言。
- **全量回归结果**：沙箱外 `make test` 退出码为 0；UBSan 与普通构建两轮均通过。

### ISSUE-D1B-003：修复 Codegen 返回类型后 FCTS 进程异常终止

- **关联 Todo / 用例**：FUNC01～FUNC08、7.2 FCTS 实施、7.4 验证。
- **状态**：已解决。
- **发现阶段**：ISSUE-D1B-002 初次修复后的 FCTS 专项回归。
- **最小复现**：构建更新后的 `build/bin/feng`，在工程目录执行 `make fcts-tests`。
- **实际结果**：原 `CE0264` 不再出现，但命令无其他诊断并以 `Abort trap: 6` 退出。
- **规范期望**：FCTS 应完成编译和运行，新增 19 个用例全部通过，总数应为 923。
- **现有证据**：编译器本身已在 `-Wall -Wextra -Werror -pedantic` 下成功构建；异常发生于
  `build/bin/feng run ./fcts/fcts_bin` 过程。LLDB 回溯确认异常发生在 Codegen 错误清理阶段：
  `cg_dispose()` 释放 `free_fns[11].c_abi_name` 时检测到非法地址；异常前实际 Codegen 错误为
  `CE0019: codegen: unsupported builtin inferred callable return type`，位置是新增跨包函数。
- **根因**：包含两个相互独立但由同一路径暴露的通用缺陷。其一，Semantic 推导整数文字得到内建别名
  `int`，`record_type_fact_for_site()` 将该别名原样记录；Codegen 的内建类型表只接受规范宽度类型名，
  因而读取 callable type fact 时拒绝 `int`。其二，`cg_register_free_fn()` 在条目完全初始化前就增加
  `free_fn_count`，返回类型解析失败后，`cg_dispose()` 会把该未完整初始化条目当成有效条目释放。
- **通用处理方案**：Semantic 记录所有推导内建类型事实时，统一按目标 `pointer_size` 使用既有
  `canonical_builtin_type_name()` 规范化别名；Codegen 的自由函数注册改为事务式初始化，仅在所有字段
  成功建立后提交计数，失败路径释放当前条目已取得的资源并清零。不得为新增用例添加显式返回类型，
  也不得删除或弱化新增用例规避异常。
- **是否需要修改既有测试**：否；只新增本文计划用例。
- **运行时性能影响**：无；两项变更均仅发生在编译期。
- **runtime ABI / 公开 ABI / `.ft` 影响**：不修改 runtime ABI、公开 ABI 或 `.ft` 格式；内建别名按
  既有规范映射到宽度明确的类型，修复后由既有 `.ft` 返回类型字段承载。
- **是否需要人工决策**：否；不涉及既有用例、runtime、特判或格式变更，且处理覆盖所有同类推导
  内建类型事实和所有自由函数注册失败路径。
- **人工决策结论**：不适用。
- **专项验证结果**：首次 `make fcts-tests` 以 `Abort trap: 6` 失败；完成规范类型事实和事务式注册修复后，
  编译器以 `-O2 -Wall -Wextra -Werror -pedantic` 重建成功，FCTS 结果为
  `923 passed, 0 failed, 0 skipped`。
- **全量回归结果**：沙箱外 `make test` 退出码为 0；UBSan 与普通构建两轮均通过。

### ISSUE-D1B-004：方案 A 修改后的第二次全量回归在 FCTS 制品恢复阶段失败

- **关联 Todo / 用例**：ISSUE-D1B-001 方案 A、7.4 沙箱外全量回归、10.9 完成标准。
- **状态**：已解决。
- **发现阶段**：按人工批准给既有 FUNC01-D 增加 `AE0058` 断言后的第二次沙箱外 `make test`。
- **最小复现**：在专项 `build/bin/test_semantic` 通过后，从工程根目录执行 `make test`。
- **实际结果**：UBSan 阶段的 C 单测、smoke `91/91` 和 std `601/601` 已通过；进入
  `build/bin/feng run ./fcts/fcts_bin` 时失败，诊断为
  `libstd.a.fd remaps field 'currentPointerCursor' of 'std.tui.screen.Screen' inconsistently`，最终退出码为 2。
- **规范期望**：完整 `make test` 应以退出码 0 完成，FCTS 应为 `923/923`。
- **现有证据**：方案 A 只在 Semantic C 测试中新增一条诊断码断言，不修改 Feng 源码、编译器或
  runtime；修改前同一工作区的沙箱外完整 `make test` 已以退出码 0 通过。失败点位于
  `feng_debug_write_merged_fd()` 合并当前 FCTS 构建调试映射与依赖 `std/std/build/macos-arm64/lib/libstd.a.fd`
  的阶段。根级 `make clean` 只删除根工程 `build/`，不会删除 `std/std`、`std/std_test`、
  `fcts/fcts_lib` 和 `fcts/fcts_bin` 的嵌套构建目录。失败后立即以完全相同的 UBSan 环境执行
  `FENG_CC=clang FENG_CC_FLAGS="-fsanitize=undefined" make fcts-tests`，结果为
  `923 passed, 0 failed, 0 skipped`，并重新生成了 `fcts_bin.fd`。
- **根因**：根因范围已收敛到根级清理未覆盖的嵌套工程增量构建产物：首次 FCTS 构建读取到彼此不一致的
  当前构建调试映射和 `std` 依赖 `.fd`，完全相同的命令在刷新相关产物后不再复现。失败目标 `.fd` 已由
  编译器清理，无法事后恢复冲突双方的具体 `read_expr` 做逐记录比对，因此不把冲突归因到某一条源码字段；
  现有证据足以排除仅新增的 Semantic 诊断码断言和函数返回类型推导实现为触发源。
- **通用处理方案**：不修改源码、既有 FCTS、std 或 runtime。使用工程已有的 `feng clean` 分别精确清理
  `std/std`、`std/std_test`、`fcts/fcts_lib` 和 `fcts/fcts_bin` 的构建产物，再从干净的嵌套工程状态执行
  完整 `make test`。若干净重验仍复现，先补充记录新的客观证据，再决定是否需要测试基础设施变更。
- **是否需要修改既有测试**：否；除已获人工批准的 FUNC01-D 单条 `AE0058` 断言外，不修改其他既有用例。
- **运行时性能影响**：无；处置仅清理并重建测试产物，不修改 runtime 或生成代码。
- **runtime ABI / 公开 ABI / `.ft` 影响**：无；不修改任何 ABI 或文件格式。
- **是否需要人工决策**：否；精确清理可再生构建产物并重跑用户已要求的全量回归属于验证过程，
  不涉及既有用例、runtime、特判或规范变更。若重验指向上述范围，届时再按规则请求人工决策。
- **人工决策结论**：不适用。
- **专项验证结果**：方案 A 后 `build/bin/test_semantic` 输出 `semantic tests passed`；失败后完全相同的
  UBSan FCTS 命令重跑通过 `923/923`。
- **全量回归结果**：第二次沙箱外 `make test` 退出码为 2，失败信息如上；随后使用 `feng clean`
  精确清理四个嵌套工程构建产物，干净状态下重新执行沙箱外 `make test`，最终退出码为 0。UBSan 与
  普通构建两轮的 smoke 均为 `91/91`、std 均为 `601/601`、FCTS 均为 `923/923`，其余工程目标全部通过，
  `.fd` 冲突未再复现。

实施过程中发现任何偏离主规范、本文方案或预期测试结果的问题时，必须先在本节记录客观事实，再继续
分析。需要人工决策的问题在取得明确结论前，不得继续相关实现或通过修改既有用例绕过。

后续问题按以下模板追加：

```markdown
### ISSUE-D1B-XXX：问题标题

- **关联 Todo / 用例**：
- **状态**：待分析 / 待人工决策 / 已解决
- **发现阶段**：
- **最小复现**：
- **实际结果**：
- **规范期望**：
- **现有证据**：
- **根因**：
- **通用处理方案**：
- **是否需要修改既有测试**：
- **运行时性能影响**：
- **runtime ABI / 公开 ABI / `.ft` 影响**：
- **是否需要人工决策**：
- **人工决策结论**：
- **专项验证结果**：
- **全量回归结果**：
```

## 9 交付记录

- **最终状态**：已完成；实现、专项验证、人工决策项和全量回归均已收口。
- **实际文件变更**：新增消费侧 FCTS 文件和 provider 文件；向 `main.ff` 登记入口；向
  `test_semantic.c` 新增五个 case 与 runner，并按人工决策为既有 FUNC01-D 补充一条 `AE0058` 断言；
  Semantic 规范化推导内建类型事实；Codegen 统一读取 callable 有效返回类型并修复自由函数注册失败
  清理；同步本文与总计划状态。
- **逻辑 case 数量**：26 个。
- **新增 FCTS 数量**：19 个。
- **复用 FCTS 数量**：1 个（FUNC06-A），未修改原用例。
- **新增 Semantic case 数量**：5 个。
- **复用 Semantic case 数量**：1 个（FUNC01-D），按人工决策仅补充一条 `AE0058` 诊断码断言。
- **实施前基线**：`904 passed, 0 failed, 0 skipped`。
- **专项验证**：`make fcts-tests` 为 `923 passed, 0 failed, 0 skipped`；
  `build/bin/test_semantic` 输出 `semantic tests passed`。
- **沙箱外全量回归**：清理嵌套工程构建产物后的 `make test` 退出码为 0；UBSan 与普通构建两轮均为
  smoke `91/91`、std `601/601`、FCTS `923/923`，其余工程目标均通过。
- **产品代码变更**：有；仅修复编译期 Semantic type fact 与 Codegen callable 返回类型解析、失败清理，
  详见 ISSUE-D1B-002、ISSUE-D1B-003。
- **运行时性能影响**：无；未修改 runtime，新增解析只发生在编译期。
- **runtime ABI / 公开 ABI / `.ft` 影响**：不修改 runtime ABI、公开 ABI 或 `.ft` 格式；推导结果使用
  既有 type fact 和 `.ft` 返回类型字段。
- **未解决问题**：无。
- **建议 commit message**：`test: cover callable return type inference`。

## 10 完成标准

只有同时满足以下条件，D1B 才能标记为已完成：

1. FUNC01～FUNC06 的 24 格矩阵全部具有新增或明确复用的直接证据；
2. A、B、C 三列合法行为均由 FCTS 真实执行，D 列均由 Semantic 确认拒绝；
3. 新增 19 个 FCTS、复用 1 个 FCTS、新增 5 个 Semantic case、复用 1 个 Semantic case 均通过；
4. FUNC06-C 独立证明块 Lambda 多个返回一致；
5. FUNC07 确认多个返回一致的公开函数推导签名经过 `fcts_lib -> fcts_bin` 制品边界；
6. FUNC08 独立证明块 Lambda 返回上下文与外层函数隔离；
7. 既有测试变更仅限 ISSUE-D1B-001 人工批准的 FUNC01-D 单条 `AE0058` 诊断码断言；
8. 所有实施问题均已解决或取得明确人工处置结论；
9. `make fcts-tests`、Semantic 专项、沙箱外完整 `make test` 和 `git diff --check` 全部通过；
10. 本文 Todo、问题记录、交付记录和总计划状态均已同步。
