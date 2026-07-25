# Feng 异常系统开发文档

> 规范来源：[docs/feng-exception.md](../docs/feng-exception.md)  
> 本文记录运行时机制、实现原理与待办任务。  
> **交付范围**：仅覆盖 `throw` / `try` / `catch` / `unknown` 的完整实现。`match` 类型收窄与 `defer` 另立文档单独交付，本文不涉及。

---

## 0 设计约束：正常路径零开销（强制）

**正常执行路径（无异常抛出）上，异常系统不得引入任何运行时开销。**

- `try` 入口不执行任何代码，无寄存器保存，无 push/pop。
- 所有用于支持异常展开的元数据以**静态只读表**的形式存在，不参与正常路径执行。

### 实现机制：LSDA 静态表 + libunwind

Feng 采用基于静态 LSDA 表 + libunwind 的零开销异常机制：

- **`.eh_frame` CFI**：C 编译器（Clang/GCC）自动为每个函数生成，描述寄存器还原规则，Feng 不需要手动生成。
- **LSDA**（Language-Specific Data Area）：由 Feng codegen 在生成的 C 文件中以静态 struct 输出，描述 try 的 PC 区间、landing pad 地址、catch 类型列表。
- **Personality 函数**（`__feng_personality_v0`）：libunwind 在展开过程中逐帧调用，读取 LSDA 决定是否在当前帧 catch、跳转到哪个 landing pad。
- **`_Unwind_RaiseException` / `_Unwind_Resume`**：libunwind 提供的展开入口，由 `feng_throw` 调用。

当前 vendoring 选择：

- 仓库内 `third_party/libunwind` 采用 **LLVM libunwind 20.1.8** 的最小源码闭包。该实现包含 `libunwind.cpp`，因此不是纯 C 依赖。
- `https://github.com/libunwind/libunwind` 是 C 为主的 libunwind 实现，并提供 `_Unwind_*` API；但其 README 和源码布局主要面向 ELF。2026-05-24 在 macOS/arm64 上验证，默认构建因 Darwin 缺少 ELF/link.h 相关接口、GNU alias 与 ucontext 约束失败，不能作为当前 Darwin 后端的直接替换。
- LLVM libunwind 源码包含 Windows/SEH 相关实现，主要可用于 MinGW/SEH 场景；Feng 当前生成 C 后端使用 GNU label address、DWARF/Mach-O/ELF unwind metadata 与 `_Unwind_*`，不能视为已支持 MSVC/Windows。Windows 仍需独立 SEH 后端设计与验证。
- 构建上 `scripts/build_libunwind.sh` 先单独产出 `extlib/<platform>/libfeng_unwind.a`；根 `Makefile` 再将该 archive 解包并合入 `build/lib/<platform>/libfeng_runtime.a`。完整平台、SDK / sysroot 与归档匹配规则统一见 [feng-release-and-instanll.md](feng-release-and-instanll.md) §8.3–§8.4；生成程序与 CLI driver 的稳定链接面仍只有对应平台的 `libfeng_runtime`。

**正常路径**：`try` 入口无任何代码，PC 区间在 LSDA 中隐式标记 try 范围，完全零开销。  
**抛出路径**：`feng_throw` → `_Unwind_RaiseException` → libunwind 逐帧调用 personality → personality 读 LSDA 匹配类型 → 跳转 landing pad。

### Landing pad 地址获取

生成的 C 代码中用 GCC/Clang 扩展 `&&label` 取 landing pad 及区间地址，编译器将其求值为链接时可重定位的 `void*`。

**关键约束**：`&&label` 只能引用**同一函数内**的 label，因此 LSDA 必须声明为函数内的 `static` 局部变量（GNU 扩展允许将同函数内的 label 地址用作局部 `static` 的初始化器，链接器生成重定位）。**不得**声明为文件作用域的全局 `static`（label 地址对文件作用域不可见）：

```c
void generated_fn(void) {
    /* LSDA 声明为函数内 static local（GNU 扩展，&&label 仅在同函数内有效） */
    static const FengCatchClause __clauses[] = { ... };
    static const FengLSDA __lsda = {
        .pc_begin    = &&__try_begin_1,   /* 函数内 label 地址，链接器重定位 */
        .pc_end      = &&__try_end_1,
        .landing_pad = &&__lp_1,
        .clauses     = __clauses,
        .clause_count = N,
    };
    /* 模块初始化时注册 &__lsda（见下文） */
    __try_begin_1:;
    /* ... try body ... */
    __try_end_1:;
    goto __after_1;
    __lp_1:;
    /* ... landing pad ... */
    __after_1:;
}
```

