# Feng LSP 推导 callable 返回类型提示修复方案

> 状态：待 Review
>
> 性质：独立 LSP bugfix 工程方案，不修改 Feng 语言语义
>
> 发现日期：2026-08-24
>
> 关联文档：
>
> - [函数规范 §4.1](../specifications/feng-function.md)：省略返回类型时的权威推导规则；
> - [Feng LSP 已交付方案](./feng-lsp-delivered.md)：LSP 已交付能力与查询语义基线；
> - [Feng LSP Hover 提示优化方案](./feng-lsp-hover-optimize.md)：Hover 签名与类型名称展示规则；
> - [Feng LSP 性能优化方案](./feng-lsp-performance-optimize.md)：最后一次成功分析、持久符号索引和
>   交互请求性能边界；
> - [函数返回类型推导测试补齐实施文档](./feng-test-function-return-inference-implementation-pending.md)：
>   callable 返回推导的语言行为与跨包编译基线。

## 1 文档定位

本文只定义 LSP 如何读取并展示编译器已经确定的 callable 有效返回类型，以及对应的修复与测试步骤。
函数、方法和 Lambda 的返回类型推导规则仍只由函数主规范定义；本文不得新增推导规则，也不得让 LSP
自行重新推导类型。

本问题属于工具提示与类型查询不一致：Semantic、符号导出和 Codegen 已经持有正确推导结果，但部分
源码 AST-backed LSP 路径仍把“省略显式返回类型”误当成 `void`。

本修复必须保持既有 LSP 性能，并遵守性能主规范的统一硬门槛：本文覆盖的 Hover 与 Completion 在受控
基准中的**每个实测样本均不得超过 16ms**。

## 2 问题与实测结果

### 2.1 顶层函数

```feng
func choose(flag: bool) {
  if flag {
    return 1;
  }
  return 2;
}
```

在 64 位目标上，Semantic 推导结果为规范化的 `i64`。当前实际 LSP 协议结果在函数声明处和调用处均为：

```text
func choose(flag: bool): void
```

期望结果为：

```text
func choose(flag: bool): i64
```

32 位目标对应显示 `i32`。

### 2.2 普通实例方法

```feng
type Picker {
  func pick(flag: bool) {
    if flag {
      return "left";
    }
    return "right";
  }
}
```

当前方法声明处和调用处均错误显示：

```text
func pick(flag: bool): void
```

期望显示：

```text
func pick(flag: bool): string
```

上述结果已经通过真实 `feng lsp --stdio` 的 `textDocument/hover` 请求复现，不是仅根据源码推测。

## 3 当前实现事实

### 3.1 Semantic 已记录 callable 的有效返回类型

Semantic 在 callable 分析成功后，以 `FengCallableSignature` 对象地址作为 `FengSemanticTypeFact.site` 的
精确指针键记录 type fact：

- 显式返回类型记录对应类型；
- 无 `return` 或只有 `return;` 记录 `void`；
- 多个一致的有值 `return` 记录统一后的类型；
- 多个不一致的有值 `return` 由 Semantic 拒绝，不产生可供成功 LSP 查询使用的结果。

因此，成功分析中的 `callable->return_type == NULL` 只表示“源码省略声明”，不表示有效返回类型必然为
`void`。LSP 必须使用与 Semantic 写入时相同的精确指针键查询 type fact，才能得到实际结果。

本文下文将这个精确指针键简称为 `callable fact key`。它不是源码 token、range 或其他源码位置，也不是
调用表达式发生的位置；其含义仅是 Semantic type fact 表中用于按指针身份匹配 AST 对象的 `site` 键。

### 3.2 推导结果已经写入 `.ft`

公开 callable 的符号导出已经使用以下统一路径：

1. `build_callable_return_type_with_tparams()` 在没有显式返回类型时按相同 `callable fact key` 查询
   Semantic type fact；
2. 顶层函数、普通类型成员和 `fit` 成员均把得到的类型写入 `FengSymbolDeclView.return_type`；
3. `.ft` writer 把 `decl->return_type` 序列化为 callable `TSEQ` 的最后一个类型元素；
4. `.ft` reader 将该元素恢复为 `FengSymbolDeclView.return_type`；
5. symbol-backed Hover、Completion Detail 和 Signature Help 均通过
   `feng_symbol_decl_return_type()` 读取该字段。

函数返回类型推导交付中的 `fcts_lib -> fcts_bin` 用例已经证明跨包编译和运行可以消费该推导签名。
因此，对于**确实由外部 `.ft` / `.fb` 符号加载的公开 callable**，当前数据链已经具备正确提示的条件，
symbol-backed 格式化也会使用恢复后的返回类型。

但不能据此把所有“跨包”场景都判定为已正确：

- 外部 `.ft` / `.fb` 依赖走 symbol-backed 路径，按当前实现应正确，但尚无“省略返回类型 callable”的
  LSP 协议回归用例直接锁定该结论；
