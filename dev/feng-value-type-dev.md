# Feng `@value` 内建注解开发草案

> 本文档记录 `@value` 内建注解的设计方案。
> **状态**：草案阶段，尚未实现，仅讨论。
> 本文档是 [feng-value-model-delivered.md](./feng-value-model-delivered.md) 的 Phase 3（值语义 struct）落地方案。
> 本文档不修改任何语言权威规范（`docs/`），待方案确认后迁入规范。

---

## 0 背景

### 现状

Feng 当前有两种具名类型声明形式：

- **`type`**（对象类型）：堆分配，`FengManagedHeader`，引用语义，赋值复制引用。
- **`tuple`**（元组类型）：栈/内联分配，无托管头，值语义，赋值复制值。

两者之间存在一个明显的空白：用户想要「具名花括号字段 + 值语义 + 可带方法 + 无元素数量限制」的类型时，只能选择堆分配的 `type`。

值模型文档（`feng-value-model-delivered.md` §9.3）已预留 Phase 3 位置：

> Phase 3：值语义 struct（未来特性）
> 同 Phase 2，仅当语言规范引入后启动；runtime 同样应零修改。

现有的 `FengAggregateDescriptor`、五类聚合 API（`feng_aggregate_retain/release/assign/take/default_init`）、三分类 walker、数组元素分类——全部已为新增 aggregate 类型做好 OCP 准备。

### 直接目标

引入 `@value` 内建注解，使 `type` 声明的值语义变体成为可能：

```feng
@value
type Point {
  var x: float;
  var y: float;
}
```

`@value type` 是值类型：无托管头，栈/内联分配，赋值与传参复制值。语法与用法与普通 `type` 基本一致（支持构造器、泛型、方法、spec 满足；**禁止终结器**，见 §2.3），其余差别在存储模型与传递语义（详见 §1.2）。

### 非目标

- 不修改 `FengManagedHeader`、`FengTypeTag`、单指针原语或五类聚合 API。
- 不修改 `FengScalarBox` 结构或其 API。
- 不为 `@value type` 引入新的 runtime 类型或 runtime 函数。
- 不改变 tuple 的现有语义或实现。

---

## 1 核心设计决策

### 1.1 与 tuple 的关系

`@value type` 的**运行时处理**与 tuple **一致**：作为其他 type 成员时的内联布局、生命周期处理（retain/release/assign/take/default_init）、相等性处理（`equal_fn`）、box 结构——全部参考 tuple 路径（借鉴 tuple 的生成模式与聚合 API，per-type 生成描述符/equal_fn/box，生命周期走同一套聚合 API），走聚合值模型（`FengAggregateDescriptor` + 五类聚合 API）。

`@value type` 的**语法与用法**与普通 `type` **一致**：构造器、泛型、方法、spec 声明头满足、fit 扩展、可见性修饰、witness 路径等全部复用普通 `type` 的处理路径（**例外：禁止终结器**，见 §2.3）。

两者的差别本质上是**成员访问方式**与**语法/用法的扩展点**：

| 维度 | tuple | `@value type` |
|------|-------|---------------|
| 语法形式 | `type Name(T1, T2)` 圆括号 | `type Name { ... }` 花括号 |
| 成员访问 | 位置（`item1`、`item2`） | 命名（`x`、`y`） |
| 元素数量 | 0 或 2~8 | 无限制 |
| 成员可变性 | 元素始终不可变 | 支持 `let`/`var` |
| 构造器 | 无（字面量贴合） | 支持（同普通 `type`） |
| 终结器 | 不支持 | 不支持（编译期报错，见 §2.3） |
| 声明头满足 spec | 不支持 | 支持（同普通 `type`） |
| `fit` 扩展 | 支持 | 支持（同普通 `type`） |
| 泛型 | 支持 | 支持（同普通 `type`） |
| 取地址（`&`） | 不支持 | 支持（需 `@abi`，见 §3.7） |

### 1.2 与普通 type 的关系

`@value type` 的用法与普通 `type` 基本一致：支持构造器、泛型、方法、静态成员、可见性修饰（**禁止终结器**，见 §2.3）。其余差别在底层存储与传递模型（见下表）：

| 维度 | `type`（普通对象） | `@value type`（值类型） |
|------|---|---|
| 存储 | 堆分配，`FengManagedHeader` | 栈/内联，无 header |
| 赋值 | 引用复制 | 值复制 |
| 等值比较 | 默认引用身份 | 默认值比较 |
| 值分类 | `managed-pointer` | `trivial` 或 `aggregate` |
| spec 装箱 | 不需要（自身即托管对象） | 需要（逃逸时装箱） |
| 自引用 | 允许 | 不允许（编译错误） |
| 终结器 | 支持 | 不支持（编译错误，见 §2.3） |

### 1.3 设计原则

1. **运行时处理与 tuple 一致**：作为其他 type 成员时的内联布局、生命周期处理、相等性处理、box 结构、witness 生成——全部复用 tuple 路径（详见 §9「复用与参考分类」的「复用元组」三项）。
2. **语法与用法与普通 type 一致**：构造器、泛型、方法、spec 满足、fit 扩展等全部复用普通 type 路径。方法签名与方法体中 `self` 的处理与普通 type **完全一致**（`self` 统一为 `struct X *self` 指针，`self.field` 的可变性遵循 `let`/`var` 规则），构造器同理。**语法用法层面两处差别**：禁止终结器（见 §2.3）、取地址操作（见 §3.7）。**codegen 发码层面三处差别**（构造器栈分配、ABI surface、成员访问/赋值 `.` vs `->`）见 §7.4.2。
3. **runtime 零修改**：符合 OCP——新增 aggregate 类型仅需新增描述符，runtime walker/API 不动。
4. **codegen 最小改动**：tuple codegen 函数全部基于 `field->c_name` + `offsetof`，与成员访问方式（位置/命名）无关，可通过重命名 + guard 扩展直接复用，不需要创建独立函数。仅构造器（栈分配 vs 字面量贴合）、ABI surface（无 box）、成员访问/赋值（`.` vs `->`）需独立处理。
5. **值类型循环引用编译期拒绝**：`@value type` 与 tuple 同属值类型，直接或间接循环引用必须编译期报错（见 §3.5）。

---

## 2 语法

### 2.1 基础声明

```feng
@value
type Point {
  var x: float;
  var y: float;
}
```

`@value` 仅可用于 `type` 声明，不可用于 `spec`、`fit`、函数或变量。

### 2.2 构造器

```feng
@value
type Counter {
  var count: int;

  func Counter() {
    self.count = 0;
  }

  func Counter(initial: int) {
    self.count = initial;
  }
}

let c1 = Counter {};
let c2 = Counter { count: 5 };
let c3 = Counter(10);
```

构造器在栈上初始化值，完成后按值语义返回。`self` 指向正在初始化的栈上值地址。

### 2.3 终结器（禁止）

`@value type` **禁止定义终结器**，编译期报错。

```feng
@value
type Buffer {
  var data: byte*;
  var size: int;

  func ~Buffer() {   // ❌ 编译错误：@value type 不允许定义终结器
    feng_free(self.data);
  }
}
```

**理由**：`@value type` 是值语义，赋值与传参复制值。若允许终结器，开发者会用于释放资源（如 C 指针指向的内存），但值复制会导致同一资源被多个副本持有，终结器在副本生命周期结束时各自调用，极易出现多次释放。参考 C# 与 Swift 的结构体均不允许定义终结器。需要释放资源的类型应使用普通 `type`（堆对象，引用语义，单次释放）。

**影响**：`@value type` 的运行时处理与 tuple 完全一致——无终结器调用路径，不通过 `FENG_NODE_DEFER` 注册，box descriptor 的 `finalizer` 字段统一为 `NULL`。详见 §3.8、§5.3、§7.2。

### 2.4 泛型

```feng
@value
type Pair<T, U> {
  let first: T;
  let second: U;
}

let p: Pair<int, string> = Pair { first: 1, second: "hello" };
```

**泛型 `@value type` 总是 aggregate**，即使实例化后全字段 trivial 也走 aggregate 路径。这与 tuple 的处理一致——简化泛型分派，避免在编译期根据泛型参数切换 trivial/aggregate 分派。

### 2.5 方法

```feng
@value
type Point {
  var x: float;
  var y: float;

  func length(): float {
    return sqrt(self.x * self.x + self.y * self.y);
  }
}
```

方法中的 `self` 是指向栈上/内联值的地址，而非堆对象引用。

---

## 3 语义

### 3.1 值分类

由 semantic 阶段根据字段类型自动判定，复用现有三分类：

| 条件 | 分类 | 描述符 |
|------|------|--------|
| 全字段 trivial（标量、bool） | `CG_VK_TRIVIAL` | `FengTrivialDescriptor`（与标量一致，连 trivial 值也有描述符） |
| 至少一个托管字段（string、对象引用、spec） | `CG_VK_AGGREGATE` | `FengAggregateDescriptor` |
| 泛型实例（无论字段类型） | `CG_VK_AGGREGATE` | `FengAggregateDescriptor` |

**值分类只看字段是否含托管成员**——与 tuple 的判定逻辑完全一致。`@value type` 禁止终结器（见 §2.3），故不存在「终结器 + 值分类」的组合问题。

### 3.2 赋值与传参

```feng
let a = Point { x: 1.0, y: 2.0 };
let b = a;     // 值复制：memcpy + aggregate retain（如有托管字段）
var c = a;
c.x = 3.0;    // c 是独立副本，a.x 仍为 1.0
```

