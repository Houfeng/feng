# Feng defer 开发文档

> 规范来源：[docs/feng-defer.md](../docs/feng-defer.md)
> 本文记录实现方案、运行时机制、codegen 接入点与待办任务。

---

## 0 设计约束

1. **正常路径零额外分配**：defer 注册时 closure 对象在栈上分配，不走托管堆，不参与 ARC。
2. **复用现有清理基础设施**：defer 与托管局部释放共享同一套 TLS cleanup chain + personality 遍历机制，不引入独立的 defer 链表或新的运行时展开路径。
3. **与托管局部释放对称**：defer 节点在 cleanup chain 上与 RELEASE 节点混合 LIFO，两套退出路径（codegen 静态 emit + runtime 动态遍历）调用同一份底层执行函数。
4. **defer 不逃逸**：defer 块只在注册它的作用域退出时执行一次，closure 对象的生命周期不超过该作用域，因此可以在栈上分配。

---

## 1 方案选择

### 1.1 方案 B：扩展现有 FengCleanupNode（选定）

在现有 TLS cleanup chain 上扩展节点类型，新增 `FENG_NODE_DEFER`，复用 personality CLEANUP_PHASE 遍历与 landing pad `feng_frame_release_to` 机制。

**选择理由**：

- 改动最小：runtime ~40 行，codegen ~150 行，不改现有托管局部释放与 try/catch 机制。
- 异常路径自动覆盖：personality CLEANUP_PHASE 已遍历 chain，DEFER 节点无需额外处理。
- 多 defer LIFO 天然满足：TLS chain 天然 LIFO。
- 与托管局部释放混合 LIFO：同一 chain 上 RELEASE 与 DEFER 按注册逆序混合释放，符合规范要求。
- 风险低：只增不减，回归面小。

### 1.2 未选方案

- **全量切换 `__attribute__((cleanup))`**：dev/feng-exception-dev.md 曾设想此方向，但属于大重构（20+ codegen 调用点 + landing pad 重写），不符合"即简单又完整"。`__attribute__((cleanup))` 在 Feng 异常（手动 libunwind 展开）路径上仍需手动 landing pad emit，优势不明显。**建议后续作为独立优化交付，本次不涉及。**
- **内联 emit（Swift/Zig 方式）**：codegen 在每个退出点（return / break / continue / throw / landing pad）前直接 emit defer 体代码。异常路径上 landing pad 需 emit defer 体，膨胀严重；且 codegen 需精确识别所有可能抛异常的点，复杂度大增。Feng 的异常机制（libunwind + personality 遍历 cleanup chain）决定了 runtime 必须能通过函数指针调用 defer 体，内联方案无法满足此要求。
- **Go 式独立 defer 链表**：引入与 cleanup chain 并行的独立 defer 链表，两套机制并存，架构冗余，且异常路径需额外遍历 defer 链表。

---

## 2 运行时变更

### 2.1 FengCleanupNode 层次重构

现有 `FengCleanupNode` 通过字段 NULL 判别节点类型（slot != NULL → SLOT，aggregate_desc != NULL → AGGREGATE，全 NULL → MARKER）。新增 DEFER 后引入显式 `kind` 字段，顶层分三类：

```bash
FengCleanupNode (cleanup chain 节点)
├─ FENG_NODE_MARKER     帧边界(函数/try)
├─ FENG_NODE_RELEASE    托管局部释放
│   ├─ slot 模式（单个托管指针）
│   └─ aggregate 模式（聚合值 + 描述符）
└─ FENG_NODE_DEFER      defer 块执行
```

DEFER 与 RELEASE 在 kind 层并列（两者都是"退出时执行的动作"），SLOT/AGGREGATE 是 RELEASE 内部的两种释放形式。DEFER 的语义是"执行用户代码"，与 RELEASE 的"释放托管内存"是不同概念，不应在枚举中与 SLOT/AGGREGATE 并列。

### 2.2 结构体变更

```c
typedef enum {
    FENG_NODE_MARKER,    /* 帧边界(函数/try) */
    FENG_NODE_RELEASE,   /* 托管局部释放(slot/aggregate 内部判别) */
    FENG_NODE_DEFER,     /* defer 块执行 */
} FengChainNodeKind;

typedef struct FengCleanupNode {
    struct FengCleanupNode *prev;
    FengChainNodeKind kind;
    /* RELEASE 节点字段（SLOT 与 AGGREGATE 内部判别沿用现有逻辑） */
    void **slot;
    void *aggregate_value;
    const FengAggregateDescriptor *aggregate_desc;
    /* DEFER 节点字段 */
    void (*defer_fn)(void *);
    void *defer_closure;
} FengCleanupNode;
```

