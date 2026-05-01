# 外部包 use 支持计划

> 本文档描述从「`use` 外部模块」到「完整类型检查 → 编译 → 链接 → 运行」的完整实现路线。  
> 目标原则：**最小改动、架构合理、面向未来**。  
> 分四个 Phase 实现，每个 Phase 结束后做一次全量回归（`make test`）。

## 1. 背景与目标

当前已完成：

- `feng compile --pkg=<path.fb>` CLI 解析
- Frontend 将 `.fb` 注册为 `FengSymbolProvider`
- 核心语义查询接口已收敛为 `get_module`；语义分析可通过抽象查询接口拿到外部模块
- 当前代码已具备“外部 public decl 生成最小 `FengDecl` / `FengProgram` 并注入 analysis”的骨架
- 但：该骨架当前仍位于 CLI 侧原型实现中；后续需收敛为“symbol 负责构造与释放 API，CLI 只负责调度与生命周期控制”，且目前仍只覆盖最小名字面；完整签名、codegen extern、链接消费尚未完成

最终目标：`use` 外部包模块后，与 `use` 同编译输入中的普通模块**走完全相同的处理路径**：名字进可见表 → 类型检查 → codegen 生成正确 C → 链接 → 运行正确。

## 2. 关键架构事实

- `FengDecl` / `FengProgram` 是核心编译器统一定义；外部包导入时构造的只是“无 body 的同构对象”，不是另一套模型
- `FengSemanticModule` 也必须保持同一模型；内部模块与外部模块进入核心编译器后，共享同一套模块表、名字解析、可见性、类型检查逻辑
- 核心编译器不能依赖或感知 symbol 模块；核心只通过 `FengSemanticImportedModuleQuery` 进行抽象查询
- symbol 模块负责从 `.fb` 加载符号，并解析、构造核心编译器可直接使用的 `FengDecl` / `FengProgram` / `FengSemanticModule`
- symbol 模块负责提供 imported-module cache 的创建 / 查询 / 释放 API；CLI 不直接释放 `FengDecl` / `FengProgram` 细项，只调用 symbol 暴露的 release API
- CLI 只负责调度：创建 provider、创建 imported-module cache、组装 query、控制生命周期、把抽象接口交给核心编译器
- 外部模块对象生命周期必须至少覆盖 semantic / codegen / symbol export 全流程；不能在语义分析结束后立刻释放
- 核心编译器改动原则：尽可能不改或少改；只允许为“抽象查询接入、analysis 注入时序、external module 标记、extern codegen、链接输入透传”增加最小必要改动，不在核心中引入 `.fb` / `.ft` / ZIP / symbol provider 细节
- 内外差异应只体现在输出阶段：codegen / export / link 根据“是否外部模块”“是否有 body”决定发码和链接策略；语义层不复制出第二套内部/外部规则
- `FengSemanticModuleOrigin` / `origin` 这类来源标记的职责应限定为 codegen / export / link 的输出提示，而不是 resolver / typecheck 的规则分叉开关

- 所有 Feng 函数在生成 C 中均为 `static`（当前整包单文件编译）
- 类型描述符 `FengTypeDesc__feng__pkg__mod__T` 已是非 `static` 全局符号（可以跨链接单元）
- C 名称 mangling：`feng__{路径用__分隔}__{name}__from__{sig}`（函数）/ `Feng__{路径}__{方法}`
- `FengSymbolDeclView` 包含完整签名（params、return\_type、members、declared\_specs）
- `FengSymbolTypeView` 可 1:1 映射到 `FengTypeRef`（named/pointer/array）
- `import_public_names()` 接受 `FengSemanticModule*`，内外模块处理逻辑完全一致
- 外部包 `.fb` 结构：`feng.fm`（元信息）+ `mod/*.ft`（符号）+ `lib/<os>-<arch>/lib*.a`（静态库）

## 3. 显式范围外

- 动态库（`.dylib`/`.so`）
- 增量编译、依赖版本选择、远程下载
- 对 `use ext.m as M;` 别名的额外测试（实现路径与非别名完全重合，Phase 1 自然覆盖，不单独拆）

---

## Phase 1 — 语义：名字导入

