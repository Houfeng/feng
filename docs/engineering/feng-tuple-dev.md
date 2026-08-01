# 元组开发任务

规范文档：[docs/specifications/feng-tuple.md](../specifications/feng-tuple.md)

---

## Parser

- [x] P1. 解析具名元组类型声明：`type Foo(T1, T2)` — 以 `(` 与对象类型 `{` 在 parse 层区分，生成元组 type 节点
- [x] P2. 解析元组类型在类型位置的出现（参数类型、返回类型、绑定类型注解）
- [x] P3. 解析元组字面量 `()` / `(expr, expr, ...)` 作为值表达式
- [x] P4. 解析解构绑定 `let (x, y) = ...` / `var (x, y) = ...`，支持空位（相邻逗号间无标识符）
- [x] P5. 解析显式类型转换 `(TupleType)expr`（与现有转换语法复用）
- [x] P6. 元素数量检查：具名元组类型仅允许 0 个或 2~8 个元素；1 个或超过 8 个元素，Parser 立即报错；`()` 解析为 0 元组字面量；`(expr)` 始终解析为普通括号表达式

## Semantic

- [x] S1. 具名元组类型注册到类型系统，标记为元组 kind，记录元素类型列表
- [x] S2. 字面量元组类型推断：在 let/var 绑定、函数入参、函数返回值、类型成员绑定、显式类型转换五个上下文中，由目标具名元组类型决定各位置类型；元素数量或类型不符报错
- [x] S3. 字面量元组在无具名类型上下文中（如 `let a = (1, 2)`）报编译错误
- [x] S4. 成员访问 `.item1` / `.item2` / ... 解析：根据具名元组元素列表生成对应字段访问
- [x] S5. 元素不可变：无论 `let`/`var`，禁止对单个元素原地赋值（`a.item1 = x` 报错）— parser 合成 member 时固定 `FENG_MUTABILITY_LET`，由现有字段不可变检查路径自动覆盖，需验证无遗漏
- [x] S6. 具名元组间显式转换：结构（元素数量 + 各位置类型）完全相同时允许，否则报错；禁止隐式转换
- [x] S7. 解构语义展开：平铺为各元素的独立绑定，继承 `let`/`var` 语义；空位跳过，不产生绑定；若右侧不是简单变量（如函数调用），codegen 先生成临时变量持有结果，再按位置展开，避免右侧被求值多次
- [x] S8. 解构右侧为字面量元组时的语义处理：按位置推断各位置类型，编译期展开
- [x] S9. 嵌套解构报编译错误（解构模式只允许标识符或空位，不允许 `(...)`）
- [x] S10. 泛型元组：**验证**现有泛型机制对元组无遗漏，无需新增逻辑。`type_params[]` 已有，实例化时现有替换逻辑遍历 `members[].as.field.type` 做参数替换，元组 member 与普通 struct field 使用同一个 `FengTypeMember.as.field` 结构，路径完全命中。S2 的逐位置字面量检查在实例化后读到的已是具体类型，无需感知泛型。
- [x] S11. 具名元组不能作为泛型上界约束：约束检查处加一个 `is_tuple` 判断报错，改动极小
- [x] S12. `fit` 支持：**验证**现有 fit 机制可覆盖，无需新增逻辑。`members[]` 已有，fit 路径直接命中

## Codegen

- [x] C1. 具名元组 ABI 布局：栈上连续结构，按元素类型顺序排列（与 struct 对齐规则一致）
- [x] C2. 元组字面量 codegen：按位置对各元素求值，写入栈上布局
- [x] C3. 成员访问 `.itemN` codegen：生成对应偏移量的字段读取
- [x] C4. 赋值与传参拷贝语义：整体值拷贝，不涉及引用
- [x] C5. 解构 codegen：展开为各元素的独立局部变量赋值；空位不生成赋值指令
- [x] C6. 字面量元组解构 codegen：各位置直接对表达式求值并绑定到局部变量
- [x] C7. 显式类型转换 codegen：结构相同时为内存级 reinterpret（无运行时开销）
- [x] C8. `var` 绑定整体替换 codegen：生成整体覆盖写，禁止单元素写路径

## 测试

