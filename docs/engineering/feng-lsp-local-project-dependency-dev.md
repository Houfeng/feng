# Feng LSP 工作区多成功分析缓存方案

> 状态：待 Review，尚未实施。
>
> 关联文档：
>
> - [Feng LSP 已交付方案](feng-lsp-delivered.md)：定义既有 LSP 功能行为。
> - [Feng LSP 性能优化方案](feng-lsp-performance-optimize.md)：定义后台分析、成功后替换和请求性能基线。
> - [Feng DAP 开发方案](feng-dap-dev.md)：定义 `PKG_NAME://<package-relative path>` 的 DAP 映射。
>
> 本文档是本地项目依赖 Definition、References、Rename 和 Implementation 的主方案。

---

## 1. 最终结论

本次优化采用最小模型：把 `FengLspService` 中单个 `last_successful_analysis` 改为工作区生命周期内的
`last_successful_analysis[]` 动态数组。

```text
当前：

FengLspService
└── last_successful_analysis

修改后：

FengLspService
└── last_successful_analyses[]
    ├── 项目 A 最后一次成功 FengLspAnalysisSession
    ├── 项目 B 最后一次成功 FengLspAnalysisSession
    └── 项目 C 最后一次成功 FengLspAnalysisSession
```

每个 `FengLspAnalysisSession` 已经包含该项目全部源码文件的 source、AST 和项目级 Semantic
Analysis。因此不增加第二份文件缓存，不把工作区所有文件合并进同一次 Semantic Analysis，也不增加
复杂 `ProjectState`。

统一查询规则：

1. 普通请求从全局成功分析数组中查找包含请求文件的 session；
2. Definition、References、Rename 和 Implementation 均遍历全局成功分析数组；
3. 同项目与跨项目不使用两套流程，同项目只是全局遍历命中了当前 session；
4. 每次请求都先返回当前内存缓存能够证明正确的结果，不同步等待分析；
5. 数组未命中文件或缓存不再精确时，只向后台队列提交任务，由 analyzer 按现有逻辑查找最近
   `feng.fm` 并重建所属项目；
6. 项目切换和 `didClose` 不删除成功 session，只有该项目的新成功 candidate 才替换自己的旧 session；
7. parser、semantic 和 frontend 分析主体继续使用现有实现。

同一 manifest 无论成功重建多少次，数组中始终只有一个元素；既有 LSP 性能标准全部保持不变。

生产代码变更仅限 `src/cli/lsp/`，最小实现只修改 `src/cli/lsp/service.c`。测试只允许增加新用例，
不得修改已有测试用例。

---

## 2. 当前代码事实

### 2.1 `last_successful_analysis` 目前没有缓存 key

当前 `FengLspService` 直接持有：

```c
FengLspAnalysisSession last_successful_analysis;
size_t last_successful_generation;
```

它不是 Map，也不是数组。`FengLspAnalysisSession.manifest_path` 只记录来源 manifest，当前没有作为
缓存索引使用。

后台 candidate 成功后直接执行：

```c
previous = service->last_successful_analysis;
service->last_successful_analysis = candidate;
```

因此项目 B 的成功分析会覆盖项目 A 的成功分析。当前问题的本质不是显式“按项目清缓存”，而是整个
LSP 服务只有一个成功分析槽位。

### 2.2 一个 session 已经保存一个项目的所有文件解析结果

`FengLspAnalysisSession` 已包含：

```c
FengSemanticAnalysis *analysis;
FengCliLoadedSource *sources;
size_t source_count;
char *manifest_path;
FengSymbolImportedModuleCache *imported_module_cache;
```

每个 `FengCliLoadedSource` 包含：

- 物理 source path；
- source text 和长度；
- `feng_parse_source()` 产生的 `FengProgram` AST。

项目分析由 `feng_cli_project_open().source_paths` 决定输入文件。因此一个成功 session 本身已经是：

```text
manifest
├── sources[]
│   ├── file path + text + AST
│   ├── file path + text + AST
│   └── file path + text + AST
└── Semantic Analysis
```

不需要再把每个文件的 AST 复制到单独的 workspace source cache。

### 2.3 当前命中方式是 path + text，不是 manifest key

`analysis_matches_document()` 当前执行：

1. `find_source(session, document->path)`；
2. 比较成功 session 中的 source text 与 IDE 当前 text 是否完全相同。

因此修改为数组后，正常请求只需依次对每个成功 session 调用现有 `find_source()` /
`analysis_matches_document()`。不需要每次请求向上查找 `feng.fm`。

### 2.4 本包 References/Rename 已经遍历 session 中全部文件

当前本包 References 的核心流程是：

```text
last_successful_analysis
→ find_program(document path)
→ resolve_target_at()
→ collect_references()
→ 遍历 session.sources 中全部 AST
```

Rename 复用同一个 `collect_references()` 结果生成 `WorkspaceEdit`。单次请求不会重新全量分析项目。

当前 `resolved_targets_equal()` 使用 `FengDecl *`、`FengTypeMember *` 等 AST 指针相等判断同一符号。
这在一个 session 内成立；跨 session 时，同一导入符号可能拥有不同 AST 地址，因此全局聚合时必须增加
一次稳定身份适配，不能只把不同 session 的 target 指针直接互相比较。

### 2.5 本地依赖目前不是 `sources` 中的物理源码

`build_project_session()` 只把当前 `feng.fm` 的 `source_paths` 交给 frontend。本地依赖通过
`feng_cli_deps_resolve_for_manifest()` 得到 `.fb`，再由 imported-module cache 合成外部 AST。