**目标：** `use ext.pkg.mod;` 和 `use ext.pkg.mod as M;` 均正常工作；外部模块的公开名字（type/spec/function/binding）进入可见表，语义分析感知不到内外区别。

### 1.1 `semantic.h`：重构 `FengSemanticImportedModuleQuery`

删除 `module_exists` 回调，替换为 `get_module`，职责合二为一：

```c
typedef struct FengSemanticImportedModuleQuery {
    const void *user;
    const FengSemanticModule *(*get_module)(const void *user,
                                            const FengSlice *segments,
                                            size_t segment_count);
} FengSemanticImportedModuleQuery;
```

语义：`get_module` 返回非 NULL ↔ 模块存在且可使用；返回 NULL ↔ 模块不存在（报错）。无需两个回调，不存在职责重叠。

### 1.2 `analyzer.c`：在 resolve 前预注入外部模块

在分析早期扫描所有 `use`，若目标不是本次编译输入中的本地模块，则调用 `get_module`；命中后直接把外部模块追加进 `analysis->modules[]`。这样后续：

- `find_module_index_by_path()`
- `import_public_names()`
- `append_alias_entry()`
- `build_program_aliases()`

都继续走现有本地模块路径，无需在 `check_symbol_conflicts()` 中增加一套“外部模块专用分支”。

核心伪代码：

```c
if (find_module_index_by_path(analysis, use_decl->segments, use_decl->segment_count)
        == analysis->module_count) {
    const FengSemanticModule *ext = imported_query->get_module(
        imported_query->user, use_decl->segments, use_decl->segment_count);
    if (ext != NULL) {
        ok = add_external_module(analysis, ext);
    }
}
```

语义核心**无新逻辑**：外部模块一旦进入 `analysis`，名字导入、别名注册、后续 resolver、可见性判断、类型检查都复用既有逻辑；不因“来自外部包”复制出第二套语义路径。

### 1.3 symbol 模块：ImportedModuleCache + `provider_get_module` 回调

新增一个属于 symbol 模块的缓存对象（可放在 `provider` 相邻实现或独立 symbol 子模块），负责：
1. 将 `FengSymbolImportedModule*` 映射为 `FengSemanticModule*`（按路径缓存，同一模块多次 use 返回同一指针）
2. 为每个公开 decl 构造核心编译器可直接消费的 `FengDecl`；Phase 1 先填最小名字面，Phase 2 再补完整签名；`body = NULL`
3. 将 decl 数组包进 `FengProgram`，再包进 `FengSemanticModule`
4. 由 symbol 模块统一持有这些对象，并暴露 release API；CLI 只持有缓存句柄，直到 analysis / codegen / export 全部结束后再调用 release API 释放

`provider_get_module` 回调实现：

```c
static const FengSemanticModule *provider_get_module(
    const void *user, const FengSlice *segments, size_t segment_count)
{
    ImportedModuleCache *ctx = (ImportedModuleCache *)user;
    // 1. 查缓存
    // 2. cache miss → find_module → build synthetic FengSemanticModule
    // 3. 存入缓存并返回
}
```

这一步的职责边界必须明确：构造 `FengDecl` / `FengProgram` 属于 symbol 模块，不属于 CLI；CLI 只负责持有句柄并调用 symbol 的释放 API。

### 1.4 测试

- 语义单测：`use` 外部模块（type、spec、function、binding），名字在可见表中，不报 undefined
- 语义单测：`use ext.m as M;` 别名注册成功，`M.SomeType` 路径可解析（若 resolver 已支持别名 qualified 访问则测，否则仅验不报错）
- 语义单测：外部模块不存在时报正确错误（回归验证 `module_exists` 降级路径）
- `make test` 全量回归

---

## Phase 2 — 语义：完整签名合成

**目标：** 外部函数调用参数/返回类型检查通过；外部类型字段读写类型检查通过；外部 spec 约束检查通过。

### 2.1 扩展 symbol 模块的导入模块构造逻辑

在 Phase 1 构建合成 `FengDecl` 时补全签名：

| 源（`FengSymbolDeclView`） | 合成目标（`FengDecl`） |
|---|---|
| `params[]` + `return_type` | `as.function_decl.params[]` + `as.function_decl.return_type` |
| `members[]`（field/method） | `as.type_decl.members[]` |
| `declared_specs[]` | `as.type_decl.declared_specs[]` |

