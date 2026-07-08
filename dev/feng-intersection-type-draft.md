# 交叉类型（Intersection Type）设计草案

> **状态**：已确定，待实施
> **日期**：2026-07-07
> **关联**：[feng-value-model-delivered.md](./feng-value-model-delivered.md)、[feng-generics-delivered.md](./feng-generics-delivered.md)

---

## 1 背景

### 1.1 现有 Spec 体系

Feng 语言当前有四种 spec form：

| Form | 语法 | 满足方式 | 值表示 |
|---|---|---|---|
| OBJECT | `spec T { methods }` | 名义（需 `type X: T`） | `{ subject, witness }` |
| CALLABLE | `spec T(...): R` | 未绑定时结构性（签名匹配）；绑定后不可隐式匹配，可显式转换 | closure struct |
| UNION | `spec T: A \| B` | 结构性（成员匹配）；收窄前不可访问，必须收窄到具体类型（非联合类型）才能访问 | tagged union + `_fwd` |
| INTERSECTION | `spec T: A & B` | 结构性（成员匹配）；X 名义匹配所有成员 spec 即满足 | `{ subject, merged_witness }` |

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

问题：`MyType` 必须**显式声明** `GreetAndDisplay`，否则无法作为参数传入。类型定义方必须知道并引用组合 spec，增加了不必要的耦合。

#### 1.2.2 目标

引入交叉类型，使名义满足多个成员 spec 的类型**自动满足**交叉约束，无需显式声明交叉类型本身：

