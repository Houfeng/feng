# Feng LSP 性能优化方案

> 状态：LSP 性能优化主体实现完成。2026-08-24 统一确认的 Hover 与 Completion `Max ≤ 16ms`
> 自动化硬门槛待补齐；仓库全量回归已执行，既有 DAP 子进程和 VS Code 图标基线失败仍未通过，
> 真实 VS Code 无 loading 体验需在重启 LSP 后由开发者最终确认。
>
> 关联文档：
> - [Feng LSP 已交付方案](feng-lsp-delivered.md)：定义已交付 LSP 能力与语义行为基线。
> - [Feng LSP 关键字与注解补全优化](feng-lsp-optimize.md)：定义关键字、注解、Snippet、内建类型与字面量的具体补全和 Hover 行为。
>
> 本文档是 Feng LSP 性能架构的主规范。关联文档只定义功能行为，不重复定义缓存、调度、请求优先级和性能验收规则。

---

## 1. 背景与结论

当前 VS Code 扩展仅使用标准 Language Client，明显的 `loading` 来自 LSP 服务端同步执行昂贵分析，不是插件 UI 自身造成。

在 `std_test`（约 3.2 万行）上通过真实 LSP 协议得到的本地基线如下。该数据用于确认瓶颈数量级，正式实施时必须由自动化基准重新采集：

| 场景 | 当前耗时 |
| --- | ---: |
| 首次 Hover 方法参数内建类型 | 约 1609ms |
| 相同位置再次 Hover | 约 765ms |
| 编辑后首次 Hover 字符串字面量 | 约 821ms |
| 缓存命中的字面量 Hover | 约 0.13ms |
| 输入参数类型 Completion，第一次 | 约 775ms |
| 输入参数类型 Completion，第二次 | 约 777ms |

已确认的主要原因：

1. Hover 在识别内建类型和字面量之前，先尝试整项目语义分析和符号缓存查询。
2. 当前 Hover 缓存会在新分析开始前被释放；分析失败时最近一次成功结果也随之丢失。
3. 任意打开文档变化都会通过全局 `document_revision` 使 Hover 缓存失效。
4. Completion 不复用 Hover 的成功语义分析，每次请求都可能重新执行整项目分析。
5. cached completion 已经追加关键字或内建类型候选时，没有同步更新 `item_count`，上层会把有效结果当作空结果并继续慢路径。
6. LSP 服务端串行处理消息，`$/cancelRequest` 未实际取消请求；连续输入会使慢 Completion 排队。
7. manifest、依赖、provider、workspace `.ft` 和当前文件解析仍可能在交互请求路径重复构建。

本方案结论：

- 交互请求必须只读取当前文档状态、持久 workspace 索引和最近一次成功语义分析，不得同步执行整项目分析。
- workspace 生命周期内只保留**最后一次成功语义分析**，不保存多个历史版本。
- 新分析使用临时 candidate 构建，只有成功后才能原子替换最后一次成功结果。
- 暂不修改核心编译器，可以消除当前主要卡顿并使常见 Hover / Completion 达到无体感延迟；脏代码下最新语义推导的精确度仍受现有 parser / semantic 能力限制。

---

## 2. 范围与边界

### 2.1 本阶段允许修改

- `src/cli/lsp/` 下的协议处理、文档管理、缓存、查询、调度与响应构建。
- `editors/feng-vscode/` 中与标准 LSP capability、同步方式和性能观测直接相关的配置。
- `Makefile` 中 CLI 测试显式 LSP 源文件清单，以及调度线程所需的编译 / 链接选项。
- `test/cli/` 中的 LSP 协议正确性测试；若需修改已有测试用例，必须先取得人工批准。
- `scripts/` 或新增测试目录中的独立 LSP 协议性能基准。
- `dev/` 中本方案的实施状态与验收结果。
- 新增 LSP 专用模块、数据结构、测试和性能基准。
- 通过现有公开 API 调用 lexer、parser、semantic 和 symbol provider。

### 2.2 本阶段禁止修改

- `src/lexer/`
- `src/parser/`
- `src/semantic/`
- `src/codegen/`
- `src/runtime/`（Feng 语言运行时）
- `src/symbol/` 及 `.ft` / `.fb` 格式
- 编译器既有语义、诊断码和构建行为

不得在 LSP 中复制第二套完整 parser 或 semantic，不得为某个具体类型、成员名或项目增加特判。

### 2.3 本阶段不承诺

由于不修改核心编译器，本阶段不承诺：

1. 增量语义分析；后台成功分析仍会执行完整项目分析。
2. parser 失败时返回完整容错 AST。
3. semantic 出现普通源码错误时返回部分语义模型。
4. 对尚未成功分析的新复杂表达式立即给出完全精确的类型推导和成员补全。
5. 已进入核心 semantic 调用后能够在函数内部立即取消。

这些限制不得通过返回未经证明的候选或错误 Hover 来掩盖。

### 2.4 LSP 命名约束

`runtime` 专用于 Feng 语言运行时。LSP 自身不得继续使用 `runtime` 作为模块、类型、变量或日志概念，避免与 `src/runtime/` 及 Feng runtime ABI 混淆。

LSP 统一使用以下职责命名：

| 名称 | 职责 |
| --- | --- |
| `server` | stdio framing、服务进程生命周期和顶层循环 |
| `service` | LSP capability、JSON-RPC 分发和服务级状态 |
| `workspace` | project、document、index、analysis 与 generation 状态 |
| `request_context` | 单次请求的 URI、取消状态和响应构建上下文 |

实施时执行以下重命名：

- `src/cli/lsp/runtime.c` → `src/cli/lsp/service.c`
- `src/cli/lsp/runtime.h` → `src/cli/lsp/service.h`
- `FengLspRuntime` → `FengLspService`
- `feng_lsp_runtime_*` → `feng_lsp_service_*`

现有 `server.c` 保持传输层职责，不把 service 状态和全部请求实现重新并入 `server.c`。新增 LSP 标识符禁止使用 `runtime`。

---

## 3. 性能目标与验收门槛

以下指标均指 LSP 服务端从收到完整请求到写出完整响应的处理时间，不包含 VS Code UI 展示延迟：