SLOT/AGGREGATE 的内部判别保留现有方式（slot != NULL → SLOT，aggregate_desc != NULL → AGGREGATE），只加顶层 kind 字段区分三类节点。现有 `feng_cleanup_push` / `feng_cleanup_push_aggregate` 内部自动设 `kind = FENG_NODE_RELEASE`，调用方无感。

### 2.3 新增 API

```c
/* 注册 defer 节点到 cleanup chain */
void feng_defer_push(FengCleanupNode *node,
                     void (*fn)(void *),
                     void *closure);
```

实现与 `feng_cleanup_push` 类似：设 `kind = FENG_NODE_DEFER`，填 `defer_fn` / `defer_closure`，prev 指向 `g_cleanup_top`，更新 `g_cleanup_top`。

### 2.4 feng_cleanup_release_node 变更

现有 `feng_cleanup_release_node`(feng_exception.c:23) 增加 DEFER 分支：

```c
static void feng_cleanup_release_node(FengCleanupNode *node) {
    if (node == NULL) return;
    switch (node->kind) {
    case FENG_NODE_DEFER:
        node->defer_fn(node->defer_closure);
        return;
    case FENG_NODE_RELEASE:
        /* 原有 slot/aggregate 释放逻辑不变 */
        if (node->aggregate_desc != NULL) {
            if (node->aggregate_value != NULL) {
                feng_aggregate_release(node->aggregate_value, node->aggregate_desc);
                node->aggregate_value = NULL;
            }
            return;
        }
        void **slot = node->slot;
        if (slot != NULL && *slot != NULL) {
            feng_release(*slot);
            *slot = NULL;
        }
        return;
    case FENG_NODE_MARKER:
        /* 帧标记不释放资源，由 feng_cleanup_release_to_frame_marker 处理 */
        return;
    }
}
```

### 2.5 边界检查更新：marker 判别从字段 NULL 改为 kind（必须）

引入 `FENG_NODE_DEFER` 后，原有依赖 `slot == NULL && aggregate_desc == NULL` 判别 marker 的不变量被破坏：DEFER 节点同样满足此条件（slot=NULL, aggregate_desc=NULL, defer_fn!=NULL）。两个使用此判别的函数必须改为 `kind == FENG_NODE_MARKER`，**此变更必须随本次 defer 交付一起完成，不是可选附带清理**。

#### 2.5.1 feng_cleanup_release_to_frame_marker (feng_exception.c:43)

```c
static void feng_cleanup_release_to_frame_marker(void) {
    while (g_cleanup_top != NULL) {
        FengCleanupNode *node = g_cleanup_top;
        g_cleanup_top = node->prev;
        if (node->kind == FENG_NODE_MARKER) {
            FengFrameMarker *marker = (FengFrameMarker *)((char *)node - offsetof(FengFrameMarker, node));
            if (marker->is_function_boundary) {
                return;
            }
            continue;
        }
        feng_cleanup_release_node(node);
    }
}
```

边界检查语义：仅当 `kind == FENG_NODE_MARKER` 时才 cast 到 `FengFrameMarker` 读取 `is_function_boundary`，决定停止（function boundary）或跳过（try boundary）；其他节点（DEFER / RELEASE）一律交给 `feng_cleanup_release_node` 处理。

未改时的失败模式（DEFER 节点命中此分支）：

- cast 到 `FengFrameMarker` 是 UB：DEFER 节点是独立栈分配的 `FengCleanupNode`（§5.5），未嵌入 `FengFrameMarker`，`is_function_boundary` 读到节点之前的栈垃圾。
- 栈垃圾非零 → `return` 提前结束 cleanup，该帧上层所有 DEFER 与托管局部全部泄漏。
- 栈垃圾为零 → `continue` 跳过该节点，defer_fn 永不调用，personality §6.1 声称的"自动执行 defer 体"不成立。

#### 2.5.2 feng_frame_pop (feng_exception.c:202)