Windows 后端不能复用 GNU label address；后续需要以 SEH 或 Windows 可用的等价机制替换此层，平台层以外不改变 Feng 语义。

### Personality 函数注入

C 函数默认不携带自定义 personality。当前后端在每个 Feng 生成函数入口发射平台相关 `.cfi_personality` / `.cfi_lsda` inline asm，使 `_Unwind_RaiseException` 能在 thrower、中间帧与 handler frame 上调用 `__feng_personality_v0`。

2026-05-24 Darwin/arm64 POC 结论：只给含 try/catch 的函数注入 EH CFI 不足以展开；如果 thrower 或中间 Feng 函数没有 personality/FDE，`_Unwind_RaiseException` 会返回 `_URC_END_OF_STACK` 或跳过 Feng personality。因此所有可能被展开穿过的 Feng 生成函数都必须带 EH metadata，哪怕函数本身不含 try/catch。

### 编译参数要求

Feng 当前编译目标是 C；是否需要异常展开编译参数取决于后端机制，而不是 C/C++ 语言种类。

LSDA + libunwind 后端中，含 try/catch 的 Feng 生成 C 文件必须生成完整 unwind metadata。默认要求以 **`-fexceptions`** 编译；若改用 `-funwind-tables` / `-fasynchronous-unwind-tables` 等等价组合，必须先在目标编译器与平台上验证 `.eh_frame` / FDE / CFI 足以支撑 `_Unwind_RaiseException` 跨帧展开，再更新本文档与构建逻辑。`-fexceptions` 的作用：

- 使 C 栈帧生成完整 CFI（完整的 `.eh_frame` 条目），确保 libunwind 能正确还原中间帧的寄存器。
- **不**自动从 `goto`/label 结构生成 LSDA；LSDA 由 Feng codegen 显式输出为静态 struct。

不加此 flag 时，中间帧的 CFI 不完整，libunwind 无法安全展开，行为未定义。

### 跨平台封装

平台差异完全收在少数几个 helper 函数内部，codegen 与 Feng 代码不感知平台：

```c
// src/runtime/feng_exception_platform.h
void  feng_throw(void *value, const FengTypeDescriptor *desc);
void *feng_caught_value(void);   // landing pad 入口处取得被捕获的异常值
int   feng_caught_clause(void);  // landing pad 入口处取得命中子句索引（多 catch 时使用）
void  feng_rethrow(void);        // unknown catch 块中重抛
```

- **macOS / Linux**：内部调用 `_Unwind_RaiseException` / `_Unwind_Resume`，实现 Feng personality 函数。
- **Windows**：尚未交付。LLVM libunwind 有 Windows/SEH 源码，但当前 Feng C 后端的 GNU label address + DWARF/Mach-O/ELF LSDA 方案不能直接迁移到 MSVC/SEH。

### 托管局部展开清理

展开路径（throw → catch 之间）必须正确释放各帧内的托管局部。清理机制的改动分两阶段进行。

#### 本次交付（异常系统）

**中间帧**（位于 throw 与 catch 之间、自身无 try-catch 的函数帧）：

复用现有 TLS cleanup chain，新增 **TLS 帧标记**（`FengFrameMarker`）提供帧边界信息：
- Runtime 新增 `FengFrameMarker` 节点类型，插入同一 TLS 链，仅作帧边界标识，无 slot 指针。
- Codegen 在每个 Feng 生成函数入口发射 `feng_frame_push`，出口发射 `feng_frame_pop`。
- Personality 函数在 CLEANUP_PHASE 中，从链顶逐个 pop + 释放，遇到 `FengFrameMarker` 时 pop 该标记并返回 `_URC_CONTINUE_UNWIND`，完成本帧清理。
- 现有 `feng_cleanup_push` / `feng_cleanup_pop` 及 codegen 生成逻辑**完全不变**。

