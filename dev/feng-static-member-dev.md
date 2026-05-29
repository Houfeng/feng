# 静态成员（static member）实现方案

## 功能概述

为 Feng 语言新增 `static` 关键字支持：`type` 体内允许声明静态绑定（`static let`/`static var`）和静态方法（`static func`）；`fit` 体内仅允许声明静态方法（`static func`），不允许扩展静态绑定或实例字段。

**核心规则：**

- 静态成员只能通过类型名访问（`T.xxx`），不能通过实例访问
- `fit` 可为用户自定义类型和内建类型（`string`、`int` 等）扩展静态方法，但不得扩展 `let`/`var` 绑定
- 静态方法支持泛型，规则与实例方法相同
- `fit` 的静态方法在 `use` 引入对应模块后，在调用侧可见面可用，规则与 fit 实例方法一致
- 静态成员（`open` 修饰）可跨 fb 包使用，符号表中记录完整信息
- `spec` 中暂不支持声明静态成员
- 静态方法运行时开销 = 普通顶层函数（编译为直接 C 函数调用，无任何间接层）
- 静态绑定运行时开销 = 普通顶层绑定（采用相同的延迟初始化 pattern，见 `feng-top-binding-optimize.md`）
- `open`/`seal` 可见性修饰符适用于静态成员，与实例成员一致
- 静态方法与实例方法形成独立的重载集，同名不冲突

---

## 性能约束（强要求）

以下两条是硬性约束，实现时必须满足，不可妥协：

**约束 1：静态方法调用开销不能大于普通顶层函数**

静态方法必须编译为直接 C 函数调用，不得引入任何间接层（无函数指针、无 vtable、无 dispatch 表）。调用侧生成的 C 代码形式必须与调用同等签名的模块顶层函数完全相同。

**约束 2：静态绑定访问开销不能大于普通顶层绑定**

静态绑定必须采用与顶层绑定完全相同的延迟初始化 pattern（`ensure_init` + 静态存储槽），不得引入额外间接层或额外的运行时查找。访问侧生成的 C 代码形式必须与访问同等类型的模块顶层绑定完全一致。

---

## 语法

### type 体内的静态成员

```feng
type Counter {
    // 静态绑定
    open static let zero: int = 0
    open static var count: int = 0

    // 静态方法
    open static func create(): Counter { ... }
    open static func reset() { Counter.count = 0 }

    // 实例成员（不变）
    let value: int
    open func increment() { ... }
}
```

### fit 为用户自定义类型扩展静态方法

```feng
fit Counter {
    open static func fromSeed(seed: int): Counter { ... }
}
```

### 静态方法的泛型

静态方法支持泛型，规则与实例方法相同：

```feng
type Pair[A, B] {
    open static func of(a: A, b: B): Pair[A, B] { Pair { first: a, second: b } }
    let first: A
    let second: B
}

fit string {
    open static func join[T](items: [T], sep: string): string { ... }
}
```

调用方式：

```feng
let p = Pair.of(1, "hello")           // 类型从参数推断
let s = string.join(["a", "b"], ",")  // 同上
```

### fit 为内建类型扩展静态方法

```feng
fit string {
    open static func fromUtf8bytes(bytes: [byte]): string { ... }
}

fit int {
    open static func max(): int { 2147483647 }
}
```

### 跨包使用

`open` 静态成员可在引入包后通过类型名访问，`seal` 静态成员仅包内可用：

```feng
// 包 mylib 中
type Counter {
    open static var count: int = 0
    seal static var _internal: int = 0  // 包外不可访问
}

// 另一个包中
use mylib

Counter.count = Counter.count + 1  // 合法（open）
Counter._internal = 0              // 语义报错（seal）
```

### fit 静态方法的可见性

`fit` 的静态方法与 fit 实例方法遵循相同的可见性规则：引入（`use`）定义 fit 的模块后，其 `open` 静态方法在调用侧可见：