```c
void feng_frame_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_frame_pop: chain underflow");
    }
    if (g_cleanup_top->kind != FENG_NODE_MARKER) {
        feng_panic("feng_frame_pop: top cleanup node is not a frame marker");
    }
    g_cleanup_top = g_cleanup_top->prev;
}
```

边界检查语义：栈顶必须是 `FENG_NODE_MARKER`，否则 panic。原字段判别 `slot != NULL || aggregate_desc != NULL` 会让 DEFER 节点（slot=NULL, aggregate_desc=NULL）通过校验，frame_pop 误把 DEFER 当 marker 弹出（不调用 defer_fn），chain 错位，后续 `feng_frame_release_to` / `feng_cleanup_release_to_frame_marker` 的指针/位置假设失效。

#### 2.5.3 不受影响的函数

- `feng_frame_release_to`(feng_exception.c:212)：按指针身份匹配目标 marker，遍历中间节点统一调用 `feng_cleanup_release_node`（已按 kind 分派），无需 marker 判别。
- `feng_cleanup_release_all`(feng_exception.c:58)：遍历到空，每个节点调用 `feng_cleanup_release_node`（MARKER 分支为 no-op），无需 marker 判别。
- `feng_cleanup_pop`(feng_exception.c:171)：仅弹栈顶不调用 release_node，由 codegen 保证 push/pop 配对，无需 kind 校验。

### 2.6 FengFrameMarker 适配

`FengFrameMarker` 嵌入 `FengCleanupNode` 作为链节点（通过 offsetof cast），新增 kind 字段后 `feng_frame_push` / `feng_try_frame_push` 内部需设 `kind = FENG_NODE_MARKER`。FengFrameMarker 结构不变，只加 kind 填充。

---

## 3 Lexer / Parser 变更

### 3.1 Lexer

`src/lexer/token.h` 关键字列表新增：

```c
X(DEFER, "defer")
```

### 3.2 Parser

新增 `FENG_STMT_DEFER` AST 节点类型。语法 `defer { ... }`，必须花括号块，不允许单语句形式。

defer 块的 body 复用现有 `FengBlock` 结构，与 `if` / `for` / `while` 的块体一致。

---

## 4 Semantic 变更

### 4.1 作用域处理

defer 块作为普通块作用域处理，可见性规则与 `if` / `for` / `while` 等其他块一致（规范 §1 已明确）。无需引入特殊可见性逻辑。

### 4.2 语句限制检查（规范 §4）

**强禁止**（任何位置都不允许，包括嵌套子块）：`return` / `throw` / `defer`。

遍历 defer 块 AST（含所有嵌套子块），遇到 `FENG_STMT_RETURN` / `FENG_STMT_THROW` / `FENG_STMT_DEFER` 一律报错。错误码待定（建议新增独立错误码，如 `SE0xxx` 或 `AE0xxx`）。

**弱限制**（仅直接位置禁止，嵌套 `for` / `while` 内允许）：`break` / `continue`。

检查 `FENG_STMT_BREAK` / `FENG_STMT_CONTINUE` 所在的循环深度：若不在任何循环内（即 defer 块直接位置），报错；若在嵌套 `for` / `while` 内，合法。

### 4.3 位置限制

defer 仅可出现在函数体或函数体内嵌套的块作用域中。出现在模块顶层、`type` / `enum` / `spec` / `fit` 声明体中时报错。

---

## 5 Codegen 变更

### 5.1 defer 块的代码生成策略

defer 体生成**静态 C 函数** + **栈上 closure 结构体**，注册到 TLS cleanup chain。

选择理由：

- 异常路径由 runtime personality 函数遍历 cleanup chain 调用 defer 体——必须能被 runtime 通过函数指针调用，只能是静态函数。
- 正常路径（codegen emit `feng_cleanup_pop(); fn(closure);`）与异常路径调用同一份底层函数，语义保证一致。
- 静态函数 + C 编译器内联优化：短 defer 体（如 `file.close()`）通常被 C 编译器在 -O2 下内联，性能不输内联方案；长 defer 体不被内联，避免代码膨胀。
- 与现有托管局部释放机制完全对称：RELEASE 的底层函数是 `feng_release(slot)`，DEFER 的底层函数是 `defer_fn(closure)`。

### 5.2 defer 静态函数生成

defer 体生成格式：

```c
static void __defer_<func_name>_<seq>(void *_closure) {
    struct __defer_closure_<func_name>_<seq> *_c =
        (struct __defer_closure_<func_name>_<seq> *)_closure;
    /* defer 体代码，通过 _c-><field> 访问外层绑定 */
}
```