- 本地路径依赖或同工程跨模块如果以源码 AST 进入最后一次成功分析，Hover 仍优先使用 AST-backed
  路径，因此仍会错误显示 `void`；
- Completion 同时存在 AST-backed 与持久符号路径，当前没有测试保证两条路径最终一致；
- Signature Help 当前只通过 `FengLspCacheQueryContext` 和 `FengSymbolDeclView` 构造签名，不存在
  AST callable 返回类型格式化分支，因此本交付只需验证，不应预设修改其生产实现。

### 3.3 AST-backed LSP 路径错误解释了空指针

当前源码签名格式化存在以下行为：

- 顶层函数格式化直接读取 `decl->as.function_decl.return_type`；
- 普通方法、静态方法和 `fit` 方法格式化直接读取 `member->as.callable.return_type`；
- `type_ref_to_string_with_style()` 把空 type ref 固定格式化为 `void`；
- AST Completion Detail 使用不带 analysis session 的签名格式化入口；
- 完整 AST 表达式的 receiver 解析已经优先读取表达式自身的 Semantic type fact；但文本恢复使用的
  `FengLspAstReceiverState` 在执行 function/member call 时仍直接读取 callable 的显式 `return_type`，
  因而无法继续解析省略返回声明的 `makeBox().member` 链。

同一文件中的 `append_optional_static_type_annotation_with_style()` 和
`semantic_type_fact_to_string_with_style()` 已经具备可复用的正确模式：显式类型优先；省略类型时从
匹配的成功 Semantic analysis 查询 type fact；没有可靠事实时不伪造类型。现有
`FengLspAstReceiverState` 也已经能够表达 `FengTypeRef`、声明类型和内建类型，不需要新增返回类型 View。

### 3.4 根因

根因不是 Semantic 缺少推导，也不是 `.ft` 丢失返回类型，而是 LSP 内存在两条没有收敛的 callable
返回类型消费路径：

- symbol-backed 路径读取已经规范化的 `FengSymbolDeclView.return_type`；
- AST-backed 路径只读取可空的显式 `FengTypeRef *return_type`，没有读取同一 `callable fact key` 的
  Semantic type fact。

修复必须让 AST-backed 调用方复用现有 type fact 格式化和 receiver state，不得新增另一套返回类型
View、缓存或公开数据结构，也不得分别为函数名、方法种类、返回类型或包来源增加特判。

## 4 期望行为

### 4.1 返回类型来源优先级

AST-backed LSP 对普通有函数体 callable 使用以下唯一优先级：

1. 存在显式返回类型时，显示显式类型；
2. 没有显式类型但存在与当前文档匹配的成功 Semantic type fact 时，显示该 type fact；
3. 构造函数、终结器等由语言规则固定为 `void` 的特殊 callable，继续显示 `void`；
4. 没有显式类型且没有可靠 Semantic type fact 时，不得把空指针解释为推导 `void`。

第 4 种 parsed-only 或分析未就绪状态建议保留源码真实形状，例如显示 `func choose(flag: bool)`，暂不显示
返回类型后缀；不得显示未经证明的 `: void`、不得同步触发 Semantic 分析，也不得读取磁盘补算结果。

成功分析后，无 `return` 和只有 `return;` 的 callable 已具有 `void` type fact，因此仍应明确显示
`: void`。

### 4.2 类型名称

- Hover 继续遵循既有短类型名称规则；
- Completion Detail 和 Signature Help 保持各自现有类型名称风格；
- 推导的内建别名使用 Semantic type fact 中的规范化类型，例如 64 位目标上的 `int` 推导显示 `i64`；
- 不根据返回表达式文本、字面量、函数名或声明所在模块猜测类型。

### 4.3 查询面一致性

在相同的已发布语义状态下，以下位置必须显示相同的有效返回类型：

- callable 声明处 Hover；
- callable 引用和调用处 Hover；
- 顶层函数 Completion Detail；
- 实例方法、静态方法与 `fit` 方法 Completion Detail；
- Signature Help；
- 以 callable 调用结果为 receiver 的后续成员 Hover 和 Completion。

当前文件、同工程跨模块、本地路径依赖和外部 `.ft` / `.fb` 包的结果必须一致。不同查询面不得因选择了
AST-backed 或 symbol-backed 数据源而分别显示 `void` 和实际推导类型。

### 4.4 Hover 性能硬约束

本修复的 Hover 性能按以下规则验收：

- 服务端性能口径继续以 LSP 性能主方案为准，即收到完整请求到写出完整响应，不包含编辑器 UI 展示；
- 自动验收使用真实 `feng lsp --stdio` 往返计时，从发送请求帧前开始，到读取匹配的完整响应后结束；该
  口径额外包含本机 stdio framing 与管道开销，实测最大值仍必须 `≤ 16ms`；
