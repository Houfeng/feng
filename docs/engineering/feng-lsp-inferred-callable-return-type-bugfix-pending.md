# Feng LSP 推导 callable 返回类型 Hover 与调用结果 Completion 修复方案

> 状态：已完成
>
> 性质：独立 LSP bugfix 工程方案，不修改 Feng 语言语义
>
> 发现日期：2026-08-24
>
> 已确认范围：修复 Hover，以及受同一缺口直接影响的 `foo().` /
> `owner.foo().` 调用结果成员 Completion
>
> 关联文档：
>
> - [函数规范 §4.1](../specifications/feng-function.md)：省略返回类型时的权威推导规则；
> - [Feng LSP 已交付方案](./feng-lsp-delivered.md)：LSP 已交付能力与查询语义基线；
> - [Feng LSP 性能优化方案](./feng-lsp-performance-optimize.md)：Hover 与 Completion
>   `Max ≤ 16ms` 性能边界；
> - [函数返回类型推导测试补齐实施文档](./feng-test-function-return-inference-implementation-pending.md)：
>   callable 返回推导的语言行为基线。

## 1 文档定位

本文只修复两个直接消费 callable 有效返回类型的 LSP 查询面：

1. AST-backed `textDocument/hover` 中的 callable 签名；
2. 不完整输入下，`foo().` 或 `owner.foo().` 通过文本 receiver 恢复执行的成员
   `textDocument/completion`。

Semantic 已经确定并记录有效返回类型。LSP 只读取该事实，不得扫描函数体或重新实现返回类型推导。
本问题不是核心编译器推导错误，而是 LSP 把“省略显式返回类型”的空指针错误解释为 `void`，或直接把
空指针作为调用结果类型，导致后续成员 Completion 无法解析。

本次不修复 Completion 候选自身的 Detail、Signature Help、静态方法与 `fit` 方法的文本 receiver 恢复
能力，也不修改其他 LSP 查询面。

## 2 问题与实际原因

### 2.1 Hover 错误显示 `void`

```feng
func choose(flag: bool) {
  if flag {
    return 1;
  }
  return 2;
}
```

64 位目标上的 Semantic 推导结果为 `i64`，当前声明处和调用处 Hover 却显示：

```text
func choose(flag: bool): void
```

普通实例方法也已通过真实 `feng lsp --stdio` 复现相同问题。

当前原因是顶层函数和方法签名 formatter 最终直接调用
`type_ref_to_string_with_style(callable->return_type)`，而该函数把 `NULL` 固定输出为 `void`。

### 2.2 `foo().` 成员 Completion 受同一问题影响

```feng
type Box {
  let value: string;
}

func makeBox() {
  return Box { value: "ok" };
}

makeBox().
```

末尾点号可能使 parser 不保留完整 member expression，Completion 因而进入
`resolve_owner_decl_from_receiver_text()`。其 call transition 当前只读取 AST 上的显式
`return_type`；省略声明时得到 `NULL`，无法把 Semantic 已推导的 `Box` 继续作为 owner。

普通实例方法 `owner.makeBox().` 走同类路径，也会受影响。

## 3 当前实现事实

### 3.1 Semantic type fact 已存在

Semantic 分析成功后，以 `FengCallableSignature` 对象地址作为 `FengSemanticTypeFact.site` 的精确
指针键记录有效返回类型：

- 顶层函数：`&decl->as.function_decl`；
- 普通、静态及 `fit` 方法：`&member->as.callable`。

本文将其简称为 `callable fact key`。它不是源码位置或调用表达式位置；查询通过
`fact->site == site` 的指针相等关系匹配。

记录结果包括：

- 无 `return` 或只有 `return;`：`void`；
- 多个一致的有值 `return`：统一后的类型；
- 多个不一致的有值 `return`：Semantic 拒绝，不产生成功结果。

### 3.2 可复用的 LSP 能力已经存在

`append_optional_static_type_annotation_with_style()` 已实现“显式类型优先；否则查询匹配 analysis 的
type fact；无可靠事实时不伪造类型”。

`FengLspAstReceiverState` 已能表示 `FengTypeRef`、声明类型和内建类型。文本 receiver 的缺口只在顶层
function 与普通 member 两个 call transition 没有读取 type fact。