```feng
// 模块 mymod 中
fit string {
    open static func fromUtf8bytes(bytes: [byte]): string { ... }
}

// 另一个模块中
use mymod

let s = string.fromUtf8bytes(buf)  // 合法，mymod 已引入
```

### 访问语法

```feng
// 合法：通过类型名访问
let c = Counter.create()
Counter.count = Counter.count + 1
let s = string.fromUtf8bytes(buf)

// 非法：通过实例访问静态成员（语义报错）
let c = Counter { value: 0 }
c.create()    // 错误：静态方法不能通过实例访问
```

### 关键字顺序

如果有可见性修饰符（`open`/`seal`），`static` 必须紧跟在其**之后**。在 `type` 中，`static` 始终在 `let`/`var`/`func` **之前**；在 `fit` 中，`static` 只能出现在 `func` **之前**：

```
[open | seal] static (let | var | func) ...
[open | seal] static func ...  // fit 体内
```

`static` 不可出现在 `open`/`seal` 之前，以下写法均为 parse error：

```feng
type T {
    static open let x: int = 0      // 错误：static 不能在 open 之前
    static seal var count: int = 0  // 错误：static 不能在 seal 之前
}
```

### 禁止项

```feng
type T {
    static func T() { ... }         // 禁止：构造器不能加 static
    static func ~T() { ... }        // 禁止：析构器不能加 static
    static open let x: int = 0      // 禁止：static 不能在 open/seal 之前
}
```

---

## 设计决策

### 1. `is_static` bool 标志（而非新增 FengTypeMemberKind 枚举值）

`FengTypeMemberKind` 现有四个值：`FIELD`、`METHOD`、`CONSTRUCTOR`、`FINALIZER`。

静态性与成员类型正交，采用 `bool is_static` 标志附加在 `FengTypeMember` 上，而非引入 `STATIC_FIELD`、`STATIC_METHOD` 等枚举值。好处：

- 最小侵入：现有处理 FIELD/METHOD 的代码无需全量改写，按需加 `if (member->is_static)` 分支
- 语义清晰：静态性和成员种类是两个独立维度

### 2. 新增两个 FengResolvedCallableKind 值

`FengResolvedCallableKind` 新增：

- `FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD`：`type` 体内的静态方法
- `FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD`：`fit` 体内的静态方法

codegen 凭借这两个值直接生成无 `self` 参数的直接函数调用，保证零额外开销。

### 3. 内建类型静态 fit 的实现路径

`validate_function_call_expr` 中，当 callee 为 `FENG_EXPR_MEMBER` 且 object 为标识符时，目前只通过 `resolve_type_target_expr` 查找用户自定义类型。需扩展：

- `ResolvedTypeTarget` 添加 `bool is_builtin_type_name; FengSlice builtin_name;`
- `resolve_type_target_expr` 在 `FENG_EXPR_IDENTIFIER` 分支中，若名称匹配内建类型名，设 `is_builtin_type_name = true`
- 调用侧随后在 `BuiltinFit` 列表中查找静态方法

内建类型名列表（确定匹配范围）：`int`、`int8`、`int16`、`int32`、`int64`、`uint`、`uint8`、`uint16`、`uint32`、`uint64`、`float`、`float32`、`float64`、`bool`、`byte`、`string`

### 4. 静态绑定的 C 名称方案

完全复用顶层绑定的延迟初始化 pattern，前缀区分：

| 角色 | C 名称 |
|------|--------|
| 存储槽 | `Feng__<mod>__<TypeName>__static__<field>` |
| inited flag | `Feng__<mod>__<TypeName>__static__<field>__inited` |
| ensure_init 函数 | `Feng__<mod>__<TypeName>__static__<field>__ensure_init` |

### 5. 静态方法的 C 名称方案

| 角色 | C 名称 |
|------|--------|
| type 体内静态方法 | `Feng__<mod>__<TypeName>__static__<method>__<sig>` |
| fit 体内静态方法（用户类型） | `FengFit_<index>__Feng__<mod>__<TypeName>__<method>__<sig>` |
| fit 体内静态方法（内建类型） | `FengFitBuiltin__<mod>__<builtin_name>__<visibility><index>__<method>__<sig>` |

