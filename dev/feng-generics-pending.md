# Feng 泛型实现开发文档

> **状态**: 实现规划，待推进。
> 语言规范权威来源：[docs/feng-generics-draft.md](../docs/feng-generics-draft.md)
> 符号表规范权威来源：[docs/feng-symbol-table.md](../docs/feng-symbol-table.md)
> 前置任务（Phase 5.5 符号表重构）见 [dev/feng-plan.md](./feng-plan.md)。

---

## 一、开始实现前需要明确的规则

以下问题规范中尚未收口，必须由开发者决策后才能推进对应实现。  
**每项决策结果应直接更新到对应规范文档，再开始编码。**

---

### Q1 代码生成策略

**决策**：**布局单态化 + 方法共享**，现有非泛型发码路径零改动。

**核心思路**：值在结构体字段里，方法通过描述符了解字段的类型性质。两件事分别处理：

- **struct 布局**：按具体 T 在使用点单态化生成，字段大小由 T 决定
- **方法体**：只编译一份，通过 `void *self` + `FengGenericValueDescriptor *T` + `void *out` 操作

**struct 布局单态化**（编译器在使用点自动生成，用户无感知）：

```c
// type Box<T> { let value: T; }

// Box<int>
typedef struct { FengManagedHeader _hdr; int64_t value; } FengBox__int;

// Box<Widget>（Widget 是 spec，16 字节 fat value）
typedef struct { FengManagedHeader _hdr; struct FengSpecValue__Widget value; } FengBox__Widget;

// Box<(int, float)>（值类型 tuple，未来支持）
typedef struct { FengManagedHeader _hdr; int64_t value_0; double value_1; } FengBox__tuple_i64_f64;
```

**方法体只编译一份**（泛型专有发码，`void *self` + `FengGenericValueDescriptor *T` + out 参数）：

```c
// fn get(): T
void FengBox__get(void *self, const FengGenericValueDescriptor *T, void *out) {
    void *fp = (char *)self + sizeof(FengManagedHeader);   // 字段固定在 header 之后
    switch (T->kind) {
        case FENG_VALUE_TRIVIAL:                           break;
        case FENG_VALUE_MANAGED_POINTER:                   feng_retain(*(void **)fp); break;
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:      feng_aggregate_retain(fp, T->aggregate); break;
    }
    memcpy(out, fp, T->size);
}
```

**调用点**（编译器静态已知 T，直接写入目标变量或引入临时变量）：

```c
// Box<int>.get()
int64_t x;
FengBox__get(box, &feng_generic_trivial8_desc, &x);

// Box<Widget>.get()
struct FengSpecValue__Widget w;
FengBox__get(box, &feng_generic_Widget_desc, &w);

// 子表达式场景：let y = b.get() + 1
int64_t _tmp;
FengBox__get(box, &feng_generic_trivial8_desc, &_tmp);
int64_t y = _tmp + 1;
```

**`FengGenericValueDescriptor`（新增运行时结构）**：

```c
typedef struct FengGenericValueDescriptor {
    size_t          size;        /* T 占用的字节数（8/16/N×8） */
    FengValueKind   kind;        /* TRIVIAL / MANAGED_POINTER / AGGREGATE */
    const FengAggregateValueDescriptor *aggregate;  /* 仅 AGGREGATE 时非 NULL */
} FengGenericValueDescriptor;
```

各类 T 的静态实例（codegen 在使用点生成）：

```c
// 所有值类型（int/bool/float）共用一个实例
const FengGenericValueDescriptor feng_generic_trivial8_desc =
    { .size=8, .kind=FENG_VALUE_TRIVIAL, .aggregate=NULL };

// UserType — managed pointer，编译器为每个 T 生成
const FengGenericValueDescriptor feng_generic_Foo_desc =
    { .size=8, .kind=FENG_VALUE_MANAGED_POINTER, .aggregate=NULL };

// spec value — aggregate，复用已有 FengAggregateValueDescriptor，零重复
const FengGenericValueDescriptor feng_generic_Widget_desc = {
    .size      = sizeof(struct FengSpecValue__Widget),
    .kind      = FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS,
    .aggregate = &FengSpecAgg__Widget    // 直接引用现有结构，无重复数据
};
```

**与现有运行时结构的关系**：