赋值与传参的 emit 路径与 tuple 一致：

- trivial `@value`：直接 C 赋值 / `memcpy`
- aggregate `@value`：`feng_aggregate_assign`（或 `retain` + `memcpy`）

### 3.3 成员可变性

`let`/`var` 控制成员字段的可变性，语义与普通 `type` 一致：

```feng
@value
type User {
  let id: int;          // 不可变
  var name: string;     // 可变
}
```

与 tuple 的区别：tuple 的元素始终不可变（`let`/`var` 仅控制整体替换）。`@value type` 的 `var` 字段支持原地修改。

当外层绑定为 `let` 时，`var` 字段是否可修改，语义与普通 `type` 一致——`let` 阻止重新绑定，不阻止通过 `var` 字段原地修改值内部状态。

### 3.4 等值语义

`@value type` 的 `==`/`!=` 默认为**值比较**（逐字段比较），而非引用身份比较：

- trivial `@value`：逐字段 `==`（和元组一致，通过 codegen 生成的 `equal_fn`，不用 `memcmp`）
- aggregate `@value`：逐字段比较（trivial 字段走 `==`，托管字段走各自的 `==`）

这与 tuple、`string` 的值语义比较一致，与普通 `type` 的引用身份比较不同。

### 3.5 自引用禁止（编译期报错）

`@value type` 不允许直接或间接自引用——与 tuple 同属值类型，统一在 semantic 层做编译期检测：

```feng
@value
type A {
  var a: A;     // ❌ 编译错误：值类型不可直接包含自身
}

@value
type B { var a: C; }
@value
type C { var b: B; }   // ❌ 编译错误：值类型间接循环引用
```

**检测范围**：值类型（tuple + `@value type`）的字段类型若直接或间接形成循环（含自身），semantic 阶段报错。普通 `type`（堆对象，引用语义）通过指针引用，大小固定，不受此约束。

**理由**：值类型按值内联布局，类型大小必须编译期确定；自引用会导致无限大小。

**现状修正**：tuple 当前对自引用无编译期检测，codegen 静默吞过（无 C 产出、exit 0、无诊断）。实现 `@value type` 时，在 semantic 层新增**值类型循环引用编译期检测**，覆盖 tuple 与 `@value type`——两者同属值类型（按值内联布局，大小须编译期确定），检测逻辑完全一致，一并修正 tuple 的已有静默失败。

### 3.6 内联布局

`@value type` 作为其他 `type` 的成员字段时，按值内联布局（与 tuple 一致），不经过指针引用：

```feng
@value
type Point {
  var x: float;
  var y: float;
}

type Rect {
  var origin: Point;   // 内联 8 字节（两个 float），不是 Point*
  var size: Point;     // 内联 8 字节
}
```

codegen 的 `cg_emit_c_type` 对 aggregate 类型（tuple、@value type）emit `struct <name>`（按值），对普通对象类型 emit `struct <name> *`（指针）。@value type 走与 tuple 相同的 aggregate 路径。

### 3.7 取地址（`&`）

`&` 操作符要求类型满足 ABI 兼容性（由 `@abi` 注解保障成员的 ABI 兼容）。`@value` 只管值语义，不保障 ABI 兼容；因此 `&` 的支持取决于是否标注 `@abi`：

| 类型 | `&` 语义 | C 层表达 |
|------|---------|---------|
| `@abi type`（堆对象） | payload 地址（跳过 `FengManagedHeader`） | `c_abi_ptr_name(expr)`，`expr` 是堆对象指针 |
| `@value @abi type` | 结构体本身的地址（无托管头，直接取值地址） | `c_abi_ptr_name(&expr)`，`expr` 是值本身，需先取地址 |
| 普通 `type` / `@value type`（未标 `@abi`） | 不支持 | 编译错误 |

```feng
@value @abi
type Point {
  var x: float;
  var y: float;
}

var p = Point { x: 1.0, y: 2.0 };
let addr = &p;   // 类型: Point*，指向 p 的结构体本身（无托管头偏移）
```

**实现细节**（除禁止终结器外，这是 `@value` 与普通 `type` 在语法用法层面的**唯一差别**）：

- `c_abi_ptr_name` 是 codegen 按类型生成的 inline 函数（`struct <abi_layout> *(struct <struct> *self)`），堆 `@abi type` 与 `@value @abi type` 共用此符号。
- 堆 `@abi type`：`self` 指向堆对象（含 `FengManagedHeader`），函数体 `(char*)self + offsetof(first_field)` 跳过 header 返回 payload 地址；`&` 站点 `expr` 已是指针，直接 `c_abi_ptr_name(expr)`。
- `@value @abi type`：无 header，`offsetof(first_field) == 0`，函数体等价于 `(struct <abi_layout> *)self` 直接返回结构体地址；`&` 站点 `expr` 是值表达式（栈上/内联值），需 emit `c_abi_ptr_name(&expr)`。
- codegen 在 `&` 站点按 `@value` 标志区分：`@value` 值 emit 取地址 `&expr` 后传入；堆对象直接传 `expr`。`c_abi_ptr_name` 函数体本身无需分支（`offsetof` 自然为 0）。
- `c_abi_box_name`/`c_abi_value_name`（用于 `@abi func` 形参/返回值 ABI 互操作）在 `@value @abi` 下的行为见 §6.1。

### 3.8 作用域与生命周期

`@value type` 值在栈上布局，作用域退出时自动清理。**生命周期处理与 tuple 完全一致**（trivial 无操作 / aggregate release），无终结器调用路径（`@value type` 禁止定义终结器，见 §2.3）。

- trivial `@value`：无清理操作
- aggregate `@value`：push aggregate 节点（`feng_aggregate_release`），作用域退出时 pop 并调用

对象字段为 `@value type` 时：

- 字段在对象中按 `@value` 大小内联占位
- 字段读写的 retain/release 由 codegen emit 时使用聚合 API
- 对象的 `managed_fields` 按现有展平规则生成（`feng-value-model-delivered.md` §7.2）

数组元素为 `@value type` 时：

- 按值分类走 trivial 或 aggregate 路径（`feng-value-model-delivered.md` §7.3）

---

## 4 Spec 满足

### 4.1 声明头满足

`@value type` 支持在声明头直接满足 spec（与 tuple 不同，tuple 不支持声明头满足）：

```feng
spec Describable {
  func describe(): string;
}

@value
type Point: Describable {
  var x: float;
  var y: float;

  func describe(): string {
    return "(" + x.toString() + ", " + y.toString() + ")";
  }
}
```

### 4.2 fit 扩展

与 tuple 和普通 type 一致：

```feng
@value
type Color {
  let r: u8;
  let g: u8;
  let b: u8;
}

fit Color: Display {
  func toString(): string {
    return "rgb({r}, {g}, {b})";
  }
}
```

### 4.3 Direct-call

`@value type` 的方法直接调用走静态分派，不装箱：

```feng
let p = Point { x: 1.0, y: 2.0 };
p.describe();   // ✅ 静态分派，无装箱
```

这与 tuple 的 direct-call 语义一致（`feng-fit-builtin-type.md` §6.1）。

### 4.4 Spec 视角调用与装箱

当 `@value type` 值进入 spec 视角时（作为 spec 参数传递、赋值给 spec 类型变量、存入 spec 字段等），需要装箱。

```feng
func print(d: Describable): void {
  d.describe().print();
}

let p = Point { x: 1.0, y: 2.0 };
print(p);   // ✅ p 装箱为 spec subject 后传入
```

装箱方案见 §5.3。

---

## 5 描述符生成

### 5.1 Trivial @value type

全字段 trivial 的 `@value type` 生成 `FengTrivialDescriptor`（与 tuple trivial 路径一致）：

```c
// @value type Point { var x: float; var y: float; }
static bool Feng__demo__Point__equal(const void *left, const void *right) {
    const struct Feng__demo__Point *_left = (const struct Feng__demo__Point *)left;
    const struct Feng__demo__Point *_right = (const struct Feng__demo__Point *)right;
    if (_left == _right) return true;
    if (_left == NULL || _right == NULL) return false;
    // 逐字段 ==：trivial 字段走 descriptor->equal_fn 或 memcmp fallback
    if (!(&feng_f32_descriptor)->equal_fn(&_left->x, &_right->x)) return false;
    if (!(&feng_f32_descriptor)->equal_fn(&_left->y, &_right->y)) return false;
    return true;
}

static const FengTrivialDescriptor Feng__demo__Point__trivial_desc = {
    .name = "demo.Point",
    .size = sizeof(struct Feng__demo__Point),
    .equal_fn = &Feng__demo__Point__equal,   // 非 NULL，逐字段比较（与 tuple 一致）
};
```

**命名约定**：@value type 描述符符号按 `value_kind` 动态选择——trivial 用 `<struct>__trivial_desc`，aggregate 用 `<struct>__aggregate_desc`。tuple 现有命名（trivial/aggregate 统一用 `__aggregate_desc`，仅 C 类型不同）在 @value 实现阶段保持不变；**tuple 同步重命名为 `value_kind` 动态命名作为独立 TODO（见 §9.15）**，在 @value 全量交付后启动。

