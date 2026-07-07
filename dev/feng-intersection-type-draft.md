# 交叉类型（Intersection Type）设计草案

> **状态**：草案（未最终确定，未实施）
> **日期**：2026-07-07
> **关联**：[feng-value-model-delivered.md](./feng-value-model-delivered.md)、[feng-generics-delivered.md](./feng-generics-delivered.md)

---

## 1 背景

### 1.1 现有 Spec 体系

Feng 语言当前有三种 spec form：

| Form | 语法 | 满足方式 | 值表示 |
|---|---|---|---|
| OBJECT | `spec T { methods }` | 名义（需 `type X: T`） | `{ subject, witness }` |
| CALLABLE | `spec T(...): R` | 未绑定时结构性（签名匹配）；绑定后不可隐式匹配，可显式转换 | closure struct |
| UNION | `spec T: A \| B` | 结构性（成员匹配）；收窄前不可访问，必须收窄到具体类型（非联合类型）才能访问 | tagged union + `_fwd` |

Object-form spec 支持 parent specs：

```feng
spec A: B, C {
    // A 的方法集 = B.methods ∪ C.methods ∪ A.methods
    // 类型 X 必须显式声明 "type X: A" 才能满足 A
}
```

### 1.2 动机

#### 1.2.1 泛型约束的单 spec 限制

当前泛型约束位置（`T:` 之后）只能写一个 spec：

```feng
func process<T: Greetable>(v: T): string {
    v.greet();  // 只能调用 Greetable 的方法
}
```

如果需要参数同时满足多个 spec，当前只能通过定义组合 spec 来解决：

```feng
spec GreetAndDisplay: Greetable, Displayable { }

func process<T: GreetAndDisplay>(v: T): string {
    v.greet();
    v.display();
}

// 类型必须显式声明满足 GreetAndDisplay
type MyType: GreetAndDisplay { ... }
```

问题：`MyType` 即使已分别实现了 `Greetable` 和 `Displayable`，也必须**显式声明** `GreetAndDisplay`，否则无法作为参数传入。这增加了不必要的耦合。

#### 1.2.2 目标

引入交叉类型，使同时满足多个 spec 的类型**自动满足**交叉约束，无需显式声明：

```feng
// 定义命名交叉类型
spec GreetAndDisplay: Greetable & Displayable;

// MyType 已声明满足 Greetable 和 Displayable，
// 自动满足 GreetAndDisplay，无需额外声明
type MyType: Greetable, Displayable {
    func greet(): string { return "hi"; }
    func display(): string { return "display"; }
}

func process<T: GreetAndDisplay>(v: T): string {
    v.greet();
    v.display();
}

let m = MyType {};
process(m);  // OK: MyType 结构性满足 GreetAndDisplay
```

---

## 2 语法

### 2.1 定义

```feng
spec Name: MemberSpec1 & MemberSpec2;
spec Name: MemberSpec1 & MemberSpec2 & MemberSpec3;
```

使用 `&` 分隔成员，以 `;` 结束。不能有 `{ ... }` 体（交叉类型无自有成员）。

**注意**：Feng 语言的设计原则是**所有复杂类型必须命名**，拒绝在类型位置内联写法（包括 tuple、lambda、union、intersection）。交叉类型必须先定义命名类型，再引用：

```feng
// 正确：命名交叉类型
spec Both: A & B;
func process<T: Both>(v: T): string { ... }

// 不支持：内联交叉（违反命名原则）
func process<T: A & B>(v: T): string { ... }  // 错误
```

### 2.2 泛型

```feng
spec Comparable<T>: Eq<T> & Ord<T>;
```

成员 spec 可以带泛型参数，交叉类型本身也可以有泛型参数。

### 2.3 约束

- 成员 spec **必须全部是 object-form spec**
- 不支持交叉与 union 混合：`spec T: A & (B | C)` 不支持
- 交叉类型不能作为 union 成员：`spec U: (A & B) | string` 不支持
- **暂定不允许** `type X: IntersectionType`（显式声明满足交叉类型），仅通过结构性检查自动满足

---

## 3 语义

### 3.1 方法集

交叉类型 `T: T1 & T2` 的方法集为所有成员 spec 方法集的并集：

```
T.methods = T1.methods ∪ T2.methods
```

包括成员 spec 各自的 parent specs 传递闭包中的方法。

### 3.2 方法冲突

| 场景 | 处理 |
|---|---|
| 同名、同参数类型、同返回类型 | 去重，保留一份 |
| 同名、同参数类型、**不同返回类型** | 编译错误 |
| 同名、**不同参数类型或参数数量** | 允许（重载） |

冲突检测复用现有 `detect_cross_spec_method_conflicts` 逻辑（analyzer.c:24369），该函数已实现上述规则。

### 3.3 结构性满足

类型 X 满足交叉类型 `T: T1 & T2` 当且仅当 X 实现了 T 的全部方法（含字段）。

检查流程：

1. 收集 T 的方法集（成员 spec 方法的并集，去重）
2. 对每个方法 `m ∈ T.methods`，检查 X 是否实现：
   - X 自身的 `type_decl.members` 中有同名同签方法
   - 或 X 声明的某个 spec 的方法闭包中包含 m
3. 全部覆盖 → 满足；否则 → 不满足

这与现有名义满足（`subject_key_satisfies_spec_decl` 查表）不同，需要新的结构性检查路径。

### 3.4 与 `spec A: B, C` 的对比