因此项目 A session 中的 B 声明和项目 B 自身 session 中的源码声明指针不同。多成功分析数组解决了
B 源码 session 被覆盖的问题；跨 session 稳定符号身份解决两份 AST 之间的关联问题。

### 2.6 `#line` 不是该问题的数据源

`PKG_NAME://<package-relative path>` 由 Codegen 写入 C `#line`，服务于 DAP。LSP 的上述流程使用
source path、AST、Semantic Analysis 和 Symbol metadata，不读取生成 C 的 `#line`。

本方案禁止修改 IR、Codegen、DAP 或 `#line` 路径。

### 2.7 当前交互请求已经采用内存缓存优先

当前 Definition、References、Prepare Rename 和 Rename 均先用
`analysis_matches_document()` 检查已发布的 `last_successful_analysis`，命中后直接查询；不能精确命中
时才使用 current parse 或既有派生索引的安全 fallback。Hover 先执行更便宜的 current-text 精确快路径，
再尝试已发布分析；Completion 同样优先消费 current text、已发布 module/symbol index 和成功分析。

`build_analysis_session()` 只由 `background_analyzer_main()` 调用，`didOpen` / `didChange` / `didSave`
均通过 `schedule_background_analysis()` 提交完整分析 candidate；`didSave` 的同步工作只保留既有当前文件
parse diagnostics。少数绝对冷启动查询允许等待一个既有的有界发布窗口，但请求线程不执行完整项目分析。
因此本方案不是引入新的请求策略，而是把现有单成功缓存策略等价扩展到多个项目。

### 2.8 当前项目归属与项目切换

当前 `FengLspService` 没有 `current_project`、workspace root → project 或 document → manifest 的持久字段。
项目归属只在后台任务构建 candidate 时按本次 primary document 确定：

```text
didOpen / didChange / didSave 的 document URI
→ `FengLspDocument.path` 物理路径
→ `feng_cli_project_find_manifest_in_ancestors()`
→ 从文件所在目录逐级向上查找最近的 `feng.fm`
→ `feng_cli_project_open()` 得到该项目 source paths、依赖与 package path
```

`build_overlays()` 对每个已打开 document 再执行同样的最近 manifest 判断，只把与 candidate manifest
相同的 overlay 交给本次 frontend。`build_symbol_index_candidate()` 和
`build_module_index_candidate()` 也从各自 primary document 向上查找 manifest。

当前在不同本地项目间切换时实际发生的是：

1. 第一次打开项目 A 文件，`didOpen` 调度 A candidate；成功的 analysis、symbol index 和 module index
   分别发布到各自唯一槽位；
2. 随后打开或编辑项目 B 文件，相应通知调度 B analysis candidate；需要刷新 index 时也构建 B index。
   各 candidate 成功后按各自的全局 generation 替换对应的 A 单槽缓存；
3. IDE 只切换两个已经打开的编辑器标签时，标准 LSP 不发送“当前项目切换”通知，服务不会因此重建；
4. 此时请求只用请求文件 physical path 在唯一成功 session 中查找；找不到或 text 不匹配时，进入
   current parse/单份派生 index fallback，而不会在请求线程重新向上查 manifest。

因此当前不是“切换时主动定位项目”，而是“最近一次成功后台任务隐式决定单缓存属于哪个项目”。本方案
取消这个隐式当前项目：请求按 physical path 从多成功数组选择 origin session；只有缓存缺失后进入后台
candidate 时，才继续复用现有最近 `feng.fm` 定位逻辑。

---

## 3. 变更边界

### 3.1 允许变更

- `src/cli/lsp/service.c` 中的成功分析缓存、后台任务存储和请求聚合；
- 如实际编译需要，允许修改 `src/cli/lsp/` 下既有私有声明；
- 在 `test/` 中增加新用例和 fixture，不改变任何已有用例的输入、断言或 expected 文件。

### 3.2 禁止变更

- 禁止修改 `src/cli/lsp/` 之外的生产代码；
- 禁止修改 lexer、parser、semantic、symbol、codegen、runtime 和 DAP；
- 禁止修改核心编译器符号查询接口；
- 禁止修改 `.ft` / `.fb` 格式或读写实现；
- 禁止过滤、替换或改变非本地 package 的 resolved bundle/provider/imported-module 查询链路；
- 禁止修改 IR 和 `#line`；
- 禁止把多个项目源码合并成一次编译器 Semantic Analysis；
- 禁止为特定包名、模块名或目录增加特判。

---

## 4. 最小数据结构

### 4.1 成功分析数组

只增加一个薄包装：

```c
/* One project's latest successful LSP analysis retained for the service lifetime. */
typedef struct FengLspWorkspaceAnalysis {
    FengLspAnalysisSession last_successful_analysis;
    size_t last_successful_generation;
} FengLspWorkspaceAnalysis;
```

`FengLspService` 中把单值替换为数组：

```c
FengLspWorkspaceAnalysis *last_successful_analyses;
size_t last_successful_analysis_count;
size_t last_successful_analysis_capacity;
```

数组元素不重复保存 `manifest_path` 或 source paths；这些信息已经由
`FengLspAnalysisSession` 拥有。

为跨 session 身份关联，`FengLspAnalysisSession` 只补充一个 LSP 私有字段：

```c
char *package_path;
FengSymbolProvider *source_symbol_identity_index;
```

该路径直接复制 `feng_cli_project_open()` 已经计算出的 `context.package_path`，用于把 imported synthetic
program 的 bundle 来源映射回本地成功 session；它不改变项目、Symbol 或 `.fb` 接口。