```c
/* personality CLEANUP_PHASE 中间帧处理伪代码 */
while (g_cleanup_top && !is_frame_marker(g_cleanup_top)) {
    FengCleanupNode *n = g_cleanup_top; g_cleanup_top = n->prev;
    if (n->slot && *n->slot) { feng_release(*n->slot); *n->slot = NULL; }
}
if (g_cleanup_top) g_cleanup_top = g_cleanup_top->prev; /* pop 帧标记 */
return _URC_CONTINUE_UNWIND;
```

**try 帧内托管局部**（含 try/catch 的函数，位于 try 体内的托管局部）：

- Codegen 在 try 入口额外压入 `feng_try_frame_push` 标记；正常路径 try 体末尾弹出该标记。
- Landing pad 入口调用 `feng_frame_release_to(&try_marker)`，释放并弹出 try marker 之上的 cleanup nodes，再进入 catch dispatch。

```c
/* landing pad 内 try 体托管局部清理（codegen 生成，dispatch 之前） */
__lp_1:; {
    feng_frame_release_to(&try_marker);
    switch (feng_caught_clause()) { /* ... */ }
}
```

#### 推迟到 defer 交付时统一处理

`defer` 与托管局部清理共享同一套基础设施，两者应一起设计、一起交付：

- 全量切换 `__attribute__((cleanup(feng_release_ptr)))`，替换所有函数所有托管局部声明（约 20+ codegen 调用点）。
- 移除 `FengCleanupNode` / `FengFrameMarker` / `feng_cleanup_push` / `feng_cleanup_pop` 整套 runtime 机制及相应 codegen 逻辑。
- `defer` 块以同一套 cleanup 机制实现（每个 defer 块生成捕获变量的结构体 + `__attribute__((cleanup))` ）。
- **平台确认**：Windows 保持使用 Clang，`__attribute__((cleanup))` 在 macOS / Linux / Windows (Clang) 均可用，无兼容性顾虑。

---

## 1 运行时结构

### 1.1 in-flight 异常对象（FengUnwindException）

libunwind 要求抛出对象首字段为 `_Unwind_Exception`，Feng 在其后追加自身需要的字段：

```c
typedef struct FengUnwindException {
    struct _Unwind_Exception  unwind;         // libunwind 要求，必须首字段，含 exception_class 与 cleanup func
    void                     *value;          // 规范化后的抛出值（托管指针，见 §2）
    const FengTypeDescriptor *desc;           // 抛出值的类型描述符，用于 catch 匹配
    int                       matched_clause; // personality 在 CLEANUP_PHASE 写入命中子句索引，landing pad 读取后分派
} FengUnwindException;
```

`exception_class` 填写 Feng 标识符（`uint64_t`，8 字节，值为 `"FENGEXN\0"`，即 F/E/N/G/E/X/N/\0），personality 函数据此区分 Feng 异常与其他语言异常。

### 1.2 LSDA 静态展开表（codegen 生成）

```c
typedef struct FengCatchClause {
    const FengTypeDescriptor *type;   // NULL 表示 unknown（兜底匹配）
} FengCatchClause;

typedef struct FengLSDA {
    const void            *pc_begin;     // try 区间起点（&&label）
    const void            *pc_end;       // try 区间终点（&&label）
    const void            *landing_pad;  // landing pad 地址（&&label）
    const FengCatchClause *clauses;      // catch 子句类型列表，按源码顺序
    int                    clause_count;
} FengLSDA;
```

每个含 try/catch 的函数有一个静态 `FengLSDA` 数组（支持同函数内多个 try 表达式，含嵌套）。Personality 函数按当前 PC 在数组中二分查找。

### 1.3 类型描述符（FengTypeDescriptor）

```c
typedef struct FengTypeDescriptor {
    const char                       *name;
    size_t                            size;
    FengFinalizerFn                   finalizer;
    FengReleaseChildrenFn             release_children;
    bool                              is_potentially_cyclic;
    size_t                            managed_field_count;
    const FengManagedFieldDescriptor *managed_fields;
} FengTypeDescriptor;
```

- **描述符指针地址即类型身份**：catch 类型匹配通过比较 `desc` 指针实现，O(1)。
- 不含 spec impl 表，因此 catch 不支持按 spec 类型匹配——这是规范约束的技术根因。

---

## 2 throw 入口规范化