完整 AST 表达式 owner 解析已经优先读取表达式 type fact，不需要修改。

### 3.3 符号路径已经正确

公开 callable 的推导返回类型已写入 `FengSymbolDeclView.return_type` 和 `.ft`，外部包的 symbol-backed
Hover 通过 `feng_symbol_decl_return_type()` 读取。本次不修改 `.ft` schema、writer、reader 或
symbol provider，只增加协议非回归验证。

## 4 期望行为

### 4.1 Hover

AST-backed Hover 使用以下顺序：

1. 有显式返回类型时显示显式类型；
2. 省略显式类型且 AST 属于匹配的成功 analysis 时，显示对应 type fact；
3. 构造函数、终结器等固定 `void` callable 保持既有展示；
4. 无匹配成功 analysis 时，不得伪造 `: void`，也不得同步执行 Semantic 分析。

第 4 种可正常出现在冷启动 analysis 尚未发布、独立 current parse / source-module index AST 不共享
指针身份，或临时错误无法安全映射最后成功结果时。建议省略返回类型后缀。

如果 AST 确实属于匹配的成功 analysis，却查不到普通有函数体 callable 应有的 fact，则属于异常，必须
记录并停止，不能以省略后缀静默掩盖。

Hover 覆盖以下五类具名 callable：

1. 顶层函数；
2. 普通实例方法；
3. 普通静态方法；
4. `fit` 实例方法；
5. `fit` 静态方法。

返回事实至少覆盖无 `return`、只有 `return;`、多个一致返回、声明类型、带结构
`FengTypeRef` 和显式类型非回归。块 Lambda 没有独立具名声明签名，本次不新增 Lambda Hover。

### 4.2 调用结果成员 Completion

以下既有文本 receiver 形式必须消费相同有效返回类型：

```feng
makeBox().
owner.makeBox().
```

显式返回类型继续直接使用；省略声明时按正确 fact key 查询匹配 analysis，并将 `TYPE_REF`、`DECL` 或
`BUILTIN` 写入既有 receiver state。无法证明返回类型时快速返回空成员结果，不猜测 owner。

本次不扩展：

```feng
Owner.staticMethod().
owner.fitMethod().
```

上述文本恢复能力即使显式声明返回类型也尚不支持，属于独立能力缺口。静态方法与 `fit` 方法仍在 Hover
中覆盖。

### 4.3 性能

相关 Hover 与调用结果 Completion 均按真实 `feng lsp --stdio` 协议计时，所有样本
`Max ≤ 16ms`。每个热场景至少连续采集 200 个样本；P50、P95、P99 仅用于观察。

不得删除超时样本、减少成员候选、降低正确性或通过重试掩盖超限结果。

## 5 范围边界

### 5.1 本次必须交付

- 五类具名 callable 的声明、引用和调用 Hover；
- 顶层 `foo().` 与普通实例 `owner.foo().` 的文本恢复成员 Completion；
- 当前文件、跨源码模块、本地依赖及外部 `.ft` / `.fb` Hover；
- 未就绪、失败分析、最后成功分析复用及恢复；
- 相关 Hover 与调用结果 Completion 的 `Max ≤ 16ms` 证据。

### 5.2 本次不处理

- Completion 候选 Detail 或普通候选签名；
- Signature Help；
- 静态方法与 `fit` 方法的文本 receiver 恢复链；
- 完整 AST expression owner 算法；
- `.ft` round-trip Symbol 专项；
- Lambda Hover、Inlay Hint 或其他 LSP capability；
- Parser、AST、Semantic、symbol provider、Codegen、runtime 或 ABI；
- 返回类型推导规则、诊断码或错误位置。

性能主规范中的全局 Completion `Max ≤ 16ms` 继续有效，但除本次 `foo().` /
`owner.foo().` 场景外，其自动化补齐不由本文交付。

人工已批准新增用例，不得修改既有用例；确有需要时必须再次人工决策。人工已批准相关 Hover 与
Completion 执行 `Max ≤ 16ms`，但这不授权修改无关的既有测试逻辑。

## 6 实现方案

生产代码预期只修改 `src/cli/lsp/service.c`，不修改核心编译器，不新增 View、缓存、索引或公开结构。

### 6.1 AST Hover 签名

顶层函数和方法签名只在现有匹配 analysis session 的 Hover 调用中复用
`append_optional_static_type_annotation_with_style()`：