**不增加 `UserType` 字段**：描述符符号名由 `cg_init_user_type_value_symbols`（由原 `cg_init_user_type_tuple_symbols` 重命名）根据 `is_tuple`/`is_value` 与 `value_kind` 动态计算。`cg_aggregate_facts` 返回的 `CGAggregateFacts.descriptor_name` 即为正确符号名——codegen 各处通过 `facts.descriptor_name` 访问，天然按 `value_kind` 区分 trivial/aggregate 后缀。泛型全具体化实例一律用 `__aggregate_desc`（与 §2.4「泛型 @value type 总是 aggregate」一致）。

**实施时机**（关键约束）：`cg_init_user_type_value_symbols` 当前在 `cg_register_user_type`（行 7924）与泛型实例注册（行 6083）处被调用，**早于** `cg_register_user_type_members`（字段注册）。此时 `t->fields[]` 为空，`value_kind` 无法从字段计算。实施策略：

- 非泛型 @value type：在字段注册完成后（后置 pass，可在 `cg_register_user_type_members` 末尾或独立 pass）根据 `value_kind` 计算并设置 `c_aggregate_desc_name`（trivial → `__trivial_desc`，aggregate → `__aggregate_desc`）
- 泛型 @value type 具体实例：一律 `__aggregate_desc`（§2.4），无需 `value_kind` 计算
- 泛型 @value type 定义（开放参数）：不 emit 描述符，名称无关

**关键点**：`equal_fn` **非 NULL**，指向 codegen 生成的逐字段比较函数（`cg_emit_equal_function`，由原 `cg_emit_tuple_equal_function` 重命名 + guard 扩展）。这与 §3.4「不用 `memcmp`」一致——`NULL` 会 fallback 到 `memcmp`，对含浮点字段的类型会给出错误的等值语义（NaN、符号零）。

codegen 的 `cg_trivial_descriptor_expr` 已处理 `CG_TYPE_OBJECT` 且 `facts.value_kind == CG_VK_TRIVIAL` 的情况——返回 `facts.descriptor_name`。@value type 只需在 `cg_aggregate_facts` 中扩展判定即可。

### 5.2 Aggregate @value type

含托管字段的 `@value type` 生成 `FengAggregateDescriptor`（与 tuple 路径一致）：

```c
// @value type User { let id: int; let name: string; }
static bool Feng__demo__User__equal(const void *left, const void *right) {
    /* 逐字段比较：trivial 走 descriptor->equal_fn，托管走各自 == */
    /* ... */
}

static const FengManagedSlotDescriptor Feng__demo__User__aggregate_slots[] = {
    { offsetof(struct Feng__demo__User, name), FENG_SLOT_POINTER, NULL },
};

static const FengAggregateDescriptor Feng__demo__User__aggregate_desc = {
    .name = "demo.User",
    .size = sizeof(struct Feng__demo__User),
    .managed_slot_count = 1,
    .managed_slots = Feng__demo__User__aggregate_slots,
    .equal_fn = &Feng__demo__User__equal,   // 非 NULL，逐字段比较（与 tuple 一致）
    ...
};
```

`equal_fn` 同样**非 NULL**，由 `cg_emit_equal_function`（原 `cg_emit_tuple_equal_function`，重命名 + guard 扩展）生成。`FengAggregateDescriptor.equal_fn` 为 NULL 时表示「不支持 aggregate 等值」，`@value type` 必须生成非 NULL 值以满足 §3.4 的默认值比较语义。

### 5.3 Value Box 描述符

每个 `@value type`（同 tuple）生成独立的 box 描述符，用于装箱后的 spec subject：

```c
struct Feng__demo__User__spec_box {
    FengManagedHeader _hdr;
    struct Feng__demo__User value;
};

static void Feng__demo__User__spec_box_release_children(void *_self) {
    struct Feng__demo__User__spec_box *_box = (struct Feng__demo__User__spec_box *)_self;
    feng_aggregate_release(&_box->value, &Feng__demo__User__aggregate_desc);
}

const FengTypeDescriptor Feng__demo__User__spec_box_desc = {
    .name = "demo.User.__value_box",
    .size = sizeof(struct Feng__demo__User__spec_box),
    .finalizer = NULL,   // @value type 禁止定义终结器（见 §2.3），finalizer 统一为 NULL（与 tuple 一致）
    .release_children = Feng__demo__User__spec_box_release_children,
    .is_potentially_cyclic = true,   // 含托管字段时
    .managed_field_count = 1,
    .managed_fields = ...,
    ...
};
```

@value type 的 box 结构与 tuple 完全相同（`_hdr + embedded value struct`），codegen 可复用同一生成模式。**复用依据**：box 相关的所有生成逻辑——slot 描述符、`equal_fn`、`release_children`、`default_init`、spec coercion——均遍历 `t->fields[]`，通过 `field->c_name`（C 层字段名）和 `offsetof` 访问字段。tuple 的位置访问（`item1`、`item2`）与 `@value type` 的命名访问（`x`、`y`）仅在 Feng 源码层面不同，C struct 层面均为 `field->c_name`，不影响生成逻辑。`cg_emit_spec_box_subject`（原 `cg_emit_tuple_spec_box_subject`）的 spec coercion 发码（`feng_object_new` + `feng_aggregate_assign`/直接赋值）同样不逐个访问成员，将 value 作为整体处理。因此，按隔离策略（§7.4），tuple codegen 函数经重命名 + guard 扩展后直接复用，不需要创建独立函数。`UserType` 中 `c_tuple_box_*` 字段重命名为 `c_value_box_*`，tuple 与 `@value` 共用（详见 §7.2）。

**每个值类型的 box 是单独生成的**（per-type codegen，tuple 与 `@value type` 均如此）。`@value type` 禁止定义终结器（见 §2.3），故 box 的 `finalizer` 字段统一为 `NULL`（与 tuple 完全一致）。box 释放时仅由 runtime 框架调用 `release_children` 走 `feng_aggregate_release`。

`FengScalarBox`（runtime 预编译的固定 union，服务 11 种标量）保持不变——标量 box 与 per-type box 在 payload 布局、构造函数、descriptor 策略上本质不同，强行合并无收益。

**Non-escape 优化**：当 `@value` 值仅在调用栈帧内消费时（临时 coercion），可直接使用栈上地址作为 subject，不分配 box。逃逸到局部绑定、返回值或字段存储时才分配 box。口径沿用现有定义（`feng-fit-builtin-type.md` §6.3）。

---

## 6 与现有机制的交互

### 6.1 `@value` 与 `@abi`

`@value` 与 `@abi` **可组合**，各管各的职责：

- `@value`：值语义（无托管头、栈/内联分配、赋值复制值）。
- `@abi`：ABI 兼容性（成员类型满足 C ABI 要求，支持 `&` 取地址、C ABI 互操作）。

两者正交：`@value @abi type` 是「值语义 + ABI 兼容」的类型，无托管头但成员均 ABI 兼容，`&` 取结构体地址（非 payload 地址，因无 header 可跳过）。参见 §3.7。

**终结器约束**：`@value type` 本身禁止定义终结器（见 §2.3），`@abi` 的终结器禁止规则（AE0317）对此自然满足，无需额外约束。

**ABI 互操作（`@abi func` 形参/返回值）**：`@value @abi type` 作为 `@abi func` 形参或返回值时，按 ABI 结构体值语义直接传递，不经 `feng_object_new` 堆分配：

- **形参**：`c_abi_value_name` 从 Feng 值提取 ABI 结构体。对 `@value @abi type`，Feng 值本身即 ABI 结构（无 header 偏移），提取为平凡字段拷贝；现有 `c_abi_value_name` 实现按字段逐个拷贝，对 `@value @abi` 语义正确。
- **返回值**：`c_abi_box_name` 将 ABI 结构体装箱为 Feng 堆对象（`feng_object_new(&<c_desc_name>)`）。对 `@value @abi type` **不适用**——`@value type` 不生成 `FengTypeDescriptor` 定义（`c_desc_name` 字段虽有值，但对应的 C 符号不会被 emit；值类型仅有 trivial/aggregate 值描述符 `c_aggregate_desc_name` 与 spec box 描述符 `c_value_box_desc_name`），且 `@value` 值不堆分配。codegen 需在 `@abi func` 返回站点按 `@value` 标志跳过 `c_abi_box_name` 调用，直接按值返回 ABI 结构体给调用方绑定；`c_abi_box_name` 符号本身对 `@value @abi type` 不生成。

**普通 Feng 函数形参/返回值**：`@value type` 作为普通 Feng 函数形参时按值传递（`cg_aggregate_facts` 级联，`cg_emit_c_type` emit `struct X`），无需特殊处理；返回值同理（`cg_emit_return` 的 aggregate/trivial 分支级联自动处理）。仅 `@abi func` 需要上述额外互操作处理。

### 6.2 异常

`@value type` 可作为异常抛出类型。装箱路径与现有标量/tuple 异常一致——异常 payload 需要堆承载。

### 6.3 Cycle Collector

`@value type` 值本身在栈上，不参与 cycle collector。

当 `@value type` 值被装箱为 spec subject 时，box 是托管堆对象，按现有 `FengTypeDescriptor` 路径参与 CC。box 的 `managed_fields` 由 codegen 按 `feng-value-model-delivered.md` §7.2 展平规则生成。

`@value type` 作为对象字段时：对象的 `managed_fields` 按现有展平规则生成（`feng-value-model-delivered.md` §7.2），CC 不感知"这条来自一个 @value 聚合字段"。

### 6.4 数组

`@value type` 作为数组元素时：

- trivial `@value`：元素按值存储，memcpy 复制，无 retain/release
- aggregate `@value`：元素按值存储，逐元素调用聚合 API