`source_symbol_identity_index` 在 analyzer 线程中由成功 session 已有的 `FengSemanticAnalysis` 调用现成的
`feng_symbol_build_graph()` / `feng_symbol_provider_add_graph()` 构建，仅保留该项目物理源码的 workspace
符号视图。它复用 Symbol 写入前已经分配的 module-local symbol id，不读取或修改 `.ft` / `.fb`，也不改变
任何核心编译器查询接口；session 替换或释放时与 analysis 一同释放。该索引不参与普通 Hover、Completion
或编译，只用于把 imported `(module_name, symbol_id)` 精确映射到物理 source path/token。

### 4.2 数组元素身份

项目 session 使用规范化 `session.manifest_path` 作为身份：

```text
canonical manifest path → array element
```

standalone file 的 `manifest_path == NULL`，使用其唯一 `sources[0].path` 作为身份。项目 package name
不能作为 key，因为工作区可能存在同名项目。

### 4.3 不增加独立全局 source 所有权

不采用以下结构：

```c
GlobalSourceEntry {
    source;
    program;
    owner_project;
    analysis;
}
```

原因是 Semantic Analysis 内部引用 `sources[].program` 的 AST。把 source 所有权拆出 session 会扩大
生命周期、替换和释放逻辑。最小实现直接遍历 session 数组中的 `sources`。

如果实测线性查找无法满足既有性能门槛，可以在后续单独增加不拥有 AST 的
`physical path → (analysis index, source index)` 派生索引；本阶段不预先增加。

---

## 5. 缓存查找、发布与生命周期

### 5.1 按文件查找

增加统一 helper：

```c
/* Finds the newest successful analysis containing the physical source path. */
static FengLspWorkspaceAnalysis *
find_workspace_analysis_by_source_path(FengLspService *service,
                                       const char *path,
                                       const FengLspDocument *document);
```

规则：

1. 遍历 `last_successful_analyses[]`；
2. 使用现有 `find_source(session, path)`；
3. 精确查询优先选择 `analysis_matches_document(session, document)` 的元素；
4. 若历史归属变化造成多个旧 session 都包含同一路径，选择 generation 最新且文本匹配的元素；
5. 没有命中才进入缓存建立流程。

因此热请求没有 manifest 查找和磁盘 I/O。

### 5.2 缓存未命中

请求线程未命中时只把 document URI 和 generation 合并进 pending 队列，优先立即使用当前文本安全
fallback 或返回该协议允许的空结果；仅绝对冷启动可以按 §5.5 沿用既有有界发布等待。请求线程不查
manifest、不读项目源码，也不调用 frontend。

analyzer 线程取出任务后继续使用现有逻辑：

```text
document
→ build_analysis_session()
→ feng_cli_project_find_manifest_in_ancestors()
→ build_project_session() 或 build_standalone_session()
```

manifest 归属只在 candidate 构建阶段确定一次。candidate 成功发布后，后续请求通过全局数组中的
`session.sources` 直接命中。

打开依赖方项目时，现有 `module_index_scan_project()` 已递归访问本地依赖项目及其物理 source。
在这次既有遍历中，对尚未缓存、也未 pending 的本地依赖 manifest 各追加一个后台分析任务，使依赖
session 在用户首次 Definition/References/Rename 前完成预热；不扫描依赖闭包之外的 repo。若查询时
仍遇到未命中的本地 source，则再次按该物理 source path 调度现有分析，查询线程自身不执行分析。

### 5.3 按身份发布

后台重建期间，candidate 与数组中已发布的旧 session 必须拥有完全独立的生命周期。开始重建、正在解析、
正在执行 Semantic Analysis 或生成 diagnostics 时，均禁止清空、移动或修改旧 session。所有请求继续读取
旧 session 中仍可按 §5.5 证明正确的结果，不得出现“旧缓存已销毁、新缓存尚未发布”的空窗期。

只有 candidate 同时满足以下条件才允许发布：

1. `build_analysis_session()` 成功完成；
2. `candidate.exit_code == 0`；
3. `candidate.analysis != NULL`；
4. candidate 没有被取消或判定过期；
5. candidate generation 新于同一 manifest/standalone 元素的已发布 generation。

满足发布条件后：

1. 项目 session 按 `candidate.manifest_path` 在数组中查找元素；
2. standalone session 按 `candidate.sources[0].path` 查找元素；
3. 命中则在 `analysis_mutex` 内以结构体交换原子替换该元素中的旧 session 和 generation；
4. 未命中则追加数组元素；
5. candidate 失败、取消或过期时不修改任何已有元素；
6. 锁内只完成发布交换，不在锁内释放旧 session；
7. 交换完成并解除锁后，才释放已经从数组移出的旧 session。

替换路径不重新分配数组，也不先把元素置空，核心操作保持为 O(1)：

```c
pthread_mutex_lock(&service->analysis_mutex);
previous = entry->last_successful_analysis;
entry->last_successful_analysis = candidate;
entry->last_successful_generation = candidate_generation;
memset(&candidate, 0, sizeof(candidate));
pthread_mutex_unlock(&service->analysis_mutex);

session_dispose(&previous);
```

所有 session 读取者继续遵守既有 `analysis_mutex` 生命周期：必须在持锁期间完成对 session/AST 的读取，
禁止把数组元素、session、AST 或 Semantic Analysis 的借用指针带到解锁之后。由此，锁内交换完成前的请求
看到完整旧 session，交换完成后的请求看到完整新 session，不存在可观察的中间状态。

数组计数遵守以下不变量：

```text
同一 manifest 首次成功：count += 1
同一 manifest 后续成功：count 不变，原位替换
同一 manifest candidate 失败：count 不变，旧元素不变
新的 manifest 首次成功：count += 1
```

