# Feng 异常系统开发文档

> 规范来源：[docs/specifications/feng-exception.md](../specifications/feng-exception.md)
> 本文记录运行时机制、实现原理与待办任务。  
> **交付范围**：仅覆盖 `throw` / `try` / `catch` / `unknown` 的完整实现。`match` 类型收窄与 `defer` 另立文档单独交付，本文不涉及。
>
> **当前演进**：异常载荷许可集合只由
> [异常模型主规范](../specifications/feng-exception.md#33-catch) 定义；统一箱表示、descriptor 不变量、实施顺序与
> 阶段门禁由 [统一 ValueBox 与 throw/catch 对齐方案](./feng-unified-value-box-and-exception-dev.md) 定义。
> 本文不另行定义语言规则。§6 保留异常系统首次交付的历史记录，其中已经被统一方案替代的条目不代表
> 当前实现目标。

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
- **`_Unwind_RaiseException` / `_Unwind_Resume_or_Rethrow`**：libunwind 提供的抛出与重抛入口，分别由
  `feng_throw` 与 `feng_rethrow` 调用。`_Unwind_Resume` 只用于 landing pad 继续同一次清理阶段，不能用于
  已进入 catch 后重新搜索外层 handler。

当前 vendoring 选择：

- 仓库内 `third_party/libunwind` 采用 **LLVM libunwind 20.1.8** 的最小源码闭包。该实现包含 `libunwind.cpp`，因此不是纯 C 依赖。
- `https://github.com/libunwind/libunwind` 是 C 为主的 libunwind 实现，并提供 `_Unwind_*` API；但其 README 和源码布局主要面向 ELF。2026-05-24 在 macOS/arm64 上验证，默认构建因 Darwin 缺少 ELF/link.h 相关接口、GNU alias 与 ucontext 约束失败，不能作为当前 Darwin 后端的直接替换。
- LLVM libunwind 源码包含 Windows/SEH 相关实现，主要可用于 MinGW/SEH 场景；Feng 当前生成 C 后端使用 GNU label address、DWARF/Mach-O/ELF unwind metadata 与 `_Unwind_*`，不能视为已支持 MSVC/Windows。Windows 仍需独立 SEH 后端设计与验证。
- 构建上 `scripts/build_libunwind.sh` 先单独产出 `extlib/<platform>/libfeng_unwind.a`；根 `Makefile` 再将该 archive 解包并合入 `build/lib/<platform>/libfeng_runtime.a`。完整平台、SDK / sysroot 与归档匹配规则统一见 [feng-release-and-install.md](feng-release-and-install.md) §8.3–§8.4；生成程序与 CLI driver 的稳定链接面仍只有对应平台的 `libfeng_runtime`。

**正常路径**：`try` 入口无任何代码，PC 区间在 LSDA 中隐式标记 try 范围，完全零开销。  
**抛出路径**：`feng_throw` → `_Unwind_RaiseException` → libunwind 逐帧调用 personality → personality 读 LSDA 匹配类型 → 跳转 landing pad。
**原样重抛路径**：`feng_rethrow` → `_Unwind_Resume_or_Rethrow` → libunwind 从当前 catch 外重新执行搜索与
清理阶段 → personality 继续以同一异常值和 descriptor 匹配外层 catch。重抛不分配新的异常或载荷，
不增加 Feng 自有分支、descriptor 映射或 ARC/CC 操作；重新搜索外层 handler 必然执行外层 catch 的既有
descriptor 指针比较。

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

personality 指针编码必须与目标对象格式匹配：

- Mach-O 使用 `0x9b`（`indirect + pcrel + sdata4`），由 Darwin 链接器通过间接符号指针解析。
- ELF 使用 `0x1b`（`pcrel + sdata4`），直接指向 `__feng_personality_v0`。ELF 不得对函数符号使用带 `indirect` 位的 `0x9b`；否则 libunwind 会把 personality 函数开头的机器指令误读为二级指针并跳转到无效地址。

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

- **macOS / Linux**：内部调用 `_Unwind_RaiseException` / `_Unwind_Resume_or_Rethrow`，实现 Feng personality
  函数。仓库 vendored LLVM libunwind 同时提供这两个入口。
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

`feng_throw` **堆分配** `FengUnwindException`（`_Unwind_RaiseException` 在展开过程中会销毁 throw 所在帧的栈，栈分配会导致对象失效），将主规范允许的抛出值规范化为托管指针后写入 `.value`，再调用 `_Unwind_RaiseException`。异常被 catch 消费后，landing pad 代码负责释放该堆对象：

| 抛出值形态 | 规范化方式 |
|---|---|
| 普通实体 `type`、普通 `@abi type` | 原托管对象指针 |
| `string` | 原字符串对象指针 |
| 标量与 `bool` | 对应静态具体 `ValueBox<T> *` |
| 具名 enum | 该具名 enum 的 `ValueBox<Enum> *` |
| 具名 tuple、`@value type`、`@value @abi type` 及闭合泛型 Value | 对应静态具体 `ValueBox<T> *` |

array、所有 spec、开放泛型、pointer、`void` 与 callable 在 Semantic 阶段拒绝，不进入规范化。每条允许
路径都必须满足 `((const FengManagedHeader *)value)->desc == desc`；编译器静态选定对象或箱 descriptor，
不执行运行时查找、映射或按载荷内容分类。

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
- 普通实体与 `string` 的 `catch ex: T` 直接绑定对象指针。
- 标量、enum、tuple 与 `@value type` 的 `catch ex: T` 按静态 `T` 从对应 `ValueBox<T>.value` 解箱。
- `catch ex: unknown` → `ex` 静态类型为 `unknown`，只允许 `throw ex`，不可访问字段或方法。

personality 只执行一次 `clause.type == ex_desc` 指针比较，不读取 descriptor 或箱内容，不执行类型查找。

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
                feng_rethrow();  // 原样重抛，以同一 FengUnwindException 重新搜索外层 handler
        }
    }
    __after_1:;
}
```

多个 catch 子句共享同一个 landing pad；personality 在搜索阶段选定命中子句索引并记录于 `FengUnwindException.matched_clause`，landing pad 读取后 switch 分派到正确的 catch 块。`feng_release_unwind_exception()` 仅在异常被消费（未重抛）时调用，释放 `feng_throw` 堆分配的 `FengUnwindException`。

---

## 6 首次交付记录（历史）

首次交付建立了 `throw`、`try <expr>`、typed/multi catch、`unknown`、LSDA/personality、异常帧清理及
macOS/Linux libunwind 路径。原始逐项清单曾记录共享 `FengScalarBox`、允许 object-form spec throw、
`catch` 必填等过渡实现；这些条目均已被后续规范与统一 ValueBox 方案替代，因此不再保留为当前 TODO
或完成断言。

当前语言规则只见[异常模型主规范](../specifications/feng-exception.md)，当前箱表示、descriptor 不变量、
实现决策、测试矩阵与最终回归结果只见
[统一 ValueBox 与 throw/catch 对齐方案](./feng-unified-value-box-and-exception-dev.md#8-分步实施-todo)。
Windows 后端仍是独立于本次统一工作的后续设计范围。