1. 显式类型保持原展示；
2. 顶层函数以 `&decl->as.function_decl` 查询；
3. 方法以 `&member->as.callable` 查询；
4. 无可靠事实时不追加后缀；
5. callable-form `spec`、构造函数和终结器保持不变。

无 session 的 `decl_signature_to_string()` 和 `member_signature_to_string()` 继续保持原路径。不得向
Completion item 构造链传递 analysis session，不得改变 Completion Detail。

实施前后静态核对：带非空 session 的声明/成员签名 formatter 调用点仍只属于 AST Hover 和 mixin
Hover；发现其他调用面时先记录并停止。

### 6.2 文本 receiver call transition

只修改既有 `FENG_LSP_RECEIVER_PENDING_FUNCTION` 与 `FENG_LSP_RECEIVER_PENDING_MEMBER` transition：

1. 显式返回类型非空时调用 `ast_receiver_state_set_type()`；
2. 省略返回类型时按正确 fact key 查询一次；
3. 将 `TYPE_REF`、`DECL`、`BUILTIN` 写入既有 receiver state；
4. 无 fact 时沿用 fail-closed。

可以增加一个小型私有适配 helper 复用上述逻辑，但不得增加 receiver 状态种类、静态成员识别、`fit`
选择、重载解析或泛型代入。

### 6.3 性能与缓存边界

- 只读取当前请求已有的匹配成功 analysis 或既有 symbol provider；
- `feng_semantic_lookup_type_fact()` 当前线性扫描 fact 数组，不得描述为 O(1)；
- 显式类型非空时不查 fact，每个消费点最多查询一次；
- 不扫描 callable body、`return`、项目文件、依赖包或全量符号；
- 不新增同步 analysis、同步磁盘 I/O、固定等待、缓存、索引或额外查找分配；
- 不改变 analysis 发布、generation、fingerprint、position mapping 或 readiness 规则。

如果线性查询使任一相关样本超过 16ms，先记录并停止；是否修改 Semantic 索引由人工决策。

## 7 测试方案

### 7.1 归属

- 新增 LSP 用例放入 `test/cli/test_cli.c`；
- 允许新增用例和辅助代码，不得修改既有用例的输入、断言或预期；
- 不新增 FCTS，不修改 `test/symbol/test_symbol.c`；
- 异步用例使用 readiness 条件，禁止新增固定 sleep。

### 7.2 Hover 用例

新增：

1. 五类 callable 的声明、引用和调用 Hover；
2. 无 `return`、只有 `return;`、多个一致返回、声明类型、带结构类型与显式类型；
3. callable-form `spec`、构造函数和终结器非回归；
4. 跨源码模块、本地依赖及外部 `.ft` / `.fb` Hover；
5. 未就绪、失败分析、最后成功分析复用与恢复；
6. 无匹配 analysis 时不伪造 `: void`，匹配成功 analysis 缺 fact 时按异常处理。

### 7.3 调用结果 Completion 用例

新增：

1. 在包含 `makeBox().member` 的合法文本中，把光标置于点号后，返回推导类型的字段和实例方法；
2. 在包含 `owner.makeBox().member` 的合法文本中，把光标置于点号后，返回推导类型的后续成员；
3. 带结构 `FengTypeRef` 保留正确 owner 信息；
4. 显式返回类型路径非回归；
5. 只有 `makeBox().` / `owner.makeBox().` 且无匹配或失败 analysis 时为空成员结果，不退化为
   全局候选；
6. 恢复合法文本并发布成功 analysis 后，结果与冷启动成功状态一致。

不新增 Completion Detail、静态 receiver 或 `fit` receiver 用例。

### 7.4 性能用例

新增独立的协议性能场景，使用 readiness 条件，不修改既有场景固定等待，覆盖：

- 五类 callable 的 AST-backed Hover；
- 外部 symbol-backed Hover；
- 未就绪、失败分析及最后成功分析复用 Hover；
- 顶层 `foo().` 与普通实例 `owner.foo().` Completion；
- 冷启动、一次编辑后、语法错误和语义错误。

每个热场景至少 200 个样本，全部强制 `Max ≤ 16ms`。既有性能脚本中的 Hover 断言可按人工批准从旧
P95 改为 Max 门槛，但不得修改场景输入、候选预期或固定等待。无关的普通 Completion 断言留待后续。