命名规则：`__defer_<func_name>_<seq>`，`<seq>` 在函数内递增。`<func_name>` 取当前生成函数的 C 名称。

函数体生成走完整的 `cg_emit_block` 流程（含 line directive、scope 管理、嵌套语句）。

defer 函数在函数体外（文件作用域 static）生成，类似 lambda 的 `cg_emit_lambda_invoke_function`(codegen.c:11817)。codegen 需在函数体生成过程中收集 defer 函数，在函数体生成结束后追加到输出文件。

### 5.3 closure 结构体

```c
struct __defer_closure_<func_name>_<seq> {
    <type1> *<captured_var_1>;  /* 外层 var 的栈地址(引用捕获) */
    <type2> *<captured_var_2>;
    /* ... */
};
```

特点：

- **栈上分配**：closure 结构本身在 defer 注册位置所在的作用域栈上声明，生命周期不超过该作用域。defer 不逃逸，栈上分配安全。注意：closure 字段持有的是外层 var 的地址，该 var 的实际存储位置由其类型决定——标量直接在栈上，托管指针变量本身也在栈上（指向的堆内存由 ARC 管理），closure 不关心指向内容的位置，只持有 var 的地址。
- **引用捕获**：所有捕获字段都是外层 var 的指针（`<type> *`），直接持有外层 var 的地址。defer 执行时通过指针读写外层 var 的当前运行时值，而非注册时的快照。defer 块**允许修改外层 var**（与 Swift / Go / Zig 一致）——通过指针写即可。`let` 绑定也通过地址访问，但 `let` 不可变，语义上等价只读。
- **不参与 ARC**：closure 对象不是托管对象，无 `FengManagedHeader`，不参与引用计数。这是与 lambda closure 的核心区别：lambda 可逃逸（返回、存字段、异步调用），需要托管堆分配 + ARC；defer 不逃逸，栈上即可。
- **无需 capture cell**：lambda 的 var 捕获需要 capture cell（可变盒子）因为 lambda 可在多处调用且需共享可变状态；defer 只执行一次，直接持有 var 地址即可，通过地址读写实现修改外层 var。

### 5.4 捕获需求分析

分析 defer 体 AST，找出引用的外层绑定（不在 defer 块自身作用域内声明的绑定）。参考 lambda 的 `cg_compute_capture_requirements_in_lambda_body`(codegen.c:10993)思路：遍历 AST 的标识符引用，对每个引用检查是否属于外层作用域。

defer 捕获需求分析参考 lambda 的 `cg_compute_capture_requirements_in_lambda_body`(codegen.c:10993)思路，但不使用 lambda 的 capture cell 机制。defer 直接持有外层 var 的地址，通过地址读写实现修改外层 var。

**无外层绑定引用的 defer**：若 defer 体不引用任何外层绑定，closure 可为 NULL（`void *defer_closure = NULL`），defer 函数签名仍为 `void fn(void *closure)` 但内部不使用 closure 参数。这是常见场景的微优化（如 `defer { global_cleanup(); }`），但多数 defer 都会引用外层绑定，收益有限。

### 5.5 defer 注册 emit

在 defer 语句的 codegen 位置 emit：

```c
/* closure 结构体初始化(栈上) */
struct __defer_closure_<func_name>_<seq> __defer_closure_<func_name>_<seq> = {
    &<captured_var_1>,  /* 外层 var 的栈地址 */
    &<captured_var_2>,
};
/* 注册到 cleanup chain */
FengCleanupNode _defer_<func_name>_<seq>;
feng_defer_push(&_defer_<func_name>_<seq>,
                __defer_<func_name>_<seq>,
                &__defer_closure_<func_name>_<seq>);
```

### 5.6 scope 内 defer 注册管理

defer 注册追加到 scope 的 locals 列表（类似 `Local`，加 `is_defer` 标志），在 `cg_release_scope`(codegen.c:19232)按 LIFO 与托管局部混合释放。

`cg_release_scope` 遍历 locals 时，对 `is_defer` 类型的 local emit：

```c
feng_cleanup_pop();
__defer_<func_name>_<seq>(&__defer_closure_<func_name>_<seq>);
```

而非现有的：

```c
feng_cleanup_pop(); feng_release(x); x = NULL;
```