### 2.2 `FengTypeRef` 合成规则（`FengSymbolTypeView` → `FengTypeRef`）

```
BUILTIN  → named, 单段（builtin 名本身，如 "i32"、"string"）
NAMED    → named, 多段 segments（如 ["ext", "pkg", "mod", "T"]）
POINTER  → pointer, 递归合成 inner
ARRAY    → array, 递归合成 element + rank + layer_writable
```

跨模块类型引用（外部函数参数类型来自另一个外部模块）：`FengTypeRef` 填 named+segments，resolver 自动在当前可见表里查找对应合成 `FengDecl`，无需指针注入，resolver 路径不变。

### 2.3 合成对象内存管理与时序

所有合成的 `FengTypeRef`、`FengParameter`、`FengTypeMember`、字符串（name 的拷贝）均由 symbol 模块的导入模块缓存统一持有。

注意两点：

1. 这些对象的生命周期至少覆盖 semantic / codegen / symbol export，全流程结束后再统一释放。
2. `feng_semantic_compute_spec_relations(...)` 等依赖声明面的 pass，必须在外部模块注入完成后再运行；否则外部 `declared_specs` / spec 关系不会进入分析结果。

### 2.4 测试

- 语义单测：外部函数调用参数类型不匹配 → 报类型错误
- 语义单测：外部函数调用参数类型匹配 → 通过
- 语义单测：外部类型字段读取类型正确
- 语义单测：外部类型实现 spec 约束正常参与 spec 关系检查
- `make test` 全量回归

---

## Phase 3 — Codegen：包导出 + 消费者外部声明

### 子阶段 3a — LIB 模式：public 函数改为非 `static`（独立，不依赖 Phase 1/2）

**目标：** `feng compile --target=lib` 时，public 函数产出非 `static` 符号，可被外部链接单元引用。

**原因：** C 语言 `static` 函数作用域仅限于当前编译单元（`.o`），不出现在 `.a` 的符号表中，链接器看不到，消费者包无法链接。`target=lib` 时 public 函数必须去掉 `static` 才能成为可导出符号。类型描述符 `FengTypeDesc__...` 已是非 `static`，行为一致。

**改动：** `codegen.c` `cg_emit_function`，按 `target + visibility` 决定是否加 `static`：

```c
bool is_static = !(target == FENG_COMPILE_TARGET_LIB
                   && decl->visibility == FENG_VISIBILITY_PUBLIC);
// emit: (is_static ? "static " : "") rettype fn_name(...)
```

**测试：** codegen 单测，LIB 模式 public 函数生成无 `static`；PRIVATE/DEFAULT 函数仍有 `static`；BIN 模式所有函数仍有 `static`。

### 子阶段 3b — 消费者：外部模块跳过 body，emit extern 声明（依赖 Phase 1）

**目标：** 消费者编译时，外部模块不生成 body，而是 emit 正确的 C extern 声明。

这里的边界要明确：内外模块在核心编译器中仍然是同一种 `FengSemanticModule` / `FengProgram` / `FengDecl`；差异只在 codegen 阶段体现。也就是说，codegen 根据“是否外部模块”“decl 是否有可发码 body”决定：

- 内部模块：生成定义（type/function/method body）
- 外部模块：不重复生成定义，只发 extern 声明和必要的 descriptor 引用

**改动一：** `semantic.h` `FengSemanticModule` 改为来源枚举：

```c
typedef enum FengSemanticModuleOrigin {
        FENG_SEMANTIC_MODULE_ORIGIN_LOCAL = 0,
        FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE
} FengSemanticModuleOrigin;

FengSemanticModuleOrigin origin;
```

**改动二：** `frontend.c` Phase 1 的合成 `FengSemanticModule` 构建时置 `origin = FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE`。

**改动三：** `codegen.c` 遍历 `analysis->modules[]` 时：
- `origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE`：跳过 body 生成；对消费者实际引用的外部符号 emit extern 声明：
  - 类型：`extern struct Feng__{mangled};` + `extern const FengTypeDescriptor FengTypeDesc__{mangled};`
  - 函数：相同 mangling 格式，无 `static`，无 body