无 `self` 参数，等价于模块级普通函数。

---

## 生成代码示例

### Feng 源码

```feng
// module: mymod

type Counter {
    open static var count: int = 0
    open static func create(): Counter { Counter { value: 0 } }

    value: int
}

fit string {
    open static func repeat(s: string, n: int): string { ... }
}
```

### 生成的 C 代码

```c
// 静态绑定存储
static int64_t Feng__mymod__Counter__static__count = 0;
static bool    Feng__mymod__Counter__static__count__inited = false;

static void Feng__mymod__Counter__static__count__ensure_init(void) {
    if (Feng__mymod__Counter__static__count__inited) return;
    Feng__mymod__Counter__static__count = (int64_t)0;
    Feng__mymod__Counter__static__count__inited = true;
}

// 静态方法（无 self 参数，与顶层函数等价）
static struct Feng__mymod__Counter *Feng__mymod__Counter__static__create__(void) {
    // ...
}

// fit string 静态方法
static struct FengBuiltin__string FengFitBuiltin__mymod__string__i0__repeat__string_int(
    struct FengBuiltin__string s,
    int64_t n
) {
    // ...
}

// 访问 Counter.count（读）
Feng__mymod__Counter__static__count__ensure_init();
int64_t val = Feng__mymod__Counter__static__count;

// 访问 Counter.count（写，var 静态绑定）
Feng__mymod__Counter__static__count__ensure_init();
Feng__mymod__Counter__static__count = new_val;

// 调用 Counter.create()
struct Feng__mymod__Counter *c = Feng__mymod__Counter__static__create__();

// 调用 string.repeat(s, 3)
struct FengBuiltin__string r = FengFitBuiltin__mymod__string__i0__repeat__string_int(s, 3);
```

---

## 任务清单

### Phase 1 — 文档

- [x] 更新 `docs/feng-type.md`：static 成员语法、访问规则、可见性、泛型静态方法、跨包规则、禁止项
- [x] 更新 `docs/feng-fit.md`：fit static func 语法、内建类型示例、重载规则、use 可见性规则，并明确禁止 fit 中的 `static let`/`static var`

### Phase 2 — 词法器

- [x] `src/lexer/token.h`：将 `X(STATIC, "static")` 从 `FENG_RESERVED_WORD_LIST` 移入 `FENG_KEYWORD_LIST`
- [x] `test/lexer/test_lexer.c`：计数更新（keyword +1，reserved -1），删除旧保留词测试，新增关键词正向测试

### Phase 3 — 解析器 / AST

- [x] `src/parser/parser.h`：`FengTypeMember` 新增 `bool is_static`
- [x] `src/parser/parser.h`：`FengResolvedCallableKind` 新增 `TYPE_STATIC_METHOD`、`FIT_STATIC_METHOD`
- [x] `src/parser/parser.c`：`parse_type_declaration` 成员循环插入 `static` 匹配逻辑
- [x] `src/parser/parser.c`：`parse_fit_method_member` 插入 `static` 匹配逻辑，并拒绝 fit 中的 `static let`/`static var`
- [x] `src/parser/parser.c`：`parse_spec_member` 插入 `static` 拒绝检查
- [x] `src/parser/parser.c`：支持 `Box<int>.make(...)` 形式的显式泛型类型目标静态成员访问

### Phase 4 — 语义分析

- [x] `ResolvedTypeTarget` 扩展：新增 `is_builtin_type_name` + `builtin_name`
- [x] `resolve_type_target_expr`：内建类型名识别分支
- [x] 新增辅助函数 `find_type_static_member`
- [x] 新增辅助函数 `find_fit_static_method_for_type`
- [x] 新增辅助函数 `find_builtin_fit_static_method`
- [x] `validate_instance_member_expr`：静态成员访问路径（含实例访问静态成员的报错）
- [x] `infer_member_expr_type`：type 静态字段类型推断
- [x] `validate_function_call_expr`：静态方法调用路径（用户类型 + 内建类型）
- [x] `validate_type_member_overloads` / `validate_type_member_overload_overlap`：静态/实例独立重载集
- [x] 泛型静态方法解析：支持非泛型 owner、泛型 owner type、泛型 owner fit 的静态方法调用解析