| 结构 | 描述对象 | 是否改动 |
|---|---|---|
| `FengTypeDescriptor` | 堆上托管对象（有 header），用于 `feng_object_new`、ARC、Phase 1B GC | **零改动** |
| `FengAggregateValueDescriptor` | 栈上聚合值（无 header），用于 spec value ARC | **零改动，直接复用** |
| `FengGenericValueDescriptor` | 泛型参数位置上的值的操作描述 | **新增，约 10 行** |

**运行时开销**（vs 非泛型、vs spec dispatch）：

- 方法调用：**直接 call**（< spec dispatch 的 vtable 间接跳转）
- 字段操作：`memcpy(T->size)`，对 8 字节值被 C 编译器折叠为单条 MOV
- ARC：switch 三分支，trivial 分支立即跳出；分支预测友好
- 整体：比非泛型多约 1 store + 1 load（栈上 out 参数），比 spec dispatch **更快**

**对现有代码的改动**：
- 现有 `cg_register_user_type`、`cg_emit_user_method`、`cg_emit_function` 等：**零改动**
- 新增 `cg_emit_generic_type_instance`（使用点触发，生成 struct 定义）
- 新增 `cg_emit_generic_method`、`cg_emit_generic_fn`、`cg_emit_generic_call`
- 运行时新增 `FengGenericValueDescriptor` 结构定义 + `feng_generic_trivial8_desc` 实例
- `.fb` 分发：共享方法体预编译进 `lib/`，struct 定义在消费方生成，支持闭源分发

**规范更新**：`docs/feng-generics-draft.md` §7 需要按此方案重写（详见 G0-1）。

---

### Q2 约束目标是否可以是泛型 spec 实例

**决策**：**允许**。约束目标可以是任何可解析的 spec 引用，包括泛型 spec 的具体实例。

```feng
spec Reader<T> {
  fn get(): T;
}

type Box<T: Reader<int>> {   // 合法：约束目标是泛型 spec 的具体实例
  ...
}
```

**影响实现**：
- 语义分析中约束目标解析逻辑：约束目标按普通类型引用解析，支持 `NAMED_GENERIC` 节点（即带类型实参的 spec 引用）
- 父 spec 约束检查中，`spec Child<T>: Parent<int>` 的 `Parent<int>` 同样是泛型 spec 实例，按同样规则处理
- `.ft` 中 type_param 的约束 TYPS 节点可以是 `FT_TYPE_KIND_NAMED_GENERIC` 节点

---

### Q3 泛型 type 的默认零值规则

**决策**：**按字段递推**。泛型 type 的零值规则与非泛型 type 一致：将类型参数替换为实际类型实参后，若所有字段都有默认零值，则该泛型实例有默认零值（字段逐一取零值）。

```feng
type Box<T> {
  let value: T;
}

let b: Box<int>;   // 合法：value 取 int 零值（0），b 是 Box<int> 的零值实例
```

**影响实现**：
- 语义分析中的零值推导逻辑：将类型参数替换为实际类型实参后，按现有字段递推规则判断
- 若某字段的实际类型本身无零值（如某些带约束的 spec 类型），仍须显式初始化，诊断规则不变

---

### Q4 `>>` token 与嵌套泛型的歧义处理

**决策**：采用策略 1（Parser 内部 `pending_gt` 计数器），词法器零改动。

**问题背景**：词法器遇 `>` 后紧接 `>` 时，会整体产出 `FENG_TOKEN_SHR`（`>>`）（见 `src/lexer/lexer.c` `case '>'` 分支）。在 `Map<string, List<int>>` 这类嵌套泛型中，末尾 `>>` 整体被产出为 `SHR`，Parser 无法正确闭合两层 `<...>`。

**方案细化**：Parser 在类型实参解析上下文中维护一个 `int pending_gt` 计数器：

- 需要消费一个 `>` 时，先检查 `pending_gt > 0`：若是则 `pending_gt--`，不取新 token。
- `pending_gt == 0` 且当前 token 是 `SHR`（`>>`）：消费该 token，`pending_gt = 1`，本次得到一个 `>`。
- `pending_gt == 0` 且当前 token 是 `GT`（`>`）：正常消费。

`Map<string, List<int>>` 末尾 `>>` 先闭合内层 `List<int>`，再从 `pending_gt` 取第二个 `>` 闭合外层 `Map<...>`；表达式中的移位运算 `a >> b` 不在类型实参上下文中，`pending_gt` 不参与，不受影响。