| 指标 | 目标 |
| --- | ---: |
| 所有 Hover 请求延迟 Max | ≤ 16ms |
| 所有 `textDocument/completion` 请求延迟 Max | ≤ 16ms |
| 所有交互请求 P99 | ≤ 50ms |
| 已排队过期请求的取消处理 | ≤ 5ms |
| 交互请求同步磁盘 I/O | 0 次 |
| 交互请求同步整项目分析 | 0 次 |
| 编辑、失败或取消导致最后成功缓存变空 | 0 次 |
| 返回使用错误文档版本的精确语义结果 | 0 次 |

本文的 Completion 指 LSP `textDocument/completion`，即用户输入时请求候选列表的自动完成；
`completionItem/resolve` 是独立请求，不属于本表的 Completion 指标。

Hover 与 Completion 使用统一硬门槛，不再按内建类型、字面量、关键字、注解、普通查询、冷启动、
缓存命中状态或候选来源设置不同的 P95 验收值。P50、P95 和 P99 仍需采集并报告，但只作为性能分布
观测；任一 Hover 或 Completion 实测样本超过 16ms 即判定性能回归。

正式性能基准至少覆盖：

- 1 万行小项目；
- 10 万行中型项目；
- 100 万行大型项目；
- 冷启动、热请求、一次编辑后请求、连续快速输入、语法错误和语义错误；
- P50、P95、P99、最大值、内存峰值、缓存命中率和后台分析 CPU 时间。

性能测试失败必须视为回归，不能只检查平均值。

---

## 4. 核心不变量

### 4.1 只保留最后一次成功语义分析

每个 workspace 只持有一个长期缓存：`last_successful_analysis`。

```text
current_documents          当前编辑器文本，每个文档只保留当前版本
current_parse              每个打开文档最多一个当前版本解析结果
last_successful_analysis   每个 workspace 唯一的成功语义缓存
candidate_analysis         正在构建的临时对象，不属于历史缓存
```

不得保存语义分析历史版本队列，也不得为了位置映射长期保留多个旧语义快照。

### 4.2 成功后替换，失败时保留

必须遵守以下状态转换：

```text
尚无成功分析
    ├── candidate 成功 ──> last_successful = candidate
    └── candidate 失败 ──> 仍无成功分析

已有 last_successful
    ├── candidate 成功且 generation 更新
    │       └── 原子替换 last_successful
    └── candidate 失败 / 取消 / generation 不新于已发布结果
            └── 丢弃 candidate，last_successful 保持不变
```

一旦 `last_successful_analysis != NULL`，除 workspace 关闭或 LSP 退出外，不允许因为以下事件变回 `NULL`：

- `didOpen`、`didChange`、`didSave`、`didClose`；
- parser 或 semantic 报错；
- candidate 被取消或过期；
- manifest / dependency 刷新失败；
- provider 重建失败；
- 内存分配失败。

### 4.3 替换期间的短暂生命周期重叠

并发请求可能仍在读取旧分析。成功替换时允许旧对象和新对象短暂同时存活，但旧对象只用于完成已经开始的读请求，引用归零后立即释放。这是并发内存安全，不是多版本缓存。

推荐使用不可变 published 对象和引用计数：

```c
candidate = build_analysis(generation);
if (candidate != NULL && candidate->generation > published->generation) {
    old = atomic_exchange(&published, candidate);
    release(old);
} else {
    release(candidate);
}
```

若实现阶段继续使用单请求线程，也必须保持同样的“先成功构建、后替换”语义，禁止先 `session_dispose` 再构建。

### 4.4 Provider 使用相同事务语义

manifest、依赖解析和 symbol provider 也只能成功后替换：

1. 使用临时对象构建新 provider；
2. 完整加载依赖 bundle 和 workspace `.ft`；
3. 构建成功后替换旧 provider；
4. 构建失败时继续使用旧 provider。

禁止先释放 `cached_provider` 再尝试创建新 provider。

---

## 5. LSP Workspace 架构

推荐将当前集中在 LSP service 实现中的状态拆分为以下职责：

```text
FengLspService
├── ProtocolReader
├── RequestScheduler
└── WorkspaceRegistry
    └── FengLspWorkspace
        ├── DocumentStore
        ├── WorkspaceConfig
        ├── SymbolIndex
        ├── ParseCache
        ├── LastSuccessfulAnalysis
        └── BackgroundAnalyzer
```

### 5.1 DocumentStore

每个打开文档只保存当前版本：

- URI 与规范化路径；
- LSP document version；
- 当前文本；
- 行起始位置索引，用于 UTF-16 position 与 byte offset 转换；
- dirty 状态；
- 当前 parse generation；
- 最多一个当前版本 AST。

第一阶段可继续保存完整文本，但应把 `textDocumentSync.change` 从 Full 切换到 Incremental，并在 LSP 层应用 range edit，避免每次输入复制和传输整个文档。

### 5.2 WorkspaceConfig

workspace 生命周期内持久保存：

- manifest 路径与内容指纹；
- source roots；
- output / symbols root；
- 已解析依赖路径；
- package / module 映射；
- pointer size 等稳定编译选项。

交互请求不得向上遍历目录查找 manifest，不得重新打开 project，不得重新解析依赖。

### 5.3 SymbolIndex

符号查询使用分层视图，但不复制语义定义：

```text
current document declarations
        ↓ 覆盖
workspace .ft symbols
        ↓ 覆盖
dependency bundle symbols
```

请求方只消费统一查询接口，不关心符号来自哪一层。workspace `.ft` 缺失时仍可使用最后成功分析和依赖 bundle；不得在请求路径触发外部构建。

### 5.4 ParseCache

每个打开文档最多保存一个当前版本成功 AST：

- 文本版本变化后，旧 AST 不得作为当前版本精确 AST 使用；
- 当前版本解析成功后替换该文档 AST；
- 当前版本解析失败时可以保留上次成功语义缓存，但不能把旧 AST 的绝对 offset 直接应用到当前文本；
- completion repair 结果只服务单次请求，不进入长期历史缓存。

### 5.5 LastSuccessfulAnalysis

唯一成功语义缓存至少记录：