## 8 可标记实施 Todo

### 8.1 Review 与基线

- [x] 人工确认本次修复 Hover，以及直接受影响的 `foo().` / `owner.foo().` 成员 Completion。
- [x] 人工确认 Completion Detail、Signature Help、静态/fit 文本 receiver 不在本次处理。
- [x] 人工确认生产代码只修改 LSP，预期限于 `src/cli/lsp/service.c`。
- [x] 人工批准新增用例，不得修改既有用例。
- [x] 人工确认相关 Hover 与 Completion `Max ≤ 16ms`。
- [x] 人工确认无匹配 analysis 时 Hover 省略后缀、Completion fail-closed；匹配 analysis 缺 fact 时停止。
- [x] 人工确认每个热性能场景至少 200 个样本。
- [x] 运行既有 LSP 专项并记录基线。
- [x] 固化 Hover 错误显示 `void` 的最小协议复现。
- [x] 固化 `makeBox().` 缺少候选的最小协议复现；`owner.makeBox().` 随新增协议用例固化。
- [x] 记录相关请求实施前 P50、P95、P99、Max。
- [x] 核对外部 `.ft` symbol-backed Hover 的实际结果。

### 8.2 实现

- [x] 顶层函数 Hover 以 `&decl->as.function_decl` 读取有效 type fact。
- [x] 普通实例、普通静态、`fit` 实例和 `fit` 静态四类方法 Hover 以
      `&member->as.callable` 读取有效 type fact。
- [x] 复用既有可选类型注解 helper，不新增返回类型 View 或缓存。
- [x] 保持无 session formatter 与 Completion Detail 不变。
- [x] 保持 callable-form `spec`、构造函数、终结器和 symbol-backed Hover 不变。
- [x] 顶层 function receiver transition 查询正确 fact key。
- [x] 普通 member receiver transition 查询正确 fact key。
- [x] 复用既有 receiver state 表示三类 type fact。
- [x] 保持完整 AST、静态/fit 文本恢复与其他 LSP 查询面不变。
- [x] 静态复核每个消费点最多查询一次，且无核心编译器、I/O、等待或缓存改动。

### 8.3 测试

- [x] 新增五类 callable Hover 用例。
- [x] 新增 void、一致返回和结构类型 Hover，用既有声明类型与显式类型用例完成非回归覆盖。
- [x] 新增跨模块、本地依赖及外部包 Hover 用例。
- [x] 新增未就绪、失败分析、最后成功分析复用与恢复 Hover 用例。
- [x] 新增 `makeBox().` 与 `owner.makeBox().` 成员 Completion 用例。
- [x] 新增 Completion fail-closed 与恢复用例。
- [x] 新增相关协议性能场景。
- [x] 每个热场景至少 200 个样本并断言 Max ≤ 16ms。
- [x] 冷启动、编辑后和错误状态均断言 Max ≤ 16ms。
- [x] 确认没有新增或修改 Completion Detail、Signature Help、静态/fit receiver 用例。

### 8.4 验证与交付

- [x] 执行 `make build/bin/test_cli` 和 `build/bin/test_cli`。
- [x] 执行既有及新增 LSP 性能专项。
- [x] 在 Codex 沙箱外执行完整 `make test`。
- [x] 执行 `git diff --check`。
- [x] 补齐第 9 节所有问题的状态或人工处置结论。
- [x] 将本文状态更新为“已完成”。

## 9 实施过程问题记录

遇到偏离范围、既有行为或预期结果的问题，必须先记录、再分析和处理。涉及既有用例、核心编译器、
`.ft` schema、runtime、ABI、同步 I/O 或性能回退时，必须停止并由人工决策。

按以下模板追加：

### ISSUE-LSP-IRT-001：局部绑定 Hover 不能作为 Semantic readiness 信号