- 源码 AST-backed、外部 `.ft` symbol-backed、最后一次成功分析复用和无可靠事实时的 fail-closed 路径
  均必须纳入实测；
- callable 声明处、引用或调用处以及推导返回对象后的链式成员 Hover 均必须纳入实测；
- 所有 Hover 路径统一执行 `Max ≤ 16ms`，不得按查询种类、缓存状态或 readiness 状态改用其他门槛；
- 不得删除超时样本、只报告平均值、减少约定样本数或通过重试掩盖超过 16ms 的结果。

冷启动或 Semantic 尚未就绪时，Hover 必须按第 4.1 节快速返回当前可证明结果或无结果，不得为得到推导
类型同步等待分析。初始化和后台分析自身耗时单独记录，不得混入 Hover 请求后再同步承担。

### 4.5 Completion 性能硬约束

本文的 Completion 专指 LSP `textDocument/completion`，即用户输入时请求候选列表的自动完成；
`completionItem/resolve` 是独立请求，不属于本文的 Completion 指标。本修复的 Completion 按以下规则
验收：

- 自动验收使用与 Hover 相同的真实 `feng lsp --stdio` 往返计时口径；
- 当前成功 analysis、current parse、独立 source-module index、持久 symbol provider、未就绪与失败分析
  等实际路径均执行统一的 `Max ≤ 16ms`；
- 每个热请求场景至少连续采集 200 个样本，P50、P95 和 P99 只作为性能分布观测；
- 冷启动、首次请求、一次编辑后请求、语法错误、语义错误和大项目矩阵中的每个 Completion 实测样本也
  必须 `≤ 16ms`；
- 不得通过减少候选、删除 Detail、降低结果正确性、删除超时样本或重试掩盖超过 16ms 的结果。

## 5 修复范围与边界

### 5.1 必须覆盖的 callable

1. 顶层函数；
2. 普通实例方法；
3. 普通静态方法；
4. `fit` 实例方法；
5. `fit` 静态方法。

块 Lambda 没有独立的具名声明签名，并且其合法块体需要 callable-form `spec` 目标。Lambda 绑定和调用应
继续显示目标 `spec` 的签名，不在本修复中发明匿名 Lambda 返回类型或新增 Inlay Hint。若后续需要直接
展示 Lambda body 的推导结果，应另立交付。

### 5.2 必须覆盖的返回事实

- 推导 `void`：无 `return`；
- 推导 `void`：只有 `return;`；
- 推导内建类型：多个一致的整数或字符串返回；
- 推导声明类型；
- 推导 `FengTypeRef`，至少包含一个带类型参数或容器形态的返回类型；
- 显式返回类型非回归；
- 返回冲突时沿用既有 Semantic 诊断，不展示伪造的成功签名。

上述范围用于覆盖 LSP 数据源和格式化分支，不重新建立函数返回推导的完整语言笛卡尔积；语言行为矩阵已
由 D1B FCTS 与 Semantic 用例负责。

### 5.3 非目标

- 不修改函数返回类型推导规则、诊断码或错误位置；
- 不修改 Parser AST、Semantic type fact 结构、Codegen 或生成程序；
- 不修改 `.ft` schema、版本、字段或既有记录含义；
- 不修改 runtime、runtime 私有 ABI、公开 ABI 或运行时性能；
- 不新增 LSP Inlay Hint capability；
- 不在 LSP 中解析函数体并重复实现返回类型统一算法；
- 不以固定 sleep 等待索引，不在交互请求中同步执行整项目分析或磁盘 I/O；
- 不放宽 LSP 性能主方案的既有指标，本文新增的相关 Hover 与 Completion 实测最大值必须 `≤ 16ms`；
- 未经人工批准，不修改任何既有测试用例。

## 6 通用实现方案

按当前实现，本文所需的签名格式化、Completion session 传递和文本 receiver 转换入口均为
`src/cli/lsp/service.c` 内部实现；Semantic 已经在 `resolve_callable()` 中记录有效返回 type fact，并已通过
`feng_semantic_lookup_type_fact()` 提供查询。因此，预期生产代码变更只限 LSP 的 `service.c`，不修改
Parser、AST、Semantic analyzer、type fact 结构或查询 API、symbol provider、Codegen、runtime 和 ABI。
新增 CLI / Symbol / 性能用例属于测试变更，不代表修改核心编译器生产实现。

如果实施时发现必须修改 `src/semantic/`、AST 定义或其他核心编译器生产代码才能取得该返回类型，必须先
记录问题并触发第 10 节停止条件，由人工重新决定范围；不得把核心改动作为本方案的默认步骤。

### 6.1 复用现有 AST 类型注解读取路径

不新增 AST callable 返回类型 View。签名格式化直接复用现有
`append_optional_static_type_annotation_with_style()`：

1. 显式 `FengTypeRef` 非空时，沿用现有 `type_ref_to_string_with_style()`；
2. 显式类型为空且存在匹配 analysis 时，按与 Semantic 写入时相同的 `callable fact key` 调用现有
   `feng_semantic_lookup_type_fact()`，再复用 `semantic_type_fact_to_string_with_style()`；