- workspace generation；
- 分析所使用的每个源文件内容或内容指纹；
- `FengLspAnalysisSession`；
- 与 session 同寿命的 imported module cache；
- 是否可用于指定文档的精确位置查询。

LastSuccessfulAnalysis 只读发布。请求处理不能修改其中 AST、semantic analysis 或 imported module 数据。

---

## 6. 文档版本与查询正确性

只保留最后一次成功分析时，必须明确“缓存存在”和“缓存与当前文本完全一致”是两个不同事实。

### 6.1 精确查询

当当前文档内容指纹与 `last_successful_analysis` 中对应源文件一致时，可以使用缓存 AST 的位置和语义事实精确查询。

### 6.2 当前文本查询

以下能力直接基于当前文本或当前 token，不依赖语义缓存版本：

- 关键字 Hover；
- 注解 Hover；
- 内建类型 Hover；
- 字符串、整数和浮点字面量 Hover；
- 关键字、注解和内建类型 Completion；
- import 路径前缀；
- 当前输入前缀和 Completion 上下文分类。

### 6.3 当前 AST 加持久符号索引

当前文档解析成功但完整语义分析尚未成功时，可以使用：

- 当前 AST 中的参数、局部绑定、显式类型注解和声明；
- 持久 workspace / dependency SymbolIndex；
- last successful 中与当前位置无关的稳定全局声明事实。

不得把旧 AST 的 source pointer、token offset 或局部 binding 指针应用到当前 AST。

### 6.4 无法安全对应时

如果查询依赖最新类型推导，而当前文档与最后成功分析不同且无法通过当前 AST 与稳定符号身份安全解析，则必须返回保守结果或 `null` / 空候选，等待后台分析成功。禁止为了避免空结果而返回可能错误的成员或类型。

### 6.5 错误恢复一致性

临时不完整输入不得使文档进入不可恢复状态。对于同一 LSP 会话中的编辑序列 `A → 不完整状态 B → 恢复状态 A`，恢复后的 Hover / Completion 结果必须与以下结果一致：

- 同一会话首次处于状态 A 时的结果；
- 新启动 LSP 后直接打开状态 A 时的结果。

成员补全至少覆盖 `foo. → 退格删除点号 → 再次输入 foo.`。每次成功应用 `didChange` 后，当前文本、version、UTF-16 position 映射、当前 parse 状态和单次 repair 输入必须属于同一文档版本。失败 parse、失败 candidate、已消费的冷启动等待状态和中间 generation 都不得阻止后续版本重新查询已发布缓存与索引。

---

## 7. Hover 优化路径

Hover 必须从低成本、与当前文本一致的查询开始：

```text
1. 解析 URI、position，取得当前 document 与 offset
2. 当前 token 分类
   ├── keyword
   ├── annotation
   ├── builtin type / alias
   └── integer / float / string literal
3. 当前版本 AST 查询
4. last_successful 精确语义查询（内容指纹一致）
5. 当前 AST + 持久 SymbolIndex 查询
6. 无安全结果时返回 null
```

约束：

- 步骤 2 命中后必须立即返回，不能先构建 analysis 或 provider。
- 字面量定位不得每次从文件开头扫描到光标；DocumentStore 应提供当前 token 索引或等价的局部定位能力。
- 步骤 3 至 5 只能读取已存在对象，不能触发 manifest、依赖或整项目分析。
- 现有功能文档定义的 Hover 文本和 Markdown 格式保持不变。

---

## 8. Completion 优化路径

Completion 必须以当前输入为中心，禁止把完整语义分析作为请求 fallback：

```text
1. 解析当前 CompletionContext
2. annotation / keyword / builtin type 等纯上下文候选
3. 当前 AST 的参数、局部绑定、self 和当前模块声明
4. 持久 SymbolIndex 的 workspace / dependency 候选
5. last_successful 中版本一致的类型事实和成员
6. 必要时仅对当前文本做现有 repair + 单文件 parse
7. 返回候选；后台分析独立进行
```

必须移除以下交互路径行为：

- Completion 请求内调用 `build_analysis_session`；
- repair Completion 请求内再次执行整项目分析；
- 同一文档版本重复打开 project、解析依赖或创建 provider；
- 因 `item_count` 未更新而丢弃已经生成的候选；
- 通过全局 `g_completion_uri` 保存请求状态。

### 8.1 Completion 候选计数

候选构建必须使用统一 append 接口，由接口同时完成：

- JSON 追加；
- 候选总数更新；
- label 去重；
- 可选来源和排序信息记录。

禁止调用方分别维护 JSON 是否有项和 `item_count`，避免两者再次不一致。

### 8.2 CompletionItem Resolve

初始 Completion 响应只返回排序和展示所需的最小字段。文档注释、完整签名、额外 import edit 等非首屏必要信息通过 `completionItem/resolve` 按需计算。

resolve 的 `data` 必须包含可重新定位候选的稳定信息，不得依赖“上一次 Completion 的第 N 项”或全局请求变量。

### 8.3 排序与质量

性能优化不得降低候选质量。统一候选模型应为未来排序保留以下字段：

- 当前作用域距离；
- 精确前缀 / 模糊匹配分数；
- local、current module、imported module、dependency 等来源；
- 静态 / 实例成员适用性；
- 可见性；
- 是否需要自动 import。

本阶段不在本文重复定义具体排序算法；排序行为应在后续独立功能规范中定义。

---

## 9. 后台分析与调度

### 9.1 请求分类

调度器至少区分：

| 优先级 | 请求 |
| --- | --- |
| 最高 | Completion、Hover、Signature Help |
| 高 | Definition、Prepare Rename |
| 中 | References、Rename |
| 低 | Diagnostics、workspace 索引刷新、完整 candidate 分析 |

### 9.2 文档变化

`didOpen` / `didChange` 只执行：

1. 更新当前文档文本、version 和行索引；
2. 使该文档当前 parse 状态失效；
3. 安排当前文件 parse；
4. `didOpen` 立即安排首次 candidate，连续 `didChange` / `didSave` debounce 后安排新的 candidate 完整分析；
5. 标记更旧、尚未开始的 candidate 任务为过期。

不得清除 `last_successful_analysis`。