- **关联 Todo / 用例**：8.1 实施前基线
- **状态**：已解决
- **发现阶段**：实施前最小协议复现
- **最小复现**：打开包含 `let ready = lspInferredReturnMakeBox();` 的合法文档，连续查询 `ready` Hover
- **实际结果**：200 次查询均只返回 `let ready`，没有出现用于判断 analysis 已发布的推导类型
- **期望结果**：基线脚本需要一个可由协议响应确认匹配 Semantic analysis 已发布的 readiness 条件
- **现有证据**：临时基线脚本以真实 `feng lsp --stdio` 执行后退出，未进入生产代码修改阶段
- **根因**：原夹具位于较大的 `fcts_bin` 工程，200 次快速请求结束时仍未通过响应观察到匹配
  Semantic analysis；局部绑定 Hover 本身可以作为 readiness 信号，但“固定请求次数”不能保证该条件成立
- **通用处理方案**：改用最小独立工程，并以 Hover 响应实际出现局部绑定的推导类型作为 readiness
  条件，在有界 deadline 内按响应轮询，不使用固定 sleep
- **是否需要修改既有测试**：否
- **交互性能影响**：readiness 之后另行采样，不把轮询请求计入热路径数据
- **runtime / ABI / `.ft` 影响**：无
- **是否需要人工决策**：当前否；若无法复用现有 readiness 条件则重新评估
- **人工决策结论**：不适用
- **专项验证结果**：最小工程中成功观察到 `let ready: LspInferredReturnBox`
- **全量回归结果**：2026-08-24 沙箱外 `make test` exit 0

### ISSUE-LSP-IRT-002：实施前推导返回 Hover 基线超过 16ms

- **关联 Todo / 用例**：8.1 实施前性能基线
- **状态**：已解决
- **发现阶段**：真实 stdio 最小协议复现
- **最小复现**：在 `fcts_bin` 源码内存快照中追加推导返回函数，以 references 响应作为 readiness 后连续
  查询声明 Hover 200 次
- **实际结果**：Hover 错误显示 `func lspInferredReturnMakeBox(): void`；P50 `16.340ms`、P95
  `20.147ms`、P99 `20.207ms`、Max `20.414ms`
- **期望结果**：保留错误显示的功能基线，同时所有 Hover 样本 Max `≤ 16ms`
- **现有证据**：同一会话中的调用结果 Completion 返回空候选，P50 `0.638ms`、P95 `0.698ms`、P99
  `0.805ms`、Max `0.834ms`
- **根因**：references 响应不能证明当前 AST 已有匹配的成功 Semantic analysis；该轮 Hover
  采样包含了 `wait_for_initial_query_state()` 的冷启动有界等待，不是真实热路径性能
- **通用处理方案**：使用 ISSUE-LSP-IRT-001 的协议可观测 readiness 条件，条件成立后再连续采集
  200 个样本；不放宽门槛、不删除样本且不增加缓存
- **是否需要修改既有测试**：否
- **交互性能影响**：校准后实施前 Hover P50 `0.014ms`、P95 `0.019ms`、P99 `0.024ms`、
  Max `0.028ms`；Completion P50 `0.024ms`、P95 `0.034ms`、P99 `0.045ms`、Max
  `0.116ms`，均未超过 16ms
- **runtime / ABI / `.ft` 影响**：无
- **是否需要人工决策**：否；校准后的真实热路径未发生性能回退
- **人工决策结论**：不适用
- **专项验证结果**：真实 `feng lsp --stdio` 最小复现通过 200 样本热路径基线，并稳定复现 Hover
  错显 `func lspInferredReturnMakeBox(): void`；旧 Completion 夹具为空经 ISSUE-LSP-IRT-004 确认为
  无匹配 analysis，不作为调用结果缺陷的性能基线
- **全量回归结果**：2026-08-24 沙箱外 `make test` exit 0

### ISSUE-LSP-IRT-003：既有 `test_cli` 基线出现子进程等待失败

- **关联 Todo / 用例**：8.1 既有 LSP 专项基线
- **状态**：已解决
- **发现阶段**：生产代码修改前基线
- **最小复现**：执行既有 `build/bin/test_cli`
- **实际结果**：一次执行正常结束；随后一次执行报告 `error: process exited with status -1 (no such
  process)`，并在 `test/cli/test_cli.c:634` 的 `WEXITSTATUS(status) == 0` 断言失败
- **期望结果**：未修改生产代码和现有用例时，既有 `test_cli` 稳定通过
- **现有证据**：当前工作区仅修改本文档；失败发生在本次生产代码实现之前
- **根因**：`test_cli` 包含 LLDB 启动被调试程序的用例；Codex 沙箱禁止该调试子进程，导致 LLDB
  返回失败，进而触发通用命令状态断言