3. 没有匹配 analysis 或没有 type fact 时，不追加返回类型后缀；
4. 构造函数和终结器按既有结构种类保留语言固定的 `void` 展示，不依赖猜测或名称特判。

顶层函数的 `callable fact key` 必须是 `&decl->as.function_decl`，方法的 `callable fact key` 必须是
`&member->as.callable`；Semantic 在 `resolve_callable()` 中正是以这两个 `FengCallableSignature *` 地址
写入 type fact，查询端则以 `fact->site == site` 的指针相等关系匹配。不得误传外层 `FengDecl *`、
`FengTypeMember *`、源码位置或调用表达式地址，否则即使它们描述同一份源码，也无法命中该 type fact。

### 6.2 收敛签名格式化

1. `decl_signature_to_string_with_session_and_style()` 的函数分支在参数列表后先输出 `)`，再把
   `&decl->as.function_decl` 和显式 `return_type` 交给现有可选类型注解 helper；
2. `member_signature_to_string_with_session_and_style()` 对普通方法和静态方法执行同样处理，
   `callable fact key` 使用 `&member->as.callable`；构造函数和终结器继续明确输出 `: void`；
3. `decl_hover_signature_to_string()` 已经把匹配 session 传入上述函数，不新增 Hover 专用格式化分支；
4. AST Completion 的 `build_completion_json()` 已经持有 session，但
   `append_decl_completion_item()`、`append_member_completion_item()` 及其中间调用当前丢弃了该参数；只在
   AST 确实属于该 session 时继续传递 session，并调用已有 session-aware formatter；
5. 当前解析或独立 source-module index 的 AST 与成功 analysis 不共享 `callable fact key` 指针身份时，
   必须继续使用无 analysis 路径并省略未知返回后缀，不得把不匹配 session 强行传入；
6. callable-form `spec` 的显式签名格式化保持不变；
7. symbol-backed Hover 与 Completion Detail 继续以 `FengSymbolDeclView.return_type` 为权威，不反向查询
   源码 AST。

### 6.3 收敛调用结果 receiver 解析

完整 AST 表达式的 `resolve_owner_decl_from_object_expr()` 与
`resolve_owner_type_ref_from_object_expr()` 已经优先读取表达式 type fact，不为本修复重写。需要补齐的
实际缺口是 `resolve_owner_decl_from_receiver_text()` 执行文本恢复链时的两个 call transition：当前分别
直接读取顶层函数和成员的显式 `return_type`。

补齐后以下形式能够继续解析：

```feng
makeBox().value
owner.makeBox().value
```

当前文本恢复链的 `find_member_by_name()` 只查普通类型或 object-form `spec` 的直接成员，且类型名根会先
进入 constructor pending 状态；该路径当前不支持 `Owner.staticMethod().member` 或
`owner.fitMethod().member`。这是既有 receiver 能力边界，不是推导返回类型读取错误。本修复不增加静态
成员识别或 `fit` 选择；静态方法与 `fit` 方法的完整 AST 表达式继续由表达式 type fact 覆盖。若要扩展
文本恢复链，必须另行 Review。

实现复用现有 `FengLspAstReceiverState`。如需要避免重复 fact-kind 分支，只允许增加一个小型私有适配
helper：显式返回类型非空时调用现有 `ast_receiver_state_set_type()`；否则按正确的 `callable fact key`
查询一次 type fact，并把 `TYPE_REF`、`DECL` 或 `BUILTIN` 写入 receiver state 已有字段。该 helper 不形成
新的返回类型 View，也不持有或复制 type fact。

这里只消费 Semantic 已确定的类型，不扩大成员可见性、重载、泛型代入或 `fit` 选择规则。无法确定时按
既有 fail-closed 规则返回无结果，不得猜测 owner。

### 6.4 保持性能与缓存边界

- 只读取当前请求已有的匹配成功 analysis 或持久 symbol provider；
- `feng_semantic_lookup_type_fact()` 当前会线性扫描 analysis 的 type fact 数组；本文不得把它描述成 O(1)
  查询，也不得在同一次签名格式化或单个 receiver call transition 中重复调用；
- 只有显式返回类型为空时才允许按 `callable fact key` 查询 type fact，每次签名格式化或 receiver call
  transition 最多查询一次；
- symbol-backed 返回类型继续直接读取既有 `FengSymbolDeclView.return_type`；不得为读取返回类型新增源码
  反查或额外 symbol 扫描，Signature Help 既有候选查找流程不在本修复中改写；
- 不得为确定返回类型新增 callable body、`return` 语句、项目文件、依赖包或全量 symbol 扫描；
- 不得为本修复增加第二套持久缓存、每次请求重建索引或为查找推导类型引入额外堆分配；响应字符串沿用
  既有格式化路径；