### 9.3 Candidate 合并

连续输入期间只保留最新待分析 generation：

- 一个完整分析正在运行时，不再并行启动第二个完整分析；
- 中间 generation 只要尚未开始就直接丢弃；
- 正在运行的核心分析本阶段不强制中断；只要成功且 generation 新于已发布结果，即使当前文档已有更新 generation，也先替换 last successful；
- 已完成结果若不新于已发布 generation，则不得覆盖已发布结果；
- 最新 generation 在 debounce 后继续分析。

本阶段连续编辑与保存的 debounce 固定为 75ms，从最后一次 `didChange` / `didSave`
调度开始计算；首次 `didOpen` 不 debounce，以便 workspace 索引尽早在交互前发布。等待期间若出现更新 generation，只替换尚未开始的 candidate 并重新计时；
不得阻塞协议读取或交互请求。该值是 LSP 调度参数，不改变编译器行为。

### 9.4 协议读取与交互执行

协议读取不得被完整分析阻塞。实现阶段应将以下职责分离：

- 消息读取与 JSON-RPC request/cancel 登记；
- 交互请求执行；
- 后台完整分析。

所有跨线程共享对象必须不可变发布或通过明确锁保护。启用并行前必须审计现有 parser、semantic、symbol provider 公共调用的可重入性，并增加并发压力测试。

请求队列必须保持通知可见性边界：请求只能在它前面的通知已应用、它后面的通知尚未应用时执行。
优先级调整只允许发生在两个通知之间的连续请求区间内，禁止把后到的 `didChange` 提前到
已排队请求之前。协议读取线程只负责 framing、登记 request / cancel 和入队；唯一交互执行线程
负责修改 DocumentStore 与执行查询，后台分析线程只读取不可变 snapshot。

### 9.5 取消

- 收到 `$/cancelRequest` 后，尚未执行的请求立即从队列移除。
- 已开始的 LSP 自身扫描、候选构建和 JSON 构建必须定期检查取消状态。
- 已进入不可取消的核心分析调用时，完成后不得发布已经倒退的 generation。
- 取消属于正常控制流，不得记录为协议错误或清除缓存。

取消响应使用 JSON-RPC / LSP `RequestCancelled` 错误码 `-32800`。协议读取线程收到取消后应立即
标记对应 request；尚未执行的 request 从队列移除并立即响应，已开始的 request 在 LSP 自身的
扫描和结果构建边界检查标记。已经完成或未知的 request id 忽略。

---

## 10. Diagnostics

`didSave` 不得同步执行完整分析并阻塞后续交互请求。Diagnostics 与 candidate 分析共享后台结果：

1. candidate 成功时发布对应 generation 的 diagnostics；
2. candidate 因源码错误失败时，仍发布本次收集到的 parse / semantic diagnostics，但不替换 `last_successful_analysis`；
3. diagnostics 必须携带或内部关联文档 version，过期结果不得覆盖更新版本；
4. 发布 diagnostics 和发布成功语义缓存是两个独立动作。

首次启动尚无成功分析时，Hover / Completion 仍可使用当前文本、当前 parse 和 SymbolIndex，不等待 diagnostics。

---

## 11. 内存与生命周期

长期对象上限：

- 每个打开文档一个当前文本；
- 每个打开文档最多一个当前版本 AST；
- 每个 workspace 一个 `last_successful_analysis`；
- 每个 workspace 一个持久 SymbolIndex / provider；
- 每个 workspace 最多一个正在构建的 candidate。

替换瞬间允许旧 published 与新 candidate 短暂重叠。旧 published 在最后一个读者释放后立即回收。

每个 analysis session 必须持有其全部源码路径，路径生命周期覆盖 candidate 构建、发布、并发查询和最终回收。
同一源码对应的 `FengCliLoadedSource.path` 与 `FengProgram.path` 必须绑定到同一份由 session 持有的持久路径；
禁止在 session 发布后继续引用 document snapshot、project context 或其他临时构建对象中的路径。

可淘汰的派生数据，例如模糊搜索临时表、JSON buffer 和 repair AST，应使用明确容量或请求生命周期，不得通过清除 `last_successful_analysis` 回收内存。

---

## 12. 可观测性

必须为每个 LSP 请求记录可聚合的性能事件，默认不得污染 stdout 协议流：

- method；
- request id；
- document version；
- workspace generation；
- 总耗时；
- query path（text / current AST / last successful / symbol index / repair）；
- cache hit / miss；
- 候选数量或 Hover 是否命中；
- 是否取消或因 generation 过期丢弃；
- 是否发生同步 I/O；
- 是否错误进入完整分析。

性能日志必须通过显式 trace / benchmark 开关启用，并写入 stderr 或独立日志。生产默认模式只保留低开销计数器。

---

## 13. 分阶段实施

### Phase A：缓存正确性

- [x] 新增 workspace 级 `last_successful_analysis`。
- [x] 将 Hover 的破坏式重建改为 candidate 成功后替换。
- [x] provider 改为成功后替换。
- [x] 增加分析失败、取消、编辑后缓存仍存在的协议测试。

### Phase B：Hover / Completion 快路径

- [x] Hover 先处理当前 token 的关键字、注解、内建类型和字面量。
- [x] 增加 DocumentStore token / line 索引，避免 position 从文件开头扫描。
- [x] 修复 cached completion 候选计数。
- [x] 统一 completion item append、精确计数与 label 去重接口。
- [x] 禁止 Completion 请求同步调用完整 analysis。
- [x] 移除 `g_completion_uri`，改为显式 request context。
- [x] 保证 `foo. → 删除 → foo.` 等临时错误后的 Completion 恢复一致性。

### Phase C：持久 Workspace

- [x] manifest、project、dependency 和 provider 提升到 workspace 生命周期。
- [x] 实现 current parse、last successful、source module index 与 dependency provider 分层查询。
- [x] 每个文档只缓存当前版本 parse。
- [x] 支持 Incremental text synchronization。

### Phase D：调度与取消

- [x] 分离协议读取、交互执行和后台分析。
- [x] 实现请求优先级、debounce 和 generation 合并。
- [x] 实现 `$/cancelRequest`。
- [x] 完成核心公开 API 可重入性审计和并发压力测试。
- [x] 将 Diagnostics 切换为后台结果消费。