禁止把每次 candidate 当作新元素追加，否则数组会退化成历史版本缓存并造成查询歧义。

不同项目 generation 不能使用一个全局“新 generation 覆盖旧 generation”的判断。发布时只比较同一
数组元素的 generation，避免项目 B 的较新全局文档序号阻止项目 A 首次发布。

### 5.4 生命周期

- `didOpen` / `didChange` / `didSave`：更新文档 overlay 并调度对应 candidate，不删除成功数组元素；
- `didClose`：清除该文档 overlay 和诊断，不删除其成功 session；
- 切换编辑文件或项目：不改变成功分析数组；
- candidate 失败：保留该项目旧成功 session；
- service shutdown：循环 `session_dispose()` 后释放数组。

工作区在本方案中等于一个 LSP service 进程的生命周期，不需要为缓存新增 workspace root 扫描。
数组只包含 IDE 实际打开/请求过并成功分析的项目，不主动扫描整个 repo。

### 5.5 缓存优先请求状态机

所有需要分析状态的 LSP 请求必须遵守“读缓存在前、是否重建在后”的固定顺序。这里的“缓存”包括
current document 的 text/token/parse、已发布 symbol/module index 和成功 semantic session；各协议继续保留
现有更便宜且精确的 current-text 快路径，本方案不强制把 semantic session 移到这些快路径之前。

| 当前状态 | 本次请求响应 | 后台动作 |
| --- | --- | --- |
| 找到与当前 document text 完全匹配的成功 session | 立即返回精确缓存结果 | 该项目没有更高 generation 时不重建；否则只复用/补充最新任务 |
| 当前请求位置仍处于既有 successful prefix | 立即返回该位置可证明正确的缓存结果 | 当前 generation 尚未 pending/running 时调度 |
| exact session 不匹配，但当前 parse/派生索引可安全回答 | 立即返回现有 current-text fallback | 调度所属项目最新 generation |
| 只有不兼容的旧成功 session | 不返回旧版本的精确结果；按协议返回安全 fallback、`[]` 或 `null` | 调度所属项目最新 generation |
| 完全没有该文件的成功 session | 可保留既有冷启动短时发布等待；超时后立即返回安全 fallback、`[]` 或 `null` | 新增所属项目后台任务 |
| 相同或更新 generation 已 pending/running | 按以上缓存规则立即响应 | 不重复调度 |

请求处理的伪代码固定为：

```c
cached = find_workspace_analysis_by_source_path(service, document->path, document);
response = try_build_response_from_cache(cached, document, request);
if (response == NULL && workspace_query_is_absolutely_cold(service, document)) {
    wait_for_existing_bounded_publication(service, document);
    cached = find_workspace_analysis_by_source_path(service, document->path, document);
    response = try_build_response_from_cache(cached, document, request);
}
if (workspace_analysis_needs_refresh(cached, document) &&
    !workspace_analysis_is_already_scheduled(service, document)) {
    schedule_background_analysis(service, document, ...);
}
return response != NULL ? response : safe_empty_response(request);
```

“缓存优先”不等于返回已知错误的 stale 结果。现有 `analysis_matches_document()`、
`analysis_position_matches_document()`、current parse 和 current-text symbol/module index 的精确性判断继续
生效。重建只影响后续请求，不能阻塞、替换或延迟本次已经可由缓存回答的请求。

现有 `wait_for_initial_query_state()`、`wait_for_initial_module_index()` 和
`wait_for_initial_symbol_index()` 只能用于绝对冷启动且没有可用缓存/fallback 的场景，并继续受既有
16ms/8ms 上限约束。只要任一可证明正确的内存结果已经存在，请求就必须立即返回，禁止进入等待；这些
等待只给已在后台构建的首个 candidate 一次发布机会，绝不允许在请求线程执行或启动 frontend。

是否触发重建只由缓存状态决定：当前 text 精确命中且没有更高 generation 时不触发；document generation
高于该项目已发布 generation、只有 successful prefix、缓存缺失、所属 manifest/依赖配置发生变化或既有
bundle path 失效时才调度。相同或更新 generation 已经 pending/running 时只复用该任务，不重复入队。

---

## 6. 后台分析调度

解析和 Semantic Analysis 主体不改：

- 继续使用 `analysis_task_clone()`；
- 继续使用 `build_analysis_session()`；
- 继续使用 `build_project_session()` / `build_standalone_session()`；
- 继续使用 `feng_cli_frontend_run_with_overlays()`；
- 继续使用一个 analyzer worker；
- 继续采用 candidate 成功后替换、失败保留旧结果。

当前 scheduler 只有一个 `pending_analysis_uri`，连续编辑不同项目时后一个任务会覆盖前一个任务。
多成功分析缓存下，需要把 pending storage 改成一个小型数组/队列：

```c
typedef enum FengLspPendingAnalysisTargetKind {
    FENG_LSP_PENDING_DOCUMENT,
    FENG_LSP_PENDING_PROJECT
} FengLspPendingAnalysisTargetKind;

typedef struct FengLspPendingAnalysis {
    FengLspPendingAnalysisTargetKind target_kind;
    char *uri;
    char *manifest_path;
    size_t generation;
    bool diagnostics_requested;
    bool refresh_workspace_index;
    struct timespec due_time;
} FengLspPendingAnalysis;
```

两个 target field 必须互斥：document 任务只拥有 `uri`，project 预热任务只拥有 canonical
`manifest_path`。document 任务继续从 overlay 文档调用现有 `build_analysis_session()`；project 任务用于
尚未打开任何文件的本地依赖，由 analyzer 直接按 manifest 调用 LSP 私有的 project-session 构建路径，并
从不可变 document snapshot 收集属于该 manifest 的已打开 overlay，不向 DocumentStore 注入伪文档。