与 tuple 数组元素处理路径一致。

---

## 7 实现影响评估

### 7.1 Runtime

**零修改**。

- `FengManagedHeader`、`FengTypeTag`：不变
- 单指针原语（`feng_retain/release/assign`）：不变
- 五类聚合 API：不变
- `FengScalarBox`：不变
- Cycle collector：不变（box 走现有 `FengTypeDescriptor` 路径）
- 数组元素分类：不变（已支持三分类）
- `FENG_NODE_DEFER`：不变（`@value type` 禁止终结器，见 §2.3，不使用此节点注册栈值终结器）

### 7.2 Codegen

**最小改动**，核心是将 tuple 的值类型 codegen 函数泛化为 tuple 与 `@value` 共用。这些函数全部基于 `t->fields[]` + `field->c_name` + `offsetof`，与成员访问方式（位置/命名）无关，仅需 guard 从「仅 tuple」扩展为「tuple 或 `@value`」+ 函数重命名（命名需与实际用途一致）。无需创建独立函数。构造器、ABI surface、成员访问/赋值、方法调用站需独立处理（详见 §7.4.2）。**方法签名与 self 完全复用普通 type 路径，codegen 零修改**。

| 变更项 | 工作量 | 说明 |
|--------|--------|------|
| `UserType` 字段重命名 | 小 | `c_tuple_box_*` → `c_value_box_*`（tuple 与 `@value` box 符号共用，命名需与实际用途一致） |
| box 符号初始化函数入口扩展 | 小 | 条件从 tuple 扩展为 tuple \|\| value |
| `cg_aggregate_facts` 扩展 | 小 | `CG_TYPE_OBJECT` 分支条件扩展（见下方示意） |
| 主结构体 emit | 小 | `cg_emit_tuple_type_definition` 重命名为 `cg_emit_value_type_definition`，guard 从 `is_tuple` 扩展为 `is_tuple \|\| is_value`（`@value type` 禁止终结器，见 §2.3，不 emit `c_finalizer_name`）。**`cg_emit_user_type_forward`**：@value 新增独立分支（与 tuple 分支并列），struct body emit 走普通 type 路径（花括号字段，已有逻辑），guard 扩展**仅针对描述符/box/equal_fn 前向声明**——描述符类型由 `value_kind` 决定（trivial → `FengTrivialDescriptor`，aggregate → `FengAggregateDescriptor`），描述符符号名取自 `t->c_aggregate_desc_name`（由 `cg_init_user_type_value_symbols` 按 §5.1 命名约定计算）。box descriptor `.name` 中 `__tuple_box` 改为动态选择（tuple/`@value`） |
| 构造器 emit | 中 | 现有构造器路径硬编码 `feng_object_new` 堆分配，无法直接复用。新增独立函数 `cg_emit_value_type_construction`（栈分配 `struct X _val = {0}` + `&_val` 传 self，构造器签名与普通 type 一致），在 `cg_emit_call` 与 `FENG_EXPR_OBJECT_LITERAL` 入口加早期分支分发。原构造器路径零修改。详见 §7.4 |
| 方法签名与 self | 小 | @value 方法签名与普通 type **一致**——`struct X *self`（指针，因 `var` 成员需原地修改），`cg_emit_user_method_proto` 与 `cg_emit_user_method` 的 else 分支直接适用。**self 绑定策略**（用户决策）：@value 方法 self 用指针（方法体内可修改 `var` 成员），逃逸时复制——self 绑定 c_name 改为 `(*self)`，确保 self 在值上下文（绑定/返回/传参/lambda 捕获）自动解引用为值复制，字段访问与 mutation 保持 lvalue 语义（详见 §7.4.2 第 5 项）。方法调用站：@value recv 需 materialize 后传 `&local`（见 §7.4.2 第 4 项）。**tuple self 指针化作为独立 TODO（见 §9.16）** |
| box struct/desc/release emit | 小 | 包含在主结构体 emit 中（`cg_emit_value_type_definition`，由原 `cg_emit_tuple_type_definition` 重命名 + guard 扩展）。box descriptor 的 `finalizer` 字段统一为 `NULL`（`@value type` 禁止终结器，见 §2.3、§5.3）。`release_children` 走 `feng_aggregate_release`（与 tuple 一致） |
| spec box subject | 小 | `cg_emit_tuple_spec_box_subject` 重命名为 `cg_emit_spec_box_subject`，guard 从 `is_tuple` 扩展为 `is_tuple \|\| is_value`。spec 强制转换分发点的 tuple 分支 guard 同步扩展，不新增分支。发码逻辑将 value 作为整体处理（`feng_object_new` + `feng_aggregate_assign`/直接赋值），不逐个访问成员（详见 §5.3） |
| witness 生成路径 | 中 | `cg_ensure_tuple_box_witness_instance` 重命名为 `cg_ensure_value_box_witness_instance`，guard 扩展为 `is_tuple \|\| is_value`。@value 逃逸为 spec subject 时是 box（`_hdr + value`），需要与 tuple 相同的解包逻辑（`((struct box *)_subject)->value`），**不走**普通 type 的 `cg_ensure_witness_instance_for_type`（后者 subject 是堆指针，无解包）。**thunk 传参**：@value box thunk 传 `&box->value`（取地址，因 @value 方法 self 为指针，§7.4.2 第 5 项）；tuple box thunk 传值保持不变（tuple self 指针化作为独立 TODO，见 §9.16）。缓存表与命名前缀同步泛化 |
| trivial/aggregate descriptor emit | 小 | `cg_emit_tuple_equal_function` 重命名为 `cg_emit_equal_function`，由 `cg_emit_value_type_definition` 调用（guard 扩展后自动覆盖）。错误消息中 "tuple" 文本泛化。`equal_fn` 非 NULL（见 §3.4） |
| `&` 取地址（`@value @abi` 组合） | 小 | `c_abi_ptr_name` 函数体无需分支（`offsetof` 自然为 0）；`&` 站点按 `@value` 标志 emit 取地址 `&expr`（堆对象直接传 `expr`）；语义层补 `@value @abi` 注解组合校验 |
| `@abi func` 形参/返回的 ABI 互操作 | 小 | `cg_emit_user_type_abi_surface` 入口新增 `@value` 早期分支，分发至独立函数 `cg_emit_value_type_abi_surface`（仅生成 `c_abi_ptr_name` + `c_abi_value_name`，不生成 `c_abi_box_name`）。`c_abi_value_name` 对 `@value @abi` 是平凡拷贝（值本身即 ABI 结构）；codegen 在 `@abi func` 返回站点按 `@value` 标志跳过 box 调用，直接按值返回（见 §6.1）。原 ABI surface 函数零修改 |
| 终结器禁止诊断 | 小 | `@value type` 定义终结器时编译期报错（见 §2.3、§9.3），不 emit `c_finalizer_name`，不 push `FENG_NODE_DEFER`，无需 `cg_release_scope` combined 路径 |

**函数重命名清单**（命名需与实际用途一致——这些函数不再仅服务 tuple）：

| 现有名称 | 重命名为 | 说明 |
|---------|---------|------|
| `cg_emit_tuple_type_definition` | `cg_emit_value_type_definition` | 值类型描述符 + box 生成 |
| （`cg_emit_user_type_forward` 内新增 @value 分支） | — | tuple 分支已有，新增 @value 分支（struct body 走普通 type 路径，描述符/box/equal_fn 前向声明 guard 扩展，见 §7.2 主结构体 emit 行） |
| `cg_emit_tuple_equal_function` | `cg_emit_equal_function` | 逐字段比较函数生成 |
| `cg_emit_tuple_spec_box_subject` | `cg_emit_spec_box_subject` | spec coercion 装箱 |
| `cg_ensure_tuple_box_witness_instance` | `cg_ensure_value_box_witness_instance` | box witness thunk 生成 + 缓存 |
| `cg_tuple_aggregate_top_level_slot_count` | `cg_aggregate_top_level_slot_count` | 非 trivial 字段计数 |
| `cg_init_user_type_tuple_symbols` | `cg_init_user_type_value_symbols` | box/aggregate 符号名初始化 |
| `c_tuple_box_struct_name` | `c_value_box_struct_name` | UserType 字段 |
| `c_tuple_box_desc_name` | `c_value_box_desc_name` | UserType 字段 |
| `c_tuple_box_release_children_name` | `c_value_box_release_children_name` | UserType 字段 |

Guard 扩展示意（适用于上述所有函数）：

```c
// 现有：cg_emit_tuple_type_definition、cg_emit_tuple_spec_box_subject、
//       cg_ensure_tuple_box_witness_instance 等函数的 guard
if (!cg_user_type_is_tuple(t)) { return ...; }

// 泛化：重命名函数 + guard 扩展
if (!cg_user_type_is_tuple(t) && !cg_user_type_is_value(t)) { return ...; }

// cg_aggregate_facts 的 guard（同模式）
if (!cg_type_is_tuple_user(t) && !cg_type_is_value_user(t)) { return false; }
```

生成逻辑本身零修改——box struct 布局、release_children 生成、equal_fn 生成、witness thunk 生成对 tuple 和 @value type 完全一致；box descriptor 的 `finalizer` 字段：tuple 与 `@value type` 均一律 `NULL`（`@value type` 禁止终结器，见 §2.3、§5.3）。