### Phase E：性能验收

- [x] 新增独立 LSP 协议性能基准，不修改既有测试用例。
- [x] 覆盖冷启动、热缓存、编辑后请求、连续输入和分析失败。
- [x] 执行编译器与 VS Code 插件全量回归测试并记录既有失败。
- [x] 将 §3 指标加入回归门槛。
- [ ] 在真实 VS Code 中确认不再出现可感知 Completion / Hover loading。

---

## 14. 预计代码组织

具体文件名可在实现前根据现有目录结构调整，但职责必须分离：

| 模块 | 职责 |
| --- | --- |
| `service.c` | JSON-RPC 分发和 capability，不继续承载全部缓存实现 |
| `workspace.*` | workspace 配置、generation 和长期对象所有权 |
| `document_store.*` | 当前文本、version、行索引、当前 parse |
| `analysis_cache.*` | last-successful / candidate 构建与原子替换 |
| `symbol_index.*` | current / workspace / dependency 分层查询 |
| `scheduler.*` | 优先级、后台任务、debounce、取消 |
| `hover.*` | Hover 查询编排 |
| `completion.*` | Completion context、候选模型、排序和响应 |
| `trace.*` | 性能计数和基准事件 |

不得为了减少文件数量继续把新增状态全部堆积在 `service.c`。

除 `src/cli/lsp/` 外，完整交付预计还需要修改：

| 文件或目录 | 原因 |
| --- | --- |
| `Makefile` | `TEST_CLI_SUPPORT_SRCS` 显式列出 `server.c`、`service.c`、新增 LSP 模块和 `main.c`；模块及线程链接选项必须同步 |
| `test/cli/` | 覆盖 last-successful、失败保留、generation、取消、Completion / Hover 快路径等协议行为 |
| `scripts/` 或新增性能测试目录 | 从 stdio 协议层运行真实 LSP 性能基准 |
| `docs/engineering/feng-lsp-performance-optimize.md` | 更新任务状态、实测结果和最终验收结论 |

VS Code 标准 Language Client 会根据 initialize capability 自动选择 Full 或 Incremental text synchronization。除非实测发现客户端适配或观测需求，预计无需修改 `editors/feng-vscode/` 的生产代码；仍需执行其全量回归测试。

---

## 15. 测试要求

### 15.1 正确性测试

新增测试必须覆盖：

- 首次成功分析后发生语法错误，Hover 仍可读取最后成功缓存；
- 新 candidate 失败不会释放 last successful；
- 更旧 generation 完成后不会覆盖更新 generation；
- provider 刷新失败继续使用旧 provider；
- 当前文本与缓存不同，不使用旧绝对 offset 返回错误局部符号；
- 内建类型和字面量 Hover 不进入 analysis path；
- Completion 已生成关键字 / 内建类型候选时不会错误进入慢路径；
- 连续 Completion 中被取消的旧请求不执行；
- `didSave` 不阻塞后续 Hover / Completion；
- completion resolve 不依赖全局上一次请求状态。
- `foo. → 删除 → foo.` 后成员 Completion 与首次输入及冷启动结果一致。
- 文本上下文已经确认是成员访问时，所有缓存、当前 parse 与 repair 路径只能返回该接收者的成员；
  未完成 AST 未形成成员表达式时必须继续成员修复路径，不得把全局符号当作有效结果提前返回。
- 成员 Completion 的文本上下文必须保留完整 receiver 表达式链。链模型必须统一支持字段访问、
  方法或函数调用、数组下标及其任意交错组合；例如 `app.screen.buffer().` 与
  `foo.bar.xyz[0].get().` 必须逐步传播每个字段类型、调用返回类型和下标元素类型，返回最终 owner
  的成员。参数或下标内部允许出现嵌套括号、字符串和注释，但不得在交互请求中触发完整语义分析。
  receiver 无法解析时返回空结果，不得退化为控制流关键字或其他非成员候选。

### 15.2 性能测试

性能基准必须从协议层启动真实 `feng lsp --stdio`，不能只测内部 helper。每个场景至少预热、重复采样并输出分位数。

测试编译结果只能放在工程 `build/` 或 `temp/`，不得输出到 `/tmp` 或 `/private/tmp` 后执行。

### 15.3 回归测试

变更完成后必须执行全量回归：

- 编译器 `test/`；
- Feng 兼容性测试 `fcts/`；
- VS Code 插件测试；
- 新增 LSP 性能基准。

依赖后台 candidate、workspace index 或 provider 发布结果的协议回归用例，必须先通过协议响应观察
所需状态已经发布，再执行原有行为断言。就绪同步必须设置有限重试次数，禁止用固定休眠时间推测
线程调度是否完成，也不得放宽发布后的 Hover、Definition、Completion 或诊断结果断言。

未经人工批准，不修改已有测试用例；性能覆盖通过新增测试或新增基准实现。

---

## 16. 完成交付标准

只有同时满足以下条件，本方案才能标记完成：

- [x] Hover 和 Completion 请求路径不存在同步整项目分析。
- [x] 交互请求路径不存在 manifest、依赖和 provider 重建。
- [x] last successful 只在更新分析成功后替换。
- [x] 一旦产生成功缓存，编辑、失败和取消不会使其意外消失。
- [x] 每个 workspace 不保存多个历史语义缓存。
- [x] 当前文本与旧缓存不一致时不会使用旧绝对位置返回错误结果。
- [x] Completion 候选计数与实际 JSON 项一致。
- [x] 临时不完整输入恢复后，Hover / Completion 与冷启动相同状态结果一致。
- [x] 请求取消、generation 合并和过期结果丢弃有效。
- [ ] §3 的全部性能门槛通过自动化基准。
- [ ] 全量回归测试通过。

### 16.1 2026-07-16 实施记录

本轮未修改核心编译器，已完成：