为此只在 `service.c` 内把 `analysis_task_clone()` 扩展为同时携带 document snapshot 和上述 target，并把
`build_project_session()` 的 primary document 改为可选输入；project-open/resolve/frontend 主体保持不变。
project 任务的 project error 以 manifest path 定位，不发布 document diagnostics。

同一 canonical manifest 的连续 document/project 任务合并为最新 generation；manifest 尚未在后台解析的
document 任务暂按 URI 合并，解析后再与同 manifest 队列项合并。不同 manifest 的任务不得互相覆盖。
analyzer 仍串行处理，不增加线程，也不改变 frontend。

`module_index_scan_project()` 当前已经把递归访问到的 canonical manifest 保存在 visited list。module-index
job 在释放该 list 前，把其中尚未缓存、尚未 pending/running 的本地依赖 manifest 作为 project target
加入队列；根项目本身不重复入队。由此预热不依赖目标项目文件已经在 IDE 中打开。

### 6.1 避免重复本地依赖 resolve

当前 `build_project_session()` 每次调用 `feng_cli_deps_resolve_for_manifest()`；该公开 resolver 会物化
本地依赖。这与成功 session 数组是两个独立问题。

最小处理是复用对应旧成功 session 已拥有的 `bundle_paths`：

1. 同一 manifest 有旧成功 session 且 bundle paths 仍可加载时，candidate 直接把这些路径作为
   `FengCliFrontendInput.package_paths`；
2. 首次分析、manifest 配置变化或缓存路径失效时，才调用现有 resolver；
3. 新 candidate 复制并拥有自己的 canonical bundle paths；
4. 交互请求路径永远不调用 resolver。

这样不修改 dependency manager，也不会在每次源码编辑时重复物化本地项目。

---

## 7. 全局统一查询

### 7.1 通用流程

Definition、References、Rename 和 Implementation 统一使用：

```text
请求文件 path
→ 从 last_successful_analyses[] 找 origin session
→ resolve_target_at() 得到 origin target
→ 构造请求期间的稳定符号身份
→ 遍历 last_successful_analyses[]
→ 在每个 session 中定位同一符号
→ 复用该 session 的现有 AST/semantic 查询
→ 合并、去重并返回 physical file URI/range
```

不增加 `is_cross_project` 分支。不能定位该符号的 session 直接跳过。

### 7.2 稳定符号身份

现有 `resolved_targets_equal()` 在单个 session 内继续使用，不修改其语义。跨 session 边界增加一个
request-local identity adapter。该适配按以下步骤闭环，不把不同 session 的 AST 指针直接比较：

1. 在 origin session 中解析光标 target；
2. source target 记录 defining session 的 manifest/package path、module path、声明/成员 kind、owner
   chain、完整 callable signature、物理 source path 和 token；
3. imported target 从其 synthetic program path 取得 bundle provenance，并使用现有
   `FengImportedSymbolIdentity.module_name`、`symbol_id` 及 target AST 的精确声明形状；
4. bundle provenance 与成功 session 保存的 `package_path` 精确匹配，从而确定真正的本地 defining
   session，不能按 package name 猜测；
5. 在 defining session 的 LSP 私有 source symbol identity index 中按相同 module-local symbol id 取得物理
   source path/token，再按 path、kind、owner/member token 唯一定位源码 AST，得到规范化的 source
   identity；只有索引不可用或精确身份不能唯一验证时，才退回 module、kind、owner chain 与完整
   signature 匹配；
6. 遍历其他 session 时，仅接受 bundle provenance 指向同一 defining session，且 module、symbol id
   或完整声明形状一致的 imported AST；
7. 定位成本 session target 后，继续使用现有 `resolved_targets_equal()` 和
   `collect_references()`。

泛型 `fit` 成员也必须遵守同一套稳定身份规则。编译器为具体接收者选择或实例化
`fit T[]`、`fit T[!]` 等成员后，LSP 必须将该调用目标规范化回唯一的源码 `fit` 成员；接收者来自
字面量、局部变量、参数或其他可证明类型的表达式时，Definition 和 References 的目标必须一致。
规范化不得退化为按成员名匹配，并且必须保留只读/可写数组等完整 `fit` target 结构的区别。
同一模块允许存在多个完整形状相同的泛型 `fit`；此时必须由 symbol id 与物理 source path/token 消除歧义，
不得因为形状重复而返回空结果，也不得按声明遍历顺序猜测目标。

稳定身份是请求期间的只读值，不进入长期缓存。实现不得跨 session 保存 AST 指针，也不得依赖
`FengImportedSymbolIdentity.symbol_decl` 的地址相等。禁止只比较名称或参数数量；重载必须比较完整
signature。零个或多个候选均视为无法唯一定位：References/Implementation 不返回猜测项，Rename
整体返回 `null`。

该算法使用现有 session source AST、synthetic imported AST、bundle paths、project `package_path` 和
`FengImportedSymbolIdentity`，不需要修改 parser、semantic、Symbol API 或 FT/FB。

### 7.3 Definition

1. 在 origin session 中解析 target；
2. 若 target 已是本 session source AST，沿用当前 location 构建；
3. 若 target 是本地依赖 imported AST，使用稳定身份在全局成功数组中定位源码声明；
4. Location 使用目标 session `sources[].path` 的物理路径；
5. 找不到本地源码时返回 `null`，不得把 bundle entry 或 `PKG_NAME://...` 包装为 `file://`。

当 origin 项目因为任意源码文件存在语义错误而尚无成功 session 时，Definition 还必须支持以下只读降级
路径：