**测试：** codegen 单测：消费者程序引用外部类型/函数，生成 `.c` 文件能通过 `clang -c` 编译（不链接）。

---

## Phase 4 — 链接：从 `.fb` 提取 `.a` 并传入链接器

**目标：** `feng compile --pkg=foo.fb` 时，`foo.fb` 内的 `.a` 自动参与最终链接，消费者 binary 运行正确。

### 4.1 `frontend.c`：收集 bundle 路径，写入 `FengCliFrontendOutputs`

`add_bundle` 时不解压 `.a`，只记录 bundle 文件路径。`FengCliFrontendOutputs` 加新字段：

```c
const char **bundle_paths;   /* .fb 文件的绝对路径列表 */
size_t       bundle_count;
```

`feng_cli_frontend_run` 成功路径末尾，将所有 `--pkg` 传入的 `.fb` 路径写入该字段（调用方负责 `free`）。

### 4.2 `direct.c`：透传给 `FengCliDriverOptions`

`FengCliDriverOptions` 加相同字段；`feng_cli_direct_main` 将 `bundle_paths` 从 outputs 传给 driver options。

### 4.3 `driver.h/c`：拓扑排序 + 解压 `.a` + 链接

链接前在 driver 层完成：

1. **解析依赖图**：遍历各 `.fb` 内的 `mod/*.ft`，读取每个模块的 `uses[]`，按包粒度建有向依赖图（A 的某模块 `use` 了 B 的模块 → A 依赖 B）
2. **拓扑排序**：输出有序包列表（被依赖者在后），确保 `[A.a, B.a, ...]` 顺序正确（A 依赖 B → A 在 B 前）
3. **按序解压**：对排好序的每个 `.fb`，从 ZIP 中提取 `lib/<host_target>/lib*.a` 到 tmpdir
4. **追加链接命令**：

```
cc ... feng.c libfeng_runtime.a <sorted_A.a> <sorted_B.a> ... -lpthread -o output
```

绝对路径直接追加（不用 `-l`）。`host_target` 由已有的 `feng_fb_detect_host_target()` 获取。

### 4.4 测试

- 端到端 smoke test：
  1. 构建一个最小 Feng 库（含 1 个 public 函数），`feng pack` 产出 `testpkg.fb`
  2. 消费者工程 `feng compile --pkg=testpkg.fb`，`use` 该库，调用函数
  3. 运行 binary，输出正确
- 多包依赖顺序 smoke test：A 依赖 B，消费者同时依赖 A 和 B，验证链接顺序正确、运行正确
- `make test` 全量回归

---

## 4. 细化执行步骤

> 当前首批实施批次：先完成步骤 0、1、2。
> 本批只做职责下沉与生命周期收敛，不扩展外部 decl 签名能力，不改变现有 `--pkg` 语义表面。
> 本批完成门槛：现有 pkg 相关行为保持不变，新增 cache 生命周期测试通过，并执行一次全量 `make test` 回归。
> 下一批实施批次：步骤 6、7、8。
> 本批目标：让 imported-module cache 生成完整 decl 签名，并让外部函数、外部类型成员、外部 spec 关系真正进入语义类型检查。

下面按“先收敛职责边界，再补语义，再补 codegen，再补链接”的顺序执行。每一步都只引入当前阶段最小必要改动，避免核心编译器被迫感知 symbol 实现细节。