defer 节点的 `feng_cleanup_pop()` 与托管局部的 `feng_cleanup_pop()` 操作同一个 TLS chain，LIFO 顺序由注册顺序决定——先注册的节点（托管局部）在 chain 底部，后注册的（defer）在顶部。`cg_release_scope` 按 locals 逆序遍历，先 pop + 执行 defer，再 pop + 释放托管局部，或反之——取决于注册顺序。

### 5.7 cg_release_through 覆盖

`cg_release_through`(codegen.c:19292)用于 return / break / continue 的逐层释放，逐层调用 `cg_release_scope`。defer 注册在 scope 的 locals 列表中，`cg_release_through` 自动覆盖——无需额外逻辑。

### 5.8 cg_emit_stmt 新增分支

```c
case FENG_STMT_DEFER: ok = cg_emit_defer(cg, stmt); break;
```

`cg_emit_defer` 负责：

1. 分析 defer 体的捕获需求
2. 生成 closure 结构体定义（追加到 type_defs buffer）
3. 生成 defer 静态函数（追加到 defer 函数 buffer，函数体生成结束后合并到输出）
4. 在当前作用域 emit closure 初始化 + feng_defer_push
5. 在当前 scope 注册 defer（追加到 locals 列表，标记 is_defer）

---

## 6 异常路径上的 defer 执行

### 6.1 personality CLEANUP_PHASE

personality 函数 `__feng_personality_v0`(feng_exception.c:292)在 CLEANUP_PHASE 调用 `feng_cleanup_release_to_frame_marker()`(feng_exception.c:43)，从 TLS chain 顶逐个 pop + 释放节点。DEFER 节点的 `feng_cleanup_release_node` 分支调用 `defer_fn(defer_closure)`，自动执行 defer 体。

**personality 函数本身无需修改**：现有遍历逻辑覆盖 DEFER 节点。但其调用的 `feng_cleanup_release_to_frame_marker` 必须按 §2.5.1 更新 marker 判别为 `kind == FENG_NODE_MARKER`，否则 DEFER 节点会被误判为 marker 而跳过 defer_fn 调用。

### 6.2 landing pad

landing pad 在 `feng_frame_release_to(&marker)`(codegen.c:18949)释放到 try marker 之上的所有 cleanup nodes。DEFER 节点在此过程中被遍历并调用 `defer_fn(defer_closure)`。

**无需额外处理**：现有 landing pad 逻辑已覆盖。

### 6.3 defer 块内调用的外部函数抛出异常

规范 §4 禁止 defer 块内直接 `throw`，但 defer 块内调用的外部函数可能抛出异常。抛出后进入新的 libunwind 展开，personality 在后续帧的 CLEANUP_PHASE 继续遍历 chain。

defer 块内调用的外部函数抛出异常时，当前 defer 块的后续语句不再执行（与 try 体中抛出异常一致）。异常传播规则遵循 feng-exception.md。

---

## 7 控制转移路径上的 defer 执行

| 路径 | 现有释放机制 | defer 接入方式 |
| --- | --- | --- |
| 正常退出（块结束） | `cg_release_scope` 逐 local emit | defer 在 locals 列表中，按 LIFO 与托管局部混合 |
| break / continue | `cg_release_through(stop)` + `cg_release_scope(stop)` | 同上（通过 cg_release_scope 自动覆盖） |
| return | `cg_release_through(NULL)` | 同上 |
| throw | personality CLEANUP_PHASE + landing pad `feng_frame_release_to` | DEFER 节点在 chain 上，自动遍历调用 |

---

## 8 与 lambda closure 的对比

| 维度 | lambda closure | defer closure |
| --- | --- | --- |
| 生命周期 | 可逃逸（返回、存字段、异步调用） | 不逃逸，只执行一次 |
| 分配方式 | 托管堆（ARC 管理） | 栈上（随作用域自动回收） |
| 结构 | `FengManagedHeader` + invoke + capture cells | 纯字段列表（外层 var 的栈地址） |
| ARC 参与 | 是 | 否 |
| capture cell | 需要（var 捕获的可变盒子） | 不需要（直接持有栈地址） |
| release_children | 需要 | 不需要 |

defer closure 是"栈上不逃逸轻量闭包"，与 lambda 的"托管堆可逃逸闭包"是两种东西。捕获需求分析思路可参考 lambda（遍历 AST 找外层绑定），但 closure 结构体生成不复用 lambda 的 `cg_emit_lambda_closure_type`。