### Phase 5 — 代码生成

- [x] `UserType` 结构体扩展：`static_methods`、`static_bindings` 字段
- [x] fit 方法注册保留 `is_static` 标志，仅支持静态方法，不产生 fit 静态绑定
- [x] `cg_pass_register_*`：type 成员按 `is_static` 分发到静态槽；fit 成员仅允许分发静态方法
- [x] `cg_emit_type_static_binding`：存储槽 + inited flag + ensure_init 函数生成
- [x] `cg_emit_type_static_method`：无 self 参数的直接 C 函数生成
- [x] `FENG_EXPR_CALL` 处理：`TYPE_STATIC_METHOD` / `FIT_STATIC_METHOD` 直接调用生成
- [x] `FENG_EXPR_MEMBER` 处理：静态绑定读取 / 写入（ensure_init + 槽访问）
- [x] 泛型静态方法生成：支持非泛型 owner、泛型 owner type、泛型 owner fit，并在 fit 方法体中应用目标泛型实参替换

### Phase 6 — 测试

- [x] 词法器测试：`static` 关键词正向 + 保留词删除
- [x] 解析器测试：成功场景（static let/var/func，fit static）
- [x] 解析器测试：泛型类型目标静态成员访问（`Box<int>.make(...)`）
- [x] 解析器测试：失败场景（静态构造器、静态析构器、顺序错误、spec 中 static）
- [x] 语义测试：成功场景（类型名调用、fit 内建类型、跨包 open static）
- [x] 语义测试：泛型静态方法解析（非泛型 owner、泛型 owner type、泛型 owner fit）
- [x] 语义测试：失败场景（实例访问静态成员、重载冲突）
- [x] 代码生成测试：存储槽/ensure_init 生成、无 self 直接调用、var 赋值、builtin fit 调用
- [x] 代码生成测试：泛型静态方法（非泛型 owner、泛型 owner type、泛型 owner fit）
- [x] 全量回归：所有现有测试通过

---

## 各层变更明细

### Phase 1 — 文档（先于所有代码变更）

- 更新 `docs/feng-type.md`：补充 `static let`/`static var` 和 `static func` 语法，访问规则，可见性规则，泛型静态方法规则，跨包访问规则，禁止项（构造器/析构器不能加 static；spec 中不支持 static）
- 更新 `docs/feng-fit.md`：补充 fit 体内 `static func` 语法，内建类型静态方法扩展示例，重载冲突规则（fit 静态方法只与同类型静态方法比较），fit 静态方法的 use 可见性规则，并明确禁止 fit 中的 `static let`/`static var`

### Phase 2 — 词法器

**`src/lexer/token.h`**

```c
// 变更：将 X(STATIC, "static") 从 FENG_RESERVED_WORD_LIST 移入 FENG_KEYWORD_LIST
// 效果：产生 FENG_TOKEN_KW_STATIC，原来使用 static 会产生 lex error，现在正常词法化
```

**`test/lexer/test_lexer.c`**

- `feng_keyword_count() == 27U` → `28U`
- `feng_reserved_word_count() == 11U` → `10U`
- 删除 `ASSERT(feng_is_reserved_word("static", 6U))`
- 从 `test_reserved_words_rejected` 数组中删除 `"static"` 条目
- 新增正向测试：`ASSERT(feng_lookup_keyword("static", 6U, &kw) && kw == FENG_TOKEN_KW_STATIC)`

### Phase 3 — 解析器 / AST

**`src/parser/parser.h`**