**后续优化：描述符名称统一**——当前 `UserType` 中 `c_desc_name`（`FengTypeDescriptor`，堆对象）与 `c_aggregate_desc_name`（`FengAggregateDescriptor`/`FengTrivialDescriptor`，值类型）是独立字段，每新增一种 `value_kind` 需增加字段，不符合 OCP。更好的设计是 `c_desc_name` 作为统一的描述符名称字段，`value_kind` 仅决定描述符 C 类型与后缀（`MANAGED_POINTER` → `FengTypeDesc__...`，`TRIVIAL` → `...__trivial_desc`，`AGGREGATE` → `...__aggregate_desc`）。本次实现先保持现有字段结构不变，后续单独重构。

### 7.3 Semantic

| 变更项 | 工作量 | 说明 |
|--------|--------|------|
| `@value` 注解解析 | 小 | 与 `@abi` 类似的注解识别 |
| 值分类判定 | 小 | codegen 层 `cg_aggregate_facts` 扩展入口即可（trivial/aggregate 细分复用同一逻辑）；semantic 层 `feng_semantic_value_kind_of_decl` 对 `FENG_DECL_TYPE` 一律返回 `MANAGED_POINTER`（视 type 引用为托管指针），与 tuple 处理一致，**无需修改函数本身**。审计范围见 §7.4 |
| 值类型循环引用检测 | 中 | **新增值类型循环引用编译期检测**，覆盖 tuple + `@value type`（一并修正 tuple 现有静默失败 bug）；直接自引用 + 间接循环均报错；普通 `type`（堆对象，引用语义，大小固定）不受约束。详见 §3.5 |
| 等值运算符 | 小 | `@value type` 的 `==`/`!=` 走 codegen 生成的 `equal_fn`（`cg_emit_equal_function`，由原 tuple 函数重命名 + guard 扩展），semantic 层仅识别 `@value` 类型允许默认值比较 |
| spec 满足性检查 | 小 | 声明头满足与普通 type 一致（复用普通 type 路径）；`fit` 路径一致 |
| 构造器语义 | 小 | 与普通 type 一致，`self` 指向栈上值地址（普通 type 的 `self` 指向堆对象）。codegen 层由独立函数 `cg_emit_value_type_construction` 处理（见 §7.2、§7.4） |
| 终结器禁止诊断 | 小 | `@value type` 定义终结器时编译期报错（见 §2.3） |

### 7.4 风险分析与隔离策略

> 本节基于代码审计（codegen.c ~35000 行），识别实施风险并确定隔离策略。
> 核心发现：tuple codegen 函数全部基于 `field->c_name` + `offsetof`，与成员访问方式（位置/命名）无关，可通过重命名 + guard 扩展直接复用。**构造器、ABI surface、成员访问/赋值、方法调用站需独立处理**（详见 §7.4.2）。方法签名与 self 处理完全复用普通 type 路径，无需修改。

#### 7.4.1 `cg_aggregate_facts` 级联效应

`cg_aggregate_facts` 是 codegen 层值分类的核心入口。一旦对 `@value type` 返回 true，以下函数**自动正确处理**，无需逐一修改：

| 函数 | 自动行为 | 正确性 |
|------|---------|--------|
| `cgtype_value_kind` | 从 `MANAGED_POINTER` 变为 `TRIVIAL`/`AGGREGATE` | ✅ 正确 |
| `cgtype_is_managed` | 返回 false | ✅ 正确（@value 非托管指针） |
| `cgtype_is_aggregate` | aggregate 时返回 true | ✅ 正确 |
| `cg_emit_c_type` | emit `struct X`（非 `struct X *`） | ✅ 正确（值语义内联布局） |
| `cg_release_scope` | 走 aggregate release 路径 | ✅ 正确（与 tuple 一致） |
| `cg_trivial_descriptor_expr` | `CG_TYPE_OBJECT` 分支走 aggregate facts | ✅ 正确 |
| 字段赋值/清理等 71 处 | `cgtype_is_managed` 返回 false 后自动跳过 ARC | ✅ 正确 |

#### 7.4.2 需独立实现的风险点

以下 5 项无法通过 guard 扩展或级联处理，需新增独立处理 + 入口分支：

| # | 风险点 | 现状 | 隔离策略 | 原函数改动 |
|---|--------|------|---------|-----------|
| 1 | **构造器分配** | 两个入口（`cg_emit_call` 构造路径、`FENG_EXPR_OBJECT_LITERAL`）硬编码 `feng_object_new` 堆分配 + `->` 字段初始化 + 构造器传堆指针。tuple 无构造器（字面量贴合），无可复用路径 | 新增 `cg_emit_value_type_construction`：栈声明 `struct X _val = {0}` + `&_val` 传 self（构造器签名与普通 type 一致，`struct X *self`） | 各入口 +1 行 `if (cg_user_type_is_value(ut))` 早期分支 |
| 2 | **ABI surface** | `cg_emit_user_type_abi_surface` 生成 `c_abi_box_name`，函数体硬编码 `feng_object_new(&c_desc_name)`。tuple 不走此路径，无可复用函数 | 新增 `cg_emit_value_type_abi_surface`：仅生成 `c_abi_ptr_name`（offset=0）+ `c_abi_value_name`，不生成 `c_abi_box_name` | `cg_emit_user_type_abi_surface` 入口 +1 行早期返回 |
| 3 | **成员访问与赋值** | 赋值路径：`cg_emit_assign` 中 user type 字段赋值全部硬编码 `(recv)->field`（`->` 访问）。读取路径：`cg_emit_member` 普通对象分支（行 16433）emit `(%s)->%s`。@value 变量是值（非指针），两路径的 `->` 均导致 C 编译错误。tuple 读取路径已有 `.` 分支（行 16349），赋值路径缺失 | 赋值：`cg_emit_assign` 成员赋值路径（trivial/compound/aggregate/managed 四分支）新增 @value 分发，`cg_type_is_value_user(recv.type)` 时使用 `(recv).field` 替代 `(recv)->field`。读取：`cg_emit_member` 普通对象分支新增 @value guard，emit `(recv).field` 替代 `(recv)->field`。嵌套 @value 字段（如 `obj.value_field.x`）由成员访问递归自然处理。两者同步实现 | `cg_emit_assign` 成员赋值段 + `cg_emit_member` 普通对象分支各新增 @value 条件分支 |
| 4 | **方法调用站 recv 取地址** | `cg_emit_call` 直接方法调用路径 emit `um->c_name(recv.c_expr, ...)`。泛型共享体方法路径 emit `shared_name((void *)recv, ...)`。普通 type recv 是指针，直传正确。@value recv 是值表达式（可能是 rvalue），方法签名要求 `struct X *self`，需传地址 | @value recv 按 lvalue/rvalue 分发：lvalue recv（AST 为 IDENTIFIER/SELF/MEMBER/INDEX）直接传 `&recv.c_expr`（mutations 传播回原值）；rvalue recv（函数返回/字面量）先 `cg_materialize_to_local` 后传 `&local`。泛型共享体路径同理。aggregate rvalue 必须先 materialize 才能取地址 | `cg_emit_call` 方法调用段（含泛型共享体路径）+ `cg_emit_generic_type_method_call` + `cg_emit_generic_type_self_method_call` 新增 @value 条件分支 |
| 5 | **@value self 逃逸语义（含 lambda 捕获）** | @value 方法的 C 参数是 `struct X *self`（指针，因 `var` 成员需原地修改），当前 self 绑定 c_name = `"self"`（指针本身）。但 self 一旦逃逸（绑定到其他变量/返回/作为入参/lambda 捕获），直接 emit 指针会破坏值语义且可能悬空。普通 type self 是堆指针，retain/release 保证生命周期，无需复制 | **@value self 语义**（用户决策）：方法体内 self 用指针（`var` 成员可原地修改），逃逸时复制。实现：@value 方法的 self 为 `struct X *self`（指针），绑定时 c_name 设为 `(*self)`。效果：`self.field` → `((*self)).field` = `self->field`（修改原值，满足 `var` 成员原地修改，不逃逸）；`let y = self` → `struct X y = (*self)`（值复制，逃逸）；`return self` → `return (*self)`（返回值，逃逸）；`foo(self)` → `foo((*self))`（传值，逃逸）；`self.length()` → `method(&(*self))` = `method(self)`（传地址，不逃逸）；lambda 捕获 → `cg_scope_bind_capture_cell` 的 source_expr 传 `(*self)`，cell->value 存值副本（闭包生命周期独立于栈帧，逃逸）。`(*self)` 是 C lvalue，赋值/取地址/字段访问全部自然工作。普通 type self 保持 `"self"`（指针 + retain）不变。**tuple self 指针化作为独立 TODO（见 §9.16）** | `cg_emit_user_method` 中 @value 的 self 绑定路径（`scope_add` c_name 改为 `(*self)`；`cg_scope_bind_capture_cell` source_expr 改为 `(*self)`）新增分支 |

#### 7.4.3 `CG_TYPE_OBJECT` 堆假设审计

codegen 中 `CG_TYPE_OBJECT` 出现 62 处、`cgtype_is_managed` 116 处，部分站点直接假设堆对象语义。

- 级联自动处理大部分（见 §7.4.1）
- `feng_object_new`（风险点 1 已隔离）
- `c_desc_name` 引用（`c_desc_name` 字段对所有类型均有值，但 `@value type` 不 emit 对应的 `FengTypeDescriptor` C 符号；guard 扩展后 @value 走 `cg_emit_value_type_definition`，该函数使用 `c_aggregate_desc_name` 而非 `c_desc_name`，不会引用未定义符号）
- `feng_assign`/`feng_retain`/`feng_release` 直接调用（`cgtype_is_managed` 返回 false 后自动跳过）
- `->` 字段访问（风险点 3、4 已隔离）