| | `spec A: B, C { ... }` | `spec T: B & C;` |
|---|---|---|
| 方法集 | B ∪ C ∪ A 自有方法 | B ∪ C |
| 匹配方式 | 名义（需 `type X: A`） | 结构性（有全部方法即满足） |
| 自有成员 | 可以有 | 不能有 |
| 显式声明 | 必须 `type X: A` | 不允许 `type X: T`（暂定） |
| Witness | X 为 A 生成完整 witness | X 生成 merged witness |

---

## 4 值表示

### 4.1 结构

与 object-form spec 一致：

```
{ subject: void*, witness: MergedWitness }
```

### 4.2 Merged Witness 结构

为每个交叉类型生成独立的 witness 结构体，字段为所有成员 spec 方法的合并：

```c
// spec BothAnd: Greetable & Displayable;
struct FengSpecWitness__BothAnd {
    FengString *(*greet)(void *subject);      // 来自 Greetable
    FengString *(*display)(void *subject);    // 来自 Displayable
};
```

**按需生成**：仅当类型 X 的值在 coercion site（赋值点/转换点）被赋给交叉类型时，才生成 X 的 merged witness 实例（编译期常量）：

```c
// 仅在 coercion site 处生成
static const struct FengSpecWitness__BothAnd
FengSpecWitness__MyType__BothAnd = {
    .greet   = MyType_greet,
    .display = MyType_display,
};
```

如果没有 coercion site 使用交叉类型，则不生成任何 witness 实例。这与 union coercion 的按需记录机制一致。

### 4.3 Coercion

将 X 类型的值赋给交叉类型变量时，构造 `{ subject, merged_witness }`：

```c
struct FengSpecValue__BothAnd _tmp = {
    .subject = &mytype_instance,
    .witness = &FengSpecWitness__MyType__BothAnd,
};
```

---

## 5 作为泛型约束

使用命名交叉类型作为约束：

```feng
spec GreetAndDisplay: Greetable & Displayable;

func process<T: GreetAndDisplay>(v: T): string {
    v.greet();     // 来自 Greetable
    v.display();   // 来自 Displayable
}
```

**共享体处理**：与 object-form spec 作为约束时**完全相同**，无需改动。共享体通过 `FengGenericParamDescriptor` 获取 witness，通过 witness 函数指针调用方法。交叉类型的 merged witness 与单个 spec 的 witness 在结构上无本质区别（都是方法指针表），因此共享体代码生成路径完全复用。

---

## 6 实现路径

### 6.1 Parser

新增 `FENG_SPEC_FORM_INTERSECTION`：

```c
typedef enum FengSpecForm {
    FENG_SPEC_FORM_OBJECT = 0,
    FENG_SPEC_FORM_CALLABLE,
    FENG_SPEC_FORM_UNION,
    FENG_SPEC_FORM_INTERSECTION,
} FengSpecForm;
```

AST 新增 `intersection_form`：

```c
struct {
    FengTypeRef **members;
    size_t member_count;
} intersection_form;
```

解析逻辑：在 `:` 后解析第一个 type ref 后，根据下一个 token 分支：

| Token | Form |
|---|---|
| `\|` | UNION |
| `&` | INTERSECTION |
| `,` 或 `{` | OBJECT（parent specs） |

### 6.2 Semantic Analysis

1. **验证成员**：所有成员必须是 object-form spec
2. **方法集合并**：收集所有成员 spec 的方法（含 parent spec 闭包），去重
3. **冲突检测**：复用 `detect_cross_spec_method_conflicts`
4. **结构性满足检查**：新增 `type_decl_structurally_satisfies_intersection` 函数
5. **Coercion 记录**：记录赋值点的 merged witness 信息供 codegen 使用

### 6.3 Codegen

1. **Witness 结构体**：为每个交叉类型生成 merged witness struct
2. **Witness 实例**：为每个满足交叉类型的具体类型生成 merged witness 常量
3. **Coercion**：在赋值点生成 `{ subject, merged_witness }` 构造
4. **泛型共享体**：通过 `FengGenericParamDescriptor` 传递 merged witness

### 6.4 Symbol Table (ft_read/ft_write)

交叉类型的序列化和反序列化需要处理新 form。

---

## 7 代价评估

### 7.1 编译期

- Parser 变更量小（仿照 union form 的 `|` 解析逻辑）
- 结构性满足检查是新增逻辑路径，需要遍历方法集
- 每个交叉类型需要生成独立的 merged witness 结构体和实例

### 7.2 运行时

- 零额外运行时开销（merged witness 是编译期常量）
- 值表示与 object-form spec 一致（`{ subject, witness }`）
- ARC 行为与 object-form spec 一致

### 7.3 复杂度

- 结构性满足检查与名义满足检查是两条独立路径，增加了语义分析复杂度
- Merged witness 生成需要跨 spec 方法合并逻辑

---

## 8 待定事项

1. **`type X: IntersectionType` 是否允许**：暂定不允许，需评估是否有漏洞或问题
2. **字段（let）的结构性检查**：当成员 spec 含字段声明时，如何检查类型是否满足
3. **交叉类型的 match/narrowing**：是否支持对交叉类型值做 match（如判断值来自哪个 spec 方法实现）
4. **多层交叉**：`spec T: A & B; spec U: T & C;` 是否支持？语义上等价于 `A & B & C`
5. **交叉类型作为泛型约束时的共享体代码生成**：merged witness 在共享体内如何访问
6. **性能基准**：merged witness 生成和结构性检查的编译期开销
