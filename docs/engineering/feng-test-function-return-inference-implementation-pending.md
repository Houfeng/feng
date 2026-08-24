# Feng 函数返回类型推导 FCTS 补齐实施文档

> 状态：待 Review，尚未实施
>
> 所属总计划：[Feng 测试覆盖补齐计划](./feng-test-coverage-hardening-pending.md)
>
> 交付编号：D1B
>
> 盘点基线：2026-08-24

## 1 文档定位

本文是总计划 D1B 的唯一实施清单，负责记录函数返回类型推导 FCTS 的现状证据、用例边界、实施
Todo、验收结果和实施过程问题。

语言规则只引用主规范，本文不定义或改变函数、方法、Lambda、模块、制品或 ABI 语义。总计划只维护
D1B 的范围、顺序与整体状态；FUNC01～FUNC04 的具体实施状态以本文为准。

## 2 目标与规范依据

### 2.1 目标

在不重复现有 Semantic 证据的前提下，为以下合法语言行为增加可直接运行的 FCTS 证据：

- 省略返回类型的顶层函数可以从多条同类型返回路径得到可供调用方使用的返回类型；
- 省略返回类型的实例方法可以返回并传递推导后的值；
- `fcts_lib` 中省略返回类型的公开函数可以经包制品被 `fcts_bin` 导入和调用；
- 只有无值 `return;` 的函数可以作为无返回值函数正常调用；
- 块 Lambda 的有值 `return` 不参与外层函数的返回类型推导。

新增用例必须断言运行结果；只成功通过 Semantic 分析或编译不视为完成。

### 2.2 权威规范

- [函数规范 §4.1](../specifications/feng-function.md)：省略返回类型时的推导规则、块 Lambda 的独立
  callable body，以及返回 Lambda 时必须存在显式 callable-form `spec` 目标；
- [函数规范 §4.2](../specifications/feng-function.md)：`main` 的固定返回类型；
- [模块规范](../specifications/feng-module.md)：公开模块成员的导入与访问；
- [符号表规范](../specifications/feng-symbol-table.md)：跨包消费所依赖的符号制品规则。

若实施结果与上述规范不一致，应先按第 8 节记录最小复现，再停止相关 Todo，不得在本文中解释、改写
规范或弱化合法用例。

### 2.3 非目标

本交付不覆盖：

- 返回路径类型冲突、显式非 `void` 函数使用空 `return;`、省略返回类型时返回 Lambda 等负向行为；
  已有 Semantic 用例直接覆盖；
- 函数前向调用、函数值与显式 callable-form `spec` 的匹配；已有 Semantic 用例直接覆盖；
- 无任何 `return` 的函数推导为 `void`；现有 `test_function.ff` 已有可运行证据；
- 构造函数、终结器和 `main` 的专门返回约束；这些规则不属于普通函数返回类型推导的新增缺口；
- 静态方法、`fit` 方法、泛型函数、泛型方法、重载、异常、生命周期或 descriptor-sized 值的组合排列；
- Parser AST、Semantic 诊断、Codegen C 结构、runtime 内部行为或性能结构测试；
- 任何产品实现、既有测试语义或断言、runtime 私有 ABI、公开 ABI、`.ft` 格式或标准库 API 变更。

若实施盘点发现上述非目标存在与 FUNC01～FUNC04 不等价的独立规范缺口，应先记录到第 8 节，由人工
决定是否扩展 D1B 或另立交付，不得直接增加用例。

## 3 现有覆盖与直接缺口

