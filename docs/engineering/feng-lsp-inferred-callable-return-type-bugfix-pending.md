# Feng LSP 推导 callable 返回类型 Hover 与调用结果 Completion 修复方案

> 状态：待 Review
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

1. 顶层 `makeBox().` 返回推导类型的字段和实例方法；
2. 普通实例 `owner.makeBox().` 返回推导类型的后续成员；
3. 带结构 `FengTypeRef` 保留正确 owner 信息；
4. 显式返回类型路径非回归；
5. 无匹配或失败 analysis 时为空成员结果，不退化为全局候选；
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
- [ ] 人工确认无匹配 analysis 时 Hover 省略后缀、Completion fail-closed；匹配 analysis 缺 fact 时停止。
- [ ] 人工确认每个热性能场景至少 200 个样本。
- [ ] 运行既有 LSP 专项并记录基线。
- [ ] 固化 Hover 错误显示 `void` 的最小协议复现。
- [ ] 固化 `makeBox().` / `owner.makeBox().` 缺少候选的最小协议复现。
- [ ] 记录相关请求实施前 P50、P95、P99、Max。
- [ ] 核对外部 `.ft` symbol-backed Hover 的实际结果。

### 8.2 实现

- [ ] 顶层函数 Hover 以 `&decl->as.function_decl` 读取有效 type fact。
- [ ] 普通实例、普通静态、`fit` 实例和 `fit` 静态四类方法 Hover 以
      `&member->as.callable` 读取有效 type fact。
- [ ] 复用既有可选类型注解 helper，不新增返回类型 View 或缓存。
- [ ] 保持无 session formatter 与 Completion Detail 不变。
- [ ] 保持 callable-form `spec`、构造函数、终结器和 symbol-backed Hover 不变。
- [ ] 顶层 function receiver transition 查询正确 fact key。
- [ ] 普通 member receiver transition 查询正确 fact key。
- [ ] 复用既有 receiver state 表示三类 type fact。
- [ ] 保持完整 AST、静态/fit 文本恢复与其他 LSP 查询面不变。
- [ ] 静态复核每个消费点最多查询一次，且无核心编译器、I/O、等待或缓存改动。

### 8.3 测试

- [ ] 新增五类 callable Hover 用例。
- [ ] 新增 void、一致返回、声明类型、结构类型和显式类型 Hover 用例。
- [ ] 新增跨模块、本地依赖及外部包 Hover 用例。
- [ ] 新增未就绪、失败分析、最后成功分析复用与恢复 Hover 用例。
- [ ] 新增 `makeBox().` 与 `owner.makeBox().` 成员 Completion 用例。
- [ ] 新增 Completion fail-closed 与恢复用例。
- [ ] 新增相关协议性能场景。
- [ ] 每个热场景至少 200 个样本并断言 Max ≤ 16ms。
- [ ] 冷启动、编辑后和错误状态均断言 Max ≤ 16ms。
- [ ] 确认没有新增或修改 Completion Detail、Signature Help、静态/fit receiver 用例。

### 8.4 验证与交付

- [ ] 执行 `make build/bin/test_cli` 和 `build/bin/test_cli`。
- [ ] 执行既有及新增 LSP 性能专项。
- [ ] 在 Codex 沙箱外执行完整 `make test`。
- [ ] 执行 `git diff --check`。
- [ ] 补齐第 9 节所有问题的状态或人工处置结论。
- [ ] 将本文状态更新为“已完成”。

## 9 实施过程问题记录

遇到偏离范围、既有行为或预期结果的问题，必须先记录、再分析和处理。涉及既有用例、核心编译器、
`.ft` schema、runtime、ABI、同步 I/O 或性能回退时，必须停止并由人工决策。

按以下模板追加：

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

本次范围、只新增用例不修改既有用例及 `Max ≤ 16ms` 已确认。实施前只需继续确认：

1. 无匹配 analysis 时，是否接受 Hover 省略返回后缀、调用结果 Completion fail-closed；匹配 analysis
   缺 fact 时停止；
2. 每个热性能场景至少 200 个样本是否足够。