```c
// FengTypeMember 结构体，新增字段：
bool is_static;   // 静态成员标志，与 kind 正交

// FengResolvedCallableKind 枚举，新增两个值：
FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD,   // type 体内静态方法
FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD,    // fit 体内静态方法（含内建类型 fit）
```

**`src/parser/parser.c` — `parse_type_declaration` 成员循环**

语法顺序约束：`[open|seal]` → `static` → `let`/`var`/`func`。因此在 `parse_visibility(...)` **之后**（即 open/seal 已消费），`parse_keyword(let/var/func)` **之前**，插入：

```c
bool is_static = parser_match(parser, FENG_TOKEN_KW_STATIC);
```

`static` 不可出现在 `parse_visibility` 之前，若用户写 `static open ...`，则 `parse_visibility` 前尚未消费 `static`，后续 expect `let`/`var`/`func` 时会产生 parse error，行为正确，无需特殊处理。

- 若 `is_static == true`：
  - `let`/`var` → 正常解析字段，`member->is_static = true`
  - `func` → 检查方法名是否与类型同名（禁止静态构造器）或为析构器形式 `~TypeName`（禁止静态析构器），否则正常解析，`member->is_static = true`
  - 其他 token → 解析错误
- 更新错误消息从 `"expected type member declaration: 'let', 'var', or 'func'"` 为 `"expected type member declaration: 'let', 'var', 'func', or 'static'"` （注：static 须在 open/seal 之后）

**`src/parser/parser.c` — `parse_fit_method_member`**

同理，在 `parse_visibility(...)` **之后**插入：

```c
bool is_static = parser_match(parser, FENG_TOKEN_KW_STATIC);
```

- `let`/`var` 在 fit 中始终禁止，即使前面带 `static` 也要在 Parser 阶段报错
- `func` 正常解析，`member->is_static = true`

**`src/parser/parser.c` — `parse_spec_member`**（新增拒绝逻辑）

`spec` 中暂不支持 `static`。在 `parse_spec_member` 函数现有的 `open`/`seal` 检查之后，立即加入对 `FENG_TOKEN_KW_STATIC` 的检查：

```c
if (parser_check(parser, FENG_TOKEN_KW_STATIC)) {
    (void)parser_error_current(
        parser,
        "spec members cannot be declared 'static'");
    return NULL;
}
```

该检查放在消费 `let`/`var`/`func` 之前，使错误信息准确指向 `static` token 本身。测试中需覆盖该失败路径（见测试计划）。

### Phase 4 — 语义分析

**`src/semantic/analyzer.c`**

#### 4.1 ResolvedTypeTarget 扩展（line 10045 附近）

```c
typedef struct {
    FengTypeDecl *type_decl;
    FengModuleDecl *provider_module;
    // 新增：内建类型标识
    bool is_builtin_type_name;
    FengSlice builtin_name;
} ResolvedTypeTarget;
```

`resolve_type_target_expr` 在 `FENG_EXPR_IDENTIFIER` 分支中，若名称匹配内建类型名，设 `is_builtin_type_name = true`，`builtin_name` 为原始 slice。

#### 4.2 新增辅助函数

```c
// 在 type_decl 成员中查找 is_static=true 且名称匹配的成员（字段或方法）
static FengTypeMember *find_type_static_member(
    FengTypeDecl *type_decl,
    FengSlice name
);

// 在可见的 fit 声明中查找针对指定用户类型的静态方法
static FengTypeMember *find_fit_static_method_for_type(
    ResolveContext *ctx,
    FengTypeDecl *owner_type_decl,
    FengSlice name
);

// 在可见的 BuiltinFit 中查找针对指定内建类型的静态方法
static FengTypeMember *find_builtin_fit_static_method(
    ResolveContext *ctx,
    FengSlice builtin_name,
    FengSlice method_name
);
```

#### 4.3 validate_instance_member_expr（line ~9375）

enum item 检查之后，新增分支：

