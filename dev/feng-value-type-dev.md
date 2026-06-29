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

`@value type` 的**运行时处理**与 tuple **一致**：作为其他 type 成员时的内联布局、生命周期处理（retain/release/assign/take/default_init）、相等性处理（`equal_fn`）、box 结构——全部复用 tuple 路径，走聚合值模型（`FengAggregateDescriptor` + 五类聚合 API）。

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

1. **运行时处理与 tuple 一致**：作为其他 type 成员时的内联布局、生命周期处理、相等性处理、box 结构——全部复用 tuple 路径。
2. **语法与用法与普通 type 一致**：构造器、泛型、方法、spec 满足、fit 扩展、witness 路径等全部复用普通 type 路径；两处差别：禁止终结器（见 §2.3）、取地址操作（见 §3.7）。
3. **runtime 零修改**：符合 OCP——新增 aggregate 类型仅需新增描述符，runtime walker/API 不动。
4. **codegen 最小改动**：泛化 tuple 的 per-type box 生成路径与普通 type 的 witness 路径，入口条件从「仅 tuple」/「仅普通 type」扩展为含 `@value`。
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
| 全字段 trivial（标量、bool） | `FENG_VALUE_TRIVIAL` | `FengTrivialDescriptor`（与标量一致，连 trivial 值也有描述符） |
| 至少一个托管字段（string、对象引用、spec） | `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | `FengAggregateDescriptor` |
| 泛型实例（无论字段类型） | `FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS` | `FengAggregateDescriptor` |

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

**命名约定**：@value type 描述符符号按 C 类型直名——trivial 用 `<struct>__trivial_desc`，aggregate 用 `<struct>__aggregate_desc`，比 tuple 现有命名（trivial/aggregate 统一用 `__aggregate_desc`，仅 C 类型不同）更清晰。tuple 是否同步重命名不影响正确性，由后续决定。

**关键点**：`equal_fn` **非 NULL**，指向 codegen 生成的逐字段比较函数（复用 tuple 的 `cg_emit_tuple_equal_function` 路径）。这与 §3.4「不用 `memcmp`」一致——`NULL` 会 fallback 到 `memcmp`，对含浮点字段的类型会给出错误的等值语义（NaN、符号零）。

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

`equal_fn` 同样**非 NULL**，复用 tuple 的 `cg_emit_tuple_equal_function` 生成路径。`FengAggregateDescriptor.equal_fn` 为 NULL 时表示「不支持 aggregate 等值」，`@value type` 必须生成非 NULL 值以满足 §3.4 的默认值比较语义。

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

@value type 的 box 复用 tuple 已有的 per-type codegen 生成模式（`header + embedded value struct`），两者 C 结构完全相同，codegen 仅需将入口条件从「仅 tuple」扩展为「tuple 或 @value」。

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
- **返回值**：`c_abi_box_name` 将 ABI 结构体装箱为 Feng 堆对象（`feng_object_new(&<c_desc_name>)`）。对 `@value @abi type` **不适用**——`@value type` 无 `c_desc_name`（值类型不生成 `FengTypeDescriptor`，仅有 trivial/aggregate 值描述符与 box 描述符），且 `@value` 值不堆分配。codegen 需在 `@abi func` 返回站点按 `@value` 标志跳过 `c_abi_box_name` 调用，直接按值返回 ABI 结构体给调用方绑定；`c_abi_box_name` 符号本身对 `@value @abi type` 不生成。

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

**最小改动**，核心是泛化 tuple box 路径：

| 变更项 | 工作量 | 说明 |
|--------|--------|------|
| `UserType` 字段重命名 | 小 | `c_tuple_box_*` → `c_value_box_*`（或保留现名，仅扩入口） |
| box 符号初始化函数入口扩展 | 小 | 条件从 tuple 扩展为 tuple \|\| value |
| `cg_aggregate_facts` 扩展 | 小 | `CG_TYPE_OBJECT` 分支条件扩展（见下方示意） |
| 主结构体 emit | 中 | 复用普通 type 的方法/构造器 emit 路径，两处区别：① 无 `_hdr`（同 tuple，普通 type 有 `FengManagedHeader _hdr`）；② `self` 指向栈上值地址（普通 type 的 `self` 指向堆对象）；描述符生成走值语义描述符（trivial/aggregate）。现有 `cg_emit_user_type_definition` 在 tuple 与普通 type 之间二选一分发，需新增 `@value` 分支。`@value type` 禁止终结器（见 §2.3），不 emit `c_finalizer_name` |
| box struct/desc/release emit | 小 | box struct（`_hdr + value`）复用 tuple 生成逻辑；box descriptor 的 `finalizer` 字段统一为 `NULL`（`@value type` 禁止终结器，见 §2.3、§5.3）。`release_children` 复用 tuple 路径（`feng_aggregate_release`） |
| `cg_emit_value_spec_box_subject` 新增 | 小 | 新增独立函数，复用 tuple 生成逻辑，不动 `cg_emit_tuple_spec_box_subject` |
| witness 生成路径 | 小 | 按原则 1，复用普通 type 的 `cg_ensure_witness_instance_for_type` 路径；仅需让该路径接受 `@value` subject（声明头满足 + fit 扩展均走此路径，**不走** tuple 的 `tuple_box_witness_tables`） |
| trivial/aggregate descriptor emit | 小 | 复用 tuple 的 `cg_emit_tuple_equal_function` 生成 `equal_fn`（非 NULL） |
| `&` 取地址（`@value @abi` 组合） | 小 | `c_abi_ptr_name` 函数体无需分支（`offsetof` 自然为 0）；`&` 站点按 `@value` 标志 emit 取地址 `&expr`（堆对象直接传 `expr`）；语义层补 `@value @abi` 注解组合校验 |
| `@abi func` 形参/返回的 ABI 互操作 | 小 | `@value @abi type` 作 `@abi func` 形参/返回值时按 ABI 结构体值语义直接传递，不经 `feng_object_new`。`c_abi_value_name` 对 `@value @abi` 是平凡拷贝（值本身即 ABI 结构）；`c_abi_box_name` 不适用（`@value` 不堆分配），codegen 需在返回站点按 `@value` 标志跳过 box 调用，直接按值返回（见 §6.1） |
| 终结器禁止诊断 | 小 | `@value type` 定义终结器时编译期报错（见 §2.3、§9.3），不 emit `c_finalizer_name`，不 push `FENG_NODE_DEFER`，无需 `cg_release_scope` combined 路径 |

入口条件扩展示意：

```c
// cg_aggregate_facts: 现有
if (!cg_type_is_tuple_user(t)) { return false; }