`feng_throw` **堆分配** `FengUnwindException`（`_Unwind_RaiseException` 在展开过程中会销毁 throw 所在帧的栈，栈分配会导致对象失效），将抛出值规范化为托管指针后写入 `.value`，再调用 `_Unwind_RaiseException`。异常被 catch 消费后，landing pad 代码负责释放该堆对象：

| 抛出值形态 | 规范化方式 |
|---|---|
| 具体 `type` 实例（托管对象） | 直接作为 `void*` |
| `string` / `array` | 同上 |
| `spec` fat value（双指针：subject + witness） | 提取 subject，丢弃 witness |
| 标量（`i8`…`f64`）/ `bool` | 堆分配 `FengScalarBox` 写入值，将 box 指针作为 `void*` |

**spec witness 为何可丢弃**：witness 是 `(具体类型 → spec)` 的静态映射。catch 只按具体类型匹配（`desc` 指针比较），不需要 witness，故丢弃合法。

---

## 3 catch 类型匹配机制

Personality 函数在搜索阶段（`_UA_SEARCH_PHASE`）按 LSDA 子句顺序逐一匹配：

```
ex_desc = feng_unwind_ex->desc

for each FengCatchClause in LSDA（按源码顺序）:
    if clause.type == NULL:
        → 匹配成功（unknown 兜底）
    else if clause.type == ex_desc:
        → 匹配成功（类型命中）
```

匹配成功 → personality 返回 `_URC_HANDLER_FOUND`，记录命中子句索引。  
清理阶段（`_UA_CLEANUP_PHASE`）→ personality 将命中子句索引写入 `FengUnwindException.matched_clause`，再调用 `_Unwind_SetIP` 跳转至 `landing_pad`。Landing pad 通过 `feng_caught_clause()` 读取该索引，按索引分派到对应 catch 块。

**Landing pad 入口**：
- `feng_caught_value()` 从当前 `FengUnwindException` 中取出 `.value`。
- `catch ex: T` → `ex` 绑定为 `T*`（直接转型）。
- `catch ex: unknown` → `ex` 静态类型为 `unknown`，只允许 `throw ex`，不可访问字段或方法。
- 标量（`FengScalarBox`）通过 `FengBuiltinScalarKind` 区分具体标量种类，在具体类型 catch 中进一步细分。

---

## 4 `unknown` 关键字

- `unknown` 是语言关键字（与 `void` 同级），词法阶段识别，不可用作标识符。
- **作用域限制**：仅在 `catch` 子句的类型注解位置合法；出现在 `let`、函数参数、返回类型、字段类型等位置时语义非法，语义分析阶段报错。
- **运行时对应**：`FengCatchClause.type == NULL`，personality 函数对其无条件匹配。
- `catch ex: unknown` 中的 `ex` 标记为 `unknown` 类型，语义层禁止方法调用与字段访问，仅允许 `throw ex`。

---

## 5 生成的 C 代码结构

```c
// Feng 源码：
//   let result = try {
//       let inner: Baz = make_baz()     // try 体内托管局部
//       some_expr(inner)
//   } catch err: Foo { alt } catch ex: unknown { throw ex }

void generated_fn(void) {
    /* LSDA 声明为函数内 static local：&&label 仅在同函数内有效（GNU 扩展） */
    static const FengCatchClause __clauses_1[] = {
        { &Foo_descriptor },
        { NULL },                // unknown（兜底）
    };
    static const FengLSDA __lsda_1[] = {
        { &&__try_begin_1, &&__try_end_1, &&__lp_1, __clauses_1, 2 },
    };
    /* 模块初始化时以 &__lsda_1 注册自定义 FDE（见 §0 Personality 函数注入） */

    /* try 体内托管局部：NULL 初始化，landing pad 入口处显式释放 */
    /* （try 体外局部由现有 TLS cleanup chain 维护，本次不变） */
    BazType *inner = NULL;

    /* --- 正常路径：无任何 try 相关代码 --- */
    __try_begin_1:;
    inner = make_baz();
    result = some_expr(inner);
    feng_release(inner); inner = NULL;   /* 正常路径显式释放 try 体内局部 */
    __try_end_1:;
    goto __after_1;

    /* --- 异常路径：landing pad（personality 在 CLEANUP_PHASE 跳来） --- */
    __lp_1:; {
        feng_frame_release_to(&try_marker); /* 清理 try 体内 cleanup nodes，dispatch 之前 */
        int __clause = feng_caught_clause();  // 读取 FengUnwindException.matched_clause
        void *__ex_val = feng_caught_value();
        switch (__clause) {
            case 0: {            // catch err: Foo
                FooType *err = (FooType *)__ex_val;
                result = alt;
                feng_release_unwind_exception(); // 释放堆分配的 FengUnwindException
                goto __after_1;
            }
            case 1:              // catch ex: unknown
                feng_rethrow();  // 重抛，FengUnwindException 生命期交回 libunwind
        }
    }
    __after_1:;
}
```