无需逐一修改，仅需确认级联覆盖完整。

#### 7.4.4 无风险项（文档假设已验证正确）

- `feng_semantic_value_kind_of_decl`：仅测试文件调用，无生产依赖，确认无需修改。
- `c_abi_ptr_name` 函数体：`(char *)self + offset`，@value 时 `offsetof(first_field) == 0`，函数体无需分支。
- `c_abi_value_name` 函数体：`_abi.field = self->field`（`self` 是指针，`->` 访问）。函数体本身无需变更；调用约定由调用方适配——对 `@value @abi`，调用方 emit `&val` 传入（值取地址），与风险点 2（§7.4.2）联动处理。
- Witness thunk `_subject` 转型：`(struct X *)_subject`，对 box（`_hdr + value`）同样适用。
- `cg_emit_equal_function`（原 `cg_emit_tuple_equal_function`）：按字段遍历生成比较代码，guard 扩展后直接复用。
- `cg_emit_user_method_proto` / `cg_emit_user_method`：@value 方法走 else 分支（`struct X *self`），与普通 type 一致，C 签名无需修改。方法体内 self 绑定 c_name 需改为 `(*self)`（见 §7.4.2 第 5 项）。构造器同理（构造器 self 绑定为 `&_val`，已在 `cg_emit_user_type_member_initializers` 中处理，不受影响）。
- 方法体中 `self.field` / `self->field` 访问：self 绑定为 `(*self)` 后，`self.field` emit 为 `((*self)).field` = `self->field`（修改原值），语义正确。普通 type 方法 self 绑定为 `self`（指针），`self->field` 访问不变。
- `cg_emit_user_type_member_initializers` 中 self 绑定：`object_expr` 为 `&_val`（指针），`self` 绑定为 CG_TYPE_OBJECT，`self->field` 访问正确。

---

## 8 关联文档

- [feng-value-model-delivered.md](./feng-value-model-delivered.md)：值模型基础设施（Phase 3 的基座）
- [feng-type.md](../docs/feng-type.md)：`type` 类型规范
- [feng-tuple.md](../docs/feng-tuple.md)：元组规范
- [feng-spec.md](../docs/feng-spec.md)：`spec` 规范
- [feng-fit.md](../docs/feng-fit.md)：`fit` 扩展规范
- [feng-fit-builtin-type.md](../docs/feng-fit-builtin-type.md)：内建类型 fit 与装箱约束

---

## 9 开发任务拆解

> 本节将 `@value` 内建注解的开发任务拆解为 14 个可分步交付的 TODO（§9.1-§9.14，@value 主体）+ 2 个独立后续 TODO（§9.15 tuple 描述符命名统一、§9.16 tuple self 指针化），每步可独立通过全量回归测试并独立交付。
> 每步遵循 CLAUDE.md 基础原则：「先文档 → 后代码 → 后测试 → 全量回归」。
> 步骤间存在依赖关系（见每步「依赖」字段），按编号顺序交付；每步本身可独立交付并通过全量回归。
> 完成状态：**状态**字段标记「未开始 / 进行中 / 已完成」，子项用 `- [ ]` / `- [x]` 标记。
> Runtime 在全流程中零修改（§7.1）。

### 复用与参考分类

每个步骤标题标注复用/参考对象，区分三类：

| 分类 | 含义 | 工作量 |
|------|------|--------|
| **复用普通 type** | 主路径（semantic + codegen）复用普通 type，发码区别点做分支 | 小 |
| **复用元组** | tuple codegen 函数重命名 + guard 扩展，直接复用（详见 §7.2 函数重命名清单） | 小 |
| **新增** | 无复用对象，需独立实现 | 中 |

**复用普通 type 的发码区别点**（§7.4.2 五项风险点）：
- 构造器：栈分配（`cg_emit_value_type_construction`）vs 堆分配（`feng_object_new`）
- ABI surface：无 box（`cg_emit_value_type_abi_surface`）vs 有 box（`c_abi_box_name`）
- 成员访问与赋值：`.` 访问 vs `->` 访问（`cg_emit_assign` @value 分支）
- 方法调用站：recv materialize + 取地址 vs 直传指针
- self 逃逸语义：self 绑定 c_name 为 `(*self)`，逃逸时自动解引用为值复制 vs 普通 type 直传指针

**复用元组的三项**（重命名 + guard 扩展）：
- 描述符生成：`cg_emit_value_type_definition`（原 `cg_emit_tuple_type_definition`）——trivial/aggregate descriptor + box + equal_fn
- 生命周期：`cg_aggregate_facts` 级联 + 聚合 API（`feng_aggregate_retain/release/assign/take/default_init`）
- 装箱与 witness：`cg_emit_spec_box_subject`（原 `cg_emit_tuple_spec_box_subject`）+ `cg_ensure_value_box_witness_instance`（原 `cg_ensure_tuple_box_witness_instance`）

> 「复用元组」指 tuple codegen 函数经重命名 + guard 扩展后直接调用，不创建独立函数（详见 §5.3、§7.2）。

### 9.1 `@value` 注解解析与 AST 扩展【新增】

**状态**：已完成 ｜ **依赖**：无 ｜ **范围**：§7.3

**变更**：
- [x] Lexer 层新增 `FENG_ANNOTATION_VALUE`（`FengAnnotationKind` 枚举，参考 `FENG_ANNOTATION_ABI`）
- [x] Parser 层识别 `@value` 注解，设置 AST `is_value` 标志位
- [x] Semantic 层校验：仅可用于 `type` 声明，用于 `spec`/`fit`/函数/变量时报错
- [x] `@value type` 暂按普通 `type` 处理，不改变现有行为

**测试**：
- [x] 注解错误使用的诊断码（test/）
- [x] 正常使用不影响现有 fcts/ 行为（全量回归）

### 9.2 值类型循环引用编译期检测【新增】

**状态**：已完成 ｜ **依赖**：9.1 ｜ **范围**：§3.5、§7.3

**变更**：
- [x] Semantic 层新增值类型循环引用检测（直接自引用 + 间接循环均报错）
- [x] 检测覆盖 tuple 与 `@value type`（两者同属值类型，检测逻辑一致）
- [x] 普通 `type`（堆对象，引用语义，大小固定）不受约束
- [x] 一并修正 tuple 现有静默失败（无 C 产出、exit 0、无诊断）

**测试**：
- [x] tuple 自引用/间接循环：报编译错误，不再静默吞过（test/）
- [x] `@value type` 自引用/间接循环：报编译错误（test/）
- [x] 普通 `type` 自引用：允许（test/）
- [x] 深度间接循环（A→B→C→D→A）：报编译错误（test/）
- [x] 跨文件同模块值类型循环：报编译错误（test/）
- [x] 跨模块值类型循环（全限定名）：报编译错误（test/）
- [x] 泛型实参产生的循环（直接/嵌套 type_args）：报编译错误（test/）
- [x] tuple ↔ @value 混合循环：报编译错误（test/）
- [x] 数组包装（T[]）自引用：报编译错误（test/）
- [x] 指针 `*T` 自引用：允许（指针固定大小）（test/）
- [x] spec / union spec / callable spec / enum 字段：不产生值类型边（test/）
- [x] 全量回归

### 9.3 终结器禁止诊断【新增】

**状态**：已完成 ｜ **依赖**：9.1 ｜ **范围**：§2.3、§7.3

**变更**：
- [x] Semantic 层检测 `@value type` 定义终结器，编译期报错（§2.3）
- [x] 诊断码定义（test/）

**测试**：
- [x] `@value type` 定义终结器：报编译错误（test/）
- [x] 普通 `type` 定义终结器：允许（回归）
- [x] 全量回归

### 9.4 值语义（trivial + aggregate）【复用普通 type + 复用元组】

**状态**：已完成 ｜ **依赖**：9.1、9.2 ｜ **范围**：§3.1、§3.2、§3.3、§3.4、§3.6、§5.1、§5.2、§7.2

**变更**：
- [x] Semantic 层值分类判定（复用元组三分类：trivial / aggregate，§3.1）
- [x] 审计 codegen `CG_TYPE_OBJECT` 堆假设站点（确认级联覆盖完整，§7.4.3）
- [x] Codegen 层 `cg_aggregate_facts` guard 扩展（`tuple || value`，§7.2）
- [x] `cg_emit_tuple_type_definition` 重命名为 `cg_emit_value_type_definition`，guard 扩展为 `is_tuple || is_value`；`cg_emit_user_type_forward` 分发 guard 同步扩展（§7.2 函数重命名清单）
- [x] Trivial/Aggregate descriptor 实例生成（由 `cg_emit_value_type_definition` 统一处理，per-type，描述符类型由 `value_kind` 决定：trivial → `FengTrivialDescriptor`，aggregate → `FengAggregateDescriptor`，§5.1、§5.2）
- [x] `equal_fn` 函数生成（`cg_emit_equal_function`，由原 `cg_emit_tuple_equal_function` 重命名 + guard 扩展，per-type，§5.1、§5.2）
- [x] `FengAggregateDefaultInitDescriptor` 生成（由 `cg_emit_value_type_definition` 统一处理，per-type）
- [x] 基本值语义：赋值、传参、内联布局（`cg_aggregate_facts` 级联 + 聚合 API，§3.2、§3.6）
- [x] 成员访问：`cg_emit_member` tuple `.` 分支 guard 扩展为 `cg_type_is_tuple_user(t) || cg_type_is_value_user(t)`（变量/字段读取 `p.x`，§7.4.2 第 3 项关联）
- [x] 成员赋值：`cg_emit_assign` 成员赋值路径新增 @value 分发（trivial/compound/aggregate/managed 四分支，`(recv).field` 替代 `(recv)->field`，§7.4.2 第 3 项）
- [x] 等值比较（`==`/`!=` 走 `equal_fn`，§3.4）
- [x] 成员可变性（`let`/`var`，§3.3）