- 将 LSP `runtime.c/.h`、类型和函数统一重命名为 `service.c/.h`、`FengLspService` 和 `feng_lsp_service_*`；
- 增加后台 candidate 分析线程，只在完整分析成功且 generation 更新时替换唯一的 `last_successful_analysis`，失败分析不清除已有成功缓存；
- 将 workspace 源码模块索引与完整语义分析并行构建；dependency provider 在完整语义分析前发布，避免两者并发访问相同编译器缓存产物，并分别以 generation 保护发布；
- Hover 将关键字、注解、内建类型和字面量提升到当前文本快路径，字面量使用文档 token 索引；
- Completion 移除同步完整分析和全局请求 URI，使用当前 parse、已发布分析、源码模块索引和依赖 symbol provider；
- 支持 UTF-16 position 的 Incremental text synchronization；
- `didSave` 只同步执行当前文件 parse，完整 semantic / project diagnostics 消费后台 candidate 结果；
- 新增 `scripts/test/run_lsp_performance.py`，当时从真实 `feng lsp --stdio` 协议采样并检查 Hover P95、
  Completion P95 和交互 P99；这些历史验收口径现已由 §3 的 Hover 与 Completion 统一 Max 门槛取代。

在本机普通优化构建、`std/std_test/src/z_main.ff`、Hover 与 Completion 各 200 次采样下：

| 指标 | 实测 |
| --- | ---: |
| Hover P50 / P95 / P99 / Max | 0.018 / 0.030 / 0.041 / 0.098ms |
| Completion P50 / P95 / P99 / Max | 0.056 / 0.071 / 0.113 / 45.110ms |
| 全部交互样本 P99 | 0.098ms |

本轮没有修改既有 `test/cli/test_cli.c`。通过独立生成的 LSP-only 测试入口执行了其中全部既有 LSP 用例，结果通过；smoke 88 项、CLI direct / project、`std_test`、`fcts` 542 项和 perf constraints 也全部通过。仓库全量 UBSan 回归在既有 DAP 子进程测试处失败：`test/cli/test_cli.c:431` 收到 `process exited with status -1 (no such process)`，发生在 LSP 用例之前。VS Code 插件普通构建回归中 formatter、diagnostics、debug、syntax 通过，`debug-smoke.test.js:464` 与 `icon.test.js:39` 失败。由于全量回归未通过，本文状态保持“实施中”。

尚未完成：请求队列与 `$/cancelRequest`、debounce / 优先级调度、完整可重入性和并发压力审计、独立模块拆分、1 万 / 10 万 / 100 万行完整性能矩阵、真实 VS Code 体验验收。

### 16.2 2026-07-17 Completion 错误恢复修复

已修复 `foo. → 退格删除点号 → 再次输入 foo.` 后成员补全不能恢复的问题。根因不是失败 candidate 清除了最后成功语义分析，而是 JSON 字符串解码把合法空字符串 `""` 返回为 `NULL`；点号删除 edit 的 `text` 正是空字符串，导致该次 `didChange` 被拒绝，服务端保留旧点号，随后再次输入形成 `foo..`。

修复后 JSON 空字符串返回有效的空 C 字符串。新增 `scripts/test/test_lsp_completion_recovery.py`，通过真实 stdio 协议验证：

- 第一次输入点号能够返回成员；
- 删除点号的空字符串增量 edit 成功应用；
- 再次输入点号后的候选与第一次完全一致；
- Incremental change、Full change 与新启动 LSP 在相同文本和位置下返回相同候选。

该测试作为独立 LSP 协议回归工具保留，通过 `python3 scripts/test/test_lsp_completion_recovery.py` 手动执行，不作为正常 Makefile 工作流的依赖。修复后本机 200 次性能复测：Hover P95 0.036ms，Completion P95 0.084ms，全部交互样本 P99 0.104ms。

真实 VS Code 在未重启旧 LSP 进程时仍表现为修复前行为；执行 `Feng: Restart Language Server`
后恢复正常，确认新服务进程中的 `foo.` 成员补全可恢复。该现象不表示失败分析覆盖了最后成功缓存，
但说明本地替换 `feng` 可执行文件后必须重启既有 LSP 进程才能加载新实现。

### 16.3 最终实施收敛

本方案剩余实现按以下边界收敛，不扩展到核心编译器：

1. 交互路径取消每次请求都会进入的 10ms / 40ms 广泛冷启动等待；纯文本 Completion 在 parse / index 前返回，
   Signature Help、Completion Resolve、Definition、References 与 Rename 不再同步构建 project、
   dependency provider 或完整语义分析。仅当冷启动请求已经证明需要尚未发布的 import/provider 索引时，
   对首次冷查询和已解析程序的导入索引允许一次最大 16ms 的有界就绪窗口；初始 source-module 索引仍为最大 8ms，provider use-path 回退仍为最大 16ms。所有交互就绪窗口均不得超过 16ms；普通 `foo.`、字面量、热缓存和编辑后请求不进入该窗口。
2. workspace provider 与 source module index 只在首次缺失或显式刷新时后台构建，candidate 完整成功后
   替换旧 published 对象；普通输入只更新文档 snapshot 并 debounce 完整分析。
3. 协议读取、交互执行和后台分析分离；请求队列遵守 §9.4 的通知边界，支持优先级、取消、
   75ms debounce、generation 合并和过期结果保护。
4. DocumentStore 增加当前文本行索引；Completion 使用统一 builder 维护 JSON、数量和 label 去重。
5. 新增不进入正常 Makefile 工作流的独立 stdio 协议测试，覆盖失败缓存保留、取消、连续输入、
   并发压力以及 1 万 / 10 万 / 100 万行性能矩阵；正常 Makefile 工作流不得依赖 Python。

### 16.4 2026-07-17 最终验收结果

最终实现补充完成：

- Definition、References、Prepare Rename、Rename、Signature Help 和 Completion Resolve 请求不再同步执行完整分析、project 打开或 dependency provider 构建；
- `didOpen` 立即调度首次 workspace candidate，普通 `didChange` 使用固定 75ms debounce，只复制 debounce 后的最新文档 snapshot；
- 协议读取线程持续收包，interaction worker 按通知可见性边界和请求优先级执行，后台 analyzer 只发布更新 generation 的成功 candidate；
- queued cancellation 立即移除请求并返回 `-32800`，已开始的核心 semantic 调用按 §2.3 的边界在调用返回后观察取消；
- source module index、dependency provider 与 last successful 都采用 candidate 成功后替换；manifest、provider 或分析刷新失败保留旧 published 对象；
- dependency provider 与完整语义分析串行使用编译器缓存产物，避免并发构建产生文件级竞态；source module index 继续基于独立只读 snapshot 并行构建；
- `DocumentStore` 使用 UTF-16 行索引，Completion builder 使用请求内 label hash、精确 item count 和显式 request context；
- `FENG_LSP_TRACE=1` 可输出 method、id、version、generation、duration、query path、cache hit、cancel、同步 I/O 和完整分析标记，默认关闭。

