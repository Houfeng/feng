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

`@value type` 是值类型：无托管头，栈/内联分配，赋值与传参复制值。语法与用法与普通 `type` 基本一致（支持构造器、终结器、泛型、方法、spec 满足），差别仅在存储模型与传递语义。

### 非目标

- 不修改 `FengManagedHeader`、`FengTypeTag`、单指针原语或五类聚合 API。
- 不修改 `FengScalarBox` 结构或其 API。
- 不为 `@value type` 引入新的 runtime 类型或 runtime 函数。
- 不改变 tuple 的现有语义或实现。

---

## 1 核心设计决策

### 1.1 与 tuple 的关系

`@value type` 的生命周期处理与 tuple **一致**：编译器与运行时的处理逻辑基本相同，都走聚合值模型（`FengAggregateDescriptor` + 五类聚合 API）。

两者的差别本质上是**成员访问方式**：

| 维度 | tuple | `@value type` |
|------|-------|---------------|
| 语法形式 | `type Name(T1, T2)` 圆括号 | `type Name { ... }` 花括号 |
| 成员访问 | 位置（`item1`、`item2`） | 命名（`x`、`y`） |
| 元素数量 | 0 或 2~8 | 无限制 |
| 成员可变性 | 元素始终不可变 | 支持 `let`/`var` |
| 构造器 | 无（字面量贴合） | 支持 |
| 终结器 | 无 | 支持 |
| 声明头满足 spec | 不支持 | 支持 |
| `fit` 扩展 | 支持 | 支持 |
| 泛型 | 支持 | 支持 |

### 1.2 与普通 type 的关系

`@value type` 的用法与普通 `type` 基本一致：支持构造器、终结器、泛型、方法、静态成员、可见性修饰。差别仅在底层存储与传递模型：

| 维度 | `type`（普通对象） | `@value type`（值类型） |
|------|---|---|
| 存储 | 堆分配，`FengManagedHeader` | 栈/内联，无 header |
| 赋值 | 引用复制 | 值复制 |
| 等值比较 | 默认引用身份 | 默认值比较 |
| 值分类 | `managed-pointer` | `trivial` 或 `aggregate` |
| spec 装箱 | 不需要（自身即托管对象） | 需要（逃逸时装箱） |
| 自引用 | 允许 | 不允许（编译错误） |

### 1.3 设计原则

1. **生命周期与 tuple 一致**：编译器/运行时处理路径复用 tuple 已有基础设施。
2. **语法与用法与 type 一致**：用户心智模型连续，不因 `@value` 引入新的使用限制（除自引用禁止外）。
3. **runtime 零修改**：符合 OCP——新增 aggregate 类型仅需新增描述符，runtime walker/API 不动。
4. **codegen 最小改动**：泛化 tuple 的 per-type box 生成路径，入口条件从「仅 tuple」扩展为「tuple 或 @value」。

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

### 2.3 终结器

```feng
@value
type Buffer {
  var data: byte*;
  var size: int;

  func Buffer(size: int) {
    self.data = feng_alloc(size);
    self.size = size;
  }

  func ~Buffer() {
    feng_free(self.data);
  }
}
```

`@value type` 支持终结器。终结器在值生命周期结束时调用（作用域退出、aggregate release 路径等）。终结器的调用时机与 tuple 不同——tuple 没有终结器；`@value type` 的终结器由 codegen 在适当的清理站点 emit 调用。

终结器与普通 `type` 的终结器语义一致：负责释放值持有的非托管资源。托管字段的生命周期仍由聚合值模型管理（`feng_aggregate_release`），终结器只处理托管模型无法覆盖的资源（如 C 指针指向的内存）。

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

- trivial `@value`：`memcmp` 或逐字段 `==`
- aggregate `@value`：逐字段比较（trivial 字段走 `==`，托管字段走各自的 `==`）

这与 tuple、`string` 的值语义比较一致，与普通 `type` 的引用身份比较不同。

### 3.5 自引用禁止

`@value type` 不允许直接或间接自引用：

```feng
@value
type A {
  var a: A;     // ❌ 编译错误：值类型不可直接包含自身
}
```

理由：栈上值的类型大小必须编译期确定；自引用会导致无限大小。

### 3.6 作用域与生命周期

`@value type` 值在栈上布局，作用域退出时自动清理：

- trivial：无清理操作
- aggregate：`feng_aggregate_release`（逐槽位 release 托管字段）
- 有终结器时：先调终结器，再 aggregate release

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

全字段 trivial 的 `@value type` 生成 `FengTrivialDescriptor`：

```c
// @value type Point { var x: float; var y: float; }
static const FengTrivialDescriptor Feng__demo__Point__trivial_desc = {
    .name = "demo.Point",
    .size = sizeof(struct Feng__demo__Point),
    .equal_fn = NULL,   // NULL => memcmp
};
```

codegen 的 `cg_trivial_descriptor_expr` 已处理 `CG_TYPE_OBJECT` 且 `facts.value_kind == CG_VK_TRIVIAL` 的情况——返回 `facts.descriptor_name`。@value type 只需在 `cg_aggregate_facts` 中扩展判定即可。

### 5.2 Aggregate @value type