```feng
// 定义命名交叉类型
spec GreetAndDisplay: Greetable & Displayable;

// MyType 名义声明满足 Greetable 和 Displayable，
// 自动满足 GreetAndDisplay，无需声明 GreetAndDisplay
type MyType: Greetable, Displayable {
    func greet(): string { return "hi"; }
    func display(): string { return "display"; }
}

func process<T: GreetAndDisplay>(v: T): string {
    v.greet();
    v.display();
}

let m = MyType {};
process(m);  // OK: MyType 名义满足 GreetAndDisplay 的所有成员
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

- 成员 spec **必须全部是 object-form spec 或 Intersection-form spec**，如果成员是 IntersectionType 将进行展开
- 不支持交叉与 union 混合：`spec T: A & (B | C)` 不支持
- 交叉类型不能作为 union 成员（包括内联和命名交叉）：`spec U: (A & B) | string` 和 `spec U: BothAnd | string`（`BothAnd` 为命名交叉类型）均不支持
- **不允许** `type X: IntersectionType`（显式声明满足交叉类型），满足性从成员 spec 的名义满足自动推导

---

## 3 语义

### 3.1 方法集

交叉类型 `T: T1 & T2` 的方法集为所有成员 spec 方法集的并集：

```
T.methods = T1.methods ∪ T2.methods
```

包括成员 spec 各自的 parent specs 传递闭包中的方法。

**字段访问**：如果成员 spec 声明了字段约束（如 `let name: string`），这些字段会被处理为 getter/setter 方法纳入方法集。交叉类型的 merged witness 必须包含所有成员 spec 的字段访问器，确保通过交叉类型视角访问字段时正确工作。

### 3.2 方法冲突

| 场景 | 处理 |
|---|---|
| 同名、同参数类型、同返回类型 | 去重，保留一份 |
| 同名、同参数类型、**不同返回类型** | 编译错误 |
| 同名、**不同参数类型或参数数量** | 允许（重载） |

冲突检测复用现有 `detect_cross_spec_method_conflicts` 逻辑（analyzer.c:24369 附近，随变更行号可能偏移，实施时必须以实际代码为准），该函数已实现上述规则。

### 3.3 满足性检查

类型 X 满足交叉类型 `T: T1 & T2` 当且仅当 X **名义满足** T 的所有成员 spec。

检查流程：

1. 对每个成员 spec `Ti ∈ T.members`，调用现有 `subject_key_satisfies_spec_decl(X, Ti)` 做名义查表
2. 全部通过 → 满足；任一不通过 → 不满足

**简化点**：与纯结构匹配不同，名义匹配不需要新增检查路径，完全复用现有的名义满足检查。Merged witness 也直接从 X 已有的各成员 spec 的 witness 合并，无需遍历方法集重新查找实现。

### 3.4 与 `spec A: B, C` 的对比

| | `spec A: B, C { ... }` | `spec T: B & C;` |
|---|---|---|
| 方法集 | B ∪ C ∪ A 自有方法 | B ∪ C |
| 匹配方式 | 名义（需 `type X: A`） | 结构性（成员匹配）：X 名义满足 B 和 C 即满足 |
| 自有成员 | 可以有 | 不能有 |
| 显式声明 | 必须 `type X: A` | 不允许 `type X: T` |
| Witness | X 为 A 生成完整 witness | 从 X 的 B witness 和 C witness 合并 |

---

## 4 值表示

### 4.1 结构

与 object-form spec 一致：

```
{ subject: void*, witness: MergedWitness }
```

**性能保障**：通过交叉类型视角访问方法/字段的运行时开销与通过 object-form spec 视角访问**完全一致**（一次指针解引用 + 一次函数指针调用）。Merged witness 结构体虽然包含更多方法，但访问任何单个方法的开销不变（函数指针在固定偏移）。

### 4.2 Merged Witness 结构

为每个交叉类型生成独立的 witness 结构体，字段为所有成员 spec 方法的合并：

```c
// spec BothAnd: Greetable & Displayable;
struct FengSpecWitness__BothAnd {
    FengString *(*greet)(void *subject);      // 来自 Greetable
    FengString *(*display)(void *subject);    // 来自 Displayable
};
```

**按需生成**：仅当类型 X 的值在 coercion site（赋值点/转换点）被赋给交叉类型时，才生成 X 的 merged witness 实例（编译期常量）。由于 X 名义满足所有成员 spec，X 已有每个成员 spec 的独立 witness，merged witness 直接从这些现有 witness 合并：

```c
// 仅在 coercion site 处生成，从 MyType 已有的 Greetable 和 Displayable witness 合并
static const struct FengSpecWitness__BothAnd
FengSpecWitness__MyType__BothAnd = {
    .greet   = FengSpecWitness__MyType__Greetable.greet,
    .display = FengSpecWitness__MyType__Displayable.display,
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

1. **验证成员**：所有成员必须是 object-form spec 或 Intersection-form spec
2. **展平**：多层交叉在定义时展平并去重
3. **冲突检测**：复用 `detect_cross_spec_method_conflicts`
4. **满足性检查**：对每个使用交叉类型的位置，检查值类型是否名义满足所有成员（复用 `subject_key_satisfies_spec_decl`）
5. **Coercion 记录**：记录赋值点的 merged witness 信息供 codegen 使用（从已有的成员 witness 合并）

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
- 满足性检查复用现有名义满足路径，无需新增逻辑
- 每个交叉类型需要生成独立的 merged witness 结构体和实例

### 7.2 运行时

- 零额外运行时开销（merged witness 是编译期常量）
- 值表示与 object-form spec 一致（`{ subject, witness }`）
- ARC 行为与 object-form spec 一致

### 7.3 复杂度

- Merged witness 生成需要跨 spec witness 合并逻辑
- 方法冲突检测复用现有 `detect_cross_spec_method_conflicts`

---

## 8 已确定事项

1. **`type X: IntersectionType` 不允许**：现有 `AE0615` 验证（`analyzer.c:24754` 附近，随变更行号可能偏移，实施时必须以实际代码为准）只允许 object-form spec，交叉类型（`FENG_SPEC_FORM_INTERSECTION`）自然被拒绝。**原因**：`type X:` 后都是名义匹配的 object-form spec，如果允许交叉类型（结构性匹配），会导致语义混乱

2. **多层交叉**：`spec T: A & B; spec U: T & C;` 等价于 `A & B & C`，在定义时展平并去重（编译期），以确定 witness 结构

3. **交叉类型作为泛型约束时的共享体代码生成**：与 object-form spec 无差别，无需改动（共享体通过 witness 调用方法，merged witness 与单个 spec 的 witness 结构一致）

4. **交叉类型的 match/narrowing**：不支持

5. **字段（let/var）的处理**：满足性检查改为名义匹配后，字段的 getter/setter 已在各成员 spec 的 witness 中，merged witness 直接合并即可。字段冲突检测（同名不同类型）仍在交叉类型定义时通过 `detect_cross_spec_method_conflicts` 检查，与 object-form 一致

6. **性能保障**：以 Intersection-form 视角访问对象的开销不能大于 object-form 视角的开销（值表示和访问路径完全一致，均为 `{ subject, witness }` + 函数指针调用）

---

## 9 实施步骤

每步交付：代码实现 + 新增测试用例 + 全量回归测试通过。

- [x] **9.1 Parser：解析交叉类型语法**
  - 新增 `FENG_SPEC_FORM_INTERSECTION` 枚举值
  - AST 新增 `intersection_form` 节点（`FengTypeRef **members`, `size_t member_count`）
  - 解析逻辑：`:` 后第一个 type ref 后，`&` → INTERSECTION，`|` → UNION，`,`/`{` → OBJECT
  - 测试：`test/parser/` 新增 AST 结构测试，验证正确解析 `spec T: A & B;`

- [x] **9.2 Symbol Table：序列化/反序列化**
  - `ft_write` 支持写入 `FENG_SPEC_FORM_INTERSECTION` 及 `intersection_form` 成员列表
  - `ft_read` 支持读取并还原交叉类型 AST
  - 测试：`test/symbol/` 新增 round-trip 测试，写入后读取验证 AST 一致

- [x] **9.3 语义验证：成员类型检查**
  - 验证所有成员必须是 object-form spec 或 Intersection-form spec，否则报错
  - 多层交叉在定义时展平并去重（`spec U: T & C` 展平为 `[A, B, C]`）
  - Union 成员验证中显式拒绝 Intersection-form spec（命名交叉类型也不允许作为 union 成员）
  - 测试：`test/semantic/` 新增诊断测试，验证非法成员（union/callable）触发错误；union 使用交叉类型成员触发错误

- [x] **9.4 语义分析：方法集合并与冲突检测**
  - 收集所有成员 spec 的方法集（含 parent spec 传递闭包），去重
  - 复用 `detect_cross_spec_method_conflicts` 检测方法冲突
  - 同名同参数不同返回类型 → 编译错误；不同参数 → 允许重载
  - 字段（let/var）处理为 getter/setter 纳入方法集
  - 测试：`test/semantic/` 新增诊断测试，覆盖去重、冲突报错、重载允许三种场景

- [x] **9.5 语义分析：满足性检查**
  - 对每个使用交叉类型的位置，检查值类型是否名义满足所有成员 spec
  - 复用现有 `subject_key_satisfies_spec_decl` 对每个成员做名义查表
  - 不满足时报编译错误
  - 测试：`test/semantic/` 新增诊断测试，覆盖满足/不满足两种路径

- [x] **9.6 Codegen：Merged Witness 结构体与实例**
  - 为每个交叉类型生成 merged witness struct（`FengSpecWitness__<Name>`）
  - 为每个满足交叉类型的具体类型，按需生成 merged witness 实例（编译期常量）
  - 仅在 coercion site（赋值点）触发实例生成
  - 测试：`test/codegen/` 新增 IR 测试，验证生成正确的 witness struct 和实例

- [ ] **9.7 Codegen：Coercion 代码生成**
  - 在赋值点/转换点生成 `{ subject, merged_witness }` 构造
  - 交叉类型作为独立变量/参数类型：`let x: Both = value;`、`func foo(v: Both)`
  - 测试：`fcts/` 新增行为测试，覆盖变量声明、函数参数、函数返回值的 coercion

- [ ] **9.8 泛型约束：交叉类型作为约束**
  - 支持 `func process<T: GreetAndDisplay>(v: T)` 语法
  - 共享体代码生成复用 object-form 路径，无需改动
  - 测试：`fcts/` 新增行为测试，覆盖交叉类型约束下的泛型函数调用和方法调用

- [ ] **9.9 泛型交叉类型**
  - 支持 `spec Comparable<T>: Eq<T> & Ord<T>;` 语法
  - 成员 spec 带泛型参数，交叉类型本身有泛型参数
  - 测试：`fcts/` 新增行为测试，覆盖泛型交叉类型的定义、满足检查和泛型约束使用