| 步骤 | 改动位置 | 核心逻辑 | 完成判定 |
|---|---|---|---|
| 0. 收敛 symbol 生命周期 API | `src/symbol/provider.h` / `src/symbol/provider.c` 或新 symbol 子模块 | 先把 imported-module cache 的 API 形状定下来：`create` / `as_query` / `release`。`create` 负责基于 provider 建缓存句柄，`as_query` 返回 `FengSemanticImportedModuleQuery`，`release` 负责整体释放外部模块对象。CLI 之后只认这三个入口。 | CLI 中不再出现逐项 `free FengDecl/FengProgram` 的代码路径；生命周期边界清晰 |
| 1. 迁移当前 CLI 原型到 symbol 模块 | 当前 `src/cli/pkg_bridge.c/h` 原型，目标迁入 symbol 模块 | 把现有“由 public decl 生成最小 `FengDecl` / `FengProgram`”的实现整体迁出 CLI。迁移时不扩展功能，只搬职责。核心要求是：`get_module` 返回值的内存归 symbol 模块缓存持有。 | CLI 侧不再承担外部模块对象构造职责；当前功能行为不变 |
| 2. CLI 改成纯调度 | `src/cli/frontend.c` | Frontend 只做四件事：加载 bundle 到 provider、创建 imported-module cache、取 query 填到 `FengSemanticAnalyzeOptions`、在 analysis / codegen / export 结束后调用 symbol release API。 | `frontend.c` 中不再出现构造 `FengDecl` / `FengProgram` 的实现细节 |
| 3. 保持核心编译器最小接入面 | `src/semantic/semantic.h` | 核心仍然只保留 `FengSemanticImportedModuleQuery.get_module` 这一抽象入口，不新增任何 symbol 相关头文件依赖，也不传 provider、bundle、zip reader 等对象。 | semantic 公共头文件仍然只暴露抽象 query，不暴露 symbol 类型 |
| 4. 完成 Phase 1 的 analysis 注入路径 | `src/semantic/analyzer.c` | 在 resolve 前扫描 `use`，通过 `get_module` 把外部模块注入 `analysis->modules[]`。注入后，`find_module_index_by_path()`、`import_public_names()`、`append_alias_entry()`、`build_program_aliases()` 全部复用原逻辑。核心点是“先注入，再走既有路径”，而不是在 resolver 中四处打外部特判。 | `use ext.mod;` 和 `use ext.mod as M;` 走通，且 `import_public_names()` 不需要复制出一套外部分支 |
| 5. 先补 Phase 1 验证测试 | `test/semantic/test_semantic.c`、必要时 `test/cli/test_cli.c` | 在继续扩功能前，先锁住名字面行为：外部 type/spec/function/binding 可见；alias 路径可用；模块不存在时报错。测试要直接覆盖 query 命中和注入后的可见表行为。 | Phase 1 测试稳定通过，后续扩签名时能快速回归 |
| 6. 扩展 symbol 构造逻辑到完整签名 | symbol 模块 imported-module cache 实现 | 在现有最小 `FengDecl` 基础上，逐步补 params、return_type、members、declared_specs、`FengTypeRef` 递归构造。这里的核心逻辑是“外部模块生成的仍然是核心原生 AST/decl 模型”，不引入另一套 adapter 类型参与 resolver。 | 外部 decl 拥有足够签名信息供 resolver / infer / spec 检查直接使用 |
| 7. 调整核心 pass 时序，只做必要改动 | `src/semantic/analyzer.c` | 把依赖声明面的 pass 放到外部模块注入之后执行，尤其是 `feng_semantic_compute_spec_relations(...)`。核心点不是改 spec 算法，而是确保它看到完整的 `analysis->modules[]`。 | 外部 `declared_specs`、spec closure、spec relation 能进入后续分析结果 |
| 8. 补 Phase 2 类型测试 | `test/semantic/test_semantic.c` | 先用测试锁定四类行为：外部函数参数匹配 / 不匹配、外部字段类型、外部 spec 约束。测试驱动点是“resolver 完全按本地 decl 处理外部 decl”。 | Phase 2 测试全部通过，且无需在 resolver 各处分支区分外部 / 本地 |
| 9. 做 3a：LIB 导出 public 函数 | `src/codegen/codegen.c` | 只改 `cg_emit_function` 的 `static` 决策：`target=lib && public` 时去掉 `static`；其余路径保持不变。核心点是把改动限制在单一输出策略，不动前端和语义。 | LIB 模式 public 函数具备可导出符号，BIN 模式行为不变 |
| 10. 做 3b：消费者 extern 声明 | `src/codegen/codegen.c`、必要时 `src/semantic/semantic.h` | 利用 `origin == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE` 区分“只声明不生成 body”的模块。核心逻辑是：analysis 中保留外部模块供类型检查和命名解析使用，但 codegen 只为它们发出 extern 声明，不重复产出定义。 | 消费者引用外部类型 / 函数时，生成的 `.c` 能独立 `clang -c` |
| 11. 打通 bundle 路径透传 | `src/cli/frontend.c`、`src/cli/compile/direct.c`、`src/cli/compile/driver.h` | Frontend 只收集 bundle 绝对路径，Direct 只透传，Driver 才真正消费。核心点是职责单一：前端不解包、不排序、不链接。 | driver 在不接触前端内部结构的前提下拿到 bundle 列表 |
| 12. 在 driver 中实现包依赖排序与解包 | `src/cli/compile/driver.c` | Driver 在链接前解析 `.fb` 里的 `mod/*.ft` uses 建图，按包粒度拓扑排序，再解压 `lib/<host_target>/lib*.a` 并追加到链接命令。核心逻辑是“链接顺序问题在 driver 解决”，而不是把库顺序泄露到语义或 codegen。 | 多包依赖场景下，静态库顺序正确、链接稳定 |
| 13. 补 Phase 4 端到端测试与全量回归 | `test/smoke`、`test/cli`、必要时 `scripts` | 做最小真实链路：构建库、pack `.fb`、消费者 `--pkg` 编译、运行输出校验，再补 A 依赖 B 的顺序场景。最后统一 `make test`。 | 全链路 build → pack → consume → run 通过 |