含托管字段的 `@value type` 生成 `FengAggregateDescriptor`（与 tuple 路径一致）：

```c
// @value type User { let id: int; let name: string; }
static const FengManagedSlotDescriptor Feng__demo__User__aggregate_slots[] = {
    { offsetof(struct Feng__demo__User, name), FENG_SLOT_POINTER, NULL },
};

static const FengAggregateDescriptor Feng__demo__User__aggregate_desc = {
    .name = "demo.User",
    .size = sizeof(struct Feng__demo__User),
    .managed_slot_count = 1,
    .managed_slots = Feng__demo__User__aggregate_slots,
    ...
};
```

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
    .release_children = Feng__demo__User__spec_box_release_children,
    .is_potentially_cyclic = true,   // 含托管字段时
    .managed_field_count = 1,
    .managed_fields = ...,
    ...
};
```

@value type 的 box 复用 tuple 已有的 per-type codegen 生成模式（`header + embedded value struct`），两者 C 结构完全相同，codegen 仅需将入口条件从「仅 tuple」扩展为「tuple 或 @value」。`FengScalarBox`（runtime 预编译的固定 union，服务 11 种标量）保持不变——标量 box 与 per-type box 在 payload 布局、构造函数、descriptor 策略上本质不同，强行合并无收益。

**Non-escape 优化**：当 `@value` 值仅在调用栈帧内消费时（临时 coercion），可借用栈上地址作为 subject，不分配 box。逃逸到局部绑定、返回值或字段存储时才分配 box。口径沿用现有定义（`feng-fit-builtin-type.md` §6.3）。

---

## 6 与现有机制的交互

### 6.1 `@value` 与 `@abi`

建议 `@value` 与 `@abi` **互斥**，不可同时标注。

理由：`@abi` 当前面向堆对象模型（`FengManagedHeader` + ABI payload 抽取/装箱），与 `@value` 的无头值语义模型冲突。如果未来有需求再开放兼容路径。

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

### 7.2 Codegen

**最小改动**，核心是泛化 tuple box 路径：

| 变更项 | 工作量 | 说明 |
|--------|--------|------|
| `UserType` 字段重命名 | 小 | `c_tuple_box_*` → `c_value_box_*` |
| box 符号初始化函数重命名与入口扩展 | 小 | 条件从 tuple 扩展为 tuple \|\| value |
| `cg_aggregate_facts` 扩展 | 小 | `CG_TYPE_OBJECT` 分支条件扩展 |
| box struct/desc/release emit 函数重命名 | 小 | 生成逻辑不变 |
| `cg_emit_value_spec_box_subject` 入口扩展 | 小 | 函数内部逻辑不变 |
| witness thunk 生成路径扩展 | 中 | 需要处理 @value type 的声明头满足（tuple 不支持） |
| trivial descriptor emit | 小 | 已有路径支撑 |
| 终结器 emit | 中 | 新增清理站点：作用域退出时先调终结器再 aggregate release |

入口条件扩展示意：

```c
// cg_aggregate_facts: 现有
if (!cg_type_is_tuple_user(t)) { return false; }

// cg_aggregate_facts: 泛化
if (!cg_type_is_tuple_user(t) && !cg_type_is_value_user(t)) { return false; }
```

生成逻辑本身零修改——box struct 布局、descriptor 生成、release_children 生成对 tuple 和 @value type 完全一致。

### 7.3 Semantic

| 变更项 | 工作量 | 说明 |
|--------|--------|------|
| `@value` 注解解析 | 小 | 与 `@abi` 类似的注解识别 |
| 值分类判定 | 小 | 复用 `feng_semantic_value_kind_of_decl`，扩展判定逻辑 |
| 自引用检测 | 小 | 类型大小检查时检测循环引用 |
| 等值运算符 | 中 | `@value type` 的 `==`/`!=` 重载为值比较 |
| spec 满足性检查 | 小 | 声明头满足与普通 type 一致；`fit` 路径一致 |
| 构造器/终结器语义 | 小 | 与普通 type 一致，`self` 语义略有不同 |

---

## 8 开放问题

| # | 问题 | 建议 | 状态 |
|---|------|------|------|
| 1 | `@value` 与 `@abi` 是否兼容 | 先互斥 | 待决策 |
| 2 | `let` 绑定下 `var` 字段是否可修改 | 建议可修改（与普通 type 一致） | 待决策 |
| 3 | 终结器的确切调用时机 | 作用域退出时，先终结器后 aggregate release | 待决策 |
| 4 | trivial @value 的等值比较实现 | memcmp 还是逐字段 `==` | 待决策 |

---

## 9 关联文档

- [feng-value-model-delivered.md](./feng-value-model-delivered.md)：值模型基础设施（Phase 3 的基座）
- [feng-type.md](../docs/feng-type.md)：`type` 类型规范
- [feng-tuple.md](../docs/feng-tuple.md)：元组规范
- [feng-spec.md](../docs/feng-spec.md)：`spec` 规范
- [feng-fit.md](../docs/feng-fit.md)：`fit` 扩展规范
- [feng-fit-builtin-type.md](../docs/feng-fit-builtin-type.md)：内建类型 fit 与装箱约束