---

### Q5 泛型推导的冲突报错规则

**决策**：从实参列表从左到右推导，第一个能确定类型参数的实参位置为准；后续实参若产生不匹配，在**该不匹配实参的位置**报编译错误（信息：参数类型不匹配，期望 `T = <已推导类型>`，实际 `<当前类型>`）。

```feng
fn pair<T>(left: T, right: T): T { ... }

pair(1, "x");
//       ^^^ 编译错误：right 期望类型 int（由 left 推导 T = int），实际类型 string
```

**规则细化**：
- 推导优先级：接收者静态类型（若有）> 实参列表从左到右 > 上下文目标类型（如赋值期望类型）
- 若所有信息都不足以确定某个类型参数，则在整个调用表达式处报"无法推导类型参数 T"
- 若某位置推导出的类型与已确定结论冲突，则在该不匹配的实参位置报错

---

## 二、任务拆解

以下任务列表按推荐实现顺序排列。每项任务完成后应补测试并执行全量回归。

> **前置**：Phase 5.5 符号表结构重构必须先完成。详见 `dev/feng-plan.md`。

---

### G0 规则收口（前置于一切编码）

| 编号 | 任务 | 产出 | 备注 |
| --- | --- | --- | --- |
| G0-1 | 决策 Q1 代码生成策略，写入 `docs/feng-generics-draft.md` §7 | 规范更新 | ✓ 已决策，参见 Q1 节；阻塞 G6 已解除 |
| G0-2 | 决策 Q2 约束目标是否可以是泛型 spec 实例，更新规范 §4 和 §5 | 规范更新 | ✓ 已决策，参见 Q2 节 |
| G0-3 | 决策 Q3 泛型 type 默认零值规则，更新 `docs/feng-type.md` | 规范更新 | ✓ 已决策，参见 Q3 节（原阻塞 G4-19）|
| G0-4 | 决策 Q4 `>>` token 处理策略 | 规范更新 | ✓ 已决策，参见 Q4 节；G1-2 可删除 |

---

### G1 词法分析

词法器本身不需要新增关键字。主要工作取决于 Q4 的决策。

| 编号 | 任务 | 涉及文件 | 依赖 |
| --- | --- | --- | --- |
| G1-1 | 确认 `:<` 序列不被词法器合并为新 token，保持 `COLON` + `LT` 独立产出 | `src/lexer/lexer.c` | — |
| G1-2 | ~~（Q4 已决策为策略 1，本任务取消）~~ 词法器零改动，无需新增接口 | — | — |
| G1-3 | 补词法层单元测试：`:<`、`Map<int>`、`Map<List<int>>`、`a >> b`（位移）不被误拆 | `test/lexer/` | G1-1、G1-2 |

---

### G2 AST 扩展

Parser 的 AST 数据结构当前完全没有泛型信息，需要系统性扩展。

#### G2-1 新增类型参数定义节点

在 `src/parser/parser.h` 中新增：

```c
/* 单个类型参数定义，如 <T> 或 <T: Named> */
typedef struct FengTypeParam {
    FengToken token;
    FengSlice name;          /* 参数名 */
    FengTypeRef *constraint; /* 约束目标（NULL = 无约束） */
} FengTypeParam;
```

| 编号 | 任务 | 涉及文件 |
| --- | --- | --- |
| G2-1 | 新增 `FengTypeParam` 结构 | `src/parser/parser.h` |

#### G2-2 扩展声明节点

| 编号 | 任务 | 修改内容 |
| --- | --- | --- |
| G2-2a | `type_decl` 新增 `FengTypeParam *type_params; size_t type_param_count;` | `parser.h` |
| G2-2b | `spec_decl` 新增 `FengTypeParam *type_params; size_t type_param_count;` | `parser.h` |
| G2-2c | `FengCallableSignature` 新增 `FengTypeParam *type_params; size_t type_param_count;`（用于顶层泛型 fn 和泛型方法） | `parser.h` |
| G2-2d | `fit_decl` 已有 `target`（FengTypeRef），但需确认泛型 fit 左侧 `<T>` 的表达方式——由于规范明确 fit 左侧 `<T>` 不是新参数定义而是引用，直接复用 `FengTypeRef.named` 中的 type_args 即可，不需要新增 type_params | `parser.h`（审查，可能无需改动） |