| 规范行为 | 现有直接证据 | 当前不足 | D1B 处理 |
| --- | --- | --- | --- |
| 顶层函数推导返回类型 | `test_semantic.c` 的 `test_top_level_function_auto_infers_return_type_for_forward_call` | 只证明静态接受，且函数只有一条有值返回路径 | 新增 FUNC01，运行两条同类型路径 |
| 返回路径类型冲突 | `test_top_level_function_rejects_conflicting_inferred_return_types` | 已有负向 Semantic 直接证据 | 复用，不进入 FCTS |
| 实例方法推导返回类型 | `test_method_auto_infers_return_type_for_forward_call` | 只证明静态接受，没有运行结果 | 新增 FUNC02 的实例方法断言 |
| 导入公开函数的推导类型 | `test_imported_function_auto_infers_return_type_across_modules` | 两个源码模块在同一次 Semantic 分析中处理，未经过 `fcts_lib` 制品和跨包消费 | 新增 FUNC02 的 `fcts_lib -> fcts_bin` 断言 |
| 无 `return` 的省略返回类型函数 | `test_function.ff` 的 `do_nothing()` | 已有可运行证据 | 复用，不新增等价 case |
| 只有空 `return;` 的省略返回类型函数 | 未发现专项 FCTS | 缺少可观察调用与副作用次数断言 | 新增 FUNC03 |
| 块 Lambda 返回上下文独立 | `test_lambda.ff` 的多行 Lambda，以及 `test_special_member_block_lambda_value_return_uses_lambda_context` | 未直接证明不同类型的 Lambda 返回不会污染普通外层函数的省略返回类型推导 | 新增 FUNC04 |
| 省略返回类型时返回 Lambda | `test_omitted_return_function_rejects_lambda_signature_inference` | 已有负向 Semantic 直接证据 | 复用，不进入 FCTS |
| 推导后的函数值匹配命名 callable | `test_omitted_return_function_value_matches_named_function_type` | 已有 Semantic 直接证据，且不属于本组运行时缺口 | 复用，不新增 |

本轮未发现需要新增 Parser、Semantic 或 Codegen 用例的非等价分支。实施前若代码分支复核得到相反
证据，必须先在第 8 节说明缺口和测试归属，再由人工决定是否调整范围。

## 4 测试模型

### 4.1 文件与入口

- 新增 `fcts/fcts_bin/src/test_function_inference.ff`，包含 FUNC01～FUNC04 的消费侧夹具和唯一公开入口
  `test_function_inference()`；
- 新增 `fcts/fcts_lib/src/test/lib_function_inference.ff`，只提供 FUNC02 所需的最小公开推导函数；
- `fcts_bin` 通过现有 `import fcts_lib.test;` 路径消费 provider，不新增包或依赖；
- 经人工 Review 批准后，仅在 `fcts/fcts_bin/src/main.ff` 增加一次 `test_function_inference();`；
- 不修改 `test_function.ff`、`test_lambda.ff`、既有 compiler test 或其他现有用例的内容与断言。

当前最近一次已记录的 FCTS 基线为 D1A 完成后的 `904 passed, 0 failed, 0 skipped`。实施时必须先以
实际工程状态重新确认基线；若期间没有其他用例变更，新增 5 个 `test(...)` 后预期为 909 项，最终验收
以实施时基线加 5 为准，不以本文中的绝对总数替代实际核对。

### 4.2 最小夹具

消费侧只定义以下职责单一的夹具：

- 一个具有省略返回类型实例方法的最小引用类型，供 FUNC02 调用；
- 一个只保存调用次数的最小引用类型，供 FUNC03 观察空 `return;` 前的副作用；
- 一个显式声明返回类型的 callable-form `spec`，为 FUNC04 的块 Lambda 提供目标类型。

provider 只定义一个 `open func`，省略返回类型并返回一个固定、可断言的值。所有新增类型和函数按
工程规则编写注释；不添加模块级可变状态、泛型、继承、`fit`、重载或与目标无关的辅助层。

### 4.3 断言原则

每个 case 必须满足：

1. 由调用结果直接证明推导后的签名可用于真实执行；
2. 涉及多条返回路径时，分别调用并断言每条路径；
3. 涉及副作用时，精确断言执行次数；
4. FUNC04 中 Lambda 与外层函数使用不同返回类型，避免同类型偶然掩盖返回上下文串扰；
5. 跨包 case 必须调用 `fcts_lib` provider，不能在 `fcts_bin` 复制一个等价本地函数代替。