```
if type_target.type_decl != NULL（用户类型）:
    查找 is_static 静态成员 → 若找到，验证可见性，返回 valid
    若 object 是实例（非类型名）且找到的是静态成员 → 报错：通过实例访问静态成员

if type_target.is_builtin_type_name（内建类型）:
    查找 builtin fit 静态方法 → 同上处理
```

#### 4.4 infer_member_expr_type（line ~10156）

enum item 推断之后，新增分支：

```
if type_target.type_decl != NULL:
    查找静态字段 → 返回字段声明类型

fit 不扩展静态字段，因此内建类型目标不新增 builtin fit 静态字段推断路径。
```

#### 4.5 validate_function_call_expr（line ~13820）

callee 为 `FENG_EXPR_MEMBER`，object 解析到类型（非模块 alias）时，新增分支：

```
if type_target.type_decl != NULL:
    查找静态方法 → 参数校验 → resolved_callable.kind = TYPE_STATIC_METHOD

if type_target.is_builtin_type_name:
    查找 builtin fit 静态方法 → 参数校验 → resolved_callable.kind = FIT_STATIC_METHOD
```

#### 4.6 重载检查

`validate_type_member_overloads`：静态方法与实例方法形成独立重载集，互不冲突；静态方法之间的重载规则与实例方法相同。

`validate_type_member_overload_overlap`：fit 静态方法只与同类型静态方法检查冲突，不与实例方法比较；fit 实例方法同理。

### Phase 5 — 代码生成

> **性能约束**（见"性能约束"节）：静态方法只能生成直接 C 函数调用，严禁任何间接层；静态绑定只能采用 `ensure_init` + 静态存储槽 pattern，严禁额外的运行时查找。实现时若发现需要引入间接层，必须停下来重新设计，不可绕过约束。

**`src/codegen/codegen.c`**

#### 5.1 UserType 结构体扩展

```c
typedef struct {
    // 现有字段...
    UserMethod *methods;
    size_t method_count;
    size_t method_capacity;
    // 新增
    UserMethod   *static_methods;
    size_t        static_method_count;
    size_t        static_method_capacity;
    ModuleBinding *static_bindings;
    size_t        static_binding_count;
    size_t        static_binding_capacity;
} UserType;
```

#### 5.2 BuiltinFit 结构体扩展

```c
typedef struct {
    // 现有字段...
    UserMethod *methods;
    size_t method_count;
    // 新增
    UserMethod *static_methods;
    size_t      static_method_count;
} BuiltinFit;
```

#### 5.3 注册阶段（cg_pass_register_*）

注册 type 成员时，依据 `is_static` 分发到 `static_methods` 或 `static_bindings`；注册 fit 成员时，`is_static` 只能分发到 `static_methods`，不得产生 fit 静态绑定。

#### 5.4 静态绑定 emit（模仿 cg_emit_module_binding_ensure_init）

```c
static void cg_emit_type_static_binding(CgContext *ctx, UserType *ut, ModuleBinding *sb) {
    // 1. 存储槽声明
    // static <C_TYPE> _feng_st__<mod>__<TypeName>__<field> = <zero>;
    // 2. inited flag
    // static bool _feng_st__<mod>__<TypeName>__<field>__inited = false;
    // 3. ensure_init 函数
    // static void _feng_ensure_st__<mod>__<TypeName>__<field>(void) { ... }
}
```

#### 5.5 静态方法 emit（模仿 cg_emit_user_method，无 self 参数）

```c
static void cg_emit_type_static_method(CgContext *ctx, UserType *ut, UserMethod *sm) {
    // 函数签名：_feng_stm__<mod>__<TypeName>__<method>__<sig>(params...)
    // 无 self 参数
    // 函数体与普通方法相同
}
```

#### 5.6 调用生成（FENG_EXPR_CALL 处理）

```c
case FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD:
    // 直接调用：_feng_stm__<mod>__<TypeName>__<method>__<sig>(args...)
    // 无 self，无任何间接层

case FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD:
    // 直接调用 builtin fit 静态方法的 C 函数
    // 同样无 self，无任何间接层
```