- [x] T1. 基础：具名元组声明、字面量绑定、成员访问（`.item1` / `.item2`）
- [x] T2. 值语义：赋值拷贝、传参拷贝（修改副本不影响原值）
- [x] T3. 不可变：`let`/`var` 均禁止 `.itemN = x`，期望编译错误
- [x] T4. `var` 整体替换合法，`let` 整体替换报错
- [x] T5. 元素数量：0 元素、2 元素、8 元素均合法；1 元素、9 元素类型报错；`()` 作为 0 元组字面量合法；`(expr)` 作为括号表达式合法
- [x] T6. 字面量绑定类型匹配：元素数量或类型不符报错
- [x] T7. `let a = (1, 2)` 无具名类型上下文报错
- [x] T8. 具名元组间显式转换：结构相同允许，结构不同报错，隐式转换报错
- [x] T9. 解构：全绑定、含空位、首尾空位、全空位
- [x] T10. 字面量元组解构：`let (x, y) = (1, 2)`
- [x] T11. 嵌套解构报编译错误
- [x] T12. 泛型元组：`Pair<int, string>` 声明、绑定、成员访问、函数调用类型推断
- [x] T13. `fit`：具名元组实现 spec，方法内通过 `self.itemN` 访问元素；覆盖直接调用、spec 参数 coercion、spec 局部 coercion
- [x] T14. 具名元组作泛型上界报编译错误

---

## 关键文件

| 文件 | 层 | 作用 |
|------|----|------|
| `src/parser/parser.h` | Parser | AST 节点定义（`FengDecl`、`FengExpr`、`FengStmt`、`FengBinding`） |
| `src/parser/parser.c` | Parser | 词法驱动的递归下降解析器，`parse_type_declaration()` 是入口 |
| `src/semantic/analyzer.c` | Semantic | 主分析器，类型推断、绑定检查、coercion site 标注 |
| `src/semantic/semantic.h` | Semantic | 分析结果结构体（`FengSemanticAnalysis`、`FengSemanticTypeFact` 等） |
| `src/semantic/type_facts.c` | Semantic | 类型事实表：记录每个 AST 节点的静态类型，是 codegen 的主要输入 |
| `src/codegen/codegen.c` | Codegen | 将 AST + 语义信息翻译为 C 源码，`cg_emit_object_literal()` 是对象字面量的参照 |
| `src/codegen/codegen.h` | Codegen | `FengCodegenOutput` / `FengCodegenError` |
| `src/runtime/feng_runtime.h` | Runtime | `FengAggregateValueDescriptor`、`FengManagedSlotDescriptor`、`feng_aggregate_*` API |
| `src/runtime/feng_aggregate.c` | Runtime | 聚合值的 retain/release/assign/take/default_init 实现 |

---

## 关键数据结构

### Parser 层需新增 / 修改

**1. `FengDecl.type_decl` 扩展（`parser.h`）**

新增 `is_tuple` 一个字段，`members[]` / `member_count` 完全复用。

```c
struct {
    FengSlice name;
    FengTypeParam *type_params;
    size_t type_param_count;
    FengTypeMember **members;      /* 对象类型：用户声明的字段/方法
                                    * 元组类型：parser 按位置合成的 FIELD 成员
                                    *   name.data = "item1"/"item2"/...（C 字符串字面量，静态存储期）
                                    *   mutability = FENG_MUTABILITY_LET（总是不可变）
                                    *   kind = FENG_TYPE_MEMBER_FIELD */
    size_t member_count;           /* 同时也是元组元素数量 */
    FengTypeRef **declared_specs;
    size_t declared_spec_count;
    /* --- 新增（唯一新增字段）--- */
    bool is_tuple;                 /* true = 圆括号形式具名元组 */
} type_decl;
```

合成成员时直接用 C 字符串字面量赋给 `FengSlice.data`，无需额外数组：字面量 `"item1"` 等已具有静态存储期，可安全持有指针。

**好处**：成员访问（`.item1`）、`fit` 中 `self.itemN`、直接 fit 方法调用、codegen 字段遍历全部走已有路径，**低改动**。对象形态 spec coercion 需要 codegen 为具名元组创建运行时管理的 tuple box，避免把栈上 by-value 元组直接作为可逃逸 subject。S5（禁止元素原地赋值）也由现有的 `FENG_MUTABILITY_LET` 字段不可变检查自动覆盖，**无需新增 semantic 逻辑**。

**2. 新增 `FENG_EXPR_TUPLE_LITERAL`（`parser.h`）**