## 5 用例设计

### FUNC01：顶层函数的多条同类型返回路径

在 `fcts_bin` 定义一个省略返回类型的顶层函数，根据布尔参数走两条有值返回路径：

- 两条路径返回相同静态类型、不同值；
- 测试分别以 `true` 和 `false` 调用；
- 精确断言两个运行结果。

该 case 不重复前向调用、冲突类型、单路径返回、数字类型排列或异常退出路径。

### FUNC02：实例方法与跨包公开函数

FUNC02 在总计划中是一个范围项，但实施时拆成两个独立 `test(...)`，避免实例方法失败遮蔽跨包制品
恢复失败，或反向遮蔽：

- FUNC02-A：调用 `fcts_bin` 最小引用类型中省略返回类型的实例方法，并断言返回值；
- FUNC02-B：调用 `fcts_lib.test` 中省略返回类型的公开函数，并断言返回值。

实例方法证明运行时接收者调用；公开函数证明推导后的签名能够写入并从依赖包制品恢复。FUNC02-B
不重复 FUNC01 的多返回路径排列。本范围不扩展静态方法、`fit` 方法或跨包类型方法。

### FUNC03：只有空 `return;` 时推导为 `void`

定义一个省略返回类型的顶层函数，接收计数探针：

- 函数先将计数加一，再执行 `return;`；
- 测试正常调用该函数；
- 断言计数恰为 1。

该 case 与现有“无任何 `return`”的 `do_nothing()` 不等价，且不重复显式 `: void`、多个空返回路径
或显式非 `void` 的负向诊断。

### FUNC04：块 Lambda 的返回不污染外层推导

定义一个省略返回类型的普通顶层函数，在函数体内创建具有显式 callable-form `spec` 目标的块 Lambda：

- Lambda 返回 `string`，外层函数的所有有值返回路径返回 `int`；
- 外层函数实际调用 Lambda，并用其结果选择一个可断言的 `int` 结果，确保 Lambda body 真实执行；
- 测试覆盖外层函数的两条返回路径，并断言各自结果。

若 Lambda 的 `return` 被错误纳入外层推导，`string` 与 `int` 将形成冲突，因此该设计可以直接证明
两个 callable body 的返回上下文隔离。该 case 不覆盖返回 Lambda、构造函数、终结器或 Lambda 捕获。

## 6 计划变更边界

| 文件 | 计划变更 | 允许范围 |
| --- | --- | --- |
| `fcts/fcts_bin/src/test_function_inference.ff` | 新增 | 最小夹具、FUNC01～FUNC04、一个公开测试入口 |
| `fcts/fcts_lib/src/test/lib_function_inference.ff` | 新增 | FUNC02 所需的一个公开省略返回类型函数 |
| `fcts/fcts_bin/src/main.ff` | 既有文件追加登记 | Review 批准后只增加 `test_function_inference();` |
| 本文 | 持续更新 | Todo、问题记录、专项与全量结果、最终状态 |
| 总计划 | 状态同步 | 只更新 D1B 状态和本文链接，不复制实施记录 |

若合法 FCTS 暴露产品错误，不得修改既有测试、产品代码或 runtime 规避失败。必须先按第 8 节记录事实
和分析；凡涉及既有用例、语言语义、特判、runtime 私有 ABI、公开 ABI、`.ft` 格式或运行时性能，均
标记为“待人工决策”并停止相关实施。不确认的处置同样由人工决策。

## 7 可标记实施 Todo

Todo 使用标准 Markdown 复选框：`- [ ]` 表示未完成，`- [x]` 表示已完成。进行中或受阻事项保持
未勾选，并在条目后追加 `（进行中）` 或 `（受阻：ISSUE-D1B-XXX）`；完成后再勾选。

### 7.1 Review 与基线