- 不改变后台 analysis 发布、generation、fingerprint 或 position mapping 规则；
- 不延长 readiness 轮询，不增加固定等待；
- 不在 Hover、Completion 或 Signature Help 请求中构建项目、打开依赖或读取 `.ft`；
- 外部 `.ft` 必须由既有 workspace symbol index 预先加载后再查询。

如果现有线性 type fact 查询使任一 Hover 样本超过 16ms，必须先记录问题并停止；是否改变 Semantic type
fact 的索引结构属于人工决策，不得在本 LSP bugfix 中自行增加缓存或修改 Semantic。

AST Completion 会批量格式化候选。向 Completion 传递 session 后，多个省略返回声明的 callable 可能各自
触发一次线性 type fact 查询；实施前后必须比较相同候选规模下的 Completion 数据，并继续满足性能主
规范的所有 `textDocument/completion` 请求 `Max ≤ 16ms`。若任一样本超过门槛，同样先记录并停止，
不得私自增加请求级索引或修改 Semantic 数据结构。

实现完成后必须在相同机器、相同构建配置、相同 fixture 和相同样本数下比较修改前后数据。任何相关
Hover 或 Completion 样本超过 16ms，均视为修复未通过，不得以功能正确替代性能验收。

## 7 测试方案

### 7.1 测试归属

- LSP 协议、Hover、Completion、Signature Help 和 readiness 行为放入 `test/cli/test_cli.c`；
- `.ft` 中 callable 推导返回类型的结构化 round-trip 证据放入 `test/symbol/test_symbol.c`；
- 不新增 FCTS。D1B 的既有 FCTS 继续负责语言运行行为，本交付只验证工具消费已有语义事实。

### 7.2 源码 AST-backed 协议用例

至少新增以下直接协议断言：

1. 顶层函数推导内建返回类型，声明处和调用处 Hover 均显示规范化类型且不含 `: void`；
2. 普通实例方法、普通静态方法、`fit` 实例方法、`fit` 静态方法均在声明处和调用处显示推导类型；
3. 顶层函数和四类方法的无 `return` 与只有 `return;` 场景均显示 `: void`；
4. 顶层函数和四类方法的 Completion Detail 使用推导类型；
5. 文本恢复 receiver 链中的顶层函数和普通实例方法调用，能把推导返回对象或容器继续传播到后续成员
   Completion；
6. 顶层函数和四类方法的完整 AST 表达式沿用既有表达式 type fact 路径，链式 Hover 与 Completion 保持
   正确；
7. 同工程跨模块和本地路径依赖中的源码 callable 与当前文件结果一致；
8. parsed-only 无可靠 fact 时省略未知返回后缀；
9. 显式返回类型、callable-form `spec`、构造函数和终结器展示保持不变。

异步分析测试必须使用既有 readiness probe / barrier 等状态条件，禁止通过固定 sleep 猜测分析完成。

### 7.3 外部 `.ft` / `.fb` 用例

新增最小 provider 包，公开省略返回类型的顶层函数、普通实例方法、普通静态方法、`fit` 实例方法和
`fit` 静态方法。打包后 consumer 只通过 `.fb` 依赖访问 provider，不把 provider 源码加入 consumer
analysis。

直接断言：

- Hover 显示 `.ft` 中恢复的推导返回类型；
- Completion Detail 与 Signature Help 使用同一返回类型；
- 推导返回对象后的成员 Completion 可以继续解析；
- 结果不依赖 provider 源码文件仍存在于 consumer workspace。

Signature Help 当前只消费 `FengSymbolDeclView`，因此还应在当前 workspace symbol index 就绪后直接断言
顶层函数和四类方法的签名；测试通过时不修改 Signature Help 生产代码，只有出现与
`feng_symbol_decl_return_type()` 不一致的实测结果时才记录问题并重新分析。

同时在 `test/symbol/test_symbol.c` 新增五类公开推导 callable 的 `.ft` round-trip case，直接断言 reader
恢复的 `feng_symbol_decl_return_type()` 类型种类、规范化内建名称和必要类型参数，避免 LSP 集成失败时
无法区分“符号未写入”和“LSP 未消费”两个层次。现有 Symbol 用例已覆盖显式 callable 返回类型，但
没有覆盖省略声明后的推导返回类型，不能用现有显式用例替代该证据。

### 7.4 未就绪与失败分析

- 首次启动且尚无成功 Semantic type fact 时，省略返回声明不得错误显示 `: void`；
- 临时语法错误期间只允许按既有 fingerprint / position mapping 规则使用最后一次成功结果；
- 无可证明结果时返回无 Hover 或省略返回后缀，不得猜测类型；
- 修复后恢复合法文本，结果必须与冷启动后成功分析一致。

### 7.5 Hover 性能回归

在现有真实 stdio 性能测试基础上增加“推导 callable 返回类型”场景，不得用纯函数微基准代替协议级
证据。专项场景至少覆盖：