#### G2-3 扩展类型引用节点

```c
/* FengTypeRef.named 扩展：增加类型实参列表 */
struct {
    FengSlice *segments;
    size_t segment_count;
    FengTypeRef **type_args;   /* 新增 */
    size_t type_arg_count;     /* 新增 */
} named;
```

| 编号 | 任务 |
| --- | --- |
| G2-3 | 扩展 `FengTypeRef.named` 新增 `type_args` / `type_arg_count` |

#### G2-4 扩展调用表达式节点

```c
struct {
    FengExpr *callee;
    FengExpr **args;
    size_t arg_count;
    FengResolvedCallable resolved_callable;
    /* 新增：显式泛型调用 callee:<T1, T2>(...) 的类型实参 */
    FengTypeRef **explicit_type_args;
    size_t explicit_type_arg_count;
    bool has_explicit_type_args;
} call;
```

| 编号 | 任务 |
| --- | --- |
| G2-4 | 扩展 `FengExpr.call` 新增 `explicit_type_args` / `explicit_type_arg_count` / `has_explicit_type_args` |

---

### G3 语法分析（Parser）扩展

基于扩展后的 AST，扩展 Parser 支持泛型语法。

| 编号 | 任务 | 说明 |
| --- | --- | --- |
| G3-1 | 扩展 `type` 声明解析：检测 `Name<...>` 中的类型参数列表并填入 `type_params` | `src/parser/parser.c` |
| G3-2 | 扩展 `spec` 声明解析（object-form、callable-form）：检测类型参数列表 | `src/parser/parser.c` |
| G3-3 | 扩展类型引用解析 `parse_type_ref`：检测 `Name<T1, T2>` 并填入 `type_args`；同时解决 `>>` 歧义（依赖 G0-4 决策） | `src/parser/parser.c` |
| G3-4 | 扩展成员方法和顶层 fn 的声明解析：检测 `fn Name<T>` 中的类型参数 | `src/parser/parser.c` |
| G3-5 | 扩展调用表达式解析：检测 `callee:<T1, T2>(...)` 的显式泛型调用语法 | `src/parser/parser.c` |
| G3-6 | 确保 `parse_type_ref` 中 `<...>` 仅作为类型实参，不在非调用位置误解析 `:<...>` | `src/parser/parser.c` |
| G3-7 | 扩展 `spec` 父列表解析（`spec Child: Parent<T>`）：父列表使用扩展后的类型引用解析即可，无需单独处理 | `src/parser/parser.c` |
| G3-8 | 扩展 dump/print：新增 `type_params`、`type_args`、`explicit_type_args` 的输出 | `src/parser/dump.c` |
| G3-9 | Parser 单元测试：泛型声明、类型实例化引用、显式泛型调用、嵌套泛型、错误语法拒绝 | `test/parser/` |

**Parser 关键边界**（不能越界）：
- Parser 不得依赖语义判断某个 `<...>` 是否是泛型调用；只有 `:<...>` 才标记为显式泛型调用
- `foo<T>(...)`、`pkg.foo<T>(...)` 不得被 Parser 解析为显式泛型调用

---

### G4 语义分析扩展

这是实现量最大、最复杂的一环。