#### 5.7 静态绑定访问生成（FENG_EXPR_MEMBER，type 目标）

读取：

```c
_feng_ensure_st__<mod>__<TypeName>__<field>();
// 然后使用 _feng_st__<mod>__<TypeName>__<field>
```

写入（`var` 静态绑定）：

```c
_feng_ensure_st__<mod>__<TypeName>__<field>();
_feng_st__<mod>__<TypeName>__<field> = new_val;
```

---

## 测试计划

### 词法器测试（`test/lexer/test_lexer.c`）

- `static` 不再是保留词
- `static` 是合法关键字，`feng_keyword_count()` 返回 28
- `feng_reserved_word_count()` 返回 10

### 解析器测试（`test/parser/`）

**成功场景：**

```feng
// 静态 let 字段
type T { static let x: int = 0 }

// 静态 var 字段（带可见性）
type T { open static var count: int = 0 }

// 静态方法
type T { open static func create(): T { ... } }

// fit 用户类型静态方法
fit SomeType { open static func factory(): SomeType { ... } }

// fit 内建类型静态方法
fit string { open static func fromBytes(b: [byte]): string { ... } }
```

**失败场景（parse error）：**

```feng
type T { static func T() { ... } }    // 禁止：静态构造器
type T { static func ~T() { ... } }   // 禁止：静态析构器
type T { static open let x: int = 0 } // 禁止：static 在 open/seal 之前
type T { static seal var n: int = 0 } // 禁止：static 在 seal 之前

fit T { static let x: int = 0 }       // 禁止：fit 中不得声明 static let/static var

// 禁止：spec 中声明 static 成员
spec Foo {
    static func bar(): int
}
```

### 语义测试（`test/semantic/`）

**成功场景：**

- `T.staticMethod()` 调用静态方法 OK
- `T.staticField` 读取静态字段 OK
- `T.staticVar = value` 写入静态 var 字段 OK
- `string.fromBytes(buf)` 调用 fit 扩展的内建类型静态方法 OK
- 静态方法与实例方法同名不报重载冲突
- fit 静态方法与 type 实例方法同名不报冲突

**失败场景（semantic error）：**

- `instance.staticMethod()` 通过实例访问静态方法 → 报错
- `instance.staticField` 通过实例访问静态字段 → 报错
- type 内两个完全相同签名的 static func → 报重载冲突
- fit 静态方法与 type 同名同签名静态方法 → 报冲突

### 代码生成测试（`test/codegen/`）

- 静态绑定：C 输出包含正确的存储槽、inited flag、ensure_init 函数
- 静态方法：C 输出为无 self 参数的直接 C 函数
- 调用静态方法：生成直接函数调用，无任何间接层
- 静态 var 赋值：先 ensure_init 再写槽
- 内建类型 fit 静态方法：正确生成并被调用

### 全量回归

所有现有测试通过，无破坏性变更。

---

## 关键文件

| 文件 | 变更内容 |
|------|---------|
| `docs/feng-type.md` | 补充 static 成员规范（先于代码） |
| `docs/feng-fit.md` | 补充 fit 中 static func 规范（先于代码），明确禁止 static let/static var |
| `src/lexer/token.h` | static 从 reserved 移入 keyword list |
| `test/lexer/test_lexer.c` | 计数更新，删除 static 保留词测试，新增关键词测试 |
| `src/parser/parser.h` | FengTypeMember::is_static，FengResolvedCallableKind 新增两个值 |
| `src/parser/parser.c` | parse_type_declaration、parse_fit_method_member |
| `src/semantic/analyzer.c` | ResolvedTypeTarget 扩展，三个新辅助函数，validate_instance_member_expr、infer_member_expr_type、validate_function_call_expr 扩展，重载检查扩展 |
| `src/codegen/codegen.c` | UserType/BuiltinFit 扩展，type 静态绑定/方法注册、emit、调用生成，fit 静态方法注册与调用生成 |

---

## 未包含内容

- `spec` 中暂不支持声明静态成员（本期不实现，待后续决策）