1. 源码 AST-backed 顶层函数的声明 Hover 和调用 Hover；
2. 源码 AST-backed 的普通实例、普通静态、`fit` 实例和 `fit` 静态方法 Hover；
3. 推导返回对象后的链式成员 Hover；
4. 外部 `.ft` / `.fb` symbol-backed callable Hover；
5. 尚无可靠 Semantic fact 时的 fail-closed Hover；
6. 复用最后一次成功分析时的 Hover。

对每个热请求场景先完成既有 readiness 条件，再连续采集至少 200 个样本，并报告 P50、P95、P99 和
Max；所有样本均保留。每个场景必须满足：

```text
Max <= 16ms
```

冷启动、首次请求、一次编辑后请求、语法错误和语义错误场景也必须分别记录 Hover Max，且单次结果不得
超过 16ms。继续执行既有 1 万、10 万和 100 万行项目性能矩阵，并为矩阵中的每个 Hover 场景增加
`Max ≤ 16ms` 强制断言。P50、P95 和 P99 继续输出用于观察分布，但不再作为 Hover 验收门槛。

实施时应复用或抽取 `scripts/test/run_lsp_performance.py` 与
`scripts/test/run_lsp_performance_matrix.py` 的协议和统计能力，使脚本在任一门槛失败时返回非零状态。
修改前基线和修改后结果必须使用同一命令记录在第 9 节；不得依赖固定 sleep 等待 analysis，而应使用
已有 readiness probe / barrier。

当前两个脚本都会输出 Hover 与 Completion Max，但仍分别只按旧 Hover P95 和 Completion P95 门槛
强制失败；`run_lsp_performance_matrix.py` 在热请求前还使用固定 `0.1s` 等待。扩展其 Max 断言、增加
推导返回类型场景或把固定等待替换为 readiness 条件都属于修改既有测试，实施前必须取得人工批准。
未批准时应停止在基线阶段，由人工决定修改既有脚本还是新增独立性能脚本，不得自行选择替代方案。

### 7.6 Completion 性能回归

在相同的真实 stdio 性能测试中增加包含多项省略返回声明 callable 的 Completion 场景，至少覆盖：

1. 匹配成功 analysis 的源码 AST-backed 顶层函数和四类方法候选；
2. current parse 或独立 source-module index 与 analysis 不共享 `callable fact key` 时的无 analysis 路径；
3. 外部 `.ft` / `.fb` symbol-backed 候选；
4. 尚无可靠 Semantic fact、失败分析和最后一次成功分析复用；
5. 冷启动、首次请求、一次编辑后请求、语法错误、语义错误，以及 1 万、10 万和 100 万行项目矩阵。

每个热请求场景先通过 readiness 条件确认所测状态，再连续采集至少 200 个样本并报告 P50、P95、P99
和 Max。每个热请求样本及上述冷启动、编辑后、错误状态和大项目矩阵中的每个 Completion 样本都必须
满足 `Max ≤ 16ms`；P50、P95 和 P99 只用于观察分布。实施前后使用相同候选规模，强制断言候选数、
label 与 Detail 不发生非预期变化，不得以减少结果换取性能通过。

## 8 可标记实施 Todo

### 8.1 Review 与基线

- [x] 人工确认所有 Hover 与 Completion 统一使用 `Max ≤ 16ms`，不再保留对应 P95 验收门槛。
- [x] 基于现有实现确认不新增 AST callable 返回类型 View、缓存或公开数据结构。
- [x] 基于现有实现确认预期生产代码变更只限 `src/cli/lsp/service.c`，不修改核心编译器生产实现。
- [ ] 人工 Review 并批准第 4 节的返回类型来源优先级。
- [ ] 人工确认 parsed-only 无可靠事实时省略返回后缀，不再显示伪造的 `: void`。
- [ ] 人工确认生产代码修复范围为 AST Hover、AST Completion Detail，以及顶层函数和普通实例方法的
      文本 receiver；Signature Help 仅验证现有 symbol-backed 路径，静态方法与 `fit` 方法的文本恢复链
      不在本交付扩展。
- [ ] 人工批准扩展既有 LSP 性能脚本的场景与 Max 断言，并以 readiness 条件替换相关固定等待。
- [ ] 运行现有 LSP 专项并记录实施前基线。
- [ ] 使用相同 fixture 和至少 200 个样本记录相关 Hover 的实施前 P50、P95、P99 与 Max。
- [ ] 使用包含多项省略返回声明 callable 的相同 fixture，记录 AST Completion 的实施前候选数、P50、
      P95、P99 与 Max。
- [ ] 固化顶层函数和实例方法错误显示 `void` 的最小协议复现。
- [ ] 核对外部 `.ft` symbol-backed 路径的实施前实际结果。

### 8.2 实现