| 编号 | 任务 | 说明 | 依赖 |
| --- | --- | --- | --- |
| G4-1 | **类型参数作用域**：为每个泛型声明建立类型参数作用域，把类型参数名注册为可解析名字 | `src/semantic/analyzer.c` | — |
| G4-2 | **类型参数引用解析**：在类型位置解析时，区分"具名类型引用"和"类型参数引用" | `src/semantic/analyzer.c` | G4-1 |
| G4-3 | **约束目标解析与记录**：解析每个类型参数的约束目标（无约束 / type 约束 / spec 约束） | `src/semantic/analyzer.c` | G4-1 |
| G4-4 | **约束目标是泛型 spec 实例时的处理**（G0-2 已决策：允许，约束目标按 `NAMED_GENERIC` 节点解析） | `src/semantic/analyzer.c` | — |
| G4-5 | **泛型声明 identity 注册**：按"名称 + 泛型参数数量"注册具名泛型 type/spec；冲突检查 | `src/semantic/analyzer.c` | — |
| G4-6 | **泛型实例类型匹配**：按不变规则（invariance）检查泛型实例兼容性；类型参数不同的实例不兼容 | `src/semantic/analyzer.c` | — |
| G4-7 | **泛型具名 type/spec 的使用解析**：按"名称 + 泛型参数数量"精确解析；实参数量不匹配时报错 | `src/semantic/analyzer.c` | G4-5 |
| G4-8 | **约束体内成员访问**：若约束目标是 object-form spec，按 spec 成员集提供访问；callable-form 按签名调用；union-form 复用收窄规则 | `src/semantic/spec_member_accesses.c` | G4-3 |
| G4-9 | **无约束类型参数的成员访问禁止**：无约束类型参数不得访问成员、做关系/逻辑运算 | `src/semantic/analyzer.c` | G4-1 |
| G4-10 | **泛型重载扩展**：在现有重载规则基础上，把"泛型参数数量"并入重载签名；约束目标不参与 | `src/semantic/analyzer.c` | G4-5 |
| G4-11 | **非泛型优先**：当精确具体类型候选和泛型候选同时可匹配时，优先非泛型 | `src/semantic/analyzer.c` | G4-10 |
| G4-12 | **泛型推导**：省略显式类型实参时，从实参类型、接收者静态类型、上下文目标类型推导类型参数；不唯一时报错 | `src/semantic/analyzer.c` | G4-7 |
| G4-13 | **显式泛型调用验证**：`:<...>` 只允许在泛型可调用目标上；对非泛型函数写 `:<...>` 报错；实参数量必须与类型参数个数一致 | `src/semantic/analyzer.c` | G4-7 |
| G4-14 | **方法泛型参数重名检查**：泛型 type 内的方法泛型参数名不得与外层类型泛型参数名重名 | `src/semantic/analyzer.c` | G4-1 |
| G4-15 | **泛型父 spec 约束传递检查**：`spec Child<T>: Parent<T>` 中 T 传递到 Parent 时，检查 T 是否满足 Parent 对应位置的约束 | `src/semantic/spec_relations.c` | G4-3 |
| G4-16 | **泛型 fit 左侧解析**：`fit Box<T>: Reader<T>` 中 `<T>` 是对 Box<T> 已声明参数的引用，按"名称 + 泛型参数数量"匹配目标 type | `src/semantic/analyzer.c` | G4-5 |
| G4-17 | **泛型 fit 满足性检查**：检查 `fit Box<T>: Reader<T>` 中方法签名是否满足 Reader<T>；此处 T 统一指向 Box<T> 的类型参数 | `src/semantic/spec_relations.c` | G4-16 |
| G4-18 | **终结器泛型参数拒绝**：泛型 type 内的终结器不允许携带类型参数 | `src/semantic/analyzer.c` | — |
| G4-19 | **默认零值泛型扩展**（G0-3 已决策：按字段递推，类型参数替换后与非泛型规则一致） | `src/semantic/analyzer.c` | — |
| G4-20 | **语义分析单元测试**：所有正确语法通过；所有错误语法（错误语法 1-12）报错 | `test/semantic/` | — |

---

### G5 符号表导出（.ft 泛型支持）

基于 Phase 5.5 完成的 TSEQ 架构，扩展 ft_write.c 支持泛型声明导出。

| 编号 | 任务 | 说明 |
| --- | --- | --- |
| G5-1 | **type_param 符号导出**：为泛型 type/spec 的每个类型参数导出 `FT_SYM_KIND_TYPE_PARAM` 符号（owner = 泛型声明符号，顺序按声明顺序） | `src/symbol/ft_write.c` |
| G5-2 | **TYPE_PARAM_REF 类型节点**：在 TYPS 中生成 `FT_TYPE_KIND_TYPE_PARAM_REF` 节点（string_ref = 参数名，sym_ref = type_param 符号 ID） | `src/symbol/ft_write.c` |
| G5-3 | **NAMED_GENERIC 类型节点**：为泛型具名使用（如 `Box<T>`、`List<int>`）生成 `FT_TYPE_KIND_NAMED_GENERIC` 节点 + TSEQ 类型实参 | `src/symbol/ft_write.c` |
| G5-4 | **CALLABLE 类型节点**：泛型函数/方法的 type_ref 指向 CALLABLE 节点，其 TSEQ 中参数类型可含 TYPE_PARAM_REF | `src/symbol/ft_write.c` |
| G5-5 | **泛型 spec 的 TYPS 编码**：按 SPEC_OBJECT / SPEC_CALLABLE TYPS.kind 区分 form，sym_ref = spec 符号 ID | `src/symbol/ft_write.c` |
| G5-6 | **泛型 fit 的 extra_ref 与 attr**：fit 符号的 extra_ref 指向 NAMED_GENERIC 类型节点；FT_ATTR_FIT_SPECS 范围存 Reader<T> 等结构化使用 | `src/symbol/ft_write.c` |
| G5-7 | **泛型声明的跨模块读取**：在 `src/symbol/imported_module.c` 中扩展 .ft 读取，重建泛型声明的类型参数和 NAMED_GENERIC 使用节点 | `src/symbol/imported_module.c` |
| G5-8 | **符号表单元测试**：泛型 type/spec/fn/fit 的导出内容验证；跨模块读取后符号查询正确 | `test/symbol/` |