```c
/* 在 FengExprKind 枚举中新增 */
FENG_EXPR_TUPLE_LITERAL,

/* 在 FengExpr.as 联合体中对应的字段 */
struct {
    FengExpr **items;   /* 各位置的元素表达式；空位处为 NULL */
    size_t count;
} tuple_literal;
```

**3. 解构绑定扩展（`parser.h`）**

```c
/* FengBinding 保持不变，新增 is_destructure 和 positions */
typedef struct FengBinding {
    FengToken token;
    FengMutability mutability;
    FengSlice name;            /* is_destructure == false 时有效 */
    FengTypeRef *type;
    FengExpr *initializer;
    /* --- 新增 --- */
    bool is_destructure;
    FengSlice *destructure_names;   /* 各位置标识符；空位为空 slice（.data==NULL）*/
    size_t destructure_count;
} FengBinding;
```

### Semantic 层

**`FengSemanticTypeFact`（`semantic.h`）**

现有的 `FENG_SEMANTIC_TYPE_FACT_DECL` kind 已可标记具名元组，无需新增 kind。语义分析器在处理 `FengDecl.type_decl.is_tuple == true` 时走独立路径，向类型事实表注册该 decl；后续 coercion 检查读取 `members[]` 做位置匹配（第 i 个 member 即 item(i+1) 的类型）。

**S5（元素不可变）自动覆盖**：parser 合成的 member 均为 `FENG_MUTABILITY_LET`，现有的字段不可变赋值检查路径直接报错，无需新增逻辑。

**各任务改动量一览**

| 分类 | 任务 | 改动 |
|------|------|------|
| 必须新增逻辑 | S2/S3 字面量推断 | `FENG_EXPR_TUPLE_LITERAL` 是新 expr kind，需在 `validate_expr_against_expected_type()` 新增处理分支，逐位置检查元素类型 |
| 必须新增逻辑 | S6 显式转换检查 | 现有转换检查不认识元组，需比较两个 `is_tuple` 类型的 `member_count` + 各位置类型 |
| 必须新增逻辑 | S7/S8 解构展开 | `is_destructure` 是新字段，需新增展开逻辑将其平铺为独立 binding 序列 |
| 改动极小 | S1 类型注册 | 现有 `FENG_DECL_TYPE` 注册路径复用，只需把 `is_tuple` 透传到类型事实供 S2/S6 查询 |
| 改动极小 | S9 嵌套解构报错 | 解构 pattern 解析时加 `is_tuple` 判断即可 |
| 改动极小 | S11 上界约束报错 | 约束检查处加一个 `is_tuple` 判断 |
| 自动继承，仅验证 | S4 成员访问 | `members[]` 已有对应 field，现有成员查找路径直接命中 |
| 自动继承，仅验证 | S5 元素不可变 | `FENG_MUTABILITY_LET` 检查已有 |
| 自动继承，仅验证 | S10 泛型 | `type_params[]` 已有，实例化替换遍历 `members[].as.field.type`，路径完全命中 |
| 自动继承，仅验证 | S12 fit | `members[]` 已有，fit 路径直接工作 |

**新增 Coercion 路径**

在 `spec_coercion_sites.c`/`analyzer.c` 中，五个字面量绑定上下文各对应一个 coercion site 检查点：

```
FENG_SPEC_COERCION_FORM_OBJECT  →  保持（用于 spec 贴合）
新增逻辑（不新增 form）：
  tuple_literal 在 validate_expr_against_expected_type() 中
  检测目标类型 is_tuple == true，逐位置递归检查元素类型
```

### Runtime 层（无需新增 runtime API）

具名元组复用现有的 **`FengAggregateValueDescriptor`** 路径：

```c
/* codegen 为每个具名元组实例化 emitted 的静态描述符（参照对象类型的 emit 路径）*/
static const FengManagedSlotDescriptor MyTuple_slots[] = { ... };
static const FengAggregateDefaultInitDescriptor MyTuple_default_init = {
    .kind = FENG_DEFAULT_ZERO_BYTES   /* 或 FENG_DEFAULT_INIT_FN 若含 string 元素 */
};
static const FengAggregateValueDescriptor MyTuple_aggregate = {
    .name              = "MyTuple",
    .size              = sizeof(MyTuple_t),
    .default_init      = &MyTuple_default_init,
    .managed_slot_count = ...,
    .managed_slots     = MyTuple_slots
};
```