多个 catch 子句共享同一个 landing pad；personality 在搜索阶段选定命中子句索引并记录于 `FengUnwindException.matched_clause`，landing pad 读取后 switch 分派到正确的 catch 块。`feng_release_unwind_exception()` 仅在异常被消费（未重抛）时调用，释放 `feng_throw` 堆分配的 `FengUnwindException`。

---

## 6 TODO

当前状态：新版 `throw` / `try <expr>` / typed catch / multi catch / `unknown` 的词法、解析、语义与 C 后端 LSDA/libunwind 路径已落地；旧版 `try { ... } catch { ... } finally { ... }` 兼容语法已移除。Feng 尚未公开发布，后续只维护主规范中的 `try <expr> catch ...` 形态（`catch` 子句为必填，零 catch 在解析阶段报错）。剩余 TODO 聚焦正常路径开销继续收敛、Windows 后端与回归验证。

### 运行时机制

- [x] 定义 `FengUnwindException` 结构体（`src/runtime/feng_runtime.h`）
- [x] 定义 `FengLSDA` / `FengCatchClause` 结构体（`src/runtime/feng_runtime.h`）
- [x] 实现 `feng_throw` / `feng_caught_value` / `feng_caught_clause` / `feng_rethrow` / `feng_release_unwind_exception`
- [x] 实现 macOS/Linux `_Unwind_RaiseException` 路径与 `__feng_personality_v0`
- [x] Personality 函数：搜索阶段按 LSDA 子句顺序匹配 `desc` 指针；清理阶段将命中子句索引写入 `FengUnwindException.matched_clause`，再调用 `_Unwind_SetIP` 跳转 landing pad
- [x] LSDA 注册：当前以函数内 static `FengLSDA` + `feng_register_lsda` 注册；FDE/personality 通过生成函数入口 `.cfi_personality` / `.cfi_lsda` 注入
- [x] 定义 `FengFrameMarker` 节点类型，扩展 TLS cleanup chain 支持帧边界标记（`src/runtime/feng_runtime.h`）
- [x] 实现 `feng_frame_push` / `feng_frame_pop` / `feng_try_frame_push` / `feng_frame_release_to`（`src/runtime/feng_exception.c`）
- [x] Personality 函数 CLEANUP_PHASE 中间帧处理：从链顶释放托管局部至函数帧标记，pop 帧标记，返回 `_URC_CONTINUE_UNWIND`
- [ ] 验证正常路径（无异常抛出）汇编输出中无任何异常相关代码
- [x] 生成 C 文件以 `-std=gnu11 -fexceptions` 编译，为 LSDA 后端的 GNU label address 与 unwind metadata 做准备（`src/cli/compile/driver.c`）

### 词法 / 解析层

- [x] `src/lexer/token.h`：从 `FENG_KEYWORD_LIST` 中移除 `X(FINALLY, "finally")`
- [x] `src/lexer/token.h`：在 `FENG_KEYWORD_LIST` 中添加 `X(UNKNOWN, "unknown")`
- [x] `src/parser/parser.c`：新增表达式形式 `try <expr>`；旧块形式 `try { ... } catch { ... } finally { ... }` 已移除
- [x] `src/parser/parser.c`：实现多 `catch` 子句解析，每个子句须含 `id: Type` 注解
- [x] `src/parser/parser.c`：catch 子句类型注解支持 `unknown` token
- [x] `src/parser/parser.c`：`try` 至少须有一个 `catch` 子句；零 catch 的 `try <expr>` 在解析阶段报错（`'try' requires at least one 'catch' clause`）

### 语义层