---

### G6 代码生成

**依赖 G0-1 决策**。以下任务内容会根据决策方向调整，当前为占位说明。

| 编号 | 任务 | 说明 |
| --- | --- | --- |
| G6-1 | 确定泛型调用点的 C 发码形态（依赖 Q1 决策） | `src/codegen/codegen.c` |
| G6-2 | 泛型函数/方法的 C 函数声明形态 | `src/codegen/codegen.c` |
| G6-3 | 泛型实例化时 retain/release 的 ARC 处理（泛型参数类型的生命周期管理） | `src/codegen/codegen.c` |
| G6-4 | 代码生成单元测试 | `test/codegen/` |

---

### G7 测试与验证

| 编号 | 任务 |
| --- | --- |
| G7-1 | Parser 测试：全量覆盖 `docs/feng-generics-draft.md` 中正确语法 1-9 和错误语法 1-12 |
| G7-2 | 语义分析测试：泛型声明、重载、推导、约束访问、invariance 各场景 |
| G7-3 | 符号表测试：泛型声明导出 / 跨包读取后语义等价验证 |
| G7-4 | 代码生成 smoke 测试：`hello world` 升级为泛型函数调用；Box<T> + fit + spec 端到端 |
| G7-5 | 全量回归测试：确保既有非泛型功能无回归 |

---

## 三、任务依赖关系

```text
G0-1(代码生成策略) ─────────────────────────────────────────────── G6
G0-2(约束目标是否可为泛型实例) ─────────────────────────────────── G4-4
G0-3(默认零值规则) ──────────────────────────────────────────────── G4-19
G0-4(>>歧义处理) ────────────────────────────────────────────────── G1-2 → G3-3

Phase 5.5(符号表重构) → G5-1..G5-8

G1-1(:<确认) → G3-5(显式泛型调用解析)

G2-1(TypeParam结构) → G2-2 → G2-3 → G2-4 → G3 全部 → G4 全部

G4-1(类型参数作用域) → G4-2 → G4-3 → G4-8, G4-9, G4-15
G4-5(声明 identity) → G4-7 → G4-10, G4-11, G4-12, G4-13
G4-16(泛型 fit 左侧) → G4-17
```

---

## 四、补充说明

### 关于实现顺序建议

推荐按以下顺序推进，每步都要回归测试：

1. Phase 5.5（符号表重构，已在 feng-plan.md）
2. G0 全部决策（无编码，只收口规范）
3. G2 AST 扩展（不跑测试，只修改结构定义）
4. G1 + G3（词法和 Parser 扩展）+ G3-9（Parser 测试）
5. G4-1 ~ G4-14（语义分析主体）+ G4-20（语义测试）
6. G4-15 ~ G4-18（父 spec 约束、fit、终结器）
7. G5（符号表导出）+ G5-8（符号表测试）
8. G6（代码生成，依赖 Q1 决策）+ G7-4（smoke）
9. G7-5（全量回归）

### 关于当前代码库基础

当前 Parser AST（`src/parser/parser.h`）完全没有泛型信息：

- `FengTypeRef.named` 无 `type_args`
- `FengDecl.type_decl` / `spec_decl` 无 `type_params`
- `FengCallableSignature` 无 `type_params`
- `FengExpr.call` 无 `explicit_type_args`
- `FengSpecForm` 无 `UNION` form（union-form spec 也暂未实现）

因此 G2 AST 扩展是整个实现的起点，规模不大但牵动面广。