- [x] 核对函数主规范中的返回类型推导、Lambda 返回上下文和返回 Lambda 规则。
- [x] 盘点现有函数、Lambda FCTS 以及 Semantic 返回类型推导直接证据。
- [x] 将 FUNC01～FUNC04 收敛为非重复的运行时行为用例。
- [ ] 人工 Review 并批准本文的范围、用例设计、跨包 provider 和停止条件。
- [ ] 人工批准仅向 `fcts/fcts_bin/src/main.ff` 新增 D1B 入口登记。
- [ ] 实施前确认工作区已有变更，避免覆盖或混入其他交付。
- [ ] 实施前运行 `make fcts-tests`，记录实际基线及通过、失败、跳过数量。

### 7.2 FCTS 实施

- [ ] 新增 `fcts_lib` provider，实现 FUNC02 所需的公开省略返回类型函数。
- [ ] 新增 `test_function_inference.ff`，实现带注释的最小夹具和测试入口。
- [ ] 实施 FUNC01：顶层函数两条同类型返回路径均返回预期值。
- [ ] 实施 FUNC02-A：实例方法返回预期值。
- [ ] 实施 FUNC02-B：跨包公开函数返回预期值。
- [ ] 实施 FUNC03：只有空 `return;` 的函数正常调用且副作用发生一次。
- [ ] 实施 FUNC04：不同返回类型的块 Lambda 与外层函数返回上下文隔离。
- [ ] 在获批范围内向 `main.ff` 登记一次 `test_function_inference()`。
- [ ] 静态复核新增文件没有修改既有测试、共享可变状态、无关组合或额外输出。

### 7.3 验证

- [ ] 在工程目录执行 `make fcts-tests`，记录总数及通过、失败、跳过数量。
- [ ] 确认新增 5 个 `test(...)` 全部真实执行，不仅成功编译。
- [ ] 确认 FUNC02 的公开函数确实来自 `fcts_lib` 制品而非消费侧同名实现。
- [ ] 检查新增失败是否均已先记录到第 8 节；存在未决问题时停止，不执行交付收口。
- [ ] 在 Codex 沙箱外执行完整 `make test` 并记录结果。
- [ ] 执行 `git diff --check`。

### 7.4 交付收口

- [ ] 补齐第 8 节所有实施过程问题的最终状态或人工处置结论。
- [ ] 补齐第 9 节实际变更、测试结果、影响与未解决问题。
- [ ] 在总计划中只同步 D1B 状态，不复制本文实施细节。
- [ ] 将本文状态更新为“已完成”。

## 8 实施过程问题记录

当前记录：暂无。本文尚处于待 Review 阶段，未开始实施。

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

- **最终状态**：待实施。
- **实际文件变更**：待实施后填写。
- **新增 FCTS 数量**：计划 5 个，实际数量待填写。
- **实施前基线**：待填写。
- **专项验证**：待填写。
- **沙箱外全量回归**：待填写。
- **产品代码变更**：待填写。
- **运行时性能影响**：待填写。
- **runtime ABI / 公开 ABI / `.ft` 影响**：待填写。
- **未解决问题**：待填写。
- **建议 commit message**：待交付完成后填写。

## 10 完成标准

只有同时满足以下条件，D1B 才能标记为已完成：

1. FUNC01、FUNC02-A、FUNC02-B、FUNC03、FUNC04 均以新增 FCTS 真实运行并通过；
2. 顶层函数、实例方法、跨包公开函数、空 `return;` 和 Lambda 隔离均有直接运行断言；
3. 跨包公开函数的推导签名确实经过 `fcts_lib -> fcts_bin` 制品边界；
4. 没有新增与既有 Semantic/FCTS 等价的用例，也未修改既有测试用例的语义和断言；
5. 所有实施问题均已解决或取得明确人工处置结论；
6. `make fcts-tests`、沙箱外完整 `make test` 和 `git diff --check` 全部通过；
7. 本文 Todo、问题记录、交付记录和总计划状态均已同步。