// cg_aggregate_facts: 泛化
if (!cg_type_is_tuple_user(t) && !cg_type_is_value_user(t)) { return false; }
```

生成逻辑本身零修改——box struct 布局、release_children 生成、equal_fn 生成对 tuple 和 @value type 完全一致；box descriptor 的 `finalizer` 字段：tuple 与 `@value type` 均一律 `NULL`（`@value type` 禁止终结器，见 §2.3、§5.3）。

### 7.3 Semantic

| 变更项 | 工作量 | 说明 |
|--------|--------|------|
| `@value` 注解解析 | 小 | 与 `@abi` 类似的注解识别 |
| 值分类判定 | 小 | codegen 层 `cg_aggregate_facts` 扩展入口即可（trivial/aggregate 细分复用 tuple 逻辑）；semantic 层 `feng_semantic_value_kind_of_decl` 对 `FENG_DECL_TYPE` 一律返回 `MANAGED_POINTER`（视 type 引用为托管指针），与 tuple 处理一致，**无需修改函数本身；需审计调用点确认无『MANAGED_POINTER ⇒ 堆对象+header』假设（tuple 已沿此路径，风险低）** |
| 值类型循环引用检测 | 中 | **新增值类型循环引用编译期检测**，覆盖 tuple + `@value type`（一并修正 tuple 现有静默失败 bug）；直接自引用 + 间接循环均报错；普通 `type`（堆对象，引用语义，大小固定）不受约束。详见 §3.5 |
| 等值运算符 | 小 | `@value type` 的 `==`/`!=` 走 codegen 生成的 `equal_fn`（复用 tuple 路径），semantic 层仅识别 `@value` 类型允许默认值比较 |
| spec 满足性检查 | 小 | 声明头满足与普通 type 一致（复用普通 type 路径）；`fit` 路径一致 |
| 构造器语义 | 小 | 与普通 type 一致，`self` 指向栈上值地址（普通 type 的 `self` 指向堆对象） |
| 终结器禁止诊断 | 小 | `@value type` 定义终结器时编译期报错（见 §2.3） |

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

> 本节将 `@value` 内建注解的开发任务拆解为 14 个可分步交付的 TODO，每步可独立通过全量回归测试并独立交付。
> 每步遵循 CLAUDE.md 基础原则：「先文档 → 后代码 → 后测试 → 全量回归」。
> 步骤间存在依赖关系（见每步「依赖」字段），按编号顺序交付；每步本身可独立交付并通过全量回归。
> 完成状态：**状态**字段标记「未开始 / 进行中 / 已完成」，子项用 `- [ ]` / `- [x]` 标记。
> Runtime 在全流程中零修改（§7.1）。

### 复用与参考分类

每个步骤标题标注复用/参考对象，区分四类：

| 分类 | 含义 | 工作量 |
|------|------|--------|
| **复用元组** | runtime 路径/API 完全复用，codegen 仅泛化入口条件 | 最小 |
| **复用普通 type** | semantic + codegen 处理路径复用普通 type，仅在描述符生成与发码（`_hdr`/`self`）两处做区别 | 小 |
| **参考元组** | per-type codegen 生成，借鉴元组的生成模式，新增独立函数 | 小~中 |
| **新增** | 无参考对象，从零实现 | 中 |

**复用普通 type 的区别收敛到两处**：
- 描述符生成：@value type 生成值描述符（trivial/aggregate）而非 `FengTypeDescriptor`
- 发码：主结构体无 `_hdr`；`self` 指向栈上值地址而非堆对象

### 9.1 `@value` 注解解析与 AST 扩展【新增】

**状态**：未开始 ｜ **依赖**：无 ｜ **范围**：§7.3

**变更**：
- [ ] Semantic 层识别 `@value` 注解（与 `@abi` 类似的注解识别）
- [ ] AST 增加 `@value` 标志位
- [ ] 注解语义校验：仅可用于 `type` 声明，用于 `spec`/`fit`/函数/变量时报错
- [ ] `@value type` 暂按普通 `type` 处理，不改变现有行为

**测试**：
- [ ] 注解错误使用的诊断码（test/）
- [ ] 正常使用不影响现有 fcts/ 行为（全量回归）

### 9.2 值类型循环引用编译期检测【新增】

**状态**：未开始 ｜ **依赖**：9.1 ｜ **范围**：§3.5、§7.3

**变更**：
- [ ] Semantic 层新增值类型循环引用检测（直接自引用 + 间接循环均报错）
- [ ] 检测覆盖 tuple 与 `@value type`（两者同属值类型，检测逻辑一致）
- [ ] 普通 `type`（堆对象，引用语义，大小固定）不受约束
- [ ] 一并修正 tuple 现有静默失败（无 C 产出、exit 0、无诊断）

**测试**：
- [ ] tuple 自引用/间接循环：报编译错误，不再静默吞过（fcts/）
- [ ] `@value type` 自引用/间接循环：报编译错误
- [ ] 普通 `type` 自引用：允许
- [ ] 全量回归

### 9.3 终结器禁止诊断【新增】

**状态**：未开始 ｜ **依赖**：9.1 ｜ **范围**：§2.3、§7.3

**变更**：
- [ ] Semantic 层检测 `@value type` 定义终结器，编译期报错（§2.3）
- [ ] 诊断码定义（test/）

**测试**：
- [ ] `@value type` 定义终结器：报编译错误（test/）
- [ ] 普通 `type` 定义终结器：允许（回归）
- [ ] 全量回归

### 9.4 值语义（trivial + aggregate）【复用元组 runtime + 参考元组 codegen】

**状态**：未开始 ｜ **依赖**：9.1、9.2 ｜ **范围**：§3.1、§3.2、§3.3、§3.4、§3.6、§5.1、§5.2、§7.2

**变更**：
- [ ] Semantic 层值分类判定（复用元组三分类：trivial / aggregate，§3.1）
- [ ] 审计 `feng_semantic_value_kind_of_decl` 调用点（确认无『MANAGED_POINTER ⇒ 堆对象+header』假设，§7.3）
- [ ] Codegen 层 `cg_aggregate_facts` 扩展入口（`tuple || value`，复用元组 runtime 路径，§7.2）
- [ ] `cg_emit_user_type_definition` 新增 `@value` 分支：无 `_hdr`（同 tuple）+ 值语义描述符（§7.2）
- [ ] Trivial/Aggregate descriptor 实例生成（参考元组生成逻辑，per-type，§5.1、§5.2）
- [ ] `equal_fn` 函数生成（参考元组 `cg_emit_tuple_equal_function`，per-type，§5.1、§5.2）
- [ ] 基本值语义：赋值、传参、内联布局（复用元组 runtime 路径，§3.2、§3.6）
- [ ] 等值比较（`==`/`!=` 走 `equal_fn`，复用元组 runtime 路径，§3.4）
- [ ] 成员可变性（`let`/`var`，§3.3）

**测试**：
- [ ] trivial `@value type` 声明/赋值/传参/字段内联（fcts/）
- [ ] aggregate `@value type` 声明/赋值/传参（含 `string`/对象引用字段）
- [ ] `==`/`!=` 值比较（trivial + aggregate）
- [ ] `let`/`var` 字段可变性
- [ ] 字段 retain/release 正确性（aggregate）
- [ ] 全量回归

### 9.5 构造器【复用普通 type，发码区别】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§2.2

**变更**：
- [ ] 复用普通 type 构造器 emit 路径
- [ ] 发码区别：`self` 指向栈上值地址（普通 type 的 `self` 指向堆对象）
- [ ] 字面量贴合（`Counter {}`）+ 参数构造（`Counter(10)`）

**测试**：
- [ ] 构造器调用（fcts/）
- [ ] `self` 语义（修改字段生效）
- [ ] 多构造器重载
- [ ] 全量回归

### 9.6 方法（含 direct-call）【复用普通 type，发码区别】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§2.5、§4.3

**变更**：
- [ ] 复用普通 type 方法 emit + direct-call 路径（静态分派，不装箱）
- [ ] 发码区别：`self` 指向栈上/内联值地址（普通 type 的 `self` 指向堆对象）

**测试**：
- [ ] 方法调用（fcts/）
- [ ] `self` 语义
- [ ] direct-call 不装箱（无 box 分配）
- [ ] 全量回归

### 9.7 Spec 声明头满足【复用普通 type，subject 区别】

**状态**：未开始 ｜ **依赖**：9.4、9.6 ｜ **范围**：§4.1、§7.2

**变更**：
- [ ] 复用普通 type 声明头满足 spec 路径
- [ ] 复用普通 type witness 生成路径（`cg_ensure_witness_instance_for_type`，不走 `tuple_box_witness_tables`，§7.2）
- [ ] subject 区别：`@value type` 的 subject 是值/box（普通 type 的 subject 是堆对象）

**测试**：
- [ ] 声明头满足 spec（fcts/）
- [ ] witness 路径正确性
- [ ] 全量回归

### 9.8 fit 扩展【复用普通 type，subject 区别】

**状态**：未开始 ｜ **依赖**：9.4、9.6 ｜ **范围**：§4.2

**变更**：
- [ ] 复用普通 type fit 扩展路径
- [ ] subject 区别：`@value type` 的 subject 是值/box

**测试**：
- [ ] fit 扩展声明与方法实现（fcts/）
- [ ] 全量回归

### 9.9 值装箱（spec subject）【参考元组】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§4.4、§5.3、§7.2

**变更**：
- [ ] Box struct/desc/release 生成（参考元组 box 生成逻辑，per-type，§5.3）
- [ ] `cg_emit_value_spec_box_subject` 新增（参考元组 `cg_emit_tuple_spec_box_subject`，独立函数，不动元组路径，§7.2）
- [ ] Box descriptor 的 `finalizer` 字段统一为 `NULL`（`@value type` 禁止终结器，§2.3、§5.3）
- [ ] Spec 视角调用装箱（§4.4）
- [ ] Non-escape 优化（栈上地址作为 subject，不分配 box，§5.3）
- [ ] `UserType` 字段重命名 `c_tuple_box_*` → `c_value_box_*`（可选，或保留现名仅扩入口，§7.2）

**测试**：
- [ ] 装箱后 spec 视角调用（fcts/）
- [ ] box 释放时托管字段 release（无终结器路径）
- [ ] non-escape 优化（临时 coercion 不分配 box）
- [ ] 全量回归

### 9.10 泛型【复用普通 type，描述符区别】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§2.4

**变更**：
- [ ] 复用普通 type 泛型实例化路径（含泛型方法/共享体）
- [ ] 描述符区别：泛型 `@value type` 总是 aggregate（即使实例化后全字段 trivial），生成 `FengAggregateDescriptor`（§2.4）

**测试**：
- [ ] 泛型 `@value type` 实例化（fcts/）
- [ ] 始终走 aggregate 路径（即使全字段 trivial）
- [ ] 泛型方法
- [ ] 全量回归

### 9.11 取地址 `&`（`@value @abi` 组合）【新增】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§3.7、§6.1、§7.2

**变更**：
- [ ] `c_abi_ptr_name` 函数体（`offsetof(first_field) == 0`，无需分支，§3.7）
- [ ] `&` 站点按 `@value` 标志 emit 取地址 `&expr`（堆对象直接传 `expr`，§3.7）
- [ ] 语义层 `@value @abi` 注解组合校验
- [ ] `@value @abi type` 终结器禁止：由 `@value` 本身禁止（§2.3），`@abi` 的 AE0317 自然满足，无需额外实现（§6.1）

**测试**：
- [ ] `&p` 取地址（fcts/）
- [ ] 注解组合校验（`@value @abi` 允许，`@value` 无 `@abi` 时 `&` 报错）
- [ ] `@value @abi type` 不可声明终结器
- [ ] 全量回归

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

### 9.13 数组元素【参考元组】

**状态**：未开始 ｜ **依赖**：9.4 ｜ **范围**：§6.4

**变更**：
- [ ] 数组元素 emit（参考元组生成逻辑）
- [ ] trivial `@value` 数组：元素按值存储，memcpy 复制（§6.4）
- [ ] aggregate `@value` 数组：逐元素调用聚合 API

**测试**：
- [ ] trivial `@value type` 数组（fcts/）
- [ ] aggregate `@value type` 数组
- [ ] 全量回归

### 9.14 异常【参考元组】

**状态**：未开始 ｜ **依赖**：9.9 ｜ **范围**：§6.2

**变更**：
- [ ] 异常 payload 装箱 emit（参考元组，per-type 生成）
- [ ] `@value type` 作为异常抛出类型（装箱路径，§6.2）
- [ ] catch 端按值取出

**测试**：
- [ ] throw `@value type`（fcts/）
- [ ] catch `@value type`
- [ ] 全量回归

> **后续**：全量交付并通过回归后，将本草案迁入语言权威规范（`docs/`），按 CLAUDE.md「先文档」原则启动；迁入前本草案为唯一设计来源。