**测试**：
- [x] trivial `@value type` 声明/赋值/传参/字段内联（fcts/）
- [x] aggregate `@value type` 声明/赋值/传参（含 `string`/对象引用字段）
- [x] 成员字段赋值（`p.x = 3.0`、`p.name = "new"`、compound `p.x += 1.0`）
- [x] 嵌套字段访问与赋值（`obj.value_field.x = 3.0`、`p.nested.y = 1.0`）
- [x] `==`/`!=` 值比较（trivial + aggregate）
- [x] `let`/`var` 字段可变性（与普通 type 一致）
- [x] 字段 retain/release 正确性（aggregate）
- [x] 全量回归

### 9.5 构造器【新增独立函数，参考普通 type 构造器】

**状态**：已完成 ｜ **依赖**：9.4 ｜ **范围**：§2.2、§7.2、§7.4

**变更**：
- [x] 新增独立函数 `cg_emit_value_type_construction`：栈分配 `struct X _val = {0}` + `(&_val)` 传 self（§7.4.2 第 1 项）
- [x] `cg_emit_call` 构造器路径入口 + `FENG_EXPR_OBJECT_LITERAL` 入口各加 1 行 `@value` 早期分支
- [x] 构造器签名与普通 type **一致**：`struct X *self`（指针指向栈上值），`self->field = value` 修改正在初始化的值
- [x] 字面量贴合（`Counter {}`）+ 参数构造（`Counter(10)`）
- [x] 原构造器路径零修改
- [x] 构造器 self 绑定：@value 构造器的 C 参数为 `struct X *self`（指针），绑定 Feng name `"self"` 到 C 表达式 `"(*self)"`，使构造器体内 `self.field` 生成 `((*self)).field` ≡ `self->field`（同时适用于 `cg_emit_user_method` 非捕获/捕获路径和 `cg_emit_user_type_member_initializers` 字段初始化器捕获路径）

**测试**：
- [x] 构造器调用（fcts/）
- [x] `self` 语义（修改字段生效，与普通 type 行为一致）
- [x] 多构造器重载
- [x] trivial/aggregate @value type 构造器（含默认构造器与参数构造器）
- [x] 对象字面量 + 构造器组合（`VCounter {}`、`VCounter { count: 42 }`、`VCounter(10)`）
- [x] 构造器返回值语义（复制后独立）
- [x] 构造器从函数返回
- [x] 全量回归

### 9.6 方法（含 direct-call）【复用普通 type】

**状态**：已完成 ｜ **依赖**：9.4 ｜ **范围**：§2.5、§4.3、§7.2

**变更**：
- [x] 复用普通 type 方法 emit + direct-call 路径（静态分派，不装箱）——方法 C 签名复用普通 type 路径（`struct X *self`），self 绑定策略需独立处理（见下方）
- [x] self 参数：与普通 type **一致**——`struct X *self`（指针），C 签名无需修改。不需要扩展 tuple guard（@value 走 else 分支，与普通 type 一致）
- [x] self 绑定策略（§7.4.2 第 5 项）——**@value self 语义**（用户决策）：方法体内 self 用指针（`var` 成员可原地修改），逃逸时复制。实现：@value 方法的 self 为 `struct X *self`（指针），绑定时 c_name 改为 `(*self)`。效果：`self.field` → `((*self)).field`（修改原值，满足 `var` 成员原地修改，不逃逸）；`let y = self` → `struct X y = (*self)`（值复制，逃逸）；`return self` → `return (*self)`（返回值，逃逸）；`foo(self)` → `foo((*self))`（传值，逃逸）；`self.length()` → `method(&(*self))` = `method(self)`（传地址，不逃逸）；lambda 捕获 self → `cg_scope_bind_capture_cell` source_expr 传 `(*self)`，cell 存值副本（闭包生命周期独立于栈帧，逃逸）。普通 type self 保持 `"self"`（指针 + retain）不变。**tuple self 指针化作为独立 TODO（见 §9.16）**
- [x] 方法调用站：@value recv 按 lvalue/rvalue 分发（§7.4.2 第 4 项）——lvalue recv（IDENTIFIER/SELF/MEMBER/INDEX）直接传 `&recv.c_expr`（方法 mutations 传播回原值）；rvalue recv（函数返回/字面量）先 materialize 后传 `&local`
- [x] self 语义：`self` 指向调用方的值地址（普通 type 的 `self` 指向堆对象），`self.field` 的可变性遵循 `let`/`var` 规则，与普通 type 100% 一致

**测试**：
- [x] 方法调用（fcts/）
- [x] `self` 语义（修改 `var` 字段对调用方可见，与普通 type 行为一致）
- [x] `let` 字段不可修改（编译期报错，与普通 type 一致）
- [x] self 逃逸：`let y = self`（值复制，y 独立于原值）、`return self`（返回值）、`foo(self)`（传值）、lambda 捕获 self（值复制进 cell，闭包生命周期独立于栈帧）
- [x] direct-call 不装箱（无 box 分配）
- [x] 全量回归

### 9.7 Spec 声明头满足【复用普通 type，witness 复用元组】

**状态**：已完成 ｜ **依赖**：9.4、9.6 ｜ **范围**：§4.1、§7.2

**变更**：
- [x] 复用普通 type 声明头满足 spec 路径
- [x] witness 生成：复用元组路径（`cg_ensure_value_box_witness_instance`，由原 `cg_ensure_tuple_box_witness_instance` 重命名 + guard 扩展）。@value 逃逸为 spec subject 时是 box（`_hdr + value`），需要与 tuple 相同的解包逻辑，**不走**普通 type 的 `cg_ensure_witness_instance_for_type`
- [x] 绑定解析扩展：新增 TYPE_OWN_METHOD 分支（@value type 自己的方法满足 spec 方法），tuple 保持 CE0335 拒绝
- [x] TYPE_OWN_METHOD thunk emit：@value thunk 传 `&box->value`（指针），调用 `um->c_name`
- [x] FIT_METHOD thunk self 传参修正：@value thunk 传 `&box->value`（指针），tuple 保持 `box->value`（值）
- [x] TYPE_OWN_FIELD getter 支持：@value type `let` 字段满足 spec `let` 字段

**测试**：
- [x] 声明头满足 spec（fcts/）：VItem: VDescribable（TYPE_OWN_METHOD）
- [x] witness 路径正确性：spec 参数传递（装箱 + thunk）、spec 变量赋值
- [x] fit 方法通过 spec 调用（FIT_METHOD）：VTag fit VDescribable
- [x] let 字段通过 spec 访问（TYPE_OWN_FIELD + TYPE_OWN_METHOD）：VWidget: VNamed
- [x] 值语义保持：spec box 持有独立副本，原值可独立修改
- [x] 全量回归

> PS. 本次 9.7 的 FIT_METHOD thunk 修正同时也覆盖了 9.8（fit 扩展）中提到的「@value box thunk 传参」要求（fm->c_name(&box->value, ...)）。9.8 的 fit 路径测试也已包含在 9.7 的测试中（VTag fit VDescribable）。后续执行 9.8 时，核心工作可能仅剩验证 fit 扩展的 self 访问/逃逸语义。

### 9.8 fit 扩展【复用普通 type，witness 复用元组】

**状态**：已完成 ｜ **依赖**：9.4、9.6 ｜ **范围**：§4.2

**变更**：
- [x] 复用普通 type fit 扩展路径（代码变更已在 §9.7 中完成：FIT_METHOD thunk 传参、self 绑定、witness 生成均已支持 @value）
- [x] witness 生成：同 §9.7（`cg_ensure_value_box_witness_instance`，复用元组路径，已在 §9.7 完成）
- [x] **@value box thunk 传参**：@value box thunk 传 `fm->c_name(&box->value, ...)`（传地址，因 @value 方法 self 为指针，§7.4.2 第 5 项）。tuple box thunk 传值保持不变（tuple self 指针化作为独立 TODO，见 §9.16）。已在 §9.7 完成

**测试**：
- [x] fit 扩展声明与方法实现（fcts/）：VFitBox fit VFitCopyable（trivial）、VFitNamed fit VFitNamedCopyable（aggregate）
- [x] @value fit 方法 self 访问与逃逸正确性（`var` 成员原地修改、`let y = self` 值复制、`return self` 返回值、`foo(self)` 传值、spec 参数传递、spec 变量赋值）
- [x] tuple fit 方法 self 回归（TFitTuple fit TCopyable：self 字段访问、self 返回、spec 参数、spec 变量赋值，保持现状，tuple self 指针化在 §9.16 独立交付）
- [x] 全量回归

### 9.9 值装箱（spec subject）【复用元组】

**状态**：已完成 ｜ **依赖**：9.4 ｜ **范围**：§4.4、§5.3、§7.2