- [ ] 顶层函数签名格式化复用 `append_optional_static_type_annotation_with_style()`，`callable fact key`
      使用 `&decl->as.function_decl`，不再把空显式返回类型直接交给
      `type_ref_to_string_with_style()`。
- [ ] 普通实例、普通静态、`fit` 实例和 `fit` 静态方法复用同一注解 helper，`callable fact key` 使用
      `&member->as.callable`。
- [ ] 保持 callable-form `spec` 的显式返回格式化，以及构造函数、终结器固定 `void` 展示不变。
- [ ] 保持 `append_optional_static_type_annotation_with_style()` 的既有 binding 与 field 调用行为不变，
      callable 只复用该入口，不修改其已有优先级。
- [ ] 从 `build_completion_json()` 向 AST declaration/member Completion item 构造链传递匹配的 analysis
      session，并改用已有 session-aware signature formatter。
- [ ] 对 current parse 和独立 source-module index 保持无 analysis 路径，确认未知返回类型省略后缀，不把
      不共享 `callable fact key` 指针身份的 analysis 传入 formatter。
- [ ] 文本 receiver 已支持的顶层 function 和普通实例 member call transition，在显式返回类型为空时按
      正确的 `callable fact key` 查询一次 type fact，并写入现有 `FengLspAstReceiverState`。
- [ ] 保持完整 AST 表达式优先读取表达式 type fact 的 receiver 路径不变。
- [ ] 保持 Signature Help、symbol-backed Hover、symbol-backed Completion 和 `.ft` 返回类型读取路径
      不变；仅在新增测试证明实际不一致后记录问题，不预设修改。
- [ ] 静态复核显式返回类型非空时不查询 type fact；同一次签名格式化或单个 receiver call transition
      最多执行一次现有线性 type fact 查询。
- [ ] 静态复核没有新增 View、持久缓存、请求级索引、同步 analysis、磁盘 I/O、固定等待、额外查找分配
      或按名称特判，也没有为确定返回类型新增函数体、`return`、项目文件、依赖包或符号扫描。

### 8.3 测试

- [ ] 新增源码内顶层函数声明与调用位置的 Hover 协议用例。
- [ ] 新增普通实例、普通静态、`fit` 实例和 `fit` 静态方法声明与调用位置的 Hover 协议用例。
- [ ] 新增五类 callable 的无 `return`、只有 `return;` 推导 `void` 用例，以及显式返回类型非回归用例。
- [ ] 新增 callable-form `spec`、构造函数、终结器格式化非回归用例。
- [ ] 新增五类 callable 的 AST Completion Detail 用例，并区分匹配 analysis 与 parsed-only 两种路径。
- [ ] 新增五类 callable 的现有 symbol-backed Signature Help 用例；通过时不修改生产实现。
- [ ] 新增顶层函数和普通实例方法文本 receiver 推导返回对象或容器后的链式 Completion 用例，并为
      五类 callable 保留完整 AST expression fact 路径的非回归断言。
- [ ] 新增同工程跨模块和本地路径依赖用例。
- [ ] 新增外部 `.ft` / `.fb` 包五类 callable 的 Hover、Completion Detail 与 Signature Help 用例。
- [ ] 新增五类推导 callable 返回类型 `.ft` round-trip Symbol 用例。
- [ ] 新增未就绪、失败分析与恢复用例。
- [ ] 新增源码 AST-backed 五类 callable 的协议级 Hover 性能场景。
- [ ] 新增外部 `.ft` symbol-backed、链式成员、fail-closed 和最后成功分析复用的 Hover 性能场景。
- [ ] 每个 Hover 热请求场景连续采集至少 200 个样本，并强制断言 Max ≤ 16ms。
- [ ] 为冷启动、编辑后、语法错误和语义错误 Hover 强制断言 Max ≤ 16ms。
- [ ] 为既有大项目性能矩阵的每个 Hover 场景增加 Max ≤ 16ms 断言。
- [ ] 新增匹配 analysis、无 analysis、外部 symbol、失败分析与最后成功分析复用的 Completion 性能场景。
- [ ] 每个 Completion 热请求场景连续采集至少 200 个样本，确认候选数、label 与 Detail 不变，并强制
      断言 Max ≤ 16ms。
- [ ] 为冷启动、编辑后、语法错误和语义错误 Completion 强制断言 Max ≤ 16ms。
- [ ] 为既有大项目性能矩阵的每个 Completion 场景增加 Max ≤ 16ms 断言。

### 8.4 验证与交付

- [ ] 执行 `make build/bin/test_cli` 和 `build/bin/test_cli`。
- [ ] 执行 `make build/bin/test_symbol` 和 `build/bin/test_symbol`。
- [ ] 确认既有 D1B FCTS 仍为通过状态。
- [ ] 执行推导返回类型 Hover 与 Completion 的真实 stdio 性能专项并保存完整统计结果。
- [ ] 执行 1 万、10 万和 100 万行项目 LSP 性能矩阵。
- [ ] 在 Codex 沙箱外执行完整 `make test`。
- [ ] 执行 `git diff --check`。
- [ ] 补齐第 9 节所有实施问题的最终状态或人工处置结论。
- [ ] 将本文状态更新为“已完成”。