1. 使用 current parse 与当前项目的 persistent symbol index 解析光标 target；
2. symbol index 在后台构建时为每个 imported module 记录其精确 bundle provenance，该元数据仅由 LSP
   session 持有，不写入 `.ft` / `.fb`；
3. 仅当 bundle provenance 与某个已发布本地依赖 session 的 canonical `package_path` 精确相等时，才在该
   session 的 `source_symbol_identity_index` 中查找同 module、同 owner 和完整声明形状的物理源码符号；
4. 声明形状必须覆盖 kind、名称、泛型参数、完整参数类型、返回类型及成员 owner；零个或多个候选均视为
   无法证明唯一，不得按名称、参数数量或遍历顺序猜测；
5. 唯一命中时返回源码符号保存的 physical path/token；否则沿用既有非本地 package Definition 行为。

该降级路径只能读取已经发布的内存索引，不得同步读取 bundle、扫描源码、查找 manifest 或执行 Semantic
Analysis。它不改变失败 candidate 不得发布的规则，也不把 origin 项目的部分 Semantic Analysis 当作成功
结果。

### 7.4 References

1. 遍历全部成功 session；
2. 将全局身份定位成该 session 的本地 target；
3. 以 target 的精确源码拼写对 immutable source snapshot 做必要条件预筛选；不含该字节序列的文件可直接
   跳过，包含时仍必须进入原语义解析，不得把文本命中直接当作引用；
4. 调用现有 `collect_references()`；
5. 合并每个 session 的结果；
6. 按 `(physical path, start offset, end offset)` 去重；
7. 统一处理 `includeDeclaration`。

### 7.5 Rename

Rename 与 References 使用相同全局收集流程。所有 session 收集成功后才生成一个 `WorkspaceEdit`：

- edit 按 physical file URI 分组；
- 显式 `.fb` 和远程 package 不生成本地 edit；
- 任一应参与 session 出现身份歧义、文本不匹配或结果构建失败时返回 `null`；
- 不返回部分项目的重命名结果。

### 7.6 Implementation

当前 LSP 尚未注册 `textDocument/implementation`。本阶段在 `service.c` 增加 capability、dispatch 和
handler，遍历全局成功 analysis 中已有的 `FengSpecRelation` 和
`FengSpecImplementationSelection`：

- object-form spec declaration 返回关联 type/fit；
- object-form spec member 返回精确 `impl_member`；
- 没有编译器记录的精确关系时返回空数组；
- 结果按 physical path/range 去重；
- 禁止按同名成员猜测实现。

---

## 8. 既有查询能力的迁移

所有当前直接读取：

```c
&service->last_successful_analysis
```

的位置改为下列两类 helper：

```c
find_workspace_analysis_by_source_path(...)
for_each_last_successful_analysis(...)
```

Hover、Completion、Signature Help、Document Symbol 等单文件/单项目请求只选择包含请求文件的
session，继续执行现有逻辑。References、Rename、Definition 和 Implementation 使用全局遍历。

### 8.1 非本地 package 依赖零回归

非本地 package 依赖不是本次跨 session 映射对象，其既有查询链路必须原样保留：

```text
feng_cli_deps_resolve_for_manifest()
→ 全部 resolved.package_paths（本地与非本地均保留）
→ frontend package_paths / imported_module_cache
→ symbol provider add_bundle()
→ Hover、Completion、Signature Help、Definition 等既有请求逻辑
```

本次只在 imported target 的 bundle provenance 能与某个本地成功 session 的 `package_path` 精确匹配时，
增加“定位本地 physical source 并跨 session 聚合”的分支。不能匹配本地 session 是非本地 package 的正常
状态，必须继续执行 origin session 的既有 imported-symbol/provider 查询，不能把它当作错误、不能返回新增
的 `null`/空候选，也不能触发针对远程 package 的源码分析。

必须保持以下不变量：

- `build_project_session()` 继续把 resolver 返回的全部 package bundle 交给 frontend，不按
  `dependency->is_local_path` 过滤；
- `build_symbol_index_candidate()` 继续把全部 `resolved.package_paths` 加入 provider；
- 每个已发布 session 继续持有自己的 `bundle_paths` 和 `imported_module_cache`，多项目交换不得缩短其生命周期；
- Hover、Completion、Completion Resolve、Signature Help 和 Definition 对非本地 package 的协议结果不得
  因本地 workspace 映射失败而改变；
- References/Implementation 不猜测非本地源码位置，Rename 不为 `.fb` 或非本地 package 生成 edit；
- 不修改非本地 package 的 `.ft` / `.fb` 查找、格式、读写和缓存目录。

本地增强必须是 origin session 既有查询之后的附加映射层，而不是替换 imported-symbol/provider 查询层。
若请求不涉及本地 workspace session，执行路径和当前实现保持一致。

### 8.2 派生索引的项目隔离

现有 `symbol_index` 和 `module_index` 是 current-text fallback/冷启动派生索引，不属于
`last_successful_analysis[]`。它们继续只保留当前活动查询上下文的一份，但增加一个规范化 manifest
身份：

```c
char *workspace_index_manifest_path;
```

请求或 `didChange` 找到 origin session 后，仅在该身份与 `session.manifest_path` 一致时使用派生索引；
不一致时调度现有 index refresh，并在本次请求中使用安全的 syntax/session fallback，禁止借用另一个
项目的 provider。这样无需把两个派生索引也改成多份，同时保证项目切换不会发生错误命中。

---

## 9. 分步开发 TODO