### 4.1 每步的核心控制原则

1. 先迁职责，再扩功能。先把“谁构造、谁持有、谁释放”收敛清楚，再补 Phase 2/3/4。
2. 核心编译器只接受抽象查询结果，不接受 symbol provider、本地 bundle 路径、ZIP 句柄等实现细节。
3. 外部模块一旦进入 `analysis->modules[]`，后续所有语义逻辑都优先复用本地模块路径，不复制外部分支。
4. 生命周期由 CLI 负责控制，但释放动作由 symbol 模块提供 API 完成。
5. 每个 Phase 先补对应测试，再进入下一个 Phase；不要等到最后一次性补测试。
6. 内外模块在核心编译器内保持同一模型；差异一律延后到 codegen / export / link，通过“有无 body / 是否 external”决定输出策略。

## 5. 改动文件汇总

| 文件 | Phase | 说明 |
|---|---|---|
| `src/semantic/semantic.h` | 1、3b | `module_exists` → `get_module`（合并）；加 `FengSemanticModuleOrigin origin` 来源枚举 |
| `src/semantic/analyzer.c` | 1、2 | 在 resolve 前预注入外部模块；保证依赖声明面的 pass 在注入后运行 |
| `src/symbol/provider.c` / symbol 子模块 | 1、2 | 从 `.fb` 构造 `FengDecl` / `FengProgram` / `FengSemanticModule`；缓存与释放 imported modules |
| `src/cli/frontend.c` | 1、2、4 | 仅调度 provider / query；管理 provider / query 生命周期；bundle_paths 收集；outputs 字段 |
| `src/cli/pkg_bridge.c/h` | 1、2 | 当前原型职责下沉到 symbol 模块后删除、瘦身或并入 symbol 实现 |
| `src/codegen/codegen.c` | 3a、3b | LIB 模式 public 函数去 static；extern 声明 emit |
| `src/cli/compile/direct.c` | 4 | 透传 bundle_paths |
| `src/cli/compile/driver.h` | 4 | FengCliDriverOptions 加 bundle_paths 字段 |
| `src/cli/compile/driver.c` | 4 | 拓扑排序 + 解压 .a + 链接命令 |

语义核心仍然只依赖抽象查询接口；外部模块一旦通过 `get_module` 注入 `analysis`，后续名字导入和 resolver 继续复用现有逻辑。

## 6. 验收口径

| Phase | 验收条件 |
|---|---|
| 1 | `use ext.mod;` 和 `use ext.mod as M;` 不报错；外部名字在可见表；`make test` 通过 |
| 2 | 外部函数调用/字段访问类型检查正确；签名不匹配时报正确错误；`make test` 通过 |
| 3a | LIB 模式 public 函数无 `static`；`make test` 通过 |
| 3b | 消费者生成 C 能通过 `clang -c`；`make test` 通过 |
| 4 | 端到端：build → pack → consume → run 全链路正确；`make test` 通过 |