**变更**（全部在 §9.4/§9.7 中完成）：
- [x] Box struct/desc/release 生成（由 `cg_emit_value_type_definition` 统一处理，包含在 §9.4 的主结构体 emit 中，§5.3）
- [x] `cg_emit_tuple_spec_box_subject` 重命名为 `cg_emit_spec_box_subject`，guard 扩展为 `is_tuple || is_value`。spec 强制转换分发点 guard 同步扩展，不新增分支（§7.2）
- [x] Box descriptor 的 `finalizer` 字段统一为 `NULL`（`@value type` 禁止终结器，§2.3、§5.3）
- [x] Spec 视角调用装箱（§4.4）
- [x] `UserType` 字段重命名 `c_tuple_box_*` → `c_value_box_*`（tuple 与 `@value` box 符号共用，命名需与实际用途一致，§7.2 函数重命名清单）

**测试**：
- [x] 装箱后 spec 视角调用（fcts/）——已在 §9.7/§9.8 测试中覆盖（spec 参数传递、spec 变量赋值）
- [x] box 释放时托管字段 release（无终结器路径）——新增 aggregate @value spec box 作用域退出/循环释放测试
- [x] 全量回归（352 tests passed）

### 9.10 泛型【复用普通 type + 复用元组】

**状态**：已完成 ｜ **依赖**：9.4 ｜ **范围**：§2.4

**变更**：
- [x] 复用普通 type 泛型实例化路径（含泛型方法/共享体）
- [x] 描述符生成（复用元组）：泛型 `@value type` 总是 aggregate（即使实例化后全字段 trivial），生成 `FengAggregateDescriptor`（§2.4）。理由：泛型类型参数在声明时大小未知、是否托管也未知，声明阶段无法计算准确的 `value_kind`，统一走 aggregate 路径（与 tuple 处理一致：全具体化实例 `is_generic_instance && generic_context_type_param_count == 0` 时一律生成 `FengAggregateDescriptor`）
- [x] 泛型共享体 `_type_desc` 参数类型：`@value type` 共享体接受 `const FengAggregateDescriptor *_type_desc`（非 `FengTypeDescriptor`），wrapper 传 `&c_aggregate_desc_name`。值类型仅有聚合描述符，不生成 `FengTypeDescriptor`，避免语义混乱
- [x] 泛型共享体方法路径 `@value` recv 处理：共享体 emit `shared_name((void *)recv, ...)`，`@value` recv 是值表达式（非指针），需 materialize 后传 `&local`（§7.4.2 第 4 项）

**测试**：
- [x] 泛型 `@value type` 实例化（fcts/）
- [x] 始终走 aggregate 路径（即使全字段 trivial）
- [x] 泛型方法
- [x] 全量回归

### 9.11 取地址 `&`（`@value @abi` 组合）【新增】

**状态**：已完成 ｜ **依赖**：9.4 ｜ **范围**：§3.7、§6.1、§7.2、§7.4

**变更**：
- [x] `c_abi_ptr_name` 函数体（`offsetof(first_field) == 0`，无需分支，§3.7）
- [x] `&` 站点按 `@value` 标志 emit 取地址 `&expr`（堆对象直接传 `expr`，§3.7）
- [x] Semantic 层 `&` 操作符校验扩展：接受 `@value @abi type`（当前仅接受标量/`@abi` 值/字符串/ABI 数组，需新增 `@value @abi` 分支）
- [x] `cg_emit_user_type_abi_surface` 入口新增 `@value` 早期分支，分发至 `cg_emit_value_type_abi_surface`（§7.4.2 第 2 项）
- [x] `@value @abi type` 终结器禁止：由 `@value` 本身禁止（§2.3），`@abi` 的 AE0317 自然满足，无需额外实现（§6.1）
- [x] `cg_emit_user_type_forward` 新增 `@value @abi` ABI layout 前向声明
- [x] `type_ref_is_abi_field_type` 允许 `@value @abi` 作为 `@abi` 字段（@value 无托管头）

> 注意: @value 和 @abi 应该是正交的, 互不感知, 各自独立负责自已的语义职责.

**测试**：
- [x] `&p` 取地址（fcts/）
- [x] `@value` 无 `@abi` 时 `&` 报错（语义层校验）
- [x] `@value @abi` 时 `&` 允许
- [x] `@value @abi type` 不可声明终结器
- [x] 全量回归

### 9.12 `@abi func` 形参/返回的 ABI 互操作【新增】

**状态**：未开始 ｜ **依赖**：9.11 ｜ **范围**：§6.1、§7.2

**变更**：
- [ ] `c_abi_value_name` 对 `@value @abi type` 平凡拷贝（值本身即 ABI 结构，§6.1）
- [ ] `c_abi_box_name` 对 `@value @abi type` 不生成（不堆分配）
- [ ] Codegen 在 `@abi func` 返回站点按 `@value` 标志跳过 box 调用，直接按值返回

**测试**：
- [ ] `@abi func` 形参为 `@value @abi type`（fcts/）
- [ ] `@abi func` 返回 `@value @abi type`
- [ ] 全量回归

### 9.13 数组元素【复用元组】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§6.4

**变更**：
- [ ] 数组元素 emit（`cg_aggregate_facts` 级联处理，与 tuple 一致）
- [ ] trivial `@value` 数组：元素按值存储，memcpy 复制（§6.4）
- [ ] aggregate `@value` 数组：逐元素调用聚合 API

**测试**：
- [ ] trivial `@value type` 数组（fcts/）
- [ ] aggregate `@value type` 数组
- [ ] 全量回归

### 9.14 异常【复用元组】

**状态**：未开始 ｜ **依赖**：9.9 ｜ **范围**：§6.2

**变更**：
- [ ] 异常 payload 装箱 emit（复用元组装箱路径，per-type 生成）
- [ ] `@value type` 作为异常抛出类型（装箱路径，§6.2）
- [ ] catch 端按值取出

**测试**：
- [ ] throw `@value type`（fcts/）
- [ ] catch `@value type`
- [ ] 全量回归

### 9.15 tuple 描述符命名统一【独立后续 TODO】

**状态**：未开始 ｜ **依赖**：§9.4 完成（@value 描述符命名落地后） ｜ **范围**：§5.1

**背景**：§5.1 为 @value type 引入按 `value_kind` 动态命名（trivial → `__trivial_desc`，aggregate → `__aggregate_desc`）。tuple 现有命名保持 `__aggregate_desc`（trivial/aggregate 统一），两者不一致。本 TODO 将 tuple 命名同步为 `value_kind` 动态命名，与 @value 统一。

**变更**：
- [ ] `cg_init_user_type_value_symbols` 对 tuple 也按 `value_kind` 计算描述符符号名（trivial → `__trivial_desc`，aggregate → `__aggregate_desc`）
- [ ] 实施时机同 §5.1「实施时机」：非泛型 tuple 在字段注册完成后（后置 pass）计算，泛型具体实例一律 `__aggregate_desc`
- [ ] 所有引用 `c_aggregate_desc_name` 的站点通过 `cg_aggregate_facts`/`facts.descriptor_name` 访问（已天然支持，无需逐站修改）
- [ ] 错误消息中 "tuple" 文本泛化（若未在 §9.4 完成）

**测试**：
- [ ] tuple trivial/aggregate 描述符符号名正确性（test/）
- [ ] 全量回归（tuple 描述符符号变更仅影响 C 产出符号名，行为应不变）

### 9.16 tuple self 指针化【独立后续 TODO】

**状态**：未开始 ｜ **依赖**：§9.6 完成（@value self 指针化落地后） ｜ **范围**：§7.4.2 第 5 项

**背景**：§7.4.2 第 5 项为 @value 引入 pointer self + `(*self)` 绑定（方法体内指针，逃逸复制）。tuple 现有方法 self 为 `struct X self`（值拷贝），与 @value 不一致。本 TODO 将 tuple 方法 self 同步改为 `struct X *self`（指针）+ `(*self)` 绑定，与 @value 统一，并避免大 tuple 整体拷贝的性能开销。

**变更**：
- [ ] `cg_emit_user_method_proto` tuple 分支：emit `struct X *self` 替代 `struct X self`（行 24822-24826）
- [ ] `cg_emit_user_method` tuple 分支：emit `struct X *self` 替代 `struct X self`（行 34538-34542）
- [ ] tuple 方法 self 绑定 c_name 改为 `(*self)`（`scope_add` + `cg_scope_bind_capture_cell` source_expr）
- [ ] tuple 方法调用站：recv 传 `&local` 替代 `local`（需 materialize）
- [ ] tuple box thunk 传参：`fm->c_name(&box->value, ...)` 替代 `fm->c_name(box->value, ...)`（@value 已在 §9.8 完成，tuple 同步）
- [ ] 普通 type self 保持 `"self"`（指针 + retain）不变

**测试**：
- [ ] tuple fit 方法 self 访问正确性（tuple 字段不可变，但语义应一致）
- [ ] tuple self 逃逸：`let y = self`（值复制）、`return self`（返回值）、`foo(self)`（传值）、lambda 捕获（值复制进 cell）
- [ ] tuple 方法调用站 materialize 正确性
- [ ] 全量回归（tuple 方法 ABI 变更，需重点验证 tuple fit 相关用例）

**注意**

- 每项任务完成并全量回归通过后，将相应任务标记为完成，输出 commit message，停下来等人工 Review 和下一步指令
- 如果遇到不确认的问题由人工决策

> **后续**：全量交付并通过回归后，将本草案迁入语言权威规范（`docs/`），按 CLAUDE.md「先文档」原则启动；迁入前本草案为唯一设计来源。