状态规则：`[ ]` 表示尚未完成，`[x]` 表示已经完成且具有代码、测试或检查依据。各项必须按以下顺序
实施；完成一项后直接更新本文档状态。生产实现预计只修改 `src/cli/lsp/service.c`，不新增生产 `.c`
文件，不修改 `service.h` 的 opaque service 接口，不修改 Makefile，不修改 LSP 之外的生产代码。

### Phase 0：方案确认

- [x] 核对当前单 `last_successful_analysis`、后台 candidate、成功后交换和失败保留旧缓存的代码事实。
- [x] 在本文档固定多项目数组、稳定身份、缓存优先、无缝替换、变更边界及既有性能门槛。
- [ ] 完成人工 Review，并在实现前确认本文档方案冻结。

### Phase 1：多项目成功缓存容器

- [ ] 增加 `FengLspWorkspaceAnalysis` 和 `last_successful_analyses[]` 动态数组字段，移除单成功槽位。
- [ ] 为 `FengLspAnalysisSession` 增加 LSP 私有 `package_path`，并补齐复制、所有权和释放逻辑。
- [ ] 增加按 source physical path、canonical manifest 和 standalone path 查找数组元素的 helper。
- [ ] 将 generation 判断改为同一数组元素内比较，禁止跨项目 generation 互相阻止发布。
- [ ] 修改 service shutdown，逐元素执行 `session_dispose()` 后释放数组。

### Phase 2：无空窗 candidate 发布

- [ ] 保持 candidate 独立构建，重建开始和重建期间不修改已发布数组元素。
- [ ] 仅在 §5.3 全部成功条件满足后，才在 `analysis_mutex` 内交换目标 session 和 generation。
- [ ] candidate 失败、取消或过期时只释放 candidate，旧 session 和 generation 保持不变。
- [ ] 交换完成并解锁后再释放旧 session，禁止请求观察到空或半初始化状态。
- [ ] 调整 successful-prefix 发布，使其只关联本次实际发布的项目 session/generation。

### Phase 3：多项目后台调度

- [ ] 将单一 `pending_analysis_uri` 改为可同时保存 document URI/project manifest 两类目标的小型动态队列。
- [ ] 记录 pending/running identity 和 generation；同一项目只保留最新任务，不同项目不得互相覆盖。
- [ ] 扩展 analysis task/project-session 私有 helper，使未打开文件的 project manifest 任务可消费当前
      document snapshot overlays，且不向 DocumentStore 注入伪文档。
- [ ] 实现 §5.5 的缓存状态判断：精确命中不重建，stale/miss 才按需调度。
- [ ] 相同或更新 generation 已 pending/running 时不重复入队。
- [ ] 保留既有 debounce、取消、diagnostics 请求和单 analyzer worker 行为。
- [ ] 在既有本地依赖 module-index visited manifest 遍历中预热尚未缓存且未排队的依赖项目 session。
- [ ] 同一 manifest 普通源码编辑时复用有效 resolved bundle paths，仅在 §6.1 条件满足时重新 resolve。

### Phase 4：既有单项目请求迁移

- [ ] 将所有直接读取 `service->last_successful_analysis` 的位置迁移到按 source path 选择 session 的 helper。
- [ ] 保持 Hover、Completion 等 current-text 快路径及既有有界冷启动等待顺序不变。
- [ ] 为单份派生 symbol/module index 增加 `workspace_index_manifest_path`，禁止跨项目错误借用。
- [ ] 保持全部 resolved package bundles、imported-module cache 和 symbol-provider 构建逻辑不变，确保
      非本地 package Hover、Completion、Completion Resolve、Signature Help 和 Definition 零回归。
- [ ] 确认同项目 Hover、Completion、Signature Help、Document Symbol、Definition、References 和 Rename
      行为不变。

### Phase 5：跨 session 符号查询

- [ ] 实现 §7.2 的 request-local 稳定符号身份，不跨 session 使用 AST 指针身份。
- [ ] Definition 从 imported target 定位本地依赖源码，并返回 physical file URI/range。
- [ ] References 遍历全部成功 session，聚合并按 physical path/range 去重。
- [ ] Rename 在所有相关 session 完整且无歧义时生成单个 WorkspaceEdit，否则返回 `null`。
- [ ] 增加 `textDocument/implementation` capability、dispatch 和 handler，并按精确 spec relation 聚合结果。

### Phase 6：新增测试

- [ ] 仅在 `test/` 增加所需 fixture/helper，不修改任何已有测试用例或 expected 文件。
- [ ] 实现 §11 的全部新增功能、缓存生命周期、并发发布、调度和防串线用例。
- [ ] 为精确命中不重建、任务合并和 candidate 无缝替换增加可观测断言。

### Phase 7：验收与交付

- [ ] 执行新增定向用例并记录结果。
- [ ] 执行 §10 既有性能矩阵，确认所有门槛保持不变。
- [ ] 在非 Codex 沙箱环境执行全量 `make test` 并记录结果。
- [ ] 检查生产 diff 仅位于 `src/cli/lsp/`，测试 diff 仅新增 `test/` 文件。
- [ ] 对照 §12 逐项确认完成标准，并完成人工代码 Review。

---

## 10. 性能标准

[Feng LSP 性能优化方案](feng-lsp-performance-optimize.md) 的既有标准全部保持不变，不因多 session
缓存或全局查询放宽：

| 指标 | 既有门槛 |
| --- | ---: |
| 所有 Hover 请求延迟 Max | ≤ 16ms |
| 所有 `textDocument/completion` 请求延迟 Max | ≤ 16ms |
| 所有交互请求 P99 | ≤ 50ms |
| 已排队过期请求的取消处理 | ≤ 5ms |
| 交互请求同步磁盘 I/O | 0 次 |
| 交互请求同步整项目分析 | 0 次 |
| 编辑、失败或取消导致成功缓存变空 | 0 次 |
| 返回使用错误文档版本的精确语义结果 | 0 次 |