## 9 实施过程问题记录

实施过程中遇到任何偏离本文范围、既有 LSP 行为或预期测试结果的问题，必须先在本节记录客观事实，
再分析和处理。涉及既有用例、Semantic、`.ft` schema、runtime、ABI、同步 I/O 或交互性能回退时，必须
停止相关修改并由人工决策；不确认的处置同样由人工决策。

后续问题按以下模板追加：

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
- **交互性能影响**：修改前/修改后 P50、P95、P99、Max；是否存在超过 16ms 的 Hover / Completion 样本
- **runtime / ABI / `.ft` 影响**：
- **是否需要人工决策**：
- **人工决策结论**：
- **专项验证结果**：
- **全量回归结果**：
```

## 10 强制停止条件

出现以下任一情况时，必须先记录问题并停止，由人工决定是否扩展本交付：

- 需要修改函数主规范或返回类型推导算法；
- 需要修改 Parser、AST、Semantic analyzer、Semantic 查询 API、symbol provider 或其他核心编译器生产代码；
- 需要修改 Semantic type fact 的结构、`site` 指针键身份或生命周期；
- 需要修改 `.ft` schema、版本或既有字段含义；
- 需要修改 runtime、ABI、Codegen 或生成程序；
- 需要修改任何既有测试用例；
- 需要在交互请求中同步分析、同步磁盘 I/O、固定等待或扩大缓存生命周期；
- 需要扫描 callable body、`return`、项目文件、依赖包或全量符号，或增加第二套持久缓存；
- 任一相关 Hover 或 Completion 实测样本超过 16ms；
- AST Completion 候选数、label 或 Detail 发生非预期变化；
- 无法通过复用现有类型注解 helper、`callable fact key` 和 receiver state 实现，必须新增返回类型 View，
  或按函数名、方法种类、包名、返回类型加特判；
- 修复使既有显式返回类型、构造函数、终结器、Completion、Hover 或 Signature Help 行为发生非预期变化。

## 11 完成标准

只有同时满足以下条件，本文才能标记为已完成：

1. 五类具名 callable 的推导返回类型均可在源码 Hover 中正确显示；
2. AST Hover、AST Completion Detail、symbol-backed Signature Help 和调用结果 receiver 展示同一
   Semantic 已确定的有效返回类型；
3. 推导 `void`、内建类型、声明类型和带结构的 `FengTypeRef` 均有直接证据；
4. 当前文件、跨源码模块、本地依赖和外部 `.ft` / `.fb` 包结果一致；
5. parsed-only 或失败分析状态不再把未知结果伪装为 `void`；
6. `.ft` round-trip 直接证明公开推导 callable 的返回类型被写入并恢复；
7. 未修改语言语义、Semantic 推导、`.ft` schema、runtime 或 ABI；
8. 没有新增同步分析、同步磁盘 I/O、固定等待或来源特判；
9. 相关 Hover 的真实 stdio 性能专项中，每个热请求场景至少 200 个样本且 Max ≤ 16ms；
10. 冷启动、编辑后、错误状态和大项目矩阵中的每个 Hover 实测样本均不超过 16ms；
11. 相关 Completion 的真实 stdio 性能专项中，每个热请求场景至少 200 个样本，且冷启动、编辑后、
    错误状态和大项目矩阵中的每个样本均不超过 16ms；候选数、label 与 Detail 保持不变；
12. LSP CLI 专项、Symbol 专项、D1B FCTS、LSP 性能专项、沙箱外完整 `make test` 和
    `git diff --check` 全部通过；
13. 所有实施问题均已解决或取得明确人工处置结论。

## 12 Review 重点

Hover 与 Completion `Max ≤ 16ms` 已由人工明确为统一硬约束，不属于可放宽的 Review 选项。请重点确认
以下五项：

1. 无可靠 Semantic 事实时，是否接受“省略返回后缀”而不是继续显示 `: void`；
2. 是否确认生产代码只修复 AST Hover、AST Completion Detail，以及顶层函数和普通实例方法的文本
   receiver；Signature Help 仅验证现有 symbol-backed 路径，静态方法与 `fit` 方法的文本恢复链不在本
   交付扩展；
3. 是否批准增加外部 `.fb` LSP 集成测试与独立 `.ft` round-trip Symbol 测试，以分别锁定消费层和持久层；
4. 第 7.5 与 7.6 节的协议级场景和每个热场景至少 200 个样本，是否足以证明源码、持久符号、未就绪及
   错误状态均满足性能硬约束；
5. 是否批准修改两个既有性能脚本，为 Hover 与 Completion 增加 Max 断言和推导返回类型场景，并以
   readiness 条件替换涉及的固定等待。