运行时操作完全通过 `feng_aggregate_retain` / `feng_aggregate_release` /
`feng_aggregate_assign` / `feng_aggregate_take` 完成，与对象类型一致，**无需新增任何 runtime 函数或 runtime ABI**。

对象形态 spec coercion 额外生成一个内部 tuple box：

```c
struct MyTuple__spec_box {
  FengManagedHeader _hdr;
  MyTuple_t value;
};
```

tuple box 是普通运行时管理对象，`release_children` 对 `value` 调用 `feng_aggregate_release()`；witness thunk 从 box 的 `value` 字段恢复 by-value `self`，因此元组值作为 spec 参数、spec 局部或返回值逃逸时不会悬垂。

---

## 实现原理与运行时结构

### 整体流程

```
  Feng 源码
  │
  ▼ parser.c — parse_type_declaration() 识别 ( 开头 → is_tuple=true
  │            parse_binding() 识别 let/var (x,,z) = ... → is_destructure=true
  │            parse_expr_primary() 识别 () / (e1,e2,...) → FENG_EXPR_TUPLE_LITERAL
  ▼
FengProgram (AST)
  │
  ▼ analyzer.c — resolve_decl_type() 注册具名元组，记录元素类型列表
  │             validate_expr_against_expected_type() 对 tuple_literal
  │               在五个上下文中逐位置推断 + 检查
  │             resolve_binding_destructure() 展开解构为独立绑定序列
  ▼
FengSemanticAnalysis
  │
  ▼ codegen.c — emit_type_decl() 对 is_tuple 走 tuple 路径：
  │               生成 C struct，各 itemN 字段按位置排列
  │               生成 FengAggregateValueDescriptor（含 managed slot 表）
  │             emit_tuple_literal() 按位置对各 item 求值，写入 C 复合字面量
  │             emit_destructure_binding() 展开为多个独立局部变量赋值
  ▼
C 源码 → 系统编译器 → 可执行文件
```

### 运行时内存布局

具名元组是**纯栈值类型**，codegen 在 C 层生成一个普通 `struct`：

```c
/* type Point(float, float)  →  codegen 生成 */
typedef struct { float item1; float item2; } Point_t;

/* 若元素中含有管理指针（string / 用户 type），codegen 还生成：*/
static const FengManagedSlotDescriptor Point_slots[] = {/* 无托管槽，float 是 trivial */};
static const FengAggregateValueDescriptor Point_agg = {
    .name = "Point", .size = sizeof(Point_t),
    .default_init = &(FengAggregateDefaultInitDescriptor){ .kind = FENG_DEFAULT_ZERO_BYTES },
    .managed_slot_count = 0, .managed_slots = Point_slots
};
```

- **无托管元素**（全为 int/float/bool 等 trivial 类型）：`FengAggregateValueDescriptor.managed_slot_count == 0`，赋值退化为 `memcpy`，无 retain/release 开销。
- **含托管元素**（string / 用户 type）：codegen 填充 `managed_slots`，赋值走 `feng_aggregate_assign()`，各托管槽自动 retain/release，与对象类型字段完全一致。
- **嵌套元组**（元素类型本身是元组）：托管槽使用 `FENG_SLOT_NESTED_AGGREGATE`，runtime walker 递归处理。

### 解构的编译期展开

解构不产生任何运行时成本，semantic 阶段将 `is_destructure` 的 `FengBinding` 展开为 N 个普通 `FengBinding`，codegen 处理的 AST 中已无解构节点：

```c
/* Feng 源码：右侧是简单变量，直接展开 */
let (x, , z) = t;

/* codegen 生成 */
T1 x = t.item1;
/* item2 空位：不生成任何赋值 */
T3 z = t.item3;

/* Feng 源码：右侧是函数调用，先生成临时变量，避免 foo() 被求值多次 */
let (x, , z) = foo();

/* codegen 生成 */
FooTuple_t _tmp = foo();
T1 x = _tmp.item1;
T3 z = _tmp.item3;
```

### 显式类型转换的 Codegen

`(TargetTuple)src_expr` 在结构相同时是**内存级 reinterpret**，codegen 直接生成 C 的复合字面量或 `memcpy`，零运行时开销：

```c
/* let c: MyTuple = (MyTuple)b;  →  codegen 生成 */
MyTuple_t c = *(MyTuple_t*)&b;   /* 或直接 memcpy，取决于对齐 */
```