成功数组查找和跨 session 身份定位只能读取内存。manifest 查找、dependency resolve、源码读取和项目
分析只能发生在后台 candidate 流程。

第一版使用动态数组线性遍历以保持最小改动。若性能测试证明 source/session 查找本身超过既有门槛，
必须在 `service.c` 内增加不拥有 AST 的 path 派生索引；不能通过放宽性能标准、减少查询结果或同步等待
分析解决。派生索引只优化查找复杂度，不改变 `last_successful_analysis[]` 的权威所有权模型。

---

## 11. 新增测试

测试允许在 `test/` 增加 fixture、helper 和 test function；禁止改变已有用例的行为或预期。

至少覆盖并逐项标记：

- [ ] 依次打开 A、B、C 项目后，成功分析数组同时保留三份 session。
- [ ] 在 A/B/C 之间切换和 `didClose` 后，旧成功 session 仍可命中。
- [ ] 热请求从成功数组按 source path 命中，不依赖再次查找 manifest。
- [ ] B 依赖 A 时，从 B use Definition 到 A 的物理 `.ff`。
- [x] B 存在无关语义错误且没有成功 session 时，imported type、constructor/member 与 free function 的
      Definition 仍通过精确 bundle/source identity 返回 A 的物理 `.ff`。
- [ ] 从 A 声明或 B use 发起 References，得到数组中所有合法引用。
- [x] 本地依赖同一模块存在多个同形 `fit T[]` 及 `fit T[!]` 同名成员时，非字面量接收者的
      Definition/References 精确命中目标物理声明，且不同 `fit` 之间不串线。
- [ ] Rename 返回覆盖多个 physical file URI 的单个 WorkspaceEdit。
- [ ] Rename 任一参与 session stale/失败时不返回部分 edit。
- [ ] object spec 和 spec member 的 Implementation 跨 session 返回正确结果。
- [ ] 不相关项目具有同 module/name 时不串线。
- [ ] 快速编辑不同项目时 pending task 不互相覆盖。
- [ ] 本地依赖项目没有任何 IDE open document 时，manifest 预热任务仍能成功发布且不创建伪文档。
- [ ] 某项目 candidate 失败不清除其他项目或自身旧成功 session。
- [ ] 普通源码编辑后不重复触发本地依赖物化。
- [ ] LSP 请求不修改已有 `.ft`、`.fb`、C 或 `.fd` 产物。
- [ ] 仅使用非本地 package 依赖的项目在优化前后获得一致的 Hover 和 Signature Help。
- [ ] 仅使用非本地 package 依赖的项目在冷启动、热缓存和编辑后获得一致的 Completion/Completion Resolve。
- [ ] 在本地项目 A/B 间切换后，各项目自身的非本地 package Hover、Completion 和 Definition 不串线、不丢失。
- [ ] 既有同项目 Hover、Completion、Definition、References 和 Rename 全部保持通过。
- [ ] 多项目缓存及全局查询继续满足 §10 全部既有性能门槛。
- [ ] 同一 manifest 连续成功重建后数组 count 不增长，只替换对应元素。
- [ ] 精确缓存命中的请求立即返回，不进入冷启动等待，也不新增后台任务。
- [ ] stale/miss 请求最多合并出一个最新 generation 后台任务，不在请求线程执行分析。
- [ ] 相同或更新 generation 已 pending/running 时重复请求不重复入队。
- [ ] candidate 构建期间持续请求同一项目，始终可读取旧成功 session，不出现缓存空窗。
- [ ] candidate 失败、取消或过期后，旧成功 session 及 generation 均保持不变。
- [ ] candidate 成功发布时，请求只能观察到完整旧 session 或完整新 session，不能观察到空/半初始化状态。
- [ ] 最终执行全量 `make test`。

文档变更阶段不运行测试。代码实施完成后必须先运行定向用例，再按仓库规则在非 Codex 沙箱环境运行
全量 `make test`，测试产物不得放入 `/tmp` 或 `/private/tmp` 后执行。

---

## 12. 完成标准

- 一个 LSP service 同时保留多个项目最后一次成功 `FengLspAnalysisSession`；
- 每个项目最多一份成功版本，不保存历史队列；
- 热请求通过全局成功数组直接找到 source，不重复查 manifest；
- 每次请求先返回可证明正确的内存缓存；精确命中不等待、不重建；
- stale/miss 只按需合并调度后台重建，不阻塞当前响应；
- 重建期间旧成功 session 始终保留，只有完整成功 candidate 才在锁内 O(1) 交换发布；
- 失败、取消或过期 candidate 只释放自身，不能修改或清空旧成功 session；
- 项目切换、`didClose` 和其他项目失败不清除缓存；
- 同项目和跨项目查询走同一全局遍历逻辑；
- Definition 返回本地依赖物理 source URI；
- References、Rename 和 Implementation 能聚合多个成功 session；
- Rename 不产生部分 WorkspaceEdit；
- 普通编辑不重复物化未变化的本地依赖；
- 全部既有 LSP 性能标准保持不变；
- 不修改 parser、semantic、symbol、FT/FB、IR、Codegen、DAP 或核心查询接口；
- 非本地 package 的 bundle/provider/imported-module 查询链路及既有 LSP 能力保持不变；
- 生产变更仅位于 `src/cli/lsp/`，实际最小实现集中在 `service.c`；
- 新增定向用例及全量回归通过。
