# 外部包 use 支持计划

> 本文档描述从「`use` 外部模块」到「完整类型检查 → 编译 → 链接 → 运行」的完整实现路线。  
> 目标原则：**最小改动、架构合理、面向未来**。  
> 分四个 Phase 实现，每个 Phase 结束后做一次全量回归（`make test`）。

## 1. 背景与目标

当前已完成：

- `feng compile --pkg=<path.fb>` CLI 解析
- Frontend 将 `.fb` 注册为 `FengSymbolProvider`
- 语义分析中 `module_exists` 查询（`use ext.mod;` 不报"not found"）
- 但：外部符号不进入可见表，外部类型/函数无法在编译输入中被实际引用

最终目标：`use` 外部包模块后，与 `use` 同编译输入中的普通模块**走完全相同的处理路径**：名字进可见表 → 类型检查 → codegen 生成正确 C → 链接 → 运行正确。

## 2. 关键架构事实

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

### 1.2 `analyzer.c`：`check_symbol_conflicts` 第二轮循环，约 10 行替换

`imported_query_has_module` 内部改为调用 `get_module`（返回非 NULL 即为存在）。外部模块命中后：

```c
const FengSemanticModule *ext = imported_query->get_module(
    imported_query->user, use_decl->segments, use_decl->segment_count);
if (ext != NULL) {
    if (use_decl->has_alias) {
        // 别名路径：注册 AliasEntry，不导入名字（与内部模块别名路径一致）
        ok = append_alias_entry(..., ext, use_decl, ...);
    } else {
        // 非别名路径：与内部模块完全同路径
        ok = import_public_names(ext, program, use_decl, ...);
    }
    continue;
}
// ext == NULL → 报错：模块未找到
```

语义核心**无新逻辑**：`import_public_names` 和 `append_alias_entry` 一行都不改。

### 1.3 `frontend.c`：合成上下文（SyntheticCtx）+ `provider_get_module` 回调

新增一个 `SyntheticCtx` 结构，负责：
1. 将 `FengSymbolImportedModule*` 映射为 `FengSemanticModule*`（按路径缓存，同一模块多次 use 返回同一指针）
2. 为每个公开 decl 构造最小合成 `FengDecl`（仅填 `kind`/`visibility`/`name`，`body = NULL`，`token = {0}`）
3. 将合成 decl 数组包进合成 `FengProgram`，再包进合成 `FengSemanticModule`
4. `feng_cli_frontend_run` 语义分析结束后统一 `free` 所有合成对象

`provider_get_module` 回调实现：

```c
static const FengSemanticModule *provider_get_module(
    const void *user, const FengSlice *segments, size_t segment_count)
{
    SyntheticCtx *ctx = (SyntheticCtx *)user;
    // 1. 查缓存
    // 2. cache miss → find_module → build synthetic FengSemanticModule
    // 3. 存入缓存并返回
}
```

合成对象生命周期：在 `feng_cli_frontend_run` 栈上或堆上分配，语义分析完成后 `free`，不传出。

### 1.4 测试

- 语义单测：`use` 外部模块（type、spec、function、binding），名字在可见表中，不报 undefined
- 语义单测：`use ext.m as M;` 别名注册成功，`M.SomeType` 路径可解析（若 resolver 已支持别名 qualified 访问则测，否则仅验不报错）
- 语义单测：外部模块不存在时报正确错误（回归验证 `module_exists` 降级路径）
- `make test` 全量回归

---

## Phase 2 — 语义：完整签名合成

**目标：** 外部函数调用参数/返回类型检查通过；外部类型字段读写类型检查通过；外部 spec 约束检查通过。

### 2.1 扩展 `SyntheticCtx` 合成逻辑

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

### 2.3 合成对象内存管理

所有合成的 `FengTypeRef`、`FengParameter`、`FengTypeMember`、字符串（name 的拷贝）均由 `SyntheticCtx` 统一持有，语义分析结束后整体 `free`。

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

**改动一：** `semantic.h` `FengSemanticModule` 加标志：

```c
bool is_external_package;  /* true = 来自外部 .fb 包，codegen 跳过 body */
```

**改动二：** `frontend.c` Phase 1 的合成 `FengSemanticModule` 构建时置 `is_external_package = true`。

**改动三：** `codegen.c` 遍历 `analysis->modules[]` 时：
- `is_external_package == true`：跳过 body 生成；对消费者实际引用的外部符号 emit extern 声明：
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

## 5. 改动文件汇总

| 文件 | Phase | 说明 |
|---|---|---|
| `src/semantic/semantic.h` | 1、3b | `module_exists` → `get_module`（合并）；加 `is_external_package` 标志 |
| `src/semantic/analyzer.c` | 1 | `check_symbol_conflicts` 第二轮，约 10 行替换 |
| `src/cli/frontend.c` | 1、2、4 | SyntheticCtx 构建与释放；bundle_paths 收集；outputs 字段 |
| `src/codegen/codegen.c` | 3a、3b | LIB 模式 public 函数去 static；extern 声明 emit |
| `src/cli/compile/direct.c` | 4 | 透传 bundle_paths |
| `src/cli/compile/driver.h` | 4 | FengCliDriverOptions 加 bundle_paths 字段 |
| `src/cli/compile/driver.c` | 4 | 拓扑排序 + 解压 .a + 链接命令 |

语义核心（`analyzer.c`、`import_public_names`）**零新逻辑**，只在已有函数调用点做分支路由。

## 6. 验收口径

| Phase | 验收条件 |
|---|---|
| 1 | `use ext.mod;` 和 `use ext.mod as M;` 不报错；外部名字在可见表；`make test` 通过 |
| 2 | 外部函数调用/字段访问类型检查正确；签名不匹配时报正确错误；`make test` 通过 |
| 3a | LIB 模式 public 函数无 `static`；`make test` 通过 |
| 3b | 消费者生成 C 能通过 `clang -c`；`make test` 通过 |
| 4 | 端到端：build → pack → consume → run 全链路正确；`make test` 通过 |