普通优化构建的最终协议性能结果：

| 指标 | 最终实测 |
| --- | ---: |
| `std_test` Hover P50 / P95 / P99 / Max | 0.027 / 0.036 / 0.048 / 0.057ms |
| `std_test` Completion P50 / P95 / P99 / Max | 0.021 / 0.029 / 0.029 / 0.031ms |
| Definition P95 | 0.340ms |
| queued cancellation 观测 | 0.135ms |
| 1 万 / 10 万 / 100 万行矩阵全部交互 P99 / Max | 0.229 / 0.362ms |
| 100 万行后台分析 CPU | 0.045s |
| 100 万行进程 RSS | 36704KiB |

专项测试全部通过：

- `scripts/test/test_lsp_completion_recovery.py`；
- `scripts/test/test_lsp_cache_retention.py`；
- `scripts/test/test_lsp_scheduler.py`，包括 200 个低优先级 References、Hover 抢占和 queued cancellation；
- `scripts/test/run_lsp_performance.py`；
- `scripts/test/run_lsp_performance_matrix.py`；
- 独立 LSP-only 入口执行 `test/cli/test_cli.c` 中全部既有 LSP 用例，连续三次通过，未修改既有测试文件。

仓库回归结果：编译器 archive、lexer、parser、semantic、runtime、codegen、debug、symbol 单元测试通过；smoke 88 项、CLI direct / project、`std_test`、`fcts` 542 项和 perf constraints 全部通过。`make test-sanitize` 仍在既有 DAP 子进程断言 `test/cli/test_cli.c:431` 失败，错误为 `process exited with status -1 (no such process)`，发生在 LSP 用例之前。VS Code formatter、diagnostics、debug、syntax 通过；既有 `debug-smoke.test.js:464` 和 `icon.test.js:39` 失败。上述失败不在本轮 LSP 变更路径内，因此不修改其测试或生产实现。

真实 VS Code 已确认重启 LSP 后 `foo. → 删除 → foo.` 恢复；“不再出现可感知 loading”仍需开发者用本次最终二进制重启 LSP 后完成最后一项人工体验确认。

### 16.5 2026-07-17 成员候选范围修复

已修复不完整成员访问可能返回全局候选的问题。根因是普通 AST Completion 已经以当前文本识别的成员上下文为准，
但 persistent symbol cache 路径仍只通过未完成 AST 判断成员访问；当 parser recovery 没有在光标处保留
member expression 时，该路径会进入非成员分支，并把 `Action`、`args`、`assert` 等全局符号当作有效结果提前返回。

修复后，persistent symbol cache 与普通 Completion 使用相同的当前文本成员上下文。在 AST 缺少 member expression 时，
缓存路径以文本中识别出的 receiver 构造只读查询表达式，继续解析局部绑定的显式类型、对象字面量初始化类型、
类型构造调用、`self`、模块别名和内建字面量；成员上下文不再进入全局候选分支。该修复只修改
`src/cli/lsp/service.c`，不修改 parser、semantic 或其他核心编译器模块。

新增 `scripts/test/test_lsp_member_completion_scope.py`，通过真实 stdio 协议逐字符输入 `user.`，验证：

- `let user = User { name: "alice", age: 20 };` 后输入 `user.` 返回 `name`、`age`；
- 结果不包含 `Action`、`args`、`assert`、`break` 等全局候选；
- 与既有 `scripts/test/test_lsp_completion_recovery.py` 一起执行时，删除点号再输入的恢复行为保持通过。

修复后普通优化构建性能复测：Hover P95 0.040ms，Completion P95 0.310ms，全部交互样本 P99 0.571ms；
1 万 / 10 万 / 100 万行矩阵全部交互 P99 0.161ms、Max 0.169ms，继续满足 §3 门槛。

本次普通与 UBSan 全量回归均运行到既有 DAP 子进程断言 `test/cli/test_cli.c:431` 后停止；在此之前，
archive、lexer、parser、semantic、runtime、codegen、debug、smoke 88 项、CLI direct / project、`std_test`、
`fcts` 542 项和 perf constraints 通过。VS Code formatter、diagnostics、debug、syntax 通过；既有
`debug-smoke.test.js:464` 与 `icon.test.js:39` 仍失败。未修改上述既有失败对应的生产代码或测试。

### 16.6 2026-07-17 链式成员补全修复

已修复 `app.screen.` 错误返回 `break`、`catch` 等非成员候选的问题。根因是文本 Completion context
只保留了点号前最后一段 `screen`，丢失完整 receiver `app.screen`；同时未解析出 owner 时仍会进入
关键字兜底。修复后文本 context 保留完整 receiver chain，缓存与当前分析查询均逐段解析局部变量、
字段类型和方法返回类型；链式 receiver 未解析成功时返回空结果，不再退化为关键字或全局候选。

本地源码依赖尚未进入 dependency symbol provider 时，查询可使用后台已发布的 source module index
继续解析类型。该路径只读取内存中的已发布对象，不在 Completion 请求中打开项目、读取文件或触发完整分析；
source module index 尚未发布时返回空结果，发布后后续请求自动恢复。

新增 `scripts/test/test_lsp_chained_member_completion.py`，通过真实 stdio 协议在
`examples/tui_demo/src/main.ff` 中输入 `app.screen.`，验证结果包含 `buffer`、`size`、`resize`，且不包含
`Action`、`args`、`break`、`catch`、`continue`、`defer`。普通优化构建下，链式成员、成员范围、补全恢复、
缓存保留和调度器专项测试均通过；本次调度器复测 Definition P95 为 0.110ms。