- [x] `src/semantic/`：`unknown` 类型节点仅在 catch 子句类型注解位置合法，其余位置报语义错误
- [x] `src/semantic/`：`catch ex: unknown` 中 `ex` 标记为 unknown 类型，禁止方法调用、字段访问
- [x] `src/semantic/`：`throw` 表达式类型检查：拒绝 spec 类型值、函数类型值、成员方法
- [x] `src/semantic/`：`catch` 子句类型注解检查：拒绝 spec 类型、函数类型、成员方法

### Codegen 层

- [x] `src/codegen/codegen.c`：`cg_emit_throw` 新增标量装箱路径（分配 `FengScalarBox`，填充 kind 与 payload）
- [x] `src/codegen/codegen.c`：`cg_emit_throw` 新增 spec fat value 路径（提取 `.subject`，通过 `FengManagedHeader::desc` 运行时取描述符，丢弃 witness）
- [x] `src/codegen/codegen.c`：`cg_emit_try` 重写为表达式形式，emit `__try_begin` / `__try_end` / `__lp` labels 及静态 LSDA 数据
- [x] `src/codegen/codegen.c`：实现有类型 catch 的 landing pad 代码生成（`feng_caught_value()` + 转型绑定）
- [x] `src/codegen/codegen.c`：实现多 catch 子句的 landing pad 分派（按命中子句索引 if/else-if 跳转，避免 C switch 捕获 Feng break/continue）
- [x] `src/codegen/codegen.c`：`catch ex: unknown` 子句：绑定 ex，生成 `feng_rethrow()` 路径（当前为兼容层）
- [x] `src/codegen/codegen.c`：try 表达式作为右值，结果值正确穿透到外层（当前为兼容层）
- [x] `src/codegen/codegen.c`：void try 表达式作为语句时不生成非法 `void` 结果槽；void catch 块允许正常结束（当前为兼容层）
- [x] `src/codegen/codegen.c`：catch 块内 `return` 会在离开函数前释放当前异常对象并清理已打开的异常帧（当前为兼容层）
- [x] `src/codegen/codegen.c`：catch 块内 `break` / `continue` 会在跳出 try/catch 前释放当前异常对象并清理需要离开的异常帧（当前为兼容层）
- [x] `src/codegen/codegen.c`：try 体内托管局部清理——通过 `feng_try_frame_push` + `feng_frame_release_to` 机制实现（正常路径 `cg_release_scope` 释放；异常路径 landing pad 入口 `feng_frame_release_to(&try_marker)` 释放，等价于 NULL 初始化方案，但复用 cleanup chain）
- [x] `src/codegen/codegen.c`：landing pad 入口处 dispatch 之前调用 `feng_frame_release_to(&try_marker)` 清理 try 体内所有托管局部（NULL-safe）
- [x] `src/codegen/codegen.c`：在每个含托管局部的函数入口/出口发射 `feng_frame_push` / `feng_frame_pop` 帧标记（中间帧展开清理所需），通过 `cg_emit_function_eh_prologue` 实现

### 测试

- [x] 新增测试：throw 具体 type，catch 匹配具体类型
- [x] 新增测试：throw spec 类型值（提取 subject 后抛），catch 匹配具体类型
- [x] 新增测试：throw 标量（`i32` / `bool`），catch 匹配对应类型
- [x] 新增测试：多 catch 子句，按序匹配
- [x] 新增测试：`catch ex: unknown`，仅 `throw ex` 合法
- [x] 新增测试：`catch ex: unknown` 中访问字段/方法，期望语义错误
- [x] 新增测试：`unknown` 用于 `let` / 参数 / 返回类型，期望语义错误
- [x] 规范变更：throw 允许 spec fat value（提取 subject 抛出，catch 仍须实体类型）；语义层移除对 spec throw 的拒绝
- [x] 新增测试：throw 函数值，期望编译错误
- [x] 新增测试：try 表达式作为右值（赋值）
- [x] 新增测试：void try 表达式作为语句，含 void catch 块
- [x] 新增测试：catch 块内 `return` 可正确离开函数并释放异常对象
- [x] 新增测试：catch 块内 `break` / `continue` 可正确离开 try/catch 并继续循环控制流
- [x] 新增测试：零 catch 的 `try <expr>` 在解析阶段报错（parser 单元测试 `test_try_without_catch_is_rejected` + semantic 单元测试 `test_try_without_catch_is_rejected` 各一条）
- [x] 全量回归测试通过（兼容层）