- **通用处理方案**：遵循工程既有约束，在沙箱外执行包含 LLDB 的 `test_cli` 与最终 `make test`；
  不修改既有测试
- **是否需要修改既有测试**：当前否；若需要则停止并由人工决策
- **交互性能影响**：无，与 Hover / Completion 请求路径无关
- **runtime / ABI / `.ft` 影响**：无
- **是否需要人工决策**：否；未发现既有用例或生产行为问题
- **人工决策结论**：不适用
- **专项验证结果**：同一 `build/bin/test_cli` 在沙箱外 exit 0，并输出 `cli tests passed`
- **全量回归结果**：2026-08-24 沙箱外 `make test` exit 0

### ISSUE-LSP-IRT-004：初版实现后临时 Completion 夹具仍返回空候选

- **关联 Todo / 用例**：8.2 顶层 function receiver transition
- **状态**：已解决
- **发现阶段**：初版 LSP 实现后的临时协议验证
- **最小复现**：合法文档 analysis readiness 成立后，通过 `didChange` 把末尾
  `lspInferredReturnMakeBox();` 改为等长的 `lspInferredReturnMakeBox().`，随后立即请求 Completion
- **实际结果**：Hover 已正确显示推导类型；Completion 候选仍为空
- **期望结果**：匹配成功 analysis 可提供 callable fact 时，`foo().` 返回推导类型的成员候选
- **现有证据**：实现后 Hover Max `0.044ms`、Completion Max `0.101ms`，均未超限；尚未证明
  Completion 所用 AST 与 analysis 的 fact key 匹配
- **根因**：夹具在 `didChange` 后只保留末尾点号，当前 parse 与最后成功 analysis 的 AST 指针身份
  不同，且错误文本不能发布新的成功 analysis，因此按已确认边界不存在可匹配的 callable fact
- **通用处理方案**：正向用例在包含 `makeBox().label()` 的合法文本中将 Completion 光标放在点号后，
  以协议可观测条件等待匹配 analysis；只有末尾点号的错误文本另作 fail-closed 用例，不新增来源特判
- **是否需要修改既有测试**：否
- **交互性能影响**：匹配 analysis 后 Hover Max `0.025ms`、Completion Max `0.031ms`；均未超限
- **runtime / ABI / `.ft` 影响**：无
- **是否需要人工决策**：否；结果符合已人工确认的“无匹配 analysis 时 fail-closed”边界
- **人工决策结论**：不适用
- **专项验证结果**：合法文本的点号位置返回 `value`、`label`；只有末尾点号的错误文本在 10 秒
  有界响应轮询内始终为空，证明两类状态已区分
- **全量回归结果**：2026-08-24 沙箱外 `make test` exit 0

```markdown
### ISSUE-LSP-IRT-XXX：问题标题

- **关联 Todo / 用例**：
- **状态**：待分析 / 待人工决策 / 已解决
- **发现阶段**：
- **最小复现**：
- **实际结果**：
- **期望结果**：
- **现有证据**：
- **根因**：
- **通用处理方案**：
- **是否需要修改既有测试**：
- **交互性能影响**：修改前/修改后 P50、P95、P99、Max；是否有超过 16ms 的样本
- **runtime / ABI / `.ft` 影响**：
- **是否需要人工决策**：
- **人工决策结论**：
- **专项验证结果**：
- **全量回归结果**：
```

## 10 强制停止条件

出现以下任一情况时先记录并停止：

- 需要修改语言规范、推导算法、Parser、AST、Semantic、symbol provider 或其他核心生产代码；
- 需要修改 type fact 结构、`site` 身份、索引、生命周期或 `.ft` schema；
- 需要修改 runtime、ABI、Codegen 或生成程序；
- 需要修改任何既有测试用例；
- 需要修改 Completion Detail、Signature Help 或其他未授权 LSP 查询面；
- 需要扩展静态方法或 `fit` 方法的文本 receiver；
- 需要同步分析、磁盘 I/O、固定等待、缓存或扫描 callable body；
- 匹配成功 analysis 中的 callable 缺少应有 fact；
- 任一相关 Hover 或调用结果 Completion 样本超过 16ms；
- 成员 Completion 候选范围发生非预期变化；
- 必须新增 View、缓存或来源特判才能实现；
- 既有显式返回类型、构造函数、终结器或其他 LSP 行为发生变化。