普通与 UBSan 全量回归仍只在既有 DAP 子进程断言 `test/cli/test_cli.c:431` 停止，未出现 sanitizer 报告；
VS Code 测试仍停在既有 `debug-smoke.test.js:464`。上述失败不在本次 LSP Completion 变更路径内。

### 16.7 2026-07-17 混合 receiver 链补全修复

已将上一节的纯标识符点链提升为统一的轻量 receiver 表达式链。文本层从最终成员点号向前提取完整
receiver，并解析为根值以及 member、call、index 三种后缀操作；括号内容支持嵌套、字符串、行注释和
块注释。AST/source-index 与 symbol-index 查询分别执行同一组操作语义：字段访问传播字段类型，调用
传播函数或方法返回类型，下标仅从数组类型剥离一层元素类型。未调用的方法不会被错误地当成返回值。

该实现只读取当前文档、最后成功分析和已发布索引，不修改 parser、semantic 或其他核心编译器模块，
也不在 Completion 请求中执行项目 I/O 或完整语义分析。任一步骤无法确定类型时仍返回空结果，不进入
全局候选或关键字兜底。

新增 `scripts/test/test_lsp_mixed_receiver_completion.py`，通过真实 stdio 协议验证：

- `app.screen.buffer().` 返回 `Buffer` 的 `width`、`height`、`cells`、`draw`、`fill`、`clear`；
- `foo.bar.xyz[(0)].get(/* nested ) ] */).` 依次经过字段、数组下标、方法调用后返回 `Leaf` 的
  `leafMarker`、`get`；
- `foo.bar.xyz[0].get.` 不会把未调用方法错误地解析成其返回类型；
- 有效链结果均不包含 `Action`、`args`、`break`、`catch`、`continue`、`defer`。

普通优化构建性能复测：Hover P95 0.037ms，Completion P95 0.040ms，全部交互 P99 0.042ms；
1 万 / 10 万 / 100 万行矩阵全部交互 P99 0.179ms、Max 0.247ms。普通与 UBSan 全量回归仍只在既有
DAP 子进程断言 `test/cli/test_cli.c:431` 停止，未出现 sanitizer 报告；VS Code 测试仍停在既有
`debug-smoke.test.js:464`。

### 16.8 2026-07-17 receiver 起始边界修复

已修复成员 receiver 前存在表达式分隔空白时，空白被错误纳入 receiver 文本的问题。例如
`let status: HttpStatus = HttpStatus.;` 中，右侧 `HttpStatus` 必须识别为简单类型名，不得因为其前面的
空白被误判为复杂链。receiver 反向扫描只允许跨越点号两侧的空白；未遇到前序点号时，必须保留当前
原子表达式的真实起始位置。

该修复不增加枚举或类型名特判，统一适用于标识符、调用、下标和字符串字面量组成的 receiver 链。
既有枚举不完整成员访问测试必须同时覆盖 `HttpStatus.` 与 `HttpStatus.N`，分别返回全部枚举项和前缀
匹配项。

普通构建与 UBSan 下的参数 Hover、参数缓存失效和枚举成员补全专项测试均通过；成员范围、链式与混合
receiver、补全恢复、缓存保留和调度器协议测试通过。普通优化构建复测 Hover P95 为 `0.049 ms`，
Completion P95 为 `0.041 ms`；1 万 / 10 万 / 100 万行矩阵交互 P99 为 `0.197 ms`，Max 为
`0.234 ms`。普通与 UBSan 全量回归仍只在既有 DAP 子进程断言 `test/cli/test_cli.c:431` 停止，
未出现 sanitizer 报告。

### 16.9 2026-08-24 Hover 与 Completion 性能门槛统一

经人工确认，所有 Hover 请求与所有 `textDocument/completion` 请求统一执行 §3 的 `Max ≤ 16ms` 硬门槛，
原先按查询类别设置的 Hover P95 和 Completion P95 门槛不再作为验收规范。本文此前记录的 P50、P95、
P99 和 Max 实测值均作为历史性能观测保留，不改变其事实含义；其中 §16.1 的 Completion Max
`45.110ms` 是旧口径下的历史结果，不是当前门槛的通过证据。

推导 callable 返回类型修复交付后，`scripts/test/run_lsp_performance.py` 与
`scripts/test/run_lsp_performance_matrix.py` 的所有既有 Hover 场景已经改为强制 `Max ≤ 16ms`；场景
输入、候选预期和固定等待未修改。新增
`scripts/test/run_lsp_inferred_callable_performance.py`，对五类 callable Hover、相关调用结果
Completion、错误/恢复状态及外部 symbol Hover 强制同一 Max 门槛，每个热场景至少 200 个样本。

一次交付复测中，新增专项全部样本总 Max 为 `1.822ms`，1 万 / 10 万 / 100 万行既有矩阵总 Max 为
`0.256ms`，沙箱外 `make test` exit 0。普通 Completion 的两个既有脚本仍保留旧 P95 断言，因此
“所有 Completion 场景统一 Max”的全局自动化补齐仍待后续独立交付；本次事实和范围详见
[推导 callable 返回类型提示修复方案](feng-lsp-inferred-callable-return-type-bugfix-pending.md)。

---

## 17. 后续核心编译器演进方向

本阶段完成后，若要进一步达到脏代码语义质量和大型项目 CPU 效率的第一梯队，可在独立方案中评估：

1. parser 容错 AST；
2. semantic 返回带诊断的部分可查询分析；
3. 稳定 ModuleId / DeclId / BodyId；
4. 声明、函数体和依赖边级增量失效；
5. 核心分析内部取消。

这些内容不属于本文实施范围，不得在本方案实现中顺带修改。

---

## 18. 参考架构

- [clangd Design](https://clangd.llvm.org/design/)
- [clangd Indexing](https://clangd.llvm.org/design/indexing)
- [rust-analyzer Architecture](https://rust-analyzer.github.io/book/contributing/architecture.html)
- [gopls Implementation](https://go.dev/gopls/design/implementation)
- [TypeScript Compiler API：Incremental Language Service](https://github.com/microsoft/TypeScript/wiki/Using-the-Compiler-API)
- [Language Server Protocol 3.18](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.18/specification/)