---

## 9 错误码

defer 相关的语义限制需要新增错误码（具体编号由人工决策）：

| 限制 | 建议类别 | 简述 |
| --- | --- | --- |
| defer 块内任何位置包含 return | 语义检查 | defer block cannot contain 'return' |
| defer 块内任何位置包含 throw | 语义检查 | defer block cannot contain 'throw' |
| defer 块内任何位置包含 defer | 语义检查 | defer block cannot contain nested 'defer' |
| defer 块直接位置包含 break | 语义检查 | defer block cannot directly contain 'break' |
| defer 块直接位置包含 continue | 语义检查 | defer block cannot directly contain 'continue' |
| defer 出现在非法位置(模块顶层/type/enum/spec/fit) | 语义检查 | 'defer' is only allowed inside function blocks |

---

## 10 待办任务

### 10.1 Runtime

- [ ] `FengCleanupNode` 加 `kind` 字段(`FengChainNodeKind`)
- [ ] `FengCleanupNode` 加 `defer_fn` / `defer_closure` 字段
- [ ] `feng_defer_push` API 实现（runtime 私有 ABI 扩展，需人工决策）
- [ ] `feng_defer_push` 声明添加到 `feng_runtime.h`（与 `feng_cleanup_push` 同位置）
- [ ] `feng_cleanup_release_node` 增加 `FENG_NODE_DEFER` 分支
- [ ] `feng_cleanup_push` / `feng_cleanup_push_aggregate` 内部设 `kind = FENG_NODE_RELEASE`
- [ ] `feng_frame_push` / `feng_try_frame_push` 内部设 `kind = FENG_NODE_MARKER`
- [ ] `feng_cleanup_release_to_frame_marker` marker 判别改为 `node->kind == FENG_NODE_MARKER`（必须，见 §2.5.1）
- [ ] `feng_frame_pop` 校验改为 `node->kind == FENG_NODE_MARKER`（必须，见 §2.5.2）
- [ ] 全量回归测试（异常路径 + 正常路径）

### 10.2 Lexer

- [ ] `token.h` 关键字列表新增 `DEFER`

### 10.3 Parser

- [ ] 新增 `FENG_STMT_DEFER` AST 节点类型
- [ ] `defer { ... }` 语法解析（必须花括号块）
- [ ] defer 块 body 复用 `FengBlock`

### 10.4 Semantic

- [ ] defer 块作为普通块作用域处理
- [ ] 强禁止检查：defer 块内任何位置的 `return` / `throw` / `defer`
- [ ] 弱限制检查：defer 块直接位置的 `break` / `continue`
- [ ] 位置限制检查：defer 仅在函数体/嵌套块作用域内合法
- [ ] 新增错误码

### 10.5 Codegen

- [ ] `cg_emit_stmt` 新增 `FENG_STMT_DEFER` 分支
- [ ] defer 体静态函数生成（参考 lambda invoke 函数生成模式）
- [ ] closure 结构体定义生成（栈上，引用捕获）
- [ ] 捕获需求分析（参考 lambda 的 capture requirements 分析思路）
- [ ] defer 注册 emit（closure 初始化 + `feng_defer_push`）
- [ ] scope locals 列表追加 defer 注册（`is_defer` 标志）
- [ ] `cg_release_scope` 对 defer local emit `feng_cleanup_pop(); fn(closure);`
- [ ] defer 静态函数在函数体生成结束后追加到输出
- [ ] 全量回归测试

### 10.6 测试

- [ ] 编译器测试(test/)：defer 语法解析、语义限制报错（return/throw/defer/break/continue）
- [ ] 兼容性测试(fcts/)：defer 基本行为（正常退出执行、LIFO 顺序）
- [ ] 兼容性测试(fcts/)：defer + 控制转移（return/break/continue 前 defer 执行）
- [ ] 兼容性测试(fcts/)：defer + 异常路径（throw 后 defer 执行）
- [ ] 兼容性测试(fcts/)：defer + try/catch（landing pad 内 defer 执行）
- [ ] 兼容性测试(fcts/)：defer 块内嵌套 for/while 中 break/continue 合法
- [ ] 兼容性测试(fcts/)：defer 捕获外层 var 的执行时当前值（而非注册时快照）
- [ ] 兼容性测试(fcts/)：多 defer LIFO 顺序