## 11 完成标准

1. 五类 callable 的声明、引用和调用 Hover 正确显示推导返回类型；
2. void、一致返回、声明类型、结构类型和显式类型均有直接 Hover 证据；
3. 顶层 `foo().` 与普通实例 `owner.foo().` 提供正确成员 Completion；
4. 当前文件、跨模块、本地依赖和外部包 Hover 结果一致；
5. 无匹配 analysis 时不伪造类型或 owner，匹配 analysis 缺 fact 时按异常处理；
6. Completion Detail、Signature Help、静态/fit receiver 与其他查询面未修改；
7. 未修改核心编译器、`.ft` schema、runtime 或 ABI；
8. 没有新增同步分析、I/O、固定等待、缓存或来源特判；
9. 每个相关热场景至少 200 个样本，所有相关样本 Max ≤ 16ms；
10. LSP 专项、性能专项、沙箱外 `make test` 和 `git diff --check` 全部通过；
11. 所有问题均已解决或取得明确人工结论。

## 12 Review 重点

本次范围、只新增用例不修改既有用例及 `Max ≤ 16ms` 已确认。实施结论如下：

1. 无匹配 analysis 时 Hover 省略返回后缀、调用结果 Completion fail-closed；实施中没有发现匹配
   analysis 缺 fact；
2. 每个相关热性能场景已采集 200 个样本，没有超过 16ms。

## 13 实施结果

### 13.1 代码

- 生产代码只修改 `src/cli/lsp/service.c`；
- AST-backed 顶层函数和四类方法 Hover 读取既有 callable type fact；
- 顶层函数与普通实例方法的文本 receiver call transition 复用同一有效返回类型适配逻辑；
- 无 session formatter、Completion Detail、Signature Help、静态/fit 文本 receiver、核心编译器、
  `.ft` schema、runtime 与 ABI 均未修改；
- 显式返回类型不查询 fact；省略返回类型的每个消费点最多执行一次既有线性 fact 查询。

### 13.2 用例

`test/cli/test_cli.c` 只新增用例和测试辅助代码，覆盖：

- 五类 callable 与无 `return`、空 `return;`、多个一致返回的 Hover 交叉矩阵；
- 声明及调用位置、结构返回、显式返回和无可靠 fact 的 Hover；
- 当前文件、跨源码模块、本地依赖，以及删除 provider 源码后的外部 `.ft/.fb` Hover；
- 顶层、普通实例、结构 `FengTypeRef` 与显式返回的调用结果成员 Completion；
- 无匹配 analysis 的 Hover 与 Completion fail-closed。

### 13.3 性能

新增 `scripts/test/run_lsp_inferred_callable_performance.py`，所有相关热场景均为 200 个样本。一次完整
实测中：

- 五类 AST-backed Hover 热场景 Max 介于 `0.019ms` 与 `0.076ms`；
- 顶层调用结果 Completion Max `0.069ms`，普通实例调用结果 Completion Max `0.063ms`；
- 语法错误 Hover Max `0.056ms`，语义错误 Hover Max `0.042ms`，恢复 Hover Max `0.090ms`；
- 外部 symbol-backed Hover 热场景 Max `0.105ms`；
- 包含冷启动和 readiness 请求在内的全部专项样本总 Max `1.822ms`。

既有基础协议专项以 200 样本复测：Hover Max `0.064ms`，Completion Max `0.050ms`。1 万、10 万和
100 万行矩阵复测总 Max `0.256ms`。既有性能脚本只把 Hover 强制断言从旧 P95 口径更新为
`Max ≤ 16ms`，没有修改场景输入、候选预期、固定等待或无关 Completion 断言。

### 13.4 回归

- `make build/bin/test_cli`：通过；
- 沙箱外 `build/bin/test_cli`：通过；
- 新增专项、既有基础性能专项和 1 万 / 10 万 / 100 万行矩阵：通过；
- 沙箱外 `make test`：exit 0，UBSan 与普通构建两轮均通过；普通阶段包括 91 项 smoke、601 项
  std、923 项 FCTS，以及 CLI、Symbol、性能约束、增量构建和发布脚本；
- `git diff --check`：通过。
